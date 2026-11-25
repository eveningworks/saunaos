// DOOM's front end: everything in its window that is not the game.
//
//   the launcher page   before the game -- the data being played, its
//                       title picture, Start, and the keys (mockup H3)
//   the game-data card  no WAD, or "Change game data...": pick one and
//                       download it, or use a file (mockup W1) -- after
//                       its licence: agreed to for id's, read for BSD (L2).
//                       ui/uui_datacard.h draws and runs it.
//   the key sheet       F1 over a paused game; F1 again is DOOM's own
//                       help, which the sheet would otherwise hide (H1)
#include "doom_internal.h"
#include "third_party/doom/glue/dg_saunaos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "keyboard.h"
#include "kpath.h"
#include "base/ulog.h"
#include "base/uconf.h"
#include "base/udatadir.h"
#include "base/ufileop.h"
#include "ui/utheme.h"
#include "ui/uui_checkbox.h"
#include "ui/uui_datacard.h"
#include "ui/uui_filedialog.h"
#include "ui/uui_keysheet.h"
#include "ui/uui_primitives.h"
#include "uhttp.h"
#include "base/udirs.h"

enum view { V_GAME, V_PAGE, V_DATA };     // V_DATA: the card has the window
static enum view g_view = V_GAME;
static int g_help;
static const struct udata_item *g_cur;    // the page's, then the game's

// --- the keys ----------------------------------------------------------

static const struct uui_keysheet_row MOVE[] = {
    { "Walk", "[Up] [Down]" },
    { "Turn", "[Left] [Right]" },
    { "Strafe", "[,] [.]" },
    { "Strafe, held", "[Alt]+[Left] [Right]" },
    { "Run", "hold [Shift]" },
};
static const struct uui_keysheet_row FIGHT[] = {
    { "Fire", "[Ctrl]" },
    { "Use, open doors", "[Space]" },
    { "Weapon", "[1]-[7]" },
    { "Map", "[Tab]" },
};
static const struct uui_keysheet_row GAME[] = {
    { "Menu", "[Esc]" },
    { "Save, load", "[F2] [F3]" },
    { "Quick save, load", "[F6] [F9]" },
    { "Pause", "[Pause]" },
};
static const struct uui_keysheet_row WINDOW[] = {
    { "Full screen", "[Alt]+[Enter]" },
    { "Screen effect", "[Alt]+[C]" },
    { "Keys, in game", "[F1]" },
    { "View size", "[=] [-]" },
};
#define N(a) ((int)(sizeof a / sizeof a[0]))
static const struct uui_keysheet_group KEYS[] = {
    { "MOVE", MOVE, N(MOVE) }, { "FIGHT", FIGHT, N(FIGHT) },
    { "GAME", GAME, N(GAME) }, { "WINDOW", WINDOW, N(WINDOW) },
};
static struct uui_keysheet g_sheet;

// --- the page's state --------------------------------------------------

static struct uapp *g_app;
static struct uui_datacard g_card_ui;
static struct uui_checkbox g_show;
static struct uui_filedialog g_fd;
static int g_hot = -1, g_pressed = -1;

static uint32_t g_title[DOOM_TITLE_W * DOOM_TITLE_H];
static const struct udata_item *g_title_of;
static int g_title_ok;

// --- measures -----------------------------------------------------------

static int u(void)     { return ugfx_char_h(); }
static int btn_h(void) { return utheme_control_h(); }

enum {
    C_START, C_CHANGE, C_SHOW,                 // the page
    C_RESUME,                                  // the sheet's
    CTLS
};
struct rect { int x, y, w, h; };
static struct rect g_r[CTLS];
static struct rect g_card;                     // the sheet's frame
static int g_side_w;                           // the page's side panel
// THE SIZE OF THE SURFACE LAST DRAWN, not uapp_width(): after a resize
// request the two disagree until the new buffer arrives, and a layout
// for one drawn into the other is what the pointer would then miss.
static int g_W = 640, g_H = 480;

static int in(const struct rect *r, int x, int y) {
    return r->w > 0 && x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

static int btn_w(const char *label) { return ugfx_text_width(label) + 2 * ugfx_char_w() + u(); }

// One place that says where every control is, for drawing and for the
// pointer alike.
static void layout(struct uapp *a) {
    (void)a;
    int W = g_W, H = g_H, p = u();
    memset(g_r, 0, sizeof g_r);
    memset(&g_card, 0, sizeof g_card);
    if (g_view == V_PAGE) {
        int cw, chh;
        uui_checkbox_natural_size(&g_show, &cw, &chh);
        int sw = p * 13 > cw + 2 * p ? p * 13 : cw + 2 * p;   // the checkbox's label fits
        if (sw > W * 2 / 5) sw = W * 2 / 5;
        g_side_w = sw;
        g_r[C_SHOW] = (struct rect){ p, H - p - chh, cw, chh };
        g_r[C_START] = (struct rect){ p, g_r[C_SHOW].y - p / 2 - btn_h(), sw - 2 * p, btn_h() };
        int tw = sw - 2 * p, th = tw * 3 / 4;
        int ly = p + th + p + u() + u() / 2 + 2 * u() + u() / 2;
        g_r[C_CHANGE] = (struct rect){ p, ly, ugfx_text_width("Change game data..."), u() };
        uui_checkbox_set_geometry(&g_show, g_r[C_SHOW].x, g_r[C_SHOW].y);
    } else if (g_help) {
        int sw, sh;
        uui_keysheet_natural_size(&g_sheet, &sw, &sh);
        int cw = sw + 2 * p, ch = p + u() * 2 + sh + p + btn_h() + p;
        if (cw > W - 2 * p) cw = W - 2 * p;
        g_card = (struct rect){ (W - cw) / 2, (H - ch) / 2, cw, ch };
        int rw = btn_w("Resume");
        g_r[C_RESUME] = (struct rect){ g_card.x + cw - p - rw, g_card.y + ch - p - btn_h(), rw, btn_h() };
        uui_keysheet_set_geometry(&g_sheet, g_card.x + p, g_card.y + p + u() * 2, cw - 2 * p, sh);
    }
}

// --- drawing helpers ----------------------------------------------------

static void bold(const char *t, struct ugfx_surface *s, int x, int y, int w, uint32_t bg) {
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string_elided(s, x, y, w, t, UTHEME_TEXT, bg);
    ugfx_set_font(was);
}

static void dim(const char *t, struct ugfx_surface *s, int x, int y, int w, uint32_t bg) {
    ugfx_draw_string_elided(s, x, y, w, t, ugfx_blend(UTHEME_TEXT, bg, 150), bg);
}

static void card(struct ugfx_surface *s, const struct rect *r) {
    int rad = u() / 2;
    uui_fill_round_rect(s, r->x, r->y, r->w, r->h, rad, UTHEME_OUTLINE);
    uui_fill_round_rect(s, r->x + 1, r->y + 1, r->w - 2, r->h - 2, rad - 1, UTHEME_WINDOW_BG);
}

static void button(struct ugfx_surface *s, int id, const char *label, int primary) {
    const struct rect *r = &g_r[id];
    enum uui_state st = g_pressed == id && g_hot == id ? UUI_STATE_PRESSED
                      : g_hot == id ? UUI_STATE_HOVER : UUI_STATE_REST;
    uui_button_draw(s, r->x, r->y, r->w, r->h, label, primary ? UTHEME_ACCENT : UTHEME_BUTTON_BG,
                    primary ? UTHEME_ACCENT_TEXT : UTHEME_TEXT, st);
}

// The title picture, aspect-corrected to 4:3 into `w` x `h` (nearest).
static void draw_title(struct ugfx_surface *s, int x, int y, int w, int h) {
    if (g_title_of != g_cur) {
        char *p = g_cur ? doom_iwad_path(g_cur) : NULL;
        g_title_ok = p && doom_wad_titlepic(p, g_title) == 0;
        free(p);
        g_title_of = g_cur;
    }
    ugfx_fill_rect(s, x - 1, y - 1, w + 2, h + 2, UTHEME_OUTLINE);
    if (!g_title_ok) { ugfx_fill_rect(s, x, y, w, h, ugfx_rgb(0, 0, 0)); return; }
    for (int dy = 0; dy < h; dy++) {
        const uint32_t *row = g_title + (size_t)(dy * DOOM_TITLE_H / h) * DOOM_TITLE_W;
        for (int dx = 0; dx < w; dx++) ugfx_put_pixel(s, x + dx, y + dy, row[dx * DOOM_TITLE_W / w]);
    }
}

// --- the views ----------------------------------------------------------

// THE PAGE ASKS FOR ROOM ONCE: at a big font the keys do not fit 640
// wide. 4:3, so the game still fills the window it then gets.
static void fit_page(struct uapp *a) {
    static int done, tries;
    if (done) return;
    int kw, kh, p = u();
    uui_keysheet_natural_size(&g_sheet, &kw, &kh);
    int need_w = g_side_w + p + p / 2 + kw + p;
    int need_h = p + 2 * u() + kh + 3 * u();
    if (need_w <= g_W && need_h <= g_H) { done = 1; return; }
    int w = need_w > need_h * 4 / 3 ? need_w : need_h * 4 / 3;
    // Refused before the loop runs (as fullscreen is): asked again on
    // the next frame until it takes.
    if (uapp_resize(a, w, w * 3 / 4) || ++tries >= 10) done = 1;
    uapp_redraw(a);
}

static void draw_page(struct ugfx_surface *s) {
    int W = g_W, H = g_H, p = u(), sw = g_side_w;
    uint32_t side = UTHEME_PANEL_BG, main_bg = UTHEME_WINDOW_BG;
    ugfx_fill_rect(s, 0, 0, sw, H, side);
    ugfx_fill_rect(s, sw, 0, 1, H, UTHEME_SEPARATOR);
    ugfx_fill_rect(s, sw + 1, 0, W - sw - 1, H, main_bg);

    int tw = sw - 2 * p, th = tw * 3 / 4, y = p;
    draw_title(s, p, y, tw, th);
    y += th + p;
    bold(g_cur ? g_cur->name : "No game data", s, p, y, tw, side);
    y += u() + u() / 2;
    if (g_cur) {
        // The folder it was found in: the person's, or the system's.
        char *at = doom_iwad_path(g_cur);
        dim(g_cur->id, s, p, y, tw, side);
        if (at) {
            char *slash = strrchr(at, '/');
            if (slash && slash != at) *slash = 0;
            dim(at, s, p, y + u(), tw, side);
            free(at);
        }
    }
    const struct rect *c = &g_r[C_CHANGE];
    uint32_t link = utheme_action(UTHEME_ACT_NAV);
    ugfx_draw_string_clipped(s, c->x, c->y, tw, "Change game data...", link, side);
    if (g_hot == C_CHANGE) ugfx_fill_rect(s, c->x, c->y + u() - 1, c->w, 1, link);
    button(s, C_START, "Start", 1);
    g_show.bg = side;
    uui_checkbox_draw(s, &g_show);

    int mx = sw + p + p / 2, mw = W - mx - p;
    bold("Keys", s, mx, p, mw, main_bg);
    int kw, kh;
    uui_keysheet_natural_size(&g_sheet, &kw, &kh);
    g_sheet.bg = main_bg;
    uui_keysheet_set_geometry(&g_sheet, mx, p + u() * 2, mw, kh);
    uui_keysheet_draw(s, &g_sheet);
    dim("Keyboard only; the mouse does not aim.", s, mx, H - p - u(), mw, main_bg);
}

// --- views: entering ----------------------------------------------------

static void show_page(struct uapp *a) {
    g_view = V_PAGE;
    g_hot = g_pressed = -1;
    ulogf("doom: launcher %s\n", g_cur ? g_cur->id : "-");
    uapp_redraw(a);
}

static void card_done(struct uapp *a, const struct udata_item *w, void *ctx);
static void card_use(struct uapp *a, const struct udata_item *w, void *ctx);
static void card_file(struct uapp *a, void *ctx);
static void card_cancel(struct uapp *a, void *ctx);

static const struct uui_datacard_desc CARD = {
    .log = "doom", .conf = DOOM_CONF, .name = DOOM_DATA,
    .items = DOOM_IWADS,                       // .count is DOOM_IWAD_COUNT, set at open
    .title_empty = "DOOM needs game data", .title_some = "Game data",
    .subtitle = "Choose one. Downloads go to your own data folder.",
    .aux_label = "Use a file...",
    .download = uhttp_download,
    .on_done = card_done, .on_use = card_use, .on_aux = card_file, .on_cancel = card_cancel,
};
static struct uui_datacard_desc g_desc;

static void show_data(struct uapp *a, int back) {
    g_view = V_DATA;
    g_desc = CARD;
    g_desc.count = DOOM_IWAD_COUNT;
    uui_datacard_open(&g_card_ui, a, &g_desc, g_cur, back);
}

static void card_done(struct uapp *a, const struct udata_item *w, void *ctx) {
    (void)ctx;
    doom_iwad_choose(w);
    g_cur = w;
    doom_start_game(a, w);                    // straight into the game, as the card promised
}

static void card_use(struct uapp *a, const struct udata_item *w, void *ctx) {
    (void)ctx;
    doom_iwad_choose(w);
    g_cur = w;
    show_page(a);
}

static void card_cancel(struct uapp *a, void *ctx) {
    (void)ctx;
    show_page(a);
}

static void file_chosen(void *ctx, const char *path) {
    struct uapp *a = ctx;
    if (!path) { uapp_redraw(a); return; }
    if (!doom_wad_is_iwad(path)) {
        char t[160];
        snprintf(t, sizeof t, "%s is not an IWAD, a game's main data file.", k_path_basename(path));
        uui_datacard_note(&g_card_ui, t);
        return;
    }
    const struct udata_item *w = doom_iwad_named(k_path_basename(path));
    if (!w) {
        char t[160];
        snprintf(t, sizeof t, "DOOM does not know %s; it plays doom1, doom, doom2 and freedoom.",
                 k_path_basename(path));
        uui_datacard_note(&g_card_ui, t);
        return;
    }
    // A copy goes to the person's own folder, never over the system's.
    char *dest = udata_path(DOOM_DATA, w);
    char *here = udata_find(DOOM_DATA, w);
    int same = here && !strcmp(here, path);
    free(here);
    if (!dest) return;
    if (!same) {
        // Copied, not linked: the file may be on a stick about to leave.
        // THROUGH .part AND A RENAME, so a copy that fails half way never
        // costs the WAD already installed under that name.
        static struct ufileop op;   // ~33 KB of walk state: never a local
        char *part = NULL, *rel = NULL, *t = NULL;
        if (asprintf(&part, "%s.part", dest) < 0) part = NULL;
        if (asprintf(&rel, DOOM_DATA "/%s", w->id) < 0) rel = NULL;
        if (rel) free(udatadir_prepare(rel));   // its folders
        free(rel);
        struct ufileop_policy pol = { 0 };
        if (!part || ufileop_copy(path, part, &op, &pol) != UFILEOP_OK || rename(part, dest) != 0) {
            if (part) unlink(part);
            if (asprintf(&t, "Could not copy it to %s.", dest) < 0) t = NULL;
            uui_datacard_note(&g_card_ui, t ? t : "Could not copy it.");
            free(t);
            free(part);
            free(dest);
            return;
        }
        free(part);
    }
    free(dest);
    doom_iwad_choose(w);
    g_cur = w;
    uui_datacard_close(&g_card_ui);
    show_page(a);
}

static void card_file(struct uapp *a, void *ctx) {
    (void)ctx;
    if (uui_filedialog_is_open(&g_fd)) return;
    struct uui_filedialog_opts o = {
        .mode = UUI_FILEDIALOG_OPEN,
        .title = "Use a WAD file",
        .start_dir = udir_home(),
    };
    uui_filedialog_open(a, &g_fd, &o, file_chosen, a);
}

static void activate(struct uapp *a, int id) {
    switch (id) {
    case C_START:
        if (g_cur) doom_start_game(a, g_cur);
        return;
    case C_CHANGE:
        show_data(a, 1);
        return;
    case C_SHOW:
        g_show.checked = !g_show.checked;
        uconf_set(DOOM_CONF, "show_page", g_show.checked ? "on" : "off");
        break;
    case C_RESUME:
        doom_help_close(a);
        return;
    default:
        break;
    }
    uapp_redraw(a);
}

// --- the public face ------------------------------------------------------

static void log_layout(void);

int doom_front_up(void) { return g_view != V_GAME; }
int doom_help_up(void)  { return g_help; }

int doom_front_open(struct uapp *a, int has_args) {
    g_app = a;
    uui_keysheet_init(&g_sheet, KEYS, N(KEYS), 2);
    char v[8];
    int show = !(uconf_get(DOOM_CONF, "show_page", v, sizeof v) && !strcmp(v, "off"));
    uui_checkbox_init(&g_show, 0, 0, 0, "Show this page at start", UUI_COLOR_UNSET, UUI_COLOR_UNSET);
    g_show.accent = 1;
    g_show.checked = show;
    g_cur = doom_iwad_chosen();
    if (!g_cur) {
        ulogf("doom: no IWAD in the %s data folders\n", DOOM_DATA);
        show_data(a, 0);
        return 0;
    }
    if (!show || has_args) return 1;
    show_page(a);
    return 0;
}

void doom_front_draw(struct uapp *a, struct ugfx_surface *s) {
    if (g_view == V_DATA) { uui_datacard_draw(&g_card_ui, s); return; }
    g_W = s->w;
    g_H = s->h;
    layout(a);
    if (g_view == V_PAGE) {
        fit_page(a);   // after layout: it reads the side panel's width
        draw_page(s);
    }
    log_layout();
}

void doom_help_open(struct uapp *a) {
    if (g_help || g_view != V_GAME) return;
    g_help = 1;
    g_hot = g_pressed = -1;
    dg_release_all();
    dg_hold(1);
    ulogf("doom: key sheet shown\n");
    uapp_redraw(a);
}

void doom_help_close(struct uapp *a) {
    if (!g_help) return;
    g_help = 0;
    ulogf("doom: key sheet closed, the game was %s\n", dg_paused() ? "paused" : "running");
    dg_hold(0);
    uapp_redraw(a);
}

void doom_help_draw(struct uapp *a, struct ugfx_surface *s) {
    if (!g_help) return;
    g_W = s->w;
    g_H = s->h;
    layout(a);
    int W = g_W, H = g_H, p = u();
    uui_glass_round_rect(s, 0, 0, W, H, 0, ugfx_rgb(0, 0, 0), 140, 0);
    card(s, &g_card);
    uint32_t bg = UTHEME_WINDOW_BG;
    int x = g_card.x + p, w = g_card.w - 2 * p;
    bold("Keys", s, x, g_card.y + p, w / 3, bg);
    if (g_cur) {
        char t[96];
        snprintf(t, sizeof t, "%s, %s", g_cur->name, g_cur->id);
        dim(t, s, x + ugfx_text_width("Keys") + p, g_card.y + p, w - ugfx_text_width("Keys") - p, bg);
    }
    g_sheet.bg = bg;
    uui_keysheet_draw(s, &g_sheet);
    int fy = g_r[C_RESUME].y;
    ugfx_fill_rect(s, g_card.x + 1, fy - p / 2, g_card.w - 2, 1, UTHEME_SEPARATOR);
    dim("Paused. F1 again: DOOM's own help.", s, x, fy + (btn_h() - u()) / 2,
        g_r[C_RESUME].x - x - p, bg);
    button(s, C_RESUME, "Resume", 1);
    log_layout();
}

static const char *const CTL_NAME[CTLS] = {
    [C_START] = "start", [C_CHANGE] = "change", [C_SHOW] = "show", [C_RESUME] = "resume",
};

// Every control where it is, for a test to press by asking.
static void log_layout(void) {
    static const char *const VIEW[] = { "game", "page", "data" };
    uapp_logf_layout("doom: layout view %s%s\n", VIEW[g_view], g_help ? " sheet" : "");
    for (int i = 0; i < CTLS; i++)
        if (g_r[i].w > 0)
            uapp_logf_layout("doom: layout %s %d %d %d %d\n", CTL_NAME[i], g_r[i].x, g_r[i].y,
                             g_r[i].w, g_r[i].h);
}

static int ctl_at(int x, int y) {
    for (int i = 0; i < CTLS; i++) if (in(&g_r[i], x, y)) return i;
    return -1;
}

void doom_front_press(struct uapp *a, int x, int y, unsigned buttons) {
    if (g_view == V_DATA && !g_help) { uui_datacard_press(&g_card_ui, x, y, buttons); return; }
    layout(a);
    g_pressed = ctl_at(x, y);
    uapp_redraw(a);
}

void doom_front_motion(struct uapp *a, int x, int y, unsigned buttons) {
    (void)buttons;
    if (g_view == V_DATA && !g_help) { uui_datacard_motion(&g_card_ui, x, y); return; }
    layout(a);
    int h = ctl_at(x, y);
    if (g_view == V_PAGE) uui_checkbox_hover(&g_show, x, y);
    if (h != g_hot) { g_hot = h; uapp_redraw(a); }
}

void doom_front_release(struct uapp *a, int x, int y, unsigned buttons) {
    (void)buttons;
    if (g_view == V_DATA && !g_help) { uui_datacard_release(&g_card_ui, x, y, buttons); return; }
    layout(a);
    int id = g_pressed;
    g_pressed = -1;
    if (id >= 0 && ctl_at(x, y) == id) activate(a, id);   // commit over the same control
    else uapp_redraw(a);
}

int doom_front_key(struct uapp *a, int key, unsigned mods) {
    if (g_view == V_DATA) return uui_datacard_key(&g_card_ui, key, mods);
    if (g_view == V_PAGE && (key == '\n' || key == '\r')) { activate(a, C_START); return 1; }
    return 0;
}

void doom_front_phys(int keycode, int down) {
    if (g_view == V_DATA) uui_datacard_phys(&g_card_ui, keycode, down);
}

int doom_front_user(struct uapp *a, int a0, int a1) {
    (void)a;
    return uui_datacard_user(&g_card_ui, a0, a1);
}

void doom_front_leave(void) {
    uui_datacard_close(&g_card_ui);
    g_view = V_GAME;
}
