#ifndef ULIB_USMB_H
#define ULIB_USMB_H

// An SMB2/3 CLIENT, READ-ONLY: list a share, stat a path, read a file.
// /lib/libsmb.so -- `/bin/smb` and `/bin/smbfsd` link it.
//
// **WHAT IT SPEAKS.** Dialects 2.0.2, 2.1, 3.0, 3.0.2 and 3.1.1 over
// direct TCP (port 445), NTLMv2 inside SPNEGO, and SIGNING on every
// request once a session has a key: HMAC-SHA256 for 2.x, AES-CMAC
// for 3.x, the 3.1.1 key bound to the pre-authentication hash. A guest
// or anonymous session has no key and is unsigned, which is why a
// server that REQUIRES signing (Windows 11 24H2 does) refuses one.
// Not here: encryption (a share that demands it is refused, by name),
// Kerberos, DFS referrals, multichannel, oplocks, and every write.
//
// **ONE REQUEST IN FLIGHT.** Each call sends one request and waits for
// its answer, so a session is one thread's; credits are asked for but
// never needed beyond one. That is what makes the client small, and a
// remote filesystem served one call at a time (smbfsd) needs no more.
//
// **THE PROTOCOL NEVER TOUCHES A SOCKET.** It talks through `struct
// usmb_io`, so the same file builds on the host and is tested there
// against a real Samba (`tools/smb_hostcheck.py`). usmb_dial() is the
// SaunaOS transport.
//
// Paths are relative to the share, with '/' separators: "" or "/" is
// the share's root. NAMES ARE SAUNAOS TEXT, which is UTF-8, both ways; a
// stray byte that is not UTF-8 goes out as its Latin-1 letter. User names
// and passwords too, as the keyboard types them.
#include <stddef.h>
#include <stdint.h>

struct usmb; // opaque: one connection, one session, one share

// Errors are negative. usmb_strerror() names each in a few words.
enum {
    USMB_OK         = 0,
    USMB_E_NET      = -1,  // the connection failed or broke
    USMB_E_TIMEOUT  = -2,  // no answer in time
    USMB_E_PROTO    = -3,  // the server said something this client cannot parse
    USMB_E_LOGON    = -4,  // wrong user name or password
    USMB_E_ACCESS   = -5,  // signed in, but not allowed
    USMB_E_NOENT    = -6,  // no such file or folder
    USMB_E_NOSHARE  = -7,  // no such share on that server
    USMB_E_NOTDIR   = -8,
    USMB_E_ISDIR    = -9,
    USMB_E_NOMEM    = -10,
    USMB_E_SIGN     = -11, // a response failed its signature check
    USMB_E_ENCRYPT  = -12, // the share or server requires encryption
    USMB_E_NEEDSIGN = -13, // the server requires signing and this session has no key
    USMB_E_DIALECT  = -14, // no dialect in common (an SMB1-only server)
    USMB_E_RESOLVE  = -15, // the server's name did not resolve
    USMB_E_OTHER    = -16, // any other NTSTATUS; usmb_last_status() has it
    USMB_E_SHAREDENIED = -17, // the share refused this user -- or demands encryption
};
const char *usmb_strerror(int err);

// The transport. send() writes all of `len` or fails (-1); recv()
// returns >0 bytes, 0 for a timeout or the end of the stream, -1 for an
// error. random() fills `len` bytes from a source fit for a nonce.
struct usmb_io {
    void *ctx;
    int (*send)(void *ctx, const void *buf, size_t len);
    int (*recv)(void *ctx, void *buf, size_t len, unsigned timeout_ms);
    void (*close)(void *ctx);
    int (*random)(void *ctx, void *buf, size_t len);
};

// Who to sign in as. A NULL or empty `user` is ANONYMOUS; `guest`
// signs in as "guest" with no password. `nt_hash` is MD4 of the
// password in UTF-16LE (usmb_nt_hash()) -- the only form NTLM needs, so
// a caller that saves a credential saves this, never the password. An
// empty `domain` uses the server's own (right for a standalone NAS).
struct usmb_auth {
    const char *user;
    const char *domain;
    uint8_t nt_hash[16];
};
void usmb_nt_hash(const char *password, uint8_t out[16]);

// Connect, negotiate, sign in and attach to `share` on `server` (the
// name as typed -- it goes into the share path the server checks).
// `timeout_ms` bounds each wait for an answer. Takes ownership of `io`
// on every return, success or not.
int usmb_open(struct usmb **out, const struct usmb_io *io,
              const char *server, const char *share,
              const struct usmb_auth *auth, unsigned timeout_ms);
void usmb_close(struct usmb *s);   // logs off and closes the transport

// What was negotiated, for `smb info` and the File Manager's status.
struct usmb_info {
    uint16_t dialect;      // 0x0311 for 3.1.1, ...
    int signing;           // requests and responses are signed
    int guest;             // the server signed us in as its guest
    uint32_t max_read;
};
void usmb_get_info(const struct usmb *s, struct usmb_info *out);
uint32_t usmb_last_status(const struct usmb *s); // the NTSTATUS behind USMB_E_OTHER
// The same for the last usmb_open() that FAILED in this process (0 for
// none) -- the session it would be read from is gone by then.
uint32_t usmb_failed_status(void);

// One entry's metadata. Times are seconds since the Unix epoch, UTC.
struct usmb_stat {
    uint64_t size;
    uint64_t created, modified;
    int is_dir;
    int hidden;            // FILE_ATTRIBUTE_HIDDEN
};
int usmb_stat(struct usmb *s, const char *path, struct usmb_stat *out);

// Every entry of a directory but "." and "..". Return nonzero from `cb`
// to stop early.
typedef int (*usmb_list_cb)(void *ctx, const char *name, const struct usmb_stat *st);
int usmb_list(struct usmb *s, const char *path, usmb_list_cb cb, void *ctx);

// An open file. Reads go to the server as they are asked for -- there
// is no cache here; smbfsd keeps handles open between calls instead.
struct usmb_file { uint8_t id[16]; uint64_t size; };
int usmb_open_file(struct usmb *s, const char *path, struct usmb_file *f);
// Bytes read (0 at the end of the file), or an error. At most
// usmb_info.max_read per call: 1 MiB from a server that allows large
// MTU (multi-credit READs; 2.1 and later), 64 KiB otherwise.
int64_t usmb_read(struct usmb *s, const struct usmb_file *f,
                  uint64_t offset, void *buf, uint32_t len);
int usmb_close_file(struct usmb *s, const struct usmb_file *f);

// The share's size and what is free in it, in bytes.
int usmb_fs_size(struct usmb *s, uint64_t *total, uint64_t *avail);

// "//server/share/dir", "\\server\share", "smb://server:445/share/dir":
// split into the three parts, with the port if one was given (else 0).
// 0, or -1 for a string that names no share.
int usmb_parse_url(const char *url, char *server, size_t scap, uint16_t *port,
                   char *share, size_t shcap, char *path, size_t pcap);

// THE SAUNAOS TRANSPORT: resolve `server` (a dotted quad or a DNS name),
// connect to `port` (0 = 445) and fill `io`. 0, or USMB_E_RESOLVE /
// USMB_E_NET / USMB_E_TIMEOUT.
int usmb_dial(const char *server, uint16_t port, unsigned timeout_ms, struct usmb_io *io);

#endif
