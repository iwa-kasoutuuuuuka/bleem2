#include "mips.h"
#include <stdio.h>
#include <string.h>

/*
 * MIPS R3000A interpreter: full base ISA, COP0 exceptions, delay slots.
 * Load delay slots are not emulated (same simplification as PCSX).
 *
 * decode_and_execute() returns the address of the next instruction to run:
 * pc + 4 normally, or the branch target when a branch/jump is taken (the
 * caller then executes the delay slot first).
 */

extern uint64_t total_cycles_executed;
extern uint64_t jit_cycles_executed;
uint64_t prof_sched_us = 0;
u32 g_irq_hist[16];
uint64_t prof_jit_us = 0, prof_interp_us = 0; u32 stat_interp_instr = 0, stat_jit_calls = 0;

#define SR    cop0[12]
#define CAUSE cop0[13]
#define EPC   cop0[14]
#define BADV  cop0[8]

#define CPU_TRACE_PC 1      /* keep a ring of recent PCs for crash dumps */

enum {
    EXC_INT = 0, EXC_ADEL = 4, EXC_ADES = 5, EXC_IBE = 6, EXC_DBE = 7,
    EXC_SYS = 8, EXC_BP = 9, EXC_RI = 10, EXC_CPU = 11, EXC_OV = 12
};

static u32 decode_and_execute(mips_t *cpu, mem_t *mem, gpu_t *gpu, u32 instr, u32 pc);

void cpu_reset(mips_t *cpu) {
    memset(cpu->r, 0, sizeof(cpu->r));
    memset(cpu->cop0, 0, sizeof(cpu->cop0));
    memset(cpu->gte_dr, 0, sizeof(cpu->gte_dr));
    memset(cpu->gte_cr, 0, sizeof(cpu->gte_cr));
    cpu->pc = 0xBFC00000;
    cpu->hi = cpu->lo = 0;
    cpu->SR = 0x10900000;       /* BEV=1, TS, CU0 */
    cpu->cop0[15] = 0x00000002; /* PRID */
    cpu->i_stat = 0;
    cpu->i_mask = 0;
    cpu->stop = 0;
    cpu->exc_pending = 0;
    cpu->pad_state = 0xFFFF;
}

void cpu_raise(mips_t *cpu, u32 code, u32 badvaddr) {
    cpu->exc_pending = 1;
    cpu->exc_code = code;
    cpu->exc_badvaddr = badvaddr;
}

static void cpu_take_exception(mips_t *cpu, u32 code, u32 epc, int in_bd) {
    u32 sr = cpu->SR;
    cpu->CAUSE = (cpu->CAUSE & ~0x8000007Cu) | (code << 2) | (in_bd ? 0x80000000u : 0);
    cpu->EPC = epc;
    cpu->SR = (sr & ~0x3Fu) | ((sr << 2) & 0x3C);
    cpu->pc = (sr & (1u << 22)) ? 0xBFC00180 : 0x80000080;
    cpu->exc_count++;
    cpu->last_exc_pc = epc;
    cpu->last_exc_code = code;
    if (cpu->exc_count <= 16 && code != EXC_INT)
        dlog("EXC %lu code %lu at %08lX bad %08lX a0=%08lX", (unsigned long)cpu->exc_count,
             (unsigned long)code, (unsigned long)epc, (unsigned long)cpu->BADV, (unsigned long)cpu->r[4]);
}

/* ---- fast memory paths (RAM inline, everything else via mem.c) ---- */
static inline u32 fetch32(mips_t *cpu, mem_t *mem, gpu_t *gpu, u32 pc) {
    u32 p = pc & 0x1FFFFFFF;
    if (p < 0x00800000) return *(u32 *)&mem->ram[p & 0x1FFFFF];
    if (p >= 0x1FC00000 && p < 0x1FC80000) return *(u32 *)&mem->bios[p - 0x1FC00000];
    return mem_read32(cpu, mem, gpu, pc);
}
static inline u32 rd32(mips_t *cpu, mem_t *mem, gpu_t *gpu, u32 a) {
    u32 p = a & 0x1FFFFFFF;
    if (p < 0x00800000) return *(u32 *)&mem->ram[p & 0x1FFFFF];
    return mem_read32(cpu, mem, gpu, a);
}
static inline u16 rd16(mips_t *cpu, mem_t *mem, gpu_t *gpu, u32 a) {
    u32 p = a & 0x1FFFFFFF;
    if (p < 0x00800000) return *(u16 *)&mem->ram[p & 0x1FFFFF];
    return mem_read16(cpu, mem, gpu, a);
}
static inline u8 rd8(mips_t *cpu, mem_t *mem, u32 a) {
    u32 p = a & 0x1FFFFFFF;
    if (p < 0x00800000) return mem->ram[p & 0x1FFFFF];
    return mem_read8(cpu, mem, a);
}
u32 g_pc_samples[PC_SAMPLES];
u32 g_sample_tick = 0;
int g_jit_verify_running = 0;   /* set while the JIT re-runs a block through the interpreter */
u32 g_watch_addr = 0x000959E8;     /* physical address watched for writes (debug) */
static int watch_hits = 0;
static inline void watch_store(mips_t *cpu, u32 p, u32 v, int width) {
    if ((p & ~3u) == (g_watch_addr & ~3u) && watch_hits < 24) {
        watch_hits++;
        dlog("WATCH w%d %08lX <- %08lX pc=%08lX ra=%08lX", width, (unsigned long)p, (unsigned long)v,
             (unsigned long)cpu->pc_history[(cpu->pc_history_pos - 1) & (PC_HISTORY_SIZE - 1)], (unsigned long)cpu->r[31]);
    }
}

static inline void wr32(mips_t *cpu, mem_t *mem, gpu_t *gpu, u32 a, u32 v) {
    u32 p = a & 0x1FFFFFFF;
    watch_store(cpu, p, v, 32);
    if (p < 0x00800000) { if (!(cpu->SR & 0x10000)) *(u32 *)&mem->ram[p & 0x1FFFFF] = v; return; }
    mem_write32(cpu, mem, gpu, a, v);
}
static inline void wr16(mips_t *cpu, mem_t *mem, gpu_t *gpu, u32 a, u16 v) {
    u32 p = a & 0x1FFFFFFF;
    watch_store(cpu, p, v, 16);
    if (p < 0x00800000) { if (!(cpu->SR & 0x10000)) *(u16 *)&mem->ram[p & 0x1FFFFF] = v; return; }
    mem_write16(cpu, mem, gpu, a, v);
}
static inline void wr8(mips_t *cpu, mem_t *mem, u32 a, u8 v) {
    u32 p = a & 0x1FFFFFFF;
    watch_store(cpu, p, v, 8);
    if (p < 0x00800000) { if (!(cpu->SR & 0x10000)) mem->ram[p & 0x1FFFFF] = v; return; }
    mem_write8(cpu, mem, a, v);
}

void cpu_execute(mips_t *cpu, mem_t *mem, gpu_t *gpu, int cycles) {
    int cyc = 0;
    int sched = 0;
    u32 pc = cpu->pc;

    while (cyc < cycles) {
        /* ---- hardware interrupt ---- */
        if (!g_jit_verify_running && (cpu->i_stat & cpu->i_mask)) {
            cpu->CAUSE |= 0x400;
            if (!cpu->hle_enabled && (cpu->SR & 0x401) == 0x401) {
                cpu->pc = pc;
                cpu_take_exception(cpu, EXC_INT, pc, 0);
                cpu->irq_count++;
                { u32 m = cpu->i_stat & cpu->i_mask; for (int b = 0; b < 11; b++) if (m & (1u << b)) g_irq_hist[b]++; }
                pc = cpu->pc;
            }
        } else {
            cpu->CAUSE &= ~0x400u;
        }

        /* ---- HLE BIOS trap (autoboot mode only) ---- */
        if (cpu->hle_enabled) {
            u32 ppc = pc & 0x1FFFFFFF;
            if (ppc == 0xA0 || ppc == 0xB0 || ppc == 0xC0) {
                cpu->pc = pc;
                hle_bios_call(cpu, mem, gpu, ppc);
                pc = cpu->pc;
                cyc += 16;
                if (cpu->stop) break;
                continue;
            }
        }

#if CPU_TRACE_PC
        cpu->pc_history[cpu->pc_history_pos++ & (PC_HISTORY_SIZE - 1)] = pc;
#endif

        if (pc & 3) {
            cpu->BADV = pc;
            cpu_take_exception(cpu, EXC_ADEL, pc, 0);
            pc = cpu->pc;
            cyc++;
            continue;
        }

#if USE_JIT
        if (!g_jit_verify_running) {
            cpu->pc = pc;
            int jit_ran = jit_run(cpu, mem, gpu, cycles - cyc);
            stat_jit_calls++;
            pc = cpu->pc;
            if (jit_ran > 0) {
                cyc += jit_ran;
                jit_cycles_executed += jit_ran;
                if (cpu->stop) break;
                continue;
            }
        }
#endif
        stat_interp_instr++;
        u32 instr = fetch32(cpu, mem, gpu, pc);
        u32 next = decode_and_execute(cpu, mem, gpu, instr, pc);
        cpu->r[0] = 0;

        if (cpu->exc_pending) {
            cpu->exc_pending = 0;
            cpu->BADV = cpu->exc_badvaddr;
            if (cpu->hle_enabled && cpu->exc_code == EXC_SYS) {
                hle_syscall(cpu);
                pc += 4;
            } else {
                cpu_take_exception(cpu, cpu->exc_code, pc, 0);
                pc = cpu->pc;
            }
            cyc++;
        } else if (next != pc + 4) {
            /* taken branch: run the delay slot, then jump */
            u32 ds = fetch32(cpu, mem, gpu, pc + 4);
            decode_and_execute(cpu, mem, gpu, ds, pc + 4);
            cpu->r[0] = 0;
            if (cpu->exc_pending) {
                cpu->exc_pending = 0;
                cpu->BADV = cpu->exc_badvaddr;
                if (cpu->hle_enabled && cpu->exc_code == EXC_SYS) {
                    hle_syscall(cpu);
                    pc = next;
                } else {
                    cpu_take_exception(cpu, cpu->exc_code, pc, 1);
                    pc = cpu->pc;
                }
            } else {
                pc = next;
            }
            cyc += 2;
        } else {
            pc = next;
            cyc++;
        }

        if (++sched >= 64 && !g_jit_verify_running) {
            sched = 0;
            total_cycles_executed += 64;
            if ((++g_sample_tick & 31) == 0) g_pc_samples[(g_sample_tick >> 5) & (PC_SAMPLES - 1)] = pc;
            timers_update(cpu, 64);
            cd_update(cpu, 64);
            sio_update(cpu, 64);
            if (cpu->stop) break;
        }
    }
    cpu->pc = pc;
}

/* ---- unaligned-word helpers (little endian) ---- */
static const u32 LWL_MASK[4]  = { 0x00FFFFFF, 0x0000FFFF, 0x000000FF, 0x00000000 };
static const int LWL_SHIFT[4] = { 24, 16, 8, 0 };
static const u32 LWR_MASK[4]  = { 0x00000000, 0xFF000000, 0xFFFF0000, 0xFFFFFF00 };
static const int LWR_SHIFT[4] = { 0, 8, 16, 24 };
static const u32 SWL_MASK[4]  = { 0xFFFFFF00, 0xFFFF0000, 0xFF000000, 0x00000000 };
static const int SWL_SHIFT[4] = { 24, 16, 8, 0 };
static const u32 SWR_MASK[4]  = { 0x00000000, 0x000000FF, 0x0000FFFF, 0x00FFFFFF };
static const int SWR_SHIFT[4] = { 0, 8, 16, 24 };

#define BRANCH(cond) do { if (cond) next = pc + 4 + (imm << 2); } while (0)

static u32 decode_and_execute(mips_t *cpu, mem_t *mem, gpu_t *gpu, u32 instr, u32 pc) {
    u32 next = pc + 4;
    u32 op = instr >> 26;
    int rs = (instr >> 21) & 0x1F;
    int rt = (instr >> 16) & 0x1F;
    int rd = (instr >> 11) & 0x1F;
    s32 imm = (s16)(instr & 0xFFFF);
    u32 uimm = instr & 0xFFFF;
    u32 *r = cpu->r;

    switch (op) {
    case 0x00: { /* SPECIAL */
        u32 func = instr & 0x3F;
        int sa = (instr >> 6) & 0x1F;
        switch (func) {
        case 0x00: r[rd] = r[rt] << sa; break;                           /* SLL   */
        case 0x02: r[rd] = r[rt] >> sa; break;                           /* SRL   */
        case 0x03: r[rd] = (u32)((s32)r[rt] >> sa); break;               /* SRA   */
        case 0x04: r[rd] = r[rt] << (r[rs] & 0x1F); break;               /* SLLV  */
        case 0x06: r[rd] = r[rt] >> (r[rs] & 0x1F); break;               /* SRLV  */
        case 0x07: r[rd] = (u32)((s32)r[rt] >> (r[rs] & 0x1F)); break;   /* SRAV  */
        case 0x08: next = r[rs]; break;                                  /* JR    */
        case 0x09: { u32 t = r[rs]; r[rd] = pc + 8; next = t; break; }   /* JALR  */
        case 0x0C: cpu_raise(cpu, EXC_SYS, 0); break;                    /* SYSCALL */
        case 0x0D: cpu_raise(cpu, EXC_BP, 0); break;                     /* BREAK */
        case 0x10: r[rd] = cpu->hi; break;                               /* MFHI  */
        case 0x11: cpu->hi = r[rs]; break;                               /* MTHI  */
        case 0x12: r[rd] = cpu->lo; break;                               /* MFLO  */
        case 0x13: cpu->lo = r[rs]; break;                               /* MTLO  */
        case 0x18: { int64_t v = (int64_t)(s32)r[rs] * (int64_t)(s32)r[rt];
                     cpu->lo = (u32)v; cpu->hi = (u32)(v >> 32); break; } /* MULT  */
        case 0x19: { uint64_t v = (uint64_t)r[rs] * (uint64_t)r[rt];
                     cpu->lo = (u32)v; cpu->hi = (u32)(v >> 32); break; } /* MULTU */
        case 0x1A: {                                                     /* DIV   */
            s32 n = (s32)r[rs], d = (s32)r[rt];
            if (d == 0) { cpu->lo = (n >= 0) ? 0xFFFFFFFF : 1; cpu->hi = (u32)n; }
            else if ((u32)n == 0x80000000 && d == -1) { cpu->lo = 0x80000000; cpu->hi = 0; }
            else { cpu->lo = (u32)(n / d); cpu->hi = (u32)(n % d); }
            break;
        }
        case 0x1B: {                                                     /* DIVU  */
            u32 n = r[rs], d = r[rt];
            if (d == 0) { cpu->lo = 0xFFFFFFFF; cpu->hi = n; }
            else { cpu->lo = n / d; cpu->hi = n % d; }
            break;
        }
        case 0x20: {                                                     /* ADD   */
            u32 a = r[rs], b = r[rt], s = a + b;
            if (~(a ^ b) & (a ^ s) & 0x80000000) cpu_raise(cpu, EXC_OV, 0); else r[rd] = s;
            break;
        }
        case 0x21: r[rd] = r[rs] + r[rt]; break;                         /* ADDU  */
        case 0x22: {                                                     /* SUB   */
            u32 a = r[rs], b = r[rt], s = a - b;
            if ((a ^ b) & (a ^ s) & 0x80000000) cpu_raise(cpu, EXC_OV, 0); else r[rd] = s;
            break;
        }
        case 0x23: r[rd] = r[rs] - r[rt]; break;                         /* SUBU  */
        case 0x24: r[rd] = r[rs] & r[rt]; break;                         /* AND   */
        case 0x25: r[rd] = r[rs] | r[rt]; break;                         /* OR    */
        case 0x26: r[rd] = r[rs] ^ r[rt]; break;                         /* XOR   */
        case 0x27: r[rd] = ~(r[rs] | r[rt]); break;                      /* NOR   */
        case 0x2A: r[rd] = ((s32)r[rs] < (s32)r[rt]) ? 1 : 0; break;     /* SLT   */
        case 0x2B: r[rd] = (r[rs] < r[rt]) ? 1 : 0; break;               /* SLTU  */
        default:   cpu_raise(cpu, EXC_RI, 0); break;
        }
        break;
    }
    case 0x01: {                                                         /* REGIMM */
        int cond = (rt & 1) ? ((s32)r[rs] >= 0) : ((s32)r[rs] < 0);
        if (rt & 0x10) r[31] = pc + 8;                                   /* BLTZAL/BGEZAL */
        BRANCH(cond);
        break;
    }
    case 0x02: next = (pc & 0xF0000000) | ((instr & 0x03FFFFFF) << 2); break;                   /* J    */
    case 0x03: r[31] = pc + 8; next = (pc & 0xF0000000) | ((instr & 0x03FFFFFF) << 2); break;   /* JAL  */
    case 0x04: BRANCH(r[rs] == r[rt]); break;                            /* BEQ  */
    case 0x05: BRANCH(r[rs] != r[rt]); break;                            /* BNE  */
    case 0x06: BRANCH((s32)r[rs] <= 0); break;                           /* BLEZ */
    case 0x07: BRANCH((s32)r[rs] > 0); break;                            /* BGTZ */
    case 0x08: {                                                         /* ADDI */
        u32 a = r[rs], b = (u32)imm, s = a + b;
        if (~(a ^ b) & (a ^ s) & 0x80000000) cpu_raise(cpu, EXC_OV, 0); else r[rt] = s;
        break;
    }
    case 0x09: r[rt] = r[rs] + (u32)imm; break;                          /* ADDIU */
    case 0x0A: r[rt] = ((s32)r[rs] < imm) ? 1 : 0; break;                /* SLTI  */
    case 0x0B: r[rt] = (r[rs] < (u32)imm) ? 1 : 0; break;                /* SLTIU */
    case 0x0C: r[rt] = r[rs] & uimm; break;                              /* ANDI  */
    case 0x0D: r[rt] = r[rs] | uimm; break;                              /* ORI   */
    case 0x0E: r[rt] = r[rs] ^ uimm; break;                              /* XORI  */
    case 0x0F: r[rt] = uimm << 16; break;                                /* LUI   */
    case 0x10: {                                                         /* COP0  */
        if (instr & (1 << 25)) {
            if ((instr & 0x3F) == 0x10) {                                /* RFE */
                u32 sr = cpu->SR;
                cpu->SR = (sr & ~0xFu) | ((sr >> 2) & 0xF);
            }
        } else {
            switch (rs) {
            case 0x00: r[rt] = cpu->cop0[rd]; break;                     /* MFC0 */
            case 0x04:                                                   /* MTC0 */
                if (rd == 13) cpu->CAUSE = (cpu->CAUSE & ~0x300u) | (r[rt] & 0x300);
                else if (rd != 15 && rd != 8) cpu->cop0[rd] = r[rt];
                break;
            default: break;
            }
        }
        break;
    }
    case 0x12: {                                                         /* COP2 (GTE) */
        if (instr & (1 << 25)) {
            gte_execute(cpu, instr);
        } else {
            switch (rs) {
            case 0x00: r[rt] = cpu->gte_dr[rd]; break;                   /* MFC2 */
            case 0x02: r[rt] = cpu->gte_cr[rd]; break;                   /* CFC2 */
            case 0x04: cpu->gte_dr[rd] = r[rt]; break;                   /* MTC2 */
            case 0x06: cpu->gte_cr[rd] = r[rt]; break;                   /* CTC2 */
            default: break;
            }
        }
        break;
    }
    case 0x20: r[rt] = (u32)(s32)(s8)rd8(cpu, mem, r[rs] + imm); break;                       /* LB  */
    case 0x21: { u32 a = r[rs] + imm;                                                         /* LH  */
                 if (a & 1) cpu_raise(cpu, EXC_ADEL, a);
                 else r[rt] = (u32)(s32)(s16)rd16(cpu, mem, gpu, a);
                 break; }
    case 0x22: { u32 a = r[rs] + imm; u32 m = rd32(cpu, mem, gpu, a & ~3u); int s = a & 3;    /* LWL */
                 r[rt] = (r[rt] & LWL_MASK[s]) | (m << LWL_SHIFT[s]); break; }
    case 0x23: { u32 a = r[rs] + imm;                                                         /* LW  */
                 if (a & 3) cpu_raise(cpu, EXC_ADEL, a);
                 else r[rt] = rd32(cpu, mem, gpu, a);
                 break; }
    case 0x24: r[rt] = rd8(cpu, mem, r[rs] + imm); break;                                     /* LBU */
    case 0x25: { u32 a = r[rs] + imm;                                                         /* LHU */
                 if (a & 1) cpu_raise(cpu, EXC_ADEL, a);
                 else r[rt] = rd16(cpu, mem, gpu, a);
                 break; }
    case 0x26: { u32 a = r[rs] + imm; u32 m = rd32(cpu, mem, gpu, a & ~3u); int s = a & 3;    /* LWR */
                 r[rt] = (r[rt] & LWR_MASK[s]) | (m >> LWR_SHIFT[s]); break; }
    case 0x28: wr8(cpu, mem, r[rs] + imm, (u8)r[rt]); break;                                  /* SB  */
    case 0x29: { u32 a = r[rs] + imm;                                                         /* SH  */
                 if (a & 1) cpu_raise(cpu, EXC_ADES, a);
                 else wr16(cpu, mem, gpu, a, (u16)r[rt]);
                 break; }
    case 0x2A: { u32 a = r[rs] + imm; u32 al = a & ~3u; int s = a & 3;                        /* SWL */
                 u32 m = rd32(cpu, mem, gpu, al);
                 m = (m & SWL_MASK[s]) | (r[rt] >> SWL_SHIFT[s]);
                 wr32(cpu, mem, gpu, al, m); break; }
    case 0x2B: { u32 a = r[rs] + imm;                                                         /* SW  */
                 if (a & 3) cpu_raise(cpu, EXC_ADES, a);
                 else wr32(cpu, mem, gpu, a, r[rt]);
                 break; }
    case 0x2E: { u32 a = r[rs] + imm; u32 al = a & ~3u; int s = a & 3;                        /* SWR */
                 u32 m = rd32(cpu, mem, gpu, al);
                 m = (m & SWR_MASK[s]) | (r[rt] << SWR_SHIFT[s]);
                 wr32(cpu, mem, gpu, al, m); break; }
    case 0x32: { u32 a = r[rs] + imm;                                                         /* LWC2 */
                 if (a & 3) cpu_raise(cpu, EXC_ADEL, a);
                 else cpu->gte_dr[rt] = rd32(cpu, mem, gpu, a);
                 break; }
    case 0x3A: { u32 a = r[rs] + imm;                                                         /* SWC2 */
                 if (a & 3) cpu_raise(cpu, EXC_ADES, a);
                 else wr32(cpu, mem, gpu, a, cpu->gte_dr[rt]);
                 break; }
    case 0x11: case 0x13: cpu_raise(cpu, EXC_CPU, 0); break;             /* COP1/COP3 unusable */
    default:   cpu_raise(cpu, EXC_RI, 0); break;
    }
    return next;
}
