#!/usr/bin/env python3
"""Host-side: does tools/todisk.py lay a folder over a TFS3 volume, and only that?

No VM and nothing built: it makes a small partitioned image with tools/seed_disk.py,
runs todisk.py on a folder made here, and reads the volume back with tfs3_writer's own
reader.

WHAT A BROKEN VERSION WOULD STILL PASS, and what is asserted instead:
  - files and nested folders land at the paths they have under the folder, with their
    BYTES (sizes 0, under a block, over a block, over the direct-block ceiling);
  - the top-level README.txt is not copied, and a README.txt deeper down IS;
  - a second run writes nothing, a changed file is replaced and an untouched one is not,
    and a file the folder no longer holds STAYS on the image (nothing is deleted);
  - `--dry-run` changes not one byte of the image;
  - a file the seeding put there before is still there and unchanged afterwards;
  - an image with no TFS3 volume is refused, and so is one a running qemu-system has open
    (a stand-in process whose argv[0] is qemu-system and whose command line names the image).

  python3 tools/todisk_test.py
  python3 tools/todisk_test.py --positive-control    # the qemu guard removed: must fail
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mkpart_test                                   # noqa: E402
import tfs3_writer as t3                             # noqa: E402

IMG_MB = 160
SIZES = {"zero.bin": 0, "small.txt": 700, "block.bin": 4096, "two.bin": 4097 * 3, "big.bin": 60 * 1024}


def todisk(tool, *args, env=None):
    r = subprocess.run([sys.executable, tool, *args], capture_output=True, text=True, env=env)
    return r.returncode, r.stdout + r.stderr


def read_back(disk, path):
    base, sectors = mkpart_test.volume_of(disk)
    img = t3.Tfs3Image(disk, writable=False, base_lba=base, sectors=sectors)
    try:
        ino = img.lookup(path)
        return None if ino is None else img.read_file_data(ino)
    finally:
        img.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("--positive-control", action="store_true")
    args = ap.parse_args()
    bad = []

    def check(name, ok, detail=""):
        print(f"  {'ok  ' if ok else 'FAIL'} {name}" + ("" if ok else f"   [{detail}]"))
        if not ok:
            bad.append(name)

    with tempfile.TemporaryDirectory() as tmp:
        tool = os.path.join(HERE, "todisk.py")
        env = None
        if args.positive_control:
            body = open(tool).read()
            before = "if pid and not args.dry_run:"
            if before not in body:
                sys.exit("todisk_test: the positive control cannot find the qemu guard -- fix the control")
            tool = os.path.join(tmp, "todisk_noguard.py")
            open(tool, "w").write(body.replace(before, "if False:", 1))
            env = dict(os.environ, PYTHONPATH=HERE)

        seed = os.path.join(tmp, "seed")
        os.makedirs(os.path.join(seed, "sync", "etc"))
        open(os.path.join(seed, "sync", "etc", "kept.txt"), "w").write("seeded before\n")
        disk = os.path.join(tmp, "disk.img")
        with open(disk, "wb") as f:
            f.truncate(IMG_MB * 1024 * 1024)
        r = subprocess.run([sys.executable, os.path.join(HERE, "seed_disk.py"), disk, seed],
                           capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit("todisk_test: could not seed the scratch image:\n" + r.stdout + r.stderr)

        folder = os.path.join(tmp, "todisk")
        win = os.path.join(folder, "home", "Win16", "WINDOWS")
        os.makedirs(os.path.join(win, "SUB", "DEEP"))
        data = {}
        for i, (name, n) in enumerate(SIZES.items()):
            data[name] = bytes((j * 7 + i) & 255 for j in range(n))
            with open(os.path.join(win, "SUB", "DEEP", name), "wb") as f:
                f.write(data[name])
        with open(os.path.join(folder, "README.txt"), "w") as f:
            f.write("the folder's own note")
        with open(os.path.join(win, "README.txt"), "w") as f:
            f.write("a real file\n")
        guest = "/home/Win16/WINDOWS"

        # dry run first: the image must be byte-identical afterwards
        before = hashlib.sha256(open(disk, "rb").read()).hexdigest()
        rc, out = todisk(tool, "--disk", disk, "--dir", folder, "--dry-run", env=env)
        check("--dry-run reports the files", rc == 0 and f"would write {guest}/SUB/DEEP/big.bin" in out, out[-200:])
        check("--dry-run changes nothing", hashlib.sha256(open(disk, "rb").read()).hexdigest() == before)

        rc, out = todisk(tool, "--disk", disk, "--dir", folder, env=env)
        check("the first run succeeds", rc == 0 and "6 written" in out, out[-300:])
        for name, want in data.items():
            got = read_back(disk, f"{guest}/SUB/DEEP/{name}")
            check(f"{name} ({len(want)} bytes) arrives with its bytes", got == want,
                  "missing" if got is None else f"{len(got)} bytes, different")
        check("a README.txt below the top is copied", read_back(disk, f"{guest}/README.txt") == b"a real file\n")
        check("the folder's own top-level README.txt is not", read_back(disk, "/README.txt") is None)
        check("what seeding put there is untouched", read_back(disk, "/etc/kept.txt") == b"seeded before\n")

        rc, out = todisk(tool, "--disk", disk, "--dir", folder, env=env)
        check("a second run writes nothing", rc == 0 and "0 written, 6 unchanged" in out, out[-200:])

        data["small.txt"] = b"changed " * 20
        with open(os.path.join(win, "SUB", "DEEP", "small.txt"), "wb") as f:
            f.write(data["small.txt"])
        rc, out = todisk(tool, "--disk", disk, "--dir", folder, env=env)
        check("a changed file is replaced, the others left", rc == 0 and "1 written, 5 unchanged" in out, out[-200:])
        check("...with the new bytes", read_back(disk, f"{guest}/SUB/DEEP/small.txt") == data["small.txt"])

        os.unlink(os.path.join(win, "SUB", "DEEP", "block.bin"))
        rc, out = todisk(tool, "--disk", disk, "--dir", folder, env=env)
        check("a file removed from the folder stays on the image",
              read_back(disk, f"{guest}/SUB/DEEP/block.bin") == data["block.bin"], out[-200:])

        # an image with no volume
        blank = os.path.join(tmp, "blank.img")
        with open(blank, "wb") as f:
            f.truncate(8 * 1024 * 1024)
        rc, out = todisk(tool, "--disk", blank, "--dir", folder, env=env)
        check("an image with no TFS3 volume is refused", rc != 0 and "no TFS3 volume" in out, out[-200:])

        # a qemu holding the image
        stand_in = subprocess.Popen(["bash", "-c", 'exec -a qemu-system-x86_64 python3 -c "import time; time.sleep(30)" "$0"', disk])
        try:
            time.sleep(0.4)
            before = hashlib.sha256(open(disk, "rb").read()).hexdigest()
            data["two.bin"] = data["two.bin"][::-1]
            with open(os.path.join(win, "SUB", "DEEP", "two.bin"), "wb") as f:
                f.write(data["two.bin"])
            rc, out = todisk(tool, "--disk", disk, "--dir", folder, env=env)
            check("an image a running qemu has open is refused", rc != 0 and "open" in out, out[-200:])
            check("...and left byte for byte as it was", hashlib.sha256(open(disk, "rb").read()).hexdigest() == before)
        finally:
            stand_in.kill()
            stand_in.wait()
        shutil.rmtree(folder)

    if bad:
        print(f"todisk_test: {len(bad)} failed")
        return 1
    print("todisk_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
