// /etc AND /var/lib ARE ROOT'S (users D3b2a, docs/users-design.md): the
// desktop's account writes nothing there itself. What it changed there
// went three ways -- the person's own things into their home, the seat's
// through privd without a prompt, the machine's through privd asked of an
// administrator -- and this checks each as the account (1000) would:
//
//   * a write of its own into /etc or /var/lib is EACCES;
//   * display.scale (Scope=seat) set at the seat is SAVED, through privd,
//     with no prompt -- and refused without the seat;
//   * a program's volume and a sound card's format, through privd's seat
//     actions, land in root's files -- and a bad argument is refused;
//   * a theme saved lands in ~/.local/share/themes, the person's;
//   * a saved share lands in ~/.config/mounts.d, 0600 in a 0700 folder;
//   * `config register` and `service enable` refuse up front.
//
// What a broken version would still pass, and what stops it: a seat write
// that went to the file directly (the old route) would answer UNSAVED on
// a root-owned file -- so the seat check asks for SETTING_SAVED, not "not
// EACCES"; a privd that wrote without checking passes the good requests,
// so a bad one of each must be refused; a share written by someone else
// passes "it exists", so its owner and mode are read; a theme saved into
// /etc by a root process passes "it saved", so the path is checked. No
// desktop prompt can be answered here, so anything privd asked about
// would fail ENXIO: the seat's rights are checked to need no one.
//
// SPAWNED by tools/tests/gate/usertest_run.py as root.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "rt/sys.h"
#include "syscall_abi.h"
#include "cred_abi.h"
#include "setting_abi.h"
#include "sound_abi.h"
#include "base/uconf.h"
#include "base/usetting.h"
#include "base/upriv.h"
#include "base/utest.h"
#include "media/usndfmt.h"
#include "net/unetmount.h"
#include "ui/utheme_file.h"

#define SELF   "/tests/etcfiles_test"
#define HOME   "/home/user"
#define APP    "etcfiles_test"
#define CARD   "etcfiles-test-card"
#define STEM   "etcfiles-test"
#define SHARE  "etcfiles-test"
#define SEAT_SETTING "display.scale"

static const uint32_t GROUPS[] = { 1000, 10 };
static char *ENV[] = { "HOME=" HOME, "USER=user", "XDG_CONFIG_HOME=" HOME "/.config",
                       "XDG_DATA_HOME=" HOME "/.local/share", 0 };

static int spawn_as(const char *path, char *const *argv, unsigned cred_flags) {
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.argv = argv;
    o.env = ENV;
    o.flags = SPAWN_ARGV | SPAWN_CREDS;
    o.uid = o.gid = 1000;
    o.groups = GROUPS;
    o.ngroups = 2;
    o.cred_flags = cred_flags;
    return sys_spawn_opts(path, &o);
}

static int wait_code(int pid) {
    int code = -1;
    if (pid <= 0 || sys_waitpid(pid, &code) != pid) return -1000;
    return code;
}

// A program run as 1000 at the seat, its output into `out`: the exit code.
static int run_as(char *const *argv, char *out, size_t cap) {
    int fds[2];
    out[0] = 0;
    if (pipe(fds) < 0) return -1000;
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.argv = argv;
    o.env = ENV;
    o.stdout_fd = fds[1];
    o.flags = SPAWN_ARGV | SPAWN_CREDS;
    o.uid = o.gid = 1000;
    o.groups = GROUPS;
    o.ngroups = 2;
    o.cred_flags = CRED_SEAT;
    int pid = sys_spawn_opts(argv[0], &o);
    close(fds[1]);
    size_t n = 0;
    ssize_t k;
    while (pid > 0 && n + 1 < cap && (k = read(fds[0], out + n, cap - n - 1)) > 0) n += (size_t)k;
    out[n] = 0;
    close(fds[0]);
    return wait_code(pid);
}

static int self_as(const char *mode, unsigned cred_flags) {
    char *argv[] = { SELF, (char *)mode, 0 };
    return wait_code(spawn_as(SELF, argv, cred_flags));
}

// A file made as the account in a root folder: refused at the open.
static int create_refused(const char *path) {
    errno = 0;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { close(fd); unlink(path); return 0; }
    return errno == EACCES;
}

// An open for writing of a file that is there, without changing it.
static int append_refused(const char *path) {
    errno = 0;
    int fd = open(path, O_WRONLY | O_APPEND);
    if (fd >= 0) { close(fd); return 0; }
    return errno == EACCES;
}

// The seat's display.scale, set to what it is: privd's write, SAVED.
static int seat_set(void) {
    char now[SETTING_ABI_VALUE_MAX];
    if (!usetting_get(SEAT_SETTING, now, sizeof now)) return -2;
    errno = 0;
    return usetting_set(SEAT_SETTING, now);
}

// The person at the machine.
static int child_seat(void) {
    if (!create_refused("/etc/etcfiles_test.probe")) return 10;
    if (!create_refused("/var/lib/etcfiles_test.probe")) return 11;
    if (!append_refused("/etc/resolv.conf")) return 12;   // root's now, not the desktop's
    if (seat_set() != SETTING_SAVED) return 13;

    const char *vol[] = { APP, "37", 0 };
    if (upriv_action(UPRIV_ACT_APPVOLUME, vol, 0) != 0) return 20;
    const char *bad_app[] = { "a=b", "37", 0 };
    errno = 0;
    if (upriv_action(UPRIV_ACT_APPVOLUME, bad_app, 0) == 0 || errno != EINVAL) return 21;
    const char *bad_gain[] = { APP, "101", 0 };
    errno = 0;
    if (upriv_action(UPRIV_ACT_APPVOLUME, bad_gain, 0) == 0 || errno != EINVAL) return 22;

    struct usndfmt f = { .match = 0, .fixed = 44100, .allowed = USNDFMT_DEFAULT_ALLOWED, .bits = 16 };
    if (!usndfmt_save(CARD, &f)) return 30;
    const char *bad_rate[] = { CARD, "12345", "", "auto", 0 };
    errno = 0;
    if (upriv_action(UPRIV_ACT_SOUNDFORMAT, bad_rate, 0) == 0 || errno != EINVAL) return 31;
    const char *bad_card[] = { "a]b", "match", "", "auto", 0 };
    errno = 0;
    if (upriv_action(UPRIV_ACT_SOUNDFORMAT, bad_card, 0) == 0 || errno != EINVAL) return 32;

    char path[UTHEME_PATH_MAX];
    struct utheme t;
    utheme_default(&t);
    if (!utheme_own_path(STEM, path, sizeof path)) return 40;
    if (strcmp(path, HOME "/.local/share/themes/" STEM ".theme")) return 41;
    if (!utheme_save(path, &t, 0, "etcfiles_test")) return 42;
    if (!utheme_is_own(STEM)) return 43;

    struct unetmount m;
    memset(&m, 0, sizeof m);
    snprintf(m.name, sizeof m.name, SHARE);
    snprintf(m.source, sizeof m.source, "//127.0.0.1:1/none");
    if (!unetmount_save(&m)) return 50;
    return 0;
}

// The same account away from the machine: the seat's rights are not its.
static int child_noseat(void) {
    if (seat_set() != -1 || errno != EACCES) return 60;   // the library's gate, before privd
    const char *vol[] = { APP, "37", 0 };
    errno = 0;
    if (upriv_action(UPRIV_ACT_APPVOLUME, vol, 0) == 0) return 61;
    return 0;
}

static int owned_mode(const char *path, unsigned uid, unsigned mode) {
    struct stat st;
    return stat(path, &st) == 0 && st.st_uid == uid && (unsigned)(st.st_mode & 07777) == mode;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--seat")) return child_seat();
    if (argc > 1 && !strcmp(argv[1], "--noseat")) return child_noseat();

    utest_begin("etcfiles_test", "the desktop's writes into /etc, routed (users D3b2a)",
                UTEST_VERDICT_FILE);
    struct stat st;
    utest_check(stat("/etc", &st) == 0 && st.st_uid == 0 && st.st_gid == 0 &&
                (st.st_mode & 07777) == 0755, "/etc is root's, 0755");
    utest_check(stat("/var/lib", &st) == 0 && st.st_uid == 0 && (st.st_mode & 07777) == 0755,
                "/var/lib is root's, 0755");

    int code = self_as("--seat", CRED_SEAT);
    utest_checkf(code == 0, "1000 at the seat: /etc refused, the seat's writes through privd, "
                 "its own things at home (%d)", code);
    code = self_as("--noseat", 0);
    utest_checkf(code == 0, "1000 away from the seat: the seat's rights refused (%d)", code);

    char v[16];
    utest_check(uconf_get(SND_CONFIG_FILE, APP, v, sizeof v) && !strcmp(v, "37"),
                "privd wrote the program's volume into /etc/sound.conf");
    utest_check(uconf_get_in(SND_CARDS_FILE, CARD, "rate", v, sizeof v) && !strcmp(v, "44100"),
                "privd wrote the card's format into /etc/sound-cards.conf");
    utest_check(owned_mode(SND_CONFIG_FILE, 0, 0644) || (stat(SND_CONFIG_FILE, &st) == 0 && st.st_uid == 0),
                "...and /etc/sound.conf is still root's");
    const char *theme = HOME "/.local/share/themes/" STEM ".theme";
    utest_check(stat(theme, &st) == 0 && st.st_uid == 1000, "a saved theme is in ~/.local/share/themes, the person's");
    const char *share = HOME "/.config/mounts.d/" SHARE;
    utest_checkf(owned_mode(share, 1000, 0600), "a saved share is ~/.config/mounts.d/" SHARE ", the person's, 0600 (%o)",
                 stat(share, &st) == 0 ? (unsigned)(st.st_mode & 07777) : 0u);
    utest_check(owned_mode(HOME "/.config/mounts.d", 1000, 0700), "...in a folder that is theirs, 0700");

    // The command line: refused up front, SAYING why -- a refusal by the
    // file's mode would also exit 1, so the words are what is checked.
    static char out[512];
    char *reg[] = { "/bin/config", "register", "etcfilestest", "/tmp/etcfiles_test.conf", "x", 0 };
    code = run_as(reg, out, sizeof out);
    utest_checkf(code != 0 && strstr(out, "needs administrator rights") &&
                 stat("/etc/config.d/etcfilestest.conf", &st) != 0,
                 "`config register` as 1000 is refused up front, and writes nothing (%d: %.80s)", code, out);
    char *svc[] = { "/bin/service", "disable", "etcfilestest", 0 };
    code = run_as(svc, out, sizeof out);
    utest_checkf(code != 0 && strstr(out, "needs administrator rights"),
                 "`service disable` as 1000 is refused up front (%d: %.80s)", code, out);

    // Put back what the children made.
    uconf_unset(SND_CONFIG_FILE, APP);
    uconf_set_in(SND_CARDS_FILE, CARD, "rate", NULL);
    uconf_set_in(SND_CARDS_FILE, CARD, "allowed", NULL);
    uconf_set_in(SND_CARDS_FILE, CARD, "bits", NULL);
    unlink(theme);
    unlink(share);
    unlink("/etc/config.d/etcfilestest.conf");
    return utest_end();
}
