// 8259A PIC pair: remapped to vectors 32-47, everything masked except cascade.
#include <stdint.h>
#include <stdbool.h>
#include "io.h"
#include "interrupt.h"
#include "cpu.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20
#define PIC_READ_ISR 0x0B

#define IRQ_BASE 32

void pic_init(void) {
    outb(PIC1_CMD, 0x11);  io_wait();      // ICW1: init, expect ICW4
    outb(PIC2_CMD, 0x11);  io_wait();
    outb(PIC1_DATA, IRQ_BASE);      io_wait();   // ICW2: vector offsets
    outb(PIC2_DATA, IRQ_BASE + 8);  io_wait();
    outb(PIC1_DATA, 0x04); io_wait();      // ICW3: slave on IRQ2
    outb(PIC2_DATA, 0x02); io_wait();      // ICW3: slave identity 2
    outb(PIC1_DATA, 0x01); io_wait();      // ICW4: 8086 mode
    outb(PIC2_DATA, 0x01); io_wait();

    outb(PIC1_DATA, 0xFB);                 // mask all but IRQ2 (cascade)
    outb(PIC2_DATA, 0xFF);
}

void pic_unmask(uint8_t irq) {
    if (irq < 8) {
        outb(PIC1_DATA, inb(PIC1_DATA) & ~(1u << irq));
    } else {
        outb(PIC2_DATA, inb(PIC2_DATA) & ~(1u << (irq - 8)));
        outb(PIC1_DATA, inb(PIC1_DATA) & ~(1u << 2));
    }
}

void pic_mask(uint8_t irq) {
    if (irq == 2) return;                  // never mask the cascade
    if (irq < 8) outb(PIC1_DATA, inb(PIC1_DATA) | (1u << irq));
    else outb(PIC2_DATA, inb(PIC2_DATA) | (1u << (irq - 8)));
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

static uint8_t read_isr(uint16_t cmd_port) {
    outb(cmd_port, PIC_READ_ISR);
    return inb(cmd_port);
}

bool pic_is_spurious(uint8_t irq) {
    if (irq == 7) {
        // Spurious from master: ISR bit clear, no EOI must be sent.
        return !(read_isr(PIC1_CMD) & 0x80);
    }
    if (irq == 15) {
        // Spurious from slave: master still saw a real IRQ2, so EOI master only.
        if (!(read_isr(PIC2_CMD) & 0x80)) {
            outb(PIC1_CMD, PIC_EOI);
            return true;
        }
    }
    return false;
}
