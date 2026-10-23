---
paths:
  - "kernel/drivers/usb/**"
  - "kernel/drivers/sound/**"
  - "userland/bin/soundd.c"
  - "tools/*audio*.py"
  - "tools/sndformat_test.py"
  - "tools/usb_*.py"
---

# Testing USB audio

Moved verbatim from the root CLAUDE.md (2026-10-23); it loads only when a matching file is read or edited.

- **A USB AUDIO RESULT SAYS WHICH PATH IT EXERCISED, OR IT SAYS
  NOTHING.** A real G6 is UAC2 at high speed (8000 packets/s, rate
  through a Clock Source) and UAC1 at full speed (1000/s), and only a
  physical replug reaches high speed: read `UAC1`/`UAC2` and the cadence
  from the bind line first. **Two playbacks back to back are ONE
  stream** (soundd keeps the card 2 s idle), so a restart fault needs a
  longer gap. The driver's `usbaudio: start`/`stop` lines are the
  evidence; `docs/bugs.md` has what is open.
