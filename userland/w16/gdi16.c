// gdi16.c -- GDI's exports: the object table (DCs, bitmaps, pens,
// brushes), and the calls that draw through them. The pixel work is
// blit16.c's.
//
// HANDLES: a created object is GDI_BASE + 4 * its slot; a stock object is
// STOCK_BASE + its GetStockObject index. EACH WINDOW HAS ONE DC (made
// when first asked for), which BeginPaint and GetDC hand out with its
// state reset; a memory DC (CreateCompatibleDC) draws into the bitmap
// selected into it.
//
// A display bitmap is 32-bit host pixels (docs/w16-design.md's choice):
// a program treats a device-dependent bitmap as opaque, so its depth is
// GDI's business. A 1-bit bitmap keeps that it is one (`mono`), because
// the colour conversions of a mask blit depend on it.
//
// A window DC draws straight into its top-level window's client buffer
// under `lock`, at the window's place and clipped to what of it shows;
// fb_dirty then asks the main thread to repaint once the program is
// done -- when it next waits for a message.
#include "w16_internal.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ui/ugfx.h"

#define STOCK_BASE 0x5000      // GetStockObject(i) -> STOCK_BASE + i
#define GDI_BASE   0x6000
#define TRANSPARENT 1
#define OPAQUE      2
#define R2_COPYPEN  13
#define PS_NULL     5
#define BS_NULL     1

#define WHITE_BRUSH  0
#define NULL_BRUSH   5
#define WHITE_PEN    6
#define BLACK_PEN    7
#define NULL_PEN     8
#define SYSTEM_FONT  13

// The fonts -- stock, a dialog's, CreateFont's -- are font16.c's.

// --- the object table --------------------------------------------------------

static struct w16_gdiobj *obj(struct w16 *w, uint16_t h, int type) {
    if (h < GDI_BASE || (h - GDI_BASE) % 4) return 0;
    unsigned i = (h - GDI_BASE) / 4u;
    if (i >= W16_GDI_MAX || w->gdi[i].type != type) return 0;
    return &w->gdi[i];
}

static uint16_t new_obj(struct w16 *w, int type, struct w16_gdiobj **out) {
    for (unsigned i = 0; i < W16_GDI_MAX; i++)
        if (w->gdi[i].type == GDI_FREE) {
            memset(&w->gdi[i], 0, sizeof w->gdi[i]);
            w->gdi[i].type = (uint8_t)type;
            *out = &w->gdi[i];
            return (uint16_t)(GDI_BASE + 4 * i);
        }
    *out = 0;
    return 0;
}

static int type_of(struct w16 *w, uint16_t h) {
    if (h >= STOCK_BASE && h < STOCK_BASE + 17) {
        unsigned i = h - STOCK_BASE;
        return i <= NULL_BRUSH ? GDI_BRUSH : i <= NULL_PEN ? GDI_PEN : GDI_FONT;
    }
    if (h < GDI_BASE || (h - GDI_BASE) % 4 || (h - GDI_BASE) / 4u >= W16_GDI_MAX) return GDI_FREE;
    return w->gdi[(h - GDI_BASE) / 4u].type;
}

#define MM_TEXT        1
#define MM_TWIPS       6
#define MM_ISOTROPIC   7
#define MM_ANISOTROPIC 8

static void dc_defaults(struct w16_gdiobj *dc) {
    free(dc->saved);
    dc->saved = 0;
    dc->nsaved = 0;
    dc->map_mode = MM_TEXT;
    dc->stretch_mode = 1;                        // BLACKONWHITE
    dc->fill_mode = 1;                           // ALTERNATE
    dc->worg[0] = dc->worg[1] = dc->vorg[0] = dc->vorg[1] = 0;
    dc->wext[0] = dc->wext[1] = dc->vext[0] = dc->vext[1] = 1;
    dc->has_clip = 0;
    dc->break_extra = dc->break_count = 0;
    dc->brush_ox = dc->brush_oy = 0;
    dc->pen = STOCK_BASE + BLACK_PEN;
    dc->brush = STOCK_BASE + WHITE_BRUSH;
    dc->font = STOCK_BASE + SYSTEM_FONT;
    dc->text = 0x000000;
    dc->bk = 0xFFFFFF;
    dc->bk_mode = OPAQUE;
    dc->rop2 = R2_COPYPEN;
    dc->text_align = 0;
    dc->cx = dc->cy = 0;
}

// --- mapping: logical units to the DC's pixels ------------------------------------

// v * num / den, rounded to the nearest as GDI's MulDiv did.
static int scale(long v, long num, long den) {
    if (!den) return (int)v;
    long p = v * num, ap = p < 0 ? -p : p, ad = den < 0 ? -den : den;
    long q = (ap + ad / 2) / ad;
    return (int)((p < 0) != (den < 0) ? -q : q);
}

static int dev_x(const struct w16_gdiobj *dc, int x) {
    return scale(x - dc->worg[0], dc->vext[0], dc->wext[0]) + dc->vorg[0];
}
static int dev_y(const struct w16_gdiobj *dc, int y) {
    return scale(y - dc->worg[1], dc->vext[1], dc->wext[1]) + dc->vorg[1];
}
static int log_x(const struct w16_gdiobj *dc, int x) {
    return scale(x - dc->vorg[0], dc->wext[0], dc->vext[0]) + dc->worg[0];
}
static int log_y(const struct w16_gdiobj *dc, int y) {
    return scale(y - dc->vorg[1], dc->wext[1], dc->vext[1]) + dc->worg[1];
}

// A logical rectangle as device pixels, left above right and top above
// bottom whichever way the mapping turns the axes.
static void dev_rect(const struct w16_gdiobj *dc, int *l, int *t, int *r, int *b) {
    int x0 = dev_x(dc, *l), y0 = dev_y(dc, *t), x1 = dev_x(dc, *r), y1 = dev_y(dc, *b);
    *l = x0 < x1 ? x0 : x1;
    *r = x0 < x1 ? x1 : x0;
    *t = y0 < y1 ? y0 : y1;
    *b = y0 < y1 ? y1 : y0;
}

// A logical length along x, in pixels: a pen's width, a space's extra.
static int dev_w(const struct w16_gdiobj *dc, int v) { return scale(v, dc->vext[0], dc->wext[0]); }
static int dev_h(const struct w16_gdiobj *dc, int v) { return scale(v, dc->vext[1], dc->wext[1]); }

#define CS_OWNDC   0x0020
#define CS_CLASSDC 0x0040

uint16_t w16_gdi_window_dc(struct w16 *w, uint16_t hwnd) {
    struct w16_wnd *x = w16_wnd_get(w, hwnd);
    if (!x) return 0;
    struct w16_gdiobj *dc = obj(w, x->dc, GDI_DC);
    // A CS_OWNDC (or CS_CLASSDC) window's DC keeps its objects, colours and
    // mapping from one BeginPaint or GetDC to the next -- the reason a program
    // asks for one: it selects its font once.
    int own = x->cls >= 0 && x->cls < w->ncls && (w->cls[x->cls].style & (CS_OWNDC | CS_CLASSDC));
    if (!dc || dc->hwnd != hwnd) {
        x->dc = new_obj(w, GDI_DC, &dc);
        if (!dc) return 0;
        dc->hwnd = hwnd;
        own = 0;
    }
    if (!own) dc_defaults(dc);
    return x->dc;
}

uint16_t w16_gdi_screen_dc(struct w16 *w) {
    static uint16_t screen;
    if (w->win.used) return w16_gdi_window_dc(w, w->win.hwnd);
    struct w16_gdiobj *dc = obj(w, screen, GDI_DC);
    if (!dc || !dc->screen) {
        screen = new_obj(w, GDI_DC, &dc);
        if (!dc) return 0;
        dc->screen = 1;
    }
    dc_defaults(dc);
    return screen;
}

void w16_gdi_forget_window(struct w16 *w, uint16_t hwnd) {
    struct w16_wnd *x = w16_wnd_get(w, hwnd);
    struct w16_gdiobj *dc = x ? obj(w, x->dc, GDI_DC) : 0;
    if (dc && dc->hwnd == hwnd) { free(dc->saved); memset(dc, 0, sizeof *dc); }
    if (x) x->dc = 0;
}

static void commit(struct w16_wnd *x) {
    if (!x || !x->used || !x->fb) return;
    if (x->shw != x->fbw || x->shh != x->fbh) {
        uint32_t *g = realloc(x->shown, (size_t)x->fbw * x->fbh * 4);
        if (!g) return;                          // the old frame stays shown
        x->shown = g;
        x->shw = x->fbw;
        x->shh = x->fbh;
    }
    memcpy(x->shown, x->fb, (size_t)x->fbw * x->fbh * 4);
}

void w16_gdi_flush(struct w16 *w) {
    if (!w->fb_dirty) return;
    w->fb_dirty = 0;
    pthread_mutex_lock(&w->lock);
    commit(&w->win);
    for (int i = 0; i < w->nwnds; i++)
        if (w->wnds[i] && w->wnds[i]->kind == W16_K_DIALOG) commit(w->wnds[i]);
    pthread_mutex_unlock(&w->lock);
    w->committed_ms = w16_now_ms();
    w16_task_post(w, W16_POST_PAINT);
}

const uint32_t *w16_shown(const struct w16_wnd *x, int *w, int *h) {
    if (x->shown) { *w = x->shw; *h = x->shh; return x->shown; }
    *w = x->fbw;
    *h = x->fbh;
    return x->fb;
}

// --- colours -----------------------------------------------------------------

static const uint32_t g_stock_brush[] = { 0xFFFFFF, 0xC0C0C0, 0x808080, 0x404040, 0x000000 };

// -1 for "draw nothing" (a NULL brush or pen).
// A stock or system-colour brush is a palette colour too: shown as 3.11 at
// VGA showed it (w16_vga), as a created brush's is.
static int64_t brush_of(struct w16 *w, uint16_t h) {
    if (h >= STOCK_BASE && h < STOCK_BASE + NULL_BRUSH) return w16_vga(g_stock_brush[h - STOCK_BASE]);
    if (h == STOCK_BASE + NULL_BRUSH || h == 0) return -1;
    if (h >= 1 && h <= 21) return w16_vga(w16_syscolor(h - 1u));   // COLOR_* + 1
    struct w16_gdiobj *b = obj(w, h, GDI_BRUSH);
    if (!b || b->style == BS_NULL) return b ? -1 : 0xFFFFFF;
    return w16_colorref_px(b->color);
}

#define BS_HATCHED 2
#define BS_PATTERN 3

// The 8x8 bits of HS_HORIZONTAL .. HS_DIAGCROSS, a set bit the hatch's
// colour -- Windows' own patterns, as Wine's gdi32 lists them.
static const uint8_t HATCHES[6][8] = {
    { 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00 },
    { 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08 },
    { 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01 },
    { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 },
    { 0x08, 0x08, 0x08, 0xFF, 0x08, 0x08, 0x08, 0x08 },
    { 0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81 },
};

// A hatched or bitmap brush as the blit's 8x8 pattern, in the colours of
// the DC drawing with it (a hatch's gaps and a 1-bit pattern's 1s are its
// background colour, a 1-bit pattern's 0s its text colour); a solid
// brush leaves `b` its one pixel. The pattern's origin is the DC's plus
// SetBrushOrg's.
static void pattern_of(struct w16 *w, const struct w16_gdiobj *dc, uint16_t brush, const struct gdi_target *t,
                       struct gdi_blit *b, uint32_t buf[64]) {
    struct w16_gdiobj *o = obj(w, brush, GDI_BRUSH);
    if (!o || (o->style != BS_HATCHED && o->style != BS_PATTERN)) return;
    uint32_t bk = w16_colorref_px(dc->bk), text = w16_colorref_px(dc->text);
    for (int i = 0; i < 64; i++) {
        if (o->style == BS_HATCHED)
            buf[i] = HATCHES[o->width % 6][i / 8] >> (7 - i % 8) & 1 ? w16_colorref_px(o->color) : bk;
        else if (o->mono)
            buf[i] = o->px && o->px[i] ? bk : text;
        else
            buf[i] = o->px ? o->px[i] : bk;
    }
    b->pat = buf;
    b->pat_x = t->ox + dc->brush_ox;
    b->pat_y = t->oy + dc->brush_oy;
}

uint32_t w16_brush_px(struct w16 *w, uint16_t brush) {
    int64_t v = brush_of(w, brush);
    return v < 0 ? 0xFFFFFF : (uint32_t)v;
}

uint32_t w16_brush_color(uint16_t brush) {               // kept for callers with no w at hand
    return w16_brush_px(&g_w16, brush);
}

static void rgn_set(struct w16_gdiobj *r, int l, int t, int rr, int b);

static int64_t pen_of(struct w16 *w, uint16_t h, int *width) {
    *width = 1;
    if (h == STOCK_BASE + WHITE_PEN) return 0xFFFFFF;
    if (h == STOCK_BASE + BLACK_PEN) return 0x000000;
    if (h == STOCK_BASE + NULL_PEN) return -1;
    struct w16_gdiobj *p = obj(w, h, GDI_PEN);
    if (!p) return 0;
    if (p->style == PS_NULL) return -1;
    *width = p->width > 1 ? p->width : 1;
    return w16_colorref_px(p->color);
}

// --- targets -------------------------------------------------------------------

static uint32_t g_scratch_px;    // a memory DC with no bitmap: Windows' 1x1 default
#define DEFAULT_BITMAP 0x5FFF    // what SelectObject hands back for that default

// The DC's pixels. A window DC's are its top-level window's client
// buffer, which the main thread replaces on a resize: the caller holds
// `lock`. A window that shows nothing leaves an empty clip.
static void fill_target(struct w16 *w, const struct w16_gdiobj *dc, struct gdi_target *t) {
    memset(t, 0, sizeof *t);
    if (dc->hwnd) {
        struct w16_wnd *x = w16_wnd_get(w, dc->hwnd), *top = x ? w16_wnd_top(w, x) : 0;
        if (!top || !top->fb) return;
        t->px = top->fb;
        t->w = top->fbw;
        t->h = top->fbh;
        w16_wnd_origin(w, x, &t->ox, &t->oy);
        if (!w16_wnd_visible_rect(w, x, &t->x0, &t->y0, &t->x1, &t->y1)) t->x1 = t->x0;
    } else {
        if (dc->screen) return;
        struct w16_gdiobj *bm = obj(w, dc->bitmap, GDI_BITMAP);
        if (bm) { t->px = bm->px; t->w = bm->bw; t->h = bm->bh; t->mono = bm->mono; }
        else { t->px = &g_scratch_px; t->w = t->h = 1; t->mono = 1; }
        gdi_target_whole(t);
    }
    if (dc->has_clip) {                          // the DC's own clip, inside what it may draw on
        if (dc->clip[0] + t->ox > t->x0) t->x0 = dc->clip[0] + t->ox;
        if (dc->clip[1] + t->oy > t->y0) t->y0 = dc->clip[1] + t->oy;
        if (dc->clip[2] + t->ox < t->x1) t->x1 = dc->clip[2] + t->ox;
        if (dc->clip[3] + t->oy < t->y1) t->y1 = dc->clip[3] + t->oy;
        if (t->x1 < t->x0) t->x1 = t->x0;
        if (t->y1 < t->y0) t->y1 = t->y0;
    }
}

// WS_CLIPCHILDREN: a window DC draws round its shown children, not over
// them. The clip is one rectangle, so the children's pixels are kept
// before a call draws and put back after -- the same pixels as a clip that
// left them out. Task thread, under `lock`.
static struct kept { int x, y, w, h; uint32_t *px; } *g_kept;
static int g_nkept, g_kept_cap;
static const struct gdi_target *g_kept_t;

static void restore_children(void) {
    for (int i = 0; i < g_nkept; i++) {
        struct kept *k = &g_kept[i];
        for (int r = 0; r < k->h; r++)
            memcpy(g_kept_t->px + (size_t)(k->y + r) * g_kept_t->w + k->x, k->px + (size_t)r * k->w, (size_t)k->w * 4);
        free(k->px);
    }
    g_nkept = 0;
}

static void keep_children(struct w16 *w, const struct w16_gdiobj *dc, const struct gdi_target *t) {
    restore_children();
    struct w16_wnd *x = dc->hwnd ? w16_wnd_get(w, dc->hwnd) : 0;
    if (!x || !(x->style & WS_CLIPCHILDREN) || !t->px) return;
    int n;
    struct w16_wnd **v = w16_wnd_children(w, x->hwnd, &n);
    g_kept_t = t;
    for (int i = 0; v && i < n; i++) {
        if (!w16_wnd_shown(w, v[i])) continue;
        int ox, oy, l, tp, r, b;
        w16_wnd_origin(w, v[i], &ox, &oy);
        w16_wnd_insets(v[i], &l, &tp, &r, &b);
        int x0 = ox - l, y0 = oy - tp, x1 = x0 + v[i]->w, y1 = y0 + v[i]->h;
        if (x0 < t->x0) x0 = t->x0;
        if (y0 < t->y0) y0 = t->y0;
        if (x1 > t->x1) x1 = t->x1;
        if (y1 > t->y1) y1 = t->y1;
        if (x1 <= x0 || y1 <= y0) continue;
        if (g_nkept == g_kept_cap) {
            int cap = g_kept_cap ? 2 * g_kept_cap : 8;
            struct kept *g = realloc(g_kept, (size_t)cap * sizeof *g);
            if (!g) break;
            g_kept = g;
            g_kept_cap = cap;
        }
        struct kept *k = &g_kept[g_nkept];
        *k = (struct kept){ x0, y0, x1 - x0, y1 - y0, malloc((size_t)(x1 - x0) * (y1 - y0) * 4) };
        if (!k->px) break;
        for (int row = 0; row < k->h; row++)
            memcpy(k->px + (size_t)row * k->w, t->px + (size_t)(y0 + row) * t->w + x0, (size_t)k->w * 4);
        g_nkept++;
    }
    free(v);
}

// The DC and its pixels; a window DC holds `lock` until gdi_done().
static struct w16_gdiobj *target(struct w16 *w, uint16_t hdc, struct gdi_target *t) {
    struct w16_gdiobj *dc = obj(w, hdc, GDI_DC);
    memset(t, 0, sizeof *t);
    if (!dc) return 0;
    if (dc->hwnd) pthread_mutex_lock(&w->lock);
    fill_target(w, dc, t);
    if (dc->hwnd) keep_children(w, dc, t);
    return dc;
}

// An inverting mix drawn on a window DC outside WM_PAINT is feedback meant
// to be SEEN (Calculator's pressed button is two R2_NOTXORPEN RoundRects
// with nothing between): 3.11 drew to the screen at once, and VGA's slow
// draw kept the first frame up. Here a frame commits when the program next
// waits, so both would land in one. The FIRST of a burst is therefore
// shown and held for a few frames; the rest of a burst (a rubber band
// being dragged) is not, so a drag takes one stall, not one per move.
#define FLASH_HOLD_MS  40
#define FLASH_BURST_MS 150

static int inverting_rop2(int rop2) { return rop2 == 6 || rop2 == 7 || rop2 == 10; }

static void hold_flash(struct w16 *w) {
    uint64_t now = w16_now_ms();
    int first = now - w->flash_ms >= FLASH_BURST_MS;
    w->flash_ms = now;
    pthread_mutex_lock(&w->lock);
    int owed = w16_wnd_next_paint(w) != 0;       // a window half painted would show its children missing
    pthread_mutex_unlock(&w->lock);
    if (!first || owed || w->painting) return;
    w16_gdi_flush(w);
    struct timespec ts = { 0, FLASH_HOLD_MS * 1000000L };
    nanosleep(&ts, 0);
}

static void gdi_done(struct w16 *w, struct w16_gdiobj *dc) {
    if (dc && dc->hwnd) {
        int flash = inverting_rop2(dc->rop2);
        restore_children();
        w->fb_dirty = 1;
        pthread_mutex_unlock(&w->lock);
        if (flash) hold_flash(w);
    }
}

// A window DC's lock dropped after a call that only READ (GetPixel, PtVisible).
static void gdi_read_done(struct w16 *w, struct w16_gdiobj *dc) {
    if (dc && dc->hwnd) {
        restore_children();
        pthread_mutex_unlock(&w->lock);
    }
}

static struct w16_gdiobj *new_bitmap(struct w16 *w, int bw, int bh, int mono, uint16_t *h) {
    struct w16_gdiobj *bm;
    if (bw <= 0 || bh <= 0 || bw > 4096 || bh > 4096) { *h = 0; return 0; }
    *h = new_obj(w, GDI_BITMAP, &bm);
    if (!bm) return 0;
    bm->px = calloc((size_t)bw * bh, 4);
    if (!bm->px) { bm->type = GDI_FREE; *h = 0; return 0; }
    bm->bw = bw;
    bm->bh = bh;
    bm->mono = mono;
    return bm;
}

// --- text ------------------------------------------------------------------------

// `len` bytes at a far pointer as a string, allocated; NULL after a fault.
static char *far_text(struct w16 *w, uint32_t far, unsigned len) {
    char *text = malloc(len + 1);
    if (!text) { cpu16_set_fault(&w->cpu, FAULT_API, "out of memory for a string"); return 0; }
    if (len) {
        const uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, len, 0);
        if (!p) { free(text); return 0; }
        memcpy(text, p, len);
    }
    text[len] = '\0';
    return text;
}

// `text` measured -- and drawn at (x, y) of `s` unless x is INT_MIN -- in
// the current ugfx font: a tab steps to the next of `tab` pixels (tab > 0),
// and SetTextJustification's extra pixels go to the spaces, the remainder
// to the first ones as GDI spread them. Its width.
static int text_runs(const struct w16_gdiobj *dc, struct ugfx_surface *s, int x, int y, const char *text, int tab,
                     uint32_t ink) {
    int width = 0, breaks = dc ? dc->break_count : 0, extra = dc ? dc->break_extra : 0, nth = 0;
    char cut[2] = { '\t', breaks > 0 ? ' ' : '\t' };
    for (const char *p = text; *p; ) {
        size_t run = 0;
        while (p[run] && !((tab > 0 && p[run] == cut[0]) || (breaks > 0 && p[run] == cut[1]))) run++;
        if (run) {
            char *piece = strndup(p, run);
            if (piece && x != INT_MIN) ugfx_draw_string(s, x + width, y, piece, ink, UGFX_TRANSPARENT);
            width += piece ? ugfx_text_width(piece) : 0;
            free(piece);
            p += run;
        }
        if (*p == '\t' && tab > 0) { width = (width / tab + 1) * tab; p++; }
        else if (*p == ' ' && breaks > 0) {
            width += ugfx_char_advance(' ') + extra / breaks + (nth < extra % breaks ? 1 : 0);
            nth++;
            p++;
        }
    }
    return width;
}

// `text` at (x, y) of a DC in the system font, its colours and
// background mode; a tab steps to the next of `tab` pixels when tab > 0
// (TabbedTextOut). Its width.
static int draw_text_clip(struct w16 *w, uint16_t hdc, int x, int y, const char *text, int tab,
                          const int *clip) {
    struct gdi_target t;
    struct w16_gdiobj *dc = target(w, hdc, &t);
    int width = 0, ta_dx = 0, ta_dy = 0;
    if (dc && dc->text_align) {                  // SetTextAlign: the point is the right, middle, bottom or baseline
        int cx, cy;
        w16_text_size_in(dc->font, text, &cx, &cy);
        unsigned ta = dc->text_align;
        ta_dx = (ta & 6) == 6 ? -cx / 2 : (ta & 2) ? -cx : 0;
        if ((ta & 24) == 24) {
            pthread_mutex_lock(&g_w16_text_lock);
            ta_dy = -w16_font_info(dc->font)->ascent;
            pthread_mutex_unlock(&g_w16_text_lock);
        } else if (ta & 8) {
            ta_dy = -cy;
        }
    }
    pthread_mutex_lock(&g_w16_text_lock);
    const struct ugfx_font *was = ugfx_set_font(w16_font_of(dc ? dc->font : 0));
    if (dc) { x = dev_x(dc, x) + ta_dx; y = dev_y(dc, y) + ta_dy; }
    if (dc && t.px && clip) {                    // a DC-coordinate rectangle, within the window's
        int l = clip[0], tp = clip[1], r = clip[2], b = clip[3];
        dev_rect(dc, &l, &tp, &r, &b);
        if (l + t.ox > t.x0) t.x0 = l + t.ox;
        if (tp + t.oy > t.y0) t.y0 = tp + t.oy;
        if (r + t.ox < t.x1) t.x1 = r + t.ox;
        if (b + t.oy < t.y1) t.y1 = b + t.oy;
        if (t.x1 < t.x0) t.x1 = t.x0;
        if (t.y1 < t.y0) t.y1 = t.y0;
    }
    if (dc && t.px && t.x1 > t.x0 && t.y1 > t.y0) {
        struct ugfx_surface s = ugfx_surface_for_pixels(t.px, t.w, t.h);
        ugfx_set_clip_rect(&s, t.x0, t.y0, t.x1 - t.x0, t.y1 - t.y0);
        // OPAQUE fills the whole cell box first, as Windows did: the glyphs
        // are blended over what is there, so text redrawn in place (a
        // clock's digits) would otherwise pile up.
        int asc = 0, under = 0, strike = 0;
        const struct w16_logfont *lf = w16_gdi_logfont(w, dc->font);
        if (lf && (lf->underline || lf->strikeout)) {
            asc = w16_font_info(dc->font)->ascent;
            under = lf->underline;
            strike = lf->strikeout;
        }
        for (int pass = dc->bk_mode == TRANSPARENT; pass < 2; pass++) {
            width = text_runs(dc, &s, pass ? x + t.ox : INT_MIN, y + t.oy + w16_font_info(dc->font)->dy, text, tab,
                              w16_colorref_px(dc->text));
            if (pass) continue;
            ugfx_fill_rect(&s, x + t.ox, y + t.oy, width, ugfx_char_h(), w16_colorref_px(dc->bk));
            // ...and the ink kept inside that box: an anti-aliased stand-in
            // glyph spills a pixel past its advance, which the next fill
            // would leave behind (3.11's raster fonts never spill).
            int l = x + t.ox > t.x0 ? x + t.ox : t.x0, r = x + t.ox + width < t.x1 ? x + t.ox + width : t.x1;
            ugfx_set_clip_rect(&s, l, t.y0, r - l, t.y1 - t.y0);
        }
        // An underlined or struck-out font's line, drawn by GDI as Windows drew it for a raster font.
        if (under) ugfx_fill_rect(&s, x + t.ox, y + t.oy + asc + 1, width, 1, w16_colorref_px(dc->text));
        if (strike) ugfx_fill_rect(&s, x + t.ox, y + t.oy + asc * 2 / 3, width, 1, w16_colorref_px(dc->text));
    }
    ugfx_set_font(was);
    pthread_mutex_unlock(&g_w16_text_lock);
    gdi_done(w, dc);
    return dc ? width : -1;
}

static int draw_text(struct w16 *w, uint16_t hdc, int x, int y, const char *text, int tab) {
    return draw_text_clip(w, hdc, x, y, text, tab, 0);
}

int w16_gdi_text(struct w16 *w, uint16_t hdc, int x, int y, const char *text, int tab) {
    return draw_text(w, hdc, x, y, text, tab);
}

// --- DrawText ----------------------------------------------------------------------

static uint16_t dc_font(struct w16 *w, uint16_t hdc);

#define DT_CENTER     0x0001
#define DT_RIGHT      0x0002
#define DT_VCENTER    0x0004
#define DT_BOTTOM     0x0008
#define DT_WORDBREAK  0x0010
#define DT_SINGLELINE 0x0020
#define DT_EXPANDTABS 0x0040
#define DT_NOCLIP     0x0100
#define DT_CALCRECT   0x0400
#define DT_NOPREFIX   0x0800

// One line of DrawText's: its text with the '&'s taken out, and where the
// underlined character is (-1 none).
struct dt_line { char *text; int mark; };

static int dt_width(uint16_t hfont, const char *s, size_t n, int tab) {
    char *t = strndup(s, n);
    if (!t) return 0;
    int width = 0;
    for (char *p = t; *p; ) {
        size_t run = tab > 0 ? strcspn(p, "\t") : strlen(p);
        char c = p[run];
        p[run] = 0;
        int cx, cy;
        w16_text_size_in(hfont, p, &cx, &cy);
        width += cx;
        p[run] = c;
        p += run;
        if (*p == '\t') { width = (width / tab + 1) * tab; p++; }
    }
    free(t);
    return width;
}

static char *dt_strip(const char *s, size_t n, int prefix, int *mark) {
    char *out = malloc(n + 1);
    size_t k = 0;
    *mark = -1;
    if (!out) return 0;
    for (size_t i = 0; i < n; i++) {
        if (prefix && s[i] == '&' && i + 1 < n) {
            i++;
            if (s[i] != '&' && *mark < 0) *mark = (int)k;
        }
        out[k++] = s[i];
    }
    out[k] = 0;
    return out;
}

int w16_gdi_draw_text(struct w16 *w, uint16_t hdc, const char *text, int16_t rc[4], unsigned fmt) {
    uint16_t hfont = dc_font(w, hdc);
    int lh = w16_font_line_h(hfont), width = rc[2] - rc[0], tab = 0;
    if (fmt & DT_EXPANDTABS) { int cx, cy; w16_text_size_in(hfont, " ", &cx, &cy); tab = cx * 8; }
    int prefix = !(fmt & DT_NOPREFIX);
    struct dt_line *line = 0;
    int n = 0, cap = 0, widest = 0;
    // Lines: the whole text when DT_SINGLELINE; else each '\n' (a '\r' before
    // it dropped) and, with DT_WORDBREAK, a break at the last space that fits.
    for (const char *p = text; ; ) {
        size_t len = fmt & DT_SINGLELINE ? strlen(p) : strcspn(p, "\n");
        size_t take = len;
        if ((fmt & DT_WORDBREAK) && !(fmt & DT_SINGLELINE) && dt_width(hfont, p, len, tab) > width) {
            size_t fit = 0;
            for (size_t i = 1; i <= len; i++)
                if (dt_width(hfont, p, i, tab) > width) break;
                else fit = i;
            take = fit;
            for (size_t i = fit; i > 0; i--)
                if (p[i] == ' ') { take = i; break; }
            if (take == 0) take = fit ? fit : 1;
        }
        size_t vis = take;
        if (vis && p[vis - 1] == '\r') vis--;
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            struct dt_line *g = realloc(line, (size_t)cap * sizeof *g);
            if (!g) break;
            line = g;
        }
        line[n].text = dt_strip(p, vis, prefix, &line[n].mark);
        if (!line[n].text) break;
        int lw = dt_width(hfont, line[n].text, strlen(line[n].text), tab);
        if (lw > widest) widest = lw;
        n++;
        p += take;
        if (take < len) { while (*p == ' ') p++; continue; }
        if (!*p) break;
        p++;                                     // the '\n'
    }
    int height = n * lh;
    if (fmt & DT_CALCRECT) {
        if (fmt & DT_SINGLELINE) height = lh;
        rc[2] = (int16_t)(rc[0] + widest);
        rc[3] = (int16_t)(rc[1] + height);
    } else {
        int y = rc[1];
        if (fmt & DT_SINGLELINE) {
            if (fmt & DT_VCENTER) y = rc[1] + (rc[3] - rc[1] - lh) / 2;
            else if (fmt & DT_BOTTOM) y = rc[3] - lh;
        }
        int clip[4] = { rc[0], rc[1], rc[2], rc[3] };
        for (int i = 0; i < n; i++, y += lh) {
            int lw = dt_width(hfont, line[i].text, strlen(line[i].text), tab);
            int x = fmt & DT_CENTER ? rc[0] + (width - lw) / 2 : fmt & DT_RIGHT ? rc[2] - lw : rc[0];
            draw_text_clip(w, hdc, x, y, line[i].text, tab, fmt & DT_NOCLIP ? 0 : clip);
            if (line[i].mark >= 0) {             // the mnemonic's underline, in the text colour
                int ux = x + dt_width(hfont, line[i].text, (size_t)line[i].mark, tab);
                int uw = dt_width(hfont, line[i].text + line[i].mark, 1, 0);
                struct gdi_target t;
                struct w16_gdiobj *dc = target(w, hdc, &t);
                if (dc) {
                    pthread_mutex_lock(&g_w16_text_lock);
                    int asc = w16_font_info(hfont)->ascent;
                    pthread_mutex_unlock(&g_w16_text_lock);
                    if (!(fmt & DT_NOCLIP)) {
                        if (rc[0] + t.ox > t.x0) t.x0 = rc[0] + t.ox;
                        if (rc[2] + t.ox < t.x1) t.x1 = rc[2] + t.ox;
                        if (rc[3] + t.oy < t.y1) t.y1 = rc[3] + t.oy;
                    }
                    gdi_line(&t, ux, y + asc + 1, ux + uw, y + asc + 1, w16_colorref_px(dc->text), 1, R2_COPYPEN);
                }
                gdi_done(w, dc);
            }
        }
    }
    for (int i = 0; i < n; i++) free(line[i].text);
    free(line);
    return height;
}

// `text`'s size in font `hfont` (0: the system font): width, and the
// line's height.
void w16_text_size_in(uint16_t hfont, const char *text, int *cx, int *cy) {
    pthread_mutex_lock(&g_w16_text_lock);
    const struct ugfx_font *was = ugfx_set_font(w16_font_of(hfont));
    *cx = ugfx_text_width(text);
    *cy = ugfx_char_h();
    ugfx_set_font(was);
    pthread_mutex_unlock(&g_w16_text_lock);
}

void w16_text_size(const char *text, int *cx, int *cy) { w16_text_size_in(0, text, cx, cy); }

static uint16_t dc_font(struct w16 *w, uint16_t hdc) {
    struct w16_gdiobj *dc = obj(w, hdc, GDI_DC);
    return dc ? dc->font : 0;
}

static int text_out(struct w16 *w, struct w16_args *a) {
    uint16_t hdc = w16_arg16(a);
    int16_t x = (int16_t)w16_arg16(a), y = (int16_t)w16_arg16(a);
    uint32_t far = w16_arg32(a);
    unsigned len = w16_arg16(a);
    char *text = far_text(w, far, len);
    if (!text) return -1;
    w->cpu.r[R_AX] = draw_text(w, hdc, x, y, text, 0) >= 0;
    free(text);
    return 12;
}

// --- blits ---------------------------------------------------------------------

static int bit_blt(struct w16 *w, struct w16_args *a) {
    uint16_t hd = w16_arg16(a);
    int16_t x = (int16_t)w16_arg16(a), y = (int16_t)w16_arg16(a);
    int16_t cw = (int16_t)w16_arg16(a), ch = (int16_t)w16_arg16(a);
    uint16_t hs = w16_arg16(a);
    int16_t sx = (int16_t)w16_arg16(a), sy = (int16_t)w16_arg16(a);
    uint32_t rop = w16_arg32(a);
    struct gdi_target dt, st, *sp = 0;
    struct w16_gdiobj *ddc = obj(w, hd, GDI_DC), *sdc = hs ? obj(w, hs, GDI_DC) : 0;
    // One lock for the whole call when either side is a window.
    int locked = (ddc && ddc->hwnd) || (sdc && sdc->hwnd);
    if (locked) pthread_mutex_lock(&w->lock);
    if (ddc) fill_target(w, ddc, &dt);
    if (sdc) { fill_target(w, sdc, &st); sp = &st; }
    if (ddc && ddc->hwnd) keep_children(w, ddc, &dt);   // after the source is read, not before
    if (ddc) {
        int l = x, t = y, r = x + cw, b = y + ch;
        dev_rect(ddc, &l, &t, &r, &b);
        x = (int16_t)l; y = (int16_t)t; cw = (int16_t)(r - l); ch = (int16_t)(b - t);
    }
    if (sdc) { sx = (int16_t)dev_x(sdc, sx); sy = (int16_t)dev_y(sdc, sy); }
    if (ddc) {
        int64_t br = brush_of(w, ddc->brush);
        struct gdi_blit b = {
            .dst = &dt, .src = sp, .x = x, .y = y, .w = cw, .h = ch, .sx = sx, .sy = sy,
            .rop = rop, .brush = br < 0 ? 0xFFFFFF : (uint32_t)br,
            .dst_text = w16_colorref_px(ddc->text), .dst_bk = w16_colorref_px(ddc->bk),
            .src_bk = sdc ? w16_colorref_px(sdc->bk) : 0xFFFFFF,
        };
        uint32_t pbuf[64];
        pattern_of(w, ddc, ddc->brush, &dt, &b, pbuf);
        gdi_bitblt(&b);
    }
    restore_children();
    if (ddc && ddc->hwnd) w->fb_dirty = 1;
    if (locked) pthread_mutex_unlock(&w->lock);
    w->cpu.r[R_AX] = ddc != 0;
    return 20;
}

// StretchBlt: the source rectangle sampled to the destination's size
// (nearest pixel, a negative extent mirroring), then blitted as BitBlt --
// COLORONCOLOR's result, which is what 3.1 drew a colour stretch with.
static int stretch_blt(struct w16 *w, struct w16_args *a) {
    uint16_t hd = w16_arg16(a);
    int16_t x = (int16_t)w16_arg16(a), y = (int16_t)w16_arg16(a);
    int16_t cw = (int16_t)w16_arg16(a), ch = (int16_t)w16_arg16(a);
    uint16_t hs = w16_arg16(a);
    int16_t sx = (int16_t)w16_arg16(a), sy = (int16_t)w16_arg16(a);
    int16_t sw = (int16_t)w16_arg16(a), sh = (int16_t)w16_arg16(a);
    uint32_t rop = w16_arg32(a);
    struct w16_gdiobj *ddc = obj(w, hd, GDI_DC), *sdc = hs ? obj(w, hs, GDI_DC) : 0;
    w->cpu.r[R_AX] = ddc != 0;
    if (!ddc || !cw || !ch) return 24;
    {   // both rectangles in pixels, a mirroring kept as a negative extent
        int x1 = dev_x(ddc, x + cw), y1 = dev_y(ddc, y + ch);
        x = (int16_t)dev_x(ddc, x); y = (int16_t)dev_y(ddc, y);
        cw = (int16_t)(x1 - x); ch = (int16_t)(y1 - y);
        if (sdc) {
            int sx1 = dev_x(sdc, sx + sw), sy1 = dev_y(sdc, sy + sh);
            sx = (int16_t)dev_x(sdc, sx); sy = (int16_t)dev_y(sdc, sy);
            sw = (int16_t)(sx1 - sx); sh = (int16_t)(sy1 - sy);
        }
        if (!cw || !ch) return 24;
    }
    int dw = cw < 0 ? -cw : cw, dh = ch < 0 ? -ch : ch;
    int dx = cw < 0 ? x + cw : x, dy = ch < 0 ? y + ch : y;
    int flip_x = (cw < 0) != (sw < 0), flip_y = (ch < 0) != (sh < 0);
    int aw = sw < 0 ? -sw : sw, ah = sh < 0 ? -sh : sh;
    int ax = sw < 0 ? sx + sw : sx, ay = sh < 0 ? sy + sh : sy;
    uint32_t *tmp = 0;
    int locked = ddc->hwnd || (sdc && sdc->hwnd);
    if (locked) pthread_mutex_lock(&w->lock);
    struct gdi_target dt, st, tt;
    fill_target(w, ddc, &dt);
    if (sdc && aw && ah) {
        fill_target(w, sdc, &st);
        tmp = malloc((size_t)dw * dh * 4);
        if (tmp) {
            for (int j = 0; j < dh; j++) {
                int v = ay + (flip_y ? dh - 1 - j : j) * ah / dh + st.oy;
                for (int i = 0; i < dw; i++) {
                    int u = ax + (flip_x ? dw - 1 - i : i) * aw / dw + st.ox;
                    int in = st.px && u >= st.x0 && u < st.x1 && v >= st.y0 && v < st.y1;
                    tmp[(size_t)j * dw + i] = in ? st.px[(size_t)v * st.w + u] : 0xFFFFFF;
                }
            }
            tt = (struct gdi_target){ .px = tmp, .w = dw, .h = dh, .mono = st.mono };
            gdi_target_whole(&tt);
        }
    }
    if (ddc->hwnd) keep_children(w, ddc, &dt);
    int64_t br = brush_of(w, ddc->brush);
    struct gdi_blit b = {
        .dst = &dt, .src = tmp ? &tt : 0, .x = dx, .y = dy, .w = dw, .h = dh,
        .rop = rop, .brush = br < 0 ? 0xFFFFFF : (uint32_t)br,
        .dst_text = w16_colorref_px(ddc->text), .dst_bk = w16_colorref_px(ddc->bk),
        .src_bk = sdc ? w16_colorref_px(sdc->bk) : 0xFFFFFF,
    };
    uint32_t pbuf[64];
    pattern_of(w, ddc, ddc->brush, &dt, &b, pbuf);
    gdi_bitblt(&b);
    restore_children();
    if (ddc->hwnd) w->fb_dirty = 1;
    if (locked) pthread_mutex_unlock(&w->lock);
    free(tmp);
    return 24;
}

static int pat_blt(struct w16 *w, struct w16_args *a) {
    uint16_t hd = w16_arg16(a);
    int16_t x = (int16_t)w16_arg16(a), y = (int16_t)w16_arg16(a);
    int16_t cw = (int16_t)w16_arg16(a), ch = (int16_t)w16_arg16(a);
    uint32_t rop = w16_arg32(a);
    struct gdi_target dt;
    struct w16_gdiobj *dc = target(w, hd, &dt);
    if (dc) {
        int l = x, t = y, r = x + cw, bt = y + ch;
        dev_rect(dc, &l, &t, &r, &bt);
        int64_t br = brush_of(w, dc->brush);
        struct gdi_blit b = { .dst = &dt, .x = l, .y = t, .w = r - l, .h = bt - t, .rop = rop,
                              .brush = br < 0 ? 0xFFFFFF : (uint32_t)br };
        uint32_t pbuf[64];
        pattern_of(w, dc, dc->brush, &dt, &b, pbuf);
        gdi_bitblt(&b);
    }
    gdi_done(w, dc);
    w->cpu.r[R_AX] = dc != 0;
    return 14;
}

void w16_gdi_fill(struct w16 *w, uint16_t hdc, int l, int t, int r, int b, uint16_t brush) {
    int64_t br = brush_of(w, brush);
    if (br < 0) return;
    struct gdi_target dt;
    struct w16_gdiobj *dc = target(w, hdc, &dt);
    if (dc) {
        dev_rect(dc, &l, &t, &r, &b);
        struct gdi_blit bl = { .dst = &dt, .x = l, .y = t, .w = r - l, .h = b - t,
                               .rop = 0x00F00021, .brush = (uint32_t)br };
        uint32_t pbuf[64];
        pattern_of(w, dc, brush, &dt, &bl, pbuf);
        gdi_bitblt(&bl);
    }
    gdi_done(w, dc);
}

void w16_gdi_draw_argb(struct w16 *w, uint16_t hdc, int x, int y, const uint32_t *px, int pw, int ph) {
    struct gdi_target t;
    struct w16_gdiobj *dc = target(w, hdc, &t);
    if (dc) { x = dev_x(dc, x); y = dev_y(dc, y); }
    for (int j = 0; dc && t.px && j < ph; j++)
        for (int i = 0; i < pw; i++) {
            uint32_t p = px[j * pw + i];
            int bx = x + i + t.ox, by = y + j + t.oy;
            if (!(p >> 24) || bx < t.x0 || by < t.y0 || bx >= t.x1 || by >= t.y1) continue;
            t.px[(size_t)by * t.w + bx] = t.mono ? ((p & 0xFFFFFF) ? 0xFFFFFF : 0) : (p & 0xFFFFFF);
        }
    gdi_done(w, dc);
}

void w16_gdi_invert(struct w16 *w, uint16_t hdc, int l, int t, int r, int b) {
    struct gdi_target dt;
    struct w16_gdiobj *dc = target(w, hdc, &dt);
    if (dc) {
        dev_rect(dc, &l, &t, &r, &b);
        struct gdi_blit bl = { .dst = &dt, .x = l, .y = t, .w = r - l, .h = b - t, .rop = 0x00550009 };
        gdi_bitblt(&bl);                         // DSTINVERT
    }
    gdi_done(w, dc);
}

// The raster operation a shape's inside is filled with: its DC's ROP2 mix
// over the brush, as GDI filled them -- R2_NOTXORPEN twice puts a button back
// (Calculator's press). FillRect alone is PATCOPY whatever the mix.
static uint32_t fill_rop(const struct w16_gdiobj *dc) {
    static const uint8_t R2[16] = { 0x00, 0x05, 0x0A, 0x0F, 0x50, 0x55, 0x5A, 0x5F,
                                    0xA0, 0xA5, 0xAA, 0xAF, 0xF0, 0xF5, 0xFA, 0xFF };
    unsigned m = dc->rop2 >= 1 && dc->rop2 <= 16 ? dc->rop2 : 13;
    return (uint32_t)R2[m - 1] << 16;
}

static int rectangle(struct w16 *w, struct w16_args *a) {
    uint16_t hd = w16_arg16(a);
    int l = (int16_t)w16_arg16(a), t = (int16_t)w16_arg16(a);
    int r = (int16_t)w16_arg16(a), b = (int16_t)w16_arg16(a);
    struct gdi_target dt;
    struct w16_gdiobj *dc = target(w, hd, &dt);
    if (dc) {
        dev_rect(dc, &l, &t, &r, &b);
        int64_t br = brush_of(w, dc->brush);
        int width;
        int nopen = pen_of(w, dc->pen, &width) < 0;
        if (br >= 0) {
            // inside the edge; with a NULL pen the whole box but its right and bottom rows, as GDI filled it
            struct gdi_blit bl = { .dst = &dt, .x = l + !nopen, .y = t + !nopen, .w = r - l - 1 - !nopen,
                                   .h = b - t - 1 - !nopen, .rop = fill_rop(dc), .brush = (uint32_t)br };
            uint32_t pbuf[64];
            pattern_of(w, dc, dc->brush, &dt, &bl, pbuf);
            gdi_bitblt(&bl);
        }
        int64_t pen = pen_of(w, dc->pen, &width);
        if (pen >= 0) {
            gdi_line(&dt, l, t, r - 1, t, (uint32_t)pen, width, dc->rop2);
            gdi_line(&dt, r - 1, t, r - 1, b - 1, (uint32_t)pen, width, dc->rop2);
            gdi_line(&dt, r - 1, b - 1, l, b - 1, (uint32_t)pen, width, dc->rop2);
            gdi_line(&dt, l, b - 1, l, t, (uint32_t)pen, width, dc->rop2);
        }
    }
    gdi_done(w, dc);
    w->cpu.r[R_AX] = dc != 0;
    return 10;
}

// --- shapes: a coverage mask, filled with the brush and edged with the pen -----------


// mask[y * mw + x] set where the shape covers (x0 + x, y0 + y), device pixels.
struct mask { int x0, y0, w, h; uint8_t *m; };

static int mask_new(struct mask *k, int l, int t, int r, int b) {
    k->x0 = l; k->y0 = t; k->w = r - l; k->h = b - t;
    k->m = 0;
    if (k->w <= 0 || k->h <= 0 || (long)k->w * k->h > 16L * 1024 * 1024) return 0;
    k->m = calloc((size_t)k->w * k->h, 1);
    return k->m != 0;
}

static int mask_at(const struct mask *k, int x, int y) {
    return x >= 0 && y >= 0 && x < k->w && y < k->h && k->m[(size_t)y * k->w + x];
}

// The midpoint ellipse of radii a and b (Bresenham's, in integers): the
// widest x of each row dy = 0..b from the centre, into xm[0..b].
static void ellipse_widths(long a, long b, int *xm) {
    for (long i = 0; i <= b; i++) xm[i] = 0;
    if (a <= 0 || b <= 0) { if (b >= 0) xm[0] = (int)(a > 0 ? a : 0); return; }
    long long a2 = a * a, b2 = b * b, x = 0, y = b, dx = 0, dy = 2 * a2 * b;
    long long d = 4 * b2 - 4 * a2 * b + a2;          // the decision variable, times 4
    while (dx < dy) {                               // where the slope is under 1
        if (x > xm[y]) xm[y] = (int)x;
        x++;
        dx += 2 * b2;
        if (d < 0) d += 4 * (dx + b2);
        else { y--; dy -= 2 * a2; d += 4 * (dx - dy + b2); }
    }
    d = b2 * (2 * x + 1) * (2 * x + 1) + 4 * a2 * (y - 1) * (y - 1) - 4 * a2 * b2;
    while (y >= 0) {
        if (x > xm[y]) xm[y] = (int)x;
        y--;
        dy -= 2 * a2;
        if (d > 0) d += 4 * (a2 - dy);
        else { x++; dx += 2 * b2; d += 4 * (dx - dy + a2); }
    }
}

// How far each row of a W x H ellipse starts in from its box's left (and
// ends from its right): the midpoint ellipse of radii (W - 1) / 2 and
// (H - 1) / 2, an even size doubling its middle column or row -- 3.11's
// Ellipse, row for row (Calculator's corners, docs/w16-design.md). insets
// holds H rows; 0 when out of memory.
static int *ellipse_insets(int W, int H) {
    long a = (W - 1) / 2, b = (H - 1) / 2;
    int *xm = malloc(sizeof *xm * (size_t)(b + 1)), *in = malloc(sizeof *in * (size_t)(H > 0 ? H : 1));
    if (!xm || !in) { free(xm); free(in); return 0; }
    ellipse_widths(a, b, xm);
    for (int y = 0; y < H; y++) {
        int yy = y <= b ? y : H - 1 - y;
        int v = (int)(a - xm[b - yy]);
        in[y] = v > 0 ? v : 0;
    }
    free(xm);
    return in;
}

// The ellipse inside the mask's box.
static void mask_ellipse(struct mask *k) {
    int *in = ellipse_insets(k->w, k->h);
    for (int y = 0; in && y < k->h; y++)
        for (int x = in[y]; x < k->w - in[y]; x++) k->m[(size_t)y * k->w + x] = 1;
    free(in);
}

// The box with its corners rounded: each the quarter of an ellipse whose box
// is one more than the corner's size, the box's own size at most -- so a
// corner as big as the box is that box's Ellipse, as 3.11's was.
static void mask_roundrect(struct mask *k, int ew, int eh) {
    int cw = (ew < k->w - 1 ? ew : k->w - 1) + 1, ch = (eh < k->h - 1 ? eh : k->h - 1) + 1;
    int b = (ch - 1) / 2, *in = ew > 0 && eh > 0 ? ellipse_insets(cw, ch) : 0;
    for (int y = 0; y < k->h; y++) {
        int inset = 0;
        if (in && y < b) inset = in[y];
        else if (in && y >= k->h - b) inset = in[k->h - 1 - y];
        for (int x = inset; x < k->w - inset; x++) k->m[(size_t)y * k->w + x] = 1;
    }
    free(in);
}

static long ceil_half(long v) { return v >= 0 ? (v + 1) / 2 : -(-v / 2); }

// Polygons, each `n[i]` points of `pt` (device pixels), filled ALTERNATE (1)
// or WINDING (2) by the rows' middles, as GDI's scan conversion.
static void mask_polygons(struct mask *k, const int *pt, const int *n, int npoly, int mode) {
    int total = 0;
    for (int i = 0; i < npoly; i++) total += n[i];
    long *xs = malloc(sizeof *xs * (size_t)(total + 1));
    int *dir = malloc(sizeof *dir * (size_t)(total + 1));
    if (!xs || !dir) { free(xs); free(dir); return; }
    for (int y = 0; y < k->h; y++) {
        long Y2 = 2L * (k->y0 + y) + 1;          // the row's middle, in half pixels
        int c = 0, base = 0;
        for (int p = 0; p < npoly; base += n[p], p++)
            for (int i = 0; i < n[p]; i++) {
                const int *a = pt + 2 * (base + i), *b = pt + 2 * (base + (i + 1) % n[p]);
                long ya = 2L * a[1], yb = 2L * b[1];
                if (ya == yb || !((ya <= Y2 && Y2 < yb) || (yb <= Y2 && Y2 < ya))) continue;
                // x at Y2, doubled: the crossing's place in half pixels
                xs[c] = 2L * a[0] + (Y2 - ya) * (2L * (b[0] - a[0])) / (yb - ya);
                dir[c++] = yb > ya ? 1 : -1;
            }
        for (int i = 1; i < c; i++)              // sort the crossings left to right
            for (int j = i; j > 0 && xs[j - 1] > xs[j]; j--) {
                long tx = xs[j]; xs[j] = xs[j - 1]; xs[j - 1] = tx;
                int td = dir[j]; dir[j] = dir[j - 1]; dir[j - 1] = td;
            }
        int wind = 0;
        for (int i = 0; i + 1 < c; i++) {
            wind += dir[i];
            int in = mode == 2 ? wind != 0 : !(i & 1);
            if (!in) continue;
            // pixels whose middles (2x + 1) lie in [xs[i], xs[i + 1])
            long from = ceil_half(xs[i] - 1) - k->x0, to = ceil_half(xs[i + 1] - 1) - k->x0;
            if (from < 0) from = 0;
            if (to > k->w) to = k->w;
            for (long x = from; x < to; x++) k->m[(size_t)y * k->w + x] = 1;
        }
    }
    free(xs);
    free(dir);
}

// The mask's pixels in the DC's brush, row runs as PATCOPY blits (the
// pattern and the clip are gdi_bitblt's).
static void fill_mask(struct w16 *w, const struct w16_gdiobj *dc, struct gdi_target *t, const struct mask *k) {
    int64_t br = brush_of(w, dc->brush);
    if (br < 0 || !k->m) return;
    uint32_t pbuf[64];
    struct gdi_blit tpl = { .dst = t, .h = 1, .rop = fill_rop(dc), .brush = (uint32_t)br };
    pattern_of(w, dc, dc->brush, t, &tpl, pbuf);
    for (int y = 0; y < k->h; y++)
        for (int x = 0; x < k->w; ) {
            if (!k->m[(size_t)y * k->w + x]) { x++; continue; }
            int e = x;
            while (e < k->w && k->m[(size_t)y * k->w + e]) e++;
            struct gdi_blit b = tpl;
            b.x = k->x0 + x;
            b.y = k->y0 + y;
            b.w = e - x;
            gdi_bitblt(&b);
            x = e;
        }
}

// The mask's edge -- a covered pixel with a neighbour outside -- in the DC's pen.
static void edge_mask(struct w16 *w, const struct w16_gdiobj *dc, struct gdi_target *t, const struct mask *k) {
    int width;
    int64_t pen = pen_of(w, dc->pen, &width);
    if (pen < 0 || !k->m) return;
    for (int y = 0; y < k->h; y++)
        for (int x = 0; x < k->w; x++)
            if (mask_at(k, x, y) && (!mask_at(k, x - 1, y) || !mask_at(k, x + 1, y) || !mask_at(k, x, y - 1) ||
                                     !mask_at(k, x, y + 1)))
                gdi_pixel(t, k->x0 + x, k->y0 + y, (uint32_t)pen, dc->rop2);
}

static int is_edge(const struct mask *k, int x, int y) {
    return mask_at(k, x, y) && (!mask_at(k, x - 1, y) || !mask_at(k, x + 1, y) || !mask_at(k, x, y - 1) ||
                                !mask_at(k, x, y + 1));
}

// A closed shape: its edge in the pen, the rest of it in the brush -- each
// pixel once, so a mix (R2_NOTXORPEN) drawn twice undoes itself. With a
// NULL pen the brush fills the edge too.
static void fill_and_edge(struct w16 *w, const struct w16_gdiobj *dc, struct gdi_target *t, struct mask *k) {
    int width;
    int64_t pen = pen_of(w, dc->pen, &width);
    uint8_t *edge = pen >= 0 && k->m ? calloc((size_t)k->w * k->h, 1) : 0;
    if (!edge) {
        if (pen < 0) fill_mask(w, dc, t, k);
        else edge_mask(w, dc, t, k);
        return;
    }
    for (int y = 0; y < k->h; y++)
        for (int x = 0; x < k->w; x++) edge[(size_t)y * k->w + x] = (uint8_t)is_edge(k, x, y);
    for (size_t i = 0; i < (size_t)k->w * k->h; i++) if (edge[i]) k->m[i] = 0;
    fill_mask(w, dc, t, k);
    for (int y = 0; y < k->h; y++)
        for (int x = 0; x < k->w; x++)
            if (edge[(size_t)y * k->w + x]) gdi_pixel(t, k->x0 + x, k->y0 + y, (uint32_t)pen, dc->rop2);
    free(edge);
}

// --- arcs: Arc, Pie, Chord -------------------------------------------------------
//
// AS 3.11 MADE THEM, measured pixel for pixel (samples/shapes.asm): the
// ellipse's own outline pixels, counterclockwise as the screen shows it from
// the radial line through the first point to the one through the second --
// then, for a pie, a line from the arc's end to the centre pixel and one from
// the centre back to its start; for a chord, one from the end to the start.
// That closed outline is a polygon, filled as Polygon fills.

enum { ARC_ARC, ARC_CHORD, ARC_PIE };

struct arc_px { int x, y; double d; };

static int arc_px_cmp(const void *a, const void *b) {
    double d = ((const struct arc_px *)a)->d - ((const struct arc_px *)b)->d;
    return d < 0 ? -1 : d > 0;
}

// gdi_line's pixels, its end left out, into the mask.
static void mask_line(struct mask *k, int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    while (x0 != x1 || y0 != y1) {
        int x = x0 - k->x0, y = y0 - k->y0;
        if (x >= 0 && y >= 0 && x < k->w && y < k->h) k->m[(size_t)y * k->w + x] = 1;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// What the closed outline `line` encloses inside the ellipse `e`, grown
// from the pixels `k` already holds (the polygon of its points, which misses
// some beside the curve): a 4-connected fill, which no 8-connected outline
// lets through.
static void enclosed(struct mask *k, const struct mask *e, const struct mask *line) {
    size_t n = (size_t)k->w * k->h, top = 0;
    size_t *stack = malloc(sizeof *stack * (n + 1));
    if (!stack) return;
    for (size_t i = 0; i < n; i++) {
        k->m[i] = (uint8_t)(k->m[i] && e->m[i] && !line->m[i]);
        if (k->m[i]) stack[top++] = i;
    }
    while (top) {
        size_t i = stack[--top];
        int x = (int)(i % (size_t)k->w), y = (int)(i / (size_t)k->w);
        const int nx[4] = { x - 1, x + 1, x, x }, ny[4] = { y, y, y - 1, y + 1 };
        for (int d = 0; d < 4; d++) {
            if (nx[d] < 0 || ny[d] < 0 || nx[d] >= k->w || ny[d] >= k->h) continue;
            size_t j = (size_t)ny[d] * k->w + nx[d];
            if (k->m[j] || !e->m[j] || line->m[j]) continue;
            k->m[j] = 1;
            stack[top++] = j;
        }
    }
    free(stack);
}

// Arc's, Chord's and Pie's work, in logical units.
static int arc_shape(struct w16 *w, uint16_t hdc, int kind, int l, int t, int r, int b, int x1, int y1, int x2,
                     int y2) {
    struct gdi_target dt;
    struct w16_gdiobj *dc = target(w, hdc, &dt);
    struct mask e = { 0 }, line = { 0 }, k = { 0 };
    struct arc_px *px = 0;
    int *pt = 0;
    if (!dc) goto done;
    dev_rect(dc, &l, &t, &r, &b);
    x1 = dev_x(dc, x1); y1 = dev_y(dc, y1);
    x2 = dev_x(dc, x2); y2 = dev_y(dc, y2);
    if (!mask_new(&e, l, t, r, b) || !mask_new(&line, l, t, r, b)) goto done;
    mask_ellipse(&e);
    // the sweep, from the box's middle in whole pixels, y up
    double cx = floor((l + r) / 2.0), cy = floor((t + b) / 2.0), two_pi = 2 * M_PI;
    double as = atan2(cy - y1, x1 - cx), ae = atan2(cy - y2, x2 - cx);
    double sweep = fmod(ae - as + 2 * two_pi, two_pi);
    int whole = (x1 == x2 && y1 == y2) || sweep == 0;
    int n = 0, edges = 0;
    for (int y = 0; y < e.h; y++)
        for (int x = 0; x < e.w; x++) edges += is_edge(&e, x, y);
    px = malloc(sizeof *px * (size_t)(edges + 1));
    if (!px) goto done;
    for (int y = 0; y < e.h; y++)
        for (int x = 0; x < e.w; x++) {
            if (!is_edge(&e, x, y)) continue;
            double d = fmod(atan2(cy - (t + y), (l + x) - cx) - as + 2 * two_pi, two_pi);
            if (whole || d <= sweep + 1e-9) px[n++] = (struct arc_px){ l + x, t + y, d };
        }
    qsort(px, (size_t)n, sizeof *px, arc_px_cmp);
    for (int i = 0; i < n; i++) line.m[(size_t)(px[i].y - t) * line.w + (px[i].x - l)] = 1;
    int ccx = (int)floor((l + r - 1) / 2.0), ccy = (int)floor((t + b - 1) / 2.0);   // the radii's centre pixel
    if (n && !whole && kind == ARC_PIE) {
        mask_line(&line, px[n - 1].x, px[n - 1].y, ccx, ccy);
        mask_line(&line, ccx, ccy, px[0].x, px[0].y);
    } else if (n && !whole && kind == ARC_CHORD)
        mask_line(&line, px[n - 1].x, px[n - 1].y, px[0].x, px[0].y);
    int width;
    int64_t pen = pen_of(w, dc->pen, &width);
    if (kind != ARC_ARC && n && mask_new(&k, l, t, r, b)) {
        if (whole) memcpy(k.m, e.m, (size_t)e.w * e.h);
        else {
            int np = n + (kind == ARC_PIE);
            pt = malloc(sizeof *pt * 2 * (size_t)np);
            if (!pt) goto done;
            for (int i = 0; i < n; i++) { pt[2 * i] = px[i].x; pt[2 * i + 1] = px[i].y; }
            if (kind == ARC_PIE) { pt[2 * n] = ccx; pt[2 * n + 1] = ccy; }
            mask_polygons(&k, pt, &np, 1, 1);
            enclosed(&k, &e, &line);
        }
        for (size_t i = 0; i < (size_t)k.w * k.h; i++)
            if (line.m[i]) k.m[i] = (uint8_t)(pen < 0);   // the outline is the pen's, or the brush's with none
        fill_mask(w, dc, &dt, &k);
    }
    if (pen >= 0)
        for (int y = 0; y < line.h; y++)
            for (int x = 0; x < line.w; x++)
                if (line.m[(size_t)y * line.w + x]) gdi_pixel(&dt, l + x, t + y, (uint32_t)pen, dc->rop2);
done:
    free(e.m);
    free(line.m);
    free(k.m);
    free(px);
    free(pt);
    gdi_done(w, dc);
    return dc != 0;
}

// Ellipse's work, in logical units: the box's right and bottom left out.
static int ellipse(struct w16 *w, uint16_t hdc, int l, int t, int r, int b) {
    struct gdi_target dt;
    struct w16_gdiobj *dc = target(w, hdc, &dt);
    if (dc) {
        dev_rect(dc, &l, &t, &r, &b);
        struct mask k;
        if (mask_new(&k, l, t, r, b)) {
            mask_ellipse(&k);
            fill_and_edge(w, dc, &dt, &k);
        }
        free(k.m);
    }
    gdi_done(w, dc);
    return dc != 0;
}

// RoundRect's work, in logical units: the box's right and bottom left out.
static int roundrect(struct w16 *w, uint16_t hdc, int l, int t, int r, int b, int ew, int eh) {
    struct gdi_target dt;
    struct w16_gdiobj *dc = target(w, hdc, &dt);
    if (dc) {
        dev_rect(dc, &l, &t, &r, &b);
        ew = abs(dev_w(dc, ew));
        eh = abs(dev_h(dc, eh));
        struct mask k;
        if (mask_new(&k, l, t, r, b)) {
            mask_roundrect(&k, ew, eh);
            fill_and_edge(w, dc, &dt, &k);
        }
        free(k.m);
    }
    gdi_done(w, dc);
    return dc != 0;
}

// Polygon's and PolyPolygon's work: `pt` logical, mapped here; each outline
// closed in the pen, the inside in the brush by the DC's fill mode.
static int polygons(struct w16 *w, uint16_t hdc, int *pt, const int *n, int npoly, int fill) {
    struct gdi_target dt;
    struct w16_gdiobj *dc = target(w, hdc, &dt);
    if (dc) {
        int total = 0, l = INT_MAX, tp = INT_MAX, r = INT_MIN, b = INT_MIN;
        for (int i = 0; i < npoly; i++) total += n[i];
        for (int i = 0; i < total; i++) {
            pt[2 * i] = dev_x(dc, pt[2 * i]);
            pt[2 * i + 1] = dev_y(dc, pt[2 * i + 1]);
            if (pt[2 * i] < l) l = pt[2 * i];
            if (pt[2 * i] > r) r = pt[2 * i];
            if (pt[2 * i + 1] < tp) tp = pt[2 * i + 1];
            if (pt[2 * i + 1] > b) b = pt[2 * i + 1];
        }
        struct mask k = { 0 };
        if (fill && total && mask_new(&k, l, tp, r + 1, b + 1)) {
            mask_polygons(&k, pt, n, npoly, dc->fill_mode);
            fill_mask(w, dc, &dt, &k);
        }
        free(k.m);
        int width;
        int64_t pen = pen_of(w, dc->pen, &width);
        if (width > 1) width = abs(dev_w(dc, width));
        for (int p = 0, base = 0; pen >= 0 && p < npoly; base += n[p], p++)
            for (int i = 0; i + (fill ? 0 : 1) < n[p]; i++) {
                const int *a = pt + 2 * (base + i), *c = pt + 2 * (base + (i + 1) % n[p]);
                gdi_line(&dt, a[0], a[1], c[0], c[1], (uint32_t)pen, width ? width : 1, dc->rop2);
            }
    }
    gdi_done(w, dc);
    return dc != 0;
}

// --- DIBs ----------------------------------------------------------------------

// A BITMAPINFO at a far pointer: its header and palette, checked against
// the segment. NULL after setting a fault.
static const uint8_t *bmi_at(struct w16 *w, uint32_t far, int usage, struct gdi_dib *d) {
    uint16_t sel = (uint16_t)(far >> 16), off = (uint16_t)far;
    const uint8_t *p = cpu16_ptr(&w->cpu, sel, off, 12, 0);
    if (!p) return 0;
    // The header plus the largest palette, or whatever of it the segment holds.
    uint32_t avail = 0;
    for (uint32_t n = 40 + 256 * 4; n >= 12; n /= 2)
        if (cpu16_ptr(&w->cpu, sel, off, n, 0)) { avail = n; break; }
        else w->cpu.fault = 0;
    if (gdi_dib_header(p, avail, usage == 1, d) < 0) {
        cpu16_set_fault(&w->cpu, FAULT_API, "a bitmap in a format w16 cannot read (compressed?)");
        return 0;
    }
    return p;
}

// Rows [first, first + n) of a DIB's bits, numbered from the BOTTOM as
// Windows numbers a bottom-up DIB's scan lines. NULL after a fault.
static const uint8_t *dib_bits(struct w16 *w, uint32_t far, const struct gdi_dib *d, unsigned n) {
    return cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, d->stride * (n ? n : 1), 0);
}

static int create_dibitmap(struct w16 *w, struct w16_args *a) {
    w16_arg16(a);                                // the DC: every bitmap is the display's
    uint32_t lpbmih = w16_arg32(a), init = w16_arg32(a), lpbits = w16_arg32(a), lpbmi = w16_arg32(a);
    uint16_t usage = w16_arg16(a);
    struct gdi_dib d;
    if (!bmi_at(w, lpbmih, 0, &d)) return -1;
    int bh = d.h < 0 ? -d.h : d.h;
    uint16_t h;
    struct w16_gdiobj *bm = new_bitmap(w, d.w, bh, 0, &h);
    if (bm && (init & 4)) {                      // CBM_INIT
        static struct gdi_dib pd;                // the task thread's alone; a palette is 1 KiB
        if (!bmi_at(w, lpbmi, usage, &pd)) return -1;
        const uint8_t *bits = dib_bits(w, lpbits, &pd, (unsigned)bh);
        if (!bits) return -1;
        for (int y = 0; y < bh; y++) {
            const uint8_t *row = bits + (size_t)(pd.h < 0 ? y : bh - 1 - y) * pd.stride;
            for (int x = 0; x < d.w && x < pd.w; x++) bm->px[(size_t)y * d.w + x] = gdi_dib_px(&pd, row, x);
        }
    }
    w->cpu.r[R_AX] = h;
    return 20;
}

uint16_t w16_gdi_bitmap_of_dib(struct w16 *w, const uint8_t *dib, uint32_t len) {
    struct gdi_dib d;
    if (gdi_dib_header(dib, len, 0, &d) < 0) return 0;
    int bh = d.h < 0 ? -d.h : d.h;
    if ((uint64_t)d.header_len + (uint64_t)d.stride * (unsigned)bh > len) return 0;
    // Monochrome only when its two colours are black then white -- Windows'
    // rule, so a mask stays a mask and a two-colour picture keeps its colours.
    int mono = d.bpp == 1 && d.ncolors == 2 && (d.pal[0] & 0xFFFFFF) == 0 && (d.pal[1] & 0xFFFFFF) == 0xFFFFFF;
    uint16_t h;
    struct w16_gdiobj *bm = new_bitmap(w, d.w, bh, mono, &h);
    if (!bm) return 0;
    const uint8_t *bits = dib + d.header_len;
    for (int y = 0; y < bh; y++) {
        const uint8_t *row = bits + (size_t)(d.h < 0 ? y : bh - 1 - y) * d.stride;
        for (int x = 0; x < d.w; x++) bm->px[(size_t)y * d.w + x] = gdi_dib_px(&d, row, x);
    }
    return h;
}

// The 3.1 VGA driver's 16 colours, as a 4-plane bitmap's planes spell them:
// red, green and blue bits, then intensity -- 7 is the dark grey, 8 the light.
static const uint32_t VGA16[16] = {
    0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0x808080,
    0xC0C0C0, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF,
};

uint16_t w16_gdi_bitmap_of_ddb(struct w16 *w, const uint8_t *p, uint32_t len) {
    if (len < 10) return 0;
    int bw = p[2] | p[3] << 8, bh = p[4] | p[5] << 8, wb = p[6] | p[7] << 8, planes = p[8], bpp = p[9];
    uint64_t bits = (uint64_t)wb * (unsigned)bh * (unsigned)planes;
    if (bw <= 0 || bh <= 0 || bits + 10 > len || wb < (bw * bpp + 7) / 8) return 0;
    const uint8_t *b = p + (len - bits >= 14 ? 14 : 10);   // BITMAP's bmBits pointer kept, or not
    int mono = planes == 1 && bpp == 1;
    if (!mono && !(planes == 4 && bpp == 1) && !(planes == 1 && bpp == 24)) return 0;
    uint16_t h;
    struct w16_gdiobj *bm = new_bitmap(w, bw, bh, mono, &h);
    if (!bm) return 0;
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++) {
            uint32_t px;
            if (mono) {
                px = b[(size_t)y * wb + x / 8] >> (7 - x % 8) & 1 ? 0xFFFFFF : 0;
            } else if (planes == 4) {            // each plane whole, one after another
                unsigned v = 0;
                for (int pl = 0; pl < 4; pl++)
                    v |= (unsigned)(b[(size_t)pl * wb * bh + (size_t)y * wb + x / 8] >> (7 - x % 8) & 1) << pl;
                px = w16_colorref_px((VGA16[v] & 0xFF) << 16 | (VGA16[v] & 0xFF00) | VGA16[v] >> 16);
            } else {
                const uint8_t *q = b + (size_t)y * wb + (size_t)x * 3;
                px = w16_colorref_px((uint32_t)q[2] | (uint32_t)q[1] << 8 | (uint32_t)q[0] << 16);
            }
            bm->px[(size_t)y * bw + x] = px;
        }
    return h;
}

static int set_dibits_to_device(struct w16 *w, struct w16_args *a) {
    uint16_t hdc = w16_arg16(a);
    int16_t xd = (int16_t)w16_arg16(a), yd = (int16_t)w16_arg16(a);
    int16_t cx = (int16_t)w16_arg16(a), cy = (int16_t)w16_arg16(a);
    int16_t xs = (int16_t)w16_arg16(a), ys = (int16_t)w16_arg16(a);
    uint16_t start = w16_arg16(a), lines = w16_arg16(a);
    uint32_t lpbits = w16_arg32(a), lpbmi = w16_arg32(a);
    uint16_t usage = w16_arg16(a);
    struct gdi_dib d;
    if (!bmi_at(w, lpbmi, usage, &d)) return -1;
    const uint8_t *bits = dib_bits(w, lpbits, &d, lines);
    if (!bits) return -1;
    int dib_h = d.h < 0 ? -d.h : d.h;
    struct gdi_target t;
    struct w16_gdiobj *dc = target(w, hdc, &t);
    if (dc) { xd = (int16_t)dev_x(dc, xd); yd = (int16_t)dev_y(dc, yd); }
    if (dc && t.px) {
        for (int i = 0; i < cy; i++) {
            // Destination row yd+i shows DIB row (from the bottom) ys+cy-1-i.
            int dib_row = d.h < 0 ? dib_h - 1 - (ys + i) : ys + cy - 1 - i;
            if (dib_row < (int)start || dib_row >= (int)start + (int)lines) continue;
            int ty = yd + i + t.oy;
            if (ty < t.y0 || ty >= t.y1) continue;
            const uint8_t *row = bits + (size_t)(dib_row - start) * d.stride;
            for (int j = 0; j < cx; j++) {
                int sxp = xs + j, tx = xd + j + t.ox;
                if (sxp < 0 || sxp >= d.w || tx < t.x0 || tx >= t.x1) continue;
                t.px[(size_t)ty * t.w + tx] = gdi_dib_px(&d, row, sxp);
            }
        }
    }
    gdi_done(w, dc);
    w->cpu.r[R_AX] = lines;
    return 28;
}

// --- objects -------------------------------------------------------------------

static uint16_t select_object(struct w16 *w, uint16_t hdc, uint16_t h) {
    struct w16_gdiobj *dc = obj(w, hdc, GDI_DC);
    if (!dc) return 0;
    uint16_t old = 0;
    switch (type_of(w, h)) {
    case GDI_PEN:   old = dc->pen; dc->pen = h; break;
    case GDI_BRUSH: old = dc->brush; dc->brush = h; break;
    case GDI_FONT:  old = dc->font; dc->font = h; break;
    case GDI_BITMAP: {
        if (dc->hwnd || dc->screen) return 0;
        struct w16_gdiobj *bm = obj(w, h, GDI_BITMAP), *was = obj(w, dc->bitmap, GDI_BITMAP);
        if (!bm || (bm->in_dc && bm->in_dc != hdc)) return 0;
        if (was) was->in_dc = 0;
        old = dc->bitmap ? dc->bitmap : DEFAULT_BITMAP;
        dc->bitmap = h;
        bm->in_dc = hdc;
        break;
    }
    default:
        if (h == DEFAULT_BITMAP && !dc->hwnd) {   // the default bitmap back
            struct w16_gdiobj *was = obj(w, dc->bitmap, GDI_BITMAP);
            if (was) was->in_dc = 0;
            old = dc->bitmap;
            dc->bitmap = 0;
        }
        break;
    }
    return old;
}

static void delete_object(struct w16 *w, uint16_t h) {
    int t = type_of(w, h);
    if (t == GDI_FREE || h < GDI_BASE) return;   // stock objects are never deleted
    struct w16_gdiobj *o = &w->gdi[(h - GDI_BASE) / 4u];
    if (t == GDI_BITMAP) {
        if (o->in_dc) return;                    // still selected: Windows refuses
        free(o->px);
    }
    if (t == GDI_FONT) free(o->lf);
    if (t == GDI_BRUSH || t == GDI_PALETTE) free(o->px);
    if (t == GDI_DC) {
        if (o->hwnd || o->screen) return;
        struct w16_gdiobj *bm = obj(w, o->bitmap, GDI_BITMAP);
        if (bm) bm->in_dc = 0;
        free(o->saved);
    }
    memset(o, 0, sizeof *o);
}

const uint32_t *w16_gdi_bitmap_pixels(struct w16 *w, uint16_t h, int *bw, int *bh) {
    struct w16_gdiobj *bm = obj(w, h, GDI_BITMAP);
    if (!bm) return 0;
    *bw = bm->bw;
    *bh = bm->bh;
    return bm->px;
}

uint16_t w16_gdi_bitmap_from(struct w16 *w, const uint32_t *px, int bw, int bh) {
    uint16_t h;
    struct w16_gdiobj *bm = new_bitmap(w, bw, bh, 0, &h);
    if (bm) memcpy(bm->px, px, (size_t)bw * bh * 4);
    return h;
}

void w16_gdi_delete(struct w16 *w, uint16_t h) { delete_object(w, h); }

int w16_gdi_scroll(struct w16 *w, uint16_t hdc, int dx, int dy, const int16_t *scroll, const int16_t *clip,
                   int16_t upd[4]) {
    struct gdi_target t;
    struct w16_gdiobj *dc = target(w, hdc, &t);
    memset(upd, 0, 8);
    if (!dc || !t.px) { gdi_done(w, dc); return dc != 0; }
    // In device pixels: what moves (the scroll rectangle, else everything)
    // and where it may land (the clip, else the scroll rectangle).
    int l = t.x0 - t.ox, tp = t.y0 - t.oy, r = t.x1 - t.ox, b = t.y1 - t.oy;
    if (scroll) { l = scroll[0]; tp = scroll[1]; r = scroll[2]; b = scroll[3]; dev_rect(dc, &l, &tp, &r, &b); }
    int cl = l, ct = tp, cr = r, cb = b;
    if (clip) { cl = clip[0]; ct = clip[1]; cr = clip[2]; cb = clip[3]; dev_rect(dc, &cl, &ct, &cr, &cb); }
    dx = dev_w(dc, dx);
    dy = scale(dy, dc->vext[1], dc->wext[1]);
    // the destination: the source moved, inside the clip and what the DC may draw on
    int x0 = l + dx, y0 = tp + dy, x1 = r + dx, y1 = b + dy;
    if (x0 < cl) x0 = cl;
    if (y0 < ct) y0 = ct;
    if (x1 > cr) x1 = cr;
    if (y1 > cb) y1 = cb;
    if (x0 < t.x0 - t.ox) x0 = t.x0 - t.ox;
    if (y0 < t.y0 - t.oy) y0 = t.y0 - t.oy;
    if (x1 > t.x1 - t.ox) x1 = t.x1 - t.ox;
    if (y1 > t.y1 - t.oy) y1 = t.y1 - t.oy;
    if (x1 > x0 && y1 > y0) {
        int wdt = x1 - x0, hgt = y1 - y0;
        for (int k = 0; k < hgt; k++) {
            int row = dy > 0 ? hgt - 1 - k : k;    // overlapping: copy away from the move
            uint32_t *d = t.px + (size_t)(y0 + row + t.oy) * t.w + x0 + t.ox;
            const uint32_t *sp = t.px + (size_t)(y0 + row - dy + t.oy) * t.w + x0 - dx + t.ox;
            memmove(d, sp, (size_t)wdt * 4);
        }
    }
    // What was uncovered: the strip the move left behind, inside the clip.
    int ul = cl > l ? cl : l, ut = ct > tp ? ct : tp, ur = cr < r ? cr : r, ub = cb < b ? cb : b;
    if (dy > 0) ub = ut + dy; else if (dy < 0) ut = ub + dy;
    if (dx > 0) ur = ul + dx; else if (dx < 0) ul = ur + dx;
    if (!dx && !dy) ur = ul;
    if (ur > ul && ub > ut) {
        int16_t u[4] = { (int16_t)log_x(dc, ul), (int16_t)log_y(dc, ut), (int16_t)log_x(dc, ur), (int16_t)log_y(dc, ub) };
        memcpy(upd, u, 8);
    }
    gdi_done(w, dc);
    return 1;
}

void w16_gdi_region_set(struct w16 *w, uint16_t hrgn, const int16_t rc[4]) {
    struct w16_gdiobj *r = obj(w, hrgn, GDI_REGION);
    if (r) rgn_set(r, rc[0], rc[1], rc[2], rc[3]);
}

static uint16_t device_caps(uint16_t index) {
    switch (index) {
    case 2:  return 1;                           // TECHNOLOGY: DT_RASDISPLAY
    case 4:  return 208;                         // HORZSIZE, mm
    case 6:  return 156;                         // VERTSIZE
    case 8:  return 640;                         // HORZRES
    case 10: return 480;                         // VERTRES
    case 12: return 8;                           // BITSPIXEL
    case 14: return 1;                           // PLANES
    case 16: return 0xFFFF;                      // NUMBRUSHES
    case 18: return 0xFFFF;                      // NUMPENS
    case 22: return 0;                           // NUMFONTS
    case 24: return 20;                          // NUMCOLORS: a 256-colour VGA's static ones
    case 38: return 0x0699;                      // RASTERCAPS: bitblt, bitmap64, DIBs, stretch
    case 40: return 36;                          // ASPECTX
    case 42: return 36;                          // ASPECTY
    case 44: return 51;                          // ASPECTXY
    case 88: return 96;                          // LOGPIXELSX
    case 90: return 96;                          // LOGPIXELSY
    case 104: return 256;                        // SIZEPALETTE
    default: return 0;
    }
}

// --- the dispatcher ----------------------------------------------------------------

static void set32(struct cpu16 *c, uint32_t v) {
    c->r[R_AX] = (uint16_t)v;
    c->r[R_DX] = (uint16_t)(v >> 16);
}

// SetBkColor(hdc, crColor) -> COLORREF
// SetTextColor(hdc, crColor) -> COLORREF
// Windows: The colour behind text, hatches and dashed lines, and the colour of text; each returns
// the old one. On a 16-colour VGA a colour that is no solid one draws as the nearest.
// w16: The same; a colour is shown as Windows 3.11's 256-colour VGA driver showed it -- its 20
// static colours -- so a program looks as it did.
static int api_SetBkColor(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    struct w16_gdiobj *o;
    o = obj(w, w16_arg16(a), GDI_DC);
    uint32_t v = w16_arg32(a), old = 0;
    if (o) {
        uint32_t *f = ordinal == 1 ? &o->bk : &o->text;
        old = *f;
        *f = v & 0xFFFFFF;
    }
    set32(c, old);
    return 6;
}

// SetBkMode(hdc, iBkMode) -> int
// SetROP2(hdc, fnDrawMode) -> int
// Windows: Whether text fills its background (OPAQUE) or not (TRANSPARENT); and how a pen's colour
// combines with what is already there -- copy, invert, XOR... the 16 binary raster operations. Each
// returns the old mode.
// w16: The same; all 16 raster operations.
static int api_SetBkMode(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    struct w16_gdiobj *o;
    o = obj(w, w16_arg16(a), GDI_DC);
    uint16_t v = w16_arg16(a);
    int *f = o ? (ordinal == 2 ? &o->bk_mode : &o->rop2) : 0;
    c->r[R_AX] = f ? (uint16_t)*f : 0;
    if (f && v) *f = v;
    return 4;
}

// LineTo(hdc, nXEnd, nYEnd) -> BOOL
// Windows: Draws a line in the current pen from the current position to a point, which becomes the
// position; the last pixel is left out.
// w16: The same, the last pixel left out too. A dashed or dotted pen draws solid.
static int api_LineTo(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    struct w16_gdiobj *o;
    (void)ordinal;
    uint16_t hdc = w16_arg16(a);
    int16_t x = (int16_t)w16_arg16(a), y = (int16_t)w16_arg16(a);
    struct gdi_target t;
    o = target(w, hdc, &t);
    if (o) {
        int width;
        int64_t pen = pen_of(w, o->pen, &width);
        if (width > 1) width = abs(dev_w(o, width));
        if (pen >= 0) gdi_line(&t, dev_x(o, o->cx), dev_y(o, o->cy), dev_x(o, x), dev_y(o, y), (uint32_t)pen,
                               width ? width : 1, o->rop2);
        o->cx = x;
        o->cy = y;
    }
    gdi_done(w, o);
    c->r[R_AX] = o != 0;
    return 6;
}

// MoveTo(hdc, X, Y) -> DWORD
// Windows: Moves the current position; returns the old one.
// w16: The same.
static int api_MoveTo(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    struct w16_gdiobj *o;
    (void)ordinal;
    o = obj(w, w16_arg16(a), GDI_DC);
    int16_t x = (int16_t)w16_arg16(a), y = (int16_t)w16_arg16(a);
    set32(c, o ? (uint32_t)(uint16_t)o->cy << 16 | (uint16_t)o->cx : 0);
    if (o) { o->cx = x; o->cy = y; }
    return 6;
}

// Rectangle(hdc, nLeftRect, nTopRect, nRightRect, nBottomRect) -> BOOL
// Windows: Draws a rectangle: edged in the pen, filled with the brush -- with a NULL pen, filled up
// to its right and bottom edges less one.
// w16: The same.
static int api_Rectangle(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return rectangle(w, a);
}

// PatBlt(hdc, nXLeft, nYLeft, nWidth, nHeight, dwRop) -> BOOL
// Windows: Fills a rectangle with the brush, combined with what is there by a raster operation
// (PATCOPY, PATINVERT, DSTINVERT, BLACKNESS...).
// w16: The same; any of the 256 raster operations, hatched and pattern brushes included, their
// pattern aligned to the screen as Windows' was.
static int api_PatBlt(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return pat_blt(w, a);
}

// SetPixel(hdc, X, Y, crColor) -> COLORREF
// GetPixel(hdc, X, Y) -> COLORREF
// Windows: Set or read one pixel; SetPixel returns the colour actually set.
// w16: The same, as the palette colour: a pixel holds the shade 3.11's VGA driver showed (its light
// grey is 0xC0C7C8), and the colour a program asked for is what it reads back -- 0x808080 for dark grey.
static int api_SetPixel(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    struct w16_gdiobj *o;
    uint16_t hdc = w16_arg16(a);
    int16_t x = (int16_t)w16_arg16(a), y = (int16_t)w16_arg16(a);
    uint32_t cr = ordinal == 31 ? w16_arg32(a) : 0;
    struct gdi_target t;
    o = target(w, hdc, &t);
    uint32_t out = 0xFFFFFFFFu;
    int px = (o ? dev_x(o, x) : x) + t.ox, py = (o ? dev_y(o, y) : y) + t.oy;
    if (o && t.px && px >= t.x0 && py >= t.y0 && px < t.x1 && py < t.y1) {
        uint32_t *p = t.px + (size_t)py * t.w + px;
        if (ordinal == 31) *p = t.mono ? (w16_colorref_px(cr) ? 0xFFFFFF : 0) : w16_colorref_px(cr);
        uint32_t v = w16_vga_back(*p);           // the colour a program asked for, not its shade on screen
        out = (v & 0xFF) << 16 | (v & 0xFF00) | (v >> 16 & 0xFF);      // back to a COLORREF
    }
    if (ordinal == 83) gdi_read_done(w, o);
    else gdi_done(w, o);
    set32(c, out);
    return ordinal == 31 ? 10 : 6;
}

// TextOut(hdc, nXStart, nYStart, lpString, cbString) -> BOOL
// Windows: Draws a string in the DC's font and text colour, its background filled when the mode is
// OPAQUE.
// w16: The same, in Windows 3.11's own bitmap fonts from drive C:'s WINDOWS\SYSTEM when they are
// there (and the Fonts setting allows), else in toy-os's stand-ins; an OPAQUE background fills the
// whole cell box first, as Windows did.
static int api_TextOut(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return text_out(w, a);
}

// BitBlt(hdcDest, nXDest, nYDest, nWidth, nHeight, hdcSrc, nXSrc, nYSrc, dwRop) -> BOOL
// Windows: Copies a rectangle of pixels from one DC to another -- a bitmap to the window, the
// window to a bitmap -- combined with what is there and the brush by a raster operation; a
// monochrome source takes the text and background colours.
// w16: The same: any of the 256 raster operations, monochrome to colour and back as Windows
// converted them.
static int api_BitBlt(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return bit_blt(w, a);
}

// SelectObject(hdc, hgdiobj) -> HGDIOBJ
// Windows: Puts a pen, brush, font, bitmap or region into a DC, returning the one it replaces; a
// bitmap selected into a memory DC is what that DC draws on.
// w16: The same.
static int api_SelectObject(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint16_t hdc = w16_arg16(a);
    c->r[R_AX] = select_object(w, hdc, w16_arg16(a));
    return 4;
}

// CreateBitmap(nWidth, nHeight, cPlanes, cBitsPerPel, lpvBits) -> HBITMAP
// Windows: Makes a bitmap -- monochrome, or in the display's colours -- optionally from its bits in
// the device's own layout.
// w16: The same; a monochrome bitmap's bits are read, a colour one's are not (it starts black) --
// programs fill those by drawing or with SetDIBits.
static int api_CreateBitmap(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    uint16_t h;
    (void)ordinal;
    int16_t bw = (int16_t)w16_arg16(a), bh = (int16_t)w16_arg16(a);
    uint16_t planes = w16_arg16(a), bpp = w16_arg16(a);
    uint32_t bits = w16_arg32(a);
    int mono = planes * bpp == 1;
    struct w16_gdiobj *bm = new_bitmap(w, bw, bh, mono, &h);
    if (bm && mono && (bits >> 16)) {        // device-dependent 1-bit rows: 2-byte aligned, top-down
        unsigned stride = ((unsigned)bw + 15) / 16 * 2;
        const uint8_t *p = cpu16_ptr(c, (uint16_t)(bits >> 16), (uint16_t)bits, stride * (unsigned)bh, 0);
        if (!p) return -1;
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++)
                bm->px[(size_t)y * bw + x] = p[y * stride + x / 8] >> (7 - x % 8) & 1 ? 0xFFFFFF : 0;
    }
    c->r[R_AX] = h;
    return 12;
}

// CreateCompatibleBitmap(hdc, nWidth, nHeight) -> HBITMAP
// Windows: Makes a bitmap in the colours of a DC's device -- what a program draws off-screen in,
// then copies to its window.
// w16: The same.
static int api_CreateCompatibleBitmap(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    struct w16_gdiobj *o;
    uint16_t h;
    (void)ordinal;
    uint16_t hdc = w16_arg16(a);
    int16_t bw = (int16_t)w16_arg16(a), bh = (int16_t)w16_arg16(a);
    o = obj(w, hdc, GDI_DC);
    struct w16_gdiobj *src = o ? obj(w, o->bitmap, GDI_BITMAP) : 0;
    int mono = o && !o->hwnd && !o->screen && (!src || src->mono);   // a memory DC's own bitmap decides
    new_bitmap(w, bw, bh, mono, &h);
    c->r[R_AX] = h;
    return 6;
}

// CreateCompatibleDC(hdc) -> HDC
// Windows: A memory DC like a device's: it draws on whatever bitmap is selected into it -- a 1 x 1
// monochrome one at first.
// w16: The same.
static int api_CreateCompatibleDC(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    struct w16_gdiobj *o;
    uint16_t h;
    (void)ordinal;
    w16_arg16(a);
    h = new_obj(w, GDI_DC, &o);
    if (o) dc_defaults(o);
    c->r[R_AX] = h;
    return 2;
}

// CreatePen(fnPenStyle, nWidth, crColor) -> HPEN
// Windows: A pen: solid, dashed, dotted or none, its width and colour.
// w16: The same, but dashed and dotted pens draw solid; no program run so far draws with one.
static int api_CreatePen(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    struct w16_gdiobj *o;
    uint16_t h;
    (void)ordinal;
    uint16_t style = w16_arg16(a), width = w16_arg16(a);
    uint32_t cr = w16_arg32(a);
    h = new_obj(w, GDI_PEN, &o);
    if (o) { o->style = style; o->width = width; o->color = cr & 0xFFFFFF; }
    c->r[R_AX] = h;
    return 8;
}

// CreateSolidBrush(crColor) -> HBRUSH
// Windows: A brush of one colour -- dithered on a 16-colour screen when it is no solid colour.
// w16: The same, shown as the 256-colour VGA driver showed it.
static int api_CreateSolidBrush(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    struct w16_gdiobj *o;
    uint16_t h;
    (void)ordinal;
    uint32_t cr = w16_arg32(a);
    h = new_obj(w, GDI_BRUSH, &o);
    if (o) o->color = cr & 0xFFFFFF;
    c->r[R_AX] = h;
    return 4;
}

// GetTextExtent(hdc, lpString, cbString) -> DWORD
// Windows: How wide and high a string is in the DC's font, as TextOut would draw it -- its spaces
// widened by SetTextJustification -- in logical units.
// w16: The same, from the same font TextOut draws with, so text measures as it draws.
static int api_GetTextExtent(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_gdiobj *dc = obj(w, w16_arg16(a), GDI_DC);
    uint32_t far = w16_arg32(a);
    unsigned len = w16_arg16(a);
    char *text = far_text(w, far, len);
    if (!text) return -1;
    pthread_mutex_lock(&g_w16_text_lock);
    const struct ugfx_font *was = ugfx_set_font(w16_font_of(dc ? dc->font : 0));
    int cx = text_runs(dc, 0, INT_MIN, 0, text, 0, 0), cy = ugfx_char_h();
    ugfx_set_font(was);
    pthread_mutex_unlock(&g_w16_text_lock);
    cx = w16_font_scaled(dc ? dc->font : 0, cx);
    cy = w16_font_scaled(dc ? dc->font : 0, cy);
    if (dc) { cx = scale(cx, dc->wext[0], dc->vext[0]); cy = scale(cy, dc->wext[1], dc->vext[1]); }
    if (cx < 0) cx = -cx;
    if (cy < 0) cy = -cy;
    free(text);
    set32(&w->cpu, (uint32_t)(uint16_t)cy << 16 | (uint16_t)cx);
    return 8;
}

// A TEXTMETRIC's 31 bytes for a font.
static void text_metric(const struct w16_fontinfo *fi, uint8_t tm[31]) {
    struct w16_fontinfo f = *fi;
    int16_t words[8] = {
        (int16_t)f.height, (int16_t)f.ascent, (int16_t)(f.height - f.ascent),
        (int16_t)f.internal_leading, (int16_t)f.external_leading,
        (int16_t)f.ave, (int16_t)f.maxw, (int16_t)f.weight,
    };
    memset(tm, 0, 31);
    memcpy(tm, words, sizeof words);
    tm[16] = (uint8_t)f.italic;
    tm[19] = (uint8_t)f.first;                   // tmFirstChar
    tm[20] = (uint8_t)f.last;                    // tmLastChar
    tm[21] = (uint8_t)f.def;                     // tmDefaultChar
    tm[22] = 0x20;                               // tmBreakChar
    tm[23] = (uint8_t)f.pitch_family;            // bit 0 set: VARIABLE pitch, as in the FNT
    tm[24] = (uint8_t)f.charset;
    tm[27] = tm[29] = 96;                        // tmDigitizedAspectX, Y
}

// GetTextMetrics(hdc, lptm) -> BOOL
// Windows: The DC's font's measures: height, ascent, descent, leading, average and widest
// character, weight, overhang, pitch and family, character set.
// w16: The same, from the font itself -- Windows 3.11's .FON header when it is that font, a
// TrueType face's own device metrics when it has them -- so a program lays out as it did; an
// underline or strike-out asked for is reported as made.
static int api_GetTextMetrics(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t hfont = dc_font(w, w16_arg16(a));
    uint32_t far = w16_arg32(a);
    uint8_t tm[31];
    pthread_mutex_lock(&g_w16_text_lock);
    text_metric(w16_font_info(hfont), tm);
    pthread_mutex_unlock(&g_w16_text_lock);
    const struct w16_logfont *lf = w16_gdi_logfont(w, hfont);
    if (lf) {                                    // GDI makes these for any font asked them
        tm[16] |= lf->italic ? 1 : 0;
        tm[17] = lf->underline ? 1 : 0;
        tm[18] = lf->strikeout ? 1 : 0;
    }
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, sizeof tm, 1);
    if (!p) return -1;
    memcpy(p, tm, sizeof tm);
    w->cpu.r[R_AX] = 1;
    return 6;
}

const struct w16_logfont *w16_gdi_logfont(struct w16 *w, uint16_t hfont) {
    struct w16_gdiobj *o = obj(w, hfont, GDI_FONT);
    return o ? o->lf : 0;
}

static uint16_t new_font(struct w16 *w, const struct w16_logfont *lf) {
    struct w16_gdiobj *o;
    struct w16_logfont *copy = malloc(sizeof *copy);
    if (!copy) return 0;
    *copy = *lf;
    copy->face[sizeof copy->face - 1] = 0;
    uint16_t h = new_obj(w, GDI_FONT, &o);
    if (!o) { free(copy); return 0; }
    o->lf = copy;
    if (g_w16_trace) {
        fprintf(g_w16_trace, "  font %04x: \"%s\" height %d width %d weight %d%s%s pitch&family %02x\n", h, copy->face,
                copy->height, copy->width, copy->weight, copy->italic ? " italic" : "", copy->underline ? " underline" : "",
                copy->pitch_family);
        fflush(g_w16_trace);
    }
    return h;
}

uint16_t w16_gdi_font_new(struct w16 *w, const struct w16_logfont *lf) { return new_font(w, lf); }

// CreateFont(nHeight, nWidth, nEscapement, nOrientation, fnWeight, fdwItalic, fdwUnderline, fdwStrikeOut, fdwCharSet, fdwOutputPrecision, fdwClipPrecision, fdwQuality, fdwPitchAndFamily, lpszFace) -> HFONT
// Windows: A font as near as Windows can find to what is asked: a face, a height, a weight, italic,
// underline...; Windows picks among the fonts installed.
// w16: The same choice among Windows 3.11's bitmap fonts on drive C: (MS Sans Serif, Courier, MS
// Serif, Small Fonts...) by face and the nearest size; without them, toy-os's stand-ins. Bold is
// synthesized as Windows did, with its overhang. Escapement (rotated text) is not drawn.
static int api_CreateFont(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_logfont lf = { 0 };
    lf.height = (int16_t)w16_arg16(a);
    lf.width = (int16_t)w16_arg16(a);
    w16_arg16(a); w16_arg16(a);                  // escapement, orientation: raster fonts do not turn
    lf.weight = (int16_t)w16_arg16(a);
    lf.italic = (uint8_t)w16_arg16(a);
    lf.underline = (uint8_t)w16_arg16(a);
    lf.strikeout = (uint8_t)w16_arg16(a);
    lf.charset = (uint8_t)w16_arg16(a);
    w16_arg16(a); w16_arg16(a); w16_arg16(a);    // precisions and quality
    lf.pitch_family = (uint8_t)w16_arg16(a);
    uint32_t face = w16_arg32(a);
    if ((face >> 16) && cpu16_str(&w->cpu, (uint16_t)(face >> 16), (uint16_t)face, lf.face, sizeof lf.face) < 0)
        lf.face[0] = 0;
    w->cpu.r[R_AX] = new_font(w, &lf);
    return 30;
}

// CreateFontIndirect(lplf) -> HFONT
// Windows: CreateFont from a LOGFONT structure.
// w16: The same.
static int api_CreateFontIndirect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t far = w16_arg32(a);
    const uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, 18, 0);
    if (!p) return -1;
    struct w16_logfont lf = { 0 };
    lf.height = (int16_t)(p[0] | p[1] << 8);
    lf.width = (int16_t)(p[2] | p[3] << 8);
    lf.weight = (int16_t)(p[8] | p[9] << 8);
    lf.italic = p[10];
    lf.underline = p[11];
    lf.strikeout = p[12];
    lf.charset = p[13];
    lf.pitch_family = p[17];
    if (cpu16_str(&w->cpu, (uint16_t)(far >> 16), (uint16_t)(far + 18), lf.face, sizeof lf.face) < 0) lf.face[0] = 0;
    w->cpu.r[R_AX] = new_font(w, &lf);
    return 4;
}

// A LOGFONT's 50 bytes.
static void log_font_bytes(const struct w16_logfont *lf, uint8_t out[50]) {
    memset(out, 0, 50);
    memcpy(out, &lf->height, 2);
    memcpy(out + 2, &lf->width, 2);
    memcpy(out + 8, &lf->weight, 2);
    out[10] = lf->italic;
    out[11] = lf->underline;
    out[12] = lf->strikeout;
    out[13] = lf->charset;
    out[17] = lf->pitch_family;
    for (int i = 0; i < 31 && lf->face[i]; i++) out[18 + i] = (uint8_t)lf->face[i];
}

struct enum_font { uint8_t lf[50], tm[31]; int type; };
struct enum_fonts { struct enum_font *v; int n, cap; };

static int collect_font(const struct w16_logfont *lf, const struct w16_fontinfo *fi, int type, void *ctx) {
    struct enum_fonts *e = ctx;
    if (e->n == e->cap) {
        int cap = e->cap ? 2 * e->cap : 16;
        struct enum_font *g = realloc(e->v, (size_t)cap * sizeof *g);
        if (!g) return 0;
        e->v = g;
        e->cap = cap;
    }
    log_font_bytes(lf, e->v[e->n].lf);
    text_metric(fi, e->v[e->n].tm);
    e->v[e->n++].type = type;
    return 1;
}

// One LOGFONT and TEXTMETRIC per font, or for EnumFontFamilies an ENUMLOGFONT (the LOGFONT, the full
// name, the style) and a NEWTEXTMETRIC (the TEXTMETRIC, then the TrueType flags and sizes).
static int enum_fonts(struct w16 *w, struct w16_args *a, int families) {
    w16_arg16(a);                                // the DC: every one is the display's
    uint32_t face = w16_arg32(a), fn = w16_arg32(a), data = w16_arg32(a);
    char name[32] = "";
    if ((face >> 16) && cpu16_str(&w->cpu, (uint16_t)(face >> 16), (uint16_t)face, name, sizeof name) < 0)
        return -1;
    struct enum_fonts e = { 0 };
    // Collected first: the program's function draws and measures, which takes the text lock.
    pthread_mutex_lock(&g_w16_text_lock);
    w16_font_enum(face >> 16 ? name : 0, collect_font, &e);
    pthread_mutex_unlock(&g_w16_text_lock);
    unsigned lf_n = families ? 50 + 64 + 32 : 50, tm_n = families ? 31 + 10 : 31;
    uint16_t last = 0;
    for (int i = 0; i < e.n && !w->cpu.fault; i++) {
        const char *f = (const char *)e.v[i].lf + 18;
        int dup = 0;
        for (int k = 0; families && !(face >> 16) && k < i; k++)    // a family once, at its first size
            dup |= !strcmp((const char *)e.v[k].lf + 18, f);
        if (dup) continue;
        uint16_t at = w16_sys_push(w, lf_n + tm_n);
        if (!at) break;
        uint8_t *p = w->sysmem + at;
        memset(p, 0, lf_n + tm_n);
        memcpy(p, e.v[i].lf, 50);
        memcpy(p + lf_n, e.v[i].tm, 31);
        if (families) {
            int16_t weight;
            memcpy(&weight, e.v[i].lf + 8, 2);
            int bold = weight >= 700, italic = e.v[i].lf[10] != 0;
            const char *style = bold && italic ? "Bold Italic" : bold ? "Bold" : italic ? "Italic" : "Regular";
            int tt = e.v[i].type == W16_TRUETYPE_FONTTYPE;
            if (tt && (bold || italic)) snprintf((char *)p + 50, 64, "%s %s", f, style);
            else snprintf((char *)p + 50, 64, "%s", f);
            if (tt) snprintf((char *)p + 114, 32, "%s", style);
            uint8_t *n = p + lf_n + 31;
            uint32_t flags = (italic ? 0x01 : 0) | (bold ? 0x20 : 0) | (!bold && !italic ? 0x40 : 0);
            memcpy(n, &flags, 4);
            uint16_t em = tt ? 2048 : 0;
            memcpy(n + 4, &em, 2);
            memcpy(n + 6, e.v[i].tm, 2);         // the cell's height: tmHeight
            memcpy(n + 8, e.v[i].tm + 10, 2);    // tmAveCharWidth
        }
        const uint16_t words[] = { w->sysdata, at, w->sysdata, (uint16_t)(at + lf_n), (uint16_t)e.v[i].type,
                                   (uint16_t)(data >> 16), (uint16_t)data };
        last = (uint16_t)w16_call_far(w, (uint16_t)(fn >> 16), (uint16_t)fn, words, 7);
        w16_sys_pop(w, at);
        if (!last) break;
    }
    free(e.v);
    w->cpu.r[R_AX] = last;
    return w->cpu.fault ? -1 : 14;
}

// EnumFonts(hdc, lpFaceName, lpFontFunc, lpData) -> int
// Windows: Calls a function of the program's for each font the device has -- one per face when no
// face is named, else each size of that face -- with its LOGFONT, its TEXTMETRIC and whether it is a
// raster, device or TrueType font; stops when the function returns 0, and returns what it last did.
// w16: The same, for the fonts w16 draws in: Windows 3.11's bitmap fonts from drive C:'s
// WINDOWS\SYSTEM, each face at the sizes its .FON files hold; without them, toy-os's stand-ins
// under 3.11's names (MS Sans Serif, MS Serif, Courier) at 3.11's sizes.
static int api_EnumFonts(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return enum_fonts(w, a, 0);
}

// EnumFontFamilies(hdc, lpszFamily, lpEnumFontFamProc, lParam) -> int
// Windows: EnumFonts for 3.1's TrueType: one call per family when none is named, else each of the
// family's fonts, with an ENUMLOGFONT (adding the full name and the style, "Bold Italic") and a
// NEWTEXTMETRIC (adding whether it is bold or italic, its em square and its cell); stops at a 0.
// w16: The same over the fonts EnumFonts reports; a TrueType family is reported in its regular
// style only.
static int api_EnumFontFamilies(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return enum_fonts(w, a, 1);
}

// GetTextFace(hdc, nCount, lpFaceName) -> int
// Windows: The face name of the font a DC draws in -- the one GDI chose, which may differ from the
// one asked for -- cut to the buffer; the bytes copied.
// w16: The same: the face drawn, 3.11's own or the one a stand-in takes the place of.
static int api_GetTextFace(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t hfont = dc_font(w, w16_arg16(a));
    int16_t cap = (int16_t)w16_arg16(a);
    uint32_t far = w16_arg32(a);
    char face[32];
    pthread_mutex_lock(&g_w16_text_lock);
    const char *f = w16_font_info(hfont)->face;
    snprintf(face, sizeof face, "%s", f ? f : "System");
    pthread_mutex_unlock(&g_w16_text_lock);
    w->cpu.r[R_AX] = 0;
    if (cap <= 0) return 8;
    size_t n = strlen(face);
    if (n > (size_t)cap - 1) n = (size_t)cap - 1;
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, (uint32_t)n + 1, 1);
    if (!p) return -1;
    memcpy(p, face, n);
    p[n] = 0;
    w->cpu.r[R_AX] = (uint16_t)n;
    return 8;
}

// GetCharWidth(hdc, uFirstChar, uLastChar, lpBuffer) -> BOOL
// Windows: Each character's advance in a range, in the DC's font, into an array of ints.
// w16: The same, from the glyphs TextOut draws, so a program that lays out a line from the widths
// lands where TextOut draws.
static int api_GetCharWidth(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t hfont = dc_font(w, w16_arg16(a));
    uint16_t first = w16_arg16(a), last = w16_arg16(a);
    uint32_t far = w16_arg32(a);
    w->cpu.r[R_AX] = 0;
    if (last < first || last > 255) return 10;
    unsigned n = last - first + 1u;
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, n * 2, 1);
    if (!p) return -1;
    pthread_mutex_lock(&g_w16_text_lock);
    const struct ugfx_font *was = ugfx_set_font(w16_font_of(hfont));
    for (unsigned i = 0; i < n; i++) {
        int adv = w16_font_scaled(hfont, ugfx_char_advance((char)(first + i)));
        p[2 * i] = (uint8_t)adv;
        p[2 * i + 1] = (uint8_t)(adv >> 8);
    }
    ugfx_set_font(was);
    pthread_mutex_unlock(&g_w16_text_lock);
    w->cpu.r[R_AX] = 1;
    return 10;
}

// MulDiv(nNumber, nNumerator, nDenominator) -> int
// Windows: nNumber * nNumerator / nDenominator with a 32-bit product, rounded to the nearest; -32768
// when the denominator is 0 or the answer does not fit 16 bits.
// w16: The same.
static int api_MulDiv(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    int32_t n = (int16_t)w16_arg16(a), num = (int16_t)w16_arg16(a), den = (int16_t)w16_arg16(a);
    int32_t r = -32768;
    if (den) {
        int64_t p = (int64_t)n * num, ap = p < 0 ? -p : p, ad = den < 0 ? -den : den;
        int64_t q = (ap + ad / 2) / ad;
        q = (p < 0) != (den < 0) ? -q : q;
        r = q < -32768 || q > 32767 ? -32768 : (int32_t)q;
    }
    w->cpu.r[R_AX] = (uint16_t)r;
    return 6;
}

// Only a printer asks this way, and there is no printer -- the program is told so as Windows did.
// CreateDC(lpszDriver, lpszDevice, lpszOutput, lpInitData) -> HDC
// Windows: Makes a device context for a named driver and device -- in practice a printer's.
// w16: Answers 0: there is no such device. The screen's device contexts come from GetDC,
// CreateCompatibleDC and BeginPaint, which work in full.
// Why: There is no printer (PrintDlg says why).
// Notice: Nothing beyond printing not working.
// Whole: Printing. On the roadmap: "w16: printing".
static int api_CreateDC(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg32(a); w16_arg32(a); w16_arg32(a); w16_arg32(a);
    w->cpu.r[R_AX] = 0;
    return 16;
}

// Escape(hdc, nEscape, cbInput, lpvInData, lpvOutData) -> int
// Windows: Passes a device-specific request to a device's driver -- in 3.x mostly a printer's:
// start a document, start or end a page, ask the paper size.
// w16: Answers 0: the request is not supported.
// Why: There is no printer (PrintDlg says why), and a screen device context takes none of these
// requests.
// Notice: Nothing beyond printing not working.
// Whole: Printing. On the roadmap: "w16: printing".
static int api_Escape(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a); w16_arg16(a); w16_arg16(a); w16_arg32(a); w16_arg32(a);
    w->cpu.r[R_AX] = 0;
    return 14;
}

// DeleteDC(hdc) -> BOOL
// DeleteObject(hObject) -> BOOL
// Windows: Free a DC, or a pen, brush, font, bitmap or region.
// w16: The same; a stock object is never freed.
static int api_DeleteDC(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    delete_object(w, w16_arg16(a));
    c->r[R_AX] = 1;
    return 2;
}

// GetDeviceCaps(hdc, nIndex) -> int
// Windows: What the device can do: its resolution, colours, bits per pixel, raster abilities,
// pixels per inch...
// w16: A 640 x 480 VGA with 256 colours, 20 of them static, at 96 pixels per inch -- what Windows
// 3.11's VGA driver said; 0 for an index it does not know.
static int api_GetDeviceCaps(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    c->r[R_AX] = device_caps(w16_arg16(a));
    return 4;
}

// GetStockObject(fnObject) -> HGDIOBJ
// Windows: One of Windows' ready-made pens, brushes and fonts: white, grey and black brushes, the
// null pen and brush, the System font, ANSI_FIXED_FONT...
// w16: The same seventeen.
static int api_GetStockObject(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    c->r[R_AX] = (uint16_t)(STOCK_BASE + w16_arg16(a));
    return 2;
}

// StretchBlt(hdcDest, nXOriginDest, nYOriginDest, nWidthDest, nHeightDest, hdcSrc, nXOriginSrc, nYOriginSrc, nWidthSrc, nHeightSrc, dwRop) -> BOOL
// Windows: BitBlt to a rectangle of another size, stretching or shrinking the pixels -- a mirror
// image when a width or height is negative.
// w16: The same, mirrors included, the nearest pixel taken as 3.1's COLORONCOLOR mode did for
// colour.
static int api_StretchBlt(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return stretch_blt(w, a);
}

// GetDCOrg(hdc) -> DWORD
// Windows: Where a DC's origin is on the screen.
// w16: Where it is in the program's top-level window, which is the program's screen.
static int api_GetDCOrg(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_gdiobj *dc = obj(w, w16_arg16(a), GDI_DC);
    int dx = 0, dy = 0;
    if (dc && dc->hwnd) w16_screen_offset(w, w16_wnd_get(w, dc->hwnd), &dx, &dy);
    set32(&w->cpu, (uint32_t)(uint16_t)dy << 16 | (uint16_t)dx);
    return 2;
}

// GetObject(hgdiobj, cbBuffer, lpvObject) -> int
// Windows: The description of a GDI object: a bitmap's size and colours (BITMAP), a pen's (LOGPEN),
// a brush's (LOGBRUSH), a font's (LOGFONT); the bytes copied.
// w16: The same four, cut to the buffer's size.
static int api_GetObject(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    int16_t cb = (int16_t)w16_arg16(a);
    uint32_t far = w16_arg32(a);
    uint8_t buf[50] = { 0 };
    unsigned n = 0;
    int width;
    switch (type_of(w, h)) {
    case GDI_BITMAP: {
        struct w16_gdiobj *bm = obj(w, h, GDI_BITMAP);
        if (!bm) break;
        int bpp = bm->mono ? 1 : 8;              // GetDeviceCaps' BITSPIXEL
        int16_t v[4] = { 0, (int16_t)bm->bw, (int16_t)bm->bh, (int16_t)((bm->bw * bpp + 15) / 16 * 2) };
        memcpy(buf, v, 8);
        buf[8] = 1;                              // bmPlanes
        buf[9] = (uint8_t)bpp;
        n = 14;                                  // bmBits: NULL, a DDB's bits are GDI's
        break;
    }
    case GDI_PEN: {
        int64_t px = pen_of(w, h, &width);
        struct w16_gdiobj *p = obj(w, h, GDI_PEN);
        uint16_t style = p ? p->style : px < 0 ? PS_NULL : 0;
        uint32_t cr = p ? p->color : h == STOCK_BASE + WHITE_PEN ? 0xFFFFFF : 0;
        memcpy(buf, &style, 2);
        buf[2] = (uint8_t)width;
        memcpy(buf + 6, &cr, 4);
        n = 10;
        break;
    }
    case GDI_BRUSH: {
        struct w16_gdiobj *b = obj(w, h, GDI_BRUSH);
        uint16_t style = b ? b->style : h == STOCK_BASE + NULL_BRUSH ? BS_NULL : 0;
        uint32_t px = w16_brush_px(w, h), cr = (px & 0xFF) << 16 | (px & 0xFF00) | (px >> 16 & 0xFF);
        memcpy(buf, &style, 2);
        memcpy(buf + 2, &cr, 4);
        n = 8;
        break;
    }
    case GDI_FONT: {
        const struct w16_logfont *lf = w16_gdi_logfont(w, h);
        if (lf) {
            memcpy(buf, &lf->height, 2);
            memcpy(buf + 2, &lf->width, 2);
            memcpy(buf + 8, &lf->weight, 2);
            buf[10] = lf->italic;
            buf[11] = lf->underline;
            buf[12] = lf->strikeout;
            buf[13] = lf->charset;
            buf[17] = lf->pitch_family;
            memcpy(buf + 18, lf->face, sizeof lf->face - 1);   // NUL-padded by new_font
        } else {
            pthread_mutex_lock(&g_w16_text_lock);
            const struct w16_fontinfo *f = w16_font_info(h);
            int16_t ht = (int16_t)f->height, wd = (int16_t)f->ave, wt = (int16_t)f->weight;
            buf[17] = (uint8_t)f->pitch_family;
            buf[13] = (uint8_t)f->charset;
            pthread_mutex_unlock(&g_w16_text_lock);
            memcpy(buf, &ht, 2);
            memcpy(buf + 2, &wd, 2);
            memcpy(buf + 8, &wt, 2);
            memcpy(buf + 18, "System", 6);
        }
        n = 50;
        break;
    }
    default:
        break;
    }
    if (cb >= 0 && (unsigned)cb < n) n = (unsigned)cb;
    if (n) {
        uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, n, 1);
        if (!p) return -1;
        memcpy(p, buf, n);
    }
    w->cpu.r[R_AX] = (uint16_t)n;
    return 8;
}

// LineDDA(nXStart, nYStart, nXEnd, nYEnd, lpLineFunc, lpData) -> void
// Windows: Calls a function of the program's for each point of a line, as LineTo would draw it --
// for drawing a line in some way of its own.
// w16: The same points LineTo draws, the last left out as LineTo leaves it.
static int api_LineDDA(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    int x0 = (int16_t)w16_arg16(a), y0 = (int16_t)w16_arg16(a);
    int x1 = (int16_t)w16_arg16(a), y1 = (int16_t)w16_arg16(a);
    uint32_t fn = w16_arg32(a), data = w16_arg32(a);
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while ((x0 != x1 || y0 != y1) && !w->cpu.fault) {
        const uint16_t words[] = { (uint16_t)x0, (uint16_t)y0, (uint16_t)(data >> 16), (uint16_t)data };
        w16_call_far(w, (uint16_t)(fn >> 16), (uint16_t)fn, words, 4);
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    return w->cpu.fault ? -1 : 16;
}

// PtVisible(hdc, X, Y) -> BOOL
// Windows: Whether a point is inside what the DC may draw on -- its clipping region.
// w16: The same: inside what the DC draws on and its clip rectangle.
static int api_PtVisible(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t hdc = w16_arg16(a);
    int16_t x = (int16_t)w16_arg16(a), y = (int16_t)w16_arg16(a);
    struct gdi_target t;
    struct w16_gdiobj *dc = target(w, hdc, &t);
    int px = (dc ? dev_x(dc, x) : x) + t.ox, py = (dc ? dev_y(dc, y) : y) + t.oy;
    w->cpu.r[R_AX] = dc && t.px && px >= t.x0 && py >= t.y0 && px < t.x1 && py < t.y1;
    gdi_read_done(w, dc);                        // drew nothing: no repaint
    return 6;
}

// SetBrushOrg(hdc, X, Y) -> DWORD
// Windows: Where a hatch or pattern brush's 8 x 8 pattern starts; the old origin.
// w16: The same.
static int api_SetBrushOrg(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_gdiobj *dc = obj(w, w16_arg16(a), GDI_DC);
    int16_t x = (int16_t)w16_arg16(a), y = (int16_t)w16_arg16(a);
    set32(&w->cpu, dc ? (uint32_t)(uint16_t)dc->brush_oy << 16 | (uint16_t)dc->brush_ox : 0);
    if (dc) { dc->brush_ox = x; dc->brush_oy = y; }
    return 6;
}

// UnrealizeObject(hgdiobj) -> BOOL
// Windows: Makes a brush take its origin afresh the next time it is selected.
// w16: Answers TRUE: w16 takes a brush's origin afresh at every select anyway.
static int api_UnrealizeObject(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w->cpu.r[R_AX] = type_of(w, w16_arg16(a)) != GDI_FREE;
    return 2;
}

// CreateHatchBrush(fnStyle, clrref) -> HBRUSH
// Windows: A brush of lines -- horizontal, vertical, diagonal, crossed -- in a colour over the
// background.
// w16: The same six hatches.
static int api_CreateHatchBrush(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t hs = w16_arg16(a);
    uint32_t cr = w16_arg32(a);
    struct w16_gdiobj *o;
    uint16_t h = new_obj(w, GDI_BRUSH, &o);
    if (o) { o->style = BS_HATCHED; o->width = hs < 6 ? hs : 0; o->color = cr & 0xFFFFFF; }
    w->cpu.r[R_AX] = h;
    return 6;
}

// CreatePatternBrush(hbmp) -> HBRUSH
// Windows: A brush whose 8 x 8 pattern is a bitmap's top-left corner.
// w16: The same, the 8 x 8 corner copied when the brush is made.
static int api_CreatePatternBrush(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_gdiobj *bm = obj(w, w16_arg16(a), GDI_BITMAP), *o;
    w->cpu.r[R_AX] = 0;
    if (!bm) return 2;
    uint32_t *px = calloc(64, 4);
    if (!px) return 2;
    for (int i = 0; i < 64; i++)
        px[i] = bm->px[(size_t)(i / 8 % bm->bh) * bm->bw + (size_t)(i % 8 % bm->bw)];
    uint16_t h = new_obj(w, GDI_BRUSH, &o);
    if (!o) { free(px); return 2; }
    o->style = BS_PATTERN;
    o->px = px;
    o->mono = bm->mono;
    w->cpu.r[R_AX] = h;
    return 2;
}

// CreateIC(lpszDriver, lpszDevice, lpszOutput, lpdvmInit) -> HDC
// Windows: An information context: asks a device about itself -- GetDeviceCaps, text metrics --
// without drawing on it.
// w16: The display's: a DC that answers as the screen's does and draws nothing. A printer's is
// refused (0).
static int api_CreateIC(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t drv = w16_arg32(a);
    w16_arg32(a); w16_arg32(a); w16_arg32(a);
    char name[16] = "";
    if ((drv >> 16) && cpu16_str(&w->cpu, (uint16_t)(drv >> 16), (uint16_t)drv, name, sizeof name) < 0) name[0] = 0;
    for (char *p = name; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    struct w16_gdiobj *o = 0;
    uint16_t h = !strcmp(name, "DISPLAY") ? new_obj(w, GDI_DC, &o) : 0;
    if (o) { dc_defaults(o); o->screen = 1; }
    w->cpu.r[R_AX] = h;
    return 16;
}

// --- regions: each is its bounding rectangle --------------------------------------

#define NULLREGION    1
#define SIMPLEREGION  2
#define COMPLEXREGION 3

static int rgn_kind(const struct w16_gdiobj *r) {
    if (r->rgn[2] <= r->rgn[0] || r->rgn[3] <= r->rgn[1]) return NULLREGION;
    return r->complex ? COMPLEXREGION : SIMPLEREGION;
}

static void rgn_set(struct w16_gdiobj *r, int l, int t, int rr, int b) {
    if (rr <= l || b <= t) l = t = rr = b = 0;
    r->rgn[0] = (int16_t)l; r->rgn[1] = (int16_t)t; r->rgn[2] = (int16_t)rr; r->rgn[3] = (int16_t)b;
    r->complex = 0;
}

// CreateRectRgn(nLeftRect, nTopRect, nRightRect, nBottomRect) -> HRGN
// Windows: A rectangular region.
// w16: The same.
static int api_CreateRectRgn(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    int16_t l = (int16_t)w16_arg16(a), t = (int16_t)w16_arg16(a), r = (int16_t)w16_arg16(a), b = (int16_t)w16_arg16(a);
    struct w16_gdiobj *o;
    uint16_t h = new_obj(w, GDI_REGION, &o);
    if (o) rgn_set(o, l, t, r, b);
    w->cpu.r[R_AX] = h;
    return 8;
}

// SetRectRgn(hrgn, nLeftRect, nTopRect, nRightRect, nBottomRect) -> void
// Windows: Makes a region the rectangle given.
// w16: The same.
static int api_SetRectRgn(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_gdiobj *o = obj(w, w16_arg16(a), GDI_REGION);
    int16_t l = (int16_t)w16_arg16(a), t = (int16_t)w16_arg16(a), r = (int16_t)w16_arg16(a), b = (int16_t)w16_arg16(a);
    if (o) rgn_set(o, l, t, r, b);
    return 10;
}

// CombineRgn(hrgnDest, hrgnSrc1, hrgnSrc2, fnCombineMode) -> int
// Windows: Makes a region from two others -- their intersection, union, either but not both, or one
// less the other -- of any shape the rectangles make.
// w16: Exact while the answer is plainly a rectangle: the intersection of two rectangles, the union
// of two where one holds the other, a difference that takes all or none of the first away. Any
// other answer is kept as its bounding rectangle -- a difference keeps the first rectangle whole --
// and reported COMPLEXREGION, as Windows would report it.
// Why: w16's regions are single rectangles. That covers the uses met so far: Hearts combines
// rectangles to say what to redraw, and redrawing a little more than needed draws the same picture.
// Notice: A region used to clip may let a little more be painted than Windows would.
// Whole: Regions as lists of rectangles, as Windows kept them. On the roadmap: "w16: regions as
// more than their bounding rectangle".
static int api_CombineRgn(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_gdiobj *d = obj(w, w16_arg16(a), GDI_REGION), *s1 = obj(w, w16_arg16(a), GDI_REGION);
    struct w16_gdiobj *s2 = obj(w, w16_arg16(a), GDI_REGION);
    uint16_t mode = w16_arg16(a);
    w->cpu.r[R_AX] = 0;                          // ERROR
    if (!d || !s1 || (mode != 5 && !s2)) return 8;
    int16_t A[4], B[4] = { 0, 0, 0, 0 };
    memcpy(A, s1->rgn, sizeof A);
    if (s2) memcpy(B, s2->rgn, sizeof B);
    int ac = s1->complex, bc = s2 ? s2->complex : 0;
    int aempty = A[2] <= A[0] || A[3] <= A[1], bempty = B[2] <= B[0] || B[3] <= B[1];
    int il = A[0] > B[0] ? A[0] : B[0], it = A[1] > B[1] ? A[1] : B[1];
    int ir = A[2] < B[2] ? A[2] : B[2], ib = A[3] < B[3] ? A[3] : B[3];
    int meet = il < ir && it < ib;
    switch (mode) {
    case 1:                                      // RGN_AND
        rgn_set(d, il, it, ir, ib);
        d->complex = meet && (ac || bc);
        break;
    case 2: case 3:                              // RGN_OR, RGN_XOR
        if (aempty) { memcpy(d->rgn, B, sizeof B); d->complex = bc; break; }
        if (bempty) { memcpy(d->rgn, A, sizeof A); d->complex = ac; break; }
        rgn_set(d, A[0] < B[0] ? A[0] : B[0], A[1] < B[1] ? A[1] : B[1],
                A[2] > B[2] ? A[2] : B[2], A[3] > B[3] ? A[3] : B[3]);
        d->complex = mode == 3 ? 1 : !((il == A[0] && it == A[1] && ir == A[2] && ib == A[3]) ||
                                       (il == B[0] && it == B[1] && ir == B[2] && ib == B[3]));
        break;
    case 4:                                      // RGN_DIFF
        memcpy(d->rgn, A, sizeof A);
        d->complex = ac || meet;
        if (meet && il <= A[0] && it <= A[1] && ir >= A[2] && ib >= A[3]) rgn_set(d, 0, 0, 0, 0);
        break;
    default:                                     // RGN_COPY
        memcpy(d->rgn, A, sizeof A);
        d->complex = ac;
        break;
    }
    w->cpu.r[R_AX] = (uint16_t)rgn_kind(d);
    return 8;
}

// CreateDIBitmap(hdc, lpbmih, fdwInit, lpbInit, lpbmi, fuUsage) -> HBITMAP
// Windows: A bitmap in the device's colours made from a device-independent bitmap: a header, a
// colour table, rows bottom-up.
// w16: The same, at 1, 4, 8 and 24 bits per pixel, the header, colour table and rows checked to lie
// inside the program's memory; compressed (RLE) bitmaps are refused.
static int api_CreateDIBitmap(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return create_dibitmap(w, a);
}

// SetDIBitsToDevice(hdc, XDest, YDest, dwWidth, dwHeight, XSrc, YSrc, uStartScan, cScanLines, lpvBits, lpbmi, fuColorUse) -> int
// Windows: Draws a device-independent bitmap's rows straight onto a DC.
// w16: The same, for the same bitmap formats as CreateDIBitmap.
static int api_SetDIBitsToDevice(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return set_dibits_to_device(w, a);
}

// --- the mapping, the clip and SaveDC -----------------------------------------------

// HORZSIZE and VERTSIZE over HORZRES and VERTRES: what a fixed mapping mode's
// unit is in pixels, from the device's physical size as 3.1 took it.
static void fixed_mode(struct w16_gdiobj *dc, int mode) {
    static const struct { int mode; long per_mm_num, per_mm_den; } U[] = {
        { 2, 10, 1 }, { 3, 100, 1 }, { 4, 100, 254 }, { 5, 1000, 254 }, { 6, 14400, 254 },
    };
    for (unsigned i = 0; i < sizeof U / sizeof U[0]; i++)
        if (U[i].mode == mode) {
            dc->wext[0] = (int16_t)(device_caps(4) * U[i].per_mm_num / U[i].per_mm_den);
            dc->wext[1] = (int16_t)(device_caps(6) * U[i].per_mm_num / U[i].per_mm_den);
            dc->vext[0] = (int16_t)device_caps(8);
            dc->vext[1] = (int16_t)-device_caps(10);   // y grows upward
            return;
        }
    dc->wext[0] = dc->wext[1] = dc->vext[0] = dc->vext[1] = 1;
}

// MM_ISOTROPIC: the viewport's extents cut so one logical unit is as long on both axes.
static void isotropic(struct w16_gdiobj *dc) {
    if (dc->map_mode != MM_ISOTROPIC || !dc->wext[0] || !dc->wext[1]) return;
    long xs = labs((long)dc->vext[0] * 1000 / dc->wext[0]), ys = labs((long)dc->vext[1] * 1000 / dc->wext[1]);
    if (xs < ys) dc->vext[1] = (int16_t)scale(dc->vext[1], xs, ys);
    else if (ys < xs) dc->vext[0] = (int16_t)scale(dc->vext[0], ys, xs);
}

// SetMapMode(hdc, fnMapMode) -> int
// Windows: How logical units become pixels: MM_TEXT (a unit a pixel, y down), the fixed modes
// (0.1 or 0.01 mm, 0.01 or 0.001 inch, a twip; y up), or MM_ISOTROPIC and MM_ANISOTROPIC, whose
// scale the program sets with the extents. Returns the old mode.
// w16: The same: every drawing call, DPtoLP and LPtoDP go through the mapping; a fixed mode's unit
// is taken from the display's physical size, as 3.1 did.
static int api_SetMapMode(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_gdiobj *dc = obj(w, w16_arg16(a), GDI_DC);
    uint16_t mode = w16_arg16(a);
    w->cpu.r[R_AX] = dc ? (uint16_t)dc->map_mode : 0;
    if (dc && mode >= MM_TEXT && mode <= MM_ANISOTROPIC) {
        // MM_ISOTROPIC starts from MM_LOMETRIC's extents; MM_ANISOTROPIC keeps the ones in force
        dc->map_mode = mode;
        if (mode < MM_ISOTROPIC) fixed_mode(dc, mode);
        else if (mode == MM_ISOTROPIC) fixed_mode(dc, 2);
    }
    return 4;
}

// GetMapMode(hdc) -> int
// Windows: The DC's mapping mode.
// w16: The same.
static int api_GetMapMode(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_gdiobj *dc = obj(w, w16_arg16(a), GDI_DC);
    w->cpu.r[R_AX] = dc ? (uint16_t)dc->map_mode : 0;
    return 2;
}

// SetWindowOrg(hdc, X, Y) -> DWORD
// SetWindowExt(hdc, X, Y) -> DWORD
// SetViewportOrg(hdc, X, Y) -> DWORD
// SetViewportExt(hdc, X, Y) -> DWORD
// Windows: The mapping's two rectangles: the window in logical units, the viewport in pixels -- a
// point at the window's origin lands at the viewport's, and the extents' ratio is the scale. The
// extents count only in MM_ISOTROPIC (which keeps the axes alike) and MM_ANISOTROPIC. Each returns
// the old pair, x low.
// w16: The same.
static int api_SetWindowOrg(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct w16_gdiobj *dc = obj(w, w16_arg16(a), GDI_DC);
    int16_t x = (int16_t)w16_arg16(a), y = (int16_t)w16_arg16(a);
    set32(&w->cpu, 0);
    if (!dc) return 6;
    int16_t *f = ordinal == 11 ? dc->worg : ordinal == 12 ? dc->wext : ordinal == 13 ? dc->vorg : dc->vext;
    set32(&w->cpu, (uint32_t)(uint16_t)f[1] << 16 | (uint16_t)f[0]);
    int ext = ordinal == 12 || ordinal == 14;
    if (ext && (dc->map_mode < MM_ISOTROPIC || !x || !y)) return 6;
    f[0] = x;
    f[1] = y;
    if (ext) isotropic(dc);
    return 6;
}

// GetWindowOrg(hdc) -> DWORD
// GetWindowExt(hdc) -> DWORD
// GetViewportOrg(hdc) -> DWORD
// GetViewportExt(hdc) -> DWORD
// Windows: The mapping's origins and extents, x low.
// w16: The same.
static int api_GetViewportExt(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct w16_gdiobj *dc = obj(w, w16_arg16(a), GDI_DC);
    set32(&w->cpu, 0);
    if (!dc) return 2;
    int16_t *f = ordinal == 97 ? dc->worg : ordinal == 96 ? dc->wext : ordinal == 95 ? dc->vorg : dc->vext;
    set32(&w->cpu, (uint32_t)(uint16_t)f[1] << 16 | (uint16_t)f[0]);
    return 2;
}

// DPtoLP(hdc, lpPoints, nCount) -> BOOL
// LPtoDP(hdc, lpPoints, nCount) -> BOOL
// Windows: An array of points from pixels to logical units (DPtoLP), or back (LPtoDP), through the
// DC's mapping.
// w16: The same.
static int api_DPtoLP(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct w16_gdiobj *dc = obj(w, w16_arg16(a), GDI_DC);
    uint32_t far = w16_arg32(a);
    int16_t n = (int16_t)w16_arg16(a);
    w->cpu.r[R_AX] = 0;
    if (!dc || n <= 0) return 8;
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, (uint32_t)n * 4, 1);
    if (!p) return -1;
    for (int i = 0; i < n; i++) {
        int16_t v[2];
        memcpy(v, p + 4 * i, 4);
        int x = ordinal == 67 ? log_x(dc, v[0]) : dev_x(dc, v[0]);
        int y = ordinal == 67 ? log_y(dc, v[1]) : dev_y(dc, v[1]);
        v[0] = (int16_t)x;
        v[1] = (int16_t)y;
        memcpy(p + 4 * i, v, 4);
    }
    w->cpu.r[R_AX] = 1;
    return 8;
}

// IntersectClipRect(hdc, nLeftRect, nTopRect, nRightRect, nBottomRect) -> int
// Windows: Cuts the DC's clipping region down to its overlap with a rectangle, in logical units;
// returns what is left: NULLREGION, SIMPLEREGION or COMPLEXREGION.
// w16: The same. A DC's clip is one rectangle, which the overlap of rectangles always is.
static int api_IntersectClipRect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_gdiobj *dc = obj(w, w16_arg16(a), GDI_DC);
    int l = (int16_t)w16_arg16(a), t = (int16_t)w16_arg16(a), r = (int16_t)w16_arg16(a), b = (int16_t)w16_arg16(a);
    w->cpu.r[R_AX] = 0;                          // ERROR
    if (!dc) return 10;
    dev_rect(dc, &l, &t, &r, &b);
    if (dc->has_clip) {
        if (dc->clip[0] > l) l = dc->clip[0];
        if (dc->clip[1] > t) t = dc->clip[1];
        if (dc->clip[2] < r) r = dc->clip[2];
        if (dc->clip[3] < b) b = dc->clip[3];
    }
    if (r < l) r = l;
    if (b < t) b = t;
    dc->has_clip = 1;
    dc->clip[0] = (int16_t)l; dc->clip[1] = (int16_t)t; dc->clip[2] = (int16_t)r; dc->clip[3] = (int16_t)b;
    w->cpu.r[R_AX] = r > l && b > t ? 2 : 1;
    return 10;
}

// SelectClipRgn(hdc, hrgn) -> int
// Windows: Makes a copy of a region, in pixels, the DC's clipping region; NULL takes the clip away.
// Returns the region's kind.
// w16: The same, the region being its bounding rectangle (see CombineRgn).
static int api_SelectClipRgn(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_gdiobj *dc = obj(w, w16_arg16(a), GDI_DC);
    uint16_t h = w16_arg16(a);
    struct w16_gdiobj *r = obj(w, h, GDI_REGION);
    w->cpu.r[R_AX] = 0;
    if (!dc || (h && !r)) return 4;
    dc->has_clip = r != 0;
    if (r) memcpy(dc->clip, r->rgn, sizeof dc->clip);
    w->cpu.r[R_AX] = r ? (uint16_t)rgn_kind(r) : 2;
    return 4;
}

// SaveDC(hdc) -> int
// RestoreDC(hdc, nSavedDC) -> BOOL
// Windows: Keep a DC's state -- its objects, colours, modes, mapping and clip -- on a stack, and
// return to one of them: RestoreDC(-1) the last saved, a positive number that level, the ones
// saved after it dropped.
// w16: The same. The bitmap a memory DC draws on stays as it is.
static int api_SaveDC(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct w16_gdiobj *dc = obj(w, w16_arg16(a), GDI_DC);
    int16_t n = ordinal == 39 ? (int16_t)w16_arg16(a) : 0;
    w->cpu.r[R_AX] = 0;
    if (!dc) return ordinal == 39 ? 4 : 2;
    if (ordinal == 30) {
        struct w16_gdiobj *g = realloc(dc->saved, sizeof *g * (size_t)(dc->nsaved + 1));
        if (!g) return 2;
        dc->saved = g;
        g[dc->nsaved] = *dc;
        w->cpu.r[R_AX] = (uint16_t)++dc->nsaved;
        return 2;
    }
    int level = n < 0 ? dc->nsaved + n + 1 : n;
    if (level < 1 || level > dc->nsaved) return 4;
    struct w16_gdiobj keep = dc->saved[level - 1], *stack = dc->saved;
    uint16_t bitmap = dc->bitmap, hwnd = dc->hwnd;
    uint8_t screen = dc->screen;
    *dc = keep;
    dc->saved = stack;
    dc->nsaved = level - 1;
    dc->bitmap = bitmap;
    dc->hwnd = hwnd;
    dc->screen = screen;
    w->cpu.r[R_AX] = 1;
    return 4;
}

// SetPolyFillMode(hdc, iPolyFillMode) -> int
// SetStretchBltMode(hdc, iStretchMode) -> int
// Windows: How Polygon fills a shape that crosses itself (ALTERNATE or WINDING); and how StretchBlt
// drops rows and columns when it shrinks: keeping black, keeping white, or simply deleting
// (COLORONCOLOR). Each returns the old mode.
// w16: The same fill modes. The stretch mode is kept and answered; StretchBlt always takes the
// nearest pixel, COLORONCOLOR's way.
static int api_SetStretchBltMode(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct w16_gdiobj *dc = obj(w, w16_arg16(a), GDI_DC);
    uint16_t v = w16_arg16(a);
    int *f = dc ? (ordinal == 7 ? &dc->stretch_mode : &dc->fill_mode) : 0;
    w->cpu.r[R_AX] = f ? (uint16_t)*f : 0;
    if (f && v) *f = v;
    return 4;
}

// SetTextJustification(hdc, nBreakExtra, nBreakCount) -> int
// Windows: Spreads nBreakExtra more pixels (logical units) over the next nBreakCount spaces that
// TextOut draws and GetTextExtent measures -- how a word processor justifies a line. 0 and 0 ends it.
// w16: The same, the remainder going to the first spaces.
static int api_SetTextJustification(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_gdiobj *dc = obj(w, w16_arg16(a), GDI_DC);
    int16_t extra = (int16_t)w16_arg16(a), count = (int16_t)w16_arg16(a);
    if (dc) {
        dc->break_extra = count > 0 ? (int16_t)abs(dev_w(dc, extra)) : 0;
        dc->break_count = count > 0 ? count : 0;
    }
    w->cpu.r[R_AX] = dc != 0;
    return 6;
}

// Ellipse(hdc, nLeftRect, nTopRect, nRightRect, nBottomRect) -> BOOL
// Windows: An ellipse inside a rectangle -- its right and bottom left out -- edged in the pen and
// filled with the brush.
// w16: The same, edged a pixel wide: a wider pen draws as a 1-pixel one.
static int api_Ellipse(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t hdc = w16_arg16(a);
    int l = (int16_t)w16_arg16(a), t = (int16_t)w16_arg16(a), r = (int16_t)w16_arg16(a), b = (int16_t)w16_arg16(a);
    w->cpu.r[R_AX] = (uint16_t)ellipse(w, hdc, l, t, r, b);
    return 10;
}

// RoundRect(hdc, nLeftRect, nTopRect, nRightRect, nBottomRect, nWidth, nHeight) -> BOOL
// Windows: A rectangle with rounded corners -- each a quarter of an ellipse nWidth by nHeight --
// edged in the pen and filled with the brush, its right and bottom left out.
// w16: The same, edged a pixel wide: a wider pen draws as a 1-pixel one.
static int api_RoundRect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t hdc = w16_arg16(a);
    int l = (int16_t)w16_arg16(a), t = (int16_t)w16_arg16(a), r = (int16_t)w16_arg16(a), b = (int16_t)w16_arg16(a);
    int ew = (int16_t)w16_arg16(a), eh = (int16_t)w16_arg16(a);
    w->cpu.r[R_AX] = (uint16_t)roundrect(w, hdc, l, t, r, b, ew, eh);
    return 14;
}

// Arc(hdc, nLeftRect, nTopRect, nRightRect, nBottomRect, nXStartArc, nYStartArc, nXEndArc, nYEndArc) -> BOOL
// Chord(hdc, nLeftRect, nTopRect, nRightRect, nBottomRect, nXRadial1, nYRadial1, nXRadial2, nYRadial2) -> BOOL
// Pie(hdc, nLeftRect, nTopRect, nRightRect, nBottomRect, nXRadial1, nYRadial1, nXRadial2, nYRadial2) -> BOOL
// Windows: Part of the ellipse inside a rectangle, counterclockwise from where the line from its
// centre through the first point meets it to where the line through the second does: the curve
// alone in the pen (Arc); closed by the line between its ends (Chord) or by two lines to the centre
// (Pie) and filled with the brush. The two points the same: the whole ellipse.
// w16: The same, edged a pixel wide: a wider pen draws as a 1-pixel one.
static int api_Arc(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    uint16_t hdc = w16_arg16(a);
    int v[8];
    for (int i = 0; i < 8; i++) v[i] = (int16_t)w16_arg16(a);
    int kind = ordinal == 23 ? ARC_ARC : ordinal == 26 ? ARC_PIE : ARC_CHORD;
    w->cpu.r[R_AX] = (uint16_t)arc_shape(w, hdc, kind, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    return 18;
}

// Polygon(hdc, lpPoints, nCount) -> BOOL
// Polyline(hdc, lpPoints, nCount) -> BOOL
// Windows: A closed shape through the points, edged in the pen and filled with the brush by the
// polygon fill mode (Polygon); or lines joining them, nothing filled (Polyline).
// w16: The same, ALTERNATE and WINDING both.
static int api_Polygon(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    uint16_t hdc = w16_arg16(a);
    uint32_t far = w16_arg32(a);
    int16_t n = (int16_t)w16_arg16(a);
    w->cpu.r[R_AX] = 0;
    if (n <= 0) return 8;
    const uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, (uint32_t)n * 4, 0);
    if (!p) return -1;
    int *pt = malloc(sizeof *pt * 2 * (size_t)n), cnt = n;
    if (!pt) return 8;
    for (int i = 0; i < 2 * n; i++) pt[i] = (int16_t)(p[2 * i] | p[2 * i + 1] << 8);
    w->cpu.r[R_AX] = (uint16_t)polygons(w, hdc, pt, &cnt, 1, ordinal == 36);
    free(pt);
    return 8;
}

// PolyPolygon(hdc, lpPoints, lpPolyCounts, nCount) -> BOOL
// Windows: Several closed shapes at once, the points one after another and a count for each,
// filled together by the polygon fill mode -- so one inside another can be a hole.
// w16: The same.
static int api_PolyPolygon(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t hdc = w16_arg16(a);
    uint32_t fpt = w16_arg32(a), fcnt = w16_arg32(a);
    int16_t npoly = (int16_t)w16_arg16(a);
    w->cpu.r[R_AX] = 0;
    if (npoly <= 0) return 12;
    const uint8_t *c = cpu16_ptr(&w->cpu, (uint16_t)(fcnt >> 16), (uint16_t)fcnt, (uint32_t)npoly * 2, 0);
    if (!c) return -1;
    int *n = malloc(sizeof *n * (size_t)npoly), total = 0;
    if (!n) return 12;
    for (int i = 0; i < npoly; i++) total += n[i] = c[2 * i] | c[2 * i + 1] << 8;
    const uint8_t *p = total ? cpu16_ptr(&w->cpu, (uint16_t)(fpt >> 16), (uint16_t)fpt, (uint32_t)total * 4, 0) : 0;
    int *pt = p ? malloc(sizeof *pt * 2 * (size_t)total) : 0;
    if (pt) {
        for (int i = 0; i < 2 * total; i++) pt[i] = (int16_t)(p[2 * i] | p[2 * i + 1] << 8);
        w->cpu.r[R_AX] = (uint16_t)polygons(w, hdc, pt, n, npoly, 1);
    }
    free(pt);
    free(n);
    return total && !p ? -1 : 12;
}

// CreateBitmapIndirect(lpbm) -> HBITMAP
// Windows: CreateBitmap from a BITMAP structure: its size, planes, bits per pixel and bits.
// w16: The same as CreateBitmap.
static int api_CreateBitmapIndirect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t far = w16_arg32(a);
    const uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, 14, 0);
    if (!p) return -1;
    int bw = (int16_t)(p[2] | p[3] << 8), bh = (int16_t)(p[4] | p[5] << 8), stride = p[6] | p[7] << 8;
    int mono = p[8] * p[9] == 1;
    uint32_t bits = (uint32_t)(p[10] | p[11] << 8 | p[12] << 16 | (uint32_t)p[13] << 24);
    uint16_t h;
    struct w16_gdiobj *bm = new_bitmap(w, bw, bh, mono, &h);
    if (bm && mono && (bits >> 16)) {
        if (stride < (bw + 7) / 8) stride = (bw + 15) / 16 * 2;
        const uint8_t *q = cpu16_ptr(&w->cpu, (uint16_t)(bits >> 16), (uint16_t)bits, (uint32_t)stride * (unsigned)bh, 0);
        if (!q) return -1;
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++)
                bm->px[(size_t)y * bw + x] = q[y * stride + x / 8] >> (7 - x % 8) & 1 ? 0xFFFFFF : 0;
    }
    w->cpu.r[R_AX] = h;
    return 4;
}

// CreateDiscardableBitmap(hdc, nWidth, nHeight) -> HBITMAP
// Windows: CreateCompatibleBitmap's bitmap, which Windows may throw away when memory runs short
// while it is not selected into a DC.
// w16: CreateCompatibleBitmap's: w16 throws nothing away.
static int api_CreateDiscardableBitmap(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    return api_CreateCompatibleBitmap(w, ordinal, a);
}

// SetBitmapDimension(hBitmap, nWidth, nHeight) -> DWORD
// GetBitmapDimension(hBitmap) -> DWORD
// Windows: A bitmap's size in 0.1 mm, which GDI keeps for the program and uses for nothing; Set
// returns the old one.
// w16: The same.
static int api_SetBitmapDimension(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct w16_gdiobj *bm = obj(w, w16_arg16(a), GDI_BITMAP);
    int16_t x = ordinal == 163 ? (int16_t)w16_arg16(a) : 0, y = ordinal == 163 ? (int16_t)w16_arg16(a) : 0;
    set32(&w->cpu, bm ? (uint32_t)(uint16_t)bm->dim[1] << 16 | (uint16_t)bm->dim[0] : 0);
    if (bm && ordinal == 163) { bm->dim[0] = x; bm->dim[1] = y; }
    return ordinal == 163 ? 6 : 2;
}

// GetNearestColor(hdc, crColor) -> COLORREF
// Windows: The solid colour the device would draw for a colour.
// w16: The colour as Windows 3.11's 256-colour VGA driver showed it -- what w16 draws.
static int api_GetNearestColor(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);
    uint32_t px = w16_colorref_px(w16_arg32(a) & 0xFFFFFF);
    set32(&w->cpu, (px & 0xFF) << 16 | (px & 0xFF00) | (px >> 16 & 0xFF));
    return 6;
}

// A solid colour over a rectangle of a DC, as FillRect does a brush: ExtTextOut's opaque box.
static void fill_color(struct w16 *w, uint16_t hdc, int l, int t, int r, int b, uint32_t colorref) {
    struct gdi_target dt;
    struct w16_gdiobj *dc = target(w, hdc, &dt);
    if (dc) {
        dev_rect(dc, &l, &t, &r, &b);
        struct gdi_blit bl = { .dst = &dt, .x = l, .y = t, .w = r - l, .h = b - t,
                               .rop = 0x00F00021, .brush = w16_colorref_px(colorref) };
        gdi_bitblt(&bl);
    }
    gdi_done(w, dc);
}

// GetBkColor(hdc) -> COLORREF
// GetTextColor(hdc) -> COLORREF
// Windows: The DC's background colour, and its text colour.
// w16: The same, as set -- not the nearest colour the driver would draw.
static int api_GetBkColor(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct w16_gdiobj *dc = obj(w, w16_arg16(a), GDI_DC);
    set32(&w->cpu, dc ? (ordinal == 75 ? dc->bk : dc->text) : 0);
    return 2;
}

// GetTextAlign(hdc) -> WORD
// SetTextAlign(hdc, wFlags) -> WORD
// Windows: Which point of the text the position given to TextOut names -- left, centre or right,
// top, baseline or bottom (TA_*) -- or that the current position is used and moved (TA_UPDATECP);
// SetTextAlign returns the old word.
// w16: Left, centre and right, top, baseline and bottom are honoured by TextOut and ExtTextOut;
// TA_UPDATECP is kept and answered but does not move the current position.
static int api_SetTextAlign(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct w16_gdiobj *dc = obj(w, w16_arg16(a), GDI_DC);
    uint16_t old = dc ? dc->text_align : 0;
    if (ordinal == 346) {
        uint16_t v = w16_arg16(a);
        if (dc) dc->text_align = v;
        w->cpu.r[R_AX] = old;
        return 4;
    }
    w->cpu.r[R_AX] = old;
    return 2;
}

// ExtTextOut(hdc, X, Y, fuOptions, lprc, lpString, cbString, lpDx) -> BOOL
// Windows: TextOut with a rectangle: the rectangle may be filled with the background colour first
// (ETO_OPAQUE) and the text cut to it (ETO_CLIPPED); lpDx gives the distance from each character
// to the next.
// w16: The same, including the character spacing; the fill is the whole rectangle in the
// background colour whatever the background mode.
static int api_ExtTextOut(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t hdc = w16_arg16(a);
    int x = (int16_t)w16_arg16(a), y = (int16_t)w16_arg16(a);
    unsigned opts = w16_arg16(a);
    uint32_t rcfar = w16_arg32(a), sfar = w16_arg32(a);
    unsigned n = w16_arg16(a);
    uint32_t dxfar = w16_arg32(a);
    struct w16_gdiobj *dc = obj(w, hdc, GDI_DC);
    w->cpu.r[R_AX] = 0;
    if (!dc) return 22;
    int16_t rc[4] = { 0, 0, 0, 0 };
    int have_rc = 0;
    if (rcfar >> 16) {
        const uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(rcfar >> 16), (uint16_t)rcfar, 8, 0);
        if (!p) return -1;
        for (int i = 0; i < 4; i++) rc[i] = (int16_t)(p[2 * i] | p[2 * i + 1] << 8);
        have_rc = 1;
    }
    char *text = far_text(w, sfar, n);
    if (!text) return -1;
    if (have_rc && (opts & 2)) fill_color(w, hdc, rc[0], rc[1], rc[2], rc[3], dc->bk);
    int clip[4] = { rc[0], rc[1], rc[2], rc[3] };
    const int *cp = have_rc && (opts & 4) ? clip : 0;
    if (dxfar >> 16 && n) {                      // each character at its own distance
        const uint8_t *dx = cpu16_ptr(&w->cpu, (uint16_t)(dxfar >> 16), (uint16_t)dxfar, n * 2, 0);
        if (!dx) { free(text); return -1; }
        int pen = x;
        for (unsigned i = 0; i < n; i++) {
            char one[2] = { text[i], 0 };
            draw_text_clip(w, hdc, pen, y, one, 0, cp);
            pen += (int16_t)(dx[2 * i] | dx[2 * i + 1] << 8);
        }
        w->cpu.r[R_AX] = 1;
    } else {
        w->cpu.r[R_AX] = draw_text_clip(w, hdc, x, y, text, 0, cp) >= 0;
    }
    free(text);
    return 22;
}

// ExcludeClipRect(hdc, nLeftRect, nTopRect, nRightRect, nBottomRect) -> int
// Windows: Cuts a rectangle out of the DC's clipping region, in logical units; returns the region's
// kind.
// w16: A DC's clip is one rectangle, so only an exclusion that takes a whole side off it is
// applied (a rectangle across the full height at the left or right edge, or across the full width
// at the top or bottom); one in the middle leaves the clip as it was.
// Why: A DC's clip is one rectangle, and a hole in it needs a region made of several.
// Notice: A program that excludes a rectangle from the middle of its clip draws over it.
// Whole: Regions as more than their bounding rectangle. On the roadmap: "w16: regions as more than their
// bounding rectangle".
static int api_ExcludeClipRect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_gdiobj *dc = obj(w, w16_arg16(a), GDI_DC);
    int l = (int16_t)w16_arg16(a), t = (int16_t)w16_arg16(a), r = (int16_t)w16_arg16(a), b = (int16_t)w16_arg16(a);
    w->cpu.r[R_AX] = 0;
    if (!dc) return 10;
    dev_rect(dc, &l, &t, &r, &b);
    struct gdi_target tg;
    fill_target(w, dc, &tg);
    int c[4] = { dc->has_clip ? dc->clip[0] : tg.x0 - tg.ox, dc->has_clip ? dc->clip[1] : tg.y0 - tg.oy,
                 dc->has_clip ? dc->clip[2] : tg.x1 - tg.ox, dc->has_clip ? dc->clip[3] : tg.y1 - tg.oy };
    if (t <= c[1] && b >= c[3] && l <= c[0] && r > c[0]) c[0] = r < c[2] ? r : c[2];         // the left side
    else if (t <= c[1] && b >= c[3] && r >= c[2] && l < c[2]) c[2] = l > c[0] ? l : c[0];    // the right
    else if (l <= c[0] && r >= c[2] && t <= c[1] && b > c[1]) c[1] = b < c[3] ? b : c[3];    // the top
    else if (l <= c[0] && r >= c[2] && b >= c[3] && t < c[3]) c[3] = t > c[1] ? t : c[1];    // the bottom
    dc->has_clip = 1;
    for (int i = 0; i < 4; i++) dc->clip[i] = (int16_t)c[i];
    w->cpu.r[R_AX] = c[2] > c[0] && c[3] > c[1] ? 2 : 1;
    return 10;
}

// GetClipBox(hdc, lprc) -> int
// Windows: The smallest rectangle round what the DC can draw on, in logical units; returns the
// region's kind.
// w16: The same: the visible part of the window's client area, or the bitmap, cut by the DC's clip.
static int api_GetClipBox(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_gdiobj *dc = obj(w, w16_arg16(a), GDI_DC);
    uint32_t far = w16_arg32(a);
    w->cpu.r[R_AX] = 0;
    if (!dc) return 6;
    struct gdi_target t;
    fill_target(w, dc, &t);
    int l = t.x0 - t.ox, tp = t.y0 - t.oy, r = t.x1 - t.ox, b = t.y1 - t.oy;
    if (r < l) r = l;
    if (b < tp) b = tp;
    int ll = log_x(dc, l), tl = log_y(dc, tp), rl = log_x(dc, r), bl = log_y(dc, b);
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, 8, 1);
    if (!p) return -1;
    int16_t out[4] = { (int16_t)(ll < rl ? ll : rl), (int16_t)(tl < bl ? tl : bl), (int16_t)(ll < rl ? rl : ll),
                       (int16_t)(tl < bl ? bl : tl) };
    for (int i = 0; i < 4; i++) { p[2 * i] = (uint8_t)out[i]; p[2 * i + 1] = (uint8_t)((uint16_t)out[i] >> 8); }
    w->cpu.r[R_AX] = r > l && b > tp ? 2 : 1;
    return 6;
}

// CreatePalette(lplgpl) -> HPALETTE
// Windows: A logical palette -- the colours a program wants -- from a LOGPALETTE.
// w16: The palette is kept and deleted, but colours are drawn as the 256-colour VGA driver drew
// them (its 20 static colours), so a palette changes no pixel.
static int api_CreatePalette(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t far = w16_arg32(a);
    const uint8_t *hd = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, 4, 0);
    w->cpu.r[R_AX] = 0;
    if (!hd) return -1;
    unsigned n = (unsigned)(hd[2] | hd[3] << 8);
    const uint8_t *e = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)(far + 4), (n ? n * 4 : 1), 0);
    if (!e) return -1;
    struct w16_gdiobj *o;
    uint16_t h = new_obj(w, GDI_PALETTE, &o);
    if (!h) return 4;
    o->px = calloc(n ? n : 1, 4);
    if (!o->px) { memset(o, 0, sizeof *o); return 4; }
    o->bw = (int)n;
    for (unsigned i = 0; i < n; i++) o->px[i] = (uint32_t)e[4 * i] << 16 | (uint32_t)e[4 * i + 1] << 8 | e[4 * i + 2];
    w->cpu.r[R_AX] = h;
    return 4;
}

// GetSystemPaletteEntries(hdc, wStartIndex, wNumEntries, lpPaletteEntries) -> UINT
// Windows: Reads entries of the device's hardware palette.
// w16: The 20 static colours at their places (0-9 and 246-255) and black between.
static int api_GetSystemPaletteEntries(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    static const uint32_t SYS[20] = { 0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xC0C0C0,
                                      0xC0DCC0, 0xA6CAF0, 0xFFFBF0, 0xA0A0A4, 0x808080, 0xFF0000, 0x00FF00,
                                      0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF };
    w16_arg16(a);
    unsigned start = w16_arg16(a), n = w16_arg16(a);
    uint32_t far = w16_arg32(a);
    w->cpu.r[R_AX] = 0;
    if (!(far >> 16)) { w->cpu.r[R_AX] = 256; return 10; }
    if (start >= 256) return 10;
    if (n > 256 - start) n = 256 - start;
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, (n ? n * 4 : 1), 1);
    if (!p) return -1;
    for (unsigned i = 0; i < n; i++) {
        unsigned idx = start + i;
        uint32_t c = idx < 10 ? SYS[idx] : idx >= 246 ? SYS[idx - 236] : 0;
        p[4 * i] = (uint8_t)(c >> 16); p[4 * i + 1] = (uint8_t)(c >> 8); p[4 * i + 2] = (uint8_t)c; p[4 * i + 3] = 0;
    }
    w->cpu.r[R_AX] = (uint16_t)n;
    return 10;
}

// SetMetaFileBits(hMem) -> HMETAFILE
// Windows: Makes a memory metafile of the bits in a global block, which the metafile then owns.
// w16: The block is the metafile: a metafile handle here is the global block's.
static int api_SetMetaFileBits(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w->cpu.r[R_AX] = w16_arg16(a);
    return 2;
}

// SetDIBits(hdc, hbmp, uStartScan, cScanLines, lpvBits, lpbmi, fuColorUse) -> int
// Windows: Sets a bitmap's pixels from rows of a device-independent bitmap, counted from the bottom
// of a bottom-up one; returns the rows copied.
// w16: The same, for the DIB formats CreateDIBitmap reads.
static int api_SetDIBits(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);
    struct w16_gdiobj *bm = obj(w, w16_arg16(a), GDI_BITMAP);
    unsigned start = w16_arg16(a), lines = w16_arg16(a);
    uint32_t lpbits = w16_arg32(a), lpbmi = w16_arg32(a);
    unsigned usage = w16_arg16(a);
    w->cpu.r[R_AX] = 0;
    if (!bm) return 18;
    static struct gdi_dib d;                     // the task thread's alone
    if (!bmi_at(w, lpbmi, usage, &d)) return -1;
    const uint8_t *bits = dib_bits(w, lpbits, &d, lines);
    if (!bits) return -1;
    unsigned done = 0;
    for (unsigned k = 0; k < lines; k++) {
        int y = d.h < 0 ? (int)(start + k) : bm->bh - 1 - (int)(start + k);
        if (y < 0 || y >= bm->bh) continue;
        const uint8_t *row = bits + (size_t)k * d.stride;
        for (int x = 0; x < bm->bw && x < d.w; x++) bm->px[(size_t)y * bm->bw + x] = gdi_dib_px(&d, row, x);
        done++;
    }
    w->cpu.r[R_AX] = (uint16_t)done;
    return 18;
}

// DrawFocusRect's work: every second pixel of a rectangle's border inverted, twice undoing it.
void w16_gdi_focus_rect(struct w16 *w, uint16_t hdc, int l, int t, int r, int b) {
    struct gdi_target dt;
    struct w16_gdiobj *dc = target(w, hdc, &dt);
    if (dc) {
        dev_rect(dc, &l, &t, &r, &b);
        for (int x = l; x < r; x++) {
            if (!((x - l) & 1)) { gdi_pixel(&dt, x, t, 0, 6); if (b - 1 > t) gdi_pixel(&dt, x, b - 1, 0, 6); }
        }
        for (int y = t + 1; y < b - 1; y++) {
            if (!((y - t) & 1)) { gdi_pixel(&dt, l, y, 0, 6); if (r - 1 > l) gdi_pixel(&dt, r - 1, y, 0, 6); }
        }
    }
    gdi_done(w, dc);
}

const struct w16_api g_gdi_api[] = {
    { 1, api_SetBkColor },
    { 2, api_SetBkMode },
    { 3, api_SetMapMode },
    { 4, api_SetBkMode },
    { 6, api_SetStretchBltMode },
    { 7, api_SetStretchBltMode },
    { 9, api_SetBkColor },
    { 10, api_SetTextJustification },
    { 11, api_SetWindowOrg },
    { 12, api_SetWindowOrg },
    { 13, api_SetWindowOrg },
    { 14, api_SetWindowOrg },
    { 19, api_LineTo },
    { 20, api_MoveTo },
    { 21, api_ExcludeClipRect },
    { 22, api_IntersectClipRect },
    { 23, api_Arc },
    { 24, api_Ellipse },
    { 26, api_Arc },
    { 27, api_Rectangle },
    { 28, api_RoundRect },
    { 29, api_PatBlt },
    { 30, api_SaveDC },
    { 31, api_SetPixel },
    { 33, api_TextOut },
    { 34, api_BitBlt },
    { 35, api_StretchBlt },
    { 36, api_Polygon },
    { 37, api_Polygon },
    { 38, api_Escape },
    { 39, api_SaveDC },
    { 44, api_SelectClipRgn },
    { 45, api_SelectObject },
    { 47, api_CombineRgn },
    { 48, api_CreateBitmap },
    { 49, api_CreateBitmapIndirect },
    { 51, api_CreateCompatibleBitmap },
    { 52, api_CreateCompatibleDC },
    { 53, api_CreateDC },
    { 56, api_CreateFont },
    { 57, api_CreateFontIndirect },
    { 58, api_CreateHatchBrush },
    { 60, api_CreatePatternBrush },
    { 61, api_CreatePen },
    { 64, api_CreateRectRgn },
    { 66, api_CreateSolidBrush },
    { 67, api_DPtoLP },
    { 68, api_DeleteDC },
    { 69, api_DeleteDC },
    { 70, api_EnumFonts },
    { 75, api_GetBkColor },
    { 77, api_GetClipBox },
    { 79, api_GetDCOrg },
    { 80, api_GetDeviceCaps },
    { 81, api_GetMapMode },
    { 82, api_GetObject },
    { 83, api_SetPixel },
    { 87, api_GetStockObject },
    { 90, api_GetBkColor },
    { 91, api_GetTextExtent },
    { 92, api_GetTextFace },
    { 93, api_GetTextMetrics },
    { 94, api_GetViewportExt },
    { 95, api_GetViewportExt },
    { 96, api_GetViewportExt },
    { 97, api_GetViewportExt },
    { 99, api_DPtoLP },
    { 100, api_LineDDA },
    { 103, api_PtVisible },
    { 128, api_MulDiv },
    { 148, api_SetBrushOrg },
    { 150, api_UnrealizeObject },
    { 153, api_CreateIC },
    { 154, api_GetNearestColor },
    { 156, api_CreateDiscardableBitmap },
    { 160, api_SetMetaFileBits },
    { 162, api_SetBitmapDimension },
    { 163, api_SetBitmapDimension },
    { 172, api_SetRectRgn },
    { 330, api_EnumFontFamilies },
    { 345, api_SetTextAlign },
    { 346, api_SetTextAlign },
    { 348, api_Arc },
    { 350, api_GetCharWidth },
    { 351, api_ExtTextOut },
    { 360, api_CreatePalette },
    { 375, api_GetSystemPaletteEntries },
    { 440, api_SetDIBits },
    { 442, api_CreateDIBitmap },
    { 443, api_SetDIBitsToDevice },
    { 450, api_PolyPolygon },
};
const unsigned g_gdi_api_n = sizeof g_gdi_api / sizeof g_gdi_api[0];

int w16_gdi(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    int r = w16_api_call(g_meta_api, g_meta_api_n, w, ordinal, a);
    if (r != -2) return r;
    return w16_api_call(g_gdi_api, sizeof g_gdi_api / sizeof g_gdi_api[0], w, ordinal, a);
}
