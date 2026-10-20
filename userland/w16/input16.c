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
#include "lib/udclick.h"
#include "rt/sys.h"

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

int w16_route_mouse(struct w16 *w, struct w16_wnd *top, uint16_t msg, uint16_t wp, int x, int y) {
    pthread_mutex_lock(&w->lock);
    struct w16_wnd *cap = w16_wnd_get(w, w->capture), *to;
    int cx, cy, nc = 0, dbl = 0, held = cap && w16_wnd_top(w, cap) == top;
    if (held) {
        // Held by one window: the point in its client coordinates, wherever it is.
        int ox, oy;
        w16_wnd_origin(w, cap, &ox, &oy);
        to = cap;
        cx = x - ox;
        cy = y - oy;
    } else {
        to = w16_wnd_at(w, top, x, y, &cx, &cy, &nc);
    }
    uint16_t hwnd = to ? to->hwnd : 0;
    int bar = nc && to ? w16_sbar_hit(to, cx, cy) : -1;
    if (msg == WM_LBUTTONDOWN && !nc && to) {
        uint64_t now = sys_ticks();
        int near = cx - g_last.x < 4 && g_last.x - cx < 4 && cy - g_last.y < 4 && g_last.y - cy < 4;
        int wants = to->cls >= 0 && to->cls < w->ncls && (w->cls[to->cls].style & CS_DBLCLKS);
        dbl = wants && g_last.hwnd == hwnd && near && now - g_last.tick <= udclick_ticks();
        g_last.tick = dbl ? 0 : now;
        g_last.hwnd = hwnd;
        g_last.x = cx;
        g_last.y = cy;
    }
    int disabled = to && (to->style & WS_DISABLED);
    int shape = to && !nc && to->cls >= 0 && to->cls < w->ncls ? w->cls[to->cls].cursor : WIN_CURSOR_DEFAULT;
    if (held) shape = -1;                        // no WM_SETCURSOR under a capture, as in 3.1
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
