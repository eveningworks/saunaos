; ne16.inc -- macros for writing a Win16 (NE) test program in NASM, for
; tools/gen_ne.py. The source is assembled ONCE PER SEGMENT with
; -DW16_PASS=n: the segment being built goes to .text, every other one
; to a nobits section starting at 0, so a label in any segment has its
; offset within that segment -- code can name its data. Relocations and
; the entry point are recorded in .w16rel, which gen_ne.py reads back;
; the last four bytes of the output say where .w16rel starts.
;
; Header settings are comment lines gen_ne.py reads from the source:
;   ;w16: module HELLO
;   ;w16: segment 1 code
;   ;w16: segment 2 data
;   ;w16: autodata 2
;   ;w16: stack 4096
;   ;w16: heap 1024

bits 16
cpu 286

section .text
section .w16rel align=1
section .w16tail align=1
    dd section..w16rel.start

; Record kinds in .w16rel (gen_ne.py's parse_relocs() mirrors these).
%define W16_REL_IMPORT   1   ; site, srctype, module name, ordinal
%define W16_REL_INTERNAL 2   ; site, srctype, segment, offset
%define W16_REL_ENTRY    3   ; offset in the current segment
%define W16_REL_EXPORT   4   ; ordinal, offset, name

; NE relocation source types.
%define W16_SRC_SEGMENT  2
%define W16_SRC_FARPTR   3
%define W16_SRC_OFFSET   5

%macro W16_SEGMENT 1
    %define W16_CUR %1
    %if W16_PASS == %1
        section .text
    %else
        section .w16skip%1 nobits vstart=0
    %endif
%endmacro

; Leave .w16rel for the segment being built.
%macro w16_back 0
    %if W16_PASS == W16_CUR
        section .text
    %else
        section .w16skip %+ W16_CUR
    %endif
%endmacro

; A record is written only in the segment's own pass.
%macro w16_rel_import 4   ; site, srctype, "MODULE", ordinal
    %if W16_PASS == W16_CUR
        section .w16rel
        db W16_REL_IMPORT
        dw %1
        db %2
        %strlen %%n %3
        db %%n, %3
        dw %4
        w16_back
    %endif
%endmacro

%macro w16_rel_internal 4  ; site, srctype, segment, offset
    %if W16_PASS == W16_CUR
        section .w16rel
        db W16_REL_INTERNAL
        dw %1
        db %2, %3
        dw %4
        w16_back
    %endif
%endmacro

; call far MODULE.ordinal
%macro W16_CALL 2
    db 0x9A
    %%site: dd 0
    w16_rel_import %%site, W16_SRC_FARPTR, %1, %2
%endmacro

; mov REG, <selector of segment n>
%macro W16_MOV_SEG 2
    mov %1, strict word 0
    %%end:
    w16_rel_internal %%end - 2, W16_SRC_SEGMENT, %2, 0
%endmacro

; A far pointer to LABEL in segment n, as data (a window procedure).
%macro W16_FARPTR 2
    %%site: dd 0
    w16_rel_internal %%site, W16_SRC_FARPTR, %2, %1
%endmacro

; push a far pointer to LABEL in segment n (selector, then offset)
%macro W16_PUSH_FAR 2
    push strict word 0
    %%end:
    w16_rel_internal %%end - 2, W16_SRC_SEGMENT, %2, 0
    push %1
%endmacro

%macro W16_ENTRY 1
    %if W16_PASS == W16_CUR
        section .w16rel
        db W16_REL_ENTRY
        dw %1
        w16_back
    %endif
%endmacro

%macro W16_EXPORT 3   ; ordinal, "NAME", label
    %if W16_PASS == W16_CUR
        section .w16rel
        db W16_REL_EXPORT
        dw %1, %3
        %strlen %%n %2
        db %%n, %2
        w16_back
    %endif
%endmacro
