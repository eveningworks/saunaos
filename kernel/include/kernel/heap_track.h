#ifndef KERNEL_HEAP_TRACK_H
#define KERNEL_HEAP_TRACK_H

#include <stdint.h>

// What ONE context allocated, net -- for a leak check in a KTEST.
//
// heap_used_bytes() is the whole machine's: with the desktop running and
// processes on every core, another context's kmalloc inside a test's
// window reads as the test's leak, and the preemption guard cannot stop
// it (it holds this core only). This counts only the blocks the calling
// context took and gave back -- a scheduler slot while one runs, else
// this core's kernel context -- so a thread that moves between cores
// keeps its count.
//
// A free is charged to whoever frees: a block the watched context frees
// that another made is credited to it (which can hide a leak that
// small), and one another context frees for it is not. So a
// leak check asserts "did not grow", not "is exactly zero". A KTEST run
// from the debug console has no slot, so everything that core's kernel
// context does inside the window is charged too.
//
// ONE CONTEXT AT A TIME: a second heap_track_begin() moves the watch,
// and says so in the log. KTESTs run one after another.
void heap_track_begin(void);
int64_t heap_track_end(void);   // net bytes since begin; stops the watch

#endif
