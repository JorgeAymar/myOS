// PIT channel 0 at 1000 Hz on IRQ0: millisecond tick counter.
#include <stdint.h>
#include "arch.h"
#include "io.h"
#include "interrupt.h"

#define PIT_FREQ    1193182u
#define PIT_HZ      1000u
#define PIT_CH0     0x40
#define PIT_CMD     0x43

static volatile uint64_t ticks_ms;

static void pit_tick(void) { ticks_ms++; }

void pit_init(void) {
    uint16_t divisor = (uint16_t)((PIT_FREQ + PIT_HZ / 2) / PIT_HZ);   // 1193
    outb(PIT_CMD, 0x36);                    // ch0, lo/hi byte, mode 3, binary
    outb(PIT_CH0, divisor & 0xFF);
    outb(PIT_CH0, divisor >> 8);
    irq_register(0, pit_tick);
}

uint64_t timer_ticks_ms(void) { return ticks_ms; }

static sleep_hook_t sleep_hook;
void timer_set_sleep_hook(sleep_hook_t hook) { sleep_hook = hook; }

void timer_sleep_ms(uint64_t ms) {
    if (sleep_hook) { sleep_hook(ms); return; }
    uint64_t deadline = ticks_ms + ms;
    // "sti; hlt" back to back: no IRQ can slip in between and be missed.
    while (ticks_ms < deadline) __asm__ volatile ("sti; hlt" ::: "memory");
}
