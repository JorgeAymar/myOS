// myOS kernel entry point.
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "limine.h"
#include "io.h"
#include "serial.h"

// ---- Limine requests -------------------------------------------------------
__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

// ---- Freestanding helpers required by GCC ----------------------------------
void *memcpy(void *restrict d, const void *restrict s, size_t n) {
    uint8_t *dp = d; const uint8_t *sp = s;
    while (n--) *dp++ = *sp++;
    return d;
}
void *memset(void *d, int c, size_t n) {
    uint8_t *p = d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    uint8_t *dp = d; const uint8_t *sp = s;
    if (dp < sp) { while (n--) *dp++ = *sp++; }
    else { dp += n; sp += n; while (n--) *--dp = *--sp; }
    return d;
}
int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x < *y ? -1 : 1;
    return 0;
}

// ---- QEMU test exit ---------------------------------------------------------
// With `-device isa-debug-exit,iobase=0xf4`, writing v exits QEMU with (v<<1)|1.
// Without the device (normal `make run`) the write is ignored.
#define QEMU_EXIT_SUCCESS 0x10  // -> exit code 33
#define QEMU_EXIT_FAILURE 0x11  // -> exit code 35

static void qemu_exit(uint8_t code) { outb(0xf4, code); }

static void halt_forever(void) {
    for (;;) __asm__ volatile ("cli; hlt");
}

static void fail(const char *why) {
    serial_puts("[FAIL] ");
    serial_puts(why);
    serial_puts("\n");
    qemu_exit(QEMU_EXIT_FAILURE);
    halt_forever();
}

// ---- Entry ------------------------------------------------------------------
void kmain(void) {
    serial_init();
    serial_puts("myOS booting...\n");

    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision))
        fail("limine base revision not supported");

    if (framebuffer_request.response == NULL ||
        framebuffer_request.response->framebuffer_count < 1)
        fail("no framebuffer");

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    serial_puts("[OK] framebuffer ");
    serial_putdec(fb->width); serial_puts("x");
    serial_putdec(fb->height); serial_puts("x");
    serial_putdec(fb->bpp); serial_puts("\n");

    // Paint a vertical gradient as a visible "it booted" signal.
    volatile uint32_t *px = fb->address;
    uint64_t stride = fb->pitch / 4;
    for (uint64_t y = 0; y < fb->height; y++) {
        uint32_t t = (uint32_t)(y * 255 / fb->height);
        uint32_t color = ((0x20 + t / 4) << 16) | ((0x30 + t / 6) << 8) | (0x60 + t / 2);
        for (uint64_t x = 0; x < fb->width; x++)
            px[y * stride + x] = color;
    }

    serial_puts("[TEST] boot OK\n");
    qemu_exit(QEMU_EXIT_SUCCESS);
    halt_forever();
}
