// AN APP'S OWN FILE BY ITS BARE NAME (users stage D1c, base/uconf.h):
// "uowntest.conf" reads the person's keys over /etc's, key by key and
// section by section, and writes the person's file alone, its folders
// made; root -- even with XDG_CONFIG_HOME set, as `sudo` leaves it --
// reads and writes /etc. The state and cache folders (base/udatadir.h)
// follow the same root rule.
//
// WHAT A BROKEN VERSION WOULD STILL PASS: a read back through the same
// name proves nothing -- a write that went to /etc reads back the same.
// So every write is checked in BOTH files through their absolute paths,
// and the person's directory starts missing, so the write must make it.
// The person's half runs in a child started as the desktop's account
// (`--child`); the parent is root.
//
// Everything made is removed before returning.
#include <unistd.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include "base/uconf.h"
#include "base/udatadir.h"
#include "base/uaccount.h"
#include "base/utest.h"
#include "rt/sys.h"

#define NAME      "uowntest.conf"
#define MACHINE   "/etc/uowntest.conf"
#define LIST      "uownlist.conf"
#define XDG       "/tmp/uown"
#define PERSON    XDG "/uowntest.conf"
#define SUB       "uownsub/x.conf"
#define STATE     "/tmp/uownstate"
#define SELF      "/tests/uconf_own_test"

static void clean(void) {
    sys_unlink(MACHINE);
    sys_unlink("/etc/uownlist.conf");
    sys_unlink(PERSON);
    sys_unlink(XDG "/uownlist.conf");
    sys_unlink(XDG "/uownsub/x.conf");
    rmdir(XDG "/uownsub");
    rmdir(XDG);
    rmdir(STATE "/uownapp");
    rmdir(STATE);
}

static int says(const char *path, const char *section, const char *key, const char *want) {
    char v[64] = "";
    return uconf_get_in(path, section, key, v, sizeof v) && !strcmp(v, want);
}

static int child_main(void);

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
    utest_begin("uconf_own_test", "an app's own file: ~/.config over /etc, key by key",
                UTEST_VERDICT_FILE);
    clean();
    // The machine's file, as an administrator would leave it.
    utest_check(uconf_set(MACHINE, "a", "machine") && uconf_set(MACHINE, "b", "machine") &&
                uconf_set_in(MACHINE, "sec", "c", "machine"), "the machine's file is written");

    int code = run_as_desktop();
    utest_checkf(code == 0, "as the desktop's account the layers behave (%d failed; its lines above)", code);

    // ROOT, WITH THE VARIABLES SET, has no person's layer.
    setenv("XDG_CONFIG_HOME", XDG, 1);
    setenv("XDG_STATE_HOME", STATE, 1);
    utest_check(says(NAME, 0, "a", "machine"), "root reads the machine's value, not the person's");
    utest_check(uconf_set(NAME, "b", "root") && says(MACHINE, 0, "b", "root") &&
                !says(PERSON, 0, "b", "root"), "...and writes /etc, never the person's file");
    char *st = udatadir_state("uownapp/x", "/var/lib/uownapp/x");
    utest_checkf(st && !strcmp(st, "/var/lib/uownapp/x"), "root's state is the machine's path (%s)",
                 st ? st : "(null)");
    free(st);

    clean();
    return utest_end();
}

static int child_main(void) {
    utest_begin("uconf_own_child", "as the desktop's account", 0);
    setenv("XDG_CONFIG_HOME", XDG, 1);
    setenv("XDG_STATE_HOME", STATE, 1);

    // No file of the person's yet: the machine's values.
    utest_check(says(NAME, 0, "a", "machine") && says(NAME, "sec", "c", "machine"),
                "with no person's file the machine's values are read");
    char at[UCONF_USER_PATH_MAX];
    utest_checkf(uconf_own_read_path(LIST, at, sizeof at) && !strcmp(at, "/etc/uownlist.conf"),
                 "a whole-file reader is given /etc's until the person has one (%s)", at);
    long long gen = uconf_generation(NAME);

    // The person's write: their file, made with its directory (0700).
    utest_check(uconf_set(NAME, "a", "mine"), "the person sets a key");
    utest_check(says(PERSON, 0, "a", "mine"), "...into ~/.config's file");
    utest_check(says(MACHINE, 0, "a", "machine"), "...and /etc keeps the machine's value");
    struct stat sb;
    utest_checkf(stat(XDG, &sb) == 0 && (sb.st_mode & 0777) == 0700,
                 "the person's config folder is made 0700 (%o)", (unsigned)(sb.st_mode & 0777));
    utest_check(uconf_generation(NAME) != gen, "the name's generation moved with the person's file");

    // Key by key, sections kept.
    utest_check(says(NAME, 0, "a", "mine") && says(NAME, 0, "b", "machine"),
                "the person's key wins, a key they never set is the machine's");
    utest_check(uconf_set_in(NAME, "sec", "d", "p") && says(NAME, "sec", "d", "p") &&
                says(NAME, "sec", "c", "machine"), "...in a section too");
    struct etc_config_buf doc;
    char v[64] = "", w[64] = "";
    int ok = uconf_load(NAME, &doc) && etc_config_buf_get(&doc, "a", v, sizeof v) &&
             !strcmp(v, "mine") && etc_config_buf_get(&doc, "b", w, sizeof w) && !strcmp(w, "machine");
    etc_config_buf_free(&doc);
    utest_checkf(ok, "uconf_load() answers the merged document (a=%s b=%s)", v, w);

    // AN UNSET HIDES THE MACHINE'S VALUE TOO: a tombstone in the person's
    // file, never a write to /etc.
    char got[64] = "";
    utest_checkf(uconf_unset(NAME, "a") && !uconf_get(NAME, "a", got, sizeof got),
                 "an unset of a key /etc also has leaves it absent (read %s)", got);
    utest_check(says(MACHINE, 0, "a", "machine") && says(PERSON, 0, "a[$d]", "1"),
                "...by a tombstone in the person's file, /etc untouched");
    ok = uconf_load(NAME, &doc) && !etc_config_buf_get(&doc, "a", v, sizeof v) &&
         !etc_config_buf_get(&doc, "a[$d]", w, sizeof w);
    etc_config_buf_free(&doc);
    utest_check(ok, "uconf_load()'s document has neither the key nor its tombstone");
    utest_check(uconf_set(NAME, "a", "again") && says(NAME, 0, "a", "again") &&
                !uconf_get(PERSON, "a[$d]", got, sizeof got), "a set after it takes the tombstone away");
    utest_check(uconf_set(NAME, "z", "1") && uconf_unset(NAME, "z") && !uconf_get(PERSON, "z[$d]", got, sizeof got),
                "a key only the person had leaves no tombstone");
    utest_check(uconf_unset(NAME, "neither"), "an unset of a key neither file has succeeds");

    // A name with folders makes them; a name that climbs is refused.
    utest_check(uconf_set(SUB, "k", "v") && says(XDG "/uownsub/x.conf", 0, "k", "v"),
                "a name with a folder writes there, the folder made");
    utest_check(!uconf_set("../uownesc.conf", "k", "v") && !uconf_set("a//b.conf", "k", "v") &&
                !uconf_set("./x.conf", "k", "v"), "a name with .., . or an empty part is refused");

    // A whole-file writer and reader agree on the person's file.
    utest_check(uconf_own_write_path(LIST, at, sizeof at) && !strcmp(at, XDG "/uownlist.conf"),
                "a whole-file writer is given the person's file");
    int fd = sys_open(at, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (fd >= 0) { sys_write(fd, "x\n", 2); sys_close(fd); }
    utest_checkf(uconf_own_read_path(LIST, at, sizeof at) && !strcmp(at, XDG "/uownlist.conf"),
                 "...and a reader then reads it (%s)", at);

    // The state folder is the person's.
    char *st = udatadir_state_prepare("uownapp/x", "/var/lib/uownapp/x");
    utest_checkf(st && !strcmp(st, STATE "/uownapp/x") && stat(STATE "/uownapp", &sb) == 0,
                 "the person's state is $XDG_STATE_HOME's, its folders made (%s)", st ? st : "(null)");
    free(st);
    return utest_end();
}
