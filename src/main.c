#include "mips.h"
#include <kos.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include "font_data.h"

KOS_INIT_FLAGS(INIT_DEFAULT);

uint64_t total_cycles_executed = 0;
uint64_t jit_cycles_executed = 0;
extern u32 g_vmu_flush_ms;
extern int g_dlog_serial;
static int fb_w = 640, fb_h = 480, fb_stride = 640;   /* HUD target size / stride */

static volatile int  loop_stage = 0;      /* where the main loop is (watchdog) */
static volatile u32  loop_frames = 0;

/* ---- PC sampling profiler: a thread samples the main thread's saved pc ---- */
#define PROF_BASE   0x8C010000u
#define PROF_SPAN   (1024 * 1024)
static kthread_t *main_thd = NULL;
static u16 prof_c[PROF_SPAN / 32];
static u32 prof_jit[8], prof_other, prof_total;
static void *sampler_thread(void *arg) {
    (void)arg;
    extern int jit_code_tag(u32 pc);
    while (1) {
        thd_sleep(1);
        if (!main_thd) continue;
        u32 pc = main_thd->context.pc;
        prof_total++;
        int t = jit_code_tag(pc);
        if (t >= 0) prof_jit[t & 7]++;
        else if (pc - PROF_BASE < PROF_SPAN) prof_c[(pc - PROF_BASE) >> 5]++;
        else prof_other++;
    }
    return NULL;
}
static void prof_report(void) {
    dlog("PROF n=%lu jit: entry=%lu load=%lu store=%lu exit=%lu call=%lu br=%lu alu=%lu other=%lu",
         (unsigned long)prof_total, (unsigned long)prof_jit[1], (unsigned long)prof_jit[2], (unsigned long)prof_jit[3],
         (unsigned long)prof_jit[4], (unsigned long)prof_jit[5], (unsigned long)prof_jit[6], (unsigned long)prof_jit[7],
         (unsigned long)prof_other);
    char line[200]; int n = 0;
    for (int k = 0; k < 32; k++) {
        int best = -1; u16 bv = 0;
        for (int i = 0; i < (int)(PROF_SPAN / 32); i++) if (prof_c[i] > bv) { bv = prof_c[i]; best = i; }
        if (best < 0 || bv == 0) break;
        n += snprintf(line + n, sizeof(line) - n, "%08lX=%u ", (unsigned long)(PROF_BASE + best * 32), bv);
        prof_c[best] = 0;
        if (n > 70) { dlog("  C %s", line); n = 0; line[0] = 0; }
    }
    if (n) dlog("  C %s", line);
    memset(prof_c, 0, sizeof(prof_c)); memset(prof_jit, 0, sizeof(prof_jit)); prof_other = 0; prof_total = 0;
}
static void *watchdog_thread(void *arg) {
    (void)arg;
    u32 last = 0; int stuck = 0;
    while (1) {
        thd_sleep(2000);
        if (loop_frames == last) {
            stuck++;
            {
                extern volatile int g_jit_stage; extern volatile u32 g_jit_last_fn;
                u32 pc = g_cpu ? g_cpu->pc : 0;
                extern volatile u32 g_dma_nodes, g_dma_words, g_dma_ch;
                printf("WATCHDOG: no frame for %ds, stage=%d jit=%d fn=%08lX frames=%lu pc=%08lX budget=%d dma ch=%lu nodes=%lu words=%lu\n",
                       stuck * 2, loop_stage, g_jit_stage, (unsigned long)g_jit_last_fn, (unsigned long)loop_frames,
                       (unsigned long)pc, g_cpu ? g_cpu->budget : 0, (unsigned long)g_dma_ch, (unsigned long)g_dma_nodes,
                       (unsigned long)g_dma_words);
                if (g_cpu && g_mem && stuck == 2) {
                    for (int k = -4; k < 8; k++)
                        printf("  %08lX: %08lX\n", (unsigned long)(pc + k * 4), (unsigned long)mem_read32(g_cpu, g_mem, g_gpu, pc + k * 4));
                    u16 *f = (u16 *)g_jit_last_fn;
                    if (f) { printf("  code:"); for (int k = 0; k < 24; k++) printf(" %04X", f[k]); printf("\n"); }
                }
            }
        } else { stuck = 0; last = loop_frames; }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* HUD text into the Dreamcast framebuffer (640x480 RGB565)            */
/* ------------------------------------------------------------------ */

static void fb_fill(u16 *fb, int x, int y, int w, int h, u16 c) {
    for (int j = 0; j < h; j++) {
        u16 *row = &fb[(y + j) * fb_stride + x];
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

static void fb_text(u16 *fb, int x, int y, const char *s, u16 color) {
    while (*s) {
        u8 c = (u8)*s++;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c >= 128) c = '?';
        const u8 *gl = font_8x8[c];
        for (int i = 0; i < 8; i++) {
            u8 row = gl[i];
            if (!row) continue;
            u16 *dst = &fb[(y + i) * fb_stride + x];
            for (int j = 0; j < 8; j++) if (row & (0x80 >> j)) dst[j] = color;
        }
        x += 8;
        if (x > fb_w - 8) return;
    }
}

/* ------------------------------------------------------------------ */
/* SH-4 exception trap: dump registers + recent log on screen and halt  */
/* ------------------------------------------------------------------ */

static void sh4_exception_trap(irq_t code, irq_context_t *ctx, void *data) {
    (void)data;
    char b[96];
    u16 *fb = vram_s;
    fb_w = 640; fb_h = 480; fb_stride = 640;
    fb_fill(fb, 0, 0, fb_w, fb_h, 0x0000);
    snprintf(b, sizeof(b), "SH4 EXCEPTION %04X PC=%08lX PR=%08lX SR=%08lX",
             (unsigned)code, (unsigned long)ctx->pc, (unsigned long)ctx->pr, (unsigned long)ctx->sr);
    fb_text(fb, 8, 8, b, 0xF800);
    for (int i = 0; i < 16; i += 2) {
        snprintf(b, sizeof(b), "R%02d=%08lX R%02d=%08lX", i, (unsigned long)ctx->r[i], i + 1, (unsigned long)ctx->r[i + 1]);
        fb_text(fb, 8, 24 + (i / 2) * 10, b, 0xFFFF);
    }
    if (g_cpu) {
        snprintf(b, sizeof(b), "PS1 PC=%08lX EXC=%lu HLE=%lu", (unsigned long)g_cpu->pc,
                 (unsigned long)g_cpu->exc_count, (unsigned long)g_cpu->hle_calls);
        fb_text(fb, 8, 110, b, 0x07E0);
        for (int i = 1; i <= 6; i++) {
            snprintf(b, sizeof(b), "hist-%d %08lX", i,
                     (unsigned long)g_cpu->pc_history[(g_cpu->pc_history_pos - i) & (PC_HISTORY_SIZE - 1)]);
            fb_text(fb, 8, 120 + i * 10, b, 0x07E0);
        }
    }
    int lines = dlog_line_count();
    for (int i = 0; i < 24; i++) {
        if (i + 1 > lines) break;
        fb_text(fb, 8, fb_h - 10 - i * 10, dlog_line_from_end(i + 1), i == 0 ? 0xFFE0 : 0xE318);
    }
    for (;;) { }
}

/* ------------------------------------------------------------------ */
/* Interpreter micro-benchmark (synthetic MIPS loops in PS1 RAM)        */
/* ------------------------------------------------------------------ */

#define RUN_BENCH 0

__attribute__((unused)) static void run_bench(mips_t *cpu, mem_t *mem, gpu_t *gpu) {
    u32 *code = (u32 *)&mem->ram[0x100000];
    uint64_t t0, dt;

    /* ALU loop: addiu v0,v0,1 ; bne v0,a0,-2 ; nop ; j 0x80100000 ; nop */
    code[0] = 0x24420001; code[1] = 0x1444FFFE; code[2] = 0; code[3] = 0x08040000; code[4] = 0;
    cpu->r[2] = 0; cpu->r[4] = 0xFFFFFFFF; cpu->pc = 0x80100000;
    t0 = timer_us_gettime64();
    cpu_execute(cpu, mem, gpu, 1000000);
    dt = timer_us_gettime64() - t0;
    dlog("BENCH alu: 1M cyc in %lu us -> %lu ns/instr", (unsigned long)dt, (unsigned long)(dt * 1000 / 1000000));

    /* MEM loop: lw v1,0(a1) ; sw v1,4(a1) ; addiu v0,v0,1 ; bne v0,a0,-4 ; nop ; j ; nop */
    code[0] = 0x8CA30000; code[1] = 0xACA30004; code[2] = 0x24420001; code[3] = 0x1444FFFC;
    code[4] = 0; code[5] = 0x08040000; code[6] = 0;
    cpu->r[2] = 0; cpu->r[5] = 0x80180000; cpu->pc = 0x80100000;
    t0 = timer_us_gettime64();
    cpu_execute(cpu, mem, gpu, 1000000);
    dt = timer_us_gettime64() - t0;
    dlog("BENCH mem: 1M cyc in %lu us -> %lu ns/instr", (unsigned long)dt, (unsigned long)(dt * 1000 / 1000000));

    /* calibration: plain C loop of 1M iterations */
    volatile u32 acc = 0;
    t0 = timer_us_gettime64();
    for (u32 i = 0; i < 1000000; i++) acc += i;
    dt = timer_us_gettime64() - t0;
    dlog("BENCH c-loop: 1M iter in %lu us", (unsigned long)dt);

    memset(code, 0, 64);
    cpu_reset(cpu);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    vid_set_mode(DM_640x480, PM_RGB565);
    display_init();
    dbgio_init();
    dbgio_dev_select("scif");

    /* mask SH-4 FPU exceptions (GTE float path) */
    uint32_t fpscr;
    __asm__ __volatile__("sts fpscr, %0" : "=r"(fpscr));
    fpscr &= ~0x0003F000;
    __asm__ __volatile__("lds %0, fpscr" : : "r"(fpscr));

    vid_clear(0, 0, 0);
    irq_set_handler(EXC_UNHANDLED_EXC, sh4_exception_trap, NULL);

    dlog("BLEEM2 core build %s %s", __DATE__, __TIME__);
    thd_create(1, watchdog_thread, NULL);
    memcard_init();
    main_thd = thd_get_current();
#if PROFILER
    thd_create(1, sampler_thread, NULL);
#endif
    g_dlog_serial = !RELEASE_BUILD;

    mem_t *mem = mem_init();
    if (!mem) { dlog("FATAL: mem_init"); return -1; }
    gpu_t *gpu = (gpu_t *)memalign(32, sizeof(gpu_t));
    if (!gpu) { dlog("FATAL: gpu alloc"); return -1; }
    static mips_t cpu_instance __attribute__((aligned(32)));
    mips_t *cpu = &cpu_instance;
    memset(cpu, 0, sizeof(mips_t));
    cpu->ram_base = mem->ram;
    g_cpu = cpu; g_mem = mem; g_gpu = gpu;

    gpu_init(gpu);
    cd_init();
    cpu_reset(cpu);
    ps1_spu_init();
#if USE_JIT
    jit_init();
    { extern void jit_selftest(mips_t *, mem_t *, gpu_t *); jit_selftest(cpu, mem, gpu); }
#endif

    /* disc image */
    if (cd_init_bin("METAL_SL.BIN") < 0) dlog("CD: no disc image found");

    /* BIOS image */
    static const char *bios_paths[] = {
        "/cd/OPENBIOS.BIN", "/cd/SCPH1000.BIN",
        "/cd/psbios/OPENBIOS.BIN", "/cd/psbios/SCPH1000.BIN",
        "/pc/psbios/openbios.bin", "/pc/psbios/scph1000.bin"
    };
    int bios_ok = 0;
    for (unsigned i = 0; i < sizeof(bios_paths) / sizeof(bios_paths[0]); i++) {
        if (mem_load_bios(mem, bios_paths[i]) == 0) { dlog("BIOS: %s", bios_paths[i]); bios_ok = 1; break; }
    }
    if (!bios_ok) dlog("BIOS: none found");

#if RUN_BENCH
    run_bench(cpu, mem, gpu);
#endif

    int autoboot = BOOT_AUTOBOOT || !bios_ok;
    if (autoboot) {
        cpu->hle_enabled = 1;
        u32 entry = cd_load_exe_to_ram(cpu, mem);
        if (entry) cpu->pc = entry;
        else { dlog("FATAL: nothing to run"); cpu->stop = 1; }
    } else {
        cpu->hle_enabled = 0;
        cpu->pc = 0xBFC00000;
    }
    dlog("boot: %s pc=%08lX", autoboot ? "AUTOBOOT/HLE" : "BIOS", (unsigned long)cpu->pc);
    dlog_flush_vmu();

    u32 frames = 0;
    int stopped_logged = 0;
    uint64_t last_ms = timer_ms_gettime64();
    uint64_t frame_ms = 0;
    uint64_t win_emu = 0, win_conv = 0, win_total_us = 0, win_wall0 = timer_ms_gettime64();
    uint64_t disp_end_ms = 0;
    int fps = 0, fps_count = 0;
    uint64_t fps_t0 = last_ms;
    char buf[96];

    while (1) {
        loop_stage = 1; loop_frames = frames;
        uint64_t tf0 = timer_us_gettime64();
        input_poll();
        memcard_poll();
#if AUTOPLAY
        {   /* frame-based input script: deterministic test runs */
            static const struct { u32 f0, f1; u16 mask; } script[] = {
                { 600, 620, 1u << 3 }, { 1050, 1070, 1u << 3 }, { 1500, 1520, 1u << 14 },
                { 2400, 2550, 1u << 5 }, { 2500, 2510, 1u << 14 }, { 2560, 2570, 1u << 13 },
                { 2700, 2850, 1u << 5 }, { 2800, 2810, 1u << 14 }, { 3000, 3200, 1u << 5 }, { 3100, 3110, 1u << 14 },
                { 3400, 3600, 1u << 5 }, { 3500, 3510, 1u << 14 }, { 3650, 3660, 1u << 13 },
            };
            u16 m = 0;
            for (unsigned i = 0; i < sizeof(script) / sizeof(script[0]); i++) if (frames >= script[i].f0 && frames < script[i].f1) m |= script[i].mask;
            if (frames >= 3700) {               /* keep playing: advance right, shoot, jump now and then */
                u32 ph = (frames - 3700) % 180;
                if (ph < 120) m |= 1u << 5;                      /* Right */
                if ((ph % 12) < 5) m |= 1u << 13;                /* Circle: shoot */
                if (ph >= 60 && ph < 66) m |= 1u << 14;          /* Cross: jump */
                if (ph >= 150 && ph < 170) m |= 1u << 7;         /* Left: back off */
                if ((frames / 180) % 7 == 3 && ph >= 120 && ph < 140) m |= 1u << 6;   /* Down: crouch */
                if ((frames / 180) % 11 == 5 && ph >= 100 && ph < 110) m |= 1u << 12; /* Triangle: grenade */
            }
            g_pad_state &= (u16)~m;
        }
#endif
        if (g_pad_state != cpu->pad_state) dlog("PAD %04lX f=%lu", (unsigned long)g_pad_state, (unsigned long)frames);
        cpu->pad_state = g_pad_state;

        uint64_t t0 = timer_ms_gettime64();
        cpu_execute(cpu, mem, gpu, CPU_CYCLES_PER_FRAME);
        loop_stage = 2;
        uint64_t t1 = timer_ms_gettime64();
        frame_ms = t1 - t0;
        win_emu += frame_ms;
        { extern uint64_t g_mdec_win_us; static uint64_t last = 0; static u32 mx = 0;   /* worst MDEC frame in the window */
          u32 d = (u32)(g_mdec_win_us - last); last = g_mdec_win_us; if (d > mx) mx = d;
          if (frames % 300 == 299) { extern uint64_t g_xa_win_us; extern u32 g_mdec_cmds_win, g_xa_sectors;
            char ml[128]; snprintf(ml, sizeof(ml), "MDX f=%lu mdec_avg=%lu.%lums mdec_max=%lu.%lums frames=%lu xa_avg=%luus xa=%lu",
              (unsigned long)frames + 1, (unsigned long)(g_mdec_win_us / 300000), (unsigned long)((g_mdec_win_us / 30000) % 10),
              (unsigned long)(mx / 1000), (unsigned long)((mx / 100) % 10), (unsigned long)g_mdec_cmds_win,
              (unsigned long)(g_xa_win_us / 300), (unsigned long)g_xa_sectors);
            extern void dlog_win(const char *); dlog_win(ml);
            g_mdec_win_us = 0; last = 0; mx = 0; g_xa_win_us = 0; g_mdec_cmds_win = 0; } }

        /* vblank */
        cpu->i_stat |= 1;
        cpu->vblank_count++;
        gpu_vblank(gpu);
        if (cpu->hle_enabled) hle_vblank(cpu, mem);
        loop_stage = 7;
        ps1_spu_poll();
        loop_stage = 8;

        uint64_t tb0 = timer_ms_gettime64();
        loop_stage = 3;
        display_begin(gpu);
        loop_stage = 4;
        u16 *hud = display_hud_buffer(&fb_stride, &fb_w, &fb_h);
        uint64_t blit_ms = timer_ms_gettime64() - tb0;
        win_conv += blit_ms;

#if MASTER_HUD
        fb_fill(hud, 0, 22, fb_w, 20, 0x0000);
        snprintf(buf, sizeof(buf), "PC:%08lX FR:%lu FPS:%d EMU:%lums EXC:%lu IRQ:%lu",
                 (unsigned long)cpu->pc, (unsigned long)frames, fps, (unsigned long)frame_ms,
                 (unsigned long)cpu->exc_count, (unsigned long)cpu->irq_count);
        fb_text(hud, 4, 24, buf, 0xFFFF);
        {
            extern uint64_t prof_gpu_us, prof_sched_us;
            snprintf(buf, sizeof(buf), "SR:%08lX IS:%04lX IM:%04lX DISP:%d MODE:%02lX POLY:%lu GPU:%lums SCH:%lums BLT:%lums %s",
                     (unsigned long)cpu->cop0[12], (unsigned long)cpu->i_stat, (unsigned long)cpu->i_mask,
                     gpu->disp_enabled, (unsigned long)gpu->disp_mode, (unsigned long)gpu->polys,
                     (unsigned long)(prof_gpu_us / 1000), (unsigned long)(prof_sched_us / 1000),
                     (unsigned long)blit_ms, cpu->stop ? "STOPPED" : "RUN");
            prof_gpu_us = 0; prof_sched_us = 0;
        }
        fb_text(hud, 4, 33, buf, 0x83FF);

        /* debug: whole VRAM (1024x512) subsampled 4x into the top-right corner */
        if (HUD_VRAM_OVERLAY && fb_w == 640) for (int y = 0; y < 128; y++) {
            const u16 *src = &gpu->vram[(y * 4) << 10];
            u16 *dst = &vram_s[(44 + y) * 640 + 384];
            for (int x = 0; x < 256; x++) {
                u16 p = src[x * 4];
                dst[x] = (u16)(((p & 0x1F) << 11) | ((p & 0x3E0) << 1) | ((p >> 10) & 0x1F));
            }
        }
        fb_fill(hud, 0, fb_h - 64, fb_w, 64, 0x0000);
        int lines = dlog_line_count();
        for (int i = 0; i < 7; i++) {
            if (i + 1 > lines) break;
            fb_text(hud, 4, fb_h - 62 + (6 - i) * 9, dlog_line_from_end(i + 1), i == 0 ? 0xFFE0 : 0xE318);
        }
#endif

        loop_stage = 5;
        uint64_t te0 = timer_ms_gettime64();
        display_end();
        loop_stage = 6;
        disp_end_ms = timer_ms_gettime64() - te0;
        thd_pass();
        win_total_us += timer_us_gettime64() - tf0;

        fps_count++;
        if (t1 - fps_t0 >= 1000) { fps = fps_count; fps_count = 0; fps_t0 = t1; }

        if (frames % 300 == 0 && frames > 0 && !cpu->stop) {
            uint64_t now = timer_ms_gettime64();
            { char wl[128]; snprintf(wl, sizeof(wl), "WIN f=%lu emu_avg=%lu.%lums conv_avg=%lu.%lums total_avg=%lu.%lums wall=%lums/300f", (unsigned long)frames,
                 (unsigned long)(win_emu / 300), (unsigned long)((win_emu * 10 / 300) % 10),
                 (unsigned long)(win_conv / 300), (unsigned long)((win_conv * 10 / 300) % 10),
                 (unsigned long)(win_total_us / 300000), (unsigned long)((win_total_us / 30000) % 10), (unsigned long)(now - win_wall0));
              dlog("%s", wl); extern void dlog_win(const char *); dlog_win(wl); }
            win_emu = 0; win_conv = 0; win_total_us = 0; win_wall0 = now;
        }
        if (frames > 2000 && (cpu->pc & 0x1FC00000) == 0x1FC00000 && !cpu->stop) {
            static int bios_frames = 0;
            if (++bios_frames == 30) {
                dlog("BIOS RETURN DETECTED at f=%lu pc=%08lX sr=%08lX cause=%08lX epc=%08lX badv=%08lX", (unsigned long)frames, (unsigned long)cpu->pc, (unsigned long)cpu->cop0[12], (unsigned long)cpu->cop0[13], (unsigned long)cpu->cop0[14], (unsigned long)cpu->cop0[8]);
                for (int i = 1; i <= 12; i++) dlog(" hist-%d %08lX", i, (unsigned long)cpu->pc_history[(cpu->pc_history_pos - i) & (PC_HISTORY_SIZE - 1)]);
                dlog(" last exc %lu at %08lX", (unsigned long)cpu->last_exc_code, (unsigned long)cpu->last_exc_pc);
                dlog_flush_vmu(); cpu->stop = 1;
            }
        }
        if (frames % 30 == 0 && !cpu->stop) {
            dlog("HB f=%lu pc=%08lX sr=%08lX is=%04lX im=%04lX exc=%lu irq=%lu fps=%d emu=%lums gs=%08lX",
                 (unsigned long)frames, (unsigned long)cpu->pc, (unsigned long)cpu->cop0[12],
                 (unsigned long)cpu->i_stat, (unsigned long)cpu->i_mask,
                 (unsigned long)cpu->exc_count, (unsigned long)cpu->irq_count, fps,
                 (unsigned long)frame_ms, (unsigned long)gpu_read_status(gpu));
            { char cs[160]; cd_stats(cs, sizeof(cs)); dlog("  %s", cs); }
            { extern uint64_t prof_spu_us, prof_jit_us, prof_gpu_us; extern u32 stat_interp_instr, stat_jit_calls;
              { extern uint64_t prof_dma_us, prof_skip_us, prof_lookup_us; dlog("  frame: emu=%lums conv=%lums pvr=%lums spu=%lums/30f dma=%lums/30f skip=%lums/30f jit=%lums/30f lookup=%lums/30f interp=%lu jitcalls=%lu vmu=%lums", (unsigned long)frame_ms, (unsigned long)blit_ms, (unsigned long)disp_end_ms, (unsigned long)(prof_spu_us / 1000), (unsigned long)(prof_dma_us / 1000), (unsigned long)(prof_skip_us / 1000), (unsigned long)(prof_jit_us / 1000), (unsigned long)(prof_lookup_us / 1000), (unsigned long)stat_interp_instr, (unsigned long)stat_jit_calls, (unsigned long)g_vmu_flush_ms); prof_dma_us = 0; prof_skip_us = 0; prof_lookup_us = 0; }
              g_dlog_serial = 0;           /* remaining heartbeat detail goes to the VMU log only */
              prof_spu_us = 0; prof_jit_us = 0; prof_gpu_us = 0; stat_interp_instr = 0; stat_jit_calls = 0; }
            dlog("  probe vram=%04X fb=%04X fbw=%d dispxy=%d,%d", gpu->vram[((gpu->disp_y + 120) & 511) * 1024 + ((gpu->disp_x + 160) & 1023)], vram_s[(fb_h / 2) * fb_w + fb_w / 2], fb_w, gpu->disp_x, gpu->disp_y);
            { extern u32 g_aica_underruns, g_aica_playing, g_aica_starts, g_aica_rvb; dlog("  aica playing=%lu starts=%lu underruns=%lu rvb=%08lX", (unsigned long)g_aica_playing, (unsigned long)g_aica_starts, (unsigned long)g_aica_underruns, (unsigned long)g_aica_rvb); g_aica_starts = 0; g_aica_underruns = 0; }
            dlog("  pad=%04lX spu=%06lX", (unsigned long)cpu->pad_state, (unsigned long)ps1_spu_get_active_channels());
#if USE_JIT
            { char js[160]; jit_stats(js, sizeof(js)); dlog("  %s jitcyc=%llu", js, (unsigned long long)jit_cycles_executed); }
#endif
            { extern u32 g_irq_hist[16], g_sio_first[256];
              dlog("  irq vbl=%lu cd=%lu dma=%lu t0=%lu t1=%lu t2=%lu sio=%lu spu=%lu | sio first 01=%lu 81=%lu", (unsigned long)g_irq_hist[0], (unsigned long)g_irq_hist[2], (unsigned long)g_irq_hist[3], (unsigned long)g_irq_hist[4], (unsigned long)g_irq_hist[5], (unsigned long)g_irq_hist[6], (unsigned long)g_irq_hist[7], (unsigned long)g_irq_hist[9], (unsigned long)g_sio_first[1], (unsigned long)g_sio_first[0x81]);
              memset(g_irq_hist, 0, sizeof(g_irq_hist)); memset(g_sio_first, 0, sizeof(g_sio_first)); }
            { extern u32 g_px_rect, g_px_tri, g_xfer_hw, g_px_semi, g_px_copy, g_px_fill; dlog("  px rect=%lu semi=%lu tri=%lu copy=%lu fill=%lu xferhw=%lu per 30f", (unsigned long)g_px_rect, (unsigned long)g_px_semi, (unsigned long)g_px_tri, (unsigned long)g_px_copy, (unsigned long)g_px_fill, (unsigned long)g_xfer_hw); g_px_rect = g_px_tri = g_xfer_hw = g_px_semi = g_px_copy = g_px_fill = 0; }
            { extern uint64_t prof_dma_ch_us[8], prof_dma_gp0_us; extern u32 prof_dma_count[8];
              dlog("  dma2=%lums(gp0 %lums, n=%lu) dma3=%lums(n=%lu) dma4=%lums(n=%lu) dma6=%lums(n=%lu) per 30f", (unsigned long)(prof_dma_ch_us[2] / 1000), (unsigned long)(prof_dma_gp0_us / 1000), (unsigned long)prof_dma_count[2], (unsigned long)(prof_dma_ch_us[3] / 1000), (unsigned long)prof_dma_count[3], (unsigned long)(prof_dma_ch_us[4] / 1000), (unsigned long)prof_dma_count[4], (unsigned long)(prof_dma_ch_us[6] / 1000), (unsigned long)prof_dma_count[6]);
              { extern u32 g_mdec_blocks, g_mdec_cmds, g_xa_sectors, g_xa_underruns; dlog("  mdec dma0=%lums(n=%lu) dma1=%lums(n=%lu) mb=%lu cmds=%lu xa=%lu xa_under=%lu per 30f", (unsigned long)(prof_dma_ch_us[0] / 1000), (unsigned long)prof_dma_count[0], (unsigned long)(prof_dma_ch_us[1] / 1000), (unsigned long)prof_dma_count[1], (unsigned long)g_mdec_blocks, (unsigned long)g_mdec_cmds, (unsigned long)g_xa_sectors, (unsigned long)g_xa_underruns); g_mdec_blocks = g_mdec_cmds = 0; }
              memset(prof_dma_ch_us, 0, sizeof(prof_dma_ch_us)); memset(prof_dma_count, 0, sizeof(prof_dma_count)); prof_dma_gp0_us = 0; }
            dlog("  gpu disp=%d,%d en=%d mode=%02lX draw=%d,%d-%d,%d off=%d,%d tpage=%03X mask=%d/%d polys=%lu",
                 gpu->disp_x, gpu->disp_y, gpu->disp_enabled, (unsigned long)gpu->disp_mode,
                 gpu->draw_x0, gpu->draw_y0, gpu->draw_x1, gpu->draw_y1, gpu->off_x, gpu->off_y,
                 gpu->tpage, gpu->mask_set, gpu->mask_check, (unsigned long)gpu->polys);
            dlog("  v59E8=%08lX v48B0=%08lX v48B4=%08lX v48B8=%08lX v48BC=%08lX",
                 (unsigned long)mem_read32(cpu, mem, gpu, 0x800959E8), (unsigned long)mem_read32(cpu, mem, gpu, 0x800948B0),
                 (unsigned long)mem_read32(cpu, mem, gpu, 0x800948B4), (unsigned long)mem_read32(cpu, mem, gpu, 0x800948B8),
                 (unsigned long)mem_read32(cpu, mem, gpu, 0x800948BC));
            if (frames >= 30 && frames % 60 == 0) {
                /* coarse profile: distinct sampled PCs outside the vsync wait */
                char line[100]; int n = 0; u32 seen[16]; int ns = 0;
                line[0] = 0;
                for (int i = 0; i < PC_SAMPLES && ns < 16; i++) {
                    u32 s = g_pc_samples[i] & ~0xFFu;
                    if (s >= 0x8006E500 && s < 0x8006E700) continue;
                    int dup = 0;
                    for (int k = 0; k < ns; k++) if (seen[k] == s) { dup = 1; break; }
                    if (dup) continue;
                    seen[ns++] = s;
                    n += snprintf(line + n, sizeof(line) - n, "%08lX ", (unsigned long)s);
                    if (n > 80) { dlog("  smp %s", line); n = 0; line[0] = 0; }
                }
                if (n) dlog("  smp %s", line);
            }
            if (frames >= 30 && frames % 120 == 0) {
                dlog("PROF-CHECK");
#if PROFILER
                prof_report();
#endif
                g_dlog_serial = 0;           /* verbose dumps go to the VMU log only */
                u32 p = cpu->pc & ~3u;
                for (int k = -24; k < 8; k += 4) {
                    u32 a = p + k * 4;
                    dlog("  pc%08lX: %08lX %08lX %08lX %08lX", (unsigned long)a,
                         (unsigned long)mem_read32(cpu, mem, gpu, a), (unsigned long)mem_read32(cpu, mem, gpu, a + 4),
                         (unsigned long)mem_read32(cpu, mem, gpu, a + 8), (unsigned long)mem_read32(cpu, mem, gpu, a + 12));
                }
                dlog("  code@%08lX: %08lX %08lX %08lX %08lX %08lX", (unsigned long)(p - 8),
                     (unsigned long)mem_read32(cpu, mem, gpu, p - 8), (unsigned long)mem_read32(cpu, mem, gpu, p - 4),
                     (unsigned long)mem_read32(cpu, mem, gpu, p), (unsigned long)mem_read32(cpu, mem, gpu, p + 4),
                     (unsigned long)mem_read32(cpu, mem, gpu, p + 8));
                dlog("  a0=%08lX a1=%08lX v0=%08lX v1=%08lX s0=%08lX ra=%08lX sp=%08lX", (unsigned long)cpu->r[4],
                     (unsigned long)cpu->r[5], (unsigned long)cpu->r[2], (unsigned long)cpu->r[3], (unsigned long)cpu->r[16],
                     (unsigned long)cpu->r[31], (unsigned long)cpu->r[29]);
                for (int t = 0; t < 3; t++) {
                    u32 c, m, tg;
                    timers_debug(t, &c, &m, &tg);
                    dlog("  tmr%d cnt=%04lX mode=%04lX tgt=%04lX", t, (unsigned long)c, (unsigned long)m, (unsigned long)tg);
                }
                dlog("  code+C: %08lX %08lX %08lX %08lX", (unsigned long)mem_read32(cpu, mem, gpu, p + 12),
                     (unsigned long)mem_read32(cpu, mem, gpu, p + 16), (unsigned long)mem_read32(cpu, mem, gpu, p + 20),
                     (unsigned long)mem_read32(cpu, mem, gpu, p + 24));
                {
                    u32 ra = cpu->r[31] & ~3u;
                    for (int k = -44; k < 8; k += 4) {
                        u32 a = ra + k * 4;
                        dlog("  %08lX: %08lX %08lX %08lX %08lX", (unsigned long)a,
                             (unsigned long)mem_read32(cpu, mem, gpu, a), (unsigned long)mem_read32(cpu, mem, gpu, a + 4),
                             (unsigned long)mem_read32(cpu, mem, gpu, a + 8), (unsigned long)mem_read32(cpu, mem, gpu, a + 12));
                    }
                    dlog("  s1=%08lX s2=%08lX s3=%08lX t0=%08lX t1=%08lX", (unsigned long)cpu->r[17],
                         (unsigned long)cpu->r[18], (unsigned long)cpu->r[19], (unsigned long)cpu->r[8], (unsigned long)cpu->r[9]);
                    u32 fp = cpu->r[16] & ~3u;   /* s0: function pointer called by the idle loop */
                    for (int k = 0; k < 16; k += 4) {
                        u32 a = fp + k * 4;
                        dlog("  fn %08lX: %08lX %08lX %08lX %08lX", (unsigned long)a,
                             (unsigned long)mem_read32(cpu, mem, gpu, a), (unsigned long)mem_read32(cpu, mem, gpu, a + 4),
                             (unsigned long)mem_read32(cpu, mem, gpu, a + 8), (unsigned long)mem_read32(cpu, mem, gpu, a + 12));
                    }
                }
            }
            g_dlog_serial = !RELEASE_BUILD;
            loop_stage = 9;
            if (VMU_LOG_FLUSH && frames % LOG_FLUSH_FRAMES == 0) { dlog_flush_vmu(); extern void jit_dump_flush(void); jit_dump_flush(); }   /* release: VMU writes only on a crash */
            loop_stage = 10;
        }
        if (cpu->stop && !stopped_logged) {
            stopped_logged = 1;
            dlog("CPU STOPPED pc=%08lX last exc %lu at %08lX", (unsigned long)cpu->pc,
                 (unsigned long)cpu->last_exc_code, (unsigned long)cpu->last_exc_pc);
            for (int i = 1; i <= 8; i++)
                dlog(" hist-%d %08lX", i, (unsigned long)cpu->pc_history[(cpu->pc_history_pos - i) & (PC_HISTORY_SIZE - 1)]);
            dlog_flush_vmu();
        }
        frames++;
    }
    return 0;
}
