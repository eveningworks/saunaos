// The application processors: found in the MADT, started with
// INIT-SIPI-SIPI one at a time, set up like the BSP and PARKED
// (docs/smp-design.md, stage 3). Nothing is scheduled on them yet -- a
// parked core sits in `cli; hlt` and runs no kernel code, which is why
// the kernel debugger needs no roundup until stage 5. It wakes only for
// smp_call(), which is how the BSP hands it work (stage 4's tests, the
// memory types), and for a kernel-map shootdown (tlb.h's own vector).
//
// The sequence and its delays are Linux's (arch/x86/kernel/smpboot.c,
// do_boot_cpu() and wakeup_secondary_cpu_via_init()); one core at a time
// as Linux did before 6.5's parallel bring-up, because the trampoline's
// data page then holds one core's stack and nothing has to pick its own.
#include "smp.h"
#include "percpu.h"
#include "gdt.h"
#include "idt.h"
#include "lapic.h"
#include "acpi.h"
#include "paging.h"
#include "pmm.h"
#include "heap.h"
#include "msr.h"
#include "multiboot.h"
#include "cpuinfo.h"
#include "clocksource.h"
#include "barrier.h"
#include "spinlock.h"
#include "bkl.h"
#include "irqflags.h"
#include "klog.h"
#include "kfmt.h"
#include "knum.h"
#include "string.h"
#include "tlb.h"          // tlb_service(): smp_poll_ipis()
#include "scheduler.h"    // scheduler_timeslice_ms(): an AP's tick
#include <stddef.h>

_Static_assert(offsetof(struct cpu, self) == CPU_OFF_SELF, "percpu.h: CPU_OFF_SELF");
_Static_assert(offsetof(struct cpu, user_rsp) == CPU_OFF_USER_RSP, "percpu.h: CPU_OFF_USER_RSP");
_Static_assert(offsetof(struct cpu, tss) + offsetof(struct tss, rsp0) == CPU_OFF_TSS_RSP0,
               "percpu.h: CPU_OFF_TSS_RSP0 (syscall_entry.asm's CPU_TSS_RSP0)");
_Static_assert(sizeof(struct cpu) <= 4096, "an AP's struct cpu is one page");

// smp_trampoline.asm's template and the labels it is entered by.
extern const uint8_t smp_tramp_start[], smp_tramp_32[], smp_tramp_64[], smp_tramp_end[];

// The trampoline's data page, one page after its code: the layout
// smp_trampoline.asm's D_* offsets read.
struct smp_tramp_data {
    uint64_t gdt[4];         // 0x00: null, code32 (0x08), data32 (0x10), code64 (0x18)
    uint16_t gdtr_limit;     // 0x20
    uint32_t gdtr_base;      // 0x22
    uint8_t  pad0[10];
    uint32_t jmp32_off;      // 0x30
    uint16_t jmp32_sel;
    uint16_t pad1;
    uint32_t jmp64_off;      // 0x38
    uint16_t jmp64_sel;
    uint16_t pad2;
    uint64_t cr3;            // 0x40
    uint64_t stack;          // 0x48
    uint64_t cpu;            // 0x50
    uint64_t entry;          // 0x58
} __attribute__((packed));
_Static_assert(offsetof(struct smp_tramp_data, gdtr_limit) == 0x20 &&
               offsetof(struct smp_tramp_data, jmp32_off) == 0x30 &&
               offsetof(struct smp_tramp_data, jmp64_off) == 0x38 &&
               offsetof(struct smp_tramp_data, cr3) == 0x40 &&
               offsetof(struct smp_tramp_data, entry) == 0x58,
               "smp_trampoline.asm's D_* offsets");

// Flat 4 GiB segments; the ACCESSED bit already set, because the CPU
// otherwise writes it into the descriptor on the first load.
#define TRAMP_CODE32 0x00CF9B000000FFFFULL
#define TRAMP_DATA32 0x00CF93000000FFFFULL
#define TRAMP_CODE64 0x00AF9B000000FFFFULL

// An AP's own memory, one contiguous run: its struct cpu, then the
// stack it parks on (also its RSP0 -- a parked core never enters from
// ring 3), the #DF stack and the NMI stack, as gdt.c gives the BSP.
#define AP_STACK_BYTES 16384
#define AP_IST_BYTES   8192
#define AP_PAGES ((4096 + AP_STACK_BYTES + 2 * AP_IST_BYTES) / 4096)

// How long a core has to call in, and then to park. Linux gives a core
// ten seconds; one is a hundred times what QEMU's TCG takes and a
// thousand times real hardware, and a boot that waits ten seconds per
// dead core is its own bug.
#define CALLIN_MS 1000

static struct cpu **g_cpus;  // [0] is the BSP (gdt_boot_cpu())
static uint64_t g_tramp_phys;  // 0 until a page is found
static int g_cpu_count = 1;

// The BSP's control registers, replayed on each AP.
static uint64_t g_bsp_cr0, g_bsp_cr4, g_bsp_efer;

int smp_cpu_count(void) { return g_cpus ? g_cpu_count : 1; }

const struct cpu *smp_cpu_at(int index) {
    if (index == 0) return gdt_boot_cpu();
    if (!g_cpus || index < 0 || index >= g_cpu_count) return 0;
    return g_cpus[index];
}

static int online(const struct cpu *c) { return c && c->state >= CPU_PARKED; }

int smp_online_count(void) {
    int n = 0;
    for (int i = 0; i < smp_cpu_count(); i++) n += online(smp_cpu_at(i));
    return n;
}

int smp_apic_online(uint32_t apic_id) {
    for (int i = 0; i < smp_cpu_count(); i++) {
        const struct cpu *c = smp_cpu_at(i);
        if (c && c->apic_id == apic_id) return online(c);
    }
    return 0;
}

int smp_apic_scheduling(uint32_t apic_id) {
    for (int i = 0; i < smp_cpu_count(); i++) {
        const struct cpu *c = smp_cpu_at(i);
        if (c && c->apic_id == apic_id) return c->index == 0 || c->state == CPU_RUNNING;
    }
    return 0;
}

uint64_t smp_trampoline_phys(void) { return g_tramp_phys; }

static void delay_us(uint64_t us) {
    uint64_t end = clocksource_now_ns() + us * 1000;
    while (clocksource_now_ns() < end) cpu_relax();
}

// ---- the AP's side ---------------------------------------------------

// smp=run: this core schedules. Its own periodic LAPIC tick and the
// reschedule IPI share SMP_SCHED_VECTOR, and isr_dispatch() now gives
// its interrupts the ordinary path -- the kernel lock, then
// scheduler_ap_event(). This loop is the core's idle context: a switch
// to a process saves it (inside that interrupt), and a switch back
// returns into it. It holds nothing while it halts.
static void __attribute__((noreturn)) ap_schedule(struct cpu *c) {
    lapic_timer_start_here(SMP_SCHED_VECTOR, scheduler_timeslice_ms());
    __atomic_store_n(&c->state, CPU_RUNNING, __ATOMIC_RELEASE);
    for (;;) __asm__ volatile ("sti; hlt; cli" ::: "memory");
}

static uint64_t read_cr0(void) { uint64_t v; __asm__ volatile ("mov %%cr0, %0" : "=r"(v)); return v; }
static uint64_t read_cr4(void) { uint64_t v; __asm__ volatile ("mov %%cr4, %0" : "=r"(v)); return v; }

// Entered from the trampoline with interrupts off, on the kernel's page
// tables and the trampoline's GDT, with GS.base unset -- so nothing here
// may call anything that reads this_cpu() before gdt_load_cpu().
//
// **NOTHING HERE MAY TOUCH SHARED KERNEL STATE: NO LOG LINE, NO HEAP, NO
// PREEMPTION GUARD.** The kernel has no lock that excludes another core
// until stage 4. Its first version logged through syscall_insn_init():
// the log interleaved, and the racing preemption count stayed raised,
// so every later kmutex warned and the boot locked up. Registers, this
// core's own struct cpu and its own LAPIC only; the BSP reports.
//
// **EVERY PER-CORE REGISTER THE BSP SET UP IS SET HERE AGAIN**, or this
// core differs in a way nothing notices until stage 5 runs code on it:
// CR0 (WP, the FPU's MP/NE), CR4 (SMEP, SMAP, OSFXSR, PGE...), EFER (NXE,
// SCE), the SYSCALL MSRs, PAT and the MTRRs. smp_test.c compares what
// this core read back.
static void __attribute__((noreturn)) ap_entry(struct cpu *c) {
    c->state = CPU_ALIVE;

    uint8_t *base = (uint8_t *)c;
    gdt_load_cpu(c, (uint64_t)(uintptr_t)(base + 4096 + AP_STACK_BYTES),
                 (uint64_t)(uintptr_t)(base + 4096 + AP_STACK_BYTES + AP_IST_BYTES),
                 (uint64_t)(uintptr_t)(base + 4096 + AP_STACK_BYTES + 2 * AP_IST_BYTES));

    __asm__ volatile ("mov %0, %%cr0" :: "r"(g_bsp_cr0) : "memory");
    __asm__ volatile ("mov %0, %%cr4" :: "r"(g_bsp_cr4) : "memory");
    // LMA is the CPU's to set; Linux clears it from the copy too.
    wrmsr(MSR_EFER, g_bsp_efer & ~(1ULL << 10));
    idt_load_ap();
    paging_memory_types_apply();
    lapic_init_ap();

    c->seen_cr0 = read_cr0();
    c->seen_cr4 = read_cr4();
    c->seen_efer = rdmsr(MSR_EFER);
    paging_memory_types_now(&c->seen_pat, &c->seen_mtrr_def);

    __atomic_store_n(&c->state, CPU_PARKED, __ATOMIC_RELEASE);
    // Every local source is masked and the I/O APIC routes to the BSP,
    // so what wakes this halt is the BSP's call IPI (smp_call()).
    for (;;) {
        __asm__ volatile ("sti; hlt; cli" ::: "memory");
        if (__atomic_load_n(&c->sched_go, __ATOMIC_ACQUIRE)) break;
    }
    ap_schedule(c);
}

// ---- running a function on another core ------------------------------
//
// Linux's smp_call_function_single(), NT's KeIpiGenericCall(): a
// mailbox in the target's struct cpu and an IPI to say it is full.

#define CALL_MS 1000

static struct spinlock g_call_lock;   // one sender at a time

// **A CORE INSIDE A CALL LOGS NOTHING** (smp.h: fn touches nothing
// shared), and a wait inside one -- a call that calls back -- is such a
// core: its warnings are dropped, as tlb.c's are. Counted lockups still
// say it (spin_wait_overdue()).
static int may_log(void) {
    return !g_cpus || !g_cpus[spin_cpu_index()]->call_running;
}

// **THE WAIT ANSWERS WHAT IS SENT TO THIS CORE MEANWHILE**: the target
// may be waiting on it -- a call back, or a shootdown from a core whose
// call this is. Polled with interrupts off, so the IPI cannot run the
// same call here at once (call_service() is not re-entered).
// Done means REACHED OR PASSED: a later sender's call may already have
// finished after this one (serial arithmetic, so the wrap is harmless).
static int call_reached(const struct cpu *c, uint32_t seq) {
    return (int32_t)(__atomic_load_n(&c->call_done, __ATOMIC_ACQUIRE) - seq) >= 0;
}

static int wait_done(struct cpu *c, uint32_t seq, uint32_t ms) {
    uint64_t end = clocksource_now_ns() + (uint64_t)ms * 1000000ull;
    while (!call_reached(c, seq)) {
        uint64_t f = irq_save();
        smp_poll_ipis();
        irq_restore(f);
        if (clocksource_now_ns() >= end) return 0;
        cpu_relax();
    }
    return 1;
}

// The mailbox's other half: from the IPI, or from a spin with
// interrupts off (smp_poll_ipis()). **NOT RE-ENTERED**: a call that
// spins for a lock (stage 4's kernel-lock test does) polls from inside
// itself, and would otherwise run itself again there.
//
// **THE CALL IS CLAIMED BY SWAPPING call_fn OUT**, and a sender that gave
// up takes it back with the same swap (wait_call()): whichever swap gets
// it decides whether fn runs, so a late core never runs a call whose
// `arg` its sender's returned frame held. Taken back, it is marked done.
static void call_service(struct cpu *c) {
    uint32_t seq = __atomic_load_n(&c->call_seq, __ATOMIC_ACQUIRE);
    if (seq == c->call_done || c->call_running) return;
    c->call_running = 1;
    void (*fn)(void *) = __atomic_exchange_n(&c->call_fn, (void (*)(void *))0, __ATOMIC_ACQ_REL);
    if (fn) { fn(c->call_arg); c->calls_taken++; }
    c->call_running = 0;
    __atomic_store_n(&c->call_done, seq, __ATOMIC_RELEASE);
}

// **A PARKED AP ANSWERS NOTHING BUT THE CALL VECTOR, AND TOUCHES NOTHING
// SHARED ON THE WAY** -- isr_dispatch() sends it here before its first
// global. An exception never arrives: a ring-0 one on any AP is a kernel
// fault, reported and panicked by isr_dispatch() as the BSP's is. An NMI
// no stop asked for is counted and dropped, Linux's "unknown reason".
void smp_ap_interrupt(uint64_t vector) {
    struct cpu *c = this_cpu();
    if (vector == 2) { c->nmi_stray++; return; }
    if (vector < 32) {   // unreachable: isr_dispatch() reports these
        c->wedge_vec = vector + 1;
        smp_stop_here();
    }
    if (vector == LAPIC_SPURIOUS_VECTOR) return;   // no EOI for a spurious one
    lapic_eoi();
    if (vector == SMP_CALL_VECTOR) call_service(c);
}

// ---- stopping the others for a panic -----------------------------------
//
// Linux's smp_send_stop() by NMI, NT's KeFreezeExecution: the panicking
// core must be the only one left running, or a process on another core
// writes on through the countdown and the record stops being the last
// thing that happened. **AN NMI, NOT A CALL**: the core to stop may spin
// with interrupts off for the kernel lock the panicking core holds.

static volatile int g_stopping;

int smp_stop_requested(void) { return __atomic_load_n(&g_stopping, __ATOMIC_ACQUIRE); }

void smp_stop_here(void) {
    __asm__ volatile ("cli");
    struct cpu *c = this_cpu();
    if (c) __atomic_store_n(&c->state, CPU_STOPPED, __ATOMIC_RELEASE);
    for (;;) __asm__ volatile ("cli; hlt");   // an NMI handler that never irets: no more NMIs
}

int smp_stop_others(void) {
    if (!g_cpus) return 0;
    __atomic_store_n(&g_stopping, 1, __ATOMIC_SEQ_CST);
    uint32_t me = spin_cpu_index();
    int sent = 0, stopped = 0;
    for (int i = 0; i < g_cpu_count; i++) {
        struct cpu *c = g_cpus[i];
        if ((uint32_t)i == me || !online(c)) continue;
        if (lapic_send_nmi((uint8_t)c->apic_id)) sent++;
    }
    // A bounded wait (Linux's is a second): a core that never says so is
    // named in the report rather than waited for.
    uint64_t end = clocksource_now_ns() + 100ull * 1000000ull;
    for (int i = 0; i < g_cpu_count; i++) {
        struct cpu *c = g_cpus[i];
        if ((uint32_t)i == me || !c) continue;
        while (online(c) && clocksource_now_ns() < end) cpu_relax();
        if (__atomic_load_n(&c->state, __ATOMIC_ACQUIRE) == CPU_STOPPED) stopped++;
    }
    klog_printf(KLOG_CRIT "smp: core %u stopped %d other core(s) of %d sent an NMI\n",
                me, stopped, sent);
    return stopped;
}

void smp_poll_ipis(void) {
    tlb_service();
    if (g_cpus) call_service(g_cpus[spin_cpu_index()]);   // the BSP too: an AP may call it
}

// A PARKED core that did not take a call is given no more: its next one
// would wait again, and a caller may hold the kernel lock meanwhile.
// **A SCHEDULING ONE IS ONLY SLOW** -- it may have interrupts off for a
// long call, or be a starved vCPU -- and is never written off: it is
// running a process, and a core marked wedged that later took an
// ordinary page fault was halted for good with that process on it
// (smp=run, TCG under load). Linux's CSD-lock timeout warns the same way.
static void wedged(struct cpu *c, int index) {
    if (__atomic_load_n(&c->state, __ATOMIC_ACQUIRE) == CPU_RUNNING) {
        if (may_log())
            klog_printf(KLOG_WARN "smp: core %d did not take a call within %d ms -- "
                        "taken back; still running, so not written off\n", index, CALL_MS);
        return;
    }
    __atomic_store_n(&c->state, CPU_WEDGED, __ATOMIC_RELEASE);
    if (may_log())
        klog_printf(KLOG_WARN "smp: core %d did not take a call within %d ms -- "
                    "given no more\n", index, CALL_MS);
}

// The end of the wait for call `seq` -- the caller's own, never whatever
// the mailbox holds now: a later sender's call would be taken back in its
// place. **ONE THAT HAS STARTED IS WAITED FOR, HOWEVER LONG** -- its `arg`
// is in use until it returns (Linux's csd_lock_wait()), on a parked core
// too: the old write-off returned while fn still ran -- unless its core
// halts in it (a fault panics, and the panic stops this core too). A wait past its second is
// counted as a spinlock lockup is, so ktest names the test it happened
// in. One not started is taken back.
static int wait_call(struct cpu *c, int index, uint32_t seq, uint32_t ms) {
    if (wait_done(c, seq, ms)) return 1;
    // **TAKEN BACK UNDER THE SEND LOCK, AND ONLY WHILE THE MAILBOX HOLDS
    // THIS CALL**: a later sender may have written its own the moment this
    // one finished, and swapping that out would report ours failed and
    // theirs done without running it. A newer seq means ours finished.
    uint64_t f = spin_lock_irqsave(&g_call_lock);
    void (*taken)(void *) = 0;
    if (c->call_seq == seq && !call_reached(c, seq))
        taken = __atomic_exchange_n(&c->call_fn, (void (*)(void *))0, __ATOMIC_ACQ_REL);
    spin_unlock_irqrestore(&g_call_lock, f);
    if (taken) {
        wedged(c, index);
        return 0;
    }
    // Taken already: finished just now, or still running.
    if (call_reached(c, seq)) return 1;
    spin_wait_overdue((uintptr_t)&c->call_done, (uintptr_t)__builtin_return_address(0),
                      spin_cpu_index(), (uint32_t)index);
    if (may_log())
        klog_printf(KLOG_WARN "smp: core %d is still running a call after %u ms -- "
                    "waiting for it\n", index, ms);
    while (!wait_done(c, seq, CALL_MS))
        if (__atomic_load_n(&c->state, __ATOMIC_ACQUIRE) <= CPU_STOPPED) return 0;
    return 1;
}

// Writes core `index`'s mailbox and sends the IPI, with the caller's
// interrupts already off: the call's seq, or -1 when the core takes none
// now (not online, its last call unfinished, the IPI refused).
static int64_t send_call(int index, void (*fn)(void *), void *arg) {
    if (!g_cpus || index < 0 || index >= g_cpu_count) return -1;
    struct cpu *c = g_cpus[index];
    uint32_t st = __atomic_load_n(&c->state, __ATOMIC_ACQUIRE);
    if (st != CPU_PARKED && st != CPU_RUNNING) return -1;   // a scheduling AP takes calls too

    uint64_t f = spin_lock_irqsave(&g_call_lock);
    // The last call to this core has to be finished before its mailbox
    // is written again -- checked, never waited for here, with
    // interrupts off. A wait = 0 caller waits with smp_call_wait().
    uint32_t prev = c->call_seq;
    if (__atomic_load_n(&c->call_done, __ATOMIC_ACQUIRE) != prev) {
        spin_unlock_irqrestore(&g_call_lock, f);
        if (may_log()) klog_printf(KLOG_WARN "smp: core %d is still running its last call\n", index);
        return -1;
    }
    c->call_fn = fn;
    c->call_arg = arg;
    uint32_t seq = prev + 1;
    __atomic_store_n(&c->call_seq, seq, __ATOMIC_RELEASE);
    int sent = lapic_send_ipi((uint8_t)c->apic_id, SMP_CALL_VECTOR);
    spin_unlock_irqrestore(&g_call_lock, f);
    if (!sent) {
        if (may_log()) klog_printf(KLOG_WARN "smp: the call IPI to core %d was not accepted\n", index);
        return -1;
    }
    return seq;
}

int smp_call(int index, void (*fn)(void *), void *arg, int wait) {
    // **"WHICH CORE AM I" AND THE SEND ARE ONE STEP, INTERRUPTS OFF**: a
    // preemption between them could move the caller onto `index`, which
    // then sent itself the call. A move during the wait is harmless --
    // the IPI waits on the core it was sent to.
    uint64_t f0 = irq_save();
    if (index == (int)spin_cpu_index()) {   // the caller's own core
        fn(arg);   // interrupts off, as fn runs elsewhere
        irq_restore(f0);
        return 1;
    }
    int64_t seq = send_call(index, fn, arg);
    irq_restore(f0);
    if (seq < 0) return 0;
    return wait ? wait_call(g_cpus[index], index, (uint32_t)seq, CALL_MS) : 1;
}

// No "is it my own core" shortcut: read with interrupts on it could be
// stale. A call smp_call() ran here left the mailbox as it was -- done.
// Waits for the LAST call sent to the core, so it is for a caller whose
// own wait = 0 call that is; smp_call_others() waits on its own seqs.
int smp_call_wait(int index, uint32_t ms) {
    if (!g_cpus) return index == 0;   // one core: smp_call() ran it in place
    if (index < 0 || index >= g_cpu_count) return 0;
    struct cpu *c = g_cpus[index];
    return wait_call(c, index, __atomic_load_n(&c->call_seq, __ATOMIC_ACQUIRE), ms);
}

// A write-combining change after bring-up (paging_set_write_combining()):
// each AP replays the BSP's new snapshot, and records what it now has
// for smp_test.c, as at bring-up.
static void memory_types_cb(void *arg) {
    (void)arg;
    struct cpu *c = this_cpu();
    paging_memory_types_apply();
    paging_memory_types_now(&c->seen_pat, &c->seen_mtrr_def);
}

void smp_memory_types_changed(void) {
    int want = 0, me = (int)spin_cpu_index();
    for (int i = 0; i < smp_cpu_count(); i++) want += i != me && online(smp_cpu_at(i));
    int done = smp_call_others(memory_types_cb, 0);
    if (done != want)
        klog_printf(KLOG_WARN "smp: %d of %d other core(s) took the new memory types\n",
                    done, want);
}

// ---- smp=run ---------------------------------------------------------

static int g_run_wanted = 1, g_scheduling;   // smp=run is the default (smp=park opts out)
static int wait_state(struct cpu *c, uint32_t at_least, uint32_t ms);

int smp_scheduling(void) { return g_scheduling; }

void smp_resched(int index) {
    if (!g_cpus || index <= 0 || index >= g_cpu_count) return;
    (void)lapic_send_ipi((uint8_t)g_cpus[index]->apic_id, SMP_SCHED_VECTOR);
}

static void go_cb(void *arg) {
    (void)arg;
    __atomic_store_n(&this_cpu()->sched_go, 1, __ATOMIC_RELEASE);
}

void smp_start_scheduling(void) {
    if (smp_online_count() < 2) return;
    if (!g_run_wanted) {
        klog_write("smp: smp=park -- the other cores stay parked\n");
        return;
    }
    if (!lapic_timer_rate()) {
        klog_write(KLOG_WARN "smp: smp=run needs the LAPIC timer for each core's tick -- "
                   "the other cores stay parked\n");
        return;
    }
    int started = 0;
    for (int i = 1; i < g_cpu_count; i++) {
        struct cpu *c = g_cpus[i];
        if (c->state != CPU_PARKED) continue;
        c->run_start_ns = clocksource_now_ns();
        if (!smp_call(i, go_cb, 0, 1) || !wait_state(c, CPU_RUNNING, CALL_MS)) {
            klog_printf(KLOG_WARN "smp: core %d did not start scheduling\n", i);
            continue;
        }
        started++;
    }
    g_scheduling = started > 0;
    klog_printf("smp: smp=run -- %d other core(s) scheduling processes; "
                "device interrupts and the kernel context stay on core 0\n", started);
}

// Every core but the caller -- which under smp=run may be an AP, the BSP
// then being one of the others. **ALL SENT BEFORE ANY WAIT, INTERRUPTS
// OFF**: "the caller's core" is decided once, where no move can make it
// skip the core it left (Linux's smp_call_function_many()). A move during
// the waits is harmless, as in smp_call().
int smp_call_others(void (*fn)(void *), void *arg) {
    int n = smp_cpu_count();
    if (n < 2) return 0;
    int64_t seq[n];
    uint64_t f = irq_save();
    int me = (int)spin_cpu_index();
    for (int i = 0; i < n; i++) seq[i] = i == me ? -1 : send_call(i, fn, arg);
    irq_restore(f);
    int done = 0;
    for (int i = 0; i < n; i++)
        done += seq[i] >= 0 && wait_call(g_cpus[i], i, (uint32_t)seq[i], CALL_MS);
    return done;
}

// ---- the BSP's side --------------------------------------------------

// The highest free pair of pages below the EBDA: Linux's real-mode
// blob is placed top-down below 1 MiB too. Page 0 holds the IVT and BDA.

static void find_tramp_cb(const struct multiboot_mmap_region *r) {
    if (r->type != 1) return;
    uint64_t lo = (r->base + 0xFFF) & ~0xFFFull;
    uint64_t hi = (r->base + r->length) & ~0xFFFull;
    if (lo < 0x1000) lo = 0x1000;
    if (hi > 0x9F000) hi = 0x9F000;
    for (uint64_t p = hi; p >= lo + 0x2000; p -= 0x1000) {
        uint64_t start = p - 0x2000;
        if (start <= g_tramp_phys) return;     // not better than what we have
        if (!pmm_boot_range_reserved(start, p)) { g_tramp_phys = start; return; }
    }
}

// Linux's smp_quirk_init_udelay(): no 10 ms INIT wait on a modern part.
static int init_delay_ms(void) {
    struct cpu_info ci;
    cpu_info_get(&ci);
    if (k_strcmp(ci.vendor, "GenuineIntel") == 0 && ci.family >= 6) return 0;
    if (k_strcmp(ci.vendor, "AuthenticAMD") == 0 && ci.family >= 0xF) return 0;
    if (k_strcmp(ci.vendor, "HygonGenuine") == 0) return 0;
    return 10;
}

static int wait_state(struct cpu *c, uint32_t at_least, uint32_t ms) {
    uint64_t end = clocksource_now_ns() + (uint64_t)ms * 1000000ull;
    while (__atomic_load_n(&c->state, __ATOMIC_ACQUIRE) < at_least) {
        if (clocksource_now_ns() >= end) return 0;
        cpu_relax();
    }
    return 1;
}

static int start_ap(struct cpu *c, struct smp_tramp_data *d, int init_ms) {
    d->stack = (uint64_t)(uintptr_t)c + 4096 + AP_STACK_BYTES;
    d->cpu = (uint64_t)(uintptr_t)c;
    __atomic_store_n(&c->state, CPU_STARTING, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);   // the data page before the IPI

    uint8_t page = (uint8_t)(g_tramp_phys >> 12);
    if (!lapic_send_init((uint8_t)c->apic_id)) return 0;
    if (init_ms) clocksource_delay_ms((uint32_t)init_ms);
    // Two STARTUPs, the MP spec's sequence; a core already running
    // ignores the second.
    for (int i = 0; i < 2; i++) {
        if (!lapic_send_startup((uint8_t)c->apic_id, page)) return 0;
        delay_us(init_ms ? 300 : 10);
        if (__atomic_load_n(&c->state, __ATOMIC_ACQUIRE) != CPU_STARTING) break;
        delay_us(init_ms ? 200 : 10);
    }
    return wait_state(c, CPU_PARKED, CALLIN_MS);
}

void smp_init(void) {
    struct cpu *boot = gdt_boot_cpu();
    boot->index = 0;
    boot->apic_id = lapic_read_id();
    boot->state = CPU_RUNNING;

    // `maxcpus=N` counts the BSP, and 0 is `nosmp`, as in Linux.
    uint32_t max = 0;
    char word[16];
    if (multiboot_cmdline_value("maxcpus=", word, sizeof word)) {
        if (!k_parse_u32(word, &max)) {
            klog_printf("smp: maxcpus=%s is not a number -- ignored\n", word);
            max = 0;
        } else if (max <= 1) {
            klog_printf("smp: maxcpus=%u -- one core\n", max);
            return;
        }
    }
    // `smp=run`, the default, schedules on the APs once boot is over;
    // `smp=park` leaves them parked (docs/boot-flags.md).
    if (multiboot_cmdline_value("smp=", word, sizeof word)) {
        if (!k_strcmp(word, "park")) g_run_wanted = 0;
        else if (k_strcmp(word, "run"))
            klog_printf("smp: smp=%s is not run or park -- running\n", word);
    }
    if (multiboot_cmdline_has("nosmp")) {
        klog_write("smp: disabled by `nosmp` -- one core\n");
        return;
    }
    if (!lapic_present()) {
        klog_write("smp: no Local APIC -- one core\n");
        return;
    }

    // Which cores: every MADT entry the firmware ENABLED, but this one.
    // An xAPIC IPI names an 8-bit destination and 0xFF is broadcast, so
    // an id above 254 (an x2APIC-only core) is out of reach here.
    int want = 0;
    for (int i = 0; i < acpi_cpu_count(); i++) {
        const struct acpi_cpu *a = acpi_cpu_at(i);
        if (!(a->flags & ACPI_CPU_ENABLED) || a->apic_id == boot->apic_id) continue;
        if (a->apic_id > 254) {
            klog_printf("smp: core with APIC id %u needs x2APIC -- not started\n", a->apic_id);
            continue;
        }
        if (max && 1 + want >= (int)max) {
            klog_printf("smp: maxcpus=%u -- APIC id %u not started\n", max, a->apic_id);
            continue;
        }
        want++;
    }
    if (want == 0) {
        klog_write("smp: no other core to start\n");
        return;
    }

    multiboot_mmap_foreach(find_tramp_cb);
    if (!g_tramp_phys) {
        klog_write(KLOG_WARN "smp: no free pages below 1 MiB for the trampoline -- one core\n");
        return;
    }
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    cr3 &= ~0xFFFull;
    if (cr3 >= 0x100000000ull) {
        // The trampoline loads CR3 in 32-bit mode. The kernel's PML4 is in
        // its own .bss, under 4 GiB by the link and the relocator.
        klog_printf(KLOG_WARN "smp: kernel PML4 at 0x%lx is above 4 GiB -- one core\n", cr3);
        return;
    }

    g_cpus = kzalloc((size_t)(1 + want) * sizeof *g_cpus);
    if (!g_cpus) {
        klog_write(KLOG_WARN "smp: out of memory for the core table -- one core\n");
        return;
    }
    g_cpus[0] = boot;

    // What every AP replays (ap_entry()).
    g_bsp_cr0 = read_cr0();
    g_bsp_cr4 = read_cr4();
    g_bsp_efer = rdmsr(MSR_EFER);
    paging_memory_types_snapshot();

    // The code page first, while it is still writable; then executable
    // for the bring-up (and read-only: never W+X).
    uint8_t *code = (uint8_t *)(uintptr_t)g_tramp_phys;
    struct smp_tramp_data *d = (struct smp_tramp_data *)(uintptr_t)(g_tramp_phys + 0x1000);
    k_memset(code, 0, 0x2000);
    k_memcpy(code, smp_tramp_start, (size_t)(smp_tramp_end - smp_tramp_start));
    d->gdt[0] = 0;
    d->gdt[1] = TRAMP_CODE32;
    d->gdt[2] = TRAMP_DATA32;
    d->gdt[3] = TRAMP_CODE64;
    d->gdtr_limit = sizeof d->gdt - 1;
    d->gdtr_base = (uint32_t)(g_tramp_phys + 0x1000);
    d->jmp32_off = (uint32_t)(g_tramp_phys + (uint64_t)(smp_tramp_32 - smp_tramp_start));
    d->jmp32_sel = 0x08;
    d->jmp64_off = (uint32_t)(g_tramp_phys + (uint64_t)(smp_tramp_64 - smp_tramp_start));
    d->jmp64_sel = 0x18;
    d->cr3 = cr3;
    d->entry = (uint64_t)(uintptr_t)ap_entry;
    if (!paging_set_kernel_exec(g_tramp_phys, 4096, 1)) {
        klog_write(KLOG_WARN "smp: could not make the trampoline executable -- one core\n");
        return;
    }

    int init_ms = init_delay_ms();
    uint64_t t0 = clocksource_now_ns();
    int started = 0;
    for (int i = 0; i < acpi_cpu_count() && g_cpu_count < 1 + want; i++) {
        const struct acpi_cpu *a = acpi_cpu_at(i);
        if (!(a->flags & ACPI_CPU_ENABLED) || a->apic_id == boot->apic_id || a->apic_id > 254)
            continue;

        uint64_t phys = pmm_alloc_contiguous(AP_PAGES, PMM_ZONE_ANY);
        if (!phys) {
            klog_write(KLOG_WARN "smp: out of memory for a core's stacks -- stopping here\n");
            break;
        }
        struct cpu *c = (struct cpu *)(uintptr_t)phys;   // identity-mapped
        k_memset(c, 0, sizeof *c);
        c->self = c;
        c->index = (uint32_t)g_cpu_count;
        c->sched_cur = -1;     // its idle loop, once it schedules
        c->sched_rot = -2;     // ROT_KERNEL
        c->apic_id = a->apic_id;
        g_cpus[g_cpu_count++] = c;

        if (start_ap(c, d, init_ms)) {
            started++;
            continue;
        }
        // Back to wait-for-SIPI, so a core that is merely slow cannot wake
        // later into a data page that names someone else's stack. Its
        // memory stays allocated: it is not known to have stopped using it.
        klog_printf(KLOG_WARN "smp: core with APIC id %u did not %s within %d ms -- left in INIT\n",
                    c->apic_id, c->state == CPU_STARTING ? "call in" : "finish its setup",
                    CALLIN_MS);
        lapic_send_init((uint8_t)c->apic_id);
        c->state = CPU_FAILED;
    }

    paging_set_kernel_exec(g_tramp_phys, 4096, 0);
    uint64_t us = (clocksource_now_ns() - t0) / 1000;
    klog_printf("smp: %d of %d core(s) online, %d parked; started in %lu us "
                "(trampoline at 0x%lx, INIT delay %d ms)\n",
                1 + started, 1 + want, started, us, g_tramp_phys, init_ms);
}
