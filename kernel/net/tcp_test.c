// KTESTs for the TCP stack's passive open, driven through tcp_input() with
// hand-built segments -- no peer, no wire.
#include "net.h"
#include "netdev.h"
#include "ktest.h"
#include "string.h"
#include "scheduler.h"   // the preemption guard: blocks are a shared pool

#define PORT 47123

// Live blocks, not free ones: the table grows on demand (kslots), so
// "allocate until it fails" no longer measures anything.
static int live_blocks(void) { return tcp_blocks_in_use(); }

static int seg(uint8_t *out, uint32_t src, uint32_t dst, uint16_t sport,
               uint32_t seq, uint8_t flags) {
    k_memset(out, 0, 20);
    out[0] = (uint8_t)(sport >> 8); out[1] = (uint8_t)sport;
    out[2] = PORT >> 8;             out[3] = PORT & 0xFF;
    for (int i = 0; i < 4; i++) out[4 + i] = (uint8_t)(seq >> (24 - i * 8));
    out[12] = 5 << 4;
    out[13] = flags;
    out[14] = 0x20;                 // window 8192
    uint8_t pseudo[12];
    for (int i = 0; i < 4; i++) {
        pseudo[i] = (uint8_t)(src >> (24 - i * 8));
        pseudo[4 + i] = (uint8_t)(dst >> (24 - i * 8));
    }
    pseudo[8] = 0; pseudo[9] = 6; pseudo[10] = 0; pseudo[11] = 20;
    uint16_t cs = net_htons(net_checksum_two(pseudo, 12, out, 20));
    k_memcpy(out + 16, &cs, 2);
    return 20;
}

KTEST("tcp", "a half-open connection that is reset gives its block back") {
    struct net_device *dev = net_default_device();
    if (!dev || !dev->ip || !dev->netmask) KTEST_SKIP("no configured network device");
    // An on-link address nobody has, so the SYN+ACK stops at ARP.
    uint32_t peer = (dev->ip & dev->netmask) | 0x63;
    if (peer == dev->ip) peer ^= 1;

    scheduler_preempt_disable();
    int lis = tcp_open(PORT);
    int ok = lis >= 0 && tcp_listen(lis) == 0;
    int before = ok ? live_blocks() : -1;

    uint8_t pkt[20];
    tcp_input(dev, peer, dev->ip, pkt, (uint32_t)seg(pkt, peer, dev->ip, 40000, 1000, 0x02));
    int during = live_blocks();
    // The reset must sit where the next byte was expected: 1000 + the SYN.
    tcp_input(dev, peer, dev->ip, pkt, (uint32_t)seg(pkt, peer, dev->ip, 40000, 1001, 0x04));
    int after = live_blocks();
    if (lis >= 0) tcp_release(lis);
    scheduler_preempt_enable();

    KTEST_ASSERT(ok);
    KTEST_ASSERT_EQ(during, before + 1);   // the SYN took a block
    KTEST_ASSERT_EQ(after, before);        // and the reset gave it back
}

// seg(), with an acknowledgment number.
static int seg_ack(uint8_t *out, uint32_t src, uint32_t dst, uint16_t sport,
                   uint32_t seq, uint32_t ack, uint8_t flags) {
    seg(out, src, dst, sport, seq, flags);
    for (int i = 0; i < 4; i++) out[8 + i] = (uint8_t)(ack >> (24 - i * 8));
    out[16] = out[17] = 0;
    uint8_t pseudo[12];
    for (int i = 0; i < 4; i++) {
        pseudo[i] = (uint8_t)(src >> (24 - i * 8));
        pseudo[4 + i] = (uint8_t)(dst >> (24 - i * 8));
    }
    pseudo[8] = 0; pseudo[9] = 6; pseudo[10] = 0; pseudo[11] = 20;
    uint16_t cs = net_htons(net_checksum_two(pseudo, 12, out, 20));
    k_memcpy(out + 16, &cs, 2);
    return 20;
}

KTEST("tcp", "an ACK for something our SYN never sent is answered with a reset") {
    struct net_device *dev = net_default_device();
    if (!dev || !dev->ip || !dev->netmask) KTEST_SKIP("no configured network device");
    // An on-link address nobody has, so our segments stop at ARP.
    uint32_t peer = (dev->ip & dev->netmask) | 0x64;
    if (peer == dev->ip) peer ^= 1;

    scheduler_preempt_disable();
    int c = tcp_open(PORT);
    int ok = c >= 0 && tcp_connect(c, peer, 40001) == 0;
    uint32_t r0 = tcp_out_rsts();
    uint8_t pkt[20];
    // A bare ACK for an old connection's bytes: what a peer's TIME_WAIT
    // half answers a reused port pair's SYN with. 12345 is our ISN + 1
    // with probability 2^-32.
    tcp_input(dev, peer, dev->ip, pkt, (uint32_t)seg_ack(pkt, peer, dev->ip, 40001, 5000, 12345, 0x10));
    uint32_t r1 = tcp_out_rsts();
    int st1 = tcp_state(c);
    // A SYN with no ACK acknowledges nothing, so it is not answered so.
    tcp_input(dev, peer, dev->ip, pkt, (uint32_t)seg_ack(pkt, peer, dev->ip, 40001, 6000, 0, 0x02));
    uint32_t r2 = tcp_out_rsts();
    if (c >= 0) tcp_release(c);
    scheduler_preempt_enable();

    KTEST_ASSERT(ok);
    KTEST_ASSERT_EQ(r1, r0 + 1);                    // the reset
    KTEST_ASSERT_EQ(st1, TCP_STATE_SYN_SENT);       // and our SYN still waits
    KTEST_ASSERT_EQ(r2, r1);
}
