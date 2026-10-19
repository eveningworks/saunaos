#ifndef NETWORK_POPUP_H
#define NETWORK_POPUP_H

// The taskbar's network item: a state-shaped icon left of the speaker,
// opening a card (wm_flyout.h) that says which interface is up, what
// address it has, how fast the link is and what it is carrying. Same peer-file pattern as
// calendar_popup.h and brightness_popup.h -- state, drawing and
// hit-testing for one popup, sharing the WM's globals through
// wm_internal.h.
//
// WHY AN ICON WITH A PANEL, NOT AN ADDRESS IN THE STRIP. Windows 11,
// GNOME Shell and Plasma all put a state-shaped icon in the tray and
// keep the address one click away (Plasma's Details tab, macOS's
// Option-click). None of them spend panel width on an IP: the taskbar
// wants it, and the address is not what changes.
//
// ITS ACTIONS ARE /bin/netctl's, RUN AS A CHILD: the switch (`netctl
// down|up`) and Renew. netd answers "accepted" and works for seconds, and
// a compositor that waited on it would freeze; the once-a-second poll
// shows the outcome. Copy address is the clipboard's, Details... is Task
// Manager, the gear System Settings' Network pages.
//
// THREE STATES, AND THE MIDDLE ONE IS THE HONEST PART. `link_known` is
// three-valued in the ABI (query_abi.h): a driver that cannot answer is
// NOT a driver reporting "down", and the e1000 in a default QEMU guest
// is exactly that case. So the icon says "connected" only on the
// evidence of an ADDRESS, never on a link flag it may not have.
//
// WHETHER A CARD IS STILL ASKING IS netd's TO SAY, not this file's to
// guess: it pushes each card's phase (lib/unetctl.h, NETCTL_WATCH), and
// an address held while netd ASKS is not a connection -- nor is one on
// a link the driver KNOWS is down. "Connecting" is asking (for the
// first NET_CONNECTING_S seconds, then the icon stops moving and the
// card says "No address") or a link renegotiating just after the
// switch went on. Everything else still comes from QUERY_NETDEV: an address, a
// link-local one (the 169.254/16 prefix udhcp.c falls back to), or none.

// Whether the panel is currently open -- read by wm_render.c and
// wm_input.c, as with every other overlay.
extern int network_open;

// Registers the tray item. Call once, from wm.c's startup, after the
// other tray items so it lands left of the clock.
void network_tray_init(void);

// Re-reads QUERY_NETDEV and updates the icon and the item's visibility.
// Called from the WM's once-a-second tick -- see the .c file for why a
// cadence rather than a generation compare.
void network_poll(void);

// The overlay contract (wm_overlay.h).
void network_draw(int mx, int my);
int  network_handle_click(int mx, int my);
int  network_hover_at(int mx, int my);
void network_damage(void);
// Where it is, for wm_overlay.h's automatic damage. 0 when it has
// no rect to report.
int network_rect(int *x, int *y, int *w, int *h);
void network_close(void);

// The panel's rect and the tray item's, from the one geometry function
// behind drawing, hit-testing and `gui network --json`.
struct network_geom {
    int x, y, w, h;
    int tray_x, tray_y, tray_w, tray_h;
    int pad, row_h;
    int hero_h;
    int sw_x, sw_y, sw_w, sw_h;            // the adapter's switch; w 0 with no device
    int graph_x, graph_y, graph_w, graph_h; // traffic; h 0 when not shown
    int legend_y;
    int kv_y, kv_rows, kv_key_w;           // DETAILS
    int note_y;                            // the no-device sentence
    int rule1_y, cap1_y, rule2_y, cap2_y;
    int foot_y, foot_h, btn_y, btn_h;
    int copy_x, copy_w, renew_x, renew_w, details_x, details_w, gear_x, gear_w;
};
void network_geometry(struct network_geom *g);

// What the panel is currently saying, for `gui network --json`.
struct network_view {
    int  have_device;
    int  connected;          // has a routable address
    int  link_local;         // 169.254/16 -- an address, but nobody answered
    int  link_known, link_up;
    int  admin_down;         // switched off (`netctl down`)
    char name[20];
    char ip[20], mask[20], gw[20];
    int  prefix;             // the netmask's length
    char location[16];
    char driver[20];
    char mac[20];
    unsigned long long link_bps;
    unsigned long long rx_rate, tx_rate;   // bytes a second, the last sample
    int  device_count;
    int  cable_out;          // the driver KNOWS the link is down
    int  negotiating;        // ...but the card was switched on moments ago
    int  asking;             // netd is asking for a lease: any address is unconfirmed
    int  connecting;         // negotiating, or asking for under NET_CONNECTING_S
    unsigned long long asking_ms;   // for how long
};
void network_view(struct network_view *v);
// The open card's busy bar, while connecting: damage it when it has
// moved, and when the loop should next wake for it (-1: no need).
void network_animate(void);
int  network_wait_ms(void);
int  network_tray_hidden(void);

#endif
