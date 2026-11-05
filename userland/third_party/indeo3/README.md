# Indeo Video 3 tables -- vendored, not written here

`upstream/` is FFmpeg's, byte for byte. **Do not edit it**: a change is a
patch under `patches/` (`docs/conventions/build.md`, "A VENDORED PORT IS
NEVER EDITED IN PLACE").

- **Upstream**: https://github.com/FFmpeg/FFmpeg, tag `n7.1`
- **Files**:
  - `upstream/indeo3data.h` -- `libavcodec/indeo3data.h` at that tag
    (sha256 `2183c5379cd9980043d9c807f41fc5271641a09704d4fc3d3c0719dd2202d502`).
  - `upstream/COPYING` -- `COPYING.LGPLv2.1` at that tag, renamed to the
    file name `tools/checks/check_licenses.py` looks for
    (sha256 `b634ab5640e258563c536e658cad87080553df6f34f62269a21d554844e58bfe`).
- **Licence**: LGPL-2.1-or-later (the header's own notice), declared as
  `PORT_LICENSE_indeo3` in the Makefile.

## What is taken, and what is not

ONLY the constant tables: the 21 delta codebooks Indeo 3 cells quantise
against (`vq_tab`), each with its dyad count and its quad side. They are
data an Indeo 3 stream is meaningless without -- the codec's own
codebooks -- and not derivable from a format description.

The decoder that reads them is SaunaOS's own, MIT:
`userland/libs/media/uvid_indeo3.c`, written from the public format
descriptions (the MultimediaWiki page, Ferguson and Melanson's
`indeo3.txt`, and the decoder pseudo-code in Intel's US patent 5,386,232,
which the wiki links) with FFmpeg's decoded OUTPUT as the oracle --
never FFmpeg's `indeo3.c`. `tools/hostchecks/indeo3_hostcheck.py`
compares the two frame by frame.

## glue/

Ours. `glue/config.h` answers the one thing the header asks its
includer for, `HAVE_BIGENDIAN` (FFmpeg's `config.h`): the tables pack two
deltas into a word in the host's byte order, and x86-64 is little-endian.
