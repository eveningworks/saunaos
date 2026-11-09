// <ar.h> -- the Unix archive format: what `ar` writes and `ld` reads
// (every .a in /usr/lib), and what make reads for a `lib.a(member.o)`
// target. A FORMAT, so a header only: the common System V / GNU layout,
// field for field as binutils writes it.
#ifndef _AR_H
#define _AR_H

#define ARMAG  "!<arch>\n"   // the archive's first eight bytes
#define SARMAG 8
#define ARFMAG "`\n"         // the end of every member header

// Each member is this 60-byte header, then its bytes, padded to even.
// Every field is ASCII, space-padded, NOT NUL-terminated.
struct ar_hdr {
    char ar_name[16];   // "name/" (GNU), or "/N" into the long-name table
    char ar_date[12];   // seconds since the epoch, decimal
    char ar_uid[6];
    char ar_gid[6];
    char ar_mode[8];    // octal
    char ar_size[10];   // the member's size in bytes, decimal
    char ar_fmag[2];    // ARFMAG
};

#endif
