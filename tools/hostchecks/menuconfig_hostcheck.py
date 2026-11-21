#!/usr/bin/env python3
"""Check `make menuconfig` (tools/build/menuconfig.py) on the host.

    python3 tools/hostchecks/menuconfig_hostcheck.py
    python3 tools/hostchecks/menuconfig_hostcheck.py --positive-control   # must FAIL

Every check runs on a COPY of build.conf in a scratch folder, never the
checkout's own.

  MODEL: every build_conf.py option is in exactly one group and has help
         from build.conf's comment block; a refused value is refused; a
         local save writes only build.local.conf, drops a value equal to
         build.conf's, and removes an emptied file; `S` changes exactly the
         changed lines of build.conf -- every comment byte for byte -- adds
         a line only for what the file did not set, and removes the local
         file; build_conf.py then reads both files as the model says.
  SCREEN: the real curses UI in a pseudo-terminal, driven by keys: Down
         to Debugging, Tab, Down to kasan, Space, s, q must leave
         `option kasan = yes` in build.local.conf and build.conf untouched;
         then Space, S, y on the same line must put `yes` into build.conf's
         kasan line and remove the local file.

The positive control makes the local save write build.conf instead --
the accident this design exists to prevent -- and expects red.
"""
import argparse
import os
import pty
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401

RUNNER = {
    "covers": "menuconfig saves to build.local.conf, writes build.conf only on S, and keeps its comments",
    "gate": True,
}

REPO = toolpath.REPO
sys.path.insert(0, os.path.join(REPO, "tools", "build"))
import build_conf as bc  # noqa: E402
import menuconfig as mc  # noqa: E402
sys.path.insert(0, os.path.join(REPO, "tools", "dev"))
import update_server as us  # noqa: E402

FAILS = []


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        FAILS.append(what)


def scratch():
    d = tempfile.mkdtemp(prefix="menuconfig-")
    base = os.path.join(d, "build.conf")
    shutil.copy(os.path.join(REPO, "build.conf"), base)
    return d, base, os.path.join(d, "build.local.conf")


def model_checks():
    d, base, local = scratch()
    try:
        model_body(d, base, local)
    finally:
        shutil.rmtree(d, ignore_errors=True)


def model_body(d, base, local):
    grouped = [n for _t, names in bc.GROUPS for n in names]
    check(sorted(grouped) == sorted(bc.OPTIONS), "every option is in exactly one group")
    original = open(base).read()
    m = mc.Model(base, local)
    check(all(m.help.get(n, (None, []))[1] for n in bc.OPTIONS),
          "every option has help from build.conf's comment block")
    check(m.set(("option", "hz"), "333") is not None, "a refused value is refused (hz = 333)")
    check(m.set(("option", "kasan"), "yes") is None, "kasan = yes is accepted")
    check(m.set(("driver", "e1000"), "builtin") is None, "a driver can go back to builtin")
    check(m.save_local() is None, "the local save succeeds")
    check(open(base).read() == original, "a local save leaves build.conf byte for byte")
    text = open(local).read() if os.path.exists(local) else ""
    check("option kasan = yes" in text and "e1000 = builtin" in text,
          "build.local.conf holds both changes")
    opts, drv = bc.load(base, local)
    check(opts["kasan"][0] == "yes" and drv["e1000"][0] == "builtin",
          "build_conf.py reads the overrides on top")
    # Back to build.conf's value: it drops out, and the file goes when empty.
    m.set(("option", "kasan"), "no")
    m.set(("driver", "e1000"), "module")
    m.save_local()
    check(not os.path.exists(local), "a local file with nothing left in it is removed")
    # S: into build.conf, in place.
    m.set(("option", "kasan"), "yes")
    m.set(("option", "grub_timeout"), "3")   # not set in the file: a new line
    m.save_local()
    m.set(("option", "hz"), "250")           # unsaved, promoted too
    check(m.promote() is None, "S succeeds")
    new = open(base).read()
    old_lines, new_lines = original.split("\n"), new.split("\n")
    import difflib
    changed = [ln for ln in difflib.ndiff(old_lines, new_lines) if ln.startswith("- ")]
    added = [ln for ln in new_lines if ln not in old_lines]
    check(len(new_lines) == len(old_lines) + 1, "S added exactly one line (grub_timeout)")
    check(any(ln.strip() == "option grub_timeout = 3" for ln in added), "...and it is grub_timeout's")
    kasan = [ln for ln in new_lines if ln.startswith("option kasan")]
    check(kasan == ["option kasan    = yes"], "kasan's line keeps its spacing, value changed")
    check([ln for ln in new_lines if ln.startswith("option hz")] == ["option hz       = 250"],
          "an unsaved change is promoted too")
    comments_old = [ln for ln in old_lines if ln.startswith("#") or not ln.strip()]
    comments_new = [ln for ln in new_lines if ln.startswith("#") or not ln.strip()]
    check(comments_old == comments_new, "every comment and blank line kept")
    check(not os.path.exists(local), "S removes build.local.conf")
    opts, _drv = bc.load(base, local)
    check(opts["kasan"][0] == "yes" and opts["hz"][0] == "250" and opts["grub_timeout"][0] == "3",
          "build_conf.py reads build.conf as S wrote it")
    check(len(changed) == 2, "S changed exactly two existing lines (kasan, hz)")
    # A local override set back to build.conf's value, then S: nothing
    # written, the override gone -- not left behind the screen's `no`.
    m = mc.Model(base, local)
    m.set(("option", "ubsan"), "yes")
    m.save_local()
    before = open(base).read()
    m.set(("option", "ubsan"), "no")
    check(not m.changes() and m.promote() is None, "S after reverting an override succeeds")
    check(open(base).read() == before and not os.path.exists(local),
          "...writes nothing into build.conf and removes the override")
    # An override equal to build.conf's value is not one (`off` against `no`).
    with open(local, "w") as fh:
        fh.write("option ubsan = off\n")
    o, dr = bc.load(base, local)
    check(bc.overrides(base, o, dr, local) == [], "a local line saying build.conf's value is no override")
    os.remove(local)
    m = mc.Model(base, local)
    check(m.set(("option", "cmdline"), "") is None and not m.pending,
          "an empty answer leaves an unset option unset")


def round2_checks():
    """Profiles, the map kind, the module check and the boot-word picker's
    logic -- profiles written into a scratch configs/, never the tree's."""
    d, base, local = scratch()
    saved_profiles = bc.PROFILES
    try:
        bc.PROFILES = os.path.join(d, "configs")
        os.makedirs(bc.PROFILES)
        for name in ("release", "debug"):
            shutil.copy(os.path.join(REPO, "configs", name + ".conf"), bc.PROFILES)
        check(bc.option_value("x", 0, "kdebug", "serial") == ("KDEBUG", "1") and
              bc.option_value("x", 0, "kdebug", "off") == ("KDEBUG", ""), "kdebug maps its words")
        with open(local, "w") as fh:
            fh.write("option kasan = yes\n")
        o, _d = bc.load(base, local, bc.profile_path("release"))
        check(o["debugcon"][0] == "no" and o["kasan"][0] == "no",
              "a profile is read on top of build.conf, and build.local.conf is not")
        os.remove(local)
        stamp = os.path.join(d, "seed-overrides")
        cases = [("# profile: release\n", False), ("# profile: debug\n", True),
                 ("# kdebug: 1\n", True),
                 ("# build.local.conf overrides: kasan = yes\n", True), ("", False)]
        for text, refused in cases:
            with open(stamp, "w") as fh:
                fh.write(text)
            got = us._local_conf_reason(stamp) is not None
            check(got == refused, f"publish {'refuses' if refused else 'takes'} {text.strip() or 'build.conf alone'}")
        m = mc.Model(base, local)
        check(m.load_profile("debug") is None and m.value(("option", "kasan")) == "yes"
              and ("option", "kasan") in m.pending, "loading a profile makes its lines unsaved changes")
        with open(os.path.join(bc.PROFILES, "bad.conf"), "w") as fh:
            fh.write("option ubsan = yes\noption hz = abc\n")
        with open(os.path.join(bc.PROFILES, "broke.conf"), "w") as fh:
            fh.write("this line has no equals sign\n")
        with open(os.path.join(bc.PROFILES, "Stray.conf"), "w") as fh:
            fh.write("option ubsan = yes\n")
        check("Stray" not in bc.profiles(), "a file no profile may be named is not offered")
        groups = dict(mc.build_groups(m))
        check(any(i.key == ("broken", "broke") for i in groups["Profiles"]),
              "a broken profile shows as broken rather than stopping the UI")
        m.pending.clear()
        check(m.load_profile("bad") is not None and not m.pending, "a broken profile loads nothing")
        m.load_profile("debug")
        check(m.write_profile("mine", "A test profile.") is None, "w writes the changes as a profile")
        o, _d = bc.load(base, profile=bc.profile_path("mine"))
        check(o["ubsan"][0] == "yes" and o["debug_preempt"][0] == "yes", "...which build_conf.py reads back")
        # The module check, on the build's own objects when there are any.
        need, obj = m.module_check("block_ram")
        if need is None:
            print(f"  skip  module check: {os.path.relpath(obj, REPO)} not built")
        else:
            check("blk_register" in need, "block_ram needs blk_register exported to be a module")
            check(m.module_check("e1000")[0] in ([], None), "e1000, a module today, imports only exports")
        rows = mc.boot_words()
        check(len(rows) >= 50, f"the picker reads docs/boot-flags.md's table ({len(rows)} rows)")
        smp = next(r for r in rows if r.key == "smp")
        plain = next(r for r in rows if r.forms == ["nokaslr"])
        words = ["video=1920x1080"]
        words = mc.toggle_word(None, plain, words)
        check(words == ["video=1920x1080", "nokaslr"], "a plain word toggles on")
        words = mc.toggle_word(None, plain, words)
        check(words == ["video=1920x1080"], "...and off, the rest kept")
        seq = []
        for _ in range(3):
            words = mc.toggle_word(None, smp, words)
            seq.append([w for w in words if w.startswith("smp")])
        check(seq == [["smp=run"], ["smp=park"], []], "a choice steps run, park, then off")
    finally:
        bc.PROFILES = saved_profiles
        shutil.rmtree(d, ignore_errors=True)


def drive(base, local, keys, cols=110, rows=32, timeout=15):
    """Run the UI in a pty, send `keys` (a list of byte strings) one at a
    time once it has drawn, and wait for it to exit."""
    pid, fd = pty.fork()
    if pid == 0:
        try:
            os.environ["TERM"] = "xterm-256color"
            os.execvp(sys.executable, [sys.executable, os.path.join(REPO, "tools/build/menuconfig.py"),
                                       base, "--local", local])
        finally:
            os._exit(127)   # never run the parent's checks in the child
    import fcntl
    import termios
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    out = b""

    def pump(sec):
        nonlocal out
        end = time.time() + sec
        while time.time() < end:
            r, _w, _x = select.select([fd], [], [], 0.05)
            if r:
                try:
                    out += os.read(fd, 65536)
                except OSError:
                    return False
        return True

    pump(1.0)
    for k in keys:
        os.write(fd, k)
        pump(0.25)
    end = time.time() + timeout
    status = None
    while time.time() < end:
        done, st = os.waitpid(pid, os.WNOHANG)
        if done:
            status = st
            break
        pump(0.1)
    if status is None:
        os.kill(pid, 9)
        os.waitpid(pid, 0)
    os.close(fd)
    return status, out


def screen_checks():
    d, base, local = scratch()
    try:
        screen_body(d, base, local)
    finally:
        shutil.rmtree(d, ignore_errors=True)


def screen_body(d, base, local):
    original = open(base).read()
    # Down as terminfo says for this TERM: with the keypad on, xterm's is
    # ESC O B, and ESC [ B would read as a bare Esc, which quits.
    down = subprocess.run(["tput", "-T", "xterm-256color", "kcud1"],
                          capture_output=True).stdout or b"\x1bOB"
    tab = b"\t"
    # The groups: Media and boot, Timer, Debugging -> two Downs.
    status, out = drive(base, local, [down, down, tab, down, b" ", b"s", b"q"])
    check(status is not None and os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0,
          "the UI exits cleanly on q")
    check(b"SaunaOS build configuration" in out, "the UI drew its title")
    text = open(local).read() if os.path.exists(local) else ""
    check("option kasan = yes" in text, "Space then s on kasan saved it to build.local.conf")
    check(open(base).read() == original, "...and left build.conf untouched")
    # The picker: cmdline is the third row of the first group.
    # Filtered on the exact word, so a new row mentioning SMP cannot move it.
    status, _out = drive(base, local, [tab, down, down, b"\n", b"/", *[bytes([c]) for c in b"smp=run"],
                                       b"\n", b" ", b"\x1b", b"s", b"q"])
    text = open(local).read() if os.path.exists(local) else ""
    check("option cmdline = smp=run" in text, "the picker's smp=run saved as cmdline")
    os.remove(local)
    status, _out = drive(base, local, [down, down, tab, down, b" ", b"s", b"q"])
    # S from there: the local yes goes into build.conf and the file goes.
    status, out = drive(base, local, [b"S", b"y", b"q"])
    check(status is not None and os.WIFEXITED(status), "the UI exits after S")
    lines = open(base).read().split("\n")
    check([ln for ln in lines if ln.startswith("option kasan")] == ["option kasan    = yes"],
          "S put kasan = yes into build.conf's own line")
    check(not os.path.exists(local), "S removed build.local.conf")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--positive-control", action="store_true",
                    help="make the local save write build.conf; the check must go red")
    args = ap.parse_args()
    if args.positive_control:
        mc.Model.save_local = lambda self: (self.promote(), None)[1]
    print("menuconfig_hostcheck: the model")
    model_checks()
    print("menuconfig_hostcheck: profiles, modules, boot words")
    round2_checks()
    if not args.positive_control:
        print("menuconfig_hostcheck: the screen, in a pseudo-terminal")
        screen_checks()
    if FAILS:
        print(f"menuconfig_hostcheck: FAIL -- {len(FAILS)} check(s)")
        return 1
    print("menuconfig_hostcheck: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
