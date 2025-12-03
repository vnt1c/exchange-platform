#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

#include "account.h"
#include "protocol.h"

struct account {
    char *user;
    funds_t balance;
    quantity_t inventory;
    pthread_mutex_t mutex;
};

static pthread_mutex_t global_mutex;
static ACCOUNT *account_arr[MAX_ACCOUNTS];
static int curr_index = 0;

int accounts_init() {
    // pthread_mutex_init returns non-zero on failure
    if (pthread_mutex_init(&global_mutex, NULL)) { 
        return -1;
    }
    return 0;
}

void accounts_fini() {
    pthread_mutex_lock(&global_mutex);
    for (int i = 0; i < MAX_ACCOUNTS; i++) {
        if (account_arr[i]) {
            pthread_mutex_destroy(&(account_arr[i]->mutex));
            free(account_arr[i]->user);
            free(account_arr[i]);
        }
    }
    pthread_mutex_unlock(&global_mutex);
    pthread_mutex_destroy(&global_mutex);
}

ACCOUNT *account_lookup(char *name) {
    pthread_mutex_lock(&global_mutex);
    // Search for existing account
    for (int i = 0; i < MAX_ACCOUNTS; i++) {
        if (account_arr[i]) {
            // Account exists, is it the user we are looking for?
            if (strcmp(name, account_arr[i]->user) == 0) {
                pthread_mutex_unlock(&global_mutex);
                return account_arr[i];
            }
        }
    }

    // Max # of accounts reached
    if (curr_index >= MAX_ACCOUNTS) {
        pthread_mutex_unlock(&global_mutex);
        return NULL;
    }

    ACCOUNT *new_acc = malloc(sizeof(struct account));
    if (!new_acc) {
        pthread_mutex_unlock(&global_mutex);
        return NULL;
    }
    new_acc->user = strdup(name);       // strdup allocates memory
    if (!new_acc->user) {
        free(new_acc);
        pthread_mutex_unlock(&global_mutex);
        return NULL;
    }
    new_acc->balance = 0;
    new_acc->inventory = 0;
    pthread_mutex_init(&new_acc->mutex, NULL);

    account_arr[curr_index++] = new_acc;
    pthread_mutex_unlock(&global_mutex);
    return new_acc;
}

void account_increase_balance(ACCOUNT *account, funds_t amount) {
    pthread_mutex_lock(&account->mutex);
    account->balance += amount;
    pthread_mutex_unlock(&account->mutex);
}

int account_decrease_balance(ACCOUNT *account, funds_t amount) {
    pthread_mutex_lock(&account->mutex);
    if (account->balance < amount) {
        pthread_mutex_unlock(&account->mutex);
        return -1;
    }
    account->balance -= amount;
    pthread_mutex_unlock(&account->mutex);
    return 0;
}

void account_increase_inventory(ACCOUNT *account, quantity_t quantity) {
    pthread_mutex_lock(&account->mutex);
    account->inventory += quantity;
    pthread_mutex_unlock(&account->mutex);
}

int account_decrease_inventory(ACCOUNT *account, quantity_t quantity) {
    pthread_mutex_lock(&account->mutex);
    if (account->inventory < quantity) {
        pthread_mutex_unlock(&account->mutex);
        return -1;
    }
    account->inventory -= quantity;
    pthread_mutex_unlock(&account->mutex);
    return 0;
}

void account_get_status(ACCOUNT *account, BRS_STATUS_INFO *infop) {
    pthread_mutex_lock(&account->mutex);
    infop->balance = htonl(account->balance);
    infop->inventory = htonl(account->inventory);
    pthread_mutex_unlock(&account->mutex);
}