#!/usr/bin/env python3
"""A throwaway Samba server in Docker, for testing the SMB client.

Shared by tools/smb_hostcheck.py (the client built for the host) and
tools/smb_test.py (the client inside a guest, which reaches this through
SLIRP's 10.0.2.2). It is a FIXTURE, not a test: it serves known files
and says what they are.

LOOPBACK ONLY. The container's 445 is published on 127.0.0.1, never on
the LAN -- QEMU's SLIRP connects from the host process, so the guest
reaches it anyway.

THE IMAGE IS BUILT ONCE (`saunaos-samba:1`, from debian:12 plus apt's
samba), and that build is the one step that reaches the network:
deb.debian.org through Docker. Every run after that is local.

    python3 tools/samba_server.py --serve            # run one until Ctrl-C
    python3 tools/samba_server.py --serve --max SMB2_10 --signing auto

    with samba_server.SambaServer(max_protocol="SMB3_02") as srv:
        srv.port, srv.files["media/hello.txt"], srv.USER, srv.PASSWORD
"""
import argparse
import hashlib
import os
import random
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

IMAGE = "saunaos-samba:1"
DOCKERFILE = """FROM debian:12
RUN apt-get update && apt-get install -y --no-install-recommends samba \\
    && rm -rf /var/lib/apt/lists/*
"""

USER = "reader"
PASSWORD = "correct horse"      # a space, so quoting is exercised end to end
DIALECTS = ("SMB2_02", "SMB2_10", "SMB3_00", "SMB3_02", "SMB3_11")
# What each dialect name is on the wire, for checking `info`.
WIRE = {"SMB2_02": 0x0202, "SMB2_10": 0x0210, "SMB3_00": 0x0300,
        "SMB3_02": 0x0302, "SMB3_11": 0x0311}
MANY = 1500                     # entries in media/many -- several QUERY_DIRECTORY rounds
UNICODE_NAME = "Ünïcødé — name.txt"     # the dash is past Latin-1: comes back as UTF-8
LATIN1_NAME = "Café Ünïcødé.txt"         # all Latin-1: comes back as Latin-1 bytes


def wire_name(name):
    """A name as the client hands it back (usmb.h): Latin-1 when Latin-1
    can spell it, UTF-8 when it cannot."""
    try:
        return name.encode("latin-1")
    except UnicodeEncodeError:
        return name.encode("utf-8")

SMB_CONF = """[global]
  server role = standalone server
  workgroup = WORKGROUP
  netbios name = TOYNAS
  server min protocol = {min}
  server max protocol = {max}
  server signing = {signing}
  map to guest = Bad User
  disable netbios = yes
  smb ports = 445
  load printers = no
  printing = bsd
  printcap name = /dev/null
  disable spoolss = yes
  log level = 1
  passdb backend = tdbsam
[media]
  path = /srv/media
  read only = yes
  valid users = {user}
[public]
  path = /srv/public
  read only = yes
  guest ok = yes
[sealed]
  path = /srv/public
  read only = yes
  valid users = {user}
  server smb encrypt = required
"""


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def ensure_image(rebuild=False):
    if not rebuild and sh(["docker", "image", "inspect", IMAGE]).returncode == 0:
        return
    with tempfile.TemporaryDirectory() as d:
        with open(os.path.join(d, "Dockerfile"), "w") as fh:
            fh.write(DOCKERFILE)
        print(f"samba_server: building {IMAGE} (fetches samba from deb.debian.org)",
              file=sys.stderr)
        r = sh(["docker", "build", "-t", IMAGE, d])
        if r.returncode:
            sys.exit(f"samba_server: docker build failed:\n{r.stderr[-2000:]}")


def make_files(root):
    """The share contents, deterministic. Returns {relpath: bytes}."""
    files = {}
    rng = random.Random(0x5a4b)

    def put(rel, data):
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as fh:
            fh.write(data)
        files[rel] = data

    put("media/hello.txt", b"hello from samba\n")
    put("media/big.bin", rng.randbytes(3 * 1024 * 1024 + 12345))
    put(f"media/{UNICODE_NAME}", "ünïcødé contents\n".encode())
    put(f"media/{LATIN1_NAME}", b"latin-1 name\n")
    put("media/sub/nested/deep.txt", b"three levels down\n")
    put("media/empty.txt", b"")
    for i in range(MANY):
        put(f"media/many/f{i:04d}.txt", f"{i}\n".encode())
    put("public/readme.txt", b"anyone may read this\n")
    os.makedirs(os.path.join(root, "media", "Empty folder"), exist_ok=True)
    for dirpath, dirnames, filenames in os.walk(root):
        os.chmod(dirpath, 0o755)
        for f in filenames:
            os.chmod(os.path.join(dirpath, f), 0o644)
    return files


def negotiate_answered(port):
    """1 if an SMB2 NEGOTIATE (dialect 2.0.2) on `port` gets an SMB2 reply."""
    hdr = b"\xfeSMB" + (64).to_bytes(2, "little") + bytes(58)
    body = (36).to_bytes(2, "little") + (1).to_bytes(2, "little") + bytes(32) + (0x0202).to_bytes(2, "little")
    msg = hdr + body
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=1) as s:
            s.sendall(len(msg).to_bytes(4, "big") + msg)
            return s.recv(8)[4:8] == b"\xfeSMB"
    except OSError:
        return False


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class SambaServer:
    USER = USER
    PASSWORD = PASSWORD

    def __init__(self, min_protocol="SMB2_02", max_protocol="SMB3_11",
                 signing="mandatory", port=None):
        self.min, self.max, self.signing = min_protocol, max_protocol, signing
        self.port = port or free_port()
        self.name = f"saunaos-samba-{os.getpid()}-{self.port}"
        self.dir = None
        self.files = {}

    def sha256(self, rel):
        return hashlib.sha256(self.files[rel]).hexdigest()

    def listing(self, rel_dir):
        """{name: size} for the entries directly under rel_dir (files and
        dirs), each name as the client spells it (wire_name(), bytes)."""
        base = os.path.join(self.dir, rel_dir)
        out = {}
        for n in os.listdir(base):
            p = os.path.join(base, n)
            out[wire_name(n)] = None if os.path.isdir(p) else os.path.getsize(p)
        return out

    def __enter__(self):
        ensure_image()
        self.dir = tempfile.mkdtemp(prefix="saunaos-samba-")
        os.chmod(self.dir, 0o755)
        self.files = make_files(self.dir)
        conf = os.path.join(self.dir, "smb.conf")
        with open(conf, "w") as fh:
            fh.write(SMB_CONF.format(min=self.min, max=self.max,
                                     signing=self.signing, user=USER))
        start = (f"useradd -M -s /usr/sbin/nologin {USER} && "
                 f"printf '%s\\n%s\\n' '{PASSWORD}' '{PASSWORD}' | smbpasswd -a -s {USER} >/dev/null && "
                 "exec smbd --foreground --no-process-group --debug-stdout -s /etc/samba/smb.conf")
        r = sh(["docker", "run", "-d", "--rm", "--name", self.name,
                "-p", f"127.0.0.1:{self.port}:445",
                "-v", f"{self.dir}/media:/srv/media:ro",
                "-v", f"{self.dir}/public:/srv/public:ro",
                "-v", f"{conf}:/etc/samba/smb.conf:ro",
                IMAGE, "sh", "-c", start])
        if r.returncode:
            self._cleanup_dir()
            sys.exit(f"samba_server: docker run failed:\n{r.stderr}")
        self._wait_ready()
        return self

    def _wait_ready(self):
        deadline = time.time() + 30
        while time.time() < deadline:
            # Docker's proxy accepts TCP before smbd listens, so a connect
            # proves nothing: a NEGOTIATE answered is the signal.
            if negotiate_answered(self.port):
                return
            if sh(["docker", "inspect", "-f", "{{.State.Running}}", self.name]).stdout.strip() != "true":
                break
            time.sleep(0.3)
        log = sh(["docker", "logs", self.name])
        self.__exit__(None, None, None)
        sys.exit(f"samba_server: smbd did not start:\n{log.stdout[-2000:]}{log.stderr[-2000:]}")

    def log(self):
        return sh(["docker", "logs", self.name]).stdout

    def _cleanup_dir(self):
        if self.dir:
            shutil.rmtree(self.dir, ignore_errors=True)
            self.dir = None

    def __exit__(self, *exc):
        sh(["docker", "rm", "-f", self.name])
        self._cleanup_dir()
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--serve", action="store_true", help="run one server until Ctrl-C")
    ap.add_argument("--min", default="SMB2_02", choices=DIALECTS)
    ap.add_argument("--max", default="SMB3_11", choices=DIALECTS)
    ap.add_argument("--signing", default="mandatory", choices=("mandatory", "auto", "disabled"))
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--build", action="store_true", help="rebuild the image")
    args = ap.parse_args()
    if args.build:
        ensure_image(rebuild=True)
    if not args.serve:
        return
    # A SIGTERM (a `kill`, a `timeout`) stops it as Ctrl-C does, so the
    # container is removed rather than left running.
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt))
    with SambaServer(args.min, args.max, args.signing, args.port or None) as srv:
        print(f"serving on 127.0.0.1:{srv.port} (guest: 10.0.2.2:{srv.port}) "
              f"user {USER!r} password {PASSWORD!r}; shares media, public, sealed")
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
