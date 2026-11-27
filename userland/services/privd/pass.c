// Passwords -- see pass.h. yescrypt at libxcrypt's default cost ("$y$j9T$":
// N=4096, r=32, 16 MiB), so a hash moves between this and a Linux box.
#include "pass.h"
#include "rt/sys.h"
#include "yescrypt.h"
#include <errno.h>
#include <fcntl.h>
#include <shadow.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SHADOW     "/etc/shadow"
#define SHADOW_NEW "/etc/shadow.new"

// Every byte compared, so how long a wrong answer takes says nothing
// about how much of it was right.
static int same(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    unsigned char d = na != nb;
    for (size_t i = 0; i < na && i < nb; i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

// THE ACCOUNT'S HASH FIELD, in getspnam()'s storage until the next call:
// "" when it has none -- no line, or no file at all (a disk from before
// D2a) -- and LOCKED ("!") when the file is there but not root's alone:
// someone wrote it who should not have, so nothing in it is believed and
// no password opens anything (fail closed).
static const char *hash_of(const char *user) {
    errno = 0;
    struct spwd *sp = getspnam(user);
    if (sp) return sp->sp_pwdp;
    if (errno == EPERM) {
        printf("privd: %s is not root's alone (0600) -- every account locked\n", SHADOW);
        return "!";
    }
    return "";
}

static int locked(const char *h) { return h[0] == '!' || h[0] == '*'; }

int pass_has(const char *user) {
    return hash_of(user)[0] != '\0';   // a locked account "has" one nobody knows
}

int pass_can_sign(const char *user) {
    const char *h = hash_of(user);
    return h[0] != '\0' && !locked(h);
}

int pass_check(const char *user, const char *pw) {
    const char *h = hash_of(user);
    // No password: "" alone matches -- except root, which signs in by
    // password only when one is set (Ubuntu's locked root).
    if (h[0] == '\0') return strcmp(user, "root") != 0 && pw[0] == '\0';
    if (locked(h)) return 0;
    uint8_t *got = yescrypt((const uint8_t *)pw, (const uint8_t *)h);
    return got && same((const char *)got, h);
}

static int hash_new(const char *pw, char *out, size_t cap) {
    uint8_t salt[16];
    if (sys_getrandom(salt, sizeof salt) != (int)sizeof salt) return -EIO;
    yescrypt_params_t p = { .flags = YESCRYPT_DEFAULTS, .N = 4096, .r = 32, .p = 1 };
    uint8_t setting[96];
    if (!yescrypt_encode_params_r(&p, salt, sizeof salt, setting, sizeof setting)) return -EINVAL;
    uint8_t *h = yescrypt((const uint8_t *)pw, setting);
    if (!h) return -ENOMEM;
    if ((size_t)snprintf(out, cap, "%s", (const char *)h) >= cap) return -E2BIG;
    return 0;
}

// The current file's text: "" when there is none, or none root may
// trust; NULL when it could not be read -- then nothing is written, or
// every other account's line would be lost.
static char *read_current(void) {
    FILE *f = fopen(SHADOW, "r");
    if (!f && errno != ENOENT) return NULL;
    struct stat st;
    if (f && (fstat(fileno(f), &st) < 0 || st.st_uid != 0 || (st.st_mode & 077))) {
        printf("privd: %s is not root's alone (0600) -- replaced\n", SHADOW);
        fclose(f);
        f = NULL;
    }
    size_t cap = 256, n = 0;
    char *s = malloc(cap);
    if (!s) { if (f) fclose(f); return NULL; }
    if (f) {
        for (;;) {
            size_t k = fread(s + n, 1, cap - n - 1, f);
            if (k == 0) {
                if (ferror(f)) { free(s); fclose(f); return NULL; }
                break;
            }
            n += k;
            if (cap - n - 1 == 0) {
                char *ns = realloc(s, cap * 2);
                if (!ns) { free(s); fclose(f); return NULL; }
                s = ns;
                cap *= 2;
            }
        }
        fclose(f);
    }
    s[n] = '\0';
    return s;
}

static int write_all(int fd, const char *s, size_t n) {
    while (n) {
        ssize_t k = write(fd, s, n);
        if (k <= 0) return -1;
        s += k;
        n -= (size_t)k;
    }
    return 0;
}

// The account's line replaced by one with `hash`, or added -- or, when
// `drop`, taken out.
static int shadow_write(const char *user, const char *hash, int drop) {
    char *cur = read_current();
    if (!cur) return -EIO;
    long day = (long)(time(NULL) / 86400);
    size_t un = strlen(user), cap = strlen(cur) + un + strlen(hash) + 64;
    char *out = malloc(cap);
    if (!out) { free(cur); return -ENOMEM; }
    size_t o = 0;
    int done = 0;
    for (char *line = cur; *line; ) {
        char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        if (len > un && !strncmp(line, user, un) && line[un] == ':' && drop) {
            // taken out
        } else if (len > un && !strncmp(line, user, un) && line[un] == ':' && !done) {
            // name:hash:lastchg:REST -- the rest kept as it was.
            const char *rest = line + un + 1;
            int skip = 0;
            const char *p = rest;
            while (p < line + len && skip < 2) if (*p++ == ':') skip++;
            o += (size_t)snprintf(out + o, cap - o, "%s:%s:%ld:", user, hash, day);
            if (skip == 2) { memcpy(out + o, p, (size_t)(line + len - p)); o += (size_t)(line + len - p); }
            else o += (size_t)snprintf(out + o, cap - o, "0:99999:7:::");
            out[o++] = '\n';
            done = 1;
        } else if (len) {
            memcpy(out + o, line, len);
            o += len;
            out[o++] = '\n';
        }
        line += len + (nl != NULL);
    }
    if (!done && !drop) o += (size_t)snprintf(out + o, cap - o, "%s:%s:%ld:0:99999:7:::\n", user, hash, day);
    free(cur);
    // O_EXCL after the unlink: a file the desktop made at that name (it
    // may add files to /etc) is never the one written.
    unlink(SHADOW_NEW);
    int fd = open(SHADOW_NEW, O_WRONLY | O_CREAT | O_EXCL, 0600);
    int rc = fd < 0 ? -errno : 0;
    if (!rc && (fchown(fd, 0, 0) < 0 || fchmod(fd, 0600) < 0)) rc = -errno;
    if (!rc && (write_all(fd, out, o) < 0 || fsync(fd) < 0)) rc = -EIO;
    if (fd >= 0) close(fd);
    if (!rc && rename(SHADOW_NEW, SHADOW) < 0) rc = -errno;
    if (rc) unlink(SHADOW_NEW);
    memset(out, 0, o);
    free(out);
    return rc;
}

int pass_set(const char *user, const char *pw) {
    if (!user[0] || strchr(user, ':') || strchr(user, '\n')) return -EINVAL;
    char hash[256] = "";
    if (pw && pw[0]) {
        int rc = hash_new(pw, hash, sizeof hash);
        if (rc) return rc;
    }
    return shadow_write(user, hash, 0);
}

int pass_drop(const char *user) {
    if (!user[0] || strchr(user, ':') || strchr(user, '\n')) return -EINVAL;
    return shadow_write(user, "", 1);
}
