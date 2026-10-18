#!/usr/bin/env python3
"""System Update's channels: one server address, the server's own list.

WHAT IT CHECKS, and what a broken version would still pass:

  - an address from before the list (`.../stable`) is SPLIT into the
    server and its channel when the window checks -- read back from
    /etc/update.conf, not from the window;
  - the check lands on Main ("channel main of 3" in the app's own line),
    and Change channel... opens a dialog with all three rows -- Dev too,
    because this is a -dev build -- DRAWN: the rows' rect changed against
    a screenshot taken before the dialog, with the window's title bar as
    the control that must not;
  - choosing Release, whose build is OLDER, asks before anything: the
    question names it, and Wait leaves the machine's file as it was while
    the view says it is waiting -- a window that went back without asking
    passes neither;
  - Go back then installs it, and `sum` reads the older file;
  - each channel row's build has its TIME and a newer/this build/older
    badge against this machine (Release's is older);
  - Change server... is a dialog: Test finds the server's three channels
    without touching /etc/update.conf, reports an address nothing answers,
    Use refuses an ftp:// address with the dialog left up, and a recent
    row plus Use saves and checks it.

The server is tools/update_server.py's handler in this process, three
channels on an ephemeral loopback port the guest reaches as 10.0.2.2:
nothing leaves this machine. update_test.py covers the same rules
through /bin/update.

    python3 tools/sysupdate_channels_test.py [--instance N]
"""
import argparse
import http.server
import os
import re
import sys
import tempfile
import threading
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from gui_debug import DebugConsole, enter_gui   # noqa: E402
from qmp_test import QMPSession                 # noqa: E402
import port_guard                               # noqa: E402
import update_server                            # noqa: E402
from harness import Results                     # noqa: E402
from update_test import Older                   # noqa: E402

APP = "/bin/wm/system/sysupdate"
TITLE = "System Update"
V_INSTALLED, V_WAITING = 4, 9

_res = Results()
check = _res.check


def serve(tmp):
    manifest = update_server.Manifest(os.path.join(os.path.dirname(HERE), "seed", "sync"))
    sources = {"dev": manifest, "main": manifest, "release": Older(manifest, tmp)}
    prefixes = {f"/{k}": v for k, v in sources.items()}
    prefixes["/stable"] = manifest
    base = update_server.make_handler(prefixes, default="main", declared=sources)

    class Quiet(base):
        def _say(self, *a):
            pass

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Quiet)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


LINES = []


def logs(dbg):
    LINES.extend(dbg.logs("sysupdate:", clear=True))
    return LINES


def wait_for(dbg, pattern, since, timeout=120):
    deadline = time.time() + timeout
    while time.time() < deadline:
        for ln in logs(dbg)[since:]:
            m = re.search(pattern, ln)
            if m:
                return m
        time.sleep(0.4)
    return None


def layout_rect(dbg, key):
    """Content rect of `key` from the app's own layout lines."""
    want = f"sysupdate: layout {key} "
    for ln in reversed(logs(dbg)):
        if want in ln:
            p = ln.split(want, 1)[1].split()
            if len(p) >= 4 and all(v.lstrip("-").isdigit() for v in p[:4]):
                return tuple(int(v) for v in p[:4])
    return None


def summed(dbg, path):
    for line in dbg.send(f"sh sum {path}").splitlines():
        f = line.split()
        if len(f) == 3 and f[2] == path and f[0].isdigit():
            return int(f[0]), int(f[1])
    return None


def type_text(dbg, text):
    """Replaces the focused field's text: Ctrl+A (its control code, as a
    keyboard sends it), then each character."""
    dbg.key("0x01")
    for ch in text:
        dbg.key(ch if ch.isalnum() else f"0x{ord(ch):02x}", settle=False)
    dbg.settle()


def press(dbg, cx, cy, key):
    r = layout_rect(dbg, key)
    if r:
        dbg.click(cx + r[0] + r[2] // 2, cy + r[1] + r[3] // 2)
    return r


def server_dialog(dbg, qmp, out_dir, base, cx, cy):
    """Change server...: a dialog with the recent rows; Test asks without
    saving; a bad address is refused with the dialog left up; a row and
    Use save it and check."""
    from PIL import Image
    w = dbg.widgets(TITLE)
    btn = w.get("change")
    check("the window has a Change server... button", btn is not None)
    if not btn:
        return
    shut = os.path.join(out_dir, "sysupd_srv_closed.png")
    qmp.screenshot(shut)   # the same boxes with no dialog over them
    mark = len(LINES)
    dbg.click(btn["screen"]["x"] + btn["w"] // 2, btn["screen"]["y"] + btn["h"] // 2)
    m = wait_for(dbg, r"sysupdate: server dialog, (\d+) rows", mark, timeout=20)
    check("Change server... opens a dialog listing the recent servers",
          m is not None and int(m.group(1)) >= 1, m and m.group(0))
    dbg.settle()
    logs(dbg)
    result = layout_rect(dbg, "testresult")
    if not (result and layout_rect(dbg, "test") and layout_rect(dbg, "servers.row 0")):
        check("...with the field, Test, the rows and the result line laid out", False)
        return
    box = (cx + result[0], cy + result[1], cx + result[0] + result[2], cy + result[1] + result[3])
    shot0 = os.path.join(out_dir, "sysupd_srv_open.png")
    qmp.screenshot(shot0)

    conf0 = dbg.send("sh cat /etc/update.conf")
    mark = len(LINES)
    press(dbg, cx, cy, "test")
    m = wait_for(dbg, r"sysupdate: test (\S+) rc (-?\d+) channels (\d+)", mark, timeout=30)
    check("Test reaches the server and finds its three channels",
          m is not None and m.group(1) == base and m.group(2) == "1" and m.group(3) == "3", m and m.group(0))
    dbg.settle()
    shot1 = os.path.join(out_dir, "sysupd_srv_tested.png")
    qmp.screenshot(shot1)
    a, b = Image.open(shot0).convert("RGB"), Image.open(shot1).convert("RGB")
    check("...and the result is DRAWN (the result line changed)",
          a.crop(box).tobytes() != b.crop(box).tobytes())
    check("...without saving anything", dbg.send("sh cat /etc/update.conf") == conf0)

    press(dbg, cx, cy, "serveredit")
    type_text(dbg, "http://10.0.2.2:9")
    mark = len(LINES)
    press(dbg, cx, cy, "test")
    m = wait_for(dbg, r"sysupdate: test (\S+) rc (-?\d+)", mark, timeout=60)
    check("Test of an address nothing answers says so",
          m is not None and m.group(1) == "http://10.0.2.2:9" and m.group(2) == "-1", m and m.group(0))

    type_text(dbg, "ftp://example")
    mark = len(LINES)
    press(dbg, cx, cy, "dialog.button 1")
    m = wait_for(dbg, r"sysupdate: server refused", mark, timeout=10)
    dbg.settle()
    # PIXELS, not the layout log (it repeats nothing unchanged): the result
    # line differs from that box with no dialog up AND from Test's text.
    refused = os.path.join(out_dir, "sysupd_srv_refused.png")
    qmp.screenshot(refused)
    c = Image.open(refused).convert("RGB").crop(box).tobytes()
    up = c != Image.open(shut).convert("RGB").crop(box).tobytes() and c != b.crop(box).tobytes()
    check("Use refuses an address that is not http(s), and the dialog stays up saying so",
          m is not None and up, m and m.group(0))
    check("...saving nothing", dbg.send("sh cat /etc/update.conf") == conf0)

    mark = len(LINES)
    press(dbg, cx, cy, "servers.row 0")
    press(dbg, cx, cy, "dialog.button 1")
    m = wait_for(dbg, r"sysupdate: server (\S+)$", mark, timeout=10)
    check("a recent row and Use save that server", m is not None and m.group(1) == base, m and m.group(0))
    m = wait_for(dbg, r"sysupdate: view (\d+)", mark, timeout=120)
    check("...and check it", m is not None, m and m.group(0))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    port_guard.add_instance_args(ap)
    ap.add_argument("--in-gui", action="store_true")
    ap.add_argument("--logs", default=None, help="directory for screenshots")
    args = ap.parse_args()
    port_guard.resolve_instance(args, "sysupdate_channels_test")

    why = update_server._stale_reason(os.path.join(os.path.dirname(HERE), "seed", "sync"))
    if why:
        check("the staging tree matches the build", False, f"{why} -- run `make iso`")
        return _res.finish("sysupdate_channels_test")

    tmp = tempfile.mkdtemp(prefix="sysupd-ch-")
    out_dir = args.logs or tmp
    srv = serve(tmp)
    try:
        qmp = QMPSession(port=args.qmp_port)
        if not args.in_gui:
            enter_gui(qmp, args.sock)
        dbg = DebugConsole(args.sock)
        print("System Update channels (userland/update/upd.c, uui_choicelist)")
        base = f"http://10.0.2.2:{srv.server_address[1]}"
        dbg.send(f"sh update --server {base}/stable")
        dbg.send("sh update --channel default")
        before = summed(dbg, Older.PATH)
        logs(dbg)
        mark = len(LINES)

        dbg.spawn(APP, TITLE)
        m = wait_for(dbg, r"sysupdate: channel (\S+) of (\d+), behind (\d)", mark)
        check("the check lands on Main, of the server's three channels",
              m is not None and m.group(1) == "main" and m.group(2) == "3", m and m.group(0))
        conf = dbg.send("sh cat /etc/update.conf")
        check("the old .../stable address is split into the server and its channel",
              re.search(rf"server={re.escape(base)}\s", conf + "\n") is not None
              and "channel=main" in conf, conf.strip()[-160:])

        from PIL import Image
        dbg.settle()
        w = dbg.widgets(TITLE)
        win = dbg.window(TITLE)
        cx, cy = win["content"]["x"], win["content"]["y"]
        shot0 = os.path.join(out_dir, "sysupd_ch_before.png")
        qmp.screenshot(shot0)

        mark = len(LINES)
        btn = w.get("changechannel")
        check("the window has a Change channel... button", btn is not None)
        if not btn:
            return _res.finish("sysupdate_channels_test")
        dbg.click(btn["screen"]["x"] + btn["w"] // 2, btn["screen"]["y"] + btn["h"] // 2)
        m = wait_for(dbg, r"sysupdate: channel dialog, (\d+) rows", mark, timeout=20)
        check("Change channel... opens the list, Dev included on a -dev build",
              m is not None and m.group(1) == "3", m and m.group(0))
        rows = {}
        for ln in logs(dbg):
            r = re.search(r"sysupdate: channel row \d+ (\S+) '([^']*)' (.+)$", ln)
            if r:
                rows[r.group(1)] = (r.group(2), r.group(3).strip())
        check("each row's build carries its TIME, not the date alone",
              len(rows) == 3 and all(re.search(r"\d{4}-\d\d-\d\d \d\d:\d\d$", v) for v, _ in rows.values()),
              str(rows))
        check("...and says how it stands against this machine: Release older, Main not",
              rows.get("release", ("", ""))[1] == "older"
              and rows.get("main", ("", ""))[1] in ("newer", "this build"), str(rows))
        dbg.settle()
        logs(dbg)
        row0 = layout_rect(dbg, "channels.row 0")
        use = layout_rect(dbg, "dialog.button 1")
        check("...with the rows and the dialog's buttons where the app says",
              row0 is not None and use is not None, f"row0 {row0} use {use}")
        if not (row0 and use):
            return _res.finish("sysupdate_channels_test")
        shot1 = os.path.join(out_dir, "sysupd_ch_dialog.png")
        qmp.screenshot(shot1)
        a, b = Image.open(shot0).convert("RGB"), Image.open(shot1).convert("RGB")
        box = (cx + row0[0] + 2, cy + row0[1] + 2, cx + row0[0] + row0[2] - 2, cy + row0[1] + row0[3] - 2)
        chrome = (win["x"] + 4, win["y"] + 4, win["x"] + win["w"] - 120, cy - 4)
        check("the rows are DRAWN (the first row's rect changed)",
              list(a.crop(box).getdata()) != list(b.crop(box).getdata()))
        check("...and the title bar did not (the control)",
              list(a.crop(chrome).getdata()) == list(b.crop(chrome).getdata()))

        # Release is the first row; then Use.
        mark = len(LINES)
        dbg.click(cx + row0[0] + row0[2] // 2, cy + row0[1] + row0[3] // 2)
        dbg.click(cx + use[0] + use[2] // 2, cy + use[1] + use[3] // 2)
        m = wait_for(dbg, r"sysupdate: (\S+) is behind this machine", mark, timeout=20)
        check("choosing Release, which is OLDER, asks first", m is not None and m.group(1) == "release",
              m and m.group(0))
        dbg.settle()
        logs(dbg)
        qmp.screenshot(os.path.join(out_dir, "sysupd_ch_behind.png"))
        wait_btn = layout_rect(dbg, "dialog.button 1")
        if not wait_btn:
            check("...with Wait and Go back to answer", False)
            return _res.finish("sysupdate_channels_test")
        mark = len(LINES)
        dbg.click(cx + wait_btn[0] + wait_btn[2] // 2, cy + wait_btn[1] + wait_btn[3] // 2)
        m = wait_for(dbg, r"sysupdate: view (\d+) changed (\d+)", mark)
        check("Wait checks Release and says it is waiting", m is not None and int(m.group(1)) == V_WAITING,
              m and m.group(0))
        check("...leaving the machine's file as it was", summed(dbg, Older.PATH) == before)

        dbg.settle()
        qmp.screenshot(os.path.join(out_dir, "sysupd_ch_waiting.png"))
        w = dbg.widgets(TITLE)
        main_btn = w.get("main")
        mark = len(LINES)
        if main_btn:
            dbg.click(main_btn["screen"]["x"] + main_btn["w"] // 2,
                      main_btn["screen"]["y"] + main_btn["h"] // 2)
        m = wait_for(dbg, r"sysupdate: view (\d+) changed (\d+)", mark)
        check("Go back installs Release's build", m is not None and int(m.group(1)) == V_INSTALLED,
              m and m.group(0))
        check("...and sum reads the older file",
              summed(dbg, Older.PATH) == (zlib.crc32(Older.BODY), len(Older.BODY)))
        server_dialog(dbg, qmp, out_dir, base, cx, cy)
        dbg.close_window(TITLE)
        # Back to Main, so no later tool inherits Release's file.
        dbg.send("sh update --channel main")
        dbg.send("sh update")
        dbg.send("sh update --server http://10.0.2.2:8080")
        dbg.send("sh update --channel default")
    finally:
        srv.shutdown()
    return _res.finish("sysupdate_channels_test")


if __name__ == "__main__":
    sys.exit(main())
