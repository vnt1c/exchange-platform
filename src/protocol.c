#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>

#include <time.h>

#include "protocol.h"

int proto_send_packet(int fd, BRS_PACKET_HEADER *hdr, void *payload) {
    if (payload == NULL) hdr->size = 0;

    // First, send the header
    size_t bytes_left = sizeof(BRS_PACKET_HEADER);
    size_t offset = 0;

    while (bytes_left > 0) {
        // Start where we left off and continue writing
        ssize_t bytes_sent = write(fd, ((char*) hdr) + offset, bytes_left);
        // Error check (write returns -1 or 0)
        if (bytes_sent <= 0) {
            if (bytes_sent == 0) errno = EPIPE;
            return -1;
        }

        // Update vars
        bytes_left -= bytes_sent;
        offset += bytes_sent;
    }

    bytes_left = ntohs(hdr->size);
    offset = 0;

    if (payload != NULL && bytes_left > 0) {
        while (bytes_left > 0) {
            ssize_t bytes_sent = write(fd, ((char*) payload) + offset, bytes_left);
            if (bytes_sent <= 0) {
                if (bytes_sent == 0) errno = EPIPE;
                return -1;
            }

            bytes_left -= bytes_sent;
            offset += bytes_sent;
        }
    }

    return 0;
}

int proto_recv_packet(int fd, BRS_PACKET_HEADER *hdr, void **payloadp) {
    // 'payloadp' is a pointer to a pointer
    // fd is the file descriptor with incoming data
    // store into hdr and payloadp
    // FIRSTLY, READ THE INCOMING HEADER.

    if (payloadp) *payloadp = NULL;

    size_t bytes_left = sizeof(BRS_PACKET_HEADER);
    size_t offset = 0;

    while (bytes_left > 0) {
        // Start where we left off and continue writing
        ssize_t bytes_read = read(fd, ((char*) hdr) + offset, bytes_left);
        // Error check (read returns -1 or 0)
        if (bytes_read <= 0) {
            return -1;
        }

        // Update vars
        bytes_left -= bytes_read;
        offset += bytes_read;
    }

    size_t payloadp_len = ntohs(hdr->size);     // network -> host byte order
    if (payloadp_len == 0) return 0;            // nothing to read
    
    void *tmp = malloc(payloadp_len);       // allocate mem for temporary buffer
    if (tmp == NULL) return -1;

    bytes_left = payloadp_len;
    offset = 0;

    while (bytes_left > 0) {
        ssize_t bytes_read = read(fd, ((char*) tmp) + offset, bytes_left);
        if (bytes_read <= 0) {
            free(tmp);
            return -1;
        }

        bytes_left -= bytes_read;
        offset += bytes_read;
    }

    if (payloadp) {
        *payloadp = tmp;
    } else {
        free(tmp);
    }

    printf("payload: %p\n", *payloadp);

    return 0;
}