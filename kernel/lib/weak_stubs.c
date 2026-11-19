// Weak fallback implementations of every cross-module API.
// They let each module be developed and tested in isolation: a real (strong)
// definition in its owning module always overrides the weak one at link time.
// Do NOT add real logic here.
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "arch.h"
#include "mm.h"
#include "input.h"
#include "rtc.h"
#include "desktop.h"
#include "boot.h"
#include "string.h"
#include "panic.h"
#include "subsystems.h"

#define WEAK __attribute__((weak))

// ---- arch ----
static volatile uint64_t fake_ms;
WEAK void arch_init(void) {}
WEAK void irq_register(uint8_t irq, irq_handler_t h) { (void)irq; (void)h; }
WEAK uint64_t timer_ticks_ms(void) { return fake_ms++; }
WEAK void timer_sleep_ms(uint64_t ms) {
    for (volatile uint64_t i = 0; i < ms * 20000; i++) {}
    fake_ms += ms;
}

// ---- mm (bump allocator over a static pool, no free) ----
static uint8_t weak_pool[16 * 1024 * 1024] __attribute__((aligned(16)));
static size_t weak_pool_used;
WEAK void mm_init(void) {}
WEAK uint64_t mm_hhdm_offset(void) { return boot_hhdm_offset(); }
WEAK uint64_t pmm_alloc_page(void) { return 0; }
WEAK void pmm_free_page(uint64_t p) { (void)p; }
WEAK uint64_t pmm_total_bytes(void) { return 0; }
WEAK uint64_t pmm_free_bytes(void) { return 0; }
WEAK bool vmm_map(uint64_t v, uint64_t p, uint64_t f) { (void)v; (void)p; (void)f; return false; }
WEAK void vmm_unmap(uint64_t v) { (void)v; }
WEAK uint64_t vmm_translate(uint64_t v) { (void)v; return 0; }
WEAK void *kmalloc(size_t size) {
    if (!size) return NULL;
    size = (size + 15) & ~(size_t)15;
    if (weak_pool_used + size + 16 > sizeof weak_pool) panic("weak kmalloc: pool exhausted");
    uint8_t *p = weak_pool + weak_pool_used;
    *(size_t *)p = size;                       // header with size for krealloc
    weak_pool_used += size + 16;
    return p + 16;
}
WEAK void *kcalloc(size_t n, size_t size) {
    void *p = kmalloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}
WEAK void *krealloc(void *ptr, size_t size) {
    void *n = kmalloc(size);
    if (ptr && n) {
        size_t old = *(size_t *)((uint8_t *)ptr - 16);
        memcpy(n, ptr, old < size ? old : size);
    }
    return n;
}
WEAK void kfree(void *p) { (void)p; }

// ---- input / rtc ----
WEAK void input_init(void) {}
WEAK bool input_poll(struct input_event *ev) { (void)ev; return false; }
WEAK void input_inject(const struct input_event *ev) { (void)ev; }
WEAK void rtc_read(struct rtc_time *t) {
    *t = (struct rtc_time){ .year = 2026, .month = 1, .day = 1 };
}

// ---- desktop ----
WEAK void desktop_init(void) {}
WEAK void desktop_frame(void) {}
WEAK void desktop_run(void) { halt_forever(); }

// ---- subsystems ----
WEAK void sched_init(void) {}
WEAK void fs_init(void) {}
WEAK void net_init(void) {}
