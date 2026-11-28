// The embedded controller, on uACPI (docs/aml-design.md, stage 4b) --
// Linux's drivers/acpi/ec.c in polling mode. Two things the firmware's
// AML needs from the host:
//
//   - the EmbeddedControl ADDRESS SPACE: a Field in the EC's 256 bytes
//     is read and written through RD_EC/WR_EC (ec_proto.c);
//   - its EVENTS: the EC raises a GPE when SCI_EVT is set, QR_EC names
//     the event, and the firmware's `_Qxx` method for that number runs --
//     brightness keys, the lid, the AC adapter, battery changes.
//
// WHERE IT IS FOUND. The ECDT first (ports, GPE, the EC's path), with
// the handler on the namespace ROOT before `_INI` runs, so AML outside
// the EC device can reach it early -- Linux's acpi_ec_ecdt_probe(); else
// the first PNP0C09 device, its `_CRS` giving the data port then the
// command port. Installing the handler runs every EC region's `_REG`
// (the ASUS's sets ECAV, which 46 of its methods test).
//
// EVENTS ONLY IN ACPI MODE: in legacy mode the GPE belongs to the
// firmware's SMI handler, and no SCI would arrive anyway. The GPE handler
// runs in the SCI's interrupt and only reads the status port; QR_EC and
// the `_Qxx` it names run on kacpid. A STORM -- more than EC_STORM events
// in a second -- disables the GPE for the rest of the boot and says so.
#include <uacpi/event.h>
#include <uacpi/kernel_api.h>
#include <uacpi/namespace.h>
#include <uacpi/opregion.h>
#include <uacpi/resources.h>
#include <uacpi/status.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

#include "acpi.h"
#include "acpi_internal.h"
#include "clocksource.h"
#include "ec_proto.h"
#include "heap.h"
#include "io.h"
#include "kfmt.h"
#include "klog.h"
#include "ratelimit.h"
#include "kmutex.h"
#include "scheduler.h"
#include "string.h"
#include "uacpi_host.h"

#define EC_BUSY_NS     1000000ull   // one wait for IBF/OBF spins 1 ms...
#define EC_WAIT_NS   500000000ull   // ...then parks 1 ms between polls, failing at 500 ms
#define EC_NAP_NS      1000000ull
#define EC_QUERY_BATCH 16           // events taken per pass before kacpid looks at other work
#define EC_STORM       100          // GPEs in one second that turn it off

static struct {
    struct ec_io io;
    struct kmutex lock;             // one transaction at a time; held across a nap
    uacpi_namespace_node *node;     // the EC device: _GPE and the _Qxx live here
    uacpi_namespace_node *handler_at;
    uint32_t via;                   // ACPI_EC_VIA_*
    int32_t gpe;                    // -1: none known
    uint32_t events;                // ACPI_EC_EVENTS_*
    uint64_t gpes, queries;
    uint32_t last_query;
    uint32_t qcount[256];           // a query number is a byte
    uint64_t window_start, window_n;
    uacpi_handle work;
    char path[48];
} g_ec;

// Park for a while; where nothing can park (no scheduler slot) the
// caller just polls on.
static void nap(void) {
    static char chan;
    scheduler_block_kernel_until(&chan, SCHED_WAIT_TIMER, clocksource_now_ns() + EC_NAP_NS);
}

static uint8_t port_in(uint16_t p) { return inb(p); }
static void port_out(uint16_t p, uint8_t v) { outb(p, v); }

static void set_ports(uint16_t cmd, uint16_t data) {
    g_ec.io.cmd = cmd;
    g_ec.io.data = data;
    g_ec.io.in = port_in;
    g_ec.io.out = port_out;
    g_ec.io.now_ns = clocksource_now_ns;
    g_ec.io.busy_ns = EC_BUSY_NS;
    g_ec.io.wait_ns = EC_WAIT_NS;
    g_ec.io.nap = nap;
}

// --- the address space ---------------------------------------------------

static uacpi_status ec_region(uacpi_region_op op, uacpi_handle data) {
    if (op == UACPI_REGION_OP_ATTACH || op == UACPI_REGION_OP_DETACH) return UACPI_STATUS_OK;
    if (op != UACPI_REGION_OP_READ && op != UACPI_REGION_OP_WRITE) return UACPI_STATUS_INVALID_ARGUMENT;
    uacpi_region_rw_data *rw = data;
    if (rw->byte_width < 1 || rw->byte_width > 8 || rw->address + rw->byte_width > 256)
        return UACPI_STATUS_INVALID_ARGUMENT;
    // A wider access is that many bytes, lowest address first -- the
    // EC's space is bytes, and Linux's acpi_ec_space_handler() loops too.
    uint64_t v = op == UACPI_REGION_OP_WRITE ? rw->value : 0;
    int bad = 0;
    kmutex_lock(&g_ec.lock);
    for (unsigned i = 0; i < rw->byte_width && !bad; i++) {
        uint8_t addr = (uint8_t)(rw->address + i), b;
        if (op == UACPI_REGION_OP_READ) {
            bad = ec_read(&g_ec.io, addr, &b);
            v |= (uint64_t)b << (8 * i);
        } else {
            bad = ec_write(&g_ec.io, addr, (uint8_t)(v >> (8 * i)));
        }
    }
    kmutex_unlock(&g_ec.lock);
    if (bad) {
        static struct ratelimit rl;
        unsigned held = 0;
        if (ratelimit_ok(&rl, &held))
            klog_printf("ec: %s of byte 0x%02x timed out (%u more held back)\n",
                        op == UACPI_REGION_OP_READ ? "read" : "write", (unsigned)rw->address, held);
        return UACPI_STATUS_HARDWARE_TIMEOUT;
    }
    if (op == UACPI_REGION_OP_READ) rw->value = v;
    return UACPI_STATUS_OK;
}

// --- events ---------------------------------------------------------------

// Counted per GPE, in the interrupt: a GPE that fires with no event
// behind it is a storm as much as a flood of events is. Past the limit
// the GPE is not re-enabled again this boot.
static int storm(void) {
    uint64_t now = clocksource_now_ns();
    if (now - g_ec.window_start >= 1000000000ull) {
        g_ec.window_start = now;
        g_ec.window_n = 0;
    }
    if (++g_ec.window_n <= EC_STORM || g_ec.events != ACPI_EC_EVENTS_ON) return 0;
    g_ec.events = ACPI_EC_EVENTS_STORM;
    klog_printf("ec: over %u GPEs in a second -- GPE 0x%02x off for this boot\n",
                (unsigned)EC_STORM, (unsigned)g_ec.gpe);
    return 1;
}

// On kacpid: take each waiting event and run its method.
static void query_work(uacpi_handle ctx) {
    (void)ctx;
    for (int i = 0; i < EC_QUERY_BATCH && g_ec.events == ACPI_EC_EVENTS_ON; i++) {
        uint8_t q = 0;
        kmutex_lock(&g_ec.lock);
        int bad = ec_query(&g_ec.io, &q);
        kmutex_unlock(&g_ec.lock);
        if (bad || !q) return;   // a failed QR_EC is not retried: the next GPE is
        g_ec.queries++;
        g_ec.qcount[q]++;
        g_ec.last_query = q;
        char name[5];
        k_snprintf(name, sizeof name, "_Q%02X", (unsigned)q);
        uacpi_status st = uacpi_execute_simple(g_ec.node, name);
        static struct ratelimit rl;
        unsigned held = 0;
        if (ratelimit_ok(&rl, &held))
            klog_printf("ec: event 0x%02x -> %s%s (%u more held back)\n", (unsigned)q, name,
                        st == UACPI_STATUS_OK ? "" : st == UACPI_STATUS_NOT_FOUND ? ", no such method"
                                                                                   : ", failed",
                        held);
    }
    // A full batch with SCI_EVT still up: the edge for it was taken at
    // dispatch, so no new one may come -- queue another pass, as Linux
    // keeps going while SCI_EVT is set.
    if (g_ec.events == ACPI_EC_EVENTS_ON && (inb(g_ec.io.cmd) & EC_STS_SCI_EVT))
        uacpi_kernel_schedule_work(UACPI_WORK_GPE_EXECUTION, g_ec.work, query_work, UACPI_NULL);
}

// In the SCI's interrupt: the status read has no side effect, so it may
// race a transaction on kacpid. SCI_EVT stays set until QR_EC; a pass
// that ends with it still set queues the next one itself.
static uacpi_interrupt_ret ec_gpe(uacpi_handle ctx, uacpi_namespace_node *dev, uacpi_u16 idx) {
    (void)ctx; (void)dev; (void)idx;
    g_ec.gpes++;
    if (storm()) return UACPI_INTERRUPT_HANDLED;
    if (inb(g_ec.io.cmd) & EC_STS_SCI_EVT)
        uacpi_kernel_schedule_work(UACPI_WORK_GPE_EXECUTION, g_ec.work, query_work, UACPI_NULL);
    return UACPI_INTERRUPT_HANDLED | (g_ec.events == ACPI_EC_EVENTS_ON ? UACPI_GPE_REENABLE : 0);
}

// --- finding it -----------------------------------------------------------

static int install(uacpi_namespace_node *at) {
    uacpi_status st = uacpi_install_address_space_handler(at, UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER,
                                                          ec_region, UACPI_NULL);
    if (st != UACPI_STATUS_OK) {
        klog_printf("ec: the address-space handler was refused: %s\n", uacpi_status_to_string(st));
        return 0;
    }
    g_ec.handler_at = at;
    return 1;
}

static void remember_path(uacpi_namespace_node *n) {
    const char *p = uacpi_namespace_node_generate_absolute_path(n);
    if (!p) return;
    k_strlcpy(g_ec.path, p, sizeof g_ec.path);
    uacpi_free_absolute_path(p);
}

// ACPI 6.5 5.2.16. Through the kernel's own copy of the tables: uACPI's
// <uacpi/acpi.h> names its structs as kernel/acpi.h does.
struct ecdt {
    struct acpi_sdt_header hdr;
    struct acpi_gas control, data;
    uint32_t uid;
    uint8_t gpe_bit;
    char id[];
} __attribute__((packed));
_Static_assert(sizeof(struct ecdt) == 65, "the ECDT's fixed part");

void acpi_ec_probe_ecdt(void) {
    const struct ecdt *e = (const struct ecdt *)acpi_find_table("ECDT");
    if (!e) return;
    uint32_t len = e->hdr.length;
    int io = len >= sizeof *e && e->control.space_id == UACPI_ADDRESS_SPACE_SYSTEM_IO &&
             e->data.space_id == UACPI_ADDRESS_SPACE_SYSTEM_IO &&
             e->control.address && e->control.address <= 0xFFFF &&
             e->data.address && e->data.address <= 0xFFFF;
    if (!io) {
        klog_write("ec: the ECDT is short or not port I/O -- looking for the device instead\n");
        return;
    }
    // The path, NUL-terminated within the table. The record keeps 48
    // bytes (QUERY_ACPI_EC's), and a path cut there would name nothing:
    // said, and the device found by its PNP0C09 instead.
    char id[48];
    uint32_t n = len - (uint32_t)sizeof *e, m = 0;
    while (m < n && e->id[m]) m++;
    if (m >= sizeof id) {
        klog_printf("ec: the ECDT's EC path is %u bytes, over %u -- found by PNP0C09 instead\n",
                    (unsigned)m, (unsigned)sizeof id - 1);
        m = 0;
    }
    k_memcpy(id, e->id, m);
    id[m] = 0;
    set_ports((uint16_t)e->control.address, (uint16_t)e->data.address);
    g_ec.gpe = e->gpe_bit;

    if (!install(uacpi_namespace_root())) return;
    g_ec.via = ACPI_EC_VIA_ECDT;
    k_strlcpy(g_ec.path, id, sizeof g_ec.path);
    if (id[0] && uacpi_namespace_node_find(UACPI_NULL, id, &g_ec.node) != UACPI_STATUS_OK) {
        g_ec.node = UACPI_NULL;   // the device is found after namespace init instead
        klog_printf("ec: the ECDT names %s, which the namespace does not have\n", id);
    }
    klog_printf("ec: from the ECDT -- %s, command 0x%x, data 0x%x, GPE 0x%02x\n",
                id[0] ? id : "(no path)", (unsigned)g_ec.io.cmd, (unsigned)g_ec.io.data,
                (unsigned)g_ec.gpe);
}

struct crs_ports { uint16_t port[2]; int n; };

static uacpi_iteration_decision crs_port(void *user, uacpi_resource *r) {
    struct crs_ports *p = user;
    uint16_t at;
    if (r->type == UACPI_RESOURCE_TYPE_IO) at = r->io.minimum;
    else if (r->type == UACPI_RESOURCE_TYPE_FIXED_IO) at = r->fixed_io.address;
    else return UACPI_ITERATION_DECISION_CONTINUE;
    p->port[p->n++] = at;
    return p->n == 2 ? UACPI_ITERATION_DECISION_BREAK : UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision first_ec(void *user, uacpi_namespace_node *node, uacpi_u32 depth) {
    (void)depth;
    *(uacpi_namespace_node **)user = node;
    return UACPI_ITERATION_DECISION_BREAK;
}

// Finds the device and turns its events on: 0, or why it could not.
static const char *serve(void) {
    uacpi_namespace_node *dev = UACPI_NULL;
    if (!g_ec.node) uacpi_find_devices("PNP0C09", first_ec, &dev);
    if (!g_ec.node && dev) g_ec.node = dev;

    if (!g_ec.via) {
        if (!dev) return "the interpreter found no embedded controller";
        struct crs_ports p = { { 0, 0 }, 0 };
        uacpi_resources *res = UACPI_NULL;
        if (uacpi_get_current_resources(dev, &res) == UACPI_STATUS_OK) {
            uacpi_for_each_resource(res, crs_port, &p);
            uacpi_free_resources(res);
        }
        if (p.n < 2 || !p.port[0] || !p.port[1])
            return "the embedded controller's _CRS has no two ports";
        set_ports(p.port[1], p.port[0]);   // ACPI 6.5 12.11: data first, then command/status
        if (!install(dev)) return "the embedded controller's handler was refused";
        g_ec.via = ACPI_EC_VIA_CRS;
        g_ec.gpe = -1;
        remember_path(dev);
        klog_printf("ec: %s from its _CRS -- command 0x%x, data 0x%x\n", g_ec.path,
                    (unsigned)g_ec.io.cmd, (unsigned)g_ec.io.data);
    } else if (g_ec.node) {
        remember_path(g_ec.node);
    }
    if (!g_ec.node) return "no embedded controller device in the namespace";

    // The device's _GPE wins over the ECDT's, as in Linux; a Package
    // (a GPE block device) is not handled.
    uacpi_u64 v;
    if (uacpi_eval_simple_integer(g_ec.node, "_GPE", &v) == UACPI_STATUS_OK && v < 256)
        g_ec.gpe = (int32_t)v;
    if (g_ec.gpe < 0) return "the embedded controller has no _GPE";
    if (!(acpi_get_state()->flags & ACPI_F_PWRBTN))
        return "the SCI is not routed (legacy mode, or acpi_sci.c declined it)";
    g_ec.work = uacpi_kernel_create_work_item();
    if (!g_ec.work) return "no memory for the embedded controller's work";
    uacpi_status st = uacpi_install_gpe_handler(UACPI_NULL, (uacpi_u16)g_ec.gpe,
                                                UACPI_GPE_TRIGGERING_EDGE, ec_gpe, UACPI_NULL);
    if (st == UACPI_STATUS_OK) {
        g_ec.events = ACPI_EC_EVENTS_ON;
        st = uacpi_enable_gpe(UACPI_NULL, (uacpi_u16)g_ec.gpe);
    }
    if (st != UACPI_STATUS_OK) {
        g_ec.events = ACPI_EC_EVENTS_OFF;
        klog_printf("ec: GPE 0x%02x refused: %s\n", (unsigned)g_ec.gpe, uacpi_status_to_string(st));
        return "the embedded controller's GPE was refused";
    }
    klog_printf("ec: events on, GPE 0x%02x\n", (unsigned)g_ec.gpe);
    return 0;
}

// An EC whose events are off leaves a machine that entered ACPI mode
// for it (acpi=uacpi on legacy-mode firmware) worse off than legacy
// mode: acpi_sci_ec_unserved() hands it back, and is a no-op elsewhere.
void acpi_ec_probe_namespace(void) {
    const char *why = serve();
    if (!why) return;
    klog_printf("ec: %s -- no events\n", why);
    acpi_sci_ec_unserved(why);
}

void acpi_ec_reset(void) {
    if (g_ec.work) uacpi_kernel_free_work_item(g_ec.work);
    k_memset(&g_ec, 0, sizeof g_ec);
}

void acpi_ec_info(struct acpi_ec_info *out) {
    k_memset(out, 0, sizeof *out);
    out->via = g_ec.via;
    if (!g_ec.via) return;
    out->cmd = g_ec.io.cmd;
    out->data = g_ec.io.data;
    out->gpe = g_ec.gpe;
    out->events = g_ec.events;
    out->transactions = g_ec.io.transactions;
    out->timeouts = g_ec.io.timeouts;
    out->flushed = g_ec.io.flushed;
    out->naps = g_ec.io.naps;
    out->longest_wait_ns = g_ec.io.longest_wait_ns;
    out->gpes = g_ec.gpes;
    out->queries = g_ec.queries;
    out->last_query = g_ec.last_query;
    for (int i = 0; i < 256; i++) out->distinct += g_ec.qcount[i] != 0;
    k_strlcpy(out->path, g_ec.path, sizeof out->path);
}
