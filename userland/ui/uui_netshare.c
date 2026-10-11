// See uui_netshare.h.
#include "ui/uui_netshare.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/ugfx.h"
#include "lib/unetmount.h"
#include "lib/icon_cache.h"
#include "rt/sys.h"
#include "query_abi.h"
#include "userfs_abi.h"

// Positive: an app dispatches a dialog answer only when its code is > 0.
enum { NS_OK = 1, NS_CANCEL = 2 };
enum { KIND_ADD = 1, KIND_SIGNIN = 2 };

#define MOUNT_WAIT_MS 8000ull

static unsigned long long now_ms(void) { return sys_monotonic_ns() / 1000000ull; }

static void open_dialog(struct uui_netshare *n, const char *ok_label) {
    struct uui_dialog_button btns[2] = {
        { "Cancel", NS_CANCEL, 0 }, { ok_label, NS_OK, UUI_DLG_PRIMARY },
    };
    uui_dialog_open(n->dlg, n->title, n->rows, n->row[0] ? 1 : 0, btns, 2, 1, NS_CANCEL);
    int w, h;
    uui_form_ops.natural_size(&n->form, &w, &h);
    uui_dialog_set_body(n->dlg, &n->item, ugfx_char_w() * 44, h);
    uui_dialog_focus(n->dlg, &n->item);
    // A padlock beside Sign in's title; Add has none, so its fields keep
    // the dialog's whole width.
    if (n->kind == KIND_SIGNIN) uui_dialog_set_picture(n->dlg, icon_get("lock", ugfx_char_h() * 3));
}

static void prepare(struct uui_netshare *n, struct uui_dialog *d, int kind) {
    memset(n, 0, sizeof *n);
    n->dlg = d;
    n->kind = kind;
    n->rows[0] = n->row;
    uui_form_init(&n->form);
    n->item = (struct uui_item){ .ops = &uui_form_ops, .widget = &n->form, .name = "netshare" };
}

void uui_netshare_add(struct uui_netshare *n, struct uui_dialog *d) {
    prepare(n, d, KIND_ADD);
    snprintf(n->title, sizeof n->title, "Add network share");
    n->f_addr = uui_form_add_text(&n->form, "Address", "",
                                  "//server/share or \\\\server\\share. An IP address works too.");
    n->f_name = uui_form_add_text(&n->form, "Name", "", "Shown under Network, and as the folder /net/NAME.");
    n->f_open = uui_form_add_check(&n->form, "Open it now", 1, NULL);
    open_dialog(n, "Add");
    uui_dialog_set_note(d, "Read-only. You are asked to sign in the first time it opens.");
    uui_form_focus(&n->form, n->f_addr);
}

void uui_netshare_signin(struct uui_netshare *n, struct uui_dialog *d, const char *point,
                         const char *server, const char *user) {
    prepare(n, d, KIND_SIGNIN);
    snprintf(n->point, sizeof n->point, "%s", point);
    snprintf(n->server, sizeof n->server, "%s", server && server[0] ? server : "the server");
    snprintf(n->title, sizeof n->title, "Sign in to %s", n->server);
    const char *name = strrchr(point, '/');
    snprintf(n->row, sizeof n->row, "%s on %s needs a user name and password.",
             name && name[1] ? name + 1 : point, n->server);
    n->f_user = uui_form_add_text(&n->form, "User name", user ? user : "", NULL);
    n->f_pass = uui_form_add_password(&n->form, "Password", NULL);
    n->f_remember = uui_form_add_check(&n->form, "Remember password", 0,
                                       "Saved as a hash, never as text.");
    open_dialog(n, "Sign in");
    uui_form_focus(&n->form, user && user[0] ? n->f_pass : n->f_user);
}

struct uui_item *uui_netshare_item(struct uui_netshare *n) { return &n->item; }

int uui_netshare_is_open(const struct uui_netshare *n) {
    return n->dlg && uui_dialog_is_open(n->dlg) && n->dlg->body == &n->item;
}

static void reopen_with(struct uui_netshare *n, const char *error, int focus_row) {
    snprintf(n->error, sizeof n->error, "%s", error);
    uui_form_set_error(&n->form, n->error);
    open_dialog(n, n->kind == KIND_ADD ? "Add" : "Sign in");
    if (n->kind == KIND_ADD)
        uui_dialog_set_note(n->dlg, "Read-only. You are asked to sign in the first time it opens.");
    uui_form_focus(&n->form, focus_row);
}

// //server/share in any of the three spellings, written the one way.
// 0 if it names no share.
static int normalise(const char *in, char *out, size_t cap) {
    const char *p = in;
    while (*p == ' ') p++;
    if (!strncmp(p, "smb://", 6)) p += 6;
    else if ((p[0] == '/' && p[1] == '/') || (p[0] == '\\' && p[1] == '\\')) p += 2;
    else return 0;
    size_t o = 0;
    if (cap < 3) return 0;
    out[o++] = '/'; out[o++] = '/';
    int seps = 0, after = 0;
    for (; *p && *p != ' '; p++) {
        char c = *p == '\\' ? '/' : *p;
        if (c == '/') { if (o && out[o - 1] == '/') continue; seps++; after = 0; }
        else after++;
        if (o + 2 > cap) return 0;
        out[o++] = c;
    }
    while (o > 2 && out[o - 1] == '/') o--;
    out[o] = '\0';
    // A server, then at least one more component: the share.
    return seps >= 1 && (after > 0 || seps >= 2) && out[2] != '/';
}

static void *signin_thread(void *arg) {
    struct uui_netshare *n = arg;
    int64_t rc = sys_userfs(USERFS_CTL, (uint64_t)(uintptr_t)n->ctl, 0);
    n->result = rc < 0 ? -sys_errno() : 0;
    __atomic_store_n(&n->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static pthread_t g_thread;   // one sign-in at a time, for the process

static void answer_signin(struct uui_netshare *n, int code) {
    if (code != NS_OK) {
        uui_form_wipe_passwords(&n->form);
        n->state = UUI_NETSHARE_CLOSED;
        return;
    }
    const char *user = uui_form_text(&n->form, n->f_user);
    if (!user[0]) { reopen_with(n, "Enter a user name.", n->f_user); return; }
    struct userfs_ctl *c = calloc(1, sizeof *c);
    if (!c) { reopen_with(n, "Out of memory.", n->f_pass); return; }
    snprintf(c->point, sizeof c->point, "%s", n->point);
    int len = snprintf((char *)c->data, sizeof c->data, "%s%c%c%s%c%d", user, 0, 0,
                       uui_form_text(&n->form, n->f_pass), 0,
                       uui_form_checked(&n->form, n->f_remember) ? 1 : 0);
    uui_form_wipe_passwords(&n->form);
    if (len < 0 || len + 1 > (int)sizeof c->data) { free(c); reopen_with(n, "Too long.", n->f_user); return; }
    c->code = USERFS_CTL_LOGIN;
    c->len = (uint32_t)len + 1;
    n->ctl = c;
    n->done = 0;
    if (pthread_create(&g_thread, NULL, signin_thread, n) != 0) {
        signin_thread(n);     // no thread: ask from here, and wait
    }
    n->busy = 1;
    n->state = UUI_NETSHARE_BUSY;
}

static void answer_add(struct uui_netshare *n, int code) {
    if (code != NS_OK) { n->state = UUI_NETSHARE_CLOSED; return; }
    struct unetmount m;
    memset(&m, 0, sizeof m);
    if (!normalise(uui_form_text(&n->form, n->f_addr), m.source, sizeof m.source)) {
        reopen_with(n, "That is not a share address: //server/share.", n->f_addr);
        return;
    }
    snprintf(m.name, sizeof m.name, "%s", uui_form_text(&n->form, n->f_name));
    if (!m.name[0]) unetmount_suggest(m.source, m.name, sizeof m.name);
    if (!unetmount_name_ok(m.name)) {
        reopen_with(n, "A name is letters, digits, '-', '_' and '.', with no spaces.", n->f_name);
        return;
    }
    struct unetmount old;
    if (unetmount_load(m.name, &old)) {
        reopen_with(n, "A share with that name already exists.", n->f_name);
        return;
    }
    if (!unetmount_save(&m)) {
        reopen_with(n, "Could not save it in " UNETMOUNT_DIR ".", n->f_addr);
        return;
    }
    snprintf(n->name, sizeof n->name, "%s", m.name);
    unetmount_point(m.name, n->point, sizeof n->point);
    if (!uui_form_checked(&n->form, n->f_open)) { n->state = UUI_NETSHARE_CLOSED; return; }
    n->since = now_ms();
    n->busy = 1;
    n->state = UUI_NETSHARE_BUSY;
}

void uui_netshare_answer(struct uui_netshare *n, int code) {
    uui_dialog_set_body(n->dlg, NULL, 0, 0);
    if (n->kind == KIND_ADD) answer_add(n, code);
    else answer_signin(n, code);
}

static int mounted(const char *point) {
    struct query_fsinfo fs;
    for (int i = 0; sys_query_record(QUERY_FSINFO, i, &fs, sizeof fs) > 0; i++)
        if ((fs.flags & QUERY_FS_REMOTE) && !strcmp(fs.point, point)) return 1;
    return 0;
}

int uui_netshare_poll(struct uui_netshare *n, char *out, size_t cap) {
    // The name follows the address until the user types one of their own.
    if (n->kind == KIND_ADD && uui_netshare_is_open(n)) {
        const char *cur = uui_form_text(&n->form, n->f_name);
        if (!cur[0] || !strcmp(cur, n->suggested)) {
            char src[160], name[32] = "";
            if (normalise(uui_form_text(&n->form, n->f_addr), src, sizeof src))
                unetmount_suggest(src, name, sizeof name);
            if (strcmp(name, cur)) {
                uui_form_set_text(&n->form, n->f_name, name);
                snprintf(n->suggested, sizeof n->suggested, "%s", name);
            }
        }
    }
    if (n->state != UUI_NETSHARE_BUSY) {
        int s = n->state;
        if (s == UUI_NETSHARE_OPEN) snprintf(out, cap, "%s", n->point);
        n->state = UUI_NETSHARE_IDLE;
        return s;
    }
    if (n->kind == KIND_ADD) {
        // smbfsd notices the new file within a couple of seconds.
        if (mounted(n->point) || now_ms() - n->since > MOUNT_WAIT_MS) {
            n->busy = 0;
            n->state = UUI_NETSHARE_IDLE;
            snprintf(out, cap, "%s", n->point);
            return UUI_NETSHARE_OPEN;
        }
        return UUI_NETSHARE_BUSY;
    }
    if (!__atomic_load_n(&n->done, __ATOMIC_ACQUIRE)) return UUI_NETSHARE_BUSY;
    pthread_join(g_thread, NULL);
    memset(n->ctl, 0, sizeof(struct userfs_ctl));
    free(n->ctl);
    n->ctl = NULL;
    n->busy = 0;
    if (n->result == 0) {
        n->state = UUI_NETSHARE_IDLE;
        snprintf(out, cap, "%s", n->point);
        return UUI_NETSHARE_OPEN;
    }
    reopen_with(n, n->result == -EACCES ? "Wrong user name or password."
                 : n->result == -ETIMEDOUT ? "The server did not answer."
                 : "Could not sign in.", n->f_pass);
    n->state = UUI_NETSHARE_IDLE;
    return UUI_NETSHARE_IDLE;
}
