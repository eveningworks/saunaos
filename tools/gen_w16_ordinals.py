#!/usr/bin/env python3
"""tools/gen_w16_ordinals.py -- the Win16 API's ordinals and names, for w16.

A Win16 program imports by ORDINAL ("USER.41"), so w16 needs the name
behind each number to say what a program calls (`w16 --info`) and to
name an import it has not implemented -- and a KIND for each (Dialogs,
Files, Clipboard...), from the rules in KIND_RULES below, so a list of
what a program lacks reads as what it needs. The authoritative public list is
Wine's 16-bit .spec files; this reads them and writes
userland/w16/w16_names.c -- ordinal, name and the bytes of Pascal
arguments each function pops, which are facts of the Windows ABI. The
spec files are not vendored: fetch them yourself (the tool prints the
URLs) and pass their directory, so the build never reaches the network.

    python3 tools/gen_w16_ordinals.py --urls
    python3 tools/gen_w16_ordinals.py --spec-dir DIR
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "userland", "w16", "w16_names.c")
WINE_TAG = "wine-9.0"
MODULES = [("KERNEL", "krnl386.exe16"), ("USER", "user.exe16"), ("GDI", "gdi.exe16"),
           ("COMMDLG", "commdlg.dll16"), ("SHELL", "shell.dll16"), ("KEYBOARD", "keyboard.drv16"),
           ("SOUND", "sound.drv16"), ("SYSTEM", "system.drv16"), ("TOOLHELP", "toolhelp.dll16"),
           ("MMSYSTEM", "mmsystem.dll16"), ("DDEML", "ddeml.dll16"), ("OLECLI", "olecli.dll16"),
           ("OLESVR", "olesvr.dll16"), ("LZEXPAND", "lzexpand.dll16"), ("VER", "ver.dll16"),
           ("WIN87EM", "win87em.dll16")]

# WHAT A FUNCTION IS FOR, the first matching rule winning: (module or "*",
# a regex on the name, the kind). A module with one purpose names it once.
KIND_RULES = [
    ("KERNEL", r"^__", "Constants"),
    ("KERNEL", r"Profile", "INI files"),
    ("KERNEL", r"^_l|File|PathName|Drive|Directory|^DeleteFile", "Files"),
    ("KERNEL", r"PDB|DOS|Dos|NetBIOS|EMS|A20|^WinOldAp", "DOS"),
    ("KERNEL", r"^WinExec|^GetCurPID", "Tasks"),
    ("KERNEL", r"FreeSpace|HeapSpaces|HandleCount|LongPtrAdd|Swap", "Memory"),
    ("KERNEL", r"^Local", "Local heap"),
    ("KERNEL", r"^Global|Selector|Segment|^Alloc|^Free.*Sel|Lock|Code|Data", "Memory"),
    ("KERNEL", r"Resource|^Load|^Find|Access", "Resources"),
    ("KERNEL", r"Module|Library|ProcAddress|Instance|^MakeProcInstance|^FreeProcInstance", "Modules"),
    ("KERNEL", r"Atom", "Atoms"),
    ("KERNEL", r"Task|Yield|Event|^InitTask|WaitEvent|Exit|Fatal", "Tasks"),
    ("KERNEL", r"^lstr|^Ansi|^Is[A-Z]|Char", "Strings"),
    ("KERNEL", r"", "System"),
    ("USER", r"Dlg|Dialog|MessageBox|MessageBeep", "Dialogs"),
    ("USER", r"Clipboard", "Clipboard"),
    ("USER", r"Comm", "Serial ports"),
    ("USER", r"Atom", "Atoms"),
    ("USER", r"Driver", "Drivers"),
    ("USER", r"Palette", "Colours"),
    ("USER", r"ClientToScreen|ScreenToClient|MapWindowPoints", "Coordinates"),
    ("USER", r"Menu", "Menus"),
    ("USER", r"Scroll", "Scroll bars"),
    ("USER", r"Caret", "Caret"),
    ("USER", r"Cursor|Capture", "Mouse"),
    ("USER", r"Key|Focus|Accelerator|Hotkey", "Keyboard"),
    ("USER", r"Timer", "Timers"),
    ("USER", r"Hook", "Hooks"),
    ("USER", r"Message|^Post|^Send|^Peek|^Dispatch|^Translate|^Reply|^InSend|Quit", "Messages"),
    ("USER", r"Paint|Invalidate|Validate|Update|Redraw|^GetDC|^ReleaseDC|^GetWindowDC|DrawText|DrawIcon|FillRect|FrameRect|InvertRect|DrawFocus|^GrayString|TabbedText|^GetSysColor|^SetSysColor", "Painting"),
    ("USER", r"Rect|Point", "Rectangles"),
    ("USER", r"Icon|Bitmap|LoadString|LoadAccel|^Load", "Resources"),
    ("USER", r"^lstr|^Ansi|^wsprintf|^wvsprintf|^IsChar|Char", "Strings"),
    ("USER", r"Class|Window|Wnd|Parent|Child|Show|^Move|^Close|^Open|^Enable|Prop|^SetActive|^GetActive|^Arrange|^Cascade|^Tile|Desktop|Text|^Destroy|^Create", "Windows"),
    ("USER", r"WNDPROC|Zoomed|Iconic|Popup|RadioButton|CheckDlg|Wallpaper|DeskPattern", "Windows"),
    ("USER", r"", "System"),
    ("GDI", r"Escape|Print|Abort|Doc|Page|Spool|Device|^SetAbort|PQ$|Job|^DM|WriteDialog", "Printing"),
    ("GDI", r"^Engine|Glyph|ENGINE", "Text"),
    ("GDI", r"MulDiv|^RCOS|^RSIN", "Maths"),
    ("GDI", r"Text|Font|Char|^GetTextExtent|Tab", "Text"),
    ("GDI", r"Bitmap|Blt|DIB|^SetPixel|^GetPixel|Stretch", "Bitmaps"),
    ("GDI", r"Palette|Color|RGB", "Colours"),
    ("GDI", r"Rgn|Region|Clip", "Regions"),
    ("GDI", r"Meta", "Metafiles"),
    ("GDI", r"Map|Viewport|Window(Org|Ext)|DPtoLP|LPtoDP|Origin|Ext", "Coordinates"),
    ("GDI", r"Pen|Brush|Object|Stock", "Drawing objects"),
    ("GDI", r"DC$|DC[A-Z]|SaveDC|RestoreDC", "Device contexts"),
    ("GDI", r"Line|Rect|Ellipse|Polygon|Poly|Arc|Pie|Chord|Fill|Flood|MoveTo", "Drawing"),
    ("GDI", r"", "Drawing"),
    ("COMMDLG", r"", "Common dialogs"),
    ("SHELL", r"Drag", "Drag and drop"),
    ("SHELL", r"Reg", "Registry"),
    ("SHELL", r"", "Shell"),
    ("KEYBOARD", r"Ansi|Oem", "Strings"),
    ("KEYBOARD", r"", "Keyboard"),
    ("SOUND", r"", "Sound"),
    ("MMSYSTEM", r"", "Multimedia"),
    ("SYSTEM", r"", "System"),
    ("TOOLHELP", r"", "Debugging"),
    ("DDEML", r"", "DDE"),
    ("OLECLI", r"", "OLE"),
    ("OLESVR", r"", "OLE"),
    ("LZEXPAND", r"", "Files"),
    ("VER", r"", "Versions"),
    ("WIN87EM", r"", "Maths"),
    ("*", r"", "Other"),
]
ARG_BYTES = {"word": 2, "s_word": 2, "long": 4, "ptr": 4, "str": 4, "segptr": 4,
             "segstr": 4, "double": 8}
# Exports Wine only STUBS, so its spec gives no arguments: the Pascal bytes
# from OLE 1.0's ole.h prototypes (an LPOLEOBJECT, LPOLECLIENT, LPOLESTREAM,
# LHCLIENTDOC or string 4, an OLEOPT_* or OLECLIPFORMAT 2), for the calls a
# program w16 runs imports.
STUB_ARGS = {
    ("OLECLI", 2): 4, ("OLECLI", 3): 8, ("OLECLI", 4): 24, ("OLECLI", 6): 20, ("OLECLI", 13): 4,
    ("OLECLI", 15): 12, ("OLECLI", 16): 6, ("OLECLI", 18): 8, ("OLECLI", 19): 16, ("OLECLI", 20): 4,
    ("OLECLI", 21): 16, ("OLECLI", 22): 4, ("OLECLI", 24): 8, ("OLECLI", 25): 6, ("OLECLI", 26): 6,
    ("OLECLI", 27): 4, ("OLECLI", 28): 10, ("OLECLI", 29): 8, ("OLECLI", 32): 24, ("OLECLI", 34): 28,
    ("OLECLI", 35): 4, ("OLECLI", 36): 4, ("OLECLI", 37): 4, ("OLECLI", 38): 32, ("OLECLI", 39): 36,
    ("OLECLI", 40): 4, ("OLECLI", 56): 8,
}
LINE = re.compile(r"^\s*(\d+)\s+(pascal|cdecl|varargs|stub|equate|variable)\s+(.*)$")


def parse(path):
    out = {}
    for raw in open(path, encoding="utf-8"):
        m = LINE.match(raw.split("#", 1)[0].rstrip())
        if not m:
            continue
        ordinal, kind, rest = int(m.group(1)), m.group(2), m.group(3).split()
        while rest and rest[0].startswith("-"):
            rest.pop(0)
        if not rest:
            continue
        name, argbytes, register = rest[0], -1, "-register" in raw
        if kind in ("pascal", "cdecl", "varargs"):
            fm = re.match(r"([A-Za-z_]\w*)\s*\((.*?)\)", " ".join(rest))
            if not fm:
                continue
            name = fm.group(1)
            args = fm.group(2).split()
            if kind == "pascal" and all(a in ARG_BYTES for a in args):
                argbytes = sum(ARG_BYTES[a] for a in args)
        elif kind not in ("stub", "equate", "variable"):
            continue
        if register:
            argbytes = -2                # takes and returns registers (InitTask)
        out[ordinal] = (name, argbytes)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--spec-dir", help="the directory holding the .spec files")
    ap.add_argument("--urls", action="store_true", help="print where to fetch them")
    a = ap.parse_args()
    if a.urls or not a.spec_dir:
        for _, dll in MODULES:
            print(f"https://gitlab.winehq.org/wine/wine/-/raw/{WINE_TAG}/dlls/{dll}/{dll}.spec")
        return 0 if a.urls else 1
    lines = [
        "// w16_names.c -- GENERATED by tools/gen_w16_ordinals.py from Wine's",
        f"// {WINE_TAG} 16-bit .spec files: each Win16 export's ordinal, name, and the",
        "// bytes of Pascal arguments it pops (-1 not a plain Pascal call, -2 one that",
        "// takes registers), and what it is for (KIND_RULES). Facts of the Windows",
        "// ABI; do not edit by hand.",
        "",
        '#include "w16_names.h"',
        "",
    ]
    kinds = []
    for _, _, k in KIND_RULES:
        if k not in kinds:
            kinds.append(k)

    def kind_of(mod, name):
        for m, rx, k in KIND_RULES:
            if (m == mod or m == "*") and re.search(rx, name):
                return kinds.index(k)
        raise AssertionError("KIND_RULES ends with a catch-all")

    lines.append("const char *const w16_kind_names[] = {")
    lines += [f'    "{k}",' for k in kinds]
    lines.append("};")
    lines.append(f"const unsigned w16_kind_count = {len(kinds)};")
    lines.append(f"const unsigned w16_kind_other = {kinds.index('Other')};")
    lines.append("")
    total = 0
    for mod, dll in MODULES:
        table = parse(os.path.join(a.spec_dir, dll + ".spec"))
        lines.append(f"static const struct w16_name names_{mod.lower()}[] = {{")
        for o in sorted(table):
            name, argb = table[o]
            if argb == -1 and (mod, o) in STUB_ARGS:
                argb = STUB_ARGS[(mod, o)]
            lines.append(f'    {{ {o}, {argb}, {kind_of(mod, name)}, "{name}" }},')
        lines.append("};")
        lines.append("")
        total += len(table)
    lines.append("const struct w16_name_module w16_name_modules[] = {")
    for mod, _ in MODULES:
        lines.append(f'    {{ "{mod}", names_{mod.lower()}, '
                     f"sizeof names_{mod.lower()} / sizeof names_{mod.lower()}[0] }},")
    lines.append("    { 0, 0, 0 },")
    lines.append("};")
    lines += [
        "",
        "static int same_module(const char *a, const char *b) {",
        "    for (; *a && *b; a++, b++) {",
        "        char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a;",
        "        char y = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;",
        "        if (x != y) return 0;",
        "    }",
        "    return *a == *b;",
        "}",
        "",
        "const struct w16_name *w16_name_lookup(const char *module, unsigned ordinal) {",
        '    if (same_module(module, "KRNL386")) module = "KERNEL";',
        "    for (const struct w16_name_module *m = w16_name_modules; m->module; m++) {",
        "        if (!same_module(m->module, module)) continue;",
        "        unsigned lo = 0, hi = m->count;",
        "        while (lo < hi) {",
        "            unsigned mid = (lo + hi) / 2;",
        "            if (m->names[mid].ordinal == ordinal) return &m->names[mid];",
        "            if (m->names[mid].ordinal < ordinal) lo = mid + 1;",
        "            else hi = mid;",
        "        }",
        "    }",
        "    return 0;",
        "}",
    ]
    open(OUT, "w").write("\n".join(lines) + "\n")
    print(f"gen_w16_ordinals: {total} exports -> {os.path.relpath(OUT, ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
