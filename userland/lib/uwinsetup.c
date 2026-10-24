// See uwinsetup.h. Windows 3.x's Setup, Express mode, from SETUP.INF.
#include "lib/uwinsetup.h"
#include "lib/ufatimg.h"
#include "lib/umsexpand.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define NAME_MAX_DOS 12
#define IMG_MAX      (64u << 20)   // a disk image this reads whole: DMF is 1.7 MB, FAT16 ones far less than this
#define INF_MAX      (1u << 20)
#define FIELD        96
#define FIELDS       16

struct entry {
    char name[NAME_MAX_DOS + 1];
    uint32_t size;
    struct ufatimg_ent fat;        // an image's entry
};

struct container {
    char *path;
    int is_img;
    uint8_t *img;
    struct ufatimg fat;
    struct entry *ents;
    int n, cap;
};

struct section {
    char name[48];
    int first, count;
};

// A line Setup would have written into WIN.INI or SYSTEM.INI: added at the end of its section
// when the key is not there, so a kept INI gains it and keeps every value of the user's.
struct inikey {
    char file[12], sec[24], key[64], val[96];
};

struct uwinsetup {
    struct inikey *ini;
    int nini, capini;
    struct container *c;
    int nc, capc;
    char *inf;                     // SETUP.INF's text, split into lines in place
    char **lines;
    int nlines, caplines;
    struct section *secs;
    int nsecs, capsecs;
    struct uws_item *items;
    int nitems, capitems;
    char title[96], lang[8], windir[16], xlat[8], keydll[16], res[24];
    uint64_t bytes;
    int missing;
    struct umsx_scratch scratch;
};

static int grow(void **p, int *cap, int need, size_t elem) {
    if (need <= *cap) return 0;
    int n = *cap ? *cap * 2 : 16;
    while (n < need) n *= 2;
    void *q = realloc(*p, (size_t)n * elem);
    if (!q) return -ENOMEM;
    *p = q;
    *cap = n;
    return 0;
}

static char up(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }
static char low(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static int same(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (up(*a) != up(*b)) return 0;
    return !*a && !*b;
}

static int ends_with(const char *s, const char *tail) {
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && same(s + a - b, tail);
}

// `src` into `dst`, cut to fit: for names a check has already bounded.
static void scopy(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

// "a/b" into `out`: 0, or -ENAMETOOLONG rather than a path cut short.
static int join(char *out, size_t cap, const char *a, const char *b) {
    int k = snprintf(out, cap, "%s/%s", a, b);
    return k < 0 || (size_t)k >= cap ? -ENAMETOOLONG : 0;
}

static void say(char *err, size_t cap, const char *fmt, const char *a, const char *b) {
    if (err && cap) snprintf(err, cap, fmt, a, b);
}

// --- the disks ---------------------------------------------------------------------------------

static struct container *new_container(struct uwinsetup *s, const char *path) {
    if (grow((void **)&s->c, &s->capc, s->nc + 1, sizeof *s->c)) return 0;
    struct container *c = &s->c[s->nc];
    memset(c, 0, sizeof *c);
    if (!(c->path = strdup(path))) return 0;
    s->nc++;
    return c;
}

static int add_entry(struct container *c, const char *name, uint32_t size, const struct ufatimg_ent *fat) {
    if (strlen(name) > NAME_MAX_DOS) return 0;          // no DOS disk has it: not a setup file
    if (grow((void **)&c->ents, &c->cap, c->n + 1, sizeof *c->ents)) return -ENOMEM;
    struct entry *e = &c->ents[c->n++];
    memset(e, 0, sizeof *e);
    memcpy(e->name, name, strlen(name) + 1);
    e->size = size;
    if (fat) e->fat = *fat;
    return 0;
}

static int is_img(const char *name) { return ends_with(name, ".img") || ends_with(name, ".ima"); }

static int read_dir(struct uwinsetup *s, const char *path) {
    struct container *c = new_container(s, path);
    if (!c) return -ENOMEM;
    DIR *d = opendir(path);
    if (!d) return -errno;
    struct dirent *de;
    char full[1024];
    while ((de = readdir(d))) {
        struct stat st;
        if (de->d_name[0] == '.' || is_img(de->d_name)) continue;   // an image is a disk, read as one
        if (join(full, sizeof full, path, de->d_name) || stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (add_entry(c, de->d_name, (uint32_t)st.st_size, 0)) { closedir(d); return -ENOMEM; }
    }
    closedir(d);
    return 0;
}

// The whole file into memory; NULL when it cannot be read or is over `max`
// -- or, `partial`, just its first `max` bytes.
static uint8_t *slurp(const char *path, size_t max, size_t *n, int partial) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t *buf = 0;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len == cap) {
            if (cap >= max && partial) break;
            if (cap >= max) { free(buf); fclose(f); return 0; }
            size_t nc = cap ? cap * 2 : 65536;
            if (nc > max) nc = max;
            uint8_t *q = realloc(buf, nc);
            if (!q) { free(buf); fclose(f); return 0; }
            buf = q;
            cap = nc;
        }
        size_t got = fread(buf + len, 1, cap - len, f);
        if (!got) break;
        len += got;
    }
    fclose(f);
    *n = len;
    return buf;
}

static int img_ent(void *ctx, const struct ufatimg_ent *e) {
    struct container *c = ctx;
    if (e->attr & UFATIMG_DIR) return 0;
    return add_entry(c, e->name, e->size, e) ? 1 : 0;
}

static int read_img(struct uwinsetup *s, const char *path) {
    size_t n = 0;
    uint8_t *img = slurp(path, IMG_MAX, &n, 0);
    if (!img) return 0;                                  // unreadable: not one of the disks
    struct ufatimg fat;
    if (ufatimg_open(&fat, img, n) != 0) { free(img); return 0; }
    struct container *c = new_container(s, path);
    if (!c) { free(img); return -ENOMEM; }
    c->is_img = 1;
    c->img = img;
    c->fat = fat;
    ufatimg_list(&c->fat, 0, img_ent, c);   // a damaged directory lists what it can
    return 0;
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

// Every folder and image in `dir`, sorted, so the same disks always read alike.
static int read_folder_of_disks(struct uwinsetup *s, const char *dir, int images_only) {
    DIR *d = opendir(dir);
    if (!d) return -errno;
    char **names = 0;
    int n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (grow((void **)&names, &cap, n + 1, sizeof *names) || !(names[n] = strdup(de->d_name))) {
            closedir(d);
            while (n--) free(names[n]);
            free(names);
            return -ENOMEM;
        }
        n++;
    }
    closedir(d);
    if (n) qsort(names, (size_t)n, sizeof *names, by_name);   // an empty folder has no array
    int r = 0;
    char full[1024];
    for (int i = 0; i < n && !r; i++) {
        struct stat st;
        if (join(full, sizeof full, dir, names[i]) || stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode) && !images_only) r = read_dir(s, full);
        else if (S_ISREG(st.st_mode) && is_img(names[i])) r = read_img(s, full);
    }
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
    return r;
}

static int find(const struct uwinsetup *s, const char *name, int *ci, int *ei) {
    for (int i = 0; i < s->nc; i++)
        for (int j = 0; j < s->c[i].n; j++)
            if (same(s->c[i].ents[j].name, name)) {
                *ci = i;
                *ei = j;
                return 1;
            }
    return 0;
}

// The name a disk carries `want` under: as it is, or as COMPRESS -r left it --
// the last character of the extension a '_' ("MAIN.CPL" -> "MAIN.CP_",
// "WINVER" -> "WINVER._").
static int find_disk(const struct uwinsetup *s, const char *want, int *ci, int *ei, char *as) {
    char alt[NAME_MAX_DOS + 3];                          // "WINVER" + "._"
    const char *dot = strchr(want, '.');
    if (find(s, want, ci, ei)) { strcpy(as, s->c[*ci].ents[*ei].name); return 1; }
    scopy(alt, NAME_MAX_DOS + 1, want);                  // `want` is a bounded 8.3 name
    size_t k = strlen(alt);
    if (!dot) memcpy(alt + k, "._", 3);
    else if (strlen(dot + 1) >= 3) alt[k - 1] = '_';
    else memcpy(alt + k, "_", 2);
    if (strlen(alt) <= NAME_MAX_DOS && find(s, alt, ci, ei)) { strcpy(as, s->c[*ci].ents[*ei].name); return 1; }
    return 0;
}

struct membuf { uint8_t *p; size_t n, cap, max; };

// Keeps what fits under `max` and says stop (1) past it: a header peek takes the start of a cluster.
static int mem_put(void *ctx, const uint8_t *d, size_t n) {
    struct membuf *m = ctx;
    int full = m->n + n > m->max;
    if (full) n = m->max - m->n;
    if (m->n + n > m->cap) {
        size_t nc = m->cap ? m->cap * 2 : 65536;
        while (nc < m->n + n) nc *= 2;
        uint8_t *q = realloc(m->p, nc);
        if (!q) return 1;
        m->p = q;
        m->cap = nc;
    }
    memcpy(m->p + m->n, d, n);
    m->n += n;
    return full;
}

// A disk file's bytes into a fresh buffer (the caller frees it): all of it,
// refused past `max` -- or, `partial`, its first `max` bytes.
static uint8_t *disk_bytes(const struct uwinsetup *s, int ci, int ei, size_t max, size_t *n, int partial) {
    const struct container *c = &s->c[ci];
    const struct entry *e = &c->ents[ei];
    if (!c->is_img) {
        char full[1024];
        if (join(full, sizeof full, c->path, e->name)) return 0;
        return slurp(full, max, n, partial);
    }
    struct membuf m = { 0, 0, 0, max };
    int r = ufatimg_read(&c->fat, &e->fat, mem_put, &m);
    if (r && !(partial && r == -EIO && m.n == max)) { free(m.p); return 0; }
    *n = m.n;
    return m.p ? m.p : calloc(1, 1);
}

// --- SETUP.INF -----------------------------------------------------------------------------------

static void trim(char **p) {
    char *s = *p;
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    *p = s;
}

static int parse_inf(struct uwinsetup *s) {
    char *p = s->inf;
    struct section *cur = 0;
    while (*p) {
        char *line = p;
        char *nl = strchr(p, '\n');
        if (nl) { *nl = 0; p = nl + 1; }
        else p += strlen(p);
        // A ';' outside quotes ends the line ("1:setup.exe     ; dossetup program").
        int q = 0;
        for (char *c = line; *c; c++) {
            if (*c == '"') q = !q;
            else if (*c == ';' && !q) { *c = 0; break; }
        }
        trim(&line);
        if (!*line) continue;
        if (*line == '[') {
            char *end = strchr(line, ']');
            if (!end) continue;
            *end = 0;
            if (grow((void **)&s->secs, &s->capsecs, s->nsecs + 1, sizeof *s->secs)) return -ENOMEM;
            cur = &s->secs[s->nsecs++];
            char *name = line + 1;
            trim(&name);
            size_t k;
            for (k = 0; name[k] && k < sizeof cur->name - 1; k++) cur->name[k] = low(name[k]);
            cur->name[k] = 0;
            cur->first = s->nlines;
            cur->count = 0;
            continue;
        }
        if (!cur) continue;
        if (grow((void **)&s->lines, &s->caplines, s->nlines + 1, sizeof *s->lines)) return -ENOMEM;
        s->lines[s->nlines++] = line;
        cur->count++;
    }
    return 0;
}

static const struct section *section(const struct uwinsetup *s, const char *name) {
    for (int i = 0; i < s->nsecs; i++)
        if (same(s->secs[i].name, name)) return &s->secs[i];
    return 0;
}

// `key = value`: the key (lower case) into `key`, and the value; NULL when the
// line is a plain list entry (a ':' ',' or '"' before any '=').
static const char *keyed(const char *line, char *key, size_t cap) {
    const char *eq = strchr(line, '=');
    if (!eq) return 0;
    for (const char *c = line; c < eq; c++)
        if (*c == ':' || *c == ',' || *c == '"') return 0;
    size_t k = 0;
    for (const char *c = line; c < eq && k < cap - 1; c++)
        if (*c != ' ' && *c != '\t') key[k++] = low(*c);
    key[k] = 0;
    eq++;
    while (*eq == ' ' || *eq == '\t') eq++;
    return eq;
}

// Comma-separated fields, quotes kept out, each trimmed. The count.
static int fields(const char *v, char out[FIELDS][FIELD]) {
    int n = 0;
    size_t k = 0;
    int q = 0;
    memset(out, 0, (size_t)FIELDS * FIELD);
    for (const char *c = v;; c++) {
        if (*c == '"') { q = !q; continue; }
        if ((*c == ',' && !q) || !*c) {
            char *f = out[n];
            while (k && (f[k - 1] == ' ' || f[k - 1] == '\t')) f[--k] = 0;
            char *t = f;
            while (*t == ' ' || *t == '\t') t++;
            memmove(f, t, strlen(t) + 1);
            n++;
            k = 0;
            if (!*c || n == FIELDS) break;
            continue;
        }
        if (k < FIELD - 1) out[n][k++] = *c;
    }
    return n;
}

// "6:WINVER" -> its disk and name; 0 when the field is no file ("x:*vddvga", a description).
static int file_ref(const char *f, int *disk, char *name) {
    if (!(*f >= '0' && *f <= '9')) return 0;
    int d = 0;
    while (*f >= '0' && *f <= '9') d = d * 10 + (*f++ - '0');
    if (*f++ != ':' || !*f) return 0;
    size_t k = 0;
    for (; *f && *f != ' ' && *f != '\t'; f++) {
        if (k >= NAME_MAX_DOS || *f == '/' || *f == '\\') return 0;
        name[k++] = up(*f);
    }
    name[k] = 0;
    *disk = d;
    return k > 0;
}

static int plan_add(struct uwinsetup *s, const char *name, int disk, int dest, int kind) {
    for (int i = 0; i < s->nitems; i++)
        if (same(s->items[i].want, name)) return 0;     // the first section to name a file decides it
    if (grow((void **)&s->items, &s->capitems, s->nitems + 1, sizeof *s->items)) return -ENOMEM;
    struct uws_item *it = &s->items[s->nitems++];
    memset(it, 0, sizeof *it);
    memcpy(it->want, name, strlen(name) + 1);
    memcpy(it->name, name, strlen(name) + 1);
    it->disk = disk;
    it->dest = (uint8_t)dest;
    it->kind = (uint8_t)kind;
    return 0;
}

// Each plain entry's first field: "2:lzexpand.dll", "6:SND.CPL,,novermsg".
static int plan_section(struct uwinsetup *s, const char *name, int dest) {
    const struct section *sec = section(s, name);
    if (!sec) return 0;
    char f[FIELDS][FIELD], key[48], file[NAME_MAX_DOS + 1];
    for (int i = 0; i < sec->count; i++) {
        const char *l = s->lines[sec->first + i];
        int d;
        if (keyed(l, key, sizeof key)) continue;
        fields(l, f);
        if (file_ref(f[0], &d, file) && plan_add(s, file, d, dest, UWS_FILE)) return -ENOMEM;
    }
    return 0;
}

// "#net, 0:" / "#pwin386, 0:system": another section's files, to WINDOWS or
// SYSTEM; a keyed line's own files ("pbrush = 6:PBRUSH.DLL") go to `dest`.
static int plan_refs(struct uwinsetup *s, const char *name, int dest) {
    const struct section *sec = section(s, name);
    if (!sec) return 0;
    char f[FIELDS][FIELD], key[48], file[NAME_MAX_DOS + 1];
    for (int i = 0; i < sec->count; i++) {
        const char *l = s->lines[sec->first + i];
        const char *v = keyed(l, key, sizeof key);
        int n = fields(v ? v : l, f);
        for (int k = 0; k < n; k++) {
            int d;
            if (f[k][0] == '#') {
                const char *ref = f[k] + 1;
                int to = dest;
                if (k + 1 < n && f[k + 1][0] == '0' && f[k + 1][1] == ':') {
                    to = ends_with(f[k + 1], "system") ? UWS_SYSTEM : UWS_WINDOWS;
                    k++;
                }
                if (plan_section(s, ref, to)) return -ENOMEM;
            } else if (v && file_ref(f[k], &d, file) && plan_add(s, file, d, dest, UWS_FILE)) {
                return -ENOMEM;
            }
        }
    }
    return 0;
}

// The value of `key` in section `name`, or NULL.
static const char *value_of(const struct uwinsetup *s, const char *name, const char *key) {
    const struct section *sec = section(s, name);
    char k[48];
    if (!sec) return 0;
    for (int i = 0; i < sec->count; i++) {
        const char *v = keyed(s->lines[sec->first + i], k, sizeof k);
        if (v && same(k, key)) return v;
    }
    return 0;
}

// Every file a keyed value names, to `dest`.
static int plan_value(struct uwinsetup *s, const char *sec, const char *key, int dest) {
    const char *v = value_of(s, sec, key);
    if (!v) return 0;
    char f[FIELDS][FIELD], file[NAME_MAX_DOS + 1];
    int n = fields(v, f), d;
    for (int k = 0; k < n; k++)
        if (file_ref(f[k], &d, file) && plan_add(s, file, d, dest, UWS_FILE)) return -ENOMEM;
    return 0;
}

static void data_value(const struct uwinsetup *s, const char *key, char *out, size_t cap, const char *dflt) {
    char f[FIELDS][FIELD];
    const char *v = value_of(s, "data", key);
    if (v) fields(v, f);
    scopy(out, cap, v && f[0][0] ? f[0] : dflt);
}

static int nospace_eq(const char *a, const char *b);

static int ini_add(struct uwinsetup *s, const char *file, const char *sec, const char *key, const char *val) {
    if (grow((void **)&s->ini, &s->capini, s->nini + 1, sizeof *s->ini)) return -ENOMEM;
    struct inikey *k = &s->ini[s->nini++];
    scopy(k->file, sizeof k->file, file);
    scopy(k->sec, sizeof k->sec, sec);
    scopy(k->key, sizeof k->key, key);
    scopy(k->val, sizeof k->val, val);
    return 0;
}

// A driver's file as SYSTEM.INI names it: lower case, "MOUSE.DRV" -> "mouse.drv".
static void lower_into(char *out, size_t cap, const char *in) {
    size_t i = 0;
    for (; in[i] && i + 1 < cap; i++) out[i] = low(in[i]);
    out[i] = 0;
}

// `key = 2:file.drv, "Description", ...` in `sec`: the file (lower case) and the description;
// 0 when there is no such line.
static int choice(const struct uwinsetup *s, const char *sec, const char *key, char *file, size_t fcap,
                  char *desc, size_t dcap) {
    const char *v = value_of(s, sec, key);
    if (!v) return 0;
    char f[FIELDS][FIELD], name[NAME_MAX_DOS + 1];
    int n = fields(v, f), d;
    file[0] = desc[0] = 0;
    if (n > 0 && file_ref(f[0], &d, name)) lower_into(file, fcap, name);
    if (n > 1) scopy(desc, dcap, f[1]);
    return 1;
}

// SYSTEM.INI's [boot], [boot.description] and [keyboard] for the machine w16 is, and WIN.INI's
// [fonts] -- what Setup wrote beside the files (Control Panel's Fonts reads [fonts]). [386Enh] is
// left out: it describes DOS virtual machines, and w16 runs none.
static int plan_ini(struct uwinsetup *s) {
    char file[NAME_MAX_DOS + 1], desc[96], f[FIELDS][FIELD], name[NAME_MAX_DOS + 1];
    int r = 0, d;
    r |= ini_add(s, "SYSTEM.INI", "boot", "shell", "progman.exe");
    const char *mach = value_of(s, "machine", "ibm_compatible");
    if (mach && fields(mach, f) > 0) r |= ini_add(s, "SYSTEM.INI", "boot.description", "system.drv", f[0]);
    static const struct { const char *sec, *key, *boot; } DRV[] = {
        { "system", "system", "system.drv" }, { "system", "sound", "sound.drv" }, { "system", "comm", "comm.drv" },
        { "keyboard.drivers", "kbd", "keyboard.drv" },
    };
    for (size_t i = 0; i < sizeof DRV / sizeof DRV[0]; i++)
        if (choice(s, DRV[i].sec, DRV[i].key, file, sizeof file, desc, sizeof desc))
            r |= ini_add(s, "SYSTEM.INI", "boot", DRV[i].boot, file);
    if (choice(s, "pointing.device", "ps2mouse", file, sizeof file, desc, sizeof desc)) {
        r |= ini_add(s, "SYSTEM.INI", "boot", "mouse.drv", file);
        r |= ini_add(s, "SYSTEM.INI", "boot.description", "mouse.drv", desc);
    }
    const char *vga = value_of(s, "display", "vga");
    if (vga) {
        int n = fields(vga, f);
        if (n > 0 && file_ref(f[0], &d, name)) { lower_into(file, sizeof file, name); r |= ini_add(s, "SYSTEM.INI", "boot", "display.drv", file); }
        if (n > 1) r |= ini_add(s, "SYSTEM.INI", "boot.description", "display.drv", f[1]);
        if (n > 2) r |= ini_add(s, "SYSTEM.INI", "boot.description", "aspect", f[2]);
        for (int k = 0; k < n; k++)
            if (file_ref(f[k], &d, name) && ends_with(name, ".3GR")) {
                lower_into(file, sizeof file, name);
                r |= ini_add(s, "SYSTEM.INI", "boot", "386grabber", file);
            }
    }
    static const struct { const char *sec, *boot; } FON[] = {
        { "sysfonts", "fonts.fon" }, { "fixedfonts", "fixedfon.fon" }, { "oemfonts", "oemfonts.fon" },
    };
    for (size_t i = 0; i < 3; i++) {
        const struct section *fs = section(s, FON[i].sec);
        for (int k = 0; fs && k < fs->count; k++) {
            int n = fields(s->lines[fs->first + k], f);
            if (n > 2 && nospace_eq(f[2], s->res) && file_ref(f[0], &d, name)) {
                lower_into(file, sizeof file, name);
                r |= ini_add(s, "SYSTEM.INI", "boot", FON[i].boot, file);
                break;
            }
        }
    }
    // The language and the keyboard layout: "" for US English, which needs neither file.
    char lang_file[NAME_MAX_DOS + 1] = "";
    if (choice(s, "language", s->lang, lang_file, sizeof lang_file, desc, sizeof desc)) {
        r |= ini_add(s, "SYSTEM.INI", "boot", "language.dll", lang_file);
        r |= ini_add(s, "SYSTEM.INI", "boot.description", "language.dll", desc);
    }
    const char *kt = value_of(s, "keyboard.types", "t4s0enha");
    if (kt && fields(kt, f) > 0) r |= ini_add(s, "SYSTEM.INI", "boot.description", "keyboard.typ", f[0]);
    r |= ini_add(s, "SYSTEM.INI", "keyboard", "type", "4");
    char kbd_dll[NAME_MAX_DOS + 1] = "";
    if (!same(s->keydll, "usadll")) choice(s, "keyboard.tables", s->keydll, kbd_dll, sizeof kbd_dll, desc, sizeof desc);
    r |= ini_add(s, "SYSTEM.INI", "keyboard", "keyboard.dll", kbd_dll);
    const char *cp = value_of(s, "codepages", s->xlat);
    if (cp) {
        int n = fields(cp, f);
        r |= ini_add(s, "SYSTEM.INI", "boot.description", "codepage", s->xlat);
        if (n > 3) r |= ini_add(s, "SYSTEM.INI", "boot.description", "woafont.fon", f[3]);
    }
    // WIN.INI's [fonts]: each raster font for VGA's resolution or a plotter, each TrueType face.
    const struct section *fs = section(s, "fonts");
    for (int k = 0; fs && k < fs->count; k++) {
        int n = fields(s->lines[fs->first + k], f);
        if (n > 2 && (nospace_eq(f[2], s->res) || same(f[2], "CONTINUOUSSCALING")) && file_ref(f[0], &d, name))
            r |= ini_add(s, "WIN.INI", "fonts", f[1], name);
    }
    const struct section *tt = section(s, "ttfonts");
    for (int k = 0; tt && k < tt->count; k++) {
        int n = fields(s->lines[tt->first + k], f);
        if (n > 1 && file_ref(f[0], &d, name)) r |= ini_add(s, "WIN.INI", "fonts", f[1], name);
    }
    return r ? -ENOMEM : 0;
}

static int nospace_eq(const char *a, const char *b) {
    for (;;) {
        while (*a == ' ') a++;
        while (*b == ' ') b++;
        if (up(*a) != up(*b)) return 0;
        if (!*a) return 1;
        a++;
        b++;
    }
}

static int plan(struct uwinsetup *s) {
    char f[FIELDS][FIELD], file[NAME_MAX_DOS + 1];
    int d, r = 0;
    char raw[sizeof s->title / 2];
    data_value(s, "welcome", raw, sizeof raw, "Microsoft Windows");
    // An INF is Windows-1252 ("Microsoft Työryhmä-Windows 3.11"); the title is shown as UTF-8.
    size_t o = 0;
    for (const unsigned char *c = (const unsigned char *)raw; *c && o + 3 < sizeof s->title; c++) {
        if (*c < 0x80) s->title[o++] = (char)*c;
        else if (*c >= 0xA0) {                           // Latin-1's half of 1252
            s->title[o++] = (char)(0xC0 | *c >> 6);
            s->title[o++] = (char)(0x80 | (*c & 0x3F));
        } else s->title[o++] = '?';
    }
    s->title[o] = 0;
    data_value(s, "deflang", s->lang, sizeof s->lang, "enu");
    data_value(s, "defxlat", s->xlat, sizeof s->xlat, "437");
    data_value(s, "defkeydll", s->keydll, sizeof s->keydll, "usadll");
    char defdir[64];
    data_value(s, "defdir", defdir, sizeof defdir, "C:\\WINDOWS");
    // "C:\WINDOWS" -> "WINDOWS": one 8.3 folder under drive C:, nothing that climbs out of it.
    const char *wd = strrchr(defdir, '\\');
    wd = wd ? wd + 1 : defdir;
    int ok = *wd && strlen(wd) <= 8;
    for (const char *c = wd; *c && ok; c++) ok = (up(*c) >= 'A' && up(*c) <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_';
    scopy(s->windir, sizeof s->windir, ok ? wd : "WINDOWS");
    for (char *c = s->windir; *c; c++) *c = up(*c);

    // What DOS Setup copies before Windows runs, then Windows Setup's own list.
    static const struct { const char *sec; int dest; } BASE[] = {
        { "windows", UWS_WINDOWS }, { "windows.system", UWS_SYSTEM }, { "windows.system.386", UWS_SYSTEM },
        { "win.devices.win386", UWS_WINDOWS },
        { "win.apps", UWS_WINDOWS }, { "win.games", UWS_WINDOWS }, { "win.scrs", UWS_WINDOWS },
        { "win.bmps", UWS_WINDOWS }, { "win.readme", UWS_WINDOWS },
    };
    for (size_t i = 0; i < sizeof BASE / sizeof BASE[0] && !r; i++) r = plan_section(s, BASE[i].sec, BASE[i].dest);
    const struct section *sec = section(s, "winexec");     // "Krnl386 = 1:krnl386.exe"
    for (int i = 0; sec && i < sec->count && !r; i++) {
        char key[48];
        const char *v = keyed(s->lines[sec->first + i], key, sizeof key);
        if (v && file_ref(v, &d, file)) r = plan_add(s, file, d, UWS_SYSTEM, UWS_FILE);
    }
    if (!r) r = section(s, "win.copy.win386") ? plan_refs(s, "win.copy.win386", UWS_WINDOWS)
                                                : plan_refs(s, "win.copy", UWS_WINDOWS);
    if (!r) r = plan_refs(s, "win.dependents", UWS_WINDOWS);

    // The machine w16 is: VGA, its fonts and 386 grabber, a PS/2 mouse, the standard drivers.
    const char *vga = value_of(s, "display", "vga");
    if (vga && !r) {
        int n = fields(vga, f);
        scopy(s->res, sizeof s->res, n > 2 ? f[2] : "100,96,96");
        for (int k = 0; k < n && !r; k++) {
            if (!file_ref(f[k], &d, file)) continue;
            r = plan_add(s, file, d, UWS_SYSTEM, UWS_FILE);
            // The grabber's own section lists the DOS-window fonts, a column per code page.
            if (!r && ends_with(file, ".3GR")) {
                const struct section *g = section(s, file);
                int col = !strcmp(s->xlat, "437") ? 0 : 1;
                char gf[FIELDS][FIELD], gfile[NAME_MAX_DOS + 1];
                for (int i = 0; g && i < g->count && !r; i++) {
                    int gn = fields(s->lines[g->first + i], gf), gd;
                    if (gn > col && file_ref(gf[col], &gd, gfile)) r = plan_add(s, gfile, gd, UWS_SYSTEM, UWS_FILE);
                }
            }
        }
    }
    // Raster fonts for VGA's resolution, the plotter fonts, every TrueType face.
    static const char *const FONTS[] = { "sysfonts", "fixedfonts", "oemfonts", "fonts" };
    for (size_t i = 0; i < 4 && !r; i++) {
        const struct section *fs = section(s, FONTS[i]);
        for (int k = 0; fs && k < fs->count && !r; k++) {
            int n = fields(s->lines[fs->first + k], f);
            if (n > 2 && (nospace_eq(f[2], s->res) || same(f[2], "CONTINUOUSSCALING")) && file_ref(f[0], &d, file))
                r = plan_add(s, file, d, UWS_SYSTEM, UWS_FILE);
        }
    }
    const struct section *tt = section(s, "ttfonts");
    for (int k = 0; tt && k < tt->count && !r; k++) {
        int n = fields(s->lines[tt->first + k], f);
        for (int j = 0; j < n && !r; j++)
            if (file_ref(f[j], &d, file)) r = plan_add(s, file, d, UWS_SYSTEM, UWS_FILE);
    }
    if (!r) r = plan_value(s, "keyboard.drivers", "kbd", UWS_SYSTEM);
    if (!r) r = plan_value(s, "pointing.device", "ps2mouse", UWS_SYSTEM);
    if (!r) r = plan_value(s, "system", "system", UWS_SYSTEM);
    if (!r) r = plan_value(s, "system", "sound", UWS_SYSTEM);
    if (!r) r = plan_value(s, "system", "comm", UWS_SYSTEM);
    if (!r) r = plan_value(s, "codepages", s->xlat, UWS_SYSTEM);
    if (!r) r = plan_value(s, "language", s->lang, UWS_SYSTEM);
    // US English is KEYBOARD.DRV's own: Setup copies no KBDUS.DLL (measured).
    if (!r && !same(s->keydll, "usadll")) r = plan_value(s, "keyboard.tables", s->keydll, UWS_SYSTEM);
    // Setup keeps itself for adding a driver later; a set that lacks it is no less a set.
    if (!r) r = plan_add(s, "SETUP.INF", 1, UWS_SYSTEM, UWS_OPTIONAL);
    if (!r) r = plan_add(s, "SETUP.EXE", 1, UWS_WINDOWS, UWS_OPTIONAL);
    if (r) return r;

    // The templates become the INI files; SOUND.DRV is the chosen sound driver's copy.
    static const char *const SRC[][2] = { { "WIN.SRC", "WIN.INI" }, { "SYSTEM.SRC", "SYSTEM.INI" },
                                          { "CONTROL.SRC", "CONTROL.INI" } };
    for (int i = 0; i < s->nitems; i++) {
        struct uws_item *it = &s->items[i];
        for (size_t k = 0; k < 3; k++)
            if (same(it->want, SRC[k][0])) {
                scopy(it->name, sizeof it->name, SRC[k][1]);
                it->kind = UWS_TEMPLATE;
            }
    }
    const char *snd = value_of(s, "system", "sound");
    if (snd && file_ref(snd, &d, file) && !same(file, "SOUND.DRV")) {
        if ((r = plan_add(s, "SOUND.DRV", d, UWS_SYSTEM, UWS_FILE))) return r;
        scopy(s->items[s->nitems - 1].want, sizeof s->items[0].want, file);
    }

    if ((r = plan_ini(s))) return r;

    // Where each is on these disks, and how big it will be.
    for (int i = 0; i < s->nitems; i++) {
        struct uws_item *it = &s->items[i];
        int ci, ei;
        if (!find_disk(s, it->want, &ci, &ei, it->disk_name)) {
            it->disk_name[0] = 0;
            if (it->kind != UWS_OPTIONAL) s->missing++;
            continue;
        }
        it->size = s->c[ci].ents[ei].size;
        size_t n = 0;
        uint8_t *head = disk_bytes(s, ci, ei, 512, &n, 1);
        struct umsx_info info;
        if (head && umsx_probe(head, n, &info) == 0 && info.has_length && info.kind != UMSX_PLAIN)
            it->size = info.length;
        free(head);
        s->bytes += it->size;
    }
    return 0;
}

struct uwinsetup *uwinsetup_open(const char *src, char *err, size_t errcap) {
    struct uwinsetup *s = calloc(1, sizeof *s);
    if (!s) { say(err, errcap, "out of memory%s%s", "", ""); return 0; }
    struct stat st;
    int r;
    if (stat(src, &st) != 0) {
        say(err, errcap, "%s: %s", src, strerror(errno));
        uwinsetup_close(s);
        return 0;
    }
    if (S_ISDIR(st.st_mode)) {
        r = read_dir(s, src);
        if (!r) r = read_folder_of_disks(s, src, 0);
    } else {
        char dir[1024];
        scopy(dir, sizeof dir, src);
        char *slash = strrchr(dir, '/');
        if (slash) *slash = 0;
        else scopy(dir, sizeof dir, ".");
        r = read_folder_of_disks(s, slash && !dir[0] ? "/" : dir, 1);
    }
    int ci, ei;
    if (r || !find(s, "SETUP.INF", &ci, &ei)) {
        say(err, errcap, r ? "%s: cannot read the disks: %s" : "%s: no SETUP.INF on these disks%s",
            src, r ? strerror(-r) : "");
        uwinsetup_close(s);
        return 0;
    }
    size_t n = 0;
    uint8_t *inf = disk_bytes(s, ci, ei, INF_MAX, &n, 0);
    char *text = inf ? realloc(inf, n + 1) : 0;
    if (!text) {
        free(inf);
        say(err, errcap, "%s: SETUP.INF cannot be read%s", src, "");
        uwinsetup_close(s);
        return 0;
    }
    text[n] = 0;
    for (size_t i = 0; i < n; i++)
        if (!text[i]) text[i] = ' ';                     // a stray NUL must not end the file early
    s->inf = text;
    if (parse_inf(s) || plan(s)) {
        say(err, errcap, "%s: out of memory reading SETUP.INF%s", src, "");
        uwinsetup_close(s);
        return 0;
    }
    if (!section(s, "disks") || !s->nitems) {
        say(err, errcap, "%s: SETUP.INF is not a Windows 3.x Setup's%s", src, "");
        uwinsetup_close(s);
        return 0;
    }
    return s;
}

void uwinsetup_close(struct uwinsetup *s) {
    if (!s) return;
    for (int i = 0; i < s->nc; i++) {
        free(s->c[i].path);
        free(s->c[i].img);
        free(s->c[i].ents);
    }
    free(s->c);
    free(s->inf);
    free(s->lines);
    free(s->secs);
    free(s->items);
    free(s->ini);
    free(s);
}

const char *uwinsetup_title(const struct uwinsetup *s) { return s->title; }
const char *uwinsetup_lang(const struct uwinsetup *s) { return s->lang; }
const char *uwinsetup_windir(const struct uwinsetup *s) { return s->windir; }
int uwinsetup_containers(const struct uwinsetup *s) {
    int n = 0;
    for (int i = 0; i < s->nc; i++) n += s->c[i].n > 0;   // the folder holding the images is no disk
    return n;
}
int uwinsetup_count(const struct uwinsetup *s) { return s->nitems; }
const struct uws_item *uwinsetup_item(const struct uwinsetup *s, int i) {
    return i >= 0 && i < s->nitems ? &s->items[i] : 0;
}
int uwinsetup_missing(const struct uwinsetup *s) { return s->missing; }
uint64_t uwinsetup_bytes(const struct uwinsetup *s) { return s->bytes; }

// --- installing ----------------------------------------------------------------------------------

// A folder that is there is fine however mkdir says so: under w16's unveil,
// mkdir("/home") answers EACCES rather than EEXIST.
static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int mkdir_p(const char *path) {
    char buf[1024];
    if (strlen(path) >= sizeof buf) return -ENAMETOOLONG;
    scopy(buf, sizeof buf, path);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (!is_dir(buf) && mkdir(buf, 0755) != 0 && errno != EEXIST) return -errno;
        *p = '/';
    }
    if (!is_dir(buf) && mkdir(buf, 0755) != 0 && errno != EEXIST) return -errno;
    return 0;
}

static int exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

#define FILE_MAX (16u << 20)   // no 3.x setup file expands past a few MB

static int install_one(struct uwinsetup *s, struct uws_item *it, const char *dir, char *err, size_t errcap) {
    int ci, ei;
    char as[NAME_MAX_DOS + 1];
    if (!find_disk(s, it->want, &ci, &ei, as)) {
        say(err, errcap, "%s is on none of these disks%s", it->want, "");
        return -ENOENT;
    }
    size_t n = 0;
    uint8_t *raw = disk_bytes(s, ci, ei, FILE_MAX, &n, 0);
    if (!raw) {
        say(err, errcap, "%s cannot be read from %s", as, s->c[ci].path);
        return -EIO;
    }
    struct membuf m = { 0, 0, 0, FILE_MAX };
    uint32_t written;
    int r = umsx_expand(raw, n, &s->scratch, mem_put, &m, &written);
    free(raw);
    if (r) {
        free(m.p);
        say(err, errcap, "%s: %s", as, r == -ENOTSUP ? "compressed in a way this does not expand"
                                                    : "damaged -- it does not expand");
        return r;
    }
    // "6:WINVER": Setup installs a program named without an extension as an .EXE (measured).
    if (!strchr(it->name, '.') && m.n >= 2 && m.p[0] == 'M' && m.p[1] == 'Z' && strlen(it->name) <= 8)
        strcat(it->name, ".EXE");
    char path[1024], part[1100];
    if (join(path, sizeof path, dir, it->name) || join(part, sizeof part, dir, it->name)) {
        free(m.p);
        say(err, errcap, "%s/%s: the path is too long", dir, it->name);
        return -ENAMETOOLONG;
    }
    strcat(part, ".part");                               // part[] is sized past path[] for it
    FILE *f = fopen(part, "wb");
    if (!f || (m.n && fwrite(m.p, 1, m.n, f) != m.n) || fclose(f) != 0) {
        r = errno ? -errno : -EIO;
        if (f) unlink(part);
        free(m.p);
        say(err, errcap, "%s: %s", path, strerror(-r));
        return r;
    }
    free(m.p);
    if (rename(part, path) != 0) {
        unlink(path);                                    // a filesystem that will not rename over a file
        if (rename(part, path) != 0) {
            r = -errno;
            unlink(part);
            say(err, errcap, "%s: %s", path, strerror(-r));
            return r;
        }
    }
    return 0;
}

// A key is there when a line in the section starts with it and '=' -- no case, spaces around it:
// 1, or 2 when its value is empty (a template's placeholder, SYSTEM.SRC's "display.drv="), with
// *eq where the line's end is, to fill it.
static int has_key(const char *text, size_t a, size_t b, const char *key, size_t *eq) {
    size_t k = strlen(key);
    for (size_t i = a; i < b;) {
        size_t e = i;
        while (e < b && text[e] != '\n') e++;
        size_t j = i;
        while (j < e && (text[j] == ' ' || text[j] == '\t')) j++;
        if (e - j > k) {
            size_t m = 0;
            while (m < k && up(text[j + m]) == up(key[m])) m++;
            size_t t = j + m;
            while (m == k && t < e && (text[t] == ' ' || text[t] == '\t')) t++;
            if (m == k && t < e && text[t] == '=') {
                size_t v = t + 1;
                while (v < e && (text[v] == ' ' || text[v] == '\t' || text[v] == '\r')) v++;
                *eq = t + 1;
                return v == e ? 2 : 1;
            }
        }
        i = e + 1;
    }
    return 0;
}

// Writes `file`'s keys into <dir>/<file>: each absent one at its section's end, a new section at
// the file's end, in the file's own line endings; written through a .part and a rename.
static int apply_ini(struct uwinsetup *s, const char *dir, const char *file, char *err, size_t errcap) {
    char path[1024], part[1100];
    if (join(path, sizeof path, dir, file) || join(part, sizeof part, dir, file)) return -ENAMETOOLONG;
    strcat(part, ".part");
    size_t n = 0;
    uint8_t *old = slurp(path, INF_MAX, &n, 0);
    char *text = malloc(n + 1);
    if (!text) { free(old); return -ENOMEM; }
    if (old) memcpy(text, old, n);
    text[n] = 0;
    free(old);
    const char *eol = strstr(text, "\r\n") || !n ? "\r\n" : "\n";
    int changed = 0;
    for (int i = 0; i < s->nini; i++) {
        const struct inikey *k = &s->ini[i];
        if (!same(k->file, file)) continue;
        // The section: its header's line, and where it ends (the next header, or the end).
        size_t len = strlen(text), a = len, b = len, at = 0;
        int found = 0;
        while (at < len) {
            size_t e = at;
            while (e < len && text[e] != '\n') e++;
            size_t j = at;
            while (j < e && (text[j] == ' ' || text[j] == '\t')) j++;
            if (j < e && text[j] == '[') {
                if (found) { b = at; break; }
                size_t q = j + 1, m = 0, sl = strlen(k->sec);
                while (m < sl && q + m < e && up(text[q + m]) == up(k->sec[m])) m++;
                if (m == sl && q + m < e && text[q + m] == ']') { found = 1; a = e < len ? e + 1 : e; }
            }
            at = e + 1;
        }
        size_t eq = 0;
        int there = found ? has_key(text, a, b, k->key, &eq) : 0;
        if (there == 1 || (there == 2 && !k->val[0])) continue;
        if (there == 2) {                                  // the placeholder's blanks become the value
            size_t blank = 0;
            while (eq + blank < len && (text[eq + blank] == ' ' || text[eq + blank] == '\t')) blank++;
            memmove(text + eq, text + eq + blank, len - eq - blank + 1);
            len -= blank;
            size_t vl = strlen(k->val);
            char *t = realloc(text, len + vl + 1);
            if (!t) { free(text); return -ENOMEM; }
            text = t;
            memmove(text + eq + vl, text + eq, len - eq + 1);
            memcpy(text + eq, k->val, vl);
            changed = 1;
            continue;
        }
        // The key goes after the section's last line that is not blank, so the blank line
        // between two sections stays between them.
        if (found) {
            size_t last = a;
            for (size_t i = a; i < b;) {
                size_t e = i;
                while (e < b && text[e] != '\n') e++;
                for (size_t q = i; q < e; q++)
                    if (text[q] != ' ' && text[q] != '\t' && text[q] != '\r') { last = e < b ? e + 1 : e; break; }
                i = e + 1;
            }
            b = last;
        }
        char line[256];
        int ln = found ? snprintf(line, sizeof line, "%s=%s%s", k->key, k->val, eol)
                       : snprintf(line, sizeof line, "%s[%s]%s%s=%s%s", len && text[len - 1] != '\n' ? eol : "",
                                  k->sec, eol, k->key, k->val, eol);
        if (ln < 0 || (size_t)ln >= sizeof line) continue;
        size_t pos = found ? b : len;
        if (found && pos > 0 && text[pos - 1] != '\n') {      // a section that ends the file with no newline
            memmove(line + strlen(eol), line, (size_t)ln + 1);
            memcpy(line, eol, strlen(eol));
            ln += (int)strlen(eol);
        }
        char *t = realloc(text, len + (size_t)ln + 1);
        if (!t) { free(text); return -ENOMEM; }
        text = t;
        memmove(text + pos + ln, text + pos, len - pos + 1);
        memcpy(text + pos, line, (size_t)ln);
        changed = 1;
    }
    int r = 0;
    if (changed) {
        FILE *f = fopen(part, "wb");
        size_t len = strlen(text);
        if (!f || fwrite(text, 1, len, f) != len || fclose(f) != 0) r = errno ? -errno : -EIO;
        else if (rename(part, path) != 0 && (unlink(path), rename(part, path) != 0)) r = -errno;
        if (r) { unlink(part); say(err, errcap, "%s: %s", path, strerror(-r)); }
    }
    free(text);
    return r;
}

int uwinsetup_run(struct uwinsetup *s, const char *drive_c, int flags, uws_progress progress, void *ctx,
                  char *err, size_t errcap) {
    char win[1024], sys[1024];
    int r = join(win, sizeof win, drive_c, s->windir);
    if (!r) r = join(sys, sizeof sys, win, "SYSTEM");
    if (!r) r = mkdir_p(sys);
    if (r) {
        say(err, errcap, "%s: %s", sys, strerror(-r));
        return r;
    }
    for (int i = 0; i < s->nitems; i++) {
        struct uws_item *it = &s->items[i];
        const char *dir = it->dest == UWS_SYSTEM ? sys : win;
        char path[1024];
        if (join(path, sizeof path, dir, it->name)) {
            say(err, errcap, "%s/%s: the path is too long", dir, it->name);
            return -ENAMETOOLONG;
        }
        const char *what = "copy";
        if (!it->disk_name[0] && it->kind == UWS_OPTIONAL) what = "absent";
        else if (it->kind == UWS_TEMPLATE && (flags & UWS_KEEP_INI) && exists(path)) what = "kept";
        if (progress && progress(ctx, i, s->nitems, it, what)) {
            say(err, errcap, "stopped%s%s", "", "");
            return -EINTR;
        }
        if (strcmp(what, "copy")) continue;
        if ((r = install_one(s, it, dir, err, errcap))) return r;
    }
    if ((r = apply_ini(s, win, "WIN.INI", err, errcap))) return r;
    return apply_ini(s, win, "SYSTEM.INI", err, errcap);
}
