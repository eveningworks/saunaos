"""tools/lib/ls_long.py -- read `/bin/ls -l`'s long form (userland/utils/ls.c).

    -rw-r--r--  user  user         312 27.11.2026 17:02:00  notes.txt
    drwsr-xr-x  user  user             27.11.2026 17:06:00  Desktop/

The type and mode, the owner, the group, the size (blank for a folder), the
time in the locale's spelling (more than one word), the name. One parser
here so a column added to the long form is fixed in one place, not in every
test that reads a file's size from it.
"""

import re

_MODE = re.compile(r"^[-d][-rwxsStT]{9}$")


def parse(line):
    """A dict (type, mode, owner, group, size, line) for a long-form line,
    or None. `size` is None for a folder."""
    p = line.split()
    if len(p) < 5 or not _MODE.match(p[0]):
        return None
    size = None
    if p[0][0] == "-":
        try:
            size = int(p[3])
        except ValueError:
            return None
    return {"type": p[0][0], "mode": p[0][1:], "owner": p[1], "group": p[2],
            "size": size, "line": line.rstrip()}


def entries(lines):
    """Every long-form entry in `lines` (a string or an iterable of lines)."""
    if isinstance(lines, str):
        lines = lines.splitlines()
    return [e for e in (parse(ln) for ln in lines) if e]


def name_of(e):
    """The entry's name: its last word, as the callers before this read it."""
    return e["line"].split()[-1]


def size(lines, name):
    """The size of the FILE `name` in a listing of its folder, or -1."""
    for e in entries(lines):
        if e["type"] == "-" and name_of(e) == name:
            return e["size"]
    return -1
