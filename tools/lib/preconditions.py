"""Can a test run on this machine at all? -- the `needs` keys of a RUNNER header.

A test that needs an IWAD, Docker or ffmpeg is a SKIP with a reason when
they are missing, never a failure: a check that cannot pass on a clean
checkout is one people learn to skim past. `needs` in a tool's RUNNER
header (lib/runner_meta.py) is `[key, why]`; the keys are the branches
below. Used by ondemand_sweep.py and hostchecks.py.
"""
import os
import shutil
import subprocess

import toolpath

REPO = toolpath.REPO


def precondition_met(kind):
    """Can this tool run here at all? Returns (ok, why_not)."""
    if kind is None:
        return True, ""
    key, why = kind
    if key == "samba":
        # The image, not just Docker: building it is a download, which a
        # sweep must not do unasked. A skip either way.
        if not shutil.which("docker"):
            return False, why
        r = subprocess.run(["docker", "image", "inspect", "saunaos-samba:1"],
                           capture_output=True, text=True)
        return r.returncode == 0, why
    if key == "iwad":
        for root, _dirs, files in os.walk(os.path.join(REPO, "seed")):
            if any(f.lower().endswith(".wad") for f in files):
                return True, ""
        return False, why
    if key == "alsa_loop":
        # The CABLE itself is not checked here -- the tool probes it and
        # skips with its own message, because proving the loop means
        # playing a tone through it and that is the tool's job, not a
        # precondition's. What this gates is the cheap half: the
        # userspace and the two cards existing at all.
        if not all(shutil.which(t) for t in ("aplay", "arecord", "amixer")):
            return False, why
        try:
            cards = open("/proc/asound/cards").read()
        except OSError:
            return False, why
        return ("[G6" in cards and "[Generic" in cards), why
    if key == "host_audio":
        # A HOST-side check rather than a guest one: it compiles the
        # decoder with the host gcc and compares against ffmpeg, so what
        # it needs is tools, not a VM. Missing them is a skip -- the gate
        # must not start requiring lame and ffmpeg on every checkout,
        # the same rule that keeps Docker out of preflight.
        return all(shutil.which(t) for t in ("gcc", "lame", "ffmpeg")), why
    if key == "host_audio_flac":
        # host_audio plus Xiph's `flac`, the FLAC half's oracle. A skip.
        return all(shutil.which(t) for t in ("gcc", "lame", "ffmpeg", "flac")), why
    if key == "netsurf_oracle":
        # The host nsmonkey IS the oracle, and building it needs gperf
        # and perl -- a skip without it, as lame and ffmpeg are above.
        from netsurf_vendor import DEFAULT_WORK
        return os.access(os.path.join(DEFAULT_WORK, "src", "netsurf", "nsmonkey"), os.X_OK), why
    if key == "host_video":
        # ffmpeg is both the encoder of the sweep and its oracle. A skip.
        return all(shutil.which(t) for t in ("gcc", "ffmpeg")), why
    if key == "host_fluidsynth":
        # FluidSynth is the ORACLE: without it the check would run only
        # its fuzz half and still print a pass. A skip, like lame and
        # ffmpeg above.
        return all(shutil.which(t) for t in ("gcc", "fluidsynth")), why
    if key == "host_pillow":
        # Host gcc plus Pillow, which is the ORACLE rather than a
        # convenience: both image harnesses judge our codecs against it.
        # Missing either is a skip for the same reason lame and ffmpeg
        # are -- a check that cannot pass on a clean checkout is one
        # people learn to skim past.
        if not shutil.which("gcc"):
            return False, why
        try:
            import PIL  # noqa: F401
        except ImportError:
            return False, why
        return True, why
    if key == "host_pillow_magick":
        # host_pillow plus ImageMagick, which writes the BMP and GIF
        # variants Pillow cannot (RLE8, OS/2 headers, 16-bit masks,
        # animations at offsets). A skip when missing, the same rule.
        ok, _ = precondition_met(("host_pillow", why))
        return ok and shutil.which("magick") is not None, why
    if key == "host_cc":
        # Host gcc only. A skip rather than a failure on a checkout
        # without it, same rule as host_audio above.
        return shutil.which("gcc") is not None, why
    if key == "openssl":
        # The test certificates are made with `openssl req -x509`.
        # Missing it is a skip for the same reason lame and ffmpeg are:
        # a check that cannot pass on a clean checkout is one people
        # learn to ignore.
        return shutil.which("openssl") is not None, why
    if key == "live_iso":
        # A separate ISO that `make iso` does not build. Missing it is a
        # precondition, not a failure -- reporting it as red would train
        # the reader to skim past a red line.
        return os.path.exists(os.path.join(REPO, "saunaos-live.iso")), why
    if key == "kcmdline":
        # The BUILT config (iso/boot/grub/grub.cfg), and only its
        # `multiboot2` line. The template at the repo root is the wrong
        # file twice over: it still holds the @KCMDLINE@ placeholder,
        # and it documents `video=` in four comment lines -- so a
        # substring search over it returns TRUE on every checkout and
        # runs hires_test against a default-mode ISO, where CLAUDE.md
        # says every check in it passes VACUOUSLY. A precondition that
        # is wrong in the permissive direction manufactures coverage
        # that does not exist, which is worse than reporting a failure.
        # Reading the ISO's copy is still right even though an ordinary
        # boot reads disk.img's: both are generated from the repo-root
        # template with the same KCMDLINE in one `make iso`, so they
        # cannot disagree about what was baked in.
        cfg = os.path.join(REPO, "iso", "boot", "grub", "grub.cfg")
        if not os.path.exists(cfg):
            return False, "no built ISO tree -- run `make iso` first"
        for line in open(cfg).read().splitlines():
            if line.strip().startswith("multiboot2") and "video=" in line:
                return True, ""
        return False, why
    return True, ""
