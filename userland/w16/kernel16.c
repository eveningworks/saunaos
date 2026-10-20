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

// GetProfileInt(lpAppName, lpKeyName, nDefault) -> UINT
// GetPrivateProfileInt(lpAppName, lpKeyName, nDefault, lpFileName) -> UINT
// Windows: A number from an INI file -- WIN.INI, or the program's own -- by section and key: the
// value's leading digits, or the default when the key is not there.
// w16: The same, read from the files on drive C:: a file named without a path is in C:\WINDOWS, as
// Windows kept them. A user's own WIN.INI there is read as it is.
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

// GetProfileString(lpAppName, lpKeyName, lpDefault, lpReturnedString, nSize) -> int
// GetPrivateProfileString(lpAppName, lpKeyName, lpDefault, lpReturnedString, nSize, lpFileName) -> int
// Windows: A string from an INI file by section and key, or the default; a NULL section lists the
// sections, a NULL key the section's keys, each ended by a 0. Returns the characters copied.
// w16: The same, from the files in C:\WINDOWS (GetProfileInt says where).
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

// WriteProfileString(lpAppName, lpKeyName, lpString) -> BOOL
// WritePrivateProfileString(lpAppName, lpKeyName, lpString, lpFileName) -> BOOL
// Windows: Writes a key's value into an INI file, making the file and section if need be; a NULL
// value removes the key, a NULL key the whole section.
// w16: The same, in C:\WINDOWS -- which is where a program's settings, such as Minesweeper's best
// times, live between runs.
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

// FatalExit(Code) -> void
// Windows: Ends the program at once; in the debugging version of Windows it first showed where.
// w16: Ends the program with Windows 3.1's Application Error box and a crash report naming the
// code.
static int api_FatalExit(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    char s[256];
    (void)ordinal;
    (void)a;
    snprintf(s, sizeof s, "the program called FatalExit(%u)", cpu16_arg(c, 0));
    cpu16_set_fault(c, FAULT_API, s);
    return -1;
}


// MakeProcInstance(lpProc, hInstance) -> FARPROC
// Windows: Makes a small thunk that loads the instance's data segment before calling a callback --
// a window or dialog procedure, a timer -- so the callback finds its own data.
// w16: Returns the procedure itself: w16 enters every callback with the program's data segment
// already loaded, so no thunk is needed.
static int api_MakeProcInstance(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    uint32_t p = w16_arg32(a);                // -- every callback is entered with DS = DGROUP
    w16_arg16(a);
    c->r[R_AX] = (uint16_t)p;
    c->r[R_DX] = (uint16_t)(p >> 16);
    return 6;
}

// FreeProcInstance(lpProc) -> void
// Windows: Frees a thunk MakeProcInstance made.
// w16: Nothing to free (MakeProcInstance says why).
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

// lstrcpy(lpString1, lpString2) -> LPSTR
// lstrcat(lpString1, lpString2) -> LPSTR
// Windows: Copy a string, or add it to the end of another, by far pointers; return the destination.
// w16: The same, every byte checked to lie inside the segment it names.
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

// lstrlen(lpString) -> int
// Windows: A string's length in bytes.
// w16: The same.
static int api_lstrlen(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    char *s = far_dup(w, w16_arg32(a));
    if (!s) return -1;
    c->r[R_AX] = (uint16_t)strlen(s);
    free(s);
    return 4;
}

// The one task: its PSP stands for it, and DX the head of the task list, which is the same task.
// GetCurrentTask() -> HTASK
// Windows: The running task's handle -- and, in DX, the first task of the list of all of them.
// w16: The program's one task, both times: each program runs in a w16 of its own.
static int api_GetCurrentTask(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    (void)a;
    w->cpu.r[R_AX] = w->cpu.r[R_DX] = w->psp;
    return 0;
}

// CATCH AND THROW are Win16's setjmp and longjmp: Catch(lpCatchBuf) keeps
// where it will return and the registers a C function keeps (BP SI DI DS,
// SS:SP), and answers 0; Throw(lpCatchBuf, nThrow) returns from that
// Catch a second time, answering nThrow. Register calls in Wine's spec,
// so their arguments are read off the stack here, not by w16_args. A
// Throw out of a callback w16 itself called (a window procedure inside
// SendMessage) would leave w16's C frames behind: refused, with a reason.
#define CATCH_WORDS 9

// Catch(lpCatchBuf) -> int
// Windows: Win16's setjmp: keeps where it was called from and the registers a C function keeps, and
// returns 0; a later Throw with that buffer returns from it again with Throw's value.
// w16: The same, for any Throw that does not cross a callback w16 itself is in the middle of
// calling (Throw says why).
static int api_Catch(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    (void)a;
    struct cpu16 *c = &w->cpu;
    uint16_t off = cpu16_arg(c, 0), sel = cpu16_arg(c, 2);
    const uint8_t *ret = cpu16_ptr(c, c->sreg[S_SS], c->r[R_SP], 4, 0);
    uint8_t *buf = cpu16_ptr(c, sel, off, 2 * CATCH_WORDS, 1);
    if (!ret || !buf) return -1;
    uint16_t v[CATCH_WORDS] = {
        (uint16_t)(ret[0] | ret[1] << 8), (uint16_t)(ret[2] | ret[3] << 8), (uint16_t)(c->r[R_SP] + 8),
        c->r[R_BP], c->r[R_SI], c->r[R_DI], c->sreg[S_DS], c->sreg[S_SS], (uint16_t)c->depth,
    };
    memcpy(buf, v, sizeof v);
    c->r[R_AX] = 0;
    return 4;
}

// Throw(lpCatchBuf, nThrow) -> void
// Windows: Win16's longjmp: returns from the Catch that filled the buffer a second time, answering
// nThrow -- how a program gets out of a deep error.
// w16: The same, from anywhere the program's own calls reach. A Throw out of a window procedure
// that w16 is calling (inside SendMessage, say) would leave w16's own work half done, so it ends
// the program with a crash report saying so instead; no program run so far does it.
static int api_Throw(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    (void)a;
    struct cpu16 *c = &w->cpu;
    uint16_t value = cpu16_arg(c, 0), off = cpu16_arg(c, 2), sel = cpu16_arg(c, 4);
    const uint8_t *buf = cpu16_ptr(c, sel, off, 2 * CATCH_WORDS, 0);
    if (!buf) return -1;
    uint16_t v[CATCH_WORDS];
    memcpy(v, buf, sizeof v);
    if (v[8] != c->depth) {
        cpu16_set_fault(c, FAULT_API, "Throw out of a window procedure w16 called, which it cannot unwind");
        return -1;
    }
    c->sreg[S_SS] = v[7];
    c->r[R_SP] = (uint16_t)(v[2] - 4);           // the Catch's return address, put back for the return
    uint8_t *ret = cpu16_ptr(c, c->sreg[S_SS], c->r[R_SP], 4, 1);
    if (!ret) return -1;
    ret[0] = (uint8_t)v[0]; ret[1] = (uint8_t)(v[0] >> 8);
    ret[2] = (uint8_t)v[1]; ret[3] = (uint8_t)(v[1] >> 8);
    c->r[R_BP] = v[3];
    c->r[R_SI] = v[4];
    c->r[R_DI] = v[5];
    c->sreg[S_DS] = v[6];
    c->r[R_AX] = value;
    return 0;                                    // the far return lands where Catch returned
}

// w16 runs no second program yet -- Hearts asks for NETDDE.EXE, the network DDE agent -- so the
// file is not found.
// WinExec(lpCmdLine, nCmdShow) -> UINT
// Windows: Starts another program from a command line, in the same Windows.
// w16: Starts nothing and answers that the file was not found (2).
// Why: A second Windows program would need a second w16 and a way for the two to see each other as
// one Windows -- which they do not have yet. Hearts asks for NETDDE.EXE, the agent for network
// play.
// Notice: Hearts goes on without network play.
// Whole: w16 starting a second program -- and for Hearts, NetDDE. On the roadmap: "w16: one program
// starting or finding another".
static int api_WinExec(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg32(a);
    w16_arg16(a);
    w->cpu.r[R_AX] = 2;                          // ERROR_FILE_NOT_FOUND
    return 6;
}

// DOS3Call() -> registers
// Windows: DOS's INT 21h by a far call: a DOS service -- open a file, read a directory, ask the
// date -- with DOS's registers in and out.
// w16: The same as w16's INT 21h: the file, directory, drive and date services programs use, on the
// drives the program sees.
static int api_DOS3Call(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    (void)a;
    return w16_int(c, 0x21) < 0 ? -1 : 0;
}

// SetErrorMode(fuErrorMode) -> UINT
// Windows: Whether Windows shows its own box for a critical error -- a drive not ready -- or leaves
// the program to see the failure; returns the old mode.
// w16: Answers 0 and keeps nothing: w16 shows no critical-error box, so a program always sees the
// failure itself.
static int api_SetErrorMode(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    c->r[R_AX] = 0;
    return 2;
}

// OutputDebugString(lpOutputString) -> void
// Windows: Sends a line to the debugger, or the debug terminal.
// w16: Writes it to w16's standard error, naming the program, where a terminal or the log shows it.
static int api_OutputDebugString(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    char s[256];
    (void)ordinal;
    if (far_str(w, w16_arg32(a), s, sizeof s) < 0) return -1;
    fprintf(stderr, "w16: %s: debug: %s\n", w->path, s);
    return 4;
}

// GetWinFlags() -> DWORD
// Windows: What Windows runs on and in: protected mode, the processor, standard or enhanced mode, a
// coprocessor.
// w16: Protected mode, a 386, enhanced mode, a coprocessor -- as __WINFLAGS says.
static int api_GetWinFlags(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    (void)a;
    c->r[R_AX] = 0x0425;                         // WF_PMODE WF_CPU386 WF_ENHANCED WF_80x87
    c->r[R_DX] = 0;
    return 0;
}

// FatalAppExit(wAction, lpMessageText) -> void
// Windows: Shows a message box with the text and ends the program.
// w16: Ends the program with the Application Error box and a crash report carrying the text.
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

// IsDBCSLeadByte(bTestChar) -> BOOL
// Windows: Whether a byte starts a two-byte character, in the Far East editions' character sets.
// w16: Always FALSE: w16 is a Western Windows, every character one byte.
static int api_IsDBCSLeadByte(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    c->r[R_AX] = 0;
    return 2;
}

// GetVersion() -> DWORD
// Windows: The Windows version in the low word -- major, then minor -- and DOS's in the high word.
// w16: The version the program's settings say -- 3.10 by default, or 3.11 or 3.00 (Retro Launcher's
// Version told) -- on DOS 5.0.
static int api_GetVersion(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    (void)a;
    c->r[R_AX] = w16_prefs_winver(&g_w16_prefs);   // the version the program is told
    c->r[R_DX] = 0x0500;
    return 0;
}

// Bytes at pData of another instance's DGROUP into the same place in this one's. There is only ever
// the one instance, so they are already there.
// GetInstanceData(hInstance, pData, cbData) -> int
// Windows: Copies bytes from a running copy of the program's data segment into this copy's -- how a
// second copy started where the first had got to.
// w16: Answers that the bytes were copied: each program runs in its own w16, so there is only ever
// the one copy and they are already there.
static int api_GetInstanceData(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    w16_arg16(a);
    w16_arg16(a);
    w->cpu.r[R_AX] = w16_arg16(a);
    return 6;
}

// WaitEvent(hTask) -> BOOL
// Windows: Waits until the task has an event; a program's start-up calls it once.
// w16: Returns at once: the start-up call is the only one programs make.
static int api_WaitEvent(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    struct cpu16 *c = &w->cpu;
    (void)ordinal;
    w16_arg16(a);
    c->r[R_AX] = 0;
    return 2;
}

// InitTask() -> registers
// Windows: A program's first call: sets up its stack, heap and command line, and answers in
// registers -- the instance, the previous instance, the command line, how to show the window.
// w16: The same, with the command line w16 was given made into the DOS path the program can open.
static int api_InitTask(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    (void)a;
    init_task(w);
    return 0;
}

// AddAtom(lpszString) -> ATOM
// DeleteAtom(nAtom) -> ATOM
// GetAtomName(nAtom, lpszBuffer, cbBuffer) -> UINT
// Windows: A number for a string in the program's atom table -- the same number for the same
// string, without case; "#123" is the integer atom 123 -- one less use of it, and its string back.
// w16: The same numbers, from the table class names and registered messages use. A deleted atom
// keeps its number until the program ends, so a later AddAtom of the string gets the same one.
static int api_AddAtom(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    char name[256];
    w->cpu.r[R_AX] = 0;
    if (ordinal == 71) { w16_arg16(a); return 2; }   // DeleteAtom: 0 is success
    if (ordinal == 72) {
        uint16_t atom = w16_arg16(a);
        uint32_t far = w16_arg32(a);
        int16_t cap = (int16_t)w16_arg16(a);
        if (atom >= W16_ATOM_BASE && (unsigned)(atom - W16_ATOM_BASE) < (unsigned)w->natoms)
            snprintf(name, sizeof name, "%s", w->atoms[atom - W16_ATOM_BASE]);
        else if (atom && atom < W16_ATOM_BASE)
            snprintf(name, sizeof name, "#%u", atom);
        else
            return 8;
        if (cap <= 0) return 8;
        size_t n = strlen(name);
        if (n > (size_t)cap - 1) n = (size_t)cap - 1;
        uint8_t *p = cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, (uint32_t)n + 1, 1);
        if (!p) return -1;
        memcpy(p, name, n);
        p[n] = 0;
        w->cpu.r[R_AX] = (uint16_t)n;
        return 8;
    }
    uint32_t far = w16_arg32(a);
    if (!(far >> 16)) { w->cpu.r[R_AX] = (uint16_t)far; return 4; }   // MAKEINTATOM
    if (cpu16_str(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, name, sizeof name) < 0) return -1;
    if (name[0] == '#') {
        unsigned v = 0;
        for (const char *p = name + 1; *p >= '0' && *p <= '9'; p++) v = v * 10 + (unsigned)(*p - '0');
        w->cpu.r[R_AX] = (uint16_t)(v < W16_ATOM_BASE ? v : 0);
        return 4;
    }
    w->cpu.r[R_AX] = w16_atom_add(w, name);
    return 4;
}

const struct w16_api g_kernel_api[] = {
    { 1, api_FatalExit },
    { 3, api_GetVersion },
    { 30, api_WaitEvent },
    { 36, api_GetCurrentTask },
    { 51, api_MakeProcInstance },
    { 52, api_FreeProcInstance },
    { 54, api_GetInstanceData },
    { 55, api_Catch },
    { 56, api_Throw },
    { 70, api_AddAtom },
    { 71, api_AddAtom },
    { 72, api_AddAtom },
    { 88, api_lstrcpy },
    { 89, api_lstrcpy },
    { 90, api_lstrlen },
    { 91, api_InitTask },
    { 102, api_DOS3Call },
    { 107, api_SetErrorMode },
    { 115, api_OutputDebugString },
    { 132, api_GetWinFlags },
    { 137, api_FatalAppExit },
    { 166, api_WinExec },
    { 207, api_IsDBCSLeadByte },
};
const unsigned g_kernel_api_n = sizeof g_kernel_api / sizeof g_kernel_api[0];

static int kernel_call(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    int r = w16_kernel_mem(w, ordinal, a);
    if (r != -2) return r;
    if ((r = w16_kernel_res(w, ordinal, a)) != -2) return r;
    if ((r = profile(w, ordinal, a)) != -2) return r;
    if ((r = w16_api_call(g_file_api, g_file_api_n, w, ordinal, a)) != -2) return r;
    if ((r = w16_api_call(g_module_api, g_module_api_n, w, ordinal, a)) != -2) return r;
    return w16_api_call(g_kernel_api, sizeof g_kernel_api / sizeof g_kernel_api[0], w, ordinal, a);
}

int w16_kernel(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    return w16_kernel_heap_call(w, ordinal, a, kernel_call);
}

// INT 21h: the DOS calls a Windows program still makes. 4Ch ends it.
// INT 75h: the coprocessor's exception (win87em16.c).
int w16_int(struct cpu16 *c, uint8_t n) {
    if (n == 0x75) return w16_fpu_irq(c->user);
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
