/* bmd.c - BitmapData pixel storage and operations (see bmd.h).
 *
 * A faithful port of Ruffle's core/src/bitmap (bitmap_data.rs,
 * operations.rs, turbulence.rs), including the Flash Player quirks it
 * reproduces. */
#include "bmd.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define A_(c) ((u32)(c) >> 24)
#define R_(c) (((u32)(c) >> 16) & 255u)
#define G_(c) (((u32)(c) >> 8) & 255u)
#define B_(c) ((u32)(c) & 255u)
#define RGBA(r, g, b, a) ((((u32)(a) & 255u) << 24) | (((u32)(r) & 255u) << 16) | (((u32)(g) & 255u) << 8) | ((u32)(b) & 255u))
#define WITH_ALPHA(c, a) (((u32)(c) & 0x00FFFFFFu) | (((u32)(a) & 255u) << 24))
#define PX(b, x, y) ((b)->px[(size_t)(y) * (b)->w + (x)])

/* Rust `as` casts from f64: saturating, NaN -> 0 */
static u8 f_u8(double d) { if (d != d || d <= 0) return 0; if (d >= 255) return 255; return (u8)d; }
static s32 f_i32(double d) { if (d != d) return 0; if (d >= 2147483647.0) return 2147483647; if (d <= -2147483648.0) return (s32)0x80000000; return (s32)d; }

/* ---------------------------------------------------------------- */
/* colours                                                          */

u32 bmd_premul(u32 c, int transparent) {
    u32 a = transparent ? A_(c) : 255u;
    u32 r = (R_(c) * a + 127) / 255, g = (G_(c) * a + 127) / 255, b = (B_(c) * a + 127) / 255;
    return RGBA(r, g, b, a);
}

/* brute-forced by Ruffle to match Flash's unmultiply results */
static const u32 premul_factor[256] = {
    0, 16678912, 8339456, 5559638, 4169728, 3335783, 2779819, 2386603, 2086230, 1855488,
    1667892, 1518251, 1391151, 1285234, 1193302, 1111928, 1043895, 981113, 927744, 879275,
    834621, 795535, 759126, 726358, 695839, 668183, 642538, 618737, 596651, 576171, 555964,
    538706, 522104, 506319, 490557, 477321, 464038, 451353, 439544, 428244, 417582, 407500,
    397768, 388535, 379630, 371117, 363179, 355235, 348050, 340965, 334052, 327038, 321269,
    315077, 309159, 303586, 298189, 293092, 287981, 283080, 278251, 273892, 269268, 265179,
    261087, 256971, 253160, 249322, 245508, 242164, 238575, 235245, 231859, 228848, 225785,
    222712, 219616, 216827, 213985, 211432, 208835, 206075, 203750, 201196, 198895, 196223,
    194301, 191987, 189686, 187636, 185559, 183426, 181453, 179444, 177638, 175855, 174054,
    171948, 170489, 168695, 166889, 165365, 163519, 162045, 160508, 158970, 157429, 156150,
    154610, 153081, 151803, 150511, 148986, 147709, 146420, 145116, 143868, 142586, 141545,
    140277, 139194, 137957, 136954, 135676, 134652, 133621, 132604, 131577, 130552, 129527,
    128508, 127476, 126451, 125432, 124670, 123645, 122818, 121847, 121082, 120060, 119288,
    118263, 117502, 116720, 115967, 115195, 114424, 113655, 112893, 112125, 111356, 110563,
    109811, 109048, 108287, 107766, 107004, 106236, 105724, 104953, 104434, 103676, 102904,
    102375, 101879, 101119, 100604, 99834, 99321, 98813, 98112, 97533, 97019, 96509, 95994,
    95486, 94713, 94185, 93689, 93179, 92667, 92149, 91643, 91129, 90621, 90068, 89597,
    89342, 88829, 88318, 87804, 87294, 87034, 86523, 85994, 85499, 85245, 84732, 84222,
    83956, 83450, 82937, 82685, 82173, 81840, 81405, 80889, 80638, 80127, 79862, 79354,
    79103, 78590, 78332, 78077, 77565, 77308, 76795, 76541, 76284, 75766, 75518, 75262,
    74748, 74493, 74238, 73691, 73470, 73214, 72959, 72447, 72189, 71935, 71671, 71166,
    70911, 70651, 70399, 70140, 69886, 69615, 69116, 68861, 68603, 68350, 68093, 67839,
    67576, 67326, 67070, 66813, 66556, 66302, 66046, 65791, 65408
};

u32 bmd_unmul(u32 c) {
    u32 f = premul_factor[A_(c)];
    u32 r = (u8)((R_(c) * f + 0x8000u) >> 16), g = (u8)((G_(c) * f + 0x8000u) >> 16), b = (u8)((B_(c) * f + 0x8000u) >> 16);
    return RGBA(r, g, b, A_(c));
}

u32 bmd_blend_over(u32 dst, u32 src) {
    u32 inv = 256u - A_(src);
    u32 r = (R_(src) + ((R_(dst) * inv) >> 8)) & 255u;
    u32 g = (G_(src) + ((G_(dst) * inv) >> 8)) & 255u;
    u32 b = (B_(src) + ((B_(dst) * inv) >> 8)) & 255u;
    u32 a = (A_(src) + ((A_(dst) * inv) >> 8)) & 255u;
    return RGBA(r, g, b, a);
}

/* swf ColorTransform * Color (on unmultiplied colours) */
static u32 cx_color(const SWFCxform *cx, u32 c) {
    s32 v[4], m[4], ad[4]; int i;
    if (A_(c) == 0) return c;
    v[0] = (s32)R_(c); v[1] = (s32)G_(c); v[2] = (s32)B_(c); v[3] = (s32)A_(c);
    m[0] = cx->mr; m[1] = cx->mg; m[2] = cx->mb; m[3] = cx->ma;
    ad[0] = cx->ar; ad[1] = cx->ag; ad[2] = cx->ab; ad[3] = cx->aa;
    for (i = 0; i < 4; i++) {
        s32 p = (s32)(s16)((m[i] * v[i]) >> 8), s = p + ad[i];
        if (s > 32767) s = 32767; if (s < -32768) s = -32768;
        if (s < 0) s = 0; if (s > 255) s = 255;
        v[i] = s;
    }
    return RGBA(v[0], v[1], v[2], v[3]);
}

static int cx_is_identity(const SWFCxform *cx) {
    return cx->mr == 256 && cx->mg == 256 && cx->mb == 256 && cx->ma == 256 && !cx->ar && !cx->ag && !cx->ab && !cx->aa;
}

/* ---------------------------------------------------------------- */
/* lifetime                                                         */

BitmapData *bmd_new_pixels(u32 w, u32 h, int transparent, u32 *px) {
    BitmapData *b = (BitmapData*)calloc(1, sizeof(BitmapData));
    if (!b) { free(px); return NULL; }
    b->w = w; b->h = h; b->transparent = transparent ? 1 : 0; b->px = px; b->refs = 1;
    return b;
}

BitmapData *bmd_new(u32 w, u32 h, int transparent, u32 fill) {
    size_t n = (size_t)w * h, i; u32 *px = NULL, c = bmd_premul(fill, transparent);
    if (n) { px = (u32*)malloc(n * 4); if (!px) return NULL; for (i = 0; i < n; i++) px[i] = c; }
    return bmd_new_pixels(w, h, transparent, px);
}

BitmapData *bmd_from_straight(const Bitmap *src, int transparent) {
    size_t n = (size_t)src->w * src->h, i; u32 *px = n ? (u32*)malloc(n * 4) : NULL;
    if (n && !px) return NULL;
    if (src->pm) memcpy(px, src->pm, n * 4);   /* a symbol's premultiplied data as stored */
    else for (i = 0; i < n; i++) px[i] = bmd_premul(src->px[i], 1);
    return bmd_new_pixels((u32)src->w, (u32)src->h, transparent, px);
}

/* symbol images are instantiated again and again (embedded asset classes): premultiply once */
static u32 *shbuf_alloc(size_t n); static void shbuf_ref(u32 *px); static void shbuf_unref(u32 *px);
#define PM_CACHE 1024
static struct { const Bitmap *src; u32 *pm; int w, h; } g_pm_cache[PM_CACHE];
static unsigned pm_hash(const Bitmap *b) { unsigned long v = (unsigned long)(size_t)b; return (unsigned)((v >> 4) ^ (v >> 14)) & (PM_CACHE - 1); }
BitmapData *bmd_from_symbol_image(const Bitmap *src, int transparent) {
    size_t n = (size_t)src->w * src->h; unsigned h = pm_hash(src); u32 *pm = NULL; BitmapData *b;
    if (n < 4096) return bmd_from_straight(src, transparent);
    if (g_pm_cache[h].src == src && g_pm_cache[h].w == src->w && g_pm_cache[h].h == src->h) pm = g_pm_cache[h].pm;
    if (!pm) {
        size_t k; pm = shbuf_alloc(n); if (!pm) return bmd_from_straight(src, transparent);
        if (src->pm) memcpy(pm, src->pm, n * 4);
        else for (k = 0; k < n; k++) pm[k] = bmd_premul(src->px[k], 1);
        shbuf_unref(g_pm_cache[h].pm); g_pm_cache[h].src = src; g_pm_cache[h].pm = pm; g_pm_cache[h].w = src->w; g_pm_cache[h].h = src->h;
    }
    b = (BitmapData*)calloc(1, sizeof(BitmapData));   /* the pixels stay shared until written (copy on write) */
    if (!b) return NULL;
    shbuf_ref(pm);
    b->w = (u32)src->w; b->h = (u32)src->h; b->transparent = transparent ? 1 : 0; b->px = pm; b->px_shared = 1; b->refs = 1;
    b->view = *src; b->view_ok = 1; b->view_shared = 1;
    return b;
}
/* the image cache dropped this decoded image */
void bmd_forget_symbol_image(const Bitmap *src) { unsigned h = pm_hash(src); if (g_pm_cache[h].src == src) { shbuf_unref(g_pm_cache[h].pm); g_pm_cache[h].pm = NULL; g_pm_cache[h].src = NULL; } }
BitmapData *bmd_clone(const BitmapData *s) {
    size_t n = (size_t)s->w * s->h; u32 *px = NULL; BitmapData *b;
    if (n && s->px) { px = (u32*)malloc(n * 4); if (!px) return NULL; memcpy(px, s->px, n * 4); }
    b = bmd_new_pixels(s->w, s->h, s->transparent, px);
    if (b) b->disposed = s->disposed;
    return b;
}

/* shared pixel buffers: two header words (reference count), pixels after */
static u32 *shbuf_alloc(size_t n) { u32 *raw = (u32*)malloc((n + 2) * 4); if (!raw) return NULL; raw[0] = 1; return raw + 2; }
static void shbuf_ref(u32 *px) { px[-2]++; }
static void shbuf_unref(u32 *px) { if (px && --px[-2] == 0) free(px - 2); }
static void px_release(BitmapData *b) { if (b->px_shared) shbuf_unref(b->px); else free(b->px); b->px = NULL; b->px_shared = 0; }
void bmd_own(BitmapData *b) {
    size_t n; u32 *np;
    if (!b || !b->px_shared) return;
    n = (size_t)b->w * b->h; np = (u32*)malloc(n ? n * 4 : 4);
    if (!np) return;
    memcpy(np, b->px, n * 4); shbuf_unref(b->px); b->px = np; b->px_shared = 0;
}
BitmapData *bmd_ref(BitmapData *b) { if (b) b->refs++; return b; }

void bmd_unref(BitmapData *b) {
    if (!b || --b->refs > 0) return;
    px_release(b); if (!b->view_shared) free(b->view.px); free(b);
}

/* bitmaps used as drawing fills (AVM1 beginBitmapFill): Ruffle's drawing holds
   the texture as it was synced when the fill was set; later pixel changes and
   dispose() do not reach it */
typedef struct { BitmapData *b; Bitmap keep; } FillReg;
static FillReg *g_freg; static int g_nfreg, g_capfreg;
static void fill_keep(FillReg *r) {
    const Bitmap *v = bmd_view(r->b); Bitmap *k = &r->keep; size_t n;
    free(k->px); free(k->pm_own); memset(k, 0, sizeof(*k));
    if (!v) return;
    n = (size_t)v->w * v->h;
    k->px = (u32*)malloc(n * 4); k->pm_own = (u32*)malloc(n * 4);
    if (!k->px || !k->pm_own) { free(k->px); free(k->pm_own); memset(k, 0, sizeof(*k)); return; }
    memcpy(k->px, v->px, n * 4); memcpy(k->pm_own, v->pm, n * 4); k->pm = k->pm_own; k->w = v->w; k->h = v->h;
}
int bmd_fill_register(BitmapData *b) {
    int i;
    if (!b) return 0;
    for (i = 0; i < g_nfreg; i++) if (g_freg[i].b == b) { fill_keep(&g_freg[i]); return BMD_FILL_ID0 - i; }
    if (g_nfreg == g_capfreg) {
        int nc = g_capfreg ? g_capfreg * 2 : 16; FillReg *n = (FillReg*)realloc(g_freg, (size_t)nc * sizeof(FillReg));
        if (!n) return 0;
        g_freg = n; g_capfreg = nc;
    }
    memset(&g_freg[g_nfreg], 0, sizeof(FillReg)); g_freg[g_nfreg].b = bmd_ref(b);
    fill_keep(&g_freg[g_nfreg]);
    return BMD_FILL_ID0 - g_nfreg++;
}
const Bitmap *bmd_fill_lookup(int id) {
    int i = BMD_FILL_ID0 - id;
    if (i < 0 || i >= g_nfreg) return NULL;
    return g_freg[i].keep.px ? &g_freg[i].keep : NULL;
}

void bmd_dispose(BitmapData *b) {
    px_release(b); b->w = b->h = 0; b->disposed = 1;
    if (!b->view_shared) free(b->view.px); b->view.px = NULL; b->view_shared = 0; b->view.w = b->view.h = 0; b->view_ok = 0;
    b->version++;
}

void bmd_changed(BitmapData *b) { b->view_ok = 0; b->version++; }

const Bitmap *bmd_view(BitmapData *b) {
    size_t n, i;
    if (!b || b->disposed || !b->w || !b->h || !b->px) return NULL;
    b->view.pm = b->px;   /* the exact premultiplied texels */
    if (b->view_ok) return &b->view;
    if (b->view_shared) { b->view.px = NULL; b->view.w = b->view.h = 0; b->view_shared = 0; }   /* changed: render from its own copy now */
    n = (size_t)b->w * b->h;
    if (!b->view.px || b->view.w != (int)b->w || b->view.h != (int)b->h) {
        free(b->view.px);
        b->view.px = (u32*)malloc(n * 4);
        if (!b->view.px) return NULL;
        b->view.w = (int)b->w; b->view.h = (int)b->h;
    }
    for (i = 0; i < n; i++) b->view.px[i] = bmd_unmul(b->px[i]);
    b->view_ok = 1;
    return &b->view;
}

int bmd_size_valid(int v, u32 w, u32 h) {
    if (w == 0 || h == 0) return 0;
    if (v <= 9) { if (w > 2880 || h > 2880) return 0; }
    else if (v <= 12) { if (w >= 0x2000 || h >= 0x2000 || (u64)w * h >= 0x1000000) return 0; }
    else { if (w > 0x6666666 || h > 0x6666666 || (u64)w * h >= 0x20000000) return 0; }
    return 1;
}

/* ---------------------------------------------------------------- */
/* regions (ruffle_render::bitmap::PixelRegion)                     */

PxRegion reg_whole(u32 w, u32 h) { PxRegion r; r.x0 = 0; r.y0 = 0; r.x1 = w; r.y1 = h; return r; }

static s32 sat_add(s32 a, s32 b) { s64 s = (s64)a + b; if (s > 2147483647) return 2147483647; if (s < -2147483647 - 1) return (s32)0x80000000; return (s32)s; }

PxRegion reg_i32(s32 x, s32 y, s32 w, s32 h) {
    PxRegion r; s32 bx = sat_add(x, w), by = sat_add(y, h);
    s32 x0 = x < bx ? x : bx, y0 = y < by ? y : by, x1 = x < bx ? bx : x, y1 = y < by ? by : y;
    r.x0 = x0 > 0 ? (u32)x0 : 0; r.y0 = y0 > 0 ? (u32)y0 : 0; r.x1 = x1 > 0 ? (u32)x1 : 0; r.y1 = y1 > 0 ? (u32)y1 : 0;
    return r;
}

static PxRegion reg_encompassing(u32 ax, u32 ay, u32 bx, u32 by) {
    PxRegion r; u32 x1 = ax > bx ? ax : bx, y1 = ay > by ? ay : by;
    r.x0 = ax < bx ? ax : bx; r.y0 = ay < by ? ay : by;
    r.x1 = x1 == 0xFFFFFFFFu ? x1 : x1 + 1; r.y1 = y1 == 0xFFFFFFFFu ? y1 : y1 + 1;
    return r;
}


void reg_clamp(PxRegion *r, u32 w, u32 h) {
    if (r->x0 > w) r->x0 = w; if (r->y0 > h) r->y0 = h;
    if (r->x1 > w) r->x1 = w; if (r->y1 > h) r->y1 = h;
}

#define RW(r) ((r).x1 - (r).x0)
#define RH(r) ((r).y1 - (r).y0)

static void isect(const s32 *a, const s32 *b, s32 *o) {
    s32 ax0 = a[0] < a[2] ? a[0] : a[2], ay0 = a[1] < a[3] ? a[1] : a[3];
    s32 bx0 = b[0] < b[2] ? b[0] : b[2], by0 = b[1] < b[3] ? b[1] : b[3];
    s32 x0 = ax0 > bx0 ? ax0 : bx0, y0 = ay0 > by0 ? ay0 : by0;
    s32 x1 = a[2] < b[2] ? a[2] : b[2], y1 = a[3] < b[3] ? a[3] : b[3];
    if (x0 > x1) x0 = x1; if (y0 > y1) y0 = y1;
    o[0] = x0; o[1] = y0; o[2] = x1; o[3] = y1;
}

static u32 to_u32(s32 v) { return v < 0 ? 0 : (u32)v; }

void reg_clamp_with_intersection(PxRegion *self, s32 sx, s32 sy, s32 ox, s32 oy, s32 w, s32 h, PxRegion *other) {
    s32 r1[4], r2[4], sz[4], t[4], in[4]; int empty;
    r1[0] = (s32)self->x0 - sx; r1[1] = (s32)self->y0 - sy; r1[2] = (s32)self->x1 - sx; r1[3] = (s32)self->y1 - sy;
    r2[0] = (s32)other->x0 - ox; r2[1] = (s32)other->y0 - oy; r2[2] = (s32)other->x1 - ox; r2[3] = (s32)other->y1 - oy;
    sz[0] = 0; sz[1] = 0; sz[2] = w; sz[3] = h;
    isect(r1, r2, t); isect(t, sz, in);
    empty = in[0] == in[2] || in[1] == in[3];
    if (empty) { self->x0 = self->y0 = self->x1 = self->y1 = 0; *other = *self; return; }
    self->x0 = to_u32(in[0] + sx); self->y0 = to_u32(in[1] + sy); self->x1 = to_u32(in[2] + sx); self->y1 = to_u32(in[3] + sy);
    other->x0 = to_u32(in[0] + ox); other->y0 = to_u32(in[1] + oy); other->x1 = to_u32(in[2] + ox); other->y1 = to_u32(in[3] + oy);
}

/* the usual source/destination clamp of the copy-like operations */
static int src_dst(BitmapData *t, BitmapData *src, s32 sx, s32 sy, s32 sw, s32 sh, s32 dx, s32 dy, PxRegion *sr, PxRegion *dr) {
    *sr = reg_whole(src->w, src->h);
    *dr = reg_whole(t->w, t->h);
    reg_clamp_with_intersection(dr, dx, dy, sx, sy, sw, sh, sr);
    return RW(*dr) != 0 && RH(*dr) != 0;
}

/* ---------------------------------------------------------------- */
/* LehmerRng                                                        */

typedef struct { u32 x; } Lehmer;
static u32 lehmer_next(Lehmer *r) { r->x = (u32)(((u64)r->x * 16807u) % 2147483647u); return r->x; }
static u8 lehmer_range(Lehmer *r, u8 lo, u8 hi) { return (u8)(lo + (u8)(lehmer_next(r) % ((u32)(u8)(hi - lo) + 1))); }

/* ---------------------------------------------------------------- */
/* simple pixel operations                                          */

void bmd_fill_rect(BitmapData *t, s32 x, s32 y, s32 w, s32 h, u32 color) {
    PxRegion r = reg_i32(x, y, w, h); u32 c, yy, xx;
    reg_clamp(&r, t->w, t->h);
    if (!RW(r) || !RH(r)) return;
    c = bmd_premul(color, t->transparent);
    for (yy = r.y0; yy < r.y1; yy++) for (xx = r.x0; xx < r.x1; xx++) PX(t, xx, yy) = c;
    bmd_changed(t);
}

void bmd_set_pixel32(BitmapData *t, u32 x, u32 y, u32 color) {
    if (x >= t->w || y >= t->h) return;
    PX(t, x, y) = bmd_premul(color, t->transparent);
    bmd_changed(t);
}

u32 bmd_get_pixel32(BitmapData *t, u32 x, u32 y) {
    if (x >= t->w || y >= t->h) return 0;
    return t->transparent ? bmd_unmul(PX(t, x, y)) : PX(t, x, y);
}

void bmd_set_pixel(BitmapData *t, u32 x, u32 y, u32 color) {
    if (x >= t->w || y >= t->h) return;
    if (t->transparent) PX(t, x, y) = bmd_premul(WITH_ALPHA(color, A_(PX(t, x, y))), 1);
    else PX(t, x, y) = WITH_ALPHA(color, 255);
    bmd_changed(t);
}

u32 bmd_get_pixel(BitmapData *t, u32 x, u32 y) {
    if (x >= t->w || y >= t->h) return 0;
    return WITH_ALPHA(bmd_unmul(PX(t, x, y)), 0);
}

int bmd_flood_fill(BitmapData *t, u32 x, u32 y, u32 color) {
    u32 expected, repl, *stk; size_t n = 0, cap = 256;
    if (x >= t->w || y >= t->h) return 0;
    expected = PX(t, x, y);
    repl = bmd_premul(color, t->transparent);
    if (expected == repl) return 0;
    stk = (u32*)malloc(cap * 2 * sizeof(u32));
    if (!stk) return 0;
    stk[n * 2] = x; stk[n * 2 + 1] = y; n++;
    while (n) {
        u32 px, py;
        n--; px = stk[n * 2]; py = stk[n * 2 + 1];
        if (PX(t, px, py) != expected) continue;
        if (n + 4 > cap) { u32 *ns = (u32*)realloc(stk, cap * 4 * sizeof(u32)); if (!ns) break; stk = ns; cap *= 2; }
        if (px > 0) { stk[n * 2] = px - 1; stk[n * 2 + 1] = py; n++; }
        if (py > 0) { stk[n * 2] = px; stk[n * 2 + 1] = py - 1; n++; }
        if (px < t->w - 1) { stk[n * 2] = px + 1; stk[n * 2 + 1] = py; n++; }
        if (py < t->h - 1) { stk[n * 2] = px; stk[n * 2 + 1] = py + 1; n++; }
        PX(t, px, py) = repl;
    }
    free(stk);
    bmd_changed(t);
    return 1;
}

void bmd_noise(BitmapData *t, s32 seed, u8 low, u8 high, int ch, int gray) {
    Lehmer rng; u32 x, y;
    rng.x = seed <= 0 ? (u32)(-(s64)seed + 1) : (u32)seed;
    for (y = 0; y < t->h; y++) for (x = 0; x < t->w; x++) {
        u32 c;
        if (gray) {
            u8 g = lehmer_range(&rng, low, high), a = 255;
            if (t->transparent && (ch & BMD_ALPHA)) a = lehmer_range(&rng, low, high);
            c = RGBA(g, g, g, a);
        } else {
            u8 r = 0, g = 0, b = 0, a = 255;
            if (ch & BMD_RED) r = lehmer_range(&rng, low, high);
            if (ch & BMD_GREEN) g = lehmer_range(&rng, low, high);
            if (ch & BMD_BLUE) b = lehmer_range(&rng, low, high);
            if (t->transparent && (ch & BMD_ALPHA)) a = lehmer_range(&rng, low, high);
            c = RGBA(r, g, b, a);
        }
        PX(t, x, y) = bmd_premul(c, t->transparent);
    }
    bmd_changed(t);
}

/* ---------------------------------------------------------------- */
/* Perlin turbulence (the SVG feTurbulence reference code)          */

#define RAND_M 2147483647LL
#define RAND_A 16807LL
#define RAND_Q 127773LL
#define RAND_R 2836LL
#define TB 0x100
#define TBM 0xff
#define PERLIN_N 0x1000

typedef struct {
    s32 lattice[TB + TB + 2];
    double grad[4][TB + TB + 2][2];
} Turb;

typedef struct { s32 width, height, wrap_x, wrap_y; } Stitch;

static s64 turb_setup_seed(s64 seed) {
    if (seed <= 0) seed = -(seed % (RAND_M - 1)) + 1;
    if (seed > RAND_M - 1) seed = RAND_M - 1;
    return seed;
}
static s64 turb_random(s64 seed) {
    s64 r = RAND_A * (seed % RAND_Q) - RAND_R * (seed / RAND_Q);
    if (r <= 0) r += RAND_M;
    return r;
}

static void turb_init(Turb *t, s64 seed) {
    int i, j, k; double s;
    seed = turb_setup_seed(seed);
    for (k = 0; k < 4; k++) {
        for (i = 0; i < TB; i++) {
            t->lattice[i] = i;
            for (j = 0; j < 2; j++) {
                seed = turb_random(seed);
                t->grad[k][i][j] = (double)((seed % (TB + TB)) - TB) / TB;
            }
            s = sqrt(t->grad[k][i][0] * t->grad[k][i][0] + t->grad[k][i][1] * t->grad[k][i][1]);
            t->grad[k][i][0] /= s; t->grad[k][i][1] /= s;
        }
    }
    for (i = TB - 1; i >= 1; i--) {
        s32 kk = t->lattice[i];
        seed = turb_random(seed);
        j = (int)(seed % TB);
        t->lattice[i] = t->lattice[j]; t->lattice[j] = kk;
    }
    for (i = 0; i < TB + 2; i++) {
        t->lattice[TB + i] = t->lattice[i];
        for (k = 0; k < 4; k++) for (j = 0; j < 2; j++) t->grad[k][TB + i][j] = t->grad[k][i][j];
    }
}

static double s_curve(double t) { return t * t * (3. - 2. * t); }
static double lerp(double t, double a, double b) { return a + t * (b - a); }

static double turb_noise2(const Turb *tb, int ch, double vx, double vy, const Stitch *st) {
    double t, rx0, rx1, ry0, ry1, sx, sy, a, b, u, v; const double *q;
    s32 bx0, bx1, by0, by1, i, j, b00, b10, b01, b11;
    t = vx + PERLIN_N; bx0 = f_i32(t); bx1 = (s32)((u32)bx0 + 1u); rx0 = t - (double)f_i32(t); rx1 = rx0 - 1.0;
    t = vy + PERLIN_N; by0 = f_i32(t); by1 = (s32)((u32)by0 + 1u); ry0 = t - (double)f_i32(t); ry1 = ry0 - 1.0;
    if (st) {
        if (bx0 >= st->wrap_x) bx0 = (s32)((u32)bx0 - (u32)st->width);
        if (bx1 >= st->wrap_x) bx1 = (s32)((u32)bx1 - (u32)st->width);
        if (by0 >= st->wrap_y) by0 = (s32)((u32)by0 - (u32)st->height);
        if (by1 >= st->wrap_y) by1 = (s32)((u32)by1 - (u32)st->height);
    }
    bx0 &= TBM; bx1 &= TBM; by0 &= TBM; by1 &= TBM;
    i = tb->lattice[bx0]; j = tb->lattice[bx1];
    b00 = tb->lattice[i + by0]; b10 = tb->lattice[j + by0];
    b01 = tb->lattice[i + by1]; b11 = tb->lattice[j + by1];
    sx = s_curve(rx0); sy = s_curve(ry0);
    q = tb->grad[ch][b00]; u = rx0 * q[0] + ry0 * q[1];
    q = tb->grad[ch][b10]; v = rx1 * q[0] + ry0 * q[1];
    a = lerp(sx, u, v);
    q = tb->grad[ch][b01]; u = rx0 * q[0] + ry1 * q[1];
    q = tb->grad[ch][b11]; v = rx1 * q[0] + ry1 * q[1];
    b = lerp(sx, u, v);
    return lerp(sy, a, b);
}

static double turbulence(const Turb *tb, int ch, double px, double py, double fx, double fy, int octaves, int fractal, int stitch,
                         double tile_w, double tile_h, const double *offs) {
    Stitch st, *sp = NULL; double sum = 0.0, ratio = 1.0; int o;
    if (stitch) {
        s32 w, h;
        if (fx != 0.0) {
            double lo = floor(tile_w * fx) / tile_w, hi = ceil(tile_w * fx) / tile_w;
            fx = (fx / lo < hi / fx) ? lo : hi;
        }
        if (fy != 0.0) {
            /* (Ruffle reproduces the reference code's use of the x frequency here) */
            double lo = floor(tile_h * fx) / tile_h, hi = ceil(tile_h * fy) / tile_h;
            fy = (fy / lo < hi / fy) ? lo : hi;
        }
        w = f_i32(tile_w * fx + 0.5); h = f_i32(tile_h * fy + 0.5);
        st.width = w; st.height = h;
        st.wrap_x = (s32)((u32)f_i32(0.0 * fx) + PERLIN_N + (u32)w);
        st.wrap_y = (s32)((u32)f_i32(0.0 * fy) + PERLIN_N + (u32)h);
        sp = &st;
    }
    for (o = 0; o < octaves; o++) {
        double vx = (px + offs[o * 2]) * fx * ratio, vy = (py + offs[o * 2 + 1]) * fy * ratio;
        double n = turb_noise2(tb, ch, vx, vy, sp);
        sum += (fractal ? n : fabs(n)) / ratio;
        ratio *= 2.0;
        if (sp) {
            st.width = (s32)((u32)st.width * 2u);
            st.wrap_x = (s32)(2u * (u32)st.wrap_x - PERLIN_N);
            st.height = (s32)((u32)st.height * 2u);
            st.wrap_y = (s32)(2u * (u32)st.wrap_y - PERLIN_N);
        }
    }
    return sum;
}

void bmd_perlin_noise(BitmapData *t, double bx, double by, int octaves, s32 seed, int stitch, int fractal, int chans, int gray, const double *offs) {
    Turb *tb; double fx = bx == 0.0 ? 0.0 : 1.0 / bx, fy = by == 0.0 ? 0.0 : 1.0 / by; u32 x, y;
    double tw = (double)t->w, th = (double)t->h;
    tb = (Turb*)malloc(sizeof(Turb));
    if (!tb) return;
    turb_init(tb, (s64)seed);
    for (y = 0; y < t->h; y++) for (x = 0; x < t->w; x++) {
        double px = (double)x, py = (double)y, n[4]; u8 c[4]; int k;
        if (gray) {
            n[0] = turbulence(tb, 0, px, py, fx, fy, octaves, fractal, stitch, tw, th, offs);
            n[1] = n[2] = n[0];
            n[3] = (chans & BMD_ALPHA) ? turbulence(tb, 1, px, py, fx, fy, octaves, fractal, stitch, tw, th, offs) : 1.0;
        } else {
            int channel = 0;
            for (k = 0; k < 4; k++) {
                n[k] = k == 3 ? 1.0 : -1.0;
                if (chans & (1 << k)) {
                    n[k] = turbulence(tb, channel, px, py, fx, fy, octaves, fractal, stitch, tw, th, offs);
                    channel++;
                }
            }
        }
        for (k = 0; k < 4; k++) c[k] = f_u8(fractal ? ((n[k] * 255.0 + 255.0) + 0.5) / 2.0 : (n[k] * 255.0) + 0.5);
        if (!t->transparent) c[3] = 255;
        PX(t, x, y) = RGBA(c[0], c[1], c[2], c[3]);
    }
    free(tb);
    bmd_changed(t);
}

/* ---------------------------------------------------------------- */
/* channel / colour operations                                      */

void bmd_copy_channel(BitmapData *t, s32 dx, s32 dy, s32 sx, s32 sy, s32 sw, s32 sh, BitmapData *src, s32 sc, s32 dc) {
    PxRegion sr, dr; int shift = sc == 1 ? 16 : sc == 2 ? 8 : sc == 4 ? 0 : sc == 8 ? 24 : -1; u32 x, y, w, h;
    if (!src_dst(t, src, sx, sy, sw, sh, dx, dy, &sr, &dr)) return;
    h = RH(dr) < RH(sr) ? RH(dr) : RH(sr); w = RW(dr) < RW(sr) ? RW(dr) : RW(sr);
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
        u32 ox = dr.x0 + x, oy = dr.y0 + y;
        u32 orig = bmd_unmul(PX(t, ox, oy)), sc_ = bmd_unmul(PX(src, sr.x0 + x, sr.y0 + y));
        u32 part = shift >= 0 ? (sc_ >> shift) & 255u : 0, res;
        switch (dc) {
        case 1: res = (orig & 0xFF00FFFFu) | (part << 16); break;
        case 2: res = (orig & 0xFFFF00FFu) | (part << 8); break;
        case 4: res = (orig & 0xFFFFFF00u) | part; break;
        case 8: res = (orig & 0x00FFFFFFu) | (part << 24); break;
        default: res = orig; break;
        }
        PX(t, ox, oy) = bmd_premul(res, t->transparent);
    }
    bmd_changed(t);
}

void bmd_color_transform(BitmapData *t, u32 x0, u32 y0, u32 x1, u32 y1, const SWFCxform *cx) {
    u32 x, y;
    if (cx->mr == 256 && cx->mg == 256 && cx->mb == 256 && cx->ma >= 256 && !cx->ar && !cx->ag && !cx->ab && !cx->aa) return;
    if (x0 > t->w) x0 = t->w; if (y0 > t->h) y0 = t->h;
    if (x1 > t->w) x1 = t->w; if (y1 > t->h) y1 = t->h;
    if (x1 == 0 || y1 == 0 || x0 == x1 || y0 == y1) return;
    for (y = y0; y < y1; y++) for (x = x0; x < x1; x++)
        PX(t, x, y) = bmd_premul(cx_color(cx, bmd_unmul(PX(t, x, y))), t->transparent);
    bmd_changed(t);
}

int bmd_threshold_op(const char *s) {
    if (!strcmp(s, "==")) return TH_EQ;
    if (!strcmp(s, "!=")) return TH_NE;
    if (!strcmp(s, "<")) return TH_LT;
    if (!strcmp(s, "<=")) return TH_LE;
    if (!strcmp(s, ">")) return TH_GT;
    if (!strcmp(s, ">=")) return TH_GE;
    return -1;
}

static int th_match(int op, u32 v, u32 t) {
    switch (op) {
    case TH_EQ: return v == t; case TH_NE: return v != t;
    case TH_LT: return v < t; case TH_LE: return v <= t;
    case TH_GT: return v > t; default: return v >= t;
    }
}

u32 bmd_threshold(BitmapData *t, BitmapData *src, s32 sx, s32 sy, s32 sw, s32 sh, s32 dx, s32 dy, int op, u32 threshold, u32 colour, u32 mask, int copy_source) {
    PxRegion sr, dr; u32 masked = threshold & mask, count = 0, x, y, col = bmd_premul(colour, 1);
    if (!src_dst(t, src, sx, sy, sw, sh, dx, dy, &sr, &dr)) return 0;
    for (y = 0; y < RH(dr); y++) for (x = 0; x < RW(dr); x++) {
        u32 s = PX(src, sr.x0 + x, sr.y0 + y);
        if (th_match(op, s & mask, masked)) { count++; PX(t, dr.x0 + x, dr.y0 + y) = col; }
        else if (copy_source) PX(t, dr.x0 + x, dr.y0 + y) = s;
    }
    bmd_changed(t);
    return count;
}

void bmd_scroll(BitmapData *t, s32 x, s32 y) {
    s32 w = (s32)t->w, h = (s32)t->h, rev_y = y > 0, rev_x = y == 0 && x > 0;
    s32 yf, yt, ddy, xf, xt, ddx, sy, sx;
    if ((x == 0 && y == 0) || (x < 0 ? -x : x) >= w || (y < 0 ? -y : y) >= h) return;
    yf = rev_y ? h - y - 1 : -y; yt = rev_y ? -1 : h; ddy = rev_y ? -1 : 1;
    xf = rev_x ? w - x - 1 : (-x > 0 ? -x : 0); xt = rev_x ? -1 : (w < w - x ? w : w - x); ddx = rev_x ? -1 : 1;
    for (sy = yf; sy != yt; sy += ddy)
        for (sx = xf; sx != xt; sx += ddx)
            PX(t, (u32)(sx + x), (u32)(sy + y)) = PX(t, (u32)sx, (u32)sy);
    bmd_changed(t);
}

void bmd_palette_map(BitmapData *t, BitmapData *src, s32 sx, s32 sy, s32 sw, s32 sh, s32 dx, s32 dy, const u32 *ch) {
    PxRegion sr, dr; u32 x, y;
    if (!src_dst(t, src, sx, sy, sw, sh, dx, dy, &sr, &dr)) return;
    for (y = 0; y < RH(dr); y++) for (x = 0; x < RW(dr); x++) {
        u32 s = bmd_unmul(PX(src, sr.x0 + x, sr.y0 + y));
        u32 sum = ch[R_(s)] + ch[256 + G_(s)] + ch[512 + B_(s)] + ch[768 + A_(s)];
        PX(t, dr.x0 + x, dr.y0 + y) = bmd_premul(sum, 1);
    }
    bmd_changed(t);
}

BitmapData *bmd_compare(BitmapData *l, BitmapData *r) {
    size_t n = (size_t)l->w * l->h, i; int diff = 0; u32 *px = n ? (u32*)malloc(n * 4) : NULL;
    if (n && !px) return NULL;
    for (i = 0; i < n; i++) {
        u32 a = bmd_unmul(l->px[i]), b = bmd_unmul(r->px[i]);
        if (a == b) px[i] = 0;
        else if ((a & 0xFFFFFFu) != (b & 0xFFFFFFu)) { diff = 1; px[i] = RGBA(R_(a) - R_(b), G_(a) - G_(b), B_(a) - B_(b), 255); }
        else { u32 al = (A_(a) - A_(b)) & 255u; diff = 1; px[i] = RGBA(al, al, al, al); }
    }
    if (!diff) { free(px); return NULL; }
    return bmd_new_pixels(l->w, l->h, 1, px);
}

int bmd_hit_test_point(BitmapData *t, u8 th, s32 x, s32 y) {
    if (x < 0 || y < 0 || x >= (s32)t->w || y >= (s32)t->h) return 0;
    return A_(PX(t, (u32)x, (u32)y)) >= (u32)(th ? th : 1);
}

int bmd_hit_test_rect(BitmapData *t, u8 th, s32 x, s32 y, s32 w, s32 h) {
    PxRegion r = reg_i32(x, y, w, h); u32 xx, yy;
    reg_clamp(&r, t->w, t->h);
    for (yy = r.y0; yy < r.y1; yy++) for (xx = r.x0; xx < r.x1; xx++) if (A_(PX(t, xx, yy)) >= th) return 1;
    return 0;
}

int bmd_hit_test_bitmap(BitmapData *t, s32 spx, s32 spy, u8 sth, BitmapData *o, s32 tpx, s32 tpy, u8 tth) {
    s32 xd = tpx - spx, yd = tpy - spy, sw = (s32)t->w, sh = (s32)t->h, tw = (s32)o->w, th = (s32)o->h, v;
    u32 sx0, tx0, w, sy0, ty0, h, x, y;
    if (xd < 0) { sx0 = 0; tx0 = (u32)(-xd); v = tw + xd; v = sw < v ? sw : v; w = v > 0 ? (u32)v : 0; }
    else { sx0 = (u32)xd; tx0 = 0; v = sw - xd; v = tw < v ? tw : v; w = v > 0 ? (u32)v : 0; }
    if (yd < 0) { sy0 = 0; ty0 = (u32)(-yd); v = th + yd; v = sh < v ? sh : v; h = v > 0 ? (u32)v : 0; }
    else { sy0 = (u32)yd; ty0 = 0; v = sh - yd; v = th < v ? th : v; h = v > 0 ? (u32)v : 0; }
    for (x = 0; x < w; x++) for (y = 0; y < h; y++)
        if (A_(PX(t, sx0 + x, sy0 + y)) >= sth && A_(PX(o, tx0 + x, ty0 + y)) >= tth) return 1;
    return 0;
}

void bmd_color_bounds(BitmapData *t, int find, u32 mask, u32 color, u32 *out) {
    u32 minx = t->w, maxx = 0, miny = t->h, maxy = 0, x, y;
    if (!t->transparent) mask |= 0xFF000000u;
    color = bmd_premul(color, t->transparent);
    for (x = 0; x < t->w; x++) for (y = 0; y < t->h; y++) {
        u32 p = PX(t, x, y); int m = find ? (p & mask) == color : (p & mask) != color;
        if (m) { if (x < minx) minx = x; if (x > maxx) maxx = x; if (y < miny) miny = y; if (y > maxy) maxy = y; }
    }
    if (maxx > 0 || maxy > 0) { out[0] = minx; out[1] = miny; out[2] = maxx - minx + 1; out[3] = maxy - miny + 1; }
    else out[0] = out[1] = out[2] = out[3] = 0;
}

static u32 clamp256(s32 v) { return v < 0 ? 0u : v > 256 ? 256u : (u32)v; }

void bmd_merge(BitmapData *t, BitmapData *src, s32 sx, s32 sy, s32 sw, s32 sh, s32 dx, s32 dy, s32 rm, s32 gm, s32 bm, s32 am) {
    PxRegion sr, dr; u32 x, y, mr = clamp256(rm), mg = clamp256(gm), mb = clamp256(bm), ma = clamp256(am);
    if (!src_dst(t, src, sx, sy, sw, sh, dx, dy, &sr, &dr)) return;
    for (y = 0; y < RH(dr); y++) for (x = 0; x < RW(dr); x++) {
        u32 s = bmd_unmul(PX(src, sr.x0 + x, sr.y0 + y)), d = bmd_unmul(PX(t, dr.x0 + x, dr.y0 + y));
        u32 r = ((R_(s) * mr + R_(d) * (256 - mr)) & 0xFFFFu) / 256;
        u32 g = ((G_(s) * mg + G_(d) * (256 - mg)) & 0xFFFFu) / 256;
        u32 b = ((B_(s) * mb + B_(d) * (256 - mb)) & 0xFFFFu) / 256;
        u32 a = ((A_(s) * ma + A_(d) * (256 - ma)) & 0xFFFFu) / 256;
        PX(t, dr.x0 + x, dr.y0 + y) = bmd_premul(RGBA(r, g, b, a), t->transparent);
    }
    bmd_changed(t);
}

/* ---------------------------------------------------------------- */
/* copies                                                           */

void bmd_copy_region(BitmapData *src, BitmapData *dst, PxRegion sr, PxRegion dr, int blend) {
    u32 x, y, w = RW(dr), h = RH(dr);
    if (!src->transparent) blend = 0;
    if (src == dst) {
        s32 ddx = (s32)dr.x0 - (s32)sr.x0, ddy = (s32)dr.y0 - (s32)sr.y0; int rev = ddy >= 0 && ddx >= 0;
        if (!blend && sr.x0 == dr.x0 && sr.y0 == dr.y0 && sr.x1 == dr.x1 && sr.y1 == dr.y1) return;
        if (blend) {
            u32 i, j;
            for (j = 0; j < h; j++) for (i = 0; i < w; i++) {
                u32 xx = rev ? w - 1 - i : i, yy = rev ? h - 1 - j : j;
                PX(dst, dr.x0 + xx, dr.y0 + yy) = bmd_blend_over(PX(dst, dr.x0 + xx, dr.y0 + yy), PX(dst, sr.x0 + xx, sr.y0 + yy));
            }
        } else {
            u32 j;
            for (j = 0; j < h; j++) {
                u32 yy = rev ? h - 1 - j : j;
                memmove(&PX(dst, dr.x0, dr.y0 + yy), &PX(dst, sr.x0, sr.y0 + yy), (size_t)w * 4);
            }
        }
        bmd_changed(dst);
        return;
    }
    if (!blend && (dst->transparent || !src->transparent)) {
        for (y = 0; y < h; y++) memcpy(&PX(dst, dr.x0, dr.y0 + y), &PX(src, sr.x0, sr.y0 + y), (size_t)w * 4);
    } else {
        int opaque = !dst->transparent;
        for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
            u32 *d = &PX(dst, dr.x0 + x, dr.y0 + y), s = PX(src, sr.x0 + x, sr.y0 + y);
            if (blend) { u32 c = bmd_blend_over(*d, s); if (opaque) c = WITH_ALPHA(c, 255); *d = c; }
            else *d = WITH_ALPHA(bmd_unmul(s), 255);
        }
    }
    bmd_changed(dst);
}

void bmd_copy_pixels(BitmapData *t, BitmapData *src, s32 sx, s32 sy, s32 sw, s32 sh, s32 dx, s32 dy, int merge_alpha) {
    PxRegion sr, dr;
    if (!src_dst(t, src, sx, sy, sw, sh, dx, dy, &sr, &dr)) return;
    bmd_copy_region(src, t, sr, dr, (src->transparent && !t->transparent) || merge_alpha);
}

static int in_bounds(const BitmapData *b, s32 x, s32 y) { return x >= 0 && x < (s32)b->w && y >= 0 && y < (s32)b->h; }

static u8 round_u8(double d) { return f_u8(d < 0 ? ceil(d - 0.5) : floor(d + 0.5)); }

void bmd_copy_pixels_alpha(BitmapData *t, BitmapData *src, s32 sx, s32 sy, s32 sw, s32 sh, s32 dx, s32 dy, BitmapData *al, s32 ax, s32 ay, int merge_alpha) {
    PxRegion dreg; s32 x, y; int tt = t->transparent, st = src->transparent, at = al->transparent;
    if (src == al && ax == sx && ay == sy) { bmd_copy_pixels(t, src, sx, sy, sw, sh, dx, dy, merge_alpha); return; }
    dreg = reg_encompassing(to_u32(dx), to_u32(dy), to_u32(dx + sw), to_u32(dy + sh));
    reg_clamp(&dreg, t->w, t->h);
    if (!RW(dreg) || !RH(dreg)) return;
    for (y = sy; y < sy + sh; y++) for (x = sx; x < sx + sw; x++) {
        s32 tx = x - sx + dx, ty = y - sy + dy, axx = x - sx + ax, ayy = y - sy + ay;
        u32 s, d, fa, inter; double a; u8 r, g, b;
        if (!in_bounds(t, tx, ty) || !in_bounds(src, x, y)) continue;
        s = PX(src, (u32)x, (u32)y);
        d = PX(t, (u32)tx, (u32)ty);
        if (at) {
            u32 aa;
            if (!in_bounds(al, axx, ayy)) continue;
            aa = A_(PX(al, (u32)axx, (u32)ayy));
            if (st) fa = aa == 255 ? A_(s) : ((aa * A_(s)) >> 8) & 255u;
            else fa = aa;
        } else fa = st ? A_(s) : 255;
        a = A_(s) / 255.0;
        r = round_u8(R_(s) / a); g = round_u8(G_(s) / a); b = round_u8(B_(s) / a);
        if (a == 0.0) { r = R_(s) ? 255 : 0; g = G_(s) ? 255 : 0; b = B_(s) ? 255 : 0; }
        inter = bmd_premul(RGBA(r, g, b, fa), 1);
        d = (merge_alpha || !tt) ? bmd_blend_over(d, inter) : inter;
        PX(t, (u32)tx, (u32)ty) = d;
    }
    bmd_changed(t);
}

/* ---------------------------------------------------------------- */
/* pixelDissolve (Feistel permutation)                              */

static u32 feistel_block(u32 len) {
    u32 bits = 0, n;
    if (len < 2) len = 2;
    n = len - 1;
    while (n > 0) { n /= 2; bits++; }
    return bits + (bits % 2);
}

static u32 feistel_index(u32 raw, u32 block) {
    u32 half = block / 2, h1 = raw >> half, h2 = raw & ((1u << half) - 1), f = (h2 * h2 + 1) % (1u << half);
    return ((h1 ^ f) << half) | h2;
}

static void dissolve_px(BitmapData *t, BitmapData *src, u32 fill, u32 bx, u32 by, u32 rx, u32 ry, u32 wx, u32 wy) {
    if (!src) PX(t, wx + bx, wy + by) = bmd_premul(fill, t->transparent);
    else PX(t, wx + bx, wy + by) = PX(src, rx + bx, ry + by);
}

s32 bmd_pixel_dissolve(BitmapData *t, BitmapData *src, s32 sx, s32 sy, s32 sw, s32 sh, s32 dx, s32 dy, s32 seed, s32 num, u32 fill) {
    int empty; return bmd_pixel_dissolve_ex(t, src, sx, sy, sw, sh, dx, dy, seed, num, fill, &empty);
}
/* *empty: the clipped area had no pixels (nothing written, 0 returned) */
s32 bmd_pixel_dissolve_ex(BitmapData *t, BitmapData *src, s32 sx, s32 sy, s32 sw, s32 sh, s32 dx, s32 dy, s32 seed, s32 num, u32 fill, int *empty) {
    PxRegion sr, dr; u32 len, block, perm_len, raw; BitmapData *other; s32 i;
    *empty = 1;
    if (sw < 0) sw = 0; if (sh < 0) sh = 0;
    if (!sw || !sh) return 0;
    if (!src_dst(t, src, sx, sy, sw, sh, dx, dy, &sr, &dr)) return 0;
    *empty = 0;
    other = src == t ? NULL : src;
    len = RW(dr) * RH(dr);
    if (num > (s32)len) num = (s32)len;
    dissolve_px(t, other, fill, 0, 0, sr.x0, sr.y0, dr.x0, dr.y0);
    block = feistel_block(len);
    perm_len = 1u << block;
    raw = (u32)(seed % (s32)perm_len);
    for (i = 0; i < num; i++) {
        u32 fi = 0, loops = 0;
        while ((fi == 0 || fi >= len) && len != 1) {
            raw = (raw + 1) % perm_len;
            fi = feistel_index(raw, block);
            if (++loops > perm_len + 2) break;
        }
        dissolve_px(t, other, fill, fi % RW(dr), fi / RW(dr), sr.x0, sr.y0, dr.x0, dr.y0);
    }
    bmd_changed(t);
    return (s32)raw;
}

/* ---------------------------------------------------------------- */
/* draw                                                             */

/* layer covers exactly `area` (stride = its width) */
void bmd_draw_layer_area(BitmapData *t, const u32 *layer, PxRegion area) {
    u32 x, y, aw = area.x1 - area.x0; int opaque = !t->transparent;
    for (y = area.y0; y < area.y1; y++) for (x = area.x0; x < area.x1; x++) {
        u32 s = layer[(size_t)(y - area.y0) * aw + (x - area.x0)], c;
        if (!A_(s) && !(s & 0xFFFFFFu)) continue;
        c = bmd_blend_over(PX(t, x, y), s);
        if (opaque) c = WITH_ALPHA(c, 255);
        PX(t, x, y) = c;
    }
    bmd_changed(t);
}
/* one premultiplied channel of a blend mode (Flash's BlendMode on premultiplied pixels) */
static int blend_ch(int mode, int s, int d, int sa, int da) {
    int r, ns = s * (255 - da), nd = d * (255 - sa);
    switch (mode) {
    case 3: r = (s * d + ns + nd + 127) / 255; break;                                   /* multiply */
    case 4: r = s + d - (s * d + 127) / 255; break;                                     /* screen */
    case 5: r = ((s * da > d * sa ? s * da : d * sa) + ns + nd + 127) / 255; break;      /* lighten */
    case 6: r = ((s * da < d * sa ? s * da : d * sa) + ns + nd + 127) / 255; break;      /* darken */
    case 7: r = s + d - (2 * (s * da < d * sa ? s * da : d * sa) + 127) / 255; break;    /* difference */
    case 8: r = s + d; break;                                                           /* add */
    case 9: r = d - s; break;                                                           /* subtract */
    case 10: r = (sa * (da - d) + nd + 127) / 255; break;                               /* invert */
    case 13: case 14: {                                                                 /* overlay, hardlight */
        int hard = mode == 14, test = hard ? 2 * s <= sa : 2 * d <= da;
        if (test) r = (2 * s * d + ns + nd + 127) / 255;
        else r = (sa * da - 2 * (da - d) * (sa - s) + ns + nd + 127) / 255;
        break; }
    default: r = s + (d * (255 - sa) + 127) / 255; break;
    }
    if (r < 0) r = 0; if (r > 255) r = 255;
    return r;
}
/* draw() with a blend mode: the rendered layer is combined with the bitmap channel by channel */
void bmd_draw_layer_area_blend(BitmapData *t, const u32 *layer, PxRegion area, int mode) {
    u32 x, y, aw = area.x1 - area.x0; int opaque = !t->transparent;
    if (mode < 3 || mode > 14 || mode == 11 || mode == 12) { bmd_draw_layer_area(t, layer, area); return; }
    for (y = area.y0; y < area.y1; y++) for (x = area.x0; x < area.x1; x++) {
        u32 s = layer[(size_t)(y - area.y0) * aw + (x - area.x0)], d = PX(t, x, y); int sa = (int)A_(s), da = (int)A_(d), a, r, g, b;
        if (!sa && !(s & 0xFFFFFFu)) continue;
        a = mode == 9 || mode == 10 ? da : sa + da * (255 - sa) / 255;
        if (mode == 8) { a = sa + da; if (a > 255) a = 255; }
        r = blend_ch(mode, (int)R_(s), (int)R_(d), sa, da); g = blend_ch(mode, (int)G_(s), (int)G_(d), sa, da); b = blend_ch(mode, (int)B_(s), (int)B_(d), sa, da);
        if (opaque) a = 255;
        if (r > a) r = a; if (g > a) g = a; if (b > a) b = a;
        PX(t, x, y) = RGBA((u32)r, (u32)g, (u32)b, (u32)a);
    }
    bmd_changed(t);
}
void bmd_draw_layer(BitmapData *t, const u32 *layer, PxRegion area) {
    u32 x, y; int opaque = !t->transparent;
    reg_clamp(&area, t->w, t->h);
    for (y = area.y0; y < area.y1; y++) for (x = area.x0; x < area.x1; x++) {
        u32 s = layer[(size_t)y * t->w + x], c;
        if (!A_(s) && !(s & 0xFFFFFFu)) continue;
        c = bmd_blend_over(PX(t, x, y), s);
        if (opaque) c = WITH_ALPHA(c, 255);
        PX(t, x, y) = c;
    }
    bmd_changed(t);
}

static void blend_and_transform(BitmapData *src, BitmapData *dst, PxRegion sr, PxRegion dr, const SWFCxform *cx) {
    u32 x, y; int opaque = !dst->transparent;
    for (y = 0; y < RH(dr); y++) for (x = 0; x < RW(dr); x++) {
        u32 c = PX(src, sr.x0 + x, sr.y0 + y);
        c = bmd_premul(cx_color(cx, bmd_unmul(c)), 1);
        c = bmd_blend_over(PX(dst, dr.x0 + x, dr.y0 + y), c);
        if (opaque) c = WITH_ALPHA(c, 255);
        PX(dst, dr.x0 + x, dr.y0 + y) = c;
    }
    bmd_changed(dst);
}

int bmd_draw_bitmap_fast(BitmapData *t, BitmapData *src, const SWFMatrix *m, const SWFCxform *cx, int blend_mode, const s32 *clip) {
    PxRegion sr, dr; s32 tx, ty, cx0, cy0, cw, ch;
    (void)blend_mode;
    if (m->sx != 1.0 || m->r0 != 0.0 || m->r1 != 0.0 || m->sy != 1.0) return 0;
    sr = reg_whole(src->w, src->h); dr = reg_whole(t->w, t->h);
    tx = (s32)floor(m->tx / 20.0); ty = (s32)floor(m->ty / 20.0);
    if (clip) { cx0 = clip[0]; cy0 = clip[1]; cw = clip[2]; ch = clip[3]; }
    else { cx0 = 0; cy0 = 0; cw = (s32)t->w; ch = (s32)t->h; }
    reg_clamp_with_intersection(&dr, cx0, cy0, cx0 - tx, cy0 - ty, cw, ch, &sr);
    if (!cx_is_identity(cx)) blend_and_transform(src, t, sr, dr, cx);
    else bmd_copy_region(src, t, sr, dr, src->transparent);
    return 1;
}
