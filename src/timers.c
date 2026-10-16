#include "mips.h"
#include <string.h>

/*
 * PS1 root counters (timers 0-2) at 0x1F801100-0x1F80112F.
 *  +0 current value, +4 mode, +8 target
 */

typedef struct {
    u32 count;
    u32 mode;
    u32 target;
    u32 acc;        /* sub-tick accumulator for divided clocks */
} ptimer_t;

static ptimer_t tm[3];

#define CYCLES_PER_HBLANK 2146   /* 33868800 / 60 / 263 */

void timers_init(void) {
    memset(tm, 0, sizeof(tm));
    for (int i = 0; i < 3; i++) tm[i].mode = 0x400;
}

static void timer_tick(mips_t *cpu, int ch, u32 ticks) {
    ptimer_t *t = &tm[ch];
    if (ticks == 0) return;

    u32 before = t->count;
    u32 cnt = before + ticks;
    u32 target = t->target & 0xFFFF;
    int reached = 0, overflow = 0;

    if (t->mode & 0x8) {                 /* reset to 0 when target reached */
        if (cnt >= target) {
            reached = 1;
            cnt = (target + 1) ? (cnt % (target + 1)) : 0;
        }
    } else {
        if (before < target && cnt >= target) reached = 1;
        if (cnt > 0xFFFF) { cnt &= 0xFFFF; overflow = 1; }
    }

    if (reached) {
        t->mode |= 0x800;
        if (t->mode & 0x10) { cpu->i_stat |= (1u << (4 + ch)); t->mode &= ~0x400; }
    }
    if (overflow) {
        t->mode |= 0x1000;
        if (t->mode & 0x20) { cpu->i_stat |= (1u << (4 + ch)); t->mode &= ~0x400; }
    }
    t->count = cnt;
}

static u32 tm_pending = 0;
static void timers_flush(mips_t *cpu);
/* batch the cycle updates: the counters are only brought up to date every 1024
 * cycles or when a timer register is accessed (IRQ timing granularity ~30us) */
void timers_update(mips_t *cpu, u32 cycles) {
    tm_pending += cycles;
    if (tm_pending < 1024) return;
    timers_flush(cpu);
}
static void timers_flush(mips_t *cpu) {
    u32 cycles = tm_pending;
    tm_pending = 0;
    if (!cycles) return;
    /* Timer 0: system clock or dot clock */
    {
        u32 src = (tm[0].mode >> 8) & 3;
        u32 ticks = (src & 1) ? (cycles * 8) / 5 : cycles;
        timer_tick(cpu, 0, ticks);
    }
    /* Timer 1: system clock or hblank */
    {
        u32 src = (tm[1].mode >> 8) & 3;
        if (src & 1) {
            tm[1].acc += cycles;
            u32 ticks = tm[1].acc / CYCLES_PER_HBLANK;
            tm[1].acc %= CYCLES_PER_HBLANK;
            timer_tick(cpu, 1, ticks);
        } else {
            timer_tick(cpu, 1, cycles);
        }
    }
    /* Timer 2: system clock or system clock / 8; sync mode 0/3 stops it */
    {
        u32 mode = tm[2].mode;
        int stopped = (mode & 1) && (((mode >> 1) & 3) == 0 || ((mode >> 1) & 3) == 3);
        if (!stopped) {
            u32 src = (mode >> 8) & 3;
            if (src & 2) {
                tm[2].acc += cycles;
                u32 ticks = tm[2].acc / 8;
                tm[2].acc %= 8;
                timer_tick(cpu, 2, ticks);
            } else {
                timer_tick(cpu, 2, cycles);
            }
        }
    }
}

void timers_debug(int ch, u32 *count, u32 *mode, u32 *target) {
    if (ch < 0 || ch > 2) return;
    *count = tm[ch].count; *mode = tm[ch].mode; *target = tm[ch].target;
}

u32 timers_read(mips_t *cpu, u32 paddr) {
    int ch = (paddr >> 4) & 3;
    timers_flush(cpu);
    if (ch > 2) return 0;
    switch (paddr & 0xF) {
        case 0: return tm[ch].count & 0xFFFF;
        case 4: { u32 v = tm[ch].mode; tm[ch].mode &= ~0x1800; return v; }
        case 8: return tm[ch].target & 0xFFFF;
    }
    return 0;
}

void timers_write(mips_t *cpu, u32 paddr, u32 val) {
    int ch = (paddr >> 4) & 3;
    timers_flush(cpu);
    if (ch > 2) return;
    static int tw_log = 0;
    if (tw_log++ < 24) dlog("TMR%d wr +%lX=%04lX (cnt %04lX)", ch, (unsigned long)(paddr & 0xF), (unsigned long)val, (unsigned long)tm[ch].count);
    switch (paddr & 0xF) {
        case 0: tm[ch].count = val & 0xFFFF; break;
        case 4: tm[ch].mode = (val & 0x3FF) | 0x400; tm[ch].count = 0; tm[ch].acc = 0; break;
        case 8: tm[ch].target = val & 0xFFFF; break;
    }
}
