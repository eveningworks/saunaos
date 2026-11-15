// Intel's TCO watchdog timer, in the LPC bridge of ICH9 and every PCH up
// to the 9 series -- Linux's iTCO_wdt "version 2". QEMU's q35 (ICH9) has
// one, and so do both Broadwell laptops (Wildcat Point-LP).
//
// THE TIMER COUNTS DOWN TWICE. The first time it reaches zero it sets
// TIMEOUT and raises an SMI (when the firmware enabled one) and reloads;
// the second time, with TIMEOUT still set, the chipset resets the machine
// -- unless GCS.NO_REBOOT is set, which is the firmware's (and a stopped
// watchdog's) way of keeping it harmless. So a feed clears TIMEOUT as well
// as reloading, and a timeout of N seconds is two counts of N/2.
//
// THE TRAP: THE FIRMWARE'S SMI HANDLER OWNS THE FIRST EXPIRY. One that
// clears TIMEOUT makes the second count never reset anything; the ASUS's
// resets the machine there instead, at HALF the timeout. Linux turns the
// SMI off on TCO v1 only (`turn_SMI_watchdog_clear_off`); this leaves
// SMI_EN alone too, so time a deliberate hang on a new machine.
//
// Registers: TCOBASE = the ACPI PM base (LPC 0x40) + 0x60; GCS is a
// 32-bit register at RCBA (LPC 0xF0) + 0x3410.
#include "watchdog.h"
#include "pci.h"
#include "pci_internal.h"
#include "pci_driver.h"
#include "driver.h"
#include "io.h"
#include "klog.h"
#include "kfmt.h"
#include "ktest.h"
#include <stdint.h>

DRIVER_DECLARE("itco", "watchdog", "Intel TCO watchdog timer (ICH9-PCH9)");

#define LPC_ABASE     0x40
#define LPC_ACPI_CNTL 0x44
#define ACPI_EN       0x80
#define LPC_RCBA      0xF0
#define RCBA_EN       0x1
#define GCS_OFFSET    0x3410
#define GCS_NO_REBOOT 0x20

#define TCO_RLD   0x00
#define TCO1_STS  0x04
#define TCO2_STS  0x06
#define TCO1_CNT  0x08
#define TCO_TMR   0x12

#define TCO1_TIMEOUT    0x0008
#define TCO2_SECOND_TO  0x0002
#define TCO2_BOOT_STS   0x0004
#define TCO_TMR_HLT     0x0800
#define TCO_LOCK        0x1000

#define TICK_MS     600     // one count of TCO_TMR
#define TMR_MIN     4       // "values 0h-3h are ignored"
#define TMR_MAX     0x3FF

static uint16_t g_tco;              // TCOBASE
static volatile uint32_t *g_gcs;    // identity-mapped, below 4 GiB

// The parts Linux's lpc_ich lists as iTCO v2, by range.
static int tco_v2_part(uint16_t id) {
    static const struct { uint16_t lo, hi; } r[] = {
        { 0x2910, 0x291F },   // ICH9 (QEMU's q35 is 0x2918)
        { 0x3A14, 0x3A1A },   // ICH10
        { 0x3B00, 0x3B1F },   // 5 series
        { 0x1C40, 0x1C5F },   // 6 series
        { 0x1E40, 0x1E5F },   // 7 series
        { 0x8C40, 0x8C5F },   // 8 series (Lynx Point)
        { 0x9C40, 0x9C47 },   // Lynx Point-LP
        { 0x8CC1, 0x8CC6 },   // 9 series
        { 0x9CC1, 0x9CC9 },   // Wildcat Point-LP (the ASUS's 0x9CC7)
        { 0x8D40, 0x8D5F },   // Wellsburg
    };
    for (unsigned i = 0; i < sizeof r / sizeof r[0]; i++)
        if (id >= r[i].lo && id <= r[i].hi) return 1;
    return 0;
}

// Two counts make one timeout, a count being 0.6 s.
static int itco_ticks_for(int secs) {
    int t = (secs * 1000) / (2 * TICK_MS);
    if (t < TMR_MIN) t = TMR_MIN;
    if (t > TMR_MAX) t = TMR_MAX;
    return t;
}

static int no_reboot(int set) {
    uint32_t v = *g_gcs;
    v = set ? (v | GCS_NO_REBOOT) : (v & ~GCS_NO_REBOOT);
    *g_gcs = v;
    return ((*g_gcs & GCS_NO_REBOOT) != 0) == (set != 0);
}

static void tco_ping(void) {
    outw((uint16_t)(g_tco + TCO1_STS), TCO1_TIMEOUT);   // write 1 to clear
    outw((uint16_t)(g_tco + TCO_RLD), 1);
}

static void tco_stop(void) {
    outw((uint16_t)(g_tco + TCO1_CNT), (uint16_t)(inw((uint16_t)(g_tco + TCO1_CNT)) | TCO_TMR_HLT));
    no_reboot(1);
}

static int tco_start(int secs) {
    uint16_t t = (uint16_t)itco_ticks_for(secs);
    uint16_t tmr = inw((uint16_t)(g_tco + TCO_TMR));
    outw((uint16_t)(g_tco + TCO_TMR), (uint16_t)((tmr & ~TMR_MAX) | t));
    if ((inw((uint16_t)(g_tco + TCO_TMR)) & TMR_MAX) != t) return 0;
    if (!no_reboot(0)) return 0;
    tco_ping();
    outw((uint16_t)(g_tco + TCO1_CNT), (uint16_t)(inw((uint16_t)(g_tco + TCO1_CNT)) & ~TCO_TMR_HLT));
    return !(inw((uint16_t)(g_tco + TCO1_CNT)) & TCO_TMR_HLT);
}

static const struct hw_watchdog g_itco = {
    .name = "itco",
    .min_secs = (TMR_MIN * 2 * TICK_MS + 999) / 1000,
    .max_secs = TMR_MAX * 2 * TICK_MS / 1000,
    .start = tco_start, .ping = tco_ping, .stop = tco_stop,
};

static int itco_probe(const struct pci_device *d) {
    if (!tco_v2_part(d->device_id))
        return pci_probe_decline(d, "not a TCO v2 chipset (%04x)", d->device_id);
    if (g_tco) return pci_probe_decline(d, "a second LPC bridge; one TCO is driven");

    uint32_t abase = pci_config_read32(d, LPC_ABASE) & 0xFF80u;
    if (!abase || !(pci_config_read8(d, LPC_ACPI_CNTL) & ACPI_EN))
        return pci_probe_decline(d, "the ACPI I/O range is not decoded");
    uint32_t rcba = pci_config_read32(d, LPC_RCBA);
    if (!(rcba & RCBA_EN)) return pci_probe_decline(d, "no root complex base (RCBA off)");

    g_tco = (uint16_t)(abase + 0x60);
    g_gcs = (volatile uint32_t *)(uintptr_t)((rcba & 0xFFFFC000u) + GCS_OFFSET);
    uint16_t cnt = inw((uint16_t)(g_tco + TCO1_CNT));
    if (cnt == 0xFFFF) {
        g_tco = 0;
        return pci_probe_decline(d, "nothing answers at TCOBASE");
    }

    // THE LAST RESET'S CAUSE survives in the resume well: said once,
    // then cleared, so the next boot reports only its own.
    // Not on the ASUS, whose firmware resets at the FIRST expiry and
    // clears both status bits (docs/boot-flags.md, `watchdog=`).
    uint16_t sts2 = inw((uint16_t)(g_tco + TCO2_STS));
    if (sts2 & TCO2_SECOND_TO)
        klog_write(KLOG_WARN "itco: THE LAST RESET WAS THIS WATCHDOG'S -- the machine "
                   "stopped being fed\n");
    outw((uint16_t)(g_tco + TCO2_STS), TCO2_SECOND_TO | TCO2_BOOT_STS);
    outw((uint16_t)(g_tco + TCO1_STS), TCO1_TIMEOUT);

    int was_running = !(cnt & TCO_TMR_HLT);
    tco_stop();
    // NO_REBOOT must be clearable, or the timer can count but never reset.
    if (!no_reboot(0)) {
        no_reboot(1);
        return pci_probe_decline(d, "NO_REBOOT is locked by the firmware -- it could never reset");
    }
    no_reboot(1);

    klog_printf("itco: TCO v2 at port 0x%x, GCS at 0x%lx%s%s\n", g_tco,
                (uint64_t)(uintptr_t)g_gcs, was_running ? "; the firmware left it RUNNING -- halted" : "",
                (cnt & TCO_LOCK) ? "; TCO_LOCK set" : "");
    watchdog_register(&g_itco);
    return 0;
}

static const struct pci_match itco_matches[] = {
    { 0x8086, PCI_ANY, 0x06, 0x01, PCI_ANY },
};

PCI_DRIVER("itco", itco_matches, itco_probe);

KTEST("itco", "a timeout is two counts of 0.6 s, clamped to what TCO_TMR holds") {
    KTEST_ASSERT_EQ(itco_ticks_for(60), 50);
    KTEST_ASSERT_EQ(itco_ticks_for(1), TMR_MIN);
    KTEST_ASSERT_EQ(itco_ticks_for(100000), TMR_MAX);
    KTEST_ASSERT(g_itco.min_secs * 1000 >= TMR_MIN * 2 * TICK_MS);
}
