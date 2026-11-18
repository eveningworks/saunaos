// uargbatch.h: an argument list split to fit SPAWN_ARGS_MAX.
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "syscall_abi.h"   // SPAWN_ARGS_MAX
#include "rt/sys.h"         // environ, sys_env_bytes
#include "uargbatch.h"

int uargbatch(char *const *head, int nhead, char *const *items, int nitems,
              void (*run)(char *const *argv, void *ctx), void *ctx) {
    // The environment the spawn passes shares the budget.
    size_t env = sys_env_bytes(environ);
    size_t cap = env < SPAWN_ARGS_MAX ? SPAWN_ARGS_MAX - env : 0;
    size_t head_len = 0;
    for (int i = 0; i < nhead; i++) head_len += strlen(head[i]) + 1;
    // Not even the shortest item (an empty one) fits beside the head.
    if (nitems > 0 && head_len + 1 > cap) { errno = E2BIG; return -1; }
    // At most every item in one batch, plus the head and the NULL.
    char **argv = malloc((size_t)(nhead + nitems + 1) * sizeof *argv);
    if (!argv) { errno = ENOMEM; return -1; }
    for (int i = 0; i < nhead; i++) argv[i] = head[i];

    int runs = 0, skipped = 0, at = 0;
    while (at < nitems) {
        size_t len = head_len;
        int k = nhead;
        for (; at < nitems; at++) {
            size_t one = strlen(items[at]) + 1;
            if (head_len + one > cap) { skipped = 1; continue; }   // never fits
            if (len + one > cap) break;                            // the next batch's
            argv[k++] = items[at];
            len += one;
        }
        if (k == nhead) break;   // only items that never fit were left
        argv[k] = 0;
        run(argv, ctx);
        runs++;
    }
    free(argv);
    if (skipped) { errno = ENAMETOOLONG; return -1; }
    return runs;
}
