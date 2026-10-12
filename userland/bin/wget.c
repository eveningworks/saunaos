// wget -- fetch a URL over HTTP or HTTPS.
//
// The protocol lives in /lib/libhttp.so (<uhttp.h>), not here: there
// were about to be three copies of a URL parser and a header scan --
// this, /bin/httpd, and the /bin/update that docs/update-design.md
// designs -- and the part that is easy to get wrong is invisible in all
// three (a stream read returns at most SYS_NET_STREAM_READ_MAX bytes,
// so the blank line ending the headers can straddle any two reads).
//
// What is left here is the FRONT END: arguments, where the body goes,
// and how a failure is worded.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include "lib/cmd.h"
#include "lib/uargs.h"
#include "lib/uprogress.h"
#include "uhttp.h"

// A URL with no scheme is tried as https FIRST and falls back to
// http only when 443 does not answer -- never on a certificate or
// handshake failure. See <uhttp.h>.

static int f_insecure, f_weak, f_server;
static const char *f_out, *f_max_redirect, *f_post, *f_header;

static const struct uargs_opt OPTS[] = {
    { "output-document", 'O', "FILE", "write the body to FILE instead of standard output", 0, &f_out },
    { "no-check-certificate", 'k', 0, "do not verify the server's certificate (https only)", &f_insecure, 0 },
    { "weak-entropy", 0, 0, "key a TLS connection even from TSC jitter", &f_weak, 0 },
    { "max-redirect", 0, "N", "follow at most N redirects (default 20; 0 follows none)", 0, &f_max_redirect },
    { "server-response", 'S', 0, "print the response's status line and headers", &f_server, 0 },
    { "post-data", 0, "STRING", "send a POST with STRING as its form-encoded body", 0, &f_post },
    { "header", 0, "LINE", "add the request header LINE (\"Name: value\")", 0, &f_header },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "wget",
    .usage = "[OPTION]... URL...",
    .summary = "Fetch URLs over HTTP or HTTPS and print them, or save them with -O.",
    .opts = OPTS,
    .notes = "Several URLs are fetched in turn, over one kept-alive connection per host.\n"
             "A URL with no scheme tries https first and falls back to http only when\n"
             "port 443 does not answer.",
};

struct out {
    int  fd;          // -1 means stdout
    int  insecure;    // for the warning printed on connect
    struct uprogress prog;
};

static int sink(void *ctx, const void *data, size_t len) {
    struct out *o = ctx;
    const char *p = data;
    size_t len_in = len;
    while (len) {
        long w = (o->fd < 0) ? (long)fwrite(p, 1, len, stdout)
                             : write(o->fd, p, len);
        if (w <= 0) return -1;
        p += w;
        len -= (size_t)w;
    }
    uprogress_add(&o->prog, len_in);
    return 0;
}

// Printed before the body so a response from an unexpected host is
// attributable, which is why the address is shown rather than only the
// name. On https it also names the protocol version and the suite --
// the two facts that say what the encryption actually is.
static void on_connect(void *ctx, uint32_t ip, const char *ver, const char *cipher) {
    struct out *o = ctx;
    printf("connecting to %u.%u.%u.%u",
           (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    if (ver) printf(" -- %s, %s", ver, cipher ? cipher : "?");
    printf("\n");
    if (ver && o->insecure)
        printf("wget: WARNING: the server's identity was NOT verified\n");
}

// A downgrade the user did not ask for must not be silent: they typed a
// bare host, so they never chose plaintext, and the line that says so is
// the only chance they get to notice.
static void on_fallback(void *ctx, const char *host) {
    (void)ctx;
    printf("wget: no https on %s, falling back to http (not encrypted)\n", host);
}

// -S: what the server said, indented as GNU wget prints it. On stdout,
// before the body, so a captured run reads top to bottom.
static void on_header(void *ctx, const char *line, size_t len) {
    (void)ctx;
    printf("  %.*s\n", (int)len, line);
}

// THE METER GOES TO fd 2, NEVER fd 1: without -O the body IS stdout,
// so a meter there would corrupt every redirected download. It draws
// only when fd 2 is a terminal, which is what keeps a captured stderr
// free of repaint junk (GNU wget and curl both do this).
static void on_headers(void *ctx, int status, unsigned long content_length) {
    struct out *o = ctx;
    if (status >= 200 && status <= 299)
        uprogress_begin(&o->prog, 2, content_length);
}

// One URL. 0, or 1 for a failure or a non-2xx status.
static int fetch(struct uhttp_request *req, struct out *o, const char *url) {
    req->url = url;
    req->sink_ctx = o;
    o->prog = (struct uprogress){ .fd = -1 };
    int rc = uhttp_fetch(req);
    uprogress_end(&o->prog);
    if (rc != 0) {
        printf("wget: %s\n", req->err);
        // The library states the condition; naming the flag that
        // overrides it is this program's job, since the flag is this
        // program's.
        if (!req->allow_weak_entropy && strstr(req->err, "randomness"))
            printf("wget: pass --weak-entropy to accept it anyway\n");
        return 1;
    }
    if (o->fd >= 0) printf("saved %lu bytes to %s\n", req->body_bytes, f_out);
    // A non-2xx status is REPORTED and the body still printed, because
    // an error page is usually the explanation. The exit status is what
    // a script reads.
    if (req->status < 200 || req->status > 299) {
        printf("wget: server returned status %d\n", req->status);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    if (a.argc < 1) return uargs_error(&PROG, "missing URL");

    struct uhttp_request req;
    memset(&req, 0, sizeof req);
    req.insecure = f_insecure != 0;
    req.allow_weak_entropy = f_weak != 0;
    // GNU wget's default, and the reason: most https URLs now answer a
    // first request with a redirect.
    req.max_redirects = 20;
    if (f_max_redirect) {
        char *end;
        long n = strtol(f_max_redirect, &end, 10);
        if (*end || n < 0 || n > 100)
            return uargs_error(&PROG, "--max-redirect wants a number from 0 to 100");
        req.max_redirects = (int)n;
    }
    const char *headers[3] = { 0 };
    int nh = 0;
    if (f_header) headers[nh++] = f_header;
    if (f_post) {
        headers[nh++] = "Content-Type: application/x-www-form-urlencoded";
        req.method = "POST";
        req.body = f_post;
        req.body_len = strlen(f_post);
    }
    req.headers = headers;
    req.sink = sink;
    req.on_connect = on_connect;
    req.on_fallback = on_fallback;
    req.on_headers = on_headers;
    if (f_server) req.on_header = on_header;

    struct out o = { .fd = -1, .insecure = req.insecure };
    if (f_out) {
        // Several URLs into one -O file are concatenated, as GNU's are.
        o.fd = open(f_out, O_WRONLY | O_CREAT | O_TRUNC);
        if (o.fd < 0) { cmd_fail("wget", f_out); return 1; }
    }
    // A pool only when there is a second request to reuse it.
    if (a.argc > 1) req.pool = uhttp_pool_new(NULL);

    int failed = 0;
    for (int i = 0; i < a.argc; i++) failed |= fetch(&req, &o, a.argv[i]);

    if (req.pool) uhttp_pool_free(req.pool);
    if (o.fd >= 0) close(o.fd);
    return failed;
}
