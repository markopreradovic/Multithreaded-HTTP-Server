#ifndef LOGGER_H
#define LOGGER_H

#include "common.h"

typedef enum {
    LOG_INFO,
    LOG_ERROR
} log_level_t;

extern log_level_t current_log_level;
extern FILE*       log_file;
extern const char* log_file_path;

void log_message(log_level_t level, const char* format, ...);

void log_access(const char* client_ip, uint16_t client_port,
                const char* method, const char* uri,
                int status, long duration_us);

#endif // LOGGER_H