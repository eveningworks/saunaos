// The people's accounts -- see uusers.h.
#include "base/uusers.h"
#include "base/upriv.h"
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>

int uusers_is_admin(const char *name) {
    struct passwd *pw = getpwnam(name);
    if (!pw) return 0;
    gid_t primary = pw->pw_gid;
    struct group *g = getgrnam("wheel");
    if (!g) return 0;
    if (g->gr_gid == primary) return 1;
    for (int i = 0; g->gr_mem && g->gr_mem[i]; i++)
        if (!strcmp(g->gr_mem[i], name)) return 1;
    return 0;
}

static int nologin(const char *shell) {
    const char *b = shell ? strrchr(shell, '/') : 0;
    b = b ? b + 1 : shell;
    return b && (!strcmp(b, "nologin") || !strcmp(b, "false"));
}

void uusers_free(struct uuser *u, int n) {
    for (int i = 0; u && i < n; i++) {
        free(u[i].name);
        free(u[i].real);
        free(u[i].home);
    }
    free(u);
}

int uusers_list(struct uuser **out) {
    *out = 0;
    int n = 0, cap = 0;
    struct uuser *u = 0;
    setpwent();
    struct passwd *pw;
    while ((pw = getpwent())) {
        if (pw->pw_uid < 1000 || nologin(pw->pw_shell)) continue;
        if (n == cap) {
            int nc = cap ? cap * 2 : 4;
            struct uuser *nu = realloc(u, (size_t)nc * sizeof *nu);
            if (!nu) { endpwent(); uusers_free(u, n); errno = ENOMEM; return -1; }
            u = nu;
            cap = nc;
        }
        // The GECOS field's first comma-separated part is the full name.
        const char *g = pw->pw_gecos ? pw->pw_gecos : "";
        size_t gl = strcspn(g, ",");
        struct uuser *e = &u[n];
        e->name = strdup(pw->pw_name);
        e->real = gl ? strndup(g, gl) : strdup(pw->pw_name);
        e->home = strdup(pw->pw_dir ? pw->pw_dir : "");
        e->uid = pw->pw_uid;
        e->admin = 0;
        if (!e->name || !e->real || !e->home) {
            free(e->name); free(e->real); free(e->home);
            endpwent();
            uusers_free(u, n);
            errno = ENOMEM;
            return -1;
        }
        n++;
    }
    endpwent();
    // After the walk: getgrnam() and getpwnam() share no state with it,
    // but uusers_is_admin() rereads passwd.
    for (int i = 0; i < n; i++) u[i].admin = uusers_is_admin(u[i].name);
    *out = u;
    return n;
}

int uusers_name_ok(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n < 1 || n > 32) return 0;
    if (!((s[0] >= 'a' && s[0] <= 'z') || s[0] == '_')) return 0;
    for (size_t i = 1; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}

int uusers_full_ok(const char *s) {
    return s && strlen(s) <= UUSERS_FULL_MAX && !strchr(s, ':') && !strchr(s, '\n') && !strchr(s, '\r');
}

// One UTF-8 Latin letter folded to its ASCII base, lower case; 0 when it
// is not a letter this knows. `*len` is how many bytes it took.
static char fold(const unsigned char *p, int *len) {
    *len = 1;
    unsigned char c = p[0];
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return (char)c;
    if (c == 0xC3 && p[1] >= 0x80 && p[1] <= 0xBF) {
        *len = 2;
        // U+00C0..U+00FF, Latin-1's letters, by their second byte.
        static const char MAP[65] = "aaaaaaaceeeeiiiidnooooo-ouuuuyts"   // C0..DF
                                    "aaaaaaaceeeeiiiidnooooo-ouuuuyty";  // E0..FF
        char m = MAP[p[1] - 0x80];
        return m == '-' ? 0 : m;
    }
    if (c >= 0x80) {   // another non-ASCII character: skipped whole
        int k = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        for (int i = 1; i < k; i++) if (!p[i]) { k = i; break; }
        *len = k;
    }
    return 0;
}

void uusers_name_from_full(const char *full, char *out, int cap) {
    if (cap <= 0) return;
    out[0] = '\0';
    if (!full) return;
    const unsigned char *p = (const unsigned char *)full;
    while (*p == ' ' || *p == '\t') p++;
    int o = 0;
    while (*p && *p != ' ' && *p != '\t' && o < 32 && o + 1 < cap) {
        int len;
        char c = fold(p, &len);
        p += len;
        if (!c) continue;
        if (o == 0 && c >= '0' && c <= '9') continue;   // a name starts with a letter
        out[o++] = c;
    }
    out[o] = '\0';
}

static int run(int on_tty, const char *what, const char *action, const char *const *args) {
    return on_tty ? upriv_action_tty(what, action, args) : upriv_action(action, args, 0);
}

int uusers_add(const char *name, const char *full, int admin, const char *password, int on_tty) {
    if (!uusers_name_ok(name) || !uusers_full_ok(full ? full : "")) { errno = EINVAL; return -1; }
    const char *args[] = { name, full ? full : "", admin ? "1" : "0", password ? password : "", 0 };
    return run(on_tty, "user add", UPRIV_ACT_USERADD, args);
}

int uusers_remove(const char *name, int delete_home, int on_tty) {
    const char *args[] = { name, delete_home ? "delete" : "keep", 0 };
    return run(on_tty, "user remove", UPRIV_ACT_USERDEL, args);
}

int uusers_set(const char *name, const char *field, const char *value, int on_tty) {
    const char *args[] = { name, field, value ? value : "", 0 };
    return run(on_tty, "user set", UPRIV_ACT_USERMOD, args);
}
