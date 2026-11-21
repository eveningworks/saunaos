// A kernel-trace record as one line, and the group names -- see
// api/ktrace_fmt.h. COMPILED INTO BOTH RINGS (the kernel's panic tail,
// libbase for /bin/trace), so it may use only the toolkit: no heap, no
// locks, nothing kernel-only. At a panic it runs with the other cores
// stopped and the heap possibly the reason why.
#include <stdarg.h>
#include "ktrace_fmt.h"
#include "kfmt.h"
#include "string.h"

#define ROW(id_, name_, group_, args_) { (id_), (group_), (name_), (args_) },
static const struct ktrace_event_desc g_events[] = { KTRACE_EVENTS(ROW) };
#undef ROW

const struct ktrace_event_desc *ktrace_event_table(unsigned *n) {
    if (n) *n = sizeof g_events / sizeof g_events[0];
    return g_events;
}

const struct ktrace_event_desc *ktrace_event_desc(unsigned id) {
    for (unsigned i = 0; i < sizeof g_events / sizeof g_events[0]; i++)
        if (g_events[i].id == id) return &g_events[i];
    return 0;
}

static char state_letter(uint64_t s) {
    switch (s) {
    case KTRACE_STATE_UNUSED:  return '-';   // the kernel, or an idle core
    case KTRACE_STATE_READY:   return 'R';
    case KTRACE_STATE_RUNNING: return 'R';
    case KTRACE_STATE_ZOMBIE:  return 'Z';
    case KTRACE_STATE_BLOCKED: return 'S';
    default:                   return '?';
    }
}

// Appends to out[*len..cap); 0 (and *len untouched past the fit) when full.
static int put(char *out, size_t cap, size_t *len, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
static int put(char *out, size_t cap, size_t *len, const char *fmt, ...) {
    if (*len >= cap) return 0;
    va_list ap;
    va_start(ap, fmt);
    size_t n = k_vsnprintf(out + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (n >= cap - *len) return 0;
    *len += n;
    return 1;
}

// ticks as "12.345 us": the unit that keeps it short.
static int put_ticks(char *out, size_t cap, size_t *len, uint64_t t, uint64_t hz) {
    if (!hz) return put(out, cap, len, "%llu ticks", (unsigned long long)t);
    uint64_t ns = t / hz * 1000000000ull + t % hz * 1000000000ull / hz;
    if (ns < 10000) return put(out, cap, len, "%llu ns", (unsigned long long)ns);
    if (ns < 10000000)
        return put(out, cap, len, "%llu.%03llu us", (unsigned long long)(ns / 1000),
                   (unsigned long long)(ns % 1000));
    return put(out, cap, len, "%llu.%03llu ms", (unsigned long long)(ns / 1000000),
               (unsigned long long)(ns / 1000 % 1000));
}

static int put_arg(char *out, size_t cap, size_t *len, const char *name, size_t name_len,
                   char kind, uint64_t v, const struct ktrace_fmt_ctx *c) {
    if (!put(out, cap, len, " %.*s=", (int)name_len, name)) return 0;
    switch (kind) {
    case 'd': return put(out, cap, len, "%lld", (long long)v);
    case 's': return put(out, cap, len, "%c", state_letter(v));
    case 'v': return put(out, cap, len, "%llu", (unsigned long long)v);
    case 't': return put_ticks(out, cap, len, v, c ? c->tsc_hz : 0);
    case 'n': {
        const char *nm = c && c->syscall_name ? c->syscall_name((unsigned)v) : 0;
        if (nm) return put(out, cap, len, "%s", nm);
        return put(out, cap, len, "%llu", (unsigned long long)v);
    }
    default:  return put(out, cap, len, "%#llx", (unsigned long long)v);
    }
}

int ktrace_fmt_rec(char *out, size_t cap, const struct ktrace_rec *r,
                   const struct ktrace_fmt_ctx *c) {
    if (!out || !cap || !r) return 0;
    size_t len = 0;
    uint64_t t0 = c ? c->t0_tsc : 0, hz = c ? c->tsc_hz : 0;
    uint64_t d = r->tsc > t0 ? r->tsc - t0 : 0;
    int ok;
    if (hz)
        ok = put(out, cap, &len, "%5llu.%06llu", (unsigned long long)(d / hz),
                 (unsigned long long)(d % hz * 1000000ull / hz));
    else
        ok = put(out, cap, &len, "%12llu", (unsigned long long)d);
    ok = ok && put(out, cap, &len, " c%u [%d] ", (unsigned)r->cpu, (int)r->tid);
    const struct ktrace_event_desc *e = ktrace_event_desc(r->event);
    if (!e) {
        ok = ok && put(out, cap, &len, "event%u %#llx %#llx %#llx %#llx", (unsigned)r->event,
                       (unsigned long long)r->a[0], (unsigned long long)r->a[1],
                       (unsigned long long)r->a[2], (unsigned long long)r->a[3]);
    } else {
        ok = ok && put(out, cap, &len, "%s", e->name);
        // "name:kind name:kind ...", one argument each.
        const char *p = e->args;
        for (int i = 0; ok && i < 4 && *p; i++) {
            while (*p == ' ') p++;
            const char *nm = p;
            while (*p && *p != ':' && *p != ' ') p++;
            size_t nl = (size_t)(p - nm);
            char kind = 'x';
            if (*p == ':') { p++; if (*p) kind = *p++; }
            while (*p && *p != ' ') p++;
            if (nl) ok = put_arg(out, cap, &len, nm, nl, kind, r->a[i], c);
        }
    }
    if (!ok) { out[0] = 0; return 0; }
    return (int)len;
}

#define GROUP_ROW(bit_, name_) { (bit_), (name_) },
static const struct { uint32_t bit; const char *name; } g_groups[] = { KTRACE_GROUPS(GROUP_ROW) };
#undef GROUP_ROW
#define NGROUPS (sizeof g_groups / sizeof g_groups[0])

int ktrace_parse_groups(const char *list, uint32_t *mask) {
    if (!list || !mask) return 0;
    if (!k_strcmp(list, "all") || !k_strcmp(list, "on")) { *mask = KTRACE_G_ALL; return 1; }
    uint32_t m = 0;
    const char *p = list;
    for (;;) {
        const char *w = p;
        while (*p && *p != ',') p++;
        size_t n = (size_t)(p - w);
        unsigned g = 0;
        for (; g < NGROUPS; g++)
            if (k_strlen(g_groups[g].name) == n && !k_strncmp(g_groups[g].name, w, n)) break;
        if (g == NGROUPS) return 0;   // an empty word too: "switch,,irq"
        m |= g_groups[g].bit;
        if (!*p) break;
        p++;
    }
    *mask = m;
    return 1;
}

int ktrace_format_groups(uint32_t mask, char *out, size_t cap) {
    if (!out || !cap) return 0;
    size_t len = 0;
    int ok;
    if (!(mask & KTRACE_G_ALL)) ok = put(out, cap, &len, "off");
    else if ((mask & KTRACE_G_ALL) == KTRACE_G_ALL) ok = put(out, cap, &len, "all");
    else {
        ok = 1;
        for (unsigned g = 0; ok && g < NGROUPS; g++)
            if (mask & g_groups[g].bit)
                ok = put(out, cap, &len, "%s%s", len ? "," : "", g_groups[g].name);
    }
    if (!ok) { out[0] = 0; return 0; }
    return (int)len;
}
