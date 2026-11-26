#ifndef ULIB_ULEAVE_H
#define ULIB_ULEAVE_H

// The desktop's Leave page, asked for by a ROOT program: its Restart run
// for the person at the desktop, every app asked to close first and the
// restart stopped if one says no (WIN_REQ_LEAVE) -- logind's Reboot()
// going through the session rather than past it, where `reboot` would
// end the apps unasked.

#ifdef __cplusplus
extern "C" {
#endif

// 0 when the desktop has begun its Restart; -1 with errno EPERM (not
// root), ENXIO (no desktop is running), EBUSY (the person is already
// leaving), EINVAL.
int uleave_restart(void);

#ifdef __cplusplus
}
#endif
#endif
