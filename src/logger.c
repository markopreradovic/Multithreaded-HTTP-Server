#include "logger.h"

log_level_t current_log_level = LOG_INFO;
FILE*       log_file           = NULL;
const char* log_file_path      = "server.log";

void log_message(log_level_t level, const char* format, ...) {
    if (level < current_log_level) return;

    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    const char* level_str = (level == LOG_INFO) ? "INFO" : "ERROR";

    va_list args;
    va_start(args, format);

    fprintf(stderr, "[%s] %s: ", timestamp, level_str);
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    fflush(stderr);

    if (log_file) {
        fprintf(log_file, "[%s] %s: ", timestamp, level_str);
        vfprintf(log_file, format, args);
        fprintf(log_file, "\n");
        fflush(log_file);
    }

    va_end(args);
}

void log_access(const char* client_ip, uint16_t client_port,
                const char* method, const char* uri,
                int status, long duration_us) {
    time_t now = time(NULL);
    struct tm tm_info;
    char timebuf[32];
    localtime_r(&now, &tm_info);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm_info);

    printf("[ACCESS] %s %s:%u \"%s %s\" %d (%ld us)\n",
           timebuf, client_ip, client_port, method, uri, status, duration_us);
    fflush(stdout);
}