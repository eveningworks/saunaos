; SYSCALL's entry and SYSRET's exit: the fast way into the same
; isr_dispatch() `int 0x80` reaches. idt.c's syscall_insn_init() points
; MSR_LSTAR here.
;
; THE FRAME IS `int 0x80`'s, FIELD FOR FIELD -- ss, rsp, rflags, cs, rip,
; error 0, vector 128, then the fifteen registers in isr_common's order
; -- so signals, restarts, exec, strace and the crash report never learn
; which instruction a call came in by. Linux's entry_SYSCALL_64 builds
; its pt_regs the same way for the same reason.
;
; **THE TRAP: FOR A FEW INSTRUCTIONS AT EACH END, RING 0 RUNS ON THE
; USER'S STACK.** SYSCALL does not switch stacks, so until the second
; `mov` below RSP is whatever ring 3 left in it, and SYSRET needs it put
; back before the last instruction. IF is clear in both windows
; (MSR_FMASK, and the `cli`), which leaves only an NMI able to arrive --
; it runs on its own IST stack for exactly this (idt.c). A kernel
; debugger single-stepping through these lines steps on the user's stack.
;
; LEAVING: SYSRET only when the frame is still what SYSCALL made it.
; Anything that rewrote it -- a signal, sigreturn, exec, a restart's
; rewind -- leaves RCX != RIP and goes out through iretq, which restores
; every register. Also iretq: RF or TF set (SYSRET cannot restore RF,
; and restoring TF traps at once in ring 3), and a RIP outside the user
; half, because SYSRET to a non-canonical address faults IN RING 0 on
; Intel with the user's RSP loaded (CVE-2012-0217).
bits 64

extern isr_dispatch
extern isr_resume_frame
extern gdt_tss

%define SEL_USER_DATA 0x1b   ; gdt.h's SEL_USER_DATA, RPL 3
%define SEL_USER_CODE 0x23   ; gdt.h's SEL_USER_CODE, RPL 3
%define TSS_RSP0      4      ; architectural: rsp0 follows a 4-byte reserved field
%define RFLAGS_TF_RF  0x10100

; Frame slots, in 8-byte words from RSP once everything is pushed --
; the same indices as scheduler.h's SCHED_TF_*.
%define F_R11    4
%define F_RCX    12
%define F_RIP    17
%define F_CS     18
%define F_RFLAGS 19
%define F_SS     21

section .bss
align 8
; Ring 3's RSP for the two instructions between arriving and having a
; stack to push it on. One CPU, IF clear: nothing else can be here.
syscall_user_rsp: resq 1

section .text

global syscall_entry
syscall_entry:
    mov [rel syscall_user_rsp], rsp
    mov rsp, [rel gdt_tss + TSS_RSP0]

    push qword SEL_USER_DATA          ; ss
    push qword [rel syscall_user_rsp] ; rsp
    push r11                          ; rflags, as SYSCALL saved them
    push qword SEL_USER_CODE          ; cs
    push rcx                          ; rip, as SYSCALL saved it
    push qword 0                      ; error code
    push qword 128                    ; vector: a syscall, as for int 0x80

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

    mov rdi, rsp
    call isr_dispatch

    cli
    mov rcx, [rsp + F_RCX*8]
    cmp rcx, [rsp + F_RIP*8]
    jne syscall_exit_iret
    mov r11, [rsp + F_R11*8]
    cmp r11, [rsp + F_RFLAGS*8]
    jne syscall_exit_iret
    test r11, RFLAGS_TF_RF
    jnz syscall_exit_iret
    shr rcx, 47                       ; above the user half?
    jnz syscall_exit_iret
    cmp qword [rsp + F_CS*8], SEL_USER_CODE
    jne syscall_exit_iret
    cmp qword [rsp + F_SS*8], SEL_USER_DATA
    jne syscall_exit_iret

; From here RSP stops pointing at a whole frame (tools/gdb/toyos.py's
; unwinder claims syscall_entry only up to this label).
global syscall_exit_pops
syscall_exit_pops:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11                           ; == rflags, checked above
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx                           ; == rip, checked above
    pop rbx
    pop rax
    add rsp, 16                       ; vector + error code
    mov rsp, [rsp + 24]               ; the frame's rsp: rip 0, cs 8, rflags 16
    o64 sysret

syscall_exit_iret:
    jmp isr_resume_frame
