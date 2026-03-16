; mmu_test.asm — Bare-metal MMU test for CPUTwo
;
; Memory layout (all physical, MMU off at start):
;   0x00000000  This program (code + data)
;   0x00001000  L1 page table  (4 KB, 1024 x 4-byte PTEs)
;   0x00002000  L2 page table for VA[31:22]=0 (covers VA 0x00000000-0x003FFFFF)
;   0x00003000  Scratch data page (identity-mapped)
;
; Test plan:
;   1. Build identity-map page tables covering the program pages 0-7.
;   2. Enable MMU (SATP.EN=1, PPN=1 for L1 at 0x00001000).
;   3. Load through VA 0x00003000 and verify the data is correct.
;   4. Store through VA 0x00003004 and verify D bit gets set in the L2 PTE.
;   5. Read UART status register via its physical address (MMIO bypass check).
;   6. Execute SFENCE — should not fault (supervisor mode).
;   7. Disable MMU (SATP=0).
;   8. Verify physical access still works after disabling MMU.
;   9. Print "MMU OK\n" if everything passed; print "FAIL\n" otherwise.
;
; PTE bit layout:
;   bit  0 = V  (valid)
;   bit  1 = D  (dirty — set by hardware on store)
;   bit  5 = R  (readable)
;   bit  6 = W  (writable)
;   bit  7 = X  (executable)
;   bit  9 = G  (global — not flushed by SFENCE / SATP write)
;   bits 31:12 = PPN
;
; Non-leaf PTE (L1 -> L2):  R=W=X=0, V=1  =>  (L2_PPN << 12) | 0x01
; Leaf PTE flags used here: G|X|W|R|V = 0x200|0x80|0x40|0x20|0x01 = 0x2E1
; All pages are supervisor-only (U bit = 0).

.equ UART_STATUS, 0x03F00000
.equ UART_TX,     0x03F00004
.equ SATP_ADDR,   0x03FFF018
.equ BADADDR_ADDR,0x03FFF01C

.equ L1_BASE,     0x00001000   ; L1 table at PPN=1
.equ L2_BASE,     0x00002000   ; L2 table at PPN=2
.equ DATA_PAGE,   0x00003000   ; scratch page at PPN=3

        ; ── Initialise stack (just below MMIO) ────────────────────────
        MOVI32  sp, 0x03F00000

        ; ── Build L1 table ────────────────────────────────────────────
        ; L1[0]: non-leaf PTE -> L2 table at PPN=2
        ;   value = (2 << 12) | 0x01 = 0x00002001
        MOVI32  r0, L1_BASE
        MOVI32  r1, 0x00002001
        SW      r1, [r0]          ; L1[0] = pointer to L2

        ; ── Build L2 table (identity map pages 0-7) ───────────────────
        ; Each leaf PTE: (PPN << 12) | 0x2E1  (G|X|W|R|V, supervisor-only)
        MOVI32  r0, L2_BASE

        ; L2[0] -> PPN=0 (0x00000000 — code starts here)
        MOVI32  r1, 0x000002E1
        SW      r1, [r0]

        ; L2[1] -> PPN=1 (0x00001000 — L1 table page)
        MOVI32  r1, 0x000012E1
        ADDI    r2, r0, 4
        SW      r1, [r2]

        ; L2[2] -> PPN=2 (0x00002000 — L2 table page)
        MOVI32  r1, 0x000022E1
        ADDI    r2, r0, 8
        SW      r1, [r2]

        ; L2[3] -> PPN=3 (0x00003000 — scratch data page)
        MOVI32  r1, 0x000032E1
        ADDI    r2, r0, 12
        SW      r1, [r2]

        ; L2[4] -> PPN=4
        MOVI32  r1, 0x000042E1
        ADDI    r2, r0, 16
        SW      r1, [r2]

        ; L2[5] -> PPN=5
        MOVI32  r1, 0x000052E1
        ADDI    r2, r0, 20
        SW      r1, [r2]

        ; L2[6] -> PPN=6
        MOVI32  r1, 0x000062E1
        ADDI    r2, r0, 24
        SW      r1, [r2]

        ; L2[7] -> PPN=7
        MOVI32  r1, 0x000072E1
        ADDI    r2, r0, 28
        SW      r1, [r2]

        ; ── Write sentinel to scratch data page (physical) ────────────
        MOVI32  r0, DATA_PAGE
        MOVI32  r1, 0xDEADBEEF
        SW      r1, [r0]          ; mem[0x3000] = 0xDEADBEEF

        ; ── Enable MMU: SATP = 0x80000001 (EN=1, PPN=1) ──────────────
        MOVI32  r0, SATP_ADDR
        MOVI32  r1, 0x80000001
        SW      r1, [r0]          ; MMU active; TLB flushed

        ; ══ TEST 1: load via virtual address ══════════════════════════
        ; VA 0x00003000 -> PA 0x00003000 (identity map)
        MOVI32  r0, DATA_PAGE
        LW      r2, [r0]
        MOVI32  r3, 0xDEADBEEF
        CMP     r2, r3
        BNE     fail

        ; ══ TEST 2: store via virtual address; verify D bit ═══════════
        MOVI32  r0, DATA_PAGE
        MOVI32  r1, 0x12345678
        ADDI    r4, r0, 4         ; VA 0x00003004
        SW      r1, [r4]          ; store — hardware should set D bit in L2[3]

        ; Read back via VA to confirm store worked
        LW      r2, [r4]
        CMP     r2, r1
        BNE     fail

        ; L2[3] PTE is at physical byte offset 12 = 0x0C from L2_BASE
        ; PA of L2[3] PTE = 0x00002000 + 3*4 = 0x0000200C
        ; (VA == PA for identity map, so we can read it via VA too)
        MOVI32  r4, 0x0000200C
        LW      r5, [r4]          ; read L2[3] PTE
        MOVI    r6, 2             ; D bit = bit 1
        AND     r5, r5, r6
        CMP     r5, r6
        BNE     fail              ; D bit not set -> FAIL

        ; ══ TEST 3: MMIO bypass — UART reachable with MMU on ══════════
        MOVI32  r0, UART_STATUS   ; 0x03F00000 — in MMIO region, bypasses MMU
        LW      r1, [r0]
        MOVI    r2, 1
        AND     r1, r1, r2
        CMP     r1, r2
        BNE     fail              ; TX not ready bit -> unexpected

        ; ══ TEST 4: SFENCE ════════════════════════════════════════════
        SFENCE

        ; ══ TEST 5: disable MMU ═══════════════════════════════════════
        MOVI32  r0, SATP_ADDR
        MOVI    r1, 0
        SW      r1, [r0]          ; SATP = 0 => MMU off

        ; Verify physical access still works
        MOVI32  r0, DATA_PAGE
        LW      r2, [r0]
        MOVI32  r3, 0xDEADBEEF
        CMP     r2, r3
        BNE     fail

        ; ══ All tests passed ══════════════════════════════════════════
        MOVI32  r1, ok_msg
        MOVI32  r2, UART_TX
print_loop:
        LBU     r0, [r1]
        CMPI    r0, 0
        BEQ     done
        SW      r0, [r2]
        ADDI    r1, r1, 1
        JMP     r12, print_loop
done:
        HALT

fail:
        MOVI32  r1, fail_msg
        MOVI32  r2, UART_TX
fail_loop:
        LBU     r0, [r1]
        CMPI    r0, 0
        BEQ     fail_done
        SW      r0, [r2]
        ADDI    r1, r1, 1
        JMP     r12, fail_loop
fail_done:
        HALT

ok_msg:
        .asciiz "MMU OK\n"
fail_msg:
        .asciiz "FAIL\n"
