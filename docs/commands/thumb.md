# thumb

**a `/bin` program.**

**Category:** Graphics and the desktop

## Synopsis

```
thumb [-s PX] [-q] FILE...
```

## Options

- `FILE` -- a picture (QOI, PNG, JPEG, BMP, GIF) or a video.
- `-s PX`, `--size PX` -- the thumbnail's longer side in pixels, 16..256;
  the default is 64.
- `-q`, `--quiet` -- print nothing; the exit status says how it went.
- `-h`, `--help` -- the options.

## Description

Makes each FILE's thumbnail in `/var/cache/thumbnails`, or confirms the
one there is newer than the file -- the cache the File Manager, the
Image Viewer's filmstrip and the desktop all read (`lib/uthumb.h`). A
video's thumbnail is one of its frames.

```
/$ thumb -s 48 /home/Desktop/beach.jpg /home/Desktop/notes.txt
/home/Desktop/beach.jpg: ok
/home/Desktop/notes.txt: not a picture or a video
```

**IT IS THE DESKTOP'S THUMBNAILER.** The desktop is drawn by the
compositor, which never runs a picture or video decoder on a file
someone else wrote: it runs `thumb` for the pictures on the desktop and
reads back only the finished QOI. A file that crashes a decoder ends
this program, not the desktop -- Windows' out-of-process thumbnail
handlers and GNOME's sandboxed thumbnailers make the same split.

**IT DECODES SANDBOXED** (`pledge`/`unveil`, `abi/sandbox_abi.h`): before
the first decode it can read only the folders of the FILEs it was given
and write only `/var/cache/thumbnails`, and it keeps no promise but
files -- no network, no spawn. Given files in more folders than the
unveil list holds, it reads everything instead, still writing only the
cache. `ps --sandbox` shows it while it runs.

## Exit status

`0` every FILE has a thumbnail, `1` some FILE is not a picture or a
video (or could not be read), `2` a usage error, or the sandbox could not
be entered (it then decodes nothing).

## See also

[`open`](open.md) to open a file with the app its type belongs to.
