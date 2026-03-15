; Hello, World! — DodgyCPU
;
; CPU resets into supervisor mode at address 0x00000000.
; UART device is memory-mapped at 0x03F00000:
;   +0x00  Status   — bit 0 = TX ready (always 1 in emulator)
;   +0x04  TX       — write a byte here to send it to stdout
;
; Algorithm: load each byte of the string; test for null terminator;
; write the byte to UART TX; repeat.

        MOVI32  r1, msg             ; r1 = pointer to string
        MOVI32  r2, 0x03F00004      ; r2 = UART TX register address

loop:
        LBU     r0, [r1]            ; r0 = *r1 (zero-extended byte)
        CMPI    r0, 0               ; compare r0 with zero to set flags
        BEQ     done                ; null terminator reached, stop
        SW      r0, [r2]            ; write byte to UART TX
        ADDI    r1, r1, 1           ; advance string pointer
        JMP     r12, loop           ; loop back (r12 = scratch, discard)

done:
        HALT

msg:
        .asciiz "Hello, World!\n"
