// Drag-and-drop BETWEEN windows: the compositor's half.
//
// A client's toolkit runs its own drag session inside its window
// (ui/uui_route.h's third rule) and cannot see past its edges. So when
// one starts it says so (WIN_REQ_DRAG_START), having put the files in
// the clipboard page's DRAG SLOT (lib/uclip.h), and this file offers
// the drag to whatever the held pointer is over that is NOT the source:
// WIN_EV_DRAG_OVER while it hovers, DRAG_LEAVE when it moves on, DROP
// on the release. The desktop is a target too (files land in
// /home/Desktop) and a source (a file icon dragged onto a window).
//
// This is Wayland's wl_data_device shape -- the compositor brokers,
// the data rides shared memory, the source and target never meet --
// minus the type negotiation, since there is one kind. X11's XDND does
// it with client messages between the two windows and a selection,
// which is the design every compositor since has moved away from.
//
// THE SOURCE KEEPS ITS OWN SESSION: it still receives MOUSE_MOVE and
// MOUSE_UP with the button held (content_pressed), so a drop back in
// its own window is its own toolkit's, and a drop elsewhere reaches it
// only as a release nothing accepted -- the file having moved anyway.
//
// THE DRAG IMAGE IS THIS FILE'S, EVERYWHERE -- the source's window
// included, where its toolkit stops drawing its own (uapp.c, an
// exported drag): the items' icon, translucent, a count for several,
// and under it what a release would do. Windows 11's drag image; a
// client-drawn ghost stopped at its window's edge and had to be
// repainted by the client on every move. What a release would do comes
// from the window under the pointer (WIN_REQ_DRAG_FEEDBACK, as Wayland's
// wl_data_offer.accept), from the desktop's own label, or from this
// file for a title bar and the desktop's background.
#include "wm_internal.h"
#include "wm_dnd.h"
#include "wm_rawin.h"
#include "desktop.h"
#include "ui/uui.h"
#include "ui/utheme.h"
#include "lib/uclip.h"
#include "lib/icon_cache.h"
#include "lib/ufiletype.h"
#include "ui/uui_primitives.h"
#include "kpath.h"
#include "kapi.h"
#include "rt/sys.h"

static int g_active;
static int g_count;
static int g_src_pid;      // the dragging client, 0 for the desktop
static int g_over;         // window index under the pointer, or -1
static int g_took_drop;    // set on the tick a drop went to a window
// The image: the first item's type icon, its alpha cut to 80% once at
// the start, so every frame is a plain alpha blit.
#define ICON_PX 48
static uint32_t g_icon[ICON_PX * ICON_PX];
static int g_icon_w, g_icon_h;
// What a release here would do, as the window under the pointer last
// said: whose answer it is (0: none yet), and its words.
static int g_fb_pid, g_fb_accepted;
static char g_fb_text[WIN_TITLE_LEN];
static char g_chip[96];    // the line drawn under the icon, "" for none
static int g_chip_box[4];  // where it was last laid out, for `gui dnd`
// The image as last DAMAGED, so the next change repaints its old box
// and its new one and nothing else.
static int g_ghost_on, g_gx, g_gy, g_gw, g_gh;
// What the target window was last told, so a held still pointer sends
// it nothing: each DRAG_OVER costs it a page copy and a repaint.
static int g_sent_x, g_sent_y;
static unsigned g_sent_mods;

int wm_dnd_active(void) { return g_active; }
int wm_dnd_took_drop(void) { return g_took_drop; }

// The first item's icon from the drag slot the source just filled.
static void load_icon(void) {
    static struct uclip c;   // 64 KiB, lib/uclip.h says why static
    g_icon_w = g_icon_h = 0;
    if (!uclip_drag_load(&c) || uclip_count(&c) <= 0) return;
    const char *first = uclip_path(&c, 0);
    struct sys_stat st;
    int is_dir = sys_stat(first, &st) == 0 && st.is_dir;
    const struct uimg *ico = icon_get(ufiletype_icon(k_path_basename(first), is_dir), ICON_PX);
    if (!ico || ico->w > ICON_PX || ico->h > ICON_PX) return;
    for (int i = 0; i < ico->w * ico->h; i++) {
        uint32_t p = ico->px[i];
        g_icon[i] = (p & 0x00FFFFFFu) | ((((p >> 24) * 204u) / 255u) << 24);
    }
    g_icon_w = ico->w;
    g_icon_h = ico->h;
}

static void begin(int pid, int count) {
    g_active = 1;
    g_count = count;
    g_src_pid = pid;
    g_over = -1;
    g_took_drop = 0;
    g_ghost_on = 0;
    g_sent_x = g_sent_y = -1;
    g_fb_pid = 0;
    g_chip[0] = '\0';
    load_icon();
}

void wm_dnd_feedback(int pid, int accepted, int copy, const char *where) {
    (void)copy;   // `where` already says Move or Copy
    if (!g_active) return;
    g_fb_pid = pid;
    g_fb_accepted = accepted;
    k_strlcpy(g_fb_text, where, sizeof g_fb_text);
    WM_REDRAW();   // the chip changes on the next frame's ghost_update()
}

void wm_dnd_start(int pid, int count) { begin(pid, count > 0 ? count : 1); }
void wm_dnd_start_desktop(int count) { begin(0, count > 0 ? count : 1); }

static void leave_current(void) {
    if (g_over >= 0 && g_over < window_count) {
        wm_client_send_mouse(&windows[g_over], WIN_EV_DRAG_LEAVE, 0, 0, 0);
        if (windows[g_over].client_pid == g_fb_pid) g_fb_pid = 0;   // its answer was for there
    }
    g_over = -1;
}

void wm_dnd_end(int pid) {
    if (!g_active || (g_src_pid && pid != g_src_pid)) return;
    leave_current();
    g_active = 0;
    WM_REDRAW();
}

// The topmost window under the pointer whose CONTENT the pointer is
// over, excluding the source's own windows -- its toolkit has those.
// THREE ANSWERS: an index; NOWHERE, the desktop background; and
// SOURCE, covered by the dragging client's own window (or a title bar,
// which takes nothing). The release cares about the difference: a
// drop over the source is its toolkit's, over nowhere the desktop's --
// the first version folded both into -1 and moved a file dropped
// inside its own pane onto the desktop.
#define NOWHERE (-1)
#define SOURCE  (-2)
static int target_under(int mx, int my) {
    const int *zorder = wm_stacking(0);
    for (int z = window_count - 1; z >= 0; z--) {
        int i = zorder[z];
        struct window *w = &windows[i];
        if (w->state == WIN_MINIMIZED) continue;
        if (!uui_hit(w->x, w->y, w->w, w->h, mx, my)) continue;
        if (g_src_pid && w->client_pid == g_src_pid) return SOURCE;
        if (!w->popup && my < w->y + WM_TITLEBAR_H) return SOURCE;
        return wm_client_is_client_window(w) ? i : SOURCE;
    }
    return NOWHERE;
}

// Over something that will take NOTHING from a release here: another
// window's title bar, or a window that is not a client's (the WM's own
// popups). The source's own windows are excluded -- its toolkit decides
// there -- and so is the desktop background, which takes files.
int wm_dnd_refused_at(int mx, int my) {
    if (!g_active) return 0;
    const int *zorder = wm_stacking(0);
    for (int z = window_count - 1; z >= 0; z--) {
        int i = zorder[z];
        struct window *w = &windows[i];
        if (w->state == WIN_MINIMIZED) continue;
        if (!uui_hit(w->x, w->y, w->w, w->h, mx, my)) continue;
        if (g_src_pid && w->client_pid == g_src_pid) return 0;
        return target_under(mx, my) == SOURCE;
    }
    return 0;
}

// WHAT A RELEASE AT (mx, my) WOULD DO, into g_chip. A window's own
// answer while it is the one under the pointer; nothing until it has
// answered, so a stale "Move to" never shows over the next window.
static void chip_for(int t, int mx, int my) {
    int copy = (wm_rawin_pointer_mods() & KEY_MOD_CTRL) != 0;
    const char *dl = desktop_drop_label();
    g_chip[0] = '\0';
    if (dl) {
        k_strlcpy(g_chip, dl, sizeof g_chip);
    } else if (t == NOWHERE) {
        // The desktop's own icon drag over its background is a rearrange.
        if (g_src_pid) k_strlcpy(g_chip, copy ? "Copy to Desktop" : "Move to Desktop", sizeof g_chip);
    } else if (wm_dnd_refused_at(mx, my)) {
        k_strlcpy(g_chip, "Cannot drop here", sizeof g_chip);
    } else {
        int pid = t >= 0 ? windows[t].client_pid : g_src_pid;
        if (pid && pid == g_fb_pid) {
            if (g_fb_accepted) k_strlcpy(g_chip, g_fb_text, sizeof g_chip);
            else if (pid != g_src_pid) k_strlcpy(g_chip, "Cannot drop here", sizeof g_chip);
        }
    }
}

// The image's parts for the pointer at (mx, my): the icon (and the stack
// behind it), the count badge, the chip under it. The desktop's own drag
// draws its icons itself, so it gets the chip alone.
struct ghost { int ix, iy, cx, cy, cw, ch, bx, by, br; };

static void ghost_layout(int mx, int my, struct ghost *g) {
    int pad = ugfx_px(8), stack = g_count > 1 ? ugfx_px(8) : 0;
    g->ix = mx + ugfx_px(12);
    g->iy = my + ugfx_px(16) + stack;
    int icon_h = g_src_pid && g_icon_w ? g_icon_h : 0;
    g->br = g_count > 1 && icon_h ? ugfx_char_h() * 2 / 3 : 0;
    g->bx = g->ix + g_icon_w + stack;
    g->by = g->iy - stack;
    g->cx = g->ix;
    g->cy = g->iy + icon_h + (icon_h ? ugfx_px(6) : 0);
    g->cw = g_chip[0] ? ugfx_text_width(g_chip) + 2 * pad : 0;
    g->ch = g_chip[0] ? ugfx_char_h() + pad : 0;
}

// The box all of it covers; 0 when nothing is drawn.
static int ghost_rect(int mx, int my, int *x, int *y, int *w, int *h) {
    struct ghost g;
    ghost_layout(mx, my, &g);
    int stack = g_count > 1 ? ugfx_px(8) : 0;
    int iw = g_src_pid && g_icon_w ? g_icon_w + stack + g.br : 0;
    if (!iw && !g.cw) return 0;
    *x = g.ix;
    *y = g.iy - stack - g.br;
    *w = iw > g.cw ? iw : g.cw;
    *h = g.cy + g.ch - *y;
    return 1;
}

// A FRAME ONLY WHEN THE LABEL CHANGED, and only its old and new boxes.
// This used to ask for a full-screen repaint on every pass of the loop
// while a drag was held: a fast mouse wakes the loop hundreds of times
// a second, and the recomposes starved the source app of the CPU it
// needed to move its own ghost (the File Manager's label sat still
// until the mouse stopped, on the ASUS).
static void ghost_update(int mx, int my, int t) {
    static char drawn[sizeof g_chip];
    chip_for(t, mx, my);
    int x = 0, y = 0, w = 0, h = 0;
    int on = ghost_rect(mx, my, &x, &y, &w, &h);
    struct ghost lg;
    ghost_layout(mx, my, &lg);
    g_chip_box[0] = lg.cx; g_chip_box[1] = lg.cy; g_chip_box[2] = lg.cw; g_chip_box[3] = lg.ch;
    int same_chip = !k_strcmp(drawn, g_chip);
    k_strlcpy(drawn, g_chip, sizeof drawn);
    if (same_chip && on == g_ghost_on &&
        (!on || (x == g_gx && y == g_gy && w == g_gw && h == g_gh))) return;
    if (g_ghost_on) wm_damage_rect(g_gx - 1, g_gy - 1, g_gw + 2, g_gh + 2);
    if (on) wm_damage_rect(x - 1, y - 1, w + 2, h + 2);
    g_ghost_on = on;
    g_gx = x; g_gy = y; g_gw = w; g_gh = h;
    WM_REDRAW();
}

void wm_dnd_motion(int mx, int my, uint8_t buttons) {
    g_took_drop = 0;
    if (!g_active) return;
    int t = target_under(mx, my);
    if (buttons & 0x1) {
        int over = t >= 0 ? t : -1;
        unsigned mods = wm_rawin_pointer_mods();
        int news = mx != g_sent_x || my != g_sent_y || mods != g_sent_mods;
        if (over != g_over) { leave_current(); g_over = over; news = 1; }
        g_sent_x = mx; g_sent_y = my; g_sent_mods = mods;
        desktop_drop_hover(t == NOWHERE ? mx : -1, t == NOWHERE ? my : -1);
        if (t >= 0 && news) wm_client_send_mouse(&windows[t], WIN_EV_DRAG_OVER, mx, my, 0);
        ghost_update(mx, my, t);   // after the hover, which can change the label
        return;
    }
    // The release. Over a window: its drop. Over the desktop
    // BACKGROUND: the desktop's, unless the desktop is the source (its
    // own icon drag). Over the source: nothing -- its toolkit has it.
    if (t >= 0) {
        wm_client_send_mouse(&windows[t], WIN_EV_DRAG_OVER, mx, my, 0);
        wm_client_send_mouse(&windows[t], WIN_EV_DROP, mx, my, 0);
        g_took_drop = 1;
    } else if (t == NOWHERE && g_src_pid != 0 && desktop_drop_here(mx, my)) {
        g_took_drop = 1;
    }
    // A window's drag is over: no icon stays lit. The desktop's own drag
    // keeps its target for desktop_update_drag(), which runs next.
    if (g_src_pid != 0) desktop_drop_hover(-1, -1);
    g_over = -1;
    g_active = 0;
    // THE SLOT IS NOT CLEARED HERE. The target reads it when it gets
    // round to the DROP event, which is after this tick; clearing it on
    // the release handed the File Manager an empty payload (measured:
    // the pane accepted mid-drag and nothing moved). The next drag's
    // begin overwrites it, and nothing reads it between drags.
    WM_REDRAW();
}

void wm_dnd_draw(int mx, int my) {
    if (!g_active) return;
    int x, y, w, h;
    if (!ghost_rect(mx, my, &x, &y, &w, &h)) return;
    struct ugfx_surface *s = wm_surface();
    struct ghost g;
    ghost_layout(mx, my, &g);
    if (g_src_pid && g_icon_w) {
        // Several: the same icon twice more behind, offset up and right.
        if (g_count > 1) {
            int k = ugfx_px(8);
            ugfx_blit_alpha(s, g.ix + k, g.iy - k, g_icon_w, g_icon_h, g_icon, g_icon_w);
            ugfx_blit_alpha(s, g.ix + k / 2, g.iy - k / 2, g_icon_w, g_icon_h, g_icon, g_icon_w);
        }
        ugfx_blit_alpha(s, g.ix, g.iy, g_icon_w, g_icon_h, g_icon, g_icon_w);
        if (g.br) {
            char n[12];
            k_snprintf(n, sizeof n, "%d", g_count);
            ugfx_fill_circle(s, g.bx, g.by, g.br, UTHEME_ACCENT);
            ugfx_draw_string(s, g.bx - ugfx_text_width(n) / 2, g.by - ugfx_char_h() / 2, n,
                             UTHEME_ACCENT_TEXT, UTHEME_ACCENT);
        }
    }
    if (g.cw) {
        uui_control_frame(s, g.cx, g.cy, g.cw, g.ch, UTHEME_WHITE, UTHEME_OUTLINE);
        ugfx_draw_string_clipped(s, g.cx + ugfx_px(8), g.cy + (g.ch - ugfx_char_h()) / 2,
                                 g.cw - 2 * ugfx_px(8), g_chip, UTHEME_TEXT, UTHEME_WHITE);
    }
    wm_damage_rect(x - 1, y - 1, w + 2, h + 2);
}

int wm_dnd_state(int *count, int *over_pid, const char **chip, const char **icon, int box[4]) {
    *count = g_count;
    *over_pid = g_fb_pid;
    *chip = g_chip;
    *icon = g_icon_w ? "yes" : "no";
    for (int i = 0; i < 4; i++) box[i] = g_chip_box[i];
    return g_active;
}

void wm_dnd_windows_moving(int idx, int to_front) {
    g_over = wm_index_after_move(g_over, idx, to_front);
}
