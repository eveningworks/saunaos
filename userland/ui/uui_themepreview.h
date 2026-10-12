#ifndef UUI_THEMEPREVIEW_H
#define UUI_THEMEPREVIEW_H

// A THEME, drawn small: a desktop with a taskbar and one window -- title
// bar and caption buttons, a sidebar with a selected row, a field, a
// control and the accent button -- in a palette that is NOT the live
// one. The gallery cards in System Settings and the Theme Editor's
// preview are both this widget, so what a theme looks like is drawn one
// way. Scales from a card to a page: text labels appear once there is
// room for them, and below that the same places are bars.

#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui_widget.h"

struct uui_themepreview {
    int x, y, w, h;
    const struct utheme *theme;   // borrowed; NULL draws the live one
    int taskbar_light;            // the bundle's desktop.taskbar_theme
    int corner;                   // the window's corner radius, px at full size
    uint32_t desk;                // the desktop behind the window
};

void uui_themepreview_init(struct uui_themepreview *p, const struct utheme *t);
void uui_themepreview_draw(struct ugfx_surface *s, const struct uui_themepreview *p);

extern const struct uui_widget_ops uui_themepreview_ops;

#endif
