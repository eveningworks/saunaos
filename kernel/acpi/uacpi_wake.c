// The wake set and the runtime GPEs, through uACPI -- Linux's
// drivers/acpi/wakeup.c and the end of acpi_scan_init(): every device's
// _PRW read, its GPE marked for wake, then every OTHER GPE with an
// _Lxx/_Exx method enabled, so the firmware's own events run.
//
// WHAT IS ARMED FOR WAKE. A _PRW says a device CAN wake the machine;
// Linux enables few of them by default (the buttons and the lid), and the
// rest wait for a person in /proc/acpi/wakeup. So: a power button, sleep
// button or lid whose _PRW reaches S5 is enabled for wake; every other
// row is listed. uacpi_prepare_for_sleep_state() then masks every GPE and
// enables the wake ones -- the S5 write's whole wake set.
//
// THE RUNTIME HALF RUNS FIRMWARE ON EVERY EVENT (_L6D on a USB PME, _L69
// on a PCIe hotplug), dispatched by uACPI as work on kacpid.
// `acpi_mask_gpe=0x6d,0x69` masks named ones -- Linux's word, which
// exists for a GPE that storms -- and `nogpe` keeps them all off.
#include <uacpi/event.h>
#include <uacpi/namespace.h>
#include <uacpi/status.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

#include "acpi.h"
#include "heap.h"
#include "kfmt.h"
#include "klog.h"
#include "knum.h"
#include "multiboot.h"
#include "query.h"
#include "spinlock.h"
#include "string.h"
#include "uacpi_host.h"

static struct spinlock g_lock;      // g_rows, g_n
static struct query_acpi_wake *g_rows;
static int g_n;

int acpi_wake_get(int index, struct query_acpi_wake *out) {
    uint64_t f = spin_lock_irqsave(&g_lock);
    int ok = index >= 0 && index < g_n;
    if (ok) *out = g_rows[index];
    spin_unlock_irqrestore(&g_lock, f);
    return ok;
}

// Buttons and the lid: what Linux's button driver enables for wake.
static int wakes_by_default(const char *hid) {
    return !k_strcmp(hid, "PNP0C0C") || !k_strcmp(hid, "PNP0C0D") || !k_strcmp(hid, "PNP0C0E");
}

// _PRW's package: [GPE, deepest state, power resources...], the GPE an
// Integer for \_GPE's block or a [block device, index] Package.
static void read_prw(uacpi_namespace_node *dev, struct query_acpi_wake *r) {
    uacpi_object *obj = UACPI_NULL;
    if (uacpi_eval_simple_typed(dev, "_PRW", UACPI_OBJECT_PACKAGE_BIT, &obj) != UACPI_STATUS_OK) {
        r->flags |= QUERY_WAKE_UNREAD;
        return;
    }
    uacpi_object_array arr;
    uacpi_u64 gpe = 0, state = 0;
    if (uacpi_object_get_package(obj, &arr) != UACPI_STATUS_OK || arr.count < 2 ||
        uacpi_object_get_integer(arr.objects[1], &state) != UACPI_STATUS_OK) {
        r->flags |= QUERY_WAKE_UNREAD;
    } else if (uacpi_object_get_integer(arr.objects[0], &gpe) == UACPI_STATUS_OK) {
        r->gpe = gpe;
        r->sleep_state = state;
    } else {
        // A GPE block device's event: listed, never set up -- no machine
        // here has one, and naming its device takes resolving a reference.
        r->flags |= QUERY_WAKE_GPE_BLOCK;
        r->sleep_state = state;
    }
    uacpi_object_unref(obj);
}

// `acpi_mask_gpe=0x6d,0x69`: each named GPE of \_GPE masked.
static int mask_named(void) {
    char v[96];
    if (!multiboot_cmdline_value("acpi_mask_gpe=", v, sizeof v)) return 0;
    int masked = 0;
    char *p = v;
    while (*p) {
        char *e = p;
        while (*e && *e != ',') e++;
        char save = *e;
        *e = 0;
        uint64_t g;
        if (k_parse_hex(p, &g) && g <= 0xFFFF && uacpi_mask_gpe(UACPI_NULL, (uacpi_u16)g) == UACPI_STATUS_OK) {
            klog_printf("acpi: GPE 0x%llx masked (acpi_mask_gpe=)\n", (unsigned long long)g);
            masked++;
        } else {
            klog_printf("acpi: acpi_mask_gpe= -- '%s' is not a GPE this machine has\n", p);
        }
        if (!save) break;
        p = e + 1;
    }
    return masked;
}

void acpi_wake_probe(void) {
    int nd;
    void **devs = acpi_uacpi_nodes(UACPI_OBJECT_DEVICE_BIT, &nd);
    int n = 0;
    for (int i = 0; i < nd; i++) {
        uacpi_namespace_node *prw = UACPI_NULL;
        if (uacpi_namespace_node_find(devs[i], "_PRW", &prw) == UACPI_STATUS_OK && prw) devs[n++] = devs[i];
    }
    struct query_acpi_wake *rows = n ? kzalloc(sizeof *rows * (unsigned)n) : 0;
    if (n && !rows) {
        klog_write("acpi: no memory for the wake set -- no GPE is marked for wake\n");
        n = 0;
    }
    int marked = 0, armed = 0, s5 = 0;
    for (int i = 0; i < n; i++) {
        uacpi_namespace_node *dev = devs[i];
        struct query_acpi_wake *r = &rows[i];
        const char *path = uacpi_namespace_node_generate_absolute_path(dev);
        k_strlcpy(r->path, path ? path : "?", sizeof r->path);
        if (path) uacpi_free_absolute_path(path);
        uacpi_id_string *hid = UACPI_NULL;
        if (uacpi_eval_hid(dev, &hid) == UACPI_STATUS_OK && hid) {
            k_strlcpy(r->hid, hid->value, sizeof r->hid);
            uacpi_free_id_string(hid);
        }
        read_prw(dev, r);
        if (r->flags & (QUERY_WAKE_UNREAD | QUERY_WAKE_GPE_BLOCK)) continue;
        if (r->sleep_state >= 5) s5++;
        uacpi_status st = uacpi_setup_gpe_for_wake(UACPI_NULL, (uacpi_u16)r->gpe, dev);
        if (st != UACPI_STATUS_OK) {
            klog_printf("acpi: %s -- its wake GPE 0x%llx was refused: %s\n", r->path,
                        (unsigned long long)r->gpe, uacpi_status_to_string(st));
            continue;
        }
        marked++;
        if (r->sleep_state >= 5 && wakes_by_default(r->hid) &&
            uacpi_enable_gpe_for_wake(UACPI_NULL, (uacpi_u16)r->gpe) == UACPI_STATUS_OK) {
            r->flags |= QUERY_WAKE_ARMED_S5;
            armed++;
        }
    }
    kfree(devs);
    uint64_t f = spin_lock_irqsave(&g_lock);
    g_rows = rows;
    g_n = n;
    spin_unlock_irqrestore(&g_lock, f);
    klog_printf("acpi: %d device(s) can wake the machine, %d from S5; %d armed for wake at power-off\n",
                marked, s5, armed);

    if (multiboot_cmdline_has("nogpe")) {
        klog_write("acpi: runtime GPEs left off (nogpe)\n");
        return;
    }
    uacpi_status st = uacpi_finalize_gpe_initialization();
    if (st != UACPI_STATUS_OK) {
        klog_printf("acpi: the runtime GPEs were not enabled: %s\n", uacpi_status_to_string(st));
        return;
    }
    int masked = mask_named();
    klog_printf("acpi: runtime GPEs enabled -- the firmware's _Lxx/_Exx now run%s\n",
                masked ? ", the masked ones aside" : "");
}

// After uacpi_state_reset(), which freed the nodes.
void acpi_wake_reset(void) {
    uint64_t f = spin_lock_irqsave(&g_lock);
    struct query_acpi_wake *r = g_rows;
    g_rows = 0;
    g_n = 0;
    spin_unlock_irqrestore(&g_lock, f);
    kfree(r);
}
