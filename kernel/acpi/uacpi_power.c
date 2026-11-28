// Power through uACPI (docs/aml-design.md, stage 4c): the fixed power
// button as uACPI's fixed event, and S5 entered after the firmware's own
// `_PTS(5)` -- the preparation whose absence made the ASUS need two
// presses to start after an ACPI-mode shutdown (docs/decisions.md).
//
// LINUX'S SPLIT, KEPT: acpi_power_off_prepare() runs `_PTS`/`_SST` with
// interrupts on, after the devices are shut down, because AML may reach
// the embedded controller and sleep; acpi_power_off() then writes the
// sleep registers with interrupts off. Here the first half is
// uacpi_prepare_for_sleep_state() in system_poweroff()'s own syscall,
// and uACPI's uacpi_enter_sleep_state() turns interrupts off itself.
//
// acpi_poweroff()'s ladder stays the fallback: for a namespace that is
// not READY, a `_PTS` that failed, an `acpimode=` experiment (they are
// the ladder's), and a sleep write that returned.
#include <uacpi/uacpi.h>
#include <uacpi/event.h>
#include <uacpi/namespace.h>
#include <uacpi/sleep.h>
#include <uacpi/status.h>

#include "acpi.h"
#include "acpi_internal.h"
#include "klog.h"
#include "kfmt.h"
#include "uacpi_host.h"

static int g_button;   // uACPI holds the fixed power button's handler

// Runs under uacpi_glue.c's g_sci_lock, so the press is only NOTED:
// acpi_sci.c sends SIGPWR once the dispatch has returned, keeping the
// signal's locks out from under the SCI's.
static uacpi_interrupt_ret power_button(uacpi_handle ctx) {
    (void)ctx;
    acpi_power_button_note();
    return UACPI_INTERRUPT_HANDLED;
}

// THE ORDER IS THE INVARIANT: uACPI's handler goes in BEFORE acpi_sci.c
// stops looking, so a press in between is acked by one of the two. The
// other way round, uACPI's dispatch would see an enabled event with no
// handler and DISABLE the button for the rest of the boot.
void acpi_uacpi_take_power_button(void) {
    const struct acpi_state *s = acpi_get_state();
    if (!(s->flags & ACPI_F_PWRBTN)) return;   // never armed, or handed back with the EC
    uacpi_status st = uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_POWER_BUTTON,
                                                        power_button, UACPI_NULL);
    if (st != UACPI_STATUS_OK) {
        klog_printf("uacpi: the power button stays acpi_sci.c's -- the handler was refused: %s\n",
                    uacpi_status_to_string(st));
        return;
    }
    g_button = 1;
    acpi_sci_button_to_uacpi(1);
    klog_write("uacpi: the power button is uACPI's fixed event now\n");
}

// The reverse, for a teardown: acpi_sci.c looks again FIRST, for the
// same reason; uacpi_state_reset() then drops uACPI's handler.
void acpi_uacpi_give_back_power_button(void) {
    if (!g_button) return;
    acpi_sci_button_to_uacpi(0);
    g_button = 0;
}

// Decided once the namespace is up, so `acpi` can say which S5 runs: a
// `\_S5` to read the sleep types from, and no `acpimode=` experiment.
void acpi_uacpi_decide_s5(void) {
    uacpi_namespace_node *n = 0;
    if (uacpi_namespace_node_find(UACPI_NULL, "\\_S5", &n) != UACPI_STATUS_OK || !n) {
        klog_write("uacpi: no \\_S5 in the namespace -- S5 stays the ladder's\n");
        return;
    }
    if (acpi_mode_word("poweroff") || acpi_mode_word("never")) {
        klog_write("uacpi: S5 left to the ladder for an acpimode= experiment\n");
        return;
    }
    __atomic_or_fetch(&acpi_state_mut()->flags, ACPI_F_S5_UACPI, __ATOMIC_RELEASE);
}

void acpi_uacpi_forget_s5(void) {
    __atomic_and_fetch(&acpi_state_mut()->flags, ~(uint32_t)ACPI_F_S5_UACPI, __ATOMIC_RELEASE);
}

void acpi_uacpi_poweroff(void) {
    struct acpi_uacpi_info i;
    acpi_uacpi_info(&i);
    if (i.state != ACPI_UACPI_READY || acpi_uacpi_is_inline()) return;
    const struct acpi_state *s = acpi_get_state();
    if (!(s->flags & ACPI_F_S5_UACPI)) return;
    uacpi_status st = uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5);
    if (st != UACPI_STATUS_OK) {
        if (s->flags & ACPI_F_S5) {
            klog_printf("uacpi: _PTS(5) failed: %s -- the ladder's S5 instead\n",
                        uacpi_status_to_string(st));
            return;
        }
        // No byte-scan _S5_ to fall back on: S5 anyway, as Linux goes on
        // past a failed _PTS. uACPI refuses the write itself if it never
        // read \_S5's types.
        klog_printf("uacpi: _PTS(5) failed: %s -- S5 through uACPI anyway\n",
                    uacpi_status_to_string(st));
    } else {
        klog_write("uacpi: _PTS(5) done -- S5 in ACPI mode\n");
    }
    // THE TRAP: uACPI's hardware-reduced S5 polls WAK_STS with no bound,
    // interrupts off, so a write that does not take never returns. The
    // ladder's SLEEP_CONTROL_REG write is bounded; _PTS has run.
    if (s->flags & ACPI_F_HW_REDUCED) {
        klog_write("uacpi: hardware-reduced -- S5 by the ladder's bounded write\n");
        return;
    }
    st = uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);
    klog_printf("uacpi: S5 through uACPI returned (%s) -- the ladder's S5 next\n",
                uacpi_status_to_string(st));
}
