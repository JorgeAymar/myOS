#include "kprintf.h"
#include "serial.h"
#include <stdint.h>
#include <stdbool.h>

struct out { char *buf; size_t size, len; };

static void put(struct out *o, char c) {
    if (o->len + 1 < o->size) o->buf[o->len] = c;
    o->len++;
}

static void put_num(struct out *o, uint64_t v, unsigned base, bool upper,
                    bool neg, int width, char pad) {
    char tmp[24]; int i = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do { tmp[i++] = digits[v % base]; v /= base; } while (v);
    int len = i + (neg ? 1 : 0);
    if (neg && pad == '0') put(o, '-');
    for (; len < width; len++) put(o, pad);
    if (neg && pad != '0') put(o, '-');
    while (i) put(o, tmp[--i]);
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    struct out o = { buf, size, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(&o, *fmt); continue; }
        fmt++;
        char pad = ' '; int width = 0, lng = 0;
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l' || *fmt == 'z') { lng++; fmt++; }
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int n = 0; while (s[n]) n++;
            for (; n < width; n++) put(&o, ' ');
            while (*s) put(&o, *s++);
            break;
        }
        case 'c': put(&o, (char)va_arg(ap, int)); break;
        case 'd': case 'i': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            bool neg = v < 0;
            put_num(&o, neg ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v, 10, false, neg, width, pad);
            break;
        }
        case 'u': put_num(&o, lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 10, false, false, width, pad); break;
        case 'x': case 'X':
            put_num(&o, lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 16, *fmt == 'X', false, width, pad);
            break;
        case 'p':
            put(&o, '0'); put(&o, 'x');
            put_num(&o, (uint64_t)va_arg(ap, void *), 16, false, false, 16, '0');
            break;
        case '%': put(&o, '%'); break;
        case 0: fmt--; break;
        default: put(&o, '%'); put(&o, *fmt); break;
        }
    }
    if (size) buf[o.len < size ? o.len : size - 1] = 0;
    return (int)o.len;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

// Uniprocessor: one whole line goes to the serial port inside a single
// irq-disabled section (same flag save/restore as irq_save() in the scheduler,
// so it nests with the PMM/VMM/heap sections). No lock is taken, so a panic or
// exception that interrupts a kprintf cannot deadlock. The line is formatted
// into a bounded stack buffer first (longer lines are truncated at 511 bytes)
// and nothing in the critical section allocates.
void kprintf(const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    uint64_t f;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    serial_puts(buf);
    if (f & (1u << 9)) __asm__ volatile ("sti" ::: "memory");
}
