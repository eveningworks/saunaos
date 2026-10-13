# webcontent

**a `/bin` program.**

**Category:** Graphics and the desktop

## Synopsis

    webcontent --app SERVICE [--net SERVICE] [--scheme]

## Options

- `--app SERVICE` -- the embedding app's channel (`wv<pid>`); required.
- `--net SERVICE` -- the network process's channel (`wn<pid>`). Without
  it the page reaches only local pages -- `file:`, `resource:`, `data:`
  -- and an http(s) link is handed back to the app instead.
- `--scheme` -- ask the embedding app for `app:` pages (Help's).
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

Typing `about:crash` in the Web Browser's address field makes the page's
engine fault on purpose (Chrome's `chrome://crash`), to see that.

What it prints goes to the application log: `log -u webcontent`.
