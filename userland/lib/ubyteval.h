#ifndef ULIB_UBYTEVAL_H
#define ULIB_UBYTEVAL_H

// THE BYTES AT AN OFFSET, READ AS VALUES -- a hex viewer's data
// inspector (HxD's, ImHex's, 010 Editor's): binary, signed and unsigned
// integers of each width, floats, a UTF-8 character, the value as an
// offset into the file -- plus the text forms a selection is copied as
// and the offsets and byte patterns a person types. Pure code.
//
// **A VALUE THAT NEEDS MORE BYTES THAN REMAIN IS UNAVAILABLE**, never
// read past the end and never padded: the row says so instead.
#include <stddef.h>
#include <stdint.h>

enum ubyteval_row {
    UBV_BINARY, UBV_INT8, UBV_INT16, UBV_INT32, UBV_INT64, UBV_HEX64,
    UBV_FLOAT32, UBV_FLOAT64, UBV_UTF8, UBV_ROWS
};

// The row's name ("Int32") and how many bytes it reads.
const char *ubyteval_name(int row);
int         ubyteval_width(int row);
// Row `row` of the `avail` bytes at `b` (up to 8 are looked at), in
// `out`: 1 when shown; 0 when it needs more bytes than remain, with
// `out` saying so. An integer is shown signed, and unsigned beside it
// when the two differ ("-16 (unsigned 240)").
int ubyteval_row(const uint8_t *b, int avail, int big_endian, int row, char *out, size_t cap);
// The value read as an OFFSET into a file of `size` bytes: the widest of
// 64, 32 and 16 bits that lands inside it. 1 with `*out`, or 0.
int ubyteval_as_offset(const uint8_t *b, int avail, int big_endian, uint64_t size, uint64_t *out);

// --- what a person types ----------------------------------------------

// "0x2BC0", "11200", "+16", "-0x10" (relative to `cur`) as an offset in
// a file of `size` bytes. 1 with `*out`; 0 with `*why` a sentence.
int ubyteval_parse_offset(const char *s, uint64_t cur, uint64_t size, uint64_t *out, const char **why);
// "C0 2B", "c02b", "C0,2B" as bytes into `out` (up to `cap`). The count,
// or -1 for text that is not whole bytes of hex.
int ubyteval_parse_hex(const char *s, uint8_t *out, int cap);

// --- what a selection is copied as -------------------------------------

// "7F 45 4C 46": the bytes as hex, a space between. The length written
// (without the NUL), or -1 when it does not fit -- then nothing is.
int ubyteval_hex_text(const uint8_t *b, size_t n, char *out, size_t cap);
// The text column's reading: printable ASCII as itself, '.' for the rest.
int ubyteval_ascii_text(const uint8_t *b, size_t n, char *out, size_t cap);
// A C array, twelve bytes a line:
//   unsigned char data[5] = {
//       0x7f, 0x45, 0x4c, 0x46, 0x02,
//   };
int ubyteval_c_array(const uint8_t *b, size_t n, char *out, size_t cap);
// How big each of those is for `n` bytes, NUL included -- to allocate.
size_t ubyteval_hex_size(size_t n);
size_t ubyteval_c_size(size_t n);

#endif
