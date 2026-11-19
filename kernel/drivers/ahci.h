#pragma once
// AHCI register layout (AHCI 1.3.1). Private to kernel/drivers/ahci.c.
#include <stdint.h>

// Generic host control
#define HBA_CAP     0x00
#define HBA_GHC     0x04
#define HBA_IS      0x08
#define HBA_PI      0x0c
#define HBA_VS      0x10
#define HBA_CAP2    0x24
#define HBA_BOHC    0x28

#define GHC_HR      (1u << 0)
#define GHC_IE      (1u << 1)
#define GHC_AE      (1u << 31)
#define CAP2_BOH    (1u << 0)
#define BOHC_BOS    (1u << 0)
#define BOHC_OOS    (1u << 1)
#define BOHC_BB     (1u << 4)

// Port registers (offset 0x100 + port * 0x80)
#define PORT_BASE(p) (0x100u + (uint32_t)(p) * 0x80u)
#define PX_CLB      0x00
#define PX_CLBU     0x04
#define PX_FB       0x08
#define PX_FBU      0x0c
#define PX_IS       0x10
#define PX_IE       0x14
#define PX_CMD      0x18
#define PX_TFD      0x20
#define PX_SIG      0x24
#define PX_SSTS     0x28
#define PX_SCTL     0x2c
#define PX_SERR     0x30
#define PX_SACT     0x34
#define PX_CI       0x38

#define PXCMD_ST    (1u << 0)
#define PXCMD_SUD   (1u << 1)
#define PXCMD_POD   (1u << 2)
#define PXCMD_FRE   (1u << 4)
#define PXCMD_FR    (1u << 14)
#define PXCMD_CR    (1u << 15)

#define PXIS_TFES   (1u << 30)

#define TFD_ERR     (1u << 0)
#define TFD_DRQ     (1u << 3)
#define TFD_BSY     (1u << 7)

#define SIG_SATA    0x00000101u
#define SIG_ATAPI   0xeb140101u

#define FIS_TYPE_REG_H2D 0x27

#define ATA_CMD_IDENTIFY       0xec
#define ATA_CMD_READ_DMA_EXT   0x25
#define ATA_CMD_WRITE_DMA_EXT  0x35
#define ATA_CMD_FLUSH_EXT      0xea

struct ahci_cmd_header {
    uint16_t flags;               // bits 0-4 CFL (dwords), bit 6 W, bit 10 C
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved[4];
} __attribute__((packed));

struct ahci_prdt {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc;                 // bits 0-21 byte count - 1, bit 31 IOC
} __attribute__((packed));

struct fis_reg_h2d {
    uint8_t type;
    uint8_t flags;                // bit 7: command
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0, lba1, lba2;
    uint8_t device;
    uint8_t lba3, lba4, lba5;
    uint8_t featureh;
    uint8_t countl, counth;
    uint8_t icc;
    uint8_t control;
    uint8_t reserved[4];
} __attribute__((packed));

struct ahci_cmd_table {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    struct ahci_prdt prdt[];      // at offset 0x80
} __attribute__((packed));
