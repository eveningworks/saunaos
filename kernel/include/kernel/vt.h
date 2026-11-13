#ifndef KERNEL_VT_H
#define KERNEL_VT_H

#include <stdint.h>

// THE VIRTUAL TERMINALS: four screens behind Ctrl+Alt+F1..F4, one shown
// at a time (docs/tty-design.md, "The virtual terminals").
//
// VT1 is the kernel's console and tty0's: everything the kernel prints
// lands there, whichever VT is on screen. VT2-4 are terminals of their
// own ("vt2".."vt4"), created on the first switch to each.
//
// TWO HALVES, as on Linux (vt.c over fbcon): kernel/drivers/vga.c draws
// the screens -- each a grid of cells, so a hidden one can be redrawn --
// and kernel/tty/vt.c is the terminals on them and the switching.

#define VGA_VC_COUNT 4

struct tty;

// --- the screens (kernel/drivers/vga.c) ------------------------------

int  vga_vc_shown(void);   // 1..VGA_VC_COUNT
// A grid and a scrollback for VT n. 0 when there is no memory for the
// grid (the VT cannot be shown); a missing scrollback is only logged.
int  vga_vc_prepare(int n);
// Puts VT n on screen and presents it. 0 when it was never prepared.
int  vga_vc_show(int n);
// A terminal's output: bytes as its discipline sent them ('\n' a bare
// line feed). VT1's is tty0's vga_putc_console().
void vga_vc_write(int n, const char *s, unsigned len);
// Blank, cursor home, colours and modes back to the start.
void vga_vc_clear(int n);
void vga_vc_size(uint16_t *rows, uint16_t *cols);
// From scheduler_idle(): presents the tail of the last output burst.
void vga_idle(void);

// --- the terminals (kernel/tty/vt.c) ---------------------------------

// INTERRUPT-SAFE: they record the request and wake the `vt` kernel
// thread, which does the work -- a switch redraws the whole screen and
// may allocate, and the keyboard IRQ may do neither.
void vt_request_switch(int n);
void vt_request_scroll(int lines);   // the shown VT's history; negative = back

// Where a key goes: the shown VT's terminal, tty0 for VT1.
struct tty *vt_input_tty(void);
// The shown VT is TEXT -- not VT1 while a compositor holds it.
int vt_text_shown(void);

// /dev/ttyN: a new description of VT n's terminal, or -errno. INIT'S
// ALONE (-EPERM otherwise) -- a VT's keys are whoever sits at it, and
// only the service manager starts a session there -- and only once the
// VT has been shown (-ENXIO before), which is how init learns where a
// shell is wanted.
int vt_open_desc(int n);

// Starts the `vt` kernel thread. After init, which must be pid 1.
void vt_start(void);

#endif
