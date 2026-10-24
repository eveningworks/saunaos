#!/usr/bin/env python3
"""A Windows 3.x SETUP DISK SET with no Microsoft file in it.

w16's installer (userland/lib/uwinsetup.c) reads a real SETUP.INF and
COMPRESS.EXE's KWAJ files; testing it needs disks, and the repo can carry
no Microsoft bytes. This writes a set in the same SHAPE -- a SETUP.INF
with the sections the real Windows for Workgroups 3.11 one uses, each file
KWAJ-compressed by tools/mscompress.py under its `NAME.EX_` name -- holding
files that are only their own names, and it says, by its OWN table rather
than by the installer's rules, where each one must end up.

    python3 tools/gen_win3_disks.py OUT [--form flat|disks|images|mixed] [--list]

`--form`: every file in OUT (flat); OUT/disk1..disk4 (disks); OUT/Disk01.img
..Disk04.img, 1.44 MB FAT12 floppies made with mtools (images); disks 1-2
as folders and 3-4 as images (mixed). `--list` prints `WINDOWS/NAME` or
`WINDOWS/SYSTEM/NAME` per expected file. Imported, `build(out, form)`
returns {relative path: bytes}.
"""
import argparse
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mscompress  # noqa: E402

W, S = "WINDOWS", "WINDOWS/SYSTEM"

# (disk, name as SETUP.INF says it, where it must land, installed name or None for the same).
# Each row is one rule of Setup's, named in the comment beside it.
FILES = [
    (1, "WINSETUP.EXE", W, None),        # [windows]: DOS Setup's list, to WINDOWS
    (1, "WINHELP.EXE", W, None),
    (2, "WIN.SRC", W, "WIN.INI"),        # a template becomes its INI
    (2, "SYSTEM.SRC", W, "SYSTEM.INI"),
    (1, "GDI.EXE", S, None),             # [windows.system]
    (1, "USER.EXE", S, None),            # (what Retro Launcher takes as "Windows is installed")
    (2, "LZEXPAND.DLL", S, None),
    (2, "CPWIN386.CPL", S, None),        # [windows.system.386]
    (1, "KRNL386.EXE", S, None),         # [winexec]'s Krnl386 =
    (3, "CONTROL.SRC", W, "CONTROL.INI"),  # [net], via [win.copy.win386]'s "#net, 0:"
    (3, "WINVER", W, "WINVER.EXE"),      # no extension, an MZ program: Setup adds .EXE
    (4, "PROGMAN.EXE", W, None),         # [win.shell], "#win.shell, 0:"
    (4, "CONTROL.EXE", W, None),
    (3, "WIN386.EXE", S, None),          # [pwin386], "#pwin386, 0:system"
    (4, "MAIN.CPL", S, None),            # [win.other], "#win.other, 0:system"
    (4, "SND.CPL", S, None),
    (3, "CALC.EXE", W, None),            # [win.apps]
    (3, "SOL.EXE", W, None),             # [win.games]
    (4, "CHIMES.WAV", W, None),          # [win.bmps]
    (4, "PBRUSH.DLL", W, None),          # [win.dependents]: "pbrush = 6:PBRUSH.DLL"
    (4, "CARDS.DLL", W, None),           # [win.dependents]: "hearts = #hearts"
    (4, "FAX.CPL", S, None),             # "msfax = #faxsys, 0:system, #msfax": to SYSTEM...
    (4, "FAXVIEW.EXE", W, None),         # ...and the next section back to WINDOWS
    (2, "VGA.DRV", S, None),             # [display]'s vga line: driver, logo, grabber
    (2, "VGALOGO.LGO", S, None),
    (2, "VGA.3GR", S, None),
    (2, "CGA40WOA.FON", S, None),        # the grabber's section, the 437 column
    (2, "VGASYS.FON", S, None),          # [sysfonts] at VGA's "100,96,96"
    (3, "SSERIFE.FON", S, None),         # [fonts] at VGA's resolution
    (3, "ROMAN.FON", S, None),           # [fonts]' CONTINUOUSSCALING
    (3, "ARIAL.FOT", S, None),           # [ttfonts]: both files
    (3, "ARIAL.TTF", S, None),
    (2, "KEYBOARD.DRV", S, None),        # [keyboard.drivers] kbd
    (2, "MOUSE.DRV", S, None),           # [pointing.device] ps2mouse
    (2, "SYSTEM.DRV", S, None),          # [system]
    (2, "MMSOUND.DRV", S, None),
    (2, "MMSOUND.DRV", S, "SOUND.DRV"),  # ...the sound driver, also as SOUND.DRV
    (2, "DOSAPP.FON", S, None),          # [codepages] 437
    (2, "LANGSCA.DLL", S, None),         # [language] fin
    (2, "KBDFI.DLL", S, None),           # [keyboard.tables] findll
    (1, "SETUP.INF", S, None),           # Setup keeps itself
]
# On the disks and named in SETUP.INF, but not for this machine: never installed.
DECOYS = [(2, "8514.DRV"), (2, "8514SYS.FON"), (3, "SSERIFF.FON"), (2, "KBDUS.DLL"), (2, "LANGGER.DLL"),
          (2, "VGA850.FON"), (2, "CGA40850.FON"), (2, "LMOUSE.DRV")]

INF = """;; A test SETUP.INF in the shape of Windows for Workgroups 3.11's -- no Microsoft text
[setup]
    help = setup.hlp

[data]
    startup   = WIN.COM
    defdir    = C:\\WINDOWS
    welcome   = "Microsoft Testi-Windows 3.11 Ty\xf6ryhm\xe4"
    deflang   = fin
    defxlat   = 437
    defkeydll = findll

[winexec]
    Krnl386   = 1:krnl386.exe

[disks]
    1 =. ,"Test Disk 1",disk1
    2 =. ,"Test Disk 2",disk2
    3 =. ,"Test Disk 3",disk3
    4 =. ,"Test Disk 4",disk4

[windows]
    1:winsetup.exe
    1:winhelp.exe        ; a comment after a field
    2:win.src,    Net
    2:system.src, Net

[windows.system]
    1:gdi.exe
    1:user.exe
    2:lzexpand.dll

[windows.system.386]
    2:cpwin386.cpl

[win.copy.win386]
   #net,       0:
   #win.shell, 0:
   #pwin386,   0:system
   #win.other, 0:system

[net]
   3:CONTROL.SRC,    "Windows User Files"
   3:WINVER

[win.shell]
   4:PROGMAN.EXE,    "Program Manager"
   4:CONTROL.EXE,    "Control Panel"

[pwin386]
   3:WIN386.EXE

[win.other]
   4:MAIN.CPL
   4:SND.CPL,,novermsg

[win.apps]
   3:CALC.EXE,     "Calculator"                ,  43072, calc

[win.games]
   3:SOL.EXE,      "Solitaire"        , 180688, sol

[win.bmps]
   4:chimes.wav,   "Chimes Sound"             ,  15920

[win.dependents]
pbrush   = 4:PBRUSH.DLL
hearts   = #hearts
msfax    = #faxsys, 0:system, #msfax

[hearts]
   4:CARDS.DLL,,nodelete

[faxsys]
   4:FAX.CPL

[msfax]
   4:FAXVIEW.EXE

[display]
8514     = 2:8514.drv,     "8514/a",  "100,120,120", , 2:vgalogo.lgo,   2:vdd8514.386,  2:vgadib.3gr,,  2:vgalogo.rle, 8514
vga      = 2:vga.drv,      "VGA",     "100,96,96",   , 2:vgalogo.lgo,   x:*vddvga,      2:vga.3gr

[VGA.3gr]
2:CGA40WOA.FON,2:CGA40850.FON

[sysfonts]
2:vgasys.fon,"VGA (640x480) resolution System Font", "100,96,96"
2:8514sys.fon,"8514/a (1024x768) resolution System Font", "100,120,120"

[fonts]
   3:SSERIFE.FON, "MS Sans Serif (VGA res)", "100,96,96"
   3:SSERIFF.FON, "MS Sans Serif (8514/a res)", "100,120,120"
   3:ROMAN.FON, "Roman (Plotter)",   "CONTINUOUSSCALING"

[ttfonts]
3:ARIAL.FOT,   "Arial (TrueType)",   3:arial.ttf,   ""

[keyboard.drivers]
kbd   = 2:keyboard.drv

[keyboard.tables]
findll = 2:kbdfi.dll , "Finnish"
usadll = 2:kbdus.dll , "US"

[pointing.device]
lmouse   = 2:lmouse.drv,  "Logitech",   2:lvmd.386, lmouse
ps2mouse = 2:mouse.drv,   "Microsoft, or IBM PS/2",   x:*vmd

[system]
system   =    2:system.drv
sound    =    2:mmsound.drv

[codepages]
850 = 2:xlat850.bin, 2:vga850.fon, 2:app850.fon, "Multi-Lingual (850)"
437 =              ,             , 2:dosapp.fon, "English (437)"

[language]
deu = 2:langger.dll, "German"  ,1031
fin = 2:langsca.dll, "Finnish" ,1035
"""


def content(name):
    """A file that is only its own name -- an MZ header for the extension-less program."""
    body = ("w16 installer test file %s\r\n" % name).encode() * 20
    return b"MZ" + body if name == "WINVER" else body


def disk_name(name):
    base, dot, ext = name.partition(".")
    return base + "._" if not dot else base + "." + (ext[:2] + "_" if len(ext) == 3 else ext + "_")


def _write_disk(folder, disk):
    os.makedirs(folder, exist_ok=True)
    for d, name, _, _ in FILES + [(d, n, None, None) for d, n in DECOYS]:
        if d != disk or name == "SETUP.INF":
            continue
        with open(os.path.join(folder, disk_name(name)), "wb") as f:
            f.write(mscompress.kwaj(content(name), 3, name if "." in name else None))
    if disk == 1:
        with open(os.path.join(folder, "SETUP.INF"), "wb") as f:
            f.write(INF.encode("latin-1"))


def _image(folder, img, label):
    with open(img, "wb") as f:
        f.truncate(1474560)
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    subprocess.run(["mformat", "-i", img, "-f", "1440", "-v", label, "::"], check=True, env=env, capture_output=True)
    files = sorted(os.listdir(folder))
    if files:
        subprocess.run(["mcopy", "-i", img, *[os.path.join(folder, f) for f in files], "::/"],
                       check=True, env=env, capture_output=True)


def build(out, form="flat"):
    """Writes the set at `out`; returns {"WINDOWS/..." path: bytes} it must install."""
    os.makedirs(out, exist_ok=True)
    for disk in range(1, 5):
        if form == "flat":
            _write_disk(out, disk)
        elif form == "disks" or (form == "mixed" and disk <= 2):
            _write_disk(os.path.join(out, "disk%d" % disk), disk)
        else:
            tmp = os.path.join(out, ".stage%d" % disk)
            _write_disk(tmp, disk)
            _image(tmp, os.path.join(out, "Disk%02d.img" % disk), "TESTDSK%d" % disk)
            shutil.rmtree(tmp)
    want = {}
    for _, name, where, as_name in FILES:
        data = INF.encode("latin-1") if name == "SETUP.INF" else content(name)
        want["%s/%s" % (where, as_name or name)] = data
    return want


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("out")
    ap.add_argument("--form", choices=("flat", "disks", "images", "mixed"), default="flat")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()
    want = build(args.out, args.form)
    if args.list:
        for p in sorted(want):
            print(p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
