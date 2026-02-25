#ifndef SERVER_H
#define SERVER_H

#include "common.h"
#include "http.h"

extern volatile sig_atomic_t running;
extern volatile sig_atomic_t reload;

void signal_handler(int sig);
int  create_and_bind_socket(uint16_t port);
int  serve_file(int client_fd, const char* filepath);
int  execute_cgi(int client_fd, const char* script_path,
                 const http_request_t* req);

#endif // SERVER_H