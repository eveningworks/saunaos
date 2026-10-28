// input16.c -- the MAIN thread's half of input: a key or a press on a
// top-level window (the program's, or a dialog's) posted to the task as
// the Win16 message for the window it is meant for -- the child under
// the pointer, the window holding the capture, or the one with the focus.
//
// toywm hands over the TRANSLATED key, so the character is known here:
// WM_KEYDOWN and then WM_CHAR are posted together, and TranslateMessage
// (user16.c) has nothing left to do. A message carries the Shift, Ctrl and
// Alt it was made with, which GetKeyState answers while it is handled.
#include "w16_internal.h"

#include "keyboard.h"
#include "win_proto.h"   // WIN_CURSOR_*
#include "input_keys.h"
#include "rt/sys.h"
#include <time.h>

#define VK_BACK   0x08
#define VK_TAB    0x09
#define VK_RETURN 0x0D
#define VK_ESCAPE 0x1B
#define CS_DBLCLKS 0x0008
#define HTHSCROLL 6
#define HTVSCROLL 7

// A key's Windows virtual-key code (VK_*): letters and digits are their
// own, the rest by table. 0 for a key with none.
static const struct { int key, vk; } VK[] = {
    { '\n', VK_RETURN }, { '\r', VK_RETURN }, { '\b', VK_BACK }, { '\t', VK_TAB }, { 0x1B, VK_ESCAPE },
    { KEY_ARROW_LEFT, VK_LEFT }, { KEY_ARROW_UP, VK_UP }, { KEY_ARROW_RIGHT, VK_RIGHT }, { KEY_ARROW_DOWN, VK_DOWN },
    { KEY_PAGE_UP, VK_PRIOR }, { KEY_PAGE_DOWN, VK_NEXT }, { KEY_END, VK_END }, { KEY_HOME, VK_HOME },
    { KEY_DELETE, VK_DELETE }, { KEY_INSERT, VK_INSERT },
    { KEY_F1, 0x70 }, { KEY_F2, 0x71 }, { KEY_F3, 0x72 }, { KEY_F4, 0x73 }, { KEY_F5, 0x74 },
    { KEY_F6, 0x75 }, { KEY_F7, 0x76 }, { KEY_F8, 0x77 }, { KEY_F9, 0x78 }, { KEY_F10, 0x79 },
    { KEY_F11, 0x7A }, { KEY_F12, 0x7B },
};

int w16_vk_of(int key) {
    if (key >= 'a' && key <= 'z') return key - 32;
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') || key == ' ') return key;
    for (unsigned i = 0; i < sizeof VK / sizeof VK[0]; i++)
        if (VK[i].key == key) return VK[i].vk;
    return 0;
}

// The window of `top` a key goes to: the focus, when it is in `top`.
static uint16_t key_target(struct w16 *w, struct w16_wnd *top) {
    pthread_mutex_lock(&w->lock);
    struct w16_wnd *f = w16_wnd_get(w, w->focus);
    uint16_t to = f && w16_wnd_top(w, f) == top ? f->hwnd : top->hwnd;
    pthread_mutex_unlock(&w->lock);
    return to;
}

// Shift and Ctrl as keys of their own: a WM_KEYDOWN when one is first seen
// held with a key, a WM_KEYUP when a key arrives without it -- the messages
// a real keyboard sends, which a program tracking the keys itself (Write's
// Ctrl+Home, Shift+arrows) reads instead of GetKeyState. toywm sends no event
// for a modifier alone, so a release is seen at the next key.
static void modifiers(struct w16 *w, uint16_t to, unsigned mods) {
    static unsigned held;
    static const struct { unsigned mod; uint16_t vk; } M[] = { { KEY_MOD_SHIFT, VK_SHIFT }, { KEY_MOD_CTRL, VK_CONTROL } };
    for (unsigned i = 0; i < 2; i++) {
        if ((mods & M[i].mod) && !(held & M[i].mod)) w16_post_msg_mods(w, to, WM_KEYDOWN, M[i].vk, 1, mods);
        if (!(mods & M[i].mod) && (held & M[i].mod)) w16_post_msg_mods(w, to, WM_KEYUP, M[i].vk, 0xC0000001u, mods);
    }
    held = mods & (KEY_MOD_SHIFT | KEY_MOD_CTRL);
}

// --- a held key's repeat, on Windows' timing -------------------------------------------------
//
// A Windows program's held key repeats at WIN.INI's KeyboardDelay and KeyboardSpeed
// (w16_key_repeat()), as 3.1's keyboard driver set the 8042's typematic -- not at SaunaOS's,
// which repeats only a PS/2 keyboard's keys at all. So w16 hears keys BY POSITION too
// (WIN_EV_KEY_PHYS: both edges, never a repeat), pairs a physical press with the key it typed,
// makes that key's repeats itself from the tick until the physical release, and drops SaunaOS's
// own. A key with no physical press -- the debug console's, a tap on the on-screen keyboard --
// is passed on once and never repeated, so a release that never comes holds nothing down.

#define PAIR_US 250000L
#define SB_LINEUP   0
#define SB_LINEDOWN 1
#define SB_ENDSCROLL 8                          // a physical press and its key arrive this close

static int g_phys[8];                            // the keys physically down: USB boot keyboards report six
static int g_nphys;
static struct { int code; long at; } g_lone_phys;        // a press its key has not come for yet
static struct { int key; unsigned mods; uint16_t dlg; long at; } g_lone_key;   // and the other way
static struct { int key, phys; unsigned mods; uint16_t dlg; long next; int on; } g_rep;

static long now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

static int phys_down(int code) {
    for (int i = 0; i < g_nphys; i++)
        if (g_phys[i] == code) return 1;
    return 0;
}

static void repeat_from(int key, unsigned mods, uint16_t dlg, int phys, long pressed) {
    long delay, period;
    w16_key_repeat(&delay, &period);
    g_rep.key = key;
    g_rep.mods = mods;
    g_rep.dlg = dlg;
    g_rep.phys = phys;
    g_rep.next = pressed + delay;
    g_rep.on = 1;
}

int w16_key_press(int key, unsigned mods, uint16_t dlg) {
    if (g_rep.on && key == g_rep.key && phys_down(g_rep.phys)) return 0;   // SaunaOS's own repeat
    long t = now_us();
    g_rep.on = 0;                                // a new key stops the last one's repeat, as 3.1's
    if (g_lone_phys.code && t - g_lone_phys.at < PAIR_US) {
        repeat_from(key, mods, dlg, g_lone_phys.code, g_lone_phys.at);
        g_lone_phys.code = 0;
    } else {
        g_lone_key.key = key;
        g_lone_key.mods = mods;
        g_lone_key.dlg = dlg;
        g_lone_key.at = t;
    }
    return 1;
}

// Modifiers and locks type nothing of their own: never paired, never repeated.
static int is_modifier(int code) {
    switch (code) {
    case INPUT_KEY_LEFTCTRL: case INPUT_KEY_RIGHTCTRL: case INPUT_KEY_LEFTSHIFT: case INPUT_KEY_RIGHTSHIFT:
    case INPUT_KEY_LEFTALT: case INPUT_KEY_RIGHTALT: case INPUT_KEY_LEFTMETA: case INPUT_KEY_RIGHTMETA:
    case INPUT_KEY_CAPSLOCK: case INPUT_KEY_NUMLOCK: case INPUT_KEY_SCROLLLOCK:
        return 1;
    default:
        return 0;
    }
}

void w16_key_phys(int code, int down) {
    if (is_modifier(code)) return;
    if (!down) {
        for (int i = 0; i < g_nphys; i++)
            if (g_phys[i] == code) g_phys[i--] = g_phys[--g_nphys];
        if (g_rep.phys == code) g_rep.on = 0;
        if (g_lone_phys.code == code) g_lone_phys.code = 0;
        return;
    }
    if (!phys_down(code) && g_nphys < (int)(sizeof g_phys / sizeof g_phys[0])) g_phys[g_nphys++] = code;
    long t = now_us();
    if (g_lone_key.at && t - g_lone_key.at < PAIR_US) {
        repeat_from(g_lone_key.key, g_lone_key.mods, g_lone_key.dlg, code, g_lone_key.at);
        g_lone_key.at = 0;
    } else {
        g_lone_phys.code = code;
        g_lone_phys.at = t;
    }
}

void w16_key_release_all(void) {
    g_nphys = 0;
    g_rep.on = 0;
    g_lone_phys.code = 0;
    g_lone_key.at = 0;
}

int w16_key_due(int *key, unsigned *mods, uint16_t *dlg) {
    if (!g_rep.on) return 0;
    long t = now_us(), delay, period;
    if (t < g_rep.next) return 0;
    w16_key_repeat(&delay, &period);
    int n = (int)((t - g_rep.next) / period) + 1;
    if (n > 4) n = 4;                            // a tick that came late does not flood the program
    g_rep.next = t + period - (t - g_rep.next) % period;
    *key = g_rep.key;
    *mods = g_rep.mods;
    *dlg = g_rep.dlg;
    return n;
}

void w16_route_key(struct w16 *w, struct w16_wnd *top, int key, unsigned mods, int up) {
    uint16_t to = key_target(w, top);
    mods &= KEY_MOD_SHIFT | KEY_MOD_CTRL | KEY_MOD_ALT;
    int vk = w16_vk_of(key);
    if (!up) modifiers(w, to, mods);
    if (up) {
        if (vk) w16_post_msg_mods(w, to, WM_KEYUP, (uint16_t)vk, 0xC0000001u, mods);
        return;
    }
    if (vk) w16_post_msg_mods(w, to, WM_KEYDOWN, (uint16_t)vk, 1, mods);
    int ch = key == '\n' ? '\r' : key;
    // Ctrl and a letter is its control character, as Windows made it.
    if ((mods & KEY_MOD_CTRL) && ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))) ch &= 0x1F;
    if ((ch >= 0x20 && ch <= 0xFF && ch != 0x7F) || (ch > 0 && ch < 0x20))
        w16_post_msg_mods(w, to, WM_CHAR, (uint16_t)ch, 1, mods);
}

// The last press, for a double click: when, where and on what.
static struct { uint64_t tick; uint16_t hwnd; int x, y; } g_last;

// Where the pointer was last seen, for the wheel, which carries no position: the top-level window
// and the point in its client coordinates.
static struct { uint16_t top; int x, y; } g_ptr;

int w16_route_mouse(struct w16 *w, struct w16_wnd *top, uint16_t msg, uint16_t wp, int x, int y) {
    pthread_mutex_lock(&w->lock);
    g_ptr.top = top->hwnd;
    g_ptr.x = x;
    g_ptr.y = y;
    struct w16_wnd *cap = w16_wnd_get(w, w->capture), *to;
    int cx, cy, nc = 0, dbl = 0, held = cap && w16_wnd_top(w, cap) == top;
    if (held) {
        // Held by one window: the point in its client coordinates, wherever it is -- except a press
        // on that window's OWN scroll bar, which is its frame's, as anywhere else: an open drop-down
        // holds the capture, and its list must still scroll.
        int ox, oy, l, t, r, b;
        w16_wnd_origin(w, cap, &ox, &oy);
        w16_wnd_insets(cap, &l, &t, &r, &b);
        to = cap;
        cx = x - ox;
        cy = y - oy;
        int wx = cx + l, wy = cy + t;
        if (msg == WM_LBUTTONDOWN && (cx < 0 || cy < 0 || cx >= cap->cw || cy >= cap->ch) &&
            wx >= 0 && wy >= 0 && wx < cap->w && wy < cap->h && w16_sbar_hit(cap, wx, wy) >= 0) {
            nc = 1;
            cx = wx;
            cy = wy;
        }
    } else {
        to = w16_wnd_at(w, top, x, y, &cx, &cy, &nc);
    }
    uint16_t hwnd = to ? to->hwnd : 0;
    int bar = nc && to ? w16_sbar_hit(to, cx, cy) : -1;
    if (msg == WM_LBUTTONDOWN && !nc && to) {
        uint64_t now = sys_ticks();
        int near = cx - g_last.x < 4 && g_last.x - cx < 4 && cy - g_last.y < 4 && g_last.y - cy < 4;
        int wants = to->cls >= 0 && to->cls < w->ncls && (w->cls[to->cls].style & CS_DBLCLKS);
        dbl = wants && g_last.hwnd == hwnd && near && now - g_last.tick <= w16_dclick_ticks();
        g_last.tick = dbl ? 0 : now;
        g_last.hwnd = hwnd;
        g_last.x = cx;
        g_last.y = cy;
    }
    int disabled = to && (to->style & WS_DISABLED);
    uint16_t hc = to && !nc && to->cls >= 0 && to->cls < w->ncls ? w->cls[to->cls].cursor : W16_CURSOR_BASE;
    int shape = hc ? w16_cursor_shape(hc) : -1;  // a class with none: the program's SetCursor
    if (held) shape = -1;                        // no WM_SETCURSOR under a capture, as in 3.1
    if (shape >= 0) w->cursor_now = hc;
    pthread_mutex_unlock(&w->lock);
    if (!hwnd || disabled) return WIN_CURSOR_DEFAULT;
    if (nc) {                                    // the frame of a child: its scroll bars
        if (msg == WM_LBUTTONDOWN && bar >= 0)
            w16_post_msg(w, hwnd, WM_NCLBUTTONDOWN, bar == SB_VERT ? HTVSCROLL : HTHSCROLL,
                         (uint32_t)(uint16_t)cy << 16 | (uint16_t)cx);
        return shape;
    }
    w16_post_msg(w, hwnd, dbl ? WM_LBUTTONDBLCLK : msg, wp, (uint32_t)(uint16_t)cy << 16 | (uint16_t)cx);
    return shape;
}

// THE WHEEL, which Windows 3.1 never had, as the drivers that brought it to 3.1's programs did:
// each notch three WM_VSCROLL lines (up for +) to the window under the pointer that has a
// vertical scroll bar, or its nearest parent that has -- a list, an open drop-down's list, a
// multiline edit, a document window. Messages 3.1 already had; a program that never asked for a
// wheel scrolls by one anyway.
void w16_route_wheel(struct w16 *w, struct w16_wnd *top, int notches) {
    if (!notches) return;
    pthread_mutex_lock(&w->lock);
    struct w16_wnd *at = 0;
    if (g_ptr.top == top->hwnd) {
        int cx, cy, nc;
        at = w16_wnd_at(w, top, g_ptr.x, g_ptr.y, &cx, &cy, &nc);
    }
    for (int depth = 0; at && depth < 32 && !(at->style & WS_VSCROLL); depth++)
        at = at->kind == W16_K_CHILD ? w16_wnd_get(w, at->parent) : 0;
    uint16_t hwnd = at && !(at->style & WS_DISABLED) ? at->hwnd : 0;
    pthread_mutex_unlock(&w->lock);
    if (!hwnd) return;
    uint16_t code = notches > 0 ? SB_LINEUP : SB_LINEDOWN;
    for (int i = 0; i < 3 * (notches > 0 ? notches : -notches); i++) w16_post_msg(w, hwnd, WM_VSCROLL, code, 0);
    w16_post_msg(w, hwnd, WM_VSCROLL, SB_ENDSCROLL, 0);
}
