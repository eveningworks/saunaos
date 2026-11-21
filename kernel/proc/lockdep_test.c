// KTESTs for lock-order checking (lockdep.h). Each provokes one mistake
// between locks of its own named classes and asserts lockdep reported it
// ONCE, naming them -- and forgets those classes after, so the running
// kernel's readout carries nothing from a test. Skipped in a kernel
// built without `option lockdep`.
//
// The kmutexes are never contended: the kernel context has no slot to
// sleep in, and an inversion is reported at the take, before any wait.
#include "ktest.h"
#include "lockdep.h"
#include "spinlock.h"
#include "kmutex.h"
#include "irqflags.h"
#include "string.h"

#ifdef SAUNAOS_LOCKDEP
// Reports naming class `name` (as a or b) of `kind`, 0 for any.
static int reports_naming(const char *name, int kind) {
    int n = 0;
    struct lockdep_report r;
    char a[40], b[40];
    for (int i = 0; lockdep_report_get(i, &r); i++) {
        if (kind && r.kind != kind) continue;
        a[0] = b[0] = 0;
        if (r.a) lockdep_class_name(r.a, a, sizeof a);
        if (r.b) lockdep_class_name(r.b, b, sizeof b);
        if (k_strncmp(a, name, k_strlen(name)) == 0 || k_strncmp(b, name, k_strlen(name)) == 0) n++;
    }
    return n;
}

static int last_names(const char *name, const char *x, const char *y) {
    struct lockdep_report r;
    char a[40] = "", b[40] = "";
    for (int i = lockdep_report_count() - 1; i >= 0; i--) {
        if (!lockdep_report_get(i, &r)) continue;
        if (r.a) lockdep_class_name(r.a, a, sizeof a);
        if (r.b) lockdep_class_name(r.b, b, sizeof b);
        if (k_strncmp(a, name, k_strlen(name)) && k_strncmp(b, name, k_strlen(name))) continue;
        return k_strcmp(a, x) == 0 && k_strcmp(b, y) == 0;
    }
    return 0;
}
#endif

KTEST("lockdep", "an A then B, B then A inversion between kmutexes is reported once, naming both") {
#ifndef SAUNAOS_LOCKDEP
    KTEST_SKIP("built without option lockdep");
#else
    struct kmutex a = {0}, b = {0};
    kmutex_name(&a, "ktest-inv-A");
    kmutex_name(&b, "ktest-inv-B");
    kmutex_lock(&a); kmutex_lock(&b); kmutex_unlock(&b); kmutex_unlock(&a);
    KTEST_ASSERT_EQ(reports_naming("ktest-inv-", 0), 0);   // one order is no mistake
    for (int round = 0; round < 3; round++) {
        kmutex_lock(&b); kmutex_lock(&a); kmutex_unlock(&a); kmutex_unlock(&b);
    }
    int n = reports_naming("ktest-inv-", LOCKDEP_INVERSION);
    int named = last_names("ktest-inv-", "ktest-inv-B", "ktest-inv-A");   // holding B, taking A
    lockdep_forget("ktest-inv-A");
    lockdep_forget("ktest-inv-B");
    KTEST_ASSERT_EQ(n, 1);
    KTEST_ASSERT(named);
#endif
}

KTEST("lockdep", "the same inversion between spinlocks is reported once") {
#ifndef SAUNAOS_LOCKDEP
    KTEST_SKIP("built without option lockdep");
#else
    struct spinlock a = {0}, b = {0};
    spin_lock_name(&a, "ktest-spin-A");
    spin_lock_name(&b, "ktest-spin-B");
    uint64_t f = spin_lock_irqsave(&a); spin_lock(&b); spin_unlock(&b); spin_unlock_irqrestore(&a, f);
    f = spin_lock_irqsave(&b); spin_lock(&a); spin_unlock(&a); spin_unlock_irqrestore(&b, f);
    f = spin_lock_irqsave(&b); spin_lock(&a); spin_unlock(&a); spin_unlock_irqrestore(&b, f);
    int n = reports_naming("ktest-spin-", LOCKDEP_INVERSION);
    int named = last_names("ktest-spin-", "ktest-spin-B", "ktest-spin-A");
    lockdep_forget("ktest-spin-A");
    lockdep_forget("ktest-spin-B");
    KTEST_ASSERT_EQ(n, 1);
    KTEST_ASSERT(named);
#endif
}

KTEST("lockdep", "a three-class cycle is reported once, with its whole path") {
#ifndef SAUNAOS_LOCKDEP
    KTEST_SKIP("built without option lockdep");
#else
    struct kmutex a = {0}, b = {0}, c = {0};
    kmutex_name(&a, "ktest-cyc-A");
    kmutex_name(&b, "ktest-cyc-B");
    kmutex_name(&c, "ktest-cyc-C");
    kmutex_lock(&a); kmutex_lock(&b); kmutex_unlock(&b); kmutex_unlock(&a);   // A -> B
    kmutex_lock(&b); kmutex_lock(&c); kmutex_unlock(&c); kmutex_unlock(&b);   // B -> C
    int before = reports_naming("ktest-cyc-", 0);
    kmutex_lock(&c); kmutex_lock(&a); kmutex_unlock(&a); kmutex_unlock(&c);   // C -> A: closes it
    int n = reports_naming("ktest-cyc-", LOCKDEP_INVERSION);
    struct lockdep_report r = {0};
    for (int i = lockdep_report_count() - 1; i >= 0; i--)
        if (lockdep_report_get(i, &r) && r.kind == LOCKDEP_INVERSION) break;
    lockdep_forget("ktest-cyc-A");
    lockdep_forget("ktest-cyc-B");
    lockdep_forget("ktest-cyc-C");
    KTEST_ASSERT_EQ(before, 0);
    KTEST_ASSERT_EQ(n, 1);
    KTEST_ASSERT_EQ(r.path_len, 2);   // A -> B -> C, the order C -> A reverses
#endif
}

KTEST("lockdep", "two of one class held together is a report, and none with a subclass") {
#ifndef SAUNAOS_LOCKDEP
    KTEST_SKIP("built without option lockdep");
#else
    struct kmutex p = {0}, q = {0};
    kmutex_name(&p, "ktest-nest");
    kmutex_name(&q, "ktest-nest");
    // Annotated: parent as subclass 0, child as 1 -- an order, not a recursion.
    kmutex_lock(&p); kmutex_lock_nested(&q, 1); kmutex_unlock(&q); kmutex_unlock(&p);
    int annotated = reports_naming("ktest-nest", 0);
    // The same nest unannotated.
    kmutex_lock(&p); kmutex_lock(&q); kmutex_unlock(&q); kmutex_unlock(&p);
    int bare = reports_naming("ktest-nest", LOCKDEP_RECURSION);
    // And the annotated order reversed is an inversion between subclasses.
    kmutex_lock_nested(&q, 1); kmutex_lock(&p); kmutex_unlock(&p); kmutex_unlock(&q);
    int reversed = reports_naming("ktest-nest", LOCKDEP_INVERSION);
    // A recursive take of the SAME lock is the lock's own business.
    kmutex_lock(&p); kmutex_lock(&p); kmutex_unlock(&p); kmutex_unlock(&p);
    int total = reports_naming("ktest-nest", 0);
    lockdep_forget("ktest-nest");
    KTEST_ASSERT_EQ(annotated, 0);
    KTEST_ASSERT_EQ(bare, 1);
    KTEST_ASSERT_EQ(reversed, 1);
    KTEST_ASSERT_EQ(total, 2);
#endif
}

KTEST("lockdep", "many of one class under a nest lock is no report, and releases match") {
#ifndef SAUNAOS_LOCKDEP
    KTEST_SKIP("built without option lockdep");
#else
    struct kmutex outer = {0}, m[4] = {0};
    kmutex_name(&outer, "ktest-nl-outer");
    for (int i = 0; i < 4; i++) kmutex_name(&m[i], "ktest-nl-each");
    struct lockdep_stats s0, s1;
    lockdep_get_stats(&s0);
    kmutex_lock(&outer);
    for (int i = 0; i < 4; i++) kmutex_lock_nest_lock(&m[i], &outer);
    for (int i = 3; i >= 0; i--) kmutex_unlock(&m[i]);
    kmutex_unlock(&outer);
    lockdep_get_stats(&s1);
    int n = reports_naming("ktest-nl-", 0);
    lockdep_forget("ktest-nl-outer");
    lockdep_forget("ktest-nl-each");
    KTEST_ASSERT_EQ(n, 0);
    KTEST_ASSERT_EQ(s1.unmatched, s0.unmatched);
#endif
}

// fs_exclusive_begin()'s shape: released by slot, not in reverse. The last
// release must still find the shared entry, or the task keeps a stale one
// and its next lock of the class reads as a recursion.
KTEST("lockdep", "many under a nest lock released in the order taken leave nothing held") {
#ifndef SAUNAOS_LOCKDEP
    KTEST_SKIP("built without option lockdep");
#else
    struct kmutex outer = {0}, m[4] = {0}, later = {0};
    kmutex_name(&outer, "ktest-nf-outer");
    for (int i = 0; i < 4; i++) kmutex_name(&m[i], "ktest-nf-each");
    kmutex_name(&later, "ktest-nf-each");
    struct lockdep_stats s0, s1;
    lockdep_get_stats(&s0);
    kmutex_lock(&outer);
    for (int i = 0; i < 4; i++) kmutex_lock_nest_lock(&m[i], &outer);
    for (int i = 0; i < 4; i++) kmutex_unlock(&m[i]);
    kmutex_unlock(&outer);
    kmutex_lock(&later);          // one of the class, alone: no recursion
    kmutex_unlock(&later);
    lockdep_get_stats(&s1);
    int n = reports_naming("ktest-nf-", 0);
    lockdep_forget("ktest-nf-outer");
    lockdep_forget("ktest-nf-each");
    KTEST_ASSERT_EQ(s1.unmatched, s0.unmatched);
    KTEST_ASSERT_EQ(n, 0);
#endif
}

KTEST("lockdep", "a spinlock taken in an interrupt and with interrupts on is reported once") {
#ifndef SAUNAOS_LOCKDEP
    KTEST_SKIP("built without option lockdep");
#else
    struct spinlock l = {0};
    spin_lock_name(&l, "ktest-irq");
    // An interrupt as isr_dispatch() marks one: interrupts off, the depth raised.
    uint64_t f = irq_save();
    int was = lockdep_hardirq_enter();
    spin_lock(&l); spin_unlock(&l);
    lockdep_hardirq_exit(was);
    irq_restore(f);
    int in_irq_only = reports_naming("ktest-irq", 0);
    // irqsave is the right way: still nothing.
    f = spin_lock_irqsave(&l); spin_unlock_irqrestore(&l, f);
    int irqsave = reports_naming("ktest-irq", 0);
    // With interrupts on: the interrupt could spin on this core's holder.
    f = irq_save();
    __asm__ volatile ("sti");
    spin_lock(&l); spin_unlock(&l);
    spin_lock(&l); spin_unlock(&l);
    irq_restore(f);
    int n = reports_naming("ktest-irq", LOCKDEP_IRQ_INCONSISTENT);
    lockdep_forget("ktest-irq");
    KTEST_ASSERT_EQ(in_irq_only, 0);
    KTEST_ASSERT_EQ(irqsave, 0);
    KTEST_ASSERT_EQ(n, 1);
#endif
}

KTEST("lockdep", "a kmutex taken holding a spinlock, or in an interrupt, is reported") {
#ifndef SAUNAOS_LOCKDEP
    KTEST_SKIP("built without option lockdep");
#else
    struct spinlock s = {0};
    struct kmutex m = {0};
    spin_lock_name(&s, "ktest-sis-spin");
    kmutex_name(&m, "ktest-sis-mutex");
    // Free, so it does not sleep -- but it could have (kmutex.c's might_sleep() logs it too).
    uint64_t f = spin_lock_irqsave(&s);
    kmutex_lock(&m); kmutex_unlock(&m);
    spin_unlock_irqrestore(&s, f);
    int sis = reports_naming("ktest-sis-", LOCKDEP_SLEEP_IN_SPIN);
    int named = last_names("ktest-sis-", "ktest-sis-spin", "ktest-sis-mutex");
    f = irq_save();
    int was = lockdep_hardirq_enter();
    kmutex_lock(&m); kmutex_unlock(&m);
    lockdep_hardirq_exit(was);
    irq_restore(f);
    int in_irq = reports_naming("ktest-sis-", LOCKDEP_KMUTEX_IN_IRQ);
    lockdep_forget("ktest-sis-spin");
    lockdep_forget("ktest-sis-mutex");
    KTEST_ASSERT_EQ(sis, 1);
    KTEST_ASSERT(named);
    KTEST_ASSERT_EQ(in_irq, 1);
#endif
}

KTEST("lockdep", "the running kernel's locks are classes, and nothing it took was unnamed") {
#ifndef SAUNAOS_LOCKDEP
    KTEST_SKIP("built without option lockdep");
#else
    struct lockdep_stats s;
    lockdep_get_stats(&s);
    KTEST_ASSERT(s.classes > 3);     // the heap, the frames, mounts... by now
    KTEST_ASSERT(s.checked > 0);
    KTEST_ASSERT_EQ(s.reports_lost, 0);
    KTEST_ASSERT_EQ(reports_naming("", LOCKDEP_UNNAMED), 0);
#endif
}
