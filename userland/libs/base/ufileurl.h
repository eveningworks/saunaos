#ifndef ULIB_UFILEURL_H
#define ULIB_UFILEURL_H

// A file: URL and the local path it names (RFC 8089), and the web's name
// for what a file is (its Content-Type) -- what a browser engine and the
// app that reads files for it both have to agree on.
#include <stddef.h>

// The absolute path a file: URL names, percent-escapes decoded:
// "file:///home/a%20b.html" and "file://localhost/home/a b.html" are both
// "/home/a b.html". A query or fragment is not part of it. 0, or -1 --
// writing NOTHING -- for a URL that is not file:, names a remote host,
// decodes to an escape that is not two hex digits or a NUL, or does not
// fit `cap`.
int ufileurl_to_path(const char *url, char *out, size_t cap);

// The Content-Type a browser gives a file by its name's extension:
// "text/html" for .html, "image/png" for .png. "text/plain" for anything
// it does not know -- shown, never run.
const char *ufileurl_mime(const char *path);

#endif
