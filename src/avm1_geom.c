/* avm1_geom.c - flash.geom.ColorTransform and flash.geom.Transform.
 *
 * (Point, Rectangle and Matrix are ActionScript classes of the player
 * globals.)  A Transform refers to its clip by path, like any AVM1 clip
 * reference, so it follows a clip that is removed and placed again. */
#include "avm1_int.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define DE  PA_DONT_ENUM
#define DD  PA_DONT_DELETE
#define RO  PA_READ_ONLY
#define V8  PA_VERSION_8

extern AObj *lib_class(VM *vm, Env *env, NativeFn ctor, NativeFn call, AObj *super_proto, int proto_first, AObj **proto_out);
extern void dg_local_bounds(MovieClip *c, SWFRect *out, int *any);
extern void dg_union(SWFRect *acc, int *any, const SWFRect *r, const SWFMatrix *m);
extern void clip_world_matrix(MovieClip *c, SWFMatrix *out);

static void defm(VM *vm, Env *env, AObj *o, const char *name, NativeFn f, u16 attr) { o_define(vm, o, str_intern(vm, name), v_obj(fn_native(vm, env, f, NULL)), attr); }
static void defp(VM *vm, Env *env, AObj *o, const char *name, NativeFn g, NativeFn s, u16 attr) { o_add_property(vm, o, str_intern(vm, name), fn_native(vm, env, g, NULL), s ? fn_native(vm, env, s, NULL) : NULL, attr); }

/* "new flash.geom.X(args)" through the current globals */
Value a1_new_geom(Act *a, const char *cls, int argc, Value *argv) {
    VM *vm = a->vm; AObj *o = act_global(a); Value v; const char *path[3]; int i;
    path[0] = "flash"; path[1] = "geom"; path[2] = cls;
    for (i = 0; i < 3; i++) {
        v = o_get(a, o, str_intern(vm, path[i]));
        if (vm->err) return v_undef();
        o = v_to_obj_or_bare(a, v);
        if (vm->err || !o) return v_undef();
    }
    return o_construct(a, o, argc, argv);
}

/* ---------------------------------------------------------------- */
/* ColorTransform                                                   */

enum { CT_RM = 0, CT_GM, CT_BM, CT_AM, CT_RO, CT_GO, CT_BO, CT_AO };

static void ct_free(AObj *o) { free(o->nat.p); o->nat.p = NULL; }

double *a1_color_transform_of(Value v) {
    AObj *o = v.t == V_OBJ ? v.u.o : NULL;
    if (o && o->nk == NK_SUPER) o = o->nat.sup.self;
    return o && o->nk == NK_COLORTRANSFORM ? (double*)o->nat.p : NULL;
}
static double *ct_of(AObj *o) { return a1_color_transform_of(v_obj(o)); }

static Value ct_ctor(Act *a, AObj *self, int argc, Value *argv) {
    double v[8] = { 1, 1, 1, 1, 0, 0, 0, 0 }, *p; AObj *t; int i;
    if (argc >= 8) {
        for (i = 0; i < 8; i++) { v[i] = v_num_of(a, argv[i]); if (a->vm->err) return v_undef(); }
    } else if (argc == 1) {
        double *src = a1_color_transform_of(argv[0]);
        if (src) memcpy(v, src, sizeof(v));
    }
    t = o_native_target(self);
    if (t) {
        p = (double*)malloc(sizeof(v));
        if (!p) return v_undef();
        memcpy(p, v, sizeof(v));
        t->nk = NK_COLORTRANSFORM; t->nat.p = p; t->nfree = ct_free;
    }
    return v_obj(self);
}

#define CT_ACCESSOR(name, idx) \
    static Value ct_get_##name(Act *a, AObj *self, int argc, Value *argv) { double *c = ct_of(self); (void)a; (void)argc; (void)argv; return c ? v_num(c[idx]) : v_undef(); } \
    static Value ct_set_##name(Act *a, AObj *self, int argc, Value *argv) { double *c = ct_of(self); if (c && argc > 0) { double d = v_num_of(a, argv[0]); if (a->vm->err) return v_undef(); c = ct_of(self); if (c) c[idx] = d; } return v_undef(); }
CT_ACCESSOR(rm, CT_RM)
CT_ACCESSOR(gm, CT_GM)
CT_ACCESSOR(bm, CT_BM)
CT_ACCESSOR(am, CT_AM)
CT_ACCESSOR(ro, CT_RO)
CT_ACCESSOR(go, CT_GO)
CT_ACCESSOR(bo, CT_BO)
CT_ACCESSOR(ao, CT_AO)

static Value ct_get_rgb(Act *a, AObj *self, int argc, Value *argv) {
    double *c = ct_of(self); u32 r, g, b; (void)a; (void)argc; (void)argv;
    if (!c) return v_undef();
    r = f64_u32(c[CT_RO]); g = f64_u32(c[CT_GO]); b = f64_u32(c[CT_BO]);
    return v_num((double)(u32)((r << 16) + (g << 8) + b));
}
static Value ct_set_rgb(Act *a, AObj *self, int argc, Value *argv) {
    double *c = ct_of(self); u32 rgb;
    if (!c || argc < 1) return v_undef();
    rgb = v_u32(a, argv[0]); if (a->vm->err) return v_undef();
    c = ct_of(self); if (!c) return v_undef();
    c[CT_RM] = c[CT_GM] = c[CT_BM] = 0;
    c[CT_RO] = (rgb >> 16) & 255; c[CT_GO] = (rgb >> 8) & 255; c[CT_BO] = rgb & 255;
    return v_undef();
}

static Value ct_to_string(Act *a, AObj *self, int argc, Value *argv) {
    static const char *names[] = { "redMultiplier", "greenMultiplier", "blueMultiplier", "alphaMultiplier", "redOffset", "greenOffset", "blueOffset", "alphaOffset" };
    SBuf b; int i; (void)argc; (void)argv;
    sb_init(&b);
    sb_unit(&b, '(');
    for (i = 0; i < 8; i++) {
        Value v = o_get(a, self, str_intern(a->vm, names[i])); AStr *s;
        if (a->vm->err) { sb_free(&b); return v_undef(); }
        s = v_str_of(a, v);
        if (a->vm->err) { sb_free(&b); return v_undef(); }
        if (i) sb_ascii(&b, ", ");
        sb_ascii(&b, names[i]); sb_unit(&b, '='); sb_str(&b, s);
    }
    sb_unit(&b, ')');
    return v_str(sb_done(a->vm, &b));
}

static Value ct_concat(Act *a, AObj *self, int argc, Value *argv) {
    double *t = ct_of(self), *o; int i; (void)a;
    if (!t || argc < 1) return v_undef();
    o = a1_color_transform_of(argv[0]);
    if (!o) return v_undef();
    for (i = 0; i < 4; i++) t[CT_RO + i] = o[CT_RO + i] * t[CT_RM + i] + t[CT_RO + i];
    for (i = 0; i < 4; i++) t[CT_RM + i] = o[CT_RM + i] * t[CT_RM + i];
    return v_undef();
}

AObj *lib_colortransform(VM *vm, Env *env, AObj *object_proto) {
    AObj *proto, *c = lib_class(vm, env, ct_ctor, NULL, object_proto, 1, &proto);
    env->p.colortransform = proto;
    defp(vm, env, proto, "alphaMultiplier", ct_get_am, ct_set_am, 0);
    defp(vm, env, proto, "redMultiplier", ct_get_rm, ct_set_rm, 0);
    defp(vm, env, proto, "greenMultiplier", ct_get_gm, ct_set_gm, 0);
    defp(vm, env, proto, "blueMultiplier", ct_get_bm, ct_set_bm, 0);
    defp(vm, env, proto, "alphaOffset", ct_get_ao, ct_set_ao, 0);
    defp(vm, env, proto, "redOffset", ct_get_ro, ct_set_ro, 0);
    defp(vm, env, proto, "greenOffset", ct_get_go, ct_set_go, 0);
    defp(vm, env, proto, "blueOffset", ct_get_bo, ct_set_bo, 0);
    defp(vm, env, proto, "rgb", ct_get_rgb, ct_set_rgb, 0);
    defm(vm, env, proto, "concat", ct_concat, 0);
    defm(vm, env, proto, "toString", ct_to_string, 0);
    return c;
}

/* a display object's color transform as a script ColorTransform */
Value a1_cxform_value(Act *a, const SWFCxform *cx) {
    Value args[8];
    args[0] = v_num(cx->mr / 256.0); args[1] = v_num(cx->mg / 256.0); args[2] = v_num(cx->mb / 256.0); args[3] = v_num(cx->ma / 256.0);
    args[4] = v_num(cx->ar); args[5] = v_num(cx->ag); args[6] = v_num(cx->ab); args[7] = v_num(cx->aa);
    return a1_new_geom(a, "ColorTransform", 8, args);
}

static s16 sat16(double d) { if (d != d) return 0; if (d >= 32767.0) return 32767; if (d <= -32768.0) return -32768; return (s16)d; }

void a1_cxform_from(const double *c, SWFCxform *cx) {
    cx->mr = sat16(c[CT_RM] * 256.0); cx->mg = sat16(c[CT_GM] * 256.0); cx->mb = sat16(c[CT_BM] * 256.0); cx->ma = sat16(c[CT_AM] * 256.0);
    cx->ar = sat16(c[CT_RO]); cx->ag = sat16(c[CT_GO]); cx->ab = sat16(c[CT_BO]); cx->aa = sat16(c[CT_AO]);
}

/* ---------------------------------------------------------------- */
/* Transform                                                        */

static void tr_mark(VM *vm, AObj *o) { ClipRef *r = (ClipRef*)o->nat.p; if (r) gc_mark_hdr(vm, &r->gc); }

static ClipRef *tr_ref(AObj *o) {
    if (o && o->nk == NK_SUPER) o = o->nat.sup.self;
    return o && o->nk == NK_TRANSFORM ? (ClipRef*)o->nat.p : NULL;
}

/* the clip of a Transform (NULL when gone) */
MovieClip *a1_transform_clip(Act *a, AObj *o) {
    ClipRef *r = tr_ref(o); MovieClip *d = NULL; int cached;
    if (!r) return NULL;
    if (!clipref_resolve(a, r, &cached, &d)) return NULL;
    return d;
}

static Value tr_ctor(Act *a, AObj *self, int argc, Value *argv) {
    ClipRef *r = NULL; AObj *t;
    if (argc != 1) return v_undef();
    if (argv[0].t == V_CLIP) r = argv[0].u.c;
    else if (argv[0].t == V_OBJ) { Value cv = clipref_value(a, argv[0].u.o); if (cv.t == V_CLIP) r = cv.u.c; }
    else return v_undef();
    t = o_native_target(self);
    if (t) { t->nk = NK_TRANSFORM; t->nat.p = r; t->nmark = tr_mark; t->nfree = NULL; }
    return v_obj(self);
}

static Value matrix_value(Act *a, double ma, double mb, double mc, double md, double tx, double ty) {
    Value args[6];
    args[0] = v_num((float)ma); args[1] = v_num((float)mb); args[2] = v_num((float)mc); args[3] = v_num((float)md);
    args[4] = v_num(tx / 20.0); args[5] = v_num(ty / 20.0);
    return a1_new_geom(a, "Matrix", 6, args);
}

static Value tr_get_matrix(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = a1_transform_clip(a, self); SWFMatrix *m; (void)argc; (void)argv;
    if (!c || !c->self) return v_undef();
    m = &c->self->matrix;
    return matrix_value(a, m->sx, m->r0, m->r1, m->sy, floor(m->tx + 0.5), floor(m->ty + 0.5));
}

static Value tr_set_matrix(Act *a, AObj *self, int argc, Value *argv) {
    static const char *props[] = { "a", "b", "c", "d", "tx", "ty" };
    MovieClip *c = a1_transform_clip(a, self); AObj *o; double v[6]; int i;
    if (!c || !c->self) return v_undef();
    if (argc != 1) return v_undef();
    o = v_to_obj_or_bare(a, argv[0]); if (a->vm->err || !o) return v_undef();
    for (i = 0; i < 6; i++) { int has = o_has_own_property(a, o, str_intern(a->vm, props[i])); if (a->vm->err) return v_undef(); if (!has) return v_undef(); }
    for (i = 0; i < 6; i++) { Value x = o_get(a, o, str_intern(a->vm, props[i])); if (a->vm->err) return v_undef(); v[i] = v_num_of(a, x); if (a->vm->err) return v_undef(); }
    c = a1_transform_clip(a, self);
    if (!c || !c->self) return v_undef();
    c->self->matrix.sx = (float)v[0]; c->self->matrix.r0 = (float)v[1]; c->self->matrix.r1 = (float)v[2]; c->self->matrix.sy = (float)v[3];
    c->self->matrix.tx = tw_from_px(v[4]); c->self->matrix.ty = tw_from_px(v[5]);
    c->self->sr_cached = 0; c->self->script_moved = 1;
    return v_undef();
}

static Value tr_get_concat_matrix(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = a1_transform_clip(a, self); RMat m; (void)argc; (void)argv;
    if (!c) return v_undef();
    rm_local_to_global(c, &m);
    return matrix_value(a, m.a, m.b, m.c, m.d, m.tx, m.ty);
}

static Value tr_get_color(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = a1_transform_clip(a, self); (void)argc; (void)argv;
    if (!c || !c->self) return v_undef();
    return a1_cxform_value(a, &c->self->cxform);
}

static Value tr_set_color(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = a1_transform_clip(a, self); double *ct;
    if (!c || !c->self || argc != 1) return v_undef();
    ct = a1_color_transform_of(argv[0]);
    if (ct) { a1_cxform_from(ct, &c->self->cxform); c->self->script_moved = 1; }
    return v_undef();
}

/* swf ColorTransform multiplication (8.8 fixed point, wrapping) */
static void cx_mul(const SWFCxform *x, const SWFCxform *y, SWFCxform *o) {
    SWFCxform r;
    r.mr = (s16)(((s32)x->mr * y->mr) >> 8); r.mg = (s16)(((s32)x->mg * y->mg) >> 8);
    r.mb = (s16)(((s32)x->mb * y->mb) >> 8); r.ma = (s16)(((s32)x->ma * y->ma) >> 8);
    r.ar = (s16)(x->ar + (s16)(((s32)x->mr * y->ar) >> 8)); r.ag = (s16)(x->ag + (s16)(((s32)x->mg * y->ag) >> 8));
    r.ab = (s16)(x->ab + (s16)(((s32)x->mb * y->ab) >> 8)); r.aa = (s16)(x->aa + (s16)(((s32)x->ma * y->aa) >> 8));
    *o = r;
}

static Value tr_get_concat_color(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = a1_transform_clip(a, self), *n; SWFCxform cx; (void)argc; (void)argv;
    if (!c || !c->self) return v_undef();
    cx = c->self->cxform;
    for (n = clip_avm1_parent(c); n && n->self; n = n->parent) cx_mul(&n->self->cxform, &cx, &cx);
    return a1_cxform_value(a, &cx);
}

static Value tr_get_pixel_bounds(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = a1_transform_clip(a, self); SWFRect l, w; int any = 0, any2 = 0; SWFMatrix m; Value args[4]; (void)argc; (void)argv;
    if (!c) return v_undef();
    l.xmin = l.ymin = l.xmax = l.ymax = 0;
    dg_local_bounds(c, &l, &any);
    w.xmin = w.ymin = w.xmax = w.ymax = 0;
    if (any) { clip_world_matrix(c, &m); dg_union(&w, &any2, &l, &m); }
    args[0] = v_num(w.xmin / 20.0); args[1] = v_num(w.ymin / 20.0);
    args[2] = v_num((w.xmax - w.xmin) / 20.0); args[3] = v_num((w.ymax - w.ymin) / 20.0);
    return a1_new_geom(a, "Rectangle", 4, args);
}

AObj *lib_transform(VM *vm, Env *env, AObj *object_proto) {
    AObj *proto, *c = lib_class(vm, env, tr_ctor, NULL, object_proto, 1, &proto);
    env->p.transform = proto;
    defp(vm, env, proto, "matrix", tr_get_matrix, tr_set_matrix, V8);
    defp(vm, env, proto, "concatenatedMatrix", tr_get_concat_matrix, NULL, V8);
    defp(vm, env, proto, "colorTransform", tr_get_color, tr_set_color, V8);
    defp(vm, env, proto, "concatenatedColorTransform", tr_get_concat_color, NULL, V8);
    defp(vm, env, proto, "pixelBounds", tr_get_pixel_bounds, NULL, V8);
    return c;
}

/* MovieClip.transform */
Value a1_make_transform(Act *a, AObj *clip) {
    Value arg = clipref_value(a, clip);
    return a1_new_geom(a, "Transform", 1, &arg);
}

void a1_apply_transform(Act *a, MovieClip *c, Value t) {
    MovieClip *src;
    if (t.t != V_OBJ || !tr_ref(t.u.o) || !c->self) return;
    src = a1_transform_clip(a, t.u.o);
    if (!src || !src->self) return;
    c->self->matrix = src->self->matrix; c->self->sr_cached = 0;
    c->self->cxform = src->self->cxform;
    c->self->script_moved = 1;
}
