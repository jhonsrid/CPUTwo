/* CPUTwo linker unit tests — C99 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "dobj.h"

/* Pull in linker internals, stubbing out main */
#define main linker_main_stub
#include "linker.c"
#undef main

/* ── Test harness ──────────────────────────────────────────────────────────── */
static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { tests_failed++; \
           fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); } \
} while(0)

/* ── .dobj builder helpers ─────────────────────────────────────────────────── */
typedef struct { uint8_t data[8192]; size_t len; } Buf;

static void b_u8 (Buf *b, uint8_t  v) { b->data[b->len++] = v; }
static void b_u16(Buf *b, uint16_t v) { b_u8(b,(v>>8)&0xFF); b_u8(b,v&0xFF); }
static void b_u32(Buf *b, uint32_t v) {
    b_u8(b,(v>>24)&0xFF); b_u8(b,(v>>16)&0xFF);
    b_u8(b,(v>> 8)&0xFF); b_u8(b,v&0xFF);
}
static void b_str(Buf *b, const char *s, int pad) {
    int i = 0;
    for (; s[i] && i < pad; i++) b_u8(b,(uint8_t)s[i]);
    for (; i < pad; i++) b_u8(b, 0);
}

/* Build a minimal .dobj binary:
   nsec=1, vma_hint, sec_data, nsym symbols, nrel relocs.
   Returns file written to `path`. */
typedef struct {
    char    name[28];
    uint32_t value;
    uint16_t sec_idx;
    uint8_t  is_global;
} TSym;

typedef struct {
    uint32_t offset;
    uint32_t sym_idx;
    uint8_t  type;
    uint8_t  sec_idx;
    int32_t  addend;
} TRel;

static void write_test_dobj(const char *path,
                             uint32_t vma_hint,
                             const uint8_t *sec_data, uint32_t sec_size,
                             const TSym *syms, uint32_t nsym,
                             const TRel *rels, uint32_t nrel)
{
    Buf b; memset(&b, 0, sizeof(b));

    uint32_t data_off = DOBJ_HEADER_SIZE
                      + DOBJ_SECTION_SIZE
                      + nsym * DOBJ_SYMBOL_SIZE
                      + nrel * DOBJ_RELOC_SIZE;

    /* Header */
    b_str(&b, DOBJ_MAGIC, 4);
    b_u8(&b, DOBJ_VERSION); b_u8(&b,0); b_u8(&b,0); b_u8(&b,0);
    b_u32(&b, 1);       /* nsec */
    b_u32(&b, nsym);
    b_u32(&b, nrel);
    b_u32(&b, data_off);

    /* Section header */
    b_str(&b, ".text", 8);
    b_u32(&b, 0);          /* offset in blob */
    b_u32(&b, sec_size);
    b_u32(&b, vma_hint);
    b_u32(&b, DOBJ_SF_EXEC | DOBJ_SF_ALLOC);

    /* Symbols */
    for (uint32_t i = 0; i < nsym; i++) {
        b_str(&b, syms[i].name, 28);
        b_u32(&b, syms[i].value);
        b_u16(&b, syms[i].sec_idx);
        b_u8(&b,  syms[i].is_global);
        b_u8(&b,  0);
    }

    /* Relocations */
    for (uint32_t i = 0; i < nrel; i++) {
        b_u32(&b, rels[i].offset);
        b_u32(&b, rels[i].sym_idx);
        b_u8(&b,  rels[i].type);
        b_u8(&b,  rels[i].sec_idx);
        b_u8(&b,0); b_u8(&b,0);
        b_u32(&b, (uint32_t)rels[i].addend);
    }

    /* Section data */
    for (uint32_t i = 0; i < sec_size; i++) b_u8(&b, sec_data[i]);

    FILE *f = fopen(path, "wb");
    fwrite(b.data, 1, b.len, f);
    fclose(f);
}

/* Reset linker global state between tests */
static void linker_reset(void) {
    free_objs();
    obj_count   = 0;
    glob_count  = 0;
    link_errors = 0;
    memset(objs,       0, sizeof(objs));
    memset(glob_syms,  0, sizeof(glob_syms));
}

/* Read big-endian u32 from output binary at byte offset */
static uint32_t read_word(const uint8_t *buf, uint32_t byte_off) {
    return ((uint32_t)buf[byte_off]   << 24)
         | ((uint32_t)buf[byte_off+1] << 16)
         | ((uint32_t)buf[byte_off+2] <<  8)
         |  (uint32_t)buf[byte_off+3];
}

/* ── Tests ─────────────────────────────────────────────────────────────────── */

/* Single object, no relocations: output matches input section data */
static void test_single_obj_no_relocs(void) {
    /* HALT instruction at address 0 */
    uint8_t code[4] = {0x12, 0x00, 0x00, 0x00};  /* HALT */
    write_test_dobj("/tmp/t1.dobj", 0, code, 4, NULL, 0, NULL, 0);

    linker_reset();
    ASSERT(load_dobj("/tmp/t1.dobj"), "load single obj");
    assign_addresses(0);
    build_globals();
    ASSERT(link_errors == 0, "no link errors");
    resolve_relocs();
    ASSERT(link_errors == 0, "no reloc errors");
    ASSERT(emit_output("/tmp/t1.bin"), "emit output");

    FILE *f = fopen("/tmp/t1.bin", "rb");
    ASSERT(f != NULL, "output file exists");
    uint8_t out[4] = {0};
    fread(out, 1, 4, f); fclose(f);
    ASSERT(memcmp(out, code, 4) == 0, "output matches input");
}

/* Single object with vma_hint: section placed at vma_hint */
static void test_vma_hint(void) {
    uint8_t code[4] = {0x12, 0x00, 0x00, 0x00};
    write_test_dobj("/tmp/t2.dobj", 0x1000, code, 4, NULL, 0, NULL, 0);

    linker_reset();
    load_dobj("/tmp/t2.dobj");
    assign_addresses(0);
    ASSERT(objs[0].secs[0].base == 0x1000, "section placed at vma_hint");
}

/* RELOC_ABS32: .word <local_sym> patched with absolute address */
static void test_abs32_reloc(void) {
    /* Layout (vma=0):
       0000: HALT            (4 bytes)
       0004: word32 = 0      (placeholder — ABS32 reloc → label 'target' = sec_off 0)
    */
    uint8_t code[8] = {0x12,0,0,0,  0,0,0,0};
    TSym sym  = {"target", 0 /*sec_off=0*/, 0 /*sec 0*/, 0};
    TRel rel  = {4 /*offset*/, 0 /*sym_idx*/, RELOC_ABS32, 0 /*sec*/, 0 /*addend*/};
    write_test_dobj("/tmp/t3.dobj", 0, code, 8, &sym, 1, &rel, 1);

    linker_reset();
    load_dobj("/tmp/t3.dobj");
    assign_addresses(0);
    build_globals();
    resolve_relocs();
    ASSERT(link_errors == 0, "abs32: no errors");
    emit_output("/tmp/t3.bin");

    FILE *f = fopen("/tmp/t3.bin", "rb");
    uint8_t out[8]; fread(out, 1, 8, f); fclose(f);
    uint32_t patched = read_word(out, 4);
    /* symbol 'target' sec_off=0, section base=0 → abs=0 */
    ASSERT(patched == 0x00000000, "abs32 patched to 0");
}

/* RELOC_ABS32 with vma_hint: abs addr = vma + sec_offset */
static void test_abs32_with_vma(void) {
    /* vma=0x4000, label at sec_off=0, .word placeholder at sec_off=4 */
    uint8_t code[8] = {0x12,0,0,0,  0,0,0,0};
    TSym sym  = {"lbl", 0, 0, 0};
    TRel rel  = {4, 0, RELOC_ABS32, 0, 0};
    write_test_dobj("/tmp/t4.dobj", 0x4000, code, 8, &sym, 1, &rel, 1);

    linker_reset();
    load_dobj("/tmp/t4.dobj");
    assign_addresses(0);
    build_globals();
    resolve_relocs();
    ASSERT(link_errors == 0, "abs32+vma: no errors");
    emit_output("/tmp/t4.bin");

    FILE *f = fopen("/tmp/t4.bin", "rb");
    /* output starts at 0, section at 0x4000 → file is 0x4008 bytes */
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0x4004, SEEK_SET);
    ASSERT(sz >= 0x4008, "output covers section");
    uint8_t word[4]; fread(word, 1, 4, f); fclose(f);
    uint32_t patched = read_word(word, 0);
    ASSERT(patched == 0x4000, "abs32+vma patched to 0x4000");
}

/* RELOC_MOVI32_LO / HI: MOVI32 r1, <label> at vma=0x2000, label at sec_off=8 */
static void test_movi32_reloc(void) {
    /* sec_off 0: MOVI  r1, 0 (placeholder)  enc = 0x0F100000
       sec_off 4: MOVHI r1, 0 (placeholder)  enc = 0x13100000
       sec_off 8: HALT                        enc = 0x12000000
    */
    uint8_t code[12] = {
        0x0F,0x10,0x00,0x00,   /* MOVI  r1, 0 */
        0x13,0x10,0x00,0x00,   /* MOVHI r1, 0 */
        0x12,0x00,0x00,0x00    /* HALT (= 'target') */
    };
    TSym sym = {"target", 8 /*sec_off*/, 0, 0};
    TRel rels[2] = {
        {0, 0, RELOC_MOVI32_LO, 0, 0},
        {4, 0, RELOC_MOVI32_HI, 0, 0}
    };
    write_test_dobj("/tmp/t5.dobj", 0x2000, code, 12, &sym, 1, rels, 2);

    linker_reset();
    load_dobj("/tmp/t5.dobj");
    assign_addresses(0);
    build_globals();
    resolve_relocs();
    ASSERT(link_errors == 0, "movi32: no errors");
    emit_output("/tmp/t5.bin");

    FILE *f = fopen("/tmp/t5.bin", "rb");
    fseek(f, 0x2000, SEEK_SET);
    uint8_t out[12]; fread(out, 1, 12, f); fclose(f);
    uint32_t movi  = read_word(out, 0);
    uint32_t movhi = read_word(out, 4);
    uint32_t target_abs = 0x2000 + 8;     /* = 0x2008 */
    ASSERT((movi  & 0xFFFF) == (target_abs & 0xFFFF), "MOVI lo patched");
    ASSERT((movhi & 0xFFFF) == (target_abs >> 16),    "MOVHI hi patched");
}

/* Cross-file CALL: object A defines _puts (global), object B has extern _puts
   and a CALL _puts instruction (RELOC_JUMP20). */
static void test_cross_call(void) {
    /* Object A: _puts at sec_off=0, vma=0x1000 */
    uint8_t codeA[4] = {0x12,0,0,0};  /* HALT (= _puts body) */
    TSym symA = {"_puts", 0, 0, 1 /*global*/};
    write_test_dobj("/tmp/tA.dobj", 0x1000, codeA, 4, &symA, 1, NULL, 0);

    /* Object B: vma=0, CALL _puts at sec_off=0
       JMP lr, 0  enc = 0x0E E0 00 00 (lr=14, offset=0) */
    uint8_t codeB[4] = {0x0E,0xE0,0x00,0x00};
    TSym symB = {"_puts", 0, (uint16_t)DOBJ_SEC_UNDEF, 0 /*extern*/};
    TRel relB = {0, 0, RELOC_JUMP20, 0, 0};
    write_test_dobj("/tmp/tB.dobj", 0, codeB, 4, &symB, 1, &relB, 1);

    /* Load B first (no vma_hint → packed at 0), then A (vma_hint=0x1000). */
    linker_reset();
    load_dobj("/tmp/tB.dobj");
    load_dobj("/tmp/tA.dobj");
    assign_addresses(0);
    build_globals();
    ASSERT(link_errors == 0, "cross call: globals ok");
    resolve_relocs();
    ASSERT(link_errors == 0, "cross call: relocs ok");
    emit_output("/tmp/tAB.bin");

    /* B at base=0, A at base=0x1000.
       CALL at 0x0000, _puts at 0x1000.  offset20 = 0x1000. */
    FILE *f = fopen("/tmp/tAB.bin", "rb");
    uint8_t instr[4]; fread(instr, 1, 4, f); fclose(f);
    uint32_t enc = read_word(instr, 0);
    uint32_t offset20 = enc & 0xFFFFF;
    int32_t  signed_off = (offset20 & 0x80000) ? (int32_t)(offset20 | 0xFFF00000u)
                                                : (int32_t)offset20;
    ASSERT(signed_off == 0x1000, "cross call: JUMP20 offset correct");
}

/* Duplicate global symbol → link error */
static void test_duplicate_global(void) {
    uint8_t code[4] = {0x12,0,0,0};
    TSym sym = {"_foo", 0, 0, 1};
    write_test_dobj("/tmp/tDup1.dobj", 0, code, 4, &sym, 1, NULL, 0);
    write_test_dobj("/tmp/tDup2.dobj", 0x100, code, 4, &sym, 1, NULL, 0);

    linker_reset();
    load_dobj("/tmp/tDup1.dobj");
    load_dobj("/tmp/tDup2.dobj");
    assign_addresses(0);
    build_globals();
    ASSERT(link_errors > 0, "duplicate global: error detected");
}

/* Undefined extern → link error */
static void test_undefined_extern(void) {
    uint8_t code[4] = {0x0E,0xE0,0x00,0x00};
    TSym sym = {"_missing", 0, (uint16_t)DOBJ_SEC_UNDEF, 0};
    TRel rel = {0, 0, RELOC_JUMP20, 0, 0};
    write_test_dobj("/tmp/tUndef.dobj", 0, code, 4, &sym, 1, &rel, 1);

    linker_reset();
    load_dobj("/tmp/tUndef.dobj");
    assign_addresses(0);
    build_globals();
    resolve_relocs();
    ASSERT(link_errors > 0, "undefined extern: error detected");
}

/* RELOC_BRANCH20: BEQ to extern target */
static void test_branch_reloc(void) {
    /* vma=0, BEQ r0,0 placeholder at offset=0, extern target at 0x100 */
    uint8_t codeA[4] = {0x12,0,0,0};           /* HALT = target */
    TSym symA = {"_tgt", 0, 0, 1};
    write_test_dobj("/tmp/tBrA.dobj", 0x100, codeA, 4, &symA, 1, NULL, 0);

    /* BEQ placeholder: opcode=0x0D, cond=0 (BEQ), offset=0 */
    uint8_t codeB[4] = {0x0D,0x00,0x00,0x00};
    TSym symB = {"_tgt", 0, (uint16_t)DOBJ_SEC_UNDEF, 0};
    TRel relB = {0, 0, RELOC_BRANCH20, 0, 0};
    write_test_dobj("/tmp/tBrB.dobj", 0, codeB, 4, &symB, 1, &relB, 1);

    /* Load B first (no vma_hint → base=0), then A (vma_hint=0x100). */
    linker_reset();
    load_dobj("/tmp/tBrB.dobj");
    load_dobj("/tmp/tBrA.dobj");
    assign_addresses(0);
    build_globals();
    resolve_relocs();
    ASSERT(link_errors == 0, "branch reloc: no errors");
    emit_output("/tmp/tBr.bin");

    /* BEQ at 0x0000, _tgt at 0x100.  offset20 = 0x100. */
    FILE *f = fopen("/tmp/tBr.bin", "rb");
    uint8_t instr[4]; fread(instr, 1, 4, f); fclose(f);
    uint32_t enc = read_word(instr, 0);
    uint32_t offset20 = enc & 0xFFFFF;
    int32_t signed_off = (offset20 & 0x80000) ? (int32_t)(offset20 | 0xFFF00000u)
                                               : (int32_t)offset20;
    ASSERT(signed_off == 0x100, "branch reloc: offset correct (0x100)");
    /* Opcode and cond preserved */
    ASSERT((enc >> 24) == 0x0D, "branch reloc: opcode 0x0D preserved");
    ASSERT(((enc >> 20) & 0xF) == 0, "branch reloc: cond=0 preserved");
}

/* ── main ──────────────────────────────────────────────────────────────────── */
int main(void) {
    test_single_obj_no_relocs();
    test_vma_hint();
    test_abs32_reloc();
    test_abs32_with_vma();
    test_movi32_reloc();
    test_cross_call();
    test_duplicate_global();
    test_undefined_extern();
    test_branch_reloc();

    fprintf(stderr, "\nLinker tests: %d/%d passed", tests_passed, tests_run);
    if (tests_failed) fprintf(stderr, ", %d FAILED", tests_failed);
    fprintf(stderr, "\n");
    return tests_failed ? 1 : 0;
}
