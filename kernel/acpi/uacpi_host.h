// What uacpi_glue.c (uACPI's callbacks) and uacpi_boot.c (kacpid, the
// state the rest of the kernel reads) share. Internal to kernel/acpi/.
#ifndef UACPI_HOST_H
#define UACPI_HOST_H

#include <stdint.h>

// kacpid's pid, 0 until it runs. Work scheduled before then -- or from
// a caller with no thread to wait on, a KTEST -- drains inline.
extern volatile int g_uacpi_kacpid_pid;

// Run every queued work item, in this context, until none is left.
void uacpi_host_drain_work(void);

// Park kacpid until work is queued (it returns at once if some is).
void uacpi_host_wait_for_work(void);

// The SCI handler uACPI asked for, HELD rather than installed: until
// stage 4c acpi_sci.c owns the line (docs/aml-design.md, stage 4a), and
// its handler calls this one after taking the power button. 1 when a
// handler is held (and the line it asked for).
int uacpi_host_sci_held(uint32_t *irq);

// Run the held handler, if any. From acpi_sci.c's interrupt handler;
// interrupt-safe. Returns uACPI's UACPI_INTERRUPT_HANDLED bit, 0 if none.
uint32_t uacpi_host_sci_dispatch(void);

// Forget every handler and queued item -- after uacpi_state_reset(),
// which has already freed what they pointed at.
void uacpi_host_reset(void);

#endif
