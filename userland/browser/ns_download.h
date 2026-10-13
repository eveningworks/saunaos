#ifndef BROWSER_NS_DOWNLOAD_H
#define BROWSER_NS_DOWNLOAD_H

// Downloads: NetSurf's download table (netsurf/download.h). A response
// NetSurf will not show -- an attachment, a type it has no handler for
// -- arrives here as it streams, the SAME response, not a second fetch,
// so a POST's result and a file behind the session's cookie download
// too. Written to /home/Downloads/<name>.part and renamed when whole;
// a failed or cancelled one leaves nothing behind.
#include "ui/uui_transfers.h"

#define NS_DOWNLOAD_DIR "/home/Downloads"
#define NS_DOWNLOADS_MAX 16

extern struct gui_download_table *ns_download_table;

// The list, newest first, in the shape uui_transfers draws.
const struct uui_xfer *ns_downloads(int *count);
int ns_downloads_running(void);
void ns_download_cancel(int i);
// Where the finished file is, and the address it came from.
const char *ns_download_path(int i);
const char *ns_download_url(int i);
// Forgets the finished, failed and cancelled ones.
void ns_downloads_clear_finished(void);

// --- what the app provides --------------------------------------------------
// A download started (`started`), or one changed: progress, done, failed.
void nsapp_download_changed(int started);

#endif
