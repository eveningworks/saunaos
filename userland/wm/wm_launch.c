// WIN_REQ_LAUNCH, the compositor's half: a program started for a client that may not start one
// (abi/win_proto.h has the contract). THE POLICY IS AN ALLOW-LIST OF ONE: /bin/w16 with "--" and an
// existing file -- a Win16 program starting another (userland/w16/shell16.c). A client asks with
// pieces of each argument; the last message starts it and is answered with the pid.
#include "wm_internal.h"
#include "lib/ugrow.h"
#include "kapi.h"
#include "rt/sys.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define W16_BIN "/bin/w16"

struct launch_asm {
    int pid;
    int nargs, bad;
    char arg[WIN_LAUNCH_ARGS][WIN_LAUNCH_ARG_MAX];
};
static struct launch_asm *g_asm;
static int g_nasm, g_asm_cap;

static struct launch_asm *asm_of(int pid) {
    for (int i = 0; i < g_nasm; i++) if (g_asm[i].pid == pid) return &g_asm[i];
    if (!UGROW(g_asm, g_asm_cap, g_nasm + 1)) return 0;
    struct launch_asm *a = &g_asm[g_nasm++];
    memset(a, 0, sizeof *a);
    a->pid = pid;
    return a;
}

static void asm_done(int pid) {
    for (int i = 0; i < g_nasm; i++)
        if (g_asm[i].pid == pid) { g_asm[i] = g_asm[--g_nasm]; return; }
}

static int start(const struct launch_asm *a) {
    struct stat st;
    if (a->bad || a->nargs < 3 || strcmp(a->arg[0], W16_BIN) || strcmp(a->arg[1], "--") || a->arg[2][0] != '/')
        return -EPERM;
    if (stat(a->arg[2], &st) != 0) return -ENOENT;
    if (!S_ISREG(st.st_mode)) return -EPERM;
    char *argv[WIN_LAUNCH_ARGS + 1];
    for (int i = 0; i < a->nargs; i++) argv[i] = (char *)a->arg[i];
    argv[a->nargs] = 0;
    int pid = sys_spawn_argv(W16_BIN, argv);
    if (pid > 0) wm_track_launched(pid);
    return pid > 0 ? pid : -EPERM;
}

// One message of a launch from `pid`; 1 with *reply set when it was the last.
int wm_launch_piece(int pid, int a, const char *text, int *reply) {
    struct launch_asm *as = asm_of(pid);
    if (!as) {
        if (a != WIN_LAUNCH_END) return 0;
        *reply = -ENOMEM;
        return 1;
    }
    if (a == WIN_LAUNCH_END) {
        *reply = start(as);
        if (*reply < 0) {
            char line[96];
            snprintf(line, sizeof line, "wm: launch for pid %d refused (%d)\n", pid, *reply);
            sys_eprint(line);
        }
        asm_done(pid);
        return 1;
    }
    if (a < 0 || a >= WIN_LAUNCH_ARGS || a > as->nargs) { as->bad = 1; return 0; }
    if (a == as->nargs) as->nargs++;
    size_t l = strlen(as->arg[a]), n = strlen(text);
    if (l + n >= WIN_LAUNCH_ARG_MAX) as->bad = 1;
    else memcpy(as->arg[a] + l, text, n + 1);
    return 0;
}

// The client went away mid-launch: what it sent is dropped.
void wm_launch_forget(int pid) { asm_done(pid); }
