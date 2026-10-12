#!/usr/bin/env python3
"""A web server on THIS machine for testing libhttp and the Web Browser.

The guest reaches it through SLIRP as 10.0.2.2, so a test of the network
half of the browser never leaves the machine. It is HTTP/1.1 with
keep-alive -- unlike harness.BodyServer, which is 1.0 and closes -- and
it COUNTS CONNECTIONS, which is the only honest oracle for "the client
reused one": a client can claim reuse; the server saw what it saw.

What it serves, beside the files of a corpus directory:

  /<file>                 the file, with a Content-Length
  /<file>?gzip            gzip-encoded (Content-Encoding: gzip)
  /<file>?chunked         Transfer-Encoding: chunked, in 100-byte chunks
  /<file>?close           Connection: close after it
  /<file>?drop            closes after it WITHOUT saying so -- what a
                          kept-alive connection the server timed out
                          looks like to the next request on it
  /redirect/N/<path>      a 302 chain N hops long, then <path>
  /redirect303/<path>     a 303 to <path> (a POST becomes a GET)
  /echo                   the request back: method, path, headers, body
  /cookie/set?k=v         sets cookie k=v for a day (a SESSION cookie is
                          rightly not saved); /cookie/show echoes Cookie:
  /stats                  "connections=N requests=M" since the last /reset
  /reset                  zeroes those counters

HTTPS: make_ca() makes a test CA and, signed by it, certificates for the
gateway address -- a good one, an expired one, and one for the wrong
name -- with openssl. The CA goes in the guest's /etc/ssl/certs; the
server presents whichever leaf a test asks for.

    python3 tools/web_server.py --root data/tests/web --port 18080
    python3 tools/web_server.py --root data/tests/web --tls DIR --port 18443
"""
import argparse
import gzip
import http.server
import os
import socket
import ssl
import subprocess
import sys
import threading
import urllib.parse

GATEWAY = "10.0.2.2"


class _Stats:
    def __init__(self):
        self.lock = threading.Lock()
        self.connections = 0
        self.requests = 0

    def add(self, conns=0, reqs=0):
        with self.lock:
            self.connections += conns
            self.requests += reqs


def _handler(root, stats):
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        timeout = 30

        def setup(self):
            super().setup()
            stats.add(conns=1)

        def log_message(self, *a):
            pass

        def _send(self, status, body=b"", ctype="text/plain; charset=utf-8",
                  headers=(), chunked=False, close=False):
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            for k, v in headers:
                self.send_header(k, v)
            if close:
                self.send_header("Connection", "close")
                self.close_connection = True
            if chunked:
                self.send_header("Transfer-Encoding", "chunked")
                self.end_headers()
                if self.command != "HEAD":
                    for i in range(0, len(body), 100):
                        part = body[i:i + 100]
                        self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
                    self.wfile.write(b"0\r\n\r\n")
                return
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def _body(self):
            n = int(self.headers.get("Content-Length") or 0)
            return self.rfile.read(n) if n else b""

        def _route(self):
            stats.add(reqs=1)
            url = urllib.parse.urlsplit(self.path)
            path, query = url.path, url.query
            if path == "/stats":
                return self._send(200, b"connections=%d requests=%d\n"
                                  % (stats.connections, stats.requests))
            if path == "/reset":
                with stats.lock:
                    stats.connections = stats.requests = 0
                return self._send(200, b"reset\n")
            if path == "/echo":
                body = self._body()
                lines = ["method=%s" % self.command, "path=%s" % self.path]
                lines += ["header %s: %s" % (k, v) for k, v in self.headers.items()]
                lines.append("body=%s" % body.decode("latin-1"))
                return self._send(200, ("\n".join(lines) + "\n").encode())
            if path.startswith("/redirect303/"):
                self._body()
                return self._send(303, headers=[("Location", "/" + path[13:])])
            if path.startswith("/redirect/"):
                rest = path[10:]
                n, _, target = rest.partition("/")
                hops = int(n)
                loc = "/redirect/%d/%s" % (hops - 1, target) if hops > 1 else "/" + target
                return self._send(302, b"moved\n", headers=[("Location", loc)])
            if path == "/cookie/set":
                k, _, v = query.partition("=")
                return self._send(200, b"<html><title>set</title><p>set</p></html>",
                                  "text/html", headers=[("Set-Cookie", "%s=%s; Path=/; Max-Age=86400" % (k, v))])
            if path == "/cookie/show":
                c = self.headers.get("Cookie", "")
                return self._send(200, ("<html><title>cookie %s</title><p>%s</p></html>"
                                        % (c, c)).encode(), "text/html")
            if path == "/ua":
                return self._send(200, (self.headers.get("User-Agent", "") + "\n").encode())
            name = os.path.normpath(path.lstrip("/")) or "index.html"
            full = os.path.join(root, name)
            if name.startswith("..") or not os.path.isfile(full):
                return self._send(404, b"not found\n")
            data = open(full, "rb").read()
            # No charset on HTML: a page's own <meta> decides, as it does
            # for a file: URL -- what browser_hostcheck compares against.
            ctype = {"html": "text/html", "css": "text/css",
                     "gif": "image/gif", "bmp": "image/bmp", "png": "image/png",
                     "jpg": "image/jpeg"}.get(name.rsplit(".", 1)[-1], "application/octet-stream")
            flags = set(query.split("&")) if query else set()
            headers = []
            if "gzip" in flags:
                data = gzip.compress(data)
                headers.append(("Content-Encoding", "gzip"))
            self._send(200, data, ctype, headers, chunked="chunked" in flags,
                       close="close" in flags)
            if "drop" in flags:
                self.close_connection = True

        do_GET = do_HEAD = do_POST = _route

    return Handler


class _Quiet(http.server.ThreadingHTTPServer):
    daemon_threads = True

    # A client that walks away mid-connection -- a test refusing a
    # certificate does exactly that -- is not a server error worth a
    # traceback in the test's output.
    def handle_error(self, request, client_address):
        e = sys.exc_info()[1]
        if not isinstance(e, (ConnectionError, ssl.SSLError, socket.timeout, OSError)):
            super().handle_error(request, client_address)


class WebServer:
    """The server in a thread. `tls` is (cert, key) or None."""

    def __init__(self, root, port=0, tls=None, bind="0.0.0.0"):
        self.stats = _Stats()
        handler = _handler(os.path.abspath(root), self.stats)
        self.srv = _Quiet((bind, port), handler)
        if tls:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(*tls)
            # The handshake in the CONNECTION's thread, not in accept():
            # one client that connects and walks away must not wedge the
            # rest (harness.BodyServer's lesson).
            self.srv.socket = ctx.wrap_socket(self.srv.socket, server_side=True,
                                              do_handshake_on_connect=False)
            orig = handler.setup

            def setup(h):
                h.request.do_handshake()
                orig(h)
            handler.setup = setup
        self.port = self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def counts(self):
        with self.stats.lock:
            return self.stats.connections, self.stats.requests

    def reset(self):
        with self.stats.lock:
            self.stats.connections = self.stats.requests = 0

    def shutdown(self):
        self.srv.shutdown()
        self.srv.server_close()


def _openssl(*args):
    subprocess.run(["openssl", *args], check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)


def make_ca(d, ip=GATEWAY):
    """A test CA and three leaves it signed, in directory `d`. Returns a
    dict of paths: ca, good, expired, wrongname (each leaf a (cert, key)
    pair with the CA appended, so the server presents the CHAIN), plus
    selfsigned. Needs `openssl`."""
    os.makedirs(d, exist_ok=True)
    p = lambda n: os.path.join(d, n)  # noqa: E731
    _openssl("req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
             "-nodes", "-keyout", p("ca.key"), "-out", p("ca.pem"), "-days", "3650",
             "-subj", "/CN=toy-os test CA",
             "-addext", "basicConstraints=critical,CA:TRUE",
             "-addext", "keyUsage=critical,keyCertSign,cRLSign")
    out = {"ca": p("ca.pem")}
    for name, san, days in (("good", "IP:%s,IP:127.0.0.1,DNS:toyos-test,DNS:localhost" % ip, 3650),
                            ("expired", "IP:%s" % ip, -1),
                            ("wrongname", "DNS:not-this-host.example", 3650)):
        _openssl("req", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes",
                 "-keyout", p(name + ".key"), "-out", p(name + ".csr"), "-subj", "/CN=" + name)
        ext = p(name + ".ext")
        with open(ext, "w") as f:
            f.write("subjectAltName=%s\nbasicConstraints=CA:FALSE\n" % san)
        # A negative -days is refused by openssl; an expired leaf is
        # one whose window ended yesterday, made with explicit dates.
        dates = (["-not_before", "20200101000000Z", "-not_after", "20200102000000Z"]
                 if days < 0 else ["-days", str(days)])
        _openssl("x509", "-req", "-in", p(name + ".csr"), "-CA", p("ca.pem"),
                 "-CAkey", p("ca.key"), "-CAcreateserial", "-out", p(name + ".crt"),
                 "-extfile", ext, *dates)
        with open(p(name + ".chain"), "wb") as f:
            f.write(open(p(name + ".crt"), "rb").read() + open(p("ca.pem"), "rb").read())
        out[name] = (p(name + ".chain"), p(name + ".key"))
    _openssl("req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
             "-nodes", "-keyout", p("self.key"), "-out", p("self.pem"), "-days", "3650",
             "-subj", "/CN=selfsigned", "-addext", "subjectAltName=IP:%s" % ip)
    out["selfsigned"] = (p("self.pem"), p("self.key"))
    return out


def free_port():
    s = socket.socket()
    s.bind(("0.0.0.0", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=os.path.join(os.path.dirname(__file__), "..",
                                                   "data", "tests", "web"))
    ap.add_argument("--port", type=int, default=18080)
    ap.add_argument("--tls", metavar="DIR", help="serve https with a test CA made in DIR")
    ap.add_argument("--leaf", default="good", choices=("good", "expired", "wrongname"))
    a = ap.parse_args()
    tls = None
    if a.tls:
        certs = make_ca(a.tls)
        tls = certs[a.leaf]
        print("CA for the guest's /etc/ssl/certs:", certs["ca"])
    srv = WebServer(a.root, a.port, tls)
    print("serving %s on port %d (%s)" % (a.root, srv.port, "https" if tls else "http"))
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        srv.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
