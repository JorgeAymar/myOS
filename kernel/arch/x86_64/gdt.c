// GDT with kernel/user segments and a TSS (IST stacks for #DF and NMI).
#include <stdint.h>
#include "interrupt.h"

struct __attribute__((packed)) tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
};

struct __attribute__((packed)) gdtr {
    uint16_t limit;
    uint64_t base;
};

#define STACK_SIZE 16384
static uint8_t df_stack[STACK_SIZE] __attribute__((aligned(16)));
static uint8_t nmi_stack[STACK_SIZE] __attribute__((aligned(16)));
static uint8_t rsp0_stack[STACK_SIZE] __attribute__((aligned(16)));

static struct tss tss;
// null, kcode, kdata, udata, ucode, tss (16-byte descriptor = 2 slots)
static uint64_t gdt[7] __attribute__((aligned(16)));

static uint64_t seg_desc(uint8_t access, uint8_t flags) {
    // base 0, limit 0xFFFFF (ignored in long mode for code/data)
    return 0x000000000000FFFFull
         | ((uint64_t)access << 40)
         | ((uint64_t)(0xF | (flags << 4)) << 48);
}

void gdt_init(void) {
    gdt[0] = 0;
    gdt[1] = seg_desc(0x9A, 0xA);   // P, DPL0, code, exec/read; G + L
    gdt[2] = seg_desc(0x92, 0xC);   // P, DPL0, data, read/write; G + DB
    gdt[3] = seg_desc(0xF2, 0xC);   // P, DPL3, data
    gdt[4] = seg_desc(0xFA, 0xA);   // P, DPL3, code64

    tss.rsp[0] = (uint64_t)(rsp0_stack + STACK_SIZE);
    tss.ist[IST_DOUBLE_FAULT - 1] = (uint64_t)(df_stack + STACK_SIZE);
    tss.ist[IST_NMI - 1] = (uint64_t)(nmi_stack + STACK_SIZE);
    tss.iomap_base = sizeof(struct tss);   // no I/O permission bitmap

    uint64_t base = (uint64_t)&tss;
    uint64_t limit = sizeof(struct tss) - 1;
    gdt[5] = (limit & 0xFFFF)
           | ((base & 0xFFFFFF) << 16)
           | (0x89ull << 40)                  // P, DPL0, 64-bit TSS (available)
           | (((limit >> 16) & 0xF) << 48)
           | (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;

    struct gdtr gdtr = { .limit = sizeof(gdt) - 1, .base = (uint64_t)gdt };
    __asm__ volatile (
        "lgdt %0\n\t"
        "mov %[ds], %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%ss\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "pushq %[cs]\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "mov %[tss], %%ax\n\t"
        "ltr %%ax\n\t"
        :
        : "m"(gdtr), [ds] "i"(GDT_KERNEL_DATA), [cs] "i"(GDT_KERNEL_CODE),
          [tss] "i"(GDT_TSS)
        : "rax", "memory");
}
