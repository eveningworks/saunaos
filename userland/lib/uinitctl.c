// Asking init to restart or power off the machine, or for realtime --
// see uinitctl.h.
#include <string.h>
#include "lib/uinitctl.h"
#include "lib/uchan.h"
#include "rt/sys.h"

// init answers before it starts stopping anything, so this is a round
// trip, not the shutdown; a slow answer means a wedged init, and the
// fallback below is for exactly that.
#define REPLY_MS 2000

int uinitctl_shutdown(int reboot) {
    struct uchan_client c;
    if (uchan_client_open(&c, INITCTL_SERVICE) == 0) {
        struct initctl_msg m, reply;
        memset(&m, 0, sizeof m);
        memset(&reply, 0, sizeof reply);
        m.verb = reboot ? INITCTL_REBOOT : INITCTL_POWEROFF;
        int ok = uchan_call(&c, &m, sizeof m, &reply, sizeof reply, REPLY_MS) == 0 &&
                 reply.result == INITCTL_OK;
        uchan_client_close(&c);
        if (ok) return 0;
    }
    sys_eprint("uinitctl: init did not take the shutdown -- stopping the machine directly\n");
    sys_poweroff(reboot);
    return -1;
}

// ONE CLIENT FOR THE LIFE OF THE PROCESS, never one per request: init
// knows a client by pid and keeps the ring it first mapped, so a reopen
// is not heard until it notices the old one gone (docs/bugs.md, the
// uchan entry). A failed call drops it; the next opens a fresh one.
static struct uchan_client g_rt;
static int g_rt_open;

int uinitctl_realtime(int pid, int policy, int prio, int *got_policy, int *got_prio) {
    if (!g_rt_open) {
        if (uchan_client_open(&g_rt, INITCTL_SERVICE) < 0) return -1;
        g_rt_open = 1;
    }
    struct initctl_msg m, reply;
    memset(&m, 0, sizeof m);
    memset(&reply, 0, sizeof reply);
    m.verb = INITCTL_REALTIME;
    m.pid = pid;
    m.policy = policy;
    m.prio = prio;
    if (uchan_call(&g_rt, &m, sizeof m, &reply, sizeof reply, REPLY_MS) != 0) {
        uchan_client_close(&g_rt);
        g_rt_open = 0;
        return -1;
    }
    if (reply.result == INITCTL_OK) {
        if (got_policy) *got_policy = reply.policy;
        if (got_prio) *got_prio = reply.prio;
    }
    return (int)reply.result;
}
