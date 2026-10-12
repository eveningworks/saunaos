#ifndef UTHEME_FILE_H
#define UTHEME_FILE_H

// Theme FILES: reading, writing and applying the `.theme` files a
// `desktop.theme` setting names (docs/theme-design.md).
//
// Shipped themes live in UTHEME_SHIPPED_DIR, themes made on this
// machine in UTHEME_LOCAL_DIR, and a local file WINS over a shipped one
// of the same stem -- /etc over /usr, as everywhere. The format is
// /etc's `name=value` (etc_config.h): the colour roles at top level,
// the frame's style in `[frame]`, the settings it carries in `[apply]`.

#include <stddef.h>
#include <stdint.h>
#include "ui/utheme.h"
#include "setting_abi.h"

#define UTHEME_SHIPPED_DIR "/usr/share/themes"
#define UTHEME_LOCAL_DIR   "/etc/themes"
#define UTHEME_STEM_MAX    32

// Every colour role a file may set, by its key: the parser's table, the
// writer's and the Theme Editor's list. `group` is the editor's heading.
struct utheme_role {
    const char *key;
    const char *label;
    const char *group;
    uint16_t offset;   // into struct utheme, a uint32_t
};
extern const struct utheme_role utheme_roles[];
extern const int utheme_role_count;
uint32_t *utheme_role_ptr(struct utheme *t, int role);

// The settings a theme CARRIES, applied with it -- Windows' `.theme`
// file writing the wallpaper and the cursors. Applied in this order
// (the wallpaper's type before the wallpaper). An empty value is "this
// theme leaves it alone".
#define UTHEME_BUNDLE_COUNT 8
extern const char *const utheme_bundle_names[UTHEME_BUNDLE_COUNT];
struct utheme_bundle {
    char value[UTHEME_BUNDLE_COUNT][SETTING_ABI_VALUE_MAX];
};

// "RRGGBB" and nothing else -- a REJECTION, not a partial parse. No
// '#': in an /etc file that starts a comment (etc_config.h), so
// "#2B2B30" reads as an empty value -- the terminal's .scheme files
// write their colours bare for the same reason.
int utheme_parse_colour(const char *s, uint32_t *out);

// A stem a file may be saved under: 1..UTHEME_STEM_MAX-1 of [a-z0-9-],
// so it is a file name and a setting value at once.
int utheme_stem_ok(const char *stem);

// The file `stem` loads from: the local one if it exists, else the
// shipped one. 0 when neither exists.
int utheme_path(const char *stem, char *out, size_t cap);
int utheme_is_shipped(const char *stem);

// Loads `stem` over the default palette into `out`, and its bundle into
// `b` (nullable). 1 when the file was found; a role whose value does not
// parse keeps the default's and is counted in `*rejected` (nullable).
int utheme_load(const char *stem, struct utheme *out, struct utheme_bundle *b);
int utheme_load_path(const char *path, struct utheme *out, struct utheme_bundle *b,
                     int *rejected);

// Writes a complete file -- every role, the frame, the non-empty bundle
// entries. 1 on success; a short write is a failure.
int utheme_save(const char *path, const struct utheme *t, const struct utheme_bundle *b,
                const char *description);

// What a person picking `stem` gets: its bundle written through the
// registry (a refused value is skipped), then `desktop.theme`. Returns
// how many bundled settings were applied, or -1 when `stem` loads
// nothing.
int utheme_apply(const char *stem);

#endif
