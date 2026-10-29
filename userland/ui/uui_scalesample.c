// uui_scalesample.h: a window and the taskbar at a display scale.
#include "ui/uui_scalesample.h"
#include "ui/utheme.h"
#include "ui/uui_primitives.h"
#include <string.h>

int uui_scale_recommended(int width_px, int width_mm) {
    if (width_px <= 0) return 100;
    // In percent x 100, rounded to the nearest quarter step.
    long v = width_mm > 0 ? (long)width_px * 2540 * 85 / ((long)width_mm * 96)
                          : (long)width_px * 10000 / 1280;
    int pct = (int)((v + 1250) / 2500 * 25);
    return pct < 100 ? 100 : pct > 200 ? 200 : pct;
}

int uui_scalesample_open(struct uui_scalesample *ss) {
    memset(ss, 0, sizeof *ss);
    const char *face = ugfx_font_session_face();
    ss->have_face = face && uui_fontface_open(&ss->face, UUI_FONT_DIR, face);
    return ss->have_face;
}

void uui_scalesample_close(struct uui_scalesample *ss) {
    if (ss->have_face) uui_fontface_close(&ss->face);
    ss->have_face = 0;
}

// One run of text, its baseline at `base`, cut at the box's right edge.
static void text(struct ugfx_surface *s, struct uui_scalesample *ss, const char *t, int px,
                 int x, int base, int right, uint32_t fg, uint32_t bg) {
    if (x >= right) return;
    if (!ss->have_face) {
        ugfx_draw_string_clipped(s, x, base - ugfx_char_h() * 3 / 4, right - x, t, fg, bg);
        return;
    }
    char fit[64];
    size_t n = strlen(t);
    if (n >= sizeof fit) n = sizeof fit - 1;
    memcpy(fit, t, n);
    fit[n] = 0;
    while (n && x + uglyph_text_width(&ss->face.reg, fit, px) > right) fit[--n] = 0;
    uglyph_text(s, &ss->face.reg, fit, px, x, base, fg);
}

void uui_scalesample_draw(struct ugfx_surface *s, struct uui_scalesample *ss,
                          int x, int y, int w, int h, int scale_pct, int text_px) {
    if (w <= 0 || h <= 0) return;
    // Every measure below is the desktop's at 100%, times the scale --
    // the same numbers ugfx_px() multiplies, so the sample is the result.
#define K(n) (((n) * scale_pct + 50) / 100)
    int px = K(text_px), line = px * 6 / 5;
    struct ugfx_clip was;
    ugfx_clip_save(s, &was);
    ugfx_clip_intersect(s, x, y, w, h);
    uui_fill_round_rect(s, x, y, w, h, utheme_radius(UTHEME_R_CONTROL), ugfx_rgb(47, 110, 134));

    // The window: its top-left corner, running off the box's right.
    int bar = K(48), wx = x + K(16), wy = y + K(12), ww = w, wh = h - bar - K(24);
    int tb = line + K(8), r = K(8);
    uint32_t title = ugfx_rgb(70, 110, 160), white = ugfx_rgb(255, 255, 255);
    uui_fill_round_rect(s, wx - 1, wy - 1, ww + 2, wh + 2, r, ugfx_rgb(60, 60, 60));
    uui_fill_round_rect(s, wx, wy, ww, wh, r, white);
    uui_fill_round_rect(s, wx, wy, ww, tb + r, r, title);
    ugfx_fill_rect(s, wx, wy + tb, ww, r, white);
    int ib = K(16), ix = wx + K(8);
    uui_fill_round_rect(s, ix, wy + (tb - ib) / 2, ib, ib, K(2), white);
    text(s, ss, "todo.txt \xe2\x80\x94 Notepad", px, ix + ib + K(6), wy + (tb + px * 7 / 10) / 2,
         x + w, white, title);
    text(s, ss, "Sauna at 19:00", px, wx + K(8), wy + tb + K(6) + px, x + w, UTHEME_TEXT, white);

    // The taskbar: Start's mark and one labelled button.
    int by = y + h - bar;
    uint32_t strip = ugfx_rgb(30, 30, 34), lit = ugfx_rgb(56, 56, 66), ink = ugfx_rgb(230, 230, 230);
    ugfx_fill_rect(s, x, by, w, bar, strip);
    int dot = K(7), cx = x + K(22), cy = by + bar / 2;
    ugfx_fill_circle(s, cx, cy - dot * 2 / 3, dot / 2 + 1, ugfx_rgb(217, 217, 222));
    ugfx_fill_circle(s, cx - dot * 3 / 4, cy + dot / 2, dot / 2 + 1, ugfx_rgb(217, 217, 222));
    ugfx_fill_circle(s, cx + dot * 3 / 4, cy + dot / 2, dot / 2 + 1, ugfx_rgb(217, 217, 222));
    int bx = x + K(48), bh = K(40), bw = K(140);
    uui_fill_round_rect(s, bx, by + (bar - bh) / 2, bw, bh, K(4), lit);
    int icon = K(24);
    uui_fill_round_rect(s, bx + K(12), by + (bar - icon) / 2, icon * 3 / 4, icon, K(2), white);
    text(s, ss, "todo.txt", px, bx + K(12) + icon + K(8), by + (bar + px * 7 / 10) / 2,
         bx + bw - K(8), ink, lit);
#undef K
    ugfx_clip_restore(s, &was);
}
