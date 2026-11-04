// list16.c -- the LISTBOX and COMBOBOX classes, as USER had them built in:
// a list of strings to pick one from, scrolled with its own scroll bar,
// and a drop-down box that shows the chosen one and opens a list under
// itself.
//
// A COMBOBOX'S LIST IS A LISTBOX, a sibling of the box, under it and
// above everything else -- Windows' was a popup window; here it is a
// child of the box's parent raised to the top of the z-order while it is
// open, so it may hang over the dialog's other controls but not past the
// dialog's edge. Its notifications go to the box, which turns them into
// CBN_* for its parent. A template's height for a combo box is its
// height OPEN; it is drawn shut at one row.
//
// OWNER-DRAWN (LBS_/CBS_OWNERDRAWFIXED): the parent sizes a row once
// (WM_MEASUREITEM) and draws each one (WM_DRAWITEM) with the control's
// DC; w16 keeps the frame, the button, the white ground and the scrolling.
// A combo's rows, and the item in its shut box, go to the BOX's parent as
// ODT_COMBOBOX naming the box -- Character Map's TrueType marks.
#include "w16_internal.h"

#include <stdlib.h>
#include <string.h>
#include "keyboard.h"
#include "ui/ugfx.h"

#define LBS_NOTIFY     0x0001
#define LBS_SORT       0x0002
#define LBS_NOREDRAW   0x0004
#define LBS_MULTIPLESEL 0x0008
#define LBS_OWNERDRAWFIXED 0x0010
#define LBS_OWNERDRAWVARIABLE 0x0020
#define LBS_HASSTRINGS 0x0040
#define LBS_NOINTEGRALHEIGHT 0x0100
#define LBS_EXTENDEDSEL 0x0800
#define LBS_DISABLENOSCROLL 0x1000
#define LB_ADDSTRING   (WM_USER + 1)
#define LB_INSERTSTRING (WM_USER + 2)
#define LB_DELETESTRING (WM_USER + 3)
#define LB_RESETCONTENT (WM_USER + 5)
#define LB_SETSEL      (WM_USER + 6)
#define LB_SETCURSEL   (WM_USER + 7)
#define LB_GETSEL      (WM_USER + 8)
#define LB_GETCURSEL   (WM_USER + 9)
#define LB_GETTEXT     (WM_USER + 10)
#define LB_GETTEXTLEN  (WM_USER + 11)
#define LB_GETCOUNT    (WM_USER + 12)
#define LB_SELECTSTRING (WM_USER + 13)
#define LB_DIR         (WM_USER + 14)
#define LB_GETTOPINDEX (WM_USER + 15)
#define LB_FINDSTRING  (WM_USER + 16)
#define LB_GETSELCOUNT (WM_USER + 17)
#define LB_GETSELITEMS (WM_USER + 18)
#define LB_SETTOPINDEX (WM_USER + 24)
#define LB_GETITEMRECT (WM_USER + 25)
#define LB_GETITEMDATA (WM_USER + 26)
#define LB_SETITEMDATA (WM_USER + 27)
#define LB_SELITEMRANGE (WM_USER + 28)
#define LB_SETCARETINDEX (WM_USER + 31)
#define LB_GETCARETINDEX (WM_USER + 32)
#define LB_SETITEMHEIGHT (WM_USER + 33)
#define LB_GETITEMHEIGHT (WM_USER + 34)
#define LB_FINDSTRINGEXACT (WM_USER + 35)
#define LB_ERR         0xFFFF
#define MK_SHIFT       0x0004
#define MK_CONTROL     0x0008
#define LBN_SELCHANGE  1
#define LBN_DBLCLK     2
#define LBN_SETFOCUS   4
#define LBN_KILLFOCUS  5
#define LBN_PICKED     0x7F00         // w16's own, to a combo box: chosen with the mouse

#define CBS_SIMPLE       0x0001
#define CBS_DROPDOWN     0x0002
#define CBS_DROPDOWNLIST 0x0003
#define CBS_OWNERDRAWFIXED 0x0010
#define CBS_OWNERDRAWVARIABLE 0x0020
#define CBS_SORT         0x0100
#define CBS_HASSTRINGS   0x0200
#define WM_DRAWITEM      0x002B
#define WM_MEASUREITEM   0x002C
#define ODT_LISTBOX      2
#define ODT_COMBOBOX     3
#define ODA_DRAWENTIRE   1
#define ODS_SELECTED     0x0001
#define ODS_DISABLED     0x0004
#define ODS_FOCUS        0x0010
#define ODS_COMBOBOXEDIT 0x1000
#define CB_GETEDITSEL    (WM_USER + 0)
#define CB_LIMITTEXT     (WM_USER + 1)
#define CB_SETEDITSEL    (WM_USER + 2)
#define CB_ADDSTRING     (WM_USER + 3)
#define CB_DELETESTRING  (WM_USER + 4)
#define CB_GETCOUNT      (WM_USER + 6)
#define CB_GETCURSEL     (WM_USER + 7)
#define CB_GETLBTEXT     (WM_USER + 8)
#define CB_GETLBTEXTLEN  (WM_USER + 9)
#define CB_INSERTSTRING  (WM_USER + 10)
#define CB_RESETCONTENT  (WM_USER + 11)
#define CB_FINDSTRING    (WM_USER + 12)
#define CB_SELECTSTRING  (WM_USER + 13)
#define CB_SETCURSEL     (WM_USER + 14)
#define CB_SHOWDROPDOWN  (WM_USER + 15)
#define CB_GETITEMDATA   (WM_USER + 16)
#define CB_SETITEMDATA   (WM_USER + 17)
#define CB_GETDROPPEDCONTROLRECT (WM_USER + 18)
#define CB_GETDROPPEDSTATE (WM_USER + 23)
#define CB_FINDSTRINGEXACT (WM_USER + 24)
#define CBN_SELCHANGE    1
#define CBN_DBLCLK       2
#define CBN_SETFOCUS     3
#define CBN_KILLFOCUS    4
#define CBN_EDITCHANGE   5
#define CBN_DROPDOWN     7
#define CBN_CLOSEUP      8
#define CBN_SELENDOK     9
#define CBN_SELENDCANCEL 10
#define EN_CHANGE        0x0300
#define EM_SETSEL        (WM_USER + 1)
#define SB_LINEUP        0
#define SB_LINEDOWN      1
#define SB_PAGEUP        2
#define SB_PAGEDOWN      3
#define SB_THUMBPOSITION 4
#define SB_THUMBTRACK    5
#define LIST_ID          1000         // a combo box's list's id

// --- LISTBOX -----------------------------------------------------------------------

struct item {
    char *text;
    uint32_t data;
    int on;                           // selected, in a multiple-selection list
    int h;                            // LBS_OWNERDRAWVARIABLE: its own height, from WM_MEASUREITEM
};

struct listbox {
    struct item *it;
    int n, cap;
    int sel, caret, top;
    int focused, captured;
    int folders;                      // w16_listbox_folders()
    uint16_t combo;                   // a combo box's list: the box hears its notifications
    int owner, strings;               // owner-drawn; and keeping strings (else an item is its data)
    int oh;                           // an owner-drawn row's height, from WM_MEASUREITEM; 0 otherwise
    int variable;                     // LBS_OWNERDRAWVARIABLE: each item its own height, measured as added
    // LBS_MULTIPLESEL (1) or LBS_EXTENDEDSEL (2): each item's own `on`, and `sel` unused; a Shift
    // range runs from `anchor` to the caret, as 3.1's did.
    int multi, anchor, ctrl_drag;
};

static struct listbox *lb_of(struct w16_wnd *x) { return x->ctl; }

static int is_sel(const struct listbox *l, int i) { return l->multi ? l->it[i].on : i == l->sel; }

// A list or combo a program's template made owner-drawn, drawn by w16 instead: COMMDLG drew its own
// folder and drive lists, and a template for it asks for the style without a parent that draws.
void w16_list_not_owner_drawn(struct w16 *w, struct w16_wnd *x) {
    struct w16_wnd *lw = x->host == w16_listbox_proc ? x : w16_combo_list(w, x);
    pthread_mutex_lock(&w->lock);
    x->style &= ~(uint32_t)(LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE);
    if (lw && lb_of(lw)) {
        lb_of(lw)->owner = 0;
        lb_of(lw)->strings = 1;
        lb_of(lw)->oh = 0;
        lb_of(lw)->variable = 0;
    }
    pthread_mutex_unlock(&w->lock);
}

void w16_listbox_folders(struct w16_wnd *x, int on) {
    struct listbox *l = lb_of(x);
    if (l) l->folders = on;
}

// A row: a line of the list's font -- or the 16 of a picture, in a list of
// folders or drives, as 3.11's COMMDLG drew them.
static int item_h(const struct w16_wnd *x) {
    int h = w16_font_line_h(x ? x->font : 0);
    const struct listbox *l = x && x->host == w16_listbox_proc ? x->ctl : 0;
    if (l && l->oh) return l->oh;
    return l && l->folders == 1 && h < 16 ? 16 : h;
}

// WM_MEASUREITEM to `to`: the height a row (or, item -1, a combo's shut box) is drawn at; `h`
// when the program leaves it. `data` is the item's, for a list measured item by item.
static int measure_item(struct w16 *w, uint16_t to, uint16_t type, uint16_t id, int item, int width, int h,
                        uint32_t data) {
    uint16_t at = w16_sys_push(w, 14);
    if (!at) return h;
    uint16_t mis[7] = { type, id, (uint16_t)item, (uint16_t)width, (uint16_t)h, (uint16_t)data,
                        (uint16_t)(data >> 16) };
    memcpy(w->sysmem + at, mis, sizeof mis);
    w16_send(w, to, WM_MEASUREITEM, id, (uint32_t)w->sysdata << 16 | at);
    uint16_t got;
    memcpy(&got, w->sysmem + at + 8, 2);
    w16_sys_pop(w, at);
    return got > 0 && got < 256 ? got : h;
}

static int measure(struct w16 *w, uint16_t to, uint16_t type, uint16_t id, int item, int width, int h) {
    return measure_item(w, to, type, id, item, width, h, 0);
}

// WM_DRAWITEM to `to` for one row (or the shut box's item), into the DC of `in`.
static void draw_owned(struct w16 *w, uint16_t to, uint16_t type, uint16_t id, uint16_t ctl, int item,
                       uint16_t state, uint16_t in, int l, int t, int r, int b, uint32_t data) {
    uint16_t at = w16_sys_push(w, 26);
    if (!at) return;
    uint16_t dis[13] = { type, id, (uint16_t)item, ODA_DRAWENTIRE, state, ctl, w16_gdi_window_dc(w, in),
                         (uint16_t)l, (uint16_t)t, (uint16_t)r, (uint16_t)b, (uint16_t)data, (uint16_t)(data >> 16) };
    memcpy(w->sysmem + at, dis, sizeof dis);
    w16_send(w, to, WM_DRAWITEM, id, (uint32_t)w->sysdata << 16 | at);
    w16_sys_pop(w, at);
}

static int rows(const struct w16_wnd *x) {
    int n = x->ch / item_h(x);
    return n > 0 ? n : 1;
}

// Item i's height: its own in a variable list, else every row's.
static int row_h(const struct w16_wnd *x, const struct listbox *l, int i) {
    return l->variable && i >= 0 && i < l->n && l->it[i].h ? l->it[i].h : item_h(x);
}

// How many rows show whole from `top` down (at least one).
static int shown(const struct w16_wnd *x, const struct listbox *l, int top) {
    if (!l->variable) return rows(x);
    int n = 0;
    for (int i = top, y = 0; i < l->n && (y += row_h(x, l, i)) <= x->ch; i++) n++;
    return n > 0 ? n : 1;
}

// The top row that shows row i whole at the foot -- the furthest down the list scrolls, for the
// last row.
static int top_ending_at(const struct w16_wnd *x, const struct listbox *l, int i) {
    if (!l->variable) return i - rows(x) + 1;
    int t = i, y = row_h(x, l, i);
    while (t > 0 && y + row_h(x, l, t - 1) <= x->ch) y += row_h(x, l, --t);
    return t;
}

// Row i's top, from the client area's: the rows between it and the top row summed.
static int row_y(const struct w16_wnd *x, const struct listbox *l, int i) {
    if (!l->variable) return (i - l->top) * item_h(x);
    int y = 0;
    for (int k = l->top; k < i; k++) y += row_h(x, l, k);
    for (int k = i; k < l->top; k++) y -= row_h(x, l, k);
    return y;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int ci_prefix(const char *s, const char *p) {
    for (; *p; s++, p++) {
        int a = *s >= 'a' && *s <= 'z' ? *s - 32 : *s, b = *p >= 'a' && *p <= 'z' ? *p - 32 : *p;
        if (a != b) return 0;
    }
    return 1;
}

static int ci_cmp(const char *s, const char *t) {
    for (;; s++, t++) {
        int a = *s >= 'a' && *s <= 'z' ? *s - 32 : (unsigned char)*s;
        int b = *t >= 'a' && *t <= 'z' ? *t - 32 : (unsigned char)*t;
        if (a != b || !a) return a - b;
    }
}

static void lb_notify(struct w16 *w, struct w16_wnd *x, struct listbox *l, uint16_t code) {
    if (l->combo) w16_send(w, l->combo, WM_COMMAND, LIST_ID, (uint32_t)code << 16 | x->hwnd);
    else if (x->style & LBS_NOTIFY) w16_notify(w, x, code);
}

static void lb_bar(struct w16 *w, struct w16_wnd *x, struct listbox *l) {
    if (!(x->style & WS_VSCROLL)) return;
    int max = l->n ? top_ending_at(x, l, l->n - 1) : 0;
    w16_sbar_set(w, x, SB_VERT, 0, max > 0 ? max : 0, l->top, 1);
}

static void lb_scroll_to(struct w16 *w, struct w16_wnd *x, struct listbox *l, int top) {
    int max = l->n ? top_ending_at(x, l, l->n - 1) : 0;
    top = clampi(top, 0, max > 0 ? max : 0);
    if (top == l->top) return;
    l->top = top;
    lb_bar(w, x, l);
    w16_invalidate(w, x, 0);
}

static void lb_show(struct w16 *w, struct w16_wnd *x, struct listbox *l, int i) {
    if (i < 0) return;
    if (i < l->top) lb_scroll_to(w, x, l, i);
    else if (i >= l->top + shown(x, l, l->top)) lb_scroll_to(w, x, l, top_ending_at(x, l, i));
}

static int lb_insert(struct w16 *w, struct w16_wnd *x, struct listbox *l, int at, const char *s, int sorted) {
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 16;
        struct item *g = realloc(l->it, sizeof *g * (size_t)cap);
        if (!g) return -1;
        l->it = g;
        l->cap = cap;
    }
    char *t = strdup(s);
    if (!t) return -1;
    if (sorted) { at = 0; while (at < l->n && ci_cmp(l->it[at].text, s) <= 0) at++; }
    if (at < 0 || at > l->n) at = l->n;
    memmove(&l->it[at + 1], &l->it[at], sizeof *l->it * (size_t)(l->n - at));
    l->it[at] = (struct item){ t, 0, 0, 0 };
    l->n++;
    if (l->sel >= at) l->sel++;
    if (l->multi && l->anchor >= at && l->n > 1) l->anchor++;
    lb_bar(w, x, l);
    w16_invalidate(w, x, 0);
    return at;
}

static void lb_delete(struct w16 *w, struct w16_wnd *x, struct listbox *l, int i) {
    free(l->it[i].text);
    memmove(&l->it[i], &l->it[i + 1], sizeof *l->it * (size_t)(l->n - i - 1));
    l->n--;
    if (l->sel == i) l->sel = -1;
    else if (l->sel > i) l->sel--;
    if (l->caret >= l->n) l->caret = l->n - 1;
    lb_scroll_to(w, x, l, l->top);
    lb_bar(w, x, l);
    w16_invalidate(w, x, 0);
}

static void lb_reset(struct w16 *w, struct w16_wnd *x, struct listbox *l) {
    for (int i = 0; i < l->n; i++) free(l->it[i].text);
    l->n = 0;
    l->sel = -1;
    l->caret = 0;
    l->anchor = 0;
    l->top = 0;
    lb_bar(w, x, l);
    w16_invalidate(w, x, 0);
}

static int lb_find(struct listbox *l, int after, const char *s, int exact) {
    for (int k = 1; k <= l->n; k++) {
        int i = (after + k) % l->n;
        if (exact ? !ci_cmp(l->it[i].text, s) : ci_prefix(l->it[i].text, s)) return i;
    }
    return -1;
}

// Items lo..hi (either order) selected, every other item too when `keep`, else cleared; 1 when
// anything changed.
static int lb_set_range(struct listbox *l, int lo, int hi, int keep) {
    if (lo > hi) { int t = lo; lo = hi; hi = t; }
    int changed = 0;
    for (int k = 0; k < l->n; k++) {
        int on = (k >= lo && k <= hi) || (keep && l->it[k].on);
        if (on != l->it[k].on) { l->it[k].on = on; changed = 1; }
    }
    return changed;
}

static void lb_select(struct w16 *w, struct w16_wnd *x, struct listbox *l, int i, int notify) {
    i = l->n ? clampi(i, -1, l->n - 1) : -1;
    int changed = l->multi ? lb_set_range(l, i, i, 0) : i != l->sel;
    l->sel = i;
    if (i >= 0) l->caret = l->anchor = i;
    lb_show(w, x, l, i);
    w16_invalidate(w, x, 0);
    if (changed && notify) lb_notify(w, x, l, LBN_SELCHANGE);
}

// A hard disk for the common dialogs' drive list, 16 by 9 as 3.11 drew it.
static void drive(struct ugfx_surface *s, int x, int y) {
    uint32_t black = w16_rgb(0x000000), white = w16_rgb(0xFFFFFF), grey = w16_rgb(0x808080);
    ugfx_fill_rect(s, x, y, 16, 9, black);
    ugfx_fill_rect(s, x + 1, y + 1, 14, 7, w16_rgb(0xC0C0C0));
    ugfx_fill_rect(s, x + 1, y + 1, 13, 1, white);
    ugfx_fill_rect(s, x + 1, y + 1, 1, 6, white);
    ugfx_fill_rect(s, x + 14, y + 1, 1, 7, grey);
    ugfx_fill_rect(s, x + 1, y + 7, 14, 1, grey);
    ugfx_fill_rect(s, x + 2, y + 3, 12, 1, black);
    ugfx_fill_rect(s, x + 2, y + 5, 12, 1, black);
    ugfx_fill_rect(s, x + 2, y + 6, 2, 1, black);
}

// What goes before an item's text in a folder or drive list: its icon, and
// how far the text moves right. Item data is indent << 8 | kind: 1 an open
// folder, 2 a shut one, 3 a drive, 4 the open folder that is current.
static int item_icon(struct ugfx_surface *s, const struct listbox *l, int i, int x, int y, int ih);

// A folder for the common dialogs' folder list: open, or shut.
// SaunaOS's own folder, for when drive C: holds no COMMDLG.DLL to take 3.11's
// from: drawn here, in the same manner -- a yellow and white dither, a grey
// foot, a black edge -- but its own shapes, not traced from Microsoft's.
// `kind` 1 an open folder, 2 a shut one, 4 the open one that is current
// (its back darkened).
static void dither(struct ugfx_surface *s, int x, int y, int w, int h, uint32_t a, uint32_t b) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) ugfx_fill_rect(s, x + i, y + j, 1, 1, ((x + i + y + j) & 1) ? a : b);
}

static void folder(struct ugfx_surface *s, int x, int y, int kind) {
    uint32_t black = w16_rgb(0x000000), white = w16_rgb(0xFFFFFF), yellow = w16_rgb(0xFFFF00),
             grey = w16_rgb(0x808080);
    // The tab: 7 by 3 on the back's top-left corner.
    dither(s, x + 1, y + 1, 5, 2, yellow, white);
    ugfx_fill_rect(s, x + 1, y, 5, 1, black);
    ugfx_fill_rect(s, x, y + 1, 1, 2, black);
    ugfx_fill_rect(s, x + 6, y + 1, 1, 1, black);
    if (kind == 2) {                             // shut: the body, 15 by 10
        dither(s, x + 1, y + 3, 13, 7, yellow, white);
        ugfx_draw_rect(s, x, y + 2, 15, 10, black);
        ugfx_fill_rect(s, x + 1, y + 10, 13, 1, grey);
        return;
    }
    // Open: the back, darkened when it is the current folder, then a front
    // flap leaning out past the back's right edge.
    ugfx_fill_rect(s, x, y + 2, 14, 1, black);
    ugfx_fill_rect(s, x, y + 2, 1, 10, black);
    ugfx_fill_rect(s, x + 13, y + 2, 1, 3, black);
    dither(s, x + 1, y + 3, 12, 2, kind == 4 ? black : yellow, white);
    for (int r = 0; r < 7; r++) {
        int yy = y + 5 + r, l = x + 3 - (r + 1) / 2, rt = x + 15 - (r + 1) / 2;
        if (r == 0 || r == 6) { ugfx_fill_rect(s, l, yy, rt - l + 1, 1, black); continue; }
        ugfx_fill_rect(s, l, yy, 1, 1, black);
        ugfx_fill_rect(s, rt, yy, 1, 1, black);
        if (r == 5) ugfx_fill_rect(s, l + 1, yy, rt - l - 1, 1, grey);
        else dither(s, l + 1, yy, rt - l - 1, 1, yellow, white);
    }
}

// SaunaOS's own TrueType mark, for when drive C: has no COMMDLG.DLL: two
// capital T's, a grey one behind a black one, drawn rather than traced.
static void tt_mark(struct ugfx_surface *s, int x, int y) {
    uint32_t black = w16_rgb(0x000000), grey = w16_rgb(0x808080);
    ugfx_fill_rect(s, x + 2, y + 1, 9, 2, grey);
    ugfx_fill_rect(s, x + 5, y + 1, 3, 9, grey);
    ugfx_fill_rect(s, x + 8, y + 3, 9, 2, black);
    ugfx_fill_rect(s, x + 11, y + 3, 3, 9, black);
}

static void paint_listbox(struct w16 *w, struct w16_wnd *x, struct ugfx_surface *s, int ox, int oy) {
    (void)w;
    struct listbox *l = lb_of(x);
    uint32_t white = w16_rgb(0xFFFFFF), black = w16_rgb(0x000000), navy = w16_rgb(0x000080);
    ugfx_fill_rect(s, ox, oy, x->cw, x->ch, white);
    if (!l || l->owner) return;                  // the rows are the parent's: paint_owned()
    uint32_t ink = x->style & WS_DISABLED ? w16_syscolor(COLOR_GRAYTEXT) : black;   // solid on the white
    int ih = item_h(x);
    for (int i = l->top; i < l->n && (i - l->top) * ih < x->ch; i++) {
        int y = oy + (i - l->top) * ih, tx = ox + 2;
        int sel = is_sel(l, i);
        tx += item_icon(s, l, i, tx, y, ih);
        // A plain list's selection and focus span the row, as 3.11's; the folder lists (owner-drawn
        // in COMMDLG) light only the name.
        int tw = ugfx_text_width(l->it[i].text);
        int hx = l->folders ? tx - 1 : ox, hw = l->folders ? (tw + 2 < ox + x->cw - tx + 1 ? tw + 2 : ox + x->cw - tx + 1) : x->cw;
        if (sel) ugfx_fill_rect(s, hx, y, hw, ih, navy);
        ugfx_draw_string_clipped(s, tx, y, ox + x->cw - tx, l->it[i].text, sel ? white : ink, UGFX_TRANSPARENT);
        if (l->focused && i == l->caret) w16_draw_focus(s, hx, y, l->folders ? (tw + 2 < x->cw ? tw + 2 : x->cw - 2) : hw, ih);
    }
}

// An owner-drawn list's rows, each the parent's to draw -- a combo's, the box's parent's.
static void paint_owned(struct w16 *w, struct w16_wnd *x) {
    uint16_t h = x->hwnd;
    struct listbox *l = lb_of(x);
    struct w16_wnd *box = l && l->combo ? w16_wnd_get(w, l->combo) : 0;
    uint16_t to = box ? box->parent : x->parent, id = box ? box->id : x->id, ctl = box ? box->hwnd : h;
    uint16_t type = box ? ODT_COMBOBOX : ODT_LISTBOX;
    for (int i = l ? l->top : 0, y = 0; l && i < l->n && !w->cpu.fault; y += row_h(x, l, i), i++) {
        int ih = row_h(x, l, i);
        if (y >= x->ch) break;
        uint16_t state = (is_sel(l, i) ? ODS_SELECTED : 0) | (l->focused && i == l->caret ? ODS_FOCUS : 0) |
                         (x->style & WS_DISABLED ? ODS_DISABLED : 0);
        draw_owned(w, to, type, id, ctl, i, state, h, 0, y, x->cw, y + ih, l->it[i].data);
        if (!(x = w16_wnd_get(w, h)) || !(l = lb_of(x))) return;   // the program may have changed it
    }
}

static int item_icon(struct ugfx_surface *s, const struct listbox *l, int i, int x, int y, int ih) {
    if (!l->folders || i < 0 || i >= l->n) return 0;
    int indent = (int)(l->it[i].data >> 8 & 0xFF), kind = (int)(l->it[i].data & 0xFF);
    x += indent * 4;
    if (kind == 5 || kind == 6) {                // a font's kind: the TrueType mark, or just its room
        const uint32_t *tt = kind == 5 ? w16_commdlg_tt() : 0;
        if (tt) ugfx_blit_alpha(s, x, y + (ih - W16_TT_H) / 2, W16_TT_W, W16_TT_H, tt, W16_TT_W);
        else if (kind == 5) tt_mark(s, x, y + (ih - W16_TT_H) / 2);
        return W16_TT_W;
    }
    // 3.11's own pictures when the user's COMMDLG.DLL is on drive C:, else ours.
    static const int REAL[] = { -1, W16_ICON_OPEN, W16_ICON_SHUT, W16_ICON_DISK, W16_ICON_CURRENT };
    const uint32_t *px = kind > 0 && kind < 5 ? w16_commdlg_icon(REAL[kind]) : 0;
    if (px) ugfx_blit_alpha(s, x, y + (ih - 16) / 2, 16, 16, px, 16);
    else if (kind == 3) drive(s, x, y + (ih - 9) / 2);
    else if (kind) folder(s, x, y + (ih - 12) / 2, kind);
    return indent * 4 + 19;
}

static int lb_index_at(const struct w16_wnd *x, struct listbox *l, int y) {
    int i = l->top + (y < 0 ? -1 : y / item_h(x));
    if (l->variable && y >= 0)
        for (i = l->top; i < l->n - 1 && y >= row_h(x, l, i); i++) y -= row_h(x, l, i);
    return l->n ? clampi(i, 0, l->n - 1) : -1;
}

static void lb_capture(struct w16 *w, uint16_t hwnd) {
    pthread_mutex_lock(&w->lock);
    w->capture = hwnd;
    pthread_mutex_unlock(&w->lock);
}

// A press in a multiple-selection list. LBS_MULTIPLESEL: the item on or off. LBS_EXTENDEDSEL: the
// item alone, Ctrl adding or taking it away, Shift the run from the anchor -- 3.1's rules.
static void lb_multi_click(struct w16 *w, struct w16_wnd *x, struct listbox *l, int i, uint16_t mk) {
    if (i < 0) return;
    int changed = 1;
    l->ctrl_drag = 0;
    if (l->multi == 1 || (mk & MK_CONTROL)) {
        l->it[i].on = !l->it[i].on;
        l->anchor = i;
        l->ctrl_drag = l->multi == 2;
    } else if (mk & MK_SHIFT) changed = lb_set_range(l, l->anchor, i, 0);
    else {
        changed = lb_set_range(l, i, i, 0);
        l->anchor = i;
    }
    l->caret = i;
    lb_show(w, x, l, i);
    w16_invalidate(w, x, 0);
    if (changed) lb_notify(w, x, l, LBN_SELCHANGE);
}

// A drag after the press: an extended list's run follows the pointer from the anchor; a
// multiple-selection list only moves its caret.
static void lb_multi_drag(struct w16 *w, struct w16_wnd *x, struct listbox *l, int i) {
    if (i < 0 || i == l->caret) return;
    l->caret = i;
    int changed = l->multi == 2 && !l->ctrl_drag && lb_set_range(l, l->anchor, i, 0);
    w16_invalidate(w, x, 0);
    if (changed) lb_notify(w, x, l, LBN_SELCHANGE);
}

// An arrow key's new caret `i`. LBS_MULTIPLESEL moves the caret only (Space chooses); an extended
// list selects the item alone -- the run from the anchor with Shift, nothing with Ctrl.
static void lb_multi_key(struct w16 *w, struct w16_wnd *x, struct listbox *l, int i) {
    unsigned m = w->key_mods;
    int changed = 0;
    if (l->multi == 2 && (m & KEY_MOD_SHIFT)) changed = lb_set_range(l, l->anchor, i, 0);
    else if (l->multi == 2 && !(m & KEY_MOD_CTRL)) {
        changed = lb_set_range(l, i, i, 0);
        l->anchor = i;
    }
    l->caret = i;
    lb_show(w, x, l, i);
    w16_invalidate(w, x, 0);
    if (changed) lb_notify(w, x, l, LBN_SELCHANGE);
}

static uint32_t far_str_put(struct w16 *w, uint32_t far, const char *s) {
    size_t n = strlen(s);
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, (unsigned)n + 1, 1);
    if (!p) return LB_ERR;
    memcpy(p, s, n + 1);
    return (uint32_t)n;
}

static char *far_get(struct w16 *w, uint32_t far) {
    char *s = malloc(512);
    if (!s) return 0;
    if (!(far >> 16) || cpu16_str(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, s, 512) < 0) { free(s); return 0; }
    return s;
}

// LB_* with a C string where the program passes a far one.
static uint32_t lb_message(struct w16 *w, struct w16_wnd *x, struct listbox *l, uint16_t msg, uint16_t wp,
                           uint32_t lp, const char *str) {
    int i = (int16_t)wp;
    switch (msg) { // dispatch-ok: bounded by Windows' LB_* messages
    case LB_ADDSTRING:
        return (uint32_t)(int16_t)lb_insert(w, x, l, -1, str ? str : "", (x->style & LBS_SORT) != 0);
    case LB_INSERTSTRING:
        return (uint32_t)(int16_t)lb_insert(w, x, l, i, str ? str : "", 0);
    case LB_DELETESTRING:
        if (i < 0 || i >= l->n) return LB_ERR;
        lb_delete(w, x, l, i);
        return (uint32_t)l->n;
    case LB_RESETCONTENT:
        lb_reset(w, x, l);
        return 0;
    case LB_SETCURSEL:
        if (i >= l->n || l->multi) return LB_ERR;
        lb_select(w, x, l, i, 0);
        return i < 0 ? LB_ERR : (uint32_t)i;
    case LB_SETSEL:                              // wParam on or off, lParam the item, -1 every one
        if (l->multi) {
            int k = (int16_t)lp;
            if (k >= l->n) return LB_ERR;
            if (k < 0) { for (k = 0; k < l->n; k++) l->it[k].on = wp != 0; }
            else l->it[k].on = wp != 0;
            w16_invalidate(w, x, 0);
            return 0;
        }
        lb_select(w, x, l, wp ? (int16_t)lp : -1, 0);
        return 0;
    case LB_GETSEL:
        return i >= 0 && i < l->n ? (uint32_t)is_sel(l, i) : LB_ERR;
    case LB_GETCURSEL:                           // a multiple-selection list's: the item with the focus
        if (l->multi) return l->n ? (uint32_t)l->caret : LB_ERR;
        return l->sel < 0 ? LB_ERR : (uint32_t)l->sel;
    case LB_GETSELCOUNT: {
        if (!l->multi) return LB_ERR;
        int c = 0;
        for (int k = 0; k < l->n; k++) c += l->it[k].on;
        return (uint32_t)c;
    }
    case LB_GETSELITEMS: {                       // up to wParam indices into the int array at lParam
        if (!l->multi) return LB_ERR;
        int c = 0;
        for (int k = 0; k < l->n && c < (int)wp; k++) {
            if (!l->it[k].on) continue;
            uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(lp >> 16), (uint16_t)(lp + 2 * c), 2, 1);
            if (!p) return LB_ERR;
            p[0] = (uint8_t)k;
            p[1] = (uint8_t)(k >> 8);
            c++;
        }
        return (uint32_t)c;
    }
    case LB_SELITEMRANGE: {                      // items LOWORD..HIWORD of lParam, on or off
        if (!l->multi || !l->n) return LB_ERR;
        int lo = clampi((int)(lp & 0xFFFF), 0, l->n - 1), hi = clampi((int)(lp >> 16), 0, l->n - 1);
        if (lo > hi) { int t = lo; lo = hi; hi = t; }
        for (int k = lo; k <= hi; k++) l->it[k].on = wp != 0;
        w16_invalidate(w, x, 0);
        return 0;
    }
    case LB_GETTEXT:
        return i >= 0 && i < l->n ? far_str_put(w, lp, l->it[i].text) : LB_ERR;
    case LB_GETTEXTLEN:
        return i >= 0 && i < l->n ? (uint32_t)strlen(l->it[i].text) : LB_ERR;
    case LB_GETCOUNT:
        return (uint32_t)l->n;
    case LB_FINDSTRING:
    case LB_FINDSTRINGEXACT:
    case LB_SELECTSTRING: {
        int f = l->n && str ? lb_find(l, i < 0 ? l->n - 1 : i, str, msg == LB_FINDSTRINGEXACT) : -1;
        if (f >= 0 && msg == LB_SELECTSTRING) lb_select(w, x, l, f, 0);
        return f < 0 ? LB_ERR : (uint32_t)f;
    }
    case LB_GETTOPINDEX:
        return (uint32_t)l->top;
    case LB_SETTOPINDEX:
        lb_scroll_to(w, x, l, i);
        return 0;
    case LB_GETITEMDATA:
        return i >= 0 && i < l->n ? l->it[i].data : LB_ERR;
    case LB_SETITEMDATA:
        if (i < 0 || i >= l->n) return LB_ERR;
        l->it[i].data = lp;
        w16_invalidate(w, x, 0);
        return 0;
    case LB_GETCARETINDEX:
        return (uint32_t)l->caret;
    case LB_SETCARETINDEX:
        if (i >= 0 && i < l->n) { l->caret = i; lb_show(w, x, l, i); w16_invalidate(w, x, 0); }
        return 0;
    case LB_GETITEMHEIGHT:                       // a variable list's item wParam's
        return (uint32_t)row_h(x, l, i);
    case LB_SETITEMHEIGHT:
        if (l->variable && i >= 0 && i < l->n && (int16_t)lp > 0) {
            l->it[i].h = (int16_t)lp;
            lb_bar(w, x, l);
            w16_invalidate(w, x, 0);
        }
        return 0;
    case LB_DIR: {
        int r = str ? w16_listbox_dir(w, x, wp, str) : -1;
        return r < 0 ? LB_ERR : (uint32_t)r;
    }
    case LB_GETITEMRECT: {
        int16_t y = (int16_t)row_y(x, l, i);
        int16_t r[4] = { 0, y, (int16_t)x->cw, (int16_t)(y + row_h(x, l, i)) };
        uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(lp >> 16), (uint16_t)lp, 8, 1);
        if (p) memcpy(p, r, 8);
        return 1;
    }
    default:
        return LB_ERR;
    }
}

struct dir_add { struct w16 *w; struct w16_wnd *x; struct listbox *l; int last; };

// Sorted as the list asks, except the drives: 3.1 put them after every file and folder.
static void dir_add(void *ctx, const char *name) {
    struct dir_add *d = ctx;
    int drive = name[0] == '[' && name[1] == '-';
    d->last = lb_insert(d->w, d->x, d->l, -1, name, (d->x->style & LBS_SORT) && !drive);
}

// LB_DIR's work: a DOS spec ("*.WAV", "C:\\WINDOWS\\*.*") listed into the box by w16_dir_list();
// the index of the last item added, or LB_ERR.
int w16_listbox_dir(struct w16 *w, struct w16_wnd *x, unsigned attr, const char *spec) {
    struct listbox *l = lb_of(x);
    if (!l) return -1;
    char dir[260];
    const char *pattern = w16_dos_split(spec, dir, sizeof dir);
    struct dir_add d = { w, x, l, -1 };
    if (w16_dir_list(dir, pattern[0] ? pattern : "*.*", attr, dir_add, &d) != 0) return -1;
    return d.last;
}

static int is_string_msg(uint16_t msg) {
    return msg == LB_ADDSTRING || msg == LB_INSERTSTRING || msg == LB_FINDSTRING || msg == LB_SELECTSTRING ||
           msg == LB_FINDSTRINGEXACT || msg == LB_DIR;
}

// A message to a list box with its string argument in C, for w16's own
// code (the common dialogs): what LB_ADDSTRING and friends would do.
uint32_t w16_listbox_str(struct w16 *w, struct w16_wnd *x, uint16_t msg, uint16_t wp, const char *s) {
    struct listbox *l = lb_of(x);
    return l ? lb_message(w, x, l, msg, wp, 0, s) : LB_ERR;
}

const char *w16_listbox_text(struct w16_wnd *x, int i) {
    struct listbox *l = lb_of(x);
    return l && i >= 0 && i < l->n ? l->it[i].text : 0;
}

uint32_t w16_listbox_proc(struct w16 *w, struct w16_wnd *x, uint16_t msg, uint16_t wp, uint32_t lp) {
    struct listbox *l = lb_of(x);
    if (!l && msg == WM_NCCREATE) {
        if (!(x->ctl = calloc(1, sizeof(struct listbox)))) return 0;
        lb_of(x)->sel = -1;
        lb_of(x)->multi = x->style & LBS_EXTENDEDSEL ? 2 : x->style & LBS_MULTIPLESEL ? 1 : 0;
        lb_of(x)->owner = (x->style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)) != 0;
        lb_of(x)->strings = !lb_of(x)->owner || (x->style & LBS_HASSTRINGS);
        // A FIXED list's row is measured once, now; a VARIABLE one's items as each is added, which
        // is when Windows asked -- Control Panel's Fonts measures an item from what its
        // WM_INITDIALOG filled in, and asked first it reads nothing.
        lb_of(x)->variable = lb_of(x)->owner && (x->style & LBS_OWNERDRAWVARIABLE);
        if (lb_of(x)->owner && !lb_of(x)->variable)
            lb_of(x)->oh = measure(w, x->parent, ODT_LISTBOX, x->id, 0, x->cw, w16_font_line_h(x->font));
        // An empty list shows no scroll bar, unless it asks to keep it.
        x->keep_sb = (x->style & LBS_DISABLENOSCROLL) != 0;
        x->sb[SB_VERT] = (struct w16_sbar){ 0, 0, 0 };
        w16_wnd_set_geometry(w, x, x->x, x->y, x->w, x->h);
        // Whole rows only, the foot moved up, unless LBS_NOINTEGRALHEIGHT --
        // 3.11's Open lists are 106 tall where their template asks 111. A variable list's rows
        // differ, so it keeps its height (Fonts' Sample box).
        if (!(x->style & LBS_NOINTEGRALHEIGHT) && !lb_of(x)->variable) {
            int ih = item_h(x), frame = x->h - x->ch, rows = x->ch / ih;
            if (rows > 0 && rows * ih != x->ch) w16_wnd_set_geometry(w, x, x->x, x->y, x->w, rows * ih + frame);
        }
        return 1;
    }
    if (!l) return w16_def_window_proc(w, x, msg, wp, lp);
    if (msg >= LB_ADDSTRING && msg <= LB_FINDSTRINGEXACT) {
        int adding = msg == LB_ADDSTRING || msg == LB_INSERTSTRING;
        uint32_t r;
        if (!l->strings && adding) {             // the "string" is the item's data
            r = lb_message(w, x, l, msg, wp, lp, "");
            if ((int16_t)r >= 0 && (int16_t)r < l->n) l->it[(int16_t)r].data = lp;
        } else {
            char *s = is_string_msg(msg) && (l->strings || msg == LB_DIR) ? far_get(w, lp) : 0;
            r = lb_message(w, x, l, msg, wp, lp, s);
            free(s);
        }
        if (adding && l->variable && (int16_t)r >= 0 && (int16_t)r < l->n) {
            l->it[(int16_t)r].h = measure_item(w, x->parent, ODT_LISTBOX, x->id, (int16_t)r, x->cw,
                                               w16_font_line_h(x->font), l->it[(int16_t)r].data);
            lb_bar(w, x, l);
        }
        return r;
    }
    switch (msg) { // dispatch-ok: a window procedure, bounded by the messages Windows' LISTBOX answered
    case WM_PAINT:
        w16_ctl_paint(w, x, paint_listbox);
        if (l->owner) paint_owned(w, x);
        return 0;
    case WM_SIZE:
        lb_scroll_to(w, x, l, l->top);
        lb_bar(w, x, l);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        l->focused = msg == WM_SETFOCUS;
        w16_invalidate(w, x, 0);
        lb_notify(w, x, l, l->focused ? LBN_SETFOCUS : LBN_KILLFOCUS);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        int16_t mx = (int16_t)lp, my = (int16_t)(lp >> 16);
        if (l->combo && (mx < 0 || my < 0 || mx >= x->w || my >= x->h)) {   // outside an open list: shut it
            w16_send(w, l->combo, CB_SHOWDROPDOWN, 0, 0);
            return 0;
        }
        if (!l->combo) w16_set_focus(w, x->hwnd);
        l->captured = 1;
        lb_capture(w, x->hwnd);
        if (l->multi) lb_multi_click(w, x, l, lb_index_at(x, l, my), wp);
        else lb_select(w, x, l, lb_index_at(x, l, my), 1);
        if (msg == WM_LBUTTONDBLCLK) lb_notify(w, x, l, LBN_DBLCLK);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (l->captured || (l->combo && (wp & 1))) {
            int16_t my = (int16_t)(lp >> 16);
            int i = lb_index_at(x, l, my);
            if (my < 0 && l->top > 0) lb_scroll_to(w, x, l, l->top - 1);
            if (my >= x->ch) lb_scroll_to(w, x, l, l->top + 1);
            if (l->multi) lb_multi_drag(w, x, l, i);
            else lb_select(w, x, l, i, 1);
        }
        return 0;
    case WM_LBUTTONUP:
        if (l->captured) {
            l->captured = 0;
            // A drop-down's list keeps the capture until it shuts; a simple box's never shuts.
            struct w16_wnd *box = l->combo ? w16_wnd_get(w, l->combo) : 0;
            if (!l->combo || (box && (box->style & 3) == CBS_SIMPLE)) lb_capture(w, 0);
            int16_t mx = (int16_t)lp, my = (int16_t)(lp >> 16);
            if (l->combo && mx >= 0 && my >= 0 && mx < x->cw && my < x->ch) lb_notify(w, x, l, LBN_PICKED);
        }
        return 0;
    case WM_KEYDOWN: {
        int vis = shown(x, l, l->top), i = l->sel >= 0 && !l->multi ? l->sel : l->caret;
        switch (wp) {
        case VK_UP:    i--; break;
        case VK_DOWN:  i = l->sel < 0 && !l->multi ? 0 : i + 1; break;
        case VK_PRIOR: i -= vis - 1; break;
        case VK_NEXT:  i += vis - 1; break;
        case VK_HOME:  i = 0; break;
        case VK_END:   i = l->n - 1; break;
        default: return 0;
        }
        if (l->multi && l->n) lb_multi_key(w, x, l, clampi(i, 0, l->n - 1));
        else if (l->n) lb_select(w, x, l, clampi(i, 0, l->n - 1), 1);
        return 0;
    }
    case WM_CHAR:
        if (wp == ' ' && l->multi && l->n) {     // the caret's item on or off
            l->it[l->caret].on = !l->it[l->caret].on;
            l->anchor = l->caret;
            w16_invalidate(w, x, 0);
            lb_notify(w, x, l, LBN_SELCHANGE);
            return 0;
        }
        if (wp > ' ' && wp < 0x100 && l->n) {
            char k[2] = { (char)wp, 0 };
            int f = lb_find(l, l->sel < 0 ? l->n - 1 : l->sel, k, 0);
            if (f >= 0) lb_select(w, x, l, f, 1);
        }
        return 0;
    case WM_VSCROLL: {
        int vis = shown(x, l, l->top), top = l->top;
        switch (wp) {
        case SB_LINEUP: top--; break;
        case SB_LINEDOWN: top++; break;
        case SB_PAGEUP: top -= vis - 1; break;
        case SB_PAGEDOWN: top += vis - 1; break;
        case SB_THUMBPOSITION: case SB_THUMBTRACK: top = (int16_t)lp; break;
        default: return 0;
        }
        lb_scroll_to(w, x, l, top);
        return 0;
    }
    case WM_NCDESTROY:
        if (l->captured) lb_capture(w, 0);
        for (int i = 0; i < l->n; i++) free(l->it[i].text);
        free(l->it);
        free(x->ctl);
        x->ctl = 0;
        return 0;
    default:
        return w16_def_window_proc(w, x, msg, wp, lp);
    }
}

// --- COMBOBOX ----------------------------------------------------------------------

struct combo {
    uint16_t list, edit;
    int dropped, sel_before, focused;
    int open_h;                       // its height open: the template's
    int oh;                           // owner-drawn: the shut box's item height, from WM_MEASUREITEM
};

static struct combo *cb_of(struct w16_wnd *x) { return x->ctl; }

static int shut_h(const struct w16_wnd *x) {   // 3.11's: 20 in a Helv 8 dialog
    const struct combo *c = x->ctl;
    return (c && c->oh ? c->oh : item_h(x)) + 7;
}

static struct w16_wnd *cb_list(struct w16 *w, struct combo *c) { return w16_wnd_get(w, c->list); }

static int cb_type(const struct w16_wnd *x) { return (int)(x->style & 3); }

static void cb_show_text(struct w16 *w, struct w16_wnd *x, struct combo *c) {
    struct w16_wnd *l = cb_list(w, c), *e = w16_wnd_get(w, c->edit);
    if (!e || !l) { w16_invalidate(w, x, 0); return; }
    struct listbox *lb = lb_of(l);
    const char *t = lb && lb->sel >= 0 ? lb->it[lb->sel].text : "";
    size_t n = strlen(t);
    uint16_t at = w16_sys_push(w, (unsigned)n + 1);
    if (w->cpu.fault) return;
    memcpy(w->sysmem + at, t, n + 1);
    w16_send(w, e->hwnd, WM_SETTEXT, 0, (uint32_t)w->sysdata << 16 | at);
    w16_sys_pop(w, at);
    w16_send(w, e->hwnd, EM_SETSEL, 0, 0xFFFF0000u);
}

static void drop(struct w16 *w, struct w16_wnd *x, struct combo *c, int open) {
    struct w16_wnd *l = cb_list(w, c);
    if (!l || cb_type(x) == CBS_SIMPLE || open == c->dropped) return;
    if (open) w16_notify(w, x, CBN_DROPDOWN);
    // Below the box when it fits in the parent, else above it, as Windows
    // opened one near the screen's foot.
    struct w16_wnd *p = w16_wnd_get(w, x->parent);
    if (open && p) {
        int below = x->y + x->h, ny = below + l->h <= p->ch || x->y - l->h < 0 ? below : x->y - l->h;
        if (ny != l->y) w16_wnd_set_geometry(w, l, l->x, ny, l->w, l->h);
    }
    pthread_mutex_lock(&w->lock);
    c->dropped = open;
    l->visible = open;
    if (open) { l->style |= WS_VISIBLE; l->order = ~0u >> 1; }   // on top of its siblings
    else l->style &= ~WS_VISIBLE;
    w->capture = open ? l->hwnd : (w->capture == l->hwnd ? 0 : w->capture);
    pthread_mutex_unlock(&w->lock);
    struct listbox *lb = lb_of(l);
    if (open && lb) { c->sel_before = lb->sel; lb->captured = 0; lb_show(w, l, lb, lb->sel); }
    if (open) w16_invalidate(w, l, 1);
    else w16_invalidate(w, w16_wnd_get(w, x->parent), 1);
    w16_invalidate(w, x, 0);
    if (!open) w16_notify(w, x, CBN_CLOSEUP);
}

static void paint_combo(struct w16 *w, struct w16_wnd *x, struct ugfx_surface *s, int ox, int oy) {
    struct combo *c = cb_of(x);
    uint32_t white = w16_rgb(0xFFFFFF), black = w16_rgb(0x000000), navy = w16_rgb(0x000080);
    int bw = W16_SB, h = x->ch;                  // the button 17 wide, 3.11's
    ugfx_fill_rect(s, ox, oy, x->cw, h, w16_parent_bg(w, x));
    if (cb_type(x) == CBS_DROPDOWNLIST) {
        ugfx_fill_rect(s, ox, oy, x->cw - bw, h, white);
        ugfx_draw_rect(s, ox, oy, x->cw - bw + 1, h, black);
        struct w16_wnd *l = c ? cb_list(w, c) : 0;
        struct listbox *lb = l ? lb_of(l) : 0;
        if (lb && lb->sel >= 0 && !lb->owner) {
            const char *t = lb->it[lb->sel].text;
            int tw = ugfx_text_width(t), ty = oy + (h - item_h(x)) / 2;
            int ix = item_icon(s, lb, lb->sel, ox + 4, ty, item_h(x)), tx = ox + 4 + ix;
            if (c->focused) ugfx_fill_rect(s, tx - 2, ty, tw + 4 < x->cw - bw - 4 - ix ? tw + 4 : x->cw - bw - 4 - ix, item_h(x), navy);
            ugfx_draw_string_clipped(s, tx, ty, x->cw - bw - 6 - ix, t, c->focused ? white : black, UGFX_TRANSPARENT);
        }
    }
    if (cb_type(x) != CBS_SIMPLE) {             // the button: a down arrow over a bar, 3.1's
        int bx = ox + x->cw - bw;
        ugfx_draw_rect(s, bx, oy, bw, h, black);
        w16_draw_bevel(s, bx + 1, oy + 1, bw - 2, h - 2, c && c->dropped);
        int cx = bx + bw / 2, cy = oy + h / 2 - 2;
        ugfx_fill_rect(s, cx - 1, cy - 3, 3, 3, black);      // the stem, the head, then the bar
        for (int i = 0; i < 4; i++) ugfx_fill_rect(s, cx - 3 + i, cy + i, 7 - 2 * i, 1, black);
        ugfx_fill_rect(s, cx - 3, cy + 5, 7, 1, black);
    }
}

uint32_t w16_combo_proc(struct w16 *w, struct w16_wnd *x, uint16_t msg, uint16_t wp, uint32_t lp) {
    struct combo *c = cb_of(x);
    if (!c && msg == WM_NCCREATE) return (x->ctl = calloc(1, sizeof(struct combo))) != 0;
    if (!c) return w16_def_window_proc(w, x, msg, wp, lp);
    struct w16_wnd *l = cb_list(w, c);
    switch (msg) {
    case WM_CREATE: {
        // Shut at one row; the list takes the rest of the template's height,
        // and the scroll bar a template asks for is the list's.
        c->open_h = x->h;
        pthread_mutex_lock(&w->lock);
        x->style &= ~(WS_VSCROLL | WS_HSCROLL);
        pthread_mutex_unlock(&w->lock);
        int owner = (x->style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)) != 0, row_h = 0;
        if (owner) {                              // the shut box's item (-1), then a row of the list
            c->oh = measure(w, x->parent, ODT_COMBOBOX, x->id, -1, x->cw - W16_SB, item_h(x));
            row_h = measure(w, x->parent, ODT_COMBOBOX, x->id, 0, x->cw - W16_SB, item_h(x));
            if (!(x = w16_wnd_get(w, x->hwnd)) || !(c = cb_of(x))) return 0xFFFFFFFFu;
        }
        // A simple box (CBS_SIMPLE) keeps its list open under the edit, a
        // pixel's gap between them as 3.1 drew it, within the template's height.
        int simple = cb_type(x) == CBS_SIMPLE, sh = shut_h(x), gap = simple ? 1 : 0;
        w16_wnd_set_geometry(w, x, x->x, x->y, x->w, sh);
        int k = w16_class_find(w, "LISTBOX"), ek = w16_class_find(w, "EDIT");
        int room = c->open_h - sh - gap;
        int lh = room > item_h(x) * 2 + 2 ? room : item_h(x) * 4 + 2;
        uint32_t ls = WS_CHILD | WS_BORDER | WS_VSCROLL | ((x->style & CBS_SORT) ? LBS_SORT : 0);
        if (simple) ls |= WS_VISIBLE | LBS_DISABLENOSCROLL;
        struct w16_wnd *lw = w16_wnd_create(w, k, "", ls, x->x, x->y + sh + gap, x->w - (simple ? 0 : W16_SB),
                                            lh, x->parent, LIST_ID, x->inst, 0);
        if (!lw) return 0xFFFFFFFFu;
        c->list = lw->hwnd;
        lb_of(lw)->combo = x->hwnd;
        if (owner) {
            lb_of(lw)->owner = 1;
            lb_of(lw)->strings = (x->style & CBS_HASSTRINGS) != 0;
            lb_of(lw)->oh = row_h;
        }
        if (cb_type(x) != CBS_DROPDOWNLIST && ek >= 0) {
            struct w16_wnd *e = w16_wnd_create(w, ek, "", WS_CHILD | WS_VISIBLE | WS_BORDER | 0x0080, 0, 0,
                                               x->w - (cb_type(x) == CBS_SIMPLE ? 0 : W16_SB), sh, x->hwnd, 1001,
                                               x->inst, 0);
            if (e) c->edit = e->hwnd;
        }
        return 0;
    }
    case WM_PAINT: {
        w16_ctl_paint(w, x, paint_combo);
        struct listbox *lb = l ? lb_of(l) : 0;
        if (lb && lb->owner && cb_type(x) == CBS_DROPDOWNLIST && lb->sel >= 0 && lb->sel < lb->n) {
            uint16_t state = ODS_COMBOBOXEDIT | (c->focused ? ODS_SELECTED | ODS_FOCUS : 0) |
                             (x->style & WS_DISABLED ? ODS_DISABLED : 0);
            draw_owned(w, x->parent, ODT_COMBOBOX, x->id, x->hwnd, lb->sel, state, x->hwnd, 2, 2,
                       x->cw - W16_SB - 1, x->ch - 2, lb->it[lb->sel].data);
        }
        return 0;
    }
    case WM_GETDLGCODE:
        // OPEN, ENTER AND ESC ARE THE LIST'S -- they shut it -- not the dialog's default button and Cancel.
        return DLGC_WANTARROWS | DLGC_WANTCHARS | (c->dropped && (wp == 0x0D || wp == 0x1B) ? DLGC_WANTMESSAGE : 0);
    case WM_SETFOCUS:
        if (c->edit) { w16_set_focus(w, c->edit); return 0; }
        c->focused = 1;
        w16_invalidate(w, x, 0);
        w16_notify(w, x, CBN_SETFOCUS);
        return 0;
    case WM_KILLFOCUS:
        c->focused = 0;
        drop(w, x, c, 0);
        w16_invalidate(w, x, 0);
        w16_notify(w, x, CBN_KILLFOCUS);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (!c->edit) w16_set_focus(w, x->hwnd);
        drop(w, x, c, !c->dropped);
        return 0;
    case WM_KEYDOWN:
        if (wp == 0x73 || ((w->key_mods & KEY_MOD_ALT) && (wp == VK_DOWN || wp == VK_UP))) {   // F4, Alt+arrow
            drop(w, x, c, !c->dropped);
            return 0;
        }
        if (c->dropped && (wp == 0x0D || wp == 0x1B)) {
            if (wp == 0x1B && l && lb_of(l)) lb_select(w, l, lb_of(l), c->sel_before, 0);
            drop(w, x, c, 0);
            w16_notify(w, x, wp == 0x0D ? CBN_SELENDOK : CBN_SELENDCANCEL);
            if (wp == 0x0D) w16_notify(w, x, CBN_SELCHANGE);
            return 0;
        }
        if (l) w16_send(w, l->hwnd, WM_KEYDOWN, wp, lp);   // up and down step through the list
        return 0;
    case WM_CHAR:
        if (l && !c->edit) w16_send(w, l->hwnd, WM_CHAR, wp, lp);
        return 0;
    case WM_COMMAND:
        if (wp == LIST_ID) {                     // from the list
            uint16_t code = (uint16_t)(lp >> 16);
            if (code == LBN_SELCHANGE) {
                cb_show_text(w, x, c);
                w16_invalidate(w, x, 0);
                if (!c->dropped) w16_notify(w, x, CBN_SELCHANGE);
            } else if (code == LBN_PICKED || code == LBN_DBLCLK) {
                drop(w, x, c, 0);
                w16_notify(w, x, CBN_SELENDOK);
                w16_notify(w, x, CBN_SELCHANGE);
                if (code == LBN_DBLCLK) w16_notify(w, x, CBN_DBLCLK);
            }
        } else if (wp == 1001 && (lp >> 16) == EN_CHANGE) {
            w16_notify(w, x, CBN_EDITCHANGE);
        }
        return 0;
    case CB_SHOWDROPDOWN:
        drop(w, x, c, wp != 0);
        return 1;
    case CB_GETDROPPEDSTATE:
        return (uint32_t)c->dropped;
    case CB_GETDROPPEDCONTROLRECT: {
        int16_t r[4] = { 0, 0, (int16_t)x->w, (int16_t)c->open_h };
        uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(lp >> 16), (uint16_t)lp, 8, 1);
        if (p) memcpy(p, r, 8);
        return 1;
    }
    case CB_LIMITTEXT: case CB_GETEDITSEL: case CB_SETEDITSEL:
        return c->edit ? w16_send(w, c->edit, msg == CB_LIMITTEXT ? WM_USER + 21 : msg == CB_GETEDITSEL ? WM_USER : WM_USER + 1,
                                  wp, lp) : 0;
    case WM_SETTEXT: case WM_GETTEXT: case WM_GETTEXTLENGTH:
        if (c->edit) return w16_send(w, c->edit, msg, wp, lp);
        if (msg == WM_GETTEXT || msg == WM_GETTEXTLENGTH) {
            struct listbox *lb = l ? lb_of(l) : 0;
            const char *t = lb && lb->sel >= 0 ? lb->it[lb->sel].text : "";
            if (msg == WM_GETTEXTLENGTH) return (uint32_t)strlen(t);
            size_t n = strlen(t);
            if (!wp) return 0;
            if (n > wp - 1u) n = wp - 1u;
            uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(lp >> 16), (uint16_t)lp, (unsigned)n + 1, 1);
            if (!p) return 0;
            memcpy(p, t, n);
            p[n] = 0;
            return (uint32_t)n;
        }
        return 0;
    case WM_NCDESTROY:
        free(x->ctl);
        x->ctl = 0;
        return 0;
    default:
        break;
    }
    // CB_* that are the list's LB_*, passed on.
    static const uint16_t map[][2] = {
        { CB_ADDSTRING, LB_ADDSTRING }, { CB_DELETESTRING, LB_DELETESTRING }, { CB_GETCOUNT, LB_GETCOUNT },
        { CB_GETCURSEL, LB_GETCURSEL }, { CB_GETLBTEXT, LB_GETTEXT }, { CB_GETLBTEXTLEN, LB_GETTEXTLEN },
        { CB_INSERTSTRING, LB_INSERTSTRING }, { CB_RESETCONTENT, LB_RESETCONTENT },
        { CB_FINDSTRING, LB_FINDSTRING }, { CB_SELECTSTRING, LB_SELECTSTRING }, { CB_SETCURSEL, LB_SETCURSEL },
        { CB_GETITEMDATA, LB_GETITEMDATA }, { CB_SETITEMDATA, LB_SETITEMDATA },
        { CB_FINDSTRINGEXACT, LB_FINDSTRINGEXACT },
    };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (map[i][0] == msg) {
            if (!l) return LB_ERR;
            uint32_t r = w16_send(w, l->hwnd, map[i][1], wp, lp);
            if (msg == CB_SETCURSEL || msg == CB_SELECTSTRING || msg == CB_RESETCONTENT) {
                cb_show_text(w, x, c);
                w16_invalidate(w, x, 0);
            }
            return r;
        }
    return w16_def_window_proc(w, x, msg, wp, lp);
}

// The combo box's list, for w16's own code: its strings set in C.
struct w16_wnd *w16_combo_list(struct w16 *w, struct w16_wnd *x) {
    struct combo *c = cb_of(x);
    return c ? cb_list(w, c) : 0;
}

void w16_combo_changed(struct w16 *w, struct w16_wnd *x) {
    struct combo *c = cb_of(x);
    if (c) cb_show_text(w, x, c);
    w16_invalidate(w, x, 0);
}
