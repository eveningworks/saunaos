// See uhlp.h. A WinHelp 3.0/3.1 file: B+ trees, LZ77 blocks, phrase
// compression, and a paragraph format of variable-length numbers.
//
// The layout is the one Wine's winhlp32 (hlpfile.c) and the helpdeco
// documentation describe; none of this is copied from either.
#include "lib/uhlp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- bounds-checked reads ----------------------------------------------------------
//
// A read past the end gives 0 and sets `bad` on the view's owner, so a
// parser can run straight through and test once. Offsets are size_t and
// every sum is checked before it is used.

struct view { const uint8_t *p; size_t n; };

static int v_ok(struct view v, size_t off, size_t len) { return off <= v.n && len <= v.n - off; }
static unsigned v16(struct view v, size_t off) { return v_ok(v, off, 2) ? (unsigned)(v.p[off] | v.p[off + 1] << 8) : 0; }
static uint32_t v32(struct view v, size_t off) {
    return v_ok(v, off, 4) ? (uint32_t)v.p[off] | (uint32_t)v.p[off + 1] << 8 | (uint32_t)v.p[off + 2] << 16 |
                                 (uint32_t)v.p[off + 3] << 24
                           : 0;
}
static int v8(struct view v, size_t off) { return v_ok(v, off, 1) ? v.p[off] : 0; }
// The length of the NUL-terminated string at `off`, or -1 if it runs off the view.
static long v_strlen(struct view v, size_t off) {
    if (off >= v.n) return -1;
    const uint8_t *z = memchr(v.p + off, 0, v.n - off);
    return z ? (long)(z - (v.p + off)) : -1;
}
static struct view v_sub(struct view v, size_t off, size_t len) {
    struct view r = { 0, 0 };
    if (v_ok(v, off, len)) { r.p = v.p + off; r.n = len; }
    return r;
}

// A cursor over a paragraph's formatting, where reads that run out set `bad`.
struct cur { struct view v; size_t at; int bad; };

static int cu8(struct cur *c) {
    if (!v_ok(c->v, c->at, 1)) { c->bad = 1; return 0; }
    return c->v.p[c->at++];
}
static unsigned cu16(struct cur *c) {
    if (!v_ok(c->v, c->at, 2)) { c->bad = 1; c->at = c->v.n; return 0; }
    unsigned r = v16(c->v, c->at);
    c->at += 2;
    return r;
}
static uint32_t cu32(struct cur *c) {
    if (!v_ok(c->v, c->at, 4)) { c->bad = 1; c->at = c->v.n; return 0; }
    uint32_t r = v32(c->v, c->at);
    c->at += 4;
    return r;
}
// The file's compressed numbers: the low bit says one unit wider, the
// rest is the value times two (signed ones are biased).
static int32_t c_long(struct cur *c) {
    if (c->at >= c->v.n) { c->bad = 1; return 0; }
    if (c->v.p[c->at] & 1) return (int32_t)(((int64_t)cu32(c) - 0x80000000LL) / 2);
    return (int32_t)(((int)cu16(c) - 0x8000) / 2);
}
static uint32_t c_ulong(struct cur *c) {
    if (c->at >= c->v.n) { c->bad = 1; return 0; }
    if (c->v.p[c->at] & 1) return cu32(c) / 2;
    return cu16(c) / 2;
}
static int c_short(struct cur *c) {
    if (c->at >= c->v.n) { c->bad = 1; return 0; }
    if (c->v.p[c->at] & 1) return ((int)cu16(c) - 0x8000) / 2;
    return ((int)cu8(c) - 0x80) / 2;
}
static unsigned c_ushort(struct cur *c) {
    if (c->at >= c->v.n) { c->bad = 1; return 0; }
    if (c->v.p[c->at] & 1) return cu16(c) / 2;
    return (unsigned)cu8(c) / 2;
}

// --- growing arrays -------------------------------------------------------------------

// tolibc has no strnlen.
static size_t hlp_strnlen(const char *s, size_t max) {
    const char *z = memchr(s, 0, max);
    return z ? (size_t)(z - s) : max;
}

static int grow(void **arr, int *cap_or_n, int n, size_t elt) {
    // `cap_or_n` here is the capacity; n the count in use.
    if (n < *cap_or_n) return 1;
    int nc = *cap_or_n ? *cap_or_n * 2 : 16;
    if ((size_t)nc > (size_t)0x7fffffff / elt) return 0;
    void *p = realloc(*arr, (size_t)nc * elt);
    if (!p) return 0;
    *arr = p;
    *cap_or_n = nc;
    return 1;
}

// --- LZ77, RLE and phrases -----------------------------------------------------------------

// The size LZ77 data expands to, or -1 for a back-reference that reaches
// before the start. A pass of its own so the output is allocated to fit.
static long lz77_size(struct view v) {
    size_t i = 0;
    long out = 0;
    while (i < v.n) {
        int mask = v.p[i++];
        for (int b = 0; b < 8 && i < v.n; b++, mask >>= 1) {
            if (mask & 1) {
                if (i + 2 > v.n) return out;
                unsigned code = v16(v, i);
                unsigned len = 3 + (code >> 12), dist = (code & 0xfff) + 1;
                if ((long)dist > out) return -1;
                out += len;
                i += 2;
            } else {
                out++;
                i++;
            }
        }
    }
    return out;
}

// `out` is `cap` bytes, the size lz77_size() said. Returns the bytes written.
static size_t lz77_expand(struct view v, uint8_t *out, size_t cap) {
    size_t i = 0, o = 0;
    while (i < v.n) {
        int mask = v.p[i++];
        for (int b = 0; b < 8 && i < v.n; b++, mask >>= 1) {
            if (mask & 1) {
                if (i + 2 > v.n) return o;
                unsigned code = v16(v, i);
                unsigned len = 3 + (code >> 12), dist = (code & 0xfff) + 1;
                i += 2;
                if (dist > o || o + len > cap) return o;
                for (unsigned k = 0; k < len; k++, o++) out[o] = out[o - dist];   // overlapping, byte by byte
            } else {
                if (o >= cap) return o;
                out[o++] = v.p[i++];
            }
        }
    }
    return o;
}

// Run-length data: a byte with the top bit set copies the next n bytes,
// otherwise it repeats the next byte n times. Writes at most `cap`.
static size_t rle_expand(struct view v, uint8_t *out, size_t cap) {
    size_t i = 0, o = 0;
    while (i < v.n) {
        unsigned n = v.p[i++];
        if (n & 0x80) {
            n &= 0x7f;
            if (!v_ok(v, i, n) || o + n > cap) break;
            memcpy(out + o, v.p + i, n);
            i += n;
        } else {
            if (i >= v.n || o + n > cap) break;
            memset(out + o, v.p[i++], n);
        }
        o += n;
    }
    return o;
}

// --- the opened file -----------------------------------------------------------------------

struct page {
    uint32_t ref;          // the topic header's record position (0xC + block<<14 + offset)
    uint32_t offset;       // TOPICPOS, what a jump names
    char *title;
    uint32_t bwd, fwd;     // browse sequence, as topic offsets or 0xFFFFFFFF
    uint32_t number;
    uint32_t nonscroll, scroll;   // TOPICPOS of the two regions, 0xFFFFFFFF when none
    char **macro; int nmacro;
};

struct keyword { char *text; int first, count; };

struct uhlp {
    uint8_t *file;
    size_t flen;

    int minor, flags, compressed;
    unsigned tbsize, dsize;
    char *title, *copyright;
    uint32_t contents_start;
    int charset;
    char **macro; int nmacro, macro_cap;
    struct uhlp_window *win; int nwin, win_cap;

    struct uhlp_font *font; int nfont;

    // phrases: n+1 offsets into one buffer
    uint32_t *ph_off; int nph; uint8_t *ph_buf; size_t ph_len;
    int have_phrases, have_phrases40;

    // the topic stream, one decompressed block each
    uint8_t *tbuf; struct view *blk; int nblk;

    struct page *page; int npage, page_cap;

    uint32_t *ctx_hash, *ctx_off; int nctx;
    uint32_t *map_id, *map_off; int nmap;
    uint32_t *tomap; int ntomap;

    struct keyword *kw; int nkw;
    int *kw_topic; int nkw_topic;     // every keyword's topics, one run each
};

// --- the internal directory and its B+ trees ----------------------------------------------

struct bt {
    struct view v;           // the tree: header, then pages
    unsigned ps;
    int levels, pages;
    unsigned root;
};

static int bt_open(struct bt *t, struct view v) {
    if (v16(v, 0) != 0x293B) return 0;
    t->v = v;
    t->ps = v16(v, 4);
    t->root = v16(v, 26);
    t->levels = (int)v16(v, 32);
    t->pages = (int)v16(v, 30);
    if (t->ps < 16 || t->levels < 1 || t->levels > 16) return 0;
    return 1;
}

// A leaf entry's shape.
#define BT_STR_U32 0     // NUL-terminated key, 4 bytes
#define BT_U32_U32 1     // 4-byte key, 4 bytes
#define BT_STR_6   2     // NUL-terminated key, 6 bytes

struct bt_it {
    struct bt t;
    int kind;
    long page;           // the leaf being read, -1 done
    size_t off;          // the next entry in it
    int left;
    int seen;
};

static int bt_page_ok(const struct bt *t, unsigned page) {
    return (size_t)38 + ((size_t)page + 1) * t->ps <= t->v.n;
}

static int bt_begin(struct bt_it *it, struct view v, int kind) {
    memset(it, 0, sizeof *it);
    if (!bt_open(&it->t, v)) return 0;
    it->kind = kind;
    unsigned cur = it->t.root;
    for (int l = it->t.levels; l > 1; l--) {
        if (!bt_page_ok(&it->t, cur)) return 0;
        cur = v16(v, 38 + (size_t)cur * it->t.ps + 4);   // an index page's first child
    }
    it->page = (long)cur;
    it->left = 0;
    return 1;
}

// The next leaf entry: key (and its length), value. 0 at the end or on damage.
static int bt_next(struct bt_it *it, const uint8_t **key, int *klen, const uint8_t **val) {
    for (;;) {
        if (it->page < 0 || it->page == 0xFFFF) return 0;
        size_t base = 38 + (size_t)it->page * it->t.ps;
        if (it->left == 0 && it->off == 0) {      // a fresh page
            if (!bt_page_ok(&it->t, (unsigned)it->page) || ++it->seen > it->t.pages + 1) return 0;
            it->left = (int)v16(it->t.v, base + 2);
            it->off = base + 8;
        }
        if (it->left > 0) {
            size_t end = base + it->t.ps;
            size_t at = it->off;
            size_t kl, vl;
            if (it->kind == BT_U32_U32) { kl = 4; vl = 4; }
            else {
                long sl = v_strlen(it->t.v, at);
                if (sl < 0 || at + (size_t)sl + 1 > end) return 0;
                kl = (size_t)sl;
                vl = it->kind == BT_STR_6 ? 6 : 4;
                if (at + kl + 1 + vl > end) return 0;
            }
            size_t total = kl + (it->kind == BT_U32_U32 ? 0 : 1) + vl;
            if (at + total > end) return 0;
            *key = it->t.v.p + at;
            *klen = (int)kl;
            *val = it->t.v.p + at + kl + (it->kind == BT_U32_U32 ? 0 : 1);
            it->off = at + total;
            it->left--;
            return 1;
        }
        it->page = (long)v16(it->t.v, base + 6);   // the next leaf
        it->off = 0;
        it->left = 0;
    }
}

// The sub-file called `name`: its data (after the 9-byte header), UsedSpace long.
static int find_sub(const struct uhlp *h, const char *name, struct view *out, size_t *reserved) {
    struct view f = { h->file, h->flen };
    uint32_t dstart = v32(f, 4);
    if (!v_ok(f, dstart, 9)) return 0;
    struct view dir = v_sub(f, (size_t)dstart + 9, h->flen - ((size_t)dstart + 9));
    struct bt_it it;
    if (!bt_begin(&it, dir, BT_STR_U32)) return 0;
    const uint8_t *k, *val;
    int kl;
    size_t nl = strlen(name);
    while (bt_next(&it, &k, &kl, &val)) {
        if ((size_t)kl == nl && memcmp(k, name, nl) == 0) {
            uint32_t off = (uint32_t)val[0] | (uint32_t)val[1] << 8 | (uint32_t)val[2] << 16 | (uint32_t)val[3] << 24;
            if (!v_ok(f, off, 9)) return 0;
            uint32_t res = v32(f, off), used = v32(f, (size_t)off + 4);
            size_t avail = h->flen - ((size_t)off + 9);
            if (used > avail) used = (uint32_t)avail;
            out->p = f.p + off + 9;
            out->n = used;
            if (reserved) *reserved = res >= 9 && res - 9 <= avail ? res - 9 : used;
            return 1;
        }
    }
    // Bitmaps are sometimes stored without the leading '|'.
    if (name[0] == '|') return find_sub(h, name + 1, out, reserved);
    return 0;
}

// --- hash, strings -----------------------------------------------------------------------------

uint32_t uhlp_hash(const char *s) {
    uint32_t hash = 0;
    for (; *s; s++) {
        unsigned c = (unsigned char)*s, x = 0;
        if (c >= 'A' && c <= 'Z') x = c - 'A' + 17;
        else if (c >= 'a' && c <= 'z') x = c - 'a' + 17;
        else if (c >= '1' && c <= '9') x = c - '0';
        else if (c == '0') x = 10;
        else if (c == '.') x = 12;
        else if (c == '_') x = 13;
        if (x) hash = hash * 43 + x;
    }
    return hash;
}

static int topic_at_offset(const struct uhlp *h, uint32_t offset, uint32_t *rel);

uint32_t uhlp_topic_hash(const struct uhlp *h, int topic) {
    if (!h || topic < 0 || topic >= h->npage) return 0;
    for (int i = 0; i < h->nctx; i++) {
        int t = topic_at_offset(h, h->ctx_off[i], 0);
        if (t == topic && h->ctx_off[i] == h->page[topic].offset) return h->ctx_hash[i];
    }
    for (int i = 0; i < h->nctx; i++)                     // a hash that lands inside the topic will do
        if (topic_at_offset(h, h->ctx_off[i], 0) == topic) return h->ctx_hash[i];
    return 0;
}

// A letter's value in the hash: 'a'..'z' are 17..42, and each step is h = h * 43 + value, mod 2^32.
// Eight letters are found by MEETING IN THE MIDDLE: every four-letter prefix's hash, sorted, against
// every four-letter suffix's contribution -- about 26^4 of each, which together reach a given
// 32-bit value some fifty times over.
static uint32_t hl_inv43(void) {                          // 43^-1 mod 2^32, by Newton's iteration
    uint32_t x = 43;
    for (int i = 0; i < 5; i++) x *= 2 - 43 * x;
    return x;
}

struct hl_pre { uint32_t hash; uint32_t idx; };

static int hl_cmp(const void *a, const void *b) {
    uint32_t x = ((const struct hl_pre *)a)->hash, y = ((const struct hl_pre *)b)->hash;
    return x < y ? -1 : x > y;
}

static void hl_letters(uint32_t idx, char out[4]) {
    for (int i = 3; i >= 0; i--) { out[i] = (char)('a' + idx % 26); idx /= 26; }
}

int uhlp_id_for_hash(uint32_t hash, char *out, size_t cap) {
    if (cap < 9) return 0;
    const uint32_t n = 26u * 26 * 26 * 26;
    struct hl_pre *pre = malloc((size_t)n * sizeof *pre);
    if (!pre) return 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t hs = 0, v = i;
        char l[4];
        hl_letters(v, l);
        for (int k = 0; k < 4; k++) hs = hs * 43 + (uint32_t)(l[k] - 'a' + 17);
        pre[i].hash = hs;
        pre[i].idx = i;
    }
    qsort(pre, n, sizeof *pre, hl_cmp);
    uint32_t p4 = 43u * 43 * 43 * 43;                     // 43^4: a prefix's weight
    uint32_t inv4 = hl_inv43();
    inv4 = inv4 * inv4 * inv4 * inv4;
    int found = 0;
    for (uint32_t sidx = 0; sidx < n && !found; sidx++) {
        char suf[4];
        hl_letters(sidx, suf);
        uint32_t c = 0;
        for (int k = 0; k < 4; k++) c = c * 43 + (uint32_t)(suf[k] - 'a' + 17);
        uint32_t need = (hash - c) * inv4;               // the prefix hash that continues to `hash`
        size_t lo = 0, hi = n;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (pre[mid].hash < need) lo = mid + 1; else hi = mid;
        }
        if (lo < n && pre[lo].hash == need) {
            char l[4];
            hl_letters(pre[lo].idx, l);
            memcpy(out, l, 4);
            memcpy(out + 4, suf, 4);
            out[8] = 0;
            found = 1;
        }
    }
    free(pre);
    (void)p4;
    return found && uhlp_hash(out) == hash;
}

static char *dup_n(const uint8_t *p, size_t n) {
    char *s = malloc(n + 1);
    if (!s) return 0;
    memcpy(s, p, n);
    s[n] = 0;
    return s;
}

// --- |SYSTEM -------------------------------------------------------------------------------------

static int read_system(struct uhlp *h, const char **err) {
    struct view s;
    if (!find_sub(h, "|SYSTEM", &s, 0)) { *err = "no |SYSTEM"; return 0; }
    if (v16(s, 0) != 0x036C || v16(s, 4) != 1) { *err = "not a WinHelp 3.x system header"; return 0; }
    h->minor = (int)v16(s, 2);
    h->flags = (int)v16(s, 10);
    if (h->minor <= 16) { h->tbsize = 0x800; h->compressed = 0; }
    else if (h->flags == 0) { h->tbsize = 0x1000; h->compressed = 0; }
    else if (h->flags == 4) { h->tbsize = 0x1000; h->compressed = 1; }
    else { h->tbsize = 0x800; h->compressed = 1; }
    h->dsize = h->compressed ? 0x4000 : h->tbsize - 0x0C;
    h->contents_start = 0xFFFFFFFF;

    if (h->minor <= 16) {
        long n = v_strlen(s, 12);
        h->title = n >= 0 ? dup_n(s.p + 12, (size_t)n) : strdup("");
        return h->title != 0;
    }
    size_t at = 12;
    while (at + 4 <= s.n) {
        unsigned type = v16(s, at), len = v16(s, at + 2);
        size_t d = at + 4;
        if (!v_ok(s, d, len)) break;
        long sl = v_strlen(v_sub(s, d, len), 0);
        const uint8_t *str = s.p + d;
        switch (type) {
        case 1:
            if (!h->title && sl >= 0) h->title = dup_n(str, (size_t)sl);
            break;
        case 2:
            if (!h->copyright && sl >= 0) h->copyright = dup_n(str, (size_t)sl);
            break;
        case 3:
            if (len == 4) h->contents_start = v32(s, d);
            break;
        case 4:
            if (sl >= 0 && grow((void **)&h->macro, &h->macro_cap, h->nmacro, sizeof *h->macro)) {
                char *m = dup_n(str, (size_t)sl);
                if (m) h->macro[h->nmacro++] = m;
            }
            break;
        case 6:
            if (len == 90 && grow((void **)&h->win, &h->win_cap, h->nwin, sizeof *h->win)) {
                struct uhlp_window *w = &h->win[h->nwin++];
                memset(w, 0, sizeof *w);
                unsigned fl = v16(s, d);
                struct view rec = v_sub(s, d, len);
                (void)rec;
                if (fl & 2) snprintf(w->name, sizeof w->name, "%.*s", (int)(v_strlen(s, d + 10) < 0 ? 0 : v_strlen(s, d + 10)), (const char *)s.p + d + 10);
                if (fl & 4) snprintf(w->caption, sizeof w->caption, "%.*s", (int)(v_strlen(s, d + 19) < 0 ? 0 : v_strlen(s, d + 19)), (const char *)s.p + d + 19);
                w->x = fl & 8 ? (int)v16(s, d + 72) : -1;
                w->y = fl & 0x10 ? (int)v16(s, d + 74) : -1;
                w->w = fl & 0x20 ? (int)v16(s, d + 76) : -1;
                w->h = fl & 0x40 ? (int)v16(s, d + 78) : -1;
                w->sr_rgb = fl & 0x100 ? v32(s, d + 82) : 0xFFFFFFFFu;
                w->nsr_rgb = fl & 0x200 ? v32(s, d + 86) : 0xFFFFFFFFu;
            }
            break;
        case 11:
            h->charset = v8(s, d);
            break;
        default: break;
        }
        at = d + len;
    }
    if (!h->title) h->title = strdup("");
    return h->title != 0;
}

// --- phrases ----------------------------------------------------------------------------------------

static int read_phrases(struct uhlp *h) {
    struct view p;
    if (find_sub(h, "|Phrases", &p, 0)) {
        unsigned num = v16(p, 0);
        size_t head = h->minor <= 16 ? 4 : 8;
        size_t tab = head + 2 * ((size_t)num + 1);
        if (!v_ok(p, 0, tab)) return 0;
        size_t dstart = h->minor <= 16 ? 6 + 2 * (size_t)num : 10 + 2 * (size_t)num;
        if (dstart > p.n) return 0;
        struct view data = v_sub(p, dstart, p.n - dstart);
        size_t dec;
        uint8_t *buf;
        if (h->minor <= 16) {
            dec = data.n;
            buf = malloc(dec ? dec : 1);
            if (!buf) return 0;
            memcpy(buf, data.p, dec);
        } else {
            long sz = lz77_size(data);
            if (sz < 0) return 0;
            buf = malloc((size_t)sz + 1);
            if (!buf) return 0;
            dec = lz77_expand(data, buf, (size_t)sz);
        }
        uint32_t *off = malloc(((size_t)num + 1) * sizeof *off);
        if (!off) { free(buf); return 0; }
        uint32_t prev = 0;
        for (unsigned i = 0; i <= num; i++) {
            long o = (long)v16(p, head + 2 * (size_t)i) - 2 * (long)num - 2;
            if (o < (long)prev || (size_t)o > dec) { free(off); free(buf); return 0; }
            off[i] = (uint32_t)o;
            prev = (uint32_t)o;
        }
        h->ph_off = off; h->nph = (int)num; h->ph_buf = buf; h->ph_len = dec;
        h->have_phrases = 1;
        return 1;
    }
    struct view idx, img;
    if (!find_sub(h, "|PhrIndex", &idx, 0) || !find_sub(h, "|PhrImage", &img, 0)) return 0;
    unsigned num = v32(idx, 4) & 0xffff;
    unsigned bc = v16(idx, 24) & 0x0f;
    uint32_t dec_size = v32(idx, 12), cpr_size = v32(idx, 16);
    uint8_t *buf;
    size_t dec;
    if (dec_size == cpr_size) {
        if (!v_ok(img, 0, dec_size)) return 0;
        buf = malloc(dec_size ? dec_size : 1);
        if (!buf) return 0;
        memcpy(buf, img.p, dec_size);
        dec = dec_size;
    } else {
        long sz = lz77_size(img);
        if (sz < 0) return 0;
        buf = malloc((size_t)sz + 1);
        if (!buf) return 0;
        dec = lz77_expand(img, buf, (size_t)sz);
    }
    uint32_t *off = malloc(((size_t)num + 1) * sizeof *off);
    if (!off) { free(buf); return 0; }
    // The lengths are a bit stream after the 28-byte header: a unary run, then up to bc more bits.
    size_t bit = 0, nbits = idx.n > 28 ? (idx.n - 28) * 8 : 0;
#define GETBIT() (bit < nbits ? (int)((idx.p[28 + bit / 8] >> (bit % 8)) & 1) : 0)
    off[0] = 0;
    for (unsigned i = 0; i < num; i++) {
        unsigned n = 1;
        while (bit < nbits) {
            int b = GETBIT();
            bit++;
            if (!b) break;
            n += 1u << bc;
        }
        if (bit < nbits) { if (GETBIT()) n++; bit++; }
        for (unsigned k = 1; k < 5 && bc > k; k++) {
            if (bit < nbits) { if (GETBIT()) n += 1u << k; bit++; }
        }
        off[i + 1] = off[i] + n;
        if (off[i + 1] > dec) { free(off); free(buf); return 0; }
    }
#undef GETBIT
    h->ph_off = off; h->nph = (int)num; h->ph_buf = buf; h->ph_len = dec;
    h->have_phrases40 = 1;
    return 1;
}

// Text as stored -> plain bytes. `in` is the stored LinkData2, `want` the
// size it expands to. Returns a malloc'd buffer of exactly `want` bytes.
static uint8_t *expand_text(const struct uhlp *h, struct view in, size_t want, size_t stored_cap) {
    uint8_t *out = calloc(want + 1, 1);
    if (!out) return 0;
    if (want <= stored_cap) {
        memcpy(out, in.p, want <= in.n ? want : in.n);
        return out;
    }
    size_t o = 0, i = 0;
    if (h->have_phrases) {
        while (i < in.n && o < want) {
            unsigned c = in.p[i];
            if (c == 0 || c >= 0x10) { out[o++] = (uint8_t)c; i++; continue; }
            if (i + 2 > in.n) break;
            unsigned code = c << 8 | in.p[i + 1];
            unsigned pi = (code - 0x100) / 2;
            if ((int)pi >= h->nph) break;
            size_t len = h->ph_off[pi + 1] - h->ph_off[pi];
            if (o + len > want) break;
            memcpy(out + o, h->ph_buf + h->ph_off[pi], len);
            o += len;
            if (code & 1 && o < want) out[o++] = ' ';
            i += 2;
        }
    } else if (h->have_phrases40) {
        while (i < in.n && o < want) {
            unsigned c = in.p[i];
            size_t len = 0;
            if ((c & 1) == 0) {
                unsigned pi = c / 2;
                if ((int)pi >= h->nph) { i++; continue; }
                len = h->ph_off[pi + 1] - h->ph_off[pi];
                if (o + len > want) break;
                memcpy(out + o, h->ph_buf + h->ph_off[pi], len);
            } else if ((c & 3) == 1) {
                if (i + 1 >= in.n) break;
                unsigned pi = (c + 1) * 64 + in.p[i + 1];
                i++;
                if ((int)pi >= h->nph) { i++; continue; }
                len = h->ph_off[pi + 1] - h->ph_off[pi];
                if (o + len > want) break;
                memcpy(out + o, h->ph_buf + h->ph_off[pi], len);
            } else if ((c & 7) == 3) {
                len = c / 8 + 1;
                if (i + 1 + len > in.n || o + len > want) break;
                memcpy(out + o, in.p + i + 1, len);
                i += len;
            } else {
                len = c / 16 + 1;
                if (o + len > want) break;
                memset(out + o, (c & 0x0f) == 7 ? ' ' : 0, len);
            }
            o += len;
            i++;
        }
    } else {
        memcpy(out, in.p, in.n < want ? in.n : want);   // no phrase table: take what is stored
    }
    return out;
}

// --- the topic stream --------------------------------------------------------------------------------

static int read_topic_blocks(struct uhlp *h, const char **err) {
    struct view t;
    size_t reserved;
    if (!find_sub(h, "|TOPIC", &t, &reserved)) { *err = "no |TOPIC"; return 0; }
    if (reserved > t.n) t.n = reserved <= h->flen - (size_t)(t.p - h->file) ? reserved : t.n;   // Windows reads the reserved span
    size_t tb = h->tbsize;
    if (t.n == 0) { *err = "empty |TOPIC"; return 0; }
    int nb = (int)((t.n - 1) / tb + 1);
    h->blk = calloc((size_t)nb, sizeof *h->blk);
    if (!h->blk) return 0;
    h->nblk = nb;
    if (h->compressed) {
        long total = 0;
        long *sz = calloc((size_t)nb, sizeof *sz);
        if (!sz) return 0;
        for (int i = 0; i < nb; i++) {
            size_t off = (size_t)i * tb;
            size_t len = t.n - off < tb ? t.n - off : tb;
            if (len < 0x0c) { sz[i] = 0; continue; }
            long s = lz77_size(v_sub(t, off + 0x0c, len - 0x0c));
            if (s < 0) { free(sz); *err = "damaged topic block"; return 0; }
            sz[i] = s;
            total += s;
        }
        h->tbuf = malloc((size_t)total + 1);
        if (!h->tbuf) { free(sz); return 0; }
        size_t o = 0;
        for (int i = 0; i < nb; i++) {
            size_t off = (size_t)i * tb;
            size_t len = t.n - off < tb ? t.n - off : tb;
            h->blk[i].p = h->tbuf + o;
            if (len >= 0x0c) h->blk[i].n = lz77_expand(v_sub(t, off + 0x0c, len - 0x0c), h->tbuf + o, (size_t)sz[i]);
            o += (size_t)sz[i];
        }
        free(sz);
    } else {
        h->tbuf = malloc((size_t)nb * h->dsize + 1);
        if (!h->tbuf) return 0;
        for (int i = 0; i < nb; i++) {
            size_t off = (size_t)i * tb;
            size_t len = t.n - off < tb ? t.n - off : tb;
            size_t n = len > 0x0c ? len - 0x0c : 0;
            if (n > h->dsize) n = h->dsize;
            h->blk[i].p = h->tbuf + (size_t)i * h->dsize;
            h->blk[i].n = n;
            if (n) memcpy(h->tbuf + (size_t)i * h->dsize, t.p + off + 0x0c, n);
        }
    }
    return 1;
}

// A topic record at `ref`: its bytes (the whole record), or an empty view.
static struct view record_at(const struct uhlp *h, uint32_t ref, unsigned *index_out) {
    struct view r = { 0, 0 };
    if (ref < 0x0c || ref == 0xFFFFFFFFu) return r;
    uint32_t idx, off;
    if (h->minor <= 16) {
        idx = (ref - 0x0c) / h->dsize;
        off = (ref - 0x0c) % h->dsize;
    } else {
        idx = (ref - 0x0c) >> 14;
        off = (ref - 0x0c) & 0x3fff;
    }
    if ((int)idx >= h->nblk || idx > 0x7fffffff) return r;
    struct view b = h->blk[idx];
    if (!v_ok(b, off, 0x15)) return r;
    if (index_out) *index_out = idx;
    uint32_t sz = v32(b, off);
    size_t n = sz < 0x15 || sz > b.n - off ? b.n - off : sz;
    r.p = b.p + off;
    r.n = n;
    return r;
}

#define REC_DISPLAY30 0x01
#define REC_TOPICHDR  0x02
#define REC_DISPLAY   0x20
#define REC_TABLE     0x23

// How far a display paragraph advances TOPICPOS, and the offset of its text.
static int para_len(struct view rec, unsigned *len) {
    struct cur c = { rec, 0x15, 0 };
    int type = v8(rec, 0x14);
    if (type == REC_DISPLAY || type == REC_TABLE) {
        c_long(&c);
        *len = c_ushort(&c);
    } else {
        *len = (unsigned)(rec.n > 15 ? rec.n - 15 : 0);
    }
    return !c.bad;
}

static int add_page(struct uhlp *h, struct view rec, uint32_t ref, uint32_t offset) {
    if (!grow((void **)&h->page, &h->page_cap, h->npage, sizeof *h->page)) return 0;
    uint32_t blocksize = v32(rec, 0), datalen = v32(rec, 0x10), tsize = v32(rec, 4);
    if (datalen > rec.n || datalen < 0x15) datalen = (uint32_t)(rec.n < 0x15 ? rec.n : 0x15);
    struct page *pg = &h->page[h->npage];
    memset(pg, 0, sizeof *pg);
    struct view title_in = v_sub(rec, datalen, rec.n - datalen);
    size_t stored_cap = blocksize > datalen ? blocksize - datalen : 0;
    if (tsize > 0x10000) tsize = 0x10000;
    uint8_t *t = expand_text(h, title_in, tsize, stored_cap);
    if (!t) return 0;
    t[tsize] = 0;
    pg->title = (char *)t;
    pg->ref = ref;
    pg->offset = offset;
    pg->number = v32(rec, 0x21);
    pg->bwd = v32(rec, 0x19);
    pg->fwd = v32(rec, 0x1d);
    pg->nonscroll = v32(rec, 0x25);
    pg->scroll = v32(rec, 0x29);
    if (h->minor <= 16) {
        pg->bwd = (pg->bwd == 0xFFFF || pg->bwd == 0xFFFFFFFFu || (int)pg->bwd >= h->ntomap) ? 0xFFFFFFFFu : h->tomap[pg->bwd];
        pg->fwd = (pg->fwd == 0xFFFF || pg->fwd == 0xFFFFFFFFu || (int)pg->fwd >= h->ntomap) ? 0xFFFFFFFFu : h->tomap[pg->fwd];
    }
    // The topic's own macros follow its title, one NUL-terminated string each.
    size_t tl = strlen(pg->title);
    for (size_t at = tl + 1; at < tsize;) {
        size_t ml = hlp_strnlen((char *)t + at, tsize - at);
        if (ml && (pg->nmacro % 8) == 0) {
            char **m = realloc(pg->macro, (size_t)(pg->nmacro + 8) * sizeof *m);
            if (!m) break;
            pg->macro = m;
        }
        if (ml) pg->macro[pg->nmacro++] = dup_n(t + at, ml);
        at += ml + 1;
    }
    h->npage++;
    return 1;
}

static int walk_topics(struct uhlp *h) {
    uint32_t ref = 0x0c, offs = 0;
    unsigned old_index = 0xffffffffu;
    int last_was_header = 0;
    size_t guard = 0, max_records = h->nblk ? (size_t)h->nblk * 0x4000 / 0x15 + 16 : 16;
    do {
        unsigned index = 0;
        struct view rec = record_at(h, ref, &index);
        if (!rec.p) break;
        if (index != old_index) { offs = 0; old_index = index; }
        int type = v8(rec, 0x14);
        uint32_t next = v32(rec, 0x0c);
        last_was_header = type == REC_TOPICHDR;
        switch (type) {
        case REC_TOPICHDR: {
            uint32_t pos = h->minor <= 16 ? ref + index * 12 : index * 0x8000u + offs;
            if (!add_page(h, rec, ref, pos)) return 0;
            break;
        }
        case REC_DISPLAY30: case REC_DISPLAY: case REC_TABLE: {
            if (!h->npage) break;
            unsigned len;
            if (!para_len(rec, &len)) return 1;
            offs += len;
            break;
        }
        default: break;
        }
        if (h->minor <= 16) {
            if (next == 0) break;
            ref += next;
        } else {
            if (next != 0xFFFFFFFFu && next <= ref) break;   // a chain only goes forward
            ref = next;
        }
        if (++guard > max_records) break;
    } while (ref != 0xFFFFFFFFu);
    // The compiler ends the stream with a topic header that has no text: not a topic.
    if (last_was_header && h->npage > 1 && h->page[h->npage - 1].title[0] == 0) {
        struct page *pg = &h->page[--h->npage];
        free(pg->title);
        for (int m = 0; m < pg->nmacro; m++) free(pg->macro[m]);
        free(pg->macro);
    }
    return 1;
}

// --- fonts ------------------------------------------------------------------------------------------------

static void read_fonts(struct uhlp *h) {
    struct view f;
    if (!find_sub(h, "|FONT", &f, 0)) return;
    unsigned face_num = v16(f, 0), dscr_num = v16(f, 2), face_off = v16(f, 4), dscr_off = v16(f, 6);
    if (!dscr_num || !face_num || dscr_off <= face_off) return;
    size_t flen = (dscr_off - face_off) / face_num;
    h->font = calloc(dscr_num, sizeof *h->font);
    if (!h->font) return;
    h->nfont = (int)dscr_num;
    int old = face_off < 12;                 // the 3.1 layout: 11-byte descriptors
    for (unsigned i = 0; i < dscr_num; i++) {
        struct uhlp_font *o = &h->font[i];
        snprintf(o->face, sizeof o->face, "Helv");
        o->half_pts = 20;
        o->rgb = 0;
        if (!old) continue;
        size_t d = dscr_off + (size_t)i * 11;
        if (!v_ok(f, d, 11)) continue;
        o->flags = f.p[d];
        o->half_pts = f.p[d + 1];
        o->family = f.p[d + 2];
        unsigned idx = v16(f, d + 3);
        o->rgb = (uint32_t)f.p[d + 5] << 16 | (uint32_t)f.p[d + 6] << 8 | f.p[d + 7];
        if (idx < face_num && flen) {
            size_t n = flen < sizeof o->face - 1 ? flen : sizeof o->face - 1;
            size_t fo = face_off + (size_t)idx * flen;
            if (v_ok(f, fo, n)) {
                memcpy(o->face, f.p + fo, n);
                o->face[n] = 0;
            }
        }
        if (o->half_pts < 6) o->half_pts = 6;
    }
}

// --- the lookup tables ------------------------------------------------------------------------------------------

static void read_tables(struct uhlp *h) {
    struct view v;
    struct bt_it it;
    const uint8_t *k, *val;
    int kl;
    if (h->minor <= 16 && find_sub(h, "|TOMAP", &v, 0)) {
        h->ntomap = (int)(v.n / 4);
        h->tomap = malloc((size_t)(h->ntomap ? h->ntomap : 1) * sizeof *h->tomap);
        if (h->tomap) for (int i = 0; i < h->ntomap; i++) h->tomap[i] = v32(v, (size_t)i * 4);
        else h->ntomap = 0;
    }
    if (find_sub(h, "|CTXOMAP", &v, 0)) {
        int n = (int)v16(v, 0);
        if ((size_t)n * 8 + 2 > v.n) n = (int)((v.n > 2 ? v.n - 2 : 0) / 8);
        h->map_id = malloc((size_t)(n ? n : 1) * sizeof *h->map_id);
        h->map_off = malloc((size_t)(n ? n : 1) * sizeof *h->map_off);
        if (h->map_id && h->map_off) {
            h->nmap = n;
            for (int i = 0; i < n; i++) {
                h->map_id[i] = v32(v, 2 + (size_t)i * 8);
                h->map_off[i] = v32(v, 6 + (size_t)i * 8);
            }
        }
    }
    if (h->minor > 16 && find_sub(h, "|CONTEXT", &v, 0) && bt_begin(&it, v, BT_U32_U32)) {
        int cap = 0, n = 0;
        uint32_t *hs = 0, *of = 0;
        while (bt_next(&it, &k, &kl, &val)) {
            if (n == cap) {
                int nc = cap ? cap * 2 : 64;
                uint32_t *a = realloc(hs, (size_t)nc * sizeof *a), *b = realloc(of, (size_t)nc * sizeof *b);
                if (a) hs = a;
                if (b) of = b;
                if (!a || !b) break;
                cap = nc;
            }
            hs[n] = (uint32_t)k[0] | (uint32_t)k[1] << 8 | (uint32_t)k[2] << 16 | (uint32_t)k[3] << 24;
            of[n] = (uint32_t)val[0] | (uint32_t)val[1] << 8 | (uint32_t)val[2] << 16 | (uint32_t)val[3] << 24;
            n++;
        }
        h->ctx_hash = hs; h->ctx_off = of; h->nctx = n;
    }
}

static int topic_at_offset(const struct uhlp *h, uint32_t offset, uint32_t *rel) {
    int best = -1;
    for (int i = 0; i < h->npage; i++)
        if (h->page[i].offset <= offset && (best < 0 || h->page[best].offset < h->page[i].offset)) best = i;
    if (best >= 0 && rel) *rel = offset - h->page[best].offset;
    return best;
}

static void read_keywords(struct uhlp *h) {
    struct view bt, data;
    struct bt_it it;
    if (!find_sub(h, "|KWBTREE", &bt, 0) || !find_sub(h, "|KWDATA", &data, 0) || !bt_begin(&it, bt, BT_STR_6)) return;
    const uint8_t *k, *val;
    int kl, cap = 0;
    while (bt_next(&it, &k, &kl, &val)) {
        unsigned count = (unsigned)(val[0] | val[1] << 8);
        uint32_t off = (uint32_t)val[2] | (uint32_t)val[3] << 8 | (uint32_t)val[4] << 16 | (uint32_t)val[5] << 24;
        if (!v_ok(data, off, (size_t)count * 4)) continue;
        if (!grow((void **)&h->kw, &cap, h->nkw, sizeof *h->kw)) break;
        struct keyword *w = &h->kw[h->nkw];
        w->text = dup_n(k, (size_t)kl);
        if (!w->text) break;
        w->first = h->nkw_topic;
        w->count = 0;
        int *t = realloc(h->kw_topic, ((size_t)h->nkw_topic + count + 1) * sizeof *t);
        if (!t) { free(w->text); break; }
        h->kw_topic = t;
        for (unsigned i = 0; i < count; i++) {
            int tp = topic_at_offset(h, v32(data, off + (size_t)i * 4), 0);
            if (tp >= 0) h->kw_topic[h->nkw_topic + w->count++] = tp;
        }
        h->nkw_topic += w->count;
        h->nkw++;
    }
}

// --- open / close ----------------------------------------------------------------------------------------------------

struct uhlp *uhlp_open(const void *data, size_t len, const char **err) {
    static const char *dummy;
    if (!err) err = &dummy;
    *err = "";
    if (len < 16 || len > 0x7fffffffu) { *err = "too short to be a help file"; return 0; }
    struct uhlp *h = calloc(1, sizeof *h);
    if (!h) { *err = "out of memory"; return 0; }
    h->file = malloc(len);
    if (!h->file) { free(h); *err = "out of memory"; return 0; }
    memcpy(h->file, data, len);
    h->flen = len;
    struct view f = { h->file, h->flen };
    if (v32(f, 0) != 0x00035F3Fu) { *err = "not a Windows Help file"; goto fail; }
    uint32_t declared = v32(f, 12);
    if (declared >= 16 && declared < h->flen) h->flen = declared;      // trailing bytes are not the file's
    if (!read_system(h, err)) goto fail;
    read_tables(h);
    read_phrases(h);
    if (!read_topic_blocks(h, err)) goto fail;
    read_fonts(h);
    if (!walk_topics(h)) { *err = "out of memory"; goto fail; }
    if (h->npage == 0) { *err = "no topics"; goto fail; }
    read_keywords(h);
    return h;
fail:
    uhlp_close(h);
    return 0;
}

void uhlp_close(struct uhlp *h) {
    if (!h) return;
    free(h->file); free(h->title); free(h->copyright);
    for (int i = 0; i < h->nmacro; i++) free(h->macro[i]);
    free(h->macro); free(h->win); free(h->font);
    free(h->ph_off); free(h->ph_buf); free(h->tbuf); free(h->blk);
    for (int i = 0; i < h->npage; i++) {
        free(h->page[i].title);
        for (int m = 0; m < h->page[i].nmacro; m++) free(h->page[i].macro[m]);
        free(h->page[i].macro);
    }
    free(h->page);
    free(h->ctx_hash); free(h->ctx_off); free(h->map_id); free(h->map_off); free(h->tomap);
    for (int i = 0; i < h->nkw; i++) free(h->kw[i].text);
    free(h->kw); free(h->kw_topic);
    free(h);
}

const char *uhlp_title(const struct uhlp *h) { return h->title ? h->title : ""; }
const char *uhlp_copyright(const struct uhlp *h) { return h->copyright; }
int uhlp_version(const struct uhlp *h) { return h->minor; }
int uhlp_nfonts(const struct uhlp *h) { return h->nfont; }
const struct uhlp_font *uhlp_font(const struct uhlp *h, int i) { return i >= 0 && i < h->nfont ? &h->font[i] : 0; }
int uhlp_nwindows(const struct uhlp *h) { return h->nwin; }
const struct uhlp_window *uhlp_window(const struct uhlp *h, int i) { return i >= 0 && i < h->nwin ? &h->win[i] : 0; }
int uhlp_nmacros(const struct uhlp *h) { return h->nmacro; }
const char *uhlp_macro(const struct uhlp *h, int i) { return i >= 0 && i < h->nmacro ? h->macro[i] : 0; }

int uhlp_ntopics(const struct uhlp *h) { return h->npage; }
const char *uhlp_topic_title(const struct uhlp *h, int t) { return t >= 0 && t < h->npage ? h->page[t].title : ""; }

int uhlp_topic_by_offset(const struct uhlp *h, uint32_t offset, uint32_t *rel) {
    if (offset == 0xFFFFFFFFu) return -1;
    return topic_at_offset(h, offset, rel);
}

int uhlp_contents(const struct uhlp *h) {
    int t = uhlp_topic_by_offset(h, h->contents_start, 0);
    return t >= 0 ? t : 0;
}

int uhlp_topic_by_hash(const struct uhlp *h, uint32_t hash) {
    if (!hash) return uhlp_contents(h);
    if (h->minor <= 16) {
        if (hash >= (uint32_t)h->ntomap) return -1;
        return uhlp_topic_by_offset(h, h->tomap[hash], 0);
    }
    for (int i = 0; i < h->nctx; i++)
        if (h->ctx_hash[i] == hash) return uhlp_topic_by_offset(h, h->ctx_off[i], 0);
    return -1;
}

int uhlp_topic_by_map(const struct uhlp *h, uint32_t map) {
    for (int i = 0; i < h->nmap; i++)
        if (h->map_id[i] == map) return uhlp_topic_by_offset(h, h->map_off[i], 0);
    return -1;
}

int uhlp_topic_prev(const struct uhlp *h, int t) {
    if (t < 0 || t >= h->npage) return -1;
    int p = uhlp_topic_by_offset(h, h->page[t].bwd, 0);
    return p == t ? -1 : p;
}
int uhlp_topic_next(const struct uhlp *h, int t) {
    if (t < 0 || t >= h->npage) return -1;
    int n = uhlp_topic_by_offset(h, h->page[t].fwd, 0);
    return n == t ? -1 : n;
}

int uhlp_nkeywords(const struct uhlp *h) { return h->nkw; }
const char *uhlp_keyword(const struct uhlp *h, int i) { return i >= 0 && i < h->nkw ? h->kw[i].text : ""; }
const int *uhlp_keyword_topics(const struct uhlp *h, int i, int *n) {
    if (i < 0 || i >= h->nkw) { *n = 0; return 0; }
    *n = h->kw[i].count;
    return h->kw_topic + h->kw[i].first;
}

// --- pictures --------------------------------------------------------------------------------------------------------------

struct build {
    const struct uhlp *h;
    struct uhlp_doc *d;
    int run_cap, para_cap, link_cap, tab_cap, table_cap, col_cap, pic_cap, hot_cap, macro_cap;
    size_t pool_cap;
    int oom;
};

static int pool_add(struct build *b, const char *s, size_t n) {
    struct uhlp_doc *d = b->d;
    if (d->pool_len + n + 1 > b->pool_cap) {
        size_t nc = b->pool_cap ? b->pool_cap * 2 : 4096;
        while (nc < d->pool_len + n + 1) nc *= 2;
        char *p = realloc(d->pool, nc);
        if (!p) { b->oom = 1; return -1; }
        d->pool = p;
        b->pool_cap = nc;
    }
    int off = (int)d->pool_len;
    memcpy(d->pool + d->pool_len, s, n);
    d->pool[d->pool_len + n] = 0;
    d->pool_len += n + 1;
    return off;
}

#define ADD(b, arr, count, capf) \
    (grow((void **)&(b)->d->arr, &(b)->capf, (b)->d->count, sizeof *(b)->d->arr) ? &(b)->d->arr[(b)->d->count++] : ((b)->oom = 1, (struct uhlp_##arr *)0))

static struct uhlp_link *new_link(struct build *b) {
    if (!grow((void **)&b->d->link, &b->link_cap, b->d->nlink, sizeof *b->d->link)) { b->oom = 1; return 0; }
    struct uhlp_link *l = &b->d->link[b->d->nlink++];
    memset(l, 0, sizeof *l);
    l->file = -1; l->window = -1; l->macro = -1;
    return l;
}

// 16 Windows colours for a 4-bit device-dependent bitmap.
static const uint32_t g_vga16[16] = {
    0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xC0C0C0,
    0x808080, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF,
};

static uint8_t *unpack_gfx(struct view src, unsigned pack, size_t want) {
    uint8_t *out = calloc(want + 1, 1);
    if (!out) return 0;
    switch (pack) {
    case 0: memcpy(out, src.p, src.n < want ? src.n : want); break;
    case 1: rle_expand(src, out, want); break;
    case 2: {
        long s = lz77_size(src);
        if (s < 0) break;
        uint8_t *t = malloc((size_t)s + 1);
        if (!t) { free(out); return 0; }
        size_t n = lz77_expand(src, t, (size_t)s);
        memcpy(out, t, n < want ? n : want);
        free(t);
        break;
    }
    case 3: {
        long s = lz77_size(src);
        if (s < 0) break;
        uint8_t *t = malloc((size_t)s + 1);
        if (!t) { free(out); return 0; }
        size_t n = lz77_expand(src, t, (size_t)s);
        rle_expand((struct view){ t, n }, out, want);
        free(t);
        break;
    }
    default: break;
    }
    return out;
}

// The hotspots a picture carries (a segmented graphic), appended to doc->hot.
static void add_hotspots(struct build *b, struct view pic, uint32_t hs_size, uint32_t hs_off, struct uhlp_pic *p) {
    if (!hs_size || !hs_off || !v_ok(pic, hs_off, 7)) return;
    size_t start = hs_off;
    unsigned n = v16(pic, start + 1);
    uint32_t macro_len = v32(pic, start + 3);
    size_t recs = start + 7, strs = recs + 15 * (size_t)n + macro_len;
    if (!v_ok(pic, recs, 15 * (size_t)n) || strs > pic.n) return;
    p->hot0 = b->d->nhot;
    for (unsigned i = 0; i < n; i++) {
        long l1 = v_strlen(pic, strs);
        if (l1 < 0) break;
        strs += (size_t)l1 + 1;
        long l2 = v_strlen(pic, strs);
        if (l2 < 0) break;
        const char *target = (const char *)pic.p + strs;
        strs += (size_t)l2 + 1;
        int type = v8(pic, recs + 15 * i);
        struct uhlp_link *lk = new_link(b);
        if (!lk) return;
        int ok = 1;
        switch (type) {
        case 0xC8: lk->kind = UHLP_LINK_MACRO; lk->macro = pool_add(b, target, (size_t)l2); break;
        case 0xE6: case 0xE7: lk->kind = type & 1 ? UHLP_LINK_JUMP : UHLP_LINK_POPUP; lk->hash = uhlp_hash(target); break;
        case 0xEE: case 0xEF: {
            const char *gt = strchr(target, '>');
            char ctx[256];
            size_t cl = gt ? (size_t)(gt - target) : (size_t)l2;
            if (cl >= sizeof ctx) cl = sizeof ctx - 1;
            memcpy(ctx, target, cl);
            ctx[cl] = 0;
            lk->kind = type & 1 ? UHLP_LINK_JUMP : UHLP_LINK_POPUP;
            lk->hash = uhlp_hash(ctx);
            if (gt) for (int w = b->h->nwin - 1; w >= 0; w--)
                if (!strcmp(gt + 1, b->h->win[w].name)) { lk->window = w; break; }
            break;
        }
        default: ok = 0; break;
        }
        if (!ok) { b->d->nlink--; continue; }
        if (!grow((void **)&b->d->hot, &b->hot_cap, b->d->nhot, sizeof *b->d->hot)) { b->oom = 1; return; }
        struct uhlp_hot *ht = &b->d->hot[b->d->nhot++];
        ht->x = (int)v16(pic, recs + 15 * i + 3);
        ht->y = (int)v16(pic, recs + 15 * i + 5);
        ht->w = (int)v16(pic, recs + 15 * i + 7);
        ht->h = (int)v16(pic, recs + 15 * i + 9);
        ht->link = b->d->nlink - 1;
        p->nhot++;
    }
}

// One picture of a |bm file (or an embedded one): appended to doc->pic; its index, or -1.
static int add_picture(struct build *b, struct view ref) {
    unsigned numpict = v16(ref, 2);
    if (!numpict) return -1;
    uint32_t boff = v32(ref, 4);              // the first picture; others are alternate formats
    if (!v_ok(ref, boff, 2)) return -1;
    struct view pic = v_sub(ref, boff, ref.n - boff);
    unsigned type = (unsigned)v8(pic, 0), pack = (unsigned)v8(pic, 1);
    struct cur c = { pic, 2, 0 };
    if (!grow((void **)&b->d->pic, &b->pic_cap, b->d->npic, sizeof *b->d->pic)) { b->oom = 1; return -1; }
    struct uhlp_pic *p = &b->d->pic[b->d->npic];
    memset(p, 0, sizeof *p);
    if (type == 5 || type == 6) {
        c_ulong(&c); c_ulong(&c);                      // pixels per metre
        unsigned planes = c_ushort(&c), bpp = c_ushort(&c);
        uint32_t w = c_ulong(&c), hgt = c_ulong(&c), clr_used = c_ulong(&c), clr_imp = c_ulong(&c);
        uint32_t csz = c_ulong(&c), hs_size = c_ulong(&c);
        uint32_t off = cu32(&c), hs_off = cu32(&c);
        if (c.bad || !w || !hgt || w > 8192 || hgt > 8192 || !planes || planes > 4 || !bpp || bpp > 32) return -1;
        unsigned nc = 0;
        uint32_t pal[256];
        if (type == 6) {
            nc = clr_used;
            if (!nc && bpp <= 8) nc = 1u << bpp;
            if (nc > 256) nc = 256;
            for (unsigned i = 0; i < nc; i++) {
                if (!v_ok(pic, c.at, 4)) return -1;
                pal[i] = (uint32_t)pic.p[c.at + 2] << 16 | (uint32_t)pic.p[c.at + 1] << 8 | pic.p[c.at];
                c.at += 4;
            }
        }
        size_t stride = type == 6 ? (((size_t)w * bpp + 31) & ~(size_t)31) / 8 : (((size_t)w * planes * bpp + 15) & ~(size_t)15) / 8;
        size_t want = stride * hgt;
        if (want > (size_t)64 << 20 || !v_ok(pic, off, csz)) return -1;
        uint8_t *bits = unpack_gfx(v_sub(pic, off, csz), pack, want);
        if (!bits) { b->oom = 1; return -1; }
        uint32_t *px = malloc((size_t)w * hgt * 4);
        if (!px) { free(bits); b->oom = 1; return -1; }
        for (uint32_t y = 0; y < hgt; y++) {
            const uint8_t *row = bits + stride * (hgt - 1 - y);          // rows are stored bottom-up
            for (uint32_t x = 0; x < w; x++) {
                uint32_t col = 0;
                if (bpp == 1) {
                    unsigned i = (row[x / 8] >> (7 - x % 8)) & 1;
                    col = type == 6 && nc > i ? pal[i] : (i ? 0xFFFFFF : 0);
                } else if (bpp == 4) {
                    unsigned i = x & 1 ? row[x / 2] & 15 : row[x / 2] >> 4;
                    col = type == 6 && nc > i ? pal[i] : g_vga16[i];
                } else if (bpp == 8) {
                    unsigned i = row[x];
                    col = type == 6 && nc > i ? pal[i] : (uint32_t)i * 0x010101u;
                } else if (bpp == 24) {
                    col = (uint32_t)row[x * 3 + 2] << 16 | (uint32_t)row[x * 3 + 1] << 8 | row[x * 3];
                } else if (bpp == 32) {
                    col = (uint32_t)row[x * 4 + 2] << 16 | (uint32_t)row[x * 4 + 1] << 8 | row[x * 4];
                }
                px[(size_t)y * w + x] = 0xFF000000u | col;
            }
        }
        if (clr_imp == 1 && nc > 0 && bpp <= 8) {                         // the last palette colour is see-through
            p->transparent = 1;
            for (size_t i = 0; i < (size_t)w * hgt; i++)
                if ((px[i] & 0xFFFFFF) == pal[nc - 1]) px[i] = 0;
        }
        free(bits);
        p->kind = UHLP_PIC_DIB;
        p->w = (int)w;
        p->h = (int)hgt;
        p->argb = px;
        b->d->npic++;
        add_hotspots(b, pic, hs_size, hs_off, p);
        return b->d->npic - 1;
    }
    if (type == 8) {
        unsigned mm = c_ushort(&c);
        unsigned pw = cu16(&c), ph = cu16(&c);
        uint32_t size = c_ulong(&c), csize = c_ulong(&c), hs_size = c_ulong(&c);
        uint32_t off = cu32(&c), hs_off = cu32(&c);
        if (c.bad || size > (size_t)64 << 20 || !v_ok(pic, off, csize)) return -1;
        uint8_t *bits = unpack_gfx(v_sub(pic, off, csize), pack, size);
        if (!bits) { b->oom = 1; return -1; }
        p->kind = UHLP_PIC_WMF;
        p->w = (int)pw;
        p->h = (int)ph;
        p->mapmode = (int)mm;
        p->wmf = bits;
        p->wmf_len = size;
        b->d->npic++;
        add_hotspots(b, pic, hs_size, hs_off, p);
        return b->d->npic - 1;
    }
    return -1;
}

static int add_picture_by_index(struct build *b, unsigned index) {
    char name[24];
    snprintf(name, sizeof name, "|bm%u", index);
    struct view ref;
    if (!find_sub(b->h, name, &ref, 0)) return -1;
    return add_picture(b, ref);
}

// --- one topic as a document -------------------------------------------------------------------------------------

static struct uhlp_run *new_run(struct build *b, int kind, int font, int link) {
    if (!grow((void **)&b->d->run, &b->run_cap, b->d->nrun, sizeof *b->d->run)) { b->oom = 1; return 0; }
    struct uhlp_run *r = &b->d->run[b->d->nrun++];
    memset(r, 0, sizeof *r);
    r->kind = (uint8_t)kind;
    r->font = (uint16_t)font;
    r->link = link;
    r->pic = -1;
    return r;
}

static struct uhlp_para *new_para(struct build *b) {
    if (!grow((void **)&b->d->para, &b->para_cap, b->d->npara, sizeof *b->d->para)) { b->oom = 1; return 0; }
    struct uhlp_para *p = &b->d->para[b->d->npara++];
    memset(p, 0, sizeof *p);
    p->run0 = b->d->nrun;
    p->table = -1;
    p->scrolls = 1;
    return p;
}

struct pstate {
    int font;
    int link;            // the open link, -1 none
};

// One display or table record, appended to the document.
static int browse_paragraph(struct build *b, struct view rec, struct pstate *st, uint32_t tpos, const struct page *pg, int *cur_table, int *cur_row) {
    const struct uhlp *h = b->h;
    if (!v_ok(rec, 0, 0x19)) return 0;
    uint32_t blocksize = v32(rec, 0), size = v32(rec, 4), datalen = v32(rec, 0x10);
    int type = v8(rec, 0x14);
    if (datalen < 0x15 || datalen > rec.n) return 0;
    if (size > 0x100000) size = 0x100000;
    size_t stored_cap = blocksize > datalen ? blocksize - datalen : 0;
    uint8_t *text = expand_text(h, v_sub(rec, datalen, rec.n - datalen), size, stored_cap);
    if (!text) { b->oom = 1; return 0; }
    uint8_t *text_base = text, *text_end = text + size;

    struct cur f = { v_sub(rec, 0, datalen), 0x15, 0 };
    unsigned parlen = 0;
    if (type == REC_DISPLAY || type == REC_TABLE) {
        c_long(&f);
        parlen = c_ushort(&f);
    }
    (void)parlen;
    int ncol = 1, table = -1;
    int result = 0;
    if (type == REC_TABLE) {
        ncol = cu8(&f);
        int ttype = cu8(&f);
        int width = 32767;
        if (ttype == 0 || ttype == 2) { width = (int)(int16_t)cu16(&f); }
        if (ncol < 1) ncol = 1;
        if (!f.bad && v_ok(f.v, f.at, (size_t)ncol * 4)) {
            if (*cur_table < 0 || 1) {
                if (!grow((void **)&b->d->table, &b->table_cap, b->d->ntable, sizeof *b->d->table)) { b->oom = 1; goto done; }
                struct uhlp_table *t = &b->d->table[b->d->ntable];
                t->ncol = ncol; t->width = width; t->type = ttype; t->nrow = 0;
                t->col0 = b->d->ncolumn;
                for (int i = 0; i < ncol; i++) {
                    if (!grow((void **)&b->d->col, &b->col_cap, b->d->ncolumn, sizeof *b->d->col)) { b->oom = 1; goto done; }
                    b->d->col[b->d->ncolumn].width = (int16_t)v16(f.v, f.at + (size_t)i * 4);
                    b->d->col[b->d->ncolumn].gap = (int16_t)v16(f.v, f.at + (size_t)i * 4 + 2);
                    b->d->ncolumn++;
                }
                table = b->d->ntable++;
                *cur_table = table;
            }
            f.at += (size_t)ncol * 4;
        } else goto done;
    }
    (void)tpos;
    (void)pg;
    int lastcol = -1;
    int nc = 0;
    for (; nc < ncol;) {
        struct uhlp_para *pr = new_para(b);
        if (!pr) goto done;
        int col_no = 0;
        if (type == REC_TABLE) {
            nc = lastcol = (int16_t)cu16(&f);
            if (nc == -1) {                       // the end of a row
                b->d->npara--;
                (*cur_row)++;
                if (table >= 0) b->d->table[table].nrow = *cur_row;
                break;
            }
            col_no = nc;
            f.at += 3;
            pr->table = table;
            pr->column = col_no;
            pr->row = *cur_row;
        } else {
            nc++;
        }
        f.at += type == REC_DISPLAY30 ? 6 : 4;
        unsigned bits = cu16(&f);
        if (bits & 0x0001) c_long(&f);
        if (bits & 0x0002) pr->before = c_short(&f);
        if (bits & 0x0004) pr->after = c_short(&f);
        if (bits & 0x0008) pr->spacing = c_short(&f);
        if (bits & 0x0010) pr->left = c_short(&f);
        if (bits & 0x0020) pr->right = c_short(&f);
        if (bits & 0x0040) pr->first = c_short(&f);
        if (bits & 0x0100) {
            pr->border = (uint8_t)cu8(&f);
            pr->border_w = (int16_t)cu16(&f);
        }
        if (bits & 0x0200) {
            int ntab = c_short(&f);
            pr->tab0 = b->d->ntab;
            for (int i = 0; i < ntab && !f.bad; i++) {
                unsigned tab = c_ushort(&f);
                unsigned ts = tab & 0x4000 ? c_ushort(&f) : 0;
                if (!grow((void **)&b->d->tab, &b->tab_cap, b->d->ntab, sizeof *b->d->tab)) { b->oom = 1; goto done; }
                b->d->tab[b->d->ntab].pos = (int)(tab & 0x3fff);
                b->d->tab[b->d->ntab].kind = (uint8_t)(ts == 1 ? 1 : ts == 2 ? 2 : 0);
                b->d->ntab++;
                pr->ntab++;
            }
        }
        switch (bits & 0xc00) {
        case 0x400: pr->align = 1; break;
        case 0x800: pr->align = 2; break;
        default: break;
        }
        pr->keep = (bits & 0x1000) != 0;
        if (type == REC_TABLE && (*cur_row) >= 0 && table >= 0 && *cur_row + 1 > b->d->table[table].nrow) b->d->table[table].nrow = *cur_row + 1;

        int pi = b->d->npara - 1, open = 1, closed_any = 0;
#define NEED_PARA() do { \
            if (!open) { \
                struct uhlp_para copy = b->d->para[pi]; \
                if (!new_para(b)) goto done; \
                pi = b->d->npara - 1; \
                b->d->para[pi] = copy; \
                b->d->para[pi].run0 = b->d->nrun; \
                b->d->para[pi].nrun = 0; \
                open = 1; \
            } } while (0)
        while (text < text_end && f.at < f.v.n && !f.bad) {
            size_t tl = hlp_strnlen((char *)text, (size_t)(text_end - text));
            if (tl) {
                NEED_PARA();
                struct uhlp_run *r = new_run(b, UHLP_RUN_TEXT, st->font, st->link);
                if (!r) goto done;
                r->text = pool_add(b, (char *)text, tl);
                r->len = (int)tl;
            }
            text += tl + 1;
            int cmd = v8(f.v, f.at);
            if (cmd == 0xff) { f.at++; break; }
            switch (cmd) {
            case 0x20: f.at += 5; break;
            case 0x21: f.at += 3; break;
            case 0x80: {
                unsigned fnt = v16(f.v, f.at + 1);
                f.at += 3;
                st->font = (int)fnt < h->nfont ? (int)fnt : 0;
                break;
            }
            case 0x81: f.at++; NEED_PARA(); new_run(b, UHLP_RUN_LINEBREAK, st->font, -1); break;
            case 0x82:                                   // a paragraph ends here (it may be an empty one)
                f.at++;
                NEED_PARA();
                b->d->para[pi].nrun = b->d->nrun - b->d->para[pi].run0;
                open = 0;
                closed_any = 1;
                break;
            case 0x83: f.at++; NEED_PARA(); new_run(b, UHLP_RUN_TAB, st->font, -1); break;
            case 0x86: case 0x87: case 0x88: {
                int place = cmd - 0x86;
                int ptype = v8(f.v, f.at + 1);
                f.at += 2;
                int32_t psize = c_long(&f);
                size_t after = f.at + (psize > 0 ? (size_t)psize : 0);
                int pic = -1;
                if (ptype == 0x22 || ptype == 0x03) {
                    if (ptype == 0x22) c_ushort(&f);
                    int mode = (int16_t)v16(f.v, f.at);
                    unsigned idx = v16(f.v, f.at + 2);
                    if (mode == 0) pic = add_picture_by_index(b, idx);
                    else if (mode == 1 && psize > 4) pic = add_picture(b, v_sub(f.v, f.at + 2, (size_t)psize - 4 <= f.v.n - f.at - 2 ? (size_t)psize - 4 : 0));
                }
                if (b->oom) goto done;
                if (pic >= 0) {
                    NEED_PARA();
                    struct uhlp_run *r = new_run(b, UHLP_RUN_PICTURE, st->font, st->link);
                    if (!r) goto done;
                    r->pic = pic;
                    r->place = (uint8_t)place;
                }
                f.at = after;
                break;
            }
            case 0x89:
                f.at++;
                st->link = -1;
                break;
            case 0x8B: f.at++; NEED_PARA(); new_run(b, UHLP_RUN_NBSP, st->font, st->link); break;
            case 0x8C: f.at++; NEED_PARA(); new_run(b, UHLP_RUN_HYPHEN, st->font, st->link); break;
            case 0xC8: case 0xCC: {
                unsigned ml = v16(f.v, f.at + 1);
                struct uhlp_link *lk = new_link(b);
                if (!lk) goto done;
                lk->kind = UHLP_LINK_MACRO;
                lk->colour = !(cmd & 4);
                size_t avail = f.v.n > f.at + 3 ? f.v.n - (f.at + 3) : 0;
                lk->macro = pool_add(b, (const char *)f.v.p + f.at + 3, hlp_strnlen((const char *)f.v.p + f.at + 3, ml < avail ? ml : avail));
                st->link = b->d->nlink - 1;
                f.at += 3 + ml;
                break;
            }
            case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE6: case 0xE7: {
                struct uhlp_link *lk = new_link(b);
                if (!lk) goto done;
                lk->kind = cmd & 1 ? UHLP_LINK_JUMP : UHLP_LINK_POPUP;
                lk->hash = v32(f.v, f.at + 1);
                lk->by_offset = (cmd == 0xE0 || cmd == 0xE1);
                lk->colour = cmd <= 0xE1 ? 1 : !(cmd & 4);
                st->link = b->d->nlink - 1;
                f.at += 5;
                break;
            }
            case 0xEA: case 0xEB: case 0xEE: case 0xEF: {
                unsigned ml = v16(f.v, f.at + 1);
                int ltype = v8(f.v, f.at + 3);
                struct uhlp_link *lk = new_link(b);
                if (!lk) goto done;
                lk->kind = cmd & 1 ? UHLP_LINK_JUMP : UHLP_LINK_POPUP;
                lk->hash = v32(f.v, f.at + 4);
                lk->colour = !(cmd & 4);
                const char *ptr = (const char *)f.v.p + f.at + 8;
                size_t avail = f.v.n > f.at + 8 ? f.v.n - (f.at + 8) : 0;
                if (ltype == 1) { if (avail) lk->window = (unsigned char)*ptr; }
                else if (ltype == 6) {
                    size_t nl = hlp_strnlen(ptr, avail);
                    for (int w = h->nwin - 1; w >= 0; w--)
                        if (strlen(h->win[w].name) == nl && !memcmp(ptr, h->win[w].name, nl)) { lk->window = w; break; }
                    ptr += nl + 1;
                    avail = avail > nl + 1 ? avail - nl - 1 : 0;
                }
                if (ltype == 4 || ltype == 6) {
                    size_t nl = hlp_strnlen(ptr, avail);
                    lk->file = pool_add(b, ptr, nl);
                }
                st->link = b->d->nlink - 1;
                f.at += 3 + ml;
                break;
            }
            default: f.at++; break;
            }
        }
        // the group's last paragraph: closed, or left open at the end; an open one
        // with nothing in it after a 0x82 is the empty paragraph that 0x82 made
        if (open) {
            b->d->para[pi].nrun = b->d->nrun - b->d->para[pi].run0;
            if (b->d->para[pi].nrun == 0 && closed_any && pi == b->d->npara - 1) b->d->npara--;
        }
#undef NEED_PARA
    }
    result = 1;
done:
    free(text_base);
    return result && !b->oom;
}

struct uhlp_doc *uhlp_render(const struct uhlp *h, int topic) {
    if (!h || topic < 0 || topic >= h->npage) return 0;
    struct build b;
    memset(&b, 0, sizeof b);
    b.h = h;
    b.d = calloc(1, sizeof *b.d);
    if (!b.d) return 0;
    const struct page *pg = &h->page[topic];
    b.d->topic = topic;
    b.d->title = strdup(pg->title);
    if (pg->nmacro) {
        b.d->macro = calloc((size_t)pg->nmacro, sizeof *b.d->macro);
        if (b.d->macro) for (int i = 0; i < pg->nmacro; i++) if ((b.d->macro[b.d->nmacro] = strdup(pg->macro[i]))) b.d->nmacro++;
    }
    struct pstate st = { 0, -1 };
    int cur_table = -1, cur_row = 0, count = 0;
    uint32_t ref = pg->ref, offs = 0;
    unsigned old_index = 0xffffffffu;
    size_t guard = 0, max_records = h->nblk ? (size_t)h->nblk * 0x4000 / 0x15 + 16 : 16;
    do {
        unsigned index = 0;
        struct view rec = record_at(h, ref, &index);
        if (!rec.p) break;
        if (index != old_index) { offs = 0; old_index = index; }
        int type = v8(rec, 0x14);
        uint32_t next = v32(rec, 0x0c);
        if (type == REC_TOPICHDR) {
            if (count++) break;
        } else if (type == REC_DISPLAY30 || type == REC_DISPLAY || type == REC_TABLE) {
            size_t first = (size_t)b.d->npara;
            if (type != REC_TABLE) { cur_table = -1; cur_row = 0; }
            if (!browse_paragraph(&b, rec, &st, 0, pg, &cur_table, &cur_row)) { if (b.oom) goto fail; }
            // the non-scrolling region: records from `nonscroll` up to `scroll`
            uint32_t pos = h->minor <= 16 ? 0 : index * 0x8000u + offs;
            if (h->minor > 16 && pg->nonscroll != 0xFFFFFFFFu && pg->scroll != 0xFFFFFFFFu &&
                pos >= pg->nonscroll && pos < pg->scroll)
                for (size_t i = first; i < (size_t)b.d->npara; i++) b.d->para[i].scrolls = 0;
            unsigned len;
            if (para_len(rec, &len)) offs += len;
        }
        if (h->minor <= 16) {
            if (next == 0) break;
            ref += next;
        } else {
            if (next != 0xFFFFFFFFu && next <= ref) break;
            ref = next;
        }
        if (++guard > max_records) break;
    } while (ref != 0xFFFFFFFFu);
    if (b.oom) goto fail;
    return b.d;
fail:
    uhlp_doc_free(b.d);
    return 0;
}

void uhlp_doc_free(struct uhlp_doc *d) {
    if (!d) return;
    free(d->title); free(d->pool); free(d->run); free(d->para); free(d->link); free(d->tab);
    free(d->table); free(d->col); free(d->hot);
    for (int i = 0; i < d->npic; i++) { free(d->pic[i].argb); free(d->pic[i].wmf); }
    free(d->pic);
    for (int i = 0; i < d->nmacro; i++) free(d->macro[i]);
    free(d->macro);
    free(d);
}

const char *uhlp_doc_str(const struct uhlp_doc *d, int off) {
    return off >= 0 && (size_t)off < d->pool_len ? d->pool + off : "";
}

char *uhlp_topic_text(const struct uhlp *h, int topic) {
    struct uhlp_doc *d = uhlp_render(h, topic);
    if (!d) return 0;
    size_t cap = 256, n = 0;
    char *out = malloc(cap);
    if (!out) { uhlp_doc_free(d); return 0; }
    for (int p = 0; p < d->npara; p++) {
        const struct uhlp_para *pr = &d->para[p];
        for (int i = 0; i < pr->nrun; i++) {
            const struct uhlp_run *r = &d->run[pr->run0 + i];
            const char *s = " ";
            size_t l = 1;
            if (r->kind == UHLP_RUN_TEXT) { s = d->pool + r->text; l = (size_t)r->len; }
            else if (r->kind != UHLP_RUN_TAB && r->kind != UHLP_RUN_NBSP && r->kind != UHLP_RUN_LINEBREAK) continue;
            if (n + l + 2 > cap) {
                while (n + l + 2 > cap) cap *= 2;
                char *g = realloc(out, cap);
                if (!g) { free(out); uhlp_doc_free(d); return 0; }
                out = g;
            }
            memcpy(out + n, s, l);
            n += l;
        }
        out[n++] = '\n';
    }
    out[n] = 0;
    uhlp_doc_free(d);
    return out;
}
