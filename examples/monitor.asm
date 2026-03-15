; DodgyCPU ROM Monitor — loads at 0x00000000
;
; Commands: s/step  c/cont  regs  break <hex>  mem <hex> [<hex>]  pc <hex>
;
; Memory layout:
;   0x00008000  EVEC table        (8 words)
;   0x00008020  user_ctx          (r0-r15, flags = 17 words = 68 bytes)
;   0x00008070  line_buf          (256 bytes)
;   0x00008170  bp_table          (16 × 8 bytes: addr, orig_instr; addr=0 → free)
;   0x00008270  step_bp           (addr, orig_instr)
;   0x00008278  step_active       (1 if step BP is armed)
;   0x0000C000  monitor stack top (grows down)
;   0x0000F000  initial user SP
;
; NOTE: r14 (lr) is the exception-entry scratch register and its user value
; is NOT saved across a trap.  User code that makes subroutine calls (which
; write lr via JMP lr,target) is unaffected; only lr-as-data is lost.

.equ UART_STATUS,   0x03F00000
.equ UART_TX,       0x03F00004
.equ UART_RX,       0x03F00008
.equ SV_BASE,       0x03FFF000   ; SV_EPC +0, SV_EFLAGS +4, SV_EVEC +8, SV_CAUSE +12

.equ EVEC_BASE,     0x00008000
.equ USER_CTX,      0x00008020   ; r0..r15 then flags  (offsets 0..64)
.equ LINE_BUF,      0x00008070
.equ LINE_BUF_SZ,   256
.equ BP_TABLE,      0x00008170
.equ MAX_BPS,       16
.equ STEP_BP,       0x00008270
.equ STEP_ACTIVE,   0x00008278
.equ MON_SP,        0x0000C000
.equ USER_SP_INIT,  0x0000F000
.equ SYSCALL_INSTR, 0x10000000

; ASCII constants used in comparisons
.equ CH_CR,  13
.equ CH_LF,  10
.equ CH_BS,  8
.equ CH_DEL, 127
.equ CH_SP,  32

    .org 0x00000000

; ── Startup ──────────────────────────────────────────────────────────────────
_start:
    MOVI32  sp, MON_SP

    ; Write EVEC address into supervisor register (SV_BASE+8 = SV_EVEC)
    MOVI32  r0, SV_BASE
    MOVI32  r1, EVEC_BASE
    SW      r1, [r0+8]

    ; Fill all 8 EVEC entries with _exception_handler
    MOVI32  r0, EVEC_BASE
    MOVI32  r1, _exception_handler
    SW      r1, [r0]
    SW      r1, [r0+4]
    SW      r1, [r0+8]
    SW      r1, [r0+12]
    SW      r1, [r0+16]
    SW      r1, [r0+20]
    SW      r1, [r0+24]
    SW      r1, [r0+28]

    ; Initialise user context: PC=0x00010000, SP=USER_SP_INIT, rest=0
    MOVI32  r0, USER_CTX
    MOVI32  r1, 0x00010000
    SW      r1, [r0+60]         ; ctx.r15 = initial user PC
    MOVI32  r1, USER_SP_INIT
    SW      r1, [r0+52]         ; ctx.r13 = initial user SP

    ; Clear step_active flag
    MOVI32  r0, STEP_ACTIVE
    MOVI    r1, 0
    SW      r1, [r0]

    MOVI32  r0, _str_banner
    CALL    _puts
    BA      _cmd_loop

; ── Command loop ─────────────────────────────────────────────────────────────
_cmd_loop:
    MOVI32  sp, MON_SP          ; reset stack on each iteration

    MOVI32  r0, _str_prompt
    CALL    _puts

    MOVI32  r0, LINE_BUF
    MOVI    r1, LINE_BUF_SZ
    CALL    _read_line

    ; Skip leading spaces; r0 → first non-space char
    MOVI32  r0, LINE_BUF
    CALL    _skip_spaces

    ; Dispatch on first character
    LBU     r1, [r0]
    CMPI    r1, 0
    BEQ     _cmd_loop

    ; 's' = 115
    CMPI    r1, 115
    BNE     _dc_c
    MOVI32  r2, _str_s
    CALL    _str_startswith
    CMPI    r0, 0
    BNE     _cmd_step
    BA      _cmd_loop

_dc_c:
    ; 'c' = 99
    CMPI    r1, 99
    BNE     _dc_r
    MOVI32  r2, _str_c
    CALL    _str_startswith
    CMPI    r0, 0
    BNE     _cmd_cont
    BA      _cmd_loop

_dc_r:
    ; 'r' = 114
    CMPI    r1, 114
    BNE     _dc_b
    MOVI32  r2, _str_regs
    CALL    _str_startswith
    CMPI    r0, 0
    BNE     _cmd_regs
    BA      _cmd_loop

_dc_b:
    ; 'b' = 98
    CMPI    r1, 98
    BNE     _dc_m
    MOVI32  r2, _str_break
    CALL    _str_startswith
    CMPI    r0, 0
    BNE     _do_break           ; r0 = ptr past "break"
    BA      _cmd_loop

_dc_m:
    ; 'm' = 109
    CMPI    r1, 109
    BNE     _dc_p
    MOVI32  r2, _str_mem
    CALL    _str_startswith
    CMPI    r0, 0
    BNE     _do_mem             ; r0 = ptr past "mem"
    BA      _cmd_loop

_dc_p:
    ; 'p' = 112
    CMPI    r1, 112
    BNE     _dc_unknown
    MOVI32  r2, _str_pc_cmd
    CALL    _str_startswith
    CMPI    r0, 0
    BNE     _do_set_pc
    BA      _cmd_loop

_dc_unknown:
    MOVI32  r0, _str_help
    CALL    _puts
    BA      _cmd_loop

; ── pc <addr> — set user PC ──────────────────────────────────────────────────
_do_set_pc:
    ; r0 = ptr past "pc"
    CALL    _skip_spaces
    CALL    _parse_hex          ; r0=val, r1=ok, r2=cursor
    CMPI    r1, 0
    BEQ     _bad_arg
    MOVI32  r1, USER_CTX
    SW      r0, [r1+60]
    MOVI32  r0, _str_pc_set
    CALL    _puts
    BA      _cmd_loop

; ── regs ─────────────────────────────────────────────────────────────────────
_cmd_regs:
    MOVI32  r4, USER_CTX        ; r4 = ctx base (callee-saved)
    MOVI    r5, 0               ; r5 = register index
_regs_loop:
    CMPI    r5, 16
    BGE     _regs_flags

    ; Load register name pointer from table
    MOVI32  r0, _reg_name_table
    LSLI    r1, r5, 2
    ADD     r0, r0, r1
    LW      r0, [r0]            ; pointer to name string
    CALL    _puts               ; e.g. "r0  "

    MOVI32  r0, _str_eq         ; " = 0x"
    CALL    _puts

    ; Print register value from ctx
    LSLI    r1, r5, 2
    ADD     r1, r4, r1
    LW      r0, [r1]
    CALL    _print_hex32
    CALL    _print_newline

    ADDI    r5, r5, 1
    BA      _regs_loop

_regs_flags:
    MOVI32  r0, _str_flags_lbl
    CALL    _puts
    LW      r0, [r4+64]
    CALL    _print_hex32
    CALL    _print_newline
    BA      _cmd_loop

; ── break <addr> ─────────────────────────────────────────────────────────────
_do_break:
    ; r0 = ptr past "break"
    CALL    _skip_spaces
    CALL    _parse_hex          ; r0=addr, r1=ok
    CMPI    r1, 0
    BEQ     _bad_arg
    MOV     r4, r0              ; r4 = target address

    ; Find free slot (slot.addr == 0 → free)
    MOVI32  r5, BP_TABLE
    MOVI    r6, 0
_bp_find:
    CMPI    r6, MAX_BPS
    BGE     _bp_full
    LW      r7, [r5]
    CMPI    r7, 0
    BEQ     _bp_install
    ADDI    r5, r5, 8
    ADDI    r6, r6, 1
    BA      _bp_find

_bp_full:
    MOVI32  r0, _str_bp_full
    CALL    _puts
    BA      _cmd_loop

_bp_install:
    LW      r7, [r4]            ; save original instruction
    SW      r4, [r5]            ; slot.addr = target
    SW      r7, [r5+4]          ; slot.orig = original instruction
    MOVI32  r7, SYSCALL_INSTR
    SW      r7, [r4]            ; patch SYSCALL
    MOVI32  r0, _str_bp_set
    CALL    _puts
    MOV     r0, r4
    CALL    _print_hex32
    CALL    _print_newline
    BA      _cmd_loop

; ── mem <addr> [<len>] ───────────────────────────────────────────────────────
_do_mem:
    ; r0 = ptr past "mem"
    CALL    _skip_spaces
    CALL    _parse_hex          ; r0=addr, r1=ok, r2=cursor
    CMPI    r1, 0
    BEQ     _bad_arg
    MOV     r4, r0              ; r4 = addr

    ; Optional length argument
    MOV     r0, r2
    CALL    _skip_spaces
    LBU     r1, [r0]
    CMPI    r1, 0
    BEQ     _mem_default
    CALL    _parse_hex          ; r0=len, r1=ok
    CMPI    r1, 0
    BEQ     _mem_default
    MOV     r5, r0
    BA      _mem_go
_mem_default:
    MOVI    r5, 64
_mem_go:
    MOV     r0, r4
    MOV     r1, r5
    CALL    _dump_mem
    BA      _cmd_loop

; ── cont ─────────────────────────────────────────────────────────────────────
_cmd_cont:
    MOVI    r0, 0
    CALL    _install_all_bps
    ; fall through

; ── Resume user code via SYSRET ──────────────────────────────────────────────
_resume_user:
    MOVI32  r0, USER_CTX

    ; Set EPC = ctx.r15, EFLAGS = ctx.flags
    LW      r1, [r0+60]
    MOVI32  r2, SV_BASE
    SW      r1, [r2]            ; SV_EPC = ctx.pc
    LW      r1, [r0+64]
    SW      r1, [r2+4]          ; SV_EFLAGS = ctx.flags

    ; Restore r1-r14, then r0 last
    LW      r1,  [r0+4]
    LW      r2,  [r0+8]
    LW      r3,  [r0+12]
    LW      r4,  [r0+16]
    LW      r5,  [r0+20]
    LW      r6,  [r0+24]
    LW      r7,  [r0+28]
    LW      r8,  [r0+32]
    LW      r9,  [r0+36]
    LW      r10, [r0+40]
    LW      r11, [r0+44]
    LW      r12, [r0+48]
    LW      r13, [r0+52]
    LW      r14, [r0+56]
    LW      r0,  [r0]
    SYSRET

; ── step ─────────────────────────────────────────────────────────────────────
_cmd_step:
    MOVI32  r4, USER_CTX
    LW      r5, [r4+60]         ; r5 = user PC
    LW      r6, [r5]            ; r6 = instruction at user PC
    LSRI    r7, r6, 24          ; r7 = opcode
    ADDI    r8, r5, 4           ; r8 = default next_pc

    ; Branch (0x0D)
    CMPI    r7, 0x0D
    BNE     _step_jmp
    MOVI32  r0, 0xFFFFF
    AND     r11, r6, r0         ; offset20
    MOVI32  r0, 0x80000
    AND     r12, r11, r0        ; sign bit
    CMPI    r12, 0
    BEQ     _step_br_nosign
    MOVI32  r0, 0xFFF00000
    OR      r11, r11, r0        ; sign-extend
_step_br_nosign:
    ADD     r12, r5, r11        ; branch target
    LW      r9, [r4+64]         ; ctx.flags
    LSRI    r10, r6, 20
    ANDI    r10, r10, 0x0F      ; cond field
    CALL    _eval_branch_cond   ; r10=cond, r9=flags -> r0=taken
    CMPI    r0, 0
    BEQ     _step_install
    MOV     r8, r12
    BA      _step_install

_step_jmp:
    ; JMP (0x0E)
    CMPI    r7, 0x0E
    BNE     _step_callr
    MOVI32  r0, 0xFFFFF
    AND     r11, r6, r0
    MOVI32  r0, 0x80000
    AND     r12, r11, r0
    CMPI    r12, 0
    BEQ     _step_jmp_nosign
    MOVI32  r0, 0xFFF00000
    OR      r11, r11, r0
_step_jmp_nosign:
    ADD     r8, r5, r11
    BA      _step_install

_step_callr:
    ; CALLR (0x2A): next = ctx.r[rs1]
    CMPI    r7, 0x2A
    BNE     _step_mov_pc
    LSRI    r10, r6, 16
    ANDI    r10, r10, 0x0F
    LSLI    r10, r10, 2
    ADD     r10, r4, r10
    LW      r8, [r10]
    BA      _step_install

_step_mov_pc:
    ; MOV pc, rs1 (opcode 0x27, rd=15)
    CMPI    r7, 0x27
    BNE     _step_install
    LSRI    r10, r6, 20
    ANDI    r10, r10, 0x0F
    CMPI    r10, 15
    BNE     _step_install
    LSRI    r10, r6, 16
    ANDI    r10, r10, 0x0F
    LSLI    r10, r10, 2
    ADD     r10, r4, r10
    LW      r8, [r10]

_step_install:
    ; Patch SYSCALL at next_pc, save original instruction
    LW      r9, [r8]
    MOVI32  r0, STEP_BP
    SW      r8, [r0]
    SW      r9, [r0+4]
    MOVI32  r9, SYSCALL_INSTR
    SW      r9, [r8]
    MOVI32  r0, STEP_ACTIVE
    MOVI    r1, 1
    SW      r1, [r0]
    ; Reinstall regular BPs except at current PC (r5) — that instruction was
    ; already restored when its BP fired; re-patching it would re-trap instead
    ; of executing it.
    MOV     r0, r5
    CALL    _install_all_bps
    BA      _resume_user

; ── Exception / SYSCALL handler ──────────────────────────────────────────────
; On entry: r0-r14 = user values (r14 is clobbered as scratch)
;           r15 = this handler, STATUS = supervisor, IE = 0
_exception_handler:
    ; Point r14 at USER_CTX and save r0-r13
    MOVI32  r14, USER_CTX
    SW      r0,  [r14]
    SW      r1,  [r14+4]
    SW      r2,  [r14+8]
    SW      r3,  [r14+12]
    SW      r4,  [r14+16]
    SW      r5,  [r14+20]
    SW      r6,  [r14+24]
    SW      r7,  [r14+28]
    SW      r8,  [r14+32]
    SW      r9,  [r14+36]
    SW      r10, [r14+40]
    SW      r11, [r14+44]
    SW      r12, [r14+48]
    SW      r13, [r14+52]
    ; r14 is the scratch register — mark as lost
    MOVI32  r0, 0xDEADBEEF
    SW      r0,  [r14+56]

    ; Read EPC, EFLAGS, CAUSE from supervisor registers
    MOVI32  r0, SV_BASE
    LW      r1, [r0]            ; EPC
    LW      r2, [r0+4]          ; EFLAGS
    LW      r3, [r0+12]         ; CAUSE

    ; Saved PC: SYSCALL → EPC-4 (EPC = instr+4); others → EPC (emulator fix)
    MOV     r4, r1
    CMPI    r3, 3
    BNE     _exc_save
    SUBI    r4, r4, 4

_exc_save:
    MOVI32  r0, USER_CTX
    SW      r4, [r0+60]         ; ctx.pc
    SW      r2, [r0+64]         ; ctx.flags
    MOVI32  sp, MON_SP

    ; Non-SYSCALL: print exception info then show regs
    CMPI    r3, 3
    BEQ     _exc_syscall
    MOVI32  r0, _str_exc_cause
    CALL    _puts
    MOV     r0, r3
    CALL    _print_hex32
    MOVI32  r0, _str_exc_at
    CALL    _puts
    MOV     r0, r4
    CALL    _print_hex32
    CALL    _print_newline
    BA      _cmd_regs

_exc_syscall:
    ; r4 = address of the SYSCALL instruction that fired
    ; Check step_active
    MOVI32  r0, STEP_ACTIVE
    LW      r1, [r0]
    CMPI    r1, 0
    BEQ     _exc_check_bp

    ; Step BP: restore original instruction, clear flag
    MOVI32  r0, STEP_BP
    LW      r1, [r0]
    LW      r2, [r0+4]
    SW      r2, [r1]
    MOVI32  r0, STEP_ACTIVE
    MOVI    r1, 0
    SW      r1, [r0]
    MOVI32  r0, _str_stepped
    CALL    _puts
    BA      _cmd_regs

_exc_check_bp:
    ; Regular BP: find which slot matches the saved PC (r4)
    MOVI32  r5, BP_TABLE
    MOVI    r6, 0
_exc_scan:
    CMPI    r6, MAX_BPS
    BGE     _cmd_regs           ; not found — just show context
    LW      r7, [r5]
    CMP     r7, r4
    BNE     _exc_scan_next
    ; Restore original instruction
    LW      r8, [r5+4]
    SW      r8, [r7]
    MOVI32  r0, _str_bp_hit
    CALL    _puts
    MOV     r0, r7
    CALL    _print_hex32
    CALL    _print_newline
    BA      _cmd_regs
_exc_scan_next:
    ADDI    r5, r5, 8
    ADDI    r6, r6, 1
    BA      _exc_scan

; ── _install_all_bps — patch SYSCALL at every active BP ──────────────────────
; r0 = address to skip (pass 0 to reinstall all)
_install_all_bps:
    PUSH    lr
    PUSH    r4
    MOV     r4, r0              ; r4 = skip address
    MOVI32  r0, BP_TABLE
    MOVI    r1, 0
_iabp_loop:
    CMPI    r1, MAX_BPS
    BGE     _iabp_done
    LW      r2, [r0]
    CMPI    r2, 0
    BEQ     _iabp_next
    CMP     r2, r4              ; skip the BP at the step-from address
    BEQ     _iabp_next
    MOVI32  r3, SYSCALL_INSTR
    SW      r3, [r2]
_iabp_next:
    ADDI    r0, r0, 8
    ADDI    r1, r1, 1
    BA      _iabp_loop
_iabp_done:
    POP     r4
    POP     lr
    RET

; ── _eval_branch_cond: r10=cond (0-10), r9=flags -> r0=1 if taken ────────────
_eval_branch_cond:
    PUSH    lr
    PUSH    r4
    PUSH    r5
    ; Extract flag bits: N=bit31, Z=bit30, C=bit29, V=bit28
    LSRI    r1, r9, 31
    ANDI    r1, r1, 1           ; N
    LSRI    r2, r9, 30
    ANDI    r2, r2, 1           ; Z
    LSRI    r3, r9, 29
    ANDI    r3, r3, 1           ; C
    LSRI    r4, r9, 28
    ANDI    r4, r4, 1           ; V
    MOVI    r0, 0
    CMPI    r10, 0              ; BEQ: Z
    BNE     _ebc1
    MOV     r0, r2
    BA      _ebc_done
_ebc1:
    CMPI    r10, 1              ; BNE: !Z
    BNE     _ebc2
    XORI    r0, r2, 1
    BA      _ebc_done
_ebc2:
    CMPI    r10, 2              ; BLT: N^V
    BNE     _ebc3
    XOR     r0, r1, r4
    BA      _ebc_done
_ebc3:
    CMPI    r10, 3              ; BGE: !(N^V)
    BNE     _ebc4
    XOR     r0, r1, r4
    XORI    r0, r0, 1
    BA      _ebc_done
_ebc4:
    CMPI    r10, 4              ; BLTU: C
    BNE     _ebc5
    MOV     r0, r3
    BA      _ebc_done
_ebc5:
    CMPI    r10, 5              ; BGEU: !C
    BNE     _ebc6
    XORI    r0, r3, 1
    BA      _ebc_done
_ebc6:
    CMPI    r10, 6              ; BA: always
    BNE     _ebc7
    MOVI    r0, 1
    BA      _ebc_done
_ebc7:
    CMPI    r10, 7              ; BGT: !Z && !(N^V)
    BNE     _ebc8
    XOR     r5, r1, r4
    OR      r5, r5, r2          ; Z | (N^V) — any 1 means NOT taken
    CMPI    r5, 0
    MOVI    r0, 0
    BNE     _ebc_done
    MOVI    r0, 1
    BA      _ebc_done
_ebc8:
    CMPI    r10, 8              ; BLE: Z || (N^V)
    BNE     _ebc9
    XOR     r5, r1, r4
    OR      r0, r5, r2
    CMPI    r0, 0
    MOVI    r0, 0
    BEQ     _ebc_done
    MOVI    r0, 1
    BA      _ebc_done
_ebc9:
    CMPI    r10, 9              ; BGTU: !C && !Z
    BNE     _ebc10
    OR      r5, r3, r2
    CMPI    r5, 0
    MOVI    r0, 0
    BNE     _ebc_done
    MOVI    r0, 1
    BA      _ebc_done
_ebc10:                         ; BLEU: C || Z
    OR      r0, r3, r2
    CMPI    r0, 0
    MOVI    r0, 0
    BEQ     _ebc_done
    MOVI    r0, 1
_ebc_done:
    POP     r5
    POP     r4
    POP     lr
    RET

; ── _dump_mem: r0=addr, r1=len ────────────────────────────────────────────────
_dump_mem:
    PUSH    lr
    PUSH    r4
    PUSH    r5
    PUSH    r6
    PUSH    r7
    PUSH    r8
    MOV     r4, r0              ; r4 = current addr
    MOV     r5, r1              ; r5 = remaining bytes
_dm_row:
    CMPI    r5, 0
    BLE     _dm_done
    ; row_len = min(16, r5)
    MOVI    r6, 16
    CMP     r5, r6
    BGE     _dm_row_16
    MOV     r6, r5
_dm_row_16:
    MOV     r0, r4
    CALL    _print_hex32
    MOVI32  r0, _str_col_sp
    CALL    _puts
    ; Hex bytes (16 columns)
    MOVI    r7, 0
_dm_hex:
    CMPI    r7, 16
    BGE     _dm_ascii_start
    CMP     r7, r6
    BGE     _dm_hex_pad
    LBUX    r0, [r4+r7]
    CALL    _print_hex8
    MOVI32  r0, _str_sp
    CALL    _puts
    BA      _dm_hex_next
_dm_hex_pad:
    MOVI32  r0, _str_three_sp
    CALL    _puts
_dm_hex_next:
    ADDI    r7, r7, 1
    BA      _dm_hex
_dm_ascii_start:
    MOVI32  r0, _str_sp
    CALL    _puts
    MOVI    r7, 0
_dm_ascii:
    CMP     r7, r6
    BGE     _dm_row_end
    LBUX    r0, [r4+r7]
    CMPI    r0, 32              ; ' '
    BLT     _dm_dot
    CMPI    r0, 126             ; '~'
    BGT     _dm_dot
    CALL    _putc
    BA      _dm_ascii_next
_dm_dot:
    MOVI    r0, 46              ; '.'
    CALL    _putc
_dm_ascii_next:
    ADDI    r7, r7, 1
    BA      _dm_ascii
_dm_row_end:
    CALL    _print_newline
    ADD     r4, r4, r6
    SUB     r5, r5, r6
    BA      _dm_row
_dm_done:
    POP     r8
    POP     r7
    POP     r6
    POP     r5
    POP     r4
    POP     lr
    RET

; ── _skip_spaces: r0=ptr -> r0=ptr past spaces ───────────────────────────────
_skip_spaces:
    LBU     r1, [r0]
    CMPI    r1, 32
    BNE     _ss_done
    ADDI    r0, r0, 1
    BA      _skip_spaces
_ss_done:
    RET

; ── _str_startswith: r0=haystack, r2=prefix -> r0=ptr-past-prefix or 0 ───────
_str_startswith:
    PUSH    lr
    PUSH    r3
    PUSH    r4
    MOV     r3, r0
    MOV     r4, r2
_ssw_loop:
    LBU     r1, [r4]
    CMPI    r1, 0
    BEQ     _ssw_match
    LBU     r0, [r3]
    CMP     r0, r1
    BNE     _ssw_fail
    ADDI    r3, r3, 1
    ADDI    r4, r4, 1
    BA      _ssw_loop
_ssw_match:
    MOV     r0, r3
    POP     r4
    POP     r3
    POP     lr
    RET
_ssw_fail:
    MOVI    r0, 0
    POP     r4
    POP     r3
    POP     lr
    RET

; ── _parse_hex: r0=ptr -> r0=value, r1=ok(1/0), r2=cursor-past-digits ────────
_parse_hex:
    PUSH    lr
    PUSH    r4
    MOV     r4, r0              ; r4 = cursor
    MOVI    r0, 0               ; accumulator
    MOVI    r1, 0               ; digit count
    ; Skip optional "0x"/"0X"
    LBU     r2, [r4]
    CMPI    r2, 48              ; '0'
    BNE     _ph_loop
    LBU     r2, [r4+1]
    CMPI    r2, 120             ; 'x'
    BEQ     _ph_0x
    CMPI    r2, 88              ; 'X'
    BNE     _ph_loop
_ph_0x:
    ADDI    r4, r4, 2
_ph_loop:
    LBU     r2, [r4]
    CMPI    r2, 48              ; < '0'
    BLT     _ph_done
    CMPI    r2, 57              ; <= '9'
    BGT     _ph_upper
    SUBI    r2, r2, 48
    BA      _ph_digit
_ph_upper:
    CMPI    r2, 65              ; 'A'
    BLT     _ph_done
    CMPI    r2, 70              ; 'F'
    BGT     _ph_lower
    SUBI    r2, r2, 55          ; 'A'=65 -> 10, so 65-55=10
    BA      _ph_digit
_ph_lower:
    CMPI    r2, 97              ; 'a'
    BLT     _ph_done
    CMPI    r2, 102             ; 'f'
    BGT     _ph_done
    SUBI    r2, r2, 87          ; 'a'=97 -> 10, so 97-87=10
_ph_digit:
    LSLI    r0, r0, 4
    OR      r0, r0, r2
    ADDI    r1, r1, 1
    ADDI    r4, r4, 1
    BA      _ph_loop
_ph_done:
    MOV     r2, r4              ; r2 = cursor past digits
    CMPI    r1, 0
    MOVI    r1, 0
    BEQ     _ph_ret
    MOVI    r1, 1
_ph_ret:
    POP     r4
    POP     lr
    RET

; ── _read_line: r0=buf, r1=max — reads one line, null-terminated, echoed ─────
_read_line:
    PUSH    lr
    PUSH    r4
    PUSH    r5
    PUSH    r6
    MOV     r4, r0
    MOV     r5, r1
    MOVI    r6, 0               ; count
_rl_loop:
    CALL    _getc
    CMPI    r0, 13              ; CR: skip
    BEQ     _rl_loop
    CMPI    r0, 10              ; LF: end
    BEQ     _rl_eol
    CMPI    r0, 8               ; BS
    BEQ     _rl_bs
    CMPI    r0, 127             ; DEL
    BEQ     _rl_bs
    SUBI    r1, r5, 1
    CMP     r6, r1
    BGE     _rl_loop            ; buffer full: keep reading, don't store
    CALL    _putc               ; echo
    SB      r0, [r4]
    ADDI    r4, r4, 1
    ADDI    r6, r6, 1
    BA      _rl_loop
_rl_bs:
    CMPI    r6, 0
    BEQ     _rl_loop
    MOVI    r0, 8
    CALL    _putc
    MOVI    r0, 32
    CALL    _putc
    MOVI    r0, 8
    CALL    _putc
    SUBI    r4, r4, 1
    SUBI    r6, r6, 1
    BA      _rl_loop
_rl_eol:
    MOVI    r0, 0
    SB      r0, [r4]
    MOVI    r0, 10
    CALL    _putc
    POP     r6
    POP     r5
    POP     r4
    POP     lr
    RET

; ── _putc: r0=char ───────────────────────────────────────────────────────────
_putc:
    PUSH    r1
    PUSH    r2
    MOVI32  r1, UART_STATUS
_putc_wait:
    LW      r2, [r1]
    ANDI    r2, r2, 1
    CMPI    r2, 0
    BEQ     _putc_wait
    MOVI32  r1, UART_TX
    SW      r0, [r1]
    POP     r2
    POP     r1
    RET

; ── _getc: -> r0=char (blocking) ─────────────────────────────────────────────
_getc:
    PUSH    r1
    PUSH    r2
    MOVI32  r1, UART_STATUS
_getc_wait:
    LW      r2, [r1]
    ANDI    r2, r2, 2           ; RX available
    CMPI    r2, 0
    BEQ     _getc_wait
    MOVI32  r1, UART_RX
    LW      r0, [r1]
    POP     r2
    POP     r1
    RET

; ── _puts: r0=null-terminated string ─────────────────────────────────────────
_puts:
    PUSH    lr
    PUSH    r2
    MOV     r2, r0
_puts_loop:
    LBU     r0, [r2]
    CMPI    r0, 0
    BEQ     _puts_done
    CALL    _putc
    ADDI    r2, r2, 1
    BA      _puts_loop
_puts_done:
    POP     r2
    POP     lr
    RET

; ── _print_hex32: r0=value — prints 8 hex digits (no "0x" prefix) ────────────
_print_hex32:
    PUSH    lr
    PUSH    r4
    PUSH    r5
    MOV     r4, r0
    MOVI    r5, 28
_ph32_loop:
    LSRR    r0, r4, r5
    ANDI    r0, r0, 0xF
    CALL    _nibble_char
    CALL    _putc
    CMPI    r5, 0
    BEQ     _ph32_done
    SUBI    r5, r5, 4
    BA      _ph32_loop
_ph32_done:
    POP     r5
    POP     r4
    POP     lr
    RET

; ── _print_hex8: r0=byte — prints 2 hex digits ───────────────────────────────
_print_hex8:
    PUSH    lr
    PUSH    r4
    MOV     r4, r0
    LSRI    r0, r4, 4
    ANDI    r0, r0, 0xF
    CALL    _nibble_char
    CALL    _putc
    ANDI    r0, r4, 0xF
    CALL    _nibble_char
    CALL    _putc
    POP     r4
    POP     lr
    RET

; ── _nibble_char: r0=0..15 -> r0=ASCII ───────────────────────────────────────
_nibble_char:
    CMPI    r0, 10
    BGE     _nc_alpha
    ADDI    r0, r0, 48          ; '0' = 48
    RET
_nc_alpha:
    ADDI    r0, r0, 87          ; 'a'-10 = 87
    RET

; ── _print_newline ────────────────────────────────────────────────────────────
_print_newline:
    PUSH    lr
    MOVI    r0, 10
    CALL    _putc
    POP     lr
    RET

_bad_arg:
    MOVI32  r0, _str_bad_arg
    CALL    _puts
    BA      _cmd_loop

; ── String constants ──────────────────────────────────────────────────────────
_str_banner:
    .asciiz "\r\nDodgyCPU ROM Monitor\r\nCommands: s[tep]  c[ont]  regs  break <hex>  mem <hex> [<hex>]  pc <hex>\r\n"
_str_prompt:    .asciiz "> "
_str_s:         .asciiz "s"
_str_c:         .asciiz "c"
_str_regs:      .asciiz "regs"
_str_break:     .asciiz "break"
_str_mem:       .asciiz "mem"
_str_pc_cmd:    .asciiz "pc"
_str_help:      .asciiz "Commands: s[tep]  c[ont]  regs  break <hex>  mem <hex> [<hex>]  pc <hex>\r\n"
_str_bad_arg:   .asciiz "bad argument\r\n"
_str_bp_full:   .asciiz "breakpoint table full\r\n"
_str_bp_set:    .asciiz "bp @ 0x"
_str_bp_hit:    .asciiz "bp hit @ 0x"
_str_stepped:   .asciiz "stepped\r\n"
_str_exc_cause: .asciiz "exception cause=0x"
_str_exc_at:    .asciiz " pc=0x"
_str_eq:        .asciiz " = 0x"
_str_col_sp:    .asciiz "  "
_str_sp:        .asciiz " "
_str_three_sp:  .asciiz "   "
_str_flags_lbl: .asciiz "flags    = 0x"
_str_pc_set:    .asciiz "pc updated\r\n"

; ── Register name pointer table ───────────────────────────────────────────────
_reg_name_table:
    .word _rn_r0
    .word _rn_r1
    .word _rn_r2
    .word _rn_r3
    .word _rn_r4
    .word _rn_r5
    .word _rn_r6
    .word _rn_r7
    .word _rn_r8
    .word _rn_r9
    .word _rn_r10
    .word _rn_r11
    .word _rn_r12
    .word _rn_sp
    .word _rn_lr
    .word _rn_pc

_rn_r0:  .asciiz "r0  "
_rn_r1:  .asciiz "r1  "
_rn_r2:  .asciiz "r2  "
_rn_r3:  .asciiz "r3  "
_rn_r4:  .asciiz "r4  "
_rn_r5:  .asciiz "r5  "
_rn_r6:  .asciiz "r6  "
_rn_r7:  .asciiz "r7  "
_rn_r8:  .asciiz "r8  "
_rn_r9:  .asciiz "r9  "
_rn_r10: .asciiz "r10 "
_rn_r11: .asciiz "r11 "
_rn_r12: .asciiz "r12 "
_rn_sp:  .asciiz "sp  "
_rn_lr:  .asciiz "lr  "
_rn_pc:  .asciiz "pc  "
