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
// A LIVE SESSION NEVER RUNS IT: the session forgets everything at power
// off, and the installer (lib/uinstall.h) is what that boot is for.
//
// THE BOOT WORDS OVERRIDE IT, systemd.firstboot='s shape: `fbs=on` runs
// setup even when marked, `fbs=off` never runs it and writes nothing.

#define UFBS_MARKER "/var/lib/fbs/done"

// The values ubootcfg_onoff_in() answers.
enum ufbs_word { UFBS_WORD_NONE, UFBS_WORD_ON, UFBS_WORD_OFF };

// The fbs= word on this boot's command line.
enum ufbs_word ufbs_boot_word(void);
// ...in `cmdline`, for the test. Whole words only: `xfbs=on` is not one.
enum ufbs_word ufbs_word_in(const char *cmdline);

// Is the marker there? Its line into `how` when it is (may be NULL).
int ufbs_done(char *how, int cap);

// Should setup run THIS boot: the word decides, else a live session
// says no, else the marker.
int ufbs_needed(void);

// Write the marker: `how` is "finished" or "skipped". 0, or -1.
int ufbs_mark(const char *how);

// Remove it, and any answered list, so the next boot runs all of setup.
// 0 (also when neither was there), or -1.
int ufbs_rearm(void);

// --- pages already answered -------------------------------------------
//
// AN INSTALL CARRIES THE LIVE SESSION'S CHOICES, and setup on the new
// machine asks only what is left -- GNOME Initial Setup skipping what
// the installer asked. `install` copies /etc, so the settings travel by
// themselves; what it adds is UFBS_ANSWERED on the target, one page id
// per line, naming each page one of whose settings the session STORED
// (set by someone, rather than left at its built-in default). The wizard
// skips those pages, and the file goes when setup ends.
//
// WRITTEN BY THE INSTALL, NEVER WORKED OUT AT BOOT: a machine somebody
// has used has stored settings everywhere, and `fbs again` there must
// still ask everything.

#define UFBS_ANSWERED "/var/lib/fbs/answered"

enum ufbs_page {
    UFBS_PAGE_REGION, UFBS_PAGE_KEYBOARD, UFBS_PAGE_TIME, UFBS_PAGE_LOOK,
    UFBS_PAGE_NAME, UFBS_PAGE_NET, UFBS_PAGES,
};

// "region", "keyboard", ... -- the file's words.
const char *ufbs_page_id(int page);
// "Region & formats", "Keyboard", ... -- what a person reads.
const char *ufbs_page_title(int page);
// Is one of `page`'s settings stored on THIS machine?
int ufbs_page_stored(int page);
// The pages stored here, a bit per page.
unsigned ufbs_stored(void);
// Write `root`UFBS_ANSWERED naming the pages in `pages` (nothing when it
// is 0). 0, or -1.
int ufbs_write_answered(const char *root, unsigned pages);
// The pages UFBS_ANSWERED names, a bit per page; 0 when there is none.
unsigned ufbs_answered(void);
// Remove UFBS_ANSWERED. 0, or -1.
int ufbs_clear_answered(void);

#endif
