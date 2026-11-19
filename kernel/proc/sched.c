// Preemptive round-robin scheduler for kernel threads (uniprocessor).
//  * Thread 0 is the kmain context (keeps the Limine boot stack).
//  * The idle thread runs only when nothing else is ready; it reaps exited
//    threads (frees their stacks) and halts.
//  * The PIT IRQ (via the arch irq-exit hook, after EOI) charges ticks, wakes
//    sleepers and preempts the running thread when its slice is used up.
// All scheduler state is protected by disabling interrupts.
#include <stdint.h>
#include <stddef.h>
#include "proc.h"
#include "proc_internal.h"
#include "arch.h"
#include "mm.h"
#include "panic.h"
#include "kprintf.h"
#include "string.h"
#include "subsystems.h"

void context_switch(uint64_t *save_rsp, uint64_t load_rsp);
void thread_trampoline(void);

struct thread *sched_current;
static struct thread main_thread;
static struct thread *idle_thread;
static struct thread *rq_head, *rq_tail;      // ready queue (FIFO)
static struct thread *sleep_head;             // unordered sleeping list
static struct thread *zombie_head;            // exited, not yet reaped
static struct thread *all_head;               // every live thread (for ps / join)
static volatile bool need_resched;
static int slice_left;
static tid_t next_tid;
static uint64_t switches;
static int live_stacks;
static uint64_t kernel_cr3;
static bool sched_running;

static inline uint64_t read_cr3_(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

uint64_t sched_context_switches(void) { return switches; }
int sched_live_stacks(void) { return live_stacks; }

// ---------------------------------------------------------------- queues ----

static void rq_push(struct thread *t) {
    t->qnext = NULL;
    if (rq_tail) rq_tail->qnext = t;
    else rq_head = t;
    rq_tail = t;
}

static struct thread *rq_pop(void) {
    struct thread *t = rq_head;
    if (t) {
        rq_head = t->qnext;
        if (!rq_head) rq_tail = NULL;
        t->qnext = NULL;
    }
    return t;
}

static void all_add(struct thread *t) {
    t->all_prev = NULL;
    t->all_next = all_head;
    if (all_head) all_head->all_prev = t;
    all_head = t;
}

static void all_remove(struct thread *t) {
    if (t->all_prev) t->all_prev->all_next = t->all_next;
    else all_head = t->all_next;
    if (t->all_next) t->all_next->all_prev = t->all_prev;
}

static struct thread *find_thread(tid_t tid) {
    for (struct thread *t = all_head; t; t = t->all_next)
        if (t->tid == tid) return t;
    return NULL;
}

void sched_make_ready(struct thread *t) {
    t->state = THREAD_READY;
    if (t != idle_thread) rq_push(t);
    if (sched_current == idle_thread) need_resched = true;
}

// ---------------------------------------------------------------- switch ----

static void check_canary(struct thread *t) {
    if (!t->stack) return;
    const uint64_t *w = (const uint64_t *)t->stack;
    for (int i = 0; i < STACK_CANARY_WORDS; i++)
        if (w[i] != STACK_CANARY) panic("sched: kernel stack overflow in thread %d '%s'", t->tid, t->name);
}

// Picks the next thread and switches to it. Interrupts must be disabled.
static void schedule(void) {
    struct thread *prev = sched_current;
    need_resched = false;
    if (prev->state == THREAD_RUNNING && prev != idle_thread) {
        prev->state = THREAD_READY;
        rq_push(prev);
    }
    struct thread *next = rq_pop();
    if (!next) next = idle_thread;
    next->state = THREAD_RUNNING;
    slice_left = SCHED_SLICE_MS;
    if (next == prev) return;

    check_canary(prev);
    switches++;
    sched_current = next;
    if (next->kstack_top) arch_set_kernel_stack(next->kstack_top);
    if (next->cr3 && next->cr3 != read_cr3_())
        __asm__ volatile ("mov %0, %%cr3" :: "r"(next->cr3) : "memory");
    context_switch(&prev->rsp, next->rsp);
    // Back in `prev`, possibly much later.
}

void sched_block(void) {
    schedule();
}

// Runs at the end of every hardware IRQ (interrupts disabled, after EOI).
static void sched_irq_exit(uint8_t irq) {
    struct thread *cur = sched_current;
    if (irq == 0) {
        cur->cpu_ticks++;
        uint64_t now = timer_ticks_ms();
        struct thread **pp = &sleep_head;
        while (*pp) {
            struct thread *t = *pp;
            if (t->wake_tick <= now) {
                *pp = t->qnext;
                sched_make_ready(t);
            } else {
                pp = &t->qnext;
            }
        }
        if (cur != idle_thread && --slice_left <= 0) need_resched = true;
    }
    if (cur == idle_thread && rq_head) need_resched = true;
    if (need_resched && cur->preempt_count == 0) schedule();
}

// ---------------------------------------------------------------- threads ---

void thread_start(thread_entry_t entry, void *arg) {
    cpu_sti();
    entry(arg);
    thread_exit();
}

tid_t thread_create(const char *name, thread_entry_t entry, void *arg) {
    uint64_t f = irq_save();          // heap has no lock yet: don't get preempted inside it
    struct thread *t = kmalloc(sizeof *t);
    uint8_t *stack = kmalloc(THREAD_STACK_SIZE);
    if (!t || !stack) {
        if (t) kfree(t);
        if (stack) kfree(stack);
        irq_restore(f);
        return -1;
    }
    irq_restore(f);

    memset(t, 0, sizeof *t);
    strncpy(t->name, name ? name : "?", THREAD_NAME_MAX - 1);
    t->stack = stack;
    t->cr3 = kernel_cr3;
    for (int i = 0; i < STACK_CANARY_WORDS; i++) ((uint64_t *)stack)[i] = STACK_CANARY;

    uint64_t *sp = (uint64_t *)(stack + THREAD_STACK_SIZE);
    *--sp = 0;                                  // alignment slot
    *--sp = (uint64_t)thread_trampoline;        // context_switch's `ret`
    *--sp = 0;                                  // rbp
    *--sp = 0;                                  // rbx
    *--sp = (uint64_t)entry;                    // r12
    *--sp = (uint64_t)arg;                      // r13
    *--sp = 0;                                  // r14
    *--sp = 0;                                  // r15
    t->rsp = (uint64_t)sp;

    f = irq_save();
    t->tid = next_tid++;
    live_stacks++;
    all_add(t);
    sched_make_ready(t);
    irq_restore(f);
    return t->tid;
}

void thread_exit(void) {
    cpu_cli();
    struct thread *t = sched_current;
    if (t == &main_thread || t == idle_thread) panic("thread_exit: '%s' may not exit", t->name);
    if (t->preempt_count) panic("thread_exit: '%s' exits with preemption disabled", t->name);
    wq_wake_all(&t->joiners);
    t->state = THREAD_ZOMBIE;
    t->qnext = zombie_head;
    zombie_head = t;
    schedule();
    panic("thread_exit: zombie '%s' was scheduled", t->name);
}

// Frees exited threads. Interrupts disabled, never on a zombie's own stack.
static void reap_zombies(void) {
    while (zombie_head) {
        struct thread *t = zombie_head;
        zombie_head = t->qnext;
        all_remove(t);
        check_canary(t);
        kfree(t->stack);
        kfree(t);
        live_stacks--;
    }
}

void thread_yield(void) {
    uint64_t f = irq_save();
    schedule();
    irq_restore(f);
}

void thread_sleep_ms(uint64_t ms) {
    if (!sched_running) {
        uint64_t deadline = timer_ticks_ms() + ms;
        while (timer_ticks_ms() < deadline) __asm__ volatile ("sti; hlt" ::: "memory");
        return;
    }
    if (ms == 0) { thread_yield(); return; }
    uint64_t f = irq_save();
    struct thread *t = sched_current;
    if (t == idle_thread) panic("thread_sleep_ms: idle thread may not sleep");
    t->wake_tick = timer_ticks_ms() + ms;
    t->state = THREAD_SLEEPING;
    t->qnext = sleep_head;
    sleep_head = t;
    schedule();
    irq_restore(f);
}

tid_t thread_current_id(void) { return sched_current ? sched_current->tid : 0; }

bool thread_join(tid_t tid) {
    uint64_t f = irq_save();
    struct thread *t = find_thread(tid);
    bool found = t != NULL;
    if (t == sched_current) { irq_restore(f); return false; }
    // Re-lookup after every wake: the target may have been reaped meanwhile.
    while ((t = find_thread(tid)) && t->state != THREAD_ZOMBIE)
        wq_wait(&t->joiners);
    irq_restore(f);
    return found;
}

void preempt_disable(void) {
    if (sched_current) sched_current->preempt_count++;
}

void preempt_enable(void) {
    struct thread *t = sched_current;
    if (!t) return;
    if (t->preempt_count <= 0) panic("preempt_enable: unbalanced in '%s'", t->name);
    if (--t->preempt_count == 0 && need_resched) {
        uint64_t f = irq_save();
        if (need_resched) schedule();
        irq_restore(f);
    }
}

const char *thread_state_name(enum thread_state s) {
    switch (s) {
    case THREAD_RUNNING:  return "running";
    case THREAD_READY:    return "ready";
    case THREAD_BLOCKED:  return "blocked";
    case THREAD_SLEEPING: return "sleeping";
    case THREAD_ZOMBIE:   return "zombie";
    }
    return "?";
}

int thread_list(struct thread_info *out, int max) {
    uint64_t f = irq_save();
    int n = 0;
    for (struct thread *t = all_head; t; t = t->all_next, n++) {
        if (n >= max || !out) continue;
        out[n].tid = t->tid;
        memcpy(out[n].name, t->name, THREAD_NAME_MAX);
        out[n].state = t->state;
        out[n].cpu_ticks = t->cpu_ticks;
    }
    irq_restore(f);
    return n;
}

// ------------------------------------------------------------------ init ----

static void idle_main(void *arg) {
    (void)arg;
    for (;;) {
        cpu_cli();
        reap_zombies();
        if (rq_head) schedule();
        // Atomic "sti; hlt": an IRQ arriving right after cli wakes us from hlt.
        __asm__ volatile ("sti; hlt" ::: "memory");
    }
}

static void sleep_hook(uint64_t ms) { thread_sleep_ms(ms); }

void sched_init(void) {
    uint64_t f = irq_save();
    kernel_cr3 = read_cr3_();

    main_thread.tid = 0;
    strncpy(main_thread.name, "kernel/main", THREAD_NAME_MAX - 1);
    main_thread.state = THREAD_RUNNING;
    main_thread.cr3 = kernel_cr3;
    sched_current = &main_thread;
    all_add(&main_thread);
    next_tid = 1;
    irq_restore(f);

    tid_t idle = thread_create("idle", idle_main, NULL);
    if (idle < 0) panic("sched: cannot allocate idle thread");

    f = irq_save();
    idle_thread = find_thread(idle);
    // thread_create queued it; idle must never sit on the ready queue.
    rq_head = rq_tail = NULL;
    idle_thread->state = THREAD_READY;
    slice_left = SCHED_SLICE_MS;
    sched_running = true;
    arch_set_irq_exit_hook(sched_irq_exit);
    timer_set_sleep_hook(sleep_hook);
    irq_restore(f);

    sched_selftest();
}
