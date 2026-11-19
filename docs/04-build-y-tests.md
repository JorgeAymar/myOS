# 4. Build, tests y depuración

## Toolchain

```bash
brew install qemu nasm xorriso x86_64-elf-gcc x86_64-elf-binutils mtools
```

| Herramienta | Para qué |
|---|---|
| `x86_64-elf-gcc` 16 / `x86_64-elf-ld` | Compilador cruzado y enlazador (no se usa el clang del Mac) |
| `nasm` | Rutinas de interrupción en ensamblador |
| `xorriso` | Generar la ISO híbrida BIOS + UEFI |
| `mtools` | Crear la imagen FAT32 `build/disk.img` sin montarla |
| `qemu-system-x86_64` | Emular un PC x86_64 (máquina q35) en el Mac ARM |
| Limine v9 | Bootloader; se descarga solo en `limine/` |

Opciones del kernel: `-ffreestanding -mcmodel=kernel -mno-red-zone -mno-sse -mno-mmx -mno-80387 -Werror`. El enlace usa `-z noexecstack` y [kernel/linker.ld](../kernel/linker.ld), que coloca el kernel en `0xffffffff80000000`.

## Comandos

| Comando | Qué hace |
|---|---|
| `make` | Compila el kernel y genera `build/myos.iso` |
| `make run` / `make run-uefi` | Abre QEMU con ventana (BIOS o UEFI). La imagen se escala al redimensionar (`zoom-to-fit`) |
| `make test` | Test automático sin pantalla en BIOS **y** UEFI |
| `make screenshot` | Captura PNG del escritorio en `build/screenshot.png` |
| `make disk` | Regenera `build/disk.img` desde cero |
| `make debug` | Arranca en pausa con GDB en `:1234` y registra interrupciones y reinicios |

## Protocolo de tests

1. `make test` genera una **ISO de test** con `limine-test.conf`, cuya línea de comandos es `test`.
2. QEMU arranca sin pantalla, con la salida serie en `stdio` y el dispositivo `isa-debug-exit` en el puerto `0xf4`.
3. Cada subsistema hace sus autotests e imprime `[OK] ...`. Si algo falla, `panic()` imprime `[FAIL] ...` y sale con el código 35.
4. Tras pintar unos cuantos frames del escritorio, el kernel imprime `[TEST] boot OK` y escribe `0x10` en el puerto `0xf4`. QEMU termina entonces con el código **33**.
5. [tests/boot_test.sh](../tests/boot_test.sh) da `PASS` solo si el código es 33 **y** el log contiene `[TEST] boot OK`. Lo hace para BIOS y para UEFI (con OVMF), y deja los logs en `build/serial-{bios,uefi}.log`.

La regla del proyecto es que **ninguna fase se cierra con `make test` en rojo**.

## Capturas automáticas

[tests/screenshot.sh](../tests/screenshot.sh) arranca la ISO normal sin pantalla, espera `WAIT` segundos y pide la captura a QEMU por **QMP**, su protocolo de control en JSON (`screendump` en formato PNG). Claude lee esa imagen para revisar los cambios visuales sin que nadie mire la pantalla.

> Nota: la consola de texto de QEMU (HMP) a través de un socket es poco fiable para esto, porque repite cada tecla y falla en silencio. Por eso se usa QMP.

## Depuración

- **Triple fault o reinicio inesperado:** usa `make debug`, que añade `-d int,cpu_reset`, y revisa la última excepción del log.
- **GDB:** con `x86_64-elf-gdb build/kernel.elf`, ejecuta `target remote :1234`.
- **Excepciones:** el volcado de registros sale por serie; un fallo de página indica la dirección (CR2) y si era una lectura o escritura, en modo usuario o kernel.

## Integración continua local

Antes de cada commit se ejecuta `make clean && make && make test`. Cada merge de un agente se valida igual antes de hacer push a GitHub.
