#!/usr/bin/env python3
"""Check the SMB client (/lib/libsmb.so's protocol) against a real Samba, on the HOST.

The protocol files (userland/dynlib/usmb.c, usmb_auth.c) and the crypto
backend (userland/backends/mbedtls/ucrypto_mbedtls.c) compile here with
the host's gcc, against the in-tree mbedTLS, beside a driver with a
POSIX socket transport. The oracle shares no code with them: Samba
(tools/samba_server.py, in Docker) is the server, and the bytes it
serves are the files this script wrote.

WHY ON THE HOST. Every dialect, the signing algorithm each one picks,
NTLMv2 and the refusals are a matrix of server configurations; a guest
boot per cell would take minutes for what this does in seconds. The
guest half -- the SaunaOS transport, /bin/smb, the bridge -- is
tools/smb_test.py.

  python3 tools/smb_hostcheck.py                    # every dialect, every refusal
  python3 tools/smb_hostcheck.py --dialect SMB3_11  # one
  python3 tools/smb_hostcheck.py --positive-control # a broken signing key: must FAIL

The control corrupts the SMB 3.x signing-key label, so a 3.x session's
signatures are wrong: the server (signing = mandatory) or the client's
own check must refuse, and the run must go red.

Exit status is non-zero if any check fails.
"""
import argparse
import hashlib
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostcheck  # noqa: E402
import samba_server  # noqa: E402

ROOT = hostcheck.ROOT

DRIVER = r"""
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "usmb.h"

static int p_send(void *c, const void *b, size_t n) {
    const char *p = b;
    while (n) { ssize_t w = send(*(int *)c, p, n, MSG_NOSIGNAL); if (w <= 0) return -1; p += w; n -= (size_t)w; }
    return 0;
}
static int p_recv(void *c, void *b, size_t n, unsigned t) {
    struct pollfd pf = { *(int *)c, POLLIN, 0 };
    int r = poll(&pf, 1, (int)t);
    if (r == 0) return 0;
    if (r < 0) return -1;
    ssize_t g = recv(*(int *)c, b, n, 0);
    return g < 0 ? -1 : (int)g;
}
static void p_close(void *c) { close(*(int *)c); free(c); }
static int p_random(void *c, void *b, size_t n) {
    (void)c;
    int fd = open("/dev/urandom", O_RDONLY);
    ssize_t g = read(fd, b, n);
    close(fd);
    return g == (ssize_t)n ? 0 : -1;
}

static int print_entry(void *ctx, const char *name, const struct usmb_stat *st) {
    (void)ctx;
    if (st->is_dir) printf("d - %s\n", name);
    else printf("f %llu %s\n", (unsigned long long)st->size, name);
    return 0;
}

int main(int argc, char **argv) {
    // smbcheck PORT SERVER SHARE USER PASSWORD CMD [PATH]
    if (argc < 7) return 2;
    int *fd = malloc(sizeof *fd);
    *fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[1])) };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(*fd, (struct sockaddr *)&a, sizeof a)) { perror("connect"); return 3; }
    struct usmb_io io = { fd, p_send, p_recv, p_close, p_random };
    struct usmb_auth au;
    memset(&au, 0, sizeof au);
    au.user = argv[4];
    usmb_nt_hash(argv[5], au.nt_hash);
    struct usmb *s;
    int rc = usmb_open(&s, &io, argv[2], argv[3], &au, 5000);
    if (rc) { printf("ERR %d %s\n", rc, usmb_strerror(rc)); return 1; }
    const char *cmd = argv[6], *path = argc > 7 ? argv[7] : "";
    if (!strcmp(cmd, "info")) {
        struct usmb_info in;
        usmb_get_info(s, &in);
        printf("dialect=0x%04x signing=%d guest=%d max_read=%u\n", in.dialect, in.signing, in.guest, in.max_read);
    } else if (!strcmp(cmd, "ls")) {
        rc = usmb_list(s, path, print_entry, NULL);
    } else if (!strcmp(cmd, "stat")) {
        struct usmb_stat st;
        rc = usmb_stat(s, path, &st);
        if (!rc) printf("%s %llu\n", st.is_dir ? "d" : "f", (unsigned long long)st.size);
    } else if (!strcmp(cmd, "get")) {
        struct usmb_file f;
        rc = usmb_open_file(s, path, &f);
        static unsigned char buf[1 << 20];   /* what one large-MTU READ may carry */
        for (uint64_t off = 0; !rc && off < f.size;) {
            int64_t n = usmb_read(s, &f, off, buf, sizeof buf);
            if (n < 0) { rc = (int)n; break; }
            if (n == 0) break;
            fwrite(buf, 1, (size_t)n, stdout);
            off += (uint64_t)n;
        }
        if (!rc) rc = usmb_close_file(s, &f);
    } else if (!strcmp(cmd, "df")) {
        uint64_t t = 0, v = 0;
        rc = usmb_fs_size(s, &t, &v);
        if (!rc) printf("%llu %llu\n", (unsigned long long)t, (unsigned long long)v);
    }
    if (rc) { printf("ERR %d %s\n", rc, usmb_strerror(rc)); usmb_close(s); return 1; }
    usmb_close(s);
    return 0;
}
"""

HOST_CONFIG = """
#define MBEDTLS_MD5_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_MD_C
#define MBEDTLS_AES_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CMAC_C
"""

MBEDTLS_FILES = ["md5", "sha256", "sha512", "md", "aes", "aesni", "aesce", "cipher",
                 "cipher_wrap", "cmac", "platform_util", "constant_time", "platform"]

# The 3.0/3.0.2 signing-key label, and 3.1.1's.
CONTROL = [('"SMB2AESCMAC", 12', '"SMB2AESCMAX", 12'),
           ('"SMBSigningKey", 14', '"SMBSigningKez", 14')]


def build(tmp, control):
    hostcheck.write(tmp, "host_config.h", HOST_CONFIG)
    srcs = [hostcheck.write(tmp, "driver.c", DRIVER)]
    srcs.append(hostcheck.stage(tmp, "userland/dynlib/usmb.c"))
    srcs.append(hostcheck.stage(tmp, "userland/dynlib/usmb_auth.c", edits=CONTROL,
                                apply=control, tool="smb_hostcheck"))
    srcs.append(hostcheck.stage(tmp, "userland/backends/mbedtls/ucrypto_mbedtls.c"))
    # The two headers alone: userland/include also holds SaunaOS's libc
    # headers, which must not shadow the host's.
    hostcheck.stage(tmp, "userland/include/usmb.h")
    hostcheck.stage(tmp, "userland/include/ucrypto.h")
    hostcheck.stage(tmp, "userland/dynlib/usmb_internal.h")
    lib = os.path.join(ROOT, "userland/ports/mbedtls/library")
    srcs += [os.path.join(lib, f + ".c") for f in MBEDTLS_FILES]
    flags = ("-O2", "-w", '-DMBEDTLS_CONFIG_FILE="host_config.h"')
    return hostcheck.compile(tmp, "smbcheck", srcs, flags=flags,
                             includes=[tmp, os.path.join(ROOT, "userland/ports/mbedtls/include"), lib],
                             tool="smb_hostcheck")


class Run:
    def __init__(self, exe):
        self.exe, self.fails, self.passes = exe, 0, 0

    def call(self, srv, share, user, pw, *cmd):
        r = subprocess.run([self.exe, str(srv.port), "toynas", share, user, pw, *cmd],
                           capture_output=True, timeout=120)
        return r.returncode, r.stdout

    def check(self, label, ok, detail=""):
        if ok:
            self.passes += 1
        else:
            self.fails += 1
            print(f"  FAIL {label} {detail}")


def dialect_cell(run, dialect):
    with samba_server.SambaServer(dialect, dialect, "mandatory") as srv:
        u, p = srv.USER, srv.PASSWORD
        rc, out = run.call(srv, "media", u, p, "info")
        # Large MTU (multi-credit READs) from 2.1 on; 2.0.2 reads one credit.
        mr = 65536 if dialect == "SMB2_02" else 1 << 20
        want = f"dialect=0x{samba_server.WIRE[dialect]:04x} signing=1 guest=0 max_read={mr}"
        run.check(f"{dialect} info", rc == 0 and out.decode().strip() == want, out[:200])
        if rc:
            return
        rc, out = run.call(srv, "media", u, p, "ls", "")
        got = {}
        for line in out.splitlines():
            kind, size, name = line.split(b" ", 2)
            got[name] = None if kind == b"d" else int(size)
        run.check(f"{dialect} ls /", rc == 0 and got == srv.listing("media"),
                  f"{sorted(got.items())[:6]} vs {sorted(srv.listing('media').items())[:6]}")
        rc, out = run.call(srv, "media", u, p, "ls", "many")
        run.check(f"{dialect} ls many", rc == 0 and len(out.decode().splitlines()) == samba_server.MANY,
                  f"{len(out.decode().splitlines())} entries")
        rc, out = run.call(srv, "media", u, p, "get", "big.bin")
        run.check(f"{dialect} get big.bin", rc == 0 and hashlib.sha256(out).hexdigest() == srv.sha256("media/big.bin"),
                  f"{len(out)} bytes")
        for name in (samba_server.UNICODE_NAME, samba_server.LATIN1_NAME):
            rc, out = run.call(srv, "media", u, p, "get", samba_server.wire_name(name))
            run.check(f"{dialect} get {samba_server.wire_name(name)!r}",
                      rc == 0 and out == srv.files[f"media/{name}"], out[:80])
        rc, out = run.call(srv, "media", u, p, "get", "sub/nested/deep.txt")
        run.check(f"{dialect} get nested", rc == 0 and out == srv.files["media/sub/nested/deep.txt"], out[:80])
        rc, out = run.call(srv, "media", u, p, "get", "empty.txt")
        run.check(f"{dialect} get empty", rc == 0 and out == b"", out[:80])
        rc, out = run.call(srv, "media", u, p, "stat", "hello.txt")
        run.check(f"{dialect} stat", rc == 0 and out.decode().strip() == f"f {len(srv.files['media/hello.txt'])}", out)
        rc, out = run.call(srv, "media", u, p, "df")
        parts = out.decode().split()
        run.check(f"{dialect} df", rc == 0 and len(parts) == 2 and int(parts[0]) > 0, out)


def refusals(run):
    with samba_server.SambaServer("SMB2_02", "SMB3_11", "mandatory") as srv:
        u, p = srv.USER, srv.PASSWORD
        cases = [
            ("wrong password", ("media", u, "nope", "info"), "ERR -4"),
            ("no such share", ("nosuch", u, p, "info"), "ERR -7"),
            ("no such file", ("media", u, p, "get", "missing.txt"), "ERR -6"),
            # ACCESS_DENIED on the wire (MS-SMB2 3.3.5.7), so the share-level refusal
            ("encrypted share", ("sealed", u, p, "info"), "ERR -17"),
            ("guest vs mandatory signing", ("public", "guest", "", "info"), "ERR -13"),
            ("ls a file", ("media", u, p, "ls", "hello.txt"), "ERR -8"),
        ]
        for label, args, want in cases:
            rc, out = run.call(srv, *args)
            run.check(label, rc != 0 and out.decode().startswith(want), out[:200])
    with samba_server.SambaServer("SMB2_02", "SMB3_11", "auto") as srv:
        rc, out = run.call(srv, "public", "guest", "", "info")
        run.check("guest on public", rc == 0 and "guest=1" in out.decode() and "signing=0" in out.decode(), out)
        rc, out = run.call(srv, "public", "guest", "", "get", "readme.txt")
        run.check("guest get", rc == 0 and out == srv.files["public/readme.txt"], out[:80])
        rc, out = run.call(srv, "public", "", "", "info")
        run.check("anonymous on public", rc == 0 and "signing=0" in out.decode(), out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dialect", choices=samba_server.DIALECTS)
    ap.add_argument("--positive-control", action="store_true")
    args = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        run = Run(build(tmp, args.positive_control))
        for d in ([args.dialect] if args.dialect else samba_server.DIALECTS):
            print(f"dialect {d}")
            dialect_cell(run, d)
        if not args.dialect:
            print("refusals")
            refusals(run)
    print(f"smb_hostcheck: {run.passes} passed, {run.fails} failed")
    if args.positive_control:
        if run.fails:
            print("smb_hostcheck: positive control went red, as it must")
            return 0
        print("smb_hostcheck: POSITIVE CONTROL STAYED GREEN -- the check cannot see a broken key")
        return 1
    return 1 if run.fails else 0


if __name__ == "__main__":
    sys.exit(main())
