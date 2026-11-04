#ifndef ULIB_UNAVHIST_H
#define ULIB_UNAVHIST_H

// unavhist -- BACK AND FORWARD through the folders a view has shown: the
// File Manager's panes and the file chooser.
//
// THE BROWSER'S SHAPE, because everyone expects it: going somewhere new
// TRUNCATES the forward list, so it is a line and never a tree. The same
// place twice in a row is not a step. A BOUNDED RING, oldest dropped:
// "where was I", not an audit trail.
//
// It only remembers. Moving the view, and not recording the move a step
// itself caused, is the caller's -- each caller's view is its own.

#define UNAVHIST_MAX  32
#define UNAVHIST_PATH 256

struct unavhist {
    char dir[UNAVHIST_MAX][UNAVHIST_PATH];
    int count;   // entries in use
    int at;      // where we are, 0..count-1; -1 before the first record
};

void unavhist_init(struct unavhist *h);
void unavhist_record(struct unavhist *h, const char *dir);
// Is there anywhere that way? `delta` is -1 (back) or +1 (forward).
int  unavhist_can(const struct unavhist *h, int delta);

// One step `delta`, to the nearest entry `ok` accepts (NULL accepts all),
// which becomes the current one: its path, or NULL when there is none.
// An entry `ok` refuses -- a folder that has gone away -- is STEPPED OVER
// and kept, since it may exist again by the time Back comes past; the
// first one refused goes to `*skipped` (NULL when none was).
const char *unavhist_step(struct unavhist *h, int delta,
                          int (*ok)(void *ctx, const char *dir), void *ctx,
                          const char **skipped);

#endif
