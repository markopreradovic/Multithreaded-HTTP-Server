#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>      // close, read, write, pause...
#include <errno.h>       // errno, perror
#include <signal.h>      // signal, sigaction
#include <sys/socket.h>  // socket, bind, listen...
#include <netinet/in.h>  // sockaddr_in, htons...
#include <arpa/inet.h>   // inet_ntoa (for debugging later)
#include <sys/epoll.h>

#define DEFAULT_PORT 8080
#define BACKLOG 128
#define MAX_EVENTS 64
#define SERVER_NAME "CServer/0.1"

volatile sig_atomic_t running = 1;

static void signal_handler(int sig) {
    (void) sig;
    running = 0;
}

static int create_and_bind_socket(uint16_t port) {
    //Creating TCP socket (IPv4)
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket failed");
        return -1;
    }

    //Enabling the port to be used right after shutdown (without it there will be 3 to 4 minutes wait time)
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        close(server_fd);
        return -1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;          //IPv4
    addr.sin_addr.s_addr = INADDR_ANY;  // 0.0.0.0 - All network interfaces
    addr.sin_port = htons(port);        // Converting port to network byte order

    // Connecting socket with an address and port
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        close(server_fd);
        return -1;
    }

    //Listening to upcoming connections (BACKLOG = how many connections can wait in queue before being refused)
    if(listen(server_fd, BACKLOG) < 0) {
        perror("listen failed");
        close(server_fd);
        return -1;
    }
    printf("Server starting and listening on port %u\n", port);
    return server_fd;
}

int main(void) {
    //Handlers.
    signal(SIGINT, signal_handler); //Ctrl C
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN); //useful for writing on an closed socket

    //Socket binding.
    int server_fd = create_and_bind_socket(DEFAULT_PORT);
    if (server_fd < 0) {
        fprintf(stderr, "Cannot start server\n");
        return EXIT_FAILURE;
    }

    //EPOLL init
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1 failed");
        close(server_fd);
        return EXIT_FAILURE;
    }

    //adding server socket to epoll
    struct epoll_event ev = {0};
    ev.events = EPOLLIN; //readable = new con
    ev.data.fd = server_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
        perror("epoll_ctl ADD server_fd failed");
        close(epoll_fd);
        close(server_fd);
        return EXIT_FAILURE;
    }

    struct epoll_event events[MAX_EVENTS];

    printf("Server running. Ctrl + C to shut down.");
    fflush(stdout);
    while (running) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds<0) {
            if (errno == EINTR && !running) {
                break; // signal interrupted wait
            }
            perror("epoll_wait failed");
            continue;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == server_fd) {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);

                int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
                if (client_fd < 0) {
                    perror("accept failed");
                    continue;
                }

                char client_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
                printf("New connection from %s:%d  (fd=%d)\n", client_ip, ntohs(client_addr.sin_port), client_fd);

                close(client_fd);
            }
        }
    }

    //Cleanup.
    printf("Shutdown signal received...\n");
    close(epoll_fd);
    close(server_fd);
    printf("Server closed.\n");
    return EXIT_SUCCESS;
}