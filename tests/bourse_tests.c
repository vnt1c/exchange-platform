#include <criterion/criterion.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <wait.h>

#include "client_registry.h"
#include "protocol.h"

static void init() {
#ifndef NO_SERVER
    int ret;
    int i = 0;
    do { // Wait for server to start
	ret = system("netstat -an | fgrep '0.0.0.0:9999' > /dev/null");
	sleep(1);
    } while(++i < 30 && WEXITSTATUS(ret));
#endif
}

static void fini() {
}

/*
 * Thread to run a command using system() and collect the exit status.
 */
void *system_thread(void *arg) {
    long ret = system((char *)arg);
    return (void *)ret;
}

// Criterion seems to sort tests by name.  This one can't be delayed
// or others will time out.
Test(student_suite, 00_start_server, .timeout = 30) {
    fprintf(stderr, "server_suite/00_start_server\n");
    int server_pid = 0;
    int ret = system("netstat -an | fgrep '0.0.0.0:9999' > /dev/null");
    cr_assert_neq(WEXITSTATUS(ret), 0, "Server was already running");
    fprintf(stderr, "Starting server...");
    if((server_pid = fork()) == 0) {
	execlp("valgrind", "bourse", "--leak-check=full", "--track-fds=yes",
	       "--error-exitcode=37", "--log-file=valgrind.out", "bin/bourse", "-p", "9999", NULL);
	fprintf(stderr, "Failed to exec server\n");
	abort();
    }
    fprintf(stderr, "pid = %d\n", server_pid);
    char *cmd = "sleep 10";
    pthread_t tid;
    pthread_create(&tid, NULL, system_thread, cmd);
    pthread_join(tid, NULL);
    cr_assert_neq(server_pid, 0, "Server was not started by this test");
    fprintf(stderr, "Sending SIGHUP to server pid %d\n", server_pid);
    kill(server_pid, SIGHUP);
    sleep(5);
    kill(server_pid, SIGKILL);
    wait(&ret);
    fprintf(stderr, "Server wait() returned = 0x%x\n", ret);
    if(WIFSIGNALED(ret)) {
	fprintf(stderr, "Server terminated with signal %d\n", WTERMSIG(ret));	
	system("cat valgrind.out");
	if(WTERMSIG(ret) == 9)
	    cr_assert_fail("Server did not terminate after SIGHUP");
    }
    if(WEXITSTATUS(ret) == 37)
	system("cat valgrind.out");
    cr_assert_neq(WEXITSTATUS(ret), 37, "Valgrind reported errors");
    cr_assert_eq(WEXITSTATUS(ret), 0, "Server exit status was not 0");
}

Test(student_suite, 01_connect, .init = init, .fini = fini, .timeout = 5) {
    fprintf(stderr, "server_suite/01_connect\n");
    int ret = system("util/client -p 9999 </dev/null | grep 'Connected to server'");
    cr_assert_eq(ret, 0, "expected %d, was %d\n", 0, ret);
}

/*
 * --- Helper thread for registry tests ---
 */
static void *register_and_unregister(void *arg) {
    CLIENT_REGISTRY *cr = arg;

    // Fake FD numbers
    int fd = (rand() % 30000) + 3;

    creg_register(cr, fd);
    usleep(5000);
    creg_unregister(cr, fd);

    return NULL;
}

Test(student_extra, client_registry_basic, .timeout = 5) {
    CLIENT_REGISTRY *cr = creg_init();
    cr_assert_not_null(cr);

    int fd1 = 10;
    int fd2 = 11;

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

    // Should block until t1 and t2 have fully unregistered
    creg_wait_for_empty(cr);

    // If we reached here, the registry emptied correctly
    cr_assert(1);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    creg_fini(cr);
}

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