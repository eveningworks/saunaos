# scummvm

**a `/usr/bin` program** -- ScummVM, which plays classic point-and-click adventure games from their original data files.

**Category:** Graphics and the desktop

## Synopsis

    scummvm [options] [target]
    scummvm --add --path=DIR
    scummvm --list-engines | --list-targets | --version

## Description

ScummVM 2026.4.0 on its own SDL backend, which SaunaOS's SDL carries
(`docs/sdl-design.md`, stage 4), built with four engines: `sky`
(Beneath a Steel Sky), `queen` (Flight of the Amazon Queen), `lure`
(Lure of the Temptress) and `drascula` (Drascula: The Vampire Strikes
Back). With no arguments it opens its own launcher, a window listing
the games it knows; a target -- the name `--add` gave a game -- starts
that game straight away. Every option ScummVM documents works as
documented (`scummvm --help` lists them).

    # scummvm --list-engines
    # scummvm --add --path=/usr/share/games/lure
    # scummvm lure

**The Start menu's ScummVM is a front** (`/bin/wm/apps/scummvm`): with a
game in `/usr/share/games` it adds any new one with `--add` and opens
ScummVM's launcher; with none, or as **Free Adventure Games**
(`/bin/wm/apps/scummvm --games`), it shows a card that downloads the four
freeware games, each checked against the SHA-256 ScummVM publishes, and
starts ScummVM when they are in. `mirror=` in `/etc/scummvm-games.conf`
fetches them from another server.

Its list and options are `/home/.config/scummvm/scummvm.ini`, its saves
`/home/.local/share/scummvm/saves/` -- ScummVM's own rule for
`XDG_CONFIG_HOME` and `XDG_DATA_HOME`, which `/etc/environment` sets (the
front sets the same defaults when started without them, or ScummVM would
write its list into its working directory). A game counts as added once
ScummVM's list names it; a folder ScummVM does not take is offered again
at the next start, up to three times, and `dmesg` says `ScummVM did not
list <id>`.

## What it deliberately does not do

- **Only those four engines.** A game for any other (SCUMM, SCI, AGI)
  is "not supported" here; another engine is one name in
  `tools/build/build_scummvm.sh`.
- **No MP3, FLAC, MIDI devices, network or OpenGL**: the four games need
  none. Drascula's music is Ogg Vorbis, which is in.

## The trap

It is on the disk only when the image was built after `make scummvm`
(the ScummVM component; never in a `NOGPL=1` build), and only then are
the Start-menu entries there.
