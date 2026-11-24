// A process's identity -- SYS_GETCRED and SPAWN_CREDS (abi/cred_abi.h;
// docs/users-design.md, stage A). kernel/cred.h has the rules: NULL is
// root, a cred is shared and never changed once made.
#include "cred.h"
#include "cred_abi.h"
#include "syscalls.h"
#include "scheduler.h"
#include "heap.h"
#include "klog.h"
#include "kfmt.h"
#include "string.h"
#include "errno.h"
#include "vmm.h"

struct cred {
    int      refs;   // __atomic: a slot_free() on another core drops one
    uint32_t ruid, euid, suid;
    uint32_t rgid, egid, sgid;
    uint32_t ngroups;
    uint32_t groups[];
};

struct cred *cred_get(struct cred *c) {
    if (c) __atomic_add_fetch(&c->refs, 1, __ATOMIC_RELAXED);
    return c;
}

void cred_put(struct cred *c) {
    if (c && __atomic_sub_fetch(&c->refs, 1, __ATOMIC_ACQ_REL) == 0) kfree(c);
}

uint32_t cred_euid(const struct cred *c) { return c ? c->euid : 0; }
uint32_t cred_ruid(const struct cred *c) { return c ? c->ruid : 0; }
uint32_t cred_egid(const struct cred *c) { return c ? c->egid : 0; }
uint32_t cred_rgid(const struct cred *c) { return c ? c->rgid : 0; }

void cred_describe(const struct cred *c, struct cred_info *out, uint32_t *groups, uint32_t cap) {
    k_memset(out, 0, sizeof *out);
    if (!c) return;
    out->ruid = c->ruid; out->euid = c->euid; out->suid = c->suid;
    out->rgid = c->rgid; out->egid = c->egid; out->sgid = c->sgid;
    out->ngroups = c->ngroups;
    if (groups)
        for (uint32_t i = 0; i < c->ngroups && i < cap; i++) groups[i] = c->groups[i];
}

struct cred *cred_current(void) {
    struct cred **slot = scheduler_current_cred();
    return slot ? *slot : 0;
}

// --- SPAWN_CREDS's arm ------------------------------------------------------

// Keyed by the calling THREAD, for the reason the sandbox's arm is
// (kernel/proc/sandbox.c): the spawn sleeps loading the program, and a
// sibling thread's spawn must not take this child's identity.
#define ARMS 8
static struct { int tid; struct cred *c; } g_arm[ARMS];

int cred_arm_spawn(uint32_t uid, uint32_t gid, const uint32_t *groups, uint32_t ngroups) {
    if (ngroups > CRED_GROUPS_MAX) return -EINVAL;
    struct cred *c = kmalloc(sizeof *c + (size_t)ngroups * sizeof c->groups[0]);
    if (!c) return -ENOMEM;
    c->refs = 1;
    c->ruid = c->euid = c->suid = uid;
    c->rgid = c->egid = c->sgid = gid;
    c->ngroups = ngroups;
    for (uint32_t i = 0; i < ngroups; i++) c->groups[i] = groups[i];

    int tid = scheduler_current_pid();
    int rc = -EBUSY;
    scheduler_preempt_disable();
    for (int i = 0; i < ARMS; i++) {
        if (g_arm[i].tid) continue;
        g_arm[i].tid = tid;
        g_arm[i].c = c;
        rc = 0;
        break;
    }
    scheduler_preempt_enable();
    if (rc) {
        kfree(c);
        klog_printf(KLOG_WARN "cred: %d spawns arming an identity at once -- refused\n", ARMS);
    }
    return rc;
}

void cred_disarm(void) { cred_disarm_tid(scheduler_current_pid()); }

void cred_disarm_tid(int tid) {
    struct cred *drop = 0;
    if (tid <= 0) return;
    scheduler_preempt_disable();
    for (int i = 0; i < ARMS; i++)
        if (g_arm[i].tid == tid) { drop = g_arm[i].c; g_arm[i].tid = 0; g_arm[i].c = 0; }
    scheduler_preempt_enable();
    cred_put(drop);   // outside the guard: kfree takes the heap lock
}

struct cred *cred_for_child(void) {
    int tid = scheduler_current_pid();
    for (int i = 0; tid && i < ARMS; i++)
        if (g_arm[i].tid == tid) return cred_get(g_arm[i].c);
    return cred_get(cred_current());
}

// --- SYS_GETCRED ------------------------------------------------------------

int sys_getcred(struct syscall_ctx *c) {
    struct cred *me = cred_current();
    struct cred_info info;
    cred_describe(me, &info, 0, 0);
    uint32_t cap = (uint32_t)c->a2;
    if (!vmm_copy_to_user(c->pml4, c->a0, &info, sizeof info)) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    uint32_t n = info.ngroups < cap ? info.ngroups : cap;
    if (n && !vmm_copy_to_user(c->pml4, c->a1, me->groups, (uint64_t)n * sizeof me->groups[0])) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    c->regs[14] = 0;
    return 0;
}
