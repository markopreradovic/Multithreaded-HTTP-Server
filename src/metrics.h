#ifndef METRICS_H
#define METRICS_H

#include "common.h"

void metrics_increment_request(void);
void metrics_increment_cache_hit(void);
void metrics_increment_cache_miss(void);
void metrics_increment_success(void);
void metrics_increment_failure(void);
void metrics_record_latency(uint64_t start_ns);
void metrics_print_summary(void);
void send_metrics_json(int client_fd, bool keep_alive);

#endif // METRICS_H