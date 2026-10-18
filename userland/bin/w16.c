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

static const struct uargs_opt OPTS[] = {
    { "info", 'i', NULL, "print the program's segments and the functions it imports", &g_info, NULL },
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
    .notes = "Exit status: the program's own, 1 the file was refused or the program\n"
             "faulted, 2 a usage error.",
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

static uint32_t client_lparam(int x, int y) {
    int cx, cy, cw, ch;
    w16_client_rect(&g_w16.win, &cx, &cy, &cw, &ch);
    return (uint32_t)(uint16_t)(y - cy) << 16 | (uint16_t)(x - cx);
}

static void on_open(struct uapp *a) {
    pthread_mutex_lock(&g_w16.lock);
    g_w16.app = a;
    pthread_mutex_unlock(&g_w16.lock);
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    struct ugfx_surface *s = uapp_surface(d);
    struct w16_wnd *win = &g_w16.win;
    pthread_mutex_lock(&g_w16.lock);
    w16_frame_draw(s, win, uapp_focused(a), g_pressed);
    int cx, cy, cw, ch;
    w16_client_rect(win, &cx, &cy, &cw, &ch);
    if (win->fb) ugfx_blit(s, cx, cy, win->cw, win->ch, win->fb, win->cw);
    pthread_mutex_unlock(&g_w16.lock);
}

static void on_press(struct uapp *a, int x, int y, unsigned buttons) {
    unsigned edges;
    enum w16_hit hit = w16_frame_hit(&g_w16.win, x, y, &edges);
    if (hit == HIT_CLIENT) {
        int right = !(buttons & 1);
        w16_post_msg(&g_w16, g_w16.win.hwnd, right ? WM_RBUTTONDOWN : WM_LBUTTONDOWN,
                     right ? MK_RBUTTON : MK_LBUTTON, client_lparam(x, y));
        return;
    }
    if (!(buttons & 1)) return;
    switch (hit) {
    case HIT_CAPTION: uapp_move_start(a, 0); break;
    case HIT_EDGE: uapp_move_start(a, edges); break;
    case HIT_MIN: case HIT_MAX:
        g_pressed = hit;
        uapp_redraw(a);
        break;
    case HIT_SYSMENU: {
        // A double click on the control-menu box closes, as on 3.1; the
        // menu a single click opens is not built yet.
        uint64_t now = sys_ticks();
        if (now - g_sysmenu_tick <= udclick_ticks()) {
            w16_post_msg(&g_w16, g_w16.win.hwnd, WM_CLOSE, 0, 0);
            g_sysmenu_tick = 0;
        } else {
            g_sysmenu_tick = now;
        }
        break;
    }
    default: break;
    }
}

static void on_release(struct uapp *a, int x, int y, unsigned buttons) {
    if (g_pressed) {                     // minimize and maximize: not built yet
        g_pressed = 0;
        uapp_redraw(a);
        return;
    }
    unsigned edges;
    if (w16_frame_hit(&g_w16.win, x, y, &edges) == HIT_CLIENT)
        w16_post_msg(&g_w16, g_w16.win.hwnd, buttons & 2 ? WM_RBUTTONUP : WM_LBUTTONUP, 0,
                     client_lparam(x, y));
}

static void on_motion(struct uapp *a, int x, int y, unsigned buttons) {
    (void)a;
    unsigned edges;
    if (w16_frame_hit(&g_w16.win, x, y, &edges) == HIT_CLIENT)
        w16_post_msg(&g_w16, g_w16.win.hwnd, WM_MOUSEMOVE,
                     (buttons & 1 ? MK_LBUTTON : 0) | (buttons & 2 ? MK_RBUTTON : 0),
                     client_lparam(x, y));
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
    (void)a; (void)mods;
    int vk = vk_of(key);
    if (vk) w16_post_msg(&g_w16, g_w16.win.hwnd, WM_KEYDOWN, (uint16_t)vk, 1);
    int ch = key == '\n' ? '\r' : key;
    if ((ch >= 0x20 && ch < 0x7F) || ch == '\r' || ch == '\b' || ch == '\t' || ch == 0x1B)
        w16_post_msg(&g_w16, g_w16.win.hwnd, WM_CHAR, (uint16_t)ch, 1);
}

static void on_key_up(struct uapp *a, int key, unsigned mods) {
    (void)a; (void)mods;
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
    if (fb) w16_post_msg(&g_w16, win->hwnd, WM_SIZE, 0, (uint32_t)ch << 16 | (uint16_t)cw);
    uapp_redraw(a);
}

static int on_close(struct uapp *a) {
    (void)a;
    w16_post_msg(&g_w16, g_w16.win.hwnd, WM_CLOSE, 0, 0);
    return 0;                            // the program decides, and ends itself
}

static int on_user(struct uapp *a, int code, int arg) {
    (void)arg;
    if (code == W16_POST_DONE) {
        uapp_quit(a, g_w16.exit_code);
        return 0;
    }
    return 1;                            // W16_POST_PAINT: copy the new pixels
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
        return w->exit_code;
    }
    struct uapp_desc desc = {
        .title = w->win.title,
        .app_id = "w16",
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
    ok = ok && unveil(NULL, NULL) == 0
            && pledge(g_info ? "stdio rpath" : "stdio rpath shm window settings query", NULL) == 0;
    if (!ok) {
        fprintf(stderr, "w16: cannot sandbox myself, so I will not read %s\n", path);
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
