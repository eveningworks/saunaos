// :name: completion in a text field (ui/uui_emojicomplete.h).
#include "ui/uui_emojicomplete.h"
#include "ui/uui_textbox.h"
#include "ui/uui_emojigrid.h"
#include "ui/uui_popup.h"
#include "ui/uui_primitives.h"
#include "ui/utheme.h"
#include "base/uemojidata.h"
#include "keyboard.h"

#include <string.h>

static struct {
    const struct uui_textbox *f;     // whose list is open, or NULL
    int start;                       // the ':' it replaces from
    int n, sel;
    int items[UUI_EMOJICOMPLETE_MAX];
    int x, y, w, h, row_h;           // where it was last drawn
    int drawn;                       // ...since it opened: only then does it take keys
} g;
static const struct uui_textbox *g_shown_field;   // the active field the overlay pass last drew
static const struct uui_textbox *g_dismissed;     // Esc'd: not again for this ':'
static int g_dismissed_at;

void uui_emojicomplete_cancel(void) {
    g.f = 0;
    g.n = 0;
    g.drawn = 0;
}

int uui_emojicomplete_open(const struct uui_textbox *f) {
    return f && g.f == f && g.n > 0 && g.drawn;
}

static int letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

void uui_emojicomplete_update(struct uui_textbox *f) {
    const struct uui_textbox *was = g.f;
    uui_emojicomplete_cancel();
    if (!f || f != g_shown_field || !f->active || f->masked || f->disabled || f->no_complete ||
        uui_textbox_has_selection(f))
        return;
    const char *t = uui_textbox_text(f);
    // An Esc holds for THAT colon only: once the caret is back at or before
    // it -- the colon deleted -- a ':' typed there again completes again.
    if (g_dismissed == f && (f->ed.cursor <= g_dismissed_at || g_dismissed_at >= (int)strlen(t) ||
                             t[g_dismissed_at] != ':'))
        g_dismissed = 0;
    int c = f->ed.cursor, i = c;
    while (i > 0 && letter(t[i - 1])) i--;
    if (c - i < 2 || i < 1 || t[i - 1] != ':') return;
    int colon = i - 1;
    if (colon > 0 && t[colon - 1] != ' ') return;
    if (g_dismissed == f && g_dismissed_at == colon) return;
    char q[UUI_TEXTBOX_MAX];
    int qn = c - i < (int)sizeof q - 1 ? c - i : (int)sizeof q - 1;
    memcpy(q, t + i, (size_t)qn);
    q[qn] = '\0';
    int n = uemojidata_search(uemojidata(), q, g.items, UUI_EMOJICOMPLETE_MAX);
    if (n <= 0) return;
    g.f = f;
    g.start = colon;
    g.n = n < UUI_EMOJICOMPLETE_MAX ? n : UUI_EMOJICOMPLETE_MAX;
    g.sel = 0;
    g.drawn = was == f;   // still showing: it keeps the keys while it is redrawn
}

// ":text" out, the emoji in, as typing would leave it: the caret after it.
static void choose(struct uui_textbox *f, int k) {
    const struct uemojidata *d = uemojidata();
    if (k < 0 || k >= g.n || g.items[k] < 0 || g.items[k] >= d->count) return;
    int from = g.start, to = f->ed.cursor;
    uui_emojicomplete_cancel();
    // The text may have changed under the list by another route (an app's
    // set_text, a paste): replace only a ':' still where it was.
    const char *t = uui_textbox_text(f);
    if (from >= to || to > (int)strlen(t) || t[from] != ':') return;
    uui_textbox_select(f, from, to);
    uui_textbox_insert(f, d->entries[g.items[k]].seq);
}

int uui_emojicomplete_key(struct uui_textbox *f, int key) {
    if (!uui_emojicomplete_open(f)) return 0;
    switch (key) {
    case '\t': case '\n': case '\r':
        choose(f, g.sel);
        return 1;
    case 0x1B:
        g_dismissed = f;
        g_dismissed_at = g.start;
        uui_emojicomplete_cancel();
        return 1;
    case KEY_ARROW_UP:
        g.sel = (g.sel + g.n - 1) % g.n;
        return 1;
    case KEY_ARROW_DOWN:
        g.sel = (g.sel + 1) % g.n;
        return 1;
    default:
        return 0;
    }
}

void uui_emojicomplete_draw(struct ugfx_surface *s, const struct uui_textbox *f) {
    if (f->active) g_shown_field = f;
    if (g.f != f || g.n <= 0) return;
    const struct uemojidata *d = uemojidata();
    int lh = ugfx_char_h(), pad = ugfx_px(6);
    g.row_h = lh + ugfx_px(8);
    g.w = f->w > ugfx_px(240) ? f->w : ugfx_px(240);
    if (g.w > s->w) g.w = s->w;
    g.h = g.n * g.row_h + 2 * pad;
    // Below the field; above it when the window has no room; inside the window.
    g.x = f->x + g.w > s->w ? s->w - g.w : f->x;
    if (g.x < 0) g.x = 0;
    g.y = f->y + f->h + 2;
    if (g.y + g.h > s->h && f->y - 2 - g.h >= 0) g.y = f->y - 2 - g.h;
    int r = uui_popup_radius();
    uui_fill_round_rect(s, g.x, g.y, g.w, g.h, r, uui_popup_border());
    uui_fill_round_rect(s, g.x + 1, g.y + 1, g.w - 2, g.h - 2, r > 1 ? r - 1 : r, uui_popup_bg());
    for (int i = 0; i < g.n; i++) {
        int ry = g.y + pad + i * g.row_h;
        uint32_t ground = uui_popup_bg();
        if (i == g.sel) {
            ground = UTHEME_SELECTION;
            uui_fill_round_rect(s, g.x + pad, ry, g.w - 2 * pad, g.row_h, uui_select_radius(0), ground);
        }
        int e = g.items[i], cell = g.row_h;
        uui_emojigrid_draw_entry(s, 0, e, lh, g.x + pad, ry, cell, cell);
        int tx = g.x + pad + cell + pad;
        ugfx_draw_string_clipped(s, tx, ry + (g.row_h - lh) / 2, g.x + g.w - pad - tx, d->entries[e].name,
                                 UTHEME_TEXT, ground);
    }
    g.drawn = 1;
}

int uui_emojicomplete_press(struct uui_textbox *f, int cx, int cy) {
    if (!uui_emojicomplete_open(f)) return 0;
    int pad = ugfx_px(6);
    if (uui_hit(g.x, g.y + pad, g.w, g.n * g.row_h, cx, cy)) {
        choose(f, (cy - g.y - pad) / g.row_h);
        return 1;
    }
    uui_emojicomplete_cancel();
    return 0;
}
