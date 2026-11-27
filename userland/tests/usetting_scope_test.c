// A SETTING'S SCOPE (users stage D1b): a `Scope=user` key is the person's,
// read from $XDG_CONFIG_HOME/<file> over /etc/<file> and written there
// alone; a machine key ignores the person's file; a process with no
// XDG_CONFIG_HOME, or running as root, sees only the machine's values
// (base/usetting_schema.h). ROOT HAS NO PERSON'S LAYER even with the
// variable set (`sudo` keeps the environment), so the person's checks run
// in a child started as the desktop's account (`--child`), and the
// parent, as root, checks that it cannot see what the child wrote.
//
// uschema_origin() -- which layer decides, and both layers' values -- is
// asked at each step, as the Configuration Editor's From column asks it.
//
// WHAT A BROKEN VERSION WOULD STILL PASS: reading back through the same
// library proves nothing, so every write is checked in BOTH files through
// uconf_get(), an independent path to the bytes -- a write that went to
// /etc instead of the person's file reads back the same value through
// usetting_get(). The person's directory is a fresh one under /tmp that
// does not exist yet, so the write must make it.
//
// The machine's value is ROOT's to write (users D3b2a): the parent sets it
// before the child starts. Every value is put back before returning: /etc/desktop.conf's
// clock_seconds as it was, the scratch directory removed.
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include "base/usetting.h"
#include "base/usetting_schema.h"
#include "base/uconf.h"
#include "base/uaccount.h"
#include "base/utest.h"
#include "rt/sys.h"

#define USER_KEY   "desktop.clock_seconds"     // Scope=user, off|on
#define USER_FILE  "/etc/desktop.conf"
#define MACH_KEY   "update.show_internal"      // machine, on|off
#define MACH_FILE  "/etc/update.conf"
#define XDG        "/tmp/uscope"   // short enough for INFO's SETTING_ABI_FILE_MAX `file`
#define SELF       "/tests/usetting_scope_test"

static char g_was_user[64], g_was_mach[64];
static int g_had_user, g_had_mach;

static void put_back(void) {
    if (g_had_user) uconf_set(USER_FILE, "clock_seconds", g_was_user);
    else uconf_unset(USER_FILE, "clock_seconds");
    if (g_had_mach) uconf_set(MACH_FILE, "show_internal", g_was_mach);
    else uconf_unset(MACH_FILE, "show_internal");
    sys_unlink(XDG "/desktop.conf");
    sys_unlink(XDG "/update.conf");
    rmdir(XDG);
}

static int file_says(const char *path, const char *key, char *out, uint32_t cap) {
    return uconf_get(path, key, out, cap);
}

// uschema_origin() for `name`, with what it said each layer holds.
static int origin(const char *name, struct uschema_origin *o) {
    static struct uschema s;
    if (!uschema_find(name, &s)) return -1;
    return uschema_origin(&s, o);
}

static int child_main(void);

// The child's exit code: its failed checks, as utest_end() counts them.
static int run_as_desktop(void) {
    struct sys_spawn_opts o;
    struct uaccount_spawn acct;
    sys_spawn_opts_init(&o);
    extern char **environ;
    if (uaccount_spawn_init(&acct, &o, "user", environ) != 0) return -2000;
    o.args = "--child";
    int pid = sys_spawn_opts(SELF, &o);
    uaccount_spawn_release(&acct);
    int code = -1;
    if (pid <= 0 || sys_waitpid(pid, &code) != pid) return -1000;
    return code;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--child")) return child_main();
    utest_begin("usetting_scope_test", "a user-scoped setting layers ~/.config over /etc",
                UTEST_VERDICT_FILE);
    g_had_user = uconf_get(USER_FILE, "clock_seconds", g_was_user, sizeof g_was_user);
    g_had_mach = uconf_get(MACH_FILE, "show_internal", g_was_mach, sizeof g_was_mach);
    sys_unlink(XDG "/desktop.conf");
    sys_unlink(XDG "/update.conf");
    rmdir(XDG);
    utest_check(usetting_set(MACH_KEY, "on") == SETTING_SAVED, "root sets a machine key");
    // The machine's value of the user key, written by root: /etc is root's
    // (users D3b2a), so the child cannot make it.
    utest_check(uconf_set(USER_FILE, "clock_seconds", "off"), "root writes the machine's value of a user key");

    int code = run_as_desktop();
    utest_checkf(code == 0, "as the desktop's account the layers behave (%d failed; its lines above)", code);

    // ROOT, WITH THE VARIABLE SET -- what `sudo` hands on -- has no layer.
    uconf_set(USER_FILE, "clock_seconds", "off");
    mkdir(XDG, 0755);
    uconf_set(XDG "/desktop.conf", "clock_seconds", "on");
    setenv("XDG_CONFIG_HOME", XDG, 1);
    char v[64] = "", f[64] = "";
    usetting_get(USER_KEY, v, sizeof v);
    utest_checkf(!strcmp(v, "off"), "root does not read the person's file, XDG_CONFIG_HOME or not (%s)", v);
    utest_check(usetting_set(USER_KEY, "on") == SETTING_SAVED &&
                file_says(USER_FILE, "clock_seconds", f, sizeof f) && !strcmp(f, "on"),
                "...and writes the machine's file");
    char up[UCONF_USER_PATH_MAX];
    utest_check(uconf_user_path(USER_FILE, up, sizeof up) == 0, "uconf_user_path() gives root none");

    put_back();
    return utest_end();
}

static int child_main(void) {
    utest_begin("usetting_scope_child", "as the desktop's account", 0);

    // With no layer a write would go to /etc, which is root's (users
    // D3b2a): the account cannot change the machine's value -- the parent
    // wrote it ("off").
    unsetenv("XDG_CONFIG_HOME");
    utest_check(usetting_set(USER_KEY, "on") != SETTING_SAVED,
                "with no XDG_CONFIG_HOME a user key's write is refused: /etc is root's");
    char v[64] = "", f[64] = "";
    utest_check(file_says(USER_FILE, "clock_seconds", f, sizeof f) && !strcmp(f, "off"),
                "...and /etc keeps the machine's value");

    // A person, whose layer is still empty: the machine's value shows.
    setenv("XDG_CONFIG_HOME", XDG, 1);
    char up[UCONF_USER_PATH_MAX];
    utest_check(uconf_user_path(USER_FILE, up, sizeof up) && !strcmp(up, XDG "/desktop.conf"),
                "the person's half keeps the basename");
    utest_check(!uconf_user_path("/etc/settings.d/desktop.theme", up, sizeof up),
                "a file below /etc has no person's half");
    usetting_get(USER_KEY, v, sizeof v);
    utest_checkf(!strcmp(v, "off"), "an empty person's layer reads the machine's value (%s)", v);

    struct setting_msg info;
    utest_checkf(usetting_info(USER_KEY, &info) == 0 && !strcmp(info.file, XDG "/desktop.conf"),
                 "INFO names the person's file as the setting's (%s)", info.file);
    // A person's path too long for the ABI's field reads as ~/.config's.
    setenv("XDG_CONFIG_HOME", "/tmp/a-config-home-longer-than-forty", 1);
    utest_checkf(usetting_info(USER_KEY, &info) == 0 && !strcmp(info.file, "~/.config/desktop.conf"),
                 "...shortened, never truncated, when it outgrows the field (%s)", info.file);
    setenv("XDG_CONFIG_HOME", XDG, 1);

    // The person's write: their file only, the directory made for it.
    utest_check(usetting_set(USER_KEY, "on") == SETTING_SAVED, "the person sets it");
    utest_check(file_says(XDG "/desktop.conf", "clock_seconds", f, sizeof f) && !strcmp(f, "on"),
                "...into the person's file, made with its directory");
    utest_check(file_says(USER_FILE, "clock_seconds", f, sizeof f) && !strcmp(f, "off"),
                "...and /etc keeps the machine's value");
    usetting_get(USER_KEY, v, sizeof v);
    utest_checkf(!strcmp(v, "on"), "the person's value wins (%s)", v);
    utest_check(uconf_get_layered(USER_FILE, "clock_seconds", f, sizeof f) && !strcmp(f, "on"),
                "uconf_get_layered() agrees");
    struct uschema_origin o;
    utest_check(origin(USER_KEY, &o) == USCHEMA_FROM_USER && o.has_user && !strcmp(o.user, "on") &&
                o.has_machine && !strcmp(o.machine, "off"),
                "uschema_origin() says the person's file decides, with both layers' values");
    {
        struct uconf_layers l;
        uconf_load_layered(USER_FILE, &l);
        int ok = uconf_layers_get(&l, "clock_seconds", f, sizeof f) && !strcmp(f, "on");
        uconf_layers_free(&l);
        utest_check(ok, "uconf_load_layered() agrees");
    }

    // The same process without the layer: what a root service sees.
    unsetenv("XDG_CONFIG_HOME");
    usetting_get(USER_KEY, v, sizeof v);
    utest_checkf(!strcmp(v, "off"), "with no XDG_CONFIG_HOME the person's value is unseen (%s)", v);
    setenv("XDG_CONFIG_HOME", XDG, 1);

    // Setting what the layers already say writes nothing.
    sys_unlink(XDG "/desktop.conf");
    utest_check(usetting_set(USER_KEY, "off") == SETTING_SAVED &&
                !file_says(XDG "/desktop.conf", "clock_seconds", f, sizeof f),
                "choosing the machine's own value writes no person's key");

    // Unset: the person's key goes, the machine's value returns.
    usetting_set(USER_KEY, "on");
    struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_UNSET;
    strlcpy(m.name, USER_KEY, sizeof m.name);
    utest_check(usetting_dispatch(&m) == 0 && m.result == SETTING_SAVED, "the person unsets it");
    usetting_get(USER_KEY, v, sizeof v);
    utest_checkf(!strcmp(v, "off") && file_says(USER_FILE, "clock_seconds", f, sizeof f) &&
                 !strcmp(f, "off"), "...and reads the machine's value again, /etc untouched (%s)", v);
    utest_check(origin(USER_KEY, &o) == USCHEMA_FROM_MACHINE && !o.has_user,
                "...and uschema_origin() says the machine's file decides");
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_UNSET;
    strlcpy(m.name, USER_KEY, sizeof m.name);
    utest_check(usetting_dispatch(&m) == 0 && m.result == SETTING_INVALID,
                "a second unset has nothing of the person's to remove");

    // A machine key ignores the person's layer both ways -- and is root's
    // to set (users D3b): the parent made it "on" before starting this.
    errno = 0;
    utest_check(usetting_set(MACH_KEY, "off") < 0 && errno == EACCES, "a machine key is refused to the person");
    utest_check(file_says(MACH_FILE, "show_internal", f, sizeof f) && !strcmp(f, "on") &&
                !file_says(XDG "/update.conf", "show_internal", f, sizeof f),
                "...written to neither file");
    uconf_user_dir_ensure();
    uconf_set(XDG "/update.conf", "show_internal", "off");
    usetting_get(MACH_KEY, v, sizeof v);
    utest_checkf(!strcmp(v, "on"), "a machine key in the person's file is not read (%s)", v);
    utest_check(origin(MACH_KEY, &o) == USCHEMA_FROM_MACHINE && !o.has_user && !strcmp(o.machine, "on"),
                "...nor counted by uschema_origin()");

    // A path too long to open: the write is refused, never sent to /etc.
    char lng[UCONF_USER_PATH_MAX + 8];
    memset(lng, 'a', sizeof lng - 1);
    lng[0] = '/';
    lng[sizeof lng - 1] = '\0';
    setenv("XDG_CONFIG_HOME", lng, 1);
    file_says(USER_FILE, "clock_seconds", f, sizeof f);
    char before[64];
    strlcpy(before, f, sizeof before);
    int r = usetting_set(USER_KEY, strcmp(before, "on") ? "on" : "off");
    utest_checkf(r == SETTING_UNSAVED && file_says(USER_FILE, "clock_seconds", f, sizeof f) &&
                 !strcmp(f, before), "a person's path past PATH_MAX refuses the write (%d), /etc untouched", r);
    setenv("XDG_CONFIG_HOME", XDG, 1);

    // An empty value in the person's file is no value.
    uconf_set(XDG "/desktop.conf", "clock_seconds", "");
    usetting_get(USER_KEY, v, sizeof v);
    file_says(USER_FILE, "clock_seconds", f, sizeof f);
    utest_checkf(!strcmp(v, f), "an empty person's value reads the machine's (%s, /etc %s)", v, f);
    utest_check(origin(USER_KEY, &o) == USCHEMA_FROM_MACHINE && !o.has_user,
                "...and is no layer to uschema_origin() either");
    memset(&m, 0, sizeof m);
    m.op = SETTING_OP_UNSET;
    strlcpy(m.name, USER_KEY, sizeof m.name);
    utest_check(usetting_dispatch(&m) == 0 && m.result == SETTING_SAVED &&
                !file_says(XDG "/desktop.conf", "clock_seconds", f, sizeof f),
                "...and unset removes the empty key");
    return utest_end();
}
