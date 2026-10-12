// What the NetSurf port needed from the C library (docs/browser-design.md,
// stage 0): iconv, inet_pton/inet_ntop, realpath, scandir, the POSIX
// clocks, strtof, the exact float maths, and the small ones.
//
// EVERY EXPECTED VALUE CAME FROM THE HOST'S PYTHON -- `ipaddress` for
// the IPv6 text forms (RFC 5952's canonical one, ties to the FIRST run),
// the `codecs` for every byte sequence -- not from running this code.
//
// The checks a broken version would still pass are the ones written
// out: a REJECTED parse (inet_pton must refuse "1.2.3", which the
// pre-POSIX inet_aton accepts; iconv must refuse an overlong UTF-8),
// and an iconv error that leaves *inbuf AT the offending character,
// which is what lets a caller recover.
//
// Prints one line per check and exits with the number of failures.
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <iconv.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "lib/utest.h"

// Converts `in` (n bytes) with a fresh descriptor; the bytes out into
// `out`, and the return value, errno and how much input was left.
struct conv { size_t ret; int err; size_t out_len; size_t in_left; char out[64]; };

static struct conv convert(const char *to, const char *from, const char *in, size_t n, size_t room) {
    struct conv c;
    memset(&c, 0, sizeof c);
    iconv_t cd = iconv_open(to, from);
    if (cd == (iconv_t)-1) { c.ret = (size_t)-2; c.err = errno; return c; }
    char *ip = (char *)in, *op = c.out;
    size_t il = n, ol = room;
    errno = 0;
    c.ret = iconv(cd, &ip, &il, &op, &ol);
    c.err = errno;
    c.out_len = (size_t)(op - c.out);
    c.in_left = il;
    iconv_close(cd);
    return c;
}

static int bytes_are(const struct conv *c, const char *hex) {
    char got[160] = "";
    for (size_t i = 0; i < c->out_len; i++) sprintf(got + 2 * i, "%02x", (unsigned char)c->out[i]);
    return !strcmp(got, hex);
}

static int v6_round_trip(const char *text, const char *canonical, const char *hex) {
    unsigned char b[16];
    if (inet_pton(AF_INET6, text, b) != 1) return 0;
    char h[33];
    for (int i = 0; i < 16; i++) sprintf(h + 2 * i, "%02x", b[i]);
    if (strcmp(h, hex)) return 0;
    char out[INET6_ADDRSTRLEN];
    return inet_ntop(AF_INET6, b, out, sizeof out) && !strcmp(out, canonical);
}

static int only_b(const struct dirent *e) { return e->d_name[0] == 'b'; }

int main(void) {
    utest_begin("libc6_test", "iconv, inet_pton, realpath, scandir, clocks, strtof", UTEST_VERDICT_FILE);

    // --- iconv --------------------------------------------------------
    const char *eacute_euro = "\xc3\xa9\xe2\x82\xac";       // "é€" in UTF-8
    struct conv c = convert("ISO-8859-1", "UTF-8", eacute_euro, 5, 64);
    utest_check(c.ret == (size_t)-1 && c.err == EILSEQ, "iconv: the euro sign has no ISO-8859-1 byte -> EILSEQ");
    utest_check(bytes_are(&c, "e9") && c.in_left == 3,
                "iconv: ...after converting the e-acute, with *inbuf left AT the euro sign");
    c = convert("latin_1//TRANSLIT", "utf8", eacute_euro, 5, 64);
    utest_check(c.ret == 1 && bytes_are(&c, "e93f"), "iconv: //TRANSLIT writes '?' and counts 1 irreversible");
    c = convert("ISO-8859-1//IGNORE", "UTF-8", eacute_euro, 5, 64);
    utest_check(c.ret == 1 && bytes_are(&c, "e9"), "iconv: //IGNORE drops it and counts 1");
    c = convert("WINDOWS-1252", "UTF-8", eacute_euro, 5, 64);
    utest_check(c.ret == 0 && bytes_are(&c, "e980"), "iconv: Windows-1252 has the euro, at 0x80");
    c = convert("UTF-8", "CP1252", "\x80\x81", 2, 64);
    utest_check(c.err == EILSEQ && bytes_are(&c, "e282ac") && c.in_left == 1,
                "iconv: CP1252's undefined 0x81 is EILSEQ, after 0x80 became the euro");
    c = convert("UTF-16", "UTF-8", "\xc3\xa9", 2, 64);
    utest_check(c.ret == 0 && bytes_are(&c, "feff00e9"), "iconv: UTF-16 out writes a BE mark, then BE");
    c = convert("UTF-16BE", "UTF-8", "\xf0\x9f\x98\x80", 4, 64);
    utest_check(bytes_are(&c, "d83dde00"), "iconv: U+1F600 becomes a surrogate pair");
    c = convert("UTF-8", "UTF-16", "\xff\xfe\xe9\x00", 4, 64);
    utest_check(c.ret == 0 && bytes_are(&c, "c3a9"), "iconv: a UTF-16 LE mark is read and dropped");
    c = convert("UTF-8", "UTF-16", "\x00\xe9", 2, 64);
    utest_check(bytes_are(&c, "c3a9"), "iconv: UTF-16 with no mark reads big-endian");
    c = convert("UTF-16LE", "UTF-8", "\xc0\x80", 2, 64);
    utest_check(c.err == EILSEQ && c.in_left == 2, "iconv: an overlong UTF-8 NUL is EILSEQ, not U+0000");
    // Three bytes for '/', which fits in one: the lead-byte table cannot
    // catch this (E0 is a valid lead), only the decoded value's minimum.
    c = convert("UTF-16LE", "UTF-8", "\xe0\x80\xaf", 3, 64);
    utest_check(c.err == EILSEQ && c.in_left == 3, "iconv: an overlong 3-byte '/' is EILSEQ");
    c = convert("UTF-16LE", "UTF-8", "\xed\xa0\x80", 3, 64);
    utest_check(c.err == EILSEQ, "iconv: a UTF-8-encoded surrogate is EILSEQ");
    c = convert("ISO-8859-1", "UTF-8", "a\xe2\x82", 3, 64);
    utest_check(c.err == EINVAL && bytes_are(&c, "61") && c.in_left == 2,
                "iconv: input ending inside a character is EINVAL, *inbuf at its first byte");
    c = convert("UTF-8", "ISO-8859-1", "\xe9\xe9", 2, 3);
    utest_check(c.err == E2BIG && c.out_len == 2 && c.in_left == 1, "iconv: no room is E2BIG, written whole or not at all");
    c = convert("KOI8-R", "UTF-8", "a", 1, 64);
    utest_check(c.ret == (size_t)-2 && c.err == EINVAL, "iconv_open: a charset it lacks fails at OPEN, with EINVAL");
    c = convert("UTF-8//BOGUS", "UTF-8", "a", 1, 64);
    utest_check(c.ret == (size_t)-2 && c.err == EINVAL, "iconv_open: an unknown //suffix is refused");

    // --- inet_pton / inet_ntop ---------------------------------------
    unsigned char a4[4];
    utest_check(inet_pton(AF_INET, "192.168.0.1", a4) == 1 && a4[0] == 192 && a4[3] == 1,
                "inet_pton: 192.168.0.1, network order");
    utest_check(inet_pton(AF_INET, "1.2.3", a4) == 0, "inet_pton: refuses inet_aton's short form 1.2.3");
    utest_check(inet_pton(AF_INET, "01.2.3.4", a4) == 0, "inet_pton: refuses a leading zero (not octal, not decimal)");
    utest_check(inet_pton(AF_INET, "256.1.1.1", a4) == 0, "inet_pton: refuses a part over 255");
    utest_check(inet_pton(AF_INET, "1.2.3.4 ", a4) == 0, "inet_pton: refuses trailing text");
    utest_check(v6_round_trip("2001:db8::1", "2001:db8::1", "20010db8000000000000000000000001"),
                "inet_pton/ntop: 2001:db8::1");
    utest_check(v6_round_trip("::ffff:1.2.3.4", "::ffff:1.2.3.4", "00000000000000000000ffff01020304"),
                "inet_pton/ntop: a v4-mapped address keeps its dotted tail");
    utest_check(v6_round_trip("::", "::", "00000000000000000000000000000000"), "inet_pton/ntop: ::");
    utest_check(v6_round_trip("1::", "1::", "00010000000000000000000000000000"), "inet_pton/ntop: 1::");
    utest_check(v6_round_trip("2001:0DB8:0:0:1:0:0:1", "2001:db8::1:0:0:1", "20010db8000000000001000000000001"),
                "inet_ntop: a tie between zero runs takes the FIRST, lower case");
    utest_check(v6_round_trip("1:0:0:2:0:0:0:3", "1:0:0:2::3", "00010000000000020000000000000003"),
                "inet_ntop: the LONGEST zero run becomes ::");
    unsigned char a6[16];
    utest_check(inet_pton(AF_INET6, "1::2::3", a6) == 0, "inet_pton: refuses two ::");
    utest_check(inet_pton(AF_INET6, "1:2:3:4:5:6:7:8:9", a6) == 0, "inet_pton: refuses nine groups");
    utest_check(inet_pton(AF_INET6, "12345::", a6) == 0, "inet_pton: refuses a five-digit group");
    errno = 0;
    utest_check(inet_pton(99, "x", a6) == -1 && errno == EAFNOSUPPORT, "inet_pton: another family is -1, EAFNOSUPPORT");
    char small[4];
    errno = 0;
    utest_check(!inet_ntop(AF_INET, a4, small, sizeof small) && errno == ENOSPC,
                "inet_ntop: a buffer too small is NULL with ENOSPC");

    // --- realpath / scandir -------------------------------------------
    char *rp = realpath("/bin/../bin/./", NULL);
    utest_check(rp && !strcmp(rp, "/bin"), "realpath: /bin/../bin/./ is /bin, malloc'd");
    free(rp);
    static char buf[PATH_MAX];   // static: PATH_MAX is twice the ring-3 frame budget
    errno = 0;
    utest_check(!realpath("/no/such/path", buf) && errno == ENOENT, "realpath: a missing path is NULL, ENOENT");
    static char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof cwd) && chdir("/") == 0) {
        utest_check(realpath("bin", buf) && !strcmp(buf, "/bin"), "realpath: a relative path resolves against the cwd");
        chdir(cwd);
    }
    struct dirent **list;
    int n = scandir("/", &list, NULL, alphasort);
    int sorted = n > 1;
    for (int i = 1; i < n; i++)
        if (strcmp(list[i - 1]->d_name, list[i]->d_name) > 0) sorted = 0;
    utest_checkf(sorted, "scandir: / sorted by alphasort (%d entries)", n);
    for (int i = 0; i < n; i++) free(list[i]);
    if (n >= 0) free(list);
    n = scandir("/", &list, only_b, alphasort);
    int all_b = n >= 2;     // /bin and /boot at least
    for (int i = 0; i < n; i++) {
        if (list[i]->d_name[0] != 'b') all_b = 0;
        free(list[i]);
    }
    if (n >= 0) free(list);
    utest_checkf(all_b, "scandir: the filter keeps only names starting 'b' (%d)", n);
    errno = 0;
    utest_check(scandir("/no/such/dir", &list, NULL, NULL) == -1 && errno, "scandir: a missing directory is -1");

    // --- clocks ---------------------------------------------------------
    struct timespec t1, t2, w1, w2, res;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    clock_gettime(CLOCK_REALTIME, &w1);
    struct timespec nap = { 0, 20 * 1000000L };
    int slept = nanosleep(&nap, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t2);
    clock_gettime(CLOCK_REALTIME, &w2);
    long long dt = (long long)(t2.tv_sec - t1.tv_sec) * 1000000000LL + (t2.tv_nsec - t1.tv_nsec);
    long long dw = (long long)(w2.tv_sec - w1.tv_sec) * 1000000000LL + (w2.tv_nsec - w1.tv_nsec);
    utest_checkf(slept == 0 && dt >= 20000000LL,
                 "nanosleep(20 ms) returned %d, then CLOCK_MONOTONIC moved %lld ns, >= 20 ms", slept, dt);
    // The point of the realtime clock carrying nanoseconds: NetSurf's
    // scheduler times 10 ms callbacks on gettimeofday(), and a clock in
    // whole seconds made every one of them wait up to a second.
    utest_checkf(dw >= 19000000LL && dw < 1000000000LL,
                 "CLOCK_REALTIME moved %lld ns across the same sleep -- finer than a second", dw);
    utest_check(clock_getres(CLOCK_REALTIME, &res) == 0 && res.tv_sec == 0, "clock_getres: REALTIME is sub-second");
    struct timeval tv;
    time_t now = time(NULL);
    utest_check(gettimeofday(&tv, NULL) == 0 && tv.tv_sec - now <= 1 && tv.tv_sec >= now &&
                tv.tv_usec >= 0 && tv.tv_usec < 1000000,
                "gettimeofday: agrees with time(), microseconds in range");
    errno = 0;
    utest_check(clock_gettime(7, &t1) == -1 && errno == EINVAL, "clock_gettime: an unknown clock is EINVAL");

    // --- numbers --------------------------------------------------------
    char *end;
    utest_check(strtof("1.5x", &end) == 1.5f && *end == 'x', "strtof: 1.5, stopping at x");
    utest_check(strtof("0.1", NULL) == 0.1f, "strtof: 0.1 rounds to the float nearest it");
    errno = 0;
    utest_check(isinf(strtof("3.5e38", NULL)) && errno == ERANGE, "strtof: past FLT_MAX is inf, ERANGE");
    utest_check(floorf(-1.5f) == -2.0f && ceilf(-1.5f) == -1.0f && roundf(2.5f) == 3.0f && truncf(-2.7f) == -2.0f,
                "floorf/ceilf/roundf/truncf");
    utest_check(sqrtf(2.0f) == 1.41421354f && fabsf(-3.0f) == 3.0f && fmodf(7.0f, 3.0f) == 1.0f,
                "sqrtf/fabsf/fmodf");
    utest_check(strncasecmp("NetSurf", "netsURF browser", 7) == 0, "strncasecmp: reachable from <string.h>");
    utest_check(isascii(0x7f) && !isascii(0x80), "isascii");
    utest_check(EXIT_SUCCESS == 0 && EXIT_FAILURE == 1, "the exit-status macros are 0 and 1");  // not named: the runner forbids that word

    fd_set s;
    FD_ZERO(&s);
    FD_SET(3, &s);
    FD_SET(200, &s);
    utest_check(FD_ISSET(3, &s) && FD_ISSET(200, &s) && !FD_ISSET(4, &s), "fd_set: FD_SET / FD_ISSET");
    FD_CLR(200, &s);
    utest_check(!FD_ISSET(200, &s), "fd_set: FD_CLR");

    return utest_end();
}
