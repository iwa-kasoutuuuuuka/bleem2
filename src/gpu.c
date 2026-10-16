#include <kos.h>
#include "mips.h"
#include <stdio.h>
#include <string.h>

/*
 * PS1 GPU: software rasterizer into a native 15-bit 1024x512 VRAM.
 * Supports flat/gouraud/textured (4/8/15 bit + CLUT) polygons, rectangles,
 * flat lines, fills, VRAM transfers and the GP1 display/control commands.
 */

static u16 ps1_vram[1024 * 512] __attribute__((aligned(32)));

void gpu_reset(gpu_t *g) {
    g->stat_bits = 0;
    g->pkt_ptr = 0;
    g->pkt_size = 0;
    g->polyline_active = 0;
    g->xfer_dir = 0;
    g->gpuread = 0;
    g->draw_x0 = g->draw_y0 = 0;
    g->draw_x1 = g->draw_y1 = 0;
    g->off_x = g->off_y = 0;
    g->tex_window = 0;
    g->mask_set = g->mask_check = 0;
    g->disp_x = g->disp_y = 0;
    g->disp_enabled = 0;
    g->dma_dir = 0;
    g->disp_mode = 0;
    g->hrange_x1 = 0x200; g->hrange_x2 = 0xC00;
    g->vrange_y1 = 0x10;  g->vrange_y2 = 0x100;
    g->irq = 0;
}

void gpu_init(gpu_t *g) {
    if (!g) return;
    memset(g, 0, sizeof(*g));
    g->vram = ps1_vram;
    memset(g->vram, 0, 1024 * 512 * 2);
    gpu_reset(g);
    dlog("GPU: VRAM 1024x512 ready");
}

/* ------------------------------------------------------------------ */
/* pixel helpers                                                       */
/* ------------------------------------------------------------------ */

static inline u16 rgb24_to_15(u32 c) {        /* 24-bit RGB -> ARGB1555 channel order */
    return (u16)((((c >> 3) & 0x1F) << 10) | ((c >> 6) & 0x3E0) | ((c >> 19) & 0x1F));
}

/* PS1 semi-transparency: mode 0 = (B+F)/2, 1 = B+F, 2 = B-F, 3 = B+F/4 (lane-parallel) */
#define NOINLINE __attribute__((noinline))
#define LANES(c)   (((c) & 0x1F) | (((c) & 0x3E0) << 3) | (((c) & 0x7C00) << 6))
#define UNLANES(l) ((u16)(((l) & 0x1F) | (((l) >> 3) & 0x3E0) | (((l) >> 6) & 0x7C00)))
static inline u16 blend15(u16 b, u16 f, int mode) {
    u32 s;
    switch (mode) {
    case 0: return (u16)(((b & f & 0x7FFF) + ((((b ^ f) & 0x7FFF) >> 1) & 0x3DEF)) | (f & 0x8000));
    case 1: { u32 x = LANES(b) + LANES(f); u32 o = x & 0x202020; s = (x | (o - (o >> 5))) & 0x1F1F1F; break; }
    case 2: { u32 x = (LANES(b) | 0x202020) - LANES(f); u32 o = x & 0x202020; s = x & (o - (o >> 5)); break; }
    default: { u32 x = LANES(b) + ((LANES(f) >> 2) & 0x070707); u32 o = x & 0x202020; s = (x | (o - (o >> 5))) & 0x1F1F1F; break; }
    }
    return (u16)(UNLANES(s) | (f & 0x8000));
}

static inline void put_pixel(gpu_t *g, int x, int y, u16 c) {
    if (x < g->draw_x0 || x > g->draw_x1 || y < g->draw_y0 || y > g->draw_y1) return;
    u16 *p = &g->vram[((y & 511) << 10) + (x & 1023)];
    if (g->mask_check && (*p & 0x8000)) return;
    *p = c | (g->mask_set ? 0x8000 : 0);
}

/* texture page description resolved once per primitive.
 * Framebuffer pixels are kept in Dreamcast ARGB1555 channel order (R high, B low) so the
 * display needs no conversion; texture data uploaded by the game stays in PS1 order and is
 * swapped when sampled: CLUTs through a small cache of swapped copies, 15-bit texels inline. */
#define SWAP_RB(c) ((u16)(((c) & 0x83E0) | (((c) & 0x1F) << 10) | (((c) >> 10) & 0x1F)))
typedef struct {
    int depth;          /* 0 = 4-bit, 1 = 8-bit, 2 = 15-bit */
    int tx2;            /* page x in bytes (tx*2) */
    int ty;             /* page y (0 or 256) */
    const u16 *clut;    /* swapped CLUT copy (16 or 256 entries) */
} texpage_t;

#define CLUT_CACHE 8
static u16 clut_cache[CLUT_CACHE][256] __attribute__((aligned(32)));
static u32 clut_key[CLUT_CACHE];
static int clut_next = 0;
static int clut_dirty = 1;             /* set whenever VRAM is written outside the rasterizer */

static inline void texpage_setup(gpu_t *g, u16 tpage, u16 clut, texpage_t *t) {
    t->depth = (tpage >> 7) & 3; if (t->depth > 2) t->depth = 2;
    t->tx2 = (tpage & 0xF) * 128;
    t->ty = ((tpage >> 4) & 1) * 256;
    t->clut = NULL;
    if (t->depth >= 2) return;
    if (clut_dirty) { memset(clut_key, 0, sizeof(clut_key)); clut_dirty = 0; }
    u32 key = 0x80000000u | ((u32)clut << 1) | (u32)t->depth;
    for (int i = 0; i < CLUT_CACHE; i++) if (clut_key[i] == key) { t->clut = clut_cache[i]; return; }
    int slot = clut_next; clut_next = (clut_next + 1) % CLUT_CACHE;
    const u16 *src = &g->vram[((clut >> 6) & 0x1FF) * 1024];
    int cx = (clut & 0x3F) * 16, n = t->depth ? 256 : 16;
    for (int k = 0; k < n; k++) clut_cache[slot][k] = SWAP_RB(src[(cx + k) & 1023]);
    clut_key[slot] = key;
    t->clut = clut_cache[slot];
}

static inline u16 tex_sample(gpu_t *g, const texpage_t *t, int u, int v) {
    u &= 255; v &= 255;
    const u16 *row = &g->vram[(t->ty + v) << 10];
    switch (t->depth) {
    case 0: {
        u32 b = ((const u8 *)row)[(t->tx2 + (u >> 1)) & 2047];
        return t->clut[(u & 1) ? (int)(b >> 4) : (int)(b & 15)];
    }
    case 1:
        return t->clut[((const u8 *)row)[(t->tx2 + u) & 2047]];
    default:
        return SWAP_RB(row[((t->tx2 >> 1) + u) & 1023]);
    }
}

/* legacy entry used by other code paths */
static inline u16 sample_tex(gpu_t *g, int u, int v, u16 tpage, u16 clut) {
    texpage_t t; texpage_setup(g, tpage, clut, &t);
    return tex_sample(g, &t, u, v);
}

/* ------------------------------------------------------------------ */
/* primitives                                                          */
/* ------------------------------------------------------------------ */

typedef struct { int x, y; int r, g, b; int u, v; } vtx_t;

static inline int imin3(int a, int b, int c) { int m = a < b ? a : b; return m < c ? m : c; }
static inline int imax3(int a, int b, int c) { int m = a > b ? a : b; return m > c ? m : c; }

/* 16.16 x-gradient of attribute (pa,pb,pc) over the triangle */
static inline int plane_dx(int pa, int pb, int pc, int A0, int A1, int A2, int area) {
    int64_t nx = (int64_t)pa * A0 + (int64_t)pb * A1 + (int64_t)pc * A2;
    return (int)((nx << 16) / area);
}
static inline int plane_at(int pa, int pb, int pc, int w0, int w1, int w2, int area) {
    int64_t n = (int64_t)pa * w0 + (int64_t)pb * w1 + (int64_t)pc * w2;
    return (int)((n << 16) / area);
}

u32 g_px_rect = 0, g_px_tri = 0;
u32 g_xfer_hw = 0, g_px_semi = 0, g_px_copy = 0, g_px_fill = 0;
static void NOINLINE draw_triangle(gpu_t *g, const vtx_t *v0, const vtx_t *v1, const vtx_t *v2,
                          int textured, int gouraud, u16 tpage, u16 clut, u16 flat15, int semi) {
    int smode = (tpage >> 5) & 3;
    const vtx_t *a = v0, *b = v1, *c = v2;
    int area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    if (area == 0) return;
    if (area < 0) { const vtx_t *t = b; b = c; c = t; area = -area; }

    int minx = imin3(a->x, b->x, c->x), maxx = imax3(a->x, b->x, c->x);
    int miny = imin3(a->y, b->y, c->y), maxy = imax3(a->y, b->y, c->y);
    if (maxx - minx > 1023 || maxy - miny > 511) return;
    if (minx < g->draw_x0) minx = g->draw_x0;
    if (miny < g->draw_y0) miny = g->draw_y0;
    if (maxx > g->draw_x1) maxx = g->draw_x1;
    if (maxy > g->draw_y1) maxy = g->draw_y1;
    if (minx > maxx || miny > maxy) return;

    /* edge function coefficients: w = A*x + B*y + C */
    int A0 = b->y - c->y, B0 = c->x - b->x, C0 = b->x * c->y - c->x * b->y;
    int A1 = c->y - a->y, B1 = a->x - c->x, C1 = c->x * a->y - a->x * c->y;
    int A2 = a->y - b->y, B2 = b->x - a->x, C2 = a->x * b->y - b->x * a->y;

    g->polys++;
    g->frame_polys++;
    g_px_tri += (u32)(maxx - minx + 1) * (u32)(maxy - miny + 1) / 2;

    const u16 maskset = g->mask_set ? 0x8000 : 0;
    const int maskchk = g->mask_check;
    texpage_t tp;
    int du = 0, dv = 0, dr = 0, dg = 0, db = 0;
    if (textured) {
        texpage_setup(g, tpage, clut, &tp);
        du = plane_dx(a->u, b->u, c->u, A0, A1, A2, area);
        dv = plane_dx(a->v, b->v, c->v, A0, A1, A2, area);
    } else if (gouraud) {
        dr = plane_dx(a->r, b->r, c->r, A0, A1, A2, area);
        dg = plane_dx(a->g, b->g, c->g, A0, A1, A2, area);
        db = plane_dx(a->b, b->b, c->b, A0, A1, A2, area);
    }
    /* row-start values at (minx, miny) and their y gradients */
    int u_row = 0, v_row = 0, r_row = 0, g_row = 0, b_row = 0, duy = 0, dvy = 0, dry = 0, dgy = 0, dby = 0;
    {
        int w0 = A0 * minx + B0 * miny + C0, w1 = A1 * minx + B1 * miny + C1, w2 = A2 * minx + B2 * miny + C2;
        if (textured) {
            u_row = plane_at(a->u, b->u, c->u, w0, w1, w2, area) + 0x8000;
            v_row = plane_at(a->v, b->v, c->v, w0, w1, w2, area) + 0x8000;
            duy = plane_dx(a->u, b->u, c->u, B0, B1, B2, area);
            dvy = plane_dx(a->v, b->v, c->v, B0, B1, B2, area);
        } else if (gouraud) {
            r_row = plane_at(a->r, b->r, c->r, w0, w1, w2, area);
            g_row = plane_at(a->g, b->g, c->g, w0, w1, w2, area);
            b_row = plane_at(a->b, b->b, c->b, w0, w1, w2, area);
            dry = plane_dx(a->r, b->r, c->r, B0, B1, B2, area);
            dgy = plane_dx(a->g, b->g, c->g, B0, B1, B2, area);
            dby = plane_dx(a->b, b->b, c->b, B0, B1, B2, area);
        }
    }

    for (int y = miny; y <= maxy; y++) {
        int w0 = A0 * minx + B0 * y + C0;
        int w1 = A1 * minx + B1 * y + C1;
        int w2 = A2 * minx + B2 * y + C2;
        u16 *row = &g->vram[(y & 511) << 10];
        /* find the covered span on this row (skip leading outside pixels) */
        int x = minx;
        while (x <= maxx && (w0 | w1 | w2) < 0) { x++; w0 += A0; w1 += A1; w2 += A2; }
        if (x > maxx) continue;
        if (textured) {
            int u = u_row + duy * (y - miny) + du * (x - minx);
            int v = v_row + dvy * (y - miny) + dv * (x - minx);
            for (; x <= maxx && (w0 | w1 | w2) >= 0; x++, w0 += A0, w1 += A1, w2 += A2, u += du, v += dv) {
                u16 col = tex_sample(g, &tp, u >> 16, v >> 16);
                if (col == 0) continue;
                u16 *p = &row[x & 1023];
                if (maskchk && (*p & 0x8000)) continue;
                if (semi && (col & 0x8000)) col = blend15(*p, col, smode);
                *p = col | maskset;
            }
        } else if (gouraud) {
            int r = r_row + dry * (y - miny) + dr * (x - minx);
            int gg = g_row + dgy * (y - miny) + dg * (x - minx);
            int bb = b_row + dby * (y - miny) + db * (x - minx);
            for (; x <= maxx && (w0 | w1 | w2) >= 0; x++, w0 += A0, w1 += A1, w2 += A2, r += dr, gg += dg, bb += db) {
                u16 col = (u16)(((r >> 19) << 10) | ((gg >> 19) << 5) | (bb >> 19));
                u16 *p = &row[x & 1023];
                if (maskchk && (*p & 0x8000)) continue;
                if (semi) col = blend15(*p, col, smode);
                *p = col | maskset;
            }
        } else {
            for (; x <= maxx && (w0 | w1 | w2) >= 0; x++, w0 += A0, w1 += A1, w2 += A2) {
                u16 *p = &row[x & 1023];
                if (maskchk && (*p & 0x8000)) continue;
                u16 col = flat15;
                if (semi) col = blend15(*p, col, smode);
                *p = col | maskset;
            }
        }
    }
}

/* semi-transparency with a per-primitive constant or per-pixel foreground */
typedef struct { int mode; u32 fl; u32 f2; u16 fa; } semi_t;
static inline void semi_setup(semi_t *s, int mode, u16 f) {
    s->mode = mode; s->fa = f & 0x8000;
    s->fl = (mode == 3) ? ((LANES(f) >> 2) & 0x070707) : LANES(f);
    s->f2 = ((u32)(f & 0x7FFF) << 16) | (f & 0x7FFF);
}
/* constant foreground, one background pixel */
static inline u16 semi_px(const semi_t *s, u16 b) {
    u32 x, o;
    switch (s->mode) {
    case 0: return (u16)(((b & s->f2 & 0x7FFF) + ((((b ^ s->f2) & 0x7FFF) >> 1) & 0x3DEF)) | s->fa);
    case 2: x = (LANES(b) | 0x202020) - s->fl; o = x & 0x202020; return (u16)(UNLANES(x & (o - (o >> 5))) | s->fa);
    default: x = LANES(b) + s->fl; o = x & 0x202020; return (u16)(UNLANES((x | (o - (o >> 5))) & 0x1F1F1F) | s->fa);
    }
}
/* constant foreground, mode 0 (average), two packed background pixels */
/* cached 32K-entry blend table for flat semi-transparent rects in modes 1-3 (constant foreground) */
static u16 semi_lut[32768] __attribute__((aligned(32)));
static u32 semi_lut_key = 0;
static const u16 *semi_lut_get(const semi_t *s) {
    u32 key = 0x80000000u | ((u32)s->mode << 16) | (s->f2 & 0xFFFFu);
    if (key != semi_lut_key) { for (u32 b = 0; b < 32768; b++) semi_lut[b] = semi_px(s, (u16)b); semi_lut_key = key; }
    return semi_lut;
}
static inline u32 semi_px2_avg(const semi_t *s, u32 b2) {
    b2 &= 0x7FFF7FFF;
    return ((b2 & s->f2) + (((b2 ^ s->f2) >> 1) & 0x3DEF3DEF)) | (s->fa ? 0x80008000u : 0);
}

/* Row blitters kept out of line so their loops run entirely in registers
 * (inlined into the large GP0 dispatcher they were spilling to the stack). */
static void NOINLINE __attribute__((unused)) blit4(u16 *p, const u8 *t, const u16 *cl, int n, int odd, u16 maskset) {
    int i = 0;
    if (odd) { u16 c = cl[*t++ >> 4]; if (c) *p = c | maskset; p++; i = 1; }
    for (; i + 1 < n; i += 2, p += 2, t++) {
        u32 b = *t; u16 c0 = cl[b & 15], c1 = cl[b >> 4];
        if (c0) p[0] = c0 | maskset;
        if (c1) p[1] = c1 | maskset;
    }
    if (i < n) { u16 c = cl[*t & 15]; if (c) *p = c | maskset; }
}
static void NOINLINE blit4_semi(u16 *p, const u8 *t, const u16 *cl, int n, int odd, u16 maskset, int smode) {
    int i = 0;
    if (odd) { u16 c = cl[*t++ >> 4]; if (c) { if (c & 0x8000) c = blend15(*p, c, smode); *p = c | maskset; } p++; i = 1; }
    for (; i + 1 < n; i += 2, p += 2, t++) {
        u32 b = *t; u16 c0 = cl[b & 15], c1 = cl[b >> 4];
        if (c0) { if (c0 & 0x8000) c0 = blend15(p[0], c0, smode); p[0] = c0 | maskset; }
        if (c1) { if (c1 & 0x8000) c1 = blend15(p[1], c1, smode); p[1] = c1 | maskset; }
    }
    if (i < n) { u16 c = cl[*t & 15]; if (c) { if (c & 0x8000) c = blend15(*p, c, smode); *p = c | maskset; } }
}
static void NOINLINE blit8(u16 *p, const u8 *t, const u16 *cl, int n, u16 maskset) {
    for (int i = 0; i < n; i++) { u16 c = cl[t[i]]; if (c) p[i] = c | maskset; }
}
static void NOINLINE blit8_semi(u16 *p, const u8 *t, const u16 *cl, int n, u16 maskset, int smode) {
    for (int i = 0; i < n; i++) { u16 c = cl[t[i]]; if (c) { if (c & 0x8000) c = blend15(p[i], c, smode); p[i] = c | maskset; } }
}
static void NOINLINE blit16(u16 *p, const u16 *t, int n, u16 maskset) {
    for (int i = 0; i < n; i++) { u16 c = SWAP_RB(t[i]); if (c) p[i] = c | maskset; }
}
static void NOINLINE blit16_semi(u16 *p, const u16 *t, int n, u16 maskset, int smode) {
    for (int i = 0; i < n; i++) { u16 c = SWAP_RB(t[i]); if (c) { if (c & 0x8000) c = blend15(p[i], c, smode); p[i] = c | maskset; } }
}

/* whole-sprite blitters: rows that neither wrap in VRAM nor in the texture page */
static void NOINLINE blit4_rows(u16 *dst, const u8 *tex, const u16 *cl, int n, int rows, int odd, u16 maskset) {
    for (; rows > 0; rows--, dst += 1024, tex += 2048) {
        __builtin_prefetch(dst + 1024); __builtin_prefetch(dst + 1024 + 16); __builtin_prefetch(tex + 2048);
        u16 *p = dst; const u8 *t = tex; int i = 0;
        if (odd) { u16 c = cl[*t++ >> 4]; if (c) *p = c | maskset; p++; i = 1; }
        for (; i + 1 < n; i += 2, p += 2, t++) {
            u32 b = *t; u16 c0 = cl[b & 15], c1 = cl[b >> 4];
            if (c0) p[0] = c0 | maskset;
            if (c1) p[1] = c1 | maskset;
        }
        if (i < n) { u16 c = cl[*t & 15]; if (c) *p = c | maskset; }
    }
}
static void NOINLINE blit8_rows(u16 *dst, const u8 *tex, const u16 *cl, int n, int rows, u16 maskset) {
    for (; rows > 0; rows--, dst += 1024, tex += 2048)
    {   __builtin_prefetch(dst + 1024); __builtin_prefetch(dst + 1024 + 16); __builtin_prefetch(tex + 2048); __builtin_prefetch(tex + 2048 + 32);
        for (int i = 0; i < n; i++) { u16 c = cl[tex[i]]; if (c) dst[i] = c | maskset; }
    }
}
static void NOINLINE blit16_rows(u16 *dst, const u16 *tex, int n, int rows, u16 maskset) {
    for (; rows > 0; rows--, dst += 1024, tex += 1024)
        for (int i = 0; i < n; i++) { u16 c = SWAP_RB(tex[i]); if (c) dst[i] = c | maskset; }
}

/* 4-bit sprite rows with a 32-bit CLUT copy (no sign/zero extension in the loop) */
#define BLIT4_BODY(STORE)                                                              \
    for (; rows > 0; rows--, dst += 1024, tex += 2048) {                               \
        __builtin_prefetch(dst + 1024); __builtin_prefetch(dst + 1024 + 16); __builtin_prefetch(tex + 2048); \
        u16 *p = dst; const u8 *t = tex; int i = 0;                                    \
        if (odd) { u32 c = cl[*t++ >> 4]; if (c) STORE(p[0], c); p++; i = 1; }          \
        for (; i + 1 < n; i += 2, p += 2, t++) {                                       \
            u32 b = *t; u32 c0 = cl[b & 15], c1 = cl[b >> 4];                          \
            if (c0) STORE(p[0], c0);                                                   \
            if (c1) STORE(p[1], c1);                                                   \
        }                                                                              \
        if (i < n) { u32 c = cl[*t & 15]; if (c) STORE(p[0], c); }                      \
    }
#define ST_PLAIN(d, c) ((d) = (u16)(c))
#define ST_MASK(d, c)  ((d) = (u16)((c) | maskset))
/* 4-bit sprite rows: destination-aligned pixel pairs stored as one 32-bit word when both are opaque */
static void NOINLINE blit4_rows32(u16 *dst, const u8 *tex, const u32 *cl, int n, int rows, int odd) {
    for (; rows > 0; rows--, dst += 1024, tex += 2048) {
        __builtin_prefetch(dst + 1024); __builtin_prefetch(dst + 1024 + 16); __builtin_prefetch(tex + 2048);
        u16 *p = dst; const u8 *t = tex; int i = 0; int o = odd;      /* o: next texel is the high nibble of *t */
        if ((uintptr_t)p & 2) {                                        /* align the destination to 4 bytes */
            u32 c = o ? cl[*t++ >> 4] : cl[*t & 15];
            o ^= 1; if (c) *p = (u16)c; p++; i = 1;
        }
        if (!o) {
            for (; i + 1 < n; i += 2, p += 2, t++) {
                u32 b = *t; u32 c0 = cl[b & 15], c1 = cl[b >> 4];
                if (c0 && c1) *(u32 *)p = c0 | (c1 << 16);
                else { if (c0) p[0] = (u16)c0; if (c1) p[1] = (u16)c1; }
            }
            if (i < n) { u32 c = cl[*t & 15]; if (c) *p = (u16)c; }
        } else {
            for (; i + 1 < n; i += 2, p += 2, t++) {
                u32 c0 = cl[t[0] >> 4], c1 = cl[t[1] & 15];
                if (c0 && c1) *(u32 *)p = c0 | (c1 << 16);
                else { if (c0) p[0] = (u16)c0; if (c1) p[1] = (u16)c1; }
            }
            if (i < n) { u32 c = cl[*t >> 4]; if (c) *p = (u16)c; }
        }
    }
}
static void NOINLINE blit4_rows32m(u16 *dst, const u8 *tex, const u32 *cl, int n, int rows, int odd, u16 maskset) {
    BLIT4_BODY(ST_MASK)
}

#define RECT_TEX_LOOP(FETCH)                                                        \
    do {                                                                            \
        for (int i = i0; i < i1; i++) {                                             \
            u16 col = (FETCH);                                                      \
            if (col == 0) continue;                                                 \
            u16 *p = &row[(x + i) & 1023];                                          \
            if (maskchk && (*p & 0x8000)) continue;                                 \
            if (semi && (col & 0x8000)) col = blend15(*p, col, smode);              \
            *p = col | maskset;                                                     \
        }                                                                           \
    } while (0)

#define RECT_TEX_FAST(FETCH)                                                        \
    do {                                                                            \
        u16 *p = &row[x + i0];                                                      \
        if (!semi) {                                                                \
            for (int i = 0; i < n; i++, p++) { u16 col = (FETCH); if (col) *p = col | maskset; } \
        } else {                                                                    \
            for (int i = 0; i < n; i++, p++) {                                      \
                u16 col = (FETCH);                                                  \
                if (col == 0) continue;                                             \
                if (col & 0x8000) col = blend15(*p, col, smode);                    \
                *p = col | maskset;                                                 \
            }                                                                       \
        }                                                                           \
    } while (0)

static void NOINLINE draw_rect(gpu_t *g, int x, int y, int w, int h, int textured, int u0, int v0,
                      u16 tpage, u16 clut, u16 flat15, int semi) {
    if (w > 1023 || h > 511) return;
    int smode = (tpage >> 5) & 3;
    const u16 maskset = g->mask_set ? 0x8000 : 0;
    const int maskchk = g->mask_check;
    /* clip to the drawing area */
    int i0 = 0, i1 = w, j0 = 0, j1 = h;
    if (x < g->draw_x0) i0 = g->draw_x0 - x;
    if (x + w - 1 > g->draw_x1) i1 = g->draw_x1 - x + 1;
    if (y < g->draw_y0) j0 = g->draw_y0 - y;
    if (y + h - 1 > g->draw_y1) j1 = g->draw_y1 - y + 1;
    if (i0 >= i1 || j0 >= j1) return;
    const int n = i1 - i0;
    g_px_rect += (u32)n * (u32)(j1 - j0);
    if (semi) g_px_semi += (u32)n * (u32)(j1 - j0);
    /* destination span stays inside one VRAM row without wrapping? */
    const int xs = x + i0;
    const int dst_fast = !maskchk && xs >= 0 && xs + n <= 1024;
    g->polys++;

    if (textured) {
        texpage_t tp;
        texpage_setup(g, tpage, clut, &tp);
        const int us = u0 + i0;
        /* source fast path: no u wrap, no texture row wrap, no CLUT wrap */
        int src_fast = dst_fast && us >= 0 && us + n <= 256;
        if (tp.depth == 1) src_fast = src_fast && tp.tx2 + 256 <= 2048;
        else if (tp.depth == 0) src_fast = src_fast && 1;                             /* 4-bit rows never wrap (tx2+128 <= 2048) */
        else src_fast = src_fast && (tp.tx2 >> 1) + 256 <= 1024;
        {   /* whole sprite at once when no row wraps (the common case) */
            int rows = j1 - j0, vy = (v0 + j0) & 255, yy = (y + j0) & 511;
            if (src_fast && !semi && yy + rows <= 512 && vy + rows <= 256) {
                u16 *dst = &g->vram[(yy << 10) + xs];
                const u8 *tex = (const u8 *)&g->vram[(tp.ty + vy) << 10] + tp.tx2;
                if (tp.depth == 0) {
                    u32 cl32[16]; const u16 *cl = tp.clut;
                    for (int k = 0; k < 16; k++) cl32[k] = cl[k];
                    if (maskset) blit4_rows32m(dst, tex + (us >> 1), cl32, n, rows, us & 1, maskset);
                    else blit4_rows32(dst, tex + (us >> 1), cl32, n, rows, us & 1);
                }
                else if (tp.depth == 1) blit8_rows(dst, tex + us, tp.clut, n, rows, maskset);
                else blit16_rows(dst, (const u16 *)tex + us, n, rows, maskset);
                return;
            }
        }
        for (int j = j0; j < j1; j++) {
            u16 *row = &g->vram[((y + j) & 511) << 10];
            int v = (v0 + j) & 255;
            const u8 *trow = (const u8 *)&g->vram[(tp.ty + v) << 10];
            if (tp.depth == 1) {
                if (src_fast) {
                    if (semi) blit8_semi(&row[xs], trow + tp.tx2 + us, tp.clut, n, maskset, smode);
                    else blit8(&row[xs], trow + tp.tx2 + us, tp.clut, n, maskset);
                } else {
                    RECT_TEX_LOOP(tp.clut[trow[(tp.tx2 + ((u0 + i) & 255)) & 2047]]);
                }
            } else if (tp.depth == 0) {
                if (src_fast) {
                    if (semi) blit4_semi(&row[xs], trow + tp.tx2 + (us >> 1), tp.clut, n, us & 1, maskset, smode);
                    else blit4(&row[xs], trow + tp.tx2 + (us >> 1), tp.clut, n, us & 1, maskset);
                } else {
                    RECT_TEX_LOOP(({ int u = (u0 + i) & 255; u32 b = trow[(tp.tx2 + (u >> 1)) & 2047];
                                     tp.clut[(u & 1) ? (int)(b >> 4) : (int)(b & 15)]; }));
                }
            } else {
                const u16 *trow16 = (const u16 *)trow;
                if (src_fast) {
                    if (semi) blit16_semi(&row[xs], trow16 + (tp.tx2 >> 1) + us, n, maskset, smode);
                    else blit16(&row[xs], trow16 + (tp.tx2 >> 1) + us, n, maskset);
                } else {
                    RECT_TEX_LOOP(SWAP_RB(trow16[((tp.tx2 >> 1) + ((u0 + i) & 255)) & 1023]));
                }
            }
        }
        return;
    }

    if (semi) {
        semi_t s; semi_setup(&s, smode, flat15);
        for (int j = j0; j < j1; j++) {
            u16 *row = &g->vram[((y + j) & 511) << 10];
            if (dst_fast) {
                u16 *p = &row[xs];
                int i = 0;
                if (s.mode == 0) {
                    if ((xs & 1) && n > 0) { *p = semi_px(&s, *p) | maskset; p++; i = 1; }
                    u32 ms2 = maskset ? 0x80008000u : 0;
                    for (; i + 1 < n; i += 2, p += 2) *(u32 *)p = semi_px2_avg(&s, *(u32 *)p) | ms2;
                } else {
                    const u16 *lut = semi_lut_get(&s);
                    for (; i < n; i++, p++) *p = lut[*p & 0x7FFF] | maskset;
                }
                for (; i < n; i++, p++) *p = semi_px(&s, *p) | maskset;
            } else {
                for (int i = i0; i < i1; i++) {
                    u16 *p = &row[(x + i) & 1023];
                    if (maskchk && (*p & 0x8000)) continue;
                    *p = semi_px(&s, *p) | maskset;
                }
            }
        }
        return;
    }

    u16 col = flat15 | maskset;
    for (int j = j0; j < j1; j++) {
        u16 *row = &g->vram[((y + j) & 511) << 10];
        if (dst_fast) {
            u16 *p = &row[xs];
            int i = 0;
            if ((xs & 1) && n > 0) { *p++ = col; i = 1; }
            u32 c2 = ((u32)col << 16) | col;
            for (; i + 1 < n; i += 2, p += 2) *(u32 *)p = c2;
            if (i < n) *p = col;
        } else {
            for (int i = i0; i < i1; i++) {
                u16 *p = &row[(x + i) & 1023];
                if (maskchk && (*p & 0x8000)) continue;
                *p = col;
            }
        }
    }
}

static void draw_line(gpu_t *g, int x0, int y0, int x1, int y1, u16 col) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    if (dx > 1023 || dy > 511) return;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (int guard = 0; guard < 2048; guard++) {
        put_pixel(g, x0, y0, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

/* ------------------------------------------------------------------ */
/* GP0 command execution                                               */
/* ------------------------------------------------------------------ */

static int gp0_packet_size(gpu_t *g, u32 word) {
    u8 cmd = word >> 24;
    if (cmd == 0x02) return 3;
    if (cmd >= 0x20 && cmd <= 0x3F) {
        int quad = (cmd & 0x08) ? 1 : 0, tex = (cmd & 0x04) ? 1 : 0, gou = (cmd & 0x10) ? 1 : 0;
        int verts = quad ? 4 : 3;
        return verts * (1 + tex + gou) + (gou ? 0 : 1);
    }
    if (cmd >= 0x40 && cmd <= 0x5F) {
        if (cmd & 0x08) return 0;                 /* polyline: variable, terminated */
        return (cmd & 0x10) ? 4 : 3;
    }
    if (cmd >= 0x60 && cmd <= 0x7F) {
        int tex = (cmd & 0x04) ? 1 : 0;
        int size = (cmd >> 3) & 3;
        return 2 + tex + (size == 0 ? 1 : 0);
    }
    if (cmd >= 0x80 && cmd <= 0x9F) return 4;
    if (cmd >= 0xA0 && cmd <= 0xBF) return 3;
    if (cmd >= 0xC0 && cmd <= 0xDF) return 3;
    return 1;
}

uint64_t prof_gpu_us = 0;

static void gp0_execute_inner(gpu_t *g);
static void gp0_execute(gpu_t *g) { gp0_execute_inner(g); }

static void gp0_execute_inner(gpu_t *g) {
    u32 *p = g->pkt;
    u8 cmd = p[0] >> 24;
    g->draw_calls++;

    if (cmd >= 0x20 && cmd <= 0x3F) {                       /* polygons */
        int quad = (cmd & 0x08) != 0, tex = (cmd & 0x04) != 0, gou = (cmd & 0x10) != 0;
        int verts = quad ? 4 : 3;
        vtx_t v[4];
        u16 clut = 0, tpage = g->tpage;
        int i = 0;
        u32 col = p[0] & 0xFFFFFF;
        for (int n = 0; n < verts; n++) {
            if (n > 0 && gou) col = p[i] & 0xFFFFFF;
            if (n == 0 || gou) i++;                           /* color word */
            u32 xy = p[i++];
            v[n].x = (s16)(xy & 0xFFFF) + g->off_x;
            v[n].y = (s16)(xy >> 16) + g->off_y;
            v[n].r = col & 0xFF; v[n].g = (col >> 8) & 0xFF; v[n].b = (col >> 16) & 0xFF;
            if (tex) {
                u32 uv = p[i++];
                v[n].u = uv & 0xFF; v[n].v = (uv >> 8) & 0xFF;
                if (n == 0) clut = uv >> 16;
                if (n == 1) tpage = uv >> 16;
            } else {
                v[n].u = v[n].v = 0;
            }
        }
        if (tex) {
            g->tpage = tpage;
            g->stat_bits = (g->stat_bits & ~0x7FFu) | (tpage & 0x7FF);
        }
        u16 flat15 = rgb24_to_15(p[0]);
        draw_triangle(g, &v[0], &v[1], &v[2], tex, gou, tpage, clut, flat15, (cmd & 2) != 0);
        if (quad) draw_triangle(g, &v[1], &v[2], &v[3], tex, gou, tpage, clut, flat15, (cmd & 2) != 0);
        return;
    }

    if (cmd >= 0x60 && cmd <= 0x7F) {                       /* rectangles */
        int tex = (cmd & 0x04) != 0;
        int size = (cmd >> 3) & 3;
        int i = 1;
        u32 xy = p[i++];
        int x = (s16)(xy & 0xFFFF) + g->off_x;
        int y = (s16)(xy >> 16) + g->off_y;
        int u0 = 0, v0 = 0; u16 clut = 0;
        if (tex) { u32 uv = p[i++]; u0 = uv & 0xFF; v0 = (uv >> 8) & 0xFF; clut = uv >> 16; }
        int w, h;
        if (size == 0) { w = p[i] & 0x3FF; h = (p[i] >> 16) & 0x1FF; }
        else if (size == 1) { w = h = 1; }
        else if (size == 2) { w = h = 8; }
        else { w = h = 16; }
        draw_rect(g, x, y, w, h, tex, u0, v0, g->tpage, clut, rgb24_to_15(p[0]), (cmd & 2) != 0);
        return;
    }

    switch (cmd) {
    case 0x02: {                                             /* fill rectangle */
        u16 c = rgb24_to_15(p[0]);
        int x = p[1] & 0x3F0, y = (p[1] >> 16) & 0x1FF;
        int w = ((p[2] & 0x3FF) + 0xF) & ~0xF, h = (p[2] >> 16) & 0x1FF;
        g_px_fill += (u32)w * (u32)h;
        clut_dirty = 1;
        u32 c2 = ((u32)c << 16) | c;
        for (int j = 0; j < h; j++) {
            u32 *row = (u32 *)&g->vram[((y + j) & 511) << 10];
            for (int i = 0; i < (w >> 1); i++) row[((x >> 1) + i) & 511] = c2;
        }
        break;
    }
    case 0x1F:
        g->irq = 1;
        break;
    case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F:
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
    case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F: {
        int sx = p[1] & 0x3FF, sy = (p[1] >> 16) & 0x1FF;
        int dx = p[2] & 0x3FF, dy = (p[2] >> 16) & 0x1FF;
        int w = (((p[3] & 0x3FF) - 1) & 0x3FF) + 1, h = ((((p[3] >> 16) & 0x1FF) - 1) & 0x1FF) + 1;
        g_px_copy += (u32)w * (u32)h;
        clut_dirty = 1;
        if (sx + w <= 1024 && dx + w <= 1024) {
            /* rows do not wrap horizontally: move whole rows (overlap-safe) */
            for (int j = 0; j < h; j++)
                memmove(&g->vram[((dy + j) & 511) * 1024 + dx], &g->vram[((sy + j) & 511) * 1024 + sx], (size_t)w * 2);
        } else {
            for (int j = 0; j < h; j++)
                for (int i = 0; i < w; i++)
                    g->vram[((dy + j) & 511) * 1024 + ((dx + i) & 1023)] =
                        g->vram[((sy + j) & 511) * 1024 + ((sx + i) & 1023)];
        }
        break;
    }
    case 0xE1:
        g->tpage = p[0] & 0x7FF;
        g->stat_bits = (g->stat_bits & ~0x87FFu) | (p[0] & 0x7FF) | ((p[0] & 0x800) ? 0x8000 : 0);
        break;
    case 0xE2: g->tex_window = p[0] & 0xFFFFF; break;
    case 0xE3: g->draw_x0 = p[0] & 0x3FF; g->draw_y0 = (p[0] >> 10) & 0x1FF; break;
    case 0xE4: g->draw_x1 = p[0] & 0x3FF; g->draw_y1 = (p[0] >> 10) & 0x1FF; break;
    case 0xE5: {
        int ox = p[0] & 0x7FF, oy = (p[0] >> 11) & 0x7FF;
        g->off_x = (ox & 0x400) ? (ox - 0x800) : ox;
        g->off_y = (oy & 0x400) ? (oy - 0x800) : oy;
        break;
    }
    case 0xE6:
        g->mask_set = p[0] & 1;
        g->mask_check = (p[0] >> 1) & 1;
        g->stat_bits = (g->stat_bits & ~0x1800u) | ((p[0] & 3) << 11);
        break;
    default:
        if (cmd >= 0xA0 && cmd <= 0xBF) {                    /* CPU -> VRAM */
            g->xfer_x = p[1] & 0x3FF; g->xfer_y = (p[1] >> 16) & 0x1FF;
            g->xfer_w = (((p[2] & 0x3FF) - 1) & 0x3FF) + 1;
            g->xfer_h = ((((p[2] >> 16) & 0x1FF) - 1) & 0x1FF) + 1;
            g->xfer_idx = 0;
            g->xfer_dir = 1;
        } else if (cmd >= 0xC0 && cmd <= 0xDF) {             /* VRAM -> CPU */
            g->xfer_x = p[1] & 0x3FF; g->xfer_y = (p[1] >> 16) & 0x1FF;
            g->xfer_w = (((p[2] & 0x3FF) - 1) & 0x3FF) + 1;
            g->xfer_h = ((((p[2] >> 16) & 0x1FF) - 1) & 0x1FF) + 1;
            g->xfer_idx = 0;
            g->xfer_dir = 2;
        } else if (cmd >= 0x40 && cmd <= 0x5F) {             /* single line */
            u16 c = rgb24_to_15(p[0]);
            int gou = (cmd & 0x10) != 0;
            u32 a = p[1], b = gou ? p[3] : p[2];
            draw_line(g, (s16)(a & 0xFFFF) + g->off_x, (s16)(a >> 16) + g->off_y,
                      (s16)(b & 0xFFFF) + g->off_x, (s16)(b >> 16) + g->off_y, c);
        }
        break;
    }
}

static int xf_cx, xf_cy, xf_last = -1;          /* incremental transfer cursor (avoids a divide per halfword) */
static inline void xfer_cursor(gpu_t *g) {
    if (g->xfer_idx != xf_last) { xf_cx = g->xfer_idx % g->xfer_w; xf_cy = g->xfer_idx / g->xfer_w; }
}
static inline void xfer_step(gpu_t *g) {
    g->xfer_idx++;
    if (++xf_cx >= g->xfer_w) { xf_cx = 0; xf_cy++; }
    xf_last = g->xfer_idx;
}
static inline void xfer_write_halfword(gpu_t *g, u16 v) {
    int total = g->xfer_w * g->xfer_h;
    if (g->xfer_idx >= total) return;
    g_xfer_hw++;
    xfer_cursor(g);
    int x = (g->xfer_x + xf_cx) & 1023;
    int y = (g->xfer_y + xf_cy) & 511;
    clut_dirty = 1;
    g->vram[(y << 10) + x] = v;
    xfer_step(g);
}

void gpu_write_data(gpu_t *g, u32 val) {
    if (g->xfer_dir == 1) {
        xfer_write_halfword(g, val & 0xFFFF);
        xfer_write_halfword(g, val >> 16);
        if (g->xfer_idx >= g->xfer_w * g->xfer_h) g->xfer_dir = 0;
        return;
    }
    if (g->polyline_active) {
        if ((val & 0xF000F000) == 0x50005000) { g->polyline_active = 0; g->pkt_ptr = 0; return; }
        /* draw segment from previous vertex */
        u8 cmd = g->pkt[0] >> 24;
        int gou = (cmd & 0x10) != 0;
        if (gou) {
            /* alternating color / vertex words */
            if (g->pkt_ptr & 1) { g->pkt[2] = val; }
            else {
                u32 a = g->pkt[1], b = g->pkt[2];
                if (g->pkt_ptr >= 3)
                    draw_line(g, (s16)(a & 0xFFFF) + g->off_x, (s16)(a >> 16) + g->off_y,
                              (s16)(val & 0xFFFF) + g->off_x, (s16)(val >> 16) + g->off_y,
                              rgb24_to_15(g->pkt[0]));
                (void)b;
                g->pkt[1] = val;
            }
            g->pkt_ptr++;
        } else {
            u32 a = g->pkt[1];
            if (g->pkt_ptr >= 2)
                draw_line(g, (s16)(a & 0xFFFF) + g->off_x, (s16)(a >> 16) + g->off_y,
                          (s16)(val & 0xFFFF) + g->off_x, (s16)(val >> 16) + g->off_y,
                          rgb24_to_15(g->pkt[0]));
            g->pkt[1] = val;
            g->pkt_ptr++;
        }
        return;
    }
    if (g->pkt_ptr == 0) {
        g->pkt_size = gp0_packet_size(g, val);
        if (g->pkt_size == 0) {               /* polyline start */
            g->pkt[0] = val;
            g->pkt_ptr = 1;
            g->polyline_active = 1;
            return;
        }
    }
    if (g->pkt_ptr < 16) g->pkt[g->pkt_ptr] = val;
    g->pkt_ptr++;
    if (g->pkt_ptr >= g->pkt_size) {
        gp0_execute(g);
        g->pkt_ptr = 0;
    }
}

/* DMA fast path: consume whole packets straight from the list buffer */
void gpu_write_words(gpu_t *g, const u32 *w, int n) {
    int i = 0;
    while (i < n) {
        if (g->pkt_ptr != 0 || g->xfer_dir == 1 || g->polyline_active) { gpu_write_data(g, w[i++]); continue; }
        int size = gp0_packet_size(g, w[i]);
        if (size <= 0 || size > 16 || i + size > n) { gpu_write_data(g, w[i++]); continue; }
        memcpy(g->pkt, &w[i], (size_t)size * 4);
        g->pkt_size = size;
        gp0_execute(g);
        g->pkt_ptr = 0;
        i += size;
    }
}

void gpu_write_command(gpu_t *g, u32 val) { gpu_write_data(g, val); }

u32 gpu_read_data(gpu_t *g) {
    if (g->xfer_dir == 2) {
        u32 v = 0;
        for (int k = 0; k < 2; k++) {
            int total = g->xfer_w * g->xfer_h;
            u16 px = 0;
            if (g->xfer_idx < total) {
                xfer_cursor(g);
                int x = (g->xfer_x + xf_cx) & 1023;
                int y = (g->xfer_y + xf_cy) & 511;
                px = g->vram[(y << 10) + x];
                xfer_step(g);
            }
            v |= (u32)px << (k * 16);
        }
        if (g->xfer_idx >= g->xfer_w * g->xfer_h) g->xfer_dir = 0;
        return v;
    }
    return g->gpuread;
}

u32 gpu_read_status(gpu_t *g) {
    u32 s = g->stat_bits & 0x9FFF;
    u32 dm = g->disp_mode;
    s |= (dm & 0x40) ? (1u << 16) : 0;        /* hres2 */
    s |= (dm & 0x3) << 17;                    /* hres1 */
    s |= (dm & 0x4) ? (1u << 19) : 0;         /* vres  */
    s |= (dm & 0x8) ? (1u << 20) : 0;         /* PAL   */
    s |= (dm & 0x10) ? (1u << 21) : 0;        /* 24bit */
    s |= (dm & 0x20) ? (1u << 22) : 0;        /* interlace */
    if (!g->disp_enabled) s |= (1u << 23);
    if (g->irq) s |= (1u << 24);
    int ready_cmd = (g->xfer_dir == 1) ? 0 : 1;
    int ready_vram = (g->xfer_dir == 2) ? 1 : 0;
    u32 dmareq = 0;
    switch (g->dma_dir) {
        case 1: dmareq = 1; break;
        case 2: dmareq = ready_cmd; break;
        case 3: dmareq = ready_vram; break;
    }
    s |= dmareq << 25;
    s |= (u32)ready_cmd << 26;
    s |= (u32)ready_vram << 27;
    s |= (1u << 28);
    s |= (u32)g->dma_dir << 29;

    /* Beam position: derive the current scanline from the cycles spent in
     * this frame so that bit 31 (odd line / field) drops during vblank. */
    {
        u32 cyc = (u32)(total_cycles_executed - g->frame_start_cycles);
        if (cyc > 0xFFFFFF) cyc = 0xFFFFFF;
        u32 line = (cyc * 263u) / CPU_CYCLES_PER_FRAME;
        int in_vblank = line >= 240;
        int interlaced = (dm & 0x20) && (dm & 0x4);
        if (interlaced) {
            /* field flag (bit 13) is the inverse of the odd-lines flag (bit 31):
             * OpenBIOS' vsync expects the pairs (1,0) and (0,1) to alternate. */
            if (!g->odd_frame) s |= (1u << 13);
            if (!in_vblank && g->odd_frame) s |= (1u << 31);
        } else {
            s |= (1u << 13);                                 /* always 1 when not interlaced */
            if (!in_vblank && (line & 1)) s |= (1u << 31);
        }
    }
    return s;
}

void gpu_write_status(gpu_t *g, u32 val) {
    u8 cmd = (val >> 24) & 0x3F;
    switch (cmd) {
    case 0x00: gpu_reset(g); break;
    case 0x01: g->pkt_ptr = 0; g->polyline_active = 0; g->xfer_dir = 0; break;
    case 0x02: g->irq = 0; break;
    case 0x03: g->disp_enabled = !(val & 1); break;
    case 0x04: g->dma_dir = val & 3; break;
    case 0x05: g->disp_x = val & 0x3FF; g->disp_y = (val >> 10) & 0x1FF; break;
    case 0x06: g->hrange_x1 = val & 0xFFF; g->hrange_x2 = (val >> 12) & 0xFFF; break;
    case 0x07: g->vrange_y1 = val & 0x3FF; g->vrange_y2 = (val >> 10) & 0x3FF; break;
    case 0x08: g->disp_mode = val & 0xFF; break;
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        switch (val & 0xF) {
            case 2: g->gpuread = g->tex_window; break;
            case 3: g->gpuread = (g->draw_x0 & 0x3FF) | ((g->draw_y0 & 0x1FF) << 10); break;
            case 4: g->gpuread = (g->draw_x1 & 0x3FF) | ((g->draw_y1 & 0x1FF) << 10); break;
            case 5: g->gpuread = (g->off_x & 0x7FF) | ((g->off_y & 0x7FF) << 11); break;
            case 7: g->gpuread = 2; break;
            default: break;
        }
        break;
    default: break;
    }
}

void gpu_vblank(gpu_t *g) {
    g->odd_frame ^= 1;
    g->frame_polys = 0;
    g->frame_start_cycles = total_cycles_executed;
}

/* ------------------------------------------------------------------ */
/* Display: PS1 VRAM display area -> Dreamcast 640x480 RGB565          */
/* ------------------------------------------------------------------ */

void gpu_blit_to_screen(gpu_t *g, u16 *fb, int out_w, int out_h) {
    static const int hres_tab[4] = { 256, 320, 512, 640 };
    int W = (g->disp_mode & 0x40) ? 368 : hres_tab[g->disp_mode & 3];
    int H = ((g->disp_mode & 0x4) && (g->disp_mode & 0x20)) ? 480 : 240;

    if (!g->disp_enabled) {
        u32 *d = (u32 *)fb;
        for (int i = 0; i < out_w * out_h / 2; i++) d[i] = 0;
        return;
    }

    u32 xstep = ((u32)W << 16) / out_w;
    u32 ystep = ((u32)H << 16) / out_h;
    u32 sy_fp = 0;
    for (int y = 0; y < out_h; y++, sy_fp += ystep) {
        const u16 *src = &g->vram[((g->disp_y + (sy_fp >> 16)) & 511) << 10];
        u32 *dst = (u32 *)&fb[y * out_w];
        u32 sx_fp = (u32)g->disp_x << 16;
        for (int x = 0; x < out_w / 2; x++) {
            u16 p0 = src[(sx_fp >> 16) & 1023]; sx_fp += xstep;
            u16 p1 = src[(sx_fp >> 16) & 1023]; sx_fp += xstep;
            u32 c0 = ((p0 & 0x1F) << 11) | ((p0 & 0x3E0) << 1) | ((p0 >> 10) & 0x1F);
            u32 c1 = ((p1 & 0x1F) << 11) | ((p1 & 0x3E0) << 1) | ((p1 >> 10) & 0x1F);
            dst[x] = c0 | (c1 << 16);
        }
    }
}
