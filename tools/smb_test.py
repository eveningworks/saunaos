#!/usr/bin/env python3
"""Network shares in a guest: /bin/smb, `mount -t smb`, and saved shares under /net.

The protocol is checked against every dialect by tools/smb_hostcheck.py
on the host; this is the half only a guest can show -- the toy-os
transport (the kernel's TCP stack), the userfs bridge between the VFS
and a process, smbfsd's supervisor and workers, and a share that comes
back after a reboot.

NOTHING LEAVES THIS MACHINE. The server is Samba in Docker
(tools/samba_server.py) published on 127.0.0.1, which the guest reaches
as SLIRP's 10.0.2.2.

THE ORACLE IS THE HOST: every file the guest reads was written by the
fixture, and the guest's SHA-256 of what it read is compared with the
host's of what it wrote -- a client that dropped, reordered or
duplicated a chunk cannot pass on a plausible-looking listing.

WHAT A BROKEN BUILD WOULD STILL PASS, and how each check is shaped
against it:
  * a share that lists but serves wrong bytes: the big file (3 MiB, past
    any single read) is hashed end to end, through both paths;
  * a mount that is only ever local: the saved share is read after a
    COLD BOOT, from a state the test first sees as `idle`;
  * a supervisor that never restarts anything: the worker is killed and
    the share must answer again from a new one;
  * a dead server that hangs the caller: the unreachable share must fail
    within a bound, not merely fail.

  python3 tools/smb_test.py [--instance N] [--positive-control]
      (after `make iso`)

--positive-control saves a WRONG password hash for the share, so every
read through /net must fail and the run must go red.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import port_guard  # noqa: E402
import samba_server  # noqa: E402
from harness import Results, copy_disk  # noqa: E402

VM = os.path.join(HERE, "vm.py")
GW = "10.0.2.2"


class Guest:
    def __init__(self, instance, disk):
        self.instance, self.disk = instance, disk

    def vm(self, *args, timeout=300):
        cmd = [sys.executable, VM, "--instance", str(self.instance), "--disk", self.disk,
               "--timeout", str(timeout - 10), *args]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                           errors="replace")
        return r.stdout + r.stderr

    def sh(self, *cmds, timeout=300):
        return self.vm("exec", *cmds, timeout=timeout)

    def mount_state(self, point):
        out = self.sh("mount")
        m = re.search(r"\s" + re.escape(point) + r"\s+smb\s+ro,([^\n]+)", out)
        return m.group(1).strip() if m else None

    def wait_state(self, point, want, timeout_s=20):
        end = time.time() + timeout_s
        st = None
        while time.time() < end:
            st = self.mount_state(point)
            if st in want:
                return st
            time.sleep(1)
        return st


def sha_in_guest(g, path):
    out = g.sh(f"sum -a sha256 {path}")
    m = re.search(r"\b([0-9a-f]{64})\b", out)
    return m.group(1) if m else out[-200:]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--instance", default="auto")
    ap.add_argument("--positive-control", action="store_true")
    args = ap.parse_args()
    inst = port_guard.find_free_instance() if args.instance == "auto" else int(args.instance)
    if inst is None:
        sys.exit("smb_test: no free VM slot")
    print(f"smb_test: VM slot {inst}"
          + ("  [POSITIVE CONTROL -- reads through /net must FAIL]" if args.positive_control else ""))

    res = Results()
    disk_src = os.path.join(REPO, "disk.img")
    tmp = tempfile.mkdtemp(prefix="smbtest.")
    disk = copy_disk(disk_src, os.path.join(tmp, "smb.img"))
    g = Guest(inst, disk)
    with samba_server.SambaServer() as srv:
        url = f"//{GW}:{srv.port}/media"
        cred = f"-u {srv.USER} -p '{srv.PASSWORD}'"
        big = srv.sha256("media/big.bin")
        try:
            out = g.vm("start")
            if "ready" not in out:
                res.check("the guest boots", False, out[-300:])
                return res.finish("smb_test")

            print("phase 1: /bin/smb")
            out = g.sh(f"smb {cred} info {url}")
            res.check("info: SMB 3.1.1, every message signed",
                      "SMB 3.1.1" in out and "every message" in out, out[-300:])
            out = g.sh(f"smb {cred} ls {url}")
            res.check("ls names the share's entries",
                      all(n in out for n in ("hello.txt", "big.bin", "many/", "sub/", "Empty folder/")),
                      out[-400:])
            g.sh(f"smb {cred} get {url}/big.bin /tmp/cli.bin")
            res.check("get: 3 MiB file, byte-exact", sha_in_guest(g, "/tmp/cli.bin") == big)
            out = g.sh(f"smb -u {srv.USER} -p nope info {url}")
            res.check("a wrong password says so", "wrong user name or password" in out, out[-200:])
            out = g.sh(f"smb -u guest info //{GW}:{srv.port}/public")
            res.check("guest vs required signing says so", "requires signing" in out, out[-200:])

            print("phase 2: mount -t smb")
            g.sh(f"mount -t smb -o user={srv.USER} {url} /mnt")
            res.check("mounted, waiting for a password",
                      g.mount_state("/mnt") == "sign-in needed", g.mount_state("/mnt"))
            out = g.sh(f"smb {cred} login /mnt")
            res.check("smb login is accepted", "Signed in" in out, out[-200:])
            res.check("connected after the login", g.mount_state("/mnt") == "connected")
            out = g.sh("cat /mnt/sub/nested/deep.txt")
            res.check("a nested file reads through the mount", "three levels down" in out, out[-200:])
            g.sh("cp /mnt/big.bin /tmp/mnt.bin")
            res.check("cp through the mount: byte-exact", sha_in_guest(g, "/tmp/mnt.bin") == big)
            # 8 KiB reads straight off the mount: the kernel's read-ahead
            # windows (128 KiB growing to 1 MiB) answer most of them.
            res.check("sum read through the mount (read-ahead): byte-exact",
                      sha_in_guest(g, "/mnt/big.bin") == big)
            # Paged (SYS_LISTDIR_AT) by cp -r's walk, as the File Manager
            # pages -- `ls` alone stops at one listing, on every filesystem.
            last = samba_server.MANY - 1
            out = g.sh("cp -r /mnt/many /tmp/many", f"cat /tmp/many/f{last:04d}.txt", timeout=600)
            res.check(f"a {samba_server.MANY}-entry folder is walked to its last entry",
                      re.search(rf"^{last}$", out, re.M) is not None, out[-200:])
            g.sh("umount /mnt")
            res.check("umount ends it", g.mount_state("/mnt") is None)

            g.sh(f"open smb://{GW}:{srv.port}/public")
            st = g.wait_state("/net/public", {"idle", "connected", "connecting", "sign-in needed"}, timeout_s=10)
            res.check("open smb://... mounts the share for the session", st is not None, st)
            out = g.sh("ls /etc/mounts.d")
            res.check("...and saves nothing", "public" not in out.split("---")[-1], out[-200:])
            g.sh("umount /net/public")

            print("phase 3: a saved share")
            g.sh("mkdir /etc/mounts.d", f"write /etc/mounts.d/media Source={url}",
                 f"append /etc/mounts.d/media User={srv.USER}")
            st = g.wait_state("/net/media", {"sign-in needed"})
            res.check("smbfsd mounts it, asking for a password", st == "sign-in needed", st)
            out = g.sh(f"smb {cred} -r login media", "cat /etc/mounts.d/media")
            res.check("Remember saves a hash, never the password",
                      re.search(r"Hash=[0-9a-f]{32}", out) is not None and srv.PASSWORD not in out.split("login media")[-1],
                      out[-300:])
            if args.positive_control:
                g.sh("rm /etc/mounts.d/media", f"write /etc/mounts.d/media Source={url}",
                     f"append /etc/mounts.d/media User={srv.USER}",
                     "append /etc/mounts.d/media Hash=00112233445566778899aabbccddeeff")
                time.sleep(3)

            print("  cold boot")
            g.vm("stop")
            g.vm("start")
            st = g.wait_state("/net/media", {"idle", "sign-in needed", "connected"})
            res.check("after a cold boot it is mounted and idle", st == "idle", st)
            g.sh("cp /net/media/big.bin /tmp/net.bin")
            res.check("read after the boot, no password asked: byte-exact",
                      sha_in_guest(g, "/tmp/net.bin") == big)
            out = g.sh("cat /net/media/hello.txt")
            res.check("a small file reads", "hello from samba" in out, out[-200:])
            res.check("connected", g.mount_state("/net/media") == "connected")

            out = g.sh("ps")
            pids = [int(m.group(1)) for m in re.finditer(r"^\s*(\d+)\s+(\d+)\s+.*smbfsd\s*$", out, re.M)]
            ppids = {int(m.group(1)): int(m.group(2)) for m in re.finditer(r"^\s*(\d+)\s+(\d+)\s+.*smbfsd\s*$", out, re.M)}
            workers = [p for p in pids if ppids.get(p) in pids]
            if workers:
                g.sh(f"kill {workers[0]}")
                st = g.wait_state("/net/media", {"idle", "connected"}, timeout_s=15)
                res.check("a killed worker is replaced", st in ("idle", "connected"), st)
                out = g.sh("cat /net/media/hello.txt")
                res.check("and the share answers again", "hello from samba" in out, out[-200:])
            else:
                res.check("found the worker to kill", False, out[-400:])

            g.sh(f"write /etc/mounts.d/dead Source=//{GW}:1/nothing")
            g.wait_state("/net/dead", {"idle"})
            t0 = time.time()
            g.sh("ls /net/dead")
            took = time.time() - t0
            st = g.mount_state("/net/dead")
            res.check("a server that refuses fails fast, as unreachable",
                      st == "unreachable" and took < 20, f"{st}, {took:.1f}s")
            g.sh("rm /etc/mounts.d/dead")
            time.sleep(4)
            out = g.sh("mount", "ls /net")
            res.check("forgetting a share unmounts it and removes its folder",
                      "/net/dead" not in out and "dead/" not in out, out[-300:])
        finally:
            g.vm("stop")
            subprocess.run(["rm", "-rf", tmp])
    rc = res.finish("smb_test")
    if args.positive_control:
        if rc:
            print("smb_test: positive control went red, as it must")
            return 0
        print("smb_test: POSITIVE CONTROL STAYED GREEN")
        return 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
