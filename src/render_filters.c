/* render_filters.c - bitmap filters and blend modes in software.
 *
 * Layers are premultiplied 0xAARRGGBB.  The filter math follows Ruffle's
 * wgpu filter shaders (blur.wgsl, glow.wgsl, bevel.wgsl, color_matrix.wgsl,
 * displacement_map.wgsl), which imitate Flash Player:
 *   blur        box blur per pass (horizontal then vertical) whose kernel is
 *               blurX/blurY pixels wide, fractional edges weighted, results
 *               truncated to 8 bits like Flash's fixed point
 *   glow/shadow the blurred alpha (offset by distance/angle for the shadow)
 *               tinted, scaled by strength, inner/knockout/hide-object
 *   bevel       highlight and shadow from the blurred alpha sampled on both
 *               sides along the angle
 *   gradient    glow and bevel with the colour looked up in a gradient
 *   colour matrix, convolution, displacement map
 * Blend modes composite a layer onto the (opaque) surface; inside a layer
 * being rendered on black and on white (to recover its alpha), alpha and
 * erase work against that background. */
#include "render_filters.h"
#include "bmd.h"
#include "pbj.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "fastcvt.h"

static const double PASS_SCALES[15] = { 1.0, 2.1, 2.7, 3.1, 3.5, 3.8, 4.0, 4.2, 4.4, 4.6, 5.0, 6.0, 6.0, 7.0, 7.0 };

static double fx16(s32 v) { return v / 65536.0; }
static int passes_of(const SwfFilter *f) { int p = f->passes; return p < 1 ? 1 : p > 15 ? 15 : p; }

/* how far a filter reaches past the source (px, ceil) */
static void blur_reach(double bx, double by, int passes, double *rx, double *ry) {
    double s = PASS_SCALES[(passes < 1 ? 1 : passes > 15 ? 15 : passes) - 1];
    *rx = s * bx > 0 ? s * bx : 0; *ry = s * by > 0 ? s * by : 0;
}
void flt_extent(const SwfFilter *f, int n, int *l, int *t, int *r, int *b) {
    double L = 0, T = 0, R = 0, B = 0; int i;
    for (i = 0; i < n; i++) {
        const SwfFilter *q = &f[i]; double rx = 0, ry = 0, dx = 0, dy = 0;
        switch (q->kind) {
        case SF_BLUR: blur_reach(fx16(q->blur_x), fx16(q->blur_y), passes_of(q), &rx, &ry); L += rx; R += rx; T += ry; B += ry; break;
        case SF_GLOW: case SF_GRADIENTGLOW: case SF_DROPSHADOW: case SF_BEVEL: case SF_GRADIENTBEVEL:
            blur_reach(fx16(q->blur_x), fx16(q->blur_y), passes_of(q), &rx, &ry);
            L += rx; R += rx; T += ry; B += ry;
            if (q->kind != SF_GLOW && !(q->kind == SF_GRADIENTGLOW && q->distance == 0)) {
                dx = cos(fx16(q->angle)) * fx16(q->distance); dy = sin(fx16(q->angle)) * fx16(q->distance);
                if (q->kind == SF_DROPSHADOW || q->kind == SF_GRADIENTGLOW) { if (dx < 0) L -= dx; else R += dx; if (dy < 0) T -= dy; else B += dy; }
                else { L += fabs(dx); R += fabs(dx); T += fabs(dy); B += fabs(dy); }
            }
            break;
        default: break;
        }
    }
    *l = (int)ceil(L); *t = (int)ceil(T); *r = (int)ceil(R); *b = (int)ceil(B);
}

/* Ruffle Filter::impotent: filters that change nothing are skipped */
int flt_impotent(const SwfFilter *f) {
    if (f->kind == SF_BLUR) return f->passes == 0 || (f->blur_x <= 65536 && f->blur_y <= 65536);
    if (f->kind == SF_COLORMATRIX) {
        static const float id[20] = { 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0 }; int i;
        for (i = 0; i < 20; i++) if (f->m[i] != id[i]) return 0;
        return 1;
    }
    return 0;
}
/* Filter::scale by the stage view scale (blur widths and distances) */
static s32 scale_blur16(s32 b, double k) { return (s32)((double)(b - 65536) * k) + 65536; }
void flt_scale(SwfFilter *f, double kx, double ky) {
    switch (f->kind) {
    case SF_BLUR: case SF_GLOW: f->blur_x = scale_blur16(f->blur_x, kx); f->blur_y = scale_blur16(f->blur_y, ky); break;
    case SF_DROPSHADOW: case SF_BEVEL: case SF_GRADIENTGLOW: case SF_GRADIENTBEVEL:
        f->blur_x = scale_blur16(f->blur_x, kx); f->blur_y = scale_blur16(f->blur_y, ky); f->distance = (s32)((double)f->distance * ky); break;
    case SF_DISPLACEMENT: f->view_x = (f->view_x > 0 ? f->view_x : 1.0f) * (float)kx; f->view_y = (f->view_y > 0 ? f->view_y : 1.0f) * (float)ky; break;
    default: break;
    }
}
/* Filter::calculate_dest_rect on a rectangle in twips */
void flt_dest_rect(const SwfFilter *f, s32 *x0, s32 *y0, s32 *x1, s32 *y1) {
    if (f->kind == SF_BLUR || f->kind == SF_GLOW || f->kind == SF_DROPSHADOW || f->kind == SF_BEVEL) {
        double sc = PASS_SCALES[(f->passes < 1 ? 1 : f->passes > 15 ? 15 : f->passes) - 1], bx = sc * fx16(f->blur_x), by = sc * fx16(f->blur_y);
        s32 gx = (s32)((bx > 0 ? bx : 0) * 20.0), gy = (s32)((by > 0 ? by : 0) * 20.0);
        *x0 -= gx; *x1 += gx; *y0 -= gy; *y1 += gy;
        if (f->kind == SF_DROPSHADOW || f->kind == SF_BEVEL) {
            double d = fx16(f->distance), a = fx16(f->angle); s32 x = (s32)(cos(a) * d * 20.0), y = (s32)(sin(a) * d * 20.0);
            if (f->kind == SF_DROPSHADOW) { if (x < 0) *x0 += x; else *x1 += x; if (y < 0) *y0 += y; else *y1 += y; }
            else { if (x < 0) { *x0 += x; *x1 -= x; } else { *x1 += x; *x0 -= x; } if (y < 0) { *y0 += y; *y1 -= y; } else { *y1 += y; *y0 -= y; } }
        }
    }
}

/* ---- blur ---- */

/* the layers are Rgba8 textures: a value is stored as the nearest 8-bit step */
static u32 q8(double v) { return v <= 0 ? 0 : v >= 1 ? 255 : (u32)floor(v * 255.0 + 0.5); }

/* one pass of Ruffle's blur.wgsl along a row or column of `len` premultiplied
   texels (stride apart; dst contiguous): a box of `full` texels whose two end texels weigh the
   fractional part, sampled clamped to the edge, the result floored to 8 bits */
#ifdef FLASHNT_SSE2_DISPATCH
#define BLUR_RUN_DECL static void blur_run_x87
#include "blur_run.inc"
void blur_run_sse2(const u32 *src, u32 *dst, int len, size_t stride, double full);
static void blur_run(const u32 *src, u32 *dst, int len, size_t stride, double full) {
    if (g_cpu_sse2) blur_run_sse2(src, dst, len, stride, full); else blur_run_x87(src, dst, len, stride, full);
}
#else
#define BLUR_RUN_DECL static void blur_run
#include "blur_run.inc"
#endif
/* Ruffle's BlurFilter::apply: `passes` times horizontal then vertical; an axis
   whose width is 1 or less is skipped; returns 0 when nothing was done */
static int blur_layer(u32 *px, int w, int h, double bx, double by, int passes) {
    u32 *tmp; int p, i, x, y, any = 0;
    if (passes <= 0 || w <= 0 || h <= 0) return 0;
    tmp = (u32*)malloc((size_t)(w > h ? w : h) * 4);
    if (!tmp) return 0;
    if (bx > 255) bx = 255; if (by > 255) by = 255;
    for (p = 0; p < passes; p++) for (i = 0; i < 2; i++) {
        double full = i == 0 ? bx : by;
        if (full <= 1.0) continue;
        any = 1;
        if (i == 0) for (y = 0; y < h; y++) { u32 *row = px + (size_t)y * w; blur_run(row, tmp, w, 1, full); memcpy(row, tmp, (size_t)w * 4); }
        else for (x = 0; x < w; x++) { int k; blur_run(px + x, tmp, h, (size_t)w, full); for (k = 0; k < h; k++) px[(size_t)k * w + x] = tmp[k]; }
    }
    free(tmp);
    return any;
}
void flt_blur(u32 *px, int w, int h, double bx, double by, int passes) { blur_layer(px, w, h, bx, by, passes); }

/* BitmapData.applyFilter's blur and colour matrix as Ruffle runs them: over
   the source rectangle (sx, sy, sw, sh) of a w x h texture sampled clamped to
   its edges; a blur's first pass reads past the rectangle, the later ones are
   clamped to it. Returns the sw x sh result (malloc'd), NULL when unsupported. */
static void color_matrix(u32 *px, int n, const float *m);
u32 *flt_displacement_rect(const u32 *tex, int W, int H, int sx, int sy, int sw, int sh, const SwfFilter *f, const Bitmap *map);
static u32 *glow_rect(const u32 *tex, int w, int h, int sx, int sy, int sw, int sh, const SwfFilter *f);
u32 *flt_apply_rect(const u32 *tex, int w, int h, int sx, int sy, int sw, int sh, const SwfFilter *f) {
    u32 *cur, *tmp, *ext; int x, y, p, i, first = 1, M = 0, len;
    if (f->kind == SF_DISPLACEMENT && f->map_bmd) { cur = flt_displacement_rect(tex, w, h, sx, sy, sw, sh, f, bmd_view((BitmapData*)f->map_bmd)); if (cur) return cur; }
    if (f->kind == SF_GLOW || f->kind == SF_DROPSHADOW || f->kind == SF_BEVEL) return glow_rect(tex, w, h, sx, sy, sw, sh, f);
    if (f->kind != SF_BLUR && f->kind != SF_COLORMATRIX && f->kind != SF_DISPLACEMENT) return NULL;
    if (sw <= 0 || sh <= 0 || w <= 0 || h <= 0) return NULL;
    cur = (u32*)malloc((size_t)sw * sh * 4 + 4);
    if (!cur) return NULL;
#define TEXC(X, Y) tex[(size_t)((Y) < 0 ? 0 : (Y) >= h ? h - 1 : (Y)) * w + ((X) < 0 ? 0 : (X) >= w ? w - 1 : (X))]
    for (y = 0; y < sh; y++) for (x = 0; x < sw; x++) cur[(size_t)y * sw + x] = TEXC(sx + x, sy + y);
    if (f->kind == SF_COLORMATRIX) { color_matrix(cur, sw * sh, f->m); return cur; }
    if (f->kind == SF_DISPLACEMENT) return cur;   /* no map: a plain copy (Ruffle's fallback blit) */
    {
        double bx = fx16(f->blur_x), by = fx16(f->blur_y);
        if (bx > 255) bx = 255; if (by > 255) by = 255;
        len = (sw > sh ? sw : sh) + 2 * 260;
        tmp = (u32*)malloc((size_t)len * 4); ext = (u32*)malloc((size_t)len * 4);
        if (!tmp || !ext) { free(tmp); free(ext); return cur; }
        for (p = 0; p < f->passes; p++) for (i = 0; i < 2; i++) {
            double full = i == 0 ? bx : by;
            if (full <= 1.0) continue;
            if (first) {
                /* the first pass samples the source texture itself */
                float radius = (float)((full - 1.0) / 2.0); M = (int)ceil(radius) + 1;
                if (i == 0) for (y = 0; y < sh; y++) {
                    for (x = 0; x < sw + 2 * M; x++) ext[x] = TEXC(sx - M + x, sy + y);
                    blur_run(ext, tmp, sw + 2 * M, 1, full);
                    memcpy(cur + (size_t)y * sw, tmp + M, (size_t)sw * 4);
                } else for (x = 0; x < sw; x++) {
                    for (y = 0; y < sh + 2 * M; y++) ext[y] = TEXC(sx + x, sy - M + y);
                    blur_run(ext, tmp, sh + 2 * M, 1, full);
                    for (y = 0; y < sh; y++) cur[(size_t)y * sw + x] = tmp[M + y];
                }
                first = 0;
            } else if (i == 0) for (y = 0; y < sh; y++) { u32 *row = cur + (size_t)y * sw; blur_run(row, tmp, sw, 1, full); memcpy(row, tmp, (size_t)sw * 4); }
            else for (x = 0; x < sw; x++) { blur_run(cur + x, tmp, sh, (size_t)sw, full); for (y = 0; y < sh; y++) cur[(size_t)y * sw + x] = tmp[y]; }
        }
        free(tmp); free(ext);
    }
#undef TEXC
    return cur;
}

/* the blurred copy of a layer for glow / shadow / bevel (the source itself when the blur does nothing) */
static u32 *blurred_copy(const u32 *px, int w, int h, const SwfFilter *f) {
    u32 *b = (u32*)malloc((size_t)w * h * 4 + 4);
    if (!b) return NULL;
    memcpy(b, px, (size_t)w * h * 4);
    blur_layer(b, w, h, fx16(f->blur_x), fx16(f->blur_y), f->passes);
    return b;
}
/* where glow / bevel read the blurred alpha (Ruffle's blur uv): texel x of the
   output samples u = (x0 + x + 0.5 + offset) / W of a bw x bh texture */
typedef struct { const u32 *b; int bw, bh; float x0, y0, W, H; } BlurSrc;
/* nearest sample of the blurred alpha at texel (x, y) moved by (ox, oy); 0 off the texture (glow.wgsl) */
static double blur_at(const BlurSrc *s, int x, int y, float ox, float oy) {
    float u = (s->x0 + (float)x + 0.5f + ox) / s->W, v = (s->y0 + (float)y + 0.5f + oy) / s->H; int tx, ty;
    if (u < 0 || u > 1 || v < 0 || v > 1) return 0;
    tx = (int)floor(u * s->bw); ty = (int)floor(v * s->bh);
    if (tx >= s->bw) tx = s->bw - 1; if (ty >= s->bh) ty = s->bh - 1; if (tx < 0) tx = 0; if (ty < 0) ty = 0;
    return (s->b[(size_t)ty * s->bw + tx] >> 24) / 255.0;
}
static void blur_src_layer(BlurSrc *s, const u32 *b, int w, int h) { s->b = b; s->bw = w; s->bh = h; s->x0 = 0; s->y0 = 0; s->W = (float)w; s->H = (float)h; }
static double sat(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static u32 pack(double a, double r, double g, double b) {
    return (q8(a) << 24) | (q8(r) << 16) | (q8(g) << 8) | q8(b);
}
#define CA(c) (((c) >> 24) / 255.0)
#define CR(c) ((((c) >> 16) & 255) / 255.0)
#define CG(c) ((((c) >> 8) & 255) / 255.0)
#define CB(c) (((c) & 255) / 255.0)

/* glow, drop shadow, gradient glow (glow.wgsl; the gradient maps the alpha);
   (ox, oy): where the blur is sampled relative to the pixel */
static void glow_ex(u32 *px, int w, int h, const SwfFilter *f, float ox, float oy, const u32 *lut, const BlurSrc *bs) {
    u32 *bl = bs ? NULL : blurred_copy(px, w, h, f); int x, y; BlurSrc own; float str = (float)(f->strength / 256.0);
    int inner = (f->flags & SFF_INNER) != 0, knock = (f->flags & SFF_KNOCKOUT) != 0, comp = (f->flags & SFF_COMPOSITE) != 0;
    double cr = f->color[0] / 255.0, cg = f->color[1] / 255.0, cb = f->color[2] / 255.0, cal = f->color[3] / 255.0;
    if (!bs) { if (!bl) return; blur_src_layer(&own, bl, w, h); bs = &own; }
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
        u32 d = px[(size_t)y * w + x]; double blur = blur_at(bs, x, y, ox, oy), al, r, g, b, a, da = CA(d);
        double qr = cr, qg = cg, qb = cb;
        if (lut) {   /* gradient: colour and alpha by the blurred strength */
            int idx = (int)(sat(inner ? (1 - blur) * str : blur * str) * 255); u32 c = lut[idx];
            al = (c >> 24) / 255.0; qr = ((c >> 16) & 255) / 255.0; qg = ((c >> 8) & 255) / 255.0; qb = (c & 255) / 255.0;
        } else al = cal * sat((inner ? 1 - blur : blur) * str);
        if (inner) {
            if (knock || !comp) { r = qr * al * da; g = qg * al * da; b = qb * al * da; a = al * da; }
            else { r = qr * al * da + CR(d) * (1 - al); g = qg * al * da + CG(d) * (1 - al); b = qb * al * da + CB(d) * (1 - al); a = al * da + da * (1 - al); }
        } else {
            if (knock) { r = qr * al * (1 - da); g = qg * al * (1 - da); b = qb * al * (1 - da); a = al * (1 - da); }
            else if (comp) { r = qr * al * (1 - da) + CR(d); g = qg * al * (1 - da) + CG(d); b = qb * al * (1 - da) + CB(d); a = al * (1 - da) + da; }
            else { r = qr * al; g = qg * al; b = qb * al; a = al; }
        }
        px[(size_t)y * w + x] = pack(a, r, g, b);
    }
    free(bl);
}
static void glow(u32 *px, int w, int h, const SwfFilter *f, float ox, float oy, const u32 *lut) { glow_ex(px, w, h, f, ox, oy, lut, NULL); }

/* bevel and gradient bevel (bevel.wgsl) */
static void bevel_ex(u32 *px, int w, int h, const SwfFilter *f, const u32 *lut, const BlurSrc *bs) {
    u32 *bl = bs ? NULL : blurred_copy(px, w, h, f); int x, y; BlurSrc own; float str = (float)(f->strength / 256.0);
    float dist = (float)fx16(f->distance), ang = (float)fx16(f->angle), ox = (float)cos(ang) * dist, oy = (float)sin(ang) * dist;
    int knock = (f->flags & SFF_KNOCKOUT) != 0, full = (f->flags & SFF_ONTOP) != 0, inner_f = (f->flags & SFF_INNER) != 0;
    int outer = full || !inner_f, inner = full || inner_f;
    /* SWF bevel colours: color = shadow, color2 = highlight (premultiplied for the shader) */
    double sa = f->color[3] / 255.0, sr = f->color[0] / 255.0 * sa, sg = f->color[1] / 255.0 * sa, sb = f->color[2] / 255.0 * sa;
    double ha = f->color2[3] / 255.0, hr = f->color2[0] / 255.0 * ha, hg = f->color2[1] / 255.0 * ha, hb = f->color2[2] / 255.0 * ha;
    if (!bs) { if (!bl) return; blur_src_layer(&own, bl, w, h); bs = &own; }
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
        u32 d = px[(size_t)y * w + x]; double l = blur_at(bs, x, y, ox, oy), rr = blur_at(bs, x, y, -ox, -oy);
        double gr, gg, gb, ga, r, g, b, a, da = CA(d);
        if (lut) {
            double v = sat((l - rr) * str * 0.5 + 0.5); u32 c = lut[(int)(v * 255)];
            ga = (c >> 24) / 255.0; gr = ((c >> 16) & 255) / 255.0 * ga; gg = ((c >> 8) & 255) / 255.0 * ga; gb = (c & 255) / 255.0 * ga;
        } else {
            double hi = sat((l - rr) * str), sh = sat((rr - l) * str);
            gr = hr * hi + sr * sh; gg = hg * hi + sg * sh; gb = hb * hi + sb * sh; ga = ha * hi + sa * sh;
        }
        if (inner && outer) {
            if (knock) { r = gr; g = gg; b = gb; a = ga; }
            else { r = CR(d) - CR(d) * ga + gr; g = CG(d) - CG(d) * ga + gg; b = CB(d) - CB(d) * ga + gb; a = da - da * ga + ga; }
        } else if (inner) {
            if (knock) { r = gr * da; g = gg * da; b = gb * da; a = ga * da; }
            else { r = gr * da + CR(d) * (1 - ga); g = gg * da + CG(d) * (1 - ga); b = gb * da + CB(d) * (1 - ga); a = ga * da + da * (1 - ga); }
        } else {
            if (knock) { r = gr - gr * da; g = gg - gg * da; b = gb - gb * da; a = ga - ga * da; }
            else { r = CR(d) + gr - gr * da; g = CG(d) + gg - gg * da; b = CB(d) + gb - gb * da; a = da + ga - ga * da; }
        }
        px[(size_t)y * w + x] = pack(a, r, g, b);
    }
    free(bl);
}
static void bevel(u32 *px, int w, int h, const SwfFilter *f, const u32 *lut) { bevel_ex(px, w, h, f, lut, NULL); }

/* applyFilter's glow, drop shadow and bevel (Ruffle wgpu): the filter runs over
   the source rectangle; the blur is the source-sized blur of that rectangle
   (or the source texture itself when it blurs nothing), read with uvs of the
   source texture */
static u32 *glow_rect(const u32 *tex, int w, int h, int sx, int sy, int sw, int sh, const SwfFilter *f) {
    SwfFilter bf; u32 *cur, *bl = NULL; BlurSrc bs; int x, y; double bx = fx16(f->blur_x), by = fx16(f->blur_y);
    if (sw <= 0 || sh <= 0 || w <= 0 || h <= 0) return NULL;
    cur = (u32*)malloc((size_t)sw * sh * 4 + 4);
    if (!cur) return NULL;
    for (y = 0; y < sh; y++) for (x = 0; x < sw; x++) {
        int X = sx + x, Y = sy + y;
        X = X < 0 ? 0 : X >= w ? w - 1 : X; Y = Y < 0 ? 0 : Y >= h ? h - 1 : Y;
        cur[(size_t)y * sw + x] = tex[(size_t)Y * w + X];
    }
    memset(&bf, 0, sizeof(bf)); bf.kind = SF_BLUR; bf.blur_x = f->blur_x; bf.blur_y = f->blur_y; bf.passes = f->passes;
    if (f->passes > 0 && (bx > 1.0 || by > 1.0)) bl = flt_apply_rect(tex, w, h, sx, sy, sw, sh, &bf);
    if (bl) { bs.b = bl; bs.bw = sw; bs.bh = sh; } else { bs.b = tex; bs.bw = w; bs.bh = h; }
    bs.x0 = (float)sx; bs.y0 = (float)sy; bs.W = (float)w; bs.H = (float)h;
    if (f->kind == SF_GLOW) glow_ex(cur, sw, sh, f, 0, 0, NULL, &bs);
    else if (f->kind == SF_DROPSHADOW) glow_ex(cur, sw, sh, f, -(float)cos(fx16(f->angle)) * (float)fx16(f->distance), -(float)sin(fx16(f->angle)) * (float)fx16(f->distance), NULL, &bs);
    else bevel_ex(cur, sw, sh, f, NULL, &bs);
    free(bl);
    return cur;
}

static void gradient_lut(const SwfFilter *f, u32 *lut) {
    int i, k;
    for (i = 0; i < 256; i++) {
        int n = f->ncolors; u8 c[4];
        if (n <= 0) { lut[i] = 0; continue; }
        if (i <= f->ratio[0]) memcpy(c, f->gcolor[0], 4);
        else if (i >= f->ratio[n - 1]) memcpy(c, f->gcolor[n - 1], 4);
        else {
            for (k = 1; k < n && f->ratio[k] < i; k++) ;
            { int r0 = f->ratio[k - 1], r1 = f->ratio[k]; double t = r1 > r0 ? (double)(i - r0) / (r1 - r0) : 0; int j;
              for (j = 0; j < 4; j++) c[j] = (u8)(f->gcolor[k - 1][j] + (f->gcolor[k][j] - f->gcolor[k - 1][j]) * t + 0.5); }
        }
        lut[i] = ((u32)c[3] << 24) | ((u32)c[0] << 16) | ((u32)c[1] << 8) | c[2];
    }
}

static void color_matrix(u32 *px, int n, const float *m) {
    int i;
    for (i = 0; i < n; i++) {
        u32 c = px[i]; double a = CA(c), r = a ? CR(c) / a : 0, g = a ? CG(c) / a : 0, b = a ? CB(c) / a : 0, R, G, B, A;
        R = sat(m[0] * r + m[1] * g + m[2] * b + m[3] * a + m[4] / 255.0);
        G = sat(m[5] * r + m[6] * g + m[7] * b + m[8] * a + m[9] / 255.0);
        B = sat(m[10] * r + m[11] * g + m[12] * b + m[13] * a + m[14] / 255.0);
        A = sat(m[15] * r + m[16] * g + m[17] * b + m[18] * a + m[19] / 255.0);
        px[i] = pack(A, R * A, G * A, B * A);
    }
}

/* ConvolutionFilter: on unpremultiplied colour, edges clamped or the substitute colour */
static void convolution(u32 *px, int w, int h, const SwfFilter *f) {
    u32 *src; int x, y, i, j, mx = f->mx, my = f->my; double div = f->divisor ? f->divisor : 1, bias = f->bias;
    int clamp = (f->flags & SFF_CLAMP) != 0, pres = (f->flags & SFF_PRESERVE) != 0;
    u32 sub = ((u32)f->color[3] << 24) | ((u32)f->color[0] << 16) | ((u32)f->color[1] << 8) | f->color[2];
    if (!f->cmatrix || mx <= 0 || my <= 0 || f->ncm < mx * my) return;
    src = (u32*)malloc((size_t)w * h * 4);
    if (!src) return;
    for (i = 0; i < w * h; i++) src[i] = bmd_unmul(px[i]);
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
        double s[4] = { 0, 0, 0, 0 }; u32 o = src[(size_t)y * w + x]; int ch;
        for (j = 0; j < my; j++) for (i = 0; i < mx; i++) {
            int sx = x + i - mx / 2, sy = y + j - my / 2; u32 c; double k = f->cmatrix[j * mx + i];
            if (sx < 0 || sy < 0 || sx >= w || sy >= h) { if (clamp) { sx = sx < 0 ? 0 : sx >= w ? w - 1 : sx; sy = sy < 0 ? 0 : sy >= h ? h - 1 : sy; c = src[(size_t)sy * w + sx]; } else c = sub; }
            else c = src[(size_t)sy * w + sx];
            s[0] += k * (c >> 24); s[1] += k * ((c >> 16) & 255); s[2] += k * ((c >> 8) & 255); s[3] += k * (c & 255);
        }
        for (ch = 0; ch < 4; ch++) { s[ch] = s[ch] / div + bias; if (s[ch] < 0) s[ch] = 0; if (s[ch] > 255) s[ch] = 255; }
        { u32 a = pres ? (o >> 24) : (u32)s[0]; px[(size_t)y * w + x] = bmd_premul((a << 24) | ((u32)s[1] << 16) | ((u32)s[2] << 8) | (u32)s[3], 1); }
    }
    free(src);
}

/* DisplacementMapFilter (displacement_map.wgsl); ox, oy: the layer's origin
   relative to the object's (0,0) in device px */
/* one bilinear sample (repeat addressing, 8-bit weights) of a premultiplied texture, as floats 0..255 */
static void tex_bilinear_repeat(const u32 *t, int W, int H, float u, float v, float *o) {
    float tx = u * (float)W - 0.5f, ty = v * (float)H - 0.5f, fx, fy; int x0, y0, x1, y1, c;
    float x0f = (float)floor(tx), y0f = (float)floor(ty);
    fx = (float)floor((tx - x0f) * 256.0f + 0.5f) / 256.0f; fy = (float)floor((ty - y0f) * 256.0f + 0.5f) / 256.0f;
    x0 = (int)x0f % W; if (x0 < 0) x0 += W; y0 = (int)y0f % H; if (y0 < 0) y0 += H;
    x1 = (x0 + 1) % W; y1 = (y0 + 1) % H;
    for (c = 0; c < 4; c++) {
        int sh = c == 0 ? 24 : c == 1 ? 16 : c == 2 ? 8 : 0;
        float a = (float)((t[(size_t)y0 * W + x0] >> sh) & 255), b = (float)((t[(size_t)y0 * W + x1] >> sh) & 255);
        float d = (float)((t[(size_t)y1 * W + x0] >> sh) & 255), e = (float)((t[(size_t)y1 * W + x1] >> sh) & 255);
        float top = a + (b - a) * fx, bot = d + (e - d) * fx;
        o[c] = top + (bot - top) * fy;
    }
}
/* Ruffle's displacement_map.wgsl over the rectangle (sx, sy, sw, sh) of a W x H
   texture: the map is read (nearest, clamped) at the texture position less the
   map point, the source bilinearly (repeating) at the displaced position */
u32 *flt_displacement_rect(const u32 *tex, int W, int H, int sx, int sy, int sw, int sh, const SwfFilter *f, const Bitmap *map) {
    u32 *out; int x, y; float vx = f->view_x > 0 ? f->view_x : 1.0f, vy = f->view_y > 0 ? f->view_y : 1.0f;
    if (sw <= 0 || sh <= 0 || W <= 0 || H <= 0 || !map || !map->px || !map->w || !map->h) return NULL;
    out = (u32*)malloc((size_t)sw * sh * 4 + 4);
    if (!out) return NULL;
    for (y = 0; y < sh; y++) for (x = 0; x < sw; x++) {
        float u = ((float)(sx + x) + 0.5f) / (float)W, v = ((float)(sy + y) + 0.5f) / (float)H;
        float spx = u * (float)W, spy = v * (float)H, mu, mv, cx, cy, dx, dy, du, dv, o[4]; int oob, inmap; u32 m = 0, r;
        mu = (spx - (float)f->map_x) / vx / (float)map->w;
        mv = (spy - (float)f->map_y) / vy / (float)map->h;
        inmap = !(mu < 0 || mu > 1 || mv < 0 || mv > 1);
        if (inmap) {
            int mx = (int)floor(mu * map->w), my = (int)floor(mv * map->h);
            if (mx >= map->w) mx = map->w - 1; if (my >= map->h) my = map->h - 1;
            m = map->px[(size_t)my * map->w + mx];
        }
        #define COMPF(ch) ((ch) == 1 ? (float)((m >> 16) & 255) : (ch) == 2 ? (float)((m >> 8) & 255) : (ch) == 4 ? (float)(m & 255) : (ch) == 8 ? (float)(m >> 24) : 128.0f)
        if (!inmap) { cx = (f->comp_x == 1 || f->comp_x == 2 || f->comp_x == 4 || f->comp_x == 8) ? 127.5f : 128.0f; cy = (f->comp_y == 1 || f->comp_y == 2 || f->comp_y == 4 || f->comp_y == 8) ? 127.5f : 128.0f; }   /* outside the map: 0.5 in every channel */
        else { cx = COMPF(f->comp_x); cy = COMPF(f->comp_y); }
        #undef COMPF
        dx = spx + (cx - 128.0f) * (vx * f->scale_x) / 256.0f; dy = spy + (cy - 128.0f) * (vy * f->scale_y) / 256.0f;
        du = dx / (float)W; dv = dy / (float)H;
        oob = du < 0 || du > 1 || dv < 0 || dv > 1;
        if (f->mode == 1) { du = du < 0 ? 0 : du > 1 ? 1 : du; dv = dv < 0 ? 0 : dv > 1 ? 1 : dv; }
        else if (f->mode == 2 && oob) { du = u; dv = v; }
        if (f->mode == 3 && oob) {
            float a = f->color[3] / 255.0f;
            r = ((u32)floor(a * 255.0f + 0.5f) << 24) | ((u32)floor(f->color[0] / 255.0f * a * 255.0f + 0.5f) << 16) | ((u32)floor(f->color[1] / 255.0f * a * 255.0f + 0.5f) << 8) | (u32)floor(f->color[2] / 255.0f * a * 255.0f + 0.5f);
        } else {
            tex_bilinear_repeat(tex, W, H, du, dv, o);
            r = ((u32)floor(o[0] + 0.5f) << 24) | ((u32)floor(o[1] + 0.5f) << 16) | ((u32)floor(o[2] + 0.5f) << 8) | (u32)floor(o[3] + 0.5f);
        }
        out[(size_t)y * sw + x] = r;
    }
    return out;
}
static void displacement(u32 *px, int w, int h, const SwfFilter *f, const Bitmap *map, double ox, double oy) {
    u32 *o = map ? flt_displacement_rect(px, w, h, 0, 0, w, h, f, map) : NULL;
    (void)ox; (void)oy;
    if (!o) return;
    memcpy(px, o, (size_t)w * h * 4);
    free(o);
}

void flt_apply(u32 *px, int w, int h, const SwfFilter *f, double ox, double oy) {
    u32 lut[256];
    if (!px || w <= 0 || h <= 0) return;
    switch (f->kind) {
    case SF_BLUR: flt_blur(px, w, h, fx16(f->blur_x), fx16(f->blur_y), f->passes); break;
    case SF_GLOW: glow(px, w, h, f, 0, 0, NULL); break;
    case SF_DROPSHADOW: glow(px, w, h, f, -(float)cos(fx16(f->angle)) * (float)fx16(f->distance), -(float)sin(fx16(f->angle)) * (float)fx16(f->distance), NULL); break;
    case SF_GRADIENTGLOW: gradient_lut(f, lut); glow(px, w, h, f, -(float)cos(fx16(f->angle)) * (float)fx16(f->distance), -(float)sin(fx16(f->angle)) * (float)fx16(f->distance), lut); break;
    case SF_BEVEL: bevel(px, w, h, f, NULL); break;
    case SF_GRADIENTBEVEL: gradient_lut(f, lut); bevel(px, w, h, f, lut); break;
    case SF_COLORMATRIX: color_matrix(px, w * h, f->m); break;
    case SF_CONVOLUTION: convolution(px, w, h, f); break;
    case SF_DISPLACEMENT: displacement(px, w, h, f, f->map_bmd ? bmd_view((BitmapData*)f->map_bmd) : NULL, ox, oy); break;
    case SF_SHADER: pbj_filter_layer(f->pbj, px, w, h); break;
    default: break;
    }
}

/* ---- blend modes ---- */

static double blend_ch(int mode, double cs, double cd) {
    switch (mode) {
    case BLEND_MULTIPLY: return cs * cd;
    case BLEND_SCREEN: return cs + cd - cs * cd;
    case BLEND_LIGHTEN: return cs > cd ? cs : cd;
    case BLEND_DARKEN: return cs < cd ? cs : cd;
    case BLEND_DIFFERENCE: return fabs(cs - cd);
    case BLEND_ADD: return cs + cd > 1 ? 1 : cs + cd;
    case BLEND_SUBTRACT: return cd - cs < 0 ? 0 : cd - cs;
    case BLEND_INVERT: return 1 - cd;
    case BLEND_OVERLAY: return cd < 0.5 ? 2 * cs * cd : 1 - 2 * (1 - cs) * (1 - cd);
    case BLEND_HARDLIGHT: return cs < 0.5 ? 2 * cs * cd : 1 - 2 * (1 - cs) * (1 - cd);
    default: return cs;
    }
}

/* the layer's colour transform (premultiplied in, premultiplied out); NULL when identity */
static u32 *layer_cxform(const u32 *layer, size_t n, const SWFCxform *cx) {
    u32 *tl; size_t i;
    if (!cx || (cx->ma == 256 && cx->mr == 256 && cx->mg == 256 && cx->mb == 256 && !cx->aa && !cx->ar && !cx->ag && !cx->ab)) return NULL;
    tl = (u32*)malloc(n * 4 + 4);
    if (!tl) return NULL;
    for (i = 0; i < n; i++) {
        u8 R, G, B, A; u32 u = bmd_unmul(layer[i]);
        R = (u8)(u >> 16); G = (u8)(u >> 8); B = (u8)u; A = (u8)(u >> 24); cxform_apply(cx, &R, &G, &B, &A);
        tl[i] = bmd_premul(((u32)A << 24) | ((u32)R << 16) | ((u32)G << 8) | B, 1);
    }
    return tl;
}

/* the layer (premultiplied, lw x lh at lx, ly) onto the surface with a blend
   mode, like Ruffle's wgpu backend: normal / layer / add / subtract / screen
   are GPU blend states, the others a shader reading the resolved parent
   (alpha and erase only when a Layer is above: in_layer) */
void flt_composite(Surface *s, const u32 *layer, int lx, int ly, int lw, int lh, int mode, const SWFCxform *cx, int in_layer) {
    u32 *tl = layer_cxform(layer, (size_t)lw * lh, cx), *out; int x, y;
    if (tl) layer = tl;
    switch (mode) {
    case BLEND_ADD: surf_draw_pixels(s, layer, lx, ly, lw, lh, TB_ADD); free(tl); return;
    case BLEND_SUBTRACT: surf_draw_pixels(s, layer, lx, ly, lw, lh, TB_SUBTRACT); free(tl); return;
    case BLEND_SCREEN: surf_draw_pixels(s, layer, lx, ly, lw, lh, TB_SCREEN); free(tl); return;
    case BLEND_MULTIPLY: case BLEND_LIGHTEN: case BLEND_DARKEN: case BLEND_DIFFERENCE: case BLEND_INVERT:
    case BLEND_OVERLAY: case BLEND_HARDLIGHT: case BLEND_ALPHA: case BLEND_ERASE: break;
    default: surf_draw_pixels(s, layer, lx, ly, lw, lh, TB_NORMAL); free(tl); return;
    }
    if ((mode == BLEND_ALPHA || mode == BLEND_ERASE) && !in_layer) { free(tl); return; }
    out = (u32*)malloc((size_t)lw * lh * 4 + 4);
    if (!out) { free(tl); return; }
    surf_resolve(s);
    for (y = 0; y < lh; y++) for (x = 0; x < lw; x++) {
        int sx = lx + x, sy = ly + y; u32 c = layer[(size_t)y * lw + x], d; double sa, da, r, g, b, a, dr, dg, db;
        u32 *o = &out[(size_t)y * lw + x];
        if (sx < 0 || sy < 0 || sx >= s->w || sy >= s->h) { *o = 0; continue; }
        d = s->px[(size_t)sy * s->w + sx];
        if (!(c >> 24)) { *o = d; continue; }   /* discard: the parent stays */
        sa = CA(c); da = CA(d); dr = CR(d); dg = CG(d); db = CB(d);
        if (mode == BLEND_ALPHA) { r = dr * sa; g = dg * sa; b = db * sa; a = sa * da; }
        else if (mode == BLEND_ERASE) { r = dr * (1 - sa); g = dg * (1 - sa); b = db * (1 - sa); a = (1 - sa) * da; }
        else {
            double ur = CR(c) / sa, ug = CG(c) / sa, ub = CB(c) / sa, vr = da > 0 ? dr / da : 0, vg = da > 0 ? dg / da : 0, vb = da > 0 ? db / da : 0;
            r = CR(c) * (1 - da) + dr * (1 - sa) + sa * da * blend_ch(mode, ur, vr);
            g = CG(c) * (1 - da) + dg * (1 - sa) + sa * da * blend_ch(mode, ug, vg);
            b = CB(c) * (1 - da) + db * (1 - sa) + sa * da * blend_ch(mode, ub, vb);
            a = sa + da * (1 - sa);
        }
        *o = ((u32)(sat(a) * 255 + 0.5) << 24) | ((u32)(sat(r) * 255 + 0.5) << 16) | ((u32)(sat(g) * 255 + 0.5) << 8) | (u32)(sat(b) * 255 + 0.5);
    }
    /* written over the parent (through its mask) */
    surf_draw_pixels_where(s, out, layer, lx, ly, lw, lh);
    free(out); free(tl);
}

/* blendShader: the kernel gets the surface as its first image and the layer
   (colour transformed) as its second */
void flt_composite_shader(Surface *s, const u32 *layer, int lx, int ly, int lw, int lh, const SWFCxform *cx, void *job) {
    u32 *tl = layer_cxform(layer, (size_t)lw * lh, cx), *bg, *out; size_t n = (size_t)s->w * s->h; int x, y;
    if (tl) layer = tl;
    surf_resolve(s);
    bg = (u32*)malloc(n * 4 + 4); out = (u32*)malloc((size_t)lw * lh * 4 + 4);
    if (bg && out) {
        memcpy(bg, s->px, n * 4);
        pbj_blend_layer((PbjJob*)job, bg, s->w, s->h, layer, lx, ly, lw, lh);
        for (y = 0; y < lh; y++) for (x = 0; x < lw; x++) {
            int sx = lx + x, sy = ly + y;
            out[(size_t)y * lw + x] = (sx < 0 || sy < 0 || sx >= s->w || sy >= s->h) ? 0 : bg[(size_t)sy * s->w + sx];
        }
        surf_draw_pixels(s, out, lx, ly, lw, lh, TB_REPLACE);
    }
    free(bg); free(out); free(tl);
}

/* BitmapData.applyFilter: the source rectangle, filtered, written (not
   blended) at destPoint, with the filter's margins around it */
void bmd_apply_filter(BitmapData *dst, BitmapData *src, s32 sx, s32 sy, s32 sw, s32 sh, s32 dx, s32 dy, const SwfFilter *f) {
    int x, y; BitmapData *tmp = NULL;
    if (!dst || !src || dst->disposed || src->disposed || sw <= 0 || sh <= 0) return;
    if (src == dst) { tmp = bmd_clone(src); if (!tmp) return; src = tmp; }
    if (f->kind == SF_SHADER) {
        /* Ruffle: the kernel covers the source size, its first input is the
           whole source bitmap; the result is copied (clipped) to destPoint */
        PbjJob *j = f->pbj; PbjTex tex[16]; float *out; int t;
        sx = sy = 0; sw = (s32)src->w; sh = (s32)src->h;   /* Flash ignores sourceRect for shaders (Ruffle) */
        if (j && (double)sw * sh <= 16777216.0 && (out = (float*)malloc((size_t)sw * sh * 4 * sizeof(float))) != NULL) {
            memcpy(tex, j->tex, sizeof(tex));
            for (t = 0; t < j->shader->nparams; t++) if (j->shader->params[t].texture) {
                PbjTex *q = &tex[j->shader->params[t].tex_index & 15];
                memset(q, 0, sizeof(*q)); q->present = 1; q->w = q->pw = (int)src->w; q->h = q->ph = (int)src->h; q->px = src->px;
                break;
            }
            if (pbj_run(j->shader, j->vals, tex, 16, 0, 0, sw, sh, 1, out))
                for (y = 0; y < sh; y++) for (x = 0; x < sw; x++) {
                    s32 px = dx + x, py = dy + y; const float *o = out + ((size_t)y * sw + x) * 4; u32 c;
                    if (px < 0 || py < 0 || (u32)px >= dst->w || (u32)py >= dst->h) continue;
                    c = ((u32)pbj_unorm8(o[3]) << 24) | ((u32)pbj_unorm8(o[0]) << 16) | ((u32)pbj_unorm8(o[1]) << 8) | pbj_unorm8(o[2]);
                    if (!dst->transparent) { c = bmd_unmul(c); c = 0xFF000000u | (c & 0xFFFFFFu); }
                    dst->px[(size_t)py * dst->w + px] = c;
                }
            free(out);
            bmd_changed(dst);
        }
        if (tmp) bmd_unref(tmp);
        return;
    }
    {   /* Ruffle: the source-sized result replaces the pixels at destPoint */
        u32 *o = flt_apply_rect(src->px, (int)src->w, (int)src->h, sx, sy, sw, sh, f);
        if (o) {
            for (y = 0; y < sh; y++) for (x = 0; x < sw; x++) {
                s32 px = dx + x, py = dy + y; u32 c;
                if (px < 0 || py < 0 || (u32)px >= dst->w || (u32)py >= dst->h) continue;
                c = o[(size_t)y * sw + x];
                if (!dst->transparent) { c = bmd_unmul(c); c = 0xFF000000u | (c & 0xFFFFFFu); }
                dst->px[(size_t)py * dst->w + px] = c;
            }
            free(o);
            if (tmp) bmd_unref(tmp);
            bmd_changed(dst);
            return;
        }
    }
    /* filters Ruffle's renderer lacks (convolution, gradient glow / bevel): a plain clipped copy */
    for (y = 0; y < sh; y++) for (x = 0; x < sw; x++) {
        s32 qx = sx + x, qy = sy + y, px = dx + x, py = dy + y; u32 c;
        if (qx < 0 || qy < 0 || (u32)qx >= src->w || (u32)qy >= src->h) continue;
        if (px < 0 || py < 0 || (u32)px >= dst->w || (u32)py >= dst->h) continue;
        c = src->px[(size_t)qy * src->w + qx];
        if (!dst->transparent) { c = bmd_unmul(c); c = 0xFF000000u | (c & 0xFFFFFFu); }
        dst->px[(size_t)py * dst->w + px] = c;
    }
    if (tmp) bmd_unref(tmp);
    bmd_changed(dst);
}
/* BitmapData.generateFilterRect */
void bmd_filter_rect(const SwfFilter *f, double *x, double *y, double *w, double *h) {
    int l, t, r, b; flt_extent(f, 1, &l, &t, &r, &b);
    *x -= l; *y -= t; *w += l + r; *h += t + b;
}
