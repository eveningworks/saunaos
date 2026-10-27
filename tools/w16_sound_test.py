#!/usr/bin/env python3
"""tools/w16_sound_test.py -- a Win16 program's sounds, judged on the HOST.

w16's sound test program (userland/w16/samples/sound.asm) plays three
sounds that can be told apart by PITCH, in order: a 1102.5 Hz square WAV
from its own memory through sndPlaySound(SND_MEMORY), the Asterisk event
through MessageBeep, and two 440 Hz SOUND.DRV notes. The guest boots with
an AC97 recorded by QEMU's wav audiodev -- what the DEVICE played,
measured here, sharing no code with the guest (tools/audio_test.py's
oracle) -- and the program's exit code says what each call answered.

Three runs, each on its own copy of the image:

  1. the defaults: exit 7 (a device, the WAV played, a voice); ~1100 Hz,
     then a sound that is NOT 2000 Hz (SaunaOS's chime: drive C: has no
     WIN.INI), then ~0.9 s of 440 Hz;
  2. drive C:'s WIN.INI names SystemAsterisk=test.wav, a 2000 Hz square
     this tool writes to C:\\WINDOWS: the middle sound is 2000 Hz -- the
     user's own [sounds] reach MessageBeep;
  3. `sounds=off`: exit 0 (no device, nothing played, no voice) and the
     recording silent.

What a broken build still passes: none of the pitch checks -- a WAV
decoded at the wrong rate moves 1100 Hz, the SaunaOS fallback in place of
the INI's file shows as no 2000 Hz, a note table off by a semitone moves
440. --positive-control breaks the WAV's rate (the sound plays at half
the pitch) and must turn the first run red.

    python3 tools/w16_sound_test.py [--instance N] [--positive-control]

ON DEMAND (ondemand_sweep.py): it needs a guest with a sound card, which
the GUI suite's guests do not have.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from harness import copy_disk                  # noqa: E402
from port_guard import port_is_free            # noqa: E402

WINDOW = 0.05          # seconds a window spans
LOUD = 500             # a window carries sound above this peak


def vm(n, *args, **kw):
    return subprocess.run([sys.executable, os.path.join(HERE, "vm.py"), "--instance", str(n), *args],
                          cwd=REPO, capture_output=True, text=True, **kw)


def wait_port_free(port, timeout=30):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if port_is_free(port):
            return True
        time.sleep(0.5)
    return False


def square_wav(path, freq, seconds=0.4, rate=11025):
    n = int(seconds * rate)
    half = rate / freq / 2
    data = bytes(0xC0 if int(i / half) % 2 == 0 else 0x40 for i in range(n))
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + n) + b"WAVEfmt " +
                struct.pack("<IHHIIHH", 16, 1, 1, rate, rate, 1, 8) + b"data" + struct.pack("<I", n) + data)


def windows(path):
    """[(start seconds, Hz or None)] for each window; the header read by
    hand (a stopped VM leaves its RIFF sizes zero)."""
    with open(path, "rb") as f:
        d = f.read()
    if len(d) < 44 or d[:4] != b"RIFF":
        return []
    ch = struct.unpack_from("<H", d, 22)[0] or 1
    rate = struct.unpack_from("<I", d, 24)[0] or 1
    n = (len(d) - 44) // 2
    s = struct.unpack(f"<{n}h", d[44:44 + n * 2])[::ch]
    w = int(rate * WINDOW)
    out = []
    for i in range(0, len(s) - w, w):
        seg = s[i:i + w]
        if max(abs(x) for x in seg) <= LOUD:
            out.append((i / rate, None))
            continue
        zc = sum(1 for j in range(1, len(seg)) if (seg[j - 1] < 0) != (seg[j] < 0))
        out.append((i / rate, zc / 2 / WINDOW))
    return out


def band(win, f, tol, steady=3):
    """The start times of the windows within `tol` Hz of `f`, in stretches
    of `steady` or more in a row: a sound's edge window can read any pitch."""
    out, run = [], []
    for t, hz in win + [(None, None)]:
        if hz is not None and abs(hz - f) <= tol:
            run.append(t)
            continue
        if len(run) >= steady:
            out += run
        run = []
    return out


def run_once(n, img, wav, setup):
    vm(n, "stop")
    wait_port_free(4445 + n)
    if os.path.exists(wav):
        os.remove(wav)
    boot = vm(n, "--disk", img, "--audio-wav", wav, "start", timeout=600)
    if boot.returncode != 0:
        return None, boot.stdout + boot.stderr
    try:
        for cmd in setup:
            if cmd[0] == "put":
                vm(n, "put", cmd[1], cmd[2], timeout=120)
            else:
                vm(n, "exec", *cmd, timeout=120)
        time.sleep(2)
        out = vm(n, "exec", "w16 /usr/share/w16/sound.exe", timeout=300).stdout
        time.sleep(1)
    finally:
        vm(n, "stop")
        wait_port_free(4445 + n)
    code = next((int(x.split()[-1]) for x in out.splitlines() if x.startswith("w16: exit ")), 0)
    return code, out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--instance", type=int, default=0)
    ap.add_argument("--positive-control", action="store_true",
                    help="make the sample's WAV say half its rate: the first run must go red")
    args = ap.parse_args()
    passes, fails = [], []

    def check(name, ok, detail=""):
        (passes if ok else fails).append(name)
        print(("  ok    " if ok else "  FAIL  ") + name + (f"  -- {detail}" if detail else ""))

    tmp = tempfile.mkdtemp(prefix="w16_sound_")
    exe = "/usr/share/w16/sound.exe"
    setup0 = []
    if args.positive_control:
        # The sample rebuilt with its WAV's rate halved: 1102 Hz becomes 551.
        src = open(os.path.join(REPO, "userland/w16/samples/sound.asm")).read()
        src = src.replace("dd 11025, 11025", "dd 5512, 5512")
        bad = os.path.join(tmp, "sound.asm")
        open(bad, "w").write(src)
        subprocess.run(["cp", os.path.join(REPO, "userland/w16/samples/ne16.inc"), tmp], check=True)
        subprocess.run([sys.executable, os.path.join(HERE, "gen_ne.py"), bad, "-o", os.path.join(tmp, "sound.exe")],
                       check=True, cwd=tmp)
        setup0 = [("put", os.path.join(tmp, "sound.exe"), exe)]
    test_wav = os.path.join(tmp, "test.wav")
    square_wav(test_wav, 2000)
    win_ini = os.path.join(tmp, "WIN.INI")
    with open(win_ini, "w") as f:
        f.write("[sounds]\r\nSystemAsterisk=test.wav, Asterisk\r\n")

    conf_off = os.path.join(tmp, "win16.conf")
    with open(conf_off, "w") as f:
        f.write("sounds=off\n")

    cases = [
        ("defaults", setup0),
        ("WIN.INI", setup0 + [("mkdir /home/Win16",), ("mkdir /home/Win16/WINDOWS",),
                              ("put", win_ini, "/home/Win16/WINDOWS/WIN.INI"),
                              ("put", test_wav, "/home/Win16/WINDOWS/TEST.WAV")]),
        ("off", setup0 + [("put", conf_off, "/etc/win16.conf")]),
    ]
    for name, setup in cases:
        img = os.path.join(tmp, f"{name}.img")
        copy_disk("disk.img", img, cwd=REPO)
        wav = os.path.join(tmp, f"{name}.wav")
        print(f"w16_sound_test: {name}")
        code, out = run_once(args.instance, img, wav, setup)
        if code is None:
            check(f"[{name}] the guest booted with a sound card", False, out[-200:])
            continue
        win = windows(wav)
        loud = [t for t, hz in win if hz is not None]
        wavs, notes, high = band(win, 1102, 60), band(win, 440, 25), band(win, 2000, 60)
        detail = (f"{len(loud)} loud windows: {len(wavs)} ~1102 Hz, {len(high)} ~2000 Hz, {len(notes)} ~440 Hz; "
                  f"first {loud[0]:.2f}s last {loud[-1]:.2f}s" if loud else "silent")
        if name == "off":
            check("[off] the program is told there is no device and no voice (exit 0)", code == 0, f"exit {code}")
            check("[off] nothing played", not loud, detail)
            continue
        check(f"[{name}] each call answered: a device, the WAV played, a voice (exit 7)", code == 7, f"exit {code}")
        check(f"[{name}] sndPlaySound(SND_MEMORY) played the program's WAV at its rate (~1102 Hz, 0.3 s)",
              len(wavs) >= 4, detail)
        # Between the WAV and the notes: MessageBeep's event.
        beep = [t for t, hz in win if hz is not None and wavs and notes and wavs[-1] < t < notes[0]
                and t not in wavs and t not in notes]
        if name == "defaults":
            check("[defaults] MessageBeep played an event sound between them, not the INI's 2000 Hz (SaunaOS's chime)",
                  len(beep) >= 3 and not high, detail)
        else:
            check("[WIN.INI] MessageBeep played drive C:'s TEST.WAV, named by [sounds] SystemAsterisk (~2000 Hz)",
                  len(high) >= 4 and all(t in beep for t in high), detail)
        check(f"[{name}] SOUND.DRV's two quarter notes of value 46 are A440, ~0.9 s, after the rest",
              len(notes) >= 14 and bool(wavs) and notes[0] > wavs[-1], detail)
    if fails:
        print(f"w16_sound_test: the images and recordings are kept in {tmp}")
    else:
        shutil.rmtree(tmp, ignore_errors=True)
    print()
    print(f"w16_sound_test: {len(passes)} passed, {len(fails)} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
