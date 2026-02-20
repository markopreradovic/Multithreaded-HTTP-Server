#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#pragma clang diagnostic ignored "-Wincompatible-pointer-types"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include <time.h>

#define LATENCY_BUCKETS 10
#define DEFAULT_PORT 8080
#define BACKLOG 128
#define MAX_EVENTS 64
#define SERVER_NAME "CServer/0.1"
#define THREAD_COUNT 4 //worker threads
#define MAX_HEADER_COUNT 64
#define MAX_HEADER_LEN 512
#define READ_BUFFER_SIZE 4096
#define CACHE_MAX_ITEMS  100     // max number of files in cache
#define CACHE_MAX_SIZE   (10 * 1024 * 1024)  // 10 MB max memorije

volatile sig_atomic_t running = 1;
//Basic metrics
static atomic_uint_fast64_t total_requests      = 0;
static atomic_uint_fast64_t cache_hits          = 0;
static atomic_uint_fast64_t cache_misses        = 0;
static atomic_uint_fast64_t successful_responses = 0;  // 2xx
static atomic_uint_fast64_t failed_responses    = 0;   // 4xx, 5xx
static atomic_uint_fast64_t latency_buckets[LATENCY_BUCKETS] = {0};
//For p50/p95/p99 approximation (simple, not exact)
static atomic_uint_fast64_t latency_sum_us = 0;          // total microseconds
static atomic_uint_fast64_t latency_count = 0;           // number of measured requests

typedef struct cache_node {
    char* filepath;              // ključ
    int   fd;                    // fajl descriptor (mmap-ovan)
    void* mapped_data;           // mmap pointer
    size_t mapped_size;          // veličina mapiranog regiona (st.st_size)
    time_t last_access;          // za LRU (ali za sada koristimo listu)
    struct cache_node* prev;
    struct cache_node* next;
} cache_node_t;

typedef struct {
    cache_node_t* head;
    cache_node_t* tail;
    int count;
    size_t total_size;
    pthread_mutex_t mutex;
} lru_cache_t;

static lru_cache_t cache = {0};

typedef struct task {
    int client_fd;
    struct sockaddr_in client_addr;
    struct task *next;
} task_t;

typedef struct {
    pthread_t* threads;
    int thread_count;
    task_t *queue_head;
    task_t *queue_tail;
    pthread_mutex_t queue_mutex;
    pthread_cond_t queue_cond;
    volatile int shutdown;
} thread_pool_t;

typedef struct {
    char method[16];
    char uri[1024];  //index.html?param=1
    char version[16]; //1.1
    char headers[MAX_HEADER_COUNT][MAX_HEADER_LEN];
    int header_count;
    size_t content_length;
    char *body;
} http_request_t;

static void free_request(http_request_t *req) {
    if (req->body) free(req->body);
    memset(req,0,sizeof(*req));
}

static int parse_request(int client_fd, http_request_t *req) {
    char buffer[READ_BUFFER_SIZE];
    ssize_t total_read = 0;
    ssize_t n;

    memset(req,0,sizeof(*req));

    //Reading until \r\n\r\n
    while (total_read < (ssize_t)(sizeof(buffer) - 1))  {
        n = recv(client_fd, buffer + total_read, sizeof(buffer) - total_read - 1, 0);
        if (n <= 0) {
            if (n == 0) printf("[Parser] Connection closed by client\n");
            else        perror("[Parser] recv");
            return -1;
        }

        total_read += n;
        buffer[total_read] = '\0';

        if (strstr(buffer, "\r\n\r\n")) {
            break;
        }
    }

    if (total_read == 0) return -1;

    char* line = strtok(buffer, "\r\n");
    if (!line) return -1;

    if (sscanf(line, "%15s %1023s %15s", req->method, req->uri, req->version) != 3) {
        printf("[Parser] Invalid request line: %s\n", line);
        return -1;
    }

    //Header parsing
    while ((line = strtok(NULL, "\r\n")) != NULL && strlen(line) > 0) {
        if (req->header_count >= MAX_HEADER_COUNT) break;

        strncpy(req->headers[req->header_count], line, MAX_HEADER_LEN - 1);
        req->headers[req->header_count][MAX_HEADER_LEN - 1] = '\0';
        req->header_count++;

        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            req->content_length = strtoul(line + 15, NULL, 10);
        }
    }
    return 0;
}

static void print_request(const http_request_t* req) {
    printf("[Worker] Parsed request:\n");
    printf("  Method: %s\n", req->method);
    printf("  URI:    %s\n", req->uri);
    printf("  Version:%s\n", req->version);
    printf("  Headers (%d):\n", req->header_count);

    for (int i = 0; i < req->header_count; i++) {
        printf("    %s\n", req->headers[i]);
    }

    if (req->content_length > 0) {
        printf("  Body expected: %zu bytes\n", req->content_length);
    }
    fflush(stdout);
}

static void send_response(int client_fd, int status, const char* body, size_t body_len, const char* content_type) {
    char header[1024];
    const char* status_text = (status == 200) ? "OK" : "Not Found";
    snprintf(header, sizeof(header),
             "HTTP/1.1 %d %s\r\n"
             "Server: %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"           // keep-alive later
             "\r\n",
             status, status_text,
             SERVER_NAME,
             content_type,
             body_len);
    //Sending header..
    send(client_fd, header, strlen(header), 0);
    if (body && body_len > 0) send(client_fd,body,body_len,0);
}

static void send_404(int client_fd) {
    const char* body =
        "<!DOCTYPE html>\n"
        "<html><head><title>404 Not Found</title></head>\n"
        "<body><h1>404 Not Found</h1><p>The requested resource was not found on this server.</p></body></html>";
    send_response(client_fd, 404, body, strlen(body), "text/html; charset=utf-8");
}

static thread_pool_t pool = {0};

static const char* get_content_type(const char* path) {
    const char* ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";

    if (strcasecmp(ext, ".html") == 0 || strcasecmp(ext, ".htm") == 0) return "text/html; charset=utf-8";
    if (strcasecmp(ext, ".css")  == 0) return "text/css; charset=utf-8";
    if (strcasecmp(ext, ".txt")  == 0) return "text/plain; charset=utf-8";
    if (strcasecmp(ext, ".jpg")  == 0 || strcasecmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, ".png")  == 0) return "image/png";
    if (strcasecmp(ext, ".gif")  == 0) return "image/gif";

    return "application/octet-stream";
}

static void cache_move_to_head(cache_node_t* node) {
    if (node == cache.head) return;

    if (node->prev) node->prev->next = node->next;
    if (node->next) node->next->prev = node->prev;
    if (node == cache.tail) cache.tail = node->prev;

    node->next = cache.head;
    node->prev = NULL;
    if (cache.head) cache.head->prev = node;
    cache.head = node;
    if (!cache.tail) cache.tail = node;
    node->last_access = time(NULL);
}

static void cache_add(cache_node_t* node) {
    node->prev = NULL;
    node->next = cache.head;
    if (cache.head) cache.head->prev = node;
    cache.head = node;
    if (!cache.tail) cache.tail = node;

    cache.count++;
    cache.total_size += node->mapped_size;
}

static void cache_evict_lru(void) {
    if (!cache.tail) return;

    cache_node_t* node = cache.tail;
    cache.tail = node->prev;
    if (cache.tail) cache.tail->next = NULL;
    else cache.head = NULL;

    cache.count--;
    cache.total_size -= node->mapped_size;

    free(node->filepath);
    free(node->mapped_data);
    free(node);

    printf("[Cache] Evicted LRU entry\n");
}

static bool cache_get_or_load(const char* filepath, int* fd_out, void** mapped_out, size_t* size_out) {
    pthread_mutex_lock(&cache.mutex);

    cache_node_t* node = cache.head;
    while (node) {
        if (strcmp(node->filepath, filepath) == 0) {
            cache_move_to_head(node);
            *fd_out = node->fd;
            *mapped_out = node->mapped_data;
            *size_out = node->mapped_size;
            pthread_mutex_unlock(&cache.mutex);
            printf("[Cache] HIT (mmap): %s (%zu bytes)\n", filepath, node->mapped_size);
            return true;
        }
        node = node->next;
    }

    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        pthread_mutex_unlock(&cache.mutex);
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size == 0) {
        close(fd);
        pthread_mutex_unlock(&cache.mutex);
        return false;
    }

    size_t file_size = st.st_size;

    while (cache.count >= CACHE_MAX_ITEMS || cache.total_size + file_size > CACHE_MAX_SIZE) {
        cache_evict_lru();
    }

    // mmap
    void* mapped = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        perror("[Cache] mmap failed");
        close(fd);
        pthread_mutex_unlock(&cache.mutex);
        return false;
    }

    cache_node_t* new_node = malloc(sizeof(cache_node_t));
    if (!new_node) {
        munmap(mapped, file_size);
        close(fd);
        pthread_mutex_unlock(&cache.mutex);
        return false;
    }

    new_node->filepath    = strdup(filepath);
    new_node->fd          = fd;
    new_node->mapped_data = mapped;
    new_node->mapped_size = file_size;
    new_node->last_access = time(NULL);

    cache_add(new_node);
    pthread_mutex_unlock(&cache.mutex);

    printf("[Cache] MISS → MMAP LOADED: %s (%zu bytes)\n", filepath, file_size);

    *fd_out = fd;
    *mapped_out = mapped;
    *size_out = file_size;
    return true;
}

static void metrics_increment_request(void) {
    atomic_fetch_add(&total_requests, 1);
}

static void metrics_increment_cache_hit(void) {
    atomic_fetch_add(&cache_hits, 1);
}

static void metrics_increment_cache_miss(void) {
    atomic_fetch_add(&cache_misses, 1);
}

static void metrics_increment_success(void) {
    atomic_fetch_add(&successful_responses, 1);
}

static void metrics_increment_failure(void) {
    atomic_fetch_add(&failed_responses, 1);
}

// Serve file - returns HTTP status code (200, 404, etc.)
static int serve_file(int client_fd, const char* filepath) {
    metrics_increment_request();

    printf("[serve_file] Trying: %s\n", filepath);

    int mapped_fd = -1;
    void* mapped_ptr = NULL;
    size_t mapped_size = 0;

    if (cache_get_or_load(filepath, &mapped_fd, &mapped_ptr, &mapped_size)) {
        metrics_increment_cache_hit();

        char header[1024];
        snprintf(header, sizeof(header),
                 "HTTP/1.1 200 OK\r\n"
                 "Server: %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 SERVER_NAME,
                 get_content_type(filepath),
                 mapped_size);

        send(client_fd, header, strlen(header), 0);

        off_t offset = 0;
        while (offset < mapped_size) {
            ssize_t sent = sendfile(client_fd, mapped_fd, &offset, mapped_size - offset);
            if (sent <= 0) {
                if (sent < 0) perror("[serve_file] sendfile cache failed");
                break;
            }
        }
        return 200;  // success
    }

    metrics_increment_cache_miss();

    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        perror("[serve_file] open failed");
        send_404(client_fd);
        return 404;  // not found
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        send_404(client_fd);
        return 500;  // internal error
    }

    size_t file_size = st.st_size;

    char header[1024];
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\n"
             "Server: %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n",
             SERVER_NAME,
             get_content_type(filepath),
             file_size);

    send(client_fd, header, strlen(header), 0);

    off_t offset = 0;
    while (offset < file_size) {
        ssize_t sent = sendfile(client_fd, fd, &offset, file_size - offset);
        if (sent <= 0) {
            if (sent < 0) perror("[serve_file] sendfile fallback failed");
            break;
        }
    }

    close(fd);
    return 200;  // success
}

// Record latency in microseconds
static void metrics_record_latency(uint64_t start_ns) {
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    uint64_t end_ns = end.tv_sec * 1000000000ULL + end.tv_nsec;
    uint64_t duration_ns = end_ns - start_ns;
    uint64_t duration_us = duration_ns / 1000;

    // Update sum and count
    atomic_fetch_add(&latency_sum_us, duration_us);
    atomic_fetch_add(&latency_count, 1);

    // Bucket it
    int bucket;
    if (duration_us < 1000) bucket = 0;           // <1ms
    else if (duration_us < 5000) bucket = 1;      // 1-5ms
    else if (duration_us < 10000) bucket = 2;     // 5-10ms
    else if (duration_us < 50000) bucket = 3;     // 10-50ms
    else if (duration_us < 100000) bucket = 4;    // 50-100ms
    else bucket = 5;                               // >100ms

    atomic_fetch_add(&latency_buckets[bucket], 1);
}

static void cache_init(void) {
    pthread_mutex_init(&cache.mutex, NULL);
    cache.head = cache.tail = NULL;
    cache.count = 0;
    cache.total_size = 0;
    printf("[MMAP Cache] Initialized (max items: %d, max size: %zu MB)\n",
           CACHE_MAX_ITEMS, CACHE_MAX_SIZE / (1024*1024));
}

static void cache_destroy(void) {
    pthread_mutex_lock(&cache.mutex);

    cache_node_t* current = cache.head;
    while (current) {
        cache_node_t* next = current->next;

        if (current->mapped_data != MAP_FAILED && current->mapped_data != NULL) {
            munmap(current->mapped_data, current->mapped_size);
        }
        if (current->fd >= 0) close(current->fd);
        free(current->filepath);
        free(current);

        current = next;
    }

    cache.head = cache.tail = NULL;
    cache.count = 0;
    cache.total_size = 0;

    pthread_mutex_unlock(&cache.mutex);
    pthread_mutex_destroy(&cache.mutex);
    printf("[MMAP Cache] Destroyed\n");
}



static void metrics_print_summary(void) {
    uint64_t total    = atomic_load_explicit(&total_requests,      memory_order_relaxed);
    uint64_t hits     = atomic_load_explicit(&cache_hits,          memory_order_relaxed);
    uint64_t misses   = atomic_load_explicit(&cache_misses,        memory_order_relaxed);
    uint64_t success  = atomic_load_explicit(&successful_responses, memory_order_relaxed);
    uint64_t failures = atomic_load_explicit(&failed_responses,    memory_order_relaxed);

    double hit_rate = (hits + misses > 0) ? (double)hits / (hits + misses) * 100.0 : 0.0;

    uint64_t count = atomic_load_explicit(&latency_count, memory_order_relaxed);
    uint64_t sum_us = atomic_load_explicit(&latency_sum_us, memory_order_relaxed);
    double avg_latency_ms = count > 0 ? (double)sum_us / count / 1000.0 : 0.0;

    // Simple p50/p95/p99 approximation (not exact, but good enough)
    uint64_t cumulative = 0;
    double p50 = 0, p95 = 0, p99 = 0;
    if (count > 0) {
        uint64_t p50_target = count * 50 / 100;
        uint64_t p95_target = count * 95 / 100;
        uint64_t p99_target = count * 99 / 100;

        for (int i = 0; i < LATENCY_BUCKETS; i++) {
            cumulative += atomic_load_explicit(&latency_buckets[i], memory_order_relaxed);
            if (p50 == 0 && cumulative >= p50_target) p50 = i * 10;  // rough bucket mid
            if (p95 == 0 && cumulative >= p95_target) p95 = i * 10;
            if (p99 == 0 && cumulative >= p99_target) p99 = i * 10;
        }
    }

    printf("\n[Metrics Summary]\n");
    printf("  Total requests:     %lu\n", total);
    printf("  Cache hits:         %lu\n", hits);
    printf("  Cache misses:       %lu\n", misses);
    printf("  Cache hit rate:     %.2f%%\n", hit_rate);
    printf("  Successful (2xx):   %lu\n", success);
    printf("  Failed (4xx/5xx):   %lu\n", failures);
    printf("  Avg latency:        %.2f ms\n", avg_latency_ms);
    printf("  Latency p50:        ~%.0f ms\n", p50);
    printf("  Latency p95:        ~%.0f ms\n", p95);
    printf("  Latency p99:        ~%.0f ms\n", p99);
    fflush(stdout);
}

// Simple JSON metrics response
static void send_metrics_json(int client_fd) {
    uint64_t total    = atomic_load_explicit(&total_requests,      memory_order_relaxed);
    uint64_t hits     = atomic_load_explicit(&cache_hits,          memory_order_relaxed);
    uint64_t misses   = atomic_load_explicit(&cache_misses,        memory_order_relaxed);
    uint64_t success  = atomic_load_explicit(&successful_responses, memory_order_relaxed);
    uint64_t failures = atomic_load_explicit(&failed_responses,    memory_order_relaxed);

    double hit_rate = (hits + misses > 0) ? (double)hits / (hits + misses) * 100.0 : 0.0;

    char json[2048];
    snprintf(json, sizeof(json),
             "{\n"
             "  \"requests\": {\n"
             "    \"total\": %lu,\n"
             "    \"successful\": %lu,\n"
             "    \"failed\": %lu\n"
             "  },\n"
             "  \"cache\": {\n"
             "    \"hits\": %lu,\n"
             "    \"misses\": %lu,\n"
             "    \"hit_rate_percent\": %.2f\n"
             "  }\n"
             "}\n",
             total, success, failures, hits, misses, hit_rate);

    char header[1024];
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\n"
             "Server: %s\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n",
             SERVER_NAME,
             strlen(json));

    send(client_fd, header, strlen(header), 0);
    send(client_fd, json, strlen(json), 0);
}

// Worker thread - main request processing loop
static void* worker_thread(void* arg) {
    (void)arg;

    while (1) {
        task_t* task = NULL;

        // Wait for task
        pthread_mutex_lock(&pool.queue_mutex);

        while (pool.queue_head == NULL && !pool.shutdown) {
            pthread_cond_wait(&pool.queue_cond, &pool.queue_mutex);
        }

        if (pool.shutdown) {
            pthread_mutex_unlock(&pool.queue_mutex);
            return NULL;
        }

        task = pool.queue_head;
        pool.queue_head = task->next;
        if (pool.queue_head == NULL) {
            pool.queue_tail = NULL;
        }

        pthread_mutex_unlock(&pool.queue_mutex);

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &task->client_addr.sin_addr, client_ip, sizeof(client_ip));

        uint16_t client_port = ntohs(task->client_addr.sin_port);

        printf("[Worker] Processing connection from %s:%u (fd=%d)\n",
               client_ip, client_port, task->client_fd);

        // Start latency measurement
        struct timespec start;
        clock_gettime(CLOCK_MONOTONIC, &start);
        uint64_t start_ns = start.tv_sec * 1000000000ULL + start.tv_nsec;

        http_request_t req;
        memset(&req, 0, sizeof(req));

        // Parse HTTP request
        if (parse_request(task->client_fd, &req) != 0) {
            printf("[Worker] Parse failed or connection closed early from %s:%u\n",
                   client_ip, client_port);
            send_response(task->client_fd, 400, "<h1>400 Bad Request</h1>", 23, "text/html");
            metrics_increment_failure();
            metrics_record_latency(start_ns);
            goto cleanup;
        }

        print_request(&req);

        // Special /metrics endpoint
        if (strcmp(req.uri, "/metrics") == 0) {
            printf("[Worker] Serving /metrics JSON from %s:%u\n", client_ip, client_port);
            send_metrics_json(task->client_fd);
            metrics_increment_success();
            metrics_record_latency(start_ns);
            goto cleanup;
        }

        char full_path[1024] = {0};
        const char* document_root = "./www";

        // Basic path protection
        if (strstr(req.uri, "..") != NULL ||
            strstr(req.uri, "/.") != NULL ||
            strstr(req.uri, "\\") != NULL) {
            printf("[Worker] Forbidden path attempt: %s from %s:%u\n",
                   req.uri, client_ip, client_port);
            send_response(task->client_fd, 403, "<h1>403 Forbidden</h1>", 23, "text/html");
            metrics_increment_failure();
            metrics_record_latency(start_ns);
            goto cleanup;
        }

        // Map URI to file path
        if (strcmp(req.uri, "/") == 0 || strcmp(req.uri, "") == 0) {
            snprintf(full_path, sizeof(full_path), "%s/index.html", document_root);
        } else {
            if (req.uri[0] != '/') {
                snprintf(full_path, sizeof(full_path), "%s/%s", document_root, req.uri);
            } else {
                snprintf(full_path, sizeof(full_path), "%s%s", document_root, req.uri);
            }
        }

        printf("[Worker] Attempting to serve: %s for URI %s from %s:%u\n",
               full_path, req.uri, client_ip, client_port);

        // Serve file and get status
        int status = serve_file(task->client_fd, full_path);

        // Update success/failure based on returned status
        if (status >= 200 && status < 300) {
            metrics_increment_success();
        } else {
            metrics_increment_failure();
        }

        // Record latency
        metrics_record_latency(start_ns);

    cleanup:
        free_request(&req);
        close(task->client_fd);
        free(task);
    }

    return NULL;
}



static int thread_pool_init(int num_threads) {
    pool.thread_count = num_threads;
    pool.threads = malloc(num_threads * sizeof(pthread_t));
    if (!pool.threads) return -1;

    pthread_mutex_init(&pool.queue_mutex, NULL);
    pthread_cond_init(&pool.queue_cond, NULL);

    pool.queue_head = pool.queue_tail = NULL;
    pool.shutdown = 0;

    for (int i=0; i<num_threads; i++) {
        if (pthread_create(&pool.threads[i], NULL, worker_thread, NULL) != 0) {
            fprintf(stderr, "Failed to create worker thread %d\n", i);
            // cleanup...
            return -1;
        }
    }

    printf("Thread pool started with %d workers\n", num_threads);
    return 0;
}

static void thread_pool_shutdown() {
    pthread_mutex_lock(&pool.queue_mutex);
    pool.shutdown = 1;
    pthread_cond_broadcast(&pool.queue_cond);
    pthread_mutex_unlock(&pool.queue_mutex);

    for (int i = 0; i < pool.thread_count; i++) {
        pthread_join(pool.threads[i], NULL);
    }

    free(pool.threads);
    task_t* current = pool.queue_head;
    while (current) {
        task_t* next = current->next;
        close(current->client_fd);
        free(current);
        current = next;
    }
    pthread_mutex_destroy(&pool.queue_mutex);
    pthread_cond_destroy(&pool.queue_cond);
}

static void enqueue_task(int client_fd, struct sockaddr_in* client_addr) {
    task_t* task = malloc(sizeof(task_t));
    if (!task) {
        close(client_fd);
        return;
    }

    task->client_fd = client_fd;
    task->client_addr = *client_addr;
    task->next = NULL;

    pthread_mutex_lock(&pool.queue_mutex);

    if (pool.queue_tail) {
        pool.queue_tail->next = task;
    } else {
        pool.queue_head = task;
    }
    pool.queue_tail = task;

    pthread_cond_signal(&pool.queue_cond);
    pthread_mutex_unlock(&pool.queue_mutex);
}

static void signal_handler(int sig) {
    (void) sig;
    running = 0;
}

static int create_and_bind_socket(uint16_t port) {
    //Creating TCP socket (IPv4)
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket failed");
        return -1;
    }

    //Enabling the port to be used right after shutdown (without it there will be 3 to 4 minutes wait time)
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        close(server_fd);
        return -1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;          //IPv4
    addr.sin_addr.s_addr = INADDR_ANY;  // 0.0.0.0 - All network interfaces
    addr.sin_port = htons(port);        // Converting port to network byte order

    // Connecting socket with an address and port
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        close(server_fd);
        return -1;
    }

    //Listening to upcoming connections (BACKLOG = how many connections can wait in queue before being refused)
    if(listen(server_fd, BACKLOG) < 0) {
        perror("listen failed");
        close(server_fd);
        return -1;
    }
    printf("Server starting and listening on port %u\n", port);
    return server_fd;
}



int main(void) {
    //Handlers.
    signal(SIGINT, signal_handler); //Ctrl C
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN); //useful for writing on an closed socket

    //Socket binding.
    int server_fd = create_and_bind_socket(DEFAULT_PORT);
    if (server_fd < 0) return EXIT_FAILURE;

    if (thread_pool_init(THREAD_COUNT) != 0) {
        close(server_fd);
        return EXIT_FAILURE;
    }

    cache_init();
    //EPOLL init
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1 failed");
        thread_pool_shutdown();
        close(server_fd);
        return EXIT_FAILURE;
    }

    //adding server socket to epoll
    struct epoll_event ev = { .events=EPOLLIN, .data.fd = server_fd};
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);

    struct epoll_event events[MAX_EVENTS];

    printf("Server running on port %d with %d workers. Press Ctrl+C to shut down.\n", DEFAULT_PORT, THREAD_COUNT);
    fflush(stdout);

    while (running) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR && !running) break;
            perror("epoll_wait");
            continue;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == server_fd) {
                struct sockaddr_in client_addr;
                socklen_t len = sizeof(client_addr);

                int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &len);
                if (client_fd < 0) {
                    perror("accept");
                    continue;
                }

                enqueue_task(client_fd, &client_addr);
            }
        }
        static time_t last_print = 0;
        time_t now = time(NULL);
        if (now - last_print >= 10) {
            metrics_print_summary();
            last_print = now;
        }
    }


    printf("\nShutting down...\n");
    metrics_print_summary();
    close(epoll_fd);
    cache_destroy();
    thread_pool_shutdown();
    close(server_fd);
    printf("Server closed.\n");

    return EXIT_SUCCESS;
}