#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/socket.h>

#include "client_registry.h"

struct client_registry {
    sem_t sem;
    pthread_mutex_t mutex;
    int fd_list[4096];
    int fd_count;
};

CLIENT_REGISTRY *creg_init() {
    struct client_registry *cr = malloc(sizeof(struct client_registry));

    if (!cr) {
        return NULL;
    }

    if ((sem_init(&cr->sem, 0, 0)) == -1) {
        free(cr);
        return NULL;
    }
    
    if ((pthread_mutex_init(&cr->mutex, NULL)) != 0) {
        sem_destroy(&cr->sem);
        free(cr);
        return NULL;
    }

    memset(cr->fd_list, -1, sizeof(cr->fd_list));
    cr->fd_count = 0;

    return cr;
}

void creg_fini(CLIENT_REGISTRY *cr) {
    sem_destroy(&cr->sem);
    pthread_mutex_destroy(&cr->mutex);

    free(cr);
}

int creg_register(CLIENT_REGISTRY *cr, int fd) {
    pthread_mutex_lock(&cr->mutex);
    for (int i = 0; i < 4096; i++) {
        if (cr->fd_list[i] == -1) {
            cr->fd_list[i] = fd;
            (cr->fd_count)++;

            pthread_mutex_unlock(&cr->mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&cr->mutex);
    return -1;
}

int creg_unregister(CLIENT_REGISTRY *cr, int fd) {
    pthread_mutex_lock(&cr->mutex);
    for (int i = 0; i < 4096; i++) {
        if (cr->fd_list[i] == fd) {
            cr->fd_list[i] = -1;
            (cr->fd_count)--;
            if (cr->fd_count == 0) {
                sem_post(&cr->sem);
            }
            pthread_mutex_unlock(&cr->mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&cr->mutex);
    return -1;
}

void creg_wait_for_empty(CLIENT_REGISTRY *cr) {
    sem_wait(&cr->sem);
}


void creg_shutdown_all(CLIENT_REGISTRY *cr) {
    pthread_mutex_lock(&cr->mutex);
    for (int i = 0; i < 4096; i++) {
        if (cr->fd_list[i] != -1) {
            shutdown(cr->fd_list[i], SHUT_RD);
        }
    }
    pthread_mutex_unlock(&cr->mutex);
}