// The ACPI embedded controller's byte protocol (ACPI 6.5, 12.3): RD_EC,
// WR_EC and QR_EC over a command/status port and a data port, each wait
// POLLED WITH A BOUND (docs/aml-design.md, stage 4b): a spin of `busy_ns`,
// then a poll after each `nap()` until `wait_ns` -- Linux's
// drivers/acpi/ec.c in polling mode (a 550 us spin, then sleeps, 500 ms
// in all), without its burst mode. The ASUS's EC took 934 us for a byte.
//
// FREE OF THE KERNEL -- ports and the clock come in through `struct
// ec_io` -- so tools/hostchecks/ec_hostcheck.py drives it against a
// simulated EC. uacpi_ec.c is the kernel's caller.
//
// NOT REENTRANT: one transaction at a time, which the caller's lock
// gives. A wait that runs out leaves the EC mid-transaction; the next
// one starts by draining a stale output byte (ec_flush()).
#ifndef EC_PROTO_H
#define EC_PROTO_H

#include <stdint.h>

// The status register (read from the command port).
#define EC_STS_OBF      0x01   // output buffer full: the data port has a byte for us
#define EC_STS_IBF      0x02   // input buffer full: the EC has not taken our last byte
#define EC_STS_CMD      0x08   // the last byte written was a command
#define EC_STS_BURST    0x10
#define EC_STS_SCI_EVT  0x20   // an event is waiting: answer it with QR_EC
#define EC_STS_SMI_EVT  0x40

#define EC_CMD_READ     0x80
#define EC_CMD_WRITE    0x81
#define EC_CMD_QUERY    0x84

struct ec_io {
    uint16_t cmd, data;                   // the command/status port, the data port
    uint8_t (*in)(uint16_t port);
    void (*out)(uint16_t port, uint8_t v);
    uint64_t (*now_ns)(void);
    uint64_t busy_ns;                     // spin this long first...
    uint64_t wait_ns;                     // ...and give up on ONE wait for IBF/OBF after this
    void (*nap)(void);                    // between polls past busy_ns; 0: keep spinning
    // What it measured -- the caller reports them.
    uint64_t transactions, timeouts, flushed, naps;
    uint64_t longest_wait_ns;
};

// 0, or -1 when a wait ran out (the transaction is abandoned).
int ec_read(struct ec_io *e, uint8_t addr, uint8_t *val);
int ec_write(struct ec_io *e, uint8_t addr, uint8_t val);
// The pending event's number in *q -- 0 when there was none.
int ec_query(struct ec_io *e, uint8_t *q);

#endif
