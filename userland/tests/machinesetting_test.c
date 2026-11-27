// WHO MAY CHANGE A SETTING (users D3b, docs/users-design.md): the
// machine's are root's alone -- the kernel's refused at SYS_SETTING, a
// declared one in libbase -- a person's anyone's, a seat's the person at
// the machine's; privd's `settings` action makes a machine change for an
// account that may not.
//
// THE KERNEL'S PERSON'S SETTINGS (users D3c): a SET of one is the
// MACHINE's default, root's; the person at the machine APPLIES theirs,
// which libbase keeps in ~/.config/saunaos.conf and a sign-in applies
// (usetting_session_apply()), the machine's value kept for REVERT.
//
// What a broken version would still pass, and what stops it: a gate that
// refused everyone passes every refusal, so each kind is also asked of a
// caller that must get PAST it; a gate that let everyone through passes
// every check made as root, so the refusals are asked of children started
// as 1000, with and without the seat. Every write sets a value to what it
// already is, so nothing here changes the machine.
//
// SPAWNED by tools/tests/gate/usertest_run.py as root.
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include "rt/sys.h"
#include "syscall_abi.h"
#include "cred_abi.h"
#include "setting_abi.h"
#include "base/usetting.h"
#include "base/upriv.h"
#include "base/utest.h"
#include "base/uconf.h"

#define SELF "/tests/machinesetting_test"

#define KERNEL_MACHINE "system.timezone"     // SETTING_WHO_MACHINE
#define KERNEL_PERSON  "system.mouse.speed"  // SETTING_WHO_PERSON
#define KERNEL_SEAT    "system.brightness"   // SETTING_WHO_SEAT
#define DECL_MACHINE   "system.hostname"     // no Scope=
#define DECL_PERSON    "display.scale"       // Scope=user (users D3c)
// The person's layer for the children that have one: a folder of its own.
#define XDG_DIR        "/tmp/machinesetting_test.d"
#define XDG_FILE       XDG_DIR "/saunaos.conf"

static const uint32_t GROUPS[] = { 1000, 10 };

static char *XDG_ENV[] = { "XDG_CONFIG_HOME=" XDG_DIR, 0 };

static int spawn_as(const char *mode, unsigned cred_flags) {
    char *argv[] = { SELF, (char *)mode, 0 };
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.argv = argv;
    if (!strcmp(mode, "--person")) o.env = XDG_ENV;
    o.flags = SPAWN_ARGV | SPAWN_CREDS;
    o.uid = o.gid = 1000;
    o.groups = GROUPS;
    o.ngroups = 2;
    o.cred_flags = cred_flags;
    return sys_spawn_opts(SELF, &o);
}

static int wait_code(int pid) {
    int code = -1;
    if (pid <= 0 || sys_waitpid(pid, &code) != pid) return -1000;
    return code;
}

// SET `name` to its own value: -1 with EACCES when refused at the gate;
// anything else got past it (a value an unavailable setting refuses is
// still past the gate).
static int set_same(const char *name) {
    char now[SETTING_ABI_VALUE_MAX];
    if (!usetting_get(name, now, sizeof now)) return -2;
    errno = 0;
    int rc = usetting_set(name, now);
    return rc < 0 && errno == EACCES ? -1 : 0;
}

static int raw(uint32_t op, const char *name, const char *value) {
    struct setting_msg m;
    memset(&m, 0, sizeof m);
    m.op = op;
    if (name) strlcpy(m.name, name, sizeof m.name);
    if (value) strlcpy(m.value, value, sizeof m.value);
    errno = 0;
    if (sys_setting(&m) != 0) return errno == EACCES ? -1 : -2;
    return op == SETTING_OP_PERSON_COUNT || op == SETTING_OP_REVERT ? (int)m.count : (int)m.result;
}

static int child(int seat) {
    if (set_same(KERNEL_MACHINE) != -1) return 10;
    if (set_same(DECL_MACHINE) != -1) return 11;
    // With no person's layer (no XDG_CONFIG_HOME) a SET is the machine's.
    if (set_same(KERNEL_PERSON) != -1) return 12;
    if (set_same(KERNEL_SEAT) != (seat ? 0 : -1)) return 13;
    // A person's value is the seat's to apply, and nobody's to put back.
    char now[SETTING_ABI_VALUE_MAX];
    if (!usetting_get(KERNEL_PERSON, now, sizeof now)) return 14;
    int applied = raw(SETTING_OP_APPLY, KERNEL_PERSON, now);
    // Dropped at once, so a failure below leaves no person's value live.
    int dropped = seat ? raw(SETTING_OP_APPLY, KERNEL_PERSON, "") : SETTING_SAVED;
    if (applied != (seat ? SETTING_SAVED : -1)) return 19;
    if (dropped != SETTING_SAVED) return 22;
    if (raw(SETTING_OP_APPLY, KERNEL_MACHINE, "UTC") != -1) return 20;   // the gate's, seat or not
    if (raw(SETTING_OP_REVERT, 0, 0) != -1) return 21;
    // The library's answer agrees with the gate's, for a UI's shield.
    struct setting_msg m;
    if (usetting_info(KERNEL_MACHINE, &m) != 0 || !usetting_needs_admin(&m)) return 15;
    if (usetting_info(KERNEL_PERSON, &m) != 0 || usetting_needs_admin(&m)) return 16;
    if (usetting_info(DECL_PERSON, &m) != 0 || usetting_needs_admin(&m)) return 17;
    // UNSET is a write too.
    struct setting_msg u;
    memset(&u, 0, sizeof u);
    u.op = SETTING_OP_UNSET;
    strlcpy(u.name, KERNEL_MACHINE, sizeof u.name);
    errno = 0;
    if (usetting_dispatch(&u) == 0 || errno != EACCES) return 18;
    return 0;
}

// At the seat WITH a person's layer: a change of pointer speed is applied
// and kept in their file, /etc untouched; a sign-in's apply reads it back.
static int child_person(void) {
    char now[SETTING_ABI_VALUE_MAX], want[16], v[SETTING_ABI_VALUE_MAX];
    int speed = 0;
    if (!usetting_get_int(KERNEL_PERSON, &speed)) return 30;
    int other = speed > 50 ? speed - 10 : speed + 10;
    snprintf(want, sizeof want, "%d", other);
    if (usetting_set(KERNEL_PERSON, want) != SETTING_SAVED) return 31;
    if (!usetting_get(KERNEL_PERSON, now, sizeof now) || strcmp(now, want)) return 32;
    if (!uconf_get(XDG_FILE, "mouse.speed", v, sizeof v) || strcmp(v, want)) return 33;
    if (raw(SETTING_OP_PERSON_COUNT, 0, 0) != 1) return 34;
    // UNSET: the machine's back, and the person's file forgets it.
    struct setting_msg u;
    memset(&u, 0, sizeof u);
    u.op = SETTING_OP_UNSET;
    strlcpy(u.name, KERNEL_PERSON, sizeof u.name);
    if (usetting_dispatch(&u) != 0 || u.result != SETTING_SAVED) return 35;
    if (!usetting_get_int(KERNEL_PERSON, &speed) || speed == other) return 36;
    if (uconf_get(XDG_FILE, "mouse.speed", v, sizeof v)) return 37;
    // A sign-in: the file's value applied, the machine's kept for init.
    if (!uconf_set(XDG_FILE, "mouse.speed", want)) return 38;
    if (usetting_session_apply() < 1) return 39;
    if (!usetting_get(KERNEL_PERSON, now, sizeof now) || strcmp(now, want)) return 40;
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--noseat")) return child(0);
    if (argc > 1 && !strcmp(argv[1], "--seat")) return child(1);
    if (argc > 1 && !strcmp(argv[1], "--person")) return child_person();

    utest_begin("machinesetting_test", "who may change a setting, and privd's settings action",
                UTEST_VERDICT_FILE);
    // Root passes every gate.
    utest_check(set_same(KERNEL_MACHINE) == 0, "root sets the machine's kernel setting");
    utest_check(set_same(DECL_MACHINE) == 0, "root sets the machine's declared setting");
    struct setting_msg m;
    utest_check(usetting_info(KERNEL_MACHINE, &m) == 0 && !usetting_needs_admin(&m),
                "root needs no administrator prompt");
    utest_check(usetting_info(KERNEL_PERSON, &m) == 0 && (m.sflags & SETTING_ABI_SF_PERSON),
                "the kernel marks pointer speed a person's");
    utest_check(usetting_info(KERNEL_SEAT, &m) == 0 && (m.sflags & SETTING_ABI_SF_SEAT),
                "the kernel marks brightness the seat's");

    int code = wait_code(spawn_as("--noseat", 0));
    utest_checkf(code == 0, "1000 without the seat: machine and seat refused, a person's APPLY too (%d)", code);
    code = wait_code(spawn_as("--seat", CRED_SEAT));
    utest_checkf(code == 0, "1000 at the seat: the seat's and a person's APPLY allowed, the machine's still refused (%d)", code);

    // A person's own pointer speed (users D3c). REVERT is the machine's
    // whole, so only where no session's values are live.
    char etc_before[SETTING_ABI_VALUE_MAX] = "", etc_after[SETTING_ABI_VALUE_MAX] = "";
    int speed_before = 0, speed_after = -1;
    if (raw(SETTING_OP_PERSON_COUNT, 0, 0) == 0) {
        mkdir(XDG_DIR, 0755);
        chown(XDG_DIR, 1000, 1000);
        uconf_get("/etc/saunaos.conf", "mouse.speed", etc_before, sizeof etc_before);
        usetting_get_int(KERNEL_PERSON, &speed_before);
        code = wait_code(spawn_as("--person", CRED_SEAT));
        utest_checkf(code == 0, "a person's pointer speed: applied, in their file, unset, applied again at "
                     "sign-in (%d)", code);
        uconf_get("/etc/saunaos.conf", "mouse.speed", etc_after, sizeof etc_after);
        utest_checkf(!strcmp(etc_before, etc_after), "...and /etc's is the machine's still (%s, %s)",
                     etc_before, etc_after);
        int n = raw(SETTING_OP_REVERT, 0, 0);
        usetting_get_int(KERNEL_PERSON, &speed_after);
        utest_checkf(n == 1 && speed_after == speed_before, "REVERT, as init does at a sign-out, puts "
                     "the machine's back (%d settings, %d -> %d)", n, speed_before, speed_after);
        unlink(XDG_FILE);
        rmdir(XDG_DIR);
    }

    // privd's action, as root (no prompt): every name checked before anything
    // is asked, an unknown one refused.
    char tz[SETTING_ABI_VALUE_MAX], val[SETTING_ABI_VALUE_MAX + 1];
    if (usetting_get(KERNEL_MACHINE, tz, sizeof tz)) {
        struct usetting_change c[] = { { KERNEL_MACHINE, tz } };
        int rc = usetting_apply_admin(c, 1);
        utest_checkf(rc == 0, "privd's settings action sets a machine setting (%d, errno %d)", rc, errno);
        struct usetting_change bad[] = { { KERNEL_MACHINE, tz }, { "system.no_such_setting", "1" } };
        errno = 0;
        rc = usetting_apply_admin(bad, 2);
        utest_checkf(rc < 0 && errno == EINVAL, "...and refuses a request naming an unknown setting (errno %d)",
                     errno);
        snprintf(val, sizeof val, "=%s", tz);
        const char *args[] = { KERNEL_MACHINE, val, KERNEL_MACHINE, 0 };   // a name with no value
        errno = 0;
        rc = upriv_action(UPRIV_ACT_SETTINGS, args, 0);
        utest_checkf(rc < 0 && errno == EINVAL, "...and one whose names and values do not pair (errno %d)", errno);
    }
    return utest_end();
}
