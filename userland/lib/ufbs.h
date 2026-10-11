#ifndef ULIB_UFBS_H
#define ULIB_UFBS_H

// First-boot setup (FBS): whether it should run this boot, and the
// marker that says it has -- for the wizard (/bin/wm/system/firstboot),
// `fbs` and System Settings.
//
// THE MARKER IS A FILE, macOS's /var/db/.AppleSetupDone and GNOME's
// gnome-initial-setup-done: present means done, whatever it says. Its one
// line ("finished 2026-10-11 20:14" / "skipped ...") is for a person.
// Development images are seeded WITH it (tools/seed_disk.py --fbs-done);
// the live image and a disk made by `install` are not.
//
// THE BOOT WORDS OVERRIDE IT, systemd.firstboot='s shape: `fbs=on` runs
// setup even when marked, `fbs=off` never runs it and writes nothing.

#define UFBS_MARKER "/var/lib/fbs/done"

enum ufbs_word { UFBS_WORD_NONE, UFBS_WORD_ON, UFBS_WORD_OFF };

// The fbs= word on this boot's command line.
enum ufbs_word ufbs_boot_word(void);
// ...in `cmdline`, for the test. Whole words only: `xfbs=on` is not one.
enum ufbs_word ufbs_word_in(const char *cmdline);

// Is the marker there? Its line into `how` when it is (may be NULL).
int ufbs_done(char *how, int cap);

// Should setup run THIS boot: the word decides, else the marker.
int ufbs_needed(void);

// Write the marker: `how` is "finished" or "skipped". 0, or -1.
int ufbs_mark(const char *how);

// Remove it, so the next boot runs setup. 0 (also when it was absent), or -1.
int ufbs_rearm(void);

#endif
