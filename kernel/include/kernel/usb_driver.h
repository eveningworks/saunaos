#ifndef USB_DRIVER_H
#define USB_DRIVER_H

#include <stdint.h>
#include "usb.h"

// USB CLASS DRIVERS, BOUND PER INTERFACE: a driver declares what it
// drives and a bind/unbind pair, and the core offers it each interface
// its table matches -- Linux's `usb_driver` + id table, PCI_DRIVER's
// shape here. The core names no driver; a new one is a USB_DRIVER()
// line in its own file.
//
// usb_bind_drivers() walks a device's interfaces in order and hands each
// one nobody holds yet to the FIRST driver in link order whose table
// matches it. A driver that needs a SIBLING interface -- CDC-ECM's data
// interface, audio's streaming one -- takes it with usb_interface_claim()
// from inside bind, so it is never offered to anyone else. Two drivers
// matching one interface is a mistake this cannot see; the KTEST in
// usb_driver.c looks for it.

#define USB_ANY 0xFFFF

struct usb_match {
    uint16_t vendor, product;              // or USB_ANY
    uint16_t if_class, if_subclass, if_protocol;   // or USB_ANY
};

#define USB_MATCH_IF(c, s, p)       { USB_ANY, USB_ANY, (c), (s), (p) }
#define USB_MATCH_ID_IF(v, d, c)    { (v), (d), (c), USB_ANY, USB_ANY }

struct usb_driver {
    const char *name;                 // the DRIVER_DECLARE name
    const struct usb_match *matches;
    int nmatches;
    // 1: the driver took interface `iface` (an index into d->ifs). 0: it
    // looked and declined, and the interface stays free for a driver
    // later in the table. `cfg` is the raw configuration -- class-specific
    // descriptors the interface walk does not keep.
    int  (*bind)(struct usb_device_info *d, int iface, const uint8_t *cfg,
                 uint32_t total);
    // Undoes bind for that interface, on an unplug or a claim. MUST stop
    // the transfers it posted before freeing their buffers
    // (xhci_cancel_endpoint()); the slot is still enabled.
    void (*unbind)(struct usb_device_info *d, int iface);
} __attribute__((aligned(32)));

// File scope: `static const struct usb_match ecm_matches[] = { ... };
// USB_DRIVER("cdc-ecm", ecm_matches, ecm_bind, ecm_unbind);`
#define USB_DRIVER(name_str, table, bind_fn, unbind_fn)                       \
    static const struct usb_driver usb_driver_##bind_fn                       \
        __attribute__((used, section(".usb_drivers"))) = {                    \
            .name = name_str, .matches = table,                                \
            .nmatches = (int)(sizeof(table) / sizeof((table)[0])),             \
            .bind = bind_fn, .unbind = unbind_fn,                              \
        }

int usb_driver_count(void);
const struct usb_driver *usb_driver_at(int i);

// The first driver whose table takes this interface of a device with
// these ids, or NULL.
const struct usb_driver *usb_driver_for(uint16_t vendor, uint16_t product,
                                        const struct usb_interface_info *ifc);

// Does this configuration hold an interface some driver here takes?
// Enumeration's test for choosing among a device's configurations.
int usb_config_driveable(const uint8_t *cfg, uint32_t total,
                         uint16_t vendor, uint16_t product);

// Offer every interface of `d` to the table, and take them all back.
void usb_bind_drivers(struct usb_device_info *d, const uint8_t *cfg, uint32_t total);
void usb_unbind_drivers(struct usb_device_info *d);

// From inside bind: the sibling interface `ifnum` (a bInterfaceNumber)
// belongs to this driver too. 0 when another driver already holds it.
int usb_interface_claim(struct usb_device_info *d, uint8_t ifnum, const char *driver);

#endif
