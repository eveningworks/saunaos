// membw.c -- memory bandwidth, the ceiling under the compositor's copies.
// A dragged frame is mostly copies now (a client's pixels into the back
// buffer, the back buffer to the display), and whether they have
// anything left to give depends on what this machine can move at all.
// Each loop runs on a SCREEN-SIZED buffer (1920x1080x4, past every cache
// here) and on a cache-sized one, and reports GB/s of bytes moved; `gui
// compositor` gives the stages' bytes and times to compare with.
//
//   /tests/membw            all of them
//   /tests/membw 3          three rounds each (default 5), the best kept
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt/sys.h"
#include "ui/ugfx_pix.h"

#define SCREEN_BYTES (1920u * 1080u * 4u)
#define CACHE_BYTES  (256u * 1024u)

static uint32_t *g_a, *g_b;
static volatile uint64_t g_sink;

static void op_memcpy(size_t n)  { memcpy(g_b, g_a, n); }
static void op_memset(size_t n)  { memset(g_b, 0x5A, n); }
static void op_read(size_t n) {
    const uint64_t *p = (const uint64_t *)g_a;
    uint64_t s = 0;
    for (size_t i = 0; i < n / 8; i += 4) s += p[i] ^ p[i + 1] ^ p[i + 2] ^ p[i + 3];
    g_sink = s;
}
// The present's own loop: streaming stores, no read of the destination.
static void op_stream(size_t n) { ugfx_pix()->to_screen(g_b, g_a, (int)(n / 4)); }

struct op {
    const char *name;
    void (*fn)(size_t n);
};

static const struct op OPS[] = {
    { "memcpy (rep movsb)", op_memcpy },
    { "streamed copy (to_screen)", op_stream },
    { "memset (rep stosb)", op_memset },
    { "read", op_read },
};

// The best of `rounds`, each long enough to swamp the clock's grain.
static double gbps(const struct op *o, size_t n, int rounds) {
    unsigned reps = n >= SCREEN_BYTES ? 8 : 2000;
    double best = 0;
    for (int r = 0; r < rounds; r++) {
        o->fn(n);                                 // warm, and page the buffers in
        unsigned long long t0 = sys_monotonic_ns();
        for (unsigned i = 0; i < reps; i++) o->fn(n);
        unsigned long long dt = sys_monotonic_ns() - t0;
        double v = dt ? (double)n * reps / (double)dt : 0;   // bytes per ns = GB/s
        if (v > best) best = v;
    }
    return best;
}

int main(int argc, char **argv) {
    int rounds = argc > 1 ? atoi(argv[1]) : 5;
    if (rounds < 1) rounds = 1;
    g_a = malloc(SCREEN_BYTES);
    g_b = malloc(SCREEN_BYTES);
    if (!g_a || !g_b) { fprintf(stderr, "membw: out of memory\n"); return 1; }
    for (size_t i = 0; i < SCREEN_BYTES / 4; i++) g_a[i] = (uint32_t)(i * 2654435761u);
    memset(g_b, 0, SCREEN_BYTES);
    printf("membw: GB/s of the buffer's bytes (a copy moves twice that over the bus); pixel loops %s\n",
           ugfx_pix()->name);
    printf("%-26s %12s %12s\n", "", "screen 8 MB", "cache 256 KB");
    for (unsigned k = 0; k < sizeof OPS / sizeof OPS[0]; k++)
        printf("%-26s %12.2f %12.2f\n", OPS[k].name, gbps(&OPS[k], SCREEN_BYTES, rounds),
               gbps(&OPS[k], CACHE_BYTES, rounds));
    return 0;
}
