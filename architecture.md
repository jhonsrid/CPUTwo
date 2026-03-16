# CPUTwo Architecture

## Overview

A 32-bit RISC-inspired architecture targeting Linux/UNIX portability and simple emulator implementation. Deliberately minimal — enough power to run real software, not more.

---

## Reset State

At power-on / emulator start, all registers and memory are initialised to zero except STATUS:

- `PC` (`r15`) = `0x00000000` — execution begins at address 0
- `STATUS` = `0x01` — **supervisor mode**, interrupts disabled (IE = 0)
- All general-purpose registers (`r0`–`r14`) = 0
- `flags` = 0
- `EPC`, `EFLAGS`, `EVEC`, `CAUSE` = 0

Starts in supervisor mode so startup code can initialise `EVEC` and the stack before dropping to user mode. No interrupts fire until software sets `STATUS.IE` = 1.

`SP` (`r13`) resets to zero, which is not a usable stack address. **Startup code at address 0 must initialise SP before making any subroutine call.** The conventional value is `0x03F00000` — the base of the MMIO region, one word past the top of usable RAM — so the first push lands at `0x03EFFFFC`.

---

## Word Size & Memory

- **32-bit** words and addresses
- **64 MB** addressable space (`0x00000000`–`0x03FFFFFF`); addresses ≥ `0x04000000` cause a bus error (cause 0x02)
- **Byte-addressable**, little-endian (least-significant byte at lowest address)
- Memory-mapped I/O (no separate I/O instructions)

---

## Registers

### General & Special Purpose

| Name | Width | Purpose |
|---|---|---|
| `r0`–`r12` | 32-bit | General purpose |
| `r13` / `sp` | 32-bit | Stack pointer |
| `r14` / `lr` | 32-bit | Link register (return address) |
| `r15` / `pc` | 32-bit | Program counter |
| `flags` | 32-bit | Status flags (N, Z, C, V + reserved) |

13 GPRs keeps the register file tiny but is enough for a C ABI. `r0` is not hardwired to zero.

### Flags Register Bit Layout

| Bit | Flag | Meaning |
|---|---|---|
| 31 | N | Negative — result was negative (sign bit set) |
| 30 | Z | Zero — result was zero |
| 29 | C | Carry — unsigned carry or borrow out |
| 28 | V | Overflow — signed overflow |
| 27–0 | — | Reserved, read as zero |

### Supervisor-Only Registers

Supervisor registers are **memory-mapped** into the CPU control region at `0x03FFF000` and accessed with normal `LW`/`SW` instructions. A user-mode access to these addresses raises a bus error (cause 0x02).

| Address | Register | Purpose |
|---|---|---|
| `0x03FFF000` | `EPC` | PC saved on entry — faulting instruction address for exceptions; `PC + 4` (return address) for SYSCALL. **Read/write**: write to adjust the return address before SYSRET. |
| `0x03FFF004` | `EFLAGS` | `flags` register saved on entry. Restored by SYSRET. **Read/write**: write to alter the flags SYSRET restores. |
| `0x03FFF008` | `EVEC` | Base address of the exception vector table. **Read/write**: must be written before enabling interrupts. |
| `0x03FFF00C` | `CAUSE` | Most recent exception cause code. **Read-only** — writes ignored. |
| `0x03FFF010` | `STATUS` | Bit 0 = privilege (0 = User, 1 = Supervisor); bit 1 = IE (1 = interrupts enabled); bits 31–2 reserved. **Read/write.** |
| `0x03FFF014` | `ESTATUS` | Exception-entry snapshot of `STATUS`. Saved automatically on every exception entry; restored by SYSRET (with bit 0 forced clear). **Read/write**: write before SYSRET to control the IE bit the returning task will see. |

---

## Instruction Set

Fixed **32-bit instruction width** throughout — no variable-length encoding, no compressed instructions.

### Instruction Formats

```
R-type (register ops):   [ opcode 8 | rd 4 | rs1 4 | rs2 4 | shift 5 | func 7 ]
I-type (immediate):      [ opcode 8 | rd 4 | rs1 4 | imm16             ]
B-type (branch):         [ opcode 8 | cond 4 | offset20                ]
M-type (memory):         [ opcode 8 | rd 4 | rs1 4 | imm16             ]
J-type (jump/call):      [ opcode 8 | rd 4 | offset20                  ]
```

Bit positions (bit 31 = MSB):

| Bits | R-type | I-type | B-type | M-type | J-type |
|---|---|---|---|---|---|
| 31–24 | opcode | opcode | opcode | opcode | opcode |
| 23–20 | rd | rd | cond | rd | rd |
| 19–16 | rs1 | rs1 | offset[19:16] | rs1 | offset[19:16] |
| 15–12 | rs2 | imm16[15:12] | offset[15:12] | imm16[15:12] | offset[15:12] |
| 11–7 | shift[4:0] | imm16[11:7] | offset[11:7] | imm16[11:7] | offset[11:7] |
| 6–0 | func | imm16[6:0] | offset[6:0] | imm16[6:0] | offset[6:0] |

**`func` field**: Reserved in the current ISA; must be zero. Available for future ISA extensions without an opcode change.

**`imm16`**: 16-bit immediate, sign-extended to 32 bits unless stated otherwise (MOVI, MOVHI, LUI, ANDI, ORI, XORI, CMPI use zero-extension).

**offset20 extraction**: The 20-bit signed byte offset in B-type and J-type spans bits 19–0. Sign-extend to 32 bits before adding to PC. Range: ±512 KB.

**Unused fields**: For instructions with no source register (MOVI, MOVHI), the rs1 field (bits 19–16) is ignored by the CPU and must be written as zero by the assembler. For instructions with no operands at all (SYSCALL, SYSRET, HALT), all bits below the opcode are ignored and must be zero.

---

### Complete Opcode Table

R-type register operand and I-type immediate operand variants are **separate opcodes**.

| Opcode | Mnemonic | Format | Operation |
|---|---|---|---|
| 0x00 | ADD | R | `rd = rs1 + rs2` |
| 0x01 | SUB | R | `rd = rs1 - rs2` |
| 0x02 | AND | R | `rd = rs1 & rs2` |
| 0x03 | OR  | R | `rd = rs1 \| rs2` |
| 0x04 | XOR | R | `rd = rs1 ^ rs2` |
| 0x05 | NOT | R | `rd = ~rs1` |
| 0x06 | LSL | R | `rd = rs1 << shift` |
| 0x07 | LSR | R | `rd = rs1 >> shift` (logical) |
| 0x08 | ASR | R | `rd = rs1 >> shift` (arithmetic) |
| 0x09 | MUL | R | `rd = (rs1 * rs2)[31:0]` (low 32 bits) |
| 0x0A | DIV | R | `rd = rs1 / rs2` (signed) |
| 0x0B | LW  | M | `rd = mem32[rs1 + imm16]` |
| 0x0C | SW  | M | `mem32[rs1 + imm16] = rd` |
| 0x0D | B*  | B | Branch (condition in `cond` field — see Branch Conditions) |
| 0x0E | JMP | J | `rd = PC + 4; PC = PC + offset20` — use `r0` as `rd` for an unconditional jump with no link |
| 0x0F | MOVI | I | `rd = imm16` (zero-extended) |
| 0x10 | SYSCALL | — | Enter supervisor mode (see Privilege section) |
| 0x11 | SYSRET  | — | Return to user mode (see Privilege section) |
| 0x12 | HALT    | — | Stop execution; no register or flag changes |
| 0x13 | MOVHI | I | `rd = (rd & 0xFFFF) \| (imm16 << 16)` |
| 0x14 | ADDI  | I | `rd = rs1 + imm16` (signed imm16) |
| 0x15 | SUBI  | I | `rd = rs1 - imm16` (signed imm16) |
| 0x16 | ANDI  | I | `rd = rs1 & imm16` (zero-extended imm16) |
| 0x17 | ORI   | I | `rd = rs1 \| imm16` (zero-extended imm16) |
| 0x18 | XORI  | I | `rd = rs1 ^ imm16` (zero-extended imm16) |
| 0x19 | LSLI  | I | `rd = rs1 << (imm16 & 0x1F)` |
| 0x1A | LSRI  | I | `rd = rs1 >> (imm16 & 0x1F)` (logical) |
| 0x1B | ASRI  | I | `rd = rs1 >> (imm16 & 0x1F)` (arithmetic) |
| 0x1C | LH    | M | `rd = sign_ext16(mem16[rs1 + imm16])` |
| 0x1D | LHU   | M | `rd = zero_ext16(mem16[rs1 + imm16])` |
| 0x1E | LB    | M | `rd = sign_ext8(mem8[rs1 + imm16])` |
| 0x1F | LBU   | M | `rd = zero_ext8(mem8[rs1 + imm16])` |
| 0x20 | SH    | M | `mem16[rs1 + imm16] = rd[15:0]` |
| 0x21 | SB    | M | `mem8[rs1 + imm16] = rd[7:0]` |
| 0x22 | MULH  | R | `rd = (rs1 * rs2)[63:32]` (signed high) |
| 0x23 | MULHU | R | `rd = (rs1 * rs2)[63:32]` (unsigned high) |
| 0x24 | DIVU  | R | `rd = rs1 / rs2` (unsigned) |
| 0x25 | MOD   | R | `rd = rs1 % rs2` (signed) |
| 0x26 | MODU  | R | `rd = rs1 % rs2` (unsigned) |
| 0x27 | MOV   | R | `rd = rs1` |
| 0x28 | CMP   | R | `flags = rs1 - rs2` (no rd written; rd field must be zero) |
| 0x29 | CMPI  | I | `flags = rs1 - imm16` (zero-extended imm16, range 0..65535; no rd written; rd field must be zero) |
| 0x2A | CALLR | R | `rd = PC + 4; PC = rs1` — register indirect call; use `r0` as `rd` to branch without saving |
| 0x2B | ADDC  | R | `rd = rs1 + rs2 + C` — add with carry |
| 0x2C | SUBC  | R | `rd = rs1 - rs2 - C` — subtract with borrow |
| 0x2D | LSLR  | R | `rd = rs1 << (rs2 & 0x1F)` — logical shift left by register amount |
| 0x2E | LSRR  | R | `rd = rs1 >> (rs2 & 0x1F)` (logical) — logical shift right by register amount |
| 0x2F | ASRR  | R | `rd = rs1 >> (rs2 & 0x1F)` (arithmetic) — arithmetic shift right by register amount |
| 0x30 | LWX   | R | `rd = mem32[rs1 + rs2]` — word load, base + register index |
| 0x31 | LBX   | R | `rd = sign_ext8(mem8[rs1 + rs2])` — signed byte load, indexed |
| 0x32 | LBUX  | R | `rd = zero_ext8(mem8[rs1 + rs2])` — unsigned byte load, indexed |
| 0x33 | SWX   | R | `mem32[rs1 + rs2] = rd` — word store, indexed |
| 0x34 | SBX   | R | `mem8[rs1 + rs2] = rd[7:0]` — byte store, indexed |
| 0x35 | LHX   | R | `rd = sign_ext16(mem16[rs1 + rs2])` — signed halfword load, indexed |
| 0x36 | LHUX  | R | `rd = zero_ext16(mem16[rs1 + rs2])` — unsigned halfword load, indexed |
| 0x37 | SHX   | R | `mem16[rs1 + rs2] = rd[15:0]` — halfword store, indexed |
| 0x38 | LUI   | I | `rd = imm16 << 16` (rd[15:0] zeroed; no read of rd — ordering-safe unlike MOVHI) |
| 0x39 | ROLR  | R | `rd = (rs1 << (rs2 & 0x1F)) \| (rs1 >> (32 - (rs2 & 0x1F)))` — rotate left by register amount |
| 0x3A | RORR  | R | `rd = (rs1 >> (rs2 & 0x1F)) \| (rs1 << (32 - (rs2 & 0x1F)))` — rotate right by register amount |
| 0x3B | ROLI  | R | `rd = (rs1 << sh) \| (rs1 >> (32 - sh))` — rotate left by immediate (shift field, 0..31) |
| 0x3C | RORI  | R | `rd = (rs1 >> sh) \| (rs1 << (32 - sh))` — rotate right by immediate (shift field, 0..31) |
| 0x3D | CAS | R | Atomic compare-and-swap: `tmp = mem32[rs1]`; if `tmp == rd` then `mem32[rs1] = rs2`, `Z=1`; else `rd = tmp`, `Z=0`. Raises misaligned (cause 1) if `rs1` is not word-aligned. N, C, V unchanged. |

Indexed memory instructions use the **R-type** encoding with `rs2` as the index register instead of an immediate offset. Alignment rules are identical to their non-indexed counterparts: LWX/SWX require 4-byte alignment, LHX/LHUX/SHX require 2-byte alignment, byte operations have no alignment requirement.

Assembler syntax: `LWX rd, [rs1+rs2]` and `SWX rd, [rs1+rs2]`. The `+rs2` part may be omitted to use `r0` as the index (`LWX rd, [rs1]`).

### Branch Condition Codes

All branches use opcode `0x0D`. The `cond` field (bits 23–20) selects the condition:

| cond | Mnemonic | Condition tested | Flags checked |
|---|---|---|---|
| 0 | BEQ  | Equal / zero | Z = 1 |
| 1 | BNE  | Not equal / non-zero | Z = 0 |
| 2 | BLT  | Less than (signed) | N ≠ V |
| 3 | BGE  | Greater than or equal (signed) | N = V |
| 4 | BLTU | Less than (unsigned) | C = 1 |
| 5 | BGEU | Greater than or equal (unsigned) | C = 0 |
| 6 | BA   | Always (unconditional branch) | — |
| 7 | BGT  | Greater than (signed) | Z = 0 and N = V |
| 8 | BLE  | Less than or equal (signed) | Z = 1 or N ≠ V |
| 9 | BGTU | Greater than (unsigned) | C = 0 and Z = 0 |
| 10 | BLEU | Less than or equal (unsigned) | C = 1 or Z = 1 |
| 11–15 | — | Reserved — illegal instruction exception | — |

**Branch target**: `PC = PC + sign_extend(offset20)`. Offset is in bytes, relative to the branch instruction's own PC.

**BLT/BGE/BGT/BLE** use the standard two's-complement signed conditions and give correct results even when the subtraction overflows. Use `CMP` or `SUB` before branching; use `CMP` when only the flags matter.

### Flag Updates

| Instruction group | N | Z | C | V |
|---|---|---|---|---|
| ADD, ADDI | ✓ | ✓ | carry out | overflow |
| ADDC | ✓ | ✓ | carry out | overflow |
| SUB, SUBI | ✓ | ✓ | borrow | overflow |
| SUBC | ✓ | ✓ | borrow out | overflow |
| CMP, CMPI | ✓ | ✓ | borrow | overflow |
| AND, OR, XOR, NOT, LSL, LSR, ASR, LSLR, LSRR, ASRR, ROLR, RORR, ROLI, RORI (+ immediate variants) | ✓ | ✓ | 0 | 0 |
| MUL, MULH, MULHU, DIV, DIVU, MOD, MODU | ✓ | ✓ | — | — |
| MOV, MOVI, MOVHI, LUI | — | — | — | — |
| LW, LH, LHU, LB, LBU, SW, SH, SB, LWX, LHX, LHUX, LBX, LBUX, SWX, SHX, SBX | — | — | — | — |

✓ = updated, 0 = cleared, — = unchanged.

> **Note:** `MOV`, `MOVI`, and `MOVHI` do **not** update any flags. This means a `MOV` immediately before a branch does not affect the branch outcome — use `CMP`/`CMPI` to set flags explicitly.

---

## Calling Convention (C ABI)

- `r0`–`r3`: arguments / return values
- `r4`–`r11`: callee-saved
- `r12`: scratch / caller-saved
- `sp` (`r13`): stack pointer, full-descending
- `lr` (`r14`): return address (set by `JMP`)
- `pc` (`r15`): program counter

> **`lr` is clobbered by every kernel trap.** On any exception entry (SYSCALL, hardware interrupt, or fault) the hardware uses `r14` as a scratch register before the first instruction of the handler executes, irrecoverably destroying the user's `lr` value. User code must treat `lr` as **volatile across any SYSCALL boundary** — a value stored in `lr` that is not also saved on the stack will be lost. Subroutine call-and-return code is unaffected because the return address is already saved on the stack (or consumed before the syscall), but storing data in `lr` across a syscall is not safe.

Stack is **full-descending** (sp points to last pushed item, grows downward). `sp` must be **4-byte aligned** at every call boundary; the callee may assume this on entry.

Standard push/pop sequences — available as pseudo-instructions `PUSH rd` and `POP rd`:

```
; PUSH rd  (assembler expands to:)
ADDI  sp, sp, -4
SW    rd, [sp]

; POP rd  (assembler expands to:)
LW    rd, [sp]
ADDI  sp, sp, 4
```

### Subroutine call and return

`JMP lr, target` is the call instruction: it stores `PC + 4` in `lr` and jumps to `target`.

Returning from a subroutine uses the `RET` pseudo-instruction (assembler alias for `MOV pc, lr`), which copies `lr` into `pc`. If a function calls further subroutines it must save and restore `lr` on the stack:

```
callee:
    ADDI  sp, sp, -4   ; save lr (only needed if making further calls)
    SW    lr, [sp]
    ...
    LW    lr, [sp]
    ADDI  sp, sp, 4
    RET                ; return — equivalent to MOV pc, lr
```

### r15 (pc) as a general-purpose destination

`pc` is a normal register that can be the destination of **any** instruction. Writing to `r15` redirects control flow: the next instruction fetched is from the value just written, not from `PC + 4`. This enables several useful patterns without dedicated opcodes:

| Pattern | Assembler alias | Effect |
|---|---|---|
| `MOV  pc, lr`  | `RET`    | Return from subroutine |
| `MOV  pc, rs1` | `BR rs1` | Indirect branch / computed goto |
| `LW   pc, [rs1 + imm]` | — | Jump via pointer in memory (switch tables) |
| `ADD  pc, pc, r0` | — | Computed relative jump |

The assembler provides `RET` and `BR rs1` as pseudo-instructions for the two most common cases.

`CALLR rd, rs1` is the register indirect call instruction: it stores `PC + 4` in `rd` (conventionally `lr`) and jumps to `rs1`. Use `CALLR r0, rs1` to branch indirectly without saving a return address.

### Multi-word (64-bit) arithmetic

`ADDC` and `SUBC` carry/borrow from the previous word's `C` flag into the next word's addition or subtraction, enabling arbitrary-precision arithmetic:

```
; 64-bit add: (r1:r0) + (r3:r2) → (r5:r4)
ADD   r4, r0, r2    ; low words — sets C on carry out
ADDC  r5, r1, r3    ; high words + carry in

; 64-bit subtract: (r1:r0) - (r3:r2) → (r5:r4)
SUB   r4, r0, r2    ; low words — sets C on borrow
SUBC  r5, r1, r3    ; high words - borrow in
```

### 32-bit Constant Loading

To load an arbitrary 32-bit constant into a register use a MOVI+MOVHI pair:

```
MOVI  rd, <low16>    ; rd = low 16 bits (zero-extended)
MOVHI rd, <high16>   ; rd[31:16] = high 16 bits, rd[15:0] unchanged
```

**Ordering constraint**: `MOVHI` is a read-modify-write — its operation is `rd = (rd & 0xFFFF) | (imm16 << 16)`, so it reads the current low 16 bits of `rd` and keeps them. `MOVI` must always execute **before** `MOVHI` when loading a 32-bit value, or the low 16 bits will be the old register contents (typically zero at reset, but unpredictable in general code).

The assembler's `MOVI32 rd, <imm32>` pseudo-instruction always expands to `MOVI` followed by `MOVHI` in the correct order and hides this dependency.

**`LUI` eliminates the ordering footgun.** `LUI rd, <high16>` unconditionally sets `rd = high16 << 16` with the low 16 bits zeroed — it does not read `rd` first. A full 32-bit load using `LUI` has no ordering constraint:

```
LUI  rd, <high16>   ; rd = high16 << 16  (rd[15:0] = 0 always)
ORI  rd, rd, <low16>; rd[15:0] = low16
```

Neither `MOVI`, `MOVHI`, nor `LUI` modifies the flags register, so a 32-bit constant load does not disturb the result of a preceding `CMP`.

---

## Privilege Levels

Two modes only — **User** and **Supervisor**.

### SYSCALL Entry Sequence (hardware)

1. Save `PC + 4` → `EPC` (return address past the SYSCALL instruction)
2. Save `flags` → `EFLAGS`
3. Set `CAUSE` = 0x03 (syscall)
4. Set `STATUS` = 0x01 (supervisor mode, IE cleared — interrupts disabled)
5. Jump to `mem32[EVEC + (3 * 4)]` (the syscall handler address)

**SYSCALL in supervisor mode** triggers a double fault. See Double Fault.

### SYSRET Sequence (hardware)

1. Set `PC` = `EPC`
2. Set `flags` = `EFLAGS`
3. Set `STATUS` = `ESTATUS & ~1` (restores IE from ESTATUS; bit 0 forced to 0 — always returns to user mode)

**SYSRET executed in user mode**: raises an illegal instruction exception (cause 0x00).

---

## Interrupts & Exceptions

### Exception Vector Table

`EVEC` holds the base address of a table of 32-bit handler addresses. Each entry is one word (4 bytes). On an exception with cause code *n*, the CPU loads the handler address from `EVEC + (n * 4)` and jumps to it. Software is responsible for populating this table before enabling interrupts.

### Exception Entry Sequence (hardware, all causes except SYSCALL)

1. **Double-fault check**: if already in supervisor mode (STATUS bit 0 = 1), halt immediately — see Double Fault below.
2. Save `STATUS` → `ESTATUS`
3. Save faulting `PC` → `EPC`
4. Save `flags` → `EFLAGS`
5. Write cause code → `CAUSE`
6. Set `STATUS` = `0x01` (supervisor mode, IE cleared — interrupts disabled)
7. Jump to `mem32[EVEC + (CAUSE * 4)]`

> **Hardware interrupt exception (cause 6)**: EPC is set to the address of the *next* instruction that would have executed — i.e. `PC` as it stood after the last instruction completed and the processor was about to fetch again. This differs from true fault causes (0–4) where EPC is the address of the faulting instruction itself. An OS interrupt handler can restore EPC directly into the supervisor register to resume the interrupted task without adjustment.

### Double Fault

If any exception or interrupt is triggered while the CPU is already in supervisor mode (STATUS bit 0 = 1), the CPU **halts immediately** without dispatching to a handler and without modifying any registers. `EPC`, `EFLAGS`, and `CAUSE` retain the values from the first exception. The emulator treats this identically to a HALT instruction.

### Interrupt Dispatch

After each instruction completes, the CPU checks for pending hardware interrupts:

1. If `STATUS.IE` (bit 1) = 0, skip — interrupts masked globally.
2. Read the interrupt controller's pending register (`0x03F02000`). If no unmasked interrupt is pending, skip.
3. Dispatch an exception with **cause 0x06** — following the standard exception entry sequence above.

The interrupt controller's enable mask (`0x03F02004`) is checked as part of step 2; a pending interrupt that is masked in the controller does not fire even if `STATUS.IE` = 1.

### Exception Cause Codes

| Code | Name | Trigger |
|---|---|---|
| 0x00 | Illegal instruction | Unknown opcode, reserved encoding, or privileged instruction in user mode |
| 0x01 | Misaligned access | LW/SW to non-word-aligned address; LH/SH to non-halfword-aligned address |
| 0x02 | Bus error | Address outside valid memory range or fetch past end of memory |
| 0x03 | Syscall | `SYSCALL` instruction executed in user mode |
| 0x04 | Divide by zero | `DIV`, `DIVU`, `MOD`, or `MODU` with zero divisor |
| 0x05 | Halt | `HALT` instruction — not a true exception; stops the emulator cleanly |
| 0x06 | Hardware interrupt | Any unmasked hardware interrupt fired — read IC pending register (`0x03F02000`) to identify source: bit 0 = timer, bit 1 = UART RX, bit 2 = UART TX, bit 3 = block device |

Cause codes 0x07 and above are reserved for future use.

### No Shadow Registers

There are no shadow or banked registers. An OS context-switch must save and restore all registers in software.

---

## Memory-Mapped I/O

Devices occupy the top 1 MB of the address space (`0x03F00000`–`0x03FFFFFF`). All registers are word-aligned 32-bit words. Byte and halfword accesses are undefined behaviour. Reads from unmapped addresses within the window return 0; writes are silently ignored.

| Address | Device |
|---|---|
| `0x03F00000` | **UART** — serial console |
| `0x03F01000` | **Timer** — periodic countdown timer |
| `0x03F02000` | **Interrupt controller** — pending / mask / acknowledge |
| `0x03F03000` | **Block device** — 512-byte sector read/write |
| `0x03FFF000` | **CPU control** — supervisor registers; user-mode access raises bus error (cause 0x02) |

### UART (`0x03F00000`)

| Offset | Register | Notes |
|---|---|---|
| `+0x00` | Status | Bit 0 = TX ready, bit 1 = RX available |
| `+0x04` | TX | Write byte to send — low 8 bits used, upper bits ignored |
| `+0x08` | RX | Read received byte — returns 0 if no data available |
| `+0x0C` | Control | Bit 0 = RX interrupt enable, bit 1 = TX interrupt enable |

TX interrupt is edge-triggered: fires once when a transmission completes (busy → ready). TX pending bit is 0 at reset. RX interrupt fires on byte receipt.

### Timer (`0x03F01000`)

| Offset | Register | Notes |
|---|---|---|
| `+0x00` | Period | Write to set period and start counting; reads remaining count. Writing 0 is undefined behaviour. |
| `+0x04` | Control | Bit 0 = enable, bit 1 = interrupt enable |

One **tick** = one instruction executed. The timer decrements by one per tick. On reaching zero it reloads the period and restarts — **periodic**, not one-shot. Clear control bit 0 to stop it.

### Interrupt Controller (`0x03F02000`)

| Offset | Register | Notes |
|---|---|---|
| `+0x00` | Pending | Read-only |
| `+0x04` | Enable mask | 1 = source enabled |
| `+0x08` | Acknowledge | Write bit(s) to clear pending |

Pending, enable mask, and acknowledge share the same bit layout:

| Bit | Source | Trigger |
|---|---|---|
| 0 | Timer | Period countdown reached zero |
| 1 | UART RX | Byte arrived (edge-triggered on receipt) |
| 2 | UART TX | Transmission completed (busy → ready) |
| 3 | Block device | Command completed (idle or error) |
| 31–4 | Reserved | — |

Pending bits are **latched** — set by a device event, cleared only by writing the corresponding bit to acknowledge.

### Block Device (`0x03F03000`)

| Offset | Register | Notes |
|---|---|---|
| `+0x00` | Sector | Sector number to read/write |
| `+0x04` | Buffer | Guest memory address for data — must be 512-byte aligned |
| `+0x08` | Command | Write 1 = read, 2 = write. Writing while status = busy is undefined behaviour. |
| `+0x0C` | Status | 0 = idle, 1 = busy, 2 = error |
| `+0x10` | Control | Bit 0 = interrupt enable — fires on command completion (idle or error) |

### Interrupt Enable

For an interrupt to reach the CPU, all three of the following must be true:

- **Device enable**: the device's control register interrupt enable bit is set. If clear, the device never asserts to the IC and the pending bit is never set.
- **IC mask**: the IC enable mask bit for that source is set. If clear, the pending bit is still set by the device but the interrupt never dispatches.
- **Global**: `STATUS.IE` = 1.

Typical init sequence: set device enable → set IC mask → set `STATUS.IE` last.

---

## Key Design Decisions

| Decision | Rationale |
|---|---|
| Fixed 32-bit instructions | Emulator decode loop is trivial |
| 16 registers | Standard C ABI fits cleanly, register file is small |
| No delay slots | Simpler emulator, simpler compiler backend |
| No FPU | Not needed for Linux kernel; add later via syscall emulation |
| Memory-mapped I/O | No special IN/OUT instructions to implement |
| Two privilege modes | Exactly what Linux needs, no more |
| Vectored exceptions | Simple table lookup vs. complex trap handling |
| Little-endian | Matches x86/ARM host byte order; simplifies C struct overlays and interop with host tools |
| Separate R/I opcodes | Avoids a mode-select bit inside the encoding; each opcode has exactly one format |
| EFLAGS supervisor register | SYSRET can restore user flags atomically without extra instructions |
| Memory-mapped supervisor registers | No CSR instructions needed; normal LW/SW in supervisor mode are sufficient |
| Double fault → halt | Simplest safe failure mode; avoids recursive exception machinery |
