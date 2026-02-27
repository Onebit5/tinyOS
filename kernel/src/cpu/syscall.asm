; where ring 3 comes in.
;
; `syscall` is fast because it does almost nothing: it puts the return
; address in rcx, the flags in r11, loads cs/ss from STAR and rip from
; LSTAR, and that is the whole of it. in particular it does NOT change
; rsp -- I arrive on the *user's* stack, in ring 0, which is as
; alarming as it sounds. the first job is to get off it.
;
; the swap used to use two globals, which was safe because SFMASK clears
; IF -- I arrive with interrupts off, and nothing could preempt me in the
; three instructions before the user's rsp was on a kernel stack.
;
; that reasoning was airtight and is now wrong. it says nothing about
; another core, and with four of them a global kernel rsp means a thread
; can `syscall` its way onto some other core's stack.
;
; so the two words are per core, found through gs -- which can be read
; without clobbering a single register, and that matters here more than
; anywhere: rax holds the call number, rcx and r11 hold the return
; address and flags sysret needs, everything else holds arguments, and
; there is no stack yet to save anything on.
;
;   gs:0   this core's kernel stack, kept current by the scheduler
;   gs:8   somewhere to park the user's stack for two instructions
;
; there is no swapgs here, and that is deliberate. the usual arrangement
; is one gs base for ring 3 and another for ring 0, exchanged on the way
; in and out -- but the *parity* of those exchanges is then per thread,
; while the bases are per core. a thread preempted inside a syscall and
; resumed on another core does its exit swap on a core that never did
; the entry one, which leaves that core's two bases the wrong way round
; and the next syscall on it reading through a base of zero. that is not
; a hypothetical: it is what this stub did on its second call.
;
; so gs simply names the current core, in both rings, always. nothing to
; keep in step and nothing to get wrong across a migration.
;
; the price is that anything loading a real selector into gs zeroes the
; base, and the next system call then writes through zero and faults in
; the kernel. that is not hypothetical either: the trip into ring 3 used
; to do exactly that, one instruction at a time, on every program that
; ever ran. see usermode.asm. every program here is one I compiled and
; none of them touch gs, so what is left is a program able to crash the
; machine rather than escape it -- a real edge, worth naming.

bits 64
section .text

extern syscall_dispatch

global syscall_entry

syscall_entry:
    mov [gs:8], rsp                         ; park the user stack briefly
    mov rsp, [gs:0]                         ; and stand on my own

    push qword [gs:8]                       ; now it is per-thread, on my
                                            ; own stack, and the per-core
                                            ; word is free again
    push rcx                                ; user rip, courtesy of syscall
    push r11                                ; user rflags, likewise

    ; ring 3 gets every register back except rax, which carries the
    ; result, and rcx and r11, which the syscall instruction itself
    ; destroyed on the way in. user code is compiled against exactly
    ; that promise, so anything else coming back changed is kernel
    ; state leaking into a program that will happily use it as a
    ; pointer.
    ;
    ; the caller-saved ones are mine to keep, because the C below may
    ; clobber them and the shuffle certainly does. rbx, rbp and r12-r15
    ; need no saving here: syscall_dispatch is an ordinary C function
    ; and the abi makes preserving those its problem, not mine.
    push rdi
    push rsi
    push rdx
    push r10
    push r8
    push r9

    sti                                     ; safe now, I am on my own stack

    ; two calling conventions meet here and they are NOT the same one.
    ;
    ;   ring 3 hands me:  nr=rax  a0=rdi a1=rsi a2=rdx a3=r10 a4=r8
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

section .note.GNU-stack noalloc noexec nowrite progbits
