; the context switch. this is the whole magic trick of a scheduler and
; its 16 instructions long.
;
;   switch_context(uint64_t *save_rsp, uint64_t *load_rsp)
;   rdi = where to stash the outgoing thread's rsp
;   rsi = where to read the incoming thread's rsp from
;
; we only touch the callee-saved registers, because the sysv abi already
; says a function call may clobber the rest -- whoever called us has
; either saved rax/rcx/etc or doesnt care about them. that makes the
; parked state of a thread just: six registers and a return address,
; sitting on its own stack.
;
; rflags is deliberately NOT saved. threads that got preempted resume
; inside an irq handler and get their flags back from iretq; threads
; that yielded voluntarily get theirs back from irq_restore(); brand
; new threads sti for themselves in the bootstrap. see thread.c

bits 64
section .text

global switch_context

switch_context:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp      ; outgoing thread is now fully described by its rsp
    mov rsp, [rsi]      ; and here we become somebody else entirely

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret                 ; returns wherever the incoming thread left off

section .note.GNU-stack noalloc noexec nowrite progbits
