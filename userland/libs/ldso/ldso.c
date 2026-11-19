// /lib/ld-toy.so -- the dynamic loader. The kernel maps the executable
// and this program, builds an auxv (abi/auxv.h), and enters HERE;
// everything dynamic happens in ring 3 (Linux's split). Eagerly, in
// this order: find the executable's PT_DYNAMIC through the auxv, load
// every DT_NEEDED library with Stage-0 mmap, lay out thread-local
// storage, apply every relocation, install the first thread's TLS, jump
// to AT_ENTRY. The libraries' constructors run LATER, when the
// program's libc asks through the vector (ldso_api.h's run_init).
//
// FREESTANDING BY NECESSITY, not taste: tolibc is what this program
// exists to load, and libsys's errno is __thread -- read through an %fs
// nothing has set up yet. So the handful of syscalls and string helpers
// below are private copies (every real libc's rtld carries the same,
// e.g. glibc's rtld private memcpy). Nothing here may include a header
// that drags either in.
//
// NOT ET_DYN, deliberately: this file links at ELF_LDSO_BASE
// (userland/libs/ldso/link.ld, kernel/include/kernel/elf.h -- the two must
// agree) as an ordinary fixed-base ET_EXEC, which is what deletes the
// loader-relocates-itself bootstrap entirely. docs/decisions.md has
// the reasoning; the cost is one reserved range no executable may grow
// into, which spawn guards.

#include <stdint.h>
#include <stddef.h>
#include "syscall_abi.h"
#include "toyabi.h"
#include "auxv.h"
#include "ldso_api.h"

// --- raw syscalls (rt/sys.c's inline, minus its __thread errno) ------

static inline int64_t sc3(uint64_t n, uint64_t a, uint64_t b, uint64_t c) {
    int64_t ret;
    __asm__ volatile ("syscall" : "=a"(ret)
                      : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return ret;
}
static inline int64_t sc1(uint64_t n, uint64_t a) { return sc3(n, a, 0, 0); }

static void ld_write(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    sc3(SYS_WRITE, 2, (uint64_t)(uintptr_t)s, len);
}

// Loudly, to fd 2 (the terminal, or the kernel log for a process with
// none): a loader that dies silently is a
// program that "crashed somewhere in libc" with no explanation.
// Decimal, into `out`, returning how many characters it wrote. The
// loader links no libc -- it IS what libc arrives through -- so there
// is no snprintf here and the alternative is printing the version as a
// word, which is what the message exists to avoid.
static int ld_u32(char *out, uint32_t v) {
    char tmp[10];
    int n = 0;
    do { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; } while (v);
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

// One line on stderr, the program carrying on.
static void warn(const char *what, const char *detail) {
    ld_write("ld-toy: ");
    ld_write(what);
    if (detail) { ld_write(": "); ld_write(detail); }
    ld_write("\n");
}

static void die(const char *what, const char *detail) {
    ld_write("ld-toy: ");
    ld_write(what);
    if (detail) { ld_write(": "); ld_write(detail); }
    ld_write("\n");
    sc1(SYS_EXIT, 127);
    for (;;) {}
}

static void *ld_mmap(uint64_t addr, uint64_t len, int prot, int flags,
                     int fd, uint64_t off) {
    struct mmap_msg m = { addr, len, prot, flags, fd, 0, off };
    int64_t r = sc1(SYS_MMAP, (uint64_t)(uintptr_t)&m);
    if (r < 0 && r >= -4095) return (void *)-1;
    return (void *)(uintptr_t)r;
}

static void ld_memset(void *d, int c, size_t n) {
    uint8_t *p = d;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)c;
}

static int ld_streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// --- just enough ELF -------------------------------------------------

struct ehdr64 {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};
struct phdr64 {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};
struct dyn64  { int64_t d_tag; uint64_t d_val; };
struct sym64  {
    uint32_t st_name;
    uint8_t  st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
};
struct rela64 { uint64_t r_offset, r_info; int64_t r_addend; };

#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_PHDR    6
#define PT_GNU_STACK 0x6474e551
#define PF_X       1

#define DT_NULL     0
#define DT_NEEDED   1
#define DT_PLTRELSZ 2
#define DT_HASH     4
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_INIT     12
#define DT_FINI     13
#define DT_JMPREL   23
#define DT_INIT_ARRAY   25
#define DT_FINI_ARRAY   26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28

#define R_X86_64_64        1
#define R_X86_64_GLOB_DAT  6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE  8

#define STB_WEAK 2

// --- the object table ------------------------------------------------

struct dobj {
    char name[64];          // "" for the executable
    uint64_t bias;          // load address minus link-time vaddr; 0 for the exe
    const struct dyn64 *dynamic;
    // From PT_DYNAMIC, already biased into pointers:
    const struct sym64 *symtab;
    const char *strtab;
    const uint32_t *hash;   // sysv: nbucket, nchain, buckets[], chains[]
    const struct rela64 *rela;   uint64_t relasz;
    const struct rela64 *jmprel; uint64_t pltrelsz;
    // Constructors and destructors, biased like the rest; 0 when absent.
    uint64_t init, fini;                         // DT_INIT / DT_FINI
    const uint64_t *init_array; uint64_t init_arraysz;
    const uint64_t *fini_array; uint64_t fini_arraysz;
    int init_state;   // INIT_NONE, INIT_RUNNING, INIT_DONE, INIT_FINISHED
    int init_seq;     // the order its initialisers ran in, from 1
    // Its program headers, mapped -- for dl_iterate_phdr. NULL when no
    // loaded segment holds them, and the object is then not reported.
    const struct phdr64 *phdr;
    uint16_t phnum;
    // Its PT_TLS, if it has one: the template (biased), its sizes, and
    // its static offset below tp -- 0 for a module dlopen()ed after the
    // layout, whose blocks are mapped per thread on first use.
    uint64_t tls_image, tls_filesz, tls_memsz, tls_align, tls_offset;
    // Loaded by a dlopen() that then failed: it stays mapped (nothing is
    // unloaded) but is never a handle, a dependency or a definition again.
    int failed;
};

enum { INIT_NONE, INIT_RUNNING, INIT_DONE, INIT_FINISHED };

// THE TABLE GROWS IN BLOCKS AND NEVER MOVES: a dlopen() handle IS a
// `struct dobj *`, so a realloc'd array would leave every handle a
// program holds pointing at freed memory. The first block is static --
// a program's own libraries need nothing allocated -- and each further
// one is an anonymous mapping, since there is no malloc in here (libc is
// one of the things being loaded). It held 8, then a fixed 24.
#define OBJS_PER_BLOCK 24

struct dobj_block {
    struct dobj o[OBJS_PER_BLOCK];
    struct dobj_block *next;   // NULL until a row past this block is asked for
};

static struct dobj_block g_block0;
static int g_nobjs;

// Row `i`, which must be below g_nobjs.
static struct dobj *obj(int i) {
    struct dobj_block *b = &g_block0;
    for (; i >= OBJS_PER_BLOCK; i -= OBJS_PER_BLOCK) b = b->next;
    return &b->o[i];
}

static void *ld_mmap(uint64_t addr, uint64_t len, int prot, int flags,
                     int fd, uint64_t off);

// The row past the end, its storage mapped, or NULL when no page could
// be. NOT counted yet: obj_publish() does that once the row is filled, so
// a thread reading g_nobjs (tls_alloc, iterate_phdr) never sees a row
// half-written by a dlopen() in another.
static struct dobj *obj_append(void) {
    struct dobj_block *b = &g_block0;
    int i = g_nobjs;
    for (; i >= OBJS_PER_BLOCK; i -= OBJS_PER_BLOCK) {
        if (!b->next) {
            uint64_t len = (sizeof *b + 4095) & ~4095ULL;
            void *m = ld_mmap(0, len, SYS_PROT_READ | SYS_PROT_WRITE,
                              SYS_MAP_PRIVATE | SYS_MAP_ANONYMOUS, -1, 0);
            if (m == (void *)-1) return 0;
            b->next = m;   // zero-filled: its own `next` is NULL
        }
        b = b->next;
    }
    return &b->o[i];
}

static void obj_publish(void) {
    __atomic_store_n(&g_nobjs, g_nobjs + 1, __ATOMIC_RELEASE);
}

// The row count as another thread may read it: rows are complete to it.
static int nobjs_now(void) { return __atomic_load_n(&g_nobjs, __ATOMIC_ACQUIRE); }

// A LOAD FAILURE IS FATAL AT STARTUP AND A RETURN VALUE FOR dlopen.
// There is no program without the libraries it links against, so the
// startup path still dies with a sentence; a plugin that will not load
// is the caller's problem to report, so dlopen records the reason and
// answers NULL. `g_nonfatal` is set only while a dlopen is running,
// which is single-threaded here.
static char g_dlerr[192];
static int  g_exec_stack;   // the executable's PT_GNU_STACK said PF_X
static uint64_t g_canary;   // ldso_seed_canary()'s, for every thread's TCB
extern uintptr_t __stack_chk_guard;
static int  g_nonfatal;
static int  g_reloc_failed;

static void ld_append(char *d, const char *s, size_t cap) {
    size_t n = 0;
    while (d[n]) n++;
    while (*s && n + 1 < cap) d[n++] = *s++;
    d[n] = 0;
}

// The fd has to go back even on the failure path, because dlopen's
// caller carries on running.
static int close_fail(int fd, const char *what, const char *detail);

static int ld_fail(const char *what, const char *detail) {
    if (!g_nonfatal) die(what, detail);
    g_dlerr[0] = 0;
    ld_append(g_dlerr, what, sizeof g_dlerr);
    if (detail) {
        ld_append(g_dlerr, ": ", sizeof g_dlerr);
        ld_append(g_dlerr, detail, sizeof g_dlerr);
    }
    return -1;
}

static uint32_t elf_hash(const char *name) {
    uint32_t h = 0, g;
    while (*name) {
        h = (h << 4) + (uint8_t)*name++;
        g = h & 0xF0000000u;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

// One object's definition of `name`, or 0. An UNDEF entry is that
// object importing the name, not defining it -- skipping those is what
// stops a lookup answering "the exe references it" for "it exists".
static const struct sym64 *lookup_in(const struct dobj *o, const char *name) {
    if (o->failed || !o->hash || !o->symtab || !o->strtab) return 0;
    uint32_t nbucket = o->hash[0];
    if (!nbucket) return 0;
    for (uint32_t i = o->hash[2 + elf_hash(name) % nbucket]; i;
         i = o->hash[2 + nbucket + i]) {
        const struct sym64 *s = &o->symtab[i];
        if (s->st_shndx == 0) continue; // SHN_UNDEF: an import
        if (ld_streq(o->strtab + s->st_name, name)) return s;
    }
    return 0;
}

// Search order: the executable first, then each library in load order
// -- so a definition in the program wins, which is what lets libc.so
// call back into symbols the executable exports.
static uint64_t resolve(const char *name, int weak) {
    for (int i = 0; i < g_nobjs; i++) {
        const struct sym64 *s = lookup_in(obj(i), name);
        if (s) return obj(i)->bias + s->st_value;
    }
    if (weak) return 0;
    // Inside a dlopen this must not kill the caller: the plugin is
    // simply unusable, and dl_open() turns the flag into a NULL.
    if (g_nonfatal) { g_reloc_failed = 1; ld_fail("undefined symbol", name); return 0; }
    die("undefined symbol", name);
    return 0;
}

static int close_fail(int fd, const char *what, const char *detail) {
    sc1(SYS_CLOSE, (uint64_t)fd);
    return ld_fail(what, detail);
}

// --- parsing one object's PT_DYNAMIC ---------------------------------

static void parse_dynamic(struct dobj *o) {
    for (const struct dyn64 *d = o->dynamic; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_SYMTAB:   o->symtab = (const struct sym64 *)(uintptr_t)(o->bias + d->d_val); break;
        case DT_STRTAB:   o->strtab = (const char *)(uintptr_t)(o->bias + d->d_val); break;
        case DT_HASH:     o->hash   = (const uint32_t *)(uintptr_t)(o->bias + d->d_val); break;
        case DT_RELA:     o->rela   = (const struct rela64 *)(uintptr_t)(o->bias + d->d_val); break;
        case DT_RELASZ:   o->relasz = d->d_val; break;
        case DT_JMPREL:   o->jmprel = (const struct rela64 *)(uintptr_t)(o->bias + d->d_val); break;
        case DT_PLTRELSZ: o->pltrelsz = d->d_val; break;
        case DT_INIT:     o->init = o->bias + d->d_val; break;
        case DT_FINI:     o->fini = o->bias + d->d_val; break;
        case DT_INIT_ARRAY: o->init_array = (const uint64_t *)(uintptr_t)(o->bias + d->d_val); break;
        case DT_FINI_ARRAY: o->fini_array = (const uint64_t *)(uintptr_t)(o->bias + d->d_val); break;
        case DT_INIT_ARRAYSZ: o->init_arraysz = d->d_val; break;
        case DT_FINI_ARRAYSZ: o->fini_arraysz = d->d_val; break;
        }
    }
}

// --- loading one library ---------------------------------------------

static int64_t pread_all(int fd, void *buf, uint64_t n, uint64_t off) {
    if (sc3(SYS_LSEEK, (uint64_t)fd, off, SYS_SEEK_SET) < 0) return -1;
    uint8_t *p = buf;
    uint64_t got = 0;
    while (got < n) {
        int64_t r = sc3(SYS_READ, (uint64_t)fd, (uint64_t)(uintptr_t)(p + got),
                        n - got);
        if (r <= 0) break;
        got += (uint64_t)r;
    }
    return (int64_t)got;
}

#define PAGE 4096ULL
#define ALIGN_DN(x) ((x) & ~(PAGE - 1))
#define ALIGN_UP(x) (((x) + PAGE - 1) & ~(PAGE - 1))

// The header block read per library. Static because this program is
// single-threaded by construction and a 4 KiB frame would be most of a
// ring-3 stack page.
static uint8_t g_hdr[4096];

// A BARE NAME IS LOOKED FOR IN /lib; ANYTHING WITH A '/' IS TAKEN AS
// GIVEN. That is the DT_NEEDED rule, and dlopen("/lib/snd/hda.so")
// wants the other half. Returns 0, or -1 with the reason recorded (see
// ld_fail: fatal at startup, a return value inside dlopen).
static void note_tls(struct dobj *o, const struct phdr64 *ph, uint64_t phnum);

static int load_object(const char *name) {
    for (int i = 0; i < g_nobjs; i++)
        if (ld_streq(obj(i)->name, name))   // already loaded -- or already refused
            return obj(i)->failed ? ld_fail("an earlier dlopen() of it failed", name) : 0;

    char path[96];
    {
        int absolute = 0;
        for (const char *p = name; *p; p++) if (*p == '/') absolute = 1;
        size_t n = 0;
        if (!absolute) for (const char *p = "/lib/"; *p; p++) path[n++] = *p;
        for (const char *p = name; *p; p++) {
            if (n + 1 >= sizeof path) return ld_fail("library name too long", name);
            path[n++] = *p;
        }
        path[n] = 0;
    }

    int fd = (int)sc3(SYS_OPEN, (uint64_t)(uintptr_t)path, 0, 0);
    if (fd < 0) return ld_fail("library not found", path);
    if (pread_all(fd, g_hdr, sizeof g_hdr, 0) < (int64_t)sizeof(struct ehdr64)) {
        sc1(SYS_CLOSE, (uint64_t)fd);
        return ld_fail("short read", path);
    }

    const struct ehdr64 *eh = (const struct ehdr64 *)g_hdr;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F' ||
        eh->e_type != 3 /* ET_DYN */)
        return close_fail(fd, "not a shared object", path);
    if (eh->e_phentsize != sizeof(struct phdr64) ||
        eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(struct phdr64) > sizeof g_hdr)
        return close_fail(fd, "program headers out of reach", path);
    const struct phdr64 *ph = (const struct phdr64 *)(g_hdr + eh->e_phoff);

    // The span every PT_LOAD needs, as one reservation -- then carve
    // each segment into it with MAP_FIXED. mmap refuses overlap, so
    // the reservation is unmapped first; single-threaded, so nothing
    // can take the hole in between (docs/decisions.md).
    uint64_t lo = ~0ULL, hi = 0, dyn_vaddr = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) dyn_vaddr = ph[i].p_vaddr;
        if (ph[i].p_type != PT_LOAD || !ph[i].p_memsz) continue;
        if (ALIGN_DN(ph[i].p_vaddr) < lo) lo = ALIGN_DN(ph[i].p_vaddr);
        if (ALIGN_UP(ph[i].p_vaddr + ph[i].p_memsz) > hi)
            hi = ALIGN_UP(ph[i].p_vaddr + ph[i].p_memsz);
    }
    if (lo >= hi || !dyn_vaddr) return close_fail(fd, "no loadable segments", path);
    // A library that builds code on the stack runs only in a process whose
    // stack is executable, which the EXECUTABLE decides at exec: nothing
    // later may make a stack RWX. A dlopen() of one is refused (glibc
    // 2.41's rule); one the program NEEDS still loads, said once on
    // stderr -- the flag often means only an object with no
    // .note.GNU-stack, and refusing would stop the program for it.
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_GNU_STACK || !(ph[i].p_flags & PF_X) || g_exec_stack) continue;
        static const char why[] = "needs an executable stack (PT_GNU_STACK RWX), and "
                                  "this program's is not -- rebuild it with -ftrampoline-impl=heap";
        if (g_nonfatal) return close_fail(fd, why, path);
        warn(path, why);
    }

    void *resv = ld_mmap(0, hi - lo, SYS_PROT_READ,
                         SYS_MAP_PRIVATE | SYS_MAP_ANONYMOUS, -1, 0);
    if (resv == (void *)-1) return close_fail(fd, "out of address space", path);
    uint64_t bias = (uint64_t)(uintptr_t)resv - lo;
    sc3(SYS_MUNMAP, (uint64_t)(uintptr_t)resv, hi - lo, 0);

    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || !ph[i].p_memsz) continue;
        int prot = SYS_PROT_READ;
        if (ph[i].p_flags & 2) prot |= SYS_PROT_WRITE;
        if (ph[i].p_flags & 1) prot |= SYS_PROT_EXEC;

        // p_vaddr and p_offset are congruent mod 4096 (the .so links
        // with -z max-page-size=4096; a lib built without it fails
        // here, by name).
        if ((ph[i].p_vaddr & (PAGE - 1)) != (ph[i].p_offset & (PAGE - 1)))
            return close_fail(fd, "segment not page-congruent "
                              "(relink with -z max-page-size=4096)", path);

        uint64_t vpage = bias + ALIGN_DN(ph[i].p_vaddr);
        uint64_t fend  = bias + ph[i].p_vaddr + ph[i].p_filesz;
        uint64_t mend  = bias + ph[i].p_vaddr + ph[i].p_memsz;

        if (ph[i].p_filesz) {
            if (ld_mmap(vpage, ALIGN_UP(fend) - vpage, prot,
                        SYS_MAP_PRIVATE | SYS_MAP_FIXED, fd,
                        ALIGN_DN(ph[i].p_offset)) == (void *)-1)
                return close_fail(fd, "segment map failed", path);
        }
        if (ALIGN_UP(mend) > ALIGN_UP(fend)) {
            if (ld_mmap(ALIGN_UP(fend), ALIGN_UP(mend) - ALIGN_UP(fend), prot,
                        SYS_MAP_PRIVATE | SYS_MAP_ANONYMOUS | SYS_MAP_FIXED,
                        -1, 0) == (void *)-1)
                return close_fail(fd, "bss map failed", path);
        }
        // The tail of the last file-backed page holds whatever the
        // FILE holds there; .bss expects zeros. Only a writable
        // segment can have one (text has filesz == memsz).
        if (mend > fend && (prot & SYS_PROT_WRITE) && (fend & (PAGE - 1)))
            ld_memset((void *)(uintptr_t)fend, 0,
                      ALIGN_UP(fend) - fend);
    }
    sc1(SYS_CLOSE, (uint64_t)fd);

    // Where its program headers landed, for dl_iterate_phdr: PT_PHDR if
    // it says, else the PT_LOAD whose file bytes cover e_phoff (an
    // `ld -shared` output maps its headers with its first segment).
    // g_hdr is reused by the next load, so nothing may point into it.
    const struct phdr64 *mapped_ph = 0;
    uint64_t phbytes = (uint64_t)eh->e_phnum * sizeof(struct phdr64);
    for (int i = 0; i < eh->e_phnum && !mapped_ph; i++)
        if (ph[i].p_type == PT_PHDR)
            mapped_ph = (const struct phdr64 *)(uintptr_t)(bias + ph[i].p_vaddr);
    for (int i = 0; i < eh->e_phnum && !mapped_ph; i++)
        if (ph[i].p_type == PT_LOAD && ph[i].p_offset <= eh->e_phoff &&
            eh->e_phoff + phbytes <= ph[i].p_offset + ph[i].p_filesz)
            mapped_ph = (const struct phdr64 *)(uintptr_t)
                (bias + ph[i].p_vaddr + (eh->e_phoff - ph[i].p_offset));
    uint16_t phnum = eh->e_phnum;

    struct dobj *o = obj_append();
    if (!o) return ld_fail("no memory for another library", name);
    ld_memset(o, 0, sizeof *o);
    for (size_t i = 0; name[i] && i + 1 < sizeof o->name; i++) o->name[i] = name[i];
    o->bias = bias;
    o->dynamic = (const struct dyn64 *)(uintptr_t)(bias + dyn_vaddr);
    o->phdr = mapped_ph;
    o->phnum = mapped_ph ? phnum : 0;
    note_tls(o, ph, phnum);            // ph is still this object's g_hdr
    // A block past a page is one no mapping here aligns -- refused at
    // startup and at dlopen() alike (tls_layout() says it once more).
    if (o->tls_align > PAGE) return ld_fail("thread-local data aligned past a page", name);
    parse_dynamic(o);
    obj_publish();
    return 0;
}

// --- constructors and destructors ------------------------------------
//
// A LIBRARY'S DEPENDENCIES ARE INITIALISED BEFORE IT, by a depth-first
// walk of DT_NEEDED -- not by load order, which is breadth-first: a
// program needing libA then libB, where libB needs libA, loads libA
// first, but reversing that order would run libB's constructors before
// libA's. glibc sorts its maps by dependency for the same reason. The
// EXECUTABLE's own arrays are not run here (start.c in libc runs them,
// after these).
//
// Run when libc asks (ldso_api.h's run_init), not before the jump to
// AT_ENTRY: a constructor may touch errno, which is __thread, and the
// executable's crt0 is what installs the TLS block.
typedef void (*init_fn)(int argc, char **argv, char **envp);
typedef void (*fini_fn)(void);

static int g_init_seq;          // objects initialised so far
static int g_started;           // run_init has been called: dlopen inits too
static int g_argc;
static char **g_argv, **g_envp;

static struct dobj *obj_named(const char *name) {
    for (int i = 0; i < g_nobjs; i++)
        if (ld_streq(obj(i)->name, name)) return obj(i);
    return 0;
}

static void init_deps(struct dobj *o);

// A cycle (two libraries needing each other) is cut where the walk
// re-enters an object it is still initialising; no order is right for
// such a pair.
static void init_obj(struct dobj *o) {
    if (o->init_state != INIT_NONE) return;
    o->init_state = INIT_RUNNING;
    init_deps(o);
    if (o->init) ((init_fn)(uintptr_t)o->init)(g_argc, g_argv, g_envp);
    for (uint64_t i = 0; o->init_array && i < o->init_arraysz / 8; i++) {
        uint64_t f = o->init_array[i];
        if (f && f != ~0ULL) ((init_fn)(uintptr_t)f)(g_argc, g_argv, g_envp);
    }
    o->init_state = INIT_DONE;
    o->init_seq = ++g_init_seq;
}

static void init_deps(struct dobj *o) {
    for (const struct dyn64 *d = o->dynamic; d->d_tag != DT_NULL; d++) {
        if (d->d_tag != DT_NEEDED) continue;
        struct dobj *dep = obj_named(o->strtab + d->d_val);
        if (dep) init_obj(dep);
    }
}

static void run_init(int argc, char **argv, char **envp) {
    g_argc = argc; g_argv = argv; g_envp = envp;
    g_started = 1;
    obj(0)->init_state = INIT_DONE;   // the executable: libc runs its arrays
    init_deps(obj(0));
    for (int i = 1; i < g_nobjs; i++) init_obj(obj(i));
}

// The reverse of initialisation, a dlopen'd library's included: each
// library's DT_FINI_ARRAY last entry first, then its DT_FINI.
static void run_fini(void) {
    for (int seq = g_init_seq; seq > 0; seq--) {
        for (int i = 1; i < g_nobjs; i++) {
            struct dobj *o = obj(i);
            if (o->init_seq != seq || o->init_state != INIT_DONE) continue;
            o->init_state = INIT_FINISHED;
            for (uint64_t k = o->fini_array ? o->fini_arraysz / 8 : 0; k > 0; k--) {
                uint64_t f = o->fini_array[k - 1];
                if (f && f != ~0ULL) ((fini_fn)(uintptr_t)f)();
            }
            if (o->fini) ((fini_fn)(uintptr_t)o->fini)();
        }
    }
}

// --- dl_iterate_phdr ---------------------------------------------------

static const uint64_t *current_dtv(void);

static int iterate_phdr(int (*cb)(struct ldso_phdr_info *, uint64_t, void *), void *data) {
    int n = nobjs_now();
    for (int i = 0; i < n; i++) {
        const struct dobj *o = obj(i);
        if (!o->phdr) continue;
        struct ldso_phdr_info info;
        ld_memset(&info, 0, sizeof info);
        info.addr  = o->bias;
        info.name  = o->name;
        info.phdr  = o->phdr;
        info.phnum = o->phnum;
        info.adds  = (uint64_t)g_nobjs;   // nothing is ever unloaded
        if (o->tls_memsz) {
            // glibc's meaning: THIS thread's block, or NULL before its
            // first use of a dlopen()ed module -- never the template.
            const uint64_t *dtv = current_dtv();
            info.tls_modid = (uint64_t)i + 1;
            info.tls_data = dtv && (uint64_t)i + 1 <= dtv[0] ? (void *)(uintptr_t)dtv[i + 1] : 0;
        }
        int r = cb(&info, sizeof info, data);
        if (r) return r;
    }
    return 0;
}

// --- thread-local storage ----------------------------------------------
//
// THE x86-64 psABI's VARIANT II, WITH A DTV, glibc's and musl's shape.
// Every object with a PT_TLS is a MODULE, numbered by its row + 1 (sparse:
// a row without TLS wastes one DTV slot, and nothing else). Each thread's
// area is one anonymous mapping:
//
//     +-- static TLS --------------------------+-- TCB --+-- DTV --------+
//     | ...lib 2's block | lib 1 | the exe     | self,dtv| cap, m1, m2...|
//     +----------------------------------------+---------+---------------+
//                                              ^ tp = %fs.base
//
// THE STATIC PART IS EVERY MODULE LOADED AT STARTUP, the executable
// nearest tp -- exactly where its local-exec offsets put it, so rt/tls.c's
// layout and this one agree for a program with no TLS library. A module
// dlopen()ed later has no static offset: its block is carved the first
// time a thread asks __tls_get_addr() for it (libc's fast path misses,
// and calls tls_get_addr below), and a library that reaches its TLS
// initial-exec (R_X86_64_TPOFF64) cannot be dlopen()ed -- glibc's static
// surplus, which this does not keep, is the only way that works.
//
// THE DTV GROWS: a thread created before a dlopen has a table too short
// for the new module, and the slow path gives it a longer one. Grown
// tables and dynamic blocks come from ONE per-thread arena of chained
// mappings (tcb[2] the newest, tcb[3]/tcb[4] its free span), so a thread
// costs a mapping or two, not one per block; tls_free() unmaps the chain.
//
// dlopen() publishes a row only once it is filled (obj_publish()), and
// rows never move, so a thread reading the count here sees whole rows.

#define PT_TLS 7
#define STT_TLS 6
#define R_X86_64_DTPMOD64 16
#define R_X86_64_DTPOFF64 17
#define R_X86_64_TPOFF64  18
#define R_X86_64_TLSDESC  36

// The TCB's words, rt/tls.c's 64 bytes: tp itself, the live DTV, the
// arena's newest chunk and its free span, and the area's own length.
enum { TCB_SELF = TCB_WORD_SELF, TCB_DTV = TCB_WORD_DTV, TCB_CHUNK, TCB_BUMP, TCB_END,
       TCB_CANARY = TCB_WORD_CANARY, TCB_AREA_LEN = 7, TCB_WORDS = 8 };
#define ARENA_CHUNK_MIN (4 * PAGE)   // a step, not a cap: a bigger block gets a bigger chunk

static uint64_t g_tls_static;  // bytes of static TLS, a multiple of g_tls_align
static uint64_t g_tls_align = 16;

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

// Records an object's PT_TLS (biased), from its program headers.
static void note_tls(struct dobj *o, const struct phdr64 *ph, uint64_t phnum) {
    for (uint64_t i = 0; i < phnum; i++) {
        if (ph[i].p_type != PT_TLS || !ph[i].p_memsz) continue;
        o->tls_image  = o->bias + ph[i].p_vaddr;
        o->tls_filesz = ph[i].p_filesz;
        o->tls_memsz  = ph[i].p_memsz;
        o->tls_align  = ph[i].p_align ? ph[i].p_align : 1;
    }
}

// Fixes the static offsets of every module loaded so far. Called once,
// after startup loading and before any relocation reads an offset.
static void tls_layout(void) {
    uint64_t off = 0;
    for (int i = 0; i < g_nobjs; i++) {
        struct dobj *o = obj(i);
        if (!o->tls_memsz) continue;
        off = align_up(off + o->tls_memsz, o->tls_align);
        o->tls_offset = off;
        if (o->tls_align > g_tls_align) g_tls_align = o->tls_align;
    }
    g_tls_static = align_up(off, g_tls_align);
}

static void ld_memcpy(void *d, const void *s, uint64_t n) {
    uint8_t *dd = d; const uint8_t *ss = s;
    while (n--) *dd++ = *ss++;
}

// The module numbered `modid`, or 0.
static struct dobj *tls_module(uint64_t modid) {
    if (!modid || modid > (uint64_t)nobjs_now()) return 0;
    struct dobj *o = obj((int)modid - 1);
    return o->tls_memsz ? o : 0;
}

static uint64_t *current_tcb(void) {
    uint64_t *tcb;
    __asm__ volatile ("mov %%fs:0, %0" : "=r"(tcb));
    return tcb;
}

static const uint64_t *current_dtv(void) {
    return (const uint64_t *)(uintptr_t)current_tcb()[TCB_DTV];
}

// `size` zeroed bytes aligned to `align` (at most a page) from the
// calling thread's arena, or 0 with no memory. A chunk starts with the
// link to the one before it, for tls_free.
static void *arena_alloc(uint64_t *tcb, uint64_t size, uint64_t align) {
    uint64_t p = align_up(tcb[TCB_BUMP], align);
    if (!tcb[TCB_CHUNK] || p + size > tcb[TCB_END]) {
        uint64_t len = ALIGN_UP(16 + align + size);
        if (len < ARENA_CHUNK_MIN) len = ARENA_CHUNK_MIN;
        uint64_t *c = ld_mmap(0, len, SYS_PROT_READ | SYS_PROT_WRITE,
                              SYS_MAP_PRIVATE | SYS_MAP_ANONYMOUS, -1, 0);
        if (c == (void *)-1) return 0;
        c[0] = tcb[TCB_CHUNK];
        c[1] = len;
        tcb[TCB_CHUNK] = (uint64_t)(uintptr_t)c;
        tcb[TCB_END] = (uint64_t)(uintptr_t)c + len;
        p = align_up((uint64_t)(uintptr_t)c + 16, align);
    }
    tcb[TCB_BUMP] = p + size;
    return (void *)(uintptr_t)p;
}

// One thread's area: static blocks from their templates, the TCB, and a
// DTV sized to every module loaded now. Returns tp, or 0 with no memory.
static void *tls_alloc(void) {
    int n = nobjs_now();
    uint64_t len = ALIGN_UP(g_tls_static + TCB_WORDS * 8 + ((uint64_t)n + 1) * 8);
    void *m = ld_mmap(0, len, SYS_PROT_READ | SYS_PROT_WRITE,
                      SYS_MAP_PRIVATE | SYS_MAP_ANONYMOUS, -1, 0);
    if (m == (void *)-1) return 0;
    uint8_t *tp = (uint8_t *)m + g_tls_static;
    uint64_t *tcb = (uint64_t *)tp;
    uint64_t *dtv = tcb + TCB_WORDS;
    dtv[0] = (uint64_t)n;
    for (int i = 0; i < n; i++) {
        struct dobj *o = obj(i);
        if (!o->tls_memsz || !o->tls_offset) continue;   // none, or dynamic
        uint8_t *block = tp - o->tls_offset;
        ld_memcpy(block, (const void *)(uintptr_t)o->tls_image, o->tls_filesz);
        dtv[i + 1] = (uint64_t)(uintptr_t)block;          // the rest is zero already
    }
    tcb[TCB_SELF] = (uint64_t)(uintptr_t)tp;              // %fs:0 reads tp itself
    tcb[TCB_CANARY] = g_canary;                           // %fs:0x28, every thread's
    tcb[TCB_DTV] = (uint64_t)(uintptr_t)dtv;
    tcb[TCB_AREA_LEN] = len;
    return tp;
}

// Gives back a thread's area and its arena. `tp` must not be the
// caller's own (a thread frees another's: at join, or once detached and
// exited).
static void tls_free(void *tpv) {
    if (!tpv) return;
    uint64_t *tcb = (uint64_t *)tpv;
    for (uint64_t c = tcb[TCB_CHUNK]; c; ) {
        uint64_t next = ((uint64_t *)(uintptr_t)c)[0];
        sc3(SYS_MUNMAP, c, ((uint64_t *)(uintptr_t)c)[1], 0);
        c = next;
    }
    sc3(SYS_MUNMAP, (uint64_t)(uintptr_t)tpv - g_tls_static, tcb[TCB_AREA_LEN], 0);
}

// __tls_get_addr()'s slow path, for the CALLING thread: the DTV grown to
// cover every module, and module `modid`'s block carved from its template.
static void *tls_get_addr(uint64_t modid, uint64_t off) {
    struct dobj *o = tls_module(modid);
    if (!o) die("__tls_get_addr: no such TLS module", 0);
    uint64_t *tcb = current_tcb();
    uint64_t *dtv = (uint64_t *)(uintptr_t)tcb[TCB_DTV];
    if (modid > dtv[0]) {
        uint64_t cap = (uint64_t)nobjs_now();
        uint64_t *nd = arena_alloc(tcb, (cap + 1) * 8, 8);
        if (!nd) die("__tls_get_addr: no memory for a longer DTV", o->name);
        ld_memcpy(nd + 1, dtv + 1, dtv[0] * 8);
        nd[0] = cap;
        tcb[TCB_DTV] = (uint64_t)(uintptr_t)nd;   // the old one stays in the area or arena
        dtv = nd;
    }
    if (!dtv[modid]) {
        uint8_t *b = arena_alloc(tcb, o->tls_memsz, o->tls_align);
        if (!b) die("__tls_get_addr: no memory for a TLS block", o->name);
        ld_memcpy(b, (const void *)(uintptr_t)o->tls_image, o->tls_filesz);
        dtv[modid] = (uint64_t)(uintptr_t)b;
    }
    return (uint8_t *)(uintptr_t)dtv[modid] + off;
}

// The row `o` is, for a relocation naming its own module.
static int row_of(const struct dobj *o) {
    for (int i = 0; i < g_nobjs; i++) if (obj(i) == o) return i;
    return 0;
}

// --- relocation ------------------------------------------------------

static void apply_rela(const struct dobj *o, const struct rela64 *r,
                       uint64_t bytes) {
    for (uint64_t n = 0; n < bytes / sizeof *r; n++, r++) {
        uint32_t type = (uint32_t)r->r_info;
        uint32_t symi = (uint32_t)(r->r_info >> 32);
        uint64_t *where = (uint64_t *)(uintptr_t)(o->bias + r->r_offset);
        switch (type) {
        case R_X86_64_RELATIVE:
            *where = o->bias + (uint64_t)r->r_addend;
            break;
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT:
        case R_X86_64_64: {
            const struct sym64 *s = &o->symtab[symi];
            const char *nm = o->strtab + s->st_name;
            uint64_t v;
            // A DEFINED entry still resolves by NAME (interposition:
            // the exe's definition outranks the lib's own), except
            // that a lookup miss falls back to the local definition.
            v = 0;
            if (s->st_shndx != 0 || (s->st_info >> 4) == STB_WEAK) {
                for (int i = 0; i < g_nobjs; i++) {
                    const struct sym64 *d = lookup_in(obj(i), nm);
                    if (d) { v = obj(i)->bias + d->st_value; break; }
                }
                if (!v && s->st_shndx != 0) v = o->bias + s->st_value;
            } else {
                v = resolve(nm, (s->st_info >> 4) == STB_WEAK);
            }
            *where = v + (type == R_X86_64_64 ? (uint64_t)r->r_addend : 0);
            break;
        }
        case R_X86_64_DTPMOD64:
        case R_X86_64_DTPOFF64:
        case R_X86_64_TPOFF64: {
            // THE DEFINING MODULE, not an address: a TLS symbol's value
            // is an offset into its module's block, so the bias is never
            // added. Interposition as for the rest: by name, exe first.
            int def = -1;
            uint64_t sv = 0;
            if (symi) {
                const struct sym64 *s = &o->symtab[symi];
                const char *nm = o->strtab + s->st_name;
                for (int i = 0; i < g_nobjs && def < 0; i++) {
                    const struct sym64 *d = lookup_in(obj(i), nm);
                    if (d && (d->st_info & 0xF) == STT_TLS) { def = i; sv = d->st_value; }
                }
                if (def < 0 && s->st_shndx != 0) { def = row_of(o); sv = s->st_value; }
                if (def < 0) {
                    if (g_nonfatal) { g_reloc_failed = 1; ld_fail("undefined thread-local symbol", nm); }
                    else die("undefined thread-local symbol", nm);
                    break;
                }
            } else {
                def = row_of(o);              // the module's own, local-dynamic
            }
            const struct dobj *m = obj(def);
            if (type == R_X86_64_DTPMOD64) {
                *where = (uint64_t)def + 1;
            } else if (type == R_X86_64_DTPOFF64) {
                *where = sv + (uint64_t)r->r_addend;
            } else if (!m->tls_offset) {
                // INITIAL-EXEC NEEDS A STATIC OFFSET, which a module
                // loaded after startup does not have (glibc keeps a
                // surplus for it; this loader does not).
                g_reloc_failed = 1;
                ld_fail("initial-exec TLS in a library loaded after startup "
                        "(rebuild it -ftls-model=global-dynamic)",
                        o->name[0] ? o->name : "the executable");
            } else {
                *where = sv + (uint64_t)r->r_addend - m->tls_offset;
            }
            break;
        }
        case R_X86_64_TLSDESC:
            die("TLS descriptors are not supported (rebuild with -mtls-dialect=gnu)",
                o->name[0] ? o->name : "the executable");
            break;
        default:
            die("unsupported relocation type",
                o->name[0] ? o->name : "the executable");
        }
    }
}

// --- entry -----------------------------------------------------------

// --- dlopen, and the vector libc reaches it through ------------------

// LOAD, THEN RELOCATE ONLY WHAT IS NEW. Everything already in the table
// was relocated at startup, and applying a relocation twice writes the
// same answer -- but a plugin's DT_NEEDED may pull in a library nobody
// had, and that one has to be relocated before anything calls it.
static void *dl_open(const char *path, int flags) {
    (void)flags; // RTLD_LAZY is accepted and ignored: binding is eager
    if (!path) { ld_fail("dlopen(NULL)", 0); return 0; }

    g_dlerr[0] = 0;
    g_nonfatal = 1;
    g_reloc_failed = 0;
    int first = g_nobjs;
    void *handle = 0;

    if (load_object(path) == 0) {
        // The plugin was already loaded: hand back the same object
        // rather than a second handle onto it.
        if (g_nobjs == first) {
            for (int i = 0; i < g_nobjs; i++)
                if (ld_streq(obj(i)->name, path)) handle = obj(i);
        } else {
            int ok = 1;
            for (int i = first; i < g_nobjs && ok; i++)
                for (const struct dyn64 *d = obj(i)->dynamic;
                     d->d_tag != DT_NULL; d++)
                    if (d->d_tag == DT_NEEDED &&
                        load_object(obj(i)->strtab + d->d_val) != 0) { ok = 0; break; }
            if (ok) {
                for (int i = first; i < g_nobjs; i++) {
                    const struct dobj *o = obj(i);
                    if (o->rela)   apply_rela(o, o->rela, o->relasz);
                    if (o->jmprel) apply_rela(o, o->jmprel, o->pltrelsz);
                }
                // AN UNRESOLVED SYMBOL IS A FAILED dlopen, not a
                // half-loaded one the caller can still call into. The
                // object stays mapped -- there is no unload here -- but
                // no handle escapes.
                if (!g_reloc_failed) handle = obj(first);
            }
            // What this call loaded is unusable if it failed: a retry, or
            // another plugin needing it, must fail too, not half-work.
            if (!handle)
                for (int i = first; i < g_nobjs; i++) obj(i)->failed = 1;
        }
    }
    g_nonfatal = 0;
    // A plugin's constructors have run when dlopen returns, its new
    // dependencies' first. Before run_init (a dlopen from a constructor
    // of the program's own) they wait for it, in dependency order.
    if (handle && g_started)
        for (int i = first; i < g_nobjs; i++) init_obj(obj(i));
    return handle;
}

static void *dl_sym(void *handle, const char *name) {
    if (!handle || !name) return 0;
    const struct dobj *o = (const struct dobj *)handle;
    const struct sym64 *sym = lookup_in(o, name);
    if (!sym) {
        g_dlerr[0] = 0;
        ld_append(g_dlerr, "undefined symbol: ", sizeof g_dlerr);
        ld_append(g_dlerr, name, sizeof g_dlerr);
        return 0;
    }
    return (void *)(uintptr_t)(o->bias + sym->st_value);
}

// NOTHING IS UNLOADED, and <dlfcn.h> says so. The object table is
// fixed-size and every caller here loads its plugins once; unmapping
// would mean tracking each object's segments and refcounting what its
// DT_NEEDED pulled in, for no caller that wants it.
static int dl_close(void *handle) { (void)handle; return 0; }

static const char *dl_error(void) { return g_dlerr[0] ? g_dlerr : 0; }

// THE CANARY, from AT_RANDOM, before anything instrumented runs: entry.asm
// calls this first, so no frame alive across the change can fail its check.
// The loader's own code compares against __stack_chk_guard (it runs
// before any %fs exists); every thread's TCB gets the same value.
__attribute__((no_stack_protector))
void ldso_seed_canary(uint64_t *sp) {
    uint64_t *p = sp + 1 + sp[0] + 1;   // past argv and its NULL
    while (*p) p++;                     // past envp
    g_canary = auxv_canary(p + 1);
    if (!g_canary) g_canary = (uint64_t)__stack_chk_guard;   // no AT_RANDOM: the built-in one
    __stack_chk_guard = (uintptr_t)g_canary;
}

uint64_t ldso_main(uint64_t *sp) {
    // The SysV block: argc, argv..., NULL, envp..., NULL, auxv pairs.
    uint64_t argc = sp[0];
    uint64_t *p = sp + 1 + argc + 1; // past argv and its NULL
    while (*p) p++;                  // past envp
    p++;                             // past its NULL

    uint64_t at_phdr = 0, at_phnum = 0, at_entry = 0;
    for (; p[0] != AT_NULL; p += 2) {
        if (p[0] == AT_PHDR)  at_phdr  = p[1];
        if (p[0] == AT_PHNUM) at_phnum = p[1];
        if (p[0] == AT_ENTRY) at_entry = p[1];
    }
    if (!at_phdr || !at_phnum || !at_entry)
        die("no auxv -- was this spawned as a dynamic executable?", 0);

    // The executable is object 0: bias 0, its PT_DYNAMIC found through
    // the mapped program headers.
    const struct phdr64 *ph = (const struct phdr64 *)(uintptr_t)at_phdr;
    struct dobj *exe = obj_append();   // row 0, in the static block
    ld_memset(exe, 0, sizeof *exe);
    for (uint64_t i = 0; i < at_phnum; i++) {
        if (ph[i].p_type == PT_GNU_STACK && (ph[i].p_flags & PF_X)) g_exec_stack = 1;
        if (ph[i].p_type == PT_DYNAMIC)
            exe->dynamic = (const struct dyn64 *)(uintptr_t)ph[i].p_vaddr;
    }
    if (!exe->dynamic) die("executable has no PT_DYNAMIC", 0);
    exe->phdr = ph;
    exe->phnum = (uint16_t)at_phnum;
    note_tls(exe, ph, at_phnum);
    if (exe->tls_align > PAGE) die("thread-local data aligned past a page", "the executable");
    parse_dynamic(exe);
    obj_publish();

    // Load every needed library, breadth-first: a library's own
    // DT_NEEDED entries land on the end of the table and are walked in
    // turn. The strtab pointer is read per object AFTER parse_dynamic.
    for (int i = 0; i < g_nobjs; i++) {
        for (const struct dyn64 *d = obj(i)->dynamic; d->d_tag != DT_NULL; d++)
            if (d->d_tag == DT_NEEDED)
                load_object(obj(i)->strtab + d->d_val);
    }

    // **A PROGRAM BUILT AGAINST A DIFFERENT ABI IS REFUSED HERE, BY
    // NAME.** Before this, an executable a build behind its libraries
    // started, relocated, and then wrote through a TLS block whose size
    // it and libc disagreed about -- a page fault into its own text,
    // three frames from anything that named the real problem.
    //
    // BOTH SIDES ARE OPTIONAL AND A MISSING ONE IS NOT A FAILURE: an
    // object built before this existed carries no stamp, and refusing
    // those would make the check itself the thing that bricks userland.
    // resolve(..., 1) is the weak form, which answers 0 rather than
    // dying. The magic is checked because these are read out of an
    // object nothing has relocated yet, so a wrong address has to look
    // wrong rather than plausible.
    {
        uint64_t rq = resolve("__toy_abi_required", 1);
        uint64_t pv = resolve("__toy_abi_provided", 1);
        if (rq && pv) {
            const struct toy_abi_stamp *r = (const struct toy_abi_stamp *)(uintptr_t)rq;
            const struct toy_abi_stamp *p = (const struct toy_abi_stamp *)(uintptr_t)pv;
            if (r->magic == TOY_ABI_MAGIC && p->magic == TOY_ABI_MAGIC &&
                r->version != p->version) {
                char msg[64];
                int n = 0;
                const char *a = "built for userland ABI ";
                for (const char *c = a; *c; c++) msg[n++] = *c;
                n += ld_u32(msg + n, r->version);
                const char *b = ", this system provides ";
                for (const char *c = b; *c; c++) msg[n++] = *c;
                n += ld_u32(msg + n, p->version);
                msg[n] = '\0';
                die(msg, "rebuild the program");
            }
        }
    }

    // Static TLS offsets before relocation: a TPOFF64 is computed from them.
    tls_layout();

    // Relocate LIBRARIES first, the executable last -- order matters
    // only in that everything must be loaded before anything resolves,
    // which the loop above already guaranteed.
    for (int i = 0; i < g_nobjs; i++) {
        const struct dobj *o = obj(i);
        if (o->rela)   apply_rela(o, o->rela, o->relasz);
        if (o->jmprel) apply_rela(o, o->jmprel, o->pltrelsz);
    }

    // THE FIRST THREAD'S TLS, sized to every module: crt0's rt/tls.c sees
    // tls_alloc filled in below and leaves %fs alone.
    {
        void *tp = tls_alloc();
        if (!tp) die("no memory for thread-local storage", 0);
        sc1(SYS_SET_TLS, (uint64_t)(uintptr_t)tp);
    }

    // HAND libc THE ENTRY POINTS, after relocation so the vector is a
    // real address. This loader has no `.dynsym` for anything to
    // resolve out of, so the direction is reversed: libc defines the
    // vector, this fills it in (abi/ldso_api.h). Weak on purpose -- a
    // program linked against a libc without it still runs.
    {
        uint64_t v = resolve("__ldso_api", 1);
        if (v) {
            struct ldso_api *api = (struct ldso_api *)(uintptr_t)v;
            api->dlopen  = dl_open;
            api->dlsym   = dl_sym;
            api->dlclose = dl_close;
            api->dlerror = dl_error;
            api->run_init = run_init;
            api->run_fini = run_fini;
            api->iterate_phdr = iterate_phdr;
            api->tls_alloc = tls_alloc;
            api->tls_free = tls_free;
            api->tls_get_addr = tls_get_addr;
            api->magic   = LDSO_API_MAGIC; // LAST: it is the ready flag
        }
    }

    return at_entry;
}
