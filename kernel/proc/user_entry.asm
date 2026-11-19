; Ring 3 entry/exit paths: SYSCALL entry stub and the first IRETQ to user mode.
bits 64
default rel

extern syscall_dispatch
extern syscall_kernel_rsp

section .bss
syscall_user_rsp: resq 1      ; scratch: IF is off and we are uniprocessor

section .text

; SYSCALL: rcx = user rip, r11 = user rflags, IF/DF cleared by FMASK, still on
; the user stack. Builds a struct syscall_frame (see syscall.c) on the thread's
; kernel stack: 10 qwords keeps the stack 16-byte aligned for the C call.
global syscall_entry
syscall_entry:
    mov [syscall_user_rsp], rsp
    mov rsp, [syscall_kernel_rsp]
    push qword [syscall_user_rsp]
    push r11
    push rcx
    push r9
    push r8
    push r10
    push rdx
    push rsi
    push rdi
    push rax
    mov rdi, rsp
    call syscall_dispatch     ; returns with interrupts disabled, result in rax
    add rsp, 8
    pop rdi
    pop rsi
    pop rdx
    pop r10
    pop r8
    pop r9
    pop rcx
    pop r11
    pop rsp
    o64 sysret

; void enter_user(uint64_t rip, uint64_t rsp, uint64_t arg) -- never returns.
; Call with interrupts disabled; RFLAGS.IF is set by the IRETQ.
global enter_user
enter_user:
    push qword 0x1b           ; SS  = user data | RPL3
    push rsi                  ; RSP
    push qword 0x202          ; RFLAGS: IF
    push qword 0x23           ; CS  = user code | RPL3
    push rdi                  ; RIP
    mov rdi, rdx              ; first argument of _start
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    iretq

section .note.GNU-stack noalloc noexec nowrite progbits
