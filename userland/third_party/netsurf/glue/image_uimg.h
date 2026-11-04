#ifndef NS_IMAGE_UIMG_H
#define NS_IMAGE_UIMG_H

// PNG, JPEG and QOI content for NetSurf, decoded by lib/uimg.h -- large
// ones in a thread (image_uimg.c). After netsurf_init().
#include "utils/errors.h"

nserror ns_image_uimg_register(void);

#endif
