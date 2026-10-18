// gdi16.c -- GDI's exports. Stage 2 has one device context, the window's
// client area, and the calls a first program draws text with, in a
// stand-in for the Windows 3.1 System font (w16_system_font()).
#include "w16_internal.h"

#include <stdlib.h>
#include <string.h>
#include "ui/ugfx.h"

#define STOCK_BASE 0x5000      // GetStockObject(i) -> STOCK_BASE + i
#define TRANSPARENT 1

static uint32_t colorref(uint32_t cr) {
    return ugfx_rgb((uint8_t)cr, (uint8_t)(cr >> 8), (uint8_t)(cr >> 16));
}

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

// A brush: COLOR_* + 1 (what a WNDCLASS names), or a stock brush.
uint32_t w16_brush_color(uint16_t brush) {
    static const uint32_t stock[] = { 0xFFFFFF, 0xC0C0C0, 0x808080, 0x404040, 0x000000 };
    if (brush >= 1 && brush <= 21) return w16_syscolor(brush - 1u);
    if (brush >= STOCK_BASE && brush < STOCK_BASE + 5) {
        uint32_t v = stock[brush - STOCK_BASE];
        return ugfx_rgb((uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v);
    }
    return ugfx_rgb(255, 255, 255);
}

// A fresh DC's state, as BeginPaint hands one out.
void w16_gdi_reset(struct w16 *w) {
    w->text_color = 0x000000;
    w->bk_color = 0xFFFFFF;
    w->bk_mode = 2;
}

static int text_out(struct w16 *w, struct w16_args *a) {
    w16_arg16(a);                                // the DC: there is one
    int16_t x = (int16_t)w16_arg16(a), y = (int16_t)w16_arg16(a);
    uint32_t far = w16_arg32(a);
    unsigned len = w16_arg16(a);
    char text[256];
    if (len >= sizeof text) len = sizeof text - 1;
    if (len) {
        const uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, len, 0);
        if (!p) return -1;
        memcpy(text, p, len);
    }
    text[len] = '\0';
    struct w16_wnd *win = &w->win;
    pthread_mutex_lock(&w->lock);
    if (win->fb) {
        struct ugfx_surface s = ugfx_surface_for_pixels(win->fb, win->cw, win->ch);
        pthread_mutex_lock(&g_w16_text_lock);
        const struct ugfx_font *was = ugfx_set_font(w16_system_font());
        ugfx_draw_string(&s, x, y, text, colorref(w->text_color),
                         w->bk_mode == TRANSPARENT ? UGFX_TRANSPARENT : colorref(w->bk_color));
        ugfx_set_font(was);
        pthread_mutex_unlock(&g_w16_text_lock);
    }
    pthread_mutex_unlock(&w->lock);
    w->cpu.r[R_AX] = 1;
    return 12;
}

int w16_gdi(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    uint32_t old;
    switch (ordinal) {
    case 1:                                      // SetBkColor(hdc, color)
        w16_arg16(a);
        old = w->bk_color;
        w->bk_color = w16_arg32(a);
        c->r[R_AX] = (uint16_t)old;
        c->r[R_DX] = (uint16_t)(old >> 16);
        return 6;
    case 2:                                      // SetBkMode(hdc, mode)
        w16_arg16(a);
        c->r[R_AX] = (uint16_t)w->bk_mode;
        w->bk_mode = w16_arg16(a);
        return 4;
    case 9:                                      // SetTextColor(hdc, color)
        w16_arg16(a);
        old = w->text_color;
        w->text_color = w16_arg32(a);
        c->r[R_AX] = (uint16_t)old;
        c->r[R_DX] = (uint16_t)(old >> 16);
        return 6;
    case 33: return text_out(w, a);
    case 87:                                     // GetStockObject(index)
        c->r[R_AX] = (uint16_t)(STOCK_BASE + w16_arg16(a));
        return 2;
    default:
        return -2;
    }
}
