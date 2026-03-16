/* CPUTwo assembler unit tests — C99
 * Compile: cc -std=c99 -Wall -o test_assembler test_assembler.c
 * Run:     ./test_assembler
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Pull in assembler internals, stubbing out main */
#define main assembler_main_stub
#include "assembler.c"
#undef main

/* ── Test harness ──────────────────────────────────────────────────────────── */
static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { tests_failed++; fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); } \
} while(0)

/* Assemble a string and return the number of errors. */
static int do_asm(const char *src, uint8_t *buf, size_t bufsz, size_t *len) {
    return assemble_string(src, buf, bufsz, len);
}

/* Read little-endian 32-bit word at buf[idx*4] */
static uint32_t read_word(uint8_t *buf, int idx) {
    int b = idx * 4;
    return  (uint32_t)buf[b]
         | ((uint32_t)buf[b+1] <<  8)
         | ((uint32_t)buf[b+2] << 16)
         | ((uint32_t)buf[b+3] << 24);
}

#define BUFSZ (64*1024)
static uint8_t buf[BUFSZ];
static size_t  blen;

/* ─────────────────────────────────────────────────────────────────────────── */
/* Basic R-type instructions                                                   */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_r_type(void) {
    /* ADD r0, r1, r2  → (0x00<<24)|(0<<20)|(1<<16)|(2<<12) = 0x00012000 */
    ASSERT(do_asm("ADD r0, r1, r2", buf, BUFSZ, &blen) == 0, "ADD: no errors");
    ASSERT(blen == 4,                                     "ADD: 4 bytes");
    ASSERT(read_word(buf,0) == 0x00012000,                "ADD r0,r1,r2 encoding");

    /* SUB r3, r4, r5  → (0x01<<24)|(3<<20)|(4<<16)|(5<<12) = 0x01345000 */
    ASSERT(do_asm("SUB r3, r4, r5", buf, BUFSZ, &blen) == 0, "SUB: no errors");
    ASSERT(read_word(buf,0) == 0x01345000,                 "SUB r3,r4,r5 encoding");

    /* AND r0, r1, r2 → 0x02012000 */
    ASSERT(do_asm("AND r0, r1, r2", buf, BUFSZ, &blen) == 0, "AND: no errors");
    ASSERT(read_word(buf,0) == 0x02012000,                 "AND encoding");

    /* OR r0, r1, r2 → 0x03012000 */
    ASSERT(do_asm("OR r0, r1, r2", buf, BUFSZ, &blen) == 0, "OR: no errors");
    ASSERT(read_word(buf,0) == 0x03012000,                "OR encoding");

    /* XOR r0, r1, r2 → 0x04012000 */
    ASSERT(do_asm("XOR r0, r1, r2", buf, BUFSZ, &blen) == 0, "XOR: no errors");
    ASSERT(read_word(buf,0) == 0x04012000,                 "XOR encoding");

    /* MUL r0, r1, r2 → 0x09012000 */
    ASSERT(do_asm("MUL r0, r1, r2", buf, BUFSZ, &blen) == 0, "MUL: no errors");
    ASSERT(read_word(buf,0) == 0x09012000,                 "MUL encoding");

    /* DIV r0, r1, r2 → 0x0A012000 */
    ASSERT(do_asm("DIV r0, r1, r2", buf, BUFSZ, &blen) == 0, "DIV: no errors");
    ASSERT(read_word(buf,0) == 0x0A012000,                 "DIV encoding");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* R2-type: NOT, MOV                                                           */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_r2_type(void) {
    /* NOT r0, r1 → (0x05<<24)|(0<<20)|(1<<16) = 0x05010000 */
    ASSERT(do_asm("NOT r0, r1", buf, BUFSZ, &blen) == 0, "NOT: no errors");
    ASSERT(read_word(buf,0) == 0x05010000,             "NOT r0,r1 encoding");

    /* MOV r1, r2 → (0x27<<24)|(1<<20)|(2<<16) = 0x27120000 */
    ASSERT(do_asm("MOV r1, r2", buf, BUFSZ, &blen) == 0, "MOV: no errors");
    ASSERT(read_word(buf,0) == 0x27120000,             "MOV r1,r2 encoding");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Shift instructions (RSHIFT format)                                         */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_shift(void) {
    /* LSL r0, r1, 4 → (0x06<<24)|(0<<20)|(1<<16)|(4<<7) = 0x06010200 */
    ASSERT(do_asm("LSL r0, r1, 4", buf, BUFSZ, &blen) == 0, "LSL: no errors");
    ASSERT(read_word(buf,0) == 0x06010200,               "LSL r0,r1,4 encoding");

    /* LSR r2, r3, 8 → (0x07<<24)|(2<<20)|(3<<16)|(8<<7) = 0x07230400 */
    ASSERT(do_asm("LSR r2, r3, 8", buf, BUFSZ, &blen) == 0, "LSR: no errors");
    ASSERT(read_word(buf,0) == 0x07230400,               "LSR r2,r3,8 encoding");

    /* ASR r0, r1, 1 → (0x08<<24)|(0<<20)|(1<<16)|(1<<7) = 0x08010080 */
    ASSERT(do_asm("ASR r0, r1, 1", buf, BUFSZ, &blen) == 0, "ASR: no errors");
    ASSERT(read_word(buf,0) == 0x08010080,               "ASR r0,r1,1 encoding");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* MOVI and MOVHI                                                              */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_movi(void) {
    /* MOVI r0, 0x1234 → (0x0F<<24)|(0<<20)|0x1234 = 0x0F001234 */
    ASSERT(do_asm("MOVI r0, 0x1234", buf, BUFSZ, &blen) == 0, "MOVI: no errors");
    ASSERT(read_word(buf,0) == 0x0F001234,                  "MOVI r0,0x1234 encoding");

    /* MOVHI r0, 0xDEAD → (0x13<<24)|(0<<20)|0xDEAD = 0x1300DEAD */
    ASSERT(do_asm("MOVHI r0, 0xDEAD", buf, BUFSZ, &blen) == 0, "MOVHI: no errors");
    ASSERT(read_word(buf,0) == 0x1300DEAD,                   "MOVHI r0,0xDEAD encoding");

    /* MOVI r0, 0 */
    ASSERT(do_asm("MOVI r0, 0", buf, BUFSZ, &blen) == 0, "MOVI 0: no errors");
    ASSERT(read_word(buf,0) == 0x0F000000,             "MOVI r0,0 encoding");

    /* MOVI r0, 65535 */
    ASSERT(do_asm("MOVI r0, 65535", buf, BUFSZ, &blen) == 0, "MOVI 65535: no errors");
    ASSERT(read_word(buf,0) == 0x0F00FFFF,                "MOVI r0,65535 encoding");

    /* MOVI r0, 65536 — should produce error */
    ASSERT(do_asm("MOVI r0, 65536", buf, BUFSZ, &blen) > 0, "MOVI 65536 should error");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* MOVI32 pseudo-instruction                                                   */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_movi32(void) {
    /* MOVI32 r0, 0x12345678 → MOVI r0, 0x5678; MOVHI r0, 0x1234 */
    ASSERT(do_asm("MOVI32 r0, 0x12345678", buf, BUFSZ, &blen) == 0, "MOVI32: no errors");
    ASSERT(blen == 8,                                              "MOVI32: 8 bytes");
    /* MOVI r0, 0x5678 → (0x0F<<24)|(0<<20)|0x5678 = 0x0F005678 */
    ASSERT(read_word(buf,0) == 0x0F005678, "MOVI32: low word (MOVI)");
    /* MOVHI r0, 0x1234 → (0x13<<24)|(0<<20)|0x1234 = 0x13001234 */
    ASSERT(read_word(buf,1) == 0x13001234, "MOVI32: high word (MOVHI)");

    /* MOVI32 r1, 0x0000FFFF → MOVI r1, 0xFFFF; MOVHI r1, 0x0000 */
    ASSERT(do_asm("MOVI32 r1, 0x0000FFFF", buf, BUFSZ, &blen) == 0, "MOVI32 0xFFFF: no errors");
    ASSERT(read_word(buf,0) == 0x0F10FFFF, "MOVI32 r1,0xFFFF: MOVI part");
    ASSERT(read_word(buf,1) == 0x13100000, "MOVI32 r1,0xFFFF: MOVHI part");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* I-type: ADDI, SUBI (signed), ANDI, ORI, XORI, LSLI/LSRI/ASRI             */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_i_type(void) {
    /* ADDI r1, r2, -1 → (0x14<<24)|(1<<20)|(2<<16)|0xFFFF = 0x1412FFFF */
    ASSERT(do_asm("ADDI r1, r2, -1", buf, BUFSZ, &blen) == 0, "ADDI -1: no errors");
    ASSERT(read_word(buf,0) == 0x1412FFFF,                  "ADDI r1,r2,-1 encoding");

    /* ADDI r0, r0, 1 → (0x14<<24)|(0<<20)|(0<<16)|1 = 0x14000001 */
    ASSERT(do_asm("ADDI r0, r0, 1", buf, BUFSZ, &blen) == 0, "ADDI 1: no errors");
    ASSERT(read_word(buf,0) == 0x14000001,                 "ADDI r0,r0,1 encoding");

    /* SUBI r3, r4, 5 → (0x15<<24)|(3<<20)|(4<<16)|5 = 0x15340005 */
    ASSERT(do_asm("SUBI r3, r4, 5", buf, BUFSZ, &blen) == 0, "SUBI 5: no errors");
    ASSERT(read_word(buf,0) == 0x15340005,                 "SUBI r3,r4,5 encoding");

    /* ADDI out-of-range: 32768 → error */
    ASSERT(do_asm("ADDI r0, r0, 32768", buf, BUFSZ, &blen) > 0, "ADDI 32768 should error");

    /* ANDI r0, r1, 0xFF → (0x16<<24)|(0<<20)|(1<<16)|0xFF = 0x160100FF */
    ASSERT(do_asm("ANDI r0, r1, 0xFF", buf, BUFSZ, &blen) == 0, "ANDI: no errors");
    ASSERT(read_word(buf,0) == 0x160100FF,                    "ANDI r0,r1,0xFF encoding");

    /* ORI r0, r1, 0x1234 → (0x17<<24)|(0<<20)|(1<<16)|0x1234 = 0x17011234 */
    ASSERT(do_asm("ORI r0, r1, 0x1234", buf, BUFSZ, &blen) == 0, "ORI: no errors");
    ASSERT(read_word(buf,0) == 0x17011234,                     "ORI encoding");

    /* XORI r0, r1, 0xFFFF → (0x18<<24)|(0<<20)|(1<<16)|0xFFFF = 0x1801FFFF */
    ASSERT(do_asm("XORI r0, r1, 0xFFFF", buf, BUFSZ, &blen) == 0, "XORI: no errors");
    ASSERT(read_word(buf,0) == 0x1801FFFF,                      "XORI encoding");

    /* LSLI r0, r1, 3 → (0x19<<24)|(0<<20)|(1<<16)|3 = 0x19010003 */
    ASSERT(do_asm("LSLI r0, r1, 3", buf, BUFSZ, &blen) == 0, "LSLI: no errors");
    ASSERT(read_word(buf,0) == 0x19010003,                  "LSLI r0,r1,3 encoding");

    /* LSRI r0, r1, 5 → (0x1A<<24)|(0<<20)|(1<<16)|5 = 0x1A010005 */
    ASSERT(do_asm("LSRI r0, r1, 5", buf, BUFSZ, &blen) == 0, "LSRI: no errors");
    ASSERT(read_word(buf,0) == 0x1A010005,                  "LSRI r0,r1,5 encoding");

    /* ASRI r0, r1, 7 → (0x1B<<24)|(0<<20)|(1<<16)|7 = 0x1B010007 */
    ASSERT(do_asm("ASRI r0, r1, 7", buf, BUFSZ, &blen) == 0, "ASRI: no errors");
    ASSERT(read_word(buf,0) == 0x1B010007,                  "ASRI r0,r1,7 encoding");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Memory instructions: LW, SW, LH, LHU, LB, LBU, SH, SB                    */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_mem(void) {
    /* LW r0, r1, 4 → (0x0B<<24)|(0<<20)|(1<<16)|4 = 0x0B010004 */
    ASSERT(do_asm("LW r0, r1, 4", buf, BUFSZ, &blen) == 0, "LW comma: no errors");
    ASSERT(read_word(buf,0) == 0x0B010004,               "LW r0,r1,4 encoding");

    /* LW r0, [r1+4] → same */
    ASSERT(do_asm("LW r0, [r1+4]", buf, BUFSZ, &blen) == 0, "LW bracket+: no errors");
    ASSERT(read_word(buf,0) == 0x0B010004,                "LW [r1+4] encoding");

    /* LW r0, [r1] → LW r0, r1, 0 → 0x0B010000 */
    ASSERT(do_asm("LW r0, [r1]", buf, BUFSZ, &blen) == 0, "LW bracket no offset: no errors");
    ASSERT(read_word(buf,0) == 0x0B010000,              "LW [r1] encoding");

    /* LW r0, [r1-4] → imm = -4 = 0xFFFC → 0x0B01FFFC */
    ASSERT(do_asm("LW r0, [r1-4]", buf, BUFSZ, &blen) == 0, "LW bracket-: no errors");
    ASSERT(read_word(buf,0) == 0x0B01FFFC,                "LW [r1-4] encoding");

    /* SW r0, r1, 0 → (0x0C<<24)|(0<<20)|(1<<16)|0 = 0x0C010000 */
    ASSERT(do_asm("SW r0, r1, 0", buf, BUFSZ, &blen) == 0, "SW: no errors");
    ASSERT(read_word(buf,0) == 0x0C010000,               "SW r0,r1,0 encoding");

    /* SW r0, [r1+8] → 0x0C010008 */
    ASSERT(do_asm("SW r0, [r1+8]", buf, BUFSZ, &blen) == 0, "SW bracket: no errors");
    ASSERT(read_word(buf,0) == 0x0C010008,                "SW [r1+8] encoding");

    /* LH r2, r3, 2 → (0x1C<<24)|(2<<20)|(3<<16)|2 = 0x1C230002 */
    ASSERT(do_asm("LH r2, r3, 2", buf, BUFSZ, &blen) == 0, "LH: no errors");
    ASSERT(read_word(buf,0) == 0x1C230002,               "LH encoding");

    /* LHU r0, r0, 0 → (0x1D<<24) = 0x1D000000 */
    ASSERT(do_asm("LHU r0, r0, 0", buf, BUFSZ, &blen) == 0, "LHU: no errors");
    ASSERT(read_word(buf,0) == 0x1D000000,                "LHU encoding");

    /* LB r0, r1, 1 → (0x1E<<24)|(0<<20)|(1<<16)|1 = 0x1E010001 */
    ASSERT(do_asm("LB r0, r1, 1", buf, BUFSZ, &blen) == 0, "LB: no errors");
    ASSERT(read_word(buf,0) == 0x1E010001,               "LB encoding");

    /* LBU r0, r1, 0 → 0x1F010000 */
    ASSERT(do_asm("LBU r0, r1, 0", buf, BUFSZ, &blen) == 0, "LBU: no errors");
    ASSERT(read_word(buf,0) == 0x1F010000,                "LBU encoding");

    /* SH r0, r1, 0 → (0x20<<24)|(0<<20)|(1<<16) = 0x20010000 */
    ASSERT(do_asm("SH r0, r1, 0", buf, BUFSZ, &blen) == 0, "SH: no errors");
    ASSERT(read_word(buf,0) == 0x20010000,               "SH encoding");

    /* SB r0, r1, 0 → 0x21010000 */
    ASSERT(do_asm("SB r0, r1, 0", buf, BUFSZ, &blen) == 0, "SB: no errors");
    ASSERT(read_word(buf,0) == 0x21010000,               "SB encoding");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* No-operand: HALT, SYSCALL, SYSRET                                           */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_nooperand(void) {
    ASSERT(do_asm("HALT",    buf, BUFSZ, &blen) == 0, "HALT: no errors");
    ASSERT(read_word(buf,0) == 0x12000000,         "HALT encoding");

    ASSERT(do_asm("SYSCALL", buf, BUFSZ, &blen) == 0, "SYSCALL: no errors");
    ASSERT(read_word(buf,0) == 0x10000000,         "SYSCALL encoding");

    ASSERT(do_asm("SYSRET",  buf, BUFSZ, &blen) == 0, "SYSRET: no errors");
    ASSERT(read_word(buf,0) == 0x11000000,         "SYSRET encoding");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Branch instructions                                                         */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_branch(void) {
    /* BEQ with label: instr at 0x00, target at 0x08 → offset=8
       enc = (0x0D<<24)|(0<<20)|(8&0xFFFFF) = 0x0D000008 */
    const char *src =
        "BEQ target\n"
        "NOP_WORD: .word 0\n"
        "target: HALT\n";
    ASSERT(do_asm(src, buf, BUFSZ, &blen) == 0, "BEQ forward label: no errors");
    ASSERT(read_word(buf,0) == 0x0D000008,   "BEQ forward label encoding");

    /* BNE */
    const char *src2 =
        "BNE lbl\n"
        ".word 0\n"
        "lbl: HALT\n";
    ASSERT(do_asm(src2, buf, BUFSZ, &blen) == 0, "BNE: no errors");
    ASSERT(read_word(buf,0) == 0x0D100008,    "BNE forward encoding");

    /* BLT cond=2 */
    const char *src3 =
        "BLT lbl\n"
        ".word 0\n"
        "lbl: HALT\n";
    ASSERT(do_asm(src3, buf, BUFSZ, &blen) == 0, "BLT: no errors");
    ASSERT(read_word(buf,0) == 0x0D200008,    "BLT forward encoding");

    /* BGE cond=3 */
    const char *src4 =
        "BGE lbl\n"
        ".word 0\n"
        "lbl: HALT\n";
    ASSERT(do_asm(src4, buf, BUFSZ, &blen) == 0, "BGE: no errors");
    ASSERT(read_word(buf,0) == 0x0D300008,    "BGE forward encoding");

    /* BLTU cond=4 */
    const char *src5 =
        "BLTU lbl\n"
        ".word 0\n"
        "lbl: HALT\n";
    ASSERT(do_asm(src5, buf, BUFSZ, &blen) == 0, "BLTU: no errors");
    ASSERT(read_word(buf,0) == 0x0D400008,    "BLTU forward encoding");

    /* BGEU cond=5 */
    const char *src6 =
        "BGEU lbl\n"
        ".word 0\n"
        "lbl: HALT\n";
    ASSERT(do_asm(src6, buf, BUFSZ, &blen) == 0, "BGEU: no errors");
    ASSERT(read_word(buf,0) == 0x0D500008,    "BGEU forward encoding");

    /* Backward branch: target before branch */
    const char *src7 =
        "target: HALT\n"
        ".word 0\n"
        "BEQ target\n";
    ASSERT(do_asm(src7, buf, BUFSZ, &blen) == 0, "BEQ backward: no errors");
    /* instr at 0x08, target at 0x00 → offset = -8 = 0xFFFF8 */
    ASSERT(read_word(buf,2) == (0x0D000000u | ((-8)&0xFFFFF)), "BEQ backward encoding");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* JMP instruction                                                             */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_jump(void) {
    /* JMP r14, target: instr at 0x00, 4 padding words (0x04-0x10), target at 0x14
       offset = 0x14 - 0x00 = 20 = 0x14
       enc = (0x0E<<24)|(14<<20)|(0x14) = 0x0EE00014 */
    const char *src =
        "JMP r14, target\n"
        ".word 0\n"
        ".word 0\n"
        ".word 0\n"
        ".word 0\n"
        "target: HALT\n";
    ASSERT(do_asm(src, buf, BUFSZ, &blen) == 0, "JMP forward label: no errors");
    ASSERT(read_word(buf,0) == 0x0EE00014,   "JMP r14,target encoding");

    /* JMP r0, label (no-link jump) */
    const char *src2 =
        "JMP r0, lbl\n"
        "lbl: HALT\n";
    ASSERT(do_asm(src2, buf, BUFSZ, &blen) == 0, "JMP r0 no-link: no errors");
    ASSERT(read_word(buf,0) == 0x0E000004,    "JMP r0,lbl encoding (offset=4)");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Register aliases: sp, lr, pc                                                */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_reg_aliases(void) {
    /* MOV sp, r0 → MOV r13, r0 → (0x27<<24)|(13<<20)|(0<<16) = 0x27D00000 */
    ASSERT(do_asm("MOV sp, r0", buf, BUFSZ, &blen) == 0, "MOV sp,r0: no errors");
    ASSERT(read_word(buf,0) == 0x27D00000,             "MOV sp,r0 encoding");

    /* MOV lr, r0 → (0x27<<24)|(14<<20)|(0<<16) = 0x27E00000 */
    ASSERT(do_asm("MOV lr, r0", buf, BUFSZ, &blen) == 0, "MOV lr,r0: no errors");
    ASSERT(read_word(buf,0) == 0x27E00000,             "MOV lr,r0 encoding");

    /* JMP lr, target — lr as rd */
    const char *src =
        "JMP lr, lbl\n"
        "lbl: HALT\n";
    ASSERT(do_asm(src, buf, BUFSZ, &blen) == 0, "JMP lr,lbl: no errors");
    ASSERT(read_word(buf,0) == 0x0EE00004,   "JMP lr,lbl encoding");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Directives: .word, .byte, .ascii, .asciiz, .space, .org                   */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_directives(void) {
    /* .word */
    ASSERT(do_asm(".word 0xDEADBEEF", buf, BUFSZ, &blen) == 0, ".word: no errors");
    ASSERT(blen == 4,                                         ".word: 4 bytes");
    ASSERT(read_word(buf,0) == 0xDEADBEEF,                   ".word value");

    /* .byte */
    ASSERT(do_asm(".byte 0xAB", buf, BUFSZ, &blen) == 0, ".byte: no errors");
    ASSERT(blen == 1,                                  ".byte: 1 byte");
    ASSERT(buf[0] == 0xAB,                             ".byte value");

    /* .ascii */
    ASSERT(do_asm(".ascii \"hi\"", buf, BUFSZ, &blen) == 0, ".ascii: no errors");
    ASSERT(blen == 2,                                     ".ascii: 2 bytes");
    ASSERT(buf[0] == 'h' && buf[1] == 'i',               ".ascii content");

    /* .asciiz */
    ASSERT(do_asm(".asciiz \"hi\"", buf, BUFSZ, &blen) == 0, ".asciiz: no errors");
    ASSERT(blen == 3,                                      ".asciiz: 3 bytes (incl null)");
    ASSERT(buf[0]=='h' && buf[1]=='i' && buf[2]=='\0',    ".asciiz content");

    /* .space */
    ASSERT(do_asm(".space 8", buf, BUFSZ, &blen) == 0, ".space 8: no errors");
    ASSERT(blen == 8,                                ".space 8: 8 bytes");
    ASSERT(buf[0]==0 && buf[7]==0,                   ".space zeros");

    /* .org */
    const char *src_org =
        ".org 0x10\n"
        "HALT\n";
    ASSERT(do_asm(src_org, buf, BUFSZ, &blen) == 0, ".org: no errors");
    ASSERT(blen == 0x14,                          ".org: output includes padding to 0x10 + 4 bytes");
    /* Check padding is zero */
    ASSERT(buf[0]==0 && buf[0xF]==0,              ".org: padding is zero");
    /* Check HALT at 0x10 */
    ASSERT(read_word(buf, 4) == 0x12000000,       ".org: HALT at 0x10");

    /* .org with label address */
    const char *src_org2 =
        ".org 0x100\n"
        "start: HALT\n";
    ASSERT(do_asm(src_org2, buf, BUFSZ, &blen) == 0, ".org 0x100: no errors");
    /* start label should be at 0x100 */
    Symbol *s = find_symbol("start");
    ASSERT(s != NULL,              ".org 0x100: start symbol exists");
    ASSERT(s && s->addr == 0x100, ".org 0x100: start address correct");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Labels and forward references                                               */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_labels(void) {
    /* Simple label on same line as instruction */
    const char *src =
        "start: HALT\n";
    ASSERT(do_asm(src, buf, BUFSZ, &blen) == 0, "label on instr line: no errors");
    Symbol *s = find_symbol("start");
    ASSERT(s != NULL,            "label 'start' exists");
    ASSERT(s && s->addr == 0,    "label 'start' at address 0");

    /* Multiple labels */
    const char *src2 =
        "a: HALT\n"
        "b: HALT\n"
        "c: HALT\n";
    ASSERT(do_asm(src2, buf, BUFSZ, &blen) == 0, "multiple labels: no errors");
    Symbol *sa = find_symbol("a");
    Symbol *sb = find_symbol("b");
    Symbol *sc = find_symbol("c");
    ASSERT(sa && sa->addr == 0, "label a=0");
    ASSERT(sb && sb->addr == 4, "label b=4");
    ASSERT(sc && sc->addr == 8, "label c=8");

    /* Forward reference in branch */
    const char *src3 =
        "BEQ done\n"
        "ADD r0, r1, r2\n"
        "done: HALT\n";
    ASSERT(do_asm(src3, buf, BUFSZ, &blen) == 0,    "forward ref branch: no errors");
    /* BEQ at 0, target=8, offset=8 */
    ASSERT(read_word(buf,0) == 0x0D000008, "forward ref BEQ encoding");

    /* .global directive */
    const char *src4 =
        ".global main\n"
        "main: HALT\n";
    ASSERT(do_asm(src4, buf, BUFSZ, &blen) == 0, ".global: no errors");
    Symbol *sm = find_symbol("main");
    ASSERT(sm != NULL,            ".global main: symbol exists");
    ASSERT(sm && sm->is_global,   ".global main: is_global set");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Error cases                                                                 */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_errors(void) {
    /* Undefined label */
    ASSERT(do_asm("BEQ undefined_label", buf, BUFSZ, &blen) > 0,
           "undefined label: error");

    /* Bad register */
    ASSERT(do_asm("ADD r0, r99, r1", buf, BUFSZ, &blen) > 0,
           "bad register r99: error");

    /* Unknown mnemonic */
    ASSERT(do_asm("FOOBAR r0, r1", buf, BUFSZ, &blen) > 0,
           "unknown mnemonic: error");

    /* Duplicate label */
    ASSERT(do_asm("foo: HALT\nfoo: HALT\n", buf, BUFSZ, &blen) > 0,
           "duplicate label: error");

    /* MOVI with value too large */
    ASSERT(do_asm("MOVI r0, 0x10000", buf, BUFSZ, &blen) > 0,
           "MOVI 0x10000 out of range: error");

    /* ADDI with signed value out of range */
    ASSERT(do_asm("ADDI r0, r1, -32769", buf, BUFSZ, &blen) > 0,
           "ADDI -32769 out of range: error");

    /* ANDI with negative value (unsigned, should error) */
    ASSERT(do_asm("ANDI r0, r1, -1", buf, BUFSZ, &blen) > 0,
           "ANDI -1 unsigned: error");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Comments and whitespace                                                     */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_comments(void) {
    const char *src =
        "; This is a comment\n"
        "HALT  ; inline comment\n";
    ASSERT(do_asm(src, buf, BUFSZ, &blen) == 0, "comments: no errors");
    ASSERT(blen == 4,                         "comments: 1 instruction");
    ASSERT(read_word(buf,0) == 0x12000000,    "comments: HALT encoding");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Case insensitivity                                                          */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_case_insensitive(void) {
    ASSERT(do_asm("halt", buf, BUFSZ, &blen) == 0, "lowercase halt: no errors");
    ASSERT(read_word(buf,0) == 0x12000000,       "lowercase halt encoding");

    ASSERT(do_asm("Halt", buf, BUFSZ, &blen) == 0, "mixed case Halt: no errors");
    ASSERT(read_word(buf,0) == 0x12000000,       "mixed case Halt encoding");

    ASSERT(do_asm("add r0, r1, r2", buf, BUFSZ, &blen) == 0, "lowercase add: no errors");
    ASSERT(read_word(buf,0) == 0x00012000,                 "lowercase add encoding");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Extended R3 instructions                                                    */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_extended_r3(void) {
    /* MULH r0, r1, r2 → (0x22<<24)|(0<<20)|(1<<16)|(2<<12) = 0x22012000 */
    ASSERT(do_asm("MULH r0, r1, r2", buf, BUFSZ, &blen) == 0, "MULH: no errors");
    ASSERT(read_word(buf,0) == 0x22012000,                  "MULH encoding");

    /* MULHU r0, r1, r2 → 0x23012000 */
    ASSERT(do_asm("MULHU r0, r1, r2", buf, BUFSZ, &blen) == 0, "MULHU: no errors");
    ASSERT(read_word(buf,0) == 0x23012000,                   "MULHU encoding");

    /* DIVU r0, r1, r2 → 0x24012000 */
    ASSERT(do_asm("DIVU r0, r1, r2", buf, BUFSZ, &blen) == 0, "DIVU: no errors");
    ASSERT(read_word(buf,0) == 0x24012000,                  "DIVU encoding");

    /* MOD r0, r1, r2 → 0x25012000 */
    ASSERT(do_asm("MOD r0, r1, r2", buf, BUFSZ, &blen) == 0, "MOD: no errors");
    ASSERT(read_word(buf,0) == 0x25012000,                 "MOD encoding");

    /* MODU r0, r1, r2 → 0x26012000 */
    ASSERT(do_asm("MODU r0, r1, r2", buf, BUFSZ, &blen) == 0, "MODU: no errors");
    ASSERT(read_word(buf,0) == 0x26012000,                  "MODU encoding");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Multi-instruction programs                                                  */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_multi_instr(void) {
    /* Simple: two instructions */
    const char *src =
        "ADD r0, r1, r2\n"
        "HALT\n";
    ASSERT(do_asm(src, buf, BUFSZ, &blen) == 0, "multi-instr: no errors");
    ASSERT(blen == 8,                         "multi-instr: 8 bytes");
    ASSERT(read_word(buf,0) == 0x00012000,    "multi-instr: ADD");
    ASSERT(read_word(buf,1) == 0x12000000,    "multi-instr: HALT");

    /* .word interleaved */
    const char *src2 =
        "HALT\n"
        ".word 0xCAFEBABE\n"
        "HALT\n";
    ASSERT(do_asm(src2, buf, BUFSZ, &blen) == 0, "interleaved .word: no errors");
    ASSERT(blen == 12,                         "interleaved .word: 12 bytes");
    ASSERT(read_word(buf,1) == 0xCAFEBABE,    "interleaved .word value");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Fibonacci-like program: assemble and check known encodings                 */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_fibonacci_program(void) {
    /* A small fib-like loop:
     *   r0 = 0 (a)
     *   r1 = 1 (b)
     *   r2 = 10 (counter)
     * loop:
     *   ADD r3, r0, r1   ; r3 = a + b
     *   MOV r0, r1        ; a = b
     *   MOV r1, r3        ; b = r3
     *   SUBI r2, r2, 1    ; counter--
     *   BNE loop          ; if counter != 0 goto loop
     *   HALT
     */
    const char *src =
        "MOVI r0, 0\n"          /* 0x00: 0F000000 */
        "MOVI r1, 1\n"          /* 0x04: 0F100001 */
        "MOVI r2, 10\n"         /* 0x08: 0F20000A */
        "loop: ADD r3, r0, r1\n"/* 0x0C: 00301000 */
        "MOV r0, r1\n"          /* 0x10: 27001000 — wait, rd=0, rs1=1 */
        "MOV r1, r3\n"          /* 0x14: 27103000 */
        "SUBI r2, r2, 1\n"      /* 0x18: 15220001 */
        "BNE loop\n"            /* 0x1C: offset = 0x0C - 0x1C = -16 */
        "HALT\n";               /* 0x20 */

    ASSERT(do_asm(src, buf, BUFSZ, &blen) == 0, "fib program: no errors");
    ASSERT(blen == 0x24,                      "fib program: 36 bytes");

    /* Check MOVI r0, 0 → 0x0F000000 */
    ASSERT(read_word(buf,0) == 0x0F000000, "fib: MOVI r0,0");
    /* Check MOVI r1, 1 → (0x0F<<24)|(1<<20)|1 = 0x0F100001 */
    ASSERT(read_word(buf,1) == 0x0F100001, "fib: MOVI r1,1");
    /* Check MOVI r2, 10 → (0x0F<<24)|(2<<20)|10 = 0x0F20000A */
    ASSERT(read_word(buf,2) == 0x0F20000A, "fib: MOVI r2,10");
    /* Check ADD r3, r0, r1 → (0x00<<24)|(3<<20)|(0<<16)|(1<<12) = 0x00301000 */
    ASSERT(read_word(buf,3) == 0x00301000, "fib: ADD r3,r0,r1");
    /* Check MOV r0, r1 → (0x27<<24)|(0<<20)|(1<<16) = 0x27010000 */
    ASSERT(read_word(buf,4) == 0x27010000, "fib: MOV r0,r1");
    /* Check MOV r1, r3 → (0x27<<24)|(1<<20)|(3<<16) = 0x27130000 */
    ASSERT(read_word(buf,5) == 0x27130000, "fib: MOV r1,r3");
    /* Check SUBI r2, r2, 1 → (0x15<<24)|(2<<20)|(2<<16)|1 = 0x15220001 */
    ASSERT(read_word(buf,6) == 0x15220001, "fib: SUBI r2,r2,1");
    /* BNE loop: instr at 0x1C, target=0x0C, offset=-16=0xFFFF0
       enc = (0x0D<<24)|(1<<20)|(0xFFFF0) = 0x0D1FFFF0 */
    ASSERT(read_word(buf,7) == 0x0D1FFFF0, "fib: BNE loop (backward)");
    /* HALT */
    ASSERT(read_word(buf,8) == 0x12000000, "fib: HALT");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Expression evaluation in operands                                          */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_expressions(void) {
    /* ADDI r0, r1, 3+4 should give imm=7 */
    ASSERT(do_asm("ADDI r0, r1, 3+4", buf, BUFSZ, &blen) == 0, "ADDI expr 3+4: no errors");
    ASSERT(read_word(buf,0) == ((0x14u<<24)|(0<<20)|(1<<16)|7), "ADDI expr 3+4 result");

    /* .word label_val where label_val is address of a label */
    const char *src =
        "start: HALT\n"
        ".word start\n";
    ASSERT(do_asm(src, buf, BUFSZ, &blen) == 0, ".word label: no errors");
    ASSERT(read_word(buf,1) == 0,             ".word start (=0)");

    /* .word label + 4 */
    const char *src2 =
        "start2: HALT\n"
        ".word start2+4\n";
    ASSERT(do_asm(src2, buf, BUFSZ, &blen) == 0, ".word label+4: no errors");
    ASSERT(read_word(buf,1) == 4,              ".word start2+4 = 4");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* New instructions: BA, CMP, CMPI                                             */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_ba(void) {
    uint8_t buf[32]; size_t blen = sizeof(buf);
    /* BA to 'done' at offset +8 from instr at 0x00:
       (0x0D<<24)|(6<<20)|8 = 0x0D600008 */
    const char *src =
        "start: BA done\n"
        "MOVI r1, 99\n"
        "done: HALT\n";
    ASSERT(do_asm(src, buf, sizeof(buf), &blen) == 0, "BA: no errors");
    ASSERT(read_word(buf, 0) == 0x0D600008u, "BA: cond=6, offset=+8");
    ASSERT(read_word(buf, 2) == 0x12000000u, "HALT after BA");
}

static void test_ba_backward(void) {
    uint8_t buf[32]; size_t blen = sizeof(buf);
    /* BA back to top: instr at 4, target at 0 → offset = -4
       (-4) & 0xFFFFF = 0xFFFC → 0x0D6FFFFC */
    const char *src =
        "top: HALT\n"
        "     BA top\n";
    ASSERT(do_asm(src, buf, sizeof(buf), &blen) == 0, "BA backward: no errors");
    ASSERT(read_word(buf, 1) == 0x0D6FFFFCu, "BA backward offset=-4");
}

static void test_cmp_encoding(void) {
    uint8_t buf[16]; size_t blen = sizeof(buf);
    /* CMP r3, r5: opcode=0x28, rd=0, rs1=3, rs2=5
       (0x28<<24)|(3<<16)|(5<<12) = 0x28035000 */
    const char *src = "CMP r3, r5\nHALT\n";
    ASSERT(do_asm(src, buf, sizeof(buf), &blen) == 0, "CMP: no errors");
    ASSERT(read_word(buf, 0) == 0x28035000u, "CMP r3,r5 encoding");
}

static void test_cmpi_encoding(void) {
    uint8_t buf[16]; size_t blen = sizeof(buf);
    /* CMPI r2, 100: opcode=0x29, rd=0, rs1=2, imm=100=0x64
       (0x29<<24)|(2<<16)|100 = 0x29020064 */
    const char *src = "CMPI r2, 100\nHALT\n";
    ASSERT(do_asm(src, buf, sizeof(buf), &blen) == 0, "CMPI: no errors");
    ASSERT(read_word(buf, 0) == 0x29020064u, "CMPI r2,100 encoding");
}

static void test_cmpi_large_encoding(void) {
    uint8_t buf[16]; size_t blen = sizeof(buf);
    /* CMPI r1, 50000 (0xC350) — previously out of signed range, now valid */
    const char *src = "CMPI r1, 50000\nHALT\n";
    ASSERT(do_asm(src, buf, sizeof(buf), &blen) == 0, "CMPI 50000: no errors");
    ASSERT(read_word(buf, 0) == 0x2901C350u, "CMPI r1,50000 encoding");

    /* CMPI r1, 0xFFFF (65535) — max valid */
    ASSERT(do_asm("CMPI r1, 0xFFFF\nHALT\n", buf, sizeof(buf), &blen) == 0, "CMPI 0xFFFF: no errors");
    ASSERT(read_word(buf, 0) == 0x2901FFFFu, "CMPI r1,0xFFFF encoding");

    /* CMPI r1, -1 — out of range (negative not allowed) */
    ASSERT(do_asm("CMPI r1, -1\n", buf, sizeof(buf), &blen) > 0, "CMPI -1 should error");

    /* CMPI r1, 65536 — out of range */
    ASSERT(do_asm("CMPI r1, 65536\n", buf, sizeof(buf), &blen) > 0, "CMPI 65536 should error");
}

static void test_ret_encoding(void) {
    uint8_t buf[8]; size_t blen = sizeof(buf);
    /* RET = MOV pc, lr = MOV r15, r14
       (0x27<<24)|(15<<20)|(14<<16) = 0x27FE0000 */
    const char *src = "RET\n";
    ASSERT(do_asm(src, buf, sizeof(buf), &blen) == 0, "RET: no errors");
    ASSERT(read_word(buf, 0) == 0x27FE0000u, "RET encoding = MOV pc,lr");
}

static void test_br_encoding(void) {
    uint8_t buf[8]; size_t blen = sizeof(buf);
    /* BR r0 = MOV pc, r0 = (0x27<<24)|(15<<20)|(0<<16) = 0x27F00000 */
    const char *src = "BR r0\n";
    ASSERT(do_asm(src, buf, sizeof(buf), &blen) == 0, "BR r0: no errors");
    ASSERT(read_word(buf, 0) == 0x27F00000u, "BR r0 encoding = MOV pc,r0");

    /* BR r5 = (0x27<<24)|(15<<20)|(5<<16) = 0x27F50000 */
    size_t blen2 = sizeof(buf);
    const char *src2 = "BR r5\n";
    ASSERT(do_asm(src2, buf, sizeof(buf), &blen2) == 0, "BR r5: no errors");
    ASSERT(read_word(buf, 0) == 0x27F50000u, "BR r5 encoding = MOV pc,r5");
}

static void test_ret_in_subroutine(void) {
    uint8_t buf[64]; size_t blen = sizeof(buf);
    /* Typical call/return frame: JMP lr,func + body + RET */
    const char *src =
        "      MOVI  r0, 42\n"
        "      JMP   lr, func\n"   /* call */
        "      HALT\n"
        "func: ADDI  r0, r0, 1\n"
        "      RET\n";
    ASSERT(do_asm(src, buf, sizeof(buf), &blen) == 0, "call/ret: no errors");
    /* func is at byte 12; JMP at byte 4, offset = 12-4 = 8
       JMP lr = (0x0E<<24)|(14<<20)|8 = 0x0EE00008 */
    ASSERT(read_word(buf, 1) == 0x0EE00008u, "JMP lr,func encoding");
    /* RET at byte 16 = 0x27FE0000 */
    ASSERT(read_word(buf, 4) == 0x27FE0000u, "RET at end of func");
}

static void test_addc_subc_encoding(void) {
    uint8_t buf[8]; size_t blen;

    /* ADDC r0, r1, r2 → (0x2B<<24)|(0<<20)|(1<<16)|(2<<12) = 0x2B012000 */
    blen = sizeof(buf);
    ASSERT(do_asm("ADDC r0, r1, r2", buf, sizeof(buf), &blen) == 0, "ADDC: no errors");
    ASSERT(read_word(buf,0) == 0x2B012000u, "ADDC r0,r1,r2 encoding");

    /* SUBC r3, r4, r5 → (0x2C<<24)|(3<<20)|(4<<16)|(5<<12) = 0x2C345000 */
    blen = sizeof(buf);
    ASSERT(do_asm("SUBC r3, r4, r5", buf, sizeof(buf), &blen) == 0, "SUBC: no errors");
    ASSERT(read_word(buf,0) == 0x2C345000u, "SUBC r3,r4,r5 encoding");
}

static void test_push_pop_encoding(void) {
    uint8_t buf[32]; size_t blen;

    /* PUSH r0 expands to:
       ADDI sp, sp, -4 → (0x14<<24)|(13<<20)|(13<<16)|0xFFFC = 0x14DDFFFC
       SW   r0, [sp]   → (0x0C<<24)|(0<<20) |(13<<16)|0      = 0x0C0D0000 */
    blen = sizeof(buf);
    ASSERT(do_asm("PUSH r0", buf, sizeof(buf), &blen) == 0, "PUSH r0: no errors");
    ASSERT(blen == 8,                              "PUSH r0: 8 bytes");
    ASSERT(read_word(buf,0) == 0x14DDFFfCu,        "PUSH r0: ADDI sp,sp,-4");
    ASSERT(read_word(buf,1) == 0x0C0D0000u,        "PUSH r0: SW r0,[sp]");

    /* PUSH lr expands to:
       ADDI sp, sp, -4 → 0x14DDFFFC
       SW   lr, [sp]   → (0x0C<<24)|(14<<20)|(13<<16)|0 = 0x0CED0000 */
    blen = sizeof(buf);
    ASSERT(do_asm("PUSH lr", buf, sizeof(buf), &blen) == 0, "PUSH lr: no errors");
    ASSERT(read_word(buf,0) == 0x14DDFFfCu,        "PUSH lr: ADDI sp,sp,-4");
    ASSERT(read_word(buf,1) == 0x0CED0000u,        "PUSH lr: SW lr,[sp]");

    /* POP r1 expands to:
       LW   r1, [sp]   → (0x0B<<24)|(1<<20)|(13<<16)|0 = 0x0B1D0000
       ADDI sp, sp, 4  → (0x14<<24)|(13<<20)|(13<<16)|4 = 0x14DD0004 */
    blen = sizeof(buf);
    ASSERT(do_asm("POP r1", buf, sizeof(buf), &blen) == 0, "POP r1: no errors");
    ASSERT(blen == 8,                              "POP r1: 8 bytes");
    ASSERT(read_word(buf,0) == 0x0B1D0000u,        "POP r1: LW r1,[sp]");
    ASSERT(read_word(buf,1) == 0x14DD0004u,        "POP r1: ADDI sp,sp,4");
}

static void test_align_directive(void) {
    uint8_t buf[64]; size_t blen;

    /* .byte followed by .align 4 — should pad to 4-byte boundary */
    const char *src1 =
        ".byte 0x42\n"
        ".align 4\n"
        "HALT\n";
    blen = sizeof(buf);
    ASSERT(do_asm(src1, buf, sizeof(buf), &blen) == 0, ".align 4: no errors");
    ASSERT(blen == 8,            ".align 4: total 8 bytes (1 + 3 pad + 4)");
    ASSERT(buf[0] == 0x42,       ".align 4: byte value preserved");
    ASSERT(buf[1] == 0 && buf[2] == 0 && buf[3] == 0, ".align 4: padding is zero");
    ASSERT(read_word(buf,1) == 0x12000000u, ".align 4: HALT at aligned address");

    /* .align 8 with 5 bytes — should pad 3 bytes */
    const char *src2 =
        ".byte 0xAA\n"
        ".byte 0xBB\n"
        ".byte 0xCC\n"
        ".byte 0xDD\n"
        ".byte 0xEE\n"
        ".align 8\n"
        "HALT\n";
    blen = sizeof(buf);
    ASSERT(do_asm(src2, buf, sizeof(buf), &blen) == 0, ".align 8: no errors");
    ASSERT(blen == 12,           ".align 8: 5+3pad+4 = 12 bytes");
    ASSERT(read_word(buf,2) == 0x12000000u, ".align 8: HALT at offset 8");

    /* label address reflects alignment */
    const char *src3 =
        ".byte 0\n"
        ".align 4\n"
        "aligned: HALT\n";
    blen = sizeof(buf);
    ASSERT(do_asm(src3, buf, sizeof(buf), &blen) == 0, ".align label: no errors");
    Symbol *s = find_symbol("aligned");
    ASSERT(s && s->addr == 4, ".align: label address is 4");
}

static void test_bgt_ble_encoding(void) {
    uint8_t buf[16]; size_t blen;

    /* BGT cond=7: (0x0D<<24)|(7<<20)|offset
       BGT lbl at addr 0, lbl at addr 8: offset=8 → 0x0D700008 */
    const char *src1 =
        "BGT lbl\n"
        ".word 0\n"
        "lbl: HALT\n";
    blen = sizeof(buf);
    ASSERT(do_asm(src1, buf, sizeof(buf), &blen) == 0, "BGT: no errors");
    ASSERT(read_word(buf, 0) == 0x0D700008u, "BGT forward encoding");

    /* BLE cond=8: (0x0D<<24)|(8<<20)|offset
       BLE lbl at addr 0, lbl at addr 8: offset=8 → 0x0D800008 */
    const char *src2 =
        "BLE lbl\n"
        ".word 0\n"
        "lbl: HALT\n";
    blen = sizeof(buf);
    ASSERT(do_asm(src2, buf, sizeof(buf), &blen) == 0, "BLE: no errors");
    ASSERT(read_word(buf, 0) == 0x0D800008u, "BLE forward encoding");
}

static void test_callr_encoding(void) {
    uint8_t buf[8]; size_t blen = sizeof(buf);
    /* CALLR lr, r5: opcode=0x2A, rd=lr=14, rs1=r5=5
       (0x2A<<24)|(14<<20)|(5<<16) = 0x2AE50000 */
    const char *src = "CALLR lr, r5\n";
    ASSERT(do_asm(src, buf, sizeof(buf), &blen) == 0, "CALLR: no errors");
    ASSERT(read_word(buf, 0) == 0x2AE50000u, "CALLR lr,r5 encoding");

    /* CALLR r0, r3: (0x2A<<24)|(0<<20)|(3<<16) = 0x2A030000 */
    size_t blen2 = sizeof(buf);
    const char *src2 = "CALLR r0, r3\n";
    ASSERT(do_asm(src2, buf, sizeof(buf), &blen2) == 0, "CALLR r0,r3: no errors");
    ASSERT(read_word(buf, 0) == 0x2A030000u, "CALLR r0,r3 encoding");
}

static void test_call_encoding(void) {
    uint8_t buf[32]; size_t blen;
    /* CALL func — same encoding as JMP lr, func:
       at addr 0, func at addr 8 → (0x0E<<24)|(14<<20)|8 = 0x0EE00008 */
    const char *src =
        "CALL func\n"
        ".word 0\n"
        "func: HALT\n";
    blen = sizeof(buf);
    ASSERT(do_asm(src, buf, sizeof(buf), &blen) == 0, "CALL: no errors");
    ASSERT(read_word(buf, 0) == 0x0EE00008u, "CALL func encoding = JMP lr,func");

    /* Verify CALL and JMP lr produce identical output */
    uint8_t buf2[32]; size_t blen2 = sizeof(buf2);
    const char *src2 =
        "JMP lr, func\n"
        ".word 0\n"
        "func: HALT\n";
    ASSERT(do_asm(src2, buf2, sizeof(buf2), &blen2) == 0, "JMP lr for comparison: no errors");
    ASSERT(read_word(buf, 0) == read_word(buf2, 0), "CALL == JMP lr,target");
}

static void test_nop_encoding(void) {
    uint8_t buf[8]; size_t blen = sizeof(buf);
    /* NOP → MOVI r0, 0 = 0x0F000000 */
    const char *src = "NOP\n";
    ASSERT(do_asm(src, buf, sizeof(buf), &blen) == 0, "NOP: no errors");
    ASSERT(blen == 4,                              "NOP: 4 bytes");
    ASSERT(read_word(buf, 0) == 0x0F000000u,       "NOP encoding = MOVI r0,0");
}

static void test_cmp_in_loop(void) {
    uint8_t buf[64]; size_t blen = sizeof(buf);
    /* Verify CMP+BLT replaces the old SUB+BLT pattern cleanly.
       loop at byte 8; CMP at byte 12; BLT at byte 16 → offset=8-16=-8
       (-8)&0xFFFFF=0xFFFF8 → BLT = (0x0D<<24)|(2<<20)|0xFFFF8 = 0x0D2FFFF8 */
    const char *src =
        "      MOVI  r0, 0\n"
        "      MOVI  r1, 10\n"
        "loop: ADDI  r0, r0, 1\n"
        "      CMP   r0, r1\n"
        "      BLT   loop\n"
        "      HALT\n";
    ASSERT(do_asm(src, buf, sizeof(buf), &blen) == 0, "CMP loop: no errors");
    ASSERT(read_word(buf, 3) == 0x28001000u, "CMP r0,r1 encoding in loop");
    ASSERT(read_word(buf, 4) == 0x0D2FFFF8u, "BLT back to loop");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Indexed memory encoding: LWX/LBX/LBUX/SWX/SBX/LHX/LHUX/SHX              */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_indexed_mem_encoding(void) {
    uint8_t buf[8]; size_t blen;

    /* LWX r2, [r0+r1]  → (0x30<<24)|(2<<20)|(0<<16)|(1<<12) = 0x30201000 */
    blen = sizeof(buf);
    ASSERT(do_asm("LWX r2, [r0+r1]", buf, sizeof(buf), &blen) == 0, "LWX: no errors");
    ASSERT(read_word(buf,0) == 0x30201000u, "LWX r2,[r0+r1] encoding");

    /* LBX r1, [r2+r3]  → (0x31<<24)|(1<<20)|(2<<16)|(3<<12) = 0x31123000 */
    blen = sizeof(buf);
    ASSERT(do_asm("LBX r1, [r2+r3]", buf, sizeof(buf), &blen) == 0, "LBX: no errors");
    ASSERT(read_word(buf,0) == 0x31123000u, "LBX r1,[r2+r3] encoding");

    /* LBUX r3, [r4+r5] → (0x32<<24)|(3<<20)|(4<<16)|(5<<12) = 0x32345000 */
    blen = sizeof(buf);
    ASSERT(do_asm("LBUX r3, [r4+r5]", buf, sizeof(buf), &blen) == 0, "LBUX: no errors");
    ASSERT(read_word(buf,0) == 0x32345000u, "LBUX r3,[r4+r5] encoding");

    /* SWX r1, [r2+r3]  → (0x33<<24)|(1<<20)|(2<<16)|(3<<12) = 0x33123000 */
    blen = sizeof(buf);
    ASSERT(do_asm("SWX r1, [r2+r3]", buf, sizeof(buf), &blen) == 0, "SWX: no errors");
    ASSERT(read_word(buf,0) == 0x33123000u, "SWX r1,[r2+r3] encoding");

    /* SBX r0, [r1+r2]  → (0x34<<24)|(0<<20)|(1<<16)|(2<<12) = 0x34012000 */
    blen = sizeof(buf);
    ASSERT(do_asm("SBX r0, [r1+r2]", buf, sizeof(buf), &blen) == 0, "SBX: no errors");
    ASSERT(read_word(buf,0) == 0x34012000u, "SBX r0,[r1+r2] encoding");

    /* LHX r4, [r5+r6]  → (0x35<<24)|(4<<20)|(5<<16)|(6<<12) = 0x35456000 */
    blen = sizeof(buf);
    ASSERT(do_asm("LHX r4, [r5+r6]", buf, sizeof(buf), &blen) == 0, "LHX: no errors");
    ASSERT(read_word(buf,0) == 0x35456000u, "LHX r4,[r5+r6] encoding");

    /* LHUX r7, [r8+r9] → (0x36<<24)|(7<<20)|(8<<16)|(9<<12) = 0x36789000 */
    blen = sizeof(buf);
    ASSERT(do_asm("LHUX r7, [r8+r9]", buf, sizeof(buf), &blen) == 0, "LHUX: no errors");
    ASSERT(read_word(buf,0) == 0x36789000u, "LHUX r7,[r8+r9] encoding");

    /* SHX r2, [r3+r4]  → (0x37<<24)|(2<<20)|(3<<16)|(4<<12) = 0x37234000 */
    blen = sizeof(buf);
    ASSERT(do_asm("SHX r2, [r3+r4]", buf, sizeof(buf), &blen) == 0, "SHX: no errors");
    ASSERT(read_word(buf,0) == 0x37234000u, "SHX r2,[r3+r4] encoding");

    /* Omitted index defaults to r0 (zero): LWX r1, [r2] → rs2=0 = 0x30120000 */
    blen = sizeof(buf);
    ASSERT(do_asm("LWX r1, [r2]", buf, sizeof(buf), &blen) == 0, "LWX no index: no errors");
    ASSERT(read_word(buf,0) == 0x30120000u, "LWX r1,[r2] (no index = r0) encoding");

    /* Register aliases work: LWX sp, [lr+sp] = LWX r13, [r14+r13] */
    blen = sizeof(buf);
    ASSERT(do_asm("LWX sp, [lr+sp]", buf, sizeof(buf), &blen) == 0, "LWX aliases: no errors");
    /* (0x30<<24)|(13<<20)|(14<<16)|(13<<12) = 0x30DED000 */
    ASSERT(read_word(buf,0) == 0x30DED000u, "LWX sp,[lr+sp] encoding");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* .equ directive                                                              */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_equ_directive(void) {
    uint8_t buf[32]; size_t blen;

    /* Simple constant: .equ SIZE, 16; MOVI r0, SIZE */
    const char *src1 =
        ".equ SIZE, 16\n"
        "MOVI r0, SIZE\n";
    blen = sizeof(buf);
    ASSERT(do_asm(src1, buf, sizeof(buf), &blen) == 0, ".equ simple: no errors");
    ASSERT(blen == 4,                                   ".equ: no bytes for directive");
    /* MOVI r0, 16 = (0x0F<<24)|(0<<20)|16 = 0x0F000010 */
    ASSERT(read_word(buf, 0) == 0x0F000010u, ".equ SIZE=16 used in MOVI");

    /* Expression: .equ BASE, 0x100; .equ END, BASE+8 */
    const char *src2 =
        ".equ BASE, 0x100\n"
        ".equ OFF, 4\n"
        ".word BASE+OFF\n";
    blen = sizeof(buf);
    ASSERT(do_asm(src2, buf, sizeof(buf), &blen) == 0, ".equ expression: no errors");
    ASSERT(read_word(buf, 0) == 0x104u, ".equ BASE+OFF = 0x104");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* BGTU / BLEU encoding                                                        */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_bgtu_bleu_encoding(void) {
    uint8_t buf[16]; size_t blen;

    /* BGTU cond=9: (0x0D<<24)|(9<<20)|offset
       BGTU lbl at addr 0, lbl at addr 8: offset=8 → 0x0D900008 */
    const char *src1 =
        "BGTU lbl\n"
        ".word 0\n"
        "lbl: HALT\n";
    blen = sizeof(buf);
    ASSERT(do_asm(src1, buf, sizeof(buf), &blen) == 0, "BGTU: no errors");
    ASSERT(read_word(buf, 0) == 0x0D900008u, "BGTU forward encoding");

    /* BLEU cond=10: (0x0D<<24)|(10<<20)|offset
       BLEU lbl at addr 0, lbl at addr 8: offset=8 → 0x0DA00008 */
    const char *src2 =
        "BLEU lbl\n"
        ".word 0\n"
        "lbl: HALT\n";
    blen = sizeof(buf);
    ASSERT(do_asm(src2, buf, sizeof(buf), &blen) == 0, "BLEU: no errors");
    ASSERT(read_word(buf, 0) == 0x0DA00008u, "BLEU forward encoding");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* LSLR / LSRR / ASRR encoding                                                */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_lslr_lsrr_asrr_encoding(void) {
    uint8_t buf[8]; size_t blen;

    /* LSLR r0, r1, r2 → (0x2D<<24)|(0<<20)|(1<<16)|(2<<12) = 0x2D012000 */
    blen = sizeof(buf);
    ASSERT(do_asm("LSLR r0, r1, r2", buf, sizeof(buf), &blen) == 0, "LSLR: no errors");
    ASSERT(read_word(buf, 0) == 0x2D012000u, "LSLR r0,r1,r2 encoding");

    /* LSRR r3, r4, r5 → (0x2E<<24)|(3<<20)|(4<<16)|(5<<12) = 0x2E345000 */
    blen = sizeof(buf);
    ASSERT(do_asm("LSRR r3, r4, r5", buf, sizeof(buf), &blen) == 0, "LSRR: no errors");
    ASSERT(read_word(buf, 0) == 0x2E345000u, "LSRR r3,r4,r5 encoding");

    /* ASRR r1, r2, r3 → (0x2F<<24)|(1<<20)|(2<<16)|(3<<12) = 0x2F123000 */
    blen = sizeof(buf);
    ASSERT(do_asm("ASRR r1, r2, r3", buf, sizeof(buf), &blen) == 0, "ASRR: no errors");
    ASSERT(read_word(buf, 0) == 0x2F123000u, "ASRR r1,r2,r3 encoding");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* LUI encoding                                                                */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_lui_encoding(void) {
    uint8_t buf[8]; size_t blen;

    /* LUI r1, 0x1234 → (0x38<<24)|(1<<20)|0x1234 = 0x38101234 */
    ASSERT(do_asm("LUI r1, 0x1234", buf, BUFSZ, &blen) == 0, "LUI: no errors");
    ASSERT(blen == 4,                                          "LUI: 4 bytes");
    ASSERT(read_word(buf,0) == 0x38101234u,                    "LUI r1,0x1234 encoding");

    /* LUI r0, 0 → 0x38000000 */
    ASSERT(do_asm("LUI r0, 0", buf, BUFSZ, &blen) == 0, "LUI 0: no errors");
    ASSERT(read_word(buf,0) == 0x38000000u,              "LUI r0,0 encoding");

    /* LUI r0, 0xFFFF → (0x38<<24)|0xFFFF = 0x3800FFFF */
    ASSERT(do_asm("LUI r0, 0xFFFF", buf, BUFSZ, &blen) == 0, "LUI 0xFFFF: no errors");
    ASSERT(read_word(buf,0) == 0x3800FFFFu,                   "LUI r0,0xFFFF encoding");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* ROLR / RORR / ROLI / RORI encoding                                          */
/* ─────────────────────────────────────────────────────────────────────────── */
static void test_rol_ror_encoding(void) {
    uint8_t buf[8]; size_t blen;

    /* ROLR r0, r1, r2 → (0x39<<24)|(0<<20)|(1<<16)|(2<<12) = 0x39012000 */
    ASSERT(do_asm("ROLR r0, r1, r2", buf, BUFSZ, &blen) == 0, "ROLR: no errors");
    ASSERT(read_word(buf,0) == 0x39012000u, "ROLR r0,r1,r2 encoding");

    /* RORR r0, r1, r2 → (0x3A<<24)|(0<<20)|(1<<16)|(2<<12) = 0x3A012000 */
    ASSERT(do_asm("RORR r0, r1, r2", buf, BUFSZ, &blen) == 0, "RORR: no errors");
    ASSERT(read_word(buf,0) == 0x3A012000u, "RORR r0,r1,r2 encoding");

    /* ROLI r0, r1, 8 → (0x3B<<24)|(0<<20)|(1<<16)|(8<<7) = 0x3B010400 */
    ASSERT(do_asm("ROLI r0, r1, 8", buf, BUFSZ, &blen) == 0, "ROLI: no errors");
    ASSERT(read_word(buf,0) == 0x3B010400u, "ROLI r0,r1,8 encoding");

    /* RORI r0, r1, 1 → (0x3C<<24)|(0<<20)|(1<<16)|(1<<7) = 0x3C010080 */
    ASSERT(do_asm("RORI r0, r1, 1", buf, BUFSZ, &blen) == 0, "RORI: no errors");
    ASSERT(read_word(buf,0) == 0x3C010080u, "RORI r0,r1,1 encoding");
}

static void test_cas_encoding(void) {
    /* CAS r0, r1, r2 → (0x3D<<24)|(0<<20)|(1<<16)|(2<<12) = 0x3D012000 */
    ASSERT(do_asm("CAS r0, r1, r2", buf, BUFSZ, &blen) == 0, "CAS: no errors");
    ASSERT(blen == 4,                                         "CAS: 4 bytes");
    ASSERT(read_word(buf,0) == 0x3D012000,                    "CAS r0,r1,r2 encoding");
}

static void test_include_directive(void) {
    /* Write a tiny assembly file to /tmp/two_inc_test.asm, then .include it */
    FILE *f = fopen("/tmp/two_inc_test.asm", "w");
    if (!f) { ASSERT(0, ".include test: could not create temp file"); return; }
    fprintf(f, "MOVI r0, 42\n");
    fclose(f);

    const char *src = ".include \"/tmp/two_inc_test.asm\"\nHALT\n";
    ASSERT(do_asm(src, buf, BUFSZ, &blen) == 0, ".include: no errors");
    ASSERT(blen >= 8,                            ".include: at least 2 instructions");
    /* First instruction: MOVI r0, 42 → (0x0F<<24)|(0<<20)|42 = 0x0F00002A */
    ASSERT(read_word(buf,0) == 0x0F00002A,       ".include: MOVI r0,42 assembled");
    /* Second instruction: HALT → 0x12000000 */
    ASSERT(read_word(buf,1) == 0x12000000,       ".include: HALT assembled after include");
}

/* ─────────────────────────────────────────────────────────────────────────── */
/* Main                                                                        */
/* ─────────────────────────────────────────────────────────────────────────── */
int main(void) {
    printf("Running assembler tests...\n");

    test_r_type();
    test_r2_type();
    test_shift();
    test_movi();
    test_movi32();
    test_i_type();
    test_mem();
    test_nooperand();
    test_branch();
    test_jump();
    test_reg_aliases();
    test_directives();
    test_labels();
    test_errors();
    test_comments();
    test_case_insensitive();
    test_extended_r3();
    test_multi_instr();
    test_fibonacci_program();
    test_expressions();
    /* New instructions */
    test_ba();
    test_ba_backward();
    test_cmp_encoding();
    test_cmpi_encoding();
    test_cmpi_large_encoding();
    test_cmp_in_loop();
    /* RET / BR pseudo-instructions */
    test_ret_encoding();
    test_br_encoding();
    test_ret_in_subroutine();
    /* ADDC / SUBC / PUSH / POP / .align */
    test_addc_subc_encoding();
    test_push_pop_encoding();
    test_align_directive();
    /* BGT / BLE / CALLR / CALL / NOP */
    test_bgt_ble_encoding();
    test_callr_encoding();
    test_call_encoding();
    test_nop_encoding();
    /* Indexed memory */
    test_indexed_mem_encoding();
    /* .equ / BGTU / BLEU / LSLR / LSRR / ASRR */
    test_equ_directive();
    test_bgtu_bleu_encoding();
    test_lslr_lsrr_asrr_encoding();
    /* LUI / ROLR / RORR / ROLI / RORI */
    test_lui_encoding();
    test_rol_ror_encoding();

    test_cas_encoding();
    test_include_directive();

    printf("\nResults: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf(", %d FAILED\n", tests_failed);
        return 1;
    }
    printf(" — all OK\n");
    return 0;
}
