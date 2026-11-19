; Kernel thread context switch. Only callee-saved registers are saved: every
; switch happens through a C call (schedule()), so the caller-saved ones are
; already dead or spilled. No FPU/SSE state (kernel is built -mno-sse).
bits 64
default rel
section .text

extern thread_start

; void context_switch(uint64_t *save_rsp, uint64_t load_rsp)
global context_switch
context_switch:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; First "return address" of a new thread. thread_create puts entry in r12 and
; arg in r13; thread_start(entry, arg) enables interrupts and never returns.
global thread_trampoline
thread_trampoline:
    mov rdi, r12
    mov rsi, r13
    xor ebp, ebp
    and rsp, -16
    call thread_start
    ud2

section .note.GNU-stack noalloc noexec nowrite progbits
