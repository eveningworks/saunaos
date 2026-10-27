// ucookie -- see lib/ucookie.h. Section numbers are RFC 6265's.
#include "lib/ucookie.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define FILE_MAGIC "# SaunaOS cookies 1"

void ucookie_jar_init(struct ucookie_jar *j) { memset(j, 0, sizeof *j); }

static void cookie_free(struct ucookie *c) {
    free(c->name);
    free(c->value);
    free(c->domain);
    free(c->path);
}

void ucookie_jar_free(struct ucookie_jar *j) {
    for (int i = 0; i < j->n; i++) cookie_free(&j->c[i]);
    free(j->c);
    memset(j, 0, sizeof *j);
}

static void remove_at(struct ucookie_jar *j, int i) {
    cookie_free(&j->c[i]);
    j->c[i] = j->c[--j->n];
    j->dirty = 1;
}

// --- 5.1.1: dates -------------------------------------------------------

static int is_delim(int ch) {
    return ch == 0x09 || (ch >= 0x20 && ch <= 0x2F) || (ch >= 0x3B && ch <= 0x40) ||
           (ch >= 0x5B && ch <= 0x60) || (ch >= 0x7B && ch <= 0x7E);
}

// Leading digits of a token, between `min` and `max` of them, with
// anything but a digit allowed after. The value, or -1.
static int digits(const char *t, int n, int min, int max, int *used) {
    int k = 0, v = 0;
    while (k < n && isdigit((unsigned char)t[k])) {
        v = v * 10 + (t[k] - '0');
        k++;
    }
    if (k < min || k > max) return -1;
    if (used) *used = k;
    return v;
}

// Howard Hinnant's days_from_civil: days since 1970-01-01.
static int64_t days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int64_t ucookie_parse_date(const char *s) {
    static const char *const MON[] = { "jan", "feb", "mar", "apr", "may", "jun",
                                       "jul", "aug", "sep", "oct", "nov", "dec" };
    int have_time = 0, have_dom = 0, have_mon = 0, have_year = 0;
    int hh = 0, mm = 0, ss = 0, dom = 0, mon = 0, year = 0;
    while (*s) {
        while (*s && is_delim((unsigned char)*s)) s++;
        const char *t = s;
        while (*s && !is_delim((unsigned char)*s)) s++;
        int n = (int)(s - t);
        if (!n) continue;
        int k1, k2, k3;
        if (!have_time) {
            int h = digits(t, n, 1, 2, &k1);
            if (h >= 0 && k1 < n && t[k1] == ':') {
                int m = digits(t + k1 + 1, n - k1 - 1, 1, 2, &k2);
                if (m >= 0 && k1 + 1 + k2 < n && t[k1 + 1 + k2] == ':') {
                    int sec = digits(t + k1 + k2 + 2, n - k1 - k2 - 2, 1, 2, &k3);
                    if (sec >= 0) {
                        hh = h, mm = m, ss = sec, have_time = 1;
                        continue;
                    }
                }
            }
        }
        if (!have_dom) {
            int d = digits(t, n, 1, 2, NULL);
            if (d >= 0) { dom = d, have_dom = 1; continue; }
        }
        if (!have_mon && n >= 3) {
            int found = 0;
            for (int i = 0; i < 12 && !found; i++)
                if (!strncasecmp(t, MON[i], 3)) mon = i + 1, found = 1;
            if (found) { have_mon = 1; continue; }
        }
        if (!have_year) {
            int y = digits(t, n, 2, 4, NULL);
            if (y >= 0) { year = y, have_year = 1; continue; }
        }
    }
    if (have_year && year >= 70 && year <= 99) year += 1900;
    else if (have_year && year <= 69) year += 2000;
    if (!have_time || !have_dom || !have_mon || !have_year) return -1;
    if (dom < 1 || dom > 31 || year < 1601 || hh > 23 || mm > 59 || ss > 59) return -1;
    return days_from_civil(year, mon, dom) * 86400 + hh * 3600 + mm * 60 + ss;
}

// --- 5.1.3 and 5.1.4: matching -----------------------------------------

static int is_ip(const char *host) {
    if (strchr(host, ':')) return 1;
    for (const char *p = host; *p; p++)
        if (!isdigit((unsigned char)*p) && *p != '.') return 0;
    return 1;
}

static int domain_match(const char *host, const char *domain) {
    if (!strcasecmp(host, domain)) return 1;
    size_t h = strlen(host), d = strlen(domain);
    return h > d && host[h - d - 1] == '.' && !strcasecmp(host + h - d, domain) && !is_ip(host);
}

static int path_match(const char *req, const char *cpath) {
    size_t n = strlen(cpath);
    if (strncmp(req, cpath, n) != 0) return 0;
    return req[n] == '\0' || cpath[n - 1] == '/' || req[n] == '/';
}

static int sends_to(const struct ucookie *c, const char *host) {
    return c->host_only ? !strcasecmp(host, c->domain) : domain_match(host, c->domain);
}

static char *default_path(const char *req) {
    if (!req || req[0] != '/') return strdup("/");
    const char *q = req + strcspn(req, "?#");
    const char *last = NULL;
    for (const char *p = req; p < q; p++) if (*p == '/') last = p;
    if (!last || last == req) return strdup("/");
    return strndup(req, (size_t)(last - req));
}

// --- 5.2 and 5.3: Set-Cookie -------------------------------------------

static char *trim_dup(const char *s, size_t n) {
    while (n && (*s == ' ' || *s == '\t')) s++, n--;
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    return strndup(s, n);
}

static char *lower(char *s) {
    for (char *p = s; p && *p; p++) *p = (char)tolower((unsigned char)*p);
    return s;
}

// A control character in a name or value: refused (RFC 6265bis 5.6),
// which also keeps the jar's tab-separated file unambiguous.
static int has_ctl(const char *s) {
    for (; *s; s++) if ((unsigned char)*s < 0x20 || *s == 0x7F) return 1;
    return 0;
}

// Makes room under the limits, oldest-used first: expired ones go, then
// `domain`'s beyond UCOOKIE_PER_DOMAIN, then the jar's beyond the total.
static void evict(struct ucookie_jar *j, const char *domain, int64_t now) {
    for (int i = 0; i < j->n;)
        if (j->c[i].expires >= 0 && j->c[i].expires <= now) remove_at(j, i);
        else i++;
    for (int pass = 0; pass < 2; pass++) {
        for (;;) {
            int count = 0, lru = -1;
            for (int i = 0; i < j->n; i++) {
                if (pass == 0 && strcasecmp(j->c[i].domain, domain)) continue;
                count++;
                if (lru < 0 || j->c[i].last_used < j->c[lru].last_used) lru = i;
            }
            if (count < (pass == 0 ? UCOOKIE_PER_DOMAIN : UCOOKIE_TOTAL) || lru < 0) break;
            remove_at(j, lru);
        }
    }
}

static int insert(struct ucookie_jar *j, struct ucookie *nc, int64_t now) {
    // The same name, domain, host-only-ness and path replace the old one,
    // keeping its creation time (5.3 step 11).
    for (int i = 0; i < j->n; i++) {
        struct ucookie *o = &j->c[i];
        if (!strcmp(o->name, nc->name) && !strcasecmp(o->domain, nc->domain) &&
            o->host_only == nc->host_only && !strcmp(o->path, nc->path)) {
            nc->created = o->created;
            remove_at(j, i);
            break;
        }
    }
    if (nc->expires >= 0 && nc->expires <= now) {   // an expiry in the past deletes
        cookie_free(nc);
        j->dirty = 1;
        return 1;
    }
    evict(j, nc->domain, now);
    if (j->n == j->cap) {
        int cap = j->cap ? j->cap * 2 : 32;
        struct ucookie *g = realloc(j->c, (size_t)cap * sizeof *g);
        if (!g) { cookie_free(nc); return 0; }
        j->c = g;
        j->cap = cap;
    }
    j->c[j->n++] = *nc;
    j->dirty = 1;
    return 1;
}

int ucookie_set(struct ucookie_jar *j, const char *host, const char *path, int secure,
                const char *sc, int64_t now) {
    if (!host || !*host || !sc) return 0;
    size_t nv = strcspn(sc, ";");
    const char *eq = memchr(sc, '=', nv);
    if (!eq) return 0;
    struct ucookie c = { 0 };
    c.name = trim_dup(sc, (size_t)(eq - sc));
    c.value = trim_dup(eq + 1, nv - (size_t)(eq + 1 - sc));
    c.expires = -1;
    c.created = c.last_used = now;
    int64_t max_age = 0, expires = -1;
    int have_max_age = 0;
    char *domain = NULL;
    if (!c.name || !c.value || !c.name[0] || has_ctl(c.name) || has_ctl(c.value) ||
        strlen(c.name) + strlen(c.value) > UCOOKIE_MAX_BYTES)
        goto refuse;

    for (const char *a = sc + nv; *a == ';';) {
        a++;
        size_t len = strcspn(a, ";");
        const char *ae = memchr(a, '=', len);
        char *an = trim_dup(a, ae ? (size_t)(ae - a) : len);
        char *av = ae ? trim_dup(ae + 1, len - (size_t)(ae + 1 - a)) : strdup("");
        if (an && av) {
            if (!strcasecmp(an, "expires")) {
                int64_t t = ucookie_parse_date(av);
                if (t >= 0) expires = t;
            } else if (!strcasecmp(an, "max-age")) {
                const char *p = av;
                int neg = *p == '-';
                if (neg) p++;
                if (isdigit((unsigned char)*p)) {
                    int64_t v = 0;
                    for (; isdigit((unsigned char)*p); p++)
                        if (v < (int64_t)1 << 40) v = v * 10 + (*p - '0');
                    if (!*p) {
                        max_age = neg ? -v : v;
                        have_max_age = 1;
                    }
                }
            } else if (!strcasecmp(an, "domain") && av[0]) {
                free(domain);
                domain = lower(strdup(av[0] == '.' ? av + 1 : av));
            } else if (!strcasecmp(an, "path")) {
                free(c.path);
                c.path = av[0] == '/' ? strdup(av) : NULL;
            } else if (!strcasecmp(an, "secure")) {
                c.secure = 1;
            } else if (!strcasecmp(an, "httponly")) {
                c.http_only = 1;
            }
        }
        free(an);
        free(av);
        a += len;
    }

    if (have_max_age) c.expires = max_age <= 0 ? 0 : now + max_age;
    else if (expires >= 0) c.expires = expires;

    char *h = lower(strdup(host));
    if (!h) goto refuse;
    if (domain && domain[0]) {
        int suffix = j->is_public_suffix ? j->is_public_suffix(domain) : strchr(domain, '.') == NULL;
        if (suffix && strcmp(domain, h) != 0) { free(h); goto refuse; }
        if (suffix) {
            c.host_only = 1;
            c.domain = h;
        } else if (!domain_match(h, domain)) {
            free(h);
            goto refuse;
        } else {
            c.domain = domain;
            domain = NULL;
            free(h);
        }
    } else {
        c.host_only = 1;
        c.domain = h;
    }
    if (!c.path) c.path = default_path(path);
    // A Secure cookie only from a secure origin (RFC 6265bis 5.6 step 14).
    if (!c.domain || !c.path || (c.secure && !secure)) goto refuse;
    free(domain);
    return insert(j, &c, now);
refuse:
    free(domain);
    cookie_free(&c);
    return 0;
}

// --- 5.4: the Cookie header ------------------------------------------------

struct pick { const struct ucookie *c; };

static int pick_cmp(const void *a, const void *b) {
    const struct ucookie *x = ((const struct pick *)a)->c, *y = ((const struct pick *)b)->c;
    size_t lx = strlen(x->path), ly = strlen(y->path);
    if (lx != ly) return lx > ly ? -1 : 1;
    return x->created < y->created ? -1 : x->created > y->created;
}

size_t ucookie_header(struct ucookie_jar *j, const char *host, const char *path, int secure,
                      int64_t now, char *out, size_t cap) {
    if (!cap) return 0;
    out[0] = '\0';
    if (!j->n) return 0;
    struct pick *p = malloc((size_t)j->n * sizeof *p);
    if (!p) return 0;
    const char *req = path && path[0] == '/' ? path : "/";
    int n = 0;
    for (int i = 0; i < j->n; i++) {
        struct ucookie *c = &j->c[i];
        if (c->expires >= 0 && c->expires <= now) continue;
        if (!sends_to(c, host) || !path_match(req, c->path) || (c->secure && !secure)) continue;
        p[n++].c = c;
    }
    qsort(p, (size_t)n, sizeof *p, pick_cmp);
    size_t len = 0;
    for (int i = 0; i < n; i++) {
        int k = snprintf(out + len, cap - len, "%s%s=%s", i ? "; " : "", p[i].c->name, p[i].c->value);
        if (k < 0 || (size_t)k >= cap - len) { out[0] = '\0'; free(p); return 0; }
        len += (size_t)k;
        ((struct ucookie *)p[i].c)->last_used = now;
    }
    free(p);
    return len;
}

int ucookie_count(const struct ucookie_jar *j, const char *host, int64_t now) {
    int n = 0;
    for (int i = 0; i < j->n; i++)
        if ((j->c[i].expires < 0 || j->c[i].expires > now) && sends_to(&j->c[i], host)) n++;
    return n;
}

int ucookie_clear(struct ucookie_jar *j, const char *host) {
    int n = 0;
    for (int i = 0; i < j->n;)
        if (sends_to(&j->c[i], host)) { remove_at(j, i); n++; }
        else i++;
    return n;
}

// --- the files ----------------------------------------------------------------

// Splits `line` in place at tabs into at most `max` fields.
static int split_tabs(char *line, char **f, int max) {
    int n = 0;
    line[strcspn(line, "\r\n")] = '\0';
    for (char *p = line; n < max;) {
        f[n++] = p;
        char *t = strchr(p, '\t');
        if (!t) break;
        *t = '\0';
        p = t + 1;
    }
    return n;
}

static int add_loaded(struct ucookie_jar *j, const char *name, const char *value, const char *domain,
                      const char *path, int host_only, int secure, int http_only, int64_t expires,
                      int64_t created, int64_t last_used, int64_t now) {
    if (!name[0] || expires <= now || !domain[0] || path[0] != '/') return 0;
    struct ucookie c = { 0 };
    c.name = strdup(name);
    c.value = strdup(value);
    c.domain = lower(strdup(domain));
    c.path = strdup(path);
    c.host_only = host_only;
    c.secure = secure;
    c.http_only = http_only;
    c.expires = expires;
    c.created = created;
    c.last_used = last_used;
    if (!c.name || !c.value || !c.domain || !c.path || has_ctl(c.value)) {
        cookie_free(&c);
        return 0;
    }
    return insert(j, &c, now);
}

int ucookie_load(struct ucookie_jar *j, const char *path, int64_t now) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    static char line[UCOOKIE_MAX_BYTES + 1024];
    int ok = fgets(line, sizeof line, fp) && !strncmp(line, FILE_MAGIC, strlen(FILE_MAGIC));
    while (ok && fgets(line, sizeof line, fp)) {
        char *f[10];
        if (split_tabs(line, f, 10) != 10) continue;
        add_loaded(j, f[8], f[9], f[0], f[2], atoi(f[1]), atoi(f[3]), atoi(f[4]),
                   strtoll(f[5], NULL, 10), strtoll(f[6], NULL, 10), strtoll(f[7], NULL, 10), now);
    }
    fclose(fp);
    j->dirty = 0;
    return ok ? 0 : -1;
}

int ucookie_save(struct ucookie_jar *j, const char *path, int64_t now) {
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *fp = fopen(tmp, "w");
    if (!fp) return -1;
    fprintf(fp, "%s\n", FILE_MAGIC);
    for (int i = 0; i < j->n; i++) {
        const struct ucookie *c = &j->c[i];
        if (c->expires < 0 || c->expires <= now) continue;   // session and expired
        fprintf(fp, "%s\t%d\t%s\t%d\t%d\t%lld\t%lld\t%lld\t%s\t%s\n", c->domain, c->host_only,
                c->path, c->secure, c->http_only, (long long)c->expires, (long long)c->created,
                (long long)c->last_used, c->name, c->value);
    }
    int bad = ferror(fp);
    if (fclose(fp) != 0 || bad || rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    j->dirty = 0;
    return 0;
}

int ucookie_import_netsurf(struct ucookie_jar *j, const char *path, int64_t now) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    static char line[UCOOKIE_MAX_BYTES + 2048];
    int n = 0;
    while (fgets(line, sizeof line, fp)) {
        // version domain domain_from_set path path_from_set secure
        // http_only expires last_used no_destroy name value
        // value_was_quoted scheme url comment
        char *f[16];
        if (line[0] == '#' || split_tabs(line, f, 16) < 13) continue;
        const char *domain = f[1];
        int host_only = domain[0] != '.';
        if (!host_only) domain++;
        static char value[UCOOKIE_MAX_BYTES + 3];
        snprintf(value, sizeof value, atoi(f[12]) ? "\"%s\"" : "%s", f[11]);
        int64_t last = strtoll(f[8], NULL, 10);
        n += add_loaded(j, f[10], value, domain, f[3], host_only, atoi(f[5]), atoi(f[6]),
                        strtoll(f[7], NULL, 10), last, last, now);
    }
    fclose(fp);
    return n;
}
