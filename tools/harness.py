"""What every test tool here had written for itself: a results table, a
disk copy, and a small HTTP server on this machine.

    from harness import Results, copy_disk, BodyServer

    res = Results()
    res.check("the window opens", win is not None, f"win={win}")
    ...
    return res.finish("devmgr_test")      # "devmgr_test: 7 passed, 0 failed"

WHY ONE FILE. 118 tools defined their own result table, 43 spelled out
the same `cp` line and three their own `http.server`; a survey on
2026-09-30 found the tables in five storage shapes and a dozen print
formats for one idea. This is the part that does NOT differ between them
(dup_scan.py's rule: share what is identical, never a loop whose
differences become callbacks). A tool with a genuinely different table
-- a JSON report -- keeps its own.

`Results` is a superset of the shapes the tools used, so adopting it is
a one-line change: `.check()`/`.ok()` (returns the verdict), `.passes`
and `.fails` (names), `.rows` ((name, ok, detail)), `.failed()`, and
`.skip()` for a check that could not be judged this run (`.skips`; a
run that only skipped exits 77, automake's SKIP), and
`poll(fn, timeout)`, a bounded wait on an observable. The
summary line `finish()` prints is the one `gui_regress.py` reads:
`<tool>: N passed, M failed`, with `, K skipped` after it when any were.
"""
import http.server
import os
import ssl
import subprocess
import sys
import threading
import time


class Results:
    def __init__(self):
        # A test is running: nothing may rebuild or edit the tree under
        # it until this process exits (tree_lock.py). Here as well as in
        # iso_guard because many tools launch QEMU without it.
        import tree_lock
        tree_lock.hold()
        self.rows = []                 # (name, ok, detail), in order
        self.passes, self.fails = [], []
        self.skips = []                # (name, why): neither a pass nor a fail
        self._history_start()

    def check(self, name, ok, detail=""):
        ok = bool(ok)
        self.rows.append((name, ok, detail))
        (self.passes if ok else self.fails).append(name)
        print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""),
              flush=True)
        return ok

    ok = check

    def skip(self, name, why):
        """A check this run could not judge -- SAID, never silently dropped."""
        self.skips.append((name, why))
        print(f"  SKIP  {name}   {why}", flush=True)

    def summary(self, tool):
        s = f"{tool}: {len(self.passes)} passed, {len(self.fails)} failed"
        return s + (f", {len(self.skips)} skipped" if self.skips else "")

    def failed(self):
        return [r for r in self.rows if not r[1]]

    def finish(self, tool):
        """Print the summary line; 0 when something was JUDGED and nothing
        failed. A skip is neither a pass nor a fail: a run that only
        skipped judged nothing and exits 77, automake's SKIP, which
        gui_regress.py reports as SKIP; a run with no checks at all is 1."""
        print("\n" + self.summary(tool))
        if self.fails:
            rc = 1
        elif self.rows:
            rc = 0
        elif self.skips:
            print(f"{tool}: nothing judged -- every check skipped")
            rc = 77
        else:
            rc = 1
        self._history_record(tool, rc)
        return rc

    # The local test history (tools/test_history.py): everything printed
    # from construction on is kept as the run's log. Not under
    # gui_regress.py, which records the tool's row itself; and only the
    # first Results of a process tees.
    _teeing = False

    def _history_start(self):
        import test_history
        self._history = None
        if (Results._teeing or not test_history.enabled()
                or os.environ.get(test_history.BY_PARENT_ENV)):
            return
        Results._teeing = True
        self._history = []
        self._history_t0 = time.time()
        sys.stdout = test_history._Tee(sys.stdout, self._history)
        sys.stderr = test_history._Tee(sys.stderr, self._history)

    def _history_record(self, tool, rc):
        if self._history is None:
            return
        import test_history
        stem = os.path.splitext(os.path.basename(sys.argv[0]))[0] or tool
        test_history.record(stem, "PASS" if rc == 0 else "SKIP" if rc == 77 else "FAIL",
                            rc=rc, duration=time.time() - self._history_t0,
                            failures=[n for n, ok, _d in self.rows if not ok],
                            summary=self.summary(tool), log_text="".join(self._history))


def poll(fn, timeout, step=0.2):
    """fn() until it answers truthy or `timeout` seconds pass; its last
    answer either way. Poll on what the caller then ASSERTS, never on
    something weaker (CLAUDE.md: a weaker exit condition is a flake)."""
    import time
    deadline = time.time() + timeout
    while True:
        v = fn()
        if v or time.time() >= deadline:
            return v
        time.sleep(step)


def copy_disk(src, dst, cwd=None):
    """A copy of a disk image to boot a test from, never the image itself.

    --reflink=auto: copy-on-write where the filesystem can (btrfs, XFS),
    a plain copy where it cannot -- never =always, which fails outright
    on ext4. --sparse=always: disk.img is megabytes of data in a 9 GB
    sparse file, and /tmp is often a tmpfs, where a filled-in copy costs
    9 GB of RAM (cp's default of `auto` usually gets this right; stating
    it means an edit cannot quietly lose it)."""
    subprocess.run(["cp", "--reflink=auto", "--sparse=always", src, dst], check=True, cwd=cwd)
    return dst


class BodyServer:
    """A threaded HTTP(S) server on this machine answering every GET with
    one body (or `routes[path]`, 404 otherwise). A guest reaches it as
    SLIRP's 10.0.2.2; nothing leaves the machine.

    THREADED, and that is not an optimisation: a single-threaded server
    does a TLS handshake inside accept(), so one client that connects and
    walks away wedges every later connection."""

    def __init__(self, port=0, body=b"", routes=None, cert=None, key=None,
                 bind="0.0.0.0", ctype="text/plain"):
        routes = dict(routes or {})
        default = body

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.0"

            def do_GET(self):
                data = routes.get(self.path, default if not routes else None)
                if data is None:
                    self.send_response(404)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                self.send_response(200)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *a):
                pass

        self.routes = routes
        self.srv = http.server.ThreadingHTTPServer((bind, port), Handler)
        if cert:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(cert, key)
            self.srv.socket = ctx.wrap_socket(self.srv.socket, server_side=True)
        self.port = self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def shutdown(self):
        self.srv.shutdown()
        self.srv.server_close()
