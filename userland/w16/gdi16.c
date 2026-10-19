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

// THE SYSTEM FONT AT A FIXED SIZE. A Windows program lays its window out
// for the 3.1 System font's metrics (a 16-pixel cell), so the face does
// not follow the desktop's font size: at 1080p that would overflow every
// window a program sized for itself. Liberation Sans Bold at 13 px is
// about as wide. Loaded once; the caller holds g_w16_text_lock.
#define SYSTEM_FONT_PX 13
const struct ugfx_font *w16_system_font(void) {
    static struct ugfx_font font;
    static int state;                            // 0 not tried, 1 loaded, -1 failed
    if (!state) {
        unsigned long need = ugfx_font_arena_size(SYSTEM_FONT_PX);
        void *arena = malloc(need);
        state = arena && ugfx_font_load("/usr/share/fonts/liberation-sans-bold.ttf",
                                        SYSTEM_FONT_PX, 1, &font, arena, need) ? 1 : -1;
        if (state < 0) free(arena);
    }
    return state > 0 ? &font : ugfx_font_session(UGFX_FONT_BOLD);
}

// THE FIXED-PITCH STOCK FONTS (OEM_FIXED_FONT, ANSI_FIXED_FONT,
// SYSTEM_FIXED_FONT) are DejaVu Sans Mono Bold at the same 13 px: about
// Fixedsys's 8 by 15, which is what Notepad shows its text in.
#define OEM_FIXED_FONT    10
#define ANSI_FIXED_FONT   11
#define SYSTEM_FIXED_FONT 16
const struct ugfx_font *w16_fixed_font(void) {
    static struct ugfx_font font;
    static int state;                            // 0 not tried, 1 loaded, -1 failed
    if (!state) {
        unsigned long need = ugfx_font_arena_size(SYSTEM_FONT_PX);
        void *arena = malloc(need);
        state = arena && ugfx_font_load("/usr/share/fonts/dejavu-sans-mono-bold.ttf",
                                        SYSTEM_FONT_PX, 1, &font, arena, need) ? 1 : -1;
        if (state < 0) free(arena);
    }
    return state > 0 ? &font : w16_system_font();
}

int w16_font_is_fixed(uint16_t hfont) {
    return hfont == STOCK_BASE + OEM_FIXED_FONT || hfont == STOCK_BASE + ANSI_FIXED_FONT ||
           hfont == STOCK_BASE + SYSTEM_FIXED_FONT;
}

const struct ugfx_font *w16_font_of(uint16_t hfont) {
    return w16_font_is_fixed(hfont) ? w16_fixed_font() : w16_system_font();
}

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

void w16_gdi_forget_window(struct w16 *w, uint16_t hwnd) {
    struct w16_wnd *x = w16_wnd_get(w, hwnd);
    struct w16_gdiobj *dc = x ? obj(w, x->dc, GDI_DC) : 0;
    if (dc && dc->hwnd == hwnd) memset(dc, 0, sizeof *dc);
    if (x) x->dc = 0;
}

void w16_gdi_flush(struct w16 *w) {
    if (!w->fb_dirty) return;
    w->fb_dirty = 0;
    w16_task_post(w, W16_POST_PAINT);
}

// --- colours -----------------------------------------------------------------

static const uint32_t g_stock_brush[] = { 0xFFFFFF, 0xC0C0C0, 0x808080, 0x404040, 0x000000 };

// -1 for "draw nothing" (a NULL brush or pen).
static int64_t brush_of(struct w16 *w, uint16_t h) {
    if (h >= STOCK_BASE && h < STOCK_BASE + NULL_BRUSH) return g_stock_brush[h - STOCK_BASE];
    if (h == STOCK_BASE + NULL_BRUSH || h == 0) return -1;
    if (h >= 1 && h <= 21) return w16_syscolor(h - 1u);         // COLOR_* + 1
    struct w16_gdiobj *b = obj(w, h, GDI_BRUSH);
    if (!b || b->style == BS_NULL) return b ? -1 : 0xFFFFFF;
    return w16_colorref_px(b->color);
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
    struct w16_gdiobj *bm = obj(w, dc->bitmap, GDI_BITMAP);
    if (bm) { t->px = bm->px; t->w = bm->bw; t->h = bm->bh; t->mono = bm->mono; }
    else { t->px = &g_scratch_px; t->w = t->h = 1; t->mono = 1; }
    gdi_target_whole(t);
}

// The DC and its pixels; a window DC holds `lock` until gdi_done().
static struct w16_gdiobj *target(struct w16 *w, uint16_t hdc, struct gdi_target *t) {
    struct w16_gdiobj *dc = obj(w, hdc, GDI_DC);
    memset(t, 0, sizeof *t);
    if (!dc) return 0;
    if (dc->hwnd) pthread_mutex_lock(&w->lock);
    fill_target(w, dc, t);
    return dc;
}

static void gdi_done(struct w16 *w, struct w16_gdiobj *dc) {
    if (dc && dc->hwnd) {
        w->fb_dirty = 1;
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
static int draw_text(struct w16 *w, uint16_t hdc, int x, int y, const char *text, int tab) {
    struct gdi_target t;
    struct w16_gdiobj *dc = target(w, hdc, &t);
    int width = 0;
    pthread_mutex_lock(&g_w16_text_lock);
    const struct ugfx_font *was = ugfx_set_font(w16_font_of(dc ? dc->font : 0));
    if (dc && t.px) {
        struct ugfx_surface s = ugfx_surface_for_pixels(t.px, t.w, t.h);
        ugfx_set_clip_rect(&s, t.x0, t.y0, t.x1 - t.x0, t.y1 - t.y0);
        uint32_t bg = dc->bk_mode == TRANSPARENT ? UGFX_TRANSPARENT : w16_colorref_px(dc->bk);
        for (const char *p = text; *p; ) {
            size_t run = tab > 0 ? strcspn(p, "\t") : strlen(p);
            char *piece = strndup(p, run);
            if (piece) ugfx_draw_string(&s, x + t.ox + width, y + t.oy, piece, w16_colorref_px(dc->text), bg);
            width += piece ? ugfx_text_width(piece) : 0;
            free(piece);
            p += run;
            if (*p == '\t') { width = (width / tab + 1) * tab; p++; }
        }
    }
    ugfx_set_font(was);
    pthread_mutex_unlock(&g_w16_text_lock);
    gdi_done(w, dc);
    return dc ? width : -1;
}

int w16_gdi_text(struct w16 *w, uint16_t hdc, int x, int y, const char *text, int tab) {
    return draw_text(w, hdc, x, y, text, tab);
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
    if (ddc) {
        int64_t br = brush_of(w, ddc->brush);
        struct gdi_blit b = {
            .dst = &dt, .src = sp, .x = x, .y = y, .w = cw, .h = ch, .sx = sx, .sy = sy,
            .rop = rop, .brush = br < 0 ? 0xFFFFFF : (uint32_t)br,
            .dst_text = w16_colorref_px(ddc->text), .dst_bk = w16_colorref_px(ddc->bk),
            .src_bk = sdc ? w16_colorref_px(sdc->bk) : 0xFFFFFF,
        };
        gdi_bitblt(&b);
    }
    if (ddc && ddc->hwnd) w->fb_dirty = 1;
    if (locked) pthread_mutex_unlock(&w->lock);
    w->cpu.r[R_AX] = ddc != 0;
    return 20;
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
        gdi_bitblt(&bl);
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
        if (dc->hwnd) return 0;
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
    if (t == GDI_DC) {
        if (o->hwnd) return;
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

// SetBkColor, SetTextColor(hdc, color)
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

// SetBkMode, SetROP2(hdc, mode)
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

// LineTo(hdc, x, y)
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

// MoveTo(hdc, x, y) -> the old position
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

// Rectangle
static int api_Rectangle(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return rectangle(w, a);
}

// PatBlt
static int api_PatBlt(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return pat_blt(w, a);
}

// SetPixel(hdc, x, y, color), GetPixel(hdc, x, y)
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
    if (o && o->hwnd && ordinal == 83) pthread_mutex_unlock(&w->lock);
    else gdi_done(w, o);
    set32(c, out);
    return ordinal == 31 ? 10 : 6;
}

// TextOut
static int api_TextOut(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return text_out(w, a);
}

// BitBlt
static int api_BitBlt(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return bit_blt(w, a);
}

// SelectObject(hdc, h)
static int api_SelectObject(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint16_t hdc = w16_arg16(a);
    c->r[R_AX] = select_object(w, hdc, w16_arg16(a));
    return 4;
}

// CreateBitmap(w, h, planes, bpp, lpBits)
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

// CreateCompatibleBitmap(hdc, w, h)
static int api_CreateCompatibleBitmap(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    struct w16_gdiobj *o;
    uint16_t h;
    (void)ordinal;
    uint16_t hdc = w16_arg16(a);
    int16_t bw = (int16_t)w16_arg16(a), bh = (int16_t)w16_arg16(a);
    o = obj(w, hdc, GDI_DC);
    struct w16_gdiobj *src = o ? obj(w, o->bitmap, GDI_BITMAP) : 0;
    int mono = o && !o->hwnd && (!src || src->mono);   // a memory DC's own bitmap decides
    new_bitmap(w, bw, bh, mono, &h);
    c->r[R_AX] = h;
    return 6;
}

// CreateCompatibleDC(hdc)
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

// CreatePen(style, width, color)
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

// CreateSolidBrush(color)
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

// GetTextExtent(hdc, lpString, nCount) -> MAKELONG(cx, cy)
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

// GetTextMetrics(hdc, lpMetrics): the system font's, as a 31-byte TEXTMETRIC
static int api_GetTextMetrics(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t hfont = dc_font(w, w16_arg16(a));
    uint32_t far = w16_arg32(a);
    int ave, cy, maxw;
    w16_text_size_in(hfont, "x", &ave, &cy);
    w16_text_size_in(hfont, "W", &maxw, &cy);
    int16_t words[8] = {
        (int16_t)cy, (int16_t)(cy * 13 / 16), (int16_t)(cy - cy * 13 / 16), 0, 0,   // height, ascent, descent, leadings
        (int16_t)ave, (int16_t)maxw, 700,                                          // widths, FW_BOLD
    };
    uint8_t tm[31] = { 0 };
    memcpy(tm, words, sizeof words);
    tm[19] = 0x20;                               // tmFirstChar
    tm[20] = 0xFF;                               // tmLastChar
    tm[21] = 0x80;                               // tmDefaultChar
    tm[22] = 0x20;                               // tmBreakChar
    tm[23] = w16_font_is_fixed(hfont) ? 0x30 : 0x22;   // tmPitchAndFamily: FF_MODERN fixed, or FF_SWISS variable
    tm[24] = 0;                                  // ANSI_CHARSET
    tm[27] = tm[29] = 96;                        // tmDigitizedAspectX, Y
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, sizeof tm, 1);
    if (!p) return -1;
    memcpy(p, tm, sizeof tm);
    w->cpu.r[R_AX] = 1;
    return 6;
}

// CreateDC(driver, device, output, lpInitData): only a printer asks this
// way, and there is no printer -- the program is told so as Windows did.
static int api_CreateDC(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg32(a); w16_arg32(a); w16_arg32(a); w16_arg32(a);
    w->cpu.r[R_AX] = 0;
    return 16;
}

// Escape(hdc, nEscape, cbInput, lpIn, lpOut): a printer's; none here
static int api_Escape(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a); w16_arg16(a); w16_arg16(a); w16_arg32(a); w16_arg32(a);
    w->cpu.r[R_AX] = 0;
    return 14;
}

// DeleteDC(hdc), DeleteObject(h)
static int api_DeleteDC(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    delete_object(w, w16_arg16(a));
    c->r[R_AX] = 1;
    return 2;
}

// GetDeviceCaps(hdc, index)
static int api_GetDeviceCaps(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    c->r[R_AX] = device_caps(w16_arg16(a));
    return 4;
}

// GetStockObject(index)
static int api_GetStockObject(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    c->r[R_AX] = (uint16_t)(STOCK_BASE + w16_arg16(a));
    return 2;
}

// CreateDIBitmap
static int api_CreateDIBitmap(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return create_dibitmap(w, a);
}

// SetDIBitsToDevice
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
    { 38, api_Escape },
    { 45, api_SelectObject },
    { 48, api_CreateBitmap },
    { 51, api_CreateCompatibleBitmap },
    { 52, api_CreateCompatibleDC },
    { 53, api_CreateDC },
    { 61, api_CreatePen },
    { 66, api_CreateSolidBrush },
    { 68, api_DeleteDC },
    { 69, api_DeleteDC },
    { 80, api_GetDeviceCaps },
    { 83, api_SetPixel },
    { 87, api_GetStockObject },
    { 91, api_GetTextExtent },
    { 93, api_GetTextMetrics },
    { 442, api_CreateDIBitmap },
    { 443, api_SetDIBitsToDevice },
};
const unsigned g_gdi_api_n = sizeof g_gdi_api / sizeof g_gdi_api[0];

int w16_gdi(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    return w16_api_call(g_gdi_api, sizeof g_gdi_api / sizeof g_gdi_api[0], w, ordinal, a);
}
