#include "mips.h"
#include <math.h>
#include <stdio.h>

/* SH-4 バンク切り替えを用いた行列ロード定数 */
#define FPSCR_FR (1 << 21)

/**
 * GTE 行列を SH-4 の XMTRX バンクへロード
 * PS1 GTE 回転行列: 16bit 符号付き固定小数点 (3.13) -> 1.0f = 4096
 */
void gte_load_matrix(mips_t *cpu) {
    float m[16] __attribute__((aligned(32)));
    
    /* 
     * PS1 GTE CR 0-4 に回転行列が格納されている (16bit x 9個)
     * R11 R12 | R13 R21 | R22 R23 | R31 R32 | R33 ...
     */
    int16_t r11 = (int16_t)(cpu->gte_cr[0] & 0xFFFF);
    int16_t r12 = (int16_t)(cpu->gte_cr[0] >> 16);
    int16_t r13 = (int16_t)(cpu->gte_cr[1] & 0xFFFF);
    int16_t r21 = (int16_t)(cpu->gte_cr[1] >> 16);
    int16_t r22 = (int16_t)(cpu->gte_cr[2] & 0xFFFF);
    int16_t r23 = (int16_t)(cpu->gte_cr[2] >> 16);
    int16_t r31 = (int16_t)(cpu->gte_cr[3] & 0xFFFF);
    int16_t r32 = (int16_t)(cpu->gte_cr[3] >> 16);
    int16_t r33 = (int16_t)(cpu->gte_cr[4] & 0xFFFF);

    /* 4x4 行列に展開 (Translation は含めない、ftrv の後に手動加算の方が PS1 仕様に合わせやすい) */
    m[0] = (float)r11 / 4096.0f; m[1] = (float)r12 / 4096.0f; m[2] = (float)r13 / 4096.0f; m[3] = 0.0f;
    m[4] = (float)r21 / 4096.0f; m[5] = (float)r22 / 4096.0f; m[6] = (float)r23 / 4096.0f; m[7] = 0.0f;
    m[8] = (float)r31 / 4096.0f; m[9] = (float)r32 / 4096.0f; m[10] = (float)r33 / 4096.0f; m[11] = 0.0f;
    m[12] = 0.0f; m[13] = 0.0f; m[14] = 0.0f; m[15] = 1.0f;

    uint32_t old_fpscr = 0, new_fpscr = 0;
    __asm__ __volatile__ (
        "sts fpscr, %0\n"
        "mov %0, %1\n"
        "xor %3, %1\n"
        "lds %1, fpscr\n" 
        "fmov.s @%2+, fr0\n" "fmov.s @%2+, fr1\n" "fmov.s @%2+, fr2\n" "fmov.s @%2+, fr3\n"
        "fmov.s @%2+, fr4\n" "fmov.s @%2+, fr5\n" "fmov.s @%2+, fr6\n" "fmov.s @%2+, fr7\n"
        "fmov.s @%2+, fr8\n" "fmov.s @%2+, fr9\n" "fmov.s @%2+, fr10\n" "fmov.s @%2+, fr11\n"
        "fmov.s @%2+, fr12\n" "fmov.s @%2+, fr13\n" "fmov.s @%2+, fr14\n" "fmov.s @%2+, fr15\n"
        "lds %0, fpscr\n" 
        : "=&r" (old_fpscr), "=&r" (new_fpscr)
        : "r" (m), "r" (FPSCR_FR) : "memory"
    );
}

/**
 * RTPS: 1頂点変換
 */
void gte_rtps(mips_t *cpu) {
    gte_load_matrix(cpu); // 行列ロード

    float vx = (float)(int16_t)(cpu->gte_dr[0] & 0xFFFF);
    float vy = (float)(int16_t)(cpu->gte_dr[1] & 0xFFFF);
    float vz = (float)(int16_t)(cpu->gte_dr[2] & 0xFFFF);
    
    register float f0 __asm__("fr0") = vx;
    register float f1 __asm__("fr1") = vy;
    register float f2 __asm__("fr2") = vz;
    register float f3 __asm__("fr3") = 1.0f;

    __asm__ __volatile__ ( "ftrv xmtrx, fv0\n" : "+f" (f0), "+f" (f1), "+f" (f2), "+f" (f3) :: "memory" );

    /* 平行移動 (Translation) 加算: GTE CR 5-7 (TRX, TRY, TRZ) */
    f0 += (float)(int32_t)cpu->gte_cr[5];
    f1 += (float)(int32_t)cpu->gte_cr[6];
    f2 += (float)(int32_t)cpu->gte_cr[7];

    float h = (float)(uint16_t)cpu->gte_cr[18]; if (h == 0) h = 512.0f;
    float sz = f2; if (sz < 1.0f) sz = 1.0f; // 0除算防止
    
    float res_x = f0 * h / sz;
    float res_y = f1 * h / sz;
    
    uint32_t flags = 0;
    // 異常値の飽和処理とフラグセット
    if (res_x > 32767.0f) { res_x = 32767.0f; flags |= (1<<31); }
    if (res_x < -32768.0f) { res_x = -32768.0f; flags |= (1<<31); }
    if (res_y > 32767.0f) { res_y = 32767.0f; flags |= (1<<31); }
    if (res_y < -32768.0f) { res_y = -32768.0f; flags |= (1<<31); }
    if (sz < 1.0f) { flags |= (1<<18); } // FIFO overflow/Error

    cpu->gte_dr[12] = (int16_t)res_x; 
    cpu->gte_dr[13] = (int16_t)res_y; 
    cpu->gte_dr[14] = (uint16_t)((uint32_t)sz >> 2); // PS1 SZ is often scaled
    cpu->gte_cr[31] = flags;
}

/**
 * RTP3: 3頂点同時変換 (高速化の核心)
 */
void gte_rtp3(mips_t *cpu) {
    gte_load_matrix(cpu);

    float h = (float)(uint16_t)cpu->gte_cr[18]; if (h == 0) h = 512.0f;
    float trx = (float)(int32_t)cpu->gte_cr[5];
    float try_ = (float)(int32_t)cpu->gte_cr[6];
    float trz = (float)(int32_t)cpu->gte_cr[7];

    uint32_t flags = 0;

    for (int i = 0; i < 3; i++) {
        float vx = (float)(int16_t)(cpu->gte_dr[i*2] & 0xFFFF);
        float vy = (float)(int16_t)(cpu->gte_dr[i*2] >> 16);
        float vz = (float)(int16_t)(cpu->gte_dr[i*2 + 1] & 0xFFFF);
        
        register float f0 __asm__("fr0") = vx;
        register float f1 __asm__("fr1") = vy;
        register float f2 __asm__("fr2") = vz;
        register float f3 __asm__("fr3") = 1.0f;

        __asm__ __volatile__ ( "ftrv xmtrx, fv0\n" : "+f" (f0), "+f" (f1), "+f" (f2), "+f" (f3) :: "memory" );

        f0 += trx; f1 += try_; f2 += trz;
        float sz = f2; if (sz < 1.0f) { sz = 1.0f; flags |= (1<<18); }

        float res_x = f0 * h / sz;
        float res_y = f1 * h / sz;
        
        if (res_x > 32767.0f) { res_x = 32767.0f; flags |= (1<<31); }
        if (res_x < -32768.0f) { res_x = -32768.0f; flags |= (1<<31); }
        if (res_y > 32767.0f) { res_y = 32767.0f; flags |= (1<<31); }
        if (res_y < -32768.0f) { res_y = -32768.0f; flags |= (1<<31); }

        /* SXY0, SXY1, SXY2 レジスタ (DR12, DR13, DR14) */
        cpu->gte_dr[12 + i] = ((uint16_t)(int16_t)res_x) | ((uint32_t)(uint16_t)(int16_t)res_y << 16);
        /* SZ0, SZ1, SZ2 レジスタ (DR17, DR18, DR19) */
        cpu->gte_dr[17 + i] = (uint16_t)((uint32_t)sz >> 2);
    }
    cpu->gte_cr[31] = flags;
}

/**
 * NCLIP: 法線ベクトルによるカリング判定
 */
void gte_nclip(mips_t *cpu) {
    /* [Bloom Port] NCLIP (Normal Clip) */
    int16_t x0 = (int16_t)(cpu->gte_dr[12] & 0xFFFF);
    int16_t y0 = (int16_t)(cpu->gte_dr[12] >> 16);
    int16_t x1 = (int16_t)(cpu->gte_dr[13] & 0xFFFF);
    int16_t y1 = (int16_t)(cpu->gte_dr[13] >> 16);
    int16_t x2 = (int16_t)(cpu->gte_dr[14] & 0xFFFF);
    int16_t y2 = (int16_t)(cpu->gte_dr[14] >> 16);
    
    cpu->gte_dr[24] = (int32_t)x0*(y1-y2) + (int32_t)x1*(y2-y0) + (int32_t)x2*(y0-y1);
}

/**
 * MVMVA: 行列xベクトル + 平行移動 (非常に多用される命令)
 */
void gte_mvmva(mips_t *cpu, u32 instr) {
    gte_load_matrix(cpu);
    
    /* ベクトル選択 (V0, V1, V2 or IR) */
    int v_idx = (instr >> 15) & 3;
    float vx, vy, vz;
    if (v_idx < 3) {
        vx = (float)(int16_t)(cpu->gte_dr[v_idx*2] & 0xFFFF);
        vy = (float)(int16_t)(cpu->gte_dr[v_idx*2] >> 16);
        vz = (float)(int16_t)(cpu->gte_dr[v_idx*2 + 1] & 0xFFFF);
    } else {
        vx = (float)(int16_t)cpu->gte_dr[7];
        vy = (float)(int16_t)cpu->gte_dr[8];
        vz = (float)(int16_t)cpu->gte_dr[9];
    }

    register float f0 __asm__("fr0") = vx;
    register float f1 __asm__("fr1") = vy;
    register float f2 __asm__("fr2") = vz;
    register float f3 __asm__("fr3") = 1.0f;

    __asm__ __volatile__ ( "ftrv xmtrx, fv0\n" : "+f" (f0), "+f" (f1), "+f" (f2), "+f" (f3) :: "memory" );

    /* 平行移動 (TRX, TRY, TRZ) の加算 */
    f0 += (float)(int32_t)cpu->gte_cr[5];
    f1 += (float)(int32_t)cpu->gte_cr[6];
    f2 += (float)(int32_t)cpu->gte_cr[7];

    /* 結果を IR1, IR2, IR3 (DR7, DR8, DR9) へ (PS1 は 4096 = 1.0) */
    cpu->gte_dr[7] = (int16_t)f0;
    cpu->gte_dr[8] = (int16_t)f1;
    cpu->gte_dr[9] = (int16_t)f2;
}

/**
 * AVSZ3: 3頂点のZ値平均 (OTZ)
 */
void gte_avsz3(mips_t *cpu) {
    uint32_t z1 = cpu->gte_dr[17];
    uint32_t z2 = cpu->gte_dr[18];
    uint32_t z3 = cpu->gte_dr[19];
    uint32_t h = (uint16_t)cpu->gte_cr[18]; if (h == 0) h = 512;
    /* PS1 GTE Spec: (Z1+Z2+Z3) * ZSF3 (CR2) / 4096 */
    uint32_t sum = z1 + z2 + z3;
    uint32_t average = (sum * (uint16_t)cpu->gte_cr[2]) >> 12;
    cpu->gte_dr[7] = average; // OTZ result
}

/**
 * AVSZ4: 4頂点のZ値平均
 */
void gte_avsz4(mips_t *cpu) {
    uint32_t z0 = cpu->gte_dr[16];
    uint32_t z1 = cpu->gte_dr[17];
    uint32_t z2 = cpu->gte_dr[18];
    uint32_t z3 = cpu->gte_dr[19];
    uint32_t sum = z0 + z1 + z2 + z3;
    uint32_t average = (sum * (uint16_t)cpu->gte_cr[2]) >> 12;
    cpu->gte_dr[7] = average;
}

/**
 * SQR: 各要素の二乗
 */
void gte_sqr(mips_t *cpu) {
    for (int i=0; i<3; i++) {
        int32_t v = (int16_t)((cpu->gte_dr[i/2*2] >> (i%2*16)) & 0xFFFF);
        if (i == 2) v = (int16_t)(cpu->gte_dr[1] & 0xFFFF);
        cpu->gte_dr[9+i] = (v * v) >> 12;
    }
}

void gte_op(mips_t *cpu) {
    /* [Bloom Port] OP (Outer Product) */
    int16_t v0 = (int16_t)(cpu->gte_dr[0] & 0xFFFF);
    int16_t v1 = (int16_t)(cpu->gte_dr[0] >> 16);
    int16_t v2 = (int16_t)(cpu->gte_dr[1] & 0xFFFF);
    
    int16_t r11 = (int16_t)(cpu->gte_cr[0] & 0xFFFF);
    int16_t r12 = (int16_t)(cpu->gte_cr[0] >> 16);
    
    cpu->gte_dr[7] = (v0 * r12 - v1 * r11) >> 12;
    cpu->gte_dr[8] = (v1 * r11 - v2 * r12) >> 12;
    cpu->gte_dr[9] = (v2 * r12 - v0 * r11) >> 12;
}

void gte_execute(mips_t *cpu, u32 cmd) {
    u8 op = cmd & 0x3F;
    switch(op) {
        case 0x01: gte_rtps(cpu); break;
        case 0x06: gte_nclip(cpu); break;
        case 0x12: gte_mvmva(cpu, cmd); break;
        case 0x2D: gte_avsz3(cpu); break;
        case 0x2E: gte_avsz4(cpu); break;
        case 0x1E: gte_sqr(cpu); break;
        case 0x3F: gte_op(cpu); break;
        default: break;
    }
}
