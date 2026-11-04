// grp16.c -- Program Manager's group files, made: the groups Windows 3.1's Setup made through
// Program Manager's DDE when it installed (SETUP.INF's [progman.groups]), and a group for each of
// Retro Launcher's folders. `w16 --install` writes the first, `w16 --groups` the second.
//
// THE FORMAT IS 3.1's "PMCC" .GRP, as Program Manager reads and writes it (the layout Wine's
// progman reads, checked against the files Express Setup made on the reference VM): a 34-byte
// header -- the checksum, the file's size, how the group window shows, its normal rectangle and
// its icon's place, the name's offset, the icon size and the display's bits and planes -- the
// items' offsets, then each item: its place, its icon's index, the icon's three parts' sizes and
// offsets, the description's, the command's and the icon file's; then those strings and the icon
// itself as 3.1 held it (icon16.c's block: a CURSORICONINFO, the AND rows, the XOR rows). THE
// CHECKSUM MAKES THE FILE'S WHOLE 16-BIT WORDS SUM TO 0 -- an odd last byte left out, as Program
// Manager's own check leaves it -- and Program Manager calls a file whose words do not damaged.
#include "w16_internal.h"
#include "common/w16prefs.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include "lib/uwinsetup.h"

#define ICON 32
#define ROW_W 75                         // Program Manager's own spacing of a group's icons
#define ROW_H 72
#define COL_X 21
#define HOST_MAX 512

// ONE WRITER OF PROGMAN.INI AND THE .GRP FILES AT A TIME: Retro Launcher starts `w16 --groups` on
// every change of its folders without waiting, and two at once would both take the next GroupN and
// write the same RLn.GRP.part. A lock file made exclusively beside them; one older than a minute is
// a writer that died, and is taken over.
#define LOCK_DOS "C:\\WINDOWS\\W16GRP.LCK"
static char g_lock[HOST_MAX];

static int lock_groups(void) {
    if (w16_dos_path(LOCK_DOS, g_lock, sizeof g_lock, 1, 0, 0) != 0) return -1;
    for (int tries = 0; tries < 400; tries++) {   // 20 seconds
        int fd = open(g_lock, O_CREAT | O_EXCL | O_WRONLY, 0644);
        if (fd >= 0) { close(fd); return 0; }
        struct stat st;
        if (stat(g_lock, &st) == 0 && time(0) - st.st_mtime > 60) { remove(g_lock); continue; }
        struct timespec ts = { 0, 50000000 };
        nanosleep(&ts, 0);
    }
    return -1;
}

static void unlock_groups(void) { remove(g_lock); }

// PROGMAN.INI's display.drv, as Setup wrote it: SYSTEM.INI's [boot] display.drv. Program Manager
// takes a difference between the two for a new display and converts every icon of every group --
// which, in a group of forty programs, outgrows its 64 KB block ("Extremely Low on Memory").
static void display_driver(void) {
    char have[64], drv[64];
    if (w16_ini_get("PROGMAN.INI", "Settings", "display.drv", have, sizeof have) > 0) return;
    if (w16_ini_get("SYSTEM.INI", "boot", "display.drv", drv, sizeof drv) > 0)
        w16_ini_set("PROGMAN.INI", "Settings", "display.drv", drv);
}

struct grp_item {
    char desc[64], cmd[W16_PATH_MAX], icon_path[W16_PATH_MAX];
    int index;
    uint32_t px[ICON * ICON];            // the icon, ARGB
    int iw, ih;
};

struct buf { uint8_t *b; size_t n, cap; int bad; };

static void put(struct buf *o, const void *p, size_t n) {
    if (o->bad) return;
    if (o->n + n > o->cap) {
        size_t c = o->cap ? o->cap * 2 : 4096;
        while (c < o->n + n) c *= 2;
        uint8_t *g = realloc(o->b, c);
        if (!g) { o->bad = 1; return; }
        o->b = g;
        o->cap = c;
    }
    memcpy(o->b + o->n, p, n);
    o->n += n;
}

static void put16(struct buf *o, int v) {
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    put(o, b, 2);
}

static void set16(struct buf *o, size_t at, int v) {
    if (!o->bad && at + 2 <= o->n) { o->b[at] = (uint8_t)v; o->b[at + 1] = (uint8_t)(v >> 8); }
}

// The group's bytes: `show` 1 open or 2 minimized, `rc` its normal rectangle, the items laid in rows
// `per_row` wide as Program Manager arranges them.
static int grp_bytes(struct buf *o, const char *name, int show, const int16_t rc[4], const struct grp_item *it, int n,
                     int per_row) {
    memset(o, 0, sizeof *o);
    put(o, "PMCC", 4);
    put16(o, 0);                                 // the checksum, last
    put16(o, 0);                                 // cbGroup: where the extensions would start -- the end
    put16(o, show);
    for (int i = 0; i < 4; i++) put16(o, rc[i]);
    put16(o, -1);                                // ptMin: none yet
    put16(o, -1);
    put16(o, 0);                                 // the name's offset
    put16(o, ICON);
    put16(o, ICON);
    uint8_t bp[2] = { (uint8_t)(w16_colors16() ? 1 : 8), (uint8_t)(w16_colors16() ? 4 : 1) };
    put(o, bp, 2);                               // as GetDeviceCaps answers: else Program Manager re-reads them
    put16(o, 0);
    put16(o, n ? n : 1);                         // an empty group has one empty slot, as 3.1's StartUp: none is refused
    size_t table = o->n;
    for (int i = 0; i < (n ? n : 1); i++) put16(o, 0);
    set16(o, 22, (int)o->n);
    put(o, name, strlen(name) + 1);
    for (int i = 0; i < n; i++) {
        size_t at = o->n;
        set16(o, table + 2 * (size_t)i, (int)at);
        uint32_t blen = w16_icon_block_len(it[i].iw, it[i].ih);
        uint8_t *blk = malloc(blen);
        if (!blk) { o->bad = 1; break; }
        w16_icon_block_fill(blk, it[i].px, it[i].iw, it[i].ih);
        unsigned wb = ((unsigned)it[i].iw + 15) / 16 * 2, sa = wb * (unsigned)it[i].ih, sx = sa * 4;
        size_t sd = at + 24, sc = sd + strlen(it[i].desc) + 1, si = sc + strlen(it[i].cmd) + 1;
        size_t ph = si + strlen(it[i].icon_path) + 1, pa = ph + 12, pxor = pa + sa;
        put16(o, COL_X + ROW_W * (i % per_row));
        put16(o, ROW_H * (i / per_row));
        put16(o, it[i].index);
        put16(o, 12);                            // cbHeader: a CURSORICONINFO
        put16(o, (int)sa);
        put16(o, (int)sx);
        put16(o, (int)ph);
        put16(o, (int)pa);
        put16(o, (int)pxor);
        put16(o, (int)sd);
        put16(o, (int)sc);
        put16(o, (int)si);
        put(o, it[i].desc, strlen(it[i].desc) + 1);
        put(o, it[i].cmd, strlen(it[i].cmd) + 1);
        put(o, it[i].icon_path, strlen(it[i].icon_path) + 1);
        put(o, blk, blen);
        free(blk);
    }
    if (o->bad || o->n > 0xFFF0) return -1;
    set16(o, 6, (int)o->n);
    unsigned sum = 0;                            // whole words only: an odd last byte is not summed
    for (size_t i = 0; i + 1 < o->n; i += 2) sum += (unsigned)(o->b[i] | o->b[i + 1] << 8);
    set16(o, 4, (int)(-sum & 0xFFFF));
    return 0;
}

static int write_file(const char *host, const struct buf *o) {
    char tmp[HOST_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.part", host);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    int ok = fwrite(o->b, 1, o->n, f) == o->n;
    ok &= fclose(f) == 0;
    if (!ok || rename(tmp, host) != 0) { remove(tmp); return -1; }
    return 0;
}

static uint8_t *slurp(const char *host, uint32_t *size) {
    FILE *f = fopen(host, "rb");
    if (!f) return 0;
    uint8_t *b = 0;
    long n = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    // One byte more, a NUL: a string an offset in a damaged file points at ends inside the buffer.
    if (n > 0 && n < 16L * 1024 * 1024 && fseek(f, 0, SEEK_SET) == 0 && (b = malloc((size_t)n + 1))) {
        if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = 0; }
        else { b[n] = 0; *size = (uint32_t)n; }
    }
    fclose(f);
    return b;
}

// Icon `index` of the program at host path, as ExtractIcon would give it; a blank window when it has
// none (Program Manager's own default needs its .EXE, which may not be there).
static void item_icon(const char *host, int index, struct grp_item *it) {
    uint32_t size = 0;
    uint8_t *img = slurp(host, &size);
    struct ne_module *m = img ? calloc(1, sizeof *m) : 0;
    int got = 0;
    if (m && ne_parse(img, size, m) == 0) {
        struct ne_resource r;
        for (unsigned i = 0, k = 0; i < m->nres && !got; i++)
            if (ne_resource_at(m, i, &r) == 0 && !r.type_name[0] && r.type == NE_RT_GROUP_ICON && (int)k++ == index)
                got = w16_icon_group(m, &r, it->px, &it->iw, &it->ih) == 0;
    }
    free(m);
    free(img);
    if (got && it->iw <= ICON && it->ih <= ICON) return;
    it->iw = it->ih = ICON;                      // a window: a white box under a navy caption
    for (int y = 0; y < ICON; y++)
        for (int x = 0; x < ICON; x++) {
            int edge = x == 2 || x == ICON - 3 || y == 3 || y == ICON - 4;
            int inside = x > 2 && x < ICON - 3 && y > 3 && y < ICON - 4;
            it->px[y * ICON + x] = edge ? 0xFF000000u : inside ? (y < 9 ? 0xFF000080u : 0xFFFFFFFFu) : 0;
        }
}

// --- the groups Setup made --------------------------------------------------------------

// A DOS name for a group's file: its name's first eight letters and digits, ".GRP" -- MAIN.GRP,
// ACCESSOR.GRP, as Program Manager named them.
static void grp_name(const char *name, char *out, size_t cap) {
    char b[9];
    int k = 0;
    for (const char *c = name; *c && k < 8; c++)
        if ((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9'))
            b[k++] = (char)(*c >= 'a' && *c <= 'z' ? *c - 32 : *c);
    if (!k) b[k++] = 'G';
    b[k] = 0;
    snprintf(out, cap, "%s.GRP", b);
}

// The program a group line names, found where Setup put it (C:\WINDOWS, then its SYSTEM).
static int windows_file(const char *name, char *host, size_t cap, char *dos, size_t dcap) {
    static const char *const dirs[] = { "C:\\WINDOWS\\", "C:\\WINDOWS\\SYSTEM\\" };
    for (unsigned i = 0; i < 2; i++) {
        char d[W16_PATH_MAX];
        snprintf(d, sizeof d, "%s%s", dirs[i], name);
        struct stat st;
        if (w16_dos_path(d, host, cap, 0, dos, dcap) == 0 && stat(host, &st) == 0) return 0;
    }
    return -1;
}

// Whether a [Groups] value names one of the launcher's own RLn.GRP files.
static int launcher_group(const char *v) {
    const char *b = strrchr(v, '\\');
    b = b ? b + 1 : v;
    return !strncasecmp(b, "RL", 2) && b[2] >= '0' && b[2] <= '9' && strcasestr(b, ".GRP") != 0;
}

// PROGMAN.INI's [Groups] already names a group other than the launcher's: the groups are the
// person's, and stay. *last gets the highest GroupN there, which new ones are numbered after.
static int groups_set(int *last) {
    size_t cap = 8192;
    char *names = malloc(cap), v[W16_PATH_MAX];
    int theirs = 0;
    *last = 0;
    size_t len = names ? w16_ini_names("PROGMAN.INI", "Groups", names, cap) : 0;
    for (size_t at = 0; at < len && names[at]; at += strlen(names + at) + 1) {
        if (strncasecmp(names + at, "Group", 5)) continue;
        if (atoi(names + at + 5) > *last) *last = atoi(names + at + 5);
        if (w16_ini_get("PROGMAN.INI", "Groups", names + at, v, sizeof v) > 0 && !launcher_group(v)) theirs = 1;
    }
    free(names);
    return theirs;
}

static int grp_setup(const struct uwinsetup *s, FILE *log) {
    int last;
    if (groups_set(&last)) {
        fprintf(log, "groups kept\n");
        return 0;
    }
    int ng = uwinsetup_groups(s), made = 0, rc = 0, open_one = 0;
    char order[512] = "";
    if (w16_ini_get("PROGMAN.INI", "Settings", "Order", order, sizeof order) <= 0) order[0] = 0;   // the launcher's first
    for (int g = 0; g < ng; g++) {
        char name[64], file[16], dos[W16_PATH_MAX], host[HOST_MAX];
        int open;
        struct uws_group_item *src;
        int n = uwinsetup_group(s, g, name, sizeof name, &open, &src);
        if (n < 0) continue;
        struct grp_item *it = calloc((size_t)(n ? n : 1), sizeof *it);
        int k = 0;
        for (int i = 0; it && i < n; i++) {
            char ph[HOST_MAX], pd[W16_PATH_MAX], assoc[W16_PATH_MAX];
            const char *iconf = src[i].icon[0] ? src[i].icon : src[i].cmd;
            // A document's icon is the program that opens it, as Setup's DDE got it ("README.WRI":
            // write.exe, by WIN.INI's [extensions]).
            const char *dot = strrchr(src[i].cmd, '.');
            if (!src[i].icon[0] && dot && strcasecmp(dot, ".EXE") && strcasecmp(dot, ".COM") && strcasecmp(dot, ".PIF") &&
                w16_ini_get("WIN.INI", "extensions", dot + 1, assoc, sizeof assoc) > 0) {
                char *sp = strchr(assoc, ' ');
                if (sp) *sp = 0;
                iconf = assoc;
            }
            int has = windows_file(src[i].cmd, ph, sizeof ph, pd, sizeof pd) == 0;
            if (!has) continue;                  // not installed (an option these disks lacked)
            snprintf(it[k].desc, sizeof it[k].desc, "%s", src[i].desc);
            snprintf(it[k].cmd, sizeof it[k].cmd, "%s", src[i].cmd);
            it[k].index = src[i].index;
            if (windows_file(iconf, ph, sizeof ph, pd, sizeof pd) == 0) {
                snprintf(it[k].icon_path, sizeof it[k].icon_path, "%s", src[i].icon[0] || iconf == assoc ? iconf : pd);
                item_icon(ph, src[i].index, &it[k]);
            } else {
                snprintf(it[k].icon_path, sizeof it[k].icon_path, "%s", iconf);
                item_icon("", 0, &it[k]);
            }
            k++;
        }
        free(src);
        grp_name(name, file, sizeof file);
        snprintf(dos, sizeof dos, "C:\\WINDOWS\\%s", file);
        int16_t rc4[4] = { (int16_t)(22 * made), (int16_t)(22 * made), (int16_t)(22 * made + 416), (int16_t)(22 * made + 202) };
        struct buf o = { 0 };
        int ok = it && w16_dos_path(dos, host, sizeof host, 1, 0, 0) == 0 &&
                 grp_bytes(&o, name, open ? 1 : 2, rc4, it, k, 5) == 0 && write_file(host, &o) == 0;
        free(it);
        free(o.b);
        if (!ok) { fprintf(log, "group %s NOT written\n", name); rc = -1; continue; }
        made++;
        char key[16], num[8];
        snprintf(key, sizeof key, "Group%d", ++last);
        w16_ini_set("PROGMAN.INI", "Groups", key, dos);
        snprintf(num, sizeof num, " %d", last);
        if (open) open_one = last;
        else if (strlen(order) + strlen(num) < sizeof order) strcat(order, num);
        fprintf(log, "group %s %s %d\n", name, dos, k);
    }
    if (open_one && strlen(order) + 8 < sizeof order) {   // the open one last: on top
        char num[8];
        snprintf(num, sizeof num, " %d", open_one);
        strcat(order, num);
    }
    if (made) w16_ini_set("PROGMAN.INI", "Settings", "Order", order);
    return rc;
}

// --- a group for each of Retro Launcher's folders --------------------------------------------

static int is_exe(const char *n) {
    size_t l = strlen(n);
    return l > 4 && !strcasecmp(n + l - 4, ".exe");
}

static int by_name(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

// RLn.GRP's file is already the group folder n would make: kept, so Program Manager's own changes to
// it (an icon moved, the window's place) survive; anything else, made again.
static int same_group(const char *host, const struct buf *o) {
    uint32_t size = 0;
    uint8_t *b = slurp(host, &size);
    unsigned name = b && size >= 34 ? (unsigned)(b[22] | b[23] << 8) : 0;
    int same = b && size >= 34 && name < size && !strcmp((const char *)b + name, (const char *)o->b + (o->b[22] | o->b[23] << 8));
    if (same) {                                  // the same commands, whatever their places
        unsigned na = b[32] | b[33] << 8, nb = o->b[32] | o->b[33] << 8;
        same = na == nb;
        for (unsigned i = 0; same && i < na && 34 + 2 * i + 1 < size; i++) {
            unsigned ia = b[34 + 2 * i] | b[35 + 2 * i] << 8, ib = o->b[34 + 2 * i] | o->b[35 + 2 * i] << 8;
            if (ia + 22 > size || ib + 22 > o->n) { same = 0; break; }
            unsigned ca = b[ia + 20] | b[ia + 21] << 8, cb = o->b[ib + 20] | o->b[ib + 21] << 8;
            same = ca < size && cb < o->n && !strcmp((const char *)b + ca, (const char *)o->b + cb);
        }
    }
    free(b);
    return same;
}

static int grp_folders(FILE *log) {
    char host[HOST_MAX];
    if (w16_dos_path("C:\\WINDOWS\\PROGMAN.EXE", host, sizeof host, 0, 0, 0) != 0) {
        fprintf(log, "no Program Manager on drive C:\n");
        return 0;
    }
    struct w16_folder *f;
    int nf = w16_folders_load(&f), rc = 0;
    // The [Groups] entries for RLn.GRP files, by n; a free GroupN for a new one.
    size_t cap = 8192;
    char *names = malloc(cap);
    if (!names) { w16_folders_free(f, nf); return -1; }
    size_t len = w16_ini_names("PROGMAN.INI", "Groups", names, cap);
    int last = 0;
    for (size_t at = 0; at < len && names[at]; at += strlen(names + at) + 1)
        if (!strncasecmp(names + at, "Group", 5) && atoi(names + at + 5) > last) last = atoi(names + at + 5);
    for (int i = 0; i < nf; i++) {
        DIR *d = opendir(f[i].path);
        char **exe = 0;
        int n = 0, cap = 0;
        for (struct dirent *e; d && (e = readdir(d)); ) {
            if (!is_exe(e->d_name)) continue;
            if (n == cap) {
                char **g = realloc(exe, sizeof *g * (size_t)(cap = cap ? cap * 2 : 16));
                if (!g) break;
                exe = g;
            }
            if ((exe[n] = strdup(e->d_name))) n++;
        }
        if (d) closedir(d);
        qsort(exe, (size_t)n, sizeof *exe, by_name);
        struct grp_item *it = calloc((size_t)(n ? n : 1), sizeof *it);
        int k = 0;
        for (int j = 0; it && j < n; j++) {
            char path[HOST_MAX], dos[W16_PATH_MAX];
            snprintf(path, sizeof path, "%s/%s", f[i].path, exe[j]);
            if (w16_host_to_dos(path, dos, sizeof dos) != 0) continue;
            snprintf(it[k].desc, sizeof it[k].desc, "%s", exe[j]);
            char *dot = strrchr(it[k].desc, '.');
            if (dot) *dot = 0;
            snprintf(it[k].cmd, sizeof it[k].cmd, "%s", dos);
            snprintf(it[k].icon_path, sizeof it[k].icon_path, "%s", dos);
            item_icon(path, 0, &it[k]);
            k++;
        }
        for (int j = 0; j < n; j++) free(exe[j]);
        free(exe);
        const char *title = f[i].name && f[i].name[0] ? f[i].name : strrchr(f[i].path, '/') ? strrchr(f[i].path, '/') + 1 : f[i].path;
        char dos[W16_PATH_MAX], key[16], have[W16_PATH_MAX];
        snprintf(dos, sizeof dos, "C:\\WINDOWS\\RL%d.GRP", i + 1);
        int16_t rc4[4] = { (int16_t)(22 * i), (int16_t)(22 * i), (int16_t)(22 * i + 416), (int16_t)(22 * i + 202) };
        struct buf o = { 0 };
        int ok = it && w16_dos_path(dos, host, sizeof host, 1, 0, 0) == 0 && grp_bytes(&o, title, 2, rc4, it, k, 5) == 0;
        free(it);
        if (ok && !same_group(host, &o)) ok = write_file(host, &o) == 0;
        free(o.b);
        if (!ok) { fprintf(log, "group %s NOT written\n", title); rc = -1; continue; }
        int listed = 0;                          // its [Groups] line, once
        for (size_t at = 0; at < len && names[at] && !listed; at += strlen(names + at) + 1)
            listed = w16_ini_get("PROGMAN.INI", "Groups", names + at, have, sizeof have) > 0 && !strcasecmp(have, dos);
        if (!listed) {
            snprintf(key, sizeof key, "Group%d", ++last);
            w16_ini_set("PROGMAN.INI", "Groups", key, dos);
            // First in Order: under the groups already there, so the one open stays on top.
            char order[512], num[16];                // the INI's reading trims the value: "N " goes in front
            if (w16_ini_get("PROGMAN.INI", "Settings", "Order", order, sizeof order) <= 0) order[0] = 0;
            snprintf(num, sizeof num, "%d ", last);
            if (strlen(order) + strlen(num) < sizeof order) {
                memmove(order + strlen(num), order, strlen(order) + 1);
                memcpy(order, num, strlen(num));
                w16_ini_set("PROGMAN.INI", "Settings", "Order", order);
            }
        }
        fprintf(log, "group %s %s %d\n", title, dos, k);
    }
    // A folder no longer in the list: its group goes, file and [Groups] line.
    for (int i = nf + 1; i < nf + 64; i++) {
        char dos[W16_PATH_MAX], have[W16_PATH_MAX];
        snprintf(dos, sizeof dos, "C:\\WINDOWS\\RL%d.GRP", i);
        if (w16_dos_path(dos, host, sizeof host, 0, 0, 0) != 0) break;
        remove(host);
        for (size_t at = 0; at < len && names[at]; at += strlen(names + at) + 1)
            if (w16_ini_get("PROGMAN.INI", "Groups", names + at, have, sizeof have) > 0 && !strcasecmp(have, dos))
                w16_ini_set("PROGMAN.INI", "Groups", names + at, 0);
        fprintf(log, "group gone %s\n", dos);
    }
    w16_folders_free(f, nf);
    free(names);
    return rc;
}

int w16_grp_setup(const struct uwinsetup *s, FILE *log) {
    if (lock_groups() < 0) { fprintf(log, "groups NOT written: another w16 is writing them\n"); return -1; }
    int r = grp_setup(s, log);
    display_driver();
    unlock_groups();
    return r;
}

int w16_grp_folders(FILE *log) {
    char host[HOST_MAX];
    if (w16_dos_path("C:\\WINDOWS\\PROGMAN.EXE", host, sizeof host, 0, 0, 0) != 0) {
        fprintf(log, "no Program Manager on drive C:\n");
        return 0;
    }
    if (lock_groups() < 0) { fprintf(log, "groups NOT written: another w16 is writing them\n"); return -1; }
    int r = grp_folders(log);
    display_driver();
    unlock_groups();
    return r;
}
