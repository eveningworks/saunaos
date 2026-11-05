// Indeo Video 3 (IV32, IV31) for uvid (media/uvid.h): Intel's 1993 codec,
// the video of most Video for Windows CD-ROMs.
//
// WRITTEN FROM THE FORMAT, NOT FROM AN IMPLEMENTATION: the MultimediaWiki
// "Indeo 3" page, Ferguson and Melanson's indeo3.txt, and the decoder
// pseudo-code in Intel's US patent 5,386,232 (which the wiki links), with
// FFmpeg's decoded OUTPUT as the oracle for what those leave vague. Only
// the VQ codebooks are FFmpeg's (third_party/indeo3, LGPL tables).
// tools/hostchecks/indeo3_hostcheck.py holds every sample of every frame
// against FFmpeg's.
//
// THE SHAPE. A frame is three planes, Y then 4:1:0 chroma (one U and one V
// per 4x4 luma block), each 7-bit (0..127; the output is the value << 1).
// Each plane is cut into cells by a binary tree of 2-bit codes, interleaved
// with the cells' data: a cell is INTRA (each line predicted from the line
// above it) or INTER (from the other picture, displaced by a motion vector),
// and is coded in 4x4 blocks, four line codes a block -- a "dyad" pair of
// two-pixel deltas or one "quad" of four, from the cell's VQ table -- or by
// run-length escapes. Mode 10 doubles every delta horizontally and every
// coded line vertically (8x8 blocks); an intra mode-10 cell codes the odd
// lines and averages the even ones.
//
// **TWO PICTURES, AND THE FRAME SAYS WHICH.** Flags bit 9 names the picture
// this frame is decoded into; the other is its reference. A disposable
// frame overwrites the picture the next one does too, so the reference is
// not always the previous frame.
//
// **WHAT DK's FILES EXERCISE IS WHAT IS ACCEPTED.** Every behaviour here
// was confirmed bit for bit against FFmpeg on 42 files; a feature none of
// them uses is REFUSED (-ENOTSUP, the frame is dropped) rather than
// guessed: coding modes other than 0 and 10, a non-zero CB OFFSET, half-pel
// motion, the 0xF8 fill code, a skipped block in an intra cell, the NULL
// tree code's "skip" and its intra "copy", a run of blocks longer than its
// cell. docs/roadmap.md lists them.
#include "media/uvid_internal.h"
#include "indeo3data.h"     // third_party/indeo3: vq_tab, FFmpeg's, LGPL

#include <errno.h>
#include <stdlib.h>
#include <string.h>

enum {
    IV3_FRMH = 0x46524d48,  // 'FRMH': the frame header's four words XOR to it
    IV3_VERSION = 32,
    IV3_HDR = 48,           // the bitstream header, after the 16-byte frame header
    IV3_PAD = 64,           // the line above row 0: mid grey, in 7 bits
    IV3_NULL_BITS = 128,    // a bitstream of only its header: "no change"
    IV3_STACK = 32,         // tree depth: a split halves a side of 16 or more
    IV3_TABLES = (int)(sizeof vq_tab / sizeof vq_tab[0]),
    // The format's own limits (MultimediaWiki), multiples of 4.
    IV3_MIN = 16, IV3_MAX_W = 640, IV3_MAX_H = 480,
};

// Frame flags, from the bitstream header.
#define IV3_F_8BIT    0x0002 // 8-bit samples: no known encoder
#define IV3_F_KEY     0x0004
#define IV3_F_HALFPEL 0x0030
#define IV3_F_BUFFER  0x0200

struct iv3_plane {
    int w, h, strip;        // the decoded size; chroma rounds up to 4
    uint8_t *pic[2];        // two pictures; the row above each is IV3_PAD
    uint8_t *out;           // the 8-bit picture handed out
};

struct iv3 {
    int w, h;               // 0 until the first frame
    struct iv3_plane pl[3]; // Y, U (Cb), V (Cr)
    uint8_t *mem;
    int valid[2];           // decoded from a keyframe since the last reset
    int shown;              // a picture is in `out`
    struct uvid_frame f;
    int ready;
    uint8_t step[IV3_TABLES];
};

// --- reading -----------------------------------------------------------

// One cursor for both kinds of data. Tree codes come four to a byte, MSB
// first, from a byte fetched when the last one is used up; a cell's data
// bytes come from the same cursor -- so a byte of tree codes can be half
// used when a cell's data begins, and its other codes follow that data.
struct iv3_bits {
    const uint8_t *p, *end;
    uint8_t codes;
    int left;
    int bad;
};

static int iv3_byte(struct iv3_bits *b) {
    if (b->p >= b->end) { b->bad = 1; return 0; }
    return *b->p++;
}

static int iv3_code(struct iv3_bits *b) {
    if (!b->left) {
        b->codes = (uint8_t)iv3_byte(b);
        b->left = 4;
    }
    b->left--;
    int c = b->codes >> 6;
    b->codes = (uint8_t)(b->codes << 2);
    return c;
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

// --- the VQ tables -----------------------------------------------------

// Entry `i` of table `t` as its two deltas, left pixel first. The table
// packs (a, b) as b * 256 + a, so the high byte borrows when a < 0.
static void iv3_dyad(const vqEntry *t, int i, int *d) {
    int v = t->deltas[i];
    int a = (int8_t)(v & 0xff);
    d[0] = a;
    d[1] = (v - a) / 256;
}

static int gcd(int a, int b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { int t = a % b; a = b; b = t; }
    return a;
}

// Every delta in a table is a multiple of its step -- set 2's tables are
// multiples of 2..9, set 1's of nothing (1).
static int iv3_table_step(const vqEntry *t) {
    int g = 0;
    for (int i = 0; i < t->num_dyads; i++) {
        int d[2];
        iv3_dyad(t, i, d);
        g = gcd(gcd(g, d[0]), d[1]);
    }
    return g ? g : 1;
}

// One line's four deltas from code `k` (< 0xf8), reading the next byte for
// a dyad pair: 0, or -1 for a code the table does not have.
// A DYAD PAIR IS READ RIGHT HALF FIRST; a quad's index q names the pair
// (q / side, q % side) of the table's first `side` entries, left first.
static int iv3_line(struct iv3_bits *b, const vqEntry *t, int k, int *d) {
    if (k < t->num_dyads) {
        int k2 = iv3_byte(b);
        if (b->bad || k2 >= t->num_dyads) return -1;
        iv3_dyad(t, k, d + 2);
        iv3_dyad(t, k2, d);
        return 0;
    }
    int q = k - t->num_dyads, side = t->quad_exp;
    if (q >= side * side) return -1;
    iv3_dyad(t, q / side, d);
    iv3_dyad(t, q % side, d + 2);
    return 0;
}

// --- blocks ------------------------------------------------------------

// A block's four coded lines of four deltas, from line codes and the
// escapes 0xf9..0xff. What an escape says about LATER blocks comes back
// in `*zero` (that many more blocks with zero deltas) and `*copy` (blocks
// taken from the reference unchanged, this one included). 0, or a
// negative errno.
static int iv3_block_codes(struct iv3_bits *b, const vqEntry *t, int inter,
                           int d[4][4], int *zero, int *copy) {
    memset(d, 0, 16 * sizeof d[0][0]);
    for (int l = 0; l < 4;) {
        int k = iv3_byte(b);
        if (b->bad) return -EINVAL;
        if (k < 0xf8) {
            if (iv3_line(b, t, k, d[l]) < 0) return -EINVAL;
            l++;
            continue;
        }
        switch (k) {
        case 0xff:          // zero deltas for lines 0 and 1
            if (l != 0) return -EINVAL;
            l = 2;
            break;
        case 0xfe:          // ...up to line 2
            if (l > 1) return -EINVAL;
            l = 3;
            break;
        case 0xfd:          // ...for the rest of the block
            l = 4;
            break;
        case 0xfc:          // ...and the whole next block
            l = 4;
            *zero = 1;
            break;
        case 0xfb: {        // a run of n blocks, this one included
            int n = iv3_byte(b);
            if (b->bad || (n & 0xc0) || !(n & 0x1f)) return -EINVAL;
            if (n & 0x20) {             // copied
                if (!inter) return -ENOTSUP;
                if (l) return -EINVAL;
                *copy = n & 0x1f;
            } else {                    // zero deltas, from this line on
                *zero = (n & 0x1f) - 1;
            }
            l = 4;
            break;
        }
        case 0xfa:          // copy this block from the reference
        case 0xf9:          // ...and the next
            if (!inter) return -ENOTSUP;
            if (l) return -EINVAL;
            *copy = k == 0xfa ? 1 : 2;
            l = 4;
            break;
        default:            // 0xf8, "fill": no file uses it
            return -ENOTSUP;
        }
    }
    return 0;
}

// No file seen leaves 0..127 -- the encoder keeps samples in 8..120 -- so
// this saturation guards memory and the output; FFmpeg never confirmed it.
static inline uint8_t clip7(int v) { return (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v); }

struct iv3_cell {
    int x, y, w, h;
    int vq;                 // past the motion phase: intra or inter decided
    int inter, mvx, mvy;
};

// Paints one block at (X, Y), `bs` square, from its deltas.
static void iv3_block(uint8_t *cur, const uint8_t *ref, int stride, const struct iv3_cell *c,
                      int X, int Y, int bs, int d[4][4]) {
    uint8_t *dst = cur + (size_t)Y * (size_t)stride + X;
    if (c->inter) {
        // Mode 0: a line a row. Mode 10: a line two rows, a delta two pixels.
        int sh = bs == 8;
        const uint8_t *src = ref + (ptrdiff_t)(Y + c->mvy) * stride + X + c->mvx;
        for (int r = 0; r < bs; r++, dst += stride, src += stride)
            for (int i = 0; i < bs; i++) dst[i] = clip7(src[i] + d[r >> sh][i >> sh]);
        return;
    }
    if (bs == 4) {          // each line from the one above it
        for (int r = 0; r < 4; r++, dst += stride)
            for (int i = 0; i < 4; i++) dst[i] = clip7(dst[i - stride] + d[r][i]);
        return;
    }
    // Intra mode 10: the odd rows are coded, each from the coded row two
    // above SAMPLED IN PAIRS (its left pixel of each pair); the even rows
    // average their neighbours -- except row 0 of the picture, which
    // repeats row 1 rather than averaging with the pad above it.
    for (int l = 0; l < 4; l++) {
        uint8_t *o = dst + (2 * l + 1) * stride, *mid = o - stride;
        const uint8_t *pred = mid - stride;
        for (int i = 0; i < 8; i++) o[i] = clip7(pred[i & ~1] + d[l][i >> 1]);
        if (Y + 2 * l == 0) memcpy(mid, o, 8);
        else for (int i = 0; i < 8; i++) mid[i] = (uint8_t)((pred[i] + o[i]) >> 1);
    }
}

static void iv3_copy_block(uint8_t *cur, const uint8_t *ref, int stride, const struct iv3_cell *c,
                           int X, int Y, int bs) {
    uint8_t *dst = cur + (size_t)Y * (size_t)stride + X;
    const uint8_t *src = ref + (ptrdiff_t)(Y + c->mvy) * stride + X + c->mvx;
    for (int r = 0; r < bs; r++, dst += stride, src += stride) memcpy(dst, src, (size_t)bs);
}

// An intra cell starts from the line above it QUANTISED TO ITS TABLE'S
// LATTICE, written back into the picture: the lattice through 64 (where
// every intra plane starts) with the table's step `m`, each sample moved to
// the point rounding to multiples of m puts it at, kept inside 8..120, the
// range the encoder holds samples to. Matched against FFmpeg for every
// step 2..9; a step of 1 changes nothing.
static void iv3_requant(uint8_t *row, int n, int m) {
    if (m <= 1) return;
    int q64 = (2 * IV3_PAD + m) / (2 * m) * m;
    for (int i = 0; i < n; i++) {
        int v = (2 * row[i] + m) / (2 * m) * m - q64 + IV3_PAD;
        if (v < 8) v += m;
        if (v > 120) v -= m;
        row[i] = (uint8_t)v;
    }
}

struct iv3_ctx {
    struct iv3_plane *pl;
    uint8_t *cur;           // the picture being decoded
    const uint8_t *ref;     // its reference; null when that is not valid
    const uint8_t *aq;      // ALT QUANT: 16 primary/secondary table pairs
    const uint8_t *step;
};

// One cell's VQ data, its descriptor byte already read. 0 or a negative errno.
static int iv3_vq_cell(const struct iv3_ctx *x, struct iv3_bits *b, const struct iv3_cell *c, int desc) {
    int mode = desc >> 4;
    if (mode != 0 && mode != 10) { uvid_fail("Indeo 3 coding mode this build does not decode"); return -ENOTSUP; }
    int bs = mode == 10 ? 8 : 4;
    if (c->w % bs || c->h % bs) { uvid_fail("Indeo 3 cell does not fit its blocks"); return -EINVAL; }
    // The low nibble picks a pair from ALT QUANT, primary in the high
    // nibble; modes 0 and 10 use only the primary.
    int tab = x->aq[desc & 15] >> 4;
    const vqEntry *t = &vq_tab[tab];
    int stride = x->pl->w;
    if (!c->inter && c->y > 0)
        iv3_requant(x->cur + (size_t)(c->y - 1) * (size_t)stride + c->x, c->w, x->step[tab]);

    int zero = 0, copy = 0;
    for (int Y = c->y; Y < c->y + c->h; Y += bs)
        for (int X = c->x; X < c->x + c->w; X += bs) {
            int d[4][4];
            if (copy) {
                copy--;
                iv3_copy_block(x->cur, x->ref, stride, c, X, Y, bs);
                continue;
            }
            if (zero) {
                zero--;
                memset(d, 0, sizeof d);
            } else {
                int rc = iv3_block_codes(b, t, c->inter, d, &zero, &copy);
                if (rc < 0) {
                    uvid_fail(rc == -ENOTSUP ? "Indeo 3 block code this build does not decode"
                                             : "Indeo 3 block is damaged");
                    return rc;
                }
                if (copy) {
                    copy--;
                    iv3_copy_block(x->cur, x->ref, stride, c, X, Y, bs);
                    continue;
                }
            }
            iv3_block(x->cur, x->ref, stride, c, X, Y, bs, d);
        }
    if (zero || copy) { uvid_fail("Indeo 3 run of blocks overruns its cell"); return -ENOTSUP; }
    return 0;
}

// Where a split falls: half of `n`, in whole 8-pixel units, rounded --
// so 40 splits 24 + 16 and 180 splits 88 + 92.
static int iv3_half(int n) { return (n / 8 + 1) / 2 * 8; }

// One plane: motion vectors, then the binary tree and its cells.
static int iv3_plane_decode(const struct iv3_ctx *x, const uint8_t *p, const uint8_t *end) {
    const struct iv3_plane *pl = x->pl;
    struct iv3_bits b = { p, end, 0, 0, 0 };
    if (end - p < 4) return -EINVAL;
    uint32_t nvec = le32(p);
    b.p += 4;
    if (nvec > 256 || (size_t)(end - b.p) < nvec * 2) return -EINVAL;
    const uint8_t *vec = b.p;
    b.p += nvec * 2;

    struct iv3_cell st[IV3_STACK];
    int sp = 0;
    st[sp++] = (struct iv3_cell){ 0, 0, pl->w, pl->h, 0, 0, 0, 0 };
    while (sp) {
        struct iv3_cell *c = &st[sp - 1];
        int k = iv3_code(&b);
        if (b.bad) return -EINVAL;
        if (k < 2) {
            // 0 splits top from bottom; 1 left from right -- a cell wider
            // than a strip (160 luma, 40 chroma) at the strip's edge.
            int n = k ? c->w : c->h;
            int first = k && c->w > pl->strip ? pl->strip : iv3_half(n);
            if (first <= 0 || first >= n || sp == IV3_STACK) return -EINVAL;
            struct iv3_cell t = *c;
            if (k) { t.w = first; c->x += first; c->w -= first; }
            else { t.h = first; c->y += first; c->h -= first; }
            st[sp++] = t;               // the top or left half goes first
            continue;
        }
        if (!c->vq) {                   // 2 intra, 3 inter: a byte names the vector
            c->vq = 1;
            c->inter = k == 3;
            if (c->inter) {
                uint32_t i = (uint32_t)iv3_byte(&b);
                if (b.bad || i >= nvec) return -EINVAL;
                c->mvy = (int8_t)vec[i * 2];
                c->mvx = (int8_t)vec[i * 2 + 1];
                if (c->x + c->mvx < 0 || c->y + c->mvy < 0 ||
                    c->x + c->mvx + c->w > pl->w || c->y + c->mvy + c->h > pl->h) {
                    uvid_fail("Indeo 3 motion vector leaves the picture");
                    return -EINVAL;
                }
            }
            continue;
        }
        if (c->inter && !x->ref) { uvid_fail("Indeo 3 frame without its reference"); return -EINVAL; }
        if (k == 2) {                   // NULL: copy the cell, or leave it
            if (iv3_code(&b) != 0 || b.bad || !c->inter) {
                uvid_fail("Indeo 3 cell code this build does not decode");
                return -ENOTSUP;
            }
            for (int y = 0; y < c->h; y++)
                memcpy(x->cur + (size_t)(c->y + y) * (size_t)pl->w + c->x,
                       x->ref + (size_t)(c->y + y + c->mvy) * (size_t)pl->w + c->x + c->mvx, (size_t)c->w);
        } else {
            int desc = iv3_byte(&b);
            if (b.bad) return -EINVAL;
            int rc = iv3_vq_cell(x, &b, c, desc);
            if (rc) return rc;
        }
        sp--;
    }
    return 0;
}

// --- frames ------------------------------------------------------------

static int iv3_alloc(struct iv3 *s, int w, int h) {
    size_t total = 0;
    for (int i = 0; i < 3; i++) {
        struct iv3_plane *pl = &s->pl[i];
        pl->w = i ? ((w + 3) / 4 + 3) & ~3 : w;
        pl->h = i ? ((h + 3) / 4 + 3) & ~3 : h;
        pl->strip = i ? 40 : 160;
        total += (size_t)pl->w * (size_t)(3 * pl->h + 2);
    }
    s->mem = malloc(total);
    if (!s->mem) { uvid_fail("out of memory for Indeo 3 pictures"); return -ENOMEM; }
    uint8_t *m = s->mem;
    for (int i = 0; i < 3; i++) {
        struct iv3_plane *pl = &s->pl[i];
        size_t row = (size_t)pl->w, n = row * (size_t)pl->h;
        for (int k = 0; k < 2; k++) {
            memset(m, IV3_PAD, row + n);    // the pad row, and a grey picture
            pl->pic[k] = m + row;
            m += row + n;
        }
        pl->out = m;
        m += n;
    }
    s->w = w;
    s->h = h;
    return 0;
}

static int iv3_decode(struct iv3 *s, const uint8_t *pkt, size_t len) {
    if (len < 16 + IV3_HDR) { uvid_fail("Indeo 3 frame is truncated"); return -EINVAL; }
    uint32_t fsz = le32(pkt + 12);
    if ((le32(pkt) ^ le32(pkt + 4) ^ le32(pkt + 8) ^ fsz) != IV3_FRMH) {
        uvid_fail("not an Indeo 3 frame");
        return -EINVAL;
    }
    if (fsz < IV3_HDR || fsz > len - 16) { uvid_fail("Indeo 3 frame is truncated"); return -EINVAL; }
    const uint8_t *bs = pkt + 16, *end = bs + fsz;
    if (le16(bs) != IV3_VERSION) { uvid_fail("Indeo 3 bitstream version this build does not decode"); return -ENOTSUP; }
    int flags = le16(bs + 2);
    if (le32(bs + 4) == IV3_NULL_BITS) return 1;
    if (flags & (IV3_F_8BIT | IV3_F_HALFPEL) || bs[8]) {
        uvid_fail("Indeo 3 frame feature this build does not decode");
        return -ENOTSUP;
    }
    int h = le16(bs + 12), w = le16(bs + 14);
    if (!s->w) {
        if (w < IV3_MIN || h < IV3_MIN || w > IV3_MAX_W || h > IV3_MAX_H || (w | h) & 3) {
            uvid_fail("Indeo 3 picture size outside 16x16..640x480");
            return -EINVAL;
        }
        int rc = iv3_alloc(s, w, h);
        if (rc) return rc;
    } else if (w != s->w || h != s->h) {
        uvid_fail("Indeo 3 picture changed size");
        return -EINVAL;
    }

    int cur = (flags & IV3_F_BUFFER) ? 1 : 0;
    s->valid[cur] = 0;
    // The planes are stored U, V, Y, but the header gives each one's offset.
    static const int off_at[3] = { 16, 24, 20 };
    for (int i = 0; i < 3; i++) {
        uint32_t off = le32(bs + off_at[i]);
        if (off < IV3_HDR || off >= fsz) { uvid_fail("Indeo 3 plane offset is outside the frame"); return -EINVAL; }
        struct iv3_ctx x = {
            &s->pl[i], s->pl[i].pic[cur], s->valid[cur ^ 1] ? s->pl[i].pic[cur ^ 1] : 0, bs + 32, s->step,
        };
        uvid_fail("");
        int rc = iv3_plane_decode(&x, bs + off, end);
        if (rc) {
            if (!*uvid_last_error()) uvid_fail("Indeo 3 plane is damaged");
            return rc;
        }
    }
    s->valid[cur] = 1;

    for (int i = 0; i < 3; i++) {
        const struct iv3_plane *pl = &s->pl[i];
        size_t n = (size_t)pl->w * (size_t)pl->h;
        for (size_t k = 0; k < n; k++) pl->out[k] = (uint8_t)(pl->pic[cur][k] << 1);
    }
    s->f.type = (flags & IV3_F_KEY) ? 'I' : 'P';
    return 0;
}

// --- the codec row -----------------------------------------------------

static int iv3_open(struct uvid *v) {
    struct iv3 *s = calloc(1, sizeof *s);
    if (!s) return -ENOMEM;
    for (int i = 0; i < IV3_TABLES; i++) s->step[i] = (uint8_t)iv3_table_step(&vq_tab[i]);
    v->cpriv = s;
    return 0;
}

static int iv3_feed(struct uvid *v, const struct uvid_pkt *p) {
    struct iv3 *s = v->cpriv;
    // An empty packet (AVI's drop frame) and a null frame both mean "the
    // picture does not change": the last one again, for this frame's time.
    int rc = p->len ? iv3_decode(s, p->data, p->len) : 1;
    if (rc < 0) return rc;
    if (rc == 0) s->shown = 1;
    if (!s->shown) return 0;
    s->f.w = s->w;
    s->f.h = s->h;
    s->f.fmt = UVID_YUV410;
    s->f.y = s->pl[0].out;
    s->f.cb = s->pl[1].out;
    s->f.cr = s->pl[2].out;
    s->f.y_stride = s->pl[0].w;
    s->f.c_stride = s->pl[1].w;
    s->f.full_range = 0;
    s->f.pts_ms = p->pts_ms;
    s->ready = 1;
    return 0;
}

static int iv3_frame(struct uvid *v, int eof, const struct uvid_frame **out) {
    (void)eof;
    struct iv3 *s = v->cpriv;
    if (!s->ready) return 0;
    s->ready = 0;
    *out = &s->f;
    return 1;
}

static void iv3_reset(struct uvid *v) {
    struct iv3 *s = v->cpriv;
    s->valid[0] = s->valid[1] = 0;
    s->shown = 0;
    s->ready = 0;
}

static void iv3_close(struct uvid *v) {
    struct iv3 *s = v->cpriv;
    if (!s) return;
    free(s->mem);
    free(s);
    v->cpriv = 0;
}

const struct uvid_codec uvid_codec_indeo3 = {
    .id = UVID_VC_INDEO3,
    .name = "indeo3",
    .open = iv3_open,
    .feed = iv3_feed,
    .frame = iv3_frame,
    .reset = iv3_reset,
    .close = iv3_close,
};
