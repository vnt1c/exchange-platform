#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE

#include <criterion/criterion.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <wait.h>

#include "client_registry.h"
#include "protocol.h"
#include "account.h"
#include "trader.h"
#include "exchange.h"

/*
 * ------------------------------------------------------
 * Shared init/fini helpers
 * ------------------------------------------------------
 */

static void init_server() {
#ifndef NO_SERVER
    int ret;
    int i = 0;
    do {
        ret = system("netstat -an | fgrep '0.0.0.0:9999' > /dev/null");
        sleep(1);
    } while (++i < 30 && WEXITSTATUS(ret));
#endif
}

static void fini_server() {
}

void *system_thread(void *arg) {
    long ret = system((char *)arg);
    return (void *)ret;
}

/*
 * ------------------------------------------------------
 * STUDENT SUITE — server startup + basic connectivity
 * ------------------------------------------------------
 */

Test(student_suite, 00_start_server, .timeout = 30) {
    fprintf(stderr, "server_suite/00_start_server\n");

    int server_pid = 0;
    int ret = system("netstat -an | fgrep '0.0.0.0:9999' > /dev/null");
    cr_assert_neq(WEXITSTATUS(ret), 0, "Server was already running");

    fprintf(stderr, "Starting server...\n");
    if ((server_pid = fork()) == 0) {
        execlp("valgrind", "bourse",
               "--leak-check=full", "--track-fds=yes",
               "--error-exitcode=37", "--log-file=valgrind.out",
               "bin/bourse", "-p", "9999", NULL);
        fprintf(stderr, "Failed to exec server\n");
        abort();
    }

    fprintf(stderr, "pid = %d\n", server_pid);
    char *cmd = "sleep 10";

    pthread_t tid;
    pthread_create(&tid, NULL, system_thread, cmd);
    pthread_join(tid, NULL);

    cr_assert_neq(server_pid, 0, "Server was not started");

    fprintf(stderr, "Sending SIGHUP to server pid %d\n", server_pid);
    kill(server_pid, SIGHUP);

    sleep(5);
    kill(server_pid, SIGKILL);

    wait(&ret);
    fprintf(stderr, "Server wait() returned = 0x%x\n", ret);

    if (WIFSIGNALED(ret)) {
        fprintf(stderr, "Server terminated with signal %d\n", WTERMSIG(ret));
        system("cat valgrind.out");
        if (WTERMSIG(ret) == 9)
            cr_assert_fail("Server did not terminate after SIGHUP");
    }

    if (WEXITSTATUS(ret) == 37)
        system("cat valgrind.out");

    cr_assert_neq(WEXITSTATUS(ret), 37, "Valgrind reported errors");
    cr_assert_eq(WEXITSTATUS(ret), 0, "Server exit status was not 0");
}

Test(student_suite, 01_connect, .init = init_server, .fini = fini_server, .timeout = 5) {
    fprintf(stderr, "server_suite/01_connect\n");
    int ret = system("util/client -p 9999 </dev/null | grep 'Connected to server'");
    cr_assert_eq(ret, 0);
}

/*
 * ------------------------------------------------------
 * CLIENT REGISTRY TESTS
 * ------------------------------------------------------
 */

static void *register_and_unregister(void *arg) {
    CLIENT_REGISTRY *cr = arg;
    int fd = (rand() % 30000) + 3;

    creg_register(cr, fd);
    usleep(5000);
    creg_unregister(cr, fd);

    return NULL;
}

Test(student_extra, client_registry_basic, .timeout = 5) {
    CLIENT_REGISTRY *cr = creg_init();
    cr_assert_not_null(cr);

    int fd1 = 10, fd2 = 11;

    cr_assert_eq(creg_register(cr, fd1), 0);
    cr_assert_eq(creg_register(cr, fd2), 0);
    cr_assert_eq(creg_unregister(cr, fd1), 0);
    cr_assert_eq(creg_unregister(cr, fd2), 0);

    creg_fini(cr);
}

Test(student_extra, client_registry_wait_for_empty, .timeout = 5) {
    CLIENT_REGISTRY *cr = creg_init();

    pthread_t t1, t2;
    pthread_create(&t1, NULL, register_and_unregister, cr);
    pthread_create(&t2, NULL, register_and_unregister, cr);

    creg_wait_for_empty(cr);
    cr_assert(1);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    creg_fini(cr);
}

Test(student_extra, client_registry_stress, .timeout = 10) {
    CLIENT_REGISTRY *cr = creg_init();
    pthread_t threads[32];

    for (int i = 0; i < 32; i++)
        pthread_create(&threads[i], NULL, register_and_unregister, cr);

    creg_wait_for_empty(cr);

    for (int i = 0; i < 32; i++)
        pthread_join(threads[i], NULL);

    creg_fini(cr);
}

/*
 * ------------------------------------------------------
 * PROTOCOL TESTS
 * ------------------------------------------------------
 */

Test(student_extra, protocol_roundtrip, .timeout = 5) {
    int fds[2];
    pipe(fds);

    int sender = fds[1];
    int receiver = fds[0];

    BRS_PACKET_HEADER hdr_send = {
        .type = BRS_DEPOSIT_PKT,
        .size = htons(sizeof(BRS_FUNDS_INFO)),
        .timestamp_sec = htonl(123),
        .timestamp_nsec = htonl(456)
    };

    BRS_FUNDS_INFO payload_send = { htonl(1000) };
    cr_assert_eq(proto_send_packet(sender, &hdr_send, &payload_send), 0);

    BRS_PACKET_HEADER hdr_recv;
    void *payload_recv = NULL;

    cr_assert_eq(proto_recv_packet(receiver, &hdr_recv, &payload_recv), 0);

    cr_assert_eq(hdr_recv.type, BRS_DEPOSIT_PKT);
    cr_assert_eq(ntohl(hdr_recv.timestamp_sec), 123);
    cr_assert_eq(ntohl(hdr_recv.timestamp_nsec), 456);

    BRS_FUNDS_INFO *funds = payload_recv;
    cr_assert_eq(ntohl(funds->amount), 1000);

    free(payload_recv);
}

/*
 * ------------------------------------------------------
 * ACCOUNT MODULE TESTS
 * ------------------------------------------------------
 */

Test(account_suite, lookup_returns_same_account, .timeout = 5) {
    accounts_init();
    ACCOUNT *a1 = account_lookup("bob");
    ACCOUNT *a2 = account_lookup("bob");
    cr_assert_eq(a1, a2);
    accounts_fini();
}

Test(account_suite, lookup_different_accounts, .timeout = 5) {
    accounts_init();
    ACCOUNT *a1 = account_lookup("alice");
    ACCOUNT *a2 = account_lookup("bob");
    cr_assert_neq(a1, a2);
    accounts_fini();
}

Test(account_suite, increase_balance, .timeout = 5) {
    accounts_init();
    ACCOUNT *acc = account_lookup("alice");
    account_increase_balance(acc, 500);

    BRS_STATUS_INFO info;
    account_get_status(acc, &info);
    cr_assert_eq(ntohl(info.balance), 500);

    accounts_fini();
}

Test(account_suite, decrease_balance_success, .timeout = 5) {
    accounts_init();
    ACCOUNT *acc = account_lookup("alice");
    account_increase_balance(acc, 600);

    cr_assert_eq(account_decrease_balance(acc, 200), 0);

    BRS_STATUS_INFO info;
    account_get_status(acc, &info);
    cr_assert_eq(ntohl(info.balance), 400);

    accounts_fini();
}

Test(account_suite, decrease_balance_fail, .timeout = 5) {
    accounts_init();
    ACCOUNT *acc = account_lookup("alice");
    account_increase_balance(acc, 100);

    cr_assert_eq(account_decrease_balance(acc, 200), -1);

    BRS_STATUS_INFO info;
    account_get_status(acc, &info);
    cr_assert_eq(ntohl(info.balance), 100);

    accounts_fini();
}

Test(account_suite, increase_inventory, .timeout = 5) {
    accounts_init();
    ACCOUNT *acc = account_lookup("alice");

    account_increase_inventory(acc, 30);

    BRS_STATUS_INFO info;
    account_get_status(acc, &info);
    cr_assert_eq(ntohl(info.inventory), 30);

    accounts_fini();
}

Test(account_suite, decrease_inventory_success, .timeout = 5) {
    accounts_init();
    ACCOUNT *acc = account_lookup("alice");
    account_increase_inventory(acc, 50);

    cr_assert_eq(account_decrease_inventory(acc, 20), 0);

    BRS_STATUS_INFO info;
    account_get_status(acc, &info);
    cr_assert_eq(ntohl(info.inventory), 30);

    accounts_fini();
}

Test(account_suite, decrease_inventory_fail, .timeout = 5) {
    accounts_init();
    ACCOUNT *acc = account_lookup("alice");
    account_increase_inventory(acc, 10);

    cr_assert_eq(account_decrease_inventory(acc, 20), -1);

    BRS_STATUS_INFO info;
    account_get_status(acc, &info);
    cr_assert_eq(ntohl(info.inventory), 10);

    accounts_fini();
}

static void *balance_stress(void *arg) {
    ACCOUNT *acc = arg;
    for (int i = 0; i < 10000; i++)
        account_increase_balance(acc, 1);
    return NULL;
}

Test(account_suite, concurrency_stress, .timeout = 10) {
    accounts_init();
    ACCOUNT *acc = account_lookup("alice");

    pthread_t t1, t2;
    pthread_create(&t1, NULL, balance_stress, acc);
    pthread_create(&t2, NULL, balance_stress, acc);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    BRS_STATUS_INFO info;
    account_get_status(acc, &info);

    cr_assert_eq(ntohl(info.balance), 20000);

    accounts_fini();
}

/*
 * ------------------------------------------------------
 * TRADER MODULE TESTS
 * ------------------------------------------------------
 */

// Helper: create a pipe and return both ends
static void create_pipe_fds(int *read_fd, int *write_fd) {
    int fds[2];
    pipe(fds);
    *read_fd = fds[0];
    *write_fd = fds[1];
}

Test(trader_suite, login_basic, .timeout = 5) {
    traders_init();
    accounts_init();
    
    int read_fd, write_fd;
    create_pipe_fds(&read_fd, &write_fd);
    
    TRADER *trader = trader_login(write_fd, "alice");
    cr_assert_not_null(trader, "Login should succeed");
    
    trader_logout(trader);
    
    close(read_fd);
    close(write_fd);
    accounts_fini();
    traders_fini();
}

Test(trader_suite, login_duplicate_username, .timeout = 5) {
    traders_init();
    accounts_init();
    
    int fd1_r, fd1_w, fd2_r, fd2_w;
    create_pipe_fds(&fd1_r, &fd1_w);
    create_pipe_fds(&fd2_r, &fd2_w);
    
    TRADER *trader1 = trader_login(fd1_w, "alice");
    cr_assert_not_null(trader1, "First login should succeed");
    
    TRADER *trader2 = trader_login(fd2_w, "alice");
    cr_assert_null(trader2, "Second login with same username should fail");
    
    trader_logout(trader1);
    
    close(fd1_r);
    close(fd1_w);
    close(fd2_r);
    close(fd2_w);
    accounts_fini();
    traders_fini();
}

Test(trader_suite, login_after_logout, .timeout = 5) {
    traders_init();
    accounts_init();
    
    int fd1_r, fd1_w, fd2_r, fd2_w;
    create_pipe_fds(&fd1_r, &fd1_w);
    create_pipe_fds(&fd2_r, &fd2_w);
    
    TRADER *trader1 = trader_login(fd1_w, "alice");
    cr_assert_not_null(trader1);
    
    trader_logout(trader1);
    
    // Should be able to login again after logout
    TRADER *trader2 = trader_login(fd2_w, "alice");
    cr_assert_not_null(trader2, "Should be able to login after logout");
    
    trader_logout(trader2);
    
    close(fd1_r);
    close(fd1_w);
    close(fd2_r);
    close(fd2_w);
    accounts_fini();
    traders_fini();
}

Test(trader_suite, ref_unref_basic, .timeout = 5) {
    traders_init();
    accounts_init();
    
    int read_fd, write_fd;
    create_pipe_fds(&read_fd, &write_fd);
    
    TRADER *trader = trader_login(write_fd, "bob");
    cr_assert_not_null(trader);
    
    // Increase ref count
    TRADER *ref = trader_ref(trader, "test ref");
    cr_assert_eq(ref, trader);
    
    // Decrease ref count
    trader_unref(trader, "test unref");
    
    trader_logout(trader);
    
    close(read_fd);
    close(write_fd);
    accounts_fini();
    traders_fini();
}

Test(trader_suite, ref_count_prevents_free, .timeout = 5) {
    traders_init();
    accounts_init();
    
    int read_fd, write_fd;
    create_pipe_fds(&read_fd, &write_fd);
    
    TRADER *trader = trader_login(write_fd, "charlie");
    cr_assert_not_null(trader);
    
    // Add extra references
    trader_ref(trader, "ref1");
    trader_ref(trader, "ref2");
    
    // Logout doesn't free because refs still exist
    trader_logout(trader);
    
    // Release references
    trader_unref(trader, "unref1");
    trader_unref(trader, "unref2");
    
    close(read_fd);
    close(write_fd);
    accounts_fini();
    traders_fini();
}

Test(trader_suite, get_account, .timeout = 5) {
    traders_init();
    accounts_init();
    
    int read_fd, write_fd;
    create_pipe_fds(&read_fd, &write_fd);
    
    TRADER *trader = trader_login(write_fd, "dave");
    cr_assert_not_null(trader);
    
    ACCOUNT *acc = trader_get_account(trader);
    cr_assert_not_null(acc, "Should return valid account");
    
    // Verify it's the correct account
    ACCOUNT *lookup_acc = account_lookup("dave");
    cr_assert_eq(acc, lookup_acc, "Should return same account as lookup");
    
    trader_logout(trader);
    
    close(read_fd);
    close(write_fd);
    accounts_fini();
    traders_fini();
}

Test(trader_suite, send_ack_no_payload, .timeout = 5) {
    traders_init();
    accounts_init();
    
    int read_fd, write_fd;
    create_pipe_fds(&read_fd, &write_fd);
    
    TRADER *trader = trader_login(write_fd, "eve");
    cr_assert_not_null(trader);
    
    // Send ACK with no payload
    int ret = trader_send_ack(trader, NULL);
    cr_assert_eq(ret, 0, "send_ack should succeed");
    
    // Read back the packet
    BRS_PACKET_HEADER hdr;
    void *payload = NULL;
    ret = proto_recv_packet(read_fd, &hdr, &payload);
    
    cr_assert_eq(ret, 0, "Should receive packet");
    cr_assert_eq(hdr.type, BRS_ACK_PKT, "Should be ACK packet");
    cr_assert_eq(ntohs(hdr.size), 0, "Should have no payload");
    cr_assert_null(payload, "Payload should be NULL");
    
    trader_logout(trader);
    
    close(read_fd);
    close(write_fd);
    accounts_fini();
    traders_fini();
}

Test(trader_suite, send_ack_with_payload, .timeout = 5) {
    traders_init();
    accounts_init();
    
    int read_fd, write_fd;
    create_pipe_fds(&read_fd, &write_fd);
    
    TRADER *trader = trader_login(write_fd, "frank");
    cr_assert_not_null(trader);
    
    // Create status info
    BRS_STATUS_INFO info;
    memset(&info, 0, sizeof(info));
    info.balance = htonl(1000);
    info.inventory = htonl(50);
    
    // Send ACK with payload
    int ret = trader_send_ack(trader, &info);
    cr_assert_eq(ret, 0, "send_ack should succeed");
    
    // Read back the packet
    BRS_PACKET_HEADER hdr;
    void *payload = NULL;
    ret = proto_recv_packet(read_fd, &hdr, &payload);
    
    cr_assert_eq(ret, 0, "Should receive packet");
    cr_assert_eq(hdr.type, BRS_ACK_PKT, "Should be ACK packet");
    cr_assert_eq(ntohs(hdr.size), sizeof(BRS_STATUS_INFO), "Should have status info size");
    cr_assert_not_null(payload, "Payload should not be NULL");
    
    BRS_STATUS_INFO *recv_info = (BRS_STATUS_INFO *)payload;
    cr_assert_eq(ntohl(recv_info->balance), 1000, "Balance should match");
    cr_assert_eq(ntohl(recv_info->inventory), 50, "Inventory should match");
    
    free(payload);
    trader_logout(trader);
    
    close(read_fd);
    close(write_fd);
    accounts_fini();
    traders_fini();
}

Test(trader_suite, send_nack, .timeout = 5) {
    traders_init();
    accounts_init();
    
    int read_fd, write_fd;
    create_pipe_fds(&read_fd, &write_fd);
    
    TRADER *trader = trader_login(write_fd, "grace");
    cr_assert_not_null(trader);
    
    // Send NACK
    int ret = trader_send_nack(trader);
    cr_assert_eq(ret, 0, "send_nack should succeed");
    
    // Read back the packet
    BRS_PACKET_HEADER hdr;
    void *payload = NULL;
    ret = proto_recv_packet(read_fd, &hdr, &payload);
    
    cr_assert_eq(ret, 0, "Should receive packet");
    cr_assert_eq(hdr.type, BRS_NACK_PKT, "Should be NACK packet");
    cr_assert_eq(ntohs(hdr.size), 0, "Should have no payload");
    cr_assert_null(payload, "Payload should be NULL");
    
    trader_logout(trader);
    
    close(read_fd);
    close(write_fd);
    accounts_fini();
    traders_fini();
}

Test(trader_suite, broadcast_packet, .timeout = 5) {
    traders_init();
    accounts_init();
    
    int fd1_r, fd1_w, fd2_r, fd2_w, fd3_r, fd3_w;
    create_pipe_fds(&fd1_r, &fd1_w);
    create_pipe_fds(&fd2_r, &fd2_w);
    create_pipe_fds(&fd3_r, &fd3_w);
    
    TRADER *trader1 = trader_login(fd1_w, "alice");
    TRADER *trader2 = trader_login(fd2_w, "bob");
    TRADER *trader3 = trader_login(fd3_w, "charlie");
    
    cr_assert_not_null(trader1);
    cr_assert_not_null(trader2);
    cr_assert_not_null(trader3);
    
    // Create a packet to broadcast
    BRS_PACKET_HEADER hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.type = BRS_POSTED_PKT;
    hdr.size = 0;
    
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    hdr.timestamp_sec = htonl(ts.tv_sec);
    hdr.timestamp_nsec = htonl(ts.tv_nsec);
    
    // Broadcast to all traders
    int ret = trader_broadcast_packet(&hdr, NULL);
    cr_assert_eq(ret, 0, "Broadcast should succeed");
    
    // All three traders should receive the packet
    BRS_PACKET_HEADER recv_hdr1, recv_hdr2, recv_hdr3;
    void *payload1 = NULL, *payload2 = NULL, *payload3 = NULL;
    
    cr_assert_eq(proto_recv_packet(fd1_r, &recv_hdr1, &payload1), 0);
    cr_assert_eq(proto_recv_packet(fd2_r, &recv_hdr2, &payload2), 0);
    cr_assert_eq(proto_recv_packet(fd3_r, &recv_hdr3, &payload3), 0);
    
    cr_assert_eq(recv_hdr1.type, BRS_POSTED_PKT);
    cr_assert_eq(recv_hdr2.type, BRS_POSTED_PKT);
    cr_assert_eq(recv_hdr3.type, BRS_POSTED_PKT);
    
    trader_logout(trader1);
    trader_logout(trader2);
    trader_logout(trader3);
    
    close(fd1_r); close(fd1_w);
    close(fd2_r); close(fd2_w);
    close(fd3_r); close(fd3_w);
    
    accounts_fini();
    traders_fini();
}

static void *concurrent_login_logout(void *arg) {
    int id = *(int *)arg;
    char name[32];
    snprintf(name, sizeof(name), "trader_%d", id);
    
    for (int i = 0; i < 10; i++) {
        int read_fd, write_fd;
        create_pipe_fds(&read_fd, &write_fd);
        
        TRADER *trader = trader_login(write_fd, name);
        if (trader) {
            usleep(100);
            trader_logout(trader);
        }
        
        close(read_fd);
        close(write_fd);
        usleep(100);
    }
    
    return NULL;
}

Test(trader_suite, concurrent_login_logout_stress, .timeout = 10) {
    traders_init();
    accounts_init();
    
    pthread_t threads[8];
    int ids[8];
    
    for (int i = 0; i < 8; i++) {
        ids[i] = i;
        pthread_create(&threads[i], NULL, concurrent_login_logout, &ids[i]);
    }
    
    for (int i = 0; i < 8; i++) {
        pthread_join(threads[i], NULL);
    }
    
    accounts_fini();
    traders_fini();
}

static void *concurrent_ref_unref(void *arg) {
    TRADER *trader = (TRADER *)arg;
    
    for (int i = 0; i < 1000; i++) {
        trader_ref(trader, "stress test");
        usleep(10);
        trader_unref(trader, "stress test");
    }
    
    return NULL;
}

Test(trader_suite, concurrent_ref_unref_stress, .timeout = 10) {
    traders_init();
    accounts_init();
    
    int read_fd, write_fd;
    create_pipe_fds(&read_fd, &write_fd);
    
    TRADER *trader = trader_login(write_fd, "stress_trader");
    cr_assert_not_null(trader);
    
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], NULL, concurrent_ref_unref, trader);
    }
    
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }
    
    trader_logout(trader);
    
    close(read_fd);
    close(write_fd);
    accounts_fini();
    traders_fini();
}

/*
 * ------------------------------------------------------
 * INTEGRATION: Trader + Account interaction
 * ------------------------------------------------------
 */

Test(integration_suite, trader_account_balance_modify, .timeout = 5) {
    traders_init();
    accounts_init();
    
    int read_fd, write_fd;
    create_pipe_fds(&read_fd, &write_fd);
    
    // Login trader
    TRADER *trader = trader_login(write_fd, "alice");
    cr_assert_not_null(trader);
    
    // Get account and modify balance
    ACCOUNT *acc = trader_get_account(trader);
    account_increase_balance(acc, 5000);
    
    // Verify via account_get_status
    BRS_STATUS_INFO info;
    account_get_status(acc, &info);
    cr_assert_eq(ntohl(info.balance), 5000);
    
    // Send ACK with status and verify trader can see it
    trader_send_ack(trader, &info);
    
    BRS_PACKET_HEADER hdr;
    void *payload = NULL;
    proto_recv_packet(read_fd, &hdr, &payload);
    
    cr_assert_eq(hdr.type, BRS_ACK_PKT);
    BRS_STATUS_INFO *recv_info = (BRS_STATUS_INFO *)payload;
    cr_assert_eq(ntohl(recv_info->balance), 5000);
    
    free(payload);
    trader_logout(trader);
    close(read_fd);
    close(write_fd);
    accounts_fini();
    traders_fini();
}

Test(integration_suite, trader_account_inventory_modify, .timeout = 5) {
    traders_init();
    accounts_init();
    
    int read_fd, write_fd;
    create_pipe_fds(&read_fd, &write_fd);
    
    TRADER *trader = trader_login(write_fd, "bob");
    ACCOUNT *acc = trader_get_account(trader);
    
    // Add inventory
    account_increase_inventory(acc, 100);
    
    // Verify
    BRS_STATUS_INFO info;
    account_get_status(acc, &info);
    cr_assert_eq(ntohl(info.inventory), 100);
    
    // Decrease inventory
    cr_assert_eq(account_decrease_inventory(acc, 30), 0);
    account_get_status(acc, &info);
    cr_assert_eq(ntohl(info.inventory), 70);
    
    trader_logout(trader);
    close(read_fd);
    close(write_fd);
    accounts_fini();
    traders_fini();
}

Test(integration_suite, multiple_traders_same_account_sequential, .timeout = 5) {
    traders_init();
    accounts_init();
    
    int fd1_r, fd1_w, fd2_r, fd2_w;
    create_pipe_fds(&fd1_r, &fd1_w);
    create_pipe_fds(&fd2_r, &fd2_w);
    
    // First trader logs in, deposits money
    TRADER *trader1 = trader_login(fd1_w, "charlie");
    ACCOUNT *acc1 = trader_get_account(trader1);
    account_increase_balance(acc1, 1000);
    trader_logout(trader1);
    
    // Second trader logs in with same name, should see same balance
    TRADER *trader2 = trader_login(fd2_w, "charlie");
    ACCOUNT *acc2 = trader_get_account(trader2);
    
    cr_assert_eq(acc1, acc2, "Should be same account");
    
    BRS_STATUS_INFO info;
    account_get_status(acc2, &info);
    cr_assert_eq(ntohl(info.balance), 1000, "Balance should persist");
    
    trader_logout(trader2);
    close(fd1_r); close(fd1_w);
    close(fd2_r); close(fd2_w);
    accounts_fini();
    traders_fini();
}

/*
 * ------------------------------------------------------
 * INTEGRATION: Broadcast during concurrent modifications
 * ------------------------------------------------------
 */

Test(integration_suite, broadcast_during_account_changes, .timeout = 10) {
    traders_init();
    accounts_init();
    
    int fd1_r, fd1_w, fd2_r, fd2_w;
    create_pipe_fds(&fd1_r, &fd1_w);
    create_pipe_fds(&fd2_r, &fd2_w);
    
    TRADER *trader1 = trader_login(fd1_w, "alice");
    TRADER *trader2 = trader_login(fd2_w, "bob");
    
    // Modify accounts while broadcasting
    ACCOUNT *acc1 = trader_get_account(trader1);
    account_increase_balance(acc1, 2000);
    
    // Broadcast a packet
    BRS_PACKET_HEADER hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.type = BRS_TRADED_PKT;
    hdr.size = 0;
    
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    hdr.timestamp_sec = htonl(ts.tv_sec);
    hdr.timestamp_nsec = htonl(ts.tv_nsec);
    
    trader_broadcast_packet(&hdr, NULL);
    
    // Both should receive
    BRS_PACKET_HEADER recv1, recv2;
    void *p1 = NULL, *p2 = NULL;
    cr_assert_eq(proto_recv_packet(fd1_r, &recv1, &p1), 0);
    cr_assert_eq(proto_recv_packet(fd2_r, &recv2, &p2), 0);
    
    cr_assert_eq(recv1.type, BRS_TRADED_PKT);
    cr_assert_eq(recv2.type, BRS_TRADED_PKT);
    
    // Account modification should still be correct
    BRS_STATUS_INFO info;
    account_get_status(acc1, &info);
    cr_assert_eq(ntohl(info.balance), 2000);
    
    trader_logout(trader1);
    trader_logout(trader2);
    close(fd1_r); close(fd1_w);
    close(fd2_r); close(fd2_w);
    accounts_fini();
    traders_fini();
}

/*
 * ------------------------------------------------------
 * STRESS TESTS: Heavy concurrent operations
 * ------------------------------------------------------
 */

typedef struct {
    int trader_id;
    int operations;
} stress_args;

static void *stress_trader_operations(void *arg) {
    stress_args *args = (stress_args *)arg;
    char name[32];
    snprintf(name, sizeof(name), "stress_%d", args->trader_id);
    
    for (int i = 0; i < args->operations; i++) {
        int r_fd, w_fd;
        create_pipe_fds(&r_fd, &w_fd);
        
        TRADER *trader = trader_login(w_fd, name);
        if (trader) {
            ACCOUNT *acc = trader_get_account(trader);
            
            // Perform random operations
            if (i % 3 == 0) {
                account_increase_balance(acc, 100);
            } else if (i % 3 == 1) {
                account_increase_inventory(acc, 10);
            } else {
                BRS_STATUS_INFO info;
                account_get_status(acc, &info);
            }
            
            // Send packets
            if (i % 2 == 0) {
                trader_send_ack(trader, NULL);
            } else {
                trader_send_nack(trader);
            }
            
            trader_logout(trader);
        }
        
        close(r_fd);
        close(w_fd);
        usleep(50);
    }
    
    return NULL;
}

Test(stress_suite, heavy_login_logout_with_operations, .timeout = 20) {
    traders_init();
    accounts_init();
    
    pthread_t threads[10];
    stress_args args[10];
    
    for (int i = 0; i < 10; i++) {
        args[i].trader_id = i;
        args[i].operations = 50;
        pthread_create(&threads[i], NULL, stress_trader_operations, &args[i]);
    }
    
    for (int i = 0; i < 10; i++) {
        pthread_join(threads[i], NULL);
    }
    
    accounts_fini();
    traders_fini();
}

static void *stress_broadcast(void *arg) {
    int iterations = *(int *)arg;
    
    for (int i = 0; i < iterations; i++) {
        BRS_PACKET_HEADER hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.type = BRS_POSTED_PKT;
        hdr.size = 0;
        
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        hdr.timestamp_sec = htonl(ts.tv_sec);
        hdr.timestamp_nsec = htonl(ts.tv_nsec);
        
        trader_broadcast_packet(&hdr, NULL);
        usleep(100);
    }
    
    return NULL;
}

Test(stress_suite, concurrent_broadcasts, .timeout = 15) {
    traders_init();
    accounts_init();
    
    // Create some traders to receive broadcasts
    int fds[6][2];
    TRADER *traders[3];
    
    for (int i = 0; i < 3; i++) {
        create_pipe_fds(&fds[i][0], &fds[i][1]);
        char name[32];
        snprintf(name, sizeof(name), "receiver_%d", i);
        traders[i] = trader_login(fds[i][1], name);
        cr_assert_not_null(traders[i]);
    }
    
    // Multiple threads broadcasting simultaneously
    pthread_t broadcast_threads[5];
    int iterations = 20;
    
    for (int i = 0; i < 5; i++) {
        pthread_create(&broadcast_threads[i], NULL, stress_broadcast, &iterations);
    }
    
    for (int i = 0; i < 5; i++) {
        pthread_join(broadcast_threads[i], NULL);
    }
    
    // Verify traders can still receive (drain their pipes)
    for (int i = 0; i < 3; i++) {
        // Set non-blocking to drain
        int flags = fcntl(fds[i][0], F_GETFL, 0);
        fcntl(fds[i][0], F_SETFL, flags | O_NONBLOCK);
        
        BRS_PACKET_HEADER hdr;
        void *payload;
        int count = 0;
        
        // Count received packets
        while (proto_recv_packet(fds[i][0], &hdr, &payload) == 0) {
            free(payload);
            count++;
        }
        
        // Each trader should have received all broadcasts (5 threads * 20 iterations)
        cr_assert_eq(count, 100, "Should receive all broadcasts");
        
        trader_logout(traders[i]);
        close(fds[i][0]);
        close(fds[i][1]);
    }
    
    accounts_fini();
    traders_fini();
}

/*
 * ------------------------------------------------------
 * EDGE CASES AND ERROR HANDLING
 * ------------------------------------------------------
 */

Test(edge_cases, trader_logout_with_pending_refs, .timeout = 5) {
    traders_init();
    accounts_init();
    
    int r_fd, w_fd;
    create_pipe_fds(&r_fd, &w_fd);
    
    TRADER *trader = trader_login(w_fd, "pending");
    
    // Add multiple references
    for (int i = 0; i < 10; i++) {
        trader_ref(trader, "pending ref");
    }
    
    // Logout shouldn't free trader
    trader_logout(trader);
    
    // Trader should still be usable
    BRS_STATUS_INFO info;
    memset(&info, 0, sizeof(info));
    info.balance = htonl(999);
    
    int ret = trader_send_ack(trader, &info);
    cr_assert_eq(ret, 0, "Should still be able to send");
    
    // Release all refs
    for (int i = 0; i < 10; i++) {
        trader_unref(trader, "pending unref");
    }
    
    close(r_fd);
    close(w_fd);
    accounts_fini();
    traders_fini();
}

Test(edge_cases, max_traders_limit, .timeout = 5) {
    traders_init();
    accounts_init();
    
    TRADER *traders[MAX_TRADERS];
    int fds[MAX_TRADERS][2];
    
    // Fill up to max
    for (int i = 0; i < MAX_TRADERS; i++) {
        create_pipe_fds(&fds[i][0], &fds[i][1]);
        char name[32];
        snprintf(name, sizeof(name), "max_trader_%d", i);
        traders[i] = trader_login(fds[i][1], name);
        cr_assert_not_null(traders[i], "Should succeed until max");
    }
    
    // One more should fail
    int extra_r, extra_w;
    create_pipe_fds(&extra_r, &extra_w);
    TRADER *extra = trader_login(extra_w, "extra_trader");
    cr_assert_null(extra, "Should fail when max traders reached");
    
    // Cleanup
    for (int i = 0; i < MAX_TRADERS; i++) {
        trader_logout(traders[i]);
        close(fds[i][0]);
        close(fds[i][1]);
    }
    close(extra_r);
    close(extra_w);
    
    accounts_fini();
    traders_fini();
}

Test(edge_cases, rapid_login_logout_same_name, .timeout = 10) {
    traders_init();
    accounts_init();
    
    // Rapidly login/logout with same name
    for (int i = 0; i < 100; i++) {
        int r_fd, w_fd;
        create_pipe_fds(&r_fd, &w_fd);
        
        TRADER *trader = trader_login(w_fd, "rapid");
        cr_assert_not_null(trader, "Login %d should succeed", i);
        
        trader_logout(trader);
        
        close(r_fd);
        close(w_fd);
    }
    
    accounts_fini();
    traders_fini();
}

Test(edge_cases, account_operations_across_sessions, .timeout = 5) {
    traders_init();
    accounts_init();
    
    // Session 1: deposit
    int fd1_r, fd1_w;
    create_pipe_fds(&fd1_r, &fd1_w);
    TRADER *t1 = trader_login(fd1_w, "persistent");
    ACCOUNT *acc1 = trader_get_account(t1);
    account_increase_balance(acc1, 5000);
    account_increase_inventory(acc1, 200);
    trader_logout(t1);
    close(fd1_r);
    close(fd1_w);
    
    // Session 2: verify and withdraw
    int fd2_r, fd2_w;
    create_pipe_fds(&fd2_r, &fd2_w);
    TRADER *t2 = trader_login(fd2_w, "persistent");
    ACCOUNT *acc2 = trader_get_account(t2);
    
    BRS_STATUS_INFO info;
    account_get_status(acc2, &info);
    cr_assert_eq(ntohl(info.balance), 5000, "Balance persists");
    cr_assert_eq(ntohl(info.inventory), 200, "Inventory persists");
    
    cr_assert_eq(account_decrease_balance(acc2, 1000), 0);
    cr_assert_eq(account_decrease_inventory(acc2, 50), 0);
    trader_logout(t2);
    close(fd2_r);
    close(fd2_w);
    
    // Session 3: verify changes
    int fd3_r, fd3_w;
    create_pipe_fds(&fd3_r, &fd3_w);
    TRADER *t3 = trader_login(fd3_w, "persistent");
    ACCOUNT *acc3 = trader_get_account(t3);
    
    account_get_status(acc3, &info);
    cr_assert_eq(ntohl(info.balance), 4000, "Balance updated");
    cr_assert_eq(ntohl(info.inventory), 150, "Inventory updated");
    
    trader_logout(t3);
    close(fd3_r);
    close(fd3_w);
    
    accounts_fini();
    traders_fini();
}

/*
 * ------------------------------------------------------
 * MEMORY LEAK AND RESOURCE TESTS
 * ------------------------------------------------------
 */

Test(resource_tests, no_fd_leaks, .timeout = 10) {
    traders_init();
    accounts_init();
    
    // Perform many operations and ensure fds don't leak
    for (int i = 0; i < 100; i++) {
        int r_fd, w_fd;
        create_pipe_fds(&r_fd, &w_fd);
        
        char name[32];
        snprintf(name, sizeof(name), "fd_test_%d", i % 10);
        
        TRADER *trader = trader_login(w_fd, name);
        if (trader) {
            trader_send_ack(trader, NULL);
            trader_logout(trader);
        }
        
        close(r_fd);
        close(w_fd);
    }
    
    accounts_fini();
    traders_fini();
}

Test(resource_tests, cleanup_after_fini, .timeout = 5) {
    traders_init();
    accounts_init();
    
    // Create several traders
    TRADER *traders[5];
    int fds[5][2];
    
    for (int i = 0; i < 5; i++) {
        create_pipe_fds(&fds[i][0], &fds[i][1]);
        char name[32];
        snprintf(name, sizeof(name), "cleanup_%d", i);
        traders[i] = trader_login(fds[i][1], name);
        
        // Add extra refs
        trader_ref(traders[i], "extra");
    }
    
    // Call fini - should clean up gracefully even with refs
    traders_fini();
    accounts_fini();
    
    // Close fds
    for (int i = 0; i < 5; i++) {
        close(fds[i][0]);
        close(fds[i][1]);
    }
    
    cr_assert(1, "Should complete without crashing");
}

// ==================== EXCHANGE MODULE TESTS ====================

Test(exchange_suite, init_and_fini, .timeout = 5) {
    EXCHANGE *xchg = exchange_init();
    cr_assert_not_null(xchg, "Exchange should initialize successfully");
    
    sleep(1);
    exchange_fini(xchg);
}

Test(exchange_suite, initial_status, .timeout = 5) {
    EXCHANGE *xchg = exchange_init();
    
    BRS_STATUS_INFO info;
    exchange_get_status(xchg, NULL, &info);
    
    cr_assert_eq(ntohl(info.bid), 0, "Initial bid should be 0");
    cr_assert_eq(ntohl(info.ask), 0, "Initial ask should be 0");
    cr_assert_eq(ntohl(info.last), 0, "Initial last price should be 0");
    
    sleep(1);
    exchange_fini(xchg);
}

Test(exchange_suite, post_single_buy_order, .timeout = 5) {
    accounts_init();
    traders_init();
    EXCHANGE *xchg = exchange_init();
    
    ACCOUNT *acc = account_lookup("bob");
    account_increase_balance(acc, 10000);
    
    // Create valid FD using pipe
    int pipefd[2];
    pipe(pipefd);
    TRADER *trader = trader_login(pipefd[0], "bob");
    cr_assert_not_null(trader);
    
    orderid_t order_id = exchange_post_buy(xchg, trader, 10, 100);
    cr_assert_neq(order_id, 0, "Order ID should be non-zero");
    
    BRS_STATUS_INFO info;
    exchange_get_status(xchg, NULL, &info);
    cr_assert_eq(ntohl(info.bid), 100, "Bid should be 100");
    cr_assert_eq(ntohl(info.ask), 0, "Ask should still be 0");
    
    trader_logout(trader);
    close(pipefd[0]);
    close(pipefd[1]);
    sleep(1);
    exchange_fini(xchg);
    traders_fini();
    accounts_fini();
}

Test(exchange_suite, post_single_sell_order, .timeout = 5) {
    accounts_init();
    traders_init();
    EXCHANGE *xchg = exchange_init();
    
    ACCOUNT *acc = account_lookup("alice");
    account_increase_inventory(acc, 50);
    
    int pipefd[2];
    pipe(pipefd);
    TRADER *trader = trader_login(pipefd[0], "alice");
    cr_assert_not_null(trader);
    
    orderid_t order_id = exchange_post_sell(xchg, trader, 20, 95);
    cr_assert_neq(order_id, 0, "Order ID should be non-zero");
    
    BRS_STATUS_INFO info;
    exchange_get_status(xchg, NULL, &info);
    cr_assert_eq(ntohl(info.bid), 0, "Bid should still be 0");
    cr_assert_eq(ntohl(info.ask), 95, "Ask should be 95");
    
    trader_logout(trader);
    close(pipefd[0]);
    close(pipefd[1]);
    sleep(1);
    exchange_fini(xchg);
    traders_fini();
    accounts_fini();
}

Test(exchange_suite, simple_match_exact_quantity, .timeout = 5) {
    accounts_init();
    traders_init();
    EXCHANGE *xchg = exchange_init();
    
    // Setup buyer
    ACCOUNT *buyer_acc = account_lookup("buyer");
    account_increase_balance(buyer_acc, 10000);
    int buyer_pipe[2];
    pipe(buyer_pipe);
    TRADER *buyer = trader_login(buyer_pipe[0], "buyer");
    
    // Setup seller
    ACCOUNT *seller_acc = account_lookup("seller");
    account_increase_inventory(seller_acc, 50);
    int seller_pipe[2];
    pipe(seller_pipe);
    TRADER *seller = trader_login(seller_pipe[0], "seller");
    
    // Post orders
    exchange_post_sell(xchg, seller, 10, 95);
    sleep(1);
    exchange_post_buy(xchg, buyer, 10, 100);
    sleep(1);
    
    // Check status
    BRS_STATUS_INFO buyer_info, seller_info;
    account_get_status(buyer_acc, &buyer_info);
    account_get_status(seller_acc, &seller_info);
    
    cr_assert_eq(ntohl(buyer_info.inventory), 10, "Buyer should have 10 items");
    cr_assert_eq(ntohl(seller_info.balance), 950, "Seller should have 950 funds");
    
    trader_logout(buyer);
    trader_logout(seller);
    close(buyer_pipe[0]);
    close(buyer_pipe[1]);
    close(seller_pipe[0]);
    close(seller_pipe[1]);
    sleep(1);
    exchange_fini(xchg);
    traders_fini();
    accounts_fini();
}

Test(exchange_suite, partial_fill_buy_order, .timeout = 5) {
    accounts_init();
    traders_init();
    EXCHANGE *xchg = exchange_init();
    
    ACCOUNT *buyer_acc = account_lookup("buyer");
    account_increase_balance(buyer_acc, 10000);
    int buyer_pipe[2];
    pipe(buyer_pipe);
    TRADER *buyer = trader_login(buyer_pipe[0], "buyer");
    
    ACCOUNT *seller_acc = account_lookup("seller");
    account_increase_inventory(seller_acc, 5);
    int seller_pipe[2];
    pipe(seller_pipe);
    TRADER *seller = trader_login(seller_pipe[0], "seller");
    
    // Buyer wants 10, but seller only has 5
    exchange_post_sell(xchg, seller, 5, 100);
    sleep(1);
    exchange_post_buy(xchg, buyer, 10, 100);
    sleep(1);
    
    BRS_STATUS_INFO buyer_info;
    account_get_status(buyer_acc, &buyer_info);
    cr_assert_eq(ntohl(buyer_info.inventory), 5, "Buyer should have 5 items");
    
    // Buy order should still be pending for remaining 5
    BRS_STATUS_INFO xchg_info;
    exchange_get_status(xchg, NULL, &xchg_info);
    cr_assert_eq(ntohl(xchg_info.bid), 100, "Bid should still be 100");
    
    trader_logout(buyer);
    trader_logout(seller);
    close(buyer_pipe[0]);
    close(buyer_pipe[1]);
    close(seller_pipe[0]);
    close(seller_pipe[1]);
    sleep(1);
    exchange_fini(xchg);
    traders_fini();
    accounts_fini();
}

Test(exchange_suite, partial_fill_sell_order, .timeout = 5) {
    accounts_init();
    traders_init();
    EXCHANGE *xchg = exchange_init();
    
    ACCOUNT *buyer_acc = account_lookup("buyer");
    account_increase_balance(buyer_acc, 500);
    int buyer_pipe[2];
    pipe(buyer_pipe);
    TRADER *buyer = trader_login(buyer_pipe[0], "buyer");
    
    ACCOUNT *seller_acc = account_lookup("seller");
    account_increase_inventory(seller_acc, 20);
    int seller_pipe[2];
    pipe(seller_pipe);
    TRADER *seller = trader_login(seller_pipe[0], "seller");
    
    // Seller wants to sell 20, buyer only wants 5
    exchange_post_sell(xchg, seller, 20, 90);
    sleep(1);
    exchange_post_buy(xchg, buyer, 5, 100);
    sleep(1);
    
    BRS_STATUS_INFO seller_info;
    account_get_status(seller_acc, &seller_info);
    cr_assert_eq(ntohl(seller_info.balance), 450, "Seller should have received 450");
    
    // Sell order should still be pending for remaining 15
    BRS_STATUS_INFO xchg_info;
    exchange_get_status(xchg, NULL, &xchg_info);
    cr_assert_eq(ntohl(xchg_info.ask), 90, "Ask should still be 90");
    
    trader_logout(buyer);
    trader_logout(seller);
    close(buyer_pipe[0]);
    close(buyer_pipe[1]);
    close(seller_pipe[0]);
    close(seller_pipe[1]);
    sleep(1);
    exchange_fini(xchg);
    traders_fini();
    accounts_fini();
}

Test(exchange_suite, price_at_last_trade_price, .timeout = 15) {
    accounts_init();
    traders_init();
    EXCHANGE *xchg = exchange_init();
    
    ACCOUNT *buyer_acc = account_lookup("buyer");
    account_increase_balance(buyer_acc, 10000);
    int buyer_pipe[2];
    pipe(buyer_pipe);
    TRADER *buyer = trader_login(buyer_pipe[0], "buyer");
    
    ACCOUNT *seller_acc = account_lookup("seller");
    account_increase_inventory(seller_acc, 50);
    int seller_pipe[2];
    pipe(seller_pipe);
    TRADER *seller = trader_login(seller_pipe[0], "seller");
    
    // First trade establishes last price
    exchange_post_sell(xchg, seller, 10, 95);
    sleep(1);
    exchange_post_buy(xchg, buyer, 10, 100);
    sleep(1);
    
    BRS_STATUS_INFO info;
    exchange_get_status(xchg, NULL, &info);
    funds_t first_price = ntohl(info.last);
    cr_assert_eq(first_price, 95, "First trade should be at 95");
    
    // Second trade where last price (95) is in range [94, 98]
    exchange_post_sell(xchg, seller, 10, 94);
    sleep(1);
    exchange_post_buy(xchg, buyer, 10, 98);
    sleep(1);
    
    exchange_get_status(xchg, NULL, &info);
    cr_assert_eq(ntohl(info.last), 95, "Should trade at last price 95");
    
    trader_logout(buyer);
    trader_logout(seller);
    close(buyer_pipe[0]);
    close(buyer_pipe[1]);
    close(seller_pipe[0]);
    close(seller_pipe[1]);
    sleep(1);
    exchange_fini(xchg);
    traders_fini();
    accounts_fini();
}

Test(exchange_suite, price_below_range, .timeout = 15) {
    accounts_init();
    traders_init();
    EXCHANGE *xchg = exchange_init();
    
    ACCOUNT *buyer_acc = account_lookup("buyer");
    account_increase_balance(buyer_acc, 10000);
    int buyer_pipe[2];
    pipe(buyer_pipe);
    TRADER *buyer = trader_login(buyer_pipe[0], "buyer");
    
    ACCOUNT *seller_acc = account_lookup("seller");
    account_increase_inventory(seller_acc, 50);
    int seller_pipe[2];
    pipe(seller_pipe);
    TRADER *seller = trader_login(seller_pipe[0], "seller");
    
    // Establish last price at 80
    exchange_post_sell(xchg, seller, 5, 80);
    sleep(1);
    exchange_post_buy(xchg, buyer, 5, 80);
    sleep(1);
    
    // Trade where last price (80) < sell_price (95)
    exchange_post_sell(xchg, seller, 10, 95);
    sleep(1);
    exchange_post_buy(xchg, buyer, 10, 100);
    sleep(1);
    
    BRS_STATUS_INFO info;
    exchange_get_status(xchg, NULL, &info);
    cr_assert_eq(ntohl(info.last), 95, "Should trade at sell price 95");
    
    trader_logout(buyer);
    trader_logout(seller);
    close(buyer_pipe[0]);
    close(buyer_pipe[1]);
    close(seller_pipe[0]);
    close(seller_pipe[1]);
    sleep(1);
    exchange_fini(xchg);
    traders_fini();
    accounts_fini();
}

Test(exchange_suite, price_above_range, .timeout = 15) {
    accounts_init();
    traders_init();
    EXCHANGE *xchg = exchange_init();
    
    ACCOUNT *buyer_acc = account_lookup("buyer");
    account_increase_balance(buyer_acc, 10000);
    int buyer_pipe[2];
    pipe(buyer_pipe);
    TRADER *buyer = trader_login(buyer_pipe[0], "buyer");
    
    ACCOUNT *seller_acc = account_lookup("seller");
    account_increase_inventory(seller_acc, 50);
    int seller_pipe[2];
    pipe(seller_pipe);
    TRADER *seller = trader_login(seller_pipe[0], "seller");
    
    // Establish last price at 120
    exchange_post_sell(xchg, seller, 5, 120);
    sleep(1);
    exchange_post_buy(xchg, buyer, 5, 120);
    sleep(1);
    
    // Trade where last price (120) > buy_price (100)
    exchange_post_sell(xchg, seller, 10, 95);
    sleep(1);
    exchange_post_buy(xchg, buyer, 10, 100);
    sleep(1);
    
    BRS_STATUS_INFO info;
    exchange_get_status(xchg, NULL, &info);
    cr_assert_eq(ntohl(info.last), 100, "Should trade at buy price 100");
    
    trader_logout(buyer);
    trader_logout(seller);
    close(buyer_pipe[0]);
    close(buyer_pipe[1]);
    close(seller_pipe[0]);
    close(seller_pipe[1]);
    sleep(1);
    exchange_fini(xchg);
    traders_fini();
    accounts_fini();
}

Test(exchange_suite, buyer_refund_on_lower_price, .timeout = 5) {
    accounts_init();
    traders_init();
    EXCHANGE *xchg = exchange_init();
    
    ACCOUNT *buyer_acc = account_lookup("buyer");
    account_increase_balance(buyer_acc, 2000);
    int buyer_pipe[2];
    pipe(buyer_pipe);
    TRADER *buyer = trader_login(buyer_pipe[0], "buyer");
    
    ACCOUNT *seller_acc = account_lookup("seller");
    account_increase_inventory(seller_acc, 10);
    int seller_pipe[2];
    pipe(seller_pipe);
    TRADER *seller = trader_login(seller_pipe[0], "seller");
    
    // Buyer willing to pay up to 100, but trades at 90
    exchange_post_sell(xchg, seller, 10, 90);
    sleep(1);
    exchange_post_buy(xchg, buyer, 10, 100);
    sleep(1);
    
    BRS_STATUS_INFO buyer_info;
    account_get_status(buyer_acc, &buyer_info);
    
    // Buyer encumbered 1000, paid 900, should have 1100 left
    cr_assert_eq(ntohl(buyer_info.balance), 1100, "Buyer should get refund");
    
    trader_logout(buyer);
    trader_logout(seller);
    close(buyer_pipe[0]);
    close(buyer_pipe[1]);
    close(seller_pipe[0]);
    close(seller_pipe[1]);
    sleep(1);
    exchange_fini(xchg);
    traders_fini();
    accounts_fini();
}

Test(exchange_suite, cancel_buy_order, .timeout = 5) {
    accounts_init();
    traders_init();
    EXCHANGE *xchg = exchange_init();
    
    ACCOUNT *acc = account_lookup("trader");
    account_increase_balance(acc, 5000);
    int pipefd[2];
    pipe(pipefd);
    TRADER *trader = trader_login(pipefd[0], "trader");
    
    orderid_t order_id = exchange_post_buy(xchg, trader, 10, 100);
    
    BRS_STATUS_INFO info;
    account_get_status(acc, &info);
    cr_assert_eq(ntohl(info.balance), 4000, "1000 should be encumbered");
    
    quantity_t cancelled_qty;
    int result = exchange_cancel(xchg, trader, order_id, &cancelled_qty);
    cr_assert_eq(result, 0, "Cancel should succeed");
    cr_assert_eq(cancelled_qty, 10, "Cancelled quantity should be 10");
    
    account_get_status(acc, &info);
    cr_assert_eq(ntohl(info.balance), 5000, "Funds should be restored");
    
    trader_logout(trader);
    close(pipefd[0]);
    close(pipefd[1]);
    sleep(1);
    exchange_fini(xchg);
    traders_fini();
    accounts_fini();
}

Test(exchange_suite, cancel_sell_order, .timeout = 5) {
    accounts_init();
    traders_init();
    EXCHANGE *xchg = exchange_init();
    
    ACCOUNT *acc = account_lookup("trader");
    account_increase_inventory(acc, 50);
    int pipefd[2];
    pipe(pipefd);
    TRADER *trader = trader_login(pipefd[0], "trader");
    
    orderid_t order_id = exchange_post_sell(xchg, trader, 20, 100);
    
    BRS_STATUS_INFO info;
    account_get_status(acc, &info);
    cr_assert_eq(ntohl(info.inventory), 30, "20 should be encumbered");
    
    quantity_t cancelled_qty;
    int result = exchange_cancel(xchg, trader, order_id, &cancelled_qty);
    cr_assert_eq(result, 0, "Cancel should succeed");
    cr_assert_eq(cancelled_qty, 20, "Cancelled quantity should be 20");
    
    account_get_status(acc, &info);
    cr_assert_eq(ntohl(info.inventory), 50, "Inventory should be restored");
    
    trader_logout(trader);
    close(pipefd[0]);
    close(pipefd[1]);
    sleep(1);
    exchange_fini(xchg);
    traders_fini();
    accounts_fini();
}

Test(exchange_suite, cancel_nonexistent_order, .timeout = 5) {
    accounts_init();
    traders_init();
    EXCHANGE *xchg = exchange_init();
    
    int pipefd[2];
    pipe(pipefd);
    TRADER *trader = trader_login(pipefd[0], "trader");
    
    quantity_t cancelled_qty;
    int result = exchange_cancel(xchg, trader, 99999, &cancelled_qty);
    cr_assert_eq(result, -1, "Cancel should fail for nonexistent order");
    
    trader_logout(trader);
    close(pipefd[0]);
    close(pipefd[1]);
    sleep(1);
    exchange_fini(xchg);
    traders_fini();
    accounts_fini();
}

Test(exchange_suite, cancel_wrong_trader, .timeout = 5) {
    accounts_init();
    traders_init();
    EXCHANGE *xchg = exchange_init();
    
    ACCOUNT *acc1 = account_lookup("trader1");
    account_increase_balance(acc1, 5000);
    int pipe1[2];
    pipe(pipe1);
    TRADER *trader1 = trader_login(pipe1[0], "trader1");
    
    int pipe2[2];
    pipe(pipe2);
    TRADER *trader2 = trader_login(pipe2[0], "trader2");
    
    orderid_t order_id = exchange_post_buy(xchg, trader1, 10, 100);
    
    quantity_t cancelled_qty;
    int result = exchange_cancel(xchg, trader2, order_id, &cancelled_qty);
    cr_assert_eq(result, -1, "Cancel should fail when wrong trader tries");
    
    trader_logout(trader1);
    trader_logout(trader2);
    close(pipe1[0]);
    close(pipe1[1]);
    close(pipe2[0]);
    close(pipe2[1]);
    sleep(1);
    exchange_fini(xchg);
    traders_fini();
    accounts_fini();
}

Test(exchange_suite, multiple_buy_orders_best_bid, .timeout = 5) {
    accounts_init();
    traders_init();
    EXCHANGE *xchg = exchange_init();
    
    ACCOUNT *acc1 = account_lookup("trader1");
    account_increase_balance(acc1, 10000);
    int pipe1[2];
    pipe(pipe1);
    TRADER *trader1 = trader_login(pipe1[0], "trader1");
    
    ACCOUNT *acc2 = account_lookup("trader2");
    account_increase_balance(acc2, 10000);
    int pipe2[2];
    pipe(pipe2);
    TRADER *trader2 = trader_login(pipe2[0], "trader2");
    
    exchange_post_buy(xchg, trader1, 10, 95);
    exchange_post_buy(xchg, trader2, 10, 100);
    
    BRS_STATUS_INFO info;
    exchange_get_status(xchg, NULL, &info);
    cr_assert_eq(ntohl(info.bid), 100, "Bid should be highest at 100");
    
    trader_logout(trader1);
    trader_logout(trader2);
    close(pipe1[0]);
    close(pipe1[1]);
    close(pipe2[0]);
    close(pipe2[1]);
    sleep(1);
    exchange_fini(xchg);
    traders_fini();
    accounts_fini();
}

Test(exchange_suite, multiple_sell_orders_best_ask, .timeout = 5) {
    accounts_init();
    traders_init();
    EXCHANGE *xchg = exchange_init();
    
    ACCOUNT *acc1 = account_lookup("trader1");
    account_increase_inventory(acc1, 50);
    int pipe1[2];
    pipe(pipe1);
    TRADER *trader1 = trader_login(pipe1[0], "trader1");
    
    ACCOUNT *acc2 = account_lookup("trader2");
    account_increase_inventory(acc2, 50);
    int pipe2[2];
    pipe(pipe2);
    TRADER *trader2 = trader_login(pipe2[0], "trader2");
    
    exchange_post_sell(xchg, trader1, 10, 105);
    exchange_post_sell(xchg, trader2, 10, 100);
    
    BRS_STATUS_INFO info;
    exchange_get_status(xchg, NULL, &info);
    cr_assert_eq(ntohl(info.ask), 100, "Ask should be lowest at 100");
    
    trader_logout(trader1);
    trader_logout(trader2);
    close(pipe1[0]);
    close(pipe1[1]);
    close(pipe2[0]);
    close(pipe2[1]);
    sleep(1);
    exchange_fini(xchg);
    traders_fini();
    accounts_fini();
}

Test(exchange_suite, consecutive_trades, .timeout = 10) {
    accounts_init();
    traders_init();
    EXCHANGE *xchg = exchange_init();
    
    ACCOUNT *buyer_acc = account_lookup("buyer");
    account_increase_balance(buyer_acc, 10000);
    int buyer_pipe[2];
    pipe(buyer_pipe);
    TRADER *buyer = trader_login(buyer_pipe[0], "buyer");
    
    ACCOUNT *seller_acc = account_lookup("seller");
    account_increase_inventory(seller_acc, 100);
    int seller_pipe[2];
    pipe(seller_pipe);
    TRADER *seller = trader_login(seller_pipe[0], "seller");
    
    // Post all sell orders first
    exchange_post_sell(xchg, seller, 5, 95);
    exchange_post_sell(xchg, seller, 5, 95);
    exchange_post_sell(xchg, seller, 5, 95);
    sleep(1);
    
    // Then post matching buy orders
    exchange_post_buy(xchg, buyer, 5, 100);
    exchange_post_buy(xchg, buyer, 5, 100);
    exchange_post_buy(xchg, buyer, 5, 100);
    sleep(3);  // Give time for all trades
    
    BRS_STATUS_INFO buyer_info;
    account_get_status(buyer_acc, &buyer_info);
    cr_assert_eq(ntohl(buyer_info.inventory), 15, "Buyer should have 15 items");
    
    trader_logout(buyer);
    trader_logout(seller);
    close(buyer_pipe[0]);
    close(buyer_pipe[1]);
    close(seller_pipe[0]);
    close(seller_pipe[1]);
    sleep(1);
    exchange_fini(xchg);
    traders_fini();
    accounts_fini();
}

// ==================== INTEGRATION TESTS ====================

Test(integration_suite, full_lifecycle, .timeout = 10) {
    accounts_init();
    traders_init();
    EXCHANGE *xchg = exchange_init();
    
    // Create traders
    ACCOUNT *alice_acc = account_lookup("alice");
    account_increase_balance(alice_acc, 5000);
    account_increase_inventory(alice_acc, 30);
    int alice_pipe[2];
    pipe(alice_pipe);
    TRADER *alice = trader_login(alice_pipe[0], "alice");
    
    ACCOUNT *bob_acc = account_lookup("bob");
    account_increase_balance(bob_acc, 5000);
    int bob_pipe[2];
    pipe(bob_pipe);
    TRADER *bob = trader_login(bob_pipe[0], "bob");
    
    // Alice sells, Bob buys
    exchange_post_sell(xchg, alice, 10, 100);
    sleep(1);
    exchange_post_buy(xchg, bob, 10, 105);
    sleep(1);
    
    // Check results
    BRS_STATUS_INFO alice_info, bob_info;
    account_get_status(alice_acc, &alice_info);
    account_get_status(bob_acc, &bob_info);
    
    cr_assert_eq(ntohl(alice_info.inventory), 20, "Alice should have 20 items left");
    cr_assert_eq(ntohl(alice_info.balance), 6000, "Alice should have 6000 funds");
    cr_assert_eq(ntohl(bob_info.inventory), 10, "Bob should have 10 items");
    cr_assert(ntohl(bob_info.balance) >= 3950, "Bob should have refund");
    
    trader_logout(alice);
    trader_logout(bob);
    close(alice_pipe[0]);
    close(alice_pipe[1]);
    close(bob_pipe[0]);
    close(bob_pipe[1]);
    sleep(1);
    exchange_fini(xchg);
    traders_fini();
    accounts_fini();
}

Test(integration_suite, concurrent_orders, .timeout = 10) {
    accounts_init();
    traders_init();
    EXCHANGE *xchg = exchange_init();
    
    // Multiple traders posting orders concurrently
    ACCOUNT *acc1 = account_lookup("t1");
    account_increase_balance(acc1, 10000);
    int pipe1[2];
    pipe(pipe1);
    TRADER *t1 = trader_login(pipe1[0], "t1");
    
    ACCOUNT *acc2 = account_lookup("t2");
    account_increase_balance(acc2, 10000);
    int pipe2[2];
    pipe(pipe2);
    TRADER *t2 = trader_login(pipe2[0], "t2");
    
    ACCOUNT *acc3 = account_lookup("t3");
    account_increase_inventory(acc3, 100);
    int pipe3[2];
    pipe(pipe3);
    TRADER *t3 = trader_login(pipe3[0], "t3");
    
    // Post many orders rapidly
    exchange_post_buy(xchg, t1, 5, 100);
    exchange_post_buy(xchg, t2, 5, 98);
    exchange_post_sell(xchg, t3, 10, 95);
    
    sleep(2);
    
    // At least one trade should have occurred
    BRS_STATUS_INFO info;
    exchange_get_status(xchg, NULL, &info);
    cr_assert_neq(ntohl(info.last), 0, "Last price should be set");
    
    trader_logout(t1);
    trader_logout(t2);
    trader_logout(t3);
    close(pipe1[0]);
    close(pipe1[1]);
    close(pipe2[0]);
    close(pipe2[1]);
    close(pipe3[0]);
    close(pipe3[1]);
    sleep(1);
    exchange_fini(xchg);
    traders_fini();
    accounts_fini();
}