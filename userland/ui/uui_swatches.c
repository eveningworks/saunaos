// A grid of colours to pick from -- see ui/uui_swatches.h.
#include "ui/uui_swatches.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui_describe.h"
#include "ui/uui_primitives.h"

// A cell is a line of text tall, so the grid reflows with the font.
static int cell(void) { return ugfx_char_h() + 4; }
static int gap(void)  { return 3; }
static int pitch(void) { return cell() + gap(); }

static int cols_of(const struct uui_swatches *s) {
    int c = (s->w + gap()) / pitch();
    return c < 1 ? 1 : c;
}

void uui_swatches_init(struct uui_swatches *s, const uint32_t *colours, int count, int cols) {
    *s = (struct uui_swatches){ 0 };
    s->colours = colours;
    s->count = count;
    s->cols = cols > 0 ? cols : 8;
    s->selected = s->hot = s->armed = s->picked = -1;
}

void uui_swatches_set_count(struct uui_swatches *s, int count) {
    s->count = count;
    if (s->selected >= count) s->selected = -1;
    if (s->hot >= count) s->hot = -1;
}

static void cell_rect(const struct uui_swatches *s, int i, int *x, int *y) {
    int c = cols_of(s);
    *x = s->x + (i % c) * pitch();
    *y = s->y + (i / c) * pitch();
}

int uui_swatches_at(const struct uui_swatches *s, int cx, int cy) {
    for (int i = 0; i < s->count; i++) {
        int x, y;
        cell_rect(s, i, &x, &y);
        if (uui_hit(x, y, cell(), cell(), cx, cy)) return i;
    }
    return -1;
}

int uui_swatches_take(struct uui_swatches *s, uint32_t *rgb) {
    if (s->picked < 0 || s->picked >= s->count) { s->picked = -1; return 0; }
    if (rgb) *rgb = s->colours[s->picked];
    s->picked = -1;
    return 1;
}

static void sw_natural(const void *w, int *ow, int *oh) {
    const struct uui_swatches *s = w;
    int rows = (s->count + s->cols - 1) / s->cols;
    if (rows < 1) rows = 1;
    if (ow) *ow = s->cols * pitch() - gap();
    if (oh) *oh = rows * pitch() - gap();
}

static void sw_geometry(void *w, int x, int y, int width, int height) {
    struct uui_swatches *s = w;
    s->x = x; s->y = y; s->w = width; s->h = height;
}

static void sw_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_swatches *s = w;
    *x = s->x; *y = s->y; *ow = s->w; *oh = s->h;
}

static void sw_draw(struct ugfx_surface *g, const void *w) {
    const struct uui_swatches *s = w;
    int c = cell();
    for (int i = 0; i < s->count; i++) {
        int x, y;
        cell_rect(s, i, &x, &y);
        if (y + c > s->y + s->h) break;   // past the room it was given
        // A frame in the outline colour, so white reads on a white panel.
        ugfx_fill_rect(g, x, y, c, c, s->colours[i] & 0xFFFFFF);
        ugfx_draw_rect(g, x, y, c, c, i == s->hot ? UTHEME_TEXT : UTHEME_OUTLINE);
        if (i == s->selected) {
            ugfx_draw_rect(g, x - 2, y - 2, c + 4, c + 4, UTHEME_ACCENT);
            ugfx_draw_rect(g, x - 1, y - 1, c + 2, c + 2, UTHEME_ACCENT);
        }
    }
}

static int sw_hit(const void *w, int cx, int cy) {
    return uui_swatches_at(w, cx, cy) >= 0;
}

static int sw_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_swatches *s = w;
    s->armed = uui_swatches_at(s, cx, cy);
    return s->armed >= 0;
}

static int sw_motion(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons;
    struct uui_swatches *s = w;
    int h = uui_swatches_at(s, cx, cy);
    if (h == s->hot) return 0;
    s->hot = h;
    return 1;
}

// Commits only where it was armed: a press dragged off is cancelled.
static int sw_release(void *w, int cx, int cy) {
    struct uui_swatches *s = w;
    int at = uui_swatches_at(s, cx, cy);
    int was = s->armed;
    s->armed = -1;
    if (was < 0 || at != was) return was >= 0;
    s->picked = at;
    return 1;
}

static void sw_describe(const void *w, const struct uui_describe *d) {
    const struct uui_swatches *s = w;
    uui_describe_int(d, "count", s->count);
    uui_describe_int(d, "selected", s->selected);
    for (int i = 0; i < s->count; i++) {
        int x, y;
        cell_rect(s, i, &x, &y);
        uui_describe_rect_i(d, "cell", i, x, y, cell(), cell());
    }
}

const struct uui_widget_ops uui_swatches_ops = {
    .natural_size = sw_natural,
    .set_geometry = sw_geometry,
    .bounds       = sw_bounds,
    .draw         = sw_draw,
    .hit          = sw_hit,
    .press        = sw_press,
    .motion       = sw_motion,
    .release      = sw_release,
    .describe     = sw_describe,
};
