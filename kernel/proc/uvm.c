// Per-process address spaces. The lower half (PML4 0..255) is private to the
// process; the upper half is the kernel's and is re-copied from the kernel
// PML4 before every switch to a user thread, so kernel mappings created later
// (heap growth, MMIO) are always visible. Tables are edited via the HHDM, so
// no CR3 switch is needed to build an address space.
#include <stdint.h>
#include "mm.h"
#include "string.h"
#include "proc_internal.h"

#define PTE_P   (1ull << 0)
#define PTE_W   (1ull << 1)
#define PTE_U   (1ull << 2)
#define PTE_PS  (1ull << 7)
#define PTE_NX  (1ull << 63)
#define PTE_ADDR 0x000ffffffffff000ull

static uint64_t kernel_pml4;
static int pages_live;               // user pages + their page tables
static bool nx_ok;

int uvm_pages_live(void) { return pages_live; }

static inline uint64_t *tbl(uint64_t phys) { return phys_to_virt(phys & PTE_ADDR); }
static inline unsigned ix(uint64_t va, int level) { return (va >> (12 + 9 * level)) & 0x1ff; }

static uint64_t alloc_page(void) {
    uint64_t f = irq_save();            // pmm has no lock yet
    uint64_t p = pmm_alloc_page();
    irq_restore(f);
    if (p) pages_live++;
    return p;
}

static void free_page(uint64_t p) {
    uint64_t f = irq_save();
    pmm_free_page(p);
    irq_restore(f);
    pages_live--;
}

void uvm_init(uint64_t kernel_cr3) {
    kernel_pml4 = kernel_cr3 & PTE_ADDR;
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
    nx_ok = lo & (1u << 11);
}

void uvm_sync_kernel(uint64_t pml4) {
    memcpy(tbl(pml4) + 256, tbl(kernel_pml4) + 256, 256 * sizeof(uint64_t));
}

uint64_t uvm_create(void) {
    uint64_t p = alloc_page();          // zeroed by the PMM
    if (p) uvm_sync_kernel(p);
    return p;
}

// Leaf PTE slot for va, creating tables if needed. NULL on OOM.
static uint64_t *walk(uint64_t pml4, uint64_t va, bool create) {
    uint64_t *t = tbl(pml4);
    for (int level = 3; level > 0; level--) {
        uint64_t *e = &t[ix(va, level)];
        if (!(*e & PTE_P)) {
            if (!create) return NULL;
            uint64_t p = alloc_page();
            if (!p) return NULL;
            *e = p | PTE_P | PTE_W | PTE_U;
        }
        t = tbl(*e);
    }
    return &t[ix(va, 0)];
}

bool uvm_map(uint64_t pml4, uint64_t va, uint64_t phys, bool writable, bool exec) {
    if (va >= USER_TOP || (va & (PAGE_SIZE - 1))) return false;
    uint64_t *pte = walk(pml4, va, true);
    if (!pte) return false;
    *pte = phys | PTE_P | PTE_U | (writable ? PTE_W : 0) | (!exec && nx_ok ? PTE_NX : 0);
    return true;
}

// Allocates and maps a zeroed page; returns its kernel (HHDM) address or NULL.
void *uvm_alloc(uint64_t pml4, uint64_t va, bool writable, bool exec) {
    uint64_t *pte = walk(pml4, va, false);
    if (pte && (*pte & PTE_P)) return tbl(*pte);         // already mapped (shared page of 2 segments)
    uint64_t p = alloc_page();
    if (!p) return NULL;
    if (!uvm_map(pml4, va, p, writable, exec)) { free_page(p); return NULL; }
    return phys_to_virt(p);
}

// Kernel address of user va if mapped user-accessible (and writable if asked).
void *uvm_user_ptr(uint64_t pml4, uint64_t va, bool write) {
    if (va >= USER_TOP) return NULL;
    uint64_t *pte = walk(pml4, va & ~(uint64_t)(PAGE_SIZE - 1), false);
    if (!pte || !(*pte & PTE_P) || !(*pte & PTE_U)) return NULL;
    if (write && !(*pte & PTE_W)) return NULL;
    return (uint8_t *)tbl(*pte) + (va & (PAGE_SIZE - 1));
}

static void free_level(uint64_t table, int level) {
    uint64_t *t = tbl(table);
    for (int i = 0; i < (level == 3 ? 256 : 512); i++) {
        if (!(t[i] & PTE_P)) continue;
        if (level > 0) free_level(t[i] & PTE_ADDR, level - 1);
        else free_page(t[i] & PTE_ADDR);
    }
    free_page(table);
}

// Must not be the loaded CR3.
void uvm_destroy(uint64_t pml4) { free_level(pml4, 3); }
