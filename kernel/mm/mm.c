// mm_init: brings up PMM, VMM and heap, then runs boot self-tests.
#include "mm.h"
#include "mm_internal.h"
#include "panic.h"
#include "kprintf.h"
#include "string.h"

#define MIB ((uint64_t)1024 * 1024)

static uint32_t rng = 0x12345678;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

static void test_pmm(void) {
    enum { N = 512 };
    static uint64_t pages[N];
    uint64_t free0 = pmm_free_bytes();
    for (int i = 0; i < N; i++) {
        pages[i] = pmm_alloc_page();
        if (!pages[i]) panic("mm test: pmm_alloc_page returned 0 (iteration %d)", i);
        if (pages[i] & (PAGE_SIZE - 1)) panic("mm test: unaligned page %p", (void *)pages[i]);
        uint64_t *v = phys_to_virt(pages[i]);
        for (int k = 0; k < 512; k++) if (v[k]) panic("mm test: page %p not zeroed", (void *)pages[i]);
        v[0] = pages[i] ^ 0xa5a5a5a5a5a5a5a5ull;
    }
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < i; j++)
            if (pages[i] == pages[j]) panic("mm test: page %p handed out twice", (void *)pages[i]);
        if (*(uint64_t *)phys_to_virt(pages[i]) != (pages[i] ^ 0xa5a5a5a5a5a5a5a5ull))
            panic("mm test: page %p content clobbered", (void *)pages[i]);
    }
    if (pmm_free_bytes() != free0 - N * PAGE_SIZE) panic("mm test: pmm free count mismatch");
    uint64_t lowest = pages[0];
    for (int i = 0; i < N; i++) if (pages[i] < lowest) lowest = pages[i];
    for (int i = N - 1; i >= 0; i--) pmm_free_page(pages[i]);
    if (pmm_free_bytes() != free0) panic("mm test: pmm free count not restored");
    uint64_t again = pmm_alloc_page();
    if (again != lowest) panic("mm test: freed page not reused (%p vs %p)", (void *)again, (void *)lowest);
    pmm_free_page(again);
}

static void test_vmm(void) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) panic("mm test: out of memory in vmm test");
    if (vmm_translate(MM_TEST_VIRT)) panic("mm test: test address already mapped");
    if (!vmm_map(MM_TEST_VIRT, phys, VMM_PRESENT | VMM_WRITE | VMM_NX)) panic("mm test: vmm_map failed");
    if (vmm_translate(MM_TEST_VIRT + 0x123) != phys + 0x123) panic("mm test: vmm_translate mismatch");
    volatile uint64_t *v = (volatile uint64_t *)MM_TEST_VIRT;
    v[7] = 0xdeadbeefcafef00dull;
    if (((volatile uint64_t *)phys_to_virt(phys))[7] != 0xdeadbeefcafef00dull)
        panic("mm test: write through mapping not visible via HHDM");
    vmm_unmap(MM_TEST_VIRT);
    if (vmm_translate(MM_TEST_VIRT)) panic("mm test: page still mapped after vmm_unmap");
    pmm_free_page(phys);

    // The HHDM and kernel image (possibly huge-page mapped) must translate.
    uint64_t k = (uint64_t)&test_vmm;
    if (!vmm_translate(k)) panic("mm test: kernel text does not translate");
    if (vmm_translate((uint64_t)phys_to_virt(phys)) != phys) panic("mm test: HHDM translate mismatch");
}

static void fill(uint8_t *p, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(seed + i * 7);
}
static bool check(const uint8_t *p, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; i++) if (p[i] != (uint8_t)(seed + i * 7)) return false;
    return true;
}

static void test_heap(void) {
    enum { N = 160 };
    static uint8_t *ptr[N];
    static size_t len[N];
    uint64_t free0 = pmm_free_bytes();

    for (int i = 0; i < N; i++) {
        size_t s;
        if (i == N / 2) s = 8 * MIB;
        else if (i % 37 == 0) s = 300 * 1024 + rnd() % 65536;     // large path
        else if (i % 5 == 0) s = 1024 + rnd() % (64 * 1024);
        else s = 1 + rnd() % 512;
        ptr[i] = kmalloc(s);
        len[i] = s;
        if (!ptr[i]) panic("mm test: kmalloc(%lu) failed", s);
        if ((uint64_t)ptr[i] & 15) panic("mm test: kmalloc returned unaligned %p", (void *)ptr[i]);
        fill(ptr[i], s, (uint8_t)i);
    }
    for (int i = 0; i < N; i++) {
        if (!check(ptr[i], len[i], (uint8_t)i)) panic("mm test: heap block %d corrupted", i);
        for (int j = 0; j < i; j++) {
            uint64_t a = (uint64_t)ptr[i], b = (uint64_t)ptr[j];
            if (a < b + len[j] && b < a + len[i]) panic("mm test: heap blocks %d and %d overlap", i, j);
        }
    }
    // Free in a scrambled order (stride coprime with N).
    for (int k = 0; k < N; k++) {
        int i = (k * 67 + 11) % N;
        kfree(ptr[i]);
        ptr[i] = NULL;
    }
    size_t nfree, nblocks = heap_small_blocks(&nfree);
    if (nblocks != 1 || nfree != 1)
        panic("mm test: heap did not coalesce (%lu blocks, %lu free)", nblocks, nfree);

    // Coalesced arena satisfies a big small-path request without growing.
    uint64_t end = heap_small_end();
    size_t arena = end - HEAP_SMALL_BASE;
    size_t want = arena > 200 * 1024 ? 200 * 1024 : arena / 2;
    void *big = kmalloc(want);
    if (!big || heap_small_end() != end) panic("mm test: coalesced arena not reused");
    kfree(big);

    // Large blocks give memory back; a second 8 MiB allocation succeeds.
    void *b8 = kmalloc(8 * MIB);
    if (!b8) panic("mm test: second 8 MiB kmalloc failed");
    fill(b8, 8 * MIB, 3);
    if (!check(b8, 8 * MIB, 3)) panic("mm test: 8 MiB block corrupted");
    kfree(b8);
    // Only page tables and the (retained) small arena may remain allocated.
    if (free0 - pmm_free_bytes() > arena + 64 * PAGE_SIZE)
        panic("mm test: large allocations leaked memory");

    // krealloc preserves data across growth (small -> small, small -> large) and shrink.
    uint8_t *r = kmalloc(100);
    fill(r, 100, 9);
    r = krealloc(r, 5000);
    if (!r || !check(r, 100, 9)) panic("mm test: krealloc small->small lost data");
    fill(r, 5000, 9);
    r = krealloc(r, 1 * MIB);
    if (!r || !check(r, 5000, 9)) panic("mm test: krealloc small->large lost data");
    r = krealloc(r, 64);
    if (!r || !check(r, 64, 9)) panic("mm test: krealloc shrink lost data");
    kfree(r);

    uint32_t *z = kcalloc(1000, sizeof(uint32_t));
    if (!z) panic("mm test: kcalloc failed");
    for (int i = 0; i < 1000; i++) if (z[i]) panic("mm test: kcalloc memory not zeroed");
    kfree(z);
    if (kmalloc(0) != NULL) panic("mm test: kmalloc(0) must return NULL");
    if (kcalloc((size_t)1 << 40, (size_t)1 << 40) != NULL) panic("mm test: kcalloc overflow not caught");
}

void mm_init(void) {
    pmm_init();
    vmm_init();
    heap_init();

    test_pmm();
    test_vmm();
    kprintf("[OK] mm total=%lu MiB free=%lu MiB\n", pmm_total_bytes() / MIB, pmm_free_bytes() / MIB);
    test_heap();
    kprintf("[OK] heap range=%p..%p\n", (void *)HEAP_SMALL_BASE, (void *)HEAP_LARGE_LIMIT);
}
