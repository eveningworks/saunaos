// dialog16.c -- USER's dialog manager, on the task thread: a dialog from a
// DIALOG resource (DialogBox, CreateDialog) or built in code (MessageBox,
// ShellAbout, the common dialogs), its controls made as child windows of
// w16's own classes or the program's, and 3.1's dialog keyboard.
//
// A DIALOG IS A TOP-LEVEL WINDOW of class #32770, whose window procedure
// -- DefDlgProc, here -- hands each message to the program's DLGPROC
// first (or w16's own, for a dialog built here). Its client buffer is
// painted on this thread like the main window's; dlgwin16.c draws the
// frame round it and turns input into messages. A modal one runs its own
// message loop until EndDialog; a modeless one is fed by the program's
// loop through IsDialogMessage.
//
// 3.1'S DIALOG KEYBOARD is IsDialogMessage's, before any control hears a
// key -- unless the control's WM_GETDLGCODE keeps it: Tab and Shift+Tab
// move between WS_TABSTOP controls, the arrows within a group, Enter is
// the default button, Esc is IDCANCEL, and a control's mnemonic -- or the
// mnemonic of the label before it -- goes to it.
#include "w16_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "keyboard.h"
#include "ui/ugfx.h"
#include "version.h"        // TOYOS_VERSION, generated -- tools/gen_version.sh

#define DS_SETFONT    0x40
#define BS_TYPE       0x0F
#define BS_PUSHBUTTON 0x00
#define BS_DEFPUSH    0x01
#define BS_AUTORADIOBUTTON 0x09
#define BM_SETCHECK   (WM_USER + 1)
#define BM_GETCHECK   (WM_USER + 0)
#define BM_SETSTYLE   (WM_USER + 4)
#define EM_SETSEL     (WM_USER + 1)
#define DM_GETDEFID   (WM_USER + 0)
#define DM_SETDEFID   (WM_USER + 1)
#define DC_HASDEFID   0x534B
#define SS_TYPE       0x0F
#define SS_ICON       0x03
#define SS_NOPREFIX   0x80
#define BN_CLICKED    0
#define IDOK          1
#define IDCANCEL      2
#define VK_TAB        0x09
#define VK_RETURN     0x0D
#define VK_ESCAPE     0x1B
#define COLOR_WINDOW  5

// --- the far pointers the program hands over ------------------------------------

static int get_str(struct w16 *w, uint32_t far, char *out, unsigned cap) {
    if (!(far >> 16)) { out[0] = 0; return -1; }
    return cpu16_str(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, out, cap) < 0 ? -1 : 0;
}

// A string the program passes, however long, as an allocation.
static char *dup_far(struct w16 *w, uint32_t far) {
    if (!(far >> 16)) return strdup("");
    size_t cap = 256;
    for (;;) {
        char *s = malloc(cap);
        if (!s) return 0;
        int n = cpu16_str(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, s, (unsigned)cap);
        if (n < 0) { free(s); return 0; }
        if ((size_t)n < cap - 1 || cap >= 65536) return s;
        free(s);
        cap *= 4;
    }
}

// A window's text as w16 reads it: an EDIT's own, else its label.
static char *text_of(struct w16 *w, struct w16_wnd *x) {
    if (x->host == w16_edit_proc) return w16_edit_text(w, x);
    return strdup(x->text ? x->text : "");
}

// WM_SETTEXT with a C string, through w16's scratch area.
static void set_text(struct w16 *w, struct w16_wnd *x, const char *t) {
    size_t n = strlen(t);
    if (n > 2000) n = 2000;
    uint16_t at = w16_sys_push(w, (unsigned)n + 1);
    if (w->cpu.fault) return;
    memcpy(w->sysmem + at, t, n);
    w->sysmem[at + n] = 0;
    w16_send(w, x->hwnd, WM_SETTEXT, 0, (uint32_t)w->sysdata << 16 | at);
    w16_sys_pop(w, at);
}

// --- a dialog and its controls ---------------------------------------------------------

// 3.11's dialog frame, measured at VGA: a black line and 4 of navy all
// round. A caption sits on the client's width, its top row white (so the
// head shows 3 navy and a white line). The band is DS_MODALFRAME's -- or
// a WS_DLGFRAME with no WS_BORDER; WS_CAPTION alone (both bits) is the
// black line, as Hearts' dialogs show on the reference.
static int dlgframe(uint32_t style) {
    return (style & DS_MODALFRAME) || (style & (WS_DLGFRAME | WS_BORDER)) == WS_DLGFRAME;
}
static int frame_of(uint32_t style) { return dlgframe(style) ? 5 : 1; }

int w16_dlg_frame(const struct w16_wnd *d) { return frame_of(d->style); }
int w16_dlg_frame_top(const struct w16_wnd *d) { return frame_of(d->style); }
int w16_dlg_caption(const struct w16_wnd *d) {
    return (d->style & WS_CAPTION) == WS_CAPTION ? W16_CAPTION : 0;   // 18 rows and the line under them
}

void w16_dlg_set_font(struct w16_wnd *d, int pt) {
    if (pt <= 0) pt = 8;
    d->font = W16_FONT_DIALOG;
    d->bx = (7 * pt + 4) / 8;
    d->by = (13 * pt + 4) / 8;
}

void w16_units_of(struct w16 *w, const struct w16_wnd *x, int *bx, int *by) {
    for (int depth = 0; x && depth < 64; depth++) {
        if (w16_is_dialog(x) && x->bx) { *bx = x->bx; *by = x->by; return; }
        if (x->kind != W16_K_CHILD) break;
        x = w16_wnd_get(w, x->parent);
    }
    w16_dlg_units(bx, by);
}

struct w16_wnd *w16_dlg_new(struct w16 *w, uint16_t owner, uint16_t inst, uint32_t style, int cw, int ch,
                            const char *caption, uint32_t dlgproc, w16_dlgproc hostdlg) {
    int k = w16_class_find(w, "#32770");
    struct w16_wnd *d = k >= 0 ? w16_wnd_new(w, W16_K_DIALOG) : 0;
    if (!d) return 0;
    d->cls = k;
    d->host = w16_dialog_proc;
    d->parent = owner;
    d->inst = inst;
    d->style = style & ~WS_VISIBLE;
    d->dlgproc = dlgproc;
    d->hostdlg = hostdlg;
    d->cw = d->fbw = cw > 1 ? cw : 1;
    d->ch = d->fbh = ch > 1 ? ch : 1;
    d->w = d->cw + 2 * frame_of(style);
    d->h = d->ch + w16_dlg_frame_top(d) + w16_dlg_caption(d) + frame_of(style);
    d->text = strdup(caption ? caption : "");
    d->fb = calloc((size_t)d->fbw * d->fbh + 1, 4);
    if (!d->text || !d->fb) { w16_wnd_free(w, d); return 0; }
    // The dialog hears its own creation before any control exists, as
    // Windows' CreateDialog sends it: a hook watching for the next window
    // made (MFC's, attaching its CDialog) must find the dialog, not its
    // first control.
    uint16_t at = w16_sys_push(w, 34), h = d->hwnd;
    if (w->cpu.fault) return 0;
    uint16_t cs[17] = { 0 };
    cs[2] = inst;                                // CREATESTRUCT: lpCreateParams, hInstance, ...
    cs[5] = (uint16_t)d->h; cs[6] = (uint16_t)d->w;
    cs[9] = (uint16_t)style; cs[10] = (uint16_t)(style >> 16);
    memcpy(w->sysmem + at, cs, sizeof cs);
    uint32_t lp = (uint32_t)w->sysdata << 16 | at;
    int ok = w16_send(w, h, WM_NCCREATE, 0, lp) != 0 && !w->cpu.fault &&
             w16_send(w, h, WM_CREATE, 0, lp) != 0xFFFFFFFFu && !w->cpu.fault;
    w16_sys_pop(w, at);
    if (!ok) {
        if ((d = w16_wnd_get(w, h))) w16_wnd_free(w, d);
        return 0;
    }
    return w16_wnd_get(w, h);
}

struct w16_wnd *w16_dlg_add(struct w16 *w, struct w16_wnd *d, const char *cls, uint16_t id, uint32_t style,
                            int x, int y, int cw, int ch, const char *text) {
    int k = w16_class_find(w, cls);
    if (k < 0) return 0;
    return w16_wnd_create(w, k, text, style | WS_CHILD, x, y, cw, ch, d->hwnd, id, d->inst, 0);
}

// Dialog units to pixels AS 3.11 MAPPED THEM, measured off its Open and
// Find (docs/w16-design.md): an x from the template's own position, each
// end truncated -- Find Next (182 at 30) at 371 - 52 = 319, Open's name
// field (6 at 36) at 73 - 63 = 10, half a pixel each going opposite ways;
// a y rounded, as MulDiv rounds (Find's Cancel at 37, its radio buttons at
// 62); a width or height rounded likewise -- Find is 413 by 101, a button
// 88 by 23. Every place measured fits these, and no single rule fits both axes.
// THE ORIGIN IS AN OWNED DIALOG'S ONLY: Character Map's main window (owned by
// nobody, at x 9) maps an x as MulDiv rounds it, as it does a width -- its
// labels at 3 * 7 / 4 = 5, not the 6 an origin of 9 gives, its edit at 403.
int w16_du_x(int org, int x, int bx) { return (org + x) * bx / 4 - org * bx / 4; }
int w16_du_y(int org, int y, int by) { (void)org; return (y * by + 4) / 8; }
int w16_du_w(int cx, int bx) { return (cx * bx + 2) / 4; }
int w16_du_h(int cy, int by) { return (cy * by + 4) / 8; }

// A template's control, then WM_SETFONT with the dialog's font when it has
// one, as 3.1's CreateDialog sent it.
static struct w16_wnd *add_with_font(struct w16 *w, struct w16_wnd *d, const char *cls, uint16_t id,
                                     uint32_t style, int x, int y, int cw, int ch, const char *text) {
    struct w16_wnd *c = w16_dlg_add(w, d, cls, id, style, x, y, cw, ch, text);
    if (c && d->font) {
        uint16_t h = c->hwnd;
        w16_send(w, h, WM_SETFONT, d->font, 0);
        c = w16_wnd_get(w, h);
    }
    return c;
}

struct reader {
    const uint8_t *p;
    uint32_t n, at;
    int bad;
};

static unsigned rd8(struct reader *r) {
    if (r->at + 1 > r->n) { r->bad = 1; return 0; }
    return r->p[r->at++];
}
static unsigned rd16(struct reader *r) { unsigned v = rd8(r); return v | rd8(r) << 8; }
static uint32_t rd32(struct reader *r) { uint32_t v = rd16(r); return v | (uint32_t)rd16(r) << 16; }

// A NUL-terminated string in the template, allocated.
static char *rd_str(struct reader *r) {
    uint32_t start = r->at;
    while (r->at < r->n && r->p[r->at]) r->at++;
    if (r->at >= r->n) { r->bad = 1; return 0; }
    char *s = malloc(r->at - start + 1);
    if (s) { memcpy(s, r->p + start, r->at - start); s[r->at - start] = 0; }
    r->at++;
    return s;
}

// A name or an ordinal (0xFF then a word): the ordinal's text is "".
static char *rd_name(struct reader *r, unsigned *ord) {
    *ord = 0;
    if (r->at < r->n && r->p[r->at] == 0xFF) {
        r->at++;
        *ord = rd16(r);
        return strdup("");
    }
    return rd_str(r);
}

static const char *const ORD_CLASS[] = { "BUTTON", "EDIT", "STATIC", "LISTBOX", "SCROLLBAR", "COMBOBOX" };

// The program's main window from a template naming its class: the frame,
// caption and menu bar round the template's client size, as CreateWindow
// would make it, then the dialog manager's fields.
static struct w16_wnd *dlg_main(struct w16 *w, int k, uint32_t style, int cw, int ch, const char *menu,
                                unsigned menu_id, const char *caption, uint16_t inst, uint32_t dlgproc) {
    uint16_t hm = menu[0] || menu_id ? w16_load_menu(w, menu[0] ? menu : 0, (uint16_t)menu_id) : 0;
    int f = style & WS_THICKFRAME ? W16_FRAME : 1;
    int ow = cw + 2 * f, oh = ch + 2 * f + ((style & WS_CAPTION) == WS_CAPTION ? W16_CAPTION : 0) +
                             (hm ? W16_MENU_H : 0);
    int h = w16_create_main(w, k, caption, style, ow, oh, hm, inst, 0, 0, 0);
    if (h <= 0) return 0;
    w->win.dlg = 1;
    w->win.dlgproc = dlgproc;
    return &w->win;
}

// A DIALOG resource (3.x's DLGTEMPLATE) into a dialog, its controls made:
// its units are a quarter of the font's average width and an eighth of
// its height.
static struct w16_wnd *from_template(struct w16 *w, const uint8_t *p, uint32_t n, uint16_t owner,
                                     uint16_t inst, uint32_t dlgproc, w16_dlgproc hostdlg,
                                     char *why, size_t why_cap) {
    struct reader r = { p, n, 0, 0 };
    uint32_t style = rd32(&r);
    unsigned count = rd8(&r);
    int16_t dx = (int16_t)rd16(&r), dy = (int16_t)rd16(&r);
    int16_t dw = (int16_t)rd16(&r), dh = (int16_t)rd16(&r);
    unsigned ord, menu_id, cls_id;
    char *menu = rd_name(&r, &menu_id), *cls = rd_name(&r, &cls_id), *caption = rd_str(&r);
    // A FONT (DS_SETFONT): its size sets the units; whatever the face, w16
    // draws it in its one stand-in for MS Sans Serif.
    int pt = 0;
    if (style & DS_SETFONT) {
        pt = (int)rd16(&r);
        free(rd_str(&r));
    }
    int bx, by;
    w16_dlg_units(&bx, &by);
    if (pt) { bx = (7 * pt + 4) / 8; by = (13 * pt + 4) / 8; }
    struct w16_wnd *d = 0;
    int own = cls && cls[0], ok = !r.bad && menu && cls && caption;
    if (ok && own) {
        // A class of the program's own: a window of that class, which hands
        // DefDlgProc what it does not want (Calculator's SciCalc). The only one
        // w16 makes is the program's main window, owned by nobody.
        int k = w16_class_find(w, cls);
        if (k < 0 || owner || w->win.used) {
            snprintf(why, why_cap, "a dialog of the program's own class is supported only as its main window");
            ok = 0;
        } else {
            d = dlg_main(w, k, style, w16_du_w(dw, bx), w16_du_h(dh, by), menu, menu_id, caption, inst, dlgproc);
            ok = d != 0;
        }
    } else if (ok && (menu[0] || menu_id)) {
        snprintf(why, why_cap, "a dialog with a menu is supported only as the program's main window");
        ok = 0;
    } else if (ok) {
        d = w16_dlg_new(w, owner, inst, style, w16_du_w(dw, bx), w16_du_h(dh, by), caption, dlgproc, hostdlg);
        ok = d != 0;
    }
    if (d && pt) w16_dlg_set_font(d, pt);
    free(menu); free(cls); free(caption);
    for (unsigned i = 0; ok && i < count; i++) {
        int16_t x = (int16_t)rd16(&r), y = (int16_t)rd16(&r), cw = (int16_t)rd16(&r), ch = (int16_t)rd16(&r);
        uint16_t id = (uint16_t)rd16(&r);
        uint32_t cs = rd32(&r);
        unsigned kind = 0;
        char *cname = 0;
        if (r.at < r.n && (r.p[r.at] & 0x80)) kind = rd8(&r);
        else cname = rd_str(&r);
        char *text = rd_name(&r, &ord);
        unsigned extra = rd8(&r);
        r.at += extra;
        if (r.bad || !text) { ok = 0; free(cname); free(text); break; }
        const char *klass = cname ? cname : kind >= 0x80 && kind <= 0x85 ? ORD_CLASS[kind - 0x80] : "?";
        // An icon control names one of the program's icons: draw its own.
        if (!strcmp(klass, "STATIC") && (cs & SS_TYPE) == SS_ICON) cs |= 0xFF00;
        if (w16_class_find(w, klass) < 0) {
            snprintf(why, why_cap, "a dialog control of class %s is not supported yet", klass);
            ok = 0;
        } else if (!add_with_font(w, d, klass, id, cs, owner ? w16_du_x(dx, x, bx) : w16_du_w(x, bx), w16_du_y(dy, y, by),
                                  w16_du_w(cw, bx), w16_du_h(ch, by),
                                  ord ? "" : text)) {
            if (!w->cpu.fault) snprintf(why, why_cap, "a %s control of the dialog was refused", klass);
            ok = 0;
        }
        free(cname);
        free(text);
    }
    if (ok && r.bad) { snprintf(why, why_cap, "its DIALOG resource is cut short"); ok = 0; }
    if (!ok && d) { w16_wnd_destroy(w, d); d = 0; }
    return d;
}

// --- focus and the keyboard --------------------------------------------------------

static int tabstop(struct w16_wnd *c) {
    return c->visible && !(c->style & WS_DISABLED) && (c->style & WS_TABSTOP);
}

static uint32_t dlgcode(struct w16 *w, struct w16_wnd *c, uint16_t vk) {
    return c ? w16_send(w, c->hwnd, WM_GETDLGCODE, vk, 0) : 0;
}

// The control a tab stop gives the focus to: itself -- or, for a radio
// button, the CHECKED one of its group (WS_GROUP to the next), as 3.11's
// dialog manager did (Solitaire's Options opens on "Draw Three").
static struct w16_wnd *tab_target(struct w16 *w, struct w16_wnd *d, struct w16_wnd *c) {
    if (!c || !(dlgcode(w, c, 0) & DLGC_RADIOBUTTON)) return c;
    int n, at = -1;
    struct w16_wnd **v = w16_wnd_children(w, d->hwnd, &n), *hit = c;
    for (int i = 0; v && i < n; i++) if (v[i] == c) at = i;
    int first = at;
    while (first > 0 && !(v[first]->style & WS_GROUP)) first--;
    for (int i = first; v && at >= 0 && i < n; i++) {
        if (i > first && (v[i]->style & WS_GROUP)) break;
        if ((dlgcode(w, v[i], 0) & DLGC_RADIOBUTTON) && v[i]->visible && !(v[i]->style & WS_DISABLED) &&
            (w16_send(w, v[i]->hwnd, WM_USER + 0, 0, 0) & 3)) { hit = v[i]; break; }   // BM_GETCHECK
    }
    free(v);
    return hit;
}

static struct w16_wnd *focused_in(struct w16 *w, struct w16_wnd *d) {
    return w16_wnd_is_child(w, d->hwnd, w->focus) ? w16_wnd_get(w, w->focus) : 0;
}

// The push button Enter presses: the focused one, else the default.
static uint16_t default_id(struct w16_wnd *d) { return d->defid ? d->defid : IDOK; }

// 3.1's rule: a push button with the focus is the default while it has
// it; focus elsewhere gives the dialog's own default back its edge.
static void restyle_default(struct w16 *w, struct w16_wnd *d, struct w16_wnd *to) {
    int n;
    struct w16_wnd **v = w16_wnd_children(w, d->hwnd, &n);
    int to_push = to && (dlgcode(w, to, 0) & (DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON));
    for (int i = 0; v && i < n; i++) {
        uint32_t code = dlgcode(w, v[i], 0);
        if (!(code & (DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON))) continue;
        int def = to_push ? v[i] == to : v[i]->id == default_id(d);
        if (def != !!(code & DLGC_DEFPUSHBUTTON))
            w16_send(w, v[i]->hwnd, BM_SETSTYLE, def ? BS_DEFPUSH : BS_PUSHBUTTON, 1);
    }
    free(v);
}

static void dlg_focus(struct w16 *w, struct w16_wnd *d, struct w16_wnd *c, int by_key) {
    if (!c) return;
    restyle_default(w, d, c);
    w16_set_focus(w, c->hwnd);
    // An edit reached by the keyboard is selected whole, as on 3.1.
    if (by_key && (dlgcode(w, c, 0) & DLGC_HASSETSEL)) w16_send(w, c->hwnd, EM_SETSEL, 0, 0xFFFF0000u);
}

static void tab(struct w16 *w, struct w16_wnd *d, int back) {
    int n, at = -1;
    struct w16_wnd **v = w16_wnd_children(w, d->hwnd, &n);
    if (!v || !n) { free(v); return; }
    for (int i = 0; i < n; i++) if (v[i]->hwnd == w->focus) at = i;
    int start = at >= 0 ? at : back ? 0 : n - 1;
    for (int k = 1; k <= n; k++) {
        int i = ((start + (back ? -k : k)) % n + n) % n;
        if (tabstop(v[i])) { dlg_focus(w, d, tab_target(w, d, v[i]), 1); break; }
    }
    free(v);
}

// The arrows within a group: from the focused control to the next one,
// wrapping at the group's ends; a radio button reached is checked.
static void arrow(struct w16 *w, struct w16_wnd *d, int back) {
    int n, at = -1;
    struct w16_wnd **v = w16_wnd_children(w, d->hwnd, &n);
    for (int i = 0; v && i < n; i++) if (v[i]->hwnd == w->focus) at = i;
    if (at < 0) { free(v); return; }
    int first = at, last = at;
    while (first > 0 && !(v[first]->style & WS_GROUP)) first--;
    while (last + 1 < n && !(v[last + 1]->style & WS_GROUP)) last++;
    int span = last - first + 1;
    for (int k = 1; k < span; k++) {
        int i = first + ((at - first + (back ? -k : k)) % span + span) % span;
        if (!v[i]->visible || (v[i]->style & WS_DISABLED) || (dlgcode(w, v[i], 0) & DLGC_STATIC)) continue;
        dlg_focus(w, d, v[i], 1);
        if (dlgcode(w, v[i], 0) & DLGC_RADIOBUTTON) w16_send(w, v[i]->hwnd, WM_CHAR, ' ', 0);
        break;
    }
    free(v);
}

// A command for the dialog: what a press of control `id` would say.
static void command(struct w16 *w, struct w16_wnd *d, uint16_t id) {
    struct w16_wnd *c = w16_dlg_item(w, d->hwnd, id);
    if (c && (c->style & WS_DISABLED)) return;
    w16_send(w, d->hwnd, WM_COMMAND, id, (uint32_t)BN_CLICKED << 16 | (c ? c->hwnd : 0));
}

// The control a mnemonic letter names: a button by its own &-letter, or
// the control after a label carrying it.
static struct w16_wnd *mnemonic(struct w16 *w, struct w16_wnd *d, int ch) {
    int n;
    struct w16_wnd **v = w16_wnd_children(w, d->hwnd, &n), *hit = 0;
    ch |= 0x20;
    for (int i = 0; v && i < n && !hit; i++) {
        if (!v[i]->visible || (v[i]->style & WS_DISABLED)) continue;
        uint32_t code = dlgcode(w, v[i], 0);
        if (!(code & (DLGC_STATIC | DLGC_BUTTON))) continue;   // an edit's text is not a label
        const char *t = v[i]->text, *amp = t ? strchr(t, '&') : 0;
        if (!amp || (amp[1] | 0x20) != ch || ((code & DLGC_STATIC) && (v[i]->style & SS_NOPREFIX))) continue;
        if (!(code & DLGC_STATIC)) hit = v[i];
        else for (int j = i + 1; j < n && !hit; j++) if (tabstop(v[j])) hit = v[j];
    }
    free(v);
    return hit;
}

int w16_is_dialog_message(struct w16 *w, struct w16_wnd *d, const struct w16_msg *m) {
    if (!d || (m->hwnd != d->hwnd && !w16_wnd_is_child(w, d->hwnd, m->hwnd))) return 0;
    struct w16_wnd *f = focused_in(w, d);
    unsigned mods = m->mods;
    if (m->message == WM_KEYDOWN || m->message == WM_CHAR) w->key_mods = mods;
    if (m->message == WM_KEYDOWN) {
        uint32_t code = dlgcode(w, f, m->wparam);
        if (!(code & DLGC_WANTALLKEYS)) {
            switch (m->wparam) {
            case VK_TAB:
                if (code & DLGC_WANTTAB) break;
                tab(w, d, (mods & KEY_MOD_SHIFT) != 0);
                return 1;
            case VK_RETURN:
                if (f && (code & (DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON))) command(w, d, f->id);
                else command(w, d, default_id(d));
                return 1;
            case VK_ESCAPE:
                command(w, d, IDCANCEL);
                return 1;
            case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
                if (code & DLGC_WANTARROWS) break;
                arrow(w, d, m->wparam == VK_LEFT || m->wparam == VK_UP);
                return 1;
            default:
                break;
            }
        }
    } else if (m->message == WM_CHAR) {
        uint32_t code = dlgcode(w, f, m->wparam);
        if (!(code & DLGC_WANTALLKEYS) && (m->wparam == '\r' || m->wparam == '\t' || m->wparam == 0x1B))
            return 1;                            // their WM_KEYDOWN did the work
        if (m->wparam > ' ' && m->wparam < 0x7F && ((mods & KEY_MOD_ALT) || !(code & DLGC_WANTCHARS))) {
            struct w16_wnd *c = mnemonic(w, d, m->wparam);
            if (c) {
                uint32_t cc = dlgcode(w, c, 0);
                dlg_focus(w, d, c, 1);
                if (cc & (DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON)) command(w, d, c->id);
                else if (cc & DLGC_BUTTON) w16_send(w, c->hwnd, WM_CHAR, ' ', 0);
                return 1;
            }
            if (!(code & DLGC_WANTCHARS)) return 1;
        }
    }
    w16_dispatch(w, m);
    return 1;
}

// --- DefDlgProc ------------------------------------------------------------------

static uint32_t call_dlgproc(struct w16 *w, struct w16_wnd *d, uint16_t msg, uint16_t wp, uint32_t lp) {
    if (d->hostdlg) return (uint32_t)d->hostdlg(w, d, msg, wp, lp);
    if (!d->dlgproc) return 0;
    return w16_call_wndproc(w, (uint16_t)(d->dlgproc >> 16), (uint16_t)d->dlgproc, d->hwnd, msg, wp, lp) & 0xFFFF;
}

uint32_t w16_dialog_proc(struct w16 *w, struct w16_wnd *d, uint16_t msg, uint16_t wp, uint32_t lp) {
    uint16_t h = d->hwnd;
    uint32_t r = call_dlgproc(w, d, msg, wp, lp);
    d = w16_wnd_get(w, h);
    if (!d) return r;
    if (r && msg != WM_ACTIVATE && msg != WM_SETFOCUS) return r;
    switch (msg) {
    case WM_ERASEBKGND:
        if (d->kind != W16_K_DIALOG) break;      // a class of the program's: its own brush
        w16_gdi_fill(w, wp, 0, 0, d->cw, d->ch, COLOR_WINDOW + 1);
        return 1;
    case WM_CLOSE:
        command(w, d, IDCANCEL);
        return 0;
    case WM_ACTIVATE:
    case WM_SETFOCUS: {
        // Active again: the control that last had the focus gets it back.
        if (msg == WM_ACTIVATE && !wp) return 0;
        struct w16_wnd *c = w16_wnd_get(w, d->focus);
        if (!c || !w16_wnd_is_child(w, h, c->hwnd)) {
            int n;
            struct w16_wnd **v = w16_wnd_children(w, h, &n);
            c = 0;
            for (int i = 0; v && i < n && !c; i++) if (tabstop(v[i])) c = v[i];
            free(v);
            c = tab_target(w, d, c);
        }
        if (c && w->focus != c->hwnd) dlg_focus(w, d, c, 0);
        return 0;
    }
    case DM_GETDEFID:
        return (uint32_t)DC_HASDEFID << 16 | default_id(d);
    case DM_SETDEFID:
        d->defid = wp;
        restyle_default(w, d, focused_in(w, d));
        return 1;
    default:
        break;
    }
    return w16_def_window_proc(w, d, msg, wp, lp);
}

// --- showing and running ----------------------------------------------------------------

// Shown, and the focus put where WM_INITDIALOG left it -- or on the
// first tab stop when it answered TRUE.
static void start(struct w16 *w, struct w16_wnd *d, uint32_t param, int modal) {
    uint16_t h = d->hwnd;
    int n;
    struct w16_wnd **v = w16_wnd_children(w, h, &n), *first = 0;
    for (int i = 0; v && i < n; i++) {
        if (!first && tabstop(v[i])) first = v[i];
        // The dialog's default is the push button its template says.
        if (!d->defid && (v[i]->style & BS_TYPE) == BS_DEFPUSH && (dlgcode(w, v[i], 0) & DLGC_DEFPUSHBUTTON))
            d->defid = v[i]->id;
    }
    free(v);
    d->modal = modal;
    if (d->kind == W16_K_DIALOG) {               // a main window shows as CreateWindow's do: by its style or ShowWindow
        pthread_mutex_lock(&w->lock);
        d->visible = 1;
        d->style |= WS_VISIBLE;
        pthread_mutex_unlock(&w->lock);
        w16_task_post_arg(w, W16_POST_DLG_OPEN, h);
    }
    uint32_t r = w16_send(w, h, WM_INITDIALOG, first ? first->hwnd : 0, param);
    d = w16_wnd_get(w, h);
    if (d && (r & 0xFFFF) && first && w16_wnd_get(w, first->hwnd)) dlg_focus(w, d, tab_target(w, d, first), 1);
    if (d) w16_invalidate(w, d, 1);
}

void w16_dlg_show(struct w16 *w, struct w16_wnd *d, int modal, uint32_t param) { start(w, d, param, modal); }

// Its toywm window closed, and gone before the dialog may be freed.
void w16_dlg_window_gone(struct w16 *w, struct w16_wnd *d) {
    if (d->ended == 2) return;
    w16_task_post_arg(w, W16_POST_DLG_CLOSE, d->hwnd);
    pthread_mutex_lock(&w->lock);
    while (d->ended != 2 && w->app) pthread_cond_wait(&w->cond, &w->lock);
    d->ended = 2;
    pthread_mutex_unlock(&w->lock);
}

int16_t w16_dlg_run(struct w16 *w, struct w16_wnd *d) {
    uint16_t h = d->hwnd, owner = d->parent;
    while (!w->cpu.fault && (d = w16_wnd_get(w, h)) && !d->ended) {
        struct w16_msg m;
        if (!w16_get_msg(w, &m, -1)) break;
        if (m.message == WM_QUIT) {              // the program is ending: hand it back
            w16_post_msg(w, 0, WM_QUIT, m.wparam, 0);
            break;
        }
        d = w16_wnd_get(w, h);
        if (!d || !w16_is_dialog_message(w, d, &m)) w16_dispatch(w, &m);
    }
    int16_t r = d ? d->result : -1;
    if (d) w16_wnd_destroy(w, d);
    // The owner is active again: it takes the focus back, as on Windows.
    if (owner && w16_wnd_get(w, owner) && !w->cpu.fault) w16_send(w, owner, WM_ACTIVATE, 1, 0);
    return r;
}

struct w16_wnd *w16_dlg_create(struct w16 *w, const uint8_t *tmpl, uint32_t n, uint16_t owner, uint16_t inst,
                               uint32_t dlgproc, w16_dlgproc hostdlg, uint32_t param, char *why,
                               size_t why_cap) {
    struct w16_wnd *d = from_template(w, tmpl, n, owner, inst, dlgproc, hostdlg, why, why_cap);
    if (d) start(w, d, param, 0);
    return d;
}

struct w16_wnd *w16_dlg_from_template(struct w16 *w, const uint8_t *tmpl, uint32_t n, uint16_t owner,
                                      uint16_t inst, w16_dlgproc hostdlg, char *why, size_t why_cap) {
    return from_template(w, tmpl, n, owner, inst ? inst : w->dgroup, 0, hostdlg, why, why_cap);
}

// --- the exports ------------------------------------------------------------------

// The DIALOG resource a template argument names; -1 when none.
static int find_template(struct w16 *w, uint32_t tmpl, struct ne_resource *res) {
    char name[64] = "";
    if (tmpl >> 16 && get_str(w, tmpl, name, sizeof name) < 0) return -2;
    return w16_res_find(w, NE_RT_DIALOG, 0, (uint16_t)(tmpl >> 16 ? 0 : tmpl), tmpl >> 16 ? name : 0, res);
}

static int run_template(struct w16 *w, const uint8_t *p, uint32_t n, uint16_t inst, uint16_t owner, uint32_t proc,
                        uint32_t param, int modal, int pop);

// DialogBox(hInst, lpTemplate, hWndParent, lpDialogFunc), CreateDialog
// the same, modeless; their Param forms add an lParam for WM_INITDIALOG.
static int dialog(struct w16 *w, struct w16_args *a, int with_param, int modal) {
    struct cpu16 *c = &w->cpu;
    uint16_t inst = w16_arg16(a);
    uint32_t tmpl = w16_arg32(a);
    uint16_t owner = w16_arg16(a);
    uint32_t proc = w16_arg32(a);
    uint32_t param = with_param ? w16_arg32(a) : 0;
    int pop = with_param ? 16 : 12;
    struct ne_resource res;
    int found = find_template(w, tmpl, &res);
    if (found == -2) return -1;
    c->r[R_AX] = modal ? 0xFFFF : 0;
    if (found < 0) return pop;
    return run_template(w, w->ne.img + res.off, res.len, inst, owner, proc, param, modal, pop);
}

// A DIALOG template's dialog made, then run when modal: the dialog's
// handle, or the modal result, in AX.
static int run_template(struct w16 *w, const uint8_t *p, uint32_t n, uint16_t inst, uint16_t owner, uint32_t proc,
                        uint32_t param, int modal, int pop) {
    struct cpu16 *c = &w->cpu;
    char why[96] = "";
    c->r[R_AX] = modal ? 0xFFFF : 0;
    struct w16_wnd *d = from_template(w, p, n, owner, inst ? inst : w->dgroup, proc, 0, why, sizeof why);
    if (!d) {
        if (!c->fault) cpu16_set_fault(c, FAULT_API, why[0] ? why : "out of memory for a dialog");
        return -1;
    }
    uint16_t h = d->hwnd;
    start(w, d, param, modal);
    if (!modal) { c->r[R_AX] = w16_wnd_get(w, h) ? h : 0; return pop; }
    d = w16_wnd_get(w, h);
    c->r[R_AX] = d ? (uint16_t)w16_dlg_run(w, d) : 0xFFFF;
    return c->fault ? -1 : pop;
}

// DialogBox(hInstance, lpTemplateName, hWndParent, lpDialogFunc) -> int
// Windows: Makes a modal dialog from a DIALOG resource -- its controls, their places in dialog
// units -- and runs it until the dialog procedure calls EndDialog; returns EndDialog's value. The
// owner takes no input meanwhile.
// w16: The same: the dialog is a window of its own over the program's, drawn as Windows 3.1 drew
// dialogs, with 3.1's keyboard -- Tab between controls, Enter the default button, Esc Cancel, a
// control's underlined letter. Its controls are w16's own BUTTON, STATIC, EDIT, LISTBOX and
// COMBOBOX.
static int api_DialogBox(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return dialog(w, a, 0, 1);
}

// DialogBoxParam(hInstance, lpTemplateName, hWndParent, lpDialogFunc, lParamInit) -> int
// Windows: DialogBox, with a value the dialog procedure gets in WM_INITDIALOG's lParam.
// w16: The same (DialogBox says how).
static int api_DialogBoxParam(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return dialog(w, a, 1, 1);
}

// CreateDialog(hInstance, lpTemplateName, hWndParent, lpDialogFunc) -> HWND
// Windows: Makes a modeless dialog from a DIALOG resource: it stays open beside the program's
// window, its keys passed through IsDialogMessage, until DestroyWindow.
// w16: The same, a window of its own beside the program's.
static int api_CreateDialog(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return dialog(w, a, 0, 0);
}

// CreateDialogParam(hInstance, lpTemplateName, hWndParent, lpDialogFunc, lParamInit) -> HWND
// Windows: CreateDialog, with a value for WM_INITDIALOG's lParam.
// w16: The same.
static int api_CreateDialogParam(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return dialog(w, a, 1, 0);
}

// DialogBoxIndirect(hInst, hDialogTemplate, hWndParent, lpDialogFunc): a
// template the program built in a global block; CreateDialogIndirect(hInst,
// lpDialogTemplate, hWndParent, lpDialogFunc) one at a far pointer. The
// template is read up to its segment's end; from_template() stops at its own.
static int indirect(struct w16 *w, struct w16_args *a, int modal) {
    uint16_t inst = w16_arg16(a);
    uint32_t tmpl = modal ? (uint32_t)w16_arg16(a) << 16 : w16_arg32(a);
    uint16_t owner = w16_arg16(a);
    uint32_t proc = w16_arg32(a);
    int pop = modal ? 10 : 12;
    uint16_t sel = (uint16_t)(tmpl >> 16), off = (uint16_t)tmpl;
    const struct cpu16_desc *d = &w->desc[cpu16_index(sel) % CPU16_MAX_DESC];
    uint32_t n = sel && (d->kind == D_DATA || d->kind == D_CODE) && off <= d->limit ? d->limit + 1u - off : 0;
    const uint8_t *p = n ? cpu16_ptr(&w->cpu, sel, off, n, 0) : 0;
    if (!p) {
        w->cpu.r[R_AX] = modal ? 0xFFFF : 0;
        return w->cpu.fault ? -1 : pop;
    }
    return run_template(w, p, n, inst, owner, proc, 0, modal, pop);
}

// A template the program built in a global block; read up to its segment's end, from_template()
// stopping at its own.
// DialogBoxIndirect(hInstance, hDialogTemplate, hWndParent, lpDialogFunc) -> int
// Windows: DialogBox from a template the program built in memory -- a global block -- rather than a
// resource.
// w16: The same; the template is read with every field checked to lie inside its block.
static int api_DialogBoxIndirect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return indirect(w, a, 1);
}

// CreateDialogIndirect(hInstance, lpDialogTemplate, hWndParent, lpDialogFunc) -> HWND
// Windows: CreateDialog from a template in memory, at a far pointer.
// w16: The same (DialogBoxIndirect says how it is read).
static int api_CreateDialogIndirect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return indirect(w, a, 0);
}

// GetNextDlgTabItem(hDlg, hCtl, bPrevious) -> HWND
// Windows: The control Tab (or Shift+Tab) would move to from hCtl: the next visible, enabled one
// with WS_TABSTOP, wrapping; from NULL, the first (or last).
// w16: The same walk the dialog's own Tab takes, without moving the focus.
static int api_GetNextDlgTabItem(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_wnd *d = w16_wnd_get(w, w16_arg16(a));
    uint16_t from = w16_arg16(a);
    int back = w16_arg16(a) != 0;
    uint16_t r = 0;
    int n, at = -1;
    struct w16_wnd **v = d ? w16_wnd_children(w, d->hwnd, &n) : 0;
    for (int i = 0; v && i < n; i++) if (v[i]->hwnd == from) at = i;
    int start = at >= 0 ? at : back ? 0 : n - 1;
    for (int k = 1; v && k <= n && !r; k++) {
        int i = ((start + (back ? -k : k)) % n + n) % n;
        if (tabstop(v[i])) r = v[i]->hwnd;
    }
    free(v);
    w->cpu.r[R_AX] = r;
    return 6;
}

// IsDialogMessage(hDlg, lpMsg) -> BOOL
// Windows: Gives a modeless dialog's keys their dialog meaning -- Tab, the arrows, Enter, Esc, a
// mnemonic -- when the message is for it; TRUE when it took the message.
// w16: The same, as a modal dialog's own loop does it.
static int api_IsDialogMessage(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_wnd *d = w16_wnd_get(w, w16_arg16(a));
    uint32_t far = w16_arg32(a);
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, 10, 0);
    if (!p) return -1;
    struct w16_msg m = { (uint16_t)(p[0] | p[1] << 8), (uint16_t)(p[2] | p[3] << 8), (uint16_t)(p[4] | p[5] << 8),
                         (uint32_t)(p[6] | p[7] << 8 | p[8] << 16 | (uint32_t)p[9] << 24), (uint8_t)w->key_mods };
    w->cpu.r[R_AX] = d && w16_is_dialog(d) ? (uint16_t)w16_is_dialog_message(w, d, &m) : 0;
    return w->cpu.fault ? -1 : 6;
}

// EndDialog(hDlg, nResult) -> void
// Windows: Ends a modal dialog once its procedure returns: DialogBox returns nResult.
// w16: The same; a modeless dialog, which has no loop to end, is destroyed.
static int api_EndDialog(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_wnd *d = w16_wnd_get(w, w16_arg16(a));
    int16_t r = (int16_t)w16_arg16(a);
    if (d && d->kind == W16_K_DIALOG) {
        d->result = r;
        if (!d->ended) d->ended = 1;
        if (!d->modal) w16_wnd_destroy(w, d);   // a modeless one has no loop to end
    }
    w->cpu.r[R_AX] = d != 0;
    return 4;
}

struct w16_wnd *w16_dlg_item(struct w16 *w, uint16_t dlg, uint16_t id) {
    for (int i = 0; i < w->nwnds; i++) {
        struct w16_wnd *x = w->wnds[i];
        if (x && x->used && !x->destroyed && x->kind == W16_K_CHILD && x->parent == dlg && x->id == id) return x;
    }
    return 0;
}

// GetDlgItem(hDlg, nIDDlgItem) -> HWND
// Windows: The window of a dialog's control by its id.
// w16: The same; a main window's child controls are found the same way.
static int api_GetDlgItem(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t dlg = w16_arg16(a), id = w16_arg16(a);
    struct w16_wnd *c = w16_dlg_item(w, dlg, id);
    w->cpu.r[R_AX] = c ? c->hwnd : 0;
    return 4;
}

// SetDlgItemText(hDlg, nIDDlgItem, lpString) -> void
// Windows: Sets a control's text: WM_SETTEXT to it.
// w16: The same.
static int api_SetDlgItemText(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t dlg = w16_arg16(a), id = w16_arg16(a);
    uint32_t far = w16_arg32(a);
    struct w16_wnd *c = w16_dlg_item(w, dlg, id);
    if (c) w16_send(w, c->hwnd, WM_SETTEXT, 0, far);
    return w->cpu.fault ? -1 : 8;
}

// GetDlgItemText(hDlg, nIDDlgItem, lpString, nMaxCount) -> int
// Windows: Copies a control's text into a buffer: WM_GETTEXT; the length copied.
// w16: The same; an empty string for an id that is no control.
static int api_GetDlgItemText(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t dlg = w16_arg16(a), id = w16_arg16(a);
    uint32_t far = w16_arg32(a);
    uint16_t max = w16_arg16(a);
    struct w16_wnd *c = w16_dlg_item(w, dlg, id);
    w->cpu.r[R_AX] = 0;
    if (c) w->cpu.r[R_AX] = (uint16_t)w16_send(w, c->hwnd, WM_GETTEXT, max, far);
    else if (max) {
        uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, 1, 1);
        if (!p) return -1;
        p[0] = 0;
    }
    return w->cpu.fault ? -1 : 10;
}

// SetDlgItemInt(hDlg, nIDDlgItem, uValue, bSigned) -> void
// Windows: Sets a control's text to a number, signed or not.
// w16: The same.
static int api_SetDlgItemInt(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t dlg = w16_arg16(a), id = w16_arg16(a), v = w16_arg16(a), sig = w16_arg16(a);
    struct w16_wnd *c = w16_dlg_item(w, dlg, id);
    if (c) {
        char s[16];
        if (sig) snprintf(s, sizeof s, "%d", (int16_t)v);
        else snprintf(s, sizeof s, "%u", v);
        set_text(w, c, s);
    }
    return w->cpu.fault ? -1 : 8;
}

// GetDlgItemInt(hDlg, nIDDlgItem, lpTranslated, bSigned) -> UINT
// Windows: Reads a control's text as a number, and says whether it was one: leading spaces are
// skipped and, signed, a minus taken; anything else after the digits, or too big a number, is not a
// number.
// w16: The same rules as 3.1.
static int api_GetDlgItemInt(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t dlg = w16_arg16(a), id = w16_arg16(a);
    uint32_t ok_far = w16_arg32(a);
    uint16_t sig = w16_arg16(a);
    struct w16_wnd *c = w16_dlg_item(w, dlg, id);
    char *text = c ? text_of(w, c) : strdup("");
    if (!text) return -1;
    long v = 0;
    int ok = 0;
    const char *t = text;
    while (*t == ' ') t++;
    int neg = sig && *t == '-';
    if (neg) t++;
    for (; *t >= '0' && *t <= '9' && v <= 0x10000; t++, ok = 1) v = v * 10 + (*t - '0');
    while (*t == ' ') t++;
    if (*t || v > (sig ? 32767 + neg : 65535)) ok = 0;
    free(text);
    if (!ok) v = 0;
    if (ok_far >> 16) {
        uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(ok_far >> 16), (uint16_t)ok_far, 2, 1);
        if (!p) return -1;
        p[0] = (uint8_t)ok;
        p[1] = 0;
    }
    w->cpu.r[R_AX] = (uint16_t)(neg ? -v : v);
    return 10;
}

// SendDlgItemMessage(hDlg, nIDDlgItem, uMsg, wParam, lParam) -> LRESULT
// Windows: SendMessage to a dialog's control by its id.
// w16: The same; 0 for an id that is no control.
static int api_SendDlgItemMessage(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t dlg = w16_arg16(a), id = w16_arg16(a), msg = w16_arg16(a), wp = w16_arg16(a);
    uint32_t lp = w16_arg32(a);
    struct w16_wnd *c = w16_dlg_item(w, dlg, id);
    uint32_t r = c ? w16_send(w, c->hwnd, msg, wp, lp) : 0;
    w->cpu.r[R_AX] = (uint16_t)r;
    w->cpu.r[R_DX] = (uint16_t)(r >> 16);
    return w->cpu.fault ? -1 : 12;
}

// CheckDlgButton(hDlg, nIDButton, uCheck) -> void
// Windows: Checks, unchecks or (for a three-state box) greys a check box or radio button:
// BM_SETCHECK.
// w16: The same.
static int api_CheckDlgButton(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t dlg = w16_arg16(a), id = w16_arg16(a), v = w16_arg16(a);
    struct w16_wnd *c = w16_dlg_item(w, dlg, id);
    if (c) w16_send(w, c->hwnd, BM_SETCHECK, v, 0);
    return 6;
}

// IsDlgButtonChecked(hDlg, nIDButton) -> UINT
// Windows: Whether a check box or radio button is checked: BM_GETCHECK.
// w16: The same; 0 for an id that is no control.
static int api_IsDlgButtonChecked(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t dlg = w16_arg16(a), id = w16_arg16(a);
    struct w16_wnd *c = w16_dlg_item(w, dlg, id);
    w->cpu.r[R_AX] = c ? (uint16_t)w16_send(w, c->hwnd, BM_GETCHECK, 0, 0) : 0;
    return 4;
}

// CheckRadioButton(hDlg, nIDFirstButton, nIDLastButton, nIDCheckButton) -> void
// Windows: Checks one radio button of a run of ids and unchecks the others.
// w16: The same.
static int api_CheckRadioButton(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t dlg = w16_arg16(a), first = w16_arg16(a), last = w16_arg16(a), check = w16_arg16(a);
    for (unsigned id = first; id <= last && id <= 0xFFFF; id++) {
        struct w16_wnd *c = w16_dlg_item(w, dlg, (uint16_t)id);
        if (c) w16_send(w, c->hwnd, BM_SETCHECK, id == check, 0);
    }
    return 8;
}

// DefDlgProc(hDlg, uMsg, wParam, lParam) -> LRESULT
// Windows: The default handling for a dialog of a program's own window class -- what a dialog does
// with a message its class procedure does not want.
// w16: The same handling w16's own dialogs get -- the program's DLGPROC first, when the dialog has
// one -- for a main window made from a DIALOG template; any other window gets DefWindowProc's.
static int api_DefDlgProc(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_wnd *d = w16_wnd_get(w, w16_arg16(a));
    uint16_t msg = w16_arg16(a), wp = w16_arg16(a);
    uint32_t lp = w16_arg32(a);
    uint32_t r = !d ? 0 : w16_is_dialog(d) ? w16_dialog_proc(w, d, msg, wp, lp) : w16_def_window_proc(w, d, msg, wp, lp);
    w->cpu.r[R_AX] = (uint16_t)r;
    w->cpu.r[R_DX] = (uint16_t)(r >> 16);
    return 10;
}

// MapDialogRect(hDlg, lpRect) -> void
// Windows: Turns a rectangle in dialog units -- a quarter of the dialog font's average width, an
// eighth of its height -- into pixels.
// w16: The same, from the font of the dialog given (the System font when it names none).
static int api_MapDialogRect(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t hdlg = w16_arg16(a);
    uint32_t far = w16_arg32(a);
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, 8, 1);
    if (!p) return -1;
    int16_t r[4];
    int bx, by;
    w16_units_of(w, w16_wnd_get(w, hdlg), &bx, &by);
    memcpy(r, p, 8);
    r[0] = (int16_t)(r[0] * bx / 4); r[2] = (int16_t)(r[2] * bx / 4);
    r[1] = (int16_t)(r[1] * by / 8); r[3] = (int16_t)(r[3] * by / 8);
    memcpy(p, r, 8);
    return 6;
}

// --- dialogs built in code: MessageBox, ShellAbout -------------------------------

// A box built here ends with the button pressed; Esc (IDCANCEL) ends it
// with Cancel when it has one, else with its button when it has only one
// (an OK box), else not at all -- Yes/No has no answer to guess.
static int box_proc(struct w16 *w, struct w16_wnd *d, uint16_t msg, uint16_t wp, uint32_t lp) {
    (void)lp;
    if (msg == WM_INITDIALOG) return 1;
    if (msg != WM_COMMAND) return 0;
    uint16_t id = wp;
    if (id == IDCANCEL && !w16_dlg_item(w, d->hwnd, IDCANCEL)) {
        int n, buttons = 0;
        struct w16_wnd **v = w16_wnd_children(w, d->hwnd, &n), *only = 0;
        for (int i = 0; v && i < n; i++)
            if (dlgcode(w, v[i], 0) & (DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON)) { buttons++; only = v[i]; }
        free(v);
        if (buttons != 1) return 1;
        id = only->id;
    }
    d->result = (int16_t)id;
    d->ended = 1;
    return 1;
}

// A dialog sized round its contents: `lines` of text beside an icon
// (SS_ICON's id: the MB_ICON* or -1 the program's own), then `n` buttons.
static struct w16_wnd *built(struct w16 *w, uint16_t owner, const char *caption, const char *text,
                             int icon, const char *const *labels, const uint16_t *ids, int n,
                             int def) {
    int bx, by;
    w16_dlg_units(&bx, &by);
    int ch = by, pad = bx * 2, iw = icon ? 32 + pad : 0;
    // The text's width and lines, wrapped no wider than 3.1 let it be.
    int maxw = bx * 60, tw = 0, lines = 0;
    pthread_mutex_lock(&g_w16_text_lock);
    const struct ugfx_font *was = ugfx_set_font(w16_system_font());
    for (const char *p = text; ; ) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        int lw = ugfx_text_width_n(p, len);
        lines += lw > maxw ? (lw + maxw - 1) / maxw : 1;
        if (lw > tw) tw = lw > maxw ? maxw : lw;
        if (!nl) break;
        p = nl + 1;
    }
    int bw = bx * 10;
    for (int i = 0; i < n; i++) {
        int lw = ugfx_text_width(labels[i]) + bx * 4;
        if (lw > bw) bw = lw;
    }
    ugfx_set_font(was);
    pthread_mutex_unlock(&g_w16_text_lock);
    int bh = ch + by / 2 + 6, row = n * bw + (n - 1) * pad;
    int textw = tw + 4, texth = lines * ch;
    int cw = iw + textw + 2 * pad;
    if (row + 2 * pad > cw) cw = row + 2 * pad;
    int top = icon && 32 > texth ? 32 : texth;
    int chh = pad + top + pad + bh + pad;
    struct w16_wnd *d = w16_dlg_new(w, owner, 0, WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME, cw, chh, caption, 0,
                                    box_proc);
    if (!d) return 0;
    int ok = 1;
    if (icon)
        ok &= w16_dlg_add(w, d, "STATIC", 0xFFFF, WS_VISIBLE | SS_ICON | (uint32_t)(icon & 0xFF) << 8, pad, pad,
                          32, 32, "") != 0;
    ok &= w16_dlg_add(w, d, "STATIC", 0xFFFF, WS_VISIBLE | SS_NOPREFIX, pad + iw, pad + (top - texth) / 2,
                      textw, texth, text) != 0;
    int x = (cw - row) / 2;
    for (int i = 0; ok && i < n; i++, x += bw + pad)
        ok &= w16_dlg_add(w, d, "BUTTON", ids[i], WS_VISIBLE | WS_TABSTOP | (i == def ? BS_DEFPUSH : 0),
                          x, pad + top + pad, bw, bh, labels[i]) != 0;
    if (!ok) { w16_wnd_destroy(w, d); return 0; }
    d->defid = ids[def];
    return d;
}

int16_t w16_message_box(struct w16 *w, uint16_t owner, const char *text, const char *caption, unsigned type) {
    w16_sound_event(w, w16_beep_alias(type));   // 3.1's MessageBox beeps with its icon's sound
    static const char *const L[] = { "OK", "Cancel", "&Abort", "&Retry", "&Ignore", "&Yes", "&No" };
    static const uint8_t SETS[6][4] = {
        { 1 }, { 1, 2 }, { 3, 4, 5 }, { 6, 7, 2 }, { 6, 7 }, { 4, 2 },
    };
    unsigned set = type & 0x0F;
    if (set > 5) set = 0;
    const char *labels[3];
    uint16_t ids[3];
    int n = 0;
    for (; n < 3 && SETS[set][n]; n++) { ids[n] = SETS[set][n]; labels[n] = L[ids[n] - 1]; }
    int def = (type >> 8) & 0x0F;                // MB_DEFBUTTON2, 3
    if (def >= n) def = 0;
    int icon = (type >> 4) & 0x0F;               // MB_ICONHAND 1, QUESTION 2, EXCLAMATION 3, ASTERISK 4
    struct w16_wnd *d = built(w, owner, caption && *caption ? caption : "Error", text, icon, labels, ids, n, def);
    if (!d) return 0;
    start(w, d, 0, 1);
    return w16_dlg_run(w, d);
}

// MessageBox(hWnd, lpText, lpCaption, uType) -> int
// Windows: A modal box with a message, an icon (stop, question, exclamation, information) and
// buttons -- OK, OK and Cancel, Yes and No, Abort, Retry and Ignore... -- and the event sound for
// its icon; returns the button chosen.
// w16: The same, sized round its text and drawn as Windows 3.1 drew it, with its buttons, mnemonics
// and sound (MessageBeep's).
static int api_MessageBox(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t owner = w16_arg16(a);
    uint32_t tf = w16_arg32(a), cf = w16_arg32(a);
    uint16_t type = w16_arg16(a);
    char *text = dup_far(w, tf), *cap = dup_far(w, cf);
    if (!text || !cap) { free(text); free(cap); return -1; }
    w->cpu.r[R_AX] = (uint16_t)w16_message_box(w, owner, text, cap, type);
    free(text);
    free(cap);
    return w->cpu.fault ? -1 : 12;
}

// "Name#title": the part after '#' is the first line.
// ShellAbout(hWnd, szApp, szOtherStuff, hIcon) -> int
// Windows: Shows the About box Windows' own programs use: the program's icon and name, a line of
// its own, and Windows' version, copyright and free memory.
// w16: The same layout, with the program's icon and its two strings, and what runs it -- w16 on
// toy-os -- where 3.1 named itself.
static int api_ShellAbout(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t owner = w16_arg16(a);
    uint32_t af = w16_arg32(a), of = w16_arg32(a);
    w16_arg16(a);
    char *app = dup_far(w, af), *other = dup_far(w, of);
    if (!app || !other) { free(app); free(other); return -1; }
    char *hash = strchr(app, '#');
    if (hash) *hash++ = 0;
    size_t n = strlen(app) + strlen(other) + strlen(w->path) + 160;
    char *text = malloc(n), *cap = malloc(strlen(app) + 8);
    if (text && cap) {
        snprintf(cap, strlen(app) + 8, "About %s", app);
        snprintf(text, n, "%s\n%s\n\nA Windows 3.x program, running in w16\non toy-os %s\n\n%s",
                 hash ? hash : app, other, TOYOS_VERSION, w->path);
        static const char *const ok[] = { "OK" };
        static const uint16_t ids[] = { IDOK };
        // The program's own icon beside it, when it has one.
        static uint32_t px[32 * 32];
        int iw, ih, icon = w16_icon(&w->ne, px, &iw, &ih) == 0 ? 0xFF : 0;
        struct w16_wnd *d = built(w, owner, cap, text, icon, ok, ids, 1, 0);
        if (d) {
            start(w, d, 0, 1);
            w16_dlg_run(w, d);
        }
    }
    free(text); free(cap); free(app); free(other);
    w->cpu.r[R_AX] = 1;
    return w->cpu.fault ? -1 : 12;
}

// The sound driver: voices that play nothing, yet -- a program that asks
// for one is answered as if the queue played (Minesweeper's tick).
const struct w16_api g_dialog_api[] = {
    { 1, api_MessageBox },
    { 87, api_DialogBox },
    { 88, api_EndDialog },
    { 89, api_CreateDialog },
    { 90, api_IsDialogMessage },
    { 91, api_GetDlgItem },
    { 92, api_SetDlgItemText },
    { 93, api_GetDlgItemText },
    { 94, api_SetDlgItemInt },
    { 95, api_GetDlgItemInt },
    { 96, api_CheckRadioButton },
    { 97, api_CheckDlgButton },
    { 98, api_IsDlgButtonChecked },
    { 101, api_SendDlgItemMessage },
    { 103, api_MapDialogRect },
    { 218, api_DialogBoxIndirect },
    { 219, api_CreateDialogIndirect },
    { 228, api_GetNextDlgTabItem },
    { 239, api_DialogBoxParam },
    { 241, api_CreateDialogParam },
    { 308, api_DefDlgProc },
};
const unsigned g_dialog_api_n = sizeof g_dialog_api / sizeof g_dialog_api[0];

// Nothing is ever dropped on a program yet; DragQueryFile and DragFinish answer for a drop that
// never comes.
// DragAcceptFiles(hWnd, fAccept) -> void
// Windows: Tells File Manager that a window takes files dropped on it; a drop then arrives as
// WM_DROPFILES.
// w16: Accepts the call and does nothing: no drop is ever delivered.
// Why: Dropping a file from toy-os's File Manager onto a Windows program's window needs toywm to
// carry the drop to w16, and w16 to turn it into WM_DROPFILES with DOS paths.
// Notice: A file dropped on its window is not opened. Open... in its File menu works.
// Whole: Drops delivered to Windows programs. On the roadmap: "w16: a file dropped on a program
// (WM_DROPFILES)".
static int api_DragAcceptFiles(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);
    w16_arg16(a);
    (void)w;
    return 4;
}

// DragQueryFile(hDrop, iFile, lpszFile, cb) -> UINT
// Windows: Says how many files a drop held, and each one's path.
// w16: Answers 0: there is never a drop to ask about.
// Why: No drop is ever delivered (DragAcceptFiles says why).
// Notice: Nothing: it is only asked after a drop.
// Whole: As DragAcceptFiles: on the roadmap, "w16: a file dropped on a program (WM_DROPFILES)".
static int api_DragQueryFile(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a); w16_arg16(a); w16_arg32(a); w16_arg16(a);
    w->cpu.r[R_AX] = 0;
    return 10;
}

// DragFinish(hDrop) -> void
// Windows: Frees what a drop held.
// w16: Nothing, as there is nothing to free.
// Why: No drop is ever delivered (DragAcceptFiles says why).
// Notice: Nothing.
// Whole: As DragAcceptFiles: on the roadmap, "w16: a file dropped on a program (WM_DROPFILES)".
static int api_DragFinish(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    (void)w;
    w16_arg16(a);
    return 2;
}

const struct w16_api g_shell_api[] = {
    { 9, api_DragAcceptFiles },
    { 11, api_DragQueryFile },
    { 12, api_DragFinish },
    { 22, api_ShellAbout },
};
const unsigned g_shell_api_n = sizeof g_shell_api / sizeof g_shell_api[0];

