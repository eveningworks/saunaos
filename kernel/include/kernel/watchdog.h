#ifndef KERNEL_WATCHDOG_H
#define KERNEL_WATCHDOG_H

#include <stdint.h>

// THE HARDWARE WATCHDOG: a timer in the chipset that RESETS the machine
// unless it is fed. Linux's watchdog core and /dev/watchdog, in the small:
// one device at most, one opener at a time, and the feeder is init (pid 1)
// -- systemd's RuntimeWatchdogSec= shape. A machine nobody feeds a
// watchdog on is a machine where the watchdog is stopped: the driver halts
// it at probe and only an open of /dev/watchdog starts it.
//
// `watchdog=off` on the GRUB line refuses the open (the device stays
// stopped); `watchdog=<seconds>` sets how long an unfed machine runs
// before the reset (default WATCHDOG_DEFAULT_SECS).

#define WATCHDOG_DEFAULT_SECS 60
// THE FLOOR: init feeds every 2 s behind waits of up to 2 s, so feeds can
// be ~4 s apart, and the ASUS's firmware resets at HALF the timeout.
#define WATCHDOG_MIN_SECS 15

struct hw_watchdog {
    const char *name;     // the DRIVER_DECLARE name: "itco"
    int min_secs, max_secs;
    // Arm with a reset after about `secs` unfed. 1 when it is counting;
    // 0 when the hardware refused (a firmware lock), said in the log.
    int  (*start)(int secs);
    void (*ping)(void);   // NO LOCK, NO LOG: the panic path calls it
    void (*stop)(void);   // the same
};

// A driver's probe, once it has the hardware halted.
void watchdog_register(const struct hw_watchdog *w);
int  watchdog_present(void);
int  watchdog_running(void);

// `watchdog=`: 0 = off, else seconds. A value that is not a number keeps
// the default, clamped to what the device can count.
int watchdog_parse_secs(const char *value, int *out);

// THE PANIC PATH'S TWO CALLS. Neither locks, allocates or logs. A feed
// during the countdown keeps the warm reset the panic chose from being
// overtaken by the watchdog's hard one; a halt (`panic=0`) stops it, since
// a machine asked to stay on its panic screen is meant to be read.
void watchdog_panic_ping(void);
void watchdog_panic_stop(void);

// The kernel debugger holding the machine stopped: neither the hardware
// timer nor the lockup detector may count that time (kdebug.c's stop()).
void watchdog_debugger_hold(int stopped);
int  watchdog_debugger_held(void);

// A clean shutdown or reboot: stop it before the flush, so a slow flush
// cannot turn a power-off into a reset.
void watchdog_shutdown(void);

// sys_open()'s half of "one opener": 0, or -ENOENT (no device), -EPERM
// (watchdog=off), -EBUSY (held). An open that then fails to make a
// descriptor gives the claim back.
int  watchdog_claim(void);
void watchdog_unclaim(void);
// The open made a description but no descriptor: its release stops the
// timer, since nobody holds anything that could feed it.
void watchdog_open_failed(void);

#endif
