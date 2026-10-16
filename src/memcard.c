#include "mips.h"
#include <kos.h>
#include <string.h>

/*
 * PS1 memory card (port 1) emulation: a 128KB image kept in RAM and
 * persisted to the VMU as a run-length compressed file (/vmu/a1/BLEEMMC).
 * Protocol per psx-spx (read 'R', write 'W', get id 'S'), 128-byte frames.
 */

#define MC_SIZE   (128 * 1024)
#define MC_FRAMES (MC_SIZE / 128)
#define MC_FILE   "/vmu/a1/BLEEMMC"
extern void dlog_win(const char *);
static void mclog(const char *fmt, ...) { char b[120]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap); dlog("%s", b); dlog_win(b); }
#include <stdarg.h>
#include <stdio.h>

static u8  mc_data[MC_SIZE] __attribute__((aligned(32)));
static u8  mc_flag = 0x08;          /* bit3: no write since power-up */
static int mc_dirty = 0;
static int mc_dirty_frames = 0;

static void mc_format(void) {
    memset(mc_data, 0, MC_SIZE);
    u8 *f = mc_data;
    f[0] = 'M'; f[1] = 'C'; f[127] = 'M' ^ 'C';
    for (int i = 1; i <= 15; i++) {                 /* directory: all free */
        u8 *d = mc_data + i * 128;
        d[0] = 0xA0; d[8] = 0xFF; d[9] = 0xFF; d[127] = 0xA0;
    }
    for (int i = 16; i <= 35; i++) {                /* broken sector list: none */
        u8 *d = mc_data + i * 128;
        d[0] = d[1] = d[2] = d[3] = 0xFF; d[8] = 0xFF; d[9] = 0xFF;
    }
    memcpy(mc_data + 63 * 128, mc_data, 128);       /* write test frame */
}

/* ---- persistence (RLE: <count:1><value:1>, count 1..255) ---- */
static int mc_load(void) {
    file_t fd = fs_open(MC_FILE, O_RDONLY);
    if (fd == FILEHND_INVALID) return -1;
    static u8 buf[MC_SIZE * 2 + 16];
    ssize_t n = fs_read(fd, buf, sizeof(buf));
    fs_close(fd);
    if (n < 4 || memcmp(buf, "MC1", 3) != 0) return -2;
    u32 pos = 0;
    for (ssize_t i = 4; i + 1 < n && pos < MC_SIZE; i += 2) {
        u32 cnt = buf[i]; u8 v = buf[i + 1];
        if (pos + cnt > MC_SIZE) cnt = MC_SIZE - pos;
        memset(mc_data + pos, v, cnt);
        pos += cnt;
    }
    return (pos == MC_SIZE) ? 0 : -3;
}

static int mc_save(void) {
    static u8 buf[MC_SIZE * 2 + 16];
    u32 o = 0;
    buf[o++] = 'M'; buf[o++] = 'C'; buf[o++] = '1'; buf[o++] = 0;
    u32 i = 0;
    while (i < MC_SIZE) {
        u8 v = mc_data[i]; u32 cnt = 1;
        while (i + cnt < MC_SIZE && mc_data[i + cnt] == v && cnt < 255) cnt++;
        buf[o++] = (u8)cnt; buf[o++] = v;
        i += cnt;
    }
    file_t fd = fs_open(MC_FILE, O_WRONLY | O_TRUNC);
    if (fd == FILEHND_INVALID) return -1;
    fs_write(fd, buf, o);
    fs_close(fd);
    dlog("MEMCARD: saved %lu bytes to VMU", (unsigned long)o);
    return 0;
}

void memcard_init(void) {
    mc_format();
    int r = mc_load();
    dlog("MEMCARD: %s", r == 0 ? "loaded from VMU" : "new (formatted)");
    mc_flag = 0x08; mc_dirty = 0;
}

/* called once per frame: write back a little after the last card write */
void memcard_poll(void) {
    if (!mc_dirty) return;
    if (++mc_dirty_frames < 120) return;
    mc_dirty_frames = 0;
    mc_dirty = 0;
    mc_save();
}

/* ---- transfer state machine: returns rx byte, sets *ack ---- */
static struct { int step; int cmd; u32 sector; u8 chk; u8 frame[128]; int bad; } st;

void memcard_reset(void) { st.step = 0; }

u8 memcard_transfer(u8 tx, int *ack) {
    u8 rx = 0xFF;
    *ack = 1;
    int s = st.step;
    if (s == 0) {                                   /* 0x81 already consumed by the caller */
        st.cmd = tx; st.step = 1;
        { static int nlog = 0; if (nlog++ < 40) mclog("MC cmd=%02X flag=%02X", tx, mc_flag); }
        if (tx != 0x52 && tx != 0x57 && tx != 0x53) { st.step = 0; *ack = 0; return 0xFF; }
        return mc_flag;
    }
    if (st.cmd == 0x53) {                           /* get id */
        static const u8 id[] = { 0x5A, 0x5D, 0x5C, 0x5D, 0x04, 0x00, 0x00, 0x80 };
        rx = (s - 1 < 8) ? id[s - 1] : 0xFF;
        st.step++;
        if (s - 1 >= 7) { st.step = 0; *ack = 0; }
        return rx;
    }
    if (st.cmd == 0x52) {                           /* read 128-byte frame */
        switch (s) {
        case 1: rx = 0x5A; break;
        case 2: rx = 0x5D; break;
        case 3: st.sector = (u32)tx << 8; rx = 0x00; break;
        case 4: st.sector |= tx; rx = 0x00; break;
        case 5: rx = 0x5C; break;
        case 6: rx = 0x5D; break;
        case 7:
            st.bad = st.sector >= MC_FRAMES;
            if (!st.bad) { memcpy(st.frame, mc_data + st.sector * 128, 128); st.chk = (u8)(st.sector >> 8) ^ (u8)st.sector; for (int i = 0; i < 128; i++) st.chk ^= st.frame[i]; }
            rx = st.bad ? 0xFF : (u8)(st.sector >> 8); break;
        case 8: rx = st.bad ? 0xFF : (u8)st.sector; break;
        default:
            if (s >= 9 && s < 9 + 128) rx = st.bad ? 0 : st.frame[s - 9];
            else if (s == 9 + 128) rx = st.chk;
            else if (s == 10 + 128) { { static int nlog = 0; if (nlog++ < 40) mclog("MC read sec=%lu bad=%d", (unsigned long)st.sector, st.bad); } rx = st.bad ? 0xFF : 0x47; st.step = 0; *ack = 0; return rx; }
            break;
        }
        st.step++;
        return rx;
    }
    if (st.cmd == 0x57) {                           /* write 128-byte frame */
        switch (s) {
        case 1: rx = 0x5A; break;
        case 2: rx = 0x5D; break;
        case 3: st.sector = (u32)tx << 8; st.chk = tx; rx = 0x00; break;
        case 4: st.sector |= tx; st.chk ^= tx; rx = 0x00; break;
        default:
            if (s >= 5 && s < 5 + 128) { st.frame[s - 5] = tx; st.chk ^= tx; rx = 0x00; }
            else if (s == 5 + 128) { st.bad = (tx != st.chk) ? 2 : (st.sector >= MC_FRAMES ? 1 : 0); rx = 0x00; }
            else if (s == 6 + 128) rx = 0x5C;
            else if (s == 7 + 128) rx = 0x5D;
            else if (s == 8 + 128) {
                { static int nlog = 0; if (nlog++ < 40) mclog("MC write sec=%lu bad=%d", (unsigned long)st.sector, st.bad); }
                if (st.bad == 0) {
                    memcpy(mc_data + st.sector * 128, st.frame, 128);
                    mc_dirty = 1; mc_dirty_frames = 0;
                    mc_flag &= ~0x08;
                    rx = 0x47;
                } else rx = (st.bad == 2) ? 0x4E : 0xFF;
                st.step = 0; *ack = 0; return rx;
            }
            break;
        }
        st.step++;
        return rx;
    }
    st.step = 0; *ack = 0;
    return 0xFF;
}
