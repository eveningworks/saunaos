; rtstub.asm -- a LIBRARY (an NE DLL) that stands in for a runtime in w16's tests: it exports
; THUNRTMAIN, the one name needsvb.exe imports from VBRUN300, and does nothing. It is what a test
; hands w16's "needs a file" dialog and the Retro Launcher's Add file...: copied into drive C:'s
; WINDOWS\SYSTEM as VBRUN300.DLL, it lets needsvb.exe start. Built as rtstub.dll (`;w16: library`),
; so the launcher does not list it as a program. No code of Microsoft's is in it.
; Build: tools/gen/gen_ne.py.
;
;w16: module RTSTUB
;w16: description Runtime stand-in for tests
;w16: library
;w16: segment 1 code
;w16: segment 2 data
;w16: autodata 2
;w16: stack 0
;w16: heap 0

%include "ne16.inc"

; ---------------------------------------------------------------- code
W16_SEGMENT 1

libentry:                           ; LibEntry: AX nonzero, it started
    mov ax, 1
    retf

thunrtmain:
    retf

W16_ENTRY libentry
W16_EXPORT 100, "THUNRTMAIN", thunrtmain

; ---------------------------------------------------------------- data
W16_SEGMENT 2

            dw 0
