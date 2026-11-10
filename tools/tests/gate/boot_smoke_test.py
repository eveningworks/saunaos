#!/usr/bin/env python3
"""Fast, non-GUI boot verification: boots saunaos.iso headlessly in QEMU,
watches the serial log for the expected kernel init sequence, presses the
ACPI power button (QMP `system_powerdown`) and requires the guest to power
itself off, and exits 0/1 -- no mouse/keyboard, no screenshots. Most kernel-level
changes (a new driver, a filesystem backend, a syscall) don't actually
need the full QMP GUI-testing dance (see qmp_test.py) to sanity-check;
they need "does it still boot cleanly to the point the shell/WM takes
over," which is what this answers, in a few seconds instead of a manual
screenshot-reading session.

This does NOT replace qmp_test.py for anything that touches rendering,
input, or window behavior -- a clean boot log says nothing about
whether a button is drawn in the right place. Use this as a fast first
check for kernel/driver-level changes, and qmp_test.py for GUI changes,
same division CLAUDE.md's testing section describes.

Usage:
    python3 tools/tests/gate/boot_smoke_test.py                 # uses saunaos.iso, disk.img
    python3 tools/tests/gate/boot_smoke_test.py --iso other.iso --timeout 15

Exit code 0 = every expected line appeared and no failure pattern did,
within the timeout. Exit code 1 = timed out missing something, or a
failure pattern (a kernel panic, or QEMU exiting/crashing outright)
showed up. Either way, prints a short report; add -v to also dump the
full serial log on failure for debugging.

No third-party dependencies -- doesn't need Pillow (screenshot()'s only
consumer), unlike qmp_test.py.
"""

RUNNER = {"covers": "the built ISO boots cleanly, and powers off on the ACPI power button"}

import argparse
import json
import os
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import install_grub

# Every one of these substrings must appear in the serial log for a
# boot to count as successful. Deliberately loose (substring match, not
# exact-line match) so small wording tweaks to a serial_write() message
# don't spuriously break this -- see kernel/core/kernel.c's
# serial_write() calls for where each of these actually comes from.
PASS_PATTERNS = [
    "kernel_main reached, initializing",
    "GDT/TSS initialized",
    "IDT/PIC/PIT initialized, interrupts enabled",
    "physical frame allocator initialized",
    "fs: ",  # one of "loaded persistent"/"formatted a fresh"/"no disk found" -- all are fine, see fs_init()
    "scheduler initialized",
]

# Any of these appearing anywhere in the serial log fails the test
# immediately, even before the timeout -- no point waiting the full
# window out if the kernel already panicked. "PANIC: " is
# kernel/arch/x86_64/idt.c's unrecoverable-fault message (see
# idt.c's serial_write(recoverable ? "RING-3 CRASH: " : "PANIC: ", ...)
# -- deliberately NOT matching "RING-3 CRASH:" here, since that's the
# *recoverable* per-process fault path (ring3test/elftest trigger it on
# purpose and the kernel keeps running) and this smoke test never runs
# those commands during a plain boot anyway.
FAIL_PATTERNS = [
    "PANIC:",
]

DEFAULT_TIMEOUT = 12.0
# The power button to S5: init stopping every service in reverse order,
# each with its own grace, then the S5 write.
POWERDOWN_TIMEOUT = 30.0
POWERDOWN_LINES = ["acpi: power button pressed", "init: power button pressed", "init: powering off",
                   "splash: shutdown screen on"]
# kernel/include/api/splash_layout.h
SPLASH_BG, SPLASH_PLATE, SPLASH_TEXT = (0x0B, 0x0F, 0x17), (0x28, 0x60, 0xE6), (0xD8, 0xDC, 0xE6)


def read_ppm(path):
    """(width, height, rgb bytes) of QEMU's binary P6 screendump."""
    with open(path, "rb") as f:
        data = f.read()
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            pos = data.index(b"\n", pos) + 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[pos:end])
        pos = end
    w, h = int(fields[1]), int(fields[2])
    return w, h, data[pos + 1:pos + 1 + w * h * 3]


def check_shutdown_screen(path):
    """The frame the machine powered off on is the SHUTDOWN SCREEN, read
    by its pixels where splash_layout() puts them: the splash's ground in
    a corner, the logo's blue plate, and lit text on the line under the
    spinner. The console's log and prompt -- what this replaced -- has
    none of the three. Returns a list of failures."""
    if not os.path.exists(path):
        return ["no screendump was written"]
    w, h, px = read_ppm(path)

    def at(x, y):
        i = (y * w + x) * 3
        return tuple(px[i:i + 3])

    def near(a, b, tol=6):
        return all(abs(p - q) <= tol for p, q in zip(a, b))

    size = max(48, min(192, h * 2 // 15))
    p = size * 56 // 64
    px0, py0 = (w - p) // 2, h * 2 // 5 - p // 2
    spin_r, label_y = size // 7, h * 3 // 4 + size // 7 + size // 5
    bad = []
    if not near(at(4, 4), SPLASH_BG):
        bad.append(f"corner is {at(4, 4)}, not the splash ground {SPLASH_BG}")
    if not near(at(px0 + 2, py0 + p // 2), SPLASH_PLATE):
        bad.append(f"logo plate is {at(px0 + 2, py0 + p // 2)}, not {SPLASH_PLATE}")
    lit = sum(1 for y in range(label_y, min(h, label_y + 20)) for x in range(w // 4, 3 * w // 4)
              if sum(at(x, y)) > sum(SPLASH_BG) + 200)
    if lit < 20:
        bad.append(f"no text on the status line (y={label_y}): {lit} lit pixels")
    return bad
POLL_INTERVAL = 0.2


QMP_SOCK = ".smoke.qmp"


def qmp_powerdown(sock_path, timeout=6.0, screendump=None):
    """Ask the guest to shut down over QMP, and wait for it to say it did.

    WHY THIS TEST SHUTS DOWN CLEANLY. Terminating QEMU is a power cut
    from the guest's side, and tfs3 writes its allocation bitmap
    UNJOURNALED and set-before-use (`flush_alloc_state()`): a crash
    between marking a block allocated and committing the transaction
    that references it LEAKS that block, deliberately, so that it can
    never be double-allocated. `fsck` then reports the debris, and the
    three fs KTESTs asserting `leaked == 0` go red -- which is how every
    `preflight.sh` came to fail a suite this very test had dirtied. A
    unix socket rather than a TCP port so this needs no slot from
    port_guard and cannot clash with another guest.

    Returns True if the guest powered itself down.
    """
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(timeout)
            s.connect(sock_path)
            f = s.makefile("rwb")
            f.readline()                                   # the QMP greeting
            f.write(b'{"execute":"qmp_capabilities"}\n')
            f.flush()
            f.readline()                                   # its return
            f.write(b'{"execute":"system_powerdown"}\n')
            f.flush()
            deadline = time.time() + timeout
            while time.time() < deadline:
                line = f.readline()
                if not line:
                    return False
                if b'"SHUTDOWN"' in line:
                    # -no-shutdown keeps the last frame: the screen the
                    # machine went off on.
                    if screendump:
                        f.write(json.dumps({"execute": "screendump",
                                            "arguments": {"filename": screendump}}).encode() + b"\n")
                        f.flush()
                        while b'"return"' not in line and b'"error"' not in line:
                            line = f.readline()
                            if not line:
                                break
                    return True
    except (OSError, socket.timeout):
        return False
    return False


def launch_qemu(iso, disk, serial_log, qemu_log):
    # -no-reboot -no-shutdown: a triple fault would otherwise just
    # silently reset the VM (or QEMU would exit) with nothing useful in
    # either log -- these two flags make QEMU stop and report instead,
    # so a boot that gets that badly wrong shows up as a clear failure
    # rather than a confusing timeout. No display head at all (unlike
    # qmp_test.py's launch_qemu_cmd(), which needs one for mouse input
    # routing) -- this test never sends QMP input, only reads the
    # serial log, so -display none is fine here (see qmp_test.py's
    # module docstring for why it's NOT fine when QMP input is
    # involved).
    cmd = [
        "qemu-system-x86_64",
        # WHICH MEDIUM: the disk carries GRUB and the kernel now, so it
        # boots itself (`order=c`) and the ISO is only for an image that
        # cannot -- see tools/build/install_grub.py. Never NO order: SeaBIOS
        # boots any disk with 0x55AA at LBA 0, which a partition table
        # provides, and then hangs inside the table with NO serial
        # output at all -- indistinguishable from a kernel that died
        # before its first print.
        *install_grub.qemu_boot_args(install_grub.boot_medium(disk), iso),
        # discard=unmap: the guest's ATA TRIM (kernel/drivers/ata.c) becomes a
        # hole punch in the backing file, so the image stops growing forever.
        "-drive", f"file={disk},format=raw,if=ide,discard=unmap",
        "-m", "2048",
        "-display", "none",
        "-serial", f"file:{serial_log}",
        "-no-reboot",
        "-no-shutdown",
        # Only so the guest can be asked to shut down cleanly at the end
        # -- this test still sends no input and reads only the serial
        # log. See qmp_powerdown().
        "-qmp", f"unix:{QMP_SOCK},server,nowait",
    ]
    qlog = open(qemu_log, "wb")
    return subprocess.Popen(cmd, stdout=qlog, stderr=subprocess.STDOUT)


def check_power_button(args):
    """The ACPI power button, end to end: QMP's system_powerdown presses
    it, the kernel's SCI sends init SIGPWR, init stops everything and
    writes S5, and QEMU reports SHUTDOWN. A guest that ignored the press
    (bugs.md's old `vm.py stop` SIGTERM) fails here, by name."""
    # A press before init has its SIGPWR handler is dropped by design
    # (init cannot die of an unhandled signal), so wait for init's own
    # line rather than the kernel's.
    deadline = time.monotonic() + POWERDOWN_TIMEOUT
    while "init: startup finished" not in tail(args.serial_log):
        if time.monotonic() > deadline:
            print("boot_smoke_test: FAIL -- init never said 'startup finished'", file=sys.stderr)
            return 1
        time.sleep(POLL_INTERVAL)  # sleep-ok: a poll on the serial log for the line it waits for
    t0 = time.monotonic()
    shot = args.serial_log + ".shutdown.ppm"
    if os.path.exists(shot):
        os.remove(shot)
    off = qmp_powerdown(QMP_SOCK, timeout=POWERDOWN_TIMEOUT, screendump=os.path.abspath(shot))
    log = tail(args.serial_log)
    missing = [p for p in POWERDOWN_LINES if p not in log]
    screen = check_shutdown_screen(shot) if off else []
    if off and not missing and not screen:
        print(f"boot_smoke_test: PASS -- and the power button powered it off in "
              f"{time.monotonic() - t0:.1f}s, on the shutdown screen")
        return 0
    if off and not missing:
        print("boot_smoke_test: FAIL -- powered off, but not on the shutdown screen: "
              + "; ".join(screen), file=sys.stderr)
        return 1
    print("boot_smoke_test: FAIL -- the ACPI power button did not power the guest off "
          f"(QEMU SHUTDOWN {'seen' if off else 'not seen'}; missing lines: {missing})",
          file=sys.stderr)
    if args.verbose:
        print(log, file=sys.stderr)
    return 1


def tail(path):
    if not os.path.exists(path):
        return ""
    with open(path, "r", errors="replace") as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", default="saunaos.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    ap.add_argument("--serial-log", default="smoke_serial.log")
    ap.add_argument("--qemu-log", default="smoke_qemu.log")
    ap.add_argument("-v", "--verbose", action="store_true", help="dump full serial log on failure")
    args = ap.parse_args()

    if not os.path.exists(args.iso):
        print(f"boot_smoke_test: {args.iso} not found -- build it first (make iso)", file=sys.stderr)
        return 1

    # A fresh disk each run, same reasoning as `make run`'s $(DISK_IMG)
    # target -- this test always wants a known-clean 1MiB image, not
    # whatever a previous session's disk.img happens to hold.
    if not os.path.exists(args.disk):
        with open(args.disk, "wb") as f:
            f.truncate(1024 * 1024)

    for p in (args.serial_log, args.qemu_log):
        if os.path.exists(p):
            os.remove(p)

    proc = launch_qemu(args.iso, args.disk, args.serial_log, args.qemu_log)

    deadline = time.monotonic() + args.timeout
    seen_fail = None
    try:
        while time.monotonic() < deadline:
            log = tail(args.serial_log)

            for pat in FAIL_PATTERNS:
                if pat in log:
                    seen_fail = pat
                    break
            if seen_fail:
                break

            if all(pat in log for pat in PASS_PATTERNS):
                elapsed = args.timeout - (deadline - time.monotonic())
                print(f"boot_smoke_test: booted -- all {len(PASS_PATTERNS)} expected lines seen in {elapsed:.1f}s")
                return check_power_button(args)

            if proc.poll() is not None:
                # QEMU exited on its own (crash, or -no-reboot/-no-shutdown
                # tripping on a triple fault) before we saw everything --
                # that's a failure, not a "keep waiting" situation.
                print(f"boot_smoke_test: FAIL -- qemu exited early (code {proc.returncode}) before boot completed",
                      file=sys.stderr)
                seen_fail = f"qemu exited early (code {proc.returncode})"
                break

            time.sleep(POLL_INTERVAL)
        else:
            log = tail(args.serial_log)

        if seen_fail:
            print(f"boot_smoke_test: FAIL -- saw failure indicator: {seen_fail}", file=sys.stderr)
        else:
            missing = [p for p in PASS_PATTERNS if p not in log]
            print(f"boot_smoke_test: FAIL -- timed out after {args.timeout}s, missing: {missing}", file=sys.stderr)

        if args.verbose:
            print("----- serial log -----", file=sys.stderr)
            print(log, file=sys.stderr)
            print("----- qemu log -----", file=sys.stderr)
            print(tail(args.qemu_log), file=sys.stderr)
        else:
            print("(rerun with -v to see the full serial/qemu logs)", file=sys.stderr)

        return 1
    finally:
        # CLEAN FIRST, KILL SECOND. -no-shutdown means QEMU stops the VM
        # rather than exiting, so this still terminates afterwards -- the
        # point is that the GUEST completed its flush before it did.
        if proc.poll() is None:
            qmp_powerdown(QMP_SOCK)
        if os.path.exists(QMP_SOCK):
            os.unlink(QMP_SOCK)
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    sys.exit(main())
