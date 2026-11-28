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

// Park kacpid until work is queued (it returns at once if some is) or
// `deadline_ns` (clocksource_now_ns(); 0 = none) passes.
void uacpi_host_wait_for_work(uint64_t deadline_ns);

// The SCI handler uACPI asked for, HELD rather than installed:
// acpi_sci.c owns the line (docs/aml-design.md, stage 4a) until 4d, and
// its handler calls this one -- after taking the power button itself,
// unless uACPI's fixed event has it (stage 4c). 1 when a
// handler is held (and the line it asked for).
int uacpi_host_sci_held(uint32_t *irq);

// Run the held handler, if any. From acpi_sci.c's interrupt handler;
// interrupt-safe. Returns uACPI's UACPI_INTERRUPT_HANDLED bit, 0 if none.
uint32_t uacpi_host_sci_dispatch(void);

// The embedded controller (uacpi_ec.c), in bring_up()'s order: the ECDT's
// after the namespace is loaded and before _INI runs, the device's (and
// the events) after; the reset after uacpi_state_reset().
void acpi_ec_probe_ecdt(void);
void acpi_ec_probe_namespace(void);
void acpi_ec_reset(void);

// The fixed power button (uacpi_power.c): taken from acpi_sci.c once
// the namespace is up, given back before a teardown.
void acpi_uacpi_take_power_button(void);
void acpi_uacpi_give_back_power_button(void);

// Whether uACPI's S5 will run (ACPI_F_S5_UACPI): decided after the
// namespace is up, forgotten at a teardown (uacpi_power.c).
void acpi_uacpi_decide_s5(void);
void acpi_uacpi_forget_s5(void);

// Batteries, the adapter and the lid (uacpi_supply.c): found once the
// namespace is up, polled when kacpid's wait reaches the deadline
// (0: none), forgotten after a teardown.
void acpi_supply_probe(void);
uint64_t acpi_supply_deadline(void);
void acpi_supply_poll(void);
void acpi_supply_reset(void);

// Thermal zones (uacpi_thermal.c): as the supplies, and kacpid waits for
// the earlier of the two deadlines. Not probed in the KTEST's instance.
void acpi_thermal_probe(void);
uint64_t acpi_thermal_deadline(void);
void acpi_thermal_poll(void);
void acpi_thermal_reset(void);

// The _PRW wake set and the runtime GPEs (uacpi_wake.c): once, at the end
// of bring_up(); forgotten after a teardown. Not in the KTEST's instance.
void acpi_wake_probe(void);
void acpi_wake_reset(void);

// Every node whose type is in `type_mask` (UACPI_OBJECT_*_BIT), in
// namespace order: uacpi_namespace_node pointers, in an array the caller
// kfree()s; *n its length (0 and a null array for none, or no memory).
void **acpi_uacpi_nodes(uint32_t type_mask, int *n);

// 1 while the KTEST's inline instance is the one up (uacpi_boot.c).
int acpi_uacpi_is_inline(void);

// Forget every handler and queued item -- after uacpi_state_reset(),
// which has already freed what they pointed at.
void uacpi_host_reset(void);

#endif
