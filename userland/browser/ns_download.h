#ifndef BROWSER_NS_DOWNLOAD_H
#define BROWSER_NS_DOWNLOAD_H

// Downloads, as the network process reports them (ns_net.c). A response
// NetSurf will not show -- an attachment, a type it has no handler for --
// is saved by /bin/webnet from the SAME response, not a second fetch, so
// a POST's result and a file behind the session's cookie download too,
// and it goes on after its tab closes. Written to
// /home/Downloads/<name>.part and renamed when whole; a failed or
// cancelled one leaves nothing behind.
#include "ui/uui_transfers.h"

#define NS_DOWNLOAD_DIR "/home/Downloads"


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
