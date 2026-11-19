// System call dispatch. Numbers and ABI: user/syscall.h.
#include <stdint.h>
#include "proc.h"
#include "proc_internal.h"
#include "arch.h"
#include "serial.h"

#define SYS_WRITE  0
#define SYS_EXIT   1
#define SYS_YIELD  2
#define SYS_SLEEP  3
#define SYS_GETPID 4

#define EFAULT 14
#define EBADF  9
#define ENOSYS 38

// Layout matches the pushes in user_entry.asm (lowest address first).
struct syscall_frame {
    uint64_t nr, a0, a1, a2, a3, a4, a5;
    uint64_t rip, rflags, rsp;
};

uint64_t syscall_kernel_rsp;            // kernel stack top of the running user thread
static uint64_t bytes_written;

uint64_t syscall_bytes_written(void) { return bytes_written; }

static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len) {
    if (fd != 1 && fd != 2) return -EBADF;
    if (len > USER_TOP || buf > USER_TOP - len) return -EFAULT;
    uint64_t pml4 = sched_current->cr3;
    for (uint64_t i = 0; i < len; i++) {
        // Page-by-page validation through the process page tables: a bad
        // pointer returns -EFAULT instead of faulting in ring 0.
        const char *c = uvm_user_ptr(pml4, buf + i, false);
        if (!c) return i ? (int64_t)i : -EFAULT;
        serial_putc(*c);
        bytes_written++;
    }
    return (int64_t)len;
}

uint64_t syscall_dispatch(struct syscall_frame *f) {
    cpu_sti();                          // syscalls are preemptible
    int64_t r;
    switch (f->nr) {
    case SYS_WRITE:  r = sys_write(f->a0, f->a1, f->a2); break;
    case SYS_EXIT:   process_exit((int)f->a0);
    case SYS_YIELD:  thread_yield(); r = 0; break;
    case SYS_SLEEP:  thread_sleep_ms(f->a0); r = 0; break;
    case SYS_GETPID: r = thread_current_id(); break;
    default:         r = -ENOSYS; break;
    }
    cpu_cli();
    return (uint64_t)r;
}
