// Search filters and the contents match -- see ufind.h.
#include "lib/ufind.h"
#include <caltime.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

int ufind_filter_active(const struct ufind_filter *f) {
    return f->kind >= 0 || f->age != UFIND_ANY_DATE || f->size != UFIND_ANY_SIZE;
}

int ufind_size_band(unsigned long long bytes) {
    if (bytes == 0) return UFIND_EMPTY;
    if (bytes < 16ull * 1024) return UFIND_TINY;
    if (bytes < 1024ull * 1024) return UFIND_SMALL;
    if (bytes < 128ull * 1024 * 1024) return UFIND_MEDIUM;
    return UFIND_LARGE;
}

const char *ufind_age_name(int age) {
    static const char *const N[UFIND_AGES] = {
        "Any date", "Today", "This week", "This month", "This year", "Before this year",
    };
    return (unsigned)age < UFIND_AGES ? N[age] : N[0];
}

const char *ufind_size_name(int size) {
    static const char *const N[UFIND_SIZES] = {
        "Any size", "Empty", "Tiny (under 16 KB)", "Small (under 1 MB)",
        "Medium (under 128 MB)", "Large",
    };
    return (unsigned)size < UFIND_SIZES ? N[size] : N[0];
}

// Calendar days, local time: equal on the same day, ordered.
static long local_day(long long when, struct tm *out) {
    time_t t = (time_t)when;
    localtime_r(&t, out);
    return (long)out->tm_year * 366 + out->tm_yday;
}

// CALENDAR bands, not rolling ones: "This week" is since Sunday, as the
// File Manager's Group by draws it. A clock ahead of ours is today.
static int age_ok(int age, const struct rtc_time *mod, long long now) {
    struct tm n, w;
    long dn = local_day(now, &n), dw = local_day((long long)cal_rtc_to_epoch(mod), &w);
    int same_year = w.tm_year == n.tm_year;
    switch (age) {
    case UFIND_TODAY:      return dw >= dn;
    case UFIND_THIS_WEEK:  return dw >= dn - n.tm_wday;
    case UFIND_THIS_MONTH: return (same_year && w.tm_mon == n.tm_mon) || dw > dn;
    case UFIND_THIS_YEAR:  return same_year || dw > dn;
    case UFIND_OLDER:      return w.tm_year < n.tm_year;
    default:               return 1;
    }
}

int ufind_matches(const struct ufind_filter *f, const struct sys_dirent *e, long long now) {
    if (f->kind >= 0 && (int)ufiletype_kind(e->name, e->is_dir) != f->kind) return 0;
    if (f->size != UFIND_ANY_SIZE && (e->is_dir || ufind_size_band(e->size) != f->size)) return 0;
    if (f->age != UFIND_ANY_DATE && !age_ok(f->age, &e->modified, now)) return 0;
    return 1;
}

// The line around a match at `at`, in buf[0..n): from the line's start
// or a little before the match, whichever is later, to its end or `cap`.
// `mid`: buf[0] is not a line's start (a long line's tail was carried).
static void excerpt(const char *buf, int n, int at, int mid, char *line, int cap) {
    int ls = at, le = at;
    while (ls > 0 && buf[ls - 1] != '\n') ls--;
    while (le < n && buf[le] != '\n' && buf[le] != '\r') le++;
    int lead = cap / 4, cut = ls == 0 && mid;
    if (at - ls > lead) { ls = at - lead; cut = 1; }
    while (ls < at && (buf[ls] == ' ' || buf[ls] == '\t')) ls++;   // indentation says nothing
    int o = 0;
    if (cut && cap > 4) { memcpy(line, "...", 3); o = 3; }
    for (int i = ls; i < le && o < cap - 1; i++) line[o++] = buf[i] == '\t' ? ' ' : buf[i];
    line[o] = '\0';
}

// A block, and room for the carried tail of the one before it -- a match
// split across two reads, and the start of its line.
#define BLOCK 8192
#define CARRY 512

static int newlines(const char *p, int n) {
    int c = 0;
    for (int i = 0; i < n; i++) c += p[i] == '\n';
    return c;
}

int ufind_contains(const char *path, const char *needle, int *line_no, char *line, int cap,
                   const volatile int *cancel) {
    int qn = (int)strlen(needle);
    if (cap > 0) line[0] = '\0';
    if (qn == 0 || qn >= CARRY) return 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    char *buf = malloc(CARRY + BLOCK);
    if (!buf) { close(fd); return 0; }
    int have = 0, first = 1, found = 0, lines = 0;   // newlines before buf[0]
    int mid = 0;                                      // ...and buf[0] is inside a line
    for (;;) {
        if (cancel && *cancel) break;
        long r = read(fd, buf + have, BLOCK);
        if (r <= 0) break;
        if (first && memchr(buf + have, 0, (size_t)r)) break;   // binary
        first = 0;
        int n = have + (int)r;
        for (int i = 0; i + qn <= n; i++) {
            if (!strncasecmp(buf + i, needle, (size_t)qn)) {
                excerpt(buf, n, i, mid, line, cap);
                if (line_no) *line_no = lines + newlines(buf, i) + 1;
                found = 1;
                break;
            }
        }
        if (found) break;
        // Carry the tail: the last line's start, or as much as fits.
        int keep = 0;
        while (keep < n && keep < CARRY && buf[n - 1 - keep] != '\n') keep++;
        if (keep >= CARRY) keep = qn - 1;
        if (keep < n) mid = buf[n - 1 - keep] != '\n';   // else buf[0] stays where it was
        lines += newlines(buf, n - keep);
        memmove(buf, buf + n - keep, (size_t)keep);
        have = keep;
    }
    free(buf);
    close(fd);
    return found;
}
