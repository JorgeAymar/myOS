// arch_init: bring up GDT/TSS, IDT, PIC, PIT and run boot self-tests.
#include <stdint.h>
#include "arch.h"
#include "kprintf.h"
#include "panic.h"
#include "interrupt.h"
#include "cpu.h"

static void selftest_breakpoint(void) {
    volatile uint64_t marker = 0;
    __asm__ volatile ("int3" ::: "memory");
    marker = 0x1234;                          // reached only if int3 returned
    if (marker != 0x1234) panic("arch selftest: int3 did not return");
}

static void selftest_timer(void) {
    if (!(read_rflags() & (1u << 9))) panic("arch selftest: IF not set after sti");

    // Wait for the first tick with a bounded spin so a dead timer panics
    // instead of hanging in hlt forever.
    uint64_t t0 = timer_ticks_ms();
    uint64_t spins = 0;
    while (timer_ticks_ms() == t0) {
        __asm__ volatile ("pause");
        if (++spins > 500000000ull) panic("arch selftest: PIT timer is not ticking");
    }

    uint64_t start = timer_ticks_ms();
    timer_sleep_ms(20);
    uint64_t elapsed = timer_ticks_ms() - start;
    if (elapsed < 20) panic("arch selftest: timer_sleep_ms(20) took only %lu ms", elapsed);
}

void arch_init(void) {
    cpu_cli();
    gdt_init();
    idt_init();
    pic_init();
    pit_init();
    cpu_sti();

    selftest_breakpoint();
    selftest_timer();
    kprintf("[OK] arch gdt/idt/pic/pit\n");}
