#include "mips.h"
#include <string.h>

/*
 * MDEC (0x1F801820 command/data, 0x1F801824 status/control), DMA0 in, DMA1 out.
 *
 * Input words are buffered as they arrive; macroblocks are decoded lazily when output is
 * pulled (DMA1 or data-register reads), so the decode cost lands in the DMA1 transfers.
 * IDCT: float AAN (libjpeg jidctflt) for the standard scale table; the table uploaded with
 * command 3 is ignored, as in most emulators.
 *
 * 15-bit output is written in Dreamcast ARGB1555 channel order (R high, B low) because the
 * framebuffer is kept in that order (see gpu.c) and movie frames are uploaded straight into
 * the display area.  A game that reads the pixels back as a 15-bit texture would get R/B
 * swapped; MDEC_PS1_ORDER restores the hardware order.
 */
#define MDEC_PS1_ORDER 0

#define IN_HALFWORDS 65536       /* 128KB: a 320x240 STR frame needs ~17K halfwords */
static u16 in_buf[IN_HALFWORDS] __attribute__((aligned(32)));
/* Input is copied at DMA0 time: DMA0 completes instantly here, and games refill the same
 * RLE buffer with the next frame while DMA1 is still pulling strips of this one. */
static u32 in_got;              /* halfwords received for the current command   */
static u32 in_need;             /* halfwords the current command expects         */
static u32 in_pos;              /* decode position (halfwords)                   */
static int cmd;                 /* 0 idle, 1 decode, 2 quant, 3 scale            */
static u32 cmd_word;
static int depth, sgn, bit15;
static int en_in, en_out;
static int cur_block = 4;
static int decode_done;         /* decode command ran out of macroblocks        */

static u8 qt_y[64], qt_c[64];
static s16 scale_tab[64];

static u32 out_buf[192] __attribute__((aligned(32)));
static int out_len, out_pos;

u32 g_mdec_blocks = 0, g_mdec_cmds = 0, g_mdec_cmds_win = 0;

static const u8 zagzig[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

static float aan2d[64];          /* AAN row*col scale / 8, natural order */
static int crr[256], crg[256], cbg[256], cbb[256];
static u8 clamp8[1024];          /* index value+512 -> output byte (sign handled per command) */
static u16 c5r[1024], c5g[1024], c5b[1024];   /* same, as 15-bit channel fields */
static int tables_ready = 0;

static void build_tables(void) {
    static const float a[8] = { 1.0f, 1.387039845f, 1.306562965f, 1.175875602f,
                                1.0f, 0.785694958f, 0.541196100f, 0.275899379f };
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++) aan2d[r * 8 + c] = a[r] * a[c] / 8.0f;
    for (int i = 0; i < 256; i++) {
        float v = (float)(i - 128);
        float t;
        t = 1.402f * v;  crr[i] = (int)(t < 0 ? t - 0.5f : t + 0.5f);
        t = -0.7143f * v; crg[i] = (int)(t < 0 ? t - 0.5f : t + 0.5f);
        t = -0.3437f * v; cbg[i] = (int)(t < 0 ? t - 0.5f : t + 0.5f);
        t = 1.772f * v;  cbb[i] = (int)(t < 0 ? t - 0.5f : t + 0.5f);
    }
    tables_ready = 1;
}

static void set_clamp(int is_signed) {
    for (int i = 0; i < 1024; i++) {
        int v = i - 512;
        if (v < -128) v = -128;
        if (v > 127) v = 127;
        clamp8[i] = (u8)(is_signed ? (v & 0xFF) : (v + 128));
        u16 c = clamp8[i] >> 3;
#if MDEC_PS1_ORDER
        c5r[i] = c; c5g[i] = (u16)(c << 5); c5b[i] = (u16)(c << 10);
#else
        c5r[i] = (u16)(c << 10); c5g[i] = (u16)(c << 5); c5b[i] = c;
#endif
    }
}

/* ------------------------------------------------------------------ */
/* RLE + dequant + IDCT                                                */
/* ------------------------------------------------------------------ */

static inline int s10(u32 n) { return ((int)(n << 22)) >> 22; }

/* decode one 8x8 block into out[64] (values -128..127, row-major).
 * returns 1 on success, 0 if the input ran out (position restored). */
static float blk[64];            /* coefficients; kept zero between blocks (only set entries are cleared) */
#define CLEAR_SET() do { for (int i_ = 0; i_ < nset; i_++) blk[set[i_]] = 0; } while (0)
static int decode_block(const u8 *qt, int *out) {
    u8 set[64]; int nset = 0;
    u32 p = in_pos;
    u32 end = in_got;
    u32 n;
    do {
        if (p >= end) return 0;
        n = in_buf[p++];
    } while (n == 0xFE00);
    int qscale = (n >> 10) & 0x3F;
    int k = 0;
    int val = s10(n) * qt[0];
    u32 rows = 0, colac = 0;     /* rows with coefficients; columns with coefficients below row 0 */
    for (;;) {
        if (qscale == 0) val = s10(n) * 2;
        if (val < -0x400) val = -0x400; else if (val > 0x3FF) val = 0x3FF;
        int idx = qscale ? zagzig[k] : k;
        blk[idx] = (float)val * aan2d[idx];
        set[nset++ & 63] = (u8)idx;
        rows |= 1u << (idx >> 3);
        if (idx >= 8) colac |= 1u << (idx & 7);
        if (p >= end) { CLEAR_SET(); return 0; }
        n = in_buf[p++];
        k += ((n >> 10) & 0x3F) + 1;
        if (k > 63) break;
        val = (s10(n) * qt[k] * qscale + 4) / 8;
    }
    in_pos = p;

    if (rows <= 1 && !(blk[1] != 0 || blk[2] != 0 || blk[3] != 0 || blk[4] != 0 || blk[5] != 0 || blk[6] != 0 || blk[7] != 0)) {   /* flat block */
        float d = blk[0];
        int v = (int)(d + 1024.5f) - 1024;
        if (v < -128) v = -128; else if (v > 127) v = 127;
        for (int i = 0; i < 64; i++) out[i] = v;
        CLEAR_SET();
        return 1;
    }

    float ws[64];
    int nrows = 8;
    if (rows == 1) { memcpy(ws, blk, 8 * sizeof(float)); nrows = 1; goto rowpass; }   /* only row 0: columns are constant */
    /* columns */
    for (int c = 0; c < 8; c++) {
        const float *in = blk + c;
        float *w = ws + c;
        if (!(colac & (1u << c))) {
            float d = in[0];
            w[0] = w[8] = w[16] = w[24] = w[32] = w[40] = w[48] = w[56] = d;
            continue;
        }
        float tmp0 = in[0], tmp1 = in[16], tmp2 = in[32], tmp3 = in[48];
        float tmp10 = tmp0 + tmp2, tmp11 = tmp0 - tmp2;
        float tmp13 = tmp1 + tmp3, tmp12 = (tmp1 - tmp3) * 1.414213562f - tmp13;
        tmp0 = tmp10 + tmp13; tmp3 = tmp10 - tmp13; tmp1 = tmp11 + tmp12; tmp2 = tmp11 - tmp12;
        float tmp4 = in[8], tmp5 = in[24], tmp6 = in[40], tmp7 = in[56];
        float z13 = tmp6 + tmp5, z10 = tmp6 - tmp5, z11 = tmp4 + tmp7, z12 = tmp4 - tmp7;
        tmp7 = z11 + z13;
        tmp11 = (z11 - z13) * 1.414213562f;
        float z5 = (z10 + z12) * 1.847759065f;
        tmp10 = 1.082392200f * z12 - z5;
        tmp12 = -2.613125930f * z10 + z5;
        tmp6 = tmp12 - tmp7; tmp5 = tmp11 - tmp6; tmp4 = tmp10 + tmp5;
        w[0] = tmp0 + tmp7; w[56] = tmp0 - tmp7;
        w[8] = tmp1 + tmp6; w[48] = tmp1 - tmp6;
        w[16] = tmp2 + tmp5; w[40] = tmp2 - tmp5;
        w[32] = tmp3 + tmp4; w[24] = tmp3 - tmp4;
    }
rowpass:
    /* rows */
    for (int r = 0; r < nrows; r++) {
        const float *in = ws + r * 8;
        int *o = out + r * 8;
        float tmp0 = in[0], tmp1 = in[2], tmp2 = in[4], tmp3 = in[6];
        float tmp10 = tmp0 + tmp2, tmp11 = tmp0 - tmp2;
        float tmp13 = tmp1 + tmp3, tmp12 = (tmp1 - tmp3) * 1.414213562f - tmp13;
        tmp0 = tmp10 + tmp13; tmp3 = tmp10 - tmp13; tmp1 = tmp11 + tmp12; tmp2 = tmp11 - tmp12;
        float tmp4 = in[1], tmp5 = in[3], tmp6 = in[5], tmp7 = in[7];
        float z13 = tmp6 + tmp5, z10 = tmp6 - tmp5, z11 = tmp4 + tmp7, z12 = tmp4 - tmp7;
        tmp7 = z11 + z13;
        tmp11 = (z11 - z13) * 1.414213562f;
        float z5 = (z10 + z12) * 1.847759065f;
        tmp10 = 1.082392200f * z12 - z5;
        tmp12 = -2.613125930f * z10 + z5;
        tmp6 = tmp12 - tmp7; tmp5 = tmp11 - tmp6; tmp4 = tmp10 + tmp5;
        float f[8];
        f[0] = tmp0 + tmp7; f[7] = tmp0 - tmp7;
        f[1] = tmp1 + tmp6; f[6] = tmp1 - tmp6;
        f[2] = tmp2 + tmp5; f[5] = tmp2 - tmp5;
        f[4] = tmp3 + tmp4; f[3] = tmp3 - tmp4;
        for (int i = 0; i < 8; i++) {
            int v = (int)(f[i] + 1024.5f) - 1024;
            if (v < -128) v = -128; else if (v > 127) v = 127;
            o[i] = v;
        }
    }
    if (nrows == 1) for (int r = 1; r < 8; r++) memcpy(out + r * 8, out, 8 * sizeof(int));
    CLEAR_SET();
    return 1;
}

/* decode the next macroblock into out_buf; returns 1 if one was produced */
static int decode_macroblock(void) {
    if (cmd != 1 || decode_done) return 0;
    if (!tables_ready) build_tables();
    u32 save = in_pos;
    if (depth >= 2) {
        static int cr[64], cb[64], y[4][64];
        if (!decode_block(qt_c, cr) || !decode_block(qt_c, cb) ||
            !decode_block(qt_y, y[0]) || !decode_block(qt_y, y[1]) ||
            !decode_block(qt_y, y[2]) || !decode_block(qt_y, y[3])) {
            in_pos = save;
            if (in_got >= in_need) decode_done = 1;       /* only padding left */
            return 0;
        }
        g_mdec_blocks++;
        const u8 *cl = clamp8 + 512;
        /* one chroma sample covers 2x2 pixels: Y block (cy>>2, cx>>2), position (2cy&7, 2cx&7) */
        if (depth == 2) {                                    /* 24-bit: R,G,B bytes */
            u8 *dst = (u8 *)out_buf;
            for (int cy = 0; cy < 8; cy++) {
                u8 *d0 = dst + cy * 2 * 48, *d1 = d0 + 48;
                for (int cx = 0; cx < 8; cx++, d0 += 6, d1 += 6) {
                    int ci = cy * 8 + cx;
                    int R = crr[cr[ci] + 128], G = crg[cr[ci] + 128] + cbg[cb[ci] + 128], B = cbb[cb[ci] + 128];
                    const int *yy = y[(cy >> 2) * 2 + (cx >> 2)] + ((cy * 2) & 7) * 8 + ((cx * 2) & 7);
                    int Y;
                    Y = yy[0]; d0[0] = cl[Y + R]; d0[1] = cl[Y + G]; d0[2] = cl[Y + B];
                    Y = yy[1]; d0[3] = cl[Y + R]; d0[4] = cl[Y + G]; d0[5] = cl[Y + B];
                    Y = yy[8]; d1[0] = cl[Y + R]; d1[1] = cl[Y + G]; d1[2] = cl[Y + B];
                    Y = yy[9]; d1[3] = cl[Y + R]; d1[4] = cl[Y + G]; d1[5] = cl[Y + B];
                }
            }
            out_len = 192;
        } else {                                             /* 15-bit (channel order: c5r/c5g/c5b) */
            u32 *dst = out_buf;
            u32 stp = bit15 ? 0x80008000u : 0;
            const u16 *tr = c5r + 512, *tg = c5g + 512, *tb = c5b + 512;
            for (int cy = 0; cy < 8; cy++) {
                u32 *d0 = dst + cy * 16, *d1 = d0 + 8;       /* 8 words (16 pixels) per row */
                for (int cx = 0; cx < 8; cx++) {
                    int ci = cy * 8 + cx;
                    int R = crr[cr[ci] + 128], G = crg[cr[ci] + 128] + cbg[cb[ci] + 128], B = cbb[cb[ci] + 128];
                    const int *yy = y[(cy >> 2) * 2 + (cx >> 2)] + ((cy * 2) & 7) * 8 + ((cx * 2) & 7);
                    int a = yy[0], b = yy[1], c = yy[8], e = yy[9];
                    d0[cx] = stp | (u32)(tr[a + R] | tg[a + G] | tb[a + B]) | ((u32)(tr[b + R] | tg[b + G] | tb[b + B]) << 16);
                    d1[cx] = stp | (u32)(tr[c + R] | tg[c + G] | tb[c + B]) | ((u32)(tr[e + R] | tg[e + G] | tb[e + B]) << 16);
                }
            }
            out_len = 128;
        }
    } else {                                                 /* monochrome 8x8 */
        static int y1[64];
        if (!decode_block(qt_y, y1)) {
            in_pos = save;
            if (in_got >= in_need) decode_done = 1;
            return 0;
        }
        g_mdec_blocks++;
        const u8 *cl = clamp8 + 512;
        u8 *dst = (u8 *)out_buf;
        if (depth == 1) { for (int i = 0; i < 64; i++) dst[i] = cl[y1[i]]; out_len = 16; }
        else { for (int i = 0; i < 64; i += 2) *dst++ = (u8)((cl[y1[i]] >> 4) | (cl[y1[i + 1]] & 0xF0)); out_len = 8; }
    }
    out_pos = 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Command / data interface                                            */
/* ------------------------------------------------------------------ */

static void finish_params(void) {
    if (cmd == 2) {
        const u8 *b = (const u8 *)in_buf;
        memcpy(qt_y, b, 64);
        if (cmd_word & 1) memcpy(qt_c, b + 64, 64);
        cmd = 0;
    } else if (cmd == 3) {
        memcpy(scale_tab, in_buf, 128);
        cmd = 0;
    }
}

static void start_command(u32 w) {
    cmd_word = w;
    in_got = 0; in_pos = 0;
    out_len = out_pos = 0;
    decode_done = 0;
    g_mdec_cmds++;
    if ((w >> 29) == 1) g_mdec_cmds_win++;
    switch (w >> 29) {
    case 1:
        cmd = 1;
        depth = (w >> 27) & 3; sgn = (w >> 26) & 1; bit15 = (w >> 25) & 1;
        in_need = (w & 0xFFFF) * 2;
        set_clamp(sgn);
        if (in_need == 0) decode_done = 1;
        break;
    case 2: cmd = 2; in_need = (w & 1) ? 64 : 32; break;
    case 3: cmd = 3; in_need = 64; break;
    default: cmd = 0; in_need = 0; break;
    }
}

/* room for w more words in the input buffer? */
static int to_local(u32 w) {
    if (in_got + w * 2 > IN_HALFWORDS) {
        static int olog = 0;
        if (olog++ < 4) dlog("MDEC: input overflow (%lu halfwords)", (unsigned long)(in_got + w * 2));
        return 0;
    }
    return 1;
}

static inline void take_word(u32 w) {
    if (cmd == 0 || in_got >= in_need) {
        if (cmd == 1 && !decode_done) {
            /* a new command while the old one still has macroblocks: drop the rest */
        }
        start_command(w);
        return;
    }
    if (!to_local(1)) return;
    in_buf[in_got] = (u16)w;
    in_buf[in_got + 1] = (u16)(w >> 16);
    in_got += 2;
    if (in_got >= in_need) finish_params();
}

void mdec_write_cmd(u32 w) { take_word(w); }

/* DMA0: n words from RAM */
void mdec_dma_in(const u32 *src, u32 n) {
    while (n > 0) {
        if (cmd == 0 || in_got >= in_need) { take_word(*src++); n--; continue; }
        u32 room = (in_need - in_got) / 2;
        u32 c = n < room ? n : room;
        if (to_local(c)) {
            memcpy(&in_buf[in_got], src, c * 4);
        } else {
            in_need = in_got;                                         /* give up on the rest */
            return;
        }
        in_got += c * 2;
        src += c; n -= c;
        if (in_got >= in_need) finish_params();
    }
}

static inline int ensure_output(void) {
    if (out_pos < out_len) return 1;
    return decode_macroblock();
}

u32 mdec_read_data(void) {
    if (!ensure_output()) return 0;
    return out_buf[out_pos++];
}

/* DMA1: copy up to n words; returns the number written */
u32 mdec_dma_out(u32 *dst, u32 n) {
    u32 done = 0;
    while (done < n) {
        if (!ensure_output()) break;
        u32 c = (u32)(out_len - out_pos);
        if (c > n - done) c = n - done;
        memcpy(dst + done, out_buf + out_pos, c * 4);
        out_pos += c; done += c;
    }
    return done;
}

void mdec_write_ctrl(u32 v) {
    if (v & 0x80000000u) {
        cmd = 0; in_got = in_need = in_pos = 0; out_len = out_pos = 0; decode_done = 0;
        cur_block = 4;
    }
    en_in = (v >> 30) & 1;
    en_out = (v >> 29) & 1;
}

u32 mdec_read_status(void) {
    int busy_in = (cmd != 0) && in_got < in_need;
    int have_out = (out_pos < out_len) || (cmd == 1 && ensure_output());
    u32 s = 0;
    if (!have_out) s |= 1u << 31;
    /* busy only while parameters are still arriving: a decode whose input yields more
     * macroblocks than the game pulls (corrupt or partial frame) must not hang the next command */
    if (busy_in) s |= 1u << 29;
    if (en_in && (cmd == 0 || busy_in)) s |= 1u << 28;
    if (en_out && have_out) s |= 1u << 27;
    s |= ((cmd_word >> 25) & 0xF) << 23;
    s |= (u32)cur_block << 16;
    if (busy_in) s |= ((in_need - in_got) / 2 - 1) & 0xFFFF;
    else s |= 0xFFFF;
    return s;
}

void mdec_init(void) {
    mdec_write_ctrl(0x80000000u);
    en_in = en_out = 0;
    if (!tables_ready) build_tables();
    set_clamp(0);
}
