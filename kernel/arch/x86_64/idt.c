// IDT setup, C interrupt dispatcher and exception register dumps.
#include <stdint.h>
#include <stddef.h>
#include "arch.h"
#include "kprintf.h"
#include "panic.h"
#include "interrupt.h"
#include "cpu.h"

struct __attribute__((packed)) idt_entry {
    uint16_t offset_lo;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_hi;
    uint32_t reserved;
};

struct __attribute__((packed)) idtr {
    uint16_t limit;
    uint64_t base;
};

extern const uint64_t isr_stub_table[256];

static struct idt_entry idt[256] __attribute__((aligned(16)));
static irq_handler_t irq_handlers[16];

static const char *const exception_names[32] = {
    "Divide Error (#DE)", "Debug (#DB)", "Non-Maskable Interrupt (NMI)",
    "Breakpoint (#BP)", "Overflow (#OF)", "BOUND Range Exceeded (#BR)",
    "Invalid Opcode (#UD)", "Device Not Available (#NM)", "Double Fault (#DF)",
    "Coprocessor Segment Overrun", "Invalid TSS (#TS)", "Segment Not Present (#NP)",
    "Stack-Segment Fault (#SS)", "General Protection Fault (#GP)", "Page Fault (#PF)",
    "Reserved (15)", "x87 Floating-Point (#MF)", "Alignment Check (#AC)",
    "Machine Check (#MC)", "SIMD Floating-Point (#XM)", "Virtualization (#VE)",
    "Control Protection (#CP)", "Reserved (22)", "Reserved (23)", "Reserved (24)",
    "Reserved (25)", "Reserved (26)", "Reserved (27)", "Hypervisor Injection (#HV)",
    "VMM Communication (#VC)", "Security Exception (#SX)", "Reserved (31)",
};

static void idt_set(int vec, uint64_t handler, uint8_t ist, uint8_t type_attr) {
    idt[vec] = (struct idt_entry){
        .offset_lo  = handler & 0xFFFF,
        .selector   = GDT_KERNEL_CODE,
        .ist        = ist & 7,
        .type_attr  = type_attr,
        .offset_mid = (handler >> 16) & 0xFFFF,
        .offset_hi  = (uint32_t)(handler >> 32),
        .reserved   = 0,
    };
}

void idt_init(void) {
    for (int v = 0; v < 256; v++) {
        uint8_t ist = 0;
        if (v == 8) ist = IST_DOUBLE_FAULT;
        if (v == 2) ist = IST_NMI;
        // 0x8E: present, DPL0, 64-bit interrupt gate. #BP gets DPL3 (0xEE) so
        // a future user-mode int3 traps instead of raising #GP.
        idt_set(v, isr_stub_table[v], ist, v == 3 ? 0xEE : 0x8E);
    }
    struct idtr idtr = { .limit = sizeof(idt) - 1, .base = (uint64_t)idt };
    __asm__ volatile ("lidt %0" :: "m"(idtr) : "memory");
}

void irq_register(uint8_t irq, irq_handler_t handler) {
    if (irq >= 16) panic("irq_register: bad irq %u", irq);
    irq_handlers[irq] = handler;
    if (handler) pic_unmask(irq);
    else pic_mask(irq);
}

static void dump_exception(const struct interrupt_frame *f) {
    uint64_t vec = f->vector;
    kprintf("\n[arch] *** CPU EXCEPTION %lu: %s ***\n", vec, exception_names[vec]);
    kprintf("  error=0x%016lx  RIP=%04lx:%016lx  RFLAGS=0x%016lx\n",
            f->error_code, f->cs, f->rip, f->rflags);
    kprintf("  RSP=%04lx:%016lx\n", f->ss, f->rsp);
    kprintf("  RAX=%016lx RBX=%016lx RCX=%016lx RDX=%016lx\n", f->rax, f->rbx, f->rcx, f->rdx);
    kprintf("  RSI=%016lx RDI=%016lx RBP=%016lx\n", f->rsi, f->rdi, f->rbp);
    kprintf("  R8 =%016lx R9 =%016lx R10=%016lx R11=%016lx\n", f->r8, f->r9, f->r10, f->r11);
    kprintf("  R12=%016lx R13=%016lx R14=%016lx R15=%016lx\n", f->r12, f->r13, f->r14, f->r15);
    if (vec == 14) {
        uint64_t e = f->error_code;
        kprintf("  CR2=%016lx  [%s, %s, %s%s%s%s%s]\n", read_cr2(),
                (e & 1) ? "protection violation" : "page not present",
                (e & 2) ? "write" : "read",
                (e & 4) ? "user" : "kernel",
                (e & 8) ? ", reserved bit set" : "",
                (e & 16) ? ", instruction fetch" : "",
                (e & 32) ? ", protection key" : "",
                (e & 0x8000) ? ", SGX" : "");
    }
    if (vec == 10 || vec == 11 || vec == 12 || vec == 13) {
        uint64_t e = f->error_code;
        if (e) kprintf("  selector: index=%lu table=%s%s\n", e >> 3,
                       (e & 2) ? "IDT" : ((e & 4) ? "LDT" : "GDT"),
                       (e & 1) ? " (external)" : "");
    }
    kprintf("  CR0=%016lx CR3=%016lx CR4=%016lx\n", read_cr0(), read_cr3(), read_cr4());
}

void interrupt_dispatch(struct interrupt_frame *f) {
    uint64_t vec = f->vector;

    if (vec < 32) {
        if (vec == 3) {
            // int3 is a trap: RIP already points past the instruction.
            kprintf("[arch] breakpoint at RIP=0x%016lx\n", f->rip);
            return;
        }
        dump_exception(f);
        panic("CPU exception %lu (%s) at RIP=0x%016lx",
              vec, exception_names[vec], f->rip);
    }

    if (vec < 48) {
        uint8_t irq = (uint8_t)(vec - 32);
        if (pic_is_spurious(irq)) return;
        if (irq_handlers[irq]) irq_handlers[irq]();
        pic_send_eoi(irq);
        return;
    }

    kprintf("[arch] unexpected interrupt vector %lu at RIP=0x%016lx\n", vec, f->rip);
}
