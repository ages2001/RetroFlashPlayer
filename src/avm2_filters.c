/* avm2_filters.c - flash.filters.* and DisplayObject.filters (AVM2).
 *
 * The filter classes are plain classes with typed public fields, like the
 * ones in Flash's playerglobal (and Ruffle's).  Assigning DisplayObject.filters
 * converts every filter to its SWF record (filters.h), with Flash's
 * fixed-point rounding and clamping; reading it back builds fresh objects
 * from those records. */
#include "avm2_flash_int.h"
#include "bmd.h"
#include "filters.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static A2Class *c_bitmapfilter, *c_bevel, *c_blur, *c_colormatrix, *c_convolution, *c_displacement,
               *c_dropshadow, *c_glow, *c_gradientbevel, *c_gradientglow, *c_shaderfilter;

#define OK_ (!vm->throwing)

static void sset(VM2 *vm, V2 self, const char *name, V2 v) { if (OK_) a2_set_s(vm, self, name, v); }
static V2 sget(VM2 *vm, V2 self, const char *name) { return a2_get_s(vm, self, name); }

/* typed constructor arguments with defaults */
static V2 arg_num(VM2 *vm, int argc, V2 *argv, int i, double def) { return i < argc ? v2_num(v2_tonum(vm, argv[i])) : v2_num(def); }
static V2 arg_int(VM2 *vm, int argc, V2 *argv, int i, int def) { return i < argc ? v2_int(v2_toint(vm, argv[i])) : v2_int(def); }
static V2 arg_uint(VM2 *vm, int argc, V2 *argv, int i, u32 def) { return i < argc ? v2_uint(v2_touint(vm, argv[i])) : v2_uint(def); }
static V2 arg_bool(VM2 *vm, int argc, V2 *argv, int i, int def) { return i < argc ? v2_bool(v2_tobool(vm, argv[i])) : v2_bool(def); }
static V2 arg_str(VM2 *vm, int argc, V2 *argv, int i, const char *def) {
    if (i >= argc) return v2_cstr(vm, def);
    if (argv[i].t == T_NULL || argv[i].t == T_UNDEF) return v2_null();
    return v2_str(v2_tostr(vm, argv[i]));
}
static V2 arg_obj(VM2 *vm, int argc, V2 *argv, int i, A2Class *type) { return i < argc ? a2_coerce(vm, argv[i], type) : v2_null(); }

/* ---------------------------------------------------------------- */
/* classes                                                          */

static int known_filter(A2Class *c) {
    A2Class *k[10]; int i;
    k[0] = c_bevel; k[1] = c_blur; k[2] = c_colormatrix; k[3] = c_convolution; k[4] = c_displacement;
    k[5] = c_dropshadow; k[6] = c_glow; k[7] = c_gradientbevel; k[8] = c_gradientglow; k[9] = c_shaderfilter;
    for (i = 0; i < 10; i++) if (k[i] && c && a2_is_subclass(c, k[i])) return 1;
    return 0;
}
static V2 BitmapFilter_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = SELFO; (void)argc; (void)argv;
    if (o && !known_filter(o->cls)) {
        char b[300]; sprintf(b, "%.200s$ class cannot be instantiated.", o->cls ? a2_class_name(vm, o->cls, 0)->s : "BitmapFilter");
        a2_throw_error(vm, "ArgumentError", b, 2012);
    }
    return v2_undef();
}
static V2 BitmapFilter_clone(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_null(); }

static V2 BlurFilter_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    sset(vm, self, "blurX", arg_num(vm, argc, argv, 0, 4.0)); sset(vm, self, "blurY", arg_num(vm, argc, argv, 1, 4.0));
    sset(vm, self, "quality", arg_int(vm, argc, argv, 2, 1));
    return v2_undef();
}
static V2 GlowFilter_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 a[8]; int i;
    a[0] = arg_uint(vm, argc, argv, 0, 0xFF0000); a[1] = arg_num(vm, argc, argv, 1, 1.0); a[2] = arg_num(vm, argc, argv, 2, 6.0); a[3] = arg_num(vm, argc, argv, 3, 6.0);
    a[4] = arg_num(vm, argc, argv, 4, 2.0); a[5] = arg_int(vm, argc, argv, 5, 1); a[6] = arg_bool(vm, argc, argv, 6, 0); a[7] = arg_bool(vm, argc, argv, 7, 0);
    if (!OK_) return v2_undef();
    { static const char *n[8] = { "color", "alpha", "blurX", "blurY", "strength", "quality", "inner", "knockout" }; for (i = 0; i < 8; i++) sset(vm, self, n[i], a[i]); }
    return v2_undef();
}
static V2 DropShadowFilter_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 a[11]; int i;
    static const char *n[11] = { "distance", "angle", "color", "alpha", "blurX", "blurY", "strength", "quality", "inner", "knockout", "hideObject" };
    a[0] = arg_num(vm, argc, argv, 0, 4.0); a[1] = arg_num(vm, argc, argv, 1, 45); a[2] = arg_uint(vm, argc, argv, 2, 0); a[3] = arg_num(vm, argc, argv, 3, 1.0);
    a[4] = arg_num(vm, argc, argv, 4, 4.0); a[5] = arg_num(vm, argc, argv, 5, 4.0); a[6] = arg_num(vm, argc, argv, 6, 1.0); a[7] = arg_int(vm, argc, argv, 7, 1);
    a[8] = arg_bool(vm, argc, argv, 8, 0); a[9] = arg_bool(vm, argc, argv, 9, 0); a[10] = arg_bool(vm, argc, argv, 10, 0);
    if (!OK_) return v2_undef();
    for (i = 0; i < 11; i++) sset(vm, self, n[i], a[i]);
    return v2_undef();
}
static V2 BevelFilter_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 a[12]; int i;
    static const char *n[12] = { "distance", "angle", "highlightColor", "highlightAlpha", "shadowColor", "shadowAlpha", "blurX", "blurY", "strength", "quality", "type", "knockout" };
    a[0] = arg_num(vm, argc, argv, 0, 4.0); a[1] = arg_num(vm, argc, argv, 1, 45); a[2] = arg_uint(vm, argc, argv, 2, 0xFFFFFF); a[3] = arg_num(vm, argc, argv, 3, 1.0);
    a[4] = arg_uint(vm, argc, argv, 4, 0); a[5] = arg_num(vm, argc, argv, 5, 1.0); a[6] = arg_num(vm, argc, argv, 6, 4.0); a[7] = arg_num(vm, argc, argv, 7, 4.0);
    a[8] = arg_num(vm, argc, argv, 8, 1); a[9] = arg_int(vm, argc, argv, 9, 1); a[10] = arg_str(vm, argc, argv, 10, "inner"); a[11] = arg_bool(vm, argc, argv, 11, 0);
    if (!OK_) return v2_undef();
    for (i = 0; i < 12; i++) sset(vm, self, n[i], a[i]);
    return v2_undef();
}
static V2 Gradient_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 a[11]; int i;
    static const char *n[11] = { "distance", "angle", "colors", "alphas", "ratios", "blurX", "blurY", "strength", "quality", "type", "knockout" };
    a[0] = arg_num(vm, argc, argv, 0, 4.0); a[1] = arg_num(vm, argc, argv, 1, 45); a[2] = arg_obj(vm, argc, argv, 2, vm->c_array); a[3] = arg_obj(vm, argc, argv, 3, vm->c_array);
    a[4] = arg_obj(vm, argc, argv, 4, vm->c_array); a[5] = arg_num(vm, argc, argv, 5, 4.0); a[6] = arg_num(vm, argc, argv, 6, 4.0); a[7] = arg_num(vm, argc, argv, 7, 1);
    a[8] = arg_int(vm, argc, argv, 8, 1); a[9] = arg_str(vm, argc, argv, 9, "inner"); a[10] = arg_bool(vm, argc, argv, 10, 0);
    if (!OK_) return v2_undef();
    for (i = 0; i < 11; i++) sset(vm, self, n[i], a[i]);
    return v2_undef();
}
/* GradientGlow/BevelFilter keep their fields private behind accessors */
#define GACC(field, setcheck) \
    static V2 g_get_##field(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return sget(vm, self, "_" #field); } \
    static V2 g_set_##field(VM2 *vm, V2 self, int argc, V2 *argv) { V2 v = ARG(0); setcheck; sset(vm, self, "_" #field, v); return v2_undef(); }
GACC(alphas, (void)0) GACC(angle, (void)0) GACC(blurX, (void)0) GACC(blurY, (void)0) GACC(colors, (void)0) GACC(distance, (void)0)
GACC(knockout, (void)0) GACC(quality, (void)0) GACC(ratios, (void)0) GACC(strength, (void)0)
GACC(type, if (v.t == T_NULL || v.t == T_UNDEF) { a2_throw_error(vm, "TypeError", "Parameter type must be non-null.", 2007); return v2_undef(); })

static V2 ColorMatrixFilter_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 m = arg_obj(vm, argc, argv, 0, vm->c_array);
    if (!OK_) return v2_undef();
    if (m.t != T_OBJ) {
        static const double id[20] = { 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0 }; int i;
        A2Obj *arr = a2_new_array(vm);
        for (i = 0; i < 20; i++) a2_arr_push(vm, arr, v2_num(id[i]));
        m = v2_obj(arr);
    }
    a2_set_s(vm, self, "matrix", m);
    return v2_undef();
}
static V2 array_concat(VM2 *vm, V2 v) {
    if (v.t != T_OBJ) { a2_throw_error(vm, "TypeError", "Cannot access a property or method of a null object reference.", 1009); return v2_undef(); }
    return a2_call_s(vm, v, "concat", 0, NULL);
}
static V2 cm_get_matrix(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return array_concat(vm, sget(vm, self, "_matrix")); }
static V2 cm_set_matrix(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 m = a2_coerce(vm, ARG(0), vm->c_array), c;
    if (!OK_) return v2_undef();
    c = array_concat(vm, m);
    if (OK_) sset(vm, self, "_matrix", c);
    return v2_undef();
}

static V2 ConvolutionFilter_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 a[9]; int i;
    static const char *n[9] = { "matrixX", "matrixY", "matrix", "divisor", "bias", "preserveAlpha", "clamp", "color", "alpha" };
    a[0] = arg_num(vm, argc, argv, 0, 0); a[1] = arg_num(vm, argc, argv, 1, 0); a[2] = arg_obj(vm, argc, argv, 2, vm->c_array); a[3] = arg_num(vm, argc, argv, 3, 1.0);
    a[4] = arg_num(vm, argc, argv, 4, 0.0); a[5] = arg_bool(vm, argc, argv, 5, 1); a[6] = arg_bool(vm, argc, argv, 6, 1); a[7] = arg_uint(vm, argc, argv, 7, 0); a[8] = arg_num(vm, argc, argv, 8, 0.0);
    if (!OK_) return v2_undef();
    for (i = 0; i < 9; i++) sset(vm, self, n[i], a[i]);
    return v2_undef();
}
static V2 DisplacementMapFilter_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 a[9]; int i;
    static const char *n[9] = { "mapBitmap", "mapPoint", "componentX", "componentY", "scaleX", "scaleY", "mode", "color", "alpha" };
    a[0] = arg_obj(vm, argc, argv, 0, vm->c_bitmapdata); a[1] = arg_obj(vm, argc, argv, 1, vm->c_point); a[2] = arg_uint(vm, argc, argv, 2, 0); a[3] = arg_uint(vm, argc, argv, 3, 0);
    a[4] = arg_num(vm, argc, argv, 4, 0.0); a[5] = arg_num(vm, argc, argv, 5, 0.0); a[6] = arg_str(vm, argc, argv, 6, "wrap"); a[7] = arg_uint(vm, argc, argv, 7, 0); a[8] = arg_num(vm, argc, argv, 8, 0.0);
    if (!OK_) return v2_undef();
    for (i = 0; i < 9; i++) sset(vm, self, n[i], a[i]);
    return v2_undef();
}
static V2 ShaderFilter_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { sset(vm, self, "_shader", argc > 0 ? argv[0] : v2_null()); return v2_undef(); }
#define SACC(field) \
    static V2 s_get_##field(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return sget(vm, self, "_" #field); } \
    static V2 s_set_##field(VM2 *vm, V2 self, int argc, V2 *argv) { sset(vm, self, "_" #field, ARG(0)); return v2_undef(); }
SACC(bottomExtension) SACC(leftExtension) SACC(rightExtension) SACC(topExtension) SACC(shader)

/* clone: new X(<fields in constructor order>) */
static const char *const ctor_fields[][12] = {
    { "blurX", "blurY", "quality", NULL },
    { "color", "alpha", "blurX", "blurY", "strength", "quality", "inner", "knockout", NULL },
    { "distance", "angle", "color", "alpha", "blurX", "blurY", "strength", "quality", "inner", "knockout", "hideObject", NULL },
    { "distance", "angle", "highlightColor", "highlightAlpha", "shadowColor", "shadowAlpha", "blurX", "blurY", "strength", "quality", "type", "knockout" },
    { "distance", "angle", "colors", "alphas", "ratios", "blurX", "blurY", "strength", "quality", "type", "knockout", NULL },
    { "matrixX", "matrixY", "matrix", "divisor", "bias", "preserveAlpha", "clamp", "color", "alpha", NULL },
    { "matrix", NULL },
};
static V2 clone_by(VM2 *vm, V2 self, A2Class *c, int which) {
    V2 a[12]; int n;
    for (n = 0; n < 12 && ctor_fields[which][n]; n++) { a[n] = sget(vm, self, ctor_fields[which][n]); if (!OK_) return v2_undef(); }
    return a2_construct_class(vm, c, n, a);
}
static V2 blur_clone(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return clone_by(vm, self, c_blur, 0); }
static V2 glow_clone(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return clone_by(vm, self, c_glow, 1); }
static V2 ds_clone(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return clone_by(vm, self, c_dropshadow, 2); }
static V2 bevel_clone(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return clone_by(vm, self, c_bevel, 3); }
static V2 gglow_clone(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return clone_by(vm, self, c_gradientglow, 4); }
static V2 gbevel_clone(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return clone_by(vm, self, c_gradientbevel, 4); }
static V2 conv_clone(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return clone_by(vm, self, c_convolution, 5); }
static V2 cm_clone(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return clone_by(vm, self, c_colormatrix, 6); }
static V2 dm_clone(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 a[9]; int i; static const char *n[9] = { "mapBitmap", "mapPoint", "componentX", "componentY", "scaleX", "scaleY", "mode", "color", "alpha" };
    UNUSED;
    for (i = 0; i < 9; i++) { a[i] = sget(vm, self, n[i]); if (!OK_) return v2_undef(); }
    a[0] = a2_call_s(vm, a[0], "clone", 0, NULL); if (!OK_) return v2_undef();
    a[1] = a2_call_s(vm, a[1], "clone", 0, NULL); if (!OK_) return v2_undef();
    return a2_construct_class(vm, c_displacement, 9, a);
}

void a2_filters_init(VM2 *vm) {
    A2Class *c, *bf;
    c_bitmapfilter = bf = a2_def_class(vm, "flash.filters", "BitmapFilter", vm->c_object, OK_PLAIN, BitmapFilter_ctor);
    a2_add_method(vm, bf, "clone", BitmapFilter_clone, 0);

    c_blur = c = a2_def_class(vm, "flash.filters", "BlurFilter", bf, OK_PLAIN, BlurFilter_ctor);
    a2_add_slot(vm, c, "blurX", vm->c_number); a2_add_slot(vm, c, "blurY", vm->c_number); a2_add_slot(vm, c, "quality", vm->c_int);
    a2_add_method(vm, c, "clone", blur_clone, 0); c->ctor_sig = "(Number=,Number=,int=)";

    c_glow = c = a2_def_class(vm, "flash.filters", "GlowFilter", bf, OK_PLAIN, GlowFilter_ctor);
    a2_add_slot(vm, c, "alpha", vm->c_number); a2_add_slot(vm, c, "blurX", vm->c_number); a2_add_slot(vm, c, "blurY", vm->c_number); a2_add_slot(vm, c, "color", vm->c_uint);
    a2_add_slot(vm, c, "inner", vm->c_boolean); a2_add_slot(vm, c, "knockout", vm->c_boolean); a2_add_slot(vm, c, "quality", vm->c_int); a2_add_slot(vm, c, "strength", vm->c_number);
    a2_add_method(vm, c, "clone", glow_clone, 0); c->ctor_sig = "(uint=,Number=,Number=,Number=,Number=,int=,Boolean=,Boolean=)";

    c_dropshadow = c = a2_def_class(vm, "flash.filters", "DropShadowFilter", bf, OK_PLAIN, DropShadowFilter_ctor);
    a2_add_slot(vm, c, "alpha", vm->c_number); a2_add_slot(vm, c, "angle", vm->c_number); a2_add_slot(vm, c, "blurX", vm->c_number); a2_add_slot(vm, c, "blurY", vm->c_number);
    a2_add_slot(vm, c, "color", vm->c_uint); a2_add_slot(vm, c, "distance", vm->c_number); a2_add_slot(vm, c, "hideObject", vm->c_boolean); a2_add_slot(vm, c, "inner", vm->c_boolean);
    a2_add_slot(vm, c, "knockout", vm->c_boolean); a2_add_slot(vm, c, "quality", vm->c_int); a2_add_slot(vm, c, "strength", vm->c_number);
    a2_add_method(vm, c, "clone", ds_clone, 0); c->ctor_sig = "(Number=,Number=,uint=,Number=,Number=,Number=,Number=,int=,Boolean=,Boolean=,Boolean=)";

    c_bevel = c = a2_def_class(vm, "flash.filters", "BevelFilter", bf, OK_PLAIN, BevelFilter_ctor);
    a2_add_slot(vm, c, "angle", vm->c_number); a2_add_slot(vm, c, "blurX", vm->c_number); a2_add_slot(vm, c, "blurY", vm->c_number); a2_add_slot(vm, c, "distance", vm->c_number);
    a2_add_slot(vm, c, "highlightAlpha", vm->c_number); a2_add_slot(vm, c, "highlightColor", vm->c_uint); a2_add_slot(vm, c, "knockout", vm->c_boolean); a2_add_slot(vm, c, "quality", vm->c_int);
    a2_add_slot(vm, c, "shadowAlpha", vm->c_number); a2_add_slot(vm, c, "shadowColor", vm->c_uint); a2_add_slot(vm, c, "strength", vm->c_number); a2_add_slot(vm, c, "type", vm->c_string);
    a2_add_method(vm, c, "clone", bevel_clone, 0); c->ctor_sig = "(Number=,Number=,uint=,Number=,uint=,Number=,Number=,Number=,Number=,int=,String=,Boolean=)";

    {
        A2Class *gc[2]; int k;
        c_gradientglow = gc[0] = a2_def_class(vm, "flash.filters", "GradientGlowFilter", bf, OK_PLAIN, Gradient_ctor);
        c_gradientbevel = gc[1] = a2_def_class(vm, "flash.filters", "GradientBevelFilter", bf, OK_PLAIN, Gradient_ctor);
        for (k = 0; k < 2; k++) {
            c = gc[k];
            a2_add_slot(vm, c, "_alphas", vm->c_array); a2_add_slot(vm, c, "_angle", vm->c_number); a2_add_slot(vm, c, "_blurX", vm->c_number); a2_add_slot(vm, c, "_blurY", vm->c_number);
            a2_add_slot(vm, c, "_colors", vm->c_array); a2_add_slot(vm, c, "_distance", vm->c_number); a2_add_slot(vm, c, "_knockout", vm->c_boolean); a2_add_slot(vm, c, "_quality", vm->c_int);
            a2_add_slot(vm, c, "_ratios", vm->c_array); a2_add_slot(vm, c, "_strength", vm->c_number); a2_add_slot(vm, c, "_type", vm->c_string);
            a2_add_getter(vm, c, "alphas", g_get_alphas, g_set_alphas); a2_add_getter(vm, c, "angle", g_get_angle, g_set_angle); a2_add_getter(vm, c, "blurX", g_get_blurX, g_set_blurX);
            a2_add_getter(vm, c, "blurY", g_get_blurY, g_set_blurY); a2_add_getter(vm, c, "colors", g_get_colors, g_set_colors); a2_add_getter(vm, c, "distance", g_get_distance, g_set_distance);
            a2_add_getter(vm, c, "knockout", g_get_knockout, g_set_knockout); a2_add_getter(vm, c, "quality", g_get_quality, g_set_quality); a2_add_getter(vm, c, "ratios", g_get_ratios, g_set_ratios);
            a2_add_getter(vm, c, "strength", g_get_strength, g_set_strength); a2_add_getter(vm, c, "type", g_get_type, g_set_type);
            a2_add_method(vm, c, "clone", k ? gbevel_clone : gglow_clone, 0);
            c->ctor_sig = "(Number=,Number=,Array=,Array=,Array=,Number=,Number=,Number=,int=,String=,Boolean=)";
        }
    }

    c_colormatrix = c = a2_def_class(vm, "flash.filters", "ColorMatrixFilter", bf, OK_PLAIN, ColorMatrixFilter_ctor);
    a2_add_slot(vm, c, "_matrix", vm->c_array);
    a2_add_getter(vm, c, "matrix", cm_get_matrix, cm_set_matrix);
    a2_add_method(vm, c, "clone", cm_clone, 0); c->ctor_sig = "(Array=)";

    c_convolution = c = a2_def_class(vm, "flash.filters", "ConvolutionFilter", bf, OK_PLAIN, ConvolutionFilter_ctor);
    a2_add_slot(vm, c, "alpha", vm->c_number); a2_add_slot(vm, c, "bias", vm->c_number); a2_add_slot(vm, c, "clamp", vm->c_boolean); a2_add_slot(vm, c, "color", vm->c_uint);
    a2_add_slot(vm, c, "divisor", vm->c_number); a2_add_slot(vm, c, "matrix", vm->c_array); a2_add_slot(vm, c, "matrixX", vm->c_number); a2_add_slot(vm, c, "matrixY", vm->c_number);
    a2_add_slot(vm, c, "preserveAlpha", vm->c_boolean);
    a2_add_method(vm, c, "clone", conv_clone, 0); c->ctor_sig = "(Number=,Number=,Array=,Number=,Number=,Boolean=,Boolean=,uint=,Number=)";

    c_displacement = c = a2_def_class(vm, "flash.filters", "DisplacementMapFilter", bf, OK_PLAIN, DisplacementMapFilter_ctor);
    a2_add_slot(vm, c, "alpha", vm->c_number); a2_add_slot(vm, c, "color", vm->c_uint); a2_add_slot(vm, c, "componentX", vm->c_uint); a2_add_slot(vm, c, "componentY", vm->c_uint);
    a2_add_slot(vm, c, "mapBitmap", vm->c_bitmapdata); a2_add_slot(vm, c, "mapPoint", vm->c_point); a2_add_slot(vm, c, "mode", vm->c_string);
    a2_add_slot(vm, c, "scaleX", vm->c_number); a2_add_slot(vm, c, "scaleY", vm->c_number);
    a2_add_method(vm, c, "clone", dm_clone, 0); c->ctor_sig = "(flash.display::BitmapData=,flash.geom::Point=,uint=,uint=,Number=,Number=,String=,uint=,Number=)";

    c_shaderfilter = c = a2_def_class(vm, "flash.filters", "ShaderFilter", bf, OK_PLAIN, ShaderFilter_ctor);
    a2_add_slot(vm, c, "_bottomExtension", vm->c_int); a2_add_slot(vm, c, "_leftExtension", vm->c_int); a2_add_slot(vm, c, "_rightExtension", vm->c_int); a2_add_slot(vm, c, "_topExtension", vm->c_int);
    a2_add_slot(vm, c, "_shader", NULL);
    a2_add_getter(vm, c, "bottomExtension", s_get_bottomExtension, s_set_bottomExtension); a2_add_getter(vm, c, "leftExtension", s_get_leftExtension, s_set_leftExtension);
    a2_add_getter(vm, c, "rightExtension", s_get_rightExtension, s_set_rightExtension); a2_add_getter(vm, c, "topExtension", s_get_topExtension, s_set_topExtension);
    a2_add_getter(vm, c, "shader", s_get_shader, s_set_shader); c->ctor_sig = "(flash.display::Shader=)";

    c = a2_def_class(vm, "flash.filters", "BitmapFilterQuality", vm->c_object, OK_PLAIN, NULL); a2_add_const(vm, c, "LOW", v2_int(1)); a2_add_const(vm, c, "MEDIUM", v2_int(2)); a2_add_const(vm, c, "HIGH", v2_int(3));
    c = a2_def_class(vm, "flash.filters", "BitmapFilterType", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "INNER", "inner"); a2_const_str(vm, c, "OUTER", "outer"); a2_const_str(vm, c, "FULL", "full");
    c = a2_def_class(vm, "flash.filters", "DisplacementMapFilterMode", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "WRAP", "wrap"); a2_const_str(vm, c, "CLAMP", "clamp"); a2_const_str(vm, c, "IGNORE", "ignore"); a2_const_str(vm, c, "COLOR", "color");
}

/* ---------------------------------------------------------------- */
/* object <-> SWF record                                            */

static double gnum(VM2 *vm, V2 o, const char *n) { return v2_tonum(vm, a2_get_s(vm, o, n)); }
static u32 guint(VM2 *vm, V2 o, const char *n) { return v2_touint(vm, a2_get_s(vm, o, n)); }
static s32 gint(VM2 *vm, V2 o, const char *n) { return v2_toint(vm, a2_get_s(vm, o, n)); }
static int gbool(VM2 *vm, V2 o, const char *n) { return v2_tobool(vm, a2_get_s(vm, o, n)); }
static void rgba(u8 *c, u32 rgb, double alpha) { c[0] = (u8)(rgb >> 16); c[1] = (u8)(rgb >> 8); c[2] = (u8)rgb; c[3] = sf_u8_sat(alpha * 255.0); }
static u8 passes(s32 q) { return (u8)(q < 0 ? 0 : q > 15 ? 15 : q); }
static double clampd(double v, double lo, double hi) { return v != v ? v : v < lo ? lo : v > hi ? hi : v; }
static double maxd0(double v) { return v != v ? v : v < 0 ? 0 : v; }
static int type_flags(VM2 *vm, V2 o, const char *field) {
    V2 t = a2_get_s(vm, o, field); const char *s;
    if (t.t == T_NULL || t.t == T_UNDEF) s = "null"; else s = v2_tostr(vm, t)->s;
    if (!strcmp(s, "inner")) return SFF_INNER;
    if (!strcmp(s, "outer")) return 0;
    return SFF_ONTOP;
}

static int to_record(VM2 *vm, V2 o, SwfFilter *f);
int a2_filter_record(VM2 *vm, V2 o, SwfFilter *f) { memset(f, 0, sizeof(*f)); return to_record(vm, o, f); }
static int to_record(VM2 *vm, V2 o, SwfFilter *f) {
    A2Class *c = o.u.o->cls;
    memset(f, 0, sizeof(*f));
    if (a2_is_subclass(c, c_bevel)) {
        f->kind = SF_BEVEL;
        rgba(f->color, guint(vm, o, "shadowColor"), gnum(vm, o, "shadowAlpha"));
        rgba(f->color2, guint(vm, o, "highlightColor"), gnum(vm, o, "highlightAlpha"));
        f->blur_x = sf_fixed16(maxd0(gnum(vm, o, "blurX"))); f->blur_y = sf_fixed16(maxd0(gnum(vm, o, "blurY")));
        f->angle = sf_fixed16(gnum(vm, o, "angle") * M_PI / 180.0); f->distance = sf_fixed16(gnum(vm, o, "distance"));
        f->strength = sf_fixed8(clampd(gnum(vm, o, "strength"), 0, 255));
        f->flags = SFF_COMPOSITE | type_flags(vm, o, "type") | (gbool(vm, o, "knockout") ? SFF_KNOCKOUT : 0);
        f->passes = passes(gint(vm, o, "quality"));
    } else if (a2_is_subclass(c, c_blur)) {
        f->kind = SF_BLUR;
        f->blur_x = sf_fixed16(maxd0(gnum(vm, o, "blurX"))); f->blur_y = sf_fixed16(maxd0(gnum(vm, o, "blurY")));
        f->passes = passes(gint(vm, o, "quality"));
    } else if (a2_is_subclass(c, c_colormatrix)) {
        V2 m = a2_get_s(vm, o, "_matrix"); int i;
        f->kind = SF_COLORMATRIX;
        if (m.t == T_OBJ && m.u.o->cls == vm->c_array) {
            int n = (int)a2_arr_len(m.u.o);
            for (i = 0; i < 20 && i < n; i++) { V2 e = a2_arr_get(m.u.o, i); double dv = e.t == T_HOLE ? 0.0 : v2_tonum(vm, e); f->m[i] = dv == dv ? (float)dv : 0.0f; if (vm->throwing) return 0; }
        }
    } else if (a2_is_subclass(c, c_convolution)) {
        V2 m = a2_get_s(vm, o, "matrix"); u32 mx, my; int i, n = 0; float *vals = NULL;
        f->kind = SF_CONVOLUTION;
        if (m.t == T_OBJ && m.u.o->cls == vm->c_array) {
            n = (int)a2_arr_len(m.u.o);
            vals = (float*)calloc((size_t)(n ? n : 1), sizeof(float));
            for (i = 0; vals && i < n; i++) { V2 e = a2_arr_get(m.u.o, i); vals[i] = (float)v2_tonum(vm, e.t == T_HOLE ? v2_undef() : e); if (vm->throwing) { free(vals); return 0; } }
        }
        rgba(f->color, guint(vm, o, "color"), gnum(vm, o, "alpha"));
        f->bias = (float)gnum(vm, o, "bias"); f->divisor = (float)gnum(vm, o, "divisor");
        f->flags = (gbool(vm, o, "clamp") ? SFF_CLAMP : 0) | (gbool(vm, o, "preserveAlpha") ? SFF_PRESERVE : 0);
        mx = guint(vm, o, "matrixX"); my = guint(vm, o, "matrixY");
        f->mx = mx > 255 ? 255 : (int)mx; f->my = my > 255 ? 255 : (int)my;
        {   /* the matrix is resized to matrixX * matrixY (u32 product) */
            u32 want = mx * my; size_t k;
            if (want > 65536u * 4) want = 65536u * 4;
            f->cmatrix = (float*)calloc(want ? want : 1, sizeof(float));
            for (k = 0; f->cmatrix && k < want && (int)k < n && vals; k++) f->cmatrix[k] = vals[k];
            f->ncm = (int)want;
        }
        free(vals);
    } else if (a2_is_subclass(c, c_displacement)) {
        V2 pt = a2_get_s(vm, o, "mapPoint"), mode = a2_get_s(vm, o, "mode");
        f->kind = SF_DISPLACEMENT;
        { extern BitmapData *a2_bitmapdata_of(A2Obj *o); V2 mb = a2_get_s(vm, o, "mapBitmap"); BitmapData *bd = mb.t == T_OBJ ? a2_bitmapdata_of(mb.u.o) : NULL; if (bd) f->map_bmd = bmd_ref(bd); }
        if (pt.t == T_OBJ) { f->map_x = v2_toint(vm, a2_get_s(vm, pt, "x")); f->map_y = v2_toint(vm, a2_get_s(vm, pt, "y")); }
        if (mode.t != T_NULL && mode.t != T_UNDEF) {
            const char *s = v2_tostr(vm, mode)->s;
            if (!strcmp(s, "clamp")) f->mode = 1; else if (!strcmp(s, "ignore")) f->mode = 2; else if (!strcmp(s, "color")) f->mode = 3; else if (!strcmp(s, "wrap")) f->mode = 0;
            else { a2_throw_error(vm, "ArgumentError", "Parameter mode must be one of the accepted values.", 2008); return 0; }
        }
        rgba(f->color, guint(vm, o, "color"), gnum(vm, o, "alpha"));
        f->comp_x = (u8)guint(vm, o, "componentX"); f->comp_y = (u8)guint(vm, o, "componentY");
        f->scale_x = (float)gnum(vm, o, "scaleX"); f->scale_y = (float)gnum(vm, o, "scaleY");
    } else if (a2_is_subclass(c, c_dropshadow)) {
        f->kind = SF_DROPSHADOW;
        rgba(f->color, guint(vm, o, "color"), gnum(vm, o, "alpha"));
        f->angle = sf_fixed16(gnum(vm, o, "angle") * M_PI / 180.0);
        f->blur_x = sf_fixed16(maxd0(gnum(vm, o, "blurX"))); f->blur_y = sf_fixed16(maxd0(gnum(vm, o, "blurY")));
        f->distance = sf_fixed16(gnum(vm, o, "distance")); f->strength = sf_fixed8(clampd(gnum(vm, o, "strength"), 0, 255));
        f->flags = (gbool(vm, o, "hideObject") ? 0 : SFF_COMPOSITE) | (gbool(vm, o, "inner") ? SFF_INNER : 0) | (gbool(vm, o, "knockout") ? SFF_KNOCKOUT : 0);
        f->passes = passes(gint(vm, o, "quality"));
    } else if (a2_is_subclass(c, c_glow)) {
        f->kind = SF_GLOW;
        rgba(f->color, guint(vm, o, "color"), gnum(vm, o, "alpha"));
        f->blur_x = sf_fixed16(maxd0(gnum(vm, o, "blurX"))); f->blur_y = sf_fixed16(maxd0(gnum(vm, o, "blurY")));
        f->strength = sf_fixed8(clampd(gnum(vm, o, "strength"), 0, 255));
        f->flags = SFF_COMPOSITE | (gbool(vm, o, "inner") ? SFF_INNER : 0) | (gbool(vm, o, "knockout") ? SFF_KNOCKOUT : 0);
        f->passes = passes(gint(vm, o, "quality"));
    } else if (a2_is_subclass(c, c_gradientbevel) || a2_is_subclass(c, c_gradientglow)) {
        V2 cs = a2_get_s(vm, o, "_colors"), as = a2_get_s(vm, o, "_alphas"), rs = a2_get_s(vm, o, "_ratios");
        f->kind = a2_is_subclass(c, c_gradientbevel) ? SF_GRADIENTBEVEL : SF_GRADIENTGLOW;
        if (cs.t == T_OBJ && cs.u.o->cls == vm->c_array && as.t == T_OBJ && as.u.o->cls == vm->c_array && rs.t == T_OBJ && rs.u.o->cls == vm->c_array) {
            int nc = (int)a2_arr_len(cs.u.o), na = (int)a2_arr_len(as.u.o), nr = (int)a2_arr_len(rs.u.o), i, n = nr < nc ? nr : nc;
            for (i = 0; i < n; i++) {
                V2 cv = a2_arr_get(cs.u.o, i), av = i < na ? a2_arr_get(as.u.o, i) : v2_num(1.0), rv = a2_arr_get(rs.u.o, i);
                u32 col = v2_touint(vm, cv.t == T_HOLE ? v2_undef() : cv); double al = av.t == T_HOLE ? 0.0 : v2_tonum(vm, av); s32 ra = v2_toint(vm, rv.t == T_HOLE ? v2_undef() : rv);
                if (vm->throwing) return 0;
                if (i < 16) { rgba(f->gcolor[i], col, al); f->ratio[i] = (u8)(ra < 0 ? 0 : ra > 255 ? 255 : ra); }
            }
            f->ncolors = n < 16 ? n : 16;
            for (i = 0; i < n; i++) { V2 e1 = a2_arr_get(cs.u.o, i), e2 = i < na ? a2_arr_get(as.u.o, i) : v2_num(1), e3 = a2_arr_get(rs.u.o, i); if (e1.t == T_HOLE || e1.t == T_UNDEF || e2.t == T_HOLE || e2.t == T_UNDEF || e3.t == T_HOLE || e3.t == T_UNDEF) f->ncolors = 0; }   /* a gap: no gradient (Flash draws nothing) */
        }
        f->blur_x = sf_fixed16(maxd0(gnum(vm, o, "_blurX"))); f->blur_y = sf_fixed16(maxd0(gnum(vm, o, "_blurY")));
        f->angle = sf_fixed16(gnum(vm, o, "_angle") * M_PI / 180.0); f->distance = sf_fixed16(gnum(vm, o, "_distance"));
        f->strength = sf_fixed8(clampd(gnum(vm, o, "_strength"), 0, 255));
        f->flags = SFF_COMPOSITE | (gbool(vm, o, "_knockout") ? SFF_KNOCKOUT : 0) | type_flags(vm, o, "_type");
        f->passes = passes(gint(vm, o, "_quality"));
    } else if (a2_is_subclass(c, c_shaderfilter)) {
        extern struct PbjJob *a2_shader_snapshot(VM2 *vm, V2 shader);
        V2 sh = a2_get_s(vm, o, "_shader");
        f->kind = SF_SHADER;
        f->shader_obj = sh.t == T_OBJ ? sh.u.o : NULL;
        if (!vm->throwing) f->pbj = a2_shader_snapshot(vm, sh);
    } else return 0;
    return !vm->throwing;
}

static double fx16(s32 v) { return v / 65536.0; }
static V2 arr_of_nums(VM2 *vm, const double *v, int n) { A2Obj *a = a2_new_array(vm); int i; for (i = 0; i < n; i++) a2_arr_push(vm, a, v2_num(v[i])); return v2_obj(a); }
static V2 deg(s32 a) { return v2_num(fx16(a) * 180.0 / M_PI); }
static V2 type_str(VM2 *vm, u8 flags) { return v2_cstr(vm, (flags & SFF_ONTOP) ? "full" : (flags & SFF_INNER) ? "inner" : "outer"); }
static V2 alpha_of(const u8 *c) { return v2_num(c[3] / 255.0); }
static V2 rgb_of(const u8 *c) { return v2_uint(((u32)c[0] << 16) | ((u32)c[1] << 8) | c[2]); }

static V2 from_record(VM2 *vm, const SwfFilter *f) {
    V2 a[12];
    switch (f->kind) {
    case SF_BEVEL:
        a[0] = v2_num(fx16(f->distance)); a[1] = deg(f->angle); a[2] = rgb_of(f->color2); a[3] = alpha_of(f->color2); a[4] = rgb_of(f->color); a[5] = alpha_of(f->color);
        a[6] = v2_num(fx16(f->blur_x)); a[7] = v2_num(fx16(f->blur_y)); a[8] = v2_num(f->strength / 256.0); a[9] = v2_int(f->passes); a[10] = type_str(vm, f->flags); a[11] = v2_bool(f->flags & SFF_KNOCKOUT);
        return a2_construct_class(vm, c_bevel, 12, a);
    case SF_BLUR:
        a[0] = v2_num(fx16(f->blur_x)); a[1] = v2_num(fx16(f->blur_y)); a[2] = v2_int(f->passes);
        return a2_construct_class(vm, c_blur, 3, a);
    case SF_COLORMATRIX: { double m[20]; int i; for (i = 0; i < 20; i++) m[i] = f->m[i]; a[0] = arr_of_nums(vm, m, 20); return a2_construct_class(vm, c_colormatrix, 1, a); }
    case SF_CONVOLUTION: {
        A2Obj *arr = a2_new_array(vm); int i;
        for (i = 0; i < f->ncm && f->cmatrix; i++) a2_arr_push(vm, arr, v2_num(f->cmatrix[i]));
        a[0] = v2_int(f->mx); a[1] = v2_int(f->my); a[2] = v2_obj(arr); a[3] = v2_num(f->divisor); a[4] = v2_num(f->bias);
        a[5] = v2_bool(f->flags & SFF_PRESERVE); a[6] = v2_bool(f->flags & SFF_CLAMP); a[7] = rgb_of(f->color); a[8] = alpha_of(f->color);
        return a2_construct_class(vm, c_convolution, 9, a); }
    case SF_DISPLACEMENT: {
        static const char *modes[4] = { "wrap", "clamp", "ignore", "color" };
        { extern A2Obj *a2_bitmapdata_wrap(VM2 *vm, BitmapData *b); a[0] = f->map_bmd ? v2_obj(a2_bitmapdata_wrap(vm, f->map_bmd)) : v2_null(); }
        a[1] = v2_obj(a2_point(vm, f->map_x, f->map_y)); a[2] = v2_uint((u32)f->comp_x); a[3] = v2_uint((u32)f->comp_y);
        a[4] = v2_num(f->scale_x); a[5] = v2_num(f->scale_y); a[6] = v2_cstr(vm, modes[f->mode & 3]); a[7] = rgb_of(f->color); a[8] = alpha_of(f->color);
        return a2_construct_class(vm, c_displacement, 9, a); }
    case SF_DROPSHADOW:
        a[0] = v2_num(fx16(f->distance)); a[1] = deg(f->angle); a[2] = rgb_of(f->color); a[3] = alpha_of(f->color); a[4] = v2_num(fx16(f->blur_x)); a[5] = v2_num(fx16(f->blur_y));
        a[6] = v2_num(f->strength / 256.0); a[7] = v2_int(f->passes); a[8] = v2_bool(f->flags & SFF_INNER); a[9] = v2_bool(f->flags & SFF_KNOCKOUT); a[10] = v2_bool(!(f->flags & SFF_COMPOSITE));
        return a2_construct_class(vm, c_dropshadow, 11, a);
    case SF_GLOW:
        a[0] = rgb_of(f->color); a[1] = alpha_of(f->color); a[2] = v2_num(fx16(f->blur_x)); a[3] = v2_num(fx16(f->blur_y)); a[4] = v2_num(f->strength / 256.0);
        a[5] = v2_int(f->passes); a[6] = v2_bool(f->flags & SFF_INNER); a[7] = v2_bool(f->flags & SFF_KNOCKOUT);
        return a2_construct_class(vm, c_glow, 8, a);
    case SF_GRADIENTGLOW: case SF_GRADIENTBEVEL: {
        A2Obj *cs = a2_new_array(vm), *as = a2_new_array(vm), *rs = a2_new_array(vm); int i;
        for (i = 0; i < f->ncolors; i++) { a2_arr_push(vm, cs, rgb_of(f->gcolor[i])); a2_arr_push(vm, as, alpha_of(f->gcolor[i])); a2_arr_push(vm, rs, v2_int(f->ratio[i])); }
        a[0] = v2_num(fx16(f->distance)); a[1] = deg(f->angle); a[2] = v2_obj(cs); a[3] = v2_obj(as); a[4] = v2_obj(rs);
        a[5] = v2_num(fx16(f->blur_x)); a[6] = v2_num(fx16(f->blur_y)); a[7] = v2_num(f->strength / 256.0); a[8] = v2_int(f->passes); a[9] = type_str(vm, f->flags); a[10] = v2_bool(f->flags & SFF_KNOCKOUT);
        return a2_construct_class(vm, f->kind == SF_GRADIENTGLOW ? c_gradientglow : c_gradientbevel, 11, a); }
    case SF_SHADER:
        a[0] = f->shader_obj ? v2_obj((A2Obj*)f->shader_obj) : v2_null();
        return a2_construct_class(vm, c_shaderfilter, 1, a);
    }
    return v2_null();
}

/* ---------------------------------------------------------------- */
/* DisplayObject.filters                                            */

V2 a2_filters_get(VM2 *vm, DisplayObject *d) {
    A2Obj *arr = a2_new_array(vm); int i;
    for (i = 0; d && i < d->nfilters; i++) {
        V2 v = from_record(vm, &d->filters[i]);
        if (vm->throwing) return v2_undef();
        a2_arr_push(vm, arr, v);
    }
    return v2_obj(arr);
}

void a2_filters_set(VM2 *vm, DisplayObject *d, V2 v) {
    SwfFilter *list = NULL; int n = 0, i, len;
    v = a2_coerce(vm, v, vm->c_array);
    if (vm->throwing || !d) return;
    if (v.t != T_OBJ) { swf_filters_free(d->filters, d->nfilters); d->filters = NULL; d->nfilters = 0; return; }
    if (v.u.o->cls != vm->c_array && !a2_is_subclass(v.u.o->cls, vm->c_array)) return;
    len = (int)a2_arr_len(v.u.o);
    if (len > 0) list = (SwfFilter*)calloc((size_t)len, sizeof(SwfFilter));
    for (i = 0; i < len; i++) {
        V2 e = i < v.u.o->alen ? v.u.o->arr[i] : v2_undef();
        if (i >= v.u.o->alen || e.t == T_HOLE) continue;   /* holes are skipped */
        if (e.t != T_OBJ || !e.u.o->cls || !a2_is_subclass(e.u.o->cls, c_bitmapfilter)) {
            swf_filters_free(list, n);
            a2_throw_error(vm, "ArgumentError", "Parameter 0 is of the incorrect type. Should be type Filter.", 2005);
            return;
        }
        if (!to_record(vm, e, &list[n])) { swf_filters_free(list, n + 1); return; }
        n++;
    }
    swf_filters_free(d->filters, d->nfilters);
    d->filters = n ? list : NULL; d->nfilters = n;
    if (!n) free(list);
    if (d->obj2) {   /* ShaderFilter objects stay reachable through the display object */
        A2Obj *keep = a2_new_array(vm);
        for (i = 0; i < n; i++) if (list[i].shader_obj) a2_arr_push(vm, keep, v2_obj((A2Obj*)list[i].shader_obj));
        a2_dyn_set_hidden(vm, (A2Obj*)d->obj2, a2_str(vm, "__filterShaders"), v2_obj(keep));
    }
}
