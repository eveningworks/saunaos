// The panic store -- see kernel/panic_store.h for what it is and why the
// range is a constant. This file is the record format, the boot-time
// probe, the panic-time save, and QUERY_PANIC, through which logd files
// the record into the dead boot's log.
#include "panic_store.h"
#include "multiboot.h"
#include "klog.h"
#include "kfmt.h"
#include "kcrc.h"
#include "string.h"
#include "reloc.h"
#include "query.h"
#include "initcall.h"
#include "clocksource.h"
#include "version.h"
#include "random_hw.h"
#include "knum.h"

extern char _kernel_end[];
extern char __kimage_start[];

#define PANIC_RECORD_MAGIC   0x434e5054u // "TPNC"
#define PANIC_RECORD_VERSION 1u

struct panic_record {
    uint32_t magic;
    uint32_t version;
    uint32_t len;       // text bytes after the header
    uint32_t crc;       // kcrc32 of the header with crc = 0, then the text
    uint64_t uptime_ns;
    char     build[48];
};

_Static_assert(sizeof(struct panic_record) == PANIC_RECORD_HDR,
               "PANIC_RECORD_HDR must match struct panic_record");

static uint32_t record_crc(const struct panic_record *r) {
    struct panic_record h = *r;
    h.crc = 0;
    uint32_t crc = kcrc32_update(KCRC32_INIT, &h, sizeof h);
    crc = kcrc32_update(crc, (const char *)r + sizeof *r, r->len);
    return KCRC32_FINAL(crc);
}

int panic_record_seal(void *buf, uint32_t cap, uint32_t len,
                      uint64_t uptime_ns, const char *build) {
    if (cap < sizeof(struct panic_record) || len > cap - sizeof(struct panic_record))
        return 0;
    struct panic_record *r = buf;
    r->magic = PANIC_RECORD_MAGIC;
    r->version = PANIC_RECORD_VERSION;
    r->len = len;
    r->uptime_ns = uptime_ns;
    k_memset(r->build, 0, sizeof r->build);
    if (build) k_strlcpy(r->build, build, sizeof r->build);
    r->crc = record_crc(r);
    return 1;
}

// Everything is checked before `len` is trusted, because the bytes may
// be anything at all: a cold boot's RAM, or firmware's scratch.
int panic_record_check(const void *buf, uint32_t cap) {
    const struct panic_record *r = buf;
    if (cap < sizeof *r) return -1;
    if (r->magic != PANIC_RECORD_MAGIC || r->version != PANIC_RECORD_VERSION) return -1;
    if (r->len > cap - sizeof *r) return -1;
    if (r->build[sizeof r->build - 1] != 0) return -1;
    if (record_crc(r) != r->crc) return -1;
    return (int)r->len;
}

// ---- where ----------------------------------------------------------

int panic_store_parse(const char *value, uint64_t *base) {
    if (!value) { *base = PANIC_STORE_BASE; return PANIC_STORE_DEFAULT; }
    if (!k_strcmp(value, "survey")) return PANIC_STORE_SURVEY;
    uint64_t v;
    int ok = (value[0] == '0' && (value[1] == 'x' || value[1] == 'X'))
           ? k_parse_hex(value, &v) : k_parse_size(value, 1, &v);
    if (!ok || (v & 0xFFF) || v < 0x100000ULL || v + PANIC_STORE_SIZE > 0x100000000ULL)
        return PANIC_STORE_INVALID;
    *base = v;
    return PANIC_STORE_EXPLICIT;
}

// Spread over the low 4 GiB, densest where a 4 GiB laptop's RAM is.
static const uint64_t g_candidates[PANIC_STORE_CANDIDATES] = {
    0x2000000ULL,  0x4000000ULL,  0x6000000ULL,  0x8000000ULL,  0xC000000ULL,
    0x10000000ULL, 0x18000000ULL, 0x20000000ULL, 0x30000000ULL, 0x40000000ULL,
    0x60000000ULL, 0x80000000ULL, 0xA0000000ULL, 0xC0000000ULL,
};

static int mode(uint64_t *base) {
    char v[24];
    return panic_store_parse(multiboot_cmdline_value("panic_store=", v, sizeof v) ? v : 0, base);
}

int panic_store_range(int i, uint64_t *base) {
    uint64_t b;
    switch (mode(&b)) {
    case PANIC_STORE_SURVEY:
        if (i < 0 || i >= PANIC_STORE_CANDIDATES) return 0;
        *base = g_candidates[i];
        return 1;
    case PANIC_STORE_INVALID:
        return 0;
    default:
        if (i) return 0;
        *base = b;
        return 1;
    }
}

// ---- the store ------------------------------------------------------

static int g_usable;
static int g_recovered;
static int g_ram_ok;
static uint64_t g_base = PANIC_STORE_BASE;   // the store's, once probed
static uint32_t g_kept_mask;                 // panic_store_range(i) found usable

static int overlaps(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) {
    return a0 < b1 && b0 < a1;
}

static uint64_t g_probe_base;
static void ram_cb(const struct multiboot_mmap_region *region) {
    if (region->type != 1) return;
    if (region->base <= g_probe_base &&
        region->base + region->length >= g_probe_base + PANIC_STORE_SIZE)
        g_ram_ok = 1;
}

// Why the range at `base` cannot be used on this boot, or NULL.
static const char *refusal(uint64_t base) {
    const uint64_t s = base, e = base + PANIC_STORE_SIZE;
    g_ram_ok = 0;
    g_probe_base = base;
    multiboot_mmap_foreach(ram_cb);
    if (!g_ram_ok) return "not RAM in this machine's memory map";
    uint64_t delta = kernel_reloc_delta();
    uint64_t kend = (uint64_t)(uintptr_t)_kernel_end;
    if (overlaps(s, e, 0x100000, delta ? kend - delta : kend))
        return "the kernel image has grown into it -- give panic_store= another address";
    if (delta && overlaps(s, e, (uint64_t)(uintptr_t)__kimage_start, kend))
        return "KASLR placed the kernel on it";
    uint64_t is, ie;
    if (multiboot_get_info_range(&is, &ie) && overlaps(s, e, is, ie))
        return "GRUB put its info block on it";
    for (int i = 0; ; i++) {
        struct multiboot_module_info mod;
        if (!multiboot_get_module(i, &mod)) break;
        if (overlaps(s, e, mod.start, mod.end)) return "GRUB put a module on it";
    }
    return 0;
}

// WHAT IS THERE INSTEAD tells a firmware that clears RAM (zeroes) from a
// memory scrambler re-keyed by the reset (noise) from a bootloader that
// wrote over part of it (the magic survives).
static const char *what_is_there(const uint32_t *w, uint32_t magic) {
    if (w[0] == magic) return "magic intact, the rest changed";
    if (!w[0] && !w[1] && !w[2] && !w[3]) return "zeroes";
    if (w[0] == 0xFFFFFFFFu && w[1] == 0xFFFFFFFFu) return "all ones";
    return "other bytes -- overwritten, or scrambled by the reset";
}

// ---- the survey -----------------------------------------------------

#define SURVEY_MAGIC 0x31565350u   // "PSV1"

struct survey_stamp {
    uint32_t magic;
    uint32_t pad;
    uint64_t nonce;     // this boot's; the pattern is derived from it and the address
    uint64_t base;
};

static uint64_t pattern_word(uint64_t nonce, uint64_t base, uint64_t i) {
    uint64_t x = nonce ^ (base * 0x9E3779B97F4A7C15ULL) ^ (i + 1) * 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 31; x *= 0x94D049BB133111EBULL; x ^= x >> 29;
    return x;
}

#define SURVEY_WORDS ((PANIC_STORE_SIZE - sizeof(struct survey_stamp)) / 8)

// Changed words of the previous boot's stamp at `base`; -1 if there is none.
static int64_t survey_check(uint64_t base) {
    const struct survey_stamp *st = (const void *)(uintptr_t)base;
    if (st->magic != SURVEY_MAGIC || st->base != base) return -1;
    const uint64_t *w = (const uint64_t *)(st + 1);
    int64_t changed = 0;
    for (uint64_t i = 0; i < SURVEY_WORDS; i++)
        if (w[i] != pattern_word(st->nonce, base, i)) changed++;
    return changed;
}

static void survey_stamp(uint64_t base, uint64_t nonce) {
    struct survey_stamp *st = (void *)(uintptr_t)base;
    uint64_t *w = (uint64_t *)(st + 1);
    for (uint64_t i = 0; i < SURVEY_WORDS; i++) w[i] = pattern_word(nonce, base, i);
    st->nonce = nonce;
    st->base = base;
    st->pad = 0;
    st->magic = SURVEY_MAGIC;
}

static void survey(void) {
    uint64_t nonce = arch_rdtsc() | 1;   // differs per boot; only the next boot needs it
    char kept[PANIC_STORE_CANDIDATES * 12 + 1];
    size_t kn = 0;
    int stamped = 0, seen = 0;
    kept[0] = 0;
    klog_write("panic_store: SURVEY -- no store this boot; each range below was stamped "
               "by the last survey boot, and is stamped again now\n");
    for (int i = 0; i < PANIC_STORE_CANDIDATES; i++) {
        uint64_t b = g_candidates[i];
        const char *why = refusal(b);
        if (why) {
            klog_printf("panic_store: survey 0x%lx: not tried -- %s\n", (unsigned long)b, why);
            continue;
        }
        int64_t changed = survey_check(b);
        if (changed == 0) {
            klog_printf("panic_store: survey 0x%lx: KEPT\n", (unsigned long)b);
            if (kn < sizeof kept)
                kn += k_snprintf(kept + kn, sizeof kept - kn, " 0x%lx", (unsigned long)b);
            seen++;
        } else if (changed > 0) {
            klog_printf("panic_store: survey 0x%lx: changed -- %ld of %lu words differ\n",
                        (unsigned long)b, (long)changed, (unsigned long)SURVEY_WORDS);
            seen++;
        } else {
            klog_printf("panic_store: survey 0x%lx: no stamp (%s; first word 0x%x)\n",
                        (unsigned long)b, what_is_there((const void *)(uintptr_t)b, SURVEY_MAGIC),
                        *(const uint32_t *)(uintptr_t)b);
        }
        survey_stamp(b, nonce);
        g_kept_mask |= 1u << i;
        stamped++;
    }
    if (!seen)
        klog_printf("panic_store: survey -- %d range(s) stamped; nothing from an earlier "
                    "survey boot -- restart (with panic_store=survey still set) to read them\n", stamped);
    else if (kn)
        klog_printf(KLOG_WARN "panic_store: survey -- kept across the restart:%s; boot with "
                    "panic_store=<one of them>\n", kept);
    else
        klog_printf(KLOG_WARN "panic_store: survey -- NO range came back intact; this "
                    "machine's restart does not keep RAM\n");
}

void panic_store_probe(void) {
    uint64_t b;
    int m = mode(&b);
    if (m == PANIC_STORE_SURVEY) {
        survey();
        return;
    }
    if (m == PANIC_STORE_INVALID) {
        klog_printf("panic_store: NOT in use -- panic_store= is not \"survey\" or an address "
                    "(hex or a size, 4 KiB-aligned, 1 MiB..4 GiB); a panic will not be kept\n");
        return;
    }
    const char *why = refusal(b);
    if (why) {
        klog_printf("panic_store: NOT in use -- 0x%lx: %s; a panic will not be kept\n",
                    (unsigned long)b, why);
        return;
    }
    g_base = b;
    g_usable = 1;
    g_kept_mask = 1;
    void *base = (void *)(uintptr_t)g_base;
    int len = panic_record_check(base, PANIC_STORE_SIZE);
    if (len < 0) {
        const uint32_t *w = base;
        klog_printf("panic_store: %lu KiB at 0x%lx, nothing recovered (%s; first word 0x%x)\n",
                    (unsigned long)(PANIC_STORE_SIZE / 1024), (unsigned long)g_base,
                    w[0] == PANIC_RECORD_MAGIC ? "a DAMAGED record: magic intact, length or CRC wrong"
                    : (!w[0] && !w[1] && !w[2] && !w[3]) ? "no record (zeroes)"
                    : what_is_there(w, PANIC_RECORD_MAGIC), w[0]);
        return;
    }
    g_recovered = 1;
    const struct panic_record *r = base;
    klog_printf(KLOG_WARN "panic_store: the PREVIOUS BOOT PANICKED -- recovered %d bytes "
                "of its log (build %s, up %lus); logd files it with that boot\n",
                len, r->build, (unsigned long)(r->uptime_ns / 1000000000ull));
}

int panic_store_usable(void) { return g_usable; }

int panic_store_kept(int i, uint64_t *base) {
    if (i < 0 || i >= PANIC_STORE_CANDIDATES || !(g_kept_mask & (1u << i))) return 0;
    return panic_store_range(i, base);
}

// The newest bytes of the ring, straight into the store: no staging
// buffer, since a panic may be an out-of-memory one.
void panic_store_save(void) {
    if (!g_usable) return;
    char *text = PANIC_RECORD_TEXT((uintptr_t)g_base);
    uint32_t cap = (uint32_t)(PANIC_STORE_SIZE - PANIC_RECORD_HDR);
    uint32_t retained = klog_retained_bytes();
    uint64_t from = klog_total_bytes() - (retained < cap ? retained : cap);
    uint64_t first;
    uint32_t len = klog_read(from, text, cap, &first);
    panic_record_seal((void *)(uintptr_t)g_base, (uint32_t)PANIC_STORE_SIZE,
                      len, clocksource_now_ns(), SAUNAOS_VERSION_FULL);
    g_recovered = 0; // this boot's own record is the NEXT boot's to report
}

const char *panic_store_text(uint32_t *len, uint64_t *uptime_ns, const char **build) {
    if (!g_recovered) return 0;
    const struct panic_record *r = (const void *)(uintptr_t)g_base;
    if (len) *len = r->len;
    if (uptime_ns) *uptime_ns = r->uptime_ns;
    if (build) *build = r->build;
    return PANIC_RECORD_TEXT((uintptr_t)g_base);
}

void panic_store_clear(void) {
    if (!g_recovered) return;
    ((struct panic_record *)(uintptr_t)g_base)->magic = 0;
    g_recovered = 0;
    klog_write("panic_store: record cleared\n");
}

// ---- QUERY_PANIC ----------------------------------------------------

static int panic_count(void) {
    uint32_t len;
    if (!panic_store_text(&len, 0, 0)) return 0;
    return (int)((len + QUERY_PANIC_DATA - 1) / QUERY_PANIC_DATA);
}

static int panic_fill(int index, void *out) {
    uint32_t len;
    uint64_t up;
    const char *build;
    const char *text = panic_store_text(&len, &up, &build);
    if (!text || index < 0) return 0;
    uint32_t off = (uint32_t)index * QUERY_PANIC_DATA;
    if (off >= len) return 0;
    struct query_panic *q = out;
    q->total = len;
    q->len = len - off < QUERY_PANIC_DATA ? len - off : QUERY_PANIC_DATA;
    q->uptime_ns = up;
    k_strlcpy(q->build, build, sizeof q->build);
    k_memcpy(q->data, text + off, q->len);
    return 1;
}

static const struct query_provider panic_provider = {
    .cls = QUERY_PANIC,
    .name = "panic",
    .record_size = sizeof(struct query_panic),
    .flags = QUERY_F_LIST,
    .count = panic_count,
    .fill = panic_fill,
    .fields = NULL,
    .field_count = 0,
};

static void panic_query_init(void) { query_register(&panic_provider); }
INITCALL(panic_query_init, INIT_QUERY);
