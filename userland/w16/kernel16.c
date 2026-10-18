// kernel16.c -- KERNEL's exports and the DOS calls (INT 21h) a Windows
// program makes. Stage 2: what a program needs to start and to end.
#include "w16_internal.h"

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

int w16_kernel(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    switch (ordinal) {
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
    case 0x30:                                   // DOS version: 5.0
        c->r[R_AX] = 0x0005;
        return 0;
    case 0x4C:                                   // terminate with a code
        c->exit_code = c->r[R_AX] & 0xFF;
        c->exited = 1;
        return 0;
    default:
        return -1;
    }
}
