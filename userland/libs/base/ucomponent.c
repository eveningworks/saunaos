// See base/ucomponent.h, and docs/software-design.md for the why.
#include "base/ucomponent.h"
#include "base/uconf.h"
#include "base/ulistdir.h"
#include "base/usetting.h"
#include "base/uwalk.h"
#include "base/ugrow.h"
#include "rt/sys.h"
#include "update_abi.h"
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SUFFIX ".component"

// ---- the definitions ------------------------------------------------------------

static void comp_free(struct ucomponent *c) {
    free(c->mem);
    memset(c, 0, sizeof *c);
}

void ucomponent_free(struct ucomponent_set *s) {
    for (int i = 0; i < s->count; i++) comp_free(&s->c[i]);
    free(s->c);
    memset(s, 0, sizeof *s);
}

// One definition. Every value is copied into ONE allocation the size of
// the file, which no value can exceed.
static int read_def(const char *path, const char *id, struct ucomponent *c) {
    static const char *const KEY[] = { "Name", "Summary", "Project", "Category", "License",
                                       "Icon", "Needs", "Paths", "Size", "Files" };
    enum { NKEY = (int)(sizeof KEY / sizeof KEY[0]) };
    memset(c, 0, sizeof *c);
    struct etc_config_buf cfg;
    if (!uconf_load(path, &cfg)) return 0;
    uint32_t room = (uint32_t)cfg.size + 1;
    c->mem = malloc((size_t)room * 2 + NKEY);
    if (!c->mem) { etc_config_buf_free(&cfg); return 0; }
    char *at = c->mem, *end = c->mem + (size_t)room * 2 + NKEY;
    char *val[NKEY];
    for (int k = 0; k < NKEY; k++) {
        val[k] = at;
        if (!etc_config_buf_get(&cfg, KEY[k], at, (uint32_t)(end - at))) *at = '\0';
        at += strlen(at) + 1;
    }
    etc_config_buf_free(&cfg);
    c->name = val[0]; c->summary = val[1]; c->project = val[2]; c->category = val[3];
    c->license = val[4]; c->icon = val[5]; c->needs = val[6]; c->paths = val[7];
    c->size = strtoull(val[8], 0, 10);
    c->files = atoi(val[9]);
    strlcpy(c->id, id, sizeof c->id);
    if (!c->name[0]) c->name = c->id;
    return c->paths[0] != '\0';
}

static int is_def(const char *name, char *id, size_t cap) {
    size_t n = strlen(name), s = sizeof SUFFIX - 1;
    if (n <= s || strcmp(name + n - s, SUFFIX) != 0 || n - s >= cap) return 0;
    memcpy(id, name, n - s);
    id[n - s] = '\0';
    return 1;
}

static void mark_removed(const char *id, void *ctx) {
    struct ucomponent_set *s = ctx;
    int i = ucomponent_find(s, id);
    if (i >= 0) s->c[i].removed = 1;
}

int ucomponent_load(struct ucomponent_set *s) {
    ucomponent_free(s);
    struct sys_dirent *ents = 0;
    int n = ulistdir(UCOMPONENT_DIR, &ents);
    if (n < 0) return -1;
    for (int i = 0; i < n; i++) {
        char id[UCOMPONENT_ID_MAX], path[sizeof UCOMPONENT_DIR + 72];
        if (ents[i].is_dir || !is_def(ents[i].name, id, sizeof id)) continue;
        snprintf(path, sizeof path, "%s/%s", UCOMPONENT_DIR, ents[i].name);
        if (!UGROW(s->c, s->cap, s->count + 1)) {
            fprintf(stderr, "component: out of memory at %s, the rest not listed\n", id);
            break;
        }
        if (read_def(path, id, &s->c[s->count])) s->count++;
        else comp_free(&s->c[s->count]);
    }
    free(ents);
    // By Name, as a person looks for one.
    for (int i = 1; i < s->count; i++)
        for (int j = i; j > 0 && strcmp(s->c[j].name, s->c[j - 1].name) < 0; j--) {
            struct ucomponent t = s->c[j];
            s->c[j] = s->c[j - 1];
            s->c[j - 1] = t;
        }
    ucomponent_state_each(mark_removed, s);
    return 0;
}

int ucomponent_find(const struct ucomponent_set *s, const char *id) {
    for (int i = 0; i < s->count; i++)
        if (!strcmp(s->c[i].id, id)) return i;
    return -1;
}

// Walks a space-separated list: the next word into [*w, *w + *n).
static int next_word(const char **p, const char **w, size_t *n) {
    const char *s = *p;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return 0;
    const char *e = s;
    while (*e && *e != ' ' && *e != '\t') e++;
    *w = s;
    *n = (size_t)(e - s);
    *p = e;
    return 1;
}

int ucomponent_owner(const struct ucomponent_set *s, const char *path) {
    int best = -1;
    size_t best_len = 0, plen = strlen(path);
    for (int i = 0; i < s->count; i++) {
        const char *p = s->c[i].paths, *w;
        size_t n;
        while (next_word(&p, &w, &n)) {
            int dir = w[n - 1] == '/';
            int hit = dir ? (plen >= n && !strncmp(path, w, n)) : (plen == n && !strncmp(path, w, n));
            if (hit && (best < 0 || n > best_len)) { best = i; best_len = n; }
        }
    }
    return best;
}

int ucomponent_needs(const struct ucomponent *c, const char *id) {
    const char *p = c->needs, *w;
    size_t n, len = strlen(id);
    while (next_word(&p, &w, &n))
        if (n == len && !strncmp(w, id, n)) return 1;
    return 0;
}

int ucomponent_is_gpl(const struct ucomponent *c) {
    const char *p = c->license;
    while (*p) {
        while (*p == ' ' || *p == '(' || *p == ')') p++;
        if (!strncmp(p, "GPL-", 4) || !strncmp(p, "AGPL-", 5)) return 1;
        while (*p && *p != ' ' && *p != '(' && *p != ')') p++;
    }
    return 0;
}

int ucomponent_dependents(const struct ucomponent_set *s, int idx, int *out, int cap) {
    int n = 0;
    for (int i = 0; i < s->count; i++)
        if (i != idx && !s->c[i].removed && ucomponent_needs(&s->c[i], s->c[idx].id)) {
            if (n < cap) out[n] = i;
            n++;
        }
    return n;
}

int ucomponent_remove_closure(const struct ucomponent_set *s, int idx, int *out, int cap) {
    int n = ucomponent_dependents(s, idx, out, cap);
    if (n > cap) n = cap;
    if (n < cap) out[n++] = idx;
    return n;
}

int ucomponent_add_closure(const struct ucomponent_set *s, int idx, int *out, int cap) {
    int n = 0;
    const char *p = s->c[idx].needs, *w;
    size_t len;
    while (next_word(&p, &w, &len) && n < cap) {
        char id[UCOMPONENT_ID_MAX];
        if (len >= sizeof id) continue;
        memcpy(id, w, len);
        id[len] = '\0';
        int k = ucomponent_find(s, id);
        if (k >= 0 && s->c[k].removed) out[n++] = k;
    }
    if (n < cap) out[n++] = idx;
    return n;
}

// ---- the state file -------------------------------------------------------------

int ucomponent_state_each(void (*fn)(const char *id, void *ctx), void *ctx) {
    FILE *fp = fopen(UCOMPONENT_STATE, "r");
    if (!fp) return 0;
    char line[96];
    int n = 0;
    while (fgets(line, sizeof line, fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "removed ", 8) != 0) continue;   // `added`, comments: not a removal
        const char *id = line + 8;
        while (*id == ' ') id++;
        if (!*id) continue;
        fn(id, ctx);
        n++;
    }
    fclose(fp);
    return n;
}

struct has { const char *id; int hit; };
static void has_cb(const char *id, void *ctx) {
    struct has *h = ctx;
    if (!strcmp(id, h->id)) h->hit = 1;
}

int ucomponent_state_removed(const char *id) {
    struct has h = { id, 0 };
    ucomponent_state_each(has_cb, &h);
    return h.hit;
}

static void mkdir_parents(const char *path) {
    char dir[128];
    snprintf(dir, sizeof dir, "%s", path);
    for (char *s = dir + 1; *s; s++) {
        if (*s != '/') continue;
        *s = '\0';
        struct stat st;
        if (stat(dir, &st) != 0) mkdir(dir, 0755);
        *s = '/';
    }
}

// The file rewritten WHOLE through a rename, so a power cut leaves the
// old list or the new one. Every line is kept but `id`'s own, which is
// put back when `removed`.
int ucomponent_state_set(const char *id, int removed) {
    FILE *in = fopen(UCOMPONENT_STATE, "r");
    char tmp[sizeof UCOMPONENT_STATE + 8];
    snprintf(tmp, sizeof tmp, "%s.new", UCOMPONENT_STATE);
    mkdir_parents(UCOMPONENT_STATE);
    FILE *out = fopen(tmp, "w");
    if (!out) { if (in) fclose(in); return -1; }
    int had_header = 0;
    char line[96];
    while (in && fgets(line, sizeof line, in)) {
        char t[96];
        snprintf(t, sizeof t, "%s", line);
        t[strcspn(t, "\r\n")] = '\0';
        if (t[0] == '#') had_header = 1;
        const char *w = !strncmp(t, "removed ", 8) ? t + 8 : !strncmp(t, "added ", 6) ? t + 6 : 0;
        if (w) {
            while (*w == ' ') w++;
            if (!strcmp(w, id)) continue;
        }
        fputs(t, out);
        fputc('\n', out);
    }
    if (in) fclose(in);
    if (!had_header && removed)
        fputs("# The optional components removed here (docs/software-design.md); `component add` undoes one.\n", out);
    if (removed) fprintf(out, "removed %s\n", id);
    int bad = ferror(out) != 0;
    if (fclose(out) != 0) bad = 1;
    if (bad || sys_rename2(tmp, UCOMPONENT_STATE, RENAME2_REPLACE) != 0) { unlink(tmp); return -1; }
    return 0;
}

// ---- removing ---------------------------------------------------------------------

struct rm_list {
    char **path;
    int count, cap;
    char **dir;
    int ndir, dcap;
    int oom;
};

static void rm_add(char ***arr, int *count, int *cap, int *oom, const char *path) {
    char *d = strdup(path);
    if (!d || !UGROW(*arr, *cap, *count + 1)) { free(d); *oom = 1; return; }
    (*arr)[(*count)++] = d;
}

struct rm_walk { struct rm_list *l; const struct ucomponent_set *s; int idx; };

static int rm_visit(void *ctx, const char *path, const struct sys_dirent *e) {
    struct rm_walk *w = ctx;
    if (e->is_dir) {
        rm_add(&w->l->dir, &w->l->ndir, &w->l->dcap, &w->l->oom, path);
        return 1;
    }
    // A LONGER path gives the file to another component (/tests/nsmonkey).
    if (ucomponent_owner(w->s, path) == w->idx)
        rm_add(&w->l->path, &w->l->count, &w->l->cap, &w->l->oom, path);
    return 1;
}

static void say(const struct ucomponent_progress *pr, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void say(const struct ucomponent_progress *pr, const char *fmt, ...) {
    if (!pr || !pr->log) return;
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    pr->log(pr->ctx, line);
}

static int collect(const struct ucomponent_set *s, int idx, struct rm_list *l) {
    const char *p = s->c[idx].paths, *w;
    size_t n;
    struct uwalk *walk = malloc(sizeof *walk);
    if (!walk) return -1;
    struct rm_walk ctx = { l, s, idx };
    while (next_word(&p, &w, &n)) {
        char path[UWALK_PATH];
        if (n >= sizeof path) continue;
        memcpy(path, w, n);
        path[n] = '\0';
        if (path[n - 1] != '/') {
            struct stat st;
            if (stat(path, &st) == 0 && !S_ISDIR(st.st_mode) && ucomponent_owner(s, path) == idx)
                rm_add(&l->path, &l->count, &l->cap, &l->oom, path);
            continue;
        }
        path[n - 1] = '\0';                 // the walk takes the directory's own name
        if (!uwalk_begin(walk, path)) continue;
        rm_add(&l->dir, &l->ndir, &l->dcap, &l->oom, path);
        while (uwalk_step(walk, rm_visit, &ctx)) {}
    }
    free(walk);
    return l->oom ? -1 : 0;
}

static void list_free(struct rm_list *l) {
    for (int i = 0; i < l->count; i++) free(l->path[i]);
    for (int i = 0; i < l->ndir; i++) free(l->dir[i]);
    free(l->path);
    free(l->dir);
    memset(l, 0, sizeof *l);
}

int ucomponent_remove(const struct ucomponent_set *s, int idx, const struct ucomponent_progress *pr,
                      int *at_boot, char *why, int whycap) {
    if (at_boot) *at_boot = 0;
    if (idx < 0 || idx >= s->count) { snprintf(why, (size_t)whycap, "no such component"); return -1; }
    const struct ucomponent *c = &s->c[idx];
    struct rm_list l;
    memset(&l, 0, sizeof l);
    if (collect(s, idx, &l) != 0) {
        list_free(&l);
        snprintf(why, (size_t)whycap, "out of memory listing %s's files", c->id);
        return -1;
    }
    int failed = 0, staged = 0;
    int pend = -1;
    for (int i = 0; i < l.count; i++) {
        const char *f = l.path[i];
        if (!strncmp(f, "/lib/", 5)) {
            // A library may be paged in by path under a running process:
            // the kernel removes it at the next boot, before init.
            if (pend < 0) {
                mkdir_parents(UPDATE_PENDING_PATH);
                pend = open(UPDATE_PENDING_PATH, O_WRONLY | O_CREAT | O_APPEND);
            }
            char line[UWALK_PATH + 4];
            int len = snprintf(line, sizeof line, "%c%s\n", UPDATE_REMOVE_PREFIX, f);
            if (pend < 0 || write(pend, line, (size_t)len) != len) failed++;
            else staged++;
        } else if (unlink(f) != 0 && errno != ENOENT) {
            say(pr, "%s: could not remove it", f);
            failed++;
        }
        if (pr && pr->file) pr->file(pr->ctx, i + 1, l.count);
    }
    if (pend >= 0) close(pend);
    // The directories, deepest first; one still holding a file (another
    // component's, or the owner's own) stays, which rmdir decides.
    for (int i = 1; i < l.ndir; i++)
        for (int j = i; j > 0 && strlen(l.dir[j]) > strlen(l.dir[j - 1]); j--) {
            char *t = l.dir[j];
            l.dir[j] = l.dir[j - 1];
            l.dir[j - 1] = t;
        }
    for (int i = 0; i < l.ndir; i++) rmdir(l.dir[i]);
    int files = l.count;
    list_free(&l);
    sys_sync();
    if (failed) {
        snprintf(why, (size_t)whycap, "%d of %d files could not be removed", failed, files);
        return -1;
    }
    if (ucomponent_state_set(c->id, 1) != 0) {
        snprintf(why, (size_t)whycap, "removed, but %s could not be written -- the next update "
                 "would bring it back", UCOMPONENT_STATE);
        return -1;
    }
    if (at_boot) *at_boot = staged > 0;
    say(pr, "%s: removed %d file%s%s", c->id, files, files == 1 ? "" : "s",
        staged ? " (the libraries at the next restart)" : "");
    return 0;
}

// ---- the GPL switch ---------------------------------------------------------------

int ucomponent_show_gpl(void) {
    char v[8];
    if (!usetting_get(UCOMPONENT_GPL_SETTING, v, sizeof v) || !v[0]) return 1;
    return strcmp(v, "off") != 0;
}

void ucomponent_view_load(struct ucomponent_view *v) {
    v->hiding = 0;
    if (ucomponent_show_gpl()) { ucomponent_free(&v->set); return; }
    ucomponent_load(&v->set);
    for (int i = 0; i < v->set.count; i++)
        if (ucomponent_is_gpl(&v->set.c[i])) v->hiding = 1;
}

int ucomponent_view_hides(const struct ucomponent_view *v, const char *path) {
    if (!v->hiding || !path || !path[0]) return 0;
    int i = ucomponent_owner(&v->set, path);
    return i >= 0 && ucomponent_is_gpl(&v->set.c[i]);
}

int ucomponent_view_hides_exec(const struct ucomponent_view *v, const char *exec) {
    if (!v->hiding || !exec) return 0;
    char prog[UWALK_PATH];
    size_t n = strcspn(exec, " \t");
    if (n >= sizeof prog) return 0;
    memcpy(prog, exec, n);
    prog[n] = '\0';
    return ucomponent_view_hides(v, prog);
}

void ucomponent_view_free(struct ucomponent_view *v) {
    ucomponent_free(&v->set);
    v->hiding = 0;
}

const char *ucomponent_category_label(const char *key) {
    static const struct { const char *key, *label; } T[] = {
        { "games", "Games" }, { "internet", "Internet" }, { "devtools", "Developer tools" },
        { "win16", "Windows 3.x" }, { "demos", "Demos and tests" }, { "system", "System" },
        { "graphics", "Graphics" }, { "multimedia", "Multimedia" }, { "utility", "Utilities" },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
        if (!strcmp(T[i].key, key)) return T[i].label;
    return key;
}
