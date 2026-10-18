#ifndef ABI_SANDBOX_ABI_H
#define ABI_SANDBOX_ABI_H
// A process's SANDBOX: what it has PROMISED to use (SYS_PLEDGE) and which
// paths it can still see (SYS_UNVEIL) -- OpenBSD's pledge(2) and
// unveil(2), the shape and the names. docs/decisions/kernel.md, "A
// sandbox is a promise set and a path list", has why this shape.
//
// **PROMISES ONLY SHRINK; THE UNVEIL LIST IS OPEN UNTIL IT IS LOCKED**
// (SYS_UNVEIL(NULL)), OpenBSD's rule -- a program unveils its few paths
// and then locks. Both are INHERITED by a fork, a spawn and an exec, so
// a child can never hold a promise its parent gave up, nor see past a
// list its parent locked.
//
// What a broken promise costs is OpenBSD's split. A syscall from a group
// the process did not promise KILLS it with SIGSYS and a crash report
// naming the call -- an exploit gets no second try. A path outside the
// unveiled list answers -ENOENT, as if it did not exist, because an
// engine asking for a file that is not there is ordinary and must not
// cost the page.
#include <stdint.h>

// --- promises: one bit per GROUP of syscalls ---------------------------
//
// Every row of abi/syscall_rows.h names exactly one, so a new syscall
// cannot be added without saying which group it is in. A few handlers
// ask for a second bit by ARGUMENT, the way pledge's `open` reads its
// flags: open for writing wants WPATH, with O_CREAT also CPATH; a setting
// write wants SETTINGS_WRITE; kill of another process wants PROC; an
// SHM_PUBLIC object wants PUBLISH.
#define PROMISE_STDIO          (1u << 0)  // memory, threads, futexes, time, signals, held fds
#define PROMISE_RPATH          (1u << 1)  // open to read, stat, list a directory
#define PROMISE_WPATH          (1u << 2)  // open to write, truncate, fsync
#define PROMISE_CPATH          (1u << 3)  // create, unlink, rename, mkdir, link, chmod
#define PROMISE_SHM            (1u << 4)  // shm objects: open, create PRIVATE ones, grant
#define PROMISE_PUBLISH        (1u << 5)  // create an SHM_PUBLIC object -- a server's beacon
#define PROMISE_PROC           (1u << 6)  // spawn, fork, exec, signal others, process info
#define PROMISE_INET           (1u << 7)  // sockets and the network's own calls
#define PROMISE_WINDOW         (1u << 8)  // the compositor's calls: windows and input
#define PROMISE_TTY            (1u << 9)  // change a terminal: termios, ptys, its group
#define PROMISE_SETTINGS       (1u << 10) // read settings
#define PROMISE_SETTINGS_WRITE (1u << 11) // change them
#define PROMISE_QUERY          (1u << 12) // the query registry (its wall clock is stdio's), PCI, CPUs
#define PROMISE_DEVICE         (1u << 13) // a driver's calls: BARs, IRQs, DMA, USB, sound rings
#define PROMISE_ADMIN          (1u << 14) // the machine's: mount, mkfs, time, modules, power
#define PROMISE_DIAG           (1u << 15) // the diagnostic registry: answer, or ASK, `diag NAME`.
                                          // Its own bit because asking the compositor's
                                          // name drives the desktop (`gui key`)
#define PROMISE_ALL            0xFFFFu

// Each bit's WORD, in bit order: pledge()'s strings, the kernel's log
// line and `ps --sandbox` all read this one list. OpenBSD's word where
// it has one.
#define PROMISE_WORDS { "stdio", "rpath", "wpath", "cpath", "shm", "publish", "proc", \
                        "inet", "window", "tty", "settings", "settings_write", "query", \
                        "device", "admin", "diag" }

// --- unveil rights -------------------------------------------------------
#define UNVEIL_R 0x1   // read: open to read, stat, list
#define UNVEIL_W 0x2   // write an existing file
#define UNVEIL_C 0x4   // create or remove beneath it
#define UNVEIL_X 0x8   // spawn or exec what is there

// How many paths one process may unveil. pledge's callers name a
// handful; past this SYS_UNVEIL answers -E2BIG.
#define UNVEIL_MAX 16

// **AN SHM NAME IS UNVEILED WITH THIS PREFIX**, "shm:<name>" -- an
// object name never starts with '/', so it cannot collide with a path.
// Once a process has unveiled anything, it may open a PUBLIC object only
// if that object's name is unveiled EXACTLY, or up to a '/' in it --
// "shm:font" does not cover "font.0" -- or "shm:" itself is: a beacon is how a process reaches a SERVICE, and a sandboxed
// page must reach only the two it was started for. An object created by
// the process, or GRANTED to it, needs no unveil -- the grant is already
// a capability naming this process.
#define UNVEIL_SHM_PREFIX "shm:"

#endif
