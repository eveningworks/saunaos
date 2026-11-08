#ifndef ULIB_CTYPE_H
#define ULIB_CTYPE_H

// C's <ctype.h>, ASCII only.
//
// WHY THESE ARE NOT ALL k_* WRAPPERS, unlike <string.h>'s. The toolkit
// has exactly four character predicates (k_isdigit, k_isspace,
// k_tolower, k_toupper), because those are the four the KERNEL has ever
// needed. Adding the other eight to api/string.h to wrap them here
// would put eight functions with no kernel caller into the kernel
// image, which is the opposite of the toolkit's own bar -- a second
// REAL caller, not a plausible one. So the four that exist are wrapped
// and the rest are defined here, where the C library is the caller.
//
// **ASCII ONLY, and that is not a temporary state.** There is no locale
// and there will not be one (docs/roadmap-details.md lists locales as
// deliberately not pursued), so these answer for bytes 0..127 and
// report everything above as "not a letter, not a digit, not printable
// punctuation". A UTF-8 lead byte is not alphabetic here, which is the
// honest answer for a function whose entire input is one byte.
//
// The argument is an `int` holding an `unsigned char` value or EOF, as
// C requires. A NEGATIVE argument that is not EOF is undefined in C and
// answers 0 here rather than indexing anything.
#include <kstring.h>
#ifdef __cplusplus
extern "C" {
#endif

// REAL SYMBOLS, not inlines (userland/libs/libc/ctype.c): a hosted
// compiler knows these names as builtins and calls them by itself --
// libstdc++'s std::format calls toupper() -- which a static inline in a
// header the caller never included cannot answer
// (docs/conventions/build.md).
int isdigit(int c);
int isspace(int c);
int tolower(int c);
int toupper(int c);
int islower(int c);
int isupper(int c);
int isalpha(int c);
int isalnum(int c);
int isxdigit(int c);
// The C definitions, which are about the ASCII table rather than about
// what a terminal does with the byte: printable is 0x20..0x7E, graphic
// is the same minus the space, control is everything below 0x20 plus
// DEL, punctuation is printable-and-not-alphanumeric-and-not-space.
int isprint(int c);
int isgraph(int c);
int iscntrl(int c);
int ispunct(int c);
int isblank(int c);
// XSI, and marked obsolescent there; ported code still reaches for them.
int isascii(int c);
int toascii(int c);

#ifdef __cplusplus
}
#endif

#endif
