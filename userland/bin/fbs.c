// fbs -- First-boot setup from the shell: is it done, show it at the
// next boot, show it now.
//
// The wizard itself is /bin/wm/system/firstboot, a GUI program; this is
// the half a person types, and both read lib/ufbs.h, so they cannot
// disagree about what "done" means.
#include <stdio.h>
#include <string.h>
#include "rt/sys.h"
#include "lib/uargs.h"
#include "lib/ufbs.h"

#define WIZARD "/bin/wm/system/firstboot"

static const struct uargs_cmd CMDS[] = {
    { "status", 0, "whether setup has run, and what this boot's command line says (the default)" },
    { "again",  0, "show First-boot setup at the next boot" },
    { "now",    0, "show it now, on this desktop" },
    { 0 }
};

static const struct uargs_prog PROG = {
    .name = "fbs",
    .usage = "[status] | again | now",
    .summary = "First-boot setup: the full-screen wizard a new machine shows once.",
    .cmds = CMDS,
    .notes = "The marker is " UFBS_MARKER ". The boot words fbs=on and fbs=off\n"
             "show it, or never show it, whatever the marker says (docs/boot-flags.md).",
};

static int status(void) {
    char how[64];
    if (ufbs_done(how, sizeof how))
        printf("First-boot setup: done (%s)\n", how[0] ? how : "no date recorded");
    else
        printf("First-boot setup: not done -- it shows at the next graphical boot\n");
    switch (ufbs_boot_word()) {
    case UFBS_WORD_ON:  printf("This boot has fbs=on: it shows whatever the marker says.\n"); break;
    case UFBS_WORD_OFF: printf("This boot has fbs=off: it never shows.\n"); break;
    default: break;
    }
    return 0;
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    const char *cmd = a.argc ? a.argv[0] : "status";
    if (a.argc > 1) return uargs_error(&PROG, "%s takes no arguments", cmd);

    if (!strcmp(cmd, "status")) return status();
    if (!strcmp(cmd, "again")) {
        if (ufbs_rearm() != 0) {
            fprintf(stderr, "fbs: could not remove %s\n", UFBS_MARKER);
            return 1;
        }
        printf("fbs: the next boot shows First-boot setup\n");
        return 0;
    }
    if (!strcmp(cmd, "now")) {
        int pid = sys_spawn(WIZARD, 0, -1);
        if (pid < 0) {
            fprintf(stderr, "fbs: could not start %s\n", WIZARD);
            return 1;
        }
        printf("fbs: First-boot setup is up (pid %d)\n", pid);
        return 0;
    }
    return uargs_error(&PROG, "no command `%s` -- status, again or now", cmd);
}
