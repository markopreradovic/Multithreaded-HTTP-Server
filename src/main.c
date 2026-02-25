#include "common.h"
#include "config.h"
#include "logger.h"
#include "cache.h"
#include "metrics.h"
#include "http.h"
#include "thread_pool.h"
#include "server.h"

int main(void) {
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP,  signal_handler);

    char* port_str        = get_ini_value("server.conf", "server",  "port",          "8080");
    char* doc_root_str    = get_ini_value("server.conf", "server",  "document_root", "./www");
    char* threads_str     = get_ini_value("server.conf", "server",  "thread_count",  "4");
    char* cache_items_str = get_ini_value("server.conf", "cache",   "max_items",     "100");
    char* cache_size_str  = get_ini_value("server.conf", "cache",   "max_size_mb",   "10");
    char* log_level_str   = get_ini_value("server.conf", "logging", "level",         "INFO");

    uint16_t    port           = (uint16_t)atoi(port_str);
    document_root              = doc_root_str;
    int         thread_count   = atoi(threads_str);
    int         cache_max_items __attribute__((unused)) = atoi(cache_items_str);
    size_t      cache_max_size  __attribute__((unused)) =
                    (size_t)atoi(cache_size_str) * 1024 * 1024;

    if (strcasecmp(log_level_str, "ERROR") == 0)
        current_log_level = LOG_ERROR;
    else
        current_log_level = LOG_INFO;

    log_file = fopen(log_file_path, "a");
    if (!log_file)
        log_message(LOG_ERROR, "Failed to open log file %s", log_file_path);

    free(port_str);
    free(threads_str);
    free(cache_items_str);
    free(cache_size_str);
    free(log_level_str);

    log_message(LOG_INFO, "Server starting...");
    log_message(LOG_INFO,
        "Loaded configuration: port=%u, document_root=%s, threads=%d, "
        "cache_items=%d, cache_size=%.1f MB",
        port, document_root, thread_count,
        cache_max_items, (double)cache_max_size / (1024 * 1024));

    int server_fd = create_and_bind_socket(port);
    if (server_fd < 0) {
        log_message(LOG_ERROR, "Failed to bind socket on port %u", port);
        if (log_file) fclose(log_file);
        free((void*)document_root);
        return EXIT_FAILURE;
    }

    if (thread_pool_init(thread_count) != 0) {
        log_message(LOG_ERROR,
            "Failed to initialize thread pool with %d workers", thread_count);
        close(server_fd);
        if (log_file) fclose(log_file);
        free((void*)document_root);
        return EXIT_FAILURE;
    }

    cache_init();

    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        log_message(LOG_ERROR, "epoll_create1 failed");
        cache_destroy();
        thread_pool_shutdown();
        close(server_fd);
        if (log_file) fclose(log_file);
        free((void*)document_root);
        return EXIT_FAILURE;
    }

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = server_fd };
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
        log_message(LOG_ERROR, "epoll_ctl ADD server_fd failed");
        close(epoll_fd);
        cache_destroy();
        thread_pool_shutdown();
        close(server_fd);
        if (log_file) fclose(log_file);
        free((void*)document_root);
        return EXIT_FAILURE;
    }

    struct epoll_event events[MAX_EVENTS];

    log_message(LOG_INFO,
        "Server running on port %u with %d workers. Press Ctrl+C to shut down.",
        port, thread_count);

    while (running) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR && !running) break;
            log_message(LOG_ERROR, "epoll_wait failed: %s", strerror(errno));
            continue;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == server_fd) {
                struct sockaddr_in client_addr;
                socklen_t len = sizeof(client_addr);

                int client_fd = accept(server_fd,
                                       (struct sockaddr*)&client_addr, &len);
                if (client_fd < 0) {
                    log_message(LOG_ERROR, "accept failed: %s", strerror(errno));
                    continue;
                }

                enqueue_task(client_fd, &client_addr);
            }
        }

        if (reload) {
            reload = 0;
            log_message(LOG_INFO, "Reloading configuration...");

            char* new_port_str = get_ini_value("server.conf", "server",
                                               "port", "8080");
            uint16_t new_port = (uint16_t)atoi(new_port_str);
            free(new_port_str);

            char* new_doc_root = get_ini_value("server.conf", "server",
                                               "document_root", "./www");
            free((void*)document_root);
            document_root = new_doc_root;

            char* new_threads_str = get_ini_value("server.conf", "server",
                                                  "thread_count", "4");
            int new_thread_count = atoi(new_threads_str);
            free(new_threads_str);

            char* new_log_level = get_ini_value("server.conf", "logging",
                                                "level", "INFO");
            if (strcasecmp(new_log_level, "ERROR") == 0)
                current_log_level = LOG_ERROR;
            else
                current_log_level = LOG_INFO;
            free(new_log_level);

            log_message(LOG_INFO,
                "Reload complete: port=%u, document_root=%s, threads=%d, log_level=%s",
                new_port, document_root, new_thread_count,
                (current_log_level == LOG_INFO ? "INFO" : "ERROR"));

            if (new_port != port)
                log_message(LOG_INFO,
                    "Port changed to %u - requires restart", new_port);
            if (new_thread_count != thread_count)
                log_message(LOG_INFO,
                    "Thread count changed to %d - requires restart", new_thread_count);
        }

        static time_t last_print = 0;
        time_t now = time(NULL);
        if (now - last_print >= 10) {
            metrics_print_summary();
            last_print = now;
        }
    }

    log_message(LOG_INFO, "Shutting down server...");
    metrics_print_summary();

    close(epoll_fd);
    cache_destroy();
    thread_pool_shutdown();
    close(server_fd);

    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }

    free((void*)document_root);
    log_message(LOG_INFO, "Server closed.");
    return EXIT_SUCCESS;
}