// A long job's stage line on a terminal (lib/ustageline.h).
#include "lib/ustageline.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void ustageline_draw(const char *prog, const struct fs_check_progress *p, int *width) {
    if (!p->running || !p->stages) return;
    unsigned st = p->stage < FSCK_STAGES_MAX ? p->stage : 0;
    char line[128], name[32];
    snprintf(name, sizeof name, "%s", p->names[st]);
    if (name[0] >= 'A' && name[0] <= 'Z') name[0] = (char)(name[0] - 'A' + 'a');
    int n = snprintf(line, sizeof line, "%s: stage %u of %u, %s", prog, st + 1, p->stages, name);
    if (p->total && n > 0 && n < (int)sizeof line) {
        unsigned done = p->done > p->total ? p->total : p->done;
        snprintf(line + n, sizeof line - (size_t)n, ": %u of %u %s (%u%%)", done, p->total, p->units[st],
                 (unsigned)((unsigned long long)done * 100 / p->total));
    }
    int len = (int)strlen(line);
    char out[192];   // ONE write: stderr is unbuffered, and fprintf would make it one per byte
    int n2 = snprintf(out, sizeof out, "\r%s%*s", line, *width > len ? *width - len : 0, "");
    if (n2 > 0) write(2, out, (size_t)(n2 < (int)sizeof out ? n2 : (int)sizeof out - 1));
    *width = len;
}

void ustageline_clear(int *width) {
    if (!*width) return;
    char blank[160];
    int n = snprintf(blank, sizeof blank, "\r%*s\r", *width < 150 ? *width : 150, "");
    if (n > 0) write(2, blank, (size_t)n);
    *width = 0;
}
