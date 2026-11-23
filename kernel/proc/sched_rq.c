// The run queues: one per core, Linux's struct rq, and one global list
// for the realtime class (docs/smp-design.md, stage 7, "per-core run
// queues (2b)"). Split from scheduler.c, which picks and switches; this
// file owns the queues, their locks, where a slot goes and moving one.
//
// **WHO MAY WRITE WHAT** -- the rule every caller keeps:
//   - READY <-> RUNNING on a core: that core's queue lock alone (the
//     pick, a rotation's put-back), Linux's on_rq under the rq lock;
//   - a READY slot onto another queue: BOTH queues' locks (a steal, the
//     balancer, an RT pick taking it off RQ_RT);
//   - into or out of BLOCKED, ZOMBIE or UNUSED: the slot's pi_lock and
//     its queue's lock -- and leaving BLOCKED, or dying while on a wait
//     channel, its CHAIN first (slot_lock_chained());
//   - `rq_cpu` of a slot on no list and not running: pi_lock alone;
//   - `stopped`, the class, `nice`, and `pending` that a lock-free
//     switch-in reads: pi_lock + its queue (rq_lock_slot()).
// So pi_lock + the queue the slot is on pins it, as task_rq_lock() does.
//
// **A WAKE GOES BACK TO A CORE STILL ON THE SLOT'S STACK** (on_core >= 0,
// mid-switch-out): that core finds it after its switch. On any other core
// the pick skips a live slot, and that core would halt beside it.
#include "sched_internal.h"
#include "smp.h"
#include "irqflags.h"
#include "pmm.h"
#include "string.h"
#include "klog.h"
#include "kfmt.h"
#include "clocksource.h"
#include "syscall_abi.h"   // SCHED_OTHER: no realtime slot on a fair list

// Until scheduler_init() sizes the array from the cores smp_init() found,
// core 0's queue is this one, and nothing is on it.
static struct sched_rq g_rq_boot = { .fair = { -1, -1, 0 } };
struct sched_rq *g_rqs = &g_rq_boot;
int g_nrq = 1;

SPINLOCK_COUNTED(g_rt_lock, "rt");
struct rq_list g_rt_list = { -1, -1, 0 };

int g_sched_force_core = -1;
static volatile int g_sched_steal = 1;     // kernel.sched.steal
static volatile int g_sched_balance = 1;   // kernel.sched.balance
int  scheduler_steal_enabled(void) { return g_sched_steal; }
void scheduler_steal_set(int on) { g_sched_steal = on ? 1 : 0; }
int  scheduler_balance_enabled(void) { return g_sched_balance; }
void scheduler_balance_set(int on) { g_sched_balance = on ? 1 : 0; }

static void rq_setup(struct sched_rq *rq, int core) {
    k_memset(rq, 0, sizeof *rq);
    rq->fair.head = rq->fair.tail = -1;
    k_snprintf(rq->name, sizeof rq->name, "rq%d", core);
    rq->stat.name = rq->name;
    rq->lock.stat = &rq->stat;
    spin_lock_name(&rq->lock, "rq");   // one class: two at once only by subclass
}

void sched_rq_init(void) {
    int n = smp_cpu_count();
    uint64_t pages = ((uint64_t)n * sizeof(struct sched_rq) + 4095) / 4096;
    uint64_t phys = n > 1 ? pmm_alloc_contiguous(pages, PMM_ZONE_ANY) : 0;
    struct sched_rq *a = phys ? (struct sched_rq *)(uintptr_t)phys : &g_rq_boot;   // identity-mapped
    if (!phys) n = 1;   // one queue beats no scheduler: every placement is core 0's
    for (int i = 0; i < n; i++) rq_setup(&a[i], i);
    g_rqs = a;
    g_nrq = n;
}

struct sched_rq *rq_of(int core) {
    return core > 0 && core < g_nrq ? &g_rqs[core] : &g_rqs[0];
}

struct rq_list *rq_list_of(int q) { return q == RQ_RT ? &g_rt_list : &rq_of(q)->fair; }

static const struct cpu *cpu_of(int k) { return smp_cpu_at(k); }

// `lockstat`'s rows past the declared ones (lockstat.c): one per queue.
int lockstat_extra_count(void) { return g_rqs == &g_rq_boot ? 0 : g_nrq; }
struct lockstat *lockstat_extra_at(int i) {
    return i >= 0 && i < lockstat_extra_count() ? &g_rqs[i].stat : 0;
}

// ---- locks -------------------------------------------------------------

void rq_lock(int q) { spin_lock_noguard(q == RQ_RT ? &g_rt_lock : &rq_of(q)->lock); }
void rq_unlock(int q) { spin_unlock_noguard(q == RQ_RT ? &g_rt_lock : &rq_of(q)->lock); }
void rq_lock2(int core) { spin_lock_noguard_nested(&rq_of(core)->lock, 1); }
int  rq_trylock2(int core) { return spin_trylock_noguard_nested(&rq_of(core)->lock, 1); }

void rq_lock_here(void) {
    struct cpu *c = sched_here();
    if (c->sched_lock_depth)
        klog_printf(KLOG_ERR "sched: core %u takes its queue holding it (from %p)\n",
                    spin_cpu_index(), __builtin_return_address(0));
    rq_lock((int)spin_cpu_index());
    c->sched_lock_depth = 1;
}

void rq_unlock_here(void) {
    sched_here()->sched_lock_depth = 0;
    rq_unlock((int)spin_cpu_index());
}

void pi_lock(int s) { spin_lock_noguard(&procs[s].pi_lock); }
void pi_unlock(int s) { spin_unlock_noguard(&procs[s].pi_lock); }

static int q_valid(int q) { return q == RQ_RT || (q >= 0 && q < g_nrq); }

int rq_lock_slot(int s) {
    struct sched_process *p = &procs[s];
    for (;;) {
        int q = __atomic_load_n(&p->rq_cpu, __ATOMIC_ACQUIRE);
        if (!q_valid(q)) {   // never written so: a slot starts on core 0
            klog_printf(KLOG_ERR "sched: slot %d names queue %d\n", s, q);
            __atomic_store_n(&p->rq_cpu, 0, __ATOMIC_RELEASE);
            continue;
        }
        rq_lock(q);
        if (__atomic_load_n(&p->rq_cpu, __ATOMIC_ACQUIRE) == q) return q;
        rq_unlock(q);   // moved while we waited: follow it
    }
}

// The chain is read before its lock is held and checked after both: only
// a waker (holding the chain) unlinks a slot that is not running, and a
// running one links itself only when it arms -- never while it is the one
// being killed or freed, which is what needs the chain.
const void *slot_lock_chained(int s) {
    struct sched_process *p = &procs[s];
    for (;;) {
        const void *chan = __atomic_load_n(&p->sq_on, __ATOMIC_ACQUIRE) ? p->sq_chan : 0;
        if (chan) sq_lock_raw(chan);
        pi_lock(s);
        const void *now = __atomic_load_n(&p->sq_on, __ATOMIC_ACQUIRE) ? p->sq_chan : 0;
        if (now == chan) return chan;
        pi_unlock(s);
        if (chan) sq_unlock_raw(chan);
    }
}

void slot_unlock_chained(int s, const void *chan) {
    pi_unlock(s);
    if (chan) sq_unlock_raw(chan);
}

// ---- the lists ---------------------------------------------------------

static int movable(const struct sched_process *p, int q) {
    return q >= 0 && !p->kthread && !p->pin_bsp && !p->stopped;
}

static void list_insert(struct rq_list *l, int s) {
    struct sched_process *p = &procs[s];
    __atomic_store_n(&p->rq_next, -1, __ATOMIC_RELEASE);
    p->rq_prev = l->tail;
    if (l->tail >= 0) __atomic_store_n(&procs[l->tail].rq_next, s, __ATOMIC_RELEASE);
    else __atomic_store_n(&l->head, s, __ATOMIC_RELEASE);
    l->tail = s;
    __atomic_store_n(&l->count, l->count + 1, __ATOMIC_RELEASE);
}

static void list_remove(struct rq_list *l, int s) {
    struct sched_process *p = &procs[s];
    if (p->rq_prev >= 0) __atomic_store_n(&procs[p->rq_prev].rq_next, p->rq_next, __ATOMIC_RELEASE);
    else __atomic_store_n(&l->head, p->rq_next, __ATOMIC_RELEASE);
    if (p->rq_next >= 0) procs[p->rq_next].rq_prev = p->rq_prev;
    else l->tail = p->rq_prev;
    __atomic_store_n(&l->count, l->count - 1, __ATOMIC_RELEASE);
}

void rq_enqueue(int s, int q) {
    struct sched_process *p = &procs[s];
    list_insert(rq_list_of(q), s);
    p->rq_on = 1;
    __atomic_store_n(&p->rq_cpu, q, __ATOMIC_RELEASE);
    if (movable(p, q)) {
        struct sched_rq *rq = rq_of(q);
        __atomic_store_n(&rq->nr_movable, rq->nr_movable + 1, __ATOMIC_RELEASE);
    }
}

void rq_dequeue(int s) {
    struct sched_process *p = &procs[s];
    int q = p->rq_cpu;
    list_remove(rq_list_of(q), s);
    p->rq_on = 0;
    if (movable(p, q)) {
        struct sched_rq *rq = rq_of(q);
        __atomic_store_n(&rq->nr_movable, rq->nr_movable - 1, __ATOMIC_RELEASE);
    }
}

void slot_set_stopped(int s, int on) {
    struct sched_process *p = &procs[s];
    int listed = p->rq_on && p->rq_cpu >= 0;
    int was = listed && movable(p, p->rq_cpu);
    p->stopped = (uint8_t)(on ? 1 : 0);
    int now = listed && movable(p, p->rq_cpu);
    if (was != now) {
        struct sched_rq *rq = rq_of(p->rq_cpu);
        __atomic_store_n(&rq->nr_movable, rq->nr_movable + (now ? 1 : -1), __ATOMIC_RELEASE);
    }
}

// ---- state -------------------------------------------------------------

void sched_state_locked(int s, enum sched_state st) {
    struct sched_process *p = &procs[s];
    int dying = st == SCHED_UNUSED || st == SCHED_ZOMBIE;
    if (p->sq_on && ((p->state == SCHED_BLOCKED && st != SCHED_BLOCKED) || dying)) {
        sq_unlink(s);   // its chain: held by the caller (slot_lock_chained())
        if (dying) { p->arm_chan = 0; p->armed_woken = 0; }
    }
    if (p->rq_on) rq_dequeue(s);
    if (p->state == SCHED_RUNNING) rq_of(p->rq_cpu)->nr_running--;
    if (st == SCHED_RUNNING) {
        int me = (int)spin_cpu_index();
        __atomic_store_n(&p->rq_cpu, me, __ATOMIC_RELEASE);
        rq_of(me)->nr_running++;
    }
    p->state = st;
}

void sched_make_ready(int s, int q) {
    struct sched_process *p = &procs[s];
    if (p->sq_on && p->state == SCHED_BLOCKED) sq_unlink(s);
    if (p->state == SCHED_RUNNING) rq_of(p->rq_cpu)->nr_running--;
    if (p->rq_on) rq_dequeue(s);
    rq_enqueue(s, q);
    p->state = SCHED_READY;
}

void sched_set_state(int s, enum sched_state st) {
    uint64_t f = irq_save();
    const void *chan = slot_lock_chained(s);
    int q = rq_lock_slot(s);
    sched_state_locked(s, st);
    rq_unlock(q);
    slot_unlock_chained(s, chan);
    irq_restore(f);
}

void sched_publish(int s) {
    uint64_t f = irq_save();
    pi_lock(s);
    int q = sched_place(s, (int)spin_cpu_index());
    rq_lock(q);
    procs[s].vruntime = rq_of(q)->min_vruntime;   // a newcomer joins the pack, not ahead of it
    sched_make_ready(s, q);
    procs[s].claimed = 0;   // published: no longer only the allocators'
    alive_count++;
    rq_unlock(q);
    pi_unlock(s);
    sched_kick_wake(s, q);
    irq_restore(f);
}

// ---- placement ---------------------------------------------------------

int sched_core_usable(int k) {
    if (k == 0) return 1;
    if (!smp_scheduling() || k < 0 || k >= g_nrq) return 0;
    const struct cpu *c = cpu_of(k);
    return c && __atomic_load_n(&c->state, __ATOMIC_ACQUIRE) == CPU_RUNNING;
}

// Nothing running, nothing queued: an AP in its idle loop, or the BSP's
// kernel context halting.
int sched_core_idle(int k) {
    if (!sched_core_usable(k)) return 0;
    if (__atomic_load_n(&rq_of(k)->fair.count, __ATOMIC_ACQUIRE)) return 0;
    const struct cpu *c = cpu_of(k);
    if (!c || __atomic_load_n(&c->sched_cur, __ATOMIC_ACQUIRE) >= 0) return 0;
    return k != 0 || scheduler_cpu_halting();
}

static int core_load(int k);

// **A PROCESS GOES ON AN AP, NEVER ON CORE 0'S QUEUE** while any AP
// schedules: core 0 takes the device interrupts and runs the kernel
// context, which holds the kernel lock whenever it runs -- every return
// to it from a process there retakes that lock. Placed there whenever it
// idled, the benches' pipe pairs made core 0 switch 137k times in 6 s
// (6k at 2a) and the kernel lock's waits went from 0.2-0.3 s to 1-2.4 s.
// A hint of core 0 (a spawn from the kernel context, a slot that last ran
// there) takes the least-loaded AP.
static int least_loaded_ap(void) {
    int best = -1, bl = 0;
    for (int k = 1; k < g_nrq; k++) {
        if (!sched_core_usable(k)) continue;
        int l = core_load(k);
        if (best < 0 || l < bl) { best = k; bl = l; }
    }
    return best < 0 ? 0 : best;
}

int sched_place(int s, int hint) {
    const struct sched_process *p = &procs[s];
    if (p->kthread || p->pin_bsp || !smp_scheduling() || g_nrq < 2) return 0;
    int force = g_sched_force_core;
    if (force >= 0 && sched_core_usable(force)) return force;
    int live = __atomic_load_n(&p->on_core, __ATOMIC_ACQUIRE);
    if (live >= 0 && sched_core_usable(live)) return live;
    if (hint <= 0 || !sched_core_usable(hint)) hint = (int)spin_cpu_index();
    if (hint > 0 && sched_core_idle(hint)) return hint;
    for (int n = 1; n < g_nrq; n++) {
        int k = (hint + n) % g_nrq;
        if (k && sched_core_idle(k)) return k;
    }
    return hint > 0 ? hint : least_loaded_ap();
}

// A fair slot's vruntime is relative to its queue's floor: moved, it
// keeps its distance from the pack (Linux's migrate_task_rq_fair()).
void sched_vr_migrate(int s, int from, int to) {
    if (from == to || from < 0 || to < 0) return;
    int64_t d = (int64_t)(rq_of(to)->min_vruntime - rq_of(from)->min_vruntime);
    uint64_t v = procs[s].vruntime;
    procs[s].vruntime = d < 0 && (uint64_t)-d > v ? 0 : v + (uint64_t)d;
}

void sched_kick_wake(int s, int k) {
    (void)s;
    if (k < 0 || k == (int)spin_cpu_index()) return;
    const struct cpu *c = cpu_of(k);
    if (c && __atomic_load_n(&c->sched_cur, __ATOMIC_ACQUIRE) < 0) sched_kick_core(k);
}

// ---- moving a slot: the idle steal and the balancer --------------------

// The movable READY slot on q's list that ran longest ago (Linux's
// can_migrate_task() prefers the cache-cold), or -1. q's lock held.
static int pick_movable(int q) {
    int best = -1, n = 0;
    uint64_t when = 0;
    for (int i = rq_of(q)->fair.head; i >= 0 && n <= MAX_PROCS; i = procs[i].rq_next, n++) {
        const struct sched_process *p = &procs[i];
        if (p->state != SCHED_READY || !movable(p, q)) continue;
        if (__atomic_load_n(&p->on_core, __ATOMIC_ACQUIRE) >= 0) continue;   // a core is on its stack
        if (best < 0 || p->left_tsc < when) { best = i; when = p->left_tsc; }
    }
    return best;
}

static void move_slot(int s, int from, int to) {
    rq_dequeue(s);
    sched_vr_migrate(s, from, to);
    rq_enqueue(s, to);
}

int sched_steal_hint(int me) {
    if (!g_sched_steal || !smp_scheduling() || g_nrq < 2 || me == 0) return 0;
    for (int k = 0; k < g_nrq; k++)
        if (k != me && __atomic_load_n(&rq_of(k)->nr_movable, __ATOMIC_ACQUIRE) > 0) return 1;
    return 0;
}

// The busiest other queue by what could move, its lock taken in core
// order -- or TRIED, when it comes first: this core's is held, and letting
// it go mid-pick would let a wake in behind the pick's back.
int sched_steal(int me) {
    if (!g_sched_steal || !smp_scheduling() || g_nrq < 2 || me == 0) return -1;
    int from = -1, most = 0;
    for (int k = 0; k < g_nrq; k++) {
        if (k == me) continue;
        int n = __atomic_load_n(&rq_of(k)->nr_movable, __ATOMIC_ACQUIRE);
        if (n > most) { most = n; from = k; }
    }
    if (from < 0) return -1;
    if (from > me) rq_lock2(from);
    else if (!rq_trylock2(from)) return -1;
    int s = pick_movable(from);
    if (s >= 0) {
        move_slot(s, from, me);
        rq_of(me)->steals++;
    }
    rq_unlock(from);
    return s;
}

// What a core has to do: what could move off it, and itself if busy.
static int core_load(int k) {
    const struct cpu *c = cpu_of(k);
    int busy = c && (__atomic_load_n(&c->sched_cur, __ATOMIC_ACQUIRE) >= 0 ||
                     (k == 0 && !scheduler_cpu_halting()));
    return __atomic_load_n(&rq_of(k)->nr_movable, __ATOMIC_ACQUIRE) + busy;
}

#define BALANCE_SLICES 4
static uint64_t g_last_balance_ns;

// Linux's load_balance() at one level, no domains: every few slices, from
// the BSP's tick, one slot from the longest queue to the shortest when
// they differ by two or more. Read without locks; the move re-checks.
void sched_balance_tick(uint64_t now) {
    if (!g_sched_balance || !smp_scheduling() || g_nrq < 2) return;
    uint64_t every = (uint64_t)BALANCE_SLICES * scheduler_timeslice_ms() * 1000000ull;
    if (now - g_last_balance_ns < every) return;
    g_last_balance_ns = now;
    int hi = -1, lo = -1, hl = 0, ll = 0;
    for (int k = 0; k < g_nrq; k++) {
        if (!sched_core_usable(k)) continue;
        int l = core_load(k);
        if (rq_of(k)->nr_movable > 0 && (hi < 0 || l > hl)) { hi = k; hl = l; }
        if (k && (lo < 0 || l < ll)) { lo = k; ll = l; }   // never onto core 0 (sched_place())
    }
    if (hi < 0 || lo < 0 || hi == lo || hl - ll < 2) return;
    uint64_t f = irq_save();
    rq_lock(hi < lo ? hi : lo);
    rq_lock2(hi < lo ? lo : hi);
    int s = pick_movable(hi);
    if (s >= 0) {
        move_slot(s, hi, lo);
        rq_of(lo)->pulls++;
    }
    rq_unlock(hi < lo ? lo : hi);
    rq_unlock(hi < lo ? hi : lo);
    if (s >= 0) sched_kick_wake(s, lo);
    irq_restore(f);
}

// ---- freezing a thread group -------------------------------------------

// RUNNING IS READ UNDER THE SLOT'S LOCKS, never as a hint: a rotation
// puts the running slot back READY under its queue lock and, declining,
// makes it RUNNING again (lockless_rotate()) -- a reader without the lock
// saw "not running" and freed a thread that was.
static int running_core_locked(int s) {
    return procs[s].state == SCHED_RUNNING ? procs[s].rq_cpu : -1;
}

int sched_freeze_slot(int s) {
    uint64_t f = irq_save();
    pi_lock(s);
    int q = rq_lock_slot(s);
    slot_set_stopped(s, 1);
    int core = running_core_locked(s);
    rq_unlock(q);
    pi_unlock(s);
    irq_restore(f);
    return core;
}

int sched_running_core(int s) {
    uint64_t f = irq_save();
    pi_lock(s);
    int q = rq_lock_slot(s);
    int core = running_core_locked(s);
    rq_unlock(q);
    pi_unlock(s);
    irq_restore(f);
    return core;
}

void sched_unfreeze_slot(int s) {
    uint64_t f = irq_save();
    pi_lock(s);
    int q = rq_lock_slot(s);
    slot_set_stopped(s, 0);
    int listed = procs[s].rq_on && q >= 0;
    rq_unlock(q);
    pi_unlock(s);
    if (listed) sched_kick_wake(s, q);
    irq_restore(f);
}

// ---- the invariants, checked at every switch in a debug build ----------
//
// THIS core's queue, under its lock: every slot on the list names it and
// is READY, no slot naming it as on a list is missing from it -- so none
// is on two -- and the counts agree. A slot moves onto or off this list
// only under this lock, so the scan of the table is exact. `incoming` is
// the slot this switch claimed: READY, on no list, for a moment.
#if defined(SAUNAOS_LOCKDEP) || defined(SAUNAOS_DEBUG_PREEMPT)
static int g_rq_check_latched;
static unsigned g_rq_checks;

static void rq_bad(const char *where, const char *what, int s, int a, int b) {
    if (g_rq_check_latched++) return;
    klog_printf(KLOG_ERR "sched: RUN QUEUE INVARIANT at %s on core %u -- %s (slot %d: %d vs %d)\n",
                where, spin_cpu_index(), what, s, a, b);
    scheduler_trace_dump();
}

void sched_rq_check(const char *where, int incoming) {
    if (g_rq_check_latched) return;
    g_rq_checks++;
    int me = (int)spin_cpu_index();
    struct sched_rq *rq = rq_of(me);
    int n = 0, mov = 0;
    for (int i = rq->fair.head; i >= 0; i = procs[i].rq_next) {
        if (i >= MAX_PROCS || n > MAX_PROCS) { rq_bad(where, "the list runs off", i, n, MAX_PROCS); return; }
        const struct sched_process *p = &procs[i];
        if (!p->rq_on || p->rq_cpu != me) { rq_bad(where, "listed, names another queue", i, p->rq_cpu, me); return; }
        if (p->state != SCHED_READY) { rq_bad(where, "listed, not READY", i, (int)p->state, SCHED_READY); return; }
        if (p->policy != SCHED_OTHER) { rq_bad(where, "realtime on a fair list", i, p->policy, 0); return; }
        mov += movable(p, me);
        n++;
    }
    if (n != rq->fair.count) { rq_bad(where, "count", -1, n, rq->fair.count); return; }
    if (mov != rq->nr_movable) { rq_bad(where, "movable count", -1, mov, rq->nr_movable); return; }
    int named = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        const struct sched_process *p = &procs[i];
        if (p->rq_on && p->rq_cpu == me) named++;
        if (p->state == SCHED_READY && !p->rq_on && i != incoming &&
            __atomic_load_n(&p->rq_cpu, __ATOMIC_ACQUIRE) == me && !spin_held(&p->pi_lock)) {
            rq_bad(where, "READY on no list", i, p->rq_cpu, me);
            return;
        }
    }
    if (named != n) { rq_bad(where, "slots naming this list", -1, named, n); return; }
    int cur = current_index;
    int running = cur >= 0 && procs[cur].state == SCHED_RUNNING;
    if (rq->nr_running != running) { rq_bad(where, "nr_running", cur, rq->nr_running, running); return; }
    if (running && procs[cur].rq_cpu != me) { rq_bad(where, "running here, names another", cur, procs[cur].rq_cpu, me); return; }
}
unsigned scheduler_rq_checks(void) { return g_rq_checks; }
unsigned scheduler_rq_check_failures(void) { return (unsigned)g_rq_check_latched; }
#else
void sched_rq_check(const char *where, int incoming) { (void)where; (void)incoming; }
unsigned scheduler_rq_checks(void) { return 0; }
unsigned scheduler_rq_check_failures(void) { return 0; }
#endif

uint64_t scheduler_rq_steals(void) {
    uint64_t n = 0;
    for (int k = 0; k < g_nrq; k++) n += g_rqs[k].steals;
    return n;
}

uint64_t scheduler_rq_pulls(void) {
    uint64_t n = 0;
    for (int k = 0; k < g_nrq; k++) n += g_rqs[k].pulls;
    return n;
}

int scheduler_rq_queued(int core) {
    return core >= 0 && core < g_nrq ? __atomic_load_n(&g_rqs[core].fair.count, __ATOMIC_ACQUIRE) : 0;
}
