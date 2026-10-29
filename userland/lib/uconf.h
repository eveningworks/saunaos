#ifndef ULIB_UCONF_H
#define ULIB_UCONF_H

#include <stdint.h>
#include "etc_config.h" // the SHARED parser -- see below

// /etc access for a ring-3 program: reading and writing `name=value`
// documents.
//
// **The PARSER here is the kernel's own**, compiled a second time into
// libuapp.a (the Makefile's shared-source rule, as with geom.c and
// string.c). Only the I/O is ring-3: `etc_config.c` was split so that
// the half which merely looks at a buffer is freestanding, and the half
// that reaches for `fs.h` lives in `etc_config_file.c`.
//
// That split is the whole point. CLAUDE.md's standing rule is not to
// hand-roll a parser for a config file, and the reason is specific: two
// `name=value` implementations drift, and the drift surfaces as the
// system and `config` disagreeing about what a file says -- the shape
// of bug nobody looks for.
//
// **THIS WAS `wm_conf_*` AND MOVED HERE WHEN IT GOT A SECOND CALLER.**
// It lived in `userland/wm/` while the window manager was the only
// ring-3 thing with an /etc file of its own; the File Manager, which
// remembers each pane's directory in /etc/files.conf, is the second.
// `wm_conf_*` still exists and now delegates -- the WM's own wrapper
// also answers `wm_setting_generation()`, which is about the SETTINGS
// REGISTRY rather than about a file and stays there.
//
// The document is allocated AT THE FILE'S SIZE (etc_config.h), from
// uconf_mem below; ETC_CONFIG_FILE_MAX is the sanity bound in both
// rings, so a file too large for the kernel is too large here.

// Ring 3's allocator for the shared parser's documents: malloc/free.
extern const struct etc_config_mem uconf_mem;

// Reads a whole config file into `buf`, allocated at its size: FREE IT
// with etc_config_buf_free(), on every path, success or not (a failed
// load leaves nothing to free, and freeing it is still safe). 0 if the
// file is missing, unreadable, empty, or over ETC_CONFIG_FILE_MAX -- the
// last REFUSED rather than truncated, since a truncated config file
// parses as a valid one with entries missing. A load never frees what
// `buf` held before: a buffer reused for a second load is freed first.
//
// ANSWERED FROM A PER-PROCESS CACHE while the path's fs_generation_of()
// has not moved, so asking again costs a syscall and a copy, not a read.
// A write through a hard link to the file is NOT seen (uconf.c).
int uconf_load(const char *path, struct etc_config_buf *buf);

// The cache's total-bytes cap (UCONF_CACHE_BYTES), for the tests: set
// it (returns the old one), and see how often a document could not be
// kept because of it and whether that has been said.
uint32_t uconf_cache_set_cap(uint32_t bytes);
unsigned long long uconf_cache_cap_hits(void);
int uconf_cache_cap_said(void);

// One key from a file -- a uconf_load() and a parse per call. Prefer
// uconf_load() + etc_config_buf_get() when asking several questions.
int uconf_get(const char *path, const char *key, char *out, uint32_t out_size);

// The same, scoped to `[section]` -- NULL or "" is top level, which is
// what the two unsuffixed calls pass. See etc_config.h for the format.
int uconf_get_in(const char *path, const char *section, const char *key,
                 char *out, uint32_t out_size);

// Writes one key, rewriting the document around it. Creates the file if
// it is missing. 0 if the write failed -- including a SHORT write,
// which is a failure and not a partial success: a config file half
// rewritten parses, with the tail of the document gone.
int uconf_set(const char *path, const char *key, const char *value);

int uconf_set_in(const char *path, const char *section,
                 const char *key, const char *value);

// Removes `key` from the file, rewriting the document around it. The
// rewriter treats a NULL value as a removal, so this is uconf_set()
// with one argument -- named rather than spelled at the call sites,
// because "set it to nothing" and "take it out" are different requests
// and only one of them is what a NULL means here.
int uconf_unset(const char *path, const char *key);

#endif // ULIB_UCONF_H
