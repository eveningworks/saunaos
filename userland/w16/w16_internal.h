// w16_internal.h -- what w16's parts share (docs/w16-design.md). Split by
// concern, as userland/wm/ is:
//   ne.c, ne_info.c   read the NE file; `--info`
//   cpu16.c           the 16-bit CPU
//   task16.c          load the program, its thread, the message queue,
//                     and the dispatch of its imports
//   kernel16.c        KERNEL (and INT 21h), user16.c USER, gdi16.c GDI
//   frame16.c         the Windows 3.1 caption and frame (main thread)
//
// TWO THREADS. The program -- interpreter, message loop, GDI -- runs on
// the TASK thread, so a modal loop inside a window procedure simply
// blocks there. The uapp MAIN thread owns the toywm window: it draws the
// frame, copies the client pixels, and turns input into messages. They
// meet in `struct w16` under `lock`: the queue, the window's geometry and
// its client buffer. The task tells the main thread to repaint with
// uapp_post(), the one toolkit call a worker may make.
#ifndef W16_INTERNAL_H
#define W16_INTERNAL_H

#include <pthread.h>
#include <stdint.h>

#include "cpu16.h"
#include "ne.h"
#include "w16_names.h"

struct uapp;

void ne_print_info(const struct ne_module *m);

// --- Win16 constants this layer uses -------------------------------------

#define WM_CREATE        0x0001
#define WM_DESTROY       0x0002
#define WM_SIZE          0x0005
#define WM_ACTIVATE      0x0006
#define WM_PAINT         0x000F
#define WM_CLOSE         0x0010
#define WM_QUIT          0x0012
#define WM_ERASEBKGND    0x0014
#define WM_SHOWWINDOW    0x0018
#define WM_KEYDOWN       0x0100
#define WM_KEYUP         0x0101
#define WM_CHAR          0x0102
#define WM_MOUSEMOVE     0x0200
#define WM_LBUTTONDOWN   0x0201
#define WM_LBUTTONUP     0x0202
#define WM_RBUTTONDOWN   0x0204
#define WM_RBUTTONUP     0x0205

#define WS_CAPTION       0x00C00000u
#define WS_SYSMENU       0x00080000u
#define WS_THICKFRAME    0x00040000u
#define WS_MINIMIZEBOX   0x00020000u
#define WS_MAXIMIZEBOX   0x00010000u

#define W16_CW_USEDEFAULT ((int16_t)0x8000)

// --- the shared state ------------------------------------------------------

struct w16_msg {
    uint16_t hwnd, message, wparam;
    uint32_t lparam;
};

#define W16_QUEUE    256
#define W16_CLASSES  32

struct w16_class {
    char name[64];
    uint16_t proc_sel, proc_off;
    uint16_t background;          // an HBRUSH, or COLOR_* + 1
    uint16_t style;
};

// The program's one top-level window (stage 2: one per program). Its
// RECT is the toywm window: frame16.c draws the non-client part around
// the client buffer.
struct w16_wnd {
    int used;
    uint16_t hwnd;
    int cls;
    char title[128];
    uint32_t style;
    int w, h;                     // the whole window, frame included
    int cw, ch;                   // the client area
    uint32_t *fb;                 // client pixels, cw * ch, under `lock`
    int visible;
    int inval, erase;             // a pending WM_PAINT, and whether it erases
};

struct w16 {
    const char *path;
    struct ne_module ne;
    struct cpu16 cpu;
    struct cpu16_desc *desc;
    uint16_t seg_sel[NE_MAX_SEGMENTS + 1];       // NE segment n -> its selector
    uint16_t thunk_sel[NE_MAX_MODULES + 1];      // module reference -> its thunk selector
    char modname[NE_MAX_MODULES + 1][16];
    uint16_t dgroup, psp, sysdata;               // sysdata: what w16 hands the program
    uint8_t *sysmem;

    pthread_mutex_t lock;
    pthread_cond_t cond;                         // the task waits for messages on this
    struct w16_msg q[W16_QUEUE];
    unsigned qhead, qtail;
    int quit_posted;
    uint16_t quit_code;

    struct w16_class cls[W16_CLASSES];
    int ncls;
    struct w16_wnd win;

    struct uapp *app;                            // the main thread's, once open
    int window_ready;                            // the first window exists (main waits)
    int task_done;
    int exit_code;
    int active;                                  // the toywm window has focus

    uint32_t text_color, bk_color;               // GDI: one DC's worth for now
    int bk_mode;
};

extern struct w16 g_w16;
extern pthread_mutex_t g_w16_text_lock;          // ugfx's text is not thread-safe

// uapp_post() codes, task -> main.
#define W16_POST_PAINT 1
#define W16_POST_DONE  2
#define W16_POST_TITLE 3

// --- task16.c --------------------------------------------------------------

int w16_load(struct w16 *w, const uint8_t *img, uint32_t size, const char *path);
void *w16_task_main(void *arg);
void w16_post_msg(struct w16 *w, uint16_t hwnd, uint16_t msg, uint16_t wparam, uint32_t lparam);
uint32_t w16_call_wndproc(struct w16 *w, uint16_t sel, uint16_t off, uint16_t hwnd,
                          uint16_t msg, uint16_t wparam, uint32_t lparam);
void w16_task_post(struct w16 *w, int code);     // uapp_post() once the main thread is up

// Pascal arguments, read FIRST TO LAST: the first was pushed first, so it
// sits highest, just under the byte count the function pops.
struct w16_args {
    struct cpu16 *c;
    unsigned pos;
};
uint16_t w16_arg16(struct w16_args *a);
uint32_t w16_arg32(struct w16_args *a);

// Each module's dispatcher: the bytes to pop, or -1 after naming the
// failure (cpu16_set_fault). `argbytes` is the generated table's.
int w16_kernel(struct w16 *w, unsigned ordinal, struct w16_args *a);
int w16_user(struct w16 *w, unsigned ordinal, struct w16_args *a);
int w16_gdi(struct w16 *w, unsigned ordinal, struct w16_args *a);
int w16_int(struct cpu16 *c, uint8_t n);

// A sysdata scratch area: `len` bytes w16 fills and hands the program.
uint16_t w16_sys_alloc(struct w16 *w, unsigned len);

// --- user16.c / gdi16.c ------------------------------------------------------

uint32_t w16_syscolor(unsigned index);            // COLOR_* -> ugfx colour
uint32_t w16_brush_color(uint16_t brush);
void w16_gdi_reset(struct w16 *w);
struct ugfx_font;
const struct ugfx_font *w16_system_font(void);   // under g_w16_text_lock

// --- frame16.c (main thread) ---------------------------------------------------

#define W16_FRAME      4                          // the sizing border
#define W16_CAPTION    20                         // the caption, its bottom line included

struct ugfx_surface;
void w16_frame_draw(struct ugfx_surface *s, const struct w16_wnd *win, int active, int pressed);
// What a point of the window is: the client, the caption, a button, or a
// sizing edge (WIN_EDGE_* bits in *edges).
enum w16_hit { HIT_CLIENT, HIT_CAPTION, HIT_SYSMENU, HIT_MIN, HIT_MAX, HIT_EDGE, HIT_NONE };
enum w16_hit w16_frame_hit(const struct w16_wnd *win, int x, int y, unsigned *edges);
void w16_client_rect(const struct w16_wnd *win, int *x, int *y, int *cw, int *ch);

#endif
