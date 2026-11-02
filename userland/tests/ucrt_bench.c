// A BENCHMARK, not a test: what the CRT effect (ui/ucrt.h) costs a frame
// at a given size, per preset and per pass. Not in the gate, because a
// timing number varies with the machine -- read it on the hardware the
// question is about.
//
// The picture is DOOM-shaped: 320x200 nearest-scaled to the rect, with
// a scanline per source row, which is the fullscreen case the roadmap
// measured. It is composed into a plain malloc'd surface, so the number
// is ucrt's own and not the cost of writing a scanout buffer.
//
//     /tests/ucrt_bench [W H [FRAMES]]     (default 1440 1080 30)
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "rt/sys.h"
#include "ui/ucrt.h"

#define say(...) fprintf(stderr, __VA_ARGS__)

static unsigned long ms10(unsigned long long ns, int frames) {
    return (unsigned long)(ns / 100000ULL / (unsigned)frames);   // tenths of a ms
}

int main(int argc, char **argv) {
    int w = argc > 2 ? atoi(argv[1]) : 1440, h = argc > 2 ? atoi(argv[2]) : 1080;
    int frames = argc > 3 ? atoi(argv[3]) : 30;
    if (w < 8 || h < 8 || w > 4095 || h > 4095 || frames < 1) {
        say("usage: ucrt_bench [W H [FRAMES]]\n");
        return 2;
    }
    uint32_t *src = malloc((size_t)w * h * 4), *dst = calloc((size_t)w * h, 4);
    if (!src || !dst) {
        say("ucrt_bench: no memory for %dx%d\n", w, h);
        return 1;
    }
    uint32_t seed = 12345;
    static uint32_t small[320 * 200];   // static: 256 KiB is past what the stack grows by
    for (int i = 0; i < 320 * 200; i++) {
        seed = seed * 1103515245u + 12345u;
        small[i] = (seed >> 8) & 0xFFFFFF;
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) src[(long)y * w + x] = small[(y * 200 / h) * 320 + x * 320 / w];
    struct ugfx_surface s = { .pixels = dst, .w = w, .h = h };

    unsigned long long c0 = sys_monotonic_ns();
    for (int i = 0; i < frames; i++) memcpy(dst, src, (size_t)w * h * 4);
    unsigned long long cns = sys_monotonic_ns() - c0;
    say("ucrt_bench: %dx%d, %d frames; a plain copy %lu.%lu ms\n", w, h, frames,
        ms10(cns, frames) / 10, ms10(cns, frames) % 10);

    for (int p = 0; p < UCRT_PRESET_COUNT; p++) {
        struct ucrt c;
        ucrt_init(&c);
        c.look = ucrt_presets[p];
        c.src_lines = 200;
        c.period = h / 200 < 2 ? 2 : h / 200;
        ucrt_apply_from(&c, src, w, &s, 0, 0, w, h);   // builds the buffers and the map
        memset(c.stage_ns, 0, sizeof c.stage_ns);
        unsigned long long t0 = sys_monotonic_ns();
        for (int i = 0; i < frames; i++) ucrt_apply_from(&c, src, w, &s, 0, 0, w, h);
        unsigned long long ns = sys_monotonic_ns() - t0;
        unsigned long t = ms10(ns, frames), g = ms10(c.stage_ns[UCRT_STAGE_GLOW], frames),
                      k = ms10(c.stage_ns[UCRT_STAGE_COMPOSE], frames),
                      wp = ms10(c.stage_ns[UCRT_STAGE_WARP], frames);
        say("ucrt_bench: %-12s %lu.%lu ms a frame = glow %lu.%lu, compose %lu.%lu, warp %lu.%lu\n",
            ucrt_preset_names[p], t / 10, t % 10, g / 10, g % 10, k / 10, k % 10, wp / 10, wp % 10);
        ucrt_free(&c);
    }
    free(src);
    free(dst);
    return 0;
}
