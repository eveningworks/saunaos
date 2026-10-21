; zbyname.asm -- a program that imports FUNCTIONS BY NAME that w16 cannot number: one from a DLL
; of its own (MYDLL), one from KERNEL that w16's tables lack. It exits at once, so neither is
; ever called; the point is what `w16 --check` and the Retro Launcher say of it: every such import
; counts and is LISTED as missing, once each however often it is named -- a program that imports
; a name from a DLL (RASEXE does) read "1 call missing" in the panel and "Missing 0" in the calls
; window while --check counted it and never named it.
; Its name sorts last on purpose: tests index the samples by their place in a name-ordered listing.
; Build: tools/gen_ne.py.
;
;w16: module BYNAME
;w16: description Unnumbered imports test
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
    jmp done
    W16_CALL_NAME "MYDLL", "Frobnicate"
    W16_CALL_NAME "KERNEL", "NoSuchFunction"
    W16_CALL_NAME "MYDLL", "Frobnicate"     ; the same name again: one import

done:
    mov ax, 0x4C00
    int 0x21

W16_ENTRY start

; ---------------------------------------------------------------- data
W16_SEGMENT 2

            times 16 db 0           ; the Windows header words a task keeps
