#ifndef USERFS_ABI_H
#define USERFS_ABI_H

// A FILESYSTEM SERVED BY A PROCESS (kernel/fs/userfs.c): the kernel
// turns each call on the mount into a request, a daemon answers it.
// Linux's FUSE in shape -- /dev/fuse's read() and write() are TAKE and
// REPLY here -- and READ-ONLY in scope: stat, list, read, and nothing
// that changes a file. /bin/smbfsd is the first server.
//
// SYS_USERFS, RDI = the op:
//   USERFS_MOUNT  RSI = struct userfs_mount *. Mounts at `point` (which
//                 must exist) and makes the CALLER its server; returns a
//                 handle (> 0). A point already served by a process that
//                 has exited is ADOPTED rather than refused, so a
//                 restarted daemon picks its mounts back up. -EBUSY if
//                 its server is alive.
//   USERFS_TAKE   RSI = handle, RDX = struct userfs_req *. Fills it with
//                 the next request and returns 1; blocks while there is
//                 none and returns 0 when woken -- ask again. -ENODEV
//                 once the mount is gone (unmounted): the daemon's cue to
//                 stop serving it.
//   USERFS_REPLY  RSI = struct userfs_reply *. Answers request `id`; a
//                 request whose caller gave up is silently dropped.
//   USERFS_STATE  RSI = struct userfs_state *. What the mount is doing
//                 (connected, asking for a password, unreachable) and
//                 the share's size -- read back through QUERY_FSINFO
//                 without waking the daemon, so a file manager polling
//                 its sidebar never causes a connection.
//   USERFS_CTL    RSI = struct userfs_ctl *. ANY process asks the server
//                 of `point` something (sign in, reconnect); blocks until
//                 the server answers and returns its result.
//
// THE SERVER MUST NOT TOUCH ITS OWN MOUNT: it would be waiting on a
// request only it can answer. The kernel refuses such a call at once
// rather than letting it time out.
#include <stdint.h>

#define USERFS_MOUNT 0
#define USERFS_TAKE  1
#define USERFS_REPLY 2
#define USERFS_STATE 3
#define USERFS_CTL   4

#define USERFS_PATH_MAX 1024   // a request's mount-relative path, NUL included
#define USERFS_CTL_MAX  512    // a USERFS_CTL payload

struct userfs_mount {
    char point[256];           // "/net/media"
    char type[16];             // "smb" -- what `mount` and `df` show
    char source[128];          // "//nas/media" -- likewise
};

// Requests (kernel -> server).
#define USERFS_OP_STAT 1       // reply: struct userfs_stat
#define USERFS_OP_LIST 2       // offset = the entry to resume at; reply: packed userfs_dirent
#define USERFS_OP_READ 3       // offset, len (up to 1 MiB: the kernel reads ahead); reply: the bytes
#define USERFS_OP_CTL  4       // ctl, data; reply: up to `len` bytes back, and the result

struct userfs_req {
    uint32_t id;
    uint32_t op;               // USERFS_OP_*
    uint64_t offset;
    uint32_t len;              // the most the reply may carry
    uint32_t ctl;              // USERFS_OP_CTL: the asker's code
    uint32_t data_len;
    uint32_t reserved;
    char path[USERFS_PATH_MAX];  // "/" for the root
    uint8_t data[USERFS_CTL_MAX];
};

#define USERFS_LIST_MORE 1     // reply flag: entries remain past these

struct userfs_reply {
    uint32_t id;
    int32_t result;            // >= 0 success (READ: bytes, LIST: entries); else -errno
    uint64_t buf;              // the reply's bytes, in the server's memory
    uint32_t len;
    uint32_t flags;            // USERFS_LIST_MORE
};

struct userfs_stat {
    uint64_t size;
    uint64_t created, modified;    // seconds since the epoch, UTC
    uint32_t is_dir;
    uint32_t reserved;
};

// One entry of a LIST reply. Entries are packed, each starting on an
// 8-byte boundary: the name follows, NOT NUL-terminated.
struct userfs_dirent {
    uint64_t size;
    uint64_t modified;
    uint16_t name_len;
    uint8_t  is_dir;
    uint8_t  reserved[5];
};

// What a mount is doing. 0 is never sent: a local filesystem reports it.
#define USERFS_ST_IDLE        1  // not connected; the next access connects
#define USERFS_ST_CONNECTING  2
#define USERFS_ST_CONNECTED   3
#define USERFS_ST_NEED_LOGIN  4  // no password, or the saved one was refused
#define USERFS_ST_UNREACHABLE 5  // the server did not answer
#define USERFS_ST_FAILED      6  // anything else; `note` says what
#define USERFS_ST_NO_SERVER   7  // the KERNEL's: the daemon has exited

struct userfs_state {
    uint32_t handle;
    uint32_t state;            // USERFS_ST_*
    uint64_t total, avail;     // bytes; 0 when unknown
    char note[96];             // one line, for a tooltip or `df`
};

// USERFS_CTL codes, for smbfsd. A payload is NUL-separated fields.
#define USERFS_CTL_LOGIN      1  // "user\0domain\0password\0remember(0|1)"
#define USERFS_CTL_RECONNECT  2  // drop the connection; the next access makes a new one
#define USERFS_CTL_SAVE       3  // "name": a share opened for the session becomes a saved one

struct userfs_ctl {
    char point[256];
    uint32_t code;             // USERFS_CTL_*
    uint32_t len;
    uint8_t data[USERFS_CTL_MAX];
};

#endif
