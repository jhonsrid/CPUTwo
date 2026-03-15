; DodgyCPU Preemptive Kernel
;
; Three concurrent user tasks demonstrate:
;   Task 0 — counting loop, timer-preempted, prints progress, calls sys_exit
;   Task 1 — prints a few times then divides by zero (fault-kill demo)
;   Task 2 — short bursts with sys_yield between each, clean sys_exit
;
; ── Kernel supervisor ABI ──────────────────────────────────────────────────────
; In supervisor context (inside any exception handler or the scheduler) the
; following register conventions apply on top of the normal calling convention:
;
;   r11  Current TCB pointer.  Set by _exception_entry immediately after the
;        context save and held for the lifetime of the handler.  All handlers
;        that need the TCB use r11; _schedule computes its own TCB pointer
;        independently.  r11 is callee-saved by the kernel utility functions,
;        so it survives CALL.
;
;   r14  Hardware scratch / link register.  r14 is CLOBBERED on every
;        exception entry — the hardware uses it as the scratch register before
;        the first instruction of the handler executes, so the user's lr value
;        is irrecoverably lost.  This means:
;          • User tasks must treat lr as volatile across any SYSCALL boundary.
;            A task that stores a value in lr and then issues a SYSCALL will
;            not see that value restored on return.
;          • Inside the kernel, never use r14 to hold a value that must survive
;            a CALL instruction — CALL writes the return address into r14.
;            Use r11 (TCB ptr) or a stack slot instead.
;
; Syscall ABI:  r0 = number (0=exit, 1=write r1=buf r2=len, 2=yield)
;               return value in r0 (written to TCB.saved_r0 before resume)
;
; Memory layout:
;   0x00000000  kernel code (_start)
;   0x00001000  EVEC table (8 × 4 bytes)
;   0x00001020  kernel data: current_task(+0), num_tasks(+4), r0_scratch(+8)
;   0x00001030  TCB array (3 × 80 bytes = 240 bytes, ends at 0x00001120)
;   0x00001200  kernel stack top (grows down)
;   0x00002000  task 0 stack top
;   0x00003000  task 1 stack top
;   0x00004000  task 2 stack top
;   0x00010000  task 0 code
;   0x00011000  task 1 code
;   0x00012000  task 2 code

; ── MMIO ──────────────────────────────────────────────────────────────────────
.equ UART_STATUS,   0x03F00000
.equ UART_TX,       0x03F00004
.equ UART_RX,       0x03F00008
.equ UART_CTRL,     0x03F0000C
.equ TIMER_PERIOD,  0x03F01000
.equ TIMER_CTRL,    0x03F01004
.equ IC_PENDING,    0x03F02000
.equ IC_MASK,       0x03F02004
.equ IC_ACK,        0x03F02008
.equ SV_BASE,       0x03FFF000
.equ SV_EPC,        0x03FFF000
.equ SV_EFLAGS,     0x03FFF004
.equ SV_EVEC,       0x03FFF008
.equ SV_CAUSE,      0x03FFF00C
.equ SV_STATUS,     0x03FFF010

; ── Kernel layout ─────────────────────────────────────────────────────────────
.equ EVEC_BASE,     0x00001000
.equ KDATA_BASE,    0x00001020
.equ KDATA_CURTASK, 0x00001020   ; current task index (word)
.equ KDATA_NTASKS,  0x00001024   ; total number of tasks (word)
.equ KDATA_R0SCRATCH, 0x00001028 ; temp cell for r0 during exception entry
.equ TCB_BASE,      0x00001030
.equ KERN_SP,       0x00001200

; ── TCB field offsets (80 bytes / 0x50 per TCB) ───────────────────────────────
.equ TCB_SIZE,      80
.equ TCB_R0,        0
.equ TCB_R1,        4
.equ TCB_R2,        8
.equ TCB_R3,        12
.equ TCB_R4,        16
.equ TCB_R5,        20
.equ TCB_R6,        24
.equ TCB_R7,        28
.equ TCB_R8,        32
.equ TCB_R9,        36
.equ TCB_R10,       40
.equ TCB_R11,       44
.equ TCB_R12,       48
.equ TCB_SP,        52
.equ TCB_LR,        56           ; not saved (lost on exception entry)
.equ TCB_PC,        60
.equ TCB_FLAGS,     64
.equ TCB_STATE,     68
.equ TCB_ID,        72
; TCB state values
.equ STATE_RUNNABLE, 0
.equ STATE_ZOMBIE,   1

; ── Task stacks / entry points ────────────────────────────────────────────────
.equ TASK0_SP,      0x00002000
.equ TASK1_SP,      0x00003000
.equ TASK2_SP,      0x00004000
.equ TASK0_ENTRY,   0x00010000
.equ TASK1_ENTRY,   0x00011000
.equ TASK2_ENTRY,   0x00012000
.equ NUM_TASKS,     3
.equ TIMER_QUANTUM, 500

    .org 0x00000000

; ── _start ────────────────────────────────────────────────────────────────────
_start:
    MOVI32  sp, KERN_SP

    ; Point EVEC at our table
    MOVI32  r0, SV_BASE
    MOVI32  r1, EVEC_BASE
    SW      r1, [r0+8]

    ; Fill all 8 EVEC entries with _exception_entry
    MOVI32  r0, EVEC_BASE
    MOVI32  r1, _exception_entry
    SW      r1, [r0+0]
    SW      r1, [r0+4]
    SW      r1, [r0+8]
    SW      r1, [r0+12]
    SW      r1, [r0+16]
    SW      r1, [r0+20]
    SW      r1, [r0+24]
    SW      r1, [r0+28]

    ; Kernel data: current_task = NUM_TASKS-1 so first schedule() wraps to 0
    MOVI32  r0, KDATA_BASE
    MOVI    r1, 2               ; NUM_TASKS - 1
    SW      r1, [r0+0]
    MOVI    r1, NUM_TASKS
    SW      r1, [r0+4]
    MOVI    r1, 0
    SW      r1, [r0+8]          ; r0_scratch = 0

    ; Initialise all three TCBs via _create_task(r0=entry, r1=sp_top, r2=id)
    MOVI32  r0, TASK0_ENTRY
    MOVI32  r1, TASK0_SP
    MOVI    r2, 0
    CALL    _create_task

    MOVI32  r0, TASK1_ENTRY
    MOVI32  r1, TASK1_SP
    MOVI    r2, 1
    CALL    _create_task

    MOVI32  r0, TASK2_ENTRY
    MOVI32  r1, TASK2_SP
    MOVI    r2, 2
    CALL    _create_task

    ; Configure timer
    MOVI32  r0, TIMER_PERIOD
    MOVI32  r1, TIMER_QUANTUM
    SW      r1, [r0]            ; write period (also starts countdown)
    MOVI32  r0, TIMER_CTRL
    MOVI    r1, 3               ; bit0=enable, bit1=irq enable
    SW      r1, [r0]

    ; Enable timer in IC
    MOVI32  r0, IC_MASK
    MOVI    r1, 1               ; bit 0 = timer
    SW      r1, [r0]

    ; Print banner
    MOVI32  r0, _str_banner
    CALL    _kern_puts

    ; Jump into scheduler (IE still 0; SYSRET sets it to 1 when first task runs)
    BA      _schedule

; ── _create_task: initialise a TCB ────────────────────────────────────────────
; r0 = entry PC, r1 = stack top, r2 = task id (0-based)
; All other saved fields default to 0 (memory is zero at reset).
_create_task:
    PUSH    lr
    PUSH    r3
    PUSH    r4
    ; r3 = &TCB[r2] = TCB_BASE + r2 * TCB_SIZE
    MOVI32  r3, TCB_SIZE
    MUL     r3, r2, r3
    MOVI32  r4, TCB_BASE
    ADD     r3, r3, r4
    SW      r0, [r3+TCB_PC]
    SW      r1, [r3+TCB_SP]
    MOVI    r4, STATE_RUNNABLE
    SW      r4, [r3+TCB_STATE]
    SW      r2, [r3+TCB_ID]
    POP     r4
    POP     r3
    POP     lr
    RET

; ── Exception entry ────────────────────────────────────────────────────────────
; Hardware on entry: r0-r13 = user values, r14 = scratch (user lr lost),
;                    r15 = this handler, STATUS = supervisor, IE = 0
_exception_entry:
    ; Step 1 — save r0 to scratch cell (r14 free to use as pointer)
    MOVI32  r14, KDATA_R0SCRATCH
    SW      r0, [r14]

    ; Step 2 — compute TCB address of current task into r14
    ;          r14 = TCB_BASE + current_task * TCB_SIZE
    MOVI32  r0, KDATA_CURTASK
    LW      r0, [r0]            ; r0 = current_task index
    MOVI32  r14, TCB_SIZE
    MUL     r0, r0, r14         ; r0 = index * 80
    MOVI32  r14, TCB_BASE
    ADD     r14, r14, r0        ; r14 = &TCB[current_task]

    ; Step 3 — restore r0 from scratch, then save all registers to TCB
    MOVI32  r0, KDATA_R0SCRATCH
    LW      r0, [r0]            ; r0 = original user r0

    SW      r0,  [r14+0]
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
    ; r14 (lr) is the scratch register — mark as lost with sentinel
    MOVI32  r0, 0xDEADBEEF
    SW      r0, [r14+56]

    ; Step 4 — read supervisor registers: EPC, EFLAGS, CAUSE
    MOVI32  r0, SV_BASE
    LW      r1, [r0+0]          ; r1 = EPC
    LW      r2, [r0+4]          ; r2 = EFLAGS
    LW      r3, [r0+12]         ; r3 = CAUSE

    ; EPC semantics:
    ;   SYSCALL (cause 3): EPC = PC+4 (return address past SYSCALL) — correct as-is
    ;   IRQ     (cause 6): EPC = r[15] at time of IRQ = NEXT instr  — correct as-is
    ;   Faults            : EPC = faulting instruction address       — task will be killed
    SW      r1, [r14+TCB_PC]    ; TCB.saved_pc = EPC
    SW      r2, [r14+TCB_FLAGS] ; TCB.saved_flags = EFLAGS

    ; Establish kernel stack
    MOVI32  sp, KERN_SP

    ; Move TCB pointer from r14 into r11 (the designated kernel TCB register).
    ; r14 must not be used as the TCB pointer beyond this point: every CALL
    ; instruction writes the return address into r14, silently destroying it.
    MOV     r11, r14

    ; Dispatch
    CMPI    r3, 3
    BEQ     _syscall_dispatch
    CMPI    r3, 6
    BEQ     _irq_handler
    BA      _fault_handler

; ── IRQ handler ───────────────────────────────────────────────────────────────
_irq_handler:
    ; Acknowledge timer (bit 0) in IC before SYSRET (IC latch must be clear
    ; before IE goes high, or the interrupt re-fires immediately)
    MOVI32  r0, IC_ACK
    MOVI    r1, 1
    SW      r1, [r0]
    ; Current task stays RUNNABLE; schedule next
    BA      _schedule

; ── Fault handler ─────────────────────────────────────────────────────────────
; r3 = cause, r11 = current TCB pointer (kernel TCB register — see ABI above)
_fault_handler:
    MOVI32  r0, _str_fault
    CALL    _kern_puts
    LW      r0, [r11+TCB_ID]
    CALL    _kern_print_dec
    MOVI32  r0, _str_fault_cause
    CALL    _kern_puts
    MOV     r0, r3
    CALL    _kern_print_hex32
    MOVI32  r0, _str_fault_at
    CALL    _kern_puts
    LW      r0, [r11+TCB_PC]
    CALL    _kern_print_hex32
    MOVI    r0, 10
    CALL    _kern_putc
    MOVI    r0, STATE_ZOMBIE
    SW      r0, [r11+TCB_STATE]
    BA      _schedule

; ── Syscall dispatch ──────────────────────────────────────────────────────────
; r11 = current TCB pointer (kernel TCB register — see ABI above)
_syscall_dispatch:
    LW      r0, [r11+TCB_R0]    ; user r0 = syscall number
    CMPI    r0, 0
    BEQ     _sys_exit
    CMPI    r0, 1
    BEQ     _sys_write
    CMPI    r0, 2
    BEQ     _sys_yield
    ; Unknown syscall — kill task
    MOVI32  r0, _str_badsys
    CALL    _kern_puts
    MOVI    r0, STATE_ZOMBIE
    SW      r0, [r11+TCB_STATE]
    BA      _schedule

_sys_exit:
    MOVI32  r0, _str_task_pfx
    CALL    _kern_puts
    LW      r0, [r11+TCB_ID]
    CALL    _kern_print_dec
    MOVI32  r0, _str_exited
    CALL    _kern_puts
    MOVI    r0, STATE_ZOMBIE
    SW      r0, [r11+TCB_STATE]
    BA      _schedule

_sys_write:
    ; reload args from TCB (r14 would be gone; r11 = TCB is still valid)
    LW      r5, [r11+TCB_R1]    ; buf ptr
    LW      r6, [r11+TCB_R2]    ; length
    MOV     r7, r6              ; save for return value
    MOVI32  r8, UART_STATUS
    MOVI32  r9, UART_TX
_syswrite_loop:
    CMPI    r6, 0
    BLE     _syswrite_done
    LBU     r0, [r5]
_syswrite_txwait:
    LW      r1, [r8]
    ANDI    r1, r1, 1
    CMPI    r1, 0
    BEQ     _syswrite_txwait
    SW      r0, [r9]
    ADDI    r5, r5, 1
    SUBI    r6, r6, 1
    BA      _syswrite_loop
_syswrite_done:
    SW      r7, [r11+TCB_R0]    ; return value in saved r0
    BA      _schedule

_sys_yield:
    BA      _schedule

; ── Scheduler (round-robin) ────────────────────────────────────────────────────
; Finds next RUNNABLE task starting from current_task+1, updates current_task,
; restores context, and SYSRETs into the chosen task.
_schedule:
    MOVI32  r0, KDATA_BASE
    LW      r1, [r0+0]          ; r1 = current_task
    LW      r2, [r0+4]          ; r2 = num_tasks

    MOV     r3, r2              ; r3 = retry counter (try each slot once)
    MOV     r4, r1              ; r4 = candidate

_sched_loop:
    CMPI    r3, 0
    BEQ     _sched_all_dead

    ADDI    r4, r4, 1           ; advance candidate
    CMP     r4, r2
    BLT     _sched_nowrap
    MOVI    r4, 0               ; wrap to 0
_sched_nowrap:
    SUBI    r3, r3, 1

    ; TCB address of candidate: r5 = TCB_BASE + r4 * TCB_SIZE
    MOVI32  r5, TCB_SIZE
    MUL     r5, r4, r5
    MOVI32  r6, TCB_BASE
    ADD     r5, r5, r6          ; r5 = &TCB[candidate]

    LW      r6, [r5+68]         ; TCB.state
    CMPI    r6, STATE_RUNNABLE
    BNE     _sched_loop

_sched_found:
    ; Update current_task
    MOVI32  r0, KDATA_BASE
    SW      r4, [r0+0]

    ; Restore EPC and EFLAGS from TCB into supervisor registers
    LW      r0, [r5+60]         ; TCB.saved_pc
    LW      r1, [r5+64]         ; TCB.saved_flags
    MOVI32  r2, SV_BASE
    SW      r0, [r2+0]          ; SV_EPC = saved_pc
    SW      r1, [r2+4]          ; SV_EFLAGS = saved_flags

    ; Restore registers r1-r4 (use r5 as TCB ptr throughout)
    LW      r1,  [r5+4]
    LW      r2,  [r5+8]
    LW      r3,  [r5+12]
    LW      r4,  [r5+16]
    ; skip r5 for now
    LW      r6,  [r5+24]
    LW      r7,  [r5+28]
    LW      r8,  [r5+32]
    LW      r9,  [r5+36]
    LW      r10, [r5+40]
    LW      r11, [r5+44]
    LW      r12, [r5+48]
    LW      r13, [r5+52]
    ; r14 is not restored (documented lost on every trap)
    ; Restore r5 last (loses TCB ptr), then r0
    MOV     r0, r5              ; r0 = TCB ptr
    LW      r5, [r0+20]         ; r5 = saved_r5  (TCB ptr now gone)
    LW      r0, [r0+0]          ; r0 = saved_r0
    SYSRET

_sched_all_dead:
    MOVI32  r0, _str_alldead
    CALL    _kern_puts
    HALT

; ── Kernel UART utilities ──────────────────────────────────────────────────────
_kern_putc:
    ; r0 = char — busy-waits on TX ready
    PUSH    r1
    PUSH    r2
    MOVI32  r1, UART_STATUS
_kputc_wait:
    LW      r2, [r1]
    ANDI    r2, r2, 1
    CMPI    r2, 0
    BEQ     _kputc_wait
    MOVI32  r1, UART_TX
    SW      r0, [r1]
    POP     r2
    POP     r1
    RET

_kern_puts:
    ; r0 = null-terminated string
    PUSH    lr
    PUSH    r2
    MOV     r2, r0
_kputs_loop:
    LBU     r0, [r2]
    CMPI    r0, 0
    BEQ     _kputs_done
    CALL    _kern_putc
    ADDI    r2, r2, 1
    BA      _kputs_loop
_kputs_done:
    POP     r2
    POP     lr
    RET

_kern_print_hex32:
    ; r0 = 32-bit value, prints 8 hex digits
    PUSH    lr
    PUSH    r4
    PUSH    r5
    MOV     r4, r0
    MOVI    r5, 28
_kph32_loop:
    LSRR    r0, r4, r5
    ANDI    r0, r0, 0xF
    CALL    _kern_nibble_char
    CALL    _kern_putc
    CMPI    r5, 0
    BEQ     _kph32_done
    SUBI    r5, r5, 4
    BA      _kph32_loop
_kph32_done:
    POP     r5
    POP     r4
    POP     lr
    RET

_kern_nibble_char:
    ; r0 = 0..15 -> r0 = ASCII
    CMPI    r0, 10
    BGE     _knc_alpha
    ADDI    r0, r0, 48          ; '0'
    RET
_knc_alpha:
    ADDI    r0, r0, 87          ; 'a' - 10
    RET

_kern_print_dec:
    ; r0 = small non-negative integer (0..9 only — sufficient for task IDs)
    PUSH    lr
    ADDI    r0, r0, 48          ; '0'
    CALL    _kern_putc
    POP     lr
    RET

; ── Kernel strings ─────────────────────────────────────────────────────────────
_str_banner:
    .asciiz "\r\nDodgyCPU Kernel\r\n"
_str_fault:
    .asciiz "FAULT task "
_str_fault_cause:
    .asciiz " cause=0x"
_str_fault_at:
    .asciiz " pc=0x"
_str_task_pfx:
    .asciiz "T"
_str_exited:
    .asciiz ": exited\r\n"
_str_badsys:
    .asciiz "unknown syscall\r\n"
_str_alldead:
    .asciiz "\r\nkernel: all tasks done\r\n"

; ─────────────────────────────────────────────────────────────────────────────
; Task 0 — counting loop, prints "T0:N\n" every 200 iterations, exits at 1000
; ─────────────────────────────────────────────────────────────────────────────
    .org 0x00010000
task0:
    MOVI32  r4, 0               ; r4 = counter
    MOVI32  r5, 200             ; print interval
    MOVI32  r6, 1000            ; exit threshold
task0_loop:
    ADDI    r4, r4, 1
    ; Check for print (counter % 200 == 0)
    MOV     r0, r4
    MOVI32  r1, 200
    MODU    r0, r0, r1
    CMPI    r0, 0
    BNE     task0_check_exit
    ; sys_write "T0:" + decimal_of_(r4/200) + "\n"
    ; Build message in task0_msg buffer
    MOVI32  r0, task0_msg+3     ; point to digit position
    ; r4/200 => quotient fits in a single digit for 1..5
    MOV     r1, r4
    MOVI32  r2, 200
    DIVU    r1, r1, r2          ; r1 = iteration number (1..5)
    ADDI    r1, r1, 48          ; to ASCII digit
    SB      r1, [r0]
    ; sys_write(task0_msg, 5)
    MOVI    r0, 1
    MOVI32  r1, task0_msg
    MOVI    r2, 5
    SYSCALL
task0_check_exit:
    CMP     r4, r6
    BLT     task0_loop
    MOVI    r0, 0               ; sys_exit
    SYSCALL
task0_loop_end:
    BA      task0_loop_end      ; should never reach here

    .align 4
task0_msg:
    .ascii "T0:0\n"             ; byte [3] is the digit, patched at runtime

; ─────────────────────────────────────────────────────────────────────────────
; Task 1 — prints "T1:alive\n" 3 times, then divides by zero (fault demo)
; ─────────────────────────────────────────────────────────────────────────────
    .org 0x00011000
task1:
    MOVI32  r4, 0               ; iteration counter
task1_loop:
    ADDI    r4, r4, 1
    ; sys_write "T1:alive\n"
    MOVI    r0, 1
    MOVI32  r1, task1_msg
    MOVI    r2, 9
    SYSCALL
    ; On 4th iteration: divide by zero
    CMPI    r4, 3
    BLE     task1_loop
    ; Intentional divide by zero — triggers fault (cause 4), kernel kills task
    MOVI    r0, 1
    MOVI    r1, 0
    DIV     r0, r0, r1
    ; Never reached
task1_spin:
    BA      task1_spin

    .align 4
task1_msg:
    .ascii "T1:alive\n"

; ─────────────────────────────────────────────────────────────────────────────
; Task 2 — yields 5 times printing "T2:yieldN\n", then sys_exit
; ─────────────────────────────────────────────────────────────────────────────
    .org 0x00012000
task2:
    MOVI32  r4, 0               ; yield counter
task2_loop:
    CMPI    r4, 5
    BGE     task2_done
    ; Patch digit in message
    MOVI32  r0, task2_msg+8
    MOV     r1, r4
    ADDI    r1, r1, 49          ; '1'..'5'
    SB      r1, [r0]
    ; sys_write "T2:yieldN\n"
    MOVI    r0, 1
    MOVI32  r1, task2_msg
    MOVI    r2, 10
    SYSCALL
    ADDI    r4, r4, 1
    MOVI    r0, 2               ; sys_yield
    SYSCALL
    BA      task2_loop
task2_done:
    MOVI    r0, 0               ; sys_exit
    SYSCALL
task2_spin:
    BA      task2_spin

    .align 4
task2_msg:
    .ascii "T2:yield1\n"
