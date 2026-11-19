// AHCI SATA driver (polling, one command slot per port). Owner: fs agent.
// Finds the controller by PCI class 01.06.01, takes it over from the firmware,
// and registers every SATA disk as a block device. ATAPI ports are skipped.
// Every wait has a timeout so a missing or broken device never hangs boot.
#include "ahci.h"
#include "blockdev.h"
#include "vfs.h"
#include "pci.h"
#include "mm.h"
#include "arch.h"
#include "string.h"
#include "kprintf.h"

#define AHCI_MAX_PORTS   32
#define BOUNCE_PAGES     16                         // 64 KiB per command
#define SECTOR           512
#define MAX_SECTORS_CMD  (BOUNCE_PAGES * PAGE_SIZE / SECTOR)
#define CMD_TIMEOUT_MS   5000

struct ahci_port {
    struct blockdev bd;
    volatile uint8_t *regs;
    int index;
    uint64_t clb_phys;                              // command list (1 KiB) + received FIS at +0x400
    uint64_t ctba_phys;                             // command table for slot 0
    uint64_t bounce_phys[BOUNCE_PAGES];
};

static struct {
    volatile uint8_t *abar;
    struct ahci_port *ports[AHCI_MAX_PORTS];
    int disks;
} ahci;

static inline uint32_t hba_rd(uint32_t off) { return *(volatile uint32_t *)(ahci.abar + off); }
static inline void hba_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(ahci.abar + off) = v; }
static inline uint32_t px_rd(struct ahci_port *p, uint32_t off) { return *(volatile uint32_t *)(p->regs + off); }
static inline void px_wr(struct ahci_port *p, uint32_t off, uint32_t v) { *(volatile uint32_t *)(p->regs + off) = v; }

// Waits until (read() & mask) == value or timeout. Returns true on success.
static bool wait_hba(uint32_t off, uint32_t mask, uint32_t value, uint64_t ms) {
    uint64_t end = timer_ticks_ms() + ms;
    for (;;) {
        if ((hba_rd(off) & mask) == value) return true;
        if (timer_ticks_ms() > end) return (hba_rd(off) & mask) == value;
        __asm__ volatile ("pause");
    }
}
static bool wait_port(struct ahci_port *p, uint32_t off, uint32_t mask, uint32_t value, uint64_t ms) {
    uint64_t end = timer_ticks_ms() + ms;
    for (;;) {
        if ((px_rd(p, off) & mask) == value) return true;
        if (timer_ticks_ms() > end) return (px_rd(p, off) & mask) == value;
        __asm__ volatile ("pause");
    }
}

static bool port_stop(struct ahci_port *p) {
    px_wr(p, PX_CMD, px_rd(p, PX_CMD) & ~PXCMD_ST);
    bool ok = wait_port(p, PX_CMD, PXCMD_CR, 0, 500);
    px_wr(p, PX_CMD, px_rd(p, PX_CMD) & ~PXCMD_FRE);
    ok = wait_port(p, PX_CMD, PXCMD_FR, 0, 500) && ok;
    return ok;
}

static bool port_start(struct ahci_port *p) {
    if (!wait_port(p, PX_CMD, PXCMD_CR, 0, 500)) return false;
    px_wr(p, PX_CMD, px_rd(p, PX_CMD) | PXCMD_FRE);
    px_wr(p, PX_CMD, px_rd(p, PX_CMD) | PXCMD_ST);
    return true;
}

static void port_recover(struct ahci_port *p) {
    port_stop(p);
    px_wr(p, PX_SERR, 0xffffffff);
    px_wr(p, PX_IS, 0xffffffff);
    port_start(p);
}

// Issues one ATA command on slot 0 and polls for completion. The data (if any)
// lives in the bounce buffer: bytes = count * 512, at most MAX_SECTORS_CMD.
static int port_cmd(struct ahci_port *p, uint8_t cmd, uint64_t lba, uint32_t count,
                    uint32_t bytes, bool write) {
    if (!wait_port(p, PX_TFD, TFD_BSY | TFD_DRQ, 0, 1000)) {
        port_recover(p);
        if (!wait_port(p, PX_TFD, TFD_BSY | TFD_DRQ, 0, 1000)) return VFS_EIO;
    }
    struct ahci_cmd_header *hdr = phys_to_virt(p->clb_phys);
    struct ahci_cmd_table *tbl = phys_to_virt(p->ctba_phys);
    memset(tbl, 0, sizeof *tbl + BOUNCE_PAGES * sizeof(struct ahci_prdt));

    uint16_t prdtl = 0;
    for (uint32_t done = 0; done < bytes; prdtl++) {
        uint32_t chunk = bytes - done > PAGE_SIZE ? PAGE_SIZE : bytes - done;
        tbl->prdt[prdtl].dba = (uint32_t)p->bounce_phys[prdtl];
        tbl->prdt[prdtl].dbau = (uint32_t)(p->bounce_phys[prdtl] >> 32);
        tbl->prdt[prdtl].dbc = chunk - 1;
        done += chunk;
    }

    struct fis_reg_h2d *fis = (struct fis_reg_h2d *)tbl->cfis;
    fis->type = FIS_TYPE_REG_H2D;
    fis->flags = 0x80;
    fis->command = cmd;
    fis->device = (cmd == ATA_CMD_IDENTIFY) ? 0 : 0x40;   // LBA mode
    fis->lba0 = (uint8_t)lba;
    fis->lba1 = (uint8_t)(lba >> 8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = (uint8_t)(lba >> 32);
    fis->lba5 = (uint8_t)(lba >> 40);
    fis->countl = (uint8_t)count;
    fis->counth = (uint8_t)(count >> 8);

    memset(hdr, 0, sizeof *hdr);
    hdr->flags = (uint16_t)((sizeof(struct fis_reg_h2d) / 4) | (write ? (1u << 6) : 0));
    hdr->prdtl = prdtl;
    hdr->ctba = (uint32_t)p->ctba_phys;
    hdr->ctbau = (uint32_t)(p->ctba_phys >> 32);

    px_wr(p, PX_IS, 0xffffffff);
    __asm__ volatile ("mfence" ::: "memory");
    px_wr(p, PX_CI, 1);

    uint64_t end = timer_ticks_ms() + CMD_TIMEOUT_MS;
    for (;;) {
        uint32_t is = px_rd(p, PX_IS);
        if (is & PXIS_TFES) {
            kprintf("[ahci] port %d: task file error cmd=%02x tfd=%x\n", p->index, cmd, px_rd(p, PX_TFD));
            port_recover(p);
            return VFS_EIO;
        }
        if (!(px_rd(p, PX_CI) & 1)) break;
        if (timer_ticks_ms() > end) {
            kprintf("[ahci] port %d: command %02x timed out\n", p->index, cmd);
            port_recover(p);
            return VFS_EIO;
        }
        __asm__ volatile ("pause");
    }
    if (px_rd(p, PX_TFD) & TFD_ERR) {
        port_recover(p);
        return VFS_EIO;
    }
    __asm__ volatile ("mfence" ::: "memory");
    return 0;
}

static void bounce_copy_out(struct ahci_port *p, uint8_t *dst, uint32_t bytes) {
    for (uint32_t i = 0; bytes; i++) {
        uint32_t chunk = bytes > PAGE_SIZE ? PAGE_SIZE : bytes;
        memcpy(dst, phys_to_virt(p->bounce_phys[i]), chunk);
        dst += chunk;
        bytes -= chunk;
    }
}

static void bounce_copy_in(struct ahci_port *p, const uint8_t *src, uint32_t bytes) {
    for (uint32_t i = 0; bytes; i++) {
        uint32_t chunk = bytes > PAGE_SIZE ? PAGE_SIZE : bytes;
        memcpy(phys_to_virt(p->bounce_phys[i]), src, chunk);
        src += chunk;
        bytes -= chunk;
    }
}

static int ahci_bd_read(struct blockdev *bd, uint64_t lba, uint32_t count, void *buf) {
    struct ahci_port *p = bd->priv;
    uint8_t *out = buf;
    if (lba + count > bd->sectors) return VFS_EINVAL;
    while (count) {
        uint32_t n = count > MAX_SECTORS_CMD ? MAX_SECTORS_CMD : count;
        int r = port_cmd(p, ATA_CMD_READ_DMA_EXT, lba, n, n * SECTOR, false);
        if (r) return r;
        bounce_copy_out(p, out, n * SECTOR);
        out += n * SECTOR;
        lba += n;
        count -= n;
    }
    return 0;
}

static int ahci_bd_write(struct blockdev *bd, uint64_t lba, uint32_t count, const void *buf) {
    struct ahci_port *p = bd->priv;
    const uint8_t *in = buf;
    if (lba + count > bd->sectors) return VFS_EINVAL;
    while (count) {
        uint32_t n = count > MAX_SECTORS_CMD ? MAX_SECTORS_CMD : count;
        bounce_copy_in(p, in, n * SECTOR);
        int r = port_cmd(p, ATA_CMD_WRITE_DMA_EXT, lba, n, n * SECTOR, true);
        if (r) return r;
        in += n * SECTOR;
        lba += n;
        count -= n;
    }
    return 0;
}

static bool alloc_port_memory(struct ahci_port *p) {
    p->clb_phys = pmm_alloc_page();
    p->ctba_phys = pmm_alloc_page();
    if (!p->clb_phys || !p->ctba_phys) return false;
    for (int i = 0; i < BOUNCE_PAGES; i++) {
        p->bounce_phys[i] = pmm_alloc_page();
        if (!p->bounce_phys[i]) return false;
    }
    return true;
}

static void free_port_memory(struct ahci_port *p) {
    if (p->clb_phys) pmm_free_page(p->clb_phys);
    if (p->ctba_phys) pmm_free_page(p->ctba_phys);
    for (int i = 0; i < BOUNCE_PAGES; i++)
        if (p->bounce_phys[i]) pmm_free_page(p->bounce_phys[i]);
}

static void probe_port(int index, uint32_t cap) {
    struct ahci_port *p = kcalloc(1, sizeof *p);
    if (!p) return;
    p->index = index;
    p->regs = ahci.abar + PORT_BASE(index);

    if (!port_stop(p)) {
        kprintf("[ahci] port %d: cannot stop command engine\n", index);
        kfree(p);
        return;
    }
    if (!alloc_port_memory(p)) {
        kprintf("[ahci] port %d: out of memory\n", index);
        free_port_memory(p);
        kfree(p);
        return;
    }
    uint64_t fb_phys = p->clb_phys + 0x400;
    px_wr(p, PX_CLB, (uint32_t)p->clb_phys);
    px_wr(p, PX_CLBU, (uint32_t)(p->clb_phys >> 32));
    px_wr(p, PX_FB, (uint32_t)fb_phys);
    px_wr(p, PX_FBU, (uint32_t)(fb_phys >> 32));
    px_wr(p, PX_IE, 0);
    px_wr(p, PX_SERR, 0xffffffff);
    px_wr(p, PX_IS, 0xffffffff);
    px_wr(p, PX_CMD, px_rd(p, PX_CMD) | PXCMD_FRE);
    if (cap & (1u << 27)) px_wr(p, PX_CMD, px_rd(p, PX_CMD) | PXCMD_SUD);   // staggered spin-up
    px_wr(p, PX_CMD, px_rd(p, PX_CMD) | PXCMD_POD);

    // Device present and PHY communication established?
    if (!wait_port(p, PX_SSTS, 0xf, 3, 50)) goto skip;
    px_wr(p, PX_SERR, 0xffffffff);
    if (!wait_port(p, PX_TFD, TFD_BSY | TFD_DRQ, 0, 1000)) {
        kprintf("[ahci] port %d: device busy, skipped\n", index);
        goto skip;
    }
    uint32_t sig = px_rd(p, PX_SIG);
    if (sig == SIG_ATAPI) {
        kprintf("[ahci] port %d: ATAPI device, skipped\n", index);
        goto skip;
    }
    if (sig != SIG_SATA) {
        kprintf("[ahci] port %d: unknown signature %x, skipped\n", index, sig);
        goto skip;
    }
    if (!port_start(p)) goto skip;

    if (port_cmd(p, ATA_CMD_IDENTIFY, 0, 0, SECTOR, false)) {
        kprintf("[ahci] port %d: IDENTIFY failed\n", index);
        port_stop(p);
        goto skip;
    }
    uint16_t id[256];
    bounce_copy_out(p, (uint8_t *)id, SECTOR);
    uint64_t sectors;
    if (id[83] & (1u << 10))
        sectors = (uint64_t)id[100] | ((uint64_t)id[101] << 16) | ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    else
        sectors = (uint64_t)id[60] | ((uint64_t)id[61] << 16);
    for (int i = 0; i < 20; i++) {
        p->bd.model[i * 2] = (char)(id[27 + i] >> 8);
        p->bd.model[i * 2 + 1] = (char)id[27 + i];
    }
    p->bd.model[40] = 0;
    for (int i = 39; i >= 0 && (p->bd.model[i] == ' ' || p->bd.model[i] == 0); i--) p->bd.model[i] = 0;

    ksnprintf(p->bd.name, sizeof p->bd.name, "sata%d", ahci.disks);
    p->bd.sectors = sectors;
    p->bd.sector_size = SECTOR;
    p->bd.read = ahci_bd_read;
    p->bd.write = ahci_bd_write;
    p->bd.priv = p;
    ahci.ports[index] = p;
    if (blockdev_register(&p->bd) < 0) {
        kprintf("[ahci] port %d: block device table full\n", index);
        return;
    }
    ahci.disks++;
    kprintf("[OK] ahci port %d sectors=%lu model='%s'\n", index, sectors, p->bd.model);
    return;

skip:
    port_stop(p);
    px_wr(p, PX_CLB, 0); px_wr(p, PX_CLBU, 0);
    px_wr(p, PX_FB, 0);  px_wr(p, PX_FBU, 0);
    free_port_memory(p);
    kfree(p);
}

int ahci_init(void) {
    const struct pci_device *dev = pci_find_class(0x01, 0x06, 0x01);
    if (!dev) {
        kprintf("[WARN] ahci: no AHCI controller found\n");
        return 0;
    }
    bool is_io;
    uint64_t size;
    uint64_t base = pci_bar(dev, 5, &is_io, &size);
    if (is_io || !base) {
        kprintf("[WARN] ahci: BAR5 is not a memory BAR\n");
        return 0;
    }
    if (size < 0x1100) size = 0x1100;
    pci_enable(dev);
    ahci.abar = mmio_map(base, size);

    // BIOS/OS handoff.
    if (hba_rd(HBA_CAP2) & CAP2_BOH) {
        hba_wr(HBA_BOHC, hba_rd(HBA_BOHC) | BOHC_OOS);
        if (wait_hba(HBA_BOHC, BOHC_BOS, 0, 25) && (hba_rd(HBA_BOHC) & BOHC_BB))
            wait_hba(HBA_BOHC, BOHC_BB, 0, 2000);
    }

    // Reset the HBA and enable AHCI mode.
    hba_wr(HBA_GHC, hba_rd(HBA_GHC) | GHC_AE);
    hba_wr(HBA_GHC, hba_rd(HBA_GHC) | GHC_HR);
    if (!wait_hba(HBA_GHC, GHC_HR, 0, 1000)) {
        kprintf("[WARN] ahci: HBA reset timed out\n");
        return 0;
    }
    hba_wr(HBA_GHC, (hba_rd(HBA_GHC) | GHC_AE) & ~GHC_IE);
    hba_wr(HBA_IS, 0xffffffff);

    uint32_t cap = hba_rd(HBA_CAP);
    uint32_t pi = hba_rd(HBA_PI);
    uint32_t vs = hba_rd(HBA_VS);
    if (!(cap & (1u << 31)))
        kprintf("[WARN] ahci: controller lacks 64-bit addressing, assuming low memory\n");
    kprintf("[ahci] %02x:%02x.%u version %x.%x ports=%x cap=%x\n", dev->bus, dev->slot, dev->func,
            vs >> 16, vs & 0xffff, pi, cap);

    for (int i = 0; i < AHCI_MAX_PORTS; i++)
        if (pi & (1u << i)) probe_port(i, cap);
    return ahci.disks;
}
