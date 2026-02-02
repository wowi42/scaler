#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>

#define DEFAULT_PORT 8080
#define BUFFER_SIZE 4096
#define MAX_ALLOCATIONS 4096

typedef struct {
    int health_up;
    int wait_seconds;
    size_t allocated_mb;
    time_t start_time;
} ServerState;

static ServerState state = {
    .health_up = 1,
    .wait_seconds = 0,
    .allocated_mb = 0,
    .start_time = 0
};

static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static void *allocations[MAX_ALLOCATIONS];
static int alloc_count = 0;

static void send_response(int client, int status, const char *status_text, const char *body) {
    char response[BUFFER_SIZE];
    int body_len = strlen(body);
    int len = snprintf(response, sizeof(response),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        status, status_text, body_len, body);
    write(client, response, len);
}

static void send_json_ok(int client, const char *body) {
    send_response(client, 200, "OK", body);
}

static void send_bad_request(int client, const char *msg) {
    char body[256];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    send_response(client, 400, "Bad Request", body);
}

static void send_not_found(int client) {
    send_response(client, 404, "Not Found", "{\"error\":\"not found\"}");
}

static void send_method_not_allowed(int client) {
    send_response(client, 405, "Method Not Allowed", "{\"error\":\"method not allowed\"}");
}

static int parse_positive_int(const char *s, int *out) {
    if (!s || !*s) return -1;
    char *end;
    errno = 0;
    long val = strtol(s, &end, 10);
    if (errno || *end != '\0' || val < 0 || val > INT_MAX) return -1;
    *out = (int)val;
    return 0;
}

static int starts_with(const char *str, const char *prefix) {
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

static void handle_get_root(int client) {
    char body[512];
    pthread_mutex_lock(&state_mutex);
    time_t uptime = time(NULL) - state.start_time;
    snprintf(body, sizeof(body),
        "{\"uptime\":%ld,\"allocated_memory_mb\":%zu,\"health_state\":\"%s\",\"wait_seconds\":%d}",
        (long)uptime, state.allocated_mb, state.health_up ? "up" : "down", state.wait_seconds);
    pthread_mutex_unlock(&state_mutex);
    send_json_ok(client, body);
}

static void handle_get_up(int client) {
    pthread_mutex_lock(&state_mutex);
    int wait_secs = state.wait_seconds;
    int healthy = state.health_up;
    pthread_mutex_unlock(&state_mutex);

    if (wait_secs > 0) {
        sleep(wait_secs);
    }
    if (healthy) {
        send_response(client, 200, "OK", "{\"status\":\"healthy\"}");
    } else {
        send_response(client, 500, "Internal Server Error", "{\"status\":\"unhealthy\"}");
    }
}

static void handle_post_up_down(int client) {
    pthread_mutex_lock(&state_mutex);
    state.health_up = 0;
    pthread_mutex_unlock(&state_mutex);
    send_json_ok(client, "{\"health_state\":\"down\",\"message\":\"health set to down\"}");
}

static void handle_post_up_up(int client) {
    pthread_mutex_lock(&state_mutex);
    state.health_up = 1;
    pthread_mutex_unlock(&state_mutex);
    send_json_ok(client, "{\"health_state\":\"up\",\"message\":\"health set to up\"}");
}

static void handle_post_up_wait(int client, const char *seconds_str) {
    int seconds;
    if (parse_positive_int(seconds_str, &seconds) < 0) {
        send_bad_request(client, "invalid seconds value");
        return;
    }
    pthread_mutex_lock(&state_mutex);
    state.wait_seconds = seconds;
    pthread_mutex_unlock(&state_mutex);
    char body[128];
    snprintf(body, sizeof(body), "{\"wait_seconds\":%d,\"message\":\"wait time configured\"}", seconds);
    send_json_ok(client, body);
}

static void handle_post_alloc(int client, const char *mb_str) {
    int mb;
    if (parse_positive_int(mb_str, &mb) < 0 || mb <= 0) {
        send_bad_request(client, "invalid mb value (must be positive integer)");
        return;
    }

    size_t bytes = (size_t)mb * 1024 * 1024;
    void *ptr = malloc(bytes);
    if (!ptr) {
        send_response(client, 500, "Internal Server Error", "{\"error\":\"allocation failed\"}");
        return;
    }

    // Touch every page to commit memory (assuming 4KB pages)
    volatile char *p = (volatile char *)ptr;
    for (size_t i = 0; i < bytes; i += 4096) {
        p[i] = 1;
    }

    pthread_mutex_lock(&state_mutex);
    // Store allocation pointer so it won't be freed
    if (alloc_count < MAX_ALLOCATIONS) {
        allocations[alloc_count++] = ptr;
    }
    state.allocated_mb += mb;
    size_t total = state.allocated_mb;
    pthread_mutex_unlock(&state_mutex);

    char body[128];
    snprintf(body, sizeof(body), "{\"allocated_mb\":%d,\"total_allocated_mb\":%zu}", mb, total);
    send_json_ok(client, body);
}

static void handle_request(int client, const char *method, const char *path) {
    // GET /
    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        handle_get_root(client);
        return;
    }

    // GET /up
    if (strcmp(method, "GET") == 0 && strcmp(path, "/up") == 0) {
        handle_get_up(client);
        return;
    }

    // POST /down
    if (strcmp(method, "POST") == 0 && strcmp(path, "/down") == 0) {
        handle_post_up_down(client);
        return;
    }

    // POST /up
    if (strcmp(method, "POST") == 0 && strcmp(path, "/up") == 0) {
        handle_post_up_up(client);
        return;
    }

    // POST /wait/{seconds}
    if (strcmp(method, "POST") == 0 && starts_with(path, "/wait/")) {
        const char *seconds_str = path + strlen("/wait/");
        handle_post_up_wait(client, seconds_str);
        return;
    }

    // POST /alloc/{mb}
    if (strcmp(method, "POST") == 0 && starts_with(path, "/alloc/")) {
        const char *mb_str = path + strlen("/alloc/");
        handle_post_alloc(client, mb_str);
        return;
    }

    // Check if path exists but method is wrong
    if (strcmp(path, "/") == 0 || strcmp(path, "/up") == 0 ||
        strcmp(path, "/down") == 0 || starts_with(path, "/wait/") ||
        starts_with(path, "/alloc/")) {
        send_method_not_allowed(client);
        return;
    }

    send_not_found(client);
}

static void *handle_client(void *arg) {
    int client = *(int *)arg;
    free(arg);

    char buffer[BUFFER_SIZE];
    ssize_t n = read(client, buffer, sizeof(buffer) - 1);
    if (n <= 0) {
        close(client);
        return NULL;
    }
    buffer[n] = '\0';

    // Parse request line: METHOD PATH HTTP/1.x
    char method[16] = {0};
    char path[256] = {0};
    char *line_end = strstr(buffer, "\r\n");
    if (!line_end) {
        send_bad_request(client, "malformed request");
        close(client);
        return NULL;
    }

    if (sscanf(buffer, "%15s %255s", method, path) != 2) {
        send_bad_request(client, "malformed request line");
        close(client);
        return NULL;
    }

    // Remove query string if present
    char *query = strchr(path, '?');
    if (query) *query = '\0';

    handle_request(client, method, path);
    close(client);
    return NULL;
}

int main(void) {
    // Get port from environment or use default
    int port = DEFAULT_PORT;
    const char *port_env = getenv("PORT");
    if (port_env) {
        int p;
        if (parse_positive_int(port_env, &p) == 0 && p > 0 && p <= 65535) {
            port = p;
        } else {
            fprintf(stderr, "Invalid PORT value '%s', using default %d\n", port_env, DEFAULT_PORT);
        }
    }

    state.start_time = time(NULL);

    // Create socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    // Allow port reuse
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port)
    };

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(server_fd, 128) < 0) {
        perror("listen");
        return 1;
    }

    printf("Server listening on port %d\n", port);

    while (1) {
        int client = accept(server_fd, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        int *client_fd = malloc(sizeof(int));
        if (!client_fd) {
            close(client);
            continue;
        }
        *client_fd = client;

        pthread_t thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        if (pthread_create(&thread, &attr, handle_client, client_fd) != 0) {
            perror("pthread_create");
            close(client);
            free(client_fd);
        }

        pthread_attr_destroy(&attr);
    }

    return 0;
}
