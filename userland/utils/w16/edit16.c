// edit16.c -- the EDIT class, as USER had it built in: a single-line
// field or a multi-line editor, its selection, caret and scrolling, the
// clipboard, one level of undo, and the EM_* messages a program drives it
// with.
//
// THE TEXT LIVES IN THE PROGRAM'S LOCAL HEAP, as on Windows: an edit made
// with the program's hInstance keeps it in a block of that DGROUP, and
// EM_GETHANDLE hands the program that block -- Notepad reads a file
// straight into one and gives it back with EM_SETHANDLE. An edit of w16's
// own (a common dialog's) keeps its text in w16's memory instead. Either
// way the text is NUL-terminated, Windows' "\r\n" between lines; a lone
// "\n" breaks a line too, as Wine's edit lets it, so a SaunaOS text file
// reads as lines.
//
// LINES are recomputed from the text after every change: each line's
// start and length, a hard break's two (or one) characters after it, or
// none where WORD WRAP broke it -- a multi-line edit without
// ES_AUTOHSCROLL wraps at its width.
#include "w16_internal.h"

#include <stdlib.h>
#include <string.h>
#include "keyboard.h"
#include "ui/ugfx.h"

#define ES_CENTER      0x0001
#define ES_RIGHT       0x0002
#define ES_MULTILINE   0x0004
#define ES_UPPERCASE   0x0008
#define ES_LOWERCASE   0x0010
#define ES_PASSWORD    0x0020
#define ES_AUTOVSCROLL 0x0040
#define ES_AUTOHSCROLL 0x0080
#define ES_NOHIDESEL   0x0100
#define ES_READONLY    0x0800
#define ES_WANTRETURN  0x1000

#define EM_GETSEL      (WM_USER + 0)
#define EM_SETSEL      (WM_USER + 1)
#define EM_GETRECT     (WM_USER + 2)
#define EM_SETRECT     (WM_USER + 3)
#define EM_SETRECTNP   (WM_USER + 4)
#define EM_SCROLL      (WM_USER + 5)
#define EM_LINESCROLL  (WM_USER + 6)
#define EM_GETMODIFY   (WM_USER + 8)
#define EM_SETMODIFY   (WM_USER + 9)
#define EM_GETLINECOUNT (WM_USER + 10)
#define EM_LINEINDEX   (WM_USER + 11)
#define EM_SETHANDLE   (WM_USER + 12)
#define EM_GETHANDLE   (WM_USER + 13)
#define EM_GETTHUMB    (WM_USER + 14)
#define EM_LINELENGTH  (WM_USER + 17)
#define EM_REPLACESEL  (WM_USER + 18)
#define EM_SETFONT     (WM_USER + 19)
#define EM_GETLINE     (WM_USER + 20)
#define EM_LIMITTEXT   (WM_USER + 21)
#define EM_CANUNDO     (WM_USER + 22)
#define EM_UNDO        (WM_USER + 23)
#define EM_FMTLINES    (WM_USER + 24)
#define EM_LINEFROMCHAR (WM_USER + 25)
#define EM_SETWORDBREAK (WM_USER + 26)
#define EM_SETTABSTOPS (WM_USER + 27)
#define EM_SETPASSWORDCHAR (WM_USER + 28)
#define EM_EMPTYUNDOBUFFER (WM_USER + 29)
#define EM_GETFIRSTVISIBLELINE (WM_USER + 30)
#define EM_SETREADONLY (WM_USER + 31)
#define EM_GETPASSWORDCHAR (WM_USER + 34)

#define EN_SETFOCUS    0x0100
#define EN_KILLFOCUS   0x0200
#define EN_CHANGE      0x0300
#define EN_UPDATE      0x0400
#define EN_ERRSPACE    0x0500
#define EN_MAXTEXT     0x0501
#define EN_HSCROLL     0x0601
#define EN_VSCROLL     0x0602

#define SB_LINEUP        0
#define SB_LINEDOWN      1
#define SB_PAGEUP        2
#define SB_PAGEDOWN      3
#define SB_THUMBPOSITION 4
#define SB_THUMBTRACK    5
#define SB_TOP           6
#define SB_BOTTOM        7

#define LMEM_MOVEABLE  0x0002
#define LIMIT_DEFAULT  30000          // EM_LIMITTEXT's default, Windows 3.1's
// The text's inset from the client's left edge: 3.11's multi-line edit has
// none (Notepad's text starts at its edge); a one-line field, which draws
// its frame inside its client area, keeps two.
#define MARGIN (multi(x) ? 0 : 2)

struct line {
    int start, len, brk;              // brk: the line break's characters after it (0 a wrap)
};

struct edit {
    uint16_t hbuf;                    // the text in the program's local heap (EM_GETHANDLE's)...
    char *host;                       // ...or in w16's own memory, for an edit of w16's
    unsigned cap;                     // host: its allocation
    int len;
    int anchor, caret;                // the selection is between them; the caret is the moving end
    int top;                          // the first line shown
    int xoff;                         // pixels scrolled to the left
    int limit;
    int modified, focused, captured;
    int want_x;                       // the caret's x a move up or down keeps
    char pw;                          // ES_PASSWORD's character
    struct line *ln;
    int nlines, ln_cap;
    char *undo;                       // EM_UNDO: what was removed at undo_at...
    int undo_at, undo_ins;            // ...and how much was put in its place
    int can_undo;
    uint16_t hfont;                   // WM_SETFONT's: Notepad's is SYSTEM_FIXED_FONT
    int noredraw;                     // WM_SETREDRAW(FALSE): changes are not drawn till it is TRUE
    int blink_off;                    // the caret's blink is in its off half
};

#define CARET_TIMER 0xFFF0            // the caret's blink, a timer of the edit's own
#define CARET_MS    530               // GetCaretBlinkTime's default

static int multi(const struct w16_wnd *x) { return (x->style & ES_MULTILINE) != 0; }

static struct edit *ed(struct w16_wnd *x) { return x->ctl; }

// Its font, for the caller holding the text lock.
static const struct ugfx_font *efont(const struct edit *e) { return w16_font_of(e->hfont); }

// A line's height: its font's 3.11 cell, not the stand-in face's own.
static int line_h(const struct edit *e) { return w16_font_line_h(e->hfont); }

// Drawn again, unless WM_SETREDRAW said not yet.
static void redraw(struct w16 *w, struct w16_wnd *x, struct edit *e) {
    if (!e->noredraw) w16_invalidate(w, x, 0);
}

// --- the text ------------------------------------------------------------------

static int local(struct w16 *w, const struct w16_wnd *x) { return x->inst == w->dgroup && w->dgroup; }

// The text, NUL-terminated; valid until the next change to it or to the
// local heap. Never NULL.
static char *text(struct w16 *w, struct edit *e) {
    static char empty[1];
    if (e->hbuf) {
        unsigned size;
        uint8_t *p = w16_local_ptr(w, e->hbuf, &size);
        if (p && size) return (char *)p;
        return empty;
    }
    return e->host ? e->host : empty;
}

// Room for `need` characters and the NUL; 0 when there is none to have.
static int reserve(struct w16 *w, struct w16_wnd *x, struct edit *e, int need) {
    if (e->hbuf) {
        unsigned size;
        if (w16_local_ptr(w, e->hbuf, &size) && size >= (unsigned)need + 1) return 1;
        if (need + 1 > 0xFFF0) return 0;
        uint16_t h = w16_local_realloc(w, e->hbuf, (uint16_t)(need + 1 + 32), LMEM_MOVEABLE);
        if (!h) return 0;
        e->hbuf = h;
        return 1;
    }
    if (local(w, x)) {                           // the first text of a program's edit
        uint16_t h = w16_local_alloc(w, LMEM_MOVEABLE, (uint16_t)(need + 1 + 32));
        if (!h) return 0;
        unsigned size;
        char *p = (char *)w16_local_ptr(w, h, &size);
        if (!p) return 0;
        memcpy(p, e->host ? e->host : "", (size_t)e->len + 1);
        free(e->host);
        e->host = 0;
        e->cap = 0;
        e->hbuf = h;
        return 1;
    }
    if (e->cap >= (unsigned)need + 1) return 1;
    unsigned cap = e->cap ? e->cap : 64;
    while (cap < (unsigned)need + 1) cap *= 2;
    char *g = realloc(e->host, cap);
    if (!g) return 0;
    if (!e->host) g[0] = 0;
    e->host = g;
    e->cap = cap;
    return 1;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int sel_lo(const struct edit *e) { return e->anchor < e->caret ? e->anchor : e->caret; }
static int sel_hi(const struct edit *e) { return e->anchor > e->caret ? e->anchor : e->caret; }

// --- measuring, in the system font (the caller holds the text lock) ------------

// 32 dialog units, eight average characters of the font being measured
// (the caller has set it): 3.1's tab stops.
static int tab_px(void) {
    static const struct ugfx_font *of;
    static int px;
    const struct ugfx_font *f = ugfx_set_font(0);
    ugfx_set_font(f);
    if (f != of || !px) {
        of = f;
        px = (ugfx_text_width("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") + 26) / 52 * 8;
        if (px < 8) px = 64;
    }
    return px;
}

// The width of t[from, to) drawn from x = `at` (tabs step to stops).
static int seg_width(const char *t, int from, int to, int at, char pw) {
    int x = at, tab = tab_px();
    while (from < to) {
        if (t[from] == '\t' && !pw) {
            x = ((x - at) / tab + 1) * tab + at;
            from++;
            continue;
        }
        int run = from;
        while (run < to && (t[run] != '\t' || pw)) run++;
        if (pw) {
            char c[2] = { pw, 0 };
            x += ugfx_text_width(c) * (run - from);
        } else {
            x += ugfx_text_width_n(t + from, run - from);
        }
        from = run;
    }
    return x - at;
}

// The index on line l nearest x pixels into it.
static int index_at(const char *t, const struct line *l, int x, char pw) {
    int best = l->start, bestd = x < 0 ? -x : x;
    for (int i = l->start + 1; i <= l->start + l->len; i++) {
        int d = seg_width(t, l->start, i, 0, pw) - x;
        if (d < 0) d = -d;
        if (d < bestd) { best = i; bestd = d; }
        else if (d > bestd && i > best + 1) break;
    }
    return best;
}

// --- lines ---------------------------------------------------------------------

static int add_line(struct edit *e, int start, int len, int brk) {
    if (e->nlines == e->ln_cap) {
        int cap = e->ln_cap ? e->ln_cap * 2 : 64;
        struct line *g = realloc(e->ln, sizeof *g * (size_t)cap);
        if (!g) return 0;
        e->ln = g;
        e->ln_cap = cap;
    }
    e->ln[e->nlines++] = (struct line){ start, len, brk };
    return 1;
}

static void reflow(struct w16 *w, struct w16_wnd *x, struct edit *e) {
    const char *t = text(w, e);
    e->nlines = 0;
    if (!multi(x)) { add_line(e, 0, e->len, 0); return; }
    int wrap = !(x->style & (ES_AUTOHSCROLL | WS_HSCROLL)), room = x->cw - 2 * MARGIN;
    pthread_mutex_lock(&g_w16_text_lock);
    const struct ugfx_font *was = ugfx_set_font(efont(e));
    int i = 0;
    for (;;) {
        int end = i;
        while (end < e->len && t[end] != '\r' && t[end] != '\n') end++;
        int brk = end >= e->len ? 0 : t[end] == '\n' ? 1 : t[end + 1] == '\n' ? 2 : 1;
        // A hard line, wrapped into as many as the width needs.
        int s = i;
        while (wrap && room > 0 && s < end && seg_width(t, s, end, 0, e->pw) > room) {
            int fit = s + 1;
            while (fit < end && seg_width(t, s, fit + 1, 0, e->pw) <= room) fit++;
            int cut = fit;
            while (cut > s && t[cut - 1] != ' ' && t[cut - 1] != '\t') cut--;
            if (cut == s) cut = fit;             // one word wider than the line
            if (!add_line(e, s, cut - s, 0)) goto out;
            s = cut;
        }
        if (!add_line(e, s, end - s, brk)) goto out;
        if (end >= e->len) break;
        i = end + brk;
    }
out:
    ugfx_set_font(was);
    pthread_mutex_unlock(&g_w16_text_lock);
    if (!e->nlines) add_line(e, 0, 0, 0);
}

static int line_of(const struct edit *e, int i) {
    int lo = 0, hi = e->nlines - 1;
    while (lo < hi) {                            // the last line starting at or before i
        int mid = (lo + hi + 1) / 2;
        if (e->ln[mid].start <= i) lo = mid;
        else hi = mid - 1;
    }
    // At a wrap, the index belongs to the next line's start.
    return lo;
}

static int visible_lines(const struct w16_wnd *x) {
    const struct edit *e = x->ctl;
    int n = e ? x->ch / line_h(e) : 1;
    return n > 0 ? n : 1;
}

// --- scrolling and the scroll bars ------------------------------------------------

static int widest(struct w16 *w, struct edit *e) {
    const char *t = text(w, e);
    int best = 0;
    pthread_mutex_lock(&g_w16_text_lock);
    const struct ugfx_font *was = ugfx_set_font(efont(e));
    for (int i = 0; i < e->nlines; i++) {
        int wd = seg_width(t, e->ln[i].start, e->ln[i].start + e->ln[i].len, 0, e->pw);
        if (wd > best) best = wd;
    }
    ugfx_set_font(was);
    pthread_mutex_unlock(&g_w16_text_lock);
    return best;
}

// THE THUMB IS A PERCENTAGE, 0 to 100, as 3.1's EDIT had it: its own bars
// are ranged so, and EM_GETTHUMB answers so -- Notepad's bars are its
// window's, not the edit's, and it moves their thumb to what this says.
static int max_top(struct w16_wnd *x, struct edit *e) {
    int m = e->nlines - visible_lines(x);
    return m > 0 ? m : 0;
}

static int max_xoff(struct w16 *w, struct w16_wnd *x, struct edit *e) {
    int m = widest(w, e) + 2 * MARGIN - x->cw;
    return m > 0 ? m : 0;
}

static int thumb(int v, int max) { return max ? (int)((long)v * 100 / max) : 0; }

static void update_bars(struct w16 *w, struct w16_wnd *x, struct edit *e) {
    if (x->style & WS_VSCROLL) w16_sbar_set(w, x, SB_VERT, 0, 100, thumb(e->top, max_top(x, e)), 1);
    if (x->style & WS_HSCROLL) w16_sbar_set(w, x, SB_HORZ, 0, 100, thumb(e->xoff, max_xoff(w, x, e)), 1);
}

// Scrolled so the caret shows, the bars told -- and the parent, with
// EN_VSCROLL or EN_HSCROLL when the text moved, as Wine's edit tells it.
static void show_caret(struct w16 *w, struct w16_wnd *x, struct edit *e) {
    int l = line_of(e, e->caret), vis = visible_lines(x), top0 = e->top, xoff0 = e->xoff;
    if (multi(x)) {
        if (l < e->top) e->top = l;
        if (l >= e->top + vis) e->top = l - vis + 1;
    }
    if (e->top > e->nlines - 1) e->top = e->nlines - 1;
    if (e->top < 0) e->top = 0;
    int wrap = multi(x) && !(x->style & (ES_AUTOHSCROLL | WS_HSCROLL));
    if (!wrap) {
        const char *t = text(w, e);
        pthread_mutex_lock(&g_w16_text_lock);
        const struct ugfx_font *was = ugfx_set_font(efont(e));
        int cx = seg_width(t, e->ln[l].start, e->caret, 0, e->pw), room = x->cw - 2 * MARGIN;
        ugfx_set_font(was);
        pthread_mutex_unlock(&g_w16_text_lock);
        if (cx < e->xoff) e->xoff = cx - room / 4 > 0 ? cx - room / 4 : 0;
        if (cx > e->xoff + room - 1) e->xoff = cx - room + room / 4;
        if (e->xoff < 0) e->xoff = 0;
    } else {
        e->xoff = 0;
    }
    update_bars(w, x, e);
    if (e->top != top0) w16_notify(w, x, EN_VSCROLL);
    if (e->xoff != xoff0) w16_notify(w, x, EN_HSCROLL);
}

static void scroll_to(struct w16 *w, struct w16_wnd *x, struct edit *e, int top, int xoff) {
    top = clampi(top, 0, e->nlines - 1 > 0 ? e->nlines - 1 : 0);
    if (xoff < 0) xoff = 0;
    if (top == e->top && xoff == e->xoff) return;
    int vert = top != e->top;
    e->top = top;
    e->xoff = xoff;
    update_bars(w, x, e);
    redraw(w, x, e);
    w16_notify(w, x, vert ? EN_VSCROLL : EN_HSCROLL);
}

// The caret shown, its blink started over: what a move or a key does,
// so it never vanishes under the typing.
static void caret_on(struct w16 *w, struct w16_wnd *x, struct edit *e) {
    e->blink_off = 0;
    if (e->focused) w16_set_timer(w, x->hwnd, CARET_TIMER, CARET_MS);
}

// --- changing the text -------------------------------------------------------------

static void set_undo(struct edit *e, const char *t, int at, int removed, int inserted) {
    // Typing on at the end of what was just typed is one step to undo.
    if (e->can_undo && !removed && at == e->undo_at + e->undo_ins && e->undo && !e->undo[0]) {
        e->undo_ins += inserted;
        return;
    }
    char *u = strndup(t + at, (size_t)removed);
    if (!u) { e->can_undo = 0; return; }
    free(e->undo);
    e->undo = u;
    e->undo_at = at;
    e->undo_ins = inserted;
    e->can_undo = 1;
}

// [from, to) replaced by `ins` (n characters), the caret after it. 0 when
// it did not fit: the limit (EN_MAXTEXT) or the heap (EN_ERRSPACE).
static int replace(struct w16 *w, struct w16_wnd *x, struct edit *e, int from, int to, const char *ins,
                   int n, int undoable) {
    from = clampi(from, 0, e->len);
    to = clampi(to, from, e->len);
    int limit = e->limit ? e->limit : LIMIT_DEFAULT, newlen = e->len - (to - from) + n;
    if (n && newlen > limit && newlen > e->len) {
        n = limit - (e->len - (to - from));
        if (n <= 0) { w16_notify(w, x, EN_MAXTEXT); return 0; }
        if (n > 0 && ins[n - 1] == '\r') n--;    // never half a line break
    }
    newlen = e->len - (to - from) + n;
    char *copy = n ? strndup(ins, (size_t)n) : 0;  // `ins` may be in the text being moved
    if (n && !copy) return 0;
    if (!reserve(w, x, e, newlen)) { free(copy); w16_notify(w, x, EN_ERRSPACE); return 0; }
    char *t = text(w, e);
    if (undoable) set_undo(e, t, from, to - from, n);
    memmove(t + from + n, t + to, (size_t)(e->len - to) + 1);
    if (n) memcpy(t + from, copy, (size_t)n);
    free(copy);
    e->len = newlen;
    e->anchor = e->caret = from + n;
    e->modified = 1;
    caret_on(w, x, e);
    reflow(w, x, e);
    show_caret(w, x, e);
    redraw(w, x, e);
    w16_notify(w, x, EN_UPDATE);
    w16_notify(w, x, EN_CHANGE);
    return 1;
}

// The selection replaced by C text, ES_UPPERCASE and friends applied.
static void type_text(struct w16 *w, struct w16_wnd *x, struct edit *e, const char *s, int n) {
    if (x->style & ES_READONLY) return;
    char *c = strndup(s, (size_t)n);
    if (!c) return;
    for (int i = 0; i < n; i++) {
        if ((x->style & ES_UPPERCASE) && c[i] >= 'a' && c[i] <= 'z') c[i] = (char)(c[i] - 32);
        if ((x->style & ES_LOWERCASE) && c[i] >= 'A' && c[i] <= 'Z') c[i] = (char)(c[i] + 32);
    }
    replace(w, x, e, sel_lo(e), sel_hi(e), c, n, 1);
    free(c);
}

static void set_all(struct w16 *w, struct w16_wnd *x, struct edit *e, const char *s) {
    int n = (int)strlen(s);
    if (!multi(x)) {                             // one line: it stops at the first break
        int i = 0;
        while (i < n && s[i] != '\r' && s[i] != '\n') i++;
        n = i;
    }
    int was_limit = e->limit;
    e->limit = 0xFFF0;                           // WM_SETTEXT is not held to EM_LIMITTEXT
    replace(w, x, e, 0, e->len, s, n, 0);
    e->limit = was_limit;
    e->anchor = e->caret = 0;
    e->top = 0;
    e->xoff = 0;
    e->modified = 0;
    e->can_undo = 0;
    show_caret(w, x, e);
}

// --- the clipboard --------------------------------------------------------------------

static void copy(struct w16 *w, struct edit *e) {
    int lo = sel_lo(e), hi = sel_hi(e);
    if (lo == hi || e->pw) return;
    w16_clip_put_text(text(w, e) + lo, hi - lo);
}

static void paste(struct w16 *w, struct w16_wnd *x, struct edit *e) {
    int n, m = 0;
    char *s = w16_clip_get_text(&n);             // Windows' "\r\n" and ANSI already
    if (!s) return;
    while (m < n && (multi(x) || (s[m] != '\r' && s[m] != '\n'))) m++;   // one line: the first
    type_text(w, x, e, s, m);
    free(s);
}

// --- painting ----------------------------------------------------------------------

// Where line i's text starts, from the client area's left: the margin less the scroll, or with
// ES_CENTER or ES_RIGHT the line centred or set against the right margin -- each line of a
// multiline edit, as 3.1 aligned them (Date/Time's fields are one-line multiline edits for it).
// The edit's font current, under g_w16_text_lock.
static int line_left(const struct w16_wnd *x, const struct edit *e, const char *t, int i) {
    if (!(x->style & (ES_CENTER | ES_RIGHT))) return MARGIN - e->xoff;
    int wd = seg_width(t, e->ln[i].start, e->ln[i].start + e->ln[i].len, 0, e->pw);
    return x->style & ES_CENTER ? (x->cw - wd) / 2 : x->cw - MARGIN - wd;
}

static void paint(struct w16 *w, struct w16_wnd *x, struct edit *e) {
    struct ugfx_surface s;
    int ox, oy;
    w16_paint_begin(w, x);
    if (!w16_wnd_paint(w, x, &s, &ox, &oy)) return;
    uint32_t white = w16_rgb(0xFFFFFF), black = w16_rgb(0x000000), navy = w16_rgb(0x000080);
    uint32_t ink = (x->style & WS_DISABLED) ? w16_rgb(0x808080) : black;
    ugfx_fill_rect(&s, ox, oy, x->cw, x->ch, white);
    // A single-line edit with WS_BORDER draws its frame inside its own
    // client area in a dialog template; the border style is non-client.
    pthread_mutex_lock(&g_w16_text_lock);
    const struct ugfx_font *was = ugfx_set_font(efont(e));
    const char *t = text(w, e);
    int lh = line_h(e), vis = visible_lines(x), lo = sel_lo(e), hi = sel_hi(e);
    int show_sel = e->focused || (x->style & ES_NOHIDESEL);
    int y0 = multi(x) ? oy : oy + (x->ch - lh) / 2;
    struct ugfx_clip saved;
    ugfx_clip_save(&s, &saved);
    ugfx_clip_intersect(&s, ox + 1, oy, x->cw - 1, x->ch);
    for (int i = e->top; i < e->nlines && i < e->top + vis + 1; i++) {
        const struct line *l = &e->ln[i];
        int y = y0 + (i - e->top) * lh, at = l->start, end = l->start + l->len;
        int base = ox + line_left(x, e, t, i);
        // Runs of one kind -- selected or not -- each drawn whole.
        while (at < end) {
            int sel = show_sel && at >= lo && at < hi, run = at;
            while (run < end && (show_sel && run >= lo && run < hi) == sel) run++;
            int px = base + seg_width(t, l->start, at, 0, e->pw);
            int wd = seg_width(t, at, run, px - base, e->pw);
            if (sel) ugfx_fill_rect(&s, px, y, wd, lh, navy);
            // Each tab-free piece: ugfx_draw_string knows nothing of tabs.
            for (int p = at; p < run; ) {
                int q = p;
                while (q < run && (t[q] != '\t' || e->pw)) q++;
                if (q > p) {
                    char *piece = e->pw ? malloc((size_t)(q - p) + 1) : strndup(t + p, (size_t)(q - p));
                    if (piece && e->pw) { memset(piece, e->pw, (size_t)(q - p)); piece[q - p] = 0; }
                    if (piece) ugfx_draw_string(&s, base + seg_width(t, l->start, p, 0, e->pw), y, piece,
                                                sel ? white : ink, UGFX_TRANSPARENT);
                    free(piece);
                }
                p = q < run ? q + 1 : q;
            }
            at = run;
        }
        // A selected line break shows as a sliver, as 3.1's did.
        if (show_sel && l->brk && end >= lo && end < hi)
            ugfx_fill_rect(&s, base + seg_width(t, l->start, end, 0, e->pw), y, 4, lh, navy);
    }
    if (e->focused && !e->blink_off) {
        int cl = line_of(e, e->caret);
        if (cl >= e->top && cl < e->top + vis + 1) {
            const struct line *l = &e->ln[cl];
            int cx = ox + line_left(x, e, t, cl) + seg_width(t, l->start, e->caret, 0, e->pw);
            ugfx_fill_rect(&s, cx, y0 + (cl - e->top) * lh, 1, lh, black);
        }
    }
    ugfx_clip_restore(&s, &saved);
    ugfx_set_font(was);
    pthread_mutex_unlock(&g_w16_text_lock);
    w16_wnd_paint_done(w);
}

// --- the caret moving ------------------------------------------------------------------

static int caret_x(struct w16 *w, struct edit *e) {
    const char *t = text(w, e);
    const struct line *l = &e->ln[line_of(e, e->caret)];
    pthread_mutex_lock(&g_w16_text_lock);
    const struct ugfx_font *was = ugfx_set_font(efont(e));
    int cx = seg_width(t, l->start, e->caret, 0, e->pw);
    ugfx_set_font(was);
    pthread_mutex_unlock(&g_w16_text_lock);
    return cx;
}

static int on_line_x(struct w16 *w, struct edit *e, int line, int x) {
    const char *t = text(w, e);
    pthread_mutex_lock(&g_w16_text_lock);
    const struct ugfx_font *was = ugfx_set_font(efont(e));
    int i = index_at(t, &e->ln[line], x, e->pw);
    ugfx_set_font(was);
    pthread_mutex_unlock(&g_w16_text_lock);
    return i;
}

static int is_word(char c) { return c != ' ' && c != '\t' && c != '\r' && c != '\n'; }

static int word_left(const char *t, int i) {
    while (i > 0 && !is_word(t[i - 1])) i--;
    while (i > 0 && is_word(t[i - 1])) i--;
    return i;
}

static int word_right(const char *t, int len, int i) {
    while (i < len && is_word(t[i])) i++;
    while (i < len && !is_word(t[i])) i++;
    return i;
}

// One character left or right, a "\r\n" counting as one.
static int step(const char *t, int len, int i, int dir) {
    if (dir < 0) {
        if (i <= 0) return 0;
        return i >= 2 && t[i - 1] == '\n' && t[i - 2] == '\r' ? i - 2 : i - 1;
    }
    if (i >= len) return len;
    return t[i] == '\r' && t[i + 1] == '\n' ? i + 2 : i + 1;
}

static void move(struct w16 *w, struct w16_wnd *x, struct edit *e, int to, int extend, int keep_x) {
    caret_on(w, x, e);
    e->caret = clampi(to, 0, e->len);
    if (!extend) e->anchor = e->caret;
    if (!keep_x) e->want_x = -1;
    show_caret(w, x, e);
    redraw(w, x, e);
}

static void key_down(struct w16 *w, struct w16_wnd *x, struct edit *e, uint16_t vk) {
    unsigned m = w->key_mods;
    int shift = (m & KEY_MOD_SHIFT) != 0, ctrl = (m & KEY_MOD_CTRL) != 0;
    const char *t = text(w, e);
    int l = line_of(e, e->caret), vis = visible_lines(x);
    switch (vk) {
    case VK_LEFT:
        if (!shift && e->anchor != e->caret) { move(w, x, e, sel_lo(e), 0, 0); break; }
        move(w, x, e, ctrl ? word_left(t, e->caret) : step(t, e->len, e->caret, -1), shift, 0);
        break;
    case VK_RIGHT:
        if (!shift && e->anchor != e->caret) { move(w, x, e, sel_hi(e), 0, 0); break; }
        move(w, x, e, ctrl ? word_right(t, e->len, e->caret) : step(t, e->len, e->caret, 1), shift, 0);
        break;
    case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT: {
        if (!multi(x)) break;
        if (e->want_x < 0) e->want_x = caret_x(w, e);
        int d = vk == VK_UP ? -1 : vk == VK_DOWN ? 1 : vk == VK_PRIOR ? -(vis > 1 ? vis - 1 : 1) : (vis > 1 ? vis - 1 : 1);
        int nl = clampi(l + d, 0, e->nlines - 1);
        if (vk == VK_PRIOR || vk == VK_NEXT) e->top = clampi(e->top + d, 0, e->nlines - 1);
        move(w, x, e, on_line_x(w, e, nl, e->want_x), shift, 1);
        break;
    }
    case VK_HOME:
        move(w, x, e, ctrl ? 0 : e->ln[l].start, shift, 0);
        break;
    case VK_END:
        move(w, x, e, ctrl ? e->len : e->ln[l].start + e->ln[l].len, shift, 0);
        break;
    case VK_DELETE:
        if (x->style & ES_READONLY) break;
        if (shift) { copy(w, e); }               // Shift+Del: cut
        if (e->anchor != e->caret) replace(w, x, e, sel_lo(e), sel_hi(e), "", 0, 1);
        else if (!shift && e->caret < e->len) replace(w, x, e, e->caret, step(t, e->len, e->caret, 1), "", 0, 1);
        break;
    case VK_INSERT:
        if (ctrl) copy(w, e);                    // Ctrl+Ins: copy; Shift+Ins: paste
        else if (shift) paste(w, x, e);
        break;
    default:
        break;
    }
}

static void key_char(struct w16 *w, struct w16_wnd *x, struct edit *e, uint16_t ch) {
    const char *t = text(w, e);
    if (x->style & ES_READONLY) return;
    if (ch == '\b') {
        if (e->anchor != e->caret) replace(w, x, e, sel_lo(e), sel_hi(e), "", 0, 1);
        else if (e->caret > 0) replace(w, x, e, step(t, e->len, e->caret, -1), e->caret, "", 0, 1);
        return;
    }
    if (ch == '\r') {
        if (multi(x)) type_text(w, x, e, "\r\n", 2);
        return;
    }
    if (ch == '\t') {
        if (multi(x)) type_text(w, x, e, "\t", 1);
        return;
    }
    if (ch < 0x20 || ch == 0x7F || ch > 0xFF) return;   // Ctrl+letter is the program's, not text
    char c = (char)ch;
    type_text(w, x, e, &c, 1);
}

// --- the mouse ----------------------------------------------------------------------------

static int index_at_point(struct w16 *w, struct w16_wnd *x, struct edit *e, int px, int py) {
    int lh = line_h(e);
    int l = multi(x) ? e->top + (py < 0 ? -1 - (-py - 1) / lh : py / lh) : 0;
    l = clampi(l, 0, e->nlines - 1);
    const char *t = text(w, e);
    pthread_mutex_lock(&g_w16_text_lock);
    const struct ugfx_font *was = ugfx_set_font(efont(e));
    int left = line_left(x, e, t, l);
    ugfx_set_font(was);
    pthread_mutex_unlock(&g_w16_text_lock);
    return on_line_x(w, e, l, px - left);
}

static void set_capture(struct w16 *w, uint16_t hwnd) {
    pthread_mutex_lock(&w->lock);
    w->capture = hwnd;
    pthread_mutex_unlock(&w->lock);
}

// --- the window procedure ------------------------------------------------------------------

static uint32_t far_put(struct w16 *w, uint32_t far, const char *s, unsigned n) {
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, n + 1, 1);
    if (!p) return 0;
    memcpy(p, s, n);
    p[n] = 0;
    return n;
}

static char *far_get(struct w16 *w, uint32_t far) {
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

char *w16_edit_text(struct w16 *w, struct w16_wnd *x) {
    struct edit *e = ed(x);
    return e ? strndup(text(w, e), (size_t)e->len) : strdup("");
}

uint32_t w16_edit_proc(struct w16 *w, struct w16_wnd *x, uint16_t msg, uint16_t wp, uint32_t lp) {
    struct edit *e = ed(x);
    if (!e && msg != WM_NCCREATE && msg != WM_CREATE) return w16_def_window_proc(w, x, msg, wp, lp);
    switch (msg) { // dispatch-ok: a window procedure, bounded by the messages Windows' EDIT answered
    case WM_NCCREATE:
        if (!x->ctl && !(x->ctl = calloc(1, sizeof(struct edit)))) return 0;
        e = ed(x);
        e->want_x = -1;
        e->hfont = x->font;                      // its dialog's, when it is in one
        if (x->style & ES_PASSWORD) e->pw = '*';
        return 1;
    case WM_CREATE:
        if (!e) return 0xFFFFFFFFu;
        set_all(w, x, e, x->text ? x->text : "");
        return 0;
    case WM_SIZE:
        reflow(w, x, e);
        show_caret(w, x, e);
        return 0;
    case WM_PAINT:
        paint(w, x, e);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTCHARS | DLGC_HASSETSEL | DLGC_WANTARROWS |
               (multi(x) && (x->style & ES_WANTRETURN) ? DLGC_WANTALLKEYS : 0);
    case WM_SETFOCUS:
        e->focused = 1;
        caret_on(w, x, e);
        redraw(w, x, e);
        w16_notify(w, x, EN_SETFOCUS);
        return 0;
    case WM_KILLFOCUS:
        e->focused = 0;
        w16_kill_timer(w, x->hwnd, CARET_TIMER);
        redraw(w, x, e);
        w16_notify(w, x, EN_KILLFOCUS);
        return 0;
    case WM_TIMER:
        if (wp != CARET_TIMER) return 0;
        e->blink_off = !e->blink_off && e->focused;
        redraw(w, x, e);
        return 0;
    case WM_SETREDRAW:
        e->noredraw = !wp;
        if (wp) w16_invalidate(w, x, 0);
        return 0;
    case WM_ENABLE:
        redraw(w, x, e);
        return 0;
    case WM_KEYDOWN:
        key_down(w, x, e, wp);
        return 0;
    case WM_CHAR:
        key_char(w, x, e, wp);
        return 0;
    case WM_LBUTTONDOWN:
        w16_set_focus(w, x->hwnd);
        e->captured = 1;
        set_capture(w, x->hwnd);
        move(w, x, e, index_at_point(w, x, e, (int16_t)lp, (int16_t)(lp >> 16)), (wp & 0x0004) != 0, 0);
        return 0;
    case WM_LBUTTONDBLCLK: {
        const char *t = text(w, e);
        int i = e->caret, a = i, b = i;
        while (a > 0 && is_word(t[a - 1])) a--;
        while (b < e->len && is_word(t[b])) b++;
        e->anchor = a;
        move(w, x, e, b, 1, 0);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (e->captured) {
            int16_t px = (int16_t)lp, py = (int16_t)(lp >> 16);
            move(w, x, e, index_at_point(w, x, e, px, py), 1, 0);
        }
        return 0;
    case WM_LBUTTONUP:
        if (e->captured) {
            e->captured = 0;
            set_capture(w, 0);
        }
        return 0;
    case WM_VSCROLL:
    case WM_HSCROLL: {
        // Lines down, characters across; a thumb's position is a percentage.
        int bx, by, vis = visible_lines(x);
        w16_dlg_units(&bx, &by);
        int vert = msg == WM_VSCROLL, max = vert ? max_top(x, e) : max_xoff(w, x, e);
        // EM_GETTHUMB as the code: where the thumb is, 3.x's way of asking --
        // Notepad sets its own window's thumb from this after every scroll.
        if (wp == EM_GETTHUMB) return (uint32_t)thumb(vert ? e->top : e->xoff, max);
        int pos = vert ? e->top : e->xoff, line = vert ? 1 : bx;
        int page = vert ? (vis > 1 ? vis - 1 : 1) : (x->cw > 2 * bx ? x->cw - bx : bx);
        switch (wp) {
        case SB_LINEUP:   pos -= line; break;
        case SB_LINEDOWN: pos += line; break;
        case SB_PAGEUP:   pos -= page; break;
        case SB_PAGEDOWN: pos += page; break;
        case SB_THUMBPOSITION: case SB_THUMBTRACK: pos = (int)((long)(int16_t)lp * max / 100); break;
        case SB_TOP:      pos = 0; break;
        case SB_BOTTOM:   pos = max; break;
        default: return 0;
        }
        pos = clampi(pos, 0, max);
        if (vert) scroll_to(w, x, e, pos, e->xoff);
        else scroll_to(w, x, e, e->top, pos);
        return 0;
    }
    case WM_SETTEXT: {
        char *s = far_get(w, lp);
        if (!s) return 0;
        set_all(w, x, e, s);
        free(s);
        return 1;
    }
    case WM_GETTEXT: {
        if (!wp) return 0;
        unsigned n = (unsigned)e->len < wp - 1u ? (unsigned)e->len : wp - 1u;
        return far_put(w, lp, text(w, e), n);
    }
    case WM_GETTEXTLENGTH:
        return (uint32_t)e->len;
    case WM_CUT:
        copy(w, e);
        if (e->anchor != e->caret && !(x->style & ES_READONLY)) replace(w, x, e, sel_lo(e), sel_hi(e), "", 0, 1);
        return 0;
    case WM_COPY:
        copy(w, e);
        return 0;
    case WM_PASTE:
        paste(w, x, e);
        return 0;
    case WM_CLEAR:
        if (e->anchor != e->caret && !(x->style & ES_READONLY)) replace(w, x, e, sel_lo(e), sel_hi(e), "", 0, 1);
        return 0;
    case WM_UNDO:
    case EM_UNDO: {
        if (!e->can_undo || !e->undo) return 0;
        char *back = strdup(e->undo);
        int at = e->undo_at, ins = e->undo_ins;
        if (!back) return 0;
        replace(w, x, e, at, at + ins, back, (int)strlen(back), 1);   // its inverse is the next undo
        e->anchor = at;
        e->caret = at + (int)strlen(back);
        free(back);
        redraw(w, x, e);
        return 1;
    }
    case EM_CANUNDO:
        return (uint32_t)e->can_undo;
    case EM_EMPTYUNDOBUFFER:
        e->can_undo = 0;
        return 0;
    case EM_GETSEL:
        return (uint32_t)sel_hi(e) << 16 | (uint16_t)sel_lo(e);
    case EM_SETSEL: {
        int a = (int16_t)(lp & 0xFFFF), b = (int16_t)(lp >> 16);
        if (a == -1) { e->anchor = e->caret; redraw(w, x, e); return 0; }   // no selection
        if (b == -1 || b > e->len) b = e->len;
        if (a > e->len) a = e->len;
        e->anchor = clampi(a, 0, e->len);
        e->caret = clampi(b, 0, e->len);
        show_caret(w, x, e);
        redraw(w, x, e);
        return 1;
    }
    case EM_REPLACESEL: {
        char *s = far_get(w, lp);
        if (!s) return 0;
        type_text(w, x, e, s, (int)strlen(s));
        free(s);
        return 0;
    }
    case EM_GETMODIFY:
        return (uint32_t)e->modified;
    case EM_SETMODIFY:
        e->modified = wp != 0;
        return 0;
    case EM_GETLINECOUNT:
        return (uint32_t)e->nlines;
    case EM_LINEINDEX: {
        int l = (int16_t)wp == -1 ? line_of(e, e->caret) : (int)wp;
        return l >= 0 && l < e->nlines ? (uint32_t)e->ln[l].start : 0xFFFF;
    }
    case EM_LINEFROMCHAR:
        return (uint32_t)line_of(e, (int16_t)wp == -1 ? sel_lo(e) : clampi(wp, 0, e->len));
    case EM_LINELENGTH: {
        if ((int16_t)wp == -1) {                 // what of the selected lines is not selected
            int a = line_of(e, sel_lo(e)), b = line_of(e, sel_hi(e));
            return (uint32_t)((sel_lo(e) - e->ln[a].start) + (e->ln[b].start + e->ln[b].len - sel_hi(e)));
        }
        return (uint32_t)e->ln[line_of(e, clampi(wp, 0, e->len))].len;
    }
    case EM_GETLINE: {
        if (wp >= (unsigned)e->nlines) return 0;
        uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(lp >> 16), (uint16_t)lp, 2, 0);
        if (!p) return 0;
        unsigned max = (unsigned)(p[0] | p[1] << 8), n = (unsigned)e->ln[wp].len < max ? (unsigned)e->ln[wp].len : max;
        uint8_t *q = n ? cpu16_ptr(&w->cpu, (uint16_t)(lp >> 16), (uint16_t)lp, n, 1) : p;
        if (!q) return 0;
        memcpy(q, text(w, e) + e->ln[wp].start, n);   // no NUL: Windows' way
        return n;
    }
    case EM_LIMITTEXT:
        e->limit = wp;
        return 0;
    case EM_GETHANDLE:
        if (!e->hbuf && local(w, x)) reserve(w, x, e, e->len);
        return e->hbuf;
    case EM_SETHANDLE: {
        unsigned size;
        uint8_t *p = wp ? w16_local_ptr(w, wp, &size) : 0;
        if (!p) return 0;
        free(e->host);
        e->host = 0;
        e->cap = 0;
        e->hbuf = wp;
        unsigned n = 0;
        while (n < size && p[n]) n++;
        if (n == size) { n = size ? size - 1 : 0; p[n] = 0; }   // never past the block
        e->len = (int)n;
        e->anchor = e->caret = 0;
        e->top = 0;
        e->xoff = 0;
        e->modified = 0;
        e->can_undo = 0;
        reflow(w, x, e);
        show_caret(w, x, e);
        redraw(w, x, e);
        return 0;
    }
    case EM_GETFIRSTVISIBLELINE:
        return (uint32_t)e->top;
    case EM_GETTHUMB:
        return (uint32_t)thumb(e->top, max_top(x, e));
    case EM_LINESCROLL: {
        int bx, by;
        w16_dlg_units(&bx, &by);
        scroll_to(w, x, e, e->top + (int16_t)(lp & 0xFFFF), e->xoff + (int16_t)(lp >> 16) * bx);
        return 1;
    }
    case EM_SCROLL:
        return w16_edit_proc(w, x, WM_VSCROLL, wp, 0);
    case EM_SETREADONLY:
        pthread_mutex_lock(&w->lock);
        if (wp) x->style |= ES_READONLY;
        else x->style &= ~(uint32_t)ES_READONLY;
        pthread_mutex_unlock(&w->lock);
        return 1;
    case EM_SETPASSWORDCHAR:
        e->pw = (char)wp;
        reflow(w, x, e);
        redraw(w, x, e);
        return 0;
    case EM_GETPASSWORDCHAR:
        return (uint8_t)e->pw;
    case EM_FMTLINES:                            // soft breaks are never put in the text here
        return wp;
    case EM_SETTABSTOPS:
    case EM_SETWORDBREAK:
        return 1;
    case EM_GETRECT: {
        int16_t r[4] = { MARGIN, 0, (int16_t)(x->cw - MARGIN), (int16_t)x->ch };
        uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(lp >> 16), (uint16_t)lp, 8, 1);
        if (p) memcpy(p, r, 8);
        return 0;
    }
    case EM_SETFONT:
    case WM_SETFONT:                             // only the system font or a fixed one, as w16 has those
        e->hfont = wp;
        reflow(w, x, e);
        show_caret(w, x, e);
        if (lp || msg == EM_SETFONT) redraw(w, x, e);
        return 0;
    case WM_GETFONT:
        return e->hfont;
    case EM_SETRECT: case EM_SETRECTNP:
        return 0;
    case WM_NCDESTROY:
        // ITS TEXT GOES WITH IT, local block and all, as Windows' did:
        // Notepad's Word Wrap makes a new edit, hands it a copy and
        // destroys the old one, counting on this -- kept, two toggles
        // filled its 64 KiB heap with sample.txt.
        if (e->hbuf) w16_local_free(w, e->hbuf);
        if (e->captured) set_capture(w, 0);
        w16_kill_timer(w, x->hwnd, CARET_TIMER);
        free(e->host);
        free(e->ln);
        free(e->undo);
        free(x->ctl);
        x->ctl = 0;
        return 0;
    default:
        return w16_def_window_proc(w, x, msg, wp, lp);
    }
}
