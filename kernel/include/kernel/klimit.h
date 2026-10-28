#ifndef KLIMIT_H
#define KLIMIT_H

// A kernel table's CEILING: sized from RAM the first time it is asked,
// and reported by ONE log line the first time a request is refused at it.
//
// The table itself grows on demand; the ceiling only stops one runaway
// spending the kernel heap on metadata. There is no setting and no
// rlimit behind it -- the number follows the machine, as Linux's
// fs.file-max and inotify's max_user_watches do (docs/decisions/kernel.md,
// "Kernel limits are sized from RAM, not settings or rlimits").
//
//     static struct klimit g_lim = KLIMIT("fswatch: path watches", 64,
//                                         sizeof(struct watch), 16, 1u << 20);
//     if (live >= klimit_get(&g_lim)) { klimit_hit(&g_lim); return -ENOSPC; }
//
// Interrupt-safe to READ once computed; klimit_hit() takes no lock (an
// atomic exchange decides who logs).
#include <stdint.h>

struct klimit {
    const char *what;      // the log line's subject
    uint32_t ram_div;      // the table may use RAM / ram_div; 0 = fixed at `floor`
    uint32_t entry_bytes;  // what one entry costs the kernel
    uint32_t floor;        // never below -- what the fixed table used to hold
    uint32_t ceiling;      // never above -- an index type or a scan bounds it
    uint32_t value;        // 0 until the first klimit_get()
    uint32_t logged;       // klimit_hit() has spoken
};

#define KLIMIT(what, div, bytes, lo, hi) { (what), (div), (uint32_t)(bytes), (lo), (hi), 0, 0 }
// A ceiling that does NOT scale, kept for a reason stated where it is
// declared -- still reported once when hit.
#define KLIMIT_FIXED(what, n) { (what), 0, 0, (n), (n), 0, 0 }

// The arithmetic alone: ram / div / bytes, clamped to [lo, hi].
uint32_t klimit_from_ram(uint64_t ram, uint32_t div, uint32_t bytes, uint32_t lo, uint32_t hi);

// The ceiling. Computed once from pmm_total_frames(); before the frame
// allocator is up it answers `floor` without caching.
uint32_t klimit_get(struct klimit *l);

// A request was refused at the ceiling: logs the first time per boot.
void klimit_hit(struct klimit *l);

// FOR A KTEST: replace the ceiling (0 = recompute from RAM) and re-arm
// the log line. Returns the value it replaced.
uint32_t klimit_test_set(struct klimit *l, uint32_t value);

// FOR A KTEST: whether `l`'s line is in the log after byte `since`
// (klog_total_bytes() taken before the refusal).
int klimit_test_logged(const struct klimit *l, uint64_t since);

#endif
