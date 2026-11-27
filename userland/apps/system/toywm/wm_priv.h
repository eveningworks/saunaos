#ifndef WM_PRIV_H
#define WM_PRIV_H
// The desktop as privd's prompt agent -- wm_priv.c. Called once a loop
// iteration: registers with privd (retrying while it is not running),
// takes its questions, and shows each as the Authentication Required
// dialog in turn.
void wm_priv_poll(void);

// The debug console's `gui prompt allow|deny|ask` -- what a GUI test's
// harness sets, which already has the run of the machine through that
// console: ALLOW and DENY answer every question at once, unseen; ASK, the
// default, shows it. Never a setting: no program may answer for the person.
#define WM_PRIV_ASK   0
#define WM_PRIV_ALLOW 1
#define WM_PRIV_DENY  2
void wm_priv_set_policy(int policy);
int  wm_priv_policy(void);
int  wm_priv_registered(void);   // privd took us as its agent
unsigned wm_priv_questions(void);   // questions received this boot

// SIGNAL A WINDOW'S PROCESS, whoever owns it: sys_kill(), and for an
// elevated (root) one privd's kill action, which takes the desktop's word
// without asking -- the person chose Force Quit here. 0, or -1.
int wm_priv_kill(int pid, int sig);

// A SEAT ACTION from the desktop itself (base/upriv.h: one privd granted the
// person at the machine without asking -- the per-app volume), sent over
// the AGENT's own channel and its answer ignored: it never blocks the
// frame, and no second channel replaces the agent's (no wm_priv_reopen()
// needed). `args` NULL-terminated. 0 when sent, -1 when it could not be
// (no agent: privd not running).
int wm_priv_seat_action(const char *action, const char *const *args);

// The screen locked, or came back from the saver or from off: privd
// forgets the passwords `login.admin_prompt`'s remember choices keep.
void wm_priv_forget(void);

// After any OTHER privd call this process made (base/upriv.h's one
// channel per process): registers as the agent again at the next poll.
void wm_priv_reopen(void);
// A prompt is showing or waiting: reopening now would drop it unanswered.
int wm_priv_busy(void);
#endif
