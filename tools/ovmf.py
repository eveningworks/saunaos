#!/usr/bin/env python3
"""tools/ovmf.py -- QEMU's UEFI firmware (OVMF), found and wired up once.

A UEFI boot needs two flash images: the firmware CODE, read-only and
shared, and a VARIABLE STORE the firmware writes (boot entries, the boot
order). The store is a per-guest COPY of the distribution's template --
handing two guests one store, or QEMU the template itself, lets one
firmware's writes land in another's boot.

Distributions put the files in different places under different names;
FIRMWARE lists the pairs this repo knows, the shape of the Makefile's
grub-mkrescue/grub2-mkrescue fallback.

Used by `make run UEFI=1` and `vm.py --uefi`:

    python3 tools/ovmf.py --qemu-args build/ovmf_vars.fd
"""

import argparse
import os
import shutil
import sys

# (code, variable-store template), first match wins.
FIRMWARE = (
    ("/usr/share/edk2/x64/OVMF_CODE.4m.fd", "/usr/share/edk2/x64/OVMF_VARS.4m.fd"),      # Arch
    ("/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_VARS_4M.fd"),              # Debian, Ubuntu
    ("/usr/share/edk2/ovmf/OVMF_CODE.fd", "/usr/share/edk2/ovmf/OVMF_VARS.fd"),          # Fedora
    ("/usr/share/OVMF/OVMF_CODE.fd", "/usr/share/OVMF/OVMF_VARS.fd"),                    # older Debian
)


def firmware():
    """(code, vars_template), or None on a host without OVMF."""
    for code, tmpl in FIRMWARE:
        if os.path.isfile(code) and os.path.isfile(tmpl):
            return code, tmpl
    return None


def qemu_args(vars_path):
    """The two -drive pflash arguments, creating `vars_path` from the
    template if it does not exist yet. None without OVMF."""
    fw = firmware()
    if not fw:
        return None
    code, tmpl = fw
    if not os.path.exists(vars_path):
        os.makedirs(os.path.dirname(os.path.abspath(vars_path)), exist_ok=True)
        shutil.copyfile(tmpl, vars_path)
    return ["-drive", f"if=pflash,format=raw,readonly=on,file={code}",
            "-drive", f"if=pflash,format=raw,file={vars_path}"]


NOT_FOUND = ("no OVMF (QEMU's UEFI firmware) on this host -- install edk2-ovmf "
             "(Arch, Fedora) or ovmf (Debian, Ubuntu); looked for "
             + ", ".join(c for c, _t in FIRMWARE))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--qemu-args", metavar="VARS",
                    help="print QEMU's pflash arguments, the variable store at VARS")
    args = ap.parse_args()
    if not args.qemu_args:
        fw = firmware()
        print(f"code {fw[0]}\nvars {fw[1]}" if fw else NOT_FOUND)
        return 0 if fw else 1
    a = qemu_args(args.qemu_args)
    if not a:
        print(f"ovmf: {NOT_FOUND}", file=sys.stderr)
        return 1
    print(" ".join(a))
    return 0


if __name__ == "__main__":
    sys.exit(main())
