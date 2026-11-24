#ifndef ULIB_UBOOTMODE_H
#define ULIB_UBOOTMODE_H

#ifdef __cplusplus
extern "C" {
#endif

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
// A size the firmware does not offer comes up as GRUB's fallback
// instead; it is remembered (UBOOTMODE_CONF) and offered greyed after.
// A trial that comes up at NEITHER is a boot loader that set no mode at
// all -- the firmware's own -- and greys nothing (ubootmode_outcome()).

#define UBOOTMODE_KEPT   "toy_mode"
#define UBOOTMODE_TRY    "toy_mode_try"
#define UBOOTMODE_TRIED  "toy_mode_tried"
#define UBOOTMODE_CONF   "/etc/display.conf"
#define UBOOTMODE_REFUSED_KEY "firmware_refused"         // a BIOS/CSM boot's
#define UBOOTMODE_REFUSED_UEFI_KEY "firmware_refused_uefi" // a UEFI boot's
#define UBOOTMODE_SIZE   16      // "3840x2160" and its terminator, with room
// What GRUB falls back to when the size asked for is not offered: the
// stanza's second choice (boot/grub.cfg, and ubootcfg.c's VIDEO_LINE for
// an older file) and the kernel's multiboot2 framebuffer request
// (boot.asm). Change them together.
#define UBOOTMODE_FALLBACK "1280x720"

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

// What this boot's trial came to, from the size the screen came up at:
// SET at the size tried; REFUSED at UBOOTMODE_FALLBACK, so the firmware
// does not offer it; NOT_SET at anything else, the firmware's own mode,
// so the boot loader set none (an old GRUB on the X99, 2026-11-23). A
// refusal GRUB's `auto` turned into a third size reads as NOT_SET too --
// the safe way to be wrong, since NOT_SET greys nothing -- and a loader
// setting nothing on firmware that starts at the fallback reads REFUSED.
enum ubootmode_outcome { UBOOTMODE_SET, UBOOTMODE_REFUSED, UBOOTMODE_NOT_SET };
enum ubootmode_outcome ubootmode_outcome(const char *tried, int now_w, int now_h);

// The sizes this machine's firmware turned down, from UBOOTMODE_CONF --
// THIS BOOT'S firmware: a VESA BIOS and a GOP driver offer different
// lists, so a size refused on one is still offered on the other.
int ubootmode_refused(const char *size);
int ubootmode_refuse(const char *size);
// Forget the refusals of the BIOS boot path, the UEFI one, or both: a
// boot loader newly written there answers afresh (a refresh forgets the
// path it rewrote), and `bootcfg resolution forget` forgets both. 0, or -1.
int ubootmode_forget(int bios, int uefi);

#ifdef __cplusplus
}
#endif

#endif
