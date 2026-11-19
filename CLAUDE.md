# myOS

Hobby operating system written from scratch: x86_64, C11 freestanding + NASM, booted by Limine.
Roadmap and phase list: see PLAN.md. Work one phase at a time.

## Commands
- `make` — build `build/myos.iso` (fetches/builds Limine on first run via `make deps`)
- `make test` — headless boot test under BIOS **and** UEFI. Must print `PASS [bios]` and `PASS [uefi]`.
- `make screenshot` — boots headless and writes `build/screenshot.png` (`WAIT=6 make screenshot` for slower boots). Read the PNG to verify visual changes.
- `make run` / `make run-uefi` — interactive QEMU window, serial on stdout
- `make debug` — paused with GDB stub on :1234 plus `-d int,cpu_reset` logging (triple-fault hunting)
- Serial logs of test runs: `build/serial-bios.log`, `build/serial-uefi.log`

## Test protocol
- The kernel reports through COM1 serial. A test passes only when the kernel prints `[TEST] boot OK`
  and exits through `isa-debug-exit` with `QEMU_EXIT_SUCCESS` (QEMU exit status 33).
- New subsystems add self-checks at boot that print `[OK] <subsystem>` or call `fail("...")`.
- Never finish a phase with `make test` failing. Visual changes also need a screenshot check.

## Rules
- Host is an Apple Silicon Mac: the OS is x86_64 and only runs in QEMU (emulated). No VirtualBox.
- Toolchain is `x86_64-elf-gcc` / `x86_64-elf-ld` (Homebrew), never the host clang.
- Kernel flags: no SSE/MMX/x87, no red zone, `-mcmodel=kernel`, higher half at 0xffffffff80000000.
- `-Werror` is on: fix warnings, don't silence them.
- Layout: `kernel/arch/x86_64` (CPU-specific), `kernel/mm`, `kernel/drivers`, `kernel/gfx`, `kernel/wm`,
  `kernel/fs`, `kernel/proc`, `kernel/net`, `kernel/lib`, headers in `kernel/include`, user apps in `user/`.
- Visual identity is original to myOS. Don't copy Apple/Microsoft assets or trade dress.
- One commit per finished phase (or meaningful step) with `make test` green.
