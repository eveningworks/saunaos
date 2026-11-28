// See wm_thermal.h. The hot notices.
#include "wm_internal.h"
#include "wm_thermal.h"
#include "crash_notice.h"
#include "base/ulog.h"
#include "kapi.h"
#include "rt/sys.h"
#include "query_abi.h"

#define TOO_HOT "The computer is too hot"
#define COOLED_MC 3000     // N2 goes once the zone is this far under where it came

static int g_spell;        // "Running hot" went up this passive spell
static int g_near;         // "too hot" is up
static struct wm_thermal_debug g_dbg;

static void open_taskmgr(void) {
    int pid = sys_spawn("/bin/wm/system/taskmgr", "", -1);
    if (pid > 0) wm_track_launched(pid);
}

static int whole_c(long long mc) { return (int)((mc >= 0 ? mc + 500 : mc - 500) / 1000); }

void wm_thermal_poll(void) {
    // The zone nearest its critical trip speaks for the machine; passive
    // cooling is any zone's.
    struct query_thermal t, hot;
    int zones = 0, slowing = 0, applied = 1, near = 0;
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
    g_dbg.zones = zones;
    g_dbg.temp_mc = zones ? hot.temp_mc : QUERY_THERMAL_NONE;
    g_dbg.passive_mc = zones ? hot.passive_mc : QUERY_THERMAL_NONE;
    g_dbg.critical_mc = zones ? hot.critical_mc : QUERY_THERMAL_NONE;
    g_dbg.slowing = slowing;
    if (!zones) return;

    if (slowing && !g_spell) {
        g_spell = 1;
        char sub[128];
        if (applied)
            k_snprintf(sub, sizeof sub, "The CPU reached %d °C and is slowed down to cool it.", whole_c(hot.temp_mc));
        else
            k_snprintf(sub, sizeof sub, "The CPU reached %d °C and could not be slowed down (%s).",
                       whole_c(hot.temp_mc), why[0] ? why : "no frequency control");
        crash_notice_info_act("Power", "Running hot", sub, "tb-chart", "Task Manager", open_taskmgr);
        ulogf("wm: running hot (%d C) -- passive cooling, the notice is up\n", whole_c(hot.temp_mc));
    } else if (!slowing) {
        g_spell = 0;
    }

    if (hot.critical_mc != QUERY_THERMAL_NONE) {
        near = hot.temp_mc + QUERY_THERMAL_WARN_MC >= hot.critical_mc;
        if (near && !g_near) {
            g_near = 1;
            char sub[128];
            k_snprintf(sub, sizeof sub, "It is at %d °C and shuts down at %d °C to protect itself. Save your work.",
                       whole_c(hot.temp_mc), whole_c(hot.critical_mc));
            crash_notice_info_from("Power", TOO_HOT, sub, "tb-chart", 0, 1);
            ulogf("wm: too hot (%d C of %d C) -- the notice is up\n", whole_c(hot.temp_mc),
                  whole_c(hot.critical_mc));
        } else if (g_near && hot.temp_mc + QUERY_THERMAL_WARN_MC + COOLED_MC < hot.critical_mc) {
            g_near = 0;
            crash_notice_drop_info(TOO_HOT);
            ulogf("wm: cooled to %d C -- the too-hot notice is down\n", whole_c(hot.temp_mc));
        }
    }
    g_dbg.near_critical = near;
    g_dbg.hot_notice = g_spell;
    g_dbg.too_hot_notice = g_near;
}

void wm_thermal_debug(struct wm_thermal_debug *d) { *d = g_dbg; }
