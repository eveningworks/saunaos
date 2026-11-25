// The password database, /etc/shadow (<shadow.h>).
#include <shadow.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "etcdb.h"

#define SHADOW_FILE "/etc/shadow"
enum { F_NAME, F_PWD, F_LSTCHG, F_MIN, F_MAX, F_WARN, F_INACT, F_EXPIRE, F_FLAG, NF };

// An empty field is -1, Linux's "not set"; anything but digits names
// nobody.
static int num(const char *s, long *out) {
    if (!*s) { *out = -1; return 1; }
    unsigned v;
    if (!etcdb_id(s, &v)) return 0;
    *out = (long)v;
    return 1;
}

static struct spwd g_sp;
static char *g_buf;
static size_t g_cap;

// A file root does not own alone is not the password database: /etc is
// the desktop's to add files to until users D3, so a /etc/shadow the
// person made there would otherwise name root's password (sshd's
// StrictModes, for the same reason).
static FILE *open_db(void) {
    FILE *f = fopen(SHADOW_FILE, "r");
    if (!f) return NULL;
    struct stat st;
    if (fstat(fileno(f), &st) < 0 || st.st_uid != 0 || (st.st_mode & 077)) {
        fclose(f);
        errno = EPERM;
        return NULL;
    }
    return f;
}

struct spwd *getspnam(const char *name) {
    if (!name) return NULL;
    struct etcdb db = { open_db(), 0, 0 };
    if (!db.f) return NULL;   // errno: ENOENT, EACCES or EPERM
    char *f[NF];
    struct spwd *r = NULL;
    int oom = 0;
    while (etcdb_next(&db, SHADOW_FILE, f, NF)) {
        if (strcmp(f[F_NAME], name) != 0) continue;
        long n[7];
        int ok = 1;
        for (int i = 0; i < 7; i++) ok &= num(f[F_LSTCHG + i], &n[i]);
        if (!ok) continue;   // a malformed line names nobody
        size_t need = strlen(f[F_NAME]) + strlen(f[F_PWD]) + 2;
        if (need > g_cap) {
            char *nb = realloc(g_buf, need);
            if (!nb) { oom = 1; break; }
            g_buf = nb;
            g_cap = need;
        }
        size_t a = strlen(f[F_NAME]) + 1;
        memcpy(g_buf, f[F_NAME], a);
        memcpy(g_buf + a, f[F_PWD], strlen(f[F_PWD]) + 1);
        g_sp.sp_namp = g_buf;
        g_sp.sp_pwdp = g_buf + a;
        g_sp.sp_lstchg = n[0];
        g_sp.sp_min = n[1];
        g_sp.sp_max = n[2];
        g_sp.sp_warn = n[3];
        g_sp.sp_inact = n[4];
        g_sp.sp_expire = n[5];
        g_sp.sp_flag = n[6] < 0 ? 0 : (unsigned long)n[6];
        r = &g_sp;
        break;
    }
    if (!r) errno = oom ? ENOMEM : ENOENT;
    etcdb_close(&db);
    free(db.line);
    return r;
}
