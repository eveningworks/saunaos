#ifndef NS_TOYOS_ZLIB_H
#define NS_TOYOS_ZLIB_H

// The corner of zlib's API NetSurf calls (utils/hashtable.c: one
// in-memory inflate and a line reader over a maybe-gzipped file), over
// toy-os's own decoder, lib/uinflate.h -- not a vendored zlib. ONLY that
// corner: anything else NetSurf someday calls is a compile error here,
// which is the point of keeping this beside the port rather than in
// tolibc, where it would look like zlib.
//
// **THE WHOLE COMPRESSED STREAM MUST ARRIVE IN THE FIRST inflate()**,
// because uinflate decodes one contiguous buffer (its header says why).
// That is how NetSurf calls it. A caller feeding a stream in pieces gets
// Z_STREAM_ERROR on the second piece, not a quietly short result.
#include <stddef.h>
#include <stdint.h>

#define Z_NULL 0
#define MAX_WBITS 15

#define Z_NO_FLUSH 0
#define Z_FINISH   4

#define Z_OK            0
#define Z_STREAM_END    1
#define Z_STREAM_ERROR (-2)
#define Z_DATA_ERROR   (-3)
#define Z_MEM_ERROR    (-4)
#define Z_BUF_ERROR    (-5)

typedef unsigned char Bytef;
typedef unsigned int uInt;
typedef unsigned long uLong;
typedef void *voidpf;

typedef struct z_stream_s {
    Bytef *next_in;
    uInt avail_in;
    uLong total_in;
    Bytef *next_out;
    uInt avail_out;
    uLong total_out;
    const char *msg;
    void *state;            // the shim's: the decoded bytes still to hand out
    voidpf (*zalloc)(voidpf, uInt, uInt);   // accepted and ignored: malloc
    void (*zfree)(voidpf, voidpf);
    voidpf opaque;
} z_stream;
typedef z_stream *z_streamp;

// windowBits: 8..15 a zlib stream, -8..-15 raw deflate, +16 gzip,
// +32 either wrapper, decided from the first bytes -- zlib's encoding.
int inflateInit2(z_streamp strm, int windowBits);
int inflate(z_streamp strm, int flush);
int inflateEnd(z_streamp strm);

// A file read whole at gzopen() and inflated if it starts with gzip's
// magic, plain otherwise -- zlib's "transparent" reading. Read-only.
typedef struct ns_gzfile *gzFile;
gzFile gzopen(const char *path, const char *mode);
char *gzgets(gzFile file, char *buf, int len);
int gzclose(gzFile file);

#endif
