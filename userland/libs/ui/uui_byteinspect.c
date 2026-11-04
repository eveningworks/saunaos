// A data inspector (ui/uui_byteinspect.h).
#include "ui/uui_byteinspect.h"
#include "ui/uui_primitives.h"
#include "ui/uui_ctxmenu.h"
#include "ui/utheme.h"
#include "base/ubyteval.h"
#include "media/uclip.h"

#include <stdio.h>
#include <string.h>

enum { P_SEG0 = 0, P_SEG1, P_GOTO, P_B0, P_B1, P_B2, P_COUNT };

static const char *const ENDIAN[] = { "Little", "Big" };
static const char *const BTN[3] = { "Copy as hex", "Copy as C array", "Save as..." };

// ONE derivation of where everything is, which draw, hit and describe all call.
struct lay {
    int pad, lh, row_h, name_w;
    int head_y;
    int card_x, card_w, card_y, card_h, rows_y;
    int link_y, link_x, link_w;
    int sel_y, card2_y, card2_h;
    int bx[3], by[3], bw[3], bh;
};

static void layout(struct uui_byteinspect *bi, struct lay *l) {
    l->pad = utheme_pad() * 2;
    l->lh = ugfx_char_h();
    l->row_h = l->lh + ugfx_px(6);
    l->name_w = ugfx_text_width("Float64") + l->lh;
    l->card_x = bi->x + l->pad;
    l->card_w = bi->w - 2 * l->pad;
    l->head_y = bi->y + l->pad;
    int sw, sh;
    uui_segmented_natural_size(&bi->seg, &sw, &sh);
    uui_segmented_set_geometry(&bi->seg, bi->x + bi->w - l->pad - sw, l->head_y + (l->lh - sh) / 2);
    l->card_y = l->head_y + (sh > l->lh ? sh : l->lh) + l->pad;
    l->rows_y = l->card_y + l->pad / 2;
    l->card_h = UBV_ROWS * l->row_h + l->pad;
    l->link_y = l->card_y + l->card_h + l->pad;
    l->link_x = l->card_x + ugfx_text_width("As an offset: 0x0000000000000000 ") ;
    l->link_w = ugfx_text_width("Go to") + 4;
    l->sel_y = l->link_y + l->lh + 2 * l->pad;
    l->card2_y = l->sel_y + l->lh + l->pad;
    l->bh = utheme_control_h();
    int inner = l->card_x + l->pad / 2, iw = l->card_w - l->pad;
    int line = l->card2_y + l->pad / 2;
    int by = line + 2 * l->row_h;
    int half = (iw - l->pad / 2) / 2;
    l->bx[0] = inner;                   l->bw[0] = half;
    l->bx[1] = inner + half + l->pad / 2; l->bw[1] = iw - half - l->pad / 2;
    l->bx[2] = inner;                   l->bw[2] = half;
    l->by[0] = l->by[1] = by;
    l->by[2] = by + l->bh + l->pad / 2;
    l->card2_h = l->by[2] + l->bh + l->pad / 2 - l->card2_y;
    for (int i = 0; i < 3; i++) uui_button_set_geometry(&bi->btn[i], l->bx[i], l->by[i], l->bw[i], l->bh);
}

void uui_byteinspect_init(struct uui_byteinspect *bi) {
    memset(bi, 0, sizeof *bi);
    uui_segmented_init(&bi->seg, ENDIAN, 2, 0);
    for (int i = 0; i < 3; i++) {
        uui_button_init(&bi->btn[i], 0, 0, 1, 1, BTN[i], UUI_ROLE(control_bg), UUI_ROLE(text), i + 1);
        bi->btn[i].outlined = 1;
    }
    bi->hot = bi->armed = bi->menu_row = -1;
}

void uui_byteinspect_set(struct uui_byteinspect *bi, uint64_t at, const uint8_t *bytes, int avail,
                         uint64_t size) {
    bi->have = 1;
    bi->at = at;
    bi->size = size;
    bi->avail = avail < 0 ? 0 : avail > 8 ? 8 : avail;
    memset(bi->bytes, 0, sizeof bi->bytes);
    if (bytes && bi->avail) memcpy(bi->bytes, bytes, (size_t)bi->avail);
}

void uui_byteinspect_set_selection(struct uui_byteinspect *bi, int has, uint64_t lo, uint64_t len,
                                   const uint8_t *preview, int n) {
    bi->has_sel = has;
    bi->sel_lo = lo;
    bi->sel_len = len;
    bi->preview_n = n < 0 ? 0 : n > UUI_BYTEINSPECT_PREVIEW ? UUI_BYTEINSPECT_PREVIEW : n;
    if (preview && bi->preview_n) memcpy(bi->preview, preview, (size_t)bi->preview_n);
}

int uui_byteinspect_offset(const struct uui_byteinspect *bi, uint64_t *out) {
    return bi->have && ubyteval_as_offset(bi->bytes, bi->avail, bi->big_endian, bi->size, out);
}

int uui_byteinspect_take(struct uui_byteinspect *bi) {
    int e = bi->event;
    bi->event = UUI_BI_NONE;
    return e;
}

static int part_enabled(const struct uui_byteinspect *bi, int p) {
    uint64_t o;
    switch (p) {
    case P_SEG0: case P_SEG1: return bi->have;
    case P_GOTO: return uui_byteinspect_offset(bi, &o);
    case P_B0: case P_B1: return bi->has_sel;
    case P_B2: return bi->has_sel && bi->save_ok;
    default: return 0;
    }
}

static int part_at(struct uui_byteinspect *bi, int cx, int cy) {
    struct lay l;
    layout(bi, &l);
    int s = uui_segmented_at(&bi->seg, cx, cy);
    if (s == 0 || s == 1) return P_SEG0 + s;
    if (uui_hit(l.link_x, l.link_y, l.link_w, l.lh, cx, cy)) return P_GOTO;
    for (int i = 0; i < 3; i++)
        if (uui_hit(l.bx[i], l.by[i], l.bw[i], l.bh, cx, cy)) return P_B0 + i;
    return -1;
}

static int row_at(struct uui_byteinspect *bi, int cx, int cy) {
    struct lay l;
    layout(bi, &l);
    if (!uui_hit(l.card_x, l.rows_y, l.card_w, UBV_ROWS * l.row_h, cx, cy)) return -1;
    return (cy - l.rows_y) / l.row_h;
}

static uint32_t pane_bg(void) { return ugfx_blend(UTHEME_CHROME, UTHEME_WHITE, 110); }

static void card(struct ugfx_surface *s, int x, int y, int w, int h) {
    int r = uui_control_radius(0);
    uui_fill_round_rect(s, x, y, w, h, r, UTHEME_BORDER);
    uui_fill_round_rect(s, x + 1, y + 1, w - 2, h - 2, uui_control_radius(1), UTHEME_WHITE);
}

static void bi_draw(struct ugfx_surface *s, const void *w) {
    struct uui_byteinspect *bi = (struct uui_byteinspect *)(uintptr_t)w;
    struct lay l;
    layout(bi, &l);
    uint32_t bg = pane_bg(), fg = UTHEME_TEXT, muted = utheme_current()->text_muted, white = UTHEME_WHITE;
    ugfx_fill_rect(s, bi->x, bi->y, bi->w, bi->h, bg);
    ugfx_fill_rect(s, bi->x, bi->y, 1, bi->h, UTHEME_SEPARATOR);
    struct ugfx_clip saved;
    ugfx_clip_save(s, &saved);
    ugfx_clip_intersect(s, bi->x, bi->y, bi->w, bi->h);

    char head[48];
    if (bi->have) snprintf(head, sizeof head, "Value at 0x%llX", (unsigned long long)bi->at);
    else snprintf(head, sizeof head, "Value");
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string_clipped(s, bi->x + l.pad, l.head_y, bi->seg.x - bi->x - 2 * l.pad, head,
                             utheme_action(UTHEME_ACT_VIEW), bg);
    ugfx_set_font(was);
    bi->seg.selected = bi->big_endian;
    bi->seg.disabled = !bi->have;
    bi->seg.hovered = bi->hot == P_SEG0 ? 0 : bi->hot == P_SEG1 ? 1 : -1;
    uui_segmented_draw(s, &bi->seg);

    card(s, l.card_x, l.card_y, l.card_w, l.card_h);
    for (int r = 0; r < UBV_ROWS; r++) {
        int ry = l.rows_y + r * l.row_h, ty = ry + (l.row_h - l.lh) / 2;
        if (r) ugfx_fill_rect(s, l.card_x + l.pad / 2, ry, l.card_w - l.pad, 1, ugfx_blend(white, UTHEME_SEPARATOR, 120));
        ugfx_draw_string_clipped(s, l.card_x + l.pad / 2, ty, l.name_w, ubyteval_name(r), muted, white);
        char val[96] = "";
        int shown = bi->have && ubyteval_row(bi->bytes, bi->avail, bi->big_endian, r, val, sizeof val);
        if (!bi->have) strlcpy(val, "no file", sizeof val);
        const struct ugfx_font *f = shown && bi->mono ? ugfx_set_font(bi->mono) : 0;
        ugfx_draw_string_clipped(s, l.card_x + l.pad / 2 + l.name_w, ty + (l.lh - ugfx_char_h()) / 2,
                                 l.card_w - l.pad - l.name_w, val, shown ? fg : muted, white);
        if (f) ugfx_set_font(f);
    }

    uint64_t o;
    int has_off = uui_byteinspect_offset(bi, &o);
    char line[64];
    if (has_off) snprintf(line, sizeof line, "As an offset: 0x%llX", (unsigned long long)o);
    else snprintf(line, sizeof line, "As an offset: none in the file");
    ugfx_draw_string_clipped(s, l.card_x, l.link_y, l.link_x - l.card_x - 4, line, has_off ? fg : muted, bg);
    if (has_off) {
        uint32_t link = utheme_action(UTHEME_ACT_NAV);
        ugfx_draw_string_clipped(s, l.link_x + 2, l.link_y, l.link_w, "Go to", link, bg);
        if (bi->hot == P_GOTO) ugfx_fill_rect(s, l.link_x + 2, l.link_y + l.lh - 1, l.link_w - 4, 1, link);
    }

    was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string_clipped(s, l.card_x, l.sel_y, l.card_w, "Selection", utheme_action(UTHEME_ACT_EDIT), bg);
    ugfx_set_font(was);
    card(s, l.card_x, l.card2_y, l.card_w, l.card2_h);
    int lx = l.card_x + l.pad / 2, lw = l.card_w - l.pad, ly = l.card2_y + l.pad / 2;
    char range[96], prev[64] = "";
    if (bi->has_sel) {
        snprintf(range, sizeof range, "0x%llX - 0x%llX, %llu byte%s", (unsigned long long)bi->sel_lo,
                 (unsigned long long)(bi->sel_lo + bi->sel_len - 1), (unsigned long long)bi->sel_len,
                 bi->sel_len == 1 ? "" : "s");
        ubyteval_hex_text(bi->preview, (size_t)bi->preview_n, prev, sizeof prev);
        if (bi->sel_len > (uint64_t)bi->preview_n) strlcat(prev, " ...", sizeof prev);
    } else {
        snprintf(range, sizeof range, "Nothing selected");
    }
    ugfx_draw_string_clipped(s, lx, ly + (l.row_h - l.lh) / 2, lw, range, bi->has_sel ? fg : muted, white);
    const struct ugfx_font *f = bi->mono ? ugfx_set_font(bi->mono) : 0;
    ugfx_draw_string_clipped(s, lx, ly + l.row_h + (l.row_h - ugfx_char_h()) / 2, lw, prev, fg, white);
    if (f) ugfx_set_font(f);
    for (int i = 0; i < 3; i++) {
        bi->btn[i].disabled = !part_enabled(bi, P_B0 + i);
        bi->btn[i].hovered = bi->hot == P_B0 + i;
        bi->btn[i].pressed = bi->armed == P_B0 + i && bi->hot == P_B0 + i;
        uui_button_draw_one(s, &bi->btn[i]);
    }
    ugfx_clip_restore(s, &saved);
}

static void bi_natural(const void *w, int *ow, int *oh) {
    struct uui_byteinspect *bi = (struct uui_byteinspect *)(uintptr_t)w;
    *ow = ugfx_char_h() * 19;
    struct uui_byteinspect probe = *bi;
    probe.x = probe.y = 0;
    probe.w = *ow;
    struct lay l;
    layout(&probe, &l);
    *oh = l.card2_y + l.card2_h + l.pad;
}

static void bi_geometry(void *w, int x, int y, int width, int height) {
    struct uui_byteinspect *bi = w;
    bi->x = x; bi->y = y; bi->w = width; bi->h = height;
}

static void bi_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_byteinspect *bi = w;
    *x = bi->x; *y = bi->y; *ow = bi->w; *oh = bi->h;
}

static int bi_hit(const void *w, int cx, int cy) {
    const struct uui_byteinspect *bi = w;
    return uui_hit(bi->x, bi->y, bi->w, bi->h, cx, cy);
}

static int bi_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_byteinspect *bi = w;
    int p = part_at(bi, cx, cy);
    bi->armed = p >= 0 && part_enabled(bi, p) ? p : -1;
    bi->hot = bi->armed;
    return 1;
}

static int bi_motion(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons;
    struct uui_byteinspect *bi = w;
    int p = part_at(bi, cx, cy);
    if (p >= 0 && !part_enabled(bi, p)) p = -1;
    if (p == bi->hot) return 0;
    bi->hot = p;
    return 1;
}

// A part commits on RELEASE over the part it was pressed on.
static int bi_release(void *w, int cx, int cy) {
    struct uui_byteinspect *bi = w;
    int was = bi->armed;
    bi->armed = -1;
    if (was < 0 || part_at(bi, cx, cy) != was) return 1;
    switch (was) {
    case P_SEG0: case P_SEG1:
        if (bi->big_endian != (was == P_SEG1)) {
            bi->big_endian = was == P_SEG1;
            bi->event = UUI_BI_ENDIAN;
        }
        break;
    case P_GOTO: bi->event = UUI_BI_GOTO; break;
    case P_B0: bi->event = UUI_BI_COPY_HEX; break;
    case P_B1: bi->event = UUI_BI_COPY_C; break;
    case P_B2: bi->event = UUI_BI_SAVE; break;
    }
    return 1;
}

static int bi_cursor(const void *w, int cx, int cy) {
    struct uui_byteinspect *bi = (struct uui_byteinspect *)(uintptr_t)w;
    int p = part_at(bi, cx, cy);
    return p == P_GOTO && part_enabled(bi, p) ? WIN_CURSOR_HAND : WIN_CURSOR_DEFAULT;
}

// --- a value row's own menu: copy it -------------------------------------------------

enum { M_COPY = 1, M_COPY_ALL };
static const struct uui_menu_item MENU[] = {
    UUI_MENU("Copy value", M_COPY, 0),
    UUI_MENU("Copy all values", M_COPY_ALL, 0),
};

static unsigned menu_flags(void *w, int code) {
    struct uui_byteinspect *bi = w;
    char v[96];
    if (!bi->have) return UUI_MI_DISABLED;
    if (code == M_COPY && !ubyteval_row(bi->bytes, bi->avail, bi->big_endian, bi->menu_row, v, sizeof v))
        return UUI_MI_DISABLED;
    return 0;
}

static int menu_run(void *w, int code) {
    struct uui_byteinspect *bi = w;
    char all[UBV_ROWS * 112] = "", v[96];
    for (int r = 0; r < UBV_ROWS; r++) {
        if (code == M_COPY && r != bi->menu_row) continue;
        if (!ubyteval_row(bi->bytes, bi->avail, bi->big_endian, r, v, sizeof v)) continue;
        char line[112];
        snprintf(line, sizeof line, code == M_COPY ? "%.0s%s" : "%s: %s\n", ubyteval_name(r), v);
        strlcat(all, line, sizeof all);
    }
    if (all[0]) uclip_set_text(all, (int)strlen(all));
    return 0;
}

static int bi_context_menu(void *w, int cx, int cy, struct uui_ctxmenu_src *out) {
    struct uui_byteinspect *bi = w;
    int r = row_at(bi, cx, cy);
    if (r < 0 || !bi->have) return 0;
    bi->menu_row = r;
    out->w = bi;
    out->items = MENU;
    out->count = (int)(sizeof MENU / sizeof MENU[0]);
    out->flags = menu_flags;
    out->run = menu_run;
    return 1;
}

static const char *const PART_NAMES[] = { "binary", "int8", "int16", "int32", "int64", "hex64",
                                          "float32", "float64", "utf8" };

static void bi_describe(const void *w, const struct uui_describe *d) {
    struct uui_byteinspect *bi = (struct uui_byteinspect *)(uintptr_t)w;
    struct lay l;
    layout(bi, &l);
    uui_describe_rect(d, "endian", bi->seg.x, bi->seg.y, bi->seg.w, bi->seg.h);
    uui_describe_rect(d, "goto", l.link_x, l.link_y, l.link_w, l.lh);
    static const char *const bn[3] = { "copy_hex", "copy_c", "save" };
    for (int i = 0; i < 3; i++) uui_describe_rect(d, bn[i], l.bx[i], l.by[i], l.bw[i], l.bh);
    for (int r = 0; r < UBV_ROWS; r++) {
        char v[96];
        if (bi->have && ubyteval_row(bi->bytes, bi->avail, bi->big_endian, r, v, sizeof v))
            uui_describe_str(d, PART_NAMES[r], v);
        else
            uui_describe_str(d, PART_NAMES[r], "-");
    }
}

const struct uui_widget_ops uui_byteinspect_ops = {
    .natural_size = bi_natural,
    .set_geometry = bi_geometry,
    .bounds = bi_bounds,
    .draw = bi_draw,
    .hit = bi_hit,
    .press = bi_press,
    .motion = bi_motion,
    .release = bi_release,
    .cursor = bi_cursor,
    .context_menu = bi_context_menu,
    .describe = bi_describe,
};
