// A Windows Help topic, drawn as a document -- see ui/uui_helpview.h.
//
// ONE LAYOUT PASS turns the document's paragraphs into a flat list of
// ITEMS (a word, a picture, a rule) in region space; drawing walks the
// list under a clip and hit-testing walks it again, so what is drawn and
// what is clickable cannot disagree. Two regions: the non-scrolling
// heading, then the scrolling body.
#include "ui/uui_helpview.h"
#include "ui/uwmf_draw.h"
#include "ui/uui_widget.h"
#include "ui/uui_ctxmenu.h"
#include "ui/uui_primitives.h"
#include "ui/utheme.h"
#include "lib/ucharset.h"
#include "keyboard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { IT_TEXT, IT_PIC, IT_RULE };
enum { UL_SOLID = 1, UL_DOTTED = 2, UL_STRIKE = 4, UL_DOUBLE = 8 };

struct uhv_item {
    uint8_t kind, ul, region, bits;
    int x, y, w, h;
    int font;
    uint32_t color;           // 0 = the theme's text colour
    int link;                 // a doc link, or -1
    int str;                  // arena offset of NUL-terminated UTF-8
    int pic;
    int para;                 // the document paragraph it came from
    int ord;                  // TEXT: its place in reading order
    int ly;                   // the top of the LINE it is on: items of one line share it, whatever their font
};

struct uhv_font {
    uint8_t family, bold, italic;
    int px;
    struct ugfx_font f;
    void *arena;
    int ok;
};

struct uhv_fonts {
    struct uhv_font *f;
    int n, cap;
    int generation;
};

#define PATH_ROOT "/usr/share/fonts/"

// --- fonts ----------------------------------------------------------------------------------------

static const struct ugfx_font *font_for(struct uui_helpview *v, int family, int bold, int italic, int px, int *index) {
    struct uhv_fonts *fs = v->fonts;
    for (int i = 0; i < fs->n; i++) {
        struct uhv_font *f = &fs->f[i];
        if (f->family == family && f->bold == bold && f->italic == italic && f->px == px) {
            if (index) *index = i;
            return f->ok ? &f->f : ugfx_font_session(bold ? UGFX_FONT_BOLD : UGFX_FONT_REGULAR);
        }
    }
    if (fs->n == fs->cap) {
        int nc = fs->cap ? fs->cap * 2 : 8;
        struct uhv_font *g = realloc(fs->f, (size_t)nc * sizeof *g);
        if (!g) return ugfx_font_session(UGFX_FONT_REGULAR);
        fs->f = g;
        fs->cap = nc;
    }
    struct uhv_font *f = &fs->f[fs->n];
    memset(f, 0, sizeof *f);
    f->family = (uint8_t)family;
    f->bold = (uint8_t)bold;
    f->italic = (uint8_t)italic;
    f->px = px;
    const char *base = family == UHLP_FAMILY_MODERN ? "dejavu-sans-mono" : family == UHLP_FAMILY_ROMAN ? "liberation-serif" : "liberation-sans";
    char path[96];
    // The file naming: <face>[-bold][-italic].ttf.
    snprintf(path, sizeof path, PATH_ROOT "%s%s%s.ttf", base, bold ? "-bold" : "", italic ? "-italic" : "");
    unsigned long need = ugfx_font_arena_size(px);
    f->arena = malloc(need);
    if (f->arena) {
        f->ok = ugfx_font_load(path, px, 0, &f->f, f->arena, need);   // the -bold file is already bold: emboldening it again smears
        if (!f->ok) { free(f->arena); f->arena = 0; }
    }
    if (index) *index = fs->n;
    fs->n++;
    return f->ok ? &f->f : ugfx_font_session(bold ? UGFX_FONT_BOLD : UGFX_FONT_REGULAR);
}

static const struct ugfx_font *font_at(const struct uui_helpview *v, int i) {
    struct uhv_font *f = &v->fonts->f[i];
    return f->ok ? &f->f : ugfx_font_session(f->bold ? UGFX_FONT_BOLD : UGFX_FONT_REGULAR);
}

static void fonts_free(struct uhv_fonts *fs) {
    if (!fs) return;
    for (int i = 0; i < fs->n; i++) free(fs->f[i].arena);
    free(fs->f);
    free(fs);
}

// --- init / free ----------------------------------------------------------------------------------

void uui_helpview_init(struct uui_helpview *v) {
    memset(v, 0, sizeof *v);
    uui_sbar_init(&v->bar);
    v->armed = v->hover = v->armed_hot = -1;
    v->ctx_link = -1;
    v->fonts = calloc(1, sizeof *v->fonts);
    v->owns_fonts = 1;
}

void uui_helpview_free(struct uui_helpview *v) {
    if (v->pop) { uui_helpview_free(v->pop); free(v->pop); v->pop = 0; }
    free(v->item);
    free(v->order);
    free(v->arena);
    if (v->owns_fonts) fonts_free(v->fonts);
    v->item = 0; v->arena = 0; v->fonts = 0; v->order = 0; v->norder = 0;
    v->nitem = v->item_cap = 0;
    v->arena_len = v->arena_cap = 0;
}

void uui_helpview_set_doc(struct uui_helpview *v, const struct uhlp *h, const struct uhlp_doc *d, int scroll) {
    v->help = h;
    v->doc = d;
    v->doc_serial++;
    v->scroll = scroll;
    v->armed = v->hover = v->armed_hot = -1;
    v->sel_on = v->selecting = 0;
    if (v->pop) uui_helpview_popup_close(v);
}

void uui_helpview_set_zoom(struct uui_helpview *v, int zoom) {
    v->zoom = zoom < UHV_ZOOM_MIN ? UHV_ZOOM_MIN : zoom > UHV_ZOOM_MAX ? UHV_ZOOM_MAX : zoom;
}

void uui_helpview_set_highlight(struct uui_helpview *v, const char *words) {
    snprintf(v->hl, sizeof v->hl, "%s", words ? words : "");
}

int uui_helpview_content_height(const struct uui_helpview *v) { return v->ns_h + v->sc_h; }

// --- the layout ---------------------------------------------------------------------------------------

struct atom {
    uint8_t kind;             // 0 word, 1 space, 2 tab, 3 picture
    uint8_t ul;
    uint8_t place;
    int w, asc, desc;
    int font;
    uint32_t color;
    int link;
    int str;
    int pic;
    int x;
    int para;
};

struct lay {
    struct uui_helpview *v;
    const struct uhlp *h;
    const struct uhlp_doc *d;
    int em;
    int region;
    int para;                 // the paragraph being laid out
    // floats: the y a left / right picture ends at, and its width
    int lfl_until, lfl_w, rfl_until, rfl_w;
    struct atom *atom; int natom, atom_cap;
    int oom;
};

// Half-points (144 to the inch) to pixels: 96 dpi when the interface face is its default 14 px -- a 10-point
// face is 13 px, as in WINHELP.EXE -- and in proportion to it when the face is changed.
static int to_px(const struct lay *L, int hp) {
    long n = (long)hp * L->em;
    return (int)(n >= 0 ? (n + 10) / 21 : -((-n + 10) / 21));
}

static struct uhv_item *add_item(struct uui_helpview *v, int kind) {
    if (v->nitem == v->item_cap) {
        int nc = v->item_cap ? v->item_cap * 2 : 256;
        struct uhv_item *g = realloc(v->item, (size_t)nc * sizeof *g);
        if (!g) return 0;
        v->item = g;
        v->item_cap = nc;
    }
    struct uhv_item *it = &v->item[v->nitem++];
    memset(it, 0, sizeof *it);
    it->kind = (uint8_t)kind;
    it->link = -1;
    it->pic = -1;
    return it;
}

static int arena_add(struct uui_helpview *v, const char *s, size_t n) {
    if (v->arena_len + n + 1 > v->arena_cap) {
        size_t nc = v->arena_cap ? v->arena_cap * 2 : 8192;
        while (nc < v->arena_len + n + 1) nc *= 2;
        char *g = realloc(v->arena, nc);
        if (!g) return -1;
        v->arena = g;
        v->arena_cap = nc;
    }
    int off = (int)v->arena_len;
    memcpy(v->arena + v->arena_len, s, n);
    v->arena[v->arena_len + n] = 0;
    v->arena_len += n + 1;
    return off;
}

// A font index (into the shared set) for the file's font `fi`.
static int resolve_font(struct lay *L, int fi, int *asc, int *desc) {
    const struct uhlp_font *hf = uhlp_font(L->h, fi);
    struct uui_helpview *v = L->v;
    int hp = hf ? hf->half_pts : 20;
    hp += v->zoom * 4;
    int px = to_px(L, hp);
    if (px < 8) px = 8;
    if (px > 96) px = 96;
    int bold = hf && (hf->flags & UHLP_FONT_BOLD) != 0, italic = hf && (hf->flags & UHLP_FONT_ITALIC) != 0;
    int fam = hf ? hf->family : UHLP_FAMILY_SWISS;
    if (hf && !strcmp(hf->face, "Courier")) fam = UHLP_FAMILY_MODERN;
    int idx = 0;
    const struct ugfx_font *f = font_for(v, fam, bold, italic, px, &idx);
    int lh = f->line_h > 0 ? f->line_h : px + px / 4;
    if (asc) *asc = lh * 4 / 5;
    if (desc) *desc = lh - lh * 4 / 5;
    return idx;
}

static int atom_add(struct lay *L, const struct atom *a) {
    if (L->natom == L->atom_cap) {
        int nc = L->atom_cap ? L->atom_cap * 2 : 64;
        struct atom *g = realloc(L->atom, (size_t)nc * sizeof *g);
        if (!g) { L->oom = 1; return 0; }
        L->atom = g;
        L->atom_cap = nc;
    }
    L->atom[L->natom] = *a;
    L->atom[L->natom++].para = L->para;
    return 1;
}

static uint32_t link_colour(struct uui_helpview *v) { (void)v; return UTHEME_ACCENT; }

struct pctx {
    int x0, w;                // the box this paragraph flows in (region space)
    int left, right, first;   // in pixels
    int line_x;               // pen x within the line, from the line's left edge
    int line_w;               // the line's width
    int line_off;             // the line's left offset (indent, a float)
    int lines;                // lines finished
    int y;                    // the line's top
    int align;
    int spacing;
    int ntab; const struct uhlp_tab *tab;
};

static void line_geometry(struct lay *L, struct pctx *P) {
    int off = P->left + (P->lines == 0 ? P->first : 0);
    if (off < 0) off = 0;
    int w = P->w - off - P->right;
    if (P->y < L->lfl_until) { off += L->lfl_w; w -= L->lfl_w; }
    if (P->y < L->rfl_until) w -= L->rfl_w;
    if (w < L->em) w = L->em;
    P->line_off = off;
    P->line_w = w;
}

// Places the buffered atoms as one line at P->y; advances P->y.
static void flush_line(struct lay *L, struct pctx *P, int force_empty_h) {
    struct uui_helpview *v = L->v;
    int asc = 0, desc = 0, used = 0;
    for (int i = 0; i < L->natom; i++) {
        struct atom *a = &L->atom[i];
        if (a->asc > asc) asc = a->asc;
        if (a->desc > desc) desc = a->desc;
        if (a->kind != 3 || a->place == 0) { int e = a->x + a->w; if (e > used) used = e; }
    }
    if (!L->natom) { asc = force_empty_h * 4 / 5; desc = force_empty_h - asc; }
    int lh = asc + desc;
    if (P->spacing > lh) { asc += P->spacing - lh; lh = P->spacing; }
    int shift = 0;
    if (P->align == 1) shift = P->line_w - used;
    else if (P->align == 2) shift = (P->line_w - used) / 2;
    if (shift < 0) shift = 0;
    for (int i = 0; i < L->natom; i++) {
        struct atom *a = &L->atom[i];
        if (a->kind == 2) continue;
        struct uhv_item *it = add_item(v, a->kind == 3 ? IT_PIC : IT_TEXT);
        if (!it) { L->oom = 1; return; }
        it->region = (uint8_t)L->region;
        it->x = P->x0 + P->line_off + shift + a->x;
        it->w = a->w;
        it->font = a->font;
        it->color = a->color;
        it->link = a->link;
        it->ul = a->ul;
        it->str = a->str;
        it->pic = a->pic;
        it->para = a->para;
        it->ly = P->y;
        if (a->kind == 3) {
            it->h = a->asc + a->desc;
            it->y = P->y + asc - a->asc;
        } else {
            it->h = a->asc + a->desc;
            it->y = P->y + asc - a->asc;
        }
    }
    P->y += lh;
    P->lines++;
    L->natom = 0;
    P->line_x = 0;
    line_geometry(L, P);
}

// Where the next tab stop after `x` (from the box's left) is.
static int next_tab(struct lay *L, struct pctx *P, int x_from_box) {
    int best = -1;
    for (int i = 0; i < P->ntab; i++) {
        int t = to_px(L, P->tab[i].pos);
        if (t > x_from_box && (best < 0 || t < best)) best = t;
    }
    if (best >= 0) return best;
    int step = to_px(L, 72);
    if (step < 8) step = 8;
    return (x_from_box / step + 1) * step;
}

static void add_text_atoms(struct lay *L, struct pctx *P, const struct uhlp_run *r, int font, int asc, int desc) {
    // font-ok: measuring inside the layout, which helpview_draw, draw_popup and op_describe bracket.
    struct uui_helpview *v = L->v;
    const struct uhlp_font *hf = uhlp_font(L->h, r->font);
    uint32_t color = hf ? hf->rgb : 0;
    uint8_t ul = 0;
    if (hf && (hf->flags & UHLP_FONT_UNDERLINE)) ul |= UL_SOLID;
    if (hf && (hf->flags & UHLP_FONT_STRIKE)) ul |= UL_STRIKE;
    if (hf && (hf->flags & UHLP_FONT_DOUBLE)) ul |= UL_DOUBLE;
    int link = r->link;
    if (link >= 0 && link < L->d->nlink) {
        const struct uhlp_link *lk = &L->d->link[link];
        if (lk->colour) color = link_colour(v);
        // a hidden hotspot (no colour) is drawn as plain text, a pop-up's as well -- WINHELP.EXE's contents rows
        if (lk->colour) ul |= lk->kind == UHLP_LINK_POPUP ? UL_DOTTED : UL_SOLID;
    }
    if (color == 0) color = 0;
    const char *src = L->d->pool + r->text;
    size_t n = (size_t)r->len;
    char *u = malloc(n * 3 + 1);
    if (!u) { L->oom = 1; return; }
    long un = ucharset_cp1252_to_utf8(u, n * 3 + 1, src, n, '?');
    if (un < 0) un = 0;
    u[un] = 0;
    ugfx_set_font(font_at(v, font));
    size_t i = 0;
    while (i < (size_t)un) {
        size_t j = i;
        int is_space = u[i] == ' ';
        while (j < (size_t)un && (u[j] == ' ') == is_space) j++;
        // a word wider than the line breaks hard; a space run is one atom
        while (i < j) {
            size_t take = j - i;
            int w = ugfx_text_width_n(u + i, (int)take);
            if (!is_space) {
                int room = P->line_w - P->line_x;
                if (w > room && L->natom > 0) {
                    flush_line(L, P, asc + desc);
                    room = P->line_w - P->line_x;
                }
                if (w > room) {                           // longer than a whole line: cut it
                    int fit = ugfx_text_fit_chars(u + i, room > 8 ? room : 8);
                    if (fit < 1) fit = 1;
                    if ((size_t)fit < take) {
                        take = (size_t)fit;
                        w = ugfx_text_width_n(u + i, (int)take);
                    }
                }
            } else if (L->natom == 0 && P->lines > 0) {
                i = j;                                     // spaces that wrapped are not drawn
                break;
            }
            char save = u[i + take];
            u[i + take] = 0;
            int off = arena_add(v, u + i, take);
            u[i + take] = save;
            if (off < 0) { L->oom = 1; free(u); return; }
            struct atom a = { .kind = is_space ? 1 : 0, .ul = ul, .w = w, .asc = asc, .desc = desc, .font = font,
                              .color = color, .link = link, .str = off, .x = P->line_x };
            if (!atom_add(L, &a)) { free(u); return; }
            P->line_x += w;
            i += take;
            if (!is_space && i < j) flush_line(L, P, asc + desc);   // the rest of a cut word
        }
        i = j;
    }
    free(u);
}

// One paragraph, flowed in a box at (x0, *y) `w` wide. Advances *y.
static void layout_para(struct lay *L, const struct uhlp_para *pa, int x0, int w, int *y) {
    // font-ok: inside the layout, which helpview_draw, draw_popup and op_describe bracket.
    L->para = (int)(pa - L->d->para);
    struct pctx P;
    memset(&P, 0, sizeof P);
    P.x0 = x0;
    P.w = w;
    P.left = to_px(L, pa->left);
    P.right = to_px(L, pa->right);
    P.first = to_px(L, pa->first);
    P.align = pa->align;
    P.spacing = pa->spacing > 0 ? to_px(L, pa->spacing) : 0;
    P.ntab = pa->ntab;
    P.tab = pa->ntab ? L->d->tab + pa->tab0 : 0;
    *y += to_px(L, pa->before);
    P.y = *y;
    int top = *y;
    L->natom = 0;
    line_geometry(L, &P);
    int first_font = 0, fasc = 0, fdesc = 0;
    if (pa->nrun > 0) first_font = L->d->run[pa->run0].font;
    int fidx = resolve_font(L, first_font, &fasc, &fdesc);
    (void)fidx;
    for (int i = 0; i < pa->nrun && !L->oom; i++) {
        const struct uhlp_run *r = &L->d->run[pa->run0 + i];
        int asc, desc;
        int fidx2 = resolve_font(L, r->font, &asc, &desc);
        switch (r->kind) {
        case UHLP_RUN_TEXT: add_text_atoms(L, &P, r, fidx2, asc, desc); break;
        case UHLP_RUN_LINEBREAK: flush_line(L, &P, asc + desc); break;
        case UHLP_RUN_TAB: {
            int from_box = P.line_off + P.line_x;
            int stop = next_tab(L, &P, from_box);
            int wdt = stop - from_box;
            struct atom a = { .kind = 2, .w = wdt, .asc = asc, .desc = desc, .font = fidx2, .x = P.line_x };
            if (wdt > 0 && atom_add(L, &a)) P.line_x += wdt;
            break;
        }
        case UHLP_RUN_NBSP: case UHLP_RUN_HYPHEN: {
            const char *s = r->kind == UHLP_RUN_HYPHEN ? "-" : " ";
            ugfx_set_font(font_at(L->v, fidx2));
            int wdt = ugfx_text_width(s);
            int off = arena_add(L->v, s, 1);
            struct atom a = { .kind = r->kind == UHLP_RUN_HYPHEN ? 0 : 1, .w = wdt, .asc = asc, .desc = desc, .font = fidx2,
                              .link = r->link, .str = off, .x = P.line_x };
            if (off >= 0 && atom_add(L, &a)) P.line_x += wdt;
            break;
        }
        case UHLP_RUN_PICTURE: {
            if (r->pic < 0 || r->pic >= L->d->npic) break;
            const struct uhlp_pic *pic = &L->d->pic[r->pic];
            int pw = pic->w > 0 ? pic->w : 16, ph = pic->h > 0 ? pic->h : 16;
            // a metafile's size is HIMETRIC (0.01 mm): at 96 dpi, as WINHELP.EXE drew it -- a DIB is pixels, unscaled too
            if (pic->kind == UHLP_PIC_WMF) {
                pw = (int)((long)pic->w * 96 / 2540);
                ph = (int)((long)pic->h * 96 / 2540);
                if (pw < 1) pw = 1;
                if (ph < 1) ph = 1;
            }
            if (r->place == 0) {
                if (P.line_x + pw > P.line_w && L->natom > 0) flush_line(L, &P, asc + desc);
                struct atom a = { .kind = 3, .w = pw, .asc = ph, .desc = 0, .pic = r->pic, .link = r->link, .x = P.line_x,
                                  .font = fidx2 };
                if (atom_add(L, &a)) P.line_x += pw;
            } else {
                // a floated picture: at the line's start (left) or end (right), the text wraps beside it
                if (L->natom > 0) flush_line(L, &P, asc + desc);
                struct uhv_item *it = add_item(L->v, IT_PIC);
                if (!it) { L->oom = 1; break; }
                it->region = (uint8_t)L->region;
                it->pic = r->pic;
                it->link = r->link;
                it->w = pw;
                it->h = ph;
                it->y = P.y;
                if (r->place == 1) {
                    it->x = x0 + P.left;
                    L->lfl_until = P.y + ph + 2;
                    L->lfl_w = pw + 6;
                } else {
                    it->x = x0 + w - P.right - pw;
                    L->rfl_until = P.y + ph + 2;
                    L->rfl_w = pw + 6;
                }
                line_geometry(L, &P);
            }
            break;
        }
        default: break;
        }
    }
    if (L->natom > 0 || pa->nrun == 0 || P.lines == 0) flush_line(L, &P, fasc + fdesc);
    *y = P.y + to_px(L, pa->after);
    // a picture floated past the text still takes the room
    if (L->lfl_until > *y && L->lfl_until - 2 > *y) *y = L->lfl_until - 2;
    if (L->rfl_until > *y && L->rfl_until - 2 > *y) *y = L->rfl_until - 2;
    // the paragraph's border
    if (pa->border) {
        struct uhv_item *it = add_item(L->v, IT_RULE);
        if (it) {
            it->region = (uint8_t)L->region;
            it->bits = pa->border;
            // from the first line's start when it hangs out past the rest (a tabbed table's header row),
            // as WINHELP.EXE draws it; one pixel, two for the thick style -- border_w is no pen width
            int from = P.first < 0 ? P.left + P.first : P.left;
            it->x = x0 + from - 3;
            it->w = w - from - P.right + 6;
            it->y = top - 2;
            it->h = P.y - top + 4;
            it->font = pa->border & 0x20 ? 2 : 1;
        }
    }
}

// The paragraphs of one table, row by row.
static int layout_table(struct lay *L, int first, int total, int x0, int w, int *y) {
    const struct uhlp_doc *d = L->d;
    int t = d->para[first].table;
    int end = first;
    while (end < total && d->para[end].table == t && d->para[end].scrolls == d->para[first].scrolls) end++;
    const struct uhlp_table *tb = &d->table[t];
    int ncol = tb->ncol > 0 ? tb->ncol : 1;
    int *cx = calloc((size_t)ncol, sizeof *cx), *cw = calloc((size_t)ncol, sizeof *cw);
    if (!cx || !cw) { free(cx); free(cw); L->oom = 1; return end; }
    int pos = 0;
    for (int c = 0; c < ncol; c++) {
        const struct uhlp_column *col = &d->col[tb->col0 + c];
        int gap = to_px(L, col->gap), wd = to_px(L, col->width);
        cx[c] = pos + gap / 2;
        cw[c] = wd > 4 ? wd : 4;
        pos += wd + gap;
    }
    // squeeze to the box if the table is wider than it
    if (pos > w && pos > 0) {
        for (int c = 0; c < ncol; c++) { cx[c] = cx[c] * w / pos; cw[c] = cw[c] * w / pos > 4 ? cw[c] * w / pos : 4; }
    }
    int p = first;
    while (p < end && !L->oom) {
        int row = d->para[p].row;
        int row_end = p;
        while (row_end < end && d->para[row_end].row == row) row_end++;
        int row_top = *y, row_bot = *y;
        for (int c = 0; c < ncol; c++) {
            int cy = row_top;
            L->lfl_until = L->rfl_until = 0;
            for (int q = p; q < row_end; q++)
                // A paragraph with no runs in a cell is a spacing record with nothing to show: WINHELP gives it no
                // height, and one per cell made every row of a real table a blank line taller and left the
                // first cell's text a line below its neighbour's.
                if (d->para[q].column == c && d->para[q].nrun > 0) layout_para(L, &d->para[q], x0 + cx[c], cw[c], &cy);
            if (cy > row_bot) row_bot = cy;
        }
        *y = row_bot;
        p = row_end;
    }
    free(cx);
    free(cw);
    return end;
}

// --- reading order, for the selection ----------------------------------------------------------------

static const struct uui_helpview *g_ord_view;

static int ord_cmp(const void *pa, const void *pb) {
    const struct uhv_item *a = &g_ord_view->item[*(const int *)pa], *b = &g_ord_view->item[*(const int *)pb];
    if (a->region != b->region) return (int)a->region - (int)b->region;
    if (a->ly != b->ly) return a->ly - b->ly;
    return a->x - b->x;
}

// The text items sorted by (region, line, x): the order a reader meets them in, which is
// not the order the layout made them in for a table (a column at a time).
static void build_order(struct uui_helpview *v) {
    free(v->order);
    v->order = NULL;
    v->norder = 0;
    v->sel_on = 0;                                  // a new layout is a new text
    int n = 0;
    for (int i = 0; i < v->nitem; i++) if (v->item[i].kind == IT_TEXT) n++;
    v->order = malloc((size_t)(n ? n : 1) * sizeof *v->order);
    if (!v->order) return;
    for (int i = 0; i < v->nitem; i++) if (v->item[i].kind == IT_TEXT) v->order[v->norder++] = i;
    g_ord_view = v;
    qsort(v->order, (size_t)v->norder, sizeof *v->order, ord_cmp);
    for (int k = 0; k < v->norder; k++) v->item[v->order[k]].ord = k;
}

static void layout_all(struct uui_helpview *v, int width, int em) {
    struct lay L;
    memset(&L, 0, sizeof L);
    L.v = v;
    L.h = v->help;
    L.d = v->doc;
    L.em = em;
    v->nitem = 0;
    v->arena_len = 0;
    v->ns_h = v->sc_h = 0;
    if (!L.d || !L.h) return;
    int pad = em / 2 + 2;
    int ys[2] = { 0, 0 };
    // the topic's first paragraph brings its own space above (WINHELP.EXE adds none); a pad only without it
    for (int p = 0; p < L.d->npara; p++)
        if (L.d->para[p].scrolls) { ys[1] = L.d->para[p].before ? 0 : pad / 2; break; }
    int total = L.d->npara;
    for (int p = 0; p < total && !L.oom;) {
        const struct uhlp_para *pa = &L.d->para[p];
        int region = pa->scrolls ? 1 : 0;
        L.region = region;
        L.lfl_until = L.rfl_until = 0;
        if (pa->table >= 0) p = layout_table(&L, p, total, pad, width - 2 * pad, &ys[region]);
        else { layout_para(&L, pa, pad, width - 2 * pad, &ys[region]); p++; }
    }
    free(L.atom);
    v->ns_h = ys[0] ? ys[0] + 4 : 0;
    v->sc_h = ys[1] + pad / 2 + 4;
    build_order(v);
}

// --- measuring against the current size --------------------------------------------------------------------

static int em_now(void) {
    int em = ugfx_font_session_px();
    if (em < 8) em = ugfx_char_h() * 5 / 6;
    return em < 8 ? 12 : em;
}

static void ensure_layout(struct uui_helpview *v) {
    int em = em_now();
    int gen = (int)ugfx_font_generation();
    if (v->fonts->generation != gen) {                 // the session font changed: faces are stale
        for (int i = 0; i < v->fonts->n; i++) free(v->fonts->f[i].arena);
        v->fonts->n = 0;
        v->fonts->generation = gen;
        v->m_doc = 0;
    }
    int barw = uui_sbar_width();
    for (int pass = 0; pass < 2; pass++) {
        int bar = pass == 0 ? v->bar_on : !v->bar_on;
        int width = v->w - (bar ? barw : 0);
        if (v->m_doc != v->doc || v->m_serial != v->doc_serial || v->m_w != width || v->m_zoom != v->zoom ||
            v->m_em != em || v->m_bar != bar) {
            layout_all(v, width, em);
            v->m_doc = v->doc;
            v->m_serial = v->doc_serial;
            v->m_w = width;
            v->m_zoom = v->zoom;
            v->m_em = em;
            v->m_bar = bar;
        }
        // is the bar needed for this height?
        int vp = v->h - v->ns_h;
        int need = v->sc_h > vp;
        if (need == bar) { v->bar_on = bar; break; }
        v->bar_on = need;
        v->m_bar = -1;                                  // lay out again at the other width
    }
}

// --- drawing -------------------------------------------------------------------------------------------------------

static int region_top(const struct uui_helpview *v, int region) {
    return region == 0 ? v->y : v->y + v->ns_h - v->scroll;
}

static void draw_rule(struct ugfx_surface *s, const struct uhv_item *it, int ox, int oy) {
    uint32_t c = UTHEME_TEXT;
    int x = it->x + ox, y = it->y + oy, w = it->w, h = it->h;
    int th = it->font > 1 ? 2 : 1;
    if (it->bits & 0x01) {
        ugfx_fill_rect(s, x, y, w, th, c); ugfx_fill_rect(s, x, y + h - th, w, th, c);
        ugfx_fill_rect(s, x, y, th, h, c); ugfx_fill_rect(s, x + w - th, y, th, h, c);
        return;
    }
    if (it->bits & 0x02) ugfx_fill_rect(s, x, y, w, th, c);
    if (it->bits & 0x08) ugfx_fill_rect(s, x, y + h - th, w, th, c);
    if (it->bits & 0x04) ugfx_fill_rect(s, x, y, th, h, c);
    if (it->bits & 0x10) ugfx_fill_rect(s, x + w - th, y, th, h, c);
}

static void draw_wmf_placeholder(struct ugfx_surface *s, int x, int y, int w, int h) {
    ugfx_draw_rect(s, x, y, w, h, UTHEME_BORDER);
    ugfx_draw_line(s, x, y, x + w - 1, y + h - 1, UTHEME_BORDER, GEOM_ALIASED);
    ugfx_draw_line(s, x + w - 1, y, x, y + h - 1, UTHEME_BORDER, GEOM_ALIASED);
}

// A metafile's font, from the faces the topic's text uses: its family guessed from the 3.1 face name.
static const struct ugfx_font *wmf_font(void *ctx, const char *face, int px, int bold, int italic) {
    int fam = !strncasecmp(face, "Courier", 7) ? UHLP_FAMILY_MODERN
            : !strncasecmp(face, "Tms", 3) || !strncasecmp(face, "Times", 5) ? UHLP_FAMILY_ROMAN : UHLP_FAMILY_SWISS;
    return font_for((struct uui_helpview *)ctx, fam, bold, italic, px, 0);
}

// The main window's two backgrounds, when the file names them (|SYSTEM's window record "main", or one
// with no name): RASPHONE.HLP's grey band at the top. White otherwise, and always in a pop-up.
static void region_colours(const struct uui_helpview *v, uint32_t *sr, uint32_t *nsr) {
    *sr = *nsr = UTHEME_WHITE;
    if (!v->help || v->is_popup) return;
    for (int i = 0; i < uhlp_nwindows(v->help); i++) {
        const struct uhlp_window *w = uhlp_window(v->help, i);
        if (w->name[0] && strcasecmp(w->name, "main")) continue;
        if (w->sr_rgb != 0xFFFFFFFFu) *sr = 0xFF000000u | w->sr_rgb;
        if (w->nsr_rgb != 0xFFFFFFFFu) *nsr = 0xFF000000u | w->nsr_rgb;
        return;
    }
}

static int hl_match(const char *hl, const char *text, int *at, int *len) {
    // the first case-insensitive occurrence of any highlight word in `text`
    const char *p = hl;
    while (*p) {
        while (*p == ' ') p++;
        int wn = 0;
        while (p[wn] && p[wn] != ' ') wn++;
        if (!wn) break;
        for (int i = 0; text[i]; i++) {
            int k = 0;
            while (k < wn && text[i + k] && (text[i + k] | 0x20) == (p[k] | 0x20)) k++;
            if (k == wn) { *at = i; *len = wn; return 1; }
        }
        p += wn;
    }
    return 0;
}

static void draw_items(struct ugfx_surface *s, struct uui_helpview *v, int region, int clip_x, int clip_y, int clip_w, int clip_h) {
    // font-ok: every caller (helpview_draw, draw_popup) puts back the font it found -- the leak this check
    // exists for was HERE, before they did.
    ugfx_set_clip_rect(s, clip_x, clip_y, clip_w, clip_h);
    int oy = region_top(v, region), ox = v->x;
    for (int i = 0; i < v->nitem; i++) {
        const struct uhv_item *it = &v->item[i];
        if (it->region != region) continue;
        int ix = it->x + ox, iy = it->y + oy;
        if (iy + it->h < clip_y || iy > clip_y + clip_h) continue;
        switch (it->kind) {
        case IT_RULE: draw_rule(s, it, ox, oy); break;
        case IT_PIC: {
            const struct uhlp_pic *pic = &v->doc->pic[it->pic];
            if (pic->kind == UHLP_PIC_DIB && pic->argb) ugfx_blit_alpha(s, ix, iy, pic->w, pic->h, pic->argb, pic->w);
            else {
                // the session face first: wmf_font may grow the face table, and the current font must not
                // point into it then
                ugfx_set_font(ugfx_font_session(UGFX_FONT_REGULAR));
                if (!pic->wmf || uwmf_draw(s, pic->wmf, pic->wmf_len, ix, iy, it->w, it->h, wmf_font, v) < 0)
                    draw_wmf_placeholder(s, ix, iy, it->w, it->h);
            }
            if (it->link >= 0 && it->link == v->hover) ugfx_draw_rect(s, ix, iy, it->w, it->h, UTHEME_ACCENT);
            for (int k = 0; k < pic->nhot; k++) {
                const struct uhlp_hot *hot = &v->doc->hot[pic->hot0 + k];
                if (v->hover == hot->link && v->hover >= 0)
                    ugfx_draw_rect(s, ix + hot->x, iy + hot->y, hot->w, hot->h, UTHEME_ACCENT);
            }
            break;
        }
        default: {
            const char *txt = v->arena + it->str;
            const struct ugfx_font *f = font_at(v, it->font);
            ugfx_set_font(f);
            uint32_t col = it->color ? 0xFF000000u | it->color : UTHEME_TEXT;
            if (it->link >= 0 && it->link == v->hover) col = ugfx_blend(col, UTHEME_TEXT, 90);
            int tw = ugfx_text_width(txt);
            if (v->sel_on) {
                int so = v->sel_a_ord, sc = v->sel_a_ch, eo = v->sel_f_ord, ec = v->sel_f_ch;
                if (so > eo || (so == eo && sc > ec)) { int t = so; so = eo; eo = t; t = sc; sc = ec; ec = t; }
                if (it->ord >= so && it->ord <= eo) {
                    int a = it->ord == so ? sc : 0, b = it->ord == eo ? ec : (int)strlen(txt);
                    int x1 = ix + ugfx_text_width_n(txt, a), x2 = ix + ugfx_text_width_n(txt, b);
                    if (x2 > x1) ugfx_fill_rect(s, x1, iy, x2 - x1, it->h, UTHEME_SELECTION);
                }
            }
            if (v->hl[0]) {
                int at, len;
                if (hl_match(v->hl, txt, &at, &len)) {
                    int x1 = ix + ugfx_text_width_n(txt, at), x2 = ix + ugfx_text_width_n(txt, at + len);
                    ugfx_fill_rect(s, x1, iy, x2 - x1, it->h, UTHEME_SELECTION);
                }
            }
            ugfx_draw_string(s, ix, iy, txt, col, UGFX_TRANSPARENT);
            int base = iy + it->h * 4 / 5;
            if (it->ul & UL_SOLID) ugfx_fill_rect(s, ix, base + 2, tw, 1, col);
            if (it->ul & UL_DOUBLE) { ugfx_fill_rect(s, ix, base + 2, tw, 1, col); ugfx_fill_rect(s, ix, base + 4, tw, 1, col); }
            if (it->ul & UL_DOTTED) for (int dx = 0; dx < tw; dx += 2) ugfx_put_pixel(s, ix + dx, base + 2, col);
            if (it->ul & UL_STRIKE) ugfx_fill_rect(s, ix, iy + it->h / 2, tw, 1, col);
            break;
        }
        }
    }
    ugfx_clear_clip_rect(s);
}

static void clamp_scroll(struct uui_helpview *v) {
    int vp = v->h - v->ns_h;
    int max = v->sc_h - vp;
    if (max < 0) max = 0;
    if (v->scroll > max) v->scroll = max;
    if (v->scroll < 0) v->scroll = 0;
}

static void draw_popup(struct ugfx_surface *s, struct uui_helpview *v);

static void helpview_draw(struct ugfx_surface *s, struct uui_helpview *v) {
    uint32_t sr, nsr;
    region_colours(v, &sr, &nsr);
    ugfx_fill_rect(s, v->x, v->y, v->w, v->h, sr);
    if (!v->doc) return;
    // THE FONT SELECTED IS THE PROCESS'S, not this widget's: every widget drawn after it uses
    // whatever it leaves, so a zoomed face here made the toolbar and the tree zoom too.
    const struct ugfx_font *saved = ugfx_font_current();
    ensure_layout(v);
    clamp_scroll(v);
    int barw = v->bar_on ? uui_sbar_width() : 0;
    int vp = v->h - v->ns_h;
    uui_sbar_place(&v->bar, v->x + v->w - barw, v->y + v->ns_h, vp);
    uui_sbar_set(&v->bar, v->sc_h, vp, v->scroll);
    v->bar.step = em_now() * 2;
    if (v->ns_h) {
        if (nsr != sr) ugfx_fill_rect(s, v->x, v->y, v->w, v->ns_h, nsr);
        draw_items(s, v, 0, v->x, v->y, v->w, v->ns_h);
        ugfx_fill_rect(s, v->x, v->y + v->ns_h - 1, v->w, 1, UTHEME_SEPARATOR);
    }
    draw_items(s, v, 1, v->x, v->y + v->ns_h, v->w - barw, vp);
    ugfx_set_font(saved);
    if (v->bar_on) uui_sbar_draw(&v->bar, s, UTHEME_WHITE, UTHEME_TEXT);
}

// --- hit testing ------------------------------------------------------------------------------------------------------------

// The doc link under (cx, cy), and the rect to anchor a pop-up to. -1 for none.
static int link_at(struct uui_helpview *v, int cx, int cy, int *rx, int *ry, int *rw, int *rh) {
    if (!v->doc || cx < v->x || cx >= v->x + v->w - (v->bar_on ? uui_sbar_width() : 0)) return -1;
    for (int region = 0; region < 2; region++) {
        int top = region == 0 ? v->y : v->y + v->ns_h, bot = region == 0 ? v->y + v->ns_h : v->y + v->h;
        if (cy < top || cy >= bot) continue;
        int oy = region_top(v, region), ox = v->x;
        for (int i = 0; i < v->nitem; i++) {
            const struct uhv_item *it = &v->item[i];
            if (it->region != region) continue;
            int ix = it->x + ox, iy = it->y + oy;
            if (it->kind == IT_PIC) {
                const struct uhlp_pic *pic = &v->doc->pic[it->pic];
                for (int k = 0; k < pic->nhot; k++) {
                    const struct uhlp_hot *hot = &v->doc->hot[pic->hot0 + k];
                    if (uui_hit(ix + hot->x, iy + hot->y, hot->w, hot->h, cx, cy)) {
                        if (rx) { *rx = ix + hot->x; *ry = iy + hot->y; *rw = hot->w; *rh = hot->h; }
                        return hot->link;
                    }
                }
            }
            if (it->link >= 0 && it->kind != IT_RULE && uui_hit(ix, iy, it->w, it->h, cx, cy)) {
                if (rx) { *rx = ix; *ry = iy; *rw = it->w; *rh = it->h; }
                return it->link;
            }
        }
    }
    return -1;
}

static void take_link(struct uui_helpview *v, int link, int ax, int ay, int aw, int ah) {
    const struct uhlp_link *lk = &v->doc->link[link];
    memset(&v->taken, 0, sizeof v->taken);
    v->taken.kind = lk->kind;
    v->taken.hash = lk->hash;
    v->taken.by_offset = lk->by_offset;
    v->taken.window = lk->window;
    if (lk->file >= 0) snprintf(v->taken.file, sizeof v->taken.file, "%s", uhlp_doc_str(v->doc, lk->file));
    if (lk->macro >= 0) snprintf(v->taken.macro, sizeof v->taken.macro, "%s", uhlp_doc_str(v->doc, lk->macro));
    v->taken.ax = ax; v->taken.ay = ay; v->taken.aw = aw; v->taken.ah = ah;
    v->have_taken = 1;
}

int uui_helpview_take_link(struct uui_helpview *v, struct uui_helpview_link *out) {
    if (v->pop && v->pop->have_taken) {                 // a link in the pop-up: it closes, and the app hears it
        *out = v->pop->taken;
        v->pop->have_taken = 0;
        uui_helpview_popup_close(v);
        return 1;
    }
    if (!v->have_taken) return 0;
    *out = v->taken;
    v->have_taken = 0;
    return 1;
}

// --- the selection ---------------------------------------------------------------------------------------------------------------

// The text item nearest (cx, cy) and the byte under the pointer in it. 0 when there is no text.
static int pos_at(struct uui_helpview *v, int cx, int cy, int *ord, int *ch) {
    if (!v->norder) return 0;
    const struct ugfx_font *saved = ugfx_font_current();
    long best = -1;
    int bo = 0, bch = 0;
    for (int k = 0; k < v->norder; k++) {
        const struct uhv_item *it = &v->item[v->order[k]];
        int ix = it->x + v->x, iy = it->y + region_top(v, it->region);
        int dy = cy < iy ? iy - cy : cy > iy + it->h - 1 ? cy - (iy + it->h - 1) : 0;
        int dx = cx < ix ? ix - cx : cx > ix + it->w ? cx - (ix + it->w) : 0;
        long score = (long)dy * 100000 + dx;
        if (best >= 0 && score >= best) continue;
        best = score;
        bo = k;
        const char *txt = v->arena + it->str;
        if (dx == 0) {
            ugfx_set_font(font_at(v, it->font));
            bch = ugfx_text_index_at_x(txt, cx - ix);
        } else {
            bch = cx < ix ? 0 : (int)strlen(txt);
        }
    }
    ugfx_set_font(saved);
    *ord = bo;
    *ch = bch;
    return 1;
}

static void sel_range(const struct uui_helpview *v, int *so, int *sc, int *eo, int *ec) {
    *so = v->sel_a_ord; *sc = v->sel_a_ch; *eo = v->sel_f_ord; *ec = v->sel_f_ch;
    if (*so > *eo || (*so == *eo && *sc > *ec)) { int t = *so; *so = *eo; *eo = t; t = *sc; *sc = *ec; *ec = t; }
}

int uui_helpview_has_selection(const struct uui_helpview *v) {
    if (!v->sel_on) return 0;
    return v->sel_a_ord != v->sel_f_ord || v->sel_a_ch != v->sel_f_ch;
}

void uui_helpview_clear_selection(struct uui_helpview *v) { v->sel_on = v->selecting = 0; }

void uui_helpview_select_all(struct uui_helpview *v) {
    if (!v->norder) return;
    const struct uhv_item *last = &v->item[v->order[v->norder - 1]];
    v->sel_a_ord = 0; v->sel_a_ch = 0;
    v->sel_f_ord = v->norder - 1; v->sel_f_ch = (int)strlen(v->arena + last->str);
    v->sel_on = 1;
}

char *uui_helpview_selection_text(const struct uui_helpview *v) {
    if (!uui_helpview_has_selection(v)) return NULL;
    int so, sc, eo, ec;
    sel_range(v, &so, &sc, &eo, &ec);
    size_t cap = 256, n = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    for (int k = so; k <= eo && k < v->norder; k++) {
        const struct uhv_item *it = &v->item[v->order[k]];
        const char *txt = v->arena + it->str;
        int a = k == so ? sc : 0, b = k == eo ? ec : (int)strlen(txt);
        if (b < a) b = a;
        size_t need = (size_t)(b - a) + 3;
        if (n + need > cap) {
            while (n + need > cap) cap *= 2;
            char *g = realloc(out, cap);
            if (!g) { free(out); return NULL; }
            out = g;
        }
        if (k > so) {                                       // between two items: a paragraph, a wrapped line, or the same line
            const struct uhv_item *prev = &v->item[v->order[k - 1]];
            if (it->para != prev->para) out[n++] = '\n';
            else if ((it->region != prev->region || it->ly != prev->ly) && n && out[n - 1] != ' ') out[n++] = ' ';
        }
        memcpy(out + n, txt + a, (size_t)(b - a));
        n += (size_t)(b - a);
    }
    out[n] = 0;
    return out;
}

// --- the pop-up ----------------------------------------------------------------------------------------------------------------

void uui_helpview_popup(struct uui_helpview *v, const struct uhlp *h, const struct uhlp_doc *d,
                        int ax, int ay, int aw, int ah) {
    if (!v->pop) {
        v->pop = calloc(1, sizeof *v->pop);
        if (!v->pop) return;
        uui_sbar_init(&v->pop->bar);
        v->pop->armed = v->pop->hover = v->pop->armed_hot = -1;
        v->pop->fonts = v->fonts;
        v->pop->owns_fonts = 0;
        v->pop->is_popup = 1;
    }
    v->pop->help = h;
    v->pop->doc = d;
    v->pop->doc_serial++;
    v->pop->scroll = 0;
    v->pop->zoom = v->zoom;
    v->pop->have_taken = 0;
    v->pop_ax = ax; v->pop_ay = ay; v->pop_aw = aw; v->pop_ah = ah;
}

void uui_helpview_popup_close(struct uui_helpview *v) {
    if (v->pop) { v->pop->doc = 0; v->pop->have_taken = 0; }
}

int uui_helpview_popup_open(const struct uui_helpview *v) { return v->pop && v->pop->doc; }

static void popup_geometry(struct uui_helpview *v) {
    struct uui_helpview *p = v->pop;
    int em = em_now();
    int w = em * 26;
    if (w > v->w - 2 * em) w = v->w - 2 * em;
    if (w < em * 8) w = em * 8;
    p->zoom = v->zoom;
    p->x = 0; p->y = 0; p->w = w; p->h = 4000;           // measured tall, then cut to the room
    p->m_doc = 0;
    ensure_layout(p);
    int h = p->sc_h + p->ns_h;
    int maxh = v->h - 2 * em;
    if (h > maxh) h = maxh;
    p->h = h;
    p->m_doc = 0;
    ensure_layout(p);
    int x = v->pop_ax;
    if (x + w > v->x + v->w - em / 2) x = v->x + v->w - em / 2 - w;
    if (x < v->x + em / 2) x = v->x + em / 2;
    int y = v->pop_ay + v->pop_ah + 4;
    if (y + h > v->y + v->h - em / 2) y = v->pop_ay - h - 4;
    if (y < v->y + em / 2) y = v->y + em / 2;
    p->x = x; p->y = y;
}

static void draw_popup(struct ugfx_surface *s, struct uui_helpview *v) {
    if (!uui_helpview_popup_open(v)) return;
    const struct ugfx_font *saved = ugfx_font_current();
    popup_geometry(v);
    struct uui_helpview *p = v->pop;
    uui_fill_round_rect(s, p->x + 2, p->y + 3, p->w, p->h, 8, ugfx_blend(UTHEME_WHITE, 0x000000, 40));
    uui_fill_round_rect(s, p->x, p->y, p->w, p->h, 8, UTHEME_WHITE);
    uui_stroke_round_rect(s, p->x, p->y, p->w, p->h, 8, UTHEME_BORDER);
    struct ugfx_clip clip;
    ugfx_clip_save(s, &clip);
    ugfx_clip_intersect(s, p->x + 2, p->y + 2, p->w - 4, p->h - 4);
    {
        ensure_layout(p);
        int barw = p->bar_on ? uui_sbar_width() : 0;
        draw_items(s, p, 0, p->x, p->y, p->w, p->ns_h ? p->ns_h : 0);
        draw_items(s, p, 1, p->x, p->y + p->ns_h, p->w - barw, p->h - p->ns_h);
    }
    ugfx_clip_restore(s, &clip);
    ugfx_set_font(saved);
}

// --- the ops ---------------------------------------------------------------------------------------------------------------------

static void op_draw(struct ugfx_surface *s, const void *w) { helpview_draw(s, (struct uui_helpview *)(void *)(uintptr_t)w); }
static void op_overlay(struct ugfx_surface *s, const void *w) { draw_popup(s, (struct uui_helpview *)(void *)(uintptr_t)w); }
static void op_natural(const void *w, int *ow, int *oh) { (void)w; if (ow) *ow = 0; if (oh) *oh = 0; }
static void op_geom(void *w, int x, int y, int cw, int ch) {
    struct uui_helpview *v = w;
    v->x = x; v->y = y; v->w = cw; v->h = ch;
}
static void op_bounds(const void *w, int *x, int *y, int *cw, int *ch) {
    const struct uui_helpview *v = w;
    if (x) *x = v->x;
    if (y) *y = v->y;
    if (cw) *cw = v->w;
    if (ch) *ch = v->h;
}
static int op_hit(const void *w, int cx, int cy) {
    const struct uui_helpview *v = w;
    return uui_hit(v->x, v->y, v->w, v->h, cx, cy);        // the whole rect: the bar is in it
}
static int op_overlay_active(const void *w) { return uui_helpview_popup_open(w); }

static int op_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_helpview *v = w;
    if (uui_helpview_popup_open(v)) {                       // a click in the pop-up is its own; anywhere else closes it
        struct uui_helpview *p = v->pop;
        if (uui_hit(p->x, p->y, p->w, p->h, cx, cy)) {
            int rx, ry, rw, rh;
            p->armed = link_at(p, cx, cy, &rx, &ry, &rw, &rh);
            if (p->armed >= 0) { p->pop_ax = rx; p->pop_ay = ry; p->pop_aw = rw; p->pop_ah = rh; return 1; }
        }
        uui_helpview_popup_close(v);
        return 1;
    }
    int r = uui_sbar_press(&v->bar, cx, cy);
    if (r & UUI_SBAR_TOOK) {
        if (r & UUI_SBAR_MOVED) v->scroll = v->bar.top;
        return 1;
    }
    int rx, ry, rw, rh;
    v->armed = link_at(v, cx, cy, &rx, &ry, &rw, &rh);
    v->pop_ax = rx; v->pop_ay = ry; v->pop_aw = rw; v->pop_ah = rh;
    // A press in the text may become a drag: remember where, and where in the text.
    v->press_x = cx; v->press_y = cy;
    v->sel_on = 0;
    int ord, ch;
    if (pos_at(v, cx, cy, &ord, &ch)) {
        v->sel_a_ord = v->sel_f_ord = ord;
        v->sel_a_ch = v->sel_f_ch = ch;
        v->selecting = 1;
    }
    return v->armed >= 0 || v->selecting;
}

static int op_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_helpview *v = w;
    if (uui_helpview_popup_open(v)) {
        struct uui_helpview *p = v->pop;
        int hv = link_at(p, cx, cy, 0, 0, 0, 0);
        int ch = hv != p->hover;
        p->hover = hv;
        return ch;
    }
    int r = uui_sbar_motion(&v->bar, cx, cy, buttons);
    if (r & UUI_SBAR_MOVED) v->scroll = v->bar.top;
    if (r & UUI_SBAR_TOOK) return 1;
    if (buttons && v->selecting) {
        int dx = cx - v->press_x, dy = cy - v->press_y;
        if (!v->sel_on && dx * dx + dy * dy < 16) return 0;      // not yet a drag
        v->armed = -1;                                          // a drag is not a click on a link
        v->sel_on = 1;
        // past the top or bottom of the scrolling text, the view follows the pointer
        int top = v->y + v->ns_h, bot = v->y + v->h;
        if (cy < top) { v->scroll -= em_now(); clamp_scroll(v); }
        else if (cy > bot) { v->scroll += em_now(); clamp_scroll(v); }
        int ord, ch;
        if (pos_at(v, cx, cy, &ord, &ch)) { v->sel_f_ord = ord; v->sel_f_ch = ch; }
        return 1;
    }
    if (buttons) return r != 0;
    int hv = link_at(v, cx, cy, 0, 0, 0, 0);
    int changed = hv != v->hover || (r & UUI_SBAR_REDRAW);
    v->hover = hv;
    return changed;
}

static int op_release(void *w, int cx, int cy) {
    struct uui_helpview *v = w;
    if (uui_helpview_popup_open(v)) {
        struct uui_helpview *p = v->pop;
        int rx, ry, rw, rh;
        int l = link_at(p, cx, cy, &rx, &ry, &rw, &rh);
        if (p->armed >= 0 && l == p->armed) take_link(p, l, rx, ry, rw, rh);
        p->armed = -1;
        return 1;
    }
    int r = uui_sbar_release(&v->bar);
    (void)r;
    v->selecting = 0;
    if (!uui_helpview_has_selection(v)) v->sel_on = 0;
    int rx, ry, rw, rh;
    int l = link_at(v, cx, cy, &rx, &ry, &rw, &rh);
    if (v->armed >= 0 && l == v->armed) take_link(v, l, rx, ry, rw, rh);
    v->armed = -1;
    return 1;
}

static int op_wheel(void *w, int notches) {
    struct uui_helpview *v = w;
    if (uui_helpview_popup_open(v)) return 1;
    v->scroll -= notches * em_now() * 3;
    clamp_scroll(v);
    return 1;
}

static int op_cursor(const void *w, int cx, int cy) {
    struct uui_helpview *v = (struct uui_helpview *)(void *)(uintptr_t)w;
    struct uui_helpview *t = uui_helpview_popup_open(v) ? v->pop : v;
    if (link_at(t, cx, cy, 0, 0, 0, 0) >= 0) return WIN_CURSOR_HAND;
    if (t == v && cx >= v->x && cx < v->x + v->w - (v->bar_on ? uui_sbar_width() : 0) && cy >= v->y && cy < v->y + v->h)
        for (int k = 0; k < v->norder; k++) {                         // over a word: the text cursor
            const struct uhv_item *it = &v->item[v->order[k]];
            if (uui_hit(it->x + v->x, it->y + region_top(v, it->region), it->w, it->h, cx, cy)) return WIN_CURSOR_TEXT;
        }
    return WIN_CURSOR_DEFAULT;
}

int uui_helpview_key(struct uui_helpview *v, int key, unsigned mods) {
    (void)mods;
    if (uui_helpview_popup_open(v)) {
        if (key == 0x1B) { uui_helpview_popup_close(v); return 1; }
        return 0;
    }
    int vp = v->h - v->ns_h, line = em_now() * 2;
    switch (key) {
    case KEY_ARROW_UP: v->scroll -= line; break;
    case KEY_ARROW_DOWN: v->scroll += line; break;
    case KEY_PAGE_UP: v->scroll -= vp - line; break;
    case KEY_PAGE_DOWN: v->scroll += vp - line; break;
    case KEY_HOME: v->scroll = 0; break;
    case KEY_END: v->scroll = v->sc_h; break;
    default: return 0;
    }
    clamp_scroll(v);
    return 1;
}

// The links as drawn, for a test to click: where and what.
static void op_describe(const void *w, const struct uui_describe *d) {
    struct uui_helpview *v = (struct uui_helpview *)(void *)(uintptr_t)w;
    if (!v->doc || v->w <= 0) return;
    const struct ugfx_font *saved = ugfx_font_current();
    ensure_layout(v);
    ugfx_set_font(saved);
    uui_describe_int(d, "scroll", v->scroll);
    uui_describe_int(d, "content_h", v->ns_h + v->sc_h);
    uui_describe_int(d, "ns_h", v->ns_h);
    int shown = 0;
    for (int i = 0; i < v->nitem && shown < 24; i++) {
        const struct uhv_item *it = &v->item[i];
        if (it->link < 0 || it->kind != IT_TEXT) continue;
        int top = region_top(v, it->region);
        int y = it->y + top;
        if (y + it->h <= v->y || y >= v->y + v->h) continue;
        uui_describe_rect_i(d, "link", it->link, it->x + v->x, y, it->w, it->h);
        shown++;
    }
    if (uui_helpview_popup_open(v)) {
        popup_geometry(v);
        ugfx_set_font(saved);
        uui_describe_rect(d, "popup", v->pop->x, v->pop->y, v->pop->w, v->pop->h);
    }
}

// --- the right-click menu ----------------------------------------------------------------------------------

enum { M_OPEN = 1, M_COPY, M_SELALL, M_BACK, M_FWD, M_HOME };

static const struct uui_menu_item MENU[] = {
    UUI_MENU("Copy",       M_COPY,   "Ctrl+C"),
    UUI_MENU("Select All", M_SELALL, "Ctrl+A"),
    UUI_MENU_SEP,
    UUI_MENU("Back",       M_BACK,   "Alt+Left"),
    UUI_MENU("Forward",    M_FWD,    "Alt+Right"),
    UUI_MENU("Contents",   M_HOME,   "Ctrl+Home"),
};
static const struct uui_menu_item MENU_LINK[] = {
    UUI_MENU("Open",       M_OPEN,   0),
    UUI_MENU_SEP,
    UUI_MENU("Copy",       M_COPY,   "Ctrl+C"),
    UUI_MENU("Select All", M_SELALL, "Ctrl+A"),
    UUI_MENU_SEP,
    UUI_MENU("Back",       M_BACK,   "Alt+Left"),
    UUI_MENU("Forward",    M_FWD,    "Alt+Right"),
    UUI_MENU("Contents",   M_HOME,   "Ctrl+Home"),
};

static unsigned menu_flags(void *w, int code) {
    struct uui_helpview *v = w;
    switch (code) {
    case M_COPY: return uui_helpview_has_selection(v) ? 0 : UUI_MI_DISABLED;
    case M_SELALL: return v->norder ? 0 : UUI_MI_DISABLED;
    case M_BACK: return v->can_back ? 0 : UUI_MI_DISABLED;
    case M_FWD: return v->can_fwd ? 0 : UUI_MI_DISABLED;
    default: return 0;
    }
}

static int menu_run(void *w, int code) {
    struct uui_helpview *v = w;
    switch (code) {
    case M_OPEN:
        if (v->ctx_link >= 0 && v->doc) {
            take_link(v, v->ctx_link, v->ctx_ax, v->ctx_ay, v->ctx_aw, v->ctx_ah);
            return 1;
        }
        return 0;
    case M_SELALL: uui_helpview_select_all(v); return 1;
    case M_COPY: v->cmd = UHV_CMD_COPY; return 1;
    case M_BACK: v->cmd = UHV_CMD_BACK; return 1;
    case M_FWD: v->cmd = UHV_CMD_FORWARD; return 1;
    case M_HOME: v->cmd = UHV_CMD_CONTENTS; return 1;
    default: return 0;
    }
}

int uui_helpview_take_command(struct uui_helpview *v) {
    int c = v->cmd;
    v->cmd = 0;
    return c;
}

static int op_context_menu(void *w, int cx, int cy, struct uui_ctxmenu_src *out) {
    struct uui_helpview *v = w;
    if (!v->doc || uui_helpview_popup_open(v)) return 0;
    if (cx < v->x || cx >= v->x + v->w - (v->bar_on ? uui_sbar_width() : 0) || cy < v->y || cy >= v->y + v->h) return 0;
    v->ctx_link = link_at(v, cx, cy, &v->ctx_ax, &v->ctx_ay, &v->ctx_aw, &v->ctx_ah);
    const struct uui_menu_item *items = v->ctx_link >= 0 ? MENU_LINK : MENU;
    out->w = v;
    out->items = items;
    out->count = v->ctx_link >= 0 ? (int)(sizeof MENU_LINK / sizeof MENU_LINK[0]) : (int)(sizeof MENU / sizeof MENU[0]);
    out->flags = menu_flags;
    out->run = menu_run;
    return 1;
}

const struct uui_widget_ops uui_helpview_ops = {
    .context_menu = op_context_menu,
    .draw = op_draw,
    .draw_overlay = op_overlay,
    .natural_size = op_natural,
    .set_geometry = op_geom,
    .bounds = op_bounds,
    .hit = op_hit,
    .press = op_press,
    .motion = op_motion,
    .release = op_release,
    .wheel = op_wheel,
    .cursor = op_cursor,
    .overlay_active = op_overlay_active,
    .describe = op_describe,
};
