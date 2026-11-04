#ifndef WM_PROF_H
#define WM_PROF_H
// WHERE A FRAME'S TIME GOES: each frame's TSC cycles charged to the stage
// that was running, by SWITCHING rather than nesting -- wm_stage(s) charges
// the time since the last switch to the stage that was current and makes
// `s` current, returning the old one to switch back to. So a shadow drawn
// inside the taskbar is the shadow's, not the taskbar's, and the stages
// add up to the frame. `gui compositor` reports the mean per stage, the
// full-screen and damage-limited buckets apart as the frame totals are.
//
// Cheap enough to leave on: one rdtsc and two adds per switch, a few
// dozen switches a frame.

enum wm_stage {
    WMS_OTHER,     // what no stage below claims: damage, culling, bookkeeping
    WMS_DESKTOP,   // the wallpaper
    WMS_ICONS,     // the desktop's icon grid over it
    WMS_SHADOW,
    WMS_GLASS,     // blur and tint behind see-through surfaces
    WMS_CHROME,    // title bars, frames, corners, the resize grip
    WMS_CLIENT,    // a window's own pixels blitted from its buffer
    WMS_TASKBAR,
    WMS_OVERLAY,   // the Start menu, flyouts, menus, animations, a drag's ghost
    WMS_CURSOR,
    WMS_PRESENT,   // the finished frame to the display
    WMS_COUNT
};

int  wm_stage(int s);
void wm_stage_frame_begin(void);
// Folds the frame into `full`'s bucket; a frame not recorded (a verify
// render) is dropped with wm_stage_frame_begin() at the next one.
void wm_stage_frame_end(int full);
// The mean cycles per stage over the bucket's frames; their count.
unsigned wm_stage_stats(int full, unsigned long long cyc[WMS_COUNT]);
const char *wm_stage_name(int s);
void wm_stage_reset(void);

#endif
