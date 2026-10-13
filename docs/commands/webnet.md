# webnet

**a `/bin` program.**

**Category:** Networking

## Synopsis

    webnet --app PID [--weak-entropy]

## Options

- `--app PID` -- the Web Browser it serves; required. It answers on the
  channel `wn<PID>` and exits when that browser does.
- `--weak-entropy` -- key TLS even when this machine's only randomness is
  timer jitter, as `wget --weak-entropy` does; the browser passes it on
  from its own `--weak-entropy`, for a test guest with no virtio-rng.
- `-h`, `--help` -- the options.

## Description

The Web Browser's **network process**: one per browser window, started
by it, never typed. It owns what a page's engine (`webcontent`) is not
trusted with -- the sockets and TLS (through `/lib/libhttp.so`, with a
pool of kept-alive connections), the cookie jar, the certificates a
person accepted anyway on NetSurf's warning page, and downloads, which
it writes itself so a download goes on after the tab that started it
closes. WebKit's NetworkProcess and Chrome's Network Service are the
same split (`docs/browser-design.md`, stage 5).

It keeps:

| file | what |
|---|---|
| `/var/lib/browser/cookie-jar` | the cookies, a line each, tab-separated; session cookies are not written. Saved every ten seconds when it changed, and on exit. |
| `/home/Downloads/<name>` | each download, as `<name>.part` until it is whole |

The first time it starts with no `cookie-jar`, it reads NetSurf's own
cookie file (`/var/lib/browser/cookies`, from before stage 5) once.

What it prints goes to the application log: `log -u webnet`.
