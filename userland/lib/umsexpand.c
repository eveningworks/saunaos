// See umsexpand.h. SZDD and KWAJ, COMPRESS.EXE's two formats, expanded.
#include "lib/umsexpand.h"
#include <errno.h>
#include <string.h>

static const uint8_t SZDD_MAGIC[8] = { 'S', 'Z', 'D', 'D', 0x88, 0xF0, 0x27, 0x33 };
static const uint8_t KWAJ_MAGIC[8] = { 'K', 'W', 'A', 'J', 0x88, 0xF0, 0x27, 0xD1 };

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// A NUL-terminated field of at most `max` bytes, the NUL counted, at *p.
static int field(const uint8_t *in, size_t n, size_t *p, size_t max, char *out) {
    size_t i = 0;
    for (;;) {
        if (*p >= n || i >= max) return -EINVAL;
        uint8_t c = in[(*p)++];
        if (!c) break;
        out[i++] = (char)c;
    }
    while (i && out[i - 1] == ' ') i--;   // WINVER._ stores its extension as " "
    out[i] = 0;
    return 0;
}

int umsx_probe(const uint8_t *in, size_t n, struct umsx_info *info) {
    memset(info, 0, sizeof *info);
    if (n >= 14 && !memcmp(in, SZDD_MAGIC, 8)) {
        if (in[8] != 'A') return -ENOTSUP;
        info->kind = UMSX_SZDD;
        info->missing = (char)in[9];
        info->length = rd32(in + 10);
        info->has_length = 1;
        info->data = 14;
        return 0;
    }
    if (n < 14 || memcmp(in, KWAJ_MAGIC, 8)) {
        info->kind = UMSX_PLAIN;
        info->length = (uint32_t)n;
        info->has_length = 1;
        return 0;
    }
    info->kind = UMSX_KWAJ;
    info->method = rd16(in + 8);
    info->data = rd16(in + 10);
    unsigned flags = rd16(in + 12);
    size_t p = 14;
    // The optional fields, in this order when their flag is set (libmspack's kwajd.c).
    if (flags & 0x01) {
        if (n - p < 4) return -EINVAL;
        info->length = rd32(in + p);
        info->has_length = 1;
        p += 4;
    }
    if (flags & 0x02) {
        if (n - p < 2) return -EINVAL;
        p += 2;
    }
    if (flags & 0x04) {
        if (n - p < 2) return -EINVAL;
        size_t k = rd16(in + p);
        p += 2;
        if (n - p < k) return -EINVAL;
        p += k;
    }
    char base[9] = "", ext[4] = "";
    if ((flags & 0x08) && field(in, n, &p, 9, base) < 0) return -EINVAL;
    if ((flags & 0x10) && field(in, n, &p, 4, ext) < 0) return -EINVAL;
    if (flags & 0x20) {
        if (n - p < 2) return -EINVAL;
        size_t k = rd16(in + p);
        p += 2;
        if (n - p < k) return -EINVAL;
        p += k;
    }
    if (info->data < p || info->data > n) return -EINVAL;
    if (base[0]) {
        size_t b = strlen(base);
        memcpy(info->name, base, b);
        if (ext[0]) {
            info->name[b] = '.';
            memcpy(info->name + b + 1, ext, strlen(ext) + 1);
        } else {
            info->name[b] = 0;
        }
    }
    return 0;
}

void umsx_name(const struct umsx_info *info, const char *disk_name, char *out, size_t cap) {
    if (!cap) return;
    if (info->kind == UMSX_KWAJ && info->name[0]) {
        size_t k = strlen(info->name);
        if (k >= cap) k = cap - 1;
        memcpy(out, info->name, k);
        out[k] = 0;
        return;
    }
    size_t k = strlen(disk_name);
    if (k >= cap) k = cap - 1;
    memcpy(out, disk_name, k);
    out[k] = 0;
    // "SND.CP_" -> "SND.CPL": the character COMPRESS -r took, where SZDD kept it.
    if (info->kind == UMSX_SZDD && k && out[k - 1] == '_' && info->missing)
        out[k - 1] = info->missing;
}

// --- output ----------------------------------------------------------------------------------

struct sink {
    umsx_out out;
    void *ctx;
    uint8_t *buf;
    size_t fill;
    uint32_t total, limit;
    int bounded;
};

// 0 go on, 1 the recorded length is reached, -EIO the caller stopped it.
static int put(struct sink *k, uint8_t b) {
    if (k->bounded && k->total >= k->limit) return 1;
    k->buf[k->fill++] = b;
    k->total++;
    if (k->fill == UMSX_CHUNK) {
        if (k->out(k->ctx, k->buf, k->fill)) return -EIO;
        k->fill = 0;
    }
    return 0;
}

static int flush(struct sink *k) {
    if (k->fill && k->out(k->ctx, k->buf, k->fill)) return -EIO;
    k->fill = 0;
    return 0;
}

// --- LZSS: SZDD, and KWAJ method 2 -----------------------------------------------------------
//
// A control byte, read from its LOW bit up: 1 a literal byte, 0 a match of
// two bytes -- a 12-bit position IN THE WINDOW (not a distance back) and a
// length of 3..18. The window starts full of spaces, writing at 4096 - 16.
static int lzss(const uint8_t *p, const uint8_t *end, struct sink *k, uint8_t *win) {
    memset(win, ' ', UMSX_WINDOW);
    unsigned pos = UMSX_WINDOW - 16;
    while (p < end) {
        unsigned ctl = *p++;
        for (int bit = 0; bit < 8 && p < end; bit++, ctl >>= 1) {
            if (ctl & 1) {
                uint8_t c = *p++;
                win[pos] = c;
                pos = (pos + 1) & (UMSX_WINDOW - 1);
                int r = put(k, c);
                if (r) return r < 0 ? r : 0;
            } else {
                if (end - p < 2) return 0;          // a match cut off by the end: done, as EXPAND was
                unsigned off = p[0] | (unsigned)(p[1] & 0xF0) << 4, len = (p[1] & 0x0F) + 3u;
                p += 2;
                while (len--) {
                    uint8_t c = win[off];
                    win[pos] = c;
                    off = (off + 1) & (UMSX_WINDOW - 1);
                    pos = (pos + 1) & (UMSX_WINDOW - 1);
                    int r = put(k, c);
                    if (r) return r < 0 ? r : 0;
                }
            }
        }
    }
    return 0;
}

// --- KWAJ method 3: LZ + Huffman -------------------------------------------------------------
//
// Bits are read from each byte's HIGH bit down. The stream ends where its
// bytes do, mid-symbol: a read that runs past the last real bit ends the
// file there (libmspack's input_end), and what it half-read is dropped.

struct bits {
    const uint8_t *p, *end;
    uint32_t buf;
    int n;
    uint64_t used, total;   // bits taken, bits there are
};

static unsigned getbits(struct bits *b, int n) {
    while (b->n < n) {
        uint32_t byte = b->p < b->end ? *b->p++ : 0;
        b->buf |= byte << (24 - b->n);
        b->n += 8;
    }
    unsigned v = b->buf >> (32 - n);
    b->buf <<= n;
    b->n -= n;
    b->used += (unsigned)n;
    return v;
}

static int past_end(const struct bits *b) { return b->used > b->total; }

// Canonical codes, shortest first and by symbol within a length -- the
// order libmspack's make_decode_table assigns them; puff.c's decode.
static int decode(struct bits *b, const uint16_t *count, const uint16_t *symbol) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= (int)getbits(b, 1);
        int c = count[len];
        if (code - c < first) return symbol[index + (code - first)];
        index += c;
        first = (first + c) << 1;
        code <<= 1;
    }
    return -1;
}

static const int KWAJ_NSYMS[5] = { 16, 16, 32, 64, 256 };   // MATCHLEN1, MATCHLEN2, LITLEN, OFFSET, LITERAL

// One table's code lengths, by its encoding type (kwajd.c's lzh_read_lens).
static int read_lens(struct bits *b, unsigned type, int nsyms, uint8_t *lens) {
    unsigned c, sel;
    switch (type) {
    case 0:    // all one length: the table is every symbol, evenly
        c = nsyms == 16 ? 4 : nsyms == 32 ? 5 : nsyms == 64 ? 6 : 8;
        for (int i = 0; i < nsyms; i++) lens[i] = (uint8_t)c;
        return 0;
    case 1:    // each the same as the last, one longer, or a new 4-bit length
        c = getbits(b, 4);
        lens[0] = (uint8_t)c;
        for (int i = 1; i < nsyms; i++) {
            if (getbits(b, 1)) {
                if (getbits(b, 1)) c = getbits(b, 4);
                else c++;
            }
            if (c > 15) return -EINVAL;
            lens[i] = (uint8_t)c;
        }
        return 0;
    case 2:    // each one shorter, the same, one longer, or a new 4-bit length
        c = getbits(b, 4);
        lens[0] = (uint8_t)c;
        for (int i = 1; i < nsyms; i++) {
            sel = getbits(b, 2);
            if (sel == 3) c = getbits(b, 4);
            else c = c + sel - 1;
            if (c > 15) return -EINVAL;   // also catches 0 - 1, which wraps
            lens[i] = (uint8_t)c;
        }
        return 0;
    case 3:    // each a 4-bit length
        for (int i = 0; i < nsyms; i++) lens[i] = (uint8_t)getbits(b, 4);
        return 0;
    default:
        return -EINVAL;
    }
}

// The decode table for `lens`: -EINVAL when the lengths over-subscribe the
// code space. An INCOMPLETE code is allowed -- an unused table has no
// codes at all -- and a bit pattern that names no symbol fails in decode.
static int build(const uint8_t *lens, int nsyms, uint16_t *count, uint16_t *symbol) {
    uint16_t offs[16];
    memset(count, 0, 16 * sizeof *count);
    for (int i = 0; i < nsyms; i++) count[lens[i]]++;
    count[0] = 0;
    int left = 1;
    for (int len = 1; len <= 15; len++) {
        left <<= 1;
        left -= count[len];
        if (left < 0) return -EINVAL;
    }
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (uint16_t)(offs[len] + count[len]);
    for (int i = 0; i < nsyms; i++)
        if (lens[i]) symbol[offs[lens[i]]++] = (uint16_t)i;
    return 0;
}

enum { T_MATCHLEN1, T_MATCHLEN2, T_LITLEN, T_OFFSET, T_LITERAL };

static int lzh(const uint8_t *p, const uint8_t *end, struct sink *k, struct umsx_scratch *s) {
    struct bits b = { p, end, 0, 0, 0, (uint64_t)(end - p) * 8 };
    uint16_t *sym[5];
    unsigned types[6];
    // Six 4-bit types, though five tables follow: the sixth keeps the reader byte-aligned.
    for (int i = 0; i < 6; i++) types[i] = getbits(&b, 4);
    int at = 0;
    for (int t = 0; t < 5; t++) {
        sym[t] = s->symbol + at;
        if (read_lens(&b, types[t], KWAJ_NSYMS[t], s->lens + at) < 0) return -EINVAL;
        if (build(s->lens + at, KWAJ_NSYMS[t], s->count[t], sym[t]) < 0) return -EINVAL;
        at += KWAJ_NSYMS[t];
    }
    if (past_end(&b)) return -EINVAL;   // the tables alone ran off the file

    uint8_t *win = s->window;
    memset(win, ' ', UMSX_WINDOW);
    unsigned pos = 0;
    int lit_run = 0;
#define SYM(t, v) do {                                           \
        (v) = decode(&b, s->count[t], sym[t]);                   \
        if (past_end(&b)) return 0;                              \
        if ((v) < 0) return -EINVAL;                             \
    } while (0)
    for (;;) {
        int len, j;
        // After a literal run shorter than the longest, a run cannot follow -- MATCHLEN2's lengths.
        if (lit_run) SYM(T_MATCHLEN2, len);
        else SYM(T_MATCHLEN1, len);
        if (len > 0) {
            len += 2;
            lit_run = 0;
            SYM(T_OFFSET, j);
            unsigned offset = (unsigned)j << 6;
            offset |= getbits(&b, 6);
            if (past_end(&b)) return 0;
            while (len-- > 0) {
                uint8_t c = win[(pos + UMSX_WINDOW - offset) & (UMSX_WINDOW - 1)];
                win[pos] = c;
                pos = (pos + 1) & (UMSX_WINDOW - 1);
                int r = put(k, c);
                if (r) return r < 0 ? r : 0;
            }
        } else {
            SYM(T_LITLEN, len);
            len++;
            lit_run = len != 32;   // 32 is "more literals follow"
            while (len-- > 0) {
                SYM(T_LITERAL, j);
                win[pos] = (uint8_t)j;
                pos = (pos + 1) & (UMSX_WINDOW - 1);
                int r = put(k, (uint8_t)j);
                if (r) return r < 0 ? r : 0;
            }
        }
    }
#undef SYM
}

int umsx_expand(const uint8_t *in, size_t n, struct umsx_scratch *s,
                umsx_out out, void *ctx, uint32_t *written) {
    struct umsx_info info;
    *written = 0;
    int r = umsx_probe(in, n, &info);
    if (r < 0) return r;
    struct sink k = { out, ctx, s->chunk, 0, 0, info.length, info.has_length };
    const uint8_t *p = in + info.data, *end = in + n;
    switch (info.kind) {
    case UMSX_PLAIN:
        k.bounded = 0;
        while (p < end && !(r = put(&k, *p++))) {}
        break;
    case UMSX_SZDD:
        r = lzss(p, end, &k, s->window);
        break;
    case UMSX_KWAJ:
        switch (info.method) {
        case 0: while (p < end && !(r = put(&k, *p++))) {} break;
        case 1: while (p < end && !(r = put(&k, (uint8_t)~*p++))) {} break;
        case 2: r = lzss(p, end, &k, s->window); break;
        case 3: r = lzh(p, end, &k, s); break;
        default: return -ENOTSUP;
        }
        break;
    }
    if (r > 0) r = 0;   // the recorded length reached: done
    if (r < 0) return r;
    if ((r = flush(&k)) < 0) return r;
    *written = k.total;
    if (info.has_length && info.kind != UMSX_PLAIN && k.total != info.length) return -EINVAL;
    return 0;
}
