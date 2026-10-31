#ifndef UUI_DRIVE_H
#define UUI_DRIVE_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_widget.h"

// REMOVABLE DRIVES, A ROW EACH: the stick's icon, its name, what it holds
// ("32.0 GiB FAT32 \xb7 25.3 GiB free"), then Open and an Eject button at
// the right. The tray's USB flyout draws these; KDE's Disks & Devices
// applet and Windows' "Safely Remove Hardware" list are this row. A stick
// with SEVERAL volumes is a stick row (its size, the one Eject) with a
// row per volume indented under it, each with its own Open -- GNOME
// Disks' and Plasma's grouping; one volume stays one row.
//
// THE CALLER OWNS THE ROWS and refills them as sticks come and go
// (uui_drive_fill() from lib/udrive.h's list); the widget keeps which
// button is hovered or pressed, and forgets a press whose row has gone.
// A press arms a button and the release over the same one fires it:
// read `fired_row` / `fired_btn` (UUI_DRIVE_OPEN or UUI_DRIVE_EJECT).
//
// Drawn either through the ops (on a card of its own, among settings
// rows) or directly with uui_drive_draw() on a caller's ground -- the
// compositor's flyout, which hands it one click rather than a press and
// a release, calls press then release itself.

struct udrive;

enum { UUI_DRIVE_OPEN = 0, UUI_DRIVE_EJECT = 1, UUI_DRIVE_BTNS = 2 };

struct uui_drive_row {
    char title[64];
    char sub[96];
    const char *icon;     // an icon_get() name; NULL for "drive-usb"
    int can_open;         // Open is offered: the volume is mounted
    int can_eject;        // Eject is offered: the row is a whole stick
    int indent;           // a volume under the stick row above it
    int busy;             // an Eject is under way: its button is greyed
};

struct uui_drive {
    int x, y, w, h;
    struct uui_drive_row *rows;            // caller-owned
    int count;
    const char *empty;                     // shown when count is 0; may be NULL
    int hover_row, hover_btn;              // OWNED
    int press_row, press_btn;              // OWNED
    int fired_row, fired_btn;              // the last button released over; -1 before
};

void uui_drive_init(struct uui_drive *l, struct uui_drive_row *rows, int count,
                    const char *empty);
// A row from a stick's only volume: its name, the describe line, Open
// when mounted, Eject.
void uui_drive_fill(struct uui_drive_row *r, const struct udrive *d);
// The stick row over `volumes` of them (`d` is any one): its model, its
// size and the count, Eject only.
void uui_drive_fill_stick(struct uui_drive_row *r, const struct udrive *d, int volumes);
// One of those volumes, indented: its label or "FAT32 volume", its space,
// Open when mounted.
void uui_drive_fill_volume(struct uui_drive_row *r, const struct udrive *d);
int  uui_drive_row_h(void);
// Button `btn` of row `row`; 0 when that row has no such button.
int  uui_drive_button_rect(const struct uui_drive *l, int row, int btn,
                           int *x, int *y, int *w, int *h);
// The rows on a ground of `bg` (UGFX_TRANSPARENT blends, as a flyout's ink).
void uui_drive_draw(struct ugfx_surface *s, const struct uui_drive *l, uint32_t bg);
// 1 when the hovered button changed (redraw).
int  uui_drive_hover(struct uui_drive *l, int cx, int cy);
// 1 when a button was pressed.
int  uui_drive_press(struct uui_drive *l, int cx, int cy);
// 2 when the press fired (fired_row/fired_btn are set), 1 when a press
// ended elsewhere, 0 when nothing was pressed.
int  uui_drive_release(struct uui_drive *l, int cx, int cy);

extern const struct uui_widget_ops uui_drive_ops;

#endif
