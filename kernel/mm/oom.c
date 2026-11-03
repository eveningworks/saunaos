// The out-of-memory killer -- see kernel/oom.h for what it is and is not.
//
// The shape is Linux's: a fault that cannot get a frame chooses a
// victim by size, sends it SIGKILL and retries. What differs, and why:
//
//   * NO oom_reaper. A victim parked mid-call dies on its way out of the
//     kernel (scheduler_kill()'s deferred death), so its frames come
//     back late; the faulter WAITS for them, in short bounded sleeps,
//     and only after a grace period chooses somebody else.
//   * NO per-process score adjustment. Three kinds of process are never
//     chosen -- init, a kernel thread, the compositor -- and everything
//     else is ranked by resident user memory alone.
#include "oom.h"
#include "pmm.h"
#include "vmm.h"          // vmm_fault_in() -- the retry
#include "scheduler.h"
#include "signal.h"
#include "signal_abi.h"   // SIGKILL
#include "kthread.h"
#include "win_role.h"     // win_server_compositor_pid()
#include "proc_info.h"
#include "clocksource.h"
#include "heap.h"
#include "string.h"
#include "kfmt.h"
#include "klog.h"

#define MS(n) ((uint64_t)(n) * 1000000u)

// How often a waiting faulter looks again, how long a victim gets to
// leave the kernel before the next is chosen, and how long a faulter
// waits in all before it is ended itself.
#define OOM_POLL_NS   MS(10)
#define OOM_GRACE_NS  MS(2000)
#define OOM_GIVEUP_NS MS(10000)

// One byte per scheduler slot: "this context's last fault-in found no
// frame". Per slot because a syscall's fault-in can sleep on a disk
// read between the note and the take, and another context's note in
// that gap must not land on this one.
static uint8_t *g_noted;
static int g_noted_slots;

static uint64_t g_kills;
static int g_last_pid;
static char g_last_name[PROC_NAME_MAX];
static uint64_t g_last_bytes;

// The victim still on its way out, and when it stops being waited for.
static int g_victim;
static uint64_t g_victim_until;

static int g_kill_allocating;

static int current_slot(void) {
    int pid = scheduler_current_pid();
    return pid ? scheduler_pid_slot(pid) : -1;
}

void oom_note(void) {
    if (!g_noted) {
        int n = scheduler_max_procs();
        g_noted = kzalloc((size_t)n);   // a kernel allocation: not held to the user reserve
        if (!g_noted) return;           // then a fault-in failure reads as a crash, as before
        g_noted_slots = n;
    }
    int s = current_slot();
    if (s >= 0 && s < g_noted_slots) g_noted[s] = 1;
}

int oom_fault_take(void) {
    int s = current_slot();
    if (!g_noted || s < 0 || s >= g_noted_slots || !g_noted[s]) return 0;
    g_noted[s] = 0;
    return 1;
}

static int spared(int pid) {
    return pid == scheduler_init_pid() || kthread_is(pid) ||
           pid == win_server_compositor_pid();
}

// The live process holding the most user memory, or 0. Threads share
// their leader's address space, so only leaders are ranked.
static int choose(struct proc_info *out) {
    struct proc_info pi, best = {0};
    int n = scheduler_max_procs();
    for (int i = 0; i < n; i++) {
        if (!scheduler_proc_info(i, &pi) || !pi.pid) continue;
        if (pi.state == PROC_STATE_UNUSED || pi.state == PROC_STATE_ZOMBIE) continue;
        if (pi.tgid && pi.tgid != pi.pid) continue;
        if (spared(pi.pid) || pi.pid == g_victim) continue;
        if (!best.pid || pi.mem_bytes > best.mem_bytes) best = pi;
    }
    *out = best;
    return best.pid;
}

static void record(const struct proc_info *v, int me) {
    char me_name[PROC_NAME_MAX];
    scheduler_name_of(me, me_name, sizeof me_name);
    g_kills++;
    g_last_pid = v->pid;
    g_last_bytes = v->mem_bytes;
    k_strlcpy(g_last_name, v->name, sizeof g_last_name);
    klog_printf(KLOG_ERR "oom: out of memory -- ended pid %d (%s, %lu KiB) for pid %d (%s)\n",
                v->pid, v->name, v->mem_bytes / 1024, me, me_name);
}

// 1 room was made, 0 the current process must end, -1 cannot act here.
static int make_room(int me) {
    uint64_t start = clocksource_now_ns();
    for (;;) {
        if (pmm_user_room()) return 1;
        uint64_t now = clocksource_now_ns();
        if (now - start > OOM_GIVEUP_NS) {
            klog_printf(KLOG_ERR "oom: no memory came back in %lu ms -- pid %d ends\n",
                        OOM_GIVEUP_NS / MS(1), me);
            return 0;
        }

        int waiting = g_victim && now < g_victim_until && scheduler_pid_alive(g_victim);
        if (!waiting) {
            struct proc_info v;
            if (g_kill_allocating && !spared(me)) {
                if (!scheduler_proc_info_pid(me, &v)) return 0;
            } else if (!choose(&v)) {
                klog_printf(KLOG_ERR "oom: out of memory and nothing left to end -- pid %d ends\n", me);
                return 0;
            }
            record(&v, me);
            if (v.pid == me) return 0;
            g_victim = v.pid;
            g_victim_until = now + OOM_GRACE_NS;
            signal_send(v.pid, SIGKILL);
            continue;   // a victim not parked mid-call is gone already
        }

        // THE CHANNEL IS NEVER WOKEN: this is a poll, bounded by the timer.
        if (!scheduler_block_kernel_until(&g_victim, SCHED_WAIT_TIMER, now + OOM_POLL_NS))
            return -1;
    }
}

int oom_recover_fault(uint64_t pml4_phys, uint64_t vaddr, uint64_t err) {
    int me = scheduler_current_tgid();
    if (!me || scheduler_preempt_depth() > 0) return -1;

    // Bounded: room that somebody else takes before the retry sends this
    // round again, and a fault that keeps failing must not spin here.
    for (int round = 0; round < 16; round++) {
        int r = make_room(me);
        if (r < 0) return -1;
        if (r == 0) break;
        if (vmm_fault_in(pml4_phys, vaddr, err)) return 1;
        if (!oom_fault_take()) return -1;   // failed for some other reason
    }
    scheduler_signal_raise(scheduler_current_pid(), SIGKILL);
    return 0;
}

uint64_t oom_kills(void) { return g_kills; }

int oom_last_victim(int *pid, char *name, unsigned cap, uint64_t *bytes) {
    if (!g_last_pid) return 0;
    if (pid) *pid = g_last_pid;
    if (name && cap) k_strlcpy(name, g_last_name, cap);
    if (bytes) *bytes = g_last_bytes;
    return 1;
}

int oom_kill_allocating(void) { return g_kill_allocating; }
void oom_set_kill_allocating(int on) { g_kill_allocating = on ? 1 : 0; }
