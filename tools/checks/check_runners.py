#!/usr/bin/env python3
"""A test no runner runs: one outside tools/tests/ and hostchecks/, or without a RUNNER header.

A tool's folder names its runner (lib/runner_meta.py), so the old
failure -- a test added to tools/ and to no runner's list, found red
months later -- needs only two things to hold: every test is in a
runner's folder, and every one says what it covers. Also refuses an
unknown RUNNER key (a misspelt `parallel` would silently run a tool
alone) and two tools claiming one name (`-k`/`--only` would pick
either).
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import runner_meta  # noqa: E402

if __name__ == "__main__":
    sys.argv = [sys.argv[0], "--check"]
    sys.exit(runner_meta.main())
