#ifndef KERNEL_SANDBOX_H
#define KERNEL_SANDBOX_H
// A process's sandbox -- the kernel half of SYS_PLEDGE and SYS_UNVEIL
// (abi/sandbox_abi.h has the contract, kernel/proc/sandbox.c the code).
//
// NULL MEANS UNRESTRICTED, and almost every process has NULL: the cost
// of the check to them is one pointer test in syscall_dispatch(). A
// sandbox is shared, refcounted, between a process and the children
// that inherited it, and copied before a pledge or unveil changes it.
#include <stdint.h>

struct sandbox;

// --- lifetime (the scheduler's) ----------------------------------------
struct sandbox *sandbox_get(struct sandbox *sb);   // +1 reference; NULL passes through
void sandbox_put(struct sandbox *sb);              // -1; frees at zero

// What a child spawned by the CURRENT process starts with: the caller's
// sandbox, narrowed by the promises sys_spawn() armed for it
// (SPAWN_SANDBOX). `*out` is a reference the slot owns, NULL for an
// unrestricted child. -ENOMEM when a narrowed one could not be made, and
// then the spawn must fail: a child started free is the one outcome a
// sandboxed spawn may not have.
int sandbox_for_child(struct sandbox **out);

// The answer when sandbox_for_child() failed past the point a spawn can
// be unwound: a shared, never-freed sandbox with NO promises, so the
// child ends at its first syscall rather than running unrestricted.
void sandbox_lockout(struct sandbox **slot);

// SPAWN_SANDBOX's half. Armed by the asker and consumed by its own
// spawn, the strace arm's shape (kernel/proc/strace.c): a spawn can sleep
// loading its ELF, so "the next spawn anywhere" would be somebody else's.
// -EBUSY when every arm is taken -- the caller then starts no child.
int  sandbox_arm_spawn(uint32_t promises);
void sandbox_disarm(void);

// --- the checks (the current process) ----------------------------------

// The dispatcher's: 1 if the current process may make a syscall in
// group `promise`.
int sandbox_allows(uint32_t promise);

// A handler's second bit, asked by ARGUMENT (open for writing, a
// setting write, kill of another pid). 0 when the call may go ahead; 1
// when the bit is not held and the process has been ENDED, exactly as
// the dispatcher ends it -- and then **THE HANDLER MUST RETURN AT ONCE**,
// touching nothing: the process is gone and the trapframe is the next
// one's, as after SYS_EXIT. `call` names it in the log and the report.
int sandbox_require(uint32_t promise, const char *call, uint64_t *regs);

// The dispatcher's when sandbox_allows() said no: logs, writes the crash
// report and ends the process with SIGSYS. Returns -- the caller's
// trapframe is the next process's by then, as after SYS_EXIT.
void sandbox_violation(const char *call, uint32_t promise, uint64_t *regs);

// May the current process reach `abs` (an already-resolved absolute
// path) with UNVEIL_* rights `need`? 0, -ENOENT (outside every unveiled
// path) or -EACCES (unveiled, without that right).
//
// **ON THE RESOLVED STRING, NOT THE INODE**, and that is sound only
// while the filesystem follows no symlink (tfs3.c does not): one that is
// followed must be checked again at its target, as OpenBSD's namei does.
int sandbox_path(const char *abs, unsigned need);

// May the current process open the PUBLIC shm object `name` it neither
// created nor was granted? (UNVEIL_SHM_PREFIX in abi/sandbox_abi.h.)
int sandbox_shm_public(const char *name);

// Is the current process sandboxed at all? Cheap.
int sandbox_active(void);

// For proc_info: the promises of the process in `slot`, or PROMISE_ALL.
uint32_t sandbox_promises(const struct sandbox *sb);

#endif
