#!/usr/bin/env python3
"""A machine installed as toy-os keeps its settings and log when it boots
as SaunaOS.

The kernel reads /etc/saunaos.conf at INIT_CONFIG, so a machine whose
settings are still in /etc/toyos.conf would boot with none of them; the
kernel moves the old names to the new ones right after the root mount
(vfs.c, rename_from_toy_os()). Only a boot can show that, so this:

  1. boots a COPY of disk.img, saves a setting, and gives the settings
     file and the log their old names -- the state an installed machine
     is in when the renamed build first boots;
  2. reboots, and checks the files have the new names and the setting
     reads back;
  3. writes an /etc/toyos.conf again while /etc/saunaos.conf exists,
     reboots, and checks NEITHER was touched: a move never replaces.

    python3 tools/rename_migrate_test.py [--instance N]
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from harness import copy_disk  # noqa: E402

VM = os.path.join(HERE, "vm.py")
HOST = "migratedbox"


class Result:
    def __init__(self):
        self.fails = 0

    def check(self, what, ok, detail=""):
        print(f"  {'ok  ' if ok else 'FAIL'}  {what}"
              + (f"\n          {detail}" if detail and not ok else ""))
        self.fails += 0 if ok else 1


def vm(inst, *args, timeout=180):
    p = subprocess.run([sys.executable, VM, "--instance", str(inst), *args], cwd=REPO,
                       capture_output=True, text=True, timeout=timeout)
    return p.stdout + p.stderr


def section(out, cmd):
    """What `cmd` printed: vm.py exec heads each command's output with
    `--- cmd ---`."""
    head = f"--- {cmd} ---\n"
    if head not in out:
        return ""
    return out.split(head, 1)[1].split("\n--- ", 1)[0]


def reboot(inst):
    """vm.py's own reboot: it waits for the NEW boot, where a poll of the
    console right after `reboot` is still answered by the old one."""
    return "rebooted and ready" in vm(inst, "reboot", timeout=300)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--instance", type=int, default=0)
    inst = ap.parse_args().instance
    r = Result()
    tmp = tempfile.mkdtemp(prefix="rename_migrate_test_")
    disk = os.path.join(tmp, "disk.img")
    copy_disk(os.path.join(REPO, "disk.img"), disk)
    try:
        out = vm(inst, "--disk", disk, "--reboot", "start", timeout=300)
        if "ready" not in out:
            print(f"rename_migrate_test: guest did not boot\n{out}")
            return 1
        vm(inst, "exec", f"config set system.hostname {HOST}")
        # logd stopped first: running, it recreates saunaos.log behind the
        # move, and a machine with both names keeps both.
        old = vm(inst, "exec", "service stop logd", "mv /etc/saunaos.conf /etc/toyos.conf",
                 "mv /var/log/saunaos.log /var/log/toyos.log",
                 "cp /var/log/toyos.log /var/log/toyos.log.1", "ls /etc", "ls /var/log")
        r.check("the fixture: settings and logs under the OLD names only",
                "toyos.conf" in old and "saunaos.conf" not in old.split("ls /etc")[-1]
                and "toyos.log.1" in old, old[-400:])

        r.check("the machine came back", reboot(inst))
        after = vm(inst, "exec", "ls /etc", "ls /var/log", "config get system.hostname", "dmesg")
        etc, log = section(after, "ls /etc"), section(after, "ls /var/log")
        r.check("/etc/toyos.conf became /etc/saunaos.conf",
                "saunaos.conf" in etc and "toyos.conf" not in etc, etc)
        moves = [ln for ln in section(after, "dmesg").splitlines() if "vfs: moved" in ln]
        r.check("the kernel moved all three, the log's rotation too (logd rotates it again after)",
                len(moves) == 3 and "saunaos.log" in log and "toyos.log" not in log,
                "\n".join(moves) + "\n" + log)
        r.check("the setting saved under the old name reads back",
                section(after, "config get system.hostname").strip() == HOST,
                section(after, "config get system.hostname"))

        vm(inst, "exec", "cp /etc/saunaos.conf /etc/toyos.conf",
           "config set system.hostname keptbox")
        r.check("the machine came back again", reboot(inst))
        both = vm(inst, "exec", "ls /etc", "config get system.hostname", "cat /etc/toyos.conf")
        etc = section(both, "ls /etc")
        r.check("with both names present, neither is touched",
                "toyos.conf" in etc and "saunaos.conf" in etc
                and section(both, "config get system.hostname").strip() == "keptbox"
                and HOST in section(both, "cat /etc/toyos.conf"), both[-500:])
    finally:
        vm(inst, "stop")
        shutil.rmtree(tmp, ignore_errors=True)

    print(f"\nrename_migrate_test: {'PASSED' if not r.fails else f'{r.fails} FAILED'}")
    return 1 if r.fails else 0


if __name__ == "__main__":
    sys.exit(main())
