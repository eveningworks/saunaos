#!/usr/bin/env bash
# tools/runners/preflight.sh -- "am I safe to deliver?" in one command.
#
# Every session's delivery step is supposed to end with a clean
# `make clean && make all && make iso` + `boot_smoke_test.py` + `ktest_run.py` run
# against the FINAL state (see CLAUDE.md's "Delivering changes"
# section) -- but that's three separate commands run by hand, and it's
# easy to deliver after only re-running one of them, or after a stray
# uncommitted file got left out. This wraps all of it into a single
# pass/fail so there's one command to run right before committing, not
# three to remember.
#
# What it checks, in order (stops at the first failure):
#   1. make clean && make all -- full rebuild from scratch, no stale .o
#      files hiding a real error.
#   2. make iso -- confirms the ISO actually assembles.
#   2a. make all NOGPL=1 into build/nogpl -- a build with no GPL code in
#       it, failing if ANY object was compiled from a GPL-only port
#       (the Makefile's PORT_LICENSE_* block says which). "It builds" is
#       not the claim; "nothing GPL went into it" is.
#   2b. tools/runners/checks.py -- every static rule in tools/checks/, among
#       them check_deps.py, which confirms every build directory's .d files
#       are actually reaching make. A clean build hides this entirely
#       (nothing is stale when everything was just compiled), which is
#       exactly why it went unnoticed for a directory move; the check
#       is cheap and structural, so it runs here rather than nowhere.
#   2c. tools/runners/hostchecks.py --gate -- the host checks whose RUNNER
#       header says `gate`, in parallel.
#   2d. every tools/tests/gate/ test that boots nothing, found by folder.
#   3.tools/tests/gate/boot_smoke_test.py -- confirms the built ISO boots cleanly.
#   3b. tools/tests/gate/ktest_run.py -- runs the in-kernel test suite and fails
#       the whole preflight if any test failed. "Does it boot" and
#       "does it work" are different questions; this asks the second.
#   3c. tools/tests/gate/usertest_run.py -- the same question one ring out. The
#       KTESTs run INSIDE the kernel and so cannot tell whether a
#       /tests binary still links, loads and passes its own checks;
#       that gap is what libc_test was written into, and without this
#       step it would have been run once by hand and never again.
#   4. git status --short -- just informational: lists what's dirty so
#      you can eyeball it against the file list you are about to report.
#   5. background wait loops -- also informational. A backgrounded poll
#      on an artifact that never appears (`until [ -s out.log ]`) cannot
#      exit, and nothing else in a session ever mentions it again.
#
# The closing message warns if `git config user.name` is unset, since a
# commit would then either fail or carry the wrong identity -- this repo
# commits as `SaunaOS <noreply@saunaos.local>` on purpose (see
# docs/decisions.md's history-scrub entry).
#
# Usage:
#   tools/runners/preflight.sh              # full check
#   tools/runners/preflight.sh --skip-clean # skip `make clean` (faster re-check
#                                    # after preflight already passed
#                                    # once and only docs changed since)
#
# Exit code 0 = safe to deliver. Exit code 1 = stop, something's wrong
# -- read the last command's output above the FAIL line.

set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# GUARDED, because `set -e` is deliberately not in effect here (this is
# `set -uo pipefail`, so each step's status is checked by hand). An
# unguarded cd that failed would run the whole gate -- make clean
# included -- in whatever directory the caller happened to be in.
cd "$repo_root" || { echo "preflight: cannot cd to $repo_root" >&2; exit 1; }

skip_clean=0
for arg in "$@"; do
  case "$arg" in
    --skip-clean) skip_clean=1 ;;
    *) echo "preflight: unknown argument '$arg'" >&2; exit 1 ;;
  esac
done

# THE TEST LOCK (tools/lib/tree_lock.py): refuse to start under another
# test -- `make clean` would pull its build away -- then hold it for the
# whole run, so nothing rebuilds or edits the tree under this one.
python3 tools/lib/tree_lock.py || exit 1
exec 9>"$repo_root/.test-lock"
flock -s 9
mkdir -p "$repo_root/.test-lock.d" && echo "preflight.sh $*" > "$repo_root/.test-lock.d/$$"
export SAUNAOS_TEST_LOCK_HELD=$$

# THE LOCAL TEST HISTORY (tools/lib/test_history.py): this run's output is
# kept and recorded at exit, and every tool it runs names it as parent.
# The original stdout/stderr are saved on 3 and 4 so the exit trap can
# close the pipe into tee -- tee has the log until it has seen EOF.
pf_args="$*"
pf_start=$(date +%s)
pf_reason=""
pf_id="$(date +%Y%m%d-%H%M%S)-preflight-$$"
export SAUNAOS_TEST_HISTORY_PARENT="$pf_id"
pf_log="$(mktemp "${TMPDIR:-/tmp}/preflight.$$.XXXXXX")"
exec 3>&1 4>&2
exec > >(tee -a "$pf_log") 2>&1
pf_tee=$!
pf_exit() {
  local rc=$?
  rm -f "$repo_root/.test-lock.d/$$"
  exec 1>&3 2>&4
  wait "$pf_tee" 2>/dev/null
  SAUNAOS_TEST_HISTORY_PARENT="" python3 tools/lib/test_history.py record --id "$pf_id" \
    --tool preflight --rc "$rc" --duration "$(( $(date +%s) - pf_start ))" \
    --argv "$pf_args" --log "$pf_log" ${pf_reason:+--failure "$pf_reason"}
  rm -f "$pf_log"
}
trap pf_exit EXIT

step() { echo "== $1 =="; }
fail() { pf_reason="$1"; echo "preflight: FAIL -- $1"; exit 1; }

# A VM LEFT RUNNING FAILS THIS GATE MINUTES LATER, AND NOT WHERE YOU
# LOOK. `make iso` re-seeds disk.img while a guest holds a write lock on
# it, and the first thing to actually complain is boot_smoke_test, with
# "qemu exited early (code 1)" -- which reads as the kernel failing to
# boot rather than as a leftover from the previous command. A whole
# clean-build cycle is spent getting to that wrong conclusion.
#
# Checked here rather than in iso_guard.py because this is about a
# PROCESS, not about the ISO being stale, and because the fix is one the
# person running it has to make.
# THE COMMIT IDENTITY IS PER-REPOSITORY, AND A CLONE DOES NOT CARRY IT.
#
# `git config --local user.name/email` lives in .git/config, which is
# not part of what gets cloned -- so a fresh checkout falls back to the
# GLOBAL identity, which for most people is their real name and address.
# This project scrubbed exactly that out of every prior commit with a
# history rewrite (docs/decisions.md), and nothing in git warns you
# before the first commit reintroduces it. By then it is in the history
# and only another rewrite gets it out.
#
# WHAT IS CHECKED, and why it is not the exact value: the hazard is
# "you did not decide, so your global identity leaked in". Demanding one
# specific name would also refuse anyone working on a FORK, who has
# every right to commit as themselves -- so a local identity being SET
# is the hard requirement, and this project's own convention is a note.
#
# In the gate rather than in a git hook because .git/hooks is not cloned
# either: a guard against a clone-time problem has to live somewhere
# that travels with the repository.
conv_name="SaunaOS"
conv_email="noreply@saunaos.local"
local_name="$(git config --local user.name 2>/dev/null || true)"
local_email="$(git config --local user.email 2>/dev/null || true)"
if [ -z "$local_name" ] || [ -z "$local_email" ]; then
  echo "preflight: this repository has no commit identity of its own." >&2
  echo "preflight: it is per-repository and a CLONE DOES NOT CARRY IT, so" >&2
  echo "preflight: commits here would use your GLOBAL identity:" >&2
  echo "preflight:   $(git config user.name 2>/dev/null || echo '(unset)') <$(git config user.email 2>/dev/null || echo '(unset)')>" >&2
  echo "preflight: set one deliberately -- this project's own convention is" >&2
  echo "preflight:   git config --local user.name  '$conv_name'" >&2
  echo "preflight:   git config --local user.email '$conv_email'" >&2
  echo "preflight: on a fork, your own name and address are fine; the point" >&2
  echo "preflight: is that it is a CHOICE rather than a leak." >&2
  exit 1
fi
# A Claude Code cloud session commits as Claude, which its signing key
# verifies on GitHub (docs/decisions/build.md) -- the other convention.
if { [ "$local_name" != "$conv_name" ] || [ "$local_email" != "$conv_email" ]; } &&
   { [ "$local_name" != "Claude" ] || [ "$local_email" != "noreply@anthropic.com" ]; }; then
  echo "preflight: note -- committing as $local_name <$local_email>," >&2
  echo "preflight: not this project's convention ($conv_name <$conv_email>)." >&2
fi

# Glob every instance's pidfile, not a hard-coded 0-3 list: vm.py
# --instance goes to 15 and gui_regress uses up to 16 (DEFAULT_JOBS),
# so a guest on any of those holds disk.img's write lock and must be
# caught here -- the whole point of the check. `.vm.pid` is slot 0;
# `.vm.N.pid` are the rest. nullglob via the `[ -f ]` guard: an unmatched
# glob stays literal and is skipped.
vm_pid_file=""
for f in .vm.pid .vm.*.pid; do
  [ -f "$f" ] || continue
  if kill -0 "$(cat "$f" 2>/dev/null)" 2>/dev/null; then vm_pid_file="$f"; break; fi
done
if [ -n "$vm_pid_file" ]; then
  echo "preflight: a vm.py guest is still running (pid $(cat "$vm_pid_file"), $vm_pid_file)." >&2
  echo "preflight: it holds a write lock on disk.img, which \`make iso\`" >&2
  echo "preflight: re-seeds -- the gate would fail several minutes from now" >&2
  echo "preflight: in boot_smoke_test, looking like a boot failure." >&2
  echo "preflight: run \`python3 tools/lib/vm.py stop\` first." >&2
  exit 1
fi

if [ "$skip_clean" -eq 0 ]; then
  step "make clean && make all"
  make clean >/tmp/preflight_clean.log 2>&1 || fail "make clean (see /tmp/preflight_clean.log)"
  make all >/tmp/preflight_build.log 2>&1 || fail "make all (see /tmp/preflight_build.log)"
else
  step "make all (--skip-clean: no full rebuild)"
  make all >/tmp/preflight_build.log 2>&1 || fail "make all (see /tmp/preflight_build.log)"
fi

step "make iso"
make iso >/tmp/preflight_iso.log 2>&1 || fail "make iso (see /tmp/preflight_iso.log)"

step "make all NOGPL=1 (nothing compiled from a GPL-only port)"
make all NOGPL=1 BUILD=build/nogpl >/tmp/preflight_nogpl.log 2>&1 ||
    fail "make all NOGPL=1 (see /tmp/preflight_nogpl.log)"
for p in $(make -s gpl-only-ports); do
    leak=$(find build/nogpl -path "*/ports/$p/*" -name '*.o' | head -3)
    [ -z "$leak" ] || fail "NOGPL=1 compiled GPL-only port $p: $leak -- a program built from it is missing from PORT_PROGRAMS_$p"
done
# GONE once looked at: check_deps.py walks every directory under build/,
# and this tree's .d files rightly reach no make but its own.
rm -rf build/nogpl

# Every static rule in tools/checks/ -- found by being there, so a new one
# is in this gate the moment it is saved. Several need the build above
# (check_deps reads build/'s .d files, check_layout the seeded image).
step "runners/checks.py (every tools/checks/ rule: widget ops, key routing, docs, layout, ...)"
python3 tools/runners/checks.py || fail "static checks (the FAILED block above names the rule)"

step "test_history.py --selftest (the local run history records, reads and prunes)"
python3 tools/lib/test_history.py --selftest || fail "test history selftest"

# The baked font's header is GENERATED, and regenerating it needs a font
# most checkouts do not have installed -- so a hand-edit there survives
# until somebody who DOES have it regenerates and silently reverts it.
# Comment-and-counts only; no font needed to run this.
step "genttf.py --check (the generated font header matches its generator)"
python3 tools/gen/genttf.py --check || fail "generated font header check"

# The host checks that gate: their RUNNER says `gate`. Host-only and in
# parallel; each one's reason to be here is in its own header.
step "runners/hostchecks.py --gate (host-built code against foreign oracles)"
python3 tools/runners/hostchecks.py --gate || fail "gate host checks"

# Every tools/tests/gate/ test that boots nothing, each named by its
# RUNNER header. The three that boot run after, in their own order.
while IFS=$'\t' read -r rel name covers; do
  case "$name" in boot_smoke|ktest_run|usertest_run) continue ;; esac
  step "$name ($covers)"
  python3 "$rel" || fail "$name"
done < <(python3 tools/lib/runner_meta.py --folder tests/gate)

step "boot_smoke_test.py"
python3 tools/tests/gate/boot_smoke_test.py || fail "boot smoke test"

step "ktest (in-kernel test suite)"
python3 tools/tests/gate/ktest_run.py || fail "kernel test suite"

# The ring-3 side of the same question. ktest runs INSIDE the kernel, so
# it cannot see whether a /tests binary still links and runs at all --
# which is exactly the gap libc_test was written into. Boots once more
# against a temporary copy of disk.img; see the tool's docstring for
# what it deliberately does NOT run and why.
step "usertest_run.py (ring-3 /tests diagnostics)"
python3 tools/tests/gate/usertest_run.py || fail "ring-3 userland tests"

step "git status --short (informational -- compare against your delivery file list)"
git status --short || true

# LINGERING WAIT LOOPS. A session that backgrounds a poll on an artifact
# which never comes to exist leaves a shell spinning until the session
# ends -- `until [ -s out.log ]` cannot tell "not yet" from "never", and
# a superseded run's log is never. Five were found six hours old in one
# session, and only because someone asked what was running.
#
# **THE SHAPE TO MATCH IS `sleep` INSIDE A LOOP, NOT `[`.** This used to
# look for `(until|while) *\[`, which sees only the loops whose test is a
# bracket expression -- so `while ps aux | grep -q foo; do sleep 15; done`
# was invisible to it. Two of those ran for nearly two hours in one
# session and this step printed `none` throughout, because the pattern
# they can never escape (a poll matching its OWN command line) is also
# the pattern this was not looking for.
#
# Informational, never a failure: an interactive session legitimately
# has shells of its own, and this cannot tell them apart. It only says
# what is there, which is the part nobody thinks to look at.
step "background wait loops (informational -- close what is no longer needed)"
# EXCLUDING THIS SCRIPT'S OWN ANCESTRY. The match is over command
# LINES, so a shell whose command merely CONTAINS the words -- this
# comment being written, for instance -- looks exactly like a loop
# running them. The invoking shell is never the leak, so drop it and
# everything above it.
mine=""
pid=$$
while [ -n "$pid" ] && [ "$pid" != "1" ] && [ "$pid" != "0" ]; do
    mine="$mine $pid"
    pid=$(ps -o ppid= -p "$pid" 2>/dev/null | tr -d ' ')
done
# shellcheck disable=SC2009  # pgrep cannot match the loop's shape, and
# the elapsed time is half the answer -- a waiter minutes old is normal,
# one hours old is the leak.
waiters=$(ps -eo pid,etime,args 2>/dev/null \
          | grep -E '(until|while).*;[[:space:]]*do.*sleep' | grep -v grep \
          | while read -r wpid rest; do
                case " $mine " in *" $wpid "*) continue ;; esac
                echo "$wpid $rest"
            done || true)
if [ -n "$waiters" ]; then
    printf '  %s\n' "$waiters"
    echo "  ^ each of these is polling for something. If the run it waits"
    echo "    on is finished or superseded, kill it -- and prefer the"
    echo "    background job's OWN completion signal over a waiter beside"
    echo "    it, which is redundant even when it works (CLAUDE.md)."
    echo "    A waiter that greps \`ps\` for a name its OWN command line"
    echo "    carries matches itself and can NEVER exit: wait on a PID"
    echo "    (kill -0) or an artifact, and bound it -- tools/dev/wait_for.sh."
else
    echo "  none"
fi

echo
python3 tools/lib/preflight_stamp.py --write || true   # what update_server.py --publish checks
echo "preflight: PASS -- build, iso, boot smoke test, ktest and the"
echo "preflight: ring-3 /tests diagnostics all clean."
if [ -z "$(git config user.name 2>/dev/null)" ]; then
  echo "preflight: WARNING -- no git identity configured. This repo commits"
  echo "preflight: as SaunaOS <noreply@saunaos.local>; set it before committing"
  echo "preflight: so a real name cannot leak into history (see CLAUDE.md)."
else
  echo "preflight: safe to commit; push verified work (see CLAUDE.md)."
fi
