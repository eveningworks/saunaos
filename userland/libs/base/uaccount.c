#include "base/uaccount.h"
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The same list as init's creds_for() (userland/services/init/init.c),
// which keeps its own copy because init links no libraries.
static int is_login_var(const char *e) {
    static const char *const V[] = { "HOME=", "USER=", "LOGNAME=", "SHELL=", "XDG_CONFIG_HOME=",
                                     "XDG_DATA_HOME=", "XDG_STATE_HOME=", "XDG_CACHE_HOME=" };
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
    for (int i = 0; i < UACCOUNT_NVARS; i++) free(s->strs[i]);
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
    static const char *const XDG[][2] = { { "XDG_CONFIG_HOME", ".config" }, { "XDG_DATA_HOME", ".local/share" },
                                          { "XDG_STATE_HOME", ".local/state" }, { "XDG_CACHE_HOME", ".cache" } };
    const char *dir = strcmp(pw->pw_dir, "/") ? pw->pw_dir : "";
    for (int i = 0; i < 4; i++) {
        size_t n = strlen(XDG[i][0]) + strlen(dir) + strlen(XDG[i][1]) + 3;
        if ((s->strs[4 + i] = malloc(n))) snprintf(s->strs[4 + i], n, "%s=%s/%s", XDG[i][0], dir, XDG[i][1]);
    }
    int ng = 0;
    getgrouplist(name, gid, NULL, &ng);
    s->groups = malloc((size_t)(ng > 0 ? ng : 1) * sizeof *s->groups);
    int ne = 0;
    while (base && base[ne]) ne++;
    s->env = malloc((size_t)(ne + UACCOUNT_NVARS + 1) * sizeof *s->env);
    int strs_ok = 1;
    for (int i = 0; i < UACCOUNT_NVARS; i++) strs_ok &= s->strs[i] != NULL;
    if (!s->groups || !s->env || !strs_ok || getgrouplist(name, gid, s->groups, &ng) < 0) {
        uaccount_spawn_release(s);
        errno = ENOMEM;
        return -1;
    }
    int k = 0;
    for (int i = 0; i < ne; i++)
        if (!is_login_var(base[i])) s->env[k++] = base[i];
    for (int i = 0; i < UACCOUNT_NVARS; i++) s->env[k++] = s->strs[i];
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
