// PCI configuration space access, bus enumeration and MMIO mapping. Owner: core.
#include "pci.h"
#include "io.h"
#include "mm.h"
#include "kprintf.h"
#include "panic.h"

#define PCI_MAX_DEVICES 64
#define MMIO_BASE  0xffffd00000000000ull   // PML4 slot 416, clear of HHDM/heap/kernel
#define MMIO_LIMIT (MMIO_BASE + (1ull << 36))

static struct pci_device devices[PCI_MAX_DEVICES];
static int device_count;
static uint64_t mmio_next = MMIO_BASE;

static inline void outl(uint16_t port, uint32_t v) {
    __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t v;
    __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static uint32_t cfg_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
           ((uint32_t)func << 8) | (off & 0xfc);
}
static uint32_t raw_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    outl(0xcf8, cfg_addr(bus, slot, func, off));
    return inl(0xcfc);
}

uint32_t pci_read32(const struct pci_device *d, uint8_t off) {
    return raw_read32(d->bus, d->slot, d->func, off);
}
uint16_t pci_read16(const struct pci_device *d, uint8_t off) {
    return (uint16_t)(pci_read32(d, off) >> ((off & 2) * 8));
}
uint8_t pci_read8(const struct pci_device *d, uint8_t off) {
    return (uint8_t)(pci_read32(d, off) >> ((off & 3) * 8));
}
void pci_write32(const struct pci_device *d, uint8_t off, uint32_t v) {
    outl(0xcf8, cfg_addr(d->bus, d->slot, d->func, off));
    outl(0xcfc, v);
}
void pci_write16(const struct pci_device *d, uint8_t off, uint16_t v) {
    uint32_t old = pci_read32(d, off);
    int shift = (off & 2) * 8;
    old = (old & ~(0xffffu << shift)) | ((uint32_t)v << shift);
    pci_write32(d, off & 0xfc, old);
}

uint64_t pci_bar(const struct pci_device *d, int n, bool *is_io, uint64_t *size) {
    uint8_t off = (uint8_t)(0x10 + n * 4);
    uint32_t bar = pci_read32(d, off);
    if (bar & 1) {
        if (is_io) *is_io = true;
        if (size) {
            pci_write32(d, off, 0xffffffff);
            uint32_t m = pci_read32(d, off) & ~3u;
            pci_write32(d, off, bar);
            *size = (uint16_t)(~m + 1);
        }
        return bar & ~3u;
    }
    if (is_io) *is_io = false;
    bool is64 = ((bar >> 1) & 3) == 2;
    uint64_t base = bar & ~0xfu;
    if (is64) base |= (uint64_t)pci_read32(d, off + 4) << 32;
    if (size) {
        pci_write32(d, off, 0xffffffff);
        uint64_t m = pci_read32(d, off) & ~0xfu;
        pci_write32(d, off, bar);
        if (is64) {
            uint32_t hi = pci_read32(d, off + 4);
            pci_write32(d, off + 4, 0xffffffff);
            m |= (uint64_t)pci_read32(d, off + 4) << 32;
            pci_write32(d, off + 4, hi);
        } else {
            m |= 0xffffffff00000000ull;
        }
        *size = ~m + 1;
    }
    return base;
}

void pci_enable(const struct pci_device *d) {
    pci_write16(d, 0x04, pci_read16(d, 0x04) | 0x7);
}

void *mmio_map(uint64_t phys, size_t size) {
    uint64_t start = phys & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end = (phys + size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t virt = mmio_next;
    if (virt + (end - start) > MMIO_LIMIT) panic("mmio_map: out of MMIO space");
    for (uint64_t p = start; p < end; p += PAGE_SIZE) {
        if (!vmm_map(virt + (p - start), p, VMM_PRESENT | VMM_WRITE | VMM_NOCACHE | VMM_NX))
            panic("mmio_map: vmm_map failed for %p", (void *)p);
    }
    mmio_next += (end - start) + PAGE_SIZE;   // guard page between mappings
    return (void *)(virt + (phys - start));
}

static void add_function(uint8_t bus, uint8_t slot, uint8_t func) {
    if (device_count >= PCI_MAX_DEVICES) return;
    uint32_t id = raw_read32(bus, slot, func, 0);
    struct pci_device *d = &devices[device_count++];
    d->bus = bus; d->slot = slot; d->func = func;
    d->vendor = (uint16_t)id; d->device = (uint16_t)(id >> 16);
    uint32_t cls = raw_read32(bus, slot, func, 0x08);
    d->revision = (uint8_t)cls; d->prog_if = (uint8_t)(cls >> 8);
    d->subclass = (uint8_t)(cls >> 16); d->class_code = (uint8_t)(cls >> 24);
    uint32_t irq = raw_read32(bus, slot, func, 0x3c);
    d->irq_line = (uint8_t)irq; d->irq_pin = (uint8_t)(irq >> 8);
}

void pci_init(void) {
    device_count = 0;
    for (int bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t id = raw_read32((uint8_t)bus, slot, 0, 0);
            if ((id & 0xffff) == 0xffff) continue;
            uint8_t hdr = (uint8_t)(raw_read32((uint8_t)bus, slot, 0, 0x0c) >> 16);
            int nfunc = (hdr & 0x80) ? 8 : 1;
            for (uint8_t f = 0; f < nfunc; f++) {
                if ((raw_read32((uint8_t)bus, slot, f, 0) & 0xffff) == 0xffff) continue;
                add_function((uint8_t)bus, slot, f);
            }
        }
    }
    for (int i = 0; i < device_count; i++) {
        struct pci_device *d = &devices[i];
        kprintf("[pci] %02x:%02x.%u %04x:%04x class %02x.%02x.%02x irq %u\n",
                d->bus, d->slot, d->func, d->vendor, d->device,
                d->class_code, d->subclass, d->prog_if, d->irq_line);
    }
    kprintf("[OK] pci %d devices\n", device_count);
}

int pci_count(void) { return device_count; }
const struct pci_device *pci_get(int i) { return (i >= 0 && i < device_count) ? &devices[i] : 0; }

const struct pci_device *pci_find(uint16_t vendor, uint16_t device) {
    for (int i = 0; i < device_count; i++)
        if (devices[i].vendor == vendor && devices[i].device == device) return &devices[i];
    return 0;
}

const struct pci_device *pci_find_class(uint8_t c, uint8_t s, uint8_t p) {
    for (int i = 0; i < device_count; i++) {
        const struct pci_device *d = &devices[i];
        if (d->class_code == c && d->subclass == s && (p == 0xff || d->prog_if == p)) return d;
    }
    return 0;
}
