// 4-level paging on the current address space (CR3). Limine's tables are kept and extended;
// intermediate tables come from the PMM and are reached through the HHDM.
#include "mm.h"
#include "mm_internal.h"
#include "panic.h"

#define PTE_PRESENT (1ull << 0)
#define PTE_WRITE   (1ull << 1)
#define PTE_USER    (1ull << 2)
#define PTE_HUGE    (1ull << 7)

static bool nx_supported;

static inline uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}
static inline void invlpg(uint64_t va) { __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory"); }
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t *table_at(uint64_t entry) { return phys_to_virt(entry & PAGE_MASK_ADDR); }
static inline unsigned idx(uint64_t va, int level) { return (va >> (12 + 9 * level)) & 0x1ff; }

void vmm_init(void) {
    nx_supported = rdmsr(0xC0000080) & (1ull << 11);   // EFER.NXE
}

// Returns the page table (level 0) covering va, creating missing tables if `create`.
// Returns NULL if a table is missing (and !create), allocation fails, or a huge page is in the way.
static uint64_t *walk(uint64_t va, bool create, bool user) {
    uint64_t *t = table_at(read_cr3());
    for (int level = 3; level > 0; level--) {
        uint64_t *e = &t[idx(va, level)];
        if (!(*e & PTE_PRESENT)) {
            if (!create) return NULL;
            uint64_t p = pmm_alloc_page();
            if (!p) return NULL;
            *e = p | PTE_PRESENT | PTE_WRITE | (user ? PTE_USER : 0);
        } else {
            if (*e & PTE_HUGE) return NULL;   // 1 GiB / 2 MiB page: not a table
            if (user) *e |= PTE_USER;
        }
        t = table_at(*e);
    }
    return t;
}

static bool vmm_map_unlocked(uint64_t virt, uint64_t phys, uint64_t flags) {
    if ((virt | phys) & (PAGE_SIZE - 1)) return false;
    if (!nx_supported) flags &= ~VMM_NX;
    uint64_t *pt = walk(virt, true, flags & VMM_USER);
    if (!pt) return false;
    pt[idx(virt, 0)] = (phys & PAGE_MASK_ADDR) | (flags & (VMM_NX | 0xfff)) | PTE_PRESENT;
    invlpg(virt);
    return true;
}

static void vmm_unmap_unlocked(uint64_t virt) {
    virt &= ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t *pt = walk(virt, false, false);
    if (!pt) return;
    pt[idx(virt, 0)] = 0;
    invlpg(virt);
}

uint64_t vmm_translate(uint64_t virt) {
    uint64_t *t = table_at(read_cr3());
    for (int level = 3; level >= 0; level--) {
        uint64_t e = t[idx(virt, level)];
        if (!(e & PTE_PRESENT)) return 0;
        if (level == 0 || ((e & PTE_HUGE) && level <= 2)) {
            uint64_t span = 1ull << (12 + 9 * level);
            return ((e & PAGE_MASK_ADDR) & ~(span - 1)) + (virt & (span - 1));
        }
        t = table_at(e);
    }
    return 0;
}

// ---- locked public entry points ----
bool vmm_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t f = mm_irq_save();
    bool r = vmm_map_unlocked(virt, phys, flags);
    mm_irq_restore(f);
    return r;
}
void vmm_unmap(uint64_t virt) {
    uint64_t f = mm_irq_save();
    vmm_unmap_unlocked(virt);
    mm_irq_restore(f);
}
