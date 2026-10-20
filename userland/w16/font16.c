// font16.c -- the fonts a Win16 program's text is drawn in: the stock
// ones (System, Fixedsys, Terminal...), a dialog template's FONT, and
// whatever CreateFont asks for. Each comes from Windows 3.11's own bitmap
// fonts -- the .FON files in C:\WINDOWS\SYSTEM, read with lib/winfnt --
// when the `fonts` setting allows and the file is there, else from a
// toy-os stand-in (Liberation Sans, Serif, DejaVu Sans Mono). TRUETYPE faces
// (Arial, Times New Roman, Courier New) come from C:'s own .TTF files under the
// same setting, else from those same toy-os fonts, which are their metric
// twins -- so a line breaks where 3.11 broke it either way. Split from
// gdi16.c: what draws is there, what a font IS is here.
//
// THE FONT FILES ARE THE PROGRAM'S INPUT, untrusted like the program:
// lib/winfnt checks every offset, and w16 reads them inside its sandbox.
//
// Every call here runs under g_w16_text_lock (the drawing does too), so
// the caches below need no lock of their own.
#include "w16_internal.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "font_ttf.h"                            // the glyph slots a ugfx font holds
#include "ttf.h"
#include "lib/ufile.h"
#include "lib/winfnt.h"
#include "w16/w16prefs.h"
#include "ui/ugfx.h"

int g_w16_fonts = W16_FONTS_WIN311;              // w16.c sets it from the program's settings

#define STOCK_BASE        0x5000
#define OEM_FIXED_FONT    10
#define ANSI_FIXED_FONT   11
#define ANSI_VAR_FONT     12
#define SYSTEM_FONT       13
#define DEVICE_DEFAULT_FONT 14
#define SYSTEM_FIXED_FONT 16
#define FW_BOLD_FROM      600                    // a weight GDI draws bold

// --- 3.11's own fonts, found once ---------------------------------------------

struct real {
    struct winfnt f;
    char file[16];                               // its .FON's name, for --fonts
};
static struct real *g_real;
static int g_nreal, g_scanned;
static uint8_t **g_files;                         // the files' bytes, which the fonts point into
static int g_nfiles;

static int is_fon(const char *n) {
    size_t l = strlen(n);
    return l > 4 && !strcasecmp(n + l - 4, ".fon");
}

static void scan(void) {
    if (g_scanned) return;
    g_scanned = 1;
    static char dir[1024], path[1400];           // once, on the one thread that scans
    if (w16_dos_path("C:\\WINDOWS\\SYSTEM", dir, sizeof dir, 0, 0, 0) != 0) return;
    DIR *d = opendir(dir);
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (!is_fon(de->d_name)) continue;
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        unsigned char *bytes = 0;
        size_t n = 0;
        if (ufile_slurp(path, 4ul * 1024 * 1024, &bytes, &n) != UFILE_OK) continue;
        uint8_t **gf = realloc(g_files, sizeof *g_files * (size_t)(g_nfiles + 1));
        if (!gf) { free(bytes); continue; }
        g_files = gf;
        g_files[g_nfiles++] = bytes;
        int count = winfnt_count(bytes, n);
        for (int i = 0; i < count; i++) {
            struct winfnt f;
            if (winfnt_get(bytes, n, i, &f) < 0 || f.vector) continue;
            struct real *g = realloc(g_real, sizeof *g_real * (size_t)(g_nreal + 1));
            if (!g) break;
            g_real = g;
            g_real[g_nreal].f = f;
            snprintf(g_real[g_nreal].file, sizeof g_real[g_nreal].file, "%s", de->d_name);
            g_nreal++;
        }
    }
    if (d) closedir(d);
}

// 3.0's names for 3.1's raster faces.
static const char *alias(const char *face) {
    static const struct { const char *from, *to; } A[] = {
        { "Helv", "MS Sans Serif" }, { "Tms Rmn", "MS Serif" },
    };
    for (unsigned i = 0; i < sizeof A / sizeof A[0]; i++)
        if (!strcasecmp(face, A[i].from)) return A[i].to;
    return face;
}

// --- TrueType: the user's own from C:, else toy-os's metric twins ------------------

struct ttface {
    char family[32];
    int style;                                   // TTF_BOLD | TTF_ITALIC
    char *path;
    int upem, asc, desc, gap, avg, maxw;         // font units: Windows' cell (OS/2), hhea's gap, OS/2's average
    int pitch_family;                            // what GetTextMetrics says of it
};
static struct ttface *g_tt;
static int g_ntt, g_tt_scanned;

// The faces Windows 3.1 shipped as TrueType, and the toy-os files drawn in
// their place: metric-compatible by design (Liberation), or the nearest
// monospace (DejaVu, for Courier New).
static const struct { const char *family; const char *file[4]; int pitch_family; } TWINS[] = {
    { "Arial", { "liberation-sans", "liberation-sans-bold", "liberation-sans-italic", "liberation-sans-bold-italic" }, 0x27 },
    { "Times New Roman", { "liberation-serif", "liberation-serif-bold", "liberation-serif-italic",
                           "liberation-serif-bold-italic" }, 0x17 },
    { "Courier New", { "dejavu-sans-mono", "dejavu-sans-mono-bold", "dejavu-sans-mono-italic",
                       "dejavu-sans-mono-bold-italic" }, 0x36 },
};

static int be16(const uint8_t *p) { return p[0] << 8 | p[1]; }

// A .TTF's family, style and metrics, read and checked by ttf.c. 0 when it is
// no TrueType font ttf.c draws.
static int tt_read(const char *path, struct ttface *out) {
    unsigned char *bytes = 0;
    size_t n = 0;
    if (ufile_slurp(path, 16ul * 1024 * 1024, &bytes, &n) != UFILE_OK) return 0;
    struct ttf_font f;
    int ok = ttf_open(&f, bytes, (uint32_t)n) && ttf_family(&f, out->family, sizeof out->family) > 0;
    if (ok) {
        out->style = ttf_style(&f);
        out->upem = f.units_per_em;
        ttf_win_metrics(&f, &out->asc, &out->desc);
        out->gap = f.line_gap - ((out->asc + out->desc) - (f.ascent + f.descent));
        out->avg = f.os2 && f.os2_len >= 4 ? (int16_t)be16(bytes + f.os2 + 2) : f.units_per_em / 2;
        out->maxw = (int)be16(bytes + f.hhea + 10);
        out->pitch_family = 0x27;
        for (unsigned i = 0; i < sizeof TWINS / sizeof TWINS[0]; i++)
            if (!strcasecmp(TWINS[i].family, out->family)) out->pitch_family = TWINS[i].pitch_family;
        out->path = strdup(path);
        ok = out->path != 0;
    }
    free(bytes);
    return ok;
}

static int tt_add(const struct ttface *t) {
    struct ttface *g = realloc(g_tt, sizeof *g * (size_t)(g_ntt + 1));
    if (!g) return 0;
    g_tt = g;
    g_tt[g_ntt++] = *t;
    return 1;
}

static int is_ttf(const char *n) {
    size_t l = strlen(n);
    return l > 4 && !strcasecmp(n + l - 4, ".ttf");
}

// C:'s TrueType fonts when the setting allows them, then the twins of any of
// 3.1's three faces C: did not have.
static void tt_scan(void) {
    if (g_tt_scanned) return;
    g_tt_scanned = 1;
    static char dir[1024], path[1400];
    DIR *d = g_w16_fonts == W16_FONTS_WIN311 && w16_dos_path("C:\\WINDOWS\\SYSTEM", dir, sizeof dir, 0, 0, 0) == 0
             ? opendir(dir) : 0;
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (!is_ttf(de->d_name)) continue;
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        struct ttface t = { 0 };
        if (tt_read(path, &t) && !tt_add(&t)) free(t.path);
    }
    if (d) closedir(d);
    for (unsigned i = 0; i < sizeof TWINS / sizeof TWINS[0]; i++) {
        int have = 0;
        for (int k = 0; k < g_ntt; k++) have |= !strcasecmp(g_tt[k].family, TWINS[i].family);
        for (int st = 0; !have && st < 4; st++) {
            snprintf(path, sizeof path, "/usr/share/fonts/%s.ttf", TWINS[i].file[st]);
            struct ttface t = { 0 };
            if (!tt_read(path, &t)) continue;
            snprintf(t.family, sizeof t.family, "%s", TWINS[i].family);   // drawn as, and named, 3.1's face
            t.style = st == 0 ? 0 : st == 1 ? TTF_BOLD : st == 2 ? TTF_ITALIC : TTF_BOLD | TTF_ITALIC;
            t.pitch_family = TWINS[i].pitch_family;
            if (!tt_add(&t)) free(t.path);
        }
    }
}

// The TrueType face for a request: its family in the style asked, else its
// regular face (*synth: bold drawn by smearing, as GDI did). NULL when the
// family is not a TrueType one.
static const struct ttface *tt_find(const char *family, int bold, int italic, int *synth) {
    tt_scan();
    int want = (bold ? TTF_BOLD : 0) | (italic ? TTF_ITALIC : 0);
    const struct ttface *best = 0;
    int best_score = -1;
    for (int i = 0; i < g_ntt; i++) {
        if (strcasecmp(g_tt[i].family, family)) continue;
        int st = g_tt[i].style;
        // the exact style, else one with the italic asked (bold can be made), else any
        int score = st == want ? 3 : (st & TTF_ITALIC) == (want & TTF_ITALIC) && !(st & ~want) ? 2 : !(st & ~want) ? 1 : 0;
        if (score > best_score) { best = &g_tt[i]; best_score = score; }
    }
    if (best) *synth = bold && !(best->style & TTF_BOLD);
    return best;
}

// The raster font for `face` nearest `height` (a cell height when positive,
// a character height -- the cell less its internal leading -- when
// negative, 3.11's default when 0), or -1 when 3.11's fonts have no face
// of that name.
static int match(const char *face, int height) {
    scan();
    face = alias(face);
    int best = -1, best_d = 0;
    for (int i = 0; i < g_nreal; i++) {
        const struct winfnt *f = &g_real[i].f;
        if (strcasecmp(f->face, face)) continue;
        int have = height < 0 ? f->pix_height - f->internal_leading : f->pix_height;
        int want = height ? (height < 0 ? -height : height) : 13;
        int d = have > want ? (have - want) * 2 + 1 : (want - have) * 2;   // a smaller size wins a tie
        if (best < 0 || d < best_d) { best = i; best_d = d; }
    }
    return best;
}

// --- fonts as drawn: a ugfx font, and the metrics GetTextMetrics tells ---------

struct inst {
    int real;                                    // into g_real, or -1 for a stand-in or a TrueType face
    int standin;                                 // which stand-in face (STAND_*)
    const struct ttface *tt;                     // the TrueType face, NULL for the other two
    int px, bold;
    struct ugfx_font uf;
    void *mem;
    struct w16_fontinfo info;
};
static struct inst **g_inst;                    // each its own allocation: a font in use never moves
static int g_ninst;

// A 3.11 raster font as a ugfx font: its glyphs in toy-os's slots, at its
// own cell, emboldened as GDI did a raster font -- each row smeared one
// pixel right, the advance one wider.
static int from_real(struct inst *in) {
    const struct winfnt *f = &g_real[in->real].f;
    int extra = in->bold ? 1 : 0, cw = 1, h = f->pix_height;
    for (int s = 0; s < FONT_TTF_GLYPH_COUNT; s++) {
        int w = winfnt_width(f, font_ttf_slot_codepoint(s));
        if (w + extra > cw) cw = w + extra;
    }
    if (cw > 255) cw = 255;                       // ugfx keeps an advance in a byte
    size_t cell = (size_t)cw * (size_t)h;
    unsigned char *mem = calloc(cell * FONT_TTF_GLYPH_COUNT + FONT_TTF_GLYPH_COUNT, 1);
    if (!mem) return -1;
    unsigned char *adv = mem + cell * FONT_TTF_GLYPH_COUNT;
    long sum = 0;
    for (int s = 0; s < FONT_TTF_GLYPH_COUNT; s++) {
        unsigned char *g = mem + cell * (size_t)s;
        int w = winfnt_glyph(f, font_ttf_slot_codepoint(s), g, cw, cw - extra);
        if (w < 0) w = 0;
        if (extra)
            for (int y = 0; y < h; y++)
                for (int x = w; x > 0; x--)
                    if (g[y * cw + x - 1]) g[y * cw + x] = 255;
        adv[s] = (unsigned char)(w + extra < cw ? w + extra : cw);
        sum += adv[s];
    }
    in->mem = mem;
    in->uf = (struct ugfx_font){ .glyphs = mem, .advances = adv, .kern = 0, .char_w = cw,
                                 .char_h = h, .line_h = h, .count = FONT_TTF_GLYPH_COUNT };
    struct w16_fontinfo *i = &in->info;
    i->height = h;
    i->ascent = f->ascent;
    i->internal_leading = f->internal_leading;
    i->external_leading = f->external_leading;
    i->ave = f->avg_width + extra;
    i->maxw = f->max_width + extra;
    i->weight = in->bold ? 700 : f->weight;
    i->italic = f->italic;
    i->pitch_family = f->pitch_family;
    i->charset = f->charset;
    i->first = f->first;
    i->last = f->last;
    i->def = f->first + f->default_char;
    i->face = f->face;
    i->ave = (int)(sum / FONT_TTF_GLYPH_COUNT) > 0 && !f->avg_width ? (int)(sum / FONT_TTF_GLYPH_COUNT) : i->ave;
    return 0;
}

enum { STAND_SANS, STAND_SERIF, STAND_MONO };
// The 3.11 face each stand-in takes the place of: what GetTextFace and EnumFonts name.
static const char *const STAND_FACE[3] = { "MS Sans Serif", "MS Serif", "Courier" };

static int from_standin(struct inst *in) {
    static const char *const FILES[3][2] = {
        { "/usr/share/fonts/liberation-sans.ttf", "/usr/share/fonts/liberation-sans-bold.ttf" },
        { "/usr/share/fonts/liberation-serif.ttf", "/usr/share/fonts/liberation-serif-bold.ttf" },
        { "/usr/share/fonts/dejavu-sans-mono.ttf", "/usr/share/fonts/dejavu-sans-mono-bold.ttf" },
    };
    int px = in->px < 6 ? 6 : in->px > 96 ? 96 : in->px;
    unsigned long need = ugfx_font_arena_size(px);
    void *arena = malloc(need);
    if (!arena || !ugfx_font_load(FILES[in->standin][in->bold], px, 0, &in->uf, arena, need)) {
        free(arena);
        return -1;
    }
    in->mem = arena;
    struct w16_fontinfo *i = &in->info;
    int h = in->uf.line_h;
    i->height = h;
    i->ascent = h * 4 / 5;
    i->internal_leading = h / 8;
    const struct ugfx_font *was = ugfx_set_font(&in->uf);
    i->ave = ugfx_text_width("x");
    i->maxw = ugfx_text_width("W");
    ugfx_set_font(was);
    i->weight = in->bold ? 700 : 400;
    i->pitch_family = in->standin == STAND_MONO ? 0x30 : in->standin == STAND_SERIF ? 0x11 : 0x21;   // bit 0: variable
    i->first = 0x20;
    i->last = 0xFF;
    i->def = 0x80;
    i->face = STAND_FACE[in->standin];
    return 0;
}

// A TrueType face at an em of `px`, with Windows' metrics for it: the cell
// from OS/2's ascent and descent, the leading inside it and the gap beyond.
static int from_tt(struct inst *in) {
    const struct ttface *t = in->tt;
    int px = in->px < 6 ? 6 : in->px > UGFX_FONT_PX_MAX ? UGFX_FONT_PX_MAX : in->px;
    unsigned long need = ugfx_font_arena_size(px);
    void *arena = malloc(need);
    if (!arena || !ugfx_font_load(t->path, px, in->bold, &in->uf, arena, need)) {
        free(arena);
        return -1;
    }
    in->mem = arena;
    struct w16_fontinfo *i = &in->info;
    int asc = (t->asc * px + t->upem / 2) / t->upem, desc = (t->desc * px + t->upem / 2) / t->upem;
    // Where the face was hinted for this size, Windows' own device metrics:
    // its cell (VDMX) and each glyph's advance (hdmx) -- what 3.1 laid text
    // out by, a pixel or two off the outline's scaled numbers.
    unsigned char *bytes = 0;
    size_t n = 0;
    struct ttf_font f;
    int base = asc;                              // where ugfx put the baseline: the outline's ascent
    if (ufile_slurp(t->path, 16ul * 1024 * 1024, &bytes, &n) == UFILE_OK && ttf_open(&f, bytes, (uint32_t)n)) {
        int ymax, ymin, adv;
        base = (f.ascent * px + f.units_per_em / 2) / f.units_per_em;
        if (ttf_vdmx(&f, px, &ymax, &ymin) && ymax > 0) { asc = ymax; desc = -ymin; }
        unsigned char *advances = (unsigned char *)in->uf.advances;   // the arena's: ours to change
        for (int k = 0; advances && k < in->uf.count; k++)
            if (ttf_hdmx(&f, px, ttf_glyph_index(&f, font_ttf_slot_draw_codepoint(k)), &adv))
                advances[k] = (unsigned char)(adv + (in->bold && adv ? (px / 24 < 1 ? 1 : px / 24 > 3 ? 3 : px / 24) : 0));
    }
    free(bytes);
    in->uf.kern = 0;                             // GDI's TextOut never kerned
    i->dy = asc > base ? asc - base : 0;         // Windows' cell may reach higher above the baseline
    i->height = asc + desc;
    i->ascent = asc;
    i->internal_leading = i->height - px > 0 ? i->height - px : 0;
    i->external_leading = t->gap > 0 ? (t->gap * px + t->upem / 2) / t->upem : 0;
    i->ave = (t->avg * px + t->upem / 2) / t->upem + (in->bold ? 1 : 0);
    i->maxw = (t->maxw * px + t->upem / 2) / t->upem + (in->bold ? 1 : 0);
    i->weight = in->bold || (t->style & TTF_BOLD) ? 700 : 400;
    i->italic = (t->style & TTF_ITALIC) != 0;
    i->pitch_family = t->pitch_family;
    i->first = 0x20;
    i->last = 0xFF;
    i->def = 0x80;
    i->face = t->family;
    return 0;
}

// The font instance for a real font, a stand-in or a TrueType face, made once and kept.
static struct inst *instance_of(int real, int standin, const struct ttface *tt, int px, int bold) {
    for (int i = 0; i < g_ninst; i++) {
        struct inst *in = g_inst[i];
        if (in->real == real && in->tt == tt && (real >= 0 || (in->standin == standin && in->px == px)) &&
            in->bold == bold)
            return in;
    }
    struct inst **g = realloc(g_inst, sizeof *g_inst * (size_t)(g_ninst + 1));
    if (!g) return 0;
    g_inst = g;
    struct inst *in = calloc(1, sizeof *in);
    if (!in) return 0;
    in->real = real;
    in->standin = standin;
    in->tt = tt;
    in->px = px;
    in->bold = bold;
    if ((real >= 0 ? from_real(in) : tt ? from_tt(in) : from_standin(in)) < 0) { free(in); return 0; }
    in->info.uf = &in->uf;
    g_inst[g_ninst++] = in;
    return in;
}

static struct inst *instance(int real, int standin, int px, int bold) { return instance_of(real, standin, 0, px, bold); }

// What a font request comes to: 3.11's face when allowed and present, else
// the stand-in -- `cell` the stand-in's line, `px` its size.
static const struct w16_fontinfo *resolve(const char *face, int height, int weight, int standin, int px, int cell) {
    int bold = weight >= FW_BOLD_FROM;
    if (g_w16_fonts == W16_FONTS_WIN311) {
        int r = match(face, height);
        if (r >= 0) {
            struct inst *in = instance(r, 0, 0, bold && g_real[r].f.weight < FW_BOLD_FROM);
            if (in) return &in->info;
        }
    }
    struct inst *in = instance(-1, standin, px, bold);
    if (!in) return 0;
    if (cell) in->info.height = cell;            // a stock font keeps 3.11's cell over the stand-in's own
    return &in->info;
}

// --- what an HFONT is ---------------------------------------------------------


static const struct w16_fontinfo *stock(unsigned i) {
    switch (i) {
    case SYSTEM_FIXED_FONT: return resolve("Fixedsys", 15, 400, STAND_MONO, 13, 15);
    case OEM_FIXED_FONT:    return resolve("Terminal", 12, 400, STAND_MONO, 13, 15);
    case ANSI_FIXED_FONT:   return resolve("Courier", 13, 400, STAND_MONO, 13, 15);
    case ANSI_VAR_FONT:     return resolve("MS Sans Serif", 13, 400, STAND_SANS, 11, 13);
    default:                return resolve("System", 16, 700, STAND_SANS, 13, 16);
    }
}

// A TrueType face asked by a LOGFONT: its em from the height -- a character
// height when negative, a cell (ascent and descent) when positive, 12 points
// when 0 -- as Windows' TrueType mapper sized it. NULL: no such face.
static const struct w16_fontinfo *truetype(const struct w16_logfont *lf) {
    int synth;
    const struct ttface *t = tt_find(lf->face, lf->weight >= FW_BOLD_FROM, lf->italic, &synth);
    if (!t) return 0;
    int px = lf->height < 0 ? -lf->height
           : lf->height ? (lf->height * t->upem + (t->asc + t->desc) / 2) / (t->asc + t->desc) : 16;
    struct inst *in = instance_of(-1, 0, t, px, synth);
    return in ? &in->info : 0;
}

static const struct w16_fontinfo *of(uint16_t hfont) {
    if (hfont == W16_FONT_DIALOG) return resolve("MS Sans Serif", 13, 700, STAND_SANS, 11, 13);
    if (hfont >= STOCK_BASE && hfont < STOCK_BASE + 32) return stock(hfont - STOCK_BASE);
    const struct w16_logfont *lf = w16_gdi_logfont(&g_w16, hfont);
    if (!lf) return stock(SYSTEM_FONT);
    int fixed = (lf->pitch_family & 3) == 1;
    int family = lf->pitch_family & 0xF0;
    int standin = fixed || family == 0x30 ? STAND_MONO : family == 0x10 ? STAND_SERIF : STAND_SANS;
    if (!lf->face[0]) {
        // No face named: the family's raster font, as 3.11's mapper chose.
        const char *face = fixed || family == 0x30 ? "Courier" : family == 0x10 ? "MS Serif"
                          : family == 0x20 ? "MS Sans Serif" : "System";
        int px = lf->height < 0 ? -lf->height : lf->height ? lf->height * 4 / 5 : 13;
        return resolve(face, lf->height, lf->weight, standin, px, 0);
    }
    const struct w16_fontinfo *tt = truetype(lf);
    if (tt) return tt;
    if (strcasestr(lf->face, "courier") || strcasestr(lf->face, "fixedsys") || strcasestr(lf->face, "terminal"))
        standin = STAND_MONO;
    else if (strcasestr(lf->face, "serif") && !strcasestr(lf->face, "sans")) standin = STAND_SERIF;
    else if (strcasestr(lf->face, "roman") || strcasestr(lf->face, "tms")) standin = STAND_SERIF;
    int px = lf->height < 0 ? -lf->height : lf->height ? lf->height * 4 / 5 : 13;
    return resolve(lf->face, lf->height, lf->weight, standin, px, 0);
}

// --- EnumFonts --------------------------------------------------------------------

// The sizes 3.11's VGA fonts come in, for the stand-ins to offer the same:
// MS Sans Serif and MS Serif at 8, 10, 12, 14, 18 and 24 points, Courier at
// 10, 12 and 15.
static const struct { int standin; int px[6]; } STAND_SIZES[3] = {
    { STAND_SANS, { 13, 16, 20, 24, 29, 37 } },
    { STAND_SERIF, { 13, 16, 20, 24, 29, 37 } },
    { STAND_MONO, { 13, 16, 20, 0, 0, 0 } },
};

static int report_as(const struct w16_fontinfo *fi, const char *face, int type, w16_font_enum_fn fn, void *ctx);

static int report(const struct w16_fontinfo *fi, const char *face, w16_font_enum_fn fn, void *ctx) {
    return report_as(fi, face, W16_RASTER_FONTTYPE, fn, ctx);
}

// The TrueType families, one each (any size draws): its regular face at 12 points.
static int report_tt(const char *face, int *n, w16_font_enum_fn fn, void *ctx) {
    tt_scan();
    for (int i = 0; i < g_ntt; i++) {
        int seen = 0;
        for (int k = 0; k < i; k++) seen |= !strcasecmp(g_tt[k].family, g_tt[i].family);
        if (seen || (face && strcasecmp(face, g_tt[i].family))) continue;
        struct w16_logfont lf = { .height = -16, .weight = 400 };
        snprintf(lf.face, sizeof lf.face, "%s", g_tt[i].family);
        const struct w16_fontinfo *fi = truetype(&lf);
        if (!fi) continue;
        (*n)++;
        if (!report_as(fi, g_tt[i].family, W16_TRUETYPE_FONTTYPE, fn, ctx)) return 0;
    }
    return 1;
}

static int report_as(const struct w16_fontinfo *fi, const char *face, int type, w16_font_enum_fn fn, void *ctx) {
    struct w16_logfont lf = { 0 };
    lf.height = (int16_t)fi->height;
    lf.width = (int16_t)fi->ave;
    lf.weight = (int16_t)fi->weight;
    lf.italic = (uint8_t)fi->italic;
    lf.charset = (uint8_t)fi->charset;
    lf.pitch_family = (uint8_t)fi->pitch_family;
    snprintf(lf.face, sizeof lf.face, "%s", face);
    return fn(&lf, fi, type, ctx);
}

int w16_font_enum(const char *face, w16_font_enum_fn fn, void *ctx) {
    int n = 0;
    if (!report_tt(face, &n, fn, ctx)) return n;
    if (g_w16_fonts == W16_FONTS_WIN311) {
        scan();
        const char *want = face ? alias(face) : 0;
        for (int i = 0; i < g_nreal; i++) {
            const struct winfnt *f = &g_real[i].f;
            if (want && strcasecmp(f->face, want)) continue;
            int seen = 0;
            for (int k = 0; !want && k < i; k++) seen |= !strcasecmp(g_real[k].f.face, f->face);
            if (seen) continue;
            struct inst *in = instance(i, 0, 0, 0);
            if (!in) continue;
            n++;
            if (!report(&in->info, f->face, fn, ctx)) return n;
        }
        if (g_nreal) return n;                       // none found: the stand-ins, as drawn
    }
    for (int s = 0; s < 3; s++) {
        if (face && strcasecmp(STAND_FACE[s], alias(face))) continue;
        for (int k = 0; k < 6 && STAND_SIZES[s].px[k]; k++) {
            int px = STAND_SIZES[s].px[k];
            struct inst *in = instance(-1, STAND_SIZES[s].standin, px * 4 / 5, 0);
            if (!in) continue;
            struct w16_fontinfo fi = in->info;
            fi.height = px;
            n++;
            if (!report(&fi, STAND_FACE[s], fn, ctx)) return n;
            if (!face) break;                        // one per face
        }
    }
    return n;
}

const struct w16_fontinfo *w16_font_info(uint16_t hfont) {
    const struct w16_fontinfo *i = of(hfont);
    static struct w16_fontinfo none;
    if (i) return i;
    // Nothing would load at all: the session's own face, so text still shows.
    none.uf = 0;
    none.height = 16;
    return &none;
}

const struct ugfx_font *w16_font_of(uint16_t hfont) {
    const struct w16_fontinfo *i = w16_font_info(hfont);
    return i->uf ? i->uf : ugfx_font_session(UGFX_FONT_BOLD);
}

int w16_font_line_h(uint16_t hfont) {
    pthread_mutex_lock(&g_w16_text_lock);
    int h = w16_font_info(hfont)->height;
    pthread_mutex_unlock(&g_w16_text_lock);
    return h > 0 ? h : 16;
}

int w16_font_ascent(const struct ugfx_font *uf) {
    for (int i = 0; i < g_ninst; i++)
        if (&g_inst[i]->uf == uf) return g_inst[i]->info.ascent;
    return uf ? uf->line_h * 13 / 16 : 13;
}

int w16_font_overhang(const struct ugfx_font *uf) {
    for (int i = 0; i < g_ninst; i++)
        if (&g_inst[i]->uf == uf) return g_inst[i]->real >= 0 && g_inst[i]->bold;
    return 0;
}

const struct ugfx_font *w16_system_font(void) { return w16_font_of(STOCK_BASE + SYSTEM_FONT); }
const struct ugfx_font *w16_fixed_font(void) { return w16_font_of(STOCK_BASE + SYSTEM_FIXED_FONT); }
const struct ugfx_font *w16_dialog_font(void) { return w16_font_of(W16_FONT_DIALOG); }

int w16_font_is_fixed(uint16_t hfont) {
    return hfont == STOCK_BASE + OEM_FIXED_FONT || hfont == STOCK_BASE + ANSI_FIXED_FONT ||
           hfont == STOCK_BASE + SYSTEM_FIXED_FONT;
}

// `w16 --fonts`: 3.11's fonts as found -- what the launcher's Options
// show, since the launcher parses no font itself. A line per font,
// "font FILE POINTS PIXELS WEIGHT<tab>FACE"; then per face a sample of its
// size nearest 13 pixels, "sample W H<tab>FACE" and H rows of W 0s and 1s.
#define SAMPLE "The quick brown fox"
void w16_fonts_list(FILE *out) {
    scan();
    for (int i = 0; i < g_nreal; i++) {
        const struct winfnt *f = &g_real[i].f;
        fprintf(out, "font %s %d %d %d\t%s\n", g_real[i].file, f->points, f->pix_height, f->weight, f->face);
    }
    for (int i = 0; i < g_nreal; i++) {
        int seen = 0;
        for (int k = 0; k < i; k++) seen |= !strcasecmp(g_real[k].f.face, g_real[i].f.face);
        if (seen) continue;
        const struct winfnt *f = &g_real[match(g_real[i].f.face, 13)].f;
        int w = 0, h = f->pix_height;
        for (const char *p = SAMPLE; *p; p++) w += winfnt_width(f, (unsigned char)*p) > 0 ? winfnt_width(f, (unsigned char)*p) : 0;
        uint8_t *px = calloc((size_t)w * (size_t)h + 1, 1), *g = calloc(256u * (size_t)h, 1);
        if (!px || !g) { free(px); free(g); return; }
        int x = 0;
        for (const char *p = SAMPLE; *p; p++) {
            int gw = winfnt_glyph(f, (unsigned char)*p, g, 256, 256);
            for (int y = 0; gw > 0 && y < h; y++)
                for (int c = 0; c < gw && x + c < w; c++) px[y * w + x + c] = g[y * 256 + c];
            x += gw > 0 ? gw : 0;
        }
        fprintf(out, "sample %d %d\t%s\n", w, h, f->face);
        for (int y = 0; y < h; y++) {
            for (int c = 0; c < w; c++) fputc(px[y * w + c] ? '1' : '0', out);
            fputc('\n', out);
        }
        free(px);
        free(g);
    }
}
