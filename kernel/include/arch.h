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

// ---- hooks for the scheduler (kernel/proc) ----
// Called at the end of every hardware IRQ, after the handler ran and EOI was
// sent, with interrupts disabled and on the interrupted context's stack. The
// hook may context-switch (the interrupted thread resumes later through the
// normal iretq path). Used for preemption on IRQ0 and deferred reschedules.
typedef void (*irq_exit_hook_t)(uint8_t irq);
void arch_set_irq_exit_hook(irq_exit_hook_t hook);
// When set, timer_sleep_ms(ms) calls hook(ms) instead of its sti;hlt loop
// (lets the scheduler run other threads while the caller sleeps).
typedef void (*sleep_hook_t)(uint64_t ms);
void timer_set_sleep_hook(sleep_hook_t hook);
// Kernel stack loaded by the CPU on a ring 3 -> ring 0 transition (TSS.rsp0).
void arch_set_kernel_stack(uint64_t rsp0);

// ---- userspace support (kernel/proc) ----
// Enables SYSCALL/SYSRET (EFER.SCE, STAR, LSTAR=entry, FMASK clears IF/DF/TF/AC).
// GDT selectors for ring 3: user data 0x1b, user code 0x23.
#define ARCH_USER_CS 0x23
#define ARCH_USER_SS 0x1b
void arch_enable_syscall(void (*entry)(void));
// Called for a CPU exception raised in ring 3 (interrupts disabled, on the
// thread's kernel stack). The hook normally kills the faulting thread and does
// not return; if it returns, the exception is treated as fatal (panic).
typedef void (*user_fault_hook_t)(uint64_t vector, uint64_t rip, uint64_t error_code);
void arch_set_user_fault_hook(user_fault_hook_t hook);

static inline void cpu_hlt(void) { __asm__ volatile ("hlt"); }
static inline void cpu_cli(void) { __asm__ volatile ("cli" ::: "memory"); }
static inline void cpu_sti(void) { __asm__ volatile ("sti" ::: "memory"); }
