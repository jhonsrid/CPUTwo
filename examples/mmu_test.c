/*
 * mmu_test.c — Bare-metal MMU test for CPUTwo.
 *
 * No libc / pdclib — all I/O goes directly to the UART MMIO registers.
 * Compile with cputwo-tcc (the cross-compiler) and a custom linker script
 * that places the binary at PA 0x00000000, or assemble from mmu_test.asm
 * which is the assembled version of this test.
 *
 * Memory layout assumed:
 *   0x00000000  Program code + data (this file, < 4 KB)
 *   0x00001000  L1 page table  (PPN = 1)
 *   0x00002000  L2 page table  (PPN = 2)
 *   0x00003000  Scratch data page (PPN = 3)
 *
 * Test sequence:
 *   1. Build two-level identity-map page tables covering PA 0x0–0x7FFF.
 *   2. Enable MMU: write SATP = 0x80000001 (EN=1, PPN=1).
 *   3. Load via VA 0x00003000 — verify data matches what was written
 *      to PA 0x00003000 before MMU enable.
 *   4. Store via VA 0x00003004 — verify the hardware sets D bit in L2[3].
 *   5. Read UART status via physical 0x03F00000 — MMIO bypass check.
 *   6. Execute SFENCE (opcode 0x3E).
 *   7. Disable MMU (SATP = 0).
 *   8. Verify physical access still works.
 *   9. Print "MMU OK\n" on success or "FAIL\n" on any check failure.
 *
 * PTE bit layout:
 *   bit  0  V  — valid
 *   bit  1  D  — dirty (hardware sets on store)
 *   bit  5  R  — readable
 *   bit  6  W  — writable
 *   bit  7  X  — executable
 *   bit  9  G  — global (not flushed by SFENCE / SATP write)
 *   31:12   PPN
 */

#include <stdint.h>

/* ── MMIO addresses ───────────────────────────────────────────────────────── */
#define UART_STATUS  (*(volatile uint32_t *)0x03F00000u)
#define UART_TX      (*(volatile uint32_t *)0x03F00004u)
#define SATP         (*(volatile uint32_t *)0x03FFF018u)
#define BADADDR      (*(volatile uint32_t *)0x03FFF01Cu)

/* ── Page table layout ────────────────────────────────────────────────────── */
#define L1_BASE    0x00001000u   /* physical address of L1 table */
#define L2_BASE    0x00002000u   /* physical address of L2 table */
#define DATA_PAGE  0x00003000u   /* scratch data page */

static volatile uint32_t *const l1 = (volatile uint32_t *)L1_BASE;
static volatile uint32_t *const l2 = (volatile uint32_t *)L2_BASE;

/* ── PTE flag constants ───────────────────────────────────────────────────── */
#define PTE_V   (1u << 0)
#define PTE_D   (1u << 1)
#define PTE_R   (1u << 5)
#define PTE_W   (1u << 6)
#define PTE_X   (1u << 7)
#define PTE_G   (1u << 9)

/* Leaf PTE: supervisor-only, global, R|W|X|V */
#define PTE_LEAF_FLAGS  (PTE_G | PTE_X | PTE_W | PTE_R | PTE_V)
/* Non-leaf PTE: V only (no R/W/X means pointer to next level) */
#define PTE_NONLEAF     (PTE_V)

/* Build a leaf PTE from a physical page number */
#define LEAF_PTE(ppn)    (((uint32_t)(ppn) << 12) | PTE_LEAF_FLAGS)
/* Build a non-leaf PTE from a physical page number */
#define NONLEAF_PTE(ppn) (((uint32_t)(ppn) << 12) | PTE_NONLEAF)

/* ── Minimal UART output ──────────────────────────────────────────────────── */
static void uart_putc(char c) {
    UART_TX = (uint32_t)(unsigned char)c;
}

static void uart_puts(const char *s) {
    while (*s)
        uart_putc(*s++);
}

/* ── SFENCE — inline assembly (raw opcode word) ─────────────────────────── */
/* cputwo-tcc doesn't have an assembler directive for SFENCE yet, so emit
   the raw instruction word via a .word directive in an asm block.
   If your toolchain does not support inline asm, use mmu_test.asm instead. */
static inline void sfence(void) {
    /* opcode 0x3E in bits[31:24]; all other bits zero */
    /* Raw encoding: 0x3E000000 */
    /* Use a volatile memory barrier to prevent reordering around the fence. */
    __asm__ volatile(".word 0x3E000000" ::: "memory");
}

/* ── Main test ────────────────────────────────────────────────────────────── */
void _start(void) {
    int ok = 1;

    /* --- Step 1: Build page tables (physical memory, MMU off) --- */

    /* L1[0]: non-leaf pointing at L2 (PPN=2 => 0x00002000) */
    l1[0] = NONLEAF_PTE(2);

    /* L2[0..7]: identity map pages 0-7 */
    for (int i = 0; i < 8; i++)
        l2[i] = LEAF_PTE(i);

    /* Write a sentinel value to the scratch data page */
    volatile uint32_t *data = (volatile uint32_t *)DATA_PAGE;
    data[0] = 0xDEADBEEFu;

    /* --- Step 2: Enable MMU --- */
    /* SATP = EN=1 | PPN=1 (L1 at 0x00001000) */
    SATP = 0x80000001u;

    /* --- Test 1: Load via VA 0x00003000 --- */
    if (data[0] != 0xDEADBEEFu)
        ok = 0;

    /* --- Test 2: Store via VA 0x00003004; check D bit --- */
    data[1] = 0x12345678u;
    if (data[1] != 0x12345678u)
        ok = 0;

    /* L2[3] PTE is at offset 3*4=12 from L2_BASE = 0x0000200C */
    uint32_t pte3 = *(volatile uint32_t *)0x0000200Cu;
    if (!(pte3 & PTE_D))
        ok = 0; /* D bit not set by hardware */

    /* --- Test 3: MMIO bypass — UART status readable with MMU on --- */
    if (!(UART_STATUS & 1u))
        ok = 0; /* TX-ready bit should always be 1 */

    /* --- Test 4: SFENCE --- */
    sfence();

    /* --- Test 5: Disable MMU --- */
    SATP = 0u;

    /* --- Test 6: Physical access still works after MMU off --- */
    if (data[0] != 0xDEADBEEFu)
        ok = 0;

    /* --- Report result --- */
    if (ok)
        uart_puts("MMU OK\n");
    else
        uart_puts("FAIL\n");

    /* Emit HALT (opcode 0x12 in bits[31:24]) */
    __asm__ volatile(".word 0x12000000");
    __builtin_unreachable();
}
