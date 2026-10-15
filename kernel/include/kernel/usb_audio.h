#ifndef USB_AUDIO_H
#define USB_AUDIO_H

#include <stdint.h>
// The types and the parser are SHARED WITH RING 3 and live in api/;
// what is below is the ring-0 half -- binding a device and driving its
// endpoint, which only the kernel can do.
#include "usb_audio_parse.h"

struct usb_device_info;


// Is a USB audio device bound? The KTESTs' skip condition.
int usb_audio_bound(void);

#endif
