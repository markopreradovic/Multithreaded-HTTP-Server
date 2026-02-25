#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include "common.h"

typedef struct task {
    int                client_fd;
    struct sockaddr_in client_addr;
    struct task*       next;
} task_t;

typedef struct {
    pthread_t*      threads;
    int             thread_count;
    task_t*         queue_head;
    task_t*         queue_tail;
    pthread_mutex_t queue_mutex;
    pthread_cond_t  queue_cond;
    volatile int    shutdown;
} thread_pool_t;

extern thread_pool_t pool;

int  thread_pool_init(int num_threads);
void thread_pool_shutdown(void);
void enqueue_task(int client_fd, struct sockaddr_in* client_addr);

#endif // THREAD_POOL_H