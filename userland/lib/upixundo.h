#ifndef ULIB_UPIXUNDO_H
#define ULIB_UPIXUNDO_H

#include <stddef.h>
#include <stdint.h>
#include "lib/uimg.h"
#include "lib/upixel.h"

// upixundo -- UNDO FOR A PICTURE BEING PAINTED: Paint.NET's history
// shape. Each step keeps only the RECTANGLE a change touched, before and
// after, so a dab of the brush on a 4000x3000 photo costs the dab and
// not 48 MB. A change that resizes the picture (a rotate, a crop) keeps
// the whole of both.
//
// BEGIN, CHANGE, COMMIT. begin() copies the picture aside; the caller
// paints, growing a dirty rect (lib/upixel.h); commit() cuts that rect
// out of the copy and the picture. The copy is one buffer kept between
// steps, so a stroke costs one memcpy of the picture at its start and
// nothing per motion event.
//
// BOUNDED BY BYTES, oldest dropped -- a count would be wrong both ways,
// a thousand dabs being small and ten full-picture steps large. The
// newest step is always kept, whatever it costs.
//
// A STEP KNOWS ITS PICTURE: undo puts the pixels back into the image the
// step was committed on, so one history can span several (a layered
// picture's layers). Steps that are not pixels -- a layer added, moved,
// merged -- are the caller's own records, pushed with upixundo_push() and
// undone through their ops, in the same line as the strokes (Paint.NET's
// one History). The caller keeps every image a step names alive until
// that step is dropped; a custom step's `drop` is where it hears so.

struct upixundo_custom {
    void (*apply)(void *payload, int redo);   // undo it (redo 0) or do it again
    void (*drop)(void *payload);              // the step is forgotten: free what it holds
};

struct upixundo_step {
    struct uimg *im;         // the picture it was committed on; NULL for a custom step
    const struct upixundo_custom *custom;
    void *payload;
    size_t custom_bytes;
    int x, y, w, h;          // the region; the whole picture when `whole`
    int whole;
    int bw, bh, aw, ah;      // a whole step's sizes before and after
    uint32_t *before, *after;
    unsigned id;             // what upixundo_state() reports at this step
    int joined;              // undone and redone with the step before it
};

struct upixundo {
    struct upixundo_step *steps;
    int count, at, cap;      // `at` steps are done; the rest can be redone
    size_t bytes, limit;
    unsigned next_id;
    struct uimg snap;        // begin()'s copy
    int snapped;
    int join_next;           // upixundo_join(): the next step joins the last
};

void upixundo_init(struct upixundo *u, size_t limit_bytes);
void upixundo_clear(struct upixundo *u);   // forgets every step (a new picture)
void upixundo_free(struct upixundo *u);

// Before a change: the picture as it is. 0, or -ENOMEM (the change then
// cannot be undone, and commit() records nothing).
int upixundo_begin(struct upixundo *u, const struct uimg *im);
// The change begun was taken back by its caller: nothing to commit.
void upixundo_cancel(struct upixundo *u);
// After it: the `dirty` part of the change, or -- when the picture's
// size changed, or `dirty` is NULL -- the whole picture. An empty rect
// records nothing. Either way the redo list is gone. 0 or -ENOMEM.
int upixundo_commit(struct upixundo *u, struct uimg *im, const struct upix_rect *dirty);
// A change already made that is not pixels, as one step costing `bytes`
// against the limit. The redo list is gone. On -ENOMEM the step is not
// kept and `drop` has already been called.
int upixundo_push(struct upixundo *u, const struct upixundo_custom *ops, void *payload, size_t bytes);
// ONE CHANGE MADE OF TWO STEPS -- the canvas grown and a picture pasted
// into it, a layer added and filled: the next step committed or pushed
// is undone and redone together with the one before it, as one Ctrl+Z.
void upixundo_join(struct upixundo *u);

int upixundo_can_undo(const struct upixundo *u);
int upixundo_can_redo(const struct upixundo *u);
// One step back or forward, applied to the picture it was committed on
// (whose pixels a whole step reallocates), or through its custom ops.
// `*changed` is what to redraw; its w 0 means all of it -- a size change
// or a custom step. 1 if a step was applied.
int upixundo_undo(struct upixundo *u, struct upix_rect *changed);
int upixundo_redo(struct upixundo *u, struct upix_rect *changed);

// Where the history stands, as a number that changes with every step
// done or undone and comes back when a step is redone -- compare it with
// the one taken at a save to know whether the picture is unsaved. 0 is
// "no step done".
unsigned upixundo_state(const struct upixundo *u);

#endif
