#pragma once
// Kernel threads, preemptive round-robin scheduler and synchronisation
// primitives. Owner: proc agent (kernel/proc/). Uniprocessor.
//
// sched_init() (declared in subsystems.h) turns the running kmain context into
// thread 0 ("kernel/main"), starts an idle thread and enables preemption from
// the PIT (time slice SCHED_SLICE_MS). After it, timer_sleep_ms() blocks the
// calling thread instead of spinning in hlt.
//
// Rules:
//  * Blocking calls (thread_sleep_ms, thread_join, mutex_lock, sem_down,
//    cond_wait, wq_wait) must not be called from IRQ handlers or with a
//    spinlock held. Wake-ups (mutex_unlock, sem_up, cond_signal, wq_wake_*)
//    are IRQ-safe; the actual switch happens when the IRQ returns.
//  * The kernel heap is NOT thread-safe yet: allocate with preemption
//    disabled (preempt_disable) or from a single thread until mm adds a lock.
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SCHED_SLICE_MS      10
#define THREAD_STACK_SIZE   (16 * 1024)
#define THREAD_NAME_MAX     32

typedef int tid_t;
typedef void (*thread_entry_t)(void *arg);

enum thread_state {
    THREAD_RUNNING,
    THREAD_READY,
    THREAD_BLOCKED,     // on a wait queue (mutex, semaphore, join...)
    THREAD_SLEEPING,    // until a tick deadline
    THREAD_ZOMBIE,      // exited, waiting to be reaped
};

// ---- threads ----
// Creates a kernel thread that runs entry(arg) and exits when entry returns.
// Returns its tid (> 0), or -1 if out of memory.
tid_t thread_create(const char *name, thread_entry_t entry, void *arg);
__attribute__((noreturn)) void thread_exit(void);
void thread_yield(void);
void thread_sleep_ms(uint64_t ms);
tid_t thread_current_id(void);
// Blocks until thread `tid` has exited. Returns false if no such thread.
bool thread_join(tid_t tid);

// Per-thread preemption control (nests). While disabled the timer still ticks
// but the current thread is not switched out; a pending reschedule happens in
// the outermost preempt_enable().
void preempt_disable(void);
void preempt_enable(void);

// ---- introspection (for `ps`) ----
struct thread_info {
    tid_t tid;
    char name[THREAD_NAME_MAX];
    enum thread_state state;
    uint64_t cpu_ticks;      // timer ticks (ms) spent running
};
// Fills up to `max` entries; returns the total number of threads (may be > max).
int thread_list(struct thread_info *out, int max);
const char *thread_state_name(enum thread_state s);
uint64_t sched_context_switches(void);

// ---- spinlocks ----
// On a uniprocessor these mainly exclude the scheduler/IRQs; the flag still
// catches recursive locking bugs.
typedef struct { volatile uint32_t locked; } spinlock_t;
#define SPINLOCK_INIT { 0 }
void spin_init(spinlock_t *l);
void spin_lock(spinlock_t *l);              // disables preemption
void spin_unlock(spinlock_t *l);
uint64_t spin_lock_irqsave(spinlock_t *l);  // disables interrupts; returns saved flags
void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags);

// ---- wait queues ----
struct thread;
typedef struct { struct thread *head, *tail; } wait_queue_t;
#define WAIT_QUEUE_INIT { 0, 0 }
void wq_init(wait_queue_t *wq);
// Low-level: call with interrupts disabled; blocks the current thread on wq and
// returns (still with interrupts disabled) after it is woken. Re-check your
// condition in a loop.
void wq_wait(wait_queue_t *wq);
bool wq_wake_one(wait_queue_t *wq);         // true if a thread was woken
int wq_wake_all(wait_queue_t *wq);          // number of threads woken

// ---- mutex (blocking, non-recursive) ----
typedef struct {
    volatile bool locked;
    tid_t owner;
    wait_queue_t waiters;
    uint64_t contended;      // times a locker had to block (statistics)
} mutex_t;
#define MUTEX_INIT { false, 0, WAIT_QUEUE_INIT, 0 }
void mutex_init(mutex_t *m);
void mutex_lock(mutex_t *m);
bool mutex_trylock(mutex_t *m);
void mutex_unlock(mutex_t *m);

// ---- counting semaphore ----
typedef struct {
    volatile int64_t count;
    wait_queue_t waiters;
} semaphore_t;
void sem_init(semaphore_t *s, int64_t count);
void sem_down(semaphore_t *s);
bool sem_trydown(semaphore_t *s);
void sem_up(semaphore_t *s);                // IRQ-safe

// ---- condition variable (use with a mutex_t) ----
typedef struct { wait_queue_t waiters; } cond_t;
void cond_init(cond_t *c);
void cond_wait(cond_t *c, mutex_t *m);      // atomically unlocks m, waits, relocks
void cond_signal(cond_t *c);
void cond_broadcast(cond_t *c);

// ---- userspace processes (ring 3) ----
// Loads a static ELF64 executable into a new address space and runs it in
// ring 3 as a new thread; `arg` arrives in rdi of its entry point. When it
// exits (or is killed by a CPU exception: PROC_EXIT_KILLED) its exit code is
// stored to *exit_code if non-NULL (the pointer must stay valid until then).
// Returns the pid (== tid, usable with thread_join) or -1.
#define PROC_EXIT_KILLED (-1)
tid_t process_spawn(const char *name, const void *elf, size_t size, long arg, int *exit_code);
