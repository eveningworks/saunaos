// w16.c -- run a 16-bit Windows program (docs/w16-design.md). Its parts
// live in userland/w16/; this is main(). Stage 0: --info reads an NE
// file and says what it holds and imports; running one comes later.
//
// THE FILE IS HOSTILE INPUT: the process gives itself up to reading that
// one file before it reads a byte, and refuses to run if it cannot.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "lib/uargs.h"
#include "w16/w16_internal.h"

#define W16_FILE_MAX (16u << 20)   // NE files are at most a few MiB

static int g_info;

static const struct uargs_opt OPTS[] = {
    { "info", 'i', NULL, "print the program's segments and the functions it imports", &g_info, NULL },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "w16",
    .usage = "--info FILE",
    .summary = "Run a 16-bit Windows (Windows 3.x) program. Only --info works so far:\n"
               "it reads an NE executable and lists its segments and its imports.",
    .opts = OPTS,
    .notes = "Exit status: 0 done, 1 the file was refused, 2 a usage error.",
};

static uint8_t *slurp(const char *path, uint32_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *buf = NULL;
    size_t cap = 0, n = 0;
    for (;;) {
        if (n == cap) {
            if (cap >= W16_FILE_MAX) { free(buf); fclose(f); *size = UINT32_MAX; return NULL; }
            cap = cap ? cap * 2 : 64 * 1024;
            uint8_t *nb = realloc(buf, cap);
            if (!nb) { free(buf); fclose(f); return NULL; }
            buf = nb;
        }
        size_t got = fread(buf + n, 1, cap - n, f);
        if (got == 0) break;
        n += got;
    }
    fclose(f);
    *size = (uint32_t)n;
    return buf;
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc != 1) return uargs_error(&PROG, "expected one FILE");
    if (!g_info)
        return uargs_error(&PROG, "running a program is not built yet; --info reads one");
    const char *path = a.argv[0];

    if (unveil(path, "r") != 0 || unveil(NULL, NULL) != 0 || pledge("stdio rpath", NULL) != 0) {
        fprintf(stderr, "w16: cannot sandbox myself, so I will not read %s\n", path);
        return 1;
    }
    uint32_t size = 0;
    uint8_t *img = slurp(path, &size);
    if (!img) {
        fprintf(stderr, "w16: %s: %s\n", path,
                size == UINT32_MAX ? "larger than any NE program" : strerror(errno));
        return 1;
    }
    static struct ne_module m;
    int err = ne_parse(img, size, &m);
    if (err < 0) {
        fprintf(stderr, "w16: %s: %s\n", path, ne_strerror(err));
        free(img);
        return 1;
    }
    ne_print_info(&m);
    free(img);
    return 0;
}
