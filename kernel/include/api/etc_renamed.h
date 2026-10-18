#ifndef ETC_RENAMED_H
#define ETC_RENAMED_H

#include <stdint.h>

// THE SETTINGS RENAMED TO DOTTED NAMES (2026-10-18): `desktop.
// transparency_taskbar` became `desktop.transparency.taskbar`, a branch
// `config get desktop.transparency` can list (docs/settings-and-
// queries.md, "Names"). Both rings compile this table, so every reader
// of /etc agrees on it without a case of its own:
//
//   - a READ of a key that is absent falls back to the key it was
//     (etc_config.c), so a file written before the rename still says
//     what it said;
//   - a WRITE replaces the old key's line in place (etc_config.c), so a
//     file migrates the first time each setting is saved;
//   - a LOOKUP by the old name finds the new one (setting.c,
//     usetting_schema.c, /bin/config), so a script that sets the old
//     name keeps working.
//
// An entry stays until no machine can have a file from before it.
struct etc_renamed {
    const char *ns;    // the namespace -- the file the key lives in
    const char *was;   // the key before the rename, "transparency_taskbar"
    const char *now;   // the key after it, "transparency.taskbar"
};

extern const struct etc_renamed etc_renamed[];
extern const int etc_renamed_count;

// The key `key` was called before the rename, or NULL. Keys are matched
// without their namespace: a dotted key exists only since the rename, so
// one cannot mean two things.
const char *etc_renamed_was(const char *key);

// The name an OLD name -- qualified "desktop.transparency_taskbar", or
// bare "transparency_taskbar" -- has now, in the form it was given.
// 1 with `out` filled, 0 when it was never renamed (or does not fit).
int etc_renamed_now(const char *name, char *out, uint32_t cap);

#endif
