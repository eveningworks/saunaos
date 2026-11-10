// ACPI mode at boot, the SCI, and the one event it carries here: the
// FIXED power button (PM1's PWRBTN_STS). A press sends SIGPWR to init,
// which shuts the machine down in order -- logind's
// `HandlePowerKey=poweroff`, reached without an input layer between.
//
// THE OTHER EVENTS ARE MASKED, NOT IGNORED. The SCI is a level-triggered
// line: a status bit set behind a set enable holds it asserted, and a
// handler that does not clear it is an interrupt storm. Every GPE needs
// AML (`_Lxx`/`_Exx`, the embedded controller's `_Qxx`) to service, and
// there is no interpreter here, so every GPE stays masked -- what
// Linux's acpi_hw_disable_all_gpes() does before it enables the ones it
// has handlers for -- and PM1_EN carries PWRBTN_EN alone.
//
// A CONTROL-METHOD button (FADT PWR_BUTTON set: a PNP0C0C device that
// raises a GPE and `Notify`s) is AML too, and is logged, not armed.
#include "acpi.h"
#include "acpi_internal.h"
#include "io.h"
#include "irq.h"
#include "klog.h"
#include "kfmt.h"
#include "signal.h"       // signal_send() -- safe from an interrupt for SIGPWR
#include "signal_abi.h"
#include "scheduler.h"    // scheduler_init_pid()

// PM1 status/enable bits (ACPI 6.4 tables 4.11 and 4.12). The status
// half is write-1-to-clear.
#define PM1_PWRBTN (1u << 8)

// The block is [STS][EN], each half PM1_EVT_LEN/2 bytes; both registers
// are 16 bits at the start of their half.
static uint16_t pm1_half(const struct acpi_state *s) {
    return s->pm1_evt_len >= 4 ? (uint16_t)(s->pm1_evt_len / 2) : 2;
}

static uint16_t pm1_sts(const struct acpi_state *s) {
    uint16_t v = inw((uint16_t)s->pm1a_evt);
    if (s->pm1b_evt) v |= inw((uint16_t)s->pm1b_evt);
    return v;
}

static void pm1_ack(const struct acpi_state *s, uint16_t bits) {
    outw((uint16_t)s->pm1a_evt, bits);
    if (s->pm1b_evt) outw((uint16_t)s->pm1b_evt, bits);
}

static void pm1_enable(const struct acpi_state *s, uint16_t bits) {
    outw((uint16_t)(s->pm1a_evt + pm1_half(s)), bits);
    if (s->pm1b_evt) outw((uint16_t)(s->pm1b_evt + pm1_half(s)), bits);
}

// A SHARED line, so a call with no PWRBTN_STS is somebody else's and
// returns without touching anything.
static void sci_irq(uint64_t *regs) {
    (void)regs;
    struct acpi_state *s = acpi_state_mut();
    if (!(pm1_sts(s) & PM1_PWRBTN)) return;
    pm1_ack(s, PM1_PWRBTN);
    s->pwrbtn_presses++;

    int init = scheduler_init_pid();
    klog_printf("acpi: power button pressed -- SIGPWR to init (pid %d)\n", init);
    if (init <= 0 || !signal_send(init, SIGPWR))
        klog_write("acpi: no init to tell -- the press is dropped\n");
}

void acpi_sci_init(void) {
    struct acpi_state *s = acpi_state_mut();
    if (!(s->flags & ACPI_F_FADT) || (s->flags & ACPI_F_HW_REDUCED) || !s->pm1a_evt) return;
    if (acpi_mode_word("legacy")) {
        klog_write("acpi: staying in legacy mode (acpimode=legacy) -- no power button\n");
        return;
    }
    if (s->flags & ACPI_F_PWRBTN_CM) {
        klog_write("acpi: the power button is a control-method device -- needs AML, not armed\n");
        return;
    }
    if (s->sci_int >= IRQ_MAX) {
        klog_printf("acpi: SCI on GSI %u, past the lines this kernel routes -- no power button\n",
                    (unsigned)s->sci_int);
        return;
    }

    if (!acpi_enter_mode(s)) {
        klog_write("acpi: could not enter ACPI mode -- no power button\n");
        return;
    }

    // Quiet first, then armed, then routed: nothing may be pending
    // behind an enable when the line opens.
    acpi_gpe_block_off(s->gpe0_blk, s->gpe0_len);
    acpi_gpe_block_off(s->gpe1_blk, s->gpe1_len);
    pm1_enable(s, 0);
    pm1_ack(s, 0xFFFF);
    pm1_enable(s, PM1_PWRBTN);

    uint8_t line = (uint8_t)s->sci_int;
    // ACPI 6.4 5.2.9: a GSI is level, active low; an ISA line takes its
    // MADT override (or the ELCR) inside irq_unmask().
    if (line >= 16) irq_set_trigger(line, 1, 1);
    irq_register_handler(line, sci_irq);
    int rc = irq_unmask(line);
    if (rc) {
        irq_unregister_handler(line, sci_irq);
        pm1_enable(s, 0);
        klog_printf("acpi: SCI line %u could not be routed (%d) -- no power button\n",
                    (unsigned)line, rc);
        return;
    }
    s->flags |= ACPI_F_PWRBTN;
    klog_printf("acpi: ACPI mode on, power button armed on SCI %u%s\n", (unsigned)line,
                (s->flags & ACPI_F_FW_LEGACY) ? " (firmware handed over in legacy mode)" : "");
}
