// See ufbs.h.
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "lib/ufbs.h"
#include "lib/ubootcfg.h"   // ubootcfg_cmdline()
#include "lib/udiskscan.h"  // udisk_live_root()
#include "lib/usetting.h"

enum ufbs_word ufbs_word_in(const char *cmdline) {
    return (enum ufbs_word)ubootcfg_onoff_in(cmdline, "fbs=");
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
    default:            return !udisk_live_root() && !ufbs_done(0, 0);
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
    if (stat(UFBS_ANSWERED, &st) == 0 && ufbs_clear_answered() != 0) return -1;
    if (stat(UFBS_MARKER, &st) != 0) return 0;
    return unlink(UFBS_MARKER) == 0 ? 0 : -1;
}

// --- pages already answered -------------------------------------------

// Each page's settings, as the wizard writes them.
static const struct { const char *id, *title, *settings[6]; } PAGES[UFBS_PAGES] = {
    { "region",   "Region & formats", { "locale.region", "locale.time_format" } },
    { "keyboard", "Keyboard",  { "system.keyboard_layout", "system.keyboard_layouts",
                                 "system.keyboard_dead_keys" } },
    { "time",     "Time zone", { "system.timezone", "system.ntp" } },
    { "look",     "Appearance", { "desktop.wallpaper", "desktop.wallpaper_type", "system.cursor_theme",
                                  "desktop.taskbar_theme", "desktop.transparency" } },
    { "name",     "Computer name", { "system.hostname" } },
    { "net",      "Network",   { "update.server" } },
};

const char *ufbs_page_id(int page) {
    return page >= 0 && page < UFBS_PAGES ? PAGES[page].id : "";
}

const char *ufbs_page_title(int page) {
    return page >= 0 && page < UFBS_PAGES ? PAGES[page].title : "";
}

int ufbs_page_stored(int page) {
    if (page < 0 || page >= UFBS_PAGES) return 0;
    for (int i = 0; i < 6 && PAGES[page].settings[i]; i++) {
        static struct setting_msg m;   // ~1 KB: never on a 2 KiB frame
        if (usetting_find(PAGES[page].settings[i], &m) >= 0 && m.stored[0]) return 1;
    }
    return 0;
}

unsigned ufbs_stored(void) {
    unsigned bits = 0;
    for (int p = 0; p < UFBS_PAGES; p++)
        if (ufbs_page_stored(p)) bits |= 1u << p;
    return bits;
}

int ufbs_write_answered(const char *root, unsigned pages) {
    if (!pages) return 0;
    char dir[96], path[128];
    static const char *const LEVELS[] = { "/var", "/var/lib", "/var/lib/fbs" };
    for (unsigned i = 0; i < sizeof LEVELS / sizeof LEVELS[0]; i++) {
        snprintf(dir, sizeof dir, "%s%s", root, LEVELS[i]);
        mkdir(dir, 0755);   // already there is not a failure
    }
    snprintf(path, sizeof path, "%s" UFBS_ANSWERED, root);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    int ok = 1;
    for (int p = 0; p < UFBS_PAGES; p++) {
        if (!(pages & (1u << p))) continue;
        char line[24];
        int n = snprintf(line, sizeof line, "%s\n", PAGES[p].id);
        if (write(fd, line, (size_t)n) != n) ok = 0;
    }
    if (fsync(fd) != 0) ok = 0;
    close(fd);
    return ok ? 0 : -1;
}

unsigned ufbs_answered(void) {
    char buf[256];
    int fd = open(UFBS_ANSWERED, O_RDONLY);
    if (fd < 0) return 0;
    long n = read(fd, buf, sizeof buf - 1);
    close(fd);
    buf[n > 0 ? n : 0] = 0;
    unsigned bits = 0;
    for (char *line = buf, *end; *line; line = end) {
        end = line + strcspn(line, "\n");
        if (*end) *end++ = 0;
        for (int p = 0; p < UFBS_PAGES; p++)
            if (!strcmp(line, PAGES[p].id)) bits |= 1u << p;
    }
    return bits;
}

int ufbs_clear_answered(void) {
    struct stat st;
    if (stat(UFBS_ANSWERED, &st) != 0) return 0;
    return unlink(UFBS_ANSWERED) == 0 ? 0 : -1;
}
