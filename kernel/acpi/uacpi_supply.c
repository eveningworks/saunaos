// Batteries, the AC adapter and the lid, through uACPI (docs/aml-design.md,
// stage 4e): Linux's battery.c, ac.c and button.c in one file, as facts
// QUERY_POWER/QUERY_POWER_SUPPLY hand to ring 3.
//
// WHO RUNS WHAT. Everything that evaluates AML runs on kacpid: the probe
// in bring_up(), each Notify() handler (uACPI queues them as work), and
// the poll kacpid makes when its wait times out. Readers -- any syscall,
// any core -- copy the published records under g_lock and never touch the
// namespace, so a reader never waits for an embedded controller.
//
// WHY A POLL. A battery Notify()s on a state change (plugged, unplugged,
// its _BTP trip point), but most firmware says nothing as the percentage
// falls, which is why UPower polls. 30 s while a battery is present, 10 s
// while the charge is critical.
//
// THE CRITICAL SHUTDOWN is the kernel's, not the desktop's, so a text-mode
// boot gets it too: on battery at CRITICAL_PCT or below (or with the
// firmware's own critical bit) for CRITICAL_HOLD_NS, SIGPWR to init -- the
// power button's clean shutdown. Two readings apart, so one bad sample
// cannot do it; `battery=noshutdown` turns it off, so a firmware that
// always reads 0% cannot make the machine unbootable.
#include <uacpi/uacpi.h>
#include <uacpi/namespace.h>
#include <uacpi/notify.h>
#include <uacpi/status.h>
#include <uacpi/types.h>
#include <uacpi/utilities.h>

#include "acpi.h"
#include "battery_parse.h"
#include "clocksource.h"
#include "heap.h"
#include "kfmt.h"
#include "klog.h"
#include "multiboot.h"
#include "scheduler.h"
#include "signal.h"
#include "spinlock.h"
#include "string.h"
#include "uacpi_host.h"

#define POLL_NS          30000000000ull
#define CRITICAL_POLL_NS 10000000000ull
#define NOT_READY_POLL_NS 10000000000ull   // a battery whose information has not come yet...
#define NOT_READY_WINDOW_NS 60000000000ull //   ...for the first minute after boot
#define CRITICAL_HOLD_NS 20000000000ull
#define CRITICAL_PCT     ACPI_POWER_CRITICAL_PCT
// _STA bit 4, ACPI 6.5 6.3.7: a battery is in the bay. Spelled here because
// <uacpi/acpi.h> redefines kernel/acpi.h's structs.
#define STA_BATTERY_PRESENT (1u << 4)

// _BIX has 21 elements (revision 1); the spec sets the bound, so a longer
// package's extra elements are ignored rather than allocated for.
#define PKG_MAX 24

struct supply {
    uacpi_namespace_node *node;
    struct acpi_supply pub;        // what a reader copies
    struct bat_info info;          // a battery's last good _BIX/_BIF
    int have_info;
    int said_why;                  // the "reads nothing" line went out
    int said_not_ready;
    char path[ACPI_SUPPLY_PATH_MAX];
};

static struct spinlock g_lock;     // g_pub and every supply's `pub`
static struct supply *g_sup;       // fixed after the probe, until a reset
static int g_n;
static struct acpi_power g_pub;
static uacpi_namespace_node *g_lid;
static char g_lid_path[ACPI_SUPPLY_PATH_MAX];
static uint64_t g_next_poll;       // kacpid's deadline; 0 = none
static uint64_t g_critical_since;
static int g_shutdown_sent;

// --- the readers' half ---------------------------------------------------

void acpi_power_info(struct acpi_power *out) {
    uint64_t f = spin_lock_irqsave(&g_lock);
    *out = g_pub;
    spin_unlock_irqrestore(&g_lock, f);
}

int acpi_supply_get(int index, struct acpi_supply *out) {
    uint64_t f = spin_lock_irqsave(&g_lock);
    int ok = index >= 0 && index < g_n;
    if (ok) *out = g_sup[index].pub;
    spin_unlock_irqrestore(&g_lock, f);
    return ok;
}

// --- uACPI's objects, flattened ------------------------------------------

static int flatten(uacpi_object *obj, struct bat_val *v) {
    uacpi_object_array arr;
    if (uacpi_object_get_package(obj, &arr) != UACPI_STATUS_OK) return -1;
    int n = arr.count < PKG_MAX ? (int)arr.count : PKG_MAX;
    for (int i = 0; i < n; i++) {
        uacpi_object *e = arr.objects[i];
        k_memset(&v[i], 0, sizeof v[i]);
        uacpi_u64 x;
        uacpi_data_view d;
        if (uacpi_object_get_integer(e, &x) == UACPI_STATUS_OK) {
            v[i].n = x;
        } else {
            v[i].is_text = 1;   // a String, a Buffer -- or a Reference, which no number field accepts
            if (uacpi_object_get_string_or_buffer(e, &d) == UACPI_STATUS_OK) {
                unsigned m = 0;
                while (m < d.length && m + 1 < BAT_TEXT_MAX && d.const_text[m]) m++;
                k_memcpy(v[i].text, d.const_text, m);
                v[i].text[m] = 0;
            }
        }
    }
    return n;
}

// The last evaluation's status, for the line that says why a battery reads
// nothing.
static uacpi_status g_last_eval;

static int eval_package(uacpi_namespace_node *node, const char *name, struct bat_val *v) {
    uacpi_object *obj = UACPI_NULL;
    g_last_eval = uacpi_eval_simple_typed(node, name, UACPI_OBJECT_PACKAGE_BIT, &obj);
    if (g_last_eval != UACPI_STATUS_OK) return -1;
    int n = flatten(obj, v);
    uacpi_object_unref(obj);
    return n;
}

static void path_of(uacpi_namespace_node *node, char *out) {
    const char *p = uacpi_namespace_node_generate_absolute_path(node);
    k_strlcpy(out, p ? p : "?", ACPI_SUPPLY_PATH_MAX);
    if (p) uacpi_free_absolute_path(p);
}

// --- refreshing one device (kacpid) --------------------------------------

static void battery_read(struct supply *s, int reread_info, struct acpi_supply *p) {
    *p = s->pub;
    uacpi_u32 sta = 0;
    if (uacpi_eval_sta(s->node, &sta) != UACPI_STATUS_OK || !(sta & STA_BATTERY_PRESENT)) {
        // An empty bay: nothing it said before still holds.
        s->have_info = 0;
        s->said_why = s->said_not_ready = 0;   // a battery put in next is news again
        p->flags = 0;
        p->model[0] = p->serial[0] = p->kind[0] = p->maker[0] = 0;
        p->percent = -1;
        p->design_mwh = p->full_mwh = p->now_mwh = p->rate_mw = p->mv = p->cycles = ACPI_SUPPLY_UNKNOWN;
        return;
    }
    struct bat_val v[PKG_MAX];
    if (reread_info || !s->have_info) {
        struct bat_info b;
        int n = eval_package(s->node, "_BIX", v);
        int ok = n >= 0 && bat_parse_bix(v, n, &b) == 0;
        uacpi_status bix_st = g_last_eval;
        int bix_n = n;
        if (!ok) {
            n = eval_package(s->node, "_BIF", v);
            ok = n >= 0 && bat_parse_bif(v, n, &b) == 0;
        }
        // ONCE per battery: a bay that reads nothing says why, or nobody can.
        if (!ok && !s->said_why) {
            s->said_why = 1;
            klog_printf("power: %s -- neither _BIX (%s, %d element(s)) nor _BIF (%s, %d) gave a battery's "
                        "information\n", s->path, uacpi_status_to_string(bix_st), bix_n,
                        uacpi_status_to_string(g_last_eval), n);
        }
        // NOT READY: firmware answers its default package -- every number
        // unknown -- until its EC has the battery's data (the ASUS does,
        // early in boot). Not information; asked again at the next reading.
        if (ok && b.design_mwh == BAT_UNKNOWN && b.full_mwh == BAT_UNKNOWN) {
            ok = 0;
            if (!s->said_not_ready) {
                s->said_not_ready = 1;
                klog_printf("power: %s -- its information is not ready yet; asked again at the next reading\n",
                            s->path);
            }
        }
        if (ok) {
            s->info = b;
            s->have_info = 1;
            // The firmware's own alarm at our critical level, in its units;
            // Notify(0x80) when the charge crosses it. Optional.
            if (b.full_mwh != BAT_UNKNOWN) {
                uint64_t at = b.full_mwh * CRITICAL_PCT / 100;
                if (b.mah) at = b.design_mv && b.design_mv != BAT_UNKNOWN ? at * 1000 / b.design_mv : 0;
                uacpi_object *arg = uacpi_object_create_integer(at);
                if (arg) {
                    uacpi_object_array args = { &arg, 1 };
                    uacpi_execute(s->node, "_BTP", &args);
                    uacpi_object_unref(arg);
                }
            }
        }
    }
    // The last reading's state stands until a _BST parses (below).
    p->flags = ACPI_SUPPLY_PRESENT |
               (s->pub.flags & (ACPI_SUPPLY_DISCHARGING | ACPI_SUPPLY_CHARGING | ACPI_SUPPLY_CRITICAL));
    if (!s->have_info) return;   // present, but nothing it says parses
    p->design_mwh = s->info.design_mwh;
    p->full_mwh = s->info.full_mwh;
    p->cycles = s->info.cycles;
    if (s->info.mah) p->flags |= ACPI_SUPPLY_MAH;
    k_strlcpy(p->model, s->info.model, sizeof p->model);
    k_strlcpy(p->serial, s->info.serial, sizeof p->serial);
    k_strlcpy(p->kind, s->info.type, sizeof p->kind);
    k_strlcpy(p->maker, s->info.maker, sizeof p->maker);
    struct bat_status st;
    int n = eval_package(s->node, "_BST", v);
    if (n < 0 || bat_parse_bst(v, n, &s->info, &st) != 0) return;   // the last reading stands
    p->flags = ACPI_SUPPLY_PRESENT | (s->info.mah ? ACPI_SUPPLY_MAH : 0);
    p->flags |= (st.state & BAT_DISCHARGING ? ACPI_SUPPLY_DISCHARGING : 0) |
                (st.state & BAT_CHARGING ? ACPI_SUPPLY_CHARGING : 0) |
                (st.state & BAT_CRITICAL ? ACPI_SUPPLY_CRITICAL : 0);
    p->now_mwh = st.now_mwh;
    p->rate_mw = st.rate_mw;
    p->mv = st.mv;
    p->percent = bat_percent(&s->info, &st);
}

static void mains_read(struct supply *s, struct acpi_supply *p) {
    *p = s->pub;
    uacpi_u64 on = 0;
    p->flags = ACPI_SUPPLY_PRESENT;
    if (uacpi_eval_simple_integer(s->node, "_PSR", &on) == UACPI_STATUS_OK && on)
        p->flags |= ACPI_SUPPLY_ONLINE;
}

// Recompute the summary from the published records. Under g_lock.
static void summarise(void) {
    uint32_t keep = g_pub.flags & (ACPI_POWER_LID_PRESENT | ACPI_POWER_LID_OPEN | ACPI_POWER_SHUTDOWN_SENT);
    uint32_t fl = keep;
    int discharging = 0;
    for (int i = 0; i < g_n; i++) {
        const struct acpi_supply *p = &g_sup[i].pub;
        if (p->type == ACPI_SUPPLY_MAINS) {
            fl |= ACPI_POWER_AC_PRESENT;
            if (p->flags & ACPI_SUPPLY_ONLINE) fl |= ACPI_POWER_AC_ONLINE;
        } else if (p->flags & ACPI_SUPPLY_PRESENT) {
            fl |= ACPI_POWER_BATTERY;
            if (p->flags & ACPI_SUPPLY_DISCHARGING) discharging = 1;
        }
    }
    // No adapter device at all: the battery's own state says it.
    if ((fl & ACPI_POWER_AC_PRESENT) ? !(fl & ACPI_POWER_AC_ONLINE) && (fl & ACPI_POWER_BATTERY) : discharging)
        fl |= ACPI_POWER_ON_BATTERY;
    g_pub.flags = fl;
}

// Publish `p` as supply `i`; the generation moves only on a real change.
static void publish(int i, const struct acpi_supply *p) {
    uint64_t f = spin_lock_irqsave(&g_lock);
    if (k_memcmp(&g_sup[i].pub, p, sizeof *p) != 0) {
        g_sup[i].pub = *p;
        summarise();
        g_pub.generation++;
    }
    spin_unlock_irqrestore(&g_lock, f);
}

static void lid_read(int counted) {
    uacpi_u64 open = 1;
    if (uacpi_eval_simple_integer(g_lid, "_LID", &open) != UACPI_STATUS_OK) return;
    uint64_t f = spin_lock_irqsave(&g_lock);
    uint32_t was = g_pub.flags;
    if (open) g_pub.flags |= ACPI_POWER_LID_OPEN;
    else g_pub.flags &= ~(uint32_t)ACPI_POWER_LID_OPEN;
    int changed = was != g_pub.flags;
    if (changed && counted) g_pub.lid_changes++;
    if (changed) g_pub.generation++;
    spin_unlock_irqrestore(&g_lock, f);
    if (changed && counted) klog_printf("power: the lid is %s\n", open ? "open" : "closed");
}

// --- the critical charge -------------------------------------------------

static int combined_percent(void) {
    uint64_t now = 0, full = 0;
    uint64_t f = spin_lock_irqsave(&g_lock);
    for (int i = 0; i < g_n; i++) {
        const struct acpi_supply *p = &g_sup[i].pub;
        if (p->type != ACPI_SUPPLY_BATTERY || !(p->flags & ACPI_SUPPLY_PRESENT)) continue;
        uint64_t fu = p->full_mwh != ACPI_SUPPLY_UNKNOWN && p->full_mwh ? p->full_mwh : p->design_mwh;
        if (fu == ACPI_SUPPLY_UNKNOWN || p->now_mwh == ACPI_SUPPLY_UNKNOWN) continue;
        now += p->now_mwh;
        full += fu;
    }
    spin_unlock_irqrestore(&g_lock, f);
    return full ? (int)((now * 100 + full / 2) / full) : -1;
}

static void check_critical(void) {
    struct acpi_power pw;
    acpi_power_info(&pw);
    int fw_critical = 0;
    for (int i = 0; i < g_n; i++) {
        struct acpi_supply p;
        if (acpi_supply_get(i, &p) && (p.flags & ACPI_SUPPLY_CRITICAL) && (p.flags & ACPI_SUPPLY_DISCHARGING))
            fw_critical = 1;
    }
    int pct = combined_percent();
    int critical = (pw.flags & ACPI_POWER_ON_BATTERY) && ((pct >= 0 && pct <= CRITICAL_PCT) || fw_critical);
    uint64_t now = clocksource_now_ns();
    if (!critical) {
        if (g_critical_since) klog_write("power: the battery is out of its critical level\n");
        g_critical_since = 0;
        g_shutdown_sent = 0;   // charged and drained again: the rule holds again
        return;
    }
    if (!g_critical_since) {
        g_critical_since = now;
        klog_printf("power: the battery is critical (%d%%%s) -- shutting down in %llu s unless it charges\n",
                    pct, fw_critical ? ", the firmware says so" : "",
                    (unsigned long long)(CRITICAL_HOLD_NS / 1000000000ull));
        return;
    }
    if (g_shutdown_sent || now - g_critical_since < CRITICAL_HOLD_NS) return;
    if (multiboot_cmdline_has("battery=noshutdown")) {
        klog_write("power: still critical; battery=noshutdown -- staying up\n");
        g_shutdown_sent = 1;   // said once
        return;
    }
    g_shutdown_sent = 1;   // asked once per critical spell, whatever the answer
    int init = scheduler_init_pid();
    klog_printf("power: the battery is critical -- SIGPWR to init (pid %d) for a clean shutdown\n", init);
    if (init <= 0 || !signal_send(init, SIGPWR)) {
        klog_write("power: no init to tell -- staying up\n");
        return;
    }
    uint64_t f = spin_lock_irqsave(&g_lock);
    g_pub.flags |= ACPI_POWER_SHUTDOWN_SENT;
    g_pub.generation++;
    spin_unlock_irqrestore(&g_lock, f);
}

static void schedule_poll(void) {
    struct acpi_power pw;
    acpi_power_info(&pw);
    if (!(pw.flags & ACPI_POWER_BATTERY)) {
        // An empty bay still polls, slowly, for a battery put in unannounced.
        g_next_poll = g_n ? clocksource_now_ns() + POLL_NS : 0;
        return;
    }
    int waiting = 0;
    for (int i = 0; i < g_n; i++)
        if (g_sup[i].pub.type == ACPI_SUPPLY_BATTERY && (g_sup[i].pub.flags & ACPI_SUPPLY_PRESENT) &&
            !g_sup[i].have_info)
            waiting = 1;
    g_next_poll = clocksource_now_ns() + (g_critical_since ? CRITICAL_POLL_NS
                                          : waiting && clocksource_now_ns() < NOT_READY_WINDOW_NS
                                          ? NOT_READY_POLL_NS : POLL_NS);
}

static void refresh_all(int reread_info) {
    for (int i = 0; i < g_n; i++) {
        struct acpi_supply p;
        if (g_sup[i].pub.type == ACPI_SUPPLY_MAINS) mains_read(&g_sup[i], &p);
        else battery_read(&g_sup[i], reread_info, &p);
        publish(i, &p);
    }
    check_critical();
    schedule_poll();
}

// --- Notify() ------------------------------------------------------------
//
// 0x80 status changed, 0x81 information changed (a battery put in or
// taken out). An adapter's change moves every battery's state too, so
// both refresh everything -- there are two or three devices.

static uacpi_status supply_notify(uacpi_handle ctx, uacpi_namespace_node *node, uacpi_u64 value) {
    (void)ctx; (void)node;
    uint64_t f = spin_lock_irqsave(&g_lock);
    g_pub.notifies++;
    spin_unlock_irqrestore(&g_lock, f);
    struct acpi_power was;
    acpi_power_info(&was);
    refresh_all(value == 0x81);
    struct acpi_power now;
    acpi_power_info(&now);
    if ((was.flags ^ now.flags) & ACPI_POWER_AC_ONLINE)
        klog_printf("power: the charger is %s\n", now.flags & ACPI_POWER_AC_ONLINE ? "plugged in" : "unplugged");
    return UACPI_STATUS_OK;
}

static uacpi_status lid_notify(uacpi_handle ctx, uacpi_namespace_node *node, uacpi_u64 value) {
    (void)ctx; (void)node;
    if (value == 0x80) lid_read(1);
    return UACPI_STATUS_OK;
}

// --- finding them --------------------------------------------------------

struct found { uacpi_namespace_node **nodes; int n, cap; };

static uacpi_iteration_decision collect(void *user, uacpi_namespace_node *node, uacpi_u32 depth) {
    (void)depth;
    struct found *f = user;
    if (f->n == f->cap) {
        int cap = f->cap ? f->cap * 2 : 4;
        uacpi_namespace_node **grown = kmalloc(sizeof *grown * (unsigned)cap);
        if (!grown) return UACPI_ITERATION_DECISION_BREAK;
        if (f->n) k_memcpy(grown, f->nodes, sizeof *grown * (unsigned)f->n);
        kfree(f->nodes);
        f->nodes = grown;
        f->cap = cap;
    }
    f->nodes[f->n++] = node;
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision first_lid(void *user, uacpi_namespace_node *node, uacpi_u32 depth) {
    (void)depth;
    *(uacpi_namespace_node **)user = node;
    return UACPI_ITERATION_DECISION_BREAK;
}

void acpi_supply_probe(void) {
    struct found ac = { 0, 0, 0 }, bat = { 0, 0, 0 };
    uacpi_find_devices("ACPI0003", collect, &ac);
    uacpi_find_devices("PNP0C0A", collect, &bat);
    int n = ac.n + bat.n;
    struct supply *s = n ? kzalloc(sizeof *s * (unsigned)n) : 0;
    if (n && !s) {
        klog_write("power: no memory for the power supplies -- none are read this boot\n");
        n = 0;
    }
    for (int i = 0; i < n; i++) {
        int is_ac = i < ac.n;
        s[i].node = is_ac ? ac.nodes[i] : bat.nodes[i - ac.n];
        s[i].pub.type = is_ac ? ACPI_SUPPLY_MAINS : ACPI_SUPPLY_BATTERY;
        s[i].pub.percent = -1;
        s[i].pub.design_mwh = s[i].pub.full_mwh = s[i].pub.now_mwh = ACPI_SUPPLY_UNKNOWN;
        s[i].pub.rate_mw = s[i].pub.mv = s[i].pub.cycles = ACPI_SUPPLY_UNKNOWN;
        uacpi_object_name nm = uacpi_namespace_node_name(s[i].node);
        k_memcpy(s[i].pub.name, nm.text, 4);
        s[i].pub.name[4] = 0;
        path_of(s[i].node, s[i].path);
    }
    kfree(ac.nodes);
    kfree(bat.nodes);
    uint64_t f = spin_lock_irqsave(&g_lock);
    g_sup = s;
    g_n = n;
    spin_unlock_irqrestore(&g_lock, f);

    for (int i = 0; i < n; i++)
        if (uacpi_install_notify_handler(s[i].node, supply_notify, UACPI_NULL) != UACPI_STATUS_OK)
            klog_printf("power: %s -- its Notify() handler was refused; it is polled only\n", s[i].path);
    refresh_all(1);
    for (int i = 0; i < n; i++) {
        struct acpi_supply p;
        acpi_supply_get(i, &p);
        if (p.type == ACPI_SUPPLY_MAINS)
            klog_printf("power: AC adapter %s -- %s\n", s[i].path, p.flags & ACPI_SUPPLY_ONLINE ? "online" : "offline");
        else if (!(p.flags & ACPI_SUPPLY_PRESENT))
            klog_printf("power: battery %s -- the bay is empty\n", s[i].path);
        else if (!s[i].have_info)
            klog_printf("power: battery %s -- in its bay, its information not read yet\n", s[i].path);
        else
            klog_printf("power: battery %s -- %s %s, %d%%, %llu of %llu mWh (design %llu)%s\n", s[i].path,
                        p.maker, p.model, p.percent, (unsigned long long)p.now_mwh,
                        (unsigned long long)p.full_mwh, (unsigned long long)p.design_mwh,
                        p.flags & ACPI_SUPPLY_MAH ? ", converted from mAh" : "");
    }

    uacpi_namespace_node *lid = UACPI_NULL;
    uacpi_find_devices("PNP0C0D", first_lid, &lid);
    if (lid) {
        g_lid = lid;
        path_of(lid, g_lid_path);
        f = spin_lock_irqsave(&g_lock);
        g_pub.flags |= ACPI_POWER_LID_PRESENT | ACPI_POWER_LID_OPEN;   // open until _LID says otherwise
        g_pub.generation++;
        spin_unlock_irqrestore(&g_lock, f);
        lid_read(0);
        if (uacpi_install_notify_handler(lid, lid_notify, UACPI_NULL) != UACPI_STATUS_OK)
            klog_printf("power: lid %s -- its Notify() handler was refused; closing it does nothing\n", g_lid_path);
        struct acpi_power pw;
        acpi_power_info(&pw);
        klog_printf("power: lid %s -- %s\n", g_lid_path, pw.flags & ACPI_POWER_LID_OPEN ? "open" : "closed");
    }
    if (!n && !lid) klog_write("power: no battery, AC adapter or lid in the namespace\n");
}

uint64_t acpi_supply_deadline(void) { return g_next_poll; }

void acpi_supply_poll(void) {
    if (!g_next_poll || clocksource_now_ns() < g_next_poll) return;
    uint64_t f = spin_lock_irqsave(&g_lock);
    g_pub.polls++;
    spin_unlock_irqrestore(&g_lock, f);
    refresh_all(0);
}

// After uacpi_state_reset(), which freed the handlers and the nodes.
void acpi_supply_reset(void) {
    uint64_t f = spin_lock_irqsave(&g_lock);
    struct supply *s = g_sup;
    g_sup = 0;
    g_n = 0;
    k_memset(&g_pub, 0, sizeof g_pub);
    spin_unlock_irqrestore(&g_lock, f);
    kfree(s);
    g_lid = UACPI_NULL;
    g_next_poll = 0;
    g_critical_since = 0;
    g_shutdown_sent = 0;
}
