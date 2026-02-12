#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>      // close, read, write, pause...
#include <errno.h>       // errno, perror
#include <signal.h>      // signal, sigaction
#include <sys/socket.h>  // socket, bind, listen...
#include <netinet/in.h>  // sockaddr_in, htons...
#include <arpa/inet.h>   // inet_ntoa (for debugging later)
#include <sys/epoll.h>
#include <pthread.h>

#define DEFAULT_PORT 8080
#define BACKLOG 128
#define MAX_EVENTS 64
#define SERVER_NAME "CServer/0.1"
#define THREAD_COUNT 4 //worker threads

volatile sig_atomic_t running = 1;

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

static thread_pool_t pool = {0};

static void* worker_thread(void *arg) {
    (void)arg;

    while (1) {
        task_t *task = NULL;

        pthread_mutex_lock(&pool.queue_mutex);
        while (pool.queue_head == NULL && !pool.shutdown) {
            pthread_cond_wait(&pool.queue_cond, &pool.queue_mutex);
        }

        if (pool.shutdown) {
            pthread_mutex_unlock(&pool.queue_mutex);
            return NULL;
        }

        //takes the task from the queue head
        task = pool.queue_head;
        pool.queue_head = task->next;
        if (pool.queue_head == NULL) {
            pool.queue_tail = NULL;
        }

        pthread_mutex_unlock(&pool.queue_mutex);

        //Task processing
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &task->client_addr.sin_addr, client_ip, sizeof(client_ip));
        printf("[Worker] Processing connection from %s:%d (fd=%d)\n", client_ip, ntohs(task->client_addr.sin_port), task->client_fd);

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

                // Umesto close() → šaljemo u thread pool
                enqueue_task(client_fd, &client_addr);
            }
        }
    }

    printf("\nShutting down...\n");
    close(epoll_fd);
    thread_pool_shutdown();
    close(server_fd);
    printf("Server closed.\n");

    return EXIT_SUCCESS;
}