#pragma once
// CPU setup, interrupts and timer. Owner: arch agent (kernel/arch/x86_64/).
#include <stdint.h>
#include <stdbool.h>

// Sets up GDT (+TSS), IDT with exception handlers, remaps the PIC (IRQ 0-15 ->
// vectors 32-47), starts the PIT at 1000 Hz and enables interrupts (sti).
// Exceptions print a register dump via kprintf and then panic().
void arch_init(void);

typedef void (*irq_handler_t)(void);
// Registers a handler for a legacy IRQ line (0-15) and unmasks it. EOI is sent
// by the arch layer after the handler returns.
void irq_register(uint8_t irq, irq_handler_t handler);

uint64_t timer_ticks_ms(void);          // milliseconds since arch_init
void timer_sleep_ms(uint64_t ms);       // blocks (hlt loop) for at least ms

static inline void cpu_hlt(void) { __asm__ volatile ("hlt"); }
static inline void cpu_cli(void) { __asm__ volatile ("cli" ::: "memory"); }
static inline void cpu_sti(void) { __asm__ volatile ("sti" ::: "memory"); }
