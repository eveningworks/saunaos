#!/usr/bin/env python3
"""/bin/component end to end: remove an optional component, update past it, add it back.

docs/software-design.md, stage 2. WHAT IT PROVES, each against an
INDEPENDENT reader (`ls`, `sum`, `cat`) rather than component's report:

  1. `component list` names the build's components, Demos and tests
     installed.
  2. `component remove demos` deletes its files -- /bin/wm/demos, /tests,
     the demo apps' Start-menu entries -- but NOT /tests/nsmonkey, which
     the Web page engine holds (a longer path), and writes `removed
     demos` to /var/lib/update/components.
  3. An update leaves it removed: `update --check` says its files are
     left out and lists none of them, and after `update` they are still
     absent.
  4. ...and so does an update from a server that tags NOTHING (an older
     one): the machine tags the manifest from its own definitions.
  5. `component add demos` fetches it back from the server's per-build
     route at the machine's own build: `sum` agrees with the manifest
     for its files, and the state file no longer names it.
  6. Needs=: removing the Web page engine removes the Web Browser too;
     adding the Browser back brings the engine with it.
  7. A server without this machine's build answers 404, and `add` says
     "update first, then add it" and changes nothing -- and closes the
     update log it opened (strace: the `open` of it has its `close`).
  8. A build that merely does not STAGE a removed component (its lines
     and its definition gone, as NOGPL=1 or a checkout with no compiler
     leaves them) does not take it off the removed list: the next build
     that has it again still leaves it out.
  9. A file whose connection the server drops once is fetched again,
     once, with a log line, and the add still succeeds.
 10. `add` tags an UNTAGGED per-build manifest from the machine's own
     definitions, as an update does.
 11. With no --from, `add` fetches from the server and channel the
     machine's build came from (the record's `# from`), not the
     configured one -- which here has no such build.

**IT NEVER LEAVES THIS MACHINE.** The server is tools/dev/update_server.py's
own handler, run in this process on an ephemeral loopback port and
reached through SLIRP's 10.0.2.2.

    python3 tools/tests/sweep/component_test.py
    python3 tools/tests/sweep/component_test.py --positive-control

THE POSITIVE CONTROL serves check 3's update with every `component=` tag
renamed to one the machine does not know AND makes the machine's own
definitions unreadable, so nothing can tell the removed files are
optional: the update must then bring them back, and check 3 must fail.
"""

# tools/dev/update_server.py's handler in-process on a loopback port; one
# guest on unix sockets in a temporary directory (no QMP port).
RUNNER = {"covers": "/bin/component: remove, update past it, add back at the machine's build",
          "parallel": True}

import argparse
import http.server
import os
import re
import shutil
import sys
import tempfile
import threading

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from net_test import launch, kill  # noqa: E402
import update_server  # noqa: E402
from harness import Results, copy_disk  # noqa: E402

ROOT = toolpath.REPO
GATEWAY = "10.0.2.2"


class Server:
    """update_server's handler for this checkout's staging tree, with two
    switches: strip every `component=` tag (an older server), and refuse
    every per-build route (a build the server pruned)."""

    def __init__(self):
        self.manifest = update_server.Manifest(os.path.join(ROOT, "seed", "sync"))
        self.untagged = False
        self.retag = False        # the positive control: tags nobody knows
        self.no_builds = False
        self.unstaged = None      # a component this build does not stage at all
        self.drop_files = 0       # per-build file requests to close unanswered
        self.requests = []        # every path asked for, in order
        # "/main" serves the same tree: the channel a machine's record names.
        base = update_server.make_handler({"": self.manifest, "/main": self.manifest})
        outer = self

        class Handler(base):
            def log_message(self, *a):
                pass

            def _say(self, *a):
                pass

            def do_GET(self):
                outer.requests.append(self.path)
                if outer.no_builds and "/builds/" in self.path:
                    self._send(404, b"build not on this server\n")
                    return
                if outer.drop_files and "/builds/" in self.path and "/files/" in self.path:
                    outer.drop_files -= 1
                    self.close_connection = True     # no status line: a dropped connection
                    return
                if (outer.untagged or outer.retag or outer.unstaged) and self.path.endswith("/manifest"):
                    text, _ = outer.manifest.build()
                    if outer.unstaged:
                        cid = outer.unstaged
                        text = "".join(ln + "\n" for ln in text.splitlines()
                                       if f"component={cid}" not in ln
                                       and f"/usr/share/components/{cid}.component" not in ln)
                    if outer.untagged or outer.retag:
                        sub = ",component=zz-nobody" if outer.retag else ""
                        text = re.sub(r",?component=[a-z0-9-]+", sub, text)
                        text = re.sub(r" ,", " ", text)
                    self._send(200, text.encode(), "text/plain")
                    return
                super().do_GET()

        self.srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def crc(self, path):
        text, _ = self.manifest.build()
        for line in text.splitlines():
            f = line.split(" ")
            if len(f) >= 3 and f[2] == path:
                return int(f[0])
        return None

    def lines_of(self, cid):
        text, _ = self.manifest.build()
        return [ln.split(" ")[2] for ln in text.splitlines()
                if f"component={cid}" in ln]


def summed(sh, path):
    for line in sh.run(f"sum {path}").splitlines():
        f = line.split()
        if len(f) == 3 and f[2] == path and f[0].isdigit():
            return int(f[0]), int(f[1])
    return None


def absent(sh, path):
    out = sh.run(f"ls {path}")
    return "no such" in out.lower() or "not found" in out.lower() or "cannot" in out.lower()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("--keep", action="store_true", help="leave the guest running")
    ap.add_argument("--positive-control", action="store_true",
                    help="hide that the files are optional in check 3: it must FAIL")
    args = ap.parse_args()

    disk_src = os.path.join(ROOT, "disk.img")
    if not os.path.exists(disk_src):
        print("component_test: SKIP -- no disk.img; run `make iso` first")
        return 0
    why = update_server._stale_reason(os.path.join(ROOT, "seed", "sync"))
    if why:
        print(f"component_test: FAIL -- the staging tree is stale ({why}); run `make iso`")
        return 1

    c = Results()
    tmp = tempfile.mkdtemp(prefix="component_test.")
    pidfile = None
    srv = Server()
    url = f"http://{GATEWAY}:{srv.port}"
    demos = srv.lines_of("demos")
    try:
        disk = os.path.join(tmp, "component.img")
        copy_disk(disk_src, disk)
        print("component_test: booting")
        sh, pidfile = launch(disk, tmp, "comp", "e1000")

        # 1. the list
        out = sh.run("component list")
        c.ok("component list names the build's components, demos installed",
             re.search(r"demos\s+Demos and tests\s+MIT\s+\S+\s+installed", out) and "webengine" in out
             and "doom" in out, out.strip()[-500:])
        c.ok("the manifest tags Demos and tests' files", len(demos) > 100, f"{len(demos)} lines")

        # 2. remove
        out = sh.run("component remove demos", timeout=300)
        c.ok("component remove demos says what it removed",
             re.search(r"demos: removed \d+ files", out), out.strip()[-300:])
        c.ok(".../bin/wm/demos/counter is gone", absent(sh, "/bin/wm/demos/counter"))
        c.ok("...and /tests/sample.txt", absent(sh, "/tests/sample.txt"))
        c.ok("...and the Counter's Start-menu entry", absent(sh, "/usr/wm/applications/counter.desktop"))
        c.ok("...but /tests/nsmonkey stays: the Web page engine's (a longer path)",
             summed(sh, "/tests/nsmonkey") is not None)
        c.ok("...and the definition stays, so it can be added back",
             summed(sh, "/usr/share/components/demos.component") is not None)
        state = sh.run("cat /var/lib/update/components")
        c.ok("...and the state file says `removed demos`", "removed demos" in state, state.strip()[-200:])
        c.ok("component list shows it removed",
             re.search(r"demos\s+Demos and tests.*removed", sh.run("component list")))

        # 3. an update leaves it removed
        if args.positive_control:
            srv.retag = True
            sh.run("mv /usr/share/components /usr/share/components.hidden")
        out = sh.run(f"update --from {url} --check -v", timeout=600)
        c.ok("update --check says the removed component's files are left out",
             "component demos: removed on this machine" in out, out.strip()[-300:])
        c.ok("...and lists none of them to fetch",
             "/tests/sample.txt" not in out and "/bin/wm/demos/counter" not in out, out.strip()[-300:])
        sh.run(f"update --from {url}", timeout=900)
        c.ok("after `update`, /bin/wm/demos/counter is still absent", absent(sh, "/bin/wm/demos/counter"))
        c.ok("...and /tests/sample.txt", absent(sh, "/tests/sample.txt"))
        if args.positive_control:
            srv.retag = False
            sh.run("mv /usr/share/components.hidden /usr/share/components")

        # 4. a server that tags nothing
        srv.untagged = True
        out = sh.run(f"update --from {url} --check -v", timeout=600)
        srv.untagged = False
        c.ok("an untagged manifest (an older server) still leaves them out",
             "component demos: removed on this machine" in out and "/tests/sample.txt" not in out,
             out.strip()[-300:])

        # 8. a build that does not stage it at all: still removed afterwards
        srv.unstaged = "demos"
        sh.run(f"update --from {url}", timeout=900)
        srv.unstaged = None
        state = sh.run("cat /var/lib/update/components")
        c.ok("a build that does not stage Demos and tests leaves it on the removed list",
             "removed demos" in state, state.strip()[-200:])
        out = sh.run(f"update --from {url} --check -v", timeout=600)
        c.ok("...so the next build that has it again still leaves its files out",
             "component demos: removed on this machine" in out and "/tests/sample.txt" not in out,
             out.strip()[-300:])
        sh.run(f"update --from {url}", timeout=900)      # its definition back, for `add`
        if "removed demos" not in sh.run("cat /var/lib/update/components"):
            sh.run("component remove demos", timeout=300)   # so the checks below test their own thing

        # 7. (before 5: nothing is fetched) a pruned build
        srv.no_builds = True
        out = sh.run(f"strace -e open,close -o /tmp/ct.trace /bin/component add demos --from {url}",
                     timeout=300)
        srv.no_builds = False
        c.ok("a server without this machine's build: `update first, then add it`",
             "update first, then add it" in out, out.strip()[-300:])
        c.ok("...and nothing changed", absent(sh, "/tests/sample.txt")
             and "removed demos" in sh.run("cat /var/lib/update/components"))
        trace = sh.run("cat /tmp/ct.trace")
        m = re.search(r'open\("/var/log/update\.log"[^)]*\)\s*=\s*(\d+)', trace)
        c.ok("...and closes the update log it opened",
             m and re.search(rf"close\({m.group(1)}\)\s*=\s*0", trace[m.end():]),
             trace.strip()[-400:])

        # 5. add it back -- one file's connection dropped on the way
        srv.drop_files = 1
        out = sh.run(f"component add demos --from {url}", timeout=900)
        dropped = srv.drop_files == 0
        srv.drop_files = 0
        c.ok("component add demos fetches at this machine's build",
             "adding at this machine's build" in out and "/builds/" in out and "added 1 component" in out,
             out.strip()[-300:])
        c.ok("...fetching again, once, the file whose connection was dropped",
             dropped and "retrying once" in out, out.strip()[-300:])
        for path in ("/tests/sample.txt", "/bin/wm/demos/counter", "/usr/wm/applications/counter.desktop"):
            got = summed(sh, path)
            c.ok(f"sum agrees with the manifest for {path}",
                 got is not None and got[0] == srv.crc(path), f"sum {got} manifest {srv.crc(path)}")
        state = sh.run("cat /var/lib/update/components")
        c.ok("...and the state file no longer names it", "removed demos" not in state, state.strip()[-200:])
        out = sh.run(f"update --from {url} --check", timeout=600)
        c.ok("...and the machine is up to date again", "up to date" in out, out.strip()[-200:])

        # 6. Needs=
        out = sh.run("component remove webengine", timeout=300)
        c.ok("removing the Web page engine removes the Web Browser too",
             "removing browser too" in out and absent(sh, "/bin/wm/apps/browser")
             and absent(sh, "/bin/webcontent"), out.strip()[-300:])
        state = sh.run("cat /var/lib/update/components")
        c.ok("...both marked removed", "removed webengine" in state and "removed browser" in state,
             state.strip()[-200:])
        # ...from an UNTAGGED per-build manifest (10): tagged here.
        srv.untagged = True
        out = sh.run(f"component add browser --from {url}", timeout=900)
        srv.untagged = False
        c.ok("adding the Browser back brings the engine with it, from an untagged manifest",
             "adding webengine too" in out and "added 2 components" in out
             and summed(sh, "/bin/webcontent") is not None
             and summed(sh, "/bin/wm/apps/browser") is not None, out.strip()[-300:])

        # 11. no --from: the channel the machine's build came from
        sh.run(f"update --server {url}/nochannel")      # configured: a server with no such build
        sh.run(f"update --from {url}/main", timeout=900)
        rec = sh.run("head -n 6 /var/lib/update/installed")
        c.ok("an update records where the build came from", f"# from {url}/main" in rec, rec.strip()[-300:])
        sh.run("component remove demos", timeout=300)
        srv.requests.clear()
        out = sh.run("component add demos", timeout=900)
        main_builds = [r for r in srv.requests if r.startswith("/main/builds/")]
        c.ok("with no --from, add fetches from that channel, not the configured server",
             "added 1 component" in out and main_builds, f"{out.strip()[-300:]} | {srv.requests[:4]}")
        c.ok("...and the files match", summed(sh, "/tests/sample.txt") is not None
             and summed(sh, "/tests/sample.txt")[0] == srv.crc("/tests/sample.txt"))
    finally:
        if pidfile and not args.keep:
            kill(pidfile)
        srv.srv.shutdown()
        if not args.keep:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print(f"component_test: guest and files kept in {tmp}")

    bad = c.failed()
    print(f"\ncomponent_test: {len(c.rows) - len(bad)}/{len(c.rows)} checks passed")
    if args.positive_control:
        if bad:
            print("component_test: positive control OK -- an update that cannot see the removal is caught")
            return 0
        print("component_test: POSITIVE CONTROL DID NOT FAIL")
        return 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
