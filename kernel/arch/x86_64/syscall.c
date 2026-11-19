// SYSCALL/SYSRET MSR setup. The entry stub itself lives with its user (kernel/proc).
#include <stdint.h>
#include "arch.h"
#include "cpu.h"
#include "interrupt.h"

#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_FMASK  0xC0000084
#define EFER_SCE   (1ull << 0)

void arch_enable_syscall(void (*entry)(void)) {
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
    // SYSCALL: CS = 0x08, SS = 0x10. SYSRET (64-bit): CS = 0x10+16 = 0x20|3,
    // SS = 0x10+8 = 0x18|3 -- matches the GDT order kdata, udata, ucode.
    wrmsr(MSR_STAR, ((uint64_t)GDT_KERNEL_DATA << 48) | ((uint64_t)GDT_KERNEL_CODE << 32));
    wrmsr(MSR_LSTAR, (uint64_t)entry);
    // Clear IF, DF, TF and AC on entry.
    wrmsr(MSR_FMASK, (1u << 9) | (1u << 10) | (1u << 8) | (1u << 18));
}
