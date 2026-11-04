# `tools/gen/` -- Generators

One entry per tool in `tools/gen/`: what it does, why it exists, and the traps it
encodes. The index of every folder is `docs/tools.md`; `tools/t which NAME` finds a tool.

- **`gen_tools_index.py`** -- regenerates `docs/tools.md`'s "Every tool,
  by folder" section from each tool's own docstring (or a shell script's
  first comment), between its BEGIN/END markers. `--check` is run by
  `check_docs.py`, so a tool added, moved, deleted or re-described
  without re-running it fails the gate. The folder order and roles come
  from `toolpath.FOLDERS`, which `tools/t` prints too.

- **`genttf.py`** -- font generation; `--font PATH` when JetBrains Mono
  is not installed -- the v2.304 release reproduces the committed tables
  byte for byte

- **`gen_kbs.py`** -- generates the `seed/sync/usr/share/kbs/<layout>`
  keyboard-layout data files, dead keys included, from Linux's own XKB
  data; `--all` for every layout in its `LAYOUTS` table, `--check` to
  re-measure that each still fits the format (a codepoint for every key,
  its dead keys within the kernel's fixed accent table; it reads
  `/usr/include/X11/keysymdef.h` for a named keysym past Latin-1),
  `--choices` for the Settings display names -- see `docs/decisions.md`
  on layouts being data files

- **`genrelocs.py`** -- builds the kernel's own relocation table for
  kernel ASLR: extracts every ABSOLUTE reference from a
  `ld --emit-relocs` link and emits it as a C array the second link
  pass embeds in `.krelocs` (~7,400 fixups, 29 KB), which
  `kernel/arch/x86_64/reloc.c` applies at boot. `--verify` re-derives
  the table from the FINAL image and fails the build if the two
  disagree -- run automatically by the kernel's link rule, because a
  table that disagrees with its image is otherwise a kernel that does
  not boot with nothing to read. Three traps live in the Makefile rule
  and are commented there: the shipped kernel must have its `.rela`
  sections stripped (GRUB will not boot the `--emit-relocs` image, and
  the symptom is an EMPTY serial log), `build/krelocs.c` must be a
  named prerequisite and `.PRECIOUS` or make deletes it as an
  intermediate, and `kernel/arch/x86_64/linker.ld`'s `.krelocs` must stay after `.data`.
  See `docs/decisions.md`.
- **`gen_next_up.py`** -- regenerates the "Next up" section at the top
  of `docs/roadmap.md` from the `**NEXT**` markers on the items
  themselves.

  **WHY THERE IS A SECOND AXIS.** The roadmap is ordered by DEPENDENCY,
  which is what makes reading it top to bottom answer "what next" for
  the phases -- and deliberately not by priority, since a total priority
  order would imply the tracks depend on each other when they do not. So
  urgency cannot share that ordering, and an item states its own where
  it lives.

  **WHY IT IS GENERATED**, rather than a short hand-kept list of titles
  at the top: that list is a pointer whose correctness depends on
  somebody remembering to update a second place, which is the shape of
  every maintenance burden this repo has deleted (CLAUDE.md's "prefer
  facts that cannot go stale"). `check_docs.py` runs it with `--check`
  and fails the build when the committed section is stale, exactly as it
  already does for the decisions index.

  It collects only UNFINISHED items: a marker that survived a tick would
  put completed work in the queue, and nobody remembers to remove one.

  **The generated block is EXEMPT from `check_docs.py`'s one-line cap.**
  This copies each marked item and appends its section name, so an item
  that fits when typed can fail as its own generated copy -- a rule
  punishing the wrong line, in a file nobody edits by hand. The cap is
  about what somebody writes; both halves are still checked at the
  source item.
- **`gen_commands_index.py`** -- regenerates the categorised index of
  command pages into `docs/commands/README.md`, the same shape (and for
  the same reason) as `gen_decisions_index.py`. **The category comes
  from each PAGE**, a `**Category:**` line near its top -- not from a
  table in the script, which would be the central list the settings
  registry deliberately does not have, where a new command means editing
  two files and the one nobody edits is the table. A page with no
  category is reported rather than filed under a default, since a
  catch-all is the pile nobody reads. `check_docs.py` runs it with
  `--check` and fails the build when the committed index is stale: the
  coverage check refuses a command with no page, and this refuses a page
  nobody can find.
- **`gen_modalias.py`** -- writes `/lib/modules/modules.alias` from the
  built `.ko` files (one `pci <vendor> <device> <class> <subclass>
  <progif> <module>` line per PCI match in each module's
  `.pci_drivers`), which is what the kernel loads modules BY at boot --
  Linux's `depmod` output, derived from the objects themselves so it
  cannot disagree with them. It also FAILS THE BUILD on a module whose
  undefined symbols are not all `EXPORT_SYMBOL`s in
  `kernel/core/kexports.c` (the load would be refused at runtime;
  this names the symbol at build time) and on a relocation type the
  loader does not handle (a module compiled without `-mcmodel=large`).
  `kernel/samples/modules/unexported.c` is the deliberate exception. Hand-rolled ELF64
  parse, so the build needs no pyelftools.
- **`gen_syms.py`** -- bakes the kernel's function symbol table into the
  image so a panic can name the function instead of printing an address
  nobody can resolve (the kernel relocates itself, so a raw RIP is
  meaningless without the boot log). Same two-pass + `--verify` shape as
  `genrelocs.py`; the blob is deliberately pointer-free so it costs no
  relocations.
- **`gen_w16_calls.py`** -- writes `/usr/share/doc/windows/calls.md` and
  `stand-ins.md` at seed time (`-o DIR`) from the comment block above each
  handler in `userland/w16/`'s API tables: the call's signature line,
  `Windows:`, `w16:`, and for a stand-in (`task16.c`'s `PARTLY`) `Why:`,
  `Notice:`, `Whole:` -- Wine's c2man shape. Help reads the pages whole;
  Retro Launcher's About and Why? and `w16 --why` show one call's section.
  It also writes `missing.md` from `userland/w16/missing16.c`'s blocks for
  calls w16 does NOT answer (`Windows:`, `Why:`, `By:`), a `Why:` that
  names another such call ("As OpenComm.") followed by that call's reason.
  It FAILS the build on a call without its block, a stand-in without its
  three fields, a non-stand-in with them, or a missing-call block for a
  call that is answered now, or whose name is not w16_names.c's; `--check` is what
  `check_docs.py` runs, `--missing` lists every problem by file and line.
  The table-to-module map is read from `task16.c`'s `w16_has()` and the
  names from `w16_names.c`, so a new table or module needs no edit here.
  `docs/conventions/gui.md` has the format.
- **`gen_toolkit_index.py`** -- writes `docs/toolkit.md`, one row per
  `userland/lib/` and `userland/ui/` header, taken from the header's own
  top comment (its first sentence). `--check` fails when the file is
  stale or a header has no top comment, and `check_docs.py` runs it -- so
  a new header needs a first comment line that says what it is, and then
  a rerun. It exists because the ring-3 toolkit had no index and an app
  hand-rolled a fourth `.desktop` reader beside `uconf`.
- **`gen_audio.py`** -- generates the WAV files that ship:
  `data/usr/share/sounds/*.wav` (the sound effects the Audio Player
  lists and Minesweeper plays) and `data/tests/sine1k.wav` (the
  FIXTURE). Written here rather than fetched, for the reason the
  wallpapers and icons are: a build-time dependency on somebody's media
  files is the failure that shipped images with no keyboard layouts for
  months, silently. **Every file is deliberately in a DIFFERENT
  format** -- 44.1 kHz stereo 16-bit, 22.05 kHz mono, 48 kHz mono,
  44.1 kHz 8-bit unsigned -- so the shipped data alone exercises each
  branch of `userland/lib/usnd.c`'s rate/channel/width conversion on
  every boot. A set that were all 48 kHz stereo would exercise the
  copy-only fast path and nothing else, forever. `sine1k.wav` is
  separate and is the only one with a measurable property: a STEADY
  1 kHz tone, because `tools/tests/sweep/audio_test.py` judges the recording by
  counting zero crossings on the host and the musical files have no
  single frequency to count. No Pillow, no third-party module -- a WAV
  is a 44-byte header and some integers. Its noise burst uses a fixed
  LCG rather than `random`, so a regenerated file is byte-identical and
  a tracked binary does not change for no reason.
- **`gen_imgdata.py`** -- generates the wallpapers and the decoders'
  test vectors: `data/wallpapers/*.jpg` (the desktop backgrounds, drawn
  here rather than committed as somebody's photograph, so the repo owns
  every pixel it carries; `lakeside`, a sauna cabin at dusk, is the
  default -- `desktop.wallpaper`'s `Default=` and `desktop.c`'s
  `WALLPAPER_DEFAULT` -- and `sauna` the room itself) and `userland/tests/uimg_vectors.h` (the JPEG
  decoder's test vectors). One tool because both are "a JPEG produced by
  libjpeg for SaunaOS to read back", and splitting them would leave two
  scripts with the same encoder settings to keep in step. **The vectors'
  reference pixels are LIBJPEG's, not this decoder's**, which is the
  whole point of them: a decoder compared against its own output is
  self-consistent, and so is one with a wrong IDCT constant. Both
  outputs are COMMITTED, like the cursor themes and the baked font, so a
  checkout without Pillow still builds. Nine vectors, chosen for what
  each can fail at on its own: 4:4:4/4:2:2/4:2:0, a 17x9 image whose
  partial MCU catches a decoder that forgets to crop its padding,
  grayscale, restart markers (a decoder ignoring them drifts in BANDS
  after the first interval), and three refusals -- progressive,
  truncated, not-an-image -- which must come back as the RIGHT errno,
  since a decoder answering "broken" to everything would pass a test
  that only asked whether it failed. BMP and GIF vectors too (Pillow
  writes and reads them, tolerance 0), and one ANIMATION -- each frame
  as Pillow composites it, each delay after `uimg_anim_next()`'s clamp.
  `--pictures` writes `data/usr/share/pictures/`: a sample of each BMP
  and GIF kind the decoders read, two GIFs animated, the RLE8 and alpha
  BMPs packed by hand because Pillow writes neither.
- **`gen_icons.py`** -- draws the application icons into `data/icons/`
  (one 64x64 QOI per icon NAME) and `--check`s that the files on disk
  match the script, the same contract `gen_cursors.py` has. It holds
  the SYMBOLIC glyphs (toolbar, tray -- one flat ink, tinted at draw
  time); the colour art is **`icon_art.py`**, its module: each app as
  its own object with a soft gradient and shadow, no plate (Windows 11's
  and GNOME's shape, chosen from mockups 2026-10-05), and the folders,
  file types, drives and categories to match. That art is drawn at 4x
  and box-reduced in premultiplied alpha, because Pillow's ImageDraw does
  not anti-alias; its `D4` draws in the 64-px space at 4x, so an older
  64-px drawing (the folder emblems) ports verbatim. Drawn here rather
  than committed as somebody's icon set so the repo owns every pixel it
  ships. **PILLOW encodes
  them**, which is what keeps `uimg_qoi.c`'s DECODER honest: these files
  come from a foreign implementation, so a chunk type the decoder
  misreads cannot round-trip through a matching bug of our own. (The
  repo does write QOI now -- `/bin/screenshot` does -- and its encoder is
  checked the other way round, by Pillow decoding it; see
  `uimg_codec_hostcheck.py`.) **Crash Test gets no
  icon on purpose** -- it is the entry that exercises the letter-tile
  fallback on every boot, the same trick `data/fonts/` plays by shipping
  `vera-mono` with no bold companion.
- **`gen_sf2.py`** -- generates `toy-gm.sf2`, the built-in General MIDI
  SoundFont, into `data/usr/share/soundfonts/` (tracked). Every bank
  worth hearing is somebody else's and tens of megabytes, so the
  repository carries a small one of its own and `fetch_soundfont.py`
  adds a real one. **Each melodic sample is an attack plus ONE EXACT
  CYCLE**: additive partials with their own decays for T seconds, then
  frozen amplitudes for the loop, seamless because the sample's rate is
  `L x f0` for an integer cycle length L. One root per octave,
  band-limited for its zone; twenty timbres shared by the 128 programs
  through preset-level generator offsets; drums as one-shots. **It
  writes attenuations divided by 0.4**, because every synth that matters
  reads the generator as 0.4 dB a unit. Deterministic; pure Python, no
  numpy; nothing in the build runs it.
- **`gen_mp3_tables.py`** -- regenerates
  `userland/lib/usnd_mp3_tables.h`, the three Layer III tables that have
  no generating formula (the Huffman codes, the 512-tap synthesis window,
  the scalefactor band edges). It exists so that data which was TAKEN can
  be CHECKED: without it the header is 473 lines nobody can audit without
  redoing the work, and `loc.py` counts it as code somebody wrote.

  **It verifies far more than it copies.** Two public-domain
  implementations store the Huffman tables in two completely different
  packed formats; this walks BOTH back to the standard's plain (length,
  codeword) per (x, y) form and refuses to write unless they agree entry
  for entry -- 1378 entries across 15 tables -- with every table also
  required to be a complete prefix code. Both checks earned their place:
  an early hand-written table 7 failed the Kraft check, which is how
  transcribing from memory was abandoned, and a first version of the
  pdmp3 walk dropped that format's `>= 250` long-jump escape, which
  surfaced as table 24 disagreeing in 63 of 256 entries -- the walker was
  wrong, not the data, and only having two sources made that visible.

  `--check` verifies without writing (useful for asking whether the
  committed header still matches its sources); `--from DIR` uses local
  copies instead of downloading. Not run by the build and not in any
  gate: it needs the network, and the header it writes is committed.
- **`gen_music.py`** -- generates the MP3s that ship, into
  `data/usr/share/music/` and `data/tests/`. Same call `gen_audio.py`
  made for the WAVs: written rather than fetched, because a build-time
  dependency on somebody else's media is the failure that shipped images
  with no keyboard layouts for months. The output is tracked, so a clean
  checkout has music, and it is ours, so `LICENSE` needs no entry for it.
  **The encode is as much the point as the tune**: `first-boot.mp3` is
  joint stereo VBR with an ID3v2 tag (mid/side frames, a Xing header,
  and a few KiB to skip before the first sync word), while
  `data/tests/sine1k.mp3` is mono CBR with no tag at all -- and it is a
  steady 1 kHz tone because `audio_test.py` judges playback by counting
  zero crossings in QEMU's own recording, which music cannot be judged
  by. It also writes `first-boot.mid`, THE SAME SCORE as a Standard
  MIDI File -- format 1 with a conductor track, running status, a
  sustain pedal, pan and modulation, a pitch-bend scoop and a closing
  ritardando, so the data alone exercises the MIDI codec. With `flac`
  on PATH it also writes `first-boot.flac` (the first 20 s at 24 bits,
  Vorbis comments and a cover PICTURE), `data/tests/sine1k.flac`, and
  `data/tests/ramp24.flac`, whose every sample is a formula of its index
  so `/tests/usnd_test` checks exactness with no reference decoder.
  Needs `lame` (not for `--midi-only`); nothing in the build runs it.
- **`gen_video.py`** -- generates the videos that ship, into
  `data/usr/share/videos/` (and `dusk.mpg` into
  `data/wallpapers/animated/`), plus `/tests`' fixtures. The call
  `gen_music.py` made: drawn here (ffmpeg's lavfi sources, our fonts,
  our music) and tracked, so a clean checkout has video and `LICENSE`
  needs no entry. **Each clip is a different path through the decoders**
  -- I/P/B MPEG-1 with MP2 at 44.1 kHz, the same at 48 kHz, Motion JPEG
  with 22.05 kHz mono PCM in AVI -- and **every frame carries its number
  and timecode burnt in**, because identical content moving is
  pixel-identical and a dropped frame must be told from a repeated one.
  `av-sync.mpg` puts a white frame and a 1 kHz beep at every second, for
  `vplay --sync`. The fixtures: `tiny.mpg` (72x40, not a multiple of
  16) with `tiny.yuv`, FFmpeg's decode of it, as `uvid_test`'s
  reference; `tiny.avi`, whose PCM is a formula of the sample index.
  Needs ffmpeg with libfreetype; nothing in the build runs it.
- **`gen_unicode_names.py`** -- Unicode's names and blocks for exactly
  the code points `data/fonts/*.ttf` map, into
  `data/usr/share/unicode/{names,blocks}` (tracked) for
  `lib/uunicode.h` and the Character Map. Names from Python's
  `unicodedata`, blocks from Perl's copy of `Blocks.txt`: nothing is
  fetched. Re-run when a font is added; the Unicode License travels as
  `data/licenses/unicode.txt`.
- **`gen_wcwidth.py`** -- the East Asian Wide table a grid of cells
  measures with (`wcwidth()`'s rule: 2 cells for W and F, 1 otherwise)
  into `userland/lib/uwcwidth_table.h` (tracked), read by
  `lib/uwcwidth.h` for `utext`'s columns. From Python's `unicodedata`
  at the interpreter's Unicode version, printed into the header: nothing
  is fetched. `--check` fails on a stale table and only REPORTS one from
  another Unicode version. Re-run when a newer Python brings a newer
  Unicode.
- **`gen_cursors.py`** -- generates the shipped cursor themes into
  `data/cursors/`, which the Makefile's `seed` target stages onto the
  image. **Into `data/`, NOT `seed/sync/`** -- that tree is gitignored
  and `make clean` deletes it, so the first version's themes were never
  committed and every checkout but the authoring one silently got the
  built-in fallback. It is the authoring path for a new theme too: every
  theme is a row of `IMAGE_THEMES` (fill, rim and busy-ring colours,
  and optionally a heavier rim and body, as Classic bold has) drawn over
  the shared supersampled geometry (`GEOM`) and rendered natively at
  1x/2x/3x into QOI files that Pillow encodes. Classic and Classic bold
  were mask themes ported from the built-ins until 2026-10-01; they are
  image themes on the same geometry now. `--check` fails
  if they are stale -- binary QOIs included, so a different Pillow that
  encodes differently reads as stale (re-run and look at the art).
- **`gen_saver_thumbs.py`** -- draws each screensaver's picture for System
  Settings' gallery and monitor (`data/wm/savers/<name>.jpg`). A saver is
  a program, and Settings can neither draw nor host one, so each ships a
  frame the way a macOS screen saver bundle ships a thumbnail. Run on a
  VM booted from a fresh image, so the Desktop cube's picture shows the
  default desktop. A dark frame of small points is grown before it is
  shrunk, or Starfield's stars average away to black. **A new saver
  needs it run.**
- **`gen_bootwords.py`** -- `build/gen/bootwords.h`, the boot words
  `bootcfg` and the Boot Manager accept, parsed out of
  `docs/boot-flags.md`'s table at Makefile PARSE time (as `kconfig.h`
  is) and rewritten only when it changed. The table is the only list of
  boot words -- the kernel matches by substring with no registry -- so a
  word documented there is accepted on the next build and one that is
  not cannot be offered. The description is the Effect column's first
  clause with markdown and shouting stripped; `--print` shows the result.
  It fails the build if it finds fewer than ten rows, which is what a
  reshaped table would look like.
- **`gen_signames.py`** -- `signames.c` for the dash port, from
  `kernel/include/abi/signal_abi.h`. Replaces dash's own
  `src/mksignames.c` for two independent reasons, either of which would
  be enough: that file is **GPL-2 from GNU Bash** and its output is
  LINKED, so using it would pull the GPL into the shell binary; and it
  reads the **HOST's** `<signal.h>`, so a dash built with it would list
  Linux's signals and miss this kernel's numbering. The names are parsed
  out of the header rather than written down, so a signal added there
  appears on the next build. Run by the Makefile, not by hand.
- **`gen_ne.py`** -- builds a Win16 (NE) executable from ONE NASM source
  written with `userland/w16/samples/ne16.inc`, so w16's test programs
  need no foreign toolchain and no Microsoft binary. The source is
  assembled once per segment (`-DW16_PASS=n`), the other segments laid
  out in nobits sections from 0 so code can name its data; macros record
  imports, internal far pointers, the entry and exports in a trailing
  section; `;w16:` comment lines set the module, segments, stack and
  heap, and the resources -- strings, bitmaps, dialogs and their
  controls, menus, accelerators, 32x32 cursors drawn as rows of `.`
  clear, `~` invert, `#` black, `o` white -- so a test program stays one
  text file. Each relocation is a chain of one (its site holds 0xFFFF).
  `--dump FILE` is an independent reader the host check compares with.
  `make iso` builds every sample into `/usr/share/w16/`.
- **`gen_w16_ordinals.py`** -- writes `userland/w16/w16_names.c`, each
  Win16 export's ordinal, name, Pascal argument bytes and KIND (what it
  is for: Dialogs, Files, Clipboard...), for KERNEL, USER, GDI and the
  other DLLs a 3.x program imports (COMMDLG, SHELL, KEYBOARD, OLECLI,
  MMSYSTEM...), from Wine's 16-bit `.spec` files -- the authoritative
  public list, read from a directory you fetched them to (`--urls`
  prints where), so the build never reaches the network. Kinds come
  from `KIND_RULES`, first match wins, with a default per module so
  "Other" means only an import with no name at all.
- **`gen_win3_disks.py`** -- a Windows 3.x setup disk set with no Microsoft
  file in it: a SETUP.INF in the shape of Windows for Workgroups 3.11's
  (each section the installer reads, decoys for other hardware and
  languages), its files KWAJ-compressed under `NAME.EX_` names, written as
  one folder, a folder per disk, 1.44 MB floppy images or a mix
  (`--form`). It says where each file must land from ITS OWN table, not
  the installer's rules; `--list` prints it. Used by
  `uwinsetup_hostcheck.py` and the guest test of `w16 --install`.
- **`mscompress.py`** -- COMPRESS.EXE's two formats WRITTEN: `kwaj IN OUT
  [--method 0-3] [--name NAME.EXT]`, `szdd IN OUT`, and `kwaj()`/`szdd()`
  to import. Method 3's tables go out in the cheapest of the four
  code-length encodings (`force_type=` picks one), and a file always
  records its length -- the stream has no end marker. The other half of
  `umsexpand_hostcheck.py`, and how a test builds a Windows setup disk from
  its own files.
- **`hlpgen.py`** -- writes WinHelp 3.1 files from Python (`Help`,
  `Topic`, `sample()`): the B+ trees, LZ77 blocks, phrases, fonts,
  contexts, keywords, windows, links, macros and tables. The input of
  `uhlp_hostcheck.py`, and the file a GUI test of Help Viewer can open.
  Its docstring has the layout rules a writer shares with a real file
  (a record may not cross a block unless `span=True`, which writes
  literals-only blocks filled to their last byte, `SPAN_CHUNK` bytes each
  -- zero padding after compressed data decompresses into garbage;
  TOPICPOS counts the display text written in the block so far; a topic
  header's NonScroll and Scroll are RECORD addresses; a window record has
  its type at 2, name at 12, caption at 21, colours as COLORREFs).
