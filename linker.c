/* CPUTwo linker — C99
 * Compile: cc -std=c99 -Wall -o linkertwo linker.c
 * Usage:   ./linkertwo [options] file1.dobj [file2.dobj ...] -o output.bin
 *
 * Options:
 *   -o <file>    output binary (default: out.bin)
 *   -T <addr>    base address for sections without a vma_hint (hex, default 0)
 *   -m <file>    emit map file (address → symbol name)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "dobj.h"

/* ── Limits ──────────────────────────────────────────────────────────────── */
#define MAX_OBJS      256
#define MAX_SECS      256   /* total sections across all objects */
#define MAX_GLOB_SYMS 8192

/* ── Big-endian helpers ──────────────────────────────────────────────────── */
static uint32_t rd32(FILE *f) {
    uint32_t a=(uint8_t)fgetc(f), b=(uint8_t)fgetc(f),
             c=(uint8_t)fgetc(f), d=(uint8_t)fgetc(f);
    return (a<<24)|(b<<16)|(c<<8)|d;
}
static uint16_t rd16(FILE *f) {
    uint32_t a=(uint8_t)fgetc(f), b=(uint8_t)fgetc(f);
    return (uint16_t)((a<<8)|b);
}
static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static void put32(uint8_t *p, uint32_t v) {
    p[0]=(v>>24)&0xFF; p[1]=(v>>16)&0xFF; p[2]=(v>>8)&0xFF; p[3]=v&0xFF;
}

/* ── In-memory object representation ────────────────────────────────────── */
typedef struct {
    char     name[28];
    uint32_t value;      /* section-relative offset */
    uint16_t sec_idx;    /* DOBJ_SEC_UNDEF = extern */
    uint8_t  is_global;
} LSym;                  /* loaded symbol */

typedef struct {
    uint32_t offset;     /* section-relative */
    uint32_t sym_idx;
    uint8_t  type;
    uint8_t  sec_idx;
    int32_t  addend;
} LRel;                  /* loaded relocation */

typedef struct {
    uint32_t vma_hint;
    uint32_t size;
    uint32_t base;       /* assigned by linker */
    uint8_t *data;       /* mutable copy of section bytes */
} LSec;                  /* loaded section */

typedef struct {
    char   filename[256];
    uint32_t nsec, nsym, nrel;
    LSec  *secs;
    LSym  *syms;
    LRel  *rels;
} ObjFile;

static ObjFile objs[MAX_OBJS];
static int     obj_count = 0;
static int     link_errors = 0;

/* ── Global symbol table ─────────────────────────────────────────────────── */
typedef struct {
    char     name[28];
    uint32_t abs_addr;
    int      obj_idx;
} GlobSym;

static GlobSym glob_syms[MAX_GLOB_SYMS];
static int     glob_count = 0;

static GlobSym *find_global(const char *name) {
    for (int i = 0; i < glob_count; i++)
        if (strcmp(glob_syms[i].name, name) == 0) return &glob_syms[i];
    return NULL;
}

/* ── Load a .dobj file ───────────────────────────────────────────────────── */
static int load_dobj(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open '%s'\n", path); return 0; }

    /* magic + version */
    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, DOBJ_MAGIC, 4) != 0) {
        fprintf(stderr, "%s: not a .dobj file\n", path); fclose(f); return 0;
    }
    fgetc(f);          /* version — ignored */
    fgetc(f); fgetc(f); fgetc(f); /* pad */

    if (obj_count >= MAX_OBJS) { fprintf(stderr, "too many object files\n"); fclose(f); return 0; }
    ObjFile *obj = &objs[obj_count++];
    memset(obj, 0, sizeof(*obj));
    strncpy(obj->filename, path, sizeof(obj->filename)-1);

    obj->nsec = rd32(f);
    obj->nsym = rd32(f);
    obj->nrel = rd32(f);
    uint32_t data_off = rd32(f);

    /* Section headers */
    obj->secs = (LSec*)calloc(obj->nsec, sizeof(LSec));
    for (uint32_t i = 0; i < obj->nsec; i++) {
        char name[8]; fread(name, 1, 8, f); (void)name;
        uint32_t sec_data_offset = rd32(f);
        obj->secs[i].size     = rd32(f);
        obj->secs[i].vma_hint = rd32(f);
        rd32(f); /* flags */
        obj->secs[i].data     = NULL; /* filled after reading data blob */
        /* store sec_data_offset temporarily in base */
        obj->secs[i].base     = sec_data_offset;
    }

    /* Symbols */
    obj->syms = (LSym*)calloc(obj->nsym, sizeof(LSym));
    for (uint32_t i = 0; i < obj->nsym; i++) {
        fread(obj->syms[i].name, 1, 28, f);
        obj->syms[i].value    = rd32(f);
        obj->syms[i].sec_idx  = rd16(f);
        obj->syms[i].is_global = (uint8_t)fgetc(f);
        fgetc(f); /* pad */
    }

    /* Relocations */
    obj->rels = (LRel*)calloc(obj->nrel, sizeof(LRel));
    for (uint32_t i = 0; i < obj->nrel; i++) {
        obj->rels[i].offset  = rd32(f);
        obj->rels[i].sym_idx = rd32(f);
        obj->rels[i].type    = (uint8_t)fgetc(f);
        obj->rels[i].sec_idx = (uint8_t)fgetc(f);
        fgetc(f); fgetc(f); /* pad */
        obj->rels[i].addend  = (int32_t)rd32(f);
    }

    /* Section data blob */
    long cur = ftell(f);
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, (long)data_off, SEEK_SET);
    uint32_t blob_size = (uint32_t)(file_size - (long)data_off);
    uint8_t *blob = (uint8_t*)calloc(blob_size ? blob_size : 1, 1);
    if (blob_size > 0) fread(blob, 1, blob_size, f);
    (void)cur;

    /* Point each section's data into the blob */
    for (uint32_t i = 0; i < obj->nsec; i++) {
        uint32_t off = obj->secs[i].base; /* was stored there above */
        obj->secs[i].base = 0;            /* reset for address assignment */
        /* Make a mutable copy of this section's bytes */
        obj->secs[i].data = (uint8_t*)calloc(obj->secs[i].size ? obj->secs[i].size : 1, 1);
        if (off < blob_size && obj->secs[i].size > 0) {
            uint32_t copy = obj->secs[i].size;
            if (off + copy > blob_size) copy = blob_size - off;
            memcpy(obj->secs[i].data, blob + off, copy);
        }
    }
    free(blob);
    fclose(f);
    return 1;
}

/* ── Assign base addresses to all sections ───────────────────────────────── */
static void assign_addresses(uint32_t text_base) {
    uint32_t next = text_base;
    for (int oi = 0; oi < obj_count; oi++) {
        for (uint32_t si = 0; si < objs[oi].nsec; si++) {
            LSec *sec = &objs[oi].secs[si];
            if (sec->vma_hint != 0) {
                sec->base = sec->vma_hint;
            } else {
                /* Align to 4 bytes */
                sec->base = (next + 3u) & ~3u;
            }
            uint32_t end = sec->base + sec->size;
            if (end > next) next = (end + 3u) & ~3u;
        }
    }
}

/* ── Build global symbol table ───────────────────────────────────────────── */
static void build_globals(void) {
    for (int oi = 0; oi < obj_count; oi++) {
        ObjFile *obj = &objs[oi];
        for (uint32_t si = 0; si < obj->nsym; si++) {
            LSym *sym = &obj->syms[si];
            if (!sym->is_global) continue;
            if (sym->sec_idx == (uint16_t)DOBJ_SEC_UNDEF) continue; /* extern ref */
            uint32_t abs_addr = obj->secs[sym->sec_idx].base + sym->value;
            GlobSym *existing = find_global(sym->name);
            if (existing) {
                fprintf(stderr, "%s: duplicate global symbol '%s' (also in %s)\n",
                        obj->filename, sym->name, objs[existing->obj_idx].filename);
                link_errors++;
                continue;
            }
            if (glob_count >= MAX_GLOB_SYMS) {
                fprintf(stderr, "too many global symbols\n");
                link_errors++;
                return;
            }
            GlobSym *g = &glob_syms[glob_count++];
            strncpy(g->name, sym->name, 27);
            g->abs_addr = abs_addr;
            g->obj_idx  = oi;
        }
    }
}

/* ── Resolve relocations ─────────────────────────────────────────────────── */
static void resolve_relocs(void) {
    for (int oi = 0; oi < obj_count; oi++) {
        ObjFile *obj = &objs[oi];
        for (uint32_t ri = 0; ri < obj->nrel; ri++) {
            LRel *r = &obj->rels[ri];
            if (r->sec_idx >= obj->nsec) {
                fprintf(stderr, "%s: reloc %u: bad section index %u\n",
                        obj->filename, ri, r->sec_idx);
                link_errors++;
                continue;
            }
            LSec *sec  = &obj->secs[r->sec_idx];
            LSym *sym  = &obj->syms[r->sym_idx];

            /* Resolve symbol to absolute address */
            uint32_t S;
            if (sym->sec_idx == (uint16_t)DOBJ_SEC_UNDEF) {
                GlobSym *g = find_global(sym->name);
                if (!g) {
                    fprintf(stderr, "%s: undefined symbol '%s'\n",
                            obj->filename, sym->name);
                    link_errors++;
                    continue;
                }
                S = g->abs_addr;
            } else {
                S = obj->secs[sym->sec_idx].base + sym->value;
            }
            S = (uint32_t)((int32_t)S + r->addend);

            /* Site's absolute address and pointer into section data */
            uint32_t site_addr = sec->base + r->offset;
            if (r->offset + 4 > sec->size) {
                fprintf(stderr, "%s: reloc %u out of section bounds\n",
                        obj->filename, ri);
                link_errors++;
                continue;
            }
            uint8_t *p = sec->data + r->offset;

            switch (r->type) {
            case RELOC_ABS32:
                put32(p, S);
                break;
            case RELOC_MOVI32_LO: {
                uint32_t instr = get32(p);
                instr = (instr & 0xFFFF0000u) | (S & 0xFFFF);
                put32(p, instr);
                break;
            }
            case RELOC_MOVI32_HI: {
                uint32_t instr = get32(p);
                instr = (instr & 0xFFFF0000u) | ((S >> 16) & 0xFFFF);
                put32(p, instr);
                break;
            }
            case RELOC_BRANCH20:
            case RELOC_JUMP20: {
                int64_t delta = (int64_t)S - (int64_t)site_addr;
                if (delta < -524288 || delta > 524287) {
                    fprintf(stderr, "%s: reloc %u: %s offset out of range at 0x%08X"
                            " (delta=%lld)\n",
                            obj->filename, ri,
                            r->type == RELOC_BRANCH20 ? "branch" : "jump",
                            site_addr, (long long)delta);
                    link_errors++;
                    break;
                }
                /* Preserve opcode (31:24) + cond/rd (23:20), patch offset (19:0) */
                uint32_t instr = get32(p);
                instr = (instr & 0xFFF00000u) | ((uint32_t)delta & 0xFFFFF);
                put32(p, instr);
                break;
            }
            default:
                fprintf(stderr, "%s: unknown relocation type %u\n",
                        obj->filename, r->type);
                link_errors++;
                break;
            }
        }
    }
}

/* ── Write flat binary output ────────────────────────────────────────────── */
static int emit_output(const char *path) {
    /* Compute total size */
    uint32_t out_size = 0;
    for (int oi = 0; oi < obj_count; oi++)
        for (uint32_t si = 0; si < objs[oi].nsec; si++) {
            uint32_t end = objs[oi].secs[si].base + objs[oi].secs[si].size;
            if (end > out_size) out_size = end;
        }

    if (out_size == 0) {
        fprintf(stderr, "warning: no sections to emit\n");
        out_size = 0;
    }

    uint8_t *buf = (uint8_t*)calloc(out_size ? out_size : 1, 1);
    for (int oi = 0; oi < obj_count; oi++)
        for (uint32_t si = 0; si < objs[oi].nsec; si++) {
            LSec *sec = &objs[oi].secs[si];
            if (sec->size > 0)
                memcpy(buf + sec->base, sec->data, sec->size);
        }

    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write '%s'\n", path); free(buf); return 0; }
    if (out_size > 0) fwrite(buf, 1, out_size, f);
    fclose(f);
    free(buf);
    return 1;
}

/* ── Write map file ──────────────────────────────────────────────────────── */
static void write_map(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot write map '%s'\n", path); return; }
    for (int i = 0; i < glob_count; i++)
        fprintf(f, "%08X %s\n", glob_syms[i].abs_addr, glob_syms[i].name);
    /* Also dump all local symbols per object */
    for (int oi = 0; oi < obj_count; oi++) {
        ObjFile *obj = &objs[oi];
        for (uint32_t si = 0; si < obj->nsym; si++) {
            LSym *sym = &obj->syms[si];
            if (sym->is_global) continue; /* already in globals */
            if (sym->sec_idx == (uint16_t)DOBJ_SEC_UNDEF) continue;
            uint32_t addr = obj->secs[sym->sec_idx].base + sym->value;
            fprintf(f, "%08X %s\n", addr, sym->name);
        }
    }
    fclose(f);
}

/* ── Free loaded objects ─────────────────────────────────────────────────── */
static void free_objs(void) {
    for (int oi = 0; oi < obj_count; oi++) {
        ObjFile *obj = &objs[oi];
        for (uint32_t si = 0; si < obj->nsec; si++) free(obj->secs[si].data);
        free(obj->secs);
        free(obj->syms);
        free(obj->rels);
    }
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    const char *outfile  = "out.bin";
    const char *mapfile  = NULL;
    uint32_t    text_base = 0;
    int         ninputs  = 0;
    const char *inputs[MAX_OBJS];

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i+1 < argc) {
            outfile = argv[++i];
        } else if (strcmp(argv[i], "-T") == 0 && i+1 < argc) {
            char *end;
            text_base = (uint32_t)strtoul(argv[++i], &end, 16);
        } else if (strcmp(argv[i], "-m") == 0 && i+1 < argc) {
            mapfile = argv[++i];
        } else if (argv[i][0] != '-') {
            if (ninputs < MAX_OBJS) inputs[ninputs++] = argv[i];
        } else {
            fprintf(stderr, "unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    if (ninputs == 0) {
        fprintf(stderr, "usage: %s [options] file1.dobj [...] -o output.bin\n"
                        "  -o <file>    output binary (default: out.bin)\n"
                        "  -T <addr>    base address for sections (hex)\n"
                        "  -m <file>    map file\n", argv[0]);
        return 1;
    }

    /* Load */
    for (int i = 0; i < ninputs; i++)
        if (!load_dobj(inputs[i])) return 1;

    /* Link */
    assign_addresses(text_base);
    build_globals();
    if (link_errors) goto done;
    resolve_relocs();
    if (link_errors) goto done;
    emit_output(outfile);
    if (mapfile) write_map(mapfile);

done:
    free_objs();
    if (link_errors) {
        fprintf(stderr, "%d linker error(s)\n", link_errors);
        return 1;
    }
    return 0;
}
