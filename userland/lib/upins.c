// Pinned folders -- see upins.h.
#include "lib/upins.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "rt/sys.h"
#include "kpath.h"

struct pin { char path[UPINS_PATH]; char label[UPINS_LABEL]; };

// Every line of the file, existing folders or not: what add and remove
// rewrite must not lose a pin to a disk that is unplugged right now.
static int read_all(struct pin *out, int cap) {
    FILE *f = fopen(UPINS_FILE, "r");
    if (!f) return 0;
    int n = 0;
    char line[UPINS_PATH + UPINS_LABEL + 4];
    while (n < cap && fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        if (line[0] != '/') continue;           // blank, a comment, or not a path
        char *tab = strchr(line, '\t');
        if (tab) *tab++ = '\0';
        strlcpy(out[n].path, line, UPINS_PATH);
        strlcpy(out[n].label, tab ? tab : "", UPINS_LABEL);
        n++;
    }
    fclose(f);
    return n;
}

static int write_all(const struct pin *in, int n) {
    FILE *f = fopen(UPINS_FILE, "w");
    if (!f) return -errno;
    fputs("# Folders pinned to Places (the File Manager, the file dialog): path[TAB name]\n", f);
    for (int i = 0; i < n; i++) {
        if (in[i].label[0]) fprintf(f, "%s\t%s\n", in[i].path, in[i].label);
        else fprintf(f, "%s\n", in[i].path);
    }
    return fclose(f) == 0 ? 0 : -EIO;
}

static int index_of(const struct pin *all, int n, const char *path) {
    for (int i = 0; i < n; i++)
        if (!strcmp(all[i].path, path)) return i;
    return -1;
}

// The callers' buffers are static: a ring-3 frame has 2 KiB, and eight
// pins are 2.3 KiB.
static struct pin g_all[UPINS_MAX + 1];

int upins_load(char (*out)[UPINS_PATH], int cap) {
    int n = read_all(g_all, UPINS_MAX), k = 0;
    for (int i = 0; i < n && k < cap; i++) {
        struct sys_stat st;
        if (sys_stat(g_all[i].path, &st) == 0 && st.is_dir) strlcpy(out[k++], g_all[i].path, UPINS_PATH);
    }
    return k;
}

int upins_has(const char *path) {
    int n = read_all(g_all, UPINS_MAX);
    return index_of(g_all, n, path) >= 0;
}

int upins_add(const char *path) {
    struct sys_stat st;
    if (!path || path[0] != '/' || strlen(path) >= UPINS_PATH ||
        sys_stat(path, &st) != 0 || !st.is_dir) return -EINVAL;
    int n = read_all(g_all, UPINS_MAX);
    if (index_of(g_all, n, path) >= 0) return -EEXIST;
    if (n >= UPINS_MAX) return -ENOSPC;
    strlcpy(g_all[n].path, path, UPINS_PATH);
    g_all[n++].label[0] = '\0';
    return write_all(g_all, n);
}

int upins_remove(const char *path) {
    int n = read_all(g_all, UPINS_MAX), i = index_of(g_all, n, path);
    if (i < 0) return -ENOENT;
    memmove(&g_all[i], &g_all[i + 1], (size_t)(n - i - 1) * sizeof g_all[0]);
    return write_all(g_all, n - 1);
}

void upins_label(const char *path, char *out, int cap) {
    int n = read_all(g_all, UPINS_MAX), i = index_of(g_all, n, path);
    const char *base = k_path_basename(path);
    strlcpy(out, i >= 0 && g_all[i].label[0] ? g_all[i].label : base[0] ? base : path, (size_t)cap);
}

int upins_set_label(const char *path, const char *label) {
    if (strpbrk(label, "\t\r\n")) return -EINVAL;
    int n = read_all(g_all, UPINS_MAX), i = index_of(g_all, n, path);
    if (i < 0) return -ENOENT;
    // Its own folder's name is no name of its own.
    strlcpy(g_all[i].label, strcmp(label, k_path_basename(path)) ? label : "", UPINS_LABEL);
    return write_all(g_all, n);
}

int upins_move(const char *path, int delta) {
    int n = read_all(g_all, UPINS_MAX), i = index_of(g_all, n, path);
    if (i < 0) return -ENOENT;
    int j = i + delta;
    if (j < 0) j = 0;
    if (j >= n) j = n - 1;
    if (j == i) return -ERANGE;
    struct pin p = g_all[i];
    if (j < i) memmove(&g_all[j + 1], &g_all[j], (size_t)(i - j) * sizeof p);
    else memmove(&g_all[i], &g_all[i + 1], (size_t)(j - i) * sizeof p);
    g_all[j] = p;
    return write_all(g_all, n);
}
