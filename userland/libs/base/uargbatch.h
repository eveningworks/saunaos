#ifndef ULIB_UARGBATCH_H
#define ULIB_UARGBATCH_H

// A LONG ARGUMENT LIST SPLIT ACROSS AS MANY SPAWNS AS IT NEEDS -- xargs'
// job against ARG_MAX, here against SPAWN_ARGS_MAX: a spawn's whole
// argv and environment, every string's NUL included, fit in that many
// bytes or the spawn is refused with E2BIG. Each batch leaves room for
// `environ`, which `run` must therefore pass (sys_spawn() does). A caller with a selection of any size
// (`/bin/trash put` over a desktop's icons) hands the fixed head and the
// items here instead of building one argv that may not fit.

// Calls `run` once per batch with a NULL-terminated argv: the `nhead`
// strings of `head` (the program and its fixed options), then as many of
// `items` as fit, in order. Returns how many batches ran. An item that
// cannot fit even alone is skipped, the rest still run, and the result
// is -1 -- as is out of memory for the argv, which runs nothing.
int uargbatch(char *const *head, int nhead, char *const *items, int nitems,
              void (*run)(char *const *argv, void *ctx), void *ctx);

#endif
