// See wm_thermal.h. The hot notices.
#include "wm_internal.h"
#include "wm_thermal.h"
#include "crash_notice.h"
#include "base/ulog.h"
#include "base/usetting.h"
#include "apps/system/toywm/wm_conf.h"   // wm_setting_generation()
#include "kapi.h"
#include "rt/sys.h"
#include "query_abi.h"

#define TOO_HOT "The computer is too hot"
#define RUNNING_HOT "Running hot"
#define COOLED_MC 3000     // "too hot" ends once the zone is this far under where it began
#define SET_HOT "desktop.hot_notices"

// What is TRUE (a passive spell, a zone near critical) is kept apart from
// what is SHOWN (a card up), so the switch can go off and on mid-spell.
static int g_spell, g_near;            // the facts
static int g_hot_up, g_too_hot_up;     // the cards
static struct wm_thermal_debug g_dbg;
static uint32_t g_seen_setting = (uint32_t)-1;
static int g_enabled = 1;

// `desktop.hot_notices`, read again only when a setting changed. Off, the
// cards that are up go. "Running hot" is news about a moment, so it is not
// raised again when the switch comes back on; "too hot" is a state, and
// the poll raises it again while it is still true.
static void read_setting(void) {
    uint32_t gen = wm_setting_generation();
    if (gen == g_seen_setting) return;
    g_seen_setting = gen;
    char v[8] = "on";
    usetting_get(SET_HOT, v, sizeof v);
    g_enabled = k_strcmp(v, "off") != 0;
    if (g_enabled) return;
    if (g_hot_up) crash_notice_drop_info(RUNNING_HOT);
    if (g_too_hot_up) crash_notice_drop_info(TOO_HOT);
    g_hot_up = g_too_hot_up = 0;
}

static void open_taskmgr(void) {
    int pid = sys_spawn("/bin/wm/system/taskmgr", "", -1);
    if (pid > 0) wm_track_launched(pid);
}

static int whole_c(long long mc) { return (int)((mc >= 0 ? mc + 500 : mc - 500) / 1000); }

void wm_thermal_poll(void) {
    // The zone nearest its critical trip speaks for the machine; passive
    // cooling is any zone's.
    struct query_thermal t, hot;
    int zones = 0, slowing = 0, applied = 1;
    char why[sizeof t.why] = "";
    long long margin = 0;
    k_memset(&hot, 0, sizeof hot);
    // THE ZONES COME FIRST, then the CPU's sensors -- each of those an IPI
    // to its core, so the walk stops at the first.
    QUERY_FOREACH(QUERY_THERMAL, t, i) {
        if (t.kind != QUERY_THERMAL_ZONE) break;
        if (!(t.flags & QUERY_THERMAL_VALID)) continue;
        if (t.flags & QUERY_THERMAL_PASSIVE) {
            slowing = 1;
            if (!(t.flags & QUERY_THERMAL_APPLIED)) {
                applied = 0;
                t.why[sizeof t.why - 1] = 0;
                k_strlcpy(why, t.why, sizeof why);
            }
        }
        long long m = t.critical_mc == QUERY_THERMAL_NONE ? (1LL << 40) : t.critical_mc - t.temp_mc;
        if (!zones++ || m < margin) {
            margin = m;
            hot = t;
        }
    }
    read_setting();
    g_dbg.zones = zones;
    g_dbg.temp_mc = zones ? hot.temp_mc : QUERY_THERMAL_NONE;
    g_dbg.passive_mc = zones ? hot.passive_mc : QUERY_THERMAL_NONE;
    g_dbg.critical_mc = zones ? hot.critical_mc : QUERY_THERMAL_NONE;
    g_dbg.slowing = slowing;
    g_dbg.enabled = g_enabled;
    if (!zones) return;

    // "Running hot": once as a spell starts. Its card times out by itself.
    if (slowing && !g_spell) {
        g_spell = 1;
        if (g_enabled) {
            char sub[128];
            if (applied)
                k_snprintf(sub, sizeof sub, "The CPU reached %d °C and is slowed down to cool it.",
                           whole_c(hot.temp_mc));
            else
                k_snprintf(sub, sizeof sub, "The CPU reached %d °C and could not be slowed down (%s).",
                           whole_c(hot.temp_mc), why[0] ? why : "no frequency control");
            crash_notice_info_act("Power", RUNNING_HOT, sub, "tb-chart", "Task Manager", open_taskmgr);
            g_hot_up = 1;
            ulogf("wm: running hot (%d C) -- passive cooling, the notice is up\n", whole_c(hot.temp_mc));
        }
    } else if (!slowing) {
        g_spell = g_hot_up = 0;
    }

    // "Too hot": up while the zone is near critical, down once it cools.
    if (hot.critical_mc != QUERY_THERMAL_NONE) {
        if (hot.temp_mc + QUERY_THERMAL_WARN_MC >= hot.critical_mc) {
            g_near = 1;
        } else if (g_near && hot.temp_mc + QUERY_THERMAL_WARN_MC + COOLED_MC < hot.critical_mc) {
            g_near = 0;
            if (g_too_hot_up) {
                crash_notice_drop_info(TOO_HOT);
                g_too_hot_up = 0;
                ulogf("wm: cooled to %d C -- the too-hot notice is down\n", whole_c(hot.temp_mc));
            }
        }
        if (g_near && g_enabled && !g_too_hot_up) {
            char sub[128];
            k_snprintf(sub, sizeof sub, "It is at %d °C and shuts down at %d °C to protect itself. Save your work.",
                       whole_c(hot.temp_mc), whole_c(hot.critical_mc));
            crash_notice_info_from("Power", TOO_HOT, sub, "tb-chart", 0, 1);
            g_too_hot_up = 1;
            ulogf("wm: too hot (%d C of %d C) -- the notice is up\n", whole_c(hot.temp_mc),
                  whole_c(hot.critical_mc));
        }
    }
    g_dbg.near_critical = g_near;
    g_dbg.hot_notice = g_hot_up;
    g_dbg.too_hot_notice = g_too_hot_up;
}

void wm_thermal_debug(struct wm_thermal_debug *d) { *d = g_dbg; }
