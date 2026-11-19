#pragma once
// Fatal error handling and QEMU test exit. Implemented in kernel/lib/panic.c.
#include <stdint.h>

#define QEMU_EXIT_SUCCESS 0x10  // QEMU exit status 33 (only with isa-debug-exit device)
#define QEMU_EXIT_FAILURE 0x11  // QEMU exit status 35

void qemu_exit(uint8_t code);
__attribute__((noreturn)) void halt_forever(void);
// Prints "[FAIL] <msg>" on serial, exits QEMU with failure (test mode) and halts.
__attribute__((noreturn, format(printf, 1, 2))) void panic(const char *fmt, ...);
