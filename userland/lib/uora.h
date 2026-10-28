#ifndef ULIB_UORA_H
#define ULIB_UORA_H

// uora -- OPENRASTER, the layered picture Krita, GIMP and MyPaint all
// read: a .zip (lib/uzip.h) whose FIRST member is "mimetype", STORED,
// holding "image/openraster"; a stack.xml naming the layers TOP FIRST;
// a PNG per layer; and mergedimage.png plus Thumbnails/thumbnail.png for
// a reader that cannot do layers.
//
// THE LAYERS HERE ARE BOTTOM FIRST, as a painter stacks them and as
// Paint keeps them; the file's top-first order is turned round in here.
// Every layer comes back the canvas's size: one stored smaller with an
// x/y offset (Krita writes them cropped) is placed onto a transparent
// canvas. Groups are read as the layers in them; their own opacity is
// not applied. A parser REJECTS rather than guesses.

#include "lib/uimg.h"

#define UORA_NAME_MAX 64
#define UORA_ATTRS_MAX 16

// A FOREIGN ATTRIBUTE, the spec's room for what it does not define:
// written on the layer as `saunaos:<name>="<value>"` (every reader ignores
// another program's), read back from any `saunaos:` attribute. Paint keeps
// a text layer's words and face in these.
struct uora_attr {
    char name[24];
    char *value;   // malloc'd; uora_free() frees it on a loaded layer
};

struct uora_layer {
    struct uimg img;
    char name[UORA_NAME_MAX];
    int opacity;   // 0..255
    int hidden;
    struct uora_attr attrs[UORA_ATTRS_MAX];
    int nattrs;
};

// Writes `n` layers, bottom first, each w x h, and `merged` -- the
// picture as shown -- for mergedimage.png and the thumbnail. Through
// `<path>.part` and a rename, as lib/uzip.h writes. 0, or a negative
// errno with `err` a sentence.
int uora_save(const char *path, const struct uora_layer *layers, int n, const struct uimg *merged,
              char *err, int errcap);
// Reads one: *out (malloc'd, uora_free() it), bottom first, *n layers on
// a *w x *h canvas.
int uora_load(const char *path, struct uora_layer **out, int *n, int *w, int *h, char *err, int errcap);
// Frees the layers' pixels and attribute values, and the array.
void uora_free(struct uora_layer *layers, int n);

#endif
