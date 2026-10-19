// kernel16.c -- KERNEL's exports and the DOS calls (INT 21h) a Windows
// program makes. Stage 2: what a program needs to start and to end.
#include "w16_internal.h"
#include "w16/w16prefs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SW_SHOWNORMAL 1

// InitTask (KERNEL.91) is a REGISTER function: no arguments, its results
// in registers -- AX=1, ES:BX the command tail, CX the stack, DX the
// show state, SI the previous instance, DI this one -- and it records the
// stack's bounds in the task's reserved words at the base of DGROUP.
static void init_task(struct w16 *w) {
    struct cpu16 *c = &w->cpu;
    uint8_t *dg = cpu16_ptr(c, w->dgroup, 0, 16, 1);
    if (dg) {
        uint16_t top = (uint16_t)(c->r[R_SP] - w->ne.stack);
        uint16_t words[3] = { top, top, c->r[R_SP] };   // pStackTop, pStackMin, pStackBot
        memcpy(dg + 0x0A, words, sizeof words);
    }
    c->r[R_AX] = 1;
    c->r[R_BX] = 0x81;
    c->r[R_CX] = w->ne.stack;
    c->r[R_DX] = SW_SHOWNORMAL;
    c->r[R_SI] = 0;
    c->r[R_DI] = w->dgroup;
    c->sreg[S_ES] = w->psp;
}

// A far string argument into out; "" for a NULL pointer.
static int far_str(struct w16 *w, uint32_t far, char *out, unsigned cap) {
    out[0] = 0;
    if (!(far >> 16)) return 0;
    if (cpu16_str(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, out, cap) < 0) {
        cpu16_set_fault(&w->cpu, FAULT_API, "a Windows call was passed a bad string");
        return -1;
    }
    return 0;
}

// Up to `cap` bytes of `s`, NUL-terminated, at a far buffer; its length.
static int put_str(struct w16 *w, uint32_t far, const char *s, unsigned cap) {
    if (!cap) return 0;
    unsigned n = (unsigned)strlen(s);
    if (n >= cap) n = cap - 1;
    uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, n + 1, 1);
    if (!p) return -1;
    memcpy(p, s, n);
    p[n] = 0;
    return (int)n;
}

// THE PROFILE (INI) CALLS, over real INI files on drive C: (ini16.c).
// GetProfile* and WriteProfile* are WIN.INI's; the Private ones name
// their file. An argument may be NULL where Windows let it be: a NULL
// section or key asks for the names, a NULL value removes a key.

static int ini_file(struct w16 *w, unsigned ordinal, unsigned private_ord, uint32_t far,
                    char *out, unsigned cap) {
    if (ordinal != private_ord) { snprintf(out, cap, "WIN.INI"); return 0; }
    return far_str(w, far, out, cap);
}

// Get[Private]ProfileInt(app, key, default[, file]): the value's leading
// digits as Windows read them, or the default when the key is absent.
static int api_GetProfileInt(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    uint32_t app = w16_arg32(a), key = w16_arg32(a);
    uint16_t dflt = w16_arg16(a);
    uint32_t file = ordinal == 127 ? w16_arg32(a) : 0;
    char sec[96], k[96], f[128], v[256];
    if (far_str(w, app, sec, sizeof sec) < 0 || far_str(w, key, k, sizeof k) < 0 ||
        ini_file(w, ordinal, 127, file, f, sizeof f) < 0)
        return -1;
    c->r[R_AX] = dflt;
    if (w16_ini_get(f, sec, k, v, sizeof v)) {
        unsigned n = 0;
        for (const char *p = v; *p >= '0' && *p <= '9'; p++) n = n * 10 + (unsigned)(*p - '0');
        c->r[R_AX] = (uint16_t)n;
    }
    return ordinal == 57 ? 10 : 14;
}

// Get[Private]ProfileString(app, key, default, buf, size[, file]): the
// characters copied. A NULL app lists the sections, a NULL key the keys.
static int api_GetProfileString(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    uint32_t app = w16_arg32(a), key = w16_arg32(a), def = w16_arg32(a), buf = w16_arg32(a);
    uint16_t size = w16_arg16(a);
    uint32_t file = ordinal == 128 ? w16_arg32(a) : 0;
    char sec[96], k[96], f[128], d[256], v[1024];
    if (far_str(w, app, sec, sizeof sec) < 0 || far_str(w, key, k, sizeof k) < 0 ||
        far_str(w, def, d, sizeof d) < 0 || ini_file(w, ordinal, 128, file, f, sizeof f) < 0)
        return -1;
    int n;
    if (!(app >> 16) || !(key >> 16)) {
        size_t got = w16_ini_names(f, (app >> 16) ? sec : 0, v, size < sizeof v ? size : sizeof v);
        uint8_t *p = size ? cpu16_ptr(c, (uint16_t)(buf >> 16), (uint16_t)buf, size, 1) : 0;
        if (size && !p) return -1;
        if (p) memcpy(p, v, got + 1 < size ? got + 1 : size);
        n = (int)got;
    } else {
        if (!w16_ini_get(f, sec, k, v, sizeof v)) snprintf(v, sizeof v, "%s", d);
        n = put_str(w, buf, v, size);
        if (n < 0) return -1;
    }
    c->r[R_AX] = (uint16_t)n;
    return ordinal == 58 ? 18 : 22;
}

// Write[Private]ProfileString(app, key, value[, file]): 1 when written.
static int api_WriteProfileString(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    uint32_t app = w16_arg32(a), key = w16_arg32(a), val = w16_arg32(a);
    uint32_t file = ordinal == 129 ? w16_arg32(a) : 0;
    char sec[96], k[96], v[256], f[128];
    if (far_str(w, app, sec, sizeof sec) < 0 || far_str(w, key, k, sizeof k) < 0 ||
        far_str(w, val, v, sizeof v) < 0 || ini_file(w, ordinal, 129, file, f, sizeof f) < 0)
        return -1;
    c->r[R_AX] = (uint16_t)w16_ini_set(f, sec, (key >> 16) ? k : 0, (val >> 16) ? v : 0);
    return ordinal == 59 ? 12 : 16;
}

const struct w16_api g_profile_api[] = {
    { 57, api_GetProfileInt },
    { 58, api_GetProfileString },
    { 59, api_WriteProfileString },
    { 127, api_GetProfileInt },
    { 128, api_GetProfileString },
    { 129, api_WriteProfileString },
};
const unsigned g_profile_api_n = sizeof g_profile_api / sizeof g_profile_api[0];

static int profile(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    return w16_api_call(g_profile_api, sizeof g_profile_api / sizeof g_profile_api[0], w, ordinal, a);
}

// FatalExit(code)
static int api_FatalExit(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    char s[256];
    (void)ordinal;
    (void)a;
    snprintf(s, sizeof s, "the program called FatalExit(%u)", cpu16_arg(c, 0));
    cpu16_set_fault(c, FAULT_API, s);
    return -1;
}

// GetModuleHandle(name): this program's
static int api_GetModuleHandle(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg32(a);
    c->r[R_AX] = w->dgroup;
    return 4;
}

// MakeProcInstance(lpProc, hInst): the proc itself
static int api_MakeProcInstance(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint32_t p = w16_arg32(a);                // -- every callback is entered with DS = DGROUP
    w16_arg16(a);
    c->r[R_AX] = (uint16_t)p;
    c->r[R_DX] = (uint16_t)(p >> 16);
    return 6;
}

// FreeProcInstance(lpProc)
static int api_FreeProcInstance(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    (void)w;
    w16_arg32(a);
    return 4;
}

// A far string of any length (a segment's worth at most), allocated; NULL
// after a fault.
static char *far_dup(struct w16 *w, uint32_t far) {
    size_t cap = 256;
    for (;;) {
        char *s = malloc(cap);
        if (!s) { cpu16_set_fault(&w->cpu, FAULT_API, "out of memory for a string"); return 0; }
        if (far_str(w, far, s, (unsigned)cap) < 0) { free(s); return 0; }
        if (strlen(s) < cap - 1 || cap >= 65536) return s;
        free(s);
        cap *= 4;
    }
}

// lstrcpy, lstrcat(dst, src) -> dst
static int api_lstrcpy(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    uint32_t dst = w16_arg32(a), src = w16_arg32(a);
    char *s = far_dup(w, src), *t = ordinal == 89 && s ? far_dup(w, dst) : 0;
    if (!s || (ordinal == 89 && !t)) { free(s); return -1; }
    size_t at = t ? strlen(t) : 0, n = strlen(s);
    uint8_t *p = cpu16_ptr(c, (uint16_t)(dst >> 16), (uint16_t)((uint16_t)dst + at), (unsigned)n + 1, 1);
    if (p) memcpy(p, s, n + 1);
    free(s);
    free(t);
    if (!p) return -1;
    c->r[R_AX] = (uint16_t)dst;
    c->r[R_DX] = (uint16_t)(dst >> 16);
    return 8;
}

// lstrlen(s)
static int api_lstrlen(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    char *s = far_dup(w, w16_arg32(a));
    if (!s) return -1;
    c->r[R_AX] = (uint16_t)strlen(s);
    free(s);
    return 4;
}

// GetProcAddress(hModule, lpProcName): no module but the program's own is
// loaded, so nothing is found -- NULL, which a program asking for an
// optional one (Pen Windows' RegisterPenApp) takes as "not here".
static int api_GetProcAddress(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);
    w16_arg32(a);
    w->cpu.r[R_AX] = 0;
    w->cpu.r[R_DX] = 0;
    return 6;
}

// DOS3Call: INT 21h by a far call, same registers
static int api_DOS3Call(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    (void)a;
    return w16_int(c, 0x21) < 0 ? -1 : 0;
}

// SetErrorMode(mode)
static int api_SetErrorMode(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    c->r[R_AX] = 0;
    return 2;
}

// OutputDebugString(s): to stderr
static int api_OutputDebugString(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    char s[256];
    (void)ordinal;
    if (far_str(w, w16_arg32(a), s, sizeof s) < 0) return -1;
    fprintf(stderr, "w16: %s: debug: %s\n", w->path, s);
    return 4;
}

// GetWinFlags: protected mode, enhanced, a 386
static int api_GetWinFlags(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    (void)a;
    c->r[R_AX] = 0x0001 | 0x0020 | 0x0004;
    c->r[R_DX] = 0;
    return 0;
}

// FatalAppExit(action, lpMessage)
static int api_FatalAppExit(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    char s[256], t[256];
    (void)ordinal;
    w16_arg16(a);
    if (far_str(w, w16_arg32(a), s, sizeof s) < 0) return -1;
    snprintf(t, sizeof t, "the program called FatalAppExit: %s", s);
    cpu16_set_fault(c, FAULT_API, t);
    return -1;
}

// IsDBCSLeadByte(b): no double-byte sets
static int api_IsDBCSLeadByte(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    c->r[R_AX] = 0;
    return 2;
}

// GetVersion: Windows 3.10 on DOS 5.0
static int api_GetVersion(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    (void)a;
    c->r[R_AX] = w16_prefs_winver(&g_w16_prefs);   // the version the program is told
    c->r[R_DX] = 0x0500;
    return 0;
}

// WaitEvent(hTask)
static int api_WaitEvent(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    c->r[R_AX] = 0;
    return 2;
}

// InitTask
static int api_InitTask(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    (void)a;
    init_task(w);
    return 0;
}

const struct w16_api g_kernel_api[] = {
    { 1, api_FatalExit },
    { 3, api_GetVersion },
    { 30, api_WaitEvent },
    { 47, api_GetModuleHandle },
    { 50, api_GetProcAddress },
    { 51, api_MakeProcInstance },
    { 52, api_FreeProcInstance },
    { 88, api_lstrcpy },
    { 89, api_lstrcpy },
    { 90, api_lstrlen },
    { 91, api_InitTask },
    { 102, api_DOS3Call },
    { 107, api_SetErrorMode },
    { 115, api_OutputDebugString },
    { 132, api_GetWinFlags },
    { 137, api_FatalAppExit },
    { 207, api_IsDBCSLeadByte },
};
const unsigned g_kernel_api_n = sizeof g_kernel_api / sizeof g_kernel_api[0];

int w16_kernel(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    int r = w16_kernel_mem(w, ordinal, a);
    if (r != -2) return r;
    if ((r = w16_kernel_res(w, ordinal, a)) != -2) return r;
    if ((r = profile(w, ordinal, a)) != -2) return r;
    if ((r = w16_api_call(g_file_api, g_file_api_n, w, ordinal, a)) != -2) return r;
    return w16_api_call(g_kernel_api, sizeof g_kernel_api / sizeof g_kernel_api[0], w, ordinal, a);
}

// INT 21h: the DOS calls a Windows program still makes. 4Ch ends it.
int w16_int(struct cpu16 *c, uint8_t n) {
    if (n != 0x21) return -1;
    if (g_w16_trace) {
        fprintf(g_w16_trace, "INT 21h AX=%04x BX=%04x CX=%04x DX=%04x\n", c->r[R_AX], c->r[R_BX], c->r[R_CX],
                c->r[R_DX]);
        fflush(g_w16_trace);
    }
    switch (c->r[R_AX] >> 8) {
    case 0x25:                                   // set an interrupt vector: kept by nobody
        return 0;
    case 0x30:                                   // DOS version: 5.0
        c->r[R_AX] = 0x0005;
        return 0;
    case 0x35:                                   // get an interrupt vector: none installed
        c->r[R_BX] = 0;
        c->sreg[S_ES] = 0;
        return 0;
    case 0x4C:                                   // terminate with a code
        c->exit_code = c->r[R_AX] & 0xFF;
        c->exited = 1;
        return 0;
    default:
        return w16_dos_file(c->user, c);
    }
}
