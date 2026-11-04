# apps/system/toywm/ -> /bin/wm/system/toywm. Every .c here is linked in;
# toywm.c has main(). Fields: Makefile, "component manifests".
# The window manager is made of userland/wm/, a directory of its own
# until stage 4 of docs/tree-design.md. Listed rather than wildcarded on
# purpose: a stray .c dropped into userland/wm/ should fail to link with
# an undefined symbol, not get silently absorbed into the desktop.
EXTRA = wm/wm wm/wm_rawin wm/wm_render wm/wm_prof wm/wm_redraw wm/wm_input wm/wm_client \
                        wm/wm_debug wm/wm_tray wm/wm_taskbar wm/wm_watchdog wm/wm_watch wm/wm_shortcut \
                        wm/desktop wm/start_menu wm/start_store wm/context_menu wm/taskbar_menu \
                        wm/calendar_popup \
                        wm/wm_tooltip \
                        wm/wm_peek wm/wm_thumb wm/wm_switcher wm/wm_snap wm/wm_snaplayout wm/wm_notify wm/wm_launch wm/wm_quick \
                        wm/volume_popup wm/brightness_popup wm/tray_slider_popup wm/crash_notice wm/wm_setup_hold \
                        wm/network_popup wm/remote_popup wm/drives_popup wm/layout_popup \
                        wm/wm_overlay wm/osk \
                        wm/confirm_dialog wm/leave_page wm/close_batch wm/cursor_theme \
                        wm/gui_apps wm/wm_log wm/wm_fs wm/wm_conf \
                        wm/wm_geometry \
                        wm/wm_hwcursor wm/wm_cursor_image \
                        wm/wm_dnd wm/wm_scanout wm/wm_idle wm/wm_screenshot \
                        wm/wm_shadow wm/wm_glass wm/wm_flyout wm/wm_anim wm/wm_flash \
                        wm/wm_background
