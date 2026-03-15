; Primes less than 100 — DodgyCPU (revised)
;
; Demonstrates the full current instruction set:
;   MOVI32  — 32-bit constant load
;   CMP     — register comparison (flags only, no scratch register consumed)
;   CMPI    — immediate comparison
;   BA      — unconditional branch (replaces JMP rN, target)
;   JMP lr  — subroutine call (stores return address in lr)
;   RET     — return from subroutine (MOV pc, lr)
;
; Register allocation (main):
;   r4  = n         current prime candidate (2 .. 99)
;   r5  = d         trial divisor (2 .. n-1)
;   r6  = is_prime  flag: 1 while no divisor found, 0 once composite confirmed
;   r7  = UART TX   constant throughout; treated as program-global
;
; print_num(r0): prints r0 as a decimal number followed by newline.
;   Uses r0, r1, r2 as scratch.  Saves/restores lr.  r7 must hold UART TX.

; ── Startup ──────────────────────────────────────────────────────────────────
        MOVI32  sp, 0x03F00000      ; initialise stack pointer to top of RAM
        MOVI32  r7, 0x03F00004      ; UART TX register

; ── Outer loop: test each candidate n ────────────────────────────────────────
        MOVI    r4, 2               ; n = 2 (first candidate)

outer:
        MOVI    r6, 1               ; is_prime = true
        MOVI    r5, 2               ; d = 2 (first trial divisor)

; ── Inner loop: try divisors 2 .. n-1 ────────────────────────────────────────
inner:
        CMP     r5, r4              ; compare d with n
        BGE     found_prime         ; d >= n → no divisor found → n is prime

        MOD     r0, r4, r5          ; r0 = n mod d  (also sets Z if zero)
        BNE     next_d              ; remainder != 0 → try next divisor

        MOVI    r6, 0               ; n is composite
        BA      found_prime         ; skip remaining divisors

next_d:
        ADDI    r5, r5, 1           ; d++
        BA      inner

; ── Decide whether to print ───────────────────────────────────────────────────
found_prime:
        CMPI    r6, 0               ; test is_prime flag
        BEQ     next_n              ; composite — skip

        MOV     r0, r4              ; print_num argument = n
        JMP     lr, print_num       ; call print_num(r0)

; ── Advance to next candidate ─────────────────────────────────────────────────
next_n:
        ADDI    r4, r4, 1           ; n++
        CMPI    r4, 100             ; compare n with 100
        BLT     outer               ; n < 100 → keep going

        HALT

; ── print_num(r0) ─────────────────────────────────────────────────────────────
; Prints r0 (0..99) as a decimal string followed by '\n'.
; Caller-saved: r0, r1, r2.  Saves and restores lr.
; r7 must hold the UART TX address (set by startup code above).
print_num:
        ADDI    sp, sp, -4          ; push lr
        SW      lr, [sp]

        MOVI    r1, 10
        DIV     r2, r0, r1          ; r2 = n / 10  (tens digit; Z=1 if 0)
        BEQ     skip_tens           ; single-digit number — omit tens digit
        ADDI    r2, r2, 48          ; tens + '0'
        SW      r2, [r7]            ; send tens digit to UART

skip_tens:
        MOD     r0, r0, r1          ; r0 = n % 10  (units digit; r1 still = 10)
        ADDI    r0, r0, 48          ; units + '0'
        SW      r0, [r7]            ; send units digit to UART

        MOVI    r0, 10              ; '\n' = 0x0A
        SW      r0, [r7]

        LW      lr, [sp]            ; pop lr
        ADDI    sp, sp, 4
        RET                         ; return to caller  (MOV pc, lr)
