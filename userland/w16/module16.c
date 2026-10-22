// module16.c -- the modules a program reaches besides itself, and how
// every NE image is bound to them (what KERNEL's loader did):
//
//   Windows' own (KERNEL, USER, GDI... any w16_names knows) are w16's C.
//   Each gets ONE thunk selector whose offsets are its ordinals, shared
//   by the program and every DLL, and a by-NAME import is turned into
//   its ordinal through the same tables.
//
//   A program's own DLL (Hearts' CARDS.DLL) is loaded as Windows loaded
//   one: found beside the program or in C:\WINDOWS\SYSTEM and C:\WINDOWS,
//   its segments given selectors, its relocations bound the same way,
//   its exported prologues patched to load its own DGROUP, its local
//   heap its own, and its LibEntry run. Loaded once, kept until the
//   program ends (FreeLibrary counts and keeps it: nothing unloads).
//
// AN hINSTANCE IS A DGROUP SELECTOR, a DLL's as a program's, so the local
// heap a KERNEL call works on is the one DS names (w16_heap_of()).
#include "w16_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Windows' own modules ------------------------------------------------------

static int same(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a;
        char y = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

int w16_is_builtin(const char *module) {
    if (same(module, "KRNL386")) return 1;
    for (const struct w16_name_module *m = w16_name_modules; m->module; m++)
        if (same(m->module, module)) return 1;
    return 0;
}

// A Windows module's export by name: its ordinal, 0 when unknown.
unsigned w16_builtin_ordinal(const char *module, const char *name) {
    if (same(module, "KRNL386")) module = "KERNEL";
    for (const struct w16_name_module *m = w16_name_modules; m->module; m++)
        if (same(m->module, module))
            for (unsigned i = 0; i < m->count; i++)
                if (same(m->names[i].name, name)) return m->names[i].ordinal;
    return 0;
}

uint16_t w16_thunk_sel(struct w16 *w, const char *module) {
    if (same(module, "KRNL386")) module = "KERNEL";
    for (int i = 0; i < w->nthunks; i++)
        if (same(w->thunks[i].name, module)) return w->thunks[i].sel;
    void *g = realloc(w->thunks, sizeof *w->thunks * (size_t)(w->nthunks + 1));
    if (!g) return 0;
    w->thunks = g;
    uint16_t sel = cpu16_alloc(&w->cpu, D_THUNK, 0, 0xFFFF, 0);
    if (!sel) return 0;
    snprintf(w->thunks[w->nthunks].name, sizeof w->thunks[0].name, "%s", module);
    for (char *p = w->thunks[w->nthunks].name; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    w->thunks[w->nthunks++].sel = sel;
    return sel;
}

const char *w16_thunk_module(struct w16 *w, uint16_t sel) {
    for (int i = 0; i < w->nthunks; i++)
        if (w->thunks[i].sel == sel) return w->thunks[i].name;
    return 0;
}

// KERNEL's constants: an import of one is patched to its value at load, never called.
// __AHSHIFT = 3
// Windows: A constant a program imports rather than a function: how far to shift a count of 64 KB
// steps to get the selector step of a huge (over 64 KB) block -- 3 in protected mode.
// w16: 3, patched in where the program uses it, as Windows' protected mode had it. w16 makes no
// block over 64 KB (GlobalAlloc), so a program never has a huge block to step through.
// __AHINCR = 8
// Windows: The selector step itself, 1 << __AHSHIFT: what a huge pointer adds to its selector to
// reach the next 64 KB.
// w16: 8, patched in at load.
// __WINFLAGS = 0x0425
// Windows: A constant saying what Windows runs on and in -- the processor, protected mode, standard
// or enhanced mode, a coprocessor.
// w16: 0x0425 -- protected mode, a 386, enhanced mode, a coprocessor -- what 3.1 said on the
// machines its programs expected.
static const struct { uint16_t ordinal, value; } KERNEL_EQUATES[] = {
    { 113, 3 },        // __AHSHIFT: a huge pointer's selector step is 1 << 3
    { 114, 8 },        // __AHINCR
    { 178, 0x0425 },   // __WINFLAGS: protected mode, a 386, enhanced mode, a coprocessor
};

int w16_kernel_equate(const char *module, unsigned ordinal, uint16_t *value) {
    if (!same(module, "KERNEL") && !same(module, "KRNL386")) return 0;
    for (unsigned i = 0; i < sizeof KERNEL_EQUATES / sizeof KERNEL_EQUATES[0]; i++)
        if (KERNEL_EQUATES[i].ordinal == ordinal) { *value = KERNEL_EQUATES[i].value; return 1; }
    return 0;
}

// --- binding an image -------------------------------------------------------------

static int fail(const char *path, char *why, size_t cap, const char *msg) {
    snprintf(why, cap, "%s", msg);
    fprintf(stderr, "w16: %s: %s\n", path, msg);
    return -1;
}

static int apply_value(uint8_t *mem, uint32_t len, uint16_t at, uint8_t src, int additive,
                       uint16_t sel, uint16_t off) {
    unsigned need = src == NE_SRC_FARPTR ? 4 : src == NE_SRC_LOBYTE ? 1 : 2;
    if ((uint32_t)at + need > len) return -1;
    uint8_t *p = mem + at;
    uint16_t cur = (uint16_t)(p[0] | (need > 1 ? p[1] << 8 : 0));
    switch (src) {
    case NE_SRC_LOBYTE: p[0] = (uint8_t)(additive ? p[0] + off : off); break;
    case NE_SRC_SEGMENT: p[0] = (uint8_t)sel; p[1] = (uint8_t)(sel >> 8); break;
    case NE_SRC_OFFSET: {
        uint16_t v = additive ? (uint16_t)(cur + off) : off;
        p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
        break;
    }
    case NE_SRC_FARPTR: {
        uint16_t v = additive ? (uint16_t)(cur + off) : off;
        p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
        p[2] = (uint8_t)sel; p[3] = (uint8_t)(sel >> 8);
        break;
    }
    default: return -1;
    }
    return 0;
}

// Where an import lands: a Windows module's thunk, or a DLL's entry.
static int import_target(struct w16 *w, const struct w16_image *im, unsigned modref, int by_name,
                         uint16_t b, uint16_t *sel, uint16_t *off, char *why, size_t cap) {
    char mod[16], fn[64] = "", msg[160];
    if (ne_module_name(im->ne, modref, mod, sizeof mod) < 0)
        return fail(im->path, why, cap, "an imported module's name is malformed");
    if (by_name && ne_imported_name(im->ne, b, fn, sizeof fn) < 0)
        return fail(im->path, why, cap, "an imported function's name is malformed");
    if (w16_is_builtin(mod)) {
        unsigned ord = by_name ? w16_builtin_ordinal(mod, fn) : b;
        if (!ord) {
            snprintf(msg, sizeof msg, "imports %s.%s, a name w16 does not know", mod, fn);
            return fail(im->path, why, cap, msg);
        }
        if (w16_kernel_equate(mod, ord, off)) { *sel = *off; return 0; }
        *sel = w16_thunk_sel(w, mod);
        *off = (uint16_t)ord;
        return *sel ? 0 : fail(im->path, why, cap, "too many selectors");
    }
    int err;
    struct w16_dll *d = w16_dll_load(w, mod, &err);
    if (!d) {
        snprintf(msg, sizeof msg, "needs %s.DLL, which %s", mod, err == 2 ? "is not there" : "w16 cannot load");
        return fail(im->path, why, cap, msg);
    }
    unsigned ord = by_name ? ne_export_find(&d->ne, fn) : b;
    uint16_t seg;
    if (!ord || ne_entry(&d->ne, ord, &seg, off) < 0) {
        snprintf(msg, sizeof msg, "imports %s.%s%u, which it does not export", mod, by_name ? fn : "", by_name ? 0 : b);
        return fail(im->path, why, cap, msg);
    }
    *sel = d->seg_sel[seg];
    return 0;
}

// One relocation record of segment `n`: its target, then every site of
// its chain (just the one when additive).
static int relocate(struct w16 *w, const struct w16_image *im, unsigned n, const struct ne_reloc *r,
                    uint8_t *mem, uint32_t len, char *why, size_t cap) {
    uint16_t sel = 0, off = 0;
    switch (r->flags & 3) {
    case NE_REL_INTERNAL: {
        unsigned seg = r->a & 0xFF;
        off = r->b;
        if (seg == 0xFF) {                  // a movable segment: through the entry table
            uint16_t s16;
            if (ne_entry(im->ne, r->b, &s16, &off) < 0)
                return fail(im->path, why, cap, "a relocation names a missing entry point");
            seg = s16;
        }
        if (seg < 1 || seg > im->ne->nseg) return fail(im->path, why, cap, "a relocation names no segment");
        sel = im->seg_sel[seg];
        break;
    }
    case NE_REL_IMPORT_ORD:
    case NE_REL_IMPORT_NAME:
        if (r->a < 1 || r->a > im->ne->nmod) return fail(im->path, why, cap, "an import names no module");
        if (import_target(w, im, r->a, (r->flags & 3) == NE_REL_IMPORT_NAME, r->b, &sel, &off, why, cap) < 0)
            return -1;
        break;
    default:
        // OSFIXUP: where Windows with no coprocessor turned an FPU instruction
        // into its emulator's INT 34h-3Dh. w16 has a 387, so they stay real.
        return 0;
    }
    int additive = (r->flags & NE_REL_ADDITIVE) != 0;
    uint16_t at = r->site;
    for (unsigned guard = 0; guard < 0x10000; guard++) {
        uint16_t next = (uint32_t)at + 2 <= len ? (uint16_t)(mem[at] | mem[at + 1] << 8) : 0xFFFF;
        if (apply_value(mem, len, at, r->src, additive, sel, off) < 0) {
            char msg[80];
            snprintf(msg, sizeof msg, "a relocation in segment %u lies outside it", n);
            return fail(im->path, why, cap, msg);
        }
        if (additive || next == 0xFFFF) return 0;
        at = next;
    }
    return fail(im->path, why, cap, "a relocation chain loops");
}

int w16_bind_image(struct w16 *w, const struct w16_image *im, char *why, size_t cap) {
    for (unsigned n = 1; n <= im->ne->nseg; n++) {
        const struct ne_segment *s = &im->ne->seg[n - 1];
        struct cpu16_desc *d = &w->desc[cpu16_index(im->seg_sel[n])];
        for (unsigned i = 0; i < s->nrelocs; i++) {
            struct ne_reloc r;
            if (ne_reloc_at(im->ne, n, i, &r) < 0) return fail(im->path, why, cap, "a relocation is malformed");
            if (relocate(w, im, n, &r, d->base, d->limit + 1, why, cap) < 0) return -1;
        }
    }
    return 0;
}

int w16_load_segments(struct w16 *w, const struct w16_image *im, uint32_t dgroup_extra, char *why, size_t cap) {
    for (unsigned n = 1; n <= im->ne->nseg; n++) {
        const struct ne_segment *s = &im->ne->seg[n - 1];
        uint32_t want = s->minalloc > s->length ? s->minalloc : s->length;
        if (n == im->ne->autodata) want += dgroup_extra;
        if (want > 0x10000) {
            if (n == im->ne->autodata) return fail(im->path, why, cap, "its data, heap and stack exceed 64 KiB");
            want = 0x10000;
        }
        if (want == 0) want = 16;
        uint8_t *mem = calloc(1, 0x10000);   // a whole segment: a later resize needs no move
        if (!mem) return fail(im->path, why, cap, "out of memory");
        if (s->file_off) memcpy(mem, im->ne->img + s->file_off, s->length);
        int data = (s->flags & NE_SEG_DATA) != 0;
        im->seg_sel[n] = cpu16_alloc(&w->cpu, data ? D_DATA : D_CODE, mem, want - 1, (uint16_t)n);
        if (!im->seg_sel[n]) { free(mem); return fail(im->path, why, cap, "too many segments"); }
    }
    return 0;
}

// --- a program's own DLLs ------------------------------------------------------------

// "CARDS", "cards.dll", "C:\GAMES\CARDS.DLL" -> "CARDS": what a module is
// known by. Its file name is the name plus ".DLL" unless it had one.
static void module_name(const char *spec, char *out, size_t cap) {
    const char *b = spec;
    for (const char *p = spec; *p; p++) if (*p == '\\' || *p == '/' || *p == ':') b = p + 1;
    size_t n = 0;
    for (; b[n] && b[n] != '.' && n + 1 < cap; n++) out[n] = b[n] >= 'a' && b[n] <= 'z' ? (char)(b[n] - 32) : b[n];
    out[n] = 0;
}

// The DLL's host file: as named when the name is a path, else beside the
// program, in the current folder, C:\WINDOWS\SYSTEM and C:\WINDOWS --
// Windows' order. 0, or 2 (file not found).
static int find_dll(struct w16 *w, const char *spec, char *host, size_t cap) {
    char file[80];
    const char *base = spec;
    for (const char *p = spec; *p; p++) if (*p == '\\' || *p == '/' || *p == ':') base = p + 1;
    snprintf(file, sizeof file, "%s%s", base, strchr(base, '.') ? "" : ".DLL");
    char prog_dir[280] = "", dos[300];
    const char *slash = strrchr(w->path, '/');
    if (slash) {
        char dir[512];
        snprintf(dir, sizeof dir, "%.*s", (int)(slash - w->path), w->path);
        if (w16_host_to_dos(dir[0] ? dir : "/", prog_dir, sizeof prog_dir) < 0) prog_dir[0] = 0;
        size_t n = strlen(prog_dir);
        if (n && prog_dir[n - 1] == '\\') prog_dir[n - 1] = 0;
    }
    for (int i = base != spec ? 0 : 1; i < 5; i++) {
        switch (i) {
        case 0: snprintf(dos, sizeof dos, "%s", spec); break;
        case 1: if (!prog_dir[0]) continue;
                snprintf(dos, sizeof dos, "%s\\%s", prog_dir, file); break;
        case 2: snprintf(dos, sizeof dos, "%s", file); break;
        case 3: snprintf(dos, sizeof dos, "C:\\WINDOWS\\SYSTEM\\%s", file); break;
        default: snprintf(dos, sizeof dos, "C:\\WINDOWS\\%s", file); break;
        }
        FILE *f;
        if (w16_dos_path(dos, host, cap, 0, 0, 0) == 0 && (f = fopen(host, "rb"))) { fclose(f); return 0; }
        if (i == 0) break;                       // a path names one place only
    }
    return 2;
}

static uint8_t *read_file(const char *path, uint32_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t *buf = 0;
    size_t n = 0, cap = 0;
    for (;;) {
        if (n == cap) {
            cap = cap ? cap * 2 : 65536;
            if (cap > 16u << 20) { free(buf); fclose(f); return 0; }   // no NE module is this large
            uint8_t *g = realloc(buf, cap);
            if (!g) { free(buf); fclose(f); return 0; }
            buf = g;
        }
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (got == 0) break;
    }
    fclose(f);
    *size = (uint32_t)n;
    return buf;
}

// Each exported entry that loads its DGROUP with "push ds; pop ax" or
// "mov ax, ds" now loads it outright ("mov ax, DGROUP") -- what Windows'
// loader did to a library's prologues, so a call from the program arrives
// with the DLL's data, not the caller's.
static void patch_prologues(struct w16 *w, struct w16_dll *d) {
    unsigned n = ne_entry_count(&d->ne);
    for (unsigned ord = 1; ord <= n; ord++) {
        uint16_t seg, off;
        uint8_t flags;
        if (ne_entry_flags(&d->ne, ord, &seg, &off, &flags) < 0 || !(flags & NE_ENTRY_SHARED_DATA)) continue;
        struct cpu16_desc *c = &w->desc[cpu16_index(d->seg_sel[seg])];
        if ((uint32_t)off + 3 > (uint32_t)c->limit + 1) continue;
        uint8_t *p = c->base + off;
        if ((p[0] == 0x1E && p[1] == 0x58) || (p[0] == 0x8C && p[1] == 0xD8)) {
            p[0] = 0xB8;
            p[1] = (uint8_t)d->dgroup;
            p[2] = (uint8_t)(d->dgroup >> 8);
        }
    }
}

// LibEntry: DI its hInstance, DS its DGROUP, CX its heap size, ES:SI no
// command line. Its AX says whether it started.
static int lib_entry(struct w16 *w, struct w16_dll *d) {
    if (!d->ne.entry_cs) return 1;
    struct cpu16 *c = &w->cpu;
    uint16_t r[8], s[4], flags = c->flags;
    memcpy(r, c->r, sizeof r);
    memcpy(s, c->sreg, sizeof s);
    c->r[R_DI] = d->hinst;
    c->r[R_CX] = d->ne.heap;
    c->r[R_SI] = 0;
    c->sreg[S_ES] = 0;
    c->sreg[S_DS] = d->dgroup;
    cpu16_call_far(c, d->seg_sel[d->ne.entry_cs], d->ne.entry_ip);
    int ok = c->r[R_AX] != 0;
    memcpy(c->r, r, sizeof r);
    c->sreg[S_ES] = s[S_ES];
    c->sreg[S_DS] = s[S_DS];
    c->flags = flags;
    return ok && !c->fault;
}

struct w16_dll *w16_dll_load(struct w16 *w, const char *spec, int *err) {
    char name[16], host[1024], why[160];
    module_name(spec, name, sizeof name);
    *err = 2;
    for (int i = 0; i < w->ndlls; i++)
        if (!strcmp(w->dlls[i]->name, name)) { w->dlls[i]->refs++; return w->dlls[i]; }
    if (!name[0] || find_dll(w, spec, host, sizeof host)) return 0;
    *err = 11;                                   // ERROR_BAD_FORMAT: there, but not a library w16 can load
    struct w16_dll *d = calloc(1, sizeof *d);
    void *g = realloc(w->dlls, sizeof *w->dlls * (size_t)(w->ndlls + 1));
    if (!d || !g) { free(d); return 0; }
    w->dlls = g;
    snprintf(d->name, sizeof d->name, "%s", name);
    d->path = strdup(host);
    d->img = read_file(host, &d->size);
    if (!d->path || !d->img || ne_parse(d->img, d->size, &d->ne) < 0 || !(d->ne.flags & 0x8000)) {
        fprintf(stderr, "w16: %s: not a Windows library w16 can load\n", host);
        free(d->img); free(d->path); free(d);
        return 0;
    }
    // Registered before its relocations bind, so two DLLs that import
    // from each other find one another rather than loading forever.
    d->refs = 1;
    w->dlls[w->ndlls++] = d;
    struct w16_image im = { &d->ne, d->seg_sel, d->path };
    if (w16_load_segments(w, &im, d->ne.heap, why, sizeof why) < 0) { d->broken = 1; return 0; }
    d->dgroup = d->ne.autodata ? d->seg_sel[d->ne.autodata] : 0;
    d->hinst = d->dgroup ? d->dgroup : d->seg_sel[1];
    if (w16_bind_image(w, &im, why, sizeof why) < 0) { d->broken = 1; return 0; }
    patch_prologues(w, d);
    if (g_w16_trace) {
        fprintf(g_w16_trace, "  LoadLibrary %s from %s: hInstance %04x\n", d->name, d->path, d->hinst);
        fflush(g_w16_trace);
    }
    // One the program imports is bound before its task runs: its LibEntry
    // waits for the task (w16_dll_start_pending()).
    if (!w->cpu.thunk) d->pending = 1;
    else if (!lib_entry(w, d)) { d->broken = 1; return 0; }
    *err = 0;
    return d;
}

int w16_dll_start_pending(struct w16 *w) {
    for (int i = 0; i < w->ndlls; i++) {
        struct w16_dll *d = w->dlls[i];
        if (!d->pending) continue;
        d->pending = 0;
        if (!lib_entry(w, d)) {
            char why[96];
            snprintf(why, sizeof why, "%s.DLL did not start (its LibEntry failed)", d->name);
            if (!w->cpu.fault) cpu16_set_fault(&w->cpu, FAULT_API, why);
            return -1;
        }
    }
    return 0;
}

struct w16_dll *w16_dll_of(struct w16 *w, uint16_t h) {
    for (int i = 0; i < w->ndlls; i++)
        if (!w->dlls[i]->broken && (w->dlls[i]->hinst == h || w->dlls[i]->dgroup == h)) return w->dlls[i];
    return 0;
}

const struct ne_module *w16_module_of(struct w16 *w, uint16_t inst) {
    if (!inst) return 0;
    struct w16_dll *d = w16_dll_of(w, inst);
    return d ? &d->ne : &w->ne;
}

// --- the local heap a KERNEL call works on --------------------------------------

// The local-heap calls work on the heap in DS's segment, as Windows'
// did: a DLL's own when its code calls them. That heap is swapped in for
// the call and back out after -- mem16.c knows one heap, in w->dgroup.
static int local_call(unsigned ordinal) {
    return (ordinal >= 4 && ordinal <= 14) || ordinal == 121 || ordinal == 161 || ordinal == 162;
}

int w16_kernel_heap_call(struct w16 *w, unsigned ordinal, struct w16_args *a,
                         int (*call)(struct w16 *, unsigned, struct w16_args *)) {
    struct w16_dll *d = local_call(ordinal) ? w16_dll_of(w, w->cpu.sreg[S_DS]) : 0;
    if (!d || !d->dgroup) return call(w, ordinal, a);
    uint16_t dg = w->dgroup;
    struct w16_lheap h = w->lheap;
    w->dgroup = d->dgroup;
    w->lheap = d->heap;
    int r = call(w, ordinal, a);
    d->heap = w->lheap;
    w->lheap = h;
    w->dgroup = dg;
    return r;
}

// --- KERNEL's module calls ----------------------------------------------------------

static int far_name(struct w16 *w, uint32_t far, char *out, size_t cap) {
    out[0] = 0;
    return cpu16_str(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, out, (unsigned)cap) < 0 ? -1 : 0;
}

// LoadLibrary(lpLibFileName) -> HINSTANCE
// Windows: Loads a DLL and returns its instance handle, or an error number under 32 -- 2 when the
// file is not there. A DLL already loaded is counted again.
// w16: A Windows module (USER, GDI...) is always there: it answers with that module's handle. Any
// other DLL -- a program's own, as Hearts' CARDS.DLL -- is loaded from the program's folder, its
// segments relocated and its entry point run once; 2 when it is not there. NETAPI.DLL, Windows for
// Workgroups' own network layer, is "not there" even in a full 3.11 tree: it talks to a real-mode
// NetBIOS through DOS memory and INT 2Fh, which w16 has no network behind (WNetGetCaps says so too),
// and Hearts then plays alone as it does on a tree without it.
static int api_LoadLibrary(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    char spec[260], name[16];
    if (far_name(w, w16_arg32(a), spec, sizeof spec) < 0) return -1;
    module_name(spec, name, sizeof name);
    int err = 2;
    if (w16_is_builtin(name)) {                  // Windows' own: always there
        w->cpu.r[R_AX] = w16_thunk_sel(w, name);
        return 4;
    }
    if (same(name, "NETAPI")) {
        w->cpu.r[R_AX] = (uint16_t)err;
        return 4;
    }
    struct w16_dll *d = w16_dll_load(w, spec, &err);
    if (w->cpu.fault) return -1;
    w->cpu.r[R_AX] = d && !d->broken ? d->hinst : (uint16_t)err;
    return 4;
}

// FreeLibrary(hLibModule) -> void
// Windows: Counts a DLL down, unloading it when no one uses it.
// w16: Counts it down; the DLL stays loaded until the program ends.
static int api_FreeLibrary(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct w16_dll *d = w16_dll_of(w, w16_arg16(a));
    if (d && d->refs > 0) d->refs--;
    return 2;
}

// GetModuleHandle(lpModuleName) -> HMODULE
// Windows: The handle of a module already loaded, by its name; 0 if it is not.
// w16: The program's own, a DLL it loaded, or a Windows module's -- the handle GetProcAddress
// takes; 0 for one not loaded.
static int api_GetModuleHandle(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint32_t far = w16_arg32(a);
    char spec[260] = "", name[16];
    if ((far >> 16) && far_name(w, far, spec, sizeof spec) < 0) return -1;
    module_name(spec, name, sizeof name);
    uint16_t h = 0;
    if (!(far >> 16) || same(name, w->ne.name)) h = w->dgroup;
    else if (w16_is_builtin(name)) h = w16_thunk_sel(w, name);
    else
        for (int i = 0; i < w->ndlls; i++)
            if (!w->dlls[i]->broken && !strcmp(w->dlls[i]->name, name)) h = w->dlls[i]->hinst;
    w->cpu.r[R_AX] = h;
    return 4;
}

// GetProcAddress(hModule, lpProcName) -> FARPROC
// Windows: The address of a function a module exports, by name or by ordinal (a name pointer whose
// selector is 0); NULL if it exports no such function.
// w16: The same for the program and its DLLs, from their entry tables. A Windows module's function
// is answered with the far address w16 catches its calls at, so the program can call it through the
// pointer.
static int api_GetProcAddress(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    uint32_t far = w16_arg32(a);
    char fn[64] = "";
    if ((far >> 16) && far_name(w, far, fn, sizeof fn) < 0) return -1;
    uint32_t r = 0;
    const char *bmod = w16_thunk_module(w, h);
    struct w16_dll *d = w16_dll_of(w, h);
    if (bmod) {
        unsigned ord = (far >> 16) ? w16_builtin_ordinal(bmod, fn) : (uint16_t)far;
        if (ord) r = (uint32_t)h << 16 | ord;
    } else {
        const struct ne_module *m = d ? &d->ne : &w->ne;
        const uint16_t *sel = d ? d->seg_sel : w->seg_sel;
        unsigned ord = (far >> 16) ? ne_export_find(m, fn) : (uint16_t)far;
        uint16_t seg, off;
        if (ord && ne_entry(m, ord, &seg, &off) == 0) r = (uint32_t)sel[seg] << 16 | off;
    }
    if (g_w16_trace) {
        fprintf(g_w16_trace, "  GetProcAddress %04x %s -> %04x:%04x\n", h, (far >> 16) ? fn : "(ordinal)",
                (unsigned)(r >> 16), (unsigned)(r & 0xFFFF));
        fflush(g_w16_trace);
    }
    w->cpu.r[R_AX] = (uint16_t)r;
    w->cpu.r[R_DX] = (uint16_t)(r >> 16);
    return 6;
}

// GetModuleFileName(hModule, lpFilename, nSize) -> int
// Windows: The full DOS path of the file a module was loaded from.
// w16: The program's or a DLL's path, as the DOS path it has on the drives the program sees.
static int api_GetModuleFileName(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    uint32_t far = w16_arg32(a);
    int16_t cap = (int16_t)w16_arg16(a);
    struct w16_dll *d = w16_dll_of(w, h);
    char dos[300];
    if (w16_host_to_dos(d ? d->path : w->path, dos, sizeof dos) < 0) snprintf(dos, sizeof dos, "C:\\%s.EXE", w->ne.name);
    w->cpu.r[R_AX] = 0;
    if (cap <= 0) return 8;
    size_t n = strlen(dos);
    if (n >= (size_t)cap) n = (size_t)cap - 1;
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, (unsigned)n + 1, 1);
    if (!p) return -1;
    memcpy(p, dos, n);
    p[n] = 0;
    w->cpu.r[R_AX] = (uint16_t)n;
    return 8;
}

const struct w16_api g_module_api[] = {
    { 47, api_GetModuleHandle },
    { 49, api_GetModuleFileName },
    { 50, api_GetProcAddress },
    { 95, api_LoadLibrary },
    { 96, api_FreeLibrary },
};
const unsigned g_module_api_n = sizeof g_module_api / sizeof g_module_api[0];
