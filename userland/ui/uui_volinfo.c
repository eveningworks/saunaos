// uui_volinfo -- a volume's or share's card. See ui/uui_volinfo.h.
#include "ui/uui_volinfo.h"
#include <stdio.h>
#include <string.h>
#include "ui/utheme.h"
#include "ui/uui_widget.h"
#include "ui/uui_primitives.h"
#include "lib/human.h"
#include "lib/icon_cache.h"
#include "userfs_abi.h"
#include "fixed.h"

static int pad(void) { return utheme_pad(); }
static int lh(void)  { return ugfx_char_h() + utheme_gap(); }
static int per(void) { int p = ugfx_char_advance('n'); return p > 0 ? p : 8; }
static int key_w(const struct uui_volinfo *w) { return per() * (w->compact ? 9 : 12); }
static int icon_px(const struct uui_volinfo *w) { return w->compact ? 48 : 40; }
static int ring_d(void) { return ugfx_char_h() * 8; }

static const char *icon_of(const struct uvolinfo *v) {
    return v->remote ? "drive-network" : v->persistent ? "drive" : "drive-ram";
}

uint32_t uui_volinfo_colour(const struct uvolinfo *v) {
    if (v->total && v->used * 10 >= v->total * 9) return utheme_action(UTHEME_ACT_DANGER);
    if (v->remote)      return utheme_action(UTHEME_ACT_MEDIA);
    if (!v->persistent) return utheme_action(UTHEME_ACT_EDIT);
    if (v->readonly)    return utheme_action(UTHEME_ACT_ARRANGE);
    return utheme_action(UTHEME_ACT_CREATE);
}

static uint32_t ground(const struct uui_volinfo *w) {
    const struct utheme *t = utheme_current();
    if (w->bg) return w->bg;
    return w->compact ? ugfx_blend(t->panel_bg, t->field_bg, 128) : t->panel_bg;
}

static int nrows(const struct uui_volinfo *w) {
    struct uvolinfo_row r[UVOL_ROWS];
    return w->vi ? uvolinfo_rows(w->vi, r, UVOL_ROWS) : 0;
}

static int head_h(const struct uui_volinfo *w) {
    int p = pad();
    if (w->compact) return p + icon_px(w) + p + lh() + (w->vi && w->vi->total ? p + lh() : 0) + p;
    int ic = icon_px(w), t = 2 * lh();
    return 2 * p + (ic > t ? ic : t);
}

static int ring_block_h(const struct uui_volinfo *w) {
    if (w->compact || !w->vi || !w->vi->total) return 0;
    return ring_d() + 2 * pad();
}

int uui_volinfo_height(const struct uui_volinfo *w, int width) {
    (void)width;
    if (!w->vi) return 0;
    return head_h(w) + pad() + nrows(w) * lh() + pad() + ring_block_h(w);
}

// The title: the name, and for a share " on <server>" dimmer after it.
static void title(struct ugfx_surface *s, int x, int y, int tw, const struct uvolinfo *v,
                  int centre, uint32_t fg, uint32_t dim, uint32_t bg) {
    char on[80] = "";
    if (v->remote && v->server[0]) snprintf(on, sizeof on, " on %s", v->server);
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    int nw = ugfx_text_width(v->label);
    ugfx_set_font(was);
    int ow = ugfx_text_width(on), all = nw + ow;
    if (centre && all < tw) x += (tw - all) / 2;
    was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string_elided(s, x, y, tw, v->label, fg, bg);
    ugfx_set_font(was);
    if (on[0] && nw < tw) ugfx_draw_string_elided(s, x + nw, y, tw - nw, on, dim, bg);
}

static void bar(struct ugfx_surface *s, int x, int y, int bw, const struct uvolinfo *v) {
    const struct utheme *t = utheme_current();
    int bh = pad() / 2 + 2, pm = uvolinfo_used_pm(v);
    int used = pm > 0 ? bw * pm / 1000 : 0;
    if (used < 1 && v->used) used = 1;
    uui_fill_round_rect(s, x, y, bw, bh, bh / 2, t->separator);
    if (used) uui_fill_round_rect(s, x, y, used, bh, bh / 2, uui_volinfo_colour(v));
}

static int ring_top(const struct uui_volinfo *w) {
    return w->y + head_h(w) + pad() + nrows(w) * lh() + pad();
}

int uui_volinfo_ring_rect(const struct uui_volinfo *w, int *x, int *y, int *d) {
    if (!ring_block_h(w)) return 0;
    *d = ring_d();
    *x = w->x + 3 * pad();
    *y = ring_top(w) + pad();
    return 1;
}

static void draw_ring(struct ugfx_surface *s, const struct uui_volinfo *w, uint32_t bg) {
    const struct uvolinfo *v = w->vi;
    const struct utheme *t = utheme_current();
    uint32_t dim = uui_state_bg(t->text, UUI_STATE_DISABLED), c = uui_volinfo_colour(v);
    int p = pad(), d, rx, ry;
    uui_volinfo_ring_rect(w, &rx, &ry, &d);
    int r = d / 2, cx = rx + r, cy = ry + r;
    int pm = uvolinfo_used_pm(v);
    fx_t top = -FX_ONE / 4, sweep = (fx_t)((int64_t)FX_ONE * pm / 1000);
    ugfx_fill_ring(s, cx, cy, r, r * 3 / 5, top, top + FX_ONE, t->separator);
    if (sweep > 0) ugfx_fill_ring(s, cx, cy, r, r * 3 / 5, top, top + sweep, c);
    char pct[8];
    snprintf(pct, sizeof pct, "%d%%", (pm + 5) / 10);
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string(s, cx - ugfx_text_width(pct) / 2, cy - ugfx_char_h() / 2, pct, t->text, bg);
    ugfx_set_font(was);

    // The legend: a swatch and an amount per part, the capacity under.
    char u[16], f[16], cap[32];
    human_size(u, sizeof u, v->used > v->total ? v->total : v->used);
    human_size(f, sizeof f, v->total > v->used ? v->total - v->used : 0);
    char tot[16];
    human_size(tot, sizeof tot, v->total);
    snprintf(cap, sizeof cap, "Capacity %s", tot);
    int lx = cx + r + 3 * p, ly = cy - 3 * lh() / 2, sw = ugfx_char_h() - 4, aw = per() * 6;
    int room = w->x + w->w - p - lx;
    if (room <= aw) return;
    uui_fill_round_rect(s, lx, ly + 2, sw, sw, 3, c);
    ugfx_draw_string_clipped(s, lx + sw + p, ly, aw, "Used", t->text, bg);
    ugfx_draw_string_clipped(s, lx + sw + p + aw, ly, room - sw - p - aw, u, t->text, bg);
    ly += lh();
    uui_fill_round_rect(s, lx, ly + 2, sw, sw, 3, t->separator);
    ugfx_draw_string_clipped(s, lx + sw + p, ly, aw, "Free", t->text, bg);
    ugfx_draw_string_clipped(s, lx + sw + p + aw, ly, room - sw - p - aw, f, t->text, bg);
    ly += lh();
    ugfx_draw_string_clipped(s, lx, ly, room, cap, dim, bg);
}

void uui_volinfo_draw(struct ugfx_surface *s, const struct uui_volinfo *w) {
    const struct uvolinfo *v = w->vi;
    if (!v || w->w <= 0 || w->h <= 0) return;
    const struct utheme *t = utheme_current();
    uint32_t bg = ground(w), dim = uui_state_bg(t->text, UUI_STATE_DISABLED);
    int p = pad(), y = w->y, ic = icon_px(w);
    ugfx_set_clip_rect(s, w->x, w->y, w->w, w->h);
    ugfx_fill_rect(s, w->x, w->y, w->w, w->h, bg);
    const struct uimg *icon = icon_get(icon_of(v), ic);
    char kind[64], space[48];
    uvolinfo_kind_text(v, kind, sizeof kind);
    uvolinfo_space_text(v, space, sizeof space);
    if (w->compact) {
        // Centred, as the details pane draws a file: icon, name, bar.
        int tw = w->w - 2 * p;
        y += p;
        if (icon) ugfx_blit_alpha(s, w->x + (w->w - icon->w) / 2, y, icon->w, icon->h, icon->px, icon->w);
        y += ic + p;
        title(s, w->x + p, y, tw, v, 1, t->text, dim, bg);
        y += lh();
        if (v->total) {
            y += p / 2;
            bar(s, w->x + 2 * p, y, w->w - 4 * p, v);
            y += p / 2 + 4;
            int sw = ugfx_text_width(space);
            ugfx_draw_string_clipped(s, w->x + (sw < tw ? (w->w - sw) / 2 : p), y, tw, space, dim, bg);
            y += lh() - 4;
        }
        y += p;
    } else {
        uint32_t hb = ugfx_blend(t->panel_bg, t->field_bg, 140);
        int hh = head_h(w);
        ugfx_fill_rect(s, w->x, y, w->w, hh, hb);
        ugfx_fill_rect(s, w->x, y + hh - 1, w->w, 1, t->separator);
        if (icon) ugfx_blit_alpha(s, w->x + 2 * p, y + (hh - icon->h) / 2, icon->w, icon->h, icon->px, icon->w);
        int tx = w->x + 3 * p + ic, tw = w->x + w->w - p - tx, ty = y + (hh - 2 * lh()) / 2;
        title(s, tx, ty, tw, v, 0, t->text, dim, hb);
        ugfx_draw_string_elided(s, tx, ty + lh(), tw, kind, dim, hb);
        y += hh;
    }
    y += p;
    if (w->compact) ugfx_fill_rect(s, w->x + p, y - p / 2, w->w - 2 * p, 1, t->separator);
    struct uvolinfo_row rows[UVOL_ROWS];
    int n = uvolinfo_rows(v, rows, UVOL_ROWS);
    int lx = w->x + (w->compact ? p : 3 * p), vx = lx + key_w(w), vw = w->x + w->w - p - vx;
    for (int i = 0; i < n; i++, y += lh()) {
        ugfx_draw_string_clipped(s, lx, y, key_w(w) - p, rows[i].key, dim, bg);
        ugfx_draw_string_elided(s, vx, y, vw, rows[i].value, t->text, bg);
    }
    y += p;
    if (ring_block_h(w)) {
        ugfx_fill_rect(s, w->x + p, y - p / 2, w->w - 2 * p, 1, t->separator);
        draw_ring(s, w, bg);
    }
    ugfx_clear_clip_rect(s);
}

void uui_volinfo_init(struct uui_volinfo *w, const struct uvolinfo *vi, int compact) {
    memset(w, 0, sizeof *w);
    w->vi = vi;
    w->compact = compact;
}

static void vi_natural(const void *v, int *ow, int *oh) {
    const struct uui_volinfo *w = v;
    *ow = per() * (w->compact ? 28 : 56);
    *oh = uui_volinfo_height(w, *ow);
}
static void vi_geom(void *v, int x, int y, int ww, int hh) {
    struct uui_volinfo *w = v;
    w->x = x; w->y = y; w->w = ww; w->h = hh;
}
static void vi_bounds(const void *v, int *x, int *y, int *ww, int *hh) {
    const struct uui_volinfo *w = v;
    *x = w->x; *y = w->y; *ww = w->w; *hh = w->h;
}
static void vi_draw(struct ugfx_surface *s, const void *w) { uui_volinfo_draw(s, w); }

const struct uui_widget_ops uui_volinfo_ops = {
    .natural_size = vi_natural,
    .set_geometry = vi_geom,
    .bounds       = vi_bounds,
    .draw         = vi_draw,
};
