// The rename table -- see api/etc_renamed.h. Shared by both rings
// (Makefile's LIBUAPP_OBJS), so it may name nothing kernel-only.
#include "etc_renamed.h"
#include "string.h"

const struct etc_renamed etc_renamed[] = {
    { "browser", "user_agent_custom", "user_agent.custom" },
    { "desktop", "corners_controls", "corners.controls" },
    { "desktop", "corners_popups", "corners.popups" },
    { "desktop", "corners_scrollbars", "corners.scrollbars" },
    { "desktop", "corners_selection", "corners.selection" },
    { "desktop", "corners_windows", "corners.windows" },
    { "desktop", "screensaver_idle", "screensaver.idle" },
    { "desktop", "start_button", "start.button" },
    { "desktop", "start_hover", "start.hover" },
    { "desktop", "start_list", "start.list" },
    { "desktop", "start_opens", "start.opens" },
    { "desktop", "start_position", "start.position" },
    { "desktop", "start_power", "start.power" },
    { "desktop", "start_recent", "start.recent" },
    { "desktop", "taskbar_align", "taskbar.align" },
    { "desktop", "taskbar_buttons", "taskbar.buttons" },
    { "desktop", "taskbar_combine", "taskbar.combine" },
    { "desktop", "taskbar_float", "taskbar.float" },
    { "desktop", "taskbar_height", "taskbar.height" },
    { "desktop", "taskbar_peek", "taskbar.peek" },
    { "desktop", "taskbar_theme", "taskbar.theme" },
    { "desktop", "transparency_menus", "transparency.menus" },
    { "desktop", "transparency_menus_glass", "transparency.menus.glass" },
    { "desktop", "transparency_moving", "transparency.moving" },
    { "desktop", "transparency_start", "transparency.start" },
    { "desktop", "transparency_start_glass", "transparency.start.glass" },
    { "desktop", "transparency_taskbar", "transparency.taskbar" },
    { "desktop", "transparency_taskbar_glass", "transparency.taskbar.glass" },
    { "desktop", "transparency_window_glass", "transparency.window.glass" },
    { "desktop", "transparency_window_opacity", "transparency.window.opacity" },
    { "desktop", "transparency_windows", "transparency.windows" },
    { "desktop", "tray_brightness", "tray.brightness" },
    { "desktop", "tray_layout", "tray.layout" },
    { "desktop", "tray_network", "tray.network" },
    { "desktop", "tray_remote", "tray.remote" },
    { "desktop", "wallpaper_live", "wallpaper.live" },
    { "desktop", "wallpaper_mode", "wallpaper.mode" },
    { "desktop", "wallpaper_pause", "wallpaper.pause" },
    { "desktop", "wallpaper_type", "wallpaper.type" },
    { "kernel", "sched_rt_runtime_ms", "sched.rt.runtime_ms" },
    { "kernel", "sched_rt_watchdog_ms", "sched.rt.watchdog_ms" },
    { "kernel", "usb_attach_delay", "usb.attach_delay" },
    { "kernel", "usb_hcreset", "usb.hcreset" },
    { "kernel", "usb_replug", "usb.replug" },
    { "kernel", "usb_reset", "usb.reset" },
    { "locale", "week_numbers", "week.numbers" },
    { "locale", "week_start", "week.start" },
    { "shortcuts", "screenshot_region", "screenshot.region" },
    { "shortcuts", "screenshot_screen", "screenshot.screen" },
    { "shortcuts", "screenshot_window", "screenshot.window" },
    { "storage", "log_keep", "log.keep" },
    { "storage", "log_max", "log.max" },
    { "system", "cursor_size", "cursor.size" },
    { "system", "cursor_style", "cursor.style" },
    { "system", "cursor_theme", "cursor.theme" },
    { "system", "font_face", "font.face" },
    { "system", "font_mono", "font.mono" },
    { "system", "font_size", "font.size" },
    { "system", "keyboard_dead_keys", "keyboard.dead_keys" },
    { "system", "keyboard_layout", "keyboard.layout" },
    { "system", "keyboard_layouts", "keyboard.layouts" },
    { "system", "mouse_accel", "mouse.accel" },
    { "system", "mouse_speed", "mouse.speed" },
    { "system", "net_recover", "net.recover" },
    { "system", "net_recover_wait", "net.recover.wait" },
    { "system", "ntp_interval", "ntp.interval" },
    { "system", "ntp_server", "ntp.server" },
    { "system", "scroll_dir", "scroll.dir" },
    { "system", "scroll_step", "scroll.step" },
    { "system", "trash_max_age", "trash.max_age" },
    { "system", "trash_max_size", "trash.max_size" },
};
const int etc_renamed_count = (int)(sizeof etc_renamed / sizeof etc_renamed[0]);

const char *etc_renamed_was(const char *key) {
    if (!key) return 0;
    for (int i = 0; i < etc_renamed_count; i++)
        if (k_strcmp(etc_renamed[i].now, key) == 0) return etc_renamed[i].was;
    return 0;
}

int etc_renamed_now(const char *name, char *out, uint32_t cap) {
    if (!name || !out || cap == 0) return 0;
    const char *dot = 0;
    for (const char *p = name; *p; p++) if (*p == '.') { dot = p; break; }
    for (int i = 0; i < etc_renamed_count; i++) {
        const struct etc_renamed *r = &etc_renamed[i];
        if (dot) {
            uint32_t n = (uint32_t)(dot - name);
            if (k_strlen(r->ns) != n || k_strncmp(r->ns, name, n) != 0) continue;
            if (k_strcmp(r->was, dot + 1) != 0) continue;
            if (k_strlcpy(out, r->ns, cap) >= cap || k_strlcat(out, ".", cap) >= cap ||
                k_strlcat(out, r->now, cap) >= cap) return 0;
            return 1;
        }
        if (k_strcmp(r->was, name) != 0) continue;
        return k_strlcpy(out, r->now, cap) < cap;
    }
    return 0;
}
