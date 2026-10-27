; sound.asm -- w16's sound test program, no window: MMSYSTEM, USER's
; MessageBeep and SOUND.DRV, each with a sound tools/w16_sound_test.py can
; tell apart in what the guest's sound card played:
;
;   1. sndPlaySound(wav, SND_MEMORY) -- SYNC: a WAV in this data segment,
;      11025 Hz 8-bit mono, a square wave of 10-sample periods (1102.5 Hz),
;      0.3 s;
;   2. MessageBeep(MB_ICONASTERISK) -- the Asterisk event: WIN.INI's
;      [sounds] SystemAsterisk on drive C: when it names a file there,
;      else SaunaOS's chime;
;   3. SOUND.DRV: two quarter notes of value 46 (A above middle C, 440 Hz)
;      at 120 a minute -- a second of square wave.
;
; The exit code says what answered: bit 0 waveOutGetNumDevs said a
; device, bit 1 sndPlaySound said it played, bit 2 OpenSound gave a voice.
; Build: tools/gen_ne.py.
;
;w16: module SOUND
;w16: description Sound test
;w16: segment 1 code
;w16: segment 2 data
;w16: autodata 2
;w16: stack 4096
;w16: heap 1024

%include "ne16.inc"

%define SND_MEMORY       0x0004
%define MB_ICONASTERISK  0x0040
%define WAV_FRAMES       3300

; ---------------------------------------------------------------- code
W16_SEGMENT 1

start:
    W16_CALL "KERNEL", 91           ; InitTask
    or ax, ax
    jz done
    mov [hInstance], di
    push word 0
    W16_CALL "KERNEL", 30           ; WaitEvent(0)
    push word [hInstance]
    W16_CALL "USER", 5              ; InitApp(hInstance)

    W16_CALL "MMSYSTEM", 401        ; waveOutGetNumDevs()
    or ax, ax
    jz no_device
    or byte [result], 1
no_device:
    push ds                         ; sndPlaySound(wav, SND_MEMORY): synchronous
    push word wav
    push word SND_MEMORY
    W16_CALL "MMSYSTEM", 2
    or ax, ax
    jz no_wav
    or byte [result], 2
no_wav:
    push word MB_ICONASTERISK
    W16_CALL "USER", 104            ; MessageBeep(MB_ICONASTERISK): asynchronous...
    mov cx, 900
    call wait_ms                    ; ...so let it play out

    W16_CALL "SOUND", 1             ; OpenSound() -> voices, or -1
    cmp ax, 0
    jle no_voice
    or byte [result], 4
    push word 1                     ; SetVoiceAccent(1, tempo 120, volume 255, S_NORMAL, pitch 0)
    push word 120
    push word 255
    push word 0
    push word 0
    W16_CALL "SOUND", 5
    push word 1                     ; SetVoiceNote(1, 46, quarter, no dots), twice
    push word 46
    push word 4
    push word 0
    W16_CALL "SOUND", 4
    push word 1
    push word 46
    push word 4
    push word 0
    W16_CALL "SOUND", 4
    W16_CALL "SOUND", 9             ; StartSound()
    mov cx, 1300
    call wait_ms
    W16_CALL "SOUND", 2             ; CloseSound()
no_voice:
done:
    mov al, [result]
    mov ah, 0x4C
    int 0x21

; wait_ms: CX milliseconds by GetTickCount
wait_ms:
    push cx
    W16_CALL "USER", 13             ; GetTickCount -> DX:AX
    pop cx
    add ax, cx
    mov [until], ax
.again:
    W16_CALL "USER", 13
    sub ax, [until]
    js .again
    ret

W16_ENTRY start

; ---------------------------------------------------------------- data
W16_SEGMENT 2

    times 16 db 0                   ; the bytes below DS:10h are Windows' own
hInstance: dw 0
result:    db 0, 0
until:     dw 0

wav:
    db "RIFF"
    dd 36 + WAV_FRAMES
    db "WAVE", "fmt "
    dd 16
    dw 1, 1                         ; PCM, mono
    dd 11025, 11025                 ; rate, bytes a second
    dw 1, 8                         ; block, bits
    db "data"
    dd WAV_FRAMES
%rep WAV_FRAMES / 10
    db 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0x40, 0x40, 0x40, 0x40, 0x40
%endrep
