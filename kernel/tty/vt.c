// The virtual terminals' terminal half: VT2-4 as ttys, and the switch.
//
// A key asks (vt_request_switch(), from the keyboard IRQ); the `vt`
// kernel thread answers, because a switch redraws a whole screen and the
// first one to a VT allocates its terminal, grid and scrollback -- none
// of which an interrupt may do. Linux defers its switch to a workqueue
// (console_callback) for the same reason. The thread runs with
// interrupts off like every kernel thread, so a key's echo cannot land
// in the middle of a redraw.
#include "vt.h"
#include "tty.h"
#include "vga.h"
#include "keyboard.h"   // keyboard_compositor_owns()
#include "win_role.h"   // win_server_any()
#include "kthread.h"
#include "scheduler.h"
#include "irqflags.h"
#include "kfmt.h"
#include "klog.h"
#include "signal.h"     // SIGWINCH to init
#include "syscalls.h"   // tty_fd_ops, console_fd_ops -- /dev/ttyN
#include "errno.h"
#include <stddef.h>

// [n] is VT n's terminal; [1] stays NULL, VT1 being tty0 (tty_console()).
static struct tty *g_vt[VGA_VC_COUNT + 1];

static void vt_output(struct tty *t, const char *buf, unsigned len) {
    vga_vc_write((int)(uintptr_t)tty_driver_data(t), buf, len);
}

// DERIVED, as tty0's is (tty.c's console_winsize()): the font is a
// setting, so the grid changes under a running program.
static void vt_winsize(struct tty *t, uint16_t *rows, uint16_t *cols) {
    (void)t;
    vga_vc_size(rows, cols);
}

// One driver per VT so the NAME says which: /bin/tty prints it.
static const struct tty_driver g_vt_drv[VGA_VC_COUNT + 1] = {
    [2] = { .name = "vt2", .output = vt_output, .winsize = vt_winsize },
    [3] = { .name = "vt3", .output = vt_output, .winsize = vt_winsize },
    [4] = { .name = "vt4", .output = vt_output, .winsize = vt_winsize },
};
_Static_assert(VGA_VC_COUNT == 4, "a driver entry per VT above");

// --- what the keyboard asked for -------------------------------------

static volatile int g_want;     // a VT to switch to, 0 = none
static volatile int g_scroll;   // lines of history, negative = back
static int g_kick;              // the thread's wait channel

void vt_request_switch(int n) {
    if (n < 1 || n > VGA_VC_COUNT) return;
    g_want = n;
    scheduler_wake(&g_kick, 0);
}

void vt_request_scroll(int lines) {
    g_scroll += lines;
    scheduler_wake(&g_kick, 0);
}

struct tty *vt_input_tty(void) {
    int n = vga_vc_shown();
    return n > 1 && g_vt[n] ? g_vt[n] : tty_console();
}

int vt_text_shown(void) {
    return vga_vc_shown() != 1 || !keyboard_compositor_owns();
}

// --- the switch ------------------------------------------------------

static void vt_switch(int n) {
    if (n == vga_vc_shown()) return;
    // THE DESKTOP KEEPS VT1 UNTIL IT CAN HAND THE SCREEN OVER: switching
    // away from a compositor needs its scanout taken back and returned
    // (docs/tty-design.md, stage C).
    if (win_server_any()) {
        klog_printf("vt: the desktop holds the screen -- tty%d not shown\n", n);
        return;
    }
    if (n > 1 && !g_vt[n]) {
        struct tty *t = tty_create(&g_vt_drv[n], (void *)(uintptr_t)n);
        if (!t) {
            klog_printf("vt: no memory for tty%d\n", n);
            return;
        }
        g_vt[n] = t;
    }
    if (!vga_vc_prepare(n) || !vga_vc_show(n)) {
        klog_printf("vt: no memory for tty%d's screen\n", n);
        return;
    }
    klog_printf("vt: tty%d shown\n", n);
    // NOBODY ON IT: init starts a shell there (vt_open_desc() says where).
    // Linux's keyboard request reaches init as the same signal.
    if (n > 1 && !tty_session(g_vt[n]) && !tty_owner(g_vt[n])) {
        int init = scheduler_init_pid();
        if (init > 0) signal_send(init, SIGWINCH);
    }
}

int vt_open_desc(int n) {
    if (n < 1 || n > VGA_VC_COUNT) return -ENXIO;
    if (scheduler_current_tgid() != scheduler_init_pid()) return -EPERM;
    if (n == 1) return fd_desc_alloc(&console_fd_ops, -1);
    if (!g_vt[n]) return -ENXIO;
    int di = fd_desc_alloc(&tty_fd_ops, tty_index(g_vt[n]));
    return di >= 0 ? di : -ENFILE;
}

static void vt_thread(void *arg) {
    (void)arg;
    for (;;) {
        // Armed before the test, interrupts off throughout (a kernel
        // thread's), so a request between the two still wakes it.
        scheduler_wait_arm(&g_kick);
        uint64_t f = irq_save();
        int n = g_want, lines = g_scroll;
        g_want = 0;
        g_scroll = 0;
        irq_restore(f);
        if (!n && !lines) {
            scheduler_block_kernel(&g_kick, SCHED_WAIT_EVENT);
            continue;
        }
        scheduler_wait_disarm();
        if (n) vt_switch(n);
        if (lines < 0) vga_scroll_back(-lines);
        else if (lines > 0) vga_scroll_forward(lines);
    }
}

void vt_start(void) {
    if (kthread_start("vt", vt_thread, 0) <= 0)
        klog_printf(KLOG_WARN "vt: no thread -- Ctrl+Alt+F1..F4 will do nothing\n");
}
