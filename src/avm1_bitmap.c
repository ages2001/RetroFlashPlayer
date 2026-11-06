/* avm1_bitmap.c - flash.display.BitmapData for AVM1.
 *
 * Follows Ruffle's core/src/avm1/globals/bitmap_data.rs: every method
 * returns a negative number on bad arguments (despite the documentation),
 * and a disposed bitmap answers -1 to everything. */
#include "avm1_int.h"
#include "bmd.h"
#include "render_filters.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define DE  PA_DONT_ENUM
#define DD  PA_DONT_DELETE

extern AObj *lib_class(VM *vm, Env *env, NativeFn ctor, NativeFn call, AObj *super_proto, int proto_first, AObj **proto_out);
extern Value a1_new_geom(Act *a, const char *cls, int argc, Value *argv);
extern double *a1_color_transform_of(Value v);
extern void a1_cxform_from(const double *c, SWFCxform *cx);
extern s32 tw_from_px(double px);
extern void player_bmd_draw_blend(Player *p, BitmapData *t, DisplayObject *d, BitmapData *src, const SWFMatrix *m, const SWFCxform *cx, const s32 *clip, int smoothing, int blend);
extern Bitmap *image_get(SWFMovie *m, int id);

static void defm(VM *vm, Env *env, AObj *o, const char *name, NativeFn f, u16 attr) { o_define(vm, o, str_intern(vm, name), v_obj(fn_native(vm, env, f, NULL)), attr); }
static void defp(VM *vm, Env *env, AObj *o, const char *name, NativeFn g, NativeFn s, u16 attr) { o_add_property(vm, o, str_intern(vm, name), fn_native(vm, env, g, NULL), s ? fn_native(vm, env, s, NULL) : NULL, attr); }

#define ERR (a->vm->err)
#define CK(x) do { if (ERR) return (x); } while (0)
#define S(name) str_intern(a->vm, name)

static void bd_free(AObj *o) { bmd_unref((BitmapData*)o->nat.p); o->nat.p = NULL; }

/* the BitmapData of an object (any state) */
BitmapData *a1_bitmapdata_of(AObj *o) {
    if (o && o->nk == NK_SUPER) o = o->nat.sup.self;
    return o && o->nk == NK_BITMAPDATA ? (BitmapData*)o->nat.p : NULL;
}

/* get_bitmap_data: 1 valid, 0 disposed, -1 not a BitmapData */
static int bd_state(AObj *o, BitmapData **out) {
    BitmapData *b = a1_bitmapdata_of(o);
    *out = b;
    if (!b) return -1;
    return b->disposed ? 0 : 1;
}

static BitmapData *valid(AObj *o) { BitmapData *b; return bd_state(o, &b) == 1 ? b : NULL; }

static void set_native(AObj *o, BitmapData *b) {
    if (o->nfree) o->nfree(o);
    o->nk = NK_BITMAPDATA; o->nat.p = b; o->nfree = bd_free;
}

/* a new script object for an existing BitmapData (proto: flash.display.BitmapData.prototype) */
AObj *a1_bitmapdata_wrap(Act *a, BitmapData *b) {
    AObj *o = a1_new_with_proto(a, "flash.display.BitmapData");
    if (o) set_native(o, bmd_ref(b));
    return o;
}

static AObj *new_bitmap_data(Act *a, int has_proto, Value proto, BitmapData *b) {
    AObj *o = obj_bare(a->vm);
    if (has_proto) o_define(a->vm, o, a->vm->s_proto, proto, DE | DD);
    set_native(o, b);
    return o;
}

static Value num(double d) { return v_num(d); }
static Value neg(int n) { return v_num((double)n); }

/* x, y, width, height through has_property / get (try_get_rect) */
static int try_get_rect(Act *a, AObj *o, s32 *r) {
    static const char *names[4] = { "x", "y", "width", "height" }; int i;
    for (i = 0; i < 4; i++) { int h = o_has_property(a, o, S(names[i])); if (ERR || !h) return 0; }
    for (i = 0; i < 4; i++) { Value v = o_get(a, o, S(names[i])); if (ERR) return 0; r[i] = v_i32(a, v); if (ERR) return 0; }
    return 1;
}

/* f64 `as i32` (saturating, NaN -> 0) */
static s32 d_i32(double d) { return d != d ? 0 : d >= 2147483647.0 ? 2147483647 : d <= -2147483648.0 ? (s32)0x80000000 : (s32)d; }
static u32 d_u32(double d) { return d != d || d <= 0 ? 0 : d >= 4294967295.0 ? 0xFFFFFFFFu : (u32)d; }

/* dest_point.x / .y as f64 then `as i32` */
static void get_point(Act *a, AObj *o, s32 *x, s32 *y) {
    Value v = o_get(a, o, S("x")); double d;
    if (ERR) return;
    d = v_num_of(a, v); if (ERR) return;
    *x = d_i32(d);
    v = o_get(a, o, S("y")); if (ERR) return;
    d = v_num_of(a, v); if (ERR) return;
    *y = d_i32(d);
}

static AObj *get_object(Act *a, int argc, Value *argv, int i) { return v_to_obj_or_bare(a, arg(argc, argv, i)); }

static Value make_rect(Act *a, double x, double y, double w, double h) {
    Value args[4], r;
    args[0] = v_num(x); args[1] = v_num(y); args[2] = v_num(w); args[3] = v_num(h);
    r = a1_new_geom(a, "Rectangle", 4, args);
    if (ERR) return v_undef();
    return r.t == V_OBJ ? r : neg(-1);
}

/* ---------------------------------------------------------------- */

static Value bd_ctor(Act *a, AObj *self, int argc, Value *argv) {
    u32 w, h, fill = 0xFFFFFFFFu; int tr = 1; AObj *t; BitmapData *b;
    if (argc < 2) return v_undef();
    w = v_u32(a, argv[0]); CK(v_undef());
    h = v_u32(a, argv[1]); CK(v_undef());
    if (argc > 2) tr = v_bool_of(a, argv[2]);
    if (argc > 3) { fill = v_u32(a, argv[3]); CK(v_undef()); }
    if (!bmd_size_valid(a->swf_version, w, h)) return v_undef();
    t = o_native_target(self);
    if (!t) return v_obj(self);
    b = bmd_new(w, h, tr, fill);
    if (!b) return v_undef();
    set_native(t, b);
    return v_obj(self);
}

static Value bd_height(Act *a, AObj *self, int argc, Value *argv) { BitmapData *b = valid(self); (void)a; (void)argc; (void)argv; return b ? num(b->h) : neg(-1); }
static Value bd_width(Act *a, AObj *self, int argc, Value *argv) { BitmapData *b = valid(self); (void)a; (void)argc; (void)argv; return b ? num(b->w) : neg(-1); }
static Value bd_transparent(Act *a, AObj *self, int argc, Value *argv) { BitmapData *b = valid(self); (void)a; (void)argc; (void)argv; return b ? v_bool(b->transparent) : neg(-1); }
static Value bd_rectangle(Act *a, AObj *self, int argc, Value *argv) { BitmapData *b = valid(self); (void)argc; (void)argv; return b ? make_rect(a, 0, 0, b->w, b->h) : neg(-1); }

static Value bd_get_pixel(Act *a, AObj *self, int argc, Value *argv) {
    BitmapData *b; u32 x, y;
    if (argc < 2 || !(b = valid(self))) return neg(-1);
    x = v_u32(a, argv[0]); CK(v_undef()); y = v_u32(a, argv[1]); CK(v_undef());
    b = valid(self); if (!b) return neg(-1);
    return num((s32)bmd_get_pixel(b, x, y));
}

static Value bd_get_pixel32(Act *a, AObj *self, int argc, Value *argv) {
    BitmapData *b; u32 x, y;
    if (argc < 2 || !(b = valid(self))) return neg(-1);
    x = v_u32(a, argv[0]); CK(v_undef()); y = v_u32(a, argv[1]); CK(v_undef());
    b = valid(self); if (!b) return neg(-1);
    return num((s32)bmd_get_pixel32(b, x, y));
}

static Value bd_set_pixel(Act *a, AObj *self, int argc, Value *argv) {
    BitmapData *b; u32 x, y, c;
    if (argc < 3 || !(b = valid(self))) return neg(-1);
    x = v_u32(a, argv[0]); CK(v_undef()); y = v_u32(a, argv[1]); CK(v_undef()); c = v_u32(a, argv[2]); CK(v_undef());
    b = valid(self); if (!b) return neg(-1);
    bmd_set_pixel(b, x, y, c);
    return num(0);
}

static Value bd_set_pixel32(Act *a, AObj *self, int argc, Value *argv) {
    BitmapData *b; u32 x, y, c;
    if (argc < 3 || !(b = valid(self))) return neg(-1);
    x = v_u32(a, argv[0]); CK(v_undef()); y = v_u32(a, argv[1]); CK(v_undef()); c = v_u32(a, argv[2]); CK(v_undef());
    b = valid(self); if (!b) return neg(-1);
    bmd_set_pixel32(b, x, y, c);
    return num(0);
}

/* the source BitmapData argument: NULL with *ret set on failure */
static BitmapData *src_arg(Act *a, int argc, Value *argv, int i, Value *ret) {
    AObj *o = get_object(a, argc, argv, i); BitmapData *s; int st;
    if (ERR) { *ret = v_undef(); return NULL; }
    st = bd_state(o, &s);
    if (st == 1) return s;
    *ret = neg(st == 0 ? -3 : -2);
    return NULL;
}

static Value bd_copy_channel(Act *a, AObj *self, int argc, Value *argv) {
    BitmapData *b, *s; Value ret; AObj *ro, *po; s32 r[4], sc, dc, dx, dy;
    if (argc < 5 || !valid(self)) return neg(-1);
    if (!(s = src_arg(a, argc, argv, 0, &ret))) return ret;
    ro = get_object(a, argc, argv, 1); CK(v_undef());
    if (!try_get_rect(a, ro, r)) { CK(v_undef()); return neg(-4); }
    po = get_object(a, argc, argv, 2); CK(v_undef());
    sc = v_i32(a, argv[3]); CK(v_undef());
    dc = v_i32(a, argv[4]); CK(v_undef());
    dx = v_i32(a, o_get(a, po, S("x"))); CK(v_undef());
    dy = v_i32(a, o_get(a, po, S("y"))); CK(v_undef());
    b = valid(self); if (!b || s->disposed) return neg(-1);
    bmd_copy_channel(b, dx, dy, r[0], r[1], r[2], r[3], s, sc, dc);
    return neg(-1);
}

static Value bd_fill_rect(Act *a, AObj *self, int argc, Value *argv) {
    BitmapData *b; AObj *ro; s32 r[4]; u32 c;
    if (argc < 2 || !valid(self)) return neg(-1);
    ro = get_object(a, argc, argv, 0); CK(v_undef());
    if (!try_get_rect(a, ro, r)) { CK(v_undef()); return neg(-1); }
    c = v_u32(a, argv[1]); CK(v_undef());
    b = valid(self); if (!b) return neg(-1);
    bmd_fill_rect(b, r[0], r[1], r[2], r[3], c);
    return num(0);
}

static Value bd_clone(Act *a, AObj *self, int argc, Value *argv) {
    BitmapData *b = valid(self), *n; Value proto; int has; (void)argc; (void)argv;
    if (!b) return neg(-1);
    has = o_get_local_stored(a, self, a->vm->s_proto, &proto); CK(v_undef());
    b = valid(self); if (!b) return neg(-1);
    n = bmd_clone(b);
    if (!n) return v_undef();
    return v_obj(new_bitmap_data(a, has, proto, n));
}

static Value bd_dispose(Act *a, AObj *self, int argc, Value *argv) {
    BitmapData *b = valid(self); (void)a; (void)argc; (void)argv;
    if (!b) return neg(-1);
    bmd_dispose(b);
    return v_undef();
}

static Value bd_flood_fill(Act *a, AObj *self, int argc, Value *argv) {
    BitmapData *b; u32 x, y, c;
    if (argc < 3 || !valid(self)) return neg(-1);
    x = v_u32(a, argv[0]); CK(v_undef()); y = v_u32(a, argv[1]); CK(v_undef()); c = v_u32(a, argv[2]); CK(v_undef());
    b = valid(self); if (!b) return neg(-1);
    return num(bmd_flood_fill(b, x, y, c) ? 1 : 0);
}

static int clamp_u8(s32 v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static Value bd_noise(Act *a, AObj *self, int argc, Value *argv) {
    BitmapData *b; s32 seed; int low, high = 255, ch = BMD_RED | BMD_GREEN | BMD_BLUE, gray;
    if (argc < 1 || !valid(self)) return neg(-1);
    seed = v_i32(a, argv[0]); CK(v_undef());
    low = clamp_u8(v_i32(a, arg(argc, argv, 1))); CK(v_undef());
    if (argc > 2) { high = clamp_u8(v_i32(a, argv[2])); CK(v_undef()); }
    if (argc > 3) { ch = v_u8(a, argv[3]) & 15; CK(v_undef()); }
    gray = v_bool_of(a, arg(argc, argv, 4));
    b = valid(self); if (!b) return neg(-1);
    bmd_noise(b, seed, (u8)low, (u8)(high > low ? high : low), ch, gray);
    return num(0);
}

/* Value::as_blend_mode: 0 normal, 11 alpha, 12 erase ...; -1 unknown */
static int blend_of(Act *a, Value v) {
    static const char *names[] = { "normal", "", "layer", "multiply", "screen", "lighten", "darken", "difference", "add", "subtract", "invert", "alpha", "erase", "overlay", "hardlight", NULL };
    if (v.t == V_UNDEF || v.t == V_NULL) return 0;
    if (v.t == V_NUM) { u32 n = (u32)f64_u32(v.u.n) & 255u; if (n <= 1) return 0; return n <= 14 ? (int)n : -1; }
    if (v.t == V_STR) { const char *s = str_c(a->vm, v.u.s); int i; for (i = 0; names[i]; i++) if (names[i][0] && !strcmp(s, names[i])) return i; }
    return -1;
}

static Value bd_draw(Act *a, AObj *self, int argc, Value *argv) {
    BitmapData *b = valid(self), *src = NULL; SWFMatrix m; SWFCxform cx; int blend = 0, smoothing = 0, has_clip = 0, st;
    s32 clip[4]; Value sv; AObj *so; DisplayObject *d = NULL;
    if (!b) return neg(-1);
    swf_matrix_identity(&m); swf_cxform_identity(&cx);
    if (argc > 1) {
        static const char *mp[6] = { "a", "b", "c", "d", "tx", "ty" }; double v[6]; int i, ok = 1;
        AObj *mo = v_to_obj_or_bare(a, argv[1]); CK(v_undef());
        for (i = 0; i < 6 && ok; i++) { Value x = o_get(a, mo, S(mp[i])); if (ERR) { ok = 0; break; } v[i] = v_num_of(a, x); if (ERR) ok = 0; }
        if (ERR) return v_undef();
        if (ok) { m.sx = (float)v[0]; m.r0 = (float)v[1]; m.r1 = (float)v[2]; m.sy = (float)v[3]; m.tx = tw_from_px(v[4]); m.ty = tw_from_px(v[5]); }
    }
    if (argc > 2) { double *c = a1_color_transform_of(argv[2]); if (c) a1_cxform_from(c, &cx); }
    if (argc > 3) { int bm = blend_of(a, argv[3]); if (bm >= 0) blend = bm; }
    if (argc > 4) {
        static const char *rp[4] = { "x", "y", "width", "height" }; int i; AObj *ro = v_to_obj_or_bare(a, argv[4]); CK(v_undef());
        has_clip = 1;
        for (i = 0; i < 4; i++) { Value x; if (!o_get_local_stored(a, ro, S(rp[i]), &x)) { has_clip = 0; break; } CK(v_undef()); clip[i] = v_i32(a, x); CK(v_undef()); }
    }
    if (argc > 5) smoothing = v_bool_of(a, argv[5]);
    sv = arg(argc, argv, 0);
    so = v_to_obj_or_bare(a, sv); CK(v_undef());
    st = bd_state(so, &src);
    if (st == 0) return neg(-3);
    if (st < 0) {
        MovieClip *c = o_display(so);
        src = NULL;
        if (!c && sv.t == V_STR) { c = act_resolve_target_display(a, act_target_or_root(a), sv, 0); CK(v_undef()); }
        if (!c || !c->self) return neg(-2);
        d = c->self;
    }
    b = valid(self); if (!b) return neg(-1);
    if (src && (blend == 11 || blend == 12)) return num(0);
    if (src && (blend == 0 || blend == 2) && bmd_draw_bitmap_fast(b, src, &m, &cx, blend, has_clip ? clip : NULL)) return num(0);
    player_bmd_draw_blend(a->vm->player, b, d, src, &m, &cx, has_clip ? clip : NULL, smoothing, blend);
    return num(0);
}

static Value bd_apply_filter(Act *a, AObj *self, int argc, Value *argv) {
    BitmapData *b = valid(self), *s; Value ret; AObj *ro, *po; double sx, sy, sw, sh; s32 dx = 0, dy = 0;
    PxRegion sr, dr;
    if (!b) return neg(-1);
    if (!(s = src_arg(a, argc, argv, 0, &ret))) return ret;
    ro = get_object(a, argc, argv, 1); CK(v_undef());
    sx = v_num_of(a, o_get(a, ro, S("x"))); CK(v_undef());
    sy = v_num_of(a, o_get(a, ro, S("y"))); CK(v_undef());
    sw = v_num_of(a, o_get(a, ro, S("width"))); CK(v_undef());
    sh = v_num_of(a, o_get(a, ro, S("height"))); CK(v_undef());
    po = get_object(a, argc, argv, 2); CK(v_undef());
    get_point(a, po, &dx, &dy); CK(v_undef());
    {
        extern int a1_filter_record(AObj *o, SwfFilter *out); SwfFilter f; AObj *fo = get_object(a, argc, argv, 3); CK(v_undef());
        if (fo->nk != NK_FILTER) return neg(-1);
        b = valid(self); if (!b || s->disposed) return neg(-1);
        {
            u32 ux = d_u32(sx), uy = d_u32(sy), uw = d_u32(sw), uh = d_u32(sh);
            if (!uw || !uh) return num(0);
            memset(&f, 0, sizeof(f));
            if (a1_filter_record(fo, &f)) { bmd_apply_filter(b, s, (s32)ux, (s32)uy, (s32)uw, (s32)uh, dx, dy, &f); free(f.cmatrix); if (f.map_bmd) bmd_unref(f.map_bmd); }
            else {
                sr = reg_whole(s->w, s->h); dr = reg_whole(b->w, b->h);
                reg_clamp_with_intersection(&dr, dx, dy, (s32)ux, (s32)uy, (s32)uw, (s32)uh, &sr);
                if (dr.x1 > dr.x0 && dr.y1 > dr.y0) bmd_copy_region(s, b, sr, dr, 0);
            }
        }
    }
    return num(0);
}

static Value bd_generate_filter_rect(Act *a, AObj *self, int argc, Value *argv) { (void)a; (void)argc; (void)argv; if (!valid(self)) return neg(-1); return v_undef(); }

static Value bd_color_transform(Act *a, AObj *self, int argc, Value *argv) {
    BitmapData *b; AObj *ro; s32 r[4]; double *c; SWFCxform cx;
    if (argc < 2 || !valid(self)) return neg(-1);
    ro = get_object(a, argc, argv, 0); CK(v_undef());
    if (!try_get_rect(a, ro, r)) { CK(v_undef()); return neg(-2); }
    c = a1_color_transform_of(argv[1]);
    if (!c) return neg(-3);
    a1_cxform_from(c, &cx);
    b = valid(self); if (!b) return neg(-1);
    bmd_color_transform(b, r[0] > 0 ? (u32)r[0] : 0, r[1] > 0 ? (u32)r[1] : 0, (u32)(r[0] + r[2]), (u32)(r[1] + r[3]), &cx);
    return neg(-1);
}

static Value bd_get_color_bounds_rect(Act *a, AObj *self, int argc, Value *argv) {
    BitmapData *b; u32 mask, color, out[4]; int find = 1;
    if (argc < 2 || !valid(self)) return neg(-1);
    mask = v_u32(a, argv[0]); CK(v_undef());
    color = v_u32(a, argv[1]); CK(v_undef());
    if (argc > 2) find = v_bool_of(a, argv[2]);
    b = valid(self); if (!b) return neg(-1);
    bmd_color_bounds(b, find, mask, color, out);
    return make_rect(a, out[0], out[1], out[2], out[3]);
}

static Value bd_perlin_noise(Act *a, AObj *self, int argc, Value *argv) {
    BitmapData *b; double bx, by, *offs; s32 oct, seed; int stitch, fractal, ch = BMD_RED | BMD_GREEN | BMD_BLUE, gray, i; AObj *oo;
    if (argc < 6 || !valid(self)) return neg(-1);
    bx = v_num_of(a, argv[0]); CK(v_undef());
    by = v_num_of(a, argv[1]); CK(v_undef());
    oct = v_i32(a, argv[2]); CK(v_undef()); if (oct < 0) oct = 0;
    seed = v_i32(a, argv[3]); CK(v_undef());
    stitch = v_bool_of(a, argv[4]);
    fractal = v_bool_of(a, argv[5]);
    if (argc > 6) { ch = v_u8(a, argv[6]) & 15; CK(v_undef()); }
    gray = v_bool_of(a, arg(argc, argv, 7));
    oo = get_object(a, argc, argv, 8); CK(v_undef());
    offs = (double*)calloc((size_t)oct * 2 + 2, sizeof(double));
    if (!offs) return v_undef();
    for (i = 0; i < oct; i++) {
        Value e = o_get_element(a, oo, i);
        if (ERR) { free(offs); return v_undef(); }
        if (e.t == V_OBJ) {
            offs[i * 2] = v_num_of(a, o_get(a, e.u.o, S("x"))); if (ERR) { free(offs); return v_undef(); }
            offs[i * 2 + 1] = v_num_of(a, o_get(a, e.u.o, S("y"))); if (ERR) { free(offs); return v_undef(); }
        }
    }
    b = valid(self);
    if (b) bmd_perlin_noise(b, bx, by, oct, seed, stitch, fractal, ch, gray, offs);
    free(offs);
    return b ? num(0) : neg(-1);
}

static Value bd_hit_test(Act *a, AObj *self, int argc, Value *argv) {
    BitmapData *b, *o; AObj *fp, *co; Value x, y, w, h; s32 tlx, tly; int th1, hx, hy, hw, hh;
    if (argc < 3 || !valid(self)) return neg(-1);
    fp = get_object(a, argc, argv, 0); CK(v_undef());
    hx = o_get_local_stored(a, fp, S("x"), &x); CK(v_undef());
    hy = o_get_local_stored(a, fp, S("y"), &y); CK(v_undef());
    if (!hx || !hy) return neg(-2);
    tlx = v_i32(a, x); CK(v_undef()); tly = v_i32(a, y); CK(v_undef());
    th1 = clamp_u8(v_i32(a, argv[1])); CK(v_undef());
    co = get_object(a, argc, argv, 2); CK(v_undef());
    if ((o = a1_bitmapdata_of(co)) != NULL) {
        AObj *sp; s32 px = 0, py = 0; int th2;
        if (o->disposed) return neg(-3);
        sp = get_object(a, argc, argv, 3); CK(v_undef());
        hx = o_get_local_stored(a, sp, S("x"), &x); CK(v_undef());
        hy = o_get_local_stored(a, sp, S("y"), &y); CK(v_undef());
        if (hx && hy) { px = v_i32(a, x); CK(v_undef()); py = v_i32(a, y); CK(v_undef()); }
        else if (argc > 3) return neg(-4);
        th2 = clamp_u8(v_i32(a, arg(argc, argv, 4))); CK(v_undef());
        b = valid(self); if (!b || o->disposed) return neg(-1);
        return v_bool(bmd_hit_test_bitmap(b, tlx, tly, (u8)th1, o, px, py, (u8)th2));
    }
    hx = o_get_local_stored(a, co, S("x"), &x); CK(v_undef());
    hy = o_get_local_stored(a, co, S("y"), &y); CK(v_undef());
    hw = o_get_local_stored(a, co, S("width"), &w); CK(v_undef());
    hh = o_get_local_stored(a, co, S("height"), &h); CK(v_undef());
    if (hx && hy && !hw && !hh) {
        s32 tx = v_i32(a, x), ty; CK(v_undef()); ty = v_i32(a, y); CK(v_undef());
        b = valid(self); if (!b) return neg(-1);
        return v_bool(bmd_hit_test_point(b, (u8)th1, (s32)((u32)tx - (u32)tlx), (s32)((u32)ty - (u32)tly)));
    }
    if (hx && hy && hw && hh) {
        s32 tx = v_i32(a, x), ty, tw, tht; CK(v_undef()); ty = v_i32(a, y); CK(v_undef());
        tw = v_i32(a, w); CK(v_undef()); tht = v_i32(a, h); CK(v_undef());
        b = valid(self); if (!b) return neg(-1);
        return v_bool(bmd_hit_test_rect(b, (u8)th1, (s32)((u32)tx - (u32)tlx), (s32)((u32)ty - (u32)tly), tw, tht));
    }
    return neg(-3);
}

static Value bd_copy_pixels(Act *a, AObj *self, int argc, Value *argv) {
    BitmapData *b, *s, *al; Value ret; AObj *ro, *po, *ao; s32 r[4], dx = 0, dy = 0; int merge = -1;
    if (argc < 3 || !valid(self)) return neg(-1);
    if (!(s = src_arg(a, argc, argv, 0, &ret))) return ret;
    ro = get_object(a, argc, argv, 1); CK(v_undef());
    if (!try_get_rect(a, ro, r)) { CK(v_undef()); return neg(-4); }
    po = get_object(a, argc, argv, 2); CK(v_undef());
    get_point(a, po, &dx, &dy); CK(v_undef());
    if (argc >= 6) merge = v_bool_of(a, argv[5]);
    ao = get_object(a, argc, argv, 3); CK(v_undef());
    if ((al = valid(ao)) != NULL) {
        AObj *apo = get_object(a, argc, argv, 4); s32 ax = 0, ay = 0; CK(v_undef());
        get_point(a, apo, &ax, &ay); CK(v_undef());
        b = valid(self); if (!b || s->disposed || al->disposed) return neg(-1);
        bmd_copy_pixels_alpha(b, s, r[0], r[1], r[2], r[3], dx, dy, al, ax, ay, merge < 0 ? 1 : merge);
    } else {
        b = valid(self); if (!b || s->disposed) return neg(-1);
        bmd_copy_pixels(b, s, r[0], r[1], r[2], r[3], dx, dy, merge < 0 ? 0 : merge);
    }
    return num(0);
}

static Value bd_merge(Act *a, AObj *self, int argc, Value *argv) {
    BitmapData *b, *s; Value ret; AObj *ro, *po; s32 r[4], dx = 0, dy = 0, rm, gm, bm, am = 255;
    if (argc < 6 || !valid(self)) return neg(-1);
    if (!(s = src_arg(a, argc, argv, 0, &ret))) return ret;
    ro = get_object(a, argc, argv, 1); CK(v_undef());
    if (!try_get_rect(a, ro, r)) { CK(v_undef()); return neg(-4); }
    po = get_object(a, argc, argv, 2); CK(v_undef());
    get_point(a, po, &dx, &dy); CK(v_undef());
    rm = v_i32(a, argv[3]); CK(v_undef());
    gm = v_i32(a, argv[4]); CK(v_undef());
    bm = v_i32(a, argv[5]); CK(v_undef());
    if (argc > 6) { am = v_i32(a, argv[6]); CK(v_undef()); }
    b = valid(self); if (!b || s->disposed) return neg(-1);
    bmd_merge(b, s, r[0], r[1], r[2], r[3], dx, dy, rm, gm, bm, am);
    return neg(-1);
}

static Value bd_palette_map(Act *a, AObj *self, int argc, Value *argv) {
    BitmapData *b, *s; Value ret; AObj *ro, *po; s32 r[4], dx = 0, dy = 0; u32 *ch; int k, i;
    static const int shifts[4] = { 16, 8, 0, 24 };
    if (argc < 4 || !valid(self)) return neg(-1);
    if (!(s = src_arg(a, argc, argv, 0, &ret))) return ret;
    ro = get_object(a, argc, argv, 1); CK(v_undef());
    if (!try_get_rect(a, ro, r)) { CK(v_undef()); return neg(-4); }
    po = get_object(a, argc, argv, 2); CK(v_undef());
    get_point(a, po, &dx, &dy); CK(v_undef());
    ch = (u32*)malloc(4 * 256 * sizeof(u32));
    if (!ch) return v_undef();
    for (k = 0; k < 4; k++) {
        Value av = 3 + k < argc ? argv[3 + k] : v_null();
        for (i = 0; i < 256; i++) {
            if (av.t == V_OBJ) {
                Value e = o_get_element(a, av.u.o, i);
                if (ERR) { free(ch); return v_undef(); }
                ch[k * 256 + i] = v_u32(a, e) & 0xFFu;
                if (ERR) { free(ch); return v_undef(); }
            } else ch[k * 256 + i] = (u32)i << shifts[k];
        }
    }
    b = valid(self);
    if (b && !s->disposed) bmd_palette_map(b, s, r[0], r[1], r[2], r[3], dx, dy, ch);
    free(ch);
    return neg(-1);
}

static Value bd_pixel_dissolve(Act *a, AObj *self, int argc, Value *argv) {
    BitmapData *b, *s; Value ret; AObj *ro, *po; s32 r[4], dx = 0, dy = 0, seed, n, res; u32 fill = 0; int empty;
    /* (Flash: without numPixels nothing happens and -1 comes back) */
    if (argc < 5 || !valid(self)) return neg(-1);
    if (!(s = src_arg(a, argc, argv, 0, &ret))) return ret;
    ro = get_object(a, argc, argv, 1); CK(v_undef());
    if (!try_get_rect(a, ro, r)) { CK(v_undef()); return neg(-4); }
    po = get_object(a, argc, argv, 2); CK(v_undef());
    get_point(a, po, &dx, &dy); CK(v_undef());
    seed = v_i32(a, argv[3]); CK(v_undef());
    n = v_i32(a, argv[4]); CK(v_undef());
    if (argc > 5) { fill = v_u32(a, argv[5]); CK(v_undef()); }
    b = valid(self); if (!b || s->disposed) return neg(-1);
    res = bmd_pixel_dissolve_ex(b, s, r[0], r[1], r[2], r[3], dx, dy, seed, n, fill, &empty);
    /* Flash hands the seed back when there is no area or no pixel to dissolve */
    return num(empty || n == 0 ? seed : res);
}

static Value bd_scroll(Act *a, AObj *self, int argc, Value *argv) {
    BitmapData *b; s32 x, y;
    if (argc < 2 || !valid(self)) return neg(-1);
    x = v_i32(a, argv[0]); CK(v_undef()); y = v_i32(a, argv[1]); CK(v_undef());
    b = valid(self); if (!b) return neg(-1);
    bmd_scroll(b, x, y);
    return num(0);
}

static Value bd_threshold(Act *a, AObj *self, int argc, Value *argv) {
    BitmapData *b, *s; Value ret; AObj *ro, *po; s32 r[4], dx = 0, dy = 0; int op; u32 th, col, mask = 0xFFFFFFFFu, cnt; int copy; AStr *os;
    if (argc < 5 || !valid(self)) return neg(-1);
    if (!(s = src_arg(a, argc, argv, 0, &ret))) return ret;
    ro = get_object(a, argc, argv, 1); CK(v_undef());
    if (!try_get_rect(a, ro, r)) { CK(v_undef()); return neg(-4); }
    po = get_object(a, argc, argv, 2); CK(v_undef());
    get_point(a, po, &dx, &dy); CK(v_undef());
    os = v_str_of(a, argv[3]); CK(v_undef());
    op = bmd_threshold_op(str_c(a->vm, os)); if (op < 0) op = TH_LT;
    th = v_u32(a, argv[4]); CK(v_undef());
    col = v_u32(a, arg(argc, argv, 5)); CK(v_undef());
    if (argc > 6) { mask = v_u32(a, argv[6]); CK(v_undef()); }
    copy = v_bool_of(a, arg(argc, argv, 7));
    b = valid(self); if (!b || s->disposed) return neg(-1);
    cnt = bmd_threshold(b, s, r[0], r[1], r[2], r[3], dx, dy, op, th, col, mask, copy);
    return num(cnt);
}

static Value bd_compare(Act *a, AObj *self, int argc, Value *argv) {
    BitmapData *b, *o, *res; AObj *oo; Value proto; int has;
    if (argc < 1 || !valid(self)) return neg(-1);
    oo = get_object(a, argc, argv, 0); CK(v_undef());
    if (!(o = valid(oo))) return neg(-2);
    b = valid(self); if (!b) return neg(-1);
    if (b->w != o->w) return neg(-3);
    if (b->h != o->h) return neg(-4);
    res = bmd_compare(b, o);
    if (!res) return num(0);
    has = o_get_local_stored(a, self, a->vm->s_proto, &proto);
    if (ERR) { bmd_unref(res); return v_undef(); }
    return v_obj(new_bitmap_data(a, has, proto, res));
}

static int export_id(VM *vm, SWFMovie *m, AStr *name) {
    int i;
    if (!m) return -1;
    for (i = 0; i < m->nexports; i++) {
        AStr *e = str_from_swf(vm, m->exports[i].name, strlen(m->exports[i].name), m->version);
        if (str_eq(e, name)) return m->exports[i].id;
    }
    return -1;
}

static Value bd_load_bitmap(Act *a, AObj *self, int argc, Value *argv) {
    AStr *name = v_str_of(a, arg(argc, argv, 0)); MovieClip *t; SWFMovie *m; int id; SWFChar *ch; Bitmap *bm; BitmapData *b; Value proto;
    CK(v_undef());
    t = act_target_or_root(a);
    m = t ? t->movie : NULL;
    if (!m) m = a->vm->player->movie;
    id = export_id(a->vm, m, name);
    ch = id >= 0 ? swf_get_char(m, id) : NULL;
    if (!ch || ch->type != CHAR_BITMAP) return v_undef();
    bm = image_get(m, id);
    if (!bm) return v_undef();
    b = bmd_from_straight(bm, 1);
    if (!b) return v_undef();
    {
        int has = o_get_local_stored(a, self, str_intern(a->vm, "prototype"), &proto);
        if (ERR) { bmd_unref(b); return v_undef(); }
        return v_obj(new_bitmap_data(a, has, proto, b));
    }
}

AObj *lib_bitmapdata(VM *vm, Env *env, AObj *object_proto) {
    AObj *proto, *c = lib_class(vm, env, bd_ctor, NULL, object_proto, 1, &proto);
    env->p.bitmapdata = proto;
    defp(vm, env, proto, "width", bd_width, NULL, 0);
    defp(vm, env, proto, "height", bd_height, NULL, 0);
    defp(vm, env, proto, "rectangle", bd_rectangle, NULL, 0);
    defp(vm, env, proto, "transparent", bd_transparent, NULL, 0);
    defm(vm, env, proto, "getPixel", bd_get_pixel, 0);
    defm(vm, env, proto, "setPixel", bd_set_pixel, 0);
    defm(vm, env, proto, "fillRect", bd_fill_rect, 0);
    defm(vm, env, proto, "copyPixels", bd_copy_pixels, 0);
    defm(vm, env, proto, "applyFilter", bd_apply_filter, 0);
    defm(vm, env, proto, "scroll", bd_scroll, 0);
    defm(vm, env, proto, "threshold", bd_threshold, 0);
    defm(vm, env, proto, "draw", bd_draw, 0);
    defm(vm, env, proto, "pixelDissolve", bd_pixel_dissolve, 0);
    defm(vm, env, proto, "getPixel32", bd_get_pixel32, 0);
    defm(vm, env, proto, "setPixel32", bd_set_pixel32, 0);
    defm(vm, env, proto, "floodFill", bd_flood_fill, 0);
    defm(vm, env, proto, "getColorBoundsRect", bd_get_color_bounds_rect, 0);
    defm(vm, env, proto, "perlinNoise", bd_perlin_noise, 0);
    defm(vm, env, proto, "colorTransform", bd_color_transform, 0);
    defm(vm, env, proto, "hitTest", bd_hit_test, 0);
    defm(vm, env, proto, "paletteMap", bd_palette_map, 0);
    defm(vm, env, proto, "merge", bd_merge, 0);
    defm(vm, env, proto, "noise", bd_noise, 0);
    defm(vm, env, proto, "copyChannel", bd_copy_channel, 0);
    defm(vm, env, proto, "clone", bd_clone, 0);
    defm(vm, env, proto, "dispose", bd_dispose, 0);
    defm(vm, env, proto, "generateFilterRect", bd_generate_filter_rect, 0);
    defm(vm, env, proto, "compare", bd_compare, 0);
    o_define_c(vm, c, "RED_CHANNEL", v_num(1), 0);
    o_define_c(vm, c, "GREEN_CHANNEL", v_num(2), 0);
    o_define_c(vm, c, "BLUE_CHANNEL", v_num(4), 0);
    o_define_c(vm, c, "ALPHA_CHANNEL", v_num(8), 0);
    defm(vm, env, c, "loadBitmap", bd_load_bitmap, 0);
    return c;
}
