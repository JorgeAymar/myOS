// myOS kernel entry point. Owner: core (integration only — keep it thin).
#include <stdint.h>
#include "boot.h"
#include "serial.h"
#include "kprintf.h"
#include "panic.h"
#include "arch.h"
#include "mm.h"
#include "input.h"
#include "desktop.h"
#include "pci.h"
#include "subsystems.h"

void kmain(void) {
    serial_init();
    kprintf("myOS booting...\n");

    boot_check();
    struct limine_framebuffer *fb = boot_framebuffer();
    kprintf("[OK] boot framebuffer %lux%lux%u cmdline='%s'\n",
            fb->width, fb->height, fb->bpp, boot_cmdline());

    arch_init();
    mm_init();
    pci_init();
    sched_init();
    input_init();
    fs_init();
    net_init();
    desktop_init();

    if (boot_is_test()) {
        // Headless test mode: render a few frames, then report and exit QEMU.
        tls_selftest();
        html_selftest();
        for (int i = 0; i < 3; i++) desktop_frame();
        kprintf("[TEST] boot OK\n");
        qemu_exit(QEMU_EXIT_SUCCESS);
        halt_forever();
    }

    desktop_run();
}
