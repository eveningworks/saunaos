# The clipboard past one page

**Status: BUILT 2026-11-28, option B1, all three stages** (94dbc5862,
d83ae8c53, aee3d0d07). Written 2026-11-27 as fixed tables round 4, track
D (`docs/roadmap.md`, "The clipboard past one 64 KiB page"). The
maintainer's answers to the open questions, all as recommended: B1, the
inline area stays 64 KiB, a sandboxed paster reaches objects through
`unveil("shm:clipboard")`, a clipboard per session is a later roadmap
item, and no ceiling (clipboardd logs an object past 64 MiB).

**Two things below were wrong about the kernel, and the build fixed
them:** a shm name could not contain '/' (`name_ok()` refused it), so
`clip/<serial>` would have been refused -- names now take '/' between
segments, and the objects are `clipboard/<n>`, under the page's own
name, which also makes the page's unveil the objects' (question 3 needs
no second unveil); and the kernel capped an object at 32 MiB
(`SHM_BYTES_MAX`), a ceiling under "no ceiling", which went too.

## What is wrong today

The clipboard is one shared page, `struct clip_page`
(`userland/libs/base/uclip_page.h`), created by `/bin/clipboardd` and
read and written directly by every app through `media/uclip.h`. Three
bounds are compiled into it:

| Bound | Value | What a person sees past it |
|---|---|---|
| `CLIP_BYTES` | 64 KiB of payload | Copy of a longer text answers 0; the app says so or (worse) says nothing |
| `CLIP_MAX` | 64 entries | Copying 65 files: `uclip_add()` answers 0 and `uclip_commit()` then puts NOTHING on the clipboard |
| `CLIP_DRAG_BYTES` | 8 KiB | A drag of many files, or of a few deep ones, carries none |

Two things make the bounds worse than their size:

- **The desktop ignores the answer.** `desktop.c`'s `add_selection()`
  calls `uclip_add()` without reading it and counts every icon, so a
  copy of 65 icons reports 65 and pastes nothing. (This one is a bug
  whatever the design; stage 1 fixes it.)
- **Track A made paths long.** A path is sized to fit everywhere now,
  up to `FS_PATH_MAX` 4096. Sixteen such paths fill the clipboard, and
  two fill the drag slot. Before track A a path stopped at 255 bytes
  and the page looked roomy.

`struct uclip`, the reader's copy, holds `char data[CLIP_BYTES]` by
value, so every reader carries 64 KiB whether the clipboard holds a
word or nothing.

## What real systems do

**Wayland.** The clipboard holds no data. A source OFFERS MIME types
(`wl_data_source.offer`); a paster picks one and calls
`wl_data_offer.receive(mime, fd)` with the write end of a pipe; the
compositor passes the fd to the source, which writes and closes. No
cap, no copy until asked, and the format is negotiated. The cost is
the famous one: when the source exits, the clipboard is empty. Every
desktop adds a clipboard manager that pastes everything into itself to
keep it (through `ext-data-control`, earlier `wlr-data-control`). Drags
use the same source and offer objects, separate from the selection.

**X11.** The same ownership model: the selection OWNER answers a
`ConvertSelection` by writing a property on the requester's window.
Past the server's maximum request size it switches to INCR, chunks
handed over one property write at a time. Same weakness, same clipboard
managers.

**Windows.** The opposite: `SetClipboardData()` hands the system a
memory block, and the system owns it from then on. It survives the
app, costs a full copy at Copy time, and has no cap but memory.
Delayed rendering (`SetClipboardData(format, NULL)`, then
`WM_RENDERFORMAT`) is the opt-in Wayland shape, and an app that used it
must render everything at exit (`WM_RENDERALLFORMATS`).

**macOS.** `pboard` is a server that holds the data, as Windows does,
with lazy "promised" types on top.

So there are two shapes: THE SYSTEM HOLDS IT (Windows, macOS: survives
the app, a copy up front) and THE SOURCE HOLDS IT (X11, Wayland: no
copy until pasted, formats negotiated, gone with the source unless a
manager copies it anyway).

**SaunaOS today is the first shape**, and deliberately:
`uclip_page.h` says the clipboard surviving every app's exit is the
reason the daemon exists. The roadmap line says "handed over as
Wayland does", but what this OS needs from Wayland is the missing CAP,
not the ownership model. Taking the ownership model would need a
clipboard manager at once to give back what the page gives for free.
This doc recommends keeping "the system holds it" and removing the cap.

## What must stay true

1. **The clipboard outlives the app that copied.**
2. **Greying Paste costs nothing.** An app watches `serial` in the page
   every frame; that stays a memory read.
3. **A small copy stays as cheap as it is.** A word copied is a memcpy
   under the page's lock, no round trip to a server.
4. **A Copy that cannot be done says so.** Never a partial file set.
5. **The drag slot stays apart from the clipboard**, as XdndSelection
   and Wayland's drag source are: a drag must not replace what was
   copied.
6. **A sandboxed process that may paste today may paste tomorrow**
   without its unveil list growing by a directory of other people's
   data.

## The options

### A. A bigger page

Raise `CLIP_BYTES` to, say, 4 MiB; drop `CLIP_MAX` (the entries are
packed strings and need no count bound). One line each.

- For: nothing else changes.
- Against: it is a cap again, which "Size it to the data" rules out; a
  shared object is charged at its full size from the first touch of
  each page only, but every reader's `struct uclip` would need to stop
  holding the payload by value anyway. Kept here as the baseline.

### B. Small payloads in the page, large ones in an object of their own (recommended)

The page keeps its header, its seqlock and an INLINE area (the present
64 KiB, or less). A payload that fits is written exactly as today. One
that does not goes into a shared-memory object sized to it, and the
page names that object instead of holding the bytes:

```
struct clip_page                       shm "clip/<serial>"  (only when large)
  magic, seq, lock...                    the packed payload, len bytes
  op, kind, count, serial
  len            -- bytes of payload
  where          -- CLIP_INLINE | CLIP_OBJECT
  object_serial  -- names the object when CLIP_OBJECT
  data[INLINE]   -- the payload when CLIP_INLINE
  drag_*         -- the same pair for the drag slot
```

Who makes the object decides who may free it, since `shm_unlink` is the
creator's alone. Two ways:

- **B1: clipboardd makes it.** The copier asks the daemon for an object
  of N bytes over a `uchan` (the daemon has none today and gains one),
  maps it, fills it, commits the page. The daemon unlinks the previous
  object once no page names it, as it already deletes copied pictures
  nobody names. One round trip, only for a large copy.
- **B2: the copier makes it, the daemon adopts it.** No new channel:
  the daemon sees the page name an object, maps it (which keeps the
  memory alive after the copier exits) and the kernel lets the dead
  creator's name go. It needs a way for the daemon to remove a name it
  did not create, which the kernel refuses today on purpose.

B1 is the recommendation: it adds a small protocol and no exception to
a security rule.

- For: no cap but memory; the common case is untouched (points 2 and
  3); the reader maps a large payload instead of copying it, so a paste
  of 20 MiB of text costs no 20 MiB copy until the app takes it;
  ownership stays where it is (point 1).
- Against: `struct uclip` changes shape (below); a daemon protocol; and
  a sandboxed paster must be let at the object (open question 3).

### C. Large payloads in a file under `/tmp/clipboard`

What a copied PICTURE already does: the copier writes a file, the page
names it, clipboardd deletes the ones no page names.

- For: the smallest change, with a worked example in the tree
  (`uclip_set_image()`), and no new protocol.
- Against: `/tmp` is a RAM disk with its own size limit, so the cap
  becomes that mount's; a paste is `read()` calls into a buffer (two
  copies of a large payload in memory at once, where B maps one); a
  file is readable by whoever may read the directory, which with
  several accounts is a question B's daemon-made object does not raise;
  and a sandboxed paster needs the directory unveiled.

### D. The source holds it (Wayland's whole shape)

Offers and MIME types brokered by the compositor, bytes over a pipe,
clipboardd turned into the clipboard manager that pastes everything
into itself.

- For: negotiated formats (the roadmap's "Typed clipboard formats"
  item), nothing copied until pasted.
- Against: the largest change by far -- every one of the ~40 files
  that call `uclip_*` -- and it gives up point 1 only to rebuild it
  with a manager. Formats can be added to B later as more kinds or a
  format list in the page; they do not need the ownership model.

## The API change B needs

`struct uclip` stops holding the payload:

```
struct uclip {            // today: char data[CLIP_BYTES] by value
    uint32_t op, kind, count, len, serial;
    const char *data;     // the inline copy, or the mapped object
    void *own;            // what uclip_free() releases
};
int  uclip_load(struct uclip *c);   // as today
void uclip_free(struct uclip *c);   // NEW: every load is paired with one
```

Every caller that loads gains a `uclip_free()`; `uclip_path()`,
`uclip_text()` and `uclip_image()` keep their signatures and still
point into `c`. The staging side (`uclip_begin`/`uclip_add`/
`uclip_commit`) grows its buffer with `base/ugrow.h` instead of failing
at `CLIP_BYTES`, and `uclip_add()` fails only when memory does.
`UCLIP_TEXT_MAX` goes; `uclip_set_text()` fails on memory alone.

## Stages

1. **The honest minimum, no layout change.** The desktop reads
   `uclip_add()`'s answer and reports a failed copy; `CLIP_MAX` goes
   (nothing indexes by it; entries are walked as packed strings). A
   test copies 65 desktop icons and pastes 65.
2. **The clipboard past the page (B1).** The page's `where` word, the
   daemon's channel and object, `uclip_free()` at every caller. Tests:
   a 1 MiB text through Notepad to Notepad, byte-exact; 2,000 files
   copied in the File Manager and pasted; the copier killed before the
   paste; a second large copy frees the first object (the daemon's shm
   count before and after).
3. **The drag slot the same way.** `CLIP_DRAG_BYTES` goes; a drag of
   2,000 files drops whole. Separate because the compositor reads the
   slot while brokering, and its reader changes with it.

## Open questions for the maintainer

1. **Which option.** B1 recommended; C is the small one if a new
   daemon protocol is not wanted yet.
2. **The inline size.** Keep 64 KiB (every text copy seen so far stays
   on the fast path), or shrink the page to 4 KiB so the page itself is
   small and nearly everything but a word goes through the object.
   Recommended: keep 64 KiB.
3. **A sandboxed paster** (a browser page process). An shm unveil
   covers a name exactly or up to a '/' in it (`abi/sandbox_abi.h`), so
   objects named `clip/<serial>` are all covered by one
   `unveil("shm:clip")` beside the page's own. The other way is a GRANT
   from the daemon to the pid that asks, which needs no unveil but a
   round trip per paste. Recommended: the name with a '/', since a
   process allowed the clipboard page is allowed the clipboard.
4. **Several accounts.** Since users stage D the clipboard page is one
   per machine: what one account copies, the next to sign in can paste.
   A clipboard per session (the daemon started per login, as fontd is
   since D3c) is its own item, but B1's "the daemon makes the object"
   is the place a per-session rule would be enforced, so decide the
   direction before stage 2.
5. **A ceiling.** No cap is the rule, but a 2 GiB copy takes 2 GiB of
   RAM until replaced. Windows has none either; the OOM killer is the
   backstop here. Recommended: none, and the daemon logs any object
   over 64 MiB.
