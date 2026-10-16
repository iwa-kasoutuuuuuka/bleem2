#include <kos.h>
#include "mips.h"
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <stddef.h>

/*
 * bleem2 - jit2.c : basic-block MIPS -> SH-4 recompiler (step 4: block linking)
 *
 * Register conventions inside generated code (set up once by the driver
 * trampoline, see call_block()):
 *   R8  = cpu pointer            R9-R12 = cached MIPS registers (per block)
 *   R13 = scratch (next pc)      R14    = return address into the driver
 *   R0-R7 = temporaries (clobbered by the C helpers, which preserve R8-R14)
 *
 * Every block ends by decrementing cpu->budget; while budget > 0 it jumps
 * straight to the next block through a patchable literal (static branch
 * targets only).  Unresolved links, dynamic jumps (JR) and exhausted budgets
 * return to the driver with cpu->pc set; the driver then resolves the link.
 *
 * Self-modifying code: 4KB pages carry a generation counter bumped by RAM
 * writes; blocks re-verify their source checksum when the generation
 * changed.  Replaced/evicted blocks get their entry overwritten with a stub
 * so stale links stay harmless.
 *
 * All SH-4 encodings were verified against sh-elf-as (docs/JIT_PLAN.md).
 */

#define JIT_CODE_SIZE   (4 * 1024 * 1024)
#define JIT_HASH_BITS   16
#define JIT_HASH_SIZE   (1 << JIT_HASH_BITS)
#define JIT_MAX_INSTR   24
#define JIT_MAX_LITS    96
#define JIT_PAGES       (512 + 128)        /* 2MB RAM + 512KB BIOS, 4KB pages */
#define JIT_CHUNK       256                /* cycles per driver entry */
#define JIT_LINKING     1                  /* 0: return to the driver after every block (debug) */
#define JIT_EXTEND_BLOCKS (0 && !JIT_VERIFY)  /* keep compiling past conditional branches */
#define JIT_DUMP_PC 0x8006E5D4           /* dlog the SH-4 code generated for this block */
#define JIT_POLL_SKIP 1                  /* fast-forward detected polling loops */
#define JIT_FORWARD_NOCHECK 0            /* forward static exits skip the budget check */
static char jit_dump_buf[4096]; static int jit_dump_len = 0, jit_dump_written = 0;
void jit_dump_flush(void) {
    if (!jit_dump_len || jit_dump_written) return;
    file_t f = fs_open("/vmu/a1/BLEEMJIT", O_WRONLY | O_TRUNC);
    if (f == FILEHND_INVALID) return;
    fs_write(f, jit_dump_buf, jit_dump_len);
    fs_close(f);
    jit_dump_written = 1;
}

typedef void (*block_fn_t)(mips_t *cpu);

typedef struct {
    u32 pc;             /* MIPS start pc (0xFFFFFFFF = empty) */
    u32 len;            /* number of MIPS instructions covered */
    u32 checksum;       /* XOR of source words */
    u32 cycles;
    u32 gen;            /* page generation the checksum was verified against */
    int has_store;
    int self_loop;      /* block branches back to its own start and has no stores (idle loop) */
    u32 poll_addr;      /* constant RAM address polled by the branch condition (0 = none) */
    u32 poll_regs;      /* registers that must stay unchanged for the loop to be idle */
    int poll_logged;
    block_fn_t fn;
} jit_block_t;

static jit_block_t blocks[JIT_HASH_SIZE];
static u16 *code_base = NULL;
static u16 *code_ptr = NULL;
static u32 page_gen[JIT_PAGES];
static u8  page_has_code[32] __attribute__((aligned(32)));   /* 64KB RAM regions containing compiled blocks (store fast-path gate) */
uint64_t prof_skip_us = 0;
uint64_t prof_lookup_us = 0;
static u32 g_spu_tick = 0;
extern uint64_t g_spu_last_ms;
static u32 stat_skipped = 0;
static u32 idle_last_pc = 0, idle_last_hash = 0;
static jit_block_t *prev_owner = NULL;
static jit_block_t *poll_last_blk = NULL; static u32 poll_last_val = 0, poll_last_regs = 0; static int poll_streak = 0, poll_hits = 0;
static u32 stat_compiled = 0, stat_runs = 0, stat_fallbacks = 0, stat_flushes = 0, stat_smc = 0, stat_recheck = 0, stat_links = 0, stat_idle = 0;
static int sched_acc = 0;
volatile int g_jit_stage = 0;          /* watchdog: where jit_run is */
volatile u32 g_jit_last_fn = 0;
static u32 link_pool[131072] __attribute__((aligned(32)));   /* link targets live in data, not code */
static jit_block_t *link_owner[131072];                     /* block that owns each link slot (exit) */
static int link_pool_used = 0;
static inline u32 fetch(mips_t *cpu, mem_t *mem, gpu_t *gpu, u32 pc);

static inline int page_index(u32 paddr) {
    if (paddr < 0x00800000) return (paddr & 0x1FFFFF) >> 12;
    if (paddr >= 0x1FC00000 && paddr < 0x1FC80000) return 512 + ((paddr - 0x1FC00000) >> 12);
    return -1;
}

/* called on every RAM write that did not go through the inline store path */
void jit_invalidate(u32 addr, u32 len) {
    u32 p = addr & 0x1FFFFFFF;
    int first = page_index(p), last = page_index(p + (len ? len - 1 : 0));
    if (first < 0) return;
    if (last < first) last = first;
    for (int i = first; i <= last; i++) page_gen[i]++;
}

/* ------------------------------------------------------------------ */
/* memory helpers called from generated code (R4 = addr, R5 = value)   */
/* ------------------------------------------------------------------ */

static u32 stat_slow_rd = 0, stat_slow_wr = 0;
static u32 slow_hist[4096];                              /* slow-path accesses by 1MB region */
static u32 jit_rd32(u32 a) { stat_slow_rd++; slow_hist[a >> 20]++; return mem_read32(g_cpu, g_mem, g_gpu, a & ~3u); }
static u32 jit_rd16(u32 a) { return mem_read16(g_cpu, g_mem, g_gpu, a & ~1u); }
static u32 jit_rd8 (u32 a) { return mem_read8(g_cpu, g_mem, a); }
static void jit_wr32(u32 a, u32 v) { stat_slow_wr++; slow_hist[a >> 20]++; mem_write32(g_cpu, g_mem, g_gpu, a & ~3u, v); }
static void jit_wr16(u32 a, u32 v) { mem_write16(g_cpu, g_mem, g_gpu, a & ~1u, (u16)v); }
static void jit_wr8 (u32 a, u32 v) { mem_write8(g_cpu, g_mem, a, (u8)v); }

static void jit_div(u32 n_, u32 d_) {
    s32 n = (s32)n_, d = (s32)d_;
    if (d == 0) { g_cpu->lo = (n >= 0) ? 0xFFFFFFFF : 1; g_cpu->hi = (u32)n; }
    else if ((u32)n == 0x80000000 && d == -1) { g_cpu->lo = 0x80000000; g_cpu->hi = 0; }
    else { g_cpu->lo = (u32)(n / d); g_cpu->hi = (u32)(n % d); }
}
static void jit_divu(u32 n, u32 d) {
    if (d == 0) { g_cpu->lo = 0xFFFFFFFF; g_cpu->hi = n; }
    else { g_cpu->lo = n / d; g_cpu->hi = n % d; }
}
static const u32 LWL_MASK[4]  = { 0x00FFFFFF, 0x0000FFFF, 0x000000FF, 0x00000000 };
static const int LWL_SHIFT[4] = { 24, 16, 8, 0 };
static const u32 LWR_MASK[4]  = { 0x00000000, 0xFF000000, 0xFFFF0000, 0xFFFFFF00 };
static const int LWR_SHIFT[4] = { 0, 8, 16, 24 };
static const u32 SWL_MASK[4]  = { 0xFFFFFF00, 0xFFFF0000, 0xFF000000, 0x00000000 };
static const int SWL_SHIFT[4] = { 24, 16, 8, 0 };
static const u32 SWR_MASK[4]  = { 0x00000000, 0x000000FF, 0x0000FFFF, 0x00FFFFFF };
static const int SWR_SHIFT[4] = { 0, 8, 16, 24 };
static u32 jit_lwl(u32 a, u32 old) { u32 m = mem_read32(g_cpu, g_mem, g_gpu, a & ~3u); int s = a & 3; return (old & LWL_MASK[s]) | (m << LWL_SHIFT[s]); }
static u32 jit_lwr(u32 a, u32 old) { u32 m = mem_read32(g_cpu, g_mem, g_gpu, a & ~3u); int s = a & 3; return (old & LWR_MASK[s]) | (m >> LWR_SHIFT[s]); }
static void jit_swl(u32 a, u32 v) { u32 al = a & ~3u; int s = a & 3; u32 m = mem_read32(g_cpu, g_mem, g_gpu, al); mem_write32(g_cpu, g_mem, g_gpu, al, (m & SWL_MASK[s]) | (v >> SWL_SHIFT[s])); }
static void jit_swr(u32 a, u32 v) { u32 al = a & ~3u; int s = a & 3; u32 m = mem_read32(g_cpu, g_mem, g_gpu, al); mem_write32(g_cpu, g_mem, g_gpu, al, (m & SWR_MASK[s]) | (v << SWR_SHIFT[s])); }
static void jit_mtc0(u32 rd, u32 v) {
    if (rd == 13) g_cpu->cop0[13] = (g_cpu->cop0[13] & ~0x300u) | (v & 0x300);
    else if (rd != 15 && rd != 8) g_cpu->cop0[rd] = v;
}
static void jit_gte(u32 instr, u32 unused) { (void)unused; gte_execute(g_cpu, instr); }

#define OFF_R(i)       ((i) * 4)
#define OFF_PC         ((u32)offsetof(mips_t, pc))
#define OFF_HI         ((u32)offsetof(mips_t, hi))
#define OFF_LO         ((u32)offsetof(mips_t, lo))
#define OFF_GTE_DR(i)  ((u32)offsetof(mips_t, gte_dr) + (i) * 4)
#define OFF_GTE_CR(i)  ((u32)offsetof(mips_t, gte_cr) + (i) * 4)
#define OFF_COP0(i)    ((u32)offsetof(mips_t, cop0) + (i) * 4)
#define OFF_BUDGET     ((u32)offsetof(mips_t, budget))
#define OFF_LINK       ((u32)offsetof(mips_t, link_site))

/* fallback histogram: first distinct unsupported instruction kinds */
static u32 fb_words[24];
static int fb_count = 0;
static void note_fallback(u32 instr) {
    u32 key = (instr >> 26) ? (instr & 0xFC000000) : (instr & 0xFC00003F);
    for (int i = 0; i < fb_count; i++) if (fb_words[i] == key) return;
    if (fb_count < 24) fb_words[fb_count++] = key;
}

/* ------------------------------------------------------------------ */
/* SH-4 emitter                                                        */
/* ------------------------------------------------------------------ */

typedef struct { u16 *bf, *bf2, *done; int is_store, width; } slow_t;

typedef struct {
    u16 *start;
    u16 *p;
    struct { u16 *at; u32 val; } lits[JIT_MAX_LITS];
    int nlits;
    int overflow;
    u32 block_pc;       /* MIPS pc of the block being compiled (forward/backward exits) */
    int link_first;     /* first link slot allocated by this compile */
    int isc;            /* block compiled for SR.IsC=1: stores are dropped, never linked */
    int end_block;      /* instruction terminates the block (MTC0 SR) */
    int no_link;        /* exit must return to the driver (no block linking) */
    slow_t slow[48];    /* deferred slow paths */
    int nslow;
    u16 *slow_first;    /* oldest pending BF site */
    int map[32];        /* MIPS reg -> cached SH-4 reg (9..12) or 0 */
    int dirty[32];
    int ncached;
    int cached[4];
    int first_write[32];/* cached reg is written before any read: skip the entry load */
    /* link literals: index into lits[] whose final address is a link site */
    int link_lit[2];
    int nlinks;
    int tag;            /* profiler category of the code being emitted */
} emit_t;

/* profiler: category per 8-byte granule of generated code */
static u8 code_tag[JIT_CODE_SIZE / 8];
enum { TAG_NONE, TAG_ENTRY, TAG_LOAD, TAG_STORE, TAG_EXIT, TAG_CALL, TAG_BRANCH, TAG_ALU };
int jit_code_tag(u32 pc) {
    if (!code_base) return -1;
    u32 off = pc - (u32)(uintptr_t)code_base;
    if (off >= JIT_CODE_SIZE) return -1;
    return code_tag[off >> 3];
}

static inline void E(emit_t *e, u16 op) { code_tag[((uintptr_t)e->p - (uintptr_t)code_base) >> 3] = (u8)e->tag; *e->p++ = op; }

#define SH_MOV_RR(m, n)        (0x6003 | ((n) << 8) | ((m) << 4))     /* MOV Rm,Rn          */
#define SH_MOV_IMM(i, n)       (0xE000 | ((n) << 8) | ((i) & 0xFF))   /* MOV #imm,Rn        */
#define SH_MOVL_DISP_LD(d, m, n) (0x5000 | ((n) << 8) | ((m) << 4) | ((d) & 0xF)) /* MOV.L @(d*4,Rm),Rn */
#define SH_MOVL_DISP_ST(m, d, n) (0x1000 | ((n) << 8) | ((m) << 4) | ((d) & 0xF)) /* MOV.L Rm,@(d*4,Rn) */
#define SH_MOVL_R0_LD(m, n)    (0x000E | ((n) << 8) | ((m) << 4))     /* MOV.L @(R0,Rm),Rn  */
#define SH_MOVL_R0_ST(m, n)    (0x0006 | ((n) << 8) | ((m) << 4))     /* MOV.L Rm,@(R0,Rn)  */
#define SH_MOVL_LD(m, n)       (0x6002 | ((n) << 8) | ((m) << 4))     /* MOV.L @Rm,Rn       */
#define SH_MOVW_LD(m, n)       (0x6001 | ((n) << 8) | ((m) << 4))     /* MOV.W @Rm,Rn (sx)  */
#define SH_MOVB_LD(m, n)       (0x6000 | ((n) << 8) | ((m) << 4))     /* MOV.B @Rm,Rn (sx)  */
#define SH_MOVB_R0_LD(m, n)    (0x000C | ((n) << 8) | ((m) << 4))     /* MOV.B @(R0,Rm),Rn  */
#define SH_MOVL_ST(m, n)       (0x2002 | ((n) << 8) | ((m) << 4))     /* MOV.L Rm,@Rn       */
#define SH_MOVW_ST(m, n)       (0x2001 | ((n) << 8) | ((m) << 4))     /* MOV.W Rm,@Rn       */
#define SH_MOVB_ST(m, n)       (0x2000 | ((n) << 8) | ((m) << 4))     /* MOV.B Rm,@Rn       */
#define SH_ADD_RR(m, n)        (0x300C | ((n) << 8) | ((m) << 4))
#define SH_ADD_IMM(i, n)       (0x7000 | ((n) << 8) | ((i) & 0xFF))
#define SH_SUB_RR(m, n)        (0x3008 | ((n) << 8) | ((m) << 4))
#define SH_AND_RR(m, n)        (0x2009 | ((n) << 8) | ((m) << 4))
#define SH_OR_RR(m, n)         (0x200B | ((n) << 8) | ((m) << 4))
#define SH_XOR_RR(m, n)        (0x200A | ((n) << 8) | ((m) << 4))
#define SH_NOT_RR(m, n)        (0x6007 | ((n) << 8) | ((m) << 4))
#define SH_NEG(m, n)           (0x600B | ((n) << 8) | ((m) << 4))
#define SH_SHLD(m, n)          (0x400D | ((n) << 8) | ((m) << 4))
#define SH_SHAD(m, n)          (0x400C | ((n) << 8) | ((m) << 4))
#define SH_SHLR8(n)            (0x4019 | ((n) << 8))
#define SH_SHLR2(n)            (0x4009 | ((n) << 8))
#define SH_SHLL2(n)            (0x4008 | ((n) << 8))
#define SH_CMP_EQ(m, n)        (0x3000 | ((n) << 8) | ((m) << 4))     /* T = Rn == Rm       */
#define SH_CMP_HS(m, n)        (0x3002 | ((n) << 8) | ((m) << 4))     /* T = Rn >= Rm (u)   */
#define SH_CMP_GE(m, n)        (0x3003 | ((n) << 8) | ((m) << 4))     /* T = Rn >= Rm (s)   */
#define SH_CMP_HI(m, n)        (0x3006 | ((n) << 8) | ((m) << 4))     /* T = Rn > Rm (u)    */
#define SH_CMP_GT(m, n)        (0x3007 | ((n) << 8) | ((m) << 4))     /* T = Rn > Rm (s)    */
#define SH_CMP_PZ(n)           (0x4011 | ((n) << 8))                  /* T = Rn >= 0        */
#define SH_CMP_PL(n)           (0x4015 | ((n) << 8))                  /* T = Rn > 0         */
#define SH_TST_RR(m, n)        (0x2008 | ((n) << 8) | ((m) << 4))     /* T = (Rn & Rm) == 0 */
#define SH_MOVT(n)             (0x0029 | ((n) << 8))
#define SH_EXTSB(m, n)         (0x600E | ((n) << 8) | ((m) << 4))
#define SH_EXTSW(m, n)         (0x600F | ((n) << 8) | ((m) << 4))
#define SH_EXTUB(m, n)         (0x600C | ((n) << 8) | ((m) << 4))
#define SH_EXTUW(m, n)         (0x600D | ((n) << 8) | ((m) << 4))
#define SH_DMULSL(m, n)        (0x300D | ((n) << 8) | ((m) << 4))
#define SH_DMULUL(m, n)        (0x3005 | ((n) << 8) | ((m) << 4))
#define SH_STS_MACH(n)         (0x000A | ((n) << 8))
#define SH_STS_MACL(n)         (0x001A | ((n) << 8))
#define SH_JSR(n)              (0x400B | ((n) << 8))
#define SH_JMP(n)              (0x402B | ((n) << 8))
#define SH_BT(d)               (0x8900 | ((d) & 0xFF))
#define SH_BF(d)               (0x8B00 | ((d) & 0xFF))
#define SH_BRA(d)              (0xA000 | ((d) & 0xFFF))
#define SH_NOP                 0x0009
#define SH_RTS                 0x000B
#define SH_STSL_PR             0x4F22
#define SH_LDSL_PR             0x4F26
#define SH_PUSH(m)             (0x2F06 | ((m) << 4))                  /* MOV.L Rm,@-R15     */
#define SH_POP(n)              (0x60F6 | ((n) << 8))                  /* MOV.L @R15+,Rn     */

static void patch_bt(u16 *at, u16 *target) { int d = ((int)(target - at) - 2); *at = (*at & 0xFF00) | (d & 0xFF); }
static void patch_bra(u16 *at, u16 *target) { int d = ((int)(target - at) - 2); *at = (*at & 0xF000) | (d & 0xFFF); }

/* load 32-bit constant into Rn (MOV #imm when it fits, else literal pool); returns lit index or -1 */
static int emit_imm32_lit(emit_t *e, int n, u32 val, int force_lit) {
    if (!force_lit && (s32)val >= -128 && (s32)val <= 127) { E(e, SH_MOV_IMM(val, n)); return -1; }
    if (e->nlits >= JIT_MAX_LITS) { e->overflow = 1; E(e, SH_NOP); return -1; }
    int idx = e->nlits;
    e->lits[idx].at = e->p;
    e->lits[idx].val = val;
    e->nlits++;
    E(e, 0xD000 | (n << 8));            /* MOV.L @(disp,PC),Rn : patched later */
    return idx;
}
static void emit_imm32(emit_t *e, int n, u32 val) { emit_imm32_lit(e, n, val, 0); }

static void emit_r0_const(emit_t *e, u32 v) {
    if (v <= 127) { E(e, SH_MOV_IMM(v, 0)); }
    else if (v <= 254) { E(e, SH_MOV_IMM(127, 0)); E(e, SH_ADD_IMM(v - 127, 0)); }
    else emit_imm32(e, 0, v);
}

static void emit_ld_reg_mem(emit_t *e, int mreg, int n) {
    if (mreg == 0) { E(e, SH_MOV_IMM(0, n)); return; }
    if (mreg < 16) { E(e, SH_MOVL_DISP_LD(mreg, 8, n)); return; }
    emit_r0_const(e, OFF_R(mreg));
    E(e, SH_MOVL_R0_LD(8, n));
}
static void emit_st_reg_mem(emit_t *e, int m, int mreg) {
    if (mreg == 0) return;
    if (mreg < 16) { E(e, SH_MOVL_DISP_ST(m, mreg, 8)); return; }
    emit_r0_const(e, OFF_R(mreg));
    E(e, SH_MOVL_R0_ST(m, 8));
}
static void emit_ld_reg(emit_t *e, int mreg, int n) {
    int c = e->map[mreg];
    if (c) { if (c != n) E(e, SH_MOV_RR(c, n)); return; }
    emit_ld_reg_mem(e, mreg, n);
}
static void emit_st_reg(emit_t *e, int m, int mreg) {
    if (mreg == 0) return;
    int c = e->map[mreg];
    if (c) { if (c != m) E(e, SH_MOV_RR(m, c)); e->dirty[mreg] = 1; return; }
    emit_st_reg_mem(e, m, mreg);
}
static void emit_st_off(emit_t *e, int m, u32 off) { emit_r0_const(e, off); E(e, SH_MOVL_R0_ST(m, 8)); }
static void emit_ld_off(emit_t *e, u32 off, int n) { emit_r0_const(e, off); E(e, SH_MOVL_R0_LD(8, n)); }

/* register-targeted helpers: operate directly on cached SH-4 registers when possible */
/* returns a register holding MIPS reg (cached reg, or loads into tmp) */
static int src_reg(emit_t *e, int mreg, int tmp) {
    int c = e->map[mreg];
    if (c) return c;
    emit_ld_reg_mem(e, mreg, tmp);
    return tmp;
}
/* destination register for MIPS reg: cached reg, or tmp (committed afterwards) */
static int dst_reg(emit_t *e, int mreg, int tmp) {
    int c = e->map[mreg];
    return c ? c : tmp;
}
static void commit_reg(emit_t *e, int sh, int mreg) {
    if (mreg == 0) return;
    int c = e->map[mreg];
    if (c) { if (c != sh) E(e, SH_MOV_RR(sh, c)); e->dirty[mreg] = 1; return; }
    emit_st_reg_mem(e, sh, mreg);
}
/* rd = rs OP rt for two-operand SH ops (opbase | n<<8 | m<<4 computes Rn = Rn OP Rm) */
static void emit_alu_rr(emit_t *e, u16 opbase, int rs, int rt, int rd, int commutative) {
    if (rd == 0) return;
    int d = dst_reg(e, rd, 1);
    int s = src_reg(e, rs, 1);
    int t = src_reg(e, rt, (s == 1) ? 2 : 1);
    if (d == s) { E(e, opbase | (d << 8) | (t << 4)); }
    else if (d == t && commutative) { E(e, opbase | (d << 8) | (s << 4)); }
    else if (d == t) { E(e, SH_MOV_RR(s, 3)); E(e, opbase | (3 << 8) | (t << 4)); E(e, SH_MOV_RR(3, d)); }
    else { E(e, SH_MOV_RR(s, d)); E(e, opbase | (d << 8) | (t << 4)); }
    commit_reg(e, d, rd);
}
/* rt = rs OP imm (imm materialised in R2 unless the op has an immediate form) */
static void emit_alu_ri(emit_t *e, u16 opbase, int rs, int rt, u32 imm) {
    if (rt == 0) return;
    int d = dst_reg(e, rt, 1);
    int s = src_reg(e, rs, 1);
    if (d != s) E(e, SH_MOV_RR(s, d));
    if (opbase == SH_ADD_RR(0, 0) && (s32)imm >= -128 && (s32)imm <= 127) {
        if (imm) E(e, SH_ADD_IMM(imm, d));
    } else {
        emit_imm32(e, 2, imm);
        E(e, opbase | (d << 8) | (2 << 4));
    }
    commit_reg(e, d, rt);
}
/* rd = rt shifted by sa (constant) */
static void emit_shift_imm(emit_t *e, int kind, int rt, int rd, int sa) {   /* kind: 0 = SLL, 1 = SRL, 2 = SRA */
    if (rd == 0) return;
    int d = dst_reg(e, rd, 1);
    int s = src_reg(e, rt, 1);
    if (d != s) E(e, SH_MOV_RR(s, d));
    if (sa) {
        if (kind == 0 && (sa == 1 || sa == 2 || sa == 8 || sa == 16)) {
            E(e, 0x4000 | (d << 8) | (sa == 1 ? 0x00 : sa == 2 ? 0x08 : sa == 8 ? 0x18 : 0x28));      /* SHLL/SHLL2/8/16 */
        } else if (kind == 1 && (sa == 1 || sa == 2 || sa == 8 || sa == 16)) {
            E(e, 0x4001 | (d << 8) | (sa == 1 ? 0x00 : sa == 2 ? 0x08 : sa == 8 ? 0x18 : 0x28));      /* SHLR/SHLR2/8/16 */
        } else if (kind == 2 && sa == 1) {
            E(e, 0x4021 | (d << 8));                                                                   /* SHAR */
        } else {
            E(e, SH_MOV_IMM(kind == 0 ? sa : -sa, 2));
            E(e, (kind == 2 ? SH_SHAD(2, d) : SH_SHLD(2, d)));
        }
    }
    commit_reg(e, d, rd);
}
/* rd = rt shifted by (rs & 31) */
static void emit_shift_var(emit_t *e, int kind, int rs, int rt, int rd) {
    if (rd == 0) return;
    int d = dst_reg(e, rd, 1);
    int t = src_reg(e, rt, 1);
    int s = src_reg(e, rs, 3);
    E(e, SH_MOV_IMM(31, 0)); E(e, SH_MOV_RR(s, 2)); E(e, SH_AND_RR(0, 2));
    if (kind != 0) E(e, SH_NEG(2, 2));
    if (d != t) E(e, SH_MOV_RR(t, d));
    E(e, kind == 2 ? SH_SHAD(2, d) : SH_SHLD(2, d));
    commit_reg(e, d, rd);
}
/* rd = (rs < rt) signed/unsigned, or vs immediate */
static void emit_slt(emit_t *e, int rs, int rt, int rd, int is_unsigned, int use_imm, u32 imm) {
    if (rd == 0) return;
    int s = src_reg(e, rs, 1);
    int t;
    if (use_imm) { emit_imm32(e, 2, imm); t = 2; }
    else t = src_reg(e, rt, (s == 1) ? 2 : 1);
    E(e, is_unsigned ? SH_CMP_HI(s, t) : SH_CMP_GT(s, t));           /* T = t > s  <=>  s < t */
    int d = dst_reg(e, rd, 1);
    E(e, SH_MOVT(d));
    commit_reg(e, d, rd);
}

/* call helper(R4, R5) -> R0 (helpers preserve R8-R14) */
static void emit_call(emit_t *e, void *fn) {
    int t0 = e->tag; e->tag = TAG_CALL;
    E(e, SH_PUSH(7));
    emit_imm32(e, 3, (u32)fn);
    E(e, SH_JSR(3));
    E(e, SH_NOP);
    E(e, SH_POP(7));
    e->tag = t0;
}

/* block entry: load cached registers */
static void emit_entry(emit_t *e) {
    e->tag = TAG_ENTRY;
    for (int i = 0; i < e->ncached; i++) if (!e->first_write[e->cached[i]]) emit_ld_reg_mem(e, e->cached[i], 9 + i);
}
/* write back dirty cached registers */
static void emit_writeback(emit_t *e) {
    int t0 = e->tag; e->tag = TAG_ENTRY;
    for (int i = 0; i < e->ncached; i++)
        if (e->dirty[e->cached[i]]) emit_st_reg_mem(e, 9 + i, e->cached[i]);
    e->tag = t0;
}

/* lay out the literal pool after the code and patch the MOV.L @(disp,PC) */
static int emit_finish(emit_t *e, u32 **lit_addr_out) {
    if (((uintptr_t)e->p) & 2) E(e, SH_NOP);        /* align 4 */
    u32 *placed[JIT_MAX_LITS];
    for (int i = 0; i < e->nlits; i++) {
        u16 *at = e->lits[i].at;
        uintptr_t base = ((uintptr_t)at + 4) & ~3u;
        u32 *lit = NULL;
        for (int k = 0; k < i; k++) {
            if (e->lits[k].val == e->lits[i].val && placed[k] && (uintptr_t)placed[k] >= base &&
                ((uintptr_t)placed[k] - base) / 4 <= 255) { lit = placed[k]; break; }
        }
        if (!lit) {
            lit = (u32 *)e->p;
            *lit = e->lits[i].val;
            e->p += 2;
        }
        uintptr_t disp = ((uintptr_t)lit - base) / 4;
        if (disp > 255) return -1;
        *at |= (u16)disp;
        placed[i] = lit;
        if (lit_addr_out) lit_addr_out[i] = lit;
    }
    return 0;
}

/* ---- deferred slow paths (emitted out of line so the fast path falls through) ---- */
static void emit_slow_paths(emit_t *e) {
    int t0 = e->tag;
    for (int i = 0; i < e->nslow; i++) {
        slow_t *s = &e->slow[i];
        u16 *here = e->p;
        if ((here - s->bf) > 128 || (s->bf2 && (here - s->bf2) > 128)) e->overflow = 1;
        patch_bt(s->bf, here);
        if (s->bf2) patch_bt(s->bf2, here);
        e->tag = s->is_store ? TAG_STORE : TAG_LOAD;
        /* scratchpad (0x1F800000, 1KB) inline before the helper call */
        {
            extern u8 g_scratchpad[1024];
            u32 amask = (s->width == 4) ? 0x3FC : (s->width == 2) ? 0x3FE : 0x3FF;
            emit_imm32(e, 2, 0x1FFFFC00); E(e, SH_MOV_RR(4, 1)); E(e, SH_AND_RR(2, 1));
            emit_imm32(e, 2, 0x1F800000); E(e, SH_CMP_EQ(2, 1));
            u16 *nosp = e->p; E(e, SH_BF(0));
            emit_imm32(e, 1, amask); E(e, SH_AND_RR(4, 1));
            emit_imm32(e, 2, (u32)g_scratchpad); E(e, SH_ADD_RR(2, 1));
            if (s->is_store) E(e, (s->width == 4) ? SH_MOVL_ST(5, 1) : (s->width == 2) ? SH_MOVW_ST(5, 1) : SH_MOVB_ST(5, 1));
            else E(e, (s->width == 4) ? SH_MOVL_LD(1, 0) : (s->width == 2) ? SH_MOVW_LD(1, 0) : SH_MOVB_LD(1, 0));
            u16 *bra2 = e->p; E(e, SH_BRA(0)); E(e, SH_NOP);
            patch_bra(bra2, s->done);
            patch_bt(nosp, e->p);
        }
        if (s->is_store)
            emit_call(e, (s->width == 4) ? (void *)jit_wr32 : (s->width == 2) ? (void *)jit_wr16 : (void *)jit_wr8);
        else {
            emit_call(e, (s->width == 4) ? (void *)jit_rd32 : (s->width == 2) ? (void *)jit_rd16 : (void *)jit_rd8);
            if (s->width == 2) E(e, SH_EXTSW(0, 0));
            if (s->width == 1) E(e, SH_EXTSB(0, 0));
        }
        u16 *bra = e->p; E(e, SH_BRA(0)); E(e, SH_NOP);
        patch_bra(bra, s->done);
    }
    e->nslow = 0;
    e->slow_first = NULL;
    e->tag = t0;
}
/* flush pending slow paths in the middle of a block when the BF range is about to run out */
static void maybe_flush_slow(emit_t *e) {
    if (!e->nslow) return;
    if ((e->p - e->slow_first) * 2 < 128) return;
    u16 *skip = e->p; E(e, SH_BRA(0)); E(e, SH_NOP);
    emit_slow_paths(e);
    patch_bra(skip, e->p);
}
static void add_slow(emit_t *e, u16 *bf, u16 *bf2, u16 *done, int is_store, int width) {
    if (e->nslow >= (int)(sizeof(e->slow) / sizeof(e->slow[0]))) { e->overflow = 1; return; }
    slow_t *s = &e->slow[e->nslow++];
    s->bf = bf; s->bf2 = bf2; s->done = done; s->is_store = is_store; s->width = width;
    if (!e->slow_first) e->slow_first = bf;
}

/* Inline RAM load. R4 = address -> R0 = value (sign/zero extended).
 * RAM test: (addr & 0x1F800000) == 0 covers the 8MB mirror space of KUSEG/KSEG0/KSEG1;
 * mirrors and I/O take the slow path.  R13 = RAM base. */
static void emit_load(emit_t *e, int width, int sign) {
    int t0 = e->tag; e->tag = TAG_LOAD;
    maybe_flush_slow(e);
    emit_imm32(e, 2, 0x1F800000); E(e, SH_TST_RR(2, 4));
    u16 *bf = e->p; E(e, SH_BF(0));
    emit_imm32(e, 1, (width == 4) ? 0x1FFFFC : (width == 2) ? 0x1FFFFE : 0x1FFFFF);
    E(e, SH_AND_RR(4, 1));
    E(e, SH_ADD_RR(13, 1));
    E(e, (width == 4) ? SH_MOVL_LD(1, 0) : (width == 2) ? SH_MOVW_LD(1, 0) : SH_MOVB_LD(1, 0));
    u16 *done = e->p;
    if (!sign && width == 2) E(e, SH_EXTUW(0, 0));
    if (!sign && width == 1) E(e, SH_EXTUB(0, 0));
    add_slow(e, bf, NULL, done, 0, width);
    e->tag = t0;
}

/* Inline RAM store. R4 = address, R5 = value.
 * Blocks only run with SR.IsC clear (jit_run checks it, MTC0 SR ends a block).
 * Pages that contain compiled code take the slow path (the helper bumps page_gen). */
static void emit_store(emit_t *e, int width) {
    int t0 = e->tag; e->tag = TAG_STORE;
    maybe_flush_slow(e);
    emit_imm32(e, 2, 0x1F800000); E(e, SH_TST_RR(2, 4));
    u16 *bf = e->p; E(e, SH_BF(0));
    emit_imm32(e, 1, (width == 4) ? 0x1FFFFC : (width == 2) ? 0x1FFFFE : 0x1FFFFF);
    E(e, SH_AND_RR(4, 1));
    u16 *bf2 = NULL;                                                                 /* no code-page gate: SMC is caught by the lookup checksum */
    E(e, SH_ADD_RR(13, 1));
    E(e, (width == 4) ? SH_MOVL_ST(5, 1) : (width == 2) ? SH_MOVW_ST(5, 1) : SH_MOVB_ST(5, 1));
    u16 *done = e->p;
    add_slow(e, bf, bf2, done, 1, width);
    e->tag = t0;
}

/*
 * Driver return protocol: R1 = next pc, R2 = link slot address (0 = none),
 * R7 = remaining budget; JMP @R14 (the trampoline stores them into cpu).
 *
 * Block exit for a statically known next pc:
 *   budget (R7) -= cycles; if (budget > 0) jump through the link slot (initially
 *   pointing at this exit's slow path); else return to the driver.
 * With e->no_link the block always returns to the driver.
 */
static void emit_exit_static(emit_t *e, u32 next_pc, u32 cycles) {
    e->tag = TAG_EXIT;
    if (!e->no_link && next_pc > e->block_pc && JIT_FORWARD_NOCHECK) {
        /* forward transition: no budget check (a chain of forward jumps always terminates;
         * backward branches and dynamic exits still check), ADD in the JMP delay slot */
        if (link_pool_used >= (int)(sizeof(link_pool) / sizeof(link_pool[0]))) { e->overflow = 1; return; }
        u32 *slot = &link_pool[link_pool_used++];
        emit_imm32(e, 3, (u32)slot);
        E(e, SH_MOVL_LD(3, 3));
        E(e, SH_JMP(3));
        E(e, SH_ADD_IMM(-(int)cycles, 7));
        u16 *slow = e->p;
        *slot = (u32)(uintptr_t)slow;
        emit_imm32(e, 1, next_pc);
        emit_imm32(e, 2, (u32)slot);
        E(e, SH_JMP(14));
        E(e, SH_NOP);
        return;
    }
    if (e->no_link) {
        E(e, SH_ADD_IMM(-(int)cycles, 7));
        emit_imm32(e, 1, next_pc);
        E(e, SH_MOV_IMM(0, 2));
        E(e, SH_JMP(14));
        E(e, SH_NOP);
        return;
    }
    if (link_pool_used >= (int)(sizeof(link_pool) / sizeof(link_pool[0]))) { e->overflow = 1; return; }
    u32 *slot = &link_pool[link_pool_used++];
    /* load the link target first so its latency hides behind the budget arithmetic */
    emit_imm32(e, 3, (u32)slot);                    /* R3 = &link slot (data)           */
    E(e, SH_MOVL_LD(3, 3));                         /* R3 = *slot (next block entry)    */
    E(e, SH_ADD_IMM(-(int)cycles, 7));
    E(e, SH_CMP_PL(7));
    u16 *bf = e->p; E(e, SH_BF(0));
    E(e, SH_JMP(3));
    E(e, SH_NOP);
    u16 *slow = e->p;
    *slot = (u32)(uintptr_t)slow;                   /* unresolved: fall into the slow exit */
    emit_imm32(e, 1, next_pc);
    emit_imm32(e, 2, (u32)slot);
    E(e, SH_JMP(14));
    E(e, SH_NOP);
    patch_bt(bf, slow);
}

/* Block exit with a dynamic next pc in register tr (JR/JALR). */
static void emit_exit_dynamic(emit_t *e, int tr, u32 cycles) {
    e->tag = TAG_EXIT;
    E(e, SH_ADD_IMM(-(int)cycles, 7));
    if (tr != 1) E(e, SH_MOV_RR(tr, 1));
    E(e, SH_MOV_IMM(0, 2));
    E(e, SH_JMP(14));
    E(e, SH_NOP);
}

/* ------------------------------------------------------------------ */
/* MIPS instruction compiler                                           */
/* ------------------------------------------------------------------ */

static int compile_simple(emit_t *e, u32 instr) {
    u32 op = instr >> 26;
    int rs = (instr >> 21) & 0x1F, rt = (instr >> 16) & 0x1F, rd = (instr >> 11) & 0x1F;
    int sa = (instr >> 6) & 0x1F;
    s32 imm = (s16)(instr & 0xFFFF);
    u32 uimm = instr & 0xFFFF;

    if (instr == 0) return 1;                                    /* NOP */

#define ADDR_TO_R4() do { emit_ld_reg(e, rs, 4); if (imm >= -128 && imm <= 127) { if (imm) E(e, SH_ADD_IMM(imm, 4)); } else { emit_imm32(e, 2, (u32)imm); E(e, SH_ADD_RR(2, 4)); } } while (0)
#define LOAD_TO(mreg) do { int c_ = e->map[mreg]; if (c_) { E(e, SH_MOV_RR(0, c_)); e->dirty[mreg] = 1; } else if ((mreg) < 16) emit_st_reg_mem(e, 0, mreg); else { E(e, SH_MOV_RR(0, 1)); emit_st_reg_mem(e, 1, mreg); } } while (0)

    switch (op) {
    case 0x00: {
        u32 func = instr & 0x3F;
        switch (func) {
        case 0x00: /* SLL */  emit_shift_imm(e, 0, rt, rd, sa); return 1;
        case 0x02: /* SRL */  emit_shift_imm(e, 1, rt, rd, sa); return 1;
        case 0x03: /* SRA */  emit_shift_imm(e, 2, rt, rd, sa); return 1;
        case 0x04: /* SLLV */ emit_shift_var(e, 0, rs, rt, rd); return 1;
        case 0x06: /* SRLV */ emit_shift_var(e, 1, rs, rt, rd); return 1;
        case 0x07: /* SRAV */ emit_shift_var(e, 2, rs, rt, rd); return 1;
        case 0x10: /* MFHI */ emit_ld_off(e, OFF_HI, 1); emit_st_reg(e, 1, rd); return 1;
        case 0x12: /* MFLO */ emit_ld_off(e, OFF_LO, 1); emit_st_reg(e, 1, rd); return 1;
        case 0x11: /* MTHI */ emit_ld_reg(e, rs, 1); emit_st_off(e, 1, OFF_HI); return 1;
        case 0x13: /* MTLO */ emit_ld_reg(e, rs, 1); emit_st_off(e, 1, OFF_LO); return 1;
        case 0x18: /* MULT */  { int s = src_reg(e, rs, 1), t = src_reg(e, rt, 2); E(e, SH_DMULSL(t, s)); E(e, SH_STS_MACL(1)); emit_st_off(e, 1, OFF_LO); E(e, SH_STS_MACH(1)); emit_st_off(e, 1, OFF_HI); return 1; }
        case 0x19: /* MULTU */ { int s = src_reg(e, rs, 1), t = src_reg(e, rt, 2); E(e, SH_DMULUL(t, s)); E(e, SH_STS_MACL(1)); emit_st_off(e, 1, OFF_LO); E(e, SH_STS_MACH(1)); emit_st_off(e, 1, OFF_HI); return 1; }
        case 0x1A: /* DIV  */ emit_ld_reg(e, rs, 4); emit_ld_reg(e, rt, 5); emit_call(e, (void *)jit_div);  return 1;
        case 0x1B: /* DIVU */ emit_ld_reg(e, rs, 4); emit_ld_reg(e, rt, 5); emit_call(e, (void *)jit_divu); return 1;
        case 0x20: case 0x21: /* ADD/ADDU */ emit_alu_rr(e, SH_ADD_RR(0, 0), rs, rt, rd, 1); return 1;
        case 0x22: case 0x23: /* SUB/SUBU */ emit_alu_rr(e, SH_SUB_RR(0, 0), rs, rt, rd, 0); return 1;
        case 0x24: /* AND */ emit_alu_rr(e, SH_AND_RR(0, 0), rs, rt, rd, 1); return 1;
        case 0x25: /* OR  */ emit_alu_rr(e, SH_OR_RR(0, 0), rs, rt, rd, 1); return 1;
        case 0x26: /* XOR */ emit_alu_rr(e, SH_XOR_RR(0, 0), rs, rt, rd, 1); return 1;
        case 0x27: /* NOR */ { if (!rd) return 1; emit_alu_rr(e, SH_OR_RR(0, 0), rs, rt, rd, 1); int d = e->map[rd]; if (d) E(e, SH_NOT_RR(d, d)); else { emit_ld_reg_mem(e, rd, 1); E(e, SH_NOT_RR(1, 1)); emit_st_reg_mem(e, 1, rd); } return 1; }
        case 0x2A: /* SLT */  emit_slt(e, rs, rt, rd, 0, 0, 0); return 1;
        case 0x2B: /* SLTU */ emit_slt(e, rs, rt, rd, 1, 0, 0); return 1;
        default: return 0;
        }
    }
    case 0x08: case 0x09: /* ADDI/ADDIU */ emit_alu_ri(e, SH_ADD_RR(0, 0), rs, rt, (u32)imm); return 1;
    case 0x0A: /* SLTI  */ emit_slt(e, rs, 0, rt, 0, 1, (u32)imm); return 1;
    case 0x0B: /* SLTIU */ emit_slt(e, rs, 0, rt, 1, 1, (u32)imm); return 1;
    case 0x0C: /* ANDI */ emit_alu_ri(e, SH_AND_RR(0, 0), rs, rt, uimm); return 1;
    case 0x0D: /* ORI  */ emit_alu_ri(e, SH_OR_RR(0, 0), rs, rt, uimm); return 1;
    case 0x0E: /* XORI */ emit_alu_ri(e, SH_XOR_RR(0, 0), rs, rt, uimm); return 1;
    case 0x0F: /* LUI  */ { if (!rt) return 1; int d = dst_reg(e, rt, 1); emit_imm32(e, d, uimm << 16); commit_reg(e, d, rt); return 1; }
    case 0x10: {                                                 /* COP0 */
        if (instr & (1 << 25)) return 0;                          /* RFE: interpreter */
        if (rs == 0x00) { emit_ld_off(e, OFF_COP0(rd), 1); emit_st_reg(e, 1, rt); return 1; }
        if (rs == 0x04) { emit_imm32(e, 4, rd); emit_ld_reg(e, rt, 5); emit_call(e, (void *)jit_mtc0); if (rd == 12) { e->end_block = 1; e->no_link = 1; } return 1; }
        return 0;
    }
    case 0x12: {                                                 /* COP2 */
        if (instr & (1 << 25)) { emit_imm32(e, 4, instr); emit_call(e, (void *)jit_gte); return 1; }
        switch (rs) {
        case 0x00: emit_ld_off(e, OFF_GTE_DR(rd), 1); emit_st_reg(e, 1, rt); return 1;
        case 0x02: emit_ld_off(e, OFF_GTE_CR(rd), 1); emit_st_reg(e, 1, rt); return 1;
        case 0x04: emit_ld_reg(e, rt, 1); emit_st_off(e, 1, OFF_GTE_DR(rd)); return 1;
        case 0x06: emit_ld_reg(e, rt, 1); emit_st_off(e, 1, OFF_GTE_CR(rd)); return 1;
        default: return 0;
        }
    }
    case 0x20: /* LB  */ ADDR_TO_R4(); emit_load(e, 1, 1); LOAD_TO(rt); return 1;
    case 0x24: /* LBU */ ADDR_TO_R4(); emit_load(e, 1, 0); LOAD_TO(rt); return 1;
    case 0x21: /* LH  */ ADDR_TO_R4(); emit_load(e, 2, 1); LOAD_TO(rt); return 1;
    case 0x25: /* LHU */ ADDR_TO_R4(); emit_load(e, 2, 0); LOAD_TO(rt); return 1;
    case 0x23: /* LW  */ ADDR_TO_R4(); emit_load(e, 4, 1); LOAD_TO(rt); return 1;
    case 0x28: case 0x29: case 0x2B: case 0x2A: case 0x2E: case 0x3A: if (e->isc) return 1; /* fallthrough */
    case 0x7F: break;
    }
    switch (op) {
    case 0x28: /* SB  */ ADDR_TO_R4(); emit_ld_reg(e, rt, 5); emit_store(e, 1); return 1;
    case 0x29: /* SH  */ ADDR_TO_R4(); emit_ld_reg(e, rt, 5); emit_store(e, 2); return 1;
    case 0x2B: /* SW  */ ADDR_TO_R4(); emit_ld_reg(e, rt, 5); emit_store(e, 4); return 1;
    case 0x22: /* LWL */ ADDR_TO_R4(); emit_ld_reg(e, rt, 5); emit_call(e, (void *)jit_lwl); LOAD_TO(rt); return 1;
    case 0x26: /* LWR */ ADDR_TO_R4(); emit_ld_reg(e, rt, 5); emit_call(e, (void *)jit_lwr); LOAD_TO(rt); return 1;
    case 0x2A: /* SWL */ ADDR_TO_R4(); emit_ld_reg(e, rt, 5); emit_call(e, (void *)jit_swl); return 1;
    case 0x2E: /* SWR */ ADDR_TO_R4(); emit_ld_reg(e, rt, 5); emit_call(e, (void *)jit_swr); return 1;
    case 0x32: /* LWC2 */ ADDR_TO_R4(); emit_load(e, 4, 1); E(e, SH_MOV_RR(0, 1)); emit_st_off(e, 1, OFF_GTE_DR(rt)); return 1;
    case 0x3A: /* SWC2 */ ADDR_TO_R4(); emit_ld_off(e, OFF_GTE_DR(rt), 5); emit_store(e, 4); return 1;
    default:
        return 0;
    }
#undef ADDR_TO_R4
#undef LOAD_TO
}

static int is_branch(u32 instr) {
    u32 op = instr >> 26;
    if (op == 0) { u32 f = instr & 0x3F; return f == 0x08 || f == 0x09; }
    if (op == 0x01) { int rt = (instr >> 16) & 0x1F; return (rt & 0x1E) == 0 || (rt & 0x1E) == 0x10; }
    return op >= 0x02 && op <= 0x07;
}
static int is_store(u32 instr) {
    u32 op = instr >> 26;
    return op == 0x28 || op == 0x29 || op == 0x2B || op == 0x2A || op == 0x2E || op == 0x3A;
}
/* does the instruction write MIPS register r? (loads, ALU, LUI, MF*, JAL/JALR links excluded here) */
static int writes_reg(u32 instr, int r) {
    if (r == 0 || instr == 0) return 0;
    u32 op = instr >> 26;
    int rt = (instr >> 16) & 0x1F, rd = (instr >> 11) & 0x1F;
    if (op == 0x00) return rd == r || ((instr & 0x3F) == 0x09 && rd == r);
    if (op == 0x10 || op == 0x12) return ((instr >> 21) & 0x1F) == 0x00 || ((instr >> 21) & 0x1F) == 0x02 ? rt == r : 0;
    if (op == 0x02) return 0;
    if (op == 0x03) return r == 31;
    if (op >= 0x28 && op <= 0x2E) return 0;          /* stores */
    if (op == 0x3A) return 0;
    return rt == r;
}

/*
 * Compile the branch at pc with its delay slot and emit the block exits.
 * cycles = total cycles of the block (including this branch pair).
 * The condition is evaluated after the delay slot when the slot does not
 * write the compared registers; otherwise it is computed first into R13
 * (the RAM base register, restored afterwards).
 */
static int compile_branch(emit_t *e, u32 instr, u32 ds, u32 pc, u32 cycles, int *cont) {
    u32 op = instr >> 26;
    int rs = (instr >> 21) & 0x1F, rt = (instr >> 16) & 0x1F, rd = (instr >> 11) & 0x1F;
    s32 imm = (s16)(instr & 0xFFFF);
    u32 target = pc + 4 + (imm << 2);
    u32 jtarget = (pc & 0xF0000000) | ((instr & 0x03FFFFFF) << 2);
    u32 fallthrough = pc + 8;
    int kind;                     /* 0 = dynamic, 1 = unconditional static, 2 = conditional */
    u32 static_target = 0;
    int early = 0;                /* condition / target computed before the delay slot (in R13) */
    int invert = 0;               /* T = !cond after evaluation */

    switch (op) {
    case 0x00: kind = 0; break;                                       /* JR / JALR */
    case 0x02: kind = 1; static_target = jtarget; break;              /* J   */
    case 0x03: emit_imm32(e, 1, pc + 8); emit_st_reg(e, 1, 31); kind = 1; static_target = jtarget; break; /* JAL */
    case 0x04: case 0x05: kind = 2; invert = (op == 0x05); break;      /* BEQ / BNE   : T = (rs == rt) */
    case 0x06: case 0x07: kind = 2; invert = (op == 0x06); break;      /* BLEZ / BGTZ : T = (rs > 0)  */
    case 0x01: kind = 2; invert = !(rt & 1); break;                   /* BLTZ / BGEZ : T = (rs >= 0) */
    default: return 0;
    }
    if (op == 0x01 && (rt & 0x10)) { emit_imm32(e, 1, pc + 8); emit_st_reg(e, 1, 31); }   /* BLTZAL/BGEZAL */

    /* evaluate the condition (into T) or the jump target */
    #define EVAL_COND() do {                                                                   \
        if (op == 0x04 || op == 0x05) { int s_ = src_reg(e, rs, 1); int t_ = src_reg(e, rt, (s_ == 1) ? 2 : 1); E(e, SH_CMP_EQ(t_, s_)); } \
        else if (op == 0x06 || op == 0x07) { int s_ = src_reg(e, rs, 1); E(e, SH_CMP_PL(s_)); } \
        else { int s_ = src_reg(e, rs, 1); E(e, SH_CMP_PZ(s_)); }                           \
    } while (0)

    int uses_rt = (op == 0x04 || op == 0x05);
    if (kind == 0) {
        if (writes_reg(ds, rs)) { emit_ld_reg(e, rs, 1); emit_st_off(e, 1, OFF_LINK); early = 1; }   /* target saved in cpu->link_site (scratch until the exit) */
        if ((instr & 0x3F) == 0x09) { emit_imm32(e, 1, pc + 8); emit_st_reg(e, 1, rd); }
    } else if (kind == 2) {
        if (writes_reg(ds, rs) || (uses_rt && writes_reg(ds, rt))) {
            EVAL_COND();
            E(e, SH_MOVT(1));
            if (invert) { E(e, SH_MOV_IMM(1, 0)); E(e, SH_XOR_RR(0, 1)); }
            emit_st_off(e, 1, OFF_LINK);                              /* condition saved in cpu->link_site (scratch) */
            early = 1;
        }
    }
    if (ds != 0 && !compile_simple(e, ds)) return 0;                 /* delay slot */

    if (kind == 0) {
        int tr;
        if (early) { emit_ld_off(e, OFF_LINK, 1); tr = 1; }
        else tr = src_reg(e, rs, 1);
        emit_writeback(e);
        emit_exit_dynamic(e, tr, cycles);
    } else if (kind == 1) {
        emit_writeback(e);
        emit_exit_static(e, static_target, cycles);
    } else {
        /* T = branch taken */
        if (early) {
            emit_ld_off(e, OFF_LINK, 1);
            E(e, SH_TST_RR(1, 1));                                    /* T = (cond == 0) -> BT means not taken */
        } else {
            EVAL_COND();
        }
        /* branch over the taken path when not taken: BT if (early || invert) else BF */
        int skip_on_t = early ? 1 : invert;
        if (cont && *cont) {
            u16 *br = e->p; E(e, skip_on_t ? SH_BT(0) : SH_BF(0));
            emit_writeback(e);                                        /* taken path only */
            emit_exit_static(e, target, cycles);
            patch_bt(br, e->p);
            e->tag = TAG_ALU;
        } else {
            emit_writeback(e);
            u16 *br = e->p; E(e, skip_on_t ? SH_BT(0) : SH_BF(0));
            emit_exit_static(e, target, cycles);                      /* taken */
            patch_bt(br, e->p);
            emit_exit_static(e, fallthrough, cycles);                 /* not taken */
        }
    }
    #undef EVAL_COND
    if (cont) *cont = (kind == 2) ? *cont : 0;
    return 1;
}

/* classify the GPRs an instruction reads (r1, r2) and writes (w); -1 = none */
static void instr_regs(u32 instr, int *r1, int *r2, int *w) {
    u32 op = instr >> 26;
    int rs = (instr >> 21) & 0x1F, rt = (instr >> 16) & 0x1F, rd = (instr >> 11) & 0x1F;
    *r1 = *r2 = *w = -1;
    if (instr == 0) return;
    if (op == 0x00) {
        u32 f = instr & 0x3F;
        *r1 = rs; *r2 = rt;
        if (f == 0x08) return;                               /* JR */
        if (f == 0x11 || f == 0x13 || f == 0x18 || f == 0x19 || f == 0x1A || f == 0x1B) return;  /* MTHI/MTLO/MULT/DIV */
        *w = rd;                                             /* incl. JALR */
        return;
    }
    if (op == 0x02) return;                                  /* J */
    if (op == 0x03) { *w = 31; return; }                     /* JAL */
    if (op == 0x01) { *r1 = rs; if (rt & 0x10) *w = 31; return; }      /* BLTZ/BGEZ(AL) */
    if (op == 0x04 || op == 0x05) { *r1 = rs; *r2 = rt; return; }      /* BEQ/BNE */
    if (op == 0x06 || op == 0x07) { *r1 = rs; return; }                /* BLEZ/BGTZ */
    if (op == 0x10 || op == 0x12) {                           /* COP0/COP2 */
        if (instr & (1 << 25)) return;                       /* GTE op / RFE */
        if (rs == 0x00 || rs == 0x02) *w = rt; else *r1 = rt;
        return;
    }
    if (op == 0x0F) { *w = rt; return; }                     /* LUI */
    if (op >= 0x28 && op <= 0x2E) { *r1 = rs; *r2 = rt; return; }      /* stores */
    if (op == 0x3A) { *r1 = rs; return; }                    /* SWC2 */
    if (op == 0x32) { *r1 = rs; return; }                    /* LWC2 */
    if (op == 0x22 || op == 0x26) { *r1 = rs; *r2 = rt; *w = rt; return; }   /* LWL/LWR merge rt */
    *r1 = rs; *w = rt;                                       /* ALU immediate / loads */
}

/* ---- poll-loop detection ----------------------------------------------------
 * A block that loads a constant RAM address (lui/addiu + lw), derives the
 * condition of a backward branch from that value, stores only to the stack and
 * does not modify the other registers feeding the condition is a polling loop.
 * At run time, when the polled value and those registers are unchanged across
 * several driver chunks (with interrupts enabled), part of the budget is skipped
 * while the peripherals keep ticking (an interrupt is the only way out). */
typedef struct {
    u32 lui_val[32]; u8 lui_ok[32]; u8 derived[32];
    u32 roots[32];      /* non-derived registers feeding each derived register */
    u32 written;        /* registers written anywhere in the block */
    u32 cand; int bad;
} poll_t;
#define RBIT(r) (((r) > 0) ? (1u << (r)) : 0u)
static void poll_track(poll_t *p, u32 instr) {
    if (instr == 0) return;
    u32 op = instr >> 26;
    int rs = (instr >> 21) & 0x1F, rt = (instr >> 16) & 0x1F, rd = (instr >> 11) & 0x1F;
    s32 imm = (s16)(instr & 0xFFFF);
    { int r1, r2, w; instr_regs(instr, &r1, &r2, &w); if (w > 0) p->written |= RBIT(w); }
    if (op == 0x0F) { p->lui_val[rt] = (u32)(instr & 0xFFFF) << 16; p->lui_ok[rt] = 1; p->derived[rt] = 0; return; }
    if (op == 0x23) {                                                    /* LW */
        int d = 0;
        if (p->lui_ok[rs]) { u32 a = p->lui_val[rs] + (u32)imm; if ((a & 0x1F800000) == 0) { p->cand = a; d = 1; } }
        p->lui_ok[rt] = 0; p->derived[rt] = (u8)d; p->roots[rt] = 0;
        return;
    }
    if (is_store(instr)) { if (rs != 29) p->bad = 1; return; }
    if (op == 0x09 || op == 0x0D) {                                      /* ADDIU / ORI: address building */
        if (p->lui_ok[rs]) { p->lui_val[rt] = (op == 0x09) ? p->lui_val[rs] + (u32)imm : (p->lui_val[rs] | (instr & 0xFFFF)); p->lui_ok[rt] = 1; }
        else p->lui_ok[rt] = 0;
        p->derived[rt] = p->derived[rs]; p->roots[rt] = p->roots[rs];
        return;
    }
    if (op == 0x00) {
        u32 f = instr & 0x3F;
        if (f == 0x08 || f == 0x11 || f == 0x13 || f == 0x18 || f == 0x19 || f == 0x1A || f == 0x1B) return;
        if (rd) {
            int d = p->derived[rs] | p->derived[rt];
            p->derived[rd] = (u8)d; p->lui_ok[rd] = 0;
            p->roots[rd] = d ? ((p->derived[rs] ? p->roots[rs] : RBIT(rs)) | (p->derived[rt] ? p->roots[rt] : RBIT(rt))) : 0;
        }
        return;
    }
    if (op >= 0x08 && op <= 0x0E) { p->derived[rt] = p->derived[rs]; p->roots[rt] = p->roots[rs]; p->lui_ok[rt] = 0; return; }
    { int r1, r2, w; instr_regs(instr, &r1, &r2, &w); if (w > 0) { p->derived[w] = 0; p->lui_ok[w] = 0; } }
}
/* returns the invariant register mask (non-derived inputs) or 0 if the branch is not a poll */
static u32 poll_branch(const poll_t *p, u32 instr, u32 pc, int *ok) {
    u32 op = instr >> 26;
    int rs = (instr >> 21) & 0x1F, rt = (instr >> 16) & 0x1F;
    s32 imm = (s16)(instr & 0xFFFF);
    u32 target = pc + 4 + ((u32)imm << 2);
    *ok = 0;
    if (target >= pc) return 0;                                           /* only backward branches */
    int two = (op == 0x04 || op == 0x05);
    if (!(two || op == 0x06 || op == 0x07 || op == 0x01)) return 0;
    int d = p->derived[rs] || (two && p->derived[rt]);
    if (!d) return 0;
    u32 m = p->derived[rs] ? p->roots[rs] : RBIT(rs);
    if (two) m |= p->derived[rt] ? p->roots[rt] : RBIT(rt);
    *ok = 1;
    return m;
}

/* Pre-scan a block to pick the most used MIPS registers for caching.
 * Also records which registers are written before being read (no entry load needed). */
static void plan_register_cache(mips_t *cpu, mem_t *mem, gpu_t *gpu, u32 pc, emit_t *e) {
    int count[32], seen[32];
    memset(count, 0, sizeof(count));
    memset(seen, 0, sizeof(seen));
    memset(e->first_write, 0, sizeof(e->first_write));
    u32 cur = pc;
    for (u32 n = 0; n < JIT_MAX_INSTR; n++) {
        u32 instr = fetch(cpu, mem, gpu, cur);
        u32 op = instr >> 26;
        int br = is_branch(instr);
        u32 ds = br ? fetch(cpu, mem, gpu, cur + 4) : 0;
        u32 seq[2] = { instr, ds };
        for (int k = 0; k < (br ? 2 : 1); k++) {
            int r1, r2, w;
            instr_regs(seq[k], &r1, &r2, &w);
            if (r1 > 0) { count[r1]++; if (!seen[r1]) seen[r1] = 1; }
            if (r2 > 0) { count[r2]++; if (!seen[r2]) seen[r2] = 1; }
            if (w > 0) { count[w]++; if (!seen[w]) { seen[w] = 1; e->first_write[w] = 1; } }
        }
        if (br) {
            if (!JIT_EXTEND_BLOCKS || op == 0x00 || op == 0x02 || op == 0x03 || is_branch(ds)) break;
            cur += 8; n++;
            continue;
        }
        cur += 4;
    }
    count[0] = 0;
    e->ncached = 0;
    for (int slot = 0; slot < 4; slot++) {
        int best = -1, bc = 1;
        for (int r = 1; r < 32; r++) if (count[r] > bc && !e->map[r]) { bc = count[r]; best = r; }
        if (best < 0) break;
        e->map[best] = 9 + slot;
        e->cached[slot] = best;
        e->ncached++;
    }
}

/* ------------------------------------------------------------------ */
/* block management                                                    */
/* ------------------------------------------------------------------ */

void jit_flush_cache(void) {
    code_ptr = code_base;
    link_pool_used = 0;
    memset(page_has_code, 0, sizeof(page_has_code));
    for (int i = 0; i < JIT_HASH_SIZE; i++) { blocks[i].pc = 0xFFFFFFFF; blocks[i].fn = NULL; }
    stat_flushes++;
}

void jit_init(void) {
    code_base = (u16 *)memalign(4096, JIT_CODE_SIZE);
    memset(page_gen, 0, sizeof(page_gen));
    memset(page_has_code, 0, sizeof(page_has_code));
    jit_flush_cache();
    dlog("JIT2: code cache %d KB, %d block slots, chunk %d", JIT_CODE_SIZE / 1024, JIT_HASH_SIZE, JIT_CHUNK);
}

static inline u32 fetch(mips_t *cpu, mem_t *mem, gpu_t *gpu, u32 pc) {
    u32 p = pc & 0x1FFFFFFF;
    if (p < 0x00800000) return *(u32 *)&mem->ram[p & 0x1FFFFF];
    if (p >= 0x1FC00000 && p < 0x1FC80000) return *(u32 *)&mem->bios[p - 0x1FC00000];
    return mem_read32(cpu, mem, gpu, pc);
}

/* Overwrite a block entry with a stub: cpu->pc = pc; return to the driver. */
static void patch_entry_unlink(u16 *entry, u32 pc) {
    entry[0] = 0xD102;              /* MOV.L @(2,PC),R1  -> literal at entry+12 (next pc) */
    entry[1] = SH_MOV_IMM(0, 2);    /* R2 = 0: no link site */
    entry[2] = SH_JMP(14);          /* return to the driver */
    entry[3] = SH_NOP;
    entry[4] = SH_NOP;
    entry[5] = SH_NOP;
    *(u32 *)&entry[6] = pc;
    dcache_flush_range((uintptr_t)entry, 16);
    icache_flush_range((uintptr_t)entry, 16);
}

/* Overwrite a block entry with a jump to new code. */
static void patch_entry_redirect(u16 *entry, u16 *target) {
    entry[0] = 0xD302;              /* MOV.L @(2,PC),R3 -> literal at entry+12 */
    entry[1] = SH_JMP(3);
    entry[2] = SH_NOP;
    entry[3] = SH_NOP;
    entry[4] = SH_NOP;
    entry[5] = SH_NOP;
    *(u32 *)&entry[6] = (u32)target;
    dcache_flush_range((uintptr_t)entry, 16);
    icache_flush_range((uintptr_t)entry, 16);
}

static jit_block_t *compile_block_n(mips_t *cpu, mem_t *mem, gpu_t *gpu, u32 pc, jit_block_t *b, u32 max_instr);
static jit_block_t *compile_block(mips_t *cpu, mem_t *mem, gpu_t *gpu, u32 pc, jit_block_t *b) {
    /* retry with shorter blocks when the literal pool or displacement overflows */
    static const u32 tries[] = { JIT_MAX_INSTR, 12, 6, 3, 1 };
    for (unsigned t = 0; t < sizeof(tries) / sizeof(tries[0]); t++) {
        jit_block_t *r = compile_block_n(cpu, mem, gpu, pc, b, tries[t]);
        if (r) return r;
    }
    return NULL;
}
static jit_block_t *compile_block_n(mips_t *cpu, mem_t *mem, gpu_t *gpu, u32 pc, jit_block_t *b, u32 max_instr) {
    if ((u8 *)code_ptr > (u8 *)code_base + JIT_CODE_SIZE - 16384) jit_flush_cache();
    u32 key = pc; int isc = (int)(pc & 1); pc &= ~1u;           /* bit 0 of the key = IsC mode */

    emit_t e;
    memset(&e, 0, sizeof(e));
    while (((uintptr_t)code_ptr) & 3) code_ptr++;
    e.start = e.p = code_ptr;
    e.block_pc = pc;
    e.isc = isc; e.no_link = isc;
    e.link_first = link_pool_used;

    u32 checksum = 0, len = 0, cycles = 0;
    int has_store = 0, self_loop = 0;
    poll_t pt; memset(&pt, 0, sizeof(pt)); u32 poll_addr = 0, poll_regs = 0;
    u32 cur = pc;

    /* count instructions first so the exits know the block's cycle cost */
    u32 n_simple = 0, has_branch = 0;
    {
        u32 c = pc;
        for (u32 n = 0; n < JIT_MAX_INSTR; n++) {
            u32 ins = fetch(cpu, mem, gpu, c);
            if (is_branch(ins)) { has_branch = 1; break; }
            n_simple++;
            c += 4;
        }
    }
    (void)n_simple; (void)has_branch;

    plan_register_cache(cpu, mem, gpu, pc, &e);
    emit_entry(&e);

    while (len < max_instr) {
        u32 instr = fetch(cpu, mem, gpu, cur);
        if (is_branch(instr)) {
            u32 ds = fetch(cpu, mem, gpu, cur + 4);
            if (is_branch(ds)) break;
            { int pok; u32 pm = poll_branch(&pt, instr, cur, &pok); if (pok && pt.cand && !poll_addr) { poll_addr = pt.cand; poll_regs = pm; } }
            poll_track(&pt, ds);
            maybe_flush_slow(&e);
            u16 *save = e.p; int savel = e.nlits;
            e.tag = TAG_BRANCH;
            {   /* never extend across a branch back to the block start (keeps idle-loop detection) */
                u32 bop = instr >> 26, btg = 0xFFFFFFFF;
                if (bop >= 0x04 || bop == 0x01) btg = cur + 4 + ((s32)(s16)(instr & 0xFFFF) << 2);
                if (btg == pc) self_loop = 1;
            }
            int cont = JIT_EXTEND_BLOCKS && (len + 2 < max_instr) && !self_loop;
            self_loop = 0;
            if (!compile_branch(&e, instr, ds, cur, cycles + 2, &cont)) { e.p = save; e.nlits = savel; e.nlinks = 0; break; }
            if (is_store(ds)) has_store = 1;
            if (cont) {
                checksum ^= instr ^ ds;
                len += 2; cycles += 2;
                cur += 8;
                continue;
            }
            {
                u32 bop = instr >> 26;
                u32 tgt = 0xFFFFFFFF;
                if (bop == 0x02 || bop == 0x03) tgt = (cur & 0xF0000000) | ((instr & 0x03FFFFFF) << 2);
                else if (bop >= 0x04 || bop == 0x01) tgt = cur + 4 + ((s32)(s16)(instr & 0xFFFF) << 2);
                self_loop = (tgt == pc) && !has_store;
            }
            checksum ^= instr ^ ds;
            len += 2; cycles += 2;
            goto done;
        }
        maybe_flush_slow(&e);
        u16 *save = e.p; int savel = e.nlits;
        e.tag = TAG_ALU;
        if (!compile_simple(&e, instr)) { e.p = save; e.nlits = savel; note_fallback(instr); break; }
        poll_track(&pt, instr);
        if (is_store(instr)) has_store = 1;
        checksum ^= instr;
        len++; cycles++;
        cur += 4;
        if (e.end_block) break;
    }
    if (len == 0) return NULL;
    maybe_flush_slow(&e);
    emit_writeback(&e);
    emit_exit_static(&e, cur, cycles);
done:
    emit_slow_paths(&e);
    if (e.overflow) return NULL;
    while ((e.p - e.start) < 8) E(&e, SH_NOP);        /* entry stubs overwrite 16 bytes: never emit a shorter block */
    u32 *lit_addr[JIT_MAX_LITS];
    if (emit_finish(&e, lit_addr) < 0) return NULL;
    (void)lit_addr;

    dcache_flush_range((uintptr_t)e.start, (uintptr_t)e.p - (uintptr_t)e.start);
    icache_flush_range((uintptr_t)e.start, (uintptr_t)e.p - (uintptr_t)e.start);

    /* retire the previous code of this slot */
    if (b->fn != NULL) {
        if (b->pc == key) patch_entry_redirect((u16 *)b->fn, e.start);
        else patch_entry_unlink((u16 *)b->fn, b->pc & ~1u);
    }

    int pg = page_index(pc & 0x1FFFFFFF);
    b->pc = key; b->len = len; b->checksum = checksum; b->cycles = cycles; b->has_store = has_store; b->self_loop = self_loop;
    b->poll_addr = (pt.bad || (poll_regs & pt.written)) ? 0 : poll_addr; b->poll_regs = poll_regs;
    b->poll_logged = 0;
    b->gen = (pg >= 0) ? page_gen[pg] : 0;
    for (int i = e.link_first; i < link_pool_used; i++) link_owner[i] = b;
    b->fn = (block_fn_t)e.start;
#ifdef JIT_DUMP_PC
    if (pc == JIT_DUMP_PC && !jit_dump_len) {
        int nw = (int)(e.p - e.start);
        int n = snprintf(jit_dump_buf, sizeof(jit_dump_buf), "JITDUMP pc=%08lX len=%lu words=%d\n", (unsigned long)pc, (unsigned long)len, nw);
        for (int k = 0; k < nw && n < (int)sizeof(jit_dump_buf) - 8; k++) n += snprintf(jit_dump_buf + n, sizeof(jit_dump_buf) - n, "%04X%s", e.start[k], ((k % 12) == 11) ? "\n" : " ");
        jit_dump_len = n;
    }
#endif
    code_ptr = e.p;
    if ((pc & 0x1F800000) == 0) { page_has_code[(pc & 0x1FFFFF) >> 16] = 1; page_has_code[((pc + len * 4 - 1) & 0x1FFFFF) >> 16] = 1; }
    stat_compiled++;
    return b;
}

static jit_block_t *lookup_block(mips_t *cpu, mem_t *mem, gpu_t *gpu, u32 pc) {
    /* two-way set: slots (h, h^1); a miss evicts the empty slot or alternates */
    u32 h = ((pc >> 2) ^ (pc >> 14)) & (JIT_HASH_SIZE - 1);
    jit_block_t *b = &blocks[h];
    if (b->pc != pc) {
        jit_block_t *b2 = &blocks[h ^ 1];
        if (b2->pc == pc) b = b2;
        else if (b2->pc == 0xFFFFFFFF && b->pc != 0xFFFFFFFF) b = b2;
        else if (b->pc != 0xFFFFFFFF && b2->pc != 0xFFFFFFFF) { static u32 rr = 0; if (++rr & 1) b = b2; }
    }
    if (b->pc != pc) {
        if (!compile_block(cpu, mem, gpu, pc, b)) {
            if (b->fn != NULL) patch_entry_unlink((u16 *)b->fn, b->pc & ~1u);
            b->pc = pc; b->fn = NULL; stat_fallbacks++;
            return NULL;
        }
        return b;
    }
    if (b->fn == NULL) return NULL;
    /* always verify the source words on driver entry: stores no longer track code pages
     * (direct links between blocks bypass this check exactly as they bypassed page_gen) */
    {
        stat_recheck++;
        u32 sum = 0;
        u32 cpc = pc & ~1u;
        if ((cpc & 0x1F800000) == 0) { const u32 *src = (const u32 *)&mem->ram[cpc & 0x1FFFFC]; for (u32 i = 0; i < b->len; i++) sum ^= src[i]; }
        else for (u32 i = 0; i < b->len; i++) sum ^= fetch(cpu, mem, gpu, cpc + i * 4);
        if (sum != b->checksum) {
            stat_smc++;
            if (!compile_block(cpu, mem, gpu, pc, b)) {
                patch_entry_unlink((u16 *)b->fn, pc & ~1u);
                b->pc = pc; b->fn = NULL;
                return NULL;
            }
        }
    }
    return b;
}

/* Driver trampoline: save R8-R14/PR, R8 = cpu, R14 = return address, jump into the block. */
u32 g_blk_sp = 0, g_blk_sp_bad = 0;
static void call_block(block_fn_t fn, mips_t *cpu) {
    register u32 r4 __asm__("r4") = (u32)fn;
    register u32 r5 __asm__("r5") = (u32)cpu;
    register u32 r6 __asm__("r6") = (u32)&cpu->budget;
    register u32 r3 __asm__("r3") = (u32)&cpu->link_site;
    register u32 r2 __asm__("r2") = (u32)g_mem->ram;
    register u32 r1 __asm__("r1") = (u32)&cpu->pc;
    __asm__ __volatile__(
        "mov.l r8,@-r15\n\t"
        "mov.l r9,@-r15\n\t"
        "mov.l r10,@-r15\n\t"
        "mov.l r11,@-r15\n\t"
        "mov.l r12,@-r15\n\t"
        "mov.l r13,@-r15\n\t"
        "mov.l r14,@-r15\n\t"
        "sts.l pr,@-r15\n\t"
        "mov.l r6,@-r15\n\t"          /* &budget    */
        "mov.l r3,@-r15\n\t"          /* &link_site */
        "mov.l r1,@-r15\n\t"          /* &pc        */
        "mov.l @r6,r7\n\t"            /* R7  = budget   */
        "mov r2,r13\n\t"              /* R13 = RAM base */
        "mov r5,r8\n\t"               /* R8  = cpu      */
        "mov.l 2f,r0\n\t"
        "mov.l r15,@r0\n\t"           /* g_blk_sp = R15 (stack check) */
        "mova 1f,r0\n\t"
        "mov r0,r14\n\t"              /* R14 = return   */
        "jmp @r4\n\t"
        "nop\n"
        ".align 2\n"
        "1:\n\t"                      /* blocks return with R1 = next pc, R2 = link slot, R7 = budget */
        "mov.l 2f,r0\n\t"
        "mov.l @r0,r0\n\t"
        "cmp/eq r0,r15\n\t"
        "bt 3f\n\t"
        "mov.l 4f,r0\n\t"
        "mov.l r15,@r0\n\t"           /* g_blk_sp_bad = bad R15 */
        "mov.l 2f,r0\n\t"
        "mov.l @r0,r15\n\t"           /* repair the stack pointer */
        "3:\n\t"
        "mov.l @r15+,r6\n\t"
        "mov.l r1,@r6\n\t"
        "mov.l @r15+,r6\n\t"
        "mov.l r2,@r6\n\t"
        "mov.l @r15+,r6\n\t"
        "mov.l r7,@r6\n\t"
        "lds.l @r15+,pr\n\t"
        "mov.l @r15+,r14\n\t"
        "mov.l @r15+,r13\n\t"
        "mov.l @r15+,r12\n\t"
        "mov.l @r15+,r11\n\t"
        "mov.l @r15+,r10\n\t"
        "mov.l @r15+,r9\n\t"
        "mov.l @r15+,r8\n\t"
        "bra 5f\n\t"
        "nop\n"
        ".align 2\n"
        "2: .long _g_blk_sp\n"
        "4: .long _g_blk_sp_bad\n"
        "5:\n\t"
        : "+r"(r4), "+r"(r5), "+r"(r6), "+r"(r3), "+r"(r2), "+r"(r1)
        :
        : "r0", "r7", "memory", "t", "mach", "macl");
}

/*
 * Run compiled blocks from cpu->pc until the budget is spent, an interrupt
 * becomes deliverable, or an uncompilable instruction is hit.
 * Returns cycles executed (0 = interpret one instruction at cpu->pc).
 */
int jit_run(mips_t *cpu, mem_t *mem, gpu_t *gpu, int budget) {
    int cyc = 0;
    while (cyc < budget) {
        if (cpu->stop) break;
        u32 isc_key = (cpu->cop0[12] & 0x10000) ? 1u : 0u;        /* IsC: separate blocks whose stores are dropped */
        if ((cpu->i_stat & cpu->i_mask) && (cpu->cop0[12] & 0x401) == 0x401) break;
        u32 pc = cpu->pc;
        u32 p = pc & 0x1FFFFFFF;
        if ((pc & 3) || !(p < 0x00800000 || (p >= 0x1FC00000 && p < 0x1FC80000))) break;
        g_jit_stage = 1;
        jit_block_t *b = lookup_block(cpu, mem, gpu, pc | isc_key);
        if (!b) break;
        g_jit_stage = 2; g_jit_last_fn = (u32)b->fn;
        /* poll candidate: this block, or the block whose backward exit just returned (loop tail) */
        jit_block_t *pb = b->poll_addr ? b : ((prev_owner && prev_owner->poll_addr) ? prev_owner : NULL);
        if (JIT_POLL_SKIP && pb && (cpu->cop0[12] & 0x401) == 0x401) {
            u32 v = *(u32 *)&mem->ram[pb->poll_addr & 0x1FFFFC];
            u32 rh = 0;
            { u32 m = pb->poll_regs; rh = 0; for (int i = 1; i < 32; i++) if (m & (1u << i)) rh = rh * 31 + cpu->r[i]; }
            if (pb == poll_last_blk && v == poll_last_val && rh == poll_last_regs) poll_streak++;
            else { poll_streak = 0; poll_hits = 0; poll_last_blk = pb; poll_last_val = v; poll_last_regs = rh; }
            if (poll_streak >= 3) {
                stat_idle++;
                uint64_t tsk0 = timer_us_gettime64();
                if (!b->poll_logged) {
                    b->poll_logged = 1;
                    dlog("POLL blk=%08lX addr=%08lX regs=%08lX len=%lu f=%lu", (unsigned long)b->pc, (unsigned long)b->poll_addr, (unsigned long)b->poll_regs, (unsigned long)b->len, (unsigned long)cpu->vblank_count);
                    for (u32 i = 0; i < b->len && i < 16; i += 4) dlog("  m%08lX: %08lX %08lX %08lX %08lX", (unsigned long)(b->pc + i * 4), (unsigned long)fetch(cpu, mem, gpu, b->pc + i * 4), (unsigned long)fetch(cpu, mem, gpu, b->pc + i * 4 + 4), (unsigned long)fetch(cpu, mem, gpu, b->pc + i * 4 + 8), (unsigned long)fetch(cpu, mem, gpu, b->pc + i * 4 + 12));
                }
                int limit = cyc + (1024 << (poll_hits < 8 ? poll_hits : 8));
                if (limit > budget) limit = budget;
                poll_hits++; poll_streak = 0;
                while (cyc < limit) {
                    int step = limit - cyc;
                    if (step > 512) step = 512;
                    cyc += step;
                    stat_skipped += step;
                    total_cycles_executed += step;
                    timers_update(cpu, step);
                    cd_update(cpu, step);
                    sio_update(cpu, step);
                    if ((cpu->i_stat & cpu->i_mask) && (cpu->cop0[12] & 0x401) == 0x401) break;
                }
                prof_skip_us += timer_us_gettime64() - tsk0;
                continue;
            }
        }

        int was_self_loop = b->self_loop;
        u32 loop_pc = b->pc;
        int chunk = budget - cyc;
        if (chunk > JIT_CHUNK) chunk = JIT_CHUNK;
#if !JIT_LINKING
        chunk = 1;
#elif JIT_VERIFY
        if (cpu->vblank_count >= JIT_VERIFY_FROM_FRAME) chunk = 1;                                   /* no linking while verifying */
#endif
        cpu->budget = chunk;
        cpu->link_site = 0;
#if JIT_VERIFY
        if (!b->has_store && cpu->vblank_count >= JIT_VERIFY_FROM_FRAME) {
            static int mismatches = 0;
            u32 r0[32], hi0 = cpu->hi, lo0 = cpu->lo, pc0 = cpu->pc;
            u32 gd0[32], gc0[32], gd1[32], gc1[32];
            memcpy(r0, cpu->r, sizeof(r0)); memcpy(gd0, cpu->gte_dr, sizeof(gd0)); memcpy(gc0, cpu->gte_cr, sizeof(gc0));
            call_block(b->fn, cpu);
            cpu->r[0] = 0;
            u32 r1[32], hi1 = cpu->hi, lo1 = cpu->lo, pc1 = cpu->pc;
            memcpy(r1, cpu->r, sizeof(r1)); memcpy(gd1, cpu->gte_dr, sizeof(gd1)); memcpy(gc1, cpu->gte_cr, sizeof(gc1));
            memcpy(cpu->r, r0, sizeof(r0)); cpu->hi = hi0; cpu->lo = lo0; cpu->pc = pc0;
            memcpy(cpu->gte_dr, gd0, sizeof(gd0)); memcpy(cpu->gte_cr, gc0, sizeof(gc0));
            g_jit_verify_running = 1;
            cpu_execute(cpu, mem, gpu, (int)b->cycles);
            g_jit_verify_running = 0;
            int bad = -1;
            for (int i = 1; i < 32; i++) if (r1[i] != cpu->r[i]) { bad = i; break; }
            if (bad < 0 && (hi1 != cpu->hi || lo1 != cpu->lo)) bad = 32;
            if (bad < 0 && pc1 != cpu->pc) bad = 33;
            if (bad < 0) for (int i = 0; i < 32; i++) if (gd1[i] != cpu->gte_dr[i]) { bad = 40 + i; break; }
            if (bad < 0) for (int i = 0; i < 32; i++) if (gc1[i] != cpu->gte_cr[i]) { bad = 80 + i; break; }
            if (bad >= 0 && mismatches++ < 20)
                dlog("JITV mismatch blk=%08lX len=%lu reg=%d jit=%08lX int=%08lX", (unsigned long)pc0,
                     (unsigned long)b->len, bad, (unsigned long)(bad < 32 ? r1[bad] : bad == 32 ? lo1 : pc1),
                     (unsigned long)(bad < 32 ? cpu->r[bad] : bad == 32 ? cpu->lo : cpu->pc));
            cpu->budget = chunk - (int)b->cycles;
            cpu->link_site = 0;
        } else
#endif
        call_block(b->fn, cpu);
        g_jit_stage = 3;
        if (g_blk_sp_bad) {
            static int reported = 0;
            if (reported++ < 4) {
                dlog("JIT STACK IMBALANCE blk=%08lX fn=%08lX sp=%08lX exp=%08lX len=%lu", (unsigned long)b->pc, (unsigned long)b->fn, (unsigned long)g_blk_sp_bad, (unsigned long)g_blk_sp, (unsigned long)b->len);
                for (u32 i = 0; i < b->len && i < 24; i++) dlog("  m%08lX: %08lX", (unsigned long)(b->pc + i * 4), (unsigned long)fetch(cpu, mem, gpu, b->pc + i * 4));
                u16 *w = (u16 *)b->fn; char line[100]; int n = 0;
                for (int i = 0; i < 160; i++) { n += snprintf(line + n, sizeof(line) - n, "%04X ", w[i]); if ((i % 16) == 15) { dlog("  %s", line); n = 0; } }
                if (n) dlog("  %s", line);
            }
            g_blk_sp_bad = 0;
        }
        cpu->r[0] = 0;
        int executed = chunk - cpu->budget;
        if (executed <= 0) executed = (int)b->cycles;
        stat_runs++;
        cyc += executed;

        /* resolve the link the exiting block asked for (skip if the cache was flushed meanwhile) */
        g_jit_stage = 4;
        prev_owner = NULL;
        if (cpu->link_site) {
            u32 si = (u32)((u32 *)cpu->link_site - link_pool);
            if (si < 131072) prev_owner = link_owner[si];
            u32 npc = cpu->pc;
            u32 np = npc & 0x1FFFFFFF;
            u32 site = cpu->link_site;
            u32 flushes_before = stat_flushes;
            if (!(npc & 3) && (np < 0x00800000 || (np >= 0x1FC00000 && np < 0x1FC80000))) {
                jit_block_t *nb = lookup_block(cpu, mem, gpu, npc);
                if (nb && stat_flushes == flushes_before) { *(u32 *)site = (u32)nb->fn; stat_links++; }
            }
        }

        sched_acc += executed;
        while (sched_acc >= 128) {
            sched_acc -= 128;
            total_cycles_executed += 128;
            if ((++g_sample_tick & 15) == 0) g_pc_samples[(g_sample_tick >> 4) & (PC_SAMPLES - 1)] = cpu->pc;
            timers_update(cpu, 128);
            cd_update(cpu, 128);
            sio_update(cpu, 128);
            if ((++g_spu_tick & 127) == 0 && timer_ms_gettime64() - g_spu_last_ms >= 20) ps1_spu_poll();   /* keep the AICA rings fed during long frames */
        }

        /* idle loop: the block keeps branching to itself without side effects.
         * Only an interrupt (or time-driven hardware state) can end it, so
         * fast-forward the remaining budget while keeping the peripherals ticking. */
        if (was_self_loop && cpu->pc == loop_pc) {
            /* only when the register state did not change since the last visit (a counting loop makes progress) */
            u32 h = 0; for (int i = 1; i < 32; i++) h = h * 31 + cpu->r[i];
            int same = (idle_last_pc == loop_pc && idle_last_hash == h);
            idle_last_pc = loop_pc; idle_last_hash = h;
            if (!same) goto no_idle;
            stat_idle++;
            while (cyc < budget) {
                int step = budget - cyc;
                if (step > 512) step = 512;
                cyc += step;
                stat_skipped += step;
                total_cycles_executed += step;
                timers_update(cpu, step);
                cd_update(cpu, step);
                sio_update(cpu, step);
                if ((cpu->i_stat & cpu->i_mask) && (cpu->cop0[12] & 0x401) == 0x401) break;
            }
        }
    no_idle: ;
    }
    return cyc;
}

int jit_execute(mips_t *cpu, mem_t *mem, gpu_t *gpu) {
    return jit_run(cpu, mem, gpu, 1);
}

/* Self-test: compile and run two synthetic blocks (budget 1 -> no linking). */
void jit_selftest(mips_t *cpu, mem_t *mem, gpu_t *gpu) {
    static const struct { const char *name; u32 code[6]; u32 pc; } tests[2] = {
        { "alu+jr", { 0x24420001, 0x34430010, 0x00621821, 0x03E00008, 0x00000000, 0 }, 0x80001000 },
        { "mem+beq", { 0x8C820000, 0xAC820004, 0x24840008, 0x00021880, 0x10430002, 0x00000000 }, 0x80001100 },
    };
    for (int t = 0; t < 2; t++) {
        u32 *code = (u32 *)&mem->ram[tests[t].pc & 0x1FFFFF];
        for (int i = 0; i < 6; i++) code[i] = tests[t].code[i];
        memset(cpu->r, 0, sizeof(cpu->r));
        cpu->r[2] = 5; cpu->r[4] = 0x80002000; cpu->r[31] = 0x80003000;
        *(u32 *)&mem->ram[0x2000] = 0x11223344;
        *(u32 *)&mem->ram[0x2004] = 0;
        cpu->pc = tests[t].pc;
        jit_block_t *b = &blocks[(cpu->pc >> 2) & (JIT_HASH_SIZE - 1)];
        b->pc = 0xFFFFFFFF; b->fn = NULL;
        if (!compile_block(cpu, mem, gpu, cpu->pc, b)) { dlog("JITT %s: compile failed", tests[t].name); continue; }
        u16 *w = (u16 *)b->fn;
        int n = (int)(code_ptr - w);
        dlog("JITT %s: len=%lu cyc=%lu words=%d", tests[t].name, (unsigned long)b->len, (unsigned long)b->cycles, n);
        for (int i = 0; i < n; i += 8) {
            char line[96]; int o = 0;
            for (int k = i; k < i + 8 && k < n; k++) o += snprintf(line + o, sizeof(line) - o, "%04X ", w[k]);
            dlog("  %s", line);
        }
        dlog_flush_vmu();
        cpu->budget = 1; cpu->link_site = 0;
        call_block(b->fn, cpu);
        cpu->r[0] = 0;
        dlog("JITT %s: v0=%08lX v1=%08lX a0=%08lX pc=%08lX mem2004=%08lX budget=%d site=%08lX (expect %s)", tests[t].name,
             (unsigned long)cpu->r[2], (unsigned long)cpu->r[3], (unsigned long)cpu->r[4],
             (unsigned long)cpu->pc, (unsigned long)*(u32 *)&mem->ram[0x2004], cpu->budget, (unsigned long)cpu->link_site,
             t == 0 ? "v0=6 v1=1C pc=80003000" : "v0=11223344 v1=4488CD10 a0=80002008 pc=80001118 mem=11223344");
        dlog_flush_vmu();
        b->pc = 0xFFFFFFFF; b->fn = NULL;
        for (int i = 0; i < 6; i++) code[i] = 0;
    }
    jit_flush_cache();
    cpu_reset(cpu);
}

void jit_stats(char *out, int len) {
    int o = snprintf(out, len, "jit skipped=%lu slowrd=%lu slowwr=%lu ", (unsigned long)stat_skipped, (unsigned long)stat_slow_rd, (unsigned long)stat_slow_wr); stat_slow_rd = stat_slow_wr = 0; stat_skipped = 0;
    for (int k = 0; k < 4; k++) { int best = -1; u32 bv = 0; for (int i = 0; i < 4096; i++) if (slow_hist[i] > bv) { bv = slow_hist[i]; best = i; } if (best < 0 || !bv) break; o += snprintf(out + o, len - o, "%03X:%lu ", best, (unsigned long)bv); slow_hist[best] = 0; }
    memset(slow_hist, 0, sizeof(slow_hist));
    o += snprintf(out + o, len - o, "blocks=%lu runs=%lu links=%lu idle=%lu fb=%lu smc=%lu rechk=%lu flush=%lu code=%luKB fb:",
             (unsigned long)stat_compiled, (unsigned long)stat_runs, (unsigned long)stat_links, (unsigned long)stat_idle, (unsigned long)stat_fallbacks,
             (unsigned long)stat_smc, (unsigned long)stat_recheck, (unsigned long)stat_flushes,
             (unsigned long)(((u8 *)code_ptr - (u8 *)code_base) / 1024));
    for (int i = 0; i < fb_count && o < len - 10; i++) o += snprintf(out + o, len - o, "%lX ", (unsigned long)fb_words[i]);
}
