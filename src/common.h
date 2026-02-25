#ifndef COMMON_H
#define COMMON_H

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include <time.h>

#define LATENCY_BUCKETS         10
#define DEFAULT_PORT            8080
#define BACKLOG                 128
#define MAX_EVENTS              64
#define SERVER_NAME             "CServer/0.1"
#define THREAD_COUNT            4
#define MAX_HEADER_COUNT        64
#define MAX_HEADER_LEN          512
#define READ_BUFFER_SIZE        4096
#define CACHE_MAX_ITEMS         100
#define CACHE_MAX_SIZE          (10 * 1024 * 1024)
#define MAX_HEADER_SIZE         (8 * 1024)
#define MAX_HEADERS             64
#define MAX_BODY_SIZE           (1 * 1024 * 1024)
#define CONNECTION_TIMEOUT_SEC  30

#endif // COMMON_H