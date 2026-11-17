// The host widget harness -- see uitest.h. Input goes to the router and
// then the focus ring in uapp_window.c's order; a change there is a
// change here, or the harness tests an order no app runs.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uitest.h"
#include "ui/utheme.h"
#include "ui/uui_anim.h"

void (*uitest_report)(int ok, const char *what);
const char *uitest_case;

static void report(int ok, const char *what) {
    if (uitest_report) uitest_report(ok, what);
    else if (!ok) fprintf(stderr, "uitest: FAIL %s\n", what);
}

// --- the font ---------------------------------------------------------------

// Liberation Sans, as fontd publishes the default face: one file per
// weight. The arenas live for the whole test, as the session's do.
static const char *const FACE[UGFX_FONT_SLOTS] = {
    [UGFX_FONT_SLOT(UGFX_FONT_FAMILY_UI, UGFX_FONT_REGULAR)]   = "/usr/share/fonts/liberation-sans.ttf",
    [UGFX_FONT_SLOT(UGFX_FONT_FAMILY_UI, UGFX_FONT_BOLD)]      = "/usr/share/fonts/liberation-sans-bold.ttf",
    [UGFX_FONT_SLOT(UGFX_FONT_FAMILY_MONO, UGFX_FONT_REGULAR)] = "/usr/share/fonts/dejavu-sans-mono.ttf",
    [UGFX_FONT_SLOT(UGFX_FONT_FAMILY_MONO, UGFX_FONT_BOLD)]    = "/usr/share/fonts/dejavu-sans-mono-bold.ttf",
};

void uitest_setup(void) {
    static struct ugfx_font fonts[UGFX_FONT_SLOTS];
    unsigned long need = ugfx_font_arena_size(UITEST_FONT_PX);
    for (int slot = 0; slot < UGFX_FONT_SLOTS; slot++) {
        void *arena = malloc(need);
        if (!arena || !ugfx_font_load(FACE[slot], UITEST_FONT_PX, 0, &fonts[slot], arena, need)) {
            fprintf(stderr, "uitest: cannot rasterize %s at %d px (is UITEST_ROOT set?)\n",
                    FACE[slot], UITEST_FONT_PX);
            exit(99);
        }
        ugfx_font_adopt(slot, &fonts[slot]);
    }
    ugfx_set_font(NULL);
    utheme_init();   // no desktop.theme set: the default palette
}

// --- the window -------------------------------------------------------------

void uitest_window(struct uitest_win *win, struct uui_item *items, int count,
                   struct uui_layout *layout, int w, int h) {
    memset(win, 0, sizeof *win);
    win->pixels = calloc((size_t)w * (size_t)h, sizeof *win->pixels);
    if (!win->pixels) { fprintf(stderr, "uitest: no memory for a %dx%d window\n", w, h); exit(99); }
    win->surface = ugfx_surface_for_pixels(win->pixels, w, h);
    win->items = items;
    win->count = count;
    win->layout = layout;
    win->mouse_x = win->mouse_y = UUI_NOWHERE;
    uitest_relayout(win);
    uui_router_init(&win->router, items, count);
    if (uui_router_duplicate_id(&win->router)) report(0, "the window's widget ids are unique");
    uitest_frame(win);
}

void uitest_window_free(struct uitest_win *win) {
    free(win->pixels);
    win->pixels = NULL;
}

void uitest_set_focus(struct uitest_win *win, struct uui_focus *focus) { win->focus = focus; }

void uitest_relayout(struct uitest_win *win) {
    if (win->layout) uui_layout_run(win->layout, 0, 0, win->surface.w, win->surface.h);
}

// The same top_paint() order: the panel ground, the layout when the
// router does not already paint it, then the router's items.
void uitest_frame(struct uitest_win *win) {
    struct ugfx_surface *s = &win->surface;
    ugfx_clear_clip_rect(s);
    ugfx_fill(s, UTHEME_PANEL_BG);
    if (win->layout && !(win->count && uui_router_covers_layout(&win->router, win->layout)))
        uui_layout_draw(s, win->layout);
    if (win->count) uui_router_draw(&win->router, s);
}

void uitest_settle(struct uitest_win *win) {
    uui_anim_take();
    uitest_frame(win);
    for (int i = 0; uui_anim_take(); i++) {
        if (i * UUI_ANIM_FRAME_MS > 10000) {
            report(0, "the window's animations settle within ten seconds");
            return;
        }
        uitest_advance_ms(UUI_ANIM_FRAME_MS);
        uitest_frame(win);
    }
}

// --- finding a widget by name -------------------------------------------------

static struct uui_item *find_in(struct uui_item *items, int count, const char *name) {
    for (int i = 0; i < count; i++) {
        struct uui_item *it = &items[i];
        if (it->name && strcmp(it->name, name) == 0) return it;
        int n = 0;
        struct uui_item *kids = it->ops && it->ops->children ? it->ops->children(it->widget, &n) : NULL;
        struct uui_item *hit = kids ? find_in(kids, n, name) : NULL;
        if (hit) return hit;
    }
    return NULL;
}

struct uui_item *uitest_find(struct uitest_win *win, const char *name) {
    struct uui_item *it = find_in(win->items, win->count, name);
    if (!it && win->layout) {
        int n = 0;
        struct uui_item *kids = uui_layout_ops.children(win->layout, &n);
        it = kids ? find_in(kids, n, name) : NULL;
    }
    if (!it) {
        char what[160];
        snprintf(what, sizeof what, "the window has a widget named \"%s\"", name);
        report(0, what);
    }
    return it;
}

int uitest_rect(struct uitest_win *win, const char *name, int *x, int *y, int *w, int *h) {
    struct uui_item *it = uitest_find(win, name);
    *x = *y = *w = *h = 0;
    if (!it) return 0;
    if (!it->ops || !it->ops->bounds) {
        char what[160];
        snprintf(what, sizeof what, "\"%s\" reports its bounds", name);
        report(0, what);
        return 0;
    }
    it->ops->bounds(it->widget, x, y, w, h);
    return 1;
}

// (dx, dy) inside a widget: from the left/top, or from the right/bottom
// edge when negative.
static int point_in(struct uitest_win *win, const char *name, int dx, int dy, int *px, int *py) {
    int x, y, w, h;
    if (!uitest_rect(win, name, &x, &y, &w, &h)) return 0;
    *px = dx >= 0 ? x + dx : x + w + dx;
    *py = dy >= 0 ? y + dy : y + h + dy;
    return 1;
}

// --- input --------------------------------------------------------------------

static int note(struct uitest_win *win, int id, int changed) {
    win->last_id = id;
    win->last_changed = changed;
    return id;
}

int uitest_press_at(struct uitest_win *win, int x, int y) {
    uitest_advance_ms(1000);
    win->mouse_x = x;
    win->mouse_y = y;
    int changed = 0;
    int id = uui_router_press(&win->router, x, y, 0, &changed);
    if (win->focus) uui_focus_click(win->focus, x, y);
    return note(win, id, changed);
}

int uitest_motion_at(struct uitest_win *win, int x, int y, unsigned buttons) {
    uitest_advance_ms(10);
    win->mouse_x = x;
    win->mouse_y = y;
    int changed = 0;
    int id = uui_router_motion(&win->router, x, y, buttons, 0, &changed);
    return note(win, id, changed);
}

int uitest_release_at(struct uitest_win *win, int x, int y) {
    uitest_advance_ms(10);
    win->mouse_x = x;
    win->mouse_y = y;
    int changed = 0;
    int id = uui_router_release(&win->router, x, y, &changed);
    return note(win, id, changed);
}

int uitest_click_at(struct uitest_win *win, int x, int y) {
    uitest_press_at(win, x, y);
    return uitest_release_at(win, x, y);
}

int uitest_drag_at(struct uitest_win *win, int x0, int y0, int x1, int y1, int steps) {
    if (steps < 1) steps = 1;
    uitest_press_at(win, x0, y0);
    for (int i = 1; i <= steps; i++)
        uitest_motion_at(win, x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps, 1);
    return uitest_release_at(win, x1, y1);
}

int uitest_wheel_at(struct uitest_win *win, int x, int y, int notches) {
    uitest_motion_at(win, x, y, 0);
    int changed = 0;
    int id = uui_router_wheel(&win->router, x, y, notches, &changed);
    return note(win, id, changed);
}

int uitest_click(struct uitest_win *win, const char *name) {
    int x, y, w, h;
    if (!uitest_rect(win, name, &x, &y, &w, &h)) return 0;
    return uitest_click_at(win, x + w / 2, y + h / 2);
}

int uitest_click_in(struct uitest_win *win, const char *name, int dx, int dy) {
    int x, y;
    return point_in(win, name, dx, dy, &x, &y) ? uitest_click_at(win, x, y) : 0;
}

int uitest_double_click_in(struct uitest_win *win, const char *name, int dx, int dy) {
    int x, y;
    if (!point_in(win, name, dx, dy, &x, &y)) return 0;
    uitest_click_at(win, x, y);
    // The second press inside the double-click time: no second-long gap.
    uitest_advance_ms(-1000 + 50);
    return uitest_click_at(win, x, y);
}

int uitest_wheel(struct uitest_win *win, const char *name, int notches) {
    int x, y, w, h;
    if (!uitest_rect(win, name, &x, &y, &w, &h)) return 0;
    return uitest_wheel_at(win, x + w / 2, y + h / 2, notches);
}

int uitest_hover(struct uitest_win *win, const char *name, int dx, int dy) {
    int x, y;
    return point_in(win, name, dx, dy, &x, &y) ? uitest_motion_at(win, x, y, 0) : 0;
}

int uitest_key(struct uitest_win *win, int key, unsigned mods) {
    int changed = 0;
    // uapp stops here only when the overlay's owner has an id; a change
    // without one goes on to the focus ring, as it does there.
    int id = uui_router_overlay_key(&win->router, key, mods, &changed);
    if (id) { note(win, id, changed); return 1; }
    if (win->focus && uui_focus_key(win->focus, key, mods)) {
        int cur = win->focus->current;
        note(win, cur >= 0 ? uui_router_id_of(&win->router, win->focus->items[cur].widget) : 0, 1);
        return 1;
    }
    note(win, 0, 0);
    return 0;
}

// --- output -------------------------------------------------------------------

uint32_t uitest_pixel_at(struct uitest_win *win, int x, int y) {
    if (x < 0 || y < 0 || x >= win->surface.w || y >= win->surface.h) {
        char what[96];
        snprintf(what, sizeof what, "pixel (%d,%d) is inside the %dx%d window",
                 x, y, win->surface.w, win->surface.h);
        report(0, what);
        return 0;
    }
    return win->pixels[(size_t)y * (size_t)win->surface.w + (size_t)x] & 0x00FFFFFFu;
}

uint32_t uitest_pixel(struct uitest_win *win, const char *name, int dx, int dy) {
    int x, y;
    return point_in(win, name, dx, dy, &x, &y) ? uitest_pixel_at(win, x, y) : 0;
}

// The REAL fopen: $UITEST_KEEP is a host path, and the wrapped one
// (uitest_host.c) would put it under the root, where no such folder is.
FILE *__real_fopen(const char *path, const char *mode);

void uitest_keep(struct uitest_win *win, const char *tag) {
    const char *dir = getenv("UITEST_KEEP");
    if (!dir || !*dir) return;
    char path[512];
    snprintf(path, sizeof path, "%s/%s-%s.ppm", dir, uitest_case ? uitest_case : "uitest", tag);
    FILE *f = __real_fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", win->surface.w, win->surface.h);
    for (int i = 0; i < win->surface.w * win->surface.h; i++) {
        uint32_t p = win->pixels[i];
        unsigned char rgb[3] = { (unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
