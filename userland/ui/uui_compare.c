// The two files of a name conflict, side by side -- see uui_compare.h.
#include "ui/uui_compare.h"
#include "ui/uui_widget.h"
#include "ui/uui_primitives.h"
#include "ui/utheme.h"
#include "lib/uimg.h"
#include "lib/human.h"
#include "lib/udate.h"
#include "rt/sys.h"
#include <caltime.h>
#include <string.h>
#include <stdio.h>

// A card's picture box, its padding and the gap between the two cards:
// font-derived, so the body reflows with the face.
static int pic_h(void) { return ugfx_char_h() * 5; }
static int pad(void)   { return ugfx_char_w(); }
static int gap(void)   { return ugfx_char_w() * 2; }
static int line_h(void) { return ugfx_char_h() + 4; }
static int card_h(void) { return pad() * 2 + line_h() + pic_h() + 6 + 2 * line_h(); }

void uui_compare_init(struct uui_compare *c, const char *rest_label) {
    memset(c, 0, sizeof *c);
    uui_checkbox_init(&c->rest, 0, 0, 0, rest_label, UUI_ROLE(field_bg), UUI_ROLE(text));
    c->rest.accent = 1;
    if (!rest_label) c->rest.label = 0;
}

static void judge(struct uui_compare *c) {
    struct uui_compare_side *a = &c->side[0], *b = &c->side[1];
    a->newer = b->newer = a->larger = b->larger = 0;
    if (a->missing || b->missing) return;
    if (a->modified != b->modified) (a->modified > b->modified ? a : b)->newer = 1;
    if (a->bytes != b->bytes) (a->bytes > b->bytes ? a : b)->larger = 1;
}

void uui_compare_set(struct uui_compare *c, int i, const char *caption, const char *path,
                     const struct uimg *picture) {
    if (i < 0 || i > 1) return;
    struct uui_compare_side *s = &c->side[i];
    memset(s, 0, sizeof *s);
    snprintf(s->caption, sizeof s->caption, "%s", caption ? caption : "");
    s->picture = picture;
    struct sys_stat st;
    if (!path || sys_stat(path, &st) != 0) {
        s->missing = 1;
        snprintf(s->size, sizeof s->size, "gone");
    } else {
        s->bytes = st.size;
        s->modified = cal_rtc_to_epoch(&st.modified);
        human_size(s->size, sizeof s->size, st.size);
        udate_format(s->when, sizeof s->when, &st.modified, UDATE_DATE | UDATE_TIME);
    }
    judge(c);
}

void uui_compare_set_picture(struct uui_compare *c, int i, const struct uimg *picture) {
    if (i >= 0 && i <= 1) c->side[i].picture = picture;
}

static int has_rest(const struct uui_compare *c) { return c->rest.label != 0; }

static void ops_natural(const void *w, int *ow, int *oh) {
    const struct uui_compare *c = (const struct uui_compare *)w;
    int cw = ugfx_char_w() * 22;
    int bw = 0, bh = 0;
    if (has_rest(c)) uui_checkbox_natural_size(&c->rest, &bw, &bh);
    if (ow) *ow = cw * 2 + gap();
    if (oh) *oh = card_h() + (has_rest(c) ? line_h() + bh : 0);
}

static void ops_geometry(void *w, int x, int y, int width, int height) {
    struct uui_compare *c = (struct uui_compare *)w;
    c->x = x; c->y = y; c->w = width; c->h = height;
    uui_checkbox_set_geometry(&c->rest, x, y + card_h() + line_h() / 2);
}

static void ops_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_compare *c = (const struct uui_compare *)w;
    if (x) *x = c->x;
    if (y) *y = c->y;
    if (ow) *ow = c->w;
    if (oh) *oh = c->h;
}

// A badge: a pill of tinted ink, its width the word's. Returns its width.
static int badge(struct ugfx_surface *s, int x, int y, const char *word, uint32_t ink) {
    int tw = ugfx_text_width(word), h = ugfx_char_h() + 2, bw = tw + 10;
    uint32_t fill = ugfx_blend(UTHEME_WHITE, ink, 40);
    uui_fill_round_rect(s, x, y, bw, h, h / 2, fill);
    ugfx_draw_string_clipped(s, x + 5, y + 1, tw, word, ink, fill);
    return bw;
}

static void draw_side(struct ugfx_surface *s, const struct uui_compare *c, int i, int x, int w) {
    const struct uui_compare_side *sd = &c->side[i];
    int y = c->y, h = card_h(), p = pad();
    uint32_t bg = UTHEME_WHITE, dim = utheme_current()->text_muted;
    uui_fill_round_rect(s, x, y, w, h, 10, bg);
    uui_stroke_round_rect(s, x, y, w, h, 10, UTHEME_OUTLINE);
    int ty = y + p;
    ugfx_draw_string_elided(s, x + p, ty, w - 2 * p, sd->caption, dim, bg);
    ty += line_h();
    // The picture, centred in its box at its own size or less.
    if (sd->picture) {
        const struct uimg *pic = sd->picture;
        int pw = pic->w, ph = pic->h, box_w = w - 2 * p, box_h = pic_h();
        if (pw > box_w || ph > box_h) {
            if (pw * box_h > ph * box_w) { ph = ph * box_w / pw; pw = box_w; }
            else { pw = pw * box_h / ph; ph = box_h; }
            ugfx_blit_scaled_alpha(s, x + p + (box_w - pw) / 2, ty + (box_h - ph) / 2, pw, ph,
                                   pic->px, pic->w, pic->h, pic->w, 255);
        } else {
            ugfx_blit_alpha(s, x + p + (box_w - pw) / 2, ty + (box_h - ph) / 2, pw, ph,
                            pic->px, pic->w);
        }
    }
    ty += pic_h() + 6;
    int kw = ugfx_text_width("Modified") + p;
    uint32_t create = utheme_action(UTHEME_ACT_CREATE);
    ugfx_draw_string_clipped(s, x + p, ty, kw, "Size", dim, bg);
    int vx = x + p + kw;
    ugfx_draw_string_clipped(s, vx, ty, w - (vx - x) - p, sd->size, UTHEME_TEXT, bg);
    vx += ugfx_text_width(sd->size) + 6;
    if (sd->larger && vx < x + w - p) badge(s, vx, ty - 1, "larger", dim);
    ty += line_h();
    ugfx_draw_string_clipped(s, x + p, ty, kw, "Modified", dim, bg);
    vx = x + p + kw;
    ugfx_draw_string_clipped(s, vx, ty, w - (vx - x) - p, sd->when, UTHEME_TEXT, bg);
    vx += ugfx_text_width(sd->when) + 6;
    if (sd->newer && vx < x + w - p) badge(s, vx, ty - 1, "newer", create);
}

static void ops_draw(struct ugfx_surface *s, const void *w) {
    const struct uui_compare *c = (const struct uui_compare *)w;
    int cw = (c->w - gap()) / 2;
    draw_side(s, c, 0, c->x, cw);
    draw_side(s, c, 1, c->x + cw + gap(), cw);
    if (has_rest(c)) uui_checkbox_draw(s, &c->rest);
}

// The whole body is the widget's; only the box answers a press.
static int ops_hit(const void *w, int cx, int cy) {
    const struct uui_compare *c = (const struct uui_compare *)w;
    return uui_hit(c->x, c->y, c->w, c->h, cx, cy);
}

static int ops_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_compare *c = (struct uui_compare *)w;
    c->pressed = has_rest(c) && uui_checkbox_hit(&c->rest, cx, cy);
    return c->pressed;   // taken, or the router has no release to bring back
}

// The box toggles on the release over it, so a press dragged off is
// a press taken back.
static int ops_release(void *w, int cx, int cy) {
    struct uui_compare *c = (struct uui_compare *)w;
    int was = c->pressed;
    c->pressed = 0;
    if (!was || !uui_checkbox_hit(&c->rest, cx, cy)) return 0;
    uui_checkbox_toggle(&c->rest);
    return 1;
}

static int ops_motion(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons;
    struct uui_compare *c = (struct uui_compare *)w;
    return has_rest(c) ? uui_checkbox_hover(&c->rest, cx, cy) : 0;
}

// Space toggles the box, as a checkbox's key does everywhere.
static int ops_key(void *w, int key, unsigned mods) {
    (void)mods;
    struct uui_compare *c = (struct uui_compare *)w;
    return has_rest(c) ? uui_checkbox_key(&c->rest, key) : 0;
}

static int ops_accepts_focus(const void *w) {
    return has_rest((const struct uui_compare *)w);
}

static void ops_set_focused(void *w, int focused) {
    ((struct uui_compare *)w)->rest.focused = focused;
}

// Each side's facts and badges, and the box -- what a test asserts.
static void ops_describe(const void *w, const struct uui_describe *d) {
    const struct uui_compare *c = (const struct uui_compare *)w;
    for (int i = 0; i < 2; i++) {
        char key[16];
        snprintf(key, sizeof key, "side%d", i);
        char v[96];
        snprintf(v, sizeof v, "%llu|%s|%s", c->side[i].bytes,
                 c->side[i].newer ? "newer" : "-", c->side[i].larger ? "larger" : "-");
        uui_describe_str(d, key, v);
    }
    uui_describe_rect(d, "rest", c->rest.x, c->rest.y, c->rest.w, c->rest.h);
    uui_describe_int(d, "rest_checked", c->rest.checked);
}

const struct uui_widget_ops uui_compare_ops = {
    .natural_size  = ops_natural,
    .set_geometry  = ops_geometry,
    .bounds        = ops_bounds,
    .draw          = ops_draw,
    .hit           = ops_hit,
    .press         = ops_press,
    .release       = ops_release,
    .motion        = ops_motion,
    .key           = ops_key,
    .accepts_focus = ops_accepts_focus,
    .set_focused   = ops_set_focused,
    .describe      = ops_describe,
};
