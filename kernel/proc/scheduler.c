// Preemptive round-robin scheduler for ring-3 processes -- built as the
// ORIGINAL Milestone 16 (the old numbering in the git history, not
// docs/roadmap.md's current Milestone 16).
//
// DESIGN
// ------
// Every milestone through M15 ran at most one ring-3 process at a time,
// synchronously: process_run_ring3() (process.c) drops to ring 3 and
// gets control back only when that process calls the exit syscall,
// via a setjmp/longjmp-style save/restore of the CALLER's kernel
// context (g_process_ctx in process.c). That's a real, useful
// mechanism, but it's fundamentally a function call, not scheduling --
// nothing else can run while a process is "in flight" through it.
//
// This file adds honest preemptive multitasking on top, WITHOUT
// touching that mechanism: the two coexist, chosen per-syscall by
// whether the exiting process is scheduler-managed (see
// scheduler_current_pid(), used by syscall.c). Every one of the M8-M15
// test commands keeps using process_run_ring3() untouched and is
// provably unaffected (see below).
//
// The core trick: isr_common (isr.asm) already saves a process's full
// register state (15 GP regs + vector + error code + the CPU-pushed
// rip/cs/rflags/rsp/ss) onto whatever stack was active when the
// interrupt fired, calls isr_dispatch(regs) with a pointer to it, then
// -- unmodified through M15 -- just pops those same registers back off
// THE SAME stack and iretq's, resuming exactly what was interrupted.
//
// M16 generalizes that last step: isr_common now reloads rsp from a
// global, g_next_kernel_rsp (defined in idt.c), immediately before the
// pop+iretq sequence. isr_dispatch sets it to `regs` (i.e. a no-op --
// resume what was interrupted) at the very top of the function, for
// EVERY vector, unconditionally. Only scheduler_tick() (called for
// vector 32, the timer, and only when armed) or scheduler_on_exit()
// (called from syscall.c's SYS_EXIT handler) ever override it, to
// point at a DIFFERENT saved register block instead -- another
// process's, or back to whatever kernel code (the shell, blocked in
// scheduler_demo_run()'s wait loop) was running before any process got
// the CPU.
//
// This works uniformly for every case that matters here:
//   - Switching between two ring-3 processes: each gets its own
//     dedicated kernel stack (proc.kstack), used as the CPU's RSP0 (via
//     gdt_set_kernel_stack()) while that process is the one running --
//     so if it's interrupted, its register block lands on ITS OWN
//     stack, not shared with any other process. Switching processes is
//     just: point g_next_kernel_rsp at the other one's saved block,
//     switch CR3 (vmm_switch_address_space -- safe mid-ISR because
//     every process's PML4 shares kernel entry 0, see vmm.h), and
//     repoint RSP0 for next time.
//   - Launching a process for the FIRST time: its "saved register
//     block" is synthesized once, in spawn_from_fs() below, instead
//     of being the product of a real interrupt -- but it's laid out
//     identically (r15..rax zeroed, rip/cs/rflags/rsp/ss set to the
//     ELF's entry point and a fresh user stack), so isr_common's
//     ordinary epilogue can't tell the difference. This unifies "first
//     launch" and "resume after preemption" into one mechanism.
//   - Switching back to the kernel/shell (scheduler_demo_run()'s wait
//     loop) once no process is ready: kernel code is ring 0, so this is
//     a same-privilege interrupt return (iretq only restores
//     rip/cs/rflags, not rsp/ss, since there was no stack switch) --
//     the exact same "just point g_next_kernel_rsp elsewhere" trick
//     handles it too, using whatever real stack the shell's wait loop
//     was actually using when last interrupted (kernel_saved_rsp,
//     refreshed every tick that finds no process running).
//
// SAFETY FOR M8-M15, AND EVERYTHING SPAWNED NEITHER BY schedtest NOR
// Terminal's async run/ls (Milestone 1 phase 4b, docs/roadmap.md)
// ---------------------------------------------------------------------
// scheduler_armed is set true once, permanently, in scheduler_init()
// ("continuously armed" -- the roadmap item this generalizes from
// demo-only) rather than being flipped on/off around
// scheduler_demo_run()'s own wait loop the way it used to be. This is
// still safe for every M8-M15 test command and every legacy
// elf_run_from_fs() caller (once `run`/`ls`; a KTEST now) despite
// being permanently on: scheduler_tick() being armed only matters once
// something is actually in the process table (alive_count > 0) --
// find_next_ready() scanning an all-SCHED_UNUSED table always returns
// -1, so every tick that finds nothing ready just re-confirms
// g_next_kernel_rsp at whatever isr_dispatch's default already set it
// to (`regs`, i.e. a genuine no-op, byte-for-byte the same as the old
// disarmed early-return). scheduler_on_exit() is likewise only ever
// reached via syscall.c's `if (scheduler_current_pid())` guard, and
// scheduler_current_pid() returns 0 whenever current_index is -1 --
// which it always is unless something was actually spawned through
// this file's spawn_from_fs(). So every existing test command's exit
// path (the old process_context_restore(&g_process_ctx, ...) call in
// syscall.c) is completely untouched by this file, exactly as before --
// only the mechanism that used to keep it that way (a flag flipped
// off) changed to a different one (an empty table).
#include "vt.h"         // vga_idle() -- the console's tail, from the idle work
#include "uaddr.h" // uaddr_is_user() -- a thread pointer ring 3 may hold
#include "sandbox.h"   // slot_free() drops the slot's reference
#include "sched_internal.h"
#include "bkl.h"
#include "smp.h"      // smp=run: which core runs a slot, the reschedule IPI
#include "lockup.h"  // lockup_idle()
#include "vmm.h"
#include "process.h" // process_context_is_armed() -- see kernel_slot_runnable()
#include "page_report.h" // page_report_idle() -- idle work scheduler_idle() owns
#include "win_input.h" // raw input to a ring-3 compositor // win_server_client_gone() -- see scheduler_on_exit()
#include "netdev.h"    // net_poll() -- the idle half of the receive path
#include "kfmt.h"      // klog_printf, vga_printf
#include "idt.h"     // isr_depth_get()/_set() -- the depth travels with kernel_rsp
#include "syscall_abi.h" // SYS_RETRY -- the wake value a blocked waiter sees
#include "gdt.h"
#include "tls.h"    // FS.base -- a thread pointer is per THREAD, see switch_to()
#include "vga.h"
#include "klog.h"
#include "clocksource.h" // CPU time is measured, not counted -- bill_current()
#include "clockevent.h"  // clockevent_reprogram() on every switch, clockevent_in_idle()
#include "random_hw.h"   // arch_rdtsc() -- off-CPU time, see context_load_globals()
#include "debug_console.h"
#include "ata.h"       // ata_idle() -- the idle work scheduler_idle() owns
#include "input.h"     // input_poll_sources() -- ditto, for a device with no IRQ
#include "string.h" // k_strlcpy -- proc_name_from_path()
#include "pmm.h"    // the table and the stack chunks
#include "irqflags.h" // per-process I/O counters, charged from any context
#include "ktrace.h"   // the trace's switch and wakeup points
#include "lockstat.h" // SPINLOCK_COUNTED: the scheduler lock

_Static_assert(SCHED_READY == KTRACE_STATE_READY && SCHED_RUNNING == KTRACE_STATE_RUNNING &&
               SCHED_ZOMBIE == KTRACE_STATE_ZOMBIE && SCHED_BLOCKED == KTRACE_STATE_BLOCKED,
               "a switch record's prev_state is the scheduler's state: abi/ktrace_abi.h");

// The thread pointer belonging to the KERNEL CONTEXT -- which in
// practice means a ring-3 process the legacy elf_run.c loader is
// running, since ring 0 itself never reads %fs. See
// scheduler_set_tls().
static uint64_t kernel_fs_base;

// A SWITCH MOVES THE CPU ITSELF; NOTHING NOMINATES A FRAME ANY MORE.
// switch_to() saves the outgoing context and restores the incoming one
// on the spot (Linux's __switch_to_asm, NT's SwapContext), so a resumed
// context returns out through the dispatch it parked in and that
// dispatch's own epilogue iretqs from the frame it arrived on. What
// this replaced -- g_next_kernel_rsp, then a per-dispatch resume slot
// with a deferred (slot, depth) nomination beside it -- is gone with
// it; see docs/blocking-design.md.

// "/bin/wm/demos/uidemo" -> "uidemo". A task manager column is a few
// characters wide, so the last component is the useful part and the
// path is not kept at all (see abi/proc_info.h).
void proc_name_from_path(char *dst, int cap, const char *path) {
    if (cap <= 0) return;
    dst[0] = '\0';
    if (!path) return;

    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    // A path ending in '/' leaves nothing; keep the whole thing rather
    // than reporting an empty name, which would read as a kernel bug.
    if (!*base) base = path;
    k_strlcpy(dst, base, (size_t)cap);
}

// scheduler.h deliberately does not include fs.h, so struct sched_cwd
// spells its size as a literal. This is what stops the two drifting.
_Static_assert(sizeof(((struct sched_cwd *)0)->path) == FS_PATH_MAX,
               "struct sched_cwd::path must match FS_PATH_MAX");
_Static_assert(sizeof(((struct mmap_region *)0)->path) == FS_PATH_STORED_MAX,
               "mmap_region.path is a STORED path (fs.h): 32 per process, so it "
               "takes the smaller bound and mmap REFUSES anything longer");

// THE TABLE, sized from RAM in scheduler_init(): g_max_procs slots, of
// which g_slots_ready have a kernel stack, and g_slot_end bounds every scan
// (sched_internal.h, MAX_PROCS). NULL and 0 before init, so an early scan
// is an empty loop.
struct sched_process *procs;
int g_max_procs;
int g_slot_end;
int g_slots_ready;
static int g_guards_on;   // scheduler_guard_pages_init() has run

// A SLOT BEING BUILT IS CLAIMED, and only the allocators look. A spawn
// picks a slot and then reads the ELF from disk, which SLEEPS -- and the
// slot stayed SCHED_UNUSED meanwhile, so a second spawn in that window
// took the same one. Both filled it; the first child never existed and
// its descriptor table, holding its parent's pipe end, was never
// released, so the parent read forever (argv_test in block(pipe), about
// 1 suite run in 5). Linux's TASK_NEW is the same idea as a state; a
// flag only the allocators consult keeps every other scan of the table
// seeing "not a process" without auditing each one (procs[i].claimed).

// pid -> slot + 1, or 0. Kept by slot_claim() and slot_free() alone.
static uint16_t g_pid_slot[SCHED_PID_MAX];
static int g_last_pid;

// Still in use as a GROUP or a SESSION by a live or zombie slot: Linux
// keeps a `struct pid` alive for that, and handing the number out would
// put a new process into an old group.
static int pid_named(int pid) {
    for (int i = 0; i < MAX_PROCS; i++)
        if (procs[i].state != SCHED_UNUSED && (procs[i].pgid == pid || procs[i].sid == pid))
            return 1;
    return 0;
}

// The next free pid after the last one handed out, or 0. Interrupts off.
static int pid_alloc(void) {
    int pid = g_last_pid;
    for (int n = 0; n < SCHED_PID_MAX; n++) {
        if (++pid >= SCHED_PID_MAX) pid = SCHED_PID_RESERVED;
        if (g_pid_slot[pid] || pid_named(pid)) continue;
        g_last_pid = pid;
        return pid;
    }
    return 0;
}

// The next SCHED_SLOT_CHUNK slots' kernel stacks and ext parts, or 0.
// Under the preemption guard rather than with interrupts off: the frame
// allocator and the page split sleep on nothing, and a kernel-context
// spawner cannot park. g_slots_ready moves LAST, so no scan sees a slot
// without its stack.
static int slot_grow(void) {
    if (!procs || g_slots_ready >= g_max_procs) return 0;
    int n = g_max_procs - g_slots_ready;
    if (n > SCHED_SLOT_CHUNK) n = SCHED_SLOT_CHUNK;
    uint64_t kpages = ((uint64_t)n * sizeof(struct kstack) + 4095) / 4096;
    uint64_t epages = ((uint64_t)n * sizeof(struct sched_slot_ext) + 4095) / 4096;
    uint64_t kphys = pmm_alloc_contiguous(kpages, PMM_ZONE_ANY);
    if (!kphys) return 0;
    uint64_t ephys = pmm_alloc_contiguous(epages, PMM_ZONE_ANY);
    if (!ephys) { pmm_free_contiguous(kphys, kpages); return 0; }
    struct kstack *ks = (struct kstack *)(uintptr_t)kphys;   // identity-mapped
    struct sched_slot_ext *ex = (struct sched_slot_ext *)(uintptr_t)ephys;
    k_memset(ex, 0, epages * 4096);
    int base = g_slots_ready;
    for (int i = 0; i < n; i++) {
        procs[base + i].kstack = &ks[i];
        procs[base + i].ext = &ex[i];
        procs[base + i].on_core = -1;   // slot_claim() waits for a live one
        if (g_guards_on) kstack_guard_arm(&ks[i]);
    }
    g_slots_ready = base + n;
    return 1;
}

int slot_claim(void) {
    for (int attempt = 0; attempt < 2; attempt++) {
        uint64_t f;
        __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) :: "memory");
        int slot = -1;
        for (int i = 0; i < g_slots_ready; i++) {
            // NOT WHILE ITS LAST CONTEXT IS STILL LIVE: a core that just
            // switched away from it (an exit) may still be on its stack.
            if (procs[i].state == SCHED_UNUSED && !procs[i].claimed &&
                __atomic_load_n(&procs[i].on_core, __ATOMIC_ACQUIRE) < 0) {
                int pid = pid_alloc();   // the one place a pid is chosen
                if (!pid) break;
                procs[i].claimed = 1;
                procs[i].pid = pid;
                g_pid_slot[pid] = (uint16_t)(i + 1);
                if (i >= g_slot_end) g_slot_end = i + 1;   // before anyone can see it
                slot = i;
                break;
            }
        }
        __asm__ volatile ("pushq %0; popfq" :: "r"(f) : "memory", "cc");
        if (slot >= 0) return slot;
        if (attempt) return -1;
        scheduler_preempt_disable();
        int grew = slot_grow();
        scheduler_preempt_enable();
        if (!grew) return -1;
    }
    return -1;
}

// Published (its state is no longer UNUSED) or abandoned: either way the
// claim is over.
void slot_unclaim(int slot) {
    if (slot < 0 || slot >= g_slots_ready) return;
    if (procs[slot].state == SCHED_UNUSED) slot_free(slot);   // abandoned
    procs[slot].claimed = 0;
}

// THE ONE WAY A SLOT BECOMES FREE: its pid goes with it, so no lookup can
// find a reused slot under the pid of what was there before.
void slot_free(int slot) {
    int pid = procs[slot].pid;
    if (procs[slot].fd_holds)   // a frame abandoned with a hold: that description never closes
        klog_printf(KLOG_ERR "sched: pid %d freed holding %u open-file description(s)\n",
                    pid, (unsigned)procs[slot].fd_holds);
    procs[slot].fd_holds = 0;
    sandbox_put(procs[slot].ext->sandbox);
    procs[slot].ext->sandbox = 0;
    k_memset(&procs[slot].ext->io, 0, sizeof procs[slot].ext->io);
    procs[slot].ext->no_crash_report = 0;   // whatever reuses the slot reports
    if (pid > 0 && pid < SCHED_PID_MAX && g_pid_slot[pid] == slot + 1) g_pid_slot[pid] = 0;
    sched_set_state(slot, SCHED_UNUSED);
    procs[slot].pid = 0;
    procs[slot].kthread = 0;
}

void scheduler_test_set_last_pid(int pid) { g_last_pid = pid; }

// The pid init holds, or 0 on a boot that has no init (nothing spawned
// it, or /bin/init is missing). Everything that treats pid 1 specially
// asks this rather than testing `pid == 1`, so a boot without an init
// behaves exactly as this kernel did before one existed -- rather than
// adopting orphans to a pid nobody is running and refusing to kill
// whatever happens to be in slot 0.
int g_init_pid = 0;

int scheduler_init_pid(void) { return g_init_pid; }
void scheduler_set_init_pid(int pid) { g_init_pid = pid; }

// When the CURRENT occupant of the CPU (a process, or the kernel
// context when current_index is -1) started running, in clocksource
// nanoseconds. bill_current() is the only thing that reads or moves it.
#define g_run_start_ns (sched_here()->run_start_ns)   // per core

// current_index (sched_internal.h): the slot on THIS core, -1 for the
// kernel context on the BSP or the idle loop on an AP.
static int scheduler_armed = 0;
static int kernel_saved_isr_depth = 0; // the kernel slot's isr_depth_get()
static int kernel_preempt_depth = 0;   // and its scheduler_preempt_depth()
static int kernel_bkl_depth = 1;       // and its bkl_depth(): it is always in the kernel
static uint64_t kernel_offcpu_tsc, kernel_left_tsc; // and its off-CPU time
static struct kernel_context kernel_kctx; // and where the kernel context
                                          // itself is parked -- rip 0
                                          // until it has been left once
const struct kernel_context *scheduler_kernel_kctx(void) { return &kernel_kctx; }
static uint64_t kernel_saved_rsp = 0; // refreshed every tick that finds
                                        // current_index == -1
volatile int alive_count = 0;

// On an AP (smp=run), "the kernel context" is that core's idle loop:
// its own saved context and depths in struct cpu, never the BSP's
// kernel_* above, and never a rotation participant (docs/smp-design.md,
// stage 5 -- everything the kernel context does stays on the BSP).
static int on_ap(void) { return spin_cpu_index() != 0; }

// THE KERNEL CONTEXT AS A ROTATION PARTICIPANT
// --------------------------------------------
// Through Milestone 1 phase 4b the kernel context was not scheduled at
// all: it resumed only on a tick that found NOTHING ready, so any ready
// ring-3 process starved it completely until every one of them exited.
// That is what froze wm_run() for the whole lifetime of a spawned
// process -- the Terminal's async spawn only looked live because a
// process's output reaches the screen from inside its own SYS_WRITE
// handler (see userland/apps/system/toywm/wm.c's per-frame poll comment), not because the
// WM was drawing.
//
// The kernel now takes a position in the same round-robin cycle a
// process does, so wm_run() keeps drawing, routing input and polling
// while ring-3 processes run. It deliberately does NOT become a
// struct sched_process: it has no address space of its own (kernel code
// is correct under any process's CR3 -- every PML4 shares kernel entry
// 0, see vmm.h), no FP state worth saving (kernel and kernel/shell/ are built
// -mno-sse, see switch_to()'s comment), no kstack of its own (ring 0
// interrupting ring 0 doesn't switch stacks, so its trapframe lands on
// whatever kernel stack it was already using), and no slot to reap.
// All it needs is a position in the cycle and the saved trapframe
// pointer this file already kept for it.

// Where the rotation last stopped: 0..MAX_PROCS-1 for a process slot,
// ROT_KERNEL for the kernel context. Deliberately separate from
// current_index, which still means exactly what it always did (-1
// whenever a scheduler-managed process is NOT the thing running) --
// syscall.c depends on that through scheduler_current_pid(), and
// conflating the two would change every M8-M15 exit path.
// rotation_pos is per core too (sched_internal.h).

// --- thread groups ---------------------------------------------------
//
// A thread is a slot whose leader is somebody else. Everything a
// PROCESS owns is read through the leader's slot, so there is one copy
// of it however many threads share it -- which is the difference
// between this and two processes that happen to share a page table.

int is_thread(int idx) {
    return procs[idx].tgid != procs[idx].pid;
}

int pid_slot(int pid) {
    if (pid < 1 || pid >= SCHED_PID_MAX) return -1;
    int s = (int)g_pid_slot[pid] - 1;
    // Checked against the slot: a lookup from an interrupt or the stopped
    // debugger may race a claim, and must miss rather than misname.
    return s >= 0 && procs[s].pid == pid ? s : -1;
}

// The slot holding what this group shares. Falls back to `idx` for a
// group whose leader is already gone -- unreachable while a thread runs
// (the group dies as a unit) and it keeps every caller here total.
int leader_index(int idx) {
    int lead = pid_slot(procs[idx].tgid);
    return lead < 0 ? idx : lead;
}

uint64_t kernel_stack_top(int idx) {
    return kstack_top(procs[idx].kstack);
}

uint64_t kernel_stack_base(int idx) {
    return kstack_base(procs[idx].kstack);
}

// Called wherever a stack starts a new life. The trapframe the spawn
// path has just written at the top is what `reserve_top` protects.
void kstack_arm_slot(int idx) {
    kstack_arm(procs[idx].kstack, TRAPFRAME_WORDS * 8, &procs[idx].kstack_peak);
}

// The canary check. Deliberately fatal rather than a log line: the
// canary being gone means something has already written outside its
// stack, so every piece of state this scheduler is about to act on is
// suspect -- and carrying on is exactly how the original bug presented,
// as a fault in an innocent process several context switches later.
static void kstack_verify(int idx) {
    if (kstack_canary_ok(procs[idx].kstack)) return;
    klog_printf("KERNEL STACK OVERFLOW: slot %d (pid %d, \"%s\") overran its "
                "%d-byte stack -- canary at %lx destroyed\n",
                idx, procs[idx].pid, procs[idx].name, PROC_KSTACK_SIZE,
                kernel_stack_base(idx));
    vga_printf("KERNEL STACK OVERFLOW: pid %d (\"%s\") overran its kernel stack\n",
               procs[idx].pid, procs[idx].name);
    // There is no panic() to call -- the panic machinery lives in the
    // fault handler, and going through it is what buys the function
    // name, the registers and the stack scan. `ud2` is the cheapest way
    // in, and the line above says what it really was.
    __asm__ volatile ("ud2");
}

uint64_t scheduler_kstack_base(int idx) {
    if (idx < 0 || idx >= g_slots_ready) return 0;
    return kernel_stack_base(idx);
}

int scheduler_slot_end(void) { return g_slot_end; }

// Unmaps the guard page below every kernel stack. Called from
// kernel_main() AFTER paging_enforce_wx(), which rewrites every PDE and
// would otherwise put the huge page back.
void scheduler_guard_pages_init(void) {
    int ok = 0;
    for (int i = 0; i < g_slots_ready; i++) {
        if (kstack_guard_arm(procs[i].kstack)) ok++;
    }
    g_guards_on = 1;   // every later chunk arms its own
    klog_printf("sched: %d/%d kernel-stack guard pages armed (%d KiB stacks); "
                "the limit is %d processes, stacks allocated %d at a time\n",
                ok, g_slots_ready, PROC_KSTACK_SIZE / 1024, g_max_procs, SCHED_SLOT_CHUNK);
}

// Which slot's guard page contains `addr`, or -1. The fault reporter
// asks, so a page fault on a guard page is reported as what it is
// instead of as an anonymous #PF in the middle of the kernel.
int scheduler_kstack_guard_slot(uint64_t addr) {
    for (int i = 0; i < g_slots_ready; i++) {
        if (kstack_guard_contains(procs[i].kstack, addr)) return i;
    }
    return -1;
}

// LINUX'S SHAPE (kernel/fork.c, set_max_threads()): process structures may
// use at most an eighth of RAM. Floored at one chunk, so a small machine
// keeps what SaunaOS always had; capped so pids can never run out
// (SCHED_PID_MAX is four times the ceiling: own pid, pgid, sid, spare).
static int limit_from_ram(void) {
    uint64_t ram = pmm_total_frames() * pmm_frame_size();
    uint64_t per = sizeof(struct sched_process) + sizeof(struct kstack) +
                   sizeof(struct sched_slot_ext);
    uint64_t n = ram / 8 / per;
    if (n < SCHED_SLOT_CHUNK) n = SCHED_SLOT_CHUNK;
    if (n > SCHED_PROCS_CEILING) n = SCHED_PROCS_CEILING;
    return (int)n;
}

void scheduler_init(void) {
    g_max_procs = limit_from_ram();
    uint64_t pages = ((uint64_t)g_max_procs * sizeof(struct sched_process) + 4095) / 4096;
    uint64_t phys = pmm_alloc_contiguous(pages, PMM_ZONE_ANY);
    if (!phys) {   // a smaller table beats no scheduler
        g_max_procs = SCHED_SLOT_CHUNK;
        pages = ((uint64_t)g_max_procs * sizeof(struct sched_process) + 4095) / 4096;
        phys = pmm_alloc_contiguous(pages, PMM_ZONE_ANY);
    }
    procs = (struct sched_process *)(uintptr_t)phys;
    if (procs) k_memset(procs, 0, pages * 4096);
    sleepq_init();
    g_slot_end = 0;
    g_slots_ready = 0;
    slot_grow();   // the first chunk now, so boot keeps a guaranteed 64
    set_current_index(-1);
    set_rotation_pos(ROT_KERNEL);
    // Permanently armed from here on -- see this file's top comment on
    // why that's safe with an empty process table. Was `= 0` (disarmed,
    // only scheduler_demo_run() ever flipped it) before Milestone 1
    // phase 4b generalized this from a one-off demo to a real,
    // continuously-available spawn mechanism.
    scheduler_armed = 1;
    kernel_saved_rsp = 0;
    kernel_saved_isr_depth = 0;
    k_memset(&kernel_kctx, 0, sizeof kernel_kctx);
    alive_count = 0;
}

// Whether the kernel context is a runnable participant right now.
//
// Normally it is -- that's the whole point of ROT_KERNEL. The one
// exception is the LEGACY BLOCKING PATH (process_run_ring3(),
// process.c), which runs a ring-3 process WITHOUT giving it a procs[]
// slot: from this file's point of view that process's trapframe simply
// IS "the kernel context" (current_index stays -1, so every tick stores
// its regs into kernel_saved_rsp). Rotating away from it and back would
// resume a ring-3 process under whatever CR3 and RSP0 the scheduler
// process left behind -- a foreign address space and a shared kernel
// stack -- so while one is in flight the kernel position drops out of
// the rotation entirely and this file behaves exactly as it did before,
// preserving every M8-M15 test command and every elf_run_from_fs()
// caller (once `run`/`ls`; a KTEST now) unchanged.
//
// process_context_is_armed() (process.h) is precisely the predicate "a
// blocking ring-3 process is in flight", so it is reused directly
// rather than tracking a second flag here that could drift out of
// agreement with it.
static int kernel_slot_runnable(void) {
    if (process_context_is_armed()) return 0;
    // Never select the kernel before a tick has captured a real
    // trapframe for it -- g_next_kernel_rsp = 0 would iretq into
    // nothing. Unreachable in practice (the kernel is always what's
    // running when the first spawn happens, so the very next tick saves
    // it before any switch away can occur), but this is a boot-critical
    // path and the check is one compare.
    return kernel_saved_rsp != 0;
}

// A process's nice value and class, by pid. The picker is the only
// reader; these, sched_class_reset() and the RT watchdog are the only
// writers. Permission is the syscall's business, not this file's.
int scheduler_set_priority(int pid, int value) {
    int idx = pid_slot(pid);
    if (idx < 0 || procs[idx].state == SCHED_UNUSED)
        return -ESRCH;
    if (value < -20 || value > 19) return -EINVAL;
    procs[idx].nice = (int8_t)value;
    return 0;
}

int scheduler_get_priority(int pid, int *value) {
    int idx = pid_slot(pid);
    if (idx < 0 || procs[idx].state == SCHED_UNUSED)
        return -ESRCH;
    *value = procs[idx].nice;
    return 0;
}

static uint64_t g_rt_seq;   // rt_seq's counter: the back of every RT queue

int scheduler_set_class(int pid, int policy, int rt_prio) {
    int idx = pid_slot(pid);
    if (idx < 0 || procs[idx].state == SCHED_UNUSED || procs[idx].state == SCHED_ZOMBIE)
        return -ESRCH;
    if (policy == SCHED_OTHER ? rt_prio != 0
        : (policy != SCHED_FIFO && policy != SCHED_RR) ||
          rt_prio < SCHED_RT_PRIO_MIN || rt_prio > SCHED_RT_PRIO_MAX)
        return -EINVAL;
    int was_rt = procs[idx].policy != SCHED_OTHER;
    procs[idx].policy = (uint8_t)policy;
    procs[idx].rt_prio = (uint8_t)rt_prio;
    if (policy != SCHED_OTHER) {
        if (!was_rt) procs[idx].rt_run_ns = 0;
        procs[idx].rt_seq = ++g_rt_seq;
    } else if (was_rt) {
        vr_place(idx);   // rejoins the fair pack where it is now, not where it left it
    }
    set_need_resched(1);  // either way the best runnable may have changed
    return 0;
}

int scheduler_get_class(int pid, int *policy, int *rt_prio) {
    int idx = pid_slot(pid);
    if (idx < 0 || procs[idx].state == SCHED_UNUSED)
        return -ESRCH;
    *policy = procs[idx].policy;
    *rt_prio = procs[idx].rt_prio;
    return 0;
}

void sched_class_reset(int slot, int parent) {
    procs[slot].nice = parent >= 0 ? procs[parent].nice : 0;
    procs[slot].policy = SCHED_OTHER;
    procs[slot].rt_prio = 0;
    procs[slot].rt_seq = 0;
    procs[slot].rt_run_ns = 0;
}

// A WAKE THAT OUTRANKS WHAT IS RUNNING asks for a switch on the way out
// of the trap it happened in (scheduler_trap_exit()) -- Linux's
// TIF_NEED_RESCHED. Only a STRICTLY better rank: an RT wake at the same
// priority would preempt on every interrupt and give nothing back. A
// SCHED_OTHER wake beside SCHED_OTHER is vruntime's business -- see
// wake_slot() and wake_preempt().
// g_need_resched: per core (sched_internal.h).

// THE RUN QUEUE: every SCHED_READY slot on one list, so a pick walks
// what can run instead of the whole table -- the structure per-core run
// queues split (docs/smp-design.md, stage 7). Order means nothing: the
// pick ranks by vruntime and by rotation distance (pick_next()).
// `g_nr_running` lets check_one_running() count instead of scan.
static int g_rq_head = -1, g_rq_tail = -1;
static int g_rq_count, g_nr_running;

static void rq_insert(int s) {
    struct sched_process *p = &procs[s];
    __atomic_store_n(&p->rq_next, -1, __ATOMIC_RELEASE);
    p->rq_prev = g_rq_tail;
    if (g_rq_tail >= 0) __atomic_store_n(&procs[g_rq_tail].rq_next, s, __ATOMIC_RELEASE);
    else __atomic_store_n(&g_rq_head, s, __ATOMIC_RELEASE);
    g_rq_tail = s;
    p->rq_on = 1;
    __atomic_store_n(&g_rq_count, g_rq_count + 1, __ATOMIC_RELEASE);
}

static void rq_remove(int s) {
    struct sched_process *p = &procs[s];
    if (p->rq_prev >= 0) __atomic_store_n(&procs[p->rq_prev].rq_next, p->rq_next, __ATOMIC_RELEASE);
    else __atomic_store_n(&g_rq_head, p->rq_next, __ATOMIC_RELEASE);
    if (p->rq_next >= 0) procs[p->rq_next].rq_prev = p->rq_prev;
    else g_rq_tail = p->rq_prev;
    p->rq_on = 0;
    __atomic_store_n(&g_rq_count, g_rq_count - 1, __ATOMIC_RELEASE);
}

void sched_set_state(int slot, enum sched_state s) {
    uint64_t f = sched_lock();
    struct sched_process *p = &procs[slot];
    // Off its wait channel's chain when the wait is over, or the slot is
    // gone -- not when a RUNNING slot that has armed is preempted (READY).
    // A waker unlinks before it wakes, so its chain lock is never wanted
    // twice here.
    if (p->sq_on && ((p->state == SCHED_BLOCKED && s != SCHED_BLOCKED) ||
                     s == SCHED_UNUSED || s == SCHED_ZOMBIE))
        sq_unlink_any(slot);
    if (s == SCHED_READY) { if (!p->rq_on) rq_insert(slot); }
    else if (p->rq_on) rq_remove(slot);
    if (p->state == SCHED_RUNNING) g_nr_running--;
    if (s == SCHED_RUNNING) g_nr_running++;
    p->state = s;
    sched_unlock(f);
}

int sched_ready_count(void) { return __atomic_load_n(&g_rq_count, __ATOMIC_ACQUIRE); }

// Walks the ready list. Under the lock that is the whole story; WITHOUT
// it (scheduler_any_ready(), a hint) each link is read once and checked,
// and the walk is bounded, since a slot taken off and put back behind
// the reader can lead it round again.
#define RQ_FOR_EACH(i, n)                                              \
    for (int i = __atomic_load_n(&g_rq_head, __ATOMIC_ACQUIRE), n = 0;          \
         i >= 0 && i < MAX_PROCS && n <= MAX_PROCS;                             \
         i = __atomic_load_n(&procs[i].rq_next, __ATOMIC_ACQUIRE), n++)

static int is_rt(int idx) { return procs[idx].policy != SCHED_OTHER; }

// What a wake has to beat: an RT process's priority (1..99), or 0 for
// the fair class -- which the kernel context always belongs to.
static int rank_of(int idx) { return is_rt(idx) ? procs[idx].rt_prio : 0; }
static int running_rank(void) {
    return current_index >= 0 ? rank_of(current_index) : 0;
}

// An AP runs processes only: a kernel thread is kernel-context work,
// which stays on the BSP (docs/smp-design.md, stage 5's picks).
// READY and still live on a core is a context switched out a moment ago
// whose core is still on its stack (ctx_on()'s comment): not yet.
static int live_elsewhere(int slot) {
    return __atomic_load_n(&procs[slot].on_core, __ATOMIC_ACQUIRE) >= 0 && slot != current_index;
}

static int runnable_at(int idx) {
    return procs[idx].state == SCHED_READY && !procs[idx].stopped &&
           !(procs[idx].kthread && on_ap()) && !live_elsewhere(idx);
}

// WHO RUNS NEXT WITHIN A LEVEL IS WHOEVER HAS RUN LEAST -- CFS's rule,
// with the strict levels above it kept. Two tie-breaks this replaced
// (a preempted process first, Windows'; a context woken mid-call first,
// NEXT_BUDDY's) each held alone and together had no bound: a disk-bound
// thread and the kernel context handed the CPU to each other on every
// IRQ and every other READY process waited forever (docs/decisions.md,
// "Within a level, whoever has run least runs next"). Both fall out of
// this one: a preempted or long-parked context has used less.
//
// The kernel context takes part at level 0 with its own figure, which
// its idle hlt does not advance -- idle is not running (Linux's idle
// task is no CFS entity at all).
uint64_t kernel_vruntime;
uint64_t g_min_vruntime;      // the pack's floor; never goes back
static int g_kernel_halting;         // inside scheduler_idle_halt()
int scheduler_cpu_halting(void) { return g_kernel_halting; }

static uint64_t vr_credit_ns(void);  // one slice -- see g_timeslice_ms

static uint64_t *vr_of(int rot) {
    return rot == ROT_KERNEL ? &kernel_vruntime : &procs[rot].vruntime;
}

// NICE IS A WEIGHT: CFS's sched_prio_to_weight[], nice -20..19, 1024 at
// 0 and ~1.25x a step, so two busy processes one step apart split the
// CPU ~55/45 whatever their absolute values. vruntime advances by the
// time run over the weight, and least-vruntime-first does the rest.
static const uint32_t NICE_WEIGHT[40] = {
    88761, 71755, 56483, 46273, 36291, 29154, 23254, 18705, 14949, 11916,
     9548,  7620,  6100,  4904,  3906,  3121,  2501,  1991,  1586,  1277,
     1024,   820,   655,   526,   423,   335,   272,   215,   172,   137,
      110,    87,    70,    56,    45,    36,    29,    23,    18,    15,
};

static uint64_t vr_scale(int rot, uint64_t ns) {
    if (rot == ROT_KERNEL) return ns;
    return ns * 1024 / NICE_WEIGHT[procs[rot].nice + 20];
}

// THE REALTIME GUARDS (abi/syscall_abi.h, SYS_SCHED_SETSCHEDULER).
// Throttling is Linux's sched_rt_runtime_us over a fixed 1 s period,
// but WORK-CONSERVING: a throttled RT process still runs when no
// SCHED_OTHER process wants the CPU, since idling then helps nobody.
// The kernel context does not count as wanting it -- it is the idle loop
// far more often than the text shell.
#define RT_PERIOD_NS 1000000000ull
static uint32_t g_rt_runtime_ms = SCHED_RT_RUNTIME_DEFAULT_MS;
static uint32_t g_rt_watchdog_ms = SCHED_RT_WATCHDOG_DEFAULT_MS;
static uint64_t g_rt_period_start_ns, g_rt_used_ns;
static int g_rt_throttled;

static void rt_period_roll(uint64_t now) {
    if (now - g_rt_period_start_ns < RT_PERIOD_NS) return;
    g_rt_period_start_ns = now;
    g_rt_used_ns = 0;
    g_rt_throttled = 0;
}

static int fair_process_runnable(void) {
    RQ_FOR_EACH(i, n)
        if (runnable_at(i) && !is_rt(i)) return 1;
    return 0;
}

static int rt_allowed(void) { return !g_rt_throttled || !fair_process_runnable(); }

// The best runnable RT process -- highest rt_prio, then lowest rt_seq
// -- or -1 for none, or none allowed to run.
static int rt_pick(void) {
    int pick = -1;
    RQ_FOR_EACH(i, n) {
        if (!runnable_at(i) || !is_rt(i)) continue;
        if (pick < 0 || procs[i].rt_prio > procs[pick].rt_prio ||
            (procs[i].rt_prio == procs[pick].rt_prio && procs[i].rt_seq < procs[pick].rt_seq))
            pick = i;
    }
    if (pick >= 0 && !rt_allowed()) return -1;
    return pick;
}

// A context arriving -- woken, spawned, or the kernel after idling --
// goes no further back than one slice behind the pack. Without the
// floor, a process that slept an hour is owed an hour (CFS's
// place_entity()).
void vr_place(int rot) {
    uint64_t c = vr_credit_ns();
    uint64_t floor = g_min_vruntime > c ? g_min_vruntime - c : 0;
    if (*vr_of(rot) < floor) *vr_of(rot) = floor;
}

// Returns who runs next: the best RT process if one may run, otherwise
// the SCHED_OTHER process -- or the kernel context, which is one -- that
// has run least by weighted vruntime. Falls back to ROT_KERNEL when
// nothing else is runnable: a tick that finds nothing resumes the kernel.
// The pick itself, writing nothing the pick reads: `*pv` is the chosen
// one's vruntime when it is a fair pick, else untouched. A lock-free
// event that then declines must have changed nothing
// (scheduler_ap_event_lockless()): g_min_vruntime moved by a pick that
// was never made placed every later wake against it.
static int pick_next(int start, uint64_t *pv_out) {
    int rt = rt_pick();
    if (rt >= 0) return rt;

    // Among equals the next one along from `start` wins, which is the old
    // round-robin: `d` is that distance, 1 .. npos, over positions 0 ..
    // MAX_PROCS-1 (slots) and MAX_PROCS (the kernel's). `start` may be
    // ROT_KERNEL, -1, or past an end that has since moved.
    int pick = ROT_KERNEL, found = 0, pd = 0;
    uint64_t pv = 0;
    int npos = MAX_PROCS + 1;
    int from = (start < 0 || start >= MAX_PROCS) ? MAX_PROCS : start;
    if (!on_ap() && kernel_slot_runnable()) {
        vr_place(ROT_KERNEL);   // idling left it behind: owed one slice, not the idle
        pick = ROT_KERNEL;
        pv = kernel_vruntime;
        pd = (MAX_PROCS - from + npos) % npos;
        if (pd == 0) pd = npos;
        found = 1;   // `found`, not `pick < 0`: ROT_KERNEL is negative
    }
    RQ_FOR_EACH(idx, n) {
        // STOPPED IS CHECKED HERE AND NOWHERE ELSE (runnable_at()).
        // One picker means one place suspension has to be honoured.
        if (!runnable_at(idx) || is_rt(idx)) continue;
        uint64_t v = procs[idx].vruntime;
        int d = (idx - from + npos) % npos;
        if (d == 0) d = npos;
        if (!found || v < pv || (v == pv && d < pd)) { pick = idx; pv = v; pd = d; found = 1; }
    }
    if (!found) return ROT_KERNEL;
    *pv_out = pv;
    return pick;
}

int find_next_runnable(int start) {
    rt_period_roll(clocksource_now_ns());
    uint64_t pv = 0;
    int pick = pick_next(start, &pv);
    if (pv > g_min_vruntime) g_min_vruntime = pv;
    return pick;
}

// Hands the CPU to `idx`, including its floating-point registers.
//
// The FXRSTOR is unconditional and has no matching "was it dirty?"
// check -- that's the eager model fpu.h argues for. It also means a
// process can never observe another process's XMM/x87 contents, which
// the lazy alternative got wrong badly enough to become a CVE.
//
// Note there's no restore for the kernel side (current_index == -1):
// the kernel and kernel/shell/ are built `-mno-sse` and have no FP state to
// preserve. If that ever stops being true, this is one of the two
// places that has to grow a save (the other is scheduler_tick()'s
// outgoing branch), and the ISR path becomes a third -- see fpu.h.
// The kernel slot's half of "the resume frame and the ISR depth travel
// together". Every site that captured kernel_saved_rsp captured only
// half the context before this existed.
static void save_kernel_frame(uint64_t *regs) {
    kernel_saved_rsp = (uint64_t)regs;
}

// A ring of the last few scheduler transitions, for a state that cannot
// happen -- a process left SCHED_RUNNING while nothing is current, say.
// **CONSECUTIVE IDENTICAL ENTRIES COLLAPSE**, or five seconds of an idle
// rotation pushes out the transition that caused the trouble: the ring
// is short on purpose (see CLAUDE.md on a probe outrunning its log).
#define SCHED_TRACE_N 24
static struct sched_trace_ent {
    const char *what;
    int idx, cur, repeat, state, core;
} g_trace[SCHED_TRACE_N];
static unsigned g_trace_n;

void trace_sched(const char *what, int idx) {
    if (g_trace_n) {
        struct sched_trace_ent *last = &g_trace[(g_trace_n - 1) % SCHED_TRACE_N];
        if (last->what == what && last->idx == idx && last->cur == current_index &&
            last->core == (int)spin_cpu_index()) {
            last->repeat++;
            return;
        }
    }
    struct sched_trace_ent *e = &g_trace[g_trace_n % SCHED_TRACE_N];
    e->what = what; e->idx = idx; e->cur = current_index; e->repeat = 0;
    e->core = (int)spin_cpu_index();
    e->state = (idx >= 0 && idx < MAX_PROCS) ? (int)procs[idx].state : -1;
    g_trace_n++;
}

void scheduler_trace_dump(void) {
    unsigned first = g_trace_n > SCHED_TRACE_N ? g_trace_n - SCHED_TRACE_N : 0;
    for (unsigned i = first; i < g_trace_n; i++) {
        struct sched_trace_ent *e = &g_trace[i % SCHED_TRACE_N];
        klog_printf("sched: core %d %s idx=%d cur=%d state=%d (x%d)\n", e->core, e->what,
                    e->idx, e->cur, e->state, e->repeat + 1);
    }
}

// **AT MOST ONE SLOT IS SCHED_RUNNING, AND IT IS `current_index`.** The
// state is only reachable through switch_to(), only the rotation puts a
// process back to READY, and it only does that for the process that is
// current -- so a slot left RUNNING while somebody else is current is
// unschedulable for the rest of the boot. Checked at every switch, ONCE
// per boot: the report is the trace ring above, which is worth nothing
// if a flood has already pushed the cause out of it.
static int g_running_latched;

// Which core runs slot `i`, or -1: RUNNING is one slot per core now.
int sched_core_of(int i) {
    for (int k = 0; k < smp_cpu_count(); k++) {
        const struct cpu *c = smp_cpu_at(k);
        if (c && c->sched_cur == i && (k == 0 || c->state == CPU_RUNNING)) return k;
    }
    return -1;
}

// "Here" and "where it runs" read with interrupts off: a caller moved
// between them called a sibling on the core it had just left "not on
// another core" -- and that sibling is then freed where it stands.
int sched_on_other_core(int i) {
    if (procs[i].state != SCHED_RUNNING) return 0;
    uint64_t f = irq_save();
    int k = sched_core_of(i);
    int other = k >= 0 && k != (int)spin_cpu_index();
    irq_restore(f);
    return other;
}

// smp=run: make core `k` reschedule now rather than at its next tick
// (Linux's resched_curr() + its IPI). Under the kernel lock, so the
// target is in ring 3 or idle and its fields are ours to write.
// Interrupts off: "is it mine" and the flag it sets must be one core's.
void sched_kick_core(int k) {
    uint64_t f = irq_save();
    if (k < 0 || k == (int)spin_cpu_index()) {
        if (k >= 0) set_need_resched(1);
        irq_restore(f);
        return;
    }
    struct cpu *c = (struct cpu *)smp_cpu_at(k);
    if (c && c->state == CPU_RUNNING) {
        c->sched_resched = 1;
        smp_resched(k);
    }
    irq_restore(f);
}

// A slot became runnable: an idle AP takes it now (smp=run only).
static void kick_idle_core(int i) {
    if (!smp_scheduling() || procs[i].kthread) return;
    for (int k = 1; k < smp_cpu_count(); k++) {
        const struct cpu *c = smp_cpu_at(k);
        if (c && c->state == CPU_RUNNING && c->sched_cur < 0 && !c->sched_resched &&
            k != (int)spin_cpu_index()) {
            sched_kick_core(k);
            return;
        }
    }
}

static void check_one_running(const char *where) {
    if (g_running_latched) return;
    // COUNTED FIRST, the table scanned only on a mismatch: every RUNNING
    // slot on a core means as many RUNNING slots as cores running one.
    // Two cores naming one slot would hide an orphan in the count, so
    // that forces the scan too.
    int on = 0, dup = 0;
    for (int k = 0; k < smp_cpu_count(); k++) {
        const struct cpu *c = smp_cpu_at(k);
        if (!c || c->sched_cur < 0 || (k != 0 && c->state != CPU_RUNNING)) continue;
        if (procs[c->sched_cur].state == SCHED_RUNNING) on++;
        for (int j = 0; j < k; j++) {
            const struct cpu *o = smp_cpu_at(j);
            if (o && o->sched_cur == c->sched_cur && (j == 0 || o->state == CPU_RUNNING)) dup = 1;
        }
    }
    if (on == g_nr_running && !dup) return;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state != SCHED_RUNNING || sched_core_of(i) >= 0) continue;
        g_running_latched = 1;
        klog_printf("sched: INVARIANT at %s -- slot %d (pid %d, \"%s\") is RUNNING "
                    "while cur=%d (tgid=%d)\n", where, i, procs[i].pid, procs[i].name,
                    current_index, procs[i].tgid);
        klog_printf("sched: on core %u; slot %d is live on core %d, switched in at %p\n",
                    spin_cpu_index(), i, procs[i].on_core, (void *)procs[i].on_site);
        for (int k = 0; k < smp_cpu_count(); k++) {
            const struct cpu *c = smp_cpu_at(k);
            if (c) klog_printf("sched: core %d runs slot %d, state %u, wedged by vector %ld\n", k,
                               c->sched_cur, (unsigned)c->state, (long)c->wedge_vec - 1);
        }
        scheduler_trace_dump();
        return;
    }
}

// A PROCESS THAT HAS NEVER RUN CANNOT HAVE SAVED A CONTEXT, so it gets
// one built by hand: rsp at its trapframe, rip at the interrupt
// epilogue's pops. The first restore therefore lands exactly where a
// resume always used to -- Linux plants `ret_from_fork` on a fresh
// kernel stack for the same reason. The callee-saved registers are
// zeroed and immediately overwritten by those pops.
void proc_start_context(int slot, const uint64_t *tf) {
    struct kernel_context *k = &procs[slot].kctx;
    procs[slot].on_core = -1;   // never run: live nowhere
    k_memset(k, 0, sizeof *k);
    k->rsp = (uint64_t)tf;
    k->rip = (uint64_t)&isr_start_frame;
    // Its own, not the spawner's: a slot is reused, and both of these
    // describe a context that does not exist yet.
    procs[slot].isr_depth = 0;
    procs[slot].preempt_depth = 0;
    procs[slot].bkl_depth = 1;   // isr_start_frame drops it for a ring-3 frame
#ifdef SAUNAOS_LOCKDEP
    lockdep_task_reset(&procs[slot].ld);
#endif
    procs[slot].parked_in_kernel = 0;
    procs[slot].preempted_in_kernel = 0;
    procs[slot].delay_tsc[0] = procs[slot].delay_tsc[1] = procs[slot].delay_tsc[2] = 0;
    procs[slot].delay_why = SCHED_DELAY_NONE;
    procs[slot].offcpu_tsc = 0;
    procs[slot].left_tsc = 0;
}

// WHO IS BEING SWITCHED AWAY FROM, PASSED IN RATHER THAN READ OFF
// current_index. Every caller but the tick clears current_index before
// it switches -- a blocking process is no longer current, and an
// exiting one is a zombie -- so reading it here would park the outgoing
// context in the KERNEL's slot and leave the real one unreachable.
// That mattered the moment a switch started saving anything; it did
// not before, when it only nominated a frame the caller had already
// written into procs[].
//
// -1 is the kernel context, which is a rotation participant like any
// other. An exiting process passes its own slot: the save is dead
// state, but the switch still has to HAPPEN.
static struct kernel_context *idle_kctx(void) {
    return on_ap() ? &this_cpu()->idle_kctx : &kernel_kctx;
}
static struct kernel_context *kctx_of(int from) {
    return from >= 0 ? &procs[from].kctx : idle_kctx();
}
static int *isr_depth_of(int from) {
    return from >= 0 ? &procs[from].isr_depth
         : on_ap() ? &this_cpu()->idle_isr_depth : &kernel_saved_isr_depth;
}
static int *preempt_of(int from) {
    return from >= 0 ? &procs[from].preempt_depth
         : on_ap() ? &this_cpu()->idle_preempt_depth : &kernel_preempt_depth;
}
static int *bkl_of(int from) {
    return from >= 0 ? &procs[from].bkl_depth
         : on_ap() ? &this_cpu()->idle_bkl_depth : &kernel_bkl_depth;
}
static uint64_t *left_of(int from) {
    return from >= 0 ? &procs[from].left_tsc
         : on_ap() ? &this_cpu()->idle_left_tsc : &kernel_left_tsc;
}
static uint64_t *offcpu_of(int from) {
    return from >= 0 ? &procs[from].offcpu_tsc
         : on_ap() ? &this_cpu()->idle_offcpu_tsc : &kernel_offcpu_tsc;
}

// **THE CPU MOVES HERE, not in an epilogue.** process_context_save()
// returns 0 on the way out and non-zero when this context is resumed,
// which is the entire switch: everything after the save runs on the
// outgoing stack and is abandoned by the restore, and everything the
// resumed context needs was installed for it by whoever resumed it.
//
// So the order matters and is not arbitrary: park OURSELVES first, then
// install the incoming context, then go. Installing first would run the
// rest of this function under the incoming process's CR3.
// Depth, not a flag: sections nest, and an inner one must not re-enable
// preemption an outer one is relying on. See api/scheduler.h.
//
// **AND IT TRAVELS WITH THE CONTEXT, exactly as isr_depth does.** A
// switch moves the CPU on the spot now, so a process that parks inside
// a guarded section parks WITH IT RAISED -- and as one global that
// left the whole machine unpreemptible for as long as somebody else
// was running. It presented as three tty tests failing and the suite
// taking 24s instead of 0.3s. docs/blocking-design.md names this as
// the fourth instance of "a global describing a per-context property",
// beside g_next_kernel_rsp, g_isr_depth and the old resume slot; this
// is it being cured the same way they were. And PER CORE since SMP
// stage 7 (struct cpu's preempt_depth), so a core holding a spinlock
// outside the kernel lock raises its own guard and nobody else's.

// The globals that describe the CURRENT context rather than the
// machine. Saved into the outgoing slot and reloaded from the incoming
// one on every switch, which is the whole of what makes them per
// context. kernel_* hold the kernel context's, since it has no slot.
// Which delay bucket a slot leaving the CPU is about to fill: READY is
// waiting for the CPU; a park on the disk or a kernel lock is that; any
// other park is a wait by choice and counts nowhere -- delayacct's split.
// MID-CALL ONLY: flock() also parks with SCHED_WAIT_LOCK, at the call's
// entry, and a lock a program asked to wait for is its choice.
static uint8_t delay_why_leaving(const struct sched_process *p) {
    if (p->state == SCHED_READY) return SCHED_DELAY_RUNQ;
    if (p->state != SCHED_BLOCKED || !p->parked_in_kernel) return SCHED_DELAY_NONE;
    if (p->wait_reason == SCHED_WAIT_DISK) return SCHED_DELAY_DISK;
    if (p->wait_reason == SCHED_WAIT_LOCK) return SCHED_DELAY_LOCK;
    return SCHED_DELAY_NONE;
}

static void delay_close(struct sched_process *p, uint64_t now) {
    if (p->delay_why != SCHED_DELAY_NONE && now > p->delay_mark)
        p->delay_tsc[p->delay_why] += now - p->delay_mark;
    p->delay_why = SCHED_DELAY_NONE;
}

// Every way a parked slot is made READY passes here, so it is also the
// kernel trace's wakeup point.
void sched_delay_woken(int slot) {
    struct sched_process *p = &procs[slot];
    KTRACE(KTRACE_G_WAKEUP, KTRACE_EV_WAKEUP, p->pid, (uintptr_t)p->wait_chan,
           p->wait_reason, p->tgid);
    uint64_t now = arch_rdtsc();
    delay_close(p, now);
    p->delay_why = SCHED_DELAY_RUNQ;
    p->delay_mark = now;
}

static void context_save_globals(int from) {
    *isr_depth_of(from) = isr_depth_get();
    *preempt_of(from) = this_cpu_read(preempt_depth);
    *bkl_of(from) = bkl_depth();
    uint64_t now = arch_rdtsc();
    *left_of(from) = now;
    if (from >= 0) {
        procs[from].delay_why = delay_why_leaving(&procs[from]);
        procs[from].delay_mark = now;   // the same read: accounting costs no clock
    }
}

// THE KERNEL LOCK'S DEPTH IS NOT LOADED HERE but last, by
// switch_finish(): loading a 0 releases the lock, and the switch is not
// done with what it protects until then.
static void context_load_globals(int to) {
    isr_depth_set(*isr_depth_of(to));
    this_cpu_write(preempt_depth, *preempt_of(to));
    uint64_t *left = left_of(to);
    uint64_t *off  = offcpu_of(to);
    uint64_t now = arch_rdtsc();
    if (*left && now > *left) *off += now - *left;
    *left = 0;
    if (to >= 0) delay_close(&procs[to], now);
}

uint64_t scheduler_offcpu_tsc(void) {
    return *offcpu_of(current_index);
}

// THE TIME SLICE IS A DEADLINE, NOT A TICK COUNT (kernel.timeslice_ms).
// Every switch starts a new one; a timer event that finds it over, with
// somebody else runnable, rotates. On the periodic path that is checked
// once a tick, so a slice shorter than a tick is a tick -- which at the
// old 100 Hz is exactly the rotate-every-tick this replaced.
static uint32_t g_timeslice_ms = SCHED_TIMESLICE_DEFAULT_MS;
#define g_slice_start_ns (sched_here()->slice_start_ns)   // per core
#define g_slice_end_ns   (sched_here()->slice_end_ns)

static void slice_restart(void) {
    g_slice_start_ns = clocksource_now_ns();
    g_slice_end_ns = g_slice_start_ns + (uint64_t)g_timeslice_ms * 1000000ull;
}

// A DEADLINE WAKE PREEMPTS AN EQUAL. A process whose own timeout came
// due runs NOW rather than when the busy one's slice ends -- once that
// one has had WAKE_GRAN_NS of its slice (CFS's wakeup granularity);
// short of it, the slice is cut to it. Without this a deadline kept to
// the microsecond was followed by up to a slice of waiting for the CPU.
// ONLY for deadlines: an interrupt-driven wake keeps the strictly-better
// rule in wake_slot() (docs/decisions/kernel.md, "A wake preempts only
// from a better level"), since one armed deadline cannot be "every
// interrupt".
#define WAKE_GRAN_NS 1000000ull

static uint64_t vr_credit_ns(void) { return (uint64_t)g_timeslice_ms * 1000000ull; }

// Has `i` run less than what is running now, by more than a
// granularity? The in-progress slice counts -- the running one has
// been using the CPU since g_run_start_ns.
static int vr_behind_running(int i) {
    uint64_t now = clocksource_now_ns();
    uint64_t run = now > g_run_start_ns ? now - g_run_start_ns : 0;
    uint64_t cur = current_index >= 0
                 ? procs[current_index].vruntime + vr_scale(current_index, run)
                 : kernel_vruntime + (g_kernel_halting ? 0 : run);
    return procs[i].vruntime + WAKE_GRAN_NS < cur;
}

static void wake_preempt(int i) {
    if (is_rt(i)) procs[i].rt_seq = ++g_rt_seq;            // the back of its queue
    else vr_place(i);
    if (current_index < 0 && clockevent_in_idle()) return; // the idle path hands over already
    if (current_index < 0 && on_ap()) return;             // an idle AP takes it at its next event
    if (rank_of(i) > running_rank()) { set_need_resched(1); return; }
    if (is_rt(i) || running_rank() > 0) return;            // equal RT waits; fair never beats RT
    if (!vr_behind_running(i)) return;                     // it has had its share
    uint64_t gran_end = g_slice_start_ns + WAKE_GRAN_NS;
    if (clocksource_now_ns() >= gran_end) {
        set_need_resched(1);
    } else if (gran_end < g_slice_end_ns) {
        g_slice_end_ns = gran_end;
    }
}

uint32_t scheduler_timeslice_ms(void) { return g_timeslice_ms; }

int scheduler_set_timeslice_ms(uint32_t ms) {
    if (ms < SCHED_TIMESLICE_MIN_MS || ms > SCHED_TIMESLICE_MAX_MS) return 0;
    g_timeslice_ms = ms;
    return 1;
}

uint32_t scheduler_rt_runtime_ms(void) { return g_rt_runtime_ms; }
uint32_t scheduler_rt_watchdog_ms(void) { return g_rt_watchdog_ms; }

int scheduler_set_rt_runtime_ms(uint32_t ms) {
    if (ms < 1 || ms > RT_PERIOD_NS / 1000000) return 0;
    g_rt_runtime_ms = ms;
    return 1;
}

int scheduler_set_rt_watchdog_ms(uint32_t ms) {
    if (ms > SCHED_RT_WATCHDOG_MAX_MS) return 0;
    g_rt_watchdog_ms = ms;
    return 1;
}

// Lock-free callers too (the idle event, clockevent), so a HINT: the
// walk is bounded and the empty case reads one word.
int scheduler_any_ready(void) {
    int count = sched_ready_count();
    if (!count) return 0;
    int cur = current_index, seen = 0;
    RQ_FOR_EACH(i, n) {
        if (i != cur && runnable_at(i)) return 1;
        seen++;
    }
    // A slot put back at the tail while this stood on it ends the walk
    // early (rq_insert()'s -1): fewer seen than listed answers "ready",
    // and the caller's locked path decides.
    return seen < count;
}

int scheduler_kernel_running(void) { return current_index < 0; }

// Context switches made -- the lockup detector's "the scheduler moved".
static uint64_t g_switches;
uint64_t scheduler_switch_count(void) { return g_switches; }

// The hung-task check's question (kernel/lockup.h): a slot parked INSIDE
// the kernel on a lock or a disk since before `before_ns` and not yet
// reported. Marks it reported, so each park is said once.
int scheduler_hung_find(uint64_t before_ns, struct sched_hung *out) {
    for (int i = 0; i < MAX_PROCS; i++) {
        struct sched_process *p = &procs[i];
        if (p->state != SCHED_BLOCKED || !p->parked_in_kernel || p->hung_reported) continue;
        if (p->wait_reason != SCHED_WAIT_LOCK && p->wait_reason != SCHED_WAIT_DISK) continue;
        if (!p->parked_at_ns || p->parked_at_ns >= before_ns) continue;
        p->hung_reported = 1;
        out->pid = p->pid;
        out->reason = p->wait_reason;
        out->parked_at_ns = p->parked_at_ns;
        out->rsp = p->kctx.rsp;
        k_strlcpy(out->name, p->name, sizeof out->name);
        return 1;
    }
    return 0;
}

// **THE SCHEDULER LOCK** (sched_internal.h): taken here, recursive per
// core, and let go by the switch it leads into.
SPINLOCK_COUNTED(g_sched_lock, "sched");
static unsigned g_sched_lock_faults;

static void sched_lock_fault(const char *what, int depth, void *site) {
    if (g_sched_lock_faults++ < 6)
        klog_printf(KLOG_ERR "sched: %s (lock depth %d on core %u, from %p)\n", what, depth,
                    spin_cpu_index(), site);
}

uint64_t sched_lock(void) {
    uint64_t f = irq_save();
    struct cpu *c = sched_here();
    if (c->sched_lock_depth++ == 0) spin_lock_noguard(&g_sched_lock);
    return f;
}

void sched_unlock(uint64_t flags) {
    struct cpu *c = sched_here();
    if (c->sched_lock_depth <= 0) sched_lock_fault("unlock without the lock", c->sched_lock_depth,
                                              __builtin_return_address(0));
    else if (--c->sched_lock_depth == 0) spin_unlock_noguard(&g_sched_lock);
    irq_restore(flags);
}

int sched_locked_here(void) { return sched_here()->sched_lock_depth > 0; }
unsigned scheduler_lock_faults(void) { return g_sched_lock_faults; }


// A switch is entered holding the lock ONCE and leaves without it: the
// incoming context resumes holding nothing of it, and a depth above one
// is a caller that would lose its own hold.
static void sched_switch_release(void) {
    struct cpu *c = sched_here();
    if (c->sched_lock_depth != 1) {
        sched_lock_fault(c->sched_lock_depth <= 0 ? "switch without the lock"
                                                  : "switch holding the lock twice",
                         c->sched_lock_depth, __builtin_return_address(0));
        if (c->sched_lock_depth <= 0) return;
    }
    c->sched_lock_depth = 0;
    spin_unlock_noguard(&g_sched_lock);
}

// The last of a switch before the restore: the incoming context's depth
// in the kernel lock (0 releases it, bkl_load_depth()), then this lock.
static void switch_finish(int bkl_to) {
    bkl_load_depth(bkl_to);
    sched_switch_release();
}

// **A CONTEXT IS LIVE ON AT MOST ONE CORE** (Linux's p->on_cpu): stamped at
// each restore, cleared at each save, and a restore of a stamped one is
// reported -- two cores on one kernel stack, the failure that shows only
// later as a smashed stack.
// **AND IT STOPS BEING LIVE ONLY ONCE NOTHING RUNS ON ITS STACK**: the
// mark goes in process_context_restore_release(), after RSP has moved,
// so the scheduler lock can be let go before the restore. A READY
// context still marked live is not picked (find_next_runnable()), and
// its slot is not reused (slot_claim()).
static int g_kctx_on_core = 0;            // the BSP's kernel context runs at boot
static uintptr_t g_kctx_on_site;
static unsigned g_double_resumes;
static int g_live_sink;                   // a self-switch's: the context stays live
unsigned scheduler_double_resumes(void) { return g_double_resumes; }
static volatile int *live_of(int slot) {
    if (slot >= 0) return &procs[slot].on_core;
    return on_ap() ? &this_cpu()->idle_live : &g_kctx_on_core;
}
static void ctx_on(int slot, uintptr_t site) {
    int *oc = slot >= 0 ? &procs[slot].on_core : (on_ap() ? 0 : &g_kctx_on_core);
    uintptr_t *os = slot >= 0 ? &procs[slot].on_site : &g_kctx_on_site;
    if (!oc) return;
    if (*oc >= 0 && *oc != (int)spin_cpu_index() && g_double_resumes++ < 6)
        klog_printf(KLOG_ERR "sched: DOUBLE RESUME of slot %d (pid %d) on core %u: still live on core %d, "
                    "switched in at %p; now from %p\n", slot, slot >= 0 ? procs[slot].pid : 0,
                    spin_cpu_index(), *oc, (void *)*os, (void *)site);
    *oc = (int)spin_cpu_index();
    *os = site;
}

void switch_to(int from, int idx) {
    __atomic_fetch_add(&g_switches, 1, __ATOMIC_RELAXED);
    this_cpu_add(sched_switches, 1);
    check_one_running("switch_to");
    trace_sched("switch_to", idx);
    kstack_verify(idx);   // before trusting anything else about this slot
    KTRACE(KTRACE_G_SWITCH, KTRACE_EV_SWITCH, from >= 0 ? procs[from].pid : 0,
           from >= 0 ? procs[from].state : 0, procs[idx].pid, procs[idx].tgid);
    // A slot selected before anything gave it somewhere to resume. Was
    // a zero RSP handed to isr_common; now it is a jump to address 0,
    // which is no more diagnosable, so it is still checked here.
    if (!procs[idx].kctx.rip) {
        klog_printf("SLOT HAS NO SAVED CONTEXT: idx=%d pid=%d state=%d \"%s\"\n",
                    idx, procs[idx].pid, procs[idx].state, procs[idx].name);
        vga_printf("\nSLOT HAS NO SAVED CONTEXT: idx=%d pid=%d state=%d \"%s\"\n",
                   idx, procs[idx].pid, procs[idx].state, procs[idx].name);
        __asm__ volatile ("ud2");
    }
    // A FREED ADDRESS SPACE IS NEVER LOADED: its frames may already be
    // another process's page tables. An exit gives its slot up (0, the
    // kernel's tables) before it destroys its own.
    if (procs[idx].pml4_phys && !pmm_frame_is_used(procs[idx].pml4_phys)) {
        klog_printf("SWITCH INTO A FREED ADDRESS SPACE: idx=%d pid=%d state=%d \"%s\" pml4=%#lx\n",
                    idx, procs[idx].pid, procs[idx].state, procs[idx].name,
                    procs[idx].pml4_phys);
        vga_printf("\nSWITCH INTO A FREED ADDRESS SPACE: idx=%d pid=%d \"%s\"\n",
                   idx, procs[idx].pid, procs[idx].name);
        __asm__ volatile ("ud2");
    }

    if (process_context_save(kctx_of(from)) != 0) return;  // resumed: back
    volatile int *live = from == idx ? &g_live_sink : live_of(from);

    context_save_globals(from);
    fpu_restore(procs[idx].fpu);
    // The thread pointer, and it has to be here: iretq reloads CS and SS
    // and leaves the hidden segment bases alone, so without this every
    // thread would read the last-scheduled thread's `__thread` storage.
    arch_set_fs_base(procs[idx].fs_base);
    vmm_switch_address_space(procs[idx].pml4_phys ? procs[idx].pml4_phys
                                                  : vmm_kernel_pml4_phys());
    gdt_set_kernel_stack(kernel_stack_top(idx));
    context_load_globals(idx);
    sched_set_state(idx, SCHED_RUNNING);
    procs[idx].preempted_in_kernel = 0;
    set_current_index(idx);
    set_rotation_pos(idx);
    slice_restart();
    // A new slice, and the tick back if it was stopped. An AP's tick is
    // its own periodic LAPIC timer (smp.c), not the clockevent.
    if (!on_ap()) clockevent_reprogram();
    ctx_on(idx, (uintptr_t)__builtin_return_address(0));
    switch_finish(*bkl_of(idx));
    process_context_restore_release(&procs[idx].kctx, 1, live);
}

// The ROT_KERNEL counterpart to switch_to(): hand the CPU back to the
// kernel context. No CR3 switch, no RSP0 repoint and no FP restore --
// see the ROT_KERNEL comment above for why the kernel needs none of the
// three. Kept as its own function purely so both callers (the tick and
// the exit path) state the same thing once.
void switch_to_kernel(int from) {
    check_one_running("to_kernel");
    trace_sched("to_kernel", -1);
    int was = from;
    set_current_index(-1);
    set_rotation_pos(ROT_KERNEL);
    // The LEGACY loader's thread pointer -- the same
    // two-owners-one-representation shape the heap and the cwd have
    // (scheduler.h): a process run by elf_run.c has no slot to keep one
    // in, and without this the last scheduled thread's %fs would still
    // be loaded when it resumed.
    arch_set_fs_base(on_ap() ? 0 : kernel_fs_base);
    slice_restart();
    if (!on_ap()) clockevent_reprogram();
    // ALREADY THE KERNEL -- the overwhelmingly common case, since a tick
    // with nothing spawned lands here every time. It used to nominate
    // the frame the tick had just captured, which was a no-op by a
    // longer route; now there is nothing to switch to.
    if (was < 0) { sched_switch_release(); return; }
    __atomic_fetch_add(&g_switches, 1, __ATOMIC_RELAXED);
    this_cpu_add(sched_switches, 1);
    KTRACE(KTRACE_G_SWITCH, KTRACE_EV_SWITCH, procs[was].pid, procs[was].state, 0, 0);
    // find_next_runnable()'s FALLBACK returns ROT_KERNEL without asking
    // kernel_slot_runnable(), so it can reach here before the kernel
    // context has ever been left -- and a restore then jumps to 0.
    // Measured not to fire under the interrupt gate; kept because the
    // fallback is genuinely unguarded.
    if (!idle_kctx()->rip) {
        klog_printf("KERNEL SLOT HAS NO SAVED CONTEXT (armed=%d, core %u)\n",
                    process_context_is_armed(), spin_cpu_index());
        vga_printf("\nKERNEL SLOT HAS NO SAVED CONTEXT (armed=%d)\n",
                   process_context_is_armed());
        __asm__ volatile ("ud2");
    }
    if (process_context_save(&procs[was].kctx) != 0) return; // resumed: back
    context_save_globals(was);
    context_load_globals(-1);
    ctx_on(-1, (uintptr_t)__builtin_return_address(0));
    switch_finish(*bkl_of(-1));
    process_context_restore_release(idle_kctx(), 1, live_of(was));
}

int scheduler_mark_current_ready(void) {
    int pid = scheduler_current_pid();
    if (pid <= 0 || current_index < 0) return 0;
    procs[current_index].ready = 1;
    return 1;
}

int scheduler_max_procs(void) { return g_max_procs; }
int scheduler_live_count(void) { return alive_count; }

// CPU time is billed by MEASURING IT, not by counting ticks.
//
// The history is worth the paragraph, because the obvious
// implementation is the wrong one and this kernel shipped it twice.
// Billing was `cpu_ticks++` from the timer, and SYS_YIELD rescheduled
// through the same function -- so a yield, which elapses microseconds,
// charged a whole 10ms tick. A polling app yields about once per tick,
// so it billed itself 100 ticks a second against a 100Hz clock: a
// stable, entirely fake 100%, for every polling app simultaneously,
// which one CPU obviously cannot do. Charging only from the timer
// fixed the impossibility and replaced it with the opposite error --
// sampled accounting cannot see a process that runs for less than a
// tick, so a client drawing one frame every 10ms read 0%.
//
// Both errors have the same root: a TICK COUNT is not a DURATION. So
// the scheduler asks a clock (kernel/clocksource.h) how much time
// actually passed, and charges that. The PIT-backed source makes this
// no better than before; the TSC-backed one, registered once the CPU
// is calibrated, makes it exact to the nanosecond. Nothing here knows
// which is live, which is the point of the interface.
//
// The invariant: every path that stops running the current process
// calls bill_current() BEFORE changing current_index. Miss one and
// that slice is credited to whoever runs next.
// The same slices, totalled machine-wide, so a caller can divide rather
// than sum per-process percentages -- which miss a process that started
// and exited between two samples. Read through QUERY_CPULOAD.
static uint64_t g_proc_ns = 0;   // charged to some slot
static uint64_t g_kernel_ns = 0; // charged to nobody: the idle halt, or the text shell

// RT time, against both guards. A demotion here is seen by the very
// pick that follows, since every pick bills first.
static void rt_charge(int idx, uint64_t slice, uint64_t now) {
    rt_period_roll(now);
    g_rt_used_ns += slice;
    if (!g_rt_throttled && g_rt_runtime_ms < RT_PERIOD_NS / 1000000 &&
        g_rt_used_ns >= (uint64_t)g_rt_runtime_ms * 1000000ull) {
        g_rt_throttled = 1;
        klog_printf("sched: realtime throttled -- %u ms of RT in this second; "
                    "ordinary processes get the rest\n", g_rt_runtime_ms);
    }
    procs[idx].rt_run_ns += slice;
    if (g_rt_watchdog_ms && procs[idx].rt_run_ns >= (uint64_t)g_rt_watchdog_ms * 1000000ull) {
        klog_printf("sched: pid %d (\"%s\") ran %u ms realtime without blocking -- "
                    "moved to SCHED_OTHER\n", procs[idx].pid, procs[idx].name,
                    (unsigned)(procs[idx].rt_run_ns / 1000000));
        procs[idx].policy = SCHED_OTHER;
        procs[idx].rt_prio = 0;
        vr_place(idx);
    }
}

// Is a guard due for the RT process on the CPU? The tick asks, since a
// FIFO process is otherwise never rotated and so never billed.
static int rt_guard_due(int idx, uint64_t now) {
    uint64_t run = now > g_run_start_ns ? now - g_run_start_ns : 0;
    if (g_rt_watchdog_ms && procs[idx].rt_run_ns + run >= (uint64_t)g_rt_watchdog_ms * 1000000ull)
        return 1;
    if (now - g_rt_period_start_ns >= RT_PERIOD_NS) return 0;
    return g_rt_used_ns + run >= (uint64_t)g_rt_runtime_ms * 1000000ull &&
           g_rt_runtime_ms < RT_PERIOD_NS / 1000000 && fair_process_runnable();
}

void bill_current(void) {
    uint64_t now = clocksource_now_ns();
    if (now > g_run_start_ns) {
        uint64_t slice = now - g_run_start_ns;
        if (current_index >= 0) {
            // STORED WHOLE: QUERY_CORELOAD reads it from another core.
            struct cpu *me = sched_here();
            __atomic_store_n(&me->busy_ns, me->busy_ns + slice, __ATOMIC_RELAXED);
            procs[current_index].cpu_ns += slice;
            procs[current_index].vruntime += vr_scale(current_index, slice);
            __atomic_fetch_add(&g_proc_ns, slice, __ATOMIC_RELAXED);   // every core's, some without the kernel lock
            if (is_rt(current_index)) rt_charge(current_index, slice, now);
        } else {
            __atomic_fetch_add(&g_kernel_ns, slice, __ATOMIC_RELAXED);
            if (!g_kernel_halting && !on_ap()) kernel_vruntime += slice;
        }
    }
    // Reset unconditionally, including when the KERNEL context was
    // running: leaving the old start in place would hand the next
    // process everything the kernel just spent.
    g_run_start_ns = now;
}

void scheduler_cpu_time(uint64_t *proc_ns, uint64_t *kernel_ns) {
    // Billed first, so the slice in progress is not missing from the
    // answer -- without it a machine running ONE busy process reports
    // whatever it had at the last rotation, up to 10 ms stale.
    bill_current();
    if (proc_ns) *proc_ns = g_proc_ns;
    if (kernel_ns) *kernel_ns = g_kernel_ns;
}

uint64_t scheduler_test_vr_scale(int idx, uint64_t ns) { return vr_scale(idx, ns); }
void scheduler_test_rt_charge(int idx, uint64_t ns) { rt_charge(idx, ns, clocksource_now_ns()); }
void scheduler_test_rt_budget_reset(void) { g_rt_used_ns = 0; g_rt_throttled = 0; }

// The rotation, shared by the timer and by SYS_YIELD. They are the same
// operation now that neither one is where billing happens -- see
// bill_current() above.
static void scheduler_rotate(uint64_t *regs);
static void rotate(uint64_t *regs, int to_idle);

// The most any runnable fair process has run: where scheduler_yield()
// puts a yielder, the back of the pack.
static uint64_t fair_top(int except) {
    uint64_t top = 0;
    RQ_FOR_EACH(i, n)
        if (i != except && runnable_at(i) && !is_rt(i) && procs[i].vruntime > top)
            top = procs[i].vruntime;
    return top;
}

// Whether a timer event should rotate: the slice is over -- or the
// kernel context is IDLING with a process ready, which should run now
// rather than when the idle loop's slice would have ended.
// THE IDLE KERNEL'S HANDOVER IS A YIELD: it goes to the back of the pack.
// A halt does not advance kernel_vruntime, so left where it was the
// kernel stayed the least-run and the picker handed the CPU straight back
// to the idle loop -- ~23,000 times a second until its slice ran out, and
// a lone busy process got half the CPU. It still gets one idle pass per
// process slice (input, network, the disk's write-back), as before.
static int timer_wants_rotation(uint64_t now) {
    if (current_index < 0 && on_ap()) return g_need_resched || scheduler_any_ready();
    if (current_index < 0 && clockevent_in_idle()) {
        if (!scheduler_any_ready()) return 0;
        uint64_t top = fair_top(-1);
        if (kernel_vruntime < top) kernel_vruntime = top;
        return 1;
    }
    if (g_need_resched) return 1;
    if (current_index >= 0 && procs[current_index].policy == SCHED_FIFO && now >= g_slice_end_ns) {
        // A FIFO PROCESS HAS NO SLICE, only the guards. The slice end
        // still comes round as the cadence they are checked at -- and
        // must be pushed on, or a deadline in the past refires at once.
        if (rt_guard_due(current_index, now)) return 1;
        slice_restart();
        clockevent_reprogram();
        return 0;
    }
    return now >= g_slice_end_ns;   // SCHED_RR and SCHED_OTHER alike
}

// **THE BSP's KERNEL CONTEXT HOLDS THE KERNEL LOCK WHENEVER IT RUNS**, so
// one that busy-waits -- a KTEST polling for a process's exit -- keeps an
// AP's process out of the kernel for good under smp=run (on one core the
// tick would preempt it for that process). So at a tick where it COULD
// be preempted -- the guard down, not nested in another interrupt, no
// legacy process in flight -- any core queued for the lock goes first.
// That is the preemption it would have had (bkl.h's argument), so the
// context-wide globals are put in their ring-3 state for the other core,
// as a switch would, and back after.
static void kernel_let_waiters_in(void) {
    if (current_index >= 0 || on_ap() || !smp_scheduling()) return;
    if (this_cpu_read(preempt_depth) > 0 || isr_depth_get() != 1 || process_context_is_armed()) return;
    if (!bkl_contended()) return;
    int isr = isr_depth_get();
    isr_depth_set(0);
    int d = bkl_drop_all();
    bkl_retake(d);   // behind every core already queued: FIFO
    isr_depth_set(isr);
}

void scheduler_tick(uint64_t *regs) {
    // BEFORE the rotation, and outside scheduler_rotate()'s armed
    // check: a sleeper's deadline has nothing to do with whether the
    // scheduler is currently rotating, and a woken process wants to be
    // eligible for the switch this very tick rather than the next one.
    uint64_t now = clocksource_now_ns();
    scheduler_wake_timers(now);
    if (timer_wants_rotation(now)) scheduler_rotate(regs);
    else kernel_let_waiters_in();
}

void scheduler_timer_event(uint64_t *regs, uint64_t now) {
    scheduler_wake_timers(now);
    if (timer_wants_rotation(now)) scheduler_rotate(regs);
    else kernel_let_waiters_in();
}

// An AP's own tick and the reschedule IPI (smp.c): the rotation, nothing
// else -- sleepers' deadlines, the watchdog and the idle work stay with
// the BSP's clockevent.
void scheduler_ap_event(uint64_t *regs) {
    this_cpu_add(sched_events, 1);
    // **A PROCESS IN A LONG CALL GIVES THE KERNEL LOCK UP BY BEING
    // PREEMPTED**, Linux's cond_resched() -- which is schedule(). With
    // nothing else ready this AP never rotates, so fsck's whole-volume
    // walk held every other core out of the kernel for over a second.
    // A real switch to this AP's idle context, whose interrupt exit
    // releases the lock, and which picks the process up again at its
    // next tick: dropping the lock in place, with no switch, left
    // per-context state for another core to trample (a GP fault in the
    // BSP's kernel context, 1 run in 6). AHEAD of the ordinary rotation,
    // which may pick this process again, or another inside a call, and
    // so release nothing. rotate() keeps its own guards.
    if (current_index >= 0 && !(regs[TF_CS] & 3) && bkl_contended()) { rotate(regs, 1); return; }
    if (timer_wants_rotation(clocksource_now_ns())) scheduler_rotate(regs);
}

// ---- an AP's scheduling WITHOUT THE KERNEL LOCK (smp-design.md, stage 7)
//
// An AP's tick or reschedule, or a yield, that finds nothing to do, or
// that switches between two contexts holding none of the kernel lock --
// a process in ring 3, this core's idle loop -- needs the scheduler lock
// alone. Everything else DECLINES, having changed nothing, and goes the
// ordinary way. A context switched out here is saved at kernel-lock depth
// 0 and resumes into a frame that releases nothing (bkl_load_depth()).
static volatile int g_lockless_sched = 1;   // kernel.lockless_sched
int  scheduler_lockless_sched_enabled(void) { return g_lockless_sched; }
void scheduler_lockless_sched_set(int on) { g_lockless_sched = on ? 1 : 0; }

// MAY THE RUNNING CONTEXT BE LEFT RUNNING, OR SWITCHED OUT, WITHOUT THE
// KERNEL LOCK? A fair process with nothing the ordinary way back to ring
// 3 does pending: a signal, a re-issue. An RT one goes the ordinary way
// for its guards. What may be switched IN is lockless_rotate()'s to say.
static int lockless_ctx_ok(int i) {
    const struct sched_process *p = &procs[i];
    return p->policy == SCHED_OTHER && !p->kthread && !p->syscall_reissue &&
           !(p->pending & ~p->blocked);
}

// Whether this core is somewhere the kernel lock is not held: depth 0,
// no guard, not inside another interrupt -- ring 3 or the idle loop.
static int lockless_here(uint64_t *regs) {
    if (!g_lockless_sched || !on_ap() || !smp_scheduling()) return 0;
    if (this_cpu_read(bkl_depth) || this_cpu_read(preempt_depth) || isr_depth_get() > 1) return 0;
    int cur = current_index;
    return cur < 0 || (regs[TF_CS] & 3);
}

// The rotation, under the scheduler lock (held, once) with interrupts
// off. `yielding`: SYS_YIELD's move to the back of the pack first.
// Returns 0, the lock RELEASED and nothing changed, when the running
// context must go the ordinary way; otherwise 1 once it has switched (or
// found nothing better), the lock let go by the switch.
static int lockless_rotate(uint64_t *regs, int yielding, void (*ack)(uint64_t *)) {
    int cur = current_index;
    if (cur >= 0 && !lockless_ctx_ok(cur)) { sched_switch_release(); return 0; }
    // **BILLED BEFORE THE PICK**, as rotate() does: unbilled, the slice
    // that just ended leaves the running process looking least-run, and it
    // is picked again every time.
    bill_current();
    rt_period_roll(clocksource_now_ns());   // the RT budget's clock, as find_next_runnable() rolls it
    uint64_t vr_billed = cur >= 0 ? procs[cur].vruntime : 0;
    if (cur >= 0) {
        if (yielding) {
            uint64_t top = fair_top(cur);
            if (procs[cur].vruntime > top) top = procs[cur].vruntime;
            procs[cur].vruntime = top;
        }
        sched_set_state(cur, SCHED_READY);   // as rotate() does, so the pick may choose it again
    }
    uint64_t pv = 0;
    int next = pick_next(rotation_pos, &pv);
    if (next == cur || (next == ROT_KERNEL && cur < 0)) {
        // Nothing better: carry on, a new slice. No switch, no lock.
        if (cur >= 0) sched_set_state(cur, SCHED_RUNNING);
        if (pv > g_min_vruntime) g_min_vruntime = pv;
        if (ack) { ack(regs); this_cpu_add(sched_events, 1); }
        set_need_resched(0);
        slice_restart();
        this_cpu_add(sched_lockless, 1);
        sched_switch_release();
        return 1;
    }
    // **ONLY INTO A CONTEXT THAT HOLDS NONE OF THE KERNEL LOCK** -- one
    // this path switched out, or the idle loop saved by it. Taking the
    // lock at the end of the switch for one that does was tried and
    // panicked the KTEST suite 2 runs in 2 (docs/decisions/kernel.md): the core then
    // waits for the lock on the outgoing context's stack, with the
    // incoming one already RUNNING. So it declines, and the ordinary
    // path takes the event.
    if (next == ROT_KERNEL ? this_cpu()->idle_bkl_depth != 0
                           : procs[next].bkl_depth != 0 || procs[next].preempted_in_kernel) {
        if (cur >= 0) {
            sched_set_state(cur, SCHED_RUNNING);
            procs[cur].vruntime = vr_billed;   // a yield's move is the ordinary path's to make
        }
        sched_switch_release();
        return 0;
    }
    if (pv > g_min_vruntime) g_min_vruntime = pv;
    if (ack) { ack(regs); this_cpu_add(sched_events, 1); }
    set_need_resched(0);
    if (cur >= 0) {
        procs[cur].kernel_rsp = (uint64_t)regs;
        kstack_verify(cur);
        fpu_save(procs[cur].fpu);
        procs[cur].preempted_in_kernel = 0;
        trace_sched("rotate_out", cur);
    }
    this_cpu_add(sched_lockless, 1);
    if (next == ROT_KERNEL) switch_to_kernel(cur);
    else switch_to(cur, next);
    return 1;
}

int scheduler_ap_event_lockless(uint64_t *regs, void (*ack)(uint64_t *regs)) {
    if (!lockless_here(regs)) return 0;
    // NOT the contended case scheduler_ap_event() opens with: that is a
    // process inside a call, which lockless_here() has already refused.
    int cur = current_index;
    uint64_t now = clocksource_now_ns();   // before the lock: a PM-timer read is an exit
    // **NOTHING TO DO IS DECIDED WITH NO LOCK AT ALL** -- Linux's idle loop
    // reading need_resched. A process made READY after this read is woken
    // with a kick (kick_idle_core(), under the lock), whose IPI is this
    // core's next event; a signal posted to the running one kicks it too.
    if (cur >= 0 ? !g_need_resched && now < g_slice_end_ns && lockless_ctx_ok(cur)
                 : !g_need_resched && !scheduler_any_ready()) {
        ack(regs);
        // BILLED HERE TOO, so an idle core's time reaches the totals a
        // slice at a time rather than as one lump of seconds when it next
        // switches (scheduler_cpu_time() bills only the caller's core).
        bill_current();
        this_cpu_add(sched_events, 1);
        this_cpu_add(sched_lockless, 1);
        return 1;
    }
    uint64_t f = sched_lock();
    if (cur >= 0 && !lockless_ctx_ok(cur)) { sched_unlock(f); return 0; }
    if (!timer_wants_rotation(now)) {
        ack(regs);
        bill_current();
        this_cpu_add(sched_events, 1);
        this_cpu_add(sched_lockless, 1);
        sched_unlock(f);
        return 1;
    }
    int done = lockless_rotate(regs, 0, ack);
    irq_restore(f);
    return done;
}

int scheduler_yield_lockless(uint64_t *regs) {
    if (!lockless_here(regs) || current_index < 0) return 0;
    uint64_t f = sched_lock();
    regs[TF_RAX] = 0;   // sys_yield()'s answer, written before a switch can resume elsewhere
    int done = lockless_rotate(regs, 1, 0);
    irq_restore(f);
    return done;
}

// block_common() for an MPSAFE handler: park at the syscall's entry with
// no kernel lock, answered by the wake's value as a locked park is.
// **IT DECLINES, HAVING WRITTEN NOTHING, WHERE IT CANNOT SWITCH LOCK-FREE**
// -- a signal or re-issue due, an RT process, or a next context that holds
// the kernel lock (lockless_rotate()'s rule) -- so the caller's handler
// declines too and the locked path parks it. The caller armed `chan`
// before testing its condition; a decline leaves the arm for that path.
int scheduler_block_current_lockless(uint64_t *regs, const void *chan, int reason) {
    if (!lockless_here(regs) || current_index < 0) return 0;
    int cur = current_index;
    struct sched_process *p = &procs[cur];
    int taken = 0;   // the kernel lock, for the context switched into
    uint64_t f;
    int next;
    uint64_t pv;
    // **BILLED BEFORE THE PICK**, as lockless_rotate() does, and the clock
    // read before the lock (a PM-timer read is an exit).
    uint64_t now = clocksource_now_ns();
    bill_current();
    uint64_t q;
    for (;;) {
        f = sched_lock();
        if (sq_park_begin(cur, chan, &q)) {
            regs[TF_RAX] = (uint64_t)p->arm_value;
            sched_unlock(f);
            if (taken) bkl_drop_for_switch();
            return 1;
        }
        if (!lockless_ctx_ok(cur)) {
            sq_park_abandon(chan, q);   // a decline writes nothing: the arm stays
            sched_unlock(f);
            if (taken) bkl_drop_for_switch();
            return 0;
        }
        rt_period_roll(now);   // the RT budget the pick reads, as find_next_runnable() rolls it
        pv = 0;
        next = pick_next(rotation_pos, &pv);   // cur is RUNNING, so never itself
        int needs = next == ROT_KERNEL ? this_cpu()->idle_bkl_depth != 0
                                       : procs[next].bkl_depth != 0 || procs[next].preempted_in_kernel;
        if (!needs || taken) break;
        // **THE KERNEL LOCK FIRST, THEN THE PICK AGAIN**: kernel lock
        // before scheduler lock, and nothing picked or marked while it is
        // waited for (bkl_take_for_switch()).
        sq_park_abandon(chan, q);
        sched_unlock(f);
        bkl_take_for_switch(regs);
        taken = 1;
    }
    if (pv > g_min_vruntime) g_min_vruntime = pv;
    sq_park_commit(cur, chan);
    p->rt_run_ns = 0;
    p->kernel_rsp = (uint64_t)regs;
    kstack_verify(cur);
    fpu_save(p->fpu);
    p->preempted_in_kernel = 0;
    sched_set_state((int)(p - procs), SCHED_BLOCKED);
    trace_sched("block_lockless", cur);
    p->wait_chan = chan;
    p->wake_at_ns = 0;
    p->wait_reason = reason;
    p->parked_at_ns = now;
    p->hung_reported = 0;
    sq_unlock(chan, q);
    set_current_index(-1);
    this_cpu_add(sched_lockless, 1);
    if (next == ROT_KERNEL) switch_to_kernel(cur);
    else switch_to(cur, next);
    irq_restore(f);   // resumed, perhaps on another core
    return 1;
}

uint64_t scheduler_next_event_ns(void) {
    uint64_t next = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state != SCHED_BLOCKED || !procs[i].wake_at_ns) continue;
        if (!next || procs[i].wake_at_ns < next) next = procs[i].wake_at_ns;
    }
    // The slice's end matters only with somebody to hand over to. A
    // process always has one -- the kernel context is a participant.
    if (current_index >= 0 || scheduler_any_ready())
        if (!next || g_slice_end_ns < next) next = g_slice_end_ns;
    return next;
}

// SYS_YIELD's entry into the same rotation. Distinct from the tick only
// so the call sites read honestly; the accounting difference that used
// to justify two paths is gone.
// A YIELD GOES TO THE BACK of its level: its vruntime is moved up to the
// most anyone there has run, so every peer goes first -- the old
// "SCHED_COMPAT_YIELD" behaviour of CFS. Without it the least-run rule
// hands a yielder straight back the CPU it just gave up, since a
// process that only yields has run almost nothing (cputime_test).
// An RT yield goes to the back of its priority, sched_yield()'s POSIX
// meaning -- and does not reset the watchdog, which counts until a BLOCK.
void scheduler_yield(uint64_t *regs) {
    if (current_index >= 0 && is_rt(current_index)) {
        procs[current_index].rt_seq = ++g_rt_seq;
    } else if (current_index >= 0) {
        uint64_t top = fair_top(current_index);
        if (procs[current_index].vruntime > top) top = procs[current_index].vruntime;
        if (kernel_slot_runnable() && kernel_vruntime > top) top = kernel_vruntime;
        procs[current_index].vruntime = top;
    }
    scheduler_rotate(regs);
}

void scheduler_trap_exit(uint64_t *regs) {
    if (g_need_resched) scheduler_rotate(regs);
}

void scheduler_preempt_disable(void) { this_cpu_add(preempt_depth, 1); }

int scheduler_preempt_depth(void) { return this_cpu_read(preempt_depth); }

void scheduler_fd_hold(int delta) {
    int i = current_index;   // this thread's slot, whichever core it is on
    if (i >= 0) procs[i].fd_holds = (uint8_t)(procs[i].fd_holds + delta);
}

void scheduler_preempt_enable(void) {
    // Read then add: at a depth above 0 nothing can move this context.
    if (this_cpu_read(preempt_depth) > 0) this_cpu_add(preempt_depth, -1);
    // Clamped at 0 rather than allowed to go negative: an extra enable()
    // is a bug, but letting the count drift below zero would make the
    // NEXT legitimate disable() a no-op, turning a local mistake into a
    // silent loss of protection somewhere else entirely.
}

static void scheduler_rotate(uint64_t *regs) { rotate(regs, 0); }

static void rotate(uint64_t *regs, int to_idle) {
    if (!scheduler_armed) return;
    // A syscall reaches here too (scheduler_trap_exit(), SYS_YIELD), and
    // under a trap gate it arrives with IF SET: a tick nested between
    // "state = READY" and switch_to()'s save resumes a torn context.
    sched_switch_begin();


    // UNCONDITIONALLY, and before anything can return early or switch:
    // this closes the slice that just ended, whoever owned it. Doing it
    // inside the `current_index >= 0` branch below was wrong in the one
    // case that matters -- when the KERNEL context was running there is
    // nobody to charge, but the start timestamp still has to move, and
    // leaving it stale handed the next process everything the kernel had
    // just spent. Measured: a process billed 9.51 SECONDS across a 300ms
    // window.
    bill_current();


    // A LEGACY BLOCKING PROCESS CANNOT BE PARKED, so while one is in
    // flight this file does not switch at all -- it resumes exactly
    // what was interrupted, which is what it did before the kernel
    // joined the rotation.
    //
    // process_run_ring3() runs a ring-3 process with NO procs[] entry:
    // from here it simply *is* "the kernel context", and its trapframe
    // lands in kernel_saved_rsp. There is nowhere to record its CR3 or
    // its RSP0, so switching away and back resumes it under whatever
    // address space the other process left loaded.
    //
    // kernel_slot_runnable() already refused to SELECT the kernel
    // position for that reason -- but find_next_runnable()'s fallback
    // returns ROT_KERNEL anyway when nothing else is runnable, which
    // reintroduced the exact case the guard existed to prevent. The
    // real fix is here: don't start the rotation at all.
    //
    // Found the hard way. Running `ls` from the debug console (the
    // legacy path) while a ring-3 GUI client was alive scheduled the
    // client, then resumed `ls` under the CLIENT's page tables --
    // reported as a bare "RING-3 CRASH: Page fault" in the client,
    // which is about as far from the actual cause as a symptom gets.
    // The window is narrow but entirely reachable: any `run` from the
    // physical shell while a client has a window open.
    if (process_context_is_armed() && !on_ap()) {
        if (current_index < 0) save_kernel_frame(regs);
        slice_restart();   // asked again a slice from now, not in a storm
        return;
    }

    // A CRITICAL SECTION IS OPEN -- resume exactly what was interrupted,
    // the same treatment (and for the same reason) as the armed legacy
    // process above: switching away would let another caller re-enter
    // code that is holding shared state. See scheduler_preempt_disable()
    // in api/scheduler.h for what holds this and why. The slice is
    // already billed above, so accounting is unaffected.
    if (this_cpu_read(preempt_depth) > 0) {
        if (current_index < 0 && !on_ap()) save_kernel_frame(regs);
        slice_restart();
        return;
    }

    // Let go by the switch at the end, whichever: the caller has
    // interrupts off, so there is no flag to restore after.
    (void)sched_lock();

    // Consumed only by a rotation that can happen: one refused above
    // leaves it for the next trap rather than losing the wake.
    set_need_resched(0);

    if (current_index >= 0) {
        // This process was the one running for the tick that just
        // fired. Counted here rather than at switch_to() time because
        // this is the only place that knows a whole tick elapsed under
        // it -- see abi/proc_info.h on why the total, not a percentage.
        procs[current_index].kernel_rsp = (uint64_t)regs;
        kstack_verify(current_index);  // it just stopped running -- check its stack
        // Paired with switch_to()'s FXRSTOR. Saved on the way out
        // whether or not the process has touched FP: "has it?" is
        // exactly the question the lazy scheme answered with CR0.TS,
        // and exactly the question that turned out to be dangerous to
        // answer (see fpu.h).
        fpu_save(procs[current_index].fpu);
        sched_set_state(current_index, SCHED_READY);
        procs[current_index].preempted_in_kernel = !(regs[SCHED_TF_CS] & 3);
        // SCHED_RR's one difference from FIFO: a spent slice goes to the
        // back of its priority. A preemption leaves it at the head.
        if (procs[current_index].policy == SCHED_RR && clocksource_now_ns() >= g_slice_end_ns)
            procs[current_index].rt_seq = ++g_rt_seq;
        trace_sched("rotate_out", current_index);
    } else if (!on_ap()) {
        save_kernel_frame(regs);
    }

    // Rotate from wherever the cycle last stopped, not from
    // current_index -- those differ precisely when the kernel is the
    // thing running (current_index -1, rotation_pos ROT_KERNEL), which
    // is exactly the case that has to advance past the kernel's own
    // position instead of restarting the scan at slot 0 every tick.
    int next = to_idle ? ROT_KERNEL : find_next_runnable(rotation_pos);
    if (next == ROT_KERNEL) {
        // Either nothing else is runnable, or the kernel's turn came up
        // in the rotation. In the overwhelmingly common case (nothing
        // has been spawned at all) kernel_saved_rsp was just set to
        // `regs` above, so this stays the genuine no-op it always was.
        int was = current_index;
        switch_to_kernel(was);
        if (was < 0) kernel_let_waiters_in();
        return;
    }

    switch_to(current_index, next);
}

// BLOCKING SYSCALLS, AND WHY THEY DESCHEDULE RATHER THAN WAIT
// -----------------------------------------------------------
// Parks the calling process on `reason` and hands the CPU to whatever
// is next in the rotation. `regs` is the syscall's own trapframe --
// the same pointer isr_dispatch was handed -- so the process resumes
// from the instruction after its `syscall` when woken, with
// scheduler_wake()'s value already in RAX.
//
// The obvious implementation of a blocking syscall -- `sti`, then spin
// or `hlt` inside the handler until the thing you're waiting for
// arrives -- was tried in this kernel and is genuinely unsafe here, not
// merely slow. A blocking keyboard read was built that way once: it
// worked for exactly one keystroke and then hung, because
// g_next_kernel_rsp (idt.c) is a single global "where to resume"
// pointer. It is correct for the scheduler's own use but was never
// meant to be reentrant, so a nested IRQ handler overwrites it while
// the outer int-0x80 handler is still on the stack, and that outer
// handler's epilogue then resumes into a stale frame.
//
// Descheduling sidesteps that entirely instead of trying to make the
// global reentrant. Nothing nests: the handler does not wait, it
// RETURNS, through the ordinary isr_common epilogue, into a different
// entity -- exactly the switch scheduler_tick() and scheduler_on_exit()
// already perform, using machinery that is already proven. Interrupts
// stay off for the whole handler, as they always were.
//
// Returns 1 if the caller was parked (its syscall must then return
// WITHOUT touching regs[TF_RAX] -- the wake writes it), or 0 if the
// caller isn't a scheduler-managed process and therefore has no slot to
// park in (the legacy process_run_ring3() path, or kernel code). A 0
// return is not an error the caller may ignore: it means "you must fall
// back to non-blocking behaviour", because there is nowhere to put this
// process to sleep.
// The one global wait channel -- a deadline. Its CONTENTS are never
// read; only its address matters, which is the whole point of a
// channel. `char` rather than `int` so it is guaranteed its own
// distinct address. (There were two until terminals became objects; see
// scheduler.h.)
const char sched_chan_timer;

// A process's own channel, woken by ITS children when they exit. Using
// the slot's address means the channel is stable for as long as the
// slot is, and unique across processes without a second table.
//
// Note this is the slot's address, not the pid: a reaped slot that is
// reused hands the new occupant the same channel, which is correct --
// the old occupant is gone and cannot be waiting on it.
const void *scheduler_wait_chan_pid(int pid) {
    int idx = pid_slot(pid);
    if (idx < 0) return 0;
    return &procs[idx];
}

_Static_assert(TF_RAX < SCHED_TF_SLOTS,
               "a test trapframe must be big enough to hold the RAX slot a wake writes");
_Static_assert(TF_RDI == SCHED_TF_RDI && TF_CS == SCHED_TF_CS &&
               TF_RFLAGS == SCHED_TF_RFLAGS && TF_RSP == SCHED_TF_RSP &&
               TF_SS == SCHED_TF_SS,
               "api/scheduler.h's SCHED_TF_* must match this file's TF_*");
_Static_assert(TF_VECTOR == SCHED_TF_VECTOR,
               "api/scheduler.h's SCHED_TF_VECTOR must match TF_VECTOR");
_Static_assert(TF_RIP == SCHED_TF_RIP,
               "api/scheduler.h's SCHED_TF_RIP must match TF_RIP");
_Static_assert(TRAPFRAME_WORDS == SIGFRAME_TF_SLOTS,
               "abi/signal_abi.h's SIGFRAME_TF_SLOTS must be a whole trapframe");
_Static_assert(TF_RAX == SCHED_TF_RAX,
               "scheduler.h's public RAX slot index must match the real trapframe layout");

static int block_common(uint64_t *regs, const void *chan, int reason,
                        uint64_t wake_at_ns) {
    if (current_index < 0) return 0;


    // **A WAKE THAT ARRIVED WHILE WE WERE CHECKING.** The caller armed
    // before testing its condition, and scheduler_wake() found that arm
    // rather than a blocked process -- so there IS something to do and
    // parking now would sleep through it. Answer as a wake would have,
    // and let the caller loop round and look again.
    //
    // **INTERRUPTS GO OFF BEFORE THIS TEST, NOT AFTER IT: "was I woken?"
    // and "I am no longer runnable" are one step.** A wake between them
    // finds a process that is not blocked yet and whose arm it can only
    // mark too late, so it is dropped -- waitpid() parked for good
    // beside a zombie. Linux sets the task state before the last test
    // of the condition for the same reason.
    // **AND UNDER THE SCHEDULER LOCK AND `chan`'s CHAIN LOCK**, which a
    // wake holds whenever it makes a slot READY: a
    // waker on another core with no kernel lock (a lock-free pipe call)
    // otherwise lands between this test and BLOCKED, seeing neither.
    // Billed first, interrupts off: its clock read may be a PM-timer exit,
    // not to be spent holding the scheduler lock.
    uint64_t irq = irq_save();
    bill_current(); // this slice ends here, or goes on -- see bill_current()
    uint64_t parked_now = clocksource_now_ns();   // not under the chain lock: wakers spin on it
    uint64_t lk = sched_lock();   // let go by the switch below, or by an early return
    // ...AND UNDER `chan`'s CHAIN LOCK until BLOCKED, which a wake's walk
    // holds (sq_park_begin()).
    uint64_t q;
    if (sq_park_begin(current_index, chan, &q)) {
        regs[TF_RAX] = (uint64_t)procs[current_index].arm_value;
        sched_unlock(lk);
        irq_restore(irq);
        return 1;
    }

    // **A SIGNAL THAT ARRIVED WHILE WE WERE CHECKING**, answered as
    // scheduler_signal_raise() answers a parked process. It wakes only
    // what is BLOCKED, so one raised between the caller's test and this
    // line -- reachable once interrupts are on inside a call -- was left
    // pending beside a sleeper (a sigsuspend() that slept through its
    // signal, a read deaf to Ctrl-C). With interrupts off in a syscall,
    // syscall entry delivers first and this never fires.
    struct sched_process *self = &procs[current_index];
    if (self->pending & ~self->blocked) {
        sched_signal_answer_park(self, regs, reason);
        sq_park_cancel(current_index, chan, q);
        sched_unlock(lk);
        irq_restore(irq);
        return 1;
    }

    int idx = current_index;
    procs[idx].rt_run_ns = 0;   // it blocked: the RT watchdog starts again
    procs[idx].kernel_rsp = (uint64_t)regs;
    kstack_verify(idx);
    fpu_save(procs[idx].fpu);
    sq_park_commit(idx, chan);
    sched_set_state(idx, SCHED_BLOCKED);
    trace_sched("block", idx);
    procs[idx].wait_chan = chan;
    // SET UNCONDITIONALLY, and 0 for an unbounded wait. It has to be
    // written on EVERY block rather than only where a deadline is
    // wanted: slots are recycled, so an inherited deadline in the past
    // makes wake_timers() release the process the instant it parks --
    // which, once that loop stopped filtering on SCHED_CHAN_TIMER,
    // turned every blocking wait on the machine into a spin. That is
    // what it did: the whole GUI suite failed at once.
    procs[idx].wake_at_ns = wake_at_ns;
    procs[idx].wait_reason = reason;
    procs[idx].parked_at_ns = parked_now;
    procs[idx].hung_reported = 0;
    sq_unlock(chan, q);   // BLOCKED and on the chain: a waker now waits for our switch
    set_current_index(-1);

    int next = find_next_runnable(rotation_pos);
    if (next == ROT_KERNEL) switch_to_kernel(idx);
    else switch_to(idx, next);
    irq_restore(irq);   // resumed: the caller's flag, as the early returns give it
    return 1;
}

// SYS_SLEEP's half of the block/wake pair: park until a DEADLINE rather
// than until an event.
//
// It is a separate entry point from scheduler_block_current() only
// because the deadline has to be recorded somewhere before the switch,
// and a wait reason cannot carry it: scheduler_wake() releases every
// process parked on a reason at once, which is right for "a pipe has
// data" and wrong for "it is 09:00" -- two sleepers almost never share
// an instant.
int scheduler_block_current(uint64_t *regs, const void *chan, int reason) {
    return block_common(regs, chan, reason, 0);
}

int scheduler_sleep_current(uint64_t *regs, uint64_t wake_at_ns) {
    return block_common(regs, SCHED_CHAN_TIMER, SCHED_WAIT_TIMER, wake_at_ns);
}

int scheduler_block_current_until(uint64_t *regs, const void *chan, int reason,
                                  uint64_t wake_at_ns) {
    return block_common(regs, chan, reason, wake_at_ns);
}

// The deadline-aware counterpart of scheduler_wake(), called once per
// timer tick. Returns how many sleepers it released.
//
// Same restraint as scheduler_wake() and for the same reason: this runs
// in the timer IRQ, so it only flips state and writes an already-saved
// trapframe. The woken process runs at the next ordinary rotation.
//
// In one-shot mode the timer is armed for the earliest deadline
// (scheduler_next_event_ns()), so a sleep ends on time; on the periodic
// path its resolution is one tick.
int scheduler_wake_timers(uint64_t now_ns) {
    int woken = 0;
    uint64_t sf = sched_lock();
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state != SCHED_BLOCKED) continue;
        // A DEADLINE, NOT A CHANNEL, is what this loop acts on. It used
        // to require SCHED_CHAN_TIMER, which made "sleep for a while"
        // the only timed wait the kernel had; a bounded wait on an
        // EVENT -- a datagram that may never arrive -- is the other
        // one, and it is what stops a blocking receive being a hang.
        // Linux's schedule_timeout() is the same primitive: a wait
        // queue and a deadline, either of which may fire first.
        if (!procs[i].wake_at_ns) continue;   // parked with no deadline
        if (procs[i].wake_at_ns > now_ns) continue;

        // SYS_SLEEP asked for exactly this and returns 0. Anything else
        // was waiting for an EVENT that did not come, so its handler
        // has to look again and decide -- it is the only code that
        // knows whether an empty queue at the deadline is a timeout or
        // a spurious wake.
        //
        // NOT for a context parked MID-CALL: its kernel_rsp is the
        // trapframe of a call still in progress, and its own C code
        // re-tests the condition on return -- the same rule
        // scheduler_wake_n() keeps.
        if (!procs[i].parked_in_kernel) {
            uint64_t *tf = (uint64_t *)(uintptr_t)procs[i].kernel_rsp;
            tf[TF_RAX] = procs[i].wait_chan == SCHED_CHAN_TIMER
                         ? 0 : (uint64_t)(int64_t)SYS_RETRY;
        }
        procs[i].wake_at_ns = 0;
        sched_set_state(i, SCHED_READY);
        sched_delay_woken(i);
        wake_preempt(i);   // places it, too
        woken++;
    }
    sched_unlock(sf);
    return woken;
}

// Wakes every process blocked on `reason`, giving each `value` as its
// blocking syscall's return value. Returns how many were woken (0 is
// perfectly normal -- an event with nobody waiting on it).
//
// Safe to call from an interrupt handler, which is the point: this only
// flips state and writes into an already-saved trapframe. It never
// touches g_next_kernel_rsp, so it cannot disturb whatever the
// interrupted context was going to resume into -- the woken process
// simply becomes eligible again and the next ordinary scheduler_tick()
// picks it up. That restraint is deliberate: an IRQ handler that tried
// to switch directly to the woken process is exactly the reentrancy
// this design exists to avoid.
int scheduler_wake(const void *chan, int64_t value) {
    return scheduler_wake_n(chan, value, 0);
}

// `max` waiters, or every one of them when it is 0. A bound exists
// because a futex has one: waking every waiter on a contended mutex so
// that all but one park again is the thundering herd this channel
// mechanism was built to avoid (see api/scheduler.h), and a lock's
// unlock wants exactly one.
// **PARK THE CURRENT CONTEXT MID-CALL, AND CARRY ON WHERE IT STOPPED.**
// The counterpart to scheduler_block_current(), and the reason a single
// suspend shape was worth building: that one parks at a syscall ENTRY
// and is answered by ring 3 asking again, so the C frames beneath it
// are thrown away. This one keeps them, so a caller six frames deep in
// a block walk can wait for a disk and resume on the next line with its
// locals intact.
//
// Returns 0 when there is nowhere to park -- the kernel context, a
// KTEST, the legacy loader, anything with no scheduler slot -- and the
// caller must fall back to polling rather than assume it slept.
//
// THE CALLER MUST ARM BEFORE IT TESTS ITS CONDITION, exactly as the
// entry-point version requires: a wake that lands between the test and
// the park is otherwise lost, and this one has no ring-3 retry loop
// underneath it to paper over that.
int scheduler_block_kernel(const void *chan, int reason) {
    return scheduler_block_kernel_until(chan, reason, 0);
}

int scheduler_block_kernel_until(const void *chan, int reason, uint64_t wake_at_ns) {
    if (current_index < 0) return 0;
    // **AND NOT WHILE THE PREEMPTION GUARD IS RAISED**, which is
    // Linux's "you cannot sleep holding a spinlock". The guard is what
    // makes the non-re-entrant filesystem safe (vfs.c's FS_OP), so a
    // context that slept inside one would let a second walker into
    // tfs3.c's module-level scratch buffers -- the exact corruption the
    // guard exists to prevent, reintroduced by the thing meant to
    // replace it. The caller falls back to polling, as it does for a
    // context with no slot at all.
    if (this_cpu_read(preempt_depth) > 0) return 0;
    int idx = current_index;

    // A wake that arrived while we were checking, tested with
    // interrupts already off -- see block_common() for both halves.
    uint64_t irq = irq_save();
    bill_current();   // before the lock, as block_common()'s
    uint64_t parked_now = clocksource_now_ns();   // ...and the clock, as block_common()'s
    uint64_t lk = sched_lock();   // the test and BLOCKED are one step, as block_common()'s
    uint64_t q;
    if (sq_park_begin(idx, chan, &q)) {
        sched_unlock(lk);
        irq_restore(irq);
        return 1;
    }
    procs[idx].rt_run_ns = 0;
    kstack_verify(idx);
    fpu_save(procs[idx].fpu);
    sq_park_commit(idx, chan);
    sched_set_state(idx, SCHED_BLOCKED);
    procs[idx].parked_in_kernel = 1;
    trace_sched("block_kernel", idx);
    procs[idx].wait_chan = chan;
    procs[idx].wake_at_ns = wake_at_ns;   // 0 = unbounded
    procs[idx].wait_reason = reason;
    procs[idx].parked_at_ns = parked_now;
    procs[idx].hung_reported = 0;
    sq_unlock(chan, q);
    set_current_index(-1);

    int next = find_next_runnable(rotation_pos);
    if (next == ROT_KERNEL) switch_to_kernel(idx);
    else switch_to(idx, next);

    // Resumed, on our own stack, with every frame below us intact -- and
    // with the caller's interrupt flag BACK, as Linux's schedule() leaves
    // it. Without this a call that parked once ran the rest of itself
    // with interrupts off: never preempted, and under smp=run never
    // ticking, so a long one held the kernel lock from every other core.
    procs[idx].parked_in_kernel = 0;
    irq_restore(irq);
    return 1;
}

// One BLOCKED slot, made READY and answered -- the half of a wake that
// acts on a parked process, shared by every way of choosing one.
static void wake_slot(int i, int64_t value) {
    // The whole wake under the scheduler lock: READY, its place and its
    // kick are one step to a core picking without the kernel lock.
    uint64_t f = sched_lock();
    // The saved trapframe's RAX slot IS the syscall's return value:
    // isr_common's epilogue pops it straight into the register the
    // ring-3 caller reads. Writing it here is what makes waking a
    // process and answering its syscall the same act.
    // ...but only for a process parked at a syscall ENTRY. One
    // parked mid-call resumes its own C frames and computes its
    // own answer, and its trapframe belongs to a call that has not
    // finished -- writing a return value into it would be
    // answering a question nobody asked.
    if (!procs[i].parked_in_kernel) {
        uint64_t *tf = (uint64_t *)(uintptr_t)procs[i].kernel_rsp;
        tf[TF_RAX] = (uint64_t)value;
    }
    // The wait is over, so its deadline is too. Leaving it set
    // would have the next timer tick "release" a process that is
    // already running -- overwriting the RAX of whatever syscall it
    // had reached by then.
    procs[i].wake_at_ns = 0;
    sched_set_state(i, SCHED_READY);
    sched_delay_woken(i);
    vr_place(i);
    if (is_rt(i)) procs[i].rt_seq = ++g_rt_seq;
    // "Does it outrank what runs HERE", and the flag that says so, are one
    // core's with interrupts off (the lock's): a waker moved between them
    // compared against one core and flagged another (sched_here()'s rule).
    if (rank_of(i) > running_rank()) set_need_resched(1);
    // MID-CALL, BOTH FAIR: it is usually holding the filesystem lock, so
    // it cuts in -- but only while it has run less than what it would
    // preempt. That bound is what the unconditional version lacked.
    else if (procs[i].parked_in_kernel && !is_rt(i) && running_rank() == 0 &&
             vr_behind_running(i))
        set_need_resched(1);
    kick_idle_core(i);
    sched_unlock(f);
}

// Arms on `chan` not yet stamped, stamped -- up to `left` (0: all).
// Under the chain lock: an armed slot is RUNNING, so only its chain
// orders it against the park that consumes the stamp.
static int stamp_arms(const void *chan, int64_t value, int left) {
    int n = 0;
    SQ_FOR_EACH(i, nx, chan) {
        if (left && n >= left) break;
        // A slot zombified while it armed (the arm takes no scheduler
        // lock) can sit on the chain until it is freed: never a wake.
        if (procs[i].state == SCHED_UNUSED || procs[i].state == SCHED_ZOMBIE) continue;
        if (procs[i].sq_chan != chan || procs[i].arm_chan != chan || procs[i].armed_woken) continue;
        procs[i].armed_woken = 1;
        procs[i].arm_value   = value;
        n++;
    }
    return n;
}

static int parked_on(int i, const void *chan) {
    return procs[i].sq_chan == chan && procs[i].state == SCHED_BLOCKED && procs[i].wait_chan == chan;
}

static int any_parked_on(const void *chan) {
    SQ_FOR_EACH(i, nx, chan)
        if (parked_on(i, chan)) return 1;
    return 0;
}

int scheduler_wake_n(const void *chan, int64_t value, int max) {
    // **A NULL CHANNEL WAKES NOBODY.** scheduler_wait_chan_pid() answers
    // NULL for a pid outside the table, and 0 is one -- it is what
    // notify_parent() is handed for a process whose parent is "the
    // kernel", which is every program the debug console starts. Without
    // this every unarmed process would match an arm_chan of 0.
    if (!chan) return 0;
    // **FIRST, ANYONE WHO HAS ARMED BUT NOT YET PARKED** -- RUNNING, so a
    // wake of the parked alone would drop it (the lost wakeup that made a
    // preemptible syscall unsafe); block_common() then declines to park.
    // Counted as woken, so a `max` of 1 is honoured. Under the chain lock
    // ALONE: a wake that finds nobody parked -- most of them -- never
    // takes the scheduler lock.
    uint64_t q = sq_lock(chan);
    if (!any_parked_on(chan)) {
        int stamped = stamp_arms(chan, value, max);
        sq_unlock(chan, q);
        return stamped;
    }
    sq_unlock(chan, q);
    // Somebody to make READY: the scheduler lock, then the chain again
    // (sched_internal.h's order), and the WHOLE wake from the start. Arms
    // are not stamped before this: one stamped then could consume its
    // stamp and re-arm in the gap, be stamped twice and counted twice,
    // and leave a sleeper of a FUTEX_WAKE of 2 parked.
    uint64_t sf = sched_lock();
    q = sq_lock(chan);
    int woken = stamp_arms(chan, value, max);
    SQ_FOR_EACH(i, nx, chan) {
        if (max && woken >= max) break;
        if (!parked_on(i, chan)) continue;
        sq_unlink(i);   // before wake_slot(): its READY must not want this lock
        wake_slot(i, value);
        woken++;
    }
    sq_unlock(chan, q);
    sched_unlock(sf);
    return woken;
}

int scheduler_wake_one(const void *chan) {
    if (!chan) return 0;
    uint64_t q = sq_lock(chan);
    int any = any_parked_on(chan);
    sq_unlock(chan, q);
    if (!any) return 0;
    int pick = -1;
    uint64_t sf = sched_lock();   // the pick and its wake are one step (scheduler_wake_n())
    q = sq_lock(chan);
    SQ_FOR_EACH(i, nx, chan) {
        if (!parked_on(i, chan)) continue;
        if (pick < 0 || rank_of(i) > rank_of(pick)) pick = i;
    }
    int pid = 0;
    if (pick >= 0) {
        sq_unlink(pick);
        wake_slot(pick, 0);
        pid = procs[pick].pid;
    }
    sq_unlock(chan, q);
    sched_unlock(sf);
    return pid;
}

// The thread pointer this thread's %fs resolves against. Ring 3 owns
// the layout behind it entirely (userland/libs/rt/tls.c); the kernel only
// remembers the number and reloads it on every switch.
int scheduler_set_tls(uint64_t base) {
    if (!uaddr_is_user(base)) return -EINVAL;   // the wrmsr would #GP in ring 0
    // A ring-3 process the LEGACY loader is running has no slot, and it
    // still needs a thread pointer -- every program does, since errno
    // is a `__thread` variable now. It goes in the kernel context's own
    // slot, which switch_to_kernel() reloads.
    if (current_index < 0) kernel_fs_base = base;
    else                   procs[current_index].fs_base = base;
    arch_set_fs_base(base); // whoever asked is running -- take effect now
    return 0;
}

int scheduler_set_clear_tid(uint64_t uaddr) {
    if (current_index < 0) return -EPERM;
    if ((uaddr & 3) || (uaddr && !uaddr_is_user(uaddr))) return -EINVAL;
    procs[current_index].clear_tid = uaddr;
    return procs[current_index].pid;
}

uint64_t scheduler_clear_tid(void) {
    return current_index < 0 ? 0 : procs[current_index].clear_tid;
}

int scheduler_current_pid(void) {
    return current_index < 0 ? 0 : procs[current_index].pid;
}

// The PROCESS on the CPU, where scheduler_current_pid() is the THREAD.
// Equal for everything that is not a thread, which is why every caller
// that predates threads kept working -- and why the ones that mean "the
// process" (a window's owner, a terminal's owner, a child's parent,
// getpid) had to be moved over one at a time rather than in bulk.
int scheduler_current_tgid(void) {
    if (current_index < 0) return 0;
    return procs[current_index].tgid;
}

// I/O charged with no process on the CPU: boot, the idle loop's work,
// a writeback from kernel context.
static struct sched_io g_kernel_io;

void scheduler_io_charge(enum sched_io_kind kind, uint64_t bytes) {
    if ((unsigned)kind >= SCHED_IO_KINDS) return;
    uint64_t f = irq_save();
    struct sched_io *io = &g_kernel_io;
    if (current_index >= 0) {
        int lead = pid_slot(procs[current_index].tgid);
        io = &procs[lead >= 0 ? lead : current_index].ext->io;
    }
    // ATOMIC: a lock-free read charges with no kernel lock, so two threads
    // of one process may charge their leader at once from two cores.
    __atomic_fetch_add(&io->bytes[kind], bytes, __ATOMIC_RELAXED);
    __atomic_fetch_add(&io->ops[kind], 1, __ATOMIC_RELAXED);
    irq_restore(f);
}

int scheduler_io_get(int slot, struct sched_io *out) {
    if (!out) return 0;
    if (slot == -1) { *out = g_kernel_io; return 1; }
    if (slot < 0 || slot >= g_slots_ready || procs[slot].state == SCHED_UNUSED) return 0;
    uint64_t f = irq_save();
    *out = procs[slot].ext->io;
    irq_restore(f);
    return 1;
}

int scheduler_tgid(int pid) {
    int s = pid_slot(pid);
    if (s < 0 || procs[s].state == SCHED_UNUSED) return 0;
    return procs[s].tgid;
}

int scheduler_exec_path(int pid, char *out, unsigned cap) {
    if (!out || !cap) return 0;
    out[0] = '\0';
    int s = pid_slot(pid);
    if (s < 0) return 0;
    struct sched_process *p = &procs[s];
    if (p->state == SCHED_UNUSED) return 0;
    // REFUSE rather than truncate: callers match on this string to
    // decide which program a window belongs to, and a shortened path
    // matches the wrong one.
    if (k_strlcpy(out, p->ext->exec_path, cap) >= cap) { out[0] = '\0'; return 0; }
    return out[0] ? 1 : 0;
}

int scheduler_dumpable(void) {
    return current_index < 0 || !procs[leader_index(current_index)].ext->no_crash_report;
}

void scheduler_set_dumpable(int on) {
    if (current_index >= 0) procs[leader_index(current_index)].ext->no_crash_report = !on;
}

int scheduler_current_killed(void) {
    return current_index >= 0 && (procs[current_index].pending & (1u << SIGKILL)) != 0;
}

struct sched_mm *scheduler_current_mm(void) {
    // NULL means "the kernel context is running", which for SYS_SBRK is
    // the legacy elf_run.c process -- not "this process has no heap".
    // Every slot gets one at creation.
    if (current_index < 0) return 0;
    // THE LEADER'S, not this slot's: threads share one heap because they
    // share one address space, and a per-thread copy of `brk` would let
    // two sbrk()s hand out the same page.
    return &procs[leader_index(current_index)].mm;
}

// The KERNEL CONTEXT's own directory -- the legacy elf_run.c loader and
// anything the kernel shell starts. Same two-owners-one-representation
// shape SYS_SBRK's heap has (proc_syscalls.c's g_legacy_heap): the
// legacy path has no scheduler slot to keep this in, so it gets one of
// the same TYPE, reached through the same accessors, and the two cannot
// drift in behaviour.
static struct sched_cwd kernel_cwd = { "/" };

const char *scheduler_cwd(void) {
    struct sched_cwd *c = scheduler_current_cwd();
    return c ? c->path : kernel_cwd.path;
}

void scheduler_set_kernel_cwd(const char *path) {
    if (!path || path[0] != '/') return; // callers pass an already-resolved path
    k_strlcpy(kernel_cwd.path, path, sizeof kernel_cwd.path);
}

struct sandbox **scheduler_current_sandbox(void) {
    if (current_index < 0) return 0;
    return &procs[leader_index(current_index)].ext->sandbox;
}

struct sched_cwd *scheduler_current_cwd(void) {
    // Same NULL convention as scheduler_current_mm(): "the kernel
    // context is running", i.e. the legacy loader's slot applies.
    if (current_index < 0) return 0;
    // The leader's, for scheduler_current_mm()'s reason: `cd` in one
    // thread moves the whole process, which is what chdir() means.
    return &procs[leader_index(current_index)].ext->cwd;
}

// The memory map behind a given address space. Walks the table because the
// caller (a page fault, or a copy helper) has a pml4 and not a pid --
// and a ZOMBIE is skipped deliberately: its address space is already
// destroyed, so a fault naming it is a stale mapping, not a heap page.
struct sched_mm *scheduler_mm_for_pml4(uint64_t pml4_phys) {
    if (!pml4_phys) return 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state == SCHED_UNUSED) continue;
        // A DYING GROUP's leader waits as a zombie while its space stays
        // for the threads still leaving (sched_exit.c) -- one may be in
        // ring 3 on another core and fault before its SIGKILL lands.
        if (procs[i].state == SCHED_ZOMBIE && !procs[i].group_dying) continue;
        // Threads share their leader's address space, so every one of
        // them would match -- and the first match must be the one slot
        // that owns the heap, or a growth fault in a thread would move
        // a `brk` nothing else reads.
        if (is_thread(i)) continue;
        if (procs[i].pml4_phys == pml4_phys) return &procs[i].mm;
    }
    return 0;
}

struct sched_mm *scheduler_mm_for_pid(int pid) {
    int slot = pid_slot(pid);
    if (slot < 0) return 0;
    if (procs[slot].state == SCHED_UNUSED || procs[slot].state == SCHED_ZOMBIE)
        return 0;
    if (is_thread(slot)) return 0;
    return &procs[slot].mm;
}

// The address space of a slot that is genuinely running, or 0. Kernel
// side only, and deliberately NOT part of struct proc_info: a CR3 is
// not something userland has any business seeing.
//
// A ZOMBIE answers 0 because its address space is already gone -- see
// scheduler_kill() -- and a caller auditing page tables must not walk
// freed ones.
uint64_t scheduler_slot_pml4(int slot) {
    if (slot < 0 || slot >= MAX_PROCS) return 0;
    if (procs[slot].state == SCHED_UNUSED || procs[slot].state == SCHED_ZOMBIE) return 0;
    return procs[slot].pml4_phys;
}

// **IS `pid` A CHILD OF `parent_pid`?** waitpid() needs this and did
// not have it: scheduler_poll() answers about ANY pid, so a caller that
// named something that was never its child was told RUNNING and parked
// on its OWN channel -- while that process's death woke its real
// parent's channel instead, and the waiter slept for the rest of the
// boot. POSIX answers ECHILD there, and pids RECYCLE here, so "the pid
// I spawned" and "the process in that slot now" are not the same
// question on a machine that has started fifty processes.
//
// A THREAD IS NOT A CHILD, the same rule scheduler_poll_any() keeps.
int scheduler_is_child_of(int pid, int parent_pid) {
    // **A CALLER WITH NO SLOT IS NOBODY'S PARENT.** The legacy loader
    // runs with current_index < 0, so scheduler_current_tgid() is 0 --
    // and init's ppid is 0 too, which made "is init a child of nobody?"
    // answer YES and let a wait through to a park that then refused.
    // Caught by the errno test asking for ECHILD and getting EPERM.
    if (parent_pid < 1) return 0;
    int slot = pid_slot(pid);
    if (slot < 0 || procs[slot].state == SCHED_UNUSED) return 0;
    if (is_thread(slot)) return 0;
    return procs[slot].ppid == parent_pid;
}

int scheduler_pid_valid(int pid) {
    int s = pid_slot(pid);
    return s >= 0 && procs[s].state != SCHED_UNUSED;
}

// See the fields' comment. Announces that the caller is ABOUT to wait on
// `chan`, so a wake arriving before it parks is not lost. Every blocking
// syscall calls this BEFORE it tests its condition.
// UNDER ITS CHANNEL'S CHAIN LOCK, as a wake's walk is: a waker stamping
// the OLD channel's wake between these writes would leave the new arm
// "woken" with that wake's value. Interrupts off first, so the slot read
// is this core's. An arm on another channel is over: off that chain
// first, never two chain locks at once.
void scheduler_wait_arm(const void *chan) {
    uint64_t f = irq_save();
    int cur = current_index;
    if (cur >= 0) {
        struct sched_process *p = &procs[cur];
        if (p->sq_on && p->sq_chan != chan) sq_unlink_any(cur);
        uint64_t q = sq_lock(chan);
        if (!p->sq_on) sq_link(cur, chan);
        p->arm_chan   = chan;
        p->armed_woken = 0;
        p->arm_value  = 0;
        sq_unlock(chan, q);
    }
    irq_restore(f);
}

// Withdraws the announcement -- for a caller that decided not to wait
// after all, so a later unrelated wake does not make its NEXT park a
// no-op.
void scheduler_wait_disarm(void) {
    // Every syscall's exit: nothing armed is nothing to take a lock for,
    // and no waker writes a slot whose arm is 0 (a NULL channel wakes nobody).
    int me = current_index;
    if (me < 0 || (!procs[me].arm_chan && !procs[me].armed_woken)) return;
    uint64_t f = irq_save();
    int cur = current_index;
    if (cur >= 0) {
        if (procs[cur].sq_on) sq_unlink_any(cur);   // clears the arm under the chain's lock
    }
    irq_restore(f);
}

void scheduler_demo_run(void) {
    int a = spawn_from_fs("/bin/counter_a", NULL, 0, -1, -1, -1, 0, 0, 0);
    int b = spawn_from_fs("/bin/counter_b", NULL, 0, -1, -1, -1, 0, 0, 0);
    if (a < 0 || b < 0) {
        vga_write("schedtest: failed to spawn one or both counter processes --\n");
        vga_write("were /bin/counter_a and /bin/counter_b seeded onto disk.img?\n");
        vga_write("(see the Makefile's `seed` target)\n");
        if (a >= 0) { slot_free(a); alive_count--; }   // spawn_from_fs() returns a SLOT
        if (b >= 0) { slot_free(b); alive_count--; }
        return;
    }

    vga_write("Spawned two ring-3 counter processes (A and B). The scheduler\n");
    vga_write("is continuously armed (Milestone 1 phase 4b) -- the timer\n");
    vga_write("(100Hz) will now preemptively switch between them without\n");
    vga_write("either ever yielding voluntarily. Output below is interleaved\n");
    vga_write("DIRECTLY by each process's own write syscall, not narrated by\n");
    vga_write("the kernel:\n\n");
    klog_write("scheduler: demo spawned, waiting for both processes to exit\n");

    // Just a wait loop now, not an arm/disarm pair -- see this file's
    // top comment on why permanently-armed is safe. Both processes are
    // reaped here via scheduler_poll() rather than reaching into procs[]
    // directly, same public API a real caller (Terminal) uses -- this
    // demo is otherwise the one place still allowed to busy-wait
    // (blocking the physical shell for the demo's duration is the
    // whole point, see scheduler.h's doc comment).
    int a_code = 0, b_code = 0;
    int a_done = 0, b_done = 0;
    while (!a_done || !b_done) {
        __asm__ volatile ("hlt");
        if (!a_done && scheduler_poll(procs[a].pid, &a_code) == SCHED_POLL_EXITED) a_done = 1;
        if (!b_done && scheduler_poll(procs[b].pid, &b_code) == SCHED_POLL_EXITED) b_done = 1;
    }

    vga_write("\n\nBoth processes exited. Scheduler stays armed -- every other\n");
    vga_write("command behaves exactly as it did before M16 (see this file's\n");
    vga_write("top comment on why an empty process table makes that safe).\n");
    klog_write("scheduler: demo complete\n");
}

// See scheduler.h: the kernel's idle work, in one place instead of in
// whichever loop happened to be running. Adding a second thing here
// means it becomes live in every waiting loop at once -- which is the
// point, and also the risk, so the bar is the same one the rest of this
// repo holds: it must be safe wherever the kernel is idle.
void scheduler_idle(void) {
    debug_console_poll();
    // Input devices that have no interrupt of their own. Empty unless
    // something registered a poll() -- the PS/2 pair does not, being
    // IRQ-driven, so this costs a loop over two NULLs on a machine with
    // no other input hardware. See kernel/include/kernel/input.h.
    input_poll_sources();
    // Raw input to a ring-3 compositor. Silent unless one holds the role
    // -- see win_input.c. It lives
    // here because this is the kernel's one owner of idle work, so
    // every waiting loop feeds the desktop without knowing it does.
    win_input_poll();
    // Write back a quiet disk cache (ata_cache.h). This is the "and an
    // idle timer" half of the flush policy -- the dirty-line threshold
    // bounds how much can accumulate, this bounds how LONG it can sit
    // there, so a machine nobody is touching ends up with its writes on
    // the platter rather than waiting for the next barrier. Through
    // ata.c, which skips the round if the drive is busy.
    ata_idle();
    // ...and the same half of the policy for a FILESYSTEM that is
    // holding a journal transaction open (storage.sync = batched). The
    // slot ceiling bounds how much accumulates; this bounds how long,
    // so a machine that wrote a file and was then left alone does not
    // hold that inode update indefinitely.
    fs_idle();
    // Received frames, and the protocols above them. The NIC's own
    // interrupt only queues a frame (kernel/drivers/net/net.c); this is
    // where ARP gets answered and an echo request becomes a reply, so a
    // machine with nothing to do still behaves like a host on the
    // network. Costs one compare when no card is registered.
    net_poll();
    // Free memory back to a hypervisor (page_report.h). One compare
    // without a reporter, which is every machine but a VM with a balloon.
    page_report_idle();
    // The console's last output burst onto the screen: a program that
    // printed and then slept leaves its tail in the back buffer otherwise.
    // Only a PRESENT, which refuses while the desktop owns the screen.
    vga_idle();
}

// Interrupts off around each bill: a tick that rotates bills too, and
// two interleaved bill_current()s would charge one slice twice.
void scheduler_idle_halt(void) {
    uint64_t f;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    bill_current();          // what the kernel ran up to here IS charged...
    g_kernel_halting = 1;    // ...the halt is not (see kernel_vruntime)
    lockup_idle();           // an idle CPU is not a locked one
    __asm__ volatile ("pushq %0; popfq" :: "r"(f) : "memory", "cc");
    clockevent_idle_halt();
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    bill_current();
    g_kernel_halting = 0;
    __asm__ volatile ("pushq %0; popfq" :: "r"(f) : "memory", "cc");
}

// --- the rewound-syscall window (see struct sched_proc.syscall_reissue) --

int scheduler_syscall_reissue_pending(int pid) {
    int s = pid_slot(pid);
    return s < 0 ? 0 : procs[s].syscall_reissue;
}

int scheduler_lockless_ok(void) {
    int cur = current_index;
    if (cur < 0 || g_need_resched) return 0;
    const struct sched_process *p = &procs[cur];
    return !p->syscall_reissue && !(p->pending & ~p->blocked);
}

void scheduler_syscall_entered(int pid) {
    int s = pid_slot(pid);
    if (s >= 0) procs[s].syscall_reissue = 0;
}
