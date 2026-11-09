// rm -- delete a file, or a tree with -r.
//
// A FRONT END over base/ufileop.h, which owns the walk. This file is
// rm's policy: delete without asking, print what failed, and keep going
// past a failure so one unreadable file does not abandon the rest.
//
// The deepest-first ordering, the explicit queue and the reason neither
// is recursion all live in the engine now -- see base/ufileop.h.
//
// -f is POSIX's: a path that does not exist is not an error, so
// `rm -f a.o b.o` is the "make sure they are gone" a Makefile's clean
// rule means. Any OTHER failure is still reported and still fails.
#include "rt/sys.h"
#include "base/uargs.h"
#include "base/cmd.h"
#include "base/ufileop.h"
#include <errno.h>

static struct ufileop g_op;
static int recursive, force;

static const struct uargs_opt OPTS[] = {
    { "recursive", 'r', 0, "remove a directory and everything in it", &recursive, 0 },
    { "force",     'f', 0, "a path that does not exist is not an error", &force, 0 },
    { 0 }
};

static const struct uargs_prog PROG = {
    .name = "rm",
    .usage = "[-rf] PATH...",
    .summary = "Delete files, and with -r directories and what is in them.\n"
               "No prompt, and not the Recycle Bin (that is `trash`).",
    .opts = OPTS,
};

struct errs { int reported, missing; };

static void on_error(void *ctx, const char *path, int err) {
    struct errs *e = ctx;
    if (force && err == ENOENT) { e->missing++; return; }
    e->reported++;
    cmd_fail_err("rm", path, err);
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    // POSIX: `rm -f` with nothing to remove succeeds.
    if (a.argc == 0) return force ? 0 : uargs_error(&PROG, "what should be removed?");

    int failed = 0;
    for (int i = 0; i < a.argc; i++) {
        struct errs e = { 0, 0 };
        struct ufileop_policy policy = { .on_error = on_error, .ctx = &e };
        if (ufileop_remove(a.argv[i], recursive, &g_op, &policy) != UFILEOP_OK &&
            !(e.missing && !e.reported))
            failed = 1;
    }
    return failed;
}
