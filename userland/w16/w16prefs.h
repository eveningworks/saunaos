// w16prefs.h -- how a Win16 program runs: the defaults in /etc/win16.conf
// and one program's own overrides in /etc/win16/<key>.conf, as ONE TABLE
// (lib/uprefs.h) that w16 and the Win16 Programs launcher both read, so
// they cannot disagree about a key or its default. A program's overrides
// file holds only the keys it overrides; a missing key is "the default".
//
// Also where a program's LAST RUN is recorded (/var/lib/win16/<key>.last),
// which w16 writes and the launcher shows.
#ifndef W16PREFS_H
#define W16PREFS_H

#include <stdint.h>
#include "lib/uprefs.h"

#define W16_CONF        "/etc/win16.conf"
#define W16_PROG_DIR    "/etc/win16"
#define W16_STATE_DIR   "/var/lib/win16"
#define W16_PATH_MAX    128

// The scale: Auto, then the steps, as quarters (4 = 1x, 6 = 1.5x, 12 = 3x)
// in w16_prefs_scale_q[]. Between whole steps the METHOD says how a pixel
// is spread over 1.5 of the screen's: hard and uneven, smoothed, or
// doubled first and then smoothed down (sharp-bilinear) -- the three
// DOSBox and RetroArch offer.
enum { W16_SCALE_AUTO, W16_SCALE_1, W16_SCALE_1_25, W16_SCALE_1_5, W16_SCALE_1_75, W16_SCALE_2,
       W16_SCALE_2_5, W16_SCALE_3, W16_SCALE_COUNT };
extern const int w16_prefs_scale_q[W16_SCALE_COUNT];   // [W16_SCALE_AUTO] is 0
enum { W16_METHOD_SHARP, W16_METHOD_PIXELS, W16_METHOD_SMOOTH };
enum { W16_FRAME_31, W16_FRAME_DESKTOP };
enum { W16_VER_310, W16_VER_311, W16_VER_300 };
enum { W16_FAULT_BOX, W16_FAULT_REPORT };
// Whose fonts a program's text is drawn in: Windows 3.11's own .FON files
// from C:\WINDOWS\SYSTEM where they are there, else toy-os's stand-ins.
enum { W16_FONTS_WIN311, W16_FONTS_STANDIN };

#define W16_DRIVES 22                // D: to Y:, the letters between C: and Z:

struct w16_prefs {
    int scale;                       // W16_SCALE_*
    int frame;                       // W16_FRAME_*
    int version;                     // W16_VER_*: what GetVersion tells
    int on_fault;                    // W16_FAULT_*
    int trace;                       // log every call to w16_trace_path()
    char drive_c[W16_PATH_MAX];      // drive C:; INI files live in its WINDOWS
    int method;                      // W16_METHOD_*: how a fractional scale is drawn
    int fonts;                       // W16_FONTS_*
    // Drives D: to Y:, each a folder or "" -- C: is drive_c, Z: all of toy-os.
    char drive[W16_DRIVES][W16_PATH_MAX];
};

// The keys a program's own file may override, and the whole defaults table.
extern const struct uprefs w16_prefs_defaults;
extern const struct upref w16_prefs_keys[];
extern const int w16_prefs_program_keys;   // the first N keys are per-program

// A program's key: its path with '/' as '_', the leading one dropped --
// two copies of a program in two folders are two programs.
void w16_prog_key(const char *exe, char *out, int cap);
void w16_prog_conf(const char *exe, char *out, int cap);    // its overrides file
void w16_prog_last(const char *exe, char *out, int cap);    // its last-run record
void w16_trace_path(const char *exe, char *out, int cap);   // where its trace goes

// The defaults, then the program's overrides (exe NULL: the defaults alone).
void w16_prefs_load(struct w16_prefs *p, const char *exe);

// Which of the per-program keys `exe`'s own file sets, as a bit each.
unsigned w16_prefs_overridden(const char *exe);

// Writes `exe`'s overrides file: the keys in `mask` from `p`, nothing else
// (an empty mask removes it). 0, or -1 when it could not be written.
int w16_prefs_save_program(const char *exe, const struct w16_prefs *p, unsigned mask);

// THE LAUNCHER'S FOLDERS, as many as there are: `folders=N` and
// folderI / folderI_name in /etc/win16.conf. A file from before the count
// (folder1..folder8, empties skipped) still reads. `name` "" is "named
// for its folder".
struct w16_folder { char *path, *name; };
int  w16_folders_load(struct w16_folder **out);              // how many; *out allocated
int  w16_folders_save(const struct w16_folder *f, int n);    // 0, or -1
void w16_folders_free(struct w16_folder *f, int n);

// Auto's scale, in quarters: the step nearest this screen's width over
// 1280, so a 3.1 program looks as big as on a 1280-wide screen -- 1920
// gives 1.5x, 2560 2x, 3840 3x. 4 (1x) when the width is not known.
int w16_prefs_auto_q(void);

// The version words: what GetVersion's AX and the DOS version answer.
uint16_t w16_prefs_winver(const struct w16_prefs *p);

#endif
