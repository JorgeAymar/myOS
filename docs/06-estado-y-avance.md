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
| 10 | Sistema de archivos: VFS, initrd, AHCI, FAT32 | 🔄 En curso |
| 11 | Apps: Terminal, Archivos, Editor, Reloj, Ajustes | ⏳ Pendiente (se hará después de ver el escritorio) |
| 12 | Red: e1000, TCP/IP, DHCP, DNS, HTTP | ✅ Hecha |
| 13 | TLS + navegador | ⏳ Pendiente |
| 14 | Pulido: apagado ACPI, animaciones, rendimiento | ⏳ Pendiente |
| 15 | Port a aarch64, SMP, audio | ⏳ Opcional |

## Porcentaje de avance

| Meta | Avance |
|---|---|
| **v0.1, primer escritorio usable (fases 0–7)** | **100 %**: integrada y funcionando en QEMU |
| **Plan completo (16 fases)** | **~69 %**: 11 fases cerradas (0–9 y 12) más PCI; la 10 (archivos) está en curso |
| Plan completo, ponderado por esfuerzo | **~63 %**: quedan apps, HTTPS y navegador, y pulido |

## Tamaño del código

Unas 9.000 líneas de C y ensamblador integradas en `main` (sin contar `limine.h`), a las que se sumará el sistema de archivos cuando termine.

## Próximos pasos

1. Integrar el sistema de archivos (VFS, initrd, AHCI, FAT32).
2. Llamar a `net_poll()` en el bucle principal, o desde un hilo de red, para que la red funcione después del arranque.
3. Fase 11: Terminal con shell (`ls`, `cat`, `ping`, `fetch`), Archivos y Ajustes.
