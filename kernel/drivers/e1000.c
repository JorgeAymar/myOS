// Intel 8254x/8257x ("e1000" family) NIC driver: legacy RX/TX descriptor rings
// in PMM pages, MAC from RAL/RAH (EEPROM fallback), polled receive with an
// optional legacy IRQ that only acknowledges the cause register.
// Owner: net agent. Supported: 82540EM (8086:100e, QEMU "e1000"), 82545EM
// (8086:100f), 82574L (8086:10d3, QEMU "e1000e", q35 default).
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "pci.h"
#include "mm.h"
#include "arch.h"
#include "string.h"
#include "kprintf.h"
#include "../net/net_internal.h"

// Registers (byte offsets into BAR0)
#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_EERD   0x0014
#define REG_ICR    0x00c0
#define REG_IMS    0x00d0
#define REG_IMC    0x00d8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_RDTR   0x2820
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200
#define REG_RAL0   0x5400
#define REG_RAH0   0x5404

#define CTRL_ASDE  (1u << 5)
#define CTRL_SLU   (1u << 6)
#define CTRL_RST   (1u << 26)
#define STATUS_LU  (1u << 1)
#define RCTL_EN    (1u << 1)
#define RCTL_BAM   (1u << 15)
#define RCTL_SECRC (1u << 26)
#define TCTL_EN    (1u << 1)
#define TCTL_PSP   (1u << 3)
#define ICR_TXDW   (1u << 0)
#define ICR_LSC    (1u << 2)
#define ICR_RXT0   (1u << 7)

#define TX_CMD_EOP  (1u << 0)
#define TX_CMD_IFCS (1u << 1)
#define TX_CMD_RS   (1u << 3)
#define DESC_DD     (1u << 0)
#define RX_EOP      (1u << 1)

#define RX_COUNT 32
#define TX_COUNT 32
#define BUF_SIZE 2048

struct rx_desc {
    uint64_t addr;
    uint16_t length, csum;
    uint8_t status, errors;
    uint16_t special;
} __attribute__((packed));

struct tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t cso, cmd, status, css;
    uint16_t special;
} __attribute__((packed));

static volatile uint8_t *regs;
static volatile struct rx_desc *rx_ring;
static volatile struct tx_desc *tx_ring;
static uint8_t *rx_buf[RX_COUNT];
static uint8_t *tx_buf[TX_COUNT];
static uint32_t rx_cur, tx_cur;
static bool is_82574;
static volatile uint32_t irq_count;

static inline uint32_t rd(uint32_t r) { return *(volatile uint32_t *)(regs + r); }
static inline void wr(uint32_t r, uint32_t v) { *(volatile uint32_t *)(regs + r) = v; }
#define barrier() __asm__ volatile ("" ::: "memory")

static void delay_ms(uint32_t ms) {
    uint64_t end = timer_ticks_ms() + ms;
    while (timer_ticks_ms() < end) __asm__ volatile ("pause");
}

static uint64_t alloc_page_phys(void) {
    uint64_t p = pmm_alloc_page();
    if (!p) return 0;
    memset(phys_to_virt(p), 0, PAGE_SIZE);
    return p;
}

// EEPROM word read through EERD (layout differs between 8254x and 82574).
static bool eeprom_read(uint8_t addr, uint16_t *out) {
    uint32_t start = is_82574 ? (((uint32_t)addr << 2) | 1) : (((uint32_t)addr << 8) | 1);
    uint32_t done = is_82574 ? (1u << 1) : (1u << 4);
    wr(REG_EERD, start);
    for (int i = 0; i < 100000; i++) {
        uint32_t v = rd(REG_EERD);
        if (v & done) { *out = (uint16_t)(v >> 16); return true; }
    }
    return false;
}

static void e1000_irq(void) {
    (void)rd(REG_ICR);   // reading ICR acknowledges and deasserts the line
    irq_count++;
}

static bool supported(const struct pci_device *d) {
    if (d->vendor != 0x8086) return false;
    switch (d->device) {
    case 0x100e: case 0x100f: case 0x1004: case 0x10d3: return true;
    default: return false;
    }
}

bool e1000_init(uint8_t mac_out[6], const char **model_out) {
    const struct pci_device *dev = 0;
    for (int i = 0; i < pci_count(); i++)
        if (supported(pci_get(i))) { dev = pci_get(i); break; }
    if (!dev) return false;
    is_82574 = dev->device == 0x10d3;
    *model_out = is_82574 ? "82574L" : (dev->device == 0x100f ? "82545EM" : "82540EM");

    bool is_io; uint64_t size;
    uint64_t bar = pci_bar(dev, 0, &is_io, &size);
    if (is_io || !bar) { kprintf("[net] e1000: BAR0 is not MMIO\n"); return false; }
    if (size < 0x20000 || size > 0x100000) size = 0x20000;
    pci_enable(dev);
    regs = mmio_map(bar, size);

    // Global reset, then mask everything and clear pending causes.
    wr(REG_IMC, 0xffffffff);
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_RST);
    delay_ms(2);
    for (int i = 0; i < 1000 && (rd(REG_CTRL) & CTRL_RST); i++) delay_ms(1);
    wr(REG_IMC, 0xffffffff);
    (void)rd(REG_ICR);

    // Link up: set link-up + auto speed detection, clear LRST/PHY reset/ILOS/VME.
    uint32_t ctrl = rd(REG_CTRL);
    ctrl |= CTRL_SLU | CTRL_ASDE;
    ctrl &= ~((1u << 3) | (1u << 31) | (1u << 7) | (1u << 30));
    wr(REG_CTRL, ctrl);

    // MAC address: RAL/RAH are loaded from the EEPROM after reset.
    uint32_t ral = rd(REG_RAL0), rah = rd(REG_RAH0);
    uint8_t mac[6] = { (uint8_t)ral, (uint8_t)(ral >> 8), (uint8_t)(ral >> 16),
                       (uint8_t)(ral >> 24), (uint8_t)rah, (uint8_t)(rah >> 8) };
    if (!(rah & (1u << 31)) || (ral == 0 && (rah & 0xffff) == 0)) {
        for (uint8_t w = 0; w < 3; w++) {
            uint16_t v = 0;
            if (!eeprom_read(w, &v)) { kprintf("[net] e1000: EEPROM read failed\n"); return false; }
            mac[w * 2] = (uint8_t)v; mac[w * 2 + 1] = (uint8_t)(v >> 8);
        }
    }
    wr(REG_RAL0, (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) | ((uint32_t)mac[2] << 16) |
                 ((uint32_t)mac[3] << 24));
    wr(REG_RAH0, (uint32_t)mac[4] | ((uint32_t)mac[5] << 8) | (1u << 31));
    memcpy(mac_out, mac, 6);
    for (int i = 0; i < 128; i++) wr(REG_MTA + i * 4, 0);

    // Descriptor rings: one page each (32 * 16 bytes); buffers 2 KiB, 2 per page.
    uint64_t rx_ring_phys = alloc_page_phys(), tx_ring_phys = alloc_page_phys();
    if (!rx_ring_phys || !tx_ring_phys) return false;
    rx_ring = phys_to_virt(rx_ring_phys);
    tx_ring = phys_to_virt(tx_ring_phys);
    for (int i = 0; i < RX_COUNT; i += 2) {
        uint64_t p = alloc_page_phys();
        if (!p) return false;
        for (int j = 0; j < 2; j++) {
            rx_buf[i + j] = (uint8_t *)phys_to_virt(p) + j * BUF_SIZE;
            rx_ring[i + j].addr = p + (uint64_t)j * BUF_SIZE;
            rx_ring[i + j].status = 0;
        }
    }
    for (int i = 0; i < TX_COUNT; i += 2) {
        uint64_t p = alloc_page_phys();
        if (!p) return false;
        for (int j = 0; j < 2; j++) {
            tx_buf[i + j] = (uint8_t *)phys_to_virt(p) + j * BUF_SIZE;
            tx_ring[i + j].addr = p + (uint64_t)j * BUF_SIZE;
            tx_ring[i + j].status = DESC_DD;   // free
            tx_ring[i + j].cmd = 0;
        }
    }

    wr(REG_RDBAL, (uint32_t)rx_ring_phys);
    wr(REG_RDBAH, (uint32_t)(rx_ring_phys >> 32));
    wr(REG_RDLEN, RX_COUNT * sizeof(struct rx_desc));
    wr(REG_RDH, 0);
    wr(REG_RDT, RX_COUNT - 1);
    wr(REG_RDTR, 0);
    rx_cur = 0;
    // 2048-byte buffers (BSIZE=00, BSEX=0), accept broadcast, strip CRC.
    wr(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

    wr(REG_TDBAL, (uint32_t)tx_ring_phys);
    wr(REG_TDBAH, (uint32_t)(tx_ring_phys >> 32));
    wr(REG_TDLEN, TX_COUNT * sizeof(struct tx_desc));
    wr(REG_TDH, 0);
    wr(REG_TDT, 0);
    tx_cur = 0;
    wr(REG_TIPG, 10 | (8 << 10) | (6 << 20));
    wr(REG_TCTL, TCTL_EN | TCTL_PSP | (0x0f << 4) | (0x40 << 12));

    // Interrupts: only if the legacy line is not shared with another PCI
    // function (irq_register keeps a single handler per line). The handler just
    // acknowledges ICR; frames are processed by net_poll(), so the IRQ merely
    // wakes hlt-based wait loops early.
    bool shared = false;
    for (int i = 0; i < pci_count(); i++) {
        const struct pci_device *o = pci_get(i);
        if (o != dev && o->irq_pin && o->irq_line == dev->irq_line) shared = true;
    }
    if (dev->irq_line < 16 && dev->irq_pin && !shared) {
        irq_register(dev->irq_line, e1000_irq);
        wr(REG_IMS, ICR_RXT0 | ICR_LSC);
    }

    // Wait for link (QEMU reports it immediately).
    for (int i = 0; i < 500 && !e1000_link_up(); i++) delay_ms(1);
    return true;
}

bool e1000_link_up(void) { return regs && (rd(REG_STATUS) & STATUS_LU); }

bool e1000_send(const void *frame, size_t len) {
    if (!regs || len > BUF_SIZE) return false;
    volatile struct tx_desc *d = &tx_ring[tx_cur];
    uint64_t end = timer_ticks_ms() + 50;
    while (!(d->status & DESC_DD)) {
        if (timer_ticks_ms() > end) return false;
        __asm__ volatile ("pause");
    }
    memcpy(tx_buf[tx_cur], frame, len);
    if (len < 60) { memset(tx_buf[tx_cur] + len, 0, 60 - len); len = 60; }
    d->length = (uint16_t)len;
    d->cso = 0; d->css = 0; d->special = 0;
    d->cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    d->status = 0;
    barrier();
    tx_cur = (tx_cur + 1) % TX_COUNT;
    wr(REG_TDT, tx_cur);
    return true;
}

size_t e1000_recv(void *buf, size_t max) {
    if (!regs) return 0;
    for (;;) {
        volatile struct rx_desc *d = &rx_ring[rx_cur];
        uint8_t st = d->status;
        if (!(st & DESC_DD)) return 0;
        barrier();
        size_t len = d->length;
        bool good = (st & RX_EOP) && d->errors == 0 && len <= max;
        if (good) memcpy(buf, rx_buf[rx_cur], len);
        d->status = 0;
        barrier();
        wr(REG_RDT, rx_cur);
        rx_cur = (rx_cur + 1) % RX_COUNT;
        if (good) return len;   // otherwise drop (multi-descriptor/errored) and continue
    }
}
