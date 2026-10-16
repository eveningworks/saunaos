# webcontent

**a `/bin` program.**

**Category:** Graphics and the desktop

## Synopsis

    webcontent --app SERVICE [--net SERVICE] [--scheme] [--files]

## Options

- `--app SERVICE` -- the embedding app's channel (`wv<pid>`); required.
- `--net SERVICE` -- the network process's channel (`wn<pid>`). Without
  it the page reaches only local pages -- `file:`, `resource:`, `data:`
  -- and an http(s) link is handed back to the app instead.
- `--scheme` -- ask the embedding app for `app:` pages (Help's).
- `--files` -- started for a LOCAL page: the app reads `file:` pages
  for it, and a link from one to the web is handed back to be loaded in
  a fresh process.
- `-h`, `--help` -- the options.

## Description

A web page's **engine**, one process per view: NetSurf parsing, laying
out and painting one page into shared memory for the app that embeds it
(`ui/uui_webview.h`), which starts it. Chrome's renderer and WebKit's
WebContent process are the same thing. It has no window; the page leaves
it as a pixel buffer, and the person's input arrives as records.

It is the one GPL-2.0 binary behind the Web Browser -- the browser, Help
and System Update link no NetSurf -- and a page that crashes NetSurf
costs only this process: the view keeps the last picture of the page,
greyed, under "This page stopped." with Reload and View report. The
report is `/var/crash/webcontent-<pid>.crash`.

**It runs sandboxed** (`abi/sandbox_abi.h`): the app starts it with
only `stdio rpath shm settings`, and before NetSurf starts it unveils
its own resources (`/usr/share/netsurf`), the fonts, the settings and
locale files, and its two channels, then locks that list. No socket, no
spawn, no writing a file, and no local file outside that list -- every
other `file:` page is read by the app, and only for a page the app
itself sent to a local file; a web page asking for one is told "A web
page cannot open files on this computer." A process that cannot enter
its sandbox loads no pages. `ps --sandbox` shows it, and a syscall
outside its promises ends it with a crash report naming the call.

Typing `about:crash` in the Web Browser's address field makes the page's
engine fault on purpose (Chrome's `chrome://crash`), to see that.

What it prints goes to the application log: `log -u webcontent`.
