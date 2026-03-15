/* CPUTwo emulator unit tests — C99
 * Compile: cc -std=c99 -Wall -o test_emulator test_emulator.c
 * Run:     ./test_emulator
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── Pull in the emulator internals ────────────────────────────────────── */
/* We replicate the types/helpers we need; the actual build links test_emulator
   against a separate translation unit.  Here we just #include the source and
   override main so the whole thing is self-contained in one compilation unit. */

/* Stub out main before including emulator.c */
#define main emulator_main_stub
#include "emulator.c"
#undef main

/* ── Test harness ─────────────────────────────────────────────────────── */
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { tests_failed++; fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); } \
} while(0)

/* Reset CPU and clear memory, then write a small program starting at 0 */
static void setup(CPU *cpu) {
    memset(mem, 0, MEM_SIZE);
    cpu_reset(cpu);
}

/* Encode instruction formats */
static uint32_t enc_r(uint8_t op, uint8_t rd, uint8_t rs1, uint8_t rs2, uint8_t shift) {
    return ((uint32_t)op<<24)|((uint32_t)rd<<20)|((uint32_t)rs1<<16)|((uint32_t)rs2<<12)|((uint32_t)shift<<7);
}
static uint32_t enc_i(uint8_t op, uint8_t rd, uint8_t rs1, uint16_t imm) {
    return ((uint32_t)op<<24)|((uint32_t)rd<<20)|((uint32_t)rs1<<16)|(imm);
}
static uint32_t enc_b(uint8_t cond, int32_t offset) {
    return ((uint32_t)0x0D<<24)|((uint32_t)cond<<20)|((uint32_t)(offset & 0xFFFFF));
}
static uint32_t enc_j(uint8_t rd, int32_t offset) {
    return ((uint32_t)0x0E<<24)|((uint32_t)rd<<20)|((uint32_t)(offset & 0xFFFFF));
}
static uint32_t enc_halt(void) { return (uint32_t)0x12<<24; }
static uint32_t enc_syscall(void) { return (uint32_t)0x10<<24; }
static uint32_t enc_sysret(void)  { return (uint32_t)0x11<<24; }

static void write_prog(uint32_t base, const uint32_t *instrs, int n) {
    for (int i = 0; i < n; i++)
        mem_write32(base + (uint32_t)(i*4), instrs[i]);
}

/* ─────────────────────────────────────────────────────────────────────── */
/* STEP 1-2: CPU state and memory helpers                                  */
/* ─────────────────────────────────────────────────────────────────────── */
static void test_cpu_init(void) {
    CPU cpu; setup(&cpu);
    ASSERT(cpu.r[15] == 0,         "PC starts at 0");
    ASSERT(cpu.status == 0x01,     "STATUS = supervisor at reset");
    ASSERT(cpu.flags  == 0,        "flags = 0 at reset");
    ASSERT(cpu.halted == 0,        "not halted at reset");
}

static void test_memory_rw(void) {
    memset(mem, 0, 16);
    mem_write32(0, 0xDEADBEEF);
    ASSERT(mem_read32(0) == 0xDEADBEEF, "mem_write32/read32 round-trip");
    ASSERT(mem[0]==0xDE && mem[1]==0xAD && mem[2]==0xBE && mem[3]==0xEF, "big-endian byte order");
    mem_write16(4, 0x1234);
    ASSERT(mem_read16(4) == 0x1234, "mem_write16/read16");
    mem_write8(8, 0xAB);
    ASSERT(mem_read8(8) == 0xAB, "mem_write8/read8");
}

/* ─────────────────────────────────────────────────────────────────────── */
/* STEP 3: Exception dispatch                                              */
/* ─────────────────────────────────────────────────────────────────────── */
static void test_exception_dispatch(void) {
    CPU cpu; setup(&cpu);
    cpu.status = 0x00; /* user mode — allows exception dispatch */
    /* Put a handler at address 0x100; EVEC points to table at 0x200 */
    cpu.evec = 0x200;
    /* handler for cause 1 at evec + 1*4 = 0x204 */
    mem_write32(0x204, 0x100);
    cpu.r[15]      = 0x50;
    cpu.faulting_pc = 0x50; /* main loop sets faulting_pc before raise_exception */
    cpu.flags = 0xF0000000;
    raise_exception(&cpu, 1);
    ASSERT(cpu.epc    == 0x50,         "EPC saved");
    ASSERT(cpu.eflags == 0xF0000000,   "EFLAGS saved");
    ASSERT(cpu.cause  == 1,            "CAUSE set");
    ASSERT(cpu.status == 0x01,         "STATUS = supervisor");
    ASSERT(cpu.r[15]  == 0x100,        "PC = handler address");
}

static void test_double_fault(void) {
    CPU cpu; setup(&cpu);
    cpu.status = 0x01; /* already supervisor */
    cpu.r[15]  = 0xABCD;
    raise_exception(&cpu, 2);
    ASSERT(cpu.halted == 1,   "double fault halts");
    ASSERT(cpu.r[15]  == 0xABCD, "PC unchanged on double fault");
}

/* ─────────────────────────────────────────────────────────────────────── */
/* STEP 4: MMIO supervisor register access                                 */
/* ─────────────────────────────────────────────────────────────────────── */
static void test_sv_regs(void) {
    CPU cpu; setup(&cpu);
    /* supervisor mode — should work */
    cpu.epc = 0x1234; cpu.evec = 0x5000; cpu.cause = 3;
    ASSERT(mmio_read(&cpu, 0x03FFF000) == 0x1234, "SV read EPC");
    ASSERT(mmio_read(&cpu, 0x03FFF008) == 0x5000, "SV read EVEC");
    ASSERT(mmio_read(&cpu, 0x03FFF00C) == 3,      "SV read CAUSE");
    mmio_write(&cpu, 0x03FFF000, 0xABCD);
    ASSERT(cpu.epc == 0xABCD, "SV write EPC");
    mmio_write(&cpu, 0x03FFF00C, 0x99); /* cause read-only */
    ASSERT(cpu.cause == 3, "CAUSE write ignored");
}

static void test_sv_regs_user_fault(void) {
    CPU cpu; setup(&cpu);
    cpu.status = 0x00; /* user mode */
    cpu.evec   = 0x400;
    mem_write32(0x408, 0x800); /* handler for cause 2 */
    mmio_read(&cpu, 0x03FFF000);
    ASSERT(cpu.cause == 0x02, "User-mode SV read raises bus error");
}

/* ─────────────────────────────────────────────────────────────────────── */
/* STEPS 6-7: R-type ALU + flags                                           */
/* ─────────────────────────────────────────────────────────────────────── */
static void run_to_halt(CPU *cpu) {
    cpu_run(cpu, 0);
}

static void test_alu_add(void) {
    CPU cpu; setup(&cpu);
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 5),           /* MOVI r0, 5 */
        enc_i(0x0F, 1, 0, 3),           /* MOVI r1, 3 */
        enc_r(0x00, 2, 0, 1, 0),        /* ADD  r2 = r0+r1 */
        enc_halt()
    };
    write_prog(0, prog, 4);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 8, "ADD r0+r1=8");
    ASSERT(!(cpu.flags & FLAG_Z), "ADD: Z not set");
    ASSERT(!(cpu.flags & FLAG_N), "ADD: N not set");
}

static void test_alu_sub_flags(void) {
    CPU cpu; setup(&cpu);
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 5),
        enc_i(0x0F, 1, 0, 5),
        enc_r(0x01, 2, 0, 1, 0),   /* SUB r2=5-5=0 */
        enc_halt()
    };
    write_prog(0, prog, 4);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 0, "SUB result=0");
    ASSERT(cpu.flags & FLAG_Z, "SUB: Z set when result=0");
}

static void test_alu_and_or_xor_not(void) {
    CPU cpu; setup(&cpu);
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0xFF),
        enc_i(0x0F, 1, 0, 0x0F),
        enc_r(0x02, 2, 0, 1, 0),   /* AND 0xFF & 0x0F = 0x0F */
        enc_r(0x03, 3, 0, 1, 0),   /* OR  0xFF | 0x0F = 0xFF */
        enc_r(0x04, 4, 0, 1, 0),   /* XOR 0xFF ^ 0x0F = 0xF0 */
        enc_r(0x05, 5, 1, 0, 0),   /* NOT ~r1 = 0xFFFFFFF0 */
        enc_halt()
    };
    write_prog(0, prog, 7);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 0x0F,       "AND");
    ASSERT(cpu.r[3] == 0xFF,       "OR");
    ASSERT(cpu.r[4] == 0xF0,       "XOR");
    ASSERT(cpu.r[5] == 0xFFFFFFF0, "NOT");
}

static void test_alu_shifts(void) {
    CPU cpu; setup(&cpu);
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 1),
        enc_r(0x06, 1, 0, 0, 4),          /* LSL r0<<4 = 16 */
        enc_r(0x07, 2, 1, 0, 2),          /* LSR 16>>2 = 4  */
        enc_i(0x0F, 3, 0, 0x8000),        /* MOVI r3 = 0x8000 */
        enc_i(0x13, 3, 0, 0x8000),        /* MOVHI r3 = 0x80008000 */
        enc_r(0x08, 4, 3, 0, 1),          /* ASR r3>>1 => 0xC0004000 */
        enc_halt()
    };
    write_prog(0, prog, 7);
    run_to_halt(&cpu);
    ASSERT(cpu.r[1] == 16,         "LSL 1<<4=16");
    ASSERT(cpu.r[2] == 4,          "LSR 16>>2=4");
    ASSERT(cpu.r[4] == 0xC0004000, "ASR sign extends");
}

static void test_mov(void) {
    CPU cpu; setup(&cpu);
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 42),
        enc_r(0x27, 1, 0, 0, 0),  /* MOV r1 = r0 */
        enc_halt()
    };
    write_prog(0, prog, 3);
    run_to_halt(&cpu);
    ASSERT(cpu.r[1] == 42, "MOV r1=r0=42");
}

/* ─────────────────────────────────────────────────────────────────────── */
/* STEP 8: Immediate ALU                                                   */
/* ─────────────────────────────────────────────────────────────────────── */
static void test_imm_alu(void) {
    CPU cpu; setup(&cpu);
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 100),
        enc_i(0x14, 1, 0, 50),            /* ADDI r1 = 100+50 */
        enc_i(0x15, 2, 0, 25),            /* SUBI r2 = 100-25 */
        enc_i(0x16, 3, 0, 0x0F),          /* ANDI r3 = 100 & 0x0F = 4 */
        enc_i(0x17, 4, 0, 0x100),         /* ORI  r4 = 100 | 0x100 = 0x164 */
        enc_i(0x18, 5, 0, 0xFF),          /* XORI r5 = 100 ^ 0xFF = 0x9B */
        enc_halt()
    };
    write_prog(0, prog, 7);
    run_to_halt(&cpu);
    ASSERT(cpu.r[1] == 150,   "ADDI");
    ASSERT(cpu.r[2] == 75,    "SUBI");
    ASSERT(cpu.r[3] == 4,     "ANDI");
    ASSERT(cpu.r[4] == 0x164, "ORI");
    ASSERT(cpu.r[5] == 0x9B,  "XORI");
}

static void test_imm_shifts(void) {
    CPU cpu; setup(&cpu);
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 1),
        enc_i(0x19, 1, 0, 8),         /* LSLI r1 = 1<<8 = 256 */
        enc_i(0x1A, 2, 1, 4),         /* LSRI r2 = 256>>4 = 16 */
        enc_i(0x0F, 3, 0, 0x8000),
        enc_i(0x13, 3, 0, 0x8000),    /* r3 = 0x80008000 */
        enc_i(0x1B, 4, 3, 1),         /* ASRI r4 = r3>>1 = 0xC0004000 */
        enc_halt()
    };
    write_prog(0, prog, 7);
    run_to_halt(&cpu);
    ASSERT(cpu.r[1] == 256,        "LSLI");
    ASSERT(cpu.r[2] == 16,         "LSRI");
    ASSERT(cpu.r[4] == 0xC0004000, "ASRI sign extends");
}

/* ─────────────────────────────────────────────────────────────────────── */
/* STEP 9: MOVI + MOVHI                                                    */
/* ─────────────────────────────────────────────────────────────────────── */
static void test_movi_movhi(void) {
    CPU cpu; setup(&cpu);
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0xBEEF),        /* MOVI r0 = 0xBEEF */
        enc_i(0x13, 0, 0, 0xDEAD),        /* MOVHI r0 = 0xDEADBEEF */
        enc_halt()
    };
    write_prog(0, prog, 3);
    run_to_halt(&cpu);
    ASSERT(cpu.r[0] == 0xDEADBEEF, "MOVI+MOVHI loads 0xDEADBEEF");
}

/* ─────────────────────────────────────────────────────────────────────── */
/* STEP 10: Multiply / divide                                              */
/* ─────────────────────────────────────────────────────────────────────── */
static void test_mul_div(void) {
    CPU cpu; setup(&cpu);
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 7),
        enc_i(0x0F, 1, 0, 6),
        enc_r(0x09, 2, 0, 1, 0),   /* MUL 7*6=42 */
        enc_r(0x0A, 3, 2, 0, 0),   /* DIV 42/7=6 */
        enc_r(0x25, 4, 2, 0, 0),   /* MOD 42%7=0 */
        enc_halt()
    };
    write_prog(0, prog, 6);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 42, "MUL 7*6=42");
    ASSERT(cpu.r[3] == 6,  "DIV 42/7=6");
    ASSERT(cpu.r[4] == 0,  "MOD 42%7=0");
}

static void test_mulh(void) {
    CPU cpu; setup(&cpu);
    /* 0x80000000 * 0x80000000 signed = -2^31 * -2^31 = 2^62
       high 32 bits = 2^62 >> 32 = 2^30 = 0x40000000 */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0),
        enc_i(0x13, 0, 0, 0x8000),      /* r0 = 0x80000000 */
        enc_r(0x22, 1, 0, 0, 0),        /* MULH signed high */
        enc_r(0x23, 2, 0, 0, 0),        /* MULHU unsigned high */
        enc_halt()
    };
    write_prog(0, prog, 5);
    run_to_halt(&cpu);
    ASSERT(cpu.r[1] == 0x40000000, "MULH signed high bits");
    /* unsigned: 0x80000000 * 0x80000000 = 0x4000000000000000, high = 0x40000000 */
    ASSERT(cpu.r[2] == 0x40000000, "MULHU unsigned high bits");
}

static void test_div_by_zero(void) {
    CPU cpu; setup(&cpu);
    cpu.evec = 0x400;
    mem_write32(0x410, 0x800);   /* handler for cause 4 at evec+4*4 */
    mem_write32(0x800, enc_halt());
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 10),
        enc_i(0x0F, 1, 0, 0),    /* r1 = 0 */
        enc_r(0x0A, 2, 0, 1, 0), /* DIV by zero -> exception */
        enc_halt()
    };
    write_prog(0, prog, 4);
    cpu.status = 0x00; /* user mode so exception dispatches */
    run_to_halt(&cpu);
    ASSERT(cpu.cause == 0x04, "DIV by zero raises cause 4");
}

/* ─────────────────────────────────────────────────────────────────────── */
/* STEPS 11-12: Load / store                                               */
/* ─────────────────────────────────────────────────────────────────────── */
static void test_load_store(void) {
    CPU cpu; setup(&cpu);
    /* Write known bytes at 0x1000 */
    mem[0x1000]=0xDE; mem[0x1001]=0xAD; mem[0x1002]=0xBE; mem[0x1003]=0xEF;
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0),
        enc_i(0x13, 0, 0, 0x1000>>16),  /* r0 = 0x10000000? */
        /* Actually let's use MOVI for small address */
        enc_halt()
    };
    /* Direct test via cpu_read helpers — simpler */
    CPU c2; setup(&c2);
    mem_write32(0x1000, 0x12345678);
    mem_write16(0x1004, 0xABCD);
    mem_write8 (0x1006, 0xEF);

    uint32_t w  = cpu_read32(&c2, 0x1000);
    uint16_t h  = cpu_read16(&c2, 0x1004);
    uint8_t  b  = cpu_read8 (&c2, 0x1006);
    ASSERT(w == 0x12345678, "LW round-trip");
    ASSERT(h == 0xABCD,     "LH/LHU raw");
    ASSERT(b == 0xEF,       "LB/LBU raw");

    /* sign extension */
    mem_write16(0x1008, 0x8001);
    int32_t sh = (int32_t)(int16_t)cpu_read16(&c2, 0x1008);
    ASSERT(sh == (int32_t)0xFFFF8001, "LH sign extends");
    mem_write8(0x100A, 0x80);
    int32_t sb = (int32_t)(int8_t)cpu_read8(&c2, 0x100A);
    ASSERT(sb == (int32_t)0xFFFFFF80, "LB sign extends");
    (void)prog;
}

static void test_load_store_via_cpu(void) {
    CPU cpu; setup(&cpu);
    /* SW + LW via program */
    /* r0 = 0x2000 (load address) */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0x2000),           /* MOVI r0=0x2000 */
        enc_i(0x0F, 1, 0, 0xBEEF),           /* MOVI r1=0xBEEF */
        enc_i(0x13, 1, 0, 0xDEAD),           /* MOVHI r1=0xDEADBEEF */
        enc_i(0x0C, 1, 0, 0),                /* SW r1,[r0+0] */
        enc_i(0x0B, 2, 0, 0),                /* LW r2,[r0+0] */
        enc_halt()
    };
    write_prog(0, prog, 6);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 0xDEADBEEF, "SW+LW round-trip");
}

static void test_load_widths(void) {
    CPU cpu; setup(&cpu);
    /* Put 0xDEADBEEF at 0x3000 */
    mem_write32(0x3000, 0xDEADBEEF);
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0x3000),       /* r0 = 0x3000 */
        enc_i(0x1D, 1, 0, 0),            /* LHU r1 = 0xDEAD */
        enc_i(0x1C, 2, 0, 0),            /* LH  r2 = sign_ext(0xDEAD) = 0xFFFFDEAD */
        enc_i(0x1F, 3, 0, 0),            /* LBU r3 = 0xDE */
        enc_i(0x1E, 4, 0, 0),            /* LB  r4 = sign_ext(0xDE) = 0xFFFFFFDE */
        enc_halt()
    };
    write_prog(0, prog, 6);
    run_to_halt(&cpu);
    ASSERT(cpu.r[1] == 0xDEAD,     "LHU zero-extends");
    ASSERT(cpu.r[2] == 0xFFFFDEAD, "LH sign-extends");
    ASSERT(cpu.r[3] == 0xDE,       "LBU zero-extends");
    ASSERT(cpu.r[4] == 0xFFFFFFDE, "LB sign-extends");
}

static void test_misaligned(void) {
    CPU cpu; setup(&cpu);
    cpu.status = 0x00; /* user mode */
    cpu.evec   = 0x200;
    mem_write32(0x204, 0x800);   /* handler for cause 1 */
    mem_write32(0x800, enc_halt());
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0x1001),   /* r0 = 0x1001 (odd) */
        enc_i(0x0B, 1, 0, 0),        /* LW r1,[r0+0] — misaligned */
        enc_halt()
    };
    write_prog(0, prog, 3);
    run_to_halt(&cpu);
    ASSERT(cpu.cause == 0x01, "Misaligned LW raises cause 1");
}

/* ─────────────────────────────────────────────────────────────────────── */
/* STEP 13: Branches                                                       */
/* ─────────────────────────────────────────────────────────────────────── */
static void test_branch_beq_taken(void) {
    CPU cpu; setup(&cpu);
    /* SUB r0,r0,r0 sets Z. BEQ +8 skips r1=99, so r1 stays 0 */
    uint32_t prog[] = {
        enc_r(0x01, 0, 0, 0, 0),     /* SUB r0=0-0=0, Z=1 */
        enc_b(0, 8),                  /* BEQ +8 (skip next instr) */
        enc_i(0x0F, 1, 0, 99),        /* MOVI r1=99 (should be skipped) */
        enc_halt()
    };
    write_prog(0, prog, 4);
    run_to_halt(&cpu);
    ASSERT(cpu.r[1] == 0, "BEQ taken skips instruction");
}

static void test_branch_beq_not_taken(void) {
    CPU cpu; setup(&cpu);
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 1),         /* r0=1, Z=0 */
        enc_b(0, 8),                   /* BEQ +8 — NOT taken */
        enc_i(0x0F, 1, 0, 42),         /* r1=42 (should execute) */
        enc_halt()
    };
    write_prog(0, prog, 4);
    run_to_halt(&cpu);
    ASSERT(cpu.r[1] == 42, "BEQ not taken falls through");
}

static void test_branch_bne(void) {
    CPU cpu; setup(&cpu);
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 5),
        enc_i(0x0F, 1, 0, 3),
        enc_r(0x01, 2, 0, 1, 0),      /* SUB 5-3=2, Z=0 */
        enc_b(1, 8),                   /* BNE +8 taken */
        enc_i(0x0F, 3, 0, 99),         /* skipped */
        enc_halt()
    };
    write_prog(0, prog, 6);
    run_to_halt(&cpu);
    ASSERT(cpu.r[3] == 0, "BNE taken skips instruction");
}

static void test_branch_blt_bge(void) {
    CPU cpu; setup(&cpu);
    /* r0=-1 (0xFFFFFFFF), so N=1 after MOVI? No, MOVI is zero-extend.
       Use SUB: 0 - 1 = 0xFFFFFFFF, N=1 */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0),
        enc_i(0x0F, 1, 0, 1),
        enc_r(0x01, 2, 0, 1, 0),   /* SUB 0-1, N=1 */
        enc_b(2, 8),                /* BLT +8 taken */
        enc_i(0x0F, 3, 0, 77),      /* skipped */
        enc_halt()
    };
    write_prog(0, prog, 6);
    run_to_halt(&cpu);
    ASSERT(cpu.r[3] == 0, "BLT taken when N=1");
}

static void test_branch_bltu(void) {
    CPU cpu; setup(&cpu);
    /* unsigned: 1 < 5, so SUB 1-5 has borrow (C=1) */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 1),
        enc_i(0x0F, 1, 0, 5),
        enc_r(0x01, 2, 0, 1, 0),   /* SUB 1-5 unsigned borrow C=1 */
        enc_b(4, 8),                /* BLTU +8 taken */
        enc_i(0x0F, 3, 0, 55),
        enc_halt()
    };
    write_prog(0, prog, 6);
    run_to_halt(&cpu);
    ASSERT(cpu.r[3] == 0, "BLTU taken when C=1");
}

static void test_branch_illegal_cond(void) {
    CPU cpu; setup(&cpu);
    cpu.status = 0x00;
    cpu.evec   = 0x200;
    mem_write32(0x200, 0x800); /* handler cause 0 */
    mem_write32(0x800, enc_halt());
    uint32_t prog[] = {
        enc_b(11, 8),  /* illegal cond 11 (>=11 is reserved) */
        enc_halt()
    };
    write_prog(0, prog, 2);
    run_to_halt(&cpu);
    ASSERT(cpu.cause == 0x00, "Illegal branch cond raises cause 0");
}

/* ─────────────────────────────────────────────────────────────────────── */
/* STEP 14: JMP with link                                                  */
/* ─────────────────────────────────────────────────────────────────────── */
static void test_jmp(void) {
    CPU cpu; setup(&cpu);
    /* JMP lr, +8 => lr=4 (pc after JMP), pc=0+8=8 */
    uint32_t prog[] = {
        enc_j(14, 8),            /* JMP lr, +8; lr=4, pc=8 */
        enc_i(0x0F, 1, 0, 99),   /* skipped (addr 4) */
        enc_halt()               /* addr 8 */
    };
    write_prog(0, prog, 3);
    run_to_halt(&cpu);
    ASSERT(cpu.r[14] == 4, "JMP: lr = PC+4");
    ASSERT(cpu.r[1]  == 0, "JMP: skipped instruction not executed");
}

/* ─────────────────────────────────────────────────────────────────────── */
/* STEP 15-16: SYSCALL / SYSRET                                            */
/* ─────────────────────────────────────────────────────────────────────── */
static void test_syscall_sysret(void) {
    CPU cpu; setup(&cpu);
    /* Layout:
       0x00: SYSCALL       — triggers, saves EPC=4
       0x04: HALT           — reached by SYSRET
       handler at 0x100:
       0x100: SYSRET        — returns
    */
    cpu.status = 0x00;        /* user mode */
    cpu.evec   = 0x200;
    mem_write32(0x200 + 3*4, 0x100); /* handler for cause 3 */

    uint32_t prog[] = {
        enc_syscall(),   /* addr 0 */
        enc_halt()       /* addr 4 — return target */
    };
    write_prog(0, prog, 2);

    uint32_t handler[] = {
        enc_sysret()
    };
    write_prog(0x100, handler, 1);

    run_to_halt(&cpu);
    ASSERT(cpu.halted == 1,    "SYSCALL+SYSRET: clean halt");
    ASSERT((cpu.status & 1) == 0, "SYSRET returns to user mode");
}

static void test_syscall_saves_epc(void) {
    CPU cpu; setup(&cpu);
    cpu.status = 0x00;
    cpu.evec   = 0x200;
    /* handler just halts */
    mem_write32(0x200 + 3*4, 0x100);
    mem_write32(0x100, enc_halt());
    mem_write32(0, enc_syscall()); /* at address 0 */
    run_to_halt(&cpu);
    ASSERT(cpu.epc == 4,     "SYSCALL saves EPC = instruction+4");
    ASSERT(cpu.halted == 1,  "halted cleanly");
}

static void test_sysret_in_user_mode(void) {
    CPU cpu; setup(&cpu);
    cpu.status = 0x00; /* user mode */
    cpu.evec   = 0x200;
    mem_write32(0x200, 0x100); /* handler cause 0 */
    mem_write32(0x100, enc_halt());
    mem_write32(0, enc_sysret());
    run_to_halt(&cpu);
    ASSERT(cpu.cause == 0x00, "SYSRET in user mode raises illegal instr");
}

/* ─────────────────────────────────────────────────────────────────────── */
/* STEP 17-18: HALT and illegal instruction                                */
/* ─────────────────────────────────────────────────────────────────────── */
static void test_halt(void) {
    CPU cpu; setup(&cpu);
    mem_write32(0, enc_halt());
    run_to_halt(&cpu);
    ASSERT(cpu.halted == 1, "HALT stops execution");
}

static void test_illegal_instr(void) {
    CPU cpu; setup(&cpu);
    cpu.status = 0x00;
    cpu.evec   = 0x200;
    mem_write32(0x200, 0x800);  /* cause 0 handler */
    mem_write32(0x800, enc_halt());
    mem_write32(0, 0xFF000000); /* unknown opcode 0xFF */
    run_to_halt(&cpu);
    ASSERT(cpu.cause == 0x00, "Unknown opcode raises cause 0");
}

/* ─────────────────────────────────────────────────────────────────────── */
/* STEP 20: Timer interrupt                                                */
/* ─────────────────────────────────────────────────────────────────────── */
static void test_timer_interrupt(void) {
    CPU cpu; setup(&cpu);

    /* handler just halts */
    mem_write32(0x100, enc_halt());

    cpu.evec = 0x200;
    mem_write32(0x200 + 6*4, 0x100); /* handler for cause 6 */

    /* Set timer period=3, enable timer, enable timer irq, set IC mask bit0, set STATUS.IE */
    cpu.timer_period  = 3;
    cpu.timer_count   = 3;
    cpu.timer_control = 0x03; /* enable + irq_en */
    cpu.ic_mask       = 0x01; /* timer enabled in IC */
    cpu.status        = 0x02; /* user mode, IE=1 */

    /* Program: 3 NOPs (ADD r0,r0,r0), then HALT — timer should fire before halt */
    uint32_t prog[] = {
        enc_r(0x00, 0, 0, 0, 0),  /* NOP (ADD r0,r0,r0) tick 1 -> count=2 */
        enc_r(0x00, 0, 0, 0, 0),  /* tick 2 -> count=1 */
        enc_r(0x00, 0, 0, 0, 0),  /* tick 3 -> count=0 -> latch pending */
        enc_halt()
    };
    write_prog(0, prog, 4);

    run_to_halt(&cpu);
    ASSERT(cpu.cause == 0x05 || cpu.cause == 0x06, "Timer fired or halted");
    /* The interrupt should have fired by tick 3 */
}

/* ─────────────────────────────────────────────────────────────────────── */
/* STEP 19: Bus error on out-of-range address                              */
/* ─────────────────────────────────────────────────────────────────────── */
static void test_bus_error(void) {
    CPU cpu; setup(&cpu);
    cpu.status = 0x00;
    cpu.evec   = 0x200;
    mem_write32(0x208, 0x800);   /* handler for cause 2 */
    mem_write32(0x800, enc_halt());
    /* Try to load from 0x04000000 — out of range */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0),
        enc_i(0x13, 0, 0, 0x0400),  /* r0 = 0x04000000 */
        enc_i(0x0B, 1, 0, 0),       /* LW r1,[r0+0] -> bus error */
        enc_halt()
    };
    write_prog(0, prog, 4);
    run_to_halt(&cpu);
    ASSERT(cpu.cause == 0x02, "Out-of-range address raises bus error");
}

/* ─────────────────────────────────────────────────────────────────────── */
/* BA — unconditional branch                                               */
/* ─────────────────────────────────────────────────────────────────────── */
static void test_ba_taken(void) {
    CPU cpu; setup(&cpu);
    /* BA +8 jumps over r1=99 → r1 stays 0 regardless of flags */
    uint32_t prog[] = {
        (0x0Du<<24)|(6u<<20)|(8u),  /* BA +8 (cond=6, offset=8) */
        enc_i(0x0F, 1, 0, 99),      /* skipped */
        enc_halt()
    };
    write_prog(0, prog, 3);
    run_to_halt(&cpu);
    ASSERT(cpu.r[1] == 0, "BA always taken, skips instruction");
}

static void test_ba_with_zero_flags(void) {
    CPU cpu; setup(&cpu);
    /* Even when Z=1 (BEQ would branch), BA still branches because it ignores flags */
    uint32_t prog[] = {
        enc_r(0x01, 0, 0, 0, 0),    /* SUB r0,r0,r0 → Z=1, N=0 */
        (0x0Du<<24)|(6u<<20)|(8u),  /* BA +8 — must branch even though Z=1 */
        enc_i(0x0F, 1, 0, 99),      /* skipped */
        enc_halt()
    };
    write_prog(0, prog, 4);
    run_to_halt(&cpu);
    ASSERT(cpu.r[1] == 0, "BA taken regardless of Z=1");
}

/* ─────────────────────────────────────────────────────────────────────── */
/* CMP / CMPI — flags set, no register written                            */
/* ─────────────────────────────────────────────────────────────────────── */
static void test_cmp_eq(void) {
    CPU cpu; setup(&cpu);
    uint32_t prog[] = {
        enc_i(0x0F, 1, 0, 42),
        enc_i(0x0F, 2, 0, 42),
        /* CMP r1, r2 — opcode 0x28, rd=0, rs1=1, rs2=2 */
        (0x28u<<24)|(1u<<16)|(2u<<12),
        enc_halt()
    };
    write_prog(0, prog, 4);
    run_to_halt(&cpu);
    ASSERT(cpu.flags & FLAG_Z,  "CMP equal sets Z");
    ASSERT(!(cpu.flags & FLAG_N), "CMP equal clears N");
    ASSERT(cpu.r[1] == 42, "CMP does not write r1");
    ASSERT(cpu.r[2] == 42, "CMP does not write r2");
}

static void test_cmp_lt(void) {
    CPU cpu; setup(&cpu);
    /* CMP 3, 5 → 3-5 = -2, N=1, V=0 → BLT should take (N^V=1) */
    uint32_t prog[] = {
        enc_i(0x0F, 1, 0, 3),
        enc_i(0x0F, 2, 0, 5),
        (0x28u<<24)|(1u<<16)|(2u<<12),  /* CMP r1, r2 */
        enc_halt()
    };
    write_prog(0, prog, 4);
    run_to_halt(&cpu);
    ASSERT(cpu.flags & FLAG_N,  "CMP 3<5 sets N");
    ASSERT(!(cpu.flags & FLAG_V), "CMP 3<5 no overflow → V=0");
    ASSERT(!(cpu.flags & FLAG_Z), "CMP 3<5 clears Z");
}

static void test_cmpi_eq(void) {
    CPU cpu; setup(&cpu);
    uint32_t prog[] = {
        enc_i(0x0F, 1, 0, 100),
        /* CMPI r1, 100 — opcode 0x29, rd=0, rs1=1, imm=100 */
        (0x29u<<24)|(1u<<16)|100u,
        enc_halt()
    };
    write_prog(0, prog, 3);
    run_to_halt(&cpu);
    ASSERT(cpu.flags & FLAG_Z,  "CMPI equal sets Z");
    ASSERT(cpu.r[1] == 100,     "CMPI does not write r1");
}

static void test_cmpi_large(void) {
    CPU cpu; setup(&cpu);
    /* CMPI r0, 65535 (0xFFFF): imm is zero-extended, so 0 - 65535 → borrow, N set */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0),
        (0x29u<<24)|(0u<<16)|0xFFFFu,   /* CMPI r0, 65535 */
        enc_halt()
    };
    write_prog(0, prog, 3);
    run_to_halt(&cpu);
    ASSERT(!(cpu.flags & FLAG_Z), "CMPI 0 vs 65535: not equal");
    ASSERT(cpu.flags & FLAG_N,    "CMPI 0 vs 65535: N set (0-65535 wraps negative)");
    ASSERT(cpu.flags & FLAG_C,    "CMPI 0 vs 65535: borrow set");

    /* CMPI r0, 50000 (> old signed max 32767): verify this now works */
    setup(&cpu);
    uint32_t prog2[] = {
        enc_i(0x0F, 0, 0, 50000 & 0xFFFF),
        (0x29u<<24)|(0u<<16)|(50000u),  /* CMPI r0, 50000 — equal */
        enc_halt()
    };
    write_prog(0, prog2, 3);
    run_to_halt(&cpu);
    ASSERT(cpu.flags & FLAG_Z, "CMPI r0==50000: Z set");
}

/* ─────────────────────────────────────────────────────────────────────── */
/* BLT / BGE — N^V correction                                             */
/* ─────────────────────────────────────────────────────────────────────── */
static void test_blt_with_overflow(void) {
    CPU cpu; setup(&cpu);
    /* 0x7FFFFFFF - 0x80000000 overflows: result = 0xFFFFFFFF, N=1, V=1
       True signed comparison: 0x7FFFFFFF > 0x80000000 (i.e., INT_MAX > INT_MIN)
       So BLT must NOT be taken (N^V = 1^1 = 0).
       Old behaviour (N only) would wrongly take the branch. */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0xFFFF),
        enc_i(0x13, 0, 0, 0x7FFF),   /* r0 = 0x7FFFFFFF */
        enc_i(0x0F, 1, 0, 0),
        enc_i(0x13, 1, 0, 0x8000),   /* r1 = 0x80000000 */
        enc_r(0x01, 2, 0, 1, 0),     /* SUB r2 = r0-r1; N=1, V=1 (overflow) */
        enc_b(2, 8),                  /* BLT +8 — must NOT be taken (N^V=0) */
        enc_i(0x0F, 3, 0, 1),        /* r3=1 — must execute */
        enc_halt()
    };
    write_prog(0, prog, 8);
    run_to_halt(&cpu);
    /* r2 = 0x7FFFFFFF - 0x80000000 = 0xFFFFFFFF; N=1, V=1 at that point.
       BLT should NOT be taken (N^V=0), so MOVI r3,1 executes → r3=1. */
    ASSERT(cpu.r[2] == 0xFFFFFFFF, "overflow sub: r2=0xFFFFFFFF");
    ASSERT(cpu.r[3] == 1, "BLT not taken when N=V (0x7FFFFFFF > 0x80000000 signed)");
}

static void test_bge_with_overflow(void) {
    CPU cpu; setup(&cpu);
    /* Same subtraction — BGE should be taken (N^V = 0 → N=V → taken) */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0xFFFF),
        enc_i(0x13, 0, 0, 0x7FFF),   /* r0 = 0x7FFFFFFF */
        enc_i(0x0F, 1, 0, 0),
        enc_i(0x13, 1, 0, 0x8000),   /* r1 = 0x80000000 */
        enc_r(0x01, 2, 0, 1, 0),     /* SUB r2 = r0-r1; N=1, V=1 */
        enc_b(3, 8),                  /* BGE +8 — must be taken (N=V) */
        enc_i(0x0F, 3, 0, 99),       /* skipped */
        enc_halt()
    };
    write_prog(0, prog, 8);
    run_to_halt(&cpu);
    ASSERT(cpu.r[3] == 0, "BGE taken when N=V (0x7FFFFFFF >= 0x80000000 signed)");
}

static void test_blt_normal(void) {
    CPU cpu; setup(&cpu);
    /* Normal case: -1 < 1, no overflow, BLT taken */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0xFFFF),
        enc_i(0x13, 0, 0, 0xFFFF),   /* r0 = 0xFFFFFFFF = -1 signed */
        enc_i(0x0F, 1, 0, 1),
        enc_r(0x01, 2, 0, 1, 0),     /* SUB r2 = -1 - 1 = -2; N=1, V=0 */
        enc_b(2, 8),                  /* BLT +8 — taken (N^V = 1) */
        enc_i(0x0F, 3, 0, 99),       /* skipped */
        enc_halt()
    };
    write_prog(0, prog, 7);
    run_to_halt(&cpu);
    ASSERT(cpu.r[3] == 0, "BLT taken: -1 < 1 (normal, no overflow)");
}

static void test_cmp_then_bge(void) {
    CPU cpu; setup(&cpu);
    /* Use CMP instead of SUB; verify flag-then-branch works end-to-end */
    uint32_t prog[] = {
        enc_i(0x0F, 1, 0, 10),
        enc_i(0x0F, 2, 0, 5),
        /* CMP r1, r2: 10 - 5 = 5, N=0, V=0 → BGE taken */
        (0x28u<<24)|(1u<<16)|(2u<<12),
        enc_b(3, 8),                  /* BGE +8 — taken (N=V=0) */
        enc_i(0x0F, 3, 0, 99),       /* skipped */
        enc_halt()
    };
    write_prog(0, prog, 6);
    run_to_halt(&cpu);
    ASSERT(cpu.r[3] == 0, "CMP+BGE: 10>=5 branch taken");
    ASSERT(cpu.r[1] == 10, "CMP left r1 intact");
}

/* ─────────────────────────────────────────────────────────────────────── */
/* ADDC / SUBC — add/subtract with carry                                   */
/* ─────────────────────────────────────────────────────────────────────── */
static uint32_t enc_addc(uint8_t rd, uint8_t rs1, uint8_t rs2) {
    return ((uint32_t)0x2B<<24)|((uint32_t)rd<<20)|((uint32_t)rs1<<16)|((uint32_t)rs2<<12);
}
static uint32_t enc_subc(uint8_t rd, uint8_t rs1, uint8_t rs2) {
    return ((uint32_t)0x2C<<24)|((uint32_t)rd<<20)|((uint32_t)rs1<<16)|((uint32_t)rs2<<12);
}

static void test_addc(void) {
    CPU cpu; setup(&cpu);
    /* 64-bit add: 0xFFFFFFFF + 0xFFFFFFFF
       Low:  ADD r4 = 0xFFFFFFFF+0xFFFFFFFF = 0xFFFFFFFE, C=1
       High: ADDC r5 = 0+0+1 = 1 */
    uint32_t prog[] = {
        enc_i(0x0F,0,0,0xFFFF), enc_i(0x13,0,0,0xFFFF), /* r0=0xFFFFFFFF */
        enc_i(0x0F,1,0,0xFFFF), enc_i(0x13,1,0,0xFFFF), /* r1=0xFFFFFFFF */
        enc_i(0x0F,2,0,0),                               /* r2=0 (high of r0) */
        enc_i(0x0F,3,0,0),                               /* r3=0 (high of r1) */
        enc_r(0x00,4,0,1,0),   /* ADD r4=0xFFFFFFFF+0xFFFFFFFF=0xFFFFFFFE, C=1 */
        enc_addc(5,2,3),       /* ADDC r5=0+0+C=1 */
        enc_halt()
    };
    write_prog(0, prog, 9);
    run_to_halt(&cpu);
    ASSERT(cpu.r[4] == 0xFFFFFFFE, "ADDC: low word 0xFFFFFFFF+0xFFFFFFFF");
    ASSERT(cpu.r[5] == 1,          "ADDC: high word carries in 1");
}

static void test_addc_no_carry(void) {
    CPU cpu; setup(&cpu);
    /* After an add with no carry, ADDC should not add extra 1 */
    uint32_t prog[] = {
        enc_i(0x0F,0,0,3),
        enc_i(0x0F,1,0,4),
        enc_r(0x00,2,0,1,0),  /* ADD 3+4=7, C=0 */
        enc_i(0x0F,3,0,10),
        enc_i(0x0F,4,0,20),
        enc_addc(5,3,4),      /* ADDC 10+20+0=30 */
        enc_halt()
    };
    write_prog(0, prog, 7);
    run_to_halt(&cpu);
    ASSERT(cpu.r[5] == 30, "ADDC with C=0: 10+20+0=30");
}

static void test_subc(void) {
    CPU cpu; setup(&cpu);
    /* 64-bit: 0x0000_0001_0000_0000 - 0x0000_0000_0000_0001 = 0x0000_0000_FFFF_FFFF
       Low:  SUB r4 = 0-1 = 0xFFFFFFFF, C=1 (borrow)
       High: SUBC r5 = 1-0-1 = 0 */
    uint32_t prog[] = {
        enc_i(0x0F,0,0,0),     /* r0=0  (lo of 0x100000000) */
        enc_i(0x0F,1,0,1),     /* r1=1  (hi of 0x100000000) */
        enc_i(0x0F,2,0,1),     /* r2=1  (lo of 0x1) */
        enc_i(0x0F,3,0,0),     /* r3=0  (hi of 0x1) */
        enc_r(0x01,4,0,2,0),   /* SUB r4=0-1=0xFFFFFFFF, C=1 */
        enc_subc(5,1,3),       /* SUBC r5=1-0-1=0 */
        enc_halt()
    };
    write_prog(0, prog, 7);
    run_to_halt(&cpu);
    ASSERT(cpu.r[4] == 0xFFFFFFFF, "SUBC: low word 0-1=0xFFFFFFFF");
    ASSERT(cpu.r[5] == 0,          "SUBC: high word 1-0-borrow=0");
}

/* ─────────────────────────────────────────────────────────────────────── */
/* CALLR — register indirect call                                          */
/* ─────────────────────────────────────────────────────────────────────── */
static uint32_t enc_callr(uint8_t rd, uint8_t rs1) {
    return ((uint32_t)0x2A<<24)|((uint32_t)rd<<20)|((uint32_t)rs1<<16);
}

static void test_callr(void) {
    CPU cpu; setup(&cpu);
    /* prog[0] @ 0x00: MOVI r5, 12    — target address = 12 */
    /* prog[1] @ 0x04: CALLR lr, r5   — lr = 8 (next instr), pc = 12 */
    /* prog[2] @ 0x08: MOVI r1, 99    — should be skipped */
    /* prog[3] @ 0x0C: HALT           — target */
    uint32_t prog[] = {
        enc_i(0x0F, 5, 0, 12),          /* MOVI r5, 12 */
        enc_callr(14, 5),               /* CALLR lr, r5 */
        enc_i(0x0F, 1, 0, 99),          /* MOVI r1, 99 — skipped */
        enc_halt()                      /* @ addr 12 */
    };
    write_prog(0, prog, 4);
    run_to_halt(&cpu);
    ASSERT(cpu.r[14] == 8,  "CALLR: lr = PC+4 (return addr)");
    ASSERT(cpu.r[1]  == 0,  "CALLR: skipped instruction not executed");
    ASSERT(cpu.r[5]  == 12, "CALLR: rs1 unchanged");
}

static void test_callr_r0_discards(void) {
    CPU cpu; setup(&cpu);
    /* CALLR r0, r5 — branch without saving return addr */
    uint32_t prog[] = {
        enc_i(0x0F, 5, 0, 12),          /* MOVI r5, 12 (address of HALT) */
        enc_callr(0, 5),                /* CALLR r0, r5 — jump to 12, discard ret addr */
        enc_i(0x0F, 1, 0, 99),          /* MOVI r1, 99 @ addr 8 — skipped */
        enc_halt()                      /* @ addr 12 */
    };
    write_prog(0, prog, 4);
    run_to_halt(&cpu);
    ASSERT(cpu.r[1] == 0, "CALLR r0: skipped instruction not executed");
}

/* ─────────────────────────────────────────────────────────────────────── */
/* BGT / BLE — signed greater-than and less-than-or-equal                 */
/* ─────────────────────────────────────────────────────────────────────── */
static void test_bgt_taken(void) {
    CPU cpu; setup(&cpu);
    /* 5 > 3: CMP 5,3 → 5-3=2, Z=0, N=0, V=0 → BGT taken (Z=0 && N=V) */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 5),
        enc_i(0x0F, 1, 0, 3),
        (0x28u<<24)|(0u<<16)|(1u<<12),  /* CMP r0, r1 */
        enc_b(7, 8),                    /* BGT +8 — taken */
        enc_i(0x0F, 2, 0, 99),          /* skipped */
        enc_halt()
    };
    write_prog(0, prog, 6);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 0, "BGT taken: 5 > 3");
}

static void test_bgt_not_taken_less(void) {
    CPU cpu; setup(&cpu);
    /* 3 > 5 is false: CMP 3,5 → N=1, V=0 → N≠V → BGT NOT taken */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 3),
        enc_i(0x0F, 1, 0, 5),
        (0x28u<<24)|(0u<<16)|(1u<<12),  /* CMP r0, r1 */
        enc_b(7, 8),                    /* BGT +8 — NOT taken */
        enc_i(0x0F, 2, 0, 42),          /* executed */
        enc_halt()
    };
    write_prog(0, prog, 6);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 42, "BGT not taken: 3 not > 5");
}

static void test_bgt_not_taken_equal(void) {
    CPU cpu; setup(&cpu);
    /* 5 == 5: CMP 5,5 → Z=1 → BGT NOT taken (needs Z=0) */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 5),
        enc_i(0x0F, 1, 0, 5),
        (0x28u<<24)|(0u<<16)|(1u<<12),  /* CMP r0, r1 */
        enc_b(7, 8),                    /* BGT +8 — NOT taken */
        enc_i(0x0F, 2, 0, 42),          /* executed */
        enc_halt()
    };
    write_prog(0, prog, 6);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 42, "BGT not taken: 5 not > 5 (equal)");
}

static void test_ble_taken_less(void) {
    CPU cpu; setup(&cpu);
    /* 3 <= 5: CMP 3,5 → N=1, V=0 → N≠V → BLE taken */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 3),
        enc_i(0x0F, 1, 0, 5),
        (0x28u<<24)|(0u<<16)|(1u<<12),  /* CMP r0, r1 */
        enc_b(8, 8),                    /* BLE +8 — taken */
        enc_i(0x0F, 2, 0, 99),          /* skipped */
        enc_halt()
    };
    write_prog(0, prog, 6);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 0, "BLE taken: 3 <= 5");
}

static void test_ble_taken_equal(void) {
    CPU cpu; setup(&cpu);
    /* 5 <= 5: CMP 5,5 → Z=1 → BLE taken */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 5),
        enc_i(0x0F, 1, 0, 5),
        (0x28u<<24)|(0u<<16)|(1u<<12),  /* CMP r0, r1 */
        enc_b(8, 8),                    /* BLE +8 — taken */
        enc_i(0x0F, 2, 0, 99),          /* skipped */
        enc_halt()
    };
    write_prog(0, prog, 6);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 0, "BLE taken: 5 <= 5 (equal)");
}

static void test_ble_not_taken(void) {
    CPU cpu; setup(&cpu);
    /* 5 <= 3 is false: CMP 5,3 → Z=0, N=0, V=0 → BLE NOT taken */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 5),
        enc_i(0x0F, 1, 0, 3),
        (0x28u<<24)|(0u<<16)|(1u<<12),  /* CMP r0, r1 */
        enc_b(8, 8),                    /* BLE +8 — NOT taken */
        enc_i(0x0F, 2, 0, 42),          /* executed */
        enc_halt()
    };
    write_prog(0, prog, 6);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 42, "BLE not taken: 5 not <= 3");
}

/* ─────────────────────────────────────────────────────────────────────── */
/* Indexed memory — LWX / LBX / LBUX / SWX / SBX / LHX / LHUX / SHX    */
/* ─────────────────────────────────────────────────────────────────────── */
static void test_lwx(void) {
    CPU cpu; setup(&cpu);
    mem_write32(0x200, 0xDEADBEEF);
    mem_write32(0x204, 0x12345678);
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0x200),          /* r0 = 0x200 (base) */
        enc_i(0x0F, 1, 0, 0),              /* r1 = 0 (index) */
        enc_r(0x30, 2, 0, 1, 0),           /* LWX r2, [r0+r1] → 0x200 */
        enc_i(0x0F, 1, 0, 4),              /* r1 = 4 */
        enc_r(0x30, 3, 0, 1, 0),           /* LWX r3, [r0+r1] → 0x204 */
        enc_halt()
    };
    write_prog(0, prog, 6);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 0xDEADBEEF, "LWX: base+0");
    ASSERT(cpu.r[3] == 0x12345678, "LWX: base+4");
}

static void test_swx(void) {
    CPU cpu; setup(&cpu);
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0x300),          /* r0 = 0x300 (base) */
        enc_i(0x0F, 1, 0, 8),              /* r1 = 8 (index) */
        enc_i(0x0F, 2, 0, 0xABCD),         /* r2 = 0xABCD */
        enc_r(0x33, 2, 0, 1, 0),           /* SWX r2, [r0+r1] → write to 0x308 */
        enc_halt()
    };
    write_prog(0, prog, 5);
    run_to_halt(&cpu);
    ASSERT(mem_read32(0x308) == 0xABCD, "SWX: store word at base+8");
    ASSERT(mem_read32(0x300) == 0,      "SWX: no write to base+0");
}

static void test_lbux(void) {
    CPU cpu; setup(&cpu);
    mem[0x400]   = 0x42;
    mem[0x400+3] = 0xFF;
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0x400),
        enc_i(0x0F, 1, 0, 0),
        enc_r(0x32, 2, 0, 1, 0),           /* LBUX r2, [r0+0] */
        enc_i(0x0F, 1, 0, 3),
        enc_r(0x32, 3, 0, 1, 0),           /* LBUX r3, [r0+3]: 0xFF zero-extended */
        enc_halt()
    };
    write_prog(0, prog, 6);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 0x42,         "LBUX: zero-extend 0x42");
    ASSERT(cpu.r[3] == 0xFF,         "LBUX: 0xFF zero-extended = 255, not -1");
}

static void test_lbx_signed(void) {
    CPU cpu; setup(&cpu);
    mem[0x500] = 0xFF;                     /* -1 as signed byte */
    mem[0x501] = 0x7F;                     /* +127 */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0x500),
        enc_i(0x0F, 1, 0, 0),
        enc_r(0x31, 2, 0, 1, 0),           /* LBX r2, [r0+0]: sign-extend 0xFF */
        enc_i(0x0F, 1, 0, 1),
        enc_r(0x31, 3, 0, 1, 0),           /* LBX r3, [r0+1]: sign-extend 0x7F */
        enc_halt()
    };
    write_prog(0, prog, 6);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 0xFFFFFFFF, "LBX: 0xFF sign-extended to -1");
    ASSERT(cpu.r[3] == 0x7F,       "LBX: 0x7F sign-extended stays 127");
}

static void test_sbx(void) {
    CPU cpu; setup(&cpu);
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0x600),
        enc_i(0x0F, 1, 0, 5),
        enc_i(0x0F, 2, 0, 0xAB),
        enc_r(0x34, 2, 0, 1, 0),           /* SBX r2, [r0+5] */
        enc_halt()
    };
    write_prog(0, prog, 5);
    run_to_halt(&cpu);
    ASSERT(mem[0x605] == 0xAB, "SBX: byte stored at base+5");
    ASSERT(mem[0x604] == 0,    "SBX: adjacent byte untouched");
}

static void test_lhx_lhux(void) {
    CPU cpu; setup(&cpu);
    mem_write16(0x700,   0x8000);          /* negative halfword */
    mem_write16(0x700+4, 0x1234);
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0x700),
        enc_i(0x0F, 1, 0, 0),
        enc_r(0x35, 2, 0, 1, 0),           /* LHX  r2, [r0+0]: sign-extend 0x8000 */
        enc_r(0x36, 3, 0, 1, 0),           /* LHUX r3, [r0+0]: zero-extend 0x8000 */
        enc_i(0x0F, 1, 0, 4),
        enc_r(0x36, 4, 0, 1, 0),           /* LHUX r4, [r0+4] = 0x1234 */
        enc_halt()
    };
    write_prog(0, prog, 7);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 0xFFFF8000, "LHX:  0x8000 sign-extended to -32768");
    ASSERT(cpu.r[3] == 0x8000,     "LHUX: 0x8000 zero-extended = 32768");
    ASSERT(cpu.r[4] == 0x1234,     "LHUX: 0x1234 at base+4");
}

static void test_shx(void) {
    CPU cpu; setup(&cpu);
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0x800),
        enc_i(0x0F, 1, 0, 6),
        enc_i(0x0F, 2, 0, 0xBEEF),
        enc_r(0x37, 2, 0, 1, 0),           /* SHX r2, [r0+6] */
        enc_halt()
    };
    write_prog(0, prog, 5);
    run_to_halt(&cpu);
    ASSERT(mem_read16(0x806) == 0xBEEF, "SHX: halfword stored at base+6");
    ASSERT(mem_read16(0x804) == 0,      "SHX: adjacent halfword untouched");
}

static void test_indexed_replaces_add_load(void) {
    CPU cpu; setup(&cpu);
    /* Demonstrate the selection-sort pattern:
       old: ADD r7, base, idx; LBU r5, [r7]
       new: LBUX r5, [base+idx]  — same result, one fewer instruction */
    mem[0x900 + 7] = 99;
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0x900),          /* r0 = base */
        enc_i(0x0F, 1, 0, 7),              /* r1 = index */
        /* old two-instruction sequence */
        enc_r(0x00, 7, 0, 1, 0),           /* ADD r7, r0, r1 */
        enc_i(0x1F, 5, 7, 0),              /* LBU r5, [r7] */
        /* new one-instruction sequence */
        enc_r(0x32, 6, 0, 1, 0),           /* LBUX r6, [r0+r1] */
        enc_halt()
    };
    write_prog(0, prog, 6);
    run_to_halt(&cpu);
    ASSERT(cpu.r[5] == 99, "old ADD+LBU pattern: r5=99");
    ASSERT(cpu.r[6] == 99, "new LBUX pattern: r6=99, same result");
}

/* ─────────────────────────────────────────────────────────────────────── */
/* BGTU / BLEU — unsigned greater-than and less-than-or-equal             */
/* ─────────────────────────────────────────────────────────────────────── */
static void test_bgtu_taken(void) {
    CPU cpu; setup(&cpu);
    /* 5u > 3u: CMP 5,3 → C=0 (no borrow), Z=0 → BGTU taken */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 5),
        enc_i(0x0F, 1, 0, 3),
        (0x28u<<24)|(0u<<16)|(1u<<12),  /* CMP r0, r1 */
        enc_b(9, 8),                    /* BGTU +8 — taken */
        enc_i(0x0F, 2, 0, 99),          /* skipped */
        enc_halt()
    };
    write_prog(0, prog, 6);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 0, "BGTU taken: 5u > 3u");
}

static void test_bgtu_not_taken_less(void) {
    CPU cpu; setup(&cpu);
    /* 3u > 5u is false: CMP 3,5 → C=1 (borrow) → BGTU NOT taken */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 3),
        enc_i(0x0F, 1, 0, 5),
        (0x28u<<24)|(0u<<16)|(1u<<12),  /* CMP r0, r1 */
        enc_b(9, 8),                    /* BGTU +8 — NOT taken */
        enc_i(0x0F, 2, 0, 42),          /* executed */
        enc_halt()
    };
    write_prog(0, prog, 6);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 42, "BGTU not taken: 3u not > 5u");
}

static void test_bgtu_not_taken_equal(void) {
    CPU cpu; setup(&cpu);
    /* 5u == 5u: CMP 5,5 → Z=1 → BGTU NOT taken */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 5),
        enc_i(0x0F, 1, 0, 5),
        (0x28u<<24)|(0u<<16)|(1u<<12),  /* CMP r0, r1 */
        enc_b(9, 8),                    /* BGTU +8 — NOT taken */
        enc_i(0x0F, 2, 0, 42),          /* executed */
        enc_halt()
    };
    write_prog(0, prog, 6);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 42, "BGTU not taken: 5u not > 5u (equal)");
}

static void test_bleu_taken_less(void) {
    CPU cpu; setup(&cpu);
    /* 3u <= 5u: CMP 3,5 → C=1 (borrow) → BLEU taken */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 3),
        enc_i(0x0F, 1, 0, 5),
        (0x28u<<24)|(0u<<16)|(1u<<12),  /* CMP r0, r1 */
        enc_b(10, 8),                   /* BLEU +8 — taken */
        enc_i(0x0F, 2, 0, 99),          /* skipped */
        enc_halt()
    };
    write_prog(0, prog, 6);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 0, "BLEU taken: 3u <= 5u");
}

static void test_bleu_taken_equal(void) {
    CPU cpu; setup(&cpu);
    /* 5u <= 5u: CMP 5,5 → Z=1 → BLEU taken */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 5),
        enc_i(0x0F, 1, 0, 5),
        (0x28u<<24)|(0u<<16)|(1u<<12),  /* CMP r0, r1 */
        enc_b(10, 8),                   /* BLEU +8 — taken */
        enc_i(0x0F, 2, 0, 99),          /* skipped */
        enc_halt()
    };
    write_prog(0, prog, 6);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 0, "BLEU taken: 5u <= 5u (equal)");
}

static void test_bleu_not_taken(void) {
    CPU cpu; setup(&cpu);
    /* 5u <= 3u is false: CMP 5,3 → C=0, Z=0 → BLEU NOT taken */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 5),
        enc_i(0x0F, 1, 0, 3),
        (0x28u<<24)|(0u<<16)|(1u<<12),  /* CMP r0, r1 */
        enc_b(10, 8),                   /* BLEU +8 — NOT taken */
        enc_i(0x0F, 2, 0, 42),          /* executed */
        enc_halt()
    };
    write_prog(0, prog, 6);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 42, "BLEU not taken: 5u not <= 3u");
}

/* ─────────────────────────────────────────────────────────────────────── */
/* LSLR / LSRR / ASRR — register-amount shifts                           */
/* ─────────────────────────────────────────────────────────────────────── */
static uint32_t enc_lslr(uint8_t rd, uint8_t rs1, uint8_t rs2) {
    return ((uint32_t)0x2D<<24)|((uint32_t)rd<<20)|((uint32_t)rs1<<16)|((uint32_t)rs2<<12);
}
static uint32_t enc_lsrr(uint8_t rd, uint8_t rs1, uint8_t rs2) {
    return ((uint32_t)0x2E<<24)|((uint32_t)rd<<20)|((uint32_t)rs1<<16)|((uint32_t)rs2<<12);
}
static uint32_t enc_asrr(uint8_t rd, uint8_t rs1, uint8_t rs2) {
    return ((uint32_t)0x2F<<24)|((uint32_t)rd<<20)|((uint32_t)rs1<<16)|((uint32_t)rs2<<12);
}

static void test_lslr(void) {
    CPU cpu; setup(&cpu);
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 1),    /* r0 = 1 */
        enc_i(0x0F, 1, 0, 4),    /* r1 = 4 (shift amount) */
        enc_lslr(2, 0, 1),       /* LSLR r2, r0, r1 → 1<<4 = 16 */
        enc_halt()
    };
    write_prog(0, prog, 4);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 16, "LSLR: 1 << 4 = 16");
    ASSERT(!(cpu.flags & FLAG_C), "LSLR: C cleared");
}

static void test_lsrr(void) {
    CPU cpu; setup(&cpu);
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0xFFFF), enc_i(0x13, 0, 0, 0xFFFF), /* r0 = 0xFFFFFFFF */
        enc_i(0x0F, 1, 0, 4),      /* r1 = 4 */
        enc_lsrr(2, 0, 1),         /* LSRR r2, r0, r1 → 0xFFFFFFFF>>4 = 0x0FFFFFFF */
        enc_halt()
    };
    write_prog(0, prog, 5);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 0x0FFFFFFF, "LSRR: 0xFFFFFFFF >> 4 = 0x0FFFFFFF (logical)");
    ASSERT(!(cpu.flags & FLAG_C), "LSRR: C cleared");
}

static void test_asrr(void) {
    CPU cpu; setup(&cpu);
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0xFFFF), enc_i(0x13, 0, 0, 0xFFFF), /* r0 = 0xFFFFFFFF = -1 */
        enc_i(0x0F, 1, 0, 4),      /* r1 = 4 */
        enc_asrr(2, 0, 1),         /* ASRR r2, r0, r1 → -1>>4 = -1 = 0xFFFFFFFF */
        enc_halt()
    };
    write_prog(0, prog, 5);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 0xFFFFFFFF, "ASRR: -1 >> 4 = -1 (arithmetic)");
    ASSERT(!(cpu.flags & FLAG_C), "ASRR: C cleared");
}

static void test_lslr_mask(void) {
    CPU cpu; setup(&cpu);
    /* shift amount should be masked to 5 bits: amount=32 → 32&31=0 → no shift */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 7),     /* r0 = 7 */
        enc_i(0x0F, 1, 0, 32),    /* r1 = 32 → masked to 0 */
        enc_lslr(2, 0, 1),        /* LSLR r2, r0, r1 → 7<<0 = 7 */
        enc_halt()
    };
    write_prog(0, prog, 4);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 7, "LSLR: shift amount 32 masked to 0, result = r0");
}

/* ─────────────────────────────────────────────────────────────────────── */
/* Fibonacci program (end-to-end)                                          */
/* ─────────────────────────────────────────────────────────────────────── */
static void test_fibonacci(void) {
    CPU cpu; setup(&cpu);
    /* Compute fib(10) = 55 iteratively in r0
       r0=a=0, r1=b=1, r2=count=10 */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0),          /* r0=0 (a) */
        enc_i(0x0F, 1, 0, 1),          /* r1=1 (b) */
        enc_i(0x0F, 2, 0, 10),         /* r2=10 (count) */
        /* loop: */
        enc_r(0x00, 3, 0, 1, 0),       /* r3 = a+b */
        enc_r(0x27, 0, 1, 0, 0),       /* r0 = b */
        enc_r(0x27, 1, 3, 0, 0),       /* r1 = r3 */
        enc_i(0x15, 2, 2, 1),          /* r2 = r2-1 */
        enc_b(1, (uint32_t)(-16)),      /* BNE -16 (back to loop) */
        enc_halt()
    };
    write_prog(0, prog, 9);
    run_to_halt(&cpu);
    ASSERT(cpu.r[0] == 55, "Fibonacci(10) = 55");
}

/* ─────────────────────────────────────────────────────────────────────── */
/* Flag: add carry and overflow                                            */
/* ─────────────────────────────────────────────────────────────────────── */
static void test_add_carry(void) {
    CPU cpu; setup(&cpu);
    /* 0xFFFFFFFF + 1 = 0 with carry */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0xFFFF),
        enc_i(0x13, 0, 0, 0xFFFF),   /* r0 = 0xFFFFFFFF */
        enc_i(0x0F, 1, 0, 1),
        enc_r(0x00, 2, 0, 1, 0),     /* ADD 0xFFFFFFFF+1 */
        enc_halt()
    };
    write_prog(0, prog, 5);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 0,          "0xFFFFFFFF+1 = 0");
    ASSERT(cpu.flags & FLAG_C,     "Carry set on wrap");
    ASSERT(cpu.flags & FLAG_Z,     "Z set when result=0");
}

static void test_add_overflow(void) {
    CPU cpu; setup(&cpu);
    /* 0x7FFFFFFF + 1 = 0x80000000 = signed overflow */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0xFFFF),
        enc_i(0x13, 0, 0, 0x7FFF),   /* r0 = 0x7FFFFFFF */
        enc_i(0x0F, 1, 0, 1),
        enc_r(0x00, 2, 0, 1, 0),
        enc_halt()
    };
    write_prog(0, prog, 5);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 0x80000000, "0x7FFFFFFF+1 = 0x80000000");
    ASSERT(cpu.flags & FLAG_V,     "Overflow set");
    ASSERT(cpu.flags & FLAG_N,     "N set (result negative)");
}

/* ─────────────────────────────────────────────────────────────────────── */
/* LUI                                                                     */
/* ─────────────────────────────────────────────────────────────────────── */
static void test_lui(void) {
    CPU cpu; setup(&cpu);
    uint32_t prog[] = {
        enc_i(0x38, 1, 0, 0x1234),      /* LUI r1, 0x1234 → r1 = 0x12340000 */
        enc_i(0x38, 2, 0, 0xDEAD),      /* LUI r2, 0xDEAD → r2 = 0xDEAD0000 */
        enc_i(0x17, 2, 2, 0xBEEF),      /* ORI r2, r2, 0xBEEF → r2 = 0xDEADBEEF */
        enc_halt()
    };
    write_prog(0, prog, 4);
    run_to_halt(&cpu);
    ASSERT(cpu.r[1] == 0x12340000u, "LUI r1,0x1234 → 0x12340000");
    ASSERT(cpu.r[2] == 0xDEADBEEFu, "LUI+ORI loads 0xDEADBEEF");
}

static void test_lui_no_flags(void) {
    CPU cpu; setup(&cpu);
    /* CMP equal sets Z; LUI must not clear it */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 7),
        enc_r(0x28, 0, 0, 0, 0),        /* CMP r0, r0 → Z=1 */
        enc_i(0x38, 1, 0, 0xABCD),      /* LUI r1, 0xABCD — must not touch flags */
        enc_halt()
    };
    write_prog(0, prog, 4);
    run_to_halt(&cpu);
    ASSERT(cpu.r[1] == 0xABCD0000u, "LUI sets upper half");
    ASSERT(cpu.flags & FLAG_Z,      "LUI preserves Z");
}

/* ─────────────────────────────────────────────────────────────────────── */
/* ROLR / RORR / ROLI / RORI                                               */
/* ─────────────────────────────────────────────────────────────────────── */
static void test_rolr(void) {
    CPU cpu; setup(&cpu);
    /* ROLR: 0x01000001 left by 4 = 0x10000010 */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0x0001),
        enc_i(0x13, 0, 0, 0x0100),      /* r0 = 0x01000001 */
        enc_i(0x0F, 1, 0, 4),
        enc_r(0x39, 2, 0, 1, 0),        /* ROLR r2, r0, r1 */
        enc_halt()
    };
    write_prog(0, prog, 5);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 0x10000010u, "ROLR 0x01000001 by 4 = 0x10000010");
}

static void test_rorr(void) {
    CPU cpu; setup(&cpu);
    /* RORR: 0x01000001 right by 4 = 0x10100000 */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0x0001),
        enc_i(0x13, 0, 0, 0x0100),      /* r0 = 0x01000001 */
        enc_i(0x0F, 1, 0, 4),
        enc_r(0x3A, 2, 0, 1, 0),        /* RORR r2, r0, r1 */
        enc_halt()
    };
    write_prog(0, prog, 5);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 0x10100000u, "RORR 0x01000001 by 4 = 0x10100000");
}

static void test_roli(void) {
    CPU cpu; setup(&cpu);
    /* ROLI: 0x80000001 left by 1 = 0x00000003 */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0x0001),
        enc_i(0x13, 0, 0, 0x8000),      /* r0 = 0x80000001 */
        enc_r(0x3B, 1, 0, 0, 1),        /* ROLI r1, r0, 1 */
        enc_halt()
    };
    write_prog(0, prog, 4);
    run_to_halt(&cpu);
    ASSERT(cpu.r[1] == 0x00000003u, "ROLI 0x80000001 left 1 = 0x00000003");
}

static void test_rori(void) {
    CPU cpu; setup(&cpu);
    /* RORI: 0x80000001 right by 1 = 0xC0000000 */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0x0001),
        enc_i(0x13, 0, 0, 0x8000),      /* r0 = 0x80000001 */
        enc_r(0x3C, 1, 0, 0, 1),        /* RORI r1, r0, 1 */
        enc_halt()
    };
    write_prog(0, prog, 4);
    run_to_halt(&cpu);
    ASSERT(cpu.r[1] == 0xC0000000u, "RORI 0x80000001 right 1 = 0xC0000000");
}

static void test_rotate_by_zero(void) {
    CPU cpu; setup(&cpu);
    /* Rotate by 0 = no change */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 0x1234),
        enc_i(0x0F, 1, 0, 0),           /* shift amount = 0 */
        enc_r(0x39, 2, 0, 1, 0),        /* ROLR r2, r0, r1 */
        enc_r(0x3A, 3, 0, 1, 0),        /* RORR r3, r0, r1 */
        enc_r(0x3B, 4, 0, 0, 0),        /* ROLI r4, r0, 0 */
        enc_r(0x3C, 5, 0, 0, 0),        /* RORI r5, r0, 0 */
        enc_halt()
    };
    write_prog(0, prog, 7);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 0x1234, "ROLR by 0 = no change");
    ASSERT(cpu.r[3] == 0x1234, "RORR by 0 = no change");
    ASSERT(cpu.r[4] == 0x1234, "ROLI by 0 = no change");
    ASSERT(cpu.r[5] == 0x1234, "RORI by 0 = no change");
}

/* ─────────────────────────────────────────────────────────────────────── */
/* MOV/MOVI/MOVHI must not update flags                                    */
/* ─────────────────────────────────────────────────────────────────────── */
static void test_mov_no_flags(void) {
    CPU cpu; setup(&cpu);
    /* Set known flags with ADD: 1 + (-1) = 0  → Z=1, C=1 */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 1),           /* MOVI r0, 1 */
        enc_i(0x0F, 1, 0, 0xFFFF),      /* MOVI r1, 0xFFFF (low) */
        enc_i(0x13, 1, 0, 0xFFFF),      /* MOVHI r1, 0xFFFF → r1 = 0xFFFFFFFF */
        enc_r(0x00, 2, 0, 1, 0),        /* ADD r2, r0, r1 → 0, sets Z+C */
        enc_r(0x27, 3, 0, 0, 0),        /* MOV r3, r0 — must not change flags */
        enc_halt()
    };
    write_prog(0, prog, 6);
    run_to_halt(&cpu);
    ASSERT(cpu.r[3] == 1,                  "MOV copies value");
    ASSERT(cpu.flags & FLAG_Z,             "MOV preserves Z");
    ASSERT(cpu.flags & FLAG_C,             "MOV preserves C");
    ASSERT(!(cpu.flags & FLAG_N),          "MOV preserves N clear");
}

static void test_movi_no_flags(void) {
    CPU cpu; setup(&cpu);
    /* CMP 1, 2 → sets N (result negative, borrow) */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 1),           /* MOVI r0, 1 */
        enc_i(0x0F, 1, 0, 2),           /* MOVI r1, 2 */
        enc_r(0x28, 0, 0, 1, 0),        /* CMP r0, r1 → flags from 1-2 */
        enc_i(0x0F, 2, 0, 42),          /* MOVI r2, 42 — must not change flags */
        enc_halt()
    };
    write_prog(0, prog, 5);
    run_to_halt(&cpu);
    ASSERT(cpu.r[2] == 42,                 "MOVI loads value");
    ASSERT(cpu.flags & FLAG_N,             "MOVI preserves N");
    ASSERT(cpu.flags & FLAG_C,             "MOVI preserves C (borrow)");
    ASSERT(!(cpu.flags & FLAG_Z),          "MOVI preserves Z clear");
}

static void test_movhi_no_flags(void) {
    CPU cpu; setup(&cpu);
    /* CMP equal → Z=1 */
    uint32_t prog[] = {
        enc_i(0x0F, 0, 0, 5),           /* MOVI r0, 5 */
        enc_r(0x28, 0, 0, 0, 0),        /* CMP r0, r0 → Z=1 */
        enc_i(0x0F, 1, 0, 0x00FF),      /* MOVI r1, 0x00FF */
        enc_i(0x13, 1, 0, 0x0001),      /* MOVHI r1, 0x0001 → r1 = 0x000100FF */
        enc_halt()
    };
    write_prog(0, prog, 5);
    run_to_halt(&cpu);
    ASSERT(cpu.r[1] == 0x000100FF,         "MOVHI sets high bits");
    ASSERT(cpu.flags & FLAG_Z,             "MOVHI preserves Z");
    ASSERT(!(cpu.flags & FLAG_N),          "MOVHI preserves N clear");
    ASSERT(!(cpu.flags & FLAG_C),          "MOVHI preserves C clear");
}

static void test_cas(void) {
    CPU cpu; setup(&cpu);
    mem_write32(0x1000, 0xAABBCCDD);
    cpu.r[0] = 0xAABBCCDD; /* expected */
    cpu.r[1] = 0x00001000; /* address  */
    cpu.r[2] = 0x11223344; /* new val  */
    uint32_t prog[] = {
        enc_r(0x3D, 0, 1, 2, 0), /* CAS r0, r1, r2 */
        enc_halt()
    };
    write_prog(0, prog, 2);
    run_to_halt(&cpu);
    ASSERT(mem_read32(0x1000) == 0x11223344, "CAS success: memory updated");
    ASSERT((cpu.flags & (1u<<30)) != 0,      "CAS success: Z flag set");

    /* Failing CAS */
    setup(&cpu);
    mem_write32(0x1000, 0xDEADBEEF);
    cpu.r[0] = 0x12345678; /* wrong expected */
    cpu.r[1] = 0x00001000;
    cpu.r[2] = 0x99999999;
    write_prog(0, prog, 2);
    run_to_halt(&cpu);
    ASSERT(mem_read32(0x1000) == 0xDEADBEEF, "CAS fail: memory unchanged");
    ASSERT(cpu.r[0] == 0xDEADBEEF,           "CAS fail: rd loaded with current");
    ASSERT((cpu.flags & (1u<<30)) == 0,      "CAS fail: Z flag clear");
}

static void test_estatus(void) {
    CPU cpu; setup(&cpu);
    /* On exception entry, STATUS should be saved to ESTATUS */
    cpu.status = 0x02; /* user mode, IE=1 */
    cpu.evec   = 0x200;
    mem_write32(0x20C, 0x100); /* handler for cause 3 (SYSCALL) */
    mem_write32(0x100, enc_halt());
    /* SYSCALL in user mode */
    uint32_t prog[] = { enc_syscall(), enc_halt() };
    write_prog(0, prog, 2);
    /* need user mode for syscall to dispatch */
    cpu.status = 0x00;
    cpu_run(&cpu, 0);
    /* After syscall dispatch, ESTATUS should hold the pre-trap STATUS */
    ASSERT(cpu.estatus == 0x00, "ESTATUS saved from pre-trap STATUS");
    ASSERT(cpu.status  == 0x01, "STATUS = supervisor after trap");

    /* SYSRET should restore STATUS = estatus & ~1 */
    setup(&cpu);
    cpu.estatus = 0x02; /* IE=1, user mode */
    cpu.epc     = 0x100;
    cpu.eflags  = 0;
    /* Put SYSRET in supervisor-mode program */
    mem_write32(0, enc_sysret());
    mem_write32(0x100, enc_halt());
    cpu.status = 0x01; /* supervisor so SYSRET is legal */
    cpu_run(&cpu, 0);
    ASSERT(cpu.status == 0x02, "SYSRET restores STATUS from ESTATUS");

    /* SYSRET with ESTATUS having bit0=1 must still clear it */
    setup(&cpu);
    cpu.estatus = 0x03; /* supervisor+IE — bit0 must be cleared by SYSRET */
    cpu.epc     = 0x100;
    cpu.eflags  = 0;
    mem_write32(0, enc_sysret());
    mem_write32(0x100, enc_halt());
    cpu.status = 0x01;
    cpu_run(&cpu, 0);
    ASSERT(cpu.status == 0x02, "SYSRET forces user mode even when ESTATUS has bit0=1");
}

/* ─────────────────────────────────────────────────────────────────────── */
/* Main                                                                    */
/* ─────────────────────────────────────────────────────────────────────── */
int main(void) {
    printf("Running CPUTwo emulator tests...\n\n");

    /* Step 1-2 */
    test_cpu_init();
    test_memory_rw();
    /* Step 3 */
    test_exception_dispatch();
    test_double_fault();
    /* Step 4 */
    test_sv_regs();
    test_sv_regs_user_fault();
    /* Steps 6-7 */
    test_alu_add();
    test_alu_sub_flags();
    test_alu_and_or_xor_not();
    test_alu_shifts();
    test_mov();
    /* Step 8 */
    test_imm_alu();
    test_imm_shifts();
    /* Step 9 */
    test_movi_movhi();
    /* Step 10 */
    test_mul_div();
    test_mulh();
    test_div_by_zero();
    /* Steps 11-12 */
    test_load_store();
    test_load_store_via_cpu();
    test_load_widths();
    test_misaligned();
    /* Step 13 */
    test_branch_beq_taken();
    test_branch_beq_not_taken();
    test_branch_bne();
    test_branch_blt_bge();
    test_branch_bltu();
    test_branch_illegal_cond();
    /* BA */
    test_ba_taken();
    test_ba_with_zero_flags();
    /* CMP / CMPI */
    test_cmp_eq();
    test_cmp_lt();
    test_cmpi_eq();
    test_cmpi_large();
    /* BLT/BGE N^V fix */
    test_blt_with_overflow();
    test_bge_with_overflow();
    test_blt_normal();
    test_cmp_then_bge();
    /* Step 14 */
    test_jmp();
    /* Steps 15-16 */
    test_syscall_sysret();
    test_syscall_saves_epc();
    test_sysret_in_user_mode();
    /* Steps 17-18 */
    test_halt();
    test_illegal_instr();
    /* Step 19-20 */
    test_bus_error();
    test_timer_interrupt();
    /* ADDC / SUBC */
    test_addc();
    test_addc_no_carry();
    test_subc();
    /* CALLR */
    test_callr();
    test_callr_r0_discards();
    /* BGT / BLE */
    test_bgt_taken();
    test_bgt_not_taken_less();
    test_bgt_not_taken_equal();
    test_ble_taken_less();
    test_ble_taken_equal();
    test_ble_not_taken();
    /* Indexed memory */
    test_lwx();
    test_swx();
    test_lbux();
    test_lbx_signed();
    test_sbx();
    test_lhx_lhux();
    test_shx();
    test_indexed_replaces_add_load();
    /* BGTU / BLEU */
    test_bgtu_taken();
    test_bgtu_not_taken_less();
    test_bgtu_not_taken_equal();
    test_bleu_taken_less();
    test_bleu_taken_equal();
    test_bleu_not_taken();
    /* LSLR / LSRR / ASRR */
    test_lslr();
    test_lsrr();
    test_asrr();
    test_lslr_mask();
    /* LUI */
    test_lui();
    test_lui_no_flags();
    /* ROLR / RORR / ROLI / RORI */
    test_rolr();
    test_rorr();
    test_roli();
    test_rori();
    test_rotate_by_zero();
    /* Flag tests */
    test_add_carry();
    test_add_overflow();
    /* MOV/MOVI/MOVHI must not update flags */
    test_mov_no_flags();
    test_movi_no_flags();
    test_movhi_no_flags();
    /* End-to-end */
    test_fibonacci();
    test_cas();
    test_estatus();

    printf("\nResults: %d/%d passed", tests_passed, tests_run);
    if (tests_failed) printf(", %d FAILED", tests_failed);
    printf("\n");
    return tests_failed ? 1 : 0;
}
