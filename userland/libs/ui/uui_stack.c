// See ui/uui_stack.h.
#include "ui/uui_stack.h"

void uui_stack_set(struct uui_stack *s, int index) {
    if (index < 0 || index >= s->count) return;
    s->current = index;
    for (int i = 0; i < s->count; i++) s->pages[i].hidden = i != index;
}

void uui_stack_init(struct uui_stack *s, struct uui_item *pages, int count) {
    s->x = s->y = s->w = s->h = 0;
    s->pages = pages;
    s->count = count;
    uui_stack_set(s, 0);
}

int uui_stack_show(struct uui_stack *s, const struct uui_item *page) {
    for (int i = 0; i < s->count; i++)
        if (&s->pages[i] == page) { uui_stack_set(s, i); return 1; }
    return 0;
}

// EVERY page, hidden or not: the size must not depend on which is shown.
static void stack_natural(const void *w, int *out_w, int *out_h) {
    const struct uui_stack *s = w;
    int mw = 0, mh = 0;
    for (int i = 0; i < s->count; i++) {
        const struct uui_item *p = &s->pages[i];
        int pw = 0, ph = 0;
        if (p->ops && p->ops->natural_size) p->ops->natural_size(p->widget, &pw, &ph);
        if (pw > mw) mw = pw;
        if (ph > mh) mh = ph;
    }
    if (out_w) *out_w = mw;
    if (out_h) *out_h = mh;
}

static void stack_geometry(void *w, int x, int y, int width, int height) {
    struct uui_stack *s = w;
    s->x = x; s->y = y; s->w = width; s->h = height;
    for (int i = 0; i < s->count; i++) {
        struct uui_item *p = &s->pages[i];
        if (p->ops && p->ops->set_geometry) p->ops->set_geometry(p->widget, x, y, width, height);
    }
}

static void stack_draw(struct ugfx_surface *surf, const void *w) {
    const struct uui_stack *s = w;
    if (s->current < 0 || s->current >= s->count) return;
    const struct uui_item *p = &s->pages[s->current];
    if (!p->hidden && p->ops && p->ops->draw) p->ops->draw(surf, p->widget);
}

// A container, as uui_layout is: no `hit`, so the router recurses into the
// shown page and each child answers for itself.
static struct uui_item *stack_children(void *w, int *out_count) {
    struct uui_stack *s = w;
    *out_count = s->count;
    return s->pages;
}

static void stack_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_stack *s = w;
    *x = s->x; *y = s->y; *ow = s->w; *oh = s->h;
}

const struct uui_widget_ops uui_stack_ops = {
    .children = stack_children,
    .natural_size = stack_natural,
    .set_geometry = stack_geometry,
    .draw = stack_draw,
    .bounds = stack_bounds,
};
