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
#include "rt/sys.h"
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "w16/w16_internal.h"

#define W16_FILE_MAX (16u << 20)   // NE files are at most a few MiB

#define VK_BACK   0x08
#define VK_TAB    0x09
#define VK_RETURN 0x0D
#define VK_ESCAPE 0x1B
#define MK_LBUTTON 0x0001
#define MK_RBUTTON 0x0002

static int g_info;
static const char *g_trace_path;

static const struct uargs_opt OPTS[] = {
    { "info", 'i', NULL, "print the program's segments and the functions it imports", &g_info, NULL },
    { "trace", 't', "FILE", "log every Windows call the program makes to FILE (Wine's +relay)",
      NULL, &g_trace_path },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "w16",
    .usage = "FILE\n--info FILE",
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

static uint32_t client_lparam(int x, int y) {
    int cx, cy, cw, ch;
    w16_client_rect(&g_w16.win, &cx, &cy, &cw, &ch);
    return (uint32_t)(uint16_t)(y - cy) << 16 | (uint16_t)(x - cx);
}

static void syscommand(unsigned sc) {
    w16_post_msg(&g_w16, g_w16.win.hwnd, WM_SYSCOMMAND, (uint16_t)sc, 0);
}

static int maximized(void) {
    pthread_mutex_lock(&g_w16.lock);
    int m = g_w16.win.maximized;
    pthread_mutex_unlock(&g_w16.lock);
    return m;
}

// The program stopped and its box is answered (or could not be shown):
// end with its message as the crash report's headline.
static void end_faulted(void) {
    w16_msgbox_close(&g_box);
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
    int pending = g_w16.pending_syscmd;
    g_w16.pending_syscmd = 0;
    pthread_mutex_unlock(&g_w16.lock);
    if (pending) do_syscmd(a, pending);   // ShowWindow(SW_MAXIMIZE) before the window was up
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    struct ugfx_surface *s = uapp_surface(d);
    struct w16_wnd *win = &g_w16.win;
    pthread_mutex_lock(&g_w16.lock);
    w16_frame_draw(s, win, uapp_focused(a), w16_menu_open_p(&g_menu) ? HIT_SYSMENU : g_pressed);
    int cx, cy, cw, ch;
    w16_client_rect(win, &cx, &cy, &cw, &ch);
    if (win->fb) ugfx_blit(s, cx, cy, win->cw, win->ch, win->fb, win->cw);
    pthread_mutex_unlock(&g_w16.lock);
    w16_menu_draw(&g_menu);
    w16_msgbox_draw(&g_box);
}

static void on_press(struct uapp *a, int x, int y, unsigned buttons) {
    if (g_faulted) {                     // modal: only the box's button
        w16_msgbox_press(&g_box, x, y);
        uapp_redraw(a);
        return;
    }
    unsigned edges;
    enum w16_hit hit = w16_frame_hit(&g_w16.win, x, y, &edges);
    uint64_t now = sys_ticks();
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
            if (went & bit)
                w16_post_msg(&g_w16, g_w16.win.hwnd, button_msg(bit, 1), mk_flags(buttons),
                             client_lparam(x, y));
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
    if (g_faulted) {
        if (w16_msgbox_release(&g_box, x, y)) end_faulted();
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
            if (went & bit)
                w16_post_msg(&g_w16, g_w16.win.hwnd, button_msg(bit, 0), mk_flags(buttons),
                             client_lparam(x, y));
}

static void on_motion(struct uapp *a, int x, int y, unsigned buttons) {
    if (g_faulted) return;
    if (w16_menu_open_p(&g_menu)) {
        int was = g_menu.sel;
        w16_menu_motion(&g_menu, x, y);
        if (g_menu.sel != was) uapp_redraw(a);
        return;
    }
    unsigned edges;
    if (mouse_is_programs(w16_frame_hit(&g_w16.win, x, y, &edges)))
        w16_post_msg(&g_w16, g_w16.win.hwnd, WM_MOUSEMOVE, mk_flags(buttons), client_lparam(x, y));
}

static int on_tick(struct uapp *a) {
    (void)a;
    w16_timers_fire(&g_w16);
    return 0;
}

static int vk_of(int key) {
    if (key >= 'a' && key <= 'z') return key - 32;
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') || key == ' ') return key;
    switch (key) {
    case '\n': case '\r': return VK_RETURN;
    case '\b': return VK_BACK;
    case '\t': return VK_TAB;
    case 0x1B: return VK_ESCAPE;
    case KEY_ARROW_LEFT: return 0x25;
    case KEY_ARROW_UP: return 0x26;
    case KEY_ARROW_RIGHT: return 0x27;
    case KEY_ARROW_DOWN: return 0x28;
    case KEY_PAGE_UP: return 0x21;
    case KEY_PAGE_DOWN: return 0x22;
    case KEY_END: return 0x23;
    case KEY_HOME: return 0x24;
    case KEY_DELETE: return 0x2E;
    default: return 0;
    }
}

// toywm hands over the TRANSLATED key, so the character is known here:
// WM_KEYDOWN and then WM_CHAR are posted together, and TranslateMessage
// (user16.c) has nothing left to do.
static void on_key(struct uapp *a, int key, unsigned mods) {
    if (g_faulted) {
        if (w16_msgbox_key(&g_box, key)) end_faulted();
        return;
    }
    if (w16_menu_open_p(&g_menu)) {
        unsigned id = w16_menu_key(&g_menu, key);
        if (id && id != (unsigned)-1) syscommand(id);
        uapp_redraw(a);
        return;
    }
    if (key == ' ' && (mods & KEY_MOD_ALT)) {   // Alt+Space: the control menu, through the program
        syscommand(SC_KEYMENU);
        return;
    }
    int vk = vk_of(key);
    if (vk) w16_post_msg(&g_w16, g_w16.win.hwnd, WM_KEYDOWN, (uint16_t)vk, 1);
    int ch = key == '\n' ? '\r' : key;
    if ((ch >= 0x20 && ch < 0x7F) || ch == '\r' || ch == '\b' || ch == '\t' || ch == 0x1B)
        w16_post_msg(&g_w16, g_w16.win.hwnd, WM_CHAR, (uint16_t)ch, 1);
}

static void on_key_up(struct uapp *a, int key, unsigned mods) {
    (void)a; (void)mods;
    if (g_faulted || w16_menu_open_p(&g_menu)) return;
    int vk = vk_of(key);
    if (vk) w16_post_msg(&g_w16, g_w16.win.hwnd, WM_KEYUP, (uint16_t)vk, 0xC0000001u);
}

static void on_focus(struct uapp *a, int focused) {
    (void)focused;
    uapp_redraw(a);                      // the caption's colour follows focus
}

static void on_resize(struct uapp *a, int w, int h) {
    struct w16_wnd *win = &g_w16.win;
    pthread_mutex_lock(&g_w16.lock);
    win->w = w;
    win->h = h;
    win->maximized = uapp_maximized(a);
    int max = win->maximized;
    int cx, cy, cw, ch;
    w16_client_rect(win, &cx, &cy, &cw, &ch);
    uint32_t *fb = calloc((size_t)cw * ch + 1, 4);
    if (fb) {
        free(win->fb);
        win->fb = fb;
        win->cw = cw;
        win->ch = ch;
        win->inval = win->erase = 1;
    }
    pthread_mutex_unlock(&g_w16.lock);
    if (fb) w16_post_msg(&g_w16, win->hwnd, WM_SIZE, max ? SIZE_MAXIMIZED : SIZE_RESTORED,
                         (uint32_t)ch << 16 | (uint16_t)cw);
    w16_menu_close(&g_menu);
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
        uapp_quit(a, g_w16.exit_code);
        return 0;
    case W16_POST_SYSCMD:
        do_syscmd(a, arg);
        return 1;
    case W16_POST_SIZE: {                // MoveWindow: the surface follows the window
        pthread_mutex_lock(&g_w16.lock);
        int ww = g_w16.win.w, wh = g_w16.win.h;
        pthread_mutex_unlock(&g_w16.lock);
        uapp_resize(a, ww, wh);
        return 1;
    }
    case W16_POST_FAULT:
        w16_menu_close(&g_menu);
        g_pressed = 0;
        g_faulted = 1;
        strlcpy(g_box.caption, "Application Error", sizeof g_box.caption);
        strlcpy(g_box.text, g_w16_fault_box, sizeof g_box.text);
        g_box.on_dismiss = end_faulted;
        w16_msgbox_open(&g_box, g_w16.win.w, g_w16.win.h);
        if (!g_box.popup) end_faulted();   // no box to show (minimized, no room): end now
        return 1;
    default:
        return 1;                        // W16_POST_PAINT: copy the new pixels
    }
}

static int run(const char *path, const uint8_t *img, uint32_t size) {
    struct w16 *w = &g_w16;
    if (w16_load(w, img, size, path) < 0) return 1;
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
        if (w->cpu.fault) abort();       // the report's headline was set as it stopped
        return w->exit_code;
    }
    // EACH PROGRAM ITS OWN IDENTITY: toywm remembers a window's size and
    // groups the taskbar by app_id, and one shared "w16" gave every
    // program the last one's size.
    static char appid[40];
    snprintf(appid, sizeof appid, "w16-%s", w->ne.name);
    for (char *p = appid; *p; p++)
        *p = *p >= 'A' && *p <= 'Z' ? (char)(*p + 32) : (*p == ' ' || *p == '/') ? '-' : *p;
    struct uapp_desc desc = {
        .title = w->win.title,
        .app_id = appid,
        .flags = UAPP_NO_DECOR | (w->win.style & WS_THICKFRAME ? UAPP_RESIZABLE : 0),
        .w = w->win.w,
        .h = w->win.h,
        .min_w = 120,
        .min_h = 2 * W16_FRAME + W16_CAPTION + 1,
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

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc != 1) return uargs_error(&PROG, "expected one FILE");
    const char *path = a.argv[0];

    // --info reads one file; a run also needs the window, its fonts and
    // the settings and theme a window draws with.
    int ok = unveil(path, "r") == 0;
    if (!g_info)
        ok = ok && unveil("/usr/share", "r") == 0 && unveil("/etc", "r") == 0
                && unveil("shm:toywm", "r") == 0 && unveil("shm:" FONT_BEACON_NAME, "r") == 0
                && unveil("shm:font.0", "r") == 0 && unveil("shm:font.1", "r") == 0
                && unveil("shm:font.2", "r") == 0 && unveil("shm:font.3", "r") == 0;
    if (g_trace_path) ok = ok && unveil(g_trace_path, "rwc") == 0;
    ok = ok && unveil(NULL, NULL) == 0
            && pledge(g_info ? "stdio rpath"
                      : g_trace_path ? "stdio rpath wpath cpath shm window settings query"
                                     : "stdio rpath shm window settings query", NULL) == 0;
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
