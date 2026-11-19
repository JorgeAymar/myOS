#include "panic.h"
#include "kprintf.h"
#include "serial.h"
#include "io.h"

void qemu_exit(uint8_t code) { outb(0xf4, code); }

void halt_forever(void) {
    for (;;) __asm__ volatile ("cli; hlt");
}

void panic(const char *fmt, ...) {
    __asm__ volatile ("cli");
    char buf[512];
    va_list ap; va_start(ap, fmt);
    kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    serial_puts("[FAIL] ");
    serial_puts(buf);
    serial_puts("\n");
    qemu_exit(QEMU_EXIT_FAILURE);
    halt_forever();
}
