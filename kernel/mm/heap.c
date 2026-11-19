// Kernel heap.
//  * Small/medium allocations (< LARGE_THRESHOLD): first-fit over an address-ordered, doubly linked
//    list of contiguous blocks in [HEAP_SMALL_BASE, small_end). Blocks are split on allocation and
//    coalesced with free neighbours on free. The arena grows on demand by mapping PMM pages.
//  * Large allocations: their own page-granular mapping in [HEAP_LARGE_BASE, HEAP_LARGE_LIMIT),
//    pages returned to the PMM on free. Virtual space there is bump-allocated (1 TiB, never reused).
// Every block starts with a 48-byte header carrying a magic so bad pointers and double frees panic.
#include "mm.h"
#include "mm_internal.h"
#include "panic.h"
#include "string.h"

#define MAGIC_USED  0x55534544u   // "USED"
#define MAGIC_FREE  0x46524545u   // "FREE"
#define MAGIC_LARGE 0x4c415247u   // "LARG"

#define LARGE_THRESHOLD (256u * 1024)
#define GROW_MIN        (64u * 1024)
#define MIN_SPLIT       64          // smallest remainder worth splitting off (header + 16)

struct block {
    uint32_t magic;
    uint32_t _pad;
    uint64_t size;            // total bytes incl. header (small) / mapped bytes (large)
    struct block *prev, *next;
    uint64_t req;             // requested size (diagnostics)
    uint64_t _pad2;
};
_Static_assert(sizeof(struct block) == 48, "heap header must stay 16-byte aligned");
#define HDR sizeof(struct block)

static struct block *head, *tail;
static uint64_t small_end = HEAP_SMALL_BASE;
static uint64_t large_next = HEAP_LARGE_BASE;

uint64_t heap_small_end(void) { return small_end; }

static inline size_t align_up(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

static void corrupt(const struct block *b, const char *what) {
    panic("heap: corrupted block header at %p (%s, magic=%x)", (const void *)b, what, b->magic);
}

void heap_init(void) {
    if (vmm_translate(HEAP_SMALL_BASE) || vmm_translate(HEAP_LARGE_BASE))
        panic("heap: virtual range %p already mapped", (void *)HEAP_SMALL_BASE);
    head = tail = NULL;
    small_end = HEAP_SMALL_BASE;
    large_next = HEAP_LARGE_BASE;
}

// Maps `bytes` (page multiple) of fresh pages at virt. On failure unmaps what it mapped.
static bool map_range(uint64_t virt, uint64_t bytes) {
    for (uint64_t off = 0; off < bytes; off += PAGE_SIZE) {
        uint64_t p = pmm_alloc_page();
        if (!p || !vmm_map(virt + off, p, VMM_PRESENT | VMM_WRITE | VMM_NX)) {
            if (p) pmm_free_page(p);
            for (uint64_t o = 0; o < off; o += PAGE_SIZE) {
                uint64_t q = vmm_translate(virt + o);
                vmm_unmap(virt + o);
                pmm_free_page(q);
            }
            return false;
        }
    }
    return true;
}

static void unmap_range(uint64_t virt, uint64_t bytes) {
    for (uint64_t off = 0; off < bytes; off += PAGE_SIZE) {
        uint64_t p = vmm_translate(virt + off);
        vmm_unmap(virt + off);
        if (p) pmm_free_page(p);
    }
}

// Extends the arena so that the tail block is free and at least `need` bytes.
static bool grow(size_t need) {
    size_t have = (tail && tail->magic == MAGIC_FREE) ? tail->size : 0;
    uint64_t bytes = align_up(need - have, PAGE_SIZE);
    if (bytes < GROW_MIN) bytes = GROW_MIN;
    if (small_end + bytes > HEAP_SMALL_LIMIT) return false;
    if (!map_range(small_end, bytes)) return false;

    if (have) {
        tail->size += bytes;
    } else {
        struct block *b = (struct block *)small_end;
        *b = (struct block){ .magic = MAGIC_FREE, .size = bytes, .prev = tail };
        if (tail) tail->next = b; else head = b;
        tail = b;
    }
    small_end += bytes;
    return true;
}

static void *small_alloc(size_t size) {
    size_t need = align_up(size, 16) + HDR;
    for (int attempt = 0; attempt < 2; attempt++) {
        for (struct block *b = head; b; b = b->next) {
            if (b->magic == MAGIC_USED) continue;
            if (b->magic != MAGIC_FREE) corrupt(b, "walk");
            if (b->size < need) continue;
            if (b->size - need >= MIN_SPLIT) {
                struct block *r = (struct block *)((uint8_t *)b + need);
                *r = (struct block){ .magic = MAGIC_FREE, .size = b->size - need, .prev = b, .next = b->next };
                if (b->next) b->next->prev = r; else tail = r;
                b->next = r;
                b->size = need;
            }
            b->magic = MAGIC_USED;
            b->req = size;
            return (uint8_t *)b + HDR;
        }
        if (attempt == 0 && !grow(need)) return NULL;
    }
    return NULL;
}

static void *large_alloc(size_t size) {
    uint64_t bytes = align_up(size + HDR, PAGE_SIZE);
    if (large_next + bytes + PAGE_SIZE > HEAP_LARGE_LIMIT) return NULL;
    uint64_t va = large_next;
    if (!map_range(va, bytes)) return NULL;
    large_next += bytes + PAGE_SIZE;      // leave an unmapped guard page after each allocation
    struct block *b = (struct block *)va;
    *b = (struct block){ .magic = MAGIC_LARGE, .size = bytes, .req = size };
    return (uint8_t *)b + HDR;
}

static struct block *checked_header(void *ptr, const char *who) {
    uint64_t p = (uint64_t)ptr;
    bool small = p >= HEAP_SMALL_BASE + HDR && p < small_end;
    bool large = p >= HEAP_LARGE_BASE + HDR && p < large_next;
    if ((!small && !large) || (p & 15))
        panic("%s: pointer %p was not returned by kmalloc", who, ptr);
    struct block *b = (struct block *)(p - HDR);
    if (large) {
        if (((uint64_t)b & (PAGE_SIZE - 1)) || !vmm_translate((uint64_t)b))
            panic("%s: %p is not a live large allocation (double free?)", who, ptr);
        if (b->magic != MAGIC_LARGE) panic("%s: bad large header at %p (magic=%x)", who, ptr, b->magic);
        return b;
    }
    if (b->magic == MAGIC_FREE) panic("%s: double free of %p", who, ptr);
    if (b->magic != MAGIC_USED) panic("%s: bad pointer %p (magic=%x)", who, ptr, b->magic);
    if (b->next && b->next != (struct block *)((uint8_t *)b + b->size))
        panic("%s: heap block %p has inconsistent size/link", who, ptr);
    return b;
}

void *kmalloc(size_t size) {
    if (size == 0 || size > (1ull << 40)) return NULL;
    return size >= LARGE_THRESHOLD ? large_alloc(size) : small_alloc(size);
}

void *kcalloc(size_t n, size_t size) {
    if (size && n > (size_t)-1 / size) return NULL;
    size_t total = n * size;
    void *p = kmalloc(total);
    if (p) memset(p, 0, total);   // large blocks are already zero, small may be recycled
    return p;
}

static size_t capacity(const struct block *b) { return b->size - HDR; }

void kfree(void *ptr) {
    if (!ptr) return;
    struct block *b = checked_header(ptr, "kfree");
    if (b->magic == MAGIC_LARGE) {
        b->magic = 0;
        unmap_range((uint64_t)b, b->size);
        return;
    }
    b->magic = MAGIC_FREE;
    struct block *n = b->next;
    if (n && n->magic == MAGIC_FREE) {
        b->size += n->size;
        b->next = n->next;
        if (n->next) n->next->prev = b; else tail = b;
        n->magic = 0;
    } else if (n && n->magic != MAGIC_USED) {
        corrupt(n, "next");
    }
    struct block *p = b->prev;
    if (p && p->magic == MAGIC_FREE) {
        p->size += b->size;
        p->next = b->next;
        if (b->next) b->next->prev = p; else tail = p;
        b->magic = 0;
    } else if (p && p->magic != MAGIC_USED) {
        corrupt(p, "prev");
    }
}

void *krealloc(void *ptr, size_t size) {
    if (!ptr) return kmalloc(size);
    if (size == 0) { kfree(ptr); return NULL; }
    struct block *b = checked_header(ptr, "krealloc");
    size_t cap = capacity(b);
    if (size <= cap) { b->req = size; return ptr; }
    void *n = kmalloc(size);
    if (!n) return NULL;
    memcpy(n, ptr, cap);
    kfree(ptr);
    return n;
}

// Number of blocks in the first-fit arena, and how many of them are free (for self-tests).
size_t heap_small_blocks(size_t *free_count) {
    size_t n = 0, f = 0;
    for (struct block *b = head; b; b = b->next) {
        n++;
        if (b->magic == MAGIC_FREE) f++;
    }
    if (free_count) *free_count = f;
    return n;
}
