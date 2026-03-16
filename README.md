# CPUTwo

A 32-bit RISC-inspired CPU: assembler, emulator, and ROM monitor. See `architecture.md` for the full ISA specification.

This was mostly written by Claude Code, both using Sonnet 4.6, and also local Qwen3-coder-next

---

## Building

```
cmake -B build && cmake --build build
```

Or directly with a C99 compiler:

```
cc -std=c99 -Wall -o assemblertwo assembler.c
cc -std=c99 -Wall -o emulatortwo   emulator.c
```

Binaries produced: `assemblertwo`, `emulatortwo`.

---

## Assembler

```
./assemblertwo input.asm output.bin [output.map]
```

Two-pass assembler. Emits a flat binary. The optional map file lists every symbol and its address.

### Instruction syntax

```asm
label:  MNEMONIC  operands    ; comment
```

Labels are optional. Mnemonics are case-insensitive. All register names (`r0`–`r12`, `sp`, `lr`, `pc`) are also case-insensitive.

**Register operands** — bare names:
```asm
ADD  r0, r1, r2
MOV  r3, r4
```

**Immediate operands** — decimal or hex (`0x` prefix):
```asm
MOVI  r0, 255
MOVI  r0, 0xFF
```

**Memory operands** — base register plus optional signed offset:
```asm
LW   r0, [sp]
LW   r0, [sp+8]
SW   r1, [r2-4]
```

**Indexed memory** (R-type, two register operands):
```asm
LWX  r0, [r1+r2]
SBX  r3, [r4+r5]
```

**Branch / jump targets** — label or numeric address:
```asm
BEQ  some_label
JMP  lr, 0x00001000
```

### Directives

| Directive | Effect |
|---|---|
| `.org <addr>` | Set output address |
| `.equ NAME, <val>` | Define a numeric constant (no bytes emitted) |
| `.global NAME` | Mark symbol as global in the map file |
| `.word <val>` | Emit one 32-bit little-endian word |
| `.byte <val>` | Emit one byte |
| `.space <n>` | Emit *n* zero bytes |
| `.ascii "…"` | Emit string bytes (no null terminator) |
| `.asciiz "…"` | Emit string bytes followed by a null byte |
| `.align <n>` | Pad to the next *n*-byte boundary |
| `.include "path"` | Insert another source file inline at this point |

String escapes supported: `\n`, `\r`, `\t`, `\0`, `\\`, `\"`.

`.include` paths are resolved relative to the directory of the file that contains the directive. Includes may be nested.

### Pseudo-instructions

| Pseudo | Expands to |
|---|---|
| `NOP` | `MOVI r0, 0` |
| `MOVI32 rd, imm32` | `MOVI rd, <low16>` then `MOVHI rd, <high16>` |
| `PUSH rd` | `ADDI sp, sp, -4` then `SW rd, [sp]` |
| `POP rd` | `LW rd, [sp]` then `ADDI sp, sp, 4` |
| `RET` | `MOV pc, lr` |
| `BR rs1` | `MOV pc, rs1` |
| `CALL target` | `JMP lr, target` |

### Expressions

Numeric arguments accept expressions using `+`, `-`, and `>>` with label names and `.equ` constants:

```asm
.equ UART_TX, 0x03F00004
MOVI32 r1, UART_TX
SW     r0, [r1]
```

### Example

```asm
        MOVI32  r1, msg
        MOVI32  r2, 0x03F00004      ; UART TX

loop:   LBU     r0, [r1]
        CMPI    r0, 0
        BEQ     done
        SW      r0, [r2]
        ADDI    r1, r1, 1
        JMP     r12, loop

done:   HALT

msg:    .asciiz "Hello, World!\n"
```

More examples: `tests/helloworld.asm`, `tests/primes.asm`, `tests/selection_sort.asm`.

---

## Emulator

```
./emulatortwo [options] <binary>
```

Loads the binary at address 0 (or at `load_addr` if `-a` is given) and begins execution at that address.

| Option | Description |
|---|---|
| `-a <addr>` | Load binary at *addr* instead of 0 |
| `-blk <file>` | Attach a disk image for the block device |
| `-d` | Interactive debugger (step / cont / regs / break / mem) |
| `-e` | Emit execution trace to stderr (one disassembled instruction per line) |

### Memory map

| Range | Contents |
|---|---|
| `0x00000000`–`0x03EFFFFF` | RAM (64 MB minus MMIO) |
| `0x03F00000`–`0x03F000FF` | UART |
| `0x03F01000`–`0x03F010FF` | Timer |
| `0x03F02000`–`0x03F020FF` | Interrupt controller |
| `0x03F03000`–`0x03F030FF` | Block device |
| `0x03FFF000`–`0x03FFF010` | CPU control (supervisor only) |

Accesses at or above `0x04000000` raise a bus error (cause 0x02).

### Built-in debugger (`-d`)

When launched with `-d`, execution pauses before every instruction:

| Command | Action |
|---|---|
| `s` / `step` | Execute one instruction and pause |
| `c` / `cont` | Resume free execution |
| `regs` | Print all registers and flags |
| `break <addr>` | Set a breakpoint (up to 16) |
| `mem <addr> [len]` | Hex-dump memory (*addr* and *len* in hex; default len = 64) |

The `-e` trace mode is non-interactive — it prints the program counter and disassembly of every instruction to stderr and lets the program run to completion.

### UART

The emulator maps the UART to stdin/stdout. TX writes go to stdout immediately. RX bytes are read from stdin with a non-blocking poll each instruction cycle, so programs that spin on the UART status register work correctly.

### Block device

Supply a flat binary file with `-blk disk.img`. The device reads and writes 512-byte sectors. The buffer address written to the buffer register must be 512-byte aligned; an unaligned address sets the device status to error (2) without performing I/O.

---

## ROM Monitor

`monitor.asm` is a bare-metal ROM monitor that runs on the emulator and provides an interactive source-level debugger for user programs loaded above address `0x10000`.

### Building the monitor

```
./assemblertwo monitor.asm monitor.bin
```

### Running with a user program

The monitor occupies `0x00000000`–`0x00007FFF`. User code lives at `0x00010000` or above. To combine them into one binary:

```bash
# Assemble user program at its target address
./assemblertwo myprogram.asm myprogram.bin   # .org 0x00010000 inside

# Overlay monitor at the start
cp myprogram.bin combined.bin
dd if=monitor.bin of=combined.bin bs=1 conv=notrunc

./emulatortwo combined.bin
```

### Monitor commands

All numeric arguments are **hexadecimal**.

| Command | Description |
|---|---|
| `s` / `step` | Execute one instruction of user code and return to the monitor |
| `c` / `cont` | Resume user code; return on next breakpoint |
| `regs` | Dump all 16 registers and flags from the saved user context |
| `break <addr>` | Install a breakpoint at *addr* (up to 16 active; persists across step/cont) |
| `mem <addr> [<len>]` | Hex + ASCII dump starting at *addr* (default length = 0x40 bytes) |
| `pc <addr>` | Set the user program counter before first `cont` |

On startup the user context is initialised to: PC = `0x00010000`, SP = `0x0000F000`, all other registers zero.

### How breakpoints work

Breakpoints are implemented by patching a `SYSCALL` instruction over the target address. The original instruction is saved and restored when the breakpoint fires. On `cont`, all breakpoints are re-installed. On `step`, the breakpoint at the current PC is left unpatched for that one instruction, then re-installed after the step completes.

### Caveats

- **`lr` (r14) is clobbered on trap entry.** The exception handler uses r14 as a scratch pointer to save all other registers. User code that calls subroutines via `JMP lr, target` is unaffected (lr is set *in* user code by those calls); only code that stores a value in lr and reads it back across a trap will see the sentinel value `0xDEADBEEF`.
- Breakpoints are not automatically removed; re-running `cont` after all breakpoints have been hit will re-install them.
- `step` decodes branches and jumps to compute the correct next PC. For indirect branches through memory (`LW pc, [rs1+imm]`) the next PC is not computed and the step will land at PC+4 instead of the branch target.

---

## C Toolchain

CPUTwo has a full C toolchain built from two companion repositories:

| Repository | Purpose |
|---|---|
| [`../tinycc_CPUTwo`](https://github.com/jhonsrid/tinycc_CPUTwo) | TCC cross-compiler targeting CPUTwo (`cputwo-tcc`) |
| [`../pdclib_CPUTwo`](https://github.com/jhonsrid/pdclib_CPUTwo) | pdclib C standard library ported to CPUTwo bare-metal |

### Building the compiler

```sh
cd ../tinycc_CPUTwo
make
```

Produces `cputwo-tcc`, a cross-compiler that emits 32-bit ELF executables for CPUTwo (machine type `0x9002`).

### Building the C standard library

```sh
cd ../pdclib_CPUTwo/platform/cputwo
make CC=../../../tinycc_CPUTwo/cputwo-tcc WITH_DLMALLOC=1
```

Produces `libpdclib.a` (~400 KB), which includes:

- Full C99 stdio (`printf`, `scanf`, `sprintf`, …) — backed by the UART at `0x03F00000`
- stdlib (`malloc`, `free`, `realloc`, `calloc`) — backed by dlmalloc with a 512 KB static heap
- string, math, time stubs, signal stubs

### Compiling a program

Use the `cputwo-cc` wrapper script in this repository:

```sh
./cputwo-cc hello.c -o hello.elf
```

It expands to:

```sh
cputwo-tcc -nostdlib -static \
    -B../tinycc_CPUTwo \
    -I../pdclib_CPUTwo/include \
    -I../pdclib_CPUTwo/platform/cputwo/include \
    hello.c \
    ../pdclib_CPUTwo/platform/cputwo/libpdclib.a \
    -o hello.elf
```

Any extra `cputwo-tcc` flags (e.g. `-g`, `-O2`, `-D`, `-I`) can be passed before the source files.

### Running a compiled program

Load the ELF into the emulator with the `-elf` flag (or strip it to a flat binary first):

```sh
./emulatortwo hello.elf
```

### UART I/O

`printf` / `puts` / `scanf` etc. use the UART directly:

| Register | Address | Description |
|---|---|---|
| Status | `0x03F00000` | bit 0 = TX ready, bit 1 = RX available |
| TX data | `0x03F00004` | write one byte |
| RX data | `0x03F00008` | read one byte |

The library polls the status register before each byte — no interrupts required.

---

## Tests

```
cmake --build build && ctest --test-dir build
```

Unit tests live in `test_assembler.c` and `test_emulator.c`.
