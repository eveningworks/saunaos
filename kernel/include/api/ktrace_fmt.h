#ifndef KTRACE_FMT_H
#define KTRACE_FMT_H

// One kernel-trace record as one line of text, and the group names
// `kernel.trace` takes -- ONE SOURCE COMPILED TWICE (kernel/lib/, also in
// libbase), so the panic tail the kernel writes into the panic record
// and `/bin/trace show` print the same line for the same record. The
// event table is abi/ktrace_abi.h's KTRACE_EVENTS.
#include <stddef.h>
#include <stdint.h>
#include "ktrace_abi.h"
#ifdef __cplusplus
extern "C" {
#endif

struct ktrace_event_desc {
    uint32_t id, group;
    const char *name;
    const char *args;   // "name:kind ..." -- see KTRACE_EVENTS
};

// The row for an event id, or NULL for one this build does not know.
const struct ktrace_event_desc *ktrace_event_desc(unsigned id);
// Every row, in id order: `*n` of them.
const struct ktrace_event_desc *ktrace_event_table(unsigned *n);

struct ktrace_fmt_ctx {
    uint64_t t0_tsc;    // printed as time 0 (the trace's start)
    uint64_t tsc_hz;    // 0: times print as raw ticks
    // A syscall number's name, or NULL; the hook may itself be NULL.
    const char *(*syscall_name)(unsigned nr);
};

// "   1.234567 c0 [12] switch prev=12 prev_state=S next=13 next_tgid=13".
// An unknown event prints its id and four hex arguments. Returns the
// length, or 0 -- with nothing written -- when it does not fit `cap`.
int ktrace_fmt_rec(char *out, size_t cap, const struct ktrace_rec *r,
                   const struct ktrace_fmt_ctx *c);

// "switch,syscall" or "all" (or "on") to KTRACE_G_* bits. 0 for a list
// naming anything else, or nothing: a parser rejects rather than guesses.
int ktrace_parse_groups(const char *list, uint32_t *mask);
// The other way: "switch,syscall", "all", or "off" for 0. 0 when it
// does not fit, else the length.
int ktrace_format_groups(uint32_t mask, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
