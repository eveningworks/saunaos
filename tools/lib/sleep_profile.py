#!/usr/bin/env python3
"""tools/lib/sleep_profile.py -- where a GUI suite run spent its time asleep.

    python3 tools/runners/gui_regress.py --sleep-profile /tmp/sp --logs /tmp/logs
    python3 tools/lib/sleep_profile.py /tmp/sp            # the ranked report
    python3 tools/lib/sleep_profile.py /tmp/sp --top 60

install() is the recorder: `gui_regress.py --sleep-profile DIR` writes a
sitecustomize.py calling it into DIR/.hook and puts that on each tool's
PYTHONPATH, so every time.sleep() in the tool is timed and charged to
the line that called it and to the nearest line in tools/tests/, and
the totals go to DIR/<tool>.json at exit. Only the MAIN thread's sleeps
hold a test up; a background thread's are kept apart. The report reads
those files, and ranks the MAIN thread's sleeps three ways: by
the line that called time.sleep() (settle()'s grace, harness.poll()'s
step, a bare sleep in a test), by the test line that led there, and by
tool against its wall time. A sleep is real seconds measured, not the
literal in the source: a poll's step that ends early costs what it cost.
"""

import argparse
import collections
import glob
import json
import os
import sys


def install(directory):
    """Times every time.sleep() in this process from now on, by call site,
    and writes DIR/<tool>.json at exit. What the sitecustomize.py that
    gui_regress writes calls; a no-op for an empty `directory`."""
    if not directory:
        return
    import atexit
    import json
    import sys
    import threading
    import time

    _real_sleep = time.sleep
    _started = time.monotonic()
    _sites = {}   # (thread kind, caller, test line) -> [count, seconds]

    def _where(code, line):
        path = code.co_filename
        i = path.find("/tools/")
        return f"{path[i + 1:] if i >= 0 else os.path.basename(path)}:{line}:{code.co_name}"

    def _sleep(seconds):
        t0 = time.monotonic()
        try:
            _real_sleep(seconds)
        finally:
            dt = time.monotonic() - t0
            f = sys._getframe(1)
            caller = _where(f.f_code, f.f_lineno)
            test = None
            g = f
            while g is not None:
                if "/tools/tests/" in g.f_code.co_filename:
                    test = _where(g.f_code, g.f_lineno)
                    break
                g = g.f_back
            kind = "main" if threading.current_thread() is threading.main_thread() else "thread"
            e = _sites.setdefault((kind, caller, test), [0, 0.0])
            e[0] += 1
            e[1] += dt

    time.sleep = _sleep

    @atexit.register
    def _write():
        name = os.path.splitext(os.path.basename(sys.argv[0] or "python"))[0]
        rows = [{"thread": k, "caller": c, "test": t, "count": n, "seconds": round(s, 3)}
                for (k, c, t), (n, s) in _sites.items()]
        try:
            os.makedirs(directory, exist_ok=True)
            with open(os.path.join(directory, f"{name}.json"), "w") as f:
                json.dump({"tool": name, "wall": round(time.monotonic() - _started, 2),
                           "sleeps": rows}, f)
        except OSError:
            pass


def load(d):
    out = []
    for p in sorted(glob.glob(os.path.join(d, "*.json"))):
        try:
            with open(p) as f:
                out.append(json.load(f))
        except (OSError, ValueError):
            print(f"sleep_profile: unreadable {p}", file=sys.stderr)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir", help="the directory --sleep-profile wrote")
    ap.add_argument("--top", type=int, default=30, help="rows per table (default 30)")
    args = ap.parse_args()
    tools = load(args.dir)
    if not tools:
        print(f"sleep_profile: nothing in {args.dir}")
        return 1
    by_caller = collections.defaultdict(lambda: [0, 0.0])
    by_test = collections.defaultdict(lambda: [0, 0.0])
    per_tool = []
    wall = slept = 0.0
    for t in tools:
        s = 0.0
        for r in t["sleeps"]:
            if r["thread"] != "main":
                continue
            s += r["seconds"]
            by_caller[r["caller"]][0] += r["count"]
            by_caller[r["caller"]][1] += r["seconds"]
            key = r["test"] or "(no test line on the stack)"
            by_test[key][0] += r["count"]
            by_test[key][1] += r["seconds"]
        per_tool.append((s, t["wall"], t["tool"]))
        wall += t["wall"]
        slept += s
    print(f"{len(tools)} tools, {wall:.0f} s of tool time, {slept:.0f} s of it asleep "
          f"on the main thread ({100 * slept / max(wall, 1e-3):.0f}%)\n")
    for title, table in (("by the line that slept", by_caller), ("by the test line behind it", by_test)):
        print(f"-- {title}")
        for k, (n, s) in sorted(table.items(), key=lambda kv: -kv[1][1])[:args.top]:
            print(f"  {s:7.1f} s  {n:6d}x  {k}")
        print()
    print("-- by tool (asleep / wall)")
    for s, w, name in sorted(per_tool, reverse=True)[:args.top]:
        print(f"  {s:6.1f} / {w:6.1f} s  {100 * s / max(w, 1e-3):3.0f}%  {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
