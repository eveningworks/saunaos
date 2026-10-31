// Kernel threads: a scheduler slot that runs a kernel function, never
// ring 3 -- Linux's kthread_run(). jbd2's commit thread is the reason
// they exist here (docs/fslock-design.md, stage 5a).
//
// A kernel thread runs with INTERRUPTS OFF, as a syscall's ring-0 half
// does, so it is never preempted: it gives the CPU up only by sleeping
// (scheduler_block_kernel(), a kmutex, a disk wait) or by returning,
// which ends it. A loop that never sleeps hangs the machine.
//
// It has the kernel's page table and its own kernel stack, and nothing
// a process has: no fds, no heap, no cwd, no parent (ppid 0, never a
// zombie), and no signals -- signal_send() refuses one, and `ps` shows
// the name in brackets.
#ifndef KTHREAD_H
#define KTHREAD_H

// Start `fn(arg)` in a new kernel thread called `name`. Returns its pid,
// or a negative errno (-EAGAIN: no slot).
int kthread_start(const char *name, void (*fn)(void *), void *arg);

// Is `pid` a kernel thread? 0 for a free or unknown pid.
int kthread_is(int pid);

#endif
