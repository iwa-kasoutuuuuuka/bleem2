#include <kos.h>
#include "mips.h"
#include <stdio.h>
#include <string.h>

/*
 * PS1 DMA controller (0x1F801080-0x1F8010FF)
 *  ch0 MDEC in, ch1 MDEC out, ch2 GPU, ch3 CD-ROM, ch4 SPU, ch5 PIO, ch6 OTC
 */

typedef struct {
    u32 madr;
    u32 bcr;
    u32 chcr;
} dma_ch_t;

static dma_ch_t dma[7];
static u32 dpcr = 0x07654321;
static u32 dicr = 0;

void ps1_dma_init(void) {
    memset(dma, 0, sizeof(dma));
    dpcr = 0x07654321;
    dicr = 0;
}

static void dma_update_irq(void) {
    u32 flags = (dicr >> 24) & 0x7F;
    u32 enable = (dicr >> 16) & 0x7F;
    int master = (dicr & 0x8000) || ((dicr & 0x800000) && (flags & enable));
    u32 old = dicr & 0x80000000u;
    if (master) dicr |= 0x80000000u; else dicr &= ~0x80000000u;
    if (!old && (dicr & 0x80000000u) && g_cpu) g_cpu->i_stat |= (1u << 3);
}

static void dma_finish(int ch) {
    dma[ch].chcr &= ~0x11000000u;
    if (dicr & (1u << (16 + ch))) {
        u32 was = dicr & (1u << (24 + ch));
        dicr |= (1u << (24 + ch));
        dma_update_irq();
        /* hardware only interrupts on the 0->1 edge of bit 31; with instant transfers a
         * completion can land while another channel's flag is still pending (e.g. DMA1 during
         * the CD/DMA3 handler) and would never be serviced, so a new flag always interrupts */
        if (!was && (dicr & 0x800000u) && g_cpu) g_cpu->i_stat |= (1u << 3);
    }
}

/* DMA1 (MDEC out) stays busy until the MDEC has produced all requested words:
 * a game may start it before feeding the input through DMA0. */
static int dma1_wait = 0;
static u32 dma1_left = 0;
static u32 dma1_words = 0;            /* size of the current DMA1 request */
static int dma1_fin_delay = 0;         /* cycles until DMA1 reports completion */
static void dma1_resume(mem_t *mem) {
    u32 madr = dma[1].madr & 0x1FFFFC;
    u32 n = dma1_left;
    if (madr + n * 4 > 0x200000) n = (0x200000 - madr) / 4;
    u32 done = mdec_dma_out((u32 *)(mem->ram + madr), n);
    if (done) jit_invalidate(0x80000000u | madr, done * 4);
    dma[1].madr = (madr + done * 4) & 0xFFFFFF;
    dma1_left = (done < n) ? dma1_left - done : 0;     /* words past the end of RAM are dropped */
    dma1_wait = dma1_left != 0;
    /* the data is in RAM now, but completion (CHCR, DICR flag, IRQ) is reported later: the game
     * starts the next strip from its DMA IRQ handler, and a completion raised inside that
     * handler would be acknowledged together with the previous one and lost */
    if (!dma1_wait) dma1_fin_delay = 256 + (int)dma1_words;
}

/* called with elapsed CPU cycles (from cd_update, which every scheduler loop calls) */
void dma_tick(u32 cycles) {
    if (dma1_fin_delay > 0 && (dma1_fin_delay -= (int)cycles) <= 0) { dma1_fin_delay = 0; dma_finish(1); }
}

volatile u32 g_dma_nodes = 0, g_dma_words = 0, g_dma_ch = 99;
uint64_t prof_dma_us = 0, prof_dma_ch_us[8], prof_dma_gp0_us = 0; u32 prof_dma_count[8];
uint64_t g_mdec_win_us = 0;                 /* MDEC (DMA0+DMA1, decode included) time since the last WIN line */
static void dma_execute_inner(int ch, mem_t *mem, gpu_t *gpu);
static void dma_execute(int ch, mem_t *mem, gpu_t *gpu) {
    uint64_t t0 = timer_us_gettime64();
    dma_execute_inner(ch, mem, gpu);
    uint64_t dt = timer_us_gettime64() - t0;
    prof_dma_us += dt; prof_dma_ch_us[ch & 7] += dt; prof_dma_count[ch & 7]++;
    if (ch <= 1) g_mdec_win_us += dt;
}
static void dma_execute_inner(int ch, mem_t *mem, gpu_t *gpu) {
    extern volatile int g_jit_stage;
    g_jit_stage = 10 + ch; g_dma_ch = ch; g_dma_nodes = 0; g_dma_words = 0;
    u32 madr = dma[ch].madr & 0x1FFFFC;
    u32 bcr  = dma[ch].bcr;
    u32 chcr = dma[ch].chcr;
    int to_ram = !(chcr & 1);
    int sync = (chcr >> 9) & 3;
    int step = (chcr & 2) ? -4 : 4;

    u32 words;
    if (sync == 0) words = (bcr & 0xFFFF) ? (bcr & 0xFFFF) : 0x10000;
    else words = (bcr & 0xFFFF) * ((bcr >> 16) & 0xFFFF);

    switch (ch) {
    case 2:                                           /* GPU */
        if (sync == 2) {                              /* linked list */
            u32 addr = madr;
            int guard = 0;
            static u32 trail[8];
            uint64_t t_start = timer_us_gettime64();
            u32 first_addr = madr;
            while (guard++ < 16384) {
                u32 *node = (u32 *)(mem->ram + (addr & 0x1FFFFC));
                u32 header = node[0];
                u32 count = header >> 24;
                g_dma_nodes = guard; g_dma_words += count;
                trail[guard & 7] = addr;
                if ((guard & 255) == 0 && (timer_us_gettime64() - t_start) > 20000) {
                    static int tlog = 0;
                    if (tlog++ < 20) dlog("DMA2 abort: >20ms n=%d addr=%06lX hdr=%08lX words=%lu first=%06lX", guard,
                                          (unsigned long)addr, (unsigned long)header, (unsigned long)g_dma_words, (unsigned long)first_addr);
                    break;
                }
                if ((guard & 2047) == 0) {
                    static int plog = 0;
                    if (plog++ < 40) dlog("DMA2 prog n=%d us=%lu addr=%06lX hdr=%08lX words=%lu polys=%lu first=%06lX", guard,
                                          (unsigned long)(timer_us_gettime64() - t_start), (unsigned long)addr, (unsigned long)header,
                                          (unsigned long)g_dma_words, (unsigned long)gpu->polys, (unsigned long)first_addr);
                }
                if (guard == 16384) {
                    dlog("DMA2: list too long, madr=%06lX last=%06lX %06lX %06lX %06lX hdr=%08lX n1=%08lX", (unsigned long)madr,
                         (unsigned long)trail[(guard - 1) & 7], (unsigned long)trail[(guard - 2) & 7], (unsigned long)trail[(guard - 3) & 7],
                         (unsigned long)trail[(guard - 4) & 7], (unsigned long)header, (unsigned long)node[1]);
                    break;
                }
                if (g_dma_words > 2000000) { dlog("DMA2: runaway linked list at %06lX", (unsigned long)addr); break; }
                { uint64_t tg0 = timer_us_gettime64(); gpu_write_words(gpu, &node[1], (int)count); prof_dma_gp0_us += timer_us_gettime64() - tg0; }
                if (header & 0x800000) break;
                addr = header & 0xFFFFFF;
                if (addr == 0xFFFFFF) break;
            }
            dma[ch].madr = 0xFFFFFF;
        } else {
            u32 addr = madr;
            if (!to_ram) {
                if (step == 4 && (addr & 0x1FFFFC) + words * 4 <= 0x200000) { gpu_write_words(gpu, (const u32 *)(mem->ram + (addr & 0x1FFFFC)), (int)words); addr += words * 4; words = 0; }
                for (u32 i = 0; i < words; i++) {
                    gpu_write_data(gpu, *(u32 *)(mem->ram + (addr & 0x1FFFFC)));
                    addr += step;
                }
            } else {
                for (u32 i = 0; i < words; i++) {
                    *(u32 *)(mem->ram + (addr & 0x1FFFFC)) = gpu_read_data(gpu);
                    addr += step;
                }
            }
            dma[ch].madr = addr & 0xFFFFFF;
        }
        break;
    case 3: {                                         /* CD-ROM -> RAM */
        u32 addr = madr;
        static int dma3_log = 0;
        if (dma3_log++ < 16) dlog("DMA3 -> %06lX words=%lu bcr=%08lX", (unsigned long)madr, (unsigned long)words, (unsigned long)bcr);
        for (u32 i = 0; i < words; i++) {
            *(u32 *)(mem->ram + (addr & 0x1FFFFC)) = cd_dma_read32();
            addr += step;
        }
        dma[ch].madr = addr & 0xFFFFFF;
        break;
    }
    case 4:                                           /* SPU */
        { static int d4 = 0; if (d4++ < 12) dlog("DMA4 %s %06lX words=%lu", to_ram ? "<-spu" : "->spu", (unsigned long)madr, (unsigned long)words); }
        if (!to_ram) ps1_spu_dma_transfer(mem, madr, words);
        else {
            u32 addr = madr;
            for (u32 i = 0; i < words; i++) { *(u32 *)(mem->ram + (addr & 0x1FFFFC)) = 0; addr += 4; }
        }
        break;
    case 6: {                                         /* OTC: reverse linked list */
        u32 addr = madr;
        for (u32 i = 0; i < words; i++) {
            u32 next = (i == words - 1) ? 0xFFFFFF : ((addr - 4) & 0x1FFFFC);
            *(u32 *)(mem->ram + (addr & 0x1FFFFC)) = next;
            addr -= 4;
        }
        break;
    }
    case 0: {                                         /* RAM -> MDEC */
        u32 n = words;
        if (madr + n * 4 > 0x200000) n = (0x200000 - madr) / 4;
        mdec_dma_in((const u32 *)(mem->ram + madr), n);
        dma[ch].madr = (madr + words * 4) & 0xFFFFFF;
        g_jit_stage = 20;
        dma_finish(ch);
        if (dma1_wait) dma1_resume(mem);              /* output that was waiting for this input */
        return;
    }
    case 1:                                           /* MDEC -> RAM (may wait for input) */
        dma1_left = words;
        dma1_words = words;
        dma[ch].madr = madr;
        dma1_resume(mem);
        g_jit_stage = 20;
        return;
    default:                                          /* PIO: no-op */
        break;
    }
    if (to_ram || ch == 6) jit_invalidate(0x80000000u | madr, (words ? words : 1) * 4 + 4);
    g_jit_stage = 20;
    dma_finish(ch);
}

void dma_write(u32 paddr, u32 val, mem_t *mem, gpu_t *gpu) {
    if (paddr == 0x1F8010F0) { dpcr = val; return; }
    if (paddr == 0x1F8010F4) {
        dicr = (dicr & 0xFF000000u & ~(val & 0x7F000000u)) | (val & 0x00FFFFFFu);
        dma_update_irq();
        return;
    }
    int ch = (paddr >> 4) & 0x7;
    if (ch > 6) return;
    switch (paddr & 0xF) {
        case 0x0: dma[ch].madr = val & 0xFFFFFF; break;
        case 0x4: dma[ch].bcr = val; break;
        case 0x8: {
            dma[ch].chcr = val;
            if (ch == 1 && !(val & 0x01000000)) { dma1_wait = 0; dma1_fin_delay = 0; }
            int sync = (val >> 9) & 3;
            int enabled = (dpcr >> (ch * 4 + 3)) & 1;
            if ((val & 0x01000000) && enabled && (sync != 0 || (val & 0x10000000)))
                dma_execute(ch, mem, gpu);
            break;
        }
    }
}

u32 dma_read(u32 paddr) {
    if (paddr == 0x1F8010F0) return dpcr;
    if (paddr == 0x1F8010F4) return dicr;
    int ch = (paddr >> 4) & 0x7;
    if (ch > 6) return 0;
    switch (paddr & 0xF) {
        case 0x0: return dma[ch].madr;
        case 0x4: return dma[ch].bcr;
        case 0x8: return dma[ch].chcr;
    }
    return 0;
}

/* 8/16-bit stores to the DMA registers (libcd sets/clears single DICR enable bits with sb):
 * merge into the 32-bit register; DICR flag bits are only acknowledged for written bytes */
void dma_write_sub(u32 paddr, u32 val, int width, mem_t *mem, gpu_t *gpu) {
    u32 reg = paddr & ~3u;
    int sh = (paddr & 3) * 8;
    u32 mask = ((width == 1) ? 0xFFu : 0xFFFFu) << sh;
    u32 v = (val << sh) & mask;
    if (reg == 0x1F8010F4) {
        u32 full = (dicr & 0x00FFFFFFu & ~mask) | (v & 0x00FFFFFFu) | (v & 0x7F000000u);
        dma_write(reg, full, mem, gpu);
        return;
    }
    dma_write(reg, (dma_read(reg) & ~mask) | v, mem, gpu);
}
