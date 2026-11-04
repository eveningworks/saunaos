// Which corner, edge or middle -- see ui/uui_anchor.h.
#include "ui/uui_anchor.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui_describe.h"
#include "ui/uui_primitives.h"
#include "keyboard.h"

static int cell(void) { return ugfx_char_h() + 6; }
static int gap(void)  { return 3; }
static int pitch(void) { return cell() + gap(); }

void uui_anchor_init(struct uui_anchor *a, int selected) {
    *a = (struct uui_anchor){ 0 };
    a->selected = selected >= 0 && selected < 9 ? selected : 4;
    a->hot = a->armed = -1;
}

void uui_anchor_xy(const struct uui_anchor *a, int *col, int *row) {
    *col = a->selected % 3;
    *row = a->selected / 3;
}

static int cell_at(const struct uui_anchor *a, int cx, int cy) {
    for (int i = 0; i < 9; i++)
        if (uui_hit(a->x + (i % 3) * pitch(), a->y + (i / 3) * pitch(), cell(), cell(), cx, cy)) return i;
    return -1;
}

// A pointer from the middle of a cell toward (dx, dy), one of the eight
// directions: a short stroke and a square head at its far end.
static void arrow(struct ugfx_surface *s, int x, int y, int dx, int dy, uint32_t c) {
    int m = cell() / 2, l = cell() / 4;
    for (int t = 0; t <= l; t++) ugfx_fill_rect(s, x + m + dx * t, y + m + dy * t, 1, 1, c);
    ugfx_fill_rect(s, x + m + dx * l - 1, y + m + dy * l - 1, 3, 3, c);
}

static void an_draw(struct ugfx_surface *s, const void *w) {
    const struct uui_anchor *a = w;
    int sc = a->selected % 3, sr = a->selected / 3;
    for (int i = 0; i < 9; i++) {
        int x = a->x + (i % 3) * pitch(), y = a->y + (i / 3) * pitch();
        int sel = i == a->selected;
        uint32_t face = sel ? UTHEME_ACCENT : UTHEME_WHITE;
        if (!sel && i == a->hot) face = uui_state_bg(face, UUI_STATE_HOVER);
        uui_control_frame(s, x, y, cell(), cell(), face, sel ? UTHEME_ACCENT : UTHEME_OUTLINE);
        int dc = i % 3 - sc, dr = i / 3 - sr;
        if (!sel && dc >= -1 && dc <= 1 && dr >= -1 && dr <= 1)
            arrow(s, x, y, dc, dr, ugfx_blend(UTHEME_TEXT, UTHEME_WHITE, 150));
    }
    if (a->focused) uui_focus_ring(s, a->x - 2, a->y - 2, 3 * pitch() - gap() + 4, 3 * pitch() - gap() + 4);
}

static void an_natural(const void *w, int *ow, int *oh) {
    (void)w;
    *ow = *oh = 3 * pitch() - gap();
}

static void an_geometry(void *w, int x, int y, int width, int height) {
    struct uui_anchor *a = w;
    (void)width; (void)height;
    a->x = x; a->y = y;
    a->w = a->h = 3 * pitch() - gap();
}

static void an_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_anchor *a = w;
    *x = a->x; *y = a->y; *ow = a->w; *oh = a->h;
}

static int an_hit(const void *w, int cx, int cy) { return cell_at(w, cx, cy) >= 0; }

static int an_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_anchor *a = w;
    a->armed = cell_at(a, cx, cy);
    return a->armed >= 0;
}

static int an_motion(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons;
    struct uui_anchor *a = w;
    int h = cell_at(a, cx, cy);
    if (h == a->hot) return 0;
    a->hot = h;
    return 1;
}

static int an_release(void *w, int cx, int cy) {
    struct uui_anchor *a = w;
    int at = cell_at(a, cx, cy), was = a->armed;
    a->armed = -1;
    if (was >= 0 && at == was) a->selected = at;
    return was >= 0;
}

static int an_key(void *w, int key, unsigned mods) {
    (void)mods;
    struct uui_anchor *a = w;
    int c = a->selected % 3, r = a->selected / 3;
    if (key == KEY_ARROW_LEFT && c > 0) c--;
    else if (key == KEY_ARROW_RIGHT && c < 2) c++;
    else if (key == KEY_ARROW_UP && r > 0) r--;
    else if (key == KEY_ARROW_DOWN && r < 2) r++;
    else return 0;
    a->selected = r * 3 + c;
    return 1;
}

static int an_accepts_focus(const void *w) { (void)w; return 1; }
static void an_set_focused(void *w, int f) { ((struct uui_anchor *)w)->focused = f; }

static void an_describe(const void *w, const struct uui_describe *d) {
    const struct uui_anchor *a = w;
    uui_describe_int(d, "selected", a->selected);
    for (int i = 0; i < 9; i++)
        uui_describe_rect_i(d, "cell", i, a->x + (i % 3) * pitch(), a->y + (i / 3) * pitch(), cell(), cell());
}

const struct uui_widget_ops uui_anchor_ops = {
    .natural_size  = an_natural,
    .set_geometry  = an_geometry,
    .bounds        = an_bounds,
    .draw          = an_draw,
    .hit           = an_hit,
    .press         = an_press,
    .motion        = an_motion,
    .release       = an_release,
    .key           = an_key,
    .accepts_focus = an_accepts_focus,
    .set_focused   = an_set_focused,
    .describe      = an_describe,
};
