// bench32.c -- the 32-bit work tools/bench/cpu16_bench.py times w16's CPU
// on: what a 1990s game's 32-bit code spends its time in, written as plain
// C and compiled -m32 into a flat blob (no library, no relocations). An
// LZSS decompressor (Sierra's resources and Robot videos are LZS), a
// transparent sprite blit, a CRC, and an insertion sort -- byte loads and
// stores, tight branches, shifts, a call or two.
//
// The driver puts the blob at offset 0 of a 32-bit code segment and calls
// bench(buf) cdecl, with a flat 32-bit data and stack segment. The return
// value is a checksum the driver prints, so a broken CPU shows as a number
// that changed, not just as a speed.
#include <stdint.h>

#define BUF_SIZE (1u << 20)

static uint32_t crc32(const uint8_t *p, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & -(c & 1));
    }
    return ~c;
}

// LZSS as Okumura's: a flag byte, then 8 items -- a literal, or a 12-bit
// offset and a 4-bit length into a 4 KiB ring.
static uint32_t lzss_decode(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t cap) {
    uint8_t ring[4096];
    uint32_t r = 4096 - 18, o = 0, i = 0, flags = 0;
    for (uint32_t k = 0; k < sizeof ring; k++) ring[k] = 0x20;
    while (i < n && o < cap) {
        if (!((flags >>= 1) & 0x100)) {
            flags = in[i++] | 0xFF00;
            continue;
        }
        if (flags & 1) {
            if (i >= n) break;
            uint8_t ch = in[i++];
            out[o++] = ch;
            ring[r++ & 4095] = ch;
        } else {
            if (i + 1 >= n) break;
            uint32_t a = in[i], b = in[i + 1];
            i += 2;
            uint32_t pos = a | (b & 0xF0) << 4, len = (b & 0x0F) + 3;
            for (uint32_t k = 0; k < len && o < cap; k++) {
                uint8_t ch = ring[(pos + k) & 4095];
                out[o++] = ch;
                ring[r++ & 4095] = ch;
            }
        }
    }
    return o;
}

// A sprite with a transparent colour onto a 640-wide screen.
static void blit(uint8_t *dst, const uint8_t *src, int w, int h, uint8_t skip) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t c = src[y * w + x];
            if (c != skip) dst[y * 640 + x] = c;
        }
}

static void sort(uint16_t *a, int n) {
    for (int i = 1; i < n; i++) {
        uint16_t v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; }
        a[j + 1] = v;
    }
}

// The entry: at offset 0 of the blob (the linker script puts .text.entry first).
__attribute__((section(".text.entry"))) uint32_t bench(uint8_t *buf) {
    uint8_t *src = buf, *dst = buf + 0x40000, *screen = buf + 0x80000;
    uint32_t seed = 12345;
    // An LZSS stream of mixed literals and back-references.
    for (uint32_t i = 0; i < 0x20000; i++) {
        seed = seed * 1103515245u + 12345u;
        src[i] = (uint8_t)(seed >> 16);
    }
    uint32_t sum = 0;
    for (int round = 0; round < 4; round++) {
        uint32_t got = lzss_decode(src, 0x20000, dst, 0x40000);
        sum = sum * 31 + got + crc32(dst, got);
        for (int k = 0; k < 8; k++) blit(screen + k * 7, dst + k * 4096, 64, 64, (uint8_t)k);
        sum = sum * 31 + crc32(screen, 640 * 80);
        sort((uint16_t *)(dst + round * 512), 600);
        sum = sum * 31 + crc32(dst + round * 512, 1200);
    }
    return sum;
}
