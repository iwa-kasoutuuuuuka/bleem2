#include "mips.h"
#include <stdio.h>
#include <string.h>
#include <malloc.h>

/*
 * PS1 physical memory map
 *  0x00000000-0x007FFFFF  2MB RAM (mirrored 4x)
 *  0x1F000000-0x1F7FFFFF  Expansion 1 (absent -> 0xFF)
 *  0x1F800000-0x1F8003FF  Scratchpad (1KB)
 *  0x1F801000-0x1F802FFF  I/O ports
 *  0x1FC00000-0x1FC7FFFF  BIOS ROM (512KB)
 *  0xFFFE0130             Cache control (KSEG2)
 */

mips_t *g_cpu = NULL;
mem_t  *g_mem = NULL;
gpu_t  *g_gpu = NULL;

u8 g_scratchpad[1024] __attribute__((aligned(32)));
#define scratchpad g_scratchpad
static u32 mem_ctrl[9];
static u32 ram_size_reg = 0x00000B88;
static u32 cache_ctrl = 0;
static u32 unhandled_reads = 0, unhandled_writes = 0;

mem_t *mem_init(void) {
    mem_t *mem = (mem_t *)malloc(sizeof(mem_t));
    if (!mem) return NULL;
    mem->ram = (u8 *)memalign(32, RAM_SIZE);
    mem->bios = (u8 *)memalign(32, BIOS_SIZE);
    if (!mem->ram || !mem->bios) return NULL;
    memset(mem->ram, 0, RAM_SIZE);
    memset(mem->bios, 0, BIOS_SIZE);
    memset(scratchpad, 0, sizeof(scratchpad));
    mem->scratch = scratchpad;
    ps1_dma_init();
    timers_init();
    sio_init();
    return mem;
}

int mem_load_bios(mem_t *mem, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(mem->bios, 1, BIOS_SIZE, f);
    fclose(f);
    return (n == BIOS_SIZE) ? 0 : -2;
}

/* ------------------------------------------------------------------ */
/* I/O                                                                 */
/* ------------------------------------------------------------------ */

int g_io_trace = 0;     /* >0: log that many non-timer I/O accesses */

static u32 io_read32_inner(mips_t *cpu, gpu_t *gpu, u32 p);
static u32 io_read32(mips_t *cpu, gpu_t *gpu, u32 p) {
    u32 v = io_read32_inner(cpu, gpu, p);
    if (g_io_trace > 0 && !(p >= 0x1F801100 && p < 0x1F801130) && p != 0x1F801814) {
        g_io_trace--;
        dlog("IO rd %08lX -> %08lX pc=%08lX", (unsigned long)p, (unsigned long)v, (unsigned long)cpu->pc);
    }
    return v;
}

static u32 io_read32_inner(mips_t *cpu, gpu_t *gpu, u32 p) {
    if (p >= 0x1F801080 && p < 0x1F801100) return dma_read(p);
    if (p >= 0x1F801100 && p < 0x1F801130) return timers_read(cpu, p);
    if (p >= 0x1F801040 && p < 0x1F801060) return sio_read(cpu, p);
    if (p >= 0x1F801C00 && p < 0x1F802000)
        return ps1_spu_read_reg(p) | ((u32)ps1_spu_read_reg(p + 2) << 16);
    if (p >= 0x1F801800 && p < 0x1F801804) return cd_read_reg(p);
    if (p >= 0x1F801000 && p < 0x1F801024) return mem_ctrl[(p - 0x1F801000) >> 2];
    switch (p) {
        case 0x1F801060: return ram_size_reg;
        case 0x1F801070: return cpu->i_stat;
        case 0x1F801074: return cpu->i_mask;
        case 0x1F801810: return gpu_read_data(gpu);
        case 0x1F801814: return gpu_read_status(gpu);
        case 0x1F801820: return 0;                 /* MDEC data   */
        case 0x1F801824: return 0x80040000;        /* MDEC status */
        case 0x1F802080: return 0x58534350;        /* "PCSX": PCSX-Redux expansion signature (OpenBIOS polls it) */
    }
    if (p >= 0x1F802000 && p < 0x1F803000) return 0;      /* Expansion 2 */
    if (++unhandled_reads <= 8) dlog("IO rd? %08lX", (unsigned long)p);
    return 0;
}

/* TTY capture: OpenBIOS / PCSX-Redux debug ports and SIO1 TX */
static char tty_buf[128];
static int  tty_len = 0;
static void tty_putc(u8 c) {
    if (c == '\r') return;
    if (c == '\n' || tty_len >= (int)sizeof(tty_buf) - 1) {
        tty_buf[tty_len] = 0;
        if (tty_len) dlog("TTY: %s", tty_buf);
        tty_len = 0;
        if (c != '\n' && c) tty_buf[tty_len++] = c;
        return;
    }
    tty_buf[tty_len++] = c;
}
static inline int is_tty_port(u32 p) {
    return p == 0x1F802023 || p == 0x1F802080 || p == 0x1F801050;
}

static void io_write32(mips_t *cpu, mem_t *mem, gpu_t *gpu, u32 p, u32 val) {
    if (is_tty_port(p)) { tty_putc(val & 0xFF); return; }
    if (g_io_trace > 0 && !(p >= 0x1F801100 && p < 0x1F801130) && p != 0x1F801810) {
        g_io_trace--;
        dlog("IO wr %08lX <- %08lX pc=%08lX", (unsigned long)p, (unsigned long)val, (unsigned long)cpu->pc);
    }
    if (p >= 0x1F801080 && p < 0x1F801100) { dma_write(p, val, mem, gpu); return; }
    if (p >= 0x1F801100 && p < 0x1F801130) { timers_write(cpu, p, val); return; }
    if (p >= 0x1F801040 && p < 0x1F801060) { sio_write(cpu, p, val, 4); return; }
    if (p >= 0x1F801C00 && p < 0x1F802000) {
        ps1_spu_write_reg(p, val & 0xFFFF);
        ps1_spu_write_reg(p + 2, val >> 16);
        return;
    }
    if (p >= 0x1F801800 && p < 0x1F801804) { cd_write_reg(p, val & 0xFF); return; }
    if (p >= 0x1F801000 && p < 0x1F801024) { mem_ctrl[(p - 0x1F801000) >> 2] = val; return; }
    switch (p) {
        case 0x1F801060: ram_size_reg = val; return;
        case 0x1F801070: cpu->i_stat &= val; return;
        case 0x1F801074: cpu->i_mask = val & 0x7FF; return;
        case 0x1F801810: gpu_write_data(gpu, val); return;
        case 0x1F801814: gpu_write_status(gpu, val); return;
        case 0x1F801820: case 0x1F801824: return;  /* MDEC */
    }
    if (p == 0x1F802084) {                         /* Redux: pointer to a message string */
        char s[128];
        for (int i = 0; i < 127; i++) { s[i] = mem_read8(cpu, mem, val + i); if (!s[i]) break; }
        s[127] = 0;
        dlog("REDUX: %s", s);
        return;
    }
    if (p >= 0x1F802000 && p < 0x1F803000) return;  /* Expansion 2 (POST etc.) */
    if (++unhandled_writes <= 8) dlog("IO wr? %08lX=%08lX", (unsigned long)p, (unsigned long)val);
}

/* ------------------------------------------------------------------ */
/* Reads                                                               */
/* ------------------------------------------------------------------ */

u32 mem_read32(mips_t *cpu, mem_t *mem, gpu_t *gpu, u32 addr) {
    u32 p = addr & 0x1FFFFFFF;
    if (p < 0x00800000) return *(u32 *)&mem->ram[p & 0x1FFFFF];
    if (p >= 0x1FC00000 && p < 0x1FC80000) return *(u32 *)&mem->bios[p - 0x1FC00000];
    if (p >= 0x1F800000 && p < 0x1F800400) return *(u32 *)&scratchpad[p & 0x3FF];
    if (p >= 0x1F801000 && p < 0x1F803000) return io_read32(cpu, gpu, p);
    if (p >= 0x1F000000 && p < 0x1F800000) return 0xFFFFFFFF;
    if (addr == 0xFFFE0130) return cache_ctrl;
    return 0;
}

u16 mem_read16(mips_t *cpu, mem_t *mem, gpu_t *gpu, u32 addr) {
    u32 p = addr & 0x1FFFFFFF;
    if (p < 0x00800000) return *(u16 *)&mem->ram[p & 0x1FFFFF];
    if (p >= 0x1FC00000 && p < 0x1FC80000) return *(u16 *)&mem->bios[p - 0x1FC00000];
    if (p >= 0x1F800000 && p < 0x1F800400) return *(u16 *)&scratchpad[p & 0x3FF];
    if (p >= 0x1F801C00 && p < 0x1F802000) return ps1_spu_read_reg(p);
    if (p >= 0x1F801040 && p < 0x1F801060) return sio_read(cpu, p) & 0xFFFF;
    if (p >= 0x1F801800 && p < 0x1F801804) return cd_read_reg(p);
    if (p >= 0x1F801000 && p < 0x1F803000) return (io_read32(cpu, gpu, p & ~3u) >> ((p & 2) * 8)) & 0xFFFF;
    if (p >= 0x1F000000 && p < 0x1F800000) return 0xFFFF;
    return 0;
}

u8 mem_read8(mips_t *cpu, mem_t *mem, u32 addr) {
    u32 p = addr & 0x1FFFFFFF;
    if (p < 0x00800000) return mem->ram[p & 0x1FFFFF];
    if (p >= 0x1FC00000 && p < 0x1FC80000) return mem->bios[p - 0x1FC00000];
    if (p >= 0x1F800000 && p < 0x1F800400) return scratchpad[p & 0x3FF];
    if (p >= 0x1F801800 && p < 0x1F801804) return cd_read_reg(p);
    if (p == 0x1F801040) return sio_read(cpu, p) & 0xFF;
    if (p >= 0x1F801000 && p < 0x1F803000) return (io_read32(cpu, g_gpu, p & ~3u) >> ((p & 3) * 8)) & 0xFF;
    if (p >= 0x1F000000 && p < 0x1F800000) return 0xFF;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Writes                                                              */
/* ------------------------------------------------------------------ */

void mem_write32(mips_t *cpu, mem_t *mem, gpu_t *gpu, u32 addr, u32 val) {
    u32 p = addr & 0x1FFFFFFF;
    if (p < 0x00800000) {
        if (cpu->cop0[12] & 0x10000) return;       /* cache isolated */
        *(u32 *)&mem->ram[p & 0x1FFFFF] = val;
#if USE_JIT
        jit_invalidate(addr, 4);
#endif
        return;
    }
    if (p >= 0x1F800000 && p < 0x1F800400) { *(u32 *)&scratchpad[p & 0x3FF] = val; return; }
    if (p >= 0x1F801000 && p < 0x1F803000) { io_write32(cpu, mem, gpu, p, val); return; }
    if (addr == 0xFFFE0130) { cache_ctrl = val; return; }
}

void mem_write16(mips_t *cpu, mem_t *mem, gpu_t *gpu, u32 addr, u16 val) {
    u32 p = addr & 0x1FFFFFFF;
    if (p < 0x00800000) {
        if (cpu->cop0[12] & 0x10000) return;
        *(u16 *)&mem->ram[p & 0x1FFFFF] = val;
#if USE_JIT
        jit_invalidate(addr, 2);
#endif
        return;
    }
    if (p >= 0x1F800000 && p < 0x1F800400) { *(u16 *)&scratchpad[p & 0x3FF] = val; return; }
    if (p >= 0x1F801C00 && p < 0x1F802000) { ps1_spu_write_reg(p, val); return; }
    if (p >= 0x1F801040 && p < 0x1F801060) { sio_write(cpu, p, val, 2); return; }
    if (p >= 0x1F801800 && p < 0x1F801804) { cd_write_reg(p, val & 0xFF); return; }
    if (is_tty_port(p)) { tty_putc(val & 0xFF); return; }
    if (p >= 0x1F801000 && p < 0x1F803000) {
        if ((p & 2) == 0) io_write32(cpu, mem, gpu, p, val);
        return;
    }
}

void mem_write8(mips_t *cpu, mem_t *mem, u32 addr, u8 val) {
    u32 p = addr & 0x1FFFFFFF;
    if (p < 0x00800000) {
        if (cpu->cop0[12] & 0x10000) return;
        mem->ram[p & 0x1FFFFF] = val;
#if USE_JIT
        jit_invalidate(addr, 1);
#endif
        return;
    }
    if (p >= 0x1F800000 && p < 0x1F800400) { scratchpad[p & 0x3FF] = val; return; }
    if (p >= 0x1F801800 && p < 0x1F801804) { cd_write_reg(p, val); return; }
    if (p == 0x1F801040) { sio_write(cpu, p, val, 1); return; }
    if (is_tty_port(p)) { tty_putc(val); return; }
    if (p >= 0x1F802000 && p < 0x1F803000) return;  /* POST / expansion 2 */
    if (p >= 0x1F801000 && p < 0x1F802000) {
        if ((p & 3) == 0) io_write32(cpu, mem, g_gpu, p, val);
        return;
    }
}
