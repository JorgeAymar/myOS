#pragma once
// myOS system call interface for ring 3 programs (SYSCALL instruction).
// rax = number, args in rdi, rsi, rdx, r10, r8, r9; result in rax.
// Must match kernel/proc/syscall.c.
#define SYS_WRITE  0   // write(fd, buf, len) -> bytes written (fd 1/2 = serial console)
#define SYS_EXIT   1   // exit(code), does not return
#define SYS_YIELD  2   // yield()
#define SYS_SLEEP  3   // sleep(ms)
#define SYS_GETPID 4   // getpid() -> pid

static inline long syscall3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile ("syscall"
                      : "=a"(r)
                      : "a"(n), "D"(a), "S"(b), "d"(c)
                      : "rcx", "r11", "memory");
    return r;
}

static inline long sys_write(int fd, const void *buf, unsigned long len) {
    return syscall3(SYS_WRITE, fd, (long)buf, (long)len);
}
static inline __attribute__((noreturn)) void sys_exit(int code) {
    syscall3(SYS_EXIT, code, 0, 0);
    for (;;) {}
}
static inline void sys_yield(void) { syscall3(SYS_YIELD, 0, 0, 0); }
static inline void sys_sleep(unsigned long ms) { syscall3(SYS_SLEEP, (long)ms, 0, 0); }
static inline long sys_getpid(void) { return syscall3(SYS_GETPID, 0, 0, 0); }
