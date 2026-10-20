// w16.c -- run a 16-bit Windows program (docs/w16-design.md). Its parts
// live in userland/w16/; this is main() and the MAIN thread's half of a
// running program: the toywm window -- undecorated, because frame16.c
// draws the Windows 3.1 frame -- and input turned into Win16 messages.
// The program itself runs on the task thread (task16.c).
//
// THE FILE IS HOSTILE INPUT, and so is the code in it: the process gives
// up everything it will not need before it reads a byte, and refuses to
// run if it cannot.
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "font_shm.h"
#include "keyboard.h"
#include "win_proto.h"
#include "lib/uargs.h"
#include "lib/udclick.h"
#include "lib/uclip_page.h"   // CLIP_SHM_NAME: the clipboard an EDIT copies to
#include "rt/sys.h"
#include "query_abi.h"
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "w16/w16_internal.h"
#include "w16/w16prefs.h"

#define W16_FILE_MAX (16u << 20)   // NE files are at most a few MiB

#define MK_LBUTTON 0x0001
#define MK_RBUTTON 0x0002

static int g_info, g_check, g_fonts;
static char g_tail[127];                 // the program's command line: DOS's 126 characters
static const char *g_trace_path;
static char g_exe[W16_PATH_MAX];         // the program, as an absolute path: its settings' key

static const struct uargs_opt OPTS[] = {
    { "info", 'i', NULL, "print the program's segments and the functions it imports", &g_info, NULL },
    { "check", 'c', NULL, "list what the program imports, which of it w16 has, its kind, and the icon",
      &g_check, NULL },
    { "fonts", 'f', NULL, "list Windows 3.11's own fonts found on drive C:, with a sample of each face",
      &g_fonts, NULL },
    { "trace", 't', "FILE", "log every Windows call the program makes to FILE (Wine's +relay)",
      NULL, &g_trace_path },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "w16",
    .usage = "FILE [ARGUMENT...]\n--info FILE\n--check FILE\n--fonts",
    .summary = "Run a 16-bit Windows (Windows 3.x) program in a window, or with --info\n"
               "list what an NE executable holds and the Windows functions it imports.\n"
               "Only a few functions are implemented so far: a program that calls one\n"
               "that is not ends, naming it.",
    .opts = OPTS,
    .notes = "Exit status: the program's own, 1 the file was refused, 2 a usage error.\n"
             "A program that faults, or calls a function w16 does not have, ends w16\n"
             "with SIGABRT and a crash report that names why.",
};

static uint8_t *slurp(const char *path, uint32_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *buf = NULL;
    size_t cap = 0, n = 0;
    for (;;) {
        if (n == cap) {
            if (cap >= W16_FILE_MAX) { free(buf); fclose(f); *size = UINT32_MAX; return NULL; }
            cap = cap ? cap * 2 : 64 * 1024;
            uint8_t *nb = realloc(buf, cap);
            if (!nb) { free(buf); fclose(f); return NULL; }
            buf = nb;
        }
        size_t got = fread(buf + n, 1, cap - n, f);
        if (got == 0) break;
        n += got;
    }
    fclose(f);
    *size = (uint32_t)n;
    return buf;
}

// --- the main thread's half -------------------------------------------------

static int g_pressed;                    // HIT_MIN/HIT_MAX while held, else 0
static uint64_t g_sysmenu_tick;          // the last press on the control-menu box
static uint64_t g_caption_tick;          // the last press on the caption, for its double click
static struct w16_menu g_menu;           // the control menu
static struct w16_menu g_bar;            // the menu bar's open title, its popup (menubar16.c)
static int g_bar_pos = -1;               // that title, or -1
static int g_bar_pending;                // asked of the task, not shown yet
static int g_bar_by_key;
// 3.1's menu-bar mode: a title lit with nothing dropped -- after Esc closes
// a menu opened from the keyboard, or after F10. Arrows move it, Down or
// Enter drops it, a second Esc leaves. -1 when the bar is not in it.
static int g_bar_hot = -1;
static struct w16_msgbox g_box;          // the Application Error box, once the program stopped
static int g_faulted;
static unsigned g_buttons;               // the mouse buttons down, as the program last heard

// The client-area mouse message for a button: down or up.
static uint16_t button_msg(unsigned bit, int down) {
    if (bit == 1) return down ? WM_LBUTTONDOWN : WM_LBUTTONUP;
    return down ? WM_RBUTTONDOWN : WM_RBUTTONUP;
}

// MK_* for wParam: the buttons down now, and Shift and Ctrl.
static uint16_t mk_flags(unsigned buttons) {
    unsigned b = WIN_MOUSE_BUTTONS(buttons), m = WIN_MOUSE_MODS(buttons);
    return (uint16_t)((b & 1 ? MK_LBUTTON : 0) | (b & 2 ? MK_RBUTTON : 0) |
                      (m & KEY_MOD_SHIFT ? 0x0004 : 0) | (m & KEY_MOD_CTRL ? 0x0008 : 0));
}

// The program hears the mouse in its client area -- or anywhere, while it
// holds the capture (SetCapture), as Minesweeper's press on a square does.
static int mouse_is_programs(enum w16_hit hit) {
    return hit == HIT_CLIENT || g_w16.capture;
}

// A press, a release or a move at window point (x, y), for the window
// of the program's under it (or holding the capture).
static int client_mouse(uint16_t msg, uint16_t wp, int x, int y) {
    int cx, cy, cw, ch;
    w16_client_rect(&g_w16.win, &cx, &cy, &cw, &ch);
    return w16_route_mouse(&g_w16, &g_w16.win, msg, wp, x - cx, y - cy);
}

static void syscommand(unsigned sc) {
    w16_post_msg(&g_w16, g_w16.win.hwnd, WM_SYSCOMMAND, (uint16_t)sc, 0);
}

// --- the menu bar -------------------------------------------------------------------
//
// Asking to open a title goes through the task (menubar16.c), which shows
// the program WM_INITMENUPOPUP first; W16_POST_MENU_SHOW then opens it.

static int bar_open(void) { return g_bar.popup > 0 || g_bar_pending; }

static void bar_request(struct uapp *a, int pos, int by_key) {
    int first = !bar_open();
    w16_menu_close(&g_bar);
    w16_menu_close(&g_menu);
    g_bar_pos = pos;
    g_bar_pending = 1;
    g_bar_by_key = by_key;
    w16_post_msg(&g_w16, g_w16.win.hwnd, W16_WM_MENUOPEN, (uint16_t)pos, (uint32_t)first);
    uapp_redraw(a);
}

static void bar_close(struct uapp *a) {
    w16_menu_close(&g_bar);
    g_bar_pos = -1;
    g_bar_pending = 0;
    uapp_redraw(a);
}

static void bar_show(int pos) {
    if (pos != g_bar_pos || !g_bar_pending) return;
    g_bar_pending = 0;
    int bx, by, bw, bh, tx = 0, tw = 0;
    pthread_mutex_lock(&g_w16.lock);
    w16_frame_menubar(&g_w16.win, &bx, &by, &bw, &bh);
    w16_menubar_build(&g_w16, pos, &g_bar);
    int ok = w16_menubar_title_at(&g_w16, -(pos + 1), &tx, &tw) >= 0;
    pthread_mutex_unlock(&g_w16.lock);
    if (ok && g_bar.n) w16_menu_open(&g_bar, bx + tx, by, tw, bh - 1, g_bar_by_key);
    else g_bar_pos = -1;
}

// The bar title under window point x, or -1.
static int bar_title(int x) {
    int bx, by, bw, bh;
    pthread_mutex_lock(&g_w16.lock);
    w16_frame_menubar(&g_w16.win, &bx, &by, &bw, &bh);
    int t = bh ? w16_menubar_title_at(&g_w16, x - bx, 0, 0) : -1;
    pthread_mutex_unlock(&g_w16.lock);
    return t;
}

static void bar_choose(struct uapp *a, unsigned id) {
    g_bar_pos = -1;
    w16_post_msg(&g_w16, g_w16.win.hwnd, WM_COMMAND, (uint16_t)id, 0);
    uapp_redraw(a);
}

static int maximized(void) {
    pthread_mutex_lock(&g_w16.lock);
    int m = g_w16.win.maximized;
    pthread_mutex_unlock(&g_w16.lock);
    return m;
}

// What happened the last time the program ran, for the launcher:
// "<seconds since 1970> ended" or "... stopped <the fault's headline>".
static void record_last(const char *what) {
    char path[W16_PATH_MAX + 32];
    w16_prog_last(g_exe, path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%ld %s\n", (long)time(0), what);
    fclose(f);
}

static void record_fault(void) {
    char head[160];
    size_t n = strcspn(g_w16_fault, "\n");
    snprintf(head, sizeof head, "stopped %.*s", (int)(n < 150 ? n : 150), g_w16_fault);
    record_last(head);
}

// The program stopped and its box is answered (or could not be shown):
// end with its message as the crash report's headline.
static void end_faulted(void) {
    w16_msgbox_close(&g_box);
    record_fault();
    abort();
}

static void do_syscmd(struct uapp *a, int sc) {
    switch (sc) {
    case SC_MINIMIZE: uapp_minimize(a); break;
    case SC_MAXIMIZE: uapp_set_maximized(a, 1); break;
    case SC_RESTORE:  uapp_set_maximized(a, 0); break;
    case SC_KEYMENU:
        if (!w16_menu_open_p(&g_menu) && (g_w16.win.style & WS_SYSMENU)) {
            int x, y, w, h;
            w16_frame_sysbox(&g_w16.win, &x, &y, &w, &h);
            w16_sysmenu_build(&g_menu, &g_w16.win);
            w16_menu_open(&g_menu, x, y, w, h, 1);
        }
        break;
    default: break;
    }
}

static void on_open(struct uapp *a) {
    pthread_mutex_lock(&g_w16.lock);
    g_w16.app = a;
    int pending = g_w16.pending_syscmd, end = g_w16.pending_end;
    g_w16.pending_syscmd = g_w16.pending_end = 0;
    // The title as it is NOW: a program that retitled its window before
    // this one was up (Notepad's "Notepad - FILE.TXT") posted to no one.
    char *title = g_w16.win.text ? strdup(g_w16.win.text) : 0;
    pthread_mutex_unlock(&g_w16.lock);
    if (title) uapp_set_title(a, title);
    free(title);
    if (pending) do_syscmd(a, pending);   // ShowWindow(SW_MAXIMIZE) before the window was up
    // The program stopped while its window was opening: the fault's box,
    // or the end, would otherwise have gone to no one and the window stayed.
    if (end) uapp_post(a, end, 0);
}

// The window at 1x: the frame, then the client's pixels. At 2x it is
// composed into `g_virt` first and every pixel doubled onto the surface.
static uint32_t *g_virt;
static int g_virt_w, g_virt_h;

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    struct ugfx_surface *s = uapp_surface(d);
    struct w16_wnd *win = &g_w16.win;
    pthread_mutex_lock(&g_w16.lock);
    struct ugfx_surface vs, *out = s;
    if (g_w16_q != 4) {
        if (g_virt_w != win->w || g_virt_h != win->h) {
            free(g_virt);
            g_virt = calloc((size_t)win->w * win->h + 1, 4);
            g_virt_w = g_virt ? win->w : 0;
            g_virt_h = g_virt ? win->h : 0;
        }
        if (g_virt) { vs = ugfx_surface_for_pixels(g_virt, win->w, win->h); out = &vs; }
    }
    w16_frame_draw(out, win, uapp_focused(a), w16_menu_open_p(&g_menu) ? HIT_SYSMENU : g_pressed);
    int bx, by, bw, bh;
    w16_frame_menubar(win, &bx, &by, &bw, &bh);
    if (bh) w16_menubar_draw(out, &g_w16, bx, by, bw, bar_open() ? g_bar_pos : g_bar_hot);
    int cx, cy, cw, ch;
    w16_client_rect(win, &cx, &cy, &cw, &ch);
    if (win->fb) ugfx_blit(out, cx, cy, win->fbw, win->fbh, win->fb, win->fbw);
    static struct w16_scaler sc;
    if (out != s) w16_scaler_show(&sc, s, 0, 0, g_virt, win->w, win->h);
    pthread_mutex_unlock(&g_w16.lock);
    w16_menu_draw(&g_menu);
    w16_menu_draw(&g_bar);
    w16_msgbox_draw(&g_box);
}

static void on_press(struct uapp *a, int x, int y, unsigned buttons) {
    x = w16_down(x);
    y = w16_down(y);
    if (g_faulted) {                     // modal: only the box's button
        w16_msgbox_press(&g_box, x, y);
        uapp_redraw(a);
        return;
    }
    if (g_bar_hot >= 0) { g_bar_hot = -1; uapp_redraw(a); }   // a press leaves menu-bar mode
    unsigned edges;
    enum w16_hit hit = w16_frame_hit(&g_w16.win, x, y, &edges);
    uint64_t now = sys_ticks();
    if (bar_open()) {
        // Another title switches to it, the open one closes it, and any
        // other press is the popup's -- outside it, a close.
        int t = hit == HIT_MENU ? bar_title(x) : -1;
        if (t >= 0 && t != g_bar_pos) bar_request(a, t, 0);
        else if (t >= 0) bar_close(a);
        else if (!w16_menu_press(&g_bar, x, y)) bar_close(a);
        uapp_redraw(a);
        return;
    }
    if (hit == HIT_MENU && (buttons & 1)) {
        int t = bar_title(x);
        if (t >= 0) bar_request(a, t, 0);
        return;
    }
    if (w16_menu_open_p(&g_menu)) {
        // A press in the menu is the menu's; anywhere else closes it and
        // is spent doing so -- except a second press on the box, which
        // is 3.1's double click to close.
        if (!w16_menu_press(&g_menu, x, y) && hit == HIT_SYSMENU &&
            now - g_sysmenu_tick <= udclick_ticks()) {
            syscommand(SC_CLOSE);
            g_sysmenu_tick = 0;
        }
        uapp_redraw(a);
        return;
    }
    if (mouse_is_programs(hit)) {
        unsigned now_down = WIN_MOUSE_BUTTONS(buttons) & 3, went = now_down & ~g_buttons;
        g_buttons = now_down;
        for (unsigned bit = 1; bit <= 2; bit <<= 1)
            if (went & bit) client_mouse(button_msg(bit, 1), mk_flags(buttons), x, y);
        return;
    }
    if (!(buttons & 1)) return;
    int max = maximized();
    switch (hit) {
    case HIT_CAPTION:
        // A double click maximizes or restores, as on 3.1; a maximized
        // window does not move.
        if (now - g_caption_tick <= udclick_ticks() && (g_w16.win.style & WS_MAXIMIZEBOX)) {
            syscommand(max ? SC_RESTORE : SC_MAXIMIZE);
            g_caption_tick = 0;
            break;
        }
        g_caption_tick = now;
        if (!max) uapp_move_start(a, 0);
        break;
    case HIT_EDGE: if (!max) uapp_move_start(a, edges); break;
    case HIT_MIN: case HIT_MAX:
        g_pressed = hit;
        uapp_redraw(a);
        break;
    case HIT_SYSMENU: {
        int bx, by, bw, bh;
        w16_frame_sysbox(&g_w16.win, &bx, &by, &bw, &bh);
        w16_sysmenu_build(&g_menu, &g_w16.win);
        w16_menu_open(&g_menu, bx, by, bw, bh, 0);
        g_sysmenu_tick = now;
        uapp_redraw(a);
        break;
    }
    default: break;
    }
}

static void on_release(struct uapp *a, int x, int y, unsigned buttons) {
    x = w16_down(x);
    y = w16_down(y);
    if (g_faulted) {
        if (w16_msgbox_release(&g_box, x, y)) end_faulted();
        uapp_redraw(a);
        return;
    }
    if (g_bar.popup > 0) {
        unsigned id = w16_menu_release(&g_bar, x, y);
        if (id && id < W16_MENU_RIGHT) bar_choose(a, id);
        uapp_redraw(a);
        return;
    }
    if (w16_menu_open_p(&g_menu)) {
        unsigned id = w16_menu_release(&g_menu, x, y);
        if (id && id != (unsigned)-1) syscommand(id);
        uapp_redraw(a);
        return;
    }
    unsigned edges;
    enum w16_hit hit = w16_frame_hit(&g_w16.win, x, y, &edges);
    if (g_pressed) {                     // a caption button acts on release, on itself
        if ((int)hit == g_pressed)
            syscommand(hit == HIT_MIN ? SC_MINIMIZE : maximized() ? SC_RESTORE : SC_MAXIMIZE);
        g_pressed = 0;
        uapp_redraw(a);
        return;
    }
    unsigned now_down = WIN_MOUSE_BUTTONS(buttons) & 3, went = g_buttons & ~now_down;
    g_buttons = now_down;
    if (mouse_is_programs(hit))
        for (unsigned bit = 1; bit <= 2; bit <<= 1)
            if (went & bit) client_mouse(button_msg(bit, 0), mk_flags(buttons), x, y);
}

static int edge_cursor(unsigned edges) {
    int h = edges & (WIN_EDGE_LEFT | WIN_EDGE_RIGHT), v = edges & (WIN_EDGE_TOP | WIN_EDGE_BOTTOM);
    if (h && v)                          // top-left and bottom-right drag along \, the others /
        return !(edges & WIN_EDGE_LEFT) == !(edges & WIN_EDGE_TOP) ? WIN_CURSOR_RESIZE_NWSE : WIN_CURSOR_RESIZE_NESW;
    return h ? WIN_CURSOR_RESIZE_H : v ? WIN_CURSOR_RESIZE_V : WIN_CURSOR_DEFAULT;
}

static void on_motion(struct uapp *a, int x, int y, unsigned buttons) {
    x = w16_down(x);
    y = w16_down(y);
    if (g_faulted) return;
    if (bar_open()) {
        unsigned edges;
        int t = w16_frame_hit(&g_w16.win, x, y, &edges) == HIT_MENU ? bar_title(x) : -1;
        if (t >= 0 && t != g_bar_pos) { bar_request(a, t, 0); return; }
        int was = g_bar.sel;
        w16_menu_motion(&g_bar, x, y);
        if (g_bar.sel != was) uapp_redraw(a);
        return;
    }
    if (w16_menu_open_p(&g_menu)) {
        int was = g_menu.sel;
        w16_menu_motion(&g_menu, x, y);
        if (g_menu.sel != was) uapp_redraw(a);
        return;
    }
    // The pointer takes the shape of the window under it: an edit's I-beam,
    // or over the 3.1 frame's border -- which toywm cannot see -- the
    // resize arrow for the way that edge or corner drags. Under a capture
    // it keeps the shape it had when the capture began.
    unsigned edges;
    int shape = WIN_CURSOR_DEFAULT;
    enum w16_hit hit = w16_frame_hit(&g_w16.win, x, y, &edges);
    if (mouse_is_programs(hit)) shape = client_mouse(WM_MOUSEMOVE, mk_flags(buttons), x, y);
    else if (hit == HIT_EDGE && !maximized()) shape = edge_cursor(edges);
    if (shape >= 0) uapp_set_cursor(a, shape);
}

static int on_tick(struct uapp *a) {
    (void)a;
    w16_timers_fire(&g_w16);
    return 0;
}

// A key not for the frame or the menus goes to the program's window with
// the focus (input16.c).
static void on_key(struct uapp *a, int key, unsigned mods) {
    if (g_faulted) {
        if (w16_msgbox_key(&g_box, key)) end_faulted();
        return;
    }
    if (g_bar.popup > 0) {
        unsigned id = w16_menu_key(&g_bar, key);
        if (id == W16_MENU_LEFT || id == W16_MENU_RIGHT) {
            pthread_mutex_lock(&g_w16.lock);
            int n = w16_menubar_count(&g_w16);
            pthread_mutex_unlock(&g_w16.lock);
            if (n) bar_request(a, (g_bar_pos + (id == W16_MENU_LEFT ? n - 1 : 1)) % n, 1);
        } else if (id == (unsigned)-1) {
            int was = g_bar_pos;
            bar_close(a);
            g_bar_hot = g_bar_by_key ? was : -1;   // Esc steps back to the bar, not out of it
        } else if (id) {
            bar_choose(a, id);
        }
        uapp_redraw(a);
        return;
    }
    if (w16_menu_open_p(&g_menu)) {
        unsigned id = w16_menu_key(&g_menu, key);
        if (id && id < W16_MENU_RIGHT) syscommand(id);
        uapp_redraw(a);
        return;
    }
    if (g_bar_hot >= 0) {
        pthread_mutex_lock(&g_w16.lock);
        int n = w16_menubar_count(&g_w16);
        int t = key > ' ' && key < 0x7F ? w16_menubar_mnemonic(&g_w16, key) : -1;
        pthread_mutex_unlock(&g_w16.lock);
        int hot = g_bar_hot;
        g_bar_hot = -1;
        if (n && key == KEY_ARROW_LEFT) g_bar_hot = (hot + n - 1) % n;
        else if (n && key == KEY_ARROW_RIGHT) g_bar_hot = (hot + 1) % n;
        else if (key == KEY_ARROW_DOWN || key == KEY_ARROW_UP || key == '\n' || key == '\r') bar_request(a, hot, 1);
        else if (t >= 0) bar_request(a, t, 1);
        else if (key != 0x1B) g_bar_hot = hot;   // anything else is ignored, as 3.1 beeped and stayed
        uapp_redraw(a);
        return;
    }
    if (key == ' ' && (mods & KEY_MOD_ALT)) {   // Alt+Space: the control menu, through the program
        syscommand(SC_KEYMENU);
        return;
    }
    // F10 lights the bar's first title; Alt and a title's letter drops that menu.
    if (g_w16.win.menubar && key == KEY_F10) {
        pthread_mutex_lock(&g_w16.lock);
        g_bar_hot = w16_menubar_count(&g_w16) ? 0 : -1;
        pthread_mutex_unlock(&g_w16.lock);
        uapp_redraw(a);
        return;
    }
    if (g_w16.win.menubar && (mods & KEY_MOD_ALT) && key > ' ' && key < 0x7F) {
        pthread_mutex_lock(&g_w16.lock);
        int t = w16_menubar_mnemonic(&g_w16, key);
        pthread_mutex_unlock(&g_w16.lock);
        if (t >= 0) { bar_request(a, t, 1); return; }
    }
    w16_route_key(&g_w16, &g_w16.win, key, mods, 0);
}

static void on_key_up(struct uapp *a, int key, unsigned mods) {
    (void)a; (void)mods;
    if (g_faulted || w16_menu_open_p(&g_menu) || bar_open() || g_bar_hot >= 0) return;
    w16_route_key(&g_w16, &g_w16.win, key, mods, 1);
}

static void on_focus(struct uapp *a, int focused) {
    if (!focused) g_bar_hot = -1;
    // WM_ACTIVATE, whose DefWindowProc gives the window -- or the program,
    // a child of it -- the keyboard focus.
    w16_post_msg(&g_w16, g_w16.win.hwnd, W16_WM_ACTIVATE, (uint16_t)(focused != 0), 0);
    uapp_redraw(a);                      // the caption's colour follows focus
}

static void on_resize(struct uapp *a, int w, int h) {
    struct w16_wnd *win = &g_w16.win;
    w = w16_down(w);
    h = w16_down(h);
    pthread_mutex_lock(&g_w16.lock);
    win->w = w;
    win->h = h;
    win->maximized = uapp_maximized(a);
    int max = win->maximized;
    int fitted = w16_top_fit(win) > 0, cw = win->cw, ch = win->ch;
    pthread_mutex_unlock(&g_w16.lock);
    if (fitted) {
        w16_invalidate(&g_w16, win, 1);
        w16_post_msg(&g_w16, win->hwnd, WM_SIZE, max ? SIZE_MAXIMIZED : SIZE_RESTORED,
                     (uint32_t)ch << 16 | (uint16_t)cw);
    }
    w16_menu_close(&g_menu);
    bar_close(a);
    uapp_redraw(a);
}

static int on_close(struct uapp *a) {
    (void)a;
    if (g_faulted) end_faulted();        // closing the stopped program's window is its Close
    syscommand(SC_CLOSE);                // Alt+F4 or the taskbar: Windows' SC_CLOSE, then WM_CLOSE
    return 0;                            // the program decides, and ends itself
}

static int on_user(struct uapp *a, int code, int arg) {
    switch (code) {
    case W16_POST_DONE:
        // RECORDED WHILE THE WINDOW STILL EXISTS: the launcher re-reads
        // this when its own window gets focus back, which is the moment
        // this one goes -- written after, it would read the run before.
        record_last("ended");
        uapp_quit(a, g_w16.exit_code);
        return 0;
    case W16_POST_SYSCMD:
        do_syscmd(a, arg);
        return 1;
    case W16_POST_SIZE: {                // MoveWindow: the surface follows the window
        pthread_mutex_lock(&g_w16.lock);
        int ww = g_w16.win.w, wh = g_w16.win.h;
        pthread_mutex_unlock(&g_w16.lock);
        uapp_resize(a, w16_up(ww), w16_up(wh));
        return 1;
    }
    case W16_POST_MENU_SHOW: bar_show(arg); return 1;
    case W16_POST_CURSOR: uapp_set_cursor(a, arg); return 0;
    case W16_POST_POINTER:              // w16's screen is the window: its 1x point, scaled
        uapp_set_pointer(a, w16_up((int16_t)((uint32_t)arg >> 16)), w16_up((int16_t)arg));
        return 0;
    case W16_POST_TITLE: {
        // A top-level window's new title: the taskbar's and toywm's too.
        pthread_mutex_lock(&g_w16.lock);
        struct w16_wnd *x = w16_wnd_get(&g_w16, (uint16_t)arg);
        char *t = x && x->text ? strdup(x->text) : 0;
        struct uapp_window *u = x ? x->uwin : 0;
        int main_win = x == &g_w16.win;
        pthread_mutex_unlock(&g_w16.lock);
        if (t && main_win) uapp_set_title(a, t);
        else if (t && u) uapp_window_set_title(u, t);
        free(t);
        if (u) uapp_window_redraw(u);
        return 1;
    }
    case W16_POST_DLG_OPEN:  w16_dlgwin_open(&g_w16, a, (uint16_t)arg); return 1;
    case W16_POST_DLG_CLOSE: w16_dlgwin_close(&g_w16, (uint16_t)arg); return 1;
    case W16_POST_DLG_PAINT: w16_dlgwin_redraw(&g_w16, (uint16_t)arg); return 0;
    case W16_POST_FAULT:
        w16_menu_close(&g_menu);
        g_pressed = 0;
        g_faulted = 1;
        if (g_w16_prefs.on_fault == W16_FAULT_REPORT) end_faulted();   // the report alone
        strlcpy(g_box.caption, "Application Error", sizeof g_box.caption);
        strlcpy(g_box.text, g_w16_fault_box, sizeof g_box.text);
        g_box.on_dismiss = end_faulted;
        w16_msgbox_open(&g_box, g_w16.win.w, g_w16.win.h);
        if (!g_box.popup) end_faulted();   // no box to show (minimized, no room): end now
        return 1;
    default:
        w16_dlgwin_redraw_all(&g_w16);   // W16_POST_PAINT: copy the new pixels
        return 1;
    }
}

// The scale the settings ask for: Auto is 2x on a screen 1600 pixels wide
// or more, where a 3.1 program's 640x480 world is otherwise a postage stamp.
// In quarters: the step asked for, or Auto's (w16prefs.h).
static int pick_scale(void) {
    if (g_w16_prefs.scale > W16_SCALE_AUTO && g_w16_prefs.scale < W16_SCALE_COUNT)
        return w16_prefs_scale_q[g_w16_prefs.scale];
    return w16_prefs_auto_q();
}

static int run(const char *path, const uint8_t *img, uint32_t size) {
    struct w16 *w = &g_w16;
    g_w16_q = pick_scale();
    g_w16_method = g_w16_prefs.method;
    g_w16_fonts = g_w16_prefs.fonts;
    g_w16_desktop_frame = g_w16_prefs.frame == W16_FRAME_DESKTOP;
    w16_dos_start(g_exe);
    if (w16_load(w, img, size, path, g_tail) < 0) return 1;
    ugfx_font_init();
    pthread_t task;
    if (pthread_create(&task, NULL, w16_task_main, w) != 0) {
        fprintf(stderr, "w16: cannot start the program's thread\n");
        return 1;
    }
    pthread_mutex_lock(&w->lock);
    while (!w->window_ready && !w->task_done) pthread_cond_wait(&w->cond, &w->lock);
    int ready = w->window_ready;
    pthread_mutex_unlock(&w->lock);
    if (!ready) {                        // ended, or faulted, before opening a window
        pthread_join(task, NULL);
        if (w->cpu.fault) { record_fault(); abort(); }   // the headline was set as it stopped
        record_last("ended");
        return w->exit_code;
    }
    // EACH PROGRAM ITS OWN IDENTITY: toywm remembers a window's size and
    // groups the taskbar by app_id, and one shared "w16" gave every
    // program the last one's size. A scaled window is another identity:
    // the size remembered at 1x would undo scale=2.
    static char appid[40];
    if (g_w16_q != 4) snprintf(appid, sizeof appid, "w16-%s-%gx", w->ne.name, g_w16_q / 4.0);
    else snprintf(appid, sizeof appid, "w16-%s", w->ne.name);
    for (char *p = appid; *p; p++)
        *p = *p >= 'A' && *p <= 'Z' ? (char)(*p + 32) : (*p == ' ' || *p == '/') ? '-' : *p;
    // A COPY, taken under the lock: the program runs on, and a SetWindowText
    // frees win.text -- the pointer itself once named whatever reused it.
    pthread_mutex_lock(&w->lock);
    static char *first_title;
    first_title = strdup(w->win.text ? w->win.text : "");
    pthread_mutex_unlock(&w->lock);
    struct uapp_desc desc = {
        .title = first_title ? first_title : "",
        // Sized in REAL pixels: the window's 1x size times the scale.
        .app_id = appid,
        .flags = (g_w16_desktop_frame ? 0 : UAPP_NO_DECOR) |
                 (w->win.style & WS_THICKFRAME ? UAPP_RESIZABLE : 0),
        .w = w16_up(w->win.w),
        .h = w16_up(w->win.h),
        .min_w = w16_up(120),
        .min_h = w16_up(2 * W16_FRAME + W16_CAPTION + 1),
        .on_open = on_open,
        .on_draw = on_draw,
        .on_press = on_press,
        .on_release = on_release,
        .on_motion = on_motion,
        .on_key = on_key,
        .on_key_up = on_key_up,
        .on_focus = on_focus,
        .on_resize = on_resize,
        .on_close = on_close,
        .on_user = on_user,
        .on_tick = on_tick,
        .tick_ms = 50,                   // timers: 3.1's ran at 55 ms
    };
    int r = uapp_run(&desc);
    return r ? r : w->exit_code;
}

// `--check`: what the launcher shows of a program, as lines -- its module
// and description, how many of the Windows functions it imports w16 has
// and which it lacks, and its icon as rows of AARRGGBB.
static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static int check(const uint8_t *img, uint32_t size) {
    static struct ne_module m;
    int err = ne_parse(img, size, &m);
    if (err < 0) {
        const char *what = ne_what(img, size);
        if (what) printf("foreign %s\n", what);      // the run card's "cannot run here"
        printf("error %s\n", ne_strerror(err));
        return 1;
    }
    char desc[256], mods[NE_MAX_MODULES][16];
    printf("module %s\n", m.name);
    if (ne_description(&m, desc, sizeof desc) == 0 && desc[0]) printf("description %s\n", desc);
    for (unsigned i = 1; i <= m.nmod; i++)
        if (ne_module_name(&m, i, mods[i - 1], sizeof mods[0]) < 0) snprintf(mods[i - 1], sizeof mods[0], "?");
    // Each import once: module << 16 | ordinal, collected then sorted.
    uint32_t *seen = 0;
    unsigned nseen = 0, cap = 0, has = 0, by_name = 0;
    for (unsigned s = 1; s <= m.nseg; s++)
        for (unsigned i = 0; i < m.seg[s - 1].nrelocs; i++) {
            struct ne_reloc r;
            if (ne_reloc_at(&m, s, i, &r) < 0) continue;
            if ((r.flags & 3) == NE_REL_IMPORT_NAME) { by_name++; continue; }
            if ((r.flags & 3) != NE_REL_IMPORT_ORD || r.a < 1 || r.a > m.nmod) continue;
            if (nseen == cap) {
                uint32_t *g = realloc(seen, sizeof *seen * (cap = cap ? cap * 2 : 256));
                if (!g) { printf("error out of memory\n"); return 1; }
                seen = g;
            }
            seen[nseen++] = (uint32_t)r.a << 16 | r.b;
        }
    qsort(seen, nseen, sizeof *seen, cmp_u32);
    unsigned u = 0;
    for (unsigned k = 0; k < nseen; k++)
        if (!u || seen[u - 1] != seen[k]) seen[u++] = seen[k];
    nseen = u;
    unsigned partly = 0;
    for (unsigned k = 0; k < nseen; k++) {
        const char *mod = mods[(seen[k] >> 16) - 1];
        has += w16_has(mod, seen[k] & 0xFFFF);
        partly += w16_partly(mod, seen[k] & 0xFFFF) != 0;
    }
    // `has` counts the stand-ins too: `partly` is how many of it are one.
    printf("imports %u\nhas %u\npartly %u\n", nseen + by_name, has, partly);
    // "call missing USER.1 MessageBox Dialogs": whether w16 has it (has,
    // partly, missing), the import, its name ("?" unknown) and its kind,
    // which may hold spaces -- then, for a stand-in, a tab and what it does.
    for (unsigned k = 0; k < nseen; k++) {
        const char *mod = mods[(seen[k] >> 16) - 1];
        unsigned ord = seen[k] & 0xFFFF;
        const struct w16_name *n = w16_name_lookup(mod, ord);
        const char *note = w16_partly(mod, ord);
        printf("call %s %s.%u %s %s%s%s\n", note ? "partly" : w16_has(mod, ord) ? "has" : "missing", mod, ord,
               n ? n->name : "?", w16_kind_names[n ? n->kind : w16_kind_other], note ? "\t" : "", note ? note : "");
    }
    free(seen);
    static uint32_t px[32 * 32];
    int iw, ih;
    if (w16_icon(&m, px, &iw, &ih) == 0) {
        printf("icon %d %d\n", iw, ih);
        for (int y = 0; y < ih; y++) {
            for (int x = 0; x < iw; x++) printf("%08x", px[y * iw + x]);
            printf("\n");
        }
    }
    return 0;
}

static void mkdirs(const char *path) {
    char p[W16_PATH_MAX];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; (s = strchr(s, '/')); s++) { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (g_fonts) {                               // drive C:'s WINDOWS\SYSTEM, read in the sandbox
        if (a.argc) return uargs_error(&PROG, "--fonts takes no FILE");
        w16_prefs_load(&g_w16_prefs, 0);
        snprintf(g_w16_drive_c, sizeof g_w16_drive_c, "%s", g_w16_prefs.drive_c);
        if (unveil(g_w16_drive_c, "r") != 0 || unveil(NULL, NULL) != 0 || pledge("stdio rpath", NULL) != 0) {
            fprintf(stderr, "w16: cannot sandbox myself, so I will not read the fonts\n");
            return 1;
        }
        w16_fonts_list(stdout);
        return 0;
    }
    if (a.argc < 1 || ((g_info || g_check) && a.argc != 1)) return uargs_error(&PROG, "expected one FILE");
    const char *path = a.argv[0];
    if (path[0] == '/') snprintf(g_exe, sizeof g_exe, "%s", path);
    else {
        char cwd[W16_PATH_MAX];
        snprintf(g_exe, sizeof g_exe, "%s/%s", getcwd(cwd, sizeof cwd) ? cwd : "", path);
    }

    // --info and --check read one file. A run also needs the window, its
    // fonts, the settings a window draws with, and the program's own:
    // its settings (read before the sandbox closes), drive C: for its INI
    // files, and the state directory its last run is recorded in.
    int ok;
    if (g_info || g_check) {
        ok = unveil(path, "r") == 0 && unveil(NULL, NULL) == 0 && pledge("stdio rpath", NULL) == 0;
    } else {
        w16_prefs_load(&g_w16_prefs, g_exe);
        snprintf(g_w16_drive_c, sizeof g_w16_drive_c, "%s", g_w16_prefs.drive_c);
        mkdirs(g_w16_drive_c);
        mkdirs(W16_STATE_DIR);
        static char trace[W16_PATH_MAX + 32];
        if (!g_trace_path && g_w16_prefs.trace) {
            w16_trace_path(g_exe, trace, sizeof trace);
            g_trace_path = trace;
        }
        // Drive Z: is the whole tree to read, and /home to write as well
        // (file16.c) -- what a program can reach of toy-os's files, Wine's
        // shape with the system's own folders kept read-only.
        ok = unveil("/", "r") == 0 && unveil("/home", "rwc") == 0
             && unveil(g_w16_drive_c, "rwc") == 0 && unveil(W16_STATE_DIR, "rwc") == 0
             && unveil("shm:" CLIP_SHM_NAME, "rw") == 0
             && unveil("shm:toywm", "r") == 0 && unveil("shm:" FONT_BEACON_NAME, "r") == 0
             && unveil("shm:font.0", "r") == 0 && unveil("shm:font.1", "r") == 0
             && unveil("shm:font.2", "r") == 0 && unveil("shm:font.3", "r") == 0
             && (!g_trace_path || unveil(g_trace_path, "rwc") == 0)
             && unveil(NULL, NULL) == 0
             && pledge("stdio rpath wpath cpath shm window settings query", NULL) == 0;
    }
    if (!ok) {
        fprintf(stderr, "w16: cannot sandbox myself, so I will not read %s\n", path);
        return 1;
    }
    if (g_trace_path && !(g_w16_trace = fopen(g_trace_path, "w"))) {
        fprintf(stderr, "w16: %s: %s\n", g_trace_path, strerror(errno));
        return 1;
    }
    uint32_t size = 0;
    uint8_t *img = slurp(path, &size);
    if (!img) {
        fprintf(stderr, "w16: %s: %s\n", path,
                size == UINT32_MAX ? "larger than any NE program" : strerror(errno));
        return 1;
    }
    // What follows the program is its command line -- a document to open,
    // most often -- and a toy-os path in it becomes the DOS one the
    // program can open (Z:\tests\sample.txt).
    for (int i = 1; i < a.argc; i++) {
        char dos[W16_PATH_MAX * 2];
        const char *arg = a.argv[i][0] == '/' && w16_host_to_dos(a.argv[i], dos, sizeof dos) == 0 ? dos : a.argv[i];
        size_t len = strlen(g_tail);
        if ((size_t)snprintf(g_tail + len, sizeof g_tail - len, "%s%s", len ? " " : "", arg) >= sizeof g_tail - len)
            return uargs_error(&PROG, "the command line is longer than DOS's 126 characters");
    }
    if (g_check) return check(img, size);
    if (!g_info) return run(path, img, size);
    static struct ne_module m;
    int err = ne_parse(img, size, &m);
    if (err < 0) {
        fprintf(stderr, "w16: %s: %s\n", path, ne_strerror(err));
        free(img);
        return 1;
    }
    ne_print_info(&m);
    free(img);
    return 0;
}
