// KTESTs for the kernel event trace (ktrace.h): the ring, the arming,
// per-core rings, the line formatter both rings share, and the panic
// tail. Every test arms only KTRACE_G_TEST, so the real trace points --
// which keep firing on every core while the suite runs -- write nothing
// into what a test counts.
#include "ktrace.h"
#include "ktrace_fmt.h"
#include "query_abi.h"
#include "scheduler.h"
#include "spinlock.h"   // spin_cpu_index()
#include "smp.h"
#include "cpuinfo.h"
#include "ktest.h"
#include "string.h"

#define BUSY() (ktrace_mask() != 0)

// Writes `n` test records on THIS core, numbered from `base`; the guard
// keeps them on one core and says which.
static int write_here(int n, uint64_t base) {
    scheduler_preempt_disable();
    int me = (int)spin_cpu_index();
    for (int i = 0; i < n; i++)
        KTRACE(KTRACE_G_TEST, KTRACE_EV_TEST, base + (uint64_t)i, 0x5A, 0, 0);
    scheduler_preempt_enable();
    return me;
}

static int count_test(int cpu, uint64_t *first_a0, uint64_t *last_a0, int *wrong_cpu) {
    struct ktrace_rec buf[16];
    uint64_t seq = 0, first = 0;
    int n = 0, got;
    while ((got = ktrace_read(cpu, seq, buf, 16, &first)) > 0) {
        for (int i = 0; i < got; i++) {
            if (buf[i].event != KTRACE_EV_TEST) continue;
            if (buf[i].cpu != cpu && wrong_cpu) (*wrong_cpu)++;
            if (!n && first_a0) *first_a0 = buf[i].a[0];
            if (last_a0) *last_a0 = buf[i].a[0];
            n++;
        }
        seq = first + (uint64_t)got;
    }
    return n;
}

KTEST("ktrace", "disarmed, a trace point writes nothing, and armed it writes only its groups") {
    if (BUSY()) KTEST_SKIP("a trace is running");
    if (!cpu_tsc_hz()) KTEST_SKIP("no calibrated TSC rate");
    uint64_t size = ktrace_size();
    KTEST_ASSERT(ktrace_set_size(64));
    KTEST_ASSERT(ktrace_start(KTRACE_G_TEST));
    ktrace_stop();
    KTEST_ASSERT_EQ(g_ktrace_mask, 0);
    int me = write_here(10, 0);   // stopped: the rings stay, nothing lands
    struct query_ktrace_cpu info;
    KTEST_ASSERT(ktrace_cpu_info(me, &info));
    KTEST_ASSERT_EQ(info.written, 0);
    // Armed for the test group only: another group's point stays silent.
    KTEST_ASSERT(ktrace_start(KTRACE_G_TEST));
    scheduler_preempt_disable();
    me = (int)spin_cpu_index();
    KTRACE(KTRACE_G_SWITCH, KTRACE_EV_TEST, 1, 2, 3, 4);
    KTRACE(KTRACE_G_TEST, KTRACE_EV_TEST, 7, 8, 9, 10);
    scheduler_preempt_enable();
    ktrace_stop();
    ktrace_set_size(size);
    uint64_t a0 = 0;
    KTEST_ASSERT_EQ(count_test(me, &a0, 0, 0), 1);
    KTEST_ASSERT_EQ(a0, 7);
}

KTEST("ktrace", "a full ring keeps the newest records, in order, and says how many it lost") {
    if (BUSY()) KTEST_SKIP("a trace is running");
    if (!cpu_tsc_hz()) KTEST_SKIP("no calibrated TSC rate");
    uint64_t size = ktrace_size();
    KTEST_ASSERT(ktrace_set_size(64));
    KTEST_ASSERT(ktrace_start(KTRACE_G_TEST));
    int me = write_here(200, 1000);
    ktrace_stop();
    ktrace_set_size(size);
    struct query_ktrace_cpu info;
    KTEST_ASSERT(ktrace_cpu_info(me, &info));
    KTEST_ASSERT_EQ(info.capacity, 64);
    KTEST_ASSERT_EQ(info.written, 200);
    KTEST_ASSERT_EQ(info.lost, 136);
    static struct ktrace_rec buf[64];   // 3 KiB: not on a kernel stack
    uint64_t first = 0;
    KTEST_ASSERT_EQ(ktrace_read(me, 0, buf, 64, &first), 64);
    KTEST_ASSERT_EQ(first, 136);   // the oldest that survived
    for (int i = 0; i < 64; i++) {
        KTEST_ASSERT_EQ(buf[i].a[0], 1136 + i);
        KTEST_ASSERT_EQ(buf[i].cpu, me);
        if (i) KTEST_ASSERT(buf[i].tsc >= buf[i - 1].tsc);
    }
    KTEST_ASSERT_EQ(ktrace_read(me, 200, buf, 64, &first), 0);   // nothing past the newest
}

KTEST("ktrace", "a size is rounded up to a power of two and refused out of range") {
    uint64_t size = ktrace_size();
    KTEST_ASSERT(ktrace_set_size(100));
    KTEST_ASSERT_EQ(ktrace_size(), 128);
    KTEST_ASSERT(!ktrace_set_size(10));
    KTEST_ASSERT(!ktrace_set_size(1ull << 40));
    KTEST_ASSERT_EQ(ktrace_size(), 128);
    ktrace_set_size(size);
}

static volatile int g_there_core = -1;
static void write_there(void *arg) {
    (void)arg;
    g_there_core = (int)spin_cpu_index_raw();   // a call runs with interrupts off
    for (int i = 0; i < 7; i++)
        KTRACE(KTRACE_G_TEST, KTRACE_EV_TEST, 5000 + i, 0, 0, 0);
}

KTEST("smp", "each core writes only its own trace ring") {
    if (smp_cpu_count() < 2) KTEST_SKIP("one core");
    if (BUSY()) KTEST_SKIP("a trace is running");
    if (!cpu_tsc_hz()) KTEST_SKIP("no calibrated TSC rate");
    uint64_t size = ktrace_size();
    KTEST_ASSERT(ktrace_set_size(64));
    KTEST_ASSERT(ktrace_start(KTRACE_G_TEST));
    int me = write_here(10, 100);
    int other = me == 1 ? 0 : 1;
    g_there_core = -1;
    int called = smp_call(other, write_there, 0, 1);
    ktrace_stop();
    ktrace_set_size(size);
    KTEST_ASSERT(called);
    KTEST_ASSERT_EQ(g_there_core, other);
    int wrong = 0;
    uint64_t a0 = 0, last = 0;
    KTEST_ASSERT_EQ(count_test(me, &a0, &last, &wrong), 10);
    KTEST_ASSERT_EQ(a0, 100);
    KTEST_ASSERT_EQ(last, 109);
    KTEST_ASSERT_EQ(count_test(other, &a0, &last, &wrong), 7);
    KTEST_ASSERT_EQ(a0, 5000);
    KTEST_ASSERT_EQ(last, 5006);
    KTEST_ASSERT_EQ(wrong, 0);
    for (int i = 0; i < smp_cpu_count(); i++)
        if (i != me && i != other) KTEST_ASSERT_EQ(count_test(i, 0, 0, 0), 0);
}

static const char *fake_name(unsigned nr) { return nr == 3 ? "read" : 0; }

KTEST("ktrace", "a record prints as one line, named from the event table") {
    struct ktrace_fmt_ctx c = { .t0_tsc = 1000, .tsc_hz = 1000000, .syscall_name = fake_name };
    struct ktrace_rec r = { .tsc = 1000 + 2500000, .event = KTRACE_EV_SWITCH, .cpu = 1, .tid = 12,
                            .a = { 12, KTRACE_STATE_BLOCKED, 13, 13 } };
    char line[160];
    KTEST_ASSERT(ktrace_fmt_rec(line, sizeof line, &r, &c) > 0);
    KTEST_ASSERT_EQ(k_strcmp(line, "    2.500000 c1 [12] switch prev=12 prev_state=S next=13 next_tgid=13"), 0);
    r.event = KTRACE_EV_SYS_EXIT;
    r.a[0] = 3; r.a[1] = (uint64_t)-11;
    KTEST_ASSERT(ktrace_fmt_rec(line, sizeof line, &r, &c) > 0);
    KTEST_ASSERT_EQ(k_strcmp(line, "    2.500000 c1 [12] sys_exit nr=read ret=-11"), 0);
    r.event = KTRACE_EV_SPIN_WAIT;
    r.a[0] = 0xff00; r.a[1] = 3000; r.a[2] = 0x10;   // 3000 ticks at 1 MHz: 3 ms
    KTEST_ASSERT(ktrace_fmt_rec(line, sizeof line, &r, &c) > 0);
    KTEST_ASSERT_EQ(k_strcmp(line, "    2.500000 c1 [12] spin_wait lock=0xff00 wait=3000.000 us site=0x10"), 0);
    r.event = 999;
    KTEST_ASSERT(ktrace_fmt_rec(line, sizeof line, &r, &c) > 0);
    KTEST_ASSERT(k_strncmp(line + 21, "event999 0xff00", 15) == 0);
    // What does not fit writes nothing, as kfmt's rule says.
    KTEST_ASSERT_EQ(ktrace_fmt_rec(line, 20, &r, &c), 0);
    KTEST_ASSERT_EQ(line[0], 0);
}

KTEST("ktrace", "group names parse both ways, and an unknown one is refused") {
    uint32_t m = 0;
    KTEST_ASSERT(ktrace_parse_groups("switch,syscall", &m));
    KTEST_ASSERT_EQ(m, KTRACE_G_SWITCH | KTRACE_G_SYSCALL);
    KTEST_ASSERT(ktrace_parse_groups("all", &m));
    KTEST_ASSERT_EQ(m, KTRACE_G_ALL);
    KTEST_ASSERT(!ktrace_parse_groups("switch,bogus", &m));
    KTEST_ASSERT(!ktrace_parse_groups("switch,,irq", &m));
    KTEST_ASSERT(!ktrace_parse_groups("", &m));
    KTEST_ASSERT(!ktrace_parse_groups("test", &m));   // never armed by name
    char out[64];
    KTEST_ASSERT(ktrace_format_groups(KTRACE_G_IRQ | KTRACE_G_MUTEX, out, sizeof out) > 0);
    KTEST_ASSERT_EQ(k_strcmp(out, "irq,mutex"), 0);
    KTEST_ASSERT(ktrace_format_groups(0, out, sizeof out) > 0);
    KTEST_ASSERT_EQ(k_strcmp(out, "off"), 0);
    KTEST_ASSERT(ktrace_format_groups(KTRACE_G_ALL, out, sizeof out) > 0);
    KTEST_ASSERT_EQ(k_strcmp(out, "all"), 0);
}

KTEST("ktrace", "the panic tail decodes each core's newest records, and disarms the trace") {
    if (BUSY()) KTEST_SKIP("a trace is running");
    if (!cpu_tsc_hz()) KTEST_SKIP("no calibrated TSC rate");
    KTEST_ASSERT_EQ(ktrace_panic_reserve(), 0);   // nothing armed: nothing kept
    uint64_t size = ktrace_size();
    KTEST_ASSERT(ktrace_set_size(64));
    KTEST_ASSERT(ktrace_start(KTRACE_G_TEST));
    write_here(50, 700);
    KTEST_ASSERT(ktrace_panic_reserve() > 0);
    static char text[12288];
    uint32_t len = ktrace_panic_tail(text, sizeof text - 1);
    int was_armed = g_ktrace_mask != 0;
    ktrace_stop();
    ktrace_set_size(size);
    KTEST_ASSERT(!was_armed);
    KTEST_ASSERT(len > 0 && len < sizeof text);
    text[len] = 0;
    // The newest survives, a record older than the tail's count does not.
    const char *newest = "test a0=749 a1=90";
    const char *oldest = "test a0=700 ";
    int has_newest = 0, has_oldest = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (!k_strncmp(text + i, newest, k_strlen(newest))) has_newest = 1;
        if (!k_strncmp(text + i, oldest, k_strlen(oldest))) has_oldest = 1;
    }
    KTEST_ASSERT(has_newest);
    KTEST_ASSERT(!has_oldest);
    KTEST_ASSERT(!k_strncmp(text, "ktrace: the last ", 17));
}
