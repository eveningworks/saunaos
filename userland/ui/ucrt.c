// See ucrt.h. The passes, in the order they run over the rect:
//
//   copy     the rect as the app drew it, into `buf`
//   glow     `buf` at half resolution, box-blurred twice (close to a
//            Gaussian), screen-blended back -- phosphor bloom
//   compose  per pixel: glow, then the mask's channel gains, the
//            scanline and vignette gains (both SEPARABLE tables, so a
//            pixel costs multiplies, not a distance), flicker and noise
//   curve    each output pixel samples `buf` bilinearly at the point the
//            precomputed `map` names, or is bezel
//
// Everything is integer except building the tables, which happens when
// the rect's size or the curve changes and never per frame.
#include "ui/ucrt.h"

#include <stdlib.h>
#include <string.h>
#include "rt/sys.h"

#define UCRT_BEZEL 0xFFFFFFFFu
#define FIX 16            // the map's sub-pixel steps: 4 bits a weight
#define MAP(idx, ax, ay) ((uint32_t)(idx) << 8 | (uint32_t)(ax) << 4 | (uint32_t)(ay))
#define MAP_MAX 4095      // 12 integer bits; a wider rect is drawn flat

const struct ucrt_look ucrt_presets[UCRT_PRESET_COUNT] = {
    [UCRT_PRESET_SUBTLE]  = { 1, 1, 1, UCRT_CURVE_OFF,    UCRT_MASK_OFF,      0, 0 },
    [UCRT_PRESET_SOFT]    = { 2, 2, 2, UCRT_CURVE_SLIGHT, UCRT_MASK_OFF,      0, 0 },
    [UCRT_PRESET_CLASSIC] = { 2, 2, 2, UCRT_CURVE_SUBTLE, UCRT_MASK_OFF,      0, 0 },
    [UCRT_PRESET_CURVED]  = { 3, 3, 3, UCRT_CURVE_STRONG, UCRT_MASK_APERTURE, 0, 0 },
};
const char *const ucrt_preset_names[UCRT_PRESET_COUNT] = { "Subtle", "Soft CRT", "Classic CRT", "Curved" };
const char *const ucrt_preset_words[UCRT_PRESET_COUNT] = { "subtle", "soft", "classic", "curved" };
const char *const ucrt_level_words[UCRT_LEVEL_MAX + 1] = { "off", "low", "medium", "high" };
const char *const ucrt_curve_words[UCRT_CURVE_COUNT] = { "off", "slight", "subtle", "strong" };
const char *const ucrt_mask_words[UCRT_MASK_COUNT] = { "off", "aperture", "slot" };

// Strengths per level, out of 256.
static const int SCAN[UCRT_LEVEL_MAX + 1]  = { 0, 40, 77, 115 };
static const int GLOW[UCRT_LEVEL_MAX + 1]  = { 0, 90, 170, 230 };
static const int VIG[UCRT_LEVEL_MAX + 1]   = { 0, 64, 115, 166 };
static const int MASK_DIM = 56;

// The tube: barrel strength, the bezel's width and the glass's corner
// radius, the last two as fractions of the rect's height.
static const struct { float k, inset, radius; } CURVE[UCRT_CURVE_COUNT] = {
    { 0.0f, 0.0f, 0.0f }, { 0.015f, 1.0f / 52, 1.0f / 28 }, { 0.045f, 1.0f / 37, 1.0f / 20 },
    { 0.11f, 1.0f / 29, 1.0f / 13 },
};

int ucrt_preset_of(const struct ucrt_look *l) {
    for (int i = 0; i < UCRT_PRESET_COUNT; i++)
        if (!memcmp(&ucrt_presets[i], l, sizeof *l)) return i;
    return -1;
}

int ucrt_look_on(const struct ucrt_look *l) {
    return l->scanlines || l->glow || l->vignette || l->curve || l->mask || l->flicker || l->noise;
}

int ucrt_animates(const struct ucrt_look *l) { return l->flicker || l->noise; }

void ucrt_init(struct ucrt *c) {
    memset(c, 0, sizeof *c);
    c->period = 3;
    c->bezel = 0x0c0c0c;
    c->rng = 0x9e3779b9u;
}

void ucrt_free(struct ucrt *c) {
    free(c->map);
    free(c->buf);
    free(c->half);
    free(c->half2);
    free(c->col_sums);
    free(c->lanes);
    c->map = c->buf = c->half = c->half2 = 0;
    c->col_sums = 0;
    c->lanes = 0;
    c->w = c->h = 0;
}

// --- the curve ---------------------------------------------------------

// Where on the source a point of the glass shows from: crt-geom's
// uv * (1 + k r^2), scaled so each edge's midpoint stays on its edge.
static void warp(int curve, int w, int h, float x, float y, float *sx, float *sy) {
    float in = CURVE[curve].inset * h, gw = w - 2 * in, gh = h - 2 * in;
    float u = (x - in) / gw * 2 - 1, v = (y - in) / gh * 2 - 1;
    float k = CURVE[curve].k, f = (1 + k * (u * u + v * v)) / (1 + k);
    *sx = (u * f + 1) / 2 * w;
    *sy = (v * f + 1) / 2 * h;
}

// Inside the glass's rounded rect?
static int on_glass(int curve, int w, int h, int x, int y) {
    float in = CURVE[curve].inset * h, r = CURVE[curve].radius * h;
    float x0 = in + r, x1 = w - in - r, y0 = in + r, y1 = h - in - r;
    float fx = x + 0.5f, fy = y + 0.5f;
    if (fx < in || fx > w - in || fy < in || fy > h - in) return 0;
    float dx = fx < x0 ? x0 - fx : fx > x1 ? fx - x1 : 0;
    float dy = fy < y0 ? y0 - fy : fy > y1 ? fy - y1 : 0;
    return dx * dx + dy * dy <= r * r;
}

int ucrt_margin(const struct ucrt_look *l, int w, int h) {
    if (!l->curve || w < 8 || h < 8) return 0;
    // The glass's corner, 45 degrees round its arc, is the source point
    // nearest the source's corner that still shows: everything between
    // it and the corner is cut off.
    float in = CURVE[l->curve].inset * h, r = CURVE[l->curve].radius * h;
    float d = in + r * (1 - 0.7071f), sx, sy;
    warp(l->curve, w, h, d, d, &sx, &sy);
    int m = (int)(sx > sy ? sx : sy) + 2;
    return m > 0 ? m : 0;
}

static int build_map(struct ucrt *c) {
    if (c->w > MAP_MAX || c->h > MAP_MAX) return -1;
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++) {
            uint32_t *m = &c->map[y * c->w + x];
            float sx, sy;
            warp(c->look.curve, c->w, c->h, x + 0.5f, y + 0.5f, &sx, &sy);
            sx -= 0.5f;
            sy -= 0.5f;
            if (!on_glass(c->look.curve, c->w, c->h, x, y) || sx < 0 || sy < 0 ||
                sx > c->w - 1 || sy > c->h - 1) {
                *m = UCRT_BEZEL;
                continue;
            }
            int fx = (int)(sx * FIX), fy = (int)(sy * FIX);
            *m = MAP((fy / FIX) * c->w + fx / FIX, fx % FIX, fy % FIX);
        }
    c->map_curve = c->look.curve;
    return 0;
}

int ucrt_source_point(const struct ucrt *c, int px, int py, int *sx, int *sy) {
    *sx = px;
    *sy = py;
    if (!c->look.curve || !c->map || c->map_curve != c->look.curve) return 1;
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px >= c->w) px = c->w - 1;
    if (py >= c->h) py = c->h - 1;
    // On the bezel: step toward the middle until the glass, so a drag
    // that leaves the tube keeps selecting to its edge.
    int on = 1;
    for (int i = 0; i < 64 && c->map[py * c->w + px] == UCRT_BEZEL; i++) {
        on = 0;
        px += px < c->w / 2 ? 1 : px > c->w / 2 ? -1 : 0;
        py += py < c->h / 2 ? 1 : py > c->h / 2 ? -1 : 0;
    }
    uint32_t m = c->map[py * c->w + px];
    if (m == UCRT_BEZEL) return 0;
    *sx = (int)((m >> 8) % (uint32_t)c->w);
    *sy = (int)((m >> 8) / (uint32_t)c->w);
    return on;
}

// --- buffers -----------------------------------------------------------

static int ensure(struct ucrt *c, int w, int h) {
    if (c->w == w && c->h == h && c->buf) {
        if (c->look.curve && c->map_curve != c->look.curve) return build_map(c);
        return 0;
    }
    ucrt_free(c);
    c->hw = ((w + 1) / 2 + 1) & ~1;   // even: the glow's blur works in pairs
    c->hh = ((h + 1) / 2 + 1) & ~1;
    // w + 1 more: the warp reads the right and lower neighbours of the
    // last column and row, at weight 0.
    c->buf = calloc((size_t)w * h + w + 1, 4);
    c->map = malloc((size_t)w * h * 4);
    c->half = malloc((size_t)c->hw * c->hh * 4);
    c->half2 = malloc((size_t)c->hw * c->hh * 4);
    c->col_sums = malloc((size_t)c->hw * 8);
    c->lanes = malloc((size_t)w * 3 * sizeof *c->lanes);
    if (!c->buf || !c->map || !c->half || !c->half2 || !c->col_sums || !c->lanes) {
        ucrt_free(c);
        return -1;
    }
    c->w = w;
    c->h = h;
    c->map_curve = -1;
    return c->look.curve ? build_map(c) : 0;
}

// --- glow --------------------------------------------------------------
//
// A box blur by running sums, so the radius costs nothing per pixel: two
// pixels at a time as eight 16-bit lanes (a lane holds at most
// 255 * (2r+1)), and the divide is SSE2's multiply-high by 65536/(2r+1)
// -- the same floor((sum * inv) >> 16) the scalar version took. The
// window is 2r+1 wide everywhere, the ends repeating the edge pixel.
//
// THE HALF IMAGE IS PADDED TO EVEN SIZES, for the pairs. The row blur
// clamps at the real width, so the padding column is only ever an output;
// the column blur clamps at the PADDED height, so the padding row must be
// a copy of the last real one before every column pass, or the bottom
// edge blurs toward garbage.
typedef uint8_t v8u8 __attribute__((vector_size(8)));
typedef uint16_t v8u16 __attribute__((vector_size(16)));
typedef int16_t v8s16 __attribute__((vector_size(16)));

static inline v8u16 wide2(uint32_t a, uint32_t b) {
    uint64_t t = a | (uint64_t)b << 32;
    v8u8 v;
    __builtin_memcpy(&v, &t, sizeof v);
    return __builtin_convertvector(v, v8u16);
}

static inline uint64_t unsum2(v8u16 sum, v8u16 inv) {
    v8u8 o = __builtin_convertvector((v8u16)__builtin_ia32_pmulhuw128((v8s16)sum, (v8s16)inv), v8u8);
    uint64_t t;
    __builtin_memcpy(&t, &o, sizeof t);
    return t;
}

static inline int clampi(int i, int n) { return i < 0 ? 0 : i >= n ? n - 1 : i; }

// Along rows of n pixels (stride `stride`), two rows at once; `lines` even.
static void box_rows(const uint32_t *in, uint32_t *out, int stride, int n, int lines, int r) {
    v8u16 inv = (v8u16){ 0 } + (uint16_t)((1u << 16) / (uint32_t)(2 * r + 1));
    for (int l = 0; l < lines; l += 2) {
        const uint32_t *a = in + (long)l * stride, *b = a + stride;
        uint32_t *oa = out + (long)l * stride, *ob = oa + stride;
        v8u16 sum = { 0 };
        for (int i = -r; i <= r; i++) sum += wide2(a[clampi(i, n)], b[clampi(i, n)]);
        // Clamped only where the window hangs off an end: [r, n - r - 1)
        // reads in bounds both sides.
        int mid0 = r < n ? r : n, mid1 = n - r - 1 > mid0 ? n - r - 1 : mid0;
        int i = 0;
        for (; i < mid0; i++) {
            uint64_t t = unsum2(sum, inv);
            oa[i] = (uint32_t)t;
            ob[i] = (uint32_t)(t >> 32);
            int lo = clampi(i - r, n), hi = clampi(i + r + 1, n);
            sum += wide2(a[hi], b[hi]) - wide2(a[lo], b[lo]);
        }
        for (; i < mid1; i++) {
            uint64_t t = unsum2(sum, inv);
            oa[i] = (uint32_t)t;
            ob[i] = (uint32_t)(t >> 32);
            sum += wide2(a[i + r + 1], b[i + r + 1]) - wide2(a[i - r], b[i - r]);
        }
        for (; i < n; i++) {
            uint64_t t = unsum2(sum, inv);
            oa[i] = (uint32_t)t;
            ob[i] = (uint32_t)(t >> 32);
            int lo = clampi(i - r, n), hi = clampi(i + r + 1, n);
            sum += wide2(a[hi], b[hi]) - wide2(a[lo], b[lo]);
        }
    }
}

// Down columns of `lines` pixels, two columns at once, a row at a time
// with a running sum per column pair so memory is walked in order;
// `stride` even. `sums` holds stride / 2 lane vectors.
static void box_cols(const uint32_t *in, uint32_t *out, int stride, int lines, int r, v8u16 *sums) {
    v8u16 inv = (v8u16){ 0 } + (uint16_t)((1u << 16) / (uint32_t)(2 * r + 1));
    int pairs = stride / 2;
    for (int x = 0; x < pairs; x++) sums[x] = (v8u16){ 0 };
    for (int i = -r; i <= r; i++) {
        const uint32_t *row = in + (long)clampi(i, lines) * stride;
        for (int x = 0; x < pairs; x++) sums[x] += wide2(row[2 * x], row[2 * x + 1]);
    }
    for (int l = 0; l < lines; l++) {
        uint64_t *dst = (uint64_t *)(void *)(out + (long)l * stride);
        for (int x = 0; x < pairs; x++) dst[x] = unsum2(sums[x], inv);
        const uint32_t *ra = in + (long)clampi(l - r, lines) * stride;
        const uint32_t *rb = in + (long)clampi(l + r + 1, lines) * stride;
        for (int x = 0; x < pairs; x++)
            sums[x] += wide2(rb[2 * x], rb[2 * x + 1]) - wide2(ra[2 * x], ra[2 * x + 1]);
    }
}

static void pad_col(uint32_t *b, int stride, int n, int lines) {
    if (n < stride)
        for (int y = 0; y < lines; y++) b[(long)y * stride + n] = b[(long)y * stride + n - 1];
}

static void pad_row(uint32_t *b, int stride, int n, int lines) {
    if (n < lines) __builtin_memcpy(&b[(long)n * stride], &b[(long)(n - 1) * stride], (size_t)stride * 4);
}

// The glow, at half resolution, already scaled by its strength.
static void build_glow(struct ucrt *c, const uint32_t *src, long stride) {
    int w = c->w, h = c->h, hw = c->hw, hh = c->hh;
    int nw = (w + 1) / 2, nh = (h + 1) / 2;   // the real half image; hw/hh pad it
    // The 2x2 average: two half pixels a step from four source pixels of
    // each row, in 16-bit lanes; the alpha lanes are dropped to 0. The
    // tail, and an odd width's last column, a word at a time with red
    // and blue sharing one (four lanes of 255 fit).
    const v8u16 rgb = { 0xFFFF, 0xFFFF, 0xFFFF, 0, 0xFFFF, 0xFFFF, 0xFFFF, 0 };
    for (int y = 0; y < nh; y++) {
        const uint32_t *r0 = &src[2 * y * stride], *r1 = 2 * y + 1 < h ? r0 + stride : r0;
        uint32_t *o = &c->half[y * hw];
        int x = 0;
        for (; 2 * x + 3 < w; x += 2) {
            v8u8 a0, a1, b0, b1;
            __builtin_memcpy(&a0, &r0[2 * x], sizeof a0);
            __builtin_memcpy(&a1, &r0[2 * x + 2], sizeof a1);
            __builtin_memcpy(&b0, &r1[2 * x], sizeof b0);
            __builtin_memcpy(&b1, &r1[2 * x + 2], sizeof b1);
            v8u16 p = __builtin_convertvector(a0, v8u16) + __builtin_convertvector(b0, v8u16);
            v8u16 q = __builtin_convertvector(a1, v8u16) + __builtin_convertvector(b1, v8u16);
            v8u16 sum = __builtin_shufflevector(p, q, 0, 1, 2, 3, 8, 9, 10, 11) +
                        __builtin_shufflevector(p, q, 4, 5, 6, 7, 12, 13, 14, 15);
            v8u8 out = __builtin_convertvector((sum >> 2) & rgb, v8u8);
            __builtin_memcpy(&o[x], &out, sizeof out);
        }
        for (; x < nw; x++) {
            int x0 = 2 * x, x1 = x0 + 1 < w ? x0 + 1 : x0;
            uint32_t a = r0[x0], b = r0[x1], d = r1[x0], e = r1[x1];
            uint32_t rb = (a & 0xFF00FF) + (b & 0xFF00FF) + (d & 0xFF00FF) + (e & 0xFF00FF);
            uint32_t g = (a & 0xFF00) + (b & 0xFF00) + (d & 0xFF00) + (e & 0xFF00);
            o[x] = (rb >> 2 & 0xFF00FF) | (g >> 2 & 0xFF00);
        }
    }
    pad_col(c->half, hw, nw, nh);
    pad_row(c->half, hw, nh, hh);
    // Radius in half-resolution pixels, from the scanline pitch -- which
    // the app derives from its font, so the bloom scales with the text.
    // Capped so a 16-bit lane cannot overflow: 255 * 201 < 65536.
    int r = c->period / 2 + c->look.glow;
    if (r > 100) r = 100;
    for (int pass = 0; pass < 2; pass++) {
        box_rows(c->half, c->half2, hw, nw, hh, r);
        box_cols(c->half2, c->half, hw, hh, r, c->col_sums);
        pad_row(c->half, hw, nh, hh);
    }
    uint32_t k = (uint32_t)GLOW[c->look.glow];   // < 256, so a lane stays in 16 bits
    for (long i = 0; i < (long)hw * hh; i++) {
        uint32_t q = c->half[i];
        c->half[i] = ((q & 0xFF00FF) * k >> 8 & 0xFF00FF) | ((q & 0xFF00) * k >> 8 & 0xFF00);
    }
}

// --- compose -------------------------------------------------------------

static inline uint32_t chan(uint32_t p, int sh) { return (p >> sh) & 0xFF; }

static uint32_t rnd(struct ucrt *c) {
    c->rng ^= c->rng << 13;
    c->rng ^= c->rng >> 17;
    c->rng ^= c->rng << 5;
    return c->rng;
}

// Separable falloff: 256 at the middle, down by `v` at the edge, as u^4.
static int vig_gain(int i, int n, int v) {
    float u = (2.0f * i + 1) / n - 1;
    float f = u * u;
    return 256 - (int)(v * f * f);
}

// --- the compose, two pixels at a time ----------------------------------
//
// The glow's vector types, so SSE2 with no intrinsics header: two pixels
// widen to eight 16-bit lanes, where every product here fits -- a channel
// is at most 255 (256 after the screen, clamped back) and every gain at
// most 256. The column gain and the masks are spread to lanes once per
// frame (c->lanes); the row's gain multiplies the column's in the loop,
// and the glow's half pixel serves exactly the two pixels of a step. A
// pixel's fourth lane has a gain of 0, which keeps the top byte 0.

// One row with no noise: the glow (its half-resolution row) screened in,
// the mask, the column gain times the row's `rg`.
static void compose_row(const uint32_t *in, uint32_t *out, int w, const uint32_t *glow,
                        const uint64_t *mask, const uint64_t *col, int rg) {
    // (col * rg) >> 8 in 16 bits: exact unless both are 256, and a row
    // gain of 256 is the column gain unchanged.
    v8u16 rgv = (v8u16){ 0 } + (uint16_t)rg;
    int i = 0;
    for (; i + 2 <= w; i += 2) {
        v8u8 b;
        v8u16 g;
        __builtin_memcpy(&b, &in[i], sizeof b);
        __builtin_memcpy(&g, &col[i], sizeof g);
        if (rg < 256) g = g * rgv >> 8;
        v8u16 a = __builtin_convertvector(b, v8u16);
        if (glow) {
            v8u16 q = wide2(glow[i / 2], glow[i / 2]);
            a = a + q - (a * q >> 8);   // screen: 1-(1-a)(1-b)
            a -= a >> 8;                // 256 back to 255
        }
        if (mask) {
            v8u16 m;
            __builtin_memcpy(&m, &mask[i], sizeof m);
            a = a * m >> 8;
        }
        a = a * g >> 8;
        v8u8 o = __builtin_convertvector(a, v8u8);
        __builtin_memcpy(&out[i], &o, sizeof o);
    }
    for (; i < w; i++) {   // an odd width's last pixel
        uint32_t v = 0;
        for (int k = 0; k < 3; k++) {
            uint32_t ch = in[i] >> (8 * k) & 0xFF;
            if (glow) {
                uint32_t q = glow[i / 2] >> (8 * k) & 0xFF;
                ch = ch + q - (ch * q >> 8);
                ch -= ch >> 8;
            }
            if (mask) ch = ch * ((uint32_t)(mask[i] >> (16 * k)) & 0xFFFF) >> 8;
            uint32_t g = (uint32_t)(col[i] >> (16 * k)) & 0xFFFF;
            ch = ch * (rg < 256 ? g * (uint32_t)rg >> 8 : g) >> 8;
            v |= ch << (8 * k);
        }
        out[i] = v;
    }
}

// --- the curve -------------------------------------------------------------
//
// Bilinear in 16-bit lanes, a source pixel and its right neighbour in one
// vector: with 4-bit weights a lane peaks at 255 * 16 * 16, which still
// fits. A map entry's low byte is its two weights, so the products
// (16-ax, ax) x (16-ay) and x ay come from a 256-entry table (8 KiB,
// cache-resident) instead of being spread to lanes per pixel.
_Static_assert(FIX == 16, "the lanes and the table are sized for 4-bit weights");
static v8u16 g_wtop[256], g_wbot[256];
static int g_weights_ready;

static void build_weights(void) {
    for (int e = 0; e < 256; e++) {
        uint16_t ax = (uint16_t)(e >> 4), ay = (uint16_t)(e & 15), nx = 16 - ax, ny = 16 - ay;
        v8u16 wx = { nx, nx, nx, nx, ax, ax, ax, ax };
        g_wtop[e] = wx * ny;
        g_wbot[e] = wx * ay;
    }
    g_weights_ready = 1;
}

// The glass pixel map entry `e` names (never UCRT_BEZEL).
static inline uint32_t sample(const uint32_t *buf, int w, uint32_t e) {
    const uint32_t *p = &buf[e >> 8];
    v8u8 top, bot;
    __builtin_memcpy(&top, p, sizeof top);
    __builtin_memcpy(&bot, p + w, sizeof bot);
    v8u16 v = __builtin_convertvector(top, v8u16) * g_wtop[e & 0xFF] +
              __builtin_convertvector(bot, v8u16) * g_wbot[e & 0xFF];
    v += __builtin_shufflevector(v, v, 4, 5, 6, 7, 0, 1, 2, 3);
    v8u8 o = __builtin_convertvector(v >> 8, v8u8);
    uint32_t px;
    __builtin_memcpy(&px, &o, sizeof px);
    return px;
}

int ucrt_apply(struct ucrt *c, struct ugfx_surface *s, int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w < 8 || h < 8 || !ucrt_look_on(&c->look)) return 0;
    return ucrt_apply_from(c, &s->pixels[y * s->w + x], s->w, s, x, y, w, h);
}

int ucrt_apply_from(struct ucrt *c, const uint32_t *src, int stride,
                    struct ugfx_surface *s, int x, int y, int w, int h) {
    if (x < 0) { src -= x; w += x; x = 0; }
    if (y < 0) { src -= (long)y * stride; h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0) return 0;
    int plain = w < 8 || h < 8 || !ucrt_look_on(&c->look) || w > MAP_MAX || h > MAP_MAX;
    if (plain || ensure(c, w, h) != 0) {
        for (int r = 0; r < h; r++)
            if (&s->pixels[(y + r) * s->w + x] != &src[(long)r * stride])
                __builtin_memcpy(&s->pixels[(y + r) * s->w + x], &src[(long)r * stride], (size_t)w * 4);
        return plain ? 0 : -1;
    }
    if (c->period < 2) c->period = 2;
    c->frame++;

    const struct ucrt_look *l = &c->look;
    unsigned long long t0 = sys_monotonic_ns();
    if (l->glow) build_glow(c, src, stride);
    unsigned long long t1 = sys_monotonic_ns();
    c->stage_ns[UCRT_STAGE_GLOW] += t1 - t0;

    // The tables: per column the vignette's gain, per row the scanline x
    // vignette x flicker gain. Rebuilt per frame because flicker moves
    // the row gain. Static, so ONE thread at a time may apply.
    static int col_gain[MAP_MAX + 1], row_gain[MAP_MAX + 1];
    int vig = VIG[l->vignette];
    for (int i = 0; i < w; i++) col_gain[i] = vig ? vig_gain(i, w, vig) : 256;
    int flick = l->flicker ? 256 - (int)(rnd(c) % 10) : 256;
    int scan = SCAN[l->scanlines];
    int lines = c->src_lines;
    for (int r = 0; r < h; r++) {
        int phase = r % c->period, g = 256;
        if (lines > 0) {
            // The row's source line, against the next one's and the one
            // after: the last pixel row of a line is its scanline, and a
            // line four or more rows tall shades the row above it too.
            long l0 = (long)r * lines / h, l1 = (long)(r + 1) * lines / h,
                 l2 = (long)(r + 2) * lines / h;
            if (l1 != l0) g -= scan;
            else if (h >= 4 * lines && l2 != l0) g -= scan / 2;
        } else if (phase == c->period - 1) g -= scan;
        else if (c->period >= 4 && phase == c->period - 2) g -= scan / 2;
        int v = vig ? vig_gain(r, h, vig) : 256;
        row_gain[r] = g * v / 256 * flick / 256;
    }

    if (!l->noise) {
        for (int i = 0; i < w; i++) c->lanes[i] = (uint64_t)(uint32_t)col_gain[i] * 0x100010001ull;
        if (l->mask)
            for (int shift = 0; shift < 2; shift++)
                for (int i = 0; i < w; i++) {
                    int t = (i + shift) % 3;
                    uint64_t m = 256 - MASK_DIM;
                    c->lanes[(1 + shift) * w + i] = (t == 0 ? 256 : m) << 32 | (t == 1 ? 256 : m) << 16 |
                                                    (t == 2 ? 256 : m);
                }
    }

    // Composed from the caller's picture straight to the surface, or to
    // c->buf for the curve to warp from.
    for (int r = 0; r < h; r++) {
        const uint32_t *in = &src[(long)r * stride];
        uint32_t *out = l->curve ? &c->buf[r * w] : &s->pixels[(y + r) * s->w + x];
        const uint32_t *gl = l->glow ? &c->half[(r / 2) * c->hw] : 0;
        int rg = row_gain[r];
        if (!l->noise) {
            const uint64_t *mask = 0;
            if (l->mask) {
                int shift = l->mask == UCRT_MASK_SLOT && (r / c->period) % 2 ? 1 : 0;
                mask = c->lanes + (1 + shift) * w;
            }
            compose_row(in, out, w, gl, mask, c->lanes, rg);
            continue;
        }
        // The slot mask is the aperture grille shifted half a triad on
        // alternate scanline periods -- staggered, as on a shadow-mask tube.
        int shift = l->mask == UCRT_MASK_SLOT && (r / c->period) % 2 ? 1 : 0;
        for (int i = 0; i < w; i++) {
            uint32_t p = in[i];
            int R = chan(p, 16), G = chan(p, 8), B = chan(p, 0);
            if (gl) {
                uint32_t q = gl[i / 2];
                int gr = (int)chan(q, 16), gg = (int)chan(q, 8), gb = (int)chan(q, 0);
                R = R + gr - (R * gr >> 8);   // screen: 1-(1-a)(1-b)
                G = G + gg - (G * gg >> 8);
                B = B + gb - (B * gb >> 8);
            }
            int mr = 256, mg = 256, mb = 256;
            if (l->mask) {
                int t = (i + shift) % 3;
                mr = t == 0 ? 256 : 256 - MASK_DIM;
                mg = t == 1 ? 256 : 256 - MASK_DIM;
                mb = t == 2 ? 256 : 256 - MASK_DIM;
            }
            int cg = col_gain[i] * rg >> 8;
            R = R * mr >> 8; G = G * mg >> 8; B = B * mb >> 8;
            R = R * cg >> 8; G = G * cg >> 8; B = B * cg >> 8;
            if (l->noise) {
                int n = (int)(rnd(c) % 13) - 6;
                R += n; G += n; B += n;
            }
            R = R < 0 ? 0 : R > 255 ? 255 : R;
            G = G < 0 ? 0 : G > 255 ? 255 : G;
            B = B < 0 ? 0 : B > 255 ? 255 : B;
            out[i] = ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;
        }
    }
    unsigned long long t2 = sys_monotonic_ns();
    c->stage_ns[UCRT_STAGE_COMPOSE] += t2 - t1;
    if (!l->curve) return 0;

    if (!g_weights_ready) build_weights();
    const uint32_t *buf = c->buf, bezel = c->bezel;
    for (int r = 0; r < h; r++) {
        uint32_t *out = &s->pixels[(y + r) * s->w + x];
        const uint32_t *m = &c->map[r * w];
        int i = 0;
        for (; i + 2 <= w; i += 2) {   // two at a time: independent, so they overlap
            uint32_t e0 = m[i], e1 = m[i + 1];
            out[i] = e0 == UCRT_BEZEL ? bezel : sample(buf, w, e0);
            out[i + 1] = e1 == UCRT_BEZEL ? bezel : sample(buf, w, e1);
        }
        if (i < w) out[i] = m[i] == UCRT_BEZEL ? bezel : sample(buf, w, m[i]);
    }
    c->stage_ns[UCRT_STAGE_WARP] += sys_monotonic_ns() - t2;
    return 0;
}
