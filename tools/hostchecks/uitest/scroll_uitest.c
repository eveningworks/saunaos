// Scrolling, as a person does it, in every scrolling widget an app shows
// a long list in: the wheel goes the right way, the thumb follows the
// view, a click in the track pages, and a drag of the thumb moves the view
// WITH it (userland/CLAUDE.md, rule 10). Moved down from
// tools/tests/gui/scroll_test.py, which keeps ONE view end to end (the
// Retro Launcher's grid, where the mirrored thumb was first seen); the
// rest is each widget's own logic and needs no guest.
//
// THE THUMB IS READ FROM PIXELS at the strip's edge, never from the
// widget's state: the bug this exists for was a painter handed a
// top-based position where it counts from the BOTTOM, and every number
// in the widget was right while the picture was mirrored. Each view has
// many more rows than fit -- a view with room for all its rows has no bar.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uitest.h"
#include "keyboard.h"
#include "base/ulogset.h"
#include "base/uemojidata.h"
#include "ui/uui_appcard.h"
#include "ui/uui_emojigrid.h"
#include "ui/uui_grid.h"
#include "ui/uui_hexview.h"
#include "ui/uui_loglist.h"
#include "ui/uui_table.h"
#include "ui/uui_label.h"
#include "ui/uui_wizard.h"

#define W 460
#define H 320

// (top, bottom) of the thumb in the strip at the rect's right edge, rows
// from wy; 0 when there is none. The column's commonest colour is the
// ground; the thumb is the longest run of anything clearly different --
// scroll_test.py's rule, so the two agree on what "the thumb" is.
static int thumb(struct uitest_win *win, int wx, int wy, int ww, int wh, int *t0, int *t1) {
    int x = wx + ww - 2;
    uint32_t col[1024] = { 0 };
    int n = wh < 1024 ? wh : 1024;
    if (n <= 0) return 0;
    for (int i = 0; i < n; i++) col[i] = uitest_pixel_at(win, x, wy + i);
    uint32_t ground = col[0];
    int best_count = 0;
    for (int i = 0; i < n; i++) {
        int c = 0;
        for (int j = 0; j < n; j++) c += col[j] == col[i];
        if (c > best_count) { best_count = c; ground = col[i]; }
    }
    int bs = -1, be = -1, start = -1;
    for (int i = 0; i <= n; i++) {
        int far = 0;
        if (i < n) {
            int d = 0;
            for (int s = 0; s <= 16; s += 8)
                d += abs((int)((col[i] >> s) & 0xFF) - (int)((ground >> s) & 0xFF));
            far = d > 60;
        }
        if (far && start < 0) start = i;
        else if (!far && start >= 0) {
            if (bs < 0 || i - start > be - bs) { bs = start; be = i; }
            start = -1;
        }
    }
    if (bs < 0 || be - bs < 12) return 0;
    *t0 = bs;
    *t1 = be;
    return 1;
}

static double where(int t0, int t1, int wh) { return ((t0 + t1) / 2.0) / wh; }

struct view {
    const char *label;
    const char *name;     // the item's name in the window
    int keys;             // Home/End reach it (else: a long wheel run)
    int (*key)(void *w, int key, unsigned mods);   // the APP routes keys to it, not the ring
    void *widget;
    unsigned key_mods;    // Ctrl for the Hex Viewer, whose Home/End are the row's
    int inset;            // rows above the track: a header
    int click_dx, click_dy;   // inside the view, off the bar: focuses it
};

// Read the thumb with the pointer PARKED off the bar (a hovered bar is
// drawn wide) and every animation run out.
static int read(struct uitest_win *win, const struct view *v, int *t0, int *t1) {
    int x, y, w, h;
    uitest_rect(win, v->name, &x, &y, &w, &h);
    uitest_motion_at(win, x + v->click_dx, y + v->click_dy, 0);
    uitest_settle(win);
    return thumb(win, x, y + v->inset, w, h - v->inset, t0, t1);
}

static void go(struct uitest_win *win, const struct view *v, int end) {
    int x, y, w, h;
    uitest_rect(win, v->name, &x, &y, &w, &h);
    if (v->keys) {
        if (v->key) v->key(v->widget, end ? KEY_END : KEY_HOME, v->key_mods);
        else uitest_key(win, end ? KEY_END : KEY_HOME, v->key_mods);
        return;
    }
    for (int i = 0; i < 15; i++) uitest_wheel_at(win, x + v->click_dx, y + v->click_dy, end ? -10 : 10);
}

static void check_view(struct uitest_win *win, const struct view *v) {
    char what[160];
    int x, y, w, h;
    if (!uitest_rect(win, v->name, &x, &y, &w, &h)) return;
    int wh = h - v->inset;
    uitest_click_at(win, x + v->click_dx, y + v->click_dy);

    int a0, a1, b0, b1;
    if (v->keys) go(win, v, 0);
    int have = read(win, v, &a0, &a1);
    snprintf(what, sizeof what, "%s: there is a scrollbar thumb to read", v->label);
    utest_check(have, what);
    if (!have) return;
    snprintf(what, sizeof what, "%s: at the top the thumb is at the top (%d-%d of %d)", v->label, a0, a1, wh);
    utest_check(where(a0, a1, wh) < 0.3, what);
    go(win, v, 1);
    have = read(win, v, &b0, &b1);
    snprintf(what, sizeof what, "%s: at the end the thumb is at the foot (%d-%d of %d)", v->label, b0, b1, wh);
    utest_check(have && where(b0, b1, wh) > 0.7, what);

    // The wheel, from the top: down moves the thumb down, up brings it back.
    go(win, v, 0);
    read(win, v, &a0, &a1);
    for (int i = 0; i < 4; i++) uitest_wheel_at(win, x + v->click_dx, y + v->click_dy, -3);
    have = read(win, v, &b0, &b1);
    snprintf(what, sizeof what, "%s: the wheel down moves the thumb down (%d -> %d)", v->label, a0, b0);
    utest_check(have && where(b0, b1, wh) > where(a0, a1, wh) + 0.02, what);
    int c0, c1;
    for (int i = 0; i < 2; i++) uitest_wheel_at(win, x + v->click_dx, y + v->click_dy, 3);
    have = read(win, v, &c0, &c1);
    snprintf(what, sizeof what, "%s: the wheel up moves it back (%d -> %d)", v->label, b0, c0);
    utest_check(have && where(c0, c1, wh) < where(b0, b1, wh) - 0.01, what);

    // The track: below the thumb pages down, above it pages up. In pixels:
    // a long list's page is a sliver of the track.
    int bx = x + w - 2, ty = y + v->inset;
    go(win, v, 0);
    read(win, v, &a0, &a1);
    uitest_click_at(win, bx, ty + (wh - 6 < a1 + 30 ? wh - 6 : a1 + 30));
    have = read(win, v, &b0, &b1);
    snprintf(what, sizeof what, "%s: a click in the track below the thumb moves it down (%d -> %d)", v->label, a0, b0);
    utest_check(have && b0 > a0 + 2, what);
    if (have && b0 - a0 > 6) {
        uitest_click_at(win, bx, ty + (a0 + b0) / 2);
        have = read(win, v, &c0, &c1);
        snprintf(what, sizeof what, "%s: a click in the track above the thumb moves it up (%d -> %d)", v->label, b0, c0);
        utest_check(have && c0 < b0 - 2, what);
    }

    // The thumb, dragged from the top to about the middle: it goes with the pointer.
    go(win, v, 0);
    read(win, v, &a0, &a1);
    int y0 = ty + (a0 + a1) / 2;
    uitest_drag_at(win, bx, y0, bx, y0 + wh / 2, 6);
    have = read(win, v, &b0, &b1);
    snprintf(what, sizeof what, "%s: dragging the thumb down moves it down, with the pointer (%d -> %d of %d)",
             v->label, a0, b0, wh);
    utest_check(have && where(b0, b1, wh) > 0.3 && where(b0, b1, wh) < 0.9, what);
}

// One widget in a window, placed by hand, with a focus ring of one.
static void open_one(struct uitest_win *win, struct uui_item *it, struct uui_focusable *ff,
                     struct uui_focus *focus) {
    it->ops->set_geometry(it->widget, 10, 10, W - 20, H - 20);
    uitest_window(win, it, 1, NULL, W, H);
    ff->widget = it->widget;
    ff->ops = it->ops;
    uui_focus_init(focus, ff, 1);
    uitest_set_focus(win, focus);
}

// --- the fixtures -------------------------------------------------------------------

// The Retro Launcher's grid and its Details table: sixty programs.
#define PROGRAMS 60
static void grid_cell(struct ugfx_surface *s, void *ctx, int index, int x, int y, int w, int h, int state) {
    (void)ctx; (void)state;
    char label[16];
    snprintf(label, sizeof label, "p%02d.exe", index);
    ugfx_draw_string_clipped(s, x + 4, y + h / 2, w - 8, label, UTHEME_TEXT, UTHEME_PANEL_BG);
}

static const struct uui_table_column PROGRAM_COLS[] = {
    { "Name", 0, UUI_TALIGN_LEFT },
    { "Size", 8, UUI_TALIGN_RIGHT },
};
static void program_cell(void *ctx, int row, int col, char *out, int cap) {
    (void)ctx;
    if (col == 0) snprintf(out, (size_t)cap, "p%02d.exe", row);
    else snprintf(out, (size_t)cap, "%d KB", 10 + row);
}

// The Log Viewer: six hundred lines -- a few wheel notches must still
// move the thumb a visible distance.
#define LOG_LINES 600
static struct ulog_line g_lines[LOG_LINES];
static int g_view[LOG_LINES];

// The Hex Viewer: eight KiB, five hundred rows.
#define HEX_BYTES 8192
static size_t hex_read(void *ctx, uint64_t off, void *buf, size_t n) {
    (void)ctx;
    for (size_t i = 0; i < n && off + i < HEX_BYTES; i++) ((unsigned char *)buf)[i] = (unsigned char)((off + i) * 7);
    return off >= HEX_BYTES ? 0 : (off + n > HEX_BYTES ? HEX_BYTES - off : n);
}

// The Hex Viewer's keys are its app's: hexview.c hands them to the grid.
static int hex_key(void *w, int key, unsigned mods) { return uui_hexview_key(w, key, mods); }

// Software: thirty cards, as many more than fit as scroll_test's fakes.
#define CARDS 30
static struct uui_appcard g_cards[CARDS];
static char g_card_names[CARDS][32];

int main(void) {
    uitest_begin("scroll", "scrolling: the wheel, the thumb, the track and the drag");
    struct uitest_win win;
    struct uui_focusable ff;
    struct uui_focus focus;

    // --- the Retro Launcher's program grid (ui/uui_grid) ---------------------
    static struct uui_grid grid;
    uui_grid_init(&grid, ugfx_char_w() * 12, ugfx_char_h() * 5);
    uui_grid_set(&grid, PROGRAMS, grid_cell, NULL);
    struct uui_item git = { .ops = &uui_grid_ops, .widget = &grid, .id = 1, .name = "programs" };
    open_one(&win, &git, &ff, &focus);
    check_view(&win, &(struct view){ "Retro Launcher grid", "programs", 0, 0, 0, 0, 0, 60, 40 });
    uitest_window_free(&win);

    // --- its Details view (ui/uui_table) ------------------------------------------
    static struct uui_table table;
    uui_table_init(&table, 0, 0, 0, 0, PROGRAM_COLS, 2, program_cell, NULL);
    uui_table_set_rows(&table, PROGRAMS);
    struct uui_item tit = { .ops = &uui_table_ops, .widget = &table, .id = 1, .name = "programs-table" };
    open_one(&win, &tit, &ff, &focus);
    check_view(&win, &(struct view){ "Retro Launcher details", "programs-table", 1, 0, 0, 0,
                                     uui_table_header_h(&table), 60, 60 });
    uitest_window_free(&win);

    // --- the Log Viewer's list (ui/uui_loglist) -----------------------------------
    static struct ulogset set;
    ulogset_init(&set, g_lines, g_view, LOG_LINES);
    for (int i = 0; i < LOG_LINES; i++) {
        char raw[96];
        int n = snprintf(raw, sizeof raw, "[%5d.%02d] <6>usb: line %d of the fixture", i / 10, i % 100, i);
        ulogset_add(&set, "kernel", raw, n);
    }
    ulogset_finish(&set);
    struct ulog_filter all;
    memset(&all, 0, sizeof all);
    all.only = -1;
    ulogset_filter(&set, &all);
    static struct uui_loglist log;
    uui_loglist_init(&log, &set);
    uui_loglist_set_follow(&log, 0);
    struct uui_item lit = { .ops = &uui_loglist_ops, .widget = &log, .id = 1, .name = "log" };
    open_one(&win, &lit, &ff, &focus);
    check_view(&win, &(struct view){ "Log Viewer list", "log", 1, 0, 0, 0, uui_loglist_timeline_h(&log), 120, 60 });
    uitest_window_free(&win);

    // --- the Hex Viewer's byte grid (ui/uui_hexview); Ctrl+Home/End are the file's ends ---
    static struct uui_hexview hex;
    uui_hexview_init(&hex);
    uui_hexview_set_source(&hex, HEX_BYTES, hex_read, NULL);
    hex.font = ugfx_font_mono(UGFX_FONT_REGULAR);
    struct uui_item hit = { .ops = &uui_hexview_ops, .widget = &hex, .id = 1, .name = "grid" };
    open_one(&win, &hit, &ff, &focus);
    int bx, by, bw, bh;
    uui_hexview_byte_rect(&hex, 0, 0, &bx, &by, &bw, &bh);
    check_view(&win, &(struct view){ "Hex Viewer grid", "grid", 1, hex_key, &hex, KEY_MOD_CTRL,
                                     by - 10, 120, 60 });
    uitest_window_free(&win);

    // --- Character Map's Emoji grid (ui/uui_emojigrid): every emoji with an 'e' ---------
    const struct uemojidata *d = uemojidata();
    utest_check(d && d->count > 0, "the emoji list loads from /usr/share/unicode");
    if (d && d->count > 0) {
        static int items[8192];
        int n = uemojidata_search(d, "e", items, d->count < 8192 ? d->count : 8192);
        static struct uui_emojigrid eg;
        uui_emojigrid_init(&eg, ugfx_char_h() * 5 / 2, ugfx_char_h() * 3 / 2);
        uui_emojigrid_set(&eg, items, n);
        struct uui_item eit = { .ops = &uui_emojigrid_ops, .widget = &eg, .id = 1, .name = "egrid" };
        open_one(&win, &eit, &ff, &focus);
        check_view(&win, &(struct view){ "Character Map emoji grid", "egrid", 1, 0, 0, 0, 0, 30, 30 });
        uitest_window_free(&win);
    }

    // --- Software's cards and list (ui/uui_appcard) -------------------------------------
    for (int i = 0; i < CARDS; i++) {
        snprintf(g_card_names[i], sizeof g_card_names[i], "Fake component %02d", i);
        g_cards[i] = (struct uui_appcard){
            .name = g_card_names[i], .kind = "Demos", .summary = "A definition with nothing behind it, to scroll past",
            .badge = "MIT", .size = "120 KB", .status = "Not installed", .button = "Install",
            .glyph = "Fc", .tile = 0x3b82f6 };
    }
    static struct uui_appcards cards;
    uui_appcards_init(&cards, UUI_APPCARD_CARDS);
    uui_appcards_set(&cards, g_cards, CARDS);
    struct uui_item cit = { .ops = &uui_appcards_ops, .widget = &cards, .id = 1, .name = "cards" };
    open_one(&win, &cit, &ff, &focus);
    check_view(&win, &(struct view){ "Software cards", "cards", 1, 0, 0, 0, 0, 4, 4 });
    uui_appcards_set_layout(&cards, UUI_APPCARD_ROWS);
    uitest_relayout(&win);
    cit.ops->set_geometry(cit.widget, 10, 10, W - 20, H - 20);
    int ix, iy, iw, ih;
    uui_appcards_item_rect(&cards, 0, &ix, &iy, &iw, &ih);
    check_view(&win, &(struct view){ "Software list", "cards", 1, 0, 0, 0, iy - 10, 40, iy - 10 + 4 });
    uitest_window_free(&win);

    // --- the setup wizard's page (ui/uui_wizard): taller than the window ----------
    // The page scrolls under a step header that does not (First-boot
    // setup's Appearance page ran under Back/Next at 1024x768).
    #define WZ_LINES 40
    static struct uui_label wz_lines[WZ_LINES];
    static char wz_text[WZ_LINES][32];
    static struct uui_item wz_items[WZ_LINES];
    for (int i = 0; i < WZ_LINES; i++) {
        snprintf(wz_text[i], sizeof wz_text[i], "Line %d of the page", i);
        uui_label_init(&wz_lines[i], wz_text[i]);
        wz_items[i] = (struct uui_item){ .ops = &uui_label_ops, .widget = &wz_lines[i], .flags = UUI_FILL_W };
    }
    static struct uui_layout wz_page;
    wz_page = (struct uui_layout){ .dir = UUI_COLUMN, .items = wz_items, .count = WZ_LINES };
    static struct uui_wizard_step wz_steps[1];
    wz_steps[0] = (struct uui_wizard_step){ "Appearance", "Each of these is in System Settings as well.",
                                            { .ops = &uui_layout_ops, .widget = &wz_page }, 0 };
    static struct uui_wizard wz;
    uui_wizard_init(&wz, &(struct uui_wizard_desc){ .heading = "Set up", .steps = wz_steps, .count = 1 });
    // A window of a wizard's own size: in the shared 460x320 one its rail
    // and header leave the page too short to draw a thumb in.
    struct uui_item *wit = uui_wizard_item(&wz);
    wit->ops->set_geometry(wit->widget, 0, 0, 800, 480);
    uitest_window(&win, wit, 1, NULL, 800, 480);
    ff.widget = wit->widget;
    ff.ops = wit->ops;
    uui_focus_init(&focus, &ff, 1);
    uitest_set_focus(&win, &focus);
    int px, py, pw, ph;
    // check_view() skips a view it cannot find: a wizard with no
    // scrolling page would otherwise pass with nothing checked.
    utest_check(uitest_rect(&win, "page", &px, &py, &pw, &ph), "Setup wizard: its page is a scroll view");
    check_view(&win, &(struct view){ "Setup wizard page", "page", 0, 0, 0, 0, 0, 20, 20 });
    uitest_window_free(&win);

    return uitest_end();
}
