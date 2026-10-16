#!/usr/bin/env python3
"""Lay out the same pages with NetSurf on toy-os and on the host, and compare.

WHAT IT CHECKS. Each page in data/tests/web/ (on the image at /tests/web)
is opened in NetSurf's headless monkey front end on BOTH sides -- toy-os's
/tests/nsmonkey in a VM, and the host build tools/netsurf_vendor.py left
behind, from the SAME pinned source -- then redrawn. The two outputs are
compared on what is the engine's real answer:

  - the page's TITLE,
  - its laid-out EXTENT (the last UPDATE_EXTENT: width and height),
  - every PLOT line of one full redraw: each rectangle, line, bitmap and
    text run, with its coordinates and clip.

monkey measures text at a fixed width per character, so no font enters
it: a difference is the port's -- tolibc, the shims, a compiler flag --
never the engine's. That is the stage-1 oracle of docs/browser-design.md.

WHAT IT DOES NOT COMPARE, and why. Status lines ("Fetching, Loading",
"Done (0.8s)") and their count depend on how fast each side ran; POLL
lines are upstream's main loop, which toy-os replaces; pointers differ by
address space. Those are dropped or normalised, and the file: URL prefix
is mapped to one token.

HOW THE GUEST IS PACED. tosh's debug console cannot feed a running
program's stdin, so the guest runs one dash script that pipes each
page's commands into nsmonkey with sleeps between them (--load-s,
--redraw-s). A page that did not finish loading in that time is reported
AS THAT -- "not loaded within N s" -- never as a layout difference.

    python3 tools/browser_hostcheck.py                # every page
    python3 tools/browser_hostcheck.py -k table       # pages matching
    python3 tools/browser_hostcheck.py --show text    # print both streams
    python3 tools/browser_hostcheck.py --js -k script # JavaScript on, both sides

JAVASCRIPT (`--js`) runs both engines with NetSurf's own
`--enable_javascript=1`: Duktape on toy-os (tolibc's maths, its clock,
the -nostdinc build) against the same pinned Duktape on the host. Each
side's monkey is off by default, as upstream's is.

OVER THE NETWORK: `--scheme http` or `https` has the GUEST load the same
pages from tools/web_server.py on this machine (10.0.2.2 through SLIRP;
https with a test CA put in the guest's /etc/ssl/certs) while the host
still reads them as files. Layout does not depend on how the bytes
arrived, so any difference is the fetcher's -- a lost or reordered
chunk, a header mangled, an image that never came.

Needs `make iso` and the host oracle (`tools/netsurf_vendor.py --no-copy`
rebuilds it). Boots its own guest on a sparse copy of disk.img.
"""
import argparse
import difflib
import os
import re
import select
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from netsurf_vendor import DEFAULT_WORK  # noqa: E402
from vm import parse_framed  # noqa: E402
from web_server import WebServer, make_ca, GATEWAY  # noqa: E402

VM = os.path.join(HERE, "vm.py")
PAGES = os.path.join(REPO, "data", "tests", "web")
GUEST_PAGES = "/tests/web"
HOST_TIMEOUT_S = 20
# Pages the HOST cannot be the oracle for, and why. Checked by pixels in
# tools/browser_test.py instead.
HOST_LACKS = {
    "photos.html": "the host build has no libpng or libjpeg (netsurf_vendor.py's flags)",
    "fileref.html": "it names the GUEST's file:///tests/web/bands.png, which the host has not got",
    "oom.html": "it allocates until the engine's memory limit, and the host build has none",
}

ADDR = re.compile(r"0x[0-9a-fA-F]+")


def commands(url):
    return [f"WINDOW NEW {url}", "WINDOW REDRAW 0", "QUIT"]


# ["--enable_javascript=1"] under --js. BEFORE the guest's --weak-entropy:
# NetSurf's option parser takes the word after a bare --flag as its value.
JS_ARGS = []


def host_run(monkey, url):
    """Drives the host build interactively: each command waits for the
    line that says its work is done."""
    env = dict(os.environ, LANGUAGE="en", LANG="C")
    p = subprocess.Popen([monkey] + JS_ARGS, cwd=os.path.dirname(monkey), env=env,
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, text=True, bufsize=1)
    out = []

    def until(marker):
        deadline = time.time() + HOST_TIMEOUT_S
        while time.time() < deadline:
            r, _, _ = select.select([p.stdout], [], [], 0.5)
            if not r:
                continue
            line = p.stdout.readline()
            if not line:
                return False
            out.append(line.rstrip("\n"))
            if marker in line:
                return True
        return False

    until("GENERIC STARTED")
    new, redraw, quit_ = commands(url)
    p.stdin.write(new + "\n")
    p.stdin.flush()
    until("STR Done")
    p.stdin.write(redraw + "\n")
    p.stdin.flush()
    until("REDRAW WIN 0 STOP")
    p.stdin.write(quit_ + "\n")
    p.stdin.flush()
    until("GENERIC FINISHED")
    p.wait(timeout=HOST_TIMEOUT_S)
    return out


def summarise(lines):
    """(title, extent, plot lines) -- the parts that are layout, not timing."""
    title, extent, plots, inside = None, None, [], False
    for line in lines:
        line = ADDR.sub("0x?", line)
        if line.startswith("WINDOW TITLE "):
            title = line
        elif line.startswith("WINDOW UPDATE_EXTENT "):
            extent = line
        elif line.startswith("WINDOW REDRAW WIN 0 START"):
            inside, plots = True, []
        elif line.startswith("WINDOW REDRAW WIN 0 STOP"):
            inside = False
        elif inside and line.startswith("PLOT "):
            plots.append(line)
    return title, extent, plots


def vm(args, *argv, check=True):
    cmd = [sys.executable, VM, "--disk", args.disk, "--instance", str(args.instance)] + list(argv)
    r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
    if check and r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
    return r


def guest_runs(args, names, base):
    """One dash script for every page, then each page's output read back.
    `base` is where the guest reads the pages from."""
    script = []
    for n in names:
        new, redraw, quit_ = commands(f"{base}/{n}")
        script.append(f'(echo "{new}"; sleep {args.load_s}; echo "{redraw}"; '
                      f'sleep {args.redraw_s}; echo "{quit_}") | /tests/nsmonkey {" ".join(JS_ARGS)} --weak-entropy'
                      f' > /tmp/ns-{n}.out')
    with tempfile.NamedTemporaryFile("w", suffix=".sh", delete=False) as f:
        f.write("\n".join(script) + "\n")
        local = f.name
    try:
        vm(args, "put", local, "/tmp/nscheck.sh")
    finally:
        os.unlink(local)
    budget = len(names) * (args.load_s + args.redraw_s + 10) + 30
    vm(args, "--timeout", str(budget), "exec", "dash /tmp/nscheck.sh", check=False)
    outs = {}
    for n in names:
        r = vm(args, "exec", f"readfile /tmp/ns-{n}.out", "--raw", "--label", check=False)
        framed = parse_framed(r.stdout + r.stderr)
        outs[n] = (framed or "").splitlines()
    return outs


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("-k", default="", help="only pages whose name contains this")
    ap.add_argument("--instance", type=int, default=0, help="vm.py slot")
    ap.add_argument("--disk", help="boot this image rather than a copy of disk.img")
    ap.add_argument("--load-s", type=int, default=0,
                    help="seconds a page gets to load in the guest (default 5; 12 for https, "
                         "whose handshakes under TCG cost seconds)")
    ap.add_argument("--redraw-s", type=int, default=3, help="seconds the guest's redraw gets")
    ap.add_argument("--js", action="store_true", help="JavaScript on, on both sides")
    ap.add_argument("--show", metavar="PAGE", help="print both sides' summaries for one page")
    ap.add_argument("--scheme", choices=("file", "http", "https"), default="file",
                    help="how the GUEST reads the pages (the host always reads files)")
    ap.add_argument("--port", type=int, default=18090, help="the page server's port")
    args = ap.parse_args()
    if args.js:
        JS_ARGS.append("--enable_javascript=1")
    if not args.load_s:
        args.load_s = 12 if args.scheme == "https" else 5

    monkey = os.path.join(DEFAULT_WORK, "src", "netsurf", "nsmonkey")
    if not os.access(monkey, os.X_OK):
        print(f"browser_hostcheck: no host oracle at {monkey} -- "
              "run `python3 tools/netsurf_vendor.py --no-copy`")
        return 2
    names = sorted(n for n in os.listdir(PAGES)
                   if n.endswith(".html") and args.k in n and n not in HOST_LACKS)
    if not names:
        print(f"browser_hostcheck: no page matches -k {args.k!r}")
        return 2

    tmp = None
    if not args.disk:
        src = os.path.join(REPO, "disk.img")
        if not os.path.exists(src):
            print("browser_hostcheck: no disk.img -- run `make iso` first")
            return 2
        tmp = tempfile.NamedTemporaryFile(suffix=".img", delete=False)
        tmp.close()
        subprocess.run(["cp", "--reflink=auto", "--sparse=always", src, tmp.name], check=True)
        args.disk = tmp.name

    srv, certdir = None, None
    base = f"file://{GUEST_PAGES}"
    try:
        if args.scheme != "file":
            tls = None
            if args.scheme == "https":
                certdir = tempfile.mkdtemp(prefix="hostcheck-ca.")
                certs = make_ca(certdir)
                tls = certs["good"]
            srv = WebServer(PAGES, args.port, tls=tls)
            base = f"{args.scheme}://{GATEWAY}:{srv.port}"
        if vm(args, "start").returncode != 0:
            print("browser_hostcheck: could not start the VM")
            return 2
        if certdir:
            vm(args, "put", certs["ca"], "/etc/ssl/certs/toyos-test-ca.pem")
        guest = guest_runs(args, names, base)
    finally:
        vm(args, "stop", check=False)
        if srv:
            srv.shutdown()
        if certdir:
            subprocess.run(["rm", "-rf", certdir])
        if tmp:
            os.unlink(tmp.name)

    failed = 0
    for n in names:
        h = summarise(host_run(monkey, f"file://{os.path.join(PAGES, n)}"))
        g_lines = guest[n]
        g = summarise(g_lines)
        if args.show == n:
            for side, s in (("host", h), ("toy-os", g)):
                print(f"--- {side}: {s[0]}\n    {s[1]}")
                print("\n".join("    " + p for p in s[2]))
        # Done BEFORE the redraw began: a page that finished during the
        # redraw was not loaded when it was drawn, and its short plot
        # list would read as a layout difference.
        start = next((i for i, line in enumerate(g_lines)
                      if line.startswith("WINDOW REDRAW WIN 0 START")), len(g_lines))
        if not any("STR Done" in line for line in g_lines[:start]):
            failed += 1
            print(f"  FAIL  {n:20s} not loaded within {args.load_s} s on toy-os "
                  f"({len(g_lines)} lines of output)")
            continue
        problems = []
        if h[0] != g[0]:
            problems.append(f"title {g[0]!r}, host {h[0]!r}")
        if h[1] != g[1]:
            problems.append(f"extent {g[1]!r}, host {h[1]!r}")
        if not h[2]:
            problems.append("the HOST drew nothing -- the oracle is not working")
        if h[2] != g[2]:
            diff = list(difflib.unified_diff(h[2], g[2], "host", "toy-os", lineterm="", n=1))
            problems.append(f"{len(g[2])} plot lines, host {len(h[2])}:\n        "
                            + "\n        ".join(diff[:24]))
        if problems:
            failed += 1
            print(f"  FAIL  {n:20s} " + "\n        ".join(problems))
        else:
            print(f"  PASS  {n:20s} {len(h[2])} plot lines identical")
    print(f"browser_hostcheck: {len(names) - failed}/{len(names)} pages identical")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
