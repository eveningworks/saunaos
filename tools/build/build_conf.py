#!/usr/bin/env python3
"""Resolve build.conf: which drivers are modules, and how the build is tuned.

The Makefile calls this twice per build, for the file's two halves:

    python3 tools/build/build_conf.py build.conf
    kernel/drivers/net/e1000.c

    python3 tools/build/build_conf.py build.conf --options
    STRIP ?= 1
    KCMDLINE ?= video=1920x1080 nokaslr

**ONE FILE SAYS HOW THIS KERNEL IS BUILT**, which is FreeBSD's
`sys/amd64/conf/GENERIC` (its `device` and `options` lines together) and
Linux's `.config` (which drivers are modules, beside every other
setting). The driver half came first; the options half joined it rather
than starting a second file, because "what is in this kernel" and "how
is it built" are one question asked twice.

    <name> = builtin | module      a driver
    option <name> = <value>        a build setting

Each `<name> = module` line names a driver by its source file's basename
under kernel/drivers/; the file is found by walking that tree, so there
is no name -> path table to keep true. `builtin` lines are accepted and
print nothing. An unknown name, an ambiguous one (two files with the
same basename) or a value other than the two words is an ERROR, exit
non-zero, because a misspelt module silently becoming builtin is the
kind of failure nothing else would notice.

**THE SAME STRICTNESS APPLIES TO OPTIONS, AND FOR THE SAME REASON.** An
unknown option name is an error, not a line quietly ignored: `option
strp = no` doing nothing is exactly as invisible as a misspelt driver,
and the consequence -- a build that is not configured the way the file
says -- is worse, because the file is what somebody will believe.

**A COMMAND-LINE VARIABLE STILL WINS**, because the emitted assignments
are `?=` and make ranks a command-line variable above any makefile
assignment. So the file is the checkout's default and `make STRIP=0` is
this one build, which is the precedence anyone would expect.

Another file: `make iso BUILD_CONF=my.conf`.

**`--local build.local.conf` IS READ ON TOP** (untracked, gitignored):
what `make menuconfig` saves, Linux's `.config` beside a committed
`defconfig`. A line there replaces build.conf's for this checkout only,
so an experiment cannot be committed by accident; `--options` names the
overrides in a comment, which keeps such a build from being published.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
REPO = toolpath.REPO
DRIVERS = os.path.join(REPO, "kernel", "drivers")

# EVERY OPTION, ITS MAKE VARIABLE AND WHAT IT ACCEPTS. A table rather
# than a chain of ifs, which is this project's rule for anything that
# grows by one entry at a time -- and it is what lets an unknown name be
# rejected with the list of real ones.
#
#   bool   yes/no/on/off/1/0  -> 1 or 0
#   int    digits
#   text   anything, including spaces
OPTIONS = {
    "strip":        ("STRIP", "bool",
                     "split the kernel's debug info into build/kernel.debug"),
    "compress":     ("COMPRESS", "bool",
                     "gzip the live filesystem image into the ISO"),
    "cmdline":      ("KCMDLINE", "text",
                     "boot words baked into both media (docs/boot-flags.md)"),
    "grub_timeout": ("GRUB_TIMEOUT", "int",
                     "seconds GRUB shows its menu; 0 boots straight through"),
    # THE ONLY OPTION HERE THAT REACHES THE NETWORK, which is why it is
    # called out rather than listed quietly. `yes` makes `make iso` fetch
    # the optional differently-licensed material -- and since this file
    # is TRACKED, committing `yes` would turn every clone's build into
    # one that downloads. The licence prompt still gates it, and a build
    # with no terminal is still refused rather than prompted.
    # The timer. Build options because they shape compiled code (Linux's
    # CONFIG_HZ / NO_HZ_IDLE / HIGH_RES_TIMERS); `nohz=off` and
    # `highres=off` still override the last two for one boot.
    "hz":           ("HZ", ("100", "250", "300", "500", "1000"),
                     "kernel tick rate; ring 3's SYS_TICKS stays 100/s"),
    "tick":         ("TICK", ("idle", "periodic"),
                     "idle stops the tick when nothing runs"),
    "highres":      ("HIGHRES", "bool",
                     "one-shot timer deadlines instead of tick-granular ones"),
    "extras":       ("EXTRAS", "bool",
                     "fetch optional differently-licensed material (NETWORK)"),
    # The debug builds, Linux's CONFIG_UBSAN / CONFIG_KASAN /
    # CONFIG_DEBUG_PREEMPT. Each changes every object, and the Makefile's
    # .sanitize-flag stamp rebuilds the tree when one moves.
    "ubsan":        ("UBSAN", "bool",
                     "undefined-behaviour sanitizer, kernel and ring 3"),
    "kasan":        ("KASAN", "bool",
                     "kernel address sanitizer (doubles kernel stacks)"),
    "debug_preempt": ("DEBUG_PREEMPT", "bool",
                     "log per-core reads made while preemptible"),
    # On in every checkout because every test tool drives the guest
    # through it; release media pass DEBUGCON=0 on the command line.
    "debugcon":     ("DEBUGCON", "bool",
                     "bake the serial debug console's boot word into the media"),
}

# How `make menuconfig` (tools/build/menuconfig.py) groups them, in its
# order. Every OPTIONS key appears once; menuconfig_hostcheck.py checks it.
GROUPS = [
    ("Media and boot", ["strip", "compress", "cmdline", "grub_timeout"]),
    ("Timer", ["hz", "tick", "highres"]),
    ("Debugging", ["ubsan", "kasan", "debug_preempt", "debugcon"]),
    ("Network", ["extras"]),
]

BOOL_TRUE = ("yes", "on", "true", "1")
BOOL_FALSE = ("no", "off", "false", "0")


def parse(path):
    """(kind, name, value, lineno) per non-comment line; kind is
    'driver' or 'option'."""
    out = []
    with open(path) as f:
        for n, line in enumerate(f, 1):
            s = line.split("#", 1)[0].strip()
            if not s:
                continue
            if "=" not in s:
                sys.exit(f"{path}:{n}: expected `<name> = builtin|module` "
                         f"or `option <name> = <value>`")
            name, value = (x.strip() for x in s.split("=", 1))
            if name.startswith("option ") or name.startswith("option\t"):
                opt = name[len("option"):].strip()
                if not opt:
                    sys.exit(f"{path}:{n}: `option` with no name")
                out.append(("option", opt, value, n))
                continue
            if value not in ("builtin", "module"):
                sys.exit(f"{path}:{n}: {name!r}: value must be builtin or module, "
                         f"not {value!r}")
            out.append(("driver", name, value, n))
    return out


def find_source(name):
    hits = []
    for root, _dirs, files in os.walk(DRIVERS):
        if name + ".c" in files:
            hits.append(os.path.relpath(os.path.join(root, name + ".c"), REPO))
    return sorted(hits)


def option_value(path, n, opt, value):
    """The make-side value, or exit with what was wrong."""
    var, kind, _help = OPTIONS[opt]
    if kind == "bool":
        low = value.lower()
        if low in BOOL_TRUE:
            return var, "1"
        if low in BOOL_FALSE:
            return var, "0"
        sys.exit(f"{path}:{n}: option {opt}: want yes or no, not {value!r}")
    if isinstance(kind, tuple):
        if value not in kind:
            sys.exit(f"{path}:{n}: option {opt}: want one of "
                     f"{', '.join(kind)}, not {value!r}")
        return var, value
    if kind == "int":
        if not value.isdigit():
            sys.exit(f"{path}:{n}: option {opt}: want a number, not {value!r}")
        return var, value
    # text: anything, including empty and including spaces. A make
    # comment character would end the assignment early, so it is refused
    # rather than silently truncating the value.
    if "#" in value:
        sys.exit(f"{path}:{n}: option {opt}: a '#' cannot appear in a value")
    return var, value


def _read_one(path, options, drivers, source):
    """Fold one file's lines into `options`/`drivers` (name -> (value,
    source)), checking it alone: a name set twice IN ONE FILE is an
    error, while the local file setting what build.conf set is the point."""
    seen_driver, seen_option = {}, {}
    for kind, name, value, n in parse(path):
        if kind == "option":
            if name not in OPTIONS:
                known = ", ".join(sorted(OPTIONS))
                sys.exit(f"{path}:{n}: unknown option {name!r}; the options "
                         f"are: {known}")
            if name in seen_option:
                sys.exit(f"{path}:{n}: option {name} is set twice "
                         f"(first at line {seen_option[name]})")
            seen_option[name] = n
            option_value(path, n, name, value)   # refuses a bad value
            options[name] = (value, source)
            continue
        if name in seen_driver:
            sys.exit(f"{path}:{n}: {name} is listed twice "
                     f"(first at line {seen_driver[name]})")
        seen_driver[name] = n
        hits = find_source(name)
        if not hits:
            sys.exit(f"{path}:{n}: no kernel/drivers/**/{name}.c")
        if len(hits) > 1:
            sys.exit(f"{path}:{n}: {name} is ambiguous: {', '.join(hits)}")
        drivers[name] = (value, source)


def load(path, local=None):
    """build.conf, then the untracked local file on top of it (when it
    exists): ({option: (value, source)}, {driver: (value, source)}),
    `source` being the file that set it. Exits on anything wrong, naming
    the file and line."""
    if not os.path.exists(path):
        sys.exit(f"build_conf: {path}: no such file")
    options, drivers = {}, {}
    _read_one(path, options, drivers, path)
    if local and os.path.exists(local):
        _read_one(local, options, drivers, local)
    return options, drivers


def overrides(path, options, drivers, local):
    """`name = value` for each line the local file set to something
    build.conf does not already say -- `kasan = off` against build.conf's
    `no` is the same build. A driver build.conf does not list is builtin."""
    base_o, base_d = load(path)
    out = []
    for k, (v, src) in options.items():
        if src != local:
            continue
        if k in base_o and option_value(path, 0, k, base_o[k][0]) == option_value(path, 0, k, v):
            continue
        out.append(f"{k} = {v}")
    for k, (v, src) in drivers.items():
        if src == local and v != base_d.get(k, ("builtin", None))[0]:
            out.append(f"{k} = {v}")
    return out


def main():
    args = list(sys.argv[1:])
    mode = None
    for flag in ("--options", "--overrides"):
        if flag in args:
            args.remove(flag)
            mode = flag
    local = None
    if "--local" in args:
        i = args.index("--local")
        if i + 1 >= len(args):
            sys.exit("build_conf: --local needs a file")
        local = args[i + 1]
        del args[i:i + 2]
    if len(args) != 1:
        sys.exit("usage: build_conf.py <build.conf> [--local FILE] [--options | --overrides]")
    path = args[0]
    options, drivers = load(path, local)

    if mode == "--overrides":
        print("\n".join(overrides(path, options, drivers, local)))
        return 0
    if mode == "--options":
        # ONE PER LINE, into a file the Makefile includes -- not a
        # $(shell) whose newlines collapse to spaces, which would split
        # `KCMDLINE ?= video=1920x1080 nokaslr` into two assignments.
        lines = []
        for name, (value, _src) in options.items():
            var, val = option_value(path, 0, name, value)
            lines.append(f"{var} ?= {val}")
        over = overrides(path, options, drivers, local)
        if over:
            # Read by update_server.py, which will not publish such a
            # build, and by preflight.sh, which names it.
            lines.append(f"# {os.path.basename(local)} overrides: " + ", ".join(over))
        print("\n".join(lines))
        return 0
    sources = []
    for name, (value, _src) in sorted(drivers.items()):
        if value == "module":
            sources.append(find_source(name)[0])
    print(" ".join(sources))
    return 0


if __name__ == "__main__":
    sys.exit(main())
