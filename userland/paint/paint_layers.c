// Paint's layers: the stack, the picture as shown, and every change to
// the stack as one undo step. One of Paint's units -- see paint.h.
//
// A LAYER'S PIXELS ARE SHARED, its name, opacity and visibility are the
// stack's: `struct pix` is a buffer with a count of the stacks that name
// it, and a stack is a small array of (pix, name, opacity, hidden). So a
// change to the stack -- add, delete, move, rename, merge, a rotate of
// every layer -- is the stack BEFORE and the stack AFTER, two arrays,
// and the history holds a pix alive exactly as long as some step's stack
// names it. Pixels a layer change makes (a merge, a rotate) are new
// buffers; nothing a step names is ever written again, except by the
// strokes the history records on it (lib/upixundo.h's own steps).
//
// THE VIEW is what the canvas shows: one opaque, visible layer is shown
// as itself, and anything else is composited (lib/upixel.h's
// upix_over(), bottom up over transparency) into a picture of its own,
// over only the rect a change touched.
#include "paint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/ulog.h"

struct pix {
    struct uimg img;
    int refs;        // the stacks that name it: the current one and the history's
};

// A text layer's words: counted as pixels are, never written once named.
struct textrec {
    struct paint_text t;
    int refs;
};

struct layer {
    struct pix *pix;
    struct textrec *text;   // NULL for a layer of pixels
    char name[PAINT_LAYER_NAME];
    int opacity;     // 0..255
    int hidden;
};

struct stack {
    struct layer l[PAINT_LAYERS_MAX];
    int n, active;
};

static struct stack g_stack;
// SCRATCH STACKS, file-scope because a stack is a few KB and every
// change builds one: what the next stack will be, the one being let go,
// and the live box's current one. Paint's window is one thread.
static struct stack g_next, g_old, g_now;
static struct uimg g_view;        // the composite, when one is needed
static int g_direct;              // the view IS the one layer
static unsigned g_serial;         // names "Layer 2", "Layer 3"...
static struct uui_layerrow g_rows[PAINT_LAYERS_MAX];
static int g_op_before = -1;      // the opacity a slider drag started from

struct uimg *g_layer;

// --- the counts --------------------------------------------------------

static void pix_put(struct pix *p) {
    if (p && --p->refs <= 0) {
        uimg_free(&p->img);
        free(p);
    }
}

static void text_put(struct textrec *t) {
    if (t && --t->refs <= 0) free(t);
}

static void stack_ref(const struct stack *s, int d) {
    for (int i = 0; i < s->n; i++) {
        if (d > 0) {
            s->l[i].pix->refs++;
            if (s->l[i].text) s->l[i].text->refs++;
        } else {
            pix_put(s->l[i].pix);
            text_put(s->l[i].text);
        }
    }
}

static struct pix *pix_new(int w, int h, uint32_t fill) {
    struct pix *p = calloc(1, sizeof *p);
    if (!p) return 0;
    p->img.px = malloc((size_t)w * h * 4);
    if (!p->img.px) { free(p); return 0; }
    p->img.w = w;
    p->img.h = h;
    p->img.has_alpha = 1;
    for (size_t i = 0; i < (size_t)w * h; i++) p->img.px[i] = fill;
    return p;
}

static struct pix *pix_adopt(struct uimg *im) {
    struct pix *p = calloc(1, sizeof *p);
    if (!p) return 0;
    p->img = *im;
    *im = (struct uimg){ 0 };
    return p;
}

static struct pix *pix_copy(const struct uimg *im) {
    struct pix *p = pix_new(im->w, im->h, 0);
    if (p) memcpy(p->img.px, im->px, (size_t)im->w * im->h * 4);
    return p;
}

// --- the view ------------------------------------------------------------

static int pic_w(void) { return g_stack.n ? g_stack.l[0].pix->img.w : 0; }
static int pic_h(void) { return g_stack.n ? g_stack.l[0].pix->img.h : 0; }

const struct uimg *layers_view(void) {
    return g_direct ? &g_stack.l[0].pix->img : &g_view;
}

static void composite(const struct upix_rect *r) {
    struct upix_rect all = { 0, 0, pic_w(), pic_h() };
    if (!r || r->w <= 0) r = &all;
    struct upix_rect c = { 0, 0, 0, 0 };
    upix_rect_add(&c, &g_view, r->x, r->y, r->w, r->h);
    for (int y = c.y; y < c.y + c.h; y++)
        memset(g_view.px + (size_t)y * g_view.w + c.x, 0, (size_t)c.w * 4);
    for (int i = 0; i < g_stack.n; i++)
        if (!g_stack.l[i].hidden) upix_over(&g_view, &g_stack.l[i].pix->img, &c, g_stack.l[i].opacity);
}

// After the stack changed: the active layer, the rows, and the view
// rebuilt whole.
static void restack(void) {
    if (g_stack.active >= g_stack.n) g_stack.active = g_stack.n - 1;
    if (g_stack.active < 0) g_stack.active = 0;
    g_layer = g_stack.n ? &g_stack.l[g_stack.active].pix->img : 0;
    for (int i = 0; i < g_stack.n; i++)
        g_rows[i] = (struct uui_layerrow){ g_stack.l[i].name, &g_stack.l[i].pix->img, g_stack.l[i].opacity,
                                           g_stack.l[i].hidden, g_stack.l[i].text != 0 };
    const struct layer *b = &g_stack.l[0];
    g_direct = g_stack.n == 1 && !b->hidden && b->opacity == 255;
    if (!g_direct) {
        int w = pic_w(), h = pic_h();
        if (g_view.w != w || g_view.h != h || !g_view.px) {
            uimg_free(&g_view);
            g_view.px = malloc((size_t)w * h * 4);
            if (g_view.px) { g_view.w = w; g_view.h = h; g_view.has_alpha = 1; }
        }
        if (g_view.px) composite(0);
        else g_direct = 1;   // no memory for a composite: the bottom layer alone, rather than nothing
    }
    paint_layers_changed();
}

void layers_update(const struct upix_rect *r) {
    if (!g_direct && g_view.px) composite(r);
}

// --- the history's steps ------------------------------------------------------

struct step { struct stack before, after; };

static void set_current(const struct stack *next) {
    g_old = g_stack;
    stack_ref(next, +1);
    g_stack = *next;
    stack_ref(&g_old, -1);
    restack();
}

static void step_apply(void *p, int redo) {
    struct step *s = p;
    set_current(redo ? &s->after : &s->before);
}

static void step_drop(void *p) {
    struct step *s = p;
    stack_ref(&s->before, -1);
    stack_ref(&s->after, -1);
    free(s);
}

static const struct upixundo_custom STEP = { step_apply, step_drop };

// `next` becomes the stack, as one undo step costing `bytes` of new
// pixels. The pixels it adds must come with no count of their own.
static void change(const struct stack *next, size_t bytes, const char *what) {
    struct step *s = malloc(sizeof *s);
    if (s) {
        s->before = g_stack;
        s->after = *next;
        stack_ref(&s->before, +1);
        stack_ref(&s->after, +1);
    }
    set_current(next);
    if (s) upixundo_push(&g_undo, &STEP, s, sizeof *s + bytes);
    ulogf("paint: layers %s -> %d, current %d\n", what, g_stack.n, g_stack.active);
}

// --- what Paint asks --------------------------------------------------------------

int layers_count(void) { return g_stack.n; }
int layers_active(void) { return g_stack.active; }
const struct uui_layerrow *layers_rows(void) { return g_rows; }
int layers_opacity(int i) { return i >= 0 && i < g_stack.n ? g_stack.l[i].opacity : 255; }
const char *layers_name(int i) { return i >= 0 && i < g_stack.n ? g_stack.l[i].name : ""; }

static void default_name(char *out, size_t cap) { snprintf(out, cap, "Layer %u", ++g_serial); }

static int room(void) {
    if (g_stack.n < PAINT_LAYERS_MAX) return 1;
    paint_note("A picture holds at most 32 layers");
    return 0;
}

// --- text layers, and how .ora carries them ----------------------------------

static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static void put_attr(struct uora_layer *l, const char *name, const char *value) {
    if (l->nattrs >= UORA_ATTRS_MAX) return;
    char *v = dup_str(value);
    if (!v) return;
    snprintf(l->attrs[l->nattrs].name, sizeof l->attrs[0].name, "%s", name);
    l->attrs[l->nattrs++].value = v;
}

static void text_to_attrs(const struct paint_text *t, struct uora_layer *l) {
    char v[64];
    put_attr(l, "text", t->buf);
    snprintf(v, sizeof v, "%d %d %d %d %d", t->x, t->y, t->wrap, t->w, t->h);
    put_attr(l, "box", v);
    put_attr(l, "font", t->font.family);
    snprintf(v, sizeof v, "%d %d %d %d %d", t->font.px, t->font.bold, t->font.italic, t->font.underline,
             t->font.strike);
    put_attr(l, "face", v);
    snprintf(v, sizeof v, "%08X %08X %d", (unsigned)t->ink, (unsigned)t->fill_colour, t->fill);
    put_attr(l, "ink", v);
}

static const char *get_attr(const struct uora_layer *l, const char *name) {
    for (int k = 0; k < l->nattrs; k++)
        if (!strcmp(l->attrs[k].name, name)) return l->attrs[k].value;
    return 0;
}

// A layer read from .ora with our words on it, or NULL: anything missing
// or malformed leaves it the pixels it is.
static struct textrec *text_from_attrs(const struct uora_layer *l) {
    const char *text = get_attr(l, "text"), *box = get_attr(l, "box"), *font = get_attr(l, "font"),
               *face = get_attr(l, "face"), *ink = get_attr(l, "ink");
    if (!text || !box || !font || !face || !ink || strlen(text) >= PAINT_TEXT_MAX) return 0;
    struct textrec *r = calloc(1, sizeof *r);
    if (!r) return 0;
    struct paint_text *t = &r->t;
    unsigned a, b;
    if (sscanf(box, "%d %d %d %d %d", &t->x, &t->y, &t->wrap, &t->w, &t->h) != 5 ||
        sscanf(face, "%d %d %d %d %d", &t->font.px, &t->font.bold, &t->font.italic, &t->font.underline,
               &t->font.strike) != 5 ||
        sscanf(ink, "%x %x %d", &a, &b, &t->fill) != 3) {
        free(r);
        return 0;
    }
    t->ink = a;
    t->fill_colour = b;
    snprintf(t->font.family, sizeof t->font.family, "%s", font);
    t->len = (int)strlen(text);
    memcpy(t->buf, text, (size_t)t->len + 1);
    r->refs = 1;
    return r;
}

const struct paint_text *layers_text(int i) {
    return i >= 0 && i < g_stack.n && g_stack.l[i].text ? &g_stack.l[i].text->t : 0;
}

int layers_text_at(int x, int y) {
    for (int i = g_stack.n - 1; i >= 0; i--) {
        const struct paint_text *t = layers_text(i);
        if (t && !g_stack.l[i].hidden && x >= t->x && y >= t->y && x < t->x + t->w && y < t->y + t->h) return i;
    }
    return -1;
}

static struct stack g_held;   // the stack a live box began from
static int g_live = -1;       // the live layer, or -1
static int g_live_new;        // it was added for this box

int layers_live_begin(int reopen) {
    if (g_live >= 0) return 1;
    if (reopen < 0 && !room()) return 0;
    struct pix *p = pix_new(pic_w(), pic_h(), 0);
    if (!p) { paint_note("Not enough memory for a text box"); return 0; }
    g_held = g_stack;
    stack_ref(&g_held, +1);
    struct stack *next = &g_next;
    *next = g_stack;
    int at = reopen < 0 ? g_stack.active + 1 : reopen;
    if (reopen < 0) {
        memmove(&next->l[at + 1], &next->l[at], sizeof next->l[0] * (size_t)(next->n - at));
        next->l[at] = (struct layer){ .pix = p, .opacity = 255 };
        snprintf(next->l[at].name, sizeof next->l[at].name, "Text");
        next->n++;
    } else {
        next->l[at].pix = p;   // drawn again from its words, into a clean layer
    }
    next->active = at;
    g_live = at;
    g_live_new = reopen < 0;
    set_current(next);
    return 1;
}

// A text layer's name: its first line, cut short.
static void text_name(const struct paint_text *t, char *out, size_t cap) {
    int n = 0;
    while (n < t->len && t->buf[n] != '\n' && n < 24) n++;
    snprintf(out, cap, "%.*s%s", n, t->buf, n < t->len && t->buf[n] != '\n' ? "..." : "");
}

void layers_live_end(const struct paint_text *t) {
    if (g_live < 0) return;
    int at = g_live, was_new = g_live_new;
    g_live = -1;
    if (!t) {
        if (was_new) {
            set_current(&g_held);
            stack_ref(&g_held, -1);
            return;
        }
        // A REOPENED layer emptied is gone, as a step from where it was.
        struct stack *next = &g_next;
    *next = g_stack;
        memmove(&next->l[at], &next->l[at + 1], sizeof next->l[0] * (size_t)(next->n - at - 1));
        next->n--;
        next->active = at > 0 ? at - 1 : 0;
        set_current(&g_held);
        stack_ref(&g_held, -1);
        change(next, 0, "text deleted");
        return;
    }
    // Unchanged: the stack it began from, and no step.
    const struct textrec *old = !was_new ? g_held.l[at].text : 0;
    if (old && !memcmp(&old->t, t, sizeof *t)) {
        set_current(&g_held);
        stack_ref(&g_held, -1);
        return;
    }
    struct textrec *r = malloc(sizeof *r);
    if (!r) { paint_note("Not enough memory to keep the text"); set_current(&g_held); stack_ref(&g_held, -1); return; }
    r->t = *t;
    r->refs = 0;
    struct stack *next = &g_next;
    *next = g_stack;
    next->l[at].text = r;
    char auto_name[PAINT_LAYER_NAME];
    if (old) text_name(&old->t, auto_name, sizeof auto_name);
    if (was_new || !strcmp(next->l[at].name, auto_name))
        text_name(t, next->l[at].name, sizeof next->l[at].name);
    // The step runs from the stack the box began on to this one.
    g_now = g_stack;
    stack_ref(&g_now, +1);
    set_current(&g_held);
    stack_ref(&g_held, -1);
    change(next, (size_t)pic_w() * pic_h() * 4 + sizeof *r, was_new ? "text" : "text edited");
    stack_ref(&g_now, -1);
}

void layers_rasterize(int i) {
    if (!layers_text(i)) return;
    struct stack *next = &g_next;
    *next = g_stack;
    next->l[i].text = 0;
    change(next, 0, "text to pixels");
}

int layers_reset(struct uimg *bottom) {
    struct pix *p = pix_adopt(bottom);
    if (!p) return 0;
    g_old = g_stack;
    upixundo_clear(&g_undo);   // a new picture's history: drops every step, and their counts
    g_stack = (struct stack){ .n = 1 };
    g_stack.l[0] = (struct layer){ .pix = p, .name = "Background", .opacity = 255 };
    p->refs = 1;
    stack_ref(&g_old, -1);
    g_serial = 1;
    restack();
    return 1;
}

int layers_reset_many(struct uora_layer *ls, int n) {
    if (n < 1 || n > PAINT_LAYERS_MAX) return 0;
    struct stack *next = &g_next;
    *next = (struct stack){ .n = n, .active = n - 1 };
    for (int i = 0; i < n; i++) {
        struct pix *p = pix_adopt(&ls[i].img);
        if (!p) {
            for (int j = 0; j < i; j++) { next->l[j].pix->refs = 1; pix_put(next->l[j].pix); text_put(next->l[j].text); }
            return 0;
        }
        next->l[i] = (struct layer){ .pix = p, .opacity = ls[i].opacity, .hidden = ls[i].hidden };
        snprintf(next->l[i].name, sizeof next->l[i].name, "%s", ls[i].name);
        next->l[i].text = text_from_attrs(&ls[i]);
        p->refs = 1;
    }
    g_old = g_stack;
    upixundo_clear(&g_undo);
    g_stack = g_next;
    stack_ref(&g_old, -1);
    g_serial = (unsigned)n;
    restack();
    return 1;
}

int layers_export(struct uora_layer *out) {
    for (int i = 0; i < g_stack.n; i++) {
        out[i] = (struct uora_layer){ .img = g_stack.l[i].pix->img, .opacity = g_stack.l[i].opacity,
                                      .hidden = g_stack.l[i].hidden };
        snprintf(out[i].name, sizeof out[i].name, "%s", g_stack.l[i].name);
        if (g_stack.l[i].text) text_to_attrs(&g_stack.l[i].text->t, &out[i]);
    }
    return g_stack.n;
}

void layers_export_free(struct uora_layer *out, int n) {
    for (int i = 0; i < n; i++)
        for (int k = 0; k < out[i].nattrs; k++) free(out[i].attrs[k].value);
}

void layers_select(int i) {
    if (i < 0 || i >= g_stack.n || i == g_stack.active) return;
    // NOT A STEP: which layer is current is where the tools point, as a
    // tool is -- Paint.NET's history does not record it either.
    g_stack.active = i;
    restack();
    ulogf("paint: layer %d current\n", i);
}

void layers_add(void) {
    if (!room()) return;
    struct pix *p = pix_new(pic_w(), pic_h(), 0);
    if (!p) { paint_note("Not enough memory for another layer"); return; }
    struct stack *next = &g_next;
    *next = g_stack;
    int at = g_stack.active + 1;
    memmove(&next->l[at + 1], &next->l[at], sizeof next->l[0] * (size_t)(next->n - at));
    next->l[at] = (struct layer){ .pix = p, .opacity = 255 };
    default_name(next->l[at].name, sizeof next->l[at].name);
    next->n++;
    next->active = at;
    change(next, (size_t)pic_w() * pic_h() * 4, "new");
}

void layers_duplicate(void) {
    if (!room()) return;
    const struct layer *src = &g_stack.l[g_stack.active];
    struct pix *p = pix_copy(&src->pix->img);
    if (!p) { paint_note("Not enough memory to duplicate the layer"); return; }
    struct stack *next = &g_next;
    *next = g_stack;
    int at = g_stack.active + 1;
    memmove(&next->l[at + 1], &next->l[at], sizeof next->l[0] * (size_t)(next->n - at));
    next->l[at] = *src;
    next->l[at].pix = p;
    snprintf(next->l[at].name, sizeof next->l[at].name, "%.*s copy", PAINT_LAYER_NAME - 6, src->name);
    next->n++;
    next->active = at;
    change(next, (size_t)pic_w() * pic_h() * 4, "duplicate");
}

void layers_delete(void) {
    if (g_stack.n <= 1) { paint_note("A picture keeps at least one layer"); return; }
    struct stack *next = &g_next;
    *next = g_stack;
    int at = g_stack.active;
    memmove(&next->l[at], &next->l[at + 1], sizeof next->l[0] * (size_t)(next->n - at - 1));
    next->n--;
    next->active = at > 0 ? at - 1 : 0;
    change(next, 0, "delete");
}

void layers_move(int from, int to) {
    if (from < 0 || from >= g_stack.n || to < 0 || to >= g_stack.n || from == to) return;
    struct stack *next = &g_next;
    *next = g_stack;
    struct layer moved = next->l[from];
    if (from < to) memmove(&next->l[from], &next->l[from + 1], sizeof next->l[0] * (size_t)(to - from));
    else memmove(&next->l[to + 1], &next->l[to], sizeof next->l[0] * (size_t)(from - to));
    next->l[to] = moved;
    next->active = to;
    change(next, 0, "move");
}

void layers_merge_down(void) {
    int at = g_stack.active;
    if (at == 0) { paint_note("The bottom layer has nothing below it to merge into"); return; }
    const struct layer *up = &g_stack.l[at], *down = &g_stack.l[at - 1];
    // The upper at its opacity INTO the lower's pixels, the lower keeping
    // its own -- Paint.NET's merge.
    struct pix *p = pix_copy(&down->pix->img);
    if (!p) { paint_note("Not enough memory to merge"); return; }
    struct upix_rect all = { 0, 0, pic_w(), pic_h() };
    if (!up->hidden) upix_over(&p->img, &up->pix->img, &all, up->opacity);
    struct stack *next = &g_next;
    *next = g_stack;
    next->l[at - 1].pix = p;
    next->l[at - 1].text = 0;   // its pixels are no longer only its words
    memmove(&next->l[at], &next->l[at + 1], sizeof next->l[0] * (size_t)(next->n - at - 1));
    next->n--;
    next->active = at - 1;
    change(next, (size_t)pic_w() * pic_h() * 4, "merge down");
}

void layers_flatten(void) {
    if (g_stack.n == 1 && g_stack.l[0].opacity == 255 && !g_stack.l[0].hidden) return;
    struct pix *p = pix_copy(layers_view());
    if (!p) { paint_note("Not enough memory to flatten"); return; }
    struct stack *next = &g_next;
    *next = (struct stack){ .n = 1 };
    next->l[0] = (struct layer){ .pix = p, .opacity = 255 };
    snprintf(next->l[0].name, sizeof next->l[0].name, "%s", g_stack.l[0].name);
    change(next, (size_t)pic_w() * pic_h() * 4, "flatten");
}

void layers_set_hidden(int i, int hidden) {
    if (i < 0 || i >= g_stack.n || g_stack.l[i].hidden == !!hidden) return;
    struct stack *next = &g_next;
    *next = g_stack;
    next->l[i].hidden = !!hidden;
    change(next, 0, hidden ? "hide" : "show");
}

void layers_rename(int i, const char *name) {
    if (i < 0 || i >= g_stack.n || !name[0] || !strcmp(name, g_stack.l[i].name)) return;
    struct stack *next = &g_next;
    *next = g_stack;
    snprintf(next->l[i].name, sizeof next->l[i].name, "%s", name);
    change(next, 0, "rename");
}

// A SLIDER DRAG IS ONE STEP: it shows each value as it goes and records
// only the one it ends on, from the one it began at.
void layers_opacity_preview(int v) {
    struct layer *l = &g_stack.l[g_stack.active];
    if (g_op_before < 0) g_op_before = l->opacity;
    l->opacity = v < 0 ? 0 : v > 255 ? 255 : v;
    restack();
}

void layers_opacity_commit(void) {
    if (g_op_before < 0) return;
    struct layer *l = &g_stack.l[g_stack.active];
    int to = l->opacity;
    l->opacity = g_op_before;
    g_op_before = -1;
    if (to == l->opacity) return;
    struct stack *next = &g_next;
    *next = g_stack;
    next->l[g_stack.active].opacity = to;
    change(next, 0, "opacity");
}

int layers_transform(layers_fn fn, void *ctx, const char *what) {
    struct stack *next = &g_next;
    *next = g_stack;
    size_t bytes = 0;
    for (int i = 0; i < g_stack.n; i++) {
        struct uimg out = { 0 };
        struct pix *p = 0;
        if (fn(&g_stack.l[i].pix->img, &out, i, ctx) != 0 || !(p = pix_adopt(&out))) {
            uimg_free(&out);
            for (int j = 0; j < i; j++) { next->l[j].pix->refs = 1; pix_put(next->l[j].pix); }
            return 0;
        }
        p->img.has_alpha = 1;
        next->l[i].pix = p;
        next->l[i].text = 0;   // moved or scaled, its pixels are no longer its words: pixels (GIMP's)
        bytes += (size_t)p->img.w * p->img.h * 4;
    }
    change(next, bytes, what);
    return 1;
}
