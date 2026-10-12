#ifndef ULIB_ICONV_H
#define ULIB_ICONV_H

// POSIX's character-set conversion (userland/libc/iconv.c).
//
// **THE SET IS SMALL ON PURPOSE, AND iconv_open() SAYS SO.** UTF-8,
// US-ASCII, ISO-8859-1, Windows-1252, UTF-16 and UTF-32 (each with its
// LE/BE forms; UCS-4 and WCHAR_T as UTF-32's names). Anything else fails
// at iconv_open() with EINVAL -- which is the moment a caller can choose
// a fallback (NetSurf's form submission tries the page's charset, then
// ISO-8859-1), rather than half-way through a conversion. A charset the
// web still uses (Shift_JIS, GBK, KOI8-R) is decoded on the way IN by
// libparserutils' own codecs, not by this.
//
// A name matches ignoring case, '-' and '_', so "utf8", "UTF-8" and
// "Utf_8" are one. A TARGET may carry POSIX-ish suffixes:
//   //TRANSLIT  a character the target cannot hold becomes '?', counted
//               in the return value as an irreversible conversion
//   //IGNORE    it is dropped, and counted the same way
// Without either, it is EILSEQ, and *inbuf is left pointing at it.
//
// "UTF-16" and "UTF-32" read a byte-order mark if one starts the input
// (big-endian when none), and WRITE one, big-endian, at the start of
// the output -- RFC 2781's default. The LE/BE names neither read nor
// write a mark.
#include <stddef.h>

typedef struct __iconv *iconv_t;

// (iconv_t)-1 with EINVAL for a pair this library cannot convert,
// ENOMEM if the descriptor could not be allocated.
iconv_t iconv_open(const char *tocode, const char *fromcode);

// Converts as much of *inbuf as fits in *outbuf, advancing both and
// decrementing both counts. Returns the number of IRREVERSIBLE
// conversions (//TRANSLIT, //IGNORE), or (size_t)-1 with errno:
//   EILSEQ  bytes that are not a character of the source charset, or a
//           character the target cannot hold (no suffix) -- *inbuf at it
//   EINVAL  the input ends partway through a character -- *inbuf at its
//           first byte, so the caller can append more and call again
//   E2BIG   *outbuf is full -- call again with more room
// A NULL `inbuf` (or *inbuf) resets the shift state: the next output
// starts afresh, byte-order mark and all.
size_t iconv(iconv_t cd, char **inbuf, size_t *inbytesleft,
             char **outbuf, size_t *outbytesleft);

int iconv_close(iconv_t cd);

#endif
