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
#include <strings.h>
#include <unistd.h>
#include "font_shm.h"
#include "keyboard.h"
#include "win_proto.h"
#include "lib/uargs.h"
#include "lib/umd.h"
#include "lib/uwinsetup.h"   // --install
#include "lib/uclip_page.h"   // CLIP_SHM_NAME: the clipboard an EDIT copies to
#include "lib/udrive.h"   // a disc image mounted as a drive
#include "lib/uiso.h"
#include "rt/sys.h"
#include "query_abi.h"
#include "sound_abi.h"   // SND_SERVER_NAME, SND_CLIENT_PREFIX: the daemon w16 may reach
#include "ui/uapp.h"
#include "ui/ucrt.h"
#include "ui/ugfx.h"
#include "ui/uui_anim.h"
#include "w16/w16_internal.h"
#include "w16/w16prefs.h"

#define W16_FILE_MAX (16u << 20)   // NE files are at most a few MiB

static int g_info, g_check, g_fonts, g_sounds;
static char g_tail[127];                 // the program's command line: DOS's 126 characters
static const char *g_trace_path;
static const char *g_why;                // --why CALL
static const char *g_install;            // --install DISKS
static int g_dry_run, g_replace_ini, g_groups;
static char g_exe[W16_PATH_MAX];         // the program, as an absolute path: its settings' key

static const struct uargs_opt OPTS[] = {
    { "info", 'i', NULL, "print the program's segments and the functions it imports", &g_info, NULL },
    { "check", 'c', NULL, "list what the program imports, which of it w16 has, its kind, and the icon",
      &g_check, NULL },
    { "fonts", 'f', NULL, "list Windows 3.11's own fonts found on drive C:, with a sample of each face",
      &g_fonts, NULL },
    { "sounds", 's', NULL, "list each sound event, the file it plays and whether drive C: has it",
      &g_sounds, NULL },
    { "trace", 't', "FILE", "log every Windows call the program makes to FILE (Wine's +relay)",
      NULL, &g_trace_path },
    { "why", 'w', "CALL", "say what CALL (USER.121) does and how w16 answers it, from the calls page",
      NULL, &g_why },
    { "skip-missing", 0, NULL, "answer a call w16 lacks with 0 and run on, naming it on stderr and in the "
      "last-run record -- the program may get further, or go wrong where nothing says why (the `missing` "
      "setting chooses it for good)", &g_w16_skip_missing, NULL },
    { "install", 0, "DISKS", "install Windows 3.x onto drive C: from its disks: a folder of their files, "
      "a folder per disk, or the floppy images", NULL, &g_install },
    { "dry-run", 'n', NULL, "with --install, say what would go where and change nothing", &g_dry_run, NULL },
    { "replace-ini", 0, NULL, "with --install, make WIN.INI, SYSTEM.INI and CONTROL.INI afresh even "
      "where drive C: has them", &g_replace_ini, NULL },
    { "groups", 'g', NULL, "make Program Manager a group for each of Retro Launcher's folders, on drive C: "
      "(RL1.GRP...), and take away those of folders no longer in the list", &g_groups, NULL },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "w16",
    .usage = "FILE [ARGUMENT...]\n--info FILE\n--check FILE\n--fonts\n--sounds\n--why CALL\n"
             "--install DISKS [--dry-run] [--replace-ini]\n--groups",
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
// ...a CHILD's, when g_menu_child names it (an MDI document's): its choices go to that window, and a
// second press on its box (g_child_box, window pixels at 1x) within the double-click time closes it.
static uint16_t g_menu_child;
static int g_child_box[4];
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

// --- the screen effect (ui/ucrt.h) over the client area ----------------------------
//
// Run after the window is drawn at its scale, over the client's screen
// rect only, a scanline per 3.1 row. g_fx_* is the rect it last ran on, in
// window pixels: the pointer is mapped through the curve there.
static struct ucrt g_crt;
static int g_fx_on, g_fx_x, g_fx_y, g_fx_w, g_fx_h;

// The control menu's Screen effect items: ids past a 16-bit WM_SYSCOMMAND's,
// so no program's own item can be one, and never sent to the program.
#define FX_ID 0x10000u                   // + 0 Off, + 1 + i ucrt_presets[i], + FX_CUSTOM
#define FX_CUSTOM (1u + UCRT_PRESET_COUNT)   // its own look, on: shown only when it has one

// The client as drawn and as effected, kept so a redraw that changed
// nothing under the glass -- a menu's lit row, the caption on focus --
// copies the last result instead of running every pass again.
static uint32_t *g_fx_in, *g_fx_out;
static size_t g_fx_cap;
static int g_fx_kept;                    // g_fx_in/out hold the rect and look below
static struct ucrt_look g_fx_look;

static void rect_copy(uint32_t *dst, const struct ugfx_surface *s, int x, int y, int w, int h, int to_surface) {
    for (int r = 0; r < h; r++) {
        uint32_t *row = s->pixels + (size_t)(y + r) * s->w + x, *k = dst + (size_t)r * w;
        if (to_surface) memcpy(row, k, (size_t)w * 4);
        else memcpy(k, row, (size_t)w * 4);
    }
}

static int rect_same(const uint32_t *k, const struct ugfx_surface *s, int x, int y, int w, int h) {
    for (int r = 0; r < h; r++)
        if (memcmp(s->pixels + (size_t)(y + r) * s->w + x, k + (size_t)r * w, (size_t)w * 4)) return 0;
    return 1;
}

static void draw_effect(struct ugfx_surface *s, int cx, int cy, int cw, int ch) {
    g_fx_on = 0;
    if (!g_w16_prefs.effect || !ucrt_look_on(&g_w16_prefs.crt) || cw <= 0 || ch <= 0) return;
    int x = w16_up(cx), y = w16_up(cy), w = w16_up(cx + cw) - x, h = w16_up(cy + ch) - y;
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0) return;
    int animates = ucrt_animates(&g_w16_prefs.crt), same_place = g_fx_kept && x == g_fx_x && y == g_fx_y &&
        w == g_fx_w && h == g_fx_h && !memcmp(&g_fx_look, &g_w16_prefs.crt, sizeof g_fx_look);
    if (same_place && !animates && rect_same(g_fx_in, s, x, y, w, h)) {
        rect_copy(g_fx_out, s, x, y, w, h, 1);
        g_fx_on = 1;
        return;
    }
    g_crt.look = g_w16_prefs.crt;
    g_crt.src_lines = ch;
    g_crt.period = h / ch < 2 ? 2 : h / ch;
    g_crt.bezel = 0;
    size_t need = (size_t)w * h;
    if (need > g_fx_cap) {
        free(g_fx_in);
        free(g_fx_out);
        g_fx_in = malloc(need * 4);
        g_fx_out = malloc(need * 4);
        g_fx_cap = g_fx_in && g_fx_out ? need : 0;
    }
    g_fx_kept = 0;
    if (g_fx_cap && !animates) rect_copy(g_fx_in, s, x, y, w, h, 0);
    if (ucrt_apply(&g_crt, s, x, y, w, h) != 0) return;   // no memory: left plain
    if (g_fx_cap && !animates) {
        rect_copy(g_fx_out, s, x, y, w, h, 0);
        g_fx_kept = 1;
        g_fx_look = g_w16_prefs.crt;
    }
    g_fx_on = 1;
    g_fx_x = x; g_fx_y = y; g_fx_w = w; g_fx_h = h;
    if (animates) uui_anim_request();
}

// Where a window point the program asked for (SetCursorPos) shows on the
// curved glass, so the pointer lands on what it drew there.
static void warp_to_glass(int *x, int *y) {
    if (!g_fx_on || g_crt.look.curve == UCRT_CURVE_OFF) return;
    if (*x < g_fx_x || *y < g_fx_y || *x >= g_fx_x + g_fx_w || *y >= g_fx_y + g_fx_h) return;
    int px, py;
    ucrt_glass_point(&g_crt, *x - g_fx_x, *y - g_fx_y, &px, &py);
    *x = g_fx_x + px;
    *y = g_fx_y + py;
}

// A window point as the program drew it: on the curved glass, the point
// that was drawn where it shows (the bezel, the nearest point on the glass).
static void unwarp(int *x, int *y) {
    if (!g_fx_on || g_crt.look.curve == UCRT_CURVE_OFF) return;
    // A menu or the error box is flat, over the glass: its points are its own.
    if (w16_menu_open_p(&g_menu) || g_bar.popup > 0 || g_bar_pending || g_faulted) return;
    if (*x < g_fx_x || *y < g_fx_y || *x >= g_fx_x + g_fx_w || *y >= g_fx_y + g_fx_h) return;
    int sx, sy;
    ucrt_source_point(&g_crt, *x - g_fx_x, *y - g_fx_y, &sx, &sy);
    *x = g_fx_x + sx;
    *y = g_fx_y + sy;
}

// Window pixels to the program's 1x.
static void to_w16(int *x, int *y) {
    unwarp(x, y);
    *x = w16_down(*x);
    *y = w16_down(*y);
}

// The control menu: 3.1's, then the program's own items, then the effect's.
static void build_control_menu(void) {
    g_menu_child = 0;
    w16_sysmenu_build(&g_menu, &g_w16);
    if (g_w16_desktop_frame) return;
    w16_menu_add(&g_menu, "", "", 0, 0);
    struct w16_menu *fx = w16_menu_add_sub(&g_menu, "Scr&een effect", 0);
    if (!fx) return;
    int on = g_w16_prefs.effect && ucrt_look_on(&g_w16_prefs.crt), p = ucrt_preset_of(&g_w16_prefs.crt);
    w16_menu_add(fx, "&Off", "", FX_ID, on ? 0 : W16_MF_CHECKED);
    // ucrt_presets' names with a letter each, none Off's.
    static const char *const names[] = { "&Subtle", "So&ft CRT", "&Classic CRT", "C&urved" };
    _Static_assert(sizeof names / sizeof names[0] == UCRT_PRESET_COUNT, "a menu name per ucrt preset");
    for (int i = 0; i < UCRT_PRESET_COUNT; i++)
        w16_menu_add(fx, names[i], "", FX_ID + 1 + (unsigned)i, on && p == i ? W16_MF_CHECKED : 0);
    // A look of its own (Retro Launcher's Customise) stays reachable after Off.
    if (p < 0 && ucrt_look_on(&g_w16_prefs.crt))
        w16_menu_add(fx, "Cus&tom", "", FX_ID + FX_CUSTOM, on ? W16_MF_CHECKED : 0);
}

// Off (0), preset k - 1, or FX_CUSTOM (its own look, on), now and from now
// on: the program's own setting.
static void set_effect(struct uapp *a, unsigned k) {
    g_w16_prefs.effect = k > 0;
    if (k > 0 && k <= UCRT_PRESET_COUNT) g_w16_prefs.crt = ucrt_presets[k - 1];
    uapp_redraw(a);
    // The sandbox keeps this process from writing its settings: hlpbroker
    // saves them, for this program only.
    char line[32];
    int n = snprintf(line, sizeof line, "EFFECT\t%s\n", k == FX_CUSTOM ? "custom" : ucrt_effect_word((int)k));
    if (g_w16_help_fd < 0 || write(g_w16_help_fd, line, (size_t)n) < 0)
        fprintf(stderr, "w16: could not save the screen effect\n");
}

// A control-menu choice: the effect's, or a WM_SYSCOMMAND for the program.
static void control_chosen(struct uapp *a, unsigned id) {
    if (g_menu_child) w16_post_msg(&g_w16, g_menu_child, WM_SYSCOMMAND, (uint16_t)id, 0);
    else if (id >= FX_ID && id <= FX_ID + FX_CUSTOM) set_effect(a, id - FX_ID);
    else syscommand(id);
}

static int bar_open(void);
static void bar_request(struct uapp *a, int pos, int by_key);

// A captioned child's control menu, opened at its box (or over its icon), in the main window: a
// maximized child's box is the bar's first title, opened as one.
static void child_menu(struct uapp *a, uint16_t hwnd, int by_key) {
    if (w16_menu_open_p(&g_menu) || bar_open()) return;
    pthread_mutex_lock(&g_w16.lock);
    struct w16_wnd *x = w16_wnd_get(&g_w16, hwnd);
    int ok = x && x->kind == W16_K_CHILD && w16_wnd_top(&g_w16, x) == &g_w16.win && (x->style & WS_SYSMENU);
    int maxed = ok && x->maximized, box[4] = { 0 };
    if (ok && !maxed) {
        int ox, oy, l, t, r, b, cx, cy, cw, ch;
        w16_wnd_origin(&g_w16, x, &ox, &oy);
        w16_wnd_insets(x, &l, &t, &r, &b);
        w16_client_rect(&g_w16.win, &cx, &cy, &cw, &ch);
        int f = x->iconic ? 0 : w16_border_of(x->style);
        box[0] = cx + ox - l + f;
        box[1] = cy + oy - t + f;
        box[2] = x->iconic ? x->w : W16_MBMP_W - 1;
        box[3] = x->iconic ? x->h : W16_CAPTION - 1;
        w16_child_sysmenu_build(&g_menu, x);
    }
    pthread_mutex_unlock(&g_w16.lock);
    if (maxed) { bar_request(a, 0, by_key); return; }
    if (!ok) return;
    g_menu_child = hwnd;
    memcpy(g_child_box, box, sizeof box);
    g_sysmenu_tick = sys_ticks();
    w16_menu_open(&g_menu, box[0], box[1], box[2], box[3], by_key);
    uapp_redraw(a);
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
    w16_menubar_build(&g_w16.win, pos, &g_bar);
    int ok = w16_menubar_title_at(&g_w16.win, -(pos + 1), &tx, &tw) >= 0;
    pthread_mutex_unlock(&g_w16.lock);
    if (ok && g_bar.n) w16_menu_open(&g_bar, bx + tx, by, tw, bh - 1, g_bar_by_key);
    else g_bar_pos = -1;
}

// The bar title under window point x, or -1.
static int bar_title(int x) {
    int bx, by, bw, bh;
    pthread_mutex_lock(&g_w16.lock);
    w16_frame_menubar(&g_w16.win, &bx, &by, &bw, &bh);
    int t = bh ? w16_menubar_title_at(&g_w16.win, x - bx, 0, 0) : -1;
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

// What happened the last time the program ran, for the launcher: "<seconds since 1970> ended" or
// "... stopped <the fault's headline>", then the call it lacked, each call answered 0 and the trace
// written (w16prefs.h).
static void record_last(const char *what, const char *missing) {
    char path[W16_PATH_MAX + 32];
    w16_prog_last(g_exe, path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%ld %s\n", (long)time(0), what);
    if (missing && missing[0]) fprintf(f, W16_LAST_MISSING "%s\n", missing);
    w16_zeroed_write(f);
    if (g_w16_trace && g_trace_path && g_trace_path[0] == '/') fprintf(f, W16_LAST_TRACE "%s\n", g_trace_path);
    fclose(f);
}

static void record_fault(void) {
    char head[160], missing[sizeof g_w16.missing];
    size_t n = strcspn(g_w16_fault, "\n");
    snprintf(head, sizeof head, "stopped %.*s", (int)(n < 150 ? n : 150), g_w16_fault);
    // The call it lacked, "USER.7 (ExitWindows)" in the task's words, as "USER.7 ExitWindows".
    int k = 0;
    for (const char *c = g_w16.missing; *c && k < (int)sizeof missing - 1; c++)
        if (*c != '(' && *c != ')') missing[k++] = *c;
    missing[k] = 0;
    record_last(head, missing);
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
            build_control_menu();
            w16_menu_open(&g_menu, x, y, w, h, 1);
        }
        break;
    default: break;
    }
}

static void on_open(struct uapp *a) {
    pthread_mutex_lock(&g_w16.lock);
    int pending = g_w16.pending_syscmd, above = g_w16.pending_above;
    g_w16.pending_syscmd = g_w16.pending_above = 0;
    // The title as it is NOW: a program that retitled its window before
    // this one was up (Notepad's "Notepad - FILE.TXT") posted to no one.
    char *title = g_w16.win.text ? strdup(g_w16.win.text) : 0;
    // ...and the size: Calculator shrinks its scientific dialog to the
    // standard one before showing it, and that W16_POST_SIZE was dropped too.
    int ww = w16_up(g_w16.win.w), wh = w16_up(g_w16.win.h);
    pthread_mutex_unlock(&g_w16.lock);
    if (title) uapp_set_title(a, title);
    free(title);
    if (ww != uapp_width(a) || wh != uapp_height(a)) uapp_resize(a, ww, wh);
    if (pending) do_syscmd(a, pending);   // ShowWindow(SW_MAXIMIZE) before the window was up
    if (above) uapp_set_above(a, above - 1);   // Clock's saved Always on Top, set at start
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
    w16_frame_draw(out, win, uapp_focused(a), w16_menu_open_p(&g_menu) && !g_menu_child ? HIT_SYSMENU : g_pressed);
    int bx, by, bw, bh;
    w16_frame_menubar(win, &bx, &by, &bw, &bh);
    if (bh) w16_menubar_draw(out, &g_w16.win, bx, by, bw, bar_open() ? g_bar_pos : g_bar_hot);
    int cx, cy, cw, ch;
    w16_client_rect(win, &cx, &cy, &cw, &ch);
    // The last COMPLETE frame (w16_shown), never the buffer the program may
    // be halfway through painting; while a resize waits for its repaint,
    // what the old frame does not cover is the class background.
    int sw, sh;
    const uint32_t *px = w16_shown(win, &sw, &sh);
    if (px && win->fb) {
        int bw2 = sw < win->fbw ? sw : win->fbw, bh2 = sh < win->fbh ? sh : win->fbh;
        if (bw2 < win->fbw || bh2 < win->fbh) {
            int k = win->cls;
            uint32_t bg = k >= 0 && k < g_w16.ncls ? w16_brush_color(g_w16.cls[k].background) : 0xFFFFFF;
            ugfx_fill_rect(out, cx, cy, win->fbw, win->fbh, bg);
        }
        ugfx_blit(out, cx, cy, bw2, bh2, px, sw);
        w16_caret_draw(&g_w16, out, win, cx, cy);
    }
    static struct w16_scaler sc;
    if (out != s) w16_scaler_show(&sc, s, 0, 0, g_virt, win->w, win->h);
    pthread_mutex_unlock(&g_w16.lock);
    draw_effect(s, cx, cy, cw, ch);
    w16_menu_draw(&g_menu);
    w16_menu_draw(&g_bar);
    w16_msgbox_draw(&g_box);
}

static void on_press(struct uapp *a, int x, int y, unsigned buttons) {
    to_w16(&x, &y);
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
        int on_box = g_menu_child ? x >= g_child_box[0] && y >= g_child_box[1] && x < g_child_box[0] + g_child_box[2] &&
                                    y < g_child_box[1] + g_child_box[3]
                                  : hit == HIT_SYSMENU;
        if (!w16_menu_press(&g_menu, x, y) && on_box && now - g_sysmenu_tick <= w16_dclick_ticks()) {
            if (g_menu_child) {
                w16_menu_close(&g_menu);
                w16_post_msg(&g_w16, g_menu_child, WM_SYSCOMMAND, SC_CLOSE, 0);
            } else {
                syscommand(SC_CLOSE);
            }
            g_sysmenu_tick = 0;
        }
        uapp_redraw(a);
        return;
    }
    if (mouse_is_programs(hit)) {
        buttons = w16_mouse_swap(buttons);
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
        if (now - g_caption_tick <= w16_dclick_ticks() && (g_w16.win.style & WS_MAXIMIZEBOX)) {
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
        build_control_menu();
        w16_menu_open(&g_menu, bx, by, bw, bh, 0);
        g_sysmenu_tick = now;
        uapp_redraw(a);
        break;
    }
    default: break;
    }
}

static void on_release(struct uapp *a, int x, int y, unsigned buttons) {
    to_w16(&x, &y);
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
        if (id && id != (unsigned)-1) control_chosen(a, id);
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
    buttons = w16_mouse_swap(buttons);
    unsigned now_down = WIN_MOUSE_BUTTONS(buttons) & 3, went = g_buttons & ~now_down;
    g_buttons = now_down;
    if (mouse_is_programs(hit))
        for (unsigned bit = 1; bit <= 2; bit <<= 1)
            if (went & bit) client_mouse(button_msg(bit, 0), mk_flags(buttons), x, y);
}

// A program's own cursor reaches toywm the first time it is shown, and again
// once its bits or the scale they are drawn at change: g_cursor_at[i] is the
// (generation, scale) toywm holds image i at.
static struct { uint32_t gen; int q; } *g_cursor_at;
static int g_ncursor_at;

static void show_cursor(struct uapp *a, int shape) {
    if (shape >= WIN_CURSOR_IMAGE_BASE) {
        int i = shape - WIN_CURSOR_IMAGE_BASE;
        if (i >= g_ncursor_at) {
            void *q = realloc(g_cursor_at, sizeof *g_cursor_at * (size_t)(i + 1));
            if (!q) return;
            g_cursor_at = q;
            for (int k = g_ncursor_at; k <= i; k++) g_cursor_at[k].gen = 0, g_cursor_at[k].q = 0;
            g_ncursor_at = i + 1;
        }
        static uint32_t argb[W16_CURSOR_IMG_MAX * W16_CURSOR_IMG_MAX];
        static uint8_t inv[W16_CURSOR_IMG_MAX * W16_CURSOR_IMG_MAX];
        int w, h, hx, hy;
        uint32_t gen;
        if (w16_cursor_image(i, g_w16_q, 4, argb, inv, &w, &h, &hx, &hy, &gen) < 0) shape = WIN_CURSOR_DEFAULT;
        else if (g_cursor_at[i].gen != gen || g_cursor_at[i].q != g_w16_q) {
            if (uapp_cursor_image(a, i, w, h, hx, hy, argb, inv) < 0) shape = WIN_CURSOR_DEFAULT;
            else g_cursor_at[i].gen = gen, g_cursor_at[i].q = g_w16_q;
        }
    }
    uapp_set_cursor(a, shape);
}

static int edge_cursor(unsigned edges) {
    int h = edges & (WIN_EDGE_LEFT | WIN_EDGE_RIGHT), v = edges & (WIN_EDGE_TOP | WIN_EDGE_BOTTOM);
    if (h && v)                          // top-left and bottom-right drag along \, the others /
        return !(edges & WIN_EDGE_LEFT) == !(edges & WIN_EDGE_TOP) ? WIN_CURSOR_RESIZE_NWSE : WIN_CURSOR_RESIZE_NESW;
    return h ? WIN_CURSOR_RESIZE_H : v ? WIN_CURSOR_RESIZE_V : WIN_CURSOR_DEFAULT;
}

static void on_motion(struct uapp *a, int x, int y, unsigned buttons) {
    to_w16(&x, &y);
    if (g_faulted) return;
    if (bar_open()) {
        unsigned edges;
        int t = w16_frame_hit(&g_w16.win, x, y, &edges) == HIT_MENU ? bar_title(x) : -1;
        if (t >= 0 && t != g_bar_pos) { bar_request(a, t, 0); return; }
        if (w16_menu_motion(&g_bar, x, y)) uapp_redraw(a);
        return;
    }
    if (w16_menu_open_p(&g_menu)) {
        if (w16_menu_motion(&g_menu, x, y)) uapp_redraw(a);
        return;
    }
    // The pointer takes the shape of the window under it: an edit's I-beam,
    // or over the 3.1 frame's border -- which toywm cannot see -- the
    // resize arrow for the way that edge or corner drags. Under a capture
    // it keeps the shape it had when the capture began.
    unsigned edges;
    int shape = WIN_CURSOR_DEFAULT;
    enum w16_hit hit = w16_frame_hit(&g_w16.win, x, y, &edges);
    if (mouse_is_programs(hit)) shape = client_mouse(WM_MOUSEMOVE, mk_flags(w16_mouse_swap(buttons)), x, y);
    else if (hit == HIT_EDGE && !maximized()) shape = edge_cursor(edges);
    if (shape >= 0) show_cursor(a, shape);
}

// The caret's blink: its window drawn again when it turns on or off.
static void caret_tick(struct uapp *a) {
    static int was = -1;
    pthread_mutex_lock(&g_w16.lock);
    struct w16_wnd *x = g_w16.caret.hwnd ? w16_wnd_get(&g_w16, g_w16.caret.hwnd) : 0, *top = x ? w16_wnd_top(&g_w16, x) : 0;
    int ph = top ? w16_caret_phase(&g_w16, top) : -1;
    uint16_t dlg = top && top != &g_w16.win ? top->hwnd : 0;
    pthread_mutex_unlock(&g_w16.lock);
    if (ph == was) return;
    was = ph;
    if (dlg) w16_dlgwin_redraw(&g_w16, dlg);
    else uapp_redraw(a);
}

static void handle_key(struct uapp *a, int key, unsigned mods);

// A held key's repeats on Windows' timing (input16.c), to the window it was pressed in.
static void key_repeat(struct uapp *a) {
    int key;
    unsigned mods;
    uint16_t dlg;
    for (int n = w16_key_due(&key, &mods, &dlg); n > 0; n--) {
        if (!dlg) { handle_key(a, key, mods); continue; }
        pthread_mutex_lock(&g_w16.lock);
        struct w16_wnd *d = w16_wnd_get(&g_w16, dlg);
        pthread_mutex_unlock(&g_w16.lock);
        if (d) w16_route_key(&g_w16, d, key, mods, 0);
    }
}

static int on_tick(struct uapp *a) {
    key_repeat(a);
    w16_timers_fire(&g_w16);
    w16_sound_tick();
    w16_wave_tick(&g_w16);
    caret_tick(a);
    return 0;
}

// A key not for the frame or the menus goes to the program's window with
// the focus (input16.c).
static void handle_key(struct uapp *a, int key, unsigned mods) {
    if (g_faulted) {
        if (w16_msgbox_key(&g_box, key)) end_faulted();
        return;
    }
    if (g_bar.popup > 0) {
        unsigned id = w16_menu_key(&g_bar, key);
        if (id == W16_MENU_LEFT || id == W16_MENU_RIGHT) {
            pthread_mutex_lock(&g_w16.lock);
            int n = w16_menubar_count(&g_w16.win);
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
        if (id && id < W16_MENU_RIGHT) control_chosen(a, id);
        uapp_redraw(a);
        return;
    }
    if (g_bar_hot >= 0) {
        pthread_mutex_lock(&g_w16.lock);
        int n = w16_menubar_count(&g_w16.win);
        int t = key > ' ' && key < 0x7F ? w16_menubar_mnemonic(&g_w16.win, key) : -1;
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
    if (key == '-' && (mods & KEY_MOD_ALT)) {   // Alt+-: the focused child's (an MDI document's), as 3.1 had it
        pthread_mutex_lock(&g_w16.lock);
        uint16_t f = g_w16.focus;
        pthread_mutex_unlock(&g_w16.lock);
        if (f && f != g_w16.win.hwnd) w16_post_msg(&g_w16, f, WM_SYSCOMMAND, SC_KEYMENU, '-');
        return;
    }
    // F10 lights the bar's first title; Alt and a title's letter drops that menu.
    if (g_w16.win.menubar && key == KEY_F10) {
        pthread_mutex_lock(&g_w16.lock);
        g_bar_hot = w16_menubar_count(&g_w16.win) ? 0 : -1;
        pthread_mutex_unlock(&g_w16.lock);
        uapp_redraw(a);
        return;
    }
    if (g_w16.win.menubar && (mods & KEY_MOD_ALT) && key > ' ' && key < 0x7F) {
        pthread_mutex_lock(&g_w16.lock);
        int t = w16_menubar_mnemonic(&g_w16.win, key);
        pthread_mutex_unlock(&g_w16.lock);
        if (t >= 0) { bar_request(a, t, 1); return; }
    }
    w16_route_key(&g_w16, &g_w16.win, key, mods, 0);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    if (w16_key_press(key, mods, 0)) handle_key(a, key, mods);
}

static void on_wheel(struct uapp *a, int notches) {
    (void)a;
    if (!g_faulted && !w16_menu_open_p(&g_menu) && !bar_open()) w16_route_wheel(&g_w16, &g_w16.win, notches);
}

static void on_phys_key(struct uapp *a, int code, int down, unsigned mods) {
    (void)a; (void)mods;
    w16_key_phys(code, down);
}

static void on_key_up(struct uapp *a, int key, unsigned mods) {
    (void)a; (void)mods;
    if (g_faulted || w16_menu_open_p(&g_menu) || bar_open() || g_bar_hot >= 0) return;
    w16_route_key(&g_w16, &g_w16.win, key, mods, 1);
}

static void on_focus(struct uapp *a, int focused) {
    if (!focused) g_bar_hot = -1;
    if (!focused) w16_key_release_all();         // its releases go to the window that has it now
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

static struct uapp_desc g_desc;         // run()'s; open_top() fills in the main window

// The loop is up with no window yet, and the PROGRAM STARTS NOW: a post
// made before g_w16.app was set went to no one, and a quick program's
// main window, made before the loop, never opened.
static int g_task_started;

static void on_start(struct uapp *a) {
    pthread_mutex_lock(&g_w16.lock);
    g_w16.app = a;
    pthread_mutex_unlock(&g_w16.lock);
    static pthread_t task;
    if (pthread_create(&task, NULL, w16_task_main, &g_w16) != 0) {
        fprintf(stderr, "w16: cannot start the program's thread\n");
        uapp_quit(a, 1);
        return;
    }
    g_task_started = 1;
}

// The program made its main window: the toplevel, as it is now -- its title, its size scaled, a
// thick frame resizable -- under the program's own identity.
static void open_top(struct uapp *a) {
    // EACH PROGRAM ITS OWN IDENTITY: toywm remembers a window's size and
    // groups the taskbar by app_id, and one shared "w16" gave every
    // program the last one's size. A scaled window is another identity:
    // the size remembered at 1x would undo scale=2.
    static char appid[40], *title;
    if (g_w16_q != 4) snprintf(appid, sizeof appid, "w16-%s-%gx", g_w16.ne.name, g_w16_q / 4.0);
    else snprintf(appid, sizeof appid, "w16-%s", g_w16.ne.name);
    for (char *p = appid; *p; p++)
        *p = *p >= 'A' && *p <= 'Z' ? (char)(*p + 32) : (*p == ' ' || *p == '/') ? '-' : *p;
    // A COPY, taken under the lock: the program runs on, and a SetWindowText
    // frees win.text -- the pointer itself once named whatever reused it.
    pthread_mutex_lock(&g_w16.lock);
    free(title);
    title = strdup(g_w16.win.text ? g_w16.win.text : "");
    g_desc.flags = UAPP_NO_TOPLEVEL | (g_w16_desktop_frame ? 0 : UAPP_NO_DECOR) |
                   (g_w16.win.style & WS_THICKFRAME ? UAPP_RESIZABLE : 0);
    g_desc.w = w16_up(g_w16.win.w);      // sized in REAL pixels: the window's 1x size times the scale
    g_desc.h = w16_up(g_w16.win.h);
    pthread_mutex_unlock(&g_w16.lock);
    g_desc.title = title ? title : "";
    g_desc.app_id = appid;
    uapp_open_toplevel(a);
}

static int on_user(struct uapp *a, int code, int arg) {
    if (code == W16_POST_TOP_OPEN) { open_top(a); return 1; }
    // BEFORE THE MAIN WINDOW only the dialogs, a title, a repaint and the end are for now; a
    // window command is kept for on_open, and a fault has no window to show its box in.
    int now = code == W16_POST_DONE || code == W16_POST_PAINT || code == W16_POST_TITLE ||
              (code >= W16_POST_DLG_OPEN && code <= W16_POST_DLG_PAINT) || code == W16_POST_DLG_SIZE ||
              code == W16_POST_DLG_HIDE || code == W16_POST_LAUNCH;
    if (!uapp_has_toplevel(a) && !now) {
        pthread_mutex_lock(&g_w16.lock);
        if (code == W16_POST_SYSCMD) g_w16.pending_syscmd = arg;
        if (code == W16_POST_ABOVE) g_w16.pending_above = arg + 1;
        pthread_mutex_unlock(&g_w16.lock);
        if (code == W16_POST_FAULT) end_faulted();
        if (code == W16_POST_POINTER) {          // no window to warp in: the move is dropped, and the next posts again
            int x, y;
            w16_pointer_take(&g_w16, &x, &y);
        }
        return 0;
    }
    switch (code) {
    case W16_POST_DONE:
        // RECORDED WHILE THE WINDOW STILL EXISTS: the launcher re-reads
        // this when its own window gets focus back, which is the moment
        // this one goes -- written after, it would read the run before.
        record_last("ended", 0);
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
    case W16_POST_MENU_SHOW: {                  // arg: the bar's window << 16 | the title
        uint16_t h = (uint16_t)((uint32_t)arg >> 16);
        int pos = (int16_t)(arg & 0xFFFF);
        if (h == g_w16.win.hwnd) bar_show(pos);
        else w16_dlgwin_bar_show(&g_w16, h, pos);
        return 1;
    }
    case W16_POST_CURSOR: show_cursor(a, arg); return 0;
    case W16_POST_LAUNCH: {             // WinExec: the compositor starts the second w16 (shell16.c)
        char exe[512], args[256];
        if (!w16_launch_take(exe, sizeof exe, args, sizeof args)) return 0;
        char *argv[] = { (char *)"/bin/w16", (char *)"--", exe, args[0] ? args : 0, 0 };
        w16_launch_done(uapp_launch(a, argv));
        return 0;
    }
    case W16_POST_CHILD_MENU: child_menu(a, (uint16_t)arg, 1); return 1;
    case W16_POST_CHILD_BOX:  child_menu(a, (uint16_t)arg, 0); return 1;
    case W16_POST_MOVE:                 // a press the program called its caption: the compositor drags
        if (!maximized()) uapp_move_start(a, 0);
        return 0;
    case W16_POST_ABOVE: uapp_set_above(a, arg); return 0;
    case W16_POST_POINTER:              // w16's screen is the window: its 1x point, scaled
    {
        int x, y;
        if (!w16_pointer_take(&g_w16, &x, &y)) return 0;
        x = w16_up(x);
        y = w16_up(y);
        warp_to_glass(&x, &y);
        uapp_set_pointer(a, x, y);
        return 0;
    }
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
    case W16_POST_DLG_HIDE:  w16_dlgwin_hide(&g_w16, (uint16_t)arg); return 1;
    case W16_POST_DLG_SIZE:  w16_dlgwin_resize(&g_w16, (uint16_t)arg); return 1;
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
    // The Volume panel's row: the program's own short name ("Hearts"), not w16.
    char about[128];
    if (ne_description(&w->ne, about, sizeof about) < 0 || !about[0]) snprintf(about, sizeof about, "%s", w->ne.name);
    char *short_name = w16_short_title(about);
    if (short_name) { w16_sound_set_name(short_name); free(short_name); }
    ugfx_font_init();
    // THE LOOP STARTS FIRST, the program in on_start(), before it has a window: a start-up
    // message box ("free memory", "no CD") comes before the main window,
    // and opens unowned (UAPP_NO_TOPLEVEL). W16_POST_TOP_OPEN opens the
    // toplevel when the program makes its main window (open_top()).
    g_desc = (struct uapp_desc){
        .flags = UAPP_NO_TOPLEVEL,
        .min_w = w16_up(120),
        .min_h = w16_up(2 * W16_FRAME + W16_CAPTION + 1),
        .on_start = on_start,
        .on_open = on_open,
        .on_draw = on_draw,
        .on_press = on_press,
        .on_release = on_release,
        .on_motion = on_motion,
        .on_key = on_key,
        .on_key_up = on_key_up,
        .on_phys_key = on_phys_key,
        .on_wheel = on_wheel,
        .on_focus = on_focus,
        .on_resize = on_resize,
        .on_close = on_close,
        .on_user = on_user,
        .on_tick = on_tick,
        .tick_ms = 50,                   // timers: 3.1's ran at 55 ms
    };
    int r = uapp_run(&g_desc);
    if (r && !g_task_started) fprintf(stderr, "w16: no window server to show %s in\n", path);
    return r ? r : w->exit_code;
}

// `--check`: what the launcher shows of a program, as lines -- its module
// and description, how many of the Windows functions it imports w16 has
// and which it lacks, and its icon as rows of AARRGGBB.
static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

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
    // Imports by name that w16 cannot number -- a program's own DLL, a name its tables lack -- as
    // "MODULE name", each once: they count as imports and w16 has none, so they are LISTED, or the
    // launcher says one call is missing and the calls window finds none.
    char **named = 0;
    unsigned nnamed = 0, ncap = 0;
    for (unsigned s = 1; s <= m.nseg; s++)
        for (unsigned i = 0; i < m.seg[s - 1].nrelocs; i++) {
            struct ne_reloc r;
            if (ne_reloc_at(&m, s, i, &r) < 0) continue;
            unsigned ord = r.b;
            if ((r.flags & 3) == NE_REL_IMPORT_NAME) {
                // By name, as the loader takes it: a Windows module's names
                // are known, so it is listed under its ordinal like the rest.
                char fn[64];
                ord = r.a >= 1 && r.a <= m.nmod && w16_is_builtin(mods[r.a - 1])
                      && ne_imported_name(&m, r.b, fn, sizeof fn) == 0 ? w16_builtin_ordinal(mods[r.a - 1], fn) : 0;
                if (!ord) {
                    char nm[64], key[96];
                    snprintf(key, sizeof key, "%s %s", r.a >= 1 && r.a <= m.nmod ? mods[r.a - 1] : "?",
                             ne_imported_name(&m, r.b, nm, sizeof nm) == 0 ? nm : "?");
                    if (nnamed == ncap) {
                        char **g = realloc(named, sizeof *named * (ncap = ncap ? ncap * 2 : 16));
                        if (!g) { printf("error out of memory\n"); return 1; }
                        named = g;
                    }
                    if (!(named[nnamed] = strdup(key))) { printf("error out of memory\n"); return 1; }
                    nnamed++;
                    continue;
                }
            } else if ((r.flags & 3) != NE_REL_IMPORT_ORD) continue;
            if (r.a < 1 || r.a > m.nmod) continue;
            if (nseen == cap) {
                uint32_t *g = realloc(seen, sizeof *seen * (cap = cap ? cap * 2 : 256));
                if (!g) { printf("error out of memory\n"); return 1; }
                seen = g;
            }
            seen[nseen++] = (uint32_t)r.a << 16 | ord;
        }
    qsort(named, nnamed, sizeof *named, cmp_str);
    for (unsigned k = 0; k < nnamed; k++) {
        if (by_name && !strcmp(named[by_name - 1], named[k])) free(named[k]);
        else named[by_name++] = named[k];
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
    for (unsigned k = 0; k < by_name; k++) {
        char *sp = strchr(named[k], ' ');
        *sp = 0;
        printf("call missing %s.0 %s %s\n", named[k], sp + 1, w16_kind_names[w16_kind_other]);
        free(named[k]);
    }
    free(named);
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

// `--sounds`: a line per event drive C:'s WIN.INI lists, else Windows
// 3.11's own seven -- its WIN.INI name, what 3.1 called it, the file
// [sounds] names, the file that plays, found/missing/none, and SaunaOS's
// own sound for it. Resolved as `sounds=win311` whatever the setting
// says, so the launcher's Options can show each choice before OK.
static void sounds_list(void) {
    static const struct { const char *alias, *name; } STD[] = {
        { "SystemDefault", "Default Beep" }, { "SystemAsterisk", "Asterisk" },
        { "SystemHand", "Critical Stop" }, { "SystemExclamation", "Exclamation" },
        { "SystemQuestion", "Question" }, { "SystemStart", "Windows Start" }, { "SystemExit", "Windows Exit" },
    };
    static const char *const STATUS[] = { "found", "missing", "none", "off" };
    g_w16_prefs.sounds = W16_SOUNDS_WIN311;
    static char names[4096], ini[160], host[600], saunaos[600];   // one-shot: off the stack
    size_t n = w16_ini_names("WIN.INI", "sounds", names, sizeof names);
    const char *list[64];
    int k = 0;
    for (const char *p = names; n && *p && k < 64; p += strlen(p) + 1) list[k++] = p;
    if (!k)
        for (unsigned i = 0; i < sizeof STD / sizeof STD[0]; i++) list[k++] = STD[i].alias;
    for (int i = 0; i < k; i++) {
        int st = w16_sound_resolve(list[i], ini, sizeof ini, host, sizeof host);
        g_w16_prefs.sounds = W16_SOUNDS_SAUNAOS;
        w16_sound_resolve(list[i], saunaos, sizeof saunaos, saunaos, sizeof saunaos);
        g_w16_prefs.sounds = W16_SOUNDS_WIN311;
        const char *name = list[i], *comma = strchr(ini, ',');
        for (unsigned j = 0; j < sizeof STD / sizeof STD[0]; j++)
            if (!strcasecmp(STD[j].alias, list[i])) name = STD[j].name;
        char desc[96];
        snprintf(desc, sizeof desc, "%s", comma ? comma + 1 : name);
        char *d = desc;
        while (*d == ' ') d++;
        if (comma) *(char *)comma = 0;
        printf("sound\t%s\t%s\t%s\t%s\t%s\t%s\n", list[i], d, ini, host, STATUS[st], saunaos);
    }
}

// `--why CALL`: what CALL is (w16_call_doc()) -- what it does, how w16
// answers it, and for a stand-in why only partly, or for a missing call
// why it is not there -- rendered for a terminal as `doc` renders a page.
static int why(const char *call) {
    if (unveil(W16_CALLS_DOC, "r") != 0 || unveil(W16_MISSING_DOC, "r") != 0 || unveil(NULL, NULL) != 0 ||
        pledge("stdio rpath", NULL) != 0) {
        fprintf(stderr, "w16: cannot sandbox myself, so I will not read the calls pages\n");
        return 1;
    }
    char module[16] = "";
    const char *dot = strchr(call, '.');
    if (!dot || dot == call || (size_t)(dot - call) >= sizeof module || !dot[1]) {
        fprintf(stderr, "w16: name a call as MODULE.ORDINAL, as `w16 --check FILE` lists them (USER.161)\n");
        return 1;
    }
    snprintf(module, sizeof module, "%.*s", (int)(dot - call), call);
    const struct w16_name *nm = w16_name_lookup(module, (unsigned)strtoul(dot + 1, 0, 10));
    int n = 0, from = 0;
    char *doc = w16_call_doc(call, nm ? nm->name : 0, nm ? w16_kind_names[nm->kind] : 0, &n, &from);
    if (!doc) return 1;
    struct umd_opts mo = { .cols = 80, .color = isatty(1), .indent = 3 };
    int cap = n * 4 + 8192;                      // indent and colour codes on every line
    struct umd_out o = { malloc((size_t)cap), cap, 0, 0 };
    if (o.buf) {
        umd_render(doc, n, &mo, &o);
        fwrite(o.buf, 1, (size_t)o.len, stdout);
    }
    free(o.buf);
    free(doc);
    return o.buf ? 0 : 1;
}

// The write end of the pipe to hlpbroker, -1 when it could not be started: WinHelp() says so then.
int g_w16_help_fd = -1;

// DISC IMAGES BECOME THE FOLDERS THEY ARE MOUNTED AT, before the sandbox
// closes (automountd mounts them, over its channel). The program's own CD
// first, over the drive every program has at its letter; a drive that is
// an image is a CD-ROM to MSCDEX, labelled from the disc. An image that
// will not mount leaves no drive, and says why.
static void mount_discs(void) {
    struct w16_prefs *p = &g_w16_prefs;
    if (p->cd[0]) {
        int i = (p->cd_letter[0] & ~0x20) - 'D';
        if (i < 0 || i >= W16_DRIVES) i = 0;
        snprintf(p->drive[i], sizeof p->drive[i], "%s", p->cd);
        p->label[i][0] = 0;
        if (!w16_prefs_is_cd(p, (char)('D' + i))) {
            size_t n = strlen(p->cd_drives);
            if (n < W16_DRIVES) { p->cd_drives[n] = (char)('D' + i); p->cd_drives[n + 1] = 0; }
        }
    }
    for (int i = 0; i < W16_DRIVES; i++) {
        struct stat st;
        if (!p->drive[i][0] || stat(p->drive[i], &st) != 0 || S_ISDIR(st.st_mode)) continue;
        if (uiso_is_image(p->drive[i]) != 1) continue;
        char point[64], label[64];
        if (udrive_mount_image_wait(p->drive[i], point, sizeof point, 20000) < 0) {
            fprintf(stderr, "w16: drive %c: -- the disc image %s did not mount: %s\n",
                    'D' + i, p->drive[i], strerror(errno));
            p->drive[i][0] = 0;
            continue;
        }
        if (!p->label[i][0] && uiso_label(p->drive[i], label, sizeof label) == 0) {
            int k = 0;
            for (const char *c = label; *c && k < W16_LABEL_MAX - 1; c++)
                if (w16_label_char(*c)) p->label[i][k++] = (char)w16_label_char(*c);
            p->label[i][k] = 0;
        }
        snprintf(p->drive[i], sizeof p->drive[i], "%s", point);
        if (!w16_prefs_is_cd(p, (char)('D' + i))) {
            size_t n = strlen(p->cd_drives);
            if (n < W16_DRIVES) { p->cd_drives[n] = (char)('D' + i); p->cd_drives[n + 1] = 0; }
        }
    }
}

static void start_help_broker(void) {
    int fds[2];
    if (sys_pipe(fds) != 0) return;
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    char *argv[] = { (char *)"/bin/hlpbroker", NULL };
    o.argv = argv;
    o.stdin_fd = fds[0];
    int pid = sys_spawn_opts("/bin/hlpbroker", &o);
    close(fds[0]);
    if (pid <= 0) { close(fds[1]); return; }
    g_w16_help_fd = fds[1];
    // Whose settings it may change: told now, while this process is still trusted.
    char line[W16_PATH_MAX + 16];
    int n = snprintf(line, sizeof line, "PROGRAM\t%s\n", g_exe);
    if (n >= (int)sizeof line || write(g_w16_help_fd, line, (size_t)n) < 0) {
        close(g_w16_help_fd);
        g_w16_help_fd = -1;
    }
}

// The C:\ path a person reads: C:\WINDOWS\SYSTEM\MAIN.CPL.
static void install_path(const struct uwinsetup *s, const struct uws_item *it, char *out, size_t cap) {
    snprintf(out, cap, "C:\\%s\\%s%s", uwinsetup_windir(s), it->dest == UWS_SYSTEM ? "SYSTEM\\" : "", it->name);
}

// One line a file, flushed: Retro Launcher's install dialog reads them as they come.
static int install_progress(void *ctx, int i, int n, const struct uws_item *it, const char *what) {
    char path[64];
    install_path(ctx, it, path, sizeof path);
    printf("%s %d/%d %s %s %d\n", what, i + 1, n, path, it->disk_name[0] ? it->disk_name : "-", it->disk);
    fflush(stdout);
    return 0;
}

// --install DISKS: the disks are read, never changed; only drive C: is written.
static int install(const char *src) {
    w16_prefs_load(&g_w16_prefs, 0);
    snprintf(g_w16_drive_c, sizeof g_w16_drive_c, "%s", g_w16_prefs.drive_c);
    mkdirs(g_w16_drive_c);
    if (unveil("/", "r") != 0 || unveil(g_w16_drive_c, "rwc") != 0 || unveil(NULL, NULL) != 0 ||
        pledge(g_dry_run ? "stdio rpath" : "stdio rpath wpath cpath", NULL) != 0) {
        fprintf(stderr, "w16: cannot sandbox myself, so I will not read the disks\n");
        return 1;
    }
    char err[256], path[64];
    struct uwinsetup *s = uwinsetup_open(src, err, sizeof err);
    if (!s) {
        fprintf(stderr, "w16: %s\n", err);
        return 1;
    }
    int n = uwinsetup_count(s), missing = uwinsetup_missing(s), files = 0;
    for (int i = 0; i < n; i++) {
        const struct uws_item *it = uwinsetup_item(s, i);
        files += it->disk_name[0] || it->kind != UWS_OPTIONAL;   // Setup's own, when these disks lack it, is no file
    }
    printf("setup %s\nlanguage %s\ndisks %d\nfiles %d\nbytes %llu\nmissing %d\ntarget C:\\%s %s/%s\n",
           uwinsetup_title(s), uwinsetup_lang(s), uwinsetup_containers(s), files,
           (unsigned long long)uwinsetup_bytes(s), missing, uwinsetup_windir(s), g_w16_drive_c, uwinsetup_windir(s));
    for (int i = 0; i < n && (g_dry_run || missing); i++) {
        const struct uws_item *it = uwinsetup_item(s, i);
        install_path(s, it, path, sizeof path);
        if (it->disk_name[0] && g_dry_run)
            printf("file %s %s disk %d\n", path, it->disk_name, it->disk);
        else if (!it->disk_name[0] && it->kind != UWS_OPTIONAL)
            printf("missing %s disk %d\n", path, it->disk);
    }
    int rc = 0;
    if (missing && !g_dry_run) {
        fprintf(stderr, "w16: %d file%s SETUP.INF names %s on none of these disks\n", missing,
                missing == 1 ? "" : "s", missing == 1 ? "is" : "are");
        rc = 1;
    } else if (!g_dry_run) {
        int r = uwinsetup_run(s, g_w16_drive_c, g_replace_ini ? 0 : UWS_KEEP_INI, install_progress, s,
                              err, sizeof err);
        if (r) {
            fprintf(stderr, "w16: %s\n", err);
            rc = 1;
        } else {
            // Program Manager's groups, as Setup made them through its DDE, then the launcher's folders'.
            w16_grp_setup(s, stdout);
            w16_grp_folders(stdout);
            printf("done %d files\n", files);
        }
    }
    fflush(stdout);
    uwinsetup_close(s);
    return rc;
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
    if (g_why) {
        if (a.argc) return uargs_error(&PROG, "--why takes no FILE");
        return why(g_why);
    }
    if (g_install) {
        if (a.argc) return uargs_error(&PROG, "--install takes its DISKS and no FILE");
        return install(g_install);
    }
    if (g_groups) {                              // drive C:'s Program Manager groups, written in the sandbox
        if (a.argc) return uargs_error(&PROG, "--groups takes no FILE");
        w16_prefs_load(&g_w16_prefs, 0);
        snprintf(g_w16_drive_c, sizeof g_w16_drive_c, "%s", g_w16_prefs.drive_c);
        if (unveil("/", "r") != 0 || unveil(g_w16_drive_c, "rwc") != 0 || unveil(NULL, NULL) != 0 ||
            pledge("stdio rpath wpath cpath", NULL) != 0) {
            fprintf(stderr, "w16: cannot sandbox myself, so I will not write the groups\n");
            return 1;
        }
        return w16_grp_folders(stdout) ? 1 : 0;
    }
    if (g_dry_run || g_replace_ini) return uargs_error(&PROG, "--dry-run and --replace-ini go with --install");
    if (g_sounds) {                              // WIN.INI's [sounds] on drive C:, read in the sandbox
        if (a.argc) return uargs_error(&PROG, "--sounds takes no FILE");
        w16_prefs_load(&g_w16_prefs, 0);
        snprintf(g_w16_drive_c, sizeof g_w16_drive_c, "%s", g_w16_prefs.drive_c);
        if (unveil(g_w16_drive_c, "r") != 0 || unveil("/usr/share/sounds", "r") != 0 || unveil(NULL, NULL) != 0 ||
            pledge("stdio rpath", NULL) != 0) {
            fprintf(stderr, "w16: cannot sandbox myself, so I will not read WIN.INI\n");
            return 1;
        }
        sounds_list();
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
        if (g_w16_prefs.missing == W16_MISSING_ZERO) g_w16_skip_missing = 1;
        snprintf(g_w16_drive_c, sizeof g_w16_drive_c, "%s", g_w16_prefs.drive_c);
        mkdirs(g_w16_drive_c);
        mkdirs(W16_STATE_DIR);
        mount_discs();
        static char trace[W16_PATH_MAX + 32];
        if (!g_trace_path && g_w16_prefs.trace) {
            w16_trace_path(g_exe, trace, sizeof trace);
            g_trace_path = trace;
        }
        // Help Viewer's opener, started BEFORE the sandbox closes: this process may not spawn,
        // and a program's F1 has to open a window (userland/bin/hlpbroker.c).
        start_help_broker();
        // The sound daemon's beacon, and this process's own ring to it
        // (lib/usnd_sink_daemon.c names it by pid) -- never the device.
        char snd_ring[48];
        snprintf(snd_ring, sizeof snd_ring, "shm:" SND_CLIENT_PREFIX "%d", (int)getpid());
        // Drive Z: is the whole tree to read, and /home to write as well
        // (file16.c) -- what a program can reach of SaunaOS's files, Wine's
        // shape with the system's own folders kept read-only.
        ok = unveil("/", "r") == 0 && unveil("/home", "rwc") == 0
             && unveil(g_w16_drive_c, "rwc") == 0 && unveil(W16_STATE_DIR, "rwc") == 0
             && unveil("shm:" CLIP_SHM_NAME, "rw") == 0
             && unveil("shm:toywm", "r") == 0 && unveil("shm:" FONT_BEACON_NAME, "r") == 0
             && unveil("shm:font.0", "r") == 0 && unveil("shm:font.1", "r") == 0
             && unveil("shm:font.2", "r") == 0 && unveil("shm:font.3", "r") == 0
             && (!g_trace_path || unveil(g_trace_path, "rwc") == 0)
             && unveil("shm:" SND_SERVER_NAME, "r") == 0 && unveil(snd_ring, "rwc") == 0
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
    if (g_w16_trace) {                           // the drives first: a "no such drive" is then readable
        for (char d = 'C'; d <= 'Y'; d++) {
            char label[W16_LABEL_MAX];
            if (!w16_drive_root(d)) continue;
            if (!w16_drive_label(&g_w16_prefs, d, w16_drive_root(d), label)) label[0] = 0;
            fprintf(g_w16_trace, "drive %c: %s%s, labelled %s\n", d, w16_drive_root(d),
                    w16_drive_is_cd(d) ? " (CD-ROM)" : "", label[0] ? label : "nothing");
        }
    }
    uint32_t size = 0;
    uint8_t *img = slurp(path, &size);
    if (!img) {
        fprintf(stderr, "w16: %s: %s\n", path,
                size == UINT32_MAX ? "larger than any NE program" : strerror(errno));
        return 1;
    }
    // What follows the program is its command line -- a document to open,
    // most often -- and a SaunaOS path in it becomes the DOS one the
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
