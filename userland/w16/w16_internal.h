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
//   wnd16.c           the window table: dialogs and their controls
//   dialog16.c        DialogBox and the dialog manager, BUTTON/STATIC/EDIT,
//                     MessageBox (task thread)
//   dlgwin16.c        a dialog's own window: its 3.1 look and input (main thread)
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
#define WM_SETFOCUS      0x0007
#define WM_KILLFOCUS     0x0008
#define WM_SETTEXT       0x000C
#define WM_GETTEXT       0x000D
#define WM_INITDIALOG    0x0110
#define WM_COMMAND       0x0111
// w16's own, never a program's: a key for a dialog, raw (KEY_* in
// wParam, KEY_MOD_* in lParam), which the dialog manager turns into 3.1's
// keyboard interface before any control hears it.
#define W16_WM_DLGKEY    0x7F00
// Also w16's own: the main thread asks the task to open menu-bar title
// wParam (lParam 1: the first since none was open). Handled inside
// GetMessage, so the program never sees it.
#define W16_WM_MENUOPEN  0x7F01

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
#define W16_HWND         0x2001                   // the main window; wnd16.c numbers the rest

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

// A window. `g_w16.win` is the program's main one, whose RECT is the
// toywm window (frame16.c draws the non-client part around its client
// buffer); the table (wnd16.c) holds the rest -- a dialog, whose own
// toywm window dlgwin16.c draws, and each of its controls, which paint
// into their dialog's pixels as a Windows child window does.
enum w16_kind { W16_K_MAIN, W16_K_DIALOG, W16_K_BUTTON, W16_K_STATIC, W16_K_EDIT };
struct uapp_window;
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

    // A dialog or one of its controls; the rest under `lock`.
    int kind;                     // enum w16_kind
    uint16_t parent;              // a control's dialog; a dialog's owner
    uint16_t id;                  // a control's id
    int x, y;                     // a control's place in its dialog's client
    char *text;                   // a control's text, a dialog's caption: allocated
    int pressed, focused, is_default;
    int caret, anchor, scroll;    // an EDIT's caret, selection's other end, first shown char
    uint32_t dlgproc;             // a dialog's DLGPROC, a far pointer
    uint16_t focus;               // a dialog's focused control
    uint16_t capture;             // a dialog's control that took the press
    int ended;                    // EndDialog was called
    int16_t result;
    struct uapp_window *uwin;     // a dialog's toywm window (main thread's)
    int active;                   // a dialog's window has the focus (main thread's)
    int menubar;                  // the main window has a menu bar (SetMenu); under `lock`
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
    struct w16_wnd **wnds;                       // wnd16.c: dialogs and controls, grown
    int nwnds;

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
    uint16_t menu;                               // SetMenu's HMENU, the bar under the caption
    unsigned key_mods;                           // Shift 1, Ctrl 2, Alt 4 as the last key was sent
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
#define W16_POST_DLG_OPEN  7    // arg: a dialog's hwnd -- open its window
#define W16_POST_DLG_CLOSE 8    // arg: a dialog's hwnd -- close it, then the task frees it
#define W16_POST_DLG_PAINT 9    // arg: a dialog's hwnd -- redraw it
#define W16_POST_MENU_SHOW 10   // arg: a menu-bar title -- the program has seen it open: show it

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
extern const struct w16_api g_kernel_api[], g_kernel_mem_api[], g_kernel_res_api[], g_profile_api[],
                            g_user_api[], g_user_misc_api[], g_gdi_api[];
extern const unsigned g_kernel_api_n, g_kernel_mem_api_n, g_kernel_res_api_n, g_profile_api_n,
                      g_user_api_n, g_user_misc_api_n, g_gdi_api_n;
// Does w16 implement MODULE.ordinal? What `w16 --check` counts.
int w16_has(const char *module, unsigned ordinal);

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

// user16.c: the next message for any window (waiting), and handing one to
// its window -- what a dialog's modal loop does with the program's own.
int w16_get_msg(struct w16 *w, struct w16_msg *m);
uint32_t w16_dispatch(struct w16 *w, const struct w16_msg *m);

// --- wnd16.c / dialog16.c / dlgwin16.c -------------------------------------------

// The window an HWND names -- the main one or a table entry -- or NULL.
struct w16_wnd *w16_wnd_get(struct w16 *w, uint16_t hwnd);
struct w16_wnd *w16_wnd_new(struct w16 *w, int kind);   // zeroed, its hwnd set; under no lock
void w16_wnd_free(struct w16 *w, struct w16_wnd *x);
// The control of dialog `dlg` with id `id`, or NULL.
struct w16_wnd *w16_dlg_item(struct w16 *w, uint16_t dlg, uint16_t id);
// The control at (x, y) of a dialog's client, or NULL.
struct w16_wnd *w16_dlg_item_at(struct w16 *w, uint16_t dlg, int x, int y);
// A dialog's frame and caption, in its window's 1x pixels.
int w16_dlg_frame(const struct w16_wnd *d);
int w16_dlg_caption(const struct w16_wnd *d);
extern const struct w16_api g_dialog_api[], g_shell_api[], g_sound_api[], g_menu_api[];
extern const unsigned g_menu_api_n;
// menubar16.c: the task's half of opening a bar title, and the main
// thread's -- the bar drawn, a title found, its popup built. Main-thread
// calls hold `lock`.
void w16_menu_opening(struct w16 *w, int pos, int first);
struct w16_menu;
struct ugfx_surface;
int w16_menubar_count(struct w16 *w);
int w16_menubar_title_at(struct w16 *w, int x, int *tx, int *tw);   // x < 0: title -x-1's place
int w16_menubar_mnemonic(struct w16 *w, int ch);
void w16_menubar_draw(struct ugfx_surface *s, struct w16 *w, int x, int y, int width, int open);
void w16_menubar_build(struct w16 *w, int pos, struct w16_menu *out);
// A 3.1 message box, modal over `owner`: MB_* `type`, the IDOK... chosen.
int16_t w16_message_box(struct w16 *w, uint16_t owner, const char *text, const char *caption,
                        unsigned type);
extern const unsigned g_dialog_api_n, g_shell_api_n, g_sound_api_n;
// Main thread: a dialog's window, from the W16_POST_DLG_* codes.
void w16_dlgwin_open(struct w16 *w, struct uapp *a, uint16_t hwnd);
void w16_dlgwin_close(struct w16 *w, uint16_t hwnd);
void w16_dlgwin_redraw(struct w16 *w, uint16_t hwnd);
// A dialog's whole 1x picture, frame included, into `px` (d->w * d->h).
void w16_dlg_paint(struct w16 *w, const struct w16_wnd *d, uint32_t *px);
// The DLU -> pixel base units of the dialog font: x per 4, y per 8.
void w16_dlg_units(int *bx, int *by);

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

// --- ini16.c ------------------------------------------------------------------

extern char g_w16_drive_c[128];                   // drive C:'s host folder
struct w16_prefs;
extern struct w16_prefs g_w16_prefs;              // how this program runs (w16prefs.h)
int w16_icon(const struct ne_module *m, uint32_t *px, int *w, int *h);   // icon16.c
int w16_ini_get(const char *file, const char *sec, const char *key, char *out, size_t cap);
size_t w16_ini_names(const char *file, const char *sec, char *out, size_t cap);
int w16_ini_set(const char *file, const char *sec, const char *key, const char *val);

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
enum w16_hit { HIT_CLIENT, HIT_CAPTION, HIT_SYSMENU, HIT_MIN, HIT_MAX, HIT_EDGE, HIT_MENU, HIT_NONE };
#define W16_MENU_H     18                         // the menu bar, its bottom line included (SM_CYMENU)
// The menu bar's rectangle in window coordinates (zero height: none).
void w16_frame_menubar(const struct w16_wnd *win, int *x, int *y, int *w, int *h);
enum w16_hit w16_frame_hit(const struct w16_wnd *win, int x, int y, unsigned *edges);
void w16_client_rect(const struct w16_wnd *win, int *x, int *y, int *cw, int *ch);
// scale16.c: the scale in quarters (4 = 1x, 6 = 1.5x) and how a
// fractional one is drawn (w16prefs.h's W16_METHOD_*); a 1x length in
// the window's real pixels, and back.
extern int g_w16_q, g_w16_method;
int w16_up(int v);
int w16_down(int v);
extern int g_w16_desktop_frame;                   // toywm's frame, not 3.1's
void w16_frame_fit(const struct w16_wnd *win, int *w, int *h);   // a 3.1 outer size -> the window's
void w16_scale_blit(struct ugfx_surface *dst, int dx, int dy, const uint32_t *src, int w, int h,
                    int stride);

// The control-menu box, in window coordinates: what its menu drops from.
void w16_frame_sysbox(const struct w16_wnd *win, int *x, int *y, int *w, int *h);

// --- menu16.c / msgbox16.c (main thread) -------------------------------------
//
// Both draw into a toolkit popup (UUI_POPUP_NO_DECOR: the 3.1 look is
// square, hard-shadowed and its own), so they may reach past a small
// window. Input arrives in WINDOW coordinates, as uapp hands it on.

#define W16_MF_GRAYED    0x1
#define W16_MF_SEPARATOR 0x2
#define W16_MF_CHECKED   0x4

struct w16_menu_item {
    char *text;                   // "&Restore": & marks the mnemonic; allocated
    char *accel;                  // "Alt+F4", right-aligned; "" none; allocated
    unsigned flags;               // W16_MF_*
    unsigned id;                  // what choosing it returns (an SC_* here)
};

struct w16_menu {
    struct w16_menu_item *item;   // as many as it has: w16_menu_add()
    int n, cap;
    int popup;                    // the uui_popup id; 0 closed
    int x, y, w, h;               // where it landed, window coordinates
    int sel;                      // the highlighted item, -1 none
    int armed;                    // a press began inside it
    int row_h;                    // a row's height in the system font, set as it opens
};

void w16_menu_open(struct w16_menu *m, int ax, int ay, int aw, int ah, int by_key);
void w16_menu_close(struct w16_menu *m);
void w16_menu_draw(struct w16_menu *m);
// Each returns the chosen item's id, 0 for nothing chosen yet, and -1
// once the menu has closed without a choice. w16_menu_key also returns
// W16_MENU_LEFT or W16_MENU_RIGHT for an arrow a menu bar moves on.
#define W16_MENU_LEFT  ((unsigned)-2)
#define W16_MENU_RIGHT ((unsigned)-3)
unsigned w16_menu_motion(struct w16_menu *m, int x, int y);
int w16_menu_press(struct w16_menu *m, int x, int y);    // 1: inside, 0: outside (now closed)
unsigned w16_menu_release(struct w16_menu *m, int x, int y);
unsigned w16_menu_key(struct w16_menu *m, int key);      // KEY_* or a character
int w16_menu_open_p(const struct w16_menu *m);

// The control menu of `win`, its items enabled as its style and state allow.
void w16_sysmenu_build(struct w16_menu *m, const struct w16_wnd *win);
// Empties `m` (closed or not); then items are added one by one.
void w16_menu_clear(struct w16_menu *m);
void w16_menu_add(struct w16_menu *m, const char *text, const char *accel, unsigned id, unsigned flags);

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
// 3.1's STOP sign, 32 pixels square: the Error box's and MB_ICONHAND's.
void w16_stop_sign(struct ugfx_surface *s, int x, int y);

#endif
