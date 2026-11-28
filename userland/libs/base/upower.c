// The batteries and charger as one answer -- see upower.h.
#include "base/upower.h"
#include "rt/sys.h"
#include <stdio.h>
#include <string.h>

uint64_t upower_generation(void) {
    struct query_power q;
    if (sys_query_record(QUERY_POWER, 0, &q, sizeof q) < (int)sizeof q) return 0;
    return q.generation;
}

static uint64_t known(uint64_t v) { return v == QUERY_POWER_UNKNOWN ? 0 : v; }

int upower_read(struct upower *out) {
    memset(out, 0, sizeof *out);
    out->percent = -1;
    out->seconds = -1;
    struct query_power q;
    if (sys_query_record(QUERY_POWER, 0, &q, sizeof q) < (int)sizeof q) return -1;
    out->generation = q.generation;
    out->flags = q.flags;
    out->critical_pct = (int)q.critical_pct;
    struct query_power_supply s;
    QUERY_FOREACH(QUERY_POWER_SUPPLY, s, i) {
        if (s.type != QUERY_SUPPLY_BATTERY || !(s.flags & QUERY_SUPPLY_PRESENT)) continue;
        out->batteries++;
        if (s.flags & QUERY_SUPPLY_CHARGING) out->charging = 1;
        if (s.flags & QUERY_SUPPLY_DISCHARGING) out->discharging = 1;
        // A battery whose charge is unknown adds nothing: its capacity alone
        // would read as 0% (the kernel's own sum skips it the same way).
        if (s.now_mwh == QUERY_POWER_UNKNOWN) continue;
        uint64_t full = known(s.full_mwh) ? s.full_mwh : known(s.design_mwh);
        out->now_mwh += known(s.now_mwh);
        out->full_mwh += full;
        out->design_mwh += known(s.design_mwh);
        out->rate_mw += known(s.rate_mw);
    }
    if (out->full_mwh) {
        uint64_t p = (out->now_mwh * 100 + out->full_mwh / 2) / out->full_mwh;
        out->percent = p > 100 ? 100 : (int)p;
    }
    out->plugged = !(q.flags & QUERY_POWER_ON_BATTERY);
    // Instantaneous, as Linux's energy_now / power_now: a rate of 0 (the
    // firmware's "not drawing", or unknown) gives no estimate.
    if (out->rate_mw && out->discharging)
        out->seconds = (int64_t)(out->now_mwh * 3600 / out->rate_mw);
    else if (out->rate_mw && out->charging && out->full_mwh > out->now_mwh)
        out->seconds = (int64_t)((out->full_mwh - out->now_mwh) * 3600 / out->rate_mw);
    return 0;
}

void upower_duration(int64_t seconds, char *out, unsigned cap) {
    if (!cap) return;
    out[0] = 0;
    if (seconds < 0) return;
    int64_t min = seconds / 60;
    if (min < 1) snprintf(out, cap, "under a minute");
    else if (min < 60) snprintf(out, cap, "%lld min", (long long)min);
    else snprintf(out, cap, "%lld h %lld min", (long long)(min / 60), (long long)(min % 60));
}

const char *upower_state(const struct upower *p) {
    if (p->charging) return "Charging";
    if (p->discharging || !p->plugged) return "On battery";
    return p->percent >= 99 ? "Fully charged" : "Plugged in";
}

void upower_summary(const struct upower *p, char *out, unsigned cap) {
    if (!cap) return;
    out[0] = 0;
    if (!p->batteries) return;
    char pct[8] = "?";
    if (p->percent >= 0) snprintf(pct, sizeof pct, "%d%%", p->percent);
    char t[32];
    upower_duration(p->seconds, t, sizeof t);
    if (p->charging && t[0]) snprintf(out, cap, "%s \xC2\xB7 charging, %s to full", pct, t);
    else if (p->charging) snprintf(out, cap, "%s \xC2\xB7 charging", pct);
    else if (p->discharging && t[0]) snprintf(out, cap, "%s \xC2\xB7 %s left", pct, t);
    else if (p->plugged) snprintf(out, cap, "%s \xC2\xB7 plugged in", pct);
    else snprintf(out, cap, "%s", pct);
}
