#ifndef UPDATE_UPD_H
#define UPDATE_UPD_H

// The System Update engine: one copy, behind /bin/update and the System
// Update window (docs/update-design.md). A static HTTP server carries a
// manifest and the files; this fetches the manifest, compares it with
// the machine, downloads what differs to `<path>.upd`, verifies each,
// and then commits -- in place when that is safe, or staged for the
// kernel to finish at the next boot (abi/update_abi.h) when a library
// or the kernel is in the set.
//
// **SIZE AND CRC32 DECIDE; DATES NEVER DO.** A machine with a dead CMOS
// battery would otherwise refuse every update or take every one. The
// crc is INTEGRITY, not authenticity: it arrives from the same
// unauthenticated server as the file.
//
// Not thread-safe in itself. The GUI runs upd_check()/upd_apply() on a
// worker thread and only READS the plan from its own, which is safe
// because every field a reader looks at is a single int written whole.
#include <stddef.h>
#include <stdint.h>

#define UPD_URL_MAX   256
#define UPD_PATH_MAX  192
#define UPD_LOG_PATH  "/var/log/update.log"
#define UPD_SETTING   "update.server"
#define UPD_CHANNEL_SETTING  "update.channel"        // "" = the server's default
#define UPD_INTERNAL_SETTING "update.show_internal"  // list internal channels on a released image
#define UPD_RECENT_PATH "/var/lib/update/servers"
#define UPD_RECENT_MAX 5
// The manifest this machine last applied -- dpkg's file list. What it
// listed and the next manifest does not is STALE and removed, provided
// it still has the crc it was shipped with: an edited file is kept and
// said so, and a file no manifest ever listed is never touched.
#define UPD_INSTALLED_PATH "/var/lib/update/installed"
// A shipped /etc file the owner EDITED is kept, and the new version
// lands here under its own path -- rpm's .rpmnew, but out of the
// directory: init loads every file in /etc/services.d, so a `.new`
// beside a descriptor would start the service twice.
#define UPD_NEW_DIR "/var/lib/update/new"

// What the manifest says to do with a file. KERNEL lines come in two
// forms, the ELF and the gzipped image, and the machine's own GRUB
// decides which one it can boot (see upd.c's kernel_variant()).
// /etc, /home: the machine's own once edited. A shipped change replaces
// a file only while it still has the crc the record says was shipped;
// an edited one is kept and the new version goes to UPD_NEW_DIR.
#define UPD_F_NEW_ONLY  0x1
#define UPD_F_KERNEL    0x2   // the ELF kernel image
#define UPD_F_KERNEL_GZ 0x4   // the same kernel, gzipped, for a GRUB with gzio
#define UPD_F_IF_MISSING 0x8  // written only where absent, never compared or replaced

// UPD_REMOVE: shipped last time, no longer -- a row upd_check() appends
// after the manifest's own (see UPD_INSTALLED_PATH).
enum upd_change { UPD_SAME = 0, UPD_CHANGED, UPD_NEW, UPD_IGNORED, UPD_REMOVE };

enum upd_status {
    UPD_QUEUED = 0,
    UPD_FETCHING,
    UPD_STAGED,        // downloaded and verified, as <path>.upd
    UPD_INSTALLED,     // renamed into place
    UPD_AT_BOOT,       // will be renamed into place (or removed) by the next boot
    UPD_FAILED,
    UPD_REMOVED,       // a stale file, deleted
};

struct upd_file {
    char     path[UPD_PATH_MAX];  // where it lives on THIS machine
    char     src[UPD_PATH_MAX];   // where it lives on the server, under /files
    uint32_t crc;
    uint64_t size;
    unsigned flags;               // UPD_F_*
    int      change;              // enum upd_change
    int      status;              // enum upd_status, meaningful when change is CHANGED/NEW
    uint64_t got;                 // bytes of it fetched so far
};

struct upd_plan {
    char base[UPD_URL_MAX];
    char version[48];             // "# version" in the manifest, or ""
    char built[48];               // "# built": read, and compared only to tell an older BUILD
                                  // (upd_build_cmp) -- never to pick a file
    char commit[24];              // "# commit": the build this manifest is, or ""
    struct upd_file *files;       // every manifest line this machine uses
    int count;

    // Filled by upd_check().
    int      changed;             // files to fetch
    uint64_t bytes;               // their total size
    int      removals;            // stale files to remove (UPD_REMOVE rows)
    int      kept_edited;         // stale, but edited here, so kept
    int      conf_kept;           // a changed /etc file edited here: kept, new one beside
    int      conf_removed;        // an /etc file removed here (a disabled service): not reinstalled
    char    *manifest;            // the raw text, recorded once applied
    int      staged_for_boot;     // an earlier run already staged an update (restart pending)
    int      kernel_blocked;      // the kernel changed and may not be installed: nothing will be
    // This build is OLDER than the one the machine runs (upd_build_cmp()):
    // installing it goes back. A caller asks before it does.
    int      behind;

    // The release notes (GET <base>/notes), cut at THIS machine's build
    // as apt-listchanges cuts a changelog: Markdown for uui_markdown and
    // umd, or NULL when the server has none or nothing is newer.
    char    *notes;
    int      notes_count;         // release-note lines in it, the visible kinds
    int      notes_hood;          // ...and `internal` ones, under the hood
    int      notes_quiet;         // newer commits that are not the OS's (never shown)
    int      notes_since;         // 1: cut at this machine's build; 0: it was not in the list

    // Filled by upd_apply().
    uint64_t done_bytes;
    int      at_boot;             // the set will finish at the next boot
    int      kernel_installed;    // a new kernel is in /boot: a restart is needed to run it
    int      failed;
    char     error[160];          // why the whole run stopped, or ""
};

struct upd_hooks {
    void *ctx;
    // One finished line, no newline, already written to UPD_LOG_PATH.
    void (*log)(void *ctx, const char *line);
    // Something about file `idx` changed (-1: the plan as a whole).
    // Called often while fetching; a caller throttles its own drawing.
    void (*progress)(void *ctx, const struct upd_plan *p, int idx);
    // Polled between reads; non-zero stops the run at the next file
    // boundary with nothing committed.
    int (*cancelled)(void *ctx);
};

// Fetch the manifest from `base` (http://host:port[/prefix]) and decide
// what differs. 0 on success -- including "nothing to do" -- or -1 with
// p->error set. `p` must be zeroed or freed first.
int upd_check(const char *base, struct upd_plan *p, const struct upd_hooks *h);

// Download, verify, and commit every CHANGED/NEW file. All of them are
// fetched and verified BEFORE anything is committed, so a failed or
// cancelled download changes nothing. 0 on success, -1 otherwise.
int upd_apply(struct upd_plan *p, const struct upd_hooks *h);

void upd_plan_free(struct upd_plan *p);

// ---- channels ------------------------------------------------------------------
//
// ONE ADDRESS, THE SERVER'S OWN LIST OF CHANNELS: GET <server>/channels
// names each (tools/update_server.py has the format) and which is the
// default; the machine keeps its choice in UPD_CHANNEL_SETTING. OSTree's
// remote and the refs its summary lists. A server with no list -- an
// older one, or an address that already names one channel -- is used as
// it is, one channel, as before.

struct upd_channel {
    char id[24];          // what the URL and the setting say: "main"
    char title[32];       // what a person reads: "Main"
    int  internal;        // listed only where upd_show_internal()
    char version[48];     // its build's "# version", "" while it has none
    char built[48];       // and "# built"
    char desc[96];
};

// AS MANY CHANNELS AS THE SERVER DECLARES (lib/ugrow.h): `ch` is the
// struct's own, kept and reused by the next fetch into it -- a zeroed
// struct is an empty list, and the array is never freed.
struct upd_channels {
    int count, cap;
    struct upd_channel *ch;
    int dflt;             // index of the server's default, or -1
    int chosen;           // index upd_resolve() used, or -1 for a server with no list
};

// The channel list of `server`. 1 with `out` filled, 0 when the server
// declares none (an old one, or a channel's own address), -1 when it
// could not be asked (`err` says why).
int upd_channels_fetch(const char *server, struct upd_channels *out, char *err, size_t errcap);

// The address to check: the server's chosen channel -- `channel`, else
// the setting, else the server's default -- or the server itself when it
// declares none. A SETTING STILL NAMING ONE CHANNEL'S ADDRESS
// (".../stable", from before the list) is split into the server and that
// channel, and both settings are written. 0, or -1 with `err` set.
int upd_resolve(const char *server, const char *channel, char *url, size_t cap,
                struct upd_channels *chs, char *err, size_t errcap);

// The setting; "" for the server's default.
char *upd_channel_get(char *out, size_t cap);
int   upd_channel_set(const char *id);
// List internal channels: on a development build, or when asked to.
int   upd_show_internal(void);

// Builds by (version, built): <0 when a is older than b, 0 the same, >0
// newer. A version without a pre-release part is newer than the same
// one with it (0.6.0 > 0.6.0-dev); equal versions go by `built`, and an
// empty `built` compares equal to anything.
int upd_build_cmp(const char *va, const char *ba, const char *vb, const char *bb);
// The build this machine runs: the manifest it last applied, else its
// kernel's version with no date.
void upd_machine_build(char *ver, size_t vcap, char *built, size_t bcap);

// The server address: the `update.server` setting, else its declared
// default. Returns `out`.
char *upd_server_get(char *out, size_t cap);
// Store it as the setting and move it to the front of the recent list.
// 0, or -1 when the setting refused it (too long, or not http://).
int upd_server_set(const char *url);
// Recently used servers, most recent first. Returns how many.
int upd_recent(char out[][UPD_URL_MAX], int max);

#endif
