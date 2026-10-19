// dialog16.c -- USER's dialog manager, on the task thread: DialogBox reads
// a DIALOG resource, makes a dialog and its controls (wnd16.c), has the
// main thread open the dialog's own window (dlgwin16.c), and runs a modal
// loop until the program's dialog procedure calls EndDialog. BUTTON,
// STATIC and EDIT are w16's own window classes, written here in C as
// Windows' were in USER: they keep their state and tell the dialog
// procedure what happened with WM_COMMAND.
//
// 3.1'S DIALOG KEYBOARD is the dialog manager's, before any control hears
// a key: Tab and Shift+Tab move between WS_TABSTOP controls, Enter is the
// default button, Esc is IDCANCEL, and a control's mnemonic -- or the
// mnemonic of the label before it -- goes to it. The main thread hands
// keys over raw (W16_WM_DLGKEY) so that Shift and Alt are known here.
//
// MessageBox and ShellAbout are dialogs built in code rather than read
// from a resource; their buttons end them with the button's id.
#include "w16_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "keyboard.h"
#include "ui/ugfx.h"
#include "version.h"        // TOYOS_VERSION, generated -- tools/gen_version.sh

#define WS_TABSTOP    0x00010000u
#define WS_GROUP      0x00020000u
#define WS_DLGFRAME   0x00400000u
#define WS_POPUP      0x80000000u
#define DS_SETFONT    0x40
#define BS_DEFPUSH    0x01
#define SS_TYPE       0x0F
#define SS_ICON       0x03
#define SS_NOPREFIX   0x80
#define BN_CLICKED    0
#define IDOK          1
#define IDCANCEL      2

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

static void set_text(struct w16 *w, struct w16_wnd *x, const char *t) {
    char *s = strdup(t ? t : "");
    pthread_mutex_lock(&w->lock);
    free(x->text);
    x->text = s;
    if (x->kind == W16_K_EDIT) {
        int n = s ? (int)strlen(s) : 0;
        x->caret = x->anchor = n;
        x->scroll = 0;
    }
    pthread_mutex_unlock(&w->lock);
}

static void repaint(struct w16 *w, struct w16_wnd *d) {
    w16_task_post_arg(w, W16_POST_DLG_PAINT, d->hwnd);
}

// --- building a dialog ------------------------------------------------------------

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

// A name or an ordinal (0xFF then a word): the ordinal's text is "#n".
static char *rd_name(struct reader *r, unsigned *ord) {
    *ord = 0;
    if (r->at < r->n && r->p[r->at] == 0xFF) {
        r->at++;
        *ord = rd16(r);
        return strdup("");
    }
    return rd_str(r);
}

static int frame_of(uint32_t style) { return style & (WS_DLGFRAME | 0x00800000u) ? 4 : 1; }

int w16_dlg_frame(const struct w16_wnd *d) { return frame_of(d->style); }
int w16_dlg_caption(const struct w16_wnd *d) {
    return (d->style & WS_CAPTION) == WS_CAPTION ? W16_CAPTION - 2 : 0;
}

static struct w16_wnd *new_dialog(struct w16 *w, uint16_t owner, uint32_t style, int cw, int ch,
                                  const char *caption, uint32_t dlgproc) {
    struct w16_wnd *d = w16_wnd_new(w, W16_K_DIALOG);
    if (!d) return 0;
    d->parent = owner;
    d->style = style;
    d->dlgproc = dlgproc;
    d->cw = cw;
    d->ch = ch;
    d->w = cw + 2 * frame_of(style);
    d->h = ch + 2 * frame_of(style) + w16_dlg_caption(d);
    set_text(w, d, caption);
    return d;
}

static struct w16_wnd *new_control(struct w16 *w, struct w16_wnd *d, int kind, uint16_t id,
                                   uint32_t style, int x, int y, int cw, int ch, const char *text) {
    struct w16_wnd *c = w16_wnd_new(w, kind);
    if (!c) return 0;
    c->parent = d->hwnd;
    c->id = id;
    c->style = style;
    c->x = x;
    c->y = y;
    c->w = c->cw = cw;
    c->h = c->ch = ch;
    c->is_default = kind == W16_K_BUTTON && (style & 0x0F) == BS_DEFPUSH;
    set_text(w, c, text);
    return c;
}

static void free_dialog(struct w16 *w, struct w16_wnd *d) {
    for (int i = 0; i < w->nwnds; i++) {
        struct w16_wnd *c = w->wnds[i];
        if (c && c->used && c->parent == d->hwnd && c->kind != W16_K_DIALOG) w16_wnd_free(w, c);
    }
    w16_wnd_free(w, d);
}

// A DIALOG resource (3.x's DLGTEMPLATE) into a dialog: its units are a
// quarter of the font's average width and an eighth of its height.
static struct w16_wnd *from_template(struct w16 *w, const uint8_t *p, uint32_t n, uint16_t owner,
                                     uint32_t dlgproc, char *why, size_t why_cap) {
    struct reader r = { p, n, 0, 0 };
    uint32_t style = rd32(&r);
    unsigned count = rd8(&r);
    int16_t dx = (int16_t)rd16(&r), dy = (int16_t)rd16(&r);
    int16_t dw = (int16_t)rd16(&r), dh = (int16_t)rd16(&r);
    (void)dx; (void)dy;
    unsigned ord;
    char *menu = rd_name(&r, &ord), *cls = rd_name(&r, &ord), *caption = rd_str(&r);
    if (style & DS_SETFONT) {
        rd16(&r);
        free(rd_str(&r));                        // the font: w16 has one
    }
    int bx, by;
    w16_dlg_units(&bx, &by);
    struct w16_wnd *d = r.bad ? 0 : new_dialog(w, owner, style, dw * bx / 4, dh * by / 8, caption, dlgproc);
    int ok = d && menu && cls && !(menu[0] || cls[0]);
    if (d && !ok) snprintf(why, why_cap, "a dialog with its own class or menu is not supported yet");
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
        int k = kind == 0x80 ? W16_K_BUTTON : kind == 0x81 ? W16_K_EDIT : kind == 0x82 ? W16_K_STATIC : -1;
        if (k < 0) {
            snprintf(why, why_cap, "a dialog control of class %s is not supported yet",
                     cname ? cname : kind == 0x83 ? "LISTBOX" : kind == 0x84 ? "SCROLLBAR" :
                     kind == 0x85 ? "COMBOBOX" : "?");
            ok = 0;
        } else if (!new_control(w, d, k, id, cs, x * bx / 4, y * by / 8, cw * bx / 4, ch * by / 8,
                                ord ? "" : text)) {
            ok = 0;
        }
        free(cname);
        free(text);
    }
    if (ok && r.bad) { snprintf(why, why_cap, "its DIALOG resource is cut short"); ok = 0; }
    if (!ok && d) { free_dialog(w, d); d = 0; }
    return d;
}

// --- focus and the keyboard --------------------------------------------------------

static int tabstop(const struct w16_wnd *c) {
    return c->kind != W16_K_STATIC && (c->style & WS_TABSTOP);
}

// The dialog's controls in template order, which is the tab order.
static int controls(struct w16 *w, struct w16_wnd *d, struct w16_wnd ***out) {
    struct w16_wnd **v = malloc(sizeof *v * (size_t)(w->nwnds + 1));
    int n = 0;
    for (int i = 0; v && i < w->nwnds; i++) {
        struct w16_wnd *c = w->wnds[i];
        if (c && c->used && c->parent == d->hwnd && c->kind != W16_K_DIALOG) v[n++] = c;
    }
    // Allocated in template order, but a freed entry may have been reused
    // out of place: order by HWND, which follows creation within a dialog.
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && v[j]->hwnd < v[j - 1]->hwnd; j--) {
            struct w16_wnd *t = v[j]; v[j] = v[j - 1]; v[j - 1] = t;
        }
    *out = v;
    return n;
}

static void set_focus(struct w16 *w, struct w16_wnd *d, struct w16_wnd *c) {
    pthread_mutex_lock(&w->lock);
    struct w16_wnd *was = w16_wnd_get(w, d->focus);
    if (was) was->focused = 0;
    d->focus = c ? c->hwnd : 0;
    if (c) {
        c->focused = 1;
        // An edit reached by the keyboard is selected whole, as on 3.1.
        if (c->kind == W16_K_EDIT && c->text) { c->anchor = 0; c->caret = (int)strlen(c->text); }
    }
    pthread_mutex_unlock(&w->lock);
    repaint(w, d);
}

static void tab(struct w16 *w, struct w16_wnd *d, int back) {
    struct w16_wnd **v;
    int n = controls(w, d, &v), at = -1;
    for (int i = 0; i < n; i++) if (v[i]->hwnd == d->focus) at = i;
    int start = at >= 0 ? at : back ? 0 : n - 1;
    for (int k = 1; k <= n; k++) {
        int i = ((start + (back ? -k : k)) % n + n) % n;
        if (tabstop(v[i])) { set_focus(w, d, v[i]); break; }
    }
    free(v);
}

static uint32_t dlg_call(struct w16 *w, struct w16_wnd *d, uint16_t msg, uint16_t wp, uint32_t lp) {
    if (!d->dlgproc) return 0;
    return w16_call_wndproc(w, (uint16_t)(d->dlgproc >> 16), (uint16_t)d->dlgproc, d->hwnd, msg, wp, lp);
}

// A button chosen: the program's WM_COMMAND, or -- a dialog built here --
// the end, with the button's id.
static void command(struct w16 *w, struct w16_wnd *d, uint16_t id, uint16_t ctl) {
    if (!d->dlgproc) {
        d->result = (int16_t)id;
        d->ended = 1;
        return;
    }
    dlg_call(w, d, WM_COMMAND, id, (uint32_t)BN_CLICKED << 16 | ctl);
}

// Esc: IDCANCEL to the program. A box built here ends with Cancel when it
// has one, else with its button when it has only one (an OK box), else
// stays -- Yes/No has no answer to guess.
static void escape(struct w16 *w, struct w16_wnd *d) {
    if (d->dlgproc || w16_dlg_item(w, d->hwnd, IDCANCEL)) { command(w, d, IDCANCEL, 0); return; }
    struct w16_wnd **v, *only = 0;
    int n = controls(w, d, &v), buttons = 0;
    for (int i = 0; i < n; i++) if (v[i]->kind == W16_K_BUTTON) { buttons++; only = v[i]; }
    free(v);
    if (buttons == 1) command(w, d, only->id, only->hwnd);
}

static void click(struct w16 *w, struct w16_wnd *d, struct w16_wnd *b) {
    command(w, d, b->id, b->hwnd);
}

// The control a mnemonic letter names: a button by its own &-letter, or
// the control after a label carrying it.
static struct w16_wnd *mnemonic(struct w16 *w, struct w16_wnd *d, int ch) {
    struct w16_wnd **v, *hit = 0;
    int n = controls(w, d, &v);
    ch |= 0x20;
    for (int i = 0; i < n && !hit; i++) {
        const char *t = v[i]->text, *amp = t ? strchr(t, '&') : 0;
        if (!amp || (amp[1] | 0x20) != ch || (v[i]->style & SS_NOPREFIX && v[i]->kind == W16_K_STATIC)) continue;
        if (v[i]->kind == W16_K_BUTTON) hit = v[i];
        else for (int j = i + 1; j < n && !hit; j++) if (tabstop(v[j])) hit = v[j];
    }
    free(v);
    return hit;
}

// --- EDIT ----------------------------------------------------------------------

static void edit_replace(struct w16 *w, struct w16_wnd *e, int from, int to, const char *ins) {
    pthread_mutex_lock(&w->lock);
    const char *t = e->text ? e->text : "";
    int len = (int)strlen(t), il = (int)strlen(ins);
    if (from > to) { int s = from; from = to; to = s; }
    if (from < 0) from = 0;
    if (to > len) to = len;
    char *s = malloc((size_t)(len - (to - from) + il + 1));
    if (s) {
        memcpy(s, t, (size_t)from);
        memcpy(s + from, ins, (size_t)il);
        memcpy(s + from + il, t + to, (size_t)(len - to) + 1);
        free(e->text);
        e->text = s;
        e->caret = e->anchor = from + il;
    }
    pthread_mutex_unlock(&w->lock);
}

static void edit_key(struct w16 *w, struct w16_wnd *d, struct w16_wnd *e, int key, unsigned mods) {
    int len = e->text ? (int)strlen(e->text) : 0, sel = e->caret != e->anchor;
    int shift = (mods & KEY_MOD_SHIFT) != 0, to = e->caret;
    switch (key) {
    case KEY_ARROW_LEFT:  to = sel && !shift ? (e->caret < e->anchor ? e->caret : e->anchor) : e->caret - 1; break;
    case KEY_ARROW_RIGHT: to = sel && !shift ? (e->caret > e->anchor ? e->caret : e->anchor) : e->caret + 1; break;
    case KEY_HOME:        to = 0; break;
    case KEY_END:         to = len; break;
    case '\b':
        edit_replace(w, e, sel ? e->anchor : e->caret - 1, e->caret, "");
        repaint(w, d);
        return;
    case KEY_DELETE:
        edit_replace(w, e, e->caret, sel ? e->anchor : e->caret + 1, "");
        repaint(w, d);
        return;
    default:
        if (key >= 0x20 && key < 0x7F && !(mods & (KEY_MOD_CTRL | KEY_MOD_ALT))) {
            char s[2] = { (char)key, 0 };
            edit_replace(w, e, e->anchor, e->caret, s);
            repaint(w, d);
        }
        return;
    }
    if (to < 0) to = 0;
    if (to > len) to = len;
    pthread_mutex_lock(&w->lock);
    e->caret = to;
    if (!shift) e->anchor = to;
    pthread_mutex_unlock(&w->lock);
    repaint(w, d);
}

// The character index under x in an edit, by the dialog font's widths.
static int edit_index_at(struct w16_wnd *e, int x);

// --- a dialog's messages ------------------------------------------------------------

static void key(struct w16 *w, struct w16_wnd *d, int k, unsigned mods) {
    struct w16_wnd *f = w16_wnd_get(w, d->focus);
    if (k == '\t') { tab(w, d, (mods & KEY_MOD_SHIFT) != 0); return; }
    if (k == '\n' || k == '\r') {
        struct w16_wnd **v, *def = 0;
        int n = controls(w, d, &v);
        for (int i = 0; i < n; i++) if (v[i]->is_default) def = v[i];
        free(v);
        if (f && f->kind == W16_K_BUTTON) click(w, d, f);
        else command(w, d, def ? def->id : IDOK, def ? def->hwnd : 0);
        return;
    }
    if (k == 0x1B) { escape(w, d); return; }
    if (f && f->kind == W16_K_BUTTON && k == ' ') { click(w, d, f); return; }
    if (k >= 0x20 && k < 0x7F && ((mods & KEY_MOD_ALT) || !f || f->kind != W16_K_EDIT)) {
        struct w16_wnd *m = mnemonic(w, d, k);
        if (m && m->kind == W16_K_BUTTON) { set_focus(w, d, m); click(w, d, m); return; }
        if (m) { set_focus(w, d, m); return; }
    }
    if (f && f->kind == W16_K_EDIT) edit_key(w, d, f, k, mods);
}

static void mouse(struct w16 *w, struct w16_wnd *d, uint16_t msg, int x, int y) {
    struct w16_wnd *cap = w16_wnd_get(w, d->capture);
    if (msg == WM_LBUTTONDOWN) {
        struct w16_wnd *c = w16_dlg_item_at(w, d->hwnd, x, y);
        if (!c) return;
        if (tabstop(c) || c->kind == W16_K_BUTTON) set_focus(w, d, c);
        if (c->kind == W16_K_BUTTON) {
            pthread_mutex_lock(&w->lock);
            c->pressed = 1;
            d->capture = c->hwnd;
            pthread_mutex_unlock(&w->lock);
        } else if (c->kind == W16_K_EDIT) {
            int i = edit_index_at(c, x - c->x);
            pthread_mutex_lock(&w->lock);
            c->caret = c->anchor = i;
            d->capture = c->hwnd;
            pthread_mutex_unlock(&w->lock);
        }
        repaint(w, d);
        return;
    }
    if (!cap) return;
    int inside = x >= cap->x && y >= cap->y && x < cap->x + cap->w && y < cap->y + cap->h;
    if (msg == WM_MOUSEMOVE) {
        pthread_mutex_lock(&w->lock);
        if (cap->kind == W16_K_BUTTON) cap->pressed = inside;
        else if (cap->kind == W16_K_EDIT) cap->caret = edit_index_at(cap, x - cap->x);
        pthread_mutex_unlock(&w->lock);
        repaint(w, d);
        return;
    }
    if (msg == WM_LBUTTONUP) {
        pthread_mutex_lock(&w->lock);
        int was = cap->pressed;
        cap->pressed = 0;
        d->capture = 0;
        pthread_mutex_unlock(&w->lock);
        repaint(w, d);
        if (cap->kind == W16_K_BUTTON && was && inside) click(w, d, cap);
    }
}

// One message for a dialog or one of its controls; anything else is the
// program's, dispatched as its own message loop would.
static void dlg_message(struct w16 *w, struct w16_wnd *d, const struct w16_msg *m) {
    struct w16_wnd *to = w16_wnd_get(w, m->hwnd);
    if (!to || (to != d && to->parent != d->hwnd)) { w16_dispatch(w, m); return; }
    switch (m->message) {
    case W16_WM_DLGKEY: key(w, d, m->wparam, m->lparam); break;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_MOUSEMOVE:
        mouse(w, d, m->message, (int16_t)m->lparam, (int16_t)(m->lparam >> 16));
        break;
    case WM_CLOSE: escape(w, d); break;
    default: break;
    }
}

// Opens `d`, runs it until EndDialog, closes it; the dialog's result.
static int16_t run(struct w16 *w, struct w16_wnd *d, uint32_t param) {
    w16_task_post_arg(w, W16_POST_DLG_OPEN, d->hwnd);
    struct w16_wnd **v;
    int n = controls(w, d, &v);
    struct w16_wnd *first = 0;
    for (int i = 0; i < n && !first; i++) if (tabstop(v[i])) first = v[i];
    free(v);
    if (!d->dlgproc || dlg_call(w, d, WM_INITDIALOG, first ? first->hwnd : 0, param) & 0xFFFF) {
        if (first && !d->focus) set_focus(w, d, first);
    }
    repaint(w, d);
    while (!d->ended && !w->cpu.fault) {
        struct w16_msg m;
        w16_get_msg(w, &m);
        if (m.message == WM_QUIT) {              // the program is ending: hand it back
            w16_post_msg(w, 0, WM_QUIT, m.wparam, 0);
            break;
        }
        dlg_message(w, d, &m);
    }
    w16_task_post_arg(w, W16_POST_DLG_CLOSE, d->hwnd);
    pthread_mutex_lock(&w->lock);
    while (d->ended != 2 && w->app) pthread_cond_wait(&w->cond, &w->lock);   // the window is gone
    pthread_mutex_unlock(&w->lock);
    int16_t r = d->result;
    free_dialog(w, d);
    return r;
}

static int edit_index_at(struct w16_wnd *e, int x) {
    const char *t = e->text ? e->text : "";
    int best = 0, n = (int)strlen(t);
    pthread_mutex_lock(&g_w16_text_lock);
    const struct ugfx_font *was = ugfx_set_font(w16_system_font());
    char buf[2] = { 0, 0 };
    int at = 2;                                  // the text's inset in its box
    for (int i = e->scroll; i < n && at < x; i++) {
        buf[0] = t[i];
        int cw = ugfx_text_width(buf);
        if (at + cw / 2 >= x) break;
        at += cw;
        best = i + 1;
    }
    if (best < e->scroll) best = e->scroll;
    ugfx_set_font(was);
    pthread_mutex_unlock(&g_w16_text_lock);
    return best;
}

// --- the exports ------------------------------------------------------------------

// DialogBox(hInst, lpTemplate, hWndParent, lpDialogFunc); DialogBoxParam
// adds an lParam, handed to WM_INITDIALOG.
static int dialog_box(struct w16 *w, struct w16_args *a, int with_param) {
    struct cpu16 *c = &w->cpu;
    w16_arg16(a);
    uint32_t tmpl = w16_arg32(a);
    uint16_t owner = w16_arg16(a);
    uint32_t proc = w16_arg32(a);
    uint32_t param = with_param ? w16_arg32(a) : 0;
    char name[64] = "", why[96] = "";
    if (tmpl >> 16 && get_str(w, tmpl, name, sizeof name) < 0) return -1;
    struct ne_resource res;
    if (w16_res_find(w, 5, 0, (uint16_t)(tmpl >> 16 ? 0 : tmpl), tmpl >> 16 ? name : 0, &res) < 0) {
        c->r[R_AX] = 0xFFFF;
        return with_param ? 16 : 12;
    }
    struct w16_wnd *d = from_template(w, w->ne.img + res.off, res.len, owner, proc, why, sizeof why);
    if (!d) {
        cpu16_set_fault(c, FAULT_API, why[0] ? why : "out of memory for a dialog");
        return -1;
    }
    c->r[R_AX] = (uint16_t)run(w, d, param);
    return with_param ? 16 : 12;
}

static int api_DialogBox(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return dialog_box(w, a, 0);
}

static int api_DialogBoxParam(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    return dialog_box(w, a, 1);
}

// EndDialog(hDlg, nResult)
static int api_EndDialog(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_wnd *d = w16_wnd_get(w, w16_arg16(a));
    int16_t r = (int16_t)w16_arg16(a);
    if (d && d->kind == W16_K_DIALOG) { d->result = r; d->ended = 1; }
    w->cpu.r[R_AX] = d != 0;
    return 4;
}

// GetDlgItem(hDlg, id)
static int api_GetDlgItem(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t dlg = w16_arg16(a), id = w16_arg16(a);
    struct w16_wnd *c = w16_dlg_item(w, dlg, id);
    w->cpu.r[R_AX] = c ? c->hwnd : 0;
    return 4;
}

// SetDlgItemText(hDlg, id, lpString)
static int api_SetDlgItemText(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t dlg = w16_arg16(a), id = w16_arg16(a);
    uint32_t far = w16_arg32(a);
    struct w16_wnd *c = w16_dlg_item(w, dlg, id), *d = w16_wnd_get(w, dlg);
    if (c) {
        char *s = dup_far(w, far);
        if (!s) return -1;
        set_text(w, c, s);
        free(s);
        if (d) repaint(w, d);
    }
    return 8;
}

// GetDlgItemText(hDlg, id, lpString, nMaxCount): the length copied
static int api_GetDlgItemText(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t dlg = w16_arg16(a), id = w16_arg16(a);
    uint32_t far = w16_arg32(a);
    uint16_t max = w16_arg16(a);
    struct w16_wnd *c = w16_dlg_item(w, dlg, id);
    unsigned n = 0;
    if (max) {
        pthread_mutex_lock(&w->lock);
        const char *t = c && c->text ? c->text : "";
        n = (unsigned)strlen(t);
        if (n > (unsigned)max - 1) n = max - 1u;
        uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, n + 1, 1);
        if (p) { memcpy(p, t, n); p[n] = 0; }
        pthread_mutex_unlock(&w->lock);
        if (!p) return -1;
    }
    w->cpu.r[R_AX] = (uint16_t)n;
    return 10;
}

// SetDlgItemInt(hDlg, id, uValue, bSigned)
static int api_SetDlgItemInt(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t dlg = w16_arg16(a), id = w16_arg16(a), v = w16_arg16(a), sig = w16_arg16(a);
    struct w16_wnd *c = w16_dlg_item(w, dlg, id), *d = w16_wnd_get(w, dlg);
    if (c) {
        char s[16];
        if (sig) snprintf(s, sizeof s, "%d", (int16_t)v);
        else snprintf(s, sizeof s, "%u", v);
        set_text(w, c, s);
        if (d) repaint(w, d);
    }
    return 8;
}

// GetDlgItemInt(hDlg, id, lpTranslated, bSigned): the number, and whether
// the text was one (3.1 accepts leading spaces and, signed, a minus).
static int api_GetDlgItemInt(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t dlg = w16_arg16(a), id = w16_arg16(a);
    uint32_t ok_far = w16_arg32(a);
    uint16_t sig = w16_arg16(a);
    struct w16_wnd *c = w16_dlg_item(w, dlg, id);
    long v = 0;
    int ok = 0;
    pthread_mutex_lock(&w->lock);
    const char *t = c && c->text ? c->text : "";
    while (*t == ' ') t++;
    int neg = sig && *t == '-';
    if (neg) t++;
    for (; *t >= '0' && *t <= '9' && v <= 0x10000; t++, ok = 1) v = v * 10 + (*t - '0');
    while (*t == ' ') t++;
    pthread_mutex_unlock(&w->lock);
    if (*t || v > (sig ? 32767 + neg : 65535)) ok = 0;
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

// --- dialogs built in code: MessageBox, ShellAbout -------------------------------

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
        char *line = strndup(p, (size_t)len);
        int lw = line ? ugfx_text_width(line) : 0;
        free(line);
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
    struct w16_wnd *d = new_dialog(w, owner, WS_POPUP | WS_CAPTION | WS_SYSMENU, cw, chh, caption, 0);
    if (!d) return 0;
    if (icon) new_control(w, d, W16_K_STATIC, 0xFFFF, SS_ICON | (uint32_t)(icon & 0xFF) << 8, pad, pad, 32, 32, "");
    new_control(w, d, W16_K_STATIC, 0xFFFF, SS_NOPREFIX, pad + iw, pad + (top - texth) / 2, textw, texth, text);
    int x = (cw - row) / 2;
    for (int i = 0; i < n; i++, x += bw + pad)
        new_control(w, d, W16_K_BUTTON, ids[i], WS_TABSTOP | (i == def ? BS_DEFPUSH : 0),
                    x, pad + top + pad, bw, bh, labels[i]);
    return d;
}

int16_t w16_message_box(struct w16 *w, uint16_t owner, const char *text, const char *caption, unsigned type) {
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
    return d ? run(w, d, 0) : 0;
}

// MessageBox(hwnd, lpText, lpCaption, uType)
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
    return 12;
}

// ShellAbout(hWnd, szApp, szOtherStuff, hIcon): Windows 3.1's About layout
// -- the program's icon and its own two strings -- with what runs it where
// 3.1 named itself. "Name#title": the part after '#' is the first line.
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
        int iw, ih, icon = w16_icon(&w->ne, px, &iw, &ih) == 0 ? -1 : 0;
        struct w16_wnd *d = built(w, owner, cap, text, icon, ok, ids, 1, 0);
        if (d) run(w, d, 0);
    }
    free(text); free(cap); free(app); free(other);
    w->cpu.r[R_AX] = 1;
    return 12;
}

// The sound driver: voices that play nothing, yet -- a program that asks
// for one is answered as if the queue played (Minesweeper's tick).
static int api_OpenSound(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal; (void)a;
    w->cpu.r[R_AX] = 1;                          // one voice
    return 0;
}
static int api_SoundNothing(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    const struct w16_name *n = w16_name_lookup("SOUND", ordinal);
    (void)a;
    w->cpu.r[R_AX] = 0;
    w->cpu.r[R_DX] = 0;
    return n && n->argbytes > 0 ? n->argbytes : 0;
}

const struct w16_api g_dialog_api[] = {
    { 1, api_MessageBox },
    { 87, api_DialogBox },
    { 88, api_EndDialog },
    { 91, api_GetDlgItem },
    { 92, api_SetDlgItemText },
    { 93, api_GetDlgItemText },
    { 94, api_SetDlgItemInt },
    { 95, api_GetDlgItemInt },
    { 239, api_DialogBoxParam },
};
const unsigned g_dialog_api_n = sizeof g_dialog_api / sizeof g_dialog_api[0];

const struct w16_api g_shell_api[] = {
    { 22, api_ShellAbout },
};
const unsigned g_shell_api_n = sizeof g_shell_api / sizeof g_shell_api[0];

const struct w16_api g_sound_api[] = {
    { 1, api_OpenSound },
    { 2, api_SoundNothing },     // CloseSound
    { 4, api_SoundNothing },     // SetVoiceNote
    { 5, api_SoundNothing },     // SetVoiceAccent
    { 9, api_SoundNothing },     // StartSound
    { 10, api_SoundNothing },    // StopSound
};
const unsigned g_sound_api_n = sizeof g_sound_api / sizeof g_sound_api[0];
