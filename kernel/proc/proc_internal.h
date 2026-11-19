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

// ---- userspace (process.c, uvm.c, syscall.c, user_entry.asm) ----
#define USER_TOP 0x0000800000000000ull      // first non-user (non-canonical) address
uint64_t sched_kernel_cr3(void);
void process_init(void);
void process_free(struct thread *t);
__attribute__((noreturn)) void process_exit(int code);
void user_selftest(void);

void uvm_init(uint64_t kernel_cr3);
uint64_t uvm_create(void);
void uvm_sync_kernel(uint64_t pml4);
bool uvm_map(uint64_t pml4, uint64_t va, uint64_t phys, bool writable, bool exec);
void *uvm_alloc(uint64_t pml4, uint64_t va, bool writable, bool exec);
void *uvm_user_ptr(uint64_t pml4, uint64_t va, bool write);
void uvm_destroy(uint64_t pml4);
int uvm_pages_live(void);

extern uint64_t syscall_kernel_rsp;
void syscall_entry(void);
uint64_t syscall_bytes_written(void);
