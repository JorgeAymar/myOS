#pragma once
// Internal interfaces of the x86_64 arch layer.
#include <stdint.h>
#include <stdbool.h>

// Layout must match the push order in isr.asm (lowest address first).
struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    // Pushed by the CPU
    uint64_t rip, cs, rflags, rsp, ss;
};

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x18
#define GDT_USER_CODE   0x20
#define GDT_TSS         0x28

#define IST_DOUBLE_FAULT 1
#define IST_NMI          2

void gdt_init(void);
void idt_init(void);
void pic_init(void);
void pic_send_eoi(uint8_t irq);
void pic_unmask(uint8_t irq);
void pic_mask(uint8_t irq);
// True if the IRQ is spurious; any EOI it still needs has been sent.
bool pic_is_spurious(uint8_t irq);
void pit_init(void);
void interrupt_dispatch(struct interrupt_frame *f);
