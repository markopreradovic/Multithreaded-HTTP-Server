# Multithreaded High-Performance HTTP/1.1 Server in C

Production-grade, concurrent HTTP/1.1 web server written entirely in pure C using modern Linux system programming techniques.

## Overview

This project is a high-performance HTTP/1.1 web server implemented in pure C with a focus on low-level systems programming, performance, and concurrency. The server uses epoll for non-blocking I/O multiplexing and a fixed-size thread pool to process client requests concurrently. The architecture separates event handling from request processing, enabling efficient scaling under high load while maintaining predictable performance characteristics.

Static files are served using zero-copy techniques via sendfile(), minimizing memory copies between kernel and user space. Frequently accessed files are cached in memory using an LRU eviction policy. Cached files are backed by mmap with MAP_PRIVATE and PROT_READ, allowing efficient memory usage while keeping the implementation simple and safe.

The server includes a custom-built HTTP/1.1 request parser capable of parsing the request line, headers, and Content-Length. It currently operates in Connection: close mode and does not support keep-alive connections or chunked transfer encoding. Basic path sanitization is implemented to prevent directory traversal attacks.

Runtime metrics are collected using atomic counters and exposed through a /metrics JSON endpoint. Latency is measured using histogram buckets, allowing approximate percentile calculations such as p50, p95, and p99. The server supports graceful shutdown on SIGINT and SIGTERM signals.

## Architecture

The core of the server is based on an event-driven loop using epoll for readiness notifications. Incoming connections are accepted in non-blocking mode and registered with epoll. Work items are dispatched to a thread pool through a thread-safe task queue implemented as a linked list protected by a mutex and condition variable.

The in-memory cache uses an LRU strategy with configurable limits on maximum number of items and total memory usage. When limits are exceeded, the least recently used entries are evicted. This design improves performance for frequently accessed static assets while maintaining memory constraints.

## Limitations

The server currently supports only HTTP/1.1 and does not implement HTTP/2 or TLS. Request bodies are detected but not processed, and chunked encoding is not supported. All configuration values such as port, number of threads, cache limits, and document root are hard-coded. There is no structured logging, connection timeout handling, rate limiting, or advanced security hardening beyond basic path validation.



## Purpose

This project serves as a demonstration of high-performance network programming in C, covering epoll-based I/O, thread pools, zero-copy file transfer, memory-mapped file caching, atomic metrics collection, and concurrent data structures.
