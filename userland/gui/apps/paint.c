// Paint -- draw on a picture, or touch one up, and save it.
//
// THE LAYOUT IS P3 (chosen from mockups, 2026-10-14): a slim TOOL RAIL
// down the left (a vertical uui_toolbar), a COMMAND BAR for file, undo,
// rotate and zoom, the picture in the middle (ui/uui_paintcanvas.h), a
// RIGHT PANEL that can be hidden -- the two colours, the colour picker
// (ui/uui_colourpick.h), Recent and the palette (ui/uui_swatches.h), the
// brush -- and the status panes. Windows 11 Paint's command bar with
// KolourPaint's rail, rather than either's ribbon.
//
// TWO COLOURS, MS Paint's model: the left button paints the first and
// the right button the second, and the eraser paints the second. The
// right button reaches no widget (the toolkit routes the primary alone),
// so on_press/on_motion/on_release drive it here.
//
// WHERE THINGS ARE: this file is the window; userland/paint/paint_tools.c
// is what the pointer does to the picture; lib/upixel.h paints and
// lib/upixundo.h remembers.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt/sys.h"
#include "kpath.h"
#include "keyboard.h"
#include "syscall_abi.h"
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/ulog.h"
#include "ui/uui.h"
#include "ui/uui_colourpick.h"
#include "ui/uui_dialog.h"
#include "ui/uui_filedialog.h"
#include "ui/uui_imagetypes.h"
#include "ui/uui_infonote.h"
#include "ui/uui_focus.h"
#include "ui/uui_menubar.h"
#include "ui/uui_segmented.h"
#include "ui/uui_sizepick.h"
#include "ui/uui_slider.h"
#include "ui/uui_statusbar.h"
#include "ui/uui_swatches.h"
#include "ui/uui_toolbar.h"
#include "lib/ufile.h"
#include "query_abi.h"
#include "lib/uimg.h"
#include "paint/paint.h"

#define PATH_MAX_LEN 256
#define NEW_W 800
#define NEW_H 600
#define RECENT_MAX 10

enum {
    ID_MENU = 1, ID_BAR, ID_RAIL, ID_CANVAS, ID_RECENT, ID_PALETTE, ID_SIZE, ID_SHAPE,
    ID_STATUS, ID_ASK, ID_NEWDLG, ID_NEW_BG, ID_RESIZE,
    ID_PICK = 40,   // .. + UUI_COLOURPICK_IDS
    ID_NEW_SIZE = 50,   // .. + UUI_SIZEPICK_IDS
};
enum {
    CMD_NEW = 100, CMD_OPEN, CMD_SAVE, CMD_SAVE_AS, CMD_EXIT,
    CMD_UNDO, CMD_REDO,
    CMD_ZOOM_IN, CMD_ZOOM_OUT, CMD_ZOOM_ACTUAL, CMD_ZOOM_FIT, CMD_PANEL,
    CMD_ROT_RIGHT, CMD_ROT_LEFT, CMD_ROT_180, CMD_FLIP_H, CMD_FLIP_V, CMD_RESIZE, CMD_CANVAS,
    CMD_SWAP,
    CMD_TOOL = 200,   // + enum paint_tool
    // The unsaved-changes question's answers.
    ASK_SAVE = 300, ASK_DISCARD, ASK_CANCEL,
    NEW_CREATE = 310, NEW_CANCEL,
};

// --- the state paint.h shares ---------------------------------------------

struct uimg g_img;
struct upixundo g_undo;
struct uui_paintcanvas g_canvas;
int g_tool = TOOL_BRUSH;
int g_tool_before_pick = TOOL_BRUSH;
uint32_t g_colour[2] = { 0xFF000000u, 0xFFFFFFFFu };
int g_brush = 3;
enum upix_shape g_shape = UPIX_ROUND;

// --- the window's own -------------------------------------------------------

static struct uapp *g_app;
static struct uui_menubar g_menu;
static struct uui_toolbar g_bar, g_rail;
static struct uui_colourpick g_pick;
static struct uui_swatches g_recent, g_palette;
static struct uui_slider g_size;
static struct uui_segmented g_shape_seg;
static struct uui_statusbar g_sb;
static struct uui_dialog g_ask;

// --- the New picture dialog: size cards, then the background ------------
//
// N2 from the mockups (2026-10-14): Photoshop's New Document cards over
// ui/uui_sizepick.h. "Screen" is filled in from the display at start.
static struct uui_sizepick_preset g_presets[] = {
    { "Screen", 1366, 768 }, { "Small", 800, 600 }, { "HD", 1280, 720 }, { "Full HD", 1920, 1080 },
    { "Square", 1080, 1080 }, { "A4", 1240, 1754 }, { "Icon", 256, 256 }, { "Custom", 0, 0 },
};
#define NEW_MAX 8192
static const char *const BACKGROUNDS[] = { "White", "Colour 2", "Transparent" };
static struct uui_dialog g_newdlg;
static struct uui_sizepick g_sizepick;
static struct uui_segmented g_bg_seg;
static struct uui_custom g_bg_label;
static struct uui_infonote g_bg_note;
static uint32_t g_bg_chips[3];
static struct uui_item g_bg_it[2], g_new_it[3], g_new_body;
static struct uui_layout g_bg_row, g_new_col;
static int g_last_w = NEW_W, g_last_h = NEW_H, g_last_bg;
static struct uui_filedialog g_fd;
static struct uui_focus g_focus;
static struct uui_focusable g_ring[8];

static char g_path[PATH_MAX_LEN];        // "" for a picture never saved
static unsigned g_saved_state;           // upixundo_state() at the last save or open
static int g_panel = 1;                  // the right panel shown
static int g_edit;                       // the colour the picker edits: 0 or 1
static int g_slot_down = -1;             // the right button's stroke, while held
static char g_title[PATH_MAX_LEN + 16];
static char g_zoom_text[12], g_st_pos[32], g_st_size[32], g_st_note[128];
static char g_size_text[24];
static char g_ask_line[PATH_MAX_LEN + 48];
static const char *g_ask_rows[2];

// What the unsaved-changes question was asked for: what to do once the
// picture is saved, or the change let go.
enum pending { PEND_NONE, PEND_NEW, PEND_OPEN, PEND_QUIT };
static enum pending g_pending;

static uint32_t g_recent_c[RECENT_MAX];
static int g_recent_n;
// MS Paint's colour box, Windows 7 onwards: ten dark-to-light, then ten
// lighter.
static const uint32_t PALETTE[] = {
    0x000000, 0x7F7F7F, 0x880015, 0xED1C24, 0xFF7F27, 0xFFF200, 0x22B14C, 0x00A2E8, 0x3F48CC, 0xA349A4,
    0xFFFFFF, 0xC3C3C3, 0xB97A57, 0xFFAEC9, 0xFFC90E, 0xEFE4B0, 0xB5E61D, 0x99D9EA, 0x7092BE, 0xC8BFE7,
};
static const char *const SIZES[] = { "1", "2", "3", "5", "8", "12", "20", "32" };
static const int SIZE_PX[] = { 1, 2, 3, 5, 8, 12, 20, 32 };
#define NSIZES ((int)(sizeof SIZE_PX / sizeof SIZE_PX[0]))
static const char *const SHAPES[] = { "Round", "Square" };

// Where the panel's pieces went this frame -- for on_draw's headings and
// the two colour boxes' hit test.
static int g_px, g_pw, g_box_y, g_recent_head_y, g_palette_head_y, g_brush_head_y, g_colour_head_y;
static int g_box_x[2], g_box_s;

// --- the menus and the bars ---------------------------------------------------

static const struct uui_menu_item file_items[] = {
    UUI_MENU_ICON("New", CMD_NEW, "Ctrl+N", "tb-new", UTHEME_ACT_CREATE),
    UUI_MENU_ICON("Open...", CMD_OPEN, "Ctrl+O", "tb-open", UTHEME_ACT_NAV),
    UUI_MENU_SEP,
    UUI_MENU_ICON("Save", CMD_SAVE, "Ctrl+S", "tb-save", UTHEME_ACT_CREATE),
    UUI_MENU("Save as...", CMD_SAVE_AS, "Ctrl+Shift+S"),
    UUI_MENU_SEP,
    UUI_MENU("Exit", CMD_EXIT, "Alt+F4"),
};
static const struct uui_menu_item edit_items[] = {
    UUI_MENU_ICON("Undo", CMD_UNDO, "Ctrl+Z", "tb-undo", 0),
    UUI_MENU_ICON("Redo", CMD_REDO, "Ctrl+Y", "tb-redo", 0),
};
static const struct uui_menu_item view_items[] = {
    UUI_MENU_ICON("Zoom in", CMD_ZOOM_IN, "Ctrl++", "tb-zoom-in", UTHEME_ACT_VIEW),
    UUI_MENU_ICON("Zoom out", CMD_ZOOM_OUT, "Ctrl+-", "tb-zoom-out", UTHEME_ACT_VIEW),
    UUI_MENU_ICON("Actual size", CMD_ZOOM_ACTUAL, "Ctrl+0", "tb-actual", UTHEME_ACT_VIEW),
    UUI_MENU_ICON("Fit to window", CMD_ZOOM_FIT, 0, "tb-fit", UTHEME_ACT_VIEW),
    UUI_MENU_SEP,
    UUI_MENU("Side panel", CMD_PANEL, 0),
};
static const struct uui_menu_item image_items[] = {
    UUI_MENU("Resize...", CMD_RESIZE, "Ctrl+R"),
    UUI_MENU("Canvas size...", CMD_CANVAS, "Ctrl+Shift+R"),
    UUI_MENU_SEP,
    UUI_MENU_ICON("Rotate right", CMD_ROT_RIGHT, 0, "tb-rotate-right", 0),
    UUI_MENU_ICON("Rotate left", CMD_ROT_LEFT, 0, "tb-rotate-left", 0),
    UUI_MENU("Rotate 180\xb0", CMD_ROT_180, 0),   // Latin-1, as every string drawn here
    UUI_MENU_SEP,
    UUI_MENU("Flip horizontally", CMD_FLIP_H, 0),
    UUI_MENU("Flip vertically", CMD_FLIP_V, 0),
};
static const struct uui_menu_item tool_items[] = {
    UUI_MENU_ICON("Pencil", CMD_TOOL + TOOL_PENCIL, "P", "tb-pencil", 0),
    UUI_MENU_ICON("Brush", CMD_TOOL + TOOL_BRUSH, "B", "tb-brush", 0),
    UUI_MENU_ICON("Eraser", CMD_TOOL + TOOL_ERASER, "E", "tb-eraser", 0),
    UUI_MENU_ICON("Fill", CMD_TOOL + TOOL_FILL, "F", "tb-fill", 0),
    UUI_MENU_ICON("Colour picker", CMD_TOOL + TOOL_PICK, "I", "tb-picker", 0),
};
static const struct uui_menu_item colour_items[] = {
    UUI_MENU("Swap the two colours", CMD_SWAP, "X"),
};
static const struct uui_menu_item menu_bar[] = {
    UUI_SUBMENU("File", file_items),
    UUI_SUBMENU("Edit", edit_items),
    UUI_SUBMENU("View", view_items),
    UUI_SUBMENU("Image", image_items),
    UUI_SUBMENU("Tools", tool_items),
    UUI_SUBMENU("Colours", colour_items),
};

static const struct uui_toolbar_item bar_items[] = {
    { "tb-new",          "New (Ctrl+N)",        CMD_NEW,       0, 0, 0, UTHEME_ACT_CREATE },
    { "tb-open",         "Open (Ctrl+O)",       CMD_OPEN,      0, 0, 0, UTHEME_ACT_NAV },
    { "tb-save",         "Save (Ctrl+S)",       CMD_SAVE,      0, 0, 0, UTHEME_ACT_CREATE },
    UUI_TOOLBAR_SEP,
    { "tb-undo",         "Undo (Ctrl+Z)",       CMD_UNDO,      0, 0, 0, 0 },
    { "tb-redo",         "Redo (Ctrl+Y)",       CMD_REDO,      0, 0, 0, 0 },
    UUI_TOOLBAR_SEP,
    { "tb-rotate-left",  "Rotate left",         CMD_ROT_LEFT,  0, 0, 0, 0 },
    { "tb-rotate-right", "Rotate right",        CMD_ROT_RIGHT, 0, 0, 0, 0 },
    UUI_TOOLBAR_SEP,
    { "tb-zoom-out",     "Zoom out (Ctrl+-)",   CMD_ZOOM_OUT,  0, 0, 0, UTHEME_ACT_VIEW },
    { 0,                 0,                     0,             g_zoom_text, UUI_TB_TEXT, 0, 0 },
    { "tb-zoom-in",      "Zoom in (Ctrl++)",    CMD_ZOOM_IN,   0, 0, 0, UTHEME_ACT_VIEW },
    { "tb-fit",          "Fit to window",       CMD_ZOOM_FIT,  0, 0, 0, UTHEME_ACT_VIEW },
    { "tb-pane",         "Show or hide the side panel", CMD_PANEL, 0, UUI_TB_END, 0, UTHEME_ACT_VIEW },
};

// The rail: one button per tool, latched on the one in use.
static const struct uui_toolbar_item rail_items[] = {
    { "tb-pencil", "Pencil (P)",        CMD_TOOL + TOOL_PENCIL, 0, 0, 0, 0 },
    { "tb-brush",  "Brush (B)",         CMD_TOOL + TOOL_BRUSH,  0, 0, 0, 0 },
    { "tb-eraser", "Eraser (E)",        CMD_TOOL + TOOL_ERASER, 0, 0, 0, 0 },
    UUI_TOOLBAR_SEP,
    { "tb-fill",   "Fill (F)",          CMD_TOOL + TOOL_FILL,   0, 0, 0, 0 },
    { "tb-picker", "Colour picker (I)", CMD_TOOL + TOOL_PICK,   0, 0, 0, 0 },
};

static struct uui_item g_widgets[] = {
    { .ops = &uui_toolbar_ops,       .widget = &g_bar,       .id = ID_BAR,     .name = "bar" },
    { .ops = &uui_toolbar_ops,       .widget = &g_rail,      .id = ID_RAIL,    .name = "rail" },
    { .ops = &uui_paintcanvas_ops,   .widget = &g_canvas,    .id = ID_CANVAS,  .name = "canvas" },
    { 0 },   // the colour picker: uui_colourpick_item(), set in main()
    { .ops = &uui_swatches_ops,      .widget = &g_recent,    .id = ID_RECENT,  .name = "recent" },
    { .ops = &uui_swatches_ops,      .widget = &g_palette,   .id = ID_PALETTE, .name = "palette" },
    { .ops = &uui_slider_ops,        .widget = &g_size,      .id = ID_SIZE,    .name = "size" },
    { .ops = &uui_segmented_ops,     .widget = &g_shape_seg, .id = ID_SHAPE,   .name = "shape" },
    { .ops = &uui_statusbar_ops,     .widget = &g_sb,        .id = ID_STATUS,  .name = "status" },
    // The menu bar after the bars: its popups must be on top of them.
    { .ops = &uui_menubar_ops,       .widget = &g_menu,      .id = ID_MENU,    .name = "menu" },
    { .ops = &uui_dialog_ops,        .widget = &g_ask,       .id = ID_ASK,     .name = "ask" },
    { .ops = &uui_dialog_ops,        .widget = &g_newdlg,    .id = ID_NEWDLG,  .name = "newdlg" },
    { 0 },   // Resize: paint_resize.c's dialog, set in main()
};
enum { W_PICK = 3, W_PANEL_FIRST = 3, W_PANEL_LAST = 7 };

// --- small things -------------------------------------------------------------

static int dirty(void) { return upixundo_state(&g_undo) != g_saved_state; }

static const char *doc_name(void) { return g_path[0] ? k_path_basename(g_path) : "Untitled"; }

// SENT ONLY WHEN IT CHANGES: paint_changed() runs on every motion of a
// stroke, and a title is a message to the compositor and a repaint of
// its bar -- per event, enough to starve the stroke of frames.
static void set_title(void) {
    static char sent[sizeof g_title];
    snprintf(g_title, sizeof g_title, "%s%s - Paint", dirty() ? "*" : "", doc_name());
    if (!g_app || !strcmp(sent, g_title)) return;
    snprintf(sent, sizeof sent, "%s", g_title);
    uapp_set_title(g_app, g_title);
}

static void note(const char *s) { snprintf(g_st_note, sizeof g_st_note, "%s", s); }

static void sync_size_text(void) {
    snprintf(g_size_text, sizeof g_size_text, "Brush size \xb7 %d px", g_brush);
}

static void sync_pick(void) {
    uui_colourpick_set(&g_pick, g_colour[g_edit] & 0xFFFFFF);
}

static int size_index(int px) {
    for (int i = 0; i < NSIZES; i++) if (SIZE_PX[i] >= px) return i;
    return NSIZES - 1;
}

static void set_brush(int px) {
    g_brush = px < 1 ? 1 : px > SIZE_PX[NSIZES - 1] ? SIZE_PX[NSIZES - 1] : px;
    g_size.selected = size_index(g_brush);
    sync_size_text();
}

// --- what paint.h asks of the window -------------------------------------------

void paint_changed(const struct upix_rect *r) {
    (void)r;
    set_title();
    if (g_app) uapp_redraw(g_app);
}

void paint_colour_used(int slot) {
    uint32_t c = g_colour[slot] & 0xFFFFFF;
    int at = -1;
    for (int i = 0; i < g_recent_n; i++) if (g_recent_c[i] == c) at = i;
    if (at == 0) return;
    if (at < 0) at = g_recent_n < RECENT_MAX ? g_recent_n++ : RECENT_MAX - 1;
    memmove(g_recent_c + 1, g_recent_c, sizeof g_recent_c[0] * (size_t)at);
    g_recent_c[0] = c;
    uui_swatches_set_count(&g_recent, g_recent_n);
}

void paint_colour_picked(int slot) {
    g_edit = slot;
    sync_pick();
    char buf[48];
    snprintf(buf, sizeof buf, "Picked #%06X", (unsigned)(g_colour[slot] & 0xFFFFFF));
    note(buf);
}

void paint_tool_changed(void) {
    // A crosshair to aim with; the fill and the picker act at a point.
    g_canvas.cursor = WIN_CURSOR_CROSSHAIR;
    if (g_app) uapp_redraw(g_app);
}

static void set_tool(int t) {
    if (t < 0 || t >= TOOL_COUNT || tools_busy()) return;
    if (t == TOOL_PICK && g_tool != TOOL_PICK) g_tool_before_pick = g_tool;
    g_tool = t;
    ulogf("paint: tool %d\n", t);
    paint_tool_changed();
}

// --- the picture ------------------------------------------------------------------

// A fresh picture of one colour -- white, MS Paint's default -- with an
// empty history.
static int new_picture(int w, int h, uint32_t bg) {
    uint32_t *px = malloc((size_t)w * h * 4);
    if (!px) { note("Not enough memory for a picture that big"); return 0; }
    for (size_t i = 0; i < (size_t)w * h; i++) px[i] = bg;
    uimg_free(&g_img);
    g_img.px = px;
    g_img.w = w;
    g_img.h = h;
    g_img.has_alpha = (bg >> 24) != 0xFF;
    upixundo_clear(&g_undo);
    g_saved_state = upixundo_state(&g_undo);
    g_path[0] = '\0';
    g_canvas.img = &g_img;
    uui_paintcanvas_picture_changed(&g_canvas);
    uui_paintcanvas_set_zoom(&g_canvas, 100, -1, -1);
    set_title();
    return 1;
}

static int load_picture(const char *path) {
    struct uimg im = { 0 };
    int r = uimg_load(path, &im);
    if (r != 0) {
        snprintf(g_st_note, sizeof g_st_note, "Can't open %s: %s", k_path_basename(path), uimg_last_error());
        ulogf("paint: open %s failed: %d\n", path, r);
        return 0;
    }
    uimg_free(&g_img);
    g_img = im;
    upixundo_clear(&g_undo);
    g_saved_state = upixundo_state(&g_undo);
    snprintf(g_path, sizeof g_path, "%s", path);
    g_canvas.img = &g_img;
    uui_paintcanvas_picture_changed(&g_canvas);
    // Shown whole, never enlarged: a photo opens fitted, an icon at 100%.
    uui_paintcanvas_set_zoom(&g_canvas, uui_paintcanvas_fit_zoom(&g_canvas), -1, -1);
    note("");
    set_title();
    ulogf("paint: opened %s %dx%d\n", path, g_img.w, g_img.h);
    return 1;
}

// The format a path's extension names, if this build writes it.
static const char *write_format(const char *path) {
    const char *dot = strrchr(k_path_basename(path), '.');
    if (!dot) return 0;
    if (!strcasecmp(dot, ".png")) return "png";
    if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")) return "jpeg";
    if (!strcasecmp(dot, ".qoi")) return "qoi";
    return 0;
}

static int save_to(const char *path) {
    const char *fmt = write_format(path);
    if (!fmt) { note("Save as .png, .jpg or .qoi"); return 0; }
    int r = uimg_save(path, &g_img, fmt);
    if (r != 0) {
        snprintf(g_st_note, sizeof g_st_note, "Couldn't save %s (%d)", k_path_basename(path), r);
        ulogf("paint: save %s failed: %d\n", path, r);
        return 0;
    }
    snprintf(g_path, sizeof g_path, "%s", path);
    g_saved_state = upixundo_state(&g_undo);
    snprintf(g_st_note, sizeof g_st_note, "Saved %s", k_path_basename(path));
    ulogf("paint: saved %s as %s\n", path, fmt);
    set_title();
    return 1;
}

// --- choosers and the question ---------------------------------------------------

static void run_pending(void);
static void open_new_dialog(void);

static void start_dir(char *out, int cap) {
    if (g_path[0]) { k_path_dirname(g_path, out, (size_t)cap); return; }
    struct sys_stat st;
    snprintf(out, (size_t)cap, "%s", sys_stat("/home/Pictures", &st) == 0 ? "/home/Pictures" : "/home");
}

static void open_chosen(void *ctx, const char *path) {
    (void)ctx;
    if (path) load_picture(path);
    if (g_app) uapp_redraw(g_app);
}

static void open_dialog(void) {
    if (uui_filedialog_is_open(&g_fd)) return;
    char dir[PATH_MAX_LEN];
    start_dir(dir, sizeof dir);
    struct uui_filedialog_opts o = {
        .mode = UUI_FILEDIALOG_OPEN, .title = "Open Picture", .start_dir = dir,
        .filters = uui_imagetypes_open, .filter_count = uui_imagetypes_open_count,
    };
    uui_filedialog_open(g_app, &g_fd, &o, open_chosen, 0);
}

static void save_chosen(void *ctx, const char *path) {
    (void)ctx;
    if (!path) { g_pending = PEND_NONE; return; }
    char full[PATH_MAX_LEN];
    // A name with no extension this build writes is a PNG -- MS Paint's
    // default, and the lossless one.
    if (write_format(path)) snprintf(full, sizeof full, "%s", path);
    else snprintf(full, sizeof full, "%s.png", path);
    if (save_to(full)) run_pending();
    else g_pending = PEND_NONE;
    if (g_app) uapp_redraw(g_app);
}

static void save_as_dialog(void) {
    if (uui_filedialog_is_open(&g_fd)) return;
    char dir[PATH_MAX_LEN], name[PATH_MAX_LEN];
    start_dir(dir, sizeof dir);
    snprintf(name, sizeof name, "%s", g_path[0] ? k_path_basename(g_path) : "Untitled.png");
    if (g_path[0] && !write_format(g_path)) {
        // A .bmp or .gif opened here is saved as a PNG beside it.
        char *dot = strrchr(name, '.');
        if (dot) *dot = '\0';
        strlcat(name, ".png", sizeof name);
    }
    // SAVE AS TYPE, starting at the picture's own format: picking another
    // row rewrites the name's extension (ui/uui_filedialog.h).
    struct uui_filedialog_opts o = {
        .mode = UUI_FILEDIALOG_SAVE, .title = "Save Picture As", .start_dir = dir,
        .initial_name = name,
        .filters = uui_imagetypes_save, .filter_count = uui_imagetypes_save_count,
        .filter_index = uui_imagetypes_save_index(write_format(name)),
    };
    uui_filedialog_open(g_app, &g_fd, &o, save_chosen, 0);
}

static void do_save(void) {
    if (g_path[0] && write_format(g_path)) { if (save_to(g_path)) run_pending(); else g_pending = PEND_NONE; }
    else save_as_dialog();
}

static void run_pending(void) {
    enum pending p = g_pending;
    g_pending = PEND_NONE;
    if (p == PEND_NEW) open_new_dialog();
    else if (p == PEND_OPEN) open_dialog();
    else if (p == PEND_QUIT) uapp_quit(g_app, 0);
}

// THE UNSAVED-CHANGES QUESTION, Save / Don't save / Cancel -- Windows'
// three, the safe one the default.
static void ask_then(enum pending p) {
    if (!dirty()) { g_pending = p; run_pending(); return; }
    static const struct uui_dialog_button btns[] = {
        { "Save", ASK_SAVE, UUI_DLG_PRIMARY }, { "Don't save", ASK_DISCARD, 0 },
        { "Cancel", ASK_CANCEL, 0 },
    };
    g_pending = p;
    snprintf(g_ask_line, sizeof g_ask_line, "Do you want to save changes to %s?", doc_name());
    g_ask_rows[0] = g_ask_line;
    g_ask_rows[1] = 0;
    uui_dialog_set_bounds(&g_ask, 0, 0, uapp_width(g_app), uapp_height(g_app));
    uui_dialog_open(&g_ask, "Paint", g_ask_rows, 1, btns, 3, 0, ASK_CANCEL);
}

static void ask_answer(int code) {
    uui_dialog_close(&g_ask);
    if (code == ASK_SAVE) do_save();
    else if (code == ASK_DISCARD) run_pending();
    else g_pending = PEND_NONE;
}

static void draw_label(struct ugfx_surface *s, const struct uui_custom *cu) {
    ugfx_draw_string_clipped(s, cu->x, cu->y + (cu->h - ugfx_char_h()) / 2, cu->w,
                             (const char *)cu->state, UTHEME_TEXT, UGFX_TRANSPARENT);
}

static void open_new_dialog(void) {
    if (uui_dialog_is_open(&g_newdlg)) return;
    uui_sizepick_init(&g_sizepick, ID_NEW_SIZE, g_presets, (int)(sizeof g_presets / sizeof g_presets[0]),
                      g_last_w, g_last_h, NEW_MAX);
    uui_segmented_init(&g_bg_seg, BACKGROUNDS, 3, g_last_bg);
    // B1 from the mockups: each choice shows the colour it is.
    g_bg_chips[0] = 0xFFFFFF;
    g_bg_chips[1] = g_colour[1] & 0xFFFFFF;
    g_bg_chips[2] = UUI_SEG_CHECKER;
    g_bg_seg.chips = g_bg_chips;
    g_bg_label = (struct uui_custom){ .w = ugfx_text_width("Background") + ugfx_char_w(),
                                      .h = utheme_control_h(), .draw = draw_label, .state = (void *)"Background" };
    g_bg_it[0] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &g_bg_label };
    g_bg_it[1] = (struct uui_item){ .ops = &uui_segmented_ops, .widget = &g_bg_seg, .id = ID_NEW_BG,
                                    .name = "background" };
    g_bg_row = (struct uui_layout){ .dir = UUI_ROW, .items = g_bg_it, .count = 2 };
    uui_infonote_init(&g_bg_note, "Colour 2 is the side panel's second colour. Transparent shows as a checkerboard.");
    g_new_it[0] = uui_sizepick_item(&g_sizepick);
    g_new_it[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_bg_row };
    g_new_it[2] = (struct uui_item){ .ops = &uui_infonote_ops, .widget = &g_bg_note, .name = "bgnote" };
    g_new_col = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_new_it, .count = 3 };
    g_new_body = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_new_col, .name = "new" };
    static const struct uui_dialog_button btns[] = {
        { "Cancel", NEW_CANCEL, 0 }, { "Create", NEW_CREATE, UUI_DLG_PRIMARY },
    };
    uui_dialog_set_bounds(&g_newdlg, 0, 0, uapp_width(g_app), uapp_height(g_app));
    uui_dialog_open(&g_newdlg, "New picture", 0, 0, btns, 2, 1, NEW_CANCEL);
    int bw, bh;
    uui_layout_ops.natural_size(&g_new_col, &bw, &bh);
    uui_dialog_set_body(&g_newdlg, &g_new_body, bw, bh);
    struct uui_item *tabs[] = { &g_sizepick.w_it[1], &g_sizepick.h_it[1] };
    uui_dialog_set_tabs(&g_newdlg, tabs, 2);
    uui_dialog_focus(&g_newdlg, &g_sizepick.w_it[1]);
}

static void new_answer(int code) {
    uui_dialog_close(&g_newdlg);
    uui_dialog_set_body(&g_newdlg, 0, 0, 0);
    if (code != NEW_CREATE) return;
    uui_sizepick_commit(&g_sizepick);
    int w, h;
    uui_sizepick_size(&g_sizepick, &w, &h);
    int bg = g_bg_seg.selected < 0 ? 0 : g_bg_seg.selected;
    uint32_t fill = bg == 0 ? 0xFFFFFFFFu : bg == 1 ? g_colour[1] : 0x00FFFFFFu;
    if (!new_picture(w, h, fill)) return;
    g_last_w = w;
    g_last_h = h;
    g_last_bg = bg;
    ulogf("paint: new %dx%d background %d\n", w, h, bg);
}

// --- whole-picture changes ---------------------------------------------------------

void paint_note(const char *text) { note(text); }

void paint_replace_picture(struct uimg *out) {
    upixundo_begin(&g_undo, &g_img);
    uimg_free(&g_img);
    g_img = *out;
    upixundo_commit(&g_undo, &g_img, 0);
    uui_paintcanvas_picture_changed(&g_canvas);
    paint_changed(0);
}

static void rotate(int quarters) {
    struct uimg out = { 0 };
    if (uimg_rotate(&g_img, quarters, &out) != 0) { note("Not enough memory to rotate"); return; }
    paint_replace_picture(&out);
}

static void flip(int horizontal) {
    upixundo_begin(&g_undo, &g_img);
    for (int y = 0; y < (horizontal ? g_img.h : g_img.h / 2); y++) {
        uint32_t *row = g_img.px + (size_t)y * g_img.w;
        if (horizontal) {
            for (int i = 0, j = g_img.w - 1; i < j; i++, j--) { uint32_t t = row[i]; row[i] = row[j]; row[j] = t; }
        } else {
            uint32_t *other = g_img.px + (size_t)(g_img.h - 1 - y) * g_img.w;
            for (int i = 0; i < g_img.w; i++) { uint32_t t = row[i]; row[i] = other[i]; other[i] = t; }
        }
    }
    struct upix_rect all = { 0, 0, g_img.w, g_img.h };
    upixundo_commit(&g_undo, &g_img, &all);
    paint_changed(&all);
}

static void undo_redo(int redo) {
    struct upix_rect r;
    int ok = redo ? upixundo_redo(&g_undo, &g_img, &r) : upixundo_undo(&g_undo, &g_img, &r);
    if (!ok) return;
    if (r.w == 0) uui_paintcanvas_picture_changed(&g_canvas);
    paint_changed(&r);
}

static void zoom_to(int z) {
    uui_paintcanvas_set_zoom(&g_canvas, z, -1, -1);
}

static void command(int code) {
    if (code >= CMD_TOOL && code < CMD_TOOL + TOOL_COUNT) { set_tool(code - CMD_TOOL); return; }
    if (tools_busy()) return;
    switch (code) {
    case CMD_NEW:         ask_then(PEND_NEW); break;
    case CMD_OPEN:        ask_then(PEND_OPEN); break;
    case CMD_SAVE:        do_save(); break;
    case CMD_SAVE_AS:     save_as_dialog(); break;
    case CMD_EXIT:        ask_then(PEND_QUIT); break;
    case CMD_UNDO:        undo_redo(0); break;
    case CMD_REDO:        undo_redo(1); break;
    case CMD_ZOOM_IN:     zoom_to(uui_paintcanvas_zoom_step(g_canvas.zoom, 1)); break;
    case CMD_ZOOM_OUT:    zoom_to(uui_paintcanvas_zoom_step(g_canvas.zoom, -1)); break;
    case CMD_ZOOM_ACTUAL: zoom_to(100); break;
    case CMD_ZOOM_FIT:    zoom_to(uui_paintcanvas_fit_zoom(&g_canvas)); break;
    case CMD_PANEL:       g_panel = !g_panel; break;
    case CMD_ROT_RIGHT:   rotate(1); break;
    case CMD_ROT_LEFT:    rotate(3); break;
    case CMD_ROT_180:     rotate(2); break;
    case CMD_FLIP_H:      flip(1); break;
    case CMD_FLIP_V:      flip(0); break;
    case CMD_RESIZE:      resize_open(g_app, 0); break;
    case CMD_CANVAS:      resize_open(g_app, 1); break;
    case CMD_SWAP: {
        uint32_t t = g_colour[0]; g_colour[0] = g_colour[1]; g_colour[1] = t;
        sync_pick();
        break;
    }
    default: break;
    }
    if (g_app) uapp_redraw(g_app);
}

static unsigned item_flags(int code) {
    if (code >= CMD_TOOL && code < CMD_TOOL + TOOL_COUNT)
        return g_tool == code - CMD_TOOL ? UUI_MI_CHECKED : 0;
    switch (code) {
    case CMD_UNDO:  return upixundo_can_undo(&g_undo) ? 0 : UUI_MI_DISABLED;
    case CMD_REDO:  return upixundo_can_redo(&g_undo) ? 0 : UUI_MI_DISABLED;
    case CMD_PANEL: return g_panel ? UUI_MI_CHECKED : 0;
    case CMD_ZOOM_IN:  return g_canvas.zoom >= UUI_PAINT_ZOOM_MAX ? UUI_MI_DISABLED : 0;
    case CMD_ZOOM_OUT: return g_canvas.zoom <= UUI_PAINT_ZOOM_MIN ? UUI_MI_DISABLED : 0;
    default: return 0;
    }
}

// --- the canvas's pointer, both buttons ------------------------------------------

static void canvas_pointer(void *ctx, enum uui_paint_phase ph, int x, int y, unsigned mods) {
    (void)ctx; (void)mods;
    if (g_canvas.hovering) snprintf(g_st_pos, sizeof g_st_pos, "%d, %d px", x, y);
    else g_st_pos[0] = '\0';
    if (ph == UUI_PAINT_DOWN) uui_focus_set(&g_focus, -1);   // typed letters are tools again
    if (ph != UUI_PAINT_HOVER) tools_pointer(ph, x, y, 0);
    if (g_app) uapp_redraw(g_app);
}

// --- layout and drawing ------------------------------------------------------------

static void layout(int cw, int ch) {
    int pad = ugfx_char_w(), lh = ugfx_char_h();
    int mh = uui_menubar_height(&g_menu);
    uui_menubar_set_geometry(&g_menu, 0, 0, cw, mh);
    uui_menubar_set_bounds(&g_menu, 0, 0, cw, ch);
    int th = uui_toolbar_height(&g_bar);
    uui_toolbar_ops.set_geometry(&g_bar, 0, mh, cw, th);
    uui_toolbar_set_bounds(&g_bar, 0, 0, cw, ch);
    int sbh = uui_statusbar_height(&g_sb);
    uui_statusbar_set_geometry(&g_sb, 0, ch - sbh, cw, sbh);
    int top = mh + th, bottom = ch - sbh;
    int rw, rh;
    uui_toolbar_natural_size(&g_rail, &rw, &rh);
    uui_toolbar_ops.set_geometry(&g_rail, 0, top, rw, bottom - top);
    uui_toolbar_set_bounds(&g_rail, 0, 0, cw, ch);

    g_pw = g_panel ? pad * 30 : 0;
    g_px = cw - g_pw;
    for (int i = W_PANEL_FIRST; i <= W_PANEL_LAST; i++) g_widgets[i].hidden = !g_panel;
    uui_paintcanvas_ops.set_geometry(&g_canvas, rw, top, g_px - rw - (g_panel ? 1 : 0), bottom - top);
    if (!g_panel) return;

    // The panel, top to bottom: the two colours, the picker, Recent,
    // the palette, the brush. Headings are on_draw's.
    int x = g_px + pad, w = g_pw - 2 * pad, y = top + pad;
    g_colour_head_y = y;
    y += lh + pad / 2;
    g_box_s = lh * 2;
    g_box_y = y;
    g_box_x[0] = x;
    g_box_x[1] = x + ugfx_text_width("Colour 1") + pad * 2;
    y += g_box_s + lh + pad;
    int pw_, ph_;
    g_widgets[W_PICK].ops->natural_size(g_widgets[W_PICK].widget, &pw_, &ph_);
    g_widgets[W_PICK].ops->set_geometry(g_widgets[W_PICK].widget, x, y, w, ph_);
    y += ph_ + pad;
    g_recent_head_y = y;
    y += lh + pad / 2;
    int sw, sh;
    uui_swatches_ops.natural_size(&g_recent, &sw, &sh);
    uui_swatches_ops.set_geometry(&g_recent, x, y, w, sh);
    y += sh + pad;
    g_palette_head_y = y;
    y += lh + pad / 2;
    uui_swatches_ops.natural_size(&g_palette, &sw, &sh);
    uui_swatches_ops.set_geometry(&g_palette, x, y, w, sh);
    y += sh + pad;
    g_brush_head_y = y;
    y += lh + pad / 2;
    int slw, slh;
    uui_slider_natural_size(&g_size, &slw, &slh);
    uui_slider_set_geometry(&g_size, x, y, w, slh);
    y += slh + pad / 2;
    uui_segmented_set_geometry(&g_shape_seg, x, y);
}

static void status(void) {
    snprintf(g_zoom_text, sizeof g_zoom_text, "%d%%", g_canvas.zoom);
    snprintf(g_st_size, sizeof g_st_size, "%d \xd7 %d px", g_img.w, g_img.h);
}

static void heading(struct ugfx_surface *s, int y, const char *text) {
    ugfx_draw_string_clipped(s, g_px + ugfx_char_w(), y, g_pw - 2 * ugfx_char_w(), text,
                             UTHEME_TEXT, UTHEME_PANEL_BG);
}

// A colour well: the colour, with a ring when the picker is editing it.
static void colour_box(struct ugfx_surface *s, int i) {
    int x = g_box_x[i], y = g_box_y, n = g_box_s;
    ugfx_fill_rect(s, x, y, n, n, g_colour[i] & 0xFFFFFF);
    ugfx_draw_rect(s, x, y, n, n, UTHEME_OUTLINE);
    if (g_edit == i) {
        ugfx_draw_rect(s, x - 3, y - 3, n + 6, n + 6, UTHEME_ACCENT);
        ugfx_draw_rect(s, x - 2, y - 2, n + 4, n + 4, UTHEME_ACCENT);
    }
    const char *cap = i == 0 ? "Colour 1" : "Colour 2";
    ugfx_draw_string_clipped(s, x, y + n + 4, ugfx_text_width(cap) + 1, cap, UTHEME_TEXT, UTHEME_PANEL_BG);
}

static void on_draw(struct uapp *a, struct uapp_draw *dr) {
    struct ugfx_surface *s = dr->surface;
    layout(s->w, s->h);
    status();
    g_recent.selected = -1;
    uapp_log_layout(a, "paint");
    uapp_logf_layout("paint: layout tool %d colour %08X %08X brush %d shape %d zoom %d\n", g_tool,
                     (unsigned)g_colour[0], (unsigned)g_colour[1], g_brush, (int)g_shape, g_canvas.zoom);
    uapp_logf_layout("paint: layout image %d %d dirty %d path %s\n", g_img.w, g_img.h, dirty(),
                     g_path[0] ? g_path : "-");
    if (!g_panel) return;
    int top = g_canvas.y, bottom = g_sb.y;
    ugfx_fill_rect(s, g_px, top, g_pw, bottom - top, UTHEME_PANEL_BG);
    ugfx_fill_rect(s, g_px - 1, top, 1, bottom - top, UTHEME_SEPARATOR);
    heading(s, g_colour_head_y, "Colour");
    colour_box(s, 0);
    colour_box(s, 1);
    heading(s, g_recent_head_y, g_recent_n ? "Recent" : "Recent -- the colours you paint with");
    heading(s, g_palette_head_y, "Palette");
    heading(s, g_brush_head_y, g_size_text);
}

// --- input ------------------------------------------------------------------

static void on_widget(struct uapp *a, int id, int reason) {
    int code;
    uint32_t rgb;
    if (resize_on_widget(id, reason)) { uapp_redraw(a); return; }
    if (id == ID_RESIZE) { resize_take(); uapp_redraw(a); return; }
    if (id >= ID_NEW_SIZE && id < ID_NEW_SIZE + UUI_SIZEPICK_IDS) {
        // A press moves the dialog's key focus to the field pressed.
        if (reason == UUI_REASON_PRESS && id != ID_NEW_SIZE)
            uui_dialog_focus(&g_newdlg, id == ID_NEW_SIZE + 1 ? &g_sizepick.w_it[1] : &g_sizepick.h_it[1]);
        uui_sizepick_on_widget(&g_sizepick, id);
        uapp_redraw(a);
        return;
    }
    if (id >= ID_PICK && id < ID_PICK + UUI_COLOURPICK_IDS) {
        if (uui_colourpick_on_widget(&g_pick, id)) g_colour[g_edit] = g_pick.rgb | 0xFF000000u;
        uapp_redraw(a);
        return;
    }
    switch (id) {
    case ID_MENU: if ((code = uui_menubar_take_code(&g_menu)) > 0) command(code); break;
    case ID_BAR:  if ((code = uui_toolbar_take_code(&g_bar)) > 0) command(code); break;
    case ID_RAIL: if ((code = uui_toolbar_take_code(&g_rail)) > 0) command(code); break;
    case ID_RECENT:
        if (uui_swatches_take(&g_recent, &rgb)) { g_colour[g_edit] = rgb | 0xFF000000u; sync_pick(); }
        break;
    case ID_PALETTE:
        if (uui_swatches_take(&g_palette, &rgb)) { g_colour[g_edit] = rgb | 0xFF000000u; sync_pick(); }
        break;
    case ID_SIZE:
        if (g_size.selected >= 0) { g_brush = SIZE_PX[g_size.selected]; sync_size_text(); }
        break;
    case ID_SHAPE:
        g_shape = g_shape_seg.selected == 1 ? UPIX_SQUARE : UPIX_ROUND;
        break;
    case ID_ASK:
        if ((code = uui_dialog_take_code(&g_ask)) > 0) ask_answer(code);
        break;
    case ID_NEWDLG:
        // A key went to a field through the dialog: the card follows it.
        if (reason == UUI_REASON_KEY) uui_sizepick_on_widget(&g_sizepick, ID_NEW_SIZE + 1);
        if ((code = uui_dialog_take_code(&g_newdlg)) > 0) new_answer(code);
        break;
    default: break;
    }
    uapp_redraw(a);
}

// THE RIGHT BUTTON, by hand: the second colour, on the canvas and in the
// two swatch grids. A left press reaches here too, after the widgets.
static void on_press(struct uapp *a, int x, int y, unsigned buttons) {
    unsigned b = WIN_MOUSE_BUTTONS(buttons);
    if (uui_dialog_is_open(&g_ask) || uui_dialog_is_open(&g_newdlg) || resize_is_open() ||
        uui_menubar_is_open(&g_menu)) return;
    if (b & 0x1) {
        // The two colour wells choose which one the picker edits.
        for (int i = 0; i < 2 && g_panel; i++)
            if (uui_hit(g_box_x[i], g_box_y, g_box_s, g_box_s, x, y)) { g_edit = i; sync_pick(); uapp_redraw(a); }
        return;
    }
    if (!(b & 0x2)) return;
    int i;
    if (g_panel && (i = uui_swatches_at(&g_palette, x, y)) >= 0) {
        g_colour[1] = PALETTE[i] | 0xFF000000u;
    } else if (g_panel && (i = uui_swatches_at(&g_recent, x, y)) >= 0) {
        g_colour[1] = g_recent_c[i] | 0xFF000000u;
    } else if (uui_hit(g_canvas.x, g_canvas.y, g_canvas.w, g_canvas.h, x, y) && !tools_busy()) {
        int px, py;
        uui_paintcanvas_to_picture(&g_canvas, x, y, &px, &py);
        g_slot_down = 1;
        uui_focus_set(&g_focus, -1);
        tools_pointer(UUI_PAINT_DOWN, px, py, 1);
    } else {
        return;
    }
    if (g_edit == 1) sync_pick();
    uapp_redraw(a);
}

static void on_motion(struct uapp *a, int x, int y, unsigned buttons) {
    if (g_slot_down < 0 || !(WIN_MOUSE_BUTTONS(buttons) & 0x2)) return;
    int px, py;
    uui_paintcanvas_to_picture(&g_canvas, x, y, &px, &py);
    tools_pointer(UUI_PAINT_MOVE, px, py, 1);
    uapp_redraw(a);
}

static void on_release(struct uapp *a, int x, int y, unsigned buttons) {
    (void)buttons;
    if (g_slot_down < 0) return;
    g_slot_down = -1;
    int px, py;
    uui_paintcanvas_to_picture(&g_canvas, x, y, &px, &py);
    tools_pointer(UUI_PAINT_UP, px, py, 1);
    uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    int code;
    if (uui_dialog_is_open(&g_ask)) {
        uui_dialog_key(&g_ask, key);
        if ((code = uui_dialog_take_code(&g_ask)) > 0) ask_answer(code);
        uapp_redraw(a);
        return;
    }
    if (resize_key(key)) { uapp_redraw(a); return; }
    if (uui_dialog_is_open(&g_newdlg)) {
        uui_dialog_key(&g_newdlg, key);
        uui_sizepick_on_widget(&g_sizepick, ID_NEW_SIZE + 1);
        if ((code = uui_dialog_take_code(&g_newdlg)) > 0) new_answer(code);
        uapp_redraw(a);
        return;
    }
    if (uui_menubar_key(&g_menu, key, &code)) {
        if (code > 0) command(code);
        uapp_redraw(a);
        return;
    }
    int ctrl = (mods & KEY_MOD_CTRL) != 0, shift = (mods & KEY_MOD_SHIFT) != 0;
    switch (key) {
    case 0x0E: command(CMD_NEW); return;                             // Ctrl+N
    case 0x0F: command(CMD_OPEN); return;                            // Ctrl+O
    case 0x13: command(shift ? CMD_SAVE_AS : CMD_SAVE); return;      // Ctrl+S
    case 0x12: command(shift ? CMD_CANVAS : CMD_RESIZE); return;     // Ctrl+R
    case 0x1A: command(CMD_UNDO); return;                            // Ctrl+Z
    case 0x19: command(CMD_REDO); return;                            // Ctrl+Y
    default: break;
    }
    if (ctrl) {
        if (key == '+' || key == '=') command(CMD_ZOOM_IN);
        else if (key == '-') command(CMD_ZOOM_OUT);
        else if (key == '0') command(CMD_ZOOM_ACTUAL);
        return;
    }
    // LETTERS ARE TOOLS -- unless a field has them (the hex value).
    if (g_focus.current >= 0) return;
    switch (key) {
    case 'p': set_tool(TOOL_PENCIL); break;
    case 'b': set_tool(TOOL_BRUSH); break;
    case 'e': set_tool(TOOL_ERASER); break;
    case 'f': set_tool(TOOL_FILL); break;
    case 'i': set_tool(TOOL_PICK); break;
    case 'x': command(CMD_SWAP); break;
    case '[': set_brush(SIZE_PX[size_index(g_brush) > 0 ? size_index(g_brush) - 1 : 0]); break;
    case ']': set_brush(SIZE_PX[size_index(g_brush) + 1 < NSIZES ? size_index(g_brush) + 1 : NSIZES - 1]); break;
    default: return;
    }
    uapp_redraw(a);
}

static int on_close(struct uapp *a) {
    if (uapp_question_open(a)) return 0;
    if (!dirty()) return 1;
    ask_then(PEND_QUIT);
    return 0;
}

static void default_size(int *w, int *h) {
    *w = ugfx_char_w() * 110;
    *h = ugfx_char_h() * 40;
}

static void on_open(struct uapp *a) {
    g_app = a;
    set_title();
}

int main(int argc, char **argv) {
    if (!ugfx_font_init()) return 2;
    upixundo_init(&g_undo, 256u << 20);
    uui_paintcanvas_init(&g_canvas, &g_img);
    g_canvas.on_pointer = canvas_pointer;
    uui_menubar_init(&g_menu, menu_bar, (int)(sizeof menu_bar / sizeof menu_bar[0]));
    g_menu.item_flags = item_flags;
    uui_toolbar_init(&g_bar, bar_items, (int)(sizeof bar_items / sizeof bar_items[0]));
    g_bar.item_flags = item_flags;
    g_bar.overflow = 1;
    uui_toolbar_init(&g_rail, rail_items, (int)(sizeof rail_items / sizeof rail_items[0]));
    g_rail.item_flags = item_flags;
    g_rail.vertical = 1;
    g_rail.accent_latch = 1;
    uui_colourpick_init(&g_pick, ID_PICK, g_colour[0] & 0xFFFFFF);
    g_widgets[W_PICK] = uui_colourpick_item(&g_pick);
    g_widgets[W_PICK].name = "picker";
    uui_swatches_init(&g_recent, g_recent_c, 0, RECENT_MAX);
    uui_swatches_init(&g_palette, PALETTE, (int)(sizeof PALETTE / sizeof PALETTE[0]), 10);
    uui_slider_init(&g_size, SIZES, NSIZES);
    uui_segmented_init(&g_shape_seg, SHAPES, 2, 0);
    set_brush(g_brush);
    uui_dialog_init(&g_ask);
    uui_dialog_init(&g_newdlg);
    resize_init();
    g_widgets[sizeof g_widgets / sizeof g_widgets[0] - 1] =
        (struct uui_item){ .ops = &uui_dialog_ops, .widget = resize_dialog(), .id = ID_RESIZE, .name = "resize" };
    struct query_display qd;
    if (sys_query_record(QUERY_DISPLAY, 0, &qd, sizeof qd) >= (int)sizeof qd && qd.width && qd.height) {
        g_presets[0].w = (int)qd.width;
        g_presets[0].h = (int)qd.height;
    }
    uui_statusbar_init(&g_sb);
    g_sb.count = 4;
    g_sb.panes[0].text = g_st_note; g_sb.panes[0].chars = 0;
    g_sb.panes[1].text = g_st_pos;  g_sb.panes[1].chars = 14;
    g_sb.panes[2].text = g_st_size; g_sb.panes[2].chars = 16;
    g_sb.panes[3].text = g_zoom_text; g_sb.panes[3].chars = 7;
    int n = uui_colourpick_focusables(&g_pick, g_ring, 6);
    g_ring[n++] = (struct uui_focusable){ &g_size, &uui_slider_ops };
    g_ring[n++] = (struct uui_focusable){ &g_shape_seg, &uui_segmented_ops };
    uui_focus_init(&g_focus, g_ring, n);
    uui_focus_set(&g_focus, -1);

    if (argc >= 2 && load_picture(argv[1])) {}
    else new_picture(NEW_W, NEW_H, 0xFFFFFFFFu);

    struct uapp_desc desc = {
        .title        = "Paint",
        .app_id       = "paint",
        .on_size      = default_size,
        .flags        = UAPP_RESIZABLE,
        .min_w        = 640,
        .min_h        = 420,
        .widgets      = g_widgets,
        .widget_count = (int)(sizeof g_widgets / sizeof g_widgets[0]),
        .focus        = &g_focus,
        .on_open      = on_open,
        .on_draw      = on_draw,
        .on_widget    = on_widget,
        .on_key       = on_key,
        .on_press     = on_press,
        .on_motion    = on_motion,
        .on_release   = on_release,
        .on_close     = on_close,
    };
    return uapp_run(&desc);
}
