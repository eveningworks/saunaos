"""Where everything in tools/ lives, for scripts that live in its folders.

tools/ is split by role (docs/tools.md): `tests/<runner>/`, `hostchecks/`,
`checks/`, `lib/`, `gen/`, `build/`, `fetch/`, `dev/`, `bench/`,
`runners/`. A script imports this first -- its folder is not `lib/`, so
it puts `lib/` on `sys.path` itself, then:

    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
    import toolpath  # noqa: E402,F401

which puts EVERY tools/ folder on `sys.path`, so `import harness` and
`import filemanager_test` work from anywhere, as they did when tools/
was flat. That only holds while a basename is unique across tools/,
which this module refuses to load without.

    toolpath.REPO               the repository root
    toolpath.TOOLS              tools/ itself
    toolpath.tool("vm.py")      the absolute path of a tool, by basename
"""
import ast
import os
import sys

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = os.path.dirname(TOOLS)

# Data, not scripts: never on sys.path, never searched.
_SKIP = {"__pycache__", "docker", "gdb", "systemd", "w16"}


def _walk():
    for dirpath, dirnames, filenames in os.walk(TOOLS):
        dirnames[:] = sorted(d for d in dirnames if d not in _SKIP)
        yield dirpath, filenames


_INDEX = {}
_DIRS = []
for _dir, _files in _walk():
    if _dir == TOOLS:
        continue
    _scripts = [f for f in _files if f.endswith((".py", ".sh"))]
    if not _scripts:
        continue
    _DIRS.append(_dir)
    for _f in _scripts:
        if _f in _INDEX:
            raise ImportError(f"toolpath: {_f} is in both {_INDEX[_f]} and {_dir}; "
                              "a tools/ basename must be unique")
        _INDEX[_f] = os.path.join(_dir, _f)

for _dir in _DIRS:
    if _dir not in sys.path:
        sys.path.append(_dir)


def tool(name):
    """The absolute path of the tool called `name` (a basename with its extension)."""
    try:
        return _INDEX[name]
    except KeyError:
        raise FileNotFoundError(f"toolpath: no tool named {name!r} under {TOOLS}") from None


def rel(name):
    """`tool(name)` relative to the repository root, as a doc or a message would print it."""
    return os.path.relpath(tool(name), REPO)


def all_tools():
    """Every script under tools/, as {basename: absolute path}."""
    return dict(_INDEX)


# Every folder, in the order docs/tools.md and `tools/t` present them, with
# its one-line role. Its reference file is docs/tools/<folder with - for />.md.
FOLDERS = [
    ("runners", "what runs the tests: preflight, the GUI suite, the sweep, host checks, static rules"),
    ("tests/gate", "run by every preflight"),
    ("tests/gui", "the GUI suite: a fresh image and guest each"),
    ("tests/sweep", "on demand; a missing precondition is a skip"),
    ("tests/manual", "by hand only -- each says why"),
    ("hostchecks", "our C built with the host gcc, judged by a foreign oracle"),
    ("checks", "static rules over the source, all in preflight"),
    ("lib", "the harness: vm.py, qmp_test, gui_debug, harness, toolpath..."),
    ("dev", "working on the OS: remote.py, update/web/samba servers, mutate, predates"),
    ("bench", "measurements with numbers and no verdict"),
    ("build", "what the Makefile and releases call: seed_disk, tfs3_writer, versions"),
    ("gen", "generators of checked-in data and headers"),
    ("fetch", "downloads, each behind its licence"),
]


def folder_of(name):
    """The tools/ folder a tool is in (`tests/gui`), by basename."""
    return os.path.relpath(os.path.dirname(tool(name)), TOOLS)


def summary(path):
    """A tool's one-line description: its docstring's first line, or a
    shell script's first comment line, minus a leading `tools/x.py --`."""
    try:
        text = open(path, encoding="utf-8").read()
        if path.endswith(".py"):
            doc = ast.get_docstring(ast.parse(text)) or ""
        else:
            doc = "\n".join(ln.lstrip("#").strip() for ln in text.splitlines()[1:8]
                            if ln.startswith("#"))
    except (SyntaxError, UnicodeDecodeError, OSError):
        return ""
    line = next((ln.strip() for ln in doc.splitlines() if ln.strip()), "")
    if line.startswith("tools/") and " -- " in line:
        line = line.split(" -- ", 1)[1]
    return line
