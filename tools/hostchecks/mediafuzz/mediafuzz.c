// The kernel's removable-media parsers, run on the host over a hostile
// disk: FAT32, exFAT, ISO 9660, the partition table, fs_ident.
//
// INPUT, one "sparse image": byte 0 the ops to run (FZ_OP_*), bytes 1-4
// the disk's size in sectors (little-endian, clamped), then records of a
// 4-byte sector number and that sector's 512 bytes. A sector no record
// names reads as zeros, so a 40 MiB volume is a few KiB of input -- and
// a mutation of a record's number moves its sector somewhere else, which
// is a mutation too.
//
// Built two ways from this one file: with -DFZ_LIBFUZZER under clang's
// libFuzzer (coverage-guided, mediaparse_hostcheck.py --fuzz), and with a
// main() under gcc that replays files (the crafted cases, the seeds and
// their mutations). FZ_REPORT=1 prints what mounted and what it cost,
// FZ_GUARD=lo:hi makes a write into those sectors an invariant failure,
// FZ_VERBOSE=1 shows the kernel's log lines.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fs.h"
#include "fs_ops.h"
#include "fat32.h"
#include "exfat.h"
#include "iso9660.h"
#include "partition.h"
#include "fs_ident.h"
#include "mediafuzz.h"

enum {
    FZ_OP_WRITE = 1, // create, write, rename, truncate, delete on the volume
    FZ_OP_CHECK = 2, // a read-only fsck pass
    FZ_OP_SHRINK = 4, // unmounted, the volume shrunk by 1/64 (FAT32's resize)
    FZ_OP_TOUCH = 8, // only one create, in the first directory found
};

// What a parser's ACCEPTED answer must satisfy, because its callers
// assume it. The sanitizers see memory; these see the contract.
static void bad(const char *what, const char *detail) {
    fprintf(stderr, "INVARIANT: %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
    abort();
}

#define FZ_MAX_SECTORS 0xFFFFFFFFull // 2 TiB of address space, all of it sparse
#define FZ_DEPTH 6
#define FZ_PER_DIR 256
#define FZ_READ_CAP (256u << 10)

static int load(const uint8_t *d, size_t n, unsigned *ops) {
    memset(fz.used, 0, sizeof fz.used);
    fz.nused = 0;
    fz.reads = fz.oob_io = 0; // `written` adds up over the whole input: FZ_REPORT prints it
    if (n < 5) return 0;
    *ops = d[0];
    uint64_t s = (uint32_t)d[1] | (uint32_t)d[2] << 8 | (uint32_t)d[3] << 16 | (uint32_t)d[4] << 24;
    if (s < 16) s = 16;
    if (s > FZ_MAX_SECTORS) s = FZ_MAX_SECTORS;
    fz.sectors = s;
    uint8_t sec[512];
    for (size_t o = 5; o + 4 < n; o += 516) {
        uint32_t lba = (uint32_t)d[o] | (uint32_t)d[o + 1] << 8 | (uint32_t)d[o + 2] << 16 | (uint32_t)d[o + 3] << 24;
        size_t have = n - (o + 4) < 512 ? n - (o + 4) : 512;
        memset(sec, 0, sizeof sec);
        memcpy(sec, d + o + 4, have);
        if (lba < fz.sectors) fz_put_sector(lba, sec);
    }
    return 1;
}

// ---- walking a mounted volume through its ops table -----------------------
static unsigned mounted, entries, parts; // what FZ_REPORT prints: which parsers got past their mount
struct names { char **v; uint32_t n; };
static void collect(void *ctx, const char *name, uint32_t size, int is_dir) {
    (void)size; (void)is_dir;
    struct names *l = ctx;
    if (l->n >= FZ_PER_DIR) return;
    size_t len = strlen(name);  // a name the backend hands out must be a C string
    // ONE COMPONENT: a ring-3 walker joins it to its directory.
    if (!len || !strcmp(name, ".") || !strcmp(name, "..") || strchr(name, '/'))
        bad("a listed name is not one path component", name);
    char *c = malloc(len + 1);
    memcpy(c, name, len + 1);
    l->v[l->n++] = c;
    entries++;
}

static char first_file[4096], first_dir[4096];
static uint8_t *rbuf;

static void walk(const struct fs_ops *ops, void *st, const char *dir, int depth) {
    struct names l = { calloc(FZ_PER_DIR, sizeof(char *)), 0 };
    ops->list(st, dir, collect, &l);
    for (uint32_t i = 0; i < l.n; i++) {
        char path[4096];
        snprintf(path, sizeof path, "%s%s%s", dir, dir[strlen(dir) - 1] == '/' ? "" : "/", l.v[i]);
        struct fs_stat_info si;
        if (ops->stat) ops->stat(st, path, &si);
        ops->exists(st, path);
        if (ops->is_dir(st, path)) {
            if (!first_dir[0]) snprintf(first_dir, sizeof first_dir, "%s", path);
            if (depth < FZ_DEPTH) walk(ops, st, path, depth + 1);
        } else {
            if (!first_file[0]) snprintf(first_file, sizeof first_file, "%s", path);
            uint64_t size = ops->size(st, path);
            uint32_t want = size < FZ_READ_CAP ? (uint32_t)size : FZ_READ_CAP;
            ops->read_range(st, path, 0, rbuf, want);
            if (size > 4096) ops->read_range(st, path, size - 4096, rbuf, 4096);
            if (ops->read_range_begin) {
                uint32_t total = 0;
                void *h = ops->read_range_begin(st, path, 0, rbuf, want < 8192 ? want : 8192);
                if (h) while (ops->read_range_step(st, h, &total) == FS_STEP_PENDING) {}
            }
        }
    }
    for (uint32_t i = 0; i < l.n; i++) free(l.v[i]);
    free(l.v);
}

static void writes(const struct fs_ops *ops, void *st) {
    static const char data[] = "fuzz data, a few clusters long once appended";
    ops->mkdir(st, "/fzd");
    ops->write(st, "/fzd/a.txt", data, 0);
    for (int i = 0; i < 8; i++) ops->write(st, "/fzd/a.txt", data, 1);
    ops->touch(st, "/fz.txt");
    ops->write_range(st, "/fz.txt", 70000, data, sizeof data);
    ops->rename(st, "/fzd/a.txt", "/fzb.txt");
    ops->truncate(st, "/fzb.txt", 3);
    if (first_file[0]) {
        ops->write(st, first_file, data, 1);
        ops->write_range(st, first_file, 1, data, sizeof data);
        ops->truncate(st, first_file, 1);
        ops->del(st, first_file);
    }
    if (first_dir[0]) {
        char p[4200];
        snprintf(p, sizeof p, "%s/new.txt", first_dir);
        ops->write(st, p, data, 0);
        ops->del(st, p);
        ops->del(st, first_dir);
    }
    ops->del(st, "/fzb.txt");
    ops->del(st, "/fzd");
    if (ops->sync) ops->sync(st);
}

static void run_backend(const struct fs_ops *ops, unsigned op) {
    void *st = ops->state_alloc();
    if (!st) return;
    if (ops->probe(st, &fz_dev) == 1 && ops->init(st, &fz_dev, fz.sectors * 512) == 1) {
        mounted |= ops == &fat32_ops ? 1 : 2;
        first_file[0] = first_dir[0] = 0;
        walk(ops, st, "/", 0);
        uint64_t used = 0, total = 0;
        if (ops->disk_usage && ops->disk_usage(st, &used, &total) && used > total)
            bad("disk usage: more used than the volume holds", NULL);
        if (op & FZ_OP_TOUCH) {
            char p[4200];
            snprintf(p, sizeof p, "%s/new.txt", first_dir[0] ? first_dir : "");
            ops->touch(st, p);
        } else if (op & FZ_OP_WRITE) writes(ops, st);
        if (ops->disk_usage && ops->disk_usage(st, &used, &total) && used > total)
            bad("disk usage after the writes: more used than the volume holds", NULL);
        if ((op & FZ_OP_CHECK) && ops->check) {
            struct fs_check_result r;
            memset(&r, 0, sizeof r);
            ops->check(st, 0, &r, NULL);
        }
        ops->umount(st, &fz_dev);
    }
    ops->state_free(st);
    if ((op & FZ_OP_SHRINK) && ops->resize) {
        st = ops->state_alloc();
        struct fs_resize_info info;
        memset(&info, 0, sizeof info);
        if (st) ops->resize(st, &fz_dev, fz.sectors - fz.sectors / 64, &info); // a little: FAT32 has a minimum
        if (st) ops->state_free(st);
    }
}

static void run_iso(void) {
    struct iso_vol *v;
    const char *why = "";
    char label[64];
    if (!iso9660_open(&v, "/fuzz.iso", label, sizeof label, &why)) return;
    mounted |= 4;
    first_file[0] = first_dir[0] = 0;
    walk(&iso9660_ops, v, "/", 0);
    iso9660_close(v);
}

static void run_one(const uint8_t *d, size_t n) {
    unsigned op = 0;
    if (!rbuf) rbuf = malloc(FZ_READ_CAP);
    mounted = entries = 0;
    fz.written = 0;
    if (!load(d, n, &op)) return;
    struct fs_ident id;
    fs_ident(&fz_dev, &id);
    struct partition_table t;
    if (partition_read_table_of(&fz_dev, &t) > 0) mounted |= 8;
    parts = (unsigned)t.entry_count;
    run_backend(&fat32_ops, op);
    load(d, n, &op); // each backend sees the input, not the last one's writes
    run_backend(&exfat_ops, op);
    load(d, n, &op);
    run_iso();
}

#ifdef FZ_LIBFUZZER
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) { run_one(d, n); return 0; }
#else
// `--make exfat OUT SECTORS`: a seed formatted and filled by the kernel's
// own exFAT, since the host has no exFAT writer that needs no mount.
static int make_exfat(const char *out, uint64_t sectors) {
    memset(fz.used, 0, sizeof fz.used);
    fz.nused = 0;
    fz.sectors = sectors;
    void *st = exfat_ops.state_alloc();
    if (!exfat_ops.format(st, &fz_dev) || exfat_ops.probe(st, &fz_dev) != 1 ||
        exfat_ops.init(st, &fz_dev, sectors * 512) != 1) return 1;
    static char big[20000];
    memset(big, 'x', sizeof big - 1);
    exfat_ops.mkdir(st, "/docs");
    exfat_ops.mkdir(st, "/docs/deeper");
    exfat_ops.write(st, "/docs/deeper/n\xc3\xa4me with a long name.txt", "hello", 0);
    exfat_ops.write(st, "/big.bin", big, 0);
    exfat_ops.write(st, "/small.txt", "abc", 0);
    for (int i = 0; i < 20; i++) { char p[64]; snprintf(p, sizeof p, "/docs/f%02d.txt", i); exfat_ops.write(st, p, "f", 0); }

    exfat_ops.umount(st, &fz_dev);
    exfat_ops.state_free(st);
    FILE *f = fopen(out, "wb");
    uint8_t h[5] = { 3, (uint8_t)sectors, (uint8_t)(sectors >> 8), (uint8_t)(sectors >> 16), (uint8_t)(sectors >> 24) };
    fwrite(h, 1, 5, f);
    for (uint32_t s = 0; s < FZ_SLOTS; s++) {
        if (!fz.used[s]) continue;
        int nz = 0;
        for (int i = 0; i < 512; i++) if (fz.data[s][i]) { nz = 1; break; }
        if (!nz) continue;
        uint32_t l = (uint32_t)fz.lba[s];
        uint8_t b[4] = { (uint8_t)l, (uint8_t)(l >> 8), (uint8_t)(l >> 16), (uint8_t)(l >> 24) };
        fwrite(b, 1, 4, f); fwrite(fz.data[s], 1, 512, f);
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    if (getenv("FZ_VERBOSE")) fz_verbose = 1;
    const char *g = getenv("FZ_GUARD");
    if (g) { char *e; fz.guard_lo = strtoull(g, &e, 0); fz.guard_hi = *e == ':' ? strtoull(e + 1, 0, 0) : 0; }
    if (argc == 5 && !strcmp(argv[1], "--make") && !strcmp(argv[2], "exfat"))
        return make_exfat(argv[3], strtoull(argv[4], 0, 0));
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) { perror(argv[i]); return 2; }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *d = malloc((size_t)n + 1);
        if (fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 2; }
        fclose(f);
        run_one(d, (size_t)n);
        free(d);
        if (getenv("FZ_REPORT"))
            printf("%s: %s%s%s%s(%u) %u entries, biggest alloc %zu, wrote %llu\n", argv[i],
                   mounted & 1 ? "fat32 " : "", mounted & 2 ? "exfat " : "", mounted & 4 ? "iso " : "",
                   mounted & 8 ? "parts" : "", parts, entries, fz_biggest_alloc, (unsigned long long)fz.written);
    }
    return 0;
}
#endif
