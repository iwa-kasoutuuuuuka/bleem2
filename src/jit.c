#include <kos.h>
#include "mips.h"
#include <stdio.h>
#include <string.h>
#include <malloc.h>

/**
 * Bleem2 - jit.c (encodings verified against sh-elf-as 2026-10-15; disabled via USE_JIT)
 */

typedef struct {
    u32 mips_pc;
    void (*sh4_ptr)(mips_t *cpu, mem_t *mem, gpu_t *gpu);
    int num_instr;
} jit_entry_t;

#define JIT_LOG_SIZE 14
#define JIT_ENTRIES (1 << JIT_LOG_SIZE) // 16384 entries
static jit_entry_t cache[JIT_ENTRIES];

static uint16_t *code_ptr_base = NULL;
static uint16_t *code_ptr_curr = NULL;

void jit_flush_cache() {
    code_ptr_curr = code_ptr_base;
    memset(cache, 0, sizeof(cache));
    for (int i=0; i<JIT_ENTRIES; i++) cache[i].mips_pc = 0xFFFFFFFF;
    printf("JIT: Cache Flushed (DirectMapped SMC OK)\n");
    dcache_flush_range((uintptr_t)code_ptr_base, 1024 * 1024);
    icache_flush_range((uintptr_t)code_ptr_base, 1024 * 1024);
    __asm__ __volatile__("synco");
}

void jit_invalidate(uint32_t addr, uint32_t len) {
    uint32_t start = addr & ~3;
    uint32_t end = (addr + len + 3) & ~3;
    for (uint32_t a = start; a < end; a += 4) {
        uint32_t hash = (a >> 2) & (JIT_ENTRIES - 1);
        if ((cache[hash].mips_pc & 0x1FFFFFFF) == (a & 0x1FFFFFFF)) {
            cache[hash].mips_pc = 0xFFFFFFFF;
            cache[hash].sh4_ptr = NULL;
        }
    }
    __asm__ __volatile__("synco");
}

void jit_init() {
    code_ptr_base = (uint16_t *)memalign(4096, 1024 * 1024);
    jit_flush_cache();
}

/* SH-4 32-bit Immediate Load (Correct Literal Pool with Safe Branch) */
static void emit_sh4_mov_imm32(uint16_t **ptr, int reg, uint32_t val) {
    uint16_t *p = *ptr;
    if ((uintptr_t)p & 2) { *p++ = 0x0009; } // 4-byte align P
    
    /* P:    MOV.L @(PC, 4), reg  (Loads from (P+4 & ~3) + 4 = P+8)
       P+2:  BRA Target           (Target = (P+4) + (disp*2))
       P+4:  NOP                  (Delay Slot)
       P+6:  NOP                  (Alignment padding)
       P+8:  .long val            (Data: P+8 and P+10)
       P+12: Target               (Next Instruction)
       
       Target = P + 12. 12 = 4 + (disp*2) => 8 = disp*2 => disp = 4.
    */
    *p++ = 0xD000 | (reg << 8) | 0x01; // MOV.L @(PC, 4), reg
    *p++ = 0xA003;                     // BRA +3 words: target = (P+2)+4+6 = P+12
    *p++ = 0x0009;                     // NOP
    *p++ = 0x0009;                     // Padding
    *(uint32_t *)p = val;
    p += 2;
    *ptr = p;
}

static void emit_sh4_load_reg(uint16_t **ptr, int mips_reg, int sh4_reg) {
    uint32_t disp = mips_reg * 4;
    emit_sh4_mov_imm32(ptr, 0, disp);
    /* MOV.L @(R0, R8), sh4_reg : 0000 nnnn mmmm 0110 (n=sh4_reg, m=8) */
    *(*ptr)++ = 0x0000 | (sh4_reg << 8) | (8 << 4) | 0x0E; /* MOV.L @(R0,R8),Rn = 0000nnnn10001110 */
}

static void emit_sh4_store_reg(uint16_t **ptr, int sh4_reg, int mips_reg) {
    uint32_t disp = mips_reg * 4;
    emit_sh4_mov_imm32(ptr, 0, disp);
    /* MOV.L sh4_reg, @(R0, R8) : 0000 nnnn mmmm 0010 (n=8, m=sh4_reg) */
    *(*ptr)++ = 0x0000 | (8 << 8) | (sh4_reg << 4) | 0x06; /* MOV.L Rm,@(R0,R8) = 00001000mmmm0110 */
}

int jit_execute(mips_t *cpu, mem_t *mem, gpu_t *gpu) {
    if (cpu->pc & 3) return 0;
    
    /* [SAFEGUARD] PC 範囲チェック */
    if (cpu->pc < 0x80000000 || (cpu->pc > 0x801FFFFF && cpu->pc < 0xBFC00000) || cpu->pc > 0xBFC7FFFF) {
        return 0; 
    }
    
    uint32_t hash = (cpu->pc >> 2) & (JIT_ENTRIES - 1);
    if (cache[hash].mips_pc == cpu->pc && cache[hash].sh4_ptr != NULL) {
        cache[hash].sh4_ptr(cpu, mem, gpu);
        return cache[hash].num_instr;
    }

    if (code_ptr_curr > (code_ptr_base + 1024*1024 - 4096)) {
        jit_flush_cache();
        hash = (cpu->pc >> 2) & (JIT_ENTRIES - 1);
    }

    uint32_t mips_pc = cpu->pc;
    uint32_t instr = mem_read32(cpu, mem, gpu, mips_pc);
    
    if (((uintptr_t)code_ptr_curr) & 3) code_ptr_curr++;

    uint16_t *start = code_ptr_curr;
    uint16_t *ptr = start;

    /* PROLOGUE (8 regs = 32 bytes) */
    *ptr++ = 0x4F22; // STS.L PR, @-R15
    *ptr++ = 0x2F86; // MOV.L R8, @-R15
    *ptr++ = 0x2F96; // MOV.L R9, @-R15
    *ptr++ = 0x2FA6; // MOV.L R10, @-R15
    *ptr++ = 0x2FB6; // MOV.L R11, @-R15
    *ptr++ = 0x2FC6; // MOV.L R12, @-R15
    *ptr++ = 0x2FD6; // MOV.L R13, @-R15
    *ptr++ = 0x2FE6; // MOV.L R14, @-R15
    
    *ptr++ = 0x6843; // R4=R8
    *ptr++ = 0x6953; // R5=R9
    *ptr++ = 0x6A63; // R6=R10

    int compiled = 0;
    uint8_t op = (instr >> 26) & 0x3F;

    if (op == 0x00) { /* SPECIAL */
        uint8_t func = instr & 0x3F;
        int rs = (instr >> 21) & 0x1F, rt = (instr >> 16) & 0x1F, rd = (instr >> 11) & 0x1F;
        int sa = (instr >> 6) & 0x1F;
        
        if (func == 0x00 && rd != 0) { /* SLL */
            emit_sh4_load_reg(&ptr, rt, 1);
            emit_sh4_mov_imm32(&ptr, 2, sa);
            *ptr++ = 0x412D; // SHLD R2, R1
            emit_sh4_store_reg(&ptr, 1, rd);
            compiled = 1;
        } else if (func == 0x02 && rd != 0) { /* SRL */
            emit_sh4_load_reg(&ptr, rt, 1);
            emit_sh4_mov_imm32(&ptr, 2, -sa);
            *ptr++ = 0x412D; // SHLD R2, R1
            emit_sh4_store_reg(&ptr, 1, rd);
            compiled = 1;
        } else if (func == 0x21 && rd != 0) { /* ADDU */
            emit_sh4_load_reg(&ptr, rs, 1);
            emit_sh4_load_reg(&ptr, rt, 2);
            *ptr++ = 0x312C; // ADD R2, R1
            emit_sh4_store_reg(&ptr, 1, rd);
            compiled = 1;
        } else if (func == 0x23 && rd != 0) { /* SUBU */
            emit_sh4_load_reg(&ptr, rs, 1);
            emit_sh4_load_reg(&ptr, rt, 2);
            *ptr++ = 0x3128; // SUB R2, R1
            emit_sh4_store_reg(&ptr, 1, rd);
            compiled = 1;
        } else if (func == 0x24 && rd != 0) { /* AND */
            emit_sh4_load_reg(&ptr, rs, 1);
            emit_sh4_load_reg(&ptr, rt, 2);
            *ptr++ = 0x2129; // AND R2, R1
            emit_sh4_store_reg(&ptr, 1, rd);
            compiled = 1;
        } else if (func == 0x25 && rd != 0) { /* OR */
            emit_sh4_load_reg(&ptr, rs, 1);
            emit_sh4_load_reg(&ptr, rt, 2);
            *ptr++ = 0x212B; // OR R2, R1
            emit_sh4_store_reg(&ptr, 1, rd);
            compiled = 1;
        } else if (func == 0x26 && rd != 0) { /* XOR */
            emit_sh4_load_reg(&ptr, rs, 1);
            emit_sh4_load_reg(&ptr, rt, 2);
            *ptr++ = 0x212A; // XOR R2, R1
            emit_sh4_store_reg(&ptr, 1, rd);
            compiled = 1;
        }
    } else if (op == 0x08 || op == 0x09) { /* ADDI/ADDIU */
        int rs = (instr >> 21) & 0x1F, rt = (instr >> 16) & 0x1F;
        int16_t imm = (int16_t)(instr & 0xFFFF);
        if (rt != 0) {
            emit_sh4_load_reg(&ptr, rs, 1);
            emit_sh4_mov_imm32(&ptr, 2, (uint32_t)imm);
            *ptr++ = 0x312C; // ADD R2, R1
            emit_sh4_store_reg(&ptr, 1, rt);
        }
        compiled = 1;
    } else if (op == 0x0A || op == 0x0B) { /* SLTI/SLTIU */
        int rs = (instr >> 21) & 0x1F, rt = (instr >> 16) & 0x1F;
        int16_t imm = (int16_t)(instr & 0xFFFF);
        if (rt != 0) {
            emit_sh4_load_reg(&ptr, rs, 1);
            emit_sh4_mov_imm32(&ptr, 2, (uint32_t)imm);
            if (op == 0x0A) *ptr++ = 0x3123; // CMP/GE R2, R1
            else *ptr++ = 0x3122;           // CMP/HS R2, R1
            *ptr++ = 0x0129; // MOVT R1
            *ptr++ = 0x71FF; // ADD #-1, R1
            *ptr++ = 0x611B; // NEG R1, R1
            emit_sh4_store_reg(&ptr, 1, rt);
        }
        compiled = 1;
    } else if (op == 0x0D) { /* ORI */
        int rs = (instr >> 21) & 0x1F, rt = (instr >> 16) & 0x1F;
        uint32_t uimm = (uint32_t)(instr & 0xFFFF);
        if (rt != 0) {
            emit_sh4_load_reg(&ptr, rs, 1);
            emit_sh4_mov_imm32(&ptr, 2, uimm);
            *ptr++ = 0x212B; // OR R2, R1
            emit_sh4_store_reg(&ptr, 1, rt);
        }
        compiled = 1;
    } else if (op == 0x0F) { /* LUI */
        int rt = (instr >> 16) & 0x1F;
        uint32_t val = (instr & 0xFFFF) << 16;
        if (rt != 0) {
            emit_sh4_mov_imm32(&ptr, 1, val);
            emit_sh4_store_reg(&ptr, 1, rt);
        }
        compiled = 1;
    } else if (op == 0x20) { /* LB */
        int rs = (instr >> 21) & 0x1F, rt = (instr >> 16) & 0x1F;
        int16_t imm = (int16_t)(instr & 0xFFFF);
        emit_sh4_load_reg(&ptr, rs, 6);
        emit_sh4_mov_imm32(&ptr, 0, (uint32_t)imm);
        *ptr++ = 0x360C; // ADD R0, R6
        *ptr++ = 0x6483; *ptr++ = 0x6593; // R4=R8, R5=R9
        emit_sh4_mov_imm32(&ptr, 1, (uint32_t)mem_read8);
        *ptr++ = 0x410B; *ptr++ = 0x0009; // JSR, NOP
        if (rt != 0) {
            *ptr++ = 0x6103; // MOV R0, R1
            *ptr++ = 0x611E; // EXTS.B R1, R1
            emit_sh4_store_reg(&ptr, 1, rt);
        }
        compiled = 1;
    } else if (op == 0x24) { /* LBU */
        int rs = (instr >> 21) & 0x1F, rt = (instr >> 16) & 0x1F;
        int16_t imm = (int16_t)(instr & 0xFFFF);
        emit_sh4_load_reg(&ptr, rs, 6);
        emit_sh4_mov_imm32(&ptr, 0, (uint32_t)imm);
        *ptr++ = 0x360C; // ADD R0, R6
        *ptr++ = 0x6483; *ptr++ = 0x6593; // R4=R8, R5=R9
        emit_sh4_mov_imm32(&ptr, 1, (uint32_t)mem_read8);
        *ptr++ = 0x410B; *ptr++ = 0x0009;
        if (rt != 0) {
            *ptr++ = 0x6103; // MOV R0, R1
            *ptr++ = 0x611C; // EXTU.B R1, R1
            emit_sh4_store_reg(&ptr, 1, rt);
        }
        compiled = 1;
    } else if (op == 0x23) { /* LW */
        int rs = (instr >> 21) & 0x1F, rt = (instr >> 16) & 0x1F;
        int16_t imm = (int16_t)(instr & 0xFFFF);
        emit_sh4_load_reg(&ptr, rs, 7);
        emit_sh4_mov_imm32(&ptr, 0, (uint32_t)imm);
        *ptr++ = 0x370C; // ADD R0, R7
        *ptr++ = 0x6483; *ptr++ = 0x6593; *ptr++ = 0x66A3; // Args
        emit_sh4_mov_imm32(&ptr, 1, (uint32_t)mem_read32);
        *ptr++ = 0x410B; *ptr++ = 0x0009; 
        if (rt != 0) {
            *ptr++ = 0x6103; // MOV R0, R1
            emit_sh4_store_reg(&ptr, 1, rt);
        }
        compiled = 1;
    } else if (op == 0x28) { /* SB */
        int rs = (instr >> 21) & 0x1F, rt = (instr >> 16) & 0x1F;
        int16_t imm = (int16_t)(instr & 0xFFFF);
        *ptr++ = 0x6483; *ptr++ = 0x6593; // R4=R8, R5=R9
        emit_sh4_load_reg(&ptr, rs, 6);
        emit_sh4_mov_imm32(&ptr, 0, (uint32_t)imm);
        *ptr++ = 0x360C; // ADD R0, R6
        emit_sh4_load_reg(&ptr, rt, 7); // val
        emit_sh4_mov_imm32(&ptr, 1, (uint32_t)mem_write8);
        *ptr++ = 0x410B; *ptr++ = 0x0009;
        compiled = 1;
    } else if (op == 0x2B) { /* SW */
        int rs = (instr >> 21) & 0x1F, rt = (instr >> 16) & 0x1F;
        int16_t imm = (int16_t)(instr & 0xFFFF);
        emit_sh4_load_reg(&ptr, rs, 7);
        emit_sh4_mov_imm32(&ptr, 0, (uint32_t)imm);
        *ptr++ = 0x370C; // ADD R0, R7
        emit_sh4_load_reg(&ptr, rt, 1);
        
        /* [SAFE STACK] Always 8-byte align */
        *ptr++ = 0x7FF8; // ADD #-8, R15
        *ptr++ = 0x1F10; // MOV.L R1, @(0, R15)
        
        *ptr++ = 0x6483; *ptr++ = 0x6593; *ptr++ = 0x66A3; 
        emit_sh4_mov_imm32(&ptr, 1, (uint32_t)mem_write32);
        *ptr++ = 0x410B; *ptr++ = 0x0009;
        
        *ptr++ = 0x7F08; // ADD #8, R15
        compiled = 1;
    }

    if (compiled) {
        emit_sh4_mov_imm32(&ptr, 1, mips_pc + 4);
        emit_sh4_store_reg(&ptr, 1, 32); 
        
        /* EPILOGUE */
        *ptr++ = 0x6EF6; // MOV.L @R15+, R14
        *ptr++ = 0x6DF6; // MOV.L @R15+, R13
        *ptr++ = 0x6CF6; // MOV.L @R15+, R12
        *ptr++ = 0x6BF6; // MOV.L @R15+, R11
        *ptr++ = 0x6AF6; // MOV.L @R15+, R10
        *ptr++ = 0x69F6; // MOV.L @R15+, R9
        *ptr++ = 0x68F6; // MOV.L @R15+, R8
        *ptr++ = 0x4F26; // LDS.L @R15+, PR
        *ptr++ = 0x000B; // RTS
        *ptr++ = 0x0009; // NOP

        dcache_flush_range((uintptr_t)start, (uintptr_t)ptr - (uintptr_t)start);
        icache_flush_range((uintptr_t)start, (uintptr_t)ptr - (uintptr_t)start);
        __asm__ __volatile__("synco");

        cache[hash].mips_pc = mips_pc;
        cache[hash].sh4_ptr = (void (*)(mips_t*, mem_t*, gpu_t*))start;
        cache[hash].num_instr = 1;
        code_ptr_curr = ptr;
        
        cache[hash].sh4_ptr(cpu, mem, gpu);
        return 1;
    }
    return 0;
}
