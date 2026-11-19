# 6. Estado y avance

*Actualizado: 19 de noviembre de 2026.*

## Fases

| # | Fase | Estado |
|---|---|---|
| 0 | Setup, toolchain, tests | ✅ Hecha |
| 1 | Arranque con Limine (BIOS + UEFI) | ✅ Hecha |
| 2 | CPU: GDT/TSS, IDT, excepciones, PIC, PIT | ✅ Hecha |
| 3 | Memoria: PMM, paginación, heap | ✅ Hecha |
| 4 | Entrada: teclado y ratón PS/2, RTC | ✅ Hecha |
| — | Bus PCI y `mmio_map` | ✅ Hecho |
| 5 | Gráficos: doble buffer, primitivas, fuentes | ✅ Hecha |
| 6 | Identidad visual estilo macOS | ✅ Hecha |
| 7 | Gestor de ventanas, dock, barra superior | ✅ Hecha |
| 8 | Multitarea: hilos y scheduler | ✅ Hecha |
| 9 | Userspace: ring 3, syscalls, ELF | ✅ Hecha |
| 10 | Sistema de archivos: VFS, initrd, AHCI, FAT32 | ✅ Hecha |
| 11 | Apps: Terminal, Archivos, Editor, Reloj, Ajustes | 🔄 Reloj hecho; el resto, pendiente |
| 12 | Red: e1000, TCP/IP, DHCP, DNS, HTTP | ✅ Hecha |
| 13 | TLS + navegador | ⏳ Pendiente |
| 14 | Pulido: apagado ACPI, animaciones, rendimiento | ⏳ Pendiente |
| 15 | Port a aarch64, SMP, audio | ⏳ Opcional |

## Porcentaje de avance

| Meta | Avance |
|---|---|
| **v0.1, primer escritorio usable (fases 0–7)** | **100 %**: integrada y funcionando en QEMU |
| **Plan completo (16 fases)** | **~75 %**: 12 fases cerradas (0–10 y 12) más PCI |
| Plan completo, ponderado por esfuerzo | **~68 %**: quedan apps, HTTPS y navegador, pulido y port a ARM |

## Tiempo de arranque

Desde que se lanza QEMU hasta que el escritorio está listo pasan **unos 2,5 s** en un Mac M3 emulando x86_64:

| Momento | Hito |
|---|---|
| 0,8 s | Firmware y Limine terminan; el kernel imprime `myOS booting...` |
| 0,85 s | CPU, interrupciones y memoria listos |
| 1,1 s | Multitarea, ring 3, teclado y ratón listos |
| 1,4 s | Disco FAT32 montado y verificado |
| 2,4 s | Red: DHCP (en modo test también ping, DNS y HTTP) |
| 2,5 s | Escritorio visible |

Los autotests de red y de escritura en disco ya solo corren en modo test. El segundo que tarda DHCP se debe a la autonegociación simulada de la tarjeta de red de QEMU.

## Tamaño del código

Unas 10.000 líneas de C y ensamblador integradas en `main` (sin contar `limine.h` ni las fuentes generadas), con todos los subsistemas de las rondas 1 y 2 ya integrados.

## Próximos pasos

1. Llamar a `net_poll()` en el bucle principal, o desde un hilo de red, para que la red funcione después del arranque.
2. Fase 11: Terminal con shell (`ls`, `cat`, `ping`, `fetch`), Archivos y Ajustes.
