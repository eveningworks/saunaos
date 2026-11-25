#ifndef ULIB_UPRIV_H
#define ULIB_UPRIV_H
// Administrator rights through privd, the root broker -- polkit's and
// UAC's shape, never setuid (docs/users-design.md, stage B). A program
// that is not root asks privd for ONE thing: an ACTION from its fixed
// table, or an ELEVATED SPAWN of exactly one command line. privd checks
// who is asking, asks the person through the desktop's prompt, and
// answers. Also the AGENT half: the compositor registers here to draw
// that prompt.
//
// **EVERY CALL BLOCKS UNTIL THE PERSON ANSWERS** (UPRIV_TIMEOUT_MS at
// most): a GUI program asks through ui/uapp.h's uapp_run_then(), off its frame.
#include <stdint.h>
#include "rt/sys.h"
#ifdef __cplusplus
extern "C" {
#endif

#define UPRIV_SERVICE    "privd"
#define UPRIV_TIMEOUT_MS (5 * 60 * 1000)

// The record types on the channel (base/ubulk.h); `id` pairs an answer
// with its request.
#define UPRIV_T_SPAWN   1   // client: u32 flags, path\0, argv[0]\0 argv[1]\0 ...
#define UPRIV_T_ACTION  2   // client: u32 flags, action\0, arg\0 ...
#define UPRIV_T_ANSWER  3   // privd: i32 0 or -errno
#define UPRIV_T_AGENT   4   // agent: "I draw the prompt" (no payload)
#define UPRIV_T_PROMPT  5   // privd -> agent: headline\0 body\0 action\0 program\0 asker\0
                            //   password_for\0 -- the account whose password to ask, "" for none
#define UPRIV_T_VERDICT 6   // agent: u32 1 allow, 0 cancel; then the password\0 if one was asked
#define UPRIV_T_CANCEL  7   // privd -> agent: close prompt `id`, its asker is gone
#define UPRIV_T_AUTH    8   // client: u32 flags, account\0 password\0 -- is it right?
#define UPRIV_T_RETRY   9   // privd -> agent: prompt `id`'s password was wrong; u32 tries left
#define UPRIV_T_LOGIN  10   // the greeter: u32 flags, account\0 password\0 -- sign in (D2b)

// The request's flags. SELF_CONSENT: the asker already asked the
// person on its own terminal -- `sudo` on a TEXT boot, where no desktop
// draws a prompt. privd honours it only when no agent has ever
// registered this boot, and it is a typo guard, not a boundary: stage
// D's password is what makes a text prompt one.
#define UPRIV_SELF_CONSENT 1u
// WITH_PASSWORD: the request's first string is the ASKER's password,
// read on its own terminal (`sudo`) -- a right one from an account in
// wheel is consent, at the machine or not, as sudo's is.
#define UPRIV_WITH_PASSWORD 2u
// PROBE, on UPRIV_T_AUTH: does the account have a password at all? The
// password sent is ignored and "" tested, so it answers at once.
#define UPRIV_PROBE 4u
// A wrong password costs this long before the answer (pam_faildelay),
// and the prompt asks this many times before the request is refused.
#define UPRIV_FAIL_DELAY_MS 2000
#define UPRIV_AUTH_TRIES    3

// The actions privd runs itself. Arguments are strings.
#define UPRIV_ACT_SETTIME   "settime"     // seconds since 1970, UTC
#define UPRIV_ACT_MODLOAD   "modload"     // a module's path
#define UPRIV_ACT_MODUNLOAD "modunload"   // a module's name
#define UPRIV_ACT_KILL      "kill"        // pid, signal number
#define UPRIV_ACT_BOOTCFG   "bootcfg"     // grub.cfg's whole new text
#define UPRIV_ACT_NETLINK   "netlink"     // card, which, rates, eee, flow, moderation
// An account's password (docs/users-design.md, D2a): account, its current
// password, the new one ("" removes it). Root may set any account's and
// sends no current one; anyone else only their own, with the current one
// ("" when it has none). Never prompted: the current password is the proof.
#define UPRIV_ACT_PASSWD    "passwd"
// Automatic sign-in (users D2b): the account to sign in at boot, or ""
// for the login screen. "" is the seat's without asking -- it only makes
// the machine ask for more; naming an account is ASKED (with its
// password once it has one), and only for the asker's own account.
#define UPRIV_ACT_AUTOLOGIN "autologin"
// The SEAT's, granted to the person at the machine WITHOUT asking --
// logind's set-reboot-to-boot-loader-entry is allow_active: which entry
// the next boot takes, and the screen size it tries.
#define UPRIV_ACT_BOOTENV   "bootenv"     // a boot-menu variable, its value or ""
#define UPRIV_ACT_BOOTMODE  "bootmode"    // a screen size to try, or ""
// ...and who is on this machine remotely: end a remote session, or make it
// view-only (SIGTERM, SIGUSR1, SIGUSR2 to a QUERY_REMOTESESS leader only).
#define UPRIV_ACT_REMOTE    "remote"      // pid, signal number

// Runs `path` with `argv` (argv[0] included, NULL-terminated) as root,
// as a child of this process -- `o` gives its streams and the rest, as
// sys_spawn_opts() takes them (NULL: the defaults). The pid, or -1 with
// errno: EPERM (privd refused, or this account may not ask), ECANCELED
// (the person said no), ENXIO (no one to ask: the desktop is not
// running), ENOENT (no privd), ETIMEDOUT.
int upriv_spawn(const char *path, char *const *argv, const struct sys_spawn_opts *o,
                unsigned flags);

// Runs one action. `args` is NULL-terminated. 0, or -1 with errno as
// above, or what the action itself failed with.
int upriv_action(const char *action, const char *const *args, unsigned flags);

// upriv_spawn() with the asker's own password, read on its terminal
// (UPRIV_WITH_PASSWORD). EACCES: the password was wrong.
int upriv_spawn_password(const char *path, char *const *argv, const struct sys_spawn_opts *o,
                         const char *password);
// Is `password` the account's? 0 yes; -1 with errno EACCES (no -- after
// UPRIV_FAIL_DELAY_MS), EPERM (not root, and not this account's own),
// ENOENT (no privd). An account with no password matches only "".
int upriv_auth(const char *account, const char *password);
// 1 when the account has a password, 0 when it has none, -1 with errno.
// Root, or the account's own.
int upriv_has_password(const char *account);
// SIGN IN (users D2b), the greeter's alone: privd checks the password and
// asks init to start `account`'s session (INITCTL_SESSION). 0 when it
// has; -1 with errno EACCES (wrong, after the fail delay), EPERM (not the
// greeter, or not a person's account), EBUSY (a session is on), EAGAIN (a
// wrong answer still pending), EIO (init did not answer).
int upriv_login(const char *account, const char *password);
// Sets (or, with `password` "", removes) the account's password --
// UPRIV_ACT_PASSWD. `current` NULL for root.
int upriv_passwd(const char *account, const char *current, const char *password);

// An ADMINISTRATOR TOOL's first line -- Disks, the Installer, System
// Update; Windows' requireAdministrator manifest. Root: returns 0 and the
// program carries on. Anyone else: asks privd to run this same program
// with these same arguments elevated, waits for it and EXITS with its
// code (this pid stays what the launcher knows);
// refused or declined, returns -1 with errno, and the program decides
// (an administrator tool exits).
int upriv_elevate_self(char **argv);

// --- the agent (the compositor) ----------------------------------------

struct upriv_prompt {
    uint32_t id;
    const char *headline, *body, *action, *program, *asker;   // into `buf`
    const char *password_for;   // "" when Allow alone answers
    uint32_t tries_left;        // a RETRY's
    char buf[1024];
};

struct ubulk_client;
// Registers as the prompt's agent. A handle, or NULL when privd is not
// running (try again later). privd believes only the desktop.
struct ubulk_client *upriv_agent_open(void);
// The next prompt to show: 1 with `p` filled, -1 for a cancel of `p->id`
// (close that prompt unanswered), 2 for a RETRY of `p->id` (its password
// was wrong; `p->tries_left` more), 0 for nothing waiting.
int  upriv_agent_next(struct ubulk_client *a, struct upriv_prompt *p);
void upriv_agent_answer(struct ubulk_client *a, uint32_t id, int allow);
// Allow, with the password the prompt asked for.
void upriv_agent_answer_password(struct ubulk_client *a, uint32_t id, const char *password);
void upriv_agent_close(struct ubulk_client *a);

#ifdef __cplusplus
}
#endif
#endif
