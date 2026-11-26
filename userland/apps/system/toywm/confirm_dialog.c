// See confirm_dialog.h for the design writeup.
#include "confirm_dialog.h"
#include "wm_internal.h"
#include "wm_overlay.h"
#include "ui/uui.h"
#include "ui/uui_caret.h"
#include "ui/uui_textbox.h"
#include "ui/uui_widget.h"   // uui_textbox_ops: the field's press
#include "ui/utheme.h"
#include "kapi.h"
#include <stdio.h>
#include <string.h>

int confirm_dialog_open = 0;

static const char *g_yes_label = "Yes";
static const char *g_no_label = "No";
static const char *g_message;
static void (*g_on_yes)(void);
static void (*g_on_no)(void);

// Geometry -- computed once at open time from the message's own length
// (same "size the popup to its content" approach context_menu.c's
// menu_w() uses), not recomputed every frame, since neither the
// message nor the screen size can change while a modal dialog is open.
static int g_x, g_y, g_w, g_h;

// The two buttons, as a real group: hover, pressed and
// commit-on-release all come from it rather than being reimplemented
// here (see the header for what this replaced). Screen-absolute
// geometry, like the rest of this dialog, so every draw/hit call passes
// origin (0, 0).
#define BTN_YES 0
#define BTN_NO  1
static struct uui_button g_btns[2];
static struct uui_button_group g_group;

#define DIALOG_PAD 16
#define BTN_GAP 12
#define BTN_H_EXTRA 10 // button height beyond one text row


// The real opener. Both entry points below set the labels FIRST,
// because the layout is measured from them -- a "Force Quit" button
// sized for the word "Yes" is exactly the kind of bug this file's own
// comment about hand-drawn geometry warns about.
// The detailed form's text, copied, and where its lines break.
#define WRAP_MAX 12
static struct {
    int on;
    char title[64], headline[200], body[400];
    char key[CONFIRM_DETAIL_ROWS][32], val[CONFIRM_DETAIL_ROWS][160];
    int nrows;
    int head_at[WRAP_MAX], head_len[WRAP_MAX], nhead;
    int body_at[WRAP_MAX], body_len[WRAP_MAX], nbody;
    int bar_h, text_x, key_w;
    // The password field (users D2a): its label, the field, a line for
    // "wrong password", and BUSY while privd checks what was typed.
    int pw_on, busy;
    char pw_label[96], pw_err[96];
    int pw_label_y, pw_err_y;
} g_d;
static struct uui_textbox g_pw;

// Breaks `s` at spaces into lines no wider than `w`; a word wider than
// `w` stands alone and is clipped when drawn. Past WRAP_MAX lines the
// rest is not shown -- privd's record bounds the text well inside it.
static int fits(const char *s, int n, int w) {
    char line[400];
    if (n >= (int)sizeof line) return 0;
    memcpy(line, s, (size_t)n);
    line[n] = '\0';
    return ugfx_text_width(line) <= w;
}

static int wrap(const char *s, int w, int *at, int *len) {
    int n = 0, i = 0, total = (int)strlen(s);
    while (i < total && n < WRAP_MAX) {
        while (s[i] == ' ') i++;
        if (i >= total) break;
        int start = i, end = -1;
        for (int j = i; ; ) {
            int k = j;
            while (k < total && s[k] != ' ') k++;
            int ok = fits(s + start, k - start, w);
            if (ok || end < 0) end = k;
            if (!ok || k >= total) break;
            j = k + 1;
        }
        at[n] = start;
        len[n] = end - start;
        n++;
        i = end;
    }
    return n;
}

static void layout_detailed(int yes_w, int no_w, int btn_h) {
    int ch = ugfx_char_h();
    int icon = ch * 2;
    g_d.bar_h = ch + ugfx_px(12);
    g_w = ugfx_text_width("m") * 46 + icon + DIALOG_PAD * 3;
    if (g_w > screen_w - DIALOG_PAD * 2) g_w = screen_w - DIALOG_PAD * 2;
    g_d.text_x = DIALOG_PAD + icon + DIALOG_PAD;
    int text_w = g_w - g_d.text_x - DIALOG_PAD;
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    g_d.nhead = wrap(g_d.headline, text_w, g_d.head_at, g_d.head_len);
    ugfx_set_font(was);
    g_d.nbody = wrap(g_d.body, text_w, g_d.body_at, g_d.body_len);
    g_d.key_w = 0;
    for (int i = 0; i < g_d.nrows; i++) {
        int kw = ugfx_text_width(g_d.key[i]);
        if (kw > g_d.key_w) g_d.key_w = kw;
    }
    int row_h = ch + ugfx_px(4);
    int body_h = g_d.nhead * ch + ugfx_px(8) + g_d.nbody * ch + ugfx_px(10) +
                 g_d.nrows * row_h + ugfx_px(12);
    int field_h = ch + ugfx_px(10);
    int pw_h = g_d.pw_on ? ugfx_px(10) + ch + ugfx_px(6) + field_h + ugfx_px(4) + ch : 0;
    body_h += pw_h;
    if (body_h < icon) body_h = icon;
    int foot_h = btn_h + ugfx_px(24);
    g_h = g_d.bar_h + DIALOG_PAD + body_h + DIALOG_PAD + foot_h;
    g_x = (screen_w - g_w) / 2;
    g_y = (screen_h - g_h) / 2;
    if (g_d.pw_on) {
        // Under the grid, the last thing in the body.
        int top = g_y + g_d.bar_h + DIALOG_PAD + body_h - pw_h + ugfx_px(10);
        g_d.pw_label_y = top;
        uui_textbox_set_geometry(&g_pw, g_x + g_d.text_x, top + ch + ugfx_px(6), text_w, field_h);
        g_d.pw_err_y = top + ch + ugfx_px(6) + field_h + ugfx_px(4);
    }
    int btn_y = g_y + g_h - foot_h + ugfx_px(12);
    int right = g_x + g_w - DIALOG_PAD;
    // The affirmative first, as every other dialog here orders them.
    uui_button_init(&g_btns[BTN_NO], right - no_w, btn_y, no_w, btn_h, g_no_label,
                    UTHEME_BUTTON_BG, UTHEME_TEXT, BTN_NO);
    uui_button_init(&g_btns[BTN_YES], right - no_w - BTN_GAP - yes_w, btn_y, yes_w, btn_h,
                    g_yes_label, UTHEME_ACCENT, UTHEME_ACCENT_TEXT, BTN_YES);
}

static void open_common(const char *message, void (*on_yes)(void), void (*on_no)(void)) {
    g_message = message;
    g_on_yes = on_yes;
    g_on_no = on_no;

    int ch = ugfx_char_h();
    // ugfx_text_width(), not strlen() * ugfx_char_w() -- that identity
    // stopped holding when a proportional face became loadable, and it
    // sums per-glyph advances now (see api/gfx.h, ugfx_char_advance()).
    int msg_w = ugfx_text_width(message);

    const char *yes_label = g_yes_label, *no_label = g_no_label;
    int yes_w = ugfx_text_width(yes_label) + 24;
    int no_w = ugfx_text_width(no_label) + 24;
    int buttons_w = yes_w + BTN_GAP + no_w;

    g_w = (msg_w > buttons_w ? msg_w : buttons_w) + DIALOG_PAD * 2;
    int btn_h = ch + BTN_H_EXTRA;
    if (g_d.on) {
        layout_detailed(yes_w, no_w, btn_h);
        uui_button_group_init(&g_group, g_btns, 2);
        confirm_dialog_open = 1;
        confirm_dialog_damage();
        return;
    }
    g_h = DIALOG_PAD + ch + DIALOG_PAD + btn_h + DIALOG_PAD;

    g_x = (screen_w - g_w) / 2;
    g_y = (screen_h - g_h) / 2;

    int btn_y = g_y + DIALOG_PAD + ch + DIALOG_PAD;
    int btn_x0 = g_x + (g_w - buttons_w) / 2;

    uui_button_init(&g_btns[BTN_YES], btn_x0, btn_y, yes_w, btn_h, yes_label,
                    UTHEME_BUTTON_BG, UTHEME_TEXT, BTN_YES);
    uui_button_init(&g_btns[BTN_NO], btn_x0 + yes_w + BTN_GAP, btn_y, no_w, btn_h,
                    no_label, UTHEME_BUTTON_BG, UTHEME_TEXT, BTN_NO);
    uui_button_group_init(&g_group, g_btns, 2);

    confirm_dialog_open = 1;
    confirm_dialog_damage();   // here, and wherever a dialog it replaced was drawn
}

static void close_dialog(void) {
    if (g_d.pw_on) {
        uui_textbox_set_text(&g_pw, "");
        g_btns[BTN_YES].disabled = 0;
        g_d.pw_on = g_d.busy = 0;
    }
    confirm_dialog_open = 0;   // where it was drawn is damaged by the core (wm_overlay.h)
    WM_REDRAW();
}

// A padlock, from rectangles: the shackle's two sides and top over the body.
static void draw_lock(int x, int y, int size, uint32_t c) {
    int t = size / 12 > 1 ? size / 12 : 2;
    int bw = size * 3 / 4, bh = size / 2, bx = x + (size - bw) / 2, by = y + size - bh;
    int sw = bw * 2 / 3, sx = x + (size - sw) / 2, sy = y + size / 8;
    ugfx_fill_rect(wm_surface(), sx, sy, t, by - sy, c);
    ugfx_fill_rect(wm_surface(), sx + sw - t, sy, t, by - sy, c);
    ugfx_fill_rect(wm_surface(), sx, sy, sw, t, c);
    ugfx_fill_rect(wm_surface(), bx, by, bw, bh, c);
}

static void draw_detailed(void) {
    struct ugfx_surface *s = wm_surface();
    int ch = ugfx_char_h();
    uint32_t text = UTHEME_TEXT, muted = utheme_current()->text_muted;
    ugfx_fill_rect(s, g_x, g_y, g_w, g_h, UTHEME_WHITE);
    ugfx_fill_rect(s, g_x, g_y, g_w, g_d.bar_h, UTHEME_CHROME);
    ugfx_fill_rect(s, g_x, g_y + g_d.bar_h - 1, g_w, 1, UTHEME_SEPARATOR);
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    ugfx_draw_string_clipped(s, g_x + DIALOG_PAD, g_y + (g_d.bar_h - ch) / 2, g_w - DIALOG_PAD * 2,
                             g_d.title, text, UTHEME_CHROME);
    int y = g_y + g_d.bar_h + DIALOG_PAD;
    int tx = g_x + g_d.text_x, tw = g_w - g_d.text_x - DIALOG_PAD;
    char line[400];
    for (int i = 0; i < g_d.nhead; i++, y += ch) {
        memcpy(line, g_d.headline + g_d.head_at[i], (size_t)g_d.head_len[i]);
        line[g_d.head_len[i]] = '\0';
        ugfx_draw_string_clipped(s, tx, y, tw, line, text, UTHEME_WHITE);
    }
    ugfx_set_font(was);
    draw_lock(g_x + DIALOG_PAD, g_y + g_d.bar_h + DIALOG_PAD, ch * 2, UTHEME_ACCENT);
    y += ugfx_px(8);
    for (int i = 0; i < g_d.nbody; i++, y += ch) {
        memcpy(line, g_d.body + g_d.body_at[i], (size_t)g_d.body_len[i]);
        line[g_d.body_len[i]] = '\0';
        ugfx_draw_string_clipped(s, tx, y, tw, line, muted, UTHEME_WHITE);
    }
    y += ugfx_px(10);
    int row_h = ch + ugfx_px(4);
    if (g_d.nrows) {
        int gh = g_d.nrows * row_h + ugfx_px(8);
        ugfx_fill_rect(s, tx, y, tw, gh, UTHEME_PANEL_BG);
        ugfx_draw_rect(s, tx, y, tw, gh, UTHEME_SEPARATOR);
        int ky = y + ugfx_px(4) + ugfx_px(2);
        int kx = tx + ugfx_px(8), vx = kx + g_d.key_w + ugfx_px(16);
        for (int i = 0; i < g_d.nrows; i++, ky += row_h) {
            ugfx_draw_string_clipped(s, kx, ky, vx - kx, g_d.key[i], muted, UTHEME_PANEL_BG);
            ugfx_draw_string_clipped(s, vx, ky, tx + tw - ugfx_px(8) - vx, g_d.val[i], text,
                                     UTHEME_PANEL_BG);
        }
    }
    if (g_d.pw_on) {
        was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
        ugfx_draw_string_clipped(s, tx, g_d.pw_label_y, tw, g_d.pw_label, text, UTHEME_WHITE);
        ugfx_set_font(was);
        uui_textbox_draw(s, &g_pw);
        if (g_d.pw_err[0])
            ugfx_draw_string_clipped(s, tx, g_d.pw_err_y, tw, g_d.pw_err,
                                     utheme_severity(UTHEME_SEV_ERROR), UTHEME_WHITE);
    }
    int foot_y = g_btns[BTN_YES].y - ugfx_px(12);
    ugfx_fill_rect(s, g_x, foot_y, g_w, g_y + g_h - foot_y, UTHEME_PANEL_BG);
    ugfx_fill_rect(s, g_x, foot_y, g_w, 1, UTHEME_SEPARATOR);
    ugfx_draw_rect(s, g_x, g_y, g_w, g_h, UTHEME_BORDER);
    uui_button_group_draw(&g_group, s);
}

void confirm_dialog_draw(void) {
    if (!confirm_dialog_open) return;
    if (g_d.on) { draw_detailed(); return; }

    uint32_t bg = UTHEME_PANEL_BG, border = UTHEME_BORDER, fg = UTHEME_TEXT;
    ugfx_fill_rect(wm_surface(), g_x, g_y, g_w, g_h, bg);
    ugfx_draw_rect(wm_surface(), g_x, g_y, g_w, g_h, border);

    int msg_w = ugfx_text_width(g_message);
    // Clipped to the dialog's interior: a message longer than the box
    // would otherwise be drawn straight through the border and out onto
    // the desktop, since gfx_draw_string() bounds nothing at all
    // (docs/gui-guidelines.md).
    ugfx_draw_string_clipped(wm_surface(), g_x + (g_w - msg_w) / 2, g_y + DIALOG_PAD,
                             g_w - DIALOG_PAD, g_message, fg, bg);

    uui_button_group_draw(&g_group, wm_surface()); // screen-absolute, so no origin
}

static void set_busy(int busy) {
    g_d.busy = busy;
    g_btns[BTN_YES].disabled = busy;
    g_pw.ed.flags = busy ? (g_pw.ed.flags | UUI_EDIT_READONLY) : (g_pw.ed.flags & ~UUI_EDIT_READONLY);
    confirm_dialog_damage();
    WM_REDRAW();
}

// A button chose: the affirmative of a password dialog waits for the
// caller's verdict (BUSY) rather than closing.
static void commit(int code) {
    if (code == BTN_YES && g_d.pw_on) {
        if (g_d.busy) return;
        g_d.pw_err[0] = '\0';
        set_busy(1);
        if (g_on_yes) g_on_yes();
        return;
    }
    close_dialog();
    if (code == BTN_YES) { if (g_on_yes) g_on_yes(); }
    else                 { if (g_on_no) g_on_no(); }
}

const char *confirm_dialog_password(void) {
    return confirm_dialog_open && g_d.pw_on ? uui_textbox_text(&g_pw) : 0;
}

void confirm_dialog_password_wrong(const char *why) {
    if (!confirm_dialog_open || !g_d.pw_on) return;
    snprintf(g_d.pw_err, sizeof g_d.pw_err, "%s", why ? why : "");
    uui_textbox_set_text(&g_pw, "");
    set_busy(0);
}

int confirm_dialog_text_at(int mx, int my) {
    return confirm_dialog_open && g_d.pw_on && mx >= g_pw.x && mx < g_pw.x + g_pw.w &&
           my >= g_pw.y && my < g_pw.y + g_pw.h;
}

int confirm_dialog_password_state(const char **label, const char **err, int *busy, int *typed,
                                  int *x, int *y, int *w, int *h) {
    if (!confirm_dialog_open || !g_d.pw_on) return 0;
    *label = g_d.pw_label;
    *err = g_d.pw_err;
    *busy = g_d.busy;
    *typed = (int)strlen(uui_textbox_text(&g_pw));
    *x = g_pw.x; *y = g_pw.y; *w = g_pw.w; *h = g_pw.h;
    return 1;
}

int confirm_dialog_key(int key, uint8_t mods) {
    if (!confirm_dialog_open || !g_d.on || !g_d.pw_on) return 0;
    if (key == '\n' || key == '\r') { commit(BTN_YES); return 1; }
    if (!g_d.busy && uui_textbox_key_mods(&g_pw, key, mods)) {
        uui_caret_reset();
        confirm_dialog_damage();
        WM_REDRAW();
    }
    return 1;   // modal: nothing under it hears the keys while it is up
}

int confirm_dialog_handle_click(int mx, int my) {
    if (!confirm_dialog_open) return 0;
    (void)mx; (void)my;
    // Deliberately acts on NOTHING. This fires on button-DOWN, and a
    // control that commits here can never be cancelled -- the exact trap
    // gui_apps.h's on_click documents and that this dialog fell into.
    // Arming and committing live in confirm_dialog_update_press() below.
    //
    // Still returns 1: the dialog is modal, so the click is swallowed
    // rather than passed through to whatever is underneath.
    return 1;
}

void confirm_dialog_update_press(int mx, int my, uint8_t buttons) {
    if (!confirm_dialog_open) return;

    static int down;
    if (buttons & 0x1) {
        // The password field hears the press EDGE: a click places its
        // caret, or toggles its eye (uui_textbox's `revealed`).
        if (!down && g_d.on && g_d.pw_on && !g_d.busy && uui_textbox_hit(&g_pw, mx, my)) {
            uui_textbox_ops.press(&g_pw, mx, my, 0);
            confirm_dialog_damage();
        }
        down = 1;
        // Re-hit-tested every tick, so dragging off a button visibly
        // un-presses it and dragging back re-presses.
        if (uui_button_group_press(&g_group, mx, my)) confirm_dialog_damage();
        return;
    }
    down = 0;

    // Button released. -1 means either nothing was armed or the press
    // had been dragged off -- both mean "do nothing", which is the whole
    // point of committing on release.
    int code = uui_button_group_release(&g_group);
    if (code < 0) return;
    commit(code);
}

// The registry's two ops (wm_overlay.h). `hover_at` ADOPTS the hover as
// well as reporting it -- the buttons draw from their own `hovered`
// flag -- and returns a token the core compares.
int confirm_dialog_hover_at(int mx, int my) {
    if (!confirm_dialog_open) return 0;
    uui_button_group_hover(&g_group, mx, my);
    for (int i = 0; i < g_group.count; i++)
        if (g_group.buttons[i].hovered) return i + 1;
    return 0;
}

// Its rect, and the core adds where it was last drawn (wm_overlay.h). It
// casts no shadow, so the window rect's margin is only slack.
void confirm_dialog_damage(void) { wm_overlay_damage("confirm"); }

int confirm_dialog_rect(int *x, int *y, int *w, int *h) {
    if (!confirm_dialog_open) return 0;
    *x = g_x; *y = g_y; *w = g_w; *h = g_h;
    return 1;
}


void confirm_dialog_open_with(const char *message, void (*on_yes)(void), void (*on_no)(void)) {
    // Reset, so a labelled dialog's words cannot leak into an ordinary
    // one. This is the only place that can guarantee it.
    g_yes_label = "Yes";
    g_no_label = "No";
    g_d.on = 0;
    open_common(message, on_yes, on_no);
}

void confirm_dialog_open_labelled(const char *message, const char *yes, const char *no,
                                   void (*on_yes)(void), void (*on_no)(void)) {
    g_yes_label = yes ? yes : "Yes";
    g_no_label = no ? no : "No";
    g_d.on = 0;
    open_common(message, on_yes, on_no);
}

void confirm_dialog_open_detailed(const struct confirm_detail *d, const char *yes, const char *no,
                                  void (*on_yes)(void), void (*on_no)(void)) {
    g_yes_label = yes ? yes : "Yes";
    g_no_label = no ? no : "No";
    memset(&g_d, 0, sizeof g_d);
    g_d.on = 1;
    snprintf(g_d.title, sizeof g_d.title, "%s", d->title ? d->title : "");
    snprintf(g_d.headline, sizeof g_d.headline, "%s", d->headline ? d->headline : "");
    snprintf(g_d.body, sizeof g_d.body, "%s", d->body ? d->body : "");
    g_d.nrows = d->nrows < CONFIRM_DETAIL_ROWS ? d->nrows : CONFIRM_DETAIL_ROWS;
    for (int i = 0; i < g_d.nrows; i++) {
        snprintf(g_d.key[i], sizeof g_d.key[i], "%s", d->key[i] ? d->key[i] : "");
        snprintf(g_d.val[i], sizeof g_d.val[i], "%s", d->val[i] ? d->val[i] : "");
    }
    if (d->password_label) {
        g_d.pw_on = 1;
        snprintf(g_d.pw_label, sizeof g_d.pw_label, "%s", d->password_label);
        uui_textbox_init(&g_pw, "");
        g_pw.masked = 1;
        g_pw.ed.flags |= UUI_EDIT_MASKED;
        uui_textbox_set_active(&g_pw, 1);
        uui_caret_reset();
    }
    open_common(g_d.headline, on_yes, on_no);
}

void confirm_dialog_close(void) {
    if (confirm_dialog_open) close_dialog();
}

// The buttons' own rects, for the debug console (`gui dialog`) and so
// for tests. Same reasoning as context_menu_geometry(): read from the
// SAME ui_button the dialog draws and hit-tests, so a test cannot be
// told a position a click would not land on.
//
// tools/tests/gui/dialog_test.py used to find these by scanning the button row for
// UTHEME_BUTTON_BG, which works but re-derives geometry in Python -- the
// thing this project's own rules say not to do, and which stops working
// the moment a button's label changes its width. "Force Quit"/"Wait" is
// exactly that change.
int confirm_dialog_button_rect(int index, int *x, int *y, int *w, int *h,
                                const char **label) {
    if (!confirm_dialog_open || index < 0 || index > 1) return 0;
    if (x) *x = g_btns[index].x;
    if (y) *y = g_btns[index].y;
    if (w) *w = g_btns[index].w;
    if (h) *h = g_btns[index].h;
    if (label) *label = g_btns[index].label;
    return 1;
}

const char *confirm_dialog_message(void) {
    return confirm_dialog_open ? g_message : 0;
}

const char *confirm_dialog_body(void) {
    return confirm_dialog_open ? (g_d.on ? g_d.body : "") : 0;
}
