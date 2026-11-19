# myOS — Estrategia de implementación

## 1. Qué hizo el video (y qué mejoramos)

Video: "I Forced AI to Build a Custom Operating System" (The Render Guy), con AuroraOS como resultado.

| En el video | Problema | En myOS |
|---|---|---|
| 1 prompt gigante, 7 fases de golpe (~50 min, ~6.000 líneas, 88 archivos) | Difícil de depurar y entender, sin control de versiones | Una fase por sesión, con un commit en git y un test que pasa antes de seguir |
| x86 + GRUB + BIOS, pensado para VirtualBox | No arranca en VirtualBox en Mac ARM | x86_64 + **Limine** (64 bits y framebuffer desde el arranque), con **QEMU** como objetivo principal |
| Claude probaba en QEMU "a ojo" | No había verificación automática | `make test`: QEMU sin pantalla, logs por puerto serie y capturas con `screendump` que Claude puede revisar |
| Clon visual de macOS | Sin identidad propia | Un sistema de diseño propio de myOS, definido antes del window manager |
| Navegador de texto, HTTPS hecho en el Mac anfitrión | El OS no es autónomo | Pila de red propia; TLS con una librería portada (BearSSL) en una fase posterior |
| Sin userspace real (todo dentro del kernel) | Una app que falla tumba el sistema | Ring 3, syscalls y un cargador ELF para que las apps estén aisladas |

## 2. Decisiones técnicas

- **Arquitectura:** x86_64. Tiene la mejor documentación (OSDev wiki) y QEMU la emula bien en el M3. Un port a aarch64 queda como fase opcional, que con HVF correría a velocidad nativa en UTM.
- **Bootloader:** Limine (protocolo Limine). Entrega el mapa de memoria, el framebuffer y los módulos (initrd) sin pelear con el modo real.
- **Lenguaje:** C (freestanding, C11) más un poco de ensamblador (NASM). Como alternativa, Rust `no_std`.
- **Toolchain (Homebrew):** `qemu nasm xorriso x86_64-elf-gcc x86_64-elf-binutils make`
- **Kernel:** monolítico y modular, con una carpeta por subsistema.
- **Ejecución:** `make run` abre la ventana de QEMU. `make test` corre headless con `-serial stdio -device isa-debug-exit` y un timeout.

## 3. Estructura del repo

```
myOS/
├── CLAUDE.md          # reglas para Claude: build, test y convenciones
├── PLAN.md
├── Makefile
├── limine.conf
├── kernel/
│   ├── arch/x86_64/   # GDT, IDT, ISR, paginación, entrada en ASM
│   ├── mm/            # PMM, VMM, heap
│   ├── drivers/       # serial, PS/2, PIT, RTC, framebuffer, ATA/virtio, red
│   ├── gfx/           # primitivas, fuentes, compositor
│   ├── wm/            # gestor de ventanas, dock, menús
│   ├── fs/            # VFS, initrd (tar), FAT32
│   ├── proc/          # scheduler, procesos, syscalls, ELF
│   ├── net/           # ethernet, ARP, IP, UDP, DNS, TCP, HTTP
│   └── lib/           # string, printf, estructuras
├── user/              # apps en ring 3 y una libc mínima
├── assets/            # fuentes, iconos, wallpapers
└── tests/             # scripts de arranque y comprobación
```

## 4. Hoja de ruta por fases

Cada fase se cierra con `make test` en verde, una captura revisada y un commit.

| # | Fase | Entregable verificable |
|---|---|---|
| 0 | Setup | Toolchain instalado, git, CLAUDE.md, Makefile, `make test` funcionando |
| 1 | Arranque | Limine carga el kernel, que escribe `myOS booting...` por serie y pinta la pantalla |
| 2 | Núcleo de CPU | GDT, IDT, excepciones con volcado de registros, PIC/APIC, timer PIT |
| 3 | Memoria | PMM (bitmap), paginación de 4 niveles, heap del kernel (`kmalloc`/`kfree`) |
| 4 | Entrada | Teclado y ratón PS/2, RTC, consola de texto en el framebuffer |
| 5 | Gráficos | Doble buffer, rectángulos, alpha, fuente bitmap/PSF, imágenes (TGA/BMP) |
| 6 | Identidad visual | Sistema de diseño de myOS: paleta, tipografía, iconos, wallpaper, modos claro y oscuro |
| 7 | Window manager | Ventanas arrastrables, foco, z-order, barra superior, dock y menú de sistema |
| 8 | Multitarea | Hilos del kernel, scheduler round-robin, mutex, sleep |
| 9 | Userspace | Ring 3, syscalls, cargador ELF, libc mínima; las apps son procesos separados |
| 10 | Sistema de archivos | VFS, initrd en tar, disco ATA/virtio-blk, FAT32 de lectura y escritura |
| 11 | Apps base | Terminal/shell, Archivos, Editor de texto, Reloj, Ajustes, Acerca de |
| 12 | Red | e1000/virtio-net, ARP, IPv4, UDP, DHCP, DNS, TCP, HTTP y navegador de texto |
| 13 | TLS + navegador | BearSSL portado, HTTPS nativo, render HTML básico con estilos simples |
| 14 | Pulido | Apagado y reinicio por ACPI, animaciones, rendimiento, instalador USB |
| 15 | Opcional | Port a aarch64 (UTM y HVF en el M3), SMP, audio, hardware real |

Hitos: **MVP visual** al terminar la fase 7. **Paridad con el video** al terminar la fase 12. **myOS supera a AuroraOS** al terminar las fases 9 y 13.

## 5. Cómo trabajar con Claude Code

1. **CLAUDE.md desde el día 1:** comandos de build y test, reglas como "nunca romper `make test`", estilo de C, cómo leer los logs serie y cómo tomar capturas.
2. **Una fase por sesión:** primero modo Plan, luego la implementación, `make test`, la revisión de la captura y un commit. Usar `/context-save` al cerrar.
3. **Prompts acotados:** "Implementa la fase 3 según PLAN.md. Escribe tests de arranque que verifiquen X por serie."
4. **Depuración:** `qemu -d int,cpu_reset -no-reboot` más GDB (`-s -S`) para triple faults.
5. **Revisiones:** `/code-review` antes de cerrar cada fase crítica (memoria, scheduler y red).

## 6. Riesgos

- **Triple faults y bugs de memoria:** la fase 2 incluye un buen handler de excepciones y logging por serie.
- **Emulación x86 lenta en el M3:** suficiente para un OS hobby. Si se vuelve un problema, se adelanta el port a aarch64.
- **Alcance:** el navegador "real" (HTML, CSS, JS) no tiene fin. Se limita a HTML básico.
