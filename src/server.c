#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

#include "server.h"
#include "protocol.h"
#include "trader.h"

/* I am writing ACK and NACK functions since the trader 
   versions require a trader is initialized...          */

/*
 * Send an ACK packet to a specific fd
 * 
 * @param fd  The fd to send the ACK packet to
 * @return  0 if successfully sent and -1 if unsuccessful
 */
static int send_ack(int fd) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
        perror("clock_gettime");
        return -1;
    }

    BRS_PACKET_HEADER ack = {
        .type = BRS_ACK_PKT,
        .size = 0,
        .timestamp_sec  = htonl((uint32_t)ts.tv_sec),
        .timestamp_nsec = htonl((uint32_t)ts.tv_nsec)
    };

    if (proto_send_packet(fd, &ack, NULL) == -1) {
        return -1;
    }

    return 0;
}

/*
 * Send a NACK packet to a specific fd
 * 
 * @param fd  The fd to send the NACK packet to
 * @return  0 if successfully sent and -1 if unsuccessful
 */
static int send_nack(int fd) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
        perror("clock_gettime");
        return -1;
    }

    BRS_PACKET_HEADER nack = {
        .type = BRS_NACK_PKT,
        .size = 0,
        .timestamp_sec  = htonl((uint32_t)ts.tv_sec),
        .timestamp_nsec = htonl((uint32_t)ts.tv_nsec)
    };

    if (proto_send_packet(fd, &nack, NULL) == -1) {
        return -1;
    }

    return 0;
}

void *brs_client_service(void *arg) {
    int fd = *((int*) arg);
    free(arg);

    pthread_detach(pthread_self());
    creg_register(client_registry, fd);

    // Variables
    TRADER *trader = NULL;
    BRS_PACKET_HEADER hdr;
    void *payload = NULL;

    for (;;) {
        // read packet and read header, the SWITCH
        if (proto_recv_packet(fd, &hdr, &payload) == -1) {
            break;
        }

        if (!trader && hdr.type != BRS_LOGIN_PKT) {
            send_nack(fd);
            free(payload);
            payload = NULL;
            continue;
        }

        switch (hdr.type) {
        case BRS_LOGIN_PKT: {
            // If a trader already exists, we simply send NACK
            if (trader) {
                trader_send_nack(trader);
                break;
            }

            size_t len = ntohs(hdr.size);
            char *name = malloc(len+1);
            if (!name) {
                send_nack(fd);
                break;
            }

            memcpy(name, payload, len);
            name[len] = '\0';
            trader = trader_login(fd, name);
            free(name);

            if (trader == NULL) {
                send_nack(fd);
                break;
            }

            trader_send_ack(trader, NULL);
            break;
        }
        case BRS_STATUS_PKT: {
            ACCOUNT *acc = trader_get_account(trader);
            BRS_STATUS_INFO info = {0};
            exchange_get_status(exchange, acc, &info);

            trader_send_ack(trader, &info);
            break;
        }
        case BRS_DEPOSIT_PKT: {
            ACCOUNT *acc = trader_get_account(trader);
            BRS_FUNDS_INFO *deposit = payload;
            funds_t amount = ntohl(deposit->amount);
            
            account_increase_balance(acc, amount);

            BRS_STATUS_INFO info = {0};
            exchange_get_status(exchange, acc, &info);
            trader_send_ack(trader, &info);
            break;
        } 
        case BRS_WITHDRAW_PKT: {
            ACCOUNT *acc = trader_get_account(trader);
            BRS_FUNDS_INFO *withdraw = payload;
            funds_t amount = ntohl(withdraw->amount);
            
            if (account_decrease_balance(acc, amount) == -1) {
                trader_send_nack(trader);
                break;
            }

            BRS_STATUS_INFO info = {0};
            exchange_get_status(exchange, acc, &info);
            trader_send_ack(trader, &info);
            break;
        }
        case BRS_ESCROW_PKT: {
            ACCOUNT *acc = trader_get_account(trader);
            BRS_ESCROW_INFO *escrow = payload;
            quantity_t quantity = ntohl(escrow->quantity);

            account_increase_inventory(acc, quantity);

            BRS_STATUS_INFO info = {0};
            exchange_get_status(exchange, acc, &info);
            trader_send_ack(trader, &info);
            break;
        }
        case BRS_RELEASE_PKT: {
            ACCOUNT *acc = trader_get_account(trader);
            BRS_ESCROW_INFO *release = payload;
            quantity_t quantity = ntohl(release->quantity);

            if (account_decrease_inventory(acc, quantity) == -1) {
                trader_send_nack(trader);
                break;
            }

            BRS_STATUS_INFO info = {0};
            exchange_get_status(exchange, acc, &info);
            trader_send_ack(trader, &info);
            break;
        }
        case BRS_BUY_PKT: {
            ACCOUNT *acc = trader_get_account(trader);
            BRS_ORDER_INFO *order = payload;
            quantity_t order_quantity = ntohl(order->quantity);
            funds_t order_price = ntohl(order->price);
            orderid_t order_id;

            if ((order_id = exchange_post_buy(exchange, trader, order_quantity, order_price)) == 0) {
                trader_send_nack(trader);
                break;
            }

            BRS_STATUS_INFO info = {0};
            exchange_get_status(exchange, acc, &info);
            info.orderid = htonl(order_id);
            trader_send_ack(trader, &info);
            break;
        }
        case BRS_SELL_PKT: {
            ACCOUNT *acc = trader_get_account(trader);
            BRS_ORDER_INFO *order = payload;
            quantity_t order_quantity = ntohl(order->quantity);
            funds_t order_price = ntohl(order->price);
            orderid_t order_id;

            if ((order_id = exchange_post_sell(exchange, trader, order_quantity, order_price)) == 0) {
                trader_send_nack(trader);
                break;
            }

            BRS_STATUS_INFO info = {0};
            exchange_get_status(exchange, acc, &info);
            info.orderid = htonl(order_id);
            trader_send_ack(trader, &info);
            break;
        }
        case BRS_CANCEL_PKT: {
            ACCOUNT *acc = trader_get_account(trader);
            BRS_CANCEL_INFO *cancel = payload;
            orderid_t order_id = ntohl(cancel->order);
            quantity_t canceled_qty;

            if (exchange_cancel(exchange, trader, order_id, &canceled_qty) == -1) {
                trader_send_nack(trader);
                break;
            }

            BRS_STATUS_INFO info = {0};
            exchange_get_status(exchange, acc, &info);
            info.orderid = htonl(order_id);
            info.quantity = htonl(canceled_qty);
            trader_send_ack(trader, &info);            
            break;
        }
        }

        if (payload) {
            free(payload);
            payload = NULL;
        }
    }

    if (trader) {
        trader_logout(trader);
    }

    if (payload) {
        free(payload);
    }

    creg_unregister(client_registry, fd);
    close(fd);

    return NULL;        // server.h says brs_client_service returns NULL
}