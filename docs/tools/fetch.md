# `tools/fetch/` -- Downloads

One entry per tool in `tools/fetch/`: what it does, why it exists, and the traps it
encodes. The index of every folder is `docs/tools.md`; `tools/t which NAME` finds a tool.

- **`fetch_extras.py`** -- the registry of optional, differently-licensed
  material, and the licence acceptance in front of it. `make iso
  EXTRAS=1` runs it; nothing else does, so an ordinary build reaches no
  network and an ordinary image carries nothing but ours.
  **Every fetcher downloads through its `urlopen()`**, which names itself
  (`USER_AGENT`): Python's default agent is refused by Cloudflare-fronted
  hosts, and `download.blender.org` answered the video fetch 403 until it
  did (2026-10-10). A new fetcher imports it rather than calling
  `urllib.request.urlopen()`.

  **The distinction it is built around is that fetching is not
  distributing.** Downloading a file onto your own machine makes you the
  recipient, and nothing third-party enters this repository -- which is
  why the Doom IWAD is fetched rather than committed. What collapses that
  is publishing a BUILT ARTIFACT: an ISO carrying a fetched WAD, uploaded
  as a release asset, is you distributing the WAD. So an EXTRAS build
  writes `/usr/share/licenses/extras.txt` INTO the image, saying what is
  in there and that publishing it distributes those things -- the one
  note still attached when somebody decides whether to upload it.

  Acceptance is remembered in `.extras-accepted` at the repo root
  (gitignored), keyed by item name AND a hash of the licence summary, so
  a new extra or a changed licence asks again: agreeing to id's shareware
  terms must not silently agree to whatever is added next. It is at the
  ROOT rather than under `build/` because `make clean` wipes that and
  `preflight.sh` starts with one. **A non-tty is REFUSED, never
  prompted** -- a captured build has nothing to answer with and a prompt
  there hangs forever, which is the mtools trap from a different
  direction; `LICENSE=agree` (or `SAUNAOS_LICENSE=agree`) is how CI says
  yes. `--list` shows what exists and what is present.

  It is a registry rather than a script per item, the same shape
  `display_driver` and `block_device` use: the tenth extra is a row.
- **`fetch_soundfont.py`** -- fetches GeneralUser GS (~31 MB) and its
  licence into `data/soundfonts/` (gitignored) for `make iso EXTRAS=1`,
  which is the only thing that runs it -- the `soundfont` row in
  `fetch_extras.py`. `--from PATH` copies any local `.sf2` instead
  (FluidR3_GM, say). Checks RIFF/sfbk and a preset chunk before writing
  anything, since an error page is still a 200.
- **`fetch_video.py`** -- Big Buck Bunny (the Blender Foundation, CC BY
  3.0) for the Video Player, as an `EXTRAS=1` item in
  `fetch_extras.py`'s registry: the Foundation's own 640x360 release,
  PINNED BY SHA-256, extracted with Python's zipfile and transcoded by the
  HOST's ffmpeg to MPEG-1 + MP2 (SaunaOS decodes neither H.264 nor AAC),
  the first three minutes by default (`--full` for all ten), into
  `data/videos-extra/` (gitignored) with the credit beside it. `--from
  ZIP` skips the download. A film tests what the generated clips cannot:
  scene cuts, fades, a rate-controlled encoder and a real soundtrack.
- **`fetch_wad.py`** -- puts a Doom IWAD at
  `data/doom/doom1.wad`, which `make iso` then stages onto the image.
  **`data/`, not `seed/sync/`**: the latter is a staging directory the
  build rewrites, so a 4 MB download placed there survives until the
  next `make clean` and then silently vanishes.
  The shareware `doom1.wad` by default; `--from` takes a local
  `freedoom1.wad` or a retail `DOOM.WAD` you already own. `--freedoom`
  also fetches Freedoom's release zip to `data/doom/`, checked against
  the SHA-256 its release signs, for `doom_data_test.py` to serve.

  **It validates before it writes**, and that is the whole reason it is
  a script rather than a `curl` line in a README: a download that returns
  an HTML error page is still a 200, and 341 bytes of `<?xml` named
  `doom1.wad` fails much later and much more confusingly than it should.
  It checks the `IWAD` magic and a plausible size, and refuses a PWAD
  with its own message (a patch is not a game).
- **`fetch_ca_bundle.py`** -- fetches Mozilla's CA roots into
  `data/etc/ssl/certs/mozilla-roots.pem`, as the `ca-bundle` row of
  `fetch_extras.py`'s table, so `make iso EXTRAS=1` stages a real trust
  store. Not vendored: the bundle is MPL-2.0, and fetching is not
  distributing.

  **It takes curl's PEM conversion rather than Mozilla's own
  `certdata.txt`**, which is an NSS source file in a bespoke format
  needing a parser. The trade is explicit -- one more party in the
  chain, against writing and maintaining a certdata parser here.

  **The fetch itself is verified against the host's trust store.**
  Downloading a root store over an unauthenticated connection would be a
  joke at its own expense. It also refuses a file too small or too large
  to be a root store and counts BEGIN/END blocks before writing, because
  a captive-portal page is the shape of thing that otherwise lands here;
  and it writes to a temporary name and renames, so an interrupted build
  cannot leave a truncated store that fails for only some sites.
- **`netsurf_vendor.py`** -- vendors NetSurf and the libraries it is
  built from into `userland/third_party/<port>/upstream/` at the commits pinned in its `PINS`
  table, and expat at its release tag (`EXPAT`)
  (`docs/browser-design.md`). Clones each from the netsurf-browser
  GitHub mirrors, builds them ON THE HOST with upstream's own Makefiles
  (the libraries, then NetSurf's headless `monkey` front end), and copies
  exactly what that build compiled, read off the compiler's dependency
  files -- plus each port's `sources.mk`, the translation units the
  SaunaOS Makefile builds. Run it to move a pin; the result is a diff of
  `userland/third_party/<port>/upstream/`.

  **Not every `.c` is a translation unit**, which is why `sources.mk`
  exists rather than a glob: libhubbub `#include`s one generated `.c`
  into another. **It never copies upstream's `testament.h`**, which
  records the build host's user name, host name and path; it writes a
  neutral one. **The work directory is NOT under `build/`**
  (`~/.cache/saunaos/netsurf-host` by default): the Makefile includes
  every `.d` file it finds there, and the host build leaves hundreds,
  one of which is not make syntax. The host `nsmonkey` it leaves behind
  is stage 1's oracle. Host tools: `gperf`, `perl`, `pkg-config`,
  `patch` and the host's expat (see the host-tools list). Reaches
  github.com.

  **OUR CHANGES ARE PATCHES** (`userland/third_party/<port>/patches/*.patch`,
  `-p1`, first line a sentence the port's README lists). Each checkout is
  reset to its pin and patched in name order BEFORE the host build, so
  the oracle and the copy are the same source; a patch that no longer
  applies stops the run. **expat is not built on the host** -- the host
  oracle links the host's own -- so its file set is `lib/Makefile.am`'s
  unconditional sources plus `random_getentropy.c`, headers by `gcc
  -MM` against our `userland/third_party/expat/glue/expat_config.h`.
