// Synchronisation primitives: spinlocks, wait queues, mutex, semaphore, condvar.
// Uniprocessor: atomicity against other threads and IRQs comes from disabling
// interrupts around the short critical sections inside the scheduler.
#include <stdint.h>
#include "proc.h"
#include "proc_internal.h"
#include "panic.h"

// ------------------------------------------------------------- spinlocks ----

void spin_init(spinlock_t *l) { l->locked = 0; }

static void spin_acquire(spinlock_t *l) {
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE)) {
        // Uniprocessor: if it is held, the holder can't run while we spin with
        // preemption/IRQs off, so this is a recursive-locking bug.
        panic("spinlock %p: already held (deadlock)", (void *)l);
    }
}

void spin_lock(spinlock_t *l) {
    preempt_disable();
    spin_acquire(l);
}

void spin_unlock(spinlock_t *l) {
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
    preempt_enable();
}

uint64_t spin_lock_irqsave(spinlock_t *l) {
    uint64_t f = irq_save();
    spin_acquire(l);
    return f;
}

void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags) {
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
    irq_restore(flags);
}

// ----------------------------------------------------------- wait queues ----

void wq_init(wait_queue_t *wq) { wq->head = wq->tail = NULL; }

void wq_wait(wait_queue_t *wq) {
    struct thread *t = sched_current;
    if (t->preempt_count) panic("wq_wait: '%s' would block with preemption disabled", t->name);
    t->state = THREAD_BLOCKED;
    t->qnext = NULL;
    if (wq->tail) wq->tail->qnext = t;
    else wq->head = t;
    wq->tail = t;
    sched_block();
}

static struct thread *wq_pop(wait_queue_t *wq) {
    struct thread *t = wq->head;
    if (t) {
        wq->head = t->qnext;
        if (!wq->head) wq->tail = NULL;
        t->qnext = NULL;
    }
    return t;
}

bool wq_wake_one(wait_queue_t *wq) {
    uint64_t f = irq_save();
    struct thread *t = wq_pop(wq);
    if (t) sched_make_ready(t);
    irq_restore(f);
    return t != NULL;
}

int wq_wake_all(wait_queue_t *wq) {
    uint64_t f = irq_save();
    int n = 0;
    struct thread *t;
    while ((t = wq_pop(wq))) { sched_make_ready(t); n++; }
    irq_restore(f);
    return n;
}

// ----------------------------------------------------------------- mutex ----

void mutex_init(mutex_t *m) {
    m->locked = false;
    m->owner = 0;
    wq_init(&m->waiters);
    m->contended = 0;
}

void mutex_lock(mutex_t *m) {
    uint64_t f = irq_save();
    if (m->locked && m->owner == thread_current_id())
        panic("mutex %p: recursive lock by thread %d", (void *)m, m->owner);
    while (m->locked) {
        m->contended++;
        wq_wait(&m->waiters);
    }
    m->locked = true;
    m->owner = thread_current_id();
    irq_restore(f);
}

bool mutex_trylock(mutex_t *m) {
    uint64_t f = irq_save();
    bool ok = !m->locked;
    if (ok) { m->locked = true; m->owner = thread_current_id(); }
    irq_restore(f);
    return ok;
}

void mutex_unlock(mutex_t *m) {
    uint64_t f = irq_save();
    if (!m->locked || m->owner != thread_current_id())
        panic("mutex %p: unlock by thread %d, owner %d", (void *)m, thread_current_id(), m->owner);
    m->locked = false;
    m->owner = 0;
    wq_wake_one(&m->waiters);
    irq_restore(f);
}

// ------------------------------------------------------------- semaphore ----

void sem_init(semaphore_t *s, int64_t count) {
    s->count = count;
    wq_init(&s->waiters);
}

void sem_down(semaphore_t *s) {
    uint64_t f = irq_save();
    while (s->count <= 0) wq_wait(&s->waiters);
    s->count--;
    irq_restore(f);
}

bool sem_trydown(semaphore_t *s) {
    uint64_t f = irq_save();
    bool ok = s->count > 0;
    if (ok) s->count--;
    irq_restore(f);
    return ok;
}

void sem_up(semaphore_t *s) {
    uint64_t f = irq_save();
    s->count++;
    wq_wake_one(&s->waiters);
    irq_restore(f);
}

// ------------------------------------------------------ condition variable ----

void cond_init(cond_t *c) { wq_init(&c->waiters); }

void cond_wait(cond_t *c, mutex_t *m) {
    uint64_t f = irq_save();
    mutex_unlock(m);          // with IRQs off nobody can signal in between
    wq_wait(&c->waiters);
    irq_restore(f);
    mutex_lock(m);
}

void cond_signal(cond_t *c) { wq_wake_one(&c->waiters); }
void cond_broadcast(cond_t *c) { wq_wake_all(&c->waiters); }
