// An account's identity for a spawn -- see base/ulogin.h.
#include "base/ulogin.h"
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   // environ

// An old /etc/environment may name the XDG folders under /home; they are
// replaced too, not inherited beside the account's own.
static int is_login_var(const char *e) {
    static const char *const V[] = { "HOME=", "USER=", "LOGNAME=", "SHELL=", "XDG_CONFIG_HOME=",
                                     "XDG_DATA_HOME=", "XDG_STATE_HOME=", "XDG_CACHE_HOME=" };
    for (unsigned i = 0; i < sizeof V / sizeof V[0]; i++)
        if (strncmp(e, V[i], strlen(V[i])) == 0) return 1;
    return 0;
}

static char *pair(const char *k, const char *v) {
    size_t n = strlen(k) + strlen(v) + 2;
    char *p = malloc(n);
    if (p) snprintf(p, n, "%s=%s", k, v);
    return p;
}

void ulogin_creds_release(struct ulogin_creds *c) {
    free(c->groups);
    free(c->env);
    for (int i = 0; i < ULOGIN_VARS; i++) free(c->strs[i]);
    free(c->home);
    free(c->shell);
    memset(c, 0, sizeof *c);
}

int ulogin_creds(const char *account, unsigned cred_flags, struct sys_spawn_opts *o,
                 struct ulogin_creds *c) {
    memset(c, 0, sizeof *c);
    struct passwd *pw = account && account[0] ? getpwnam(account) : 0;
    if (!pw) return -1;
    // Copied before getgrouplist(): pw is static storage.
    uid_t uid = pw->pw_uid;
    gid_t gid = pw->pw_gid;
    c->home = strdup(pw->pw_dir);
    c->shell = strdup(pw->pw_shell);
    c->strs[0] = pair("HOME", pw->pw_dir);
    c->strs[1] = pair("USER", pw->pw_name);
    c->strs[2] = pair("LOGNAME", pw->pw_name);
    c->strs[3] = pair("SHELL", pw->pw_shell);
    static const char *const XDG[][2] = { { "XDG_CONFIG_HOME", ".config" }, { "XDG_DATA_HOME", ".local/share" },
                                          { "XDG_STATE_HOME", ".local/state" }, { "XDG_CACHE_HOME", ".cache" } };
    // A home of "/" would make "//.config", which a path compare misses.
    const char *base = c->home && strcmp(c->home, "/") ? c->home : "";
    for (int i = 0; i < 4; i++) {
        size_t n = strlen(XDG[i][0]) + strlen(base) + strlen(XDG[i][1]) + 3;
        if ((c->strs[4 + i] = malloc(n))) snprintf(c->strs[4 + i], n, "%s=%s/%s", XDG[i][0], base, XDG[i][1]);
    }
    int ng = 0;
    getgrouplist(account, gid, NULL, &ng);
    c->groups = malloc((size_t)(ng > 0 ? ng : 1) * sizeof *c->groups);
    int ne = 0;
    while (environ && environ[ne]) ne++;
    c->env = malloc((size_t)(ne + ULOGIN_VARS + 1) * sizeof *c->env);
    int ok = c->home && c->shell && c->groups && c->env;
    for (int i = 0; i < ULOGIN_VARS; i++) ok &= c->strs[i] != NULL;
    if (!ok || getgrouplist(account, gid, c->groups, &ng) < 0) {
        ulogin_creds_release(c);
        return -1;
    }
    int k = 0;
    for (int i = 0; i < ne; i++)
        if (!is_login_var(environ[i])) c->env[k++] = environ[i];
    for (int i = 0; i < ULOGIN_VARS; i++) c->env[k++] = c->strs[i];
    c->env[k] = NULL;
    o->flags |= SPAWN_CREDS;
    o->uid = uid;
    o->gid = gid;
    o->groups = (const uint32_t *)c->groups;
    o->ngroups = (unsigned)ng;
    o->cred_flags = cred_flags;
    o->env = c->env;
    return 0;
}
