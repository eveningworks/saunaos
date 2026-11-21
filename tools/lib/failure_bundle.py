"""tools/lib/failure_bundle.py -- the guest's state at a test's FIRST
failed check, kept while the guest still exists.

    harness.Results.check() calls collect() on the first failure; nothing
    else needs to. DebugConsole and QMPSession register themselves here
    when they connect, so every tool that talks to a guest gets a bundle
    without an edit of its own.

A failure seen once in a full `gui_regress.py` run used to leave its
log and nothing else: by the time anyone read it the guest was stopped
and its image deleted, so `docs/bugs.md` filled with "cause not
established". The bundle is what the next question would have asked
of the live machine:

    index.txt     the check, its detail, the tool, when, what was taken
    klog.txt      the kernel log this console saw since it connected
    dmesg.txt     the guest's own log ring, from before that too
    ps.txt, cpus.txt, lockstat.txt, meminfo.txt     ring-3 readouts
    windows.json  `gui windows --json` -- the compositor's view
    cpu-regs.txt  QEMU's `info registers -a` -- THE ONE ORACLE THE GUEST
                  CANNOT FAKE: a wedged core shows its RIP here
    screen.png    the framebuffer, unsettled (a settled wait is a stall)

WHERE IT GOES: $SAUNAOS_BUNDLE_DIR if set (gui_regress.py sets it
under --logs, or under the history store), else next to the run in the
test history (`~/.local/share/saunaos/test-history/bundles/`), else a
scratch directory. One line on stdout names it, so the path lands in
the tool's log and in test_history.py's record of the run.

THE TRAP: collecting talks to the guest, so it must never wait on a
guest that has stopped answering. Every command gets CMD_TIMEOUT_S, and
the first one that comes back cut short ends the ring-3 part (QMP
needs no guest cooperation and still runs). It never raises:
a bundle that failed is a line saying so, never a changed verdict.

    SAUNAOS_FAILURE_BUNDLE=off      collect nothing

Tested on the host against a fake guest:
tools/hostchecks/failure_bundle_hostcheck.py.
"""
import os
import sys
import tempfile
import time
import weakref

LOG_LINES = 600          # the kernel log tail kept (the ring holds a few thousand)
CMD_TIMEOUT_S = 8.0      # per guest command; a healthy `sh ps` answers in well under 1 s
ENV_DIR = "SAUNAOS_BUNDLE_DIR"
ENV_OFF = "SAUNAOS_FAILURE_BUNDLE"

# (file, debug-console command). Ring-3 programs through the kernel
# shell's `sh`, so a broken ring-3 shell under test cannot answer for them.
GUEST_COMMANDS = (
    ("dmesg.txt", "sh dmesg -n 400"),     # the ring itself: lines from before the console connected
    ("ps.txt", "sh ps"),
    ("cpus.txt", "sh cpus"),
    ("lockstat.txt", "sh lockstat"),
    ("meminfo.txt", "sh meminfo"),
    ("windows.json", "gui windows --json"),
)

_consoles = weakref.WeakSet()
_qmps = weakref.WeakSet()
_taken = False           # one bundle per process: the FIRST failure is the evidence


def register_console(c):
    _consoles.add(c)


def register_qmp(q):
    _qmps.add(q)


def _out_dir(tool):
    stamp = time.strftime("%Y%m%d-%H%M%S")
    safe = "".join(ch if ch.isalnum() or ch in "-_." else "_" for ch in tool)[:40]
    base = os.environ.get(ENV_DIR)
    if base:
        d = base
    else:
        try:
            import test_history
            h = test_history.history_dir()
        except Exception:
            h = None
        d = (os.path.join(h, "bundles", f"{stamp}-{safe}") if h
             else tempfile.mkdtemp(prefix=f"bundle-{safe}-"))
    os.makedirs(d, exist_ok=True)
    return d


def _klog(c):
    lock = getattr(c, "_lock", None)
    try:
        if lock is not None:
            lock.acquire()
        lines = list(c._all_log) if getattr(c, "_log_thread", None) else list(c.log_lines)
    finally:
        if lock is not None:
            lock.release()
    return lines[-LOG_LINES:]


def _guest(c, d, taken, notes):
    try:
        with open(os.path.join(d, "klog.txt"), "w") as f:
            f.write("\n".join(_klog(c)) + "\n")
        taken.append("klog")
    except Exception as e:
        notes.append(f"klog: {e}")
    for name, cmd in GUEST_COMMANDS:
        try:
            out = c.send(cmd, timeout=CMD_TIMEOUT_S)
        except Exception as e:
            notes.append(f"{cmd}: {e}")
            break
        with open(os.path.join(d, name), "w") as f:
            f.write(out + "\n")
        taken.append(name.split(".")[0])
        if getattr(c, "_stale", False):
            notes.append(f"{cmd!r} came back cut short -- the guest stopped answering; "
                         f"the rest of the ring-3 part was skipped")
            break


def _qmp(q, d, taken, notes):
    try:
        regs = q.hmp("info registers -a")
        with open(os.path.join(d, "cpu-regs.txt"), "w") as f:
            f.write(regs + "\n")
        taken.append("cpu-regs")
    except Exception as e:                  # RemoteScreen refuses the monitor by name
        notes.append(f"info registers: {e}")
    try:
        q.screenshot(os.path.join(d, "screen.png"), stable=False, settle=0)
        taken.append("screen")
    except Exception as e:
        notes.append(f"screenshot: {e}")


def collect(check, detail="", tool=None):
    """Take the bundle once per process. Returns its directory, or None
    (off, already taken, or nothing registered to ask)."""
    global _taken
    if _taken or os.environ.get(ENV_OFF) == "off":
        return None
    consoles, qmps = list(_consoles), list(_qmps)
    if not consoles and not qmps:
        return None
    _taken = True
    tool = tool or os.path.splitext(os.path.basename(sys.argv[0]))[0] or "tool"
    t0 = time.time()
    taken, notes = [], []
    try:
        d = _out_dir(tool)
        for c in consoles[:1]:
            _guest(c, d, taken, notes)
        for q in qmps[:1]:
            _qmp(q, d, taken, notes)
        with open(os.path.join(d, "index.txt"), "w") as f:
            f.write(f"tool:    {tool}\ncheck:   {check}\ndetail:  {detail}\n"
                    f"time:    {time.strftime('%Y-%m-%dT%H:%M:%S%z')}\n"
                    f"argv:    {' '.join(sys.argv)}\ntaken:   {', '.join(taken) or 'nothing'}\n"
                    f"took:    {time.time() - t0:.1f} s\n")
            for n in notes:
                f.write(f"note:    {n}\n")
            f.write("\nA RIP in cpu-regs.txt names its function with "
                    "tools/lib/panic_resolve.py.\n")
    except Exception as e:
        print(f"failure bundle: not taken ({e})", flush=True)
        return None
    print(f"failure bundle: {d} ({', '.join(taken) or 'nothing'}; "
          f"{time.time() - t0:.1f} s)", flush=True)
    for n in notes:
        print(f"failure bundle: {n}", flush=True)
    return d
