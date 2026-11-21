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
| 11 | Apps: Terminal, Archivos, Editor, Reloj, Ajustes, Acerca de | ✅ Hecha |
| 12 | Red: e1000, TCP/IP, DHCP, DNS, HTTP | ✅ Hecha |
| 13 | TLS + navegador | ✅ Hecha (HTTPS con BearSSL y validación de certificados) |
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

## Después de la fase 11

- **Navegador HTTP** (`kernel/apps/browser.c`, `html.c`): barra de dirección, historial, redirecciones, chunked, enlaces y HTML básico sin imágenes, CSS ni JavaScript. HTTPS espera a la fase 13.
- **Editor tipo Notepad:** selección, copiar y pegar, deshacer y rehacer, Guardar como, Abrir, Buscar, Ir a línea, ajuste de línea y título con la ruta.
- **`kprintf` atómico** por línea (interrupciones desactivadas durante la escritura serie).
- **Revisión de código** de la fase 11: corregidos `cp`/`mv` sobre el mismo archivo, `rename` que podía borrar el destino, `rm .`, nombres largos en Archivos, guardar un archivo abierto truncado y el menú que quedaba abierto al cambiar el foco.

- **Segunda revisión (/review completo):** corregidos un desborde de pila al normalizar URLs, el guardia de archivo truncado del Editor (rutas con `./`), Cortar con selección mayor al portapapeles y un `<head>` sin cerrar que congelaba la interfaz. Nuevos autotests de arranque para `vfs_path_join`, `vfs_copy`, `vfs_rename` y los helpers de string; el navegador avisa que el sitio puede exigir HTTPS.

- **Hilo de red:** el Navegador y `fetch`/`ping` de la Terminal cargan en un hilo aparte (`kernel/net/netjob.c`, mutex de red); la interfaz ya no se congela. Esc cancela una carga. Autotest `[OK] net-job`.

- **Fase 13, HTTPS:** BearSSL integrado (`third_party/bearssl`, `mk/tls.mk`), 118 raíces de Mozilla (`tools/gen_ca_roots.py`), cliente TLS 1.2 con validación (`kernel/net/tls.c`), HTTPS en el Navegador y en `fetch -s`. Los autotests cargan `https://example.com` y rechazan un certificado inválido. Límite conocido: sin RDRAND la entropía es débil (jitter), y la hora del RTC va en hora local.

## Próximos pasos

1. Fase 14: apagado y reinicio por ACPI, animaciones y resolución configurable.
2. Deuda: menús Edit y View sin acciones, botón verde que solo recentra.
