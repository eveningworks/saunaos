// update -- bring this machine up to date from an update server.
//
// A FRONT END over userland/update/upd.c, which the System Update window
// shares; what it prints IS the log (/var/log/update.log), so running it
// over `remote.py exec` shows exactly what the window would have.
// docs/commands/update.md has the usage, docs/update-design.md the why.
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "rt/sys.h"
#include "lib/cmd.h"
#include "lib/human.h"
#include "lib/umd.h"
#include "lib/upager.h"
#include "update/upd.h"

#define USAGE "update [--check] [-v] [--go-back] [--from <url>] | --server [<url>] | --channel [<name>] | --channels | --log"

struct cli {
    int tty;          // redraw a live line for the file being fetched
    int drawn;        // that line is on screen and must be cleared first
    int verbose;
    long last_ms;     // when it was last redrawn: a few times a second is plenty
};

static long now_ms(void) { return (long)(sys_monotonic_ns() / 1000000ULL); }

static void clear_live(struct cli *c) {
    if (c->drawn) { fputs("\r\033[K", stdout); c->drawn = 0; }
}

static void on_log(void *ctx, const char *line) {
    struct cli *c = ctx;
    clear_live(c);
    printf("%s\n", line);
    fflush(stdout);
}

static void on_progress(void *ctx, const struct upd_plan *p, int idx) {
    struct cli *c = ctx;
    if (!c->tty || idx < 0) return;
    const struct upd_file *f = &p->files[idx];
    if (f->status != UPD_FETCHING) return;
    long t = now_ms();
    if (c->drawn && t - c->last_ms < 250 && f->got != f->size) return;
    c->last_ms = t;
    char got[16], all[16];
    human_size(got, sizeof got, p->done_bytes);
    human_size(all, sizeof all, p->bytes);
    int pct = f->size ? (int)(f->got * 100 / f->size) : 100;
    printf("\r\033[K  %s  %d%%   (%s of %s)", f->path, pct, got, all);
    fflush(stdout);
    c->drawn = 1;
}

static void list_changes(const struct upd_plan *p, int verbose) {
    int shown = 0;
    for (int i = 0; i < p->count; i++) {
        const struct upd_file *f = &p->files[i];
        if (f->change != UPD_CHANGED && f->change != UPD_NEW && f->change != UPD_REMOVE) continue;
        if (!verbose && shown == 10) {
            printf("  ... %d more (update --check -v)\n", p->changed + p->removals - shown);
            return;
        }
        if (f->change == UPD_REMOVE) {
            printf("  %-34s %7s  remove (no longer shipped)\n", f->path, "");
            shown++;
            continue;
        }
        char sz[16];
        human_size(sz, sizeof sz, f->size);
        printf("  %-34s %7s%s%s\n", f->path, sz, f->change == UPD_NEW ? "  new" : "",
               (f->flags & (UPD_F_KERNEL | UPD_F_KERNEL_GZ)) ? "  (restart needed)" : "");
        shown++;
    }
}

// The engine's notes are Markdown; umd wraps them to this terminal, as
// /bin/doc does a page.
static void show_notes(const struct upd_plan *p, int color) {
    if (!p->notes) return;
    int cols = 80;
    upager_term(NULL, &cols);
    static char buf[65536];
    struct umd_opts mo = { .cols = cols, .color = color, .indent = 2 };
    struct umd_out o = { buf, (int)sizeof buf, 0, 0 };
    umd_render(p->notes, (int)strlen(p->notes), &mo, &o);
    printf("\nWhat's new\n");
    fwrite(buf, 1, (size_t)o.len, stdout);
    if (o.overflow) printf("  ... (cut short)\n");
    printf("\n");
}

static int show_log(void) {
    int fd = open(UPD_LOG_PATH, O_RDONLY);
    if (fd < 0) { printf("update: no log yet (%s)\n", UPD_LOG_PATH); return 1; }
    char buf[1024];
    long n;
    while ((n = read(fd, buf, sizeof buf)) > 0) fwrite(buf, 1, (size_t)n, stdout);
    close(fd);
    return 0;
}

static int server_cmd(const char *url) {
    char cur[UPD_URL_MAX];
    if (!url) {
        printf("%s\n", upd_server_get(cur, sizeof cur));
        static char recent[UPD_RECENT_MAX][UPD_URL_MAX];
        int n = upd_recent(recent, UPD_RECENT_MAX);
        for (int i = 0; i < n; i++) printf("  recent: %s\n", recent[i]);
        return 0;
    }
    if (upd_server_set(url) != 0) {
        printf("update: refused %s -- an http:// address, under 64 characters\n", url);
        return 1;
    }
    printf("update: server is now %s\n", url);
    return 0;
}

// The server's channels, the one in use marked `*`.
static int channels_cmd(void) {
    static char server[UPD_URL_MAX], url[UPD_URL_MAX + 32], err[200];
    static struct upd_channels chs;
    upd_server_get(server, sizeof server);
    if (upd_resolve(server, NULL, url, sizeof url, &chs, err, sizeof err) != 0) {
        printf("update: %s\n", err);
        return 1;
    }
    if (chs.chosen < 0) { printf("update: %s declares no channels; it is one\n", server); return 0; }
    upd_server_get(server, sizeof server);
    printf("%s:\n", server);
    int internal = upd_show_internal();
    for (int i = 0; i < chs.count; i++) {
        const struct upd_channel *c = &chs.ch[i];
        if (c->internal && !internal && i != chs.chosen) continue;
        printf("%c %-10s %-12s %-16s %s%s%s\n", i == chs.chosen ? '*' : ' ', c->id,
               c->version[0] ? c->version : "(empty)", c->built, c->desc,
               i == chs.dflt ? "  [default]" : "", c->internal ? "  [internal]" : "");
    }
    return 0;
}

static int channel_cmd(const char *name) {
    char cur[32];
    if (!name) {
        upd_channel_get(cur, sizeof cur);
        printf("%s\n", cur[0] ? cur : "(the server's default)");
        return 0;
    }
    if (!strcmp(name, "default")) name = "";   // the server's own choice again
    if (name[0]) {
        static char server[UPD_URL_MAX], url[UPD_URL_MAX + 32], err[200];
        static struct upd_channels chs;
        upd_server_get(server, sizeof server);
        if (upd_resolve(server, name, url, sizeof url, &chs, err, sizeof err) == 0 && chs.chosen >= 0
            && strcmp(chs.ch[chs.chosen].id, name) != 0) {
            printf("update: the server has no channel %s (update --channels)\n", name);
            return 1;
        }
    }
    if (upd_channel_set(name) != 0) { printf("update: refused channel %s\n", name); return 1; }
    printf("update: channel is now %s\n", name[0] ? name : "the server's default");
    return 0;
}

int main(int argc, char **argv) {
    int check_only = 0, verbose = 0, go_back = 0;
    const char *from = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--check") || !strcmp(argv[i], "-n")) check_only = 1;
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "--from") && i + 1 < argc) from = argv[++i];
        else if (!strcmp(argv[i], "--server")) return server_cmd(i + 1 < argc ? argv[i + 1] : 0);
        else if (!strcmp(argv[i], "--log")) return show_log();
        else if (!strcmp(argv[i], "--channels")) return channels_cmd();
        else if (!strcmp(argv[i], "--channel")) return channel_cmd(i + 1 < argc ? argv[i + 1] : 0);
        else if (!strcmp(argv[i], "--go-back")) go_back = 1;
        else { cmd_usage(USAGE); return 1; }
    }

    // --from names ONE channel's address, as it always did; otherwise
    // the server's list picks it (upd_resolve()).
    static char server[UPD_URL_MAX + 32];
    static struct upd_channels chs;
    if (from) {
        snprintf(server, sizeof server, "%s", from);
    } else {
        static char base[UPD_URL_MAX], err[200];   // static: the ring-3 frame budget is 2 KiB
        upd_server_get(base, sizeof base);
        if (upd_resolve(base, NULL, server, sizeof server, &chs, err, sizeof err) != 0) {
            printf("update: %s\n", err);
            return 1;
        }
        if (chs.chosen >= 0) printf("update: channel %s\n", chs.ch[chs.chosen].id);
    }

    struct cli c = { .tty = isatty(1), .verbose = verbose };
    struct upd_hooks h = { .ctx = &c, .log = on_log, .progress = on_progress };
    struct upd_plan p;
    if (upd_check(server, &p, &h) != 0) { upd_plan_free(&p); return 1; }

    if (p.staged_for_boot)
        printf("update: an update is already staged -- restart to finish it\n");
    if (p.kernel_blocked)
        printf("update: nothing can be installed until GRUB shows a menu "
               "(docs/commands/update.md)\n");
    if (!p.changed && !p.removals) {
        printf("update: up to date\n");
        upd_plan_free(&p);
        return 0;
    }
    if (p.behind && !go_back) {
        printf("update: this channel's build (%s, %s) is older than this machine's -- waiting "
               "until it passes it. `update --go-back` installs it now.\n", p.version, p.built);
        upd_plan_free(&p);
        return 0;
    }
    show_notes(&p, c.tty);
    char total[16];
    human_size(total, sizeof total, p.bytes);
    printf("%d file%s to fetch, %s", p.changed, p.changed == 1 ? "" : "s", total);
    if (p.removals) printf("; %d stale to remove", p.removals);
    printf(":\n");
    list_changes(&p, verbose || !check_only);
    if (check_only || p.staged_for_boot) { upd_plan_free(&p); return 0; }

    int rc = upd_apply(&p, &h);
    clear_live(&c);
    if (p.at_boot || p.kernel_installed)
        printf("update: restart to finish (`reboot`)\n");
    upd_plan_free(&p);
    return rc == 0 ? 0 : 1;
}
