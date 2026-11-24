// base/uwallpaper.h: which of a picture's sizes a screen gets.
#include "base/uwallpaper.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// How far w/h is from the screen's shape, in thousandths of it.
static int64_t aspect_off(int w, int h, int sw, int sh) {
    int64_t d = (int64_t)w * sh - (int64_t)h * sw;
    if (d < 0) d = -d;
    return d * 1000 / ((int64_t)h * sw);
}

int uwallpaper_pick(const struct uwallpaper_size *sizes, int n, int screen_w, int screen_h) {
    if (n <= 0) return -1;
    if (screen_w <= 0 || screen_h <= 0) screen_w = screen_h = 1;
    int64_t best = -1;
    for (int i = 0; i < n; i++) {
        int64_t e = aspect_off(sizes[i].w, sizes[i].h, screen_w, screen_h);
        if (best < 0 || e < best) best = e;
    }
    int cover = -1, largest = -1;
    for (int i = 0; i < n; i++) {
        if (aspect_off(sizes[i].w, sizes[i].h, screen_w, screen_h) > best + 10) continue;
        int64_t area = (int64_t)sizes[i].w * sizes[i].h;
        if (sizes[i].w >= screen_w && sizes[i].h >= screen_h &&
            (cover < 0 || area < (int64_t)sizes[cover].w * sizes[cover].h))
            cover = i;
        if (largest < 0 || area > (int64_t)sizes[largest].w * sizes[largest].h)
            largest = i;
    }
    return cover >= 0 ? cover : largest;
}

// "<W>x<H>.<ext>" with both numbers positive and the extension one a
// decoder reads; anything else is not a size. A PARSER REJECTS rather
// than guesses, so "1920x1080 (copy).jpg" is not 1920x1080.
static int parse_size(const char *name, struct uwallpaper_size *out) {
    static const char *const EXT[] = { "jpg", "png", "qoi", "gif" };
    long v[2];
    const char *p = name;
    for (int k = 0; k < 2; k++) {
        if (*p < '0' || *p > '9') return 0;
        v[k] = 0;
        while (*p >= '0' && *p <= '9') {
            v[k] = v[k] * 10 + (*p++ - '0');
            if (v[k] > 65535) return 0;
        }
        if (*p++ != (k == 0 ? 'x' : '.')) return 0;
        if (v[k] == 0) return 0;
    }
    for (unsigned e = 0; e < sizeof EXT / sizeof EXT[0]; e++) {
        if (!strcmp(p, EXT[e])) {
            out->w = (int)v[0];
            out->h = (int)v[1];
            return 1;
        }
    }
    return 0;
}

int uwallpaper_path(const char *dir, const char *stem, int screen_w, int screen_h,
                    char *out, size_t cap) {
    char folder[512];
    if (!stem[0] || strchr(stem, '/')) return 0;
    if ((size_t)snprintf(folder, sizeof folder, "%s/%s", dir, stem) >= sizeof folder) return 0;
    DIR *d = opendir(folder);
    if (!d) return 0;
    // Grown as found: a folder holds as many sizes as someone put there.
    struct uwallpaper_size *sizes = 0;
    char **names = 0;
    int n = 0, room = 0, ok = 1;
    struct dirent *e;
    while (ok && (e = readdir(d))) {
        struct uwallpaper_size sz;
        if (e->d_type == DT_DIR || !parse_size(e->d_name, &sz)) continue;
        if (n == room) {
            int grow = room ? room * 2 : 8;
            struct uwallpaper_size *ns = realloc(sizes, (size_t)grow * sizeof *ns);
            if (ns) sizes = ns;
            char **nn = ns ? realloc(names, (size_t)grow * sizeof *nn) : 0;
            if (nn) names = nn;
            if (!ns || !nn) { ok = 0; break; }
            room = grow;
        }
        names[n] = strdup(e->d_name);
        if (!names[n]) { ok = 0; break; }
        sizes[n++] = sz;
    }
    closedir(d);
    int pick = ok ? uwallpaper_pick(sizes, n, screen_w, screen_h) : -1;
    int rc = pick >= 0 && (size_t)snprintf(out, cap, "%s/%s", folder, names[pick]) < cap;
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
    free(sizes);
    return rc;
}
