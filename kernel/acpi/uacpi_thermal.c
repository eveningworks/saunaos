// Thermal zones through uACPI: Linux's drivers/acpi/thermal.c, as facts
// QUERY_THERMAL hands to ring 3, plus what a zone asks the OS to do.
//
// WHO RUNS WHAT: as uacpi_supply.c -- the probe, the Notify() handler and
// the poll all run on kacpid; readers copy the published records under
// g_lock and never touch the namespace.
//
// READ WHEN: on the zone's Notify(0x80) (the ASUS's EC raises one), every
// _TZP when the firmware asks for polling, else every DEFAULT_POLL_NS so
// Task Manager's chart moves -- Linux reads _TMP on each sysfs read
// instead, which a reader here may not do. While passive cooling runs,
// every _TSP too.
//
// WHAT A ZONE ASKS FOR:
//   _CRT -- a clean shutdown, SIGPWR to init (the critical battery's path),
//           on TWO readings at or over it a second apart: one bad sample
//           cannot do it. `thermal.crt=-1` turns it off, `thermal.crt=C`
//           moves it -- Linux's words.
//   _HOT -- Linux hibernates; there is no S4 here, so it is logged.
//   _PSV -- passive cooling (thermal_policy.c): a frequency ceiling on
//           every core (cputherm.c), the lowest any zone asks for. Needs
//           _PSL, _TC1 and _TC2; `thermal.psv=` moves or (-1) disables it.
//   _ACx -- fans: not read (no machine here has one in its zone).
#include <uacpi/namespace.h>
#include <uacpi/notify.h>
#include <uacpi/status.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

#include "acpi.h"
#include "clocksource.h"
#include "cputherm.h"
#include "heap.h"
#include "kfmt.h"
#include "klog.h"
#include "knum.h"
#include "multiboot.h"
#include "query.h"
#include "scheduler.h"
#include "signal.h"
#include "spinlock.h"
#include "string.h"
#include "thermal_policy.h"
#include "uacpi_host.h"

#define DEFAULT_POLL_NS   5000000000ull
#define CONFIRM_NS        1000000000ull    // the second critical reading
#define DEFAULT_TSP_DS    100              // _TSP missing: 10 s, in its tenths
#define MIN_PERIOD_DS     10               // no firmware gets a reading more often than 1 s

struct zone {
    uacpi_namespace_node *node;
    struct query_thermal pub;
    struct thermal_passive passive;
    int can_passive;                // _PSV, _PSL, _TC1, _TC2 all there
    int tc1, tc2;
    uint64_t poll_ns, tsp_ns;
    uint64_t next_ns;               // the next reading
    uint64_t next_step_ns;          // the next passive step, every _TSP
    uint64_t crit_since;
    int said_hot;
};

static struct spinlock g_lock;      // every zone's `pub`, g_n
static struct zone *g_z;
static int g_n;
static uint64_t g_next;             // the earliest zone's; 0 = none
static int g_limit = 100;           // what cputherm_limit() was last given
static const char *g_limit_why;     // ...and why it did not reach the hardware, or 0

// --- readers ---------------------------------------------------------------

int acpi_thermal_get(int index, struct query_thermal *out) {
    uint64_t f = spin_lock_irqsave(&g_lock);
    int ok = index >= 0 && index < g_n;
    if (ok) *out = g_z[index].pub;
    spin_unlock_irqrestore(&g_lock, f);
    return ok;
}

// --- the boot words ----------------------------------------------------------

// `thermal.crt=` / `thermal.psv=` in degrees C: 1 and *mc set when the
// word moves the trip, *off when it is -1.
static int override(const char *key, int64_t *mc, int *off) {
    char v[16];
    int64_t c;
    *off = 0;
    if (!multiboot_cmdline_value(key, v, sizeof v) || !k_parse_i64(v, &c)) return 0;
    if (c < 0) {
        *off = 1;
        return 1;
    }
    *mc = c * 1000;
    return 1;
}

// --- one zone --------------------------------------------------------------

static int64_t trip(struct zone *z, const char *name) {
    uacpi_u64 dk = 0;
    if (uacpi_eval_simple_integer(z->node, name, &dk) != UACPI_STATUS_OK) return THERMAL_NONE;
    return thermal_dk_to_mc(dk);
}

static int small_int(struct zone *z, const char *name, int *out) {
    uacpi_u64 v = 0;
    if (uacpi_eval_simple_integer(z->node, name, &v) != UACPI_STATUS_OK || v > 100000) return 0;
    *out = (int)v;
    return 1;
}

// The trips and constants: at the probe and on Notify(0x81). Into `p`,
// a copy of the zone's record; the policy's own fields into z.
static void read_trips(struct zone *z, struct query_thermal *p) {
    p->critical_mc = trip(z, "_CRT");
    p->hot_mc = trip(z, "_HOT");
    p->passive_mc = trip(z, "_PSV");
    int off;
    int64_t mc;
    if (override("thermal.crt=", &mc, &off)) p->critical_mc = off ? THERMAL_NONE : mc;
    if (override("thermal.psv=", &mc, &off)) p->passive_mc = off ? THERMAL_NONE : mc;

    uacpi_object *psl = UACPI_NULL;
    int has_psl = 0;
    if (uacpi_eval_simple_typed(z->node, "_PSL", UACPI_OBJECT_PACKAGE_BIT, &psl) == UACPI_STATUS_OK) {
        uacpi_object_array arr;
        has_psl = uacpi_object_get_package(psl, &arr) == UACPI_STATUS_OK && arr.count > 0;
        uacpi_object_unref(psl);
    }
    int tsp = 0, tzp = 0;
    z->can_passive = p->passive_mc != THERMAL_NONE && has_psl &&
                     small_int(z, "_TC1", &z->tc1) && small_int(z, "_TC2", &z->tc2);
    if (!small_int(z, "_TSP", &tsp) || !tsp) tsp = DEFAULT_TSP_DS;
    if (tsp < MIN_PERIOD_DS) tsp = MIN_PERIOD_DS;
    z->tsp_ns = (uint64_t)tsp * 100000000ull;
    char v[16];
    uint32_t w;
    if (multiboot_cmdline_value("thermal.tzp=", v, sizeof v) && k_parse_u32(v, &w)) tzp = (int)w;
    else small_int(z, "_TZP", &tzp);
    if (tzp && tzp < MIN_PERIOD_DS) tzp = MIN_PERIOD_DS;
    z->poll_ns = tzp ? (uint64_t)tzp * 100000000ull : DEFAULT_POLL_NS;
}

static void publish(struct zone *z, struct query_thermal *p) {
    uint64_t f = spin_lock_irqsave(&g_lock);
    p->generation = z->pub.generation + 1;
    z->pub = *p;
    spin_unlock_irqrestore(&g_lock, f);
}

static void shutdown_for(struct zone *z, struct query_thermal *p) {
    int init = scheduler_init_pid();
    klog_printf(KLOG_WARN "thermal: %s is at %lld C, its critical %lld C -- SIGPWR to init (pid %d)\n",
                p->path, (long long)(p->temp_mc / 1000), (long long)(p->critical_mc / 1000), init);
    if (init <= 0 || !signal_send(init, SIGPWR)) {
        klog_write("thermal: no init to tell -- staying up\n");
        return;
    }
    p->flags |= QUERY_THERMAL_SHUTDOWN;
    (void)z;
}

// The lowest ceiling any zone asks for, written when it changed -- and
// EVERY zone's record told the outcome each time, since a zone that joins
// a spell at a ceiling no lower than another's changes nothing written.
static void apply_limit(void) {
    int lowest = 100;
    for (int i = 0; i < g_n; i++)
        if (g_z[i].passive.active && g_z[i].passive.perf_pct < lowest) lowest = g_z[i].passive.perf_pct;
    if (lowest != g_limit) {
        g_limit_why = cputherm_limit(lowest);
        if (lowest < 100 && g_limit == 100)
            klog_printf("thermal: passive cooling -- the CPUs limited to %d%%%s%s\n", lowest,
                        g_limit_why ? ": not applied, " : "", g_limit_why ? g_limit_why : "");
        else if (lowest == 100)
            klog_write("thermal: passive cooling over -- the CPUs at full speed\n");
        g_limit = lowest;
    }
    uint64_t f = spin_lock_irqsave(&g_lock);
    for (int i = 0; i < g_n; i++) {
        struct query_thermal *p = &g_z[i].pub;
        int slowed = g_z[i].passive.active && g_z[i].passive.perf_pct < 100;
        uint64_t flags = p->flags & ~(uint64_t)(QUERY_THERMAL_APPLIED | QUERY_THERMAL_PASSIVE);
        char why[sizeof p->why] = "";
        if (slowed) {
            flags |= QUERY_THERMAL_PASSIVE;
            if (g_limit_why) k_strlcpy(why, g_limit_why, sizeof why);
            else flags |= QUERY_THERMAL_APPLIED;
        }
        if (flags == p->flags && !k_strcmp(why, p->why)) continue;
        p->flags = flags;
        k_strlcpy(p->why, why, sizeof p->why);
        p->generation++;
    }
    spin_unlock_irqrestore(&g_lock, f);
}

static void read_zone(struct zone *z, int reread_trips, int policy_step) {
    struct query_thermal p;
    uint64_t f = spin_lock_irqsave(&g_lock);
    p = z->pub;
    spin_unlock_irqrestore(&g_lock, f);
    if (reread_trips) read_trips(z, &p);

    uint64_t now = clocksource_now_ns();
    uacpi_u64 dk = 0;
    p.readings++;
    p.flags &= ~(uint64_t)(QUERY_THERMAL_VALID | QUERY_THERMAL_CRITICAL | QUERY_THERMAL_HOT_SOON);
    p.temp_mc = uacpi_eval_simple_integer(z->node, "_TMP", &dk) == UACPI_STATUS_OK ? thermal_dk_to_mc(dk)
                                                                                    : THERMAL_NONE;
    int64_t t = p.temp_mc;
    int confirm = 0;
    if (t != THERMAL_NONE) {
        p.flags |= QUERY_THERMAL_VALID;
        if (p.max_mc == THERMAL_NONE || t > p.max_mc) p.max_mc = t;
        if (p.critical_mc != THERMAL_NONE && t + QUERY_THERMAL_WARN_MC >= p.critical_mc)
            p.flags |= QUERY_THERMAL_HOT_SOON;
        if (p.critical_mc != THERMAL_NONE && t >= p.critical_mc) {
            p.flags |= QUERY_THERMAL_CRITICAL;
            if (!z->crit_since) {
                z->crit_since = now;
                confirm = 1;
                klog_printf(KLOG_WARN "thermal: %s at %lld C, at its critical %lld C -- reading it again\n",
                            p.path, (long long)(t / 1000), (long long)(p.critical_mc / 1000));
            } else if (!(p.flags & QUERY_THERMAL_SHUTDOWN) && now - z->crit_since >= CONFIRM_NS) {
                shutdown_for(z, &p);
            }
        } else {
            z->crit_since = 0;
            p.flags &= ~(uint64_t)QUERY_THERMAL_SHUTDOWN;   // cooled: the rule holds again
        }
        if (p.hot_mc != THERMAL_NONE && t >= p.hot_mc && !z->said_hot) {
            z->said_hot = 1;
            klog_printf(KLOG_WARN "thermal: %s at its hot trip %lld C -- no hibernation here\n", p.path,
                        (long long)(p.hot_mc / 1000));
        } else if (p.hot_mc != THERMAL_NONE && t < p.hot_mc) {
            z->said_hot = 0;
        }
        // A spell whose trips went away (a Notify(0x81) re-read) ends here,
        // or its ceiling would hold for the rest of the boot.
        if (z->passive.active && (!z->can_passive || p.passive_mc == THERMAL_NONE)) {
            z->passive = (struct thermal_passive){ 0, 100, 0 };
            klog_printf("thermal: %s -- its passive trip is gone; the spell ends\n", p.path);
        }
        int starts = !z->passive.active && p.passive_mc != THERMAL_NONE && t >= p.passive_mc;
        int due = policy_step && z->passive.active && now >= z->next_step_ns;
        if (z->can_passive && (starts || due)) {
            int was = z->passive.active;
            thermal_passive_step(&z->passive, t, p.passive_mc, z->tc1, z->tc2);
            z->next_step_ns = now + z->tsp_ns;
            if (z->passive.active && !was)
                klog_printf("thermal: %s at %lld C, over its passive %lld C -- slowing the CPUs\n", p.path,
                            (long long)(t / 1000), (long long)(p.passive_mc / 1000));
        }
    }
    // QUERY_THERMAL_PASSIVE and _APPLIED are apply_limit()'s, which runs
    // after every reading: one place says whether anything is slowed.
    p.perf_pct = (uint64_t)(z->passive.active ? z->passive.perf_pct : 100);
    publish(z, &p);

    uint64_t next = now + (confirm ? CONFIRM_NS : z->poll_ns);
    if (z->passive.active && z->next_step_ns < next) next = z->next_step_ns;
    // A Notify between the two critical readings must not move the second.
    if (z->crit_since && !(p.flags & QUERY_THERMAL_SHUTDOWN) && z->crit_since + CONFIRM_NS < next)
        next = z->crit_since + CONFIRM_NS;
    z->next_ns = next;
}

static void schedule(void) {
    uint64_t next = 0;
    for (int i = 0; i < g_n; i++)
        if (!next || g_z[i].next_ns < next) next = g_z[i].next_ns;
    g_next = next;
}

static uacpi_status zone_notify(uacpi_handle ctx, uacpi_namespace_node *node, uacpi_u64 value) {
    (void)node;
    struct zone *z = ctx;
    if (value != 0x80 && value != 0x81) return UACPI_STATUS_OK;   // 0x82: the device lists
    uint64_t f = spin_lock_irqsave(&g_lock);
    z->pub.notifies++;
    spin_unlock_irqrestore(&g_lock, f);
    read_zone(z, value == 0x81, 0);
    apply_limit();
    schedule();
    return UACPI_STATUS_OK;
}

// --- finding them ------------------------------------------------------------

static void say_c(char *out, unsigned cap, int64_t mc) {
    if (mc == THERMAL_NONE) {
        k_strlcpy(out, "none", cap);
        return;
    }
    k_itoa(mc / 1000, out, cap);
}

void acpi_thermal_probe(void) {
    int n;
    void **nodes = acpi_uacpi_nodes(UACPI_OBJECT_THERMAL_ZONE_BIT, &n);
    struct zone *z = n ? kzalloc(sizeof *z * (unsigned)n) : 0;
    if (n && !z) {
        klog_write("thermal: no memory for the thermal zones -- none are read this boot\n");
        n = 0;
    }
    for (int i = 0; i < n; i++) {
        z[i].node = nodes[i];
        struct query_thermal *p = &z[i].pub;
        p->kind = QUERY_THERMAL_ZONE;
        p->temp_mc = p->max_mc = p->critical_mc = p->hot_mc = p->passive_mc = THERMAL_NONE;
        p->perf_pct = 100;
        z[i].passive.perf_pct = 100;
        uacpi_object_name nm = uacpi_namespace_node_name(z[i].node);
        k_memcpy(p->name, nm.text, 4);
        p->name[4] = 0;
        const char *path = uacpi_namespace_node_generate_absolute_path(z[i].node);
        k_strlcpy(p->path, path ? path : "?", sizeof p->path);
        if (path) uacpi_free_absolute_path(path);
    }
    kfree(nodes);
    uint64_t f = spin_lock_irqsave(&g_lock);
    g_z = z;
    g_n = n;
    spin_unlock_irqrestore(&g_lock, f);

    for (int i = 0; i < n; i++) {
        if (uacpi_install_notify_handler(z[i].node, zone_notify, &z[i]) != UACPI_STATUS_OK)
            klog_printf("thermal: %s -- its Notify() handler was refused; it is polled only\n", z[i].pub.path);
        read_zone(&z[i], 1, 0);
        struct query_thermal p;
        acpi_thermal_get(i, &p);
        char t[12], c[12], ps[12];
        say_c(t, sizeof t, p.temp_mc);
        say_c(c, sizeof c, p.critical_mc);
        say_c(ps, sizeof ps, p.passive_mc);
        klog_printf("thermal: zone %s -- %s C, critical %s, passive %s%s, read every %llu s\n", p.path, t, c,
                    ps, z[i].can_passive ? "" : " (no passive cooling: it lacks _PSL/_TC1/_TC2)",
                    (unsigned long long)(z[i].poll_ns / 1000000000ull));
    }
    apply_limit();
    schedule();
    if (!n) klog_write("thermal: no thermal zone in the namespace\n");
}

uint64_t acpi_thermal_deadline(void) { return g_next; }

void acpi_thermal_poll(void) {
    if (!g_next || clocksource_now_ns() < g_next) return;
    uint64_t now = clocksource_now_ns();
    for (int i = 0; i < g_n; i++)
        if (g_z[i].next_ns <= now) read_zone(&g_z[i], 0, 1);
    apply_limit();
    schedule();
}

// After uacpi_state_reset(), which freed the handlers and the nodes.
void acpi_thermal_reset(void) {
    uint64_t f = spin_lock_irqsave(&g_lock);
    struct zone *z = g_z;
    g_z = 0;
    g_n = 0;
    spin_unlock_irqrestore(&g_lock, f);
    kfree(z);
    g_next = 0;
    if (g_limit != 100) cputherm_limit(100);
    g_limit = 100;
}
