# `tools/hostchecks/` -- Host checks

One entry per tool in `tools/hostchecks/`: what it does, why it exists, and the traps it
encodes. The index of every folder is `docs/tools.md`; `tools/t which NAME` finds a tool.

Run by `tools/runners/hostchecks.py` (`tools/t host`), all in parallel;
the ones whose `RUNNER` says `gate` also in every preflight (`--gate`),
the rest in the sweep. Each stays a standalone script with its own
`--positive-control`, built through `tools/lib/hostcheck.py`.

- **`smb_hostcheck.py`** -- the SMB client's protocol
  (names UTF-8 both ways since UTF-8 step 3: a CJK name, and a stray
  Latin-1 byte reaching its file as its letter; red on the sniffing
  converter before it)
  (`userland/dynlib/usmb*.c`, `ucrypto_mbedtls.c`) built with the host's
  gcc against the in-tree mbedTLS, driven against `samba_server.py` for
  every dialect (2.0.2 to 3.1.1, signing mandatory) and the refusals
  (wrong password, no share, encryption required, guest against required
  signing), with 1 MiB reads where large MTU allows them (2.0.2 must stay
  at 64 KiB). Bytes are compared with what the fixture wrote. The
  positive control breaks the 3.x signing-key labels and must go red.
  `ondemand_sweep.py` runs it when the image exists.
- **`uimg_hostcheck.py`** -- the same `userland/libs/media/uimg_jpeg.c` and
  `uimg_jpeg_enc.c`, compiled with the host gcc and run against ~370
  Pillow-generated images (four patterns x five sizes x three
  subsamplings x three qualities, each BOTH sequential and PROGRESSIVE,
  plus grayscale, restart markers and all eight Exif orientations),
  every pixel compared against libjpeg's. **It exists because a guest
  test can only carry the vectors somebody committed**, and a JPEG
  decoder's bugs live in the combinations. It is how the chroma
  upsampler was found to be visibly wrong: replication needed a
  tolerance of 70 to pass, and the triangle filter libjpeg uses brought
  the worst case to 3.

  **`--against REV` is the check for a speed-up that must change no
  pixel**: ~130 files (every subsampling sequential and progressive at
  sizes up to 641x359, grayscale, restart markers, the eight
  orientations, and any `--file`) through this tree's decoder and REV's,
  byte for byte. A control has to CHANGE OUTPUT to prove anything: the
  colour constants split around 65536 are exact over the whole input
  range, so moving R's 26345 by one alters no pixel at all; G's -18734
  by one changes 56 of the 65536 (Cb, Cr) pairs and reddens 55 files.
  `/tests/uimg_bench FILE [REPS]` times a decode on the machine itself,
  with the first touch of a fresh buffer the image's size beside it.

  **`--positive-control`** removes the successive-approximation
  correction bit -- the one place a progressive decode ADJUSTS a
  coefficient rather than assigning it, and the bug that leaves a
  plausible, slightly wrong picture. Every judged progressive check must
  go red and every baseline one stay green. The `gradient` pattern is
  excluded from that verdict, and the reason is about the FIXTURE: a
  smooth ramp quantises to almost no nonzero AC coefficients, so the
  sabotaged line is never reached and five of them decode identically
  without it. Judging them would make the control's bar a measurement of
  how flat the test images are.

  **The ENCODER is checked in two ways, because they fail differently.**
  Our decode of the file we wrote against libjpeg's decode of the same
  bytes, at the decoder's own tolerance of 3 -- two independent decoders
  reading one bitstream must agree to a rounding step. And how much the
  file LOST against how much libjpeg loses encoding the same image at
  the same quality, which is a COMPARISON rather than a threshold on
  purpose: 4:2:0 at q85 moves a 3-pixel checkerboard by 180 levels and
  that is the format working as designed, so an absolute bar loose
  enough to pass it would pass real damage too.

  `--file` checks a real photograph; `--tolerance` tightens the bar. Not
  in any gate: it needs Pillow, and `/tests/uimg_test` is the version
  that runs in the guest.
- **`uimg_codec_hostcheck.py`** -- the QOI and PNG codecs, BOTH WAYS,
  against Pillow and Python's `zlib`; the BMP and GIF decoders over
  files Pillow and **ImageMagick** wrote (it needs `magick`).

  **BMP and GIF:** every header version (OS/2 1.x and 2.x, Windows 3,
  V2-V5), 1/4/8/16/24/32 bits, RLE8, bit masks; for GIF interlacing,
  local colour tables, frames at offsets and all three disposals, every
  frame of an animation compared through the codec's iterator, delays
  and the loop count included. What neither tool writes -- RLE4, a V4
  header, an RLE delta, top-down -- is built by hand with its expected
  pixels beside it. **The oracle is chosen per file**: Pillow where it
  reads the variant; for ARGB4444/1555 a reading of the masks written in
  the script, because Pillow refuses 4444 and drops 1555's alpha bit and
  ImageMagick widens 4 bits to a white of 240. 565/555 allow 1 against
  Pillow, which rounds a 5-bit channel down where the decoder repeats
  its bits. GIF frames compare transparent pixels as equal whatever
  their colour (Pillow keeps the palette entry's). Disposal is not under
  the positive control, but breaking disposal 2 or 3 was seen to redden
  the animation checks.

  **Encoding:** `uimg_qoi.c` and `uimg_png.c` compiled with the host gcc,
  run over eight images chosen for what they do to each format (a flat
  fill for QOI's runs, a gradient for its luma chunks, noise that no
  chunk helps, a UI-shaped image, one with alpha, a 1x1 and a 1x300),
  and every file opened again by **Pillow** -- with each PNG also
  inflated by Python's `zlib` and unfiltered by hand, so three
  implementations that share no code with ours read what we wrote.

  **A foreign decoder is the whole point.** An encoder tested against
  this repo's own decoder passes whenever the two share a mistake, and
  the two mistakes an image encoder actually makes are exactly that
  shape: a QOI index table updated on the wrong chunk, or a Huffman code
  packed least-significant-bit-first. Both produce a file that
  round-trips perfectly here and that nothing else can open.

  **Decoding:** the same argument in reverse -- files PILLOW wrote,
  read by us, compared exactly (PNG is lossless, so there is no
  tolerance to hide behind). Every colour type including palette, each
  row filter forced in turn, and sizes whose stride is not round. A
  corrupted chunk must be `-EINVAL`, never a picture.

  **Every depth and Adam7 come from a writer IN THE SCRIPT**
  (`write_png_any`), because Pillow writes neither Adam7 nor 16-bit
  truecolour -- and silently ignores `interlace=1`, which once made a
  check pass on a file that was never interlaced. Each colour type at
  each legal depth, plain and Adam7, at 13x11, 3x2 and 1x1 (the last two
  have EMPTY passes), rows cycling all five filters, with tRNS on
  palette, grey and truecolour. The expected pixels are the spec's rules
  applied to the samples written; Pillow cross-checks the WRITER on every
  file it reads faithfully -- not 16-bit grey, which it clips, nor a
  grey or truecolour tRNS key off 8 bits, which it compares after
  converting the samples and so never matches. Shown red by unpacking
  sub-byte samples least-significant first and by moving one Adam7 pass.

  `--positive-control` carries TWO sabotages in two files -- an
  unreversed Huffman code in the compressor, and the Paeth predictor
  dropped from unfiltering -- and requires encode AND decode to go red
  while QOI stays green. One sabotage reddened only half the harness,
  which meant the other half was untested. `--keep DIR` leaves the
  files to look at. Not in any gate: it needs Pillow, and
  `/tests/uimg_test` is the round-trip that runs in the guest.
- **`uvid_hostcheck.py`** -- `userland/libs/media/uvid*.c` (MPEG-1 video, the
  MPEG program stream) and `usnd_mp2.c` (Layer II), compiled with the
  host gcc and compared with FFmpeg's decode: ten encodes covering the
  quantiser ends, I-only, no-B and 60-frame GOPs, big motion, custom
  matrices, a size that is not a multiple of 16, skipped macroblocks and
  the rate-distortion search; seven MP2 encodes across rates and modes;
  then one muxed file's sound, pictures and four exact seeks. **Video is
  judged by PSNR, not equality** -- MPEG-1 defines a decoder by IDCT
  accuracy, and no frame matches FFmpeg bit for bit -- **and the bar was
  set by `--positive-control`, not by eye**: at 40 dB a half-pel
  rounding bias passed every case; the clean decoder's worst frame is
  ~60 dB and the biased one's ~49, so the default is 55. Also runs the
  decoder's own table self-test (every VLC table a prefix code). In
  `ondemand_sweep.py`; needs gcc and ffmpeg.
- **`indeo3_hostcheck.py`** -- `userland/libs/media/uvid_indeo3.c`
  (Indeo Video 3), with uvid's AVI demuxer before it and the 4:1:0 draw
  path after it, compiled with the host gcc. **Bit-exact, not PSNR**:
  Indeo 3 has no transform, so a correct decoder matches FFmpeg sample
  for sample, and every frame is compared by its timestamp against
  `ffmpeg -fps_mode passthrough -f framemd5` (a mismatch names the first
  differing sample); a null frame, which FFmpeg skips, must repeat the
  previous picture. Seeks are compared the same way three times over:
  with the file's own index; with `idx1` renamed `JUNK`, so the reader
  walks `movi` and asks the codec which frames are keyframes; and, for
  key seeks, with an `idx1` that flags no keyframe, which must land where
  the flagged index does rather than back at the first frame.
  **THE FILES STAY LOCAL** -- the IV32 AVIs of DK's
  "Multimedia Science" CD, read with `bsdtar` from `--iso` (default
  `$SAUNAOS_INDEO3_ISO`, else `~/CodingProjects/saunaos-other/DKMMSCIE.iso`)
  or `--dir`; without them that half SAYS it is skipped. Always: a
  synthesised stream (every escape, modes 0 and 10, intra and inter, a
  NULL copy, a motion vector) must decode whole, and decode to the same
  pictures again with every ALT QUANT pair moved (modes 0 and 10 take a
  cell's own table; the pairs are modes 1 and 4's), and truncated and
  corrupted frames after a valid prefix go through the codec under ASan
  and UBSan. Also `uvid_frame_draw()` against FFmpeg's RGB by PSNR (a
  measured 40 dB; 4:1:0 read as 4:2:0 is 10). Prints the decode speed.
  `--positive-control` changes the intra requantisation's rounding (the
  comparison must go red) and drops a dyad index check (the fuzz must go
  red). About a minute for all 42 files; `--files N` for fewer. Not in
  the gate (the files are local); needs gcc and ffmpeg.
- **`usnd_hostcheck.py`** -- the same `userland/libs/media/usnd_mp3.c`, compiled
  with the host gcc and run against eight lame-encoded files (CBR and
  VBR, mono/stereo/joint stereo, 32 to 320 kbps), every sample compared
  against ffmpeg's decode. Same split as `uimg_hostcheck.py` and for the
  same reason: a guest test carries the vectors somebody committed, and
  a decoder's bugs live in the combinations. **The source it encodes
  carries deliberate clicks**, because a transient is what makes an
  encoder emit SHORT BLOCKS -- three IMDCTs, subblock gains and the
  reorder, which a tone-only fixture leaves entirely untested.

  Two things it taught, both about the harness rather than the decoder.
  **The tolerance was 10 000x too loose**: it was set to 0.02 by eye,
  and `--positive-control` then showed a deliberately broken decoder
  passing two of eight checks under it. Measured agreement is ~1.6e-6
  (one LSB in 32768, which is all ISO/IEC 11172-4 requires -- a
  compliant decoder is defined by error bound, not bit-equality), so the
  default is 1e-3. And **the lag is not always a whole frame**: a file
  with a LAME/Xing tag makes ffmpeg drop the encoder delay, 1105 samples
  rather than a multiple of 1152, so a frame-stepped alignment reported
  the shipped song as badly broken. It searches every sample now.

  `--positive-control` builds the decoder with two Huffman tables
  swapped and REQUIRES every check to go red -- shaped to defeat the
  structural check on purpose, since swapping two codewords of equal
  length leaves the code complete and prefix-free. The MP3 comparison is
  in s32 against ffmpeg's `s32le` since the sound path widened, and
  agrees to ~1.3e-7.

  **And `usnd_flac.c`, EXACTLY**: a lossless decoder has no error bound
  to hide in, so every sample must equal Xiph's `flac -d` after the s32
  output is shifted back to the file's depth. Fourteen encodes shaped by
  what the format can do -- 8, 12, 16, 20, 24 and 32 bits (12 and 20
  through WAVE_FORMAT_EXTENSIBLE, the only way `flac` takes them),
  levels 0 to 8 with exhaustive search, blocks of 192 and 32768, ffmpeg's
  encoder as well as Xiph's, wasted bits, a silence and a noise burst --
  and `flac -a` was used to confirm the fixtures really hold CONSTANT,
  VERBATIM, FIXED and LPC subframes and all four channel assignments.
  Seeks are checked against the straight decode, with and without a seek
  table. Its positive control drops the Rice zig-zag and every FLAC
  check must go red. Not in any gate: it needs `lame`, `ffmpeg` and
  `flac`.
- **`midi_hostcheck.py`** -- the MIDI codec, the SoundFont parser and
  the synth (`userland/libs/media/usnd_mid.c`, `usnd_sf2.c`, `usnd_synth.c`)
  compiled with the host gcc, playing `first-boot.mid` and a GM tour
  through a bank and compared against **FluidSynth**, which shares no
  code with them. **It compares SHAPE, not samples**: two correct
  SoundFont synths never agree sample for sample, so it checks the
  loudness envelope (20 ms windows, correlated), that the level ratio
  per section is CONSTANT (a wrong attenuation or velocity curve makes
  it wander), and each tour instrument's pitch (autocorrelation, half a
  semitone) and brightness. Each tour segment starts with All Sound
  Off, so a window holds one instrument rather than a long pad's tail
  under the next attack. Then it FUZZES both parsers under ASan/UBSan
  -- mutated songs, and banks mutated where the indices are.

  **Run it with `--sf2` on a real bank too**, not only the built-in one:
  the shipped bank uses no modulators, so every modulator bug is
  invisible to it. Against GeneralUser GS it found the missing
  modulator lists, the 0.4 dB attenuation unit and the convex
  modulation-envelope attack, each confirmed by the fix and then by
  breaking it again. Positive controls that turned it red: a root key
  off by a semitone, the attenuation generator ignored, note-offs
  ignored, velocity-to-filter-envelope modulators dropped.
  `--render SONG OUT.wav` just renders, for listening. Needs `gcc`;
  the oracle half needs `fluidsynth` (skipped, loudly, without it).
  Run by `ondemand_sweep.py`.
- **`ucolour_hostcheck.py`** -- `lib/ucolour`'s HSV and HSL conversions
  against Python's `colorsys`, over a 17-step RGB grid and random
  colours: to_hsv within 0.2 degrees and 0.002, keeping the caller's hue
  for a grey and saturation for black; from_hsv to the byte and back to
  the same colour exactly; HSL within a percent. Its positive control --
  a hue sector from the wrong channels -- fails thousands of colours. In
  `ondemand_sweep.py`.
- **`winfnt_hostcheck.py FOLDER|FILE...`** -- `lib/winfnt`, the reader of
  Windows' bitmap fonts (.FON/.FNT) that w16 draws a program's text with,
  against FreeType's own winfnt driver compiled on the host: every raster
  font in every file given, every character 32..255, advance and bitmap
  bit for bit. The fonts are the maintainer's own Windows 3.11 ones, never
  in this repo, so it takes their folder as an argument and is in
  `ondemand_sweep.py`'s exclusions for that reason. Needs FreeType's
  headers (`pkg-config freetype2`). Its positive control -- a glyph's
  bits read right to left -- fails nearly every glyph.
- **`upixel_hostcheck.py`** -- `lib/upixel` and `lib/upixundo` compiled
  on the host against a reference in Python: every brush size and both
  shapes stamped and stroked off every edge (the disc computed in floats,
  not the C's doubled integers), seeded random fills at tolerance 0 and
  above it (a breadth-first search decides the region), every dirty rect
  covering what changed, and histories of strokes, fills and a
  size-changing step that undo to the first picture and redo to the
  last, with a small byte limit dropping the oldest steps; ONE history
  over two pictures and custom steps undoing step by step to each state
  before; and source-over (`upix_over`, `upix_over_at`) within an
  alpha-scaled tolerance of a floating-point Porter-Duff; brush strokes
  (`upix_stroke_*`) run forward and back over themselves against a float
  reference, for over, multiply and erase at any hardness, the chisel
  tip's exact cover, and the spray staying in its disc and repeating
  for one seed; two or three overlapping contours, some wound backwards,
  filled as one shape (`upix_mask_fill_rule()`) against the winding
  number under NONZERO and EVEN-ODD -- shown red by a nonzero span that
  ends where the winding goes from 1 to 2. Its positive control -- a fill that never seeds the row
  below, a source-over that forgets to scale the picture below, a
  stroke blending onto what it already painted, and a Subtract that adds
  -- reddens the fills, the composites, the strokes and the selections.
  Selections: rectangles, wand picks and lassos with New/Add/Subtract
  against a set reference, invert, the cut-out copy, Delete and
  keep-outside. Gradients: every shape's position against floats, the
  fill within a step and inside its mask, dithering unbiased. In
  `ondemand_sweep.py`.
- **`ora_hostcheck.py`** -- `lib/uora` (OpenRaster, Paint's layered
  save) and `lib/uzip` compiled on the HOST against Python's zipfile,
  ElementTree and Pillow, BOTH ways: what the C writer makes must have a
  STORED `mimetype` first, a `stack.xml` that parses with the layers top
  first, their names (XML specials and Latin-1), opacities and
  visibility, each layer's PNG pixel-exact, `mergedimage.png` and a
  thumbnail within 256; a Krita-shaped file from Python -- a CROPPED
  layer at an offset, a nested group, entities -- must read bottom first
  on the full canvas; a wrong mimetype and a missing PNG are refused.
  `--positive-control` leaves the stack top first and `&` unescaped.
  Needs Pillow. In `ondemand_sweep.py`.
- **`archive_hostcheck.py`** -- `lib/uzip` and `lib/utar` compiled on
  the HOST (shims for `sys_stat` and the calendar) against Python's
  zipfile, tarfile and gzip, BOTH ways: what the C writers make at every
  level (and `.tar`, `.tar.gz`, with a ustar-prefixed name and one only a
  GNU long-name member holds) must read back byte for byte, `testzip()`
  clean, and the levels must actually differ on real prose (store >
  fast > normal >= best); what Python writes (zip stored and deflated,
  GNU and pax tar, `.tar.gz`, a lone `.gz`) must list at its sizes and
  extract byte for byte; a wrong header checksum must be refused.
  `--positive-control` breaks the tar checksum and the zip CRC: exactly
  the six writer checks redden. In `ondemand_sweep.py`.
- **`filemanager_harness_hostcheck.py`** -- `filemanager_test.py`'s own
  wait and toolbar logic, on the host against a scripted console and a
  click-recording QMP stand-in: a toolbar item the app did not report
  records a failed check and clicks nothing (it once raised `KeyError`
  out of the suite); a report split across two serial sweeps parses
  whole; a wait whose predicate never holds returns None rather than
  the layout it rejected; a state the app already reported answers
  a later wait after a grace period, since the app dedupes its whole
  report block and repeats nothing; that cached answer is OFF once a
  newer report has begun, so a frame whose tail has not arrived cannot
  let the previous one answer for a state the app has left; the same
  frame STAYS ineligible across the waits that follow, since a wait
  timing out changes nothing about the app; and a respawned window
  inherits none of the old one's geometry. Seconds, no guest.
- **`exfat_hostcheck.py`** -- SaunaOS's exFAT judged by the HOST: a
  stick formatted by `mkfs.exfat` and one the guest formats itself
  (`mkfs -t exfat`), each written by the guest (a directory, a file several
  clusters long, a non-ASCII name, a rename, a delete, a truncate), then
  `fsck.exfat -n` on each image and a reader written in Python from the
  specification -- no code shared with the kernel -- that must find the
  same files byte for byte, and every name's hash right for the volume's
  own up-case table. Controls that turned it red: a hash without
  up-casing and a set checksum one byte short (fsck named both). Needs
  exfatprogs. automountd is stopped once it has mounted the host-made
  stick, so the test's own mount is the only one writing to it. In
  `ondemand_sweep.py`.
- **`settings_harness_hostcheck.py`** -- `settings_test.py`'s own
  geometry and waits, on the host against a scripted console: a sidebar
  row is aimed at where it IS rather than where it would be unscrolled
  (the row dump is taken once, at the top, and the last rows are
  reported below the WINDOW, where a click hits the taskbar and
  minimizes the app); the scroll offset is read from the newest report
  and waited for, since it is logged only on a change; a page is
  confirmed open by its own report before anything reads its controls;
  and a control counts as reachable only when its WHOLE rect is inside
  the scroll view, because the clip is at the viewport edge and a
  straddling control is half routable. Seconds, no guest.
- **`regex_hostcheck.py`** -- compiles `userland/tests/regex_cases.h`
  twice, once against the real `userland/libs/libc/regex.c` and once against
  the host's glibc `<regex.h>`, and compares.

  **It catches what a self-test structurally cannot: an expected value
  that is simply wrong.** `/tests/regex_test` asserts that the engine
  agrees with spans written by the same person at the same time; glibc
  shares no code, no author and no assumptions. On the first run it
  confirmed 71 of 74 cases and isolated exactly three deliberate
  divergences (`\t` as a tab, a stricter unmatched `)`, no
  back-references), each of which is listed with its reason in
  `KNOWN_DIVERGENCES` -- an unexplained one fails the run. Same idea as
  `uimg_hostcheck.py` against libjpeg.

  ON DEMAND: it needs a host gcc and glibc, and the gate must not start
  requiring either.
- **`libc_fmt_hostcheck.py`** -- tolibc's `printf` float and integer
  conversions, `sscanf` and `strtod`/`strtof` against glibc's, byte for
  byte and bit for bit: `kernel/lib/kfmt.c`, `userland/libs/libc/printf_float.c`,
  `scanf.c` and `strtod.c` built with the host gcc beside a driver that
  makes each call twice. Compared: the output and return value, the `%n`
  count, the parsed value's bits, `endptr` and `errno`. A fixed table
  (every flag/width/precision of `f F e E g G a A` over edge values, in
  double and `L` long double; `t`/`j`/`z`/`%n`; strtod/strtof boundary
  strings; scansets), then a seeded random sample (`--random N`,
  default 400,000 doubles: a third as many long doubles, half as many
  parse inputs) that includes the EXACT midpoints between adjacent
  doubles and floats -- the inputs a correctly rounded parser has to
  break as ties. ~2.2 million comparisons, about a minute.

  **It was red before the fix: 1,129,399 of 2,218,472 disagreed**
  (2026-11-10, the old scale-by-ten digits and multiply-and-add
  strtod). `--positive-control` breaks the tie rule in printf AND in
  strtod (both of strtod's stepping loops: either alone is undone by
  the other) and requires both to go red.

  **Two traps in building it.** Compiled against glibc's headers,
  tolibc's `sscanf` definition takes glibc's asm label
  (`__isoc99_sscanf`) and would REPLACE glibc's in the driver -- so the
  objects are renamed after compiling (`objcopy --redefine-sym`, both
  spellings), not with `-D`. And one difference is glibc's, counted
  apart rather than failed: `%#g` whose rounding carries into a new
  power of ten prints `1.e+02` where C says `1.0e+02`
  (`docs/libc-design.md`). Needs gcc, objcopy and nm.
- **`hid_parse_hostcheck.py`** -- `kernel/lib/hid_parse.c` against HID
  report descriptors CAPTURED off real devices, on the host. Run by
  `ondemand_sweep.py`; needs gcc and nothing else.

  **The fixtures are captured, not invented**: a Logitech G305
  receiver's mouse and keyboard interfaces (read with
  GET_DESCRIPTOR(0x22) at bind time) and QEMU's usb-mouse and usb-kbd.
  A descriptor written by whoever wrote the parser proves only that the
  two agree with each other.

  **Why the host rather than a guest.** The parser decides where a
  device's buttons and axes sit inside its reports, and its failure is
  not a missing feature but a pointer flying across somebody's screen,
  with a person as the only oracle. The descriptors are small and
  fixed, so the whole question can be asked on the host -- which is
  also the only way to cover a device nobody here can plug in.

  It asserts the derived layout field by field (offsets written out by
  hand from the decoded descriptor), that a keyboard's keycodes are an
  ARRAY rather than a bitmap, a round trip through a synthesised report
  including a NEGATIVE axis delta, and that every malformed input --
  truncated, empty, garbage, an impossible report size -- is REFUSED
  rather than turned into a plausible-looking layout. The refusals
  matter most: a refusal sends the driver back to the boot protocol,
  and a plausible-looking layout is the one that gets used.
- **`uregion_hostcheck.py`** -- compiles `userland/libs/base/uregion.c`, the
  rect-list region the compositor clips each window to once the opaque
  windows above it are subtracted, with the host gcc, and replays every
  operation on a Python pixel set: the rects must be disjoint, EXACTLY
  the bitmap when nothing overflowed, and a SUPERSET of it when the
  32-rect bound was hit (a clip may err large, never small). Seeded
  random sequences plus the shape the compositor actually cuts -- a
  stack of equal windows with rounded-corner holes. Also `uregion_diff()`,
  which uapp measures a present's damage with: every changed pixel
  covered, at most `max` disjoint bands, none for equal buffers, tight
  when nothing was folded. `--positive-control subtract` drops the band
  below a cut, `--positive-control diff` stops a band widening right;
  each must fail. Needs only gcc.
- **`urgn_hostcheck.py`** -- compiles `userland/libs/base/urgn.c`, the growable
  region w16's GDI regions and DC clips are, under ASan and UBSan, and
  replays seeded random set-rects, adds, cuts, unions, intersections,
  subtractions, xors and offsets on four registers (an operand may be the
  destination) on Python pixel sets: the rects must be disjoint and
  EXACTLY the bitmap -- no cap, unlike `uregion` -- with bbox, area,
  containment, overlap and equality agreeing; forty one-row spans must
  coalesce to one rect, a five-step staircase to five. `--positive-control
  cut` drops the strip below a hole, `--positive-control coalesce` merges
  rects that share only part of an edge; each must fail. Needs only gcc.
  In `ondemand_sweep.py` (`urgn_host`).
- **`utween_hostcheck.py`** -- compiles `userland/libs/base/utween.c`, the
  desktop's one easing tween, with the host gcc and checks the
  PROPERTIES a motion must have rather than an oracle (there is no
  reference easing library, and the curve's exact shape is a taste): it
  starts at `from` and lands EXACTLY on `to`, never moves back, covers
  more than half the distance in the first half of the time (ease-out),
  goes inactive exactly at the end, and a retarget mid-flight continues
  from the value at that instant without a jump. Possible on the host
  because the tween takes its clock as an argument. Its first run caught
  a rounding overshoot (100 -> 0 visited -1 for a frame) before any
  guest had drawn with it. `--positive-control` compiles a LINEAR curve
  and must fail on the ease-out check. Needs only gcc.
- **`tree_lock_hostcheck.py`** -- the test lock's PreToolUse hook refuses
  every way a source file changes while a test reads the tree, and
  nothing else. Holds the lock, feeds `tree_lock.py hook` 19 tool calls
  -- the Edit tool, `sed -i`, `perl -pi`, a redirection, `tee`, a heredoc
  `open()`, `make`, `git stash` refused; reading, scratch files, `docs/`,
  `tools/`, `build/`, `2>&1` allowed -- and compares the verdicts. Exists
  because a `sed -i` went through during a preflight on 2026-11-04: the
  hook judged only Edit/Write and `make`/`git`. `--positive-control`
  asks without holding the lock, so the refusals come back allowed and
  the run fails. In the gate.
- **`caret_hostcheck.py`** -- compiles `userland/libs/ui/uui_caret.c`, the
  one blink every text caret shares, and drives it the way the
  compositor's loop does: park until the caret's deadline or the next
  clock tick, paint the clock's frame (which asks `uui_caret_visible()`
  with the clip elsewhere, so the field's pixels do not change), then
  ask the caret. With the clock at every 5 ms of its second, on time
  and waking 30 ms late, it asserts what the field SHOWS: on in even
  half-periods, off in odd ones, for the whole blink; solid at the end;
  one repaint asked per phase; and a field that loses focus stops the
  asking within a phase. `--positive-control` restores the logic that
  took any `visible()` call for the phase's repaint -- frozen at 26 of
  200 alignments when 30 ms late -- and must fail. Needs only gcc.
- **`ucrt_hostcheck.py`** -- compiles `userland/libs/ui/ucrt.c`, the CRT
  effect, with the host gcc and the tree's `-ffreestanding`, and checks
  every pass against a plain integer model of it written in Python: the
  2x2 average, the box blur, the glow, the screen blend, the mask, the
  row x column gain and the bilinear warp, within 1 per channel (the
  screen's 255 + glow rounding to 256 is clamped before the gain, not
  after). The curve's geometry is read from the code's own map, so this
  checks the sampling, not the float maths that places it --
  `crt_test.py` clicks through the curve for that. Odd sizes are in the
  set because the glow pads its half image to even ones.
  `--positive-control` drops the padding row's copy between blur passes
  and must fail; `--bench` prints ms per preset at 640x480 and 1440x1080.
  **`--against REV` is the check for a speed-up that must change no
  pixel**: the same cases plus sizes up to 1440x1080, through this
  tree's `ucrt.c` and REV's (`git show`), compared byte for byte, map
  included. The model's +-1 tolerance cannot see a one-unit slip; this
  can. On the machine itself, `/tests/ucrt_bench [W H [FRAMES]]` times
  each preset per pass (glow, compose, warp) into a plain buffer, and
  DOOM's FPS line carries the same split.
  (`/tests/uglyph_bench [PASSES]` is its sibling for `ui/uglyph.h`: a
  page of text in three faces at three sizes, first pass vs a repaint --
  a repaint as slow as the first pass is a cache that cannot hold the
  page.)
  **BENCH WITH THE TREE'S FLAGS**: `-ffreestanding` implies
  `-fno-builtin`, so a small `memcpy()` that the host inlines is a call
  in the tree -- a vector load written that way made the effect twice as
  SLOW on the ASUS while a plain host build showed it 3x faster.
- **`usolid_hostcheck.py`** -- `usolid_slice()`, the Desktop cube's
  damage, on the cube, the pyramid and the ball, with the host gcc. After
  four cuts each solid must be closed and wound one way. Every directed
  edge needs its reverse, matched by position, which a missing cap, a cap
  wound backwards, or faces meeting a rounding apart would all break.
  Each cut must sit at its depth as it is made, and the cap must show the
  interior rows. `--positive-control` breaks each of those in turn and
  requires the finding aimed at it. In `ondemand_sweep.py`.
- **`teapot_hostcheck.py`** -- compiles Shapes' teapot mesh
  (`userland/apps/demos/gfxdemo/teapot.c`) and `ugfx_tri3d()` (`userland/libs/ui/ugfx_tex.c`)
  with the host gcc. The mesh is judged against a FLOAT evaluation of
  the control points parsed out of teapot.c itself (every vertex within
  0.002 units), and its normals by where the teapot's SHAPE says they
  must face -- body away from the axis, lid top up, bottom down --
  rather than by a second copy of the same cross product. It also holds
  teapot.h's winding claim (a face toward the eye has a positive
  screen-space area, the sign Shapes culls by). The rasteriser: two
  PIERCING triangles give the same pixels in either order through a
  depth buffer (bar the crossing line, where depths tie) and different
  ones without it -- which is the control that the scene overlaps at
  all -- and a Gouraud triangle carries its corners' shades.
  `--positive-control` breaks the mirror's winding fix-up, teapot.h's
  triangle winding and the depth test ONE AT A TIME, and requires the
  finding aimed at each -- the winding check reads the triangles through
  `teapot_cell_tris()` itself, so it tests the header rather than a copy
  of it. Needs only gcc.
- **`des_hostcheck.py`** -- `userland/libs/base/udes.c` against `openssl enc
  -des-ecb` (OpenSSL 3 keeps DES in its legacy provider, which the tool
  loads), 300 random key/block pairs plus FIPS 81's worked example, each
  also decrypted back. DES is here for VNC's password check only, where
  a wrong cipher is indistinguishable from a wrong password.
  `--positive-control` breaks the key schedule's shift table in a copy
  -- a fault a round trip alone would pass, since a wrong schedule is
  still a permutation.
- **`heap_hostcheck.py`** -- builds `kernel/lib/heap_core.c` (malloc for
  both rings) with the host gcc and fuzzes it: millions of seeded
  `kmalloc`/`krealloc`/`kfree` steps over a few thousand live,
  pattern-filled blocks, plain and with `heap debug` on, checking every
  byte, alignment, `kmalloc_size()`, and `heap_validate()` (the
  size-class free lists against the block list) every 20 000 steps.
  Then ring 3's slab front end (`userland/libs/libc/malloc.c`) over the same
  heap: that fuzz through `malloc`/`calloc`/`realloc`/`free`, and four
  threads freeing each other's blocks. `--netsurf PAGE` preloads the same allocator into the host's NetSurf
  and times a layout against glibc's -- the measurement that found the
  old first-fit walk quadratic. Every kernel-heap fuzz runs again with
  `HEAP_RETURNS=1`, the kernel's side, where a region a free empties goes
  back (`munmap`, so a region given back while still in use crashes), and
  after freeing everything at most the kept cushion may still be mapped.
  `--positive-control` builds two broken copies -- the first bins bug (a
  merge unlinking a block never listed) and a region given back with a
  live block in it -- and passes only if each is caught. In
  `ondemand_sweep.py` as `heap_host`.
- **`hash_hostcheck.py`** -- compiles `/lib/libhash.so`'s two algorithms
  (`userland/dynlib/uhash.c` plus `kernel/lib/kcrc.c`) with the host gcc
  and judges them against Python's `hashlib` and `zlib`. ~2,150 vectors:
  every size from 0 to 129, the powers-of-two boundaries, and forty
  random sizes, each fed through in seven different chunk sizes so a
  block boundary lands INSIDE an `update()` call rather than between
  two.

  **The oracle is in the standard library**, so unlike the other
  hostchecks here this needs nothing but gcc -- no Pillow, no ffmpeg.

  `--positive-control` edits a COPY of each source (one wrong rotate in
  the SHA-256 round, one wrong polynomial bit in the CRC) and requires
  the sweep to go red; the originals are never touched, which is what
  keeps the control out of shipped code. Measured: 2,137 of 2,150
  vectors disagree under it.

  ON DEMAND: it needs a host gcc, and the gate must not start requiring
  one.
- **`divti3_hostcheck.py`** -- compiles `userland/libs/libc/divti3.c` with
  the host gcc and judges the 128-bit division helpers against Python's
  arbitrary-precision integers. 17,022 vectors: every pairing of
  sixteen edge values (the 32-, 64- and 127-bit boundaries, powers of
  two, all-ones) and then 4,000 random pairs whose two widths are drawn
  INDEPENDENTLY, so `a/b` spans "both small", "both large" and "one of
  each" -- which is what decides whether the 64-bit fast path or the
  shift-subtract loop runs.

  **The oracle is arbitrary precision**, so it is the answer by
  construction rather than by a second implementation agreeing. Needs
  nothing but gcc.

  **One case is excluded and the exclusion is the interesting part.**
  `INT128_MIN / -1` has no representable quotient, so C leaves it
  undefined; Python's mathematically-correct `+2^127` would fail a
  correct implementation. libgcc returns `INT128_MIN` there and so does
  `divti3.c` -- confirmed against the host's libgcc rather than assumed.
  Divide-by-zero is not covered either: it raises #DE by design, which
  a process expecting to keep running cannot probe.

  `--positive-control` breaks the 64-bit fast path in a COPY and
  requires the sweep to go red. It is deliberately the FAST PATH rather
  than the loop, so the control also proves the sweep reaches small
  operands at all -- a sweep of large random values alone would never
  enter that branch. Measured: 2,332 of 17,022 disagree under it.

  ON DEMAND: it needs a host gcc, and the gate must not start requiring
  one.
- **`browser_hostcheck.py`** -- NetSurf's layout on SaunaOS against the
  SAME pinned source built on the host (`docs/browser-design.md`,
  stage 1). Every page in `data/tests/web/` (`/tests/web` on the image)
  is opened and redrawn by the headless `monkey` front end on both
  sides, and the title, the laid-out extent and every PLOT line of the
  redraw must be identical. monkey measures text at a fixed width per
  character, so no font enters it: a difference is the PORT's.
  On demand (`ondemand_sweep.py`'s `browser_host`), and a skip without
  the host build. **Status lines are not compared** -- their count is
  timing -- and the guest is PACED with sleeps in one dash script
  (`--load-s`, `--redraw-s`), because the debug console cannot feed a
  running program's stdin; a page that did not finish is reported as
  "not loaded within N s", never as a layout difference. Shown red by
  breaking Windows-1252's euro in libparserutils' table. The host side
  runs under `LC_ALL=C`: an inherited comma-decimal `LC_NUMERIC` writes
  a path's "WIDTH 1,000000", which `svg.html` was the first page to
  print. `nsmonkey` names a local file's type with `ufileurl_mime()`, the
  table `/bin/webcontent` uses -- SaunaOS has no `/etc/mime.types`, and
  monkey's built-in fallback knows no `.svg`.

  **`--js`** runs both engines with NetSurf's `--enable_javascript=1`
  (each monkey is off by default, as upstream's is): `script.html` is the
  page whose drawing depends on it, so a difference there is the Duktape
  port's -- tolibc's maths and clock, the `-nostdinc` build
  (`ondemand_sweep.py`'s `browser_host_js`). The flag goes BEFORE the
  guest's `--weak-entropy`: NetSurf's option parser takes the word after
  a bare `--flag` as its value. `fileref.html` is skipped in both modes:
  it names the guest's own `file:///tests/web/bands.png`.

  **`--scheme http` or `https`** has the GUEST read the same pages from
  `web_server.py` (https with a test CA put in its `/etc/ssl/certs`)
  while the host still reads files: layout does not depend on how the
  bytes arrived, so a difference is the fetcher's. `net.html` exists for
  this mode -- every older page arrives in ONE chunk with no separate
  resource, and dropping a byte per chunk turned none of them red;
  dropping every other chunk turns `net.html`'s extent red. https gets
  12 s to load by default (three handshakes under TCG), and a page whose
  `Done` came only DURING the redraw counts as not loaded -- it used to
  read as a two-line plot difference. `HOST_LACKS` names the pages the
  host cannot judge (`photos.html`: it is built without libpng and
  libjpeg), each with its reason.
- **`components_hostcheck.py`** -- the optional components' build half
  against fixture trees it makes: the shipped definitions pass; each
  fault the check exists for is reported (a missing path, a path in two
  components, a bad `Needs=`, a hand-written `Size=`, a GPL-built
  program in an MIT component, half a `Built=if-present` one); the
  staged `Size=`/`Files=` count a carved-out file (`/tests/nsmonkey`) to
  its owner; and `update_server.py`'s manifest tags each line with its
  `component=`, none in the base or `/etc`. `--positive-control` leaves
  each fault out and must fail. Python only; a `gate` check.
- **`umd_hostcheck.py`** -- compiles `userland/libs/base/umd.c` with the host
  gcc and renders every `docs/commands/*.md` page through it at 40, 80
  and 132 columns, plus the three field lookups (`umd_title`,
  `umd_field`, `umd_section_para`). No guest, no Pillow, no oracle
  beyond the pages themselves.

  **What it asserts is invariants, not an expected rendering.** A
  golden file for 109 pages would have to be regenerated every time
  anyone edits prose, and nobody would read the diff. Instead: the
  output is pure ASCII; every line fits the requested width unless it
  was COPIED verbatim rather than wrapped (a code block, or one word
  longer than the screen -- neither of which a wrapper may break);
  `**` does not survive into wrapped prose; the Synopsis block --
  the text `check_docs.py` already pins to the program's `cmd_usage()`
  -- comes out intact; and the lead paragraph of every Description has
  BALANCED backticks.

  **Comparison is on letters and digits only.** The renderer changes
  punctuation on purpose (backticks and asterisks go, an em dash
  becomes `--`), so comparing raw text would report every one of those
  as a wrapping failure.

  It found five real bugs the day it was written -- a paragraph's line
  break not being fed as whitespace, so the last word of one line and
  the first of the next were joined; unwrapped headings; a table of
  empty headers read as a rule row, which made the first DATA row the
  header for every row after it; a code block re-opened per line; and a
  cell splitting at a `\|` inside its own code span. It also found the
  SIX PAGES whose Description opened with a fragment of a table cell,
  unclosed backtick and all, since the day they were written.

  `--positive-control` renders every page with the wrap disabled and
  requires the width check to fail. Measured: 667 violations.

  ON DEMAND: it needs a host gcc, and the gate must not start requiring
  one.
- **`term_scheme_hostcheck.py`** -- reads every
  `data/usr/share/terminal/*.scheme` on the HOST and reports what each
  colour will actually be once the Terminal has loaded it. No guest, no
  compiler, a fraction of a second.

  **What it is for is the PERMUTATION.** A scheme file is written in
  ANSI order -- black, red, green, yellow, blue, magenta, cyan, white,
  then the eight bright forms -- which is the order every published
  palette is published in, so one can be copied in without being
  rearranged by hand. A cell in the emulator holds an `enum vga_color`,
  where blue is 1 and red is 4. `userland/apps/apps/terminal/term_conf.c` permutes
  between them through the kernel parser's own `ansi_color()`.

  That step is the part nobody can check by looking, and it has already
  been wrong once: `Foreground=`/`Background=` name a `Color<N>` like
  everything else in the file, and taking one as a VGA slot put
  Solarized Dark's foreground on light red. It rendered, it looked like
  a colour scheme, and it was not the one in the file.

  So the ANSI table is written out here a SECOND TIME rather than parsed
  out of `ansi.c` -- an oracle that read the table it is checking would
  agree with it by construction. If `ansi.c`'s table changes this must
  fail and be corrected by hand; that is the point of the copy. It also
  refuses a file with a missing or malformed key, an index outside
  0..15, or a default pair whose two colours are equal (which parses
  perfectly and draws invisible text).

  `--positive-control` additionally fails any scheme the permutation
  actually MOVES, and reports how many -- two, the Solarized pair, being
  the only shipped schemes whose default pair is not already at 0 and 7.
  A control that reddens nothing has measured nothing, so it exits
  non-zero when none moved. Named by `ondemand_sweep.py`.
- **`ugfx_fill_hostcheck.py`** -- compiles the real
  `userland/libs/ui/ugfx_fill.c` with the host gcc, records the alpha each
  pixel is given through a stub `ugfx_blend_pixel()`, and compares the
  coverage of circles, an ellipse and polygons against Pillow drawing
  the same shapes at 64x and box-reducing -- the convention both sides
  share is ugfx.h's (an integer point is a pixel's centre). Asserts the
  footprint (inside full, outside empty), a GRADED edge, and per-pixel
  and mean error within tolerances set from measurement. **The oracle's
  resolution matters**: at 16x, Pillow's fills including their boundary
  grew every shape by half a sample, a systematic +8 per edge pixel that
  looked like this file's error. `--positive-control` rounds the alphas
  to 0/255 (an aliased fill) and must go red. In `ondemand_sweep.py`.
- **`ugfx_pix_hostcheck.py`** -- compiles `userland/libs/ui/ugfx_pix.c` (the
  C reference), `ugfx_pix_sse2.c` and `ugfx_blur.c` with the host gcc
  and requires every SIMD op to produce the SAME BYTES as C: mix and
  blend exhaustively over every channel triple, the box passes and the
  2x resample on random pictures of every small size and radius (past
  r = 128 the SIMD tier falls back to C), and `ugfx_blur_rect()` end to
  end with each tier selected; `fill` and `to_screen` (the framebuffer
  copy, streamed where aligned) at every alignment. "Within one" fails, because a tier's
  +-1 is a seam on screen and a blur cache that misses after a switch.
  `--positive-control` drops the box average's carry and the /255's +1
  and must go red in both groups; `--bench` prints ns/pixel per tier.
  About a second; in `preflight.sh`.
- **`uwmf_hostcheck.py`** -- `userland/libs/media/uwmf.c`, the Windows metafile
  record decoder w16's PlayMetaFile and its OLE pictures draw through,
  compiled with the host gcc under ASan and UBSan. A metafile built by a
  writer in the tool -- MS-WMF's record layouts, arguments last first,
  sharing no code with the decoder -- must decode to exactly the records
  written (function, words in GDI's order, colours, ROPs, object slots,
  points and polygon counts, text and ExtTextOut's advances, a DIB's
  length, a font's face), bare and behind a placeable header; then
  thousands of mutated and truncated copies, which must decode, end or be
  refused and never read past the bytes. `--positive-control` drops the
  Polygon record's point-count bound (the fuzz goes red) and keeps the
  file's argument order (the comparison goes red). In
  `ondemand_sweep.py`; w16 is a side project and in no gate.
- **`w16_hostcheck.py`** -- w16's NE parser (`userland/utils/w16/ne.c`,
  `ne_info.c`) compiled with the host gcc under ASan and UBSan. For every
  sample in `userland/utils/w16/samples/`, `w16 --info`'s lines must EQUAL
  `gen_ne.py --dump`'s (import names aside) -- two independent readers;
  hello's imports must resolve to their names; and thousands of mutated
  and truncated copies go through the parser, every relocation, entry,
  module name and segment byte, which must parse or be refused and never
  read outside the image -- and every RESOURCE, its bytes and lookups by
  number and name. `--positive-control` drops the segment bound
  (the fuzz must go red, a heap-buffer-overflow) and reads relocation
  sites from the wrong word (the comparison must go red); dropping the
  resource bound alone also goes red (checked with `mutate.py`). In
  `ondemand_sweep.py`; w16 is a side project and in no gate.
- **`uadpcm_hostcheck.py`** -- `userland/libs/media/uadpcm.c`, the MS and IMA
  ADPCM block decoder w16's waveOut plays a program's compressed sounds
  through, against ffmpeg -- EXACTLY: both are integer algorithms with one
  right answer, so every sample must equal ffmpeg's `adpcm_ms` and
  `adpcm_ima_wav` decode. ffmpeg also encodes the vectors from PCM the
  script makes (a sweep, a chord, noise, a square after silence; mono and
  stereo at 11025, 22050 and 44100 Hz; blocks of 256 to 2048 bytes).
  `--positive-control` breaks MS's adaptation table and IMA's step (each
  goes red). Needs gcc and ffmpeg; SKIPPED without ffmpeg. In
  `ondemand_sweep.py`.
- **`cpu16_hostcheck.py`** -- w16's 16-bit CPU interpreter
  (`userland/utils/w16/cpu16.c`) against THE HOST'S OWN CPU. Its driver,
  `tools/w16/cpu16_check.c`, puts 16-bit code, data and stack segments in
  the process's LDT (`modify_ldt`, Wine's route), and runs each random
  instruction in the interpreter and natively, entered from 64-bit mode
  by a far call (built `-no-pie`: the return address and the saved stack
  are 32-bit). Registers, flags (each instruction's UNDEFINED flags
  masked) and both segments' bytes must match, over three seeds of 200k
  cases; then random bytes run as code under ASan/UBSan must fault or
  finish and never reach host memory. THE 386 TOO: the registers go in
  and out as 32 bits (high words random), and every family runs under
  the 66 prefix (dwords) and the 67 prefix (32-bit addresses, SIB with a
  scale), with a GS override, plus the 386's 0F set (near jcc, setcc,
  movzx/movsx, bt*, shld/shrd, bsf/bsr, imul r,rm, push gs). TWO HOST
  TRAPS, both found on the i7-11700 (the Ryzen hid them): no register
  survives compatibility mode (R8-R15 and the high halves are undefined
  after it, so the driver reloads from memory), and a test that leaves
  DF set runs glibc's `rep stosb` memset backwards -- the driver `cld`s.
  A host whose FNSAVE no longer stores FCS/FDS (CPUID 7 EBX bit 13) or
  keeps FDP/FOP only for an exception (bit 6) has those fields left out
  of the x87 comparison, and the driver says so. Far transfers, segment loads and
  interrupts are not in the oracle's set. A quarter of the cases are the
  x87 (`cpu16_x87.c`): one random ESC instruction in every register and
  memory form on a random FPU state (float's and double's edges, zeros,
  denormals, infinities, NaNs, empty registers, every rounding and
  precision control, unmasked exceptions), compared as both FNSAVE
  images, the instruction and operand pointers included -- the native
  run FRSTORs before the far call and FNSAVEs after, neither of which
  waits. `--positive-control` breaks
  ADC's carry-in and SAR's sign, a SIB byte's scale and a 66-prefixed push,
  FLDENV's turning of the registers for a
  new TOP and the x87's stores (the oracle goes red each time -- a broken
  address the CPU then faults on ends the driver, which counts as red) and `mem()`'s limit
  check (the fuzz goes red). A host without `modify_ldt` prints SKIPPED,
  never PASS. In `ondemand_sweep.py`.
- **`ugfx_text_hostcheck.py`** -- compiles the real
  `userland/libs/ui/ugfx_text.c` and `userland/libs/ui/uui_textbox.c` with the
  host gcc against a SYNTHETIC proportional face and sweeps ~4,400
  checks over it. The face is deliberately hostile -- `i` and `l` are
  3px where `W` and `M` are 20 -- so a surviving `* char_w` is off by
  6x rather than by a rounding error, and kerning is non-zero on real
  pairs (`AV`, `To`) because a measurement that ignores it agrees with
  drawing only when no such pair appears.

  Three things, and the second is the one that matters. The
  MEASUREMENT against a Python oracle that shares no code with it.
  The ROUND TRIP: the x at which the widget DRAWS character `i` must
  hit-test back to `i`, for every character at every caret position --
  which is what catches `draw()` and `index_at_x()` drifting apart, the
  failure that put a click on a different glyph than the pointer. And
  the WINDOW: the caret stays inside the field, the value never scrolls
  further than it must, and nothing is drawn past the inner edge.

  **Every case is BYTES** since UTF-8 step 1 (`docs/utf8-design.md`):
  Polish, Greek, Cyrillic, box drawing, CJK, a Latin-1 word and broken
  sequences, decoded by an oracle written from RFC 3629 -- so every
  index must be a character boundary, and `ugfx_text_next/prev` are
  swept at each one. It also drives `uui_edit` through a textbox and
  reads the BUFFER back: Right over a whole `ż`, Backspace and Delete
  taking both its bytes, a typed U+017C inserted as C5 BC, a masked `żż`
  as two stars. `--positive-control-bytes` puts back byte-at-a-time
  decoding and stepping and must go red.

  `--positive-control` restores the cell arithmetic and requires the
  suite to go RED (569 of 4,407 do). **That control did not fire the
  first time it was written**, and for the reason this repo keeps
  writing down: it wrote the modified copy to `tmp/uui_textbox.c` while
  the driver includes `ui/uui_textbox.c`, so the bug never reached the
  compiler and a green run meant nothing. It writes to `tmp/ui/` and
  puts that `-I` first now, and asserts the edit changed something. In
  `ondemand_sweep.py`.
- **`utext_hostcheck.py`** -- also checks the EDIT HISTORY
  (`ui/uui_undo.c`: a word, a Backspace run, a replacement and a paste
  are one step each, clean is a position, a full history forgets its
  oldest steps) and that the line-number gutter takes its share of the
  box in the hit test. It compiles `userland/libs/ui/utext.c` with the
  host gcc, beside a NAIVE implementation of the same wrap rule written
  out longhand in the driver, and requires the two to agree over a
  corpus: this repository's own Markdown, a 4,000-line synthetic
  document, a 60 KB paragraph with no newline in it at all, and
  `data/pci.ids` when it is there.

  **It exists because the editor's wrap is now ACCELERATED.** utext used
  to rescan the document from character 0 on every draw, every metric
  and every click; it keeps a sparse checkpoint table now (`struct
  utext_wrap`) so that a 1.6 MB file costs a screenful of work per frame
  instead of a documentful. An accelerator is exactly the code that is
  right on the corpus somebody tried and wrong on the one they did not,
  and its failure is a caret one character out -- invisible in a
  screenshot, obvious against an oracle.

  **The load-bearing check is a ROUND TRIP.** The point at which the
  oracle says character `i` is drawn must hit-test back to `i`, for
  every character on the screen, at five scroll positions from the top
  of the document to the bottom -- so lookups that start from a
  checkpoint deep in the file are exercised, not just the first screen.
  That is what catches `draw()` and `index_at_point()` drifting apart.
  It also checks the total line count at several widths, that
  `utext_scroll_top`/`_bottom` reach the actual ends, that a selection
  reports its TRUE length when the caller's buffer is too small for it,
  that a paste drops `\r`, and that `utext_putc` REFUSES at capacity
  rather than dropping the oldest character.

  It found a pre-existing off-by-one the hour it was written:
  `index_at_point()` tested "have we reached the target cell" BEFORE
  applying a pending wrap, while `draw()` applies it first, so clicking
  the first character of a wrapped continuation line placed the caret
  one character late -- on every wrapped line, and only there.

  `--positive-control` moves one checkpoint in the index by three
  characters and requires the round trip to go red.

  ON DEMAND: it needs a host gcc, and the gate must not start requiring
  one.
- **`uwinsetup_hostcheck.py`** -- `userland/libs/base/uwinsetup.c`, the engine
  behind `w16 --install` (with `ufatimg.c` and `umsexpand.c` under it),
  compiled on the host under ASan + UBSan. Synthetic disks from
  `gen_win3_disks.py` in all four forms (one folder, a folder per disk,
  floppy images, a mix) must install EXACTLY that generator's own table of
  files, folders and bytes -- decoys absent, no `.part` left; INI files kept
  or replaced as asked, each with Setup's `[fonts]`, `[boot]`,
  `[boot.description]` and `[keyboard]` lines and unchanged by a second
  run; a missing file planned as missing and the install
  stopping on it; a folder with no SETUP.INF refused. With
  `~/CodingProjects/saunaos-other` here (local only): from the English folder
  and from its eight floppy images, every file must land where Express
  Setup put it on the reference VM (`win311-vm/win311.img`, read with
  mtools) and match libmspack's expansion, and the INI lines Setup wrote
  must match the reference's (`REF_INI_SKIP` names the few that depend on
  the network or Setup's own run); the Finnish set must plan with
  nothing missing. Then mutated SETUP.INFs and damaged compressed files.
  `--positive-control` sends `0:system` sections to WINDOWS (placement goes
  red) and unbounds a field (UBSan goes red). In `preflight.sh`.
- **`ufatimg_hostcheck.py`** -- `userland/libs/base/ufatimg.c`, FAT12/FAT16 disk
  images read from memory (setup floppies as files), compiled on the host
  under ASan + UBSan against MTOOLS as the second reader: a 1.44 MB floppy,
  a 1.68 MB DMF one and a FAT16 volume it formats, with files of 0-513 and
  300,000 bytes, a directory two deep, a deleted and a long-named file --
  label, listings and bytes must agree. With `~/CodingProjects/saunaos-other`
  here (local only), the eight Windows for Workgroups 3.11 floppies must
  hold exactly the install set's 568 files. Then truncated and mutated
  images, each in a buffer exactly its size. `--positive-control` swaps
  FAT12's nibbles (bytes go wrong) and stops checking a cluster against
  the image's end (ASan goes red). In `preflight.sh`.
- **`umsexpand_hostcheck.py`** -- `userland/libs/base/umsexpand.c`, the SZDD and
  KWAJ expander w16's Windows installer runs over setup disks, compiled on
  the host under ASan + UBSan. Round trips: `mscompress.py` writes SZDD and
  KWAJ methods 0-3 (method 3 in each of its code-length encodings) of
  empty, tiny, random, repetitive and >64 KiB data, and the bytes and name
  must come back. Real disks: when `~/CodingProjects/saunaos-other/win311`
  is there (`SAUNAOS_W311` moves it; local only, Microsoft's files), every
  compressed file of the English and Finnish Windows for Workgroups 3.11
  sets must expand byte for byte to libmspack's output -- 1,141 files;
  skipped, saying so, anywhere else. Then thousands of truncated and
  mutated copies, each in a buffer exactly its size. `--positive-control`
  gives MATCHLEN2 32 symbols (round trips and real disks go red; the
  tables then overrun the scratch, which ASan names) and drops the LZSS
  reader's check that a match's two bytes are there (the fuzz goes red).
  In `preflight.sh`.
- **`uhlp_hostcheck.py`** -- `userland/libs/base/uhlp.c`, the Windows Help file
  reader, compiled on the host under ASan + UBSan against files WRITTEN by
  `hlpgen.py`, so the answer is known and no Microsoft data is needed. It
  checks the container (a directory tree of several levels, a compressed
  multi-block |TOPIC, the phrase table, |FONT, |SYSTEM's records), every
  context ID finding its own topic, the browse sequence, keywords, each
  topic's text against the model, links by kind, macros, a two-row table
  being ONE table (it was one per row until this check found it),
  `uhlp_hash` against an independent Python hash and `uhlp_id_for_hash`
  hashing back to its input -- then opens the same files with bytes
  flipped, ranges zeroed, sizes set to 0xFFFFFFFF and the end cut off
  (the reader parses untrusted input: a fuzz of 30,000 mutated files came
  back clean). `--fuzz N --seed S` sizes it; `--positive-control` runs a
  wrong hash multiplier (the lookups must fail), a picture's size counted
  from before its hotspot count, a record clamped to its block (the
  spanning variant must fail), the non-scrolling region's bounds compared
  with the wrong number, a window's name read from 3.0's offset, and a
  bounds check that always agrees (the fuzz must crash). Every model check
  also runs on a SPANNING file, whose records straddle block boundaries as
  Microsoft's compiler writes them; Contents has a non-scrolling region and
  the window an asymmetric colour, so a COLORREF read unswapped shows.
  In `preflight.sh`.
- **`uwmf_draw_hostcheck.py`** -- `userland/libs/ui/uwmf_draw.c`, a 16-bit
  metafile played into a box (Help Viewer's WMF pictures), compiled on the
  host with the real `media/uwmf.c` under it and STUBS of ugfx and `lib/uimg`
  that print each call. Eight metafiles written in Python, each with the
  calls it must cause worked out from MS-WMF: a window with an origin, an
  extent's shrink, a placeable header's bounds, SaveDC/RestoreDC, null and
  solid pens and brushes, COLORREF to RGB, text on its baseline in the
  mapped font size with the caller's font put back, and a 24-bit and an
  8-bit (short palette) DIB whose bits the BMP made of them must point at.
  `--positive-control`: the window's origin ignored, a DIB's palette
  forgotten. In `preflight.sh`.
- **`ubinmap_hostcheck.py`** -- the Hex Viewer's structure decoders
  (`userland/libs/base/ubinmap*.c`) and its inspector maths
  (`userland/libs/base/ubyteval.c`) on the host under ASan + UBSan, against
  files it BUILDS: an ELF64 and a big-endian ELF32, a PWAD, a TrueType
  table directory, an MBR, a GPT with UTF-16 names, FAT16 and FAT32 boot
  sectors, a QOI and a BMP, each with its exact format line and nodes
  (label, value, offset, length), the paths a status bar shows, and the
  tinted runs (disjoint; where two overlap the later wins). Then the
  hostile half: counts of 2^31 and 2^32, offsets that wrap, a negative
  WAD header, an entry size of 0, a WAD of 100000 lumps (the node cap
  must hold), and every prefix plus 3000 random flips of each file. The
  driver's reader holds EXACTLY the file and fails the run if a decoder
  ever asks it for a byte past the end -- the decoder must have refused
  first -- and every node must lie inside the file. The inspector rows are
  checked in both byte orders (the shortest float text, NaN, infinity,
  UTF-8 and the ways a sequence is not one, a value that needs more bytes
  than remain), with typed offsets, hex patterns and the copy texts.
  `--positive-control` runs eight breaks (a bounds check that always
  agrees, a range not clipped, a field read from the wrong offset, the
  WAD directory trusted past the file, the tint winner inverted, a
  section name read past .shstrtab, an integer's sign lost, the node cap
  gone); each must go red. In `preflight.sh`.
- **`uhlpmac_hostcheck.py`** -- `userland/libs/base/uhlpmac.c`, the WinHelp macro
  language (what a help file runs: its buttons, jumps, marks), on the host
  under ASan + UBSan. A table of 46 macro texts and the events each must
  cause (jumps and pop-ups by id, number, hash and keyword; commands;
  buttons; marks; `IfThen`, `IfThenElse`, `Not`, nesting; the short forms;
  every string quoting), REJECTION (a syntax error runs none of it, not
  even the macros before it), the depth limits (a macro run from a string
  from a string ... stops and says so; `A(A(A(...` is refused), what is
  known and not done saying why (`ExecProgram` is refused on purpose) --
  then the corpus mutated and fuzzed. `--fuzz N --seed S`;
  `--positive-control` runs the else branch for a true condition (the table
  must fail) and an unterminated string read past its end (the fuzz must
  crash). In `preflight.sh`.
