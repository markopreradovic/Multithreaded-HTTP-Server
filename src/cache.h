#ifndef CACHE_H
#define CACHE_H

#include "common.h"

typedef struct cache_node {
    char*              filepath;
    int                fd;
    void*              mapped_data;
    size_t             mapped_size;
    time_t             last_access;
    struct cache_node* prev;
    struct cache_node* next;
} cache_node_t;

typedef struct {
    cache_node_t*   head;
    cache_node_t*   tail;
    int             count;
    size_t          total_size;
    pthread_mutex_t mutex;
} lru_cache_t;

extern lru_cache_t cache;

void cache_init(void);
void cache_destroy(void);
bool cache_get_or_load(const char* filepath, int* fd_out,
                       void** mapped_out, size_t* size_out);

#endif // CACHE_H