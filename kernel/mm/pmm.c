// Physical page allocator: one bit per 4 KiB page (1 = used) over [0, highest usable address).
// Only LIMINE_MEMMAP_USABLE pages are ever freed into the bitmap; everything else stays used.
#include "mm.h"
#include "mm_internal.h"
#include "boot.h"
#include "panic.h"
#include "kprintf.h"

static uint64_t *bitmap;          // virtual (HHDM) pointer
static uint64_t bitmap_pages;     // number of pages tracked
static uint64_t total_bytes;
static uint64_t free_pages;
static uint64_t next_hint;        // word index to start searching from

uint64_t mm_hhdm_offset(void) { return boot_hhdm_offset(); }

void mm_zero_page(void *virt) {
    uint64_t *p = virt;
    for (size_t i = 0; i < PAGE_SIZE / 8; i++) p[i] = 0;
}

static inline bool bit_test(uint64_t pg) { return bitmap[pg / 64] & (1ull << (pg % 64)); }
static inline void bit_set(uint64_t pg) { bitmap[pg / 64] |= 1ull << (pg % 64); }
static inline void bit_clear(uint64_t pg) { bitmap[pg / 64] &= ~(1ull << (pg % 64)); }

void pmm_init(void) {
    struct limine_memmap_response *mm = boot_memmap();
    uint64_t top = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        total_bytes += e->length;
        if (e->base + e->length > top) top = e->base + e->length;
    }
    if (!top) panic("pmm: no usable memory");

    bitmap_pages = top / PAGE_SIZE;
    uint64_t words = (bitmap_pages + 63) / 64;
    uint64_t bm_bytes = words * 8;
    uint64_t bm_size = (bm_bytes + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    // Place the bitmap in the first usable region (page-aligned, above page 0) that fits it.
    uint64_t bm_phys = 0;
    for (uint64_t i = 0; i < mm->entry_count && !bm_phys; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        uint64_t b = (e->base + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        if (b == 0) b = PAGE_SIZE;
        uint64_t end = (e->base + e->length) & ~(uint64_t)(PAGE_SIZE - 1);
        if (end > b && end - b >= bm_size) bm_phys = b;
    }
    if (!bm_phys) panic("pmm: no region large enough for the %lu-byte bitmap", bm_size);
    bitmap = phys_to_virt(bm_phys);

    for (uint64_t i = 0; i < words; i++) bitmap[i] = ~0ull;   // everything used by default

    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        uint64_t first = (e->base + PAGE_SIZE - 1) / PAGE_SIZE;   // only whole pages
        uint64_t last = (e->base + e->length) / PAGE_SIZE;
        for (uint64_t pg = first; pg < last; pg++) { bit_clear(pg); free_pages++; }
    }

    // Reserve the bitmap itself and physical page 0.
    for (uint64_t pg = bm_phys / PAGE_SIZE; pg < (bm_phys + bm_size) / PAGE_SIZE; pg++)
        if (!bit_test(pg)) { bit_set(pg); free_pages--; }
    if (!bit_test(0)) { bit_set(0); free_pages--; }
    next_hint = 0;
}

uint64_t pmm_alloc_page(void) {
    uint64_t words = (bitmap_pages + 63) / 64;
    for (uint64_t n = 0; n < words; n++) {
        uint64_t w = (next_hint + n) % words;
        if (bitmap[w] == ~0ull) continue;
        uint64_t bit = (uint64_t)__builtin_ctzll(~bitmap[w]);
        uint64_t pg = w * 64 + bit;
        if (pg >= bitmap_pages) continue;
        bit_set(pg);
        free_pages--;
        next_hint = w;
        uint64_t phys = pg * PAGE_SIZE;
        mm_zero_page(phys_to_virt(phys));
        return phys;
    }
    return 0;
}

void pmm_free_page(uint64_t phys) {
    if (phys & (PAGE_SIZE - 1)) panic("pmm_free_page: unaligned address %p", (void *)phys);
    uint64_t pg = phys / PAGE_SIZE;
    if (pg == 0 || pg >= bitmap_pages) panic("pmm_free_page: bad address %p", (void *)phys);
    if (!bit_test(pg)) panic("pmm_free_page: double free of %p", (void *)phys);
    bit_clear(pg);
    free_pages++;
    if (pg / 64 < next_hint) next_hint = pg / 64;
}

uint64_t pmm_total_bytes(void) { return total_bytes; }
uint64_t pmm_free_bytes(void) { return free_pages * PAGE_SIZE; }
