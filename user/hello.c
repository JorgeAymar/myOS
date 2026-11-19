// First ring 3 program. Built by mk/proc.mk into build/user/hello.elf and
// embedded in the kernel for the boot self-test.
//   mode 0: greet, exercise write/yield/sleep/getpid, exit(42) if all checks pass
//   mode 1: touch kernel memory -> page fault -> the kernel must kill only us
#include "syscall.h"

static unsigned long len(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

void _start(long mode) {
    if (mode == 1) {
        *(volatile unsigned long *)0xffffffff80000000ul = 0;   // not user-accessible
        sys_exit(99);                                           // must not get here
    }
    const char *msg = "hello from ring 3\n";
    long pid = sys_getpid();
    long n = sys_write(1, msg, len(msg));
    sys_yield();
    sys_sleep(2);
    // Bad pointer must be rejected, not crash the kernel.
    long bad = sys_write(1, (const void *)0xffffffff80000000ul, 16);
    sys_exit(n == (long)len(msg) && bad < 0 && pid > 0 ? 42 : 1);
}
