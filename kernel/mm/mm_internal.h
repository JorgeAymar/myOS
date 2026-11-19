#pragma once
// Private interface between the files of kernel/mm/. Not part of the public mm.h contract.
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Kernel heap virtual layout (PML4 slot 384, far from HHDM at slot 256+ and the kernel at slot 511).
#define HEAP_SMALL_BASE  0xffffc00000000000ull                 // first-fit arena, grows up
#define HEAP_SMALL_LIMIT (HEAP_SMALL_BASE + (64ull << 30))      // 64 GiB
#define HEAP_LARGE_BASE  HEAP_SMALL_LIMIT                       // page-granular big allocations
#define HEAP_LARGE_LIMIT (HEAP_LARGE_BASE + (1ull << 40))       // 1 TiB
#define MM_TEST_VIRT     (HEAP_LARGE_LIMIT + 0x1000)            // scratch page for self-tests

#define PAGE_MASK_ADDR 0x000ffffffffff000ull

void pmm_init(void);
void vmm_init(void);
void heap_init(void);

void mm_zero_page(void *virt);
uint64_t heap_small_end(void);     // current top of the first-fit arena (for tests)
size_t heap_small_blocks(size_t *free_count);
