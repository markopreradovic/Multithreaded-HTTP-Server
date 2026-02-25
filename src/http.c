#include "http.h"

void free_request(http_request_t* req) {
    if (req->body) free(req->body);
    memset(req, 0, sizeof(*req));
}

int parse_request(int client_fd, http_request_t* req) {
    char buffer[READ_BUFFER_SIZE];
    ssize_t total_read = 0;
    ssize_t n;

    memset(req, 0, sizeof(*req));

    while (total_read < (ssize_t)(sizeof(buffer) - 1)) {
        n = recv(client_fd, buffer + total_read,
                 sizeof(buffer) - total_read - 1, 0);
        if (n <= 0) {
            if (n == 0) printf("[Parser] Connection closed by client\n");
            else        perror("[Parser] recv");
            return -1;
        }

        total_read += n;
        buffer[total_read] = '\0';

        if (total_read > MAX_HEADER_SIZE) {
            printf("[Parser] Headers too large (> %d bytes)\n", MAX_HEADER_SIZE);
            return -1;
        }

        if (strstr(buffer, "\r\n\r\n")) break;
    }

    if (total_read == 0) return -1;

    char* line = strtok(buffer, "\r\n");
    if (!line) return -1;

    if (sscanf(line, "%15s %1023s %15s",
               req->method, req->uri, req->version) != 3) {
        printf("[Parser] Invalid request line: %s\n", line);
        return -1;
    }

    while ((line = strtok(NULL, "\r\n")) != NULL && strlen(line) > 0) {
        if (req->header_count >= MAX_HEADERS) {
            printf("[Parser] Too many headers (> %d)\n", MAX_HEADERS);
            return -1;
        }

        strncpy(req->headers[req->header_count], line, MAX_HEADER_LEN - 1);
        req->headers[req->header_count][MAX_HEADER_LEN - 1] = '\0';
        req->header_count++;

        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            req->content_length = strtoul(line + 15, NULL, 10);
            if (req->content_length > MAX_BODY_SIZE) {
                printf("[Parser] Body too large (> %d bytes)\n", MAX_BODY_SIZE);
                return -1;
            }
        }
    }

    return 0;
}

void print_request(const http_request_t* req) {
    printf("[Worker] Parsed request:\n");
    printf("  Method: %s\n",    req->method);
    printf("  URI:    %s\n",    req->uri);
    printf("  Version:%s\n",    req->version);
    printf("  Headers (%d):\n", req->header_count);

    for (int i = 0; i < req->header_count; i++)
        printf("    %s\n", req->headers[i]);

    if (req->content_length > 0)
        printf("  Body expected: %zu bytes\n", req->content_length);

    fflush(stdout);
}

void send_response(int client_fd, int status, const char* body,
                   size_t body_len, const char* content_type) {
    char header[1024];
    const char* status_text = (status == 200) ? "OK" :
                              (status == 404) ? "Not Found" :
                              "Bad Request";

    snprintf(header, sizeof(header),
             "HTTP/1.1 %d %s\r\n"
             "Server: %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n",
             status, status_text,
             SERVER_NAME,
             content_type,
             body_len);

    send(client_fd, header, strlen(header), 0);
    if (body && body_len > 0) send(client_fd, body, body_len, 0);
}

void send_404(int client_fd) {
    const char* body =
        "<!DOCTYPE html>\n"
        "<html><head><title>404 Not Found</title></head>\n"
        "<body><h1>404 Not Found</h1>"
        "<p>The requested resource was not found on this server.</p></body></html>";

    send_response(client_fd, 404, body, strlen(body), "text/html; charset=utf-8");
}

const char* get_content_type(const char* path) {
    const char* ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";

    if (strcasecmp(ext, ".html") == 0 || strcasecmp(ext, ".htm") == 0)
        return "text/html; charset=utf-8";
    if (strcasecmp(ext, ".css")  == 0) return "text/css; charset=utf-8";
    if (strcasecmp(ext, ".txt")  == 0) return "text/plain; charset=utf-8";
    if (strcasecmp(ext, ".jpg")  == 0 || strcasecmp(ext, ".jpeg") == 0)
        return "image/jpeg";
    if (strcasecmp(ext, ".png")  == 0) return "image/png";
    if (strcasecmp(ext, ".gif")  == 0) return "image/gif";

    return "application/octet-stream";
}

bool is_keep_alive(const http_request_t* req) {
    for (int i = 0; i < req->header_count; i++) {
        if (strncasecmp(req->headers[i], "Connection:", 11) == 0) {
            const char* val = req->headers[i] + 11;
            while (*val && isspace(*val)) val++;
            if (strncasecmp(val, "keep-alive", 10) == 0) return true;
        }
    }
    return false;
}