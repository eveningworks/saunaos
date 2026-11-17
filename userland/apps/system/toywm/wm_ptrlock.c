// A window's POINTER LOCK: the pointer held still and its motion handed
// to that window as deltas (WIN_EV_MOUSE_REL) -- Wayland's
// zwp_locked_pointer_v1 with zwp_relative_pointer_v1, for a game that
// steers by motion. The holding is the KERNEL's (WIN_REQ_RAW_MOTION):
// a relative device's motion becomes a delta instead of moving the
// pointer, so a click lands where the lock is however fast the mouse
// moved, and an edge stops nothing.
//
// IN FORCE ONLY WHILE ITS WINDOW HAS THE FOCUS AND NONE OF THE
// COMPOSITOR'S OWN OVERLAYS IS UP. Alt+Tab, Super's Start menu, a flyout,
// the Leave page or a window opening over it suspends it with the pointer
// free -- those are not windows[], so the focus alone does not see them;
// the lock coming back resumes it -- the lock is the window's until it
// lets go or closes (Wayland's persistent lifetime). That is the way out a
// held pointer needs, the keyboard's, as on every desktop.
//
// AN ABSOLUTE DEVICE (a tablet) still moves the kernel's pointer; while
// the lock is in force the frame keeps reporting the lock point, so the
// locked window hears no position, only no motion.
#include "wm_internal.h"
#include "apps/system/toywm/wm_ptrlock.h"
#include "apps/system/toywm/wm_overlay.h"   // wm_overlay_any_open()
#include "rt/sys.h"
#include "win_proto.h"
#include "base/ulog.h"

// Held by open_seq, never index: a raise renumbers windows[].
static uint32_t g_seq;
static int g_active;
static int g_lx, g_ly;   // where it is held

static void hold(int on) {
    struct win_request_msg q = {0};
    q.type = WIN_REQ_RAW_MOTION;
    q.a = on ? 1 : 0;
    sys_win_request(&q);
}

static void warp(int x, int y) {
    struct win_request_msg q = {0};
    q.type = WIN_REQ_WARP_POINTER;
    q.a = x;
    q.b = y;
    sys_win_request(&q);
}

void wm_ptrlock_request(int idx, int on) {
    if (idx < 0 || idx >= window_count) return;
    if (!windows[idx].open_seq) windows[idx].open_seq = wm_next_open_seq();
    uint32_t seq = windows[idx].open_seq;
    if (on) {
        g_seq = seq;
    } else if (g_seq == seq) {
        g_seq = 0;
    }
    ulogf("wm: pointer lock %s by window %d\n", on ? "asked" : "released", idx);
}

void wm_ptrlock_frame(int *mx, int *my) {
    int idx = g_seq ? wm_window_by_seq(g_seq) : -1;
    if (g_seq && idx < 0) g_seq = 0;   // its window went away
    int want = idx >= 0 && idx == wm_focus_index() && windows[idx].state != WIN_MINIMIZED &&
               !wm_overlay_any_open();
    if (want && !g_active) {
        // Held where it is when that is over the content, else at the
        // content's centre -- a lock asked from the keyboard.
        const struct window *w = &windows[idx];
        int cx = window_content_x(w), cy = window_content_y(w);
        int cw = window_content_w(w), ch = window_content_h(w);
        if (*mx < cx || *my < cy || *mx >= cx + cw || *my >= cy + ch) {
            *mx = cx + cw / 2;
            *my = cy + ch / 2;
            warp(*mx, *my);
        }
        hold(1);
        g_active = 1;
        g_lx = *mx;
        g_ly = *my;
        ulogf("wm: pointer lock in force for window %d at %d,%d\n", idx, *mx, *my);
    } else if (!want && g_active) {
        hold(0);
        g_active = 0;
        ulogf("wm: pointer lock suspended\n");
    }
    if (g_active) {
        *mx = g_lx;
        *my = g_ly;
    }
}

void wm_ptrlock_motion(int dx, int dy) {
    int idx = g_active ? wm_window_by_seq(g_seq) : -1;
    if (idx < 0 || !wm_client_is_client_window(&windows[idx])) return;
    struct win_event ev = {0};
    ev.type = WIN_EV_MOUSE_REL;
    ev.window = windows[idx].client_win;
    ev.a = dx;
    ev.b = dy;
    wm_client_send_input(&windows[idx], &ev);
}
