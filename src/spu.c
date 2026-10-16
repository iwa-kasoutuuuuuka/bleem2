#include <kos.h>
#include <dc/sound/stream.h>
#include <malloc.h>
#include <string.h>
#include "mips.h"

/*
 * PS1 SPU: 24 ADPCM voices with pitch, ADSR envelope and loop flags,
 * mixed in software to a 44.1kHz stereo KOS sound stream.
 * Not emulated: reverb, noise, pitch modulation, SPU IRQ, CD/XA audio.
 */

#define SPU_BUF_SIZE   65536
#define SPU_RAM_SIZE   (512 * 1024)
#define SPU_REG_BASE   0x1F801C00

static int16_t *spu_pcm_buffer = NULL;
static uint8_t *spu_ram = NULL;
static snd_stream_hnd_t spu_hnd = SND_STREAM_INVALID;
static int spu_started = 0;

enum { ENV_OFF = 0, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

typedef struct {
    int32_t s1, s2;             /* ADPCM filter history */
    uint32_t cur_addr;          /* address of the current block */
    uint32_t repeat_addr;
    int16_t block[28];
    int block_valid;
    uint32_t pos;               /* 16.16 position inside the block */
    int playing;
    int env_phase;
    int32_t env_level;          /* 0..0x7FFF */
    int env_wait;               /* samples to wait between env steps */
} spu_voice_t;

static struct {
    uint16_t regs[512];
    uint32_t ram_addr;          /* transfer address */
    uint32_t endx;
    spu_voice_t v[24];
    uint32_t active_mask;
} spu;

/* ------------------------------------------------------------------ */
/* ADPCM                                                               */
/* ------------------------------------------------------------------ */

static const int f_pos[5] = { 0, 60, 115, 98, 122 };
static const int f_neg[5] = { 0, 0, -52, -55, -60 };

/* decode the 16-byte block at v->cur_addr; returns the flags byte */
static int decode_block(spu_voice_t *v) {
    if (v->cur_addr > SPU_RAM_SIZE - 16) { memset(v->block, 0, sizeof(v->block)); return 1; }
    const uint8_t *src = spu_ram + v->cur_addr;
    int shift = src[0] & 0x0F;
    int filter = (src[0] >> 4) & 0x07;
    if (shift > 12) shift = 9;
    if (filter > 4) filter = 4;
    int flags = src[1];
    for (int i = 0; i < 28; i++) {
        int nib = (i & 1) ? (src[2 + (i >> 1)] >> 4) : (src[2 + (i >> 1)] & 0x0F);
        int32_t s = (int32_t)(int16_t)(nib << 12) >> shift;
        s += (v->s1 * f_pos[filter] + v->s2 * f_neg[filter] + 32) >> 6;
        if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
        v->block[i] = (int16_t)s;
        v->s2 = v->s1;
        v->s1 = s;
    }
    v->block_valid = 1;
    return flags;
}

static void voice_advance_block(int vi) {
    spu_voice_t *v = &spu.v[vi];
    int flags = decode_block(v);
    if (flags & 0x4) v->repeat_addr = v->cur_addr;          /* loop start */
    if (flags & 0x1) {                                      /* end */
        spu.endx |= (1u << vi);
        if (flags & 0x2) {
            v->cur_addr = v->repeat_addr;                   /* loop */
        } else {
            v->playing = 0;                                 /* stop (envelope -> release) */
            v->env_phase = ENV_RELEASE;
            v->env_level = 0;
            v->cur_addr = v->repeat_addr;
        }
    } else {
        v->cur_addr += 16;
    }
}

/* ------------------------------------------------------------------ */
/* ADSR (approximation of the hardware step/shift scheme)             */
/* ------------------------------------------------------------------ */

#define ENV_STEP 8   /* envelope updated every ENV_STEP samples */
static void env_apply(spu_voice_t *v, int rate, int exp_mode, int decrease) {
    if (rate >= 0x7F) return;                               /* infinitely slow */
    int shift = (rate >> 2) - 11;
    int32_t step = decrease ? -(8 - (rate & 3)) : (7 - (rate & 3));
    if (shift > 0) {
        if (v->env_wait > 0) { v->env_wait -= ENV_STEP; return; }
        v->env_wait = (1 << shift) - 1;
    } else if (shift < 0) {
        step <<= -shift;
        step *= ENV_STEP;
    } else {
        step *= ENV_STEP;
    }
    if (exp_mode) {
        if (!decrease && v->env_level > 0x6000) step >>= 2;
        if (decrease) step = (step * v->env_level) >> 15;
        if (decrease && step == 0) step = -1;
    }
    v->env_level += step;
    if (v->env_level < 0) v->env_level = 0;
    if (v->env_level > 0x7FFF) v->env_level = 0x7FFF;
}

static void env_tick(int vi) {
    spu_voice_t *v = &spu.v[vi];
    uint16_t a1 = spu.regs[vi * 8 + 4];     /* +8  */
    uint16_t a2 = spu.regs[vi * 8 + 5];     /* +A  */
    switch (v->env_phase) {
    case ENV_ATTACK:
        env_apply(v, (a1 >> 8) & 0x7F, (a1 >> 15) & 1, 0);
        if (v->env_level >= 0x7FFF) v->env_phase = ENV_DECAY;
        break;
    case ENV_DECAY: {
        int32_t sustain = ((a1 & 0xF) + 1) * 0x800;
        env_apply(v, ((a1 >> 4) & 0xF) << 2, 1, 1);
        if (v->env_level <= sustain) { v->env_level = sustain; v->env_phase = ENV_SUSTAIN; }
        break;
    }
    case ENV_SUSTAIN:
        env_apply(v, (a2 >> 6) & 0x7F, (a2 >> 15) & 1, (a2 >> 14) & 1);
        break;
    case ENV_RELEASE:
        env_apply(v, (a2 & 0x1F) << 2, (a2 >> 5) & 1, 1);
        if (v->env_level <= 0) { v->env_level = 0; v->env_phase = ENV_OFF; v->playing = 0; }
        break;
    default:
        break;
    }
    spu.regs[vi * 8 + 6] = (uint16_t)v->env_level;          /* current ADSR volume */
}

/* ------------------------------------------------------------------ */
/* Mixer                                                               */
/* ------------------------------------------------------------------ */

static inline int32_t vol15(uint16_t r) {
    if (r & 0x8000) return 0x3FFF;                          /* sweep mode: treat as max */
    return (int16_t)(r << 1) >> 1;                          /* 15-bit signed */
}

uint64_t prof_spu_us = 0;
#if !SPU_AICA
static void *spu_callback_inner(snd_stream_hnd_t hnd, int len, int *out_len);
#endif
#if !SPU_AICA
static void *spu_callback(snd_stream_hnd_t hnd, int len, int *out_len) {
    uint64_t t0 = timer_us_gettime64();
    void *r = spu_callback_inner(hnd, len, out_len);
    prof_spu_us += timer_us_gettime64() - t0;
    return r;
}
#endif
#if !SPU_AICA
static void *spu_callback_inner(snd_stream_hnd_t hnd, int len, int *out_len) {
    (void)hnd;
    if (!spu_pcm_buffer || len > SPU_BUF_SIZE) { *out_len = 0; return spu_pcm_buffer; }
    int frames = len / 4;
    int32_t mainl = vol15(spu.regs[(0x1D80 - 0x1C00) / 2]);
    int32_t mainr = vol15(spu.regs[(0x1D82 - 0x1C00) / 2]);
    int spu_on = (spu.regs[(0x1DAA - 0x1C00) / 2] & 0x8000) ? 1 : 0;
    int mute = (spu.regs[(0x1DAA - 0x1C00) / 2] & 0x4000) ? 0 : 1;

    static int32_t mix[SPU_BUF_SIZE / 2];
    memset(mix, 0, frames * 8);
    uint32_t active = 0;

    for (int vi = 0; vi < 24; vi++) {
        spu_voice_t *v = &spu.v[vi];
        if (v->env_phase == ENV_OFF) continue;
        active |= (1u << vi);
        int32_t vl = vol15(spu.regs[vi * 8 + 0]);
        int32_t vr = vol15(spu.regs[vi * 8 + 1]);
        uint32_t pitch = spu.regs[vi * 8 + 2];
        if (pitch > 0x3FFF) pitch = 0x3FFF;
        uint32_t step = pitch << 4;                          /* 16.16: 0x1000 -> 1.0 */

        int32_t voll = 0, volr = 0;
        for (int i = 0; i < frames; i++) {
            if (!v->block_valid) voice_advance_block(vi);
            int idx = v->pos >> 16;
            int32_t s = v->block[idx];
            if ((i & (ENV_STEP - 1)) == 0) {
                env_tick(vi);
                if (v->env_phase == ENV_OFF) break;
                voll = (v->env_level * vl) >> 15;              /* envelope folded into the volume */
                volr = (v->env_level * vr) >> 15;
            }
            mix[i * 2]     += (s * voll) >> 14;
            mix[i * 2 + 1] += (s * volr) >> 14;

            v->pos += step;
            while (v->pos >= (28u << 16)) {
                v->pos -= (28u << 16);
                if (v->playing) voice_advance_block(vi);
                else { v->block_valid = 0; memset(v->block, 0, sizeof(v->block)); v->block_valid = 1; }
            }
        }
    }
    spu.active_mask = active;

    if (!spu_on || mute) {
        memset(spu_pcm_buffer, 0, frames * 4);
    } else {
        int scale = (mainl != 0x3FFF || mainr != 0x3FFF);
        for (int i = 0; i < frames * 2; i += 2) {
            int32_t l = mix[i], r = mix[i + 1];
            if (scale) { l = (l * mainl) >> 14; r = (r * mainr) >> 14; }
            if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
            if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
            spu_pcm_buffer[i] = (int16_t)l;
            spu_pcm_buffer[i + 1] = (int16_t)r;
        }
    }
    *out_len = frames * 4;
    return spu_pcm_buffer;
}
#endif

#if SPU_AICA
/* ------------------------------------------------------------------ */
/* AICA back end: one hardware channel per PS1 voice.                  */
/* The SH4 only decodes ADPCM blocks into a per-voice ring buffer in   */
/* sound RAM; pitch, volume/pan, the ADSR envelope and mixing are done */
/* by the AICA.  Channels 0-23 are programmed directly through their   */
/* registers (the KOS ARM driver is only used for memory and position  */
/* read-back); channels 24/25 carry the software reverb at 22050 Hz.   */
/* ------------------------------------------------------------------ */
#include <dc/sound/sound.h>
#include <dc/sound/aica_comm.h>
#include <dc/spu.h>
#include <dc/g2bus.h>

#define RING_SAMPLES   8192                         /* 16-bit samples per voice ring (16KB) */
#define RING_MASK      (RING_SAMPLES - 1)
#define AHEAD_MIN      2048
#define STAGE_BLOCKS   96                           /* blocks decoded per voice per poll at most */
#define RVB_CH_L       24
#define RVB_CH_R       25
#define RVB_RING       4096                         /* reverb output ring per side (22050 Hz) */
#define AICA_REG(ch, off)  (0xA0700000u + 0x80u * (u32)(ch) + (u32)(off))

typedef struct {
    uint32_t ring;          /* sound RAM address of the ring */
    uint32_t wpos;          /* next write position (samples) */
    int      started;       /* key-on issued */
    int      tail_us;       /* keep feeding this long after the shadow envelope ended (hardware release) */
    uint32_t last_freq, last_tl, last_pan;
    int      rr_ms;         /* programmed release time */
    int16_t *local;         /* SH4 copy of the ring (reverb mixing) */
    uint32_t rpos;          /* reverb read position in the ring, 16.16 */
    uint32_t est_pos;       /* estimated hardware play position (samples) */
    int      sync_ctr;      /* read the real position every few polls */
} aica_voice_t;
static aica_voice_t av[24];
static int aica_ready = 0;
static int16_t stage[STAGE_BLOCKS * 28] __attribute__((aligned(32)));
static uint64_t aica_last_us = 0;
u32 g_aica_underruns = 0, g_aica_playing = 0, g_aica_starts = 0, g_aica_rvb = 0;

/* linear 0..255 -> AICA TL (attenuation, 0.375 dB per step), from the KOS driver */
static const uint8_t aica_logs[256] = {
    255, 127, 111, 102, 95, 90, 86, 82, 79, 77, 74, 72, 70, 68, 66, 65,
    63, 62, 61, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 50, 49, 48,
    47, 47, 46, 45, 45, 44, 43, 43, 42, 42, 41, 41, 40, 40, 39, 39,
    38, 38, 37, 37, 36, 36, 35, 35, 34, 34, 34, 33, 33, 33, 32, 32,
    31, 31, 31, 30, 30, 30, 29, 29, 29, 28, 28, 28, 27, 27, 27, 27,
    26, 26, 26, 25, 25, 25, 25, 24, 24, 24, 24, 23, 23, 23, 23, 22,
    22, 22, 22, 21, 21, 21, 21, 20, 20, 20, 20, 20, 19, 19, 19, 19,
    18, 18, 18, 18, 18, 17, 17, 17, 17, 17, 17, 16, 16, 16, 16, 16,
    15, 15, 15, 15, 15, 15, 14, 14, 14, 14, 14, 14, 13, 13, 13, 13,
    13, 13, 12, 12, 12, 12, 12, 12, 11, 11, 11, 11, 11, 11, 11, 10,
    10, 10, 10, 10, 10, 10, 9, 9, 9, 9, 9, 9, 9, 8, 8, 8,
    8, 8, 8, 8, 8, 7, 7, 7, 7, 7, 7, 7, 7, 6, 6, 6,
    6, 6, 6, 6, 6, 5, 5, 5, 5, 5, 5, 5, 5, 5, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};
/* AICA envelope times (ms) for effective rate 0..63 (register rate * 2, key scaling off) */
static const float aica_ar_ms[64] = {
    1e9f, 1e9f, 8100, 6900, 6000, 4800, 4000, 3400, 3000, 2400, 2000, 1700, 1500, 1200, 1000, 860,
    760, 600, 500, 430, 380, 300, 250, 220, 190, 150, 130, 110, 95, 76, 63, 55,
    47, 38, 31, 27, 24, 19, 15, 13, 12, 9.4f, 7.9f, 6.8f, 6.0f, 4.7f, 3.8f, 3.4f,
    3.0f, 2.4f, 2.0f, 1.8f, 1.6f, 1.3f, 1.1f, 0.93f, 0.85f, 0.65f, 0.53f, 0.44f, 0.40f, 0.35f, 0.0f, 0.0f
};
static const float aica_dr_ms[64] = {
    1e9f, 1e9f, 118200, 101300, 88600, 70900, 59100, 50700, 44300, 35500, 29600, 25300, 22200, 17700, 14800, 12700,
    11100, 8900, 7400, 6300, 5500, 4400, 3700, 3200, 2800, 2200, 1800, 1600, 1400, 1100, 920, 790,
    690, 550, 460, 390, 340, 270, 230, 200, 170, 140, 110, 98, 85, 68, 57, 49,
    43, 34, 28, 25, 22, 18, 14, 12, 11, 8.5f, 7.1f, 6.1f, 5.4f, 4.3f, 3.6f, 3.1f
};

/* PS1 envelope rate (shift<<2 | step) -> milliseconds to traverse the full scale */
static float ps1_rate_ms(int rate, int decrease) {
    if (rate >= 0x7F) return 1e9f;
    int shift = (rate >> 2) - 11;
    int step = decrease ? (8 - (rate & 3)) : (7 - (rate & 3));
    float cyc_per_step = (shift > 0) ? (float)(1 << shift) : 1.0f;
    float inc = (shift < 0) ? (float)(step << (-shift)) : (float)step;
    return (32768.0f / inc) * cyc_per_step / 44.1f;
}
static int aica_rate_for(const float *tab, float ms) {
    int best = 31; float bd = 1e30f;
    for (int r = 0; r < 32; r++) { float d = tab[r * 2] - ms; if (d < 0) d = -d; if (d < bd) { bd = d; best = r; } }
    return best;
}

static inline void chw32(int ch, int off, u32 v) { g2_write_32(AICA_REG(ch, off), v); }
static inline void chw8(int ch, int off, u8 v)   { g2_write_8(AICA_REG(ch, off), v); }
static inline u32  chr32(int ch, int off)        { return g2_read_32(AICA_REG(ch, off)); }

static u32 aica_freq_reg(u32 freq) {
    u32 freq_base = 5644800; int hi = 7;
    while (freq < freq_base && hi > -8) { freq_base >>= 1; hi--; }
    u32 lo = (freq << 10) / freq_base;
    return (((u32)hi & 0xF) << 11) | (lo & 1023);
}
static u8 aica_pan_reg(int pan) {                    /* 0 = left, 128 = centre, 255 = right */
    if (pan == 0x80) return 0;
    if (pan < 0x80) return (u8)(0x10 | ((0x7F - pan) >> 3));
    return (u8)((pan - 0x80) >> 3);
}

/* program a channel to loop over its ring and key it on with the given envelope */
static void aica_chan_start(int ch, u32 base, u32 len, u32 freq, u8 tl, u8 pan, u32 aeg_lo, u32 aeg_hi) {
    chw32(ch, 0x00, (chr32(ch, 0) & ~0x4000u) | 0x8000u);     /* key off first */
    chw32(ch, 0x04, base & 0xFFFF);
    chw32(ch, 0x08, 0);
    chw32(ch, 0x0C, len & 0xFFFF);
    chw32(ch, 0x10, aeg_lo);                                  /* D2R D1R AR */
    chw32(ch, 0x14, aeg_hi);                                  /* KRS DL RR */
    chw32(ch, 0x18, aica_freq_reg(freq));
    chw32(ch, 0x1C, 0);                                       /* no LFO */
    chw32(ch, 0x20, 0);                                       /* no DSP send */
    chw8 (ch, 0x24, aica_pan_reg(pan));
    chw8 (ch, 0x25, 0xF);                                     /* direct send level: full */
    chw8 (ch, 0x28, 0x24);                                    /* LPF off */
    chw8 (ch, 0x29, tl);
    chw32(ch, 0x00, 0xC000u | 0x0200u | ((base >> 16) & 0x7F)); /* 16-bit, loop, key on */
}
static void aica_chan_keyoff(int ch) { chw32(ch, 0x00, (chr32(ch, 0) & ~0x4000u) | 0x8000u); }

/* write n samples at ring position wpos (wrapping), keeping the SH4 copy in sync */
static void ring_write(aica_voice_t *a, const int16_t *src, int n) {
    while (n > 0) {
        int chunk = RING_SAMPLES - (int)a->wpos;
        if (chunk > n) chunk = n;
        spu_memload(a->ring + a->wpos * 2, (void *)src, (size_t)chunk * 2);
        if (a->local) memcpy(a->local + a->wpos, src, (size_t)chunk * 2);
        a->wpos = (a->wpos + chunk) & RING_MASK;
        src += chunk; n -= chunk;
    }
}

static int decode_blocks(int vi, int max_blocks) {
    spu_voice_t *v = &spu.v[vi];
    int n = 0;
    for (int b = 0; b < max_blocks; b++) {
        if (v->playing) { voice_advance_block(vi); memcpy(stage + n, v->block, 28 * 2); }
        else memset(stage + n, 0, 28 * 2);
        n += 28;
    }
    return n;
}

/* ---- software reverb (PS1 algorithm at 22050 Hz) on channels 24/25 ---- */
static struct {
    uint32_t ring_l, ring_r, wpos;
    int started;
    uint32_t buf_addr;          /* current reverb buffer address inside SPU RAM */
    int16_t out_l[2048], out_r[2048];
} rvb;
static inline int16_t rvb_rd(uint32_t a) { return (int16_t)(spu_ram[a & 0x7FFFF] | (spu_ram[(a + 1) & 0x7FFFF] << 8)); }
static inline void rvb_wr(uint32_t a, int32_t v) {
    if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
    spu_ram[a & 0x7FFFF] = (uint8_t)v; spu_ram[(a + 1) & 0x7FFFF] = (uint8_t)((u32)v >> 8);
}
static inline int32_t rvb_mul(int32_t a, int32_t b) { return (a * b) >> 15; }
static inline int32_t rvb_clamp(int32_t v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : v); }
#define RVB_REG(i)  ((int16_t)spu.regs[(0x1DC0 - 0x1C00) / 2 + (i)])
#define RVB_ADDR(i) ((uint32_t)(uint16_t)spu.regs[(0x1DC0 - 0x1C00) / 2 + (i)] * 8u)

static void reverb_step(int32_t lin, int32_t rin, int16_t *outl, int16_t *outr) {
    uint32_t base = (uint32_t)(uint16_t)spu.regs[(0x1DA2 - 0x1C00) / 2] * 8u;
    uint32_t dAPF1 = RVB_ADDR(0), dAPF2 = RVB_ADDR(1);
    int32_t vIIR = RVB_REG(2), vCOMB1 = RVB_REG(3), vCOMB2 = RVB_REG(4), vCOMB3 = RVB_REG(5), vCOMB4 = RVB_REG(6);
    int32_t vWALL = RVB_REG(7), vAPF1 = RVB_REG(8), vAPF2 = RVB_REG(9);
    uint32_t mLSAME = RVB_ADDR(10), mRSAME = RVB_ADDR(11), mLCOMB1 = RVB_ADDR(12), mRCOMB1 = RVB_ADDR(13);
    uint32_t mLCOMB2 = RVB_ADDR(14), mRCOMB2 = RVB_ADDR(15), dLSAME = RVB_ADDR(16), dRSAME = RVB_ADDR(17);
    uint32_t mLDIFF = RVB_ADDR(18), mRDIFF = RVB_ADDR(19), mLCOMB3 = RVB_ADDR(20), mRCOMB3 = RVB_ADDR(21);
    uint32_t mLCOMB4 = RVB_ADDR(22), mRCOMB4 = RVB_ADDR(23), dLDIFF = RVB_ADDR(24), dRDIFF = RVB_ADDR(25);
    uint32_t mLAPF1 = RVB_ADDR(26), mRAPF1 = RVB_ADDR(27), mLAPF2 = RVB_ADDR(28), mRAPF2 = RVB_ADDR(29);
    int32_t vLIN = RVB_REG(30), vRIN = RVB_REG(31);
    int32_t vLOUT = (int16_t)spu.regs[(0x1D84 - 0x1C00) / 2], vROUT = (int16_t)spu.regs[(0x1D86 - 0x1C00) / 2];
    uint32_t cur = rvb.buf_addr;
    if (cur < base || cur >= 0x80000u) cur = base;
    #define RA(off) ((cur + (off) >= 0x80000u) ? (base + ((cur + (off)) - 0x80000u)) : (cur + (off)))   /* wrap to mBASE */
    #define RD(off) ((int32_t)rvb_rd(RA(off)))
    lin = rvb_mul(vLIN, lin); rin = rvb_mul(vRIN, rin);
    int32_t ls = rvb_mul(lin + rvb_mul(RD(dLSAME), vWALL) - RD(mLSAME - 2), vIIR) + RD(mLSAME - 2);
    int32_t rs = rvb_mul(rin + rvb_mul(RD(dRSAME), vWALL) - RD(mRSAME - 2), vIIR) + RD(mRSAME - 2);
    int32_t ld = rvb_mul(lin + rvb_mul(RD(dRDIFF), vWALL) - RD(mLDIFF - 2), vIIR) + RD(mLDIFF - 2);
    int32_t rd = rvb_mul(rin + rvb_mul(RD(dLDIFF), vWALL) - RD(mRDIFF - 2), vIIR) + RD(mRDIFF - 2);
    rvb_wr(RA(mLSAME), ls); rvb_wr(RA(mRSAME), rs); rvb_wr(RA(mLDIFF), ld); rvb_wr(RA(mRDIFF), rd);
    int32_t lo = rvb_mul(vCOMB1, RD(mLCOMB1)) + rvb_mul(vCOMB2, RD(mLCOMB2)) + rvb_mul(vCOMB3, RD(mLCOMB3)) + rvb_mul(vCOMB4, RD(mLCOMB4));
    int32_t ro = rvb_mul(vCOMB1, RD(mRCOMB1)) + rvb_mul(vCOMB2, RD(mRCOMB2)) + rvb_mul(vCOMB3, RD(mRCOMB3)) + rvb_mul(vCOMB4, RD(mRCOMB4));
    lo = rvb_clamp(lo - rvb_mul(vAPF1, RD(mLAPF1 - dAPF1))); rvb_wr(RA(mLAPF1), lo); lo = rvb_mul(lo, vAPF1) + RD(mLAPF1 - dAPF1);
    ro = rvb_clamp(ro - rvb_mul(vAPF1, RD(mRAPF1 - dAPF1))); rvb_wr(RA(mRAPF1), ro); ro = rvb_mul(ro, vAPF1) + RD(mRAPF1 - dAPF1);
    lo = rvb_clamp(lo - rvb_mul(vAPF2, RD(mLAPF2 - dAPF2))); rvb_wr(RA(mLAPF2), lo); lo = rvb_mul(lo, vAPF2) + RD(mLAPF2 - dAPF2);
    ro = rvb_clamp(ro - rvb_mul(vAPF2, RD(mRAPF2 - dAPF2))); rvb_wr(RA(mRAPF2), ro); ro = rvb_mul(ro, vAPF2) + RD(mRAPF2 - dAPF2);
    *outl = (int16_t)rvb_clamp(rvb_mul(lo, vLOUT));
    *outr = (int16_t)rvb_clamp(rvb_mul(ro, vROUT));
    cur += 2; if (cur >= 0x80000u) cur = base;
    if (cur < base) cur = base;
    rvb.buf_addr = cur;
    #undef RA
    #undef RD
}

static void aica_voice_init(void) {
    snd_init();
    for (int i = 0; i < 24; i++) {
        av[i].ring = snd_mem_malloc(RING_SAMPLES * 2);
        av[i].local = (int16_t *)memalign(32, RING_SAMPLES * 2);
        if (av[i].local) memset(av[i].local, 0, RING_SAMPLES * 2);
        av[i].wpos = 0; av[i].started = 0; av[i].tail_us = 0;
        av[i].last_freq = av[i].last_tl = av[i].last_pan = 0xFFFFFFFF;
    }
    rvb.ring_l = snd_mem_malloc(RVB_RING * 2);
    rvb.ring_r = snd_mem_malloc(RVB_RING * 2);
    rvb.wpos = 0; rvb.started = 0; rvb.buf_addr = 0;
    aica_ready = (av[23].ring != 0 && rvb.ring_r != 0);
    aica_last_us = timer_us_gettime64();
    dlog("SPU: AICA back end %s (ring %08lX, reverb %08lX)", aica_ready ? "ready" : "FAILED", (unsigned long)av[0].ring, (unsigned long)rvb.ring_l);
}

static void aica_key_on(int vi) {
    aica_voice_t *a = &av[vi];
    if (a->started) { aica_chan_keyoff(vi); a->started = 0; }
    a->wpos = 0; a->tail_us = 0; a->rpos = 0;
    a->last_freq = a->last_tl = a->last_pan = 0xFFFFFFFF;
}
static void aica_key_off(int vi) {
    aica_voice_t *a = &av[vi];
    if (a->started) aica_chan_keyoff(vi);                    /* hardware release (RR) */
}

static inline uint32_t voice_rate(int vi) {                  /* output samples per second */
    uint32_t pitch = spu.regs[vi * 8 + 2];
    if (pitch > 0x3FFF) pitch = 0x3FFF;
    if (pitch < 0x10) pitch = 0x10;
    return 44100u * pitch / 4096u;
}

/* per poll: keep every active voice's ring filled ahead of the play position,
 * advance the shadow envelope by the elapsed time and push volume/pitch changes */
static void aica_poll(void) {
    if (!aica_ready) return;
    uint64_t now = timer_us_gettime64();
    uint32_t dt_us = (uint32_t)(now - aica_last_us);
    if (dt_us > 100000) dt_us = 100000;
    if (dt_us < 500) return;
    aica_last_us = now;

    int32_t mainl = vol15(spu.regs[(0x1D80 - 0x1C00) / 2]);
    int32_t mainr = vol15(spu.regs[(0x1D82 - 0x1C00) / 2]);
    int spu_on = (spu.regs[(0x1DAA - 0x1C00) / 2] & 0x8000) ? 1 : 0;
    int mute = (spu.regs[(0x1DAA - 0x1C00) / 2] & 0x4000) ? 0 : 1;
    int rvb_on = (spu.regs[(0x1DAA - 0x1C00) / 2] & 0x80) ? 1 : 0;
    uint32_t rvb_mask = (uint32_t)spu.regs[(0x1D98 - 0x1C00) / 2] | ((uint32_t)spu.regs[(0x1D9A - 0x1C00) / 2] << 16);
    int32_t mainv = (mainl > mainr ? mainl : mainr); if (mainv < 0) mainv = -mainv;
    uint32_t active = 0;
    int rvb_n = (int)((uint64_t)dt_us * 22050u / 1000000u);
    if (rvb_n > 2048) rvb_n = 2048;
    static int32_t rvb_in_l[2048], rvb_in_r[2048];
    if (rvb_on) { memset(rvb_in_l, 0, rvb_n * 4); memset(rvb_in_r, 0, rvb_n * 4); }

    for (int vi = 0; vi < 24; vi++) {
        spu_voice_t *v = &spu.v[vi];
        aica_voice_t *a = &av[vi];
        if (v->env_phase == ENV_OFF) {
            if (a->started) {
                if (a->tail_us > 0) { a->tail_us -= (int)dt_us; }
                else { aica_chan_keyoff(vi); a->started = 0; continue; }
            } else continue;
        }
        active |= (1u << vi);
        uint32_t rate = voice_rate(vi);
        uint32_t consumed = (uint32_t)((uint64_t)rate * dt_us / 1000000u) + 1;

        /* shadow envelope (only to know when the voice is really over) */
        if (v->env_phase != ENV_OFF) {
            for (uint32_t s = 0; s < consumed; s += ENV_STEP) {
                env_tick(vi);
                if (v->env_phase == ENV_OFF) { a->tail_us = a->rr_ms * 1000 + 50000; break; }
            }
        }

        /* keep the ring filled: 4 polls of audio ahead, at least AHEAD_MIN */
        uint32_t target = consumed * 4; if (target < AHEAD_MIN) target = AHEAD_MIN; if (target > RING_SAMPLES - 512) target = RING_SAMPLES - 512;
        uint32_t pos = 0;
        if (a->started) {
            if (--a->sync_ctr <= 0) { pos = snd_get_pos((unsigned int)vi); a->sync_ctr = 4; }
            else pos = (a->est_pos + consumed) & RING_MASK;
            a->est_pos = pos;
        }
        uint32_t ahead = a->started ? ((a->wpos - pos) & RING_MASK) : a->wpos;
        if (a->started && ahead < consumed) g_aica_underruns++;
        if (ahead < target) {
            int need = (int)(target - ahead);
            int blocks = (need + 27) / 28; if (blocks > STAGE_BLOCKS) blocks = STAGE_BLOCKS;
            ring_write(a, stage, decode_blocks(vi, blocks));
        }

        /* volume / pan: voice volumes x main volume (the envelope is in hardware) */
        int32_t vl = vol15(spu.regs[vi * 8 + 0]), vr = vol15(spu.regs[vi * 8 + 1]);
        if (vl < 0) vl = -vl;
        if (vr < 0) vr = -vr;
        int32_t vmax = vl > vr ? vl : vr;
        uint32_t tl = 255, pan = 128;
        if (spu_on && !mute && vmax > 0) {
            int32_t lvl = (vmax * mainv) >> 14;                      /* 0..0x3FFF */
            u32 lin = (u32)(lvl >> 6); if (lin > 255) lin = 255;
            tl = aica_logs[lin];
            pan = (uint32_t)((vr * 255) / (vl + vr));
        }
        if (!a->started) {
            /* ADSR -> AICA AEG */
            uint16_t a1 = spu.regs[vi * 8 + 4], a2 = spu.regs[vi * 8 + 5];
            float t_att = ps1_rate_ms((a1 >> 8) & 0x7F, 0) * (((a1 >> 15) & 1) ? 1.3f : 1.0f);
            float t_dec = ps1_rate_ms(((a1 >> 4) & 0xF) << 2, 1);
            int sl = a1 & 0xF;
            float t_sus = ((a2 >> 14) & 1) ? ps1_rate_ms((a2 >> 6) & 0x7F, 1) : 1e9f;   /* only decreasing sustain */
            float t_rel = ps1_rate_ms((a2 & 0x1F) << 2, 1);
            int ar = aica_rate_for(aica_ar_ms, t_att);
            int d1r = aica_rate_for(aica_dr_ms, t_dec);
            int d2r = (t_sus >= 1e8f) ? 0 : aica_rate_for(aica_dr_ms, t_sus);
            int rr = aica_rate_for(aica_dr_ms, t_rel);
            float db = -20.0f * __builtin_log10f((float)(sl + 1) / 16.0f);
            int dl = (int)(db / 3.0f + 0.5f); if (dl > 31) dl = 31; if (dl < 0) dl = 0;
            a->rr_ms = (int)(t_rel > 20000 ? 20000 : t_rel);            aica_chan_start(vi, a->ring, RING_SAMPLES, rate, (u8)tl, (u8)pan,
                            ((u32)d2r << 11) | ((u32)d1r << 6) | (u32)ar,
                            (0xFu << 10) | ((u32)dl << 5) | (u32)rr);
            a->started = 1; g_aica_starts++;
            a->est_pos = 0; a->sync_ctr = 2;
            a->last_freq = rate; a->last_tl = tl; a->last_pan = pan;
            a->rpos = 0;
        } else {
            if (rate != a->last_freq) { chw32(vi, 0x18, aica_freq_reg(rate)); a->last_freq = rate; }
            if (tl != a->last_tl)     { chw8(vi, 0x29, (u8)tl); a->last_tl = tl; }
            if (pan != a->last_pan)   { chw8(vi, 0x24, aica_pan_reg((int)pan)); a->last_pan = pan; }
        }

        /* reverb input: resample this voice from the SH4 ring copy at 22050 Hz */
        if (rvb_on && (rvb_mask & (1u << vi)) && a->local && v->env_phase != ENV_OFF) {
            uint32_t hw = (pos << 16);
            int32_t diff = (int32_t)(a->rpos - hw); if (diff > (64 << 16) || diff < -(64 << 16)) a->rpos = hw;
            uint32_t step = (uint32_t)(((uint64_t)rate << 16) / 22050u);
            int32_t gl = (vol15(spu.regs[vi * 8 + 0]) * v->env_level) >> 15;
            int32_t gr = (vol15(spu.regs[vi * 8 + 1]) * v->env_level) >> 15;
            for (int i = 0; i < rvb_n; i++) {
                int32_t s = a->local[(a->rpos >> 16) & RING_MASK];
                rvb_in_l[i] += (s * gl) >> 14;
                rvb_in_r[i] += (s * gr) >> 14;
                a->rpos += step;
            }
        }
    }

    /* reverb output on channels 24/25 (22050 Hz, mono each, hard-panned) */
    if (rvb_on && rvb_n > 0) {
        for (int i = 0; i < rvb_n; i++) reverb_step(rvb_clamp(rvb_in_l[i]), rvb_clamp(rvb_in_r[i]), &rvb.out_l[i], &rvb.out_r[i]);
        uint32_t w = rvb.wpos;
        for (int side = 0; side < 2; side++) {
            const int16_t *src = side ? rvb.out_r : rvb.out_l;
            uint32_t ring = side ? rvb.ring_r : rvb.ring_l, wp = w; int n = rvb_n;
            while (n > 0) { int chunk = RVB_RING - (int)wp; if (chunk > n) chunk = n; spu_memload(ring + wp * 2, (void *)src, (size_t)chunk * 2); wp = (wp + chunk) & (RVB_RING - 1); src += chunk; n -= chunk; }
            if (side) rvb.wpos = wp;
        }
        if (!rvb.started) {
            u32 aeg = 0x1Fu;                                          /* no envelope */
            aica_chan_start(RVB_CH_L, rvb.ring_l, RVB_RING, 22050, 0, 0,   aeg, (0xFu << 10) | 0x1Fu);
            aica_chan_start(RVB_CH_R, rvb.ring_r, RVB_RING, 22050, 0, 255, aeg, (0xFu << 10) | 0x1Fu);
            rvb.started = 1;
        }
    } else if (rvb.started) {
        aica_chan_keyoff(RVB_CH_L); aica_chan_keyoff(RVB_CH_R); rvb.started = 0; rvb.wpos = 0;
    }
    spu.active_mask = active;
    { u32 pl = 0; for (int i = 0; i < 24; i++) if (av[i].started) pl++; g_aica_playing = pl; }
    g_aica_rvb = rvb_on ? (rvb_mask | 0x80000000u) : 0;
}
#endif /* SPU_AICA */

/* ------------------------------------------------------------------ */
/* Public interface                                                    */
/* ------------------------------------------------------------------ */

void ps1_spu_init(void) {
    memset(&spu, 0, sizeof(spu));
    spu_pcm_buffer = (int16_t *)memalign(32, SPU_BUF_SIZE);
    spu_ram = (uint8_t *)memalign(32, SPU_RAM_SIZE);
    if (!spu_pcm_buffer || !spu_ram) { dlog("SPU: alloc failed"); return; }
    memset(spu_ram, 0, SPU_RAM_SIZE);
    memset(spu_pcm_buffer, 0, SPU_BUF_SIZE);
#if SPU_AICA
    aica_voice_init();
    spu_started = aica_ready;
#else
    snd_stream_init();
    spu_hnd = snd_stream_alloc(spu_callback, SPU_BUF_SIZE);
    if (spu_hnd == SND_STREAM_INVALID) { dlog("SPU: stream alloc failed"); return; }
    snd_stream_start(spu_hnd, 44100, 1);
    spu_started = 1;
    dlog("SPU: stream started");
#endif
}

uint64_t g_spu_last_ms = 0;
void ps1_spu_poll(void) {
    if (!spu_started) return;
    uint64_t t0 = timer_us_gettime64();
    g_spu_last_ms = t0 / 1000;
#if SPU_AICA
    aica_poll();
#else
    snd_stream_poll(spu_hnd);
#endif
    prof_spu_us += timer_us_gettime64() - t0;
}

void ps1_spu_shutdown(void) {
    if (!spu_started) return;
#if SPU_AICA
    for (int i = 0; i < 24; i++) if (av[i].started) aica_chan_keyoff(i);
    spu_started = 0;
    return;
#endif
    snd_stream_stop(spu_hnd);
    snd_stream_destroy(spu_hnd);
    snd_stream_shutdown();
    spu_started = 0;
}

void ps1_spu_dma_transfer(mem_t *mem, uint32_t madr, uint32_t words) {
    if (!spu_ram) return;
    uint32_t size = words * 4;
    uint32_t m_addr = madr & 0x1FFFFC;
    if (spu.ram_addr >= SPU_RAM_SIZE) return;
    if (spu.ram_addr + size > SPU_RAM_SIZE) size = SPU_RAM_SIZE - spu.ram_addr;
    if (m_addr + size > RAM_SIZE) size = RAM_SIZE - m_addr;
    memcpy(spu_ram + spu.ram_addr, mem->ram + m_addr, size);
    spu.ram_addr += size;
}

static void key_on(int vi) {
    spu_voice_t *v = &spu.v[vi];
    v->cur_addr = (uint32_t)spu.regs[vi * 8 + 3] * 8;
    v->repeat_addr = v->cur_addr;
    v->s1 = v->s2 = 0;
    v->pos = 0;
    v->block_valid = 0;
    v->playing = 1;
    v->env_phase = ENV_ATTACK;
    v->env_level = 0;
    v->env_wait = 0;
    spu.endx &= ~(1u << vi);
#if SPU_AICA
    aica_key_on(vi);
#endif
}

static void key_off(int vi) {
    spu_voice_t *v = &spu.v[vi];
    if (v->env_phase != ENV_OFF) v->env_phase = ENV_RELEASE;
#if SPU_AICA
    aica_key_off(vi);
#endif
}

void ps1_spu_write_reg(u32 addr, u16 val) {
    u32 offset = (addr - SPU_REG_BASE) / 2;
    if (offset >= 512) return;
    spu.regs[offset] = val;

    if (addr < 0x1F801D80) {                                /* voice registers */
        int vi = (addr - SPU_REG_BASE) >> 4;
        int r = (addr >> 1) & 7;
        if (r == 7 && vi < 24) spu.v[vi].repeat_addr = (uint32_t)val * 8;   /* repeat address */
        return;
    }
    switch (addr) {
    case 0x1F801D88: for (int i = 0; i < 16; i++) if (val & (1 << i)) key_on(i); break;
    case 0x1F801D8A: for (int i = 0; i < 8; i++)  if (val & (1 << i)) key_on(16 + i); break;
    case 0x1F801D8C: for (int i = 0; i < 16; i++) if (val & (1 << i)) key_off(i); break;
    case 0x1F801D8E: for (int i = 0; i < 8; i++)  if (val & (1 << i)) key_off(16 + i); break;
    case 0x1F801DA6: spu.ram_addr = (uint32_t)val * 8; break;
    case 0x1F801DA8:                                        /* transfer FIFO */
        if (spu_ram && spu.ram_addr + 1 < SPU_RAM_SIZE) {
            spu_ram[spu.ram_addr++] = (uint8_t)(val & 0xFF);
            spu_ram[spu.ram_addr++] = (uint8_t)(val >> 8);
        }
        break;
    default: break;
    }
}

u16 ps1_spu_read_reg(u32 addr) {
    u32 offset = (addr - SPU_REG_BASE) / 2;
    switch (addr) {
    case 0x1F801DAE: return spu.regs[(0x1DAA - 0x1C00) / 2] & 0x3F;   /* SPUSTAT mirrors SPUCNT mode */
    case 0x1F801D9C: return (u16)(spu.endx & 0xFFFF);
    case 0x1F801D9E: return (u16)(spu.endx >> 16);
    case 0x1F801DA6: return (u16)(spu.ram_addr / 8);
    default: break;
    }
    if (offset < 512) return spu.regs[offset];
    return 0;
}

uint32_t ps1_spu_get_active_channels(void) {
    return spu.active_mask;
}
