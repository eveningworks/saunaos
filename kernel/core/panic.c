// panic_finish() -- what every kernel panic does once it has reported.
// See kernel/panic.h for the contract and panic_store.h for the record.
#include "panic.h"
#include "panic_store.h"
#include "multiboot.h"
#include "knum.h"
#include "klog.h"
#include "kfmt.h"
#include "vga.h"
#include "serial.h"   // serial_flush()
#include "clocksource.h"
#include "io.h"
#include "kdebug.h"
#include "lockup.h"    // lockup_panic_stop()
#include "watchdog.h"  // fed through the countdown
#include "string.h"
#include "smp.h"       // smp_stop_others()
#include "spinlock.h"  // spin_cpu_index()

int panic_parse_secs(const char *value, int *out) {
    int64_t v;
    if (!value || !k_parse_i64(value, &v)) return 0;
    if (v > 3600) v = 3600;
    if (v < -1) v = -1;
    *out = (int)v;
    return 1;
}

// `panic_reset=hard` uses the firmware's reset register straight away;
// anything else (the default, `warm`) tries power_reset_warm() first.
static int panic_reset_warm(void) {
    char v[8];
    return !(multiboot_cmdline_value("panic_reset=", v, sizeof v) && !k_strcmp(v, "hard"));
}

static int panic_secs(void) {
    char v[16];
    int secs = PANIC_DEFAULT_SECS;
    if (multiboot_cmdline_value("panic=", v, sizeof v)) panic_parse_secs(v, &secs);
    return secs;
}

// A key PRESSED on the PS/2 keyboard -- a make code, so not the release
// of the Enter that ran the command which panicked, not a controller
// reply (0xFA/0xFE), not the mouse sharing the port, and not COM1, where
// a test harness's resync bytes would end a countdown nobody is reading.
// Polled, because interrupts are off for good.
static int key_waiting(void) {
    uint8_t st = inb(0x64);
    if (st == 0xFF || !(st & 0x01)) return 0;
    uint8_t b = inb(0x60);
    if (st & 0x20) return 0;
    return b < 0x80 && b != 0x00;
}

// Whatever was already buffered when the panic hit is not a key pressed
// in answer to the countdown.
static void drain_8042(void) {
    for (int i = 0; i < 64; i++) {
        uint8_t st = inb(0x64);
        if (st == 0xFF || !(st & 0x01)) return;
        inb(0x60);
    }
}

static void wait_100ms(void) {
    if (clocksource_deadline_capable()) { clocksource_delay_ms(100); return; }
    // No clock that runs with interrupts off (only before the TSC or PM
    // timer registers): an I/O-port write is about a microsecond.
    for (int i = 0; i < 100000; i++) outb(0x80, 0);
}

// ONE CORE PANICS. The first to get here owns it and stops the rest; a
// second core faulting at the same moment halts instead of racing it to
// the record. The owner arriving again (a fault inside its own report)
// passes, and panic_finish() then resets.
static volatile int g_panic_core = -1;

void panic_begin(void) {
    __asm__ volatile ("cli");
    int me = (int)spin_cpu_index_raw();   // interrupts off: this core's
    int prev = -1;
    if (__atomic_compare_exchange_n(&g_panic_core, &prev, me, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        // This core's cycle-counter NMI off before anything else: the
        // report and the countdown run with interrupts off, which it would
        // read as a hard lockup.
        lockup_panic_stop();
        smp_stop_others();
        return;
    }
    if (prev != me) smp_stop_here();
}

int panic_owned_here(void) {
    int owner = __atomic_load_n(&g_panic_core, __ATOMIC_ACQUIRE);
    return owner >= 0 && owner == (int)spin_cpu_index_raw();
}

void panic_finish(void) {
    panic_begin();
    // A fault INSIDE the panic path lands here again; do not try to save
    // or count a second time, just get the machine back.
    static int entered;
    if (entered++) power_reset_hardware();
    // No cycle-counter NMI in the countdown, which runs with interrupts
    // off on purpose and would read as a hard lockup.
    lockup_panic_stop();
    kdebug_panic();

    int secs = panic_secs();
    int warm = panic_reset_warm();
    if (secs > 0)
        klog_printf(KLOG_CRIT "panic: restarting in %d s, %s (panic=0 on the GRUB line halts instead)\n",
                    secs, warm ? "warm (panic_reset=hard for the firmware's reset)" : "hard");
    else if (secs == 0)
        klog_write(KLOG_CRIT "panic: halted (panic=0)\n");
    // A halt is meant to be read, so the watchdog stops; a countdown feeds
    // it, so its hard reset cannot overtake the warm one chosen here.
    if (secs == 0) watchdog_panic_stop();
    panic_store_save();
    serial_flush();

    if (secs == 0) {
        vga_present_force();
        for (;;) __asm__ volatile ("hlt");
    }
    drain_8042();
    for (int left = secs * 10; left > 0; left--) {
        if (left % 10 == 0) {
            vga_printf("\rRestarting in %d s -- press a key to restart now. ", left / 10);
            vga_present_force();
        }
        if (key_waiting()) break;
        watchdog_panic_ping();
        wait_100ms();
    }
    vga_write("\nRestarting.\n");
    vga_present_force();
    serial_flush();
    // The firmware and GRUB get the watchdog's whole timeout (the driver
    // halts it at probe); an INIT# reset does not stop it.
    watchdog_panic_ping();
    if (warm) power_reset_warm();
    power_reset_hardware();
}
