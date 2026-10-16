#include <kos.h>
#include "mips.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*
 * CD-ROM: disc image access (BIN 2352/2048, ISO9660 lookup, PS-X EXE loader)
 * and the CD-ROM controller (0x1F801800-0x1F801803).
 */

/* ------------------------------------------------------------------ */
/* Disc image                                                          */
/* ------------------------------------------------------------------ */

#define CACHE_SECTORS 64
#define CACHE_READAHEAD 8          /* sectors fetched per image read */

static FILE *bin_file = NULL;
static char  image_name[128] = "";
static u32   sector_size = 2352;
static int   is_mode2 = 1;
static u32   total_sectors = 0;

static struct {
    u32 lba;
    u32 age;
    u8  data[2352];
} sec_cache[CACHE_SECTORS] __attribute__((aligned(32)));
static u32 cache_clock = 0;

const char *cd_image_name(void) { return image_name; }

void cd_stats(char *out, int len);

static int open_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return -1; }
    bin_file = f;
    sector_size = (size % 2352 == 0) ? 2352 : 2048;
    total_sectors = (u32)(size / sector_size);
    strncpy(image_name, path, sizeof(image_name) - 1);
    for (int i = 0; i < CACHE_SECTORS; i++) sec_cache[i].lba = 0xFFFFFFFF;
    if (sector_size == 2352) {
        u8 hdr[16];
        fseek(f, 16L * 2352, SEEK_SET);
        if (fread(hdr, 1, 16, f) == 16) is_mode2 = (hdr[15] == 2);
    } else {
        is_mode2 = 0;
    }
    dlog("CD: %s sectors=%lu ss=%lu mode%d", path, (unsigned long)total_sectors,
         (unsigned long)sector_size, is_mode2 ? 2 : 1);
    return 0;
}

int cd_init_bin(const char *filename) {
    char path[256];
    const char *dirs[] = { "/cd/", "/pc/", "/sd/" };

    if (filename) {
        for (int i = 0; i < 3; i++) {
            snprintf(path, sizeof(path), "%s%s", dirs[i], filename);
            if (open_image(path) == 0) return 0;
        }
    }
    /* Scan /cd for the largest *.BIN that is not a system file */
    file_t d = fs_open("/cd", O_RDONLY | O_DIR);
    if (d != FILEHND_INVALID) {
        const dirent_t *e;
        char best[64] = "";
        u32 best_size = 0;
        while ((e = fs_readdir(d)) != NULL) {
            int n = strlen(e->name);
            if (n < 5) continue;
            if (strcasecmp(e->name + n - 4, ".BIN") != 0) continue;
            if (!strcasecmp(e->name, "1ST_READ.BIN")) continue;
            if (!strcasecmp(e->name, "SCPH1000.BIN")) continue;
            if (!strcasecmp(e->name, "OPENBIOS.BIN")) continue;
            if ((u32)e->size > best_size) { best_size = e->size; strncpy(best, e->name, 63); }
        }
        fs_close(d);
        if (best[0]) {
            snprintf(path, sizeof(path), "/cd/%s", best);
            if (open_image(path) == 0) return 0;
        }
    }
    return -1;
}

static const u8 *get_raw(u32 lba) {
    int victim = 0;
    u32 oldest = 0xFFFFFFFF;
    for (int i = 0; i < CACHE_SECTORS; i++) {
        if (sec_cache[i].lba == lba) { sec_cache[i].age = ++cache_clock; return sec_cache[i].data; }
        if (sec_cache[i].age < oldest) { oldest = sec_cache[i].age; victim = i; }
    }
    u8 *buf = sec_cache[victim].data;
    sec_cache[victim].lba = 0xFFFFFFFF;
    if (!bin_file || lba >= total_sectors) {
        memset(buf, 0, 2352);
        return buf;
    }
    if (sector_size == 2352) {
        /* read-ahead: fetch a run of sectors with one image read and spread them over the oldest slots */
        static u8 stage[CACHE_READAHEAD * 2352] __attribute__((aligned(32)));
        u32 n = CACHE_READAHEAD;
        if (lba + n > total_sectors) n = total_sectors - lba;
        fseek(bin_file, (long)lba * 2352, SEEK_SET);
        size_t got = fread(stage, 1, n * 2352, bin_file);
        ps1_spu_poll();                                      /* a GD-ROM read can take tens of ms */
        u32 nsec = (u32)(got / 2352);
        if (nsec == 0) { memset(buf, 0, 2352); sec_cache[victim].lba = lba; sec_cache[victim].age = ++cache_clock; return buf; }
        memcpy(buf, stage, 2352);
        for (u32 k = 1; k < nsec; k++) {
            int v2 = -1; u32 old2 = 0xFFFFFFFF;
            for (int i = 0; i < CACHE_SECTORS; i++) {
                if (sec_cache[i].lba == lba + k) { v2 = -2; break; }
                if (i != victim && sec_cache[i].age < old2) { old2 = sec_cache[i].age; v2 = i; }
            }
            if (v2 < 0) continue;
            memcpy(sec_cache[v2].data, stage + k * 2352, 2352);
            sec_cache[v2].lba = lba + k;
            sec_cache[v2].age = cache_clock + 1;     /* younger than the victim pool, older than the hit */
        }
    } else {
        /* build a fake Mode1 raw sector around 2048 bytes of data */
        memset(buf, 0, 2352);
        memset(buf + 1, 0xFF, 10);
        u32 m = (lba + 150) / 4500, s = ((lba + 150) / 75) % 60, f = (lba + 150) % 75;
        buf[12] = (u8)(((m / 10) << 4) | (m % 10));
        buf[13] = (u8)(((s / 10) << 4) | (s % 10));
        buf[14] = (u8)(((f / 10) << 4) | (f % 10));
        buf[15] = 1;
        fseek(bin_file, (long)lba * 2048, SEEK_SET);
        if (fread(buf + 16, 1, 2048, bin_file) != 2048) memset(buf + 16, 0, 2048);
    }
    sec_cache[victim].lba = lba;
    sec_cache[victim].age = ++cache_clock;
    return buf;
}

static inline u32 data_offset(const u8 *raw) {
    if (sector_size == 2048) return 16;
    return (raw[15] == 2) ? 24 : 16;
}

int cd_read_raw_sector(u32 lba, u8 *dest) {
    memcpy(dest, get_raw(lba), 2352);
    return 0;
}

int cd_read_data_sector(u32 lba, u8 *dest) {
    const u8 *raw = get_raw(lba);
    memcpy(dest, raw + data_offset(raw), 2048);
    return 0;
}

/* ------------------------------------------------------------------ */
/* ISO9660 + PS-X EXE loader                                           */
/* ------------------------------------------------------------------ */

static u32 rd32le(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

/* find entry `name` in directory (extent lba, size); returns 1 and fills out */
static int iso_find(u32 dir_lba, u32 dir_size, const char *name, u32 *out_lba, u32 *out_size, int *is_dir) {
    static u8 sec[2048];
    int nlen = strlen(name);
    u32 sectors = (dir_size + 2047) / 2048;
    for (u32 s = 0; s < sectors; s++) {
        cd_read_data_sector(dir_lba + s, sec);
        u32 off = 0;
        while (off < 2048) {
            u8 len = sec[off];
            if (len == 0) { off = (off + 2048) & ~2047u; break; }
            if (off + len > 2048) break;
            u8 fl = sec[off + 25];
            int id_len = sec[off + 32];
            const char *id = (const char *)&sec[off + 33];
            int cmp_len = id_len;
            for (int i = 0; i < id_len; i++) if (id[i] == ';') { cmp_len = i; break; }
            if (cmp_len == nlen && strncasecmp(id, name, nlen) == 0) {
                *out_lba = rd32le(&sec[off + 2]);
                *out_size = rd32le(&sec[off + 10]);
                *is_dir = (fl & 2) ? 1 : 0;
                return 1;
            }
            off += len;
        }
    }
    return 0;
}

/* resolve "DIR\\FILE.EXT;1" style path from the root directory */
static int iso_lookup(const char *path, u32 *lba, u32 *size) {
    static u8 pvd[2048];
    cd_read_data_sector(16, pvd);
    if (memcmp(pvd + 1, "CD001", 5) != 0) { dlog("ISO: no PVD"); return 0; }
    u32 dir_lba = rd32le(&pvd[156 + 2]);
    u32 dir_size = rd32le(&pvd[156 + 10]);

    char comp[64];
    const char *p = path;
    while (*p == '\\' || *p == '/') p++;
    while (*p) {
        int n = 0;
        while (*p && *p != '\\' && *p != '/' && n < 63) comp[n++] = *p++;
        comp[n] = 0;
        while (*p == '\\' || *p == '/') p++;
        char *sc = strchr(comp, ';');
        if (sc) *sc = 0;
        u32 l, s; int d;
        if (!iso_find(dir_lba, dir_size, comp, &l, &s, &d)) { dlog("ISO: %s not found", comp); return 0; }
        if (*p) {
            if (!d) return 0;
            dir_lba = l; dir_size = s;
        } else {
            *lba = l; *size = s;
            return 1;
        }
    }
    return 0;
}

u32 cd_load_exe_to_ram(mips_t *cpu, mem_t *mem) {
    static u8 sec[2048];
    u32 lba, size;
    char exe_path[128] = "PSX.EXE";

    if (!bin_file) { dlog("LOADER: no disc image"); return 0; }

    if (iso_lookup("SYSTEM.CNF", &lba, &size)) {
        cd_read_data_sector(lba, sec);
        sec[2047] = 0;
        char *b = strstr((char *)sec, "BOOT");
        if (b) {
            char *eq = strchr(b, '=');
            if (eq) {
                eq++;
                while (*eq == ' ' || *eq == '\t') eq++;
                if (strncasecmp(eq, "cdrom:", 6) == 0) eq += 6;
                int n = 0;
                while (*eq && *eq != '\r' && *eq != '\n' && *eq != ' ' && n < 127) exe_path[n++] = *eq++;
                exe_path[n] = 0;
            }
        }
        dlog("SYSTEM.CNF boot=%s", exe_path);
    } else {
        dlog("SYSTEM.CNF missing, trying PSX.EXE");
    }

    if (!iso_lookup(exe_path, &lba, &size)) { dlog("LOADER: exe not found"); return 0; }

    cd_read_data_sector(lba, sec);
    if (memcmp(sec, "PS-X EXE", 8) != 0) { dlog("LOADER: bad PS-X EXE magic"); return 0; }

    u32 pc0    = rd32le(&sec[0x10]);
    u32 gp0    = rd32le(&sec[0x14]);
    u32 t_addr = rd32le(&sec[0x18]);
    u32 t_size = rd32le(&sec[0x1C]);
    u32 s_addr = rd32le(&sec[0x30]);
    u32 s_size = rd32le(&sec[0x34]);

    u32 dest = t_addr & 0x1FFFFF;
    if (dest + t_size > RAM_SIZE) t_size = RAM_SIZE - dest;
    u32 remaining = t_size;
    u32 cur = lba + 1;
    u8 *out = mem->ram + dest;
    while (remaining > 0) {
        cd_read_data_sector(cur++, sec);
        u32 n = remaining > 2048 ? 2048 : remaining;
        memcpy(out, sec, n);
        out += n;
        remaining -= n;
    }

    jit_invalidate(0x80000000u | dest, t_size);
    cpu->r[28] = gp0;
    cpu->r[29] = (s_addr != 0) ? (s_addr + s_size) : 0x801FFF00;
    cpu->r[30] = cpu->r[29];
    dlog("LOADER: %s pc=%08lX gp=%08lX dst=%08lX size=%lu sp=%08lX", exe_path,
         (unsigned long)pc0, (unsigned long)gp0, (unsigned long)t_addr,
         (unsigned long)t_size, (unsigned long)cpu->r[29]);
    return pc0;
}

/* ------------------------------------------------------------------ */
/* CD-ROM controller                                                   */
/* ------------------------------------------------------------------ */

#define CD_SPEEDUP   4
#define CD_STREAM_DIV 1   /* streaming sector rate divisor (1 = real drive speed) */
#define CMD_DELAY    (0xC4E1 / CD_SPEEDUP)
/* ReadN outside XA mode runs CD_SPEEDUP times faster than a real drive (loading);
 * streaming (XA mode or ReadS) runs at the real 75/150 sectors per second because STR
 * players and XA audio are paced by the sector rate. */
#define SECTOR_DELAY(mode) ((((mode) & 0x40) || cd.read_cmd == 0x1B) ? (((mode) & 0x80) ? 225792 / CD_STREAM_DIV : 451584 / CD_STREAM_DIV) \
                            :(((mode) & 0x80) ? (225792 / CD_SPEEDUP) : (451584 / CD_SPEEDUP)))

#define ST_ERROR   0x01
#define ST_MOTOR   0x02
#define ST_SEEKERR 0x04
#define ST_IDERR   0x08
#define ST_SHELL   0x10
#define ST_READ    0x20
#define ST_SEEK    0x40
#define ST_PLAY    0x80

typedef struct {
    u8  irq;
    u8  len;
    u8  data[16];
    int delay;
} cd_resp_t;

static struct {
    u8  index;
    u8  irq_enable;
    u8  irq_flag;
    u8  param[16];
    int param_len;
    u8  resp[16];
    int resp_len, resp_ptr;
    u8  data_buf[2352];
    int data_len, data_ptr;
    u8  stat;
    u8  mode;
    u32 seek_lba, cur_lba;
    int seek_pending;
    int reading;
    int read_delay;
    cd_resp_t queue[8];
    int q_head, q_count;
    u8  pending_cmd;
    int cmd_busy;
    int cmd_delay;
    u8  last_hdr[8];
    u32 sectors_read;
    u32 cmd_count;
    u8  read_cmd;                /* last ReadN (06) / ReadS (1B) */
    u8  filter_file, filter_chan;
    u8  muted;                   /* Mute (0B) / Demute (0C) */
    u8  adpcm_mute;              /* 1F801803.Index3 bit 0 */
    u8  vol_pend[4], vol[4];     /* LL, LR, RR, RL (applied on 1F801803.Index3 bit 5) */
    u32 xa_sectors, xa_filtered;
} cd;

void cd_init(void) {
    memset(&cd, 0, sizeof(cd));
    cd.stat = ST_MOTOR;
    cd.cur_lba = 0;
    cd.vol[0] = cd.vol[2] = cd.vol_pend[0] = cd.vol_pend[2] = 0x80;
    dlog("CDROM controller reset");
}

static void push_resp(u8 irq, const u8 *data, int len, int delay) {
    if (cd.q_count >= 8) return;
    cd_resp_t *r = &cd.queue[(cd.q_head + cd.q_count) & 7];
    r->irq = irq;
    r->len = len;
    memcpy(r->data, data, len);
    r->delay = delay;
    cd.q_count++;
}

static void push_stat(u8 irq, int delay) { push_resp(irq, &cd.stat, 1, delay); }

/* drop queued sector (INT1) responses, e.g. when reading is stopped */
static void flush_sector_responses(void) {
    cd_resp_t keep[8];
    int n = 0;
    for (int i = 0; i < cd.q_count; i++) {
        cd_resp_t *r = &cd.queue[(cd.q_head + i) & 7];
        if (r->irq != 1) keep[n++] = *r;
    }
    cd.q_head = 0;
    cd.q_count = n;
    for (int i = 0; i < n; i++) cd.queue[i] = keep[i];
}

static u8 bcd(u32 v) { return (u8)(((v / 10) << 4) | (v % 10)); }
static u32 unbcd(u8 v) { return (v >> 4) * 10 + (v & 0xF); }

static void lba_to_msf(u32 lba, u8 *m, u8 *s, u8 *f) {
    u32 a = lba + 150;
    *m = bcd(a / 4500); *s = bcd((a / 75) % 60); *f = bcd(a % 75);
}

static void cd_execute_cmd(u8 cmd) {
    u8 d[16];
    cd.cmd_count++;
    dlog("CD cmd %02X p0=%02X p1=%02X p2=%02X n=%lu lba=%lu rd=%lu", cmd, cd.param[0], cd.param[1], cd.param[2],
         (unsigned long)cd.cmd_count, (unsigned long)cd.cur_lba, (unsigned long)cd.sectors_read);

    switch (cmd) {
    case 0x01: /* Getstat */
        push_stat(3, 0);
        cd.stat &= ~ST_SHELL;
        break;
    case 0x02: /* Setloc */
        cd.seek_lba = (unbcd(cd.param[0]) * 60 + unbcd(cd.param[1])) * 75 + unbcd(cd.param[2]);
        if (cd.seek_lba >= 150) cd.seek_lba -= 150; else cd.seek_lba = 0;
        cd.seek_pending = 1;
        push_stat(3, 0);
        break;
    case 0x03: /* Play */
        cd.stat = (cd.stat & ~(ST_READ | ST_SEEK)) | ST_MOTOR | ST_PLAY;
        cd.reading = 0;
        push_stat(3, 0);
        break;
    case 0x04: case 0x05: /* Forward/Backward */
        push_stat(3, 0);
        break;
    case 0x06: case 0x1B: /* ReadN / ReadS */
        cd.read_cmd = cmd;
        if (cd.seek_pending) { cd.cur_lba = cd.seek_lba; cd.seek_pending = 0; }
        cd.stat = (cd.stat & ~(ST_SEEK | ST_PLAY)) | ST_MOTOR | ST_READ;
        cd.reading = 1;
        cd.read_delay = SECTOR_DELAY(cd.mode);
        push_stat(3, 0);
        break;
    case 0x07: /* MotorOn */
        cd.stat |= ST_MOTOR;
        push_stat(3, 0);
        push_stat(2, 20000);
        break;
    case 0x08: /* Stop */
        cd.reading = 0;
        flush_sector_responses();
        push_stat(3, 0);
        cd.stat = 0;
        push_stat(2, 50000 / CD_SPEEDUP);
        break;
    case 0x09: /* Pause */
        cd.reading = 0;
        flush_sector_responses();
        push_stat(3, 0);
        cd.stat &= ~(ST_READ | ST_PLAY | ST_SEEK);
        push_stat(2, 70000 / CD_SPEEDUP);
        break;
    case 0x0A: case 0x1C: /* Init / Reset */
        cd.mode = 0;
        cd.reading = 0;
        flush_sector_responses();
        cd.stat = ST_MOTOR;
        push_stat(3, 0);
        push_stat(2, 80000 / CD_SPEEDUP);
        break;
    case 0x0B: /* Mute */
        cd.muted = 1;
        push_stat(3, 0);
        break;
    case 0x0C: /* Demute */
        cd.muted = 0;
        push_stat(3, 0);
        break;
    case 0x0D: /* Setfilter */
        cd.filter_file = cd.param[0];
        cd.filter_chan = cd.param[1];
        push_stat(3, 0);
        break;
    case 0x0E: /* Setmode */
        cd.mode = cd.param[0];
        push_stat(3, 0);
        break;
    case 0x0F: /* Getparam */
        d[0] = cd.stat; d[1] = cd.mode; d[2] = 0; d[3] = cd.filter_file; d[4] = cd.filter_chan;
        push_resp(3, d, 5, 0);
        break;
    case 0x10: /* GetlocL */
        push_resp(3, cd.last_hdr, 8, 0);
        break;
    case 0x11: { /* GetlocP */
        u8 m, s, f, am, as, af;
        lba_to_msf(cd.cur_lba >= 150 ? cd.cur_lba - 150 : 0, &m, &s, &f);
        lba_to_msf(cd.cur_lba, &am, &as, &af);
        d[0] = 0x01; d[1] = 0x01; d[2] = m; d[3] = s; d[4] = f; d[5] = am; d[6] = as; d[7] = af;
        push_resp(3, d, 8, 0);
        break;
    }
    case 0x12: /* SetSession */
        push_stat(3, 0);
        push_stat(2, 20000);
        break;
    case 0x13: /* GetTN */
        d[0] = cd.stat; d[1] = 0x01; d[2] = 0x01;
        push_resp(3, d, 3, 0);
        break;
    case 0x14: { /* GetTD */
        u8 m, s, f;
        int track = unbcd(cd.param[0]);
        if (track == 0) lba_to_msf(total_sectors, &m, &s, &f);
        else { m = 0; s = 0x02; f = 0; }
        d[0] = cd.stat; d[1] = m; d[2] = s;
        push_resp(3, d, 3, 0);
        break;
    }
    case 0x15: case 0x16: /* SeekL / SeekP */
        cd.cur_lba = cd.seek_lba;
        cd.seek_pending = 0;
        cd.reading = 0;
        cd.stat = (cd.stat & ~(ST_READ | ST_PLAY)) | ST_MOTOR | ST_SEEK;
        push_stat(3, 0);
        cd.stat &= ~ST_SEEK;
        push_stat(2, 50000 / CD_SPEEDUP);
        break;
    case 0x19: /* Test */
        if (cd.param[0] == 0x20) {
            d[0] = 0x94; d[1] = 0x09; d[2] = 0x19; d[3] = 0xC0;
            push_resp(3, d, 4, 0);
        } else {
            push_stat(3, 0);
        }
        break;
    case 0x1A: /* GetID */
        push_stat(3, 0);
        if (bin_file) {
            d[0] = 0x02; d[1] = 0x00; d[2] = 0x20; d[3] = 0x00;
            d[4] = 'S'; d[5] = 'C'; d[6] = 'E'; d[7] = 'A';
            push_resp(2, d, 8, 20000 / CD_SPEEDUP);
        } else {
            memset(d, 0, 8); d[0] = 0x08; d[1] = 0x40;
            push_resp(5, d, 8, 20000 / CD_SPEEDUP);
        }
        break;
    case 0x1D: case 0x1E: /* GetQ / ReadTOC */
        push_stat(3, 0);
        push_stat(2, 50000 / CD_SPEEDUP);
        break;
    default:
        d[0] = cd.stat | ST_ERROR; d[1] = 0x40;
        push_resp(5, d, 2, 0);
        dlog("CD: unknown cmd %02X", cmd);
        break;
    }
    cd.param_len = 0;
}

static u32 cd_trace = 0;
#define CD_TRACE_LIMIT 120
extern int g_io_trace;

static void cd_deliver(mips_t *cpu, cd_resp_t *r) {
    memcpy(cd.resp, r->data, r->len);
    cd.resp_len = r->len;
    cd.resp_ptr = 0;
    cd.irq_flag = (cd.irq_flag & ~0x7) | r->irq;
    if (cd.irq_enable & cd.irq_flag & 0x1F) cpu->i_stat |= (1u << 2);
    if (cd_trace++ < CD_TRACE_LIMIT)
        dlog("CD INT%d r0=%02X len=%d en=%02X", r->irq, r->data[0], r->len, cd.irq_enable);
}

/* ------------------------------------------------------------------ */
/* XA-ADPCM                                                            */
/* ------------------------------------------------------------------ */

static const int xa_fpos[4] = { 0, 60, 115, 98 };
static const int xa_fneg[4] = { 0, 0, -52, -55 };
static s32 xa_old[2], xa_older[2];
static int16_t xa_out_l[4032], xa_out_r[4032];

/* decode one 28-sample unit: data bytes at grp+16+u (stride 4), sample size 4 or 8 bits */
static void xa_unit(const u8 *grp, int u, int bits8, int ch, int16_t *dst) {
    u8 hp = grp[4 + u];
    int shift = hp & 0x0F; if (shift > 12) shift = 9;
    int f = (hp >> 4) & 3;
    int f0 = xa_fpos[f], f1 = xa_fneg[f];
    s32 old = xa_old[ch], older = xa_older[ch];
    const u8 *d = grp + 16 + (bits8 ? u : (u >> 1));
    int nib = (u & 1) * 4;
    for (int j = 0; j < 28; j++, d += 4) {
        s32 t = bits8 ? ((s32)(s8)d[0] << 8) : ((s32)(s16)(u16)(((d[0] >> nib) & 0x0F) << 12));
        s32 s = (t >> shift) + ((old * f0 + older * f1 + 32) >> 6);
        if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
        dst[j] = (int16_t)s;
        older = old; old = s;
    }
    xa_old[ch] = old; xa_older[ch] = older;
}

uint64_t g_xa_win_us = 0;
static void xa_decode_sector(const u8 *raw) {
    uint64_t t0 = timer_us_gettime64();
    u8 ci = raw[19];
    int stereo = (ci & 3) == 1;
    u32 rate = (ci & 0x04) ? 18900 : 37800;
    int bits8 = (ci & 0x30) == 0x10;
    int units = bits8 ? 4 : 8;
    int16_t *buf_l = xa_out_l, *buf_r = xa_out_r;     /* decoded in place, volume applied below */
    int nl = 0, nr = 0;
    const u8 *grp = raw + 24;
    for (int g = 0; g < 18; g++, grp += 128) {
        for (int u = 0; u < units; u++) {
            if (stereo && (u & 1)) { xa_unit(grp, u, bits8, 1, buf_r + nr); nr += 28; }
            else { xa_unit(grp, u, bits8, 0, buf_l + nl); nl += 28; }
        }
    }
    if (!stereo) { memcpy(buf_r, buf_l, (size_t)nl * 2); nr = nl; }
    int n = nl < nr ? nl : nr;

    /* CD volume matrix (0x80 = 1.0) x SPU CD volume x main volume */
    int32_t gl, gr;
    spu_xa_gain(&gl, &gr);
    if (cd.muted || cd.adpcm_mute) gl = gr = 0;
    int32_t a = (cd.vol[0] * gl) >> 7, b = (cd.vol[3] * gl) >> 7;   /* L->L, R->L */
    int32_t c = (cd.vol[2] * gr) >> 7, e = (cd.vol[1] * gr) >> 7;   /* R->R, L->R */
    for (int i = 0; i < n; i++) {
        s32 L = buf_l[i], R = buf_r[i];
        s32 ol = ((L * a) >> 15) + ((R * b) >> 15);
        s32 orr = ((R * c) >> 15) + ((L * e) >> 15);
        if (ol > 32767) ol = 32767; else if (ol < -32768) ol = -32768;
        if (orr > 32767) orr = 32767; else if (orr < -32768) orr = -32768;
        xa_out_l[i] = (int16_t)ol; xa_out_r[i] = (int16_t)orr;
    }
    spu_xa_push(xa_out_l, xa_out_r, n, rate);
    g_xa_win_us += timer_us_gettime64() - t0;
}

/* returns 1 if the sector goes to the host (INT1), 0 if it was consumed as XA audio or filtered */
static int cd_read_next_sector(void) {
    const u8 *raw = get_raw(cd.cur_lba);
    memcpy(cd.last_hdr, raw + 12, 8);
    if ((cd.mode & 0x40) && sector_size == 2352 && raw[15] == 2 && (raw[18] & 0x24) == 0x24) {
        cd.cur_lba++;
        cd.sectors_read++;
        if ((cd.mode & 0x08) && (raw[16] != cd.filter_file || raw[17] != cd.filter_chan)) { cd.xa_filtered++; return 0; }
        cd.xa_sectors++;
        static int xlog = 0;
        if (xlog++ < 8) dlog("XA lba=%lu file=%02X ch=%02X ci=%02X", (unsigned long)(cd.cur_lba - 1), raw[16], raw[17], raw[19]);
        xa_decode_sector(raw);
        return 0;
    }
    if (cd.mode & 0x20) {
        memcpy(cd.data_buf, raw + 12, 2340);
        cd.data_len = 2340;
    } else {
        memcpy(cd.data_buf, raw + data_offset(raw), 2048);
        cd.data_len = 2048;
    }
    cd.data_ptr = cd.data_len;      /* nothing readable until BFRD is set */
    cd.cur_lba++;
    cd.sectors_read++;
    return 1;
}

void cd_update(mips_t *cpu, u32 cycles) {
    dma_tick(cycles);
    if (cd.cmd_busy) {
        cd.cmd_delay -= cycles;
        if (cd.cmd_delay <= 0) {
            cd.cmd_busy = 0;
            cd_execute_cmd(cd.pending_cmd);
        }
    }
    if (cd.reading) {
        cd.read_delay -= cycles;
        if (cd.read_delay <= 0) {
            if (cd.q_count == 0 && (cd.irq_flag & 7) == 0) {
                if (cd_read_next_sector()) push_stat(1, 0);
                cd.read_delay = SECTOR_DELAY(cd.mode);
            } else {
                cd.read_delay = 64;   /* retry soon: controller still busy */
            }
        }
    }
    if (cd.q_count > 0 && (cd.irq_flag & 7) == 0) {
        cd_resp_t *r = &cd.queue[cd.q_head];
        r->delay -= cycles;
        if (r->delay <= 0) {
            cd_deliver(cpu, r);
            cd.q_head = (cd.q_head + 1) & 7;
            cd.q_count--;
        }
    }
}

void cd_write_reg(u32 paddr, u8 val) {
    switch (paddr & 3) {
    case 0:
        cd.index = val & 3;
        break;
    case 1:
        switch (cd.index) {
        case 0:
            cd.pending_cmd = val;
            cd.cmd_busy = 1;
            cd.cmd_delay = CMD_DELAY;
            break;
        case 3: cd.vol_pend[2] = val; break;     /* R-CD -> R-SPU */
        default: break;              /* sound map data / coding info: ignore */
        }
        break;
    case 2:
        switch (cd.index) {
        case 0: if (cd.param_len < 16) cd.param[cd.param_len++] = val; break;
        case 1:
            cd.irq_enable = val & 0x1F;
            if (cd_trace++ < CD_TRACE_LIMIT) dlog("CD irqen %02X", val);
            break;
        case 2: cd.vol_pend[0] = val; break;     /* L-CD -> L-SPU */
        case 3: cd.vol_pend[3] = val; break;     /* R-CD -> L-SPU */
        default: break;
        }
        break;
    case 3:
        switch (cd.index) {
        case 0:                      /* request register */
            if (cd_trace++ < CD_TRACE_LIMIT) dlog("CD req %02X (dlen %d)", val, cd.data_len);
            if (val & 0x80) { if (cd.data_ptr >= cd.data_len) cd.data_ptr = 0; }
            else { cd.data_ptr = cd.data_len; }
            break;
        case 1:                      /* interrupt flag acknowledge */
            if (cd_trace++ < CD_TRACE_LIMIT) dlog("CD ack %02X (flag %02X)", val, cd.irq_flag);
            if (g_io_trace == 0 && cd.cmd_count == 11 && (cd.irq_flag & 7) == 2) { extern int g_io_trace; g_io_trace = 60; }
            cd.irq_flag &= ~(val & 0x1F);
            if (val & 0x40) cd.param_len = 0;
            break;
        case 2: cd.vol_pend[1] = val; break;     /* L-CD -> R-SPU */
        case 3:                      /* audio volume apply / ADPCM mute */
            cd.adpcm_mute = val & 1;
            if (val & 0x20) memcpy(cd.vol, cd.vol_pend, 4);
            break;
        default: break;
        }
        break;
    }
}

u8 cd_read_reg(u32 paddr) {
    switch (paddr & 3) {
    case 0: {
        u8 st = cd.index;
        if (cd.param_len == 0) st |= 0x08;
        if (cd.param_len < 16) st |= 0x10;
        if (cd.resp_ptr < cd.resp_len) st |= 0x20;
        if (cd.data_ptr < cd.data_len) st |= 0x40;
        if (cd.cmd_busy) st |= 0x80;
        return st;
    }
    case 1:
        if (cd.resp_ptr < cd.resp_len) return cd.resp[cd.resp_ptr++];
        return 0;
    case 2:
        if (cd.data_ptr < cd.data_len) return cd.data_buf[cd.data_ptr++];
        return 0;
    case 3:
        if (cd.index == 0 || cd.index == 2) return cd.irq_enable | 0xE0;
        return cd.irq_flag | 0xE0;
    }
    return 0;
}

void cd_stats(char *out, int len) {
    snprintf(out, len, "cd cmds=%lu rd=%lu lba=%lu reading=%d flag=%02X en=%02X q=%d dlen=%d dptr=%d stat=%02X mode=%02X xa=%lu/%lu filt=%02X:%02X",
             (unsigned long)cd.cmd_count, (unsigned long)cd.sectors_read, (unsigned long)cd.cur_lba, cd.reading,
             cd.irq_flag, cd.irq_enable, cd.q_count, cd.data_len, cd.data_ptr, cd.stat, cd.mode,
             (unsigned long)cd.xa_sectors, (unsigned long)cd.xa_filtered, cd.filter_file, cd.filter_chan);
}

u32 cd_dma_read32(void) {
    u32 v = 0;
    for (int i = 0; i < 4; i++) {
        u8 b = (cd.data_ptr < cd.data_len) ? cd.data_buf[cd.data_ptr++] : 0;
        v |= (u32)b << (i * 8);
    }
    return v;
}
