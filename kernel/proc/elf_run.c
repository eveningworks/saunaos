// See elf_run.h. Mirrors file_test.c/newsyscalls_test.c's own
// elf_load()+process_run_ring3() shape almost exactly -- the only real
// difference is where the raw ELF bytes come from.
#include "elf_run.h"
#include "elf.h"
#include "imgcache.h" // imgcache_epoch(), read before the file
#include "vmm.h"
#include "pmm.h"
#include "process.h"
#include "fs.h"
#include "heap.h"
#include "syscall.h"
#include "vga.h"
#include "klog.h"
#include "string.h"
#include "strace.h"
#include "uaddr.h"
#include "errno.h"
// Reserved, never-written padding at the top of the argv page, so a
// string landing near the end still has mapped bytes after it. 64 is
// what FS_PATH_MAX used to be, kept as the margin it actually was.
#define ARGV_TAIL_MARGIN 64


// Stack/heap/guard addresses come from uaddr.h -- this loader and the
// scheduler's build the SAME ring-3 layout, and used to say so in two
// places with nothing keeping them equal.

// **THE QUOTING RULES ARE tosh's, AND THE TWO MUST STAY EQUAL** --
// `'...'` literal, `"..."` with a backslash escaping only `"` and `\`,
// `\x` outside quotes literal, an empty quoted word a real empty
// argument (userland/libs/base/tosh.c's lex()). An UNTERMINATED quote is
// REFUSED, so a caller gets a failed spawn rather than a word nobody
// typed. `userland/tests/argv_test.c` drives both forms through one
// probe, which is what keeps them from drifting.
int elf_argv_from_string(const char *path, const char *args, char *out, size_t cap,
                         size_t *out_len) {
    size_t n = k_strlen(path);
    if (n + 1 > cap) return 0;
    k_memcpy(out, path, n + 1);
    n++;
    for (const char *p = args; p && *p; ) {
        while (*p == ' ') p++;
        if (!*p) break;
        int word = 0;   // a quoted empty word is still a word
        while (*p && *p != ' ') {
            char q = *p;
            if (q == '\\') {
                if (!p[1]) return 0;        // nothing after the backslash
                p++;
            } else if (q == '\'' || q == '"') {
                p++;
                while (*p && *p != q) {
                    if (q == '"' && *p == '\\' && (p[1] == '"' || p[1] == '\\')) p++;
                    if (n + 2 > cap) return 0;
                    out[n++] = *p++;
                }
                if (!*p) return 0;          // unterminated quote
                p++;
                word = 1;
                continue;
            }
            if (n + 2 > cap) return 0;
            out[n++] = *p++;
            word = 1;
        }
        if (word) {
            if (n + 1 > cap) return 0;   // the empty quoted word's own NUL
            out[n++] = '\0';
        }
    }
    *out_len = n;
    return 1;
}

// `env`'s shape: a NUL-separated run of strings ending in an empty one.
// Counts the entries. (`argv` is measured by its LENGTH instead -- an
// argument may be empty, and the empty string is this terminator.)
static int env_count(const char *blob) {
    int count = 0;
    if (!blob) return 0;
    for (const char *s = blob; *s; s += k_strlen(s) + 1) count++;
    return count;
}

// Entries in an `argv_len`-byte vector: one per NUL. 0 if the last byte
// is not a NUL, which is a malformed vector rather than a short one.
static int argv_count(const char *argv, size_t argv_len) {
    if (!argv || argv_len == 0 || argv[argv_len - 1] != '\0') return 0;
    int count = 0;
    for (size_t i = 0; i < argv_len; i++) if (argv[i] == '\0') count++;
    return count;
}

// The bytes elf_build_argv_on_stack() needs from the top of the stack:
// the margin, every string, the SysV block and its alignment slack. A
// caller maps ceil(this / 4096) top pages (at least its usual working
// set) and hands them over, so an argv is as big as the call that
// carried it -- up to what elf_argv_stack_pages() allows.
size_t elf_argv_stack_bytes(const char *path, const char *argv, size_t argv_len,
                            const char *env, int auxc) {
    int argc = argv_count(argv, argv_len);
    size_t argv_bytes = argc ? argv_len : k_strlen(path) + 1;
    if (!argc) argc = 1;
    int envc = env_count(env);
    size_t env_bytes = 0;
    if (env) for (const char *e = env; *e; e += k_strlen(e) + 1) env_bytes += k_strlen(e) + 1;
    return ARGV_TAIL_MARGIN + argv_bytes + env_bytes + 16
         + 8 + (size_t)(argc + 1) * 8 + (size_t)(envc + 1) * 8
         + (auxc ? (size_t)(auxc + 1) * 16 : 0);
}

// Pages for the above, or 0 past a quarter of the stack's reach -- Linux's
// ARG_MAX rule (a quarter of the stack rlimit), so the arguments can
// never leave the program too little stack to run on. SPAWN_ARGS_MAX
// bounds the strings; this bounds what a vector of many tiny ones
// costs in pointers.
int elf_argv_stack_pages(size_t bytes) {
    size_t pages = (bytes + 4095) / 4096;
    return pages > UADDR_STACK_MAX_PAGES / 4 ? 0 : (int)pages;
}

// Copies into the stack through the FRAMES that back it -- the pages are
// not contiguous physically, so a string may cross from one into the
// next. frames[k] backs the page at `top_vaddr - k * 4096`.
static void stack_put(const uint64_t *frames, uint64_t top_vaddr, uint64_t va,
                      const void *src, size_t len) {
    const uint8_t *from = (const uint8_t *)src;
    while (len) {
        uint64_t page = va & ~4095ULL;
        size_t in_page = 4096 - (size_t)(va & 4095);
        size_t n = len < in_page ? len : in_page;
        uint8_t *to = (uint8_t *)(uintptr_t)(frames[(top_vaddr - page) / 4096] + (va & 4095));
        k_memcpy(to, from, n);
        from += n;
        va += n;
        len -= n;
    }
}

static void stack_put64(const uint64_t *frames, uint64_t top_vaddr, uint64_t va, uint64_t v) {
    stack_put(frames, top_vaddr, va, &v, sizeof v);
}

// Lays argv and envp into the top `nframes` stack pages (frames[0] the
// page at `stack_vaddr`, each next one 4096 below): the strings written
// down from the top, then the SysV entry block (each pointer a *vaddr*,
// since that is what ring-3 code will dereference) below them, with
// `*out_user_rsp` set to the block's own address -- so a subsequent
// `push` from ring 3 only ever writes to fresh, lower stack. Returns 1,
// or 0 if it does not fit the pages given -- callers must treat that as
// a hard failure, never silently truncate. Size them with
// elf_argv_stack_bytes().
//
// TWO PASSES OVER EACH BLOB, and no per-entry array in this frame: the
// counts size the block, the block is placed, and the second pass
// writes each pointer straight into it. A local pointer array per
// entry would bound argc by this function's frame budget rather than
// by the pages, which is the wrong limit for a shell that globs.
int elf_build_argv_on_stack(const uint64_t *frames, int nframes, uint64_t stack_vaddr,
                             const char *path, const char *argv, size_t argv_len,
                             const char *env,
                             const uint64_t (*auxv)[2], int auxc,
                             uint64_t *out_argc, uint64_t *out_argv,
                             uint64_t *out_user_rsp) {
    if (!auxv) auxc = 0;
    if (nframes < 1 || elf_argv_stack_bytes(path, argv, argv_len, env, auxc) > (size_t)nframes * 4096)
        return 0;

    // An absent or empty vector is argv = {path}: a program entered
    // with argc == 0 dereferences argv[0] == NULL, so there is always
    // one entry.
    int argc = argv_count(argv, argv_len);
    if (argc == 0) { argv = 0; argc = 1; }
    int envc = env_count(env);

    // Strings go downward, starting ARGV_TAIL_MARGIN bytes below the
    // top -- reserved, never-written padding, so a string starting near
    // the top still has mapped bytes after it.
    //
    // **IT WAS FS_PATH_MAX, AND THAT STOPPED BEING A MARGIN WHEN A PATH
    // BECAME 4096** -- it reserved the whole page, every spawn failed
    // the size test, and the machine could not start /bin/init. A margin
    // has to be small relative to a page, which is the reason it is a
    // number of its own.
    uint64_t at = stack_vaddr + 4096 - ARGV_TAIL_MARGIN;
    uint64_t str_lo = at;
    if (!argv) {
        str_lo -= k_strlen(path) + 1;
    } else {
        str_lo -= argv_len;
    }
    if (env) for (const char *e = env; *e; e += k_strlen(e) + 1) str_lo -= k_strlen(e) + 1;
    size_t block_bytes = 8 + (size_t)(argc + 1) * 8 + (size_t)(envc + 1) * 8
                       + (auxc ? (size_t)(auxc + 1) * 16 : 0);
    uint64_t block = (str_lo - block_bytes) & ~(uint64_t)15; // SysV: 16-aligned AT ENTRY

    // The strings, argv[0] highest, each pointer written as its string
    // lands.
    stack_put64(frames, stack_vaddr, block, (uint64_t)argc);
    if (!argv) {
        size_t len = k_strlen(path);
        at -= len + 1;
        stack_put(frames, stack_vaddr, at, path, len + 1);
        stack_put64(frames, stack_vaddr, block + 8, at);
    } else {
        int i = 0;
        for (const char *s = argv; s < argv + argv_len; s += k_strlen(s) + 1, i++) {
            size_t len = k_strlen(s);
            at -= len + 1;
            stack_put(frames, stack_vaddr, at, s, len + 1);
            stack_put64(frames, stack_vaddr, block + 8 + 8 * (uint64_t)i, at);
        }
    }
    stack_put64(frames, stack_vaddr, block + 8 + 8 * (uint64_t)argc, 0); // argv terminator

    // The ENVIRONMENT's strings, placed the same way. `env` is a
    // NUL-separated run of "KEY=VALUE" terminated by an empty string --
    // one blob rather than a char** the kernel would have to walk
    // pointer by pointer, validating each one out of user memory.
    uint64_t envp = block + 8 + 8 * (uint64_t)(argc + 1);
    if (env) {
        int i = 0;
        for (const char *e = env; *e; e += k_strlen(e) + 1, i++) {
            size_t len = k_strlen(e);
            at -= len + 1;
            stack_put(frames, stack_vaddr, at, e, len + 1);
            stack_put64(frames, stack_vaddr, envp + 8 * (uint64_t)i, at);
        }
    }
    stack_put64(frames, stack_vaddr, envp + 8 * (uint64_t)envc, 0); // envp terminator

    // The SysV process-entry block, laid out below every string it
    // points at. From RSP upward:
    //
    //     (%rsp)          argc
    //     8(%rsp)         argv[0] .. argv[argc-1]
    //                     NULL              (argv terminator)
    //                     envp[0] .. envp[envc-1]
    //                     NULL              (envp terminator)
    //
    // This is the standard layout a real crt0 expects, and userland/
    // crt0.asm is what reads it. An auxv (abi/auxv.h) follows the envp
    // terminator ONLY when the caller passed one -- i.e. only for a
    // dynamic executable, whose interpreter is the one consumer. A
    // static program's stack is byte-identical to what it always was.
    //
    // **RSP is 16-byte ALIGNED at entry**, per SysV. That is a change,
    // and the previous convention is worth recording because it looked
    // wrong and wasn't: this used to hand over RSP % 16 == 8, because
    // every _start was a plain C function. GCC compiles such a function
    // like any other -- assuming a return address was pushed, i.e.
    // RSP % 16 == 8 on entry -- and sizes its prologue to land
    // 16-aligned locals from there. Handing THAT a 16-aligned RSP put
    // every aligned stack slot off by exactly 8, which surfaced as
    // userland/fpu_test.c's `movapd %xmm0,(%rsp)` taking a #GP at ring
    // 3 (established by testing, not by reading the ABI; both this and
    // the plain 8-alignment before it were invisible while userland was
    // built -mno-sse and nothing could emit an alignment-sensitive
    // instruction at all).
    //
    // A hand-written assembly _start does not have that problem -- it
    // makes no assumption about a pushed return address and realigns
    // before calling main. So the entry point moving into crt0.asm is
    // exactly what makes the standard 16-alignment correct here.
    if (auxc) {
        uint64_t av = envp + 8 * (uint64_t)(envc + 1);
        for (int i = 0; i < auxc; i++) {
            stack_put64(frames, stack_vaddr, av + 16 * (uint64_t)i, auxv[i][0]);
            stack_put64(frames, stack_vaddr, av + 16 * (uint64_t)i + 8, auxv[i][1]);
        }
        stack_put64(frames, stack_vaddr, av + 16 * (uint64_t)auxc, 0);     // AT_NULL
        stack_put64(frames, stack_vaddr, av + 16 * (uint64_t)auxc + 8, 0);
    }

    *out_argc = (uint64_t)argc;
    *out_argv = block + 8;     // &argv[0], for callers that want it
    *out_user_rsp = block;     // &argc -- what RSP must be at entry
    return 1;
}

int elf_run_from_fs(const char *path, const char *args) {
    // Into memory this function owns (fs_read_into), never the backend's
    // staging buffer, which a ring-3 file read could free mid-load.
    // kmalloc memory is carved out of the identity-mapped low 4 GiB (see
    // heap_core.c), so elf_load() takes its address directly, exactly as
    // it takes a GRUB module's -- no scratch copy.
    struct elf_source src = { path, imgcache_epoch() };   // before the read
    uint64_t fsz = fs_size(path);
    char *data = fsz && fsz < 0xFFFFFFFFu - 1 ? kmalloc((size_t)fsz + 1) : 0;
    uint32_t size = data ? fs_read_into(path, data, (uint32_t)fsz + 1) : 0;
    if (size == 0) {
        if (data) kfree(data);
        vga_write("run: couldn't read "); vga_write(path); vga_write(" from disk\n");
        return -1;
    }
    uint64_t elf_phys = (uint64_t)(uintptr_t)data;

    uint64_t as = vmm_create_address_space();
    if (!as) {
        kfree(data);
        vga_write("run: vmm_create_address_space() failed\n");
        return -1;
    }

    uint64_t entry = 0, image_end = 0;
    // `size` comes from fs_read() above and used to be discarded here;
    // it is what bounds every offset in the file. On failure the address
    // space is destroyed rather than leaked -- see elf.h.
    int loaded = elf_load(elf_phys, size, as, &entry, &image_end, 0, &src);
    kfree(data);   // segments and phdrs are in the address space now
    if (loaded < 0) {
        vga_write("run: "); vga_write(path);
        vga_write(loaded == -ENOMEM ? ": out of memory loading it\n" : " isn't a valid ELF64 executable\n");
        vmm_destroy_address_space(as);
        return -1;
    }

    // The TOP page holds argv and is where RSP starts; the rest are the
    // starting working set. Everything below them is reserved address
    // space uheap_fault() maps on demand -- see kernel/uaddr.h.
    // This blocking loader keeps its fixed working set and refuses an argv
    // that outgrows it; the scheduler's (sched_fork.c) maps what one needs.
    uint64_t stack_frames[UADDR_STACK_INIT_PAGES];
    for (int pg = 0; pg < UADDR_STACK_INIT_PAGES; pg++) {
        uint64_t frame = pmm_alloc_user_frame();
        if (!frame) {
            vga_write("run: out of physical memory for the stack\n");
            return -1;
        }
        if (!vmm_map_user_page(as, UADDR_STACK_VADDR - (uint64_t)pg * 4096, frame)) {
            vga_write("run: failed to map a stack page\n");
            return -1;
        }
        stack_frames[pg] = frame;
    }

    // Arms SYS_SBRK for this process unconditionally -- cheap
    // bookkeeping (see syscall.c's syscall_reset_mm()), not an
    // allocation, so it costs nothing for a binary that never calls
    // sbrk(). This used to be a case-by-case opt-in (only echo_test.c
    // called it, for the one binary that needed SYS_SBRK) -- made
    // universal here so any /bin binary can use it, not just the ones
    // whose bespoke kernel-side loader remembered to arm it.
    syscall_reset_mm(as, image_end);

    // The string form is split HERE, into a vector the builder takes --
    // never longer than the path and the string it came from.
    uint64_t argc = 0, argv = 0, user_rsp = 0;
    size_t vec_cap = k_strlen(path) + (args ? k_strlen(args) : 0) + 2;
    char *vec = kmalloc(vec_cap);
    size_t vec_len = 0;
    int built = vec && elf_argv_from_string(path, args, vec, vec_cap, &vec_len) &&
                elf_build_argv_on_stack(stack_frames, UADDR_STACK_INIT_PAGES, UADDR_STACK_VADDR,
                                        path, vec, vec_len, 0, 0, 0, &argc, &argv, &user_rsp);
    if (vec) kfree(vec);
    if (!built) {
        vga_write("run: arguments too long for ");
        vga_write(path);
        vga_write("\n");
        return -1;
    }

    // Last, past every failure path, as build_elf_image() does: an armed
    // trace attaches to an address space that will run.
    strace_claim(as);

    klog_write("elf_run: calling process_run_ring3() for ");
    klog_write(path);
    klog_write("\n");

    // argc/argv are NOT passed in registers any more -- they live on
    // the stack this call hands over, in the SysV layout
    // elf_build_argv_on_stack() built and userland/crt0.asm reads. The
    // register variant that used to exist here is gone rather than kept
    // "just in case": two live conventions for the same thing is how an
    // ABI rots.
    (void)argc; (void)argv; // consumed via user_rsp, see above
    int exit_code = process_run_ring3(as, entry, user_rsp);

    klog_write("elf_run: process_run_ring3() returned\n");
    return exit_code;
}
