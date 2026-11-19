#pragma once
// Memory management. Owner: mm agent (kernel/mm/).
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PAGE_SIZE 4096

// Builds the physical page allocator from the Limine memory map, takes over the
// page tables (keeps Limine's higher-half kernel + HHDM mappings) and sets up
// the kernel heap. Runs its own self-tests and prints "[OK] mm ...".
void mm_init(void);

// Physical pages (addresses are PHYSICAL; use phys_to_virt to touch them).
uint64_t pmm_alloc_page(void);      // 0 on out-of-memory; page is zeroed
void pmm_free_page(uint64_t phys);
uint64_t pmm_total_bytes(void);     // usable RAM
uint64_t pmm_free_bytes(void);

static inline void *phys_to_virt(uint64_t phys);
uint64_t mm_hhdm_offset(void);
static inline void *phys_to_virt(uint64_t phys) { return (void *)(phys + mm_hhdm_offset()); }

// Virtual memory (current address space).
#define VMM_PRESENT  (1ull << 0)
#define VMM_WRITE    (1ull << 1)
#define VMM_USER     (1ull << 2)
#define VMM_NOCACHE  (1ull << 4)
#define VMM_NX       (1ull << 63)
bool vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_unmap(uint64_t virt);
uint64_t vmm_translate(uint64_t virt);   // 0 if unmapped

// Kernel heap. Returns 16-byte aligned memory; kmalloc(0) returns NULL.
void *kmalloc(size_t size);
void *kcalloc(size_t n, size_t size);
void *krealloc(void *ptr, size_t size);
void kfree(void *ptr);
