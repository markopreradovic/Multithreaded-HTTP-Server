#include "metrics.h"

static atomic_uint_fast64_t total_requests       = 0;
static atomic_uint_fast64_t cache_hits           = 0;
static atomic_uint_fast64_t cache_misses         = 0;
static atomic_uint_fast64_t successful_responses = 0;
static atomic_uint_fast64_t failed_responses     = 0;
static atomic_uint_fast64_t latency_buckets[LATENCY_BUCKETS] = {0};
static atomic_uint_fast64_t latency_sum_us       = 0;
static atomic_uint_fast64_t latency_count        = 0;

void metrics_increment_request(void)   { atomic_fetch_add(&total_requests,       1); }
void metrics_increment_cache_hit(void) { atomic_fetch_add(&cache_hits,           1); }
void metrics_increment_cache_miss(void){ atomic_fetch_add(&cache_misses,         1); }
void metrics_increment_success(void)   { atomic_fetch_add(&successful_responses, 1); }
void metrics_increment_failure(void)   { atomic_fetch_add(&failed_responses,     1); }

void metrics_record_latency(uint64_t start_ns) {
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    uint64_t end_ns      = end.tv_sec * 1000000000ULL + end.tv_nsec;
    uint64_t duration_ns = end_ns - start_ns;
    uint64_t duration_us = duration_ns / 1000;

    atomic_fetch_add(&latency_sum_us, duration_us);
    atomic_fetch_add(&latency_count, 1);

    int bucket;
    if      (duration_us < 1000)   bucket = 0;
    else if (duration_us < 5000)   bucket = 1;
    else if (duration_us < 10000)  bucket = 2;
    else if (duration_us < 50000)  bucket = 3;
    else if (duration_us < 100000) bucket = 4;
    else                           bucket = 5;

    atomic_fetch_add(&latency_buckets[bucket], 1);
}

void metrics_print_summary(void) {
    uint64_t total    = atomic_load_explicit(&total_requests,       memory_order_relaxed);
    uint64_t hits     = atomic_load_explicit(&cache_hits,           memory_order_relaxed);
    uint64_t misses   = atomic_load_explicit(&cache_misses,         memory_order_relaxed);
    uint64_t success  = atomic_load_explicit(&successful_responses, memory_order_relaxed);
    uint64_t failures = atomic_load_explicit(&failed_responses,     memory_order_relaxed);

    double hit_rate = (hits + misses > 0)
                    ? (double)hits / (hits + misses) * 100.0 : 0.0;

    uint64_t count  = atomic_load_explicit(&latency_count,  memory_order_relaxed);
    uint64_t sum_us = atomic_load_explicit(&latency_sum_us, memory_order_relaxed);
    double avg_latency_ms = count > 0 ? (double)sum_us / count / 1000.0 : 0.0;

    uint64_t cumulative = 0;
    double p50 = 0, p95 = 0, p99 = 0;
    if (count > 0) {
        uint64_t p50_target = count * 50 / 100;
        uint64_t p95_target = count * 95 / 100;
        uint64_t p99_target = count * 99 / 100;

        for (int i = 0; i < LATENCY_BUCKETS; i++) {
            cumulative += atomic_load_explicit(&latency_buckets[i], memory_order_relaxed);
            if (p50 == 0 && cumulative >= p50_target) p50 = i * 10;
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

void send_metrics_json(int client_fd, bool keep_alive) {
    uint64_t total    = atomic_load_explicit(&total_requests,       memory_order_relaxed);
    uint64_t hits     = atomic_load_explicit(&cache_hits,           memory_order_relaxed);
    uint64_t misses   = atomic_load_explicit(&cache_misses,         memory_order_relaxed);
    uint64_t success  = atomic_load_explicit(&successful_responses, memory_order_relaxed);
    uint64_t failures = atomic_load_explicit(&failed_responses,     memory_order_relaxed);

    double hit_rate = (hits + misses > 0)
                    ? (double)hits / (hits + misses) * 100.0 : 0.0;

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
             "Connection: %s\r\n"
             "\r\n",
             SERVER_NAME,
             strlen(json),
             keep_alive ? "keep-alive" : "close");

    send(client_fd, header, strlen(header), 0);
    send(client_fd, json,   strlen(json),   0);
}