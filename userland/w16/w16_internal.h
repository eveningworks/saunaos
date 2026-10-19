// w16_internal.h -- what w16's parts share (docs/w16-design.md). Split by
// concern, as userland/wm/ is:
//   ne.c, ne_info.c   read the NE file; `--info`
//   cpu16.c           the 16-bit CPU
//   task16.c          load the program, its thread, the message queue,
//                     and the dispatch of its imports
//   kernel16.c        KERNEL (and INT 21h), user16.c USER, gdi16.c GDI
//   usermisc16.c      USER's rectangles, timers, capture, wsprintf
//   mem16.c           KERNEL's global and local heaps
//   res16.c           resources: FindResource, LoadResource, LoadString
//   blit16.c          GDI's pixel work: raster ops, lines, DIBs
//   frame16.c         the Windows 3.1 caption and frame (main thread)
//   menu16.c          a 3.1 popup menu; the control menu is its first
//   msgbox16.c        a 3.1 message box; the Application Error is its first
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
#define WM_SYSCOMMAND    0x0112
#define WM_TIMER         0x0113

// WM_SYSCOMMAND's wParam: what the frame, the control menu and Alt+Space
// ask, sent through the program so that it may refuse (DefWindowProc
// acts on them).
#define SC_SIZE          0xF000
#define SC_MOVE          0xF010
#define SC_MINIMIZE      0xF020
#define SC_MAXIMIZE      0xF030
#define SC_CLOSE         0xF060
#define SC_KEYMENU       0xF100
#define SC_RESTORE       0xF120

#define SIZE_RESTORED    0                        // WM_SIZE's wParam
#define SIZE_MAXIMIZED   2

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
#define W16_LBLOCKS  512          // local-heap blocks
#define W16_MAX_RES  512          // resources whose loaded block is remembered
#define W16_GDI_MAX  1024         // GDI objects alive at once
#define W16_TIMERS   16

// A local-heap block, kept host-side (mem16.c).
struct w16_lblock {
    uint16_t off, size;
    uint8_t used;
};

// A GDI object (gdi16.c). A DC's drawing state lives in it; a bitmap's
// pixels are 32-bit, `mono` for a 1-bit one (black or white only).
enum { GDI_FREE, GDI_PEN, GDI_BRUSH, GDI_BITMAP, GDI_DC, GDI_FONT };
struct w16_gdiobj {
    uint8_t type;
    uint32_t color;               // pen, brush: a COLORREF
    uint16_t style, width;        // pen, brush
    int bw, bh, mono;             // bitmap
    uint32_t *px;
    uint16_t in_dc;               // bitmap: the DC it is selected into
    // DC
    int window;                   // draws on the window's client area
    uint16_t bitmap, pen, brush, font;
    uint32_t text, bk;            // COLORREFs
    int bk_mode, rop2;
    int16_t cx, cy;               // the current position
};

struct w16_timer {
    uint16_t id;
    uint32_t proc;                // a TIMERPROC far pointer, or 0 for WM_TIMER to the window
    uint32_t interval_ms;
    uint64_t due_ms;
    int pending;                  // a WM_TIMER is queued and not yet retrieved
    int used;
};

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
    int maximized;                // toywm's last proposal said so; under `lock`
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
    int pending_syscmd;                          // an SC_* asked before the window was up
    char missing[64];                            // "USER.171 (WinHelp)": the call that is not there

    uint32_t text_color, bk_color;               // the window DC's, reset at BeginPaint
    int bk_mode;

    struct {                                     // the local heap (mem16.c)
        struct w16_lblock blk[W16_LBLOCKS];
        int n;
    } lheap;
    uint16_t res_sel[W16_MAX_RES];               // a loaded resource's global block
    struct w16_gdiobj gdi[W16_GDI_MAX];          // gdi16.c; handle = GDI_BASE + 4 * index
    uint16_t window_dc;                          // the one DC of the window
    int fb_dirty;                                // the window DC drew: post a repaint
    struct w16_timer timer[W16_TIMERS];          // set by the task, fired by the main thread
    uint16_t capture;                            // SetCapture's window, 0 none
    uint16_t menu;                               // SetMenu's, not drawn yet
    int16_t mouse_x, mouse_y;                    // the last mouse message's point, client coordinates
};

extern struct w16 g_w16;
#include <stdio.h>
extern FILE *g_w16_trace;                        // --trace FILE: each call into KERNEL/USER/GDI
extern pthread_mutex_t g_w16_text_lock;          // ugfx's text is not thread-safe

// uapp_post() codes, task -> main.
#define W16_POST_PAINT 1
#define W16_POST_DONE  2
#define W16_POST_TITLE 3
#define W16_POST_SYSCMD 4    // arg: the SC_* DefWindowProc carried out
#define W16_POST_FAULT  5    // the program stopped: show why, then end
#define W16_POST_SIZE   6    // MoveWindow resized the window: the surface follows

// --- task16.c --------------------------------------------------------------

int w16_load(struct w16 *w, const uint8_t *img, uint32_t size, const char *path);
void *w16_task_main(void *arg);
void w16_post_msg(struct w16 *w, uint16_t hwnd, uint16_t msg, uint16_t wparam, uint32_t lparam);
uint32_t w16_call_wndproc(struct w16 *w, uint16_t sel, uint16_t off, uint16_t hwnd,
                          uint16_t msg, uint16_t wparam, uint32_t lparam);
void w16_task_post(struct w16 *w, int code);     // uapp_post() once the main thread is up
void w16_task_post_arg(struct w16 *w, int code, int arg);
// Why the program stopped, filled by the task thread as it ends: the
// first line is the crash report's headline (set_abort_message()).
extern char g_w16_fault[1024];
extern char g_w16_fault_box[512];               // the same, as the Application Error box says it

// Pascal arguments, read FIRST TO LAST: the first was pushed first, so it
// sits highest, just under the byte count the function pops.
struct w16_args {
    struct cpu16 *c;
    unsigned pos;
};
uint16_t w16_arg16(struct w16_args *a);
uint32_t w16_arg32(struct w16_args *a);

// AN API IS A TABLE of ordinal -> handler, sorted by ordinal -- what a
// Wine .spec file says, here in C -- so each export is a named function
// (api_SetTimer) and a module grows by a row, not a case.
struct w16_api {
    uint16_t ordinal;
    int (*fn)(struct w16 *w, unsigned ordinal, struct w16_args *a);
};
// The handler's result, or -2 when the table has no such ordinal.
int w16_api_call(const struct w16_api *t, unsigned n, struct w16 *w, unsigned ordinal,
                 struct w16_args *a);

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
uint16_t w16_gdi_window_dc(struct w16 *w);        // the window's DC, its state reset
void w16_gdi_flush(struct w16 *w);                // post a repaint if the window DC drew
// A brush handle's colour as a pixel (stock, COLOR_*+1 or created); fill
// a client rectangle with it -- USER's FillRect and WM_ERASEBKGND.
uint32_t w16_brush_px(struct w16 *w, uint16_t brush);
void w16_gdi_fill(struct w16 *w, uint16_t hdc, int l, int t, int r, int b, uint16_t brush);

// --- mem16.c / res16.c --------------------------------------------------------

uint16_t w16_global_alloc(struct w16 *w, uint32_t size, const void *init);   // a selector, or 0
void w16_global_free(struct w16 *w, uint16_t h);
void w16_local_init(struct w16 *w);
int w16_kernel_mem(struct w16 *w, unsigned ordinal, struct w16_args *a);   // -2: not its own
int w16_kernel_res(struct w16 *w, unsigned ordinal, struct w16_args *a);
int w16_user_loadstring(struct w16 *w, struct w16_args *a);
int w16_user_misc(struct w16 *w, unsigned ordinal, struct w16_args *a);   // usermisc16.c
void w16_timers_fire(struct w16 *w);              // the main thread's tick
void w16_cursor_screen(struct w16 *w, int16_t *x, int16_t *y);   // MSG.pt, GetCursorPos
uint64_t w16_now_ms(void);
int w16_load_string(struct w16 *w, uint16_t id, char *out, unsigned cap);
uint16_t w16_res_load(struct w16 *w, unsigned index);
int w16_res_find(struct w16 *w, uint16_t type, const char *type_name, uint16_t id,
                 const char *name, struct ne_resource *out);

// --- blit16.c -----------------------------------------------------------------

struct gdi_target {
    uint32_t *px;
    int w, h, mono;
};

struct gdi_blit {
    const struct gdi_target *dst, *src;  // src NULL for a pattern-only ROP
    int x, y, w, h, sx, sy;
    uint32_t rop;                        // a ROP3, its code in bits 16-23
    uint32_t brush;                      // the pattern, a pixel
    uint32_t dst_text, dst_bk;           // pixels, for a 1-bit source
    uint32_t src_bk;                     // a pixel, for a 1-bit destination
};

struct gdi_dib {
    int32_t w, h;                        // h < 0: rows top-down
    unsigned bpp, ncolors;
    uint32_t pal[256];                   // as pixels
    uint32_t stride, header_len;         // header plus palette
};

uint32_t w16_colorref_px(uint32_t cr);
uint32_t gdi_rop3(uint8_t code, uint32_t p, uint32_t s, uint32_t d);
uint32_t gdi_rop2(int r2, uint32_t p, uint32_t d);
void gdi_bitblt(const struct gdi_blit *b);
void gdi_pixel(const struct gdi_target *t, int x, int y, uint32_t px, int r2);
void gdi_line(const struct gdi_target *t, int x0, int y0, int x1, int y1, uint32_t px, int width, int r2);
int gdi_dib_header(const uint8_t *bmi, uint32_t bmi_len, int pal_indices, struct gdi_dib *out);
uint32_t gdi_dib_px(const struct gdi_dib *d, const uint8_t *row, int x);
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
// The control-menu box, in window coordinates: what its menu drops from.
void w16_frame_sysbox(const struct w16_wnd *win, int *x, int *y, int *w, int *h);

// --- menu16.c / msgbox16.c (main thread) -------------------------------------
//
// Both draw into a toolkit popup (UUI_POPUP_NO_DECOR: the 3.1 look is
// square, hard-shadowed and its own), so they may reach past a small
// window. Input arrives in WINDOW coordinates, as uapp hands it on.

#define W16_MENU_MAX 16
#define W16_MF_GRAYED    0x1
#define W16_MF_SEPARATOR 0x2

struct w16_menu_item {
    char text[40];                // "&Restore": & marks the mnemonic
    char accel[16];               // "Alt+F4", right-aligned; "" none
    unsigned flags;               // W16_MF_*
    unsigned id;                  // what choosing it returns (an SC_* here)
};

struct w16_menu {
    struct w16_menu_item item[W16_MENU_MAX];
    int n;
    int popup;                    // the uui_popup id; 0 closed
    int x, y, w, h;               // where it landed, window coordinates
    int sel;                      // the highlighted item, -1 none
    int armed;                    // a press began inside it
};

void w16_menu_open(struct w16_menu *m, int ax, int ay, int aw, int ah, int by_key);
void w16_menu_close(struct w16_menu *m);
void w16_menu_draw(struct w16_menu *m);
// Each returns the chosen item's id, 0 for nothing chosen yet, and -1
// once the menu has closed without a choice.
unsigned w16_menu_motion(struct w16_menu *m, int x, int y);
int w16_menu_press(struct w16_menu *m, int x, int y);    // 1: inside, 0: outside (now closed)
unsigned w16_menu_release(struct w16_menu *m, int x, int y);
unsigned w16_menu_key(struct w16_menu *m, int key);      // KEY_* or a character
int w16_menu_open_p(const struct w16_menu *m);

// The control menu of `win`, its items enabled as its style and state allow.
void w16_sysmenu_build(struct w16_menu *m, const struct w16_wnd *win);

struct w16_msgbox {
    char caption[48];
    char text[512];               // wrapped to the box; '\n' breaks
    int popup;
    int x, y, w, h;
    int btn_x, btn_y, btn_w, btn_h;
    int pressed;                  // the Close button is held down
    void (*on_dismiss)(void);     // the compositor closed it: taken as the button
};

void w16_msgbox_open(struct w16_msgbox *b, int win_w, int win_h);
void w16_msgbox_draw(struct w16_msgbox *b);
void w16_msgbox_close(struct w16_msgbox *b);
// 1 when the box is answered: its button released on it, Enter or Esc.
int w16_msgbox_press(struct w16_msgbox *b, int x, int y);
int w16_msgbox_release(struct w16_msgbox *b, int x, int y);
int w16_msgbox_key(struct w16_msgbox *b, int key);

#endif
