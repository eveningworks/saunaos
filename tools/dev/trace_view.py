#!/usr/bin/env python3
"""A kernel event trace dump (`trace dump FILE`) as Chrome Trace Event JSON.

    python3 tools/dev/trace_view.py DUMP -o out.json    # open in ui.perfetto.dev or chrome://tracing
    python3 tools/dev/trace_view.py DUMP --text         # the records as /bin/trace show prints them
    python3 tools/dev/trace_view.py DUMP --stats        # how many of each event, per core

One track per CPU (the trace's pid 0, a thread per core): a SLICE for each
stretch a thread ran there, from one switch to the next, named
"name [tid]" from the dump's thread table; every other record an INSTANT
on that track with its arguments decoded. Times are microseconds since the
trace started, from the dump's TSC rate.

The dump DESCRIBES ITSELF: its event table carries each event's name and
argument string ("prev:d prev_state:s ..." -- abi/ktrace_abi.h's
KTRACE_EVENTS), and its syscall table the names, so this reads a dump
from a kernel newer than itself. Each table's row size is in the header,
so a row that grew is read by its known prefix. A dump of another
version, or with another magic, is refused rather than guessed at.

--text decodes as api/ktrace_fmt.h does, line for line -- the hostcheck
holds the two to that (tools/hostchecks/trace_view_hostcheck.py).
"""
import argparse
import json
import struct
import sys

MAGIC = b"SKTRACE\0"
VERSION = 1
HDR = struct.Struct("<8sIIQQ10IQ")
CPU = struct.Struct("<IIQQQQ")
EVENT = struct.Struct("<II16s64s")
SYSCALL = struct.Struct("<I28s")
NAME = struct.Struct("<ii24s")
REC = struct.Struct("<QHHi4Q")
STATE = {0: "-", 1: "R", 2: "R", 3: "Z", 4: "S"}
SWITCH = "switch"


class DumpError(Exception):
    pass


def cstr(b):
    return b.split(b"\0", 1)[0].decode("utf-8", "replace")


def rows(data, off, n, size, st, what):
    if size < st.size:
        raise DumpError(f"a {what} row of {size} bytes is shorter than this reader's {st.size}")
    out = []
    for i in range(n):
        at = off + i * size
        if at + st.size > len(data):
            raise DumpError(f"the dump ends inside its {what} table")
        out.append(st.unpack_from(data, at))
    return out, off + n * size


def load(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < HDR.size:
        raise DumpError("too short for a header")
    h = HDR.unpack_from(data, 0)
    magic, version, hdr_size, tsc_hz, armed_tsc = h[:5]
    (mask, ncpu, nevents, nsyscalls, nnames, rec_size,
     cpu_size, event_size, syscall_size, name_size) = h[5:15]
    nrecs = h[15]
    if magic != MAGIC:
        raise DumpError(f"not a kernel trace dump (magic {magic!r})")
    if version != VERSION:
        raise DumpError(f"dump version {version}; this reader knows {VERSION}")
    off = hdr_size
    cpus, off = rows(data, off, ncpu, cpu_size, CPU, "cpu")
    events, off = rows(data, off, nevents, event_size, EVENT, "event")
    syscalls, off = rows(data, off, nsyscalls, syscall_size, SYSCALL, "syscall")
    names, off = rows(data, off, nnames, name_size, NAME, "thread name")
    recs, off = rows(data, off, nrecs, rec_size, REC, "record")
    d = {
        "tsc_hz": tsc_hz, "armed_tsc": armed_tsc, "mask": mask,
        "cpus": [{"cpu": c[0], "capacity": c[2], "written": c[3], "lost": c[4], "nrecs": c[5]} for c in cpus],
        "events": {e[0]: {"name": cstr(e[2]), "group": e[1], "args": parse_args(cstr(e[3]))} for e in events},
        "syscalls": {s[0]: cstr(s[1]) for s in syscalls},
        "names": {n[0]: {"tgid": n[1], "name": cstr(n[2])} for n in names},
        "recs": [{"tsc": r[0], "event": r[1], "cpu": r[2], "tid": r[3], "a": list(r[4:8])} for r in recs],
    }
    if sum(c["nrecs"] for c in d["cpus"]) != nrecs:
        raise DumpError("the cpu rows' record counts do not add up to the header's")
    return d


def parse_args(spec):
    out = []
    for word in spec.split():
        name, _, kind = word.partition(":")
        out.append((name, kind[:1] or "x"))
    return out[:4]


def signed(v):
    return v - (1 << 64) if v >= 1 << 63 else v


def ticks_text(t, hz):
    if not hz:
        return f"{t} ticks"
    ns = t // hz * 1000000000 + t % hz * 1000000000 // hz
    if ns < 10000:
        return f"{ns} ns"
    if ns < 10000000:
        return f"{ns // 1000}.{ns % 1000:03d} us"
    return f"{ns // 1000000}.{ns // 1000 % 1000:03d} ms"


def arg_value(kind, v, d):
    if kind == "d":
        return str(signed(v))
    if kind == "s":
        return STATE.get(v, "?")
    if kind == "v":
        return str(v)
    if kind == "t":
        return ticks_text(v, d["tsc_hz"])
    if kind == "n":
        return d["syscalls"].get(v, str(v))
    return hex(v) if v else "0"   # C's %#x: no prefix on a zero


def decode_args(r, d):
    ev = d["events"].get(r["event"])
    if not ev:
        return None, {}
    return ev["name"], {name: arg_value(kind, r["a"][i], d) for i, (name, kind) in enumerate(ev["args"])}


def rel_tsc(r, d):
    return max(0, r["tsc"] - d["armed_tsc"])


def us(r, d):
    t = rel_tsc(r, d)
    return t * 1e6 / d["tsc_hz"] if d["tsc_hz"] else float(t)


def merged(d):
    # Each core's records are oldest-first already; sorted() is stable,
    # so equal stamps keep that order.
    return sorted(d["recs"], key=lambda r: r["tsc"])


def text_line(r, d):
    """api/ktrace_fmt.h's line, exactly."""
    t, hz = rel_tsc(r, d), d["tsc_hz"]
    stamp = f"{t // hz:5d}.{t % hz * 1000000 // hz:06d}" if hz else f"{t:12d}"
    head = f"{stamp} c{r['cpu']} [{r['tid']}] "
    name, args = decode_args(r, d)
    if name is None:
        a = r["a"]
        return head + f"event{r['event']} " + " ".join(hex(x) if x else "0" for x in a)
    return head + name + "".join(f" {k}={v}" for k, v in args.items())


def thread_label(tid, d):
    if tid == 0:
        return "kernel"
    n = d["names"].get(tid)
    return f"{n['name']} [{tid}]" if n else f"[{tid}]"


def chrome(d):
    ev = [{"ph": "M", "pid": 0, "name": "process_name", "args": {"name": "CPUs"}}]
    for c in d["cpus"]:
        ev.append({"ph": "M", "pid": 0, "tid": c["cpu"], "name": "thread_name",
                   "args": {"name": f"CPU {c['cpu']}"}})
        ev.append({"ph": "M", "pid": 0, "tid": c["cpu"], "name": "thread_sort_index",
                   "args": {"sort_index": c["cpu"]}})
    by_cpu = {}
    for r in d["recs"]:
        by_cpu.setdefault(r["cpu"], []).append(r)
    for cpu, recs in sorted(by_cpu.items()):
        running, since = None, None
        for r in recs:
            ts = us(r, d)
            if running is None:   # whatever was on the core when its first record was written
                running, since = r["tid"], ts
            name, args = decode_args(r, d)
            if name == SWITCH:
                if since is not None and running:
                    ev.append({"ph": "X", "pid": 0, "tid": cpu, "ts": since, "dur": max(ts - since, 0.001),
                               "name": thread_label(running, d), "cat": "run",
                               "args": {"tid": running, "until": f"prev_state={args.get('prev_state', '?')}"}})
                running, since = r["a"][2], ts
            ev.append({"ph": "i", "s": "t", "pid": 0, "tid": cpu, "ts": ts,
                       "name": name or f"event{r['event']}", "cat": "event",
                       "args": dict(args, tid=r["tid"])})
        if recs and running:
            end = us(recs[-1], d)
            if end > since:
                ev.append({"ph": "X", "pid": 0, "tid": cpu, "ts": since, "dur": end - since,
                           "name": thread_label(running, d), "cat": "run", "args": {"tid": running}})
    return {"traceEvents": ev, "displayTimeUnit": "ns",
            "otherData": {"tsc_hz": d["tsc_hz"], "cpus": d["cpus"]}}


def stats(d):
    count = {}
    for r in d["recs"]:
        name = d["events"].get(r["event"], {}).get("name", f"event{r['event']}")
        count.setdefault(r["cpu"], {}).setdefault(name, 0)
        count[r["cpu"]][name] += 1
    lines = []
    for c in d["cpus"]:
        per = count.get(c["cpu"], {})
        body = ", ".join(f"{k} {v}" for k, v in sorted(per.items()))
        lost = f" ({c['lost']} lost)" if c["lost"] else ""
        lines.append(f"cpu {c['cpu']}: {c['nrecs']} records{lost}: {body}")
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dump", help="a file `trace dump` wrote")
    ap.add_argument("-o", "--output", help="write the Chrome Trace Event JSON here ('-': stdout)")
    ap.add_argument("--text", action="store_true", help="print the records as decoded lines, cores merged by time")
    ap.add_argument("--stats", action="store_true", help="print how many of each event every core holds")
    args = ap.parse_args()
    try:
        d = load(args.dump)
    except (OSError, DumpError) as e:
        print(f"trace_view: {args.dump}: {e}", file=sys.stderr)
        return 1
    if args.text:
        for r in merged(d):
            print(text_line(r, d))
    if args.stats:
        print("\n".join(stats(d)))
    if args.output:
        out = json.dumps(chrome(d))
        if args.output == "-":
            print(out)
        else:
            with open(args.output, "w") as f:
                f.write(out)
    if not (args.text or args.stats or args.output):
        ap.error("nothing to do: -o FILE, --text or --stats")
    return 0


if __name__ == "__main__":
    sys.exit(main())
