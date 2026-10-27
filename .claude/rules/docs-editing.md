---
paths:
  - "docs/**"
---

# docs/: what each file is, and the rules for editing it

Moved verbatim from the root CLAUDE.md (2026-10-23); it loads only when a matching file is read or edited.

| File | What it is |
|---|---|
| `docs/testing.md` | running and driving the OS, QMP mechanics, what the emulator does and does not model |
| `docs/tools.md` | every script in `tools/` |
| `docs/conventions/` | convention BODIES by area -- `INDEX.md` first |
| `docs/toolkit.md` | every ring-3 toolkit header in one line -- GENERATED, check it before writing a helper or widget |
| `docs/decisions.md` | the INDEX over `docs/decisions/`: why SaunaOS works this way |
| `docs/gui-guidelines.md` | how the GUI looks and behaves -- binding; read before touching anything drawn |
| `docs/gui-app-tutorial.md` | writing a ring-3 GUI app, walked through `userland/gui/demos/counter.c` -- change the two together |
| `docs/driver-guide.md` | writing a driver; `e1000.c` and `r8169.c` are the worked examples |
| `docs/devices.md` | every driver, by registry; `check_docs.py` wants a row per `DRIVER_DECLARE` |
| `docs/filesystem-layout.md` | what lives where on disk; `check_layout.py` enforces it in both directions |
| `docs/boot-flags.md` | every kernel command-line word -- matched by SUBSTRING with no registry, so this table is the only list |
| `docs/settings-and-queries.md` | facts vs settings vs tunables |
| `docs/commands.md` | the INDEX over `docs/commands/` |
| `docs/bugs.md` / `docs/roadmap.md` / `docs/roadmap-details.md` | what is broken, what is not built yet, and the long form of both |

**Rules for editing docs/:**

- **A new convention goes in `docs/conventions/<area>.md` AND its
  headline in `INDEX.md`**, or it is invisible to the next session.
- **A decision entry** goes in its area's file, then
  `tools/gen_decisions_index.py` (the index is GENERATED and
  `check_docs.py` fails when stale; a NEW file joins the script's
  `ORDER`). Entries are SELF-CONTAINED; the bar is "why this way and not
  the obvious way" that a future session would re-litigate.
- **Milestones are NAMED, not numbered**; old numbers resolve through the
  legend at the end of `docs/roadmap-details.md`.
- **URGENCY IS MARKED WHERE THE ITEM LIVES**: `**NEXT**` on the item,
  then `tools/gen_next_up.py --write`; never hand-write the "Next up"
  list. `**NEXT**` means "before the unmarked work around it", never
  "this is broken".
- **`docs/roadmap.md` IS ORDERED BY WHAT MUST BE BUILT FIRST** -- phases
  1-4 a dependency chain, then tracks that depend on nothing. **EVERY
  ITEM IS ONE LINE**; detail goes to `docs/roadmap-details.md` under the
  SAME title.
