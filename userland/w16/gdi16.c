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

#include <stdlib.h>
#include <string.h>
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

static void dc_defaults(struct w16_gdiobj *dc) {
    dc->pen = STOCK_BASE + BLACK_PEN;
    dc->brush = STOCK_BASE + WHITE_BRUSH;
    dc->font = STOCK_BASE + SYSTEM_FONT;
    dc->text = 0x000000;
    dc->bk = 0xFFFFFF;
    dc->bk_mode = OPAQUE;
    dc->rop2 = R2_COPYPEN;
    dc->cx = dc->cy = 0;
}

uint16_t w16_gdi_window_dc(struct w16 *w, uint16_t hwnd) {
    struct w16_wnd *x = w16_wnd_get(w, hwnd);
    if (!x) return 0;
    struct w16_gdiobj *dc = obj(w, x->dc, GDI_DC);
    if (!dc || dc->hwnd != hwnd) {
        x->dc = new_obj(w, GDI_DC, &dc);
        if (!dc) return 0;
        dc->hwnd = hwnd;
    }
    dc_defaults(dc);
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
    if (dc && dc->hwnd == hwnd) memset(dc, 0, sizeof *dc);
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
        return;
    }
    if (dc->screen) return;
    struct w16_gdiobj *bm = obj(w, dc->bitmap, GDI_BITMAP);
    if (bm) { t->px = bm->px; t->w = bm->bw; t->h = bm->bh; t->mono = bm->mono; }
    else { t->px = &g_scratch_px; t->w = t->h = 1; t->mono = 1; }
    gdi_target_whole(t);
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

static void gdi_done(struct w16 *w, struct w16_gdiobj *dc) {
    if (dc && dc->hwnd) {
        restore_children();
        w->fb_dirty = 1;
        pthread_mutex_unlock(&w->lock);
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

// `text` at (x, y) of a DC in the system font, its colours and
// background mode; a tab steps to the next of `tab` pixels when tab > 0
// (TabbedTextOut). Its width.
static int draw_text_clip(struct w16 *w, uint16_t hdc, int x, int y, const char *text, int tab,
                          const int *clip) {
    struct gdi_target t;
    struct w16_gdiobj *dc = target(w, hdc, &t);
    int width = 0;
    pthread_mutex_lock(&g_w16_text_lock);
    const struct ugfx_font *was = ugfx_set_font(w16_font_of(dc ? dc->font : 0));
    if (dc && t.px && clip) {                    // a DC-coordinate rectangle, within the window's
        if (clip[0] + t.ox > t.x0) t.x0 = clip[0] + t.ox;
        if (clip[1] + t.oy > t.y0) t.y0 = clip[1] + t.oy;
        if (clip[2] + t.ox < t.x1) t.x1 = clip[2] + t.ox;
        if (clip[3] + t.oy < t.y1) t.y1 = clip[3] + t.oy;
    }
    if (dc && t.px && t.x1 > t.x0 && t.y1 > t.y0) {
        struct ugfx_surface s = ugfx_surface_for_pixels(t.px, t.w, t.h);
        ugfx_set_clip_rect(&s, t.x0, t.y0, t.x1 - t.x0, t.y1 - t.y0);
        // OPAQUE fills the whole cell box first, as Windows did: the glyphs
        // are blended over what is there, so text redrawn in place (a
        // clock's digits) would otherwise pile up.
        for (int pass = dc->bk_mode == TRANSPARENT; pass < 2; pass++) {
            width = 0;
            for (const char *p = text; *p; ) {
                size_t run = tab > 0 ? strcspn(p, "\t") : strlen(p);
                char *piece = strndup(p, run);
                if (piece && pass) ugfx_draw_string(&s, x + t.ox + width, y + t.oy, piece, w16_colorref_px(dc->text),
                                                    UGFX_TRANSPARENT);
                width += piece ? ugfx_text_width(piece) : 0;
                free(piece);
                p += run;
                if (*p == '\t') { width = (width / tab + 1) * tab; p++; }
            }
            if (pass) continue;
            ugfx_fill_rect(&s, x + t.ox, y + t.oy, width, ugfx_char_h(), w16_colorref_px(dc->bk));
            // ...and the ink kept inside that box: an anti-aliased stand-in
            // glyph spills a pixel past its advance, which the next fill
            // would leave behind (3.11's raster fonts never spill).
            int l = x + t.ox > t.x0 ? x + t.ox : t.x0, r = x + t.ox + width < t.x1 ? x + t.ox + width : t.x1;
            ugfx_set_clip_rect(&s, l, t.y0, r - l, t.y1 - t.y0);
        }
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
        int64_t br = brush_of(w, dc->brush);
        struct gdi_blit b = { .dst = &dt, .x = x, .y = y, .w = cw, .h = ch, .rop = rop,
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
        struct gdi_blit bl = { .dst = &dt, .x = l, .y = t, .w = r - l, .h = b - t, .rop = 0x00550009 };
        gdi_bitblt(&bl);                         // DSTINVERT
    }
    gdi_done(w, dc);
}

static int rectangle(struct w16 *w, struct w16_args *a) {
    uint16_t hd = w16_arg16(a);
    int16_t l = (int16_t)w16_arg16(a), t = (int16_t)w16_arg16(a);
    int16_t r = (int16_t)w16_arg16(a), b = (int16_t)w16_arg16(a);
    struct gdi_target dt;
    struct w16_gdiobj *dc = target(w, hd, &dt);
    if (dc) {
        int64_t br = brush_of(w, dc->brush);
        if (br >= 0) {
            struct gdi_blit bl = { .dst = &dt, .x = l + 1, .y = t + 1, .w = r - l - 2, .h = b - t - 2,
                                   .rop = 0x00F00021, .brush = (uint32_t)br };
            uint32_t pbuf[64];
            pattern_of(w, dc, dc->brush, &dt, &bl, pbuf);
            gdi_bitblt(&bl);
        }
        int width;
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
    if (t == GDI_BRUSH) free(o->px);
    if (t == GDI_DC) {
        if (o->hwnd || o->screen) return;
        struct w16_gdiobj *bm = obj(w, o->bitmap, GDI_BITMAP);
        if (bm) bm->in_dc = 0;
    }
    memset(o, 0, sizeof *o);
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
        if (pen >= 0) gdi_line(&t, o->cx, o->cy, x, y, (uint32_t)pen, width, o->rop2);
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
// Windows: Draws a rectangle: edged in the pen, filled with the brush.
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
// w16: The same.
static int api_SetPixel(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    struct w16_gdiobj *o;
    uint16_t hdc = w16_arg16(a);
    int16_t x = (int16_t)w16_arg16(a), y = (int16_t)w16_arg16(a);
    uint32_t cr = ordinal == 31 ? w16_arg32(a) : 0;
    struct gdi_target t;
    o = target(w, hdc, &t);
    uint32_t out = 0xFFFFFFFFu;
    int px = x + t.ox, py = y + t.oy;
    if (o && t.px && px >= t.x0 && py >= t.y0 && px < t.x1 && py < t.y1) {
        uint32_t *p = t.px + (size_t)py * t.w + px;
        if (ordinal == 31) *p = t.mono ? (w16_colorref_px(cr) ? 0xFFFFFF : 0) : w16_colorref_px(cr);
        out = (*p & 0xFF) << 16 | (*p & 0xFF00) | (*p >> 16 & 0xFF);   // back to a COLORREF
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
// Windows: How wide and high a string is in the DC's font, as TextOut would draw it.
// w16: The same, from the same font TextOut draws with, so text measures as it draws.
static int api_GetTextExtent(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t hfont = dc_font(w, w16_arg16(a));
    uint32_t far = w16_arg32(a);
    unsigned len = w16_arg16(a);
    char *text = far_text(w, far, len);
    if (!text) return -1;
    int cx, cy;
    w16_text_size_in(hfont, text, &cx, &cy);
    free(text);
    set32(&w->cpu, (uint32_t)(uint16_t)cy << 16 | (uint16_t)cx);
    return 8;
}

// GetTextMetrics(hdc, lptm) -> BOOL
// Windows: The DC's font's measures: height, ascent, descent, leading, average and widest
// character, weight, overhang, pitch and family, character set.
// w16: The same, from the font itself -- Windows 3.11's .FON header when it is that font, so a
// program lays out as it did.
static int api_GetTextMetrics(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t hfont = dc_font(w, w16_arg16(a));
    uint32_t far = w16_arg32(a);
    pthread_mutex_lock(&g_w16_text_lock);
    struct w16_fontinfo f = *w16_font_info(hfont);
    pthread_mutex_unlock(&g_w16_text_lock);
    int16_t words[8] = {
        (int16_t)f.height, (int16_t)f.ascent, (int16_t)(f.height - f.ascent),
        (int16_t)f.internal_leading, (int16_t)f.external_leading,
        (int16_t)f.ave, (int16_t)f.maxw, (int16_t)f.weight,
    };
    uint8_t tm[31] = { 0 };
    memcpy(tm, words, sizeof words);
    tm[16] = (uint8_t)f.italic;
    tm[19] = (uint8_t)f.first;                   // tmFirstChar
    tm[20] = (uint8_t)f.last;                    // tmLastChar
    tm[21] = (uint8_t)f.def;                     // tmDefaultChar
    tm[22] = 0x20;                               // tmBreakChar
    tm[23] = (uint8_t)f.pitch_family;            // bit 0 set: VARIABLE pitch, as in the FNT
    tm[24] = (uint8_t)f.charset;
    tm[27] = tm[29] = 96;                        // tmDigitizedAspectX, Y
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
    return h;
}

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
    int px = x + t.ox, py = y + t.oy;
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

const struct w16_api g_gdi_api[] = {
    { 1, api_SetBkColor },
    { 2, api_SetBkMode },
    { 4, api_SetBkMode },
    { 9, api_SetBkColor },
    { 19, api_LineTo },
    { 20, api_MoveTo },
    { 27, api_Rectangle },
    { 29, api_PatBlt },
    { 31, api_SetPixel },
    { 33, api_TextOut },
    { 34, api_BitBlt },
    { 35, api_StretchBlt },
    { 38, api_Escape },
    { 45, api_SelectObject },
    { 47, api_CombineRgn },
    { 48, api_CreateBitmap },
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
    { 68, api_DeleteDC },
    { 69, api_DeleteDC },
    { 79, api_GetDCOrg },
    { 80, api_GetDeviceCaps },
    { 82, api_GetObject },
    { 83, api_SetPixel },
    { 87, api_GetStockObject },
    { 91, api_GetTextExtent },
    { 93, api_GetTextMetrics },
    { 100, api_LineDDA },
    { 103, api_PtVisible },
    { 148, api_SetBrushOrg },
    { 150, api_UnrealizeObject },
    { 153, api_CreateIC },
    { 172, api_SetRectRgn },
    { 442, api_CreateDIBitmap },
    { 443, api_SetDIBitsToDevice },
};
const unsigned g_gdi_api_n = sizeof g_gdi_api / sizeof g_gdi_api[0];

int w16_gdi(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    return w16_api_call(g_gdi_api, sizeof g_gdi_api / sizeof g_gdi_api[0], w, ordinal, a);
}
