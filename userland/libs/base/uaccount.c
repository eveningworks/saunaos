#include "base/uaccount.h"
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int is_login_var(const char *e) {
    static const char *const V[] = { "HOME=", "USER=", "LOGNAME=", "SHELL=" };
    for (unsigned i = 0; i < sizeof V / sizeof V[0]; i++)
        if (strncmp(e, V[i], strlen(V[i])) == 0) return 1;
    return 0;
}

static char *env_pair(const char *k, const char *v) {
    size_t n = strlen(k) + strlen(v) + 2;
    char *p = malloc(n);
    if (p) snprintf(p, n, "%s=%s", k, v);
    return p;
}

void uaccount_spawn_release(struct uaccount_spawn *s) {
    free(s->groups);
    free(s->env);
    for (int i = 0; i < 4; i++) free(s->strs[i]);
    memset(s, 0, sizeof *s);
}

int uaccount_spawn_init(struct uaccount_spawn *s, struct sys_spawn_opts *o, const char *name,
                        char **base) {
    memset(s, 0, sizeof *s);
    struct passwd *pw = getpwnam(name);
    if (!pw) { errno = ENOENT; return -1; }
    // Copied before getgrouplist(): pw is static storage.
    uid_t uid = pw->pw_uid;
    gid_t gid = pw->pw_gid;
    s->strs[0] = env_pair("HOME", pw->pw_dir);
    s->strs[1] = env_pair("USER", pw->pw_name);
    s->strs[2] = env_pair("LOGNAME", pw->pw_name);
    s->strs[3] = env_pair("SHELL", pw->pw_shell);
    int ng = 0;
    getgrouplist(name, gid, NULL, &ng);
    s->groups = malloc((size_t)(ng > 0 ? ng : 1) * sizeof *s->groups);
    int ne = 0;
    while (base && base[ne]) ne++;
    s->env = malloc((size_t)(ne + 5) * sizeof *s->env);
    if (!s->groups || !s->env || !s->strs[0] || !s->strs[1] || !s->strs[2] || !s->strs[3] ||
        getgrouplist(name, gid, s->groups, &ng) < 0) {
        uaccount_spawn_release(s);
        errno = ENOMEM;
        return -1;
    }
    int k = 0;
    for (int i = 0; i < ne; i++)
        if (!is_login_var(base[i])) s->env[k++] = base[i];
    for (int i = 0; i < 4; i++) s->env[k++] = s->strs[i];
    s->env[k] = NULL;
    s->home = s->strs[0] + 5;   // past "HOME="
    o->flags |= SPAWN_CREDS;
    o->uid = uid;
    o->gid = gid;
    o->groups = (const uint32_t *)s->groups;
    o->ngroups = (unsigned)ng;
    o->cred_flags = 0;
    o->env = s->env;
    return 0;
}
