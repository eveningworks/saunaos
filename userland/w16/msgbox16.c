// msgbox16.c -- a Windows 3.1 message box, drawn by the MAIN thread into a
// toolkit popup centred on the window: the navy caption, the red STOP
// sign, the text wrapped beside it, and one bevelled button with its
// dotted focus ring. The Application Error box (a fault, an unimplemented
// call) is its first user; MessageBox() is the next.
//
// A GRABBING popup, so that its button can be pressed: a press in another
// program dismisses it, which the caller treats as the button.
#include "w16_internal.h"

#include <stdlib.h>
#include <string.h>
#include "ui/ugfx.h"
#include "ui/uui_label.h"
#include "ui/uui_popup.h"

#define BOX_W   380
#define ICON    32

static uint32_t rgb(uint32_t v) { return ugfx_rgb((uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v); }

static int margin(void) { return 16; }
static int text_x(void) { return margin() + ICON + 14; }
static int text_w(void) { return BOX_W - text_x() - margin(); }

// Lines of `text` at the box's width, '\n' breaking as well; the system
// font must be current.
static int wrap_lines(const char *text, char lines[][96], int max) {
    int n = 0;
    char para[sizeof ((struct w16_msgbox *)0)->text];
    const char *p = text;
    while (*p && n < max) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= sizeof para) len = sizeof para - 1;
        memcpy(para, p, len);
        para[len] = 0;
        if (!para[0]) lines[n++][0] = 0;
        for (const char *r = para; r && *r && n < max; )
            r = uui_label_wrap_next(r, text_w(), lines[n++], 96);
        p = nl ? nl + 1 : p + len;
    }
    return n;
}

#define MAX_LINES 12

// Dismissed by the compositor -- a press in another program: an answer.
static void dismissed(void *owner) {
    struct w16_msgbox *b = owner;
    b->popup = 0;
    if (b->on_dismiss) b->on_dismiss();
}

void w16_msgbox_open(struct w16_msgbox *b, int win_w, int win_h) {
    if (b->popup) return;
    pthread_mutex_lock(&g_w16_text_lock);
    const struct ugfx_font *was = ugfx_set_font(w16_system_font());
    char lines[MAX_LINES][96];
    int n = wrap_lines(b->text, lines, MAX_LINES), ch = ugfx_char_h();
    ugfx_set_font(was);
    pthread_mutex_unlock(&g_w16_text_lock);

    int body = n * ch > ICON ? n * ch : ICON;
    b->btn_w = 76;
    b->btn_h = 24;
    b->w = BOX_W;
    b->h = W16_CAPTION + margin() + body + margin() + b->btn_h + margin();
    int ax = (win_w - b->w) / 2, ay = (win_h - b->h) / 2;
    int x = ax, y = ay;
    b->popup = uui_popup_open(w16_up(ax), w16_up(ay - 1), w16_up(b->w), w16_up(1), w16_up(b->w),
                              w16_up(b->h), UUI_POPUP_BELOW, UUI_POPUP_GRAB | UUI_POPUP_NO_DECOR,
                              dismissed, b, &x, &y);
    b->x = w16_down(x);
    b->y = w16_down(y);
    b->btn_x = (b->w - b->btn_w) / 2;
    b->btn_y = b->h - margin() - b->btn_h;
    b->pressed = 0;
}

void w16_msgbox_close(struct w16_msgbox *b) {
    if (b->popup > 0) uui_popup_close(b->popup);
    b->popup = 0;
}

void w16_stop_sign(struct ugfx_surface *s, int x, int y) {
    // An octagon, its corners cut a third of the way along each edge.
    int c = ICON * 3 / 10;
    for (int r = 0; r < ICON; r++) {
        int inset = r < c ? c - r : r >= ICON - c ? r - (ICON - c - 1) : 0;
        ugfx_fill_rect(s, x + inset, y + r, ICON - 2 * inset, 1, rgb(0xFF0000));
    }
    const char *t = "STOP";
    if (ugfx_text_width(t) <= ICON - 4)
        ugfx_draw_string(s, x + (ICON - ugfx_text_width(t)) / 2, y + (ICON - ugfx_char_h()) / 2, t,
                         rgb(0xFFFFFF), UGFX_TRANSPARENT);
}

static void button(struct ugfx_surface *s, const struct w16_msgbox *b, const char *label) {
    int x = b->btn_x, y = b->btn_y, w = b->btn_w, h = b->btn_h;
    uint32_t black = rgb(0x000000), hi = rgb(0xFFFFFF), lo = rgb(0x808080);
    ugfx_fill_rect(s, x, y, w, h, rgb(0xC0C0C0));
    ugfx_draw_rect(s, x, y, w, h, black);
    int sh = b->pressed ? 1 : 0;
    if (b->pressed) {
        ugfx_fill_rect(s, x + 1, y + 1, w - 2, 1, lo);
        ugfx_fill_rect(s, x + 1, y + 1, 1, h - 2, lo);
    } else {
        ugfx_fill_rect(s, x + 1, y + 1, w - 3, 1, hi);
        ugfx_fill_rect(s, x + 1, y + 1, 1, h - 3, hi);
        ugfx_fill_rect(s, x + 1, y + h - 3, w - 2, 2, lo);
        ugfx_fill_rect(s, x + w - 3, y + 1, 2, h - 2, lo);
    }
    int tw = ugfx_text_width(label), tx = x + (w - tw) / 2 + sh, ty = y + (h - ugfx_char_h()) / 2 + sh;
    ugfx_draw_string(s, tx, ty, label, black, UGFX_TRANSPARENT);
    // The focus ring: every other pixel, around the label.
    for (int i = tx - 3; i < tx + tw + 3; i += 2) {
        ugfx_fill_rect(s, i, ty - 1, 1, 1, black);
        ugfx_fill_rect(s, i, ty + ugfx_char_h(), 1, 1, black);
    }
    for (int j = ty - 1; j <= ty + ugfx_char_h(); j += 2) {
        ugfx_fill_rect(s, tx - 3, j, 1, 1, black);
        ugfx_fill_rect(s, tx + tw + 2, j, 1, 1, black);
    }
}

static void draw_box(struct w16_msgbox *b, struct ugfx_surface *s);

void w16_msgbox_draw(struct w16_msgbox *b) {
    if (b->popup <= 0) return;
    struct ugfx_surface *s = uui_popup_surface(b->popup);
    if (!s) return;
    if (g_w16_q == 4) { draw_box(b, s); return; }
    uint32_t *px = malloc((size_t)b->w * b->h * 4);
    if (!px) return;
    struct ugfx_surface t = ugfx_surface_for_pixels(px, b->w, b->h);
    draw_box(b, &t);
    w16_scale_blit(s, 0, 0, px, b->w, b->h, b->w);
    free(px);
}

static void draw_box(struct w16_msgbox *b, struct ugfx_surface *s) {
    uint32_t black = rgb(0x000000), white = rgb(0xFFFFFF);
    ugfx_fill_rect(s, 0, 0, b->w, b->h, white);
    ugfx_draw_rect(s, 0, 0, b->w, b->h, black);
    int ch_h = W16_CAPTION - 1;
    ugfx_fill_rect(s, 1, 1, b->w - 2, ch_h - 1, rgb(0x000080));
    ugfx_fill_rect(s, 1, ch_h, b->w - 2, 1, black);

    pthread_mutex_lock(&g_w16_text_lock);
    const struct ugfx_font *was = ugfx_set_font(w16_system_font());
    int cw = ugfx_text_width(b->caption);
    ugfx_draw_string_clipped(s, (b->w - cw) / 2 > 4 ? (b->w - cw) / 2 : 4, 1 + (ch_h - 1 - ugfx_char_h()) / 2,
                             b->w - 8, b->caption, white, UGFX_TRANSPARENT);
    int top = W16_CAPTION + margin();
    w16_stop_sign(s, margin(), top);
    char lines[MAX_LINES][96];
    int n = wrap_lines(b->text, lines, MAX_LINES);
    for (int i = 0; i < n; i++)
        ugfx_draw_string_clipped(s, text_x(), top + i * ugfx_char_h(), text_w(), lines[i], black,
                                 UGFX_TRANSPARENT);
    button(s, b, "Close");
    ugfx_set_font(was);
    pthread_mutex_unlock(&g_w16_text_lock);
}

static int on_button(const struct w16_msgbox *b, int x, int y) {
    x -= b->x;
    y -= b->y;
    return x >= b->btn_x && x < b->btn_x + b->btn_w && y >= b->btn_y && y < b->btn_y + b->btn_h;
}

int w16_msgbox_press(struct w16_msgbox *b, int x, int y) {
    if (b->popup <= 0) return 0;
    b->pressed = on_button(b, x, y);
    return 0;
}

int w16_msgbox_release(struct w16_msgbox *b, int x, int y) {
    if (b->popup <= 0) return 0;
    int hit = b->pressed && on_button(b, x, y);
    b->pressed = 0;
    return hit;
}

int w16_msgbox_key(struct w16_msgbox *b, int key) {
    return b->popup > 0 && (key == '\n' || key == '\r' || key == 0x1B || key == ' ');
}
