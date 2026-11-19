#pragma once
// Access to information handed over by the Limine bootloader.
// Implemented in kernel/boot.c (owner: core). All pointers are already virtual.
#include <stdint.h>
#include <stdbool.h>
#include "limine.h"

void boot_check(void);                              // panics if Limine protocol unsupported
struct limine_framebuffer *boot_framebuffer(void);  // primary framebuffer (never NULL after boot_check)
struct limine_memmap_response *boot_memmap(void);   // physical memory map
uint64_t boot_hhdm_offset(void);                    // virt = phys + offset (higher-half direct map)
const char *boot_cmdline(void);                     // kernel command line ("" if none)
bool boot_is_test(void);                            // true when booted with cmdline "test"
