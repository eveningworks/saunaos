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
#include "ui/uui_nameask.h"
#include "ui/uui_scale.h"
#include "ui/uui_tabs.h"
#include "ui/uui_segmented.h"
#include "ui/uui_sizepick.h"
#include "ui/uui_statusbar.h"
#include "ui/uui_swatches.h"
#include "ui/uui_toolbar.h"
#include "ui/uui_togglebar.h"
#include "lib/ufile.h"
#include "query_abi.h"
#include "lib/uimg.h"
#include "lib/uclip.h"
#include "paint/paint.h"

#define PATH_MAX_LEN 256
#define NEW_W 800
#define NEW_H 600
#define RECENT_MAX 10

enum {
    ID_MENU = 1, ID_BAR, ID_RAIL, ID_CANVAS, ID_RECENT, ID_PALETTE, ID_SIZE, ID_KIND, ID_EXTRA,
    ID_STATUS, ID_ASK, ID_NEWDLG, ID_NEW_BG, ID_RESIZE, ID_TEXTFILL,
    ID_PTABS, ID_LBAR, ID_LAYERS, ID_LOPACITY,
    ID_SHGAL, ID_SHOUT, ID_SHFILL, ID_SHWIDTH, ID_SHAA,
    ID_SELKIND, ID_SELMODE, ID_SELTOL, ID_SELACT, ID_CTX,
    ID_PICK = 40,   // .. + UUI_COLOURPICK_IDS
    ID_NEW_SIZE = 50,   // .. + UUI_SIZEPICK_IDS
    ID_FONT = 80,       // .. + UUI_FONTPICK_IDS
};
enum {
    CMD_NEW = 100, CMD_OPEN, CMD_SAVE, CMD_SAVE_AS, CMD_EXIT,
    CMD_UNDO, CMD_REDO,
    CMD_ZOOM_IN, CMD_ZOOM_OUT, CMD_ZOOM_ACTUAL, CMD_ZOOM_FIT, CMD_PANEL,
    CMD_ROT_RIGHT, CMD_ROT_LEFT, CMD_ROT_180, CMD_FLIP_H, CMD_FLIP_V, CMD_RESIZE, CMD_CANVAS,
    CMD_SWAP,
    CMD_LAYER_NEW, CMD_LAYER_DUP, CMD_LAYER_DEL, CMD_LAYER_MERGE, CMD_LAYER_FLATTEN,
    CMD_LAYER_UP, CMD_LAYER_DOWN, CMD_LAYER_RENAME, CMD_LAYER_HIDE,
    CMD_COPY, CMD_PASTE, CMD_PASTE_LAYER, CMD_PASTE_FILE,
    CMD_CUT, CMD_SELECT_ALL, CMD_DESELECT, CMD_INVERT, CMD_SEL_DELETE, CMD_CROP,
    CMD_TOOL = 200,   // + enum paint_tool
    // The unsaved-changes question's answers.
    ASK_SAVE = 300, ASK_DISCARD, ASK_CANCEL,
    ASK_RASTERIZE, ASK_KEEP_TEXT,
    ASK_EXPAND, ASK_PASTE_KEEP, ASK_PASTE_CANCEL,
    NEW_CREATE = 310, NEW_CANCEL,
};

// --- the state paint.h shares ---------------------------------------------

struct upixundo g_undo;
struct uui_paintcanvas g_canvas;
int g_tool = TOOL_BRUSH;
int g_tool_before_pick = TOOL_BRUSH;
uint32_t g_colour[2] = { 0xFF000000u, 0xFFFFFFFFu };
int g_brush_kind = BRUSH_ROUND;
int g_brush_size[BRUSH_COUNT] = { 3, 3, 20, 24, 16 };
int g_soft_hard = 30, g_spray_density = 30;
int g_erase_size = 8, g_erase_hard = 100;
struct uui_fontpick g_fontpick;
int g_text_fill;

// --- the window's own -------------------------------------------------------

static struct uapp *g_app;
static struct uui_menubar g_menu;
static struct uui_toolbar g_bar, g_rail;
static struct uui_colourpick g_pick;
static struct uui_swatches g_recent, g_palette;
// THE BRUSH SECTION (B1 from the mockups): the kinds as a row, then Size
// and the kind's own setting as captioned rows, the colour picker's Hue
// and Saturation style.
static struct uui_togglebar g_kinds;
static const struct uui_toggle kind_items[BRUSH_COUNT] = {
    { 0, "Round brush", "br-round", 0 },   { 0, "Square brush", "br-square", 0 },
    { 0, "Soft brush", "br-soft", 0 },     { 0, "Spray", "br-spray", 0 },
    { 0, "Highlighter", "br-highlighter", 0 },
};
static const char *const KIND_NAMES[BRUSH_COUNT] = { "Round", "Square", "Soft", "Spray", "Highlighter" };
static struct uui_scale g_size, g_extra;
static struct uui_custom g_size_l, g_size_v, g_extra_l, g_extra_v;
static char g_size_val[12], g_extra_val[12];
static const char *g_extra_word = "Hardness";
static struct uui_item g_size_it[3], g_extra_it[3], g_brush_it[2];
static struct uui_layout g_size_row, g_extra_row, g_brush_col;
#define BRUSH_MAX 64
static const int BRUSH_STEPS[] = { 1, 2, 3, 5, 8, 12, 20, 32, 48, 64 };
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
static struct uui_focusable g_ring[10];
static int g_ring_font;   // where the text bar's font list sits in the ring; the size follows

static char g_path[PATH_MAX_LEN];        // "" for a picture never saved
static unsigned g_saved_state;           // upixundo_state() at the last save or open
static int g_panel = 1;                  // the right panel shown
static int g_edit;                       // the colour the picker edits: 0 or 1
static int g_slot_down = -1;             // the right button's stroke, while held
static char g_title[PATH_MAX_LEN + 16];
static char g_zoom_text[12], g_st_pos[32], g_st_size[32], g_st_note[128];
static char g_size_text[24];
static char g_ask_line[PATH_MAX_LEN + 48];
static const char *g_ask_rows[3];

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
static const int SIZE_PX[] = { 1, 2, 3, 5, 8, 12, 20, 32 };
#define NSIZES ((int)(sizeof SIZE_PX / sizeof SIZE_PX[0]))

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
    UUI_MENU_SEP,
    UUI_MENU_ICON("Cut", CMD_CUT, "Ctrl+X", "tb-cut", 0),
    UUI_MENU_ICON("Copy", CMD_COPY, "Ctrl+C", "tb-copy", 0),
    UUI_MENU_ICON("Paste", CMD_PASTE, "Ctrl+V", "tb-paste", 0),
    UUI_MENU("Paste into new layer", CMD_PASTE_LAYER, "Ctrl+Shift+V"),
    UUI_MENU("Paste from file...", CMD_PASTE_FILE, 0),
    UUI_MENU_SEP,
    UUI_MENU("Select all", CMD_SELECT_ALL, "Ctrl+A"),
    UUI_MENU("Deselect", CMD_DESELECT, "Ctrl+D"),
    UUI_MENU("Invert selection", CMD_INVERT, "Ctrl+I"),
    UUI_MENU_ICON("Delete selection", CMD_SEL_DELETE, "Del", "tb-delete", UTHEME_ACT_DANGER),
};
// THE RIGHT-CLICK MENU of the Select tool (L1): Windows 11 Paint's.
static const struct uui_menu_item ctx_items[] = {
    UUI_MENU_ICON("Cut", CMD_CUT, "Ctrl+X", "tb-cut", 0),
    UUI_MENU_ICON("Copy", CMD_COPY, "Ctrl+C", "tb-copy", 0),
    UUI_MENU_ICON("Paste", CMD_PASTE, "Ctrl+V", "tb-paste", 0),
    UUI_MENU_ICON("Delete", CMD_SEL_DELETE, "Del", "tb-delete", UTHEME_ACT_DANGER),
    UUI_MENU_SEP,
    UUI_MENU_ICON("Crop to selection", CMD_CROP, "Ctrl+Shift+X", "tb-crop", 0),
    UUI_MENU("Invert selection", CMD_INVERT, "Ctrl+I"),
    UUI_MENU("Select all", CMD_SELECT_ALL, "Ctrl+A"),
    UUI_MENU("Deselect", CMD_DESELECT, "Ctrl+D"),
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
    UUI_MENU_ICON("Crop to selection", CMD_CROP, "Ctrl+Shift+X", "tb-crop", 0),
    UUI_MENU_SEP,
    UUI_MENU_ICON("Rotate right", CMD_ROT_RIGHT, 0, "tb-rotate-right", 0),
    UUI_MENU_ICON("Rotate left", CMD_ROT_LEFT, 0, "tb-rotate-left", 0),
    UUI_MENU("Rotate 180\xb0", CMD_ROT_180, 0),   // Latin-1, as every string drawn here
    UUI_MENU_SEP,
    UUI_MENU("Flip horizontally", CMD_FLIP_H, 0),
    UUI_MENU("Flip vertically", CMD_FLIP_V, 0),
};
static const struct uui_menu_item layer_items[] = {
    UUI_MENU_ICON("New layer", CMD_LAYER_NEW, "Ctrl+Shift+N", "tb-plus", UTHEME_ACT_CREATE),
    UUI_MENU_ICON("Duplicate layer", CMD_LAYER_DUP, "Ctrl+Shift+D", "tb-copy", 0),
    UUI_MENU_ICON("Delete layer", CMD_LAYER_DEL, 0, "tb-delete", UTHEME_ACT_DANGER),
    UUI_MENU_SEP,
    UUI_MENU_ICON("Merge down", CMD_LAYER_MERGE, "Ctrl+E", "tb-merge-down", 0),
    UUI_MENU("Flatten", CMD_LAYER_FLATTEN, "Ctrl+Shift+F"),
    UUI_MENU_SEP,
    UUI_MENU_ICON("Move layer up", CMD_LAYER_UP, 0, "tb-up", 0),
    UUI_MENU_ICON("Move layer down", CMD_LAYER_DOWN, 0, "tb-down", 0),
    UUI_MENU_SEP,
    UUI_MENU_ICON("Rename layer...", CMD_LAYER_RENAME, 0, "tb-rename", UTHEME_ACT_EDIT),
    UUI_MENU("Hide layer", CMD_LAYER_HIDE, 0),
};
static const struct uui_menu_item tool_items[] = {
    UUI_MENU_ICON("Pencil", CMD_TOOL + TOOL_PENCIL, "P", "tb-pencil", 0),
    UUI_MENU_ICON("Brush", CMD_TOOL + TOOL_BRUSH, "B", "tb-brush", 0),
    UUI_MENU_ICON("Eraser", CMD_TOOL + TOOL_ERASER, "E", "tb-eraser", 0),
    UUI_MENU_ICON("Fill", CMD_TOOL + TOOL_FILL, "F", "tb-fill", 0),
    UUI_MENU_ICON("Colour picker", CMD_TOOL + TOOL_PICK, "I", "tb-picker", 0),
    UUI_MENU_ICON("Text", CMD_TOOL + TOOL_TEXT, "T", "tb-text", 0),
    UUI_MENU_ICON("Shapes", CMD_TOOL + TOOL_SHAPES, "S", "tb-shapes", 0),
    UUI_MENU_ICON("Select", CMD_TOOL + TOOL_SELECT, "M", "tb-select", 0),
};
static const struct uui_menu_item colour_items[] = {
    UUI_MENU("Swap the two colours", CMD_SWAP, "X"),
};
static const struct uui_menu_item menu_bar[] = {
    UUI_SUBMENU("File", file_items),
    UUI_SUBMENU("Edit", edit_items),
    UUI_SUBMENU("View", view_items),
    UUI_SUBMENU("Image", image_items),
    UUI_SUBMENU("Layers", layer_items),
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
    UUI_TOOLBAR_SEP,
    { "tb-select", "Select (M)",        CMD_TOOL + TOOL_SELECT, 0, 0, 0, 0 },
    { "tb-text",   "Text (T)",          CMD_TOOL + TOOL_TEXT,   0, 0, 0, 0 },
    { "tb-shapes", "Shapes (S)",        CMD_TOOL + TOOL_SHAPES, 0, 0, 0, 0 },
};

// THE TEXT BAR (T1 from the mockups): over the open text box, the face
// and a Fill latch, on a card of its own.
static struct uui_custom g_textbar_card;
static int g_textbar_sep_x;    // the divider before Fill
static struct uui_togglebar g_textfill;
static uint32_t g_fill_chip;   // Colour 2, as the Fill toggle shows it
static const struct uui_toggle textfill_items[] = {
    { "Fill", "Fill the box with Colour 2", "tb-fill", &g_fill_chip },
};

// THE PANEL'S TWO TABS (L2 from the mockups): the colours and the
// brush, or the layers -- Krita's dockers, folded into one panel.
static struct uui_tab g_ptab_list[] = { { "Colours & brush", 0, 0 }, { "Layers", 0, 0 } };
static struct uui_tabs g_ptabs;
static struct uui_toolbar g_lbar;
static struct uui_layerlist g_layerlist;
static struct uui_scale g_lopacity;
static struct uui_infonote g_lnote;
static struct uui_nameask g_rename;
static char g_opacity_text[32];
static int g_layers_head_y, g_opacity_head_y;
static const struct uui_toolbar_item lbar_items[] = {
    { "tb-plus",       "New layer (Ctrl+Shift+N)",       CMD_LAYER_NEW,   0, 0, 0, UTHEME_ACT_CREATE },
    { "tb-copy",       "Duplicate layer (Ctrl+Shift+D)", CMD_LAYER_DUP,   0, 0, 0, 0 },
    { "tb-merge-down", "Merge down (Ctrl+E)",            CMD_LAYER_MERGE, 0, 0, 0, 0 },
    { "tb-up",         "Move layer up",                  CMD_LAYER_UP,    0, 0, 0, 0 },
    { "tb-down",       "Move layer down",                CMD_LAYER_DOWN,  0, 0, 0, 0 },
    { "tb-delete",     "Delete layer",                   CMD_LAYER_DEL,   0, UUI_TB_END, 0, UTHEME_ACT_DANGER },
};

// THE SHAPE BAR (S2 from the mockups): the gallery, outline, fill, the
// width and smoothing, on a card over the picture while Shapes is the tool.
// THE SELECT BAR (L1 from the mockups): the kind, how it combines, the
// wand's tolerance, Crop and Delete -- the shape bar's card.
static struct uui_custom g_selbar_card;
static struct uui_togglebar g_selkind;
static struct uui_segmented g_selmode;
static struct uui_dropdown g_seltol;
static struct uui_toolbar g_selact;
static struct uui_menubar g_ctx;          // the right-click menu: no bar of its own
static int g_ctx_armed, g_ctx_x, g_ctx_y;
static const struct uui_toggle selkind_items[SEL_KIND_COUNT] = {
    { 0, "Rectangle", "sel-rect", 0 }, { 0, "Lasso", "sel-lasso", 0 }, { 0, "Magic wand", "sel-wand", 0 },
};
static const char *const SELMODES[] = { "New", "Add", "Subtract" };
static const int SELTOL[] = { 0, 8, 16, 32, 50, 75, 100 };
#define NSELTOL ((int)(sizeof SELTOL / sizeof SELTOL[0]))
static const char *const SELTOL_TEXT[NSELTOL] = { "Tolerance 0%", "Tolerance 8%", "Tolerance 16%", "Tolerance 32%",
                                                 "Tolerance 50%", "Tolerance 75%", "Tolerance 100%" };
static const struct uui_toolbar_item selact_items[] = {
    { "tb-crop",   "Crop to selection (Ctrl+Shift+X)", CMD_CROP,       "Crop",   0, 0, 0 },
    { "tb-delete", "Delete selection (Del)",           CMD_SEL_DELETE, "Delete", UUI_TB_END, 0, UTHEME_ACT_DANGER },
};
static struct uui_custom g_shapebar_card;
static struct uui_togglebar g_shgal, g_shaa;
static struct uui_dropdown g_shout, g_shfill, g_shwidth;
static const struct uui_toggle shgal_items[SHAPE_COUNT] = {
    { 0, "Line", "sh-line", 0 },          { 0, "Curve", "sh-curve", 0 },
    { 0, "Rectangle", "sh-rect", 0 },     { 0, "Rounded rectangle", "sh-rounded", 0 },
    { 0, "Ellipse", "sh-ellipse", 0 },    { 0, "Polygon", "sh-polygon", 0 },
    { 0, "Triangle", "sh-triangle", 0 },  { 0, "Arrow", "sh-arrow", 0 },
    { 0, "Star", "sh-star", 0 },
};
static const struct uui_toggle shaa_items[] = { { "Smooth", "Smooth edges (anti-aliased)", "tb-smooth", 0 } };
static const char *const SHOUT[] = { "No outline", "Outline: Colour 1", "Outline: Colour 2" };
static const char *const SHFILL[] = { "No fill", "Fill: Colour 2", "Fill: Colour 1" };
static const char *const SHWIDTHS[] = { "1 px", "2 px", "3 px", "5 px", "8 px", "12 px", "20 px", "32 px" };

static struct uui_item g_widgets[] = {
    { .ops = &uui_toolbar_ops,       .widget = &g_bar,       .id = ID_BAR,     .name = "bar" },
    { .ops = &uui_toolbar_ops,       .widget = &g_rail,      .id = ID_RAIL,    .name = "rail" },
    { .ops = &uui_paintcanvas_ops,   .widget = &g_canvas,    .id = ID_CANVAS,  .name = "canvas" },
    { 0 },   // the colour picker: uui_colourpick_item(), set in main()
    { .ops = &uui_swatches_ops,      .widget = &g_recent,    .id = ID_RECENT,  .name = "recent" },
    { .ops = &uui_swatches_ops,      .widget = &g_palette,   .id = ID_PALETTE, .name = "palette" },
    { .ops = &uui_layout_ops,        .widget = &g_brush_col, .name = "brushrows" },
    { .ops = &uui_togglebar_ops,     .widget = &g_kinds,     .id = ID_KIND,    .name = "brushkind" },
    { .ops = &uui_statusbar_ops,     .widget = &g_sb,        .id = ID_STATUS,  .name = "status" },
    // The text bar: over the canvas, so after it; hidden with no box open.
    { .ops = &uui_custom_ops,        .widget = &g_textbar_card, .name = "textbar", .hidden = 1 },
    { 0 },   // the font picker: uui_fontpick_item(), set in main()
    { .ops = &uui_togglebar_ops,     .widget = &g_textfill,  .id = ID_TEXTFILL, .name = "textfill", .hidden = 1 },
    { .ops = &uui_tabs_ops,          .widget = &g_ptabs,     .id = ID_PTABS,   .name = "paneltabs" },
    { .ops = &uui_toolbar_ops,       .widget = &g_lbar,      .id = ID_LBAR,    .name = "layerbar", .hidden = 1 },
    { .ops = &uui_layerlist_ops,     .widget = &g_layerlist, .id = ID_LAYERS,  .name = "layers", .hidden = 1 },
    { .ops = &uui_scale_ops,         .widget = &g_lopacity,  .id = ID_LOPACITY, .name = "opacity", .hidden = 1 },
    { .ops = &uui_infonote_ops,      .widget = &g_lnote,     .name = "layernote", .hidden = 1 },
    { .ops = &uui_custom_ops,        .widget = &g_shapebar_card, .name = "shapebar", .hidden = 1 },
    { .ops = &uui_togglebar_ops,     .widget = &g_shgal,     .id = ID_SHGAL,   .name = "shapegallery", .hidden = 1 },
    { .ops = &uui_dropdown_ops,      .widget = &g_shout,     .id = ID_SHOUT,   .name = "shapeoutline", .hidden = 1 },
    { .ops = &uui_dropdown_ops,      .widget = &g_shfill,    .id = ID_SHFILL,  .name = "shapefill", .hidden = 1 },
    { .ops = &uui_dropdown_ops,      .widget = &g_shwidth,   .id = ID_SHWIDTH, .name = "shapewidth", .hidden = 1 },
    { .ops = &uui_togglebar_ops,     .widget = &g_shaa,      .id = ID_SHAA,    .name = "shapesmooth", .hidden = 1 },
    { .ops = &uui_custom_ops,        .widget = &g_selbar_card, .name = "selectbar", .hidden = 1 },
    { .ops = &uui_togglebar_ops,     .widget = &g_selkind,   .id = ID_SELKIND, .name = "selectkind", .hidden = 1 },
    { .ops = &uui_segmented_ops,     .widget = &g_selmode,   .id = ID_SELMODE, .name = "selectmode", .hidden = 1 },
    { .ops = &uui_dropdown_ops,      .widget = &g_seltol,    .id = ID_SELTOL,  .name = "selecttolerance", .hidden = 1 },
    { .ops = &uui_toolbar_ops,       .widget = &g_selact,    .id = ID_SELACT,  .name = "selectactions", .hidden = 1 },
    // The menu bar after the bars: its popups must be on top of them.
    { .ops = &uui_menubar_ops,       .widget = &g_menu,      .id = ID_MENU,    .name = "menu" },
    { .ops = &uui_menubar_ops,       .widget = &g_ctx,       .id = ID_CTX,     .name = "ctxmenu" },
    { .ops = &uui_dialog_ops,        .widget = &g_ask,       .id = ID_ASK,     .name = "ask" },
    { .ops = &uui_dialog_ops,        .widget = &g_newdlg,    .id = ID_NEWDLG,  .name = "newdlg" },
    { 0 },   // Resize: paint_resize.c's dialog, set in main()
};
enum { W_PICK = 3, W_PANEL_FIRST = 3, W_PANEL_LAST = 7, W_TEXTBAR = 9, W_FONTPICK = 10, W_TEXTFILL = 11,
       W_PTABS = 12, W_LAYERS_FIRST = 13, W_LAYERS_LAST = 16, W_SHAPE_FIRST = 17, W_SHAPE_LAST = 22,
       W_SEL_FIRST = 23, W_SEL_LAST = 27 };

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

// The size the panel's Size row sets: the eraser's own while it is the
// tool, otherwise the brush kind's.
static int *size_slot(void) {
    return g_tool == TOOL_ERASER ? &g_erase_size : &g_brush_size[g_brush_kind];
}

// The section from the state: its heading, the two rows' values, and
// which second row the tool has -- Hardness for the soft brush and the
// eraser, Density for the spray, none for the rest.
static void sync_brush(void) {
    int eraser = g_tool == TOOL_ERASER;
    if (eraser) snprintf(g_size_text, sizeof g_size_text, "Eraser");
    else snprintf(g_size_text, sizeof g_size_text, "Brush \xb7 %s", KIND_NAMES[g_brush_kind]);
    int *sz = size_slot();
    if (!g_size.dragging) uui_scale_set_value(&g_size, *sz);
    snprintf(g_size_val, sizeof g_size_val, "%d px", *sz);
    int *extra = eraser ? &g_erase_hard : g_brush_kind == BRUSH_SOFT ? &g_soft_hard
               : g_brush_kind == BRUSH_SPRAY ? &g_spray_density : 0;
    g_brush_it[1].hidden = !extra;
    if (!extra) return;
    g_extra_word = !eraser && g_brush_kind == BRUSH_SPRAY ? "Density" : "Hardness";
    uui_scale_set_range(&g_extra, !eraser && g_brush_kind == BRUSH_SPRAY ? 1 : 0, 100);
    if (!g_extra.dragging) uui_scale_set_value(&g_extra, *extra);
    snprintf(g_extra_val, sizeof g_extra_val, "%d%%", *extra);
}

static void draw_cell(struct ugfx_surface *s, const struct uui_custom *cu) {
    const char *t = cu->state == &g_extra_word ? g_extra_word : (const char *)cu->state;
    ugfx_draw_string_clipped(s, cu->x, cu->y + (cu->h - ugfx_char_h()) / 2, cu->w, t, UTHEME_TEXT,
                             UGFX_TRANSPARENT);
}

static void brush_rows_init(void) {
    // THE CAPTION AND READOUT WIDTHS ARE THE COLOUR PICKER'S, so these
    // scales line up with Hue, Saturation and Lightness above them.
    int ch = ugfx_char_h(), lw = ugfx_text_width("Saturation") + ch / 2, vw = ugfx_text_width("359%") + ch / 2;
    if (ugfx_text_width("64 px") + ch / 2 > vw) vw = ugfx_text_width("64 px") + ch / 2;
    g_size_l = (struct uui_custom){ .w = lw, .h = utheme_control_h(), .draw = draw_cell, .state = (void *)"Size" };
    g_size_v = (struct uui_custom){ .w = vw, .h = utheme_control_h(), .draw = draw_cell, .state = g_size_val };
    g_extra_l = (struct uui_custom){ .w = lw, .h = utheme_control_h(), .draw = draw_cell, .state = &g_extra_word };
    g_extra_v = (struct uui_custom){ .w = vw, .h = utheme_control_h(), .draw = draw_cell, .state = g_extra_val };
    uui_scale_init(&g_size, 1, BRUSH_MAX, 3);
    g_size.step = 1;
    uui_scale_init(&g_extra, 0, 100, 30);
    g_extra.step = 1;
    g_size_it[0] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &g_size_l };
    g_size_it[1] = (struct uui_item){ .ops = &uui_scale_ops, .widget = &g_size, .id = ID_SIZE, .flags = UUI_FILL_W,
                                      .name = "size" };
    g_size_it[2] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &g_size_v };
    g_extra_it[0] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &g_extra_l };
    g_extra_it[1] = (struct uui_item){ .ops = &uui_scale_ops, .widget = &g_extra, .id = ID_EXTRA, .flags = UUI_FILL_W,
                                       .name = "brushextra" };
    g_extra_it[2] = (struct uui_item){ .ops = &uui_custom_ops, .widget = &g_extra_v };
    g_size_row = (struct uui_layout){ .dir = UUI_ROW, .items = g_size_it, .count = 3 };
    g_extra_row = (struct uui_layout){ .dir = UUI_ROW, .items = g_extra_it, .count = 3 };
    g_brush_it[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_size_row, .flags = UUI_FILL_W };
    g_brush_it[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_extra_row, .flags = UUI_FILL_W };
    g_brush_col = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_brush_it, .count = 2 };
    uui_togglebar_init(&g_kinds, kind_items, BRUSH_COUNT);
}

static void sync_pick(void) {
    uui_colourpick_set(&g_pick, g_colour[g_edit] & 0xFFFFFF);
}

static int size_index(int px) {
    for (int i = 0; i < NSIZES; i++) if (SIZE_PX[i] >= px) return i;
    return NSIZES - 1;
}

// [ and ]: the next size down or up the usual steps, Krita's and Photoshop's keys.
static void step_brush(int dir) {
    int *sz = size_slot(), n = (int)(sizeof BRUSH_STEPS / sizeof BRUSH_STEPS[0]), i = 0;
    if (dir > 0) { while (i < n - 1 && BRUSH_STEPS[i] <= *sz) i++; }
    else { i = n - 1; while (i > 0 && BRUSH_STEPS[i] >= *sz) i--; }
    *sz = BRUSH_STEPS[i];
    sync_brush();
}

// --- what paint.h asks of the window -------------------------------------------

void paint_changed(const struct upix_rect *r) {
    layers_update(r);
    set_title();
    if (g_app) uapp_redraw(g_app);
}

static void sync_opacity(void) {
    int pct = (layers_opacity(layers_active()) * 100 + 127) / 255;
    if (!g_lopacity.dragging) uui_scale_set_value(&g_lopacity, pct);
    snprintf(g_opacity_text, sizeof g_opacity_text, "Opacity \xb7 %d%%", pct);
}

void paint_layers_changed(void) {
    static int w, h;
    const struct uimg *v = layers_view();
    g_canvas.img = (struct uimg *)v;
    if (v->w != w || v->h != h) {
        w = v->w;
        h = v->h;
        uui_paintcanvas_picture_changed(&g_canvas);
    }
    g_layerlist.rows = layers_rows();
    g_layerlist.count = layers_count();
    g_layerlist.current = layers_active();
    sync_opacity();
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

void paint_text_changed(void) { if (g_app) uapp_redraw(g_app); }

// A bar's card over the picture: the text bar's and the shape bar's.
static void draw_card(struct ugfx_surface *s, const struct uui_custom *cu) {
    int r = uui_control_radius(0) + 2;
    for (int i = 3; i >= 1; i--)   // a soft shadow, as a menu has
        uui_fill_round_rect(s, cu->x + i, cu->y + i * 2, cu->w, cu->h, r, ugfx_blend(UTHEME_PANEL_BG, 0, 18));
    uui_fill_round_rect(s, cu->x, cu->y, cu->w, cu->h, r, UTHEME_OUTLINE);
    uui_fill_round_rect(s, cu->x + 1, cu->y + 1, cu->w - 2, cu->h - 2, r - 1, UTHEME_WHITE);
}

static void draw_textbar_card(struct ugfx_surface *s, const struct uui_custom *cu) {
    draw_card(s, cu);
    ugfx_fill_rect(s, g_textbar_sep_x, cu->y + cu->h / 4, 1, cu->h / 2, UTHEME_SEPARATOR);
}

void paint_tool_changed(void) {
    // A crosshair to aim with; the fill and the picker act at a point;
    // an I-beam where text goes.
    g_canvas.cursor = g_tool == TOOL_TEXT ? WIN_CURSOR_TEXT : WIN_CURSOR_CROSSHAIR;
    sync_brush();
    if (g_app) uapp_redraw(g_app);
}

// THE SPRAY'S CLOCK: ticks while it is held, and an idle Paint is woken
// once a minute rather than not at all (uapp_set_tick takes no 0).
#define SPRAY_TICK_MS 40
#define IDLE_TICK_MS 60000
void paint_timer(int on) {
    if (g_app) uapp_set_tick(g_app, on ? SPRAY_TICK_MS : IDLE_TICK_MS);
}

static int on_tick(struct uapp *a) {
    (void)a;
    return tools_tick();
}

static void set_tool(int t) {
    if (t < 0 || t >= TOOL_COUNT || tools_busy()) return;
    if (t != TOOL_TEXT) text_commit();
    if (t != TOOL_SHAPES) shapes_commit();
    if (t == TOOL_PICK && g_tool != TOOL_PICK) g_tool_before_pick = g_tool;
    g_tool = t;
    ulogf("paint: tool %d\n", t);
    paint_tool_changed();
}

// --- the picture ------------------------------------------------------------------

// A fresh picture of one colour -- white, MS Paint's default -- with an
// empty history.
static int new_picture(int w, int h, uint32_t bg) {
    struct uimg im = { 0 };
    im.px = malloc((size_t)w * h * 4);
    if (!im.px) { note("Not enough memory for a picture that big"); return 0; }
    for (size_t i = 0; i < (size_t)w * h; i++) im.px[i] = bg;
    im.w = w;
    im.h = h;
    im.has_alpha = (bg >> 24) != 0xFF;
    if (!layers_reset(&im)) { uimg_free(&im); note("Not enough memory for a picture that big"); return 0; }
    g_saved_state = upixundo_state(&g_undo);
    g_path[0] = '\0';
    uui_paintcanvas_set_zoom(&g_canvas, 100, -1, -1);
    set_title();
    return 1;
}

static int is_ora(const char *path) {
    const char *dot = strrchr(k_path_basename(path), '.');
    return dot && !strcasecmp(dot, ".ora");
}

static int load_picture(const char *path) {
    if (is_ora(path)) {
        struct uora_layer *ls;
        int n, w, h;
        char err[160];
        int r = uora_load(path, &ls, &n, &w, &h, err, sizeof err);
        if (r != 0 || n > PAINT_LAYERS_MAX || !layers_reset_many(ls, n)) {
            snprintf(g_st_note, sizeof g_st_note, "Can't open %s: %s", k_path_basename(path),
                     r ? err : "too many layers, or not enough memory");
            ulogf("paint: open %s failed: %d\n", path, r);
            if (r == 0) uora_free(ls, n);
            return 0;
        }
        uora_free(ls, n);   // the pixels went to the stack; the array is let go
    } else {
        struct uimg im = { 0 };
        int r = uimg_load(path, &im);
        if (r != 0 || !layers_reset(&im)) {
            snprintf(g_st_note, sizeof g_st_note, "Can't open %s: %s", k_path_basename(path),
                     r ? uimg_last_error() : "not enough memory");
            ulogf("paint: open %s failed: %d\n", path, r);
            uimg_free(&im);
            return 0;
        }
    }
    g_saved_state = upixundo_state(&g_undo);
    snprintf(g_path, sizeof g_path, "%s", path);
    // Shown whole, never enlarged: a photo opens fitted, an icon at 100%.
    uui_paintcanvas_set_zoom(&g_canvas, uui_paintcanvas_fit_zoom(&g_canvas), -1, -1);
    note("");
    set_title();
    ulogf("paint: opened %s %dx%d, %d layers\n", path, g_layer->w, g_layer->h, layers_count());
    return 1;
}

// The format a path's extension names, if this build writes it.
static const char *write_format(const char *path) {
    const char *dot = strrchr(k_path_basename(path), '.');
    if (!dot) return 0;
    if (!strcasecmp(dot, ".png")) return "png";
    if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")) return "jpeg";
    if (!strcasecmp(dot, ".qoi")) return "qoi";
    if (!strcasecmp(dot, ".ora")) return "ora";
    return 0;
}

static int save_to(const char *path) {
    const char *fmt = write_format(path);
    if (!fmt) { note("Save as .png, .jpg, .qoi or .ora"); return 0; }
    int r;
    char err[160] = "";
    if (!strcmp(fmt, "ora")) {
        static struct uora_layer ls[PAINT_LAYERS_MAX];
        int n = layers_export(ls);
        r = uora_save(path, ls, n, layers_view(), err, sizeof err);
        layers_export_free(ls, n);
    } else {
        r = uimg_save(path, layers_view(), fmt);
    }
    if (r != 0) {
        snprintf(g_st_note, sizeof g_st_note, "Couldn't save %s (%s)", k_path_basename(path), err[0] ? err : "error");
        ulogf("paint: save %s failed: %d %s\n", path, r, err);
        return 0;
    }
    snprintf(g_path, sizeof g_path, "%s", path);
    // A FLAT FILE OF A LAYERED PICTURE IS SAVED, Paint.NET's rule, with a
    // note that only .ora keeps the layers: every typed word is a layer,
    // and a PNG that never counted as saved would ask at every close.
    int flat = strcmp(fmt, "ora") && layers_count() > 1;
    g_saved_state = upixundo_state(&g_undo);
    if (flat) snprintf(g_st_note, sizeof g_st_note, "Saved %s flattened -- .ora keeps the layers", k_path_basename(path));
    else snprintf(g_st_note, sizeof g_st_note, "Saved %s", k_path_basename(path));
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

void paint_ask_rasterize(void) {
    if (uui_dialog_is_open(&g_ask)) return;
    static const struct uui_dialog_button btns[] = {
        { "Make it pixels", ASK_RASTERIZE, UUI_DLG_PRIMARY }, { "Cancel", ASK_KEEP_TEXT, 0 },
    };
    snprintf(g_ask_line, sizeof g_ask_line, "\"%s\" is a text layer. Make it pixels to paint on it?",
             layers_name(layers_active()));
    g_ask_rows[0] = g_ask_line;
    g_ask_rows[1] = "Its words can no longer be edited after that.";
    uui_dialog_set_bounds(&g_ask, 0, 0, uapp_width(g_app), uapp_height(g_app));
    uui_dialog_open(&g_ask, "Paint", g_ask_rows, 2, btns, 2, 0, ASK_KEEP_TEXT);
}

// P2 from the mockups: Paint.NET's three choices.
void paint_ask_paste(const char *name, int w, int h) {
    static const struct uui_dialog_button btns[] = {
        { "Keep canvas size", ASK_PASTE_KEEP, 0 }, { "Expand canvas", ASK_EXPAND, UUI_DLG_PRIMARY },
        { "Cancel", ASK_PASTE_CANCEL, 0 },
    };
    static char line2[96];
    snprintf(g_ask_line, sizeof g_ask_line, "The pasted picture is bigger than the canvas.");
    snprintf(line2, sizeof line2, "%s is %d \xd7 %d; the canvas is %d \xd7 %d.", name, w, h, g_layer->w, g_layer->h);
    g_ask_rows[0] = g_ask_line;
    g_ask_rows[1] = line2;
    uui_dialog_set_bounds(&g_ask, 0, 0, uapp_width(g_app), uapp_height(g_app));
    uui_dialog_open(&g_ask, "Paint", g_ask_rows, 2, btns, 3, 1, ASK_PASTE_CANCEL);
}

static void ask_answer(int code) {
    uui_dialog_close(&g_ask);
    if (code == ASK_EXPAND || code == ASK_PASTE_KEEP || code == ASK_PASTE_CANCEL) {
        paste_answer(code == ASK_EXPAND ? PASTE_EXPAND : code == ASK_PASTE_KEEP ? PASTE_KEEP : PASTE_CANCEL);
        return;
    }
    if (code == ASK_RASTERIZE) { layers_rasterize(layers_active()); return; }
    if (code == ASK_KEEP_TEXT) return;
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

static int rotate_one(const struct uimg *in, struct uimg *out, int index, void *ctx) {
    (void)index;
    return uimg_rotate(in, *(const int *)ctx, out);
}

static void rotate(int quarters) {
    if (!layers_transform(rotate_one, &quarters, "rotate")) note("Not enough memory to rotate");
}

static int flip_one(const struct uimg *in, struct uimg *out, int index, void *ctx) {
    (void)index;
    int horizontal = *(const int *)ctx;
    out->px = malloc((size_t)in->w * in->h * 4);
    if (!out->px) return -1;
    out->w = in->w;
    out->h = in->h;
    for (int y = 0; y < in->h; y++) {
        const uint32_t *src = in->px + (size_t)(horizontal ? y : in->h - 1 - y) * in->w;
        uint32_t *row = out->px + (size_t)y * in->w;
        for (int x = 0; x < in->w; x++) row[x] = src[horizontal ? in->w - 1 - x : x];
    }
    return 0;
}

static void flip(int horizontal) {
    if (!layers_transform(flip_one, &horizontal, "flip")) note("Not enough memory to flip");
}

static void undo_redo(int redo) {
    struct upix_rect r;
    int ok = redo ? upixundo_redo(&g_undo, &r) : upixundo_undo(&g_undo, &r);
    if (!ok) return;
    paint_changed(r.w ? &r : 0);
}

void paint_undo(void) { undo_redo(0); }

// COPY PICTURE: what shows, every layer together -- there is no selection
// yet, so the whole picture, as Windows 11 Paint copies with none.
static void copy_picture(void) {
    if (select_copy(0)) return;   // the selection, when there is one
    const struct uimg *v = layers_view();
    note(v && uclip_set_image(v) ? "Copied the picture" : "The clipboard refused the picture");
}

static void paste_file_chosen(void *ctx, const char *path) {
    (void)ctx;
    if (path) paste_from_file(path, 0);
    if (g_app) uapp_redraw(g_app);
}

static void paste_file_dialog(void) {
    if (uui_filedialog_is_open(&g_fd)) return;
    char dir[PATH_MAX_LEN];
    start_dir(dir, sizeof dir);
    struct uui_filedialog_opts o = {
        .mode = UUI_FILEDIALOG_OPEN, .title = "Paste from file", .start_dir = dir,
        .filters = uui_imagetypes_open, .filter_count = uui_imagetypes_open_count,
    };
    uui_filedialog_open(g_app, &g_fd, &o, paste_file_chosen, 0);
}

static void zoom_to(int z) {
    uui_paintcanvas_set_zoom(&g_canvas, z, -1, -1);
}

// RENAME, in ui/uui_nameask.h's window: the name it has, selected.
static const char *rename_check(void *ctx, const char *name) {
    (void)ctx;
    if (!name[0]) return "A layer needs a name";
    if (strlen(name) >= PAINT_LAYER_NAME) return "That name is too long";
    return 0;
}

static void rename_done(void *ctx, const char *name) {
    layers_rename((int)(long)ctx, name);
    if (g_app) uapp_redraw(g_app);
}

static void rename_open(void) {
    if (uui_nameask_is_open(&g_rename)) return;
    int i = layers_active();
    uui_nameask_open(&g_rename, g_app, "Rename layer", "Name", layers_name(i), "Rename", rename_check,
                     rename_done, (void *)(long)i);
}

static void layer_up(void)   { layers_move(layers_active(), layers_active() + 1); }
static void layer_down(void) { layers_move(layers_active(), layers_active() - 1); }
static void layer_hide(void) { layers_set_hidden(layers_active(), !layers_rows()[layers_active()].hidden); }

// The Layers menu and the panel's buttons, CMD_LAYER_NEW onwards.
static void (*const LAYER_CMD[])(void) = {
    layers_add, layers_duplicate, layers_delete, layers_merge_down, layers_flatten,
    layer_up, layer_down, rename_open, layer_hide,
};
_Static_assert(sizeof LAYER_CMD / sizeof LAYER_CMD[0] == CMD_LAYER_HIDE - CMD_LAYER_NEW + 1,
               "one handler per CMD_LAYER_*, in their order");

// Edit's copy and paste commands, in CMD_COPY's order.
static void paste_plain(void) { paste_from_clipboard(0); }
static void paste_layer(void) { paste_from_clipboard(1); }
static void cut_selection(void) { if (!select_copy(1)) note("Select something to cut"); }
static void (*const EDIT_CMD[])(void) = { copy_picture, paste_plain, paste_layer, paste_file_dialog,
                                         cut_selection, select_all, select_none, select_invert, select_delete,
                                         select_crop };
_Static_assert(sizeof EDIT_CMD / sizeof EDIT_CMD[0] == CMD_CROP - CMD_COPY + 1,
               "one handler per Edit command, in their order");

static void command(int code) {
    if (code >= CMD_TOOL && code < CMD_TOOL + TOOL_COUNT) { set_tool(code - CMD_TOOL); return; }
    if (tools_busy()) return;
    // An open text box is put into the picture before anything else acts
    // on it -- a save, an undo, a rotate -- except looking at it.
    if (code != CMD_ZOOM_IN && code != CMD_ZOOM_OUT && code != CMD_ZOOM_ACTUAL && code != CMD_ZOOM_FIT &&
        code != CMD_PANEL) {
        text_commit();
        shapes_commit();
        paste_commit();
    }
    if (code >= CMD_COPY && code <= CMD_CROP) {
        EDIT_CMD[code - CMD_COPY]();
        if (g_app) uapp_redraw(g_app);
        return;
    }
    if (code >= CMD_LAYER_NEW && code < CMD_LAYER_NEW + (int)(sizeof LAYER_CMD / sizeof LAYER_CMD[0])) {
        LAYER_CMD[code - CMD_LAYER_NEW]();
        if (g_app) uapp_redraw(g_app);
        return;
    }
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
        if (text_active()) text_restyle();
        if (shapes_active()) shapes_restyle();
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
    case CMD_PASTE: case CMD_PASTE_LAYER:
        return uclip_peek_op() == UCLIP_NONE ? UUI_MI_DISABLED : 0;
    case CMD_CUT: case CMD_DESELECT: case CMD_INVERT: case CMD_SEL_DELETE: case CMD_CROP:
        return select_any() ? 0 : UUI_MI_DISABLED;
    case CMD_PANEL: return g_panel ? UUI_MI_CHECKED : 0;
    case CMD_LAYER_DEL:   return layers_count() > 1 ? 0 : UUI_MI_DISABLED;
    case CMD_LAYER_MERGE: return layers_active() > 0 ? 0 : UUI_MI_DISABLED;
    case CMD_LAYER_UP:    return layers_active() < layers_count() - 1 ? 0 : UUI_MI_DISABLED;
    case CMD_LAYER_DOWN:  return layers_active() > 0 ? 0 : UUI_MI_DISABLED;
    case CMD_LAYER_NEW: case CMD_LAYER_DUP:
        return layers_count() < PAINT_LAYERS_MAX ? 0 : UUI_MI_DISABLED;
    case CMD_LAYER_HIDE:  return layers_rows()[layers_active()].hidden ? UUI_MI_CHECKED : 0;
    case CMD_ZOOM_IN:  return g_canvas.zoom >= UUI_PAINT_ZOOM_MAX ? UUI_MI_DISABLED : 0;
    case CMD_ZOOM_OUT: return g_canvas.zoom <= UUI_PAINT_ZOOM_MIN ? UUI_MI_DISABLED : 0;
    default: return 0;
    }
}

// --- the canvas's pointer, both buttons ------------------------------------------

static void canvas_pointer(void *ctx, enum uui_paint_phase ph, int x, int y, unsigned mods) {
    (void)ctx;
    if (g_canvas.hovering) snprintf(g_st_pos, sizeof g_st_pos, "%d, %d px", x, y);
    else g_st_pos[0] = '\0';
    if (ph == UUI_PAINT_DOWN) uui_focus_set(&g_focus, -1);   // typed letters are tools again
    tools_pointer(ph, x, y, 0, mods);
    if (g_app) uapp_redraw(g_app);
}

// R2 from the mockups: an edge handle dragged is a new canvas size, the
// picture kept at the top-left -- MS Paint's handles.
static void canvas_draw_over(void *ctx, struct ugfx_surface *s, const struct uui_paintcanvas *c) {
    (void)ctx;
    text_draw_over(s, c);
    shapes_draw_over(s, c);
    select_draw_over(s, c);
    paste_draw_over(s, c);
}

static void canvas_handle_resize(void *ctx, int w, int h) {
    (void)ctx;
    resize_canvas_to(w, h, 0, 0);
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

    // The text bar, over the open box -- above it, or below when there is
    // no room -- inside the canvas.
    int bx, by, bw, bh;
    int show_bar = text_box(&bx, &by, &bw, &bh);
    for (int i = W_TEXTBAR; i <= W_TEXTFILL; i++) g_widgets[i].hidden = !show_bar;
    if (show_bar) {
        int fw, fh, tw2, th2;
        g_widgets[W_FONTPICK].ops->natural_size(g_widgets[W_FONTPICK].widget, &fw, &fh);
        uui_togglebar_ops.natural_size(&g_textfill, &tw2, &th2);
        g_fill_chip = g_colour[1];
        g_textfill.on = (unsigned)g_text_fill;
        int sep = pad * 2;   // a gap with a divider in it, then Fill at the row's height
        int barw = fw + sep + tw2 + pad, barh = (fh > th2 ? fh : th2) + pad;
        int sx0, sy0, sx1, sy1;
        uui_paintcanvas_to_screen(&g_canvas, bx, by, &sx0, &sy0);
        uui_paintcanvas_to_screen(&g_canvas, bx + bw, by + bh, &sx1, &sy1);
        int x = sx0, y = sy0 - barh - pad;
        if (y < g_canvas.y + 2) y = sy1 + pad;
        if (x + barw > g_canvas.x + g_canvas.w - 4) x = g_canvas.x + g_canvas.w - 4 - barw;
        if (x < g_canvas.x + 4) x = g_canvas.x + 4;
        if (y + barh > g_canvas.y + g_canvas.h - 4) y = g_canvas.y + g_canvas.h - 4 - barh;
        g_textbar_card.x = x; g_textbar_card.y = y; g_textbar_card.w = barw; g_textbar_card.h = barh;
        g_widgets[W_FONTPICK].ops->set_geometry(g_widgets[W_FONTPICK].widget, x + pad / 2, y + (barh - fh) / 2, fw, fh);
        // Fill takes the toggles' own line, so its top and bottom agree.
        uui_togglebar_ops.set_geometry(&g_textfill, x + pad / 2 + fw + sep, g_fontpick.style.y, tw2,
                                       g_fontpick.style.h);
        // Halfway between the last toggle and Fill, from where they ARE.
        g_textbar_sep_x = (g_fontpick.style.x + g_fontpick.style.w + g_textfill.x) / 2;
    }

    // THE SHAPE BAR, at the canvas's top-left while Shapes is the tool.
    int show_shapes = g_tool == TOOL_SHAPES;
    for (int i = W_SHAPE_FIRST; i <= W_SHAPE_LAST; i++) g_widgets[i].hidden = !show_shapes;
    if (show_shapes) {
        int gw, gh, ow, oh, fw, fh, ww, wh, aw, ah;
        uui_togglebar_ops.natural_size(&g_shgal, &gw, &gh);
        uui_dropdown_ops.natural_size(&g_shout, &ow, &oh);
        uui_dropdown_ops.natural_size(&g_shfill, &fw, &fh);
        uui_dropdown_ops.natural_size(&g_shwidth, &ww, &wh);
        uui_togglebar_ops.natural_size(&g_shaa, &aw, &ah);
        int h = gh, gap = pad / 2;
        int barw = pad / 2 + gw + pad + ow + gap + fw + gap + ww + pad + aw + pad / 2, barh = h + pad;
        int x = rw + pad, y = top + pad;
        g_shapebar_card.x = x; g_shapebar_card.y = y; g_shapebar_card.w = barw; g_shapebar_card.h = barh;
        int cx = x + pad / 2, cy = y + pad / 2;
        uui_togglebar_ops.set_geometry(&g_shgal, cx, cy, gw, h);
        cx += gw + pad;
        uui_dropdown_ops.set_geometry(&g_shout, cx, cy, ow, h);
        cx += ow + gap;
        uui_dropdown_ops.set_geometry(&g_shfill, cx, cy, fw, h);
        cx += fw + gap;
        uui_dropdown_ops.set_geometry(&g_shwidth, cx, cy, ww, h);
        cx += ww + pad;
        uui_togglebar_ops.set_geometry(&g_shaa, cx, cy, aw, h);
        g_shgal.on = 1u << g_shape_kind;
        g_shaa.on = (unsigned)g_shape_aa;
    }

    // THE SELECT BAR, in the same place while Select is the tool.
    int show_sel = g_tool == TOOL_SELECT;
    for (int i = W_SEL_FIRST; i <= W_SEL_LAST; i++) g_widgets[i].hidden = !show_sel;
    if (show_sel) {
        int kw, kh, mw, mh, tw, th, aw, ah;
        uui_togglebar_ops.natural_size(&g_selkind, &kw, &kh);
        uui_segmented_ops.natural_size(&g_selmode, &mw, &mh);
        uui_dropdown_ops.natural_size(&g_seltol, &tw, &th);
        uui_toolbar_natural_size(&g_selact, &aw, &ah);
        int h = kh, gap = pad / 2;
        int barw = pad / 2 + kw + pad + mw + gap + tw + pad + aw + pad / 2, barh = h + pad;
        int x = rw + pad, y = top + pad;
        g_selbar_card = (struct uui_custom){ .x = x, .y = y, .w = barw, .h = barh, .draw = draw_card };
        int cx = x + pad / 2, cy = y + pad / 2;
        uui_togglebar_ops.set_geometry(&g_selkind, cx, cy, kw, h);
        cx += kw + pad;
        uui_segmented_set_geometry(&g_selmode, cx, cy + (h - mh) / 2);
        cx += mw + gap;
        uui_dropdown_ops.set_geometry(&g_seltol, cx, cy, tw, h);
        cx += tw + pad;
        uui_toolbar_ops.set_geometry(&g_selact, cx, cy + (h - ah) / 2, aw, ah);
        uui_toolbar_set_bounds(&g_selact, 0, 0, cw, ch);
        g_selkind.on = 1u << g_sel_kind;
        g_selmode.selected = g_sel_mode;
    }
    uui_menubar_set_bounds(&g_ctx, 0, 0, cw, ch);

    g_pw = g_panel ? pad * 30 : 0;
    g_px = cw - g_pw;
    int tab = g_ptabs.selected;
    for (int i = W_PANEL_FIRST; i <= W_PANEL_LAST; i++) g_widgets[i].hidden = !g_panel || tab != 0;
    for (int i = W_LAYERS_FIRST; i <= W_LAYERS_LAST; i++) g_widgets[i].hidden = !g_panel || tab != 1;
    g_widgets[W_PTABS].hidden = !g_panel;
    uui_paintcanvas_ops.set_geometry(&g_canvas, rw, top, g_px - rw - (g_panel ? 1 : 0), bottom - top);
    if (!g_panel) return;

    int x = g_px + pad, w = g_pw - 2 * pad, y = top + pad / 2;
    uui_tabs_set_geometry(&g_ptabs, x, y, w, uui_tabs_height());
    y += uui_tabs_height() + pad;
    if (tab == 1) {
        // LAYERS: the buttons, the stack filling the room, the current
        // layer's opacity, and a line on what saving keeps.
        int tw, th, nw, nh, sw2, sh2;
        g_layers_head_y = y;
        uui_toolbar_natural_size(&g_lbar, &tw, &th);
        uui_toolbar_ops.set_geometry(&g_lbar, x, y, w, th);
        uui_toolbar_set_bounds(&g_lbar, 0, 0, cw, ch);
        y += th + pad / 2;
        uui_infonote_ops.natural_size(&g_lnote, &nw, &nh);
        uui_infonote_ops.set_geometry(&g_lnote, x, bottom - pad - nh, w, nh);
        uui_scale_natural_size(&g_lopacity, &sw2, &sh2);
        int sy = bottom - pad - nh - pad - sh2;
        uui_scale_set_geometry(&g_lopacity, x, sy, w, sh2);
        g_opacity_head_y = sy - lh - pad / 2;
        uui_layerlist_ops.set_geometry(&g_layerlist, x, y, w, g_opacity_head_y - pad - y);
        return;
    }

    // The panel, top to bottom: the two colours, the picker, Recent,
    // the palette, the brush. Headings are on_draw's.
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
    int kw, kh, rw2, rh2;
    g_kinds.on = 1u << g_brush_kind;
    uui_togglebar_ops.natural_size(&g_kinds, &kw, &kh);
    uui_togglebar_ops.set_geometry(&g_kinds, x, y, kw, kh);
    y += kh + pad / 2;
    uui_layout_ops.natural_size(&g_brush_col, &rw2, &rh2);
    uui_layout_ops.set_geometry(&g_brush_col, x, y, w, rh2);
}

static void status(void) {
    snprintf(g_zoom_text, sizeof g_zoom_text, "%d%%", g_canvas.zoom);
    snprintf(g_st_size, sizeof g_st_size, "%d \xd7 %d px", g_layer->w, g_layer->h);
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
    uapp_logf_layout("paint: layout tool %d colour %08X %08X brush %d kind %d zoom %d\n", g_tool,
                     (unsigned)g_colour[0], (unsigned)g_colour[1], *size_slot(), g_brush_kind, g_canvas.zoom);
    uapp_logf_layout("paint: layout brushset hardness %d density %d eraser %d %d\n", g_soft_hard,
                     g_spray_density, g_erase_size, g_erase_hard);
    uapp_logf_layout("paint: layout image %d %d dirty %d path %s\n", g_layer->w, g_layer->h, dirty(),
                     g_path[0] ? g_path : "-");
    text_log_layout();
    shapes_log_layout(&g_canvas);
    paste_log_layout(&g_canvas);
    select_log_layout(&g_canvas);
    uapp_logf_layout("paint: layout layerstack %d current %d opacity %d tab %d text %d\n", layers_count(),
                     layers_active(), layers_opacity(layers_active()), g_ptabs.selected,
                     layers_text(layers_active()) != 0);
    if (!g_panel) return;
    int top = g_canvas.y, bottom = g_sb.y;
    ugfx_fill_rect(s, g_px, top, g_pw, bottom - top, UTHEME_PANEL_BG);
    ugfx_fill_rect(s, g_px - 1, top, 1, bottom - top, UTHEME_SEPARATOR);
    if (g_ptabs.selected == 1) {
        heading(s, g_opacity_head_y, g_opacity_text);
        return;
    }
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
    if (id >= ID_FONT && id < ID_FONT + UUI_FONTPICK_IDS) {
        // THE SIZE IS TAKEN ON ENTER OR A STEPPER, never per keystroke: a
        // half-typed "1" of 12 would be refused and put back.
        int size = id == ID_FONT + 1;
        if (!(size && (reason == UUI_REASON_KEY || g_fontpick.size.field.active))) {
            if (uui_fontpick_on_widget(&g_fontpick, id)) text_restyle();
            // BACK TO THE TEXT once the choice is made, or the next keys
            // would go on into the list or the field instead of the box.
            if (!g_fontpick.family.open && text_active()) uui_focus_set(&g_focus, -1);
        }
        uapp_redraw(a);
        return;
    }
    if (id == ID_TEXTFILL) {
        if (uui_togglebar_take(&g_textfill) == 0) { g_text_fill = !g_text_fill; text_restyle(); }
        uapp_redraw(a);
        return;
    }
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
        if (uui_colourpick_on_widget(&g_pick, id)) {
            g_colour[g_edit] = g_pick.rgb | 0xFF000000u;
            if (text_active()) text_restyle();
        if (shapes_active()) shapes_restyle();   // the open box takes the colour, Windows 11 Paint's
        }
        uapp_redraw(a);
        return;
    }
    if (id >= ID_SELKIND && id <= ID_SELACT) {
        int i;
        if (id == ID_SELKIND && (i = uui_togglebar_take(&g_selkind)) >= 0) g_sel_kind = i;
        else if (id == ID_SELMODE && g_selmode.selected >= 0) g_sel_mode = g_selmode.selected;
        else if (id == ID_SELTOL && (i = uui_dropdown_selected(&g_seltol)) >= 0) g_sel_tol = SELTOL[i];
        else if (id == ID_SELACT && (code = uui_toolbar_take_code(&g_selact)) > 0) command(code);
        uapp_redraw(a);
        return;
    }
    if (id == ID_CTX) {
        if ((code = uui_menubar_take_code(&g_ctx)) > 0) command(code);
        uapp_redraw(a);
        return;
    }
    if (id >= ID_SHGAL && id <= ID_SHAA) {
        int i;
        if (id == ID_SHGAL && (i = uui_togglebar_take(&g_shgal)) >= 0) {
            shapes_commit();   // the next shape is the new kind; the one drawn stays what it is
            g_shape_kind = i;
        } else if (id == ID_SHAA && uui_togglebar_take(&g_shaa) == 0) {
            g_shape_aa = !g_shape_aa;
        } else if (id == ID_SHOUT) {
            g_shape_outline = uui_dropdown_selected(&g_shout);
        } else if (id == ID_SHFILL) {
            g_shape_fill = uui_dropdown_selected(&g_shfill);
        } else if (id == ID_SHWIDTH) {
            int k = uui_dropdown_selected(&g_shwidth);
            if (k >= 0) g_shape_width = SIZE_PX[k];
        }
        if (shapes_active()) shapes_restyle();
        uapp_redraw(a);
        return;
    }
    switch (id) {
    case ID_PTABS: break;   // the tab chosen: layout() reads it
    case ID_LBAR: if ((code = uui_toolbar_take_code(&g_lbar)) > 0) command(code); break;
    case ID_LAYERS: {
        int row, to;
        enum uui_layer_event ev = uui_layerlist_take(&g_layerlist, &row, &to);
        if (ev != UUI_LAYER_NONE && !tools_busy()) text_commit();
        if (ev == UUI_LAYER_PICK) layers_select(row);
        else if (ev == UUI_LAYER_EYE) layers_set_hidden(row, !layers_rows()[row].hidden);
        else if (ev == UUI_LAYER_MOVE) layers_move(row, to);
        else if (ev == UUI_LAYER_RENAME) { layers_select(row); rename_open(); }
        break;
    }
    case ID_LOPACITY:
        // SHOWN AS IT MOVES, KEPT ONCE: a drag is one undo step.
        text_commit();
        layers_opacity_preview((int)((uui_scale_value(&g_lopacity) * 255 + 50) / 100));
        if (reason != UUI_REASON_PRESS && reason != UUI_REASON_MOTION) layers_opacity_commit();
        break;
    case ID_MENU: if ((code = uui_menubar_take_code(&g_menu)) > 0) command(code); break;
    case ID_BAR:  if ((code = uui_toolbar_take_code(&g_bar)) > 0) command(code); break;
    case ID_RAIL: if ((code = uui_toolbar_take_code(&g_rail)) > 0) command(code); break;
    case ID_RECENT:
        if (uui_swatches_take(&g_recent, &rgb)) { g_colour[g_edit] = rgb | 0xFF000000u; sync_pick(); }
        if (text_active()) text_restyle();
        if (shapes_active()) shapes_restyle();
        break;
    case ID_PALETTE:
        if (uui_swatches_take(&g_palette, &rgb)) { g_colour[g_edit] = rgb | 0xFF000000u; sync_pick(); }
        if (text_active()) text_restyle();
        if (shapes_active()) shapes_restyle();
        break;
    case ID_SIZE:
        *size_slot() = (int)uui_scale_value(&g_size);
        sync_brush();
        break;
    case ID_EXTRA: {
        int v = (int)uui_scale_value(&g_extra);
        if (g_tool == TOOL_ERASER) g_erase_hard = v;
        else if (g_brush_kind == BRUSH_SPRAY) g_spray_density = v;
        else g_soft_hard = v;
        sync_brush();
        break;
    }
    case ID_KIND: {
        // A KIND IS A BRUSH: picking one with another tool in hand takes
        // the brush up too -- Windows 11 Paint's Brushes gallery.
        int i = uui_togglebar_take(&g_kinds);
        if (i >= 0 && !tools_busy()) {
            g_brush_kind = i;
            if (g_tool != TOOL_BRUSH) set_tool(TOOL_BRUSH);
            sync_brush();
        }
        break;
    }
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
    // THE SELECT TOOL'S RIGHT BUTTON is its menu, opened on the release.
    if ((b & 0x2) && g_tool == TOOL_SELECT && uui_hit(g_canvas.x, g_canvas.y, g_canvas.w, g_canvas.h, x, y)) {
        if (uui_menubar_is_open(&g_ctx)) { uui_menubar_close(&g_ctx); uapp_redraw(a); return; }
        g_ctx_armed = 1;
        g_ctx_x = x;
        g_ctx_y = y;
        return;
    }
    if (b & 0x1) {
        // The two colour wells choose which one the picker edits.
        for (int i = 0; i < 2 && g_panel && g_ptabs.selected == 0; i++)
            if (uui_hit(g_box_x[i], g_box_y, g_box_s, g_box_s, x, y)) { g_edit = i; sync_pick(); uapp_redraw(a); }
        return;
    }
    if (!(b & 0x2)) return;
    int i;
    int colours = g_panel && g_ptabs.selected == 0;
    if (colours && (i = uui_swatches_at(&g_palette, x, y)) >= 0) {
        g_colour[1] = PALETTE[i] | 0xFF000000u;
    } else if (colours && (i = uui_swatches_at(&g_recent, x, y)) >= 0) {
        g_colour[1] = g_recent_c[i] | 0xFF000000u;
    } else if (uui_hit(g_canvas.x, g_canvas.y, g_canvas.w, g_canvas.h, x, y) && !tools_busy()) {
        int px, py;
        uui_paintcanvas_to_picture(&g_canvas, x, y, &px, &py);
        g_slot_down = 1;
        uui_focus_set(&g_focus, -1);
        tools_pointer(UUI_PAINT_DOWN, px, py, 1, 0);
    } else {
        return;
    }
    if (g_edit == 1) sync_pick();
    if (text_active()) text_restyle();
    if (shapes_active()) shapes_restyle();
    uapp_redraw(a);
}

static void on_motion(struct uapp *a, int x, int y, unsigned buttons) {
    if (g_slot_down < 0 || !(WIN_MOUSE_BUTTONS(buttons) & 0x2)) return;
    int px, py;
    uui_paintcanvas_to_picture(&g_canvas, x, y, &px, &py);
    tools_pointer(UUI_PAINT_MOVE, px, py, 1, 0);
    uapp_redraw(a);
}

static void on_release(struct uapp *a, int x, int y, unsigned buttons) {
    (void)buttons;
    if (g_ctx_armed) {
        g_ctx_armed = 0;
        uui_menubar_open_at(&g_ctx, ctx_items, (int)(sizeof ctx_items / sizeof ctx_items[0]), g_ctx_x, g_ctx_y);
        uapp_redraw(a);
        return;
    }
    if (g_slot_down < 0) return;
    g_slot_down = -1;
    int px, py;
    uui_paintcanvas_to_picture(&g_canvas, x, y, &px, &py);
    tools_pointer(UUI_PAINT_UP, px, py, 1, 0);
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
    // AN OPEN TEXT BOX HAS THE KEYS FIRST, Ctrl+A/C/X/V/Z/Y with them;
    // what it declines (Ctrl+S, a Ctrl+Z with no typing left to undo)
    // is Paint's, below.
    if (text_active() && g_focus.current < 0 && text_key(key, mods)) { uapp_redraw(a); return; }
    if (shapes_active() && shapes_key(key)) { uapp_redraw(a); return; }
    if (paste_active() && paste_key(key)) { uapp_redraw(a); return; }
    if (uui_menubar_key(&g_ctx, key, &code)) {
        if (code > 0) command(code);
        uapp_redraw(a);
        return;
    }
    if (g_focus.current < 0 && select_key(key, mods)) { uapp_redraw(a); return; }
    // CTRL+I IS TAB'S CODE: only the Ctrl held tells them apart.
    if (key == 0x09 && ctrl) { command(CMD_INVERT); return; }
    switch (key) {
    case 0x0E: command(shift ? CMD_LAYER_NEW : CMD_NEW); return;     // Ctrl+N, Ctrl+Shift+N
    case 0x0F: command(CMD_OPEN); return;                            // Ctrl+O
    case 0x13: command(shift ? CMD_SAVE_AS : CMD_SAVE); return;      // Ctrl+S
    case 0x12: command(shift ? CMD_CANVAS : CMD_RESIZE); return;     // Ctrl+R
    case 0x1A: command(CMD_UNDO); return;                            // Ctrl+Z
    case 0x19: command(CMD_REDO); return;                            // Ctrl+Y
    // THE CLIPBOARD'S AND THE SELECTION'S KEYS are a focused field's own
    // (the hex value's Ctrl+A, Ctrl+C), never the picture's as well.
    case 0x03: if (g_focus.current < 0) command(CMD_COPY); return;                            // Ctrl+C
    case 0x18: if (g_focus.current < 0) command(shift ? CMD_CROP : CMD_CUT); return;          // Ctrl+X
    case 0x01: if (g_focus.current < 0) command(CMD_SELECT_ALL); return;                      // Ctrl+A
    case 0x16: if (g_focus.current < 0) command(shift ? CMD_PASTE_LAYER : CMD_PASTE); return; // Ctrl+V
    case 0x05: command(CMD_LAYER_MERGE); return;                     // Ctrl+E, Photoshop's
    case 0x04: if (shift) command(CMD_LAYER_DUP); else if (g_focus.current < 0) command(CMD_DESELECT); return;
    case 0x06: if (shift) { command(CMD_LAYER_FLATTEN); return; } break;   // Ctrl+Shift+F
    default: break;
    }
    if (ctrl) {
        if (key == '+' || key == '=') command(CMD_ZOOM_IN);
        else if (key == '-') command(CMD_ZOOM_OUT);
        else if (key == '0') command(CMD_ZOOM_ACTUAL);
        return;
    }
    // ENTER IN THE TEXT BAR'S SIZE takes the size and gives the keys back
    // to the box.
    if (text_active() && g_focus.current == g_ring_font + 1 && (key == '\r' || key == '\n')) {
        uui_spinbox_commit(&g_fontpick.size);
        text_restyle();
        uui_focus_set(&g_focus, -1);
        uapp_redraw(a);
        return;
    }
    // LETTERS ARE TOOLS -- unless a field has them (the hex value, the
    // text bar's size) or a text box is open, which takes them all.
    if (g_focus.current >= 0) return;
    switch (key) {
    case 'p': set_tool(TOOL_PENCIL); break;
    case 'b': set_tool(TOOL_BRUSH); break;
    case 'e': set_tool(TOOL_ERASER); break;
    case 'f': set_tool(TOOL_FILL); break;
    case 'i': set_tool(TOOL_PICK); break;
    case 't': set_tool(TOOL_TEXT); break;
    case 's': set_tool(TOOL_SHAPES); break;
    case 'm': set_tool(TOOL_SELECT); break;
    case 'x': command(CMD_SWAP); break;
    case '[': step_brush(-1); break;
    case ']': step_brush(1); break;
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
    uui_paintcanvas_init(&g_canvas, 0);
    g_canvas.on_pointer = canvas_pointer;
    g_canvas.on_resize = canvas_handle_resize;
    g_canvas.draw_over = canvas_draw_over;
    uui_fontpick_init(&g_fontpick, ID_FONT, "liberation-sans", 32, 0, 0);
    g_widgets[W_FONTPICK] = uui_fontpick_item(&g_fontpick);
    g_widgets[W_FONTPICK].hidden = 1;
    g_widgets[W_FONTPICK].name = "fontpick";
    uui_togglebar_init(&g_textfill, textfill_items, 1);
    g_textbar_card = (struct uui_custom){ .draw = draw_textbar_card };
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
    brush_rows_init();
    sync_brush();
    g_shapebar_card = (struct uui_custom){ .draw = draw_card };
    g_selbar_card = (struct uui_custom){ .draw = draw_card };
    uui_togglebar_init(&g_selkind, selkind_items, SEL_KIND_COUNT);
    uui_segmented_init(&g_selmode, SELMODES, 3, g_sel_mode);
    uui_dropdown_init(&g_seltol, 0, 0, 0, 0, SELTOL_TEXT, NSELTOL);
    for (int i = 0; i < NSELTOL; i++) if (SELTOL[i] == g_sel_tol) uui_dropdown_set_selected(&g_seltol, i);
    uui_toolbar_init(&g_selact, selact_items, (int)(sizeof selact_items / sizeof selact_items[0]));
    uui_menubar_init(&g_ctx, 0, 0);   // no bar of its own: open_at() supplies the rows
    g_ctx.item_flags = item_flags;
    uui_togglebar_init(&g_shgal, shgal_items, SHAPE_COUNT);
    uui_togglebar_init(&g_shaa, shaa_items, 1);
    uui_dropdown_init(&g_shout, 0, 0, 0, 0, SHOUT, 3);
    uui_dropdown_set_selected(&g_shout, g_shape_outline);
    uui_dropdown_init(&g_shfill, 0, 0, 0, 0, SHFILL, 3);
    uui_dropdown_set_selected(&g_shfill, g_shape_fill);
    uui_dropdown_init(&g_shwidth, 0, 0, 0, 0, SHWIDTHS, NSIZES);
    uui_dropdown_set_selected(&g_shwidth, size_index(g_shape_width));
    uui_tabs_init(&g_ptabs, g_ptab_list, 2, 0);
    g_ptabs.ground = UTHEME_PANEL_BG;
    uui_toolbar_init(&g_lbar, lbar_items, (int)(sizeof lbar_items / sizeof lbar_items[0]));
    g_lbar.item_flags = item_flags;
    g_lbar.overflow = 1;   // a narrow panel: what does not fit goes under a More button
    uui_layerlist_init(&g_layerlist);
    uui_scale_init(&g_lopacity, 0, 100, 100);
    uui_infonote_init(&g_lnote, "PNG and JPEG save it flat; .ora keeps the layers.");
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
    g_ring_font = n;
    n += uui_fontpick_focusables(&g_fontpick, g_ring + n, 8 - n);
    g_ring[n++] = (struct uui_focusable){ &g_size, &uui_scale_focus_ops };
    g_ring[n++] = (struct uui_focusable){ &g_extra, &uui_scale_focus_ops };
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
        .on_tick      = on_tick,
        .tick_ms      = IDLE_TICK_MS,
    };
    return uapp_run(&desc);
}
