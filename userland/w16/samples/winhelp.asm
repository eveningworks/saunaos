; winhelp.asm -- WinHelp(), the call a program's Help menu makes: no window of its
; own. It asks for context number 100 of C:\WINDOWS\SAMPLE.HLP (HELP_CONTEXT), waits
; eight seconds on GetTickCount, then quits the help (HELP_QUIT) and ends.
; tools/hlpview_test.py puts tools/hlpgen.py's sample() there and checks that the
; viewer opens at the topic that number names, and goes with HELP_QUIT.
; Build: tools/gen_ne.py.
;
;w16: module WINHELP
;w16: description WinHelp test
;w16: segment 1 code
;w16: segment 2 data
;w16: autodata 2
;w16: stack 4096
;w16: heap 1024

%include "ne16.inc"

; ---------------------------------------------------------------- code
W16_SEGMENT 1

start:
    W16_CALL "KERNEL", 91           ; InitTask
    or ax, ax
    jz fail

    push word 0                     ; WinHelp(0, file, HELP_CONTEXT, 100)
    push ds
    push word help_file
    push word 1
    push word 0
    push word 100
    W16_CALL "USER", 171

    W16_CALL "USER", 13             ; GetTickCount -> DX:AX; wait eight seconds
    add ax, 8000
    mov [until], ax
.again:
    W16_CALL "USER", 13
    sub ax, [until]
    js .again

    push word 0                     ; WinHelp(0, file, HELP_QUIT, 0)
    push ds
    push word help_file
    push word 2
    push word 0
    push word 0
    W16_CALL "USER", 171

    mov ax, 0x4C00
    int 0x21
fail:
    mov ax, 0x4C01
    int 0x21

W16_ENTRY start

; ---------------------------------------------------------------- data
W16_SEGMENT 2

            times 16 db 0           ; the Windows header words a task keeps
until:      dw 0
help_file:  db "C:\WINDOWS\SAMPLE.HLP", 0
