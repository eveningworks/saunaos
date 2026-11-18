// A window with a title -- and, asked, two tab labels -- of a given
// length, for title_text_test: `titleclient N [tabs]`.
//
// The text is "\xc3\x84bc " (Äbc, then a space) repeated and cut to N bytes on a
// character boundary, so a two-byte character straddles the 31-byte
// pieces a long title travels in (abi/win_proto.h's WIN_TEXT_PIECE), and
// the test can rebuild the exact string. The tab labels are the same
// text, prefixed "A:" and "B:".
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/uapp.h"
#include "ui/ugfx.h"

static char g_title[4096];
static char g_tab[2][4096];
static int g_tabs;

static void make(char *out, size_t cap, const char *prefix, int n) {
    static const char unit[] = "\xc3\x84" "bc ";   // 5 bytes, 4 characters
    size_t at = strlen(prefix);
    memcpy(out, prefix, at);
    while ((int)at < n && at + 1 < cap) {
        size_t k = (at - strlen(prefix)) % (sizeof unit - 1);
        // Never half of the two-byte character.
        if (k == 0 && (int)at + 2 > n) break;
        out[at++] = unit[k];
        if (k == 0) out[at++] = unit[1];
    }
    out[at] = '\0';
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    ugfx_fill(uapp_surface(d), 0xFFFFFF);
}

static void on_open(struct uapp *a) {
    if (g_tabs) {
        const char *labels[2] = { g_tab[0], g_tab[1] };
        uapp_set_tabs(a, labels, 2, 0);
    }
    printf("titleclient: ready, %d-byte title\n", (int)strlen(g_title));
    fflush(stdout);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    if (key == 'q') uapp_quit(a, 0);
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 40;
    g_tabs = argc > 2 && !strcmp(argv[2], "tabs");
    make(g_title, sizeof g_title, "", n);
    make(g_tab[0], sizeof g_tab[0], "A:", n);
    make(g_tab[1], sizeof g_tab[1], "B:", n);
    struct uapp_desc desc = {
        .title   = g_title,
        .w       = 480,
        .h       = 200,
        .x       = 200,
        .y       = 160,
        .on_open = on_open,
        .on_draw = on_draw,
        .on_key  = on_key,
    };
    return uapp_run(&desc);
}
