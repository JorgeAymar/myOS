#pragma once
// Private interface between the files of kernel/proc/.
#include <stdint.h>
#include <stdbool.h>
#include "proc.h"

#define STACK_CANARY      0x57ac4ca7a12ee5edull
#define STACK_CANARY_WORDS 8

struct thread {
    uint64_t rsp;                   // saved stack pointer while switched out
    tid_t tid;
    char name[THREAD_NAME_MAX];
    enum thread_state state;
    int preempt_count;
    uint64_t cpu_ticks;
    uint64_t wake_tick;             // THREAD_SLEEPING deadline
    uint8_t *stack;                 // kmalloc'd stack (NULL for thread 0)
    uint64_t kstack_top;            // TSS.rsp0 while running (0 = leave as is)
    uint64_t cr3;                   // address space (0 = kernel, leave as is)
    void *proc;                     // userspace process data (phase 9), or NULL
    struct thread *qnext;           // ready / wait / sleep / zombie list link
    struct thread *all_next, *all_prev;
    wait_queue_t joiners;
};

static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    if (f & (1u << 9)) __asm__ volatile ("sti" ::: "memory");
}

// Scheduler core (sched.c). Call with interrupts disabled.
extern struct thread *sched_current;
void sched_make_ready(struct thread *t);
void sched_block(void);             // current->state already set; switch away
void sched_selftest(void);
int sched_live_stacks(void);
