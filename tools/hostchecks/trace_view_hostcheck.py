#!/usr/bin/env python3
"""tools/dev/trace_view.py against a synthetic kernel trace dump, on the HOST.

A C driver built with the host gcc against the tree's own
abi/ktrace_abi.h and kernel/lib/ktrace_fmt.c writes a dump of a known
scenario -- two cores, three threads, a switch each way, a syscall that
parks, an interrupt, a page fault, a lock wait, an event id the table
does not know -- exactly as `trace dump` lays one out, and prints each
record through the formatter the kernel's panic tail and `/bin/trace show`
use. Then:

  - `trace_view.py --text` must print the SAME lines (two decoders, one
    of them C, held to each other through the dump's own event table);
  - `-o` must give one track per CPU, a slice for each stretch a thread
    ran between switches (the right names, starts and lengths), and an
    instant for every record, the syscall's named;
  - `--stats` counts each core's records;
  - a dump of another version, or another magic, is refused.

`--positive-control` breaks it three ways, each of which must go RED:
the record's layout drifting from what the reader assumes, the slices
ignoring whom a switch hands the core to, and the C formatter printing a
state letter the reader does not.
"""

# trace_view.py vs a C-written dump and the shared formatter. Host gcc and
# python only. Carries a --positive-control that must go red.
RUNNER = {"name": "trace_view_host",
          "covers": "a kernel trace dump decodes to Chrome Trace JSON and text as the kernel's formatter does",
          "gate": True, "parallel": True}

import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import hostcheck  # noqa: E402

TOOL = "trace_view_hostcheck"

# The toolkit names ktrace_fmt.c uses, as the host's C library.
KFMT_SHIM = "#include <stdio.h>\n#include <stdarg.h>\n#define k_vsnprintf vsnprintf\n"
STRING_SHIM = ("#include_next <string.h>\n#define k_strcmp strcmp\n#define k_strncmp strncmp\n"
               "#define k_strlen strlen\n")

DRIVER = r"""
#include <stdio.h>
#include <string.h>
#include "ktrace_fmt.h"

#define HZ 1000000ull   /* a tick is a microsecond */
#define T0 1000ull

/* Designated: a layout change must move the bytes, not the initialiser. */
#define R(t, e, c, id, a0, a1, a2, a3) { .tsc = (t), .event = (e), .cpu = (c), .tid = (id), \
                                         .a = { (a0), (a1), (a2), (a3) } }
static struct ktrace_rec cpu0[] = {
    R(1100, KTRACE_EV_SYS_ENTER, 0, 5, 3, 9, 0x80001000, 4096),
    R(1200, KTRACE_EV_SWITCH, 0, 5, 5, KTRACE_STATE_BLOCKED, 7, 7),
    R(1300, KTRACE_EV_IRQ, 0, 7, 32, 0x401000, 0x23, 0),
    R(1350, KTRACE_EV_IRQ_EXIT, 0, 7, 32, 0, 0, 0),
    R(1500, KTRACE_EV_SWITCH, 0, 7, 7, KTRACE_STATE_READY, 5, 5),
    R(1600, KTRACE_EV_SYS_EXIT, 0, 5, 3, (uint64_t)-11, 0, 0),
};
static struct ktrace_rec cpu1[] = {
    R(1150, KTRACE_EV_SWITCH, 1, 0, 0, 0, 9, 9),
    R(1250, KTRACE_EV_FAULT, 1, 9, 0x7000, 6, 0x401234, 0),
    R(1400, KTRACE_EV_SPIN_WAIT, 1, 9, 0xffff800000123000ull, 250, 0xffff800000101000ull, 0),
    R(1450, KTRACE_EV_SWITCH, 1, 9, 9, KTRACE_STATE_ZOMBIE, 0, 0),
    R(1550, 99, 1, 0, 1, 0, 2, 3),
};
#define N0 (sizeof cpu0 / sizeof cpu0[0])
#define N1 (sizeof cpu1 / sizeof cpu1[0])

static const char *sysname(unsigned nr) { return nr == 3 ? "read" : 0; }

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    FILE *f = fopen(argv[1], "wb");
    if (!f) return 2;
    unsigned nev;
    const struct ktrace_event_desc *ev = ktrace_event_table(&nev);
    static const struct { int tid, tgid; const char *name; } names[] = {
        { 5, 5, "tosh" }, { 7, 7, "pipe_test" }, { 9, 9, "hello" },
    };
    struct ktrace_dump_hdr h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, KTRACE_DUMP_MAGIC, sizeof h.magic);
    h.version = KTRACE_DUMP_VERSION;
    h.hdr_size = sizeof h;
    h.tsc_hz = HZ;
    h.armed_tsc = T0;
    h.mask = KTRACE_G_ALL;
    h.ncpu = 2;
    h.nevents = nev;
    h.nsyscalls = 1;
    h.nnames = 3;
    h.rec_size = sizeof(struct ktrace_rec);
    h.cpu_size = sizeof(struct ktrace_dump_cpu);
    h.event_size = sizeof(struct ktrace_dump_event);
    h.syscall_size = sizeof(struct ktrace_dump_syscall);
    h.name_size = sizeof(struct ktrace_dump_name);
    h.nrecs = N0 + N1;
    fwrite(&h, sizeof h, 1, f);
    struct ktrace_dump_cpu c0 = { 0, 0, 64, N0, 0, N0 }, c1 = { 1, 0, 64, N1, 0, N1 };
    fwrite(&c0, sizeof c0, 1, f);
    fwrite(&c1, sizeof c1, 1, f);
    for (unsigned i = 0; i < nev; i++) {
        struct ktrace_dump_event e;
        memset(&e, 0, sizeof e);
        e.id = ev[i].id;
        e.group = ev[i].group;
        snprintf(e.name, sizeof e.name, "%s", ev[i].name);
        snprintf(e.args, sizeof e.args, "%s", ev[i].args);
        fwrite(&e, sizeof e, 1, f);
    }
    struct ktrace_dump_syscall sc;
    memset(&sc, 0, sizeof sc);
    sc.nr = 3;
    snprintf(sc.name, sizeof sc.name, "read");
    fwrite(&sc, sizeof sc, 1, f);
    for (unsigned i = 0; i < 3; i++) {
        struct ktrace_dump_name n;
        memset(&n, 0, sizeof n);
        n.tid = names[i].tid;
        n.tgid = names[i].tgid;
        snprintf(n.name, sizeof n.name, "%s", names[i].name);
        fwrite(&n, sizeof n, 1, f);
    }
    fwrite(cpu0, sizeof cpu0, 1, f);
    fwrite(cpu1, sizeof cpu1, 1, f);
    if (fclose(f) != 0) return 2;

    /* The formatter's lines, the cores merged by time. */
    struct ktrace_fmt_ctx ctx = { T0, HZ, sysname };
    unsigned i0 = 0, i1 = 0;
    char line[256];
    while (i0 < N0 || i1 < N1) {
        const struct ktrace_rec *r =
            i1 >= N1 || (i0 < N0 && cpu0[i0].tsc <= cpu1[i1].tsc) ? &cpu0[i0++] : &cpu1[i1++];
        if (ktrace_fmt_rec(line, sizeof line, r, &ctx) <= 0) return 3;
        printf("%s\n", line);
    }
    return 0;
}
"""

# What the JSON must hold: (cpu, name, start us, length us) of each slice.
SLICES = sorted([(0, "tosh [5]", 100.0, 100.0), (0, "pipe_test [7]", 200.0, 300.0),
                 (0, "tosh [5]", 500.0, 100.0), (1, "hello [9]", 150.0, 300.0)])

CONTROLS = [
    ("the record layout drifts from the reader's",
     "kernel/include/abi/ktrace_abi.h",
     "    uint16_t event;   // KTRACE_EV_*\n    uint16_t cpu;\n",
     "    uint16_t cpu;\n    uint16_t event;   // KTRACE_EV_*\n"),
    ("a slice ignores whom the switch hands the core to",
     "tools/dev/trace_view.py",
     'running, since = r["a"][2], ts', 'running, since = r["tid"], ts'),
    ("the C formatter letters a parked thread differently",
     "kernel/lib/ktrace_fmt.c",
     "case KTRACE_STATE_BLOCKED: return 'S';", "case KTRACE_STATE_BLOCKED: return 'D';"),
]


def build(tmp, control=None):
    inc = os.path.join(tmp, "inc")
    os.makedirs(inc, exist_ok=True)
    edit = {control[1]: [(control[2], control[3])]} if control else {}
    for h in ("kernel/include/abi/ktrace_abi.h", "kernel/include/api/ktrace_fmt.h"):
        hostcheck.stage(inc, h, edits=edit.get(h, ()), apply=h in edit, tool=TOOL)
    hostcheck.write(inc, "kfmt.h", KFMT_SHIM)
    hostcheck.write(inc, "string.h", STRING_SHIM)
    rel = "kernel/lib/ktrace_fmt.c"
    src = hostcheck.stage(tmp, rel, edits=edit.get(rel, ()), apply=rel in edit, tool=TOOL)
    view = "tools/dev/trace_view.py"
    viewer = hostcheck.stage(tmp, view, edits=edit.get(view, ()), apply=view in edit, tool=TOOL)
    drv = hostcheck.write(tmp, "driver.c", DRIVER)
    return hostcheck.compile(tmp, "ktrace_synth", [drv, src], includes=[inc], tool=TOOL), viewer


def view(viewer, *args):
    return subprocess.run([sys.executable, viewer, *args], capture_output=True, text=True)


def run(tmp, exe, viewer, quiet):
    fails = []

    def check(ok, what, detail=""):
        if not quiet:
            print(("  ok    " if ok else "  FAIL  ") + what + (f" -- {detail}" if detail and not ok else ""))
        if not ok:
            fails.append(what)

    dump = os.path.join(tmp, "synth.ktrace")
    r = subprocess.run([exe, dump], capture_output=True, text=True)
    if r.returncode != 0:
        check(False, "the driver wrote the dump", f"exit {r.returncode}")
        return fails
    c_lines = r.stdout.splitlines()

    t = view(viewer, dump, "--text")
    check(t.returncode == 0 and t.stdout.splitlines() == c_lines,
          "--text prints the C formatter's lines, line for line",
          "\n".join(f"  C:  {a}\n  py: {b}" for a, b in zip(c_lines, t.stdout.splitlines()) if a != b)[:900]
          or t.stderr[-300:])

    out = os.path.join(tmp, "out.json")
    j = view(viewer, dump, "-o", out)
    try:
        with open(out) as f:
            doc = json.load(f)
        evs = doc["traceEvents"]
    except (OSError, ValueError, KeyError):
        check(False, "-o writes Chrome Trace Event JSON", j.stderr[-300:])
        return fails
    tracks = sorted((e["tid"], e["args"]["name"]) for e in evs if e.get("name") == "thread_name")
    check(tracks == [(0, "CPU 0"), (1, "CPU 1")], "one track per CPU, named", str(tracks))
    slices = sorted((e["tid"], e["name"], round(e["ts"], 3), round(e["dur"], 3)) for e in evs if e["ph"] == "X")
    check(slices == SLICES, "a slice for each stretch a thread ran between switches", str(slices))
    inst = [e for e in evs if e["ph"] == "i"]
    check(len(inst) == 11, "an instant for every record", f"{len(inst)} instants")
    enter = [e for e in inst if e["name"] == "sys_enter"]
    check(len(enter) == 1 and enter[0]["args"].get("nr") == "read" and enter[0]["tid"] == 0,
          "...decoded: the syscall named, on its core's track", str(enter))
    unknown = [e for e in inst if e["name"] == "event99"]
    check(len(unknown) == 1, "an event the table does not know is kept, by its id", str([e["name"] for e in inst]))

    s = view(viewer, dump, "--stats")
    check(s.returncode == 0 and s.stdout.startswith("cpu 0: 6 records") and "cpu 1: 5 records" in s.stdout,
          "--stats counts each core's records", s.stdout[:200])

    with open(dump, "rb") as f:
        data = bytearray(f.read())
    bad = os.path.join(tmp, "bad.ktrace")
    struct.pack_into("<I", data, 8, 2)
    with open(bad, "wb") as f:
        f.write(data)
    b = view(viewer, bad, "--stats")
    check(b.returncode != 0 and "version 2" in b.stderr, "a dump of another version is refused", b.stderr[:200])
    data[0:8] = b"NOTATRC\0"
    with open(bad, "wb") as f:
        f.write(data)
    b = view(viewer, bad, "--stats")
    check(b.returncode != 0 and "not a kernel trace dump" in b.stderr, "...and one with another magic",
          b.stderr[:200])
    return fails


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--positive-control", action="store_true", help="break it three ways; each must go RED")
    args = ap.parse_args()
    if args.positive_control:
        red = []
        for control in CONTROLS:
            with tempfile.TemporaryDirectory() as tmp:
                exe, viewer = build(tmp, control)
                fails = run(tmp, exe, viewer, True)
            print(f"  {'RED  ' if fails else 'green'} {control[0]}: {len(fails)} check(s) failing")
            red.append(bool(fails))
        ok = all(red)
        print(f"{TOOL}: positive control {'RED as it must be' if ok else 'FAILED to go red'}")
        return 0 if ok else 1
    with tempfile.TemporaryDirectory() as tmp:
        exe, viewer = build(tmp)
        fails = run(tmp, exe, viewer, False)
    print(f"{TOOL}: {'PASS' if not fails else f'FAIL ({len(fails)})'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
