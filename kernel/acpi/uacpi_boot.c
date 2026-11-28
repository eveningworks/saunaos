// Bringing uACPI up: `kacpid`, behind `acpi=uacpi` (docs/aml-design.md,
// stage 4: the namespace, the embedded controller, the power button and
// S5).
//
// WHY A THREAD AND NOT acpi_init(). acpi_init() runs before the frame
// allocator and the heap, and the kernel context cannot park -- while
// uACPI allocates on every step and waits on semaphores, and AML may
// Sleep(). Linux brings ACPICA's tables up early and its interpreter at
// a later initcall, then hands deferred work to the kacpid workqueue;
// this is that shape. Boot does not wait: nothing consumes the
// namespace yet, and a later consumer (stage 4d's _PRT) waits on READY.
//
// THE TRAP: a kernel thread runs on core 0 with interrupts OFF and is
// never preempted, so a step holds that core until it ends -- tens of
// ms on QEMU, and a firmware loop may spin to uACPI's 30-second loop
// timeout. kacpid gives the core up between steps (core_yield()); with
// one core, the rest of boot waits for each step.
//
// SaunaOS keeps the machine's mode: UACPI_FLAG_NO_ACPI_MODE, because
// acpi_sci.c decides ACPI vs legacy mode -- and with `acpi=uacpi` it
// enters ACPI mode on an EC machine too, since uacpi_ec.c serves the EC.
#include <uacpi/uacpi.h>
#include <uacpi/namespace.h>
#include <uacpi/status.h>

#include "acpi.h"
#include "clocksource.h"
#include "heap.h"
#include "klog.h"
#include "kfmt.h"
#include "kthread.h"
#include "multiboot.h"
#include "scheduler.h"   // scheduler_current_pid()
#include "string.h"
#include "uacpi_host.h"

static struct acpi_uacpi_info g_info;   // written by whoever owns the bring-up

void acpi_uacpi_info(struct acpi_uacpi_info *out) {
    *out = g_info;
}

static uacpi_iteration_decision count_node(void *user, uacpi_namespace_node *node, uacpi_u32 depth) {
    (void)node;
    (void)depth;
    (*(uint32_t *)user)++;
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static int g_inline;   // the KTEST's instance is the one up

struct node_list { void **nodes; int n, cap; };

static uacpi_iteration_decision collect_node(void *user, uacpi_namespace_node *node, uacpi_u32 depth) {
    (void)depth;
    struct node_list *l = user;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 8;
        void **grown = kmalloc(sizeof *grown * (unsigned)cap);
        if (!grown) return UACPI_ITERATION_DECISION_BREAK;
        if (l->n) k_memcpy(grown, l->nodes, sizeof *grown * (unsigned)l->n);
        kfree(l->nodes);
        l->nodes = grown;
        l->cap = cap;
    }
    l->nodes[l->n++] = node;
    return UACPI_ITERATION_DECISION_CONTINUE;
}

void **acpi_uacpi_nodes(uint32_t type_mask, int *n) {
    struct node_list l = { 0, 0, 0 };
    uacpi_namespace_for_each_child(uacpi_namespace_root(), collect_node, UACPI_NULL,
                                   (uacpi_object_type_bits)type_mask, UACPI_MAX_DEPTH_ANY, &l);
    *n = l.n;
    return l.nodes;
}

// Let whatever else is ready on this core run. A no-op where nothing
// can park (the KTEST's inline instance).
static void core_yield(void) {
    static char chan;
    scheduler_block_kernel_until(&chan, SCHED_WAIT_TIMER, clocksource_now_ns() + 1);
}

static int fail(const char *step, uacpi_status st) {
    g_info.failed_at = step;
    g_info.status = (int32_t)st;
    __atomic_store_n(&g_info.state, ACPI_UACPI_FAILED, __ATOMIC_RELEASE);
    klog_printf("uacpi: %s failed: %s -- the interpreter is off for this boot\n",
                step, uacpi_status_to_string(st));
    acpi_ec_reset();   // an ECDT handler may be in; nothing serves it now
    acpi_sci_rearm();
    acpi_sci_ec_unserved("the AML interpreter failed");
    return 0;
}

// initialize -> load (every DSDT/SSDT's top-level code; installs the
// GPE blocks, holds the SCI) -> the ECDT's embedded controller -> namespace
// init (_STA, _INI, _REG) -> the EC device and its GPE (uacpi_ec.c) ->
// the power button (uacpi_power.c) -> the supplies, and (not for the
// KTEST's instance) the thermal zones and the wake set, whose end enables
// the runtime GPEs (uacpi_wake.c).
static int bring_up(void) {
    uint64_t t0 = clocksource_now_ns();
    uacpi_status st = uacpi_initialize(UACPI_FLAG_NO_ACPI_MODE);
    if (uacpi_unlikely_error(st)) return fail("initialize", st);
    acpi_sci_rearm();   // uACPI disabled every fixed event just now
    core_yield();
    st = uacpi_namespace_load();
    if (uacpi_unlikely_error(st)) return fail("load", st);
    acpi_ec_probe_ecdt();
    core_yield();
    st = uacpi_namespace_initialize();
    if (uacpi_unlikely_error(st)) return fail("namespace init", st);
    acpi_ec_probe_namespace();
    acpi_uacpi_take_power_button();
    acpi_uacpi_decide_s5();
    acpi_supply_probe();
    if (!g_inline) {
        // Not for the KTEST's instance: it has no kacpid to run a GPE's
        // method, and a passive limit or a critical trip there would act
        // on the machine running the suite.
        acpi_thermal_probe();
        acpi_wake_probe();
    }

    uint32_t nodes = 0, devices = 0;
    uacpi_namespace_for_each_child(uacpi_namespace_root(), count_node, UACPI_NULL,
                                   UACPI_OBJECT_ANY_BIT, UACPI_MAX_DEPTH_ANY, &nodes);
    uacpi_namespace_for_each_child(uacpi_namespace_root(), count_node, UACPI_NULL,
                                   UACPI_OBJECT_DEVICE_BIT, UACPI_MAX_DEPTH_ANY, &devices);
    g_info.nodes = nodes;
    g_info.devices = devices;
    g_info.load_us = (clocksource_now_ns() - t0) / 1000;
    __atomic_store_n(&g_info.state, ACPI_UACPI_READY, __ATOMIC_RELEASE);
    klog_printf("uacpi: namespace ready -- %u nodes, %u devices, %llu ms\n",
                (unsigned)nodes, (unsigned)devices,
                (unsigned long long)(g_info.load_us / 1000));
    return 1;
}

static void kacpid(void *arg) {
    (void)arg;
    g_uacpi_kacpid_pid = scheduler_current_pid();
    if (!bring_up()) {
        g_uacpi_kacpid_pid = 0;
        return;   // the thread ends; nothing queued work on a failed load
    }
    for (;;) {
        uint64_t a = acpi_supply_deadline(), b = acpi_thermal_deadline();
        uacpi_host_wait_for_work(!a ? b : !b ? a : a < b ? a : b);
        uacpi_host_drain_work();
        acpi_supply_poll();
        acpi_thermal_poll();
    }
}

void acpi_uacpi_start(void) {
    if (!multiboot_cmdline_has("acpi=uacpi")) return;
    if (!(acpi_get_state()->flags & ACPI_F_TABLES)) {
        klog_write("uacpi: acpi=uacpi, but this machine has no ACPI tables\n");
        return;
    }
    __atomic_store_n(&g_info.state, ACPI_UACPI_LOADING, __ATOMIC_RELEASE);
    if (kthread_start("kacpid", kacpid, 0) <= 0) {
        g_info.state = ACPI_UACPI_OFF;
        klog_write("uacpi: no slot for kacpid -- the interpreter is off for this boot\n");
        acpi_sci_ec_unserved("the AML interpreter could not start");
    }
}

// --- the KTEST's own instance ------------------------------------------


int acpi_uacpi_is_inline(void) {
    return g_inline;
}

int acpi_uacpi_bring_up_inline(void) {
    uint32_t off = ACPI_UACPI_OFF;
    if (!__atomic_compare_exchange_n(&g_info.state, &off, ACPI_UACPI_LOADING, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return 0;
    g_inline = 1;
    return bring_up();
}

void acpi_uacpi_tear_down_inline(void) {
    if (!g_inline) return;
    // Forgotten BEFORE g_inline drops, so a power-off in between never
    // picks a namespace uacpi_state_reset() is freeing.
    acpi_uacpi_give_back_power_button();
    acpi_uacpi_forget_s5();
    g_inline = 0;
    uacpi_state_reset();
    acpi_supply_reset();
    acpi_thermal_reset();
    acpi_wake_reset();
    acpi_ec_reset();
    uacpi_host_reset();
    k_memset(&g_info, 0, sizeof g_info);   // state back to OFF
    acpi_sci_rearm();
}
