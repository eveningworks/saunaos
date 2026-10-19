// ctl16.c -- w16's own window classes, as USER had them built in: BUTTON
// (push buttons, check boxes, radio buttons, group boxes) and STATIC
// (labels, icons, rectangles), plus how any control draws in the 3.1 look.
// EDIT is edit16.c's; a dialog's own class (#32770) is dialog16.c's.
//
// A CONTROL IS A WINDOW LIKE ANY OTHER: a program reaches it with
// SendMessage (BM_SETCHECK, WM_SETTEXT), it paints on WM_PAINT into its
// top-level window's buffer, and it tells its parent what happened with
// WM_COMMAND -- so a dialog of the program's and a window of w16's own
// use one implementation.
#include "w16_internal.h"

#include <stdlib.h>
#include <string.h>
#include "keyboard.h"
#include "win_proto.h"   // WIN_CURSOR_*
#include "ui/ugfx.h"

#define BS_TYPE           0x0F
#define BS_PUSHBUTTON     0x00
#define BS_DEFPUSHBUTTON  0x01
#define BS_CHECKBOX       0x02
#define BS_AUTOCHECKBOX   0x03
#define BS_RADIOBUTTON    0x04
#define BS_3STATE         0x05
#define BS_AUTO3STATE     0x06
#define BS_GROUPBOX       0x07
#define BS_AUTORADIOBUTTON 0x09
#define BS_LEFTTEXT       0x20
#define BM_GETCHECK       (WM_USER + 0)
#define BM_SETCHECK       (WM_USER + 1)
#define BM_GETSTATE       (WM_USER + 2)
#define BM_SETSTATE       (WM_USER + 3)
#define BM_SETSTYLE       (WM_USER + 4)
#define BN_CLICKED        0

#define SS_TYPE           0x0F
#define SS_CENTER         0x01
#define SS_RIGHT          0x02
#define SS_ICON           0x03
#define SS_BLACKRECT      0x04
#define SS_GRAYRECT       0x05
#define SS_WHITERECT      0x06
#define SS_BLACKFRAME     0x07
#define SS_GRAYFRAME      0x08
#define SS_WHITEFRAME     0x09
#define SS_SIMPLE         0x0B
#define SS_LEFTNOWORDWRAP 0x0C
#define SS_NOPREFIX       0x80

#define CS_DBLCLKS        0x0008
#define COLOR_WINDOW      5

// THE COLOURS 3.11 SHOWED AT VGA. Its driver programs the 16-colour
// palette, so navy, the face grey and the shadow grey reach the screen
// shifted -- measured off the reference (docs/w16-design.md). The other
// dark colours shift too, but which nominal became which is not
// established, so they pass through.
uint32_t w16_vga(uint32_t v) {
    switch (v & 0xFFFFFF) {
    case 0x000080: return 0x0000A8;
    case 0xC0C0C0: return 0xC0C7C8;
    case 0x808080: return 0x87888F;
    default:       return v & 0xFFFFFF;
    }
}

uint32_t w16_rgb(uint32_t v) {
    v = w16_vga(v);
    return ugfx_rgb((uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v);
}

// --- drawing, shared by every control (the caller holds the text lock) -----------

void w16_draw_bevel(struct ugfx_surface *s, int x, int y, int w, int h, int pressed) {
    uint32_t face = w16_rgb(0xC0C0C0), hi = w16_rgb(0xFFFFFF), lo = w16_rgb(0x808080);
    ugfx_fill_rect(s, x, y, w, h, face);
    if (pressed) {
        ugfx_fill_rect(s, x, y, w, 1, lo);
        ugfx_fill_rect(s, x, y, 1, h, lo);
        return;
    }
    // 3.11's: white along the top and left, then a shadow two deep -- the
    // inner line stops at the white, the outer runs the whole edge.
    ugfx_fill_rect(s, x, y, w - 1, 1, hi);
    ugfx_fill_rect(s, x, y, 1, h - 1, hi);
    ugfx_fill_rect(s, x + 1, y + h - 2, w - 1, 1, lo);
    ugfx_fill_rect(s, x + w - 2, y + 1, 1, h - 1, lo);
    ugfx_fill_rect(s, x, y + h - 1, w, 1, lo);
    ugfx_fill_rect(s, x + w - 1, y, 1, h, lo);
}

// `t` without its '&' markers, allocated, and where the marked letter falls.
static char *strip_amp(const char *t, int prefix, int *mark) {
    char *out = malloc(strlen(t) + 1);
    size_t n = 0;
    *mark = -1;
    if (!out) return 0;
    for (; *t; t++) {
        if (prefix && *t == '&' && t[1] != '&' && t[1]) { *mark = (int)n; continue; }
        if (prefix && *t == '&' && t[1] == '&') t++;
        out[n++] = *t;
    }
    out[n] = 0;
    return out;
}

static void underline(struct ugfx_surface *s, int x, int y, const char *line, int mark, uint32_t fg) {
    if (mark < 0 || !line[mark]) return;
    char ch[2] = { line[mark], 0 };
    ugfx_fill_rect(s, x + ugfx_text_width_n(line, mark), y + ugfx_char_h() - 2, ugfx_text_width(ch), 1, fg);
}

void w16_draw_label(struct ugfx_surface *s, int x, int y, int w, const char *text, int prefix,
                    uint32_t fg, uint32_t bg) {
    int mark;
    char *line = strip_amp(text, prefix, &mark);
    if (!line) return;
    ugfx_draw_string_clipped(s, x, y, w, line, fg, bg);
    underline(s, x, y, line, mark, fg);
    free(line);
}

void w16_draw_focus(struct ugfx_surface *s, int x, int y, int w, int h) {
    uint32_t c = w16_rgb(0x000000);
    for (int i = 0; i < w; i += 2) { ugfx_fill_rect(s, x + i, y, 1, 1, c); ugfx_fill_rect(s, x + i, y + h - 1, 1, 1, c); }
    for (int i = 0; i < h; i += 2) { ugfx_fill_rect(s, x, y + i, 1, 1, c); ugfx_fill_rect(s, x + w - 1, y + i, 1, 1, c); }
}

void w16_draw_icon(struct ugfx_surface *s, struct w16 *w, int kind, int x, int y) {
    if (kind == 1) { w16_stop_sign(s, x, y); return; }
    if (kind == 0xFF) {
        static uint32_t px[32 * 32];
        static int iw = -1, ih;
        if (iw < 0 && w16_icon(&w->ne, px, &iw, &ih) < 0) iw = 0;
        if (iw > 0) ugfx_blit_alpha(s, x + (32 - iw) / 2, y + (32 - ih) / 2, iw, ih, px, iw);
        return;
    }
    uint32_t disc = kind == 3 ? w16_rgb(0xFFFF00) : w16_rgb(0xFFFFFF);
    uint32_t ink = kind == 3 ? w16_rgb(0x000000) : w16_rgb(0x000080);
    ugfx_fill_circle(s, x + 16, y + 16, 15, w16_rgb(0x000000));
    ugfx_fill_circle(s, x + 16, y + 16, 14, disc);
    const char *g = kind == 2 ? "?" : kind == 3 ? "!" : "i";
    ugfx_draw_string(s, x + 16 - ugfx_text_width(g) / 2, y + 16 - ugfx_char_h() / 2, g, ink, disc);
}

// Text wrapped at words inside (x, y, w, h), each line placed by `align`:
// 0 left, 1 centred, 2 right. '\n' breaks a line; `wrap` 0 keeps one.
static void text_box(struct ugfx_surface *s, int x, int y, int w, int h, const char *text, int align,
                     int prefix, int wrap, uint32_t fg, uint32_t bg) {
    int mark;
    char *buf = strip_amp(text, prefix, &mark);
    if (!buf) return;
    int lh = ugfx_char_h(), at = 0;
    const char *p = buf;
    for (int ly = y; *p && ly + lh <= y + h + lh / 2; ly += lh) {
        // As many words as fit, or one cut short when none does.
        int end = 0, last_fit = 0;
        while (p[end] && p[end] != '\n') {
            int e = end;
            while (p[e] == ' ') e++;
            while (p[e] && p[e] != ' ' && p[e] != '\n') e++;
            int tw = ugfx_text_width_n(p, e);
            if (wrap && tw > w && last_fit) break;
            last_fit = end = e;
            if (wrap && tw > w) break;
        }
        int lw = ugfx_text_width_n(p, end), lx = align == 1 ? x + (w - lw) / 2 : align == 2 ? x + w - lw : x;
        char *line = strndup(p, (size_t)end);
        if (!line) break;
        if (lx < x) lx = x;
        ugfx_draw_string_clipped(s, lx, ly, w, line, fg, bg);
        underline(s, lx, ly, line, mark >= at && mark < at + end ? mark - at : -1, fg);
        free(line);
        at += end;
        p += end;
        while (*p == ' ') { p++; at++; }
        if (*p == '\n') { p++; at++; }
        if (!wrap) break;
    }
    free(buf);
}

// The colour a control's parent paints under it: a dialog's white, else
// the parent's class background.
uint32_t w16_parent_bg(struct w16 *w, struct w16_wnd *x) {
    struct w16_wnd *p = w16_wnd_get(w, x->parent);
    if (!p || p->kind == W16_K_DIALOG || p->cls < 0 || p->cls >= w->ncls) return w16_syscolor(COLOR_WINDOW);
    return w16_brush_px(w, w->cls[p->cls].background);
}

void w16_ctl_paint(struct w16 *w, struct w16_wnd *x,
                   void (*fn)(struct w16 *, struct w16_wnd *, struct ugfx_surface *, int, int)) {
    struct ugfx_surface s;
    int ox, oy;
    w16_paint_begin(w, x);
    if (!w16_wnd_paint(w, x, &s, &ox, &oy)) return;
    pthread_mutex_lock(&g_w16_text_lock);
    const struct ugfx_font *was = ugfx_set_font(w16_font_of(x->font));
    fn(w, x, &s, ox, oy);
    ugfx_set_font(was);
    pthread_mutex_unlock(&g_w16_text_lock);
    w16_wnd_paint_done(w);
}

// --- BUTTON ---------------------------------------------------------------------

struct button {
    int pressed, checked, focused, captured;
};

static struct button *btn(struct w16_wnd *x) {
    if (!x->ctl) x->ctl = calloc(1, sizeof(struct button));
    return x->ctl;
}

static int is_push(const struct w16_wnd *x) {
    unsigned t = x->style & BS_TYPE;
    return t == BS_PUSHBUTTON || t == BS_DEFPUSHBUTTON;
}

static int is_radio(const struct w16_wnd *x) {
    unsigned t = x->style & BS_TYPE;
    return t == BS_RADIOBUTTON || t == BS_AUTORADIOBUTTON;
}

// A 3.1 push button: black edge with its corners cut, a grey face raised
// by a white and a dark-grey bevel, sunk while held. The default -- or a
// focused button -- has the edge doubled.
static void paint_push(struct w16 *w, struct w16_wnd *x, struct button *b, struct ugfx_surface *s,
                       int x0, int y0) {
    int bw = x->cw, bh = x->ch;
    uint32_t black = w16_rgb(0x000000), bg = w16_parent_bg(w, x);
    ugfx_fill_rect(s, x0, y0, bw, bh, bg);
    ugfx_fill_rect(s, x0 + 1, y0, bw - 2, 1, black);
    ugfx_fill_rect(s, x0 + 1, y0 + bh - 1, bw - 2, 1, black);
    ugfx_fill_rect(s, x0, y0 + 1, 1, bh - 2, black);
    ugfx_fill_rect(s, x0 + bw - 1, y0 + 1, 1, bh - 2, black);
    int thick = (x->style & BS_TYPE) == BS_DEFPUSHBUTTON, t = thick ? 2 : 1;
    if (thick) ugfx_draw_rect(s, x0 + 1, y0 + 1, bw - 2, bh - 2, black);
    w16_draw_bevel(s, x0 + t, y0 + t, bw - 2 * t, bh - 2 * t, b->pressed);
    int mark;
    char *buf = strip_amp(x->text ? x->text : "", 1, &mark);
    if (!buf) return;
    int tw = ugfx_text_width(buf), lh = ugfx_char_h(), sh = b->pressed ? 1 : 0;
    int tx = x0 + (bw - tw) / 2 + sh, ty = y0 + (bh - lh) / 2 + sh;
    uint32_t fg = (x->style & WS_DISABLED) ? w16_rgb(0x808080) : black;
    ugfx_draw_string_clipped(s, tx, ty, bw - 4, buf, fg, w16_rgb(0xC0C0C0));
    underline(s, tx, ty, buf, mark, fg);
    if (b->focused) w16_draw_focus(s, tx - 2, ty - 1, tw + 4, lh + 2);
    free(buf);
}

// A check box's square or a radio button's circle, then the label.
static void paint_check(struct w16 *w, struct w16_wnd *x, struct button *b, struct ugfx_surface *s,
                        int x0, int y0) {
    uint32_t black = w16_rgb(0x000000), white = w16_rgb(0xFFFFFF), bg = w16_parent_bg(w, x);
    ugfx_fill_rect(s, x0, y0, x->cw, x->ch, bg);
    int box = 12, lh = ugfx_char_h(), by = y0 + (x->ch - box) / 2;
    int left = (x->style & BS_LEFTTEXT) != 0, bx = left ? x0 + x->cw - box : x0;
    if (is_radio(x)) {
        ugfx_fill_circle(s, bx + box / 2, by + box / 2, box / 2, black);
        ugfx_fill_circle(s, bx + box / 2, by + box / 2, box / 2 - 1, b->pressed ? w16_rgb(0xC0C0C0) : white);
        if (b->checked) ugfx_fill_circle(s, bx + box / 2, by + box / 2, 2, black);
    } else {
        ugfx_fill_rect(s, bx, by, box, box, b->pressed ? w16_rgb(0xC0C0C0) : white);
        ugfx_draw_rect(s, bx, by, box, box, black);
        if (b->checked == 1)
            for (int i = 2; i < box - 2; i++) {      // 3.1's X
                ugfx_fill_rect(s, bx + i, by + i, 1, 1, black);
                ugfx_fill_rect(s, bx + box - 1 - i, by + i, 1, 1, black);
            }
        if (b->checked == 2) ugfx_fill_rect(s, bx + 2, by + 2, box - 4, box - 4, w16_rgb(0x808080));
    }
    int mark;
    char *buf = strip_amp(x->text ? x->text : "", 1, &mark);
    if (!buf) return;
    int tx = left ? x0 : x0 + box + 4, ty = y0 + (x->ch - lh) / 2, room = x->cw - box - 4;
    uint32_t fg = (x->style & WS_DISABLED) ? w16_rgb(0x808080) : black;
    ugfx_draw_string_clipped(s, tx, ty, room, buf, fg, bg);
    underline(s, tx, ty, buf, mark, fg);
    if (b->focused) {
        int tw = ugfx_text_width(buf);
        w16_draw_focus(s, tx - 1, ty, (tw < room ? tw : room) + 2, lh);
    }
    free(buf);
}

// A group box: a frame round its group, the label breaking its top edge.
static void paint_group(struct w16 *w, struct w16_wnd *x, struct ugfx_surface *s, int x0, int y0) {
    uint32_t black = w16_rgb(0x000000), bg = w16_parent_bg(w, x);
    int lh = ugfx_char_h(), mid = lh / 2;
    ugfx_draw_rect(s, x0, y0 + mid, x->cw, x->ch - mid, black);
    int mark;
    char *buf = strip_amp(x->text ? x->text : "", 1, &mark);
    if (!buf) return;
    if (buf[0]) {
        int tw = ugfx_text_width(buf);
        ugfx_fill_rect(s, x0 + 6, y0, tw + 4, lh, bg);
        ugfx_draw_string_clipped(s, x0 + 8, y0, x->cw - 12, buf, black, bg);
        underline(s, x0 + 8, y0, buf, mark, black);
    }
    free(buf);
}

static void paint_button(struct w16 *w, struct w16_wnd *x, struct ugfx_surface *s, int ox, int oy) {
    struct button *b = btn(x);
    if (!b) return;
    unsigned t = x->style & BS_TYPE;
    if (t == BS_GROUPBOX) paint_group(w, x, s, ox, oy);
    else if (is_push(x)) paint_push(w, x, b, s, ox, oy);
    else paint_check(w, x, b, s, ox, oy);
}

static void set_check(struct w16 *w, struct w16_wnd *x, int v) {
    struct button *b = btn(x);
    if (!b || b->checked == v) return;
    b->checked = v;
    w16_invalidate(w, x, 0);
}

// A click: an AUTO box ticks itself (a radio button clearing the rest of
// its group), then the parent hears BN_CLICKED.
static void click(struct w16 *w, struct w16_wnd *x) {
    struct button *b = btn(x);
    if (!b) return;
    switch (x->style & BS_TYPE) {
    case BS_AUTOCHECKBOX: set_check(w, x, !b->checked); break;
    case BS_AUTO3STATE:   set_check(w, x, (b->checked + 1) % 3); break;
    case BS_AUTORADIOBUTTON: {
        int n;
        struct w16_wnd **v = w16_wnd_children(w, x->parent, &n);
        int at = -1;
        for (int i = 0; v && i < n; i++) if (v[i] == x) at = i;
        // The group runs from the WS_GROUP control at or before x to the next one.
        int first = at;
        while (first > 0 && !(v[first]->style & WS_GROUP)) first--;
        for (int i = first; v && at >= 0 && i < n; i++) {
            if (i > first && (v[i]->style & WS_GROUP)) break;
            if (v[i]->host == w16_button_proc && (v[i]->style & BS_TYPE) == BS_AUTORADIOBUTTON)
                set_check(w, v[i], v[i] == x);
        }
        free(v);
        break;
    }
    default: break;
    }
    w16_notify(w, x, BN_CLICKED);
}

static void press(struct w16 *w, struct w16_wnd *x, int down) {
    struct button *b = btn(x);
    if (b && b->pressed != down) {
        b->pressed = down;
        w16_invalidate(w, x, 0);
    }
}

static void capture(struct w16 *w, uint16_t hwnd) {
    pthread_mutex_lock(&w->lock);
    w->capture = hwnd;
    pthread_mutex_unlock(&w->lock);
}

uint32_t w16_button_proc(struct w16 *w, struct w16_wnd *x, uint16_t msg, uint16_t wp, uint32_t lp) {
    struct button *b = btn(x);
    if (!b) return 0;
    switch (msg) {
    case WM_PAINT:
        w16_ctl_paint(w, x, paint_button);
        return 0;
    case WM_GETDLGCODE:
        if ((x->style & BS_TYPE) == BS_GROUPBOX) return DLGC_STATIC;
        if (is_radio(x)) return DLGC_BUTTON | DLGC_RADIOBUTTON;
        if (is_push(x))
            return DLGC_BUTTON | ((x->style & BS_TYPE) == BS_DEFPUSHBUTTON ? DLGC_DEFPUSHBUTTON : DLGC_UNDEFPUSHBUTTON);
        return DLGC_BUTTON;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if ((x->style & BS_TYPE) == BS_GROUPBOX) return 0;
        w16_set_focus(w, x->hwnd);
        b->captured = 1;
        capture(w, x->hwnd);
        press(w, x, 1);
        return 0;
    case WM_MOUSEMOVE:
        if (b->captured) {
            int16_t mx = (int16_t)lp, my = (int16_t)(lp >> 16);
            press(w, x, mx >= 0 && my >= 0 && mx < x->cw && my < x->ch);
        }
        return 0;
    case WM_LBUTTONUP:
        if (b->captured) {
            int was = b->pressed;
            b->captured = 0;
            capture(w, 0);
            press(w, x, 0);
            if (was) click(w, x);
        }
        return 0;
    case WM_CHAR:
        if (wp == ' ' && (x->style & BS_TYPE) != BS_GROUPBOX) click(w, x);
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        b->focused = msg == WM_SETFOCUS;
        if (msg == WM_KILLFOCUS && b->captured) { b->captured = 0; capture(w, 0); b->pressed = 0; }
        w16_invalidate(w, x, 0);
        return 0;
    case WM_ENABLE:
        w16_invalidate(w, x, 0);
        return 0;
    case WM_SETTEXT: {
        uint32_t r = w16_def_window_proc(w, x, msg, wp, lp);
        w16_invalidate(w, x, 1);
        return r;
    }
    case BM_GETCHECK:
        return (uint32_t)b->checked;
    case BM_SETCHECK:
        set_check(w, x, wp > 2 ? 1 : wp);
        return 0;
    case BM_GETSTATE:
        return (uint32_t)b->checked | (b->pressed ? 4 : 0) | (b->focused ? 8 : 0);
    case BM_SETSTATE:
        press(w, x, wp != 0);
        return 0;
    case BM_SETSTYLE:
        pthread_mutex_lock(&w->lock);
        x->style = (x->style & ~0xFFFFu) | wp;
        pthread_mutex_unlock(&w->lock);
        if (lp) w16_invalidate(w, x, 0);
        return 0;
    case WM_NCDESTROY:
        if (b->captured) capture(w, 0);
        free(x->ctl);
        x->ctl = 0;
        return 0;
    default:
        return w16_def_window_proc(w, x, msg, wp, lp);
    }
}

// --- STATIC ---------------------------------------------------------------------

static void paint_static(struct w16 *w, struct w16_wnd *x, struct ugfx_surface *s, int ox, int oy) {
    uint32_t bg = w16_parent_bg(w, x), black = w16_rgb(0x000000), grey = w16_rgb(0x808080);
    uint32_t white = w16_rgb(0xFFFFFF);
    unsigned t = x->style & SS_TYPE;
    switch (t) {
    case SS_ICON:
        ugfx_fill_rect(s, ox, oy, x->cw, x->ch, bg);
        w16_draw_icon(s, w, (int)(x->style >> 8 & 0xFF), ox, oy);
        return;
    case SS_BLACKRECT: ugfx_fill_rect(s, ox, oy, x->cw, x->ch, black); return;
    case SS_GRAYRECT:  ugfx_fill_rect(s, ox, oy, x->cw, x->ch, grey); return;
    case SS_WHITERECT: ugfx_fill_rect(s, ox, oy, x->cw, x->ch, white); return;
    case SS_BLACKFRAME: ugfx_draw_rect(s, ox, oy, x->cw, x->ch, black); return;
    case SS_GRAYFRAME:  ugfx_draw_rect(s, ox, oy, x->cw, x->ch, grey); return;
    case SS_WHITEFRAME: ugfx_draw_rect(s, ox, oy, x->cw, x->ch, white); return;
    default: break;
    }
    ugfx_fill_rect(s, ox, oy, x->cw, x->ch, bg);
    int align = t == SS_CENTER ? 1 : t == SS_RIGHT ? 2 : 0;
    int wrap = t != SS_SIMPLE && t != SS_LEFTNOWORDWRAP;
    text_box(s, ox, oy, x->cw, x->ch, x->text ? x->text : "", align, !(x->style & SS_NOPREFIX), wrap,
             (x->style & WS_DISABLED) ? grey : black, bg);
}

uint32_t w16_static_proc(struct w16 *w, struct w16_wnd *x, uint16_t msg, uint16_t wp, uint32_t lp) {
    switch (msg) {
    case WM_CREATE:
        x->transparent = 1;                      // HTTRANSPARENT: a press reaches what is under it
        return 0;
    case WM_PAINT:
        w16_ctl_paint(w, x, paint_static);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_STATIC;
    case WM_SETTEXT: {
        uint32_t r = w16_def_window_proc(w, x, msg, wp, lp);
        w16_invalidate(w, x, 1);
        return r;
    }
    case WM_ENABLE:
        w16_invalidate(w, x, 0);
        return 0;
    default:
        return w16_def_window_proc(w, x, msg, wp, lp);
    }
}

void w16_register_builtins(struct w16 *w) {
    w16_class_add_host(w, "BUTTON", w16_button_proc, CS_DBLCLKS, 0);
    w16_class_add_host(w, "STATIC", w16_static_proc, 0, 0);
    int edit = w16_class_add_host(w, "EDIT", w16_edit_proc, CS_DBLCLKS, 0);
    if (edit >= 0) w->cls[edit].cursor = WIN_CURSOR_TEXT;           // IDC_IBEAM
    w16_class_add_host(w, "LISTBOX", w16_listbox_proc, CS_DBLCLKS, 0);
    w16_class_add_host(w, "COMBOBOX", w16_combo_proc, CS_DBLCLKS, 0);
    w16_class_add_host(w, "#32770", w16_dialog_proc, 0, COLOR_WINDOW + 1);
}
