#ifndef DRIVERS_PCI_H
#define DRIVERS_PCI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* what is plugged into this machine.
 *
 * every device on the bus answers to 256 bytes of configuration space
 * whose first few fields are the same for all of them: who made it,
 * what it is, and roughly what sort of thing that makes it. reading
 * those is the whole of enumeration, and it is the doorway to every
 * real driver -- you cannot write to a disk you have not found. */

#define PCI_MAX_DEVICES 32

struct pci_device {
    uint8_t  bus, slot, function;

    uint16_t vendor;
    uint16_t device;

    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  header_type;

    uint8_t  irq_line;
    uint32_t bar[6];
};

/* how a base address register is to be read. the low bits say which */
struct pci_bar {
    bool     is_io;         /* a port range rather than a memory window */
    bool     is_64bit;      /* this bar and the next one are one address */
    bool     prefetchable;
    uint64_t address;
};

/* config space, as a function, so a test can supply its own machine.
 * offset is in bytes and must be a multiple of four */
typedef uint32_t (*pci_reader)(uint8_t bus, uint8_t slot, uint8_t fn,
                               uint8_t offset);

/* walk the buses and record what answers. `read` lets the tests hand
 * over a machine of their own invention rather than needing one */
size_t pci_scan_with(pci_reader read);

/* the same, against the real hardware */
void pci_scan(void);

size_t pci_count(void);
const struct pci_device *pci_at(size_t index);

/* ---- decoding, which is pure and therefore worth testing ---- */

/* what sort of thing this is, in words. the class alone is often too
 * vague to be useful ("mass storage") so the subclass is folded in */
const char *pci_class_name(uint8_t class_code, uint8_t subclass);

/* who made it, if we happen to know */
const char *pci_vendor_name(uint16_t vendor);

/* and what they called it. NULL when we have never heard of it, which
 * is most of the time and is not a problem -- the class still says
 * what it is for */
const char *pci_device_name(uint16_t vendor, uint16_t device);

struct pci_bar pci_decode_bar(uint32_t raw);

#endif
