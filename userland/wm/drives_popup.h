#ifndef WM_DRIVES_POPUP_H
#define WM_DRIVES_POPUP_H

// THE TRAY'S USB-DRIVES ITEM: shown while a removable disk is plugged in
// (`desktop.tray.usb`), and a flyout listing each volume with Open and
// Eject, Eject all, and a gear to Disks. KDE's Disks & Devices applet is
// the shape, including what makes it more than a list: the flyout OPENS
// ITSELF for a few seconds when a stick is plugged in and mounted, so the
// person sees it arrive without hunting for it.
//
// THE COMPOSITOR NEVER TOUCHES THE DISK. Eject is asked of automountd
// (lib/udrive.h); the row goes when the disk does, and a refusal comes
// back as a reply this poll reads.
//
// It also says what happened to a stick, from the kernel's device events
// (QUERY_DEVEVENT): "<name> can be pulled out" after an Eject from
// anywhere -- here, the File Manager, `eject` -- and, when a stick that
// had a mounted volume went without one, a notice that stays until read.

struct drives_geom {
    int x, y, w, h;                  // the flyout
    int tray_x, tray_y, tray_w, tray_h;
    int pad, hero_h;
    int list_y, list_h;              // the rows (ui/uui_drive.h)
    int foot_y, foot_h, btn_y, btn_h;
    int all_x, all_w;                // Eject all
    int gear_x, gear_w;              // Disks
};

extern int drives_open;

void drives_tray_init(void);
// Once a second, from wm.c's clock block.
void drives_poll(void);
void drives_geometry(struct drives_geom *g);
void drives_draw(int mx, int my);
int  drives_handle_click(int mx, int my);
int  drives_hover_at(int mx, int my);
void drives_damage(void);
int  drives_rect(int *x, int *y, int *w, int *h);
void drives_close(void);
int  drives_tray_hidden(void);

// For `gui drives --json` (wm_debug.c): the rows, a button's rect
// (UUI_DRIVE_OPEN / UUI_DRIVE_EJECT; 0 when the row has none), and
// whether the open flyout opened itself.
int  drives_count(void);
const char *drives_row_title(int i);
const char *drives_row_sub(int i);
const char *drives_row_point(int i);
int  drives_row_busy(int i);
int  drives_row_indent(int i);   // a volume under its stick's row
int  drives_disk_count(void);    // sticks, not volumes: the header's count
int  drives_button(int row, int btn, int r[4]);
int  drives_opened_itself(void);

#endif // WM_DRIVES_POPUP_H
