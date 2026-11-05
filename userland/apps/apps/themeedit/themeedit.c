// The Theme Editor: every colour of a theme, its window frame and the
// desktop settings it carries, edited against a live preview that is
// drawn in the theme being edited and left alone by the desktop until
// Apply. Opened by System Settings' Customize... with the theme to edit,
// or from the Start menu on the one in use.
//
// The theme file is userland/libs/ui/utheme_file.h's; the preview is
// ui/uui_themepreview.h, the same drawing as Settings' gallery cards;
// the picker is ui/uui_colourpick.h. A shipped theme is never written
// over: Save on one is Save as.
//
// THE WINDOW IS themeedit.ui, read at start (ui/uui_builder.h): this file
// finds the widgets by name and keeps the behaviour.
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "ui/uapp.h"
#include "ui/uui.h"
#include "ui/uui_label.h"
#include "ui/uui_layout.h"
#include "ui/uui_toolbar.h"
#include "ui/uui_dropdown.h"
#include "ui/uui_builder.h"
#include "ui/uui_stack.h"
#include "ui/uui_themepreview.h"
#include "ui/uui_colourpick.h"
#include "ui/uui_nameask.h"
#include "ui/uui_sbar.h"
#include "ui/utheme_file.h"
#include "ui/ugfx.h"
#include "base/ulog.h"
#include "base/ucolour.h"
#include "base/usetting.h"
#include "keyboard.h"

enum { ID_TOOLBAR = 1, ID_THEMES, ID_CAPTION, ID_ALIGN, ID_PAD, ID_CORNERS, ID_TASKBAR,
       ID_PICK = 20 };
enum { CMD_SAVE = 1, CMD_SAVEAS, CMD_APPLY, CMD_REVERT };

// --- the theme being edited ---------------------------------------------

static struct utheme g_edit, g_orig;
static struct utheme_bundle g_bundle, g_orig_bundle;
static char g_stem[UTHEME_STEM_MAX] = "light";
static char g_status[160];
static char g_title[96];

static int dirty(void) {
    return memcmp(&g_edit, &g_orig, sizeof g_edit) || memcmp(&g_bundle, &g_orig_bundle, sizeof g_bundle);
}

// --- the list on the left: every role, under its group, then the two
// non-colour pages ------------------------------------------------------

#define ROW_FRAME (-1)
#define ROW_DESKTOP (-2)
#define ROW_HEAD (-3)
#define ROWS_MAX 64
struct row { int role; const char *text; };
static struct row g_rows[ROWS_MAX];
static int g_nrows, g_sel = 1, g_top;
static struct uui_custom *g_list;
static struct uui_sbar g_sb;   // the list's scrollbar (ui/uui_sbar.h)

static void build_rows(void) {
    const char *group = "";
    g_nrows = 0;
    for (int r = 0; r < utheme_role_count && g_nrows < ROWS_MAX - 4; r++) {
        if (strcmp(group, utheme_roles[r].group)) {
            group = utheme_roles[r].group;
            g_rows[g_nrows++] = (struct row){ ROW_HEAD, group };
        }
        g_rows[g_nrows++] = (struct row){ r, utheme_roles[r].label };
    }
    g_rows[g_nrows++] = (struct row){ ROW_HEAD, "Shape and desktop" };
    g_rows[g_nrows++] = (struct row){ ROW_FRAME, "Window frame style" };
    g_rows[g_nrows++] = (struct row){ ROW_DESKTOP, "Desktop" };
}

static int row_h(void) { return ugfx_char_h() + utheme_pad(); }

static struct uui_sbar *list_sbar(void) {
    uui_sbar_place(&g_sb, g_list->x + g_list->w - uui_sbar_width(), g_list->y, g_list->h);
    uui_sbar_set(&g_sb, g_nrows, g_list->h / row_h(), g_top);
    g_top = g_sb.top;   // clamped there
    return &g_sb;
}

static void draw_list(struct ugfx_surface *s, const struct uui_custom *c) {
    ugfx_fill_rect(s, c->x, c->y, c->w, c->h, UTHEME_PANEL_BG);
    struct ugfx_clip saved;
    ugfx_clip_save(s, &saved);
    ugfx_clip_intersect(s, c->x, c->y, c->w, c->h);
    int rh = row_h(), pad = utheme_pad(), sw = ugfx_char_h() * 3 / 2;
    int bar = uui_sbar_shown(list_sbar()) ? uui_sbar_width() : 0;
    int lw = c->w - bar;
    for (int i = g_top; i < g_nrows; i++) {
        int y = c->y + (i - g_top) * rh;
        if (y > c->y + c->h) break;
        const struct row *r = &g_rows[i];
        if (r->role == ROW_HEAD) {
            const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
            ugfx_draw_string_clipped(s, c->x + pad, y + (rh - ugfx_char_h()) / 2 + 2, lw - 2 * pad,
                                     r->text, UTHEME_TEXT, UGFX_TRANSPARENT);
            ugfx_set_font(was);
            continue;
        }
        uint32_t bg = UTHEME_PANEL_BG;
        if (i == g_sel) {
            bg = UTHEME_SELECTION;
            uui_fill_round_rect(s, c->x + 2, y + 1, lw - 4, rh - 2, utheme_radius(UTHEME_R_SELECTION), UTHEME_ACCENT);
            uui_fill_round_rect(s, c->x + 3, y + 2, lw - 6, rh - 4, utheme_radius(UTHEME_R_SELECTION), bg);
        }
        int tx = c->x + pad;
        if (r->role >= 0) {
            uint32_t v = *utheme_role_ptr(&g_edit, r->role);
            ugfx_fill_rect(s, tx, y + rh / 4, sw, rh / 2, UTHEME_OUTLINE);
            ugfx_fill_rect(s, tx + 1, y + rh / 4 + 1, sw - 2, rh / 2 - 2, v);
            tx += sw + pad;
        }
        ugfx_draw_string_clipped(s, tx, y + (rh - ugfx_char_h()) / 2, c->x + lw - tx - pad,
                                 r->text, UTHEME_TEXT, bg);
    }
    uui_sbar_draw(&g_sb, s, UTHEME_PANEL_BG, UTHEME_TEXT);
    ugfx_clip_restore(s, &saved);
}

// --- the preview --------------------------------------------------------

static struct uui_themepreview *g_preview;

static int corner_of(const char *preset) {
    if (!strcmp(preset, "square")) return 0;
    if (!strcmp(preset, "round")) return 12;
    if (!strcmp(preset, "medium")) return 10;
    return 8;
}

static void sync_preview(void) {
    g_preview->theme = &g_edit;
    g_preview->taskbar_light = !strcmp(g_bundle.value[1], "light");
    g_preview->corner = corner_of(g_bundle.value[0][0] ? g_bundle.value[0] : "small");
}

// --- the inspector: a colour, the frame, or the desktop ------------------

static struct uui_label *g_ins_title, *g_ins_desc;
static struct uui_colourpick *g_pick;
static struct uui_custom *g_contrast;
static struct uui_stack *g_pages;   // the inspector's three pages
static struct uui_item *g_colour_page, *g_frame_page, *g_desktop_page;

// What a role is read against, and how much contrast that needs: WCAG
// AA's 4.5:1 for text, 3:1 for an edge or an icon a person must find.
struct pair { const char *role, *other, *words; int need10; };
static const struct pair PAIRS[] = {
    { "text", "panel_bg", "on the page", 45 },        { "text", "field_bg", "in a field", 45 },
    { "text", "selection_bg", "on a selection", 45 }, { "text_muted", "panel_bg", "on the page", 45 },
    { "accent", "accent_text", "under its text", 45 }, { "accent", "panel_bg", "as a ring on the page", 30 },
    { "accent_text", "accent", "on the accent", 45 }, { "titlebar_text", "titlebar", "on the title bar", 45 },
    { "titlebar", "titlebar_text", "under the title", 45 },
    { "titlebar_inactive_text", "titlebar_inactive", "on an inactive bar", 30 },
    { "titlebar_inactive", "titlebar_inactive_text", "under an inactive title", 30 },
    { "selection_bg", "text", "under text", 45 },      { "panel_bg", "text", "under text", 45 },
    { "field_bg", "text", "under text", 45 },          { "window_bg", "text", "under text", 45 },
    { "control_bg", "text", "under a label", 45 },     { "bar_bg", "text", "under text", 45 },
    { "chrome", "text", "under text", 45 },            { "outline", "field_bg", "as a field's edge", 30 },
    { "find_hit", "text", "under text", 45 },          { "find_hit_current", "text", "under text", 45 },
    { "tooltip_bg", "tooltip_text", "under its text", 45 },
    { "tooltip_text", "tooltip_bg", "on a tooltip", 45 },
    { "danger_bg", 0, "under white text", 45 },       { "border", "window_bg", "around a window", 30 },
    { "severity_error", "field_bg", "as text", 45 },  { "severity_warning", "field_bg", "as text", 45 },
};

static int role_index(const char *key) {
    for (int r = 0; r < utheme_role_count; r++) if (!strcmp(utheme_roles[r].key, key)) return r;
    return -1;
}

static void draw_contrast(struct ugfx_surface *s, const struct uui_custom *c) {
    int role = g_rows[g_sel].role;
    if (role < 0) return;
    const char *key = utheme_roles[role].key;
    int rh = ugfx_char_h() + 4, y = c->y, any = 0;
    uint32_t me = *utheme_role_ptr(&g_edit, role);
    for (unsigned i = 0; i < sizeof PAIRS / sizeof PAIRS[0]; i++) {
        int need = PAIRS[i].need10;
        if (strcmp(PAIRS[i].role, key) && !(!strncmp(key, "action_", 7) && !strcmp(PAIRS[i].role, "chrome")))
            continue;
        int o = PAIRS[i].other ? role_index(PAIRS[i].other) : -1;
        uint32_t other = o >= 0 ? *utheme_role_ptr(&g_edit, o) : 0xFFFFFF;
        int c10 = ucolour_contrast10(me, other);
        char line[96], verdict[24];
        snprintf(line, sizeof line, "%s", PAIRS[i].words);
        snprintf(verdict, sizeof verdict, "%d.%d:1 %s", c10 / 10, c10 % 10, c10 >= need ? "passes" : "too low");
        ugfx_draw_string_clipped(s, c->x, y, c->w / 2, line, UTHEME_TEXT, UGFX_TRANSPARENT);
        uint32_t ink = c10 >= need ? utheme_action(UTHEME_ACT_CREATE) : utheme_severity(UTHEME_SEV_ERROR);
        ugfx_draw_string_clipped(s, c->x + c->w / 2, y, c->w / 2, verdict, ink, UGFX_TRANSPARENT);
        y += rh;
        any = 1;
    }
    // A command colour is ink on the command bar.
    if (!any && !strncmp(key, "action_", 7)) {
        int c10 = ucolour_contrast10(me, g_edit.chrome);
        char verdict[48];
        snprintf(verdict, sizeof verdict, "On the command bar: %d.%d:1 %s", c10 / 10, c10 % 10,
                 c10 >= 30 ? "passes" : "too low");
        ugfx_draw_string_clipped(s, c->x, y, c->w, verdict, c10 >= 30 ? utheme_action(UTHEME_ACT_CREATE)
                                 : utheme_severity(UTHEME_SEV_ERROR), UGFX_TRANSPARENT);
    }
}

// The frame and desktop pages: three and two dropdowns. Their items are
// themeedit.ui's and are known by their id=, never their place, so the file
// may reorder or relabel them; these are the ids this file means.
static struct uui_builder *g_ui;
static struct uui_dropdown *g_caption, *g_align, *g_pad, *g_corners, *g_taskbar;
static const struct { const char *id; int px; } PADS[] = {
    { "compact", 4 }, { "normal", 8 }, { "roomy", 14 }, { "tall", 20 },
};
static const char *const CORNER_WORDS[] = { "square", "small", "medium", "round" };
static const struct { const char *name; const char *ids[4]; } CHOICES[] = {
    { "caption_buttons", { "discs", "rectangles" } }, { "title_align", { "left", "centred" } },
    { "titlebar_pad", { "compact", "normal", "roomy", "tall" } },
    { "corners", { "square", "small", "medium", "round" } }, { "taskbar", { "dark", "light" } },
};

static void choose(struct uui_dropdown *d, const char *name, const char *id) {
    int i = uui_builder_choice(g_ui, "main", name, id);
    if (i >= 0) uui_dropdown_set_selected(d, i);
}
static const char *chosen(struct uui_dropdown *d, const char *name) {
    return uui_builder_choice_id(g_ui, "main", name, uui_dropdown_selected(d));
}

// --- layout ---------------------------------------------------------------

static struct uui_toolbar *g_tb;
static struct uui_dropdown *g_themes;
static char g_theme_stems[24][UTHEME_STEM_MAX];
static char g_theme_names[24][32];
static const char *g_theme_ptr[24];
static int g_ntheme;

// Save and Revert have something to do only once the theme differs from what was saved.
static unsigned tb_flags(int code) {
    if (code == CMD_SAVE || code == CMD_REVERT) return dirty() ? 0 : UUI_MI_DISABLED;
    return 0;
}

static struct uui_label *g_status_l;

// Which page the inspector shows: the colour picker for a role, the
// frame's or the desktop's dropdowns otherwise.
static void show_page(void) {
    int role = g_rows[g_sel].role;
    int colour = role >= 0, frame = role == ROW_FRAME;
    uui_stack_show(g_pages, colour ? g_colour_page : frame ? g_frame_page : g_desktop_page);
    if (colour) {
        uui_label_set_text(g_ins_title, utheme_roles[role].label);
        uui_label_set_text(g_ins_desc, utheme_roles[role].group);
        uui_colourpick_set(g_pick, *utheme_role_ptr(&g_edit, role));
    } else if (frame) {
        uui_label_set_text(g_ins_title, "Window frame style");
        uui_label_set_text(g_ins_desc, "The title bar's buttons, where its title sits, and how tall it is");
        choose(g_caption, "caption_buttons", g_edit.caption_style == UTHEME_CAPTION_RECT ? "rectangles" : "discs");
        choose(g_align, "title_align", g_edit.title_align == UTHEME_TITLE_CENTER ? "centred" : "left");
        const char *pad = "normal";
        for (unsigned k = 0; k < sizeof PADS / sizeof PADS[0]; k++) if (PADS[k].px == g_edit.titlebar_pad) pad = PADS[k].id;
        choose(g_pad, "titlebar_pad", pad);
    } else {
        uui_label_set_text(g_ins_title, "Desktop");
        uui_label_set_text(g_ins_desc, "Settings the theme sets when it is applied");
        const char *corner = "small";
        for (int k = 0; k < 4; k++) if (!strcmp(g_bundle.value[0], CORNER_WORDS[k])) corner = CORNER_WORDS[k];
        choose(g_corners, "corners", corner);
        choose(g_taskbar, "taskbar", !strcmp(g_bundle.value[1], "light") ? "light" : "dark");
    }
}

// --- loading, saving --------------------------------------------------------

static void set_title(struct uapp *a) {
    snprintf(g_title, sizeof g_title, "%s%s - Theme Editor", g_edit.name, dirty() ? " *" : "");
    if (a) uapp_set_title(a, g_title);
}

static void list_themes(void) {
    g_ntheme = 0;
    static const char *const DIRS[] = { UTHEME_SHIPPED_DIR, UTHEME_LOCAL_DIR };
    for (int d = 0; d < 2; d++) {
        DIR *dir = opendir(DIRS[d]);
        if (!dir) continue;
        struct dirent *e;
        while ((e = readdir(dir)) && g_ntheme < 24) {
            size_t n = strlen(e->d_name);
            if (n < 7 || strcmp(e->d_name + n - 6, ".theme")) continue;
            char stem[UTHEME_STEM_MAX];
            if (n - 6 >= sizeof stem) continue;
            memcpy(stem, e->d_name, n - 6);
            stem[n - 6] = '\0';
            int dup = 0;
            for (int k = 0; k < g_ntheme; k++) if (!strcmp(g_theme_stems[k], stem)) dup = 1;
            if (dup) continue;
            struct utheme t;
            utheme_load(stem, &t, 0);
            strlcpy(g_theme_stems[g_ntheme], stem, sizeof g_theme_stems[0]);
            strlcpy(g_theme_names[g_ntheme], t.name, sizeof g_theme_names[0]);
            g_theme_ptr[g_ntheme] = g_theme_names[g_ntheme];
            g_ntheme++;
        }
        closedir(dir);
    }
    uui_dropdown_set_items(g_themes, g_theme_ptr, g_ntheme);
    for (int k = 0; k < g_ntheme; k++)
        if (!strcmp(g_theme_stems[k], g_stem)) uui_dropdown_set_selected(g_themes, k);
}

static void load(struct uapp *a, const char *stem) {
    if (!utheme_load(stem, &g_orig, &g_orig_bundle)) {
        snprintf(g_status, sizeof g_status, "No theme named %s -- showing Light", stem);
        stem = "light";
        utheme_load(stem, &g_orig, &g_orig_bundle);
    }
    strlcpy(g_stem, stem, sizeof g_stem);
    g_edit = g_orig;
    g_bundle = g_orig_bundle;
    sync_preview();
    show_page();
    set_title(a);
    ulogf("themeedit: editing %s (%s)\n", g_stem, g_edit.name);
}

static struct uui_nameask g_ask;

static int write_to(const char *stem) {
    char path[96];
    snprintf(path, sizeof path, "%s/%s.theme", UTHEME_LOCAL_DIR, stem);
    int ok = utheme_save(path, &g_edit, &g_bundle, "Made on this computer");
    ulogf("themeedit: saved %s %s\n", path, ok ? "ok" : "FAILED");
    if (ok) {
        strlcpy(g_stem, stem, sizeof g_stem);
        g_orig = g_edit;
        g_orig_bundle = g_bundle;
    }
    return ok;
}

static const char *saveas_check(void *ctx, const char *name) {
    (void)ctx;
    char stem[UTHEME_STEM_MAX], path[96];
    if (!utheme_stem_from_name(name, stem, sizeof stem)) return "Use letters and digits";
    if (utheme_path(stem, path, sizeof path)) return "A theme of that name exists";
    return 0;
}

static int g_apply_after_save;

static void saveas_done(void *ctx, const char *name) {
    struct uapp *a = ctx;
    char stem[UTHEME_STEM_MAX];
    utheme_stem_from_name(name, stem, sizeof stem);
    strlcpy(g_edit.name, name, sizeof g_edit.name);
    if (write_to(stem)) {
        snprintf(g_status, sizeof g_status, "Saved as %s", name);
        if (g_apply_after_save && utheme_apply(stem) >= 0)
            snprintf(g_status, sizeof g_status, "Saved as %s and applied to the desktop", name);
    } else {
        snprintf(g_status, sizeof g_status, "Could not save %s", name);
    }
    g_apply_after_save = 0;
    list_themes();
    set_title(a);
    uapp_redraw(a);
}

static void saveas(struct uapp *a) {
    static char suggestion[48];
    snprintf(suggestion, sizeof suggestion, utheme_is_shipped(g_stem) ? "My %s" : "%s copy", g_edit.name);
    uui_nameask_open(&g_ask, a, "Save as new theme", "Name", suggestion, "Save", saveas_check, saveas_done, a);
}

// Save: a shipped theme is never written over, so the first save of
// one asks for a name.
static void save(struct uapp *a) {
    if (utheme_is_shipped(g_stem)) { saveas(a); return; }
    snprintf(g_status, sizeof g_status, write_to(g_stem) ? "Saved" : "Could not save");
    set_title(a);
}

static void apply(struct uapp *a) {
    if (dirty()) {
        if (utheme_is_shipped(g_stem)) { g_apply_after_save = 1; saveas(a); return; }
        if (!write_to(g_stem)) { snprintf(g_status, sizeof g_status, "Could not save"); return; }
    }
    int n = utheme_apply(g_stem);
    snprintf(g_status, sizeof g_status, n >= 0 ? "Applied %s to the desktop" : "Could not apply %s", g_edit.name);
    set_title(a);
}

// --- events -------------------------------------------------------------

static void edited(struct uapp *a) {
    sync_preview();
    set_title(a);
    uapp_redraw(a);
}

static void on_widget(struct uapp *a, int id, int reason) {
    (void)reason;
    int role = g_rows[g_sel].role;
    if (role >= 0 && uui_colourpick_on_widget(g_pick, id)) {
        *utheme_role_ptr(&g_edit, role) = g_pick->rgb;
        // The "none" slots follow the text, as the loader's do.
        g_edit.action[UTHEME_ACT_NONE] = g_edit.severity[UTHEME_SEV_NONE] = g_edit.text;
        edited(a);
        return;
    }
    switch (id) {
    case ID_TOOLBAR: {
        int code = uui_toolbar_take_code(g_tb);
        if (code == CMD_SAVE) save(a);
        else if (code == CMD_SAVEAS) { g_apply_after_save = 0; saveas(a); }
        else if (code == CMD_APPLY) apply(a);
        else if (code == CMD_REVERT) { load(a, g_stem); snprintf(g_status, sizeof g_status, "Reverted"); }
        uapp_redraw(a);
        return;
    }
    case ID_THEMES: {
        int k = uui_dropdown_selected(g_themes);
        if (k < 0 || k >= g_ntheme || !strcmp(g_theme_stems[k], g_stem)) return;
        if (dirty()) {
            // Nothing is thrown away unasked: back to the one being edited.
            for (int j = 0; j < g_ntheme; j++)
                if (!strcmp(g_theme_stems[j], g_stem)) uui_dropdown_set_selected(g_themes, j);
            snprintf(g_status, sizeof g_status, "Save or Revert the changes first");
        } else {
            load(a, g_theme_stems[k]);
        }
        uapp_redraw(a);
        return;
    }
    case ID_CAPTION: case ID_ALIGN: case ID_PAD: case ID_CORNERS: case ID_TASKBAR: {
        // The CHOSEN ITEM'S id, so an edited file's order cannot change a meaning.
        static const char *const NAME[] = { [ID_CAPTION] = "caption_buttons", [ID_ALIGN] = "title_align",
            [ID_PAD] = "titlebar_pad", [ID_CORNERS] = "corners", [ID_TASKBAR] = "taskbar" };
        struct uui_dropdown *dd = id == ID_CAPTION ? g_caption : id == ID_ALIGN ? g_align : id == ID_PAD ? g_pad
                                : id == ID_CORNERS ? g_corners : g_taskbar;
        const char *c = chosen(dd, NAME[id]);
        if (!c) return;
        if (id == ID_CAPTION) g_edit.caption_style = !strcmp(c, "rectangles") ? UTHEME_CAPTION_RECT : UTHEME_CAPTION_DISC;
        else if (id == ID_ALIGN) g_edit.title_align = !strcmp(c, "centred") ? UTHEME_TITLE_CENTER : UTHEME_TITLE_LEFT;
        else if (id == ID_PAD) {
            for (unsigned k = 0; k < sizeof PADS / sizeof PADS[0]; k++)
                if (!strcmp(c, PADS[k].id)) g_edit.titlebar_pad = (uint8_t)PADS[k].px;
        } else strlcpy(g_bundle.value[id == ID_CORNERS ? 0 : 1], c, sizeof g_bundle.value[0]);
        break;
    }
    default: return;
    }
    edited(a);
}

static void select_row(struct uapp *a, int i) {
    if (i < 0 || i >= g_nrows || g_rows[i].role == ROW_HEAD) return;
    g_sel = i;
    int vis = g_list->h / row_h();
    if (g_sel < g_top) g_top = g_sel;
    if (vis > 0 && g_sel >= g_top + vis) g_top = g_sel - vis + 1;
    show_page();
    ulogf("themeedit: selected %s\n", g_rows[g_sel].text);
    uapp_redraw(a);
}

static int g_mx, g_my;

static int in_list(int x, int y) {
    return x >= g_list->x && y >= g_list->y && x < g_list->x + g_list->w && y < g_list->y + g_list->h;
}

static void on_press(struct uapp *a, int x, int y, unsigned buttons) {
    if (!(buttons & 1) || !in_list(x, y)) return;
    int r = uui_sbar_press(list_sbar(), x, y);
    if (r & UUI_SBAR_TOOK) {
        g_top = g_sb.top;
        uapp_redraw(a);
        return;
    }
    select_row(a, g_top + (y - g_list->y) / row_h());
}

static void on_motion(struct uapp *a, int x, int y, unsigned buttons) {
    g_mx = x;
    g_my = y;
    int r = in_list(x, y) || g_sb.grab >= 0 ? uui_sbar_motion(list_sbar(), x, y, buttons)
                                             : uui_sbar_leave(&g_sb);
    if (r & UUI_SBAR_MOVED) g_top = g_sb.top;
    if (r & (UUI_SBAR_MOVED | UUI_SBAR_REDRAW)) uapp_redraw(a);
}

static void on_release(struct uapp *a, int x, int y, unsigned buttons) {
    (void)x; (void)y; (void)buttons;
    if (uui_sbar_release(&g_sb) & UUI_SBAR_REDRAW) uapp_redraw(a);
}

static void on_wheel(struct uapp *a, int notches) {
    if (!in_list(g_mx, g_my)) return;
    int vis = g_list->h / row_h(), top = g_top - notches * 3;   // a notch up is positive
    if (top > g_nrows - vis) top = g_nrows - vis;
    if (top < 0) top = 0;
    g_top = top;
    uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    if ((mods & KEY_MOD_CTRL) && (key == 's' || key == 'S' || key == 19)) { save(a); uapp_redraw(a); return; }
    int step = key == KEY_ARROW_DOWN ? 1 : key == KEY_ARROW_UP ? -1 : 0;
    if (!step) return;
    int i = g_sel + step;
    while (i >= 0 && i < g_nrows && g_rows[i].role == ROW_HEAD) i += step;
    select_row(a, i);
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)d;
    uui_label_set_text(g_status_l, g_status);
    uapp_log_layout(a, "themeedit");
}

static void on_open(struct uapp *a) {
    set_title(a);
}

// What themeedit.ui's names and commands mean here.
static const struct uui_builder_name NAMES[] = {
    { "toolbar", ID_TOOLBAR }, { "themes", ID_THEMES }, { "caption_buttons", ID_CAPTION },
    { "title_align", ID_ALIGN }, { "titlebar_pad", ID_PAD }, { "corners", ID_CORNERS },
    { "taskbar", ID_TASKBAR }, { "pick", ID_PICK },
    { "save", CMD_SAVE }, { "saveas", CMD_SAVEAS }, { "apply", CMD_APPLY }, { "revert", CMD_REVERT },
};

int main(int argc, char **argv) {
    if (!ugfx_font_init()) return 2;
    char stem[UTHEME_STEM_MAX] = "";
    if (argc > 1 && utheme_stem_ok(argv[1])) strlcpy(stem, argv[1], sizeof stem);
    else if (!usetting_get("desktop.theme", stem, sizeof stem) || !stem[0]) strlcpy(stem, "light", sizeof stem);

    struct uui_builder *ui = g_ui = uui_builder_load("themeedit", NAMES, (int)(sizeof NAMES / sizeof NAMES[0]));
    if (!ui) return 1;
    g_tb = uui_builder_widget(ui, "main", "toolbar", "toolbar");
    g_themes = uui_builder_widget(ui, "main", "themes", "dropdown");
    g_list = uui_builder_widget(ui, "main", "roles", "custom");
    g_preview = uui_builder_widget(ui, "main", "preview", "themepreview");
    g_ins_title = uui_builder_widget(ui, "main", "inspector_title", "label");
    g_ins_desc = uui_builder_widget(ui, "main", "inspector_desc", "label");
    g_pick = uui_builder_widget(ui, "main", "pick", "colourpick");
    g_contrast = uui_builder_widget(ui, "main", "contrast", "custom");
    g_caption = uui_builder_widget(ui, "main", "caption_buttons", "dropdown");
    g_align = uui_builder_widget(ui, "main", "title_align", "dropdown");
    g_pad = uui_builder_widget(ui, "main", "titlebar_pad", "dropdown");
    g_corners = uui_builder_widget(ui, "main", "corners", "dropdown");
    g_taskbar = uui_builder_widget(ui, "main", "taskbar", "dropdown");
    g_status_l = uui_builder_widget(ui, "main", "status", "label");
    g_pages = uui_builder_widget(ui, "main", "pages", "stack");
    g_colour_page = uui_builder_page(ui, "main", "pages", "colour_page");
    g_frame_page = uui_builder_page(ui, "main", "pages", "frame_page");
    g_desktop_page = uui_builder_page(ui, "main", "pages", "desktop_page");
    // Every choice this file means must be in the file, asked now so a
    // missing one is the error window rather than a setting that never sets.
    for (unsigned i = 0; i < sizeof CHOICES / sizeof CHOICES[0]; i++)
        for (int k = 0; k < 4 && CHOICES[i].ids[k]; k++) uui_builder_choice(ui, "main", CHOICES[i].name, CHOICES[i].ids[k]);
    if (uui_builder_error(ui)) return uui_builder_run_error(ui);

    // The two areas this file draws, sized from what they draw: the list's
    // rows, the contrast lines' four rows of (char_h + 4).
    int ch = ugfx_char_h(), cw = ugfx_char_w();
    g_list->w = cw * 18;
    g_list->h = ch * 20;
    g_list->draw = draw_list;
    g_contrast->w = cw * 22;
    g_contrast->h = (ch + 4) * 4;
    g_contrast->draw = draw_contrast;
    build_rows();
    uui_sbar_init(&g_sb);
    g_tb->item_flags = tb_flags;
    uui_label_set_text(g_status_l, g_status);

    load(0, stem);
    list_themes();

    struct uapp_desc desc = {
        .title = g_title,
        .on_open = on_open,
        .on_widget = on_widget,
        .on_draw = on_draw,
        .on_press = on_press,
        .on_motion = on_motion,
        .on_release = on_release,
        .on_wheel = on_wheel,
        .on_key = on_key,
    };
    if (!uui_builder_window(ui, "main", &desc)) return uui_builder_run_error(ui);
    return uapp_run(&desc);
}
