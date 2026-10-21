#!/usr/bin/env python3
"""Check userland/lib/uhlpmac.c (the WinHelp macro language) on the HOST.

WHAT IT CHECKS. A table of macro texts, each with the EVENTS it must cause (a jump, a command, a
button, a mark, an `unsupported`), against a driver that records what the library asks of it:

  - the name forms: case, the short forms (JI JC PI PC IF IE CB), no parentheses at all
  - separators `:` and `;`, whitespace, trailing separators
  - strings: "..." with \\" and \\\\, `...' (nesting), '...', commas and parentheses inside one
  - numbers: decimal, 0x hex, negative
  - nesting: IfThen(IsMark("a"), "...") runs its string as a macro, Not(), IfThenElse(), an answer
    passed as an argument, a string that does not parse
  - REJECTION: a syntax error anywhere runs NOTHING, even the macros before it (a parser rejects
    rather than guesses), and the reason comes back
  - the limits: macros nested in strings stop at UHLPMAC_DEPTH and SAY so; a hostile `A(A(A(...`
    is refused, not a stack overflow
  - what is known and not done (ExecProgram is refused on purpose) says why, and the rest carries on

Then the same corpus mutated -- bytes flipped, cut off, brackets and quotes dropped -- under ASan and
UBSan must parse or refuse and never read outside the text or leak.

  python3 tools/uhlpmac_hostcheck.py
  python3 tools/uhlpmac_hostcheck.py --fuzz 20000 --seed 5
  python3 tools/uhlpmac_hostcheck.py --positive-control

**A CLEAN RUN PROVES NOTHING UNTIL --positive-control HAS GONE RED**: a patched copy of the library
that runs the ELSE branch for a true condition must fail the table, and one that skips the end-of-string
check must be caught by the fuzz. Needs gcc.
"""
import argparse
import os
import subprocess
import sys
import tempfile

import hostcheck

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SAN = ("-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
       "-fno-sanitize-recover=undefined", "-fno-omit-frame-pointer")

DRIVER = r"""
#include "lib/uhlpmac.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char marks[16][32];
static int nmarks;

static void jump(void *c, int popup, int target, const char *file, const char *id, unsigned long num) {
    (void)c; printf("jump %d %d [%s] [%s] %lu\n", popup, target, file, id, num);
}
static void command(void *c, int cmd) { (void)c; printf("cmd %d\n", cmd); }
static void button(void *c, int op, const char *id, const char *cap, const char *mac) {
    (void)c; printf("button %d [%s] [%s] [%s]\n", op, id, cap, mac);
}
static int mark(void *c, int op, const char *name) {
    (void)c;
    int at = -1;
    for (int i = 0; i < nmarks; i++) if (!strcmp(marks[i], name)) at = i;
    if (op == UHLPMAC_MARK_SAVE && at < 0 && nmarks < 16) snprintf(marks[nmarks++], 32, "%s", name);
    if (op == UHLPMAC_MARK_DELETE && at >= 0) { memmove(marks[at], marks[at + 1], 32 * (size_t)(nmarks - at - 1)); nmarks--; }
    printf("mark %d [%s]\n", op, name);
    return op == UHLPMAC_MARK_IS ? at >= 0 : 0;
}
static void unsupported(void *c, const char *name, const char *why) { (void)c; printf("unsupported [%s] [%s]\n", name, why); }

static unsigned long long rng;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned)(rng >> 16); }

// run <text> | fuzz <seed> <iterations> <corpus file, one macro per line>
int main(int argc, char **argv) {
    struct uhlpmac_ops ops = { 0, jump, command, button, mark, unsupported };
    if (argc >= 3 && !strcmp(argv[1], "run")) {
        const char *err = "";
        int n = uhlpmac_run(argv[2], &ops, &err);
        printf("result %d [%s]\n", n, err);
        return 0;
    }
    if (argc < 5 || strcmp(argv[1], "fuzz")) return 2;
    rng = 0x9E3779B97F4A7C15ull ^ strtoull(argv[2], 0, 10);
    int iters = atoi(argv[3]);
    FILE *f = fopen(argv[4], "r");
    if (!f) return 2;
    static char corpus[256][512];
    int nc = 0;
    while (nc < 256 && fgets(corpus[nc], sizeof corpus[nc], f)) { corpus[nc][strcspn(corpus[nc], "\n")] = 0; nc++; }
    fclose(f);
    freopen("/dev/null", "w", stdout);
    int ran = 0;
    for (int it = 0; it < iters; it++) {
        char buf[600];
        snprintf(buf, sizeof buf, "%s", corpus[rnd() % (unsigned)nc]);
        size_t len = strlen(buf);
        int muts = 1 + (int)(rnd() % 4);
        for (int m = 0; m < muts && len; m++) {
            switch (rnd() % 6) {
            case 0: buf[rnd() % len] = (char)rnd(); break;
            case 1: len = rnd() % (len + 1); buf[len] = 0; break;                 // cut off
            case 2: { size_t at = rnd() % len; memmove(buf + at, buf + at + 1, len - at); len--; break; }  // drop a byte
            case 3: { const char *q = "\"`'(),:;\\"; buf[rnd() % len] = q[rnd() % 9]; break; }
            case 4: if (len + 8 < sizeof buf) { size_t at = rnd() % (len + 1); memmove(buf + at + 4, buf + at, len - at + 1); memcpy(buf + at, "A((\"", 4); len += 4; } break;
            default: if (len + 1 < sizeof buf) { size_t at = rnd() % (len + 1); memmove(buf + at + 1, buf + at, len - at + 1); buf[at] = (char)(' ' + rnd() % 90); len++; } break;
            }
        }
        const char *err;
        if (uhlpmac_run(buf, &ops, &err) >= 0) ran++;
    }
    freopen("/dev/tty", "w", stdout);
    printf("fuzz: %d texts, %d ran\n", iters, ran);
    return 0;
}
"""

# (macro text, expected output lines). Jump args: popup target file id num. target: 0 id 1 context 2 hash 3 keyword 4 toc.
CASES = [
    ('JumpId("a.hlp", "ctx1")', ["jump 0 0 [a.hlp] [ctx1] 0", "result 1 []"]),
    ("jumpid(`a.hlp',`ctx1')", ["jump 0 0 [a.hlp] [ctx1] 0", "result 1 []"]),
    ("JI('', 'start')", ["jump 0 0 [] [start] 0", "result 1 []"]),
    ('PopupId("", "gloss")', ["jump 1 0 [] [gloss] 0", "result 1 []"]),
    ('PI("", "gloss")', ["jump 1 0 [] [gloss] 0", "result 1 []"]),
    ('JumpContext("", 100)', ["jump 0 1 [] [] 100", "result 1 []"]),
    ('JC("", 0x20)', ["jump 0 1 [] [] 32", "result 1 []"]),
    ('PopupContext("f.hlp", 7)', ["jump 1 1 [f.hlp] [] 7", "result 1 []"]),
    ('JumpHash("", 0xDEADBEEF)', ["jump 0 2 [] [] 3735928559", "result 1 []"]),
    ('JumpKeyword("", "Printing")', ["jump 0 3 [] [Printing] 0", "result 1 []"]),
    ('JumpContents("other.hlp")', ["jump 0 4 [other.hlp] [] 0", "result 1 []"]),
    ("Contents()", ["cmd 1", "result 1 []"]),
    ("Contents", ["cmd 1", "result 1 []"]),
    ("Search();Back();Next(); Prev( ) ; History()", ["cmd 2", "cmd 3", "cmd 4", "cmd 5", "cmd 6", "result 5 []"]),
    ("Contents():Exit();", ["cmd 1", "cmd 7", "result 2 []"]),
    ("  ::  Back() ;; ", ["cmd 3", "result 1 []"]),
    ("BrowseButtons()", ["cmd 9", "result 1 []"]),
    ("CopyTopic()", ["cmd 8", "result 1 []"]),
    ('CreateButton("btn_up", "&Up", "JumpId(`a.hlp\',`top\')")',
     ["button 0 [btn_up] [&Up] [JumpId(`a.hlp',`top')]", "result 1 []"]),
    ('CB("b", "B", "Back()")', ["button 0 [b] [B] [Back()]", "result 1 []"]),
    ('DestroyButton("b")', ["button 1 [b] [] []", "result 1 []"]),
    ('EnableButton("b"):DisableButton("b")', ["button 2 [b] [] []", "button 3 [b] [] []", "result 2 []"]),
    ('ChangeButtonBinding("b", "Contents()")', ["button 4 [b] [] [Contents()]", "result 1 []"]),
    ('SaveMark("m"):IsMark("m")', ["mark 0 [m]", "mark 2 [m]", "result 2 []"]),
    ('IfThen(IsMark("m"), "Contents()")', ["mark 2 [m]", "result 2 []"]),
    ('SaveMark("m"):IfThen(IsMark("m"), "Contents()")',
     ["mark 0 [m]", "mark 2 [m]", "cmd 1", "result 4 []"]),
    ('IfThen(Not(IsMark("seen")), "SaveMark(`seen\')")',
     ["mark 2 [seen]", "mark 0 [seen]", "result 4 []"]),
    ('SaveMark("x"):IfThenElse(IsMark("x"), "Back()", "Next()")',
     ["mark 0 [x]", "mark 2 [x]", "cmd 3", "result 4 []"]),
    ('IfThenElse(IsMark("none"), "Back()", "Next()")', ["mark 2 [none]", "cmd 4", "result 3 []"]),
    ('GotoMark("m"):DeleteMark("m")', ["mark 3 [m]", "mark 1 [m]", "result 2 []"]),
    # a string given to IfThen that does not parse is reported, the rest of the macros carry on
    ('SaveMark("q"):IfThen(IsMark("q"), "Back(("):Contents()',
     ["mark 0 [q]", "mark 2 [q]", "unsupported [(text)] [an argument was expected]", "cmd 1", "result 4 []"]),
    # known and not done, and refused on purpose
    ('ExecProgram("cmd.exe", 0)', ["unsupported [ExecProgram] [a help file does not start programs here]", "result 1 []"]),
    ('Annotate()', ["unsupported [Annotate] [annotations are not built yet]", "result 1 []"]),
    ('NoSuchMacro(1, 2)', ["unsupported [NoSuchMacro] [not a macro this viewer knows]", "result 1 []"]),
    # strings
    (r'JumpId("a\"b", "c\\d")', ['jump 0 0 [a"b] [c\\d] 0', "result 1 []"]),
    ('JumpId("a,b(c)", "d;e:f")', ["jump 0 0 [a,b(c)] [d;e:f] 0", "result 1 []"]),
    ("JumpId(`x `y' z', 'w')", ["jump 0 0 [x `y' z] [w] 0", "result 1 []"]),
    ("JumpContext('', -3)", ["jump 0 1 [] [] " + str((-3) & 0xFFFFFFFFFFFFFFFF), "result 1 []"]),
    # REJECTED: nothing runs, not even what came first
    ('Contents():Back(', ["result -1 [an argument was expected]"]),
    ('Contents():JumpId("a", "b"', ["result -1 [a , or ) was expected]"]),
    ('Contents() Back()', ["result -1 [a : or ; was expected between macros]"]),
    ('JumpId("a)', ["result -1 [a string is not closed]"]),
    ('Contents():123', ["result -1 [a macro name was expected]"]),
    ('JumpId("a" "b")', ["result -1 [a , or ) was expected]"]),
    ('Back(,)', ["result -1 [an argument was expected]"]),
    # the limits
    ("A(" * 70 + ")" * 70, ["result -1 [macros nested too deeply]"]),
]
# Macros inside strings inside strings, one level too many: the run ends there and says so.
_n = "Back()"
for _ in range(10):
    _n = "IfThen(1, \"" + _n.replace("\\", "\\\\").replace('"', '\\"') + "\")"
DEEP = _n


def build(tmp, patch=None):
    drv = hostcheck.write(tmp, "uhlpmac_host.c", DRIVER)
    src = hostcheck.stage(tmp, "userland/lib/uhlpmac.c", dest="lib/uhlpmac.c", edits=patch or (),
                          apply=bool(patch), tool="uhlpmac_hostcheck")
    return hostcheck.compile(tmp, "uhlpmac_host", [drv, src], flags=SAN,
                             includes=[os.path.join(ROOT, "userland")], tool="uhlpmac_hostcheck")


def run(exe, text):
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1", UBSAN_OPTIONS="print_stacktrace=1")
    r = subprocess.run([exe, "run", text], capture_output=True, env=env, timeout=60)
    if r.returncode != 0:
        raise RuntimeError("driver failed (%d) on %r:\n%s" % (r.returncode, text[:80], r.stderr.decode(errors="replace")[:1500]))
    return r.stdout.decode("latin-1").splitlines()


def check_table(exe, problems):
    for text, want in CASES:
        got = run(exe, text)
        if got != want:
            problems.append("%r\n      wanted %r\n      got    %r" % (text[:70], want, got))
    deep = run(exe, DEEP)
    if not any("macros nested too deeply" in ln for ln in deep) or not deep[-1].startswith("result "):
        problems.append("nesting %d strings deep did not say it stopped: %r" % (10, deep[-3:]))
    if any(ln.startswith("cmd") for ln in deep):
        problems.append("the macro at the bottom of a too-deep nesting still ran: %r" % deep[-3:])


def check_fuzz(exe, tmp, seed, iters, problems):
    corpus = os.path.join(tmp, "corpus.txt")
    with open(corpus, "w") as f:
        for text, _ in CASES:
            if "\n" not in text and len(text) < 500:
                f.write(text + "\n")
    r = subprocess.run([exe, "fuzz", str(seed), str(iters), corpus], capture_output=True,
                       env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1", UBSAN_OPTIONS="print_stacktrace=1"), timeout=1800)
    if r.returncode != 0:
        problems.append("fuzz seed %d: exit %d\n%s" % (seed, r.returncode, r.stderr.decode(errors="replace")[:1500]))
        return None
    return "fuzz ok"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--fuzz", type=int, default=6000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--positive-control", action="store_true")
    args = ap.parse_args()
    if args.positive_control:
        return positive_control(args)
    problems = []
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        try:
            check_table(exe, problems)
        except RuntimeError as e:
            problems.append(str(e))
        check_fuzz(exe, tmp, args.seed, args.fuzz, problems)
    if problems:
        for p in problems:
            print("FAIL:", p)
        print("uhlpmac_hostcheck: %d problems" % len(problems))
        return 1
    print("uhlpmac_hostcheck: ok (%d macro texts, fuzz of %d)" % (len(CASES), args.fuzz))
    return 0


def positive_control(args):
    controls = [
        ("the else branch for a true condition",
         [("arg_text(r, n, arg_num(r, n, 0, depth) ? 1 : 2, b1, depth)", "arg_text(r, n, arg_num(r, n, 0, depth) ? 2 : 1, b1, depth)")], "table"),
        ("an unterminated string read past its end",
         [('if (!*p) { free(out); ps->err = "a string is not closed"; return 0; }', "")], "fuzz"),
    ]
    rc = 0
    for name, patch, half in controls:
        problems = []
        with tempfile.TemporaryDirectory() as tmp:
            exe = build(tmp, patch)
            try:
                if half == "table":
                    check_table(exe, problems)
                else:
                    check_fuzz(exe, tmp, args.seed, max(args.fuzz, 3000), problems)
            except RuntimeError as e:
                problems.append(str(e))
        if problems:
            print("control %-42s RED (good): %s" % (name, problems[0].splitlines()[0][:90]))
        else:
            print("control %-42s STAYED GREEN -- this check cannot see that break" % name)
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
