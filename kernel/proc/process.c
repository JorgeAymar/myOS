// User processes: ELF64 loader, ring 3 entry, exit and fault handling.
// A process is (for now) one thread plus a private address space.
#include <stdint.h>
#include <stddef.h>
#include "proc.h"
#include "proc_internal.h"
#include "arch.h"
#include "mm.h"
#include "string.h"
#include "kprintf.h"
#include "panic.h"

#define USER_STACK_TOP   0x00007ffffffff000ull
#define USER_STACK_PAGES 4

struct process {
    uint64_t pml4;
    uint64_t entry;
    long arg;
    int *exit_code_out;
};

void enter_user(uint64_t rip, uint64_t rsp, uint64_t arg);

// ------------------------------------------------------------------- ELF ----

#define PT_LOAD 1
#define PF_X 1
#define PF_W 2

struct elf64_ehdr {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf64_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

static bool load_elf(uint64_t pml4, const uint8_t *img, size_t size, uint64_t *entry) {
    if (size < sizeof(struct elf64_ehdr)) return false;
    const struct elf64_ehdr *eh = (const void *)img;
    if (memcmp(eh->ident, "\x7f" "ELF", 4) || eh->ident[4] != 2 /* 64-bit */ ||
        eh->ident[5] != 1 /* LE */ || eh->type != 2 /* EXEC */ || eh->machine != 0x3e)
        return false;
    if (eh->phentsize != sizeof(struct elf64_phdr) ||
        eh->phoff > size || (uint64_t)eh->phnum * sizeof(struct elf64_phdr) > size - eh->phoff)
        return false;

    const struct elf64_phdr *ph = (const void *)(img + eh->phoff);
    for (int i = 0; i < eh->phnum; i++) {
        const struct elf64_phdr *p = &ph[i];
        if (p->type != PT_LOAD || p->memsz == 0) continue;
        if (p->filesz > p->memsz || p->offset > size || p->filesz > size - p->offset) return false;
        if (p->vaddr < PAGE_SIZE || p->vaddr >= USER_TOP || p->memsz > USER_TOP - p->vaddr) return false;
        uint64_t start = p->vaddr & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t end = p->vaddr + p->memsz;
        for (uint64_t va = start; va < end; va += PAGE_SIZE) {
            uint8_t *page = uvm_alloc(pml4, va, p->flags & PF_W, p->flags & PF_X);
            if (!page) return false;
            // Copy the part of [vaddr, vaddr+filesz) that falls in this page.
            uint64_t lo = va > p->vaddr ? va : p->vaddr;
            uint64_t hi = va + PAGE_SIZE;
            if (hi > p->vaddr + p->filesz) hi = p->vaddr + p->filesz;
            if (lo < hi) memcpy(page + (lo - va), img + p->offset + (lo - p->vaddr), hi - lo);
        }
    }
    if (eh->entry < PAGE_SIZE || eh->entry >= USER_TOP) return false;
    *entry = eh->entry;
    return true;
}

// ------------------------------------------------------------- lifecycle ----

static void user_thread_main(void *arg) {
    struct process *p = arg;
    cpu_cli();
    struct thread *t = sched_current;
    t->proc = p;
    t->cr3 = p->pml4;
    t->kstack_top = (uint64_t)t->stack + THREAD_STACK_SIZE;
    arch_set_kernel_stack(t->kstack_top);
    syscall_kernel_rsp = t->kstack_top;
    uvm_sync_kernel(p->pml4);
    __asm__ volatile ("mov %0, %%cr3" :: "r"(p->pml4) : "memory");
    enter_user(p->entry, USER_STACK_TOP - 8, (uint64_t)p->arg);
}

tid_t process_spawn(const char *name, const void *elf, size_t size, long arg, int *exit_code) {
    preempt_disable();                  // heap/PMM are not locked yet
    struct process *p = kmalloc(sizeof *p);
    uint64_t pml4 = p ? uvm_create() : 0;
    bool ok = pml4 && load_elf(pml4, elf, size, &p->entry);
    for (int i = 1; ok && i <= USER_STACK_PAGES; i++)
        ok = uvm_alloc(pml4, USER_STACK_TOP - i * PAGE_SIZE, true, false) != NULL;
    preempt_enable();
    if (!ok) goto fail;
    p->pml4 = pml4;
    p->arg = arg;
    p->exit_code_out = exit_code;
    tid_t tid = thread_create(name, user_thread_main, p);
    if (tid >= 0) return tid;
fail:
    preempt_disable();
    if (pml4) uvm_destroy(pml4);
    if (p) kfree(p);
    preempt_enable();
    return -1;
}

void process_exit(int code) {
    struct thread *t = sched_current;
    struct process *p = t->proc;
    if (!p) panic("process_exit: '%s' is not a user process", t->name);
    if (p->exit_code_out) *p->exit_code_out = code;
    thread_exit();
}

// Reaper side (interrupts disabled, CR3 is not this process's).
void process_free(struct thread *t) {
    struct process *p = t->proc;
    if (!p) return;
    uvm_destroy(p->pml4);
    kfree(p);
    t->proc = NULL;
}

static void user_fault(uint64_t vector, uint64_t rip, uint64_t error) {
    struct thread *t = sched_current;
    if (!t->proc) return;               // not ours: let arch panic
    kprintf("[proc] killed '%s' (pid %d): exception %lu at 0x%lx (error 0x%lx)\n",
            t->name, t->tid, vector, rip, error);
    process_exit(PROC_EXIT_KILLED);
}

void process_init(void) {
    uvm_init(sched_kernel_cr3());
    arch_enable_syscall(syscall_entry);
    arch_set_user_fault_hook(user_fault);
}
