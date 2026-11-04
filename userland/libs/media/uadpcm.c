// media/uadpcm.c -- MS and IMA ADPCM block decoding (uadpcm.h): the
// algorithms of Microsoft's MSADPCM.ACM and of IMA's Recommended Practice
// (1992), as WAV files lay their blocks out. tools/hostchecks/uadpcm_hostcheck.py
// holds every sample to ffmpeg's decode.
#include "media/uadpcm.h"

#include <string.h>

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static int16_t clamp16(int32_t v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

static const int16_t MS_COEF[7][2] = {
    { 256, 0 }, { 512, -256 }, { 0, 0 }, { 192, 64 }, { 240, 0 }, { 460, -208 }, { 392, -232 },
};
static const int MS_ADAPT[16] = { 230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230 };

static const int IMA_INDEX[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static const int IMA_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767,
};

int uadpcm_init(struct uadpcm *a, const uint8_t *wfx, size_t len) {
    memset(a, 0, sizeof *a);
    if (len < 18) return -1;
    a->format = le16(wfx);
    a->channels = le16(wfx + 2);
    a->rate = le32(wfx + 4);
    a->block_align = le16(wfx + 12);
    unsigned bits = le16(wfx + 14), extra = le16(wfx + 16), ch = (unsigned)a->channels;
    if ((ch != 1 && ch != 2) || bits != 4 || a->rate < 1000 || a->rate > 192000) return -1;
    if (extra > len - 18) return -1;
    const uint8_t *x = wfx + 18;
    if (a->format == UADPCM_MS) {
        if (a->block_align < 7 * ch || extra < 4) return -1;
        unsigned spb = le16(x), n = le16(x + 2);
        if (n < 7 || n > 256 || extra < 4 + 4 * n) return -1;
        for (unsigned i = 0; i < n; i++) {
            a->coef[i][0] = (int16_t)le16(x + 4 + 4 * i);
            a->coef[i][1] = (int16_t)le16(x + 6 + 4 * i);
        }
        for (int i = 0; i < 7; i++)              // the format fixes the first seven: a file that differs is refused
            if (a->coef[i][0] != MS_COEF[i][0] || a->coef[i][1] != MS_COEF[i][1]) return -1;
        a->ncoef = (int)n;
        a->frames_per_block = 2 + (a->block_align - 7 * ch) * 2 / ch;
        if (spb && spb < a->frames_per_block) a->frames_per_block = spb;
        return 0;
    }
    if (a->format == UADPCM_IMA) {
        if (a->block_align < 4 * ch || (a->block_align - 4 * ch) % (4 * ch)) return -1;
        a->frames_per_block = (a->block_align - 4 * ch) * 2 / ch + 1;
        return 0;
    }
    return -1;
}

static size_t ms_block(const struct uadpcm *a, const uint8_t *in, size_t n, int16_t *out) {
    unsigned ch = (unsigned)a->channels;
    if (n < 7 * ch) return 0;
    int c1[2], c2[2], delta[2], s1[2], s2[2];
    for (unsigned c = 0; c < ch; c++) {
        unsigned p = in[c];
        if ((int)p >= a->ncoef) p = 0;
        c1[c] = a->coef[p][0];
        c2[c] = a->coef[p][1];
        delta[c] = (int16_t)le16(in + ch + 2 * c);
        s1[c] = (int16_t)le16(in + 3 * ch + 2 * c);
        s2[c] = (int16_t)le16(in + 5 * ch + 2 * c);
    }
    for (unsigned c = 0; c < ch; c++) { out[c] = (int16_t)s2[c]; out[ch + c] = (int16_t)s1[c]; }
    size_t frames = 2, at = 7 * ch, max = a->frames_per_block;
    unsigned c = 0;
    int16_t *o = out + 2 * ch;
    for (; at < n && frames < max; at++) {
        for (int half = 0; half < 2; half++) {
            int nib = half ? in[at] & 0x0F : in[at] >> 4;
            int sn = nib >= 8 ? nib - 16 : nib;
            int32_t pred = (s1[c] * c1[c] + s2[c] * c2[c]) / 256 + sn * delta[c];
            int16_t v = clamp16(pred);
            s2[c] = s1[c];
            s1[c] = v;
            delta[c] = MS_ADAPT[nib] * delta[c] / 256;
            if (delta[c] < 16) delta[c] = 16;
            *o++ = v;
            if (++c == ch) { c = 0; if (++frames == max) break; }
        }
    }
    return frames;
}

static size_t ima_block(const struct uadpcm *a, const uint8_t *in, size_t n, int16_t *out) {
    unsigned ch = (unsigned)a->channels;
    if (n < 4 * ch) return 0;
    int pred[2], index[2];
    for (unsigned c = 0; c < ch; c++) {
        pred[c] = (int16_t)le16(in + 4 * c);
        index[c] = in[4 * c + 2];
        if (index[c] > 88) index[c] = 88;
        out[c] = (int16_t)pred[c];
    }
    // After the header: per channel in turn, 4 bytes -- 8 samples, low nibble first.
    size_t groups = (n - 4 * ch) / (4 * ch), frames = 1 + groups * 8;
    if (frames > a->frames_per_block) { frames = a->frames_per_block; groups = (frames - 1) / 8; }
    for (size_t g = 0; g < groups; g++)
        for (unsigned c = 0; c < ch; c++) {
            const uint8_t *b = in + 4 * ch + (g * ch + c) * 4;
            for (int k = 0; k < 8; k++) {
                int nib = k & 1 ? b[k / 2] >> 4 : b[k / 2] & 0x0F;
                int step = IMA_STEP[index[c]], diff = step >> 3;
                if (nib & 4) diff += step;
                if (nib & 2) diff += step >> 1;
                if (nib & 1) diff += step >> 2;
                pred[c] = clamp16(nib & 8 ? pred[c] - diff : pred[c] + diff);
                index[c] += IMA_INDEX[nib];
                if (index[c] < 0) index[c] = 0;
                if (index[c] > 88) index[c] = 88;
                out[(1 + g * 8 + (size_t)k) * ch + c] = (int16_t)pred[c];
            }
        }
    return frames;
}

size_t uadpcm_block(const struct uadpcm *a, const uint8_t *in, size_t n, int16_t *out) {
    if (n > a->block_align) n = a->block_align;
    return a->format == UADPCM_MS ? ms_block(a, in, n, out) : ima_block(a, in, n, out);
}
