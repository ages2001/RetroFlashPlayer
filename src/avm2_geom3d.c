/* avm2_geom3d.c - flash.geom 3D classes: Vector3D, Matrix3D, Utils3D,
 * PerspectiveProjection.
 *
 * Flash keeps Matrix3D components as 32-bit floats and does most of its
 * arithmetic in single precision; the operations below round every step to
 * float the same way (F()), so results match bit for bit.  The formulas and
 * the precision rules follow Ruffle's implementation (MIT/Apache-2.0), which
 * documents the observable Flash Player behaviour. */
#include "avm2_flash_int.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* single precision rounding of an intermediate (x87 keeps extra precision otherwise) */
static float F(double x) { volatile float v = (float)x; return v; }

static A2Class *c_vector3d, *c_matrix3d;

/* ---------------------------------------------------------------- */
/* Vector3D                                                         */

static double vget(VM2 *vm, V2 o, const char *k) { return o.t == T_OBJ ? v2_tonum(vm, a2_get_s(vm, o, k)) : 0; }
static void vset(VM2 *vm, V2 o, const char *k, double d) { if (o.t == T_OBJ) a2_set_s(vm, o, k, v2_num(d)); }

static V2 v3_new(VM2 *vm, double x, double y, double z, double w) {
    V2 a[4]; a[0] = v2_num(x); a[1] = v2_num(y); a[2] = v2_num(z); a[3] = v2_num(w);
    return a2_construct_class(vm, c_vector3d, 4, a);
}
static int need_obj(VM2 *vm, V2 v, const char *name) {
    if (v.t == T_OBJ) return 1;
    { char b[200]; sprintf(b, "Parameter %.100s must be non-null.", name); a2_throw_error(vm, "TypeError", b, 2007); }
    return 0;
}

static V2 Vector3D_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    vset(vm, self, "x", argc > 0 ? NUM(0) : 0); vset(vm, self, "y", argc > 1 ? NUM(1) : 0);
    vset(vm, self, "z", argc > 2 ? NUM(2) : 0); vset(vm, self, "w", argc > 3 ? NUM(3) : 0);
    return v2_undef();
}
static double v3_len2(VM2 *vm, V2 s) { double x = vget(vm, s, "x"), y = vget(vm, s, "y"), z = vget(vm, s, "z"); return x * x + y * y + z * z; }
static V2 v3_length(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_num(sqrt(v3_len2(vm, self))); }
static V2 v3_lengthSquared(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_num(v3_len2(vm, self)); }
static V2 v3_toString(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Str *r = a2_str(vm, "Vector3D("); UNUSED;
    r = a2_concat(vm, r, v2_tostr(vm, a2_get_s(vm, self, "x"))); r = a2_concat(vm, r, a2_str(vm, ", "));
    r = a2_concat(vm, r, v2_tostr(vm, a2_get_s(vm, self, "y"))); r = a2_concat(vm, r, a2_str(vm, ", "));
    r = a2_concat(vm, r, v2_tostr(vm, a2_get_s(vm, self, "z"))); r = a2_concat(vm, r, a2_str(vm, ")"));
    return v2_str(r);
}
static V2 v3_add(VM2 *vm, V2 self, int argc, V2 *argv) { V2 a = ARG(0); if (!need_obj(vm, a, "a")) return v2_undef(); return v3_new(vm, vget(vm, self, "x") + vget(vm, a, "x"), vget(vm, self, "y") + vget(vm, a, "y"), vget(vm, self, "z") + vget(vm, a, "z"), 0); }
static V2 v3_subtract(VM2 *vm, V2 self, int argc, V2 *argv) { V2 a = ARG(0); if (!need_obj(vm, a, "a")) return v2_undef(); return v3_new(vm, vget(vm, self, "x") - vget(vm, a, "x"), vget(vm, self, "y") - vget(vm, a, "y"), vget(vm, self, "z") - vget(vm, a, "z"), 0); }
static V2 v3_incrementBy(VM2 *vm, V2 self, int argc, V2 *argv) { V2 a = ARG(0); if (!need_obj(vm, a, "a")) return v2_undef(); vset(vm, self, "x", vget(vm, self, "x") + vget(vm, a, "x")); vset(vm, self, "y", vget(vm, self, "y") + vget(vm, a, "y")); vset(vm, self, "z", vget(vm, self, "z") + vget(vm, a, "z")); return v2_undef(); }
static V2 v3_decrementBy(VM2 *vm, V2 self, int argc, V2 *argv) { V2 a = ARG(0); if (!need_obj(vm, a, "a")) return v2_undef(); vset(vm, self, "x", vget(vm, self, "x") - vget(vm, a, "x")); vset(vm, self, "y", vget(vm, self, "y") - vget(vm, a, "y")); vset(vm, self, "z", vget(vm, self, "z") - vget(vm, a, "z")); return v2_undef(); }
static V2 v3_clone(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v3_new(vm, vget(vm, self, "x"), vget(vm, self, "y"), vget(vm, self, "z"), vget(vm, self, "w")); }
static V2 v3_copyFrom(VM2 *vm, V2 self, int argc, V2 *argv) { V2 a = ARG(0); if (!need_obj(vm, a, "sourceVector3D")) return v2_undef(); vset(vm, self, "x", vget(vm, a, "x")); vset(vm, self, "y", vget(vm, a, "y")); vset(vm, self, "z", vget(vm, a, "z")); return v2_undef(); }
static V2 v3_equals(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 a = ARG(0); int all = argc > 1 && BOOL(1);
    if (!need_obj(vm, a, "toCompare")) return v2_undef();
    return v2_bool(vget(vm, self, "x") == vget(vm, a, "x") && vget(vm, self, "y") == vget(vm, a, "y") && vget(vm, self, "z") == vget(vm, a, "z") && (!all || vget(vm, self, "w") == vget(vm, a, "w")));
}
static V2 v3_nearEquals(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 a = ARG(0); double tol = NUM(1); int all = argc > 2 && BOOL(2);
    if (!need_obj(vm, a, "toCompare")) return v2_undef();
    if (!(fabs(vget(vm, self, "x") - vget(vm, a, "x")) < tol)) return v2_bool(0);
    if (!(fabs(vget(vm, self, "y") - vget(vm, a, "y")) < tol)) return v2_bool(0);
    if (!(fabs(vget(vm, self, "z") - vget(vm, a, "z")) < tol)) return v2_bool(0);
    if (all) { double w = vget(vm, a, "w"); vset(vm, self, "w", w); return v2_bool(fabs(w) < tol); }   /* Flash assigns instead of subtracting */
    return v2_bool(1);
}
static V2 v3_setTo(VM2 *vm, V2 self, int argc, V2 *argv) { vset(vm, self, "x", NUM(0)); vset(vm, self, "y", NUM(1)); vset(vm, self, "z", NUM(2)); return v2_undef(); }
static V2 v3_scaleBy(VM2 *vm, V2 self, int argc, V2 *argv) { double s = NUM(0); vset(vm, self, "x", vget(vm, self, "x") * s); vset(vm, self, "y", vget(vm, self, "y") * s); vset(vm, self, "z", vget(vm, self, "z") * s); return v2_undef(); }
static V2 v3_negate(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; vset(vm, self, "x", vget(vm, self, "x") * -1); vset(vm, self, "y", vget(vm, self, "y") * -1); vset(vm, self, "z", vget(vm, self, "z") * -1); return v2_undef(); }
static V2 v3_project(VM2 *vm, V2 self, int argc, V2 *argv) { double w = vget(vm, self, "w"); UNUSED; vset(vm, self, "x", vget(vm, self, "x") / w); vset(vm, self, "y", vget(vm, self, "y") / w); vset(vm, self, "z", vget(vm, self, "z") / w); return v2_undef(); }
static V2 v3_normalize(VM2 *vm, V2 self, int argc, V2 *argv) {
    double len = sqrt(v3_len2(vm, self)); UNUSED;
    if (len == 0) { vset(vm, self, "x", 0); vset(vm, self, "y", 0); vset(vm, self, "z", 0); }
    else if (len > 0) { vset(vm, self, "x", vget(vm, self, "x") / len); vset(vm, self, "y", vget(vm, self, "y") / len); vset(vm, self, "z", vget(vm, self, "z") / len); }
    else { double n = 0.0 / 0.0; vset(vm, self, "x", n); vset(vm, self, "y", n); vset(vm, self, "z", n); }
    return v2_num(len);
}
static V2 v3_dotProduct(VM2 *vm, V2 self, int argc, V2 *argv) { V2 a = ARG(0); if (!need_obj(vm, a, "a")) return v2_undef(); return v2_num(vget(vm, self, "x") * vget(vm, a, "x") + vget(vm, self, "y") * vget(vm, a, "y") + vget(vm, self, "z") * vget(vm, a, "z")); }
static V2 v3_crossProduct(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 a = ARG(0); double x, y, z, ax, ay, az;
    if (!need_obj(vm, a, "a")) return v2_undef();
    x = vget(vm, self, "x"); y = vget(vm, self, "y"); z = vget(vm, self, "z"); ax = vget(vm, a, "x"); ay = vget(vm, a, "y"); az = vget(vm, a, "z");
    return v3_new(vm, y * az - z * ay, z * ax - x * az, x * ay - y * ax, 1);   /* w is always 1 */
}
static V2 v3_angleBetween(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 a = ARG(0), b = ARG(1); (void)self;
    if (!need_obj(vm, a, "a") || !need_obj(vm, b, "b")) return v2_undef();
    return v2_num(acos((vget(vm, a, "x") * vget(vm, b, "x") + vget(vm, a, "y") * vget(vm, b, "y") + vget(vm, a, "z") * vget(vm, b, "z")) / (sqrt(v3_len2(vm, a)) * sqrt(v3_len2(vm, b)))));
}
static V2 v3_distance(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 a = ARG(0), b = ARG(1); double dx, dy, dz; (void)self;
    if (!need_obj(vm, a, "pt1") || !need_obj(vm, b, "pt2")) return v2_undef();
    dx = vget(vm, b, "x") - vget(vm, a, "x"); dy = vget(vm, b, "y") - vget(vm, a, "y"); dz = vget(vm, b, "z") - vget(vm, a, "z");
    return v2_num(sqrt(dx * dx + dy * dy + dz * dz));
}

/* ---------------------------------------------------------------- */
/* Matrix3D                                                         */

typedef struct { float m[16]; const void *owner; } M3;   /* owner: the display object it was assigned to */
static const float M3_IDENT[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

static M3 *m3_of(A2Obj *o) {
    if (!o) return NULL;
    if (!o->payload) { M3 *m = (M3*)malloc(sizeof(M3)); if (!m) return NULL; memcpy(m->m, M3_IDENT, sizeof(M3_IDENT)); m->owner = NULL; o->payload = m; }
    return (M3*)o->payload;
}
static M3 *m3_arg(VM2 *vm, V2 v, const char *name) {
    if (v.t != T_OBJ) { char b[200]; sprintf(b, "Parameter %.100s must be non-null.", name); a2_throw_error(vm, "TypeError", b, 2007); return NULL; }
    return m3_of(v.u.o);
}
/* lhs * rhs (column-major), every step in single precision */
static void m3_mul(const float *l, const float *r, float *out) {
    float t[16]; int c, row;
    for (c = 0; c < 4; c++) for (row = 0; row < 4; row++)
        t[c * 4 + row] = F(F(F(F(l[row] * r[c * 4]) + F(l[4 + row] * r[c * 4 + 1])) + F(l[8 + row] * r[c * 4 + 2])) + F(l[12 + row] * r[c * 4 + 3]));
    memcpy(out, t, sizeof(t));
}
static void m3_transpose(float *m) {
    float t; int i; static const int sw[6][2] = { { 1, 4 }, { 2, 8 }, { 3, 12 }, { 6, 9 }, { 7, 13 }, { 11, 14 } };
    for (i = 0; i < 6; i++) { t = m[sw[i][0]]; m[sw[i][0]] = m[sw[i][1]]; m[sw[i][1]] = t; }
}
static float det3(float a0, float a1, float a2, float a3, float a4, float a5, float a6, float a7, float a8) {
    return F(F(F(a0 * F(F(a4 * a8) - F(a7 * a5))) - F(a3 * F(F(a1 * a8) - F(a7 * a2)))) + F(a6 * F(F(a1 * a5) - F(a4 * a2))));
}
static float m3_det(const float *m) {
    if (m[3] == 0 && m[7] == 0 && m[11] == 0)   /* no projection: the last element folds into the 3x3 */
        return det3(m[0], m[1], F(m[2] * m[15]), m[4], m[5], F(m[6] * m[15]), m[8], m[9], F(m[10] * m[15]));
    {
        float d0 = det3(m[5], m[6], m[7], m[9], m[10], m[11], m[13], m[14], m[15]);
        float d4 = det3(m[1], m[2], m[3], m[9], m[10], m[11], m[13], m[14], m[15]);
        float d8 = det3(m[1], m[2], m[3], m[5], m[6], m[7], m[13], m[14], m[15]);
        float d12 = det3(m[1], m[2], m[3], m[5], m[6], m[7], m[9], m[10], m[11]);
        return F(F(F(F(m[0] * d0) - F(m[4] * d4)) + F(m[8] * d8)) - F(m[12] * d12));
    }
}
static int m3_invert(const float *m, float *out) {
    double d = m3_det(m), r[16]; int i;
    double m11 = m[0], m21 = m[4], m31 = m[8], m41 = m[12], m12 = m[1], m22 = m[5], m32 = m[9], m42 = m[13];
    double m13 = m[2], m23 = m[6], m33 = m[10], m43 = m[14], m14 = m[3], m24 = m[7], m34 = m[11], m44 = m[15];
    if (!(fabs(d) > 0.00000000001)) return 0;
    d = 1.0 / d;
    r[0]  =  d * (m22 * (m33 * m44 - m43 * m34) - m32 * (m23 * m44 - m43 * m24) + m42 * (m23 * m34 - m33 * m24));
    r[1]  = -d * (m12 * (m33 * m44 - m43 * m34) - m32 * (m13 * m44 - m43 * m14) + m42 * (m13 * m34 - m33 * m14));
    r[2]  =  d * (m12 * (m23 * m44 - m43 * m24) - m22 * (m13 * m44 - m43 * m14) + m42 * (m13 * m24 - m23 * m14));
    r[3]  = -d * (m12 * (m23 * m34 - m33 * m24) - m22 * (m13 * m34 - m33 * m14) + m32 * (m13 * m24 - m23 * m14));
    r[4]  = -d * (m21 * (m33 * m44 - m43 * m34) - m31 * (m23 * m44 - m43 * m24) + m41 * (m23 * m34 - m33 * m24));
    r[5]  =  d * (m11 * (m33 * m44 - m43 * m34) - m31 * (m13 * m44 - m43 * m14) + m41 * (m13 * m34 - m33 * m14));
    r[6]  = -d * (m11 * (m23 * m44 - m43 * m24) - m21 * (m13 * m44 - m43 * m14) + m41 * (m13 * m24 - m23 * m14));
    r[7]  =  d * (m11 * (m23 * m34 - m33 * m24) - m21 * (m13 * m34 - m33 * m14) + m31 * (m13 * m24 - m23 * m14));
    r[8]  =  d * (m21 * (m32 * m44 - m42 * m34) - m31 * (m22 * m44 - m42 * m24) + m41 * (m22 * m34 - m32 * m24));
    r[9]  = -d * (m11 * (m32 * m44 - m42 * m34) - m31 * (m12 * m44 - m42 * m14) + m41 * (m12 * m34 - m32 * m14));
    r[10] =  d * (m11 * (m22 * m44 - m42 * m24) - m21 * (m12 * m44 - m42 * m14) + m41 * (m12 * m24 - m22 * m14));
    r[11] = -d * (m11 * (m22 * m34 - m32 * m24) - m21 * (m12 * m34 - m32 * m14) + m31 * (m12 * m24 - m22 * m14));
    r[12] = -d * (m21 * (m32 * m43 - m42 * m33) - m31 * (m22 * m43 - m42 * m23) + m41 * (m22 * m33 - m32 * m23));
    r[13] =  d * (m11 * (m32 * m43 - m42 * m33) - m31 * (m12 * m43 - m42 * m13) + m41 * (m12 * m33 - m32 * m13));
    r[14] = -d * (m11 * (m22 * m43 - m42 * m23) - m21 * (m12 * m43 - m42 * m13) + m41 * (m12 * m23 - m22 * m13));
    r[15] =  d * (m11 * (m22 * m33 - m32 * m23) - m21 * (m12 * m33 - m32 * m13) + m31 * (m12 * m23 - m22 * m13));
    for (i = 0; i < 16; i++) out[i] = (float)r[i];
    return 1;
}
static void m3_set(float *m, const float *src) { memcpy(m, src, 16 * sizeof(float)); }
static V2 m3_new(VM2 *vm, const float *src) { V2 o = a2_construct_class(vm, c_matrix3d, 0, NULL); M3 *m = o.t == T_OBJ ? m3_of(o.u.o) : NULL; if (m) m3_set(m->m, src); return o; }
/* for Transform.matrix3D */
V2 a2_m3_from_2d(VM2 *vm, const SWFMatrix *mx, double tz) { float r[16]; memset(r, 0, sizeof(r)); r[0] = (float)mx->sx; r[1] = (float)mx->r0; r[4] = (float)mx->r1; r[5] = (float)mx->sy; r[10] = 1; r[12] = (float)(mx->tx / 20.0); r[13] = (float)(mx->ty / 20.0); r[14] = (float)tz; r[15] = 1; return m3_new(vm, r); }
/* Transform.matrix3D = m: a Matrix3D belongs to one display object (0: owned by another) */
int a2_m3_claim(A2Obj *o, const void *target) { M3 *m = m3_of(o); if (!m) return 1; if (m->owner && m->owner != target) return 0; m->owner = target; return 1; }
int a2_m3_raw(A2Obj *o, float *out) { M3 *m = m3_of(o); if (!m) return 0; memcpy(out, m->m, 16 * sizeof(float)); return 1; }
static A2Obj *num_vector(VM2 *vm) { A2Obj *a = a2_new(vm, a2_vector_class_for(vm, vm->c_number)); a->kind = OK_VECTOR; a->vec_type = vm->c_number; return a; }
static int vec_len(V2 v) { return (v.t == T_OBJ && (v.u.o->kind == OK_VECTOR || v.u.o->kind == OK_ARRAY)) ? v.u.o->alen : 0; }
static double vec_at(VM2 *vm, V2 v, int i) { return (i < vec_len(v)) ? v2_tonum(vm, v.u.o->arr[i]) : 0.0 / 0.0; }

static V2 Matrix3D_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    M3 *m = m3_of(SELFO);
    if (m && argc > 0 && vec_len(ARG(0)) == 16) { int i; for (i = 0; i < 16; i++) m->m[i] = (float)vec_at(vm, ARG(0), i); }
    return v2_undef();
}
static V2 m3_rawData_get(VM2 *vm, V2 self, int argc, V2 *argv) { M3 *m = m3_of(SELFO); A2Obj *a = num_vector(vm); int i; UNUSED; for (i = 0; m && i < 16; i++) a2_arr_push(vm, a, v2_num(m->m[i])); return v2_obj(a); }
static V2 m3_rawData_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    M3 *m = m3_of(SELFO); int i;
    if (!m || vec_len(ARG(0)) != 16) return v2_undef();
    for (i = 0; i < 16; i++) m->m[i] = (float)vec_at(vm, ARG(0), i);
    return v2_undef();
}
static V2 m3_identity(VM2 *vm, V2 self, int argc, V2 *argv) { M3 *m = m3_of(SELFO); UNUSED; if (m) m3_set(m->m, M3_IDENT); return v2_undef(); }
static V2 m3_transpose_m(VM2 *vm, V2 self, int argc, V2 *argv) { M3 *m = m3_of(SELFO); UNUSED; if (m) m3_transpose(m->m); return v2_undef(); }
static V2 m3_determinant(VM2 *vm, V2 self, int argc, V2 *argv) { M3 *m = m3_of(SELFO); UNUSED; return v2_num(m ? m3_det(m->m) : 0); }
static V2 m3_invert_m(VM2 *vm, V2 self, int argc, V2 *argv) { M3 *m = m3_of(SELFO); float r[16]; UNUSED; if (!m || !m3_invert(m->m, r)) return v2_bool(0); m3_set(m->m, r); return v2_bool(1); }
static V2 m3_append(VM2 *vm, V2 self, int argc, V2 *argv) { M3 *m = m3_of(SELFO), *l = m3_arg(vm, ARG(0), "lhs"); if (m && l) m3_mul(l->m, m->m, m->m); return v2_undef(); }
static V2 m3_prepend(VM2 *vm, V2 self, int argc, V2 *argv) { M3 *m = m3_of(SELFO), *r = m3_arg(vm, ARG(0), "rhs"); if (m && r) m3_mul(m->m, r->m, m->m); return v2_undef(); }
static V2 m3_appendTranslation(VM2 *vm, V2 self, int argc, V2 *argv) {
    M3 *m = m3_of(SELFO); double x = NUM(0), y = NUM(1), z = NUM(2);
    if (m) { m->m[12] = F(m->m[12] + (float)x); m->m[13] = F(m->m[13] + (float)y); m->m[14] = F(m->m[14] + (float)z); }
    return v2_undef();
}
static V2 m3_prependTranslation(VM2 *vm, V2 self, int argc, V2 *argv) {
    M3 *m = m3_of(SELFO); float t[16];
    memcpy(t, M3_IDENT, sizeof(t)); t[12] = (float)NUM(0); t[13] = (float)NUM(1); t[14] = (float)NUM(2);
    if (m) m3_mul(m->m, t, m->m);
    return v2_undef();
}
static int scale_matrix(VM2 *vm, int argc, V2 *argv, float *t) {
    double x = NUM(0), y = NUM(1), z = NUM(2);
    if (x == 0 || y == 0 || z == 0) { a2_throw_id(vm, "ArgumentError", 2183, NULL, NULL); return 0; }
    memcpy(t, M3_IDENT, 16 * sizeof(float)); t[0] = (float)x; t[5] = (float)y; t[10] = (float)z;
    return 1;
}
static V2 m3_appendScale(VM2 *vm, V2 self, int argc, V2 *argv) { M3 *m = m3_of(SELFO); float t[16]; if (m && scale_matrix(vm, argc, argv, t)) m3_mul(t, m->m, m->m); return v2_undef(); }
static V2 m3_prependScale(VM2 *vm, V2 self, int argc, V2 *argv) { M3 *m = m3_of(SELFO); float t[16]; if (m && scale_matrix(vm, argc, argv, t)) m3_mul(m->m, t, m->m); return v2_undef(); }
static int rotation_matrix(VM2 *vm, int argc, V2 *argv, float *out) {
    double deg = NUM(0), rad, c, s, x, y, z, x2, y2, z2, ls, cc, tx = 0, ty = 0, tz = 0; V2 axis = ARG(1), piv = ARG(2); double r[16]; int i;
    if (!need_obj(vm, axis, "axis")) return 0;
    if (argc > 2 && piv.t == T_OBJ) { tx = vget(vm, piv, "x"); ty = vget(vm, piv, "y"); tz = vget(vm, piv, "z"); }
    rad = deg * M_PI / 180.0; c = cos(rad); s = sin(rad);
    x = vget(vm, axis, "x"); y = vget(vm, axis, "y"); z = vget(vm, axis, "z");
    x2 = x * x; y2 = y * y; z2 = z * z; ls = x2 + y2 + z2;
    if (ls != 0) { double l = sqrt(ls); x /= l; y /= l; z /= l; x2 /= ls; y2 /= ls; z2 /= ls; }
    cc = 1.0 - c;
    r[0] = x2 + (y2 + z2) * c; r[1] = x * y * cc + z * s; r[2] = x * z * cc - y * s; r[3] = 0;
    r[4] = x * y * cc - z * s; r[5] = y2 + (x2 + z2) * c; r[6] = y * z * cc + x * s; r[7] = 0;
    r[8] = x * z * cc + y * s; r[9] = y * z * cc - x * s; r[10] = z2 + (x2 + y2) * c; r[11] = 0;
    r[12] = (tx * (y2 + z2) - x * (ty * y + tz * z)) * cc + (ty * z - tz * y) * s;
    r[13] = (ty * (x2 + z2) - y * (tx * x + tz * z)) * cc + (tz * x - tx * z) * s;
    r[14] = (tz * (x2 + y2) - z * (tx * x + ty * y)) * cc + (tx * y - ty * x) * s;
    r[15] = 1;
    for (i = 0; i < 16; i++) out[i] = (float)r[i];
    return 1;
}
static V2 m3_appendRotation(VM2 *vm, V2 self, int argc, V2 *argv) { M3 *m = m3_of(SELFO); float r[16]; if (m && rotation_matrix(vm, argc, argv, r)) m3_mul(r, m->m, m->m); return v2_undef(); }
static V2 m3_prependRotation(VM2 *vm, V2 self, int argc, V2 *argv) { M3 *m = m3_of(SELFO); float r[16]; if (m && rotation_matrix(vm, argc, argv, r)) m3_mul(m->m, r, m->m); return v2_undef(); }

static V2 m3_transformVector(VM2 *vm, V2 self, int argc, V2 *argv) {
    M3 *m = m3_of(SELFO); V2 v = ARG(0); float x, y, z; const float *r;
    if (!need_obj(vm, v, "vector") || !m) return v2_undef();
    r = m->m; x = (float)vget(vm, v, "x"); y = (float)vget(vm, v, "y"); z = (float)vget(vm, v, "z");   /* the vector goes to single precision first */
    return v3_new(vm, F(F(F(F(r[0] * x) + F(r[4] * y)) + F(r[8] * z)) + r[12]), F(F(F(F(r[1] * x) + F(r[5] * y)) + F(r[9] * z)) + r[13]),
                  F(F(F(F(r[2] * x) + F(r[6] * y)) + F(r[10] * z)) + r[14]), F(F(F(F(r[3] * x) + F(r[7] * y)) + F(r[11] * z)) + r[15]));
}
static V2 m3_deltaTransformVector(VM2 *vm, V2 self, int argc, V2 *argv) {
    M3 *m = m3_of(SELFO); V2 v = ARG(0); float x, y, z; const float *r;
    if (!need_obj(vm, v, "vector") || !m) return v2_undef();
    r = m->m; x = (float)vget(vm, v, "x"); y = (float)vget(vm, v, "y"); z = (float)vget(vm, v, "z");
    return v3_new(vm, F(F(F(r[0] * x) + F(r[4] * y)) + F(r[8] * z)), F(F(F(r[1] * x) + F(r[5] * y)) + F(r[9] * z)),
                  F(F(F(r[2] * x) + F(r[6] * y)) + F(r[10] * z)), F(F(F(r[3] * x) + F(r[7] * y)) + F(r[11] * z)));
}
static void vec_resize(VM2 *vm, A2Obj *a, int n) { while (a->alen < n) a2_arr_push(vm, a, v2_num(0)); }
static V2 m3_transformVectors(VM2 *vm, V2 self, int argc, V2 *argv) {
    M3 *m = m3_of(SELFO); V2 vin = ARG(0), vout = ARG(1); int n, i; double r[16];
    if (!need_obj(vm, vin, "vin") || !need_obj(vm, vout, "vout") || !m) return v2_undef();
    n = (vec_len(vin) / 3) * 3;
    if (n > vout.u.o->alen) { if (vout.u.o->vec_fixed) { a2_throw_id(vm, "RangeError", 1126, NULL, NULL); return v2_undef(); } vec_resize(vm, vout.u.o, n); }
    for (i = 0; i < 16; i++) r[i] = m->m[i];   /* this one works in double precision */
    for (i = 0; i + 2 < n + 2 && i < n; i += 3) {
        double x = vec_at(vm, vin, i), y = vec_at(vm, vin, i + 1), z = vec_at(vm, vin, i + 2);
        vout.u.o->arr[i] = v2_num(r[0] * x + r[4] * y + r[8] * z + r[12]);
        vout.u.o->arr[i + 1] = v2_num(r[1] * x + r[5] * y + r[9] * z + r[13]);
        vout.u.o->arr[i + 2] = v2_num(r[2] * x + r[6] * y + r[10] * z + r[14]);
    }
    return v2_undef();
}
static V2 m3_position_get(VM2 *vm, V2 self, int argc, V2 *argv) { M3 *m = m3_of(SELFO); UNUSED; return m ? v3_new(vm, m->m[12], m->m[13], m->m[14], 0) : v2_null(); }
static V2 m3_position_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    M3 *m = m3_of(SELFO); V2 v = ARG(0);
    if (!m || v.t != T_OBJ) return v2_undef();
    m->m[12] = (float)vget(vm, v, "x"); m->m[13] = (float)vget(vm, v, "y"); m->m[14] = (float)vget(vm, v, "z");
    return v2_undef();
}
static int col_row_arg(VM2 *vm, int argc, V2 *argv, M3 **m, V2 self) {
    u32 i = v2_touint(vm, ARG(0));
    *m = m3_of(SELFO);
    if (!need_obj(vm, ARG(1), "vector3D")) return -1;
    if (i > 3) { a2_throw_id(vm, "ArgumentError", 2004, NULL, NULL); return -1; }
    return (int)i;
}
static V2 m3_copyColumnTo(VM2 *vm, V2 self, int argc, V2 *argv) { M3 *m; int c = col_row_arg(vm, argc, argv, &m, self); V2 v = ARG(1); if (c < 0 || !m) return v2_undef(); vset(vm, v, "x", m->m[c * 4]); vset(vm, v, "y", m->m[c * 4 + 1]); vset(vm, v, "z", m->m[c * 4 + 2]); vset(vm, v, "w", m->m[c * 4 + 3]); return v2_undef(); }
static V2 m3_copyColumnFrom(VM2 *vm, V2 self, int argc, V2 *argv) { M3 *m; int c = col_row_arg(vm, argc, argv, &m, self); V2 v = ARG(1); if (c < 0 || !m) return v2_undef(); m->m[c * 4] = (float)vget(vm, v, "x"); m->m[c * 4 + 1] = (float)vget(vm, v, "y"); m->m[c * 4 + 2] = (float)vget(vm, v, "z"); m->m[c * 4 + 3] = (float)vget(vm, v, "w"); return v2_undef(); }
static V2 m3_copyRowTo(VM2 *vm, V2 self, int argc, V2 *argv) { M3 *m; int r = col_row_arg(vm, argc, argv, &m, self); V2 v = ARG(1); if (r < 0 || !m) return v2_undef(); vset(vm, v, "x", m->m[r]); vset(vm, v, "y", m->m[r + 4]); vset(vm, v, "z", m->m[r + 8]); vset(vm, v, "w", m->m[r + 12]); return v2_undef(); }
static V2 m3_copyRowFrom(VM2 *vm, V2 self, int argc, V2 *argv) { M3 *m; int r = col_row_arg(vm, argc, argv, &m, self); V2 v = ARG(1); if (r < 0 || !m) return v2_undef(); m->m[r] = (float)vget(vm, v, "x"); m->m[r + 4] = (float)vget(vm, v, "y"); m->m[r + 8] = (float)vget(vm, v, "z"); m->m[r + 12] = (float)vget(vm, v, "w"); return v2_undef(); }
static V2 m3_copyFrom(VM2 *vm, V2 self, int argc, V2 *argv) { M3 *m = m3_of(SELFO), *s = m3_arg(vm, ARG(0), "source"); if (m && s) m3_set(m->m, s->m); return v2_undef(); }
static V2 m3_copyToMatrix3D(VM2 *vm, V2 self, int argc, V2 *argv) { M3 *m = m3_of(SELFO), *d = m3_arg(vm, ARG(0), "dest"); if (m && d) m3_set(d->m, m->m); return v2_undef(); }
static V2 m3_clone(VM2 *vm, V2 self, int argc, V2 *argv) { M3 *m = m3_of(SELFO); UNUSED; return m3_new(vm, m ? m->m : M3_IDENT); }
static V2 m3_copyRawDataFrom(VM2 *vm, V2 self, int argc, V2 *argv) {
    M3 *m = m3_of(SELFO); V2 src = ARG(0); u32 idx = argc > 1 ? v2_touint(vm, ARG(1)) : 0; int tr = argc > 2 && BOOL(2), i; float t[16];
    if (!need_obj(vm, src, "source") || !m) return v2_undef();
    if ((double)idx + 16 > vec_len(src)) { a2_throw_id(vm, "ArgumentError", 2004, NULL, NULL); return v2_undef(); }
    for (i = 0; i < 16; i++) t[i] = (float)vec_at(vm, src, (int)idx + i);
    if (tr) m3_transpose(t);
    m3_set(m->m, t);
    return v2_undef();
}
static V2 m3_copyRawDataTo(VM2 *vm, V2 self, int argc, V2 *argv) {
    M3 *m = m3_of(SELFO); V2 dst = ARG(0); u32 idx = argc > 1 ? v2_touint(vm, ARG(1)) : 0; int tr = argc > 2 && BOOL(2), i; float t[16];
    if (!need_obj(vm, dst, "dest") || !m) return v2_undef();
    if (dst.u.o->kind != OK_VECTOR) return v2_undef();
    if (dst.u.o->alen < (int)idx + 16) { if (dst.u.o->vec_fixed) { a2_throw_id(vm, "RangeError", 1126, NULL, NULL); return v2_undef(); } vec_resize(vm, dst.u.o, (int)idx + 16); }
    m3_set(t, m->m); if (tr) m3_transpose(t);
    for (i = 0; i < 16; i++) dst.u.o->arr[idx + i] = v2_num(t[i]);
    return v2_undef();
}
static int orientation(VM2 *vm, V2 v) {
    A2Str *s;
    if (v.t == T_NULL || v.t == T_UNDEF) { a2_throw_error(vm, "TypeError", "Parameter orientationStyle must be non-null.", 2007); return -1; }
    s = v2_tostr(vm, v);
    if (!strcmp(s->s, "eulerAngles")) return 0;
    if (!strcmp(s->s, "axisAngle")) return 1;
    if (!strcmp(s->s, "quaternion")) return 2;
    a2_throw_id(vm, "Error", 2187, s->s, NULL);
    return -1;
}
static V2 m3_decompose(VM2 *vm, V2 self, int argc, V2 *argv) {
    M3 *m = m3_of(SELFO); int o = orientation(vm, argc > 0 ? ARG(0) : v2_cstr(vm, "eulerAngles")), i; double r[16], sx, sy, sz, rot[4] = { 0, 0, 0, 0 }; A2Obj *vec;
    if (o < 0 || !m) return v2_undef();
    for (i = 0; i < 16; i++) r[i] = m->m[i];
    /* Gram-Schmidt in single precision (Flash takes shear out of the basis):
       scale x, then y and z after removing their components along the earlier axes */
    {
        float c0[3], c1[3], c2[3], d; int k;
        for (k = 0; k < 3; k++) { c0[k] = m->m[k]; c1[k] = m->m[4 + k]; c2[k] = m->m[8 + k]; }
        sx = F(sqrt(F(F(F(c0[0] * c0[0]) + F(c0[1] * c0[1])) + F(c0[2] * c0[2]))));
        for (k = 0; k < 3; k++) c0[k] = F(c0[k] / sx);
        d = F(F(F(c0[0] * c1[0]) + F(c0[1] * c1[1])) + F(c0[2] * c1[2]));
        for (k = 0; k < 3; k++) c1[k] = F(c1[k] - F(c0[k] * d));
        sy = F(sqrt(F(F(F(c1[0] * c1[0]) + F(c1[1] * c1[1])) + F(c1[2] * c1[2]))));
        for (k = 0; k < 3; k++) c1[k] = F(c1[k] / sy);
        d = F(F(F(c0[0] * c2[0]) + F(c0[1] * c2[1])) + F(c0[2] * c2[2]));
        for (k = 0; k < 3; k++) c2[k] = F(c2[k] - F(c0[k] * d));
        d = F(F(F(c1[0] * c2[0]) + F(c1[1] * c2[1])) + F(c1[2] * c2[2]));
        for (k = 0; k < 3; k++) c2[k] = F(c2[k] - F(c1[k] * d));
        sz = F(sqrt(F(F(F(c2[0] * c2[0]) + F(c2[1] * c2[1])) + F(c2[2] * c2[2]))));
        if (r[0] * (r[5] * r[10] - r[6] * r[9]) - r[1] * (r[4] * r[10] - r[6] * r[8]) + r[2] * (r[4] * r[9] - r[5] * r[8]) < 0) sz = -sz;
        for (k = 0; k < 3; k++) c2[k] = F(c2[k] / sz);
        for (k = 0; k < 3; k++) { r[k] = c0[k]; r[4 + k] = c1[k]; r[8 + k] = c2[k]; }
    }
    if (o == 1) {
        double len;
        rot[3] = acos((r[0] + r[5] + r[10] - 1) / 2);
        len = sqrt((r[6] - r[9]) * (r[6] - r[9]) + (r[8] - r[2]) * (r[8] - r[2]) + (r[1] - r[4]) * (r[1] - r[4]));
        if (len != 0) { rot[0] = (r[6] - r[9]) / len; rot[1] = (r[8] - r[2]) / len; rot[2] = (r[1] - r[4]) / len; }
    } else if (o == 2) {
        double tr = r[0] + r[5] + r[10];
        if (tr > 0) { rot[3] = sqrt(1 + tr) / 2; rot[0] = (r[6] - r[9]) / (4 * rot[3]); rot[1] = (r[8] - r[2]) / (4 * rot[3]); rot[2] = (r[1] - r[4]) / (4 * rot[3]); }
        else if (r[0] > r[5] && r[0] > r[10]) { rot[0] = sqrt(1 + r[0] - r[5] - r[10]) / 2; rot[3] = (r[6] - r[9]) / (4 * rot[0]); rot[1] = (r[1] + r[4]) / (4 * rot[0]); rot[2] = (r[8] + r[2]) / (4 * rot[0]); }
        else if (r[5] > r[10]) { rot[1] = sqrt(1 + r[5] - r[0] - r[10]) / 2; rot[0] = (r[1] + r[4]) / (4 * rot[1]); rot[3] = (r[8] - r[2]) / (4 * rot[1]); rot[2] = (r[6] + r[9]) / (4 * rot[1]); }
        else { rot[2] = sqrt(1 + r[10] - r[0] - r[5]) / 2; rot[0] = (r[8] + r[2]) / (4 * rot[2]); rot[1] = (r[6] + r[9]) / (4 * rot[2]); rot[3] = (r[1] - r[4]) / (4 * rot[2]); }
    } else {
        rot[1] = asin(-r[2]);
        if (r[2] != 1 && r[2] != -1) { rot[0] = atan2(r[6], r[10]); rot[2] = atan2(r[1], r[0]); }
        else { rot[2] = 0; rot[0] = atan2(r[4], r[5]); }
    }
    vec = a2_new(vm, a2_vector_class_for(vm, c_vector3d)); vec->kind = OK_VECTOR; vec->vec_type = c_vector3d;
    a2_arr_push(vm, vec, v3_new(vm, r[12], r[13], r[14], 0));
    a2_arr_push(vm, vec, v3_new(vm, (float)rot[0], (float)rot[1], (float)rot[2], (float)rot[3]));
    a2_arr_push(vm, vec, v3_new(vm, sx, sy, sz, 0));
    return v2_obj(vec);
}
static V2 m3_recompose(VM2 *vm, V2 self, int argc, V2 *argv) {
    M3 *m = m3_of(SELFO); V2 comps = ARG(0); int o, i; V2 t, r, s; double rx, ry, rz, rw, rm[16]; float tm[16], rotm[16], sm[16], res[16];
    if (!need_obj(vm, comps, "components") || !m) return v2_undef();
    o = orientation(vm, argc > 1 ? ARG(1) : v2_cstr(vm, "eulerAngles"));
    if (o < 0) return v2_undef();
    if (vec_len(comps) < 3) return v2_bool(0);
    t = comps.u.o->arr[0]; r = comps.u.o->arr[1]; s = comps.u.o->arr[2];
    if (t.t != T_OBJ || r.t != T_OBJ || s.t != T_OBJ) return v2_bool(0);   /* a null component: nothing changes */
    rx = vget(vm, r, "x"); ry = vget(vm, r, "y"); rz = vget(vm, r, "z"); rw = vget(vm, r, "w");
    for (i = 0; i < 16; i++) rm[i] = 0;
    if (o == 0) {
        double cx = cos(rx), cy = cos(ry), cz = cos(rz), sx = sin(rx), sy = sin(ry), sz = sin(rz);
        rm[0] = cy * cz; rm[1] = cy * sz; rm[2] = -sy;
        rm[4] = sx * sy * cz - cx * sz; rm[5] = sx * sy * sz + cx * cz; rm[6] = sx * cy;
        rm[8] = cx * sy * cz + sx * sz; rm[9] = cx * sy * sz - sx * cz; rm[10] = cx * cy;
    } else {
        double x = rx, y = ry, z = rz, w = rw;
        if (o == 1) { x *= sin(w / 2); y *= sin(w / 2); z *= sin(w / 2); w = cos(w / 2); }
        rm[0] = 1 - 2 * y * y - 2 * z * z; rm[1] = 2 * x * y + 2 * w * z; rm[2] = 2 * x * z - 2 * w * y;
        rm[4] = 2 * x * y - 2 * w * z; rm[5] = 1 - 2 * x * x - 2 * z * z; rm[6] = 2 * y * z + 2 * w * x;
        rm[8] = 2 * x * z + 2 * w * y; rm[9] = 2 * y * z - 2 * w * x; rm[10] = 1 - 2 * x * x - 2 * y * y;
    }
    rm[15] = 1;
    for (i = 0; i < 16; i++) rotm[i] = (float)rm[i];
    memcpy(tm, M3_IDENT, sizeof(tm)); tm[12] = (float)vget(vm, t, "x"); tm[13] = (float)vget(vm, t, "y"); tm[14] = (float)vget(vm, t, "z");
    memcpy(sm, M3_IDENT, sizeof(sm)); sm[0] = (float)vget(vm, s, "x"); sm[5] = (float)vget(vm, s, "y"); sm[10] = (float)vget(vm, s, "z");
    m3_mul(rotm, sm, res); m3_mul(tm, res, res);   /* translation * (rotation * scale) */
    m3_set(m->m, res);
    return v2_bool(1);
}
/* normalized rotation quaternion of the upper 3x3 (scale left in) */
static void quat_of(const float *mf, double *q) {
    double m00 = mf[0], m10 = mf[1], m20 = mf[2], m01 = mf[4], m11 = mf[5], m21 = mf[6], m02 = mf[8], m12 = mf[9], m22 = mf[10];
    double x, y, z, w, s, tr = m00 + m11 + m22, len;
    if (tr > 0) { s = sqrt(tr + 1) * 2; w = 0.25 * s; x = (m21 - m12) / s; y = (m02 - m20) / s; z = (m10 - m01) / s; }
    else if (m00 > m11 && m00 > m22) { s = sqrt(1 + m00 - m11 - m22) * 2; w = (m21 - m12) / s; x = 0.25 * s; y = (m01 + m10) / s; z = (m02 + m20) / s; }
    else if (m11 > m22) { s = sqrt(1 + m11 - m00 - m22) * 2; w = (m02 - m20) / s; x = (m01 + m10) / s; y = 0.25 * s; z = (m12 + m21) / s; }
    else { s = sqrt(1 + m22 - m00 - m11) * 2; w = (m10 - m01) / s; x = (m02 + m20) / s; y = (m12 + m21) / s; z = 0.25 * s; }
    len = sqrt(x * x + y * y + z * z + w * w);
    if (len == 0) { q[0] = q[1] = q[2] = 0; q[3] = 1; return; }
    q[0] = x / len; q[1] = y / len; q[2] = z / len; q[3] = w / len;
}
static void m3_interp(const float *a, const float *b, double p, float *out) {
    double q0[4], q1[4], dot, k0, k1, r[4], len, x1, y1, z1, w1, rm[16]; float rotm[16], tm[16]; int i;
    quat_of(a, q0); quat_of(b, q1);
    dot = q0[0] * q1[0] + q0[1] * q1[1] + q0[2] * q1[2] + q0[3] * q1[3];
    x1 = q1[0]; y1 = q1[1]; z1 = q1[2]; w1 = q1[3];
    if (dot < 0) { dot = -dot; x1 = -x1; y1 = -y1; z1 = -z1; w1 = -w1; }
    if (dot > 0.9995) { k0 = 1 - p; k1 = p; }
    else { double th = acos(dot), st = sin(th); k0 = sin((1 - p) * th) / st; k1 = sin(p * th) / st; }
    r[0] = q0[0] * k0 + x1 * k1; r[1] = q0[1] * k0 + y1 * k1; r[2] = q0[2] * k0 + z1 * k1; r[3] = q0[3] * k0 + w1 * k1;
    len = sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
    if (len == 0) { r[0] = r[1] = r[2] = 0; r[3] = 1; len = 1; }
    for (i = 0; i < 4; i++) r[i] /= len;
    for (i = 0; i < 16; i++) rm[i] = 0;
    { double x = r[0], y = r[1], z = r[2], w = r[3];
      rm[0] = 1 - 2 * y * y - 2 * z * z; rm[1] = 2 * x * y + 2 * w * z; rm[2] = 2 * x * z - 2 * w * y;
      rm[4] = 2 * x * y - 2 * w * z; rm[5] = 1 - 2 * x * x - 2 * z * z; rm[6] = 2 * y * z + 2 * w * x;
      rm[8] = 2 * x * z + 2 * w * y; rm[9] = 2 * y * z - 2 * w * x; rm[10] = 1 - 2 * x * x - 2 * y * y; rm[15] = 1; }
    for (i = 0; i < 16; i++) rotm[i] = (float)rm[i];
    memcpy(tm, M3_IDENT, sizeof(tm));
    tm[12] = (float)(a[12] + (b[12] - (double)a[12]) * p); tm[13] = (float)(a[13] + (b[13] - (double)a[13]) * p); tm[14] = (float)(a[14] + (b[14] - (double)a[14]) * p);
    m3_mul(tm, rotm, out);   /* the scale is dropped */
}
static V2 m3_interpolate_s(VM2 *vm, V2 self, int argc, V2 *argv) {
    M3 *a, *b; float r[16]; (void)self;
    a = m3_arg(vm, ARG(0), "fromMat"); if (!a) return v2_undef();
    b = m3_arg(vm, ARG(1), "toMat"); if (!b) return v2_undef();
    m3_interp(a->m, b->m, NUM(2), r);
    return m3_new(vm, r);
}
static V2 m3_interpolateTo(VM2 *vm, V2 self, int argc, V2 *argv) {
    M3 *m = m3_of(SELFO), *b = m3_arg(vm, ARG(0), "toMat"); float r[16];
    if (!m || !b) return v2_undef();
    m3_interp(m->m, b->m, NUM(1), r); m3_set(m->m, r);
    return v2_undef();
}
static V2 m3_pointAt(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_undef(); }

/* ---------------------------------------------------------------- */
/* Utils3D                                                          */

static V2 u3_projectVector(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 a[1], p; double w; (void)self;
    if (!m3_arg(vm, ARG(0), "m")) return v2_undef();
    a[0] = ARG(1); p = m3_transformVector(vm, ARG(0), 1, a);
    if (p.t != T_OBJ) return p;
    w = vget(vm, p, "w"); vset(vm, p, "x", vget(vm, p, "x") / w); vset(vm, p, "y", vget(vm, p, "y") / w); vset(vm, p, "z", vget(vm, p, "z") / w);
    return p;
}
static V2 u3_projectVectors(VM2 *vm, V2 self, int argc, V2 *argv) {
    M3 *m = m3_arg(vm, ARG(0), "m"); V2 verts = ARG(1), proj = ARG(2), uvts = ARG(3); int n, i = 0, j = 0; double r[16];
    (void)self;
    if (!m || !need_obj(vm, verts, "verts") || !need_obj(vm, proj, "projectedVerts") || !need_obj(vm, uvts, "uvts")) return v2_undef();
    for (n = 0; n < 16; n++) r[n] = m->m[n];
    n = vec_len(verts);
    if (uvts.u.o->alen < n) vec_resize(vm, uvts.u.o, n);
    if (proj.u.o->alen < (n / 3) * 2) vec_resize(vm, proj.u.o, (n / 3) * 2);
    while (i + 2 < n) {
        double x = vec_at(vm, verts, i), y = vec_at(vm, verts, i + 1), z = vec_at(vm, verts, i + 2);
        double x1 = x * r[0] + y * r[4] + z * r[8] + r[12], y1 = x * r[1] + y * r[5] + z * r[9] + r[13], w1 = x * r[3] + y * r[7] + z * r[11] + r[15];
        proj.u.o->arr[j] = v2_num(x1 / w1); proj.u.o->arr[j + 1] = v2_num(y1 / w1);
        uvts.u.o->arr[i + 2] = v2_num(1 / w1);
        i += 3; j += 2;
    }
    return v2_undef();
}
static V2 u3_pointTowards(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; return argc > 1 ? ARG(1) : v2_null(); }

/* ---------------------------------------------------------------- */
/* PerspectiveProjection (not attached to a display object: 500px wide) */

/* A PerspectiveProjection read from a display object's transform stays linked
   to it: its values live on the display object (as the "__fov"/"__center"
   hidden properties), so every linked copy sees changes made through another. */
static A2Obj *pp_store(VM2 *vm, V2 self) { A2Prop *p = SELFO ? a2_dyn_find(SELFO, a2_str(vm, "__do")) : NULL; return (p && p->v.t == T_OBJ) ? p->v.u.o : SELFO; }
static double pp_width(VM2 *vm, V2 self) { A2Prop *p = SELFO ? a2_dyn_find(SELFO, a2_str(vm, "__w")) : NULL; return p ? v2_tonum(vm, p->v) : 500.0; }
static double pp_fov(VM2 *vm, V2 self) { A2Obj *st = pp_store(vm, self); A2Prop *p = st ? a2_dyn_find(st, a2_str(vm, "__fov")) : NULL; return p ? v2_tonum(vm, p->v) : 55.0; }
static float pp_focal(double fov, float width) { double rad = fov * M_PI / 180.0; return F((width / 2.0f) * (float)tan((M_PI - rad) / 2.0)); }
static V2 PerspectiveProjection_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    extern A2Obj *a2_point(VM2 *vm, double x, double y); UNUSED;
    if (SELFO) { a2_dyn_set_hidden(vm, SELFO, a2_str(vm, "__fov"), v2_num(55.0)); a2_dyn_set_hidden(vm, SELFO, a2_str(vm, "__center"), v2_obj(a2_point(vm, 250, 250))); }
    return v2_undef();
}
static V2 pp_fieldOfView_get(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_num(pp_fov(vm, self)); }
static V2 pp_fieldOfView_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    double f = NUM(0); A2Obj *st = pp_store(vm, self);
    if (f <= 0 || f >= 180) { a2_throw_id(vm, "ArgumentError", 2182, NULL, NULL); return v2_undef(); }
    if (st) a2_dyn_set_hidden(vm, st, a2_str(vm, "__fov"), v2_num(f));
    return v2_undef();
}
static V2 pp_focalLength_get(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_num(pp_focal(pp_fov(vm, self), (float)pp_width(vm, self))); }
static V2 pp_focalLength_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    double fl = NUM(0); A2Obj *st = pp_store(vm, self);
    if (fl <= 0) { char b[64]; sprintf(b, "%g", fl); a2_throw_id(vm, "ArgumentError", 2186, b, NULL); return v2_undef(); }
    if (st) a2_dyn_set_hidden(vm, st, a2_str(vm, "__fov"), v2_num(atan((pp_width(vm, self) / 2.0) / fl) * 360.0 / M_PI));
    return v2_undef();
}
static V2 pp_center_of(VM2 *vm, V2 self) { A2Obj *st = pp_store(vm, self); A2Prop *p = st ? a2_dyn_find(st, a2_str(vm, "__center")) : NULL; return p ? p->v : v2_null(); }
static V2 pp_projectionCenter_get(VM2 *vm, V2 self, int argc, V2 *argv) {
    extern A2Obj *a2_point(VM2 *vm, double x, double y); V2 c = pp_center_of(vm, self); UNUSED;
    if (c.t == T_OBJ) return v2_obj(a2_point(vm, vget(vm, c, "x"), vget(vm, c, "y")));
    return v2_obj(a2_point(vm, 250, 250));
}
static V2 pp_projectionCenter_set(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *st = pp_store(vm, self); if (st) a2_dyn_set_hidden(vm, st, a2_str(vm, "__center"), ARG(0)); return v2_undef(); }

/* Transform.perspectiveProjection: the stage and the root always have one */
V2 a2_transform_pp_get(VM2 *vm, A2Obj *t) {
    extern A2Obj *a2_point(VM2 *vm, double x, double y); A2Class *pc = a2_class_find_s(vm, "flash.geom::PerspectiveProjection"); V2 pp; A2Prop *on;
    SWFRect *sr = &vm->player->movie->stage; double sw = (sr->xmax - sr->xmin) / 20.0, sh = (sr->ymax - sr->ymin) / 20.0;
    int st = a2_disp(t)->is_stage, rt = a2_disp(t)->is_root;
    on = a2_dyn_find(t, a2_str(vm, "__ppOn"));
    if (!st && !rt && !(on && v2_tobool(vm, on->v))) return v2_null();
    if (!pc) return v2_null();
    if (!a2_dyn_find(t, a2_str(vm, "__fov"))) {   /* first use: the defaults */
        a2_dyn_set_hidden(vm, t, a2_str(vm, "__fov"), v2_num(55.0));
        a2_dyn_set_hidden(vm, t, a2_str(vm, "__center"), v2_obj(rt ? a2_point(vm, sw / 2, sh / 2) : a2_point(vm, 250, 250)));
    }
    pp = a2_construct_class(vm, pc, 0, NULL);
    if (pp.t != T_OBJ) return pp;
    a2_dyn_set_hidden(vm, pp.u.o, a2_str(vm, "__do"), v2_obj(t));
    a2_dyn_set_hidden(vm, pp.u.o, a2_str(vm, "__w"), v2_num(st ? 500.0 : sw));
    return pp;
}
void a2_transform_pp_set(VM2 *vm, A2Obj *t, V2 v) {
    int fixed = a2_disp(t)->is_stage || a2_disp(t)->is_root;
    if (v.t != T_OBJ) {   /* null: the stage and the root go back to the defaults, others lose theirs */
        if (fixed) a2_dyn_del(t, a2_str(vm, "__fov")); else a2_dyn_set_hidden(vm, t, a2_str(vm, "__ppOn"), v2_bool(0));
        return;
    }
    a2_dyn_set_hidden(vm, t, a2_str(vm, "__ppOn"), v2_bool(1));
    if (fixed) {   /* the stage's and the root's travel as a single precision focal length */
        double w = pp_width(vm, v), fl = (w / 2.0) * tan((M_PI - pp_fov(vm, v) * M_PI / 180.0) / 2.0);
        a2_dyn_set_hidden(vm, t, a2_str(vm, "__fov"), v2_num(atan((w / 2.0) / fl) * 360.0 / M_PI));
    } else a2_dyn_set_hidden(vm, t, a2_str(vm, "__fov"), v2_num(pp_fov(vm, v)));
    { V2 c = pp_center_of(vm, v); extern A2Obj *a2_point(VM2 *vm, double x, double y); a2_dyn_set_hidden(vm, t, a2_str(vm, "__center"), c.t == T_OBJ ? v2_obj(a2_point(vm, vget(vm, c, "x"), vget(vm, c, "y"))) : v2_obj(a2_point(vm, 250, 250))); }
}
static V2 pp_toMatrix3D(VM2 *vm, V2 self, int argc, V2 *argv) {
    float m[16], fl = pp_focal(pp_fov(vm, self), (float)pp_width(vm, self)); UNUSED;
    memset(m, 0, sizeof(m)); m[0] = fl; m[5] = fl; m[10] = 1; m[11] = 1;
    return m3_new(vm, m);
}

/* ---------------------------------------------------------------- */

void a2_geom3d_init(VM2 *vm) {
    A2Class *c;
    c_vector3d = c = a2_def_class(vm, "flash.geom", "Vector3D", vm->c_object, OK_PLAIN, Vector3D_ctor);
    a2_add_slot(vm, c, "w", vm->c_number); a2_add_slot(vm, c, "x", vm->c_number); a2_add_slot(vm, c, "y", vm->c_number); a2_add_slot(vm, c, "z", vm->c_number);
    a2_add_getter(vm, c, "length", v3_length, NULL); a2_add_getter(vm, c, "lengthSquared", v3_lengthSquared, NULL);
    { struct { const char *n; NativeFn2 f; int a; } mm[] = { { "toString", v3_toString, 0 }, { "add", v3_add, 1 }, { "subtract", v3_subtract, 1 }, { "incrementBy", v3_incrementBy, 1 }, { "decrementBy", v3_decrementBy, 1 }, { "clone", v3_clone, 0 }, { "copyFrom", v3_copyFrom, 1 }, { "equals", v3_equals, 2 }, { "nearEquals", v3_nearEquals, 3 }, { "setTo", v3_setTo, 3 }, { "scaleBy", v3_scaleBy, 1 }, { "negate", v3_negate, 0 }, { "project", v3_project, 0 }, { "normalize", v3_normalize, 0 }, { "dotProduct", v3_dotProduct, 1 }, { "crossProduct", v3_crossProduct, 1 }, { NULL, NULL, 0 } };
      int i; for (i = 0; mm[i].n; i++) a2_add_method(vm, c, mm[i].n, mm[i].f, mm[i].a); }
    a2_add_static(vm, c, "angleBetween", v3_angleBetween, 2); a2_add_static(vm, c, "distance", v3_distance, 2);
    c->ctor_sig = "(Number=,Number=,Number=,Number=)";
    a2_add_const(vm, c, "Z_AXIS", v3_new(vm, 0, 0, 1, 0)); a2_add_const(vm, c, "X_AXIS", v3_new(vm, 1, 0, 0, 0)); a2_add_const(vm, c, "Y_AXIS", v3_new(vm, 0, 1, 0, 0));

    c_matrix3d = c = a2_def_class(vm, "flash.geom", "Matrix3D", vm->c_object, OK_PLAIN, Matrix3D_ctor);
    c->ctor_sig = "(__AS3__.vec::Vector.<Number>=)";
    a2_add_getter(vm, c, "rawData", m3_rawData_get, m3_rawData_set); a2_add_getter(vm, c, "position", m3_position_get, m3_position_set); a2_add_getter(vm, c, "determinant", m3_determinant, NULL);
    { struct { const char *n; NativeFn2 f; int a; } mm[] = { { "identity", m3_identity, 0 }, { "transpose", m3_transpose_m, 0 }, { "invert", m3_invert_m, 0 }, { "append", m3_append, 1 }, { "prepend", m3_prepend, 1 },
        { "appendTranslation", m3_appendTranslation, 3 }, { "prependTranslation", m3_prependTranslation, 3 }, { "appendScale", m3_appendScale, 3 }, { "prependScale", m3_prependScale, 3 },
        { "appendRotation", m3_appendRotation, 3 }, { "prependRotation", m3_prependRotation, 3 }, { "transformVector", m3_transformVector, 1 }, { "deltaTransformVector", m3_deltaTransformVector, 1 },
        { "transformVectors", m3_transformVectors, 2 }, { "copyColumnTo", m3_copyColumnTo, 2 }, { "copyColumnFrom", m3_copyColumnFrom, 2 }, { "copyRowTo", m3_copyRowTo, 2 }, { "copyRowFrom", m3_copyRowFrom, 2 },
        { "copyFrom", m3_copyFrom, 1 }, { "copyToMatrix3D", m3_copyToMatrix3D, 1 }, { "clone", m3_clone, 0 }, { "copyRawDataFrom", m3_copyRawDataFrom, 3 }, { "copyRawDataTo", m3_copyRawDataTo, 3 },
        { "decompose", m3_decompose, 1 }, { "recompose", m3_recompose, 2 }, { "interpolateTo", m3_interpolateTo, 2 }, { "pointAt", m3_pointAt, 3 }, { NULL, NULL, 0 } };
      int i; for (i = 0; mm[i].n; i++) a2_add_method(vm, c, mm[i].n, mm[i].f, mm[i].a); }
    a2_add_static(vm, c, "interpolate", m3_interpolate_s, 3);

    c = a2_def_class(vm, "flash.geom", "Utils3D", vm->c_object, OK_PLAIN, NULL); c->flags |= CF_NOINST;
    a2_add_static(vm, c, "projectVector", u3_projectVector, 2); a2_add_static(vm, c, "projectVectors", u3_projectVectors, 4); a2_add_static(vm, c, "pointTowards", u3_pointTowards, 5);

    c = a2_def_class(vm, "flash.geom", "PerspectiveProjection", vm->c_object, OK_PLAIN, PerspectiveProjection_ctor);
    a2_add_getter(vm, c, "fieldOfView", pp_fieldOfView_get, pp_fieldOfView_set); a2_add_getter(vm, c, "focalLength", pp_focalLength_get, pp_focalLength_set);
    a2_add_getter(vm, c, "projectionCenter", pp_projectionCenter_get, pp_projectionCenter_set); a2_add_method(vm, c, "toMatrix3D", pp_toMatrix3D, 0);

    c = a2_def_class(vm, "flash.geom", "Orientation3D", vm->c_object, OK_PLAIN, NULL); c->flags |= CF_NOINST;
    a2_const_str(vm, c, "EULER_ANGLES", "eulerAngles"); a2_const_str(vm, c, "AXIS_ANGLE", "axisAngle"); a2_const_str(vm, c, "QUATERNION", "quaternion");
}
