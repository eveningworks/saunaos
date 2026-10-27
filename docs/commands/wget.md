# wget

**a `/bin` program.**

**Category:** Networking

## Synopsis

    wget [OPTION]... URL...

## Options

- `-O <file>`, `--output-document <file>` -- write the body to `<file>`
  rather than to standard output, creating it or truncating it, and
  print the byte count when each fetch finishes. Several URLs into one
  file are concatenated, as GNU wget's are. The connecting line and any
  error still go to the terminal.
- `-k`, `--no-check-certificate` -- do not verify the server's
  certificate. The connection is still encrypted; it is no longer
  *authenticated*, so a warning is printed. https only.
- `--weak-entropy` -- key the connection even when this machine's
  randomness is only TSC jitter. https only, and see below.
- `--max-redirect <n>` -- follow at most `n` redirects, 0 to 100; 20 by
  default, GNU wget's figure. `0` follows none and reports the 3xx
  status, which is curl's default.
- `-S`, `--server-response` -- print the status line and every response
  header, indented, before the body -- exactly as the server sent them,
  for each hop of a redirect.
- `--post-data <string>` -- send a POST with `<string>` as its body, as
  `application/x-www-form-urlencoded`. A 303 answer, or a 301/302, turns
  the next hop into a GET, as browsers do.
- `--header <line>` -- add one request header, `"Name: value"`. One naming
  `User-Agent` or `Accept-Encoding` replaces the default; `Host`,
  `Content-Length`, `Connection` and `Transfer-Encoding` are libhttp's and
  are refused.
- `-h`, `--help` -- every option and what it does.

## Description

`/bin/wget` — fetch URLs over HTTP or HTTPS and print them, or save them
with `-O`. Several URLs are fetched in turn over **one kept-alive
connection per host** (a `struct uhttp_pool`), which is what GNU wget
does with several URLs on its command line.

The protocol is not in this program. It lives in `/lib/libhttp.so`
(`<uhttp.h>`), which reaches `/lib/libssl.so` (`<utls.h>`, mbedTLS) for
the https half; wget is the front end that parses arguments, decides
where the body goes, and words the failures. The same library is the
Web Browser's network (`userland/backends/netsurf/fetch_http.c`),
`update`'s and DOOM's game-data downloader's.

## The scheme may be left off

`wget example.com` tries **https first** and falls back to http, which
is what a browser does with a typed address — Chrome auto-upgrades,
Firefox has HTTPS-Only Mode. GNU wget and curl both default to plaintext
instead; that is the older answer to a question the web has since
settled.

**It falls back only when port 443 does not answer.** Never on a
certificate failure, never on a handshake failure, and never on the
entropy refusal — because falling back there would turn "this server's
identity is wrong" into "let us talk in plaintext instead", which is the
downgrade HSTS exists to stop. So the fallback means "this host does not
speak https", never "https did not go well".

**The fallback is announced**, since a downgrade nobody asked for should
not be silent:

    $ wget example.invalid
    wget: no https on example.invalid, falling back to http (not encrypted)

**An explicit port cancels the guess unless it is 443.** `host:8080` is
overwhelmingly a plain server, and Chrome likewise upgrades only on the
default port. Say `https://host:8443` when you want TLS on an odd port.

A scheme this does not speak — `ftp://` — is refused rather than treated
as a hostname.

## HTTP/1.1

**HTTP/1.1, and a connection may outlive its request.** A response ends
where its framing says -- a `Content-Length`, or a chunked body -- and
then its connection waits in the pool for the next URL to the same
scheme, host and port. A lone URL has no next request, so it is sent
with `Connection: close` and the server may end the body by closing.

A kept connection the server has meanwhile closed is found out by the
request that takes it, before any byte of the answer arrives, and that
request is sent once more on a fresh connection -- a GET or a HEAD only,
which are safe to send twice. A POST always opens a fresh connection.

A gzipped body (`Accept-Encoding: gzip` is sent) is inflated before it
is written, inside chunked framing or not; `deflate` is never asked for
and refused if sent.

## The progress meter

A transfer draws a one-line meter, redrawn in place four times a second:

    [=========>          ]  47%  4.7M/10.0M  241K/s eta 22s

Without a `Content-Length` there is no bar and no percentage, because
neither can be known — it shows what has arrived, the rate, and how long
it has been going:

    4.7M 241K/s in 20s

The rate is the **average over the whole transfer**, not a recent
window. It is steadier to read, but it lags a connection whose speed
changes — so the ETA of a transfer that has just stalled is optimistic
for a while.

**It is drawn on standard error, and only when that is a terminal.**
Both halves matter. Without `-O` the body *is* standard output, so a
meter there would corrupt every `wget URL > file`; and a redirected
standard error — a service log, a test harness — would otherwise fill
with carriage returns. GNU wget and curl both make the same two choices.
So a meter appears when you are watching, and never lands in a file.

## HTTPS

**TLS 1.3 and 1.2, ECDHE with AES-GCM or ChaCha20-Poly1305.** No CBC, no
RC4, no static RSA, no 3DES — every suite offered is authenticated
encryption with forward secrecy, so a server key that leaks later cannot
decrypt traffic recorded today.

**The certificate is verified by default**, against the PEM files in
`/etc/ssl/certs`. Three outcomes, deliberately worded differently
because they send you to different places:

| Message | What it means |
|---|---|
| `no trust anchors in /etc/ssl/certs` | This machine was never told whom to trust. Build with `EXTRAS=1`, or drop a PEM in |
| `certificate verification failed: <reason>` | The certificate is wrong: `not trusted by any anchor in the store`, `has expired`, `is not valid yet`, `is for a different host name`, `is self-signed`, `has been revoked` |
| `WARNING: the server's identity was NOT verified` | You passed `-k`; the traffic is encrypted and nobody checked who is on the other end |

**The trust store is empty on a default build, and that is a state, not
an oversight.** `make iso EXTRAS=1` fetches Mozilla's CA bundle
(MPL-2.0) into it after showing the licence. Without anchors, `https://`
refuses by name rather than connecting to something it cannot vouch for.

## Randomness, and why a fetch may refuse

A TLS private key is only as secret as the randomness it came from.
`QUERY_RANDOM` reports what this machine's source actually is, and wget
**refuses** below virtio-rng:

    $ wget https://example.com/
    wget: this machine's randomness is TSC jitter, which is too weak to
      key a connection with -- give the guest a virtio-rng device, or
      accept it explicitly
    wget: pass --weak-entropy to accept it anyway

Under plain QEMU the source is TSC jitter — software timing software —
so this fires on every emulated boot. On real hardware with
RDSEED/RDRAND, or a guest given `-device virtio-rng-pci`, it does not.
`random` says which source you have.

## What it is not

**No cookies, no resume, no recursion.** A non-2xx status is reported
and the body is still printed, because an error page is usually the
explanation; the exit status is what a script reads, 1 if any URL
failed. A redirect from https to plain http is refused, never followed.

**Not a downloader.** There is no retry: a failed fetch is a failed
fetch.

**No client certificates**, and no TLS *server* anywhere in SaunaOS —
`httpd` speaks plaintext, because giving it TLS means a private key on
disk and a decision about where it lives.

## Output

    $ wget http://10.0.2.2:8000/hello
    connecting to 10.0.2.2
    hello from the host

    $ wget https://example.com/
    connecting to 104.20.23.154 -- TLSv1.3, TLS1-3-CHACHA20-POLY1305-SHA256
    <!doctype html>...

    $ wget -O /tmp/page.html https://example.com/
    connecting to 104.20.23.154 -- TLSv1.3, TLS1-3-CHACHA20-POLY1305-SHA256
    saved 4547 bytes to /tmp/page.html

The address is printed before the body so a response from an unexpected
host is attributable, and on https the negotiated version and suite are
printed beside it — the two facts that say what the encryption actually
is. Other failures:

| Message | What it means |
|---|---|
| `no nameserver configured` | run `netd`, or set one in `/etc/resolv.conf` |
| `not found` | DNS answered, and the name does not exist |
| `cannot connect to <host> port <n>: connection refused` | the host answered, and nothing is listening on that port |
| `cannot connect to <host> port <n>: <other reason>` | no answer at all -- the host is down or filtered, or the interface has no address yet |
| `cannot parse '<url>' as a URL` | a scheme this does not speak, or a host/path too long to hold |

## See also

`host` for resolution on its own, `netd` for where the nameserver comes
from, `netctl` for the counters and whether the card is configured,
`random` for the entropy source, and
`docs/conventions/kernel.md`'s networking entry for what the stack under
this does and does not do.
