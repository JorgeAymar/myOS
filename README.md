# myOS

*[Leer en español](README.es.md)*

A hobby operating system written from scratch in **C and assembly** for **x86_64**, booted by [Limine](https://github.com/limine-bootloader/limine) and run in **QEMU**. There is no Linux underneath, no external libraries and no prebuilt kernel: every layer, from boot and interrupts through memory, drivers, graphics and windows to the apps, is written in this repository.

The project is built with [Claude Code](https://claude.com/claude-code). Work moves in small phases, each with automated verification, and several agents work in parallel against well-defined module contracts.

> **Status:** early development (v0.1). Boots on BIOS and UEFI, with serial logging and a headless test harness. Memory management (PMM, paging, heap) and the PS/2 keyboard, mouse and RTC drivers are merged. CPU/interrupt setup and the desktop UI are being integrated.

## Features (v0.1 target)

- [x] Limine boot on BIOS and UEFI from a hybrid ISO
- [x] Higher-half kernel, `kprintf`, `panic`, serial console
- [x] Physical memory manager (bitmap), 4-level paging, kernel heap with corruption checks
- [x] PS/2 keyboard (US layout, extended keys, modifiers), PS/2 mouse with scroll wheel, RTC clock
- [ ] GDT/TSS, IDT, exceptions with register dumps, PIC, 1000 Hz PIT timer
- [ ] Double-buffered compositor that only redraws the parts of the screen that changed
- [ ] Original myOS desktop: top bar, dock, draggable windows
- [ ] Apps: Terminal with a shell, About myOS, Clock

## Requirements (macOS, Apple Silicon or Intel)

```bash
brew install qemu nasm xorriso x86_64-elf-gcc x86_64-elf-binutils
```

The Limine binaries (branch `v9.x-binary`) are fetched automatically on the first `make`.

> On Apple Silicon, VirtualBox **cannot** run an x86 OS. myOS targets QEMU, which emulates x86_64.

## Usage

| Command | What it does |
|---|---|
| `make` | Builds `build/myos.iso` |
| `make run` | Opens myOS in a QEMU window (BIOS boot), serial log in the terminal |
| `make run-uefi` | Same, but boots with UEFI firmware (OVMF) |
| `make test` | Headless boot test on BIOS and UEFI; should print `PASS [bios]` and `PASS [uefi]` |
| `make screenshot` | Boots headless and saves `build/screenshot.png` (`WAIT=8` to wait longer) |
| `make debug` | Starts paused with a GDB stub on `:1234` and interrupt/reset logging |
| `make clean` / `make distclean` | Removes `build/` (`distclean` also removes `limine/`) |

### How the tests work

`make test` boots a separate ISO whose kernel command line is `test`. In that mode the kernel runs its self-tests, renders a few desktop frames, prints `[TEST] boot OK` to the serial port and shuts QEMU down through the `isa-debug-exit` device. A run passes only if QEMU exits with the expected status code and the log contains that line. Each subsystem prints its own `[OK] ...` line or stops the boot with `panic()`. Logs are written to `build/serial-*.log`.

Sample output from a passing run:

```
myOS booting...
[OK] boot framebuffer 1280x800x32 cmdline='test'
[OK] mm total=238 MiB free=238 MiB
[OK] heap range=0xffffc00000000000..0xffffc11000000000
[OK] input ps2 kbd=yes mouse=yes wheel=yes
[OK] rtc 2026-11-19 01:51:29
[TEST] boot OK
```

## Project layout

```
kernel/
├── main.c            # kmain: brings up each subsystem in order (thin on purpose)
├── boot.c            # Limine requests (framebuffer, memmap, HHDM, cmdline)
├── linker.ld         # kernel linked at 0xffffffff80000000
├── include/          # module contracts: arch.h, mm.h, input.h, rtc.h, desktop.h...
├── lib/              # kprintf, string, panic, weak_stubs (fallback implementations)
├── arch/x86_64/      # GDT, IDT, ISRs, PIC, PIT
├── mm/               # PMM, VMM, heap
├── drivers/          # serial, PS/2 keyboard and mouse, RTC
├── gfx/ wm/ apps/    # graphics, window manager, desktop and apps
tests/                # boot_test.sh, screenshot.sh
limine.conf           # normal boot entry
limine-test.conf      # test boot entry (cmdline: test)
```

Each module implements the functions declared in its header under `kernel/include/`. `kernel/lib/weak_stubs.c` provides minimal "weak" fallback versions of all of them, so any module can be built and tested on its own. When linking, the real implementation always replaces the fallback.

## Roadmap

The full plan, phases 0 to 15 (multitasking, ring 3 userspace, FAT32, TCP/IP networking, HTTPS and an aarch64 port), is in [PLAN.md](PLAN.md) (in Spanish). The working rules for coding agents are in [CLAUDE.md](CLAUDE.md).

## Credits

- [Limine](https://github.com/limine-bootloader/limine): bootloader and `limine.h` (Limine boot protocol)
- Inspired by the experiment "I Forced AI to Build a Custom Operating System" by The Render Guy, which produced AuroraOS
