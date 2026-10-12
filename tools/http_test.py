#!/usr/bin/env python3
"""Drive libhttp's HTTP/1.1 half through /bin/wget against a server here.

WHAT IT PROVES, each against the SERVER's count or the BYTES, never
against what the client says about itself:

  1. Three URLs on one command line share ONE connection (the server
     counted one), and a body that arrived chunked, gzipped, or both
     hashes to the file the server sent.
  2. A kept-alive connection the server dropped without saying so is
     found out by the next request, which retries on a fresh one and
     succeeds (two connections, both bodies).
  3. Redirects are followed by default, --max-redirect 0 hands the 302
     back, and a POST answered with 303 becomes a GET.
  4. A POST carries its body and Content-Type; --header adds a line; -S
     prints every response header as the server sent it.
  5. https with a test CA: two URLs over ONE TLS connection, and an
     expired, a wrong-name and a self-signed certificate are each
     refused with THEIR reason.

**IT NEVER LEAVES THIS MACHINE.** tools/web_server.py on the host,
reached through SLIRP's 10.0.2.2.

    python3 tools/http_test.py
    python3 tools/http_test.py --keep

Needs `openssl` for the certificates (the https checks SKIP without it).
"""
import argparse
import hashlib
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from net_test import launch, kill  # noqa: E402
from harness import Results, copy_disk  # noqa: E402
from https_test import install_anchor, clear_anchors  # noqa: E402
from web_server import WebServer, make_ca, GATEWAY  # noqa: E402

PORT = 18081
PORT_TLS = 18444
BIG = 40000   # straddles many socket reads and many 100-byte chunks


def corpus(d):
    os.makedirs(d, exist_ok=True)
    # Bytes that are not text, so a CRLF or NUL mishandled anywhere shows.
    data = hashlib.sha256(b"seed").digest() * (BIG // 32 + 1)
    data = bytes((b * 7 + i) & 0xFF for i, b in enumerate(data[:BIG]))
    open(os.path.join(d, "big.bin"), "wb").write(data)
    open(os.path.join(d, "a.txt"), "wb").write(b"first body\n")
    open(os.path.join(d, "b.txt"), "wb").write(b"second body\n")
    return hashlib.sha256(data).hexdigest()


def stats(srv):
    return srv.counts()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--keep", action="store_true", help="leave the guest running")
    args = ap.parse_args()

    disk_src = os.path.join(ROOT, "disk.img")
    if not os.path.exists(disk_src):
        print("http_test: SKIP -- no disk.img; run `make iso` first")
        return 0

    c = Results()
    tmp = tempfile.mkdtemp(prefix="http_test.")
    pidfile = None
    servers = []
    try:
        web = os.path.join(tmp, "web")
        want = corpus(web)
        srv = WebServer(web, PORT)
        servers.append(srv)
        certs = make_ca(os.path.join(tmp, "ca")) if shutil.which("openssl") else None
        tls = {}
        if certs:
            for i, leaf in enumerate(("good", "expired", "wrongname", "selfsigned")):
                tls[leaf] = WebServer(web, PORT_TLS + i, tls=certs[leaf])
                servers.append(tls[leaf])

        disk = os.path.join(tmp, "http.img")
        copy_disk(disk_src, disk)
        if certs:
            clear_anchors(disk)
            install_anchor(disk, certs["ca"], "toyos-test-ca.pem")

        print("http_test: booting")
        sh, pidfile = launch(disk, tmp, "http", "e1000")
        u = f"http://{GATEWAY}:{PORT}"

        # 1. one connection, three requests, bodies through every framing
        srv.reset()
        out = sh.run(f"wget -O /tmp/big {u}/big.bin?chunked {u}/a.txt {u}/b.txt")
        conns, reqs = stats(srv)
        c.ok("three URLs ride ONE kept-alive connection (server's count)",
             conns == 1 and reqs == 3, f"connections={conns} requests={reqs}; {out.strip()[:120]}")
        out = sh.run(f"wget -O /tmp/big {u}/big.bin?chunked")
        got = sh.run("sum -a sha256 /tmp/big")
        c.ok("a CHUNKED body arrives byte for byte", want in got, got.strip()[:100])
        out = sh.run(f"wget -O /tmp/big {u}/big.bin?gzip")
        got = sh.run("sum -a sha256 /tmp/big")
        c.ok("a GZIPPED body arrives inflated, byte for byte", want in got, got.strip()[:100])
        out = sh.run(f"wget -O /tmp/big {u}/big.bin?gzip&chunked")
        got = sh.run("sum -a sha256 /tmp/big")
        c.ok("gzip INSIDE chunked arrives byte for byte", want in got, got.strip()[:100])

        # 2. a pooled connection the server dropped
        srv.reset()
        out = sh.run(f"wget {u}/a.txt?drop {u}/b.txt")
        conns, reqs = stats(srv)
        c.ok("a silently dropped connection is retried on a fresh one",
             "first body" in out and "second body" in out and conns == 2,
             f"connections={conns}; {out.strip()[:160]}")

        # 3. redirects
        out = sh.run(f"wget {u}/redirect/3/a.txt")
        c.ok("redirects are followed by default", "first body" in out, out.strip()[:120])
        out = sh.run(f"wget --max-redirect 0 {u}/redirect/1/a.txt")
        c.ok("--max-redirect 0 hands the 302 back", "status 302" in out, out.strip()[:120])
        out = sh.run(f"wget --max-redirect 2 {u}/redirect/5/a.txt")
        c.ok("...and too many redirects is an error", "more than 2 redirects" in out,
             out.strip()[:120])
        out = sh.run(f"wget --post-data x=1 {u}/redirect303/echo")
        c.ok("a POST answered with 303 becomes a GET", "method=GET" in out, out.strip()[:160])

        # 4. methods, bodies and headers
        out = sh.run(f"wget --post-data name=toy {u}/echo")
        c.ok("a POST carries its body", "method=POST" in out and "body=name=toy" in out,
             out.strip()[:200])
        c.ok("...and its Content-Type",
             "application/x-www-form-urlencoded" in out, out.strip()[:200])
        out = sh.run(f"wget --header 'X-Toy: yes' {u}/echo")
        c.ok("--header adds a request header", "X-Toy: yes" in out, out.strip()[:200])
        out = sh.run(f"wget -S {u}/a.txt")
        c.ok("-S prints the status line and every header",
             "HTTP/1.1 200" in out and "Content-Length: 11" in out, out.strip()[:200])

        # 5. https
        if certs:
            s = f"https://{GATEWAY}"
            tls["good"].reset()
            out = sh.run(f"wget --weak-entropy {s}:{PORT_TLS}/a.txt {s}:{PORT_TLS}/b.txt")
            conns, reqs = tls["good"].counts()
            c.ok("https: two URLs over ONE TLS connection",
                 "first body" in out and "second body" in out and conns == 1 and reqs == 2,
                 f"connections={conns} requests={reqs}; {out.strip()[:160]}")
            for i, (leaf, words) in enumerate((("expired", "has expired"),
                                               ("wrongname", "different host name"),
                                               ("selfsigned", "self-signed")), start=1):
                out = sh.run(f"wget --weak-entropy {s}:{PORT_TLS + i}/a.txt")
                c.ok(f"https: a certificate that is {leaf} is refused with its reason",
                     words in out and "first body" not in out, out.strip()[:160])
        else:
            print("http_test: https checks SKIPPED -- no openssl")
    finally:
        if pidfile and not args.keep:
            kill(pidfile)
        for s in servers:
            s.shutdown()
        if not args.keep:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print(f"http_test: guest and files kept in {tmp}")

    bad = c.failed()
    print(f"\nhttp_test: {len(c.rows) - len(bad)}/{len(c.rows)} checks passed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
