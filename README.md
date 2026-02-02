# HTTP Server

A single-file C HTTP server for testing health checks and memory allocation.

## Build and Run

```bash
# Build
mise run build

# Run (builds if needed)
mise run run

# Clean
mise run clean
```

Or compile directly:

```bash
gcc -pthread -o server server.c
./server
```

The server listens on port 8080 by default. Set the `PORT` environment variable to change it:

```bash
PORT=3000 ./server
```

## API Endpoints

### GET / - Server Status

Returns server uptime, allocated memory, health state, and wait configuration.

```bash
curl http://localhost:8080/
```

Response:
```json
{"uptime":42,"allocated_memory_mb":0,"health_state":"up","wait_seconds":0}
```

### GET /up - Health Check

Returns 200 if healthy, 500 if unhealthy. Respects configured wait delay.

```bash
curl -w "\nHTTP Status: %{http_code}\n" http://localhost:8080/up
```

Response (healthy):
```json
{"status":"healthy"}
HTTP Status: 200
```

Response (unhealthy):
```json
{"status":"unhealthy"}
HTTP Status: 500
```

### POST /down - Set Health to Down

```bash
curl -X POST http://localhost:8080/down
```

Response:
```json
{"health_state":"down","message":"health set to down"}
```

### POST /up - Set Health to Up

```bash
curl -X POST http://localhost:8080/up
```

Response:
```json
{"health_state":"up","message":"health set to up"}
```

### POST /wait/{seconds} - Configure Health Check Delay

Sets a delay before responding to health checks.

```bash
curl -X POST http://localhost:8080/wait/5
```

Response:
```json
{"wait_seconds":5,"message":"wait time configured"}
```

After this, `GET /up` will wait 5 seconds before responding.

Reset to no delay:
```bash
curl -X POST http://localhost:8080/wait/0
```

### POST /alloc/{mb} - Allocate Memory

Allocates the specified amount of memory (in MB) and touches all pages to commit them.

```bash
curl -X POST http://localhost:8080/alloc/100
```

Response:
```json
{"allocated_mb":100,"total_allocated_mb":100}
```

Allocate more:
```bash
curl -X POST http://localhost:8080/alloc/50
```

Response:
```json
{"allocated_mb":50,"total_allocated_mb":150}
```

## Example Session

```bash
# Start the server
mise run run &

# Check initial status
curl http://localhost:8080/

# Verify health
curl http://localhost:8080/up

# Set health to down
curl -X POST http://localhost:8080/down

# Verify health check now returns 500
curl -w "\nHTTP Status: %{http_code}\n" http://localhost:8080/up

# Set health back to up
curl -X POST http://localhost:8080/up

# Configure 3 second delay on health checks
curl -X POST http://localhost:8080/wait/3

# Health check now takes 3 seconds
time curl http://localhost:8080/up

# Allocate 100 MB of memory
curl -X POST http://localhost:8080/alloc/100

# Check status to see allocated memory
curl http://localhost:8080/

# Stop the server
pkill server
```

## Error Handling

Invalid numeric parameters return 400 Bad Request:

```bash
curl -X POST http://localhost:8080/wait/abc
# {"error":"invalid seconds value"}

curl -X POST http://localhost:8080/alloc/-5
# {"error":"invalid mb value (must be positive integer)"}
```

Unknown endpoints return 404:

```bash
curl http://localhost:8080/unknown
# {"error":"not found"}
```

Wrong HTTP method returns 405:

```bash
curl http://localhost:8080/down
# {"error":"method not allowed"}
```
