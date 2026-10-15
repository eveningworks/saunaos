// The USB class-driver table, and binding by interface. See
// kernel/include/kernel/usb_driver.h for the contract.
//
// driver-none: the bus's binding core; each class driver declares itself
#include "usb_driver.h"
#include "devevent.h"
#include "klog.h"
#include "string.h"
#include "ktest.h"

extern const struct usb_driver __usb_drivers_start[];
extern const struct usb_driver __usb_drivers_end[];

int usb_driver_count(void) {
    long n = __usb_drivers_end - __usb_drivers_start;
    return n < 0 ? 0 : (int)n;
}

const struct usb_driver *usb_driver_at(int i) {
    return i >= 0 && i < usb_driver_count() ? &__usb_drivers_start[i] : 0;
}

static int field_ok(uint16_t want, uint16_t have) { return want == USB_ANY || want == have; }

static int matches(const struct usb_match *m, uint16_t vendor, uint16_t product,
                   uint8_t cls, uint8_t sub, uint8_t proto) {
    return field_ok(m->vendor, vendor) && field_ok(m->product, product) &&
           field_ok(m->if_class, cls) && field_ok(m->if_subclass, sub) &&
           field_ok(m->if_protocol, proto);
}

static const struct usb_driver *driver_for(uint16_t vendor, uint16_t product,
                                           uint8_t cls, uint8_t sub, uint8_t proto) {
    for (int i = 0; i < usb_driver_count(); i++) {
        const struct usb_driver *drv = usb_driver_at(i);
        for (int k = 0; k < drv->nmatches; k++)
            if (matches(&drv->matches[k], vendor, product, cls, sub, proto)) return drv;
    }
    return 0;
}

const struct usb_driver *usb_driver_for(uint16_t vendor, uint16_t product,
                                        const struct usb_interface_info *ifc) {
    return ifc ? driver_for(vendor, product, ifc->if_class, ifc->if_subclass,
                            ifc->if_protocol) : 0;
}

// Every alternate-0 interface of the configuration, against the table.
// A VENDOR-SPECIFIC interface matches only an entry that names the
// device's ids -- class 0xFF describes nothing -- and CDC only the
// subclass a driver names, so a device offering RNDIS first is still
// found driveable in the configuration that has ECM.
int usb_config_driveable(const uint8_t *cfg, uint32_t total,
                         uint16_t vendor, uint16_t product) {
    for (uint32_t o = 0; o + 2 <= total; ) {
        uint32_t blen = cfg[o];
        if (blen < 2 || o + blen > total) return 0;
        if (cfg[o + 1] == 4 && blen >= 9 && cfg[o + 3] == 0 &&
            driver_for(vendor, product, cfg[o + 5], cfg[o + 6], cfg[o + 7]))
            return 1;
        o += blen;
    }
    return 0;
}

static int iface_index(const struct usb_device_info *d, uint8_t ifnum) {
    for (int i = 0; i < d->if_count; i++) if (d->ifs[i].ifnum == ifnum) return i;
    return -1;
}

int usb_interface_claim(struct usb_device_info *d, uint8_t ifnum, const char *driver) {
    int i = iface_index(d, ifnum);
    if (i < 0) return 0;
    if (d->ifs[i].driver && k_strcmp(d->ifs[i].driver, driver) != 0) return 0;
    d->ifs[i].driver = driver;
    return 1;
}

// The device-level summary QUERY_USB and the devevents carry: bound when
// any interface is, named by the first that is.
static void summarise(struct usb_device_info *d) {
    d->bound = 0;
    d->driver = 0;
    for (int i = 0; i < d->if_count; i++) {
        if (!d->ifs[i].driver) continue;
        if (!d->bound) d->driver = d->ifs[i].driver;
        d->bound = 1;
    }
}

void usb_bind_drivers(struct usb_device_info *d, const uint8_t *cfg, uint32_t total) {
    for (int i = 0; i < d->if_count; i++) {
        struct usb_interface_info *ifc = &d->ifs[i];
        if (ifc->driver) continue;          // a sibling some driver claimed
        const struct usb_driver *drv = usb_driver_for(d->vendor_id, d->product_id, ifc);
        if (!drv) continue;
        if (!drv->bind(d, i, cfg, total)) continue;
        ifc->bound_by = drv;
        if (!ifc->driver) ifc->driver = drv->name;
    }
    summarise(d);
    char id[24];
    usb_device_id(d, id, sizeof id);
    if (d->bound) devevent_add(QUERY_DEVEV_BOUND, id, d->driver ? d->driver : "",
                               "Driven by %s", d->driver ? d->driver : "a driver");
    else devevent_add(QUERY_DEVEV_NO_DRIVER, id, "", "No driver in this build takes it");
}

// Each interface a driver's bind took goes back through that driver's
// unbind, once; the siblings it claimed go with it.
void usb_unbind_drivers(struct usb_device_info *d) {
    for (int i = 0; i < d->if_count; i++) {
        const struct usb_driver *drv = d->ifs[i].bound_by;
        if (drv) drv->unbind(d, i);
    }
    for (int i = 0; i < d->if_count; i++) {
        d->ifs[i].driver = 0;
        d->ifs[i].bound_by = 0;
    }
    summarise(d);
}

// --- KTESTs --------------------------------------------------------------

KTEST("usb-driver", "every driver in the table can bind and let go") {
    KTEST_ASSERT(usb_driver_count() >= 5);   // hub, hid, audio, cdc-ecm, rtl
    for (int i = 0; i < usb_driver_count(); i++) {
        const struct usb_driver *drv = usb_driver_at(i);
        KTEST_ASSERT(drv->name && drv->name[0]);
        KTEST_ASSERT(drv->bind != 0);
        KTEST_ASSERT(drv->unbind != 0);
        KTEST_ASSERT(drv->nmatches > 0);
    }
}

// The first-match rule hides a second driver for the same interface, so
// look for one: every match entry of every driver, offered to the table,
// must come back to its own driver.
KTEST("usb-driver", "no two drivers match the same interface") {
    for (int i = 0; i < usb_driver_count(); i++) {
        const struct usb_driver *drv = usb_driver_at(i);
        for (int k = 0; k < drv->nmatches; k++) {
            const struct usb_match *m = &drv->matches[k];
            uint8_t cls = (uint8_t)(m->if_class == USB_ANY ? 0x42 : m->if_class);
            uint8_t sub = (uint8_t)(m->if_subclass == USB_ANY ? 0x42 : m->if_subclass);
            uint8_t pro = (uint8_t)(m->if_protocol == USB_ANY ? 0x42 : m->if_protocol);
            uint16_t v = m->vendor == USB_ANY ? 0x4242 : m->vendor;
            uint16_t p = m->product == USB_ANY ? 0x4242 : m->product;
            KTEST_ASSERT(driver_for(v, p, cls, sub, pro) == drv);
        }
    }
}

KTEST("usb-driver", "interfaces reach the driver their class names") {
    struct usb_interface_info hid = { .if_class = 3, .if_subclass = 1, .if_protocol = 1 };
    struct usb_interface_info hub = { .if_class = 9 };
    struct usb_interface_info ecm = { .if_class = 2, .if_subclass = 6 };
    struct usb_interface_info rndis = { .if_class = 2, .if_subclass = 2, .if_protocol = 0xFF };
    struct usb_interface_info ac = { .if_class = 1, .if_subclass = 1 };
    struct usb_interface_info as = { .if_class = 1, .if_subclass = 2 };
    struct usb_interface_info vendor = { .if_class = 0xFF };
    const struct usb_driver *d;
    KTEST_ASSERT((d = usb_driver_for(1, 1, &hid)) && !k_strcmp(d->name, "usb-hid"));
    KTEST_ASSERT((d = usb_driver_for(1, 1, &hub)) && !k_strcmp(d->name, "hub"));
    KTEST_ASSERT((d = usb_driver_for(1, 1, &ecm)) && !k_strcmp(d->name, "cdc-ecm"));
    KTEST_ASSERT((d = usb_driver_for(1, 1, &ac)) && !k_strcmp(d->name, "usb-audio"));
    KTEST_ASSERT(!usb_driver_for(1, 1, &rndis));      // no RNDIS driver here
    KTEST_ASSERT(!usb_driver_for(1, 1, &as));         // claimed by audio, never offered
    KTEST_ASSERT(!usb_driver_for(0x041E, 0x3256, &vendor));   // a G6's vendor interface
    KTEST_ASSERT(usb_driver_for(0x2357, 0x0601, &vendor) != 0);   // a UE300's
    KTEST_ASSERT(usb_driver_for(0x0BDA, 0x8156, &vendor) != 0);
}

// QEMU's usb-net, as its descriptors arrive: RNDIS in configuration 0,
// CDC-ECM in configuration 1 -- the order that used to strand it.
KTEST("usb-driver", "a configuration is driveable by what its interfaces match") {
    static const uint8_t rndis_cfg[] = {
        0x09, 0x02, 0x43, 0x00, 0x02, 0x02, 0x09, 0xc0, 0x32,
        0x09, 0x04, 0x00, 0x00, 0x01, 0x02, 0x02, 0xff, 0x06,
        0x05, 0x24, 0x00, 0x10, 0x01, 0x05, 0x24, 0x01, 0x00, 0x01,
        0x04, 0x24, 0x02, 0x00, 0x05, 0x24, 0x06, 0x00, 0x01,
        0x07, 0x05, 0x81, 0x03, 0x10, 0x00, 0x20,
        0x09, 0x04, 0x01, 0x00, 0x02, 0x0a, 0x00, 0x00, 0x04,
        0x07, 0x05, 0x82, 0x02, 0x40, 0x00, 0x00,
        0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00,
    };
    static const uint8_t ecm_cfg[] = {
        0x09, 0x02, 0x1b, 0x00, 0x02, 0x01, 0x00, 0xc0, 0x32,
        0x09, 0x04, 0x00, 0x00, 0x01, 0x02, 0x06, 0x00, 0x00,
        0x09, 0x04, 0x01, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00,
    };
    static const uint8_t vendor_cfg[] = {
        0x09, 0x02, 0x12, 0x00, 0x01, 0x01, 0x00, 0xa0, 0x32,
        0x09, 0x04, 0x00, 0x00, 0x03, 0xff, 0xff, 0x00, 0x00,
    };
    KTEST_ASSERT(!usb_config_driveable(rndis_cfg, sizeof rndis_cfg, 0x0525, 0xa4a2));
    KTEST_ASSERT(usb_config_driveable(ecm_cfg, sizeof ecm_cfg, 0x0525, 0xa4a2));
    KTEST_ASSERT(usb_config_driveable(vendor_cfg, sizeof vendor_cfg, 0x2357, 0x0601));
    KTEST_ASSERT(!usb_config_driveable(vendor_cfg, sizeof vendor_cfg, 0x2357, 0x0000));
}
