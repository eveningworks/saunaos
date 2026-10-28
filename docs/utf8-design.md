# UTF-8

A staged plan to carry UTF-8 in every string SaunaOS keeps, decoded only
where text is drawn or edited -- the move past Latin-1's 191 characters
that `docs/decisions/drivers.md` ("Nordic keyboard/character support:
Latin-1, not UTF-8") always named as the exit.

Written 2026-10-28. **Nothing is built yet.** Step markers go on the list
below as they land.

## The finding that shapes the plan

**The font is the small part. Code that treats a byte as a character is
the large one.** Decoding UTF-8 inside `ugfx_draw_string()` and
`ugfx_text_width()` fixes the functions everyone calls first, and leaves
three kinds of code that would still measure, step and cut a string one
BYTE at a time -- each drawing `é` (C3 A9) as the two Latin-1 glyphs
`Ã©`:

- **hand-rolled measuring loops** -- dozens of files call
  `ugfx_char_advance(s[i])` themselves (proportional wrap in
  `ui/utextflow.c`, `uui_markdown`, `uui_menubar`, `uui_table`,
  `uui_fileview`, `wm_render.c`, Paint's text tool, w16's GDI) rather
  than going through `ui/ugfx_text.c`. `ugfx_text_next()` /
  `ugfx_text_prev()` exist for stepping by character and nothing calls
  them;
- **three edit cores with byte cursors** -- `ui/uui_edit.c` (every text
  field, through `uui_textbox` and `utext`), `ui/utext.c` (Notepad,
  `uui_textview`, `/bin/edit`: a monospace grid where one byte is one
  cell) and `kernel/lib/klineedit.c` (the shell, both rings): Left is
  `cursor - 1`, Backspace erases one byte, a masked field draws one `*`
  per byte;
- **the terminal's cell**, `struct uvterm_cell { char ch; ... }`, one
  byte per column.

**And the ORDER bites.** Text arrives from four places -- the keyboard,
the disk, the network and clipboard, and programs. The moment drawing
decodes UTF-8, every Latin-1 byte still on disk or still coming from a
layout is an invalid sequence. A plan that switches drawing first shows
`�` for every `å` already on the machine until the conversion runs; a
plan that converts first stores UTF-8 that the old drawing shows as
`Ã¥`. Hence the transition rule below, which makes any order safe.

## What real systems do

- **Plan 9** invented UTF-8 for itself (1992) and switched everything on
  one day: strings are UTF-8, a decoded character is a `Rune`, and the
  libraries step with `chartorune()`. One flag day was possible because
  one group owned every program.
- **Linux** keeps the kernel out of it: a file name is bytes, compared
  bytewise, except where a disk format says otherwise (VFAT, exFAT and
  NTFS convert UTF-16 names through an `iocharset`/`utf8` mount option).
  Distributions moved their locales from ISO 8859-x to UTF-8 (Red Hat 8,
  2002, through Debian by 2007) and left old names to `convmv`. The
  console decodes UTF-8 in Unicode keyboard mode (`kbd_mode -u`) and
  draws from a font of at most 512 glyphs.
- **Windows NT** is UTF-16 inside (1993) with code pages ("ANSI") at the
  edges; since 1903 a program can make UTF-8 its ANSI code page by
  manifest -- the platform working its way back to UTF-8 at the edges.
- **macOS** takes UTF-8 at its APIs; HFS+ stored names decomposed (NFD)
  and APFS compares them normalisation-insensitively.
- **Toolkits** (Pango with fontconfig, Qt, DirectWrite) cache glyphs per
  (face, size, glyph) on demand and fall back through a list of faces
  when the chosen one lacks a character; a character nothing covers is
  drawn as an empty box ("tofu").
- **Editors** (VS Code, Kate, Notepad++) read a file as UTF-8 and fall
  back to Windows-1252 or Latin-1 when it is not valid, keeping the bytes
  they did not edit.

**SaunaOS follows Linux and Plan 9**: UTF-8 in every `char *`, names as
bytes in the kernel, decoding at the edges where text is drawn or
edited. It copies the toolkits' glyph cache and fallback chain. It
deliberately differs from Linux in converting the old names ONCE at the
update rather than leaving `convmv` to the user -- everything on this
disk is Latin-1 by construction, so the conversion is knowable, which it
never was on a Linux machine with decades of mixed files.

## What exists, measured (2026-10-28)

- **The session font** is rasterised by `/bin/fontd` (ring 3, sandboxed)
  into shm objects `font.N` that every client maps read-only
  (`map_fontd_font()` in `ui/ugfx.c`): 191 cells of coverage, a one-byte
  advance each and a 191 x 191 kern table, indexed through
  `font_ttf_slot()` (`api/font_ttf.h`). `glyph_index()` takes an
  `unsigned char`, so nothing above 0xFF can be named. The kernel's baked
  copy (`WIN_REQ_FONT`) is the fallback when fontd is absent.
- **A per-codepoint UTF-8 pipeline already exists, three times over**:
  `ui/uglyph.c` (Character Map, the font samples) decodes UTF-8 and
  rasterises any glyph of a font file through `lib/ttf.c`; the browser's
  `webcontent/ns_font.c` walks UTF-8 with its own advance and missing-
  glyph box; `lib/uunicode.h` encodes and decodes. Each has its own
  baseline and kerning model, and uglyph's cache (a linear scan of 384
  entries, a malloc per miss) is sized for a character map, not for every
  label on the desktop.
- **Edges that already speak UTF-8**: exFAT names (UTF-16 on disk,
  `exf_utf8_to16()`), VNC cut text (converted to Latin-1 for the
  clipboard today), w16's clipboard (Windows-1252 at its edge), SMB
  (which sniffs: UTF-8 only when a name needs it). **So mojibake exists
  today**: an exFAT stick's file named `é` arrives as C3 A9 and the File
  Manager draws it as `Ã©`.
- **Edges that do not**: FAT32 long names turn anything past ASCII into
  `?` and refuse to create one; keyboard layouts are bytes
  (`parse_value()` refuses above 0xFF, dead-key tables are `uint8_t`,
  `tools/gen_kbs.py` emits only Latin-1); the kernel's key path is
  `emit(uint16_t)`.
- **The clipboard is untagged bytes in two encodings**: Character Map
  copies Latin-1 when the text fits and UTF-8 when it does not
  (`docs/decisions/gui.md`); VNC assumes UTF-8.
- **Width**: the key event is already an `int` (`WIN_EV_KEY`'s `a`), and
  the special keys were moved into the Private Use Area precisely so a
  codepoint could share the stream (`api/keyboard.h`). Dead keys,
  though, are layout SYMBOLS from 0x100 (`KB_SYM_DEAD_BASE`), which is
  exactly where codepoints past Latin-1 start.
- **Fonts shipped**: DejaVu Sans Mono and Liberation Sans/Serif -- Latin
  Extended, Greek, Cyrillic, box drawing, arrows. Nothing covers CJK.

## Decided (2026-10-28, the maintainer)

- **UTF-8 in every string**; the kernel and filesystems keep names as
  bytes; decode where text is drawn or edited.
- **This document first, then step 1**; each later step asked for.
- **Latin-1 already on disk is converted ONCE, at the update** -- the
  `convmv` shape, logged -- not decoded with a fallback for ever.

## Decided here, and why

1. **A decoded character is a `uint32_t`, and only transiently.** No
   string type, no UTF-32 buffers: a caller steps with
   `ugfx_text_next()`/`ugfx_text_prev()` over its `char *`. Plan 9's
   shape.
2. **An invalid byte is ONE character, drawn as its Latin-1 glyph -- an
   open question below.** Decoding never fails: a byte that does not
   start a valid sequence is a character of its own, the cursor steps
   over it whole, and it is drawn as the Latin-1 character with that
   value. That is the transition rule: old Latin-1 text keeps drawing
   correctly before the conversion, and a stray Latin-1 name on an old
   SMB share or FAT stick afterwards reads as itself rather than `�`. No
   byte is rewritten unless the person edits that span.
3. **Glyphs come from three places, in order.** The shared atlas stays
   the fast path for its 191 characters -- nothing about it changes. Any
   other character comes from a per-process glyph cache rasterised from
   the session face's own `.ttf` (`lib/ttf.c`, the face name already in
   the atlas header), keyed (face, size, character), hashed rather than
   scanned, bounded in bytes with least-recently-used eviction. A
   character the face lacks falls back to DejaVu Sans Mono (the widest
   coverage shipped), and one nothing has is drawn as an outlined box the
   width of the advance. uglyph and the browser move onto the same cache,
   so there is one text model, not three.
4. **Metrics**: a cached glyph's advance is the font's (`hmtx`), at full
   precision rather than the atlas's one byte. Kerning stays the atlas's
   table for its own pairs and is skipped across a cached glyph in step 1
   -- written down, because it is visible on Greek and Cyrillic pairs.
5. **Every `ugfx` string function takes UTF-8**, and character-level
   calls take a codepoint (`ugfx_cp_advance(uint32_t)`,
   `ugfx_draw_cp()`). The hand-rolled loops are converted in step 1 and
   **a static check fails the build on a new one** -- the repo's usual
   ratchet (`tools/check_*.py`, a named waiver) -- because a loop added
   later reintroduces `Ã©` silently.
6. **A grid cell holds a codepoint and a width.** `uvterm_cell` grows
   from three bytes to a 32-bit character plus colours; `utext` keeps a
   byte buffer and computes columns by decoding. An East Asian Wide
   character takes two cells (`wcwidth()`'s rule, from Unicode's
   `EastAsianWidth.txt`, generated by a tool into a table -- the
   licence is already in `data/licenses/unicode.txt`).
7. **Editing steps by character.** `uui_edit` moves with
   `ugfx_text_next/prev`, Backspace erases the previous character's
   bytes, a typed key's codepoint is inserted as its UTF-8 encoding, a
   masked field draws one `*` per character. `klineedit` the same, in
   both rings.
8. **The clipboard is UTF-8, always**, from step 1. Character Map's
   "Latin-1 when it fits" rule retires; VNC and w16 already convert at
   their own edges.
9. **File names stay bytes; length is a BYTE budget.** TFS3's 255 is 255
   bytes (ext4's rule), so a name in Greek holds about half as many
   letters. FAT32 long names become UTF-16 <-> UTF-8 with exFAT's
   converters (moved to a shared helper); SMB sends UTF-8 always.
10. **The kernel console keeps its baked font** -- the boot font must
    exist before any process does, as Linux's VT font does. It decodes
    UTF-8 and draws a character outside its set as `?`.

## The steps

### Step 1 -- drawing and editing in ring 3 (the GUI)

- `ugfx`: every string function decodes UTF-8; the glyph cache, the
  fallback face and the missing-glyph box; codepoint-level calls.
- The hand-rolled measuring loops converted, and the static check that
  keeps them converted.
- `uui_edit` (every text field, `uui_textbox`, `utext`) steps, erases and
  inserts by character; `utext`'s columns by decoding and width; typed
  keys inserted as UTF-8.
- The clipboard UTF-8; uglyph and the browser onto the shared cache.
- **Not in step 1**: the terminal and the shell's line editor, keyboard
  layouts past Latin-1, any filesystem, the conversion, the kernel. With
  the transition rule, Latin-1 still arriving from all of those keeps
  drawing correctly.

**Visible result**: a UTF-8 file from anywhere -- Polish, Greek,
Russian, box drawing -- opens in Notepad and shows its letters; an exFAT
stick's non-ASCII names stop being mojibake; Character Map's characters
paste as themselves into any field.

### Step 2 -- the terminal, the shell, and typing past Latin-1

- `uvterm` cells hold codepoints with widths; the decoder's pending
  bytes live in `struct uvterm` beside the ANSI state, because a read
  can split a sequence (one per call would turn it into two `�`).
- `klineedit` by character in both rings; `termkey_encode()` emits UTF-8.
- Layouts emit codepoints: dead-key symbols move out of the codepoint
  range, the kernel's key path widens to 32 bits, `gen_kbs.py` emits
  past Latin-1, and the TTY encodes typed characters as UTF-8 (Linux's
  Unicode keyboard mode). **Unlocks Estonian, Latvian, Polish and
  Romanian.**

### Step 3 -- the disk

- **The conversion, once, at the update**: a one-shot service walks every
  mounted TFS3 volume and renames each name that is not valid UTF-8 from
  Latin-1 to UTF-8 (a clash gets ` (2)`, as a copy does), converts the
  text files the open question below settles, logs every change to
  `/var/log`, and leaves a marker so it never runs twice. A name that is
  already valid UTF-8 is never touched -- which is what makes running it
  twice harmless anyway.
- FAT32 long names through UTF-16; SMB UTF-8 always.

### Step 4 -- the rest

- VNC's Extended Clipboard (UTF-8 both ways), `ps --tree`'s box drawing,
  the kernel console's decode, the roadmap's remaining UTF-8 items.

## Testing

Each step's checks go red on the build before it (the positive control):

- **A corpus**, `data/tests/utf8/`: Polish, Greek, Cyrillic, box drawing,
  a CJK line (tofu, by design), an invalid byte, and a Latin-1 file.
- **Drawing**: Notepad opened on the corpus, compared by PIXELS against
  uglyph rendering the same text (an independent path); the Latin-1 file
  still shows `å` (the transition rule's control).
- **Editing**: one Right moves past `ż`, one Backspace removes both of its
  bytes (checked through the saved file, not the screen), a masked field
  of `żż` shows two stars.
- **The static check** red on a planted byte loop.
- **Step 2**: a sequence split across two pty reads draws once; a Polish
  layout types `ą` into the terminal and `cat` shows it.
- **Step 3**: a disk image seeded with Latin-1 names and files, converted,
  listed through the shell, then converted again with nothing changed.

## Non-goals, written down

Right-to-left text and bidirectional layout; shaping (Arabic joining,
Indic scripts); composing combining marks (one is drawn as a character
of its own); normalisation (names compare bytewise, Linux's rule, not
macOS's); input methods; translations. **A CJK font** is not shipped
(DejaVu and Liberation have no CJK, and Noto Sans CJK is tens of
megabytes a weight) -- see the open questions.

## Open questions for the maintainer

1. **An invalid byte: drawn as its Latin-1 character (recommended), or
   as `�` (U+FFFD)?** Latin-1 makes any step order safe and keeps old
   media readable; `�` is what GTK and most terminals show, and makes a
   wrongly encoded file obvious.
2. **Which file CONTENTS step 3 converts** (names are always converted):
   `/etc` only, or also text files under `/home` with a text extension
   (`.txt`, `.md`, `.conf`, `.desktop`, `.ini` outside w16's drives,
   whose files are Windows-1252 by contract)?
3. **A CJK font**: none (CJK shows as boxes), or an optional download
   behind `EXTRAS=1` / the DOOM-style downloader.
4. **The static check on byte loops**: fail the build (recommended), or a
   report only.

## The case against

**"Latin-1 is enough."** It stopped being enough on 2026-10-28, twice
over: Latvia and Romania are regions you can pick but layouts you cannot
type, and an exFAT stick or an SMB share already hands SaunaOS UTF-8
names that it draws as mojibake. Every month of Latin-1 adds content the
conversion has to handle.

**"It is too wide."** It is wide rather than deep -- most of step 1 is
replacing one loop shape with another -- and the static check is what
keeps a wide change from leaking back.
