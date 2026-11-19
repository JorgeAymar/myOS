; Ring 3 programs embedded in the kernel image (built by mk/proc.mk).
bits 64
section .rodata
align 16
global user_hello_elf, user_hello_elf_end
user_hello_elf:
    incbin "build/user/hello.elf"
user_hello_elf_end:

section .note.GNU-stack noalloc noexec nowrite progbits
