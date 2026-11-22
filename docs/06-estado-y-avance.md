# 6. Estado y avance

*Actualizado: 21 de noviembre de 2026.*

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
| **Plan completo (16 fases)** | **~88 %**: 14 fases cerradas (0–13) más PCI; faltan la 14 y la opcional 15 |
| Plan completo, ponderado por esfuerzo | **~82 %**: quedan el pulido, el navegador completo (imágenes y CSS) y el port a ARM |
| Navegador funcional | **~80 %**: HTTP/HTTPS, formularios GET y búsqueda; faltan imágenes y CSS conectados, y POST |

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

Unas 22.000 líneas de C y ensamblador propias en `main` (sin contar `limine.h`, las fuentes generadas ni `third_party/`, que trae BearSSL y `stb_image`).

## Después de la fase 11

- **Navegador HTTP** (`kernel/apps/browser.c`, `html.c`): barra de dirección, historial, redirecciones, chunked, enlaces y HTML básico sin imágenes, CSS ni JavaScript. HTTPS espera a la fase 13.
- **Editor tipo Notepad:** selección, copiar y pegar, deshacer y rehacer, Guardar como, Abrir, Buscar, Ir a línea, ajuste de línea y título con la ruta.
- **`kprintf` atómico** por línea (interrupciones desactivadas durante la escritura serie).
- **Revisión de código** de la fase 11: corregidos `cp`/`mv` sobre el mismo archivo, `rename` que podía borrar el destino, `rm .`, nombres largos en Archivos, guardar un archivo abierto truncado y el menú que quedaba abierto al cambiar el foco.

- **Segunda revisión (/review completo):** corregidos un desborde de pila al normalizar URLs, el guardia de archivo truncado del Editor (rutas con `./`), Cortar con selección mayor al portapapeles y un `<head>` sin cerrar que congelaba la interfaz. Nuevos autotests de arranque para `vfs_path_join`, `vfs_copy`, `vfs_rename` y los helpers de string; el navegador avisa que el sitio puede exigir HTTPS.

- **Hilo de red:** el Navegador y `fetch`/`ping` de la Terminal cargan en un hilo aparte (`kernel/net/netjob.c`, mutex de red); la interfaz ya no se congela. Esc cancela una carga. Autotest `[OK] net-job`.

- **Fase 13, HTTPS:** BearSSL integrado (`third_party/bearssl`, `mk/tls.mk`), 118 raíces de Mozilla (`tools/gen_ca_roots.py`), cliente TLS 1.2 con validación (`kernel/net/tls.c`), HTTPS en el Navegador y en `fetch -s`. Los autotests cargan `https://example.com` y rechazan un certificado inválido. Límite conocido: sin RDRAND la entropía es débil (jitter), y la hora del RTC va en hora local.

- **Navegador, ronda 3:** tildes Latin-1 en todas las fuentes (con caída a Latin-1 si la página no es UTF-8), motor HTML endurecido (239 casos de robustez en el arranque, aviso `[page truncated]`, img/form/select visibles), reintento por HTTPS cuando un sitio http resetea la conexión, y un arreglo del lector TLS que bajó `emol.com` de 76 s a ~1 s. Probados: Google, Wikipedia, Hacker News, BBC, emol.com.

- **Navegador, ronda 4:** formularios interactivos (foco con cursor, escribir, Tab, Enter, casillas, radios y selects; envío GET con la URL codificada; los POST muestran un aviso); páginas `about:form` y `about:latin1`. Probado: una búsqueda en `about:form` carga Wikipedia, y en `google.com` el navegador envía `/search?q=...`. Autotests `[OK] html forms` (14 casos) y `[OK] html robustness` (239 casos).
- **Teclado Latinoamericano:** el driver ahora interpreta las teclas por posición con la distribución Latinoamericana por defecto (`kbd us|latam` en la Terminal), con Option como AltGr para `@ | # ~ \ [ ] ^`. La ñ y las tildes todavía no se pueden escribir. `ls` acepta `-l`/`-a`.
- **Módulos nuevos aún sin conectar al navegador:** decodificador de imágenes (`kernel/gfx/img.c`, `stb_image`: PNG, JPEG, GIF y BMP; `[OK] img decode`, 44 casos) y motor CSS (`kernel/apps/css.c`; `[OK] css`, 423 casos). Los intentos de conectarlos quedaron en las ramas `worktree-agent-ac41b765fd612e5c4` (CSS) y `worktree-agent-af8393c8afa1d1e00` (imágenes), sin fusionar.
- **Subido a GitHub:** hasta `20e245b`. Después hay un commit más en local: la fusión de formularios (`7ec6d65`).

## Próximos pasos

1. Navegador: conectar el módulo CSS y el de imágenes al motor HTML y a la descarga en segundo plano; POST con cuerpo de petición; teclas muertas para ñ y tildes. Sin JavaScript (haría falta soft-float en el kernel).
2. Editor con formato (negrita, cursiva, encabezados, listas) y lector de `.docx`, como pequeño procesador de texto propio. Ejecutar apps externas (por ejemplo WordCraft, en Rust) exigiría una capa POSIX y un target de Rust para myOS: es un proyecto aparte.
3. Fase 14: apagado y reinicio por ACPI, animaciones y resolución configurable (también resuelve que la ventana de QEMU arranque pequeña en macOS).
4. Deuda: menús Edit y View sin acciones, botón verde que solo recentra, escritura no atómica de archivos (el Editor, `cp` y `echo` truncan antes de escribir), `kprintf` con interrupciones apagadas.
