// update -- bring this machine up to date from an update server.
//
// A FRONT END over userland/common/upd.c, which the System Update window
// shares. The engine writes its whole log to /var/log/update.log; this
// shows a summary and a progress bar, and the log by level with -v, -vv
// and -vvv (enum upd_log_level). Like apt and dnf it asks before it
// installs, and -y answers for a run with no terminal.
// docs/commands/update.md has the usage, docs/update-design.md the why.
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "base/human.h"
#include "base/uargs.h"
#include "base/uask.h"
#include "base/umd.h"
#include "base/upager.h"
#include "base/uprogress.h"
#include "common/upd.h"
#include "base/ubootrefresh.h"

static int o_check, o_yes, o_verbose, o_go_back, o_server, o_channel, o_channels, o_log;
static const char *o_from;

static const struct uargs_opt OPTS[] = {
    { "check",    'n', 0,     "show what would change, install nothing", &o_check, 0 },
    { "yes",      'y', 0,     "install without asking (needed with no terminal)", &o_yes, 0 },
    { "verbose",  'v', 0,     "say more: -v each decision, -vv each file, -vvv each request", &o_verbose, 0 },
    { "from",     0,   "URL", "use this channel's address for this run only", 0, &o_from },
    { "go-back",  0,   0,     "install the channel's build even if it is OLDER than this machine's", &o_go_back, 0 },
    { "server",   0,   0,     "print the update server, or set it: --server URL", &o_server, 0 },
    { "channel",  0,   0,     "print the channel this machine follows, or set it: --channel NAME", &o_channel, 0 },
    { "channels", 0,   0,     "list the server's channels", &o_channels, 0 },
    { "log",      0,   0,     "print the last run's log", &o_log, 0 },
    { 0 },
};

static const struct uargs_prog PROG = {
    .name = "update",
    .usage = "[-n] [-y] [-v...] [--from URL] [--go-back]\n"
             "--server [URL] | --channel [NAME] | --channels | --log",
    .summary = "Bring this machine up to date from its update server: show what is new,\n"
               "ask, then download and install. The System Update window does the same.",
    .opts = OPTS,
    .notes = "The full log of every run is /var/log/update.log, whatever -v says.\n"
             "Exit status: 0 done or up to date, 1 failed or not installed, 2 bad usage.",
    .counted = "v",
};

struct cli {
    int tty;               // stdout is a terminal: draw the bar and the inline "done"
    int inline_open;       // "Checking for updates..." waits for its "done" on this line
    int bar_on;            // the bar is on screen and must be cleared before a line
    struct uprogress bar;
};

static struct cli g;

// A line from the engine, at or below the asked verbosity; problems always.
static void on_log_at(void *ctx, int level, const char *line) {
    (void)ctx;
    if (level != UPD_LOG_PROBLEM && level > o_verbose) return;
    if (g.inline_open) { printf("\n"); g.inline_open = 0; }
    if (g.bar_on) { fputs("\r\033[K", stdout); g.bar.width = 0; }
    printf("  %s\n", line);
    fflush(stdout);
}

static void on_progress(void *ctx, const struct upd_plan *p, int idx) {
    (void)ctx; (void)idx;
    if (!g.bar_on) return;
    uprogress_set(&g.bar, p->done_bytes);
}

// A NEWER BOOT LOADER, said and never written: an update replaces files,
// and GRUB's core is the one write that decides whether the machine boots
// (base/ubootrefresh.h). System Update offers the same as a button.
static void boot_loader_note(void) {
    struct ubootrefresh_offer o;
    ubootrefresh_offer(&o);
    char what[200];
    ubootrefresh_offer_detail(&o, what, sizeof what);
    if (o.kind == UBOOTREFRESH_NEWER || o.kind == UBOOTREFRESH_DIFFERENT)
        printf("A %s boot loader %s%s%s%s -- %s`install --bootloader confirm` writes it.\n",
               o.kind == UBOOTREFRESH_NEWER ? "newer" : "different",
               o.pending ? "comes with this update" : "is staged",
               what[0] ? " (" : "", what, what[0] ? ")" : "", o.pending ? "after the restart, " : "");
    else if (o.kind == UBOOTREFRESH_UNKNOWN)
        printf("This disk's boot loader may be out of date (nothing records what it carries) -- "
               "`install --bootloader confirm` writes the current one.\n");
    ubootrefresh_offer_free(&o);
}

static int is_kernel(const struct upd_file *f) {
    return (f->flags & (UPD_F_KERNEL | UPD_F_KERNEL_GZ)) != 0;
}

// Every file that changes, or the first ten.
static void list_changes(const struct upd_plan *p, int all) {
    int shown = 0;
    for (int i = 0; i < p->count; i++) {
        const struct upd_file *f = &p->files[i];
        if (f->change != UPD_CHANGED && f->change != UPD_NEW && f->change != UPD_REMOVE) continue;
        if (!all && shown == 10) {
            printf("  ... %d more (update --check -v)\n", p->changed + p->removals - shown);
            return;
        }
        shown++;
        if (f->change == UPD_REMOVE) {
            printf("  %-34s %7s  remove (no longer shipped)\n", f->path, "");
            continue;
        }
        char sz[16];
        human_size(sz, sizeof sz, f->size);
        printf("  %-34s %7s%s%s\n", f->path, sz, f->change == UPD_NEW ? "  new" : "",
               is_kernel(f) ? "  (restart needed)" : "");
    }
}

// The engine's notes are Markdown; umd wraps them to this terminal, as
// /bin/doc does a page.
static void show_notes(const struct upd_plan *p) {
    if (!p->notes) return;
    int cols = 80;
    upager_term(NULL, &cols);
    static char buf[65536];
    struct umd_opts mo = { .cols = cols, .color = g.tty, .indent = 2 };
    struct umd_out o = { buf, (int)sizeof buf, 0, 0 };
    umd_render(p->notes, (int)strlen(p->notes), &mo, &o);
    printf("\nWhat's new\n");
    fwrite(buf, 1, (size_t)o.len, stdout);
    if (o.overflow) printf("  ... (cut short)\n");
}

// "16 files to download, 6.1 MiB. The kernel is one: restart to finish."
static void summary(const struct upd_plan *p) {
    char total[16];
    human_size(total, sizeof total, p->bytes);
    int kernel = 0;
    for (int i = 0; i < p->count; i++)
        if (is_kernel(&p->files[i]) && (p->files[i].change == UPD_CHANGED || p->files[i].change == UPD_NEW))
            kernel = 1;
    printf("\n%d file%s to download, %s", p->changed, p->changed == 1 ? "" : "s", total);
    if (p->removals) printf("; %d to remove", p->removals);
    printf(".%s\n", kernel ? " The kernel is one: restart to finish." : "");
    int kept = p->conf_kept + p->conf_removed + p->kept_edited;
    if (kept && !o_verbose)
        printf("%d file%s changed on this machine %s left as %s (update -v names them).\n", kept,
               kept == 1 ? "" : "s", kept == 1 ? "is" : "are", kept == 1 ? "it is" : "they are");
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

static int install(struct upd_plan *p, const struct upd_hooks *h) {
    if (o_yes && o_verbose) printf("Installing without asking (-y)\n");
    // The bar and a line per file would fight over the same row.
    g.bar_on = g.tty && o_verbose < UPD_LOG_FILE;
    if (g.bar_on) {
        uprogress_begin(&g.bar, 1, p->bytes);
        g.bar.label = "Downloading";
    } else if (!o_verbose) {
        printf("Downloading and installing... ");
        fflush(stdout);
        g.inline_open = 1;
    }
    int rc = upd_apply(p, h);
    if (g.bar_on) {
        if (rc == 0) uprogress_end(&g.bar);
        else fputs("\r\033[K", stdout);
        g.bar_on = 0;
    }
    if (g.inline_open) { printf("%s\n", rc == 0 ? "done" : "failed"); g.inline_open = 0; }
    if (rc != 0) {
        // A failed rename leaves the files before it in place: say so.
        int done = 0;
        for (int i = 0; i < p->count; i++)
            done += p->files[i].status == UPD_INSTALLED || p->files[i].status == UPD_REMOVED;
        if (done) printf("The update failed part-way: %d file%s installed, the rest not "
                         "(update --log).\n", done, done == 1 ? " was" : "s were");
        else printf("Nothing was installed.\n");
        return 1;
    }
    if (g.tty && o_verbose < UPD_LOG_FILE) printf("Installing  %s\n", p->at_boot ? "staged for the restart" : "done");
    if (p->at_boot || p->kernel_installed)
        printf("Updated to %s. Restart to finish: reboot\n", p->version[0] ? p->version : "the new build");
    else
        printf("Updated to %s.\n", p->version[0] ? p->version : "the new build");
    boot_loader_note();
    return 0;
}

int main(int argc, char **argv) {
    struct uargs a;
    if (uargs_parse(&a, &PROG, argc, argv)) return a.status;
    int modes = !!o_server + !!o_channel + !!o_channels + !!o_log;
    if (modes > 1) return uargs_error(&PROG, "--server, --channel, --channels and --log go alone");
    if (a.argc > (o_server || o_channel ? 1 : 0)) return uargs_error(&PROG, "unexpected '%s'", a.argv[a.argc - 1]);
    if (o_server) return server_cmd(a.argc ? a.argv[0] : 0);
    if (o_channel) return channel_cmd(a.argc ? a.argv[0] : 0);
    if (o_channels) return channels_cmd();
    if (o_log) return show_log();

    g.tty = isatty(1);
    struct upd_hooks h = { .ctx = &g, .log_at = on_log_at, .progress = on_progress };

    // --from names ONE channel's address, as it always did; otherwise
    // the server's list picks it (upd_resolve()).
    static char server[UPD_URL_MAX + 32];
    static struct upd_channels chs;
    const char *channel = 0;
    printf("Checking for updates... ");
    fflush(stdout);
    g.inline_open = 1;
    if (o_from) {
        snprintf(server, sizeof server, "%s", o_from);
    } else {
        static char base[UPD_URL_MAX], err[200];   // static: the ring-3 frame budget is 2 KiB
        upd_server_get(base, sizeof base);
        if (upd_resolve(base, NULL, server, sizeof server, &chs, err, sizeof err) != 0) {
            printf("failed\nupdate: %s\n", err);
            return 1;
        }
        if (chs.chosen >= 0) channel = chs.ch[chs.chosen].id;
    }

    static struct upd_plan p;   // static: the ring-3 frame budget is 2 KiB
    if (upd_check(server, &p, &h) != 0) {
        if (g.inline_open) printf("failed\n");
        upd_plan_free(&p);
        return 1;
    }
    if (g.inline_open) { printf("done\n"); g.inline_open = 0; }

    char mv[48], mb[48];
    upd_machine_build(mv, sizeof mv, mb, sizeof mb);
    // --check still lists what differs; an install stops here.
    if (p.staged_for_boot) {
        printf("An update is already staged -- restart to finish it: reboot\n");
        if (!o_check) { upd_plan_free(&p); return 0; }
    }
    if (p.kernel_blocked) {
        printf("Nothing can be installed: %s (doc update)\n", p.blocked_why);
        if (!o_check) { upd_plan_free(&p); return 1; }
    }
    if (!p.changed && !p.removals) {
        printf("SaunaOS is up to date (%s%s%s).\n", mv[0] ? mv : "this build", mb[0] ? ", built " : "", mb);
        boot_loader_note();
        upd_plan_free(&p);
        return 0;
    }
    if (p.behind && !o_go_back) {
        printf("This channel's build (%s, %s) is older than this machine's -- waiting until it "
               "passes it. `update --go-back` installs it now.\n", p.version, p.built);
        upd_plan_free(&p);
        return 0;
    }

    printf("SaunaOS %s is %s (built %s%s%s)\n", p.version[0] ? p.version : "(no version)",
           p.behind ? "an OLDER build" : "available", p.built[0] ? p.built : "no date",
           channel ? ", channel " : "", channel ? channel : "");
    if (mv[0]) printf("This machine has %s%s%s%s\n", mv, mb[0] ? " (built " : "", mb, mb[0] ? ")" : "");
    show_notes(&p);
    summary(&p);
    if (o_check || o_verbose) list_changes(&p, o_verbose > 0);
    if (o_check) { upd_plan_free(&p); return 0; }

    if (!o_yes) {
        printf("\n");
        int yes = uask_yes_no("Install the update?", 1);
        if (yes == UASK_NO_TERMINAL) {
            printf("update: %d file%s to install, and no terminal to ask -- add -y to install\n",
                   p.changed, p.changed == 1 ? "" : "s");
            upd_plan_free(&p);
            return 1;
        }
        if (!yes) { printf("Nothing was installed.\n"); upd_plan_free(&p); return 1; }
    }
    printf("\n");
    int rc = install(&p, &h);
    upd_plan_free(&p);
    return rc;
}
