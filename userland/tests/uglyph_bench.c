// A BENCHMARK, not a test: what drawing a page of text through
// ui/uglyph.h costs, the way the Web Browser draws one -- every glyph
// through uglyph_text(), in a few faces and sizes at once (body, bold,
// italic, headings). Not in the gate: a timing varies with the machine.
//
// The first pass rasterizes; the later ones are what a repaint costs, so
// a cache that cannot hold the page's glyphs shows as a steady state
// close to the first pass.
//
//     /tests/uglyph_bench [PASSES]          (default 10)
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "rt/sys.h"
#include "ui/uglyph.h"

#define say(...) fprintf(stderr, __VA_ARGS__)

static const char *const FACES[] = {
    "/usr/share/fonts/liberation-sans.ttf",
    "/usr/share/fonts/liberation-sans-bold.ttf",
    "/usr/share/fonts/liberation-serif-italic.ttf",
};
static const int SIZES[] = { 13, 16, 24 };

static const char TEXT[] =
    "The quick brown fox jumps over the lazy dog. THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG! "
    "0123456789 (a+b)*c = [x/y]; {p|q} <tag> \"quoted\" 'single' #hash $5 @home %50 ^caret &amp ~tilde "
    "Sauna, l\xc3\xb6yly, k\xc3\xa4si, \xc3\xa5, \xc3\xa9t\xc3\xa9, na\xc3\xafve, stra\xc3\x9f" "e, "
    "Hello world -- a web page has headings, links, bold and italic words, and long paragraphs of text.";

int main(int argc, char **argv) {
    int passes = argc > 1 ? atoi(argv[1]) : 10;
    if (passes < 2) passes = 2;
    enum { NF = sizeof FACES / sizeof FACES[0], NS = sizeof SIZES / sizeof SIZES[0] };
    static struct uglyph_face faces[NF];
    for (int f = 0; f < NF; f++)
        if (!uglyph_open(&faces[f], FACES[f])) {
            say("uglyph_bench: cannot open %s\n", FACES[f]);
            return 1;
        }
    int w = 1600, h = 900;
    uint32_t *px = calloc((size_t)w * h, 4);
    if (!px) return 1;
    struct ugfx_surface s = { .pixels = px, .w = w, .h = h };

    unsigned long long first = 0, rest = 0;
    int chars = 0;
    for (int p = 0; p < passes; p++) {
        unsigned long long t0 = sys_monotonic_ns();
        int y = 30;
        chars = 0;
        for (int f = 0; f < NF; f++)
            for (int k = 0; k < NS; k++) {
                uglyph_text(&s, &faces[f], TEXT, SIZES[k], 4, y, 0xFF000000u);
                chars += (int)strlen(TEXT);
                y += SIZES[k] + 6;
                if (y > h - 30) y = 30;
            }
        unsigned long long dt = sys_monotonic_ns() - t0;
        if (p == 0) first = dt; else rest += dt;
    }
    unsigned long f10 = (unsigned long)(first / 100000), r10 = (unsigned long)(rest / 100000 / (unsigned)(passes - 1));
    say("uglyph_bench: %d faces x %d sizes, %d bytes a pass: first pass %lu.%lu ms, then %lu.%lu ms a pass\n",
        NF, NS, chars, f10 / 10, f10 % 10, r10 / 10, r10 % 10);
    for (int f = 0; f < NF; f++) uglyph_close(&faces[f]);
    free(px);
    return 0;
}
