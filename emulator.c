/* CPUTwo Emulator — C99 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/select.h>
#include <unistd.h>

/* ── Memory ──────────────────────────────────────────────────────────────── */
#define MEM_SIZE   0x04000000u
#define MMIO_BASE  0x03F00000u
#define MMIO_END   0x03FFFFFFu

/* MMIO device bases */
#define UART_BASE  0x03F00000u
#define TIMER_BASE 0x03F01000u
#define IC_BASE    0x03F02000u
#define BLK_BASE   0x03F03000u
#define SV_BASE    0x03FFF000u

static unsigned char mem[MEM_SIZE];

/* ── CPU State ───────────────────────────────────────────────────────────── */
typedef struct {
    uint32_t r[16];     /* r0-r12 GPR, r13=sp, r14=lr, r15=pc */
    uint32_t flags;     /* N=31 Z=30 C=29 V=28 */
    uint32_t epc;
    uint32_t eflags;
    uint32_t evec;
    uint32_t cause;
    uint32_t status;    /* bit0=priv(1=sv) bit1=IE */
    int halted;
    uint32_t estatus;    /* STATUS saved on exception entry; restored by SYSRET */
    uint32_t uart_poll_divider; /* counts instructions; poll only every 1000 */
    uint32_t faulting_pc; /* PC of the currently-executing instruction */

    /* UART device */
    uint32_t uart_control;  /* bit0=RX irq en, bit1=TX irq en */
    int      uart_rx_ready; /* 1 if a byte is buffered */
    uint8_t  uart_rx_byte;  /* buffered RX byte */

    /* Timer device */
    uint32_t timer_period;
    uint32_t timer_count;
    uint32_t timer_control; /* bit0=enable, bit1=irq en */

    /* Interrupt controller */
    uint32_t ic_pending;
    uint32_t ic_mask;

    /* Block device */
    uint32_t blk_sector;
    uint32_t blk_buffer;
    uint32_t blk_status;    /* 0=idle 1=busy 2=error */
    uint32_t blk_control;   /* bit0=irq en */
    FILE    *blk_file;
} CPU;

/* ── Memory helpers ──────────────────────────────────────────────────────── */
static inline uint32_t mem_read32(uint32_t addr) {
    return ((uint32_t)mem[addr]<<24)|((uint32_t)mem[addr+1]<<16)|
           ((uint32_t)mem[addr+2]<<8)|(uint32_t)mem[addr+3];
}
static inline uint16_t mem_read16(uint32_t addr) {
    return (uint16_t)(((uint32_t)mem[addr]<<8)|(uint32_t)mem[addr+1]);
}
static inline uint8_t mem_read8(uint32_t addr) { return mem[addr]; }

static inline void mem_write32(uint32_t addr, uint32_t v) {
    mem[addr]=(v>>24)&0xFF; mem[addr+1]=(v>>16)&0xFF;
    mem[addr+2]=(v>>8)&0xFF; mem[addr+3]=v&0xFF;
}
static inline void mem_write16(uint32_t addr, uint16_t v) {
    mem[addr]=(v>>8)&0xFF; mem[addr+1]=v&0xFF;
}
static inline void mem_write8(uint32_t addr, uint8_t v) { mem[addr]=v; }

/* ── Forward declarations ────────────────────────────────────────────────── */
static void raise_exception(CPU *cpu, uint32_t cause);
static uint32_t mmio_read(CPU *cpu, uint32_t addr);
static void mmio_write(CPU *cpu, uint32_t addr, uint32_t val);

/* ── Exception dispatch ──────────────────────────────────────────────────── */
static void raise_exception(CPU *cpu, uint32_t cause) {
    if (cpu->status & 1) { /* double fault */
        cpu->halted = 1;
        return;
    }
    cpu->estatus = cpu->status; /* save STATUS before clobbering it */
    cpu->epc    = cpu->faulting_pc;
    cpu->eflags = cpu->flags;
    cpu->cause  = cause;
    cpu->status = 0x01;
    uint32_t hvec = cpu->evec + cause * 4;
    cpu->r[15] = (hvec < MEM_SIZE) ? mem_read32(hvec) : 0;
}

/* ── Flag helpers ────────────────────────────────────────────────────────── */
#define FLAG_N (1u<<31)
#define FLAG_Z (1u<<30)
#define FLAG_C (1u<<29)
#define FLAG_V (1u<<28)

static inline void update_flags_nz(CPU *cpu, uint32_t result) {
    cpu->flags = (cpu->flags & ~(FLAG_N|FLAG_Z))
               | (result & 0x80000000u ? FLAG_N : 0)
               | (result == 0          ? FLAG_Z : 0);
}
static inline void clear_cv(CPU *cpu) {
    cpu->flags &= ~(FLAG_C|FLAG_V);
}
static inline void update_flags_add(CPU *cpu, uint32_t a, uint32_t b, uint32_t result) {
    uint64_t ua = a, ub = b;
    int carry = (ua + ub) > 0xFFFFFFFFull;
    int overflow = (!((a^b) & 0x80000000u)) &&
                   ((result ^ a) & 0x80000000u);
    cpu->flags = (cpu->flags & ~(FLAG_N|FLAG_Z|FLAG_C|FLAG_V))
               | (result & 0x80000000u ? FLAG_N : 0)
               | (result == 0          ? FLAG_Z : 0)
               | (carry                ? FLAG_C : 0)
               | (overflow             ? FLAG_V : 0);
}
static inline void update_flags_sub(CPU *cpu, uint32_t a, uint32_t b, uint32_t result) {
    int borrow   = (uint64_t)b > (uint64_t)a;
    int overflow = ((a ^ b) & 0x80000000u) && ((result ^ a) & 0x80000000u);
    cpu->flags = (cpu->flags & ~(FLAG_N|FLAG_Z|FLAG_C|FLAG_V))
               | (result & 0x80000000u ? FLAG_N : 0)
               | (result == 0          ? FLAG_Z : 0)
               | (borrow               ? FLAG_C : 0)
               | (overflow             ? FLAG_V : 0);
}

/* ── MMIO ────────────────────────────────────────────────────────────────── */
static uint32_t mmio_read(CPU *cpu, uint32_t addr) {
    /* Supervisor registers */
    if (addr >= SV_BASE && addr <= SV_BASE+0x14) {
        if (!(cpu->status & 1)) { raise_exception(cpu, 0x02); return 0; }
        switch (addr - SV_BASE) {
            case 0x00: return cpu->epc;
            case 0x04: return cpu->eflags;
            case 0x08: return cpu->evec;
            case 0x0C: return cpu->cause;
            case 0x10: return cpu->status;
            case 0x14: return cpu->estatus;
        }
        return 0;
    }
    /* UART */
    if (addr >= UART_BASE && addr < UART_BASE+0x10) {
        switch (addr - UART_BASE) {
            case 0x00: /* status: TX always ready; RX: check buffer */
                return 0x01u | (cpu->uart_rx_ready ? 0x02u : 0);
            case 0x04: return 0; /* TX write-only */
            case 0x08: { /* RX read — consume buffered byte */
                if (!cpu->uart_rx_ready) return 0;
                uint8_t b = cpu->uart_rx_byte;
                cpu->uart_rx_ready = 0;
                cpu->ic_pending &= ~0x02u; /* clear RX pending on read */
                return (uint32_t)b;
            }
            case 0x0C: return cpu->uart_control;
        }
        return 0;
    }
    /* Timer */
    if (addr >= TIMER_BASE && addr < TIMER_BASE+0x08) {
        switch (addr - TIMER_BASE) {
            case 0x00: return cpu->timer_count;
            case 0x04: return cpu->timer_control;
        }
        return 0;
    }
    /* IC */
    if (addr >= IC_BASE && addr < IC_BASE+0x0C) {
        switch (addr - IC_BASE) {
            case 0x00: return cpu->ic_pending;
            case 0x04: return cpu->ic_mask;
            case 0x08: return 0; /* ack write-only */
        }
        return 0;
    }
    /* Block device */
    if (addr >= BLK_BASE && addr < BLK_BASE+0x14) {
        switch (addr - BLK_BASE) {
            case 0x00: return cpu->blk_sector;
            case 0x04: return cpu->blk_buffer;
            case 0x08: return 0; /* command write-only */
            case 0x0C: return cpu->blk_status;
            case 0x10: return cpu->blk_control;
        }
        return 0;
    }
    return 0;
}

static void mmio_write(CPU *cpu, uint32_t addr, uint32_t val) {
    /* Supervisor registers */
    if (addr >= SV_BASE && addr <= SV_BASE+0x14) {
        if (!(cpu->status & 1)) { raise_exception(cpu, 0x02); return; }
        switch (addr - SV_BASE) {
            case 0x00: cpu->epc    = val; break;
            case 0x04: cpu->eflags = val; break;
            case 0x08: cpu->evec   = val; break;
            case 0x0C: break; /* cause read-only */
            case 0x10: cpu->status = val; break;
            case 0x14: cpu->estatus = val; break;
        }
        return;
    }
    /* UART */
    if (addr >= UART_BASE && addr < UART_BASE+0x10) {
        switch (addr - UART_BASE) {
            case 0x04: /* TX */
                putchar((int)(val & 0xFF));
                fflush(stdout);
                /* latch TX pending if device TX irq is enabled */
                if (cpu->uart_control & 0x02)
                    cpu->ic_pending |= 0x04;
                break;
            case 0x0C: cpu->uart_control = val; break;
        }
        return;
    }
    /* Timer */
    if (addr >= TIMER_BASE && addr < TIMER_BASE+0x08) {
        switch (addr - TIMER_BASE) {
            case 0x00:
                cpu->timer_period = val;
                cpu->timer_count  = val;
                break;
            case 0x04: cpu->timer_control = val; break;
        }
        return;
    }
    /* IC */
    if (addr >= IC_BASE && addr < IC_BASE+0x0C) {
        switch (addr - IC_BASE) {
            case 0x00: break; /* pending read-only */
            case 0x04: cpu->ic_mask = val; break;
            case 0x08: cpu->ic_pending &= ~val; break; /* acknowledge */
        }
        return;
    }
    /* Block device */
    if (addr >= BLK_BASE && addr < BLK_BASE+0x14) {
        switch (addr - BLK_BASE) {
            case 0x00: cpu->blk_sector  = val; break;
            case 0x04: cpu->blk_buffer  = val; break;
            case 0x08: { /* command */
                if (!cpu->blk_file) { cpu->blk_status = 2; break; }
                if (cpu->blk_buffer & 0x1FF) { cpu->blk_status = 2; break; } /* must be 512-byte aligned */
                cpu->blk_status = 1;
                if (val == 1) { /* read */
                    if (fseek(cpu->blk_file, (long)cpu->blk_sector*512, SEEK_SET)==0 &&
                        cpu->blk_buffer + 512 <= MEM_SIZE) {
                        size_t n = fread(mem + cpu->blk_buffer, 1, 512, cpu->blk_file);
                        cpu->blk_status = (n==512) ? 0 : 2;
                    } else cpu->blk_status = 2;
                } else if (val == 2) { /* write */
                    if (fseek(cpu->blk_file, (long)cpu->blk_sector*512, SEEK_SET)==0 &&
                        cpu->blk_buffer + 512 <= MEM_SIZE) {
                        size_t n = fwrite(mem + cpu->blk_buffer, 1, 512, cpu->blk_file);
                        cpu->blk_status = (n==512) ? 0 : 2;
                    } else cpu->blk_status = 2;
                }
                if (cpu->blk_control & 1) cpu->ic_pending |= 0x08;
                break;
            }
            case 0x0C: break; /* status read-only */
            case 0x10: cpu->blk_control = val; break;
        }
        return;
    }
}

/* ── Memory access with fault checking ──────────────────────────────────── */
static uint32_t cpu_read32(CPU *cpu, uint32_t addr) {
    if (addr >= MEM_SIZE)           { raise_exception(cpu, 0x02); return 0; }
    if (addr & 3)                   { raise_exception(cpu, 0x01); return 0; }
    if (addr >= MMIO_BASE)          return mmio_read(cpu, addr);
    return mem_read32(addr);
}
static uint16_t cpu_read16(CPU *cpu, uint32_t addr) {
    if (addr >= MEM_SIZE)           { raise_exception(cpu, 0x02); return 0; }
    if (addr & 1)                   { raise_exception(cpu, 0x01); return 0; }
    if (addr >= MMIO_BASE)          return (uint16_t)mmio_read(cpu, addr);
    return mem_read16(addr);
}
static uint8_t cpu_read8(CPU *cpu, uint32_t addr) {
    if (addr >= MEM_SIZE)           { raise_exception(cpu, 0x02); return 0; }
    if (addr >= MMIO_BASE)          return (uint8_t)mmio_read(cpu, addr);
    return mem_read8(addr);
}
static void cpu_write32(CPU *cpu, uint32_t addr, uint32_t val) {
    if (addr >= MEM_SIZE)           { raise_exception(cpu, 0x02); return; }
    if (addr & 3)                   { raise_exception(cpu, 0x01); return; }
    if (addr >= MMIO_BASE)          { mmio_write(cpu, addr, val); return; }
    mem_write32(addr, val);
}
static void cpu_write16(CPU *cpu, uint32_t addr, uint16_t val) {
    if (addr >= MEM_SIZE)           { raise_exception(cpu, 0x02); return; }
    if (addr & 1)                   { raise_exception(cpu, 0x01); return; }
    if (addr >= MMIO_BASE)          { mmio_write(cpu, addr, val); return; }
    mem_write16(addr, val);
}
static void cpu_write8(CPU *cpu, uint32_t addr, uint8_t val) {
    if (addr >= MEM_SIZE)           { raise_exception(cpu, 0x02); return; }
    if (addr >= MMIO_BASE)          { mmio_write(cpu, addr, val); return; }
    mem_write8(addr, val);
}

/* ── UART RX poll (non-blocking stdin check) ─────────────────────────────── */
static void uart_poll(CPU *cpu) {
    if (cpu->uart_rx_ready) return; /* already have a byte buffered */
    fd_set fds;
    struct timeval tv = {0, 0};
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
        int c = getchar();
        if (c != EOF) {
            cpu->uart_rx_byte  = (uint8_t)c;
            cpu->uart_rx_ready = 1;
            if (cpu->uart_control & 0x01) /* RX irq enable */
                cpu->ic_pending |= 0x02;
        }
    }
}

/* ── Timer tick ──────────────────────────────────────────────────────────── */
static void timer_tick(CPU *cpu) {
    if (!(cpu->timer_control & 1)) return;
    if (cpu->timer_count > 0) cpu->timer_count--;
    if (cpu->timer_count == 0) {
        cpu->timer_count = cpu->timer_period;
        if (cpu->timer_control & 2)
            cpu->ic_pending |= 0x01;
    }
}

/* ── Interrupt check ─────────────────────────────────────────────────────── */
static void interrupt_check(CPU *cpu) {
    if (!(cpu->status & 2)) return; /* IE=0 */
    if ((cpu->ic_pending & cpu->ic_mask) == 0) return;
    /* For IRQ, EPC must be the NEXT instruction (cpu->r[15], already advanced
     * past the last executed instruction), not faulting_pc (the instruction
     * that just ran).  Temporarily redirect faulting_pc so raise_exception
     * stores the correct resume address. */
    uint32_t saved = cpu->faulting_pc;
    cpu->faulting_pc = cpu->r[15];
    raise_exception(cpu, 0x06);
    cpu->faulting_pc = saved;
}

/* ── Disassembler (for debugger) ─────────────────────────────────────────── */
static const char *cond_names[] = {"BEQ","BNE","BLT","BGE","BLTU","BGEU","BA","BGT","BLE","BGTU","BLEU"};
static const char *reg_name(int r) {
    static const char *names[] = {
        "r0","r1","r2","r3","r4","r5","r6","r7",
        "r8","r9","r10","r11","r12","sp","lr","pc"
    };
    return names[r & 15];
}

static void disasm(uint32_t addr, uint32_t instr, char *buf, size_t sz) {
    uint8_t  op    = (instr >> 24) & 0xFF;
    uint8_t  rd    = (instr >> 20) & 0x0F;
    uint8_t  rs1   = (instr >> 16) & 0x0F;
    uint8_t  rs2   = (instr >> 12) & 0x0F;
    uint8_t  shift = (instr >>  7) & 0x1F;
    uint16_t imm16 = instr & 0xFFFF;
    int16_t  simm  = (int16_t)imm16;
    uint32_t off20 = instr & 0xFFFFF;
    int32_t  soff  = (off20 & 0x80000) ? (int32_t)(off20 | 0xFFF00000u) : (int32_t)off20;
    uint32_t tgt   = addr + (uint32_t)soff;

    switch (op) {
        case 0x00: snprintf(buf,sz,"ADD  %s,%s,%s",reg_name(rd),reg_name(rs1),reg_name(rs2)); break;
        case 0x01: snprintf(buf,sz,"SUB  %s,%s,%s",reg_name(rd),reg_name(rs1),reg_name(rs2)); break;
        case 0x02: snprintf(buf,sz,"AND  %s,%s,%s",reg_name(rd),reg_name(rs1),reg_name(rs2)); break;
        case 0x03: snprintf(buf,sz,"OR   %s,%s,%s",reg_name(rd),reg_name(rs1),reg_name(rs2)); break;
        case 0x04: snprintf(buf,sz,"XOR  %s,%s,%s",reg_name(rd),reg_name(rs1),reg_name(rs2)); break;
        case 0x05: snprintf(buf,sz,"NOT  %s,%s",reg_name(rd),reg_name(rs1)); break;
        case 0x06: snprintf(buf,sz,"LSL  %s,%s,%u",reg_name(rd),reg_name(rs1),shift); break;
        case 0x07: snprintf(buf,sz,"LSR  %s,%s,%u",reg_name(rd),reg_name(rs1),shift); break;
        case 0x08: snprintf(buf,sz,"ASR  %s,%s,%u",reg_name(rd),reg_name(rs1),shift); break;
        case 0x09: snprintf(buf,sz,"MUL  %s,%s,%s",reg_name(rd),reg_name(rs1),reg_name(rs2)); break;
        case 0x0A: snprintf(buf,sz,"DIV  %s,%s,%s",reg_name(rd),reg_name(rs1),reg_name(rs2)); break;
        case 0x0B: snprintf(buf,sz,"LW   %s,[%s+%d]",reg_name(rd),reg_name(rs1),simm); break;
        case 0x0C: snprintf(buf,sz,"SW   %s,[%s+%d]",reg_name(rd),reg_name(rs1),simm); break;
        case 0x0D:
            if (rd < 11) snprintf(buf,sz,"%-4s 0x%08X",cond_names[rd],tgt);
            else         snprintf(buf,sz,"B??  (illegal cond %u)",rd);
            break;
        case 0x0E: snprintf(buf,sz,"JMP  %s,0x%08X",reg_name(rd),tgt); break;
        case 0x0F: snprintf(buf,sz,"MOVI %s,0x%04X",reg_name(rd),imm16); break;
        case 0x10: snprintf(buf,sz,"SYSCALL"); break;
        case 0x11: snprintf(buf,sz,"SYSRET"); break;
        case 0x12: snprintf(buf,sz,"HALT"); break;
        case 0x13: snprintf(buf,sz,"MOVHI %s,0x%04X",reg_name(rd),imm16); break;
        case 0x14: snprintf(buf,sz,"ADDI %s,%s,%d",reg_name(rd),reg_name(rs1),simm); break;
        case 0x15: snprintf(buf,sz,"SUBI %s,%s,%d",reg_name(rd),reg_name(rs1),simm); break;
        case 0x16: snprintf(buf,sz,"ANDI %s,%s,0x%04X",reg_name(rd),reg_name(rs1),imm16); break;
        case 0x17: snprintf(buf,sz,"ORI  %s,%s,0x%04X",reg_name(rd),reg_name(rs1),imm16); break;
        case 0x18: snprintf(buf,sz,"XORI %s,%s,0x%04X",reg_name(rd),reg_name(rs1),imm16); break;
        case 0x19: snprintf(buf,sz,"LSLI %s,%s,%u",reg_name(rd),reg_name(rs1),imm16&0x1F); break;
        case 0x1A: snprintf(buf,sz,"LSRI %s,%s,%u",reg_name(rd),reg_name(rs1),imm16&0x1F); break;
        case 0x1B: snprintf(buf,sz,"ASRI %s,%s,%u",reg_name(rd),reg_name(rs1),imm16&0x1F); break;
        case 0x1C: snprintf(buf,sz,"LH   %s,[%s+%d]",reg_name(rd),reg_name(rs1),simm); break;
        case 0x1D: snprintf(buf,sz,"LHU  %s,[%s+%d]",reg_name(rd),reg_name(rs1),simm); break;
        case 0x1E: snprintf(buf,sz,"LB   %s,[%s+%d]",reg_name(rd),reg_name(rs1),simm); break;
        case 0x1F: snprintf(buf,sz,"LBU  %s,[%s+%d]",reg_name(rd),reg_name(rs1),simm); break;
        case 0x20: snprintf(buf,sz,"SH   %s,[%s+%d]",reg_name(rd),reg_name(rs1),simm); break;
        case 0x21: snprintf(buf,sz,"SB   %s,[%s+%d]",reg_name(rd),reg_name(rs1),simm); break;
        case 0x22: snprintf(buf,sz,"MULH %s,%s,%s",reg_name(rd),reg_name(rs1),reg_name(rs2)); break;
        case 0x23: snprintf(buf,sz,"MULHU %s,%s,%s",reg_name(rd),reg_name(rs1),reg_name(rs2)); break;
        case 0x24: snprintf(buf,sz,"DIVU %s,%s,%s",reg_name(rd),reg_name(rs1),reg_name(rs2)); break;
        case 0x25: snprintf(buf,sz,"MOD  %s,%s,%s",reg_name(rd),reg_name(rs1),reg_name(rs2)); break;
        case 0x26: snprintf(buf,sz,"MODU %s,%s,%s",reg_name(rd),reg_name(rs1),reg_name(rs2)); break;
        case 0x27: snprintf(buf,sz,"MOV  %s,%s",reg_name(rd),reg_name(rs1)); break;
        case 0x28: snprintf(buf,sz,"CMP  %s,%s",reg_name(rs1),reg_name(rs2)); break;
        case 0x29: snprintf(buf,sz,"CMPI %s,%d",reg_name(rs1),simm); break;
        case 0x2A: snprintf(buf,sz,"CALLR %s,%s",reg_name(rd),reg_name(rs1)); break;
        case 0x2B: snprintf(buf,sz,"ADDC %s,%s,%s",reg_name(rd),reg_name(rs1),reg_name(rs2)); break;
        case 0x2C: snprintf(buf,sz,"SUBC %s,%s,%s",reg_name(rd),reg_name(rs1),reg_name(rs2)); break;
        case 0x2D: snprintf(buf,sz,"LSLR %s,%s,%s",reg_name(rd),reg_name(rs1),reg_name(rs2)); break;
        case 0x2E: snprintf(buf,sz,"LSRR %s,%s,%s",reg_name(rd),reg_name(rs1),reg_name(rs2)); break;
        case 0x2F: snprintf(buf,sz,"ASRR %s,%s,%s",reg_name(rd),reg_name(rs1),reg_name(rs2)); break;
        case 0x30: snprintf(buf,sz,"LWX  %s,[%s+%s]",reg_name(rd),reg_name(rs1),reg_name(rs2)); break;
        case 0x31: snprintf(buf,sz,"LBX  %s,[%s+%s]",reg_name(rd),reg_name(rs1),reg_name(rs2)); break;
        case 0x32: snprintf(buf,sz,"LBUX %s,[%s+%s]",reg_name(rd),reg_name(rs1),reg_name(rs2)); break;
        case 0x33: snprintf(buf,sz,"SWX  %s,[%s+%s]",reg_name(rd),reg_name(rs1),reg_name(rs2)); break;
        case 0x34: snprintf(buf,sz,"SBX  %s,[%s+%s]",reg_name(rd),reg_name(rs1),reg_name(rs2)); break;
        case 0x35: snprintf(buf,sz,"LHX  %s,[%s+%s]",reg_name(rd),reg_name(rs1),reg_name(rs2)); break;
        case 0x36: snprintf(buf,sz,"LHUX %s,[%s+%s]",reg_name(rd),reg_name(rs1),reg_name(rs2)); break;
        case 0x37: snprintf(buf,sz,"SHX  %s,[%s+%s]",reg_name(rd),reg_name(rs1),reg_name(rs2)); break;
        case 0x38: snprintf(buf,sz,"LUI  %s,0x%04X",reg_name(rd),imm16); break;
        case 0x39: snprintf(buf,sz,"ROLR %s,%s,%s",reg_name(rd),reg_name(rs1),reg_name(rs2)); break;
        case 0x3A: snprintf(buf,sz,"RORR %s,%s,%s",reg_name(rd),reg_name(rs1),reg_name(rs2)); break;
        case 0x3B: snprintf(buf,sz,"ROLI %s,%s,%u",reg_name(rd),reg_name(rs1),shift); break;
        case 0x3C: snprintf(buf,sz,"RORI %s,%s,%u",reg_name(rd),reg_name(rs1),shift); break;
        case 0x3D: snprintf(buf,sz,"CAS  %s,%s,%s",reg_name(rd),reg_name(rs1),reg_name(rs2)); break;
        default:   snprintf(buf,sz,"??? (0x%08X)",instr); break;
    }
}

/* ── Debugger ────────────────────────────────────────────────────────────── */
#define MAX_BREAKPOINTS 16
static uint32_t breakpoints[MAX_BREAKPOINTS];
static int num_breakpoints = 0;

static void dump_regs(CPU *cpu) {
    for (int i = 0; i < 16; i++) {
        fprintf(stderr, "  %s=0x%08X", reg_name(i), cpu->r[i]);
        if ((i & 3) == 3) fprintf(stderr, "\n");
    }
    fprintf(stderr, "  flags=0x%08X  status=0x%08X  cause=0x%08X\n",
            cpu->flags, cpu->status, cpu->cause);
}

static void dump_mem(uint32_t addr, int len) {
    for (int i = 0; i < len; i += 16) {
        fprintf(stderr, "  0x%08X: ", addr+i);
        for (int j = 0; j < 16 && i+j < len; j++)
            fprintf(stderr, "%02X ", (addr+i+j < MEM_SIZE) ? mem[addr+i+j] : 0);
        fprintf(stderr, " ");
        for (int j = 0; j < 16 && i+j < len; j++) {
            unsigned char c = (addr+i+j < MEM_SIZE) ? mem[addr+i+j] : 0;
            fprintf(stderr, "%c", (c>=32&&c<127)?c:'.');
        }
        fprintf(stderr, "\n");
    }
}

static int debug_step; /* 1=single-step, 0=running */

static void debugger_prompt(CPU *cpu, int debug) {
    char line[256];
    while (1) {
        uint32_t pc = cpu->r[15];
        char dis[80];
        uint32_t instr = (pc < MEM_SIZE) ? mem_read32(pc) : 0;
        disasm(pc, instr, dis, sizeof(dis));
        uint32_t op_byte = instr >> 24;
        if (debug == 2 && (op_byte == 0x0A || op_byte == 0x24 || op_byte == 0x25 || op_byte == 0x26)) {
            /* DIV/DIVU/MOD/MODU: dump rs1 and rs2 values for debugging */
            int _rd  = (instr >> 20) & 0xF;
            int _rs1 = (instr >> 16) & 0xF;
            int _rs2 = (instr >> 12) & 0xF;
            fprintf(stderr, "[DIVDBG] 0x%08X: %s  r%d=0x%08X(%d) r%d=0x%08X(%d)\n",
                    pc, dis, _rs1, cpu->r[_rs1], (int32_t)cpu->r[_rs1],
                    _rs2, cpu->r[_rs2], (int32_t)cpu->r[_rs2]);
        }
        fprintf(stderr, "[DBG] 0x%08X: %-40s > ", pc, dis);
        fflush(stderr);
        if (debug == 2) {
            fprintf(stderr, "\n");
            break; // Keep going for '-e' (ie "emit" mode)
        }
        if (!fgets(line, sizeof(line), stdin)) { cpu->halted=1; return; }
        /* strip newline */
        line[strcspn(line,"\n")] = 0;

        if (line[0]==0 || strcmp(line,"s")==0 || strcmp(line,"step")==0) {
            debug_step = 1; return;
        } else if (strcmp(line,"cont")==0 || strcmp(line,"c")==0) {
            debug_step = 0; return;
        } else if (strcmp(line,"regs")==0) {
            dump_regs(cpu);
        } else if (strncmp(line,"break ",6)==0) {
            if (num_breakpoints < MAX_BREAKPOINTS) {
                uint32_t a = (uint32_t)strtoul(line+6,NULL,0);
                breakpoints[num_breakpoints++] = a;
                fprintf(stderr, "  Breakpoint set at 0x%08X\n", a);
            }
        } else if (strncmp(line,"mem ",4)==0) {
            uint32_t a; int l=64;
            sscanf(line+4, "%i %i", (int*)&a, &l);
            dump_mem(a, l);
        } else {
            fprintf(stderr, "  Commands: [s]tep, [c]ont, regs, break <addr>, mem <addr> [len]\n");
        }
    }
}

/* ── Execute loop ────────────────────────────────────────────────────────── */
static void cpu_run(CPU *cpu, int debug) {
    debug_step = debug;
    while (!cpu->halted) {
        uint32_t pc = cpu->r[15];
        cpu->faulting_pc = pc; /* save before any advance for exception EPC */

        /* breakpoint check */
        if (debug) {
            for (int i = 0; i < num_breakpoints; i++) {
                if (breakpoints[i] == pc) {
                    fprintf(stderr, "  Hit breakpoint at 0x%08X\n", pc);
                    debug_step = 1; break;
                }
            }
            if (debug_step) debugger_prompt(cpu, debug_step);
            if (cpu->halted) break;
            pc = cpu->r[15]; /* may have changed in debugger */
        }

        /* fetch */
        if (pc >= MEM_SIZE) { raise_exception(cpu, 0x02); timer_tick(cpu); interrupt_check(cpu); continue; }
        uint32_t instr = mem_read32(pc);
        cpu->r[15] = pc + 4;

        /* decode */
        uint8_t  op    = (instr >> 24) & 0xFF;
        uint8_t  rd    = (instr >> 20) & 0x0F;
        uint8_t  rs1   = (instr >> 16) & 0x0F;
        uint8_t  rs2   = (instr >> 12) & 0x0F;
        uint8_t  shift = (instr >>  7) & 0x1F;
        uint16_t imm16 = (uint16_t)(instr & 0xFFFF);
        int32_t  simm  = (int32_t)(int16_t)imm16;
        uint32_t off20 = instr & 0xFFFFF;
        int32_t  soff  = (off20 & 0x80000) ? (int32_t)(off20 | 0xFFF00000u) : (int32_t)off20;

        switch (op) {
        /* ── R-type ALU ── */
        case 0x00: { uint32_t r = cpu->r[rs1]+cpu->r[rs2]; update_flags_add(cpu,cpu->r[rs1],cpu->r[rs2],r); cpu->r[rd]=r; break; }
        case 0x01: { uint32_t r = cpu->r[rs1]-cpu->r[rs2]; update_flags_sub(cpu,cpu->r[rs1],cpu->r[rs2],r); cpu->r[rd]=r; break; }
        case 0x02: { uint32_t r = cpu->r[rs1]&cpu->r[rs2]; update_flags_nz(cpu,r); clear_cv(cpu); cpu->r[rd]=r; break; }
        case 0x03: { uint32_t r = cpu->r[rs1]|cpu->r[rs2]; update_flags_nz(cpu,r); clear_cv(cpu); cpu->r[rd]=r; break; }
        case 0x04: { uint32_t r = cpu->r[rs1]^cpu->r[rs2]; update_flags_nz(cpu,r); clear_cv(cpu); cpu->r[rd]=r; break; }
        case 0x05: { uint32_t r = ~cpu->r[rs1];             update_flags_nz(cpu,r); clear_cv(cpu); cpu->r[rd]=r; break; }
        case 0x06: { uint32_t r = cpu->r[rs1]<<shift;       update_flags_nz(cpu,r); clear_cv(cpu); cpu->r[rd]=r; break; }
        case 0x07: { uint32_t r = cpu->r[rs1]>>shift;       update_flags_nz(cpu,r); clear_cv(cpu); cpu->r[rd]=r; break; }
        case 0x08: { uint32_t r = (uint32_t)((int32_t)cpu->r[rs1]>>shift); update_flags_nz(cpu,r); clear_cv(cpu); cpu->r[rd]=r; break; }
        case 0x27: { cpu->r[rd]=cpu->r[rs1]; break; }

        /* ── Compare (flags only, no rd write) ── */
        case 0x28: { uint32_t r=cpu->r[rs1]-cpu->r[rs2]; update_flags_sub(cpu,cpu->r[rs1],cpu->r[rs2],r); break; }
        case 0x29: { uint32_t r=cpu->r[rs1]-(uint32_t)imm16; update_flags_sub(cpu,cpu->r[rs1],(uint32_t)imm16,r); break; }

        /* ── Register indirect call ── */
        case 0x2A: { uint32_t target=cpu->r[rs1]; cpu->r[rd]=cpu->r[15]; cpu->r[15]=target; break; }

        /* ── Add/subtract with carry/borrow ── */
        case 0x2B: { /* ADDC */
            uint32_t cin = (cpu->flags & FLAG_C) ? 1u : 0u;
            uint64_t sum = (uint64_t)cpu->r[rs1] + (uint64_t)cpu->r[rs2] + cin;
            uint32_t r = (uint32_t)sum;
            int carry    = sum > 0xFFFFFFFFull;
            int overflow = (!((cpu->r[rs1]^cpu->r[rs2]) & 0x80000000u)) && ((r^cpu->r[rs1]) & 0x80000000u);
            cpu->flags = (cpu->flags & ~(FLAG_N|FLAG_Z|FLAG_C|FLAG_V))
                       | (r & 0x80000000u ? FLAG_N : 0)
                       | (r == 0          ? FLAG_Z : 0)
                       | (carry           ? FLAG_C : 0)
                       | (overflow        ? FLAG_V : 0);
            cpu->r[rd] = r; break; }
        case 0x2C: { /* SUBC */
            uint32_t bin = (cpu->flags & FLAG_C) ? 1u : 0u;
            uint64_t sub = (uint64_t)cpu->r[rs2] + bin;
            uint32_t r   = cpu->r[rs1] - cpu->r[rs2] - bin;
            int borrow   = sub > (uint64_t)cpu->r[rs1];
            int overflow = ((cpu->r[rs1]^cpu->r[rs2]) & 0x80000000u) && ((r^cpu->r[rs1]) & 0x80000000u);
            cpu->flags = (cpu->flags & ~(FLAG_N|FLAG_Z|FLAG_C|FLAG_V))
                       | (r & 0x80000000u ? FLAG_N : 0)
                       | (r == 0          ? FLAG_Z : 0)
                       | (borrow          ? FLAG_C : 0)
                       | (overflow        ? FLAG_V : 0);
            cpu->r[rd] = r; break; }

        /* ── Indexed memory ── */
        case 0x30: { uint32_t a=cpu->r[rs1]+cpu->r[rs2]; cpu->r[rd]=cpu_read32(cpu,a); break; }
        case 0x31: { uint32_t a=cpu->r[rs1]+cpu->r[rs2]; cpu->r[rd]=(uint32_t)(int32_t)(int8_t)cpu_read8(cpu,a); break; }
        case 0x32: { uint32_t a=cpu->r[rs1]+cpu->r[rs2]; cpu->r[rd]=cpu_read8(cpu,a); break; }
        case 0x33: { uint32_t a=cpu->r[rs1]+cpu->r[rs2]; cpu_write32(cpu,a,cpu->r[rd]); break; }
        case 0x34: { uint32_t a=cpu->r[rs1]+cpu->r[rs2]; cpu_write8(cpu,a,(uint8_t)(cpu->r[rd]&0xFF)); break; }
        case 0x35: { uint32_t a=cpu->r[rs1]+cpu->r[rs2]; cpu->r[rd]=(uint32_t)(int32_t)(int16_t)cpu_read16(cpu,a); break; }
        case 0x36: { uint32_t a=cpu->r[rs1]+cpu->r[rs2]; cpu->r[rd]=cpu_read16(cpu,a); break; }
        case 0x37: { uint32_t a=cpu->r[rs1]+cpu->r[rs2]; cpu_write16(cpu,a,(uint16_t)(cpu->r[rd]&0xFFFF)); break; }

        /* ── LUI / Rotate ── */
        case 0x38: { cpu->r[rd] = (uint32_t)imm16 << 16; break; }
        case 0x39: { uint32_t sh=cpu->r[rs2]&0x1F; uint32_t r=sh?(cpu->r[rs1]<<sh)|(cpu->r[rs1]>>(32u-sh)):cpu->r[rs1]; update_flags_nz(cpu,r); clear_cv(cpu); cpu->r[rd]=r; break; } /* ROLR */
        case 0x3A: { uint32_t sh=cpu->r[rs2]&0x1F; uint32_t r=sh?(cpu->r[rs1]>>sh)|(cpu->r[rs1]<<(32u-sh)):cpu->r[rs1]; update_flags_nz(cpu,r); clear_cv(cpu); cpu->r[rd]=r; break; } /* RORR */
        case 0x3B: { uint32_t sh=shift;             uint32_t r=sh?(cpu->r[rs1]<<sh)|(cpu->r[rs1]>>(32u-sh)):cpu->r[rs1]; update_flags_nz(cpu,r); clear_cv(cpu); cpu->r[rd]=r; break; } /* ROLI */
        case 0x3C: { uint32_t sh=shift;             uint32_t r=sh?(cpu->r[rs1]>>sh)|(cpu->r[rs1]<<(32u-sh)):cpu->r[rs1]; update_flags_nz(cpu,r); clear_cv(cpu); cpu->r[rd]=r; break; } /* RORI */
        case 0x3D: { /* CAS rd, rs1, rs2 — compare-and-swap */
            uint32_t addr = cpu->r[rs1];
            if (addr & 3) { raise_exception(cpu, 0x01); break; }
            if (addr >= MEM_SIZE) { raise_exception(cpu, 0x02); break; }
            uint32_t cur = (addr >= MMIO_BASE) ? mmio_read(cpu, addr) : mem_read32(addr);
            if (cur == cpu->r[rd]) {
                if (addr >= MMIO_BASE) mmio_write(cpu, addr, cpu->r[rs2]);
                else mem_write32(addr, cpu->r[rs2]);
                cpu->flags |= FLAG_Z;
            } else {
                cpu->r[rd] = cur;
                cpu->flags &= ~FLAG_Z;
            }
            break;
        }

        /* ── Register shifts ── */
        case 0x2D: { uint32_t r = cpu->r[rs1]<<(cpu->r[rs2]&0x1F);       update_flags_nz(cpu,r); clear_cv(cpu); cpu->r[rd]=r; break; }
        case 0x2E: { uint32_t r = cpu->r[rs1]>>(cpu->r[rs2]&0x1F);       update_flags_nz(cpu,r); clear_cv(cpu); cpu->r[rd]=r; break; }
        case 0x2F: { uint32_t r=(uint32_t)((int32_t)cpu->r[rs1]>>(cpu->r[rs2]&0x1F)); update_flags_nz(cpu,r); clear_cv(cpu); cpu->r[rd]=r; break; }

        /* ── Multiply/Divide ── */
        case 0x09: { uint32_t r=(uint32_t)((uint64_t)cpu->r[rs1]*(uint64_t)cpu->r[rs2]); update_flags_nz(cpu,r); cpu->r[rd]=r; break; }
        case 0x22: { int64_t  r=((int64_t)(int32_t)cpu->r[rs1]*(int64_t)(int32_t)cpu->r[rs2])>>32; update_flags_nz(cpu,(uint32_t)r); cpu->r[rd]=(uint32_t)r; break; }
        case 0x23: { uint64_t r=((uint64_t)cpu->r[rs1]*(uint64_t)cpu->r[rs2])>>32; update_flags_nz(cpu,(uint32_t)r); cpu->r[rd]=(uint32_t)r; break; }
        case 0x0A: if (!cpu->r[rs2]){raise_exception(cpu,0x04);break;} { int32_t r=(int32_t)cpu->r[rs1]/(int32_t)cpu->r[rs2]; update_flags_nz(cpu,(uint32_t)r); cpu->r[rd]=(uint32_t)r; break; }
        case 0x24: if (!cpu->r[rs2]){raise_exception(cpu,0x04);break;} { uint32_t r=cpu->r[rs1]/cpu->r[rs2]; update_flags_nz(cpu,r); cpu->r[rd]=r; break; }
        case 0x25: if (!cpu->r[rs2]){raise_exception(cpu,0x04);break;} { int32_t r=(int32_t)cpu->r[rs1]%(int32_t)cpu->r[rs2]; update_flags_nz(cpu,(uint32_t)r); cpu->r[rd]=(uint32_t)r; break; }
        case 0x26: if (!cpu->r[rs2]){raise_exception(cpu,0x04);break;} { uint32_t r=cpu->r[rs1]%cpu->r[rs2]; update_flags_nz(cpu,r); cpu->r[rd]=r; break; }

        /* ── I-type ALU ── */
        case 0x14: { uint32_t r=cpu->r[rs1]+(uint32_t)simm; update_flags_add(cpu,cpu->r[rs1],(uint32_t)simm,r); cpu->r[rd]=r; break; }
        case 0x15: { uint32_t r=cpu->r[rs1]-(uint32_t)simm; update_flags_sub(cpu,cpu->r[rs1],(uint32_t)simm,r); cpu->r[rd]=r; break; }
        case 0x16: { uint32_t r=cpu->r[rs1]&imm16;          update_flags_nz(cpu,r); clear_cv(cpu); cpu->r[rd]=r; break; }
        case 0x17: { uint32_t r=cpu->r[rs1]|imm16;          update_flags_nz(cpu,r); clear_cv(cpu); cpu->r[rd]=r; break; }
        case 0x18: { uint32_t r=cpu->r[rs1]^imm16;          update_flags_nz(cpu,r); clear_cv(cpu); cpu->r[rd]=r; break; }
        case 0x19: { uint32_t r=cpu->r[rs1]<<(imm16&0x1F);  update_flags_nz(cpu,r); clear_cv(cpu); cpu->r[rd]=r; break; }
        case 0x1A: { uint32_t r=cpu->r[rs1]>>(imm16&0x1F);  update_flags_nz(cpu,r); clear_cv(cpu); cpu->r[rd]=r; break; }
        case 0x1B: { uint32_t r=(uint32_t)((int32_t)cpu->r[rs1]>>(imm16&0x1F)); update_flags_nz(cpu,r); clear_cv(cpu); cpu->r[rd]=r; break; }

        /* ── MOV immediate ── */
        case 0x0F: { cpu->r[rd]=imm16; break; }
        case 0x13: { cpu->r[rd]=(cpu->r[rd]&0xFFFF)|(((uint32_t)imm16)<<16); break; }

        /* ── Loads ── */
        case 0x0B: { uint32_t a=(uint32_t)((int32_t)cpu->r[rs1]+simm); cpu->r[rd]=cpu_read32(cpu,a); break; }
        case 0x1C: { uint32_t a=(uint32_t)((int32_t)cpu->r[rs1]+simm); cpu->r[rd]=(uint32_t)(int32_t)(int16_t)cpu_read16(cpu,a); break; }
        case 0x1D: { uint32_t a=(uint32_t)((int32_t)cpu->r[rs1]+simm); cpu->r[rd]=cpu_read16(cpu,a); break; }
        case 0x1E: { uint32_t a=(uint32_t)((int32_t)cpu->r[rs1]+simm); cpu->r[rd]=(uint32_t)(int32_t)(int8_t)cpu_read8(cpu,a); break; }
        case 0x1F: { uint32_t a=(uint32_t)((int32_t)cpu->r[rs1]+simm); cpu->r[rd]=cpu_read8(cpu,a); break; }

        /* ── Stores ── */
        case 0x0C: { uint32_t a=(uint32_t)((int32_t)cpu->r[rs1]+simm); cpu_write32(cpu,a,cpu->r[rd]); break; }
        case 0x20: { uint32_t a=(uint32_t)((int32_t)cpu->r[rs1]+simm); cpu_write16(cpu,a,(uint16_t)(cpu->r[rd]&0xFFFF)); break; }
        case 0x21: { uint32_t a=(uint32_t)((int32_t)cpu->r[rs1]+simm); cpu_write8(cpu,a,(uint8_t)(cpu->r[rd]&0xFF)); break; }

        /* ── Branch ── */
        case 0x0D: {
            uint8_t cond = rd; /* cond field is bits 23-20 = rd field */
            int taken = 0;
            switch (cond) {
                case 0: taken = (cpu->flags & FLAG_Z) != 0; break;
                case 1: taken = (cpu->flags & FLAG_Z) == 0; break;
                case 2: { int n=(cpu->flags&FLAG_N)!=0, v=(cpu->flags&FLAG_V)!=0; taken=n^v; break; }
                case 3: { int n=(cpu->flags&FLAG_N)!=0, v=(cpu->flags&FLAG_V)!=0; taken=!(n^v); break; }
                case 4: taken = (cpu->flags & FLAG_C) != 0; break;
                case 5: taken = (cpu->flags & FLAG_C) == 0; break;
                case 6: taken = 1; break; /* BA — always */
                case 7: { int n=(cpu->flags&FLAG_N)!=0, v=(cpu->flags&FLAG_V)!=0, z=(cpu->flags&FLAG_Z)!=0; taken=!z && !(n^v); break; } /* BGT */
                case 8: { int n=(cpu->flags&FLAG_N)!=0, v=(cpu->flags&FLAG_V)!=0, z=(cpu->flags&FLAG_Z)!=0; taken=z || (n^v); break; } /* BLE */
                case 9: { int c=(cpu->flags&FLAG_C)!=0, z=(cpu->flags&FLAG_Z)!=0; taken=!c && !z; break; } /* BGTU */
                case 10:{ int c=(cpu->flags&FLAG_C)!=0, z=(cpu->flags&FLAG_Z)!=0; taken=c || z; break; } /* BLEU */
                default: raise_exception(cpu, 0x00); goto next_instr;
            }
            if (taken) cpu->r[15] = pc + (uint32_t)soff;
            break;
        }

        /* ── Jump ── */
        case 0x0E:
            cpu->r[rd] = cpu->r[15]; /* already pc+4 */
            cpu->r[15] = pc + (uint32_t)soff;
            break;

        /* ── Syscall/Sysret/Halt ── */
        case 0x10: /* SYSCALL */
            if (cpu->status & 1) { cpu->halted=1; break; } /* double fault */
            cpu->estatus = cpu->status; /* save STATUS before clobbering it */
            cpu->epc    = cpu->r[15]; /* already pc+4 */
            cpu->eflags = cpu->flags;
            cpu->cause  = 0x03;
            cpu->status = 0x01;
            { uint32_t hv = cpu->evec + 3*4; cpu->r[15] = (hv < MEM_SIZE) ? mem_read32(hv) : 0; }
            break;

        case 0x11: /* SYSRET */
            if (!(cpu->status & 1)) { raise_exception(cpu, 0x00); break; }
            cpu->r[15]  = cpu->epc;
            cpu->flags  = cpu->eflags;
            cpu->status = cpu->estatus & ~1u; /* restore IE from ESTATUS; force user mode */
            break;

        case 0x12: /* HALT */
            cpu->halted = 1;
            break;

        default:
            raise_exception(cpu, 0x00);
            break;
        }

        next_instr:
        timer_tick(cpu);
        if (++cpu->uart_poll_divider >= 1000) { cpu->uart_poll_divider = 0; uart_poll(cpu); }
        if (!cpu->halted) interrupt_check(cpu);
    }
}

/* ── CPU reset ───────────────────────────────────────────────────────────── */
static void cpu_reset(CPU *cpu) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->status = 0x01; /* supervisor, IE=0 */
    cpu->estatus = 0x02; /* user-mode + IE=1; default for first SYSRET */
}

/* ── ELF loader ──────────────────────────────────────────────────────────── */
static uint16_t rd_le16(const uint8_t *p) {
    return (uint16_t)((unsigned)p[0] | ((unsigned)p[1] << 8));
}
static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)((unsigned)p[0] | ((unsigned)p[1]<<8) |
                      ((unsigned)p[2]<<16) | ((unsigned)p[3]<<24));
}

/* Returns entry point address on success, (uint32_t)-1 on error. */
static uint32_t load_elf(FILE *f, const char *path) {
    uint8_t hdr[52];
    rewind(f);
    if (fread(hdr, 1, 52, f) != 52) {
        fprintf(stderr, "%s: truncated ELF header\n", path); return (uint32_t)-1;
    }
    if (hdr[4] != 1) { /* EI_CLASS != ELFCLASS32 */
        fprintf(stderr, "%s: not a 32-bit ELF\n", path); return (uint32_t)-1;
    }

    uint32_t e_entry    = rd_le32(hdr + 24);
    uint32_t e_phoff    = rd_le32(hdr + 28);
    uint16_t e_phentsize= rd_le16(hdr + 42);
    uint16_t e_phnum    = rd_le16(hdr + 44);

    for (int i = 0; i < e_phnum; i++) {
        uint8_t ph[32];
        if (fseek(f, (long)(e_phoff + (uint32_t)i * e_phentsize), SEEK_SET) != 0 ||
            fread(ph, 1, 32, f) != 32) {
            fprintf(stderr, "%s: can't read program header %d\n", path, i);
            return (uint32_t)-1;
        }
        if (rd_le32(ph + 0) != 1) continue; /* PT_LOAD = 1 */
        uint32_t p_offset = rd_le32(ph + 4);
        uint32_t p_vaddr  = rd_le32(ph + 8);
        uint32_t p_filesz = rd_le32(ph + 16);
        uint32_t p_memsz  = rd_le32(ph + 20);
        if (p_vaddr + p_memsz > MEM_SIZE) {
            fprintf(stderr, "%s: segment 0x%08X+0x%X exceeds memory\n",
                    path, p_vaddr, p_memsz);
            return (uint32_t)-1;
        }
        if (p_filesz > 0) {
            if (fseek(f, (long)p_offset, SEEK_SET) != 0 ||
                fread(mem + p_vaddr, 1, p_filesz, f) != p_filesz) {
                fprintf(stderr, "%s: can't read segment data\n", path);
                return (uint32_t)-1;
            }
        }
        if (p_memsz > p_filesz)
            memset(mem + p_vaddr + p_filesz, 0, p_memsz - p_filesz);
    }
    return e_entry;
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    static CPU cpu;
    cpu_reset(&cpu);

    int debug = 0;
    const char *binary = NULL;
    uint32_t load_addr = 0;
    const char *blk_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            debug = 1;
        } else if (strcmp(argv[i], "-e") == 0) {
            debug = 2;
        } else if (strcmp(argv[i], "-blk") == 0 && i+1 < argc) {
            blk_path = argv[++i];
        } else if (strcmp(argv[i], "-a") == 0 && i+1 < argc) {
            load_addr = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (!binary) {
            binary = argv[i];
        }
    }

    if (!binary) {
        fprintf(stderr, "Usage: emulatortwo [-d] [-e] [-a load_addr] [-blk disk.img] <binary>\n");
        fprintf(stderr, "-d is 'debugger', '-e' is 'emit trace'\n");
        return 1;
    }

    FILE *f = fopen(binary, "rb");
    if (!f) { perror(binary); return 1; }

    /* Detect ELF by magic */
    uint8_t magic[4] = {0};
    (void)fread(magic, 1, 4, f);
    int is_elf = (magic[0]==0x7f && magic[1]=='E' && magic[2]=='L' && magic[3]=='F');

    if (is_elf) {
        uint32_t entry = load_elf(f, binary);
        fclose(f);
        if (entry == (uint32_t)-1) return 1;
        cpu.r[15] = entry;
    } else {
        rewind(f);
        size_t n = fread(mem + load_addr, 1, MEM_SIZE - load_addr, f);
        fclose(f);
        (void)n;
        cpu.r[15] = load_addr;
    }

    if (blk_path) {
        cpu.blk_file = fopen(blk_path, "r+b");
        if (!cpu.blk_file) { perror(blk_path); return 1; }
    }

    cpu_run(&cpu, debug);

    if (cpu.blk_file) fclose(cpu.blk_file);
    return cpu.halted ? 0 : (int)cpu.cause;
}
