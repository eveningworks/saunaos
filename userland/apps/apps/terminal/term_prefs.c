// The GUI Terminal's Options window: terminal.ui's `options #options`,
// its rows bound to term_conf.c's table, and the ones a table cannot say
// -- the scheme gallery, the shell list and the Screen effect page (the
// CRT looks, ui/uui_crtpick.h). A modal window of its own: the
// terminal behind it stays visible, which is what a person picking a
// palette is looking at.
#include "term.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "keyboard.h"
#include "base/usetting.h"
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "base/ulog.h"
#include "base/ugrow.h"
#include "ui/utheme.h"
#include "ui/uui_dropdown.h"
#include "ui/uui_gallery.h"
#include "ui/uui_builder.h"
#include "ui/uui_prefs.h"
#include "ui/uui_route.h"
#include "ui/uui_crtpick.h"

// The Screen effect page's ids start at OPT_LOOKS (UUI_CRTPICK_IDS of them).
enum { OPT_SCHEME = 1, OPT_SHELL, OPT_LOOKS };
static const struct uui_builder_name NAMES[] = { { "scheme", OPT_SCHEME }, { "shell", OPT_SHELL } };

static struct uui_builder *g_ui;
static struct term_conf g_edit;          // what the window shows, until OK
static void (*g_on_commit)(const struct term_conf *next);

static struct uui_gallery g_schemes;
static struct uui_crtpick g_crt;
static struct uui_dropdown *g_shell;

// The schemes, loaded whole so each card can paint its own palette.
// Grown to the folder (base/ugrow.h); each keeps its own capacity.
static char (*g_names)[TERM_NAME_MAX];
static char (*g_labels)[TERM_LABEL_MAX];
static const char **g_label_ptr;
static struct term_scheme *g_sch;
static int g_scheme_count, g_names_cap, g_labels_cap, g_label_ptr_cap, g_sch_cap;

// The shell list: "System default (tosh)", then /etc/shells.
static char (*g_shells)[TERM_SHELL_MAX];
static int g_nshells, g_shells_cap, g_shell_ptr_cap;
static char g_sysdefault[48];
static const char **g_shell_ptr;   // "System default", then g_shells

// A card: a few lines of a real session in that scheme's colours --
// `ls` with its directories in bright cyan, as ls draws them, and the
// caret. VGA indices, because a scheme's `pal` is in VGA order.
static void draw_scheme(struct ugfx_surface *s, int i, int x, int y, int w, int h, void *ctx) {
    (void)ctx;
    if (i < 0 || i >= g_scheme_count) return;
    const struct term_scheme *sc = &g_sch[i];
    uint32_t bg = sc->pal[sc->bg & 15], fg = sc->pal[sc->fg & 15], dir = sc->pal[11];
    uui_fill_round_rect(s, x, y, w, h, ugfx_char_h() / 4, bg);
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_mono(UGFX_FONT_REGULAR));
    int lh = ugfx_char_h(), cw = ugfx_char_w(), px = x + cw / 2, py = y + lh / 3;
    int mw = w - cw;
    ugfx_draw_string_clipped(s, px, py, mw, "/$ ls", fg, bg);
    ugfx_draw_string_clipped(s, px, py + lh, mw, "bin/ etc/ usr/", dir, bg);
    ugfx_draw_string_clipped(s, px, py + 2 * lh, mw, "/$", fg, bg);
    if (py + 3 * lh <= y + h) ugfx_fill_rect(s, px + 3 * cw, py + 2 * lh, cw, lh, sc->cursor);
    ugfx_set_font(was);
}

// A Screen effect card's sample: the same few lines in the scheme chosen
// on the Appearance page, which the picker then runs the look over -- the
// preview is the effect, not a picture of it.
static void draw_look(struct ugfx_surface *s, int x, int y, int w, int h,
                      const struct ucrt_look *look, struct ucrt *c, void *ctx) {
    (void)ctx;
    struct term_scheme fallback;
    const struct term_scheme *sc = &fallback;
    if (g_schemes.selected >= 0 && g_schemes.selected < g_scheme_count) sc = &g_sch[g_schemes.selected];
    else term_scheme_builtin(&fallback);
    uint32_t bg = sc->pal[sc->bg & 15], fg = sc->pal[sc->fg & 15], dir = sc->pal[11];
    ugfx_fill_rect(s, x, y, w, h, bg);
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_mono(UGFX_FONT_REGULAR));
    // CENTRED, not at the corner: a vignette darkens the edges, and the
    // preview should show the look, not only its darkest part.
    int lh = ugfx_char_h(), cw = ugfx_char_w(), m = ucrt_margin(look, w, h);
    int px = x + (w - 14 * cw) / 2, py = y + (h - 3 * lh) / 2;
    if (px < x + m) px = x + m;
    if (py < y + m) py = y + m;
    int mw = x + w - px;
    ugfx_draw_string_clipped(s, px, py, mw, "/$ ls", fg, bg);
    ugfx_draw_string_clipped(s, px, py + lh, mw, "bin/ etc/ usr/", dir, bg);
    ugfx_draw_string_clipped(s, px, py + 2 * lh, mw, "/$", fg, bg);
    if (py + 3 * lh <= y + h) ugfx_fill_rect(s, px + 3 * cw, py + 2 * lh, cw, lh, sc->cursor);
    ugfx_set_font(was);
    c->period = (lh + 3) / 6;
    c->bezel = ugfx_blend(ugfx_rgb(22, 22, 22), bg, 40);
}

static int scheme_index(const char *name) {
    for (int i = 0; i < g_scheme_count; i++)
        if (strcmp(g_names[i], name) == 0) return i;
    return -1;
}

static int shell_index(const char *path) {
    if (!path[0]) return 0;
    for (int i = 0; i < g_nshells; i++)
        if (strcmp(g_shells[i], path) == 0) return 1 + i;
    return 0;
}

// The rows the window cannot bind, from `g_edit` and back.
static void to_controls(void) {
    g_schemes.selected = scheme_index(g_edit.scheme);
    uui_dropdown_set_selected(g_shell, shell_index(g_edit.shell));
    uui_crtpick_set(&g_crt, g_edit.effect, &g_edit.crt);
}

static void from_controls(void) {
    if (g_schemes.selected >= 0 && g_schemes.selected < g_scheme_count)
        strlcpy(g_edit.scheme, g_names[g_schemes.selected], sizeof g_edit.scheme);
    int sh = uui_dropdown_selected(g_shell);
    if (sh >= 1 && sh <= g_nshells) strlcpy(g_edit.shell, g_shells[sh - 1], sizeof g_edit.shell);
    else g_edit.shell[0] = '\0';
    g_edit.effect = g_crt.on;
    g_edit.crt = g_crt.look;
}

int term_prefs_load(void) {
    static const struct uui_builder_prefs TABLES[] = { { &term_conf_prefs, 0 } };
    g_ui = uui_builder_load("terminal", NAMES, (int)(sizeof NAMES / sizeof NAMES[0]));
    g_shell = uui_builder_widget(g_ui, "options", "shell", "dropdown");
    struct uui_item *scheme = uui_builder_item(g_ui, "options", "scheme");
    if (scheme) {
        scheme->ops = &uui_gallery_ops;
        scheme->widget = &g_schemes;
    }
    uui_builder_options(g_ui, "options", TABLES, 1);
    return !uui_builder_error(g_ui);
}

int term_prefs_load_error(void) { return uui_builder_run_error(g_ui); }

static void effect_rows(const char *name, int page) {
    (void)name;   // the file's one `rows`
    uui_crtpick_rows(&g_crt, page, OPT_LOOKS);
}

static void on_close(void) {
    uui_crtpick_free(&g_crt);
}

int term_prefs_is_open(void) { return uui_prefs_is_open(); }

static void on_widget(int id, int reason) {
    (void)reason;
    if (uui_crtpick_changed(&g_crt, id)) {
        g_edit.effect = g_crt.on;
        g_edit.crt = g_crt.look;
    }
}

static void edit_defaults(void *edit) { term_conf_defaults(edit); }
static void on_defaults(void) { to_controls(); }

static void on_ok(void) {
    from_controls();
    if (g_on_commit) g_on_commit(&g_edit);
}

void term_prefs_open(struct uapp *a, const struct term_conf *c,
                     void (*on_commit)(const struct term_conf *next)) {
    if (uui_prefs_is_open()) return;   // one at a time
    g_edit = *c;
    g_on_commit = on_commit;

    g_scheme_count = term_scheme_list(&g_names, &g_names_cap, &g_labels, &g_labels_cap);
    if (!UGROW(g_label_ptr, g_label_ptr_cap, g_scheme_count) || !UGROW(g_sch, g_sch_cap, g_scheme_count))
        g_scheme_count = 0;   // logged below: the gallery is empty rather than wrong
    if (!g_scheme_count) ulogf("terminal: no colour schemes listed\n");
    for (int i = 0; i < g_scheme_count; i++) {
        g_label_ptr[i] = g_labels[i];
        term_scheme_load(g_names[i], &g_sch[i]);
    }
    g_nshells = term_shells_list(&g_shells, &g_shells_cap);
    if (!UGROW(g_shell_ptr, g_shell_ptr_cap, 1 + g_nshells)) g_nshells = 0;
    char sys[64];
    if (!usetting_get("system.shell", sys, sizeof sys) || !sys[0]) strlcpy(sys, "/bin/tosh", sizeof sys);
    const char *base = strrchr(sys, '/');
    snprintf(g_sysdefault, sizeof g_sysdefault, "System default (%s)", base ? base + 1 : sys);
    g_shell_ptr[0] = g_sysdefault;
    for (int i = 0; i < g_nshells; i++) g_shell_ptr[1 + i] = g_shells[i];

    uui_gallery_init(&g_schemes, g_label_ptr, g_scheme_count, 0);
    g_schemes.draw_tile = draw_scheme;
    uui_crtpick_init(&g_crt, g_edit.effect, &g_edit.crt, draw_look, 0);
    uui_dropdown_set_items(g_shell, g_shell_ptr, 1 + g_nshells);
    to_controls();

    struct uui_prefs_desc d = {
        .log_prefix = "options",
        .edit = &g_edit,
        .edit_defaults = edit_defaults,
        .on_defaults = on_defaults,
        .on_ok = on_ok,
        .on_widget = on_widget,
        .on_close = on_close,
    };
    if (!uui_builder_options_open(g_ui, "options", &d, a, NULL, effect_rows)) ulog("uterm: could not open Options\n");
    ulogf("uterm: options open %d scheme(s) %d shell(s)\n", g_scheme_count, g_nshells);
}
