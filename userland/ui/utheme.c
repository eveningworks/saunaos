// The ring-3 toolkit's theme -- see ui/utheme.h. One live palette plus
// font-derived metrics, read by every widget through utheme_current()
// and the utheme_*() metric calls.
#include "ui/utheme.h"
#include "ui/ugfx.h"
#include "ui/uui_primitives.h"   // UUI_CAPSULE
#include "ui/utheme_file.h"
#include "lib/usetting.h"
#include <string.h>

static struct utheme g_theme;
static int g_inited;
// The pushed palettes' predecessors, innermost last (utheme_push()).
static struct utheme g_saved[2];
static int g_depth;

// The default (light) palette, filled at runtime because ugfx_rgb() is
// a function, not a constant.
//
// The greys are ordered: field_bg is the brightest thing on a page and
// the page (panel_bg) sits far enough below it that a text box reads as
// a box without its outline -- macOS's and Windows 10's spacing, not
// Plasma's, whose buttons are lighter than the ground; here a face is a
// step DARKER than the page, and bars a step darker again. Hover and
// press derive from these by luminance (uui_state_bg), so nothing else
// moves when a rung does.
void utheme_default(struct utheme *out) {
    out->window_bg   = ugfx_rgb(228, 228, 228);
    out->panel_bg    = ugfx_rgb(236, 236, 236);
    out->control_bg  = ugfx_rgb(220, 220, 226);
    out->field_bg    = ugfx_rgb(255, 255, 255);
    out->bar_bg      = ugfx_rgb(226, 226, 229);
    out->chrome      = ugfx_rgb(232, 232, 234);
    out->text        = ugfx_rgb(20, 20, 20);
    out->border      = ugfx_rgb(60, 60, 60);
    // The selection blue the desktop and widgets already draw with
    // (icon selection, table rows). A role now, so one place owns it.
    out->accent      = ugfx_rgb(70, 110, 160);
    out->accent_text = ugfx_rgb(255, 255, 255);
    // Darker than the strip ground (window_bg) as well as the control
    // face, so a resting tab reads as recessed and the selected one --
    // field_bg -- as raised out of it.
    out->tab_rest    = ugfx_rgb(200, 200, 207);
    // The two the widgets had been spelling out by hand: this outline
    // appeared in eight of them and this selection tint in six.
    out->outline     = ugfx_rgb(150, 155, 165);
    out->selection_bg = ugfx_rgb(205, 220, 240);
    out->separator   = ugfx_rgb(205, 205, 210);
    out->action[UTHEME_ACT_NONE]    = out->text;
    out->action[UTHEME_ACT_NAV]     = ugfx_rgb(47, 93, 156);
    out->action[UTHEME_ACT_VIEW]    = ugfx_rgb(31, 127, 120);
    out->action[UTHEME_ACT_CREATE]  = ugfx_rgb(44, 122, 61);
    out->action[UTHEME_ACT_EDIT]    = ugfx_rgb(107, 75, 184);
    out->action[UTHEME_ACT_DANGER]  = ugfx_rgb(178, 58, 50);
    out->action[UTHEME_ACT_ARRANGE] = ugfx_rgb(199, 116, 40);
    out->action[UTHEME_ACT_MEDIA]   = ugfx_rgb(168, 99, 15);
    out->severity[UTHEME_SEV_NONE]    = out->text;
    out->severity[UTHEME_SEV_ERROR]   = ugfx_rgb(178, 58, 50);
    // Amber DARKER than the arrange orange: that one is icon ink at 3:1,
    // this is text at 4.5:1 on white -- and lighter than the error red,
    // so the two still differ in lightness, not hue alone.
    out->severity[UTHEME_SEV_WARNING] = ugfx_rgb(160, 92, 0);
    out->text_muted   = ugfx_rgb(85, 90, 102);
    out->tooltip_bg   = ugfx_rgb(255, 252, 220);
    out->tooltip_text = out->text;
    out->find_hit     = ugfx_rgb(255, 231, 163);
    out->find_hit_current = ugfx_rgb(245, 184, 76);
    out->danger_bg    = ugfx_rgb(190, 60, 60);
    out->titlebar     = ugfx_rgb(50, 90, 160);
    out->titlebar_text = ugfx_rgb(255, 255, 255);
    out->titlebar_inactive = ugfx_rgb(120, 120, 130);
    out->titlebar_inactive_text = ugfx_rgb(255, 255, 255);
    out->caption_bg   = ugfx_rgb(230, 230, 235);
    out->caption_style = UTHEME_CAPTION_DISC;
    out->title_align  = UTHEME_TITLE_LEFT;
    out->titlebar_pad = 8;
    strlcpy(out->name, "Light", sizeof out->name);
}

uint32_t utheme_action(int role) {
    if (role < 0 || role >= UTHEME_ACT_COUNT) role = UTHEME_ACT_NONE;
    return utheme_current()->action[role];
}

uint32_t utheme_severity(int sev) {
    if (sev < 0 || sev >= UTHEME_SEV_COUNT) sev = UTHEME_SEV_NONE;
    return utheme_current()->severity[sev];
}

// The theme `desktop.theme` names, else the default. A file that is
// missing or unreadable is the default, never an error: the desktop
// has to draw with SOMETHING (utheme_file.h).
void utheme_init(void) {
    char stem[SETTING_ABI_VALUE_MAX] = "";
    if (!usetting_get("desktop.theme", stem, sizeof stem) || !stem[0] ||
        !utheme_load(stem, &g_theme, 0))
        utheme_default(&g_theme);
    g_inited = 1;
}

const struct utheme *utheme_current(void) {
    // Lazy, so a UTHEME_* colour used before uapp_run() still resolves
    // to a palette rather than to zero (which is black -- a silent,
    // ugly failure). Not while a preview has pushed one: the reload
    // would replace the preview's palette mid-paint.
    if (!g_inited && !g_depth) utheme_init();
    return &g_theme;
}

void utheme_set(const struct utheme *t) {
    g_theme = *t;
    g_inited = 1;
}

void utheme_push(const struct utheme *t) {
    if (g_depth >= (int)(sizeof g_saved / sizeof g_saved[0])) return;
    g_saved[g_depth++] = *utheme_current();
    g_theme = *t;
}

void utheme_pop(void) {
    if (g_depth > 0) g_theme = g_saved[--g_depth];
}

// --- metrics: derived from the font, clamped for before-font-up -------

// The line height, or a sane floor when the font is not fetched yet.
static int ch(void) {
    int h = ugfx_char_h();
    return h > 0 ? h : 14;
}

int utheme_indicator(void) { return ch(); }          // box ~ one line tall
int utheme_pad(void)       { return ch() / 2 + 1; }  // ~8 at a 14px line
int utheme_gap(void)       { return utheme_pad(); }
int utheme_border_w(void)  { return 1; }
int utheme_control_h(void) { return ch() + utheme_pad(); }
int utheme_focus_w(void)   { return ugfx_px(2); }   // a stroke stays 1 px; the ring scales

// --- corners and the scrollbar's look (utheme.h) ---------------------------

static const int PRESET[][UTHEME_R_COUNT] = {
    /* square */ { 0, 0, 0, 0, 0 },
    /* small  */ { 4, 8, 8, 4, 3 },                       // Windows 11's
    /* medium */ { 4, 10, 8, 8, UUI_CAPSULE },            // the look before the setting
    /* round  */ { 8, 14, 12, 10, UUI_CAPSULE },
};
static const char *const PRESET_NAME[] = { "square", "small", "medium", "round" };
static const char *const GROUP_KEY[UTHEME_R_COUNT] = {
    "desktop.corners.controls", "desktop.corners.popups", "desktop.corners.windows",
    "desktop.corners.selection", "desktop.corners.scrollbars",
};
static int g_radius[UTHEME_R_COUNT];
static int g_sb_always = 1;
static int g_round_loaded;

static void load_round(void) {
    char v[SETTING_ABI_VALUE_MAX] = "";
    int preset = 1;   // small, the default
    if (usetting_get("desktop.corners", v, sizeof v)) {
        for (int i = 0; i < 4; i++) if (!strcmp(v, PRESET_NAME[i])) preset = i;
        if (!strcmp(v, "custom")) preset = -1;
    }
    for (int g = 0; g < UTHEME_R_COUNT; g++) {
        int px = PRESET[preset < 0 ? 1 : preset][g];
        if (preset < 0 && usetting_get_int(GROUP_KEY[g], &px) && px > 16) px = 16;
        g_radius[g] = px;
    }
    g_sb_always = !(usetting_get("desktop.scrollbars", v, sizeof v) && !strcmp(v, "auto"));
    g_round_loaded = 1;
}

int utheme_radius_preset(const char *preset, enum utheme_round which) {
    if (which < 0 || which >= UTHEME_R_COUNT) return -2;
    for (int i = 0; i < 4; i++)
        if (preset && !strcmp(preset, PRESET_NAME[i])) return PRESET[i][which];
    return -2;
}

// The setting is the radius at 100%; `display.scale` multiplies it.
int utheme_radius(enum utheme_round which) {
    if (!g_round_loaded) load_round();
    if (which < 0 || which >= UTHEME_R_COUNT) return 0;
    return g_radius[which] == UUI_CAPSULE ? UUI_CAPSULE : ugfx_px(g_radius[which]);
}

int utheme_scrollbars_always(void) {
    if (!g_round_loaded) load_round();
    return g_sb_always;
}

void utheme_settings_reload(void) {
    g_round_loaded = 0;
    g_inited = 0;   // the palette too: `desktop.theme` may have moved
}

int utheme_titlebar_h(void) { return ch() + ugfx_px(utheme_current()->titlebar_pad); }
