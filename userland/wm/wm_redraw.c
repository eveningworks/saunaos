// WHO ASKED FOR EACH FULL-SCREEN FRAME. Every `redraw_pending` in toywm
// is set through WM_REDRAW() (wm_internal.h), which notes the site; when
// wm_render_frame() finds no damage reported, the frame is a full-screen
// repaint, and it is charged to every site that asked since the last
// frame. `gui frames` lists them, worst first -- the instrument for
// docs/roadmap.md's "Full repaints where a small rect would do", the
// shape perf and ETW give a profile: the cost, attributed to a line.
//
// A SITE IS A LINE OF CODE, so the table is bounded by the source, not
// by anything a user does; it still says so if it ever fills.
#include "wm_internal.h"
#include "wm_debug.h"
#include "ui/ulog.h"

#define SITES_MAX   256   // WM_REDRAW() call sites; ~170 when this was written
#define PENDING_MAX 32    // distinct sites asking within one frame

struct site { const char *file; int line; unsigned asked, full; };
static struct site g_site[SITES_MAX];
static int g_nsites;
static int g_pending[PENDING_MAX];
static int g_npending, g_pending_lost;
static unsigned g_frames_full, g_frames_partial;

static int site_of(const char *file, int line) {
    for (int i = 0; i < g_nsites; i++)
        if (g_site[i].line == line && g_site[i].file == file) return i;
    if (g_nsites == SITES_MAX) {
        static int said;
        if (!said) { said = 1; ulog("wm: gui frames' site table is full\n"); }
        return -1;
    }
    g_site[g_nsites].file = file;
    g_site[g_nsites].line = line;
    return g_nsites++;
}

void wm_redraw_at(const char *file, int line) {
    redraw_pending = 1;
    int s = site_of(file, line);
    if (s < 0) return;
    for (int i = 0; i < g_npending; i++)
        if (g_pending[i] == s) return;
    if (g_npending < PENDING_MAX) g_pending[g_npending++] = s;
    else g_pending_lost = 1;
}

void wm_redraw_charge(int has_damage) {
    for (int i = 0; i < g_npending; i++) {
        g_site[g_pending[i]].asked++;
        if (!has_damage) g_site[g_pending[i]].full++;
    }
    if (has_damage) g_frames_partial++;
    else g_frames_full++;
    g_npending = 0;
}

// `gui frames`: the sites by full-screen frames charged, then by asks.
void wm_redraw_report(struct dbg_out *o, int reset) {
    if (reset) {
        for (int i = 0; i < g_nsites; i++) g_site[i].asked = g_site[i].full = 0;
        g_frames_full = g_frames_partial = 0;
        g_pending_lost = 0;
        dbg_out_write(o, "gui: frame causes reset\r\n");
        return;
    }
    dbg_out_printf(o, "frames: %u full-screen, %u damage-limited%s\r\n", g_frames_full,
                   g_frames_partial, g_pending_lost ? " (some frames had too many askers)" : "");
    dbg_out_write(o, "   full  asked  site\r\n");
    // Selection by the largest remaining, a handful of lines: no sort
    // routine for a debug command.
    static unsigned char shown[SITES_MAX];
    for (int i = 0; i < g_nsites; i++) shown[i] = 0;
    for (;;) {
        int best = -1;
        for (int i = 0; i < g_nsites; i++) {
            if (shown[i] || !g_site[i].asked) continue;
            if (best < 0 || g_site[i].full > g_site[best].full ||
                (g_site[i].full == g_site[best].full && g_site[i].asked > g_site[best].asked))
                best = i;
        }
        if (best < 0) break;
        shown[best] = 1;
        const char *f = g_site[best].file, *slash = f;
        for (const char *c = f; *c; c++) if (*c == '/') slash = c + 1;
        dbg_out_printf(o, "  %5u  %5u  %s:%d\r\n", g_site[best].full, g_site[best].asked,
                       slash, g_site[best].line);
    }
}
