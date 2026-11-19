#pragma once
// Formatted output. Implemented in kernel/lib/kprintf.c.
// Supports: %s %c %d %i %u %x %X %p %% with optional l/ll/z, width and '0' padding.
#include <stdarg.h>
#include <stddef.h>

void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   // -> serial
int ksnprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
