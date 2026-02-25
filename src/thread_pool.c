#include "thread_pool.h"
#include "http.h"
#include "cache.h"
#include "metrics.h"
#include "config.h"
#include "logger.h"
#include "server.h"

thread_pool_t pool = {0};

static void* worker_thread(void* arg) {
    (void)arg;

    while (1) {
        task_t* task = NULL;
        int status_code = 200;

        pthread_mutex_lock(&pool.queue_mutex);
        while (pool.queue_head == NULL && !pool.shutdown)
            pthread_cond_wait(&pool.queue_cond, &pool.queue_mutex);

        if (pool.shutdown) {
            pthread_mutex_unlock(&pool.queue_mutex);
            return NULL;
        }

        task = pool.queue_head;
        pool.queue_head = task->next;
        if (pool.queue_head == NULL) pool.queue_tail = NULL;
        pthread_mutex_unlock(&pool.queue_mutex);

        struct timespec ts_start;
        clock_gettime(CLOCK_MONOTONIC, &ts_start);

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &task->client_addr.sin_addr,
                  client_ip, sizeof(client_ip));
        uint16_t client_port = ntohs(task->client_addr.sin_port);

        printf("[Worker] Processing connection from %s:%u (fd=%d)\n",
               client_ip, client_port, task->client_fd);

        http_request_t req;
        memset(&req, 0, sizeof(req));

        if (parse_request(task->client_fd, &req) != 0) {
            printf("[Worker] Parse failed or connection closed early from %s:%u\n",
                   client_ip, client_port);
            status_code = 400;
            send_response(task->client_fd, 400,
                          "<h1>400 Bad Request</h1>", 23, "text/html");
            goto cleanup;
        }

        print_request(&req);

        {
            char full_path[1024] = {0};

            if (strstr(req.uri, "..") != NULL ||
                strstr(req.uri, "/.") != NULL ||
                strstr(req.uri, "\\") != NULL) {
                printf("[Worker] Forbidden path attempt: %s from %s:%u\n",
                       req.uri, client_ip, client_port);
                status_code = 403;
                send_response(task->client_fd, 403,
                              "<h1>403 Forbidden</h1>", 23, "text/html");
                goto cleanup;
            }

            char uri_path[1024];
            char* q = strchr(req.uri, '?');
            if (q) {
                size_t len = q - req.uri;
                strncpy(uri_path, req.uri, len);
                uri_path[len] = '\0';
            } else {
                strncpy(uri_path, req.uri, sizeof(uri_path) - 1);
                uri_path[sizeof(uri_path) - 1] = '\0';
            }

            if (strcmp(uri_path, "/") == 0 || strcmp(uri_path, "") == 0 ||
                strcmp(uri_path, "/index.html") == 0) {
                snprintf(full_path, sizeof(full_path),
                         "%s/index.html", document_root);
            } else {
                snprintf(full_path, sizeof(full_path),
                         "%s%s", document_root, uri_path);
            }

            const char* ext = strrchr(full_path, '.');
            if (ext && strcasecmp(ext, ".cgi") == 0) {
                printf("[Worker] Executing CGI: %s for URI %s from %s:%u\n",
                       full_path, req.uri, client_ip, client_port);
                status_code = execute_cgi(task->client_fd, full_path, &req);
                goto cleanup;
            }

            printf("[Worker] Attempting to serve: %s for URI %s from %s:%u\n",
                   full_path, req.uri, client_ip, client_port);
            status_code = serve_file(task->client_fd, full_path);
        }

        cleanup: {
            struct timespec ts_end;
            clock_gettime(CLOCK_MONOTONIC, &ts_end);
            long duration_us = (ts_end.tv_sec - ts_start.tv_sec) * 1000000L
                             + (ts_end.tv_nsec - ts_start.tv_nsec) / 1000L;

            log_access(client_ip, client_port, req.method, req.uri,
                       status_code, duration_us);

            if (status_code >= 200 && status_code < 300)
                metrics_increment_success();
            else
                metrics_increment_failure();

            metrics_record_latency(
                ts_start.tv_sec * 1000000000ULL + ts_start.tv_nsec);

            free_request(&req);
            close(task->client_fd);
            free(task);
        }
    }
}

int thread_pool_init(int num_threads) {
    pool.thread_count = num_threads;
    pool.threads = malloc(num_threads * sizeof(pthread_t));
    if (!pool.threads) return -1;

    pthread_mutex_init(&pool.queue_mutex, NULL);
    pthread_cond_init(&pool.queue_cond,   NULL);

    pool.queue_head = pool.queue_tail = NULL;
    pool.shutdown = 0;

    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&pool.threads[i], NULL, worker_thread, NULL) != 0) {
            fprintf(stderr, "Failed to create worker thread %d\n", i);
            return -1;
        }
    }

    printf("Thread pool started with %d workers\n", num_threads);
    return 0;
}

void thread_pool_shutdown(void) {
    pthread_mutex_lock(&pool.queue_mutex);
    pool.shutdown = 1;
    pthread_cond_broadcast(&pool.queue_cond);
    pthread_mutex_unlock(&pool.queue_mutex);

    for (int i = 0; i < pool.thread_count; i++)
        pthread_join(pool.threads[i], NULL);

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

void enqueue_task(int client_fd, struct sockaddr_in* client_addr) {
    task_t* task = malloc(sizeof(task_t));
    if (!task) {
        close(client_fd);
        return;
    }

    task->client_fd   = client_fd;
    task->client_addr = *client_addr;
    task->next        = NULL;

    pthread_mutex_lock(&pool.queue_mutex);

    if (pool.queue_tail) pool.queue_tail->next = task;
    else                 pool.queue_head = task;
    pool.queue_tail = task;

    pthread_cond_signal(&pool.queue_cond);
    pthread_mutex_unlock(&pool.queue_mutex);
}