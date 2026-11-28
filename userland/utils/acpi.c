// acpi -- what this machine's firmware described, and what this kernel
// will do with it when asked to stop.
//
// WHAT IT IS FOR. `reboot --poweroff` either stops the machine or it
// does not, and when it does not there is nothing on screen to say
// which half failed. This is that: the tables that were found, the port
// the S5 write goes to, and the sleep type this machine's own `_S5_`
// object named. A `poweroff: no` line here is the answer to "why did it
// just halt instead of turning off".
//
// It is deliberately not a debug dump of every table's body; `--dump`
// gives the bytes. The AML interpreter (uACPI, behind `acpi=uacpi`)
// reports one line: whether it loaded the namespace, and how big it is.
#include <stdint.h>
#include "rt/sys.h"
#include "base/cmd.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>         // the command line, sized to its records
#include "base/ubootcfg.h"  // ubootcfg_cmdline()
#include <errno.h>          // EPERM

#define USAGE "acpi [--dump <SIG> [--at N] [--len N]] [--eval <PATH> [INT...]] [--wake]"

// Mirrors of the kernel-side flags. They cross the boundary as a number
// in the record rather than as a header apps could include: kernel/
// headers are deliberately absent from userland's include path.
#define ACPI_F_TABLES     (1u << 0)
#define ACPI_F_FADT       (1u << 1)
#define ACPI_F_S5         (1u << 2)
#define ACPI_F_HW_REDUCED (1u << 3)
#define ACPI_F_RESET      (1u << 4)
#define ACPI_F_ENABLED    (1u << 5)
#define ACPI_F_XSDT       (1u << 6)
#define ACPI_F_FW_LEGACY  (1u << 7)
#define ACPI_F_PWRBTN     (1u << 8)
#define ACPI_F_PWRBTN_CM  (1u << 9)
#define ACPI_F_EC_LEGACY  (1u << 10)
#define ACPI_F_PWRBTN_UACPI (1u << 11)
#define ACPI_F_S5_UACPI   (1u << 12)


static void put(const char *s) { sys_print(s); }

static const char *yesno(int v) { return v ? "yes" : "no"; }

static const char *source_name(uint64_t v) {
    switch (v) {
        case 1: return "multiboot2 tag";
        case 2: return "BIOS-area scan";
        default: return "not found";
    }
}

static const char *space_name(uint64_t v) {
    return v == 1 ? "port" : v == 0 ? "memory" : "unsupported space";
}

// --- --dump: the raw bytes of one table -------------------------------
//
// The same move `lsusb -D` makes, and for the same reason: a parser for
// firmware data has to be tested against real firmware data, and these
// bytes paste straight into a KTEST fixture. A real DSDT is tens of
// kilobytes, so a range is the normal way to ask.

static int hex_arg(const char *s, long *out) {
    int base = 10, i = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; i = 2; }
    long v = 0;
    if (!s[i]) return 0;
    for (; s[i]; i++) {
        int d;
        if (s[i] >= '0' && s[i] <= '9') d = s[i] - '0';
        else if (base == 16 && s[i] >= 'a' && s[i] <= 'f') d = s[i] - 'a' + 10;
        else if (base == 16 && s[i] >= 'A' && s[i] <= 'F') d = s[i] - 'A' + 10;
        else return 0;
        if (d >= base) return 0;
        v = v * base + d;
    }
    *out = v;
    return 1;
}

static int dump_table(const char *sig, long at, long want) {
    struct query_acpidump r;
    char line[128];
    long total = -1, shown = 0;

    QUERY_FOREACH(QUERY_ACPIDUMP, r, i) {
        if (strncmp(r.signature, sig, 4) != 0) continue;
        if (total < 0) {
            total = (long)r.total;
            snprintf(line, sizeof line, "%s: %ld bytes\n", r.signature, total);
            put(line);
        }
        for (unsigned k = 0; k < r.len; k++) {
            long off = (long)r.offset + (long)k;
            if (off < at) continue;
            if (want > 0 && off >= at + want) continue;
            if ((off % 16) == 0) {
                snprintf(line, sizeof line, "\n  %04lx ", off);
                put(line);
            }
            snprintf(line, sizeof line, " %02x", r.data[k]);
            put(line);
            shown++;
        }
    }
    if (total < 0) {
        snprintf(line, sizeof line, "acpi: no table with signature %s\n", sig);
        put(line);
        return 1;
    }
    put("\n");
    if (!shown) put("acpi: that range is past the end of the table\n");
    return 0;
}

// Did this boot ask for smp=park? The LAST smp= on the line, as the
// kernel reads it.
static int booted_parked(void) {
    struct query_cmdline q;
    int parts = 0;
    QUERY_FOREACH(QUERY_CMDLINE, q, i) parts++;
    char *cl = malloc((size_t)parts * QUERY_CMDLINE_PART + 1);
    if (!cl) return 0;
    char v[8];
    int parked = ubootcfg_cmdline(cl, parts * QUERY_CMDLINE_PART + 1) >= 0 &&
                 ubootcfg_value_in(cl, "smp=", v, sizeof v) && !strcmp(v, "park");
    free(cl);
    return parked;
}

// uACPI, the AML interpreter: off unless the boot asked (acpi=uacpi).
static void put_interpreter(const struct query_acpi *a) {
    char line[160];
    switch (a->uacpi_state) {
        case 1:
            put("AML interpreter: loading (kacpid)\n");
            return;
        case 2:
            snprintf(line, sizeof line,
                     "AML interpreter: uACPI, %llu namespace nodes, %llu devices, loaded in %llu ms\n",
                     (unsigned long long)a->uacpi_nodes, (unsigned long long)a->uacpi_devices,
                     (unsigned long long)(a->uacpi_load_us / 1000));
            break;
        case 3:
            snprintf(line, sizeof line, "AML interpreter: uACPI failed at %s (status %lld)\n",
                     a->uacpi_failed_at[0] ? a->uacpi_failed_at : "?", (long long)a->uacpi_status);
            break;
        default:
            put("AML interpreter: off  (the boot word acpi=uacpi starts it)\n");
            return;
    }
    put(line);
}

// The embedded controller the interpreter serves (uacpi_ec.c, stage 4b):
// nothing when there is none.
static void put_ec(void) {
    struct query_acpi_ec e;
    char line[200];
    if (sys_query_record(QUERY_ACPI_EC, 0, &e, sizeof e) < (int)sizeof e || !e.via) return;
    e.path[sizeof e.path - 1] = 0;
    const char *ev = e.events == 1 ? "events on" : e.events == 2 ? "events OFF after a storm"
                                                                 : "events off";
    if (e.gpe >= 0)
        snprintf(line, sizeof line, "Embedded ctrl:   %s (%s), ports 0x%llx/0x%llx, GPE 0x%02llx, %s\n",
                 e.path[0] ? e.path : "?", e.via == 1 ? "ECDT" : "_CRS", (unsigned long long)e.cmd,
                 (unsigned long long)e.data, (unsigned long long)e.gpe, ev);
    else
        snprintf(line, sizeof line, "Embedded ctrl:   %s (%s), ports 0x%llx/0x%llx, no GPE\n",
                 e.path[0] ? e.path : "?", e.via == 1 ? "ECDT" : "_CRS", (unsigned long long)e.cmd,
                 (unsigned long long)e.data);
    put(line);
    snprintf(line, sizeof line,
             "                 %llu access(es), %llu timed out, longest wait %llu us, %llu nap(s); "
             "%llu event(s) of %llu kind(s), last 0x%02llx\n",
             (unsigned long long)e.transactions, (unsigned long long)e.timeouts,
             (unsigned long long)(e.longest_wait_ns / 1000), (unsigned long long)e.naps,
             (unsigned long long)e.queries,
             (unsigned long long)e.distinct, (unsigned long long)e.last_query);
    put(line);
}

// Wh with one decimal, from mWh; "?" for unknown.
static void wh(uint64_t mwh, char *out, unsigned cap) {
    if (mwh == QUERY_POWER_UNKNOWN) snprintf(out, cap, "?");
    else snprintf(out, cap, "%llu.%llu", (unsigned long long)(mwh / 1000), (unsigned long long)(mwh % 1000 / 100));
}

// The batteries, the adapter and the lid (uacpi_supply.c, stage 4e).
static void put_supplies(void) {
    struct query_power pw;
    char line[200], a[16], b[16], c[16], r[16];
    if (sys_query_record(QUERY_POWER, 0, &pw, sizeof pw) < (int)sizeof pw) return;
    struct query_power_supply s;
    QUERY_FOREACH(QUERY_POWER_SUPPLY, s, i) {
        s.name[sizeof s.name - 1] = 0;
        if (s.type == QUERY_SUPPLY_MAINS) {
            snprintf(line, sizeof line, "  AC adapter:    %s  (%s)\n",
                     s.flags & QUERY_SUPPLY_ONLINE ? "plugged in" : "unplugged", s.name);
            put(line);
            continue;
        }
        if (!(s.flags & QUERY_SUPPLY_PRESENT)) {
            snprintf(line, sizeof line, "  battery %-5s  the bay is empty\n", s.name);
            put(line);
            continue;
        }
        wh(s.now_mwh, a, sizeof a);
        wh(s.full_mwh, b, sizeof b);
        wh(s.design_mwh, c, sizeof c);
        wh(s.rate_mw, r, sizeof r);
        const char *st = s.flags & QUERY_SUPPLY_CHARGING ? "charging at" : s.flags & QUERY_SUPPLY_DISCHARGING
                       ? "discharging at" : "idle,";
        char pct[8] = "?";
        if (s.percent >= 0) snprintf(pct, sizeof pct, "%lld%%", (long long)s.percent);
        snprintf(line, sizeof line, "  battery %-5s  %s, %s %s W, %s of %s Wh (design %s Wh)%s\n", s.name, pct, st,
                 r, a, b, c, s.flags & QUERY_SUPPLY_CRITICAL ? ", CRITICAL" : "");
        put(line);
        char cyc[24] = "";
        if (s.cycles != QUERY_POWER_UNKNOWN) snprintf(cyc, sizeof cyc, ", %llu cycles", (unsigned long long)s.cycles);
        snprintf(line, sizeof line, "                 %s %s, serial %s, %s%s%s\n", s.maker, s.model,
                 s.serial[0] ? s.serial : "?", s.kind[0] ? s.kind : "?", cyc,
                 s.flags & QUERY_SUPPLY_MAH ? ", counted in mAh" : "");
        put(line);
    }
    if (pw.flags & QUERY_POWER_LID_PRESENT) {
        snprintf(line, sizeof line, "  lid:           %s, %llu change(s) since boot\n",
                 pw.flags & QUERY_POWER_LID_OPEN ? "open" : "closed", (unsigned long long)pw.lid_changes);
        put(line);
    }
    if (pw.flags & QUERY_POWER_BATTERY) {
        snprintf(line, sizeof line, "  on battery:    %s; shuts down at %llu%%%s\n",
                 yesno(pw.flags & QUERY_POWER_ON_BATTERY), (unsigned long long)pw.critical_pct,
                 pw.flags & QUERY_POWER_SHUTDOWN_SENT ? " (sent)" : "");
        put(line);
    }
}

// Whole degrees C, or "?" for none.
static void deg(int64_t mc, char *out, unsigned cap) {
    if (mc == QUERY_THERMAL_NONE) snprintf(out, cap, "?");
    else snprintf(out, cap, "%lld", (long long)(mc >= 0 ? mc + 500 : mc - 500) / 1000);
}

// The thermal zones (uacpi_thermal.c) and the CPU's sensors (cputherm.c).
static void put_thermal(void) {
    struct query_thermal t;
    char line[220], now[12], hi[12], crit[12], psv[12];
    int any = 0;
    QUERY_FOREACH(QUERY_THERMAL, t, i) {
        if (!any++) put("\nThermal:\n");
        t.name[sizeof t.name - 1] = 0;
        t.path[sizeof t.path - 1] = 0;
        t.why[sizeof t.why - 1] = 0;
        deg(t.temp_mc, now, sizeof now);
        deg(t.max_mc, hi, sizeof hi);
        deg(t.critical_mc, crit, sizeof crit);
        deg(t.passive_mc, psv, sizeof psv);
        if (t.kind == QUERY_THERMAL_ZONE) {
            snprintf(line, sizeof line, "  %-12s %4s C  highest %s C  critical %s C  passive %s C\n",
                     t.path, now, hi, crit, psv);
            put(line);
            if (t.flags & QUERY_THERMAL_PASSIVE) {
                snprintf(line, sizeof line, "               slowing the CPUs to %llu%%%s%s\n",
                         (unsigned long long)t.perf_pct, t.flags & QUERY_THERMAL_APPLIED ? "" : " -- not applied: ",
                         t.flags & QUERY_THERMAL_APPLIED ? "" : t.why);
                put(line);
            }
            if (t.flags & QUERY_THERMAL_SHUTDOWN) put("               at its critical trip: shutdown sent\n");
        } else {
            snprintf(line, sizeof line, "  %-12s %4s C  highest %s C  TjMax %s C\n", t.name, now, hi, crit);
            put(line);
        }
    }
}

// --wake: every device whose _PRW names a wake event (/proc/acpi/wakeup).
static int wake_cmd(void) {
    struct query_acpi_wake w;
    char line[160];
    int n = 0;
    QUERY_FOREACH(QUERY_ACPI_WAKE, w, i) {
        if (!n++) put("Device                          HID       GPE   wakes from  at power-off\n");
        w.path[sizeof w.path - 1] = 0;
        w.hid[sizeof w.hid - 1] = 0;
        char gpe[8] = "-";
        if (!(w.flags & (QUERY_WAKE_UNREAD | QUERY_WAKE_GPE_BLOCK)))
            snprintf(gpe, sizeof gpe, "0x%02llx", (unsigned long long)w.gpe);
        else if (w.flags & QUERY_WAKE_GPE_BLOCK)
            snprintf(gpe, sizeof gpe, "block");
        char st[8] = "?";
        if (!(w.flags & QUERY_WAKE_UNREAD)) snprintf(st, sizeof st, "S%llu", (unsigned long long)w.sleep_state);
        snprintf(line, sizeof line, "%-31s %-9s %-5s %-11s %s\n", w.path, w.hid[0] ? w.hid : "-", gpe, st,
                 w.flags & QUERY_WAKE_ARMED_S5 ? "armed" : "-");
        put(line);
    }
    if (!n) put("no device names a wake event (or no AML interpreter: acpi=uacpi)\n");
    return 0;
}

// --eval PATH [INT...]: one AML object (SYS_ACPI_EVAL, `acpieval` boots).
static int eval_cmd(int argc, char **argv) {
    struct acpi_eval_msg m;
    memset(&m, 0, sizeof m);
    if (argc < 3 || strlen(argv[2]) >= sizeof m.path || argc - 3 > ACPI_EVAL_ARGS_MAX) {
        cmd_usage(USAGE);
        return 1;
    }
    snprintf(m.path, sizeof m.path, "%s", argv[2]);
    for (int i = 3; i < argc; i++) {
        char *end = 0;
        m.args[m.nargs++] = strtoull(argv[i], &end, 0);
        if (!end || *end || end == argv[i]) { cmd_usage(USAGE); return 1; }
    }
    if (sys_acpi_eval(&m) != 0) {
        int e = sys_errno();
        if (e == EPERM) cmd_fail_msg("acpi", m.path, "refused -- the kernel was not booted with acpieval");
        else cmd_fail_err("acpi", m.path, e);
        return 1;
    }
    char line[96];
    if (m.type == ACPI_EVAL_INTEGER)
        snprintf(line, sizeof line, "%llu (0x%llx)\n", (unsigned long long)m.value, (unsigned long long)m.value);
    else
        snprintf(line, sizeof line, "%s\n", m.type == ACPI_EVAL_NONE ? "(no value)" : "(not an Integer)");
    put(line);
    return 0;
}

int main(int argc, char **argv) {
    char line[160];
    struct query_acpi a;

    // --dump takes over entirely: it is a different question from the
    // summary, and mixing them would make the output unpasteable.
    if (argc > 1 && !strcmp(argv[1], "--dump")) {
        long at = 0, want = 0;
        if (argc < 3) { cmd_usage(USAGE); return 1; }
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--at") && i + 1 < argc) {
                if (!hex_arg(argv[++i], &at)) { cmd_usage(USAGE); return 1; }
            } else if (!strcmp(argv[i], "--len") && i + 1 < argc) {
                if (!hex_arg(argv[++i], &want)) { cmd_usage(USAGE); return 1; }
            } else {
                cmd_usage(USAGE);
                return 1;
            }
        }
        return dump_table(argv[2], at, want);
    }
    if (argc > 1 && !strcmp(argv[1], "--eval")) return eval_cmd(argc, argv);
    if (argc == 2 && !strcmp(argv[1], "--wake")) return wake_cmd();
    if (argc > 1) { cmd_usage(USAGE); return 1; }

    if (sys_query_record(QUERY_ACPI, 0, &a, sizeof a) < (int)sizeof a) {
        cmd_fail("acpi", 0);
        return 1;
    }

    if (!(a.flags & ACPI_F_TABLES)) {
        // Not an error. A machine can genuinely have no ACPI, and this
        // is exactly the boot where `reboot --poweroff` will halt
        // instead of powering down -- so say both things.
        put("no ACPI tables were found on this machine\n");
        snprintf(line, sizeof line, "RSDP:            %s\n",
                 source_name(a.rsdp_source));
        put(line);
        put("poweroff will fall back to the legacy port write, then halt\n");
        return 0;
    }

    snprintf(line, sizeof line, "RSDP:            revision %llu, from the %s\n",
             (unsigned long long)a.rsdp_revision, source_name(a.rsdp_source));
    put(line);
    snprintf(line, sizeof line, "Root table:      %s at 0x%llx, %llu table(s)\n",
             (a.flags & ACPI_F_XSDT) ? "XSDT" : "RSDT",
             (unsigned long long)a.rsdt_phys, (unsigned long long)a.table_count);
    put(line);
    snprintf(line, sizeof line, "ACPI mode:       %s%s%s\n",
             yesno(a.flags & ACPI_F_ENABLED),
             (a.flags & ACPI_F_HW_REDUCED) ? "  (hardware-reduced platform)" : "",
             (a.flags & ACPI_F_FW_LEGACY) ? "  (firmware booted in legacy mode)" : "");
    put(line);
    put_interpreter(&a);
    put_ec();

    put("\nPower:\n");
    snprintf(line, sizeof line, "  poweroff:      %s",
             yesno(a.flags & (ACPI_F_S5 | ACPI_F_S5_UACPI)));
    put(line);
    if (a.flags & ACPI_F_S5_UACPI) {
        put("  -- through uACPI, after the firmware's _PTS(5)\n");
    } else if (a.flags & ACPI_F_S5) {
        if (a.flags & ACPI_F_HW_REDUCED)
            snprintf(line, sizeof line, "  -- S5 type %llu to SLEEP_CONTROL_REG 0x%llx\n",
                     (unsigned long long)a.slp_typ_a,
                     (unsigned long long)a.sleep_control_addr);
        else
            snprintf(line, sizeof line, "  -- S5 type %llu to PM1a port 0x%llx%s\n",
                     (unsigned long long)a.slp_typ_a,
                     (unsigned long long)a.pm1a_cnt,
                     a.pm1b_cnt ? " (and PM1b)" : "");
        put(line);
    } else {
        put("  -- no usable _S5_, the legacy port write is what runs\n");
    }

    snprintf(line, sizeof line, "  reset:         %s", yesno(a.flags & ACPI_F_RESET));
    put(line);
    if (a.flags & ACPI_F_RESET) {
        snprintf(line, sizeof line, "  -- value 0x%llx to %s 0x%llx\n",
                 (unsigned long long)a.reset_value, space_name(a.reset_space),
                 (unsigned long long)a.reset_addr);
        put(line);
    } else {
        put("  -- no reset register, the 8042 pulse is what runs\n");
    }
    snprintf(line, sizeof line, "  power button:  %s", yesno(a.flags & ACPI_F_PWRBTN));
    put(line);
    if (a.flags & ACPI_F_PWRBTN)
        snprintf(line, sizeof line, "  -- SCI %llu%s, %llu press(es) since boot\n",
                 (unsigned long long)a.sci_int,
                 (a.flags & ACPI_F_PWRBTN_UACPI) ? ", uACPI's fixed event" : "",
                 (unsigned long long)a.pwrbtn_presses);
    else if (a.flags & ACPI_F_EC_LEGACY)
        snprintf(line, sizeof line, "  -- left to the firmware: its embedded controller needs AML\n");
    else if (a.flags & ACPI_F_PWRBTN_CM)
        snprintf(line, sizeof line, "  -- a control-method button, which needs AML\n");
    else
        snprintf(line, sizeof line, "  -- not armed (see dmesg)\n");
    put(line);
    if (a.smi_cmd) {
        snprintf(line, sizeof line, "  SMI command:   port 0x%llx, enable 0x%llx\n",
                 (unsigned long long)a.smi_cmd, (unsigned long long)a.acpi_enable);
        put(line);
    }
    put_supplies();
    put_thermal();

    put("\nTables:\n");
    struct query_acpi_table t;
    QUERY_FOREACH(QUERY_ACPI_TABLE, t, i) {
        snprintf(line, sizeof line, "  %-5s 0x%08llx  %6llu bytes  rev %-3llu  %s %s\n",
                 t.signature, (unsigned long long)t.address,
                 (unsigned long long)t.length, (unsigned long long)t.revision,
                 t.oem_id, t.oem_table_id);
        put(line);
    }

    // The MADT's processors, if there was a MADT, and which the kernel
    // started; `scheduling`, which run processes (all online ones unless smp=park).
    put("\nProcessors (MADT):\n");
    int n = 0, online = 0, scheduling = 0;
    struct query_cpu c;
    QUERY_FOREACH(QUERY_CPUS, c, i) {
        n++;
        if (c.flags & QUERY_CPU_ONLINE) online++;
        if (c.flags & QUERY_CPU_SCHEDULING) scheduling++;
        snprintf(line, sizeof line, "  cpu%-3u  %s id %-4llu  acpi id %-4llu  %s  online: %s\n",
                 i, (c.flags & QUERY_CPU_X2APIC) ? "x2apic" : "apic  ",
                 (unsigned long long)c.apic_id, (unsigned long long)c.acpi_id,
                 (c.flags & QUERY_CPU_ENABLED) ? "enabled "
                   : (c.flags & QUERY_CPU_ONLINE_CAPABLE) ? "offline " : "disabled",
                 (c.flags & QUERY_CPU_ONLINE) ? "yes" : "no");
        put(line);
    }
    if (n == 0) {
        put("  none listed -- no MADT on this machine\n");
    } else {
        snprintf(line, sizeof line,
                 "  %d listed, %d online, %llu I/O APIC(s), local APIC at 0x%llx\n",
                 n, online, (unsigned long long)a.ioapic_count,
                 (unsigned long long)a.lapic_phys);
        put(line);
        // smp=park is one reason, not the only one: under smp=run a core
        // with no LAPIC timer, or one that never took the start call, stays
        // parked too -- and the kernel log says which.
        if (online > 1 && scheduling < online) {
            snprintf(line, sizeof line, "  Processes run on %d of them; the others are parked %s\n",
                     scheduling ? scheduling : 1,
                     booted_parked() ? "(smp=park)."
                                             : "-- the kernel could not start them scheduling "
                                               "(`dmesg`, its smp: lines, says why).");
            put(line);
        }
    }

    return 0;
}
