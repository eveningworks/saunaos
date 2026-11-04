// ISO 9660 disc images, before mounting -- see base/uiso.h.
#include "base/uiso.h"
#include "kutf8.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ISO_BLOCK 2048
#define VD_FIRST 16
#define VD_LAST 100     // the kernel's bound, Linux isofs's

static int read_block(int fd, unsigned b, unsigned char *buf) {
    if (lseek(fd, (off_t)b * ISO_BLOCK, SEEK_SET) < 0) return 0;
    return read(fd, buf, ISO_BLOCK) == ISO_BLOCK;
}

// Walks the descriptor set: 1 with the label when there is a primary one.
static int scan(const char *path, char *label, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    unsigned char *vd = malloc(ISO_BLOCK);
    if (!vd) { close(fd); return -1; }
    char plain[33] = "";
    int primary = 0, joliet = 0;
    for (unsigned b = VD_FIRST; b < VD_LAST && read_block(fd, b, vd); b++) {
        if (memcmp(vd + 1, "CD001", 5) != 0 || vd[0] == 255) break;
        if (vd[0] == 1 && !primary) {
            primary = 1;
            int n = 32;
            while (n > 0 && (vd[40 + n - 1] == ' ' || vd[40 + n - 1] == 0)) n--;
            memcpy(plain, vd + 40, (size_t)n);
            plain[n] = 0;
        } else if (vd[0] == 2 && vd[88] == '%' && vd[89] == '/' &&
                   (vd[90] == '@' || vd[90] == 'C' || vd[90] == 'E') && label && cap) {
            int units = 16;
            while (units > 0 && vd[40 + 2 * units - 2] == 0 &&
                   (vd[40 + 2 * units - 1] == ' ' || vd[40 + 2 * units - 1] == 0)) units--;
            unsigned char le[32];
            for (int i = 0; i < units; i++) { le[2 * i] = vd[41 + 2 * i]; le[2 * i + 1] = vd[40 + 2 * i]; }
            joliet = kutf8_from_utf16le(le, units, label, (int)cap) >= 0;
        }
    }
    close(fd);
    free(vd);
    if (!primary) return 0;
    if (label && cap && !joliet) {
        // A-characters, but a disc is untrusted: keep what prints.
        size_t k = 0;
        for (const char *p = plain; *p && k + 1 < cap; p++)
            if ((unsigned char)*p >= 0x20 && (unsigned char)*p < 0x7f) label[k++] = *p;
        label[k] = 0;
    }
    return 1;
}

int uiso_is_image(const char *path) { return scan(path, 0, 0); }

int uiso_label(const char *path, char *out, size_t cap) {
    if (cap) out[0] = 0;
    return scan(path, out, cap) == 1 ? 0 : -1;
}
