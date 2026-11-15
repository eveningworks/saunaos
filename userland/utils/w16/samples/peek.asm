; peek.asm -- PeekMessage on an empty queue, timed by GetTickCount, no window. Three phases of 200:
;
;   A. PeekMessage, then ~12K instructions of work -- a game's loop (Sierra's SCI polls once a frame)
;   B. the same work alone
;   C. PeekMessage alone, back to back -- a program idling in a poll loop
;
; The exit code: bit 0 when A took under 100 ms more than B (no sleep after work), bit 1 when C took
; 150 ms or more (a tight poll still gives up its millisecond, as w16 does to spare a core).
; tools/tests/gui/w16_gui_test.py's `peek_idle` reads it.
; Build: tools/gen/gen_ne.py.
;
;w16: module PEEK
;w16: description PeekMessage idle test
;w16: segment 1 code
;w16: segment 2 data
;w16: autodata 2
;w16: stack 4096
;w16: heap 1024

%include "ne16.inc"

%define ROUNDS 200
%define WORK   12000

; ---------------------------------------------------------------- code
W16_SEGMENT 1

start:
    W16_CALL "KERNEL", 91           ; InitTask
    or ax, ax
    jz finish
    mov [hInstance], di
    push word 0
    W16_CALL "KERNEL", 30           ; WaitEvent(0)
    push word [hInstance]
    W16_CALL "USER", 5              ; InitApp(hInstance)

    W16_CALL "USER", 13             ; A: peek, then work
    mov [t0], ax
    mov word [round], ROUNDS
.a:
    call peek
    call work
    dec word [round]
    jnz .a
    W16_CALL "USER", 13
    sub ax, [t0]
    mov [ta], ax

    W16_CALL "USER", 13             ; B: the work alone
    mov [t0], ax
    mov word [round], ROUNDS
.b:
    call work
    dec word [round]
    jnz .b
    W16_CALL "USER", 13
    sub ax, [t0]
    mov [tb], ax

    W16_CALL "USER", 13             ; C: peeks back to back
    mov [t0], ax
    mov word [round], ROUNDS
.c:
    call peek
    dec word [round]
    jnz .c
    W16_CALL "USER", 13
    sub ax, [t0]

    cmp ax, 150
    jb .tight_fast
    or byte [result], 2
.tight_fast:
    mov ax, [ta]
    sub ax, [tb]
    cmp ax, 100
    jge finish                      ; signed: A may run a little faster than B
    or byte [result], 1
finish:
    mov al, [result]
    mov ah, 0x4C
    int 0x21

; PeekMessage(&msg, NULL, 0, 0, PM_REMOVE) on an empty queue
peek:
    push ds
    push word msg
    push word 0
    push word 0
    push word 0
    push word 1
    W16_CALL "USER", 109
    ret

; WORK instructions of the program's own, calling nothing
work:
    mov cx, WORK
.w:
    loop .w
    ret

W16_ENTRY start

; ---------------------------------------------------------------- data
W16_SEGMENT 2

    times 16 db 0                   ; the bytes below DS:10h are Windows' own
hInstance: dw 0
result:    db 0, 0
round:     dw 0
t0:        dw 0
ta:        dw 0
tb:        dw 0
msg:       times 18 db 0
