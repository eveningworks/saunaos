// ui/uvterm.h's reader thread, against a program that writes faster than
// the terminal drains: once the ring is full the reader PARKS on a futex
// rather than spinning on sys_yield() (which under SMP kept a core busy),
// and every byte still arrives, in order, once the host drains.
//
// It plays both halves: spawned with UVTERM_PARK_CHILD set (uvterm_spawn
// passes our environment on), it is the program on the pty.
//
// WHAT A BROKEN VERSION WOULD STILL PASS: a yield loop delivers every byte
// in order, so the parking is checked on its own -- state BLOCKED, wait
// FUTEX and almost no CPU across a window, as thread_test checks its
// locks. A park with no wake hangs, so the drain has a deadline. The byte
// order is read back from the GRID: the total is not a multiple of the
// width, and the pattern's period divides neither, so one lost or doubled
// byte puts the wrong letters on the last two rows.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "rt/sys.h"
#include "proc_info.h"
#include "ui/uvterm.h"
#include "base/utest.h"

#define SELF       "/tests/uvterm_park_test"
#define CHILD_ENV  "UVTERM_PARK_CHILD"
#define TOTAL      65536                 // eight rings' worth
#define COLS       50                    // TOTAL % COLS == 36
#define WINDOW_MS  300
#define CPU_MAX_NS (30ull * 1000 * 1000) // a tenth of the window

// Printable, and neither \n nor \t: the pty's ONLCR and TAB3 would
// rewrite those.
static char pattern(int i) { return (char)('!' + i % 89); }

static int child_main(void) {
    char buf[1024];
    for (int at = 0; at < TOTAL; ) {
        int n = 0;
        while (n < (int)sizeof buf && at + n < TOTAL) { buf[n] = pattern(at + n); n++; }
        if (sys_write(1, buf, (uint64_t)n) != n) return 1;
        at += n;
    }
    return 0;
}

static int ring_full(const struct uvterm *t) {
    unsigned head = __atomic_load_n(&t->head, __ATOMIC_ACQUIRE);
    return (head + 1) % UVTERM_RING_BYTES == __atomic_load_n(&t->tail, __ATOMIC_ACQUIRE);
}

// The reader: the one other thread in this process.
static int reader_slot(struct proc_info *out) {
    int me = sys_gettid(), pid = sys_getpid();
    for (int i = 0; sys_proc_info(i, out) == 0; i++)
        if (out->pid && out->tgid == pid && out->pid != me) return i;
    return -1;
}

// Does row `line` of the virtual buffer hold pattern[from .. from+n)?
static int row_holds(const struct uvterm *t, int line, int from, int n) {
    const struct uvterm_cell *r = uvterm_line(t, line);
    if (!r) return 0;
    for (int c = 0; c < n; c++)
        if (r[c].ch != (uint32_t)(unsigned char)pattern(from + c)) return 0;
    return 1;
}

static struct uvterm g_t;

int main(void) {
    if (getenv(CHILD_ENV)) return child_main();
    utest_begin("uvterm_park_test", "a terminal's reader parks while its ring is full",
                UTEST_VERDICT_FILE);

    if (!uvterm_init(&g_t, 4, COLS, 8, 7, 0)) {
        utest_check(0, "uvterm_init");
        return utest_end();
    }
    g_t.app = NULL;     // uapp_post() with no window queues and kicks nothing
    g_t.post_id = 1;
    setenv(CHILD_ENV, "1", 1);
    int spawned = uvterm_spawn(&g_t, SELF, NULL);
    unsetenv(CHILD_ENV);
    utest_check(spawned, "the writer started on a pty");
    if (!spawned) return utest_end();

    uint64_t until = sys_monotonic_ns() + 10ull * 1000 * 1000 * 1000;
    while (!ring_full(&g_t) && sys_monotonic_ns() < until) sys_sleep_ms(10);
    utest_check(ring_full(&g_t), "the ring fills while nobody drains it");

    sys_sleep_ms(50);                       // into the park
    struct proc_info a, b;
    int slot = reader_slot(&a);
    sys_sleep_ms(WINDOW_MS);
    int ok = slot >= 0 && sys_proc_info(slot, &b) == 0 && b.pid == a.pid;
    uint64_t used = ok ? b.cpu_ns - a.cpu_ns : 0;
    utest_checkf(ok && b.state == PROC_STATE_BLOCKED && b.wait_reason == PROC_WAIT_FUTEX &&
                 used < CPU_MAX_NS,
                 "the reader of a full ring is parked on a futex (state %u, wait %u, "
                 "%llu us of CPU in %d ms)", ok ? b.state : 0u, ok ? b.wait_reason : 0u,
                 (unsigned long long)(used / 1000), WINDOW_MS);

    // Drain as the Terminal does; each drain must wake the reader, or
    // this runs into its deadline with the writer stuck.
    until = sys_monotonic_ns() + 30ull * 1000 * 1000 * 1000;
    while (!__atomic_load_n(&g_t.done, __ATOMIC_ACQUIRE) && sys_monotonic_ns() < until) {
        if (!uvterm_drain(&g_t)) sys_sleep_ms(5);
    }
    uvterm_drain(&g_t);
    utest_check(g_t.done, "a drain wakes the parked reader, and it reads to the end");

    int last = g_t.sb_count + g_t.cr, tail = TOTAL % COLS;
    utest_checkf(g_t.cc == tail && row_holds(&g_t, last, TOTAL - tail, tail) &&
                 row_holds(&g_t, last - 1, TOTAL - tail - COLS, COLS),
                 "all %d bytes arrived in order (cursor at column %d, want %d)",
                 TOTAL, g_t.cc, tail);

    uvterm_stop(&g_t);
    uvterm_free(&g_t);
    return utest_end();
}
