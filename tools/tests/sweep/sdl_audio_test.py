"""tools/tests/sweep/sdl_audio_test.py -- SDL's audio, through SaunaOS's own
driver, judged on the HOST (docs/sdl-design.md, stage 2).

Two boots, each with an AC97 and its own recording (QEMU's wav audiodev:
what the CARD emitted, sharing no code with the guest -- audio_test.py's
oracle and its measure()):

  1. /tests/sdltone_test -- SDL3, 1.5 s of 1000 Hz as mono float at
     44.1 kHz into an SDL_AudioStream. SDL converts to the driver's
     stereo S16 at 48 kHz; the driver pushes into usnd (usnd_push).
  2. /tests/sdl2tone_test -- SDL2's CALLBACK API through sdl2-compat
     (ScummVM's shape), 1.5 s of 750 Hz as mono S16 at 22.05 kHz.

Each must name the "saunaos" driver, play to its end, and come out at
ITS pitch for about 1.5 s. A different pitch per boot means one boot's
recording cannot pass for the other's; a driver that dropped or
repeated buffers is caught by the tone's length, one that did not
convert by its pitch (1000 Hz played as 48 kHz is 1088 Hz), a dead
one by silence, and one that let usnd's queue run dry by a whole mixer
chunk of silence inside the tone.

    python3 tools/tests/sweep/sdl_audio_test.py [--instance N] [--keep]

On demand, not in the gate: it boots its own guest with extra hardware.
"""

RUNNER = {"covers": "SDL3 and SDL2 audio through usnd, by the card's own recording"}

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lib"))
import toolpath  # noqa: E402,F401 -- every tools/ folder on sys.path
from harness import Results, copy_disk  # noqa: E402
from audio_test import wait_serial, establish, measure, segments, CHUNK_MS  # noqa: E402

REPO = os.path.dirname(toolpath.TOOLS)

PHASES = (
    # (label, program, log prefix, pitch)
    ("SDL3", "/tests/sdltone_test", "sdltone:", 1000.0),
    ("SDL2", "/tests/sdl2tone_test", "sdltone2:", 750.0),
)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--instance", type=int, default=0)
    ap.add_argument("--keep", action="store_true", help="keep the recordings and print where")
    ap.add_argument("--kvm", action="store_true",
                    help="boot under KVM: TCG can fall behind the card, and QEMU pads the recording")
    args = ap.parse_args()
    n = args.instance
    sock = ".vm.serial" if n == 0 else f".vm.{n}.serial"
    res = Results()
    tmp = tempfile.mkdtemp(prefix="sdl_audio_test_")
    img = os.path.join(tmp, "disk.img")
    copy_disk("disk.img", img, cwd=REPO)

    def vm(*a):
        return subprocess.run([sys.executable, toolpath.tool("vm.py"), "--instance", str(n), *a],
                              cwd=REPO, capture_output=True, text=True)

    print("sdl_audio_test: checks")
    try:
        for label, prog, prefix, hz in PHASES:
            wav = os.path.join(tmp, f"{label.lower()}.wav")
            vm("stop")
            if not res.check(f"{label}: the guest boots with an AC97",
                             vm(*(["--kvm"] if args.kvm else []), "--disk", img, "--audio", "ac97",
                                "--audio-wav", wav, "start").returncode == 0):
                continue
            try:
                dbg = wait_serial(sock)
                if not res.check(f"{label}: the serial console answers", dbg is not None):
                    continue
                establish(dbg)
                dbg.send(f"sh spawn {prog}")
                lines = []
                deadline = time.time() + 90
                while time.time() < deadline:
                    lines = [ln for ln in (dbg.send("sh dmesg") or "").splitlines() if prefix in ln]
                    if any("played" in ln or "failed" in ln for ln in lines):
                        break
                    time.sleep(1.0)   # sleep-ok: polls the guest log for a line, bounded
                res.check(f"{label}: SDL opens SaunaOS's driver", any("driver saunaos" in ln for ln in lines),
                          f"{lines}")
                res.check(f"{label}: ...and plays to the end", any(ln.endswith("played") for ln in lines),
                          f"{lines}")
                dbg.close()
            finally:
                vm("stop")
            rate, secs, got, peak, tone_secs = measure(wav)
            res.check(f"{label}: the card played {hz:.0f} Hz (within 3%)", abs(got - hz) <= hz * 0.03,
                      f"{got:.1f} Hz, peak {peak}, rate {rate}")
            res.check(f"{label}: ...for about 1.5 s -- no buffer dropped or played twice",
                      1.3 <= tone_secs <= 1.75, f"{tone_secs:.2f} s of tone in {secs:.1f} s")
            # An UNDERRUN is a whole mixer chunk of silence inside the tone:
            # usnd's mixer found the pushed queue empty (audio_test's check).
            holes = [g for (_, _, g) in segments(wav, min_gap_ms=1.0)
                     if g is not None and g < 1000 and abs(g / CHUNK_MS - round(g / CHUNK_MS)) < 0.05]
            res.check(f"{label}: ...with no whole-chunk hole (the driver kept usnd fed)", not holes,
                      f"holes of {', '.join(f'{h:.1f}' for h in holes)} ms")
    finally:
        vm("stop")
        if args.keep:
            print(f"recordings in {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    print(f"\nsdl_audio_test: {len(res.passes)} passed, {len(res.fails)} failed")
    for f in res.fails:
        print("  FAILED:", f)
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
