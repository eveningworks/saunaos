#ifndef UITEST_H
#define UITEST_H

// uitest -- the HOST widget harness (docs/gui-unittest-design.md, stage 1).
//
// A test builds the same `struct uui_item` array (and layout) an app
// declares, draws it into an in-memory surface and drives it by widget
// NAME through the real router, in the order uapp's window does
// (uapp_window.c's top_press/top_key): the router first, then the focus
// ring. The toolkit is the real one, compiled with the host gcc; what is
// stubbed is the platform underneath it -- the sys_* calls, in
// uitest_sys.c -- which is Qt's offscreen QPA plugin's shape.
//
// The text is Liberation Sans at the default session size (14 px),
// rasterized by the toolkit's own ttf.c, so measurements and glyph
// pixels are the default desktop's and no host font is involved.
//
// Reporting is base/utest.h's: utest_check() and friends, one epilogue.
// Run by tools/hostchecks/uitest_hostcheck.py, which builds every case
// in tools/hostchecks/uitest/.
#include <stdint.h>
#include "base/utest.h"
#include "ui/ugfx.h"
#include "ui/uui_focus.h"
#include "ui/uui_layout.h"
#include "ui/uui_route.h"
#include "ui/uui_widget.h"

#define UITEST_FONT_PX 14

// One window: its surface, its router and, optionally, a focus ring.
struct uitest_win {
    struct ugfx_surface surface;
    uint32_t *pixels;            // malloc'd, w * h
    struct uui_item *items;
    int count;
    struct uui_layout *layout;   // or NULL: items placed by hand
    struct uui_router router;
    struct uui_focus *focus;     // or NULL
    int mouse_x, mouse_y;        // where the pointer is (the wheel goes there)
    int last_id;                 // what the last input reported to the app
    int last_changed;            // ...and whether a widget said it changed
};

// THE COUNTERS ARE THE TEST'S. base/utest.h keeps its state in statics,
// one copy per translation unit, so the harness reports a failure of its
// own (a widget name that is not there) through the test file's copy:
// uitest_begin() is a macro, expanded in the test, that hands it over.
extern void (*uitest_report)(int ok, const char *what);
extern const char *uitest_case;   // the name given uitest_begin(): uitest_keep()'s file names
static inline void uitest_report_here(int ok, const char *what) { utest_check(ok, what); }

// The font and the theme, then utest_begin(). Exits the test (status 99)
// when the font cannot be rasterized: nothing after it would mean anything.
void uitest_setup(void);
#define uitest_begin(name, title) \
    (uitest_report = uitest_report_here, uitest_case = (name), uitest_setup(), utest_begin((name), (title), 0))
#define uitest_end() utest_end()

// A window of w x h: the layout (if any) run over the whole of it, the
// router over `items`, and a first frame painted.
void uitest_window(struct uitest_win *win, struct uui_item *items, int count,
                   struct uui_layout *layout, int w, int h);
void uitest_window_free(struct uitest_win *win);
void uitest_set_focus(struct uitest_win *win, struct uui_focus *focus);
// Re-run the layout (after the app changed something it measures from).
void uitest_relayout(struct uitest_win *win);

// The item NAMED `name`, searched through nested layouts (ops->children),
// or NULL -- and a FAIL is recorded, since a test naming a widget that
// is not there tests nothing.
struct uui_item *uitest_find(struct uitest_win *win, const char *name);
// Its rect, from ops->bounds. 0 (and a FAIL) when it has none.
int uitest_rect(struct uitest_win *win, const char *name, int *x, int *y, int *w, int *h);

// --- input, in window coordinates ------------------------------------
// The clock moves a second before every press, so two clicks are never
// a double click unless uitest_double_click() asks for one.
int  uitest_press_at(struct uitest_win *win, int x, int y);
int  uitest_motion_at(struct uitest_win *win, int x, int y, unsigned buttons);
int  uitest_release_at(struct uitest_win *win, int x, int y);
int  uitest_click_at(struct uitest_win *win, int x, int y);
// A press, `steps` held motions and a release, from (x0,y0) to (x1,y1).
int  uitest_drag_at(struct uitest_win *win, int x0, int y0, int x1, int y1, int steps);
// The pointer moved there, then the wheel turned: NEGATIVE notches scroll
// DOWN (the event's own sign; uui_grid's `scroll -= notches * ...`).
int  uitest_wheel_at(struct uitest_win *win, int x, int y, int notches);

// --- input by widget name: (dx, dy) from its top-left; a negative value
// counts from the right or bottom edge (-1 = the last pixel) -----------
int  uitest_click(struct uitest_win *win, const char *name);   // its centre
int  uitest_click_in(struct uitest_win *win, const char *name, int dx, int dy);
int  uitest_double_click_in(struct uitest_win *win, const char *name, int dx, int dy);
int  uitest_wheel(struct uitest_win *win, const char *name, int notches);
int  uitest_hover(struct uitest_win *win, const char *name, int dx, int dy);

// A key, as uapp delivers one: an open popup first, then the focus ring.
// Returns 1 when either took it.
int  uitest_key(struct uitest_win *win, int key, unsigned mods);

// --- output -------------------------------------------------------------
void     uitest_frame(struct uitest_win *win);   // paint, as uapp's top_paint
// Frames UUI_ANIM_FRAME_MS apart for as long as something asks for one
// (ui/uui_anim.h) -- a gliding scroll, a widening bar -- as uapp's loop
// runs them; capped at ten seconds of animation, which is a FAIL. One
// frame after a key that glides is the glide's FIRST frame.
void     uitest_settle(struct uitest_win *win);
uint32_t uitest_pixel_at(struct uitest_win *win, int x, int y);
uint32_t uitest_pixel(struct uitest_win *win, const char *name, int dx, int dy);
// Writes the frame as a PPM to $UITEST_KEEP/<test>-<tag>.ppm when that
// variable is set; does nothing otherwise. For looking, never asserting.
void     uitest_keep(struct uitest_win *win, const char *tag);

// --- the platform ---------------------------------------------------------
void uitest_advance_ms(int ms);       // the monotonic clock sys_monotonic_ns reads
uint64_t uitest_now_ns(void);
// A setting as sys_setting() and usetting_get() answer it (NULL value
// forgets it). Settings not set here do not exist.
void uitest_setting(const char *name, const char *value);
// A KERNEL-REGISTERED setting: set as above, and also LISTED by the
// registry's COUNT and INFO (ns = the name's first segment), with its
// SETTING_ABI_TYPE_* -- an INT's SET refused outside min..max, as the
// kernel's setting_set() refuses it.
void uitest_kernel_setting(const char *name, const char *value, int type, int min, int max);
// A scalar kernel fact, "<provider>.<field>", as sys_query_field_get()
// and the QUERY_PROVIDERS walk answer it.
void uitest_fact(const char *name, unsigned long long value);
// Writes `text` to the guest file `path`, making its directories -- a
// declaration in /etc/settings.d, a person's ~/.config file. /etc and
// /home are this case's own (uitest_host.c). 1 on success.
int uitest_file(const char *path, const char *text);
// The guest file's text into `out` ("" when absent). 1 when it was read.
int uitest_read_file(const char *path, char *out, size_t cap);
// What the process has logged (base/ulog.h): a mark now, then the LAST
// line after it containing `needle`, copied to `out`. 0 when none.
size_t uitest_log_mark(void);
int    uitest_log_find(size_t mark, const char *needle, char *out, size_t cap);

// --- checks -----------------------------------------------------------------
static inline void uitest_check_int(const char *what, long got, long want) {
    char d[80];
    snprintf(d, sizeof d, "got %ld, want %ld", got, want);
    utest_check_detail(got == want, what, d);
}

#endif
