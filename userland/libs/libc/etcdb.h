// The colon-separated databases in /etc -- passwd and group -- read a
// line at a time for <pwd.h> and <grp.h>. Internal to libc.
#ifndef LIBC_ETCDB_H
#define LIBC_ETCDB_H

#include <stdio.h>
#include <stddef.h>

struct etcdb {
    FILE  *f;
    char  *line;   // getline()'s, grown to the longest line
    size_t cap;
};

// The next well-formed line split IN PLACE into exactly `nfields`
// colon-separated fields; comments, blank lines and lines with another
// field count are skipped, glibc's behaviour. 1 for a line, 0 at the end
// (or when the file cannot be opened).
int etcdb_next(struct etcdb *db, const char *path, char **field, int nfields);
void etcdb_rewind(struct etcdb *db);
void etcdb_close(struct etcdb *db);

// A whole decimal id, nothing before or after it. 0 for a malformed one.
int etcdb_id(const char *s, unsigned *out);

#endif
