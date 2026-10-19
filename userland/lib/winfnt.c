// winfnt.c -- Windows .FON/.FNT bitmap fonts (winfnt.h). Every offset is
// checked against the bytes it indexes before it is read.
#include "lib/winfnt.h"

#include <string.h>

#define RT_FONT        0x8008                    // a resource type ID with its high bit set
#define FNT_V2_HEADER  118
#define FNT_V3_HEADER  148

static unsigned rd16(const uint8_t *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | (uint32_t)rd16(p + 2) << 16; }

// Bytes [at, at + need) lie inside n.
static int inside(size_t n, size_t at, size_t need) { return at <= n && need <= n - at; }

// An FNT's version, or 0 when the bytes cannot be one.
static int fnt_version(const uint8_t *p, size_t n) {
    if (n < FNT_V2_HEADER) return 0;
    unsigned v = rd16(p);
    return v == 0x200 || (v == 0x300 && n >= FNT_V3_HEADER) ? (int)v : 0;
}

// The NE module's RT_FONT resources: calls `each` with each one's bytes
// in turn until it returns nonzero. The resources seen, or -1 for a file
// that is no NE module.
static int each_font(const uint8_t *d, size_t n, int (*each)(void *, const uint8_t *, uint32_t), void *ctx) {
    if (n < 0x40 || d[0] != 'M' || d[1] != 'Z') return -1;
    uint32_t ne = rd32(d + 0x3C);
    if (!inside(n, ne, 0x40) || d[ne] != 'N' || d[ne + 1] != 'E') return -1;
    uint32_t rt = ne + rd16(d + ne + 0x24);
    if (!inside(n, rt, 2)) return -1;
    unsigned shift = rd16(d + rt);
    if (shift > 16) return -1;
    uint32_t at = rt + 2;
    int seen = 0;
    for (int types = 0; types < 4096; types++) {
        if (!inside(n, at, 2)) return -1;
        unsigned type = rd16(d + at);
        if (!type) return seen;
        if (!inside(n, at, 8)) return -1;
        unsigned count = rd16(d + at + 2);
        at += 8;
        if (!inside(n, at, (size_t)count * 12)) return -1;
        for (unsigned i = 0; i < count; i++, at += 12) {
            if (type != RT_FONT) continue;
            uint32_t off = (uint32_t)rd16(d + at) << shift, len = (uint32_t)rd16(d + at + 2) << shift;
            if (!inside(n, off, len)) continue;              // a resource cut short: not a font here
            seen++;
            if (each && each(ctx, d + off, len)) return seen;
        }
    }
    return -1;
}

int winfnt_count(const uint8_t *data, size_t n) {
    if (fnt_version(data, n)) return 1;
    return each_font(data, n, 0, 0);
}

struct nth { int want, at; const uint8_t *p; uint32_t len; };

static int take_nth(void *ctx, const uint8_t *p, uint32_t len) {
    struct nth *t = ctx;
    if (t->at++ != t->want) return 0;
    t->p = p;
    t->len = len;
    return 1;
}

static int parse(const uint8_t *p, uint32_t n, struct winfnt *f) {
    int v = fnt_version(p, n);
    if (!v) return -1;
    uint32_t size = rd32(p + 2);
    if (size && size < n) n = size;                    // dfSize, when it says less than the resource
    memset(f, 0, sizeof *f);
    f->fnt = p;
    f->len = n;
    f->version = v;
    f->vector = rd16(p + 66) & 1;
    f->points = (int)rd16(p + 68);
    f->ascent = (int)rd16(p + 74);
    f->internal_leading = (int)rd16(p + 76);
    f->external_leading = (int)rd16(p + 78);
    f->italic = p[80];
    f->underline = p[81];
    f->strikeout = p[82];
    f->weight = (int)rd16(p + 83);
    f->charset = p[85];
    f->pix_width = (int)rd16(p + 86);
    f->pix_height = (int)rd16(p + 88);
    f->pitch_family = p[90];
    f->avg_width = (int)rd16(p + 91);
    f->max_width = (int)rd16(p + 93);
    f->first = p[95];
    f->last = p[96];
    f->default_char = p[97];
    uint32_t face = rd32(p + 105);
    f->table = v == 0x200 ? FNT_V2_HEADER : FNT_V3_HEADER;
    f->entry = v == 0x200 ? 4 : 6;
    if (f->last < f->first || f->pix_height <= 0 || f->pix_height > 255) return -1;
    // The table holds last - first + 2 entries: the characters, then the sentinel.
    if (!inside(n, f->table, (size_t)(f->last - f->first + 2) * (size_t)f->entry)) return -1;
    for (uint32_t i = 0; face && face + i < n && i < sizeof f->face - 1 && p[face + i]; i++)
        f->face[i] = (char)p[face + i];
    return 0;
}

int winfnt_get(const uint8_t *data, size_t n, int i, struct winfnt *out) {
    if (fnt_version(data, n)) return i == 0 && n <= UINT32_MAX ? parse(data, (uint32_t)n, out) : -1;
    struct nth t = { i, 0, 0, 0 };
    if (i < 0 || each_font(data, n, take_nth, &t) < 0 || !t.p) return -1;
    return parse(t.p, t.len, out);
}

// The table entry of `ch`, falling back on the default character; -1 none.
static int slot(const struct winfnt *f, unsigned ch) {
    if ((int)ch >= f->first && (int)ch <= f->last) return (int)ch - f->first;
    int d = f->default_char;                           // 2.0 and 3.0 both count it from dfFirstChar
    return d >= 0 && d <= f->last - f->first ? d : -1;
}

int winfnt_width(const struct winfnt *f, unsigned ch) {
    int s = slot(f, ch);
    return s < 0 ? -1 : (int)rd16(f->fnt + f->table + (uint32_t)s * (uint32_t)f->entry);
}

int winfnt_glyph(const struct winfnt *f, unsigned ch, uint8_t *out, int stride, int max_w) {
    int s = slot(f, ch);
    if (f->vector || s < 0) return -1;
    const uint8_t *e = f->fnt + f->table + (uint32_t)s * (uint32_t)f->entry;
    int w = (int)rd16(e);
    uint32_t off = f->entry == 4 ? rd16(e + 2) : rd32(e + 2);
    int cols = (w + 7) / 8, h = f->pix_height;
    // Column-major: each 8-pixel-wide column's `h` bytes in turn, bit 7 leftmost.
    if (!inside(f->len, off, (size_t)cols * (size_t)h)) return -1;
    int draw = w < max_w ? w : max_w;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < draw; x++)
            out[y * stride + x] = f->fnt[off + (uint32_t)(x / 8) * (uint32_t)h + (uint32_t)y] >> (7 - x % 8) & 1 ? 255 : 0;
    return w;
}
