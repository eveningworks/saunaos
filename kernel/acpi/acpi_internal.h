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
// Every GPE in a block masked and its status cleared (acpi_power.c).
void acpi_gpe_block_off(uint32_t base, uint8_t len);
// Is `acpimode=<want>` on the command line?
int acpi_mode_word(const char *want);

#endif
