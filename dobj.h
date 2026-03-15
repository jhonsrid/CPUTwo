/* CPUTwo Object File Format (.dobj) — shared constants
 *
 * Wire layout (all multi-byte fields big-endian):
 *   [Header 24 B][SectionHeader × N, 24 B each][Symbol × N, 36 B each]
 *   [Reloc × N, 16 B each][section data blobs]
 */
#ifndef DOBJ_H
#define DOBJ_H

#include <stdint.h>

/* ── Magic / version ─────────────────────────────────────────────────────── */
#define DOBJ_MAGIC    "DOBJ"        /* 4 bytes, no NUL */
#define DOBJ_VERSION  1

/* ── Wire-format record sizes ────────────────────────────────────────────── */
#define DOBJ_HEADER_SIZE   24
#define DOBJ_SECTION_SIZE  24
#define DOBJ_SYMBOL_SIZE   36   /* 28 name + 4 value + 2 sec_idx + 1 global + 1 pad */
#define DOBJ_RELOC_SIZE    16

/* ── Relocation types ────────────────────────────────────────────────────── */
#define RELOC_ABS32      1   /* .word slot  ← (S+A) as 32-bit absolute         */
#define RELOC_MOVI32_LO  2   /* MOVI  bits 15:0  ← (S+A) & 0xFFFF              */
#define RELOC_MOVI32_HI  3   /* MOVHI bits 15:0  ← (S+A) >> 16                 */
#define RELOC_BRANCH20   4   /* B-type bits 19:0 ← (S+A - site) PC-relative    */
#define RELOC_JUMP20     5   /* J-type bits 19:0 ← (S+A - site), keep rd field */

/* ── Section flags ───────────────────────────────────────────────────────── */
#define DOBJ_SF_EXEC   0x01
#define DOBJ_SF_WRITE  0x02
#define DOBJ_SF_ALLOC  0x04

/* ── Special section index ───────────────────────────────────────────────── */
#define DOBJ_SEC_UNDEF  0xFFFFu   /* symbol is external (undefined) */

#endif /* DOBJ_H */
