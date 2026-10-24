#ifndef ULIB_UMSEXPAND_H
#define ULIB_UMSEXPAND_H

#include <stdint.h>
#include <stddef.h>

// Microsoft's COMPRESS.EXE formats undone -- what EXPAND.EXE and
// LZEXPAND.DLL did: SZDD (Windows 3.1, MS-DOS 6) and KWAJ (Windows for
// Workgroups 3.11, MS-DOS 6.2x), the `FILE.EX_` on every setup disk.
// Read for w16's Windows installer, from disks nobody here made.
//
// KWAJ methods 0 (stored), 1 (XOR 0xFF), 2 (the SZDD LZSS) and 3 (LZ +
// Huffman, what WfW 3.11's disks use throughout); method 4 (MSZIP) is
// refused, no 3.x disk carries it. The format is libmspack's account of it
// (kwajd.c, szddd.c); a file both read alike is the test
// (tools/umsexpand_hostcheck.py).
//
// uinflate.h's shape, for uinflate.h's reasons: THE INPUT IS ONE
// CONTIGUOUS BUFFER, THE OUTPUT A CALLBACK, AND IT ALLOCATES NOTHING --
// the window and the tables are the caller's `struct umsx_scratch`.
//
// A PARSER HERE REJECTS rather than guesses: a header that does not fit
// the file, a Huffman table that is over-subscribed or names a length
// past 15, a code that matches no symbol, and output that disagrees with
// the length the header records are all -EINVAL.

enum umsx_kind { UMSX_PLAIN, UMSX_SZDD, UMSX_KWAJ };

struct umsx_info {
    enum umsx_kind kind;
    int method;           // KWAJ's 0..4; 0 for SZDD and a plain file
    uint32_t length;      // the expanded size, when has_length
    int has_length;       // SZDD always; KWAJ when its header carries it
    size_t data;          // where the compressed bytes start
    char name[13];        // KWAJ's stored "NAME.EXT", else ""
    char missing;         // SZDD's last character of the name ("SND.CP_" -> 'L'), else 0
};

// What a file is: 0 with `info` filled in -- a file with neither magic is
// UMSX_PLAIN, copied as it is -- or -EINVAL for a header that does not
// fit the file, -ENOTSUP for an SZDD method other than 'A'.
int umsx_probe(const uint8_t *in, size_t n, struct umsx_info *info);

#define UMSX_WINDOW 4096
#define UMSX_CHUNK  4096
#define UMSX_SYMS   384      // 16 + 16 + 32 + 64 + 256, KWAJ's five tables

// ~9 KiB: too big for a ring-3 frame. Static, or allocated.
struct umsx_scratch {
    uint8_t window[UMSX_WINDOW];
    uint8_t chunk[UMSX_CHUNK];
    uint8_t lens[UMSX_SYMS];
    uint16_t count[5][16];
    uint16_t symbol[UMSX_SYMS];
};

// Receives the expanded bytes in order; non-zero stops the expand, which
// then answers -EIO.
typedef int (*umsx_out)(void *ctx, const uint8_t *data, size_t n);

// Expands `in` (the whole file) through `out`. 0, with *written the bytes
// produced, or a negative errno: -EINVAL a damaged file, -ENOTSUP a
// method this does not read, -EIO `out` stopped it.
int umsx_expand(const uint8_t *in, size_t n, struct umsx_scratch *s,
                umsx_out out, void *ctx, uint32_t *written);

// The name a file expands to, from the name it has on the disk: KWAJ's
// stored name when it has one, else SZDD's missing character in place of
// the trailing '_', else `disk_name` itself. Into `out` (cap >= 13).
void umsx_name(const struct umsx_info *info, const char *disk_name, char *out, size_t cap);

#endif
