#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>

#include <errno.h>
#include <signal.h>
#include <pthread.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "client_registry.h"
#include "exchange.h"
#include "account.h"
#include "trader.h"
#include "debug.h"
#include "server.h"

extern EXCHANGE *exchange;
extern CLIENT_REGISTRY *client_registry;

volatile sig_atomic_t sighup_flag = 0;

static void terminate(int status);

void sighup_handler(int sig) {
    sighup_flag = 1;
}

/*
 * "Bourse" exchange server.
 *
 * Usage: bourse <port>
 */
int main(int argc, char* argv[]) {
    // Signal Handling Installation
    struct sigaction sa = {0};
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = sighup_handler;
    sigaction(SIGHUP, &sa, NULL);

    // Option processing should be performed here.
    // Option '-p <port>' is required in order to specify the port number
    // on which the server should listen.
    int port = -1;
    bool pflag = false;
    int c;

    while ((c = getopt(argc, argv, ":p:")) != -1) {
        switch (c) {
        case 'p':
            pflag = true;
            port = atoi(optarg);
            break;
        }
    }

    // -p is required
    if (!pflag) {
        fprintf(stderr, "Usage: %s -p <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Computer ports range from 0-65535
    if (port < 0 || port > 65535) {
        fprintf(stderr, "Invalid port number.\n");
        exit(EXIT_FAILURE);
    }

    // Perform required initializations of the client_registry,
    // maze, and player modules.
    client_registry = creg_init();
    accounts_init();
    traders_init();
    exchange = exchange_init();

    // TODO: Set up the server socket and enter a loop to accept connections
    // on this socket.  For each connection, a thread should be started to
    // run function brs_client_service().  In addition, you should install
    // a SIGHUP handler, so that receipt of SIGHUP will perform a clean
    // shutdown of the server.

    int listenfd, connfd;
    int opt = 1;
    struct sockaddr_in addr = {0};
    socklen_t addrlen = sizeof(addr);

    // Creating the socket and assigning a fd
    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "Failed to create server socket.\n");
        terminate(EXIT_FAILURE);
    }

    // Forcefully attaching the socket to the specified port
    if ((setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) < 0) {
        fprintf(stderr, "Failed to attach socket to port.\n");
        terminate(EXIT_FAILURE);
    }
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    // Bind socket to specified IP and port
    if ((bind(listenfd, (struct sockaddr*) &addr, sizeof(addr))) < 0) {
        fprintf(stderr, "Failed to tie socket to the specified port.\n");
        terminate(EXIT_FAILURE);
    }

    // Prepare the server socket to accept incoming connections
    if ((listen(listenfd, SOMAXCONN)) < 0) {
        fprintf(stderr, "Failed to set up listening socket.\n");
        terminate(EXIT_FAILURE);
    }

    while (!sighup_flag) {
        if ((connfd = accept(listenfd, (struct sockaddr*)&addr, &addrlen)) < 0) {
            fprintf(stderr, "Something went wrong when setting up a client socket.\n");
            continue;
        }

        int *connptr = malloc(sizeof(int));
        *connptr = connfd;

        pthread_t tid;
        if (pthread_create(&tid, NULL, brs_client_service, connptr) < 0) {
            close(connfd);
            free(connptr);
            fprintf(stderr, "Failed to create thread.\n");
            continue;
        }
    }

    close(listenfd);
    terminate(EXIT_SUCCESS);
}

/*
 * Function called to cleanly shut down the server.
 */
static void terminate(int status) {
    // Shutdown all client connections.
    // This will trigger the eventual termination of service threads.
    creg_shutdown_all(client_registry);
    
    debug("Waiting for service threads to terminate...");
    creg_wait_for_empty(client_registry);
    debug("All service threads terminated.");

    // Finalize modules.
    creg_fini(client_registry);
    exchange_fini(exchange);
    traders_fini();
    accounts_fini();

    debug("Bourse server terminating");
    exit(status);
}