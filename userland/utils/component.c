// component -- list, remove and add back this machine's optional components.
//
// The command line of the Software app (docs/software-design.md), over
// the same engine: base/ucomponent.h reads the definitions and the state
// and removes, common/upd.h adds back -- fetched at the machine's OWN
// build, as Windows' DISM /Enable-Feature takes a payload from Windows
// Update. bootcfg and Boot Manager share ubootcfg the same way.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rt/sys.h"
#include "base/uargs.h"
#include "base/human.h"
#include "base/ucomponent.h"
#include "common/upd.h"

static const char *g_from;

static const struct uargs_opt OPTS[] = {
    { "from", 0, "URL", "with add: fetch from this channel's address, not the update server's", 0, &g_from },
    { 0 },
};

static const struct uargs_cmd CMDS[] = {
    { "list",   0,     "every component, its licence, size and state (also a bare component)" },
    { "remove", "ID...", "remove components, and what needs them" },
    { "add",    "ID...", "add components back, and what they need, at this machine's build" },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "component",
    .usage = "[OPTION]... [COMMAND [ID]...]",
    .summary = "List, remove and add back the optional components: DOOM, the Web page\n"
               "engine, the compiler, Windows 3.x support, the demos and tests.",
    .opts = OPTS,
    .cmds = CMDS,
    .notes = "ID is a component's id from `component list`. A removed component stays\n"
             "removed through every update; `add` fetches it from the update server at\n"
             "the build this machine runs. Exit status: 0 done, 1 failed, 2 bad usage.",
};

static struct ucomponent_set g_set;

static void on_log(void *ctx, const char *line) {
    (void)ctx;
    printf("  %s\n", line);
    fflush(stdout);
}

static int cmd_list(void) {
    if (!g_set.count) {
        printf("component: this build has no optional components (%s is empty)\n", UCOMPONENT_DIR);
        return 0;
    }
    printf("  %-10s %-22s %-18s %9s  %s\n", "ID", "Name", "Licence", "Size", "State");
    for (int i = 0; i < g_set.count; i++) {
        const struct ucomponent *c = &g_set.c[i];
        char sz[16];
        human_size(sz, sizeof sz, c->size);
        printf("  %-10s %-22s %-18s %9s  %s%s%s\n", c->id, c->name, c->license, sz,
               c->removed ? "removed" : "installed", c->needs[0] ? ", needs " : "", c->needs);
    }
    if (!ucomponent_show_gpl())
        printf("  GPL-licensed apps are hidden from the Start menu and desktop (%s=off)\n",
               UCOMPONENT_GPL_SETTING);
    return 0;
}

// The ids on the command line, each with its closure, deduplicated, in
// the order they run, into `out` (g_set.count of them at most). -1 for an
// unknown id.
static int closure(int argc, char **argv, int adding, int *out) {
    int n = 0, cap = g_set.count;
    int *tmp = malloc(sizeof *tmp * (size_t)(cap + 1));
    if (!tmp) { printf("component: out of memory\n"); return -1; }
    for (int a = 1; a < argc; a++) {
        int i = ucomponent_find(&g_set, argv[a]);
        if (i < 0) {
            printf("component: no component \"%s\" (component list)\n", argv[a]);
            free(tmp);
            return -1;
        }
        int k = adding ? ucomponent_add_closure(&g_set, i, tmp, cap)
                       : ucomponent_remove_closure(&g_set, i, tmp, cap);
        for (int j = 0; j < k; j++) {
            int dup = 0;
            for (int m = 0; m < n; m++) dup |= out[m] == tmp[j];
            if (!dup && n < cap) out[n++] = tmp[j];
            if (tmp[j] != i && !dup)
                printf("component: %s %s too -- %s\n", adding ? "adding" : "removing", g_set.c[tmp[j]].id,
                       adding ? "it is needed" : "it needs it");
        }
    }
    free(tmp);
    return n;
}

static int cmd_remove(int argc, char **argv) {
    int *order = malloc(sizeof *order * (size_t)(g_set.count + 1));
    int n = order ? closure(argc, argv, 0, order) : -1;
    if (n < 0) { free(order); return 1; }
    int rc = 0, at_boot = 0;
    struct ucomponent_progress pr = { 0, on_log, 0 };
    for (int k = 0; k < n; k++) {
        const struct ucomponent *c = &g_set.c[order[k]];
        if (c->removed) { printf("component: %s is already removed\n", c->id); continue; }
        char why[160];
        int boot = 0;
        printf("component: removing %s (%s)\n", c->id, c->name);
        if (ucomponent_remove(&g_set, order[k], &pr, &boot, why, sizeof why) != 0) {
            printf("component: %s: %s\n", c->id, why);
            rc = 1;
        }
        at_boot |= boot;
    }
    if (at_boot) printf("component: restart to finish (`reboot`)\n");
    free(order);
    return rc;
}

static int cmd_add(int argc, char **argv) {
    int *order = malloc(sizeof *order * (size_t)(g_set.count + 1));
    const char **ids = malloc(sizeof *ids * (size_t)(g_set.count + 1));
    int n = order && ids ? closure(argc, argv, 1, order) : -1;
    if (n < 0) { free(order); free(ids); return 1; }
    int m = 0;
    for (int k = 0; k < n; k++) {
        if (!g_set.c[order[k]].removed) { printf("component: %s is already installed\n", g_set.c[order[k]].id); continue; }
        ids[m++] = g_set.c[order[k]].id;
    }
    free(order);
    if (!m) { free(ids); return 0; }
    static char server[UPD_URL_MAX + 32];
    if (g_from) {
        snprintf(server, sizeof server, "%s", g_from);
    } else {
        static char base[UPD_URL_MAX], err[200];
        static struct upd_channels chs;
        upd_server_get(base, sizeof base);
        if (upd_resolve(base, NULL, server, sizeof server, &chs, err, sizeof err) != 0) {
            printf("component: %s\n", err);
            free(ids);
            return 1;
        }
    }
    struct upd_hooks h = { .log = on_log };
    static struct upd_plan p;
    int rc = upd_component_add(server, ids, m, &p, &h);
    if (rc != 0) printf("component: %s\n", p.error);
    else if (p.at_boot) printf("component: restart to finish (`reboot`)\n");
    upd_plan_free(&p);
    free(ids);
    return rc == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    ucomponent_load(&g_set);
    const char *cmd = a.argc ? a.argv[0] : "list";
    if (!strcmp(cmd, "list")) {
        if (a.argc > 1) return uargs_error(&PROG, "'list' takes no arguments");
        return cmd_list();
    }
    if (a.argc < 2) return uargs_error(&PROG, "'%s' takes ID...", cmd);
    if (!strcmp(cmd, "remove")) return cmd_remove(a.argc, a.argv);
    if (!strcmp(cmd, "add")) return cmd_add(a.argc, a.argv);
    return uargs_error(&PROG, "unknown command '%s'", cmd);
}
