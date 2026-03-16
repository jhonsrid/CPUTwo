/* CPUTwo two-pass assembler — C99
 * Compile: cc -std=c99 -Wall -o assemblertwo assembler.c
 * Usage:   ./assemblertwo input.asm output.bin [output.map]
 *          ./assemblertwo -c input.asm output.dobj   (object-file mode)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <stdarg.h>
#include "dobj.h"

/* ── Limits ─────────────────────────────────────────────────────────────── */
#define MAX_SYMS     4096
#define MAX_LINES    65536
#define MAX_OUTPUT   (8*1024*1024)
#define MAX_LINE_LEN 1024
#define MAX_TOKS     64

/* ── Token types ─────────────────────────────────────────────────────────── */
typedef enum {
    TOK_IDENT,
    TOK_INT,
    TOK_STRING,
    TOK_COMMA,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_PLUS,
    TOK_MINUS,
    TOK_RSHIFT,
    TOK_COLON,
    TOK_NEWLINE,
    TOK_EOF,
    TOK_DOT_IDENT   /* .word, .byte, etc. */
} TokType;

typedef struct {
    TokType  type;
    char     str[256];
    int64_t  ival;
} Token;

/* ── Symbol table ─────────────────────────────────────────────────────────── */
typedef struct {
    char     name[256];
    uint32_t addr;
    int      is_global;
    int      defined;
    int      is_extern;  /* declared via .extern — linker must supply */
    int      is_label;   /* defined as a code/data label (not .equ constant) */
} Symbol;

static Symbol   symtab[MAX_SYMS];
static int      sym_count = 0;

/* ── Line storage ─────────────────────────────────────────────────────────── */
typedef struct {
    char text[MAX_LINE_LEN];
    int  lineno;
    char filename[256];
} Line;

static Line     lines[MAX_LINES];
static int      line_count = 0;

/* ── Output buffer ─────────────────────────────────────────────────────────── */
static uint8_t  outbuf[MAX_OUTPUT];
static uint32_t org_addr    = 0;    /* current assembly address */
static uint32_t write_pos   = 0;    /* byte offset into outbuf  */
static uint32_t max_write_pos = 0;

/* ── Object-file mode state ──────────────────────────────────────────────── */
#define MAX_RELOCS 65536
typedef struct {
    uint32_t offset;    /* section-relative byte offset of the site */
    uint32_t sym_index;
    uint8_t  type;      /* RELOC_* */
    int32_t  addend;
} ObjReloc;

static int      g_object_mode     = 0;
static uint32_t g_section_base    = 0;  /* absolute addr of section start (.org) */
static uint32_t g_section_vma_hint= 0;  /* preferred load address for linker     */
static ObjReloc reloc_tab[MAX_RELOCS];
static int      reloc_count       = 0;

/* ── Error tracking ─────────────────────────────────────────────────────────── */
static int      error_count = 0;
static const char *cur_filename = "<input>";
static char g_include_dir[1024] = ""; /* directory for .include lookups */
static int      cur_lineno  = 0;

static char *read_file(const char *path); /* forward declaration */

static void asm_error(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s:%d: error: ", cur_filename, cur_lineno);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    error_count++;
}

/* ── Opcode table ─────────────────────────────────────────────────────────── */
typedef enum {
    FMT_R3,        /* rd, rs1, rs2 */
    FMT_R2,        /* rd, rs1 */
    FMT_RSHIFT,    /* rd, rs1, shift5 */
    FMT_I_SIGNED,  /* rd, rs1, simm16 */
    FMT_I_UNSIGNED,/* rd, rs1, imm16 (zero-ext) */
    FMT_I_SHIFT,   /* rd, rs1, imm5  */
    FMT_MEM,       /* rd, rs1, imm16 or rd, [rs1+imm] */
    FMT_BRANCH,    /* cond implicit, offset */
    FMT_JUMP,      /* rd, offset */
    FMT_MOVI,      /* rd, imm16 zero-ext */
    FMT_MOVHI,     /* rd, imm16 zero-ext */
    FMT_NONE,      /* no operands */
    FMT_MOVI32,    /* pseudo: rd, val32 */
    FMT_CMP,       /* rs1, rs2  (no rd — flags only) */
    FMT_CMPI,      /* rs1, simm16 (no rd — flags only) */
    FMT_RET,       /* pseudo: no operands → MOV pc, lr  */
    FMT_BR,        /* pseudo: rs1         → MOV pc, rs1 */
    FMT_CALL,      /* pseudo: target      → JMP lr, target */
    FMT_PUSH,      /* pseudo: rd          → ADDI sp,-4 + SW rd,[sp] */
    FMT_POP,       /* pseudo: rd          → LW rd,[sp] + ADDI sp,4 */
    FMT_MEM_IDX    /* rd, [rs1+rs2] — indexed memory (base + register offset) */
} InstrFmt;

typedef struct {
    const char *mnemonic;
    uint8_t     opcode;
    InstrFmt    fmt;
    int         cond;   /* branch condition, -1 if not branch */
} OpcodeEntry;

static const OpcodeEntry opcode_table[] = {
    { "ADD",     0x00, FMT_R3,        -1 },
    { "SUB",     0x01, FMT_R3,        -1 },
    { "AND",     0x02, FMT_R3,        -1 },
    { "OR",      0x03, FMT_R3,        -1 },
    { "XOR",     0x04, FMT_R3,        -1 },
    { "NOT",     0x05, FMT_R2,        -1 },
    { "LSL",     0x06, FMT_RSHIFT,    -1 },
    { "LSR",     0x07, FMT_RSHIFT,    -1 },
    { "ASR",     0x08, FMT_RSHIFT,    -1 },
    { "MUL",     0x09, FMT_R3,        -1 },
    { "DIV",     0x0A, FMT_R3,        -1 },
    { "LW",      0x0B, FMT_MEM,       -1 },
    { "SW",      0x0C, FMT_MEM,       -1 },
    /* Branches — all opcode 0x0D */
    { "BEQ",     0x0D, FMT_BRANCH,     0 },
    { "BNE",     0x0D, FMT_BRANCH,     1 },
    { "BLT",     0x0D, FMT_BRANCH,     2 },
    { "BGE",     0x0D, FMT_BRANCH,     3 },
    { "BLTU",    0x0D, FMT_BRANCH,     4 },
    { "BGEU",    0x0D, FMT_BRANCH,     5 },
    { "BA",      0x0D, FMT_BRANCH,     6 },
    { "BGT",     0x0D, FMT_BRANCH,     7 },
    { "BLE",     0x0D, FMT_BRANCH,     8 },
    { "BGTU",    0x0D, FMT_BRANCH,     9 },
    { "BLEU",    0x0D, FMT_BRANCH,    10 },
    { "JMP",     0x0E, FMT_JUMP,      -1 },
    { "MOVI",    0x0F, FMT_MOVI,      -1 },
    { "SYSCALL", 0x10, FMT_NONE,      -1 },
    { "SYSRET",  0x11, FMT_NONE,      -1 },
    { "HALT",    0x12, FMT_NONE,      -1 },
    { "MOVHI",   0x13, FMT_MOVHI,     -1 },
    { "ADDI",    0x14, FMT_I_SIGNED,  -1 },
    { "SUBI",    0x15, FMT_I_SIGNED,  -1 },
    { "ANDI",    0x16, FMT_I_UNSIGNED,-1 },
    { "ORI",     0x17, FMT_I_UNSIGNED,-1 },
    { "XORI",    0x18, FMT_I_UNSIGNED,-1 },
    { "LSLI",    0x19, FMT_I_SHIFT,   -1 },
    { "LSRI",    0x1A, FMT_I_SHIFT,   -1 },
    { "ASRI",    0x1B, FMT_I_SHIFT,   -1 },
    { "LH",      0x1C, FMT_MEM,       -1 },
    { "LHU",     0x1D, FMT_MEM,       -1 },
    { "LB",      0x1E, FMT_MEM,       -1 },
    { "LBU",     0x1F, FMT_MEM,       -1 },
    { "SH",      0x20, FMT_MEM,       -1 },
    { "SB",      0x21, FMT_MEM,       -1 },
    { "MULH",    0x22, FMT_R3,        -1 },
    { "MULHU",   0x23, FMT_R3,        -1 },
    { "DIVU",    0x24, FMT_R3,        -1 },
    { "MOD",     0x25, FMT_R3,        -1 },
    { "MODU",    0x26, FMT_R3,        -1 },
    { "MOV",     0x27, FMT_R2,        -1 },
    { "CMP",     0x28, FMT_CMP,       -1 },
    { "CMPI",    0x29, FMT_CMPI,      -1 },
    { "CALLR",   0x2A, FMT_R2,        -1 },   /* rd = PC+4; PC = rs1 */
    { "ADDC",    0x2B, FMT_R3,        -1 },
    { "SUBC",    0x2C, FMT_R3,        -1 },
    { "LSLR",    0x2D, FMT_R3,        -1 },
    { "LSRR",    0x2E, FMT_R3,        -1 },
    { "ASRR",    0x2F, FMT_R3,        -1 },
    { "LWX",     0x30, FMT_MEM_IDX,  -1 },
    { "LBX",     0x31, FMT_MEM_IDX,  -1 },
    { "LBUX",    0x32, FMT_MEM_IDX,  -1 },
    { "SWX",     0x33, FMT_MEM_IDX,  -1 },
    { "SBX",     0x34, FMT_MEM_IDX,  -1 },
    { "LHX",     0x35, FMT_MEM_IDX,  -1 },
    { "LHUX",    0x36, FMT_MEM_IDX,  -1 },
    { "SHX",     0x37, FMT_MEM_IDX,  -1 },
    { "LUI",     0x38, FMT_MOVI,     -1 },
    { "ROLR",    0x39, FMT_R3,       -1 },
    { "RORR",    0x3A, FMT_R3,       -1 },
    { "ROLI",    0x3B, FMT_RSHIFT,   -1 },
    { "RORI",    0x3C, FMT_RSHIFT,   -1 },
    { "CAS",     0x3D, FMT_R3,       -1 },
    /* Pseudo-instructions */
    { "RET",     0x27, FMT_RET,       -1 },   /* MOV pc, lr  */
    { "BR",      0x27, FMT_BR,        -1 },   /* MOV pc, rs1 */
    { "CALL",    0x0E, FMT_CALL,      -1 },   /* JMP lr, target */
    { "NOP",     0x0F, FMT_NONE,      -1 },   /* MOVI r0, 0 */
    { "PUSH",    0x00, FMT_PUSH,      -1 },   /* ADDI sp,-4 + SW rd,[sp] */
    { "POP",     0x00, FMT_POP,       -1 },   /* LW rd,[sp] + ADDI sp,4 */
    { "MOVI32",  0x00, FMT_MOVI32,    -1 },
    { NULL, 0, FMT_NONE, -1 }
};

static const OpcodeEntry *find_opcode(const char *mnem) {
    /* case-insensitive search */
    for (int i = 0; opcode_table[i].mnemonic; i++) {
        const char *a = opcode_table[i].mnemonic;
        const char *b = mnem;
        int match = 1;
        while (*a || *b) {
            if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) { match = 0; break; }
            a++; b++;
        }
        if (match) return &opcode_table[i];
    }
    return NULL;
}

/* ── Register table ─────────────────────────────────────────────────────────── */
static int parse_register(const char *s) {
    /* r0-r15 */
    if ((s[0]=='r'||s[0]=='R') && s[1]) {
        char *end;
        long n = strtol(s+1, &end, 10);
        if (*end == '\0' && n >= 0 && n <= 15) return (int)n;
    }
    /* aliases */
    if (strcasecmp(s, "sp") == 0) return 13;
    if (strcasecmp(s, "lr") == 0) return 14;
    if (strcasecmp(s, "pc") == 0) return 15;
    return -1;
}

/* ── Symbol table helpers ─────────────────────────────────────────────────── */
static Symbol *find_symbol(const char *name) {
    for (int i = 0; i < sym_count; i++)
        if (strcmp(symtab[i].name, name) == 0)
            return &symtab[i];
    return NULL;
}

static Symbol *add_symbol(const char *name) {
    if (sym_count >= MAX_SYMS) { asm_error("symbol table full"); return NULL; }
    Symbol *s = &symtab[sym_count++];
    strncpy(s->name, name, sizeof(s->name)-1);
    s->name[sizeof(s->name)-1] = '\0';
    s->addr      = 0;
    s->is_global = 0;
    s->defined   = 0;
    s->is_extern = 0;
    s->is_label  = 0;
    return s;
}

static int find_sym_index(const char *name) {
    for (int i = 0; i < sym_count; i++)
        if (strcmp(symtab[i].name, name) == 0) return i;
    return -1;
}

/* In object mode, check if toks[pos] is a relocatable symbol reference.
   Returns symbol index (>=0) on success, sets *addend to any +/- constant.
   Returns -1 if it is a pure constant, register, or .equ value. */
static int peek_sym_ref(Token *toks, int ntoks, int pos, int64_t *addend) {
    *addend = 0;
    if (pos >= ntoks) return -1;
    if (toks[pos].type != TOK_IDENT) return -1;
    if (parse_register(toks[pos].str) >= 0) return -1;  /* it's a register */
    int idx = find_sym_index(toks[pos].str);
    if (idx < 0) return -1;
    Symbol *s = &symtab[idx];
    if (!s->is_extern && !s->is_label) return -1;  /* .equ constant, no reloc */
    /* optional addend: + INT or - INT */
    if (pos+2 < ntoks && toks[pos+1].type == TOK_PLUS  && toks[pos+2].type == TOK_INT)
        *addend = toks[pos+2].ival;
    else if (pos+2 < ntoks && toks[pos+1].type == TOK_MINUS && toks[pos+2].type == TOK_INT)
        *addend = -(int64_t)toks[pos+2].ival;
    return idx;
}

static void emit_reloc(uint8_t type, int sym_idx, int32_t addend) {
    if (reloc_count >= MAX_RELOCS) { asm_error("relocation table full"); return; }
    ObjReloc *r  = &reloc_tab[reloc_count++];
    r->offset    = write_pos - g_section_base;
    r->sym_index = (uint32_t)sym_idx;
    r->type      = type;
    r->addend    = addend;
}

/* ── Little-endian file helpers (used by write_dobj) ────────────────────── */
static void wle32(FILE *f, uint32_t v) {
    fputc( v     &0xFF,f); fputc((v>> 8)&0xFF,f);
    fputc((v>>16)&0xFF,f); fputc((v>>24)&0xFF,f);
}
static void wle16(FILE *f, uint16_t v) {
    fputc(v&0xFF,f); fputc((v>>8)&0xFF,f);
}

/* Serialise assembled data to a .dobj object file. */
static void write_dobj(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write '%s'\n", path); return; }

    /* Section data slice: outbuf[g_section_base .. max_write_pos] */
    uint32_t sec_data_start = g_section_base;
    uint32_t sec_size = (max_write_pos > sec_data_start)
                        ? (max_write_pos - sec_data_start) : 0;

    /* File layout sizes */
    uint32_t n_sym  = (uint32_t)sym_count;
    uint32_t n_rel  = (uint32_t)reloc_count;
    uint32_t data_off = DOBJ_HEADER_SIZE + DOBJ_SECTION_SIZE
                      + n_sym * DOBJ_SYMBOL_SIZE
                      + n_rel * DOBJ_RELOC_SIZE;

    /* ── Header ── */
    fwrite(DOBJ_MAGIC, 1, 4, f);
    fputc(DOBJ_VERSION,f); fputc(0,f); fputc(0,f); fputc(0,f);  /* pad */
    wle32(f, 1);           /* num_sections */
    wle32(f, n_sym);
    wle32(f, n_rel);
    wle32(f, data_off);

    /* ── Section header (.text) ── */
    { char n[8]; memset(n,0,8); memcpy(n,".text",5); fwrite(n,1,8,f); }
    wle32(f, 0);                              /* offset within data blob */
    wle32(f, sec_size);
    wle32(f, g_section_vma_hint);
    wle32(f, DOBJ_SF_EXEC | DOBJ_SF_ALLOC);

    /* ── Symbols ── */
    for (int i = 0; i < sym_count; i++) {
        Symbol *s = &symtab[i];
        char n[28]; memset(n,0,28);
        strncpy(n, s->name, 27);
        fwrite(n, 1, 28, f);
        /* value: section-relative offset; 0 for externs */
        uint32_t val = s->is_extern ? 0 : (s->addr - g_section_base);
        wle32(f, val);
        wle16(f, s->is_extern ? (uint16_t)DOBJ_SEC_UNDEF : (uint16_t)0);
        fputc(s->is_global ? 1 : 0, f);
        fputc(0, f);  /* pad */
    }

    /* ── Relocations ── */
    for (int i = 0; i < reloc_count; i++) {
        ObjReloc *r = &reloc_tab[i];
        wle32(f, r->offset);
        wle32(f, r->sym_index);
        fputc(r->type, f);
        fputc(0, f);  /* section_index = 0 (single .text section) */
        fputc(0,f); fputc(0,f);  /* pad */
        wle32(f, (uint32_t)r->addend);
    }

    /* ── Section data ── */
    if (sec_size > 0)
        fwrite(outbuf + sec_data_start, 1, sec_size, f);

    fclose(f);
}

/* ── Tokenizer ────────────────────────────────────────────────────────────── */
static const char *tok_ptr;

static void skip_ws(void) {
    while (*tok_ptr == ' ' || *tok_ptr == '\t') tok_ptr++;
}

static int next_token(Token *t) {
    skip_ws();
    if (*tok_ptr == '\0' || *tok_ptr == '\n' || *tok_ptr == ';') {
        t->type = TOK_EOF;
        return 0;
    }
    char c = *tok_ptr;

    /* Comment */
    if (c == ';') { t->type = TOK_EOF; return 0; }

    /* Comma */
    if (c == ',') { tok_ptr++; t->type = TOK_COMMA; return 1; }
    if (c == '[') { tok_ptr++; t->type = TOK_LBRACKET; return 1; }
    if (c == ']') { tok_ptr++; t->type = TOK_RBRACKET; return 1; }
    if (c == '+') { tok_ptr++; t->type = TOK_PLUS; return 1; }
    if (c == '-') { tok_ptr++; t->type = TOK_MINUS; return 1; }
    if (c == '>') {
        if (tok_ptr[1] == '>') {
            tok_ptr += 2; t->type = TOK_RSHIFT; return 1;
        }
        asm_error("unexpected '>'");
        tok_ptr++;
        return 0;
    }
    /* String literal */
    if (c == '"') {
        tok_ptr++;
        int i = 0;
        while (*tok_ptr && *tok_ptr != '"' && i < (int)sizeof(t->str)-1) {
            if (*tok_ptr == '\\') {
                tok_ptr++;
                switch (*tok_ptr) {
                    case 'n': t->str[i++] = '\n'; break;
                    case 'r': t->str[i++] = '\r'; break;
                    case 't': t->str[i++] = '\t'; break;
                    case '0': t->str[i++] = '\0'; break;
                    case '\\': t->str[i++] = '\\'; break;
                    case '"': t->str[i++] = '"'; break;
                    default: t->str[i++] = *tok_ptr; break;
                }
            } else {
                t->str[i++] = *tok_ptr;
            }
            tok_ptr++;
        }
        if (*tok_ptr == '"') tok_ptr++;
        t->str[i] = '\0';
        t->type = TOK_STRING;
        return 1;
    }
    /* Dot-ident (.word etc.) */
    if (c == '.') {
        tok_ptr++;
        int i = 0;
        t->str[i++] = '.';
        while (isalnum((unsigned char)*tok_ptr) || *tok_ptr == '_') {
            if (i < (int)sizeof(t->str)-1) t->str[i++] = *tok_ptr;
            tok_ptr++;
        }
        t->str[i] = '\0';
        t->type = TOK_DOT_IDENT;
        return 1;
    }
    /* Number */
    if (isdigit((unsigned char)c) || (c == '0' && (tok_ptr[1]=='x'||tok_ptr[1]=='X'))) {
        char *end;
        t->ival = (int64_t)strtoull(tok_ptr, &end, 0);
        tok_ptr = end;
        t->type = TOK_INT;
        /* Copy textual representation */
        t->str[0] = '\0';
        return 1;
    }
    /* Negative number handled by MINUS token + number */
    /* Identifier */
    if (isalpha((unsigned char)c) || c == '_') {
        int i = 0;
        while (isalnum((unsigned char)*tok_ptr) || *tok_ptr == '_') {
            if (i < (int)sizeof(t->str)-1) t->str[i++] = *tok_ptr;
            tok_ptr++;
        }
        t->str[i] = '\0';
        t->ival = 0;
        t->type = TOK_IDENT;
        /* Check for colon immediately following (label definition) */
        skip_ws();
        if (*tok_ptr == ':') {
            tok_ptr++;
            t->type = TOK_COLON;
        }
        return 1;
    }
    asm_error("unexpected character '%c'", c);
    tok_ptr++;
    return 0;
}

/* Tokenize a full line into a token array (stops at EOF/comment/newline).
   Returns number of tokens. */
static int tokenize_line(const char *line, Token *toks, int max_toks) {
    tok_ptr = line;
    int n = 0;
    while (n < max_toks) {
        Token t;
        if (!next_token(&t)) break;
        if (t.type == TOK_EOF) break;
        toks[n++] = t;
    }
    return n;
}

/* ── Expression evaluator ─────────────────────────────────────────────────── */
/* Evaluate a simple expression from tokens starting at *pos.
   Supports: integer literal, label, label+int, label-int, value>>int
   Returns 1 on success, 0 on error. */
static int eval_expr(Token *toks, int ntoks, int *pos, int pass, int64_t *result) {
    if (*pos >= ntoks) { asm_error("expected expression"); return 0; }

    int64_t val = 0;
    int neg = 0;

    /* Optional leading minus */
    if (toks[*pos].type == TOK_MINUS) {
        neg = 1;
        (*pos)++;
        if (*pos >= ntoks) { asm_error("expected value after '-'"); return 0; }
    }

    Token *t = &toks[*pos];
    if (t->type == TOK_INT) {
        val = t->ival;
        (*pos)++;
    } else if (t->type == TOK_IDENT || t->type == TOK_COLON) {
        /* label reference — t->str holds name, type might be IDENT */
        Symbol *s = find_symbol(t->str);
        if (!s) {
            if (pass == 2) {
                asm_error("undefined symbol '%s'", t->str);
                return 0;
            }
            /* pass 1: use 0 as placeholder */
            val = 0;
        } else {
            val = (int64_t)s->addr;
        }
        (*pos)++;
    } else {
        asm_error("expected integer or label");
        return 0;
    }

    if (neg) val = -val;

    /* Optional binary operator: +, -, >> */
    while (*pos < ntoks) {
        TokType op = toks[*pos].type;
        if (op == TOK_PLUS || op == TOK_MINUS || op == TOK_RSHIFT) {
            (*pos)++;
            if (*pos >= ntoks) { asm_error("expected operand after operator"); return 0; }
            int64_t rhs = 0;
            Token *rt = &toks[*pos];
            if (rt->type == TOK_INT) {
                rhs = rt->ival;
                (*pos)++;
            } else if (rt->type == TOK_IDENT) {
                Symbol *s2 = find_symbol(rt->str);
                if (!s2) {
                    if (pass == 2) { asm_error("undefined symbol '%s'", rt->str); return 0; }
                    rhs = 0;
                } else {
                    rhs = (int64_t)s2->addr;
                }
                (*pos)++;
            } else {
                asm_error("expected integer after operator");
                return 0;
            }
            if (op == TOK_PLUS) val += rhs;
            else if (op == TOK_MINUS) val -= rhs;
            else val >>= rhs;
        } else {
            break;
        }
    }

    *result = val;
    return 1;
}

/* ── Emit helpers ─────────────────────────────────────────────────────────── */
static void emit_byte(uint8_t b) {
    if (write_pos < MAX_OUTPUT) {
        outbuf[write_pos] = b;
        write_pos++;
        if (write_pos > max_write_pos) max_write_pos = write_pos;
    } else {
        asm_error("output buffer overflow");
    }
    org_addr++;
}

static void emit_word(uint32_t w) {
    emit_byte( w        & 0xFF);
    emit_byte((w >>  8) & 0xFF);
    emit_byte((w >> 16) & 0xFF);
    emit_byte((w >> 24) & 0xFF);
}

/* ── Instruction size calculator ─────────────────────────────────────────── */
/* Returns the number of bytes this line will emit (for pass 1 address tracking).
   Directives with variable size need parsing. */
static int directive_size(const char *line) {
    Token toks[MAX_TOKS];
    tok_ptr = line;
    int n = tokenize_line(line, toks, MAX_TOKS);
    if (n == 0) return 0;

    /* Skip label if present */
    int i = 0;
    if (toks[i].type == TOK_COLON) i++;

    if (i >= n || toks[i].type != TOK_DOT_IDENT) return 0;

    const char *dir = toks[i].str;
    i++;

    if (strcasecmp(dir, ".org") == 0) return 0; /* handled specially */
    if (strcasecmp(dir, ".global") == 0) return 0;
    if (strcasecmp(dir, ".align") == 0) return 0; /* handled specially */
    if (strcasecmp(dir, ".equ") == 0) return 0; /* handled specially — no bytes */
    if (strcasecmp(dir, ".word") == 0) return 4;
    if (strcasecmp(dir, ".byte") == 0) return 1;
    if (strcasecmp(dir, ".space") == 0) {
        if (i < n && toks[i].type == TOK_INT) return (int)toks[i].ival;
        return 0;
    }
    if (strcasecmp(dir, ".ascii") == 0) {
        if (i < n && toks[i].type == TOK_STRING) return (int)strlen(toks[i].str);
        return 0;
    }
    if (strcasecmp(dir, ".asciiz") == 0) {
        if (i < n && toks[i].type == TOK_STRING) return (int)strlen(toks[i].str) + 1;
        return 0;
    }
    return 0;
}

/* ── Core assembler passes ────────────────────────────────────────────────── */

/* Returns instruction size in bytes for a given mnemonic (4 or 8 for MOVI32) */
static int instr_size(const char *mnem) {
    if (strcasecmp(mnem, "movi32") == 0) return 8;
    if (strcasecmp(mnem, "push")   == 0) return 8;
    if (strcasecmp(mnem, "pop")    == 0) return 8;
    return 4;
}

static void process_directive(Token *toks, int ntoks, int start, int pass) {
    if (start >= ntoks) return;
    const char *dir = toks[start].str;
    int i = start + 1;

    if (strcasecmp(dir, ".org") == 0) {
        if (i >= ntoks || toks[i].type != TOK_INT) {
            asm_error(".org requires integer argument");
            return;
        }
        uint32_t new_addr = (uint32_t)toks[i].ival;
        if (pass == 2) {
            /* Pad if needed */
            uint32_t base = org_addr - write_pos; /* start of outbuf */
            (void)base;
            /* write_pos needs to advance to new_addr; zero-fill gap */
            if (new_addr > org_addr) {
                uint32_t gap = new_addr - org_addr;
                for (uint32_t g = 0; g < gap; g++) emit_byte(0);
            } else if (new_addr < org_addr) {
                asm_error(".org moves address backward (0x%X -> 0x%X)", org_addr, new_addr);
            }
            /* also update write_pos to match */
        }
        org_addr = new_addr;
        if (pass == 2) write_pos = new_addr;
        return;
    }

    if (strcasecmp(dir, ".extern") == 0) {
        /* Already handled in pass 1; nothing to emit. */
        return;
    }

    if (strcasecmp(dir, ".section") == 0) {
        /* Silently accepted in object mode. */
        return;
    }

    if (strcasecmp(dir, ".global") == 0) {
        if (i < ntoks && toks[i].type == TOK_IDENT) {
            Symbol *s = find_symbol(toks[i].str);
            if (!s) s = add_symbol(toks[i].str);
            if (s) s->is_global = 1;
        }
        return;
    }

    if (strcasecmp(dir, ".word") == 0) {
        if (i >= ntoks) { asm_error(".word requires argument"); return; }
        if (g_object_mode && pass == 2) {
            int64_t addend = 0;
            int sidx = peek_sym_ref(toks, ntoks, i, &addend);
            if (sidx >= 0) {
                emit_reloc(RELOC_ABS32, sidx, (int32_t)addend);
                emit_word(0);
                return;
            }
        }
        int64_t val = 0;
        eval_expr(toks, ntoks, &i, pass, &val);
        if (pass == 2) emit_word((uint32_t)val);
        else { org_addr += 4; }
        return;
    }

    if (strcasecmp(dir, ".byte") == 0) {
        if (i >= ntoks) { asm_error(".byte requires argument"); return; }
        int64_t val = 0;
        eval_expr(toks, ntoks, &i, pass, &val);
        if (pass == 2) emit_byte((uint8_t)val);
        else { org_addr += 1; }
        return;
    }

    if (strcasecmp(dir, ".space") == 0) {
        if (i >= ntoks || toks[i].type != TOK_INT) { asm_error(".space requires integer"); return; }
        uint32_t n = (uint32_t)toks[i].ival;
        if (pass == 2) { for (uint32_t j = 0; j < n; j++) emit_byte(0); }
        else { org_addr += n; }
        return;
    }

    if (strcasecmp(dir, ".ascii") == 0) {
        if (i >= ntoks || toks[i].type != TOK_STRING) { asm_error(".ascii requires string"); return; }
        const char *s = toks[i].str;
        uint32_t len = (uint32_t)strlen(s);
        if (pass == 2) { for (uint32_t j = 0; j < len; j++) emit_byte((uint8_t)s[j]); }
        else { org_addr += len; }
        return;
    }

    if (strcasecmp(dir, ".asciiz") == 0) {
        if (i >= ntoks || toks[i].type != TOK_STRING) { asm_error(".asciiz requires string"); return; }
        const char *s = toks[i].str;
        uint32_t len = (uint32_t)strlen(s);
        if (pass == 2) { for (uint32_t j = 0; j <= len; j++) emit_byte((uint8_t)s[j]); }
        else { org_addr += len + 1; }
        return;
    }

    if (strcasecmp(dir, ".align") == 0) {
        if (i >= ntoks || toks[i].type != TOK_INT) { asm_error(".align requires integer argument"); return; }
        uint32_t align_val = (uint32_t)toks[i].ival;
        if (align_val > 1) {
            uint32_t mask = align_val - 1;
            uint32_t new_addr = (org_addr + mask) & ~mask;
            uint32_t gap = new_addr - org_addr;
            if (pass == 2) { for (uint32_t g = 0; g < gap; g++) emit_byte(0); }
            else { org_addr = new_addr; }
        }
        return;
    }

    if (strcasecmp(dir, ".equ") == 0) {
        /* .equ NAME, VALUE — no bytes emitted; re-evaluate on pass 2 for forward refs */
        if (i >= ntoks || toks[i].type != TOK_IDENT) { asm_error(".equ requires symbol name"); return; }
        const char *sym_name = toks[i].str;
        i++;
        if (i >= ntoks || toks[i].type != TOK_COMMA) { asm_error(".equ requires comma after name"); return; }
        i++;
        int64_t val = 0;
        if (!eval_expr(toks, ntoks, &i, pass, &val)) return;
        Symbol *s = find_symbol(sym_name);
        if (!s) s = add_symbol(sym_name);
        if (s) { s->addr = (uint32_t)val; s->defined = 1; }
        return;
    }

    asm_error("unknown directive '%s'", dir);
}

/* Parse memory operand: rd, [rs1+imm] or rd, [rs1-imm] or rd, [rs1] or rd, rs1, imm
   After comma has already been consumed.
   Returns 1 on success. rd already parsed (passed in). */
static int parse_mem_operand(Token *toks, int ntoks, int *pos,
                              int pass, int *prs1, int64_t *pimm) {
    if (*pos >= ntoks) { asm_error("expected memory operand"); return 0; }

    if (toks[*pos].type == TOK_LBRACKET) {
        /* bracket notation: [rs1+imm], [rs1-imm], or [rs1] */
        (*pos)++;
        if (*pos >= ntoks || toks[*pos].type != TOK_IDENT) {
            asm_error("expected register in [...]");
            return 0;
        }
        int rs1 = parse_register(toks[*pos].str);
        if (rs1 < 0) { asm_error("expected register, got '%s'", toks[*pos].str); return 0; }
        (*pos)++;
        int64_t imm = 0;
        if (*pos < ntoks && toks[*pos].type == TOK_PLUS) {
            (*pos)++;
            if (!eval_expr(toks, ntoks, pos, pass, &imm)) return 0;
        } else if (*pos < ntoks && toks[*pos].type == TOK_MINUS) {
            (*pos)++;
            if (!eval_expr(toks, ntoks, pos, pass, &imm)) return 0;
            imm = -imm;
        }
        if (*pos >= ntoks || toks[*pos].type != TOK_RBRACKET) {
            asm_error("expected ']'");
            return 0;
        }
        (*pos)++;
        *prs1 = rs1;
        *pimm = imm;
        return 1;
    } else {
        /* comma-separated: rs1, imm */
        if (toks[*pos].type != TOK_IDENT) {
            asm_error("expected register");
            return 0;
        }
        int rs1 = parse_register(toks[*pos].str);
        if (rs1 < 0) { asm_error("expected register, got '%s'", toks[*pos].str); return 0; }
        (*pos)++;
        int64_t imm = 0;
        if (*pos < ntoks && toks[*pos].type == TOK_COMMA) {
            (*pos)++;
            if (!eval_expr(toks, ntoks, pos, pass, &imm)) return 0;
        }
        *prs1 = rs1;
        *pimm = imm;
        return 1;
    }
}

static void assemble_line(Token *toks, int ntoks, int pass) {
    int i = 0;

    /* Skip label definitions */
    while (i < ntoks && toks[i].type == TOK_COLON) i++;

    if (i >= ntoks) return; /* empty or label-only line */

    /* Directive? */
    if (toks[i].type == TOK_DOT_IDENT) {
        process_directive(toks, ntoks, i, pass);
        return;
    }

    /* Must be an instruction mnemonic */
    if (toks[i].type != TOK_IDENT) {
        asm_error("expected mnemonic, got token type %d ('%s')", toks[i].type, toks[i].str);
        return;
    }

    const char *mnem = toks[i].str;
    const OpcodeEntry *op = find_opcode(mnem);
    if (!op) {
        asm_error("unknown mnemonic '%s'", mnem);
        return;
    }
    i++;

    uint32_t instr_addr = org_addr;

    switch (op->fmt) {
    case FMT_NONE: {
        uint32_t enc = ((uint32_t)op->opcode << 24);
        if (pass == 2) emit_word(enc);
        else org_addr += 4;
        break;
    }
    case FMT_R3: {
        /* rd, rs1, rs2 */
        if (i >= ntoks || toks[i].type != TOK_IDENT) { asm_error("expected rd"); if(pass==1)org_addr+=4; return; }
        int rd = parse_register(toks[i].str);
        if (rd < 0) { asm_error("bad register '%s'", toks[i].str); if(pass==1)org_addr+=4; return; }
        i++;
        if (i >= ntoks || toks[i].type != TOK_COMMA) { asm_error("expected ','"); if(pass==1)org_addr+=4; return; }
        i++;
        if (i >= ntoks || toks[i].type != TOK_IDENT) { asm_error("expected rs1"); if(pass==1)org_addr+=4; return; }
        int rs1 = parse_register(toks[i].str);
        if (rs1 < 0) { asm_error("bad register '%s'", toks[i].str); if(pass==1)org_addr+=4; return; }
        i++;
        if (i >= ntoks || toks[i].type != TOK_COMMA) { asm_error("expected ','"); if(pass==1)org_addr+=4; return; }
        i++;
        if (i >= ntoks || toks[i].type != TOK_IDENT) { asm_error("expected rs2"); if(pass==1)org_addr+=4; return; }
        int rs2 = parse_register(toks[i].str);
        if (rs2 < 0) { asm_error("bad register '%s'", toks[i].str); if(pass==1)org_addr+=4; return; }
        uint32_t enc = ((uint32_t)op->opcode<<24)|((uint32_t)rd<<20)|((uint32_t)rs1<<16)|((uint32_t)rs2<<12);
        if (pass == 2) emit_word(enc);
        else org_addr += 4;
        break;
    }
    case FMT_R2: {
        /* rd, rs1 */
        if (i >= ntoks || toks[i].type != TOK_IDENT) { asm_error("expected rd"); if(pass==1)org_addr+=4; return; }
        int rd = parse_register(toks[i].str);
        if (rd < 0) { asm_error("bad register '%s'", toks[i].str); if(pass==1)org_addr+=4; return; }
        i++;
        if (i >= ntoks || toks[i].type != TOK_COMMA) { asm_error("expected ','"); if(pass==1)org_addr+=4; return; }
        i++;
        if (i >= ntoks || toks[i].type != TOK_IDENT) { asm_error("expected rs1"); if(pass==1)org_addr+=4; return; }
        int rs1 = parse_register(toks[i].str);
        if (rs1 < 0) { asm_error("bad register '%s'", toks[i].str); if(pass==1)org_addr+=4; return; }
        uint32_t enc = ((uint32_t)op->opcode<<24)|((uint32_t)rd<<20)|((uint32_t)rs1<<16);
        if (pass == 2) emit_word(enc);
        else org_addr += 4;
        break;
    }
    case FMT_RSHIFT: {
        /* rd, rs1, shift5 */
        if (i >= ntoks || toks[i].type != TOK_IDENT) { asm_error("expected rd"); if(pass==1)org_addr+=4; return; }
        int rd = parse_register(toks[i].str);
        if (rd < 0) { asm_error("bad register '%s'", toks[i].str); if(pass==1)org_addr+=4; return; }
        i++;
        if (i >= ntoks || toks[i].type != TOK_COMMA) { asm_error("expected ','"); if(pass==1)org_addr+=4; return; }
        i++;
        if (i >= ntoks || toks[i].type != TOK_IDENT) { asm_error("expected rs1"); if(pass==1)org_addr+=4; return; }
        int rs1 = parse_register(toks[i].str);
        if (rs1 < 0) { asm_error("bad register '%s'", toks[i].str); if(pass==1)org_addr+=4; return; }
        i++;
        if (i >= ntoks || toks[i].type != TOK_COMMA) { asm_error("expected ','"); if(pass==1)org_addr+=4; return; }
        i++;
        int64_t shval = 0;
        if (!eval_expr(toks, ntoks, &i, pass, &shval)) { if(pass==1)org_addr+=4; return; }
        if (shval < 0 || shval > 31) { asm_error("shift amount %lld out of range 0..31", (long long)shval); if(pass==1)org_addr+=4; return; }
        uint32_t enc = ((uint32_t)op->opcode<<24)|((uint32_t)rd<<20)|((uint32_t)rs1<<16)|((uint32_t)shval<<7);
        if (pass == 2) emit_word(enc);
        else org_addr += 4;
        break;
    }
    case FMT_I_SIGNED: {
        /* rd, rs1, simm16 */
        if (i >= ntoks || toks[i].type != TOK_IDENT) { asm_error("expected rd"); if(pass==1)org_addr+=4; return; }
        int rd = parse_register(toks[i].str);
        if (rd < 0) { asm_error("bad register '%s'", toks[i].str); if(pass==1)org_addr+=4; return; }
        i++;
        if (i >= ntoks || toks[i].type != TOK_COMMA) { asm_error("expected ','"); if(pass==1)org_addr+=4; return; }
        i++;
        if (i >= ntoks || toks[i].type != TOK_IDENT) { asm_error("expected rs1"); if(pass==1)org_addr+=4; return; }
        int rs1 = parse_register(toks[i].str);
        if (rs1 < 0) { asm_error("bad register '%s'", toks[i].str); if(pass==1)org_addr+=4; return; }
        i++;
        if (i >= ntoks || toks[i].type != TOK_COMMA) { asm_error("expected ','"); if(pass==1)org_addr+=4; return; }
        i++;
        int64_t imm = 0;
        if (!eval_expr(toks, ntoks, &i, pass, &imm)) { if(pass==1)org_addr+=4; return; }
        if (pass == 2 && (imm < -32768 || imm > 32767)) {
            asm_error("signed immediate %lld out of range -32768..32767", (long long)imm);
            if(pass==1)org_addr+=4; return;
        }
        uint32_t enc = ((uint32_t)op->opcode<<24)|((uint32_t)rd<<20)|((uint32_t)rs1<<16)|((uint32_t)(imm&0xFFFF));
        if (pass == 2) emit_word(enc);
        else org_addr += 4;
        break;
    }
    case FMT_I_UNSIGNED: {
        /* rd, rs1, imm16 */
        if (i >= ntoks || toks[i].type != TOK_IDENT) { asm_error("expected rd"); if(pass==1)org_addr+=4; return; }
        int rd = parse_register(toks[i].str);
        if (rd < 0) { asm_error("bad register '%s'", toks[i].str); if(pass==1)org_addr+=4; return; }
        i++;
        if (i >= ntoks || toks[i].type != TOK_COMMA) { asm_error("expected ','"); if(pass==1)org_addr+=4; return; }
        i++;
        if (i >= ntoks || toks[i].type != TOK_IDENT) { asm_error("expected rs1"); if(pass==1)org_addr+=4; return; }
        int rs1 = parse_register(toks[i].str);
        if (rs1 < 0) { asm_error("bad register '%s'", toks[i].str); if(pass==1)org_addr+=4; return; }
        i++;
        if (i >= ntoks || toks[i].type != TOK_COMMA) { asm_error("expected ','"); if(pass==1)org_addr+=4; return; }
        i++;
        int64_t imm = 0;
        if (!eval_expr(toks, ntoks, &i, pass, &imm)) { if(pass==1)org_addr+=4; return; }
        if (pass == 2 && (imm < 0 || imm > 65535)) {
            asm_error("unsigned immediate %lld out of range 0..65535", (long long)imm);
            if(pass==1)org_addr+=4; return;
        }
        uint32_t enc = ((uint32_t)op->opcode<<24)|((uint32_t)rd<<20)|((uint32_t)rs1<<16)|((uint32_t)(imm&0xFFFF));
        if (pass == 2) emit_word(enc);
        else org_addr += 4;
        break;
    }
    case FMT_I_SHIFT: {
        /* rd, rs1, imm5 */
        if (i >= ntoks || toks[i].type != TOK_IDENT) { asm_error("expected rd"); if(pass==1)org_addr+=4; return; }
        int rd = parse_register(toks[i].str);
        if (rd < 0) { asm_error("bad register '%s'", toks[i].str); if(pass==1)org_addr+=4; return; }
        i++;
        if (i >= ntoks || toks[i].type != TOK_COMMA) { asm_error("expected ','"); if(pass==1)org_addr+=4; return; }
        i++;
        if (i >= ntoks || toks[i].type != TOK_IDENT) { asm_error("expected rs1"); if(pass==1)org_addr+=4; return; }
        int rs1 = parse_register(toks[i].str);
        if (rs1 < 0) { asm_error("bad register '%s'", toks[i].str); if(pass==1)org_addr+=4; return; }
        i++;
        if (i >= ntoks || toks[i].type != TOK_COMMA) { asm_error("expected ','"); if(pass==1)org_addr+=4; return; }
        i++;
        int64_t imm = 0;
        if (!eval_expr(toks, ntoks, &i, pass, &imm)) { if(pass==1)org_addr+=4; return; }
        if (pass == 2 && (imm < 0 || imm > 31)) {
            asm_error("shift immediate %lld out of range 0..31", (long long)imm);
            if(pass==1)org_addr+=4; return;
        }
        uint32_t enc = ((uint32_t)op->opcode<<24)|((uint32_t)rd<<20)|((uint32_t)rs1<<16)|((uint32_t)(imm&0xFFFF));
        if (pass == 2) emit_word(enc);
        else org_addr += 4;
        break;
    }
    case FMT_MOVI: {
        /* rd, imm16 (zero-extended, 0..65535) */
        if (i >= ntoks || toks[i].type != TOK_IDENT) { asm_error("expected rd"); if(pass==1)org_addr+=4; return; }
        int rd = parse_register(toks[i].str);
        if (rd < 0) { asm_error("bad register '%s'", toks[i].str); if(pass==1)org_addr+=4; return; }
        i++;
        if (i >= ntoks || toks[i].type != TOK_COMMA) { asm_error("expected ','"); if(pass==1)org_addr+=4; return; }
        i++;
        int64_t imm = 0;
        if (!eval_expr(toks, ntoks, &i, pass, &imm)) { if(pass==1)org_addr+=4; return; }
        if (pass == 2 && (imm < 0 || imm > 65535)) {
            asm_error("MOVI immediate %lld out of range 0..65535 (use MOVI32 for larger values)", (long long)imm);
            if(pass==1)org_addr+=4; return;
        }
        uint32_t enc = ((uint32_t)op->opcode<<24)|((uint32_t)rd<<20)|((uint32_t)(imm&0xFFFF));
        if (pass == 2) emit_word(enc);
        else org_addr += 4;
        break;
    }
    case FMT_MOVHI: {
        /* rd, imm16 */
        if (i >= ntoks || toks[i].type != TOK_IDENT) { asm_error("expected rd"); if(pass==1)org_addr+=4; return; }
        int rd = parse_register(toks[i].str);
        if (rd < 0) { asm_error("bad register '%s'", toks[i].str); if(pass==1)org_addr+=4; return; }
        i++;
        if (i >= ntoks || toks[i].type != TOK_COMMA) { asm_error("expected ','"); if(pass==1)org_addr+=4; return; }
        i++;
        int64_t imm = 0;
        if (!eval_expr(toks, ntoks, &i, pass, &imm)) { if(pass==1)org_addr+=4; return; }
        if (pass == 2 && (imm < 0 || imm > 65535)) {
            asm_error("MOVHI immediate %lld out of range 0..65535", (long long)imm);
            if(pass==1)org_addr+=4; return;
        }
        uint32_t enc = ((uint32_t)op->opcode<<24)|((uint32_t)rd<<20)|((uint32_t)(imm&0xFFFF));
        if (pass == 2) emit_word(enc);
        else org_addr += 4;
        break;
    }
    case FMT_MEM: {
        /* rd, [rs1+imm] or rd, rs1, imm */
        if (i >= ntoks || toks[i].type != TOK_IDENT) { asm_error("expected rd"); if(pass==1)org_addr+=4; return; }
        int rd = parse_register(toks[i].str);
        if (rd < 0) { asm_error("bad register '%s'", toks[i].str); if(pass==1)org_addr+=4; return; }
        i++;
        if (i >= ntoks || toks[i].type != TOK_COMMA) { asm_error("expected ','"); if(pass==1)org_addr+=4; return; }
        i++;
        int rs1 = 0;
        int64_t imm = 0;
        if (!parse_mem_operand(toks, ntoks, &i, pass, &rs1, &imm)) { if(pass==1)org_addr+=4; return; }
        uint32_t enc = ((uint32_t)op->opcode<<24)|((uint32_t)rd<<20)|((uint32_t)rs1<<16)|((uint32_t)(imm&0xFFFF));
        if (pass == 2) emit_word(enc);
        else org_addr += 4;
        break;
    }
    case FMT_BRANCH: {
        /* cond from mnemonic, offset (label or literal) */
        if (i >= ntoks) { asm_error("expected branch target"); if(pass==1)org_addr+=4; return; }
        /* Object mode: extern targets need a relocation (local branches are
           already PC-relative and position-independent within the section). */
        if (g_object_mode && pass == 2) {
            int64_t addend = 0;
            int sidx = peek_sym_ref(toks, ntoks, i, &addend);
            if (sidx >= 0 && symtab[sidx].is_extern) {
                emit_reloc(RELOC_BRANCH20, sidx, (int32_t)addend);
                emit_word(((uint32_t)0x0D<<24)|((uint32_t)op->cond<<20)|0);
                break;
            }
        }
        int64_t target = 0;
        if (!eval_expr(toks, ntoks, &i, pass, &target)) { if(pass==1)org_addr+=4; return; }
        int64_t offset = target - (int64_t)instr_addr;
        if (pass == 2 && (offset < -524288 || offset > 524287)) {
            asm_error("branch offset %lld out of range ±524288", (long long)offset);
            if(pass==1)org_addr+=4; return;
        }
        uint32_t enc = ((uint32_t)0x0D<<24)|((uint32_t)op->cond<<20)|((uint32_t)(offset & 0xFFFFF));
        if (pass == 2) emit_word(enc);
        else org_addr += 4;
        break;
    }
    case FMT_JUMP: {
        /* rd, offset (label or literal) */
        if (i >= ntoks || toks[i].type != TOK_IDENT) { asm_error("expected rd"); if(pass==1)org_addr+=4; return; }
        int rd = parse_register(toks[i].str);
        if (rd < 0) { asm_error("bad register '%s'", toks[i].str); if(pass==1)org_addr+=4; return; }
        i++;
        if (i >= ntoks || toks[i].type != TOK_COMMA) { asm_error("expected ','"); if(pass==1)org_addr+=4; return; }
        i++;
        if (g_object_mode && pass == 2) {
            int64_t addend = 0;
            int sidx = peek_sym_ref(toks, ntoks, i, &addend);
            if (sidx >= 0 && symtab[sidx].is_extern) {
                emit_reloc(RELOC_JUMP20, sidx, (int32_t)addend);
                emit_word(((uint32_t)0x0E<<24)|((uint32_t)rd<<20)|0);
                break;
            }
        }
        int64_t target = 0;
        if (!eval_expr(toks, ntoks, &i, pass, &target)) { if(pass==1)org_addr+=4; return; }
        int64_t offset = target - (int64_t)instr_addr;
        if (pass == 2 && (offset < -524288 || offset > 524287)) {
            asm_error("jump offset %lld out of range ±524288", (long long)offset);
            if(pass==1)org_addr+=4; return;
        }
        uint32_t enc = ((uint32_t)0x0E<<24)|((uint32_t)rd<<20)|((uint32_t)(offset & 0xFFFFF));
        if (pass == 2) emit_word(enc);
        else org_addr += 4;
        break;
    }
    case FMT_MOVI32: {
        /* pseudo: MOVI32 rd, val32 → MOVI rd, (val&0xFFFF); MOVHI rd, (val>>16) */
        if (i >= ntoks || toks[i].type != TOK_IDENT) { asm_error("expected rd"); if(pass==1)org_addr+=8; return; }
        int rd = parse_register(toks[i].str);
        if (rd < 0) { asm_error("bad register '%s'", toks[i].str); if(pass==1)org_addr+=8; return; }
        i++;
        if (i >= ntoks || toks[i].type != TOK_COMMA) { asm_error("expected ','"); if(pass==1)org_addr+=8; return; }
        i++;
        /* Object mode: label refs need ABS32 relocations (MOVI32 encodes absolute addr) */
        if (g_object_mode && pass == 2) {
            int64_t addend = 0;
            int sidx = peek_sym_ref(toks, ntoks, i, &addend);
            if (sidx >= 0) {
                emit_reloc(RELOC_MOVI32_LO, sidx, (int32_t)addend);
                emit_word(((uint32_t)0x0F<<24)|((uint32_t)rd<<20)|0);  /* MOVI  rd, 0 */
                emit_reloc(RELOC_MOVI32_HI, sidx, (int32_t)addend);
                emit_word(((uint32_t)0x13<<24)|((uint32_t)rd<<20)|0);  /* MOVHI rd, 0 */
                break;
            }
        }
        int64_t val = 0;
        if (!eval_expr(toks, ntoks, &i, pass, &val)) { if(pass==1)org_addr+=8; return; }
        uint16_t lo = (uint16_t)(val & 0xFFFF);
        uint16_t hi = (uint16_t)((val >> 16) & 0xFFFF);
        if (pass == 2) {
            uint32_t enc1 = ((uint32_t)0x0F<<24)|((uint32_t)rd<<20)|lo;  /* MOVI */
            uint32_t enc2 = ((uint32_t)0x13<<24)|((uint32_t)rd<<20)|hi;  /* MOVHI */
            emit_word(enc1);
            emit_word(enc2);
        } else {
            org_addr += 8;
        }
        break;
    }
    case FMT_CMP: {
        /* CMP rs1, rs2 — flags only, rd=0 */
        if (i >= ntoks || toks[i].type != TOK_IDENT) { asm_error("expected rs1"); if(pass==1)org_addr+=4; return; }
        int rs1 = parse_register(toks[i].str);
        if (rs1 < 0) { asm_error("bad register '%s'", toks[i].str); if(pass==1)org_addr+=4; return; }
        i++;
        if (i >= ntoks || toks[i].type != TOK_COMMA) { asm_error("expected ','"); if(pass==1)org_addr+=4; return; }
        i++;
        if (i >= ntoks || toks[i].type != TOK_IDENT) { asm_error("expected rs2"); if(pass==1)org_addr+=4; return; }
        int rs2 = parse_register(toks[i].str);
        if (rs2 < 0) { asm_error("bad register '%s'", toks[i].str); if(pass==1)org_addr+=4; return; }
        uint32_t enc = ((uint32_t)op->opcode<<24) | ((uint32_t)rs1<<16) | ((uint32_t)rs2<<12);
        if (pass == 2) emit_word(enc);
        else org_addr += 4;
        break;
    }
    case FMT_CMPI: {
        /* CMPI rs1, simm16 — flags only, rd=0 */
        if (i >= ntoks || toks[i].type != TOK_IDENT) { asm_error("expected rs1"); if(pass==1)org_addr+=4; return; }
        int rs1 = parse_register(toks[i].str);
        if (rs1 < 0) { asm_error("bad register '%s'", toks[i].str); if(pass==1)org_addr+=4; return; }
        i++;
        if (i >= ntoks || toks[i].type != TOK_COMMA) { asm_error("expected ','"); if(pass==1)org_addr+=4; return; }
        i++;
        int64_t imm = 0;
        if (!eval_expr(toks, ntoks, &i, pass, &imm)) { if(pass==1)org_addr+=4; return; }
        if (pass == 2 && (imm < 0 || imm > 65535)) asm_error("CMPI immediate %lld out of range 0..65535", (long long)imm);
        uint32_t enc = ((uint32_t)op->opcode<<24) | ((uint32_t)rs1<<16) | ((uint16_t)imm);
        if (pass == 2) emit_word(enc);
        else org_addr += 4;
        break;
    }
    case FMT_RET: {
        /* RET — no operands; emits MOV pc, lr = MOV r15, r14 */
        uint32_t enc = ((uint32_t)0x27<<24) | (15u<<20) | (14u<<16);
        if (pass == 2) emit_word(enc);
        else org_addr += 4;
        break;
    }
    case FMT_BR: {
        /* BR rs1 — emits MOV pc, rs1 = MOV r15, rs1 */
        if (i >= ntoks || toks[i].type != TOK_IDENT) { asm_error("expected register"); if(pass==1)org_addr+=4; return; }
        int rs1 = parse_register(toks[i].str);
        if (rs1 < 0) { asm_error("bad register '%s'", toks[i].str); if(pass==1)org_addr+=4; return; }
        uint32_t enc = ((uint32_t)0x27<<24) | (15u<<20) | ((uint32_t)rs1<<16);
        if (pass == 2) emit_word(enc);
        else org_addr += 4;
        break;
    }
    case FMT_PUSH: {
        /* PUSH rd — emits: ADDI sp,sp,-4 then SW rd,[sp] */
        if (i >= ntoks || toks[i].type != TOK_IDENT) { asm_error("expected register"); if(pass==1)org_addr+=8; return; }
        int rd = parse_register(toks[i].str);
        if (rd < 0) { asm_error("bad register '%s'", toks[i].str); if(pass==1)org_addr+=8; return; }
        if (pass == 2) {
            emit_word(((uint32_t)0x14<<24)|(13u<<20)|(13u<<16)|(uint16_t)(uint32_t)-4); /* ADDI sp,sp,-4 */
            emit_word(((uint32_t)0x0C<<24)|((uint32_t)rd<<20)|(13u<<16));               /* SW rd,[sp] */
        } else { org_addr += 8; }
        break;
    }
    case FMT_POP: {
        /* POP rd — emits: LW rd,[sp] then ADDI sp,sp,4 */
        if (i >= ntoks || toks[i].type != TOK_IDENT) { asm_error("expected register"); if(pass==1)org_addr+=8; return; }
        int rd = parse_register(toks[i].str);
        if (rd < 0) { asm_error("bad register '%s'", toks[i].str); if(pass==1)org_addr+=8; return; }
        if (pass == 2) {
            emit_word(((uint32_t)0x0B<<24)|((uint32_t)rd<<20)|(13u<<16));               /* LW rd,[sp] */
            emit_word(((uint32_t)0x14<<24)|(13u<<20)|(13u<<16)|4u);                     /* ADDI sp,sp,4 */
        } else { org_addr += 8; }
        break;
    }
    case FMT_MEM_IDX: {
        /* rd, [rs1+rs2]  — base register + index register */
        if (i >= ntoks || toks[i].type != TOK_IDENT) { asm_error("expected rd"); if(pass==1)org_addr+=4; return; }
        int rd = parse_register(toks[i].str);
        if (rd < 0) { asm_error("bad register '%s'", toks[i].str); if(pass==1)org_addr+=4; return; }
        i++;
        if (i >= ntoks || toks[i].type != TOK_COMMA) { asm_error("expected ','"); if(pass==1)org_addr+=4; return; }
        i++;
        if (i >= ntoks || toks[i].type != TOK_LBRACKET) { asm_error("expected '['"); if(pass==1)org_addr+=4; return; }
        i++;
        if (i >= ntoks || toks[i].type != TOK_IDENT) { asm_error("expected base register"); if(pass==1)org_addr+=4; return; }
        int rs1 = parse_register(toks[i].str);
        if (rs1 < 0) { asm_error("bad register '%s'", toks[i].str); if(pass==1)org_addr+=4; return; }
        i++;
        int rs2 = 0;
        if (i < ntoks && toks[i].type == TOK_PLUS) {
            i++;
            if (i >= ntoks || toks[i].type != TOK_IDENT) { asm_error("expected index register after '+'"); if(pass==1)org_addr+=4; return; }
            rs2 = parse_register(toks[i].str);
            if (rs2 < 0) { asm_error("bad index register '%s'", toks[i].str); if(pass==1)org_addr+=4; return; }
            i++;
        }
        if (i >= ntoks || toks[i].type != TOK_RBRACKET) { asm_error("expected ']'"); if(pass==1)org_addr+=4; return; }
        uint32_t enc = ((uint32_t)op->opcode<<24)|((uint32_t)rd<<20)|((uint32_t)rs1<<16)|((uint32_t)rs2<<12);
        if (pass == 2) emit_word(enc);
        else org_addr += 4;
        break;
    }
    case FMT_CALL: {
        /* CALL target — emits JMP lr, target (rd forced to lr=14) */
        if (i >= ntoks) { asm_error("expected call target"); if(pass==1)org_addr+=4; return; }
        if (g_object_mode && pass == 2) {
            int64_t addend = 0;
            int sidx = peek_sym_ref(toks, ntoks, i, &addend);
            if (sidx >= 0 && symtab[sidx].is_extern) {
                emit_reloc(RELOC_JUMP20, sidx, (int32_t)addend);
                emit_word(((uint32_t)0x0E<<24)|(14u<<20)|0);
                break;
            }
        }
        int64_t target = 0;
        if (!eval_expr(toks, ntoks, &i, pass, &target)) { if(pass==1)org_addr+=4; return; }
        int64_t offset = target - (int64_t)instr_addr;
        if (pass == 2 && (offset < -524288 || offset > 524287)) {
            asm_error("call offset %lld out of range ±524288", (long long)offset);
            if(pass==1)org_addr+=4; return;
        }
        uint32_t enc = ((uint32_t)0x0E<<24) | (14u<<20) | ((uint32_t)(offset & 0xFFFFF));
        if (pass == 2) emit_word(enc);
        else org_addr += 4;
        break;
    }
    }
    (void)instr_addr; /* suppress unused warning when not used in branch/jump */
}

/* ── Pass 1: collect labels ─────────────────────────────────────────────── */
static void pass1(void) {
    org_addr = 0;
    for (int li = 0; li < line_count; li++) {
        cur_lineno = lines[li].lineno;
        cur_filename = lines[li].filename;

        Token toks[MAX_TOKS];
        int n = tokenize_line(lines[li].text, toks, MAX_TOKS);

        int i = 0;
        /* Collect labels */
        while (i < n && toks[i].type == TOK_COLON) {
            const char *name = toks[i].str;
            Symbol *s = find_symbol(name);
            if (!s) s = add_symbol(name);
            if (s) {
                if (s->defined) {
                    asm_error("duplicate label '%s'", name);
                } else {
                    s->addr     = org_addr;
                    s->defined  = 1;
                    s->is_label = 1;
                }
            }
            i++;
        }

        /* Skip if nothing else */
        if (i >= n) continue;

        /* Handle .org separately since it affects org_addr */
        if (toks[i].type == TOK_DOT_IDENT) {
            if (strcasecmp(toks[i].str, ".org") == 0) {
                i++;
                if (i < n && toks[i].type == TOK_INT) {
                    uint32_t new_addr = (uint32_t)toks[i].ival;
                    if (g_object_mode && g_section_base == 0 && new_addr != 0) {
                        g_section_base     = new_addr;
                        g_section_vma_hint = new_addr;
                    }
                    org_addr = new_addr;
                }
            } else if (strcasecmp(toks[i].str, ".extern") == 0) {
                i++;
                if (i < n && toks[i].type == TOK_IDENT) {
                    Symbol *s = find_symbol(toks[i].str);
                    if (!s) s = add_symbol(toks[i].str);
                    if (s) { s->is_extern = 1; s->defined = 0; }
                }
            } else if (strcasecmp(toks[i].str, ".section") == 0) {
                /* silently accept .section in object mode; treat as nop for now */
            } else if (strcasecmp(toks[i].str, ".global") == 0) {
                i++;
                if (i < n && toks[i].type == TOK_IDENT) {
                    Symbol *s = find_symbol(toks[i].str);
                    if (!s) s = add_symbol(toks[i].str);
                    if (s) s->is_global = 1;
                }
            } else if (strcasecmp(toks[i].str, ".align") == 0) {
                i++;
                if (i < n && toks[i].type == TOK_INT) {
                    uint32_t align_val = (uint32_t)toks[i].ival;
                    if (align_val > 1) {
                        uint32_t mask = align_val - 1;
                        org_addr = (org_addr + mask) & ~mask;
                    }
                }
            } else if (strcasecmp(toks[i].str, ".equ") == 0) {
                /* .equ NAME, VALUE — define symbol with given value */
                i++;
                if (i < n && toks[i].type == TOK_IDENT) {
                    const char *sym_name = toks[i].str;
                    i++;
                    if (i < n && toks[i].type == TOK_COMMA) {
                        i++;
                        int64_t val = 0;
                        eval_expr(toks, n, &i, 1, &val);
                        Symbol *s = find_symbol(sym_name);
                        if (!s) s = add_symbol(sym_name);
                        if (s) { s->addr = (uint32_t)val; s->defined = 1; }
                    }
                }
            } else {
                /* advance address for other directives */
                int sz = directive_size(lines[li].text);
                org_addr += (uint32_t)sz;
            }
            continue;
        }

        /* Instruction */
        if (toks[i].type == TOK_IDENT) {
            org_addr += (uint32_t)instr_size(toks[i].str);
        }
    }
}

/* ── Pass 2: encode instructions ─────────────────────────────────────────── */
static void pass2(void) {
    org_addr  = 0;
    write_pos = 0;
    max_write_pos = 0;

    for (int li = 0; li < line_count; li++) {
        cur_lineno = lines[li].lineno;
        cur_filename = lines[li].filename;

        Token toks[MAX_TOKS];
        int n = tokenize_line(lines[li].text, toks, MAX_TOKS);
        if (n == 0) continue;

        assemble_line(toks, n, 2);
    }
}

/* ── Main entry point (assemble from string) ─────────────────────────────── */
static void collect_lines(const char *src, const char *filename) {
    const char *p = src;
    int lineno = 1;
    while (*p && line_count < MAX_LINES) {
        const char *start = p;
        while (*p && *p != '\n') p++;
        int len = (int)(p - start);
        if (len >= MAX_LINE_LEN) len = MAX_LINE_LEN - 1;

        /* Detect .include directive */
        const char *q = start;
        while (q < start + len && (*q == ' ' || *q == '\t')) q++;
        int is_include = 0;
        if ((size_t)(start + len - q) >= 8 &&
            (q[0]=='.'||q[0]=='.') &&
            strncasecmp(q, ".include", 8) == 0 &&
            (q[8] == ' ' || q[8] == '\t' || q[8] == '"')) {
            q += 8;
            while (q < start + len && (*q == ' ' || *q == '\t')) q++;
            if (*q == '"') {
                q++;
                const char *fnstart = q;
                while (*q && *q != '"' && *q != '\n') q++;
                int fnlen = (int)(q - fnstart);
                if (fnlen > 0 && fnlen < 512) {
                    char incpath[1024];
                    if (g_include_dir[0])
                        snprintf(incpath, sizeof(incpath), "%s/%.*s", g_include_dir, fnlen, fnstart);
                    else
                        snprintf(incpath, sizeof(incpath), "%.*s", fnlen, fnstart);
                    char *incsrc = read_file(incpath);
                    if (!incsrc) {
                        cur_filename = filename;
                        cur_lineno   = lineno;
                        asm_error("cannot open include file '%s'", incpath);
                    } else {
                        collect_lines(incsrc, incpath);
                        free(incsrc);
                    }
                    is_include = 1;
                }
            }
        }

        if (!is_include) {
            memcpy(lines[line_count].text, start, (size_t)len);
            lines[line_count].text[len] = '\0';
            lines[line_count].lineno    = lineno;
            strncpy(lines[line_count].filename, filename, sizeof(lines[line_count].filename) - 1);
            lines[line_count].filename[sizeof(lines[line_count].filename) - 1] = '\0';
            line_count++;
        }

        lineno++;
        if (*p == '\n') p++;
    }
}

static int assemble_string(const char *src, uint8_t *out, size_t out_max, size_t *out_len) {
    /* Reset global state */
    sym_count          = 0;
    line_count         = 0;
    error_count        = 0;
    org_addr           = 0;
    write_pos          = 0;
    max_write_pos      = 0;
    reloc_count        = 0;
    g_section_base     = 0;
    g_section_vma_hint = 0;
    memset(outbuf,    0, sizeof(outbuf));
    memset(symtab,    0, sizeof(symtab));
    memset(reloc_tab, 0, sizeof(reloc_tab));
    /* Snapshot cur_filename into a local buffer so collect_lines has a stable
       pointer that cannot alias lines[].filename entries */
    static char asm_filename_buf[256];
    strncpy(asm_filename_buf, cur_filename, sizeof(asm_filename_buf) - 1);
    asm_filename_buf[sizeof(asm_filename_buf) - 1] = '\0';

    /* Split into lines */
    collect_lines(src, asm_filename_buf);

    /* Pass 1 */
    pass1();

    /* Pass 2 */
    pass2();

    /* Copy to output */
    size_t sz = max_write_pos;
    if (sz > out_max) sz = out_max;
    memcpy(out, outbuf, sz);
    *out_len = sz;

    return error_count;
}

/* ── File I/O helpers ─────────────────────────────────────────────────────── */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open '%s'\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char*)malloc((size_t)(sz+1));
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

/* ── main ─────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    /* Parse optional -c flag (object-file mode) */
    int argi = 1;
    if (argi < argc && strcmp(argv[argi], "-c") == 0) {
        g_object_mode = 1;
        argi++;
    }

    if (argc - argi < 2) {
        fprintf(stderr, "usage: %s [-c] input.asm output.bin [output.map]\n", argv[0]);
        fprintf(stderr, "       -c  emit object file (.dobj) instead of flat binary\n");
        return 1;
    }

    const char *infile  = argv[argi];
    const char *outfile = argv[argi + 1];
    const char *mapfile = (argc - argi >= 3) ? argv[argi + 2] : NULL;

    cur_filename = infile;
    /* Set include directory to the directory of the input file */
    {
        const char *slash = strrchr(infile, '/');
        if (slash) {
            int dlen = (int)(slash - infile);
            if (dlen < (int)sizeof(g_include_dir) - 1) {
                memcpy(g_include_dir, infile, (size_t)dlen);
                g_include_dir[dlen] = '\0';
            }
        }
    }
    char *src = read_file(infile);
    if (!src) return 1;

    static uint8_t out[MAX_OUTPUT];
    size_t out_len = 0;

    int errs = assemble_string(src, out, sizeof(out), &out_len);
    free(src);

    if (errs > 0) {
        fprintf(stderr, "%d error(s)\n", errs);
        return 1;
    }

    if (g_object_mode) {
        write_dobj(outfile);
    } else {
        FILE *fout = fopen(outfile, "wb");
        if (!fout) { fprintf(stderr, "cannot write '%s'\n", outfile); return 1; }
        fwrite(out, 1, out_len, fout);
        fclose(fout);
    }

    if (!g_object_mode && mapfile) {
        FILE *fmap = fopen(mapfile, "w");
        if (fmap) {
            for (int i = 0; i < sym_count; i++)
                fprintf(fmap, "%08X %s\n", symtab[i].addr, symtab[i].name);
            fclose(fmap);
        }
    }

    return 0;
}
