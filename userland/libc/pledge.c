// pledge() and unveil() -- OpenBSD's calls with OpenBSD's strings, over
// SYS_PLEDGE and SYS_UNVEIL (abi/sandbox_abi.h). See <unistd.h>.
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include "sandbox_abi.h"
#include "rt/sys.h"

static const char *const WORDS[] = PROMISE_WORDS;
#define NWORDS (sizeof WORDS / sizeof WORDS[0])

// A word list to PROMISE_* bits; -1 on a word nobody knows, which is
// refused rather than skipped -- a typo would otherwise give up nothing.
static long parse(const char *s) {
    unsigned bits = 0;
    while (*s) {
        while (*s == ' ') s++;
        size_t n = strcspn(s, " ");
        if (!n) break;
        unsigned i = 0;
        for (; i < NWORDS; i++)
            if (strlen(WORDS[i]) == n && !strncmp(WORDS[i], s, n)) break;
        // "exec" is OpenBSD's word for what PROC covers here as well.
        if (i == NWORDS && n == 4 && !strncmp(s, "exec", 4)) i = 6;
        if (i == NWORDS) return -1;
        bits |= 1u << i;
        s += n;
    }
    return bits;
}

int pledge(const char *promises, const char *execpromises) {
    // ONE SET: toy-os's survives exec as it is, so a separate set for
    // after exec has nowhere to live. Refused rather than ignored.
    if (execpromises) { errno = EINVAL; return -1; }
    if (!promises) return 0;
    long bits = parse(promises);
    if (bits < 0) { errno = EINVAL; return -1; }
    return sys_pledge((unsigned)bits);
}

int unveil(const char *path, const char *permissions) {
    if (!path && !permissions) return sys_unveil(NULL, 0);
    if (!path || !permissions) { errno = EINVAL; return -1; }
    unsigned rights = 0;
    for (const char *p = permissions; *p; p++) {
        switch (*p) {
        case 'r': rights |= UNVEIL_R; break;
        case 'w': rights |= UNVEIL_W; break;
        case 'c': rights |= UNVEIL_C; break;
        case 'x': rights |= UNVEIL_X; break;
        default: errno = EINVAL; return -1;
        }
    }
    return sys_unveil(path, rights);
}
