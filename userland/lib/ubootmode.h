#ifndef ULIB_UBOOTMODE_H
#define ULIB_UBOOTMODE_H

// The screen size GRUB sets at boot, for a display SaunaOS cannot set
// itself -- a VESA framebuffer on a card with no driver here (an RTX on
// a BIOS boot). Linux's GRUB_GFXMODE, chosen from Settings or `bootcfg
// resolution` instead of a file edited by hand.
//
// THREE grubenv VARIABLES, read by grub.cfg's stanza (ubootcfg.h):
//   toy_mode        the size every boot asks for; absent = GRUB's own
//   toy_mode_try    a size for the NEXT boot only; GRUB clears it before
//                   booting, so a size the monitor cannot show is shown
//                   once and the reset after it is back on toy_mode
//   toy_mode_tried  set by GRUB on that trial boot, cleared by GRUB on
//                   the next: this boot IS a trial, still unanswered
// Keeping a trial copies it to toy_mode; nothing else is needed to go
// back but a restart.
//
// A size the firmware does not offer comes up as GRUB's own choice
// instead; it is remembered (UBOOTMODE_CONF) and offered greyed after.

#define UBOOTMODE_KEPT   "toy_mode"
#define UBOOTMODE_TRY    "toy_mode_try"
#define UBOOTMODE_TRIED  "toy_mode_tried"
#define UBOOTMODE_CONF   "/etc/display.conf"
#define UBOOTMODE_REFUSED_KEY "firmware_refused"
#define UBOOTMODE_SIZE   16      // "3840x2160" and its terminator, with room

// The sizes offered, largest first: common VESA and panel geometries,
// since a BIOS lists those. 0 past the end.
int ubootmode_size_at(int i, int *w, int *h);
// "WxH" within 640x480..3840x2160, digits only. 1, or 0 refused.
int ubootmode_parse(const char *s, int *w, int *h);

struct ubootmode {
    char kept[UBOOTMODE_SIZE];     // "" = GRUB chooses
    char pending[UBOOTMODE_SIZE];  // a trial set for the next boot
    char tried[UBOOTMODE_SIZE];    // this boot is a trial of this size
    int  now_w, now_h;             // the screen now
    // Will a choice reach the screen? A display that sets modes itself
    // ignores GRUB's; a machine whose GRUB cannot keep a variable (no
    // grubenv, a live boot) cannot be told. `why_not` says which.
    int  usable;
    const char *why_not;
};

// Everything above, read fresh. Returns `usable`.
int ubootmode_read(struct ubootmode *m);

// Try `size` ("WxH") at the next boot, or keep GRUB's own choice from
// the next boot on (NULL). The stanza goes into grub.cfg first when it
// is missing (grub.cfg.bak keeps the old file). 0, or -1 with *why.
int ubootmode_try(const char *size, const char **why);
// Forget a pending trial.
int ubootmode_cancel(void);
// This boot's trial: keep it (toy_mode = it) or let it go (the next boot
// is back on toy_mode). Both clear toy_mode_tried, so it is asked once.
int ubootmode_keep(void);
int ubootmode_drop(void);

// The sizes this machine's firmware turned down, from UBOOTMODE_CONF.
int ubootmode_refused(const char *size);
int ubootmode_refuse(const char *size);

#endif
