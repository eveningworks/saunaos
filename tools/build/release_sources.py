#!/usr/bin/env python3
"""tools/build/release_sources.py -- the pinned upstream tarballs a release's images need beside them.

    python3 tools/build/release_sources.py OUTDIR STAGING...

Copies into OUTDIR every tarball whose binaries a STAGING tree carries,
decided as the update server decides it (update_server.SOURCE_SETS: the
licence notice each optional build writes into usr/share/licenses).
GPL-3 binaries -- the compiler, GNU make, ScummVM -- are distributed
with their corresponding source, so a tarball that is missing or does
not match its pin is a REFUSAL, never a skip. Prints one name a line.
tools/build/release_assets.sh runs it.
"""
import os
import shutil
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
import fetch_toolchain  # noqa: E402
from update_server import SOURCE_SETS  # noqa: E402


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__.strip().splitlines()[2].strip())
    out, stagings = sys.argv[1], sys.argv[2:]
    os.makedirs(out, exist_ok=True)
    done = []
    for notice, rows, dest_dir, tool in SOURCE_SETS:
        if not any(os.path.exists(os.path.join(s, "usr", "share", "licenses", notice)) for s in stagings):
            continue
        for tb, algo, pin in rows:
            if tb in done:
                continue
            src = os.path.join(dest_dir, tb)
            if not os.path.isfile(src) or fetch_toolchain.digest(src, algo) != pin:
                sys.exit(f"release_sources: REFUSING -- the images carry {notice[:-4]} but {tb} "
                         f"is missing or does not match its pin ({tool} fetches it)")
            shutil.copy2(src, os.path.join(out, tb))
            done.append(tb)
            print(tb)


if __name__ == "__main__":
    main()
