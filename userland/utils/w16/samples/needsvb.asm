; needsvb.asm -- a program that cannot start without a DLL nobody ships with it: it imports
; VBRUN300.THUNRTMAIN, as a Visual Basic 3 program does, and VBRUN300.DLL is in none of the places
; the loader looks. `w16 --check` says `needs VBRUN300.DLL Visual Basic 3.0 runtime`, the Retro
; Launcher greys its Run and offers Add file..., and w16 asks for the file before loading it
; (needdll16.c). Given rtstub.dll -- a library exporting that one name -- it starts, shows a message
; box saying so, and ends when it is closed. THUNRTMAIN is never called.
; Build: tools/gen/gen_ne.py.
;
;w16: module NEEDSVB
;w16: description Runtime test
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
    jz done
    push word 0                     ; MessageBox(NULL, text, caption, MB_OK)
    push ds
    push word box_text
    push ds
    push word title
    push word 0
    W16_CALL "USER", 1
    jmp done
    W16_CALL_NAME "VBRUN300", "THUNRTMAIN"

done:
    mov ax, 0x4C00
    int 0x21

W16_ENTRY start

; ---------------------------------------------------------------- data
W16_SEGMENT 2

            times 16 db 0           ; the Windows header words a task keeps
title:      db "Runtime test", 0
box_text:   db "It started: its runtime was found.", 0
