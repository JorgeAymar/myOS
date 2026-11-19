# 1. Visión y estrategia

## Origen

myOS nace a partir del video *"I Forced AI to Build a Custom Operating System (It Actually Worked)"* (canal The Render Guy). En él se le pide a Claude Opus que construya un sistema operativo completo en dos prompts:

- **Primer prompt:** un OS en C y ensamblador para x86, con GRUB, interfaz parecida a macOS y una ISO arrancable. Claude lo planifica en 7 fases (arranque, kernel, entrada, gráficos, gestor de ventanas, apps y pulido) y lo prueba en QEMU. En unos 50 minutos salen unas 6.000 líneas y 88 archivos. El resultado se llama AuroraOS.
- **Segundo prompt:** un navegador de texto. Claude escribe el driver de red y una pila Ethernet/IP/DNS/TCP/HTTP. El HTTPS lo resuelve un programa auxiliar que corre en el Mac, no el propio OS.
- **Problemas:** no arrancó en VirtualBox porque el Mac del autor es ARM y el OS es x86, así que se ejecutó en QEMU. La nota final fue 6,5/10.

## Qué hacemos distinto

| En el video | En myOS |
|---|---|
| Un prompt gigante, todo de golpe | Fases pequeñas, cada una con un test automático que pasa y un commit en git |
| GRUB en modo BIOS, pensado para VirtualBox | **Limine** (64 bits y framebuffer desde el arranque), BIOS **y** UEFI, en **QEMU** |
| Verificación "a ojo" | `make test` sin pantalla, con logs por serie y capturas que Claude revisa |
| HTTPS hecho en el Mac anfitrión | Pila de red propia; TLS dentro del OS en una fase posterior |
| Todo dentro del kernel | Multitarea y procesos aislados en ring 3 (fases 8 y 9) |
| Un solo agente | Varios agentes en paralelo con contratos entre módulos |

## Objetivos

1. **Primer escritorio usable (v0.1):** arranca, maneja teclado y ratón, y muestra un escritorio con ventanas.
2. **Aspecto inspirado en macOS:** barra de menú, dock translúcido, botones de semáforo y ventanas redondeadas. Todo se dibuja por código o sale de fuentes con licencia libre. No se usa nada de Apple: ni su logo, ni sus iconos, wallpapers, fuentes o nombres.
3. **Superar al video:** procesos aislados, disco con FAT32, red propia y HTTPS sin ayuda del anfitrión.

## Decisiones técnicas

- **Arquitectura x86_64.** Es la mejor documentada (OSDev wiki) y QEMU la emula bien en el Mac M3. Un port a aarch64 queda como fase opcional.
- **Bootloader Limine (v9) con su protocolo.** Entrega el mapa de memoria, el mapeo directo de la memoria física (HHDM), el framebuffer y la línea de comandos sin pasar por el modo real.
- **Lenguaje:** C11 freestanding, más NASM para las rutinas de interrupción.
- **Toolchain:** `x86_64-elf-gcc` y `x86_64-elf-binutils` de Homebrew, nunca el clang del Mac.
- **Kernel monolítico y modular,** con una carpeta por subsistema.
- **Sin SSE, MMX ni x87 en el kernel.** Así las interrupciones no tienen que guardar el estado de la FPU, y el código usa solo matemática entera.
