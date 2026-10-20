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
//   wnd16.c           the window tree: every HWND, its place, its painting
//   ctl16.c           the BUTTON and STATIC classes, and how a control draws
//   edit16.c          the EDIT class
//   scroll16.c        a window's scroll bars
//   dialog16.c        DialogBox, CreateDialog and the dialog manager,
//                     MessageBox (task thread)
//   dlgwin16.c        a dialog's own toywm window: its 3.1 frame (main thread)
//   module16.c        binding an image to Windows' modules; a program's own
//                     DLLs (LoadLibrary, GetProcAddress)
//   ddeml16.c         DDEML, mmsystem16.c MMSYSTEM: stand-ins (no peer, no wave device)
//
// TWO THREADS. The program -- interpreter, message loop, GDI -- runs on
// the TASK thread, so a modal loop inside a window procedure simply
// blocks there. The uapp MAIN thread owns the toywm windows: it draws the
// frames, copies the client pixels, and turns input into messages. They
// meet in `struct w16` under `lock`: the queue, the windows' geometry and
// their client buffers. The task tells the main thread to repaint with
// uapp_post(), the one toolkit call a worker may make.
//
// EVERY PIXEL OF A CLIENT AREA IS PAINTED ON THE TASK THREAD -- the
// program's own windows and w16's controls alike, each into its top-level
// window's buffer, as Windows painted a child into its parent's screen
// area. The main thread only copies that buffer and draws the frame round
// it, so it never reads the program's memory, which the task may be
// moving.
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
#define WM_MOVE          0x0003
#define WM_SIZE          0x0005
#define WM_ACTIVATE      0x0006
#define WM_SETFOCUS      0x0007
#define WM_KILLFOCUS     0x0008
#define WM_ENABLE        0x000A
#define WM_SETREDRAW     0x000B
#define WM_SETTEXT       0x000C
#define WM_GETTEXT       0x000D
#define WM_GETTEXTLENGTH 0x000E
#define WM_PAINT         0x000F
#define WM_CLOSE         0x0010
#define WM_QUIT          0x0012
#define WM_ERASEBKGND    0x0014
#define WM_SHOWWINDOW    0x0018
#define WM_CTLCOLOR      0x0019
#define WM_SETCURSOR     0x0020
#define WM_MOUSEACTIVATE 0x0021
#define WM_SETFONT       0x0030
#define WM_GETFONT       0x0031
#define WM_NCCREATE      0x0081
#define WM_NCDESTROY     0x0082
#define WM_NCCALCSIZE    0x0083
#define WM_NCHITTEST     0x0084
#define WM_NCPAINT       0x0085
#define WM_NCACTIVATE    0x0086
#define WM_GETDLGCODE    0x0087
#define WM_NCLBUTTONDOWN 0x00A1
#define WM_KEYDOWN       0x0100
#define WM_KEYUP         0x0101
#define WM_CHAR          0x0102
#define WM_SYSKEYDOWN    0x0104
#define WM_SYSKEYUP      0x0105
#define WM_SYSCHAR       0x0106
#define WM_INITDIALOG    0x0110
#define WM_COMMAND       0x0111
#define WM_SYSCOMMAND    0x0112
#define WM_TIMER         0x0113
#define WM_HSCROLL       0x0114
#define WM_VSCROLL       0x0115
#define WM_MOUSEMOVE     0x0200
#define WM_LBUTTONDOWN   0x0201
#define WM_LBUTTONUP     0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN   0x0204
#define WM_RBUTTONUP     0x0205
#define WM_CUT           0x0300
#define WM_COPY          0x0301
#define WM_PASTE         0x0302
#define WM_CLEAR         0x0303
#define WM_UNDO          0x0304
#define WM_USER          0x0400
// w16's own, never a program's: the main thread asks the task to open
// menu-bar title wParam (lParam 1: the first since none was open), and
// tells it a toywm window gained (wParam 1) or lost the focus. Handled
// inside GetMessage, so the program never sees them.
#define W16_WM_MENUOPEN  0x7F01
#define W16_WM_ACTIVATE  0x7F02

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

#define WS_POPUP         0x80000000u
#define WS_CHILD         0x40000000u
#define WS_VISIBLE       0x10000000u
#define WS_CLIPCHILDREN  0x02000000u
#define WS_DISABLED      0x08000000u
#define WS_CAPTION       0x00C00000u
#define WS_BORDER        0x00800000u
#define WS_DLGFRAME      0x00400000u
#define WS_VSCROLL       0x00200000u
#define WS_HSCROLL       0x00100000u
#define WS_SYSMENU       0x00080000u
#define WS_THICKFRAME    0x00040000u
#define WS_GROUP         0x00020000u
#define WS_TABSTOP       0x00010000u
#define WS_MINIMIZEBOX   0x00020000u
#define WS_MAXIMIZEBOX   0x00010000u
#define DS_MODALFRAME    0x00000080u              // a dialog's thick (navy-banded) frame

// WM_GETDLGCODE's answers: what keys a control keeps from the dialog manager.
#define DLGC_WANTARROWS      0x0001
#define DLGC_WANTTAB         0x0002
#define DLGC_WANTALLKEYS     0x0004
#define DLGC_WANTMESSAGE     0x0004
#define DLGC_HASSETSEL       0x0008
#define DLGC_DEFPUSHBUTTON   0x0010
#define DLGC_UNDEFPUSHBUTTON 0x0020
#define DLGC_RADIOBUTTON     0x0040
#define DLGC_WANTCHARS       0x0080
#define DLGC_STATIC          0x0100
#define DLGC_BUTTON          0x2000

#define VK_SHIFT   0x10
#define VK_CONTROL 0x11
#define VK_PRIOR   0x21
#define VK_NEXT    0x22
#define VK_END     0x23
#define VK_HOME    0x24
#define VK_LEFT    0x25
#define VK_UP      0x26
#define VK_RIGHT   0x27
#define VK_DOWN    0x28
#define VK_INSERT  0x2D
#define VK_DELETE  0x2E

#define W16_CW_USEDEFAULT ((int16_t)0x8000)
#define W16_HWND         0x2001                   // the main window; wnd16.c numbers the rest
#define W16_DESKTOP      0x2000                   // GetDesktopWindow's
#define W16_SB           17                       // a scroll bar's width (SM_CXVSCROLL)

// --- the shared state ------------------------------------------------------

// A queued message. `mods` is the Shift/Ctrl/Alt state (KEY_MOD_*) as the
// main thread saw it with a key -- what GetKeyState answers while the
// program handles that message. Not part of a program's MSG.
struct w16_msg {
    uint16_t hwnd, message, wparam;
    uint32_t lparam;
    uint8_t mods;
};

#define W16_QUEUE    256
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
enum { GDI_FREE, GDI_PEN, GDI_BRUSH, GDI_BITMAP, GDI_DC, GDI_FONT, GDI_REGION };
struct w16_gdiobj {
    uint8_t type;
    uint32_t color;               // pen, brush: a COLORREF
    uint16_t style, width;        // pen, brush; a hatched brush's HS_* is `width`
    int16_t rgn[4];               // region: its bounding rectangle...
    uint8_t complex;              // ...and whether it is more than that rectangle
    int bw, bh, mono;             // bitmap; a pattern brush's 8x8 in px (mono: 0 and 1)
    uint32_t *px;
    uint16_t in_dc;               // bitmap: the DC it is selected into
    struct w16_logfont *lf;       // font: what CreateFont asked for, allocated
    // DC
    uint16_t hwnd;                // a window's DC: it draws on that window's client area
    uint8_t screen;               // the display's DC before any window: answers, draws nothing
    uint16_t bitmap, pen, brush, font;
    uint32_t text, bk;            // COLORREFs
    int bk_mode, rop2;
    int16_t cx, cy;               // the current position
    int16_t brush_ox, brush_oy;   // SetBrushOrg's: kept and answered, no brush is patterned yet
};

struct w16_timer {
    uint16_t hwnd, id;
    uint32_t proc;                // a TIMERPROC far pointer, or 0 for WM_TIMER to the window
    uint32_t interval_ms;
    uint64_t due_ms;
    int pending;                  // a WM_TIMER is queued and not yet retrieved
    int used;
};

struct w16;
struct w16_wnd;
// A window procedure written in C: a built-in class's (BUTTON, EDIT...)
// or the dialog manager's. Called on the task thread, as a program's is.
typedef uint32_t (*w16_wndproc)(struct w16 *w, struct w16_wnd *x, uint16_t msg, uint16_t wp, uint32_t lp);
// A dialog procedure written in C, for a dialog w16 builds (MessageBox,
// the common dialogs): nonzero when it handled the message, as a DLGPROC.
typedef int (*w16_dlgproc)(struct w16 *w, struct w16_wnd *d, uint16_t msg, uint16_t wp, uint32_t lp);

struct w16_class {
    char *name;                   // allocated
    uint16_t atom;                // what RegisterClass returned
    uint16_t proc_sel, proc_off;  // the program's window procedure...
    w16_wndproc host;             // ...or w16's own: a built-in class
    uint16_t background;          // an HBRUSH, or COLOR_* + 1
    uint16_t style;
    uint16_t wnd_extra;           // cbWndExtra: the bytes Get/SetWindowWord reach
    uint8_t cursor;               // hCursor as a WIN_CURSOR_* shape: an EDIT's is the I-beam
    char *menu;                   // lpszMenuName: its windows' menu, by name...
    uint16_t menu_id;             // ...or by number (an ordinal)
};

// A window: the program's main one (`g_w16.win`, whose RECT is the toywm
// window, frame16.c drawing the non-client part round its buffer), a
// dialog (a toywm window of its own, dlgwin16.c's), or a CHILD -- a
// program's or a control -- which has no surface of its own: it paints
// into its top-level window's buffer at its place, as on Windows.
enum w16_kind { W16_K_MAIN, W16_K_DIALOG, W16_K_CHILD };
struct uapp_window;
struct w16_wnd {
    int used;
    uint16_t hwnd;
    unsigned order;               // its creation number: siblings' z-order and tab order
    int kind;                     // enum w16_kind
    int cls;                      // its class, an index into g_w16.cls
    uint32_t style;
    uint16_t parent;              // a child's parent; a dialog's owner
    uint16_t id;                  // a child's id (CreateWindow's hMenu)
    uint16_t inst;                // its hInstance: whose local heap an EDIT keeps its text in
    int x, y;                     // a child's place in its parent's client area
    int w, h;                     // the whole window, frame, border and scroll bars included
    int cw, ch;                   // the client area
    uint32_t *fb;                 // a top-level window's interior: client area and its scroll bars,
    int fbw, fbh;                 // fbw * fbh pixels, under `lock`
    // THE LAST COMPLETE FRAME, what the window shows: `fb` copied when the
    // program has painted all it owed (w16_gdi_flush) -- Wayland's commit.
    // The main thread draws this, never `fb`, which may be half painted.
    uint32_t *shown;
    int shw, shh;
    int visible;
    int transparent;              // a press goes through it to what is under (a label)
    int maximized;                // toywm's last proposal said so; under `lock`
    int need_size;                // an overlapped window's first WM_SIZE waits for its first show
    int inval, erase;             // a pending WM_PAINT, and whether it erases
    char *text;                   // its title or label (WM_SETTEXT), allocated; an EDIT's is its own
    uint16_t proc_sel, proc_off;  // its window procedure: the class's until SetWindowLong
    w16_wndproc host;
    uint8_t *extra;               // cbWndExtra bytes, zeroed
    uint16_t dc;                  // its window DC, once one was asked for
    struct w16_sbar { int16_t min, max, pos; } sb[2];   // SB_HORZ, SB_VERT: WS_HSCROLL, WS_VSCROLL
    int keep_sb;                  // an empty range leaves its bars shown (LBS_DISABLENOSCROLL)
    void *ctl;                    // a built-in class's own state, freed with the window
    int destroyed;                // WM_DESTROY was sent
    uint16_t font;                // the HFONT a built-in control draws in (WM_SETFONT); 0 the System font
    int has_ctl_bg;               // its parent answered WM_CTLCOLOR before this paint...
    uint32_t ctl_bg;              // ...with this brush's colour, a pixel

    // A dialog's; the rest under `lock`.
    uint32_t dlgproc;             // the program's DLGPROC, a far pointer...
    w16_dlgproc hostdlg;          // ...or w16's own
    void *dlgdata;                // w16's own dialog's state
    uint16_t focus;               // the control that last had the focus
    uint16_t defid;               // its default push button's id (DM_SETDEFID)
    int bx, by;                   // its base units: its template's FONT's, else 0 for the System font's
    int modal, ended;             // EndDialog was called (2: its window is gone)
    int16_t result;
    struct uapp_window *uwin;     // its toywm window (main thread's)
    int active;                   // its toywm window has the focus (main thread's)
    int menubar;                  // the main window has a menu bar (SetMenu); under `lock`
};

// A local heap (mem16.c): the program's, or a DLL's in its own DGROUP.
struct w16_lheap {
    struct w16_lblock *blk;                      // its blocks in address order, grown
    int n, cap;
    uint32_t end;                                // where it ends: it grows toward 64 KiB
    uint16_t *h;                                 // every moveable handle's entry
    int nh;
};

// A program's own DLL, loaded (module16.c).
struct w16_dll {
    char name[16];                               // its module name, upper case
    char *path;                                  // the host file
    uint8_t *img;
    uint32_t size;
    struct ne_module ne;
    uint16_t seg_sel[NE_MAX_SEGMENTS + 1];
    uint16_t dgroup;                             // 0: it has no data segment
    uint16_t hinst;                              // its DGROUP, else its first segment
    struct w16_lheap heap;
    int refs;
    int broken;                                  // it failed to load: kept so it is not tried again
    int pending;                                 // bound at the program's load: LibEntry still to run
};

struct w16 {
    const char *path;
    struct ne_module ne;
    struct cpu16 cpu;
    struct cpu16_desc *desc;
    uint16_t seg_sel[NE_MAX_SEGMENTS + 1];       // NE segment n -> its selector
    uint16_t dgroup, psp, sysdata;               // sysdata: what w16 hands the program
    uint8_t *sysmem;
    unsigned sys_top;                            // w16_sys_push()'s stack

    pthread_mutex_t lock;
    pthread_cond_t cond;                         // the task waits for messages on this
    struct w16_msg q[W16_QUEUE];
    unsigned qhead, qtail;
    int quit_posted;
    uint16_t quit_code;

    struct w16_class *cls;                       // registered classes, the built-in ones first
    int ncls, cls_cap;
    char **atoms;                                // the atom table: atom 0xC000 + i
    int natoms;
    struct w16_wnd win;                          // the main window
    struct w16_wnd **wnds;                       // wnd16.c: every other window, grown
    int nwnds;
    uint16_t focus;                              // the window with the keyboard focus; under `lock`

    struct uapp *app;                            // the main thread's, once open
    int window_ready;                            // the first window exists (main waits)
    int task_done;
    int exit_code;
    int active;                                  // the toywm window has focus
    int pending_syscmd;                          // an SC_* asked before the window was up
    int pending_end;                             // W16_POST_DONE or _FAULT posted before it was up
    char missing[64];                            // "USER.171 (WinHelp)": the call that is not there

    struct w16_lheap lheap;                      // the local heap of `dgroup` (mem16.c)
    struct w16_dll **dlls;                       // module16.c: the program's own DLLs, grown
    int ndlls;
    struct { uint16_t sel; char name[16]; } *thunks;   // a Windows module's thunk selector each
    int nthunks;
    uint16_t res_sel[W16_MAX_RES];               // a loaded resource's global block
    struct w16_gdiobj gdi[W16_GDI_MAX];          // gdi16.c; handle = GDI_BASE + 4 * index
    int fb_dirty;                                // a window DC drew: post a repaint
    uint64_t committed_ms;                       // when w16_gdi_flush() last took a frame
    struct w16_timer timer[W16_TIMERS];          // set by the task, fired by the main thread
    uint16_t capture;                            // SetCapture's window, 0 none; under `lock`
    uint16_t menu;                               // SetMenu's HMENU, the bar under the caption
    unsigned key_mods;                           // KEY_MOD_* of the key message being handled
    int16_t mouse_x, mouse_y;                    // the last mouse message's point, client coordinates
    uint16_t mouse_hwnd;                         // ...and the window it was for
};

extern struct w16 g_w16;
#include <stdio.h>
extern FILE *g_w16_trace;                        // --trace FILE: each call into KERNEL/USER/GDI
extern pthread_mutex_t g_w16_text_lock;          // ugfx's text is not thread-safe

// uapp_post() codes, task -> main.
#define W16_POST_PAINT 1
#define W16_POST_DONE  2
#define W16_POST_TITLE 3     // arg: a top-level window's hwnd -- its title changed
#define W16_POST_SYSCMD 4    // arg: the SC_* DefWindowProc carried out
#define W16_POST_FAULT  5    // the program stopped: show why, then end
#define W16_POST_SIZE   6    // MoveWindow resized the window: the surface follows
#define W16_POST_DLG_OPEN  7    // arg: a dialog's hwnd -- open its window
#define W16_POST_DLG_CLOSE 8    // arg: a dialog's hwnd -- close it, then the task frees it
#define W16_POST_DLG_PAINT 9    // arg: a dialog's hwnd -- redraw it
#define W16_POST_MENU_SHOW 10   // arg: a menu-bar title -- the program has seen it open: show it
#define W16_POST_CURSOR    11   // arg: a WIN_CURSOR_* -- SetCursor: the pointer's shape now
#define W16_POST_POINTER   12   // arg: x << 16 | y, in the main window -- SetCursorPos

// --- task16.c --------------------------------------------------------------

// The program loaded, its command tail (what follows its name: a DOS
// command line, 126 characters at most) in its PSP.
int w16_load(struct w16 *w, const uint8_t *img, uint32_t size, const char *path, const char *tail);
// The module an hInstance names -- whose resources LoadBitmap and the rest
// read -- or NULL for 0 (Windows' own).
const struct ne_module *w16_module_of(struct w16 *w, uint16_t inst);
void *w16_task_main(void *arg);
void w16_post_msg(struct w16 *w, uint16_t hwnd, uint16_t msg, uint16_t wparam, uint32_t lparam);
void w16_post_msg_mods(struct w16 *w, uint16_t hwnd, uint16_t msg, uint16_t wparam, uint32_t lparam,
                       unsigned mods);
uint32_t w16_call_wndproc(struct w16 *w, uint16_t sel, uint16_t off, uint16_t hwnd,
                          uint16_t msg, uint16_t wparam, uint32_t lparam);
// A far PASCAL callback: `words` pushed first to last, entered with DS
// and AX the DGROUP; DX:AX back. The registers are the caller's again after.
uint32_t w16_call_far(struct w16 *w, uint16_t sel, uint16_t off, const uint16_t *words, unsigned n);
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
// A STAND-IN's note: a call w16 answers without doing what Windows did (a
// beep that is silent, a printer that is not there), in the words the
// launcher shows as "Partly". NULL for a call w16 has in full, or lacks.
const char *w16_partly(const char *module, unsigned ordinal);

// The handler's result, or -2 when the table has no such ordinal.
int w16_api_call(const struct w16_api *t, unsigned n, struct w16 *w, unsigned ordinal,
                 struct w16_args *a);

// Each module's dispatcher: the bytes to pop, or -1 after naming the
// failure (cpu16_set_fault). `argbytes` is the generated table's.
int w16_kernel(struct w16 *w, unsigned ordinal, struct w16_args *a);
int w16_user(struct w16 *w, unsigned ordinal, struct w16_args *a);
int w16_gdi(struct w16 *w, unsigned ordinal, struct w16_args *a);
int w16_int(struct cpu16 *c, uint8_t n);

// Scratch in the sysdata segment for a structure w16 hands the program
// (a CREATESTRUCT, a PAINTSTRUCT): `len` zeroed bytes, their offset.
// Pushed and popped like a stack, as the calls that use them nest.
uint16_t w16_sys_push(struct w16 *w, unsigned len);
void w16_sys_pop(struct w16 *w, uint16_t at);

// --- user16.c / gdi16.c ------------------------------------------------------

uint32_t w16_syscolor(unsigned index);            // COLOR_* -> ugfx colour
uint32_t w16_brush_color(uint16_t brush);
// A window's DC, its state reset -- BeginPaint's and GetDC's.
uint16_t w16_gdi_window_dc(struct w16 *w, uint16_t hwnd);
// GetDC(NULL): the main window's once there is one, else the display's.
uint16_t w16_gdi_screen_dc(struct w16 *w);
// What the windows drew, made the frame they show (each top-level's `shown`)
// and a repaint posted -- when nothing more is owed a WM_PAINT. Takes `lock`.
void w16_gdi_flush(struct w16 *w);
// The frame a top-level window shows: its last complete one, else (none
// yet) its buffer as it is. Under `lock`.
const uint32_t *w16_shown(const struct w16_wnd *x, int *w, int *h);
void w16_gdi_forget_window(struct w16 *w, uint16_t hwnd);   // its DC goes with it
// A bitmap made from a packed DIB (header, palette, bits): LoadBitmap's. 0 when malformed.
uint16_t w16_gdi_bitmap_of_dib(struct w16 *w, const uint8_t *dib, uint32_t len);
// A brush handle's colour as a pixel (stock, COLOR_*+1 or created); fill
// a client rectangle with it -- USER's FillRect and WM_ERASEBKGND.
uint32_t w16_brush_px(struct w16 *w, uint16_t brush);
void w16_gdi_fill(struct w16 *w, uint16_t hdc, int l, int t, int r, int b, uint16_t brush);
void w16_gdi_invert(struct w16 *w, uint16_t hdc, int l, int t, int r, int b);   // InvertRect's
// DrawText's work: DT_* `fmt`, `rc` in DC coordinates (DT_CALCRECT sets
// its right and bottom). The text's height.
int w16_gdi_draw_text(struct w16 *w, uint16_t hdc, const char *text, int16_t rc[4], unsigned fmt);
// `text` drawn through a DC (TextOut's work; tabs step `tab` pixels when
// tab > 0): its width, -1 for a bad DC.
int w16_gdi_text(struct w16 *w, uint16_t hdc, int x, int y, const char *text, int tab);
void w16_text_size(const char *text, int *cx, int *cy);   // in the system font
void w16_text_size_in(uint16_t hfont, const char *text, int *cx, int *cy);

// The next message for any window. `wait_ms` < 0 waits for one, 0 not at
// all, else at most that long: 1 got one, 0 none. A dialog's modal loop
// and a scroll bar's tracking loop read the program's queue with it.
int w16_get_msg(struct w16 *w, struct w16_msg *m, int wait_ms);
// Only mouse messages for `hwnd` (a tracking loop's), the rest left queued.
int w16_get_mouse_msg(struct w16 *w, uint16_t hwnd, struct w16_msg *m, int wait_ms);
uint32_t w16_dispatch(struct w16 *w, const struct w16_msg *m);
void w16_paint_now(struct w16 *w);                // every WM_PAINT owed, sent now
// SendMessage: the window's procedure called now, the program's or w16's.
uint32_t w16_send(struct w16 *w, uint16_t hwnd, uint16_t msg, uint16_t wp, uint32_t lp);
#define WH_CALLWNDPROC 4
uint32_t w16_hook(int id);                        // the hook procedure set for WH_* `id`, 0 none
// w16's OWN window procedures as far pointers a program can hold and call
// (GetWindowLong(GWL_WNDPROC), GetClassInfo): the thunk module W16HOST, its
// "ordinal" the class index; a call lands in w16_host_proc_call().
#define W16_HOST_MODULE "W16HOST"
uint32_t w16_host_proc_far(struct w16 *w, int cls);
int w16_host_proc_call(struct w16 *w, unsigned cls, struct w16_args *a);
uint32_t w16_def_window_proc(struct w16 *w, struct w16_wnd *x, uint16_t msg, uint16_t wp, uint32_t lp);
// A control telling its parent: WM_COMMAND(id, MAKELONG(hwnd, code)).
void w16_notify(struct w16 *w, struct w16_wnd *x, uint16_t code);
// BeginPaint's work for w16's own controls: the border and scroll bars
// drawn, the window validated.
void w16_paint_begin(struct w16 *w, struct w16_wnd *x);
// The registered class with this name (case does not matter), or -1.
int w16_class_find(struct w16 *w, const char *name);
int w16_class_add_host(struct w16 *w, const char *name, w16_wndproc host, uint16_t style,
                       uint16_t background);
// An atom for `name` (RegisterClass, RegisterWindowMessage), 0 when out of memory.
uint16_t w16_atom_add(struct w16 *w, const char *name);

// --- wnd16.c: the window tree --------------------------------------------------

// The window an HWND names -- the main one or a table entry -- or NULL.
struct w16_wnd *w16_wnd_get(struct w16 *w, uint16_t hwnd);
struct w16_wnd *w16_wnd_new(struct w16 *w, int kind);   // zeroed, its hwnd set; under no lock
void w16_wnd_free(struct w16 *w, struct w16_wnd *x);
// CreateWindow's work, for the program and for w16 alike: the window of
// class `cls` made, WM_NCCREATE/WM_CREATE sent. NULL when refused.
struct w16_wnd *w16_wnd_create(struct w16 *w, int cls, const char *title, uint32_t style, int x, int y,
                               int cx, int cy, uint16_t parent, uint16_t id, uint16_t inst,
                               uint32_t param);
// DestroyWindow: WM_DESTROY to it and every child, then the windows gone.
void w16_wnd_destroy(struct w16 *w, struct w16_wnd *x);
// The top-level window `x` is in (itself, for one), and where x's client
// area lies in that window's client buffer.
struct w16_wnd *w16_wnd_top(struct w16 *w, struct w16_wnd *x);
void w16_wnd_origin(struct w16 *w, struct w16_wnd *x, int *ox, int *oy);
// The part of x's client area its ancestors leave showing, in its
// top-level's client coordinates; 0 when none does.
int w16_wnd_visible_rect(struct w16 *w, struct w16_wnd *x, int *x0, int *y0, int *x1, int *y1);
// The client area's inset in the window: the border and scroll bars.
void w16_wnd_insets(const struct w16_wnd *x, int *l, int *t, int *r, int *b);
void w16_wnd_set_geometry(struct w16 *w, struct w16_wnd *x, int nx, int ny, int nw, int nh);
// A top-level window's buffer made to fit what its frame and menu bar now
// leave inside it, cleared: 1 remade, 0 already fitted, -1 out of memory.
// The caller holds `lock` or owns the window.
int w16_top_fit(struct w16_wnd *win);
// Invalidate x and every child of it; posts nothing (GetMessage finds it).
void w16_invalidate(struct w16 *w, struct w16_wnd *x, int erase);
// The next window owed a WM_PAINT, a parent before its children; NULL none.
struct w16_wnd *w16_wnd_next_paint(struct w16 *w);
// Is x shown: it and every ancestor visible?
int w16_wnd_shown(struct w16 *w, struct w16_wnd *x);
// x's children in creation order, which is their z-order and tab order;
// allocated, *n of them.
struct w16_wnd **w16_wnd_children(struct w16 *w, uint16_t parent, int *n);
int w16_wnd_is_child(struct w16 *w, uint16_t parent, uint16_t hwnd);   // a descendant
// The child of top-level `top` under (x, y) of its client area, deepest
// first, and the point in that child's own coordinates (client, or
// window ones with *nc set when the point is on its border or scroll
// bars); `top` itself when none. Main thread, under `lock`.
struct w16_wnd *w16_wnd_at(struct w16 *w, struct w16_wnd *top, int x, int y, int *cx, int *cy, int *nc);
// SetFocus: WM_KILLFOCUS to the old window, WM_SETFOCUS to the new.
uint16_t w16_set_focus(struct w16 *w, uint16_t hwnd);
// A surface for painting x's client area in C: its top-level's buffer,
// clipped to what of x shows, (*ox, *oy) its origin. Takes `lock`;
// w16_wnd_paint_done() drops it and asks for the repaint. 0: nothing to paint.
struct ugfx_surface;
int w16_wnd_paint(struct w16 *w, struct w16_wnd *x, struct ugfx_surface *s, int *ox, int *oy);
void w16_wnd_paint_done(struct w16 *w);
// The same for the whole window, border and scroll bars included: (*ox,
// *oy) is the window's top-left.
int w16_wnd_paint_nc(struct w16 *w, struct w16_wnd *x, struct ugfx_surface *s, int *ox, int *oy);

// --- ctl16.c / edit16.c / scroll16.c: w16's own classes ---------------------------

// Registered first, so a program's class of the same name loses (Windows' rule).
void w16_register_builtins(struct w16 *w);
uint32_t w16_button_proc(struct w16 *w, struct w16_wnd *x, uint16_t msg, uint16_t wp, uint32_t lp);
uint32_t w16_static_proc(struct w16 *w, struct w16_wnd *x, uint16_t msg, uint16_t wp, uint32_t lp);
uint32_t w16_edit_proc(struct w16 *w, struct w16_wnd *x, uint16_t msg, uint16_t wp, uint32_t lp);
uint32_t w16_dialog_proc(struct w16 *w, struct w16_wnd *x, uint16_t msg, uint16_t wp, uint32_t lp);
char *w16_edit_text(struct w16 *w, struct w16_wnd *x);   // an EDIT's text, allocated
// Drawing in the 3.1 look, shared by the classes: a raised button face,
// text with its '&' mnemonic underlined, a dotted focus rectangle.
void w16_draw_bevel(struct ugfx_surface *s, int x, int y, int w, int h, int pressed);
void w16_draw_label(struct ugfx_surface *s, int x, int y, int w, const char *text, int prefix,
                    uint32_t fg, uint32_t bg);
void w16_draw_focus(struct ugfx_surface *s, int x, int y, int w, int h);
// A message box's icon (1 hand, 2 question, 3 exclamation, 4 asterisk),
// or 0xFF: the program's own.
void w16_draw_icon(struct ugfx_surface *s, struct w16 *w, int kind, int x, int y);
uint32_t w16_rgb(uint32_t rgb);                   // 0xRRGGBB -> a ugfx colour, as VGA showed it
uint32_t w16_vga(uint32_t rgb);                   // 0xRRGGBB as 3.11 at VGA showed it
// Paint a control's client area with `fn` (surface, origin), the system
// font set and the window validated; and the colour its parent shows.
void w16_ctl_paint(struct w16 *w, struct w16_wnd *x,
                   void (*fn)(struct w16 *, struct w16_wnd *, struct ugfx_surface *, int, int));
uint32_t w16_parent_bg(struct w16 *w, struct w16_wnd *x);
uint32_t w16_listbox_proc(struct w16 *w, struct w16_wnd *x, uint16_t msg, uint16_t wp, uint32_t lp);
uint32_t w16_combo_proc(struct w16 *w, struct w16_wnd *x, uint16_t msg, uint16_t wp, uint32_t lp);
// The common dialogs' folder list draws a folder before each item: item
// data's low byte 1 an open folder, 2 a closed one, the next byte its
// indent in steps. Set on a LISTBOX by w16's own code only.
void w16_listbox_folders(struct w16_wnd *x, int on);
uint32_t w16_listbox_str(struct w16 *w, struct w16_wnd *x, uint16_t msg, uint16_t wp, const char *s);
const char *w16_listbox_text(struct w16_wnd *x, int i);   // item i's text, or NULL
struct w16_wnd *w16_combo_list(struct w16 *w, struct w16_wnd *x);
void w16_combo_changed(struct w16 *w, struct w16_wnd *x);   // its list's selection set in C: show it

// A window's standard scroll bars (scroll16.c): drawn with its non-client
// area, tracked by DefWindowProc on WM_NCLBUTTONDOWN, set by the program.
#define SB_HORZ 0
#define SB_VERT 1
void w16_sbar_draw(struct w16 *w, struct w16_wnd *x, struct ugfx_surface *s, int ox, int oy);
void w16_sbar_track(struct w16 *w, struct w16_wnd *x, int bar, int sx, int sy);
int w16_sbar_hit(const struct w16_wnd *x, int wx, int wy);   // -1, SB_HORZ or SB_VERT
// Is bar `bar` of x shown: its style asks for it and its range is not
// empty -- 3.1 hid a standard bar whose range was (SetScrollRange 0, 0).
int w16_sbar_shown(const struct w16_wnd *x, int bar);
// SetScrollRange/SetScrollPos's work, the bar redrawn when `redraw`.
void w16_sbar_set(struct w16 *w, struct w16_wnd *x, int bar, int min, int max, int pos, int redraw);
extern const struct w16_api g_scroll_api[];
extern const unsigned g_scroll_api_n;

// --- dialog16.c / dlgwin16.c ---------------------------------------------------

// The control of dialog `dlg` with id `id`, or NULL.
struct w16_wnd *w16_dlg_item(struct w16 *w, uint16_t dlg, uint16_t id);
// A dialog's frame and caption, in its window's 1x pixels.
int w16_dlg_frame(const struct w16_wnd *d);
int w16_dlg_frame_top(const struct w16_wnd *d);
// A dialog's template FONT, `pt` points: W16_FONT_DIALOG and its units.
void w16_dlg_set_font(struct w16_wnd *d, int pt);
// Dialog units to pixels as 3.11 mapped them (dialog16.c): a place from the
// template's origin `org`, a size on its own.
int w16_du_x(int org, int x, int bx);
int w16_du_y(int org, int y, int by);
int w16_du_w(int cx, int bx);
int w16_du_h(int cy, int by);
int w16_dlg_caption(const struct w16_wnd *d);
// IsDialogMessage: the dialog keyboard for a message meant for dialog d
// or one of its controls -- 1 when handled (or dispatched) there.
int w16_is_dialog_message(struct w16 *w, struct w16_wnd *d, const struct w16_msg *m);
// A dialog from a DIALOG template (resource bytes), its controls made:
// modeless, or run to its end (the result) when `modal`.
struct w16_wnd *w16_dlg_create(struct w16 *w, const uint8_t *tmpl, uint32_t n, uint16_t owner,
                               uint16_t inst, uint32_t dlgproc, w16_dlgproc hostdlg, uint32_t param,
                               char *why, size_t why_cap);
int16_t w16_dlg_run(struct w16 *w, struct w16_wnd *d);
// A dialog built in code (MessageBox, the common dialogs) and its
// controls by class name; then w16_dlg_run() it, or leave it modeless.
struct w16_wnd *w16_dlg_new(struct w16 *w, uint16_t owner, uint16_t inst, uint32_t style, int cw, int ch,
                            const char *caption, uint32_t dlgproc, w16_dlgproc hostdlg);
struct w16_wnd *w16_dlg_add(struct w16 *w, struct w16_wnd *d, const char *cls, uint16_t id, uint32_t style,
                            int x, int y, int cw, int ch, const char *text);
// Shown, WM_INITDIALOG(param) sent, the focus put; then run it when modal.
// `dlgdata` is the caller's to free.
void w16_dlg_show(struct w16 *w, struct w16_wnd *d, int modal, uint32_t param);
// A dialog's toywm window closed and gone (DestroyWindow's part of it).
void w16_dlg_window_gone(struct w16 *w, struct w16_wnd *d);
extern const struct w16_api g_dialog_api[], g_shell_api[], g_sound_api[], g_menu_api[];
extern const unsigned g_menu_api_n;
// menubar16.c: the task's half of opening a bar title, and the main
// thread's -- the bar drawn, a title found, its popup built. Main-thread
// calls hold `lock`.
void w16_menu_opening(struct w16 *w, int pos, int first);
uint16_t w16_load_menu(struct w16 *w, const char *name, uint16_t id);   // a MENU resource's HMENU
void w16_set_menu(struct w16 *w, uint16_t hmenu);   // SetMenu on the main window
struct w16_menu;
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
// The DLU -> pixel base units of the dialog font: x per 4, y per 8.
void w16_dlg_units(int *bx, int *by);
// Main thread: the key and mouse messages for a top-level window's
// client area, routed to the child under the pointer or with the focus.
// (input16.c) `key` is toywm's (KEY_*, or the character); (x, y) is in
// top's client coordinates.
void w16_route_key(struct w16 *w, struct w16_wnd *top, int key, unsigned mods, int up);
// The pointer's shape there (WIN_CURSOR_*): the class cursor of the window under it,
// or -1 to leave it as it is while a window holds the capture (a scroll bar's drag).
int w16_route_mouse(struct w16 *w, struct w16_wnd *top, uint16_t msg, uint16_t wp, int x, int y);
// A cursor handle (LoadCursor's) as a WIN_CURSOR_* shape.
int w16_cursor_shape(uint16_t hcursor);
int w16_vk_of(int key);                           // a key's VK_* code, 0 for none
void w16_dlgwin_redraw_all(struct w16 *w);       // every open dialog shows its buffer again

// --- module16.c: binding images, Windows' modules, a program's own DLLs ----------

// A loaded NE image: the program's (in struct w16) or a DLL's.
struct w16_image {
    const struct ne_module *ne;
    uint16_t *seg_sel;                            // NE segment n -> its selector
    const char *path;
};
// Its segments given selectors (the DGROUP `dgroup_extra` bytes larger:
// heap and stack), then its relocations bound. -1 with `why` set.
int w16_load_segments(struct w16 *w, const struct w16_image *im, uint32_t dgroup_extra, char *why, size_t cap);
int w16_bind_image(struct w16 *w, const struct w16_image *im, char *why, size_t cap);
int w16_is_builtin(const char *module);           // one of Windows' own, answered in C
uint16_t w16_thunk_sel(struct w16 *w, const char *module);   // made on first use; 0 out of selectors
const char *w16_thunk_module(struct w16 *w, uint16_t sel);   // NULL: not a thunk
int w16_kernel_equate(const char *module, unsigned ordinal, uint16_t *value);
// A DLL loaded (once) by module name or path; NULL with *err 2 (not found)
// or 11 (not a library w16 can load).
struct w16_dll *w16_dll_load(struct w16 *w, const char *spec, int *err);
struct w16_dll *w16_dll_of(struct w16 *w, uint16_t hinst);   // NULL: not a DLL's
int w16_dll_start_pending(struct w16 *w);         // the task's first act: their LibEntries
// A KERNEL call, made on the local heap DS names (a DLL's own, when its code calls).
int w16_kernel_heap_call(struct w16 *w, unsigned ordinal, struct w16_args *a,
                         int (*call)(struct w16 *, unsigned, struct w16_args *));
extern const struct w16_api g_module_api[], g_ddeml_api[], g_mmsystem_api[];
extern const unsigned g_module_api_n, g_ddeml_api_n, g_mmsystem_api_n;

// --- mem16.c / res16.c --------------------------------------------------------

uint16_t w16_global_alloc(struct w16 *w, uint32_t size, const void *init);   // a selector, or 0
void w16_global_free(struct w16 *w, uint16_t h);
void w16_local_init(struct w16 *w);
// The local heap for w16's own use -- an EDIT keeping its text there, as
// Windows' did: a handle (the program may be handed it), and its block.
uint16_t w16_local_alloc(struct w16 *w, uint16_t flags, uint16_t size);
uint16_t w16_local_realloc(struct w16 *w, uint16_t h, uint16_t size, uint16_t flags);
void w16_local_free(struct w16 *w, uint16_t h);
uint8_t *w16_local_ptr(struct w16 *w, uint16_t h, unsigned *size);   // NULL: no such block
int w16_kernel_mem(struct w16 *w, unsigned ordinal, struct w16_args *a);   // -2: not its own
int w16_kernel_res(struct w16 *w, unsigned ordinal, struct w16_args *a);
int w16_user_loadstring(struct w16 *w, struct w16_args *a);
int w16_user_misc(struct w16 *w, unsigned ordinal, struct w16_args *a);   // usermisc16.c
void w16_timers_fire(struct w16 *w);              // the main thread's tick
// A timer of w16's own on a window (an EDIT's caret blink): WM_TIMER
// with `id` every `ms` till killed; setting it again starts it over.
void w16_set_timer(struct w16 *w, uint16_t hwnd, uint16_t id, uint32_t ms);
void w16_kill_timer(struct w16 *w, uint16_t hwnd, uint16_t id);
void w16_cursor_screen(struct w16 *w, int16_t *x, int16_t *y);   // MSG.pt, GetCursorPos
// What x's client point is offset by in SCREEN coordinates -- which are
// its top-level window's own (usermisc16.c).
void w16_screen_offset(struct w16 *w, struct w16_wnd *x, int *dx, int *dy);
uint64_t w16_now_ms(void);
int w16_load_string(struct w16 *w, uint16_t id, char *out, unsigned cap);
uint16_t w16_res_load(struct w16 *w, unsigned index);
int w16_res_find(struct w16 *w, uint16_t type, const char *type_name, uint16_t id,
                 const char *name, struct ne_resource *out);

// --- file16.c -----------------------------------------------------------------

// A DOS path (C: the Win16 folder, Z: the toy-os tree, else relative to
// the current folder) as a host path, matched without case; the last
// component may be missing when `creating`. 0, or a DOS error code;
// `full` (optional) gets the full DOS path.
int w16_dos_path(const char *dos, char *host, size_t cap, int creating, char *full, size_t full_cap);
int w16_host_to_dos(const char *host, char *out, size_t cap);
void w16_dos_start(const char *exe);              // the current folder: the program's own
const char *w16_dos_cwd(void);                    // the current folder, a full DOS path
int w16_dos_chdir(const char *dos);               // 0, or a DOS error
extern const struct w16_api g_commdlg_api[];
extern const unsigned g_commdlg_api_n;
int w16_dos_file(struct w16 *w, struct cpu16 *c);  // INT 21h's file calls; -1 not one
extern const struct w16_api g_file_api[], g_keyboard_api[];
extern const unsigned g_file_api_n, g_keyboard_api_n;

// --- ini16.c ------------------------------------------------------------------

extern char g_w16_drive_c[128];                   // drive C:'s host folder
const char *w16_drive_root(char d);              // a drive's host folder, "" for Z:, 0 for none
struct w16_prefs;
extern struct w16_prefs g_w16_prefs;              // how this program runs (w16prefs.h)
int w16_icon(const struct ne_module *m, uint32_t *px, int *w, int *h);   // icon16.c
// Icon group `id` (or `name`, when not NULL) of a module; that group's picture.
int w16_icon_named(const struct ne_module *m, uint16_t id, const char *name, uint32_t *px, int *w, int *h);
int w16_icon_group(const struct ne_module *m, const struct ne_resource *grp, uint32_t *px, int *w, int *h);
// LoadIcon's handle for a module's icon group (0: it has none such), and
// the picture a handle names (32x32 at most), -1 for one that names none.
uint16_t w16_icon_handle(const struct ne_module *m, uint16_t id, const char *name);
int w16_icon_of_handle(uint16_t h, uint32_t *px, int *w, int *hh);
// ARGB pixels (alpha 0 clear) drawn at (x, y) of a DC: DrawIcon's work.
void w16_gdi_draw_argb(struct w16 *w, uint16_t hdc, int x, int y, const uint32_t *px, int pw, int ph);
int w16_ini_get(const char *file, const char *sec, const char *key, char *out, size_t cap);
size_t w16_ini_names(const char *file, const char *sec, char *out, size_t cap);
int w16_ini_set(const char *file, const char *sec, const char *key, const char *val);

// --- blit16.c -----------------------------------------------------------------

// What a DC draws into: a buffer, `w` pixels a row -- a bitmap's, or a
// top-level window's client area, in which a window DC's (0, 0) is at
// (ox, oy) and only [x0, x1) x [y0, y1) is its window's to draw on.
struct gdi_target {
    uint32_t *px;
    int w, h, mono;
    int ox, oy;
    int x0, y0, x1, y1;
};
void gdi_target_whole(struct gdi_target *t);      // no offset, the whole buffer drawable

struct gdi_blit {
    const struct gdi_target *dst, *src;  // src NULL for a pattern-only ROP
    int x, y, w, h, sx, sy;
    uint32_t rop;                        // a ROP3, its code in bits 16-23
    uint32_t brush;                      // the pattern, a pixel...
    const uint32_t *pat;                 // ...or 8x8 of them (a hatched or bitmap brush), NULL none
    int pat_x, pat_y;                    // where the pattern's (0, 0) falls, buffer coordinates
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
// commdlg16.c: a 16x16 ARGB picture from the user's own COMMDLG.DLL, or NULL
// when drive C: has none -- the caller then draws its own.
enum { W16_ICON_OPEN, W16_ICON_CURRENT, W16_ICON_SHUT, W16_ICON_FLOPPY, W16_ICON_DISK, W16_ICON_NET,
       W16_ICON_CD, W16_ICON_RAM, W16_ICON_COUNT };
const uint32_t *w16_commdlg_icon(int which);
uint32_t gdi_dib_px(const struct gdi_dib *d, const uint8_t *row, int x);
struct ugfx_font;
const struct ugfx_font *w16_system_font(void);   // under g_w16_text_lock
const struct ugfx_font *w16_fixed_font(void);    // Fixedsys's stand-in, likewise
const struct ugfx_font *w16_font_of(uint16_t hfont);   // the face an HFONT draws in
// A dialog template's FONT (3.11's "Helv" 8, MS Sans Serif, bold as 3.x
// drew it): units of 7 by 13 rather than the System font's 8 by 16. The handle
// controls are given by WM_SETFONT; GetObject knows nothing of it.
#define W16_FONT_DIALOG 0x5020
const struct ugfx_font *w16_dialog_font(void);   // under g_w16_text_lock
int w16_font_line_h(uint16_t hfont);              // a row of text in it, in pixels

// font16.c: what an HFONT draws in and what GetTextMetrics tells of it --
// 3.11's own raster font when `fonts` allows and C:\WINDOWS\SYSTEM has it,
// else a stand-in. Under g_w16_text_lock; the pointers stay valid.
struct w16_fontinfo {
    const struct ugfx_font *uf;
    int height, ascent, internal_leading, external_leading;
    int ave, maxw, weight, italic, pitch_family, charset;
    int first, last, def;
};
const struct w16_fontinfo *w16_font_info(uint16_t hfont);
int w16_font_ascent(const struct ugfx_font *uf);   // pixels above its baseline
// GDI's tmOverhang: 1 for a raster font it emboldened itself (3.11's MS Sans
// Serif drawn bold), which an underline and a label's cleared box count.
int w16_font_overhang(const struct ugfx_font *uf);
extern int g_w16_fonts;                            // W16_FONTS_*: the program's setting
struct w16_logfont {                               // a CreateFont's request
    int16_t height, width, weight;
    uint8_t italic, underline, strikeout, charset, pitch_family;
    char face[32];
};
const struct w16_logfont *w16_gdi_logfont(struct w16 *w, uint16_t hfont);   // NULL: not a created font
#include <stdio.h>
void w16_fonts_list(FILE *out);                    // `w16 --fonts`
// The base units x is laid out in: its dialog's, else the System font's.
void w16_units_of(struct w16 *w, const struct w16_wnd *x, int *bx, int *by);
int w16_font_is_fixed(uint16_t hfont);

// --- frame16.c (main thread) ---------------------------------------------------

#define W16_FRAME      4                          // the sizing border
#define W16_CAPTION    19   // the caption, its bottom line included: 18 rows and a line, as 3.11 at VGA

struct ugfx_surface;
void w16_frame_draw(struct ugfx_surface *s, const struct w16_wnd *win, int active, int pressed);
// What a point of the window is: the client, the caption, a button, or a
// sizing edge (WIN_EDGE_* bits in *edges).
enum w16_hit { HIT_CLIENT, HIT_CAPTION, HIT_SYSMENU, HIT_MIN, HIT_MAX, HIT_EDGE, HIT_MENU, HIT_NONE };
#define W16_MENU_H     19                         // the menu bar, its bottom line included: 18 rows and a line, as 3.11
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
// A window's pixels kept at the scale between frames: each frame re-scales
// only what changed since the last (a dragged card, not the whole window)
// and copies the rest. Zeroed to start; w16_scaler_free() to end.
struct tap;
struct w16_scaler {
    uint32_t *prev, *out;          // the last 1x frame; the scaled one
    struct tap *tx, *ty;           // each screen column's and row's source pixels
    int w, h, W, H, q, method;
};
void w16_scaler_show(struct w16_scaler *sc, struct ugfx_surface *dst, int dx, int dy, const uint32_t *cur,
                     int w, int h);
void w16_scaler_free(struct w16_scaler *sc);

// The control-menu box drawn at (x, y): grey, its bar outlined; navy while
// its menu is open. The main window's frame and a dialog's share it.
void w16_draw_sysbox(struct ugfx_surface *s, int x, int y, int w, int h, int open);
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
