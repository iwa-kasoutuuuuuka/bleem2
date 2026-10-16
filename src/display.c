#include <kos.h>
#include <dc/pvr.h>
#include <malloc.h>
#include <string.h>
#include "mips.h"

/*
 * PVR display path: the PS1 display area is converted (15-bit BGR -> PVR
 * ARGB1555 via a lookup table) into a cached RAM buffer, uploaded to a
 * PVR texture with the store queues, and drawn as one textured quad that
 * the hardware scales to 640x480.  The HUD is drawn into the same buffer.
 */

static pvr_ptr_t tex = NULL;
static u16 *txbuf = NULL;           /* 1024x512 max, stride = cur_tw */
static u16 line[1024] __attribute__((aligned(32)));   /* one converted row (store-queue source) */
static int cur_tw = 512, cur_th = 256;
static int cur_w = 320, cur_h = 240;
static pvr_poly_hdr_t hdr512, hdr1024, hdr512_565, hdr1024_565;
static int cur_24 = 0;             /* frame was converted from a 24-bit display area (RGB565 texture) */
static int direct_ok = 0;           /* texture is 32-byte aligned: write it directly */

void display_init(void) {
    pvr_init_params_t params = {
        { PVR_BINSIZE_8, PVR_BINSIZE_0, PVR_BINSIZE_0, PVR_BINSIZE_0, PVR_BINSIZE_0 },
        256 * 1024, 0, 0, 0, 0, 0
    };
    pvr_init(&params);
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);

    tex = pvr_mem_malloc(1024 * 512 * 2);
    direct_ok = tex && (((u32)tex & 31) == 0);
    if (!direct_ok) {                   /* staging buffer only when the texture cannot be written directly */
        txbuf = (u16 *)memalign(32, 1024 * 512 * 2);
        if (txbuf) memset(txbuf, 0, 1024 * 512 * 2);
    }
    if (!tex || (!direct_ok && !txbuf)) { dlog("DISPLAY: alloc failed"); return; }
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED, 512, 256, tex, PVR_FILTER_NONE);
    cxt.txr.alpha = PVR_TXRALPHA_DISABLE;
    pvr_poly_compile(&hdr512, &cxt);
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED, 1024, 512, tex, PVR_FILTER_NONE);
    cxt.txr.alpha = PVR_TXRALPHA_DISABLE;
    pvr_poly_compile(&hdr1024, &cxt);
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED, 512, 256, tex, PVR_FILTER_NONE);
    pvr_poly_compile(&hdr512_565, &cxt);
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED, 1024, 512, tex, PVR_FILTER_NONE);
    pvr_poly_compile(&hdr1024_565, &cxt);
    dlog("DISPLAY: PVR ready, tex=%08lX direct=%d", (unsigned long)(u32)tex, direct_ok);
}

/* Convert the PS1 display area straight into the PVR texture with the store queues. */

static inline u32 conv2(u32 w) {
    return ((w & 0x001F001Fu) << 10) | (w & 0x03E003E0u) | ((w >> 10) & 0x001F001Fu);
}

void display_begin(gpu_t *g) {
    static const int hres_tab[4] = { 256, 320, 512, 640 };
    int W = (g->disp_mode & 0x40) ? 368 : hres_tab[g->disp_mode & 3];
    int H = ((g->disp_mode & 0x4) && (g->disp_mode & 0x20)) ? 480 : 240;
    cur_w = W; cur_h = H;
    cur_tw = (W > 512) ? 1024 : 512;
    cur_th = (H > 256) ? 512 : 256;

    pvr_wait_ready();                   /* previous render finished: the texture is free */
    cur_24 = 0;

    if (!g->disp_enabled) {
        if (direct_ok) sq_set(tex, 0, (size_t)cur_tw * H * 2);
        else memset(txbuf, 0, cur_tw * H * 2);
        return;
    }
    int sx = g->disp_x;
    cur_24 = (g->disp_mode & 0x10) != 0;
    if (cur_24) {
        /* 24-bit display (MDEC movies): 3 bytes per pixel -> RGB565 */
#define P565(r, gg, b) ((((r) & 0xF8) << 8) | (((gg) & 0xFC) << 3) | ((b) >> 3))
        int fast24 = !(sx & 1) && sx + (W * 3) / 2 <= 1024 && !(W & 3);
        for (int y = 0; y < H; y++) {
            const u16 *row = &g->vram[((g->disp_y + y) & 511) << 10];
            u16 *dst = direct_ok ? line : txbuf + y * cur_tw;
            if (fast24) {
                const u32 *s = (const u32 *)(row + sx);
                u32 *d = (u32 *)dst;
                for (int x = 0; x < W; x += 4, s += 3, d += 2) {
                    u32 w0 = s[0], w1 = s[1], w2 = s[2];
                    u32 p0 = P565(w0 & 0xFF, (w0 >> 8) & 0xFF, (w0 >> 16) & 0xFF);
                    u32 p1 = P565(w0 >> 24, w1 & 0xFF, (w1 >> 8) & 0xFF);
                    u32 p2 = P565((w1 >> 16) & 0xFF, w1 >> 24, w2 & 0xFF);
                    u32 p3 = P565((w2 >> 8) & 0xFF, (w2 >> 16) & 0xFF, w2 >> 24);
                    d[0] = p0 | (p1 << 16);
                    d[1] = p2 | (p3 << 16);
                }
            } else {
                const u8 *rb = (const u8 *)row;
                u32 b = (u32)sx * 2;
                for (int x = 0; x < W; x++, b += 3)
                    dst[x] = (u16)P565(rb[b & 2047], rb[(b + 1) & 2047], rb[(b + 2) & 2047]);
            }
            if (direct_ok) sq_cpy((u8 *)tex + (u32)y * cur_tw * 2, line, (size_t)W * 2);
        }
#undef P565
        return;
    }
    int fast = !(sx & 1) && sx + W <= 1024;
    if (direct_ok && fast) {
        /* framebuffer rows are already in ARGB1555 channel order (gpu.c): straight copy */
        for (int y = 0; y < H; y++)
            sq_cpy((u8 *)tex + (u32)y * cur_tw * 2, &g->vram[(((g->disp_y + y) & 511) << 10) + sx], (size_t)W * 2);
        return;
    }
    if (direct_ok) {
        /* odd display x or wrap: convert row by row */
        for (int y = 0; y < H; y++) {
            const u16 *src = &g->vram[((g->disp_y + y) & 511) << 10];
            for (int x = 0; x < W; x++) line[x] = src[(sx + x) & 1023];
            sq_cpy((u8 *)tex + (u32)y * cur_tw * 2, line, (size_t)W * 2);
        }
        return;
    }
    if (fast) {
        for (int y = 0; y < H; y++) {
            const u32 *src = (const u32 *)&g->vram[(((g->disp_y + y) & 511) << 10) + sx];
            u32 *dst = (u32 *)(txbuf + y * cur_tw);
            for (int x = 0; x < W; x += 2) *dst++ = *src++;
        }
    } else {
        for (int y = 0; y < H; y++) {
            const u16 *src = &g->vram[((g->disp_y + y) & 511) << 10];
            u16 *dst = txbuf + y * cur_tw;
            for (int x = 0; x < W; x++) dst[x] = src[(sx + x) & 1023];
        }
    }
}

/* HUD drawing target (ARGB1555, stride = cur_tw): the texture itself when written directly */
u16 *display_hud_buffer(int *stride, int *w, int *h) {
    *stride = cur_tw; *w = cur_w; *h = cur_h;
    return direct_ok ? (u16 *)tex : txbuf;
}

/* Draw the quad (the texture already holds the frame). */
void display_end(void) {
    if (!direct_ok) {
        pvr_txr_load(txbuf, tex, (size_t)cur_tw * cur_h * 2);
    }

    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    if (cur_24) pvr_prim(cur_tw == 1024 ? &hdr1024_565 : &hdr512_565, sizeof(pvr_poly_hdr_t));
    else pvr_prim(cur_tw == 1024 ? &hdr1024 : &hdr512, sizeof(pvr_poly_hdr_t));

    float u1 = (float)cur_w / (float)cur_tw;
    float v1 = (float)cur_h / (float)cur_th;
    pvr_vertex_t v;
    v.flags = PVR_CMD_VERTEX; v.argb = 0xFFFFFFFF; v.oargb = 0; v.z = 1.0f;
    v.x = 0.0f;   v.y = 0.0f;   v.u = 0.0f; v.v = 0.0f; pvr_prim(&v, sizeof(v));
    v.x = 640.0f; v.y = 0.0f;   v.u = u1;   v.v = 0.0f; pvr_prim(&v, sizeof(v));
    v.x = 0.0f;   v.y = 480.0f; v.u = 0.0f; v.v = v1;   pvr_prim(&v, sizeof(v));
    v.flags = PVR_CMD_VERTEX_EOL;
    v.x = 640.0f; v.y = 480.0f; v.u = u1;   v.v = v1;   pvr_prim(&v, sizeof(v));

    pvr_list_finish();
    pvr_scene_finish();
}
