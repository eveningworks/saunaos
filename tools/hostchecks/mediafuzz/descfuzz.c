// The kernel's USB device-reply parsers on the host (usbdesc_hostcheck.py), against hostile
// bytes: USB Audio Class descriptors (kernel/lib/usb_audio_parse.c, also
// ring 3's usbaudio) and SCSI READ CAPACITY replies (kernel/lib/scsi.c).
//
// INPUT: byte 0 picks the parser (0 UAC, 1 READ CAPACITY(10), 2 (16)),
// the rest is the device's reply. Each parser's ACCEPTED answer is held
// to what its caller then assumes -- the sanitizers alone cannot see a
// divide by zero that happens in the driver, after the parse returned.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "usb_audio_parse.h"
#include "scsi.h"

static void bad(const char *what) {
    fprintf(stderr, "INVARIANT: %s\n", what);
    abort();
}

static void uac(const uint8_t *d, size_t n) {
    struct usb_audio_stream s;
    struct usb_audio_report rep;
    memset(&s, 0, sizeof s);
    if (!usb_audio_parse(d, (uint32_t)n, &s, &rep)) return;
    printf("uac: accepted, %u format(s)\n", s.nformats);
    // sound_usb.c's bind sizes its packets from formats[fmt].subslot and
    // divides by it; usbaudio.c programs the endpoint from s.ep.
    if (s.nformats < 1 || s.nformats > USB_AUDIO_MAX_FORMATS) bad("an accepted stream has no format");
    if (s.subslot < 2 || s.subslot > 4) bad("the default's subslot is not 2-4");
    for (int i = 0; i < s.nformats; i++) {
        const struct usb_audio_format *f = &s.formats[i];
        if (f->subslot < 2 || f->subslot > 4) bad("a kept format's subslot is not 2-4");
        if (f->ep != s.ep) bad("a kept format is on another endpoint");
    }
}

static void capacity(const uint8_t *d, size_t n, int sixteen) {
    uint64_t last = 0;
    uint32_t block = 0;
    int ok = sixteen ? scsi_parse_capacity16(d, (uint32_t)n, &last, &block)
                     : scsi_parse_capacity10(d, (uint32_t)n, &last, &block);
    if (!ok) return;
    // usb_storage.c: blocks = last + 1, sectors = blocks * (block / 512).
    if (last == UINT64_MAX) bad("an accepted capacity has last + 1 == 0");
    if ((last + 1) > UINT64_MAX / (block / 512)) bad("an accepted capacity overflows in sectors");
}

static void run_one(const uint8_t *d, size_t n) {
    if (n < 1) return;
    // Copied to a buffer of exactly its size, so a read past it is ASan's.
    uint8_t *b = malloc(n - 1 ? n - 1 : 1);
    memcpy(b, d + 1, n - 1);
    switch (d[0] % 3) {
    case 0: uac(b, n - 1); break;
    case 1: capacity(b, n - 1, 0); break;
    case 2: capacity(b, n - 1, 1); break;
    }
    free(b);
}

#ifdef FZ_LIBFUZZER
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) { run_one(d, n); return 0; }
#else
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) { perror(argv[i]); return 2; }
        static uint8_t d[1 << 16];
        size_t n = fread(d, 1, sizeof d, f);
        fclose(f);
        run_one(d, n);
    }
    return 0;
}
#endif
