/* avm1_filters.c - flash.filters.* for AVM1 and the filter lists of
 * display objects.
 *
 * Script filter objects keep their values as the Flash Player does
 * (doubles, clamped by the setters).  A display object stores filters in
 * their SWF form (16.16 and 8.8 fixed point, quality as a pass count), so
 * reading `filters` back returns quantized copies, like in Flash.
 * Filters are not rendered yet. */
#include "avm1_int.h"
#include "reader.h"
#include "bmd.h"
#include "filters.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define DE  PA_DONT_ENUM
#define DD  PA_DONT_DELETE
#define RO  PA_READ_ONLY
#define V8  PA_VERSION_8

extern AObj *lib_class(VM *vm, Env *env, NativeFn ctor, NativeFn call, AObj *super_proto, int proto_first, AObj **proto_out);
extern Value a1_new_geom(Act *a, const char *cls, int argc, Value *argv);

enum { FK_DROPSHADOW = 0, FK_BLUR, FK_GLOW, FK_BEVEL, FK_GRADIENTGLOW, FK_CONVOLUTION, FK_COLORMATRIX, FK_GRADIENTBEVEL, FK_DISPLACEMENT, FK_COUNT };
static const char *class_names[FK_COUNT] = { "DropShadowFilter", "BlurFilter", "GlowFilter", "BevelFilter", "GradientGlowFilter", "ConvolutionFilter", "ColorMatrixFilter", "GradientBevelFilter", "DisplacementMapFilter" };

enum { BT_INNER = 0, BT_OUTER, BT_FULL };
enum { DM_WRAP = 0, DM_CLAMP, DM_IGNORE, DM_COLOR };

typedef struct FData {
    int    kind;
    double distance, angle, blur_x, blur_y;     /* angle in radians */
    u8     color[4], highlight[4], shadow[4];    /* r g b a */
    int    quality, inner, knockout, hide_object, type;
    u16    strength;                             /* 8.8 */
    int    ncolors; u8 gcolor[16][4]; u8 ratio[16];
    int    mx, my; float *cm; int ncm; float divisor, bias; int preserve_alpha, clamp;
    float  m[20];
    AObj  *map_bitmap; s32 map_x, map_y; s32 comp_x, comp_y; float scale_x, scale_y; int mode;
} FData;

/* ---------------------------------------------------------------- */
/* conversions with Rust's saturating casts                          */

static u8 sat_u8(double d) { if (d != d || d <= 0) return 0; if (d >= 255) return 255; return (u8)d; }
static u16 sat_u16(double d) { if (d != d || d <= 0) return 0; if (d >= 65535) return 65535; return (u16)d; }
static double clamp_d(double v, double lo, double hi) { if (v != v) return v; return v < lo ? lo : v > hi ? hi : v; }
static double clamp_also_nan(double v, double lo, double hi) { /* self.max(min).min(max) */ double r = (v != v || v < lo) ? lo : v; return r > hi ? hi : r; }
static s32 clamp_i(s32 v, s32 lo, s32 hi) { return v < lo ? lo : v > hi ? hi : v; }
static void set_rgb(u8 *c, u32 rgb) { c[0] = (u8)(rgb >> 16); c[1] = (u8)(rgb >> 8); c[2] = (u8)rgb; }
static double rgb_of(const u8 *c) { return (double)(((u32)c[0] << 16) | ((u32)c[1] << 8) | c[2]); }

static void fdefaults(FData *f, int kind) {
    memset(f, 0, sizeof(*f));
    f->kind = kind;
    f->distance = 4; f->angle = 0.785398163; f->blur_x = f->blur_y = 4; f->quality = 1; f->strength = 1 << 8;
    switch (kind) {
    case FK_DROPSHADOW: f->color[3] = 255; break;
    case FK_GLOW: f->color[0] = 255; f->color[3] = 255; f->blur_x = f->blur_y = 6; f->strength = 2 << 8; break;
    case FK_BEVEL: f->highlight[0] = f->highlight[1] = f->highlight[2] = f->highlight[3] = 255; f->shadow[3] = 255; f->type = BT_INNER; break;
    case FK_GRADIENTGLOW: case FK_GRADIENTBEVEL: f->type = BT_INNER; break;
    case FK_CONVOLUTION: f->divisor = 1; f->preserve_alpha = 1; f->clamp = 1; break;
    case FK_COLORMATRIX: f->m[0] = f->m[6] = f->m[12] = f->m[18] = 1; break;
    default: break;
    }
}

static FData *fdup(const FData *f) {
    FData *n = (FData*)malloc(sizeof(FData));
    if (!n) return NULL;
    *n = *f;
    if (f->cm && f->ncm) { n->cm = (float*)malloc((size_t)f->ncm * sizeof(float)); if (n->cm) memcpy(n->cm, f->cm, (size_t)f->ncm * sizeof(float)); else n->ncm = 0; }
    else { n->cm = NULL; n->ncm = 0; }
    return n;
}
static void ffree(FData *f) { if (f) { free(f->cm); free(f); } }

/* ---------------------------------------------------------------- */
/* objects                                                          */

static void fobj_free(AObj *o) { ffree((FData*)o->nat.p); o->nat.p = NULL; }
static void fobj_mark(VM *vm, AObj *o) { FData *f = (FData*)o->nat.p; if (f && f->map_bitmap) gc_mark_obj(vm, f->map_bitmap); }

static FData *fdata_of(AObj *o) {
    if (o && o->nk == NK_SUPER) o = o->nat.sup.self;
    return o && o->nk == NK_FILTER ? (FData*)o->nat.p : NULL;
}
static FData *fdata_kind(AObj *o, int kind) { FData *f = fdata_of(o); return f && f->kind == kind ? f : NULL; }

static void attach(AObj *t, FData *f) { t->nk = NK_FILTER; t->nsub = (u8)f->kind; t->nat.p = f; t->nfree = fobj_free; t->nmark = fobj_mark; }

/* a script object for stored filter data (proto: flash.filters.X.prototype) */
static Value filter_object(Act *a, const FData *src) {
    AObj *obj; FData *f; char path[64];
    sprintf(path, "flash.filters.%s", class_names[src->kind]);
    obj = a1_new_with_proto(a, path);
    if (!obj) return v_undef();
    f = fdup(src);
    if (!f) return v_obj(obj);
    attach(obj, f);
    return v_obj(obj);
}

/* setters, in the Flash Player's coercion and clamping */
static int set_f64(Act *a, Value v, double *out) { double d = v_num_of(a, v); if (a->vm->err) return 0; *out = d; return 1; }

typedef enum {
    P_DISTANCE = 1, P_ANGLE, P_COLOR, P_ALPHA, P_QUALITY, P_INNER, P_KNOCKOUT, P_BLURX, P_BLURY, P_STRENGTH, P_HIDE,
    P_HCOLOR, P_HALPHA, P_SCOLOR, P_SALPHA, P_TYPE, P_COLORS, P_ALPHAS, P_RATIOS, P_MATRIXX, P_MATRIXY, P_CMATRIX,
    P_DIVISOR, P_BIAS, P_PRESERVE, P_CLAMP, P_CALPHA, P_MATRIX, P_MAPBITMAP, P_MAPPOINT, P_COMPX, P_COMPY, P_SCALEX, P_SCALEY, P_MODE
} Prop_;

static void resize_cm(FData *f) {
    int n = f->mx * f->my;
    if (n > f->ncm) {
        float *nm = (float*)realloc(f->cm, (size_t)n * sizeof(float));
        if (!nm) return;
        memset(nm + f->ncm, 0, (size_t)(n - f->ncm) * sizeof(float));
        f->cm = nm; f->ncm = n;
    }
}

static int setp(Act *a, AObj *self, FData *f, int p, Value v) {
    (void)self;
    double d; s32 i; AStr *s;
    switch (p) {
    case P_DISTANCE: if (!set_f64(a, v, &d)) return 0; f->distance = d; break;
    case P_ANGLE: if (!set_f64(a, v, &d)) return 0; f->angle = fmod(d, 360.0) * (3.14159265358979323846 / 180.0); break;
    case P_COLOR: { u32 c = v_u32(a, v); if (a->vm->err) return 0; set_rgb(f->color, c); break; }
    case P_HCOLOR: { u32 c = v_u32(a, v); if (a->vm->err) return 0; set_rgb(f->highlight, c); break; }
    case P_SCOLOR: { u32 c = v_u32(a, v); if (a->vm->err) return 0; set_rgb(f->shadow, c); break; }
    case P_ALPHA: if (!set_f64(a, v, &d)) return 0; f->color[3] = sat_u8(d * 255.0); break;
    case P_HALPHA: if (!set_f64(a, v, &d)) return 0; f->highlight[3] = sat_u8(d * 255.0); break;
    case P_SALPHA: if (!set_f64(a, v, &d)) return 0; f->shadow[3] = sat_u8(d * 255.0); break;
    case P_CALPHA: if (!set_f64(a, v, &d)) return 0; f->color[3] = sat_u8(clamp_also_nan(d, 0, 1) * 255.0); break;
    case P_QUALITY: i = v_i32(a, v); if (a->vm->err) return 0; f->quality = clamp_i(i, 0, 15); break;
    case P_INNER: f->inner = v_as_bool(v, a->swf_version); break;
    case P_KNOCKOUT: f->knockout = v_as_bool(v, a->swf_version); break;
    case P_HIDE: f->hide_object = v_as_bool(v, a->swf_version); break;
    case P_PRESERVE: f->preserve_alpha = v_as_bool(v, a->swf_version); break;
    case P_CLAMP: f->clamp = v_as_bool(v, a->swf_version); break;
    case P_BLURX: if (!set_f64(a, v, &d)) return 0;
        f->blur_x = (f->kind == FK_GRADIENTGLOW || f->kind == FK_GRADIENTBEVEL) ? clamp_also_nan(d, 0, 255) : clamp_d(d, 0, 255); break;
    case P_BLURY: if (!set_f64(a, v, &d)) return 0;
        f->blur_y = (f->kind == FK_GRADIENTGLOW || f->kind == FK_GRADIENTBEVEL) ? clamp_also_nan(d, 0, 255) : clamp_d(d, 0, 255); break;
    case P_STRENGTH: { u16 st; if (!set_f64(a, v, &d)) return 0; st = sat_u16(d * 256.0); f->strength = st > 0xFF00 ? 0xFF00 : st; break; }
    case P_TYPE:
        s = v_str_of(a, v); if (a->vm->err) return 0;
        f->type = str_eq_c(s, "inner") ? BT_INNER : str_eq_c(s, "outer") ? BT_OUTER : BT_FULL;
        break;
    case P_MODE:
        s = v_str_of(a, v); if (a->vm->err) return 0;
        f->mode = str_eq_c(s, "clamp") ? DM_CLAMP : str_eq_c(s, "ignore") ? DM_IGNORE : str_eq_c(s, "color") ? DM_COLOR : DM_WRAP;
        break;
    case P_COLORS: {
        AObj *o = v_to_obj_or_bare(a, v); int len, n, k;
        if (a->vm->err || !o) return 0;
        len = o_length(a, o); if (a->vm->err) return 0;
        if (len < 0) len = 0;
        n = len < 16 ? len : 16;
        f->ncolors = n;
        for (k = 0; k < n; k++) {
            Value e = o_get_element(a, o, k); s32 c;
            if (a->vm->err) return 0;
            c = v_i32(a, e); if (a->vm->err) return 0;
            set_rgb(f->gcolor[k], (u32)c);
        }
        break; }
    case P_ALPHAS: {
        AObj *o = v.t == V_OBJ ? v.u.o : NULL; int len, k;
        if (!o) break;
        len = o_length(a, o); if (a->vm->err) return 0;
        for (k = 0; k < f->ncolors; k++) {
            u8 al = 255;
            if (k < len) {
                Value e = o_get_element(a, o, k); double x;
                if (a->vm->err) return 0;
                x = v_num_of(a, e); if (a->vm->err) return 0;
                al = (x == x && x - x == 0) ? sat_u8(255.0 * x + 0.5) : 255;
            }
            f->gcolor[k][3] = al;
        }
        break; }
    case P_RATIOS: {
        AObj *o = v.t == V_OBJ ? v.u.o : NULL; int len, k;
        if (!o) break;
        len = o_length(a, o); if (a->vm->err) return 0;
        if (len < 0) len = 0;
        if (len < f->ncolors) f->ncolors = len;
        for (k = 0; k < f->ncolors; k++) {
            Value e = o_get_element(a, o, k); s32 r;
            if (a->vm->err) return 0;
            r = v_i32(a, e); if (a->vm->err) return 0;
            f->ratio[k] = (u8)clamp_i(r, 0, 255);
        }
        break; }
    case P_MATRIXX: i = v_i32(a, v); if (a->vm->err) return 0; f->mx = clamp_i(i, 0, 15); resize_cm(f); break;
    case P_MATRIXY: i = v_i32(a, v); if (a->vm->err) return 0; f->my = clamp_i(i, 0, 15); resize_cm(f); break;
    case P_CMATRIX: {
        AObj *o = v_to_obj_or_bare(a, v); int len, k;
        if (a->vm->err || !o) return 0;
        len = o_length(a, o); if (a->vm->err) return 0;
        if (len < 0) len = 0;
        free(f->cm); f->cm = len ? (float*)calloc((size_t)len, sizeof(float)) : NULL; f->ncm = f->cm ? len : 0;
        for (k = 0; k < f->ncm; k++) {
            Value e = o_get_element(a, o, k); double x;
            if (a->vm->err) return 0;
            x = v_num_of(a, e); if (a->vm->err) return 0;
            if (k < f->ncm) f->cm[k] = (float)x;
        }
        resize_cm(f);
        break; }
    case P_DIVISOR: if (!set_f64(a, v, &d)) return 0; f->divisor = (float)d; break;
    case P_BIAS: if (!set_f64(a, v, &d)) return 0; f->bias = (float)d; break;
    case P_MATRIX: {
        float m[20]; int k;
        for (k = 0; k < 20; k++) m[k] = (float)(0.0 / 0.0);
        if (v.t == V_UNDEF || v.t == V_NULL) return 1;
        if (v.t == V_OBJ) {
            for (k = 0; k < 20; k++) {
                int len = o_length(a, v.u.o); if (a->vm->err) return 0;
                if (k < len) { Value e = o_get_element(a, v.u.o, k); double x; if (a->vm->err) return 0; x = v_num_of(a, e); if (a->vm->err) return 0; m[k] = (float)x; }
            }
        }
        memcpy(f->m, m, sizeof(m));
        break; }
    case P_MAPBITMAP:
        if (v.t == V_OBJ && v.u.o->nk == NK_BITMAPDATA) f->map_bitmap = v.u.o;
        break;
    case P_MAPPOINT:
        if (v.t == V_OBJ) {
            Value x, y; int hx, hy; double dx = 0, dy = 0; int ex = 0, ey = 0;
            hx = o_get_local_stored(a, v.u.o, str_intern(a->vm, "x"), &x);
            hy = o_get_local_stored(a, v.u.o, str_intern(a->vm, "y"), &y);
            int err1 = 0; Value thrown1 = v_undef();
            /* both coordinates are converted; the first error is rethrown after the value is set */
            if (hx) { dx = v_num_of(a, x); if (a->vm->err) { ex = 1; err1 = a->vm->err; thrown1 = a->vm->thrown; a->vm->err = 0; } }
            if (hy) { dy = v_num_of(a, y); if (a->vm->err) { ey = 1; if (!err1) { err1 = a->vm->err; thrown1 = a->vm->thrown; } a->vm->err = 0; } }
            f->map_x = f->map_y = 0;
            if (hx && hy) {
                f->map_x = ex ? (s32)0x80000000 : ((dx >= -2147483648.0 && dx <= 2147483647.0) ? (s32)dx : (s32)0x80000000);
                f->map_y = ey ? (s32)0x80000000 : ((dy >= -2147483648.0 && dy <= 2147483647.0) ? (s32)dy : (s32)0x80000000);
            }
            if (err1) { a->vm->err = err1; a->vm->thrown = thrown1; return 0; }
        } else f->map_x = f->map_y = 0;
        break;
    case P_COMPX: i = v_i32(a, v); if (a->vm->err) return 0; f->comp_x = i; break;
    case P_COMPY: i = v_i32(a, v); if (a->vm->err) return 0; f->comp_y = i; break;
    case P_SCALEX: if (!set_f64(a, v, &d)) return 0; f->scale_x = (float)clamp_also_nan(d, -65535.0, 65535.0); break;
    case P_SCALEY: if (!set_f64(a, v, &d)) return 0; f->scale_y = (float)clamp_also_nan(d, -65535.0, 65535.0); break;
    }
    return 1;
}

static Value array_of_doubles(Act *a, int n, const double *d) {
    Value items[32]; int i;
    for (i = 0; i < n && i < 32; i++) items[i] = v_num(d[i]);
    return v_obj(obj_array(a, n < 32 ? n : 32, items));
}

static Value getp(Act *a, FData *f, int p) {
    static const char *bt[] = { "inner", "outer", "full" };
    static const char *dm[] = { "wrap", "clamp", "ignore", "color" };
    switch (p) {
    case P_DISTANCE: return v_num(f->distance);
    case P_ANGLE: return v_num(f->angle * (180.0 / 3.14159265358979323846));
    case P_COLOR: return v_num(rgb_of(f->color));
    case P_HCOLOR: return v_num(rgb_of(f->highlight));
    case P_SCOLOR: return v_num(rgb_of(f->shadow));
    case P_ALPHA: case P_CALPHA: return v_num(f->color[3] / 255.0);
    case P_HALPHA: return v_num(f->highlight[3] / 255.0);
    case P_SALPHA: return v_num(f->shadow[3] / 255.0);
    case P_QUALITY: return v_num(f->quality);
    case P_INNER: return v_bool(f->inner);
    case P_KNOCKOUT: return v_bool(f->knockout);
    case P_HIDE: return v_bool(f->hide_object);
    case P_PRESERVE: return v_bool(f->preserve_alpha);
    case P_CLAMP: return v_bool(f->clamp);
    case P_BLURX: return v_num(f->blur_x);
    case P_BLURY: return v_num(f->blur_y);
    case P_STRENGTH: return v_num(f->strength / 256.0);
    case P_TYPE: return v_str(str_intern(a->vm, bt[f->type]));
    case P_MODE: return v_str(str_intern(a->vm, dm[f->mode]));
    case P_COLORS: { double d[16]; int k; for (k = 0; k < f->ncolors; k++) d[k] = rgb_of(f->gcolor[k]); return array_of_doubles(a, f->ncolors, d); }
    case P_ALPHAS: { double d[16]; int k; for (k = 0; k < f->ncolors; k++) d[k] = f->gcolor[k][3] / 255.0; return array_of_doubles(a, f->ncolors, d); }
    case P_RATIOS: { double d[16]; int k; for (k = 0; k < f->ncolors; k++) d[k] = f->ratio[k]; return array_of_doubles(a, f->ncolors, d); }
    case P_MATRIXX: return v_num(f->mx);
    case P_MATRIXY: return v_num(f->my);
    case P_CMATRIX: {
        Value *items = (Value*)malloc((size_t)(f->ncm ? f->ncm : 1) * sizeof(Value)); AObj *arr; int k;
        if (!items) return v_undef();
        for (k = 0; k < f->ncm; k++) items[k] = v_num(f->cm[k]);
        arr = obj_array(a, f->ncm, items);
        free(items);
        return v_obj(arr); }
    case P_DIVISOR: return v_num(f->divisor);
    case P_BIAS: return v_num(f->bias);
    case P_MATRIX: { double d[20]; int k; for (k = 0; k < 20; k++) d[k] = f->m[k]; return array_of_doubles(a, 20, d); }
    case P_MAPBITMAP: {
        extern BitmapData *a1_bitmapdata_of(AObj *o); extern AObj *a1_bitmapdata_wrap(Act *a, BitmapData *b);
        BitmapData *b = f->map_bitmap ? a1_bitmapdata_of(f->map_bitmap) : NULL; AObj *o;
        if (!b) return v_undef();
        o = a1_bitmapdata_wrap(a, b);
        return o ? v_obj(o) : v_undef(); }
    case P_MAPPOINT: { Value xy[2]; xy[0] = v_num(f->map_x); xy[1] = v_num(f->map_y); return a1_new_geom(a, "Point", 2, xy); }
    case P_COMPX: return v_num(f->comp_x);
    case P_COMPY: return v_num(f->comp_y);
    case P_SCALEX: return v_num(f->scale_x);
    case P_SCALEY: return v_num(f->scale_y);
    }
    return v_undef();
}

/* the table: per class, constructor argument order and declared properties */
typedef struct FProp { const char *name; int p; } FProp;
static const int ctor_args[FK_COUNT][13] = {
    /* DropShadow */ { P_DISTANCE, P_ANGLE, P_COLOR, P_ALPHA, P_BLURX, P_BLURY, P_STRENGTH, P_QUALITY, P_INNER, P_KNOCKOUT, P_HIDE, 0 },
    /* Blur */ { P_BLURX, P_BLURY, P_QUALITY, 0 },
    /* Glow */ { P_COLOR, P_ALPHA, P_BLURX, P_BLURY, P_STRENGTH, P_QUALITY, P_INNER, P_KNOCKOUT, 0 },
    /* Bevel */ { P_DISTANCE, P_ANGLE, P_HCOLOR, P_HALPHA, P_SCOLOR, P_SALPHA, P_BLURX, P_BLURY, P_STRENGTH, P_QUALITY, P_TYPE, P_KNOCKOUT, 0 },
    /* GradientGlow */ { P_DISTANCE, P_ANGLE, P_COLORS, P_ALPHAS, P_RATIOS, P_BLURX, P_BLURY, P_STRENGTH, P_QUALITY, P_TYPE, P_KNOCKOUT, 0 },
    /* Convolution: special */ { 0 },
    /* ColorMatrix */ { P_MATRIX, 0 },
    /* GradientBevel */ { P_DISTANCE, P_ANGLE, P_COLORS, P_ALPHAS, P_RATIOS, P_BLURX, P_BLURY, P_STRENGTH, P_QUALITY, P_TYPE, P_KNOCKOUT, 0 },
    /* DisplacementMap */ { P_MAPBITMAP, P_MAPPOINT, P_COMPX, P_COMPY, P_SCALEX, P_SCALEY, P_MODE, P_COLOR, P_CALPHA, 0 },
};
static const FProp props_ds[] = { { "distance", P_DISTANCE }, { "angle", P_ANGLE }, { "color", P_COLOR }, { "alpha", P_ALPHA }, { "quality", P_QUALITY }, { "inner", P_INNER }, { "knockout", P_KNOCKOUT }, { "blurX", P_BLURX }, { "blurY", P_BLURY }, { "strength", P_STRENGTH }, { "hideObject", P_HIDE }, { NULL, 0 } };
static const FProp props_blur[] = { { "blurX", P_BLURX }, { "blurY", P_BLURY }, { "quality", P_QUALITY }, { NULL, 0 } };
static const FProp props_glow[] = { { "color", P_COLOR }, { "alpha", P_ALPHA }, { "quality", P_QUALITY }, { "inner", P_INNER }, { "knockout", P_KNOCKOUT }, { "blurX", P_BLURX }, { "blurY", P_BLURY }, { "strength", P_STRENGTH }, { NULL, 0 } };
static const FProp props_bevel[] = { { "distance", P_DISTANCE }, { "angle", P_ANGLE }, { "highlightColor", P_HCOLOR }, { "highlightAlpha", P_HALPHA }, { "shadowColor", P_SCOLOR }, { "shadowAlpha", P_SALPHA }, { "quality", P_QUALITY }, { "strength", P_STRENGTH }, { "knockout", P_KNOCKOUT }, { "blurX", P_BLURX }, { "blurY", P_BLURY }, { "type", P_TYPE }, { NULL, 0 } };
static const FProp props_grad[] = { { "distance", P_DISTANCE }, { "angle", P_ANGLE }, { "colors", P_COLORS }, { "alphas", P_ALPHAS }, { "ratios", P_RATIOS }, { "blurX", P_BLURX }, { "blurY", P_BLURY }, { "quality", P_QUALITY }, { "strength", P_STRENGTH }, { "knockout", P_KNOCKOUT }, { "type", P_TYPE }, { NULL, 0 } };
static const FProp props_conv[] = { { "matrixX", P_MATRIXX }, { "matrixY", P_MATRIXY }, { "matrix", P_CMATRIX }, { "divisor", P_DIVISOR }, { "bias", P_BIAS }, { "preserveAlpha", P_PRESERVE }, { "clamp", P_CLAMP }, { "color", P_COLOR }, { "alpha", P_CALPHA }, { NULL, 0 } };
static const FProp props_cm[] = { { "matrix", P_MATRIX }, { NULL, 0 } };
static const FProp props_dm[] = { { "mapBitmap", P_MAPBITMAP }, { "mapPoint", P_MAPPOINT }, { "componentX", P_COMPX }, { "componentY", P_COMPY }, { "scaleX", P_SCALEX }, { "scaleY", P_SCALEY }, { "mode", P_MODE }, { "color", P_COLOR }, { "alpha", P_CALPHA }, { NULL, 0 } };
static const FProp *class_props[FK_COUNT] = { props_ds, props_blur, props_glow, props_bevel, props_grad, props_conv, props_cm, props_grad, props_dm };

/* table natives: index = kind * 128 + property * 2 + (0 get / 1 set); constructor = kind * 128 + 127 */
static Value fl_table(Act *a, AObj *self, int argc, Value *argv, int index) {
    int kind = index / 128, k = index % 128; FData *f;
    if (k == 127) {
        FData tmp, *nf; int i; AObj *t;
        fdefaults(&tmp, kind);
        if (kind == FK_CONVOLUTION) {
            static const int order[] = { P_MATRIXX, P_MATRIXY, P_CMATRIX };
            for (i = 0; i < 3 && i < argc; i++) if (!setp(a, self, &tmp, order[i], argv[i])) { free(tmp.cm); return v_undef(); }
            if (argc > 3) { if (!setp(a, self, &tmp, P_DIVISOR, argv[3])) { free(tmp.cm); return v_undef(); } }
            else if (argc > 0) { float s = 0; int q; for (q = 0; q < tmp.ncm; q++) s += tmp.cm[q]; tmp.divisor = s; }
            if (argc > 4 && !setp(a, self, &tmp, P_BIAS, argv[4])) { free(tmp.cm); return v_undef(); }
            if (argc > 5 && !setp(a, self, &tmp, P_PRESERVE, argv[5])) { free(tmp.cm); return v_undef(); }
            if (argc > 6 && !setp(a, self, &tmp, P_CLAMP, argv[6])) { free(tmp.cm); return v_undef(); }
            if (argc > 7) { if (!setp(a, self, &tmp, P_COLOR, argv[7])) { free(tmp.cm); return v_undef(); } tmp.color[3] = 255; }
            if (argc > 8 && !setp(a, self, &tmp, P_CALPHA, argv[8])) { free(tmp.cm); return v_undef(); }
        } else {
            for (i = 0; ctor_args[kind][i] && i < argc; i++) if (!setp(a, self, &tmp, ctor_args[kind][i], argv[i])) { free(tmp.cm); return v_undef(); }
        }
        t = o_native_target(self);
        if (t) { nf = fdup(&tmp); if (nf) attach(t, nf); }
        free(tmp.cm);
        return v_obj(self);
    }
    f = fdata_kind(self, kind);
    if (!f) return v_undef();
    if (k & 1) { if (argc > 0) setp(a, self, f, k / 2, argv[0]); return v_undef(); }
    return getp(a, f, k / 2);
}

static Value fl_clone(Act *a, AObj *self, int argc, Value *argv) {
    FData *f = fdata_of(self), *n; Value proto = v_undef(); AObj *o; (void)argc; (void)argv;
    if (!f) return v_undef();
    if (!o_get_local_stored(a, self->nk == NK_SUPER ? self->nat.sup.self : self, a->vm->s_proto, &proto)) proto = v_undef();
    o = proto.t == V_UNDEF ? obj_bare(a->vm) : obj_new_v(a->vm, proto);
    n = fdup(f);
    if (n) attach(o, n);
    return v_obj(o);
}

#define FCTOR(n, k) static Value n(Act *a, AObj *self, int argc, Value *argv) { return fl_table(a, self, argc, argv, (k) * 128 + 127); }
FCTOR(ctor_ds, FK_DROPSHADOW)
FCTOR(ctor_blur, FK_BLUR)
FCTOR(ctor_glow, FK_GLOW)
FCTOR(ctor_bevel, FK_BEVEL)
FCTOR(ctor_gglow, FK_GRADIENTGLOW)
FCTOR(ctor_conv, FK_CONVOLUTION)
FCTOR(ctor_cm, FK_COLORMATRIX)
FCTOR(ctor_gbevel, FK_GRADIENTBEVEL)
FCTOR(ctor_dm, FK_DISPLACEMENT)
static const NativeFn ctors[FK_COUNT] = { ctor_ds, ctor_blur, ctor_glow, ctor_bevel, ctor_gglow, ctor_conv, ctor_cm, ctor_gbevel, ctor_dm };

/* ASnative(11xx, n): 0 constructs, 2i+1 / 2i+2 get / set the i-th declared property */
static Value asn_filter(Act *a, AObj *self, int argc, Value *argv, int kind, int index) {
    const FProp *pp = class_props[kind]; int i;
    if (index == 0) return fl_table(a, self, argc, argv, kind * 128 + 127);
    i = (index - 1) / 2;
    { int n = 0; while (pp[n].name) n++; if (index < 1 || i >= n) return v_undef(); }
    if (!fdata_kind(self, kind)) {
        /* GradientGlow/GradientBevel share their accessors */
        if (kind == FK_GRADIENTGLOW && fdata_kind(self, FK_GRADIENTBEVEL)) kind = FK_GRADIENTBEVEL;
        else return v_undef();
    }
    return fl_table(a, self, argc, argv, kind * 128 + pp[i].p * 2 + ((index - 1) & 1));
}
#define ASNF(n, k) static Value n(Act *a, AObj *self, int argc, Value *argv, int index) { return asn_filter(a, self, argc, argv, k, index); }
ASNF(asn_ds, FK_DROPSHADOW)
ASNF(asn_blur, FK_BLUR)
ASNF(asn_glow, FK_GLOW)
ASNF(asn_bevel, FK_BEVEL)
ASNF(asn_conv, FK_CONVOLUTION)
ASNF(asn_cm, FK_COLORMATRIX)
ASNF(asn_dm, FK_DISPLACEMENT)
static Value asn_grad(Act *a, AObj *self, int argc, Value *argv, int index) {
    if (index == 1000) return fl_table(a, self, argc, argv, FK_GRADIENTBEVEL * 128 + 127);
    return asn_filter(a, self, argc, argv, FK_GRADIENTGLOW, index);
}
TableFn filter_table(int category) {
    switch (category) {
    case 1101: return asn_ds;
    case 1102: return asn_blur;
    case 1103: return asn_glow;
    case 1107: return asn_bevel;
    case 1108: return asn_grad;
    case 1109: return asn_conv;
    case 1110: return asn_cm;
    case 1111: return asn_dm;
    }
    return NULL;
}

void lib_filters(VM *vm, Env *env, AObj *object_proto, AObj *filters_pkg) {
    static const int order[] = { FK_DROPSHADOW, FK_BLUR, FK_GLOW, FK_BEVEL, FK_GRADIENTGLOW, FK_GRADIENTBEVEL, FK_CONVOLUTION, FK_COLORMATRIX, FK_DISPLACEMENT };
    AObj *bf_proto, *bf = lib_class(vm, env, NULL, NULL, object_proto, 1, &bf_proto); int k;
    AObj *classes[FK_COUNT];
    o_define(vm, bf_proto, str_intern(vm, "clone"), v_obj(fn_native(vm, env, fl_clone, NULL)), 0);
    env->p.bitmapfilter = bf_proto;
    for (k = 0; k < FK_COUNT; k++) {
        AObj *proto, *c = lib_class(vm, env, ctors[k], NULL, bf_proto, 1, &proto); const FProp *pp;
        for (pp = class_props[k]; pp->name; pp++)
            o_add_property(vm, proto, str_intern(vm, pp->name), fn_table(vm, env, fl_table, k * 128 + pp->p * 2), fn_table(vm, env, fl_table, k * 128 + pp->p * 2 + 1), V8);
        classes[k] = c;
    }
    o_define(vm, filters_pkg, str_intern(vm, "BitmapFilter"), v_obj(bf), 0);
    for (k = 0; k < FK_COUNT; k++) o_define(vm, filters_pkg, str_intern(vm, class_names[order[k]]), v_obj(classes[order[k]]), 0);
}

/* ---------------------------------------------------------------- */
/* filter lists of display objects                                  */

typedef struct FList { int n; FData **f; } FList;

void a1_filters_free(void *p) {
    FList *l = (FList*)p; int i;
    if (!l) return;
    for (i = 0; i < l->n; i++) ffree(l->f[i]);
    free(l->f); free(l);
}

/* the renderer's form of a filter (filters.h) */
static s32 fx16d(double v) { return sf_fixed16(v); }
static void to_swf(const FData *f, SwfFilter *o) {
    extern BitmapData *a1_bitmapdata_of(AObj *o);
    memset(o, 0, sizeof(*o));
    o->kind = f->kind;
    o->blur_x = fx16d(f->blur_x); o->blur_y = fx16d(f->blur_y); o->angle = fx16d(f->angle); o->distance = fx16d(f->distance);
    o->strength = (s16)f->strength; o->passes = (u8)f->quality;
    o->flags = (f->inner ? SFF_INNER : 0) | (f->knockout ? SFF_KNOCKOUT : 0) | (f->hide_object ? 0 : SFF_COMPOSITE);
    memcpy(o->color, f->color, 4);
    if (f->kind == FK_BEVEL) { memcpy(o->color, f->shadow, 4); memcpy(o->color2, f->highlight, 4); }
    if (f->kind == FK_BEVEL || f->kind == FK_GRADIENTBEVEL || f->kind == FK_GRADIENTGLOW) {
        o->flags = (f->knockout ? SFF_KNOCKOUT : 0) | SFF_COMPOSITE | (f->type == BT_INNER ? SFF_INNER : 0) | (f->type == BT_FULL ? SFF_ONTOP : 0);
    }
    o->ncolors = f->ncolors; memcpy(o->gcolor, f->gcolor, sizeof(o->gcolor)); memcpy(o->ratio, f->ratio, sizeof(o->ratio));
    o->mx = f->mx; o->my = f->my; o->divisor = f->divisor; o->bias = f->bias;
    if (f->cm && f->ncm > 0) { o->cmatrix = (float*)malloc((size_t)f->ncm * sizeof(float)); if (o->cmatrix) { memcpy(o->cmatrix, f->cm, (size_t)f->ncm * sizeof(float)); o->ncm = f->ncm; } }
    if (f->kind == FK_CONVOLUTION) o->flags = (f->clamp ? SFF_CLAMP : 0) | (f->preserve_alpha ? SFF_PRESERVE : 0);
    memcpy(o->m, f->m, sizeof(o->m));
    o->comp_x = f->comp_x; o->comp_y = f->comp_y; o->map_x = f->map_x; o->map_y = f->map_y; o->mode = f->mode; o->scale_x = f->scale_x; o->scale_y = f->scale_y;
    if (f->map_bitmap) { BitmapData *b = a1_bitmapdata_of(f->map_bitmap); if (b) o->map_bmd = bmd_ref(b); }
}
static void sync_render(A1Clip *s, FList *l, MovieClip *c) {
    DisplayObject *d = c ? c->self : NULL; SwfFilter *out = NULL; int i;
    if (!d) return;
    if (l && l->n) { out = (SwfFilter*)calloc((size_t)l->n, sizeof(SwfFilter)); if (out) for (i = 0; i < l->n; i++) to_swf(l->f[i], &out[i]); }
    swf_filters_free(d->filters, d->nfilters);
    d->filters = out; d->nfilters = out ? l->n : 0;
    (void)s;
}
static void set_list_c(A1Clip *s, FList *l, MovieClip *c) { a1_filters_free(s->filters); s->filters = l; sync_render(s, l, c); }
#define set_list(s, l) set_list_c(s, l, c)
/* a filter object in the renderer's form (BitmapData.applyFilter) */
int a1_filter_record(AObj *o, SwfFilter *out) { FData *f = fdata_of(o); if (!f) return 0; to_swf(f, out); return 1; }

static FList *list_new(void) { return (FList*)calloc(1, sizeof(FList)); }
static void list_add(FList *l, FData *f) {
    FData **n = (FData**)realloc(l->f, (size_t)(l->n + 1) * sizeof(FData*));
    if (!n) { ffree(f); return; }
    l->f = n; l->f[l->n++] = f;
}

/* MovieClip / Button .filters */
Value a1_get_filters(Act *a, MovieClip *c) {
    FList *l = c && c->a1 ? (FList*)c->a1->filters : NULL; Value *items; AObj *arr; int i, n = l ? l->n : 0;
    items = (Value*)malloc((size_t)(n ? n : 1) * sizeof(Value));
    if (!items) return v_undef();
    for (i = 0; i < n; i++) { items[i] = filter_object(a, l->f[i]); if (a->vm->err) { free(items); return v_undef(); } }
    arr = obj_array(a, n, items);
    free(items);
    return v_obj(arr);
}

void a1_set_filters(Act *a, MovieClip *c, Value v) {
    FList *l = list_new(); A1Clip *s = c ? a1_of(c) : NULL;
    if (!l || !s) { a1_filters_free(l); return; }
    if (v.t == V_OBJ) {
        int n = 0, i; AStr **keys = o_keys(a, v.u.o, 0, &n);
        for (i = n - 1; i >= 0; i--) {
            Value e = o_get(a, v.u.o, keys[i]); AObj *o; FData *f;
            if (a->vm->err) break;
            o = v_to_obj_or_bare(a, e); if (a->vm->err) break;
            f = fdata_of(o);
            if (f) { FData *q = fdup(f); if (q) list_add(l, q); }
        }
        free(keys);
        if (a->vm->err) { a1_filters_free(l); return; }
    }
    set_list(s, l);
}

/* PlaceObject3 filter lists */
static double rd_fixed(Reader *r) { return (s32)r_u32(r) / 65536.0; }
static double rd_fixed8(Reader *r) { return (s16)r_u16(r) / 256.0; }
static void rd_rgba(Reader *r, u8 *c) { c[0] = r_u8(r); c[1] = r_u8(r); c[2] = r_u8(r); c[3] = r_u8(r); }
static float rd_float(Reader *r) { union { u32 u; float f; } x; x.u = r_u32(r); return x.f; }

void a1_place_filters(MovieClip *c, const u8 *data, size_t len) {
    Reader r; int n, i; FList *l; A1Clip *s = a1_of(c);
    if (!s) return;
    reader_init(&r, data, len);
    n = r_u8(&r);
    l = list_new();
    if (!l) return;
    for (i = 0; i < n && !reader_eof(&r); i++) {
        int id = r_u8(&r); FData f; int flags;
        fdefaults(&f, id <= 7 ? id : FK_BLUR);
        switch (id) {
        case 0:
            rd_rgba(&r, f.color); f.blur_x = rd_fixed(&r); f.blur_y = rd_fixed(&r); f.angle = rd_fixed(&r); f.distance = rd_fixed(&r);
            f.strength = sat_u16(rd_fixed8(&r) * 256.0); flags = r_u8(&r);
            f.inner = (flags & 0x80) != 0; f.knockout = (flags & 0x40) != 0; f.hide_object = !(flags & 0x20); f.quality = flags & 0x1F;
            break;
        case 1:
            f.blur_x = rd_fixed(&r); f.blur_y = rd_fixed(&r); flags = r_u8(&r); f.quality = flags >> 3;
            break;
        case 2:
            rd_rgba(&r, f.color); f.blur_x = rd_fixed(&r); f.blur_y = rd_fixed(&r);
            f.strength = sat_u16(rd_fixed8(&r) * 256.0); flags = r_u8(&r);
            f.inner = (flags & 0x80) != 0; f.knockout = (flags & 0x40) != 0; f.quality = flags & 0x1F;
            break;
        case 3:
            rd_rgba(&r, f.highlight); rd_rgba(&r, f.shadow); f.blur_x = rd_fixed(&r); f.blur_y = rd_fixed(&r); f.angle = rd_fixed(&r); f.distance = rd_fixed(&r);
            f.strength = sat_u16(rd_fixed8(&r) * 256.0); flags = r_u8(&r);
            f.knockout = (flags & 0x40) != 0; f.quality = flags & 0x0F;
            f.type = (flags & 0x10) ? BT_FULL : (flags & 0x80) ? BT_INNER : BT_OUTER;
            break;
        case 4: case 7: {
            int nc = r_u8(&r), k;
            for (k = 0; k < nc; k++) { u8 col[4]; rd_rgba(&r, col); if (k < 16) memcpy(f.gcolor[k], col, 4); }
            for (k = 0; k < nc; k++) { u8 ra = r_u8(&r); if (k < 16) f.ratio[k] = ra; }
            f.ncolors = nc < 16 ? nc : 16;
            f.blur_x = rd_fixed(&r); f.blur_y = rd_fixed(&r); f.angle = rd_fixed(&r); f.distance = rd_fixed(&r);
            f.strength = sat_u16(rd_fixed8(&r) * 256.0); flags = r_u8(&r);
            f.knockout = (flags & 0x40) != 0; f.quality = flags & 0x0F;
            f.type = (flags & 0x10) ? BT_FULL : (flags & 0x80) ? BT_INNER : BT_OUTER;
            break; }
        case 5: {
            int k;
            f.mx = r_u8(&r); f.my = r_u8(&r); f.divisor = rd_float(&r); f.bias = rd_float(&r);
            f.ncm = f.mx * f.my; f.cm = f.ncm ? (float*)malloc((size_t)f.ncm * sizeof(float)) : NULL;
            for (k = 0; k < f.mx * f.my; k++) { float x = rd_float(&r); if (f.cm) f.cm[k] = x; }
            if (!f.cm) f.ncm = 0;
            rd_rgba(&r, f.color); flags = r_u8(&r);
            f.clamp = (flags & 0x02) != 0; f.preserve_alpha = (flags & 0x01) != 0;
            break; }
        case 6: { int k; for (k = 0; k < 20; k++) f.m[k] = rd_float(&r); break; }
        default: i = n; continue;
        }
        { FData *q = fdup(&f); if (q) list_add(l, q); }
        free(f.cm);
    }
    set_list(s, l);
}
