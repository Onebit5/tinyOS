; where ring 3 comes in.
;
; `syscall` is fast because it does almost nothing: it puts the return
; address in rcx, the flags in r11, loads cs/ss from STAR and rip from
; LSTAR, and that is the whole of it. in particular it does NOT change
; rsp -- we arrive on the *user's* stack, in ring 0, which is as
; alarming as it sounds. the first job is to get off it.
;
; the swap uses two globals. that is only safe because SFMASK clears IF,
; so we arrive with interrupts off and nothing can preempt us in the
; three instructions before the user's rsp is safely on a kernel stack.
; the moment it is, we can let interrupts back in.
;
; syscall_kernel_rsp is kept pointing at the running thread's kernel
; stack by the scheduler, the same way the tss rsp0 is.

bits 64
section .text

extern syscall_dispatch
extern syscall_kernel_rsp

global syscall_entry
global syscall_scratch_rsp

syscall_entry:
    mov [rel syscall_scratch_rsp], rsp      ; park the user stack briefly
    mov rsp, [rel syscall_kernel_rsp]       ; and stand on our own

    push qword [rel syscall_scratch_rsp]    ; now it is per-thread, on
                                            ; our stack, and the global
                                            ; is free for the next caller
    push rcx                                ; user rip, courtesy of syscall
    push r11                                ; user rflags, likewise

    ; ring 3 gets every register back except rax, which carries the
    ; result, and rcx and r11, which the syscall instruction itself
    ; destroyed on the way in. user code is compiled against exactly
    ; that promise, so anything else coming back changed is kernel
    ; state leaking into a program that will happily use it as a
    ; pointer.
    ;
    ; the caller-saved ones are ours to keep, because the C below may
    ; clobber them and the shuffle certainly does. rbx, rbp and r12-r15
    ; need no saving here: syscall_dispatch is an ordinary C function
    ; and the abi makes preserving those its problem, not ours.
    push rdi
    push rsi
    push rdx
    push r10
    push r8
    push r9

    sti                                     ; safe now, we are on our own stack

    ; two calling conventions meet here and they are NOT the same one.
    ;
    ;   ring 3 hands us:  nr=rax  a0=rdi a1=rsi a2=rdx a3=r10 a4=r8
    ;   sysv C wants:     arg1=rdi arg2=rsi arg3=rdx arg4=rcx arg5=r8 arg6=r9
    ;
    ; and syscall_dispatch's first argument is the number, so everything
    ; shifts one place right to make room for it. (r10 rather than rcx
    ; on the way in because the syscall instruction ate rcx for the
    ; return address.)
    ;
    ; the order below matters: each register is read before anything
    ; overwrites it. do it the other way round and arguments quietly
    ; become copies of each other.
    mov r9, r8                              ; a4
    mov r8, r10                             ; a3
    mov rcx, rdx                            ; a2
    mov rdx, rsi                            ; a1
    mov rsi, rdi                            ; a0
    mov rdi, rax                            ; and the number itself

    call syscall_dispatch                   ; returns its result in rax

    cli                                     ; nothing may interrupt the unwind

    pop r9
    pop r8
    pop r10
    pop rdx
    pop rsi
    pop rdi                                 ; rax is left alone: it is the result

    pop r11                                 ; flags to restore
    pop rcx                                 ; address to return to
    pop rsp                                 ; back onto the user's stack

    o64 sysret                              ; and back to ring 3

section .bss
    align 8
syscall_scratch_rsp:
    resq 1

section .note.GNU-stack noalloc noexec nowrite progbits
