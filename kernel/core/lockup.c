// The lockup detector -- see kernel/lockup.h for the three checks.
//
// THE HARD CHECK'S HEARTBEAT IS "AN INTERRUPT WAS TAKEN OR THE CPU WENT
// IDLE", counted in g_beat by every timer event and every idle halt. The
// NMI comes from the cycle counter, which stops while the CPU is halted,
// so an idle machine raises none; a CPU that runs with interrupts off
// raises one every LOCKUP_NMI_MS or so and finds the beat unchanged. Time
// is measured with the TSC, which counts with interrupts off -- the
// default clocksource (the PIT) is advanced BY the timer interrupt.
//
// THE NMI MAY LAND INSIDE klog_write(); the report is written anyway,
// since the machine is going down and a torn line beats no line.
#include "lockup.h"
#include "watchdog.h"
#include "panic.h"
#include "scheduler.h"
#include "clocksource.h"
#include "cpuinfo.h"
#include "random_hw.h"   // arch_rdtsc()
#include "msr.h"
#include "lapic.h"
#include "idt.h"
#include "multiboot.h"
#include "knum.h"
#include "klog.h"
#include "kfmt.h"
#include "vga.h"
#include "string.h"
#include "initcall.h"
#include "ktest.h"

#define MSR_PMC0             0xC1
#define MSR_PERFEVTSEL0      0x186
#define MSR_PERF_GLOBAL_STATUS   0x38E
#define MSR_PERF_GLOBAL_CTRL     0x38F
#define MSR_PERF_GLOBAL_OVF_CTRL 0x390

// UNHALTED_CORE_CYCLES (0x3C, umask 0), ring 3 and ring 0, interrupt on
// overflow, enabled.
#define EVTSEL_CYCLES_INT   (0x3Cu | (1u << 16) | (1u << 17) | (1u << 20) | (1u << 22))
#define LVT_DM_NMI          0x400u
#define LVT_MASKED          0x10000u

#define NS 1000000000ull

enum { MODE_UNSET = -1, MODE_OFF, MODE_WARN, MODE_PANIC };

static int g_mode = MODE_UNSET;
static uint64_t g_hung_ns;            // 0: hung tasks not checked

static volatile uint64_t g_beat;      // interrupts are being taken

// soft
static uint64_t g_last_tick_ns, g_soft_ns, g_last_switches;
static int g_soft_said;
static uint64_t g_next_hung_ns;

// hard
static int g_nmi_armed;
static uint64_t g_reload;             // PMC0's start value: -period
static uint64_t g_tsc_hz;
static uint64_t g_hard_tsc;           // LOCKUP_HARD_SECS in TSC ticks
static uint64_t g_seen_beat, g_seen_tsc;
static int g_hard_said;

uint64_t lockup_soft_step(uint64_t stretch_ns, uint64_t since_last_ns, int guarded,
                          int idle, int switched, int others_ready) {
    if (!guarded || idle || switched || !others_ready) return 0;
    return stretch_ns + since_last_ns;
}

// Both lockups end here. `lockup=warn` carries on and says it once per
// stretch; the default panics, which keeps the report in the panic store.
static void lockup_end(void) {
    if (g_mode != MODE_PANIC) return;
    vga_set_color(VGA_WHITE, VGA_RED);
    vga_write("\n*** KERNEL PANIC: lockup detected -- see the log ***\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    klog_write(KLOG_CRIT "PANIC: lockup (lockup=warn reports and carries on)\n");
    panic_finish();
}

static void hung_scan(uint64_t now) {
    if (!g_hung_ns || now < g_hung_ns) return;
    struct sched_hung h;
    // At most two a scan: a deadlock parks everyone behind it, and the
    // first is the one worth reading.
    for (int n = 0; n < 2 && scheduler_hung_find(now - g_hung_ns, &h); n++) {
        klog_printf(KLOG_ERR "LOCKUP: hung task -- pid %d \"%s\" has waited in the kernel on %s "
                    "for %llu s\n", h.pid, h.name, sched_wait_reason_name(h.reason),
                    (now - h.parked_at_ns) / NS);
        idt_log_stack_scan(h.rsp, 8);
    }
}

void lockup_idle(void) { g_beat++; }

void lockup_tick(uint64_t *regs) {
    g_beat++;
    if (g_mode == MODE_UNSET) return;
    uint64_t now = clocksource_now_ns();
    uint64_t dt = g_last_tick_ns && now > g_last_tick_ns ? now - g_last_tick_ns : 0;
    g_last_tick_ns = now;

    // A debugger stop ages every park by its length: no hung-task scan
    // until a whole threshold has passed after it.
    if (watchdog_debugger_held()) {
        g_soft_ns = 0;
        g_next_hung_ns = now + g_hung_ns;
        return;
    }
    if (now >= g_next_hung_ns) {
        g_next_hung_ns = now + 5 * NS;
        hung_scan(now);
    }
    if (g_mode == MODE_OFF) return;

    uint64_t sw = scheduler_switch_count();
    int switched = sw != g_last_switches;
    g_last_switches = sw;
    // RING 0 WITH THE PREEMPTION GUARD RAISED: what a soft lockup is. A
    // context the scheduler declines to rotate for its own reasons (an RT
    // process, the legacy `run` context) is not one.
    int guarded = (regs[18] & 3) == 0 && scheduler_preempt_depth() > 0;
    // The kernel context's halt flag stays up while a process that
    // preempted it out of the halt runs, so it counts only when the
    // kernel context is the one on the CPU.
    int idle = scheduler_kernel_running() && scheduler_cpu_halting();
    // Asked last, and only when it can matter: it walks the process
    // table, and this runs every tick.
    int ready = guarded && !idle && !switched && scheduler_any_ready();
    g_soft_ns = lockup_soft_step(g_soft_ns, dt, guarded, idle, switched, ready);
    if (!g_soft_ns) { g_soft_said = 0; return; }
    if (g_soft_ns < (uint64_t)LOCKUP_SOFT_SECS * NS || g_soft_said) return;
    g_soft_said = 1;
    klog_printf(KLOG_CRIT "LOCKUP: soft lockup -- the kernel has held the CPU for %llu s with "
                "preemption off and work waiting (pid %d)\n", g_soft_ns / NS,
                scheduler_current_pid());
    idt_report_frame(regs);
    lockup_end();
}

static void pmu_arm(void) {
    wrmsr(MSR_PMC0, g_reload);
    lapic_set_lvt_perf(LVT_DM_NMI);
}

int lockup_nmi(uint64_t *regs) {
    if (!g_nmi_armed || !(rdmsr(MSR_PERF_GLOBAL_STATUS) & 1)) return 0;
    wrmsr(MSR_PERF_GLOBAL_OVF_CTRL, 1);
    pmu_arm();   // the LVT masks itself on delivery

    uint64_t now = arch_rdtsc();
    if (watchdog_debugger_held() || g_beat != g_seen_beat) {
        g_seen_beat = g_beat;
        g_seen_tsc = now;
        g_hard_said = 0;
        return 1;
    }
    if (now - g_seen_tsc < g_hard_tsc || g_hard_said) return 1;
    g_hard_said = 1;
    klog_printf(KLOG_CRIT "LOCKUP: hard lockup -- no interrupt taken for %llu s: the CPU is "
                "running with interrupts off\n", (unsigned long long)((now - g_seen_tsc) / g_tsc_hz));
    idt_report_frame(regs);
    lockup_end();
    return 1;
}

void lockup_panic_stop(void) {
    if (!g_nmi_armed) return;
    g_nmi_armed = 0;
    wrmsr(MSR_PERF_GLOBAL_CTRL, 0);
    lapic_set_lvt_perf(LVT_MASKED);
}

static void cpuid_a(uint32_t *eax, uint32_t *ebx) {
    uint32_t ecx = 0, edx;
    uint32_t max;
    __asm__ volatile ("cpuid" : "=a"(max), "=b"(*ebx), "=c"(ecx), "=d"(edx) : "a"(0), "c"(0));
    if (max < 0xA) { *eax = 0; *ebx = 0; return; }
    __asm__ volatile ("cpuid" : "=a"(*eax), "=b"(*ebx), "=c"(ecx), "=d"(edx) : "a"(0xA), "c"(0));
}

// 1 when the NMI is armed; otherwise *why says what is missing.
static int nmi_setup(const char **why) {
    uint32_t a, b;
    cpuid_a(&a, &b);
    uint32_t version = a & 0xFF, gp = (a >> 8) & 0xFF, evlen = a >> 24;
    if (version < 2 || gp < 1) { *why = "no architectural performance counter (v2+)"; return 0; }
    if (evlen < 1 || (b & 1)) { *why = "the counter cannot count core cycles"; return 0; }
    if (!lapic_present()) { *why = "no local APIC to deliver the NMI"; return 0; }
    uint64_t hz = cpu_tsc_hz();
    if (!hz) { *why = "no calibrated TSC"; return 0; }

    // About a second of cycles a period, below 2^31: PMC0 written through
    // its legacy MSR takes 32 bits, sign-extended.
    uint64_t period = hz < 0x7FFFFFFFull ? hz : 0x7FFFFFFFull;
    g_reload = (uint64_t)(-(int64_t)period);
    g_tsc_hz = hz;
    g_hard_tsc = hz * LOCKUP_HARD_SECS;
    g_seen_beat = g_beat;
    g_seen_tsc = arch_rdtsc();

    wrmsr(MSR_PERF_GLOBAL_CTRL, 0);
    wrmsr(MSR_PERFEVTSEL0, 0);
    wrmsr(MSR_PERF_GLOBAL_OVF_CTRL, 1);
    pmu_arm();
    wrmsr(MSR_PERFEVTSEL0, EVTSEL_CYCLES_INT);
    g_nmi_armed = 1;
    wrmsr(MSR_PERF_GLOBAL_CTRL, 1);
    return 1;
}

static void lockup_init(void) {
    char v[16];
    int mode = MODE_PANIC;
    if (multiboot_cmdline_value("lockup=", v, sizeof v)) {
        if (!k_strcmp(v, "off")) mode = MODE_OFF;
        else if (!k_strcmp(v, "warn")) mode = MODE_WARN;
        else if (k_strcmp(v, "panic")) klog_printf("lockup: lockup=%s is not off, warn or panic -- panic\n", v);
    }
    int hung = LOCKUP_HUNG_SECS;
    if (multiboot_cmdline_value("hung_task=", v, sizeof v)) {
        int64_t n;
        if (!k_strcmp(v, "off")) hung = 0;
        else if (k_parse_i64(v, &n) && n >= 0 && n <= 86400) hung = (int)n;
    }
    g_hung_ns = (uint64_t)hung * NS;

    if (mode == MODE_OFF) {
        klog_printf("lockup: off (lockup=off); hung tasks %s\n", hung ? "checked" : "off");
        g_mode = mode;
        return;
    }
    const char *why = 0;
    int nmi = nmi_setup(&why);
    klog_printf("lockup: soft %d s; hard %d s %s%s; hung tasks %d s; a lockup %s\n",
                LOCKUP_SOFT_SECS, LOCKUP_HARD_SECS,
                nmi ? "(cycle-counter NMI)" : "NOT DETECTED -- ", nmi ? "" : why, hung,
                mode == MODE_PANIC ? "panics (lockup=warn to carry on)" : "is logged (lockup=warn)");
    g_mode = mode;
}
INITCALL(lockup_init, INIT_BUS);

KTEST("lockup", "a soft lockup is only guarded ring 0, busy, unswitched and with work waiting") {
    KTEST_ASSERT_EQ(lockup_soft_step(5, 3, 1, 0, 0, 1), 8);
    KTEST_ASSERT_EQ(lockup_soft_step(5, 3, 0, 0, 0, 1), 0);   // ring 3, or no guard
    KTEST_ASSERT_EQ(lockup_soft_step(5, 3, 1, 1, 0, 1), 0);   // the idle halt
    KTEST_ASSERT_EQ(lockup_soft_step(5, 3, 1, 0, 1, 1), 0);   // the scheduler moved
    KTEST_ASSERT_EQ(lockup_soft_step(5, 3, 1, 0, 0, 0), 0);   // nothing waiting
}
