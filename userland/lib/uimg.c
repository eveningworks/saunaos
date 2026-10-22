// uimg -- the codec table, the file front end, and the resampler.
// See uimg.h for why any of this is in ring 3 rather than in the kernel.
#include "lib/uimg.h"
#include "lib/uimg_reduce.h"
#include <fcntl.h>
#include <unistd.h>
#include "lib/ufile.h"
#include "rt/sys.h"
#include <kerrno.h>
#include <stdlib.h>
#include <string.h>

// THE TABLE. A format is a row here and a file beside this one -- the
// same shape as kernel/proc/syscall_table.c and the driver registries,
// and what WIC and GdkPixbuf are. Probes run in order and the first
// claim wins, so a row's position matters only if two formats could
// claim the same bytes, which none of these can.
static const struct uimg_codec *const codecs[] = {
    &uimg_codec_jpeg,
    &uimg_codec_qoi,
    &uimg_codec_png,
    &uimg_codec_bmp,
    &uimg_codec_gif,
};
#define CODEC_COUNT ((int)(sizeof codecs / sizeof codecs[0]))

// The sentence behind the last error. A static string, never a copy: a
// decoder sets it to a literal, so there is no buffer to size and
// nothing to run out of. Same idea as libjpeg's jpeg_error_mgr message,
// minus the callback table nobody here would install.
static const char *g_error = "";

void uimg_set_error(const char *msg) { g_error = msg; }
const char *uimg_last_error(void) { return g_error; }

const struct uimg_codec *uimg_probe(const void *data, size_t n) {
    const uint8_t *d = data;
    for (int i = 0; i < CODEC_COUNT; i++)
        if (codecs[i]->probe(d, n)) return codecs[i];
    return NULL;
}

int uimg_info(const void *data, size_t n, struct uimg_info *out) {
    memset(out, 0, sizeof *out);
    const struct uimg_codec *c = uimg_probe(data, n);
    if (!c) {
        uimg_set_error("not an image format this build recognises");
        return -EINVAL;
    }
    return c->info(data, n, out);
}

int uimg_decode(const void *data, size_t n, struct uimg *out) {
    memset(out, 0, sizeof *out);
    const struct uimg_codec *c = uimg_probe(data, n);
    if (!c) {
        uimg_set_error("not an image format this build recognises");
        return -EINVAL;
    }
    // A row with no decoder is a format this build WRITES and cannot
    // read. -ENOTSUP says the file is fine and we are not, which is
    // what lets the Image Viewer say so instead of calling it corrupt.
    if (!c->decode) {
        uimg_set_error("this build can write this format but not read it");
        return -ENOTSUP;
    }
    return c->decode(data, n, out);
}

void uimg_free(struct uimg *im) {
    if (!im) return;
    free(im->px);
    im->px = NULL;
    im->w = im->h = 0;
}

// Reads a whole file into one allocation (lib/ufile.h). The caller
// frees it.
//
// The wording is HERE rather than in the shared reader because these
// sentences reach a person in the Image Viewer, and an errno cannot
// carry them: an empty file and one past the ceiling are both EINVAL,
// and they are not the same problem.
static int read_file(const char *path, uint8_t **out, size_t *out_len) {
    switch (ufile_slurp(path, UIMG_MAX_FILE, out, out_len)) {
    case UFILE_OK:
        return 0;
    case UFILE_NOENT:
        uimg_set_error("no such file");
        return -ENOENT;
    case UFILE_EMPTY:
        uimg_set_error("the file is empty");
        return -EINVAL;
    case UFILE_TOO_BIG:
        // REFUSED rather than read as a prefix: a truncated JPEG
        // decodes, to a grey-tailed picture that looks like a decoder
        // bug rather than like a file that did not fit.
        uimg_set_error("the file is larger than this decoder will read");
        return -EINVAL;
    case UFILE_NOMEM:
        uimg_set_error("not enough memory to read the file");
        return -ENOMEM;
    case UFILE_OPEN:
        uimg_set_error("the file could not be opened");
        return -EIO;
    case UFILE_SHORT:
    default:
        uimg_set_error("the file ended early");
        return -EIO;
    }
}

// THE HEADER IS AT THE FRONT: a 9 MB photograph's dimensions are in its
// first few hundred bytes, and reading the whole file for them cost the
// Image Viewer and the File Manager's details a full read each. A prefix
// is read first; the whole file only when the prefix was not enough --
// a header further in, or GIF, whose frame count walks every frame.
#define UIMG_INFO_HEAD (256u * 1024u)

int uimg_load_info(const char *path, struct uimg_info *out) {
    uint8_t *buf = malloc(UIMG_INFO_HEAD);
    int fd = buf ? open(path, O_RDONLY) : -1;
    if (fd >= 0) {
        size_t n = 0;
        long got;
        while (n < UIMG_INFO_HEAD && (got = read(fd, buf + n, UIMG_INFO_HEAD - n)) > 0) n += (size_t)got;
        close(fd);
        const struct uimg_codec *c = n ? uimg_probe(buf, n) : 0;
        int whole = n < UIMG_INFO_HEAD;
        if (c && (whole || strcmp(c->name, "gif") != 0) && uimg_info(buf, n, out) == 0) {
            free(buf);
            return 0;
        }
        if (whole && n) {   // the whole file, and it did not parse: that is the answer
            int rc = uimg_info(buf, n, out);
            free(buf);
            return rc;
        }
    }
    free(buf);
    uint8_t *all;
    size_t len;
    int rc = read_file(path, &all, &len);
    if (rc < 0) return rc;
    rc = uimg_info(all, len, out);
    free(all);
    return rc;
}

int uimg_load_fit(const char *path, int bw, int bh, struct uimg *out) {
    memset(out, 0, sizeof *out);
    uint8_t *buf;
    size_t len;
    int rc = read_file(path, &buf, &len);
    if (rc < 0) return rc;
    const struct uimg_codec *c = uimg_probe(buf, len);
    struct uimg_info info;
    if (c && c->decode_reduced && uimg_info(buf, len, &info) == 0) {
        int tw, th, kx, ky;
        uimg_fit_size(info.w, info.h, bw, bh, UIMG_FIT_CONTAIN, &tw, &th);
        uimg_reduce_factors(info.w, info.h, tw, th, &kx, &ky);
        if (kx > 1 || ky > 1) {
            struct uimg small;
            rc = c->decode_reduced(buf, len, kx, ky, &small);
            free(buf);
            if (rc < 0) return rc;
            rc = uimg_scale(&small, tw, th, out);
            uimg_free(&small);
            return rc;
        }
    }
    struct uimg full;
    rc = uimg_decode(buf, len, &full);
    free(buf);
    if (rc < 0) return rc;
    int tw, th;
    uimg_fit_size(full.w, full.h, bw, bh, UIMG_FIT_CONTAIN, &tw, &th);
    rc = uimg_scale(&full, tw, th, out);
    uimg_free(&full);
    return rc;
}

int uimg_load(const char *path, struct uimg *out) {
    uint8_t *buf;
    size_t len;
    int rc = read_file(path, &buf, &len);
    if (rc < 0) return rc;
    rc = uimg_decode(buf, len, out);
    free(buf);
    return rc;
}

// --- animation -------------------------------------------------------

// A still, as an animation of one frame: decoded once at open, and
// next() hands back the same canvas.
static int still_next(struct uimg_anim *a, int *delay_ms) {
    a->index = 0;
    *delay_ms = 0;
    return 0;
}

int uimg_anim_open(const void *data, size_t n, struct uimg_anim *a) {
    memset(a, 0, sizeof *a);
    a->index = -1;
    const struct uimg_codec *c = uimg_probe(data, n);
    if (!c) {
        uimg_set_error("not an image format this build recognises");
        return -EINVAL;
    }
    if (c->anim_open) return c->anim_open(data, n, a);

    int rc = uimg_decode(data, n, &a->frame);
    if (rc < 0) return rc;
    a->frames = 1;
    a->next = still_next;
    return 0;
}

int uimg_anim_load(const char *path, struct uimg_anim *a) {
    uint8_t *buf;
    size_t len;
    memset(a, 0, sizeof *a);
    int rc = read_file(path, &buf, &len);
    if (rc < 0) return rc;
    rc = uimg_anim_open(buf, len, a);
    if (rc < 0) {
        free(buf);
        return rc;
    }
    a->file = buf;
    return 0;
}

int uimg_anim_next(struct uimg_anim *a, int *delay_ms) {
    *delay_ms = 0;
    if (!a->next) {
        uimg_set_error("the animation is not open");
        return -EINVAL;
    }
    int rc = a->next(a, delay_ms);
    // Browsers' rule, and Windows Photos': a delay of 0 or 1
    // hundredths is a file that never said, not a request to spin.
    if (rc == 0 && a->frames > 1 && *delay_ms <= 10) *delay_ms = 100;
    return rc;
}

void uimg_anim_close(struct uimg_anim *a) {
    if (!a) return;
    if (a->release) a->release(a);
    uimg_free(&a->frame);
    free(a->file);
    memset(a, 0, sizeof *a);
    a->index = -1;
}

// --- encoding ---------------------------------------------------------

// The format is NAMED. Probing the pixels to pick one would be guessing
// at the caller's intent, and the two formats here differ in what they
// are FOR rather than in what they can hold (see uimg.h).
static const struct uimg_codec *codec_named(const char *name) {
    for (int i = 0; i < CODEC_COUNT; i++)
        if (strcmp(codecs[i]->name, name) == 0) return codecs[i];
    return NULL;
}

int uimg_encode(const struct uimg *im, const char *format,
                uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    if (!im || !im->px || im->w <= 0 || im->h <= 0) {
        uimg_set_error("nothing to encode");
        return -EINVAL;
    }
    const struct uimg_codec *c = format ? codec_named(format) : NULL;
    if (!c) {
        uimg_set_error("not an image format this build recognises");
        return -ENOTSUP;
    }
    if (!c->encode) {
        uimg_set_error("this build can read this format but not write it");
        return -ENOTSUP;
    }
    return c->encode(im, out, out_len);
}

// The extension, lowercased, with no dot. An unknown one is refused
// rather than defaulted: silently writing a QOI into a file called
// .bmp would produce a file nothing opens and no error to explain it.
static const char *format_from_path(const char *path) {
    const char *dot = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '.') dot = p;
        else if (*p == '/') dot = NULL;
    if (!dot || !dot[1]) return NULL;

    static char ext[8];
    size_t n = 0;
    for (const char *p = dot + 1; *p && n + 1 < sizeof ext; p++)
        ext[n++] = (*p >= 'A' && *p <= 'Z') ? (char)(*p - 'A' + 'a') : *p;
    ext[n] = '\0';
    if (strcmp(ext, "jpg") == 0) return "jpeg";
    return ext;
}

int uimg_save(const char *path, const struct uimg *im, const char *format) {
    if (!format) format = format_from_path(path);
    if (!format) {
        uimg_set_error("the filename has no extension to pick a format from");
        return -ENOTSUP;
    }

    uint8_t *buf;
    size_t len;
    int rc = uimg_encode(im, format, &buf, &len);
    if (rc < 0) return rc;

    int fd = sys_open(path, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (fd < 0) {
        free(buf);
        uimg_set_error("the file could not be created");
        return fd;
    }
    // sys_write() completes the whole buffer (the kernel chunks it), so
    // a short return here is a real failure and not a partial write to
    // resume. A half-written image file is worse than none: it has a
    // valid header and decodes to a picture that is simply wrong.
    long wrote = sys_write(fd, buf, len);
    sys_close(fd);
    free(buf);
    if (wrote < 0 || (size_t)wrote != len) {
        uimg_set_error("the file could not be written in full");
        return wrote < 0 ? (int)wrote : -EIO;
    }
    return 0;
}

// --- fitting and resampling -------------------------------------------

void uimg_fit_size(int sw, int sh, int bw, int bh, enum uimg_fit mode,
                   int *out_w, int *out_h) {
    int w = sw, h = sh;
    if (sw <= 0 || sh <= 0 || bw <= 0 || bh <= 0) {
        *out_w = *out_h = 1;
        return;
    }
    switch (mode) {
    case UIMG_FIT_NONE:
        break;
    case UIMG_FIT_STRETCH:
        w = bw; h = bh;
        break;
    case UIMG_FIT_CONTAIN:
    case UIMG_FIT_COVER: {
        // Compare aspect ratios by cross-multiplying, which keeps this
        // integer: sw/sh vs bw/bh is sw*bh vs bw*sh.
        int wide = (int64_t)sw * bh > (int64_t)bw * sh;
        int fit_w = (mode == UIMG_FIT_CONTAIN) ? wide : !wide;
        if (fit_w) {
            w = bw;
            h = (int)(((int64_t)sh * bw + sw / 2) / sw);
        } else {
            h = bh;
            w = (int)(((int64_t)sw * bh + sh / 2) / sh);
        }
        break;
    }
    }
    *out_w = w < 1 ? 1 : w;
    *out_h = h < 1 ? 1 : h;
}

// One separable pass. `src` is sw x sh, `dst` is dw x sh (a horizontal
// pass) -- the vertical pass runs the same code on a transposed view by
// swapping the strides its caller passes in.
//
// SHRINKING BOX-AVERAGES AND ENLARGING INTERPOLATES, per axis. That
// asymmetry is the point: a box filter on an enlargement is
// nearest-neighbour (every output pixel's box holds one source pixel),
// and linear interpolation on a big reduction samples a fraction of the
// source and aliases -- which is exactly what a downscaled photo shows
// as shimmering detail. Both are 16.16 fixed point.
//
// **COLOUR IS WEIGHTED BY ALPHA, WHICH IS WHY THIS LOOKS ODD.** Mixing
// straight (non-premultiplied) RGBA by averaging all four channels
// independently is wrong wherever alpha varies: a transparent pixel
// still contributes its colour, and since a generator writes (0,0,0,0)
// outside a shape, every downscaled icon grows a dark halo along its
// edge. So the alpha channel is averaged by area, and the colour
// channels are averaged weighted by alpha -- which is the same thing as
// premultiplying, resampling and unpremultiplying, without the two extra
// passes over the image. For an opaque image alpha is constant and
// cancels exactly, which is what the opaque paths' shortcuts rest on.
// `opaque` reads every alpha byte as 255: an image whose has_alpha is 0
// is opaque by definition, and a buffer in 0x00RRGGBB (a client
// window's) would otherwise weight every colour by zero and scale to
// black.
//
// Where one output position reads from. It depends on the position
// alone, so a pass works it out once per position, not once per pixel.
enum { TAP_LERP, TAP_BOX_OPAQUE, TAP_BOX };
struct tap {
    int s0, s1;           // the first and last source sample
    int frac;             // LERP: the second sample's 16.16 weight
    uint64_t inv;         // BOX_OPAQUE: 2^32 / the weights' sum
    int64_t start, end;   // BOX: the span, 16.16
    uint32_t wt[17];      // BOX_OPAQUE: one weight per sample
};

static int make_taps(struct tap *t, int src_n, int dst_n, int opaque) {
    if (dst_n >= src_n) {
        // Enlarge: linear interpolation between the two nearest samples.
        // The map is (i + 0.5) * src/dst - 0.5 in 16.16, clamped at the
        // ends so the edge pixels are not fetched from outside.
        int64_t ratio = ((int64_t)src_n << 16) / dst_n;
        for (int i = 0; i < dst_n; i++) {
            int64_t pos = ((2 * (int64_t)i + 1) * ratio) / 2 - (1 << 15);
            if (pos < 0) pos = 0;
            int s0 = (int)(pos >> 16), s1 = s0 + 1;
            if (s1 > src_n - 1) { s1 = src_n - 1; if (s0 > s1) s0 = s1; }
            t[i].s0 = s0;
            t[i].s1 = s1;
            t[i].frac = (int)(pos & 0xFFFF);
        }
        return TAP_LERP;
    }

    // Shrink: average every source pixel whose span touches this output
    // pixel, weighted by how much of it does. Weights are 16.16 and sum
    // to exactly one output pixel's worth of source, so the divide is by
    // a constant.
    int64_t span = ((int64_t)src_n << 16) / dst_n;   // source pixels per output pixel
    int mode = (opaque && span <= (16 << 16)) ? TAP_BOX_OPAQUE : TAP_BOX;
    for (int i = 0; i < dst_n; i++) {
        int64_t start = (int64_t)i * src_n * 65536 / dst_n;
        int64_t end = start + span;
        int s0 = (int)(start >> 16);
        int s1 = (int)((end - 1) >> 16);
        if (s1 > src_n - 1) s1 = src_n - 1;
        t[i].s0 = s0;
        t[i].s1 = s1;
        t[i].start = start;
        t[i].end = end;
        if (mode != TAP_BOX_OPAQUE) continue;
        uint32_t wsum = 0;
        int nw = 0;
        for (int s = s0; s <= s1 && nw < 17; s++) {
            int64_t lo = (int64_t)s << 16, hi = lo + 65536;
            int64_t w = (end < hi ? end : hi) - (start > lo ? start : lo);
            t[i].wt[nw++] = w > 0 ? (uint32_t)w : 0;
            wsum += w > 0 ? (uint32_t)w : 0;
        }
        t[i].s1 = s0 + nw - 1;
        t[i].inv = wsum ? ((1ull << 32) + wsum / 2) / wsum : 0;
    }
    return mode;
}

// One output pixel: tap `t` applied to the line starting at `sp`.
static inline __attribute__((always_inline)) uint32_t tap_pixel(int mode, const struct tap *t, const uint32_t *sp,
                                 int step, int opaque) {
    if (mode == TAP_LERP) {
        uint32_t a = sp[(size_t)t->s0 * step];
        uint32_t b = sp[(size_t)t->s1 * step];
        int wa = 65536 - t->frac, wb = t->frac;
        if (opaque) {
            // Alpha is 255 on both sides and cancels out of the weighted
            // mean exactly, so the general path's divide is a shift here.
            uint32_t o = 0xFF000000u;
            for (int shift = 0; shift <= 16; shift += 8)
                o |= (uint32_t)(((int)((a >> shift) & 0xFF) * wa +
                                 (int)((b >> shift) & 0xFF) * wb) >> 16) << shift;
            return o;
        }
        int aa = (int)(a >> 24), ab = (int)(b >> 24);
        int alpha = (aa * wa + ab * wb) >> 16;
        int64_t caw = (int64_t)aa * wa, cbw = (int64_t)ab * wb;
        int64_t den = caw + cbw;
        uint32_t o = (uint32_t)(alpha & 0xFF) << 24;
        for (int shift = 0; shift <= 16; shift += 8) {
            int ca = (int)((a >> shift) & 0xFF), cb = (int)((b >> shift) & 0xFF);
            int v = den ? (int)(((int64_t)ca * caw + (int64_t)cb * cbw) / den) : 0;
            o |= (uint32_t)(v & 0xFF) << shift;
        }
        return o;
    }
    if (mode == TAP_BOX_OPAQUE) {
        // Sums stay in 32 bits: 255 x 16 source pixels x 65536.
        sp += (size_t)t->s0 * step;
        uint32_t r = 0, g = 0, b = 0;
        for (int k = 0; k <= t->s1 - t->s0; k++) {
            uint32_t v = sp[(size_t)k * step], w = t->wt[k];
            r += ((v >> 16) & 0xFF) * w;
            g += ((v >> 8) & 0xFF) * w;
            b += (v & 0xFF) * w;
        }
        // Rounded: the reciprocal itself is, so flooring lost a step.
        uint32_t rr = (uint32_t)((r * t->inv + (1ull << 31)) >> 32),
                 gg = (uint32_t)((g * t->inv + (1ull << 31)) >> 32),
                 bb = (uint32_t)((b * t->inv + (1ull << 31)) >> 32);
        return 0xFF000000u | ((rr > 255 ? 255 : rr) << 16) | ((gg > 255 ? 255 : gg) << 8) |
               (bb > 255 ? 255 : bb);
    }
    int64_t acc[3] = { 0, 0, 0 };
    int64_t asum = 0, wsum = 0, awsum = 0;
    for (int s = t->s0; s <= t->s1; s++) {
        int64_t lo = (int64_t)s << 16, hi = lo + 65536;
        int64_t w = (t->end < hi ? t->end : hi) - (t->start > lo ? t->start : lo);
        if (w <= 0) continue;
        uint32_t v = sp[(size_t)s * step];
        int64_t a = opaque ? 255 : (int64_t)((v >> 24) & 0xFF);
        int64_t aw = a * w;
        acc[0] += (int64_t)((v >> 16) & 0xFF) * aw;
        acc[1] += (int64_t)((v >> 8) & 0xFF) * aw;
        acc[2] += (int64_t)(v & 0xFF) * aw;
        asum += a * w;
        awsum += aw;
        wsum += w;
    }
    if (wsum == 0) wsum = 1;
    uint32_t alpha = (uint32_t)(asum / wsum) & 0xFF;
    uint32_t r = 0, g = 0, b = 0;
    if (awsum > 0) {
        r = (uint32_t)(acc[0] / awsum) & 0xFF;
        g = (uint32_t)(acc[1] / awsum) & 0xFF;
        b = (uint32_t)(acc[2] / awsum) & 0xFF;
    }
    return (alpha << 24) | (r << 16) | (g << 8) | b;
}

// THE LOOP ORDER FOLLOWS MEMORY, NOT THE AXIS. The horizontal pass reads
// along a line, so it walks a line at a time; the vertical one reads down
// a column, so it walks an output ROW at a time across every column. The
// other order strides a whole row per pixel.
//
// Inlined per MODE with the mode a constant, so the pixel loop carries
// no branch on it.
static inline __attribute__((always_inline))
void run_taps(int mode, const struct tap *t, const uint32_t *src, uint32_t *dst,
              int dst_n, int lines, int src_step, int src_line, int dst_step, int dst_line,
              int opaque) {
    if (src_step == 1) {
        for (int l = 0; l < lines; l++) {
            const uint32_t *sp = src + (size_t)l * src_line;
            uint32_t *dp = dst + (size_t)l * dst_line;
            for (int i = 0; i < dst_n; i++)
                dp[(size_t)i * dst_step] = tap_pixel(mode, &t[i], sp, 1, opaque);
        }
    } else {
        for (int i = 0; i < dst_n; i++) {
            uint32_t *dp = dst + (size_t)i * dst_step;
            for (int l = 0; l < lines; l++)
                dp[(size_t)l * dst_line] =
                    tap_pixel(mode, &t[i], src + (size_t)l * src_line, src_step, opaque);
        }
    }
}

static int resample_axis(const uint32_t *src, uint32_t *dst,
                         int src_n, int dst_n, int lines,
                         int src_step, int src_line, int dst_step, int dst_line,
                         int opaque) {
    struct tap *t = malloc((size_t)dst_n * sizeof *t);
    if (!t) return -ENOMEM;
    switch (make_taps(t, src_n, dst_n, opaque)) {
    case TAP_LERP:
        run_taps(TAP_LERP, t, src, dst, dst_n, lines, src_step, src_line, dst_step, dst_line, opaque);
        break;
    case TAP_BOX_OPAQUE:
        run_taps(TAP_BOX_OPAQUE, t, src, dst, dst_n, lines, src_step, src_line, dst_step, dst_line, 1);
        break;
    default:
        run_taps(TAP_BOX, t, src, dst, dst_n, lines, src_step, src_line, dst_step, dst_line, opaque);
        break;
    }
    free(t);
    return 0;
}

int uimg_rotate(const struct uimg *src, int quarters, struct uimg *out) {
    memset(out, 0, sizeof *out);
    if (!src->px || src->w <= 0 || src->h <= 0) {
        uimg_set_error("nothing to rotate");
        return -EINVAL;
    }
    int q = ((quarters % 4) + 4) % 4;
    int w = src->w, h = src->h;
    int ow = (q & 1) ? h : w, oh = (q & 1) ? w : h;
    uint32_t *px = malloc((size_t)ow * oh * sizeof *px);
    if (!px) {
        uimg_set_error("not enough memory to rotate the image");
        return -ENOMEM;
    }
    // Each source pixel's destination; the loops walk the SOURCE in
    // order, which is the cache-friendly side for the larger read.
    for (int y = 0; y < h; y++) {
        const uint32_t *row = src->px + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            int dx, dy;
            switch (q) {
            case 1:  dx = h - 1 - y; dy = x;         break;
            case 2:  dx = w - 1 - x; dy = h - 1 - y; break;
            case 3:  dx = y;         dy = w - 1 - x; break;
            default: dx = x;         dy = y;         break;
            }
            px[(size_t)dy * ow + dx] = row[x];
        }
    }
    out->w = ow;
    out->h = oh;
    out->px = px;
    out->has_alpha = src->has_alpha;
    return 0;
}

int uimg_scale(const struct uimg *src, int dw, int dh, struct uimg *out) {
    memset(out, 0, sizeof *out);
    if (!src->px || src->w <= 0 || src->h <= 0 || dw <= 0 || dh <= 0) {
        uimg_set_error("nothing to scale");
        return -EINVAL;
    }
    if ((int64_t)dw * dh > 64 * 1024 * 1024) {
        uimg_set_error("that target size is larger than this build will allocate");
        return -EINVAL;
    }

    if (dw == src->w && dh == src->h) {
        // A copy rather than a shared pointer: the caller frees `out`
        // independently of `src`, and aliasing the two would be a
        // double free waiting for whichever is released second.
        uint32_t *px = malloc((size_t)dw * dh * sizeof *px);
        if (!px) { uimg_set_error("not enough memory to copy the image"); return -ENOMEM; }
        memcpy(px, src->px, (size_t)dw * dh * sizeof *px);
        out->w = dw; out->h = dh; out->px = px;
        out->has_alpha = src->has_alpha;
        return 0;
    }

    // A BIG SHRINK GOES IN TWO STEPS: whole kx x ky blocks averaged first
    // (lib/uimg_reduce.h -- integer sums, a reciprocal), then the weighted
    // resampler below on a picture two to four times the target. The
    // resampler's per-pixel divides were the cost: shrinking a 1080p
    // picture to a thumbnail took longer than decoding it (imginfo -s).
    int kx, ky;
    uimg_reduce_factors(src->w, src->h, dw, dh, &kx, &ky);
    if (kx > 1 || ky > 1) {
        struct uimg_reduce r;
        struct uimg small;
        if (uimg_reduce_init(&r, src->w, src->h, kx, ky, src->has_alpha) == 0) {
            for (int y = 0; y < src->h; y++) uimg_reduce_row(&r, src->px + (size_t)y * src->w);
            if (uimg_reduce_finish(&r, &small) == 0) {
                int rc = uimg_scale(&small, dw, dh, out);
                uimg_free(&small);
                return rc;
            }
        }
        // No memory for the small copy: the one-step path below still works.
    }

    uint32_t *mid = malloc((size_t)dw * src->h * sizeof *mid);
    uint32_t *dst = malloc((size_t)dw * dh * sizeof *dst);
    if (!mid || !dst) {
        free(mid);
        free(dst);
        uimg_set_error("not enough memory to scale the image");
        return -ENOMEM;
    }

    // Horizontal: each source row becomes a dw-wide row.
    int rc = resample_axis(src->px, mid, src->w, dw, src->h, 1, src->w, 1, dw, !src->has_alpha);
    // Vertical: each of the dw columns is resampled down its own stride,
    // which is the transposed view the same routine handles by walking
    // with a stride instead of by one.
    if (rc == 0) rc = resample_axis(mid, dst, src->h, dh, dw, dw, 1, dw, 1, !src->has_alpha);
    free(mid);
    if (rc < 0) {
        free(dst);
        uimg_set_error("not enough memory to scale the image");
        return rc;
    }

    out->w = dw;
    out->h = dh;
    out->px = dst;
    out->has_alpha = src->has_alpha;
    return 0;
}

// --- uimg_format_guess --------------------------------------------------

static const struct { const char *ext, *format; } EXTS[] = {
    { ".png", "png" }, { ".jpg", "jpeg" }, { ".jpeg", "jpeg" }, { ".jpe", "jpeg" }, { ".jfif", "jpeg" },
    { ".gif", "gif" }, { ".bmp", "bmp" }, { ".dib", "bmp" }, { ".qoi", "qoi" },
};

const char *uimg_format_guess(const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    if (dot && dot != base)
        for (unsigned i = 0; i < sizeof EXTS / sizeof EXTS[0]; i++)
            if (!strcasecmp(dot, EXTS[i].ext)) return EXTS[i].format;
    uint8_t head[16];
    size_t got = ufile_read_head(path, head, sizeof head);
    const struct uimg_codec *c = got >= 4 ? uimg_probe(head, got) : 0;
    return c ? c->name : 0;
}
