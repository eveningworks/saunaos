// The structure map: nodes, the registry, tints and paths. See
// base/ubinmap.h; the decoders are ubinmap_<kind>.c.
#include "base/ubinmap_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// SNIFF ORDER MATTERS where two formats share a mark: a FAT boot sector
// and a GPT disk's protective MBR both end in 55 AA, so both are asked
// before the plain MBR.
const struct ubinmap_format ubinmap_formats[] = {
    { "elf", ubinmap_elf },
    { "wad", ubinmap_wad },
    { "ttf", ubinmap_ttf },
    { "qoi", ubinmap_qoi },
    { "bmp", ubinmap_bmp },
    { "gpt", ubinmap_gpt },
    { "fat", ubinmap_fat },
    { "mbr", ubinmap_mbr },
};
const int ubinmap_format_count = (int)(sizeof ubinmap_formats / sizeof ubinmap_formats[0]);

// --- reading ------------------------------------------------------------

int bm_read(struct ubinmap_ctx *c, uint64_t off, void *buf, size_t n) {
    uint64_t size = c->src->size;
    if (off > size || n > size - off) return -1;
    return n == 0 || c->src->read(c->src->ctx, off, buf, n) == n ? 0 : -1;
}

uint16_t bm_get16(const struct ubinmap_ctx *c, const uint8_t *p) {
    return c->be ? (uint16_t)(p[0] << 8 | p[1]) : (uint16_t)(p[1] << 8 | p[0]);
}

uint32_t bm_get32(const struct ubinmap_ctx *c, const uint8_t *p) {
    return c->be ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]
                 : (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

uint64_t bm_get64(const struct ubinmap_ctx *c, const uint8_t *p) {
    uint64_t hi = bm_get32(c, c->be ? p : p + 4), lo = bm_get32(c, c->be ? p + 4 : p);
    return hi << 32 | lo;
}

int bm_u8(struct ubinmap_ctx *c, uint64_t off, uint8_t *v) {
    *v = 0;
    return bm_read(c, off, v, 1);
}

int bm_u16(struct ubinmap_ctx *c, uint64_t off, uint16_t *v) {
    uint8_t b[2];
    *v = 0;
    if (bm_read(c, off, b, 2)) return -1;
    *v = bm_get16(c, b);
    return 0;
}

int bm_u32(struct ubinmap_ctx *c, uint64_t off, uint32_t *v) {
    uint8_t b[4];
    *v = 0;
    if (bm_read(c, off, b, 4)) return -1;
    *v = bm_get32(c, b);
    return 0;
}

int bm_u64(struct ubinmap_ctx *c, uint64_t off, uint64_t *v) {
    uint8_t b[8];
    *v = 0;
    if (bm_read(c, off, b, 8)) return -1;
    *v = bm_get64(c, b);
    return 0;
}

void bm_ascii(const uint8_t *p, size_t n, char *out, size_t cap) {
    size_t k = 0;
    for (size_t i = 0; i < n && p[i] && k + 1 < cap; i++)
        out[k++] = p[i] >= 0x20 && p[i] < 0x7F ? (char)p[i] : '?';
    if (cap) out[k] = '\0';
}

void bm_str(struct ubinmap_ctx *c, uint64_t off, size_t max, char *out, size_t cap) {
    uint8_t buf[256];
    if (cap) out[0] = '\0';
    if (off >= c->src->size) return;
    if (max > sizeof buf) max = sizeof buf;
    if (max > c->src->size - off) max = (size_t)(c->src->size - off);
    if (bm_read(c, off, buf, max)) return;
    bm_ascii(buf, max, out, cap);
}

void bm_human(uint64_t n, char *out, size_t cap) {
    static const char *const unit[] = { "bytes", "KiB", "MiB", "GiB", "TiB" };
    int u = 0;
    uint64_t whole = n, tenth = 0;
    while (whole >= 1024 && u < 4) {
        tenth = (whole % 1024) * 10 / 1024;
        whole /= 1024;
        u++;
    }
    if (!u) snprintf(out, cap, "%llu bytes", (unsigned long long)n);
    else snprintf(out, cap, "%llu.%llu %s", (unsigned long long)whole, (unsigned long long)tenth, unit[u]);
}

void bm_guid(const uint8_t g[16], char *out, size_t cap) {
    snprintf(out, cap, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9],
             g[10], g[11], g[12], g[13], g[14], g[15]);
}

// --- building ------------------------------------------------------------

static uint32_t add_text(struct ubinmap *m, const char *s) {
    size_t n = strlen(s) + 1;
    if (m->text_len + n > m->text_cap) {
        size_t cap = m->text_cap ? m->text_cap * 2 : 4096;
        while (cap < m->text_len + n) cap *= 2;
        char *t = realloc(m->text, cap);
        if (!t) { m->oom = 1; return UINT32_MAX; }
        m->text = t;
        m->text_cap = cap;
    }
    memcpy(m->text + m->text_len, s, n);
    uint32_t at = (uint32_t)m->text_len;
    m->text_len += n;
    return at;
}

int bm_full(const struct ubinmap_ctx *c) { return c->m->dropped > 0 || c->m->oom; }

int bm_add(struct ubinmap_ctx *c, int depth, uint64_t off, uint64_t len, int tint,
           const char *label, const char *fmt, ...) {
    struct ubinmap *m = c->m;
    if (m->oom) return -1;
    if (m->count >= UBINMAP_NODES_MAX) { m->dropped++; return -1; }
    if (m->count == m->cap) {
        int cap = m->cap ? m->cap * 2 : 64;
        struct ubinmap_node *n = realloc(m->nodes, (size_t)cap * sizeof *n);
        if (!n) { m->oom = 1; return -1; }
        m->nodes = n;
        m->cap = cap;
    }
    char value[160] = "";
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(value, sizeof value, fmt, ap);
        va_end(ap);
    }
    struct ubinmap_node *n = &m->nodes[m->count];
    memset(n, 0, sizeof *n);
    // CLIPPED to the file: a range a field claims is never trusted.
    uint64_t size = c->src->size;
    if (off >= size) {
        if (len) n->flags |= UBINMAP_CLIPPED;
        off = size;
        len = 0;
    } else if (len > size - off) {
        n->flags |= UBINMAP_CLIPPED;
        len = size - off;
    }
    n->off = off;
    n->len = len;
    n->depth = (int16_t)(depth < 0 ? 0 : depth > 30 ? 30 : depth);
    n->tint = (uint8_t)(tint < 0 || tint > UBINMAP_TINTS ? 0 : tint);
    n->label = add_text(m, label ? label : "");
    n->value = add_text(m, value);
    if (m->oom) return -1;
    return m->count++;
}

void bm_note(struct ubinmap_ctx *c, const char *fmt, ...) {
    if (c->m->note[0]) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->m->note, sizeof c->m->note, fmt, ap);
    va_end(ap);
}

void bm_format(struct ubinmap_ctx *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->m->format, sizeof c->m->format, fmt, ap);
    va_end(ap);
}

int ubinmap_decode(const struct ubinmap_src *src, struct ubinmap *m) {
    memset(m, 0, sizeof *m);
    for (int i = 0; i < ubinmap_format_count; i++) {
        struct ubinmap_ctx c = { src, m, 0 };
        if (ubinmap_formats[i].decode(&c)) {
            m->kind = ubinmap_formats[i].name;
            return 1;
        }
        // A decoder that said no must have added nothing; one that did
        // is cleared, so the next sees an empty map.
        ubinmap_free(m);
    }
    return 0;
}

void ubinmap_free(struct ubinmap *m) {
    free(m->nodes);
    free(m->text);
    memset(m, 0, sizeof *m);
}

// --- asking ----------------------------------------------------------------

const char *ubinmap_label(const struct ubinmap *m, int i) {
    return i >= 0 && i < m->count && m->nodes[i].label != UINT32_MAX ? m->text + m->nodes[i].label : "";
}

const char *ubinmap_value(const struct ubinmap *m, int i) {
    return i >= 0 && i < m->count && m->nodes[i].value != UINT32_MAX ? m->text + m->nodes[i].value : "";
}

int ubinmap_parent(const struct ubinmap *m, int i) {
    if (i <= 0 || i >= m->count) return -1;
    int d = m->nodes[i].depth;
    for (int k = i - 1; k >= 0; k--)
        if (m->nodes[k].depth < d) return k;
    return -1;
}

static int covers(const struct ubinmap_node *n, uint64_t off) {
    return off >= n->off && off - n->off < n->len;
}

int ubinmap_deepest(const struct ubinmap *m, uint64_t off) {
    int best = -1;
    for (int i = 0; i < m->count; i++)
        if (covers(&m->nodes[i], off) && (best < 0 || m->nodes[i].depth >= m->nodes[best].depth))
            best = i;
    return best;
}

int ubinmap_path(const struct ubinmap *m, uint64_t off, char *out, size_t cap) {
    if (cap) out[0] = '\0';
    int n = ubinmap_deepest(m, off);
    if (n < 0) return 0;
    int chain[32], k = 0;
    for (int i = n; i >= 0 && k < 32; i = ubinmap_parent(m, i)) chain[k++] = i;
    size_t at = 0;
    while (k-- > 0 && at + 1 < cap) {
        int w = snprintf(out + at, cap - at, "%s%s", at ? " > " : "", ubinmap_label(m, chain[k]));
        if (w < 0) break;
        at += (size_t)w;
        if (at >= cap) { at = cap - 1; break; }
    }
    return 1;
}

// --- tints: a sweep over the boundaries, the deepest active node winning ---

struct tnode { uint64_t off, end; int pri_depth, idx, tint; };

static int by_off(const void *a, const void *b) {
    const struct tnode *x = a, *y = b;
    return x->off < y->off ? -1 : x->off > y->off;
}

static int by_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

// The heap keeps the WINNER on top: deeper, then later in the map.
static int beats(const struct tnode *a, const struct tnode *b) {
    return a->pri_depth != b->pri_depth ? a->pri_depth > b->pri_depth : a->idx > b->idx;
}

static void heap_push(const struct tnode *t, int *h, int *n, int v) {
    int i = (*n)++;
    h[i] = v;
    while (i > 0 && beats(&t[h[i]], &t[h[(i - 1) / 2]])) {
        int p = (i - 1) / 2, s = h[p];
        h[p] = h[i];
        h[i] = s;
        i = p;
    }
}

static void heap_pop(const struct tnode *t, int *h, int *n) {
    h[0] = h[--*n];
    int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, b = i;
        if (l < *n && beats(&t[h[l]], &t[h[b]])) b = l;
        if (r < *n && beats(&t[h[r]], &t[h[b]])) b = r;
        if (b == i) break;
        int s = h[b];
        h[b] = h[i];
        h[i] = s;
        i = b;
    }
}

int ubinmap_tints(const struct ubinmap *m, struct ubinmap_span **out) {
    *out = NULL;
    int k = 0;
    for (int i = 0; i < m->count; i++) k += m->nodes[i].tint && m->nodes[i].len;
    if (!k) return 0;
    struct tnode *t = malloc((size_t)k * sizeof *t);
    uint64_t *cut = malloc((size_t)k * 2 * sizeof *cut);
    int *heap = malloc((size_t)k * sizeof *heap);
    struct ubinmap_span *s = malloc((size_t)k * 2 * sizeof *s);
    if (!t || !cut || !heap || !s) { free(t); free(cut); free(heap); free(s); return -1; }
    int n = 0;
    for (int i = 0; i < m->count; i++) {
        const struct ubinmap_node *nd = &m->nodes[i];
        if (!nd->tint || !nd->len) continue;
        t[n] = (struct tnode){ nd->off, nd->off + nd->len, nd->depth, i, nd->tint };
        cut[2 * n] = nd->off;
        cut[2 * n + 1] = nd->off + nd->len;
        n++;
    }
    qsort(t, (size_t)n, sizeof *t, by_off);
    qsort(cut, (size_t)n * 2, sizeof *cut, by_u64);
    int ncut = 0;
    for (int i = 0; i < 2 * n; i++)
        if (!ncut || cut[ncut - 1] != cut[i]) cut[ncut++] = cut[i];
    int hn = 0, next = 0, ns = 0;
    for (int b = 0; b + 1 < ncut; b++) {
        uint64_t lo = cut[b], hi = cut[b + 1];
        while (next < n && t[next].off <= lo) heap_push(t, heap, &hn, next++);
        while (hn && t[heap[0]].end <= lo) heap_pop(t, heap, &hn);
        if (!hn) continue;
        const struct tnode *w = &t[heap[0]];
        if (ns && s[ns - 1].node == w->idx && s[ns - 1].off + s[ns - 1].len == lo) {
            s[ns - 1].len += hi - lo;
        } else {
            s[ns++] = (struct ubinmap_span){ lo, hi - lo, w->tint, w->idx };
        }
    }
    free(t);
    free(cut);
    free(heap);
    *out = s;
    return ns;
}

int ubinmap_span_at(const struct ubinmap_span *s, int n, uint64_t off) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (off < s[mid].off) hi = mid - 1;
        else if (off - s[mid].off >= s[mid].len) lo = mid + 1;
        else return mid;
    }
    return -1;
}
