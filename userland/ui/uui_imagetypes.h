#ifndef UUI_IMAGETYPES_H
#define UUI_IMAGETYPES_H

// uui_imagetypes -- THE PICTURE ROWS of a file chooser's "Files of type",
// written once: "All pictures (*.png; *.jpg; ...)", then one row per
// format, then "All files" -- Windows' common dialog's wording, matched
// by lib/uimg.h's uimg_format_guess() (a known extension decides, no
// extension is sniffed: KDE's rule). Any app that opens or saves
// pictures hands these to ui/uui_filedialog.h instead of writing its own.
//
// OPEN lists every format the decoders read; SAVE only those the
// encoders write, each with the `ext` that rewrites the typed name.

#include "ui/uui_filedialog.h"

extern const struct uui_filedialog_filter uui_imagetypes_open[];
extern const int uui_imagetypes_open_count;
extern const struct uui_filedialog_filter uui_imagetypes_save[];
extern const int uui_imagetypes_save_count;

// The Save row for a codec name ("png", "jpeg", "qoi"); 0 (PNG) when it
// is not one this build writes.
int uui_imagetypes_save_index(const char *format);

#endif
