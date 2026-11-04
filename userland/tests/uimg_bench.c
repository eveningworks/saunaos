// A BENCHMARK, not a test: what decoding a picture costs, split from
// what the memory it lands in costs. Not in the gate: a timing varies
// with the machine.
//
//   touch    the first write to a fresh buffer of the decoded image's
//            size: the page faults every decode pays before a pixel
//   decode   uimg_decode() of the file, repeated; the first one pays
//            the faults, later ones may reuse freed memory
//   scale    uimg_scale() of the decoded image to fit a 256 px box,
//            as a thumbnail is made
//
//     /tests/uimg_bench FILE [REPS]          (default 5)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt/sys.h"
#include "media/uimg.h"
#include "base/ufile.h"

#define say(...) fprintf(stderr, __VA_ARGS__)

static unsigned long us(unsigned long long ns) { return (unsigned long)(ns / 1000); }

int main(int argc, char **argv) {
    if (argc < 2) {
        say("usage: uimg_bench FILE [REPS]\n");
        return 2;
    }
    int reps = argc > 2 ? atoi(argv[2]) : 5;
    if (reps < 1) reps = 1;
    uint8_t *data = 0;
    size_t len = 0;
    if (ufile_slurp(argv[1], 64u << 20, &data, &len) != UFILE_OK) {
        say("uimg_bench: cannot read %s\n", argv[1]);
        return 1;
    }
    struct uimg im;
    memset(&im, 0, sizeof im);
    unsigned long long t0 = sys_monotonic_ns();
    if (uimg_decode(data, len, &im) != 0) {
        say("uimg_bench: %s will not decode\n", argv[1]);
        return 1;
    }
    unsigned long long first = sys_monotonic_ns() - t0;
    size_t bytes = (size_t)im.w * im.h * 4;
    say("uimg_bench: %s, %dx%d: first decode %lu us\n", argv[1], im.w, im.h, us(first));

    uint8_t *fresh = malloc(bytes);
    t0 = sys_monotonic_ns();
    if (fresh) memset(fresh, 1, bytes);
    say("uimg_bench: touch %lu KiB of fresh memory: %lu us\n", (unsigned long)(bytes >> 10),
        us(sys_monotonic_ns() - t0));
    t0 = sys_monotonic_ns();
    if (fresh) memset(fresh, 2, bytes);
    say("uimg_bench: the same memory again: %lu us\n", us(sys_monotonic_ns() - t0));
    free(fresh);

    unsigned long long sum = 0, best = ~0ull;
    for (int i = 0; i < reps; i++) {
        uimg_free(&im);
        t0 = sys_monotonic_ns();
        uimg_decode(data, len, &im);
        unsigned long long dt = sys_monotonic_ns() - t0;
        sum += dt;
        if (dt < best) best = dt;
    }
    say("uimg_bench: decode x%d: mean %lu us, best %lu us\n", reps, us(sum / (unsigned)reps), us(best));

    int sw = im.w >= im.h ? 256 : 256 * im.w / im.h, sh = im.w >= im.h ? 256 * im.h / im.w : 256;
    struct uimg small;
    memset(&small, 0, sizeof small);
    t0 = sys_monotonic_ns();
    int rc = uimg_scale(&im, sw, sh, &small);
    say("uimg_bench: scale to %dx%d: %lu us%s\n", sw, sh, us(sys_monotonic_ns() - t0), rc ? " (failed)" : "");
    uimg_free(&small);
    uimg_free(&im);
    free(data);
    return 0;
}
