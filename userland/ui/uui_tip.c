// A tooltip -- see ui/uui_tip.h.
#include "ui/uui_tip.h"
#include "ui/uui_popup.h"
#include "rt/sys.h" // sys_ticks()

void uui_tip_init(struct uui_tip *t) { *t = (struct uui_tip){ .key = -1 }; }

void uui_tip_hide(struct uui_tip *t) {
    if (t->popup) {
        uui_popup_close(t->popup);
        t->popup = 0;
    }
    t->shown = 0;
}

void uui_tip_hover(struct uui_tip *t, int key) {
    if (key == t->key) return;
    t->key = key;
    t->since = sys_ticks();
    uui_tip_hide(t);   // a new button means a new tip, and the old SURFACE goes
}

int uui_tip_waiting(const struct uui_tip *t) { return t->key >= 0 && !t->shown; }

static void size_of(const char *text, int *w, int *h) {
    *w = ugfx_text_width(text) + UUI_TIP_PAD * 2;
    *h = ugfx_char_h() + UUI_TIP_PAD * 2;
}

// The compositor swept this client's popups -- it does that for ALL of
// them when a GRABBING one is dismissed, so a tooltip up beside an open
// menu goes with it. The id is already dead; just forget it.
static void tip_done(void *owner) {
    struct uui_tip *t = owner;
    t->popup = 0;
    t->shown = 0;
}

int uui_tip_tick(struct uui_tip *t, const char *text, int bx, int by, int bw, int bh, int beside) {
    int want = t->key >= 0 && text && bw > 0 && sys_ticks() - t->since >= UUI_TOOLTIP_DELAY_TICKS;
    if (want == t->shown) return 0;
    if (!want) { uui_tip_hide(t); return 1; }
    int w, h, px, py;
    size_of(text, &w, &h);
    t->shown = 1;
    // NO GRAB. The compositor flips it above the button when there is no
    // room below -- the clamp a widget can only do against its own window.
    t->popup = uui_popup_open(bx, by, bw, bh, w, h, beside ? UUI_POPUP_RIGHT : UUI_POPUP_BELOW, 0, tip_done, t,
                              &px, &py);
    return 1;
}

void uui_tip_draw(struct ugfx_surface *s, const struct uui_tip *t, const char *text, int bx, int by, int bw, int bh,
                  int beside, uint32_t bg, uint32_t fg, uint32_t border) {
    if (!t->shown || !text) return;
    int w, h, x, y;
    size_of(text, &w, &h);
    struct ugfx_surface *ps = t->popup ? uui_popup_surface(t->popup) : 0;
    if (ps) {
        // Its own surface: the compositor placed it, so the tip sits at
        // that surface's origin and nothing here clamps.
        s = ps;
        x = y = 0;
    } else {
        // In-window fallback, slid inward at the surface's edges -- a tip
        // that clips is a tip nobody can read.
        x = beside ? bx + bw + UUI_TIP_GAP : bx;
        y = beside ? by + (bh - h) / 2 : by + bh + UUI_TIP_GAP;
        if (x + w > s->w) x = s->w - w;
        if (x < 0) x = 0;
        if (y + h > s->h) y = by - h - UUI_TIP_GAP;   // no room below: flip above
    }
    ugfx_fill_rect(s, x, y, w, h, bg);
    ugfx_draw_rect(s, x, y, w, h, border);
    ugfx_draw_string_clipped(s, x + UUI_TIP_PAD, y + UUI_TIP_PAD, w - UUI_TIP_PAD * 2, text, fg, bg);
}
