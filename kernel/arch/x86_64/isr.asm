bits 64

extern isr_dispatch
; isr_dispatch returns WHERE TO RESUME, in rax -- see idt.c's wrapper and
; scheduler.c's design comment. It used to be read out of the global
; g_next_kernel_rsp instead, which a nested interrupt could clobber; the
; global is still how scheduler.c asks for a context switch, but the
; answer now rides the C stack, one copy per call.

section .text

global idt_load
idt_load:
    lidt [rdi]
    ret

%macro ISR_NOERR 1
global isr%1
isr%1:
    push qword 0        ; dummy error code
    push qword %1        ; vector number
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push qword %1        ; vector number (error code already pushed by CPU)
    jmp isr_common
%endmacro

; The vectors that can land where the saved CS does not say whose GS is
; loaded -- see isr_paranoid below.
%macro ISR_PARANOID_NOERR 1
global isr%1
isr%1:
    push qword 0
    push qword %1
    jmp isr_paranoid
%endmacro

%macro ISR_PARANOID_ERR 1
global isr%1
isr%1:
    push qword %1
    jmp isr_paranoid
%endmacro

%define MSR_GS_BASE 0xC0000101
%define F_CS        18   ; the saved CS, in words from RSP once all is pushed

; Exceptions 0-31. 8, 10-14, 17, 21, 29, 30 push an error code automatically.
ISR_NOERR 0
ISR_PARANOID_NOERR 1    ; #DB
ISR_PARANOID_NOERR 2    ; NMI
ISR_PARANOID_NOERR 3    ; #BP -- a kdebug breakpoint can sit in an entry window
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_PARANOID_ERR 8     ; #DF
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_PARANOID_NOERR 18   ; #MC
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

; IRQs remapped to 32-47, then the MSI vectors at 48-63 (lapic.h's
; LAPIC_VECTOR_BASE..LAST), then 64-71 for the I/O APIC's inputs above
; the ISA range (GSI 16-23, irq.h's irq_vector()). One stub apiece
; either way -- the vector number is what tells isr_dispatch() which
; controller to acknowledge, so the ranges must not overlap and this
; %rep is where that is decided.
%assign i 32
%rep 40
ISR_NOERR i
%assign i i+1
%endrep

; The LAPIC's spurious vector (lapic.h's LAPIC_SPURIOUS_VECTOR). It
; needs a gate for the same reason every vector does -- one delivered
; with no descriptor behind it is a #GP, and the spurious one arrives
; precisely when something has already gone slightly wrong.
ISR_NOERR 251    ; SMP_CALL_VECTOR (smp.h)
ISR_NOERR 252    ; SMP_SCHED_VECTOR (smp.h)
ISR_NOERR 253    ; TLB_FLUSH_VECTOR (tlb.h)
ISR_NOERR 255

isr_common:
    ; GS: ring 3's on an entry FROM ring 3, the kernel's otherwise (the
    ; vectors for which that is not true go through isr_paranoid). The
    ; saved CS is at [rsp + 24] here: vector, error code, rip, cs.
    test byte [rsp + 24], 3
    jnz .swap
    ; ...and one exception to "ring 0 means the kernel's": a fault ON
    ; the iretq below, which is after its swapgs (Linux's error_entry
    ; checks the same). sigreturn refuses the frames that would do it.
    push rax
    lea rax, [rel isr_iretq]
    cmp [rsp + 24], rax              ; the saved rip, one push further down
    pop rax
    jne .kernel_gs
.swap:
    swapgs
.kernel_gs:
    ; On BOTH paths: neither may run ahead on a mispredicted GS
    ; (CVE-2019-1125, Linux's FENCE_SWAPGS_*_ENTRY).
    lfence
    ; Ring 3 may have set the direction flag; the C below assumes it
    ; clear (k_memcpy is `rep movsb`), as the ABI does. The iret frame
    ; holds the interrupted RFLAGS, so this costs the interrupted code
    ; nothing.
    cld
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

    mov rdi, rsp         ; pass pointer to saved state as arg
    call isr_dispatch

    jmp isr_resume_frame ; OVER the entry below, not into it: falling
                          ; through would `mov rsp, rdi` on every
                          ; interrupt return and clobber the frame we
                          ; just came in on.

; void isr_return_to(uint64_t *regs)
;
; Resume a trapframe by name -- the tail of this stub, made callable so
; ordinary C can do what the epilogue does. ONE implementation, two
; entries, the same rule geom.c and klineedit.c are built on. Used to
; start a process that has never run: its context is a hand-built
; kernel_context whose rsp is the trapframe and whose rip is
; isr_resume_frame, so a restore lands in the pops below.
; A FRESH CONTEXT'S FIRST RESUME (proc_start_context()): rsp is its
; frame. It starts holding the kernel lock at depth 1, as every context
; resumes inside the kernel; a ring-3 frame drops it before the iretq.
; Linux's ret_from_fork, which calls schedule_tail() for the same reason.
global isr_start_frame
extern bkl_start_tail
isr_start_frame:
    mov rbx, rsp                     ; scratch: the frame's rbx is popped below
    and rsp, -16
    mov rdi, rbx
    call bkl_start_tail
    mov rsp, rbx
    jmp isr_resume_frame

global isr_return_to
isr_return_to:
    mov rsp, rdi

global isr_resume_frame
isr_resume_frame:
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

    add rsp, 16           ; pop vector number + error code
    ; Back to ring 3 means back to ring 3's GS -- every exit comes
    ; through here, a new process's first one (isr_return_to) included.
    test byte [rsp + 8], 3 ; the frame's cs: rip 0, cs 8
    jz isr_iretq_kernel
    swapgs
isr_iretq:                 ; ONLY the ring-3 return: isr_common's fault
    iretq                  ; guard must not match a ring-0 one, which never swapped
isr_iretq_kernel:
    iretq

; NMI, #DB, #BP, #DF and #MC: Linux's "paranoid" entry. Each can arrive
; in ring 0 with RING 3's GS still loaded -- inside syscall_entry before
; its swapgs, or after the one before sysret -- so the saved CS cannot
; say which GS is live. GS.base can: ring 3's is always 0 (percpu.h).
; r12 (saved in the frame, preserved by isr_dispatch) remembers a swap
; this path must undo itself; one from ring 3 the ordinary exit undoes.
isr_paranoid:
    cld
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

    mov ecx, MSR_GS_BASE
    rdmsr
    xor r12d, r12d
    or eax, edx
    jnz .kernel_gs                 ; non-zero: already the kernel's
    swapgs
    test byte [rsp + F_CS*8], 3
    jnz .kernel_gs                 ; from ring 3: isr_resume_frame swaps back
    mov r12d, 1                    ; ring 0 on ring 3's GS: ours to undo
.kernel_gs:
    lfence

    mov rdi, rsp
    call isr_dispatch

    test r12d, r12d
    jz isr_resume_frame
    swapgs                         ; the pops below touch no gs:
    jmp isr_resume_frame           ; ...and its own test sees ring 0
global isr_paranoid_end
isr_paranoid_end:
