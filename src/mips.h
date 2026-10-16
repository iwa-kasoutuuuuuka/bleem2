#ifndef MIPS_H
#define MIPS_H

#include <stdint.h>
#include <stdarg.h>
#include <kos.h>

/* ---------------------------------------------------------------------
 * Build configuration
 * ------------------------------------------------------------------- */
#define RELEASE_BUILD         0   /* 1: no HUD, no sampler thread, no serial log */
#define MASTER_HUD            1   /* on-screen debug HUD                     */
#define HUD_VRAM_OVERLAY      0   /* draw the whole PS1 VRAM in a corner     */
#define BOOT_AUTOBOOT         0   /* 1: skip BIOS, HLE-boot the PS-X EXE      */
                                  /* 0: execute the BIOS image (OpenBIOS)     */
#define USE_JIT               1   /* SH-4 dynarec: off until re-verified      */
#define CPU_CYCLES_PER_FRAME  564480  /* 33868800 / 60 */
#define PC_HISTORY_SIZE       256
#define PC_SAMPLES            256   /* coarse PC samples (every 2048 instructions) */
extern uint32_t g_pc_samples[PC_SAMPLES];
extern uint32_t g_sample_tick;
#define LOG_FLUSH_FRAMES      300 /* VMU log flush interval                   */
#define AUTOPLAY              1   /* frame-scripted pad input for deterministic test runs */
#define MEMCARD_ENABLED       0   /* PS1 memory card in port 1 (OpenBIOS card driver stalls the game: off) */
#define SPU_AICA              1   /* 1: PS1 voices on AICA hardware channels (SH4 decodes ADPCM only) */
#if RELEASE_BUILD
#undef MASTER_HUD
#define MASTER_HUD 0
#endif
#define PROFILER              (!RELEASE_BUILD)
#ifndef VMU_LOG_FLUSH
#define VMU_LOG_FLUSH         (!RELEASE_BUILD)   /* periodic VMU log writes (slow on real hardware) */
#endif

typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t  u8;
typedef int32_t  s32;
typedef int16_t  s16;
typedef int8_t   s8;

extern uint64_t total_cycles_executed;
extern uint64_t jit_cycles_executed;

struct mips_s;
struct mem_s;
struct gpu_s;

/* MIPS R3000A CPU state */
typedef struct mips_s {
    u32 r[32];          /* GPRs                                            */
    u32 pc;             /* MUST directly follow r[] (JIT addresses r[32])  */
    u32 hi, lo;
    u32 gte_dr[32];     /* COP2 data registers                             */
    u32 gte_cr[32];     /* COP2 control registers                          */
    u32 cop0[32];       /* COP0: 8=BadVAddr 12=SR 13=CAUSE 14=EPC 15=PRID  */

    int stop;
    u8 *ram_base;

    /* interrupt controller */
    u32 i_stat;
    u32 i_mask;
    u32 irq_count;

    u32 pad_state;      /* PS1 digital pad (active low)                    */
    u32 vblank_count;

    u32 pc_history[PC_HISTORY_SIZE];
    int pc_history_pos;

    /* exception bookkeeping */
    int exc_pending;
    u32 exc_code;
    u32 exc_badvaddr;
    u32 exc_count;
    u32 last_exc_pc;
    u32 last_exc_code;

    /* HLE BIOS */
    int hle_enabled;
    u32 hle_calls;
    u32 hle_pad_buf1, hle_pad_buf2;
    u32 last_bios_call, last_bios_fn;

    /* JIT driver state */
    int budget;         /* cycles left in the current chunk (decremented by blocks) */
    u32 link_site;      /* literal address to patch with the next block entry, or 0 */
} mips_t;

/* Memory */
#define RAM_SIZE  (2 * 1024 * 1024)
#define BIOS_SIZE (512 * 1024)
typedef struct mem_s {
    u8 *ram;
    u8 *bios;
    u8 *scratch;
} mem_t;

/* PS1 GPU state */
typedef struct gpu_s {
    u16 *vram;              /* 1024x512 native 15-bit                      */
    u32 stat_bits;          /* GPUSTAT bits 0-15 (E1/E6)                   */

    u32 pkt[16];
    int pkt_ptr;
    int pkt_size;
    int polyline_active;

    /* VRAM transfers */
    int xfer_dir;           /* 0 none, 1 CPU->VRAM, 2 VRAM->CPU            */
    int xfer_x, xfer_y, xfer_w, xfer_h;
    int xfer_idx;
    u32 gpuread;

    /* drawing environment */
    int draw_x0, draw_y0, draw_x1, draw_y1;
    int off_x, off_y;
    u32 tex_window;
    u16 tpage;              /* current GP0(E1) texture page            */
    int mask_set, mask_check;

    /* display environment */
    int disp_x, disp_y;
    int disp_enabled;
    int dma_dir;
    u32 disp_mode;
    int hrange_x1, hrange_x2, vrange_y1, vrange_y2;
    int odd_frame;
    uint64_t frame_start_cycles;
    int irq;

    /* stats */
    u32 draw_calls;
    u32 polys;
    u32 frame_polys;
    char status_msg[128];
} gpu_t;

/* ---- cpu.c ---- */
void cpu_reset(mips_t *cpu);
void cpu_execute(mips_t *cpu, mem_t *mem, gpu_t *gpu, int cycles);
void cpu_raise(mips_t *cpu, u32 code, u32 badvaddr);

/* ---- mem.c ---- */
extern mips_t *g_cpu;
extern mem_t  *g_mem;
extern gpu_t  *g_gpu;
mem_t *mem_init(void);
u32  mem_read32(mips_t *cpu, mem_t *mem, gpu_t *gpu, u32 addr);
u16  mem_read16(mips_t *cpu, mem_t *mem, gpu_t *gpu, u32 addr);
u8   mem_read8(mips_t *cpu, mem_t *mem, u32 addr);
void mem_write32(mips_t *cpu, mem_t *mem, gpu_t *gpu, u32 addr, u32 val);
void mem_write16(mips_t *cpu, mem_t *mem, gpu_t *gpu, u32 addr, u16 val);
void mem_write8(mips_t *cpu, mem_t *mem, u32 addr, u8 val);
int  mem_load_bios(mem_t *mem, const char *path);

/* ---- timers.c ---- */
void timers_init(void);
void timers_update(mips_t *cpu, u32 cycles);
u32  timers_read(mips_t *cpu, u32 paddr);
void timers_write(mips_t *cpu, u32 paddr, u32 val);
void timers_debug(int ch, u32 *count, u32 *mode, u32 *target);

/* ---- sio.c ---- */
void sio_init(void);
void sio_update(mips_t *cpu, u32 cycles);
u32  sio_read(mips_t *cpu, u32 paddr);
void sio_write(mips_t *cpu, u32 paddr, u32 val, int width);

/* ---- dma.c ---- */
void ps1_dma_init(void);
void dma_write(u32 paddr, u32 val, mem_t *mem, gpu_t *gpu);
u32  dma_read(u32 paddr);

/* ---- cdrom.c ---- */
void cd_init(void);
int  cd_init_bin(const char *filename);
int  cd_read_raw_sector(u32 lba, u8 *dest);
int  cd_read_data_sector(u32 lba, u8 *dest);
u32  cd_load_exe_to_ram(mips_t *cpu, mem_t *mem);
u8   cd_read_reg(u32 paddr);
void cd_write_reg(u32 paddr, u8 val);
void cd_update(mips_t *cpu, u32 cycles);
u32  cd_dma_read32(void);
const char *cd_image_name(void);
void cd_stats(char *out, int len);

/* ---- gpu.c ---- */
void gpu_init(gpu_t *gpu);
void gpu_reset(gpu_t *gpu);
u32  gpu_read_data(gpu_t *gpu);
u32  gpu_read_status(gpu_t *gpu);
void gpu_write_data(gpu_t *gpu, u32 val);     /* GP0 */
void gpu_write_words(gpu_t *gpu, const u32 *w, int n);
void gpu_write_status(gpu_t *gpu, u32 val);   /* GP1 */
void gpu_vblank(gpu_t *gpu);
void gpu_blit_to_screen(gpu_t *gpu, u16 *fb, int out_w, int out_h);

/* ---- display.c (PVR) ---- */
void display_init(void);
void display_begin(gpu_t *gpu);
u16 *display_hud_buffer(int *stride, int *w, int *h);
void display_end(void);
void gpu_write_command(gpu_t *gpu, u32 val);  /* alias of GP0 for DMA */

/* ---- gte.c ---- */
void gte_execute(mips_t *cpu, u32 cmd);

/* ---- hle.c ---- */
void hle_bios_call(mips_t *cpu, mem_t *mem, gpu_t *gpu, u32 pc);
void hle_syscall(mips_t *cpu);
void hle_vblank(mips_t *cpu, mem_t *mem);

/* ---- jit.c ---- */
void jit_init(void);
int  jit_execute(mips_t *cpu, mem_t *mem, gpu_t *gpu);
int  jit_run(mips_t *cpu, mem_t *mem, gpu_t *gpu, int budget);
void jit_flush_cache(void);
void jit_invalidate(u32 addr, u32 len);
void jit_stats(char *out, int len);
extern int g_jit_verify_running;
#define JIT_VERIFY 0        /* re-run store-free blocks in the interpreter and compare registers */
#define JIT_VERIFY_FROM_FRAME 1200

/* ---- spu.c ---- */
void ps1_spu_init(void);
void ps1_spu_poll(void);
void ps1_spu_shutdown(void);
void ps1_spu_write_reg(u32 addr, u16 val);
u16  ps1_spu_read_reg(u32 addr);
void ps1_spu_dma_transfer(mem_t *mem, u32 madr, u32 words);
u32  ps1_spu_get_active_channels(void);

/* ---- input.c ---- */
extern u16 g_pad_state;
void input_poll(void);

/* ---- vmu.c ---- */
void vmu_save_init(void);
u8   vmu_sio_exchange(u8 data);

/* ---- log.c ---- */
void dlog(const char *fmt, ...);
int  dlog_flush_vmu(void);
void memcard_init(void);
void memcard_poll(void);
void memcard_reset(void);
u8   memcard_transfer(u8 tx, int *ack);
const char *dlog_line_from_end(int n);
int  dlog_line_count(void);

#endif
