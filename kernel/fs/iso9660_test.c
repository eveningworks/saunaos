// iso9660: images built here, byte by byte, so each test knows exactly
// what is on the disc -- plain names, Joliet names, a file in two extents
// -- mounted from a file in /tmp at a folder beside it (the image on the
// mount's PARENT, the lock order iso9660.c's top comment is about). Then
// the same image damaged on purpose, a few hundred ways, which must
// mount or refuse but never fault.
#include "ktest.h"
#include "iso9660.h"
#include "mount.h"
#include "fs.h"
#include "heap.h"
#include "string.h"
#include "tmppath.h"

#define B 2048u
#define BLOCKS 32u
#define L_PVD 16
#define L_SVD 17
#define L_ROOT 20
#define L_SUB 21
#define L_JROOT 22
#define L_JSUB 23
#define L_HELLO 24
#define L_BIG1 25
#define L_BIG2 27
#define L_DEEP 28
#define BIG_TAIL 100u

static const char HELLO[] = "Hello from a disc\n";
static const char DEEP[] = "deep";

static void both32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) { p[i] = (uint8_t)(v >> (8 * i)); p[7 - i] = (uint8_t)(v >> (8 * i)); }
}
static void both16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = p[1]; p[3] = p[0]; }

// One directory record at *off; `name` is `nlen` raw bytes.
static void rec(uint8_t *dir, uint32_t *off, uint32_t lba, uint32_t len, uint8_t flags,
                const void *name, uint32_t nlen) {
    uint32_t rl = 33 + nlen + ((33 + nlen) & 1);
    if (*off % B + rl > B) *off += B - *off % B;
    uint8_t *r = dir + *off;
    k_memset(r, 0, rl);
    r[0] = (uint8_t)rl;
    both32(r + 2, lba);
    both32(r + 10, len);
    r[18] = 99; r[19] = 12; r[20] = 31; r[21] = 23; r[22] = 59; r[23] = 58; r[24] = 0;  // 1999-12-31 23:59:58 UTC
    r[25] = flags;
    both16(r + 28, 1);
    r[32] = (uint8_t)nlen;
    k_memcpy(r + 33, name, nlen);
    *off += rl;
}

static void dots(uint8_t *dir, uint32_t *off, uint32_t self, uint32_t parent) {
    uint8_t z = 0, o = 1;
    rec(dir, off, self, B, 2, &z, 1);
    rec(dir, off, parent, B, 2, &o, 1);
}

// "abc" as UCS-2 big-endian into out; its byte length.
static uint32_t ucs2(const char *s, uint8_t *out) {
    uint32_t n = 0;
    for (; *s; s++) { out[n++] = 0; out[n++] = (uint8_t)*s; }
    return n;
}

static void vd(uint8_t *d, int type, uint32_t root_lba) {
    d[0] = (uint8_t)type;
    k_memcpy(d + 1, "CD001", 5);
    d[6] = 1;
    both32(d + 80, BLOCKS);
    both16(d + 128, B);
    uint8_t *r = d + 156;
    r[0] = 34;
    both32(r + 2, root_lba);
    both32(r + 10, B);
    r[25] = 2;
    r[32] = 1;
}

// The whole image, kmalloc'd: BLOCKS * 2048 bytes.
static uint8_t *build(int joliet) {
    uint8_t *img = kzalloc(BLOCKS * B);
    if (!img) return 0;
    uint8_t *p = img + L_PVD * B;
    vd(p, 1, L_ROOT);
    k_memset(p + 40, ' ', 32);
    k_memcpy(p + 40, "TESTDISC", 8);
    if (joliet) {
        uint8_t *s = img + L_SVD * B;
        vd(s, 2, L_JROOT);
        s[88] = '%'; s[89] = '/'; s[90] = 'E';
        uint8_t lab[32];
        uint32_t n = ucs2("Test Disc", lab);
        for (uint32_t i = 0; i < 32; i += 2) { s[40 + i] = 0; s[41 + i] = ' '; }
        k_memcpy(s + 40, lab, n);
    }
    uint8_t *t = img + (joliet ? L_SVD + 1 : L_SVD) * B;
    t[0] = 255; k_memcpy(t + 1, "CD001", 5); t[6] = 1;

    uint32_t off = 0;
    uint8_t *root = img + L_ROOT * B;
    dots(root, &off, L_ROOT, L_ROOT);
    rec(root, &off, L_BIG1, B, 0x80, "BIG.BIN;1", 9);
    rec(root, &off, L_BIG2, BIG_TAIL, 0, "BIG.BIN;1", 9);
    rec(root, &off, L_HELLO, sizeof HELLO - 1, 0, "HELLO.TXT;1", 11);
    rec(root, &off, L_HELLO, 0, 0, "README.;1", 9);
    rec(root, &off, L_HELLO, 4, 0x04, "HIDDEN.ASC;1", 12);   // an associated file: not listed
    rec(root, &off, L_SUB, B, 2, "SUB", 3);
    off = 0;
    uint8_t *sub = img + L_SUB * B;
    dots(sub, &off, L_SUB, L_ROOT);
    rec(sub, &off, L_DEEP, sizeof DEEP - 1, 0, "DEEP.TXT;1", 10);

    if (joliet) {
        uint8_t nm[128];
        off = 0;
        uint8_t *jr = img + L_JROOT * B;
        dots(jr, &off, L_JROOT, L_JROOT);
        rec(jr, &off, L_HELLO, sizeof HELLO - 1, 0, nm, ucs2("Hello, long name.txt;1", nm));
        rec(jr, &off, L_JSUB, B, 2, nm, ucs2("Sub folder", nm));
        off = 0;
        uint8_t *js = img + L_JSUB * B;
        dots(js, &off, L_JSUB, L_JROOT);
        rec(js, &off, L_DEEP, sizeof DEEP - 1, 0, nm, ucs2("deep.txt;1", nm));
    }
    k_memcpy(img + L_HELLO * B, HELLO, sizeof HELLO - 1);
    for (uint32_t i = 0; i < B; i++) img[L_BIG1 * B + i] = (uint8_t)(i * 7);
    for (uint32_t i = 0; i < BIG_TAIL; i++) img[L_BIG2 * B + i] = (uint8_t)(0xA0 + i);
    k_memcpy(img + L_DEEP * B, DEEP, sizeof DEEP - 1);
    return img;
}

struct paths { char img[FS_PATH_STORED_MAX], pt[FS_PATH_STORED_MAX], f[FS_PATH_STORED_MAX]; };

static int setup(struct paths *p, const uint8_t *img) {
    tmppath(p->img, sizeof p->img, TMP_VOLATILE, "ktest-iso.img");
    tmppath(p->pt, sizeof p->pt, TMP_VOLATILE, "ktest-iso-mnt");
    fs_delete(p->img);
    if (!fs_is_dir(p->pt) && !fs_mkdir(p->pt)) return 0;
    fs_touch(p->img);
    return fs_write_range(p->img, 0, img, BLOCKS * B);
}

static const char *at(struct paths *p, const char *rel) {
    k_strlcpy(p->f, p->pt, sizeof p->f);
    k_strlcat(p->f, rel, sizeof p->f);
    return p->f;
}

struct seen { int n, hello, big, readme, sub, hidden, longname; };
static void see(void *ctx, const char *name, uint32_t size, int is_dir) {
    struct seen *s = ctx;
    s->n++;
    if (!k_strcmp(name, "HELLO.TXT") && size == sizeof HELLO - 1 && !is_dir) s->hello = 1;
    if (!k_strcmp(name, "BIG.BIN") && size == B + BIG_TAIL) s->big = 1;
    if (!k_strcmp(name, "README") && size == 0) s->readme = 1;
    if (!k_strcmp(name, "SUB") && is_dir) s->sub = 1;
    if (!k_strcmp(name, "HIDDEN.ASC")) s->hidden = 1;
    if (!k_strcmp(name, "Hello, long name.txt")) s->longname = 1;
}

KTEST("iso9660", "a plain image mounts from a file: names as written, case-blind, two extents, read-only") {
    struct paths *p = kmalloc(sizeof *p);
    uint8_t *img = build(0);
    KTEST_ASSERT(p && img && setup(p, img));
    const char *why = "";
    KTEST_ASSERT(iso9660_mount_image(p->img, p->pt, &why));

    struct seen s = { 0 };
    fs_list(p->pt, see, &s);
    KTEST_ASSERT_EQ(s.n, 4);           // no "." or "..", no associated file
    KTEST_ASSERT(s.hello && s.big && s.readme && s.sub && !s.hidden);

    char buf[64] = { 0 };
    KTEST_ASSERT_EQ(fs_read_range(at(p, "/hello.txt"), 0, buf, sizeof buf), sizeof HELLO - 1);
    KTEST_ASSERT(!k_memcmp(buf, HELLO, sizeof HELLO - 1));
    KTEST_ASSERT_EQ(fs_read_range(at(p, "/Sub/Deep.Txt"), 0, buf, sizeof buf), sizeof DEEP - 1);

    // Across the gap between the two extents: the last 10 of one, then the other.
    uint8_t big[20];
    KTEST_ASSERT_EQ(fs_read_range(at(p, "/BIG.BIN"), B - 10, big, sizeof big), 20);
    KTEST_ASSERT_EQ(big[0], (uint8_t)((B - 10) * 7));
    KTEST_ASSERT_EQ(big[10], 0xA0);
    KTEST_ASSERT_EQ(big[19], 0xA9);
    KTEST_ASSERT_EQ(fs_read_range(at(p, "/BIG.BIN"), B + BIG_TAIL - 5, big, sizeof big), 5);

    struct fs_stat_info st;
    KTEST_ASSERT(fs_stat(at(p, "/HELLO.TXT"), &st));
    KTEST_ASSERT_EQ(st.modified, 946684798ull);   // 1999-12-31 23:59:58 UTC
    KTEST_ASSERT(!fs_exists(at(p, "/HIDDEN.ASC")));
    KTEST_ASSERT(!fs_write(at(p, "/HELLO.TXT"), "x", 1));

    // Its file is pinned while it is mounted, and nothing covers it.
    KTEST_ASSERT(!fs_delete(p->img));
    char point[64];
    KTEST_ASSERT(mount_backing_under(p->img, point, sizeof point));
    KTEST_ASSERT(mount_remove(p->pt, &why));
    KTEST_ASSERT(!mount_backing_under(p->img, 0, 0));
    KTEST_ASSERT(fs_delete(p->img));
    kfree(img);
    kfree(p);
}

KTEST("iso9660", "Joliet's names win when the disc has them, and its label is the volume's") {
    struct paths *p = kmalloc(sizeof *p);
    uint8_t *img = build(1);
    KTEST_ASSERT(p && img && setup(p, img));
    struct iso_vol *v;
    char label[64];
    const char *why = "";
    KTEST_ASSERT(iso9660_open(&v, p->img, label, sizeof label, &why));
    iso9660_close(v);
    KTEST_ASSERT(!k_strcmp(label, "Test Disc"));

    KTEST_ASSERT(iso9660_mount_image(p->img, p->pt, &why));
    struct seen s = { 0 };
    fs_list(p->pt, see, &s);
    KTEST_ASSERT_EQ(s.n, 2);
    KTEST_ASSERT(s.longname && !s.hello);
    char buf[32];
    KTEST_ASSERT_EQ(fs_read_range(at(p, "/sub FOLDER/DEEP.txt"), 0, buf, sizeof buf), sizeof DEEP - 1);
    KTEST_ASSERT(mount_remove(p->pt, &why));
    fs_delete(p->img);
    kfree(img);
    kfree(p);
}

KTEST("iso9660", "refused: no descriptor, too small, the image under its own mount point") {
    struct paths *p = kmalloc(sizeof *p);
    uint8_t *img = build(0);
    KTEST_ASSERT(p && img);
    img[L_PVD * B + 1] = 'X';                    // "CX001"
    KTEST_ASSERT(setup(p, img));
    const char *why = "";
    KTEST_ASSERT(!iso9660_mount_image(p->img, p->pt, &why));
    KTEST_ASSERT(k_strstr(why, "no ISO 9660") != 0);

    fs_delete(p->img);
    fs_touch(p->img);
    fs_write_range(p->img, 0, img, 4 * B);
    KTEST_ASSERT(!iso9660_mount_image(p->img, p->pt, &why));

    // Mounting over the folder that holds the image would read it through itself.
    char dir[FS_PATH_STORED_MAX];
    k_strlcpy(dir, p->img, sizeof dir);
    char *slash = dir + k_strlen(dir);
    while (slash > dir && *slash != '/') slash--;
    *slash = 0;
    img[L_PVD * B + 1] = 'C';
    fs_delete(p->img);
    KTEST_ASSERT(setup(p, img));
    KTEST_ASSERT(!iso9660_mount_image(p->img, dir, &why));
    fs_delete(p->img);
    kfree(img);
    kfree(p);
}

KTEST("iso9660", "an image is mounted once; a name of \"..\" is no entry, a '/' in one is not a path") {
    struct paths *p = kmalloc(sizeof *p);
    uint8_t *img = build(0);
    KTEST_ASSERT(p && img);
    // HELLO.TXT's record (at 152 in the root) renamed "..", README's (196) "A/B".
    uint8_t *r = img + L_ROOT * B + 152;
    r[32] = 2; r[33] = '.'; r[34] = '.';
    r = img + L_ROOT * B + 196;
    r[32] = 3; r[33] = 'A'; r[34] = '/'; r[35] = 'B';
    KTEST_ASSERT(setup(p, img));
    const char *why = "";
    KTEST_ASSERT(iso9660_mount_image(p->img, p->pt, &why));
    struct seen s = { 0 };
    fs_list(p->pt, see, &s);
    KTEST_ASSERT_EQ(s.n, 3);                   // BIG.BIN, A_B, SUB: ".." is gone
    KTEST_ASSERT(fs_exists(at(p, "/A_B")));
    char second[FS_PATH_STORED_MAX];
    tmppath(second, sizeof second, TMP_VOLATILE, "ktest-iso-mnt2");
    if (!fs_is_dir(second)) fs_mkdir(second);
    KTEST_ASSERT(!iso9660_mount_image(p->img, second, &why));
    KTEST_ASSERT(k_strstr(why, "already mounted") != 0);
    KTEST_ASSERT(mount_remove(p->pt, &why));
    fs_delete(p->img);
    kfree(img);
    kfree(p);
}

// ---- a damaged disc ----------------------------------------------------

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return g_rng; }

struct walk { struct paths *p; int depth; int entries; char *buf; };

static void walk_dir(struct walk *w, const char *dir);

static void walk_cb(void *ctx, const char *name, uint32_t size, int is_dir) {
    struct walk *w = ctx;
    (void)size;
    if (++w->entries > 400) return;
    // Collected rather than recursed into here: a callback must not call
    // back into the same mount.
    (void)name; (void)is_dir;
}

struct names { char (*n)[FS_NAME_MAX + 1]; int *dir; int count, cap; };
static void collect(void *ctx, const char *name, uint32_t size, int is_dir) {
    struct names *ns = ctx;
    (void)size;
    if (ns->count >= ns->cap) return;
    k_strlcpy(ns->n[ns->count], name, sizeof ns->n[0]);
    ns->dir[ns->count++] = is_dir;
}

static void walk_dir(struct walk *w, const char *dir) {
    if (w->depth > 4 || w->entries > 400) return;
    struct names ns = { kmalloc(sizeof *ns.n * 16), kmalloc(sizeof(int) * 16), 0, 16 };
    char *path = kmalloc(FS_PATH_STORED_MAX * 2);
    if (ns.n && ns.dir && path) {
        fs_list(dir, collect, &ns);
        fs_list(dir, walk_cb, w);
        for (int i = 0; i < ns.count; i++) {
            k_strlcpy(path, dir, FS_PATH_STORED_MAX * 2);
            k_strlcat(path, "/", FS_PATH_STORED_MAX * 2);
            k_strlcat(path, ns.n[i], FS_PATH_STORED_MAX * 2);
            struct fs_stat_info st;
            fs_stat(path, &st);
            fs_read_range(path, 0, w->buf, B * 2);
            fs_read_range(path, B - 3, w->buf, 64);
            if (ns.dir[i]) { w->depth++; walk_dir(w, path); w->depth--; }
        }
    }
    kfree(ns.n);
    kfree(ns.dir);
    kfree(path);
}

struct outcome { int root_entries; int hello; };
static void count(void *ctx, const char *name, uint32_t size, int is_dir) {
    (void)name; (void)size; (void)is_dir;
    (*(int *)ctx)++;
}

// 1 mounted (and walked), 0 refused, -1 the test itself failed.
static int mount_and_walk(struct paths *p, const uint8_t *img, char *buf, struct outcome *o) {
    fs_delete(p->img);
    if (!setup(p, img)) return -1;
    const char *why = "";
    if (!iso9660_mount_image(p->img, p->pt, &why)) return 0;
    if (o) {
        o->root_entries = 0;
        fs_list(p->pt, count, &o->root_entries);
        o->hello = (int)fs_read_range(at(p, "/HELLO.TXT"), 0, buf, 64);
    }
    struct walk w = { p, 0, 0, buf };
    walk_dir(&w, p->pt);
    return mount_remove(p->pt, &why) ? 1 : -1;
}

KTEST("iso9660", "a damaged image mounts or is refused, and never faults (300 damaged copies)") {
    struct paths *p = kmalloc(sizeof *p);
    uint8_t *plain = build(0), *joliet = build(1);
    uint8_t *img = kmalloc(BLOCKS * B);
    char *buf = kmalloc(B * 2);
    KTEST_ASSERT(p && plain && joliet && img && buf);

    // The cases each check exists for, by hand, with what each must give.
    // Plain root: "." 0, ".." 34, BIG.BIN 68 and 110, HELLO.TXT 152. Joliet
    // root: "." 0, ".." 34, its file 68, "Sub folder" 146.
    enum { ANY = -2, REFUSED = -1 };
    static const struct { int joliet; uint32_t at; uint8_t v; int entries, hello; } cases[] = {
        { 0, L_ROOT * B + 152 + 2 + 3, 0x7f, 4, 0 },     // an extent far past the image: no bytes
        { 0, L_ROOT * B + 152 + 26, 4, 4, 0 },           // interleaved: not read
        { 0, L_ROOT * B + 68 + 32, 0xf0, 0, 0 },         // a name longer than its record: the listing ends
        { 0, L_ROOT * B + 68, 3, 0, 0 },                 // a record shorter than a header: the same
        { 0, L_PVD * B + 156 + 10 + 3, 0x7f, REFUSED, 0 },  // a root bigger than the image
        { 0, L_PVD * B + 156 + 2 + 3, 0x7f, REFUSED, 0 },   // a root past the image
        { 0, L_PVD * B + 129, 2, REFUSED, 0 },           // a block size of 512, not read
        { 0, L_ROOT * B + 284 + 10 + 3, 0xff, 4, ANY },  // a folder longer than the image: no allocation for it
        { 1, L_JROOT * B + 146 + 2, L_JROOT, 2, ANY },   // a folder that is its own parent
        { 1, L_JROOT * B + 146 + 25, 0x82, 2, ANY },     // a folder flagged multi-extent
        { 1, L_JSUB * B + 68 + 2, L_JROOT, 2, ANY },     // a file whose data is a directory
    };
    struct outcome o;
    for (uint32_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        k_memcpy(img, cases[i].joliet ? joliet : plain, BLOCKS * B);
        img[cases[i].at] = cases[i].v;
        int r = mount_and_walk(p, img, buf, &o);
        KTEST_ASSERT(r >= 0);
        if (cases[i].entries == REFUSED) { KTEST_ASSERT_EQ(r, 0); continue; }
        KTEST_ASSERT_EQ(r, 1);
        KTEST_ASSERT_EQ(o.root_entries, cases[i].entries);
        if (cases[i].hello != ANY) KTEST_ASSERT_EQ(o.hello, cases[i].hello);
    }
    int mounted = 0, refused = 0;
    for (int i = 0; i < 300; i++) {
        k_memcpy(img, i & 1 ? joliet : plain, BLOCKS * B);
        int flips = 1 + (int)(rnd() % 24);
        for (int k = 0; k < flips; k++) {
            uint32_t blk = L_PVD + (uint32_t)(rnd() % (L_JSUB - L_PVD + 1));
            img[blk * B + rnd() % 256] = (uint8_t)rnd();
        }
        int r = mount_and_walk(p, img, buf, 0);
        KTEST_ASSERT(r >= 0);
        if (r) mounted++; else refused++;
    }
    KTEST_ASSERT(mounted > 0 && refused > 0);   // both paths were exercised
    fs_delete(p->img);
    kfree(img);
    kfree(plain);
    kfree(joliet);
    kfree(buf);
    kfree(p);
}
