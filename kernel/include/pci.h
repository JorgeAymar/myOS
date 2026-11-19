#pragma once
// PCI bus enumeration (legacy config mechanism #1, ports 0xCF8/0xCFC) and MMIO
// mapping. Implemented in kernel/drivers/pci.c (owner: core).
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct pci_device {
    uint8_t bus, slot, func;
    uint16_t vendor, device;
    uint8_t class_code, subclass, prog_if, revision;
    uint8_t irq_line;   // legacy PIC IRQ (0xff = none)
    uint8_t irq_pin;    // 1=INTA .. 4=INTD, 0 = none
};

void pci_init(void);                     // enumerates all buses, prints "[OK] pci N devices"
int pci_count(void);
const struct pci_device *pci_get(int index);
const struct pci_device *pci_find(uint16_t vendor, uint16_t device);
// prog_if = 0xff matches any.
const struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if);

uint32_t pci_read32(const struct pci_device *d, uint8_t off);
uint16_t pci_read16(const struct pci_device *d, uint8_t off);
uint8_t  pci_read8(const struct pci_device *d, uint8_t off);
void pci_write32(const struct pci_device *d, uint8_t off, uint32_t v);
void pci_write16(const struct pci_device *d, uint8_t off, uint16_t v);

// Decodes BAR n (handles 64-bit memory BARs). Returns the base address (physical
// for memory BARs, port number for I/O BARs), sets *is_io and *size (if non-NULL).
uint64_t pci_bar(const struct pci_device *d, int n, bool *is_io, uint64_t *size);
// Sets I/O space, memory space and bus-master bits in the command register.
void pci_enable(const struct pci_device *d);

// Maps a physical MMIO range (uncached) into kernel virtual space and returns
// a pointer to it. Panics on failure. Never unmapped.
void *mmio_map(uint64_t phys, size_t size);
