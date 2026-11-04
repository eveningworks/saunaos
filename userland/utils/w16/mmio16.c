// mmio16.c -- MMSYSTEM's buffered file I/O, as a program reads a RIFF file
// (its WAVs) through it: open by a DOS path, read, seek, and step into and
// out of RIFF chunks. A file on disk only: a memory file (the MEM I/O
// procedure) or a program's own I/O procedure is refused at open, as
// Windows refused an I/O procedure it did not know.
#include "w16_internal.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MMIOERR_CANNOTOPEN     257
#define MMIOERR_CANNOTSEEK     261
#define MMIOERR_CANNOTREAD     259
#define MMIOERR_CHUNKNOTFOUND  265
#define MMIO_RWMODE     0x0003
#define MMIO_CREATE     0x1000
#define MMIO_FINDCHUNK  0x0010
#define MMIO_FINDRIFF   0x0020
#define MMIO_FINDLIST   0x0040

static int *g_fd;                                // by HMMIO - 1; -1 a free slot
static int g_nfd;

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i); }

static int fd_of(uint16_t h) { return h >= 1 && h <= g_nfd ? g_fd[h - 1] : -1; }

// mmioOpen(szFilename, lpmmioinfo, dwOpenFlags) -> HMMIO
// Windows: Opens a file for buffered reading and writing, by its DOS name -- or a memory block or a
// program's own I/O procedure, as lpmmioinfo says; on failure its wErrorRet says why.
// w16: The same for a file on one of w16's drives, read, written or created; a memory file or an
// I/O procedure of the program's own is refused with MMIOERR_CANNOTOPEN.
static int api_mmioOpen(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct cpu16 *c = &w->cpu;
    uint32_t name = w16_arg32(a), info = w16_arg32(a), flags = w16_arg32(a);
    uint8_t *mi = info ? cpu16_ptr(c, (uint16_t)(info >> 16), (uint16_t)info, 18, 1) : 0;
    if (info && !mi) return -1;
    c->r[R_AX] = c->r[R_DX] = 0;
    int err = MMIOERR_CANNOTOPEN, fd = -1;
    char dos[260], host[1024];
    if ((!mi || (!rd32(mi + 4) && !rd32(mi + 8))) && (name >> 16) &&
        cpu16_str(c, (uint16_t)(name >> 16), (uint16_t)name, dos, sizeof dos) >= 0 &&
        w16_dos_path(dos, host, sizeof host, (flags & MMIO_CREATE) != 0, 0, 0) == 0) {
        int mode = (flags & MMIO_RWMODE) == 0 ? O_RDONLY : (flags & MMIO_RWMODE) == 1 ? O_WRONLY : O_RDWR;
        fd = open(host, mode | (flags & MMIO_CREATE ? O_CREAT | O_TRUNC : 0), 0644);
    }
    int slot = 0;
    while (fd >= 0 && slot < g_nfd && g_fd[slot] >= 0) slot++;
    if (fd >= 0 && slot == g_nfd) {
        int *g = realloc(g_fd, sizeof *g * (size_t)(g_nfd + 1));
        if (g) { g_fd = g; g_fd[g_nfd++] = -1; }
    }
    if (fd >= 0 && slot < g_nfd) {
        g_fd[slot] = fd;
        c->r[R_AX] = (uint16_t)(slot + 1);
        err = 0;
    } else if (fd >= 0) {
        close(fd);
    }
    if (mi) { mi[12] = (uint8_t)err; mi[13] = (uint8_t)(err >> 8); }
    return 12;
}

// mmioClose(hmmio, wFlags) -> UINT
// Windows: Closes a file mmioOpen opened.
// w16: The same.
static int api_mmioClose(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    w16_arg16(a);
    int fd = fd_of(h);
    if (fd >= 0) { close(fd); g_fd[h - 1] = -1; }
    w->cpu.r[R_AX] = fd >= 0 ? 0 : 5;            // MMSYSERR_INVALHANDLE
    w->cpu.r[R_DX] = 0;
    return 4;
}

// mmioRead(hmmio, pch, cch) -> LONG
// Windows: Reads up to cch bytes into a huge buffer; how many, 0 at the end, -1 on an error.
// w16: The same, the buffer checked to lie inside the block its selector starts.
static int api_mmioRead(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    uint32_t far = w16_arg32(a), n = w16_arg32(a);
    int fd = fd_of(h);
    long r = -1;
    if (fd >= 0 && (int32_t)n >= 0) {
        uint8_t *p = n ? cpu16_ptr(&w->cpu, (uint16_t)(far >> 16), (uint16_t)far, n, 1) : 0;
        if (n && !p) return -1;
        r = n ? (long)read(fd, p, n) : 0;
    }
    w->cpu.r[R_AX] = (uint16_t)r;
    w->cpu.r[R_DX] = (uint16_t)((uint32_t)r >> 16);
    return 10;
}

// mmioSeek(hmmio, lOffset, iOrigin) -> LONG
// Windows: Moves the file position from the start, from where it is, or from the end; the new
// position, or -1.
// w16: The same.
static int api_mmioSeek(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    int32_t off = (int32_t)w16_arg32(a);
    uint16_t origin = w16_arg16(a);
    int fd = fd_of(h);
    long r = fd >= 0 && origin <= 2 ? (long)lseek(fd, off, origin == 0 ? SEEK_SET : origin == 1 ? SEEK_CUR : SEEK_END) : -1;
    w->cpu.r[R_AX] = (uint16_t)r;
    w->cpu.r[R_DX] = (uint16_t)((uint32_t)r >> 16);
    return 8;
}

// mmioDescend(hmmio, lpck, lpckParent, wFlags) -> UINT
// Windows: Steps into the RIFF chunk at the file position -- or, with MMIO_FINDCHUNK, FINDRIFF or
// FINDLIST, the first one inside lpckParent with lpck's id (or form type) -- filling lpck's MMCKINFO
// and leaving the position at its data; MMIOERR_CHUNKNOTFOUND at the parent's or the file's end.
// w16: The same.
static int api_mmioDescend(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    struct cpu16 *c = &w->cpu;
    uint16_t h = w16_arg16(a);
    uint32_t ck = w16_arg32(a), par = w16_arg32(a);
    uint16_t flags = w16_arg16(a);
    uint8_t *k = cpu16_ptr(c, (uint16_t)(ck >> 16), (uint16_t)ck, 20, 1);
    const uint8_t *p = par ? cpu16_ptr(c, (uint16_t)(par >> 16), (uint16_t)par, 20, 0) : 0;
    if (!k || (par && !p)) return -1;
    c->r[R_DX] = 0;
    int fd = fd_of(h);
    if (fd < 0) { c->r[R_AX] = 5; return 12; }
    uint32_t want_id = rd32(k), want_type = rd32(k + 8);
    uint64_t end = p ? (uint64_t)rd32(p + 12) + rd32(p + 4) : UINT64_MAX;
    for (;;) {
        off_t at = lseek(fd, 0, SEEK_CUR);
        uint8_t hd[12];
        if (at < 0 || (uint64_t)at + 8 > end || read(fd, hd, 8) != 8) { c->r[R_AX] = MMIOERR_CHUNKNOTFOUND; return 12; }
        uint32_t id = rd32(hd), size = rd32(hd + 4), type = 0;
        int form = id == 0x46464952 || id == 0x5453494C;   // "RIFF", "LIST"
        if (form && read(fd, hd + 8, 4) != 4) { c->r[R_AX] = MMIOERR_CHUNKNOTFOUND; return 12; }
        if (form) type = rd32(hd + 8);
        int match = (flags & MMIO_FINDCHUNK) ? id == want_id
                  : (flags & MMIO_FINDRIFF) ? id == 0x46464952 && type == want_type
                  : (flags & MMIO_FINDLIST) ? id == 0x5453494C && type == want_type : 1;
        if (match) {
            wr32(k, id); wr32(k + 4, size); wr32(k + 8, type); wr32(k + 12, (uint32_t)at + 8); wr32(k + 16, 0);
            c->r[R_AX] = 0;
            return 12;
        }
        if (lseek(fd, at + 8 + size + (size & 1), SEEK_SET) < 0) { c->r[R_AX] = MMIOERR_CANNOTSEEK; return 12; }
    }
}

// mmioAscend(hmmio, lpck, wFlags) -> UINT
// Windows: Steps out of the chunk lpck describes: the position past its end, its pad byte included.
// w16: The same, for a chunk read; a chunk the program wrote keeps the size it was created with.
static int api_mmioAscend(struct w16 *w, unsigned ordinal, struct w16_args *a) {
    (void)ordinal;
    uint16_t h = w16_arg16(a);
    uint32_t ck = w16_arg32(a);
    w16_arg16(a);
    const uint8_t *k = cpu16_ptr(&w->cpu, (uint16_t)(ck >> 16), (uint16_t)ck, 20, 0);
    if (!k) return -1;
    int fd = fd_of(h);
    uint32_t size = rd32(k + 4);
    w->cpu.r[R_AX] = fd < 0 ? 5 : lseek(fd, (off_t)rd32(k + 12) + size + (size & 1), SEEK_SET) < 0 ? MMIOERR_CANNOTSEEK : 0;
    w->cpu.r[R_DX] = 0;
    return 8;
}

const struct w16_api g_mmio_api[] = {
    { 1210, api_mmioOpen },
    { 1211, api_mmioClose },
    { 1212, api_mmioRead },
    { 1214, api_mmioSeek },
    { 1223, api_mmioDescend },
    { 1224, api_mmioAscend },
};
const unsigned g_mmio_api_n = sizeof g_mmio_api / sizeof g_mmio_api[0];
