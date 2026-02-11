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

#define DEFAULT_PORT 8080
#define BACKLOG 128
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

    //Socket binding.
    int server_fd = create_and_bind_socket(DEFAULT_PORT);
    if (server_fd < 0) {
        fprintf(stderr, "Cannot start server\n");
        return EXIT_FAILURE;
    }

    printf("Server running. Ctrl + C to shut down.");
    while (running) {
        printf("Awaiting connections...\n");
        fflush(stdout);
        int remaining = 5;           // čekamo 5 sekundi ukupno
        while (remaining > 0 && running) {
            sleep(1);
            remaining--;
        }
    }

    //Cleanup.
    printf("Shutdown signal received...\n");
    close(server_fd);
    printf("Server closed.\n");
    return EXIT_SUCCESS;

    //Test push
}