// The /etc colon databases (etcdb.h).
#include "etcdb.h"

int etcdb_next(struct etcdb *db, const char *path, char **field, int nfields) {
    if (!db->f) {
        db->f = fopen(path, "r");
        if (!db->f) return 0;
    }
    ssize_t n;
    while ((n = getline(&db->line, &db->cap, db->f)) >= 0) {
        while (n > 0 && (db->line[n - 1] == '\n' || db->line[n - 1] == '\r')) db->line[--n] = '\0';
        if (n == 0 || db->line[0] == '#') continue;
        int k = 0;
        char *p = db->line;
        field[k++] = p;
        for (; *p; p++) {
            if (*p != ':') continue;
            *p = '\0';
            if (k == nfields) { k = nfields + 1; break; }   // too many: skipped
            field[k++] = p + 1;
        }
        if (k == nfields) return 1;
    }
    return 0;
}

void etcdb_rewind(struct etcdb *db) {
    if (db->f) rewind(db->f);
}

void etcdb_close(struct etcdb *db) {
    if (db->f) fclose(db->f);
    db->f = NULL;
}

int etcdb_id(const char *s, unsigned *out) {
    if (!s || *s < '0' || *s > '9') return 0;
    unsigned long v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return 0;
        v = v * 10 + (unsigned long)(*s - '0');
        if (v > 0xFFFFFFFFul) return 0;
    }
    *out = (unsigned)v;
    return 1;
}
