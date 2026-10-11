// See ufbs.h.
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "lib/ufbs.h"
#include "lib/ubootcfg.h"   // ubootcfg_cmdline()

enum ufbs_word ufbs_word_in(const char *cmdline) {
    static const char WORD[] = "fbs=";
    enum ufbs_word w = UFBS_WORD_NONE;
    for (const char *p = cmdline; (p = strstr(p, WORD)) != 0; p++) {
        if (p != cmdline && p[-1] != ' ') continue;
        const char *v = p + sizeof WORD - 1;
        size_t n = strcspn(v, " ");
        // The LAST one wins, as a later word on a command line does.
        if (n == 2 && !strncmp(v, "on", 2)) w = UFBS_WORD_ON;
        else if (n == 3 && !strncmp(v, "off", 3)) w = UFBS_WORD_OFF;
    }
    return w;
}

enum ufbs_word ufbs_boot_word(void) {
    static char line[1024];
    return ubootcfg_cmdline(line, sizeof line) < 0 ? UFBS_WORD_NONE : ufbs_word_in(line);
}

int ufbs_done(char *how, int cap) {
    int fd = open(UFBS_MARKER, O_RDONLY);
    if (fd < 0) return 0;
    if (how && cap > 0) {
        long n = read(fd, how, (size_t)cap - 1);
        how[n > 0 ? n : 0] = 0;
        how[strcspn(how, "\n")] = 0;
    }
    close(fd);
    return 1;
}

int ufbs_needed(void) {
    switch (ufbs_boot_word()) {
    case UFBS_WORD_ON:  return 1;
    case UFBS_WORD_OFF: return 0;
    default:            return !ufbs_done(0, 0);
    }
}

int ufbs_mark(const char *how) {
    mkdir("/var/lib", 0755);   // already there is not a failure
    mkdir("/var/lib/fbs", 0755);
    char line[64], stamp[24] = "";
    time_t now = time(0);
    struct tm tm;
    if (localtime_r(&now, &tm)) strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M", &tm);
    int n = snprintf(line, sizeof line, "%s %s\n", how, stamp);
    int fd = open(UFBS_MARKER, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    int rc = write(fd, line, (size_t)n) == n && fsync(fd) == 0 ? 0 : -1;
    close(fd);
    return rc;
}

int ufbs_rearm(void) {
    struct stat st;
    if (stat(UFBS_MARKER, &st) != 0) return 0;
    return unlink(UFBS_MARKER) == 0 ? 0 : -1;
}
