#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <stdbool.h>

#include "exchange.h"
#include "trader.h"
#include "account.h"
#include "protocol.h"
#include "debug.h"

#define MAX_ORDERS 4096

struct order {
    TRADER *trader;
    funds_t price;
    quantity_t quantity;
    orderid_t order_id;
};

struct exchange {
    pthread_t match;
    struct order *buy_orders[MAX_ORDERS];
    struct order *sell_orders[MAX_ORDERS];
    orderid_t next_order_id;
    funds_t last_trade_price;
    bool last_trade_set;
    pthread_mutex_t mutex;
    sem_t sem;
};

static funds_t get_price(EXCHANGE *xchg, funds_t sell, funds_t buy) {
    /**
     * 1. No last trade price (this is the first trade) -> return the sell price
     * 2. There is a last trade price
     *      Case A: last price is within [sell min, buy max] -> return last trade price
     *      Case B: else -> return the endpoint (sell min or buy max) that is closest to the last trade price
     */
    if (!xchg->last_trade_set) {
        return sell;
    }

    if (sell <= (xchg->last_trade_price) && (xchg->last_trade_price <= buy)) {
        return xchg->last_trade_price;
    }

    if (xchg->last_trade_price < sell) {
        return sell;
    }

    return buy;
}

static quantity_t get_quantity(quantity_t sell, quantity_t buy) {
    return (sell < buy) ? sell : buy;
}

static void *matchmaker(void *arg) {
    EXCHANGE *xchg = (EXCHANGE *) arg; // the exchange is supposed to be passed into this thread

    for (;;) {
        sem_wait(&xchg->sem);
        
        for (;;) {
            pthread_mutex_lock(&xchg->mutex);
            int i, j;
            bool found = false;

            // First, find a match
            for (i = 0; i < MAX_ORDERS; i++) {
                if (xchg->sell_orders[i]) {
                    for (j = 0; j < MAX_ORDERS; j++) {
                        // Check if the trade meets both party req
                        if (xchg->buy_orders[j] && (xchg->buy_orders[j]->price >= xchg->sell_orders[i]->price)) {
                            found = true;
                            break;
                        }
                    }
                }
                if (found) break;
            }

            if (!found) {
                pthread_mutex_unlock(&xchg->mutex);
                break; // begin waiting again...
            }

            // init vars and get the matched price
            struct order *sell = xchg->sell_orders[i];
            struct order *buy = xchg->buy_orders[j];
            funds_t matched_price = get_price(xchg, sell->price, buy->price);
            quantity_t matched_quantity = get_quantity(sell->quantity, buy->quantity);

            // Conduct the trade
            ACCOUNT *sell_acc = trader_get_account(sell->trader);
            ACCOUNT *buy_acc = trader_get_account(buy->trader);
            account_increase_inventory(buy_acc, matched_quantity);
            account_increase_balance(sell_acc, matched_price * matched_quantity);

            // Refund the buyer
            if (matched_price < buy->price) {
                account_increase_balance(
                    buy_acc,
                    (buy->price - matched_price) * matched_quantity
                );
            }

            // set last trade
            xchg->last_trade_set = true;
            xchg->last_trade_price = matched_price;

            // decrease quantities
            buy->quantity -= matched_quantity;
            sell->quantity -= matched_quantity;

            struct order *free_buy = NULL;
            if (buy->quantity == 0) {
                xchg->buy_orders[j] = NULL;
                free_buy = buy;
            }

            struct order *free_sell = NULL;
            if (sell->quantity == 0) {
                xchg->sell_orders[i] = NULL;
                free_sell = sell;
            }

            // GET TIME FOR HEADERS
            struct timespec ts;
            // `man 2 clock_gettime`
            // CLOCK_MONOTONIC is a system-wide clock (I tested demo_server and it seems to use this)
            if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
                // perror is set in the Linux manual as such
                perror("clock_gettime");
                pthread_mutex_unlock(&xchg->mutex);
                if (free_buy) {
                    trader_unref(free_buy->trader, "order complete");
                    free(free_buy);
                }
                if (free_sell) {
                    trader_unref(free_sell->trader, "order complete");
                    free(free_sell);
                }
                return NULL;
            }

            BRS_NOTIFY_INFO bought_data = {
                .buyer = htonl(buy->order_id),
                .seller = 0,
                .quantity = htonl(matched_quantity),
                .price = htonl(matched_price)
            };

            BRS_PACKET_HEADER bought_hdr = { 
                .type = BRS_BOUGHT_PKT,
                .size = htons(sizeof(BRS_NOTIFY_INFO)),
                .timestamp_sec = htonl((uint32_t)ts.tv_sec),
                .timestamp_nsec = htonl((uint32_t)ts.tv_nsec)
            };

            BRS_NOTIFY_INFO sold_data = {
                .buyer = 0,
                .seller = htonl(sell->order_id),
                .quantity = htonl(matched_quantity),
                .price = htonl(matched_price)
            };

            BRS_PACKET_HEADER sold_hdr = { 
                .type = BRS_SOLD_PKT,
                .size = htons(sizeof(BRS_NOTIFY_INFO)),
                .timestamp_sec = htonl((uint32_t)ts.tv_sec),
                .timestamp_nsec = htonl((uint32_t)ts.tv_nsec)
            };

            BRS_NOTIFY_INFO traded_data = {
                .buyer = htonl(buy->order_id),
                .seller = htonl(sell->order_id),
                .quantity = htonl(matched_quantity),
                .price = htonl(matched_price)
            };

            BRS_PACKET_HEADER traded_hdr = { 
                .type = BRS_TRADED_PKT,
                .size = htons(sizeof(BRS_NOTIFY_INFO)),
                .timestamp_sec = htonl((uint32_t)ts.tv_sec),
                .timestamp_nsec = htonl((uint32_t)ts.tv_nsec)
            };

            pthread_mutex_unlock(&xchg->mutex);

            trader_ref(buy->trader, "matchmaker");
            trader_ref(sell->trader, "matchmaker");

            trader_send_packet(buy->trader, &bought_hdr, &bought_data);
            trader_send_packet(sell->trader, &sold_hdr, &sold_data);
            trader_broadcast_packet(&traded_hdr, &traded_data);

            trader_unref(buy->trader, "matchmaker");
            trader_unref(sell->trader, "matchmaker");

            if (free_buy) {
                trader_unref(free_buy->trader, "order complete");
                free(free_buy);
            }
            if (free_sell) {
                trader_unref(free_sell->trader, "order complete");
                free(free_sell);
            }
        }
    }

    return NULL;
}

EXCHANGE *exchange_init() {
    EXCHANGE *xchg = malloc(sizeof(struct exchange));
    if (!xchg) {
        return NULL;
    }
    xchg->last_trade_price = 0;
    xchg->last_trade_set = false;
    xchg->next_order_id = 1;
    
    memset(xchg->buy_orders, 0, sizeof(xchg->buy_orders));
    memset(xchg->sell_orders, 0, sizeof(xchg->sell_orders));

    if ((sem_init(&xchg->sem, 0, 0)) == -1) {
        free(xchg);
        return NULL;
    }

    if ((pthread_mutex_init(&xchg->mutex, NULL)) != 0) {
        sem_destroy(&xchg->sem);
        free(xchg);
        return NULL;
    }

    if ((pthread_create(&xchg->match, NULL, matchmaker, xchg)) != 0) {
        sem_destroy(&xchg->sem);
        pthread_mutex_destroy(&xchg->mutex);
        free(xchg);
        return NULL;
    }

    return xchg;
}

void exchange_fini(EXCHANGE *xchg) {
    pthread_cancel(xchg->match);
    pthread_join(xchg->match, NULL);    // wait for thread termination

    pthread_mutex_lock(&xchg->mutex);

    for (int i = 0; i < MAX_ORDERS; i++) {
        if (xchg->buy_orders[i]) {
            if (xchg->buy_orders[i]->trader) {
                trader_unref(xchg->buy_orders[i]->trader, "exchange_fini");
            }
            free(xchg->buy_orders[i]);
        }

        if (xchg->sell_orders[i]) {
            if (xchg->sell_orders[i]->trader) {
                trader_unref(xchg->sell_orders[i]->trader, "exchange_fini");
            }
            free(xchg->sell_orders[i]);
        }
    }

    pthread_mutex_unlock(&xchg->mutex);

    sem_destroy(&xchg->sem);
    pthread_mutex_destroy(&xchg->mutex);

    free(xchg);
}

void exchange_get_status(EXCHANGE *xchg, ACCOUNT *account, BRS_STATUS_INFO *infop) {
    pthread_mutex_lock(&xchg->mutex);
    if (account) {
        account_get_status(account, infop);
    } else {
        infop->balance = 0;
        infop->inventory = 0;
    }

    // If there is a last trade price, set that
    if (xchg->last_trade_set) {
        infop->last = htonl(xchg->last_trade_price);
    } else {
        infop->last = 0;
    }

    bool bid_set = false, ask_set = false;
    // Find highest bid / lowest ask
    for (int i = 0; i < MAX_ORDERS; i++) {
        if (xchg->buy_orders[i] != NULL) {
            if (!bid_set) { // bid not set yet
                infop->bid = htonl(xchg->buy_orders[i]->price);
                bid_set = true;
            } 
            else if (ntohl(infop->bid) < xchg->buy_orders[i]->price) { // bid set and we found a higher bid
                infop->bid = htonl(xchg->buy_orders[i]->price);
            }
        }
        if (xchg->sell_orders[i] != NULL) {
            if (!ask_set) { // ask not set yet
                infop->ask = htonl(xchg->sell_orders[i]->price);
                ask_set = true;
            }
            else if (ntohl(infop->ask) > xchg->sell_orders[i]->price) { // ask set and we found a higher ask
                infop->ask = htonl(xchg->sell_orders[i]->price);
            }
        }
    }

    if (!bid_set) {
        infop->bid = 0;
    }
    if (!ask_set) {
        infop->ask = 0;
    }
    pthread_mutex_unlock(&xchg->mutex);
}

orderid_t exchange_post_buy(EXCHANGE *xchg, TRADER *trader, quantity_t quantity, funds_t price) {
    if (quantity == 0) {
        return 0;
    }

    pthread_mutex_lock(&xchg->mutex);
    struct order *ordp = malloc(sizeof(struct order));
    if (!ordp) {
        pthread_mutex_unlock(&xchg->mutex);
        return 0;
    }

    ordp->trader = trader;
    ordp->quantity = quantity;
    ordp->price = price;

    trader_ref(trader, "buy order"); // increase ref count for this order

    ACCOUNT *acc = trader_get_account(trader);
    if ((account_decrease_balance(acc, quantity * price)) == -1) {
        trader_unref(trader, "buy order error");
        free(ordp);
        pthread_mutex_unlock(&xchg->mutex);
        return 0;
    }

    bool posted = false;
    for (int i = 0; i < MAX_ORDERS; i++) {
        if (!xchg->buy_orders[i]) {
            xchg->buy_orders[i] = ordp;
            ordp->order_id = xchg->next_order_id++;     // set order id then increment the xchg var
            posted = true;
            break;
        }
    }

    if (!posted) {
        account_increase_balance(acc, quantity * price);
        trader_unref(trader, "buy order error");
        free(ordp);
        pthread_mutex_unlock(&xchg->mutex);
        return 0;
    }

    orderid_t oid = ordp->order_id;

    pthread_mutex_unlock(&xchg->mutex);

    sem_post(&xchg->sem);

    BRS_NOTIFY_INFO data = {
        .buyer = htonl(oid),
        .seller = 0,
        .quantity = htonl(quantity),
        .price = htonl(price)
    };

    // GET TIME FOR HEADER
    struct timespec ts;
    // `man 2 clock_gettime`
    // CLOCK_MONOTONIC is a system-wide clock (I tested demo_server and it seems to use this)
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
        // perror is set in the Linux manual as such
        perror("clock_gettime");
        return oid;
    }

    BRS_PACKET_HEADER hdr = { 
        .type = BRS_POSTED_PKT,
        .size = htons(sizeof(BRS_NOTIFY_INFO)),
        .timestamp_sec = htonl((uint32_t)ts.tv_sec),
        .timestamp_nsec = htonl((uint32_t)ts.tv_nsec)
    };

    trader_broadcast_packet(&hdr, &data);
    
    return oid;
}

orderid_t exchange_post_sell(EXCHANGE *xchg, TRADER *trader, quantity_t quantity, funds_t price) {
    if (quantity == 0) {
        return 0;
    }

    pthread_mutex_lock(&xchg->mutex);
    struct order *ordp = malloc(sizeof(struct order));
    if (!ordp) {
        pthread_mutex_unlock(&xchg->mutex);
        return 0;
    }

    ordp->trader = trader;
    ordp->quantity = quantity;
    ordp->price = price;

    trader_ref(trader, "sell order"); // increase ref count for this order

    ACCOUNT *acc = trader_get_account(trader);
    if ((account_decrease_inventory(acc, quantity)) == -1) {
        trader_unref(trader, "sell order error");
        free(ordp);
        pthread_mutex_unlock(&xchg->mutex);
        return 0;
    }

    bool posted = false;
    for (int i = 0; i < MAX_ORDERS; i++) {
        if (!xchg->sell_orders[i]) {
            xchg->sell_orders[i] = ordp;
            ordp->order_id = xchg->next_order_id++;
            posted = true;
            break;
        }
    }

    if (!posted) {
        account_increase_inventory(acc, quantity);
        trader_unref(trader, "sell order error");
        free(ordp);
        pthread_mutex_unlock(&xchg->mutex);
        return 0;
    }

    orderid_t oid = ordp->order_id;

    pthread_mutex_unlock(&xchg->mutex);

    sem_post(&xchg->sem);

    BRS_NOTIFY_INFO data = {
        .buyer = 0,
        .seller = htonl(oid),
        .quantity = htonl(quantity),
        .price = htonl(price)
    };

    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
        perror("clock_gettime");
        return oid;
    }

    BRS_PACKET_HEADER hdr = {
        .type = BRS_POSTED_PKT,
        .size = htons(sizeof(BRS_NOTIFY_INFO)),
        .timestamp_sec = htonl((uint32_t)ts.tv_sec),
        .timestamp_nsec = htonl((uint32_t)ts.tv_nsec)
    };

    trader_broadcast_packet(&hdr, &data);

    return oid;
}

int exchange_cancel(EXCHANGE *xchg, TRADER *trader, orderid_t order, quantity_t *quantity) {
    pthread_mutex_lock(&xchg->mutex);

    for (int i = 0; i < MAX_ORDERS; i++) {
        // First, check if we found the order
        if (xchg->buy_orders[i] && (xchg->buy_orders[i]->order_id == order)) {
            struct order *ordp = xchg->buy_orders[i]; // for convenience
            // Is the correct trader trying to cancel the order?
            if (ordp->trader != trader) {
                pthread_mutex_unlock(&xchg->mutex);
                return -1;
            }

            ACCOUNT *acc = trader_get_account(trader);
            // Restore encumbered funds
            account_increase_balance(acc, ordp->price * ordp->quantity);

            BRS_NOTIFY_INFO data = {
                .buyer = htonl(order),
                .seller = 0,
                .quantity = htonl(ordp->quantity),
                .price = htonl(ordp->price)
            };

            // GET TIME FOR HEADER
            struct timespec ts;
            // `man 2 clock_gettime`
            // CLOCK_MONOTONIC is a system-wide clock (I tested demo_server and it seems to use this)
            if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
                pthread_mutex_unlock(&xchg->mutex);
                // perror is set in the Linux manual as such
                perror("clock_gettime");
                return -1;
            }

            BRS_PACKET_HEADER hdr = { 
                .type = BRS_CANCELED_PKT,
                .size = htons(sizeof(BRS_NOTIFY_INFO)),
                .timestamp_sec = htonl((uint32_t)ts.tv_sec),
                .timestamp_nsec = htonl((uint32_t)ts.tv_nsec)
            };

            *quantity = ordp->quantity;
            xchg->buy_orders[i] = NULL;

            pthread_mutex_unlock(&xchg->mutex);

            trader_unref(ordp->trader, "order cancel");
            free(ordp);
            trader_broadcast_packet(&hdr, &data);

            return 0;
        }
        if (xchg->sell_orders[i] && (xchg->sell_orders[i]->order_id == order)) {
            struct order *ordp = xchg->sell_orders[i];

            if (ordp->trader != trader) {
                pthread_mutex_unlock(&xchg->mutex);
                return -1;
            }

            ACCOUNT *acc = trader_get_account(trader);
            account_increase_inventory(acc, ordp->quantity);

            BRS_NOTIFY_INFO data = {
                .buyer = 0,
                .seller = htonl(order),
                .quantity = htonl(ordp->quantity),
                .price = htonl(ordp->price)
            };

            struct timespec ts;
            if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
                pthread_mutex_unlock(&xchg->mutex);
                perror("clock_gettime");
                return -1;
            }

            BRS_PACKET_HEADER hdr = {
                .type = BRS_CANCELED_PKT,
                .size = htons(sizeof(BRS_NOTIFY_INFO)),
                .timestamp_sec = htonl((uint32_t)ts.tv_sec),
                .timestamp_nsec = htonl((uint32_t)ts.tv_nsec)
            };

            *quantity = ordp->quantity;
            xchg->sell_orders[i] = NULL;

            pthread_mutex_unlock(&xchg->mutex);

            trader_unref(ordp->trader, "order cancel");
            free(ordp);
            trader_broadcast_packet(&hdr, &data);

            return 0;
        }
    }

    pthread_mutex_unlock(&xchg->mutex);
    return -1; // order not found
}