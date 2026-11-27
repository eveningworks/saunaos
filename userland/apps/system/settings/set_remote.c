// System Settings: Network > Remote Desktop -- one card per protocol
// (VNC, RDP), each its own switch, port and sign-in; who may connect,
// shared below them; the trusted addresses; the sessions open now. A page
// of its own, as Adapters is: the settings live in root's /etc/remote.conf
// and /etc/remote.secret (net/uremote.h), read by /bin/remoted every few
// seconds. They are the MACHINE's (users D3b2a), so the page STAGES every
// edit, and Apply... sends them as ONE privd request -- one administrator
// prompt listing each change, D3b1's shield and Apply; root's are made at
// once. The Connected rows are the seat's (privd's `remote`), at once.
#include "apps/system/settings/settings_internal.h"
#include "ui/uui_card.h"
#include "ui/uui_actionlist.h"
#include "net/uremote.h"
#include "base/upriv.h"   // upriv_strerror()

int g_show_remote;

static struct uremote_conf g_rc;

// The VNC card.
static struct uui_switch g_vnc_sw;
static struct uui_spinbox g_vnc_port;
static struct uui_textbox g_vnc_pw;
static struct uui_button g_vnc_pw_rm;
static struct uui_item g_vnc_pw_items[2];
static struct uui_layout g_vnc_pw_l;
static int g_pw_remove;   // Remove pressed: `vnc.password -` is staged
static struct uui_setting_row g_vnc_port_row, g_vnc_pw_row;
static const char *const ENC[] = { "When the viewer offers it", "Required", "Off" };
static struct uui_dropdown g_enc_dd;
static struct uui_setting_row g_enc_row;
static char g_enc_desc[200];
static struct uui_label g_vnc_note;
static struct uui_item g_vnc_body[4];
static struct uui_card g_vnc;
static char g_vnc_sub[96], g_vnc_badge[24];

// The RDP card: there, so the shape is the one chosen, and honest about
// what is not built.
static struct uui_switch g_rdp_sw;
static struct uui_label g_rdp_note;
static struct uui_item g_rdp_body[1];
static struct uui_card g_rdp;

static struct uui_item g_cards_items[2];
static struct uui_layout g_cards;

// Shared.
static const char *const WHEN[] = { "Ask every time", "Ask unless trusted", "Always allow" };
static const char *const FROM[] = { "This network only", "Anywhere" };
static struct uui_dropdown g_when_dd, g_from_dd;
static struct uui_setting_row g_when_row, g_from_row;

// Trusted viewers.
static struct uui_label g_trusted_h;
static struct uui_actionlist_row g_trusted_rows[UREMOTE_TRUSTED_MAX];
static struct uui_actionlist g_trusted;
static struct uui_textbox g_add_addr, g_add_label;
static struct uui_button g_add_b;
static struct uui_item g_add_items[3];
static struct uui_layout g_add_row_l;
static struct uui_setting_row g_add_row;

// Connected.
static struct uui_label g_conn_h;
static struct query_remotesess *g_sess;   // grown to the sessions open
static int g_nsess, g_sess_cap, g_rows_cap;
static struct uui_actionlist_row *g_sess_rows;
static struct uui_actionlist g_conn;
static uint64_t g_checked_ms;

// WHAT APPLY WILL ASK FOR: the controls hold the staged values, and the
// trusted list's edits are kept here until Apply -- adds with their label,
// removals by address.
static struct uremote_net g_add[UREMOTE_TRUSTED_MAX];
static int g_nadd;
static char g_del[UREMOTE_TRUSTED_MAX][24];
static int g_ndel;
static int g_busy;   // a request is with privd
// The trusted rows on show: the file's less the removals, then the adds.
static struct uremote_net g_shown[UREMOTE_TRUSTED_MAX];
static int g_shown_add[UREMOTE_TRUSTED_MAX];   // 1: an add, not applied yet
static int g_nshown;

static const char *addr_text(void) {
    static char out[64];
    struct query_netdev d;
    out[0] = 0;
    QUERY_FOREACH(QUERY_NETDEV, d, i) {
        if (!d.ip) continue;
        uremote_fmt_ip((uint32_t)d.ip, out, sizeof out);
        break;
    }
    return out;
}

static int deleted(const char *addr) {
    for (int i = 0; i < g_ndel; i++) if (!strcmp(g_del[i], addr)) return 1;
    return 0;
}

static void build_trusted(void) {
    g_nshown = 0;
    for (int i = 0; i < g_rc.ntrusted && g_nshown < UREMOTE_TRUSTED_MAX; i++)
        if (!deleted(g_rc.trusted[i].text)) { g_shown_add[g_nshown] = 0; g_shown[g_nshown++] = g_rc.trusted[i]; }
    for (int i = 0; i < g_nadd && g_nshown < UREMOTE_TRUSTED_MAX; i++) {
        g_shown_add[g_nshown] = 1;
        g_shown[g_nshown++] = g_add[i];
    }
    for (int i = 0; i < g_nshown; i++) {
        struct uui_actionlist_row *r = &g_trusted_rows[i];
        memset(r, 0, sizeof *r);
        if (g_shown[i].label[0])
            snprintf(r->title, sizeof r->title, "%s - %s", g_shown[i].text, g_shown[i].label);
        else
            snprintf(r->title, sizeof r->title, "%s", g_shown[i].text);
        int bits = 0;
        for (uint32_t m = g_shown[i].mask; m; m <<= 1) bits++;
        snprintf(r->sub, sizeof r->sub, g_shown_add[i] ? "Trusted once you apply"
                 : bits == 32 ? "Allowed without asking"
                 : "Every address in this subnet is allowed without asking");
        r->btn[1] = "Remove";
        r->primary = -1;
    }
    g_trusted.count = g_nshown;
}

// Rebuilds the Connected rows; 1 if what they say changed.
static int build_sessions(void) {
    static char *was;
    char *now = strdup("");
    g_nsess = uremote_sessions(&g_sess, &g_sess_cap);
    if (!UGROW(g_sess_rows, g_rows_cap, g_nsess > 0 ? g_nsess : 1)) g_nsess = 0;
    g_conn.rows = g_sess_rows;
    for (int i = 0; i < g_nsess; i++) {
        struct uui_actionlist_row *r = &g_sess_rows[i];
        memset(r, 0, sizeof *r);
        char ip[16], what[QUERY_REMOTESESS_STATUS_MAX];
        uremote_fmt_ip((uint32_t)g_sess[i].remote_ip, ip, sizeof ip);
        uremote_describe(&g_sess[i], what, sizeof what);
        snprintf(r->title, sizeof r->title, "%s", ip);
        snprintf(r->sub, sizeof r->sub, "%s", what);
        if (uremote_is_desktop(&g_sess[i]))
            r->btn[0] = strstr(what, "view only") ? "Full control" : "View only";
        r->btn[1] = "Disconnect";
        r->primary = -1;
        char *more = 0;
        if (now && asprintf(&more, "%s%s\t%s\n", now, r->title, r->sub) < 0) more = 0;
        free(now);
        now = more;
    }
    g_conn.count = g_nsess;
    int changed = !now || !was || strcmp(now, was) != 0;
    free(was);
    was = now;
    return changed;
}

static void describe_cards(void) {
    // The address a viewer types, which is the one fact the card owes;
    // which viewers work is in the note.
    const char *a = addr_text();
    if (a[0]) snprintf(g_vnc_sub, sizeof g_vnc_sub, "%s:%d", a, g_rc.vnc.port);
    else snprintf(g_vnc_sub, sizeof g_vnc_sub, "No address yet");
    int desk = 0;
    for (int i = 0; i < g_nsess; i++) desk += uremote_is_desktop(&g_sess[i]);
    if (desk) snprintf(g_vnc_badge, sizeof g_vnc_badge, "%d connected", desk);
    g_vnc.badge = desk ? g_vnc_badge : NULL;
    g_vnc.active = g_rc.vnc.enabled;
}

void remote_init(void) {
    uui_switch_init(&g_vnc_sw, 0);
    uui_spinbox_init(&g_vnc_port, 5900, 1, 65535, 1, NULL);
    // EMPTY, whatever is set: the password is root's to read (remote.secret);
    // the placeholder says whether there is one, and typing is a change.
    uui_textbox_init(&g_vnc_pw, "");
    g_vnc_pw.masked = 1;
    g_vnc_pw.placeholder = "No password";
    uui_button_init(&g_vnc_pw_rm, 0, 0, 0, 0, "Remove", UUI_ROLE(control_bg), UUI_ROLE(text), ID_RD_PW_RM);
    g_vnc_pw_rm.disabled = 1;
    g_vnc_pw_items[0] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &g_vnc_pw, .id = ID_RD_PW,
                                           .flags = UUI_FILL_W, .name = "rd_vnc_pw" };
    g_vnc_pw_items[1] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_vnc_pw_rm,
                                           .id = ID_RD_PW_RM, .name = "rd_vnc_pw_rm" };
    g_vnc_pw_l = (struct uui_layout){ .dir = UUI_ROW, .items = g_vnc_pw_items, .count = 2 };
    uui_setting_row_init(&g_vnc_port_row, "Port", "5900 is VNC's own.",
                         (struct uui_item){ .ops = &uui_spinbox_ops, .widget = &g_vnc_port,
                                            .id = ID_RD_PORT, .name = "rd_vnc_port" });
    uui_setting_row_init(&g_vnc_pw_row, "Password", "Required: with none, every viewer is refused. "
                         "Type a new one to change it.",
                         (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_vnc_pw_l,
                                            .flags = UUI_FILL_W, .name = "rd_vnc_pw_line" });
    g_vnc_port_row.flat = g_vnc_pw_row.flat = 1;
    g_vnc_pw_row.stacked = 1;
    uui_dropdown_init(&g_enc_dd, 0, 0, 0, 0, ENC, 3);
    uui_setting_row_init(&g_enc_row, "Encryption", g_enc_desc,
                         (struct uui_item){ .ops = &uui_dropdown_ops, .widget = &g_enc_dd,
                                            .id = ID_RD_ENC, .name = "rd_enc" });
    g_enc_row.flat = 1;
    g_enc_row.stacked = 1;
    uui_label_init(&g_vnc_note, "For TigerVNC, RealVNC, Remmina and macOS Screen Sharing. "
                                "Without encryption VNC checks only the first 8 characters of "
                                "the password, and the picture crosses the network as it is.");
    uui_label_set_wrap(&g_vnc_note, 4);
    g_vnc_body[0] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_vnc_port_row,
                                       .flags = UUI_FILL_W, .name = "rd_vnc_port_row" };
    g_vnc_body[1] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_vnc_pw_row,
                                       .flags = UUI_FILL_W, .name = "rd_vnc_pw_row" };
    g_vnc_body[2] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_enc_row,
                                       .flags = UUI_FILL_W, .name = "rd_enc_row" };
    g_vnc_body[3] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_vnc_note,
                                       .flags = UUI_FILL_W, .name = "rd_vnc_note" };
    uui_card_init(&g_vnc, "VNC", g_vnc_sub,
                  (struct uui_item){ .ops = &uui_switch_ops, .widget = &g_vnc_sw, .id = ID_RD_VNC,
                                     .name = "rd_vnc" },
                  g_vnc_body, 4);

    uui_switch_init(&g_rdp_sw, 0);
    g_rdp_sw.disabled = 1;
    uui_label_init(&g_rdp_note, "Not built yet. RDP is planned after encryption for VNC.");
    uui_label_set_wrap(&g_rdp_note, 3);
    g_rdp_body[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_rdp_note,
                                       .flags = UUI_FILL_W, .name = "rd_rdp_note" };
    uui_card_init(&g_rdp, "RDP", "Windows Remote Desktop",
                  (struct uui_item){ .ops = &uui_switch_ops, .widget = &g_rdp_sw, .id = ID_RD_RDP,
                                     .name = "rd_rdp" },
                  g_rdp_body, 1);
    g_cards_items[0] = (struct uui_item){ .ops = &uui_card_ops, .widget = &g_vnc,
                                          .flags = UUI_FILL_W, .name = "rd_vnc_card" };
    g_cards_items[1] = (struct uui_item){ .ops = &uui_card_ops, .widget = &g_rdp,
                                          .flags = UUI_FILL_W, .name = "rd_rdp_card" };
    g_cards = (struct uui_layout){ .dir = UUI_ROW, .items = g_cards_items, .count = 2 };

    uui_dropdown_init(&g_when_dd, 0, 0, 0, 0, WHEN, 3);
    uui_dropdown_init(&g_from_dd, 0, 0, 0, 0, FROM, 2);
    uui_setting_row_init(&g_when_row, "When someone connects",
                         "The password is checked first, whichever you pick.",
                         (struct uui_item){ .ops = &uui_dropdown_ops, .widget = &g_when_dd,
                                            .id = ID_RD_WHEN, .name = "rd_when" });
    uui_setting_row_init(&g_from_row, "Allow connections from",
                         "This network: only addresses on a network this machine's cards are on.",
                         (struct uui_item){ .ops = &uui_dropdown_ops, .widget = &g_from_dd,
                                            .id = ID_RD_FROM, .name = "rd_from" });

    uui_label_init(&g_trusted_h, "Trusted viewers");
    uui_actionlist_init(&g_trusted, g_trusted_rows, 0,
                        "No trusted addresses: everyone is asked about, unless "
                        "\"When someone connects\" says otherwise.");
    uui_textbox_init(&g_add_addr, "");
    g_add_addr.placeholder = "Address or subnet, 192.168.1.0/24";
    uui_textbox_init(&g_add_label, "");
    g_add_label.placeholder = "Name (optional)";
    uui_button_init(&g_add_b, 0, 0, 0, 0, "Add", UUI_ROLE(control_bg), UUI_ROLE(text), ID_RD_ADD);
    g_add_items[0] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &g_add_addr,
                                        .id = ID_RD_ADD_ADDR, .flags = UUI_FILL_W, .name = "rd_add_addr" };
    g_add_items[1] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &g_add_label,
                                        .id = ID_RD_ADD_LABEL, .flags = UUI_FILL_W, .name = "rd_add_label" };
    g_add_items[2] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_add_b,
                                        .id = ID_RD_ADD, .name = "rd_add" };
    g_add_row_l = (struct uui_layout){ .dir = UUI_ROW, .items = g_add_items, .count = 3 };
    uui_setting_row_init(&g_add_row, "Add a trusted address", NULL,
                         (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_add_row_l,
                                            .flags = UUI_FILL_W, .name = "rd_add_row" });
    g_add_row.stacked = 1;

    uui_label_init(&g_conn_h, "Connected");
    uui_actionlist_init(&g_conn, g_sess_rows, 0, "Nobody is connected.");
}

// Bold headings: a font, not a flag, and set once the session's font is
// known (settings.c calls this where it sets its own title's).
void remote_fonts(void) {
    g_trusted_h.font = ugfx_font_session(UGFX_FONT_BOLD);
    g_conn_h.font = ugfx_font_session(UGFX_FONT_BOLD);
}

// Every row the machine's: a shield on each, unless this is root.
static void shields(void) {
    int adm = usetting_needs_admin_flags(0);
    g_vnc_port_row.shield = g_vnc_pw_row.shield = g_enc_row.shield = adm;
    g_when_row.shield = g_from_row.shield = g_add_row.shield = adm;
}

// The field's placeholder and Remove's greying, from what is set and staged.
static void password_state(void) {
    g_vnc_pw.placeholder = g_pw_remove ? "Removed when you apply"
                         : g_rc.vnc.password_set ? "A password is set" : "No password";
    g_vnc_pw_rm.disabled = !g_rc.vnc.password_set || g_pw_remove;
}

void remote_load(void) {
    uremote_load(&g_rc);
    g_nadd = g_ndel = 0;
    shields();
    g_vnc_sw.on = g_rc.vnc.enabled;
    uui_spinbox_set_value(&g_vnc_port, g_rc.vnc.port);
    uui_textbox_set_text(&g_vnc_pw, "");
    g_pw_remove = 0;
    password_state();
    uui_dropdown_set_selected(&g_when_dd, (int)g_rc.when);
    uui_dropdown_set_selected(&g_enc_dd, (int)g_rc.vnc.encryption);
    // The fingerprint a viewer shows when it first meets this machine:
    // comparing the two is how a person knows nobody is in between.
    if (g_rc.vnc.fingerprint[0])
        snprintf(g_enc_desc, sizeof g_enc_desc, "TLS (VeNCrypt). Certificate SHA-256: %s",
                 g_rc.vnc.fingerprint);
    else
        snprintf(g_enc_desc, sizeof g_enc_desc, "TLS (VeNCrypt). The certificate is made when "
                 "VNC is first switched on.");
    uui_dropdown_set_selected(&g_from_dd, (int)g_rc.from);
    build_trusted();
    build_sessions();
    describe_cards();
}

int remote_emit(struct uui_item *out, int n, struct uui_focusable *focus, int *nfocus) {
    out[n++] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_cards,
                                  .flags = UUI_FILL_W, .name = "rd_cards" };
    focus[(*nfocus)++] = (struct uui_focusable){ &g_vnc_sw, &uui_switch_ops };
    focus[(*nfocus)++] = (struct uui_focusable){ &g_vnc_port, &uui_spinbox_ops };
    focus[(*nfocus)++] = (struct uui_focusable){ &g_vnc_pw, &uui_textbox_focus_ops };
    focus[(*nfocus)++] = (struct uui_focusable){ &g_vnc_pw_rm, &uui_button_ops };
    focus[(*nfocus)++] = (struct uui_focusable){ &g_enc_dd, &uui_dropdown_ops };
    out[n++] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_when_row,
                                  .flags = UUI_FILL_W, .name = "rd_when_row" };
    focus[(*nfocus)++] = (struct uui_focusable){ &g_when_dd, &uui_dropdown_ops };
    out[n++] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_from_row,
                                  .flags = UUI_FILL_W, .name = "rd_from_row" };
    focus[(*nfocus)++] = (struct uui_focusable){ &g_from_dd, &uui_dropdown_ops };
    out[n++] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_trusted_h,
                                  .flags = UUI_FILL_W, .name = "rd_trusted_h" };
    out[n++] = (struct uui_item){ .ops = &uui_actionlist_ops, .widget = &g_trusted, .id = ID_RD_TRUSTED,
                                  .flags = UUI_FILL_W | UUI_TRACK_HOVER, .name = "rd_trusted" };
    out[n++] = (struct uui_item){ .ops = &uui_setting_row_ops, .widget = &g_add_row,
                                  .flags = UUI_FILL_W, .name = "rd_add_row" };
    focus[(*nfocus)++] = (struct uui_focusable){ &g_add_addr, &uui_textbox_focus_ops };
    focus[(*nfocus)++] = (struct uui_focusable){ &g_add_label, &uui_textbox_focus_ops };
    focus[(*nfocus)++] = (struct uui_focusable){ &g_add_b, &uui_button_ops };
    out[n++] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_conn_h,
                                  .flags = UUI_FILL_W, .name = "rd_conn_h" };
    out[n++] = (struct uui_item){ .ops = &uui_actionlist_ops, .widget = &g_conn, .id = ID_RD_CONN,
                                  .flags = UUI_FILL_W | UUI_TRACK_HOVER, .name = "rd_conn" };
    return n;
}

int remote_fit(void) {
    int changed = 0;
    if (uui_setting_row_fit(&g_vnc_port_row)) changed = 1;
    if (uui_setting_row_fit(&g_vnc_pw_row)) changed = 1;
    if (uui_setting_row_fit(&g_enc_row)) changed = 1;
    if (uui_setting_row_fit(&g_when_row)) changed = 1;
    if (uui_setting_row_fit(&g_from_row)) changed = 1;
    if (uui_setting_row_fit(&g_add_row)) changed = 1;
    return changed;
}

// --- staging and Apply ---------------------------------------------------------

static const char *const ENC_W[] = { "prefer", "require", "off" };
static const char *const WHEN_W[] = { "ask", "ask-unless-trusted", "always" };
static const char *const FROM_W[] = { "network", "anywhere" };

// The staged changes as key/value pairs (net/uremote.h's). TWO copies:
// the footer counts into `g_count` on every frame, and Apply builds
// `g_req` once -- what the worker thread sends, never rewritten while it
// is out (g_busy), and wiped when privd has answered.
#define UREMOTE_CHANGES_MAX (6 + 2 * UREMOTE_TRUSTED_MAX)
struct kvset {
    char buf[UREMOTE_CHANGES_MAX][2][UREMOTE_PASSWORD_MAX + 32];
    const char *kv[2 * UREMOTE_CHANGES_MAX];
};
static struct kvset g_count, g_req;
static struct kvset *g_into;   // where staged() writes

static int add_kv(int n, const char *k, const char *fmt, const char *v) {
    snprintf(g_into->buf[n][0], sizeof g_into->buf[n][0], "%s", k);
    snprintf(g_into->buf[n][1], sizeof g_into->buf[n][1], fmt, v);
    g_into->kv[2 * n] = g_into->buf[n][0];
    g_into->kv[2 * n + 1] = g_into->buf[n][1];
    return n + 1;
}

static int staged(struct kvset *into) {
    g_into = into;
    int n = 0;
    char v[16];
    if (g_vnc_sw.on != g_rc.vnc.enabled) n = add_kv(n, "vnc.enabled", "=%s", g_vnc_sw.on ? "yes" : "no");
    if (uui_spinbox_value(&g_vnc_port) != g_rc.vnc.port) {
        snprintf(v, sizeof v, "%ld", (long)uui_spinbox_value(&g_vnc_port));
        n = add_kv(n, "vnc.port", "=%s", v);
    }
    int e = uui_dropdown_selected(&g_enc_dd), w = uui_dropdown_selected(&g_when_dd);
    int f = uui_dropdown_selected(&g_from_dd);
    if (e >= 0 && e < 3 && e != (int)g_rc.vnc.encryption) n = add_kv(n, "vnc.encryption", "=%s", ENC_W[e]);
    const char *pw = uui_textbox_text(&g_vnc_pw);
    if (pw[0]) n = add_kv(n, UREMOTE_CHANGE_PASSWORD, "=%s", pw);
    else if (g_pw_remove) n = add_kv(n, UREMOTE_CHANGE_PASSWORD, "%s", "-");
    if (w >= 0 && w < 3 && w != (int)g_rc.when) n = add_kv(n, "when", "=%s", WHEN_W[w]);
    if (f >= 0 && f < 2 && f != (int)g_rc.from) n = add_kv(n, "from", "=%s", FROM_W[f]);
    for (int i = 0; i < g_ndel; i++) {
        char k[48];
        snprintf(k, sizeof k, UREMOTE_CHANGE_TRUST "%s", g_del[i]);
        n = add_kv(n, k, "%s", "-");
    }
    for (int i = 0; i < g_nadd; i++) {
        char k[48];
        snprintf(k, sizeof k, UREMOTE_CHANGE_TRUST "%s", g_add[i].text);
        n = add_kv(n, k, "=%s", g_add[i].label);
    }
    return n;
}

int remote_changes(void) {
    if (!g_show_remote) return 0;
    int n = staged(&g_count);
    memset(&g_count, 0, sizeof g_count);   // a password, perhaps
    return n;
}
int remote_busy(void) { return g_busy; }

static int apply_work(void *arg) {
    return uremote_request(g_req.kv, (int)(intptr_t)arg) < 0 ? -errno : 0;
}

static void apply_done(struct uapp *a, int rc, int err, void *arg) {
    (void)err;
    int n = (int)(intptr_t)arg;
    g_busy = 0;
    memset(&g_req, 0, sizeof g_req);   // a password, perhaps
    if (rc == 0) snprintf(g_status, sizeof g_status, "Saved %d remote desktop change(s)", n);
    else if (rc == -ECANCELED) snprintf(g_status, sizeof g_status, "Not applied -- administrator rights were not given");
    else if (rc == -EINVAL) snprintf(g_status, sizeof g_status, "Not applied -- a value was refused");
    else snprintf(g_status, sizeof g_status, "Not applied -- %s", upriv_strerror(-rc));
    ulogf("settings: remote changes %d result %d\n", n, rc);
    // What is in effect after a save; the edits stay when nothing was made,
    // so asking again needs no retyping.
    if (rc == 0 && g_show_remote) remote_load();
    if (g_show_remote) relayout_page();
    uapp_redraw(a);
}

int remote_apply(void) {
    if (g_busy) return 0;
    int n = staged(&g_req);
    if (!n) { strlcpy(g_status, "Nothing to apply", sizeof g_status); return 1; }
    // ONE prompt per Apply, and it shows UPRIV_PROMPT_ROWS rows at most.
    if (n > UPRIV_PROMPT_ROWS && usetting_needs_admin_flags(0)) {
        memset(&g_req, 0, sizeof g_req);
        snprintf(g_status, sizeof g_status, "More than %d changes at once -- apply some of them first",
                 UPRIV_PROMPT_ROWS);
        return 0;
    }
    if (!g_app || uapp_run_then(g_app, apply_work, (void *)(intptr_t)n, apply_done) != 0) {
        strlcpy(g_status, "Could not ask for administrator rights", sizeof g_status);
        return 0;
    }
    g_busy = 1;
    strlcpy(g_status, usetting_needs_admin_flags(0) ? "Asking for administrator rights..." : "Saving...",
            sizeof g_status);
    return 0;
}

int remote_on_widget(int id) {
    if (!g_show_remote) return 0;
    switch (id) {
    case ID_RD_PW:
        // A new password typed over a staged removal replaces it.
        if (g_pw_remove && uui_textbox_text(&g_vnc_pw)[0]) { g_pw_remove = 0; password_state(); }
        return 1;
    case ID_RD_VNC: case ID_RD_PORT: case ID_RD_ENC: case ID_RD_WHEN: case ID_RD_FROM:
        return 1;   // staged in the control itself; the footer counts it
    case ID_RD_TRUSTED: {
        int r = g_trusted.fired_row;
        g_trusted.fired_row = -1;
        if (r < 0 || r >= g_nshown) return 1;
        if (g_shown_add[r]) {
            // An add not applied yet: forgotten.
            for (int i = 0; i < g_nadd; i++)
                if (!strcmp(g_add[i].text, g_shown[r].text)) { g_add[i] = g_add[--g_nadd]; break; }
        } else if (g_ndel < UREMOTE_TRUSTED_MAX) {
            strlcpy(g_del[g_ndel++], g_shown[r].text, sizeof g_del[0]);
        }
        build_trusted();
        return 1;
    }
    case ID_RD_CONN: {
        int r = g_conn.fired_row, b = g_conn.fired_btn;
        g_conn.fired_row = -1;
        if (r < 0 || r >= g_nsess) return 1;
        int pid = (int)g_sess[r].pid;
        if (b == 1) uremote_disconnect(pid);
        else uremote_set_view_only(pid, !strstr(g_sess_rows[r].sub, "view only"));
        ulogf("settings: remote session %d %s\n", pid, b == 1 ? "disconnect" : "view only toggled");
        return 1;
    }
    default:
        return 0;
    }
}

int remote_on_action(int code) {
    if (!g_show_remote) return 0;
    if (code == ID_RD_PW_RM) {
        if (g_vnc_pw_rm.disabled) return 1;
        uui_textbox_set_text(&g_vnc_pw, "");
        g_pw_remove = 1;
        password_state();
        strlcpy(g_status, "The VNC password is removed once you apply", sizeof g_status);
        return 1;
    }
    if (code == ID_RD_ADD) {
        struct uremote_net n;
        const char *a = uui_textbox_text(&g_add_addr), *label = uui_textbox_text(&g_add_label);
        char k[48];
        snprintf(k, sizeof k, UREMOTE_CHANGE_TRUST "%s", a);
        char v[UREMOTE_LABEL_MAX + 2];
        snprintf(v, sizeof v, "=%s", label);
        if (!uremote_parse_net(a, &n)) {
            snprintf(g_status, sizeof g_status, "\"%s\" is not an address -- try 192.168.1.20 "
                     "or 192.168.1.0/24.", a);
            return 1;
        }
        if (!uremote_change_ok(k, v)) {
            strlcpy(g_status, "A name is one line, without '#'", sizeof g_status);
            return 1;
        }
        int have = 0;
        for (int i = 0; i < g_nshown; i++) have |= !strcmp(g_shown[i].text, n.text);
        if (!have && g_nshown < UREMOTE_TRUSTED_MAX && g_nadd < UREMOTE_TRUSTED_MAX) {
            snprintf(n.label, sizeof n.label, "%s", label);
            g_add[g_nadd++] = n;
            snprintf(g_status, sizeof g_status, "%s is trusted once you apply", n.text);
        } else {
            snprintf(g_status, sizeof g_status, have ? "%s is in the list already" : "The list is full",
                     n.text);
        }
        uui_textbox_set_text(&g_add_addr, "");
        uui_textbox_set_text(&g_add_label, "");
        build_trusted();
        return 1;
    }
    return 0;
}

// Who is connected follows the machine: asked once a second.
int remote_tick(void) {
    if (!g_show_remote) return 0;
    uint64_t now = sys_monotonic_ns() / 1000000ull;
    if (now - g_checked_ms < 1000) return 0;
    g_checked_ms = now;
    int changed = build_sessions();
    if (changed) describe_cards();
    return changed;
}
