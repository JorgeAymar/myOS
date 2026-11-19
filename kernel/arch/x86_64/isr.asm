; Interrupt entry stubs for all 256 vectors and the common save/restore path.
; Every stub leaves the stack as [vector][error code][CPU frame] so the C side
; sees one uniform struct interrupt_frame (see interrupt.h).
bits 64
default rel
section .text

extern interrupt_dispatch

%macro ISR 1
isr_stub_%1:
%if (%1 == 8) || (%1 == 10) || (%1 == 11) || (%1 == 12) || (%1 == 13) || (%1 == 14) || (%1 == 17) || (%1 == 21) || (%1 == 29) || (%1 == 30)
    push qword %1         ; CPU already pushed an error code
%else
    push qword 0          ; dummy error code
    push qword %1
%endif
    jmp isr_common
%endmacro

%assign i 0
%rep 256
ISR i
%assign i i+1
%endrep

isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    cld
    mov rdi, rsp              ; struct interrupt_frame * (stack is 16-aligned here)
    call interrupt_dispatch
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16               ; vector + error code
    iretq

section .rodata
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 256
    dq isr_stub_ %+ i
%assign i i+1
%endrep

section .note.GNU-stack noalloc noexec nowrite progbits
