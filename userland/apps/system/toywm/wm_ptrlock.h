#ifndef WM_PTRLOCK_H
#define WM_PTRLOCK_H

// A window's POINTER LOCK (abi/win_proto.h's WIN_REQ_POINTER_LOCK):
// Wayland's locked pointer with relative motion beside it. See
// wm_ptrlock.c.

// The client asked (`on`) or let go. `idx` is the window's index now.
void wm_ptrlock_request(int idx, int on);

// Once a frame, after input is read: the lock comes into force while its
// window has the focus and is suspended while it does not. While in
// force the pointer is HELD, and this frame's (x, y) become where.
void wm_ptrlock_frame(int *mx, int *my);

// Relative motion from the kernel (WIN_EV_RAW_REL), for the locked window.
void wm_ptrlock_motion(int dx, int dy);


#endif
