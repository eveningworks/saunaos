#!/usr/bin/env python3
"""A local record of every test run: when did this check last fail, or pass?

    python3 tools/test_history.py                      # the last 20 runs
    python3 tools/test_history.py "sched_test.c:114"   # runs that failed it, by commit
    python3 tools/test_history.py --tool ktest_run --last 50
    python3 tools/test_history.py --show <id>          # that run's whole log

preflight.sh, ktest_run.py, gui_regress.py (one row per tool) and every
tool built on harness.Results append a line here as they finish:
commit, dirty or not, verdict, the checks that failed, timings, the
host's kernel and QEMU, and the run's whole log. A failure prints a
`history:` line saying how often that check failed lately and when the
tool last passed.

LOCAL ONLY, NEVER IN THE REPO: ~/.local/share/toy-os/test-history/
(`TOYOS_TEST_HISTORY=DIR` moves it, `=off` stops recording).
runs.jsonl is kept for good; logs/ is gzipped and pruned oldest first
past TOYOS_TEST_HISTORY_MAX_MB (default 1024).

A check is matched by its NAME, never its line number, which moves
between commits. Recording never fails a run: an error is one line on
stderr.
"""
import argparse
import contextlib
import fcntl
import gzip
import hashlib
import io
import json
import os
import re
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PARENT_ENV = "TOYOS_TEST_HISTORY_PARENT"
# Set by gui_regress.py in a tool's environment: it records that tool's
# row itself (it has the output), so harness.Results must not as well.
BY_PARENT_ENV = "TOYOS_TEST_HISTORY_BY_PARENT"


def history_dir():
    d = os.environ.get("TOYOS_TEST_HISTORY", "")
    if d == "off":
        return None
    return d or os.path.expanduser("~/.local/share/toy-os/test-history")


def enabled():
    return history_dir() is not None


def new_id(tool):
    safe = re.sub(r"[^A-Za-z0-9_.-]", "_", tool)[:40]
    # The random tail: gui_regress records many tools in one second from
    # one process, and an id must name one run.
    return f"{time.strftime('%Y%m%d-%H%M%S')}-{safe}-{os.getpid()}-{os.urandom(2).hex()}"


def _run(cmd, cwd=REPO):
    try:
        return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                              timeout=20).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


_HOST = None


def _host():
    global _HOST
    if _HOST is None:
        qemu = _run(["qemu-system-x86_64", "--version"]).splitlines()
        m = re.search(r"version (\S+)", qemu[0]) if qemu else None
        _HOST = {
            "kernel": os.uname().release,
            "qemu": m.group(1) if m else "",
            "cpus": os.cpu_count(),
        }
    h = dict(_HOST)
    try:
        h["load"] = [round(x, 2) for x in os.getloadavg()]
    except OSError:
        pass
    return h


def _tree():
    """The commit, and whether tracked files differ from it."""
    sha = _run(["git", "rev-parse", "--short=8", "HEAD"])
    diff = _run(["git", "diff", "HEAD"])
    return {
        "commit": sha,
        "dirty": bool(diff),
        "diff_hash": hashlib.sha256(diff.encode()).hexdigest()[:12] if diff else "",
        "branch": _run(["git", "rev-parse", "--abbrev-ref", "HEAD"]),
        "checkout": REPO,
        "fs": _run(["stat", "-f", "-c", "%T", REPO]),
    }


def check_name(failure):
    """What a failure is matched by: the text before a trailing
    " (file:line)" or "   detail"."""
    return re.split(r" \(|\s{3}", failure, maxsplit=1)[0].strip()


def load(d=None):
    d = d or history_dir()
    path = os.path.join(d, "runs.jsonl") if d else ""
    rows = []
    if not path or not os.path.exists(path):
        return rows
    with open(path) as f:
        for line in f:
            try:
                rows.append(json.loads(line))
            except ValueError:
                continue
    return rows


def _prune(logs, max_bytes):
    files = sorted(os.listdir(logs))           # ids sort by time
    sizes = {n: os.path.getsize(os.path.join(logs, n)) for n in files}
    total = sum(sizes.values())
    for n in files:
        if total <= max_bytes:
            break
        os.unlink(os.path.join(logs, n))
        total -= sizes[n]


def _hints(rows, row, out):
    """One line per failed check (at most three): how often it failed in
    this tool's recent runs, and the tool's last pass."""
    tool = row["tool"]
    earlier = [r for r in rows if r.get("tool") == tool and r["id"] != row["id"]]
    recent = earlier[-19:] + [row]
    last_pass = next((r for r in reversed(earlier) if r.get("verdict") == "PASS"), None)
    for f in row.get("failures", [])[:3]:
        name = check_name(f)
        hits = sum(1 for r in recent if any(check_name(x) == name for x in r.get("failures", [])))
        where = (f"last PASS of {tool}: {last_pass.get('commit', '?')}"
                 f"{'+' if last_pass.get('dirty') else ''} {last_pass['time'][:16]}"
                 if last_pass else f"no recorded PASS of {tool}")
        seen = "first time recorded" if hits == 1 else f"{hits} of the last {len(recent)} {tool} runs"
        print(f"history: {name[:70]}: {seen}; {where}", file=out)


def record(tool, verdict, *, rc=None, duration=None, failures=(), summary="",
           log_text=None, log_path=None, argv=None, target="", run_id=None,
           hints=True):
    """Append one run. Returns its id, or None when off or on error."""
    d = history_dir()
    if d is None:
        return None
    try:
        os.makedirs(os.path.join(d, "logs"), exist_ok=True)
        run_id = run_id or new_id(tool)
        row = {
            "id": run_id,
            "time": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
            "tool": tool,
            "verdict": verdict,
            "rc": rc,
            "duration_s": round(duration, 1) if duration is not None else None,
            "failures": [str(f) for f in failures],
            "summary": summary,
            "argv": list(argv) if argv is not None else sys.argv[1:],
            "target": target,
            "parent": os.environ.get(PARENT_ENV, ""),
            **_tree(),
            "host": _host(),
            "log": "",
        }
        if log_path and os.path.exists(log_path):
            with open(log_path, "rb") as f:
                log_text = f.read().decode("utf-8", errors="replace")
        if log_text:
            name = f"{run_id}.log.gz"
            with gzip.open(os.path.join(d, "logs", name), "wt") as f:
                f.write(log_text)
            row["log"] = name
        path = os.path.join(d, "runs.jsonl")
        with open(path, "a") as f:
            fcntl.flock(f, fcntl.LOCK_EX)      # gui_regress records from threads and processes
            f.write(json.dumps(row) + "\n")
        max_mb = int(os.environ.get("TOYOS_TEST_HISTORY_MAX_MB", "1024"))
        _prune(os.path.join(d, "logs"), max_mb * 1024 * 1024)
        if hints and row["failures"]:
            _hints(load(d), row, sys.stderr)
        return run_id
    except Exception as e:                      # never fail the run being recorded
        print(f"test_history: could not record {tool}: {e}", file=sys.stderr)
        return None


class _Tee(io.TextIOBase):
    def __init__(self, real, sink):
        self.real, self.sink = real, sink

    def write(self, s):
        self.sink.append(s)
        return self.real.write(s)

    def flush(self):
        self.real.flush()

    def isatty(self):
        return self.real.isatty()

    def fileno(self):
        return self.real.fileno()


@contextlib.contextmanager
def capture():
    """Everything printed inside, also kept: `with capture() as out:`,
    then "".join(out)."""
    sink = []
    old = sys.stdout, sys.stderr
    sys.stdout, sys.stderr = _Tee(old[0], sink), _Tee(old[1], sink)
    try:
        yield sink
    finally:
        sys.stdout, sys.stderr = old


# --- reading ------------------------------------------------------------

def _label(r):
    return f"{r.get('commit') or '?'}{'+' if r.get('dirty') else ''}"


def _line(r):
    fails = r.get("failures", [])
    first = (check_name(fails[0])[:60] + (f" (+{len(fails) - 1} more)" if len(fails) > 1 else "")) if fails else ""
    dur = f"{r['duration_s']:.0f}s" if r.get("duration_s") is not None else "-"
    return (f"{r['time'][:16].replace('T', ' ')}  {r['tool'][:18]:<18} {r['verdict']:<5} "
            f"{_label(r):<10} {dur:>6}  {first}")


def show_matches(rows, pattern):
    pat = pattern.lower()
    hit = [r for r in rows if any(pat in f.lower() for f in r.get("failures", []))
           or pat in r.get("summary", "").lower()]
    if not hit:
        print(f"test_history: no recorded run failed anything matching {pattern!r}")
        return 1
    for tool in sorted({r["tool"] for r in hit}):
        runs = [r for r in rows if r["tool"] == tool]
        mine = [r for r in hit if r["tool"] == tool]
        first, last = mine[0], mine[-1]
        since = runs[runs.index(first):]
        print(f"{tool}: failed it {len(mine)} times, first {first['time'][:16]} at {_label(first)}, "
              f"last {last['time'][:16]} at {_label(last)}; {len(mine)} of {len(since)} runs since the first")
        before = [r for r in runs[:runs.index(first)] if r.get("verdict") == "PASS"]
        if before:
            print(f"  last PASS before it first appeared: {before[-1]['time'][:16]} at {_label(before[-1])}")
        after = [r for r in runs if r["time"] > last["time"] and r.get("verdict") == "PASS"]
        if after:
            print(f"  passed since the last failure: {len(after)} run(s), latest at {_label(after[-1])}")
        by_commit = {}
        for r in since:
            c = by_commit.setdefault(_label(r), [0, 0])
            c[1] += 1
            c[0] += r in mine
        print("  by commit (failed/runs): " + ", ".join(f"{k} {v[0]}/{v[1]}" for k, v in by_commit.items()))
        hosts = sorted({(r["host"].get("kernel", ""), r["host"].get("qemu", "")) for r in since})
        print("  hosts: " + "; ".join(f"kernel {k}, QEMU {q}" for k, q in hosts))
    print()
    for r in hit[-20:]:
        print(_line(r) + f"   [{r['id']}]")
    return 0


def show_log(d, rows, run_id):
    r = next((r for r in rows if r["id"] == run_id or r["id"].startswith(run_id)), None)
    if not r:
        print(f"test_history: no run {run_id!r}")
        return 1
    print(json.dumps({k: v for k, v in r.items() if k != "log"}, indent=1))
    path = os.path.join(d, "logs", r["log"]) if r.get("log") else ""
    if not path or not os.path.exists(path):
        print("(no log kept" + (", pruned" if r.get("log") else "") + ")")
        return 0
    with gzip.open(path, "rt") as f:
        sys.stdout.write(f.read())
    return 0


def selftest():
    with tempfile.TemporaryDirectory() as d:
        os.environ["TOYOS_TEST_HISTORY"] = d
        ok = []
        a = record("t", "PASS", rc=0, duration=1, log_text="one", hints=False)
        b = record("t", "FAIL", rc=1, failures=["a check (x.c:1)"], log_text="two", hints=False)
        rows = load(d)
        ok.append(len(rows) == 2 and rows[1]["failures"] == ["a check (x.c:1)"])
        ok.append(check_name("a check (x.c:9)") == check_name("a check   detail") == "a check")
        err = io.StringIO()
        _hints(rows, rows[1], err)
        ok.append("first time recorded" in err.getvalue() and "last PASS of t" in err.getvalue())
        with gzip.open(os.path.join(d, "logs", f"{b}.log.gz"), "rt") as f:
            ok.append(f.read() == "two")
        _prune(os.path.join(d, "logs"), 1)
        ok.append(not os.listdir(os.path.join(d, "logs")))
        with contextlib.redirect_stdout(io.StringIO()):
            ok.append(show_matches(load(d), "x.c:1") == 0 and show_matches(load(d), "nothing") == 1)
        ok.append(a is not None)
    print(f"test_history: selftest {'ok' if all(ok) else 'FAILED'} ({sum(ok)}/{len(ok)})")
    return 0 if all(ok) else 1


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "record":
        ap = argparse.ArgumentParser(prog="test_history.py record",
                                     description="Append one run (for shell callers).")
        ap.add_argument("--tool", required=True)
        ap.add_argument("--rc", type=int, required=True, help="0 is PASS, 77 SKIP, anything else FAIL")
        ap.add_argument("--id")
        ap.add_argument("--duration", type=float)
        ap.add_argument("--failure", action="append", default=[])
        ap.add_argument("--summary", default="")
        ap.add_argument("--log", help="a file holding the run's output")
        ap.add_argument("--argv", default="", help="the run's own arguments")
        a = ap.parse_args(sys.argv[2:])
        verdict = "PASS" if a.rc == 0 else "SKIP" if a.rc == 77 else "FAIL"
        record(a.tool, verdict, rc=a.rc, duration=a.duration, failures=a.failure,
               summary=a.summary, log_path=a.log, argv=a.argv.split(), run_id=a.id)
        return 0
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 epilog="`record --help` for the shell form of recording.")
    ap.add_argument("pattern", nargs="?", help="a failed check's text, or part of it")
    ap.add_argument("--tool", help="only this tool's runs")
    ap.add_argument("--last", type=int, default=20, metavar="N", help="how many runs to list (default 20)")
    ap.add_argument("--show", metavar="ID", help="print one run's record and log")
    ap.add_argument("--json", action="store_true", help="the matching records, one JSON object a line")
    ap.add_argument("--selftest", action="store_true", help="check this module against a scratch store")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    d = history_dir()
    if d is None:
        print("test_history: off (TOYOS_TEST_HISTORY=off)")
        return 1
    rows = load(d)
    if a.tool:
        rows = [r for r in rows if r["tool"] == a.tool]
    if a.show:
        return show_log(d, rows, a.show)
    if a.json:
        for r in rows[-a.last:]:
            print(json.dumps(r))
        return 0
    if a.pattern:
        return show_matches(rows, a.pattern)
    if not rows:
        print(f"test_history: nothing recorded yet in {d}")
        return 0
    for r in rows[-a.last:]:
        print(_line(r))
    return 0


if __name__ == "__main__":
    sys.exit(main())
