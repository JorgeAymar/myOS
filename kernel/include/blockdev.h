#pragma once
// Block device registry. Owner: fs agent (kernel/fs/blockdev.c).
// Drivers (AHCI, later virtio-blk) register devices; filesystems consume them.
#include <stdint.h>
#include <stddef.h>

#define BLOCKDEV_MAX 8

struct blockdev {
    char name[16];                // "sata0", ...
    char model[41];
    uint64_t sectors;             // total sectors
    uint32_t sector_size;         // bytes (512)
    // Transfer count sectors starting at lba. buf is any kernel virtual memory.
    // Return 0 on success, negative VFS_E* style error otherwise.
    int (*read)(struct blockdev *bd, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct blockdev *bd, uint64_t lba, uint32_t count, const void *buf);
    void *priv;
};

int blockdev_register(struct blockdev *bd);   // returns index or -1 if full
int blockdev_count(void);
struct blockdev *blockdev_get(int index);

static inline int blockdev_read(struct blockdev *bd, uint64_t lba, uint32_t count, void *buf) {
    return bd->read(bd, lba, count, buf);
}
static inline int blockdev_write(struct blockdev *bd, uint64_t lba, uint32_t count, const void *buf) {
    return bd->write(bd, lba, count, buf);
}

// AHCI driver (kernel/drivers/ahci.c): probes the controller and registers
// every SATA disk. Returns the number of disks found (0 if none / no HBA).
int ahci_init(void);
