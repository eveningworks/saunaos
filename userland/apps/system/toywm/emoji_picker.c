// The emoji picker (Super+period). See emoji_picker.h for what it is and
// why it is an overlay; docs/decisions/gui.md for the shape.
#include "wm_internal.h"
#include "wm_overlay.h"
#include "wm_flyout.h"
#include "wm_debug.h"
#include "emoji_picker.h"
#include "kutf8.h"
#include "kapi.h"
#include "keyboard.h"
#include "base/uemojidata.h"
#include "base/ugrow.h"
#include "base/ulog.h"
#include "media/icon_cache.h"
#include "ui/utheme.h"
#include "ui/uui_caret.h"
#include "ui/uui_emojigrid.h"
#include "ui/uui_popup.h"
#include "ui/uui_primitives.h"
#include "ui/uui_scrollbar.h"
#include "ui/uui_textbox.h"

int emoji_picker_open = 0;

#define COLS      8
#define GRID_ROWS 5

static struct uui_emojigrid g_grid;
static struct uui_textbox g_search;
static int g_inited;
static int g_tab;                       // 0 Recently used, else group g_tab - 1
static int *g_items, g_items_cap, g_nitems;
static int g_recent[UEMOJI_RECENT_MAX], g_nrecent;
static char g_caption[96];
static int g_hot_tab = -1, g_hot_recent = -1;
static int g_bar;                       // the grid's scrollbar is being dragged

// Where it opens: the caret's rect on screen, else the window's.
static int g_caret_ok, g_cx, g_cy, g_ch;
static int g_win_ok, g_wx, g_wy, g_ww, g_wh;

// --- geometry -----------------------------------------------------------------
//
// The cell and the emoji are fixed pixels, through ugfx_px() (display.scale);
// text rows are the font's. The panel's height does not change with what it
// shows: a search hides the tabs and the Recently used row and the grid takes
// their room, so typing never makes it jump.
struct ep_geom {
    int x, y, w, h, pad, gap, small, cell, px, lh;
    int sx, sy, sw, sh;                 // the search field's frame
    int tabs, tx, ty, tw, th, ntabs;    // tabs: shown?, row, each tab's width
    int recent, rcap_y, ry;             // the Recently used row: shown?
    int hcap_y;                         // the heading
    int gx, gy, gw, gh;                 // the grid
    int fy, fh;                         // the footer band
};

static int searching(void) { return uui_textbox_text(&g_search)[0] != '\0'; }

static void geometry(struct ep_geom *g) {
    const struct uemojidata *d = uemojidata();
    g->pad = ugfx_px(12);
    g->gap = ugfx_px(8);
    g->small = ugfx_px(4);
    g->cell = ugfx_px(40);
    g->px = ugfx_px(26);
    g->lh = ugfx_char_h();
    g->sh = g->lh + ugfx_px(14);
    g->th = ugfx_px(36);
    g->fh = (g->lh * 2 + 2 > ugfx_px(32) ? g->lh * 2 + 2 : ugfx_px(32)) + 2 * g->gap;
    int bar = uui_scrollbar_overlay_width();
    g->gw = COLS * g->cell + bar;
    g->w = g->gw + 2 * g->pad;
    g->h = g->pad + g->sh + g->gap + g->th + g->gap + g->lh + g->small + g->cell + g->gap +
           g->lh + g->small + GRID_ROWS * g->cell + g->gap + g->fh;

    // Under the caret, flipped above it when it does not fit; else the
    // window's lower half; else the screen's middle.
    int x, y;
    if (g_caret_ok) {
        x = g_cx - g->pad - g->cell / 2;
        y = g_cy + g_ch + g->small;
        if (y + g->h > screen_h - taskbar_h - WM_POPUP_MARGIN) y = g_cy - g->small - g->h;
    } else if (g_win_ok) {
        x = g_wx + (g_ww - g->w) / 2;
        y = g_wy + g_wh / 2 + (g_wh / 2 - g->h) / 2;
    } else {
        x = (screen_w - g->w) / 2;
        y = (screen_h - taskbar_h - g->h) / 2;
    }
    wm_popup_place(x, y, g->w, g->h, &g->x, &g->y);

    g->sx = g->x + g->pad;
    g->sy = g->y + g->pad;
    g->sw = g->w - 2 * g->pad;
    int yy = g->sy + g->sh + g->gap;
    g->tabs = !searching();
    g->ntabs = 1 + d->ngroups;
    g->tx = g->sx;
    g->tw = (COLS * g->cell) / (g->ntabs > 0 ? g->ntabs : 1);
    g->ty = yy;
    if (g->tabs) yy += g->th + g->gap;
    g->recent = !searching() && g_tab != 0 && g_nrecent > 0;
    g->rcap_y = yy;
    g->ry = 0;   // reported by `gui emoji` even when the row is hidden
    if (g->recent) {
        g->ry = yy + g->lh + g->small;
        yy = g->ry + g->cell + g->gap;
    }
    g->hcap_y = yy;
    g->gx = g->sx;
    g->gy = yy + g->lh + g->small;
    g->fy = g->y + g->h - g->fh;
    g->gh = g->fy - g->gap - g->gy;
    g_grid.grid.cell_w = g_grid.grid.cell_h = g->cell;
    g_grid.px = g->px;
    uui_emojigrid_ops.set_geometry(&g_grid, g->gx, g->gy, g->gw, g->gh);
    uui_textbox_set_geometry(&g_search, g->sx + g->sh, g->sy + 1, g->sw - g->sh - 2, g->sh - 2);
}

static int tab_at(const struct ep_geom *g, int mx, int my) {
    if (!g->tabs || !uui_hit(g->tx, g->ty, g->tw * g->ntabs, g->th, mx, my)) return -1;
    int i = (mx - g->tx) / g->tw;
    return i < g->ntabs ? i : -1;
}

static int recent_at(const struct ep_geom *g, int mx, int my) {
    if (!g->recent || !uui_hit(g->gx, g->ry, COLS * g->cell, g->cell, mx, my)) return -1;
    int i = (mx - g->gx) / g->cell;
    return i < g_nrecent && i < COLS ? i : -1;
}

// --- what it lists ------------------------------------------------------------

static void refill(void) {
    const struct uemojidata *d = uemojidata();
    if (!UGROW(g_items, g_items_cap, d->count + 1)) {
        ulogf("emoji: out of memory listing %d entries\n", d->count);
        g_nitems = 0;
        uui_emojigrid_set(&g_grid, g_items, 0);
        return;
    }
    const char *q = uui_textbox_text(&g_search);
    if (q[0]) {
        int n = uemojidata_search(d, q, g_items, g_items_cap);
        g_nitems = n < g_items_cap ? n : g_items_cap;
        k_snprintf(g_caption, sizeof g_caption, "%d match%s for \xE2\x80\x9C%s\xE2\x80\x9D", n,
                   n == 1 ? "" : "es", q);
    } else if (g_tab == 0 || g_tab > d->ngroups) {
        g_tab = 0;
        for (g_nitems = 0; g_nitems < g_nrecent; g_nitems++) g_items[g_nitems] = g_recent[g_nitems];
        k_strlcpy(g_caption, g_nrecent ? "Recently used" : "Nothing used yet", sizeof g_caption);
    } else {
        const struct uemoji_group *gr = &d->groups[g_tab - 1];
        for (g_nitems = 0; g_nitems < gr->count; g_nitems++) g_items[g_nitems] = gr->first + g_nitems;
        k_strlcpy(g_caption, gr->name, sizeof g_caption);
    }
    uui_emojigrid_set(&g_grid, g_items, g_nitems);
    g_grid.grid.focused = 1;
}

static void set_tab(int tab) {
    if (tab == g_tab) return;
    g_tab = tab;
    refill();
    emoji_picker_damage();
}

// --- open, close, insert ----------------------------------------------------------

void emoji_picker_damage(void) { wm_overlay_damage("emoji"); }

int emoji_picker_rect(int *x, int *y, int *w, int *h) {
    struct ep_geom g;
    geometry(&g);
    *x = g.x; *y = g.y; *w = g.w; *h = g.h;
    return 1;
}

void emoji_picker_close(void) {
    if (!emoji_picker_open) return;
    emoji_picker_open = 0;   // where it was drawn is damaged by the core (wm_overlay.h)
    g_bar = 0;
    g_hot_tab = g_hot_recent = -1;
}

static void open_now(void) {
    wm_overlay_close_others("emoji");
    if (!g_inited) {
        g_inited = 1;
        uui_emojigrid_init(&g_grid, ugfx_px(40), ugfx_px(26));
        g_grid.grid.ring = 1;
        uui_textbox_init(&g_search, "");
        g_search.placeholder = "Search emoji";
        g_search.bare = 1;   // the panel draws the frame and its focus mark
    }
    g_search.bg = g_search.border = UTHEME_WHITE;
    uui_textbox_set_text(&g_search, "");
    uui_textbox_set_active(&g_search, 1);   // always: typing goes here
    // The focused window by the rule real keys are routed by, and its caret.
    g_caret_ok = g_win_ok = 0;
    int kt = wm_key_target(wm_focus_index());
    if (kt >= 0) {
        const struct window *w = &windows[kt];
        g_win_ok = 1;
        g_wx = window_content_x(w); g_wy = window_content_y(w);
        g_ww = window_content_w(w); g_wh = window_content_h(w);
        if (w->caret_h > 0) {
            g_caret_ok = 1;
            g_cx = g_wx + w->caret_x;
            g_cy = g_wy + w->caret_y;
            g_ch = w->caret_h;
        }
    }
    g_nrecent = uemojidata_recents(g_recent, UEMOJI_RECENT_MAX);
    g_tab = uemojidata()->ngroups ? 1 : 0;
    g_hot_tab = g_hot_recent = -1;
    refill();
    emoji_picker_open = 1;
    uui_caret_reset();
    emoji_picker_damage();
    ulogf("emoji: picker open for window %d (%s caret), %d emoji, %d recent\n", kt,
          g_caret_ok ? "at its" : "no", uemojidata()->count, g_nrecent);
}

void emoji_picker_toggle(void) {
    if (emoji_picker_open) emoji_picker_close();
    else open_now();
}

// THE CHARACTERS GO AS KEYS, one per code point of the emoji -- a sequence's
// joiners and selectors included, the field putting the cluster back
// together -- as 32-bit code points, which is what WIN_EV_KEY carries
// (api/keyboard.h), to the window real keys go to. `seq` is what to type:
// the entry's own, or the toned form its strip gave. Windows closes its
// panel after a pick.
static void insert_seq(int entry, const char *seq) {
    const struct uemojidata *d = uemojidata();
    if (entry < 0 || entry >= d->count) return;
    const struct uemoji_entry *e = &d->entries[entry];
    if (!seq || !seq[0]) seq = e->seq;
    int kt = wm_key_target(wm_focus_index());
    emoji_picker_close();
    if (kt < 0 || !wm_client_is_client_window(&windows[kt])) {
        ulogf("emoji: nothing to type %s into\n", e->name);
        return;
    }
    int len = (int)k_strlen(seq), o = 0;
    char codes[160] = "";
    for (int i = 0, n; i < len; i += n > 0 ? n : 1) {
        uint32_t cp = kutf8_decode(seq + i, len - i, &n);
        wm_client_send_key(&windows[kt], (int)cp, 0);
        wm_client_send_key_up(&windows[kt], (int)cp, 0);
        if (o < (int)sizeof codes - 10)
            o += k_snprintf(codes + o, sizeof codes - (size_t)o, "%sU+%04X", o ? " " : "", (unsigned)cp);
    }
    uemojidata_recent_add(entry);
    ulogf("emoji: inserted %s (%s) into window %d\n", codes, e->name, kt);
}

static void insert(int entry) { insert_seq(entry, 0); }

// What the grid activated -- a cell, or a form from its skin-tone strip.
static void insert_taken(void) {
    char seq[128];
    int e = uui_emojigrid_take_seq(&g_grid, seq, sizeof seq);
    if (e >= 0) insert_seq(e, seq);
    else emoji_picker_damage();
}

// A RIGHT-CLICK on a cell opens its skin tones (uui_emojigrid_variants());
// anywhere else in the panel it is inert. 1 when the panel took it.
int emoji_picker_right_click(int mx, int my) {
    if (!emoji_picker_open) return 0;
    struct ep_geom g;
    geometry(&g);
    if (!uui_hit(g.x, g.y, g.w, g.h, mx, my)) return 0;
    if (uui_hit(g.gx, g.gy, g.gw, g.gh, mx, my)) {
        int i = uui_emojigrid_index_at(&g_grid, mx, my);
        if (i >= 0 && uui_emojigrid_variants(&g_grid, i))
            ulogf("emoji: tones of %s\n", uemojidata()->entries[g_grid.items[i]].name);
        emoji_picker_damage();
    }
    return 1;
}

// --- input ------------------------------------------------------------------------

int emoji_picker_hover_at(int mx, int my) {
    if (!emoji_picker_open) return 0;
    struct ep_geom g;
    geometry(&g);
    uui_emojigrid_ops.motion(&g_grid, mx, my, 0);
    g_hot_tab = tab_at(&g, mx, my);
    g_hot_recent = recent_at(&g, mx, my);
    if (!uui_hit(g.x, g.y, g.w, g.h, mx, my)) return 0;
    return 1 + (g_hot_tab + 1) + 100 * (g_hot_recent + 1) + 10000 * (g_grid.grid.hot + 1) +
           (g_grid.grid.bar_hot ? 5000000 : 0) +
           (uui_emojigrid_variants_open(&g_grid) ? 10000000 * (g_grid.var_sel + 1) : 0);
}

int emoji_picker_handle_click(int mx, int my) {
    if (!emoji_picker_open) return 0;
    struct ep_geom g;
    geometry(&g);
    if (!uui_hit(g.x, g.y, g.w, g.h, mx, my)) {
        emoji_picker_close();
        // A click on the TASKBAR is still the taskbar's.
        return my < screen_h - taskbar_h;
    }
    int t = tab_at(&g, mx, my);
    if (t >= 0) { set_tab(t); return 1; }
    int r = recent_at(&g, mx, my);
    if (r >= 0) { insert(g_recent[r]); return 1; }
    if (uui_emojigrid_variants_open(&g_grid)) {
        // The strip's: a form inserts, anywhere else shuts it.
        uui_emojigrid_ops.press(&g_grid, mx, my, 0);
        insert_taken();
        return 1;
    }
    if (uui_hit(g.gx, g.gy, g.gw, g.gh, mx, my)) {
        // A CLICK INSERTS: the cell the press lands on, as Windows' panel.
        // The overlay bar inside the grid is the grid's: dragged below.
        uui_emojigrid_ops.press(&g_grid, mx, my, 0);
        if (g_grid.grid.bar_drag) { g_bar = 1; emoji_picker_damage(); return 1; }
        int i = g_grid.grid.armed;
        g_grid.grid.armed = -1;
        if (i >= 0 && i < g_nitems) insert(g_items[i]);
        else emoji_picker_damage();
        return 1;
    }
    if (uui_hit(g.sx, g.sy, g.sw, g.sh, mx, my)) {
        uui_textbox_ops.press(&g_search, mx, my, 0);
        emoji_picker_damage();
    }
    return 1;   // inert, but ours
}

void emoji_picker_update_press(int mx, int my, uint8_t buttons) {
    if (!emoji_picker_open || !g_bar) return;
    if (buttons & 0x1) uui_emojigrid_ops.motion(&g_grid, mx, my, buttons);
    else { uui_emojigrid_ops.release(&g_grid, mx, my); g_bar = 0; }
    emoji_picker_damage();
}

int emoji_picker_wheel(int mx, int my, int notches) {
    if (!emoji_picker_open) return 0;
    struct ep_geom g;
    geometry(&g);
    if (!uui_hit(g.x, g.y, g.w, g.h, mx, my)) return 0;
    if (uui_emojigrid_ops.wheel(&g_grid, notches)) emoji_picker_damage();
    return 1;
}

// THE KEYBOARD IS THE PANEL'S while it is up (wm_overlay.h): arrows move in
// the grid, Enter inserts, Esc closes, Tab steps the tabs, and whatever types
// goes to the search field. A shortcut (Ctrl or Alt) is left alone.
int emoji_picker_key(int key, uint8_t mods) {
    if (!emoji_picker_open) return 0;
    struct ep_geom g;
    geometry(&g);
    uui_caret_reset();
    // THE SKIN-TONE STRIP has the keys while it is up: Esc shuts only it,
    // Enter takes its form. The Menu key or Shift+F10 opens it.
    if (uui_emojigrid_variants_open(&g_grid) || key == KEY_MENU || (key == KEY_F10 && (mods & KEY_MOD_SHIFT))) {
        int was_open = uui_emojigrid_variants_open(&g_grid);
        int took = uui_emojigrid_ops.key(&g_grid, key, mods);
        if (was_open && (key == '\n' || key == '\r' || key == ' ')) { insert_taken(); return 1; }
        emoji_picker_damage();
        if (took || key == 0x1B) return 1;
    }
    if (key == 0x1B) { emoji_picker_close(); return 1; }
    if (key == '\n' || key == '\r') {
        int e = uui_emojigrid_focused(&g_grid);
        if (e >= 0) insert(e);
        return 1;
    }
    if (key == '\t') {
        if (g.tabs) {
            int n = g.ntabs;
            set_tab((g_tab + ((mods & KEY_MOD_SHIFT) ? n - 1 : 1)) % n);
        }
        return 1;
    }
    switch (key) {
    case KEY_ARROW_LEFT: case KEY_ARROW_RIGHT: case KEY_ARROW_UP: case KEY_ARROW_DOWN:
    case KEY_PAGE_UP: case KEY_PAGE_DOWN:
        uui_emojigrid_ops.key(&g_grid, key, mods);
        emoji_picker_damage();
        return 1;
    default:
        break;
    }
    if (mods & (KEY_MOD_CTRL | KEY_MOD_ALT)) return 0;
    const char *before = uui_textbox_text(&g_search);
    char was[UUI_TEXTBOX_MAX];
    k_strlcpy(was, before, sizeof was);
    int took = uui_textbox_key_mods(&g_search, key, mods);
    if (took && k_strcmp(was, uui_textbox_text(&g_search)) != 0) {
        refill();
        ulogf("emoji: search \"%s\" %d\n", uui_textbox_text(&g_search), g_nitems);
    }
    emoji_picker_damage();
    return took || uui_key_is_text(key);
}

int emoji_picker_wait_ms(void) {
    if (!emoji_picker_open) return -1;
    int w = uui_caret_wait_ms();
    if (w == 0) { emoji_picker_damage(); WM_REDRAW(); }
    return w;
}

// --- drawing -----------------------------------------------------------------------

static void draw_tab(struct ugfx_surface *s, const struct ep_geom *g, int i) {
    int x = g->tx + i * g->tw, y = g->ty, sel = i == g_tab;
    int r = uui_select_radius(0);
    if (sel) {
        uui_fill_round_rect(s, x + 1, y + 1, g->tw - 2, g->th - 2, r, UTHEME_SELECTION);
        int uw = ugfx_px(16);
        uui_fill_round_rect(s, x + (g->tw - uw) / 2, y + g->th - ugfx_px(4), uw, ugfx_px(3), UUI_CAPSULE,
                            UTHEME_ACCENT);
    } else if (i == g_hot_tab) {
        uui_fill_round_rect(s, x + 1, y + 1, g->tw - 2, g->th - 2, r,
                            uui_state_bg(wm_flyout_ground(), UUI_STATE_HOVER));
    }
    int icon = ugfx_px(20);
    if (i == 0) {
        const struct uimg *im = icon_get("tb-history", icon);
        if (im) ugfx_blit_tinted(s, x + (g->tw - im->w) / 2, y + (g->th - im->h) / 2, im->w, im->h, im->px,
                                 im->w, UTHEME_TEXT);
    } else {
        const struct uemojidata *d = uemojidata();
        uui_emojigrid_draw_entry(s, 0, d->groups[i - 1].first, icon, x, y, g->tw, g->th);
    }
}

// "U+1F468 U+200D U+1F469": every code point, as many as fit.
static void codes(const struct uemoji_entry *e, char *out, int cap) {
    int o = 0, len = (int)k_strlen(e->seq);
    out[0] = '\0';
    for (int i = 0, n; i < len && o < cap - 1; i += n > 0 ? n : 1) {
        uint32_t cp = kutf8_decode(e->seq + i, len - i, &n);
        int w = k_snprintf(out + o, (size_t)(cap - o), "%sU+%04X", o ? " " : "", (unsigned)cp);
        if (w <= 0 || o + w >= cap) { out[o] = '\0'; break; }
        o += w;
    }
}

void emoji_picker_draw(int mx, int my) {
    (void)mx; (void)my;
    if (!emoji_picker_open) return;
    struct ep_geom g;
    geometry(&g);
    struct ugfx_surface *s = wm_surface();
    uint32_t ground = wm_flyout_ground(), dim = wm_flyout_dim();
    wm_flyout_card(g.x, g.y, g.w, g.h, g.fh);

    // The search field: Windows 11's and the Start menu's -- rounded, an
    // accent rule while it listens, which is always.
    int fr = ugfx_px(6);
    uui_fill_round_rect(s, g.sx, g.sy, g.sw, g.sh, fr, UTHEME_OUTLINE);
    uui_fill_round_rect(s, g.sx + 1, g.sy + 1, g.sw - 2, g.sh - 2, fr - 1, UTHEME_WHITE);
    ugfx_fill_rect(s, g.sx + fr, g.sy + g.sh - 2, g.sw - 2 * fr, 2, UTHEME_ACCENT);
    const struct uimg *find = icon_get("tb-find", ugfx_px(16));
    if (find) ugfx_blit_tinted(s, g.sx + (g.sh - find->w) / 2, g.sy + (g.sh - find->h) / 2, find->w, find->h,
                               find->px, find->w, dim);
    uui_textbox_draw(s, &g_search);

    if (g.tabs)
        for (int i = 0; i < g.ntabs; i++) draw_tab(s, &g, i);

    if (g.recent) {
        ugfx_draw_string_clipped(s, g.gx, g.rcap_y, g.gw, "Recently used", dim, UGFX_TRANSPARENT);
        for (int i = 0; i < g_nrecent && i < COLS; i++) {
            int x = g.gx + i * g.cell;
            if (i == g_hot_recent)
                uui_fill_round_rect(s, x + 2, g.ry + 2, g.cell - 4, g.cell - 4, uui_select_radius(0),
                                    uui_state_bg(ground, UUI_STATE_HOVER));
            uui_emojigrid_draw_entry(s, 0, g_recent[i], g.px, x, g.ry, g.cell, g.cell);
        }
    }

    ugfx_draw_string_clipped(s, g.gx, g.hcap_y, g.gw, g_caption, dim, UGFX_TRANSPARENT);
    g_grid.grid.bg = ground;
    uui_emojigrid_ops.draw(s, &g_grid);

    // The footer: the focused emoji, its name and its code points.
    const struct uemojidata *d = uemojidata();
    int e = uui_emojigrid_focused(&g_grid);
    if (e >= 0 && e < d->count) {
        int big = ugfx_px(32), x = g.x + g.pad, ty = g.fy + (g.fh - g.lh * 2 - 2) / 2;
        uui_emojigrid_draw_entry(s, 0, e, big, x, g.fy + (g.fh - big) / 2, big, big);
        int tx = x + big + g.gap;
        const char *hint = "Enter inserts \xC2\xB7 Esc closes";
        int hw = ugfx_text_width(hint), room = g.x + g.w - g.pad - tx;
        // THE NAME FIRST: the hint shows only beside a name that fits whole
        // (a larger font leaves no room for both), and a name that does not
        // fit even alone is elided, never cut mid-letter.
        const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
        int nw = ugfx_text_width(d->entries[e].name);
        int show_hint = nw + g.gap + hw <= room;
        if (show_hint) room -= hw + g.gap;
        ugfx_draw_string_elided(s, tx, ty, room, d->entries[e].name, UTHEME_TEXT, UGFX_TRANSPARENT);
        ugfx_set_font(was);
        char cps[32];
        codes(&d->entries[e], cps, sizeof cps);
        ugfx_draw_string_clipped(s, tx, ty + g.lh + 2, room, cps, dim, UGFX_TRANSPARENT);
        if (show_hint)
            ugfx_draw_string(s, g.x + g.w - g.pad - hw, g.fy + (g.fh - g.lh) / 2, hint, dim, UGFX_TRANSPARENT);
    }
}

// --- `gui emoji` ---------------------------------------------------------------------

void emoji_picker_report(struct dbg_out *o, int json) {
    struct ep_geom g;
    geometry(&g);
    const struct uemojidata *d = uemojidata();
    int e = emoji_picker_open ? uui_emojigrid_focused(&g_grid) : -1;
    const char *tab = g_tab == 0 ? "Recent" : g_tab <= d->ngroups ? d->groups[g_tab - 1].tab : "";
    if (!json) {
        dbg_out_printf(o, "emoji: %s  x=%d y=%d w=%d h=%d  tab=%s  listed=%d  focused=%s\r\n",
                       emoji_picker_open ? "open" : "closed", g.x, g.y, g.w, g.h, tab, g_nitems,
                       e >= 0 ? d->entries[e].name : "-");
        return;
    }
    dbg_out_printf(o, "{\"open\":%s,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"entries\":%d,",
                   emoji_picker_open ? "true" : "false", g.x, g.y, g.w, g.h, d->count);
    dbg_out_printf(o, "\"tab\":\"%s\",\"tab_index\":%d,\"query\":\"%s\",\"listed\":%d,\"caption\":\"%s\",",
                   tab, g_tab, uui_textbox_text(&g_search), g_nitems, g_caption);
    dbg_out_printf(o, "\"caret\":{\"known\":%s,\"x\":%d,\"y\":%d,\"h\":%d},",
                   g_caret_ok ? "true" : "false", g_cx, g_cy, g_ch);
    if (e >= 0)
        dbg_out_printf(o, "\"focused\":{\"cp\":\"%04X\",\"vs16\":%d,\"tones\":%d,\"name\":\"%s\"},",
                       (unsigned)d->entries[e].cp, d->entries[e].vs16, d->entries[e].toned != 0, d->entries[e].name);
    else
        dbg_out_printf(o, "\"focused\":null,");
    int cx, cy, cw, ch;
    if (uui_grid_cell_rect(&g_grid.grid, 0, &cx, &cy, &cw, &ch))
        dbg_out_printf(o, "\"cell\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d},", cx, cy, cw, ch);
    dbg_out_printf(o, "\"columns\":%d,\"grid\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d},",
                   uui_grid_columns(&g_grid.grid), g.gx, g.gy, g.gw, g.gh);
    dbg_out_printf(o, "\"search\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d},", g.sx, g.sy, g.sw, g.sh);
    dbg_out_printf(o, "\"strip\":{\"open\":%s,\"sel\":%d},",
                   uui_emojigrid_variants_open(&g_grid) ? "true" : "false", g_grid.var_sel);
    dbg_out_printf(o, "\"recent_row\":%s,\"recent_cell\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d},\"recent\":[",
                   g.recent ? "true" : "false", g.gx, g.ry, g.cell, g.cell);
    for (int i = 0; i < g_nrecent; i++)
        dbg_out_printf(o, "%s\"%04X\"", i ? "," : "", (unsigned)d->entries[g_recent[i]].cp);
    dbg_out_printf(o, "],\"tabs\":[");
    for (int i = 0; g.tabs && i < g.ntabs; i++)
        dbg_out_printf(o, "%s{\"name\":\"%s\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}", i ? "," : "",
                       i ? d->groups[i - 1].tab : "Recent", g.tx + i * g.tw, g.ty, g.tw, g.th);
    dbg_out_printf(o, "]}\r\n");
}
