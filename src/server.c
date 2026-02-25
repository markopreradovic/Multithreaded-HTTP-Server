#include "server.h"
#include "cache.h"
#include "metrics.h"

volatile sig_atomic_t running = 1;
volatile sig_atomic_t reload  = 0;

void signal_handler(int sig) {
    if (sig == SIGHUP) reload  = 1;
    else               running = 0;
}

int create_and_bind_socket(uint16_t port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket failed");
        return -1;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                   &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        close(server_fd);
        return -1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen failed");
        close(server_fd);
        return -1;
    }

    printf("Server starting and listening on port %u\n", port);
    return server_fd;
}

int serve_file(int client_fd, const char* filepath) {
    metrics_increment_request();

    printf("[serve_file] Trying: %s\n", filepath);

    int    mapped_fd   = -1;
    void*  mapped_ptr  = NULL;
    size_t mapped_size = 0;

    if (cache_get_or_load(filepath, &mapped_fd, &mapped_ptr, &mapped_size)) {
        metrics_increment_cache_hit();

        char header[1024];
        snprintf(header, sizeof(header),
                 "HTTP/1.1 200 OK\r\n"
                 "Server: %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 SERVER_NAME,
                 get_content_type(filepath),
                 mapped_size);

        send(client_fd, header, strlen(header), 0);

        off_t offset = 0;
        while (offset < (off_t)mapped_size) {
            ssize_t sent = sendfile(client_fd, mapped_fd, &offset,
                                    mapped_size - offset);
            if (sent <= 0) {
                if (sent < 0) perror("[serve_file] sendfile cache failed");
                break;
            }
        }
        return 200;
    }

    metrics_increment_cache_miss();

    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        perror("[serve_file] open failed");
        send_404(client_fd);
        return 404;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        send_404(client_fd);
        return 500;
    }

    size_t file_size = st.st_size;

    char header[1024];
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\n"
             "Server: %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n",
             SERVER_NAME,
             get_content_type(filepath),
             file_size);

    send(client_fd, header, strlen(header), 0);

    off_t offset = 0;
    while (offset < (off_t)file_size) {
        ssize_t sent = sendfile(client_fd, fd, &offset, file_size - offset);
        if (sent <= 0) {
            if (sent < 0) perror("[serve_file] sendfile fallback failed");
            break;
        }
    }

    close(fd);
    return 200;
}

int execute_cgi(int client_fd, const char* script_path,
                const http_request_t* req) {
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("[CGI] pipe failed");
        send_response(client_fd, 500,
                      "<h1>500 Internal Server Error</h1>", 35, "text/html");
        return 500;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("[CGI] fork failed");
        close(pipefd[0]);
        close(pipefd[1]);
        send_response(client_fd, 500,
                      "<h1>500 Internal Server Error</h1>", 35, "text/html");
        return 500;
    }

    if (pid == 0) {  // Child
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        char* env[16] = {0};
        int idx = 0;

        env[idx++] = "GATEWAY_INTERFACE=CGI/1.1";
        env[idx++] = "SERVER_NAME=CServer/0.1";
        env[idx++] = "SERVER_PORT=9090";
        env[idx++] = "SCRIPT_NAME=/hello.cgi";

        char method_env[64];
        snprintf(method_env, sizeof(method_env),
                 "REQUEST_METHOD=%s", req->method);
        env[idx++] = method_env;

        char path_env[2048];
        snprintf(path_env, sizeof(path_env), "PATH_INFO=%s", req->uri);
        env[idx++] = path_env;

        char query_env[2048] = "QUERY_STRING=";
        char* q = strchr(req->uri, '?');
        if (q) strcat(query_env, q + 1);
        env[idx++] = query_env;

        env[idx] = NULL;

        char* argv[] = { (char*)script_path, NULL };
        execve(script_path, argv, env);
        perror("[CGI] execve failed");
        exit(1);
    }

    // Parent
    close(pipefd[1]);

    const char* default_header =
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n";
    send(client_fd, default_header, strlen(default_header), 0);

    char buffer[4096];
    ssize_t bytes;
    while ((bytes = read(pipefd[0], buffer, sizeof(buffer))) > 0)
        send(client_fd, buffer, bytes, 0);

    close(pipefd[0]);

    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 200;

    send_response(client_fd, 500,
                  "<h1>500 CGI Script Error</h1>", 30, "text/html");
    return 500;
}