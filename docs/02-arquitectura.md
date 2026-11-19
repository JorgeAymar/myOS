# 2. Arquitectura

## Flujo de arranque

```
Firmware (BIOS o UEFI)
  └─ Limine (ISO híbrida)
       ├─ lee limine.conf (o limine-test.conf con cmdline: test)
       ├─ carga kernel.elf en la mitad alta (0xffffffff80000000)
       ├─ prepara el long mode, la paginación, el HHDM y el framebuffer
       └─ salta a kmain()
            ├─ serial_init         COM1, 115200 baudios: canal de logs y tests
            ├─ boot_check          valida las respuestas de Limine
            ├─ arch_init           GDT/TSS, IDT, PIC, PIT a 1000 Hz, sti
            ├─ mm_init             PMM, VMM y heap
            ├─ pci_init            enumera el bus PCI
            ├─ sched_init          hilos, scheduler con desalojo, ring 3
            ├─ input_init          PS/2 teclado + ratón, RTC
            ├─ fs_init             VFS, initrd, AHCI, FAT32 (en curso)
            ├─ net_init            e1000 + TCP/IP, DHCP, DNS, HTTP
            ├─ desktop_init        compositor, gestor de ventanas, escritorio
            └─ modo test ? 3 frames + "[TEST] boot OK" + salida de QEMU
                         : desktop_run() (bucle a ~60 fps)
```

`kmain` ([kernel/main.c](../kernel/main.c)) es fino a propósito: solo llama a los `*_init` en orden. Cada subsistema hace sus propios autotests e imprime `[OK] ...`, o detiene el sistema con `panic()`.

## Mapa de memoria virtual

| Rango | Uso | Dueño |
|---|---|---|
| `0xffff8000_00000000 +` | HHDM: toda la RAM física mapeada por Limine (`virt = phys + offset`) | Limine / mm |
| `0xffffc000_00000000` – `+64 GiB` | Heap del kernel: arena first-fit para reservas pequeñas | mm |
| `+64 GiB` – `+1 TiB` | Heap: reservas grandes (≥ 256 KiB) en páginas propias con página de guarda | mm |
| `0xffffd000_00000000 +` | MMIO de dispositivos PCI (sin caché), con `mmio_map()` | pci |
| `0xffffffff_80000000 +` | Imagen del kernel (código, rodata, data, bss) | linker |

## Módulos y contratos

Cada módulo publica un **contrato**, un header en `kernel/include/` que declara las funciones que los demás pueden usar. El resto de su código es privado.

| Contrato | Función | Implementación |
|---|---|---|
| `boot.h` | Acceso a lo que entrega Limine | `kernel/boot.c` |
| `kprintf.h`, `string.h`, `panic.h` | Utilidades básicas | `kernel/lib/` |
| `arch.h` | CPU, interrupciones, timer | `kernel/arch/x86_64/` |
| `mm.h` | Memoria física y virtual, heap | `kernel/mm/` |
| `pci.h` | Bus PCI, BARs, `mmio_map` | `kernel/drivers/pci.c` |
| `input.h`, `rtc.h` | Teclado, ratón, reloj | `kernel/drivers/` |
| `desktop.h` | Escritorio | `kernel/gfx/`, `kernel/wm/`, `kernel/apps/` |
| `subsystems.h` | `sched_init`, `fs_init`, `net_init` | `kernel/proc/`, `kernel/fs/`, `kernel/net/` |

### Implementaciones de reserva (weak stubs)

[kernel/lib/weak_stubs.c](../kernel/lib/weak_stubs.c) define una versión mínima de **todas** las funciones de los contratos, marcada `__attribute__((weak))`. Por ejemplo, `kmalloc` reserva memoria de un bloque estático de 16 MiB, `input_poll` siempre dice "vacío" y el timer es simulado.

Así cualquier módulo compila, arranca y se prueba aunque los demás no existan. Al enlazar, la implementación real (fuerte) sustituye siempre a la de reserva. Es lo que permite que varios agentes trabajen en paralelo (ver el documento 5).

### Extensión del Makefile por módulo

El `Makefile` incluye `mk/*.mk`. Cada subsistema añade allí sus dispositivos de QEMU (`QEMU_EXTRA += ...`) y lo que necesitan los tests (`TEST_DEPS += build/disk.img`), sin tocar el Makefile principal.

## Disposición del repositorio

```
kernel/
├── main.c  boot.c  linker.ld
├── include/        contratos
├── lib/            kprintf, string, panic, weak_stubs
├── arch/x86_64/    gdt.c idt.c isr.asm pic.c pit.c arch.c
├── mm/             pmm.c vmm.c heap.c mm.c
├── drivers/        serial.c ps2.c keyboard.c mouse.c input.c rtc.c pci.c
├── gfx/ wm/ apps/  gráficos, ventanas, escritorio
├── proc/           sched, sync, process, syscall, ELF
├── net/            eth, ip, udp, dhcp, dns, tcp, http
├── fs/             en curso
user/               programas de usuario (ring 3)
mk/                 extensiones del Makefile por módulo
tests/              boot_test.sh, screenshot.sh
docs/               esta documentación
```
