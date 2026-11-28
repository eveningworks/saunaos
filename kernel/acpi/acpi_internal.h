#ifndef ACPI_INTERNAL_H
#define ACPI_INTERNAL_H

// Shared between the files of kernel/acpi/ and nowhere else. The split
// is by TABLE -- discovery, the FADT, the MADT, the facts -- but it is
// still one component, so the state is one struct reached through here
// rather than an interface between four (CLAUDE.md's rule for a split
// like userland/apps/system/toywm/'s).
#include "acpi.h"

struct acpi_state *acpi_state_mut(void);

// Can this physical range be read through boot.asm's identity map?
int acpi_phys_readable(uint64_t phys, uint32_t len);
const void *acpi_phys(uint64_t phys);

void acpi_fadt_init(void);
void acpi_madt_init(void);

// acpi_power.c: moving between legacy and ACPI mode through SMI_CMD.
// Each returns 1 when the machine ends in the mode asked for.
int acpi_enter_mode(struct acpi_state *s);
int acpi_leave_mode(struct acpi_state *s);
// The same, leaving the SCI's line unmasked -- for a running machine,
// where the line may be shared (acpi_sci_ec_unserved()).
int acpi_leave_mode_line(struct acpi_state *s, int mask_line);
// Every GPE in a block masked and its status cleared (acpi_power.c).
void acpi_gpe_block_off(uint32_t base, uint8_t len);
// Is `acpimode=<want>` on the command line?
int acpi_mode_word(const char *want);

// acpi_sci.c: a press of the fixed power button, from whichever handler
// owns it -- counted, and SIGPWR to init. Interrupt-safe.
void acpi_power_button_pressed(const char *via);
// uACPI's fixed-event handler, under g_sci_lock: a press to hand to
// acpi_power_button_pressed() once the SCI's dispatch has returned.
void acpi_power_button_note(void);
// 1: uACPI's fixed-event handler owns the button and acpi_sci.c's SCI
// handler stops acking it; 0: acpi_sci.c's again (uacpi_power.c).
void acpi_sci_button_to_uacpi(int to_uacpi);
// For the KTEST: who owns the button, and is PM1a's PWRBTN_EN set?
int acpi_sci_button_is_uacpi(void);
int acpi_sci_button_enabled(void);

// uacpi_power.c: S5 after the firmware's _PTS(5), when kacpid's
// namespace is READY and acpi_uacpi_decide_s5() chose it (ACPI_F_S5_UACPI).
// Returns only when the machine stayed up; the ladder in acpi_poweroff()
// follows. Hardware-reduced firmware gets _PTS here and the write there.
void acpi_uacpi_poweroff(void);

#endif
