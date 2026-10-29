#ifndef ETC_CONFIG_H
#define ETC_CONFIG_H

#include <stdint.h>
#include "setting_abi.h" // enum setting_result -- see the note at the bottom

// A small shared name=value config-file reader/writer for anything
// under /etc -- see kernel/lib/etc_config.c's top comment for the
// exact file format, and CLAUDE.md's `/etc` bullet for the convention
// this is part of. Replaces the hand-rolled single-purpose parsers
// tz.c and font_config.c each used to have (see the git history).
//
// This is deliberately still just a reader/writer, not a schema or a
// registry of known keys/files -- callers own their own key names and
// validate values they get back themselves (tz_find_by_name()'s
// lookup, gfx_set_font_size()'s range check, etc).
//
// ---- sections --------------------------------------------------------
//
// A `[name]` line opens a section; every key after it belongs to that
// section until the next header or the end of the file. A key before
// any header is at TOP LEVEL, which is what every file written before
// sections existed is made of -- so the four unsuffixed calls below
// mean "top level" and no existing file or call site changed meaning.
//
// The section is an ARGUMENT (`_in`), not a prefix baked into the key:
// identity here is (section, key), the shape GKeyFile, systemd units
// and NetworkManager keyfiles all use. A flattened "section.key" would
// have collided with the dotted key names /etc/settings.d already uses
// (`Choice.losangeles`), and the collision would be invisible at the
// call site. See docs/decisions/storage.md.
//
// Sections earn their place where the NAME IS DATA -- a section per
// network card, where the code does not know the names and asks the
// file (etc_config_section_count/_name). Where the name is a fixed
// schema word the payoff is compatibility: a `.desktop` file written
// here carries no header and one copied off a Linux box carries
// `[Desktop Entry]`, and etc_config_buf_get_in_or_top() reads both.

// Looks up `key` in the config file at `path`. Copies the value into
// `out` (up to out_size - 1 chars, always NUL-terminated; a value
// longer than that is truncated, same as font_config.c's old 16-byte
// limit did). Returns 1 if the file exists and `key` was found in it,
// 0 otherwise (missing file, missing key, or out_size == 0) -- `out`
// is left as an empty string in the 0 case so callers can use it
// without a separate check.
int etc_config_get(const char *path, const char *key, char *out, uint32_t out_size);

// The same, scoped to `[section]`. A NULL or empty `section` means top
// level -- so etc_config_get() is this with NULL, and always was.
// A section that is not in the file is a miss, not an error.
int etc_config_get_in(const char *path, const char *section, const char *key,
                      char *out, uint32_t out_size);

// ---- reading several keys out of ONE file --------------------------
//
// etc_config_get() loads the whole file on EVERY call (from memory in
// the kernel while it is unchanged, a disk read otherwise), which is the
// right shape for the one-key callers it was written for (tz.c asking
// for `timezone`) and quietly quadratic for anything asking a file
// several questions. The desktop was the case that mattered: each
// `.desktop` entry is asked for six keys, so a nine-entry reload issued
// 54 whole-file reads. That measured 40ms under TCG and 2.5 SECONDS
// under KVM, where every port-I/O instruction is a VM exit -- and it
// ran on every filesystem change, so saving an unrelated setting froze
// the desktop.
//
// So: load once, ask many.
//
// ---- the document, at its own size --------------------------------
//
// THE LOAD ALLOCATES `data` AT THE FILE'S SIZE, and the caller releases
// it with etc_config_buf_free(). It was a 4 KiB array inside the struct,
// so a longer file was refused whole and every holder carried 4 KiB
// whether the file was 40 bytes or not. Linux's kernel_read_file() has
// this shape: allocate at i_size, under a ceiling the caller names.
//
// The parser is compiled into both rings with no C library on its
// include path, so it can name neither kmalloc nor malloc: the ALLOCATOR
// IS PASSED IN, as klineedit takes a `struct kline_mem`. Each ring's
// loader passes its own (the kernel's etc_config_kmem, ring 3's
// uconf_mem), and the buffer remembers it, so etc_config_buf_free()
// needs no argument but the buffer.
struct etc_config_mem {
    void *(*alloc)(unsigned long bytes);
    void  (*free)(void *p);
};

// The SANITY BOUND, not a buffer: a "config file" past this is not one,
// and is refused rather than allocated (the same refusal, for the same
// reason, as a read that cannot be completed). tools/check_config_size.py
// reads it from here for the shipped files.
#define ETC_CONFIG_FILE_MAX (1024u * 1024u)

// A LOAD ALWAYS INITIALISES THE STRUCT FIRST, success or not, and never
// looks at what was there -- so a buffer used for a second load must be
// freed before it, or the first document leaks. etc_config_buf_free()
// is safe after any load, failed or not, and twice; on a struct nothing
// has loaded into, only when it was zeroed (`= {0}`, or a static).
struct etc_config_buf {
    char *data;        // size bytes and a NUL; NULL unless valid
    uint32_t size;
    int valid;
    const struct etc_config_mem *mem;   // what `data` came from
};

// Reads `path` into `buf`, allocated at the file's size. Returns 1 if
// the whole file was read. An unchanged file is copied from memory
// (etc_config_file.c), so the per-key etc_config_get() costs a copy and
// a parse, not a read. A file that cannot be read WHOLE -- over
// ETC_CONFIG_FILE_MAX, out of memory, or grown past what was allocated
// while it was read -- is a REFUSAL (0, buf invalid), not a truncation:
// half a config file parses as a valid config file whose later keys
// have silently vanished, which is the failure mode this project's
// formatter/parser conventions exist to avoid.
int etc_config_load(const char *path, struct etc_config_buf *buf);

// Releases a loaded document and leaves `buf` invalid. See the struct.
void etc_config_buf_free(struct etc_config_buf *buf);

// Fills `buf` with a COPY of `len` bytes of `doc`, allocated from `mem`
// at that size. 1 on success; 0 (buf invalid) when the copy cannot be
// allocated. What both rings' load caches hand a hit out through, and
// what the shared cases load a document with.
int etc_config_buf_copy(struct etc_config_buf *buf, const char *doc, uint32_t len,
                        const struct etc_config_mem *mem);

// The kernel's allocator for the above (etc_config_file.c).
extern const struct etc_config_mem etc_config_kmem;

// Loads answered from memory since boot; for the KTESTs.
uint64_t etc_config_cache_hits(void);

// The kernel load cache's TOTAL-BYTES cap: it keeps up to eight
// documents, each at its own size, and evicts the least recently used
// until a new one fits. The first time a document cannot be kept
// because of the cap it LOGS, once a boot. For the KTESTs: the cap can
// be lowered (returns the old one), and both events are counted.
uint32_t etc_config_cache_set_cap(uint32_t bytes);
uint64_t etc_config_cache_cap_hits(void);
uint32_t etc_config_cache_cap_logs(void);

// Rewrites `in` with `key` set to `value`, or `key` REMOVED when
// `value` is NULL. Returns the new length, or 0 if it would not fit --
// or, for a removal, if the key was not there, so a caller can tell
// "nothing to do" from "done" and leave the file untouched.
//
// Buffer to buffer and freestanding, which is the point: the ring-3 WM
// rewrites /etc files through this with libsys doing the I/O, so there
// is exactly one implementation of what a `name=value` document means.
// See kernel/lib/etc_config.c on the split.
//
// `out` is the CALLER's and must be etc_config_buf_set_room() bytes to
// be sure of fitting; etc_config_buf_set_alloc() below sizes it itself.
uint32_t etc_config_buf_set(const char *in, uint32_t in_len,
                            const char *key, const char *value,
                            char *out, uint32_t out_cap);

// The same, scoped to `[section]` (NULL or "" is top level).
//
// WHERE A NEW KEY LANDS, which is the whole of the writer's design: at
// the END OF ITS OWN SECTION -- after that section's last `key=value`
// line, before any trailing comment block, and before the next header.
// Never at the end of the file, which would silently file the key under
// whatever section happens to be last. A section that is not there yet
// is appended with its header; a top-level key in a sectioned file goes
// above the first header.
//
// A trailing comment block is skipped because in every INI-shaped
// format a comment sitting immediately above a header belongs to the
// section BELOW it.
//
// Removing the last key of a section leaves the header, and its
// comments, in place: dropping them would delete what somebody wrote
// in order to tidy up what the machine wrote.
uint32_t etc_config_buf_set_in(const char *in, uint32_t in_len,
                               const char *section,
                               const char *key, const char *value,
                               char *out, uint32_t out_cap);

// The most a rewrite of `in_len` bytes can produce, NUL included: the
// document, a newline its last line may lack, the key's own line, and a
// new section's header with the blank line above it. 0 if that does not
// fit a uint32_t.
uint32_t etc_config_buf_set_room(uint32_t in_len, const char *section,
                                 const char *key, const char *value);

// etc_config_buf_set_in() into a buffer allocated from `mem` at
// etc_config_buf_set_room(). Returns it NUL-terminated, with its length
// in *out_len, or NULL for a refusal -- the key not there to remove, a
// section that could not be read back, no memory. UNLIKE the sized
// call, an EMPTY result (the file's only key removed) is a buffer of
// length 0, not a refusal. Free it with mem->free.
char *etc_config_buf_set_alloc(const char *in, uint32_t in_len,
                               const char *section,
                               const char *key, const char *value,
                               const struct etc_config_mem *mem,
                               uint32_t *out_len);

// Same contract as etc_config_get(), answered from an already-loaded
// buffer instead of from disk. Returns 0 with `out` emptied if the
// buffer is invalid, so a failed load needs no separate check at each
// call site.
int etc_config_buf_get(const struct etc_config_buf *buf, const char *key,
                       char *out, uint32_t out_size);

// The same, scoped to `[section]`; NULL or "" is top level.
int etc_config_buf_get_in(const struct etc_config_buf *buf, const char *section,
                          const char *key, char *out, uint32_t out_size);

// `[section]` first, then TOP LEVEL -- for a document whose header is
// OPTIONAL. That is the freedesktop shape: a `.desktop` entry written
// here has no header and one copied off a Linux box opens with
// `[Desktop Entry]`, and both must read. Prefer the plain `_in` where
// the header is required; this one cannot tell a file that omits the
// header from a file that misspells it.
int etc_config_buf_get_in_or_top(const struct etc_config_buf *buf,
                                 const char *section, const char *key,
                                 char *out, uint32_t out_size);

// ---- walking the sections a file DECLARES ---------------------------
//
// The half a section buys that a dotted key name does not: the code
// asks the FILE which sections exist instead of knowing their names.
// /etc/net.conf is the case -- one section per card, named by a MAC
// address nothing in the source can predict.
//
// Names are DISTINCT and in first-appearance order: a name that opens
// twice is one section (both runs are searched for a key), so a walk
// cannot hand the same card back twice.

// A section name longer than this is not a section -- the line is
// ignored, exactly as a malformed one is. A cap rather than a
// truncation is what lets a walk hand every name to `_get_in()` and
// know the lookup means the same section it was given.
#define ETC_CONFIG_SECTION_MAX 64

int etc_config_section_count(const struct etc_config_buf *buf);

// The `index`-th distinct section name. Returns 1 and NUL-terminates
// `out`, or 0 for a bad index or a buffer under ETC_CONFIG_SECTION_MAX
// -- never a truncated name, which would name a different section.
int etc_config_section_name(const struct etc_config_buf *buf, int index,
                            char *out, uint32_t out_size);

// Sets `key=value` in the config file at `path`, creating the file if
// it doesn't exist yet. If `key` is already present, its line is
// replaced in place (every other line -- including comments -- keeps
// its position); otherwise a new "key=value" line is appended.
// Returns 1 on success, 0 if the file could not be read whole (see
// etc_config_load()), the rewrite could not be allocated, or the
// underlying fs_write() failed.
int etc_config_set(const char *path, const char *key, const char *value);

// Removes `key` from the config file at `path`, keeping every other
// line -- comments included -- exactly where it was. Returns 1 if the
// key was there and the rewrite landed, 0 otherwise.
//
// "The key was absent" and "the write failed" are both 0 on purpose:
// neither entitles a caller to say it removed anything, and an absent
// key leaves the file untouched rather than rewritten identically.
//
// This is what makes a setting REVERTIBLE to its built-in default,
// which deleting the file wholesale could not do without taking every
// other setting in it along.
int etc_config_unset(const char *path, const char *key);

// The section-scoped pair. Same contracts, `[section]` scope; NULL or
// "" is top level, which is what the two above pass.
int etc_config_set_in(const char *path, const char *section,
                      const char *key, const char *value);
int etc_config_unset_in(const char *path, const char *section, const char *key);

// `enum setting_result` -- what a "change a setting and persist it"
// call actually managed to do -- now lives in abi/setting_abi.h, which
// this header includes, so every existing `#include "etc_config.h"`
// still sees it.
//
// It moved because SYS_SETTING let a RING-3 program change a setting:
// the three-way outcome stopped being an internal detail and became
// part of the kernel<->userland contract. The alternative was a second
// copy of the enum in the ABI header, which could drift from this one
// -- and a drift there would mean a client reporting "saved" for a
// value that was not.

#endif
