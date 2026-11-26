// The Leave page's Restart, asked from outside -- see uleave.h.
#include "ui/uleave.h"
#include "ui/uapp_internal.h"   // wmchan_call(): the compositor channel
#include "win_proto.h"
#include <errno.h>

int uleave_restart(void) {
    if (!wmchan()) { errno = ENXIO; return -1; }
    int rc = wmchan_call(WIN_REQ_LEAVE, 0, WIN_LEAVE_RESTART, 0, 0, 0, -ENXIO);   // no reply: no desktop
    if (rc < 0) { errno = -rc; return -1; }
    return 0;
}
