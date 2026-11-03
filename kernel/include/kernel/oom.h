#ifndef OOM_H
#define OOM_H

#include <stdint.h>

// THE OUT-OF-MEMORY KILLER (kernel/mm/oom.c): when a ring-3 page fault
// finds no frame, the process holding the most memory is ended and the
// fault retried, instead of whichever process happened to fault next
// dying. Linux's mm/oom_kill.c, sized for here: the score is PRIVATE
// memory (what ending it gives back) and nothing else, there is no
// oom_score_adj, and the only processes spared are the ones whose loss
// takes the machine with them -- init, kernel threads and the compositor.
//
// Only DEMAND faults reach it (heap, stack, mmap, a copy-on-write
// break): fork, exec, shm creation and ramfs refuse with -ENOMEM as
// before. The user reserve (api/pmm.h) is what leaves the kernel room
// to run all of this.

// A demand-fault site found no frame for the current context. Called
// just before that site returns its failure, so the caller at the top
// -- the #PF handler, vmm_validate_user_range() -- can tell "out of
// memory" from "a wild pointer" with oom_fault_take().
void oom_note(void);

// Did the current context's last failed fault-in fail for want of a
// frame? Clears the note: one failure, one answer.
int oom_fault_take(void);

// Make room for the current context's failed fault at `vaddr` (`err`
// as the #PF error code), then retry it. Call only after
// oom_fault_take() said yes. **MAY SLEEP** -- it needs a scheduler slot
// and no preemption guard, which a ring-3 #PF and a syscall's fault-in
// both have.
//   1  the page is mapped now
//   0  the CURRENT process is the one to end -- chosen, nothing else
//      could be, or no memory came back -- and SIGKILL is pending on it
//  -1  nothing could be done from here (no slot, the guard raised): the
//      caller fails the fault exactly as it would have without this
int oom_recover_fault(uint64_t pml4_phys, uint64_t vaddr, uint64_t err);

// Since boot: processes the killer ended, and the last one's pid, name
// and size. For meminfo and the oom test.
uint64_t oom_kills(void);
int oom_last_victim(int *pid, char *name, unsigned cap, uint64_t *bytes);

// kernel.oom_kill_allocating_task -- Linux's sysctl of the same name:
// on, the process that faulted is ended rather than the biggest, which
// is how this machine behaved before the killer existed. Off by default.
int oom_kill_allocating(void);
void oom_set_kill_allocating(int on);

#endif
