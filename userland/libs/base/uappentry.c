// See base/uappentry.h.
#include "base/uappentry.h"
#include "ulistdir.h"
#include "base/uconf.h"
#include "base/uopen.h"   // UOPEN_ENTRY_SECTION
#include "base/ucomponent.h" // the GPL switch: an entry it hides
#include "rt/sys.h"
#include "kpath.h"
#include <stdlib.h>
#include <string.h>
#include "base/upath.h"

void uappentry_free(struct uappentry *e) {
    if (!e) return;
    free(e->stem);
    memset(e, 0, sizeof *e);
}

int uappentry_copy(struct uappentry *dst, const struct uappentry *src) {
    const char *f[5] = { src->stem, src->name, src->exec, src->icon, src->category };
    char **to[5] = { &dst->stem, &dst->name, &dst->exec, &dst->icon, &dst->category };
    size_t len[5], all = 0;
    for (int k = 0; k < 5; k++) all += (len[k] = strlen(f[k])) + 1;
    char *m = malloc(all);
    memset(dst, 0, sizeof *dst);
    if (!m) return 0;
    for (int k = 0; k < 5; k++) {
        memcpy(m, f[k], len[k] + 1);
        *to[k] = m;
        m += len[k] + 1;
    }
    return 1;
}

int uappentry_read(const char *path, struct uappentry *out) {
    memset(out, 0, sizeof *out);
    struct etc_config_buf cfg;
    char *v[4] = { 0 };   // Exec, Name, Icon, Category
    static const char *const KEYS[4] = { "Exec", "Name", "Icon", "Category" };
    int ok = uconf_load(path, &cfg);
    for (int k = 0; ok && k < 4; k++) v[k] = uconf_buf_dup_in_or_top(&cfg, UOPEN_ENTRY_SECTION, KEYS[k]);
    etc_config_buf_free(&cfg);
    char *sp = v[0] ? strchr(v[0], ' ') : 0;
    if (sp) *sp = '\0';
    const char *base = k_path_basename(path);
    size_t stem = strlen(base);
    if (stem > 8 && !strcmp(base + stem - 8, ".desktop")) stem -= 8;
    const char *name = v[1] && v[1][0] ? v[1] : 0;
    // stem, name, exec, icon, category, one after another.
    size_t len[5] = { stem, name ? strlen(name) : stem, v[0] ? strlen(v[0]) : 0,
                      v[2] ? strlen(v[2]) : 0, v[3] ? strlen(v[3]) : 0 };
    char *m = ok && v[0] && v[0][0] ? malloc(len[0] + len[1] + len[2] + len[3] + len[4] + 5) : 0;
    if (m) {
        const char *src[5] = { base, name ? name : base, v[0], v[2] ? v[2] : "", v[3] ? v[3] : "" };
        char **dst[5] = { &out->stem, &out->name, &out->exec, &out->icon, &out->category };
        char *at = m;
        for (int k = 0; k < 5; k++) {
            memcpy(at, src[k], len[k]);
            at[len[k]] = '\0';
            *dst[k] = at;
            at += len[k] + 1;
        }
    }
    for (int k = 0; k < 4; k++) free(v[k]);
    return m != 0;
}

static int is_entry(const char *name) {
    size_t n = strlen(name);
    return n > 8 && strcmp(name + n - 8, ".desktop") == 0;
}

// `hide`: leave out what the GPL switch hides -- the walk a LIST makes,
// not a lookup of a program that is already running.
static int walk(int (*fn)(const struct uappentry *e, void *ctx), void *ctx, int hide) {
    struct ulist_entry *ents = 0;
    int n = ulistdir(UAPPENTRY_DIR, &ents);
    int seen = 0;
    struct ucomponent_view view = { 0 };
    if (hide) ucomponent_view_load(&view);
    for (int i = 0; i < n; i++) {
        if (ents[i].is_dir || !is_entry(ents[i].name)) continue;
        char *path = upath_join(UAPPENTRY_DIR, ents[i].name);
        struct uappentry e;
        int got = path && uappentry_read(path, &e);
        int hidden = got && (ucomponent_view_hides(&view, path) || ucomponent_view_hides(&view, e.exec));
        free(path);
        if (!got) continue;
        if (hidden) { uappentry_free(&e); continue; }
        seen++;
        int stop = fn(&e, ctx);
        uappentry_free(&e);
        if (stop) break;
    }
    ucomponent_view_free(&view);
    free(ents);
    return seen;
}

int uappentry_each(int (*fn)(const struct uappentry *e, void *ctx), void *ctx) {
    return walk(fn, ctx, 1);
}

int uappentry_each_all(int (*fn)(const struct uappentry *e, void *ctx), void *ctx) {
    return walk(fn, ctx, 0);
}

struct find { const char *exec; struct uappentry *out; int hit; };

static int match(const struct uappentry *e, void *ctx) {
    struct find *f = ctx;
    if (strcmp(e->exec, f->exec) != 0) return 0;
    // The walk frees `e` when this returns: the caller gets its own.
    struct uappentry copy;
    f->hit = uappentry_copy(&copy, e);
    if (f->hit) *f->out = copy;
    return 1;
}

int uappentry_find_exec(const char *exec, struct uappentry *out) {
    if (!exec || !exec[0]) return 0;
    struct find f = { exec, out, 0 };
    walk(match, &f, 0);
    return f.hit;
}
