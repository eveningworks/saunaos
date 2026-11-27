// uchan: a message channel between two ring-3 processes, over shared
// memory, a futex and a wakeword -- with no kernel support of its own.
//
// The shape that can see it working is TWO PROCESSES, because one
// process talking to itself proves nothing about the wakeup: it never
// parks, so a channel that woke nobody would pass. This spawns itself
// as a client and serves it.
//
// WHAT EACH CHECK IS FOR, since several look alike: that a message
// arrives at all; that ASYNC really is async (the sender does not wait);
// that the server can be WOKEN OUT OF A PARK by a message rather than
// finding it on a poll; that a round trip gets an answer back; and that
// a full ring is reported rather than silently dropping; that a
// death the scan's `gone` array cannot hold is reported LATER, not lost;
// and that ONE PROCESS'S TWO CLIENTS are two connections -- each answered
// on its own ring, the first still working beside the second (a second
// open used to replace the first, and privd lost the desktop's agent).
#include <stdio.h>
#include <string.h>
#include "rt/sys.h"
#include "syscall_abi.h"
#include "base/uchan.h"
#include "base/utest.h"

#define SERVICE "chantest"

struct msg {
    uint32_t kind;      // 1 = note, 2 = please answer
    uint32_t value;
    char     text[32];
};

enum { KIND_NOTE = 1, KIND_ASK = 2 };

static int child_main(void) {
    struct uchan_client c;
    // The server may not have published yet; a client that gave up on
    // the first miss would make this a race rather than a test.
    for (int i = 0; i < 200; i++) {
        if (uchan_client_open(&c, SERVICE) == 0) break;
        sys_sleep_ms(10);
        if (i == 199) return 2;
    }

    struct msg m;
    memset(&m, 0, sizeof m);
    m.kind = KIND_NOTE;
    m.value = 41;
    snprintf(m.text, sizeof m.text, "hello");
    if (uchan_send(&c, &m, sizeof m) < 0) return 3;

    // Let the server park, so the NEXT message has to wake it rather
    // than being found by a poll. That is the check this delay exists
    // for; without it the test cannot tell the two apart.
    sys_sleep_ms(300);
    m.value = 42;
    if (uchan_send(&c, &m, sizeof m) < 0) return 4;

    struct msg reply;
    memset(&m, 0, sizeof m);
    m.kind = KIND_ASK;
    m.value = 7;
    if (uchan_call(&c, &m, sizeof m, &reply, sizeof reply, 5000) < 0) return 5;
    if (reply.value != 8) return 6;

    uchan_client_close(&c);
    return 0;
}

// Two clients in one process: each asks once and must get its OWN
// answer; then the second closes and the first must still be heard.
static int two_main(void) {
    struct uchan_client a, b;
    for (int i = 0; i < 200 && uchan_client_open(&a, SERVICE) != 0; i++) sys_sleep_ms(10);
    if (uchan_client_open(&b, SERVICE) != 0) return 2;
    struct msg m, reply;
    memset(&m, 0, sizeof m);
    m.kind = KIND_ASK;
    m.value = 100;
    if (uchan_call(&a, &m, sizeof m, &reply, sizeof reply, 5000) < 0 || reply.value != 101) return 3;
    m.value = 200;
    if (uchan_call(&b, &m, sizeof m, &reply, sizeof reply, 5000) < 0 || reply.value != 201) return 4;
    uchan_client_close(&b);
    m.value = 300;
    if (uchan_call(&a, &m, sizeof m, &reply, sizeof reply, 5000) < 0 || reply.value != 301) return 5;
    uchan_client_close(&a);
    return 0;
}

// Connects and waits to be killed -- a client for the deaths check.
static int idle_main(void) {
    struct uchan_client c;
    for (int i = 0; i < 200 && uchan_client_open(&c, SERVICE) != 0; i++) sys_sleep_ms(10);
    sys_sleep_ms(60000);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "child")) return child_main();
    if (argc > 1 && !strcmp(argv[1], "idle")) return idle_main();
    if (argc > 1 && !strcmp(argv[1], "two")) return two_main();

    utest_begin("chan_test", "a message channel between two processes",
                UTEST_VERDICT_FILE);

    struct uchan_server s;
    utest_check(uchan_server_open(&s, SERVICE) == 0, "the server publishes");

    int pid = sys_spawn("/tests/chan_test", "child", -1);
    utest_check(pid > 0, "the client spawns");
    if (pid <= 0) return utest_end();

    int got_note = 0, got_41 = 0, got_42 = 0, answered = 0, woke_from_park = 0;
    struct msg m;

    for (int frame = 0; frame < 400 && answered < 1; frame++) {
        uchan_server_scan(&s, 0, 0);

        // Park. After the first message this returns only when the
        // client sends again -- which is the wakeup under test.
        uint64_t before = sys_monotonic_ns();
        uchan_server_wait(&s, 1000);
        uint64_t waited_ms = (sys_monotonic_ns() - before) / 1000000ull;

        int from;
        while ((from = uchan_server_recv(&s, &m, sizeof m)) != 0) {
            if (m.kind == KIND_NOTE) {
                got_note++;
                if (m.value == 41 && !strcmp(m.text, "hello")) got_41 = 1;
                if (m.value == 42) {
                    got_42 = 1;
                    // The second note came 300 ms after the first, so
                    // the server was parked when it arrived -- and it
                    // came back well inside the 1000 ms deadline, which
                    // is what says a MESSAGE woke it rather than the
                    // timeout.
                    if (waited_ms >= 100 && waited_ms < 900) woke_from_park = 1;
                }
            } else if (m.kind == KIND_ASK) {
                struct msg r;
                memset(&r, 0, sizeof r);
                r.kind = m.kind;
                r.value = m.value + 1;
                uchan_server_reply(&s, from, &r, sizeof r);
                answered++;
            }
        }
    }

    utest_checkf(got_note == 2, "both notes arrive (got %d)", got_note);
    utest_check(got_41, "the first note's payload survives the ring");
    utest_check(got_42, "the second note arrives too");
    utest_check(woke_from_park,
                "a message WAKES a parked server, well inside its deadline");
    utest_checkf(answered == 1, "the round trip is answered (%d)", answered);

    int code = 0;
    utest_check(sys_waitpid(pid, &code) == pid, "the client is reaped");
    utest_checkf(code == 0, "the client saw its reply (exit %d)", code);

    // A FULL RING IS REPORTED, not swallowed. Done from here with no
    // server draining, because that is the only way to fill one.
    struct uchan_client self;
    if (uchan_client_open(&self, SERVICE) == 0) {
        struct msg junk;
        memset(&junk, 0, sizeof junk);
        int sent = 0;
        while (uchan_send(&self, &junk, sizeof junk) == 0 && sent < UCHAN_SLOTS * 4) sent++;
        utest_checkf(sent == UCHAN_SLOTS,
                     "a full ring refuses rather than dropping (took %d of %d)",
                     sent, UCHAN_SLOTS);
        uchan_client_close(&self);
    }

    // MORE DEATHS THAN `gone` HOLDS, between two scans: every one must be
    // reported eventually. A scan that dropped the overflow's rings
    // unreported lost those deaths for good (and the compositor's
    // windows with them).
    enum { IDLERS = 3 };
    int idle[IDLERS];
    for (int i = 0; i < IDLERS; i++) idle[i] = sys_spawn("/tests/chan_test", "idle", -1);
    for (int t = 0; t < 200 && s.count < IDLERS; t++) {
        uchan_server_scan(&s, 0, 0);
        if (s.count < IDLERS) sys_sleep_ms(10);
    }
    utest_checkf(s.count == IDLERS, "three idle clients connect (%d)", s.count);
    for (int i = 0; i < IDLERS; i++) {
        sys_kill(idle[i], SIGKILL);
        sys_waitpid(idle[i], &code);
    }
    int reported[IDLERS], nrep = 0;
    for (int t = 0; t < 200 && nrep < IDLERS; t++) {
        int one;
        int n = uchan_server_scan(&s, &one, 1);   // room for ONE death a scan
        if (n == 1 && nrep < IDLERS) reported[nrep++] = one;
        if (!n) sys_sleep_ms(10);
    }
    int all = nrep == IDLERS;
    for (int i = 0; i < IDLERS && all; i++) {
        int seen = 0;
        for (int j = 0; j < nrep; j++) seen |= reported[j] == idle[i];
        all = seen;
    }
    utest_checkf(all, "every death is reported, one a scan (%d of %d)", nrep, IDLERS);
    utest_checkf(s.count == 0, "...and every dead ring is reclaimed (%d left)", s.count);

    // TWO RINGS FROM ONE PROCESS, two connections.
    int two = sys_spawn("/tests/chan_test", "two", -1);
    int conns[3] = {0}, peers[3] = {0}, nasked = 0, both_open = 0;
    for (int t = 0; t < 400 && nasked < 3; t++) {
        uchan_server_scan(&s, 0, 0);
        if (s.count == 2) both_open = 1;
        uchan_server_wait(&s, 50);
        int from;
        while ((from = uchan_server_recv(&s, &m, sizeof m)) != 0) {
            if (m.kind != KIND_ASK || nasked >= 3) continue;
            conns[nasked] = from;
            peers[nasked++] = uchan_server_peer_pid(&s, from);
            struct msg r;
            memset(&r, 0, sizeof r);
            r.kind = m.kind;
            r.value = m.value + 1;
            uchan_server_reply(&s, from, &r, sizeof r);
        }
    }
    utest_checkf(nasked == 3 && both_open, "one process's two clients are both connected (%d asks)", nasked);
    utest_checkf(conns[0] == two && conns[1] >= UCHAN_CONN_EXTRA && conns[2] == two,
                 "...the first is its pid, the second an id past every pid (%d, %d, %d)",
                 conns[0], conns[1], conns[2]);
    utest_checkf(peers[0] == two && peers[1] == two, "...and both name it as the sender (%d, %d)",
                 peers[0], peers[1]);
    utest_check(two > 0 && sys_waitpid(two, &code) == two && code == 0,
                "...each answered on its own ring, the first heard after the second closed");

    // SEVENTY CLIENTS OF ONE SERVER, past the 64 the table held: every
    // ring is adopted, and the last one's message is heard. One process's
    // further rings are clients like any other, so no spawns are needed.
    enum { MANY = 70 };
    static struct uchan_client many[MANY];
    int opened = 0;
    for (int i = 0; i < MANY; i++) opened += uchan_client_open(&many[i], SERVICE) == 0;
    for (int t = 0; t < 200 && s.count < MANY; t++) {
        uchan_server_scan(&s, 0, 0);
        if (s.count < MANY) sys_sleep_ms(10);
    }
    utest_checkf(opened == MANY && s.count == MANY, "seventy clients connect (%d opened, %d adopted)",
                 opened, s.count);
    memset(&m, 0, sizeof m);
    m.kind = KIND_NOTE;
    m.value = 70;
    int heard = 0;
    if (opened == MANY && uchan_send(&many[MANY - 1], &m, sizeof m) == 0) {
        int from;
        while ((from = uchan_server_recv(&s, &m, sizeof m)) != 0)
            if (m.value == 70 && uchan_server_peer_pid(&s, from) == sys_getpid()) heard = 1;
    }
    utest_check(heard, "...and the seventieth client's message arrives");
    for (int i = 0; i < MANY; i++) uchan_client_close(&many[i]);
    for (int t = 0; t < 200 && s.count; t++) {
        uchan_server_scan(&s, 0, 0);
        if (s.count) sys_sleep_ms(10);
    }
    utest_checkf(s.count == 0, "...and all seventy rings are dropped when closed (%d left)", s.count);

    uchan_server_close(&s);
    return utest_end();
}
