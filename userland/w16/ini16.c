// ini16.c -- KERNEL's profile calls over real INI files on drive C:
// (w16prefs.h's drive_c, /home/Win16 by default): Get/WritePrivateProfile*
// on a program's own file, Get/WriteProfile* on WIN.INI.
//
// A file named without a path is in C:\WINDOWS, as Windows put it; one
// on C: is mapped below drive_c; another drive is refused, and the call
// answers its default. Names compare without case and `;` starts a
// comment, as Windows' reader did. A write rewrites the whole file -- an
// INI file is a few hundred bytes -- and creates C:\WINDOWS on the way.
//
// THE FILE IS THE PROGRAM'S OWN DATA, read back into it: a line too long
// is cut, a file too large is read to INI_MAX and no further.
#include "w16_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define INI_MAX (64 * 1024)

char g_w16_drive_c[128] = "/home/Win16";

static int ieq(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y) return 0;
        if (!x) return 1;
    }
    return 1;
}

static int ieqs(const char *a, const char *b) {
    size_t n = strlen(a);
    return n == strlen(b) && ieq(a, b, n);
}

// FILE -> a host path below drive C:; 0 when it names another drive.
static int host_path(const char *file, char *out, size_t cap) {
    char rel[160];
    if (file[0] && file[1] == ':') {
        if (file[0] != 'C' && file[0] != 'c') return 0;
        snprintf(rel, sizeof rel, "%s", file + 2);
    } else if (strchr(file, '\\') || strchr(file, '/')) {
        snprintf(rel, sizeof rel, "%s", file);            // relative to C:'s root
    } else {
        snprintf(rel, sizeof rel, "\\WINDOWS\\%s", file);
    }
    for (char *p = rel; *p; p++) {
        if (*p == '\\') *p = '/';
        else if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
        if (p[0] == '.' && p[1] == '.') return 0;          // nothing above drive C:
    }
    snprintf(out, cap, "%s%s%s", g_w16_drive_c, rel[0] == '/' ? "" : "/", rel);
    return 1;
}

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char *buf = malloc(INI_MAX + 1);
    if (!buf) { fclose(f); return 0; }
    *len = fread(buf, 1, INI_MAX, f);
    fclose(f);
    buf[*len] = 0;
    return buf;
}

// One line, trimmed, its comment gone; returns the next line.
static const char *next_line(const char *p, const char **s, const char **e) {
    const char *nl = strchr(p, '\n');
    const char *end = nl ? nl : p + strlen(p);
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    const char *q = end;
    while (q > p && (q[-1] == '\r' || q[-1] == ' ' || q[-1] == '\t')) q--;
    *s = p;
    *e = (*p == ';') ? p : q;
    return nl ? nl + 1 : 0;
}

// Calls fn for each (section, key, value) and each section header (key 0).
typedef int (*ini_fn)(void *ctx, const char *sec, const char *key, const char *val);
static void walk(const char *text, ini_fn fn, void *ctx) {
    char sec[96] = "", key[96], val[256];
    for (const char *p = text; p; ) {
        const char *s, *e;
        p = next_line(p, &s, &e);
        if (s == e) continue;
        if (*s == '[') {
            const char *c = memchr(s, ']', (size_t)(e - s));
            size_t n = (size_t)((c ? c : e) - s - 1);
            if (n >= sizeof sec) n = sizeof sec - 1;
            memcpy(sec, s + 1, n);
            sec[n] = 0;
            if (fn(ctx, sec, 0, 0)) return;
            continue;
        }
        const char *eq = memchr(s, '=', (size_t)(e - s));
        if (!eq) continue;
        const char *ke = eq;
        while (ke > s && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
        const char *vs = eq + 1;
        while (vs < e && (*vs == ' ' || *vs == '\t')) vs++;
        size_t kn = (size_t)(ke - s), vn = (size_t)(e - vs);
        if (kn >= sizeof key) kn = sizeof key - 1;
        if (vn >= sizeof val) vn = sizeof val - 1;
        memcpy(key, s, kn); key[kn] = 0;
        memcpy(val, vs, vn); val[vn] = 0;
        if (fn(ctx, sec, key, val)) return;
    }
}

struct lookup { const char *sec, *key; char *out; size_t cap; int found; };

static int find_fn(void *ctx, const char *sec, const char *key, const char *val) {
    struct lookup *l = ctx;
    if (key && ieqs(sec, l->sec) && ieqs(key, l->key)) {
        snprintf(l->out, l->cap, "%s", val);
        l->found = 1;
        return 1;
    }
    return 0;
}

// The value of [sec] key in FILE; 1 when there is one.
int w16_ini_get(const char *file, const char *sec, const char *key, char *out, size_t cap) {
    char path[256];
    size_t len;
    if (!host_path(file, path, sizeof path)) return 0;
    char *text = slurp(path, &len);
    if (!text) return 0;
    struct lookup l = { sec, key, out, cap, 0 };
    walk(text, find_fn, &l);
    free(text);
    return l.found;
}

struct names { const char *sec; char *out; size_t cap, n; };

static void add_name(struct names *ns, const char *s) {
    size_t k = strlen(s);
    if (ns->n + k + 2 > ns->cap) return;
    memcpy(ns->out + ns->n, s, k + 1);
    ns->n += k + 1;
}

static int names_fn(void *ctx, const char *sec, const char *key, const char *val) {
    (void)val;
    struct names *ns = ctx;
    if (!ns->sec && !key) add_name(ns, sec);
    else if (ns->sec && key && ieqs(sec, ns->sec)) add_name(ns, key);
    return 0;
}

// Every section name (sec NULL) or every key of [sec], each NUL-ended and
// the list ended by a second NUL -- what GetProfileString answers for a
// NULL name. The bytes written, the last NUL not counted.
size_t w16_ini_names(const char *file, const char *sec, char *out, size_t cap) {
    char path[256];
    size_t len, n = 0;
    if (cap < 2) return 0;
    out[0] = out[1] = 0;
    if (!host_path(file, path, sizeof path)) return 0;
    char *text = slurp(path, &len);
    if (!text) return 0;
    struct names ns = { sec, out, cap, 0 };
    walk(text, names_fn, &ns);
    free(text);
    n = ns.n;
    out[n] = 0;
    return n;
}

// Sets [sec] key=val in FILE (val NULL removes the key; key NULL removes
// the section), rewriting the file. 1 on success.
int w16_ini_set(const char *file, const char *sec, const char *key, const char *val) {
    char path[256];
    if (!host_path(file, path, sizeof path)) return 0;
    size_t len = 0;
    char *text = slurp(path, &len);
    size_t cap = len + strlen(sec) + (key ? strlen(key) : 0) + (val ? strlen(val) : 0) + 64;
    char *out = malloc(cap);
    if (!out) { free(text); return 0; }
    size_t n = 0;
    int in_sec = 0, seen_sec = 0, done = 0;
    for (const char *p = text; p; ) {
        const char *line = p, *s, *e;
        p = next_line(p, &s, &e);
        const char *raw_end = p ? p : line + strlen(line);
        int keep = 1;
        if (s < e && *s == '[') {
            if (in_sec && !done && key && val) {             // a new key goes at its section's end
                n += (size_t)snprintf(out + n, cap - n, "%s=%s\r\n", key, val);
                done = 1;
            }
            const char *c = memchr(s, ']', (size_t)(e - s));
            size_t sn = (size_t)((c ? c : e) - s - 1);
            in_sec = sn == strlen(sec) && ieq(s + 1, sec, sn);
            seen_sec |= in_sec;
            if (in_sec && !key) keep = 0;
        } else if (in_sec) {
            if (!key) keep = 0;
            else {
                const char *eq = memchr(s, '=', (size_t)(e - s));
                const char *ke = eq;
                while (eq && ke > s && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
                if (eq && (size_t)(ke - s) == strlen(key) && ieq(s, key, strlen(key))) {
                    keep = 0;
                    if (val && !done) n += (size_t)snprintf(out + n, cap - n, "%s=%s\r\n", key, val);
                    done = 1;
                }
            }
        }
        if (keep && n + (size_t)(raw_end - line) < cap) {
            memcpy(out + n, line, (size_t)(raw_end - line));
            n += (size_t)(raw_end - line);
            if (!p && n && out[n - 1] != '\n') { out[n++] = '\r'; out[n++] = '\n'; }
        }
    }
    if (key && val && !done) {
        if (!seen_sec) n += (size_t)snprintf(out + n, cap - n, "[%s]\r\n", sec);
        n += (size_t)snprintf(out + n, cap - n, "%s=%s\r\n", key, val);
    }
    free(text);
    char dir[256];
    snprintf(dir, sizeof dir, "%s", path);
    mkdir(g_w16_drive_c, 0755);
    for (char *slash = dir + strlen(g_w16_drive_c) + 1; (slash = strchr(slash, '/')); slash++) {
        *slash = 0;
        mkdir(dir, 0755);
        *slash = '/';
    }
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(out, 1, n, f) == n;
    if (f && fclose(f) != 0) ok = 0;
    free(out);
    return ok;
}
