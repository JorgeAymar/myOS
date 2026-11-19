# 5. Metodología multi-agente

myOS se construye con **Claude Code** como agente principal (el integrador) y varios **subagentes** que trabajan en paralelo.

## Principios

1. **Primero los contratos, después el paralelismo.** Antes de lanzar agentes, el integrador escribe los headers de `kernel/include/`, las implementaciones de reserva (`weak_stubs.c`) y el orden de arranque en `kmain`. Con eso, cada módulo se puede desarrollar sin esperar a los demás.
2. **Un worktree de git por agente.** Cada agente trabaja en su propia copia del repo (`.claude/worktrees/agent-*`) y su propia rama, así que nadie pisa los archivos de otro.
3. **Propiedad exclusiva de carpetas.** Cada agente solo puede crear y editar archivos en su carpeta (`kernel/mm/`, `kernel/net/`...). Los contratos solo admiten añadidos.
4. **El test es la definición de terminado.** Ningún agente entrega sin `make test` en verde en BIOS y UEFI, y sin sus líneas `[OK]` en el log.
5. **La integración la hace una sola persona.** El agente principal fusiona cada rama, recompila desde cero, corre los tests y solo entonces hace push.

## Rondas de trabajo

### Ronda 1: núcleo y escritorio (4 agentes)

| Agente | Carpeta | Resultado |
|---|---|---|
| arch | `kernel/arch/x86_64/` | GDT/TSS, IDT, excepciones, PIC, PIT ✅ |
| mm | `kernel/mm/` | PMM, VMM, heap ✅ |
| drivers | `kernel/drivers/` | PS/2 teclado + ratón, RTC ✅ |
| ui | `kernel/gfx/`, `wm/`, `apps/` | Compositor, ventanas, escritorio estilo macOS ✅ |

### Ronda 2: subsistemas (3 agentes, más ui en curso)

Antes de lanzarla, el integrador añadió el bus **PCI** y `mmio_map()`, que comparten disco y red, los puntos de entrada `sched_init`, `fs_init` y `net_init`, y la extensión `mk/*.mk` del Makefile.

| Agente | Carpeta | Resultado |
|---|---|---|
| net | `kernel/net/`, `drivers/e1000.c` | e1000, ARP, IP, ICMP, UDP, DHCP, DNS, TCP, HTTP ✅ |
| proc | `kernel/proc/` | Hilos, scheduler con desalojo, sincronización y ring 3 con syscalls y ELF ✅ |
| fs | `kernel/fs/`, `drivers/ahci.c` | VFS, initrd, AHCI, FAT32 con nombres largos ✅ |

Al integrar proc, el agente avisó de que el heap no tenía protección contra el desalojo. El integrador la añadió (secciones con interrupciones desactivadas en el PMM, el VMM y el heap) antes de seguir.

Lecciones de la integración de fs:
- Una regla dentro de `mk/*.mk` pasa a ser el objetivo por defecto de `make`, por eso cada `.mk` fija `.DEFAULT_GOAL := all`.
- SeaBIOS intentaba arrancar desde el disco FAT32, por eso se añadió `-boot order=d`.

## Instrucciones para cada agente

Cada agente recibe instrucciones autocontenidas con:

- qué leer primero (CLAUDE.md, PLAN.md, los contratos que consume),
- su ámbito exacto y lo que **no** puede tocar,
- qué entregar y qué autotests escribir,
- cómo verificar (`make test` y, en la UI, también capturas),
- cómo cerrar: un commit en su rama y un informe con los archivos, la API y el log.

## Incidencias y cómo se resolvieron

- **Caída de conexión con la API:** dos agentes (net y proc) se cortaron por un error de red. Como su trabajo estaba en su worktree, se reanudaron con un mensaje y siguieron donde se quedaron, sin perder nada.
- **Cambio de prioridad:** el usuario pidió ver el OS corriendo antes que las apps, y que se pareciera a macOS. Se envió el cambio al agente de UI sin detenerlo.
- **Vista previa sin esperar:** para enseñar el escritorio antes de que UI terminara, se combinó su trabajo en curso con `main` en un worktree temporal, fuera del repo, y se abrió en QEMU.
