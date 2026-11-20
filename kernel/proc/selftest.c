// Boot self-tests of the scheduler and sync primitives (run from sched_init).
// The worker threads never allocate: the heap is not thread-safe yet.
#include <stdint.h>
#include "proc.h"
#include "proc_internal.h"
#include "arch.h"
#include "panic.h"
#include "kprintf.h"

#define N_WORKERS 3
#define ITERS     200

static mutex_t counter_lock = MUTEX_INIT;
static volatile uint64_t counter;

static void counter_worker(void *arg) {
    int id = (int)(uintptr_t)arg;
    for (int i = 0; i < ITERS; i++) {
        mutex_lock(&counter_lock);
        uint64_t v = counter;
        if (i % 8 == id) thread_yield();          // hold the lock across a switch
        counter = v + 1;
        mutex_unlock(&counter_lock);
        if (i % 50 == id) thread_sleep_ms(1);
    }
}

static semaphore_t ping, pong;
static volatile int pings;
static void pong_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 20; i++) {
        sem_down(&ping);
        pings++;
        sem_up(&pong);
    }
}

static mutex_t cv_lock = MUTEX_INIT;
static cond_t cv;
static volatile int cv_stage;
static void cv_worker(void *arg) {
    (void)arg;
    mutex_lock(&cv_lock);
    while (cv_stage != 1) cond_wait(&cv, &cv_lock);
    cv_stage = 2;
    cond_broadcast(&cv);
    mutex_unlock(&cv_lock);
}

static volatile bool spin_stop;
static volatile uint64_t spin_count;
static void spinner(void *arg) {
    (void)arg;
    while (!spin_stop) spin_count++;              // never yields
}

// kprintf atomicity: three threads print numbered lines concurrently; each line
// is "[kpl] tN nNNN <marker>" and the host greps the serial log for garbling.
#define KPL_LINES 80
static void kpl_worker(void *arg) {
    int id = (int)(uintptr_t)arg;
    for (int i = 0; i < KPL_LINES; i++) {
        kprintf("[kpl] t%d n%03d ======================== end\n", id, i);
        if (i % 16 == 0) thread_yield();
    }
}

static tid_t must_create(const char *name, thread_entry_t fn, void *arg) {
    tid_t t = thread_create(name, fn, arg);
    if (t < 0) panic("sched test: thread_create('%s') failed", name);
    return t;
}

void sched_selftest(void) {
    uint64_t t_start = timer_ticks_ms();
    uint64_t sw0 = sched_context_switches();

    // 1. Mutex-protected counter from three threads with yields and sleeps.
    tid_t w[N_WORKERS];
    for (int i = 0; i < N_WORKERS; i++)
        w[i] = must_create("test/counter", counter_worker, (void *)(uintptr_t)i);
    for (int i = 0; i < N_WORKERS; i++)
        if (!thread_join(w[i])) panic("sched test: join(%d) found no thread", w[i]);
    if (counter != N_WORKERS * ITERS)
        panic("sched test: counter=%lu, expected %d (mutex broken)", counter, N_WORKERS * ITERS);
    if (counter_lock.contended == 0) panic("sched test: mutex never contended");

    // 2. Semaphore ping-pong and condition variable handshake.
    sem_init(&ping, 0);
    sem_init(&pong, 0);
    cond_init(&cv);
    tid_t pw = must_create("test/pong", pong_worker, NULL);
    tid_t cw = must_create("test/cond", cv_worker, NULL);
    for (int i = 0; i < 20; i++) {
        sem_up(&ping);
        sem_down(&pong);
        if (pings != i + 1) panic("sched test: semaphore ping-pong out of step (%d/%d)", pings, i + 1);
    }
    thread_sleep_ms(1);                           // let cv_worker reach cond_wait
    mutex_lock(&cv_lock);
    cv_stage = 1;
    cond_signal(&cv);
    while (cv_stage != 2) cond_wait(&cv, &cv_lock);
    mutex_unlock(&cv_lock);
    thread_join(pw);
    thread_join(cw);

    // 3. Sleep timing on an otherwise idle system.
    uint64_t s = timer_ticks_ms();
    thread_sleep_ms(10);
    uint64_t slept = timer_ticks_ms() - s;
    if (slept < 10 || slept > 16) panic("sched test: thread_sleep_ms(10) took %lu ms", slept);

    // 4. Preemption: a thread that never yields must neither starve us while we
    //    busy-wait nor delay our wake-up by more than about one time slice.
    tid_t sp = must_create("test/spinner", spinner, NULL);
    s = timer_ticks_ms();
    while (timer_ticks_ms() - s < 3 * SCHED_SLICE_MS) __asm__ volatile ("pause");
    if (spin_count == 0) panic("sched test: busy main thread was never preempted");
    uint64_t spun = spin_count;
    s = timer_ticks_ms();
    thread_sleep_ms(20);
    uint64_t loaded = timer_ticks_ms() - s;
    if (loaded < 20 || loaded > 20 + 2 * SCHED_SLICE_MS + 6)
        panic("sched test: sleep(20) under a CPU hog took %lu ms", loaded);
    if (spin_count == spun) panic("sched test: spinner did not run while main slept");
    spin_stop = true;
    thread_join(sp);

    // 5. Exited threads are reaped (by the idle thread) and their stacks freed.
    thread_sleep_ms(2);
    struct thread_info info[8];
    int n = thread_list(info, 8);
    if (n != 2 || sched_live_stacks() != 1)
        panic("sched test: %d threads / %d stacks left after exit, expected 2 / 1",
              n, sched_live_stacks());

    // 6. kprintf lock: concurrent printing must not interleave lines.
    for (int i = 0; i < N_WORKERS; i++)
        w[i] = must_create("test/kprintf", kpl_worker, (void *)(uintptr_t)i);
    for (int i = 0; i < N_WORKERS; i++) thread_join(w[i]);
    kprintf("[OK] kprintf-lock: %d lines from %d threads\n", N_WORKERS * KPL_LINES, N_WORKERS);

    uint64_t took = timer_ticks_ms() - t_start;
    kprintf("[OK] sched mutex counter=%lu (contended %lu), sem/cond handshake\n",
            counter, counter_lock.contended);
    kprintf("[OK] sched preempt: hog ran %lu loops, sleep(10)=%lums, sleep(20) under load=%lums\n",
            spin_count, slept, loaded);
    kprintf("[OK] sched reaped 6 threads, %d left, %lu switches, test took %lu ms\n",
            n, sched_context_switches() - sw0, took);
}

// ---- ring 3 ----
extern const uint8_t user_hello_elf[], user_hello_elf_end[];

void user_selftest(void) {
    size_t size = (size_t)(user_hello_elf_end - user_hello_elf);
    int pages0 = uvm_pages_live();
    uint64_t w0 = syscall_bytes_written();

    static int code_ok = -12345, code_bad = -12345;
    tid_t pid = process_spawn("user/hello", user_hello_elf, size, 0, &code_ok);
    if (pid < 0) panic("user test: process_spawn(hello) failed");
    thread_join(pid);
    if (code_ok != 42) panic("user test: hello exited with %d, expected 42", code_ok);
    uint64_t written = syscall_bytes_written() - w0;
    if (written != 18) panic("user test: hello wrote %lu bytes, expected 18", written);

    tid_t bad = process_spawn("user/faulty", user_hello_elf, size, 1, &code_bad);
    if (bad < 0) panic("user test: process_spawn(faulty) failed");
    thread_join(bad);
    if (code_bad != PROC_EXIT_KILLED) panic("user test: faulting process exit code %d", code_bad);

    thread_sleep_ms(2);                       // idle reaps both processes
    if (uvm_pages_live() != pages0 || sched_live_stacks() != 1)
        panic("user test: leak after exit (%d user pages, %d stacks)",
              uvm_pages_live() - pages0, sched_live_stacks());
    kprintf("[OK] user ring3: pid %d exit=%d via syscalls, faulting pid %d killed, "
            "address spaces freed\n", pid, code_ok, bad);
}
