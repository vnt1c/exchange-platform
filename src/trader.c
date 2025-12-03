#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>

#include "trader.h"
#include "protocol.h"
#include "account.h"
#include "debug.h"

struct trader {
    char *name;
    ACCOUNT *acc;
    int fd;
    int ref_count;
    pthread_mutex_t mutex;
};

static TRADER *log_table[MAX_TRADERS];
static pthread_mutex_t log_mutex;

int traders_init() {
    if (pthread_mutex_init(&log_mutex, NULL) != 0) {
        return -1;
    }
    memset(log_table, 0, sizeof(log_table));
    return 0;
}

void traders_fini() {
    pthread_mutex_lock(&log_mutex);
    for (int i = 0; i < MAX_TRADERS; i++) {
        if (log_table[i]) {
            TRADER *tmp = log_table[i];
            log_table[i] = NULL;
            trader_unref(tmp, "traders_fini");
        }
    }
    pthread_mutex_unlock(&log_mutex);
    pthread_mutex_destroy(&log_mutex);
}

TRADER *trader_login(int fd, char *name) {
    pthread_mutex_lock(&log_mutex);
    int free_slot = -1;
    for (int i = 0; i < MAX_TRADERS; i++) {
        // Account already logged in
        if (log_table[i]) {
            if (strcmp(log_table[i]->name, name) == 0) {
                pthread_mutex_unlock(&log_mutex);
                return NULL;
            }
        }
        // Found a free slot
        if ((free_slot == -1) && !log_table[i]) {
            free_slot = i;
        }
    }

    // No free slots could be found
    if (free_slot == -1) {
        pthread_mutex_unlock(&log_mutex);
        return NULL;
    }

    ACCOUNT *acc = account_lookup(name);
    if (!acc) {
        pthread_mutex_unlock(&log_mutex);
        return NULL;
    }
    TRADER *trader = malloc(sizeof(struct trader));
    if (!trader) {
        pthread_mutex_unlock(&log_mutex);
        return NULL;
    }
    trader->acc = acc;
    trader->fd = fd;
    trader->name = strdup(name);
    if (!trader->name) {
        free(trader);
        pthread_mutex_unlock(&log_mutex);
        return NULL;
    }
    trader->ref_count = 1;

    pthread_mutexattr_t recursiveMutexAttr;
    pthread_mutexattr_init(&recursiveMutexAttr);
    pthread_mutexattr_settype(&recursiveMutexAttr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&trader->mutex, &recursiveMutexAttr);

    log_table[free_slot] = trader;
    pthread_mutex_unlock(&log_mutex);
    return trader;
}

void trader_logout(TRADER *trader) {
    pthread_mutex_lock(&log_mutex);
    int trader_index = -1;
    for (int i = 0; i < MAX_TRADERS; i++) {
        if (log_table[i]) {
            if (strcmp(log_table[i]->name, trader->name) == 0) {
                trader_index = i;
                break;
            }
        }
    }

    // shouldn't happen... caller won't know error since void return value
    // error check anyway
    if (trader_index == -1) {
        pthread_mutex_unlock(&log_mutex);
        return;
    }

    log_table[trader_index] = NULL;
    trader_unref(trader, "logout");

    pthread_mutex_unlock(&log_mutex);
}

TRADER *trader_ref(TRADER *trader, char *why) {
    pthread_mutex_lock(&trader->mutex);
    (trader->ref_count)++;
    pthread_mutex_unlock(&trader->mutex);
    return trader;
}

void trader_unref(TRADER *trader, char *why) {
    pthread_mutex_lock(&trader->mutex);
    // Ref count of trader before decrement == 0: abort
    if (trader->ref_count == 0) {
        pthread_mutex_unlock(&trader->mutex);
        // Don't destroy mutex, immediately abort
        abort();
    }
    (trader->ref_count)--;
    // Ref count of trader after decrement == 0: free resources
    if (trader->ref_count == 0) {
        pthread_mutex_unlock(&trader->mutex);
        pthread_mutex_destroy(&trader->mutex);
        free(trader->name);
        free(trader);
        return;
    }
    pthread_mutex_unlock(&trader->mutex);
}

ACCOUNT *trader_get_account(TRADER *trader) {
    return trader->acc;
}

int trader_send_packet(TRADER *trader, BRS_PACKET_HEADER *pkt, void *data) {
    pthread_mutex_lock(&trader->mutex);
    if ((proto_send_packet(trader->fd, pkt, data)) == -1) {
        pthread_mutex_unlock(&trader->mutex);
        return -1;
    }
    pthread_mutex_unlock(&trader->mutex);
    return 0;
}

static void copy_table(TRADER **log) {
    pthread_mutex_lock(&log_mutex);
    for (int i = 0; i < MAX_TRADERS; i++) {
        if (log_table[i]) {
            log[i] = trader_ref(log_table[i], "broadcast");
        } else {
            log[i] = NULL;
        }
    }
    pthread_mutex_unlock(&log_mutex);
}

int trader_broadcast_packet(BRS_PACKET_HEADER *pkt, void *data) {
    // We can't directly access the log_table entries (or a deadlock will occur)
    TRADER *tmp[MAX_TRADERS];            // let's store pointers in a tmp array
    copy_table(tmp);                     // call helper

    int err = 0;
    for (int i = 0; i < MAX_TRADERS; i++) {
        TRADER *trader = tmp[i];
        if (!trader) {
            continue;
        }
        if (err == -1) {
            trader_unref(trader, "broadcast");
            continue;
        }
        if ((trader_send_packet(trader, pkt, data)) == -1) {
            err = -1;
        }
        trader_unref(trader, "broadcast");
    }
    return err;
}

// functions taken from server.c (same formatting, except trader_send_packet over proto_send_packet) 

int trader_send_ack(TRADER *trader, BRS_STATUS_INFO *info) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
        perror("clock_gettime");
        return -1;
    }

    BRS_PACKET_HEADER ack = {
        .type = BRS_ACK_PKT,
        .size = htons(sizeof(BRS_STATUS_INFO)),
        // Piazza - use time module
        .timestamp_sec  = htonl((uint32_t)ts.tv_sec),
        .timestamp_nsec = htonl((uint32_t)ts.tv_nsec)
    };

    if (!info) {
        ack.size = 0;
    }

    if (trader_send_packet(trader, &ack, info ? info : NULL) == -1) {
        return -1;
    }

    return 0;
}

int trader_send_nack(TRADER *trader) {
   struct timespec ts;

    // `man 2 clock_gettime`
    // CLOCK_MONOTONIC is a system-wide clock (I tested demo_server and it seems to use this)
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
        // perror is set in the Linux manual as such
        perror("clock_gettime");
        return -1;
    }

    // create nack header
    BRS_PACKET_HEADER nack = {
        .type = BRS_NACK_PKT,
        .size = 0,
        // Piazza @225: timestamp fields should be filled out for debugging output
        .timestamp_sec = htonl((uint32_t)ts.tv_sec),
        .timestamp_nsec = htonl((uint32_t)ts.tv_nsec)
    };

    // send the nack packet to the specified fd
    if (trader_send_packet(trader, &nack, NULL) == -1) {
        return -1;
    }

    return 0;
}