// io16.c -- the PC's I/O ports, as a Windows 3.1 program in enhanced mode saw
// them. The VMM trapped the ports its virtual devices owned and let the rest
// reach the hardware; a game read the CMOS clock, waited for the VGA's
// retrace, or timed itself on the PIT. w16 answers those few as an AT
// would, from the host's clock: there is no hardware behind them.
//
// Any other port reads all ones and swallows writes -- an empty bus, what
// probing for a card that is not there finds -- so a game looking for a
// Sound Blaster at 220h falls back to Windows' own sound. The first touch
// of each such port is named in the trace (--trace).
#include "w16_internal.h"

#include <stdio.h>
#include <time.h>

static uint8_t g_cmos_index;
static uint8_t g_pit_mode[3];          // the control word last written for each counter (43h)
static uint8_t g_pit_latched[3], g_pit_lo_next[3];
static uint16_t g_pit_latch[3];
static uint8_t g_seen[65536 / 8];      // the unemulated ports already named in the trace

static uint8_t bcd(int v) { return (uint8_t)((v / 10) << 4 | (v % 10)); }

// The MC146818's registers: the time in BCD (status B: 24-hour, BCD), no update in progress, a
// valid battery; and the AT's configuration bytes -- 640 KB base memory, 15 MB above it, as an 16 MB
// machine said (a program reads them to size itself, never to use them).
static uint8_t cmos(uint8_t reg) {
    time_t t = time(0);
    struct tm tm = *localtime(&t);
    switch (reg) {
    case 0x00: return bcd(tm.tm_sec);
    case 0x02: return bcd(tm.tm_min);
    case 0x04: return bcd(tm.tm_hour);
    case 0x06: return bcd(tm.tm_wday + 1);
    case 0x07: return bcd(tm.tm_mday);
    case 0x08: return bcd(tm.tm_mon + 1);
    case 0x09: return bcd(tm.tm_year % 100);
    case 0x32: return bcd(19 + tm.tm_year / 100);
    case 0x0A: return 0x26;                       // 32.768 kHz, 1024 Hz; UIP clear
    case 0x0B: return 0x02;                       // 24-hour, BCD, no interrupts
    case 0x0D: return 0x80;                       // the battery is good
    case 0x10: return 0x40;                       // one 1.44 MB floppy
    case 0x14: return 0x0F;                       // a floppy, an FPU, VGA, keyboard
    case 0x15: return 0x80;                       // base memory 0x0280 = 640 KB
    case 0x16: return 0x02;
    case 0x17: case 0x30: return 0x00;            // extended memory 0x3C00 = 15 MB
    case 0x18: case 0x31: return 0x3C;
    default: return 0;
    }
}

// The PIT's counters run at 1.193182 MHz from 65536 (mode 2 or 3, as the BIOS left counter 0),
// read through a latch or straight, low byte then high.
static uint16_t pit_count(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    uint64_t ticks = ns * 1193182ull / 1000000000ull;
    return (uint16_t)(0x10000 - (ticks & 0xFFFF));
}

static uint8_t pit_read(unsigned ch) {
    uint16_t v = g_pit_latched[ch] ? g_pit_latch[ch] : pit_count();
    uint8_t access = (uint8_t)(g_pit_mode[ch] >> 4 & 3);   // 1 low, 2 high, 3 low then high
    if (access == 1) { g_pit_latched[ch] = 0; return (uint8_t)v; }
    if (access == 2) { g_pit_latched[ch] = 0; return (uint8_t)(v >> 8); }
    if (!g_pit_lo_next[ch]) { g_pit_lo_next[ch] = 1; return (uint8_t)v; }
    g_pit_lo_next[ch] = 0;
    g_pit_latched[ch] = 0;
    return (uint8_t)(v >> 8);
}

static uint8_t in8(uint16_t port) {
    switch (port) {
    case 0x71: return cmos(g_cmos_index & 0x7F);
    case 0x40: case 0x41: case 0x42: return pit_read(port - 0x40u);
    case 0x61: return 0x20;                       // the refresh toggle's bit; speaker off
    case 0x3BA: case 0x3DA: {                     // VGA input status 1: retrace about 1 time in 16
        static unsigned n;
        n++;
        return (uint8_t)((n & 0x0F) == 0 ? 0x09 : (n & 1));
    }
    default: return 0xFF;
    }
}

static void out8(uint16_t port, uint8_t v) {
    switch (port) {
    case 0x70: g_cmos_index = v; break;
    case 0x43: {
        unsigned ch = v >> 6;
        if (ch > 2) break;                        // the 8254's read-back: not kept
        if ((v & 0x30) == 0) {                    // a counter latch
            g_pit_latch[ch] = pit_count();
            g_pit_latched[ch] = 1;
            g_pit_lo_next[ch] = 0;
        } else {
            g_pit_mode[ch] = v;
            g_pit_lo_next[ch] = 0;
        }
        break;
    }
    default: break;                               // the PIT's reload and the rest: nothing to drive
    }
}

static int emulated(uint16_t port) {
    return port == 0x70 || port == 0x71 || (port >= 0x40 && port <= 0x43) || port == 0x61 || port == 0x3BA ||
           port == 0x3DA;
}

int w16_port_io(struct cpu16 *c, uint16_t port, unsigned size, int out, uint32_t *val) {
    (void)c;
    if (g_w16_trace && !emulated(port) && !(g_seen[port >> 3] & (1u << (port & 7)))) {
        g_seen[port >> 3] |= (uint8_t)(1u << (port & 7));
        fprintf(g_w16_trace, "  port %04xh: no device behind it (reads all ones, writes dropped)\n", port);
        fflush(g_w16_trace);
    }
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) {         // a word or dword is its bytes at port, port+1...
        if (out) out8((uint16_t)(port + i), (uint8_t)(*val >> (8 * i)));
        else v |= (uint32_t)in8((uint16_t)(port + i)) << (8 * i);
    }
    if (!out) *val = v;
    return 0;
}
