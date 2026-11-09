#ifndef ULIB_UCOMPONENT_H
#define ULIB_UCOMPONENT_H

// The OPTIONAL COMPONENTS of this machine (docs/software-design.md): what
// each one is, whether it is installed, whose a file is, and removing one.
// Shared by /bin/component, the Software app, /bin/update (which skips a
// removed one) and the GPL switch's readers -- toywm's Start menu and
// desktop, and uappentry_each().
//
// A component is /usr/share/components/<id>.component (data/components/
// README.md): Name, Summary, License, Needs, Paths and the Size/Files the
// build summed. THE DEFINITIONS ARE THE BASE SYSTEM'S, so a removed
// component is still listed. A file belongs to the component with the
// LONGEST Paths= entry covering it (a file path, or a directory ending
// '/'), as tools/lib/components.py decides on the host.
//
// THE MACHINE'S STATE IS A DIFFERENCE LIST, /var/lib/update/components:
// `removed <id>` per line (and `added <id>` for a component a later build
// ships off by default). Not an installed list -- a component a new build
// introduces arrives by default, as a new Windows feature does, and one
// removed here stays removed across every update (rpm-ostree's override).
//
// Adding one back FETCHES, so it is upd_component_add() in
// userland/common/upd.h, beside the update it shares a manifest with.

#include <stdint.h>

#define UCOMPONENT_DIR    "/usr/share/components"
#define UCOMPONENT_STATE  "/var/lib/update/components"
#define UCOMPONENT_ID_MAX 24              // 23 bytes and the NUL: components.py's ID
#define UCOMPONENT_GPL_SETTING "software.show_gpl"

struct ucomponent {
    char id[UCOMPONENT_ID_MAX];
    // Each a NUL-terminated string in `mem`, "" when the key is absent.
    char *name, *summary, *project, *category, *license, *icon;
    char *needs;        // space-separated ids
    char *paths;        // space-separated, a directory ending '/'
    uint64_t size;      // Size=: the build's bytes
    int files;          // Files=
    int removed;        // the state file says `removed <id>`
    char *mem;
};

struct ucomponent_set {
    struct ucomponent *c;
    int count, cap;
};

// Every definition in UCOMPONENT_DIR, sorted by Name, with the state
// file's `removed` marks. 0, or -1 when the directory cannot be listed
// (none of this build's, an empty set). Free with ucomponent_free(); a
// set loaded twice is freed first by the second load.
int  ucomponent_load(struct ucomponent_set *s);
void ucomponent_free(struct ucomponent_set *s);

int ucomponent_find(const struct ucomponent_set *s, const char *id);   // index, or -1
// The component holding `path`, by the longest covering Paths= entry;
// -1 for the base system.
int ucomponent_owner(const struct ucomponent_set *s, const char *path);
// Does the component's space-separated `needs` name `id`?
int ucomponent_needs(const struct ucomponent *c, const char *id);

// GPL or AGPL anywhere in License= -- never LGPL, a library licence a
// closed program may link (the Makefile's GPL_ONLY_PORTS rule).
int ucomponent_is_gpl(const struct ucomponent *c);

// ---- the state file -------------------------------------------------------------

// Is `id` marked removed? (One read of the file.)
int ucomponent_state_removed(const char *id);
// Mark `id` removed (1) or not (0); written whole, through a rename.
// 0, or -1 when the file could not be written.
int ucomponent_state_set(const char *id, int removed);
// Every `removed` id, for update's skip: calls `fn` once per id.
int ucomponent_state_each(void (*fn)(const char *id, void *ctx), void *ctx);

// ---- removing ---------------------------------------------------------------------

struct ucomponent_progress {
    void *ctx;
    void (*log)(void *ctx, const char *line);      // one line, no newline
    void (*file)(void *ctx, int done, int total);  // after each file
};

// Delete the files of component `idx` on this machine -- each file its
// Paths= cover that no LONGER path gives to another component, then the
// directories left empty -- and mark it removed. OFFLINE: the definition
// says what. A file under /lib waits for the next boot in update's
// pending list (abi/update_abi.h), as an update's would, since a running
// process may page it in by path; `*at_boot` (may be NULL) says so.
// It removes ONLY `idx`: the caller decides what needs it (the
// command's and the app's closure). 0, or -1 with `why` set.
int ucomponent_remove(const struct ucomponent_set *s, int idx, const struct ucomponent_progress *pr,
                      int *at_boot, char *why, int whycap);

// The installed components that need `idx`, into `out` (at most `cap`);
// how many. Removing `idx` removes these too.
int ucomponent_dependents(const struct ucomponent_set *s, int idx, int *out, int cap);

// THE CLOSURES, in the order they run -- one level of Needs=, checked
// acyclic at build time. Removing `idx`: the installed components that
// need it, then `idx`. Adding it: the removed components it needs, then
// `idx`. Into `out` (at most `cap`); how many.
int ucomponent_remove_closure(const struct ucomponent_set *s, int idx, int *out, int cap);
int ucomponent_add_closure(const struct ucomponent_set *s, int idx, int *out, int cap);

// ---- the GPL switch ---------------------------------------------------------------

// The setting UCOMPONENT_GPL_SETTING: 1 (the default) shows GPL-licensed
// components' apps, 0 hides them from the Start menu and the desktop.
// A VIEW: the files stay, and the command line still runs them.
int ucomponent_show_gpl(void);

// What a list of applications hides, loaded once per walk: the
// definitions and the setting. `hides(path)` is 1 for a file -- a desktop
// entry, or the program it runs -- in a GPL component while the switch
// is off. A zeroed view hides nothing; free it with ucomponent_view_free.
struct ucomponent_view {
    struct ucomponent_set set;
    int hiding;        // the switch is off and some component is GPL
};
void ucomponent_view_load(struct ucomponent_view *v);
int  ucomponent_view_hides(const struct ucomponent_view *v, const char *path);
void ucomponent_view_free(struct ucomponent_view *v);

// The Category= key as a person reads it ("devtools" -> "Developer tools").
const char *ucomponent_category_label(const char *key);

#endif
