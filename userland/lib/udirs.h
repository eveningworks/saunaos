#ifndef ULIB_UDIRS_H
#define ULIB_UDIRS_H

// THE PERSONAL FOLDERS -- Desktop, Documents, Music, Pictures, Videos,
// Downloads and Screenshots -- asked for BY NAME, never by path: XDG's
// user-dirs (`xdg-user-dir MUSIC`) and Windows' Known Folders. Each is
// /home/<Name> unless /etc/user-dirs.conf says otherwise
// (`MUSIC=/home/Audio`, one line per folder; XDG's own file is
// `XDG_MUSIC_DIR="$HOME/Music"`, read here without the decoration).
// SaunaOS has no accounts, so /home is the one user's home.
//
// What ships -- the wallpapers, the music, the documentation -- stays in
// /usr/share; these start empty and are the person's own.

enum udir {
    UDIR_DESKTOP, UDIR_DOCUMENTS, UDIR_MUSIC, UDIR_PICTURES, UDIR_VIDEOS, UDIR_DOWNLOADS,
    UDIR_SCREENSHOTS,          // inside Pictures, as Windows and GNOME keep them
    UDIR_COUNT
};

#define UDIRS_CONF "/etc/user-dirs.conf"
// The defaults, for a constant that must name one (a preference's default).
#define UDIR_DEFAULT_DESKTOP     "/home/Desktop"
#define UDIR_DEFAULT_SCREENSHOTS "/home/Pictures/Screenshots"

// The folder's path. Read from the file once per process: a move takes
// effect in a program started after it.
const char *udir_path(enum udir d);
const char *udir_label(enum udir d);   // "Music"
const char *udir_key(enum udir d);     // "music": the file's key, lower-cased
const char *udir_icon(enum udir d);    // "place-music"
// Which folder `path` is, or -1.
int udir_of(const char *path);
// Makes every folder that is missing (xdg-user-dirs-update at login) --
// but Screenshots, made by the first capture, as Windows makes it: an
// empty folder nobody has taken a screenshot into is clutter. The
// compositor calls it at start.
void udir_create_all(void);
// Makes one folder and its parents (mkdir -p); 0, or -1 if it is not there after.
int udir_make(enum udir d);
// THE MOVE FROM THE LOWER-CASE LAYOUT (/home/desktop, /home/screenshots),
// once, before anything lists them: each old folder is renamed to its
// new place, or -- when an update has already seeded the new one --
// emptied into it entry by entry and removed. How many moved.
int udir_migrate(void);

#endif
