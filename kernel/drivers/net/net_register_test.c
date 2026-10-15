// The network device registry: naming, renaming, and what removal has
// to undo.
//
// IT USES THE LIVE REGISTRY, which already holds whatever card this
// machine has, so every check works on a fixture device and asserts
// DELTAS. A fixture must be unregistered again even when a check fails
// early, or the next boot's real card finds one fewer slot.
//
// driver-none: the net registry's tests
#include "netdev.h"
#include "net.h"
#include "ktest.h"
#include "string.h"
#include "errno.h"
#include "irqflags.h"

static int g_tx_calls;
static int fake_tx(struct net_device *dev, const void *frame, uint32_t len) {
    (void)dev; (void)frame; (void)len;
    g_tx_calls++;
    return 0;
}

static struct net_device g_fixture;

static void fixture_init(uint8_t tail0, uint8_t tail1, uint8_t tail2) {
    k_memset(&g_fixture, 0, sizeof g_fixture);
    g_fixture.driver = "ktest-net";
    g_fixture.transmit = fake_tx;
    g_fixture.mac[0] = 0x02;                       // locally administered
    g_fixture.mac[3] = tail0;
    g_fixture.mac[4] = tail1;
    g_fixture.mac[5] = tail2;
}

// The last three bytes of a MAC are the vendor's own serial for that
// card; the first three name the vendor. So the name is an identity the
// card carries, not a truncation chosen for length.
KTEST("net-registry", "a name is made from the card's own serial") {
    fixture_init(0x71, 0x8E, 0xBF);
    KTEST_ASSERT(net_register(&g_fixture));
    KTEST_ASSERT(k_strcmp(g_fixture.name, "net-718ebf") == 0);
    net_unregister(&g_fixture);
}

// THE POINT OF THE WHOLE SCHEME. A card moved to another socket is the
// same card, so it must come back as the same interface -- which is
// where an address-derived name (systemd's enp3s0) was rejected.
KTEST("net-registry", "moving a card to another socket does not rename it") {
    fixture_init(0x11, 0x22, 0x33);
    k_strlcpy(g_fixture.location, "usb13", NET_LOC_MAX);
    KTEST_ASSERT(net_register(&g_fixture));
    char first[NET_NAME_MAX];
    k_strlcpy(first, g_fixture.name, sizeof first);
    net_unregister(&g_fixture);

    k_strlcpy(g_fixture.location, "usb5", NET_LOC_MAX);   // a different port
    KTEST_ASSERT(net_register(&g_fixture));
    KTEST_ASSERT(k_strcmp(g_fixture.name, first) == 0);
    net_unregister(&g_fixture);
}

// Nothing about probe order may reach a name. It used to: the name was
// the table index, so it was handed to the next card once the first
// went away -- and sockets bind by NAME.
KTEST("net-registry", "two cards get different names whatever the order") {
    static struct net_device other;
    fixture_init(0xAA, 0xBB, 0xCC);
    k_memset(&other, 0, sizeof other);
    other.driver = "ktest-net"; other.transmit = fake_tx;
    other.mac[0] = 0x02; other.mac[3] = 0xDD; other.mac[4] = 0xEE; other.mac[5] = 0xFF;

    KTEST_ASSERT(net_register(&g_fixture));
    KTEST_ASSERT(net_register(&other));
    KTEST_ASSERT(k_strcmp(g_fixture.name, "net-aabbcc") == 0);
    KTEST_ASSERT(k_strcmp(other.name, "net-ddeeff") == 0);
    net_unregister(&g_fixture);
    KTEST_ASSERT(k_strcmp(other.name, "net-ddeeff") == 0);  // not renumbered
    net_unregister(&other);
}

// This is the bug that started it: an adapter unplugged and plugged
// back in registered a second time, and one card was listed twice.
KTEST("net-registry", "registering the same device twice is refused") {
    fixture_init(0x09, 0x09, 0x09);
    int before = net_device_count();
    KTEST_ASSERT(net_register(&g_fixture));
    KTEST_ASSERT_EQ(net_device_count(), before + 1);
    KTEST_ASSERT(!net_register(&g_fixture));
    KTEST_ASSERT_EQ(net_device_count(), before + 1);
    net_unregister(&g_fixture);
    KTEST_ASSERT_EQ(net_device_count(), before);
}

KTEST("net-registry", "a removed device is not found by name and is not listed") {
    fixture_init(0x09, 0x09, 0x08);
    KTEST_ASSERT(net_register(&g_fixture));
    KTEST_ASSERT(net_device_by_name("net-090908") == &g_fixture);
    net_unregister(&g_fixture);
    KTEST_ASSERT(net_device_by_name("net-090908") == 0);
}

// A card that comes back must not come back configured: the address was
// a lease on a network it may no longer be attached to.
KTEST("net-registry", "removal clears the address and the link state") {
    fixture_init(0x09, 0x09, 0x07);
    KTEST_ASSERT(net_register(&g_fixture));
    g_fixture.ip = 0xC0A80001; g_fixture.netmask = 0xFFFFFF00;
    g_fixture.gateway = 0xC0A800FE;
    g_fixture.link_known = 1; g_fixture.link_up = 1; g_fixture.link_bps = 1000000000u;

    net_unregister(&g_fixture);
    KTEST_ASSERT_EQ(g_fixture.ip, 0u);
    KTEST_ASSERT_EQ(g_fixture.netmask, 0u);
    KTEST_ASSERT_EQ(g_fixture.gateway, 0u);
    KTEST_ASSERT_EQ((uint32_t)g_fixture.link_known, 0u);
    KTEST_ASSERT_EQ((uint32_t)g_fixture.link_up, 0u);
}

KTEST("net-registry", "removing an unknown device does nothing") {
    fixture_init(0x09, 0x09, 0x06);
    int before = net_device_count();
    net_unregister(&g_fixture);
    KTEST_ASSERT_EQ(net_device_count(), before);
}

// --- renaming, which is what /bin/netd does over SYS_NET_RENAME ------

KTEST("net-registry", "a device can be renamed and answers to the new name") {
    fixture_init(0x09, 0x09, 0x05);
    KTEST_ASSERT(net_register(&g_fixture));
    KTEST_ASSERT(net_rename(&g_fixture, "lan"));
    KTEST_ASSERT(k_strcmp(g_fixture.name, "lan") == 0);
    KTEST_ASSERT(net_device_by_name("lan") == &g_fixture);
    KTEST_ASSERT(net_device_by_name("net-090905") == 0);
    net_unregister(&g_fixture);
}

// A name reaches /var/dhcp-<name>.lease and a socket's device binding,
// so what would break either is refused here rather than wherever it
// was first noticed.
KTEST("net-registry", "a name a lease file could not survive is refused") {
    fixture_init(0x09, 0x09, 0x04);
    KTEST_ASSERT(net_register(&g_fixture));
    KTEST_ASSERT(!net_rename(&g_fixture, ""));
    KTEST_ASSERT(!net_rename(&g_fixture, "two words"));
    KTEST_ASSERT(!net_rename(&g_fixture, "a=b"));
    KTEST_ASSERT(!net_rename(&g_fixture, "a/b"));
    KTEST_ASSERT(!net_rename(&g_fixture, "0123456789abcdef"));  // NET_NAME_MAX
    KTEST_ASSERT(k_strcmp(g_fixture.name, "net-090904") == 0);  // none took
    net_unregister(&g_fixture);
}

KTEST("net-registry", "a name another device already holds is refused") {
    static struct net_device other;
    fixture_init(0x09, 0x09, 0x03);
    k_memset(&other, 0, sizeof other);
    other.driver = "ktest-net"; other.transmit = fake_tx;
    other.mac[0] = 0x02; other.mac[5] = 0x02;

    KTEST_ASSERT(net_register(&g_fixture));
    KTEST_ASSERT(net_register(&other));
    KTEST_ASSERT(!net_rename(&other, g_fixture.name));
    KTEST_ASSERT(k_strcmp(other.name, "net-000002") == 0);
    // Renaming a device to what it is already called is a no-op, not a
    // clash with itself.
    KTEST_ASSERT(net_rename(&other, "net-000002"));
    net_unregister(&other);
    net_unregister(&g_fixture);
}

// ADMINISTRATIVELY DOWN (NET_IFC_DOWN): nothing reaches the driver and
// nothing received is delivered or counted; up again, both work. The
// frame is an unused EtherType, so the stack discards it when it drains.
KTEST("net-registry", "a card set down neither sends nor receives") {
    static uint8_t frame[60];
    fixture_init(0x09, 0x09, 0x04);
    frame[12] = 0x88; frame[13] = 0xb5;   // IEEE "local experimental"
    KTEST_ASSERT(net_register(&g_fixture));

    g_fixture.admin_down = 1;
    int calls = g_tx_calls;
    uint64_t rx = g_fixture.rx_packets;
    KTEST_ASSERT_EQ(net_tx(&g_fixture, frame, sizeof frame), -ENETDOWN);
    KTEST_ASSERT_EQ(g_tx_calls, calls);
    net_rx(&g_fixture, frame, sizeof frame);
    KTEST_ASSERT_EQ(g_fixture.rx_packets, rx);

    g_fixture.admin_down = 0;
    KTEST_ASSERT_EQ(net_tx(&g_fixture, frame, sizeof frame), 0);
    KTEST_ASSERT_EQ(g_tx_calls, calls + 1);
    net_rx(&g_fixture, frame, sizeof frame);
    KTEST_ASSERT_EQ(g_fixture.rx_packets, rx + 1);
    net_poll();   // drain it while the device is still registered
    net_unregister(&g_fixture);
}

// NET-06: a slot reassembling a datagram keeps its device for the timeout
// report; a device that goes away must take those slots with it.
KTEST("net-registry", "removing a card drops the datagrams it was reassembling") {
    fixture_init(0x06, 0x06, 0x06);
    g_fixture.ip = 0x0A630002u;                    // 10.99.0.2
    KTEST_ASSERT(net_register(&g_fixture));
    uint8_t pkt[28];
    k_memset(pkt, 0, sizeof pkt);
    pkt[0] = 0x45; pkt[3] = 28;                    // 20 + 8
    pkt[4] = 0x66; pkt[5] = 0x06;                  // id
    pkt[6] = 0x20;                                 // MF, offset 0
    pkt[8] = 64; pkt[9] = 17;
    pkt[12] = 10; pkt[13] = 99; pkt[15] = 1;       // from 10.99.0.1
    pkt[16] = 10; pkt[17] = 99; pkt[19] = 2;
    int before = ipv4_frag_pending();
    ipv4_frag_input(&g_fixture, pkt, 20, sizeof pkt, 1);
    int held = ipv4_frag_pending();
    net_unregister(&g_fixture);
    int after = ipv4_frag_pending();
    KTEST_ASSERT_EQ(held, before + 1);
    KTEST_ASSERT_EQ(after, before);
}

// NET-10: a socket bound to a card that has gone must not send through
// whichever card the route picks instead.
KTEST("net-registry", "a send on a socket bound to a removed card fails") {
    fixture_init(0x10, 0x10, 0x10);
    g_fixture.ip = 0x0A640002u;                    // 10.100.0.2/24
    g_fixture.netmask = 0xFFFFFF00u;
    KTEST_ASSERT(net_register(&g_fixture));
    char name[NET_NAME_MAX];
    k_strlcpy(name, g_fixture.name, sizeof name);
    int s = net_sock_open(NET_AF_INET, NET_SOCK_DGRAM, IP_PROTO_UDP);
    int bound = net_sock_bind(s, 0, 7811, name);
    net_unregister(&g_fixture);
    g_tx_calls = 0;
    int rc = net_sock_sendto(s, 0x0A640001u, 9, "x", 1);
    net_sock_close(s);
    KTEST_ASSERT_EQ(bound, 7811);
    KTEST_ASSERT_EQ(rc, -ENODEV);
}

// NET-07: a ring that never drains, with interrupts off. On a clock that
// only moves on the tick (boot with clocksource=pit) the time limit alone
// never expired and this never returned.
static int full_tx(struct net_device *dev, const void *frame, uint32_t len) {
    (void)dev; (void)frame; (void)len;
    g_tx_calls++;
    return -ENOSPC;
}

KTEST("net-registry", "a transmit ring that never drains is given up on, interrupts off") {
    fixture_init(0x07, 0x07, 0x07);
    g_fixture.transmit = full_tx;
    KTEST_ASSERT(net_register(&g_fixture));
    uint8_t frame[60];
    k_memset(frame, 0xFF, sizeof frame);
    g_tx_calls = 0;
    uint64_t f = irq_save();
    int rc = net_tx(&g_fixture, frame, sizeof frame);
    irq_restore(f);
    int dropped = (int)g_fixture.tx_dropped;
    net_unregister(&g_fixture);
    KTEST_ASSERT_EQ(rc, -ENOSPC);
    KTEST_ASSERT_EQ(dropped, 1);
    KTEST_ASSERT(g_tx_calls > 1);                  // it did wait before giving up
}
