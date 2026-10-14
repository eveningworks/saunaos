// Appearance > Theme: the gallery's cards, and the four buttons under it
// -- Customize (the Theme Editor), Save as new theme, Import a theme
// file, Delete. The theme FILE is userland/ui/utheme_file.h's; this page
// picks one, copies one and removes one, and Apply writes it with the
// settings it carries (utheme_apply()).
#include "settings_internal.h"
#include "ui/utheme_file.h"
#include "ui/uui_themepreview.h"
#include "ui/uui_filedialog.h"
#include "ui/uui_nameask.h"
#include "rt/sys.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#define SET_THEME "desktop.theme"
#define THEME_EDITOR "/bin/wm/apps/themeedit"

// --- the cards ----------------------------------------------------------

// One loaded theme per card, keyed by its stem: a card redraws on every
// hover, and parsing a file per paint would be the slow part.
#define TILE_CACHE 24
static struct { char stem[UTHEME_STEM_MAX]; struct utheme t; int light_bar; int corner; } g_tile[TILE_CACHE];
static int g_tile_n;

void theme_tiles_reset(void) { g_tile_n = 0; }

static int corner_of(const char *preset) {
    if (!strcmp(preset, "square")) return 0;
    if (!strcmp(preset, "round")) return 12;
    return 8;
}

static int tile_for(const char *stem) {
    for (int i = 0; i < g_tile_n; i++) if (!strcmp(g_tile[i].stem, stem)) return i;
    if (g_tile_n >= TILE_CACHE) return -1;
    int i = g_tile_n++;
    struct utheme_bundle b;
    strlcpy(g_tile[i].stem, stem, sizeof g_tile[i].stem);
    utheme_load(stem, &g_tile[i].t, &b);
    g_tile[i].light_bar = !strcmp(b.value[1], "light");     // desktop.taskbar_theme
    g_tile[i].corner = corner_of(b.value[0][0] ? b.value[0] : "small");
    return i;
}

void theme_tile(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    const struct slot *sl = ctx;
    if (i < 0 || i >= sl->choice_count) return;
    int k = tile_for(sl->choice_raw[i]);
    if (k < 0) return;
    struct uui_themepreview p;
    uui_themepreview_init(&p, &g_tile[k].t);
    p.taskbar_light = g_tile[k].light_bar;
    p.corner = g_tile[k].corner;
    p.x = x; p.y = y; p.w = w; p.h = h;
    uui_themepreview_draw(s, &p);
}

// --- which theme, and the buttons ---------------------------------------

static struct slot *theme_slot(void) {
    for (int i = 0; i < g_slot_count; i++)
        if (g_slot[i].setting >= 0 && !strcmp(g_name[g_slot[i].setting], SET_THEME)) return &g_slot[i];
    return 0;
}

// The one the page shows selected -- staged or applied.
static void picked(char *out, size_t cap) {
    struct slot *sl = theme_slot();
    const char *v = sl ? staged_value(sl) : 0;
    if (v && v[0]) strlcpy(out, v, cap);
    else if (!usetting_get(SET_THEME, out, cap) || !out[0]) strlcpy(out, "light", cap);
}

static struct uui_button g_customize, g_saveas, g_import, g_delete;
static struct uui_item g_row_items[4];
static struct uui_layout g_row;

int theme_emit_after(struct uui_item *out, int n, int i, struct uui_focusable *focus, int *fc) {
    if (g_slot[i].setting < 0 || strcmp(g_name[g_slot[i].setting], SET_THEME)) return n;
    char stem[UTHEME_STEM_MAX];
    picked(stem, sizeof stem);
    uui_button_init(&g_customize, 0, 0, 0, 0, "Customize...", UUI_ROLE(accent), UUI_ROLE(accent_text), ID_TH_CUSTOMIZE);
    uui_button_init(&g_saveas, 0, 0, 0, 0, "Save as new theme...", UUI_ROLE(control_bg), UUI_ROLE(text), ID_TH_SAVEAS);
    uui_button_init(&g_import, 0, 0, 0, 0, "Import a theme file...", UUI_ROLE(control_bg), UUI_ROLE(text), ID_TH_IMPORT);
    uui_button_init(&g_delete, 0, 0, 0, 0, "Delete", UUI_ROLE(control_bg), UUI_ROLE(text), ID_TH_DELETE);
    // A shipped theme is not this page's to delete.
    g_delete.disabled = utheme_is_shipped(stem);
    struct uui_button *b[4] = { &g_customize, &g_saveas, &g_import, &g_delete };
    static const char *const NAMES[4] = { "theme_customize", "theme_saveas", "theme_import", "theme_delete" };
    for (int k = 0; k < 4; k++) {
        g_row_items[k] = (struct uui_item){ .ops = &uui_button_ops, .widget = b[k], .id = b[k]->code,
                                            .name = NAMES[k] };
        focus[(*fc)++] = (struct uui_focusable){ b[k], &uui_button_ops };
    }
    g_row = (struct uui_layout){ .dir = UUI_ROW, .items = g_row_items, .count = 4 };
    out[n++] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_row };
    return n;
}

// Apply: a theme is its colours AND the settings it carries, so it goes
// through utheme_apply() rather than the registry's plain SET.
int theme_apply_slot(const struct slot *sl, int *changed) {
    if (sl->setting < 0 || strcmp(g_name[sl->setting], SET_THEME)) return 0;
    char stem[UTHEME_STEM_MAX];
    strlcpy(stem, staged_value((struct slot *)sl), sizeof stem);
    int n = utheme_apply(stem);
    ulogf("settings: theme %s applied with %d setting(s)\n", stem, n);
    if (n >= 0) (*changed)++;
    return 1;
}

// --- Save as new theme: a name, then a copy in /etc/themes --------------

static struct uui_nameask g_ask_name;

static const char *saveas_check(void *ctx, const char *name) {
    (void)ctx;
    char stem[UTHEME_STEM_MAX], path[96];
    if (!utheme_stem_from_name(name, stem, sizeof stem)) return "Use letters and digits";
    if (utheme_path(stem, path, sizeof path)) return "A theme of that name exists";
    return 0;
}

static void saveas_done(void *ctx, const char *name) {
    (void)ctx;
    char stem[UTHEME_STEM_MAX], path[96], from[UTHEME_STEM_MAX];
    utheme_stem_from_name(name, stem, sizeof stem);
    picked(from, sizeof from);
    struct utheme t;
    struct utheme_bundle b;
    utheme_load(from, &t, &b);
    strlcpy(t.name, name, sizeof t.name);
    snprintf(path, sizeof path, "%s/%s.theme", UTHEME_LOCAL_DIR, stem);
    int ok = utheme_save(path, &t, &b, "Made on this computer");
    ulogf("settings: theme saved as %s %s\n", path, ok ? "ok" : "FAILED");
    snprintf(g_status, sizeof g_status, ok ? "Saved as %s" : "Could not save %s", name);
    theme_tiles_reset();
    reload_settings();
    if (g_page_group >= 0) open_group(g_page_group);
}

static void saveas_open(struct uapp *a) {
    char from[UTHEME_STEM_MAX];
    picked(from, sizeof from);
    struct utheme t;
    utheme_load(from, &t, 0);
    static char suggestion[48];
    snprintf(suggestion, sizeof suggestion, "My %s", t.name);
    uui_nameask_open(&g_ask_name, a, "Save as new theme", "Name", suggestion, "Save",
                     saveas_check, saveas_done, a);
}

// --- Import a theme file ------------------------------------------------

static struct uui_filedialog g_fd;

static int keep_themes(void *ctx, const char *dir, const struct sys_dirent *e) {
    (void)ctx; (void)dir;
    if (e->is_dir) return 0;
    size_t n = strlen(e->name);
    return n > 6 && !strcmp(e->name + n - 6, ".theme");
}

static void import_chosen(void *ctx, const char *path) {
    struct uapp *a = ctx;
    if (!path) return;
    // Parsed, then WRITTEN by the writer: what lands in /etc/themes is a
    // complete, well-formed file whatever the source carried.
    struct utheme t;
    struct utheme_bundle b;
    int bad = 0;
    char stem[UTHEME_STEM_MAX], dst[96];
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char name[64];
    strlcpy(name, base, sizeof name);
    char *dot = strrchr(name, '.');
    if (dot) *dot = '\0';
    if (!utheme_load_path(path, &t, &b, &bad) || !utheme_stem_from_name(name, stem, sizeof stem)) {
        snprintf(g_status, sizeof g_status, "%s is not a theme file", base);
    } else if (utheme_path(stem, dst, sizeof dst)) {
        snprintf(g_status, sizeof g_status, "A theme named %s is already here", stem);
    } else {
        snprintf(dst, sizeof dst, "%s/%s.theme", UTHEME_LOCAL_DIR, stem);
        int ok = utheme_save(dst, &t, &b, "Imported");
        if (!ok) snprintf(g_status, sizeof g_status, "Could not write %s", dst);
        else if (bad) snprintf(g_status, sizeof g_status, "Imported %s -- %d colour(s) were not valid and use Light's", t.name, bad);
        else snprintf(g_status, sizeof g_status, "Imported %s", t.name);
        ulogf("settings: theme import %s -> %s %s bad %d\n", path, dst, ok ? "ok" : "FAILED", bad);
        theme_tiles_reset();
        reload_settings();
        if (g_page_group >= 0) open_group(g_page_group);
    }
    uapp_redraw(a);
}

static void import_open(struct uapp *a) {
    if (uui_filedialog_is_open(&g_fd)) return;
    static const struct uui_filedialog_filter types[] = {
        { "Theme files", keep_themes, 0, 0 },
        UUI_FILEDIALOG_ALL_FILES,
    };
    struct uui_filedialog_opts o = {
        .mode = UUI_FILEDIALOG_OPEN,
        .title = "Import a theme file",
        .start_dir = "/home",
        .filters = types,
        .filter_count = (int)(sizeof types / sizeof types[0]),
    };
    uui_filedialog_open(a, &g_fd, &o, import_chosen, a);
}

// --- Delete -------------------------------------------------------------

static void delete_picked(void) {
    char stem[UTHEME_STEM_MAX], path[96];
    picked(stem, sizeof stem);
    if (utheme_is_shipped(stem)) return;
    snprintf(path, sizeof path, "%s/%s.theme", UTHEME_LOCAL_DIR, stem);
    char live[UTHEME_STEM_MAX] = "";
    usetting_get(SET_THEME, live, sizeof live);
    int ok = unlink(path) == 0;
    // The theme in use goes back to Light rather than to a name nothing
    // answers to.
    if (ok && !strcmp(live, stem)) utheme_apply("light");
    snprintf(g_status, sizeof g_status, ok ? "Deleted %s" : "Could not delete %s", stem);
    ulogf("settings: theme delete %s %s\n", path, ok ? "ok" : "FAILED");
    theme_tiles_reset();
    reload_settings();
    if (g_page_group >= 0) open_group(g_page_group);
}

int theme_on_action(struct uapp *a, int code) {
    char stem[UTHEME_STEM_MAX];
    switch (code) {
    case ID_TH_CUSTOMIZE:
        picked(stem, sizeof stem);
        uapp_spawn(a, THEME_EDITOR, stem);
        return 1;
    case ID_TH_SAVEAS:
        saveas_open(a);
        return 1;
    case ID_TH_IMPORT:
        import_open(a);
        return 1;
    case ID_TH_DELETE:
        delete_picked();
        return 1;
    default:
        return 0;
    }
}
