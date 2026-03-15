; selection_sort.asm — DodgyCPU
;
; Fills a 1000-byte array with pseudo-random bytes using a small LCG:
;
;     seed = (seed * 77 + 13) & 0xFF
;
; then sorts the array in-place with selection sort.
; After HALT the sorted bytes live at address DATA (0x1000).
;
; Register map:
;   r0  — array base address (DATA)
;   r1  — n (1000), kept alive across both phases
;   r2  — fill phase: LCG seed | sort phase: outer index i
;   r3  — fill phase: fill counter | sort phase: min_idx
;   r4  — fill phase: write pointer | sort phase: inner index j
;   r5  — sort phase: data[j]
;   r6  — sort phase: data[min_idx] | fill phase: LCG scratch
;   r7  — fill phase: LCG multiplier (77)
;   r9  — n-1 (999), upper bound for outer loop

.equ DATA, 0x1000       ; base address of the array
.equ N,    1000         ; number of bytes to fill and sort

; ── Initialise ────────────────────────────────────────────────────────────────
        MOVI    r0, DATA        ; r0 = array base
        MOVI    r1, N           ; r1 = n

; ── Fill array with pseudo-random bytes ───────────────────────────────────────
; LCG: seed = (seed * 77 + 13) & 0xFF
        MOVI    r2, 0x5A        ; r2 = initial seed
        MOVI    r3, 0           ; r3 = fill counter
        MOVI    r7, 77          ; r7 = LCG multiplier (constant through fill)
        MOV     r4, r0          ; r4 = write pointer = base

fill_loop:
        SB      r2, [r4]        ; mem[r4] = seed & 0xFF
        MUL     r6, r2, r7      ; r6 = seed * 77
        ADDI    r6, r6, 13      ; r6 = seed * 77 + 13
        ANDI    r2, r6, 0xFF    ; seed = (seed * 77 + 13) & 0xFF
        ADDI    r4, r4, 1       ; advance write pointer
        ADDI    r3, r3, 1       ; counter++
        CMPI    r3, N
        BLT     fill_loop

; ── Selection sort ────────────────────────────────────────────────────────────
; for i = 0 .. n-2:
;   find index of minimum in data[i..n-1]
;   if minimum is not already at i, swap it there
        MOVI    r2, 0           ; i = 0
        SUBI    r9, r1, 1       ; r9 = n-1 (outer loop runs while i < n-1)

outer_loop:
        CMP     r2, r9          ; i < n-1 ?
        BGE     done

        MOV     r3, r2          ; min_idx = i
        ADDI    r4, r2, 1       ; j = i+1

inner_loop:
        CMP     r4, r1          ; j < n ?
        BGE     end_inner

        LBUX    r5, [r0+r4]     ; r5 = data[j]       (was: ADD r7 + LBU r5)
        LBUX    r6, [r0+r3]     ; r6 = data[min_idx] (was: ADD r8 + LBU r6)
        CMP     r5, r6          ; data[j] vs data[min_idx]
        BGEU    no_update       ; if data[j] >= data[min_idx], keep current min
        MOV     r3, r4          ; min_idx = j
no_update:
        ADDI    r4, r4, 1       ; j++
        BA      inner_loop

end_inner:
        CMP     r3, r2          ; min_idx == i?  (no swap needed)
        BEQ     next_i

        LBUX    r5, [r0+r2]     ; r5 = data[i]       (was: ADD r7 + LBU r5)
        LBUX    r6, [r0+r3]     ; r6 = data[min_idx] (was: ADD r8 + LBU r6)
        SBX     r6, [r0+r2]     ; data[i]       = data[min_idx]  (was: ADD r7 + SB)
        SBX     r5, [r0+r3]     ; data[min_idx] = old data[i]    (was: ADD r8 + SB)

next_i:
        ADDI    r2, r2, 1       ; i++
        BA      outer_loop

done:
        HALT
