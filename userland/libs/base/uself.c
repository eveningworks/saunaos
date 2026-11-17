// The running program's own path -- see uself.h.
#include "base/uself.h"
#include <string.h>
#include <unistd.h>
#include "query_abi.h"
#include "rt/sys.h"

int uself_path(char *buf, size_t cap) {
    int me = getpid();
    struct query_procpath r;
    QUERY_FOREACH(QUERY_PROCPATH, r, i) {
        if (r.pid != me) continue;
        size_t n = strnlen(r.path, sizeof r.path);
        if (n == 0 || n >= sizeof r.path || n >= cap) return -1;
        memcpy(buf, r.path, n + 1);
        return (int)n;
    }
    return -1;
}
