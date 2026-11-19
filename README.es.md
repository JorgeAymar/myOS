# myOS

*[Read in English](README.md)*

Un sistema operativo hobby escrito desde cero en **C y ensamblador** para **x86_64**, arrancado con [Limine](https://github.com/limine-bootloader/limine) y ejecutado en **QEMU**. No usa Linux, ni librerías externas, ni un kernel prefabricado: cada capa (arranque, interrupciones, memoria, drivers, gráficos, ventanas y apps) se escribe en este repo.

El proyecto se desarrolla con [Claude Code](https://claude.com/claude-code). Se avanza por fases pequeñas, cada una con verificación automática, y se usan varios agentes en paralelo que trabajan contra contratos de módulos bien definidos.

> **Estado:** en desarrollo temprano (v0.1). Arranca en BIOS y UEFI, con salida por serie y un framework de tests sin pantalla. Ya están integrados la gestión de memoria (PMM, paginación y heap) y los drivers de teclado, ratón y reloj (PS/2, RTC). Se están integrando la configuración de CPU e interrupciones y el escritorio.

## Características (objetivo v0.1)

- Arranque con Limine en BIOS y UEFI desde una ISO híbrida
- Kernel en la mitad alta de memoria (higher half), con GDT/TSS, IDT, volcado de registros en excepciones, PIC y timer PIT a 1000 Hz
- Gestor de memoria física, paginación de 4 niveles y heap del kernel
- Teclado y ratón PS/2 (con rueda) y reloj RTC
- Compositor con doble buffer y redibujado solo de las zonas que cambian
- Escritorio propio de myOS: barra superior, dock y ventanas arrastrables
- Apps: Terminal con shell, Acerca de myOS y Reloj

## Requisitos (macOS con Apple Silicon o Intel)

```bash
brew install qemu nasm xorriso x86_64-elf-gcc x86_64-elf-binutils
```

El binario de Limine (rama `v9.x-binary`) se descarga solo en el primer `make`.

> En un Mac con Apple Silicon, VirtualBox **no** puede ejecutar un OS x86. myOS corre en QEMU, que emula x86_64.

## Uso

| Comando | Qué hace |
|---|---|
| `make` | Compila `build/myos.iso` |
| `make run` | Abre myOS en una ventana de QEMU (BIOS), con el log serie en la terminal |
| `make run-uefi` | Igual, pero arranca con firmware UEFI (OVMF) |
| `make test` | Test sin pantalla en BIOS y UEFI; debe mostrar `PASS [bios]` y `PASS [uefi]` |
| `make screenshot` | Arranca sin pantalla y guarda `build/screenshot.png` (`WAIT=8` para esperar más) |
| `make debug` | Arranca en pausa con GDB en `:1234` y registra interrupciones y reinicios |
| `make clean` / `make distclean` | Borra `build/` (y `distclean` también `limine/`) |

### Cómo funcionan los tests

`make test` arranca una ISO aparte con `cmdline: test`. En ese modo el kernel ejecuta sus autotests, pinta unos cuantos frames del escritorio, escribe `[TEST] boot OK` por el puerto serie y apaga QEMU a través del dispositivo `isa-debug-exit`. El test pasa solo si QEMU termina con el código de salida esperado y el log contiene esa línea. Cada subsistema escribe su propia línea `[OK] ...` o detiene el arranque con `panic()`. Los logs quedan en `build/serial-*.log`.

## Estructura

```
kernel/
├── main.c            # kmain: orquesta la inicialización (fino a propósito)
├── boot.c            # peticiones a Limine (framebuffer, memmap, HHDM, cmdline)
├── linker.ld         # kernel en 0xffffffff80000000
├── include/          # contratos entre módulos: arch.h, mm.h, input.h, rtc.h, desktop.h...
├── lib/              # kprintf, string, panic, weak_stubs (implementaciones de reserva)
├── arch/x86_64/      # GDT, IDT, ISR, PIC, PIT
├── mm/               # PMM, VMM, heap
├── drivers/          # serie, PS/2, RTC
├── gfx/ wm/ apps/    # gráficos, gestor de ventanas, escritorio y apps
tests/                # boot_test.sh, screenshot.sh
limine.conf           # entrada normal
limine-test.conf      # entrada de test (cmdline: test)
```

Cada módulo implementa las funciones de su header en `kernel/include/`. `kernel/lib/weak_stubs.c` tiene versiones mínimas ("débiles") de todas esas funciones, así que cada módulo compila y se prueba por separado. Al enlazar, la implementación real sustituye siempre a la de reserva.

## Hoja de ruta

El plan completo, de la fase 0 a la 15 (multitarea, userspace en ring 3, FAT32, red TCP/IP, HTTPS y port a aarch64), está en [PLAN.md](PLAN.md). Las reglas de trabajo para agentes están en [CLAUDE.md](CLAUDE.md).

## Créditos

- [Limine](https://github.com/limine-bootloader/limine): bootloader y `limine.h` (protocolo Limine)
- Inspirado por el experimento "I Forced AI to Build a Custom Operating System" (The Render Guy), que creó AuroraOS
