// Recent files -- see urecent.h.
#include "base/urecent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include "rt/sys.h"
#include "kpath.h"
#include "base/uappentry.h"

#include "base/udatadir.h"

// The list and its two companions, named once a process: the person's
// state folder (users stage D1c), root's /var/lib.
static char *g_file, *g_tmp, *g_lockf, *g_dir;
static int names(void) {
    if (g_lockf) return 1;
    char *f = udatadir_state_prepare("recent", "/var/lib/recent"), *t = 0, *l = 0;
    if (!f || asprintf(&t, "%s.new", f) < 0 || asprintf(&l, "%s.lock", f) < 0) {
        free(f);
        free(t);
        return 0;
    }
    g_file = f;
    g_tmp = t;
    if ((g_dir = strdup(f))) *strrchr(g_dir, '/') = '\0';
    g_lockf = l;   // last: names() is done once this is set
    return 1;
}
#define URECENT_FILE g_file
#define TMP_FILE  g_tmp
#define LOCK_FILE g_lockf

// EVERY READ AND EVERY REWRITE HOLDS A LOCK FILE: the double click that
// spawns Notepad records the file, and Notepad records it again as it
// opens, so two writers race on every open -- and a rename does not
// replace a file here, so between the old list's unlink and the new
// one's rename a reader sees no list and writes back only its own line.
// A holder takes milliseconds; one that died holding it is waited out
// for a second and then broken.
static void lock(void) {
    if (!names()) return;
    for (int i = 0; i < 100; i++) {
        int fd = open(LOCK_FILE, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd >= 0) { close(fd); return; }
        if (errno != EEXIST) return;   // cannot lock at all: go on unlocked
        sys_sleep_ms(10);
    }
    // Stale: its holder died. Take it over.
}

static void unlock(void) { if (names()) unlink(LOCK_FILE); }

// Every line, oldest first, missing files included. Heap: a full list is
// ~30 KB, and every caller is some app's ring-3 frame.
void urecent_list_free(struct urecent_item *items, int n) {
    for (int i = 0; i < n; i++) { free(items[i].path); items[i].path = 0; }
}

// The last `cap` lines -- the file keeps URECENT_MAX, a hand-edited one
// may hold more, and the newest are at the end.
static int read_all(struct urecent_item *out, int cap) {
    if (!names()) return 0;
    FILE *f = fopen(URECENT_FILE, "r");
    if (!f) return 0;
    int n = 0;
    char *line = 0;
    size_t lcap = 0;
    while (getline(&line, &lcap, f) > 0) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        char *sp = strchr(line, ' '), *tab = sp ? strchr(sp, '\t') : 0;
        if (!sp || !tab || tab[1] != '/') continue;   // not a line this file holds
        *sp = *tab = '\0';
        char *path = strdup(tab + 1);
        if (!path) continue;
        if (n == cap) {   // the oldest goes
            free(out[0].path);
            memmove(out, out + 1, (size_t)(cap - 1) * sizeof *out);
            n--;
        }
        struct urecent_item *it = &out[n++];
        it->when = strtoll(line, 0, 10);
        strlcpy(it->app, sp + 1, sizeof it->app);
        it->path = path;
    }
    free(line);
    fclose(f);
    return n;
}

static int write_all(const struct urecent_item *in, int n) {
    if (!names()) return -ENOMEM;
    FILE *f = fopen(TMP_FILE, "w");
    if (!f) return -errno;
    for (int i = 0; i < n; i++) fprintf(f, "%lld %s\t%s\n", in[i].when, in[i].app, in[i].path);
    if (fclose(f) != 0) { unlink(TMP_FILE); return -EIO; }
    // A rename does not replace a file here, so the old list goes first:
    // a crash between the two leaves the new one under TMP_FILE.
    unlink(URECENT_FILE);
    return sys_rename(TMP_FILE, URECENT_FILE) == 0 ? 0 : -EIO;
}

// Drop `path` from the list, keeping the order. The new count.
static int drop(struct urecent_item *all, int n, const char *path) {
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(all[i].path, path)) { free(all[i].path); continue; }
        if (k != i) all[k] = all[i];
        k++;
    }
    return k;
}

int urecent_add(const char *path, const char *app) {
    if (!path || path[0] != '/' || strchr(path, '\n')) return -EINVAL;
    struct urecent_item *all = malloc((URECENT_MAX + 1) * sizeof *all);
    char *copy = strdup(path);
    if (!all || !copy) { free(all); free(copy); return -ENOMEM; }
    lock();
    int n = drop(all, read_all(all, URECENT_MAX), path);
    if (n >= URECENT_MAX) {   // the oldest go
        int over = n - URECENT_MAX + 1;
        urecent_list_free(all, over);
        memmove(all, all + over, (URECENT_MAX - 1) * sizeof *all);
        n = URECENT_MAX - 1;
    }
    struct urecent_item *it = &all[n++];
    it->when = (long long)time(0);
    // The app's name is one field of a tab-separated line.
    int k = 0;
    for (const char *s = app && app[0] ? app : "?"; *s && k < URECENT_APP - 1; s++)
        it->app[k++] = (*s == '\t' || *s == '\n') ? ' ' : *s;
    it->app[k] = '\0';
    it->path = copy;
    int e = write_all(all, n);
    unlock();
    urecent_list_free(all, n);
    free(all);
    return e;
}

int urecent_list(struct urecent_item *out, int cap) {
    struct urecent_item *all = malloc(URECENT_MAX * sizeof *all);
    if (!all) return 0;
    lock();
    int n = read_all(all, URECENT_MAX), k = 0;
    unlock();
    for (int i = n - 1; i >= 0; i--) {
        struct sys_stat st;
        if (k < cap && sys_stat(all[i].path, &st) == 0) { out[k++] = all[i]; all[i].path = 0; }
        else free(all[i].path);
    }
    free(all);
    return k;
}

int urecent_any(void) {
    struct urecent_item one;
    int n = urecent_list(&one, 1);
    urecent_list_free(&one, n);
    return n > 0;
}

int urecent_forget(const char *path) {
    struct urecent_item *all = malloc(URECENT_MAX * sizeof *all);
    if (!all) return -ENOMEM;
    lock();
    int n = read_all(all, URECENT_MAX), k = drop(all, n, path);
    int e = k == n ? -ENOENT : write_all(all, k);
    unlock();
    urecent_list_free(all, k);
    free(all);
    return e;
}

int urecent_clear(void) {
    lock();
    int e = write_all(0, 0);
    unlock();
    return e;
}

void urecent_app_of_exec(const char *exec, char *out, int cap) {
    struct uappentry e;
    if (uappentry_find_exec(exec, &e)) strlcpy(out, e.name, (size_t)cap);
    else strlcpy(out, k_path_basename(exec), (size_t)cap);
}

const char *urecent_dir(void) { return names() && g_dir ? g_dir : "/var/lib"; }
