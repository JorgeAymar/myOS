// COM1 serial driver (16550 UART). Primary debug/test output channel.
#include "serial.h"
#include "io.h"

#define COM1 0x3f8

void serial_init(void) {
    outb(COM1 + 1, 0x00); // disable interrupts
    outb(COM1 + 3, 0x80); // enable DLAB
    outb(COM1 + 0, 0x01); // divisor 1 -> 115200 baud
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); // 8N1
    outb(COM1 + 2, 0xc7); // FIFO
    outb(COM1 + 4, 0x0b); // RTS/DSR
}

void serial_putc(char c) {
    if (c == '\n') serial_putc('\r');
    while ((inb(COM1 + 5) & 0x20) == 0) { }
    outb(COM1, (uint8_t)c);
}

void serial_puts(const char *s) {
    while (*s) serial_putc(*s++);
}

void serial_putdec(uint64_t v) {
    char buf[21];
    int i = 0;
    do { buf[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) serial_putc(buf[--i]);
}

void serial_puthex(uint64_t v) {
    serial_puts("0x");
    for (int s = 60; s >= 0; s -= 4)
        serial_putc("0123456789abcdef"[(v >> s) & 0xf]);
}
