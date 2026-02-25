#include "cache.h"

lru_cache_t cache = {0};

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
    munmap(node->mapped_data, node->mapped_size);
    close(node->fd);
    free(node);

    printf("[Cache] Evicted LRU entry\n");
}

bool cache_get_or_load(const char* filepath, int* fd_out,
                       void** mapped_out, size_t* size_out) {
    pthread_mutex_lock(&cache.mutex);

    cache_node_t* node = cache.head;
    while (node) {
        if (strcmp(node->filepath, filepath) == 0) {
            cache_move_to_head(node);
            *fd_out     = node->fd;
            *mapped_out = node->mapped_data;
            *size_out   = node->mapped_size;
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

    while (cache.count >= CACHE_MAX_ITEMS ||
           cache.total_size + file_size > CACHE_MAX_SIZE) {
        cache_evict_lru();
    }

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

    *fd_out     = fd;
    *mapped_out = mapped;
    *size_out   = file_size;
    return true;
}

void cache_init(void) {
    pthread_mutex_init(&cache.mutex, NULL);
    cache.head       = cache.tail = NULL;
    cache.count      = 0;
    cache.total_size = 0;
    printf("[MMAP Cache] Initialized (max items: %d, max size: %lu MB)\n",
           CACHE_MAX_ITEMS, (unsigned long)(CACHE_MAX_SIZE / (1024 * 1024)));
}

void cache_destroy(void) {
    pthread_mutex_lock(&cache.mutex);

    cache_node_t* current = cache.head;
    while (current) {
        cache_node_t* next = current->next;
        if (current->mapped_data != MAP_FAILED && current->mapped_data != NULL)
            munmap(current->mapped_data, current->mapped_size);
        if (current->fd >= 0) close(current->fd);
        free(current->filepath);
        free(current);
        current = next;
    }

    cache.head = cache.tail = NULL;
    cache.count      = 0;
    cache.total_size = 0;

    pthread_mutex_unlock(&cache.mutex);
    pthread_mutex_destroy(&cache.mutex);
    printf("[MMAP Cache] Destroyed\n");
}