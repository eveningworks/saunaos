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

int w16_load(struct w16 *w, const uint8_t *img, uint32_t size, const char *path, const char *tail) {
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

    // Segments: data copied in, the rest zero, DGROUP grown by heap+stack;
    // then every relocation bound (module16.c).
    char why[160];
    struct w16_image im = { &w->ne, w->seg_sel, path };
    if (w16_load_segments(w, &im, (uint32_t)w->ne.heap + w->ne.stack, why, sizeof why) < 0 ||
        w16_bind_image(w, &im, why, sizeof why) < 0)
        return -1;

    // The PSP (ES at start): the command line at 81h, its length at 80h.
    // NUL-TERMINATED AND WITH NO SPACE BEFORE IT, unlike DOS's tail: a
    // program's lpCmdLine is this very address (InitTask's ES:BX), and
    // Notepad opens it as a file name as it stands. Wine lays it out so.
    uint8_t *psp = calloc(1, 256);
    w->sysmem = calloc(1, SYSDATA_SIZE);
    if (!psp || !w->sysmem) return load_fail(path, "out of memory");
    psp[0] = 0xCD; psp[1] = 0x20;
    size_t tl = tail ? strlen(tail) : 0;
    if (tl > 126) return load_fail(path, "its command line is longer than DOS's 126 characters");
    psp[0x80] = (uint8_t)tl;
    if (tl) memcpy(psp + 0x81, tail, tl);
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
    // The stack sits on the data and the local heap after it, so the heap
    // can grow (mem16.c), as Windows laid a DGROUP out.
    const struct ne_segment *ad = &w->ne.seg[w->ne.autodata - 1];
    uint32_t data = (ad->minalloc > ad->length ? ad->minalloc : ad->length);
    c->r[R_SP] = w->ne.sp ? w->ne.sp : (uint16_t)((data + w->ne.stack) & 0xFFFE);
    (void)dg;
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
    w16_local_init(w);               // InitTask's job on Windows: the heap after the stack

    pthread_mutex_init(&w->lock, 0);
    pthread_cond_init(&w->cond, 0);
    w16_register_builtins(w);
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
    uint16_t v;
    if (w16_kernel_equate(module, ordinal, &v)) return 1;
    if (!strcmp(module, "KERNEL") || !strcmp(module, "KRNL386"))
        return in_table(g_kernel_api, g_kernel_api_n, ordinal) ||
               in_table(g_kernel_mem_api, g_kernel_mem_api_n, ordinal) ||
               in_table(g_kernel_res_api, g_kernel_res_api_n, ordinal) ||
               in_table(g_profile_api, g_profile_api_n, ordinal) ||
               in_table(g_file_api, g_file_api_n, ordinal) ||
               in_table(g_module_api, g_module_api_n, ordinal);
    if (!strcmp(module, "USER"))
        return in_table(g_user_api, g_user_api_n, ordinal) ||
               in_table(g_user_misc_api, g_user_misc_api_n, ordinal) ||
               in_table(g_dialog_api, g_dialog_api_n, ordinal) ||
               in_table(g_menu_api, g_menu_api_n, ordinal) ||
               in_table(g_scroll_api, g_scroll_api_n, ordinal) ||
               in_table(g_clip_api, g_clip_api_n, ordinal);
    if (!strcmp(module, "GDI")) return in_table(g_gdi_api, g_gdi_api_n, ordinal) || in_table(g_meta_api, g_meta_api_n, ordinal);
    if (!strcmp(module, "SHELL")) return in_table(g_shell_api, g_shell_api_n, ordinal) || in_table(g_reg_api, g_reg_api_n, ordinal);
    if (!strcmp(module, "OLECLI")) return in_table(g_olecli_api, g_olecli_api_n, ordinal);
    if (!strcmp(module, "SOUND")) return in_table(g_sound_api, g_sound_api_n, ordinal);
    if (!strcmp(module, "KEYBOARD")) return in_table(g_keyboard_api, g_keyboard_api_n, ordinal);
    if (!strcmp(module, "COMMDLG")) return in_table(g_commdlg_api, g_commdlg_api_n, ordinal);
    if (!strcmp(module, "DDEML")) return in_table(g_ddeml_api, g_ddeml_api_n, ordinal);
    if (!strcmp(module, "MMSYSTEM")) return in_table(g_mmsystem_api, g_mmsystem_api_n, ordinal);
    if (!strcmp(module, "WIN87EM")) return in_table(g_win87em_api, g_win87em_api_n, ordinal);
    return 0;
}

// The stand-ins, by module and ordinal. A handler that stops being one
// leaves this table in the same change, or the launcher goes on calling
// it partly.
static const struct { const char *module; uint16_t ordinal; const char *note; } PARTLY[] = {
    { "COMMDLG", 20,  "says there is no printer" },                     // PrintDlg
    { "GDI",     38,  "a printer's call; refused" },                    // Escape
    { "DDEML",   2,   "no other program: no network play" },            // DdeInitialize
    { "DDEML",   7,   "finds no one to connect to" },                       // DdeConnect
    { "DDEML",   11,  "no conversation to send in" },                       // DdeClientTransaction
    { "DDEML",   16,  "no data ever arrives" },                             // DdeGetData
    { "DDEML",   27,  "names a service no one can reach" },                 // DdeNameService
    { "GDI",     47,  "a region is its bounding rectangle" },               // CombineRgn
    { "GDI",     53,  "no printer to make a DC for" },                  // CreateDC
    { "GDI",     123, "a palette is not realised" },                    // PlayMetaFile
    { "KERNEL",  166, "runs nothing: answers that the file is not there" }, // WinExec
    { "OLECLI",  12,  "pastes a picture, never an object to edit" },    // OleCreateFromClip
    { "OLECLI",  21,  "no program can edit the object" },               // OleActivate
    { "OLECLI",  9,   "no server: nothing can be linked" },             // OleQueryLinkFromClip
    { "OLECLI",  10,  "pastes a picture, never an object to edit" },    // OleQueryCreateFromClip
    { "OLECLI",  11,  "no server: nothing can be linked" },             // OleCreateLinkFromClip
    { "OLECLI",  34,  "no server: no new object" },                     // OleCreate
    { "OLECLI",  38,  "no server: no new object" },                     // OleCreateFromFile
    { "OLECLI",  39,  "no server: nothing can be linked" },             // OleCreateLinkFromFile
    { "SHELL",   1,   "the registration database is empty" },           // RegOpenKey
    { "SHELL",   3,   "the registration database is empty" },           // RegCloseKey
    { "SHELL",   6,   "the registration database is empty" },           // RegQueryValue
    { "SHELL",   7,   "the registration database is empty" },           // RegEnumKey
    { "SHELL",   9,   "nothing can be dropped on a program yet" },      // DragAcceptFiles
    { "SHELL",   11,  "answers no files" },                             // DragQueryFile
    { "SHELL",   12,  "does nothing" },                                 // DragFinish
    { "USER",    141, "a program's own formats stay in that program" },  // SetClipboardData
    { "USER",    31,  "always no, even when minimised" },       // IsIconic
    { "USER",    50,  "finds no other program's window" },              // FindWindow
    { "USER",    121, "only WH_CALLWNDPROC is called" },                    // SetWindowsHook
    { "USER",    291, "only WH_CALLWNDPROC is called" },                    // SetWindowsHookEx
    { "USER",    416, "shows no menu and answers FALSE" },                  // TrackPopupMenu
    { "USER",    130, "keeps no class word and answers 0" },                // SetClassWord
    { "USER",    99,  "chooses nothing" },                                  // DlgDirSelect
    { "USER",    100, "lists nothing" },                                    // DlgDirList
    { "USER",    267, "changes no scroll bar" },                            // ShowScrollBar
    { "USER",    61,  "repaints the whole window; its children do not move" },   // ScrollWindow
    { "USER",    456, "answers no icon" },                                  // LoadIconHandler
    { "KERNEL",  64,  "answers an error: no file handle for a resource" },  // AccessResource
    { "GDI",     21,  "takes off only a whole side of the clip" },          // ExcludeClipRect
};

const char *w16_partly(const char *module, unsigned ordinal) {
    if (!strcmp(module, "KRNL386")) module = "KERNEL";
    for (unsigned i = 0; i < sizeof PARTLY / sizeof PARTLY[0]; i++)
        if (PARTLY[i].ordinal == ordinal && !strcmp(PARTLY[i].module, module))
            return w16_has(module, ordinal) ? PARTLY[i].note : 0;
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
    const char *name = w16_thunk_module(w, sel);
    if (!name) name = "?";
    if (!strcmp(name, W16_FPE_MODULE)) return w16_fpu_irq_return(w);
    if (!strcmp(name, W16_HOST_MODULE)) {        // w16's own window procedure, called by the program
        struct w16_args ha = { c, 10 };
        return w16_host_proc_call(w, ordinal, &ha);
    }
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
    else if (!strcmp(name, "SHELL")) {
        r = w16_api_call(g_shell_api, g_shell_api_n, w, ordinal, &a);
        if (r == -2) r = w16_api_call(g_reg_api, g_reg_api_n, w, ordinal, &a);
    }
    else if (!strcmp(name, "OLECLI")) r = w16_api_call(g_olecli_api, g_olecli_api_n, w, ordinal, &a);
    else if (!strcmp(name, "SOUND")) r = w16_api_call(g_sound_api, g_sound_api_n, w, ordinal, &a);
    else if (!strcmp(name, "KEYBOARD")) r = w16_api_call(g_keyboard_api, g_keyboard_api_n, w, ordinal, &a);
    else if (!strcmp(name, "COMMDLG")) r = w16_api_call(g_commdlg_api, g_commdlg_api_n, w, ordinal, &a);
    else if (!strcmp(name, "DDEML")) r = w16_api_call(g_ddeml_api, g_ddeml_api_n, w, ordinal, &a);
    else if (!strcmp(name, "MMSYSTEM")) r = w16_api_call(g_mmsystem_api, g_mmsystem_api_n, w, ordinal, &a);
    else if (!strcmp(name, "WIN87EM")) r = w16_api_call(g_win87em_api, g_win87em_api_n, w, ordinal, &a);
    if (g_w16_trace && r >= 0) {                 // what it answered, as Wine's +relay ends a call with Ret
        fprintf(g_w16_trace, "  = %04x:%04x\n", c->r[R_DX], c->r[R_AX]);
        fflush(g_w16_trace);
    }
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

uint16_t w16_sys_push(struct w16 *w, unsigned len) {
    unsigned at = (w->sys_top + 3) & ~3u;
    if (at + len > SYSDATA_SIZE) {                // nested past any real program's depth
        cpu16_set_fault(&w->cpu, FAULT_API, "w16's scratch area overflowed");
        return 0;
    }
    memset(w->sysmem + at, 0, len);
    w->sys_top = at + len;
    return (uint16_t)at;
}

void w16_sys_pop(struct w16 *w, uint16_t at) {
    if (at < w->sys_top) w->sys_top = at;
}

uint32_t w16_call_far(struct w16 *w, uint16_t sel, uint16_t off, const uint16_t *words, unsigned n) {
    struct cpu16 *c = &w->cpu;
    uint16_t saved[8], ss[4], flags = c->flags;
    memcpy(saved, c->r, sizeof saved);
    memcpy(ss, c->sreg, sizeof ss);
    for (unsigned i = 0; i < n; i++) cpu16_push(c, words[i]);
    c->sreg[S_DS] = w->dgroup;       // what an instance thunk gave a callback
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

uint32_t w16_call_wndproc(struct w16 *w, uint16_t sel, uint16_t off, uint16_t hwnd,
                          uint16_t msg, uint16_t wparam, uint32_t lparam) {
    const uint16_t words[] = { hwnd, msg, wparam, (uint16_t)(lparam >> 16), (uint16_t)lparam };
    return w16_call_far(w, sel, off, words, 5);
}

// --- the queue -------------------------------------------------------------

void w16_post_msg_mods(struct w16 *w, uint16_t hwnd, uint16_t msg, uint16_t wparam, uint32_t lparam,
                       unsigned mods) {
    pthread_mutex_lock(&w->lock);
    // ONE MOVE PENDING, as Windows kept it: a WM_MOUSEMOVE replaces the
    // queued one for the same window and buttons when no other mouse
    // message came after it, so a program busy drawing a drag catches up
    // with the pointer instead of replaying every place it passed.
    if (msg == WM_MOUSEMOVE)
        for (unsigned i = w->qtail; i != w->qhead; ) {
            i = (i + W16_QUEUE - 1) % W16_QUEUE;
            struct w16_msg *q = &w->q[i];
            if (q->message < WM_MOUSEMOVE || q->message > 0x0209) continue;
            if (q->message == WM_MOUSEMOVE && q->hwnd == hwnd && q->wparam == wparam) {
                q->lparam = lparam;
                q->mods = (uint8_t)mods;
                pthread_mutex_unlock(&w->lock);
                return;
            }
            break;
        }
    unsigned next = (w->qtail + 1) % W16_QUEUE;
    if (next != w->qhead) {
        w->q[w->qtail] = (struct w16_msg){ hwnd, msg, wparam, lparam, (uint8_t)mods };
        w->qtail = next;
    }
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->lock);
}

void w16_post_msg(struct w16 *w, uint16_t hwnd, uint16_t msg, uint16_t wparam, uint32_t lparam) {
    w16_post_msg_mods(w, hwnd, msg, wparam, lparam, 0);
}

void w16_task_post_arg(struct w16 *w, int code, int arg) {
    pthread_mutex_lock(&w->lock);
    struct uapp *a = w->app;
    if (!a && code == W16_POST_SYSCMD) w->pending_syscmd = arg;   // on_open applies it
    if (!a && code == W16_POST_ABOVE) w->pending_above = arg + 1;
    if (!a && (code == W16_POST_DONE || code == W16_POST_FAULT)) w->pending_end = code;   // ...and posts it
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
    // The stack's top words, as Wine's crash dump shows them: the return
    // addresses there say who called into the fault.
    struct cpu16 *mc = &w->cpu;
    int fault = mc->fault;
    size_t len = strlen(g_w16_fault);
    len += (size_t)snprintf(g_w16_fault + len, sizeof g_w16_fault - len, "\nstack");
    for (unsigned i = 0; i < 16 && len < sizeof g_w16_fault - 6; i++) {
        const uint8_t *p = cpu16_ptr(mc, c->fault_sreg[S_SS], (uint16_t)(c->fault_r[R_SP] + 2 * i), 2, 0);
        if (!p) break;
        len += (size_t)snprintf(g_w16_fault + len, sizeof g_w16_fault - len, " %04X", p[0] | p[1] << 8);
    }
    mc->fault = fault;                           // a word past the stack's end is not the fault reported
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
    if (w16_dll_start_pending(w) == 0) cpu16_run(c, 0);
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
