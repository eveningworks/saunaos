// The hardware watchdog's core and /dev/watchdog -- see kernel/watchdog.h.
//
// LINUX'S DEVICE CONTRACT, so a feeder written for Linux feeds this one:
// the OPEN starts the timer, any WRITE feeds it, and a CLOSE stops it only
// when the last write held a 'V' (the "magic close"). Any other close
// leaves it counting, so a feeder that dies resets the machine -- which
// is the point. One opener at a time (-EBUSY).
#include "watchdog.h"
#include "syscalls.h"
#include "errno.h"
#include "vmm.h"
#include "klog.h"
#include "kfmt.h"
#include "knum.h"
#include "multiboot.h"
#include "irqflags.h"
#include "driver.h"
#include "scheduler.h"   // scheduler_current_pid() -- who opened it

static const struct hw_watchdog *g_wd;
static int g_secs;          // what start() was given
static int g_running;
static int g_open;          // a description holds it
static int g_magic;         // the last write held a 'V'
static int g_held;          // the kernel debugger has the machine stopped

int watchdog_parse_secs(const char *value, int *out) {
    if (!value) return 0;
    if (value[0] == 'o' && value[1] == 'f' && value[2] == 'f' && !value[3]) { *out = 0; return 1; }
    int64_t v;
    if (!k_parse_i64(value, &v) || v < 0) return 0;
    if (v > 3600) v = 3600;
    *out = (int)v;
    return 1;
}

// 0 = off. Clamped to the device, which is why this asks the driver.
static int configured_secs(void) {
    char v[16];
    int secs = WATCHDOG_DEFAULT_SECS;
    if (multiboot_cmdline_value("watchdog=", v, sizeof v)) watchdog_parse_secs(v, &secs);
    if (secs && secs < WATCHDOG_MIN_SECS) secs = WATCHDOG_MIN_SECS;
    if (secs && g_wd) {
        if (secs < g_wd->min_secs) secs = g_wd->min_secs;
        if (secs > g_wd->max_secs) secs = g_wd->max_secs;
    }
    return secs;
}

void watchdog_register(const struct hw_watchdog *w) {
    if (g_wd) {
        klog_printf("watchdog: %s ignored -- %s already registered\n", w->name, g_wd->name);
        return;
    }
    g_wd = w;
    driver_bound(w->name, "watchdog0");
    int secs = configured_secs();
    if (secs) klog_printf("watchdog: %s registered (%d..%d s); /dev/watchdog resets an unfed machine "
                          "after %d s\n", w->name, w->min_secs, w->max_secs, secs);
    else      klog_printf("watchdog: %s registered, OFF (watchdog=off)\n", w->name);
}

int watchdog_present(void) { return g_wd != 0; }
int watchdog_running(void) { return g_running; }

void watchdog_panic_ping(void) {
    if (g_wd && g_running) g_wd->ping();
}

void watchdog_panic_stop(void) {
    if (g_wd && g_running) { g_wd->stop(); g_running = 0; }
}

// A stop and a restart rather than a pause: the TCO has no pause, and
// restarting gives the machine the whole timeout after the debugger lets go.
void watchdog_debugger_hold(int stopped) {
    g_held = stopped;
    if (!g_wd || !g_running) return;
    if (stopped) g_wd->stop();
    else         g_wd->start(g_secs);
}

int watchdog_debugger_held(void) { return g_held; }

void watchdog_shutdown(void) {
    if (!g_wd || !g_running) return;
    g_wd->stop();
    g_running = 0;
    klog_write("watchdog: stopped for the shutdown\n");
}

// --- /dev/watchdog -----------------------------------------------------

int watchdog_claim(void) {
    if (!g_wd) return -ENOENT;
    if (!configured_secs()) return -EPERM;
    uint64_t f = irq_save();
    if (g_open) { irq_restore(f); return -EBUSY; }
    g_open = 1;
    irq_restore(f);
    return 0;
}

void watchdog_unclaim(void) { g_open = 0; }
void watchdog_open_failed(void) { g_magic = 1; }

static void wd_open(struct open_file *f, int aux) {
    (void)f; (void)aux;
    g_magic = 0;
    g_secs = configured_secs();
    if (!g_wd->start(g_secs)) {
        klog_printf(KLOG_ERR "watchdog: %s refused to start -- the machine is NOT watched\n",
                    g_wd->name);
        return;
    }
    g_running = 1;
    klog_printf("watchdog: started by pid %d -- a reset if it is not fed for %d s\n",
                scheduler_current_pid(), g_secs);
}

// The bytes are looked at only for the 'V'; what they say otherwise is
// nothing, as on Linux.
static int wd_write(struct syscall_ctx *c, struct open_file *f, uint64_t ubuf, uint64_t len) {
    (void)f;
    char buf[64];
    int magic = 0;
    for (uint64_t done = 0; done < len;) {
        uint64_t n = len - done < sizeof buf ? len - done : sizeof buf;
        if (!vmm_copy_from_user(c->pml4, buf, ubuf + done, n)) {
            c->regs[14] = (uint64_t)(int64_t)-EFAULT;
            return 0;
        }
        for (uint64_t i = 0; i < n; i++) if (buf[i] == 'V') magic = 1;
        done += n;
    }
    g_magic = magic;
    if (g_running) g_wd->ping();
    c->regs[14] = len;
    return 0;
}

static void wd_release(struct open_file *f) {
    (void)f;
    if (g_running && g_magic) {
        g_wd->stop();
        g_running = 0;
        klog_write("watchdog: stopped (closed with the magic 'V')\n");
    } else if (g_running) {
        klog_printf(KLOG_WARN "watchdog: closed WITHOUT the magic 'V' -- still counting; "
                    "the machine resets in %d s unless it is opened again\n", g_secs);
    }
    g_magic = 0;
    g_open = 0;
}

const struct fd_ops watchdog_fd_ops = { .name = "watchdog", .write = wd_write,
                                        .open = wd_open, .release = wd_release };
