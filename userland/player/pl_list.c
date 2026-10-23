// The Audio Player's playlist: one folder's playable files, with what
// their tags say, and the order they play in.
//
// THE PLAY ORDER IS SEPARATE FROM THE LIST: the list stays sorted by
// name -- that is what a person scans -- and shuffle permutes an index
// array (Fisher-Yates) with the track playing now at its head, so
// turning shuffle on never jumps away from what is playing.
#include "player/player_internal.h"
#include "lib/usnd.h"
#include "lib/ulistdir.h"
#include "lib/utags.h"
#include "lib/ufile.h"
#include "lib/dirsort.h"
#include "kpath.h"
#include "ui/ulog.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

char g_dir[PL_PATH_MAX] = "/usr/share/music";
struct pl_track *g_tracks;
int g_track_count;
int g_shuffle, g_repeat;

static int *g_order;   // play position -> track index, g_track_count of them

static int playable(const char *dir, const struct sys_dirent *e) {
    if (e->is_dir) return 0;
    char path[PL_PATH_MAX];
    if (!k_path_join(dir, e->name, path, sizeof path)) return 0;
    uint8_t head[16];
    size_t got = ufile_read_head(path, head, sizeof head);
    return got >= 12 && usnd_probe(head, got);
}

static void describe_track(const char *dir, struct pl_track *t) {
    char path[PL_PATH_MAX];
    k_path_join(dir, t->name, path, sizeof path);
    struct utags tags;
    utags_read(path, &tags, 0);
    // The NAME stands in for a missing title, extension dropped:
    // "first-boot", never "first-boot.mp3".
    if (tags.title[0]) strlcpy(t->title, tags.title, sizeof t->title);
    else {
        strlcpy(t->title, t->name, sizeof t->title);
        char *dot = strrchr(t->title, '.');
        if (dot && dot != t->title) *dot = '\0';
    }
    strlcpy(t->artist, tags.artist, sizeof t->artist);
    strlcpy(t->album, tags.album, sizeof t->album);
    t->ms = tags.length_ms;
    struct usnd_info in;
    if (usnd_load_info(path, &in) == 0) {
        if (!t->ms) t->ms = in.ms;
        snprintf(t->format, sizeof t->format, "%s", in.format);
        for (char *c = t->format; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
        if (!strcmp(t->format, "MID")) strlcpy(t->format, "MIDI", sizeof t->format);
    }
}

int pl_scan(const char *dir) {
    struct sys_dirent *all = 0;
    if (dir != g_dir) strlcpy(g_dir, dir, sizeof g_dir);
    int n = ulistdir(g_dir, &all), count = 0;
    for (int i = 0; i < n; i++)
        if (playable(g_dir, &all[i])) all[count++] = all[i];
    dirsort(all, count, DIRSORT_NAME, 0);
    // Built aside and swapped in, so running out keeps the old list.
    struct pl_track *tracks = calloc((size_t)(count ? count : 1), sizeof *tracks);
    int *order = malloc((size_t)(count ? count : 1) * sizeof *order);
    if (!tracks || !order) {
        free(tracks); free(order); free(all);
        ulogf("player: out of memory listing %s (%d tracks)\n", g_dir, count);
        return g_track_count;
    }
    for (int i = 0; i < count; i++) {
        strlcpy(tracks[i].name, all[i].name, sizeof tracks[i].name);
        describe_track(g_dir, &tracks[i]);
    }
    free(all);
    free(g_tracks); free(g_order);
    g_tracks = tracks;
    g_order = order;
    g_track_count = count;
    for (int i = 0; i < count; i++) g_order[i] = i;
    if (g_shuffle) pl_reshuffle(-1);
    ulogf("player: listing %s -- %d track(s)\n", g_dir, count);
    return count;
}

int pl_index_of(const char *name) {
    for (int i = 0; i < g_track_count; i++)
        if (!strcmp(g_tracks[i].name, name)) return i;
    return -1;
}

void pl_path(int i, char *out, size_t cap) {
    if (i < 0 || i >= g_track_count || !k_path_join(g_dir, g_tracks[i].name, out, cap))
        out[0] = '\0';
}

void pl_reshuffle(int first) {
    for (int i = 0; i < g_track_count; i++) g_order[i] = i;
    for (int i = g_track_count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int t = g_order[i]; g_order[i] = g_order[j]; g_order[j] = t;
    }
    for (int i = 0; first >= 0 && i < g_track_count; i++)
        if (g_order[i] == first) { g_order[i] = g_order[0]; g_order[0] = first; break; }
}

int pl_step(int cur, int dir) {
    if (!g_track_count) return -1;
    if (cur < 0) return g_shuffle ? g_order[0] : 0;
    int pos = cur;
    if (g_shuffle)
        for (int i = 0; i < g_track_count; i++) if (g_order[i] == cur) { pos = i; break; }
    pos += dir;
    if (pos < 0 || pos >= g_track_count) {
        if (!g_repeat) return -1;
        pos = (pos + g_track_count) % g_track_count;
    }
    return g_shuffle ? g_order[pos] : pos;
}

void pl_fmt_ms(char *out, size_t cap, uint32_t ms) {
    if (!ms) { if (cap) out[0] = '\0'; return; }
    unsigned s = (ms + 500) / 1000;
    snprintf(out, cap, "%u:%02u", s / 60, s % 60);
}
