#include "raster.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "fastcvt.h"

#ifdef RASTER_SSE2_COPY
#define RF_NAME raster_fill_sse2
#elif defined(FLASHNT_SSE2_DISPATCH)
#define RF_NAME raster_fill_x87
#else
#define RF_NAME raster_fill
#endif

#ifndef RASTER_SSE2_COPY
int g_stage_quality = 2;   /* high */
int g_draw_quality = -1;
int raster_quality = 4;    /* samples per pixel of the stage */

int quality_samples(int q) {
    static const int n[8] = { 1, 2, 4, 4, 8, 8, 16, 16 };
    return q >= 0 && q < 8 ? n[q] : 4;
}

/* standard multisample positions (Direct3D / Vulkan) */
#endif

static const float pos1[2] = { 0.5f, 0.5f };
static const float pos2[4] = { 0.75f, 0.75f, 0.25f, 0.25f };
static const float pos4[8] = { 0.375f, 0.125f, 0.875f, 0.375f, 0.125f, 0.625f, 0.625f, 0.875f };
static const float pos8[16] = { 0.5625f, 0.3125f, 0.4375f, 0.6875f, 0.8125f, 0.5625f, 0.3125f, 0.1875f,
                                0.1875f, 0.8125f, 0.0625f, 0.4375f, 0.6875f, 0.9375f, 0.9375f, 0.0625f };
static const float pos16[32] = { 0.5625f, 0.5625f, 0.4375f, 0.3125f, 0.3125f, 0.625f, 0.75f, 0.4375f,
                                 0.1875f, 0.375f, 0.625f, 0.8125f, 0.8125f, 0.6875f, 0.6875f, 0.1875f,
                                 0.375f, 0.875f, 0.5f, 0.0625f, 0.25f, 0.125f, 0.125f, 0.75f,
                                 0.0f, 0.5f, 0.9375f, 0.25f, 0.875f, 0.9375f, 0.0625f, 0.0f };
static const float *sample_pos(int ns) {
    return ns == 2 ? pos2 : ns == 4 ? pos4 : ns == 8 ? pos8 : ns == 16 ? pos16 : pos1;
}

#ifndef RASTER_SSE2_COPY
Surface *surf_create_ns(int w, int h, int ns) {
    Surface *s = (Surface*)calloc(1, sizeof(Surface));
    size_t n = (size_t)(w > 0 ? w : 1) * (size_t)(h > 0 ? h : 1);
    if (!s) return NULL;
    if (ns != 1 && ns != 2 && ns != 4 && ns != 8 && ns != 16) ns = 1;
    s->w = w; s->h = h; s->ns = ns;
    s->px = (u32*)calloc(n, sizeof(u32));
    s->sp = ns == 1 ? s->px : (u32*)calloc(n * ns, sizeof(u32));
    if (!s->px || !s->sp) { if (s->sp != s->px) free(s->sp); free(s->px); free(s); return NULL; }
    return s;
}
Surface *surf_create(int w, int h) { return surf_create_ns(w, h, raster_quality); }
void surf_free(Surface *s) {
    if (!s) return;
    if (s->sp != s->px) free(s->sp);
    free(s->px); free(s->stencil); free(s);
}
void surf_set_samples(Surface *s, int ns) {
    size_t n = (size_t)s->w * s->h; u32 *sp;
    if (ns != 1 && ns != 2 && ns != 4 && ns != 8 && ns != 16) ns = 1;
    if (ns == s->ns) return;
    sp = ns == 1 ? s->px : (u32*)calloc(n * ns, sizeof(u32));
    if (!sp) return;
    if (s->sp != s->px) free(s->sp);
    s->sp = sp; s->ns = ns;
    free(s->stencil); s->stencil = NULL; s->nmasks = 0; s->mstate = MS_NONE; s->maskers = 0; s->nostencil = 0;
}
void surf_clear_argb(Surface *s, u32 v) {
    size_t i, n = (size_t)s->w * s->h * s->ns;
    for (i = 0; i < n; i++) s->sp[i] = v;
    if (s->sp != s->px) { n = (size_t)s->w * s->h; for (i = 0; i < n; i++) s->px[i] = v; }
    if (s->stencil) memset(s->stencil, 0, (size_t)s->w * s->h * s->ns);
    s->nmasks = 0; s->mstate = MS_NONE; s->maskers = 0; s->nostencil = 0; s->dirty = 0;
}
void surf_clear(Surface *s, SWFColor c) {
    surf_clear_argb(s, 0xFF000000u | ((u32)c.r << 16) | ((u32)c.g << 8) | c.b);
}
void surf_fill_from(Surface *s, const u32 *src) {
    size_t i, n = (size_t)s->w * s->h; int k;
    if (s->sp != s->px) for (i = 0; i < n; i++) for (k = 0; k < s->ns; k++) s->sp[i * s->ns + k] = src[i];
    if (src != s->px) memcpy(s->px, src, n * 4);
    s->dirty = 0;
}
void surf_resolve(Surface *s) {
    size_t i, n = (size_t)s->w * s->h; int k, ns = s->ns;
    if (ns == 1 || !s->dirty) { s->dirty = 0; return; }
    if (ns == 4) {   /* the usual (high quality) case: the same rounding with shifts */
        const u32 *q = s->sp;
        for (i = 0; i < n; i++, q += 4) {
            u32 c0 = q[0], c1 = q[1], c2 = q[2], c3 = q[3];
            if (c0 == c1 && c0 == c2 && c0 == c3) { s->px[i] = c0; continue; }
            {
                u32 ag = ((c0 >> 8) & 0x00FF00FFu) + ((c1 >> 8) & 0x00FF00FFu) + ((c2 >> 8) & 0x00FF00FFu) + ((c3 >> 8) & 0x00FF00FFu) + 0x00020002u;
                u32 rb = (c0 & 0x00FF00FFu) + (c1 & 0x00FF00FFu) + (c2 & 0x00FF00FFu) + (c3 & 0x00FF00FFu) + 0x00020002u;
                s->px[i] = (((ag >> 2) & 0x00FF00FFu) << 8) | ((rb >> 2) & 0x00FF00FFu);
            }
        }
        s->dirty = 0;
        return;
    }
    for (i = 0; i < n; i++) {
        const u32 *q = s->sp + i * ns; u32 a = 0, r = 0, g = 0, b = 0;
        for (k = 0; k < ns; k++) { u32 c = q[k]; a += c >> 24; r += (c >> 16) & 255; g += (c >> 8) & 255; b += c & 255; }
        s->px[i] = (((a + ns / 2) / ns) << 24) | (((r + ns / 2) / ns) << 16) | (((g + ns / 2) / ns) << 8) | ((b + ns / 2) / ns);
    }
    s->dirty = 0;
}

/* ---- masks: Ruffle's stencil states ---- */
static int ensure_stencil(Surface *s) {
    if (!s->stencil) s->stencil = (u8*)calloc((size_t)s->w * s->h * s->ns, 1);
    return s->stencil != NULL;
}
/* `maskers` counts the masks being drawn (Ruffle's CommandList::maskers_in_progress):
   a mask inside a mask is not drawn, nothing is while more than one is in progress */
void surf_push_mask(Surface *s) {
    if (s->maskers == 0) { if (!ensure_stencil(s)) { s->maskers++; s->nostencil++; return; } s->nmasks++; s->mstate = MS_DRAW_STENCIL; }
    s->maskers++;
}
void surf_activate_mask(Surface *s) { if (s->maskers > 0) s->maskers--; if (s->maskers == 0 && !s->nostencil) s->mstate = MS_CONTENT; }
void surf_deactivate_mask(Surface *s) { if (s->maskers == 0 && !s->nostencil) s->mstate = MS_CLEAR_STENCIL; s->maskers++; }
void surf_pop_mask(Surface *s) {
    if (s->maskers > 0) s->maskers--;
    if (s->maskers == 0) {
        if (s->nostencil) { s->nostencil--; return; }
        if (s->nmasks > 0) s->nmasks--;
        s->mstate = s->nmasks > 0 ? MS_CONTENT : MS_NONE;
    }
}

void edgelist_init(EdgeList *l) { l->e = NULL; l->n = 0; l->cap = 0; }
void edgelist_free(EdgeList *l) { free(l->e); edgelist_init(l); }
void edgelist_clear(EdgeList *l) { l->n = 0; }
void edgelist_add(EdgeList *l, float x0, float y0, float x1, float y1) {
    REdge *e;
    if (y0 == y1) return;                        /* horizontal: no crossings */
    if (!(x0 == x0) || !(x1 == x1) || !(y0 == y0) || !(y1 == y1)) return;
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 256;
        REdge *ne = (REdge*)realloc(l->e, (size_t)nc * sizeof(REdge));
        if (!ne) return;
        l->e = ne; l->cap = nc;
    }
    e = &l->e[l->n++];
    if (y0 < y1) { e->x0 = x0; e->y0 = y0; e->x1 = x1; e->y1 = y1; e->dir = 1; }
    else         { e->x0 = x1; e->y0 = y1; e->x1 = x0; e->y1 = y0; e->dir = -1; }
}

static u8 clamp8(int v) { return (u8)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

void cxform_apply(const SWFCxform *cx, u8 *r, u8 *g, u8 *b, u8 *a) {
    *r = clamp8(((*r) * cx->mr) / 256 + cx->ar);
    *g = clamp8(((*g) * cx->mg) / 256 + cx->ag);
    *b = clamp8(((*b) * cx->mb) / 256 + cx->ab);
    *a = clamp8(((*a) * cx->ma) / 256 + cx->aa);
}

int matrix_invert(const SWFMatrix *m, double *a, double *b, double *c,
                  double *d, double *e, double *f) {
    /* forward: x' = sx*x + r1*y + tx ; y' = r0*x + sy*y + ty */
    double det = m->sx * m->sy - m->r1 * m->r0;
    if (fabs(det) < 1e-12) return 0;
    *a =  m->sy / det;  *b = -m->r1 / det;
    *c = -m->r0 / det;  *d =  m->sx / det;
    *e = -(*a * m->tx + *b * m->ty);
    *f = -(*c * m->tx + *d * m->ty);
    return 1;
}

#endif /* !RASTER_SSE2_COPY */

/* ---- paint evaluation (the fragment shaders) ---- */

static float satf(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/* k / 255.0f for every byte (the same correctly rounded floats, looked up) */
static float g_u8f[256]; static int g_u8f_ready;
static void u8f_init(void) { int k; if (g_u8f_ready) return; for (k = 0; k < 256; k++) g_u8f[k] = k / 255.0f; g_u8f_ready = 1; }
#define U8F(v) g_u8f[(v) & 255]

#ifndef RASTER_SSE2_COPY
u32 bitmap_texel_pm(const Bitmap *bm, int x, int y) {
    u32 c, a;
    if (bm->pm) return bm->pm[(size_t)y * bm->w + x];
    c = bm->px[(size_t)y * bm->w + x]; a = c >> 24;
    if (a == 255) return c;
    return (a << 24) | ((((c >> 16) & 255) * a + 127) / 255 << 16) | ((((c >> 8) & 255) * a + 127) / 255 << 8) | (((c & 255) * a + 127) / 255);
}

#endif

static void texel4(const Bitmap *bm, int x, int y, int repeat, float *o) {
    u32 c;
    if (repeat) { x %= bm->w; if (x < 0) x += bm->w; y %= bm->h; if (y < 0) y += bm->h; }
    else { if (x < 0) x = 0; else if (x >= bm->w) x = bm->w - 1; if (y < 0) y = 0; else if (y >= bm->h) y = bm->h - 1; }
    c = bitmap_texel_pm(bm, x, y);
    o[0] = U8F(c >> 16); o[1] = U8F(c >> 8); o[2] = U8F(c); o[3] = U8F(c >> 24);
}

/* texel coordinate wrapped (repeat) or clamped, as texel4 does */
static int tc_fix(int v, int n, int repeat) {
    if (repeat) { v %= n; if (v < 0) v += n; return v; }
    return v < 0 ? 0 : v >= n ? n - 1 : v;
}
/* textureSample of a premultiplied bitmap at texel coordinates (u, v) */
static void sample_bitmap(const Bitmap *bm, double u, double v, int repeat, int smooth, float *o) {
    if (!smooth) {
        int x = ifloor(u), y = ifloor(v);
        texel4(bm, x, y, repeat, o);
        return;
    } else {
        double su = u - 0.5, sv = v - 0.5; int x0 = ifloor(su), y0 = ifloor(sv), x1, y1;
        float fx = (float)(su - x0), fy = (float)(sv - y0), gx = 1 - fx, gy = 1 - fy; u32 ca, cb, cc, cd;
        /* the four texels' coordinates, wrapped or clamped once each (texel4's rule) */
        x1 = tc_fix(x0 + 1, bm->w, repeat); y1 = tc_fix(y0 + 1, bm->h, repeat);
        x0 = tc_fix(x0, bm->w, repeat); y0 = tc_fix(y0, bm->h, repeat);
        ca = bitmap_texel_pm(bm, x0, y0); cb = bitmap_texel_pm(bm, x1, y0);
        cc = bitmap_texel_pm(bm, x0, y1); cd = bitmap_texel_pm(bm, x1, y1);
#define BL(SH) ((U8F(ca >> SH) * gx + U8F(cb >> SH) * fx) * gy + (U8F(cc >> SH) * gx + U8F(cd >> SH) * fx) * fy)
        o[0] = BL(16); o[1] = BL(8); o[2] = BL(0); o[3] = BL(24);
#undef BL
    }
}

static float lin2srgb(float c) { return c <= 0.0031308f ? 12.92f * c : 1.055f * (float)pow(c, 1.0 / 2.4) - 0.055f; }

/* per-fill constants of paint_eval: the colour transform as floats and the
   row's share of the paint-space transform (the same expressions as before,
   computed once instead of per pixel) */
typedef struct { float mr, mg, mb, ma, ar, ag, ab, aa; double rb, rd; } PaintCtx;
static void paint_ctx_init(PaintCtx *c, const Paint *p) {
    c->mr = p->cx.mr / 256.0f; c->mg = p->cx.mg / 256.0f; c->mb = p->cx.mb / 256.0f; c->ma = p->cx.ma / 256.0f;
    c->ar = p->cx.ar / 255.0f; c->ag = p->cx.ag / 255.0f; c->ab = p->cx.ab / 255.0f; c->aa = p->cx.aa / 255.0f;
    c->rb = c->rd = 0;
}
static void paint_ctx_row(PaintCtx *c, const Paint *p, int y) { double dy = y + 0.5; c->rb = p->ib * dy; c->rd = p->id * dy; }

/* premultiplied colour (0..1) of the paint at pixel (x, y) of the row set by
   paint_ctx_row, the transform applied */
static void paint_eval(const Paint *p, const PaintCtx *k, int x, float *o) {
    float mr = k->mr, mg = k->mg, mb = k->mb, ma = k->ma, ar = k->ar, ag = k->ag, ab = k->ab, aa = k->aa;
    float c[4];
    if (p->kind == PAINT_SOLID) {
        c[0] = satf(p->color.r / 255.0f * mr + ar); c[1] = satf(p->color.g / 255.0f * mg + ag);
        c[2] = satf(p->color.b / 255.0f * mb + ab); c[3] = satf(p->color.a / 255.0f * ma + aa);
        o[0] = c[0] * c[3]; o[1] = c[1] * c[3]; o[2] = c[2] * c[3]; o[3] = c[3];
        return;
    } else {
        double dx = x + 0.5;
        double gx = p->ia * dx + k->rb + p->ie;
        double gy = p->ic * dx + k->rd + p->iff;
        if (p->kind == PAINT_BITMAP) {
            sample_bitmap(p->bmp, gx, gy, p->bmp_repeat, p->bmp_smooth, c);
            if (c[3] > 0) {
                c[0] /= c[3]; c[1] /= c[3]; c[2] /= c[3];
                c[0] = satf(c[0] * mr + ar); c[1] = satf(c[1] * mg + ag); c[2] = satf(c[2] * mb + ab); c[3] = satf(c[3] * ma + aa);
                o[0] = c[0] * c[3]; o[1] = c[1] * c[3]; o[2] = c[2] * c[3]; o[3] = c[3];
            } else { o[0] = o[1] = o[2] = o[3] = 0; }
            return;
        } else {
            float t, u, f; int i0, i1, j;
            double ux = gx / 16384.0, uy = gy / 16384.0;   /* uv * 2 - 1 */
            if (p->kind == PAINT_LINEAR) t = (float)(gx / 32768.0 + 0.5);
            else if (p->kind == PAINT_RADIAL) t = (float)sqrt(ux * ux + uy * uy);
            else {
                float fp = (float)p->focal, ddx = (float)(fp - ux), ddy = (float)(-uy), l = (float)sqrt(ddx * ddx + ddy * ddy);
                if (l > 0) { ddx /= l; ddy /= l; }
                t = l / ((float)sqrt(1.0f - fp * fp * ddy * ddy) + fp * ddx);
            }
            if (p->spread == 0) t = satf(t);
            else if (p->spread == 1) {
                if (t < 0) t = -t;
                if (t > 2147483647.0f) t = 0;
                if ((ifloor(t) & 1) == 0) t = t - (float)ifloor(t); else t = 1.0f - (t - (float)ifloor(t));
            } else t = t - (float)floor(t);
            if (!(t == t)) t = 0;
            /* linear sample of the 256-texel gradient texture (clamped) */
            u = t * 256.0f - 0.5f; i0 = ifloor(u); f = u - i0; i1 = i0 + 1;
            if (i0 < 0) i0 = 0; if (i0 > 255) i0 = 255; if (i1 < 0) i1 = 0; if (i1 > 255) i1 = 255;
            for (j = 0; j < 4; j++) c[j] = (p->lut[i0][j] * (1 - f) + p->lut[i1][j] * f) / 255.0f;
            if (p->linear_rgb) {
                float a = c[3], r = c[0], g = c[1], b = c[2];
                if (a > 0) { r /= a; g /= a; b /= a; }
                c[0] = lin2srgb(r) * a; c[1] = lin2srgb(g) * a; c[2] = lin2srgb(b) * a;
            }
            c[0] = satf(c[0] * mr + ar); c[1] = satf(c[1] * mg + ag); c[2] = satf(c[2] * mb + ab); c[3] = satf(c[3] * ma + aa);
            o[0] = c[0] * c[3]; o[1] = c[1] * c[3]; o[2] = c[2] * c[3]; o[3] = c[3];
            return;
        }
    }
}

/* ---- blending one premultiplied source into a sample (GPU blend unit) ---- */
static u32 unorm(float v) { return v <= 0 ? 0 : v >= 1 ? 255 : (u32)ifloor(v * 255.0f + 0.5f); }
static u32 blend_px(u32 d, const float *s, int tb) {
    float dr = U8F(d >> 16), dg = U8F(d >> 8), db = U8F(d), da = U8F(d >> 24), ia = 1 - s[3];
    float r, g, b, a = s[3] + da * ia;
    switch (tb) {
    case TB_ADD: r = s[0] + dr; g = s[1] + dg; b = s[2] + db; break;
    case TB_SUBTRACT: r = dr - s[0]; g = dg - s[1]; b = db - s[2]; break;
    case TB_SCREEN: r = s[0] + dr * (1 - s[0]); g = s[1] + dg * (1 - s[1]); b = s[2] + db * (1 - s[2]); break;
    case TB_REPLACE: r = s[0]; g = s[1]; b = s[2]; a = s[3]; break;
    default: r = s[0] + dr * ia; g = s[1] + dg * ia; b = s[2] + db * ia; break;
    }
    return (unorm(a) << 24) | (unorm(r) << 16) | (unorm(g) << 8) | unorm(b);
}

/* blend_px for one source colour over many samples.  Interiors of shapes
   cover runs of samples holding the same destination value, so the last
   result is remembered; an opaque normal source just replaces the sample
   and a fully transparent one leaves it as it is.  The results are exactly
   those of blend_px. */
typedef struct { const float *s; int tb, kind; u32 opaque, last_d, last_o; int have; } Blender;
enum { BK_GENERAL, BK_OPAQUE, BK_NOOP };
static void blender_init(Blender *b, const float *s, int tb) {
    b->s = s; b->tb = tb; b->have = 0; b->kind = BK_GENERAL;
    if (tb == TB_NORMAL && s[3] >= 1.0f && s[3] <= 1.0f) {
        /* blend_px with ia = 0: the source itself */
        b->kind = BK_OPAQUE; b->opaque = (255u << 24) | (unorm(s[0]) << 16) | (unorm(s[1]) << 8) | unorm(s[2]);
    }
    else if ((tb == TB_NORMAL || tb == TB_ADD || tb == TB_SUBTRACT || tb == TB_SCREEN) && s[0] == 0 && s[1] == 0 && s[2] == 0 && s[3] == 0) b->kind = BK_NOOP;
}
static u32 blender_px(Blender *b, u32 d) {
    if (b->kind == BK_OPAQUE) return b->opaque;
    if (b->kind == BK_NOOP) return d;
    if (b->have && d == b->last_d) return b->last_o;
    b->last_d = d; b->last_o = blend_px(d, b->s, b->tb); b->have = 1;
    return b->last_o;
}

/* apply one covered sample through the stencil state; returns 1 when the colour is to be written */
#define STENCIL_PASS(s, idx) ((s)->mstate == MS_NONE || (s)->stencil[idx] == (u8)(s)->nmasks)

/* ---- scanline fill with sample coverage ---- */

typedef struct { double x; int dir; } Cross;

static int cmp_cross(const void *A, const void *B) {
    const Cross *a = (const Cross*)A, *b = (const Cross*)B;
    return (a->x < b->x) ? -1 : (a->x > b->x);
}

/* scratch buffers shared by every fill (raster_fill is not re-entered) */
static REdge *g_edges; static int g_edges_cap;
static int *g_active, *g_bucket; static int g_active_cap, g_bucket_cap;
static Cross *g_cross; static int g_cross_cap;
static u32 *g_cov; static int g_cov_w;
static u8 *g_cnt; static size_t g_cnt_n;   /* rule 2: how many times each sample is covered */
static int grow_buf(void **p, int *cap, int need, size_t elem) {
    void *np; int nc;
    if (need <= *cap) return 1;
    nc = *cap ? *cap : 256; while (nc < need) nc *= 2;
    np = realloc(*p, (size_t)nc * elem);
    if (!np) return 0;
    *p = np; *cap = nc;
    return 1;
}

void RF_NAME(Surface *s, const EdgeList *el, int rule, const Paint *p,
                 const SWFRect *clip) {
    REdge *edges;
    int n = el->n, i, y, ymin, ymax, xmin, xmax, rows, ns = s->ns;
    int *active; int nactive = 0, next = 0;
    Cross *cross;
    u32 *cov;
    const float *sp = sample_pos(ns);
    float fymin = 1e30f, fymax = -1e30f, fxmin = 1e30f, fxmax = -1e30f;
    int write_color = s->mstate == MS_NONE || s->mstate == MS_CONTENT;
    PaintCtx pc;
    u8f_init();

    if (n == 0 || s->maskers > 1) return;
    if (s->mstate != MS_NONE && !s->stencil) return;
    for (i = 0; i < n; i++) {
        const REdge *e = &el->e[i];
        if (e->y0 < fymin) fymin = e->y0;
        if (e->y1 > fymax) fymax = e->y1;
        if (e->x0 < fxmin) fxmin = e->x0;
        if (e->x1 < fxmin) fxmin = e->x1;
        if (e->x0 > fxmax) fxmax = e->x0;
        if (e->x1 > fxmax) fxmax = e->x1;
    }
    if (!(fymin > -1e9f)) fymin = -1e9f; if (!(fymax < 1e9f)) fymax = 1e9f;
    if (!(fxmin > -1e9f)) fxmin = -1e9f; if (!(fxmax < 1e9f)) fxmax = 1e9f;
    ymin = (int)floor(fymin); ymax = (int)ceil(fymax);
    xmin = (int)floor(fxmin); xmax = (int)ceil(fxmax) + 1;
    if (ymin < 0) ymin = 0;
    if (ymax > s->h) ymax = s->h;
    if (xmin < 0) xmin = 0;
    if (xmax > s->w) xmax = s->w;
    if (clip) {
        if (clip->xmin > xmin) xmin = clip->xmin;
        if (clip->xmax < xmax) xmax = clip->xmax;
        if (clip->ymin > ymin) ymin = clip->ymin;
        if (clip->ymax < ymax) ymax = clip->ymax;
    }
    if (ymin >= ymax || xmin >= xmax) return;
    rows = ymax - ymin;

    if (!grow_buf((void**)&g_edges, &g_edges_cap, n, sizeof(REdge)) ||
        !grow_buf((void**)&g_active, &g_active_cap, n, sizeof(int)) ||
        !grow_buf((void**)&g_cross, &g_cross_cap, n, sizeof(Cross)) ||
        !grow_buf((void**)&g_bucket, &g_bucket_cap, rows + 2, sizeof(int))) return;
    if (g_cov_w < s->w) {
        free(g_cov); g_cov = (u32*)calloc((size_t)s->w + 1, sizeof(u32));
        if (!g_cov) { g_cov_w = 0; return; }
        g_cov_w = s->w;
    }
    if (rule == 2 && g_cnt_n < (size_t)s->w * ns) {
        free(g_cnt); g_cnt = (u8*)calloc((size_t)s->w * ns + 1, 1);
        if (!g_cnt) { g_cnt_n = 0; return; }
        g_cnt_n = (size_t)s->w * ns;
    }
    edges = g_edges; active = g_active; cross = g_cross; cov = g_cov;
    /* counting sort by first pixel row (rows above the clip go first, below it last) */
    {
        int *cnt = g_bucket, k;
        memset(cnt, 0, (size_t)(rows + 2) * sizeof(int));
        for (i = 0; i < n; i++) {
            int r = ifloor(el->e[i].y0) - ymin;
            if (r < 0) r = 0; if (r > rows) r = rows;
            cnt[r + 1]++;
        }
        for (k = 1; k <= rows + 1; k++) cnt[k] += cnt[k - 1];
        for (i = 0; i < n; i++) {
            int r = ifloor(el->e[i].y0) - ymin;
            if (r < 0) r = 0; if (r > rows) r = rows;
            edges[cnt[r]++] = el->e[i];
        }
    }

    s->dirty = 1;
    paint_ctx_init(&pc, p);
    for (y = ymin; y < ymax; y++) {
        int smp, x, lo = xmax, hi = xmin;
        /* activate edges starting in this pixel row or above */
        while (next < n && edges[next].y0 < (float)(y + 1)) active[nactive++] = next++;
        for (smp = 0; smp < ns; smp++) {
            float sy = (float)y + sp[smp * 2 + 1], ox = sp[smp * 2];
            int ncross = 0, k, wind = 0; u32 bit = 1u << smp;
            for (k = 0; k < nactive; k++) {
                const REdge *e = &edges[active[k]];
                if (e->y0 <= sy && sy < e->y1) {
                    /* in double: crossings exactly on a sample stay there (a GPU's edge test is exact) */
                    cross[ncross].x = (double)e->x0 + ((double)sy - e->y0) * ((double)e->x1 - e->x0) / ((double)e->y1 - e->y0);
                    cross[ncross].dir = e->dir;
                    ncross++;
                }
            }
            if (ncross < 2) continue;
            if (ncross > 24) qsort(cross, (size_t)ncross, sizeof(Cross), cmp_cross);
            else {
                int a2, b2;
                for (a2 = 1; a2 < ncross; a2++) { Cross t = cross[a2]; b2 = a2 - 1; while (b2 >= 0 && cross[b2].x > t.x) { cross[b2 + 1] = cross[b2]; b2--; } cross[b2 + 1] = t; }
            }
            for (k = 0; k < ncross - 1; k++) {
                int inside;
                wind += cross[k].dir;
                inside = rule == 1 ? (wind & 1) : (wind != 0);
                if (inside) {
                    /* samples with a <= x + ox < b */
                    double ta = (double)cross[k].x - ox, tb = (double)cross[k + 1].x - ox;
                    int xa, xb;
                    if (ta >= xmax || tb <= xmin) continue;   /* the span is outside [xmin, xmax) */
                    xa = ta <= xmin ? xmin : iceil(ta);   /* ceil, clamped to [xmin, xmax] */
                    xb = tb >= xmax ? xmax : iceil(tb);
                    if (xa >= xb) continue;
                    if (xa < lo) lo = xa; if (xb > hi) hi = xb;
                    for (x = xa; x < xb; x++) cov[x] |= bit;
                    if (rule == 2) { int c = wind < 0 ? -wind : wind; if (c > 255) c = 255; for (x = xa; x < xb; x++) g_cnt[(size_t)x * ns + smp] = (u8)c; }
                }
            }
        }
        /* drop finished edges */
        { int k; for (k = 0; k < nactive; ) { if (edges[active[k]].y1 <= (float)(y + 1)) active[k] = active[--nactive]; else k++; } }
        if (lo >= hi) continue;
        {
            u32 *row = s->sp + (size_t)y * s->w * ns;
            u8 *st = s->stencil ? s->stencil + (size_t)y * s->w * ns : NULL;
            float col[4]; int have = 0, k; Blender bl;
            paint_ctx_row(&pc, p, y);
            if (p->kind == PAINT_SOLID) { paint_eval(p, &pc, lo, col); have = 1; blender_init(&bl, col, s->blend); }
            for (x = lo; x < hi; x++) {
                u32 m = cov[x];
                if (!m) continue;
                cov[x] = 0;
                if (!write_color) {
                    if (rule == 2) memset(g_cnt + (size_t)x * ns, 0, (size_t)ns);
                    for (k = 0; k < ns; k++) if (m & (1u << k)) {
                        size_t idx = (size_t)x * ns + k;
                        if (s->mstate == MS_DRAW_STENCIL) { if (st[idx] == (u8)(s->nmasks - 1) && st[idx] < 255) st[idx]++; }
                        else if (st[idx] == (u8)s->nmasks && st[idx] > 0) st[idx]--;
                    }
                    continue;
                }
                if (p->kind != PAINT_SOLID || !have) { paint_eval(p, &pc, x, col); blender_init(&bl, col, s->blend); }
                if (rule != 2 && !(st && s->mstate == MS_CONTENT)) {   /* the common case: no stencil, one blend per sample */
                    u32 *q = row + (size_t)x * ns;
                    if (bl.kind == BK_OPAQUE) { for (k = 0; k < ns; k++) if (m & (1u << k)) q[k] = bl.opaque; }
                    else if (bl.kind != BK_NOOP) { for (k = 0; k < ns; k++) if (m & (1u << k)) q[k] = blender_px(&bl, q[k]); }
                    continue;
                }
                for (k = 0; k < ns; k++) if (m & (1u << k)) {
                    size_t idx = (size_t)x * ns + k; int c = 1;
                    if (rule == 2) { c = g_cnt[idx]; g_cnt[idx] = 0; }
                    if (st && s->mstate == MS_CONTENT && st[idx] != (u8)s->nmasks) continue;
                    if (rule == 2) { while (c-- > 0) row[idx] = blender_px(&bl, row[idx]); }   /* overlapping triangles each blend */
                    else row[idx] = blender_px(&bl, row[idx]);
                }
            }
        }
    }
}

#if defined(FLASHNT_SSE2_DISPATCH) && !defined(RASTER_SSE2_COPY)
/* NT build: the scanline fill compiled twice, x87 and SSE2 (raster_sse2.c);
   the platform layer sets g_cpu_sse2 when the CPU and the OS allow SSE2 */
int g_cpu_sse2;
void raster_fill_sse2(Surface *s, const EdgeList *el, int rule, const Paint *p, const SWFRect *clip);
void raster_fill(Surface *s, const EdgeList *el, int rule, const Paint *p, const SWFRect *clip) {
    if (g_cpu_sse2) raster_fill_sse2(s, el, rule, p, clip); else raster_fill_x87(s, el, rule, p, clip);
}
#endif

#ifndef RASTER_SSE2_COPY
/* a pixel-aligned quad of premultiplied pixels drawn onto every sample through the mask state */
void surf_draw_pixels(Surface *s, const u32 *src, int lx, int ly, int lw, int lh, int tb) {
    int x, y, k, ns = s->ns;
    u8f_init();
    if (s->maskers > 1) return;
    if (s->mstate == MS_DRAW_STENCIL || s->mstate == MS_CLEAR_STENCIL) {
        if (!s->stencil) return;
        for (y = 0; y < lh; y++) {
            int sy = ly + y; if (sy < 0 || sy >= s->h) continue;
            for (x = 0; x < lw; x++) {
                int sx = lx + x; u8 *st; if (sx < 0 || sx >= s->w) continue;
                st = s->stencil + ((size_t)sy * s->w + sx) * ns;
                for (k = 0; k < ns; k++) {
                    if (s->mstate == MS_DRAW_STENCIL) { if (st[k] == (u8)(s->nmasks - 1) && st[k] < 255) st[k]++; }
                    else if (st[k] == (u8)s->nmasks && st[k] > 0) st[k]--;
                }
            }
        }
        return;
    }
    s->dirty = 1;
    for (y = 0; y < lh; y++) {
        int sy = ly + y; if (sy < 0 || sy >= s->h) continue;
        for (x = 0; x < lw; x++) {
            int sx = lx + x; u32 c = src[(size_t)y * lw + x], *d; float col[4]; const u8 *st; Blender bl;
            if (sx < 0 || sx >= s->w) continue;
            if (!c && tb != TB_REPLACE) continue;
            col[0] = U8F(c >> 16); col[1] = U8F(c >> 8); col[2] = U8F(c); col[3] = U8F(c >> 24);
            d = s->sp + ((size_t)sy * s->w + sx) * ns;
            st = s->stencil ? s->stencil + ((size_t)sy * s->w + sx) * ns : NULL;
            blender_init(&bl, col, tb);
            for (k = 0; k < ns; k++) {
                if (st && s->mstate == MS_CONTENT && st[k] != (u8)s->nmasks) continue;
                d[k] = blender_px(&bl, d[k]);
            }
        }
    }
}

/* a replacing draw of `src` on the pixels where `where` is not fully transparent (a fragment shader that discards) */
void surf_draw_pixels_where(Surface *s, const u32 *src, const u32 *where, int lx, int ly, int lw, int lh) {
    int x, y, k, ns = s->ns;
    if (s->mstate == MS_DRAW_STENCIL || s->mstate == MS_CLEAR_STENCIL || s->maskers > 1) return;
    s->dirty = 1;
    for (y = 0; y < lh; y++) {
        int sy = ly + y; if (sy < 0 || sy >= s->h) continue;
        for (x = 0; x < lw; x++) {
            int sx = lx + x; u32 c = src[(size_t)y * lw + x], *d; const u8 *st;
            if (sx < 0 || sx >= s->w || !(where[(size_t)y * lw + x] >> 24)) continue;
            d = s->sp + ((size_t)sy * s->w + sx) * ns;
            st = s->stencil ? s->stencil + ((size_t)sy * s->w + sx) * ns : NULL;
            for (k = 0; k < ns; k++) {
                if (st && s->mstate == MS_CONTENT && st[k] != (u8)s->nmasks) continue;
                d[k] = c;
            }
        }
    }
}
#endif /* !RASTER_SSE2_COPY */
