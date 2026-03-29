# Multithreaded HTTP Server in C

A high-performance, production-oriented HTTP/1.1 server written in C from scratch, featuring a thread pool with configurable worker count, an LRU file cache backed by `mmap` for zero-copy serving, an `epoll`-driven event loop for scalable connection handling, CGI/1.1 script execution via process forking, INI-based runtime configuration with live reload, and a real-time metrics system with latency percentile tracking. Every component — from the TCP socket and HTTP parser to the cache eviction policy and thread synchronization — is implemented at the systems level without any external networking or HTTP libraries.

---

## Table of Contents

- [Overview](#overview)
- [Features](#features)
- [Architecture](#architecture)
- [Project Structure](#project-structure)
- [Modules](#modules)
- [Configuration](#configuration)
- [Building](#building)
- [Running](#running)
- [Testing with curl](#testing-with-curl)
- [CGI Support](#cgi-support)
- [Metrics](#metrics)
- [Signal Handling](#signal-handling)
- [Security](#security)
- [Technical Details](#technical-details)

---

## Overview

This project is a fully functional HTTP/1.1 server implemented in C11, targeting Linux systems. It is designed entirely around core Unix systems programming concepts: non-blocking I/O multiplexing with `epoll`, concurrent request processing with POSIX threads, memory-mapped file I/O with `mmap`, zero-copy file transfer with `sendfile`, and CGI script execution via `fork` and `execve`. The server handles multiple simultaneous connections through a fixed-size thread pool and serves static files efficiently using an LRU cache that keeps frequently accessed files mapped in memory, avoiding repeated disk reads.

The project was built as a deep dive into how production HTTP servers actually work under the hood. Rather than using a framework or library, every layer is written by hand: the TCP socket is created and bound manually, the HTTP request line and headers are parsed from raw bytes, file content is served through kernel-space transfer calls, cache eviction is managed with a doubly linked list, and worker threads are coordinated through a mutex-protected task queue and condition variables.

The scope of the project covers the full lifecycle of an HTTP request — from `accept` on the listening socket, through parsing and routing, to response generation and connection teardown — as well as the surrounding infrastructure that makes a server operationally useful: structured logging with timestamps and log levels, an access log with per-request timing, a real-time metrics system using lock-free atomic counters, a JSON metrics endpoint, live configuration reload via Unix signals, and protection against common path traversal attacks.

This is not a toy server that handles one request at a time. It is structured the way real servers are structured: a single-threaded event loop accepts connections as fast as possible and hands them off to a pool of workers, each operating independently. The codebase is split into focused modules with clear interfaces, compiled through CMake, and tested against real HTTP clients.

---

## Features

- HTTP/1.1 request parsing with support for headers, methods, URIs, and query strings
- Static file serving from a configurable document root
- LRU file cache using `mmap` for zero-copy file serving via `sendfile`
- Thread pool with configurable worker count for concurrent request handling
- `epoll`-based event loop for scalable connection acceptance
- CGI/1.1 script execution via `fork` and `execve` with environment variable injection
- INI-based configuration file (`server.conf`) with runtime reload via `SIGHUP`
- Structured access logging with timestamps and request duration
- Real-time metrics: request counts, cache hit rate, latency distribution (p50/p95/p99)
- JSON metrics endpoint (`/metrics`)
- Path traversal protection (`..`, `/.`, `\` detection)
- Graceful shutdown on `SIGINT` and `SIGTERM`
- Content-Type detection based on file extension
- 404 and 400/403/500 error responses with HTML bodies

---

## Architecture

The server uses a classic **accept-dispatch** pattern:

1. A single main thread runs an `epoll` event loop and accepts incoming TCP connections.
2. Each accepted connection is wrapped in a task and pushed onto a thread-safe queue.
3. Worker threads from the thread pool dequeue tasks and process HTTP requests independently.
4. Responses are served either from the mmap cache (cache hit) or directly from disk (cache miss), using `sendfile` for efficient kernel-space file transfer.
5. CGI requests are handled by forking a child process, redirecting its stdout to a pipe, and streaming the output back to the client.

```
Client
  |
  v
[epoll event loop]  <-- main thread
  |
  v
[Task Queue]  <-- mutex + condition variable
  |
  +---> [Worker Thread 1]
  +---> [Worker Thread 2]
  +---> [Worker Thread 3]
  +---> [Worker Thread 4]
         |
         v
    [HTTP Parser]
         |
         +---> [LRU Cache] ---> sendfile()
         |
         +---> [CGI fork/execve]
         |
         +---> [404 / Error Response]
```

---

## Project Structure

```
Multithreaded-HTTP-Server/
├── CMakeLists.txt
├── README.md
├── .gitignore
├── config/
│   └── server.conf
├── src/
│   ├── main.c
│   ├── common.h
│   ├── server.h / server.c
│   ├── http.h / http.c
│   ├── cache.h / cache.c
│   ├── thread_pool.h / thread_pool.c
│   ├── metrics.h / metrics.c
│   ├── logger.h / logger.c
│   ├── config.h / config.c
│   └── www/
│       └── index.html
├── cgi-bin/
│   └── hello.cgi
└── tests/
```

---

## Modules

### `main.c`
The entry point of the server. Loads configuration, initializes all subsystems (socket, thread pool, cache, epoll), runs the main event loop, handles reload signals, prints periodic metrics, and performs graceful shutdown.

### `server.h / server.c`
Contains the TCP socket creation and binding logic (`create_and_bind_socket`), the static file serving function (`serve_file`), CGI execution (`execute_cgi`), and signal handler registration. Owns the `running` and `reload` volatile flags used across the process.

### `http.h / http.c`
Implements HTTP/1.1 request parsing. Reads from the client socket into a buffer, extracts the request line (method, URI, version), parses headers into a fixed-size array, and extracts `Content-Length` for body size validation. Also provides response sending utilities (`send_response`, `send_404`), content-type resolution by file extension, and keep-alive detection.

### `cache.h / cache.c`
An LRU (Least Recently Used) file cache backed by `mmap`. Files are mapped into memory on first access and served on subsequent requests without touching disk. Eviction occurs when the cache exceeds the configured item count or total size limit. The cache is protected by a `pthread_mutex_t` for thread-safe access from multiple workers.

### `thread_pool.h / thread_pool.c`
A fixed-size POSIX thread pool. Worker threads block on a condition variable waiting for tasks. The main thread enqueues tasks (client file descriptors + addresses) via a mutex-protected linked list. Each worker dequeues a task, processes the full HTTP request lifecycle, logs the access, updates metrics, and closes the connection.

### `metrics.h / metrics.c`
Atomic counters for total requests, cache hits/misses, successful and failed responses, and latency measurements. Latency is recorded per request in microseconds and bucketed for approximate percentile (p50/p95/p99) calculation. Results are printed to stdout every 10 seconds and served as JSON via the `/metrics` endpoint.

### `logger.h / logger.c`
Structured logging with timestamps and log levels (`INFO`, `ERROR`). Writes to both stderr and an optionally configured log file (`server.log`). Access logging records the client IP, port, HTTP method, URI, status code, and request duration.

### `config.h / config.c`
A minimal INI file parser. Supports sections (`[server]`, `[cache]`, `[logging]`), key-value pairs, and whitespace trimming. Returns `strdup`-allocated strings for all values, with a fallback default if the key or file is not found. The global `document_root` pointer is defined here.

---

## Configuration

The server reads `server.conf` from the working directory at startup. Example configuration:

```ini
[server]
port          = 9090
document_root = ./www
thread_count  = 4

[cache]
max_items    = 100
max_size_mb  = 10

[logging]
level = INFO
```

| Key | Description | Default |
|-----|-------------|---------|
| `port` | TCP port to listen on | `8080` |
| `document_root` | Root directory for static files | `./www` |
| `thread_count` | Number of worker threads | `4` |
| `max_items` | Maximum number of files in cache | `100` |
| `max_size_mb` | Maximum total cache size in megabytes | `10` |
| `level` | Log level: `INFO` or `ERROR` | `INFO` |

Configuration can be reloaded at runtime without restarting the server by sending `SIGHUP`:

```bash
kill -HUP <pid>
```

---

## Building

**Requirements:**
- GCC or Clang
- CMake 3.16 or newer
- Linux (epoll, sendfile, mmap are Linux-specific)
- pthreads and librt (standard on all Linux distributions)

**Build steps:**

```bash
git clone https://github.com/markopreradovic/Multithreaded-HTTP-Server.git
cd Multithreaded-HTTP-Server
mkdir build && cd build
cmake ..
make
```

The resulting binary is `httpserver` inside the `build/` directory.

---

## Running

```bash
cd build
cp ../config/server.conf .
cp -r ../src/www .
./httpserver
```

The server will read `server.conf` and `./www/` from the directory it is launched from.

---

## Testing with curl

**Basic GET request:**
```bash
curl -v http://localhost:9090/
```

**Specific file:**
```bash
curl http://localhost:9090/index.html
```

**Non-existent path (404):**
```bash
curl http://localhost:9090/doesnotexist
```

**POST request:**
```bash
curl -X POST -d "data=hello" http://localhost:9090/hello.cgi
```

**CGI with query string:**
```bash
curl "http://localhost:9090/hello.cgi?name=Marko"
```

**Metrics endpoint:**
```bash
curl http://localhost:9090/metrics
```

---

## CGI Support

The server supports CGI/1.1 for files with the `.cgi` extension. When a matching request is received, the server:

1. Creates a pipe
2. Forks a child process
3. Redirects the child's stdout to the write end of the pipe
4. Calls `execve` with a set of CGI environment variables
5. Streams the child's output back to the client

The following CGI environment variables are injected:

| Variable | Value |
|----------|-------|
| `GATEWAY_INTERFACE` | `CGI/1.1` |
| `SERVER_NAME` | `CServer/0.1` |
| `REQUEST_METHOD` | e.g. `GET`, `POST` |
| `PATH_INFO` | The full request URI |
| `QUERY_STRING` | Everything after `?` in the URI |

CGI scripts must be executable. Example:

```bash
chmod +x www/hello.cgi
```

---

## Metrics

The server tracks the following metrics using lock-free atomic operations:

- Total request count
- Cache hit and miss count
- Cache hit rate (percentage)
- Successful responses (2xx)
- Failed responses (4xx, 5xx)
- Average request latency in milliseconds
- Approximate latency percentiles: p50, p95, p99

Metrics are printed to stdout every 10 seconds and are available as JSON:

```bash
curl http://localhost:9090/metrics
```

Example JSON response:

```json
{
  "requests": {
    "total": 142,
    "successful": 138,
    "failed": 4
  },
  "cache": {
    "hits": 130,
    "misses": 12,
    "hit_rate_percent": 91.55
  }
}
```

---

## Signal Handling

| Signal | Behaviour |
|--------|-----------|
| `SIGINT` | Graceful shutdown — waits for workers to finish, destroys cache, closes sockets |
| `SIGTERM` | Same as SIGINT |
| `SIGHUP` | Reloads `server.conf` at runtime without restarting |
| `SIGPIPE` | Ignored — prevents crash on broken client connections |

---

## Security

The server implements basic path traversal protection. Any request URI containing `..`, `/.`, or `\` is rejected with a `403 Forbidden` response before any file system access occurs. This prevents clients from escaping the document root.

Request and header size limits are enforced during parsing:

| Limit | Value |
|-------|-------|
| Max header block size | 8 KB |
| Max number of headers | 64 |
| Max body size | 1 MB |

---
