// task16.c -- one Win16 program: loading it into the CPU's segments,
// dispatching its imports, its thread, and its message queue.
//
// LOADING (what Windows' KERNEL did at LoadModule): every NE segment gets
// a host buffer and a selector; the automatic data segment (DGROUP) is
// its data plus the local heap plus the stack, at most 64 KiB; each
// imported module gets a THUNK selector whose offsets are its ordinals;
// then every relocation is applied, following the chain a non-additive
// one heads. The task starts with the registers Windows gave it.
#include "w16_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/uapp.h"
#include "w16/w16prefs.h"

struct w16 g_w16;
pthread_mutex_t g_w16_text_lock = PTHREAD_MUTEX_INITIALIZER;
char g_w16_fault[1024];
FILE *g_w16_trace;
struct w16_prefs g_w16_prefs;
char g_w16_fault_box[512];

#define SYSDATA_SIZE 0x1000

static int load_fail(const char *path, const char *why) {
    fprintf(stderr, "w16: %s: %s\n", path, why);
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

// One relocation record of segment `n`: its target, then every site of
// its chain (just the one when additive).
static int relocate(struct w16 *w, unsigned n, const struct ne_reloc *r, uint8_t *mem, uint32_t len) {
    uint16_t sel, off;
    char why[96];
    switch (r->flags & 3) {
    case NE_REL_INTERNAL: {
        unsigned seg = r->a & 0xFF;
        off = r->b;
        if (seg == 0xFF) {                  // a movable segment: through the entry table
            uint16_t s16;
            if (ne_entry(&w->ne, r->b, &s16, &off) < 0)
                return load_fail(w->path, "a relocation names a missing entry point");
            seg = s16;
        }
        if (seg < 1 || seg > w->ne.nseg) return load_fail(w->path, "a relocation names no segment");
        sel = w->seg_sel[seg];
        break;
    }
    case NE_REL_IMPORT_ORD:
        if (r->a < 1 || r->a > w->ne.nmod) return load_fail(w->path, "an import names no module");
        sel = w->thunk_sel[r->a];
        off = r->b;
        break;
    case NE_REL_IMPORT_NAME:
        snprintf(why, sizeof why, "imports from %s by name, which w16 does not resolve yet",
                 r->a >= 1 && r->a <= w->ne.nmod ? w->modname[r->a] : "?");
        return load_fail(w->path, why);
    default:
        return load_fail(w->path, "uses floating-point fixups, which w16 does not support yet");
    }
    int additive = (r->flags & NE_REL_ADDITIVE) != 0;
    uint16_t at = r->site;
    for (unsigned guard = 0; guard < 0x10000; guard++) {
        uint16_t next = (uint32_t)at + 2 <= len ? (uint16_t)(mem[at] | mem[at + 1] << 8) : 0xFFFF;
        if (apply_value(mem, len, at, r->src, additive, sel, off) < 0) {
            snprintf(why, sizeof why, "a relocation in segment %u lies outside it", n);
            return load_fail(w->path, why);
        }
        if (additive || next == 0xFFFF) return 0;
        at = next;
    }
    return load_fail(w->path, "a relocation chain loops");
}

int w16_load(struct w16 *w, const uint8_t *img, uint32_t size, const char *path) {
    w->path = path;
    int err = ne_parse(img, size, &w->ne);
    if (err < 0) return load_fail(path, ne_strerror(err));
    if (w->ne.flags & 0x8000) return load_fail(path, "is a library (DLL), not a program");
    if (!w->ne.autodata) return load_fail(path, "has no automatic data segment");

    w->desc = calloc(CPU16_MAX_DESC, sizeof *w->desc);
    if (!w->desc) return load_fail(path, "out of memory");
    struct cpu16 *c = &w->cpu;
    memset(c, 0, sizeof *c);
    c->desc = w->desc;

    // Segments: data copied in, the rest zero, DGROUP grown by heap+stack.
    for (unsigned n = 1; n <= w->ne.nseg; n++) {
        const struct ne_segment *s = &w->ne.seg[n - 1];
        uint32_t want = s->minalloc > s->length ? s->minalloc : s->length;
        if (n == w->ne.autodata) want += (uint32_t)w->ne.heap + w->ne.stack;
        if (want > 0x10000) {
            if (n == w->ne.autodata) return load_fail(path, "its data, heap and stack exceed 64 KiB");
            want = 0x10000;
        }
        if (want == 0) want = 16;
        uint8_t *mem = calloc(1, 0x10000);   // a whole segment: a later resize needs no move
        if (!mem) return load_fail(path, "out of memory");
        if (s->file_off) memcpy(mem, img + s->file_off, s->length);
        int data = (s->flags & NE_SEG_DATA) != 0;
        w->seg_sel[n] = cpu16_alloc(c, data ? D_DATA : D_CODE, mem, want - 1, (uint16_t)n);
        if (!w->seg_sel[n]) return load_fail(path, "too many segments");
    }
    // Imported modules: a thunk selector each.
    for (unsigned i = 1; i <= w->ne.nmod; i++) {
        if (ne_module_name(&w->ne, i, w->modname[i], sizeof w->modname[i]) < 0)
            return load_fail(path, "an imported module's name is malformed");
        w->thunk_sel[i] = cpu16_alloc(c, D_THUNK, 0, 0xFFFF, 0);
    }
    for (unsigned n = 1; n <= w->ne.nseg; n++) {
        const struct ne_segment *s = &w->ne.seg[n - 1];
        uint8_t *mem = w->desc[cpu16_index(w->seg_sel[n])].base;
        uint32_t len = w->desc[cpu16_index(w->seg_sel[n])].limit + 1;
        for (unsigned i = 0; i < s->nrelocs; i++) {
            struct ne_reloc r;
            if (ne_reloc_at(&w->ne, n, i, &r) < 0 || relocate(w, n, &r, mem, len) < 0) return -1;
        }
    }

    // The PSP (ES at start): an empty command tail.
    uint8_t *psp = calloc(1, 256);
    w->sysmem = calloc(1, SYSDATA_SIZE);
    if (!psp || !w->sysmem) return load_fail(path, "out of memory");
    psp[0] = 0xCD; psp[1] = 0x20;
    psp[0x80] = 0; psp[0x81] = 0x0D;
    w->psp = cpu16_alloc(c, D_DATA, psp, 255, 0);
    // The environment, whose selector the PSP holds at 2Ch: what a C
    // runtime's start-up scans. Its strings, an empty one, a count of 1,
    // then the program's own path -- a DOS path, as Windows had it.
    static const char vars[] = "PATH=C:\\WINDOWS\0TEMP=C:\\TEMP\0\0\1\0";
    char dospath[80] = "C:\\WINDOWS\\";
    const char *base = strrchr(w->path, '/');
    base = base ? base + 1 : w->path;
    size_t pl = strlen(dospath);
    for (size_t i = 0; base[i] && pl < sizeof dospath - 1; i++)
        dospath[pl++] = base[i] >= 'a' && base[i] <= 'z' ? (char)(base[i] - 32) : base[i];
    dospath[pl] = 0;
    uint8_t *env = calloc(1, 256);
    if (!env) return load_fail(path, "out of memory");
    memcpy(env, vars, sizeof vars - 1);
    memcpy(env + sizeof vars - 1, dospath, pl + 1);
    uint16_t env_sel = cpu16_alloc(c, D_DATA, env, 255, 0);
    psp[0x2C] = (uint8_t)env_sel;
    psp[0x2D] = (uint8_t)(env_sel >> 8);
    w->sysdata = cpu16_alloc(c, D_DATA, w->sysmem, SYSDATA_SIZE - 1, 0);
    w->dgroup = w->seg_sel[w->ne.autodata];

    // The registers a Windows task starts with.
    const struct cpu16_desc *dg = &w->desc[cpu16_index(w->dgroup)];
    c->sreg[S_CS] = w->seg_sel[w->ne.entry_cs];
    c->ip = w->ne.entry_ip;
    c->sreg[S_SS] = w->seg_sel[w->ne.ss ? w->ne.ss : w->ne.autodata];
    c->r[R_SP] = w->ne.sp ? w->ne.sp : (uint16_t)((dg->limit + 1) & 0xFFFE);
    c->sreg[S_DS] = w->dgroup;
    c->sreg[S_ES] = w->psp;
    c->r[R_BX] = w->ne.stack;
    c->r[R_CX] = w->ne.heap;
    c->r[R_DI] = w->dgroup;          // hInstance is the DGROUP selector
    c->r[R_SI] = 0;                  // no previous instance
    c->flags = F_IF | 0x0002;
    c->user = w;
    c->thunk = 0;                    // set by the task thread
    c->intr = w16_int;
    w16_local_init(w);               // InitTask's job on Windows: the heap below the stack

    pthread_mutex_init(&w->lock, 0);
    pthread_cond_init(&w->cond, 0);
    w->text_color = 0x000000;
    w->bk_color = 0xFFFFFF;
    w->bk_mode = 2;                  // OPAQUE
    return 0;
}

// --- imports -------------------------------------------------------------

int w16_api_call(const struct w16_api *t, unsigned n, struct w16 *w, unsigned ordinal,
                 struct w16_args *a) {
    unsigned lo = 0, hi = n;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (t[mid].ordinal == ordinal) return t[mid].fn(w, ordinal, a);
        if (t[mid].ordinal < ordinal) lo = mid + 1;
        else hi = mid;
    }
    return -2;
}

static int in_table(const struct w16_api *t, unsigned n, unsigned ordinal) {
    unsigned lo = 0, hi = n;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (t[mid].ordinal == ordinal) return 1;
        if (t[mid].ordinal < ordinal) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

int w16_has(const char *module, unsigned ordinal) {
    if (!strcmp(module, "KERNEL") || !strcmp(module, "KRNL386"))
        return in_table(g_kernel_api, g_kernel_api_n, ordinal) ||
               in_table(g_kernel_mem_api, g_kernel_mem_api_n, ordinal) ||
               in_table(g_kernel_res_api, g_kernel_res_api_n, ordinal) ||
               in_table(g_profile_api, g_profile_api_n, ordinal);
    if (!strcmp(module, "USER"))
        return in_table(g_user_api, g_user_api_n, ordinal) ||
               in_table(g_user_misc_api, g_user_misc_api_n, ordinal);
    if (!strcmp(module, "GDI")) return in_table(g_gdi_api, g_gdi_api_n, ordinal);
    return 0;
}

uint16_t w16_arg16(struct w16_args *a) {
    a->pos -= 2;
    return cpu16_arg(a->c, a->pos);
}

uint32_t w16_arg32(struct w16_args *a) {
    a->pos -= 4;
    return (uint32_t)cpu16_arg(a->c, a->pos) | (uint32_t)cpu16_arg(a->c, a->pos + 2) << 16;
}

static int thunk(struct cpu16 *c, uint16_t sel, uint16_t ordinal) {
    struct w16 *w = c->user;
    unsigned mod = 0;
    for (unsigned i = 1; i <= w->ne.nmod; i++)
        if (w->thunk_sel[i] == sel) mod = i;
    const char *name = mod ? w->modname[mod] : "?";
    const struct w16_name *n = w16_name_lookup(name, ordinal);
    struct w16_args a = { c, n && n->argbytes > 0 ? (unsigned)n->argbytes : 0 };
    int r = -1;
    if (g_w16_trace) {
        // The arguments as words, first pushed first: the Pascal order.
        char args[64] = "";
        unsigned nw = n && n->argbytes > 0 ? (unsigned)n->argbytes / 2 : 0, len = 0;
        for (unsigned i = 0; i < nw && i < 8 && len < sizeof args - 6; i++)
            len += (unsigned)snprintf(args + len, sizeof args - len, "%s%04x", i ? " " : "",
                                      cpu16_arg(c, (nw - 1 - i) * 2));
        fprintf(g_w16_trace, "%s.%u %s(%s) from %04x:%04x\n", name, ordinal,
                n ? n->name : "?", args, cpu16_arg(c, -4 + 2), cpu16_arg(c, -4));
        fflush(g_w16_trace);                     // a fault ends in abort(), which flushes nothing
    }
    if (!strcmp(name, "KERNEL") || !strcmp(name, "KRNL386")) r = w16_kernel(w, ordinal, &a);
    else if (!strcmp(name, "USER")) r = w16_user(w, ordinal, &a);
    else if (!strcmp(name, "GDI")) r = w16_gdi(w, ordinal, &a);
    if (r == -2 || (r < 0 && !c->fault)) {
        char why[96];
        snprintf(why, sizeof why, "%s.%u (%s) is not implemented", name, ordinal,
                 n ? n->name : "unknown");
        snprintf(w->missing, sizeof w->missing, "%s.%u (%s)", name, ordinal, n ? n->name : "unknown");
        cpu16_set_fault(c, FAULT_API, why);
        return -1;
    }
    return r;
}

uint16_t w16_sys_alloc(struct w16 *w, unsigned len) {
    // One structure at a time is all stage 2 hands over (a CREATESTRUCT),
    // so the area is reused from 0; a ring comes with nested ones.
    if (len > SYSDATA_SIZE) len = SYSDATA_SIZE;
    memset(w->sysmem, 0, len);
    return 0;
}

uint32_t w16_call_wndproc(struct w16 *w, uint16_t sel, uint16_t off, uint16_t hwnd,
                          uint16_t msg, uint16_t wparam, uint32_t lparam) {
    struct cpu16 *c = &w->cpu;
    uint16_t saved[8], ss[4], flags = c->flags;
    memcpy(saved, c->r, sizeof saved);
    memcpy(ss, c->sreg, sizeof ss);
    cpu16_push(c, hwnd);
    cpu16_push(c, msg);
    cpu16_push(c, wparam);
    cpu16_push(c, (uint16_t)(lparam >> 16));
    cpu16_push(c, (uint16_t)lparam);
    c->sreg[S_DS] = w->dgroup;       // what an instance thunk gave a window procedure
    c->r[R_AX] = w->dgroup;
    cpu16_call_far(c, sel, off);
    uint32_t result = (uint32_t)c->r[R_DX] << 16 | c->r[R_AX];
    memcpy(c->r, saved, sizeof saved);
    c->sreg[S_ES] = ss[S_ES];
    c->sreg[S_DS] = ss[S_DS];
    c->sreg[S_SS] = ss[S_SS];
    c->flags = flags;
    return result;
}

// --- the queue -------------------------------------------------------------

void w16_post_msg(struct w16 *w, uint16_t hwnd, uint16_t msg, uint16_t wparam, uint32_t lparam) {
    pthread_mutex_lock(&w->lock);
    unsigned next = (w->qtail + 1) % W16_QUEUE;
    if (next != w->qhead) {
        w->q[w->qtail] = (struct w16_msg){ hwnd, msg, wparam, lparam };
        w->qtail = next;
    }
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->lock);
}

void w16_task_post_arg(struct w16 *w, int code, int arg) {
    pthread_mutex_lock(&w->lock);
    struct uapp *a = w->app;
    if (!a && code == W16_POST_SYSCMD) w->pending_syscmd = arg;   // on_open applies it
    pthread_mutex_unlock(&w->lock);
    if (a) uapp_post(a, code, arg);
}

void w16_task_post(struct w16 *w, int code) { w16_task_post_arg(w, code, 0); }

static const char *base_of(const char *p) {
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}

// Why the program stopped, twice: the crash report's message (a headline,
// then where and the registers) and the 3.1 box's sentence.
static void describe_fault(struct w16 *w) {
    const struct cpu16 *c = &w->cpu;
    const struct cpu16_desc *d = &w->desc[cpu16_index(c->fault_cs) % CPU16_MAX_DESC];
    const char *file = base_of(w->path);
    char at[16];
    snprintf(at, sizeof at, "%04X:%04X", d->owner, c->fault_ip);
    int api = c->fault == FAULT_API;
    snprintf(g_w16_fault, sizeof g_w16_fault,
             "%s: %s%s%s\n"
             "%s stopped at %s (%s, segment %u): %s -- %s\n"
             "AX %04X  BX %04X  CX %04X  DX %04X\n"
             "SI %04X  DI %04X  BP %04X  SP %04X\n"
             "CS %04X  DS %04X  SS %04X  ES %04X  IP %04X",
             file, api ? c->fault_why : cpu16_fault_name(c->fault), api ? "" : " at ", api ? "" : at,
             w->path, at, w->ne.name, d->owner, cpu16_fault_name(c->fault), c->fault_why,
             c->fault_r[R_AX], c->fault_r[R_BX], c->fault_r[R_CX], c->fault_r[R_DX],
             c->fault_r[R_SI], c->fault_r[R_DI], c->fault_r[R_BP], c->fault_r[R_SP],
             c->fault_cs, c->fault_sreg[S_DS], c->fault_sreg[S_SS], c->fault_sreg[S_ES], c->fault_ip);
    char upper[64];
    size_t i = 0;
    for (; file[i] && i < sizeof upper - 1; i++)
        upper[i] = file[i] >= 'a' && file[i] <= 'z' ? (char)(file[i] - 32) : file[i];
    upper[i] = 0;
    if (api && w->missing[0])
        snprintf(g_w16_fault_box, sizeof g_w16_fault_box,
                 "%s called %s, which w16 does not have yet.\n\nThe program will close.",
                 w->ne.name, w->missing);
    else if (api)
        snprintf(g_w16_fault_box, sizeof g_w16_fault_box,
                 "%s stopped: %s.\n\nThe program will close.", w->ne.name, c->fault_why);
    else
        snprintf(g_w16_fault_box, sizeof g_w16_fault_box,
                 "%s caused a %s in module %s at %s.\n\nThe program will close.",
                 w->ne.name, cpu16_fault_name(c->fault), upper, at);
}

void *w16_task_main(void *arg) {
    struct w16 *w = arg;
    struct cpu16 *c = &w->cpu;
    c->thunk = thunk;
    cpu16_run(c, 0);
    if (c->fault) {
        const struct cpu16_desc *d = &w->desc[cpu16_index(c->fault_cs) % CPU16_MAX_DESC];
        fprintf(stderr, "w16: %s: %s at %u:%04x -- %s\n", w->path, cpu16_fault_name(c->fault),
                d->owner, c->fault_ip, c->fault_why);
        describe_fault(w);
        set_abort_message("%s", g_w16_fault);
        w->exit_code = 1;
    } else {
        w->exit_code = c->exit_code;
    }
    pthread_mutex_lock(&w->lock);
    w->task_done = 1;
    pthread_cond_broadcast(&w->cond);
    pthread_mutex_unlock(&w->lock);
    // A fault is shown before the program goes; the main thread aborts
    // once the box is answered.
    w16_task_post(w, c->fault ? W16_POST_FAULT : W16_POST_DONE);
    return 0;
}
