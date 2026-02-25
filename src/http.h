#ifndef HTTP_H
#define HTTP_H

#include "common.h"

typedef struct {
    char   method[16];
    char   uri[1024];
    char   version[16];
    char   headers[MAX_HEADER_COUNT][MAX_HEADER_LEN];
    int    header_count;
    size_t content_length;
    char*  body;
} http_request_t;

void free_request(http_request_t* req);
int  parse_request(int client_fd, http_request_t* req);
void print_request(const http_request_t* req);
void send_response(int client_fd, int status, const char* body,
                   size_t body_len, const char* content_type);
void send_404(int client_fd);
const char* get_content_type(const char* path);
bool is_keep_alive(const http_request_t* req);

#endif // HTTP_H