// ugfx_pix's SSE2 table: the C table's arithmetic, four pixels at a time,
// BYTE-IDENTICAL to it (ugfx_pix.h; tools/ugfx_pix_hostcheck.py holds it
// to that). Every channel is a 16-bit lane, which is where each exactness
// argument below lives:
//
//  - a box sum is at most 255 * (2r+1), so 16 bits hold it up to r = 128
//    -- past that the call goes to the C table. Lanes wrap on the add
//    before the subtract, harmlessly: the true sum always fits.
//  - (s * inv + 32768) >> 16 is mulhi(s, inv) plus the carry out of the
//    low half, which is mullo(s, inv) >> 15.
//  - y / 255 for y <= 65534 is exactly (y + 1 + (y >> 8)) >> 8.
//  - the 2x upsample's weights are only ever 0, 64 and 192 (sample
//    centres sit a quarter pixel either side of a half-size one), so the
//    bilinear sum is a 4-bit-weighted one and >> 16 becomes >> 4.
//
// The alpha lane is computed and thrown away, as the C table drops it.
#include "ui/ugfx_pix.h"
#include <emmintrin.h>
#include <stddef.h>

#define RGB 0x00FFFFFF

static inline __m128i px1(uint32_t p) {
    return _mm_unpacklo_epi8(_mm_cvtsi32_si128((int)p), _mm_setzero_si128());
}

static inline __m128i avg16(__m128i s, __m128i vinv) {
    return _mm_add_epi16(_mm_mulhi_epu16(s, vinv), _mm_srli_epi16(_mm_mullo_epi16(s, vinv), 15));
}

static inline __m128i div255(__m128i y) {
    return _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(y, _mm_set1_epi16(1)),
                                        _mm_srli_epi16(y, 8)), 8);
}

// --- the blur ----------------------------------------------------------

static void box_h(const uint32_t *src, uint32_t *dst, int n, int r) {
    uint32_t d = (uint32_t)(2 * r + 1);
    if (d > 257) { ugfx_pix_c.box_h(src, dst, n, r); return; }
    const __m128i vinv = _mm_set1_epi16((short)((65536u + d / 2) / d));
    __m128i s = _mm_setzero_si128();
    for (int k = -r; k <= r; k++) s = _mm_add_epi16(s, px1(src[k < 0 ? 0 : (k >= n ? n - 1 : k)]));
    for (int k = 0; k < n; k++) {
        __m128i q = avg16(s, vinv);
        dst[k] = (uint32_t)_mm_cvtsi128_si32(_mm_packus_epi16(q, q)) & RGB;
        int in = k + r + 1, out = k - r;
        s = _mm_add_epi16(s, _mm_sub_epi16(px1(src[in >= n ? n - 1 : in]), px1(src[out < 0 ? 0 : out])));
    }
}

// The sums are 16-bit, four lanes a column, in the caller's scratch.
static void box_v(const uint32_t *src, uint32_t *dst, int n, int m, int r, uint32_t *scratch) {
    uint32_t d = (uint32_t)(2 * r + 1);
    if (d > 257) { ugfx_pix_c.box_v(src, dst, n, m, r, scratch); return; }
    const __m128i vinv = _mm_set1_epi16((short)((65536u + d / 2) / d));
    const __m128i z = _mm_setzero_si128(), mask = _mm_set1_epi32(RGB);
    __m128i *s = (__m128i *)scratch;   // two vectors per four columns
    int n4 = n & ~3;
    for (int c = 0; c < n4 / 2; c++) _mm_storeu_si128(s + c, z);
    for (int c = n4; c < n; c++) _mm_storel_epi64((__m128i *)((char *)scratch + 8 * c), z);
    for (int k = -r; k <= r; k++) {
        const uint32_t *row = src + (size_t)(k < 0 ? 0 : (k >= m ? m - 1 : k)) * n;
        for (int c = 0; c < n4; c += 4) {
            __m128i p = _mm_loadu_si128((const __m128i *)(row + c));
            _mm_storeu_si128(s + c / 2, _mm_add_epi16(_mm_loadu_si128(s + c / 2), _mm_unpacklo_epi8(p, z)));
            _mm_storeu_si128(s + c / 2 + 1,
                             _mm_add_epi16(_mm_loadu_si128(s + c / 2 + 1), _mm_unpackhi_epi8(p, z)));
        }
        for (int c = n4; c < n; c++) {
            __m128i *sc = (__m128i *)((char *)scratch + 8 * c);
            _mm_storel_epi64(sc, _mm_add_epi16(_mm_loadl_epi64(sc), px1(row[c])));
        }
    }
    for (int j = 0; j < m; j++) {
        uint32_t *drow = dst + (size_t)j * n;
        int in = j + r + 1, out = j - r;
        const uint32_t *ri = src + (size_t)(in >= m ? m - 1 : in) * n;
        const uint32_t *ro = src + (size_t)(out < 0 ? 0 : out) * n;
        for (int c = 0; c < n4; c += 4) {
            __m128i s0 = _mm_loadu_si128(s + c / 2), s1 = _mm_loadu_si128(s + c / 2 + 1);
            _mm_storeu_si128((__m128i *)(drow + c),
                             _mm_and_si128(_mm_packus_epi16(avg16(s0, vinv), avg16(s1, vinv)), mask));
            __m128i pi = _mm_loadu_si128((const __m128i *)(ri + c));
            __m128i po = _mm_loadu_si128((const __m128i *)(ro + c));
            s0 = _mm_add_epi16(s0, _mm_sub_epi16(_mm_unpacklo_epi8(pi, z), _mm_unpacklo_epi8(po, z)));
            s1 = _mm_add_epi16(s1, _mm_sub_epi16(_mm_unpackhi_epi8(pi, z), _mm_unpackhi_epi8(po, z)));
            _mm_storeu_si128(s + c / 2, s0);
            _mm_storeu_si128(s + c / 2 + 1, s1);
        }
        for (int c = n4; c < n; c++) {
            __m128i *sc = (__m128i *)((char *)scratch + 8 * c);
            __m128i s0 = _mm_loadl_epi64(sc), q = avg16(s0, vinv);
            drow[c] = (uint32_t)_mm_cvtsi128_si32(_mm_packus_epi16(q, q)) & RGB;
            _mm_storel_epi64(sc, _mm_add_epi16(s0, _mm_sub_epi16(px1(ri[c]), px1(ro[c]))));
        }
    }
}

static inline uint32_t down1(const uint32_t *r0, const uint32_t *r1, int n, int k) {
    int i0 = 2 * k, i1 = i0 + 1 < n ? i0 + 1 : i0;
    __m128i s = _mm_add_epi16(_mm_add_epi16(px1(r0[i0]), px1(r0[i1])),
                              _mm_add_epi16(px1(r1[i0]), px1(r1[i1])));
    s = _mm_srli_epi16(_mm_add_epi16(s, _mm_set1_epi16(2)), 2);
    return (uint32_t)_mm_cvtsi128_si32(_mm_packus_epi16(s, s)) & RGB;
}

static void down2(const uint32_t *r0, const uint32_t *r1, uint32_t *dst, int n) {
    const __m128i z = _mm_setzero_si128(), two = _mm_set1_epi16(2), mask = _mm_set1_epi32(RGB);
    int k = 0;
    for (; 2 * k + 8 <= n; k += 4) {
        __m128i a0 = _mm_loadu_si128((const __m128i *)(r0 + 2 * k));
        __m128i a1 = _mm_loadu_si128((const __m128i *)(r0 + 2 * k + 4));
        __m128i b0 = _mm_loadu_si128((const __m128i *)(r1 + 2 * k));
        __m128i b1 = _mm_loadu_si128((const __m128i *)(r1 + 2 * k + 4));
        // Each a pair of source columns, the two rows already added.
        __m128i s0 = _mm_add_epi16(_mm_unpacklo_epi8(a0, z), _mm_unpacklo_epi8(b0, z));
        __m128i s1 = _mm_add_epi16(_mm_unpackhi_epi8(a0, z), _mm_unpackhi_epi8(b0, z));
        __m128i s2 = _mm_add_epi16(_mm_unpacklo_epi8(a1, z), _mm_unpacklo_epi8(b1, z));
        __m128i s3 = _mm_add_epi16(_mm_unpackhi_epi8(a1, z), _mm_unpackhi_epi8(b1, z));
        __m128i lo = _mm_add_epi16(_mm_unpacklo_epi64(s0, s1), _mm_unpackhi_epi64(s0, s1));
        __m128i hi = _mm_add_epi16(_mm_unpacklo_epi64(s2, s3), _mm_unpackhi_epi64(s2, s3));
        lo = _mm_srli_epi16(_mm_add_epi16(lo, two), 2);
        hi = _mm_srli_epi16(_mm_add_epi16(hi, two), 2);
        _mm_storeu_si128((__m128i *)(dst + k), _mm_and_si128(_mm_packus_epi16(lo, hi), mask));
    }
    for (; k < (n + 1) / 2; k++) dst[k] = down1(r0, r1, n, k);
}

// One output pixel of up2, by the 4-bit weights: wx (on v[k]) is 4, 3
// or 1 and the other 4 - wx goes to v[k1].
static inline uint32_t up1(const uint32_t *a, const uint32_t *b, int dn, int i, __m128i wy0,
                           __m128i wy1) {
    int k = i ? (i - 1) / 2 : 0, k1 = k + 1 < dn ? k + 1 : k;
    int wx = i == 0 ? 4 : (i & 1) ? 3 : 1;
    __m128i v0 = _mm_add_epi16(_mm_mullo_epi16(px1(a[k]), wy0), _mm_mullo_epi16(px1(b[k]), wy1));
    __m128i v1 = _mm_add_epi16(_mm_mullo_epi16(px1(a[k1]), wy0), _mm_mullo_epi16(px1(b[k1]), wy1));
    __m128i s = _mm_add_epi16(_mm_mullo_epi16(v0, _mm_set1_epi16((short)wx)),
                              _mm_mullo_epi16(v1, _mm_set1_epi16((short)(4 - wx))));
    s = _mm_srli_epi16(s, 4);
    return (uint32_t)_mm_cvtsi128_si32(_mm_packus_epi16(s, s)) & RGB;
}

static void up2(const uint32_t *a, const uint32_t *b, uint32_t *dst, int n, int dn, unsigned fy) {
    const __m128i z = _mm_setzero_si128(), mask = _mm_set1_epi32(RGB);
    const __m128i wy0 = _mm_set1_epi16((short)((256 - fy) >> 6)), wy1 = _mm_set1_epi16((short)(fy >> 6));
    dst[0] = up1(a, b, dn, 0, wy0, wy1);
    int k = 0;
    // Out 2k+1 .. 2k+8 from v[k] .. v[k+4].
    for (; k + 4 < dn && 2 * k + 8 < n; k += 4) {
        __m128i a0 = _mm_loadu_si128((const __m128i *)(a + k)), b0 = _mm_loadu_si128((const __m128i *)(b + k));
        __m128i a1 = _mm_loadu_si128((const __m128i *)(a + k + 1));
        __m128i b1 = _mm_loadu_si128((const __m128i *)(b + k + 1));
        __m128i vlo = _mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(a0, z), wy0),
                                    _mm_mullo_epi16(_mm_unpacklo_epi8(b0, z), wy1));
        __m128i vhi = _mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(a0, z), wy0),
                                    _mm_mullo_epi16(_mm_unpackhi_epi8(b0, z), wy1));
        __m128i wlo = _mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(a1, z), wy0),
                                    _mm_mullo_epi16(_mm_unpacklo_epi8(b1, z), wy1));
        __m128i whi = _mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(a1, z), wy0),
                                    _mm_mullo_epi16(_mm_unpackhi_epi8(b1, z), wy1));
        __m128i olo = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_slli_epi16(vlo, 1), vlo), wlo), 4);
        __m128i ohi = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_slli_epi16(vhi, 1), vhi), whi), 4);
        __m128i elo = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_slli_epi16(wlo, 1), wlo), vlo), 4);
        __m128i ehi = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_slli_epi16(whi, 1), whi), vhi), 4);
        __m128i odd = _mm_packus_epi16(olo, ohi), even = _mm_packus_epi16(elo, ehi);
        _mm_storeu_si128((__m128i *)(dst + 2 * k + 1), _mm_and_si128(_mm_unpacklo_epi32(odd, even), mask));
        _mm_storeu_si128((__m128i *)(dst + 2 * k + 5), _mm_and_si128(_mm_unpackhi_epi32(odd, even), mask));
    }
    for (int i = 2 * k + 1; i < n; i++) dst[i] = up1(a, b, dn, i, wy0, wy1);
}

// --- mixing and blending ------------------------------------------------

// mix() four pixels at a time: `ov` is over * a + 127 per lane, `vna`
// is 255 - a.
static inline __m128i mix4c(__m128i u, __m128i ov, __m128i vna) {
    const __m128i z = _mm_setzero_si128();
    __m128i lo = _mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(u, z), vna), ov);
    __m128i hi = _mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(u, z), vna), ov);
    return _mm_and_si128(_mm_packus_epi16(div255(lo), div255(hi)), _mm_set1_epi32(RGB));
}

static inline __m128i mix4(__m128i u, __m128i o, __m128i va, __m128i vna) {
    const __m128i z = _mm_setzero_si128(), k = _mm_set1_epi16(127);
    __m128i lo = _mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(u, z), vna),
                                             _mm_mullo_epi16(_mm_unpacklo_epi8(o, z), va)), k);
    __m128i hi = _mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(u, z), vna),
                                             _mm_mullo_epi16(_mm_unpackhi_epi8(o, z), va)), k);
    return _mm_and_si128(_mm_packus_epi16(div255(lo), div255(hi)), _mm_set1_epi32(RGB));
}

static inline uint32_t mix1(uint32_t u, uint32_t o, __m128i va, __m128i vna) {
    return (uint32_t)_mm_cvtsi128_si32(mix4(_mm_cvtsi32_si128((int)u), _mm_cvtsi32_si128((int)o), va, vna));
}

static void mix_const(uint32_t *dst, const uint32_t *under, uint32_t over, unsigned a, int n) {
    const __m128i va = _mm_set1_epi16((short)a), vna = _mm_set1_epi16((short)(255 - a));
    __m128i o = _mm_unpacklo_epi8(_mm_set1_epi32((int)over), _mm_setzero_si128());
    __m128i ov = _mm_add_epi16(_mm_mullo_epi16(o, va), _mm_set1_epi16(127));
    int i = 0;
    for (; i + 4 <= n; i += 4)
        _mm_storeu_si128((__m128i *)(dst + i), mix4c(_mm_loadu_si128((const __m128i *)(under + i)), ov, vna));
    for (; i < n; i++) dst[i] = mix1(under[i], over, va, vna);
}

static void mix_rows(uint32_t *dst, const uint32_t *under, const uint32_t *over, unsigned a,
                     int n) {
    const __m128i va = _mm_set1_epi16((short)a), vna = _mm_set1_epi16((short)(255 - a));
    int i = 0;
    for (; i + 4 <= n; i += 4)
        _mm_storeu_si128((__m128i *)(dst + i), mix4(_mm_loadu_si128((const __m128i *)(under + i)),
                                                    _mm_loadu_si128((const __m128i *)(over + i)), va, vna));
    for (; i < n; i++) dst[i] = mix1(under[i], over[i], va, vna);
}

static void glass(uint32_t *dst, const uint32_t *src, const uint32_t *under, unsigned a, int n) {
    const __m128i va = _mm_set1_epi16((short)a), vna = _mm_set1_epi16((short)(255 - a));
    const __m128i top = _mm_set1_epi32((int)0xFF000000u), z = _mm_setzero_si128();
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        __m128i p = _mm_loadu_si128((const __m128i *)(src + i));
        __m128i m = mix4(_mm_loadu_si128((const __m128i *)(under + i)), p, va, vna);
        __m128i plain = _mm_cmpeq_epi32(_mm_and_si128(p, top), z);
        _mm_storeu_si128((__m128i *)(dst + i), _mm_or_si128(_mm_and_si128(plain, p), _mm_andnot_si128(plain, m)));
    }
    for (; i < n; i++) {
        uint32_t p = src[i];
        dst[i] = p >> 24 ? mix1(under[i], p, va, vna) : p;
    }
}

// blend(), truncating: c * a + d * (255 - a), at most 65025.
static inline __m128i blend4(__m128i d, __m128i ca_lo, __m128i ca_hi, __m128i vna_lo, __m128i vna_hi) {
    const __m128i z = _mm_setzero_si128();
    __m128i lo = _mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(d, z), vna_lo), ca_lo);
    __m128i hi = _mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(d, z), vna_hi), ca_hi);
    return _mm_and_si128(_mm_packus_epi16(div255(lo), div255(hi)), _mm_set1_epi32(RGB));
}

static void blend_const(uint32_t *dst, uint32_t c, unsigned a, int n) {
    const __m128i vna = _mm_set1_epi16((short)(255 - a));
    __m128i ca = _mm_mullo_epi16(_mm_unpacklo_epi8(_mm_set1_epi32((int)c), _mm_setzero_si128()),
                                 _mm_set1_epi16((short)a));
    int i = 0;
    for (; i + 4 <= n; i += 4)
        _mm_storeu_si128((__m128i *)(dst + i), blend4(_mm_loadu_si128((const __m128i *)(dst + i)),
                                                      ca, ca, vna, vna));
    for (; i < n; i++)
        dst[i] = (uint32_t)_mm_cvtsi128_si32(blend4(_mm_cvtsi32_si128((int)dst[i]), ca, ca, vna, vna));
}

static void blend_cov(uint32_t *dst, uint32_t c, const uint8_t *cov, int n) {
    const __m128i z = _mm_setzero_si128(), k255 = _mm_set1_epi16(255);
    const __m128i vc = _mm_set1_epi32((int)c), cw = _mm_unpacklo_epi8(vc, z);
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32_t c4;
        __builtin_memcpy(&c4, cov + i, 4);
        if (!c4) continue;
        __m128i a16 = _mm_unpacklo_epi8(_mm_cvtsi32_si128((int)c4), z);   // a0..a3, 16-bit
        __m128i a2 = _mm_unpacklo_epi16(a16, a16);                         // a0 a0 a1 a1 ..
        __m128i alo = _mm_unpacklo_epi32(a2, a2), ahi = _mm_unpackhi_epi32(a2, a2);
        __m128i d = _mm_loadu_si128((const __m128i *)(dst + i));
        __m128i b = blend4(d, _mm_mullo_epi16(cw, alo), _mm_mullo_epi16(cw, ahi),
                           _mm_sub_epi16(k255, alo), _mm_sub_epi16(k255, ahi));
        __m128i a32 = _mm_unpacklo_epi16(a16, z);
        __m128i m0 = _mm_cmpeq_epi32(a32, z), mfull = _mm_cmpeq_epi32(a32, _mm_set1_epi32(255));
        __m128i out = _mm_or_si128(_mm_and_si128(m0, d), _mm_and_si128(mfull, vc));
        out = _mm_or_si128(out, _mm_andnot_si128(_mm_or_si128(m0, mfull), b));
        _mm_storeu_si128((__m128i *)(dst + i), out);
    }
    for (; i < n; i++) {
        unsigned al = cov[i];
        if (!al) continue;
        if (al >= 255) { dst[i] = c; continue; }
        __m128i va = _mm_set1_epi16((short)al);
        dst[i] = (uint32_t)_mm_cvtsi128_si32(blend4(_mm_cvtsi32_si128((int)dst[i]), _mm_mullo_epi16(cw, va),
                                                    _mm_mullo_epi16(cw, va), _mm_sub_epi16(k255, va),
                                                    _mm_sub_epi16(k255, va)));
    }
}

const struct ugfx_pix_ops ugfx_pix_sse2 = {
    .name = "sse2",
    .box_h = box_h, .box_v = box_v, .down2 = down2, .up2 = up2,
    .mix_const = mix_const, .mix_rows = mix_rows, .glass = glass,
    .blend_const = blend_const, .blend_cov = blend_cov,
};
