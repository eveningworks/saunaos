; An application processor's first instructions: real mode to long mode
; in one page, then into smp.c's ap_entry() on its own stack. Linux's is
; arch/x86/realmode/rm/trampoline_64.S; the shape has no alternative.
;
; This is a TEMPLATE in .rodata. smp.c copies it to a page below 1 MiB
; (a STARTUP IPI names a real-mode page), fills the DATA PAGE right
; after it, and makes the code page executable for the bring-up only.
;
; **POSITION-INDEPENDENT BY HAND: NOTHING HERE MAY NEED A RELOCATION.**
; The copy runs at whatever page smp.c found, so every address is either
; an offset into the data page or one computed from CS. A label used as
; an absolute address would be the template's, in .rodata, and the
; kernel's relocator would happily "fix" it to the wrong copy.
;
; The data page, at +0x1000 (smp.c's struct smp_tramp_data -- the two
; layouts are one, keep them so):
%define D_GDT    0x1000   ; 4 descriptors: null, code32, data32, code64
%define D_GDTR   0x1020   ; lgdt operand: limit, then the GDT's linear base
%define D_JMP32  0x1030   ; far pointer: 32-bit offset, then selector 0x08
%define D_JMP64  0x1038   ; far pointer: 32-bit offset, then selector 0x18
%define D_CR3    0x1040   ; the kernel's PML4 -- below 4 GiB, so 32-bit mode can load it
%define D_STACK  0x1048   ; the AP's stack top
%define D_CPU    0x1050   ; its struct cpu, ap_entry()'s argument
%define D_ENTRY  0x1058   ; ap_entry()

%define SEL_DATA32 0x10

section .rodata
align 16

global smp_tramp_start
smp_tramp_start:

bits 16
    cli
    cld
    mov ax, cs
    mov ds, ax
    ; The page's linear base, for the 32- and 64-bit halves (no CS
    ; trick works once segments are flat).
    xor ebx, ebx
    mov bx, ax
    shl ebx, 4

    lgdt [D_GDTR]
    mov eax, cr0
    or eax, 1                      ; PE
    mov cr0, eax
    jmp dword far [D_JMP32]

bits 32
global smp_tramp_32
smp_tramp_32:
    mov ax, SEL_DATA32
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov eax, cr4
    or eax, 1 << 5                 ; PAE
    mov cr4, eax
    mov eax, [ebx + D_CR3]
    mov cr3, eax
    ; LME, and NXE BEFORE paging: every kernel data page is NX, and with
    ; NXE clear that bit is a reserved bit -- a #PF on the first read.
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8) | (1 << 11)
    wrmsr
    mov eax, cr0
    or eax, 1 << 31                ; PG: compatibility mode, on the kernel's map
    mov cr0, eax
    jmp far [ebx + D_JMP64]

bits 64
global smp_tramp_64
smp_tramp_64:
    mov ebx, ebx                   ; a mode switch leaves the upper halves undefined
    mov rsp, [rbx + D_STACK]
    mov rdi, [rbx + D_CPU]
    mov rax, [rbx + D_ENTRY]
    xor ebp, ebp
    push 0                         ; as if called: the ABI's entry alignment
    jmp rax

global smp_tramp_end
smp_tramp_end:
