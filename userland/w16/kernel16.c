// kernel16.c -- KERNEL's exports and the DOS calls (INT 21h) a Windows
// program makes. Stage 2: what a program needs to start and to end.
#include "w16_internal.h"

#include <stdio.h>
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

// THE PROFILE (INI) CALLS ANSWER THEIR DEFAULTS: no file is read or
// written yet, so a program starts as on a first run and a write is
// accepted and dropped. Real INI files are stage 3's.
static int profile(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    switch (ordinal) {
    case 57: case 127: {                         // Get[Private]ProfileInt(app, key, default[, file])
        w16_arg32(a); w16_arg32(a);
        c->r[R_AX] = w16_arg16(a);
        if (ordinal == 127) w16_arg32(a);
        return ordinal == 57 ? 10 : 14;
    }
    case 58: case 128: {                         // Get[Private]ProfileString(app, key, default, buf, size[, file])
        w16_arg32(a); w16_arg32(a);
        uint32_t def = w16_arg32(a), buf = w16_arg32(a);
        uint16_t size = w16_arg16(a);
        if (ordinal == 128) w16_arg32(a);
        char d[256];
        if (far_str(w, def, d, sizeof d) < 0) return -1;
        int n = put_str(w, buf, d, size);
        if (n < 0) return -1;
        c->r[R_AX] = (uint16_t)n;
        return ordinal == 58 ? 18 : 22;
    }
    case 59: case 129:                           // Write[Private]ProfileString: accepted
        c->r[R_AX] = 1;
        return ordinal == 59 ? 12 : 16;
    default:
        return -2;
    }
}

int w16_kernel(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    int r = w16_kernel_mem(w, ordinal, a);
    if (r != -2) return r;
    if ((r = w16_kernel_res(w, ordinal, a)) != -2) return r;
    if ((r = profile(w, ordinal, a)) != -2) return r;
    char s[256], t[256];
    switch (ordinal) {
    case 1:                                      // FatalExit(code)
        snprintf(s, sizeof s, "the program called FatalExit(%u)", cpu16_arg(c, 0));
        cpu16_set_fault(c, FAULT_API, s);
        return -1;
    case 47:                                     // GetModuleHandle(name): this program's
        w16_arg32(a);
        c->r[R_AX] = w->dgroup;
        return 4;
    case 51: {                                   // MakeProcInstance(lpProc, hInst): the proc itself
        uint32_t p = w16_arg32(a);                // -- every callback is entered with DS = DGROUP
        w16_arg16(a);
        c->r[R_AX] = (uint16_t)p;
        c->r[R_DX] = (uint16_t)(p >> 16);
        return 6;
    }
    case 52:                                     // FreeProcInstance(lpProc)
        w16_arg32(a);
        return 4;
    case 88: case 89: {                          // lstrcpy, lstrcat(dst, src) -> dst
        uint32_t dst = w16_arg32(a), src = w16_arg32(a);
        if (far_str(w, src, s, sizeof s) < 0) return -1;
        if (ordinal == 89 && far_str(w, dst, t, sizeof t) < 0) return -1;
        if (ordinal == 89) { strncat(t, s, sizeof t - strlen(t) - 1); memcpy(s, t, sizeof s); }
        if (put_str(w, dst, s, (unsigned)strlen(s) + 1) < 0) return -1;
        c->r[R_AX] = (uint16_t)dst;
        c->r[R_DX] = (uint16_t)(dst >> 16);
        return 8;
    }
    case 90: {                                   // lstrlen(s)
        if (far_str(w, w16_arg32(a), s, sizeof s) < 0) return -1;
        c->r[R_AX] = (uint16_t)strlen(s);
        return 4;
    }
    case 102:                                    // DOS3Call: INT 21h by a far call, same registers
        return w16_int(c, 0x21) < 0 ? -1 : 0;
    case 107:                                    // SetErrorMode(mode)
        w16_arg16(a);
        c->r[R_AX] = 0;
        return 2;
    case 115:                                    // OutputDebugString(s): to stderr
        if (far_str(w, w16_arg32(a), s, sizeof s) < 0) return -1;
        fprintf(stderr, "w16: %s: debug: %s\n", w->path, s);
        return 4;
    case 132:                                    // GetWinFlags: protected mode, enhanced, a 386
        c->r[R_AX] = 0x0001 | 0x0020 | 0x0004;
        c->r[R_DX] = 0;
        return 0;
    case 137: {                                  // FatalAppExit(action, lpMessage)
        w16_arg16(a);
        if (far_str(w, w16_arg32(a), s, sizeof s) < 0) return -1;
        snprintf(t, sizeof t, "the program called FatalAppExit: %s", s);
        cpu16_set_fault(c, FAULT_API, t);
        return -1;
    }
    case 207:                                    // IsDBCSLeadByte(b): no double-byte sets
        w16_arg16(a);
        c->r[R_AX] = 0;
        return 2;
    case 3:                                      // GetVersion: Windows 3.10 on DOS 5.0
        c->r[R_AX] = 0x0A03;
        c->r[R_DX] = 0x0500;
        return 0;
    case 30:                                     // WaitEvent(hTask)
        w16_arg16(a);
        c->r[R_AX] = 0;
        return 2;
    case 91:                                     // InitTask
        init_task(w);
        return 0;
    default:
        return -2;
    }
}

// INT 21h: the DOS calls a Windows program still makes. 4Ch ends it.
int w16_int(struct cpu16 *c, uint8_t n) {
    if (n != 0x21) return -1;
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
        return -1;
    }
}
