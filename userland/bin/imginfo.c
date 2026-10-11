// imginfo -- what an image file is, and what one pixel of it decodes to.
//
// The header half is what `file` or `identify` gives you: format,
// dimensions, and how the picture is actually stored, which for a JPEG
// is the part that decides whether this build can show it at all.
//
// The pixel half exists for TESTING, and it is the reason this command
// is worth having rather than being a menu item in the viewer. A decoder
// is verified by comparing numbers against another implementation's, and
// a screenshot cannot do that: `imginfo -p 4,4 photo.jpg` prints one
// pixel's value as text, so tools/vm.py can assert on it from the host
// with no framebuffer, no compositor and no pixels involved.
#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/uargs.h"
#include "lib/ufile.h"
#include "lib/uimg.h"
#include <stdio.h>
#include <stdlib.h>
#include <kerrno.h>

static const char *why(int rc) {
    // -ENOTSUP is not a broken file and must not read like one; see
    // uimg.h on why the two codes are kept apart.
    if (rc == -ENOTSUP) return "unsupported";
    if (rc == -ENOMEM)  return "out of memory";
    if (rc == -ENOENT)  return "not found";
    return "invalid";
}

static void report(const char *path, int rc) {
    char buf[160];
    snprintf(buf, sizeof buf, "imginfo: %s: %s: %s\n", path, why(rc),
             uimg_last_error());
    sys_print(buf);
}

// "4,4" -> 4, 4. Returns 0 on anything that is not two numbers.
static int parse_xy(const char *s, int *x, int *y) {
    char *end;
    long a = strtol(s, &end, 10);
    if (end == s || *end != ',') return 0;
    const char *s2 = end + 1;
    long b = strtol(s2, &end, 10);
    if (end == s2 || *end != '\0') return 0;
    if (a < 0 || b < 0) return 0;
    *x = (int)a;
    *y = (int)b;
    return 1;
}

static int g_decode;
static const char *g_pixel, *g_scale;

static const struct uargs_opt OPTS[] = {
    { "decode", 'd', 0,     "decode it, and time the read and the decode apart", &g_decode, 0 },
    { "pixel",  'p', "X,Y", "decode it and print one pixel as RRGGBB", 0, &g_pixel },
    { "scale",  's', "PX",  "decode it, then shrink it to fit PX as a thumbnail is, and time that too",
      0, &g_scale },
    { 0 }
};

static const struct uargs_prog PROG = {
    .name = "imginfo",
    .usage = "[-d] [-p X,Y] [-s PX] PATH",
    .summary = "What an image file is -- and, decoded, what reading, decoding and\n"
               "shrinking it cost and what one pixel holds.",
    .opts = OPTS,
};

static unsigned long long ms_since(unsigned long long t0) {
    return (sys_monotonic_ns() - t0) / 1000000ull;
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc != 1) return uargs_error(&PROG, "one PATH, please");
    const char *path = a.argv[0];
    int px_box = 0;
    if (g_scale && (px_box = atoi(g_scale)) <= 0) return uargs_error(&PROG, "-s takes a size in pixels");

    struct uimg_info info;
    int rc = uimg_load_info(path, &info);
    if (rc < 0) {
        report(path, rc);
        return 1;
    }

    char buf[192];
    snprintf(buf, sizeof buf, "%s: %s %dx%d, %d component%s (%s)\n",
             path, info.format, info.w, info.h, info.components,
             info.components == 1 ? "" : "s", info.detail);
    sys_print(buf);

    if (!g_pixel && !g_decode && !g_scale) return 0;

    // READ AND DECODE TIMED APART: on a disk-backed photograph the read
    // can cost as much as the decode, and only the split says which one
    // a faster decoder would help.
    uint8_t *file;
    size_t len;
    unsigned long long t0 = sys_monotonic_ns();
    if (ufile_slurp(path, 0, &file, &len) != UFILE_OK) {
        report(path, -ENOENT);
        return 1;
    }
    unsigned long long read_ms = ms_since(t0);
    struct uimg im;
    t0 = sys_monotonic_ns();
    rc = uimg_decode(file, len, &im);
    unsigned long long dec_ms = ms_since(t0);
    free(file);
    if (rc < 0) {
        report(path, rc);
        return 1;
    }
    // The time is printed because it is the number that decides things:
    // every automated boot here is TCG, where a full-screen photograph
    // is the desktop's slowest startup step, and "how long does this
    // wallpaper cost" is otherwise a guess.
    snprintf(buf, sizeof buf, "read: %lu bytes, %llu ms\n", (unsigned long)len, read_ms);
    sys_print(buf);
    snprintf(buf, sizeof buf, "decoded: %dx%d, %lu bytes of pixels, %llu ms\n",
             im.w, im.h, (unsigned long)((size_t)im.w * im.h * 4), dec_ms);
    sys_print(buf);

    if (px_box) {
        int tw, th;
        struct uimg small;
        uimg_fit_size(im.w, im.h, px_box, px_box, UIMG_FIT_CONTAIN, &tw, &th);
        t0 = sys_monotonic_ns();
        rc = uimg_scale(&im, tw, th, &small);
        unsigned long long sc_ms = ms_since(t0);
        if (rc == 0) {
            snprintf(buf, sizeof buf, "scaled: %dx%d, %llu ms\n", tw, th, sc_ms);
            sys_print(buf);
            uimg_free(&small);
        } else {
            report(path, rc);
        }
        // The whole thumbnail path from the file, as the File Manager's
        // thumbnailer takes it: a codec that can shrinks while decoding.
        t0 = sys_monotonic_ns();
        rc = uimg_load_fit(path, px_box, px_box, &small);
        unsigned long long fit_ms = ms_since(t0);
        if (rc == 0) {
            snprintf(buf, sizeof buf, "thumbnail from the file: %dx%d, %llu ms\n", small.w, small.h, fit_ms);
            sys_print(buf);
            uimg_free(&small);
        }
    }

    if (g_pixel) {
        int x, y;
        if (!parse_xy(g_pixel, &x, &y)) {
            uimg_free(&im);
            return uargs_error(&PROG, "-p takes X,Y");
        }
        if (x >= im.w || y >= im.h) {
            sys_print("imginfo: that pixel is outside the image\n");
            uimg_free(&im);
            return 1;
        }
        uint32_t p = im.px[(size_t)y * im.w + x];
        snprintf(buf, sizeof buf, "pixel %d,%d: %02x%02x%02x\n", x, y,
                 (unsigned)((p >> 16) & 0xFF), (unsigned)((p >> 8) & 0xFF),
                 (unsigned)(p & 0xFF));
        sys_print(buf);
    }

    uimg_free(&im);
    return 0;
}
