#ifndef UFIND_H
#define UFIND_H
// WHICH FILES A SEARCH KEEPS, beyond their name: the kind, the date band
// and the size band a person narrows a search by, and whether a file
// holds a phrase -- Explorer's search filters (kind:, date:, size:) and
// Nautilus's, as data any search or chooser can apply. The bands are the
// same ones the File Manager's Group by draws, so a filter and a group
// of one name hold the same files.
//
// Contents are searched by READING THE FILE, end to end: there is no
// index here to ask (Windows Search and Tracker keep one). The caller runs
// it off its UI thread and passes a flag to stop on.
#include "lib/ufiletype.h"
#include "rt/sys.h"

enum ufind_age {
    UFIND_ANY_DATE, UFIND_TODAY, UFIND_THIS_WEEK, UFIND_THIS_MONTH, UFIND_THIS_YEAR,
    UFIND_OLDER, UFIND_AGES
};
// Sizes in Explorer's bands; a folder has no size and is in none of them.
enum ufind_size {
    UFIND_ANY_SIZE, UFIND_EMPTY, UFIND_TINY, UFIND_SMALL, UFIND_MEDIUM, UFIND_LARGE,
    UFIND_SIZES
};

struct ufind_filter {
    int kind;    // -1 any, else an enum ufiletype_kind
    int age;     // enum ufind_age
    int size;    // enum ufind_size
};

#define UFIND_FILTER_ANY { -1, UFIND_ANY_DATE, UFIND_ANY_SIZE }

// 1 when any of the three narrows anything.
int ufind_filter_active(const struct ufind_filter *f);
// 1 when `e` passes all three; `now` is seconds since the epoch, asked
// once by the caller rather than per entry.
int ufind_matches(const struct ufind_filter *f, const struct sys_dirent *e, long long now);

// The band `bytes` falls in, UFIND_EMPTY..UFIND_LARGE.
int ufind_size_band(unsigned long long bytes);
// "Any date", "Today", ... / "Any size", "Empty", "Tiny (under 16 KB)", ...
// Never NULL.
const char *ufind_age_name(int age);
const char *ufind_size_name(int size);

// 1 when the file at `path` holds `needle`, in any case, with the number
// of the line it is on (from 1) in `*line_no` and the line itself
// (trimmed around the match, "..." where cut) in `line`. 0 for
// no match, a file that reads as binary (a NUL in its first block -- grep's
// test), an unreadable one, or `*cancel` going non-zero, which is polled
// per block.
int ufind_contains(const char *path, const char *needle, int *line_no, char *line, int cap,
                   const volatile int *cancel);

#endif
