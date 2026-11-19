#pragma once
// Small CPU register / MSR helpers for x86_64.
#include <stdint.h>

static inline uint64_t read_cr0(void) { uint64_t v; __asm__ volatile ("mov %%cr0, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr2(void) { uint64_t v; __asm__ volatile ("mov %%cr2, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr3(void) { uint64_t v; __asm__ volatile ("mov %%cr3, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr4(void) { uint64_t v; __asm__ volatile ("mov %%cr4, %0" : "=r"(v)); return v; }

static inline uint64_t read_rflags(void) {
    uint64_t v;
    __asm__ volatile ("pushfq; popq %0" : "=r"(v) :: "memory");
    return v;
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)) : "memory");
}

// ~1us delay for old ISA devices (write to the unused POST port).
static inline void io_wait(void) {
    __asm__ volatile ("outb %%al, $0x80" :: "a"(0) : "memory");
}
