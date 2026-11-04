// A box being edited on the picture -- a shape's, a pasted picture's --
// drawn as a dashed frame with eight handles, and the handles found under
// the pointer. One of Paint's units -- see paint.h.
#include "paint.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uapp.h"

float paint_handle_band(void) {
    int z = g_canvas.zoom > 0 ? g_canvas.zoom : 100;
    return (float)PAINT_HANDLE_PX * 100.0f / (float)z;
}

int paint_box_handles(float x0, float y0, float x1, float y1, struct upix_pt *out) {
    float mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
    struct upix_pt h[8] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 },
                            { mx, y0 }, { x1, my }, { mx, y1 }, { x0, my } };
    for (int i = 0; i < 8; i++) out[i] = h[i];
    return 8;
}

int paint_handle_at(const struct upix_pt *h, int n, float x, float y) {
    float b = paint_handle_band() * 0.75f;
    for (int i = 0; i < n; i++)
        if (x >= h[i].x - b && x <= h[i].x + b && y >= h[i].y - b && y <= h[i].y + b) return i;
    return -1;
}

int paint_handle_cursor(int h) {
    return h >= 4 && h % 2 ? WIN_CURSOR_RESIZE_H : h >= 4 ? WIN_CURSOR_RESIZE_V : WIN_CURSOR_MOVE;
}

void paint_draw_frame(struct ugfx_surface *s, const struct uui_paintcanvas *c, float x0, float y0, float x1, float y1) {
    int sx0, sy0, sx1, sy1;
    uui_paintcanvas_to_screen(c, (int)x0, (int)y0, &sx0, &sy0);
    uui_paintcanvas_to_screen(c, (int)(x1 + 0.999f), (int)(y1 + 0.999f), &sx1, &sy1);
    for (int i = sx0; i < sx1; i += 6) {
        ugfx_fill_rect(s, i, sy0 - 1, i + 3 < sx1 ? 3 : sx1 - i, 1, UTHEME_ACCENT);
        ugfx_fill_rect(s, i, sy1, i + 3 < sx1 ? 3 : sx1 - i, 1, UTHEME_ACCENT);
    }
    for (int i = sy0; i < sy1; i += 6) {
        ugfx_fill_rect(s, sx0 - 1, i, 1, i + 3 < sy1 ? 3 : sy1 - i, UTHEME_ACCENT);
        ugfx_fill_rect(s, sx1, i, 1, i + 3 < sy1 ? 3 : sy1 - i, UTHEME_ACCENT);
    }
}

void paint_draw_handles(struct ugfx_surface *s, const struct uui_paintcanvas *c, const struct upix_pt *h, int n) {
    for (int i = 0; i < n; i++) {
        int hx, hy;
        uui_paintcanvas_to_screen(c, (int)h[i].x, (int)h[i].y, &hx, &hy);
        hx -= PAINT_HANDLE_PX / 2;
        hy -= PAINT_HANDLE_PX / 2;
        ugfx_fill_rect(s, hx, hy, PAINT_HANDLE_PX, PAINT_HANDLE_PX, UTHEME_ACCENT);
        ugfx_fill_rect(s, hx + 1, hy + 1, PAINT_HANDLE_PX - 2, PAINT_HANDLE_PX - 2, UTHEME_WHITE);
    }
}

void paint_log_handles(const struct uui_paintcanvas *c, const char *what, const struct upix_pt *h, int n) {
    for (int i = 0; i < n; i++) {
        int hx, hy;
        uui_paintcanvas_to_screen(c, (int)h[i].x, (int)h[i].y, &hx, &hy);
        uapp_logf_layout("paint: layout %s.handle %d %d %d %d %d\n", what, i, hx - PAINT_HANDLE_PX / 2,
                         hy - PAINT_HANDLE_PX / 2, PAINT_HANDLE_PX, PAINT_HANDLE_PX);
    }
}
