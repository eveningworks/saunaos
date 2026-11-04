// Per-application window geometry across launches -- see wm_geometry.h
// for the key and the save point, which are the two decisions here.
#include "wm_geometry.h"
#include "wm_internal.h"
#include "wm_conf.h"
#include "gui_apps.h"
#include "wm_snap.h"
#include "lib/usetting.h"
#include "string.h"
#include "knum.h"
#include "kfmt.h"

#define WINDOWS_CONF_PATH "/etc/windows.conf"

// READ FRESH ON EVERY RESTORE, deliberately not cached.
//
// The first version cached the parsed file and invalidated it only when
// this module's own save rewrote it -- which makes the WM blind to any
// other writer, including a person editing /etc/windows.conf and the
// test that writes a known geometry before opening a window. It failed
// exactly there, and the general shape is worth naming: a cache
// invalidated only by its owner's writes is correct only while its
// owner is the only writer, and nothing said that it was.
//
// The cost is one whole-file read per window OPENED, which is rare and
// already expensive. wm_conf.h's warning is about re-reading per KEY in
// a loop (the nine-entry desktop reload that cost 54 reads); one read
// per window is not that.
// Held between a geom_load() and the next, which frees it first.
static struct etc_config_buf g_cfg;

static void geom_load(void) {
    etc_config_buf_free(&g_cfg);
    wm_conf_load(WINDOWS_CONF_PATH, &g_cfg);  // missing file: every get says 0
}

// The app's stable identifier, or NULL for a window that has none.
//
// A client and a kernel-space app keep it in different places -- a
// client window has no `gui_app` by construction (wm.h) -- and neither
// is the taskbar's grouping int. "" is treated as absent: a client that
// named nothing must not share a slot with every other client that
// named nothing.
static const char *geom_key(const struct window *win) {
    if (!win) return 0;
    if (win->app && win->app->key && win->app->key[0]) return win->app->key;
    if (win->app_key[0]) return win->app_key;
    if (win->app_id[0]) return win->app_id;
    return 0;
}

// The key a size was saved under before keys were paths: the declared
// app_id. Read once as a fallback so nobody's window sizes reset; the
// next save writes the new key.
static const char *legacy_key(const struct window *win) {
    if (win->app && win->app->app_id && win->app->app_id[0]) return win->app->app_id;
    return win->app_id[0] ? win->app_id : 0;
}

// Does this app want its geometry remembered? Looked up in the registry
// rather than read off the window, because a client window has no
// `gui_app` to carry the flag and both kinds are launched from the same
// `.desktop` entry. An app with no registry entry at all (a window from
// something the launcher never heard of) is remembered -- the default
// is on, and an unknown app is not a reason to refuse.
static int remembers(const char *key) {
    // `desktop.windows.remember`: off forgets nothing on disk, it only
    // stops reading and writing -- turned back on, the old sizes return.
    char v[16];
    if (usetting_get("desktop.windows.remember", v, sizeof v) && k_strcmp(v, "off") == 0)
        return 0;
    for (int i = 0; i < gui_app_registry_count; i++) {
        const struct gui_app *a = &gui_app_registry[i];
        if ((a->key && k_strcmp(a->key, key) == 0) || (a->app_id && k_strcmp(a->app_id, key) == 0))
            return a->remember_geometry;
    }
    return 1;
}

// "x,y,w,h" -- four signed decimals -- then, for a window that closed
// maximized or snapped, ",max" or ",snap:<zone>" (wm_snap.h's packing,
// in decimal), with x,y,w,h its RESTORE rect. Returns 0 unless every
// part parses, so a hand-edited or truncated line leaves the default
// placement rather than a window at a partly-parsed position. A build
// from before the state field reads such a line as unparsable, which
// is the same safe answer.
#define GEOM_MAX 0x10000
static int parse_geom(const char *s, int *x, int *y, int *w, int *h, unsigned *state) {
    *state = 0;
    int *out[4] = { x, y, w, h };
    for (int i = 0; i < 4; i++) {
        if (!*s) return 0;
        int neg = 0;
        if (*s == '-') { neg = 1; s++; }
        if (*s < '0' || *s > '9') return 0;
        int v = 0;
        while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
        *out[i] = neg ? -v : v;
        if (i < 3) { if (*s != ',') return 0; s++; }
    }
    if (*s == '\0') return 1;
    if (k_strcmp(s, ",max") == 0) { *state = GEOM_MAX; return 1; }
    if (k_strncmp(s, ",snap:", 6) != 0) return 0;
    s += 6;
    unsigned z = 0;
    if (*s < '0' || *s > '9') return 0;
    while (*s >= '0' && *s <= '9' && z < 0x10000) z = z * 10 + (unsigned)(*s++ - '0');
    if (*s || !z || z >= 0x10000) return 0;
    *state = z;
    return 1;
}

// A restore is applying a state through the same calls a person makes;
// those save, and must not while the file is being read back.
static int g_restoring;

static void save_sized(const struct window *win, int want_w, int want_h);

void wm_geometry_save(const struct window *win) { save_sized(win, 0, 0); }
void wm_geometry_save_size(const struct window *win, int w, int h) { save_sized(win, w, h); }

static void save_sized(const struct window *win, int want_w, int want_h) {
    if (win->popup || g_restoring || win->geom_forgot) return; // a popup is placed by its client every time
    const char *key = geom_key(win);
    if (!key || !remembers(key)) return;

    // A MAXIMIZED WINDOW SAVES ITS RESTORE RECT, not the screen-sized
    // one it currently fills. Saving the maximized geometry would
    // reopen the app at exactly screen size but NOT maximized -- a
    // window that looks maximized, has no restore size to go back to,
    // and covers the taskbar. The restore rect is already maintained
    // for the un-maximize path (wm_input.c), so it costs nothing.
    // So does a SNAPPED one, whose saved_* is its own rect too
    // (wm_snap.h); the state rides after it and is applied on top.
    int x = win->x, y = win->y, w, h;
    wm_window_heading_size(win, &w, &h);   // a client may not have adopted it yet
    if (want_w > 0 && want_h > 0) { w = want_w; h = want_h; }
    int max = wm_window_maximized(win) ||
              (win->fullscreen && win->fs_prev == WIN_MAXIMIZED);
    if (max || win->fullscreen || win->snap) {
        x = win->saved_x; y = win->saved_y;
        w = win->saved_w; h = win->saved_h;
    }
    // A MINIMIZED window keeps its real geometry -- minimizing does not
    // move it -- and one minimized from maximized is caught above.
    if (w <= 0 || h <= 0) return;

    char value[64];
    if (max)            k_snprintf(value, sizeof value, "%d,%d,%d,%d,max", x, y, w, h);
    else if (win->snap) k_snprintf(value, sizeof value, "%d,%d,%d,%d,snap:%u", x, y, w, h, (unsigned)win->snap);
    else                k_snprintf(value, sizeof value, "%d,%d,%d,%d", x, y, w, h);
    // Unchanged: no write. A drag that ends where it began, or a save at
    // close after one at the drag's end, costs a read, not a rewrite.
    char was[64];
    if (wm_conf_get(WINDOWS_CONF_PATH, key, was, sizeof was) && k_strcmp(was, value) == 0) return;
    wm_conf_set(WINDOWS_CONF_PATH, key, value);
}

int wm_geometry_restore(int idx) {
    if (idx < 0 || idx >= window_count) return 0;
    struct window *win = &windows[idx];
    const char *key = geom_key(win);
    if (!key || !remembers(key)) return 0;

    geom_load();
    char value[64];
    const char *old = legacy_key(win);
    if (!etc_config_buf_get(&g_cfg, key, value, sizeof value) &&
        !(old && etc_config_buf_get(&g_cfg, old, value, sizeof value)))
        return 0;

    int x, y, w, h;
    unsigned state;
    if (!parse_geom(value, &x, &y, &w, &h, &state)) return 0;
    // Restored AGAIN before its first frame (a client that turns out to
    // draw its own frame, wm_client.c): start from a plain window.
    if (win->state == WIN_MAXIMIZED) win->state = WIN_NORMAL;
    win->snap = 0;

    // A SAVED SIZE FROM A BIGGER SCREEN MUST NOT BE RESTORED WHOLE.
    // The display can change between runs -- a different monitor, a
    // `video=` boot flag, a runtime mode switch -- and a window wider
    // than the screen has a title bar whose buttons are off the right
    // edge, i.e. one that cannot be closed with the mouse.
    int max_w = screen_w;
    int max_h = screen_h - taskbar_h;
    if (w > max_w) w = max_w;
    if (h > max_h) h = max_h;
    if (w < 1 || h < 1) return 0;

    // AND THE POSITION IS CLAMPED FULLY ON-SCREEN, which is a STRICTER
    // rule than the WM's general one and deliberately so. A window may
    // be DRAGGED off the left, right and bottom edges on purpose
    // (docs/gui-guidelines.md), so wm_ensure_reachable() only guarantees
    // a sliver stays grabbable -- restoring 4000x3000 at (5000,5000)
    // through it produced a 1280-wide window at x=1216, i.e. 64 pixels
    // of it on screen. That is the right answer for a drag the user
    // meant and the wrong one for a position they never chose: the
    // screen may simply be smaller than it was last time.
    // Against the SCREEN, not the work area: a window may legitimately
    // sit over the taskbar (docs/gui-guidelines.md allows dragging one
    // there), so clamping to the work area would quietly MOVE a window
    // the user had deliberately placed -- 200,150 came back as 200,120
    // the first time this was tried. Off the screen entirely is the
    // only thing being prevented here.
    if (x > screen_w - w) x = screen_w - w;
    if (y > screen_h - h) y = screen_h - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;   // the title bar must be reachable

    win->x = x; win->y = y;

    // A CLIENT OWNS ITS OWN BUFFER, so its size is a REQUEST, not an
    // assignment: the WM cannot widen a window whose pixels the client
    // allocated, and **a client may DECLINE** -- a fixed-size app
    // always does. So the frame keeps the size it has until a present
    // answers, which carries the buffer's real dimensions and is
    // adopted in on_window_present() for an accepted resize and a
    // refused one alike.
    //
    // Assigning it here as well was the bug: a declined proposal left
    // the frame at the saved size with the client still drawing its
    // own, i.e. a small window painted into the top-left corner of a
    // big one, and nothing afterwards could correct it -- the present
    // that would have carried the truth matched `client_w/h`, which
    // this had not touched.
    //
    // AND AGAINST WHAT WAS LAST ASKED, not only what the client has: a
    // client that draws its own frame is restored twice before its first
    // frame -- at create, its chrome subtracted, then again when its
    // NO_DECOR hint says there is none (wm_client.c) -- and the second
    // answer equals the size it was created at, so comparing with that
    // alone never sent it, and the first, smaller proposal stood: w16's
    // Control Panel lost the chrome's size at every launch.
    if (wm_client_is_client_window(win)) {
        int content_w = w - wm_chrome_w(win);
        int content_h = h - wm_chrome_h(win);
        int asked = win->ev_resize_w > 0 &&
                    (content_w != win->ev_resize_w || content_h != win->ev_resize_h);
        if (content_w > 0 && content_h > 0 &&
            (content_w != win->client_w || content_h != win->client_h || asked)) {
            wm_client_send_resize(win, content_w, content_h);
        }
    } else {
        win->w = w; win->h = h;
    }

    // Belt and braces: the clamp above is arithmetic on this window,
    // wm_ensure_reachable() is the WM's own invariant. If they ever
    // disagree the WM's wins.
    wm_ensure_reachable(idx);

    win->geom_state = state;
    wm_geometry_apply_state(idx);
    return 1;
}

// THE STATE IT CLOSED IN, through the calls a person's click makes --
// each keeps the rect just restored as the one to go back to. Only once
// the window says it is resizable; a client says so in its hints, after
// the create that restored it, and wm_client.c calls this again then.
void wm_geometry_apply_state(int idx) {
    if (idx < 0 || idx >= window_count) return;
    struct window *win = &windows[idx];
    if (!win->geom_state || !win->resizable) return;
    unsigned state = win->geom_state;
    win->geom_state = 0;
    g_restoring = 1;
    if (state == GEOM_MAX) wm_set_maximized(idx, 1);
    else wm_snap_window(idx, (uint16_t)state);
    g_restoring = 0;
}

static int saved_value(const struct window *win, char *out, uint32_t cap) {
    const char *key = geom_key(win);
    if (!key || win->popup) return 0;
    return wm_conf_get(WINDOWS_CONF_PATH, key, out, cap) != 0;
}

int wm_geometry_saved(const struct window *win) {
    char v[64];
    const char *key = geom_key(win);
    return key && remembers(key) && saved_value(win, v, sizeof v);
}

void wm_geometry_forget(struct window *win) {
    const char *key = geom_key(win), *old = legacy_key(win);
    if (!key) return;
    win->geom_forgot = 1;
    wm_conf_unset(WINDOWS_CONF_PATH, key);
    if (old && k_strcmp(old, key) != 0) wm_conf_unset(WINDOWS_CONF_PATH, old);
}

void wm_geometry_fit(int idx) {
    if (idx < 0 || idx >= window_count) return;
    struct window *win = &windows[idx];
    if (win->popup || win->fullscreen) return;    // its client places it

    int work_w = screen_w, work_h = screen_h - taskbar_h;
    int w = win->w < work_w ? win->w : work_w;
    int h = win->h < work_h ? win->h : work_h;
    int x = win->x, y = win->y;
    if (x > work_w - w) x = work_w - w;
    if (y > work_h - h) y = work_h - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    win->x = x; win->y = y;
    if (w == win->w && h == win->h) return;

    // Too big for the work area: a REQUEST, as in wm_geometry_restore()
    // -- the client owns its buffer, and a fixed-size one declines.
    if (wm_client_is_client_window(win)) {
        int content_w = w - wm_chrome_w(win), content_h = h - wm_chrome_h(win);
        if (content_w > 0 && content_h > 0) wm_client_send_resize(win, content_w, content_h);
    } else {
        win->w = w; win->h = h;
    }
}
