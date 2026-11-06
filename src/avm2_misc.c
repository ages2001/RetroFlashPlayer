/* avm2_misc.c - flash.geom, flash.media (Video, Camera; Sound is in avm2_sound.c), flash.display.BitmapData,
 * flash.net / flash.system / flash.ui / flash.external / filters and the
 * remaining stub classes needed so that typical content links and runs. */
#include "avm2_flash_int.h"
#include "image.h"
#include "textfield.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

static V2 dget(VM2 *vm, V2 self, const char *k) { A2Prop *p = self.t == T_OBJ ? a2_dyn_find(self.u.o, a2_str(vm, k)) : NULL; return p ? p->v : v2_undef(); }
static double dnum(VM2 *vm, V2 self, const char *k) { V2 v = dget(vm, self, k); return v.t == T_UNDEF ? 0 : v2_tonum(vm, v); }
static void dset(VM2 *vm, V2 self, const char *k, V2 v) { if (self.t == T_OBJ) a2_dyn_set(vm, self.u.o, a2_str(vm, k), v); }
static V2 r_noop(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_undef(); }
static V2 r_false(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_bool(0); }
static V2 r_true(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_bool(1); }
static V2 r_null(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_null(); }
static V2 r_zero(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_int(0); }
static V2 r_empty(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_str(vm->s_empty); }
static V2 r_self(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return self; }
static V2 r_dyn_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_undef(); }

/* ---------------------------------------------------------------- */
/* geom                                                             */

#define NUMF(name) static V2 g_##name##_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return v2_num(dnum(vm, self, #name)); } static V2 g_##name##_set(VM2 *vm, V2 self, int argc, V2 *argv) { dset(vm, self, #name, v2_num(NUM(0))); return v2_undef(); }
NUMF(x) NUMF(y) NUMF(width) NUMF(height) NUMF(a) NUMF(b) NUMF(c) NUMF(d) NUMF(tx) NUMF(ty)
NUMF(redMultiplier) NUMF(greenMultiplier) NUMF(blueMultiplier) NUMF(alphaMultiplier) NUMF(redOffset) NUMF(greenOffset) NUMF(blueOffset) NUMF(alphaOffset)

A2Obj *a2_point(VM2 *vm, double x, double y) { A2Obj *o = a2_new(vm, vm->c_point); o->kind = OK_POINT; dset(vm, v2_obj(o), "x", v2_num(x)); dset(vm, v2_obj(o), "y", v2_num(y)); return o; }
A2Obj *a2_rect(VM2 *vm, double x, double y, double w, double h) { A2Obj *o = a2_new(vm, vm->c_rectangle); o->kind = OK_RECT; dset(vm, v2_obj(o), "x", v2_num(x)); dset(vm, v2_obj(o), "y", v2_num(y)); dset(vm, v2_obj(o), "width", v2_num(w)); dset(vm, v2_obj(o), "height", v2_num(h)); return o; }
A2Obj *a2_matrix_obj(VM2 *vm, const SWFMatrix *m) { A2Obj *o = a2_new(vm, vm->c_matrix); o->kind = OK_MATRIX; dset(vm, v2_obj(o), "a", v2_num(m->sx)); dset(vm, v2_obj(o), "b", v2_num(m->r0)); dset(vm, v2_obj(o), "c", v2_num(m->r1)); dset(vm, v2_obj(o), "d", v2_num(m->sy)); dset(vm, v2_obj(o), "tx", v2_num(m->tx / 20)); dset(vm, v2_obj(o), "ty", v2_num(m->ty / 20)); return o; }
void a2_matrix_from_obj(VM2 *vm, V2 mo, SWFMatrix *m) { m->sx = dnum(vm, mo, "a"); m->r0 = dnum(vm, mo, "b"); m->r1 = dnum(vm, mo, "c"); m->sy = dnum(vm, mo, "d"); m->tx = dnum(vm, mo, "tx") * 20; m->ty = dnum(vm, mo, "ty") * 20; }

static V2 Point_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { if (SELFO) SELFO->kind = OK_POINT; dset(vm, self, "x", v2_num(argc > 0 ? NUM(0) : 0)); dset(vm, self, "y", v2_num(argc > 1 ? NUM(1) : 0)); return v2_undef(); }
static V2 pt_length(VM2 *vm, V2 self, int argc, V2 *argv) { double x = dnum(vm, self, "x"), y = dnum(vm, self, "y"); (void)argc; (void)argv; return v2_num(sqrt(x * x + y * y)); }
static V2 pt_add(VM2 *vm, V2 self, int argc, V2 *argv) { return v2_obj(a2_point(vm, dnum(vm, self, "x") + dnum(vm, ARG(0), "x"), dnum(vm, self, "y") + dnum(vm, ARG(0), "y"))); }
static V2 pt_subtract(VM2 *vm, V2 self, int argc, V2 *argv) { return v2_obj(a2_point(vm, dnum(vm, self, "x") - dnum(vm, ARG(0), "x"), dnum(vm, self, "y") - dnum(vm, ARG(0), "y"))); }
static V2 pt_clone(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return v2_obj(a2_point(vm, dnum(vm, self, "x"), dnum(vm, self, "y"))); }
static V2 pt_equals(VM2 *vm, V2 self, int argc, V2 *argv) { return v2_bool(ARG(0).t == T_OBJ && dnum(vm, self, "x") == dnum(vm, ARG(0), "x") && dnum(vm, self, "y") == dnum(vm, ARG(0), "y")); }
static V2 pt_normalize(VM2 *vm, V2 self, int argc, V2 *argv) { double x = dnum(vm, self, "x"), y = dnum(vm, self, "y"), l = sqrt(x * x + y * y), t = NUM(0); if (l > 0) { dset(vm, self, "x", v2_num(x * t / l)); dset(vm, self, "y", v2_num(y * t / l)); } return v2_undef(); }
static V2 pt_offset(VM2 *vm, V2 self, int argc, V2 *argv) { dset(vm, self, "x", v2_num(dnum(vm, self, "x") + NUM(0))); dset(vm, self, "y", v2_num(dnum(vm, self, "y") + NUM(1))); return v2_undef(); }
static V2 pt_setTo(VM2 *vm, V2 self, int argc, V2 *argv) { dset(vm, self, "x", v2_num(NUM(0))); dset(vm, self, "y", v2_num(NUM(1))); return v2_undef(); }
static V2 pt_copyFrom(VM2 *vm, V2 self, int argc, V2 *argv) { dset(vm, self, "x", v2_num(dnum(vm, ARG(0), "x"))); dset(vm, self, "y", v2_num(dnum(vm, ARG(0), "y"))); return v2_undef(); }
static V2 pt_toString(VM2 *vm, V2 self, int argc, V2 *argv) { char b[80], x[40], y[40]; (void)argc; (void)argv; sprintf(b, "(x=%s, y=%s)", v2_num_to_cstr(dnum(vm, self, "x"), x), v2_num_to_cstr(dnum(vm, self, "y"), y)); return v2_cstr(vm, b); }
static V2 pt_distance(VM2 *vm, V2 self, int argc, V2 *argv) { double dx = dnum(vm, ARG(0), "x") - dnum(vm, ARG(1), "x"), dy = dnum(vm, ARG(0), "y") - dnum(vm, ARG(1), "y"); (void)self; return v2_num(sqrt(dx * dx + dy * dy)); }
static V2 pt_interpolate(VM2 *vm, V2 self, int argc, V2 *argv) { double f = NUM(2); (void)self; return v2_obj(a2_point(vm, dnum(vm, ARG(1), "x") + (dnum(vm, ARG(0), "x") - dnum(vm, ARG(1), "x")) * f, dnum(vm, ARG(1), "y") + (dnum(vm, ARG(0), "y") - dnum(vm, ARG(1), "y")) * f)); }
static V2 pt_polar(VM2 *vm, V2 self, int argc, V2 *argv) { double l = NUM(0), a = NUM(1); (void)self; return v2_obj(a2_point(vm, l * cos(a), l * sin(a))); }

static V2 Rect_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { if (SELFO) SELFO->kind = OK_RECT; dset(vm, self, "x", v2_num(argc > 0 ? NUM(0) : 0)); dset(vm, self, "y", v2_num(argc > 1 ? NUM(1) : 0)); dset(vm, self, "width", v2_num(argc > 2 ? NUM(2) : 0)); dset(vm, self, "height", v2_num(argc > 3 ? NUM(3) : 0)); return v2_undef(); }
#define RX dnum(vm, self, "x")
#define RY dnum(vm, self, "y")
#define RW dnum(vm, self, "width")
#define RH dnum(vm, self, "height")
static V2 rc_left_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return v2_num(RX); }
static V2 rc_left_set(VM2 *vm, V2 self, int argc, V2 *argv) { double v = NUM(0); dset(vm, self, "width", v2_num(RW + RX - v)); dset(vm, self, "x", v2_num(v)); return v2_undef(); }
static V2 rc_top_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return v2_num(RY); }
static V2 rc_top_set(VM2 *vm, V2 self, int argc, V2 *argv) { double v = NUM(0); dset(vm, self, "height", v2_num(RH + RY - v)); dset(vm, self, "y", v2_num(v)); return v2_undef(); }
static V2 rc_right_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return v2_num(RX + RW); }
static V2 rc_right_set(VM2 *vm, V2 self, int argc, V2 *argv) { dset(vm, self, "width", v2_num(NUM(0) - RX)); return v2_undef(); }
static V2 rc_bottom_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return v2_num(RY + RH); }
static V2 rc_bottom_set(VM2 *vm, V2 self, int argc, V2 *argv) { dset(vm, self, "height", v2_num(NUM(0) - RY)); return v2_undef(); }
static V2 rc_size_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return v2_obj(a2_point(vm, RW, RH)); }
static V2 rc_size_set(VM2 *vm, V2 self, int argc, V2 *argv) { dset(vm, self, "width", v2_num(dnum(vm, ARG(0), "x"))); dset(vm, self, "height", v2_num(dnum(vm, ARG(0), "y"))); return v2_undef(); }
static V2 rc_topLeft_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return v2_obj(a2_point(vm, RX, RY)); }
static V2 rc_topLeft_set(VM2 *vm, V2 self, int argc, V2 *argv) { V2 a[1]; a[0] = v2_num(dnum(vm, ARG(0), "x")); rc_left_set(vm, self, 1, a); a[0] = v2_num(dnum(vm, ARG(0), "y")); rc_top_set(vm, self, 1, a); return v2_undef(); }
static V2 rc_bottomRight_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return v2_obj(a2_point(vm, RX + RW, RY + RH)); }
static V2 rc_bottomRight_set(VM2 *vm, V2 self, int argc, V2 *argv) { dset(vm, self, "width", v2_num(dnum(vm, ARG(0), "x") - RX)); dset(vm, self, "height", v2_num(dnum(vm, ARG(0), "y") - RY)); return v2_undef(); }
static V2 rc_clone(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return v2_obj(a2_rect(vm, RX, RY, RW, RH)); }
static V2 rc_contains(VM2 *vm, V2 self, int argc, V2 *argv) { double x = NUM(0), y = NUM(1); return v2_bool(x >= RX && x < RX + RW && y >= RY && y < RY + RH); }
static V2 rc_containsPoint(VM2 *vm, V2 self, int argc, V2 *argv) { double x = dnum(vm, ARG(0), "x"), y = dnum(vm, ARG(0), "y"); return v2_bool(x >= RX && x < RX + RW && y >= RY && y < RY + RH); }
static V2 rc_containsRect(VM2 *vm, V2 self, int argc, V2 *argv) { V2 r = ARG(0); double x = dnum(vm, r, "x"), y = dnum(vm, r, "y"), w = dnum(vm, r, "width"), h = dnum(vm, r, "height"); return v2_bool(x >= RX && y >= RY && x + w <= RX + RW && y + h <= RY + RH); }
static V2 rc_equals(VM2 *vm, V2 self, int argc, V2 *argv) { V2 r = ARG(0); return v2_bool(r.t == T_OBJ && RX == dnum(vm, r, "x") && RY == dnum(vm, r, "y") && RW == dnum(vm, r, "width") && RH == dnum(vm, r, "height")); }
static V2 rc_inflate(VM2 *vm, V2 self, int argc, V2 *argv) { double dx = NUM(0), dy = NUM(1); dset(vm, self, "x", v2_num(RX - dx)); dset(vm, self, "y", v2_num(RY - dy)); dset(vm, self, "width", v2_num(RW + 2 * dx)); dset(vm, self, "height", v2_num(RH + 2 * dy)); return v2_undef(); }
static V2 rc_inflatePoint(VM2 *vm, V2 self, int argc, V2 *argv) { V2 a[2]; a[0] = v2_num(dnum(vm, ARG(0), "x")); a[1] = v2_num(dnum(vm, ARG(0), "y")); return rc_inflate(vm, self, 2, a); }
static V2 rc_offset(VM2 *vm, V2 self, int argc, V2 *argv) { dset(vm, self, "x", v2_num(RX + NUM(0))); dset(vm, self, "y", v2_num(RY + NUM(1))); return v2_undef(); }
static V2 rc_offsetPoint(VM2 *vm, V2 self, int argc, V2 *argv) { dset(vm, self, "x", v2_num(RX + dnum(vm, ARG(0), "x"))); dset(vm, self, "y", v2_num(RY + dnum(vm, ARG(0), "y"))); return v2_undef(); }
static V2 rc_isEmpty(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return v2_bool(RW <= 0 || RH <= 0); }
static V2 rc_setEmpty(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; dset(vm, self, "x", v2_num(0)); dset(vm, self, "y", v2_num(0)); dset(vm, self, "width", v2_num(0)); dset(vm, self, "height", v2_num(0)); return v2_undef(); }
static V2 rc_setTo(VM2 *vm, V2 self, int argc, V2 *argv) { dset(vm, self, "x", v2_num(NUM(0))); dset(vm, self, "y", v2_num(NUM(1))); dset(vm, self, "width", v2_num(NUM(2))); dset(vm, self, "height", v2_num(NUM(3))); return v2_undef(); }
static V2 rc_copyFrom(VM2 *vm, V2 self, int argc, V2 *argv) { V2 r = ARG(0); dset(vm, self, "x", v2_num(dnum(vm, r, "x"))); dset(vm, self, "y", v2_num(dnum(vm, r, "y"))); dset(vm, self, "width", v2_num(dnum(vm, r, "width"))); dset(vm, self, "height", v2_num(dnum(vm, r, "height"))); return v2_undef(); }
static V2 rc_intersection(VM2 *vm, V2 self, int argc, V2 *argv) { V2 r = ARG(0); double x0 = RX > dnum(vm, r, "x") ? RX : dnum(vm, r, "x"), y0 = RY > dnum(vm, r, "y") ? RY : dnum(vm, r, "y"); double x1 = RX + RW < dnum(vm, r, "x") + dnum(vm, r, "width") ? RX + RW : dnum(vm, r, "x") + dnum(vm, r, "width"), y1 = RY + RH < dnum(vm, r, "y") + dnum(vm, r, "height") ? RY + RH : dnum(vm, r, "y") + dnum(vm, r, "height"); if (x1 <= x0 || y1 <= y0) return v2_obj(a2_rect(vm, 0, 0, 0, 0)); return v2_obj(a2_rect(vm, x0, y0, x1 - x0, y1 - y0)); }
static V2 rc_intersects(VM2 *vm, V2 self, int argc, V2 *argv) { V2 r = rc_intersection(vm, self, argc, argv); return v2_bool(dnum(vm, r, "width") > 0 && dnum(vm, r, "height") > 0); }
static V2 rc_union(VM2 *vm, V2 self, int argc, V2 *argv) { V2 r = ARG(0); double x0, y0, x1, y1; if (RW <= 0 || RH <= 0) return rc_clone(vm, r, 0, NULL); if (dnum(vm, r, "width") <= 0 || dnum(vm, r, "height") <= 0) return rc_clone(vm, self, 0, NULL); x0 = RX < dnum(vm, r, "x") ? RX : dnum(vm, r, "x"); y0 = RY < dnum(vm, r, "y") ? RY : dnum(vm, r, "y"); x1 = RX + RW > dnum(vm, r, "x") + dnum(vm, r, "width") ? RX + RW : dnum(vm, r, "x") + dnum(vm, r, "width"); y1 = RY + RH > dnum(vm, r, "y") + dnum(vm, r, "height") ? RY + RH : dnum(vm, r, "y") + dnum(vm, r, "height"); return v2_obj(a2_rect(vm, x0, y0, x1 - x0, y1 - y0)); }
static V2 rc_toString(VM2 *vm, V2 self, int argc, V2 *argv) { char b[160], t[4][40]; (void)argc; (void)argv; sprintf(b, "(x=%s, y=%s, w=%s, h=%s)", v2_num_to_cstr(RX, t[0]), v2_num_to_cstr(RY, t[1]), v2_num_to_cstr(RW, t[2]), v2_num_to_cstr(RH, t[3])); return v2_cstr(vm, b); }

static V2 Matrix_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { if (SELFO) SELFO->kind = OK_MATRIX; dset(vm, self, "a", v2_num(argc > 0 ? NUM(0) : 1)); dset(vm, self, "b", v2_num(argc > 1 ? NUM(1) : 0)); dset(vm, self, "c", v2_num(argc > 2 ? NUM(2) : 0)); dset(vm, self, "d", v2_num(argc > 3 ? NUM(3) : 1)); dset(vm, self, "tx", v2_num(argc > 4 ? NUM(4) : 0)); dset(vm, self, "ty", v2_num(argc > 5 ? NUM(5) : 0)); return v2_undef(); }
extern void throw_null_ref(VM2 *vm, V2 v);
static void a2_throw_null_ref_any(VM2 *vm, V2 v) { throw_null_ref(vm, v); }
static void mget(VM2 *vm, V2 m, double *o) { o[0] = dnum(vm, m, "a"); o[1] = dnum(vm, m, "b"); o[2] = dnum(vm, m, "c"); o[3] = dnum(vm, m, "d"); o[4] = dnum(vm, m, "tx"); o[5] = dnum(vm, m, "ty"); }
static void mset(VM2 *vm, V2 m, const double *o) { dset(vm, m, "a", v2_num(o[0])); dset(vm, m, "b", v2_num(o[1])); dset(vm, m, "c", v2_num(o[2])); dset(vm, m, "d", v2_num(o[3])); dset(vm, m, "tx", v2_num(o[4])); dset(vm, m, "ty", v2_num(o[5])); }
static V2 mx_identity(VM2 *vm, V2 self, int argc, V2 *argv) { double o[6] = { 1, 0, 0, 1, 0, 0 }; (void)argc; (void)argv; mset(vm, self, o); return v2_undef(); }
static V2 mx_clone(VM2 *vm, V2 self, int argc, V2 *argv) { double o[6]; V2 r; (void)argc; (void)argv; mget(vm, self, o); r = a2_construct_class(vm, vm->c_matrix, 0, NULL); mset(vm, r, o); return r; }
static V2 mx_concat(VM2 *vm, V2 self, int argc, V2 *argv) { double t[6], m[6], o[6]; mget(vm, self, t); mget(vm, ARG(0), m); o[0] = m[0] * t[0] + m[2] * t[1]; o[1] = m[1] * t[0] + m[3] * t[1]; o[2] = m[0] * t[2] + m[2] * t[3]; o[3] = m[1] * t[2] + m[3] * t[3]; o[4] = m[0] * t[4] + m[2] * t[5] + m[4]; o[5] = m[1] * t[4] + m[3] * t[5] + m[5]; mset(vm, self, o); return v2_undef(); }
static V2 mx_translate(VM2 *vm, V2 self, int argc, V2 *argv) { dset(vm, self, "tx", v2_num(dnum(vm, self, "tx") + NUM(0))); dset(vm, self, "ty", v2_num(dnum(vm, self, "ty") + NUM(1))); return v2_undef(); }
static void mx_scale_by(VM2 *vm, V2 self, double sx, double sy) { double t[6]; mget(vm, self, t); t[0] *= sx; t[1] *= sy; t[2] *= sx; t[3] *= sy; t[4] *= sx; t[5] *= sy; mset(vm, self, t); }
static V2 mx_scale(VM2 *vm, V2 self, int argc, V2 *argv) { mx_scale_by(vm, self, NUM(0), NUM(1)); return v2_undef(); }
static void mx_rotate_by(VM2 *vm, V2 self, double ang) { double t[6], o[6], sn = sin(ang), cs = cos(ang); mget(vm, self, t); o[0] = cs * t[0] + (-sn) * t[1]; o[1] = sn * t[0] + cs * t[1]; o[2] = cs * t[2] + (-sn) * t[3]; o[3] = sn * t[2] + cs * t[3]; o[4] = cs * t[4] + (-sn) * t[5]; o[5] = sn * t[4] + cs * t[5]; mset(vm, self, o); }
static V2 mx_rotate(VM2 *vm, V2 self, int argc, V2 *argv) { mx_rotate_by(vm, self, NUM(0)); return v2_undef(); }
static V2 mx_invert(VM2 *vm, V2 self, int argc, V2 *argv) { double t[6], o[6], det; (void)argc; (void)argv; mget(vm, self, t); det = t[0] * t[3] - t[2] * t[1]; o[4] = (t[3] * t[4] - t[2] * t[5]) / -det; o[5] = (t[1] * t[4] - t[0] * t[5]) / det; o[0] = t[3] / det; o[1] = t[1] / -det; o[2] = t[2] / -det; o[3] = t[0] / det; mset(vm, self, o); return v2_undef(); }
static V2 mx_transformPoint(VM2 *vm, V2 self, int argc, V2 *argv) { double m[6], x = dnum(vm, ARG(0), "x"), y = dnum(vm, ARG(0), "y"); mget(vm, self, m); return v2_obj(a2_point(vm, m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5])); }
static V2 mx_deltaTransformPoint(VM2 *vm, V2 self, int argc, V2 *argv) { double m[6], x = dnum(vm, ARG(0), "x"), y = dnum(vm, ARG(0), "y"); mget(vm, self, m); return v2_obj(a2_point(vm, m[0] * x + m[2] * y, m[1] * x + m[3] * y)); }
static void mx_box(VM2 *vm, V2 self, double sx, double sy, double r, double tx, double ty) { double id[6] = { 1, 0, 0, 1, 0, 0 }, t[6]; mset(vm, self, id); mx_rotate_by(vm, self, r); mx_scale_by(vm, self, sx, sy); mget(vm, self, t); t[4] += tx; t[5] += ty; mset(vm, self, t); }
static V2 mx_createBox(VM2 *vm, V2 self, int argc, V2 *argv) { mx_box(vm, self, NUM(0), NUM(1), argc > 2 ? NUM(2) : 0, argc > 3 ? NUM(3) : 0, argc > 4 ? NUM(4) : 0); return v2_undef(); }
static V2 mx_createGradientBox(VM2 *vm, V2 self, int argc, V2 *argv) { double w = NUM(0), h = NUM(1); mx_box(vm, self, w / 1638.4, h / 1638.4, argc > 2 ? NUM(2) : 0, (argc > 3 ? NUM(3) : 0) + w / 2, (argc > 4 ? NUM(4) : 0) + h / 2); return v2_undef(); }
static V2 mx_copyRowTo(VM2 *vm, V2 self, int argc, V2 *argv) { double t[6]; u32 r = v2_touint(vm, ARG(0)); V2 v = ARG(1); mget(vm, self, t); if (r > 2) return v2_undef(); if (v.t != T_OBJ) { a2_throw_null_ref_any(vm, v); return v2_undef(); } a2_set_s(vm, v, "x", v2_num(r == 0 ? t[0] : r == 1 ? t[1] : 0)); a2_set_s(vm, v, "y", v2_num(r == 0 ? t[2] : r == 1 ? t[3] : 0)); a2_set_s(vm, v, "z", v2_num(r == 0 ? t[4] : r == 1 ? t[5] : 1)); return v2_undef(); }
static V2 mx_copyColumnTo(VM2 *vm, V2 self, int argc, V2 *argv) { double t[6]; u32 c = v2_touint(vm, ARG(0)); V2 v = ARG(1); mget(vm, self, t); if (c > 2) return v2_undef(); if (v.t != T_OBJ) { a2_throw_null_ref_any(vm, v); return v2_undef(); } a2_set_s(vm, v, "x", v2_num(t[c * 2])); a2_set_s(vm, v, "y", v2_num(t[c * 2 + 1])); a2_set_s(vm, v, "z", v2_num(c == 2 ? 1 : 0)); return v2_undef(); }
static V2 mx_copyRowFrom(VM2 *vm, V2 self, int argc, V2 *argv) { double t[6]; u32 r = v2_touint(vm, ARG(0)); V2 v = ARG(1); if (r > 1) return v2_undef(); if (v.t != T_OBJ) { a2_throw_null_ref_any(vm, v); return v2_undef(); } mget(vm, self, t); t[r] = v2_tonum(vm, a2_get_s(vm, v, "x")); t[2 + r] = v2_tonum(vm, a2_get_s(vm, v, "y")); t[4 + r] = v2_tonum(vm, a2_get_s(vm, v, "z")); mset(vm, self, t); return v2_undef(); }
static V2 mx_setTo(VM2 *vm, V2 self, int argc, V2 *argv) { double o[6]; int i; for (i = 0; i < 6; i++) o[i] = i < argc ? v2_tonum(vm, argv[i]) : 0; mset(vm, self, o); return v2_undef(); }
static V2 mx_copyFrom(VM2 *vm, V2 self, int argc, V2 *argv) { double o[6]; mget(vm, ARG(0), o); mset(vm, self, o); return v2_undef(); }
static V2 mx_toString(VM2 *vm, V2 self, int argc, V2 *argv) { char b[240], t[6][40]; double m[6]; (void)argc; (void)argv; mget(vm, self, m); sprintf(b, "(a=%s, b=%s, c=%s, d=%s, tx=%s, ty=%s)", v2_num_to_cstr(m[0], t[0]), v2_num_to_cstr(m[1], t[1]), v2_num_to_cstr(m[2], t[2]), v2_num_to_cstr(m[3], t[3]), v2_num_to_cstr(m[4], t[4]), v2_num_to_cstr(m[5], t[5])); return v2_cstr(vm, b); }

static V2 CT_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { if (SELFO) SELFO->kind = OK_COLORTRANSFORM; dset(vm, self, "redMultiplier", v2_num(argc > 0 ? NUM(0) : 1)); dset(vm, self, "greenMultiplier", v2_num(argc > 1 ? NUM(1) : 1)); dset(vm, self, "blueMultiplier", v2_num(argc > 2 ? NUM(2) : 1)); dset(vm, self, "alphaMultiplier", v2_num(argc > 3 ? NUM(3) : 1)); dset(vm, self, "redOffset", v2_num(argc > 4 ? NUM(4) : 0)); dset(vm, self, "greenOffset", v2_num(argc > 5 ? NUM(5) : 0)); dset(vm, self, "blueOffset", v2_num(argc > 6 ? NUM(6) : 0)); dset(vm, self, "alphaOffset", v2_num(argc > 7 ? NUM(7) : 0)); return v2_undef(); }
static V2 ct_color_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return v2_uint((((u32)dnum(vm, self, "redOffset") & 255) << 16) | (((u32)dnum(vm, self, "greenOffset") & 255) << 8) | ((u32)dnum(vm, self, "blueOffset") & 255)); }
static V2 ct_color_set(VM2 *vm, V2 self, int argc, V2 *argv) { u32 c = v2_touint(vm, ARG(0)); dset(vm, self, "redMultiplier", v2_num(0)); dset(vm, self, "greenMultiplier", v2_num(0)); dset(vm, self, "blueMultiplier", v2_num(0)); dset(vm, self, "redOffset", v2_num((c >> 16) & 255)); dset(vm, self, "greenOffset", v2_num((c >> 8) & 255)); dset(vm, self, "blueOffset", v2_num(c & 255)); return v2_undef(); }
static V2 ct_concat(VM2 *vm, V2 self, int argc, V2 *argv) { V2 o = ARG(0); const char *m[] = { "redMultiplier", "greenMultiplier", "blueMultiplier", "alphaMultiplier" }, *a[] = { "redOffset", "greenOffset", "blueOffset", "alphaOffset" }; int i; for (i = 0; i < 4; i++) { dset(vm, self, a[i], v2_num(dnum(vm, self, a[i]) + dnum(vm, self, m[i]) * dnum(vm, o, a[i]))); dset(vm, self, m[i], v2_num(dnum(vm, self, m[i]) * dnum(vm, o, m[i]))); } return v2_undef(); }
static V2 ct_toString(VM2 *vm, V2 self, int argc, V2 *argv) { char b[400], t[8][40]; (void)argc; (void)argv; sprintf(b, "(redMultiplier=%s, greenMultiplier=%s, blueMultiplier=%s, alphaMultiplier=%s, redOffset=%s, greenOffset=%s, blueOffset=%s, alphaOffset=%s)", v2_num_to_cstr(dnum(vm, self, "redMultiplier"), t[0]), v2_num_to_cstr(dnum(vm, self, "greenMultiplier"), t[1]), v2_num_to_cstr(dnum(vm, self, "blueMultiplier"), t[2]), v2_num_to_cstr(dnum(vm, self, "alphaMultiplier"), t[3]), v2_num_to_cstr(dnum(vm, self, "redOffset"), t[4]), v2_num_to_cstr(dnum(vm, self, "greenOffset"), t[5]), v2_num_to_cstr(dnum(vm, self, "blueOffset"), t[6]), v2_num_to_cstr(dnum(vm, self, "alphaOffset"), t[7])); return v2_cstr(vm, b); }

/* Transform: live view onto a display object */
static A2Obj *tr_target(VM2 *vm, V2 self) { V2 t = dget(vm, self, "__target"); return (t.t == T_OBJ && t.u.o->kind == OK_DISPLAY) ? t.u.o : NULL; }
static V2 tr_matrix_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *t = tr_target(vm, self);
    if (t && a2_disp(t)->is_stage) {   /* the stage keeps a matrix set by script (it does not move the content's coordinates) */
        A2Prop *pm = a2_dyn_find(t, a2_str(vm, "__stageMatrix"));
        if (pm && pm->v.t == T_OBJ) { SWFMatrix m; a2_matrix_from_obj(vm, pm->v, &m); return v2_obj(a2_matrix_obj(vm, &m)); }
    } DisplayObject *d = t ? a2_dobj_of(t) : NULL; SWFMatrix id; (void)argc; (void)argv; if (t && a2_disp(t)->has_m3d) return v2_null(); if (!d) { swf_matrix_identity(&id); return v2_obj(a2_matrix_obj(vm, &id)); } return v2_obj(a2_matrix_obj(vm, &d->matrix)); }
static V2 tr_matrix_set(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *t = tr_target(vm, self);
    if (t && a2_disp(t)->is_stage) { if (ARG(0).t == T_OBJ) { SWFMatrix m; a2_matrix_from_obj(vm, ARG(0), &m); a2_dyn_set_hidden(vm, t, a2_str(vm, "__stageMatrix"), v2_obj(a2_matrix_obj(vm, &m))); vm->player->stage_mat = m; vm->player->has_stage_mat = 1; } return v2_undef(); } DisplayObject *d = t ? a2_dobj_of(t) : NULL; if (t && ARG(0).t != T_OBJ) { a2_disp(t)->has_m3d = 1; return v2_undef(); } if (d && ARG(0).t == T_OBJ) { d->script_moved = 1; a2_matrix_from_obj(vm, ARG(0), &d->matrix); a2_disp(t)->has_xform = 0; a2_disp(t)->has_m3d = 0; } return v2_undef(); }
static V2 tr_concatenatedMatrix(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *t = tr_target(vm, self); SWFMatrix w; (void)argc; (void)argv; if (!t) { swf_matrix_identity(&w); return v2_obj(a2_matrix_obj(vm, &w)); } { struct { float a, b, c, d; s32 tx, ty; } rm; extern void a2_world_rmat(A2Obj *o, int own_scroll, void *out); a2_world_rmat(t, 0, &rm); w.sx = rm.a; w.r0 = rm.b; w.r1 = rm.c; w.sy = rm.d; w.tx = rm.tx; w.ty = rm.ty; }
    /* Flash quirk: the stage itself and objects NOT on the display list report the device scale (20 twips / AA sub-samples) */
    if (a2_disp(t)->is_stage || !a2_on_stage(vm, t)) {   /* its own matrix with a and d scaled by the device scale (Ruffle) */
        extern int a2_quality_aa; double k = 20.0 / a2_quality_aa; DisplayObject *dd = a2_dobj_of(t);
        if (dd) { struct { float a, b, c, d; s32 tx, ty; } rm; extern void rm_from(const SWFMatrix *m, void *o); rm_from(&dd->matrix, &rm); w.sx = rm.a; w.r0 = rm.b; w.r1 = rm.c; w.sy = rm.d; w.tx = rm.tx; w.ty = rm.ty; } else swf_matrix_identity(&w);
        w.sx = (float)(w.sx * k); w.sy = (float)(w.sy * k); }
    return v2_obj(a2_matrix_obj(vm, &w)); }
static V2 tr_colorTransform_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *t = tr_target(vm, self); DisplayObject *d = t ? a2_dobj_of(t) : NULL; V2 a[8]; (void)argc; (void)argv; if (!d) return a2_construct_class(vm, vm->c_colortransform, 0, NULL); a[0] = v2_num(d->cxform.mr / 256.0); a[1] = v2_num(d->cxform.mg / 256.0); a[2] = v2_num(d->cxform.mb / 256.0); a[3] = v2_num(d->cxform.ma / 256.0); a[4] = v2_num(d->cxform.ar); a[5] = v2_num(d->cxform.ag); a[6] = v2_num(d->cxform.ab); a[7] = v2_num(d->cxform.aa); return a2_construct_class(vm, vm->c_colortransform, 8, a); }
static V2 tr_colorTransform_set(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *t = tr_target(vm, self); DisplayObject *d = t ? a2_dobj_of(t) : NULL; V2 c = ARG(0); if (!d || c.t != T_OBJ) return v2_undef(); d->script_moved = 1; d->cxform.mr = (s16)(dnum(vm, c, "redMultiplier") * 256); d->cxform.mg = (s16)(dnum(vm, c, "greenMultiplier") * 256); d->cxform.mb = (s16)(dnum(vm, c, "blueMultiplier") * 256); d->cxform.ma = (s16)(dnum(vm, c, "alphaMultiplier") * 256); d->cxform.ar = (s16)dnum(vm, c, "redOffset"); d->cxform.ag = (s16)dnum(vm, c, "greenOffset"); d->cxform.ab = (s16)dnum(vm, c, "blueOffset"); d->cxform.aa = (s16)dnum(vm, c, "alphaOffset"); return v2_undef(); }
static V2 tr_pixelBounds(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *t = tr_target(vm, self); A2Disp *st; int pend = 0; V2 r; (void)argc; (void)argv;
    if (!t) return v2_obj(a2_rect(vm, 0, 0, 0, 0));
    st = a2_disp(t);
    if (st && st->is_text && st->tfs) { pend = st->tfs->as_pending; st->tfs->as_pending = 0; }   /* pixelBounds do not apply lazy autoSize bounds */
    if (st && st->is_text && vm->stage_obj && a2_on_stage(vm, t)) {
        /* a text field on the stage reports an empty rectangle at its origin */
        V2 a[2]; A2Class *pc = a2_class_find_s(vm, "flash.geom::Point");
        a[0] = v2_num(0); a[1] = v2_num(0);
        if (pc) { V2 pt = a2_construct_class(vm, pc, 2, a); a[0] = pt; pt = a2_call_s(vm, v2_obj(t), "localToGlobal", 1, a);
            if (pt.t == T_OBJ) return v2_obj(a2_rect(vm, floor(dnum(vm, pt, "x")), floor(dnum(vm, pt, "y")), 0, 0)); }
    }
    if (!(st && st->is_text) && vm->stage_obj && a2_on_stage(vm, t)) {
        /* stage pixels covered by the object: rounded outwards, clipped to the stage */
        V2 a[1]; double x0, y0, x1, y1, sw = vm->player->movie ? (vm->player->movie->stage.xmax - vm->player->movie->stage.xmin) / 20.0 : 0, sh = vm->player->movie ? (vm->player->movie->stage.ymax - vm->player->movie->stage.ymin) / 20.0 : 0;
        a[0] = v2_obj(vm->stage_obj);
        r = a2_call_s(vm, v2_obj(t), "getBounds", 1, a);
        if (r.t != T_OBJ) return r;
        x0 = floor(dnum(vm, r, "x")); y0 = floor(dnum(vm, r, "y"));
        x1 = ceil(dnum(vm, r, "x") + dnum(vm, r, "width")); y1 = ceil(dnum(vm, r, "y") + dnum(vm, r, "height"));
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > sw) x1 = sw; if (y1 > sh) y1 = sh;
        if (x1 < x0) x1 = x0; if (y1 < y0) y1 = y0;
        return v2_obj(a2_rect(vm, x0, y0, x1 - x0, y1 - y0));
    }
    r = a2_call_s(vm, v2_obj(t), "getBounds", 0, NULL);
    if (st && st->is_text && st->tfs && pend) st->tfs->as_pending = 1;
    return r;
}
/* 3D parts of a Transform, kept on the target (no 3D rendering yet) */
extern V2 a2_m3_from_2d(VM2 *vm, const SWFMatrix *mx, double tz); extern int a2_m3_raw(A2Obj *o, float *out);
static V2 tr_matrix3D_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *t = tr_target(vm, self); DisplayObject *d = t ? a2_dobj_of(t) : NULL; SWFMatrix id; UNUSED; if (!t || !a2_disp(t)->has_m3d) return v2_null(); swf_matrix_identity(&id); return a2_m3_from_2d(vm, d ? &d->matrix : &id, a2_disp(t)->z); }
static V2 tr_matrix3D_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *t = tr_target(vm, self); DisplayObject *d = t ? a2_dobj_of(t) : NULL; float r[16]; UNUSED;
    if (!t) return v2_undef();
    if (ARG(0).t == T_OBJ && a2_m3_raw(ARG(0).u.o, r)) {
        extern int a2_m3_claim(A2Obj *o, const void *target);
        if (!a2_m3_claim(ARG(0).u.o, t)) { a2_throw_id(vm, "ArgumentError", 2189, NULL, NULL); return v2_undef(); }
        if (d) { d->matrix.sx = r[0]; d->matrix.r0 = r[1]; d->matrix.r1 = r[4]; d->matrix.sy = r[5]; d->matrix.tx = floor(r[12] * 20.0 + 0.5); d->matrix.ty = floor(r[13] * 20.0 + 0.5); }
        a2_disp(t)->z = r[14]; a2_disp(t)->has_m3d = 1;
    } else { if (d) swf_matrix_identity(&d->matrix); a2_disp(t)->z = 0; a2_disp(t)->has_m3d = 0; }
    a2_disp(t)->has_xform = 0;
    return v2_undef();
}
static V2 tr_getRelativeMatrix3D(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *t = tr_target(vm, self); UNUSED;
    if (ARG(0).t != T_OBJ) { a2_throw_id(vm, "TypeError", 2007, "relativeTo", NULL); return v2_undef(); }
    if (!t || !a2_disp(t)->has_m3d) return v2_null();
    { SWFMatrix w; extern void a2_world_matrix(A2Obj *o, SWFMatrix *out); a2_world_matrix(t, &w); return a2_m3_from_2d(vm, &w, a2_disp(t)->z); }
}
static V2 tr_pp_get(VM2 *vm, V2 self, int argc, V2 *argv) { extern V2 a2_transform_pp_get(VM2 *vm, A2Obj *t); A2Obj *t = tr_target(vm, self); UNUSED; return t ? a2_transform_pp_get(vm, t) : v2_null(); }
static V2 tr_pp_set(VM2 *vm, V2 self, int argc, V2 *argv) { extern void a2_transform_pp_set(VM2 *vm, A2Obj *t, V2 v); A2Obj *t = tr_target(vm, self); if (t) a2_transform_pp_set(vm, t, ARG(0)); return v2_undef(); }
static V2 Transform_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { if (argc > 0) dset(vm, self, "__target", ARG(0)); return v2_undef(); }

/* ---------------------------------------------------------------- */
/* Loader / LoaderInfo / net                                        */

struct SWFMovie *a2_code_movie(VM2 *vm); void a2_report_uncaught(VM2 *vm); void avm2_trace_out(const char *s); void player_run_first_frame(MovieClip *c); Bitmap *image_decode_bytes(const u8 *d, size_t n);
/* LoaderInfo of the main movie (no load state) */
V2 a2_root_loaderinfo(VM2 *vm) {
    V2 li = dget(vm, v2_obj(vm->global), "__rootLoaderInfo");
    if (li.t == T_OBJ) return li;
    li = a2_construct_class(vm, vm->c_loaderinfo, 0, NULL);
    a2_dyn_set(vm, vm->global, a2_str(vm, "__rootLoaderInfo"), li);
    return li;
}

/* Loader / URLLoader state, the LoaderInfo's (or URLLoader's) payload.
 * Timing follows Flash/Ruffle: a URL load gets its data at the end of the
 * frame it was requested in (open, progress events; images complete there);
 * a SWF's content is built at the end of the next frame (constructor with no
 * parent, added to the Loader, frame 1 scripts, init, httpStatus, complete).
 * loadBytes delivers its data at once and builds the content (SWF or image)
 * at the end of the next frame. */
enum { LS_IDLE, LS_REQUESTED, LS_HAVE_DATA, LS_DONE, LS_FAILED, LS_BUILT };
enum { LT_UNKNOWN, LT_SWF, LT_PNG, LT_JPEG, LT_GIF, LT_JXR };
typedef struct LoadState {
    int status, from_bytes, frame, ctype, ready, urlloader;
    int a1_wait;                    /* AVM1 content: init / complete wait for the AVM1 frame that runs its frame 1 */
    int early;                      /* built when its data came (end of frame): init / complete wait a frame */
    u32 loaded, total;
    SWFMovie *movie;
    size_t len;                     /* raw data follows the struct */
} LoadState;
#define LS_DATA(st) ((u8*)((st) + 1))

static LoadState *ls_of(A2Obj *o) { return o ? (LoadState*)o->payload : NULL; }
static LoadState *ls_reset(A2Obj *o, const u8 *data, size_t len) {
    LoadState *st = (LoadState*)calloc(1, sizeof(LoadState) + len + 1);
    if (!st) return NULL;
    if (data && len) memcpy(LS_DATA(st), data, len);
    st->len = len;
    free(o->payload); o->payload = st;
    return st;
}
static void ls_drop_data(A2Obj *o) {
    LoadState *st = ls_of(o), *n;
    if (!st || !st->len) return;
    n = (LoadState*)realloc(st, sizeof(LoadState) + 1);
    if (n) { n->len = 0; o->payload = n; }
}

/* --- URLs --- */
static const char *movie_url(SWFMovie *m) { return m && m->url ? m->url : "file:///movie.swf"; }
#include "fetch.h"
#define url_resolve fetch_url_resolve
#define url_to_path fetch_url_to_path
#define read_file fetch_read_file

static int sniff(const u8 *d, size_t n) {
    if (n >= 8 && (d[0] == 'F' || d[0] == 'C' || d[0] == 'Z') && d[1] == 'W' && d[2] == 'S') return LT_SWF;
    if (n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') return LT_PNG;
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8) return LT_JPEG;
    if (n >= 6 && !memcmp(d, "GIF8", 4)) return LT_GIF;
    if (n >= 8 && d[0] == 'I' && d[1] == 'I' && d[2] == 0xBC) return LT_JXR;
    return LT_UNKNOWN;
}
static const char *ctype_name(int t) { return t == LT_SWF ? "application/x-shockwave-flash" : t == LT_PNG ? "image/png" : t == LT_JPEG ? "image/jpeg" : t == LT_GIF ? "image/gif" : t == LT_JXR ? "image/jpegxr" : NULL; }
static int png_has_alpha(const u8 *d, size_t n) {
    size_t pos = 8;
    if (n < 26) return 0;
    if (d[25] == 4 || d[25] == 6) return 1;
    while (pos + 8 <= n) { u32 cl = ((u32)d[pos] << 24) | ((u32)d[pos + 1] << 16) | ((u32)d[pos + 2] << 8) | d[pos + 3]; if (!memcmp(d + pos + 4, "tRNS", 4)) return 1; if (!memcmp(d + pos + 4, "IDAT", 4)) break; pos += 12 + cl; }
    return 0;
}

/* --- events --- */
static void fire(VM2 *vm, A2Obj *t, const char *type) { a2_dispatch_simple(vm, t, a2_str(vm, type), 0); if (vm->throwing) a2_report_uncaught(vm); }
static void fire_progress(VM2 *vm, A2Obj *t, u32 loaded, u32 total) {
    A2Class *pc = a2_class_find_s(vm, "flash.events::ProgressEvent"); V2 a[5], e;
    a[0] = v2_cstr(vm, "progress"); a[1] = v2_bool(0); a[2] = v2_bool(0); a[3] = v2_num(loaded); a[4] = v2_num(total);
    e = a2_construct_class(vm, pc, 5, a);
    if (e.t == T_OBJ) a2_dispatch_event_obj(vm, t, e.u.o);
    if (vm->throwing) a2_report_uncaught(vm);
}
static int flash_net(void);
/* the root LoaderInfo's progress event before its complete (Flash's network timing only) */
void a2_root_progress(VM2 *vm, A2Obj *li) {
    V2 t; extern V2 a2_get_s(VM2 *vm, V2 o, const char *name);
    if (!flash_net()) return;
    t = a2_get_s(vm, v2_obj(li), "bytesTotal");
    if (!vm->throwing) fire_progress(vm, li, v2_touint(vm, t), v2_touint(vm, t));
}
static void fire_status(VM2 *vm, A2Obj *t, int status) {
    A2Class *hc = a2_class_find_s(vm, "flash.events::HTTPStatusEvent"); V2 a[4], e;
    a[0] = v2_cstr(vm, "httpStatus"); a[1] = v2_bool(0); a[2] = v2_bool(0); a[3] = v2_int(status);
    e = a2_construct_class(vm, hc, 4, a);
    if (e.t == T_OBJ) a2_dispatch_event_obj(vm, t, e.u.o);
    if (vm->throwing) a2_report_uncaught(vm);
}
static void fire_ioerror(VM2 *vm, V2 target, const char *msg, int id) {
    A2Class *ioe = a2_class_find_s(vm, "flash.events::IOErrorEvent"); A2Obj *e;
    if (target.t != T_OBJ) return;
    if (!a2_has_listener(target.u.o, a2_str(vm, "ioError"))) { if (vm->player->debug) fprintf(stderr, "Error #2044: Unhandled ioError:. text=%s\n", msg); return; }
    e = a2_make_event(vm, ioe, a2_str(vm, "ioError"), 0, 0);
    a2_dyn_set(vm, e, a2_str(vm, "text"), v2_cstr(vm, msg)); a2_dyn_set(vm, e, a2_str(vm, "errorID"), v2_int(id));
    a2_dispatch_event_obj(vm, target.u.o, e);
    if (vm->throwing) a2_report_uncaught(vm);
}

/* --- the queue (a hidden array: keeps the objects alive while loading) --- */
static void lq_add(VM2 *vm, A2Obj *o) {
    V2 q = dget(vm, v2_obj(vm->global), "__loadq"); u32 i, n;
    if (q.t != T_OBJ) { q = v2_obj(a2_new_array(vm)); a2_dyn_set_hidden(vm, vm->global, a2_str(vm, "__loadq"), q); }
    n = a2_arr_len(q.u.o);
    for (i = 0; i < n; i++) { V2 v = a2_arr_get(q.u.o, (int)i); if (v.t == T_OBJ && v.u.o == o) return; }
    a2_arr_push(vm, q.u.o, v2_obj(o));
}

/* --- LoaderInfo --- */
static A2Obj *li_loader_obj(VM2 *vm, V2 li) { V2 l = dget(vm, li, "__loader"); return l.t == T_OBJ ? l.u.o : NULL; }
static void li_not_ready(VM2 *vm) { a2_throw_error(vm, "Error", "The loading object is not sufficiently loaded to provide this information.", 2099); }
static SWFMovie *li_movie(VM2 *vm, V2 self) { LoadState *st = ls_of(SELFO); if (!st) return vm->player->movie; return st->ready && st->ctype == LT_SWF ? st->movie : NULL; }
static V2 LI_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_undef(); }
static V2 li_bytesLoaded(VM2 *vm, V2 self, int argc, V2 *argv) { LoadState *st = ls_of(SELFO); UNUSED; if (SELFO && !st && li_loader_obj(vm, self)) return v2_uint(0); return v2_uint(st ? st->loaded : (u32)vm->player->movie->file_len); }
static V2 li_bytesTotal(VM2 *vm, V2 self, int argc, V2 *argv) { LoadState *st = ls_of(SELFO); UNUSED; if (SELFO && !st && li_loader_obj(vm, self)) return v2_uint(0); return v2_uint(st ? st->total : (u32)vm->player->movie->file_len); }
static V2 li_url(VM2 *vm, V2 self, int argc, V2 *argv) { LoadState *st = ls_of(SELFO); UNUSED; if (li_loader_obj(vm, self)) return st && st->ready ? dget(vm, self, "__url") : v2_null(); return v2_cstr(vm, movie_url(vm->player->movie)); }
static V2 li_loaderURL(VM2 *vm, V2 self, int argc, V2 *argv) { V2 u = dget(vm, self, "__loaderURL"); UNUSED; return u.t == T_STR ? u : v2_cstr(vm, movie_url(vm->player->movie)); }
static V2 li_parameters(VM2 *vm, V2 self, int argc, V2 *argv) { V2 p = dget(vm, self, "__params"); (void)argc; (void)argv; if (p.t == T_OBJ) return p; p = v2_obj(a2_new_plain(vm)); dset(vm, self, "__params", p); return p; }
static int li_check(VM2 *vm, V2 self) { LoadState *st = ls_of(SELFO); if (li_loader_obj(vm, self) && !(st && st->ready)) { li_not_ready(vm); return 0; } return 1; }
static V2 li_width(VM2 *vm, V2 self, int argc, V2 *argv) { SWFMovie *m; UNUSED; if (!li_check(vm, self)) return v2_undef(); m = li_movie(vm, self); if (!m) { V2 c = dget(vm, self, "__content"); return c.t == T_OBJ ? a2_get_s(vm, c, "width") : v2_int(0); } return v2_int((m->stage.xmax - m->stage.xmin) / 20); }
static V2 li_height(VM2 *vm, V2 self, int argc, V2 *argv) { SWFMovie *m; UNUSED; if (!li_check(vm, self)) return v2_undef(); m = li_movie(vm, self); if (!m) { V2 c = dget(vm, self, "__content"); return c.t == T_OBJ ? a2_get_s(vm, c, "height") : v2_int(0); } return v2_int((m->stage.ymax - m->stage.ymin) / 20); }
static V2 li_allows(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; if (!li_check(vm, self)) return v2_undef(); return v2_bool(1); }
/* SWF properties of loaded non-SWF content (an image) throw #2098 */
static int li_swf_check(VM2 *vm, V2 self) { LoadState *st; if (!li_check(vm, self)) return 0; st = ls_of(SELFO); if (li_loader_obj(vm, self) && st && st->ready && st->ctype != LT_SWF) { a2_throw_error(vm, "Error", "The loading object is not a .swf file, you cannot request SWF properties from it.", 2098); return 0; } return 1; }
/* isURLInaccessible: true until a loader's content arrives */
static V2 li_isURLInaccessible(VM2 *vm, V2 self, int argc, V2 *argv) { LoadState *st = ls_of(SELFO); UNUSED; return v2_bool(li_loader_obj(vm, self) && !(st && st->ready)); }
static V2 li_frameRate(VM2 *vm, V2 self, int argc, V2 *argv) { SWFMovie *m; UNUSED; if (!li_swf_check(vm, self)) return v2_undef(); m = li_movie(vm, self); if (!m) m = vm->player->movie; return v2_num(m->framerate); }
static V2 li_swfVersion(VM2 *vm, V2 self, int argc, V2 *argv) { SWFMovie *m; UNUSED; if (!li_swf_check(vm, self)) return v2_undef(); m = li_movie(vm, self); if (!m) m = vm->player->movie; return v2_uint(m->version); }
static V2 li_asVersion(VM2 *vm, V2 self, int argc, V2 *argv) { SWFMovie *m; UNUSED; if (!li_swf_check(vm, self)) return v2_undef(); m = li_movie(vm, self); return v2_uint(m && !m->has_abc ? 2 : 3); }
static V2 li_contentType(VM2 *vm, V2 self, int argc, V2 *argv) { LoadState *st = ls_of(SELFO); UNUSED; if (dget(vm, self, "__isStage").t == T_BOOL) return v2_null(); if (li_loader_obj(vm, self)) return st && st->ready && ctype_name(st->ctype) ? v2_cstr(vm, ctype_name(st->ctype)) : v2_null(); return v2_cstr(vm, "application/x-shockwave-flash"); }
static V2 li_content(VM2 *vm, V2 self, int argc, V2 *argv) { V2 c = dget(vm, self, "__content"); (void)argc; (void)argv; if (c.t == T_OBJ) return c; if (li_loader_obj(vm, self)) return v2_null(); return vm->root_obj ? v2_obj(vm->root_obj) : (vm->player->root->obj2 ? v2_obj((A2Obj*)vm->player->root->obj2) : v2_null()); }
static V2 li_loader(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *l = li_loader_obj(vm, self); UNUSED; return l ? v2_obj(l) : v2_null(); }
static V2 LoaderContext_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    dset(vm, self, "checkPolicyFile", v2_bool(argc > 0 && BOOL(0))); dset(vm, self, "applicationDomain", argc > 1 ? ARG(1) : v2_null()); dset(vm, self, "securityDomain", argc > 2 ? ARG(2) : v2_null());
    dset(vm, self, "allowCodeImport", v2_bool(1)); dset(vm, self, "allowLoadBytesCodeExecution", v2_bool(1)); dset(vm, self, "imageDecodingPolicy", v2_cstr(vm, "onDemand")); dset(vm, self, "parameters", v2_null()); dset(vm, self, "requestedContentParent", v2_null());
    return v2_undef();
}
static V2 JPEGLoaderContext_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 a[3]; int i; for (i = 0; i < 3; i++) a[i] = argc > i + 1 ? argv[i + 1] : (i == 0 ? v2_bool(0) : v2_null());
    LoaderContext_ctor(vm, self, argc > 1 ? argc - 1 : 0, a);
    dset(vm, self, "deblockingFilter", v2_num(argc > 0 ? NUM(0) : 0));
    return v2_undef();
}
static V2 li_appDomain(VM2 *vm, V2 self, int argc, V2 *argv) {
    LoadState *st = ls_of(SELFO); extern V2 a2_domain_wrapper(VM2 *vm, int d); (void)argc; (void)argv;
    if (li_loader_obj(vm, self)) { if (!(st && st->ready)) return v2_null(); return a2_domain_wrapper(vm, st->movie && st->movie->a2_domain > 0 ? st->movie->a2_domain : 1); }
    return a2_domain_wrapper(vm, 1);   /* the main movie's domain */
}
static A2Obj *bytes_of_movie(VM2 *vm, SWFMovie *m) {
    A2Obj *ba = a2_new(vm, vm->c_bytearray); extern void a2_bytearray_set(VM2 *vm, A2Obj *o, int i, int v); u32 tl = (u32)m->bodylen + 8; size_t i;
    ba->kind = OK_BYTEARRAY;
    a2_bytearray_set(vm, ba, (int)tl - 1, 0);
    if (ba->blen < (int)tl) return ba;
    ba->bytes[0] = 'F'; ba->bytes[1] = 'W'; ba->bytes[2] = 'S'; ba->bytes[3] = m->version;
    for (i = 0; i < 4; i++) ba->bytes[4 + i] = (u8)(tl >> (8 * i));
    memcpy(ba->bytes + 8, m->body, m->bodylen);
    return ba;
}
static V2 li_bytes(VM2 *vm, V2 self, int argc, V2 *argv) {
    LoadState *st = ls_of(SELFO); V2 b = dget(vm, self, "__bytes"); A2Obj *ba; UNUSED;
    if (b.t == T_OBJ) return b;
    if (li_loader_obj(vm, self)) {
        if (!st) return v2_null();
        ba = (st->ctype == LT_SWF && st->movie) ? bytes_of_movie(vm, st->movie) : NULL;
        if (!ba) { ba = a2_new(vm, vm->c_bytearray); ba->kind = OK_BYTEARRAY; return v2_obj(ba); }
    } else ba = bytes_of_movie(vm, vm->player->movie);
    dset(vm, self, "__bytes", v2_obj(ba));
    return v2_obj(ba);
}
static V2 li_uncaughtErrorEvents(VM2 *vm, V2 self, int argc, V2 *argv) { V2 u = dget(vm, self, "__uee"); (void)argc; (void)argv; if (u.t == T_OBJ) return u; { A2Class *uc = a2_class_find_s(vm, "flash.events::UncaughtErrorEvents"); u = a2_construct_class(vm, uc ? uc : vm->c_eventdispatcher, 0, NULL); } dset(vm, self, "__uee", u); return u; }
static V2 li_sharedEvents(VM2 *vm, V2 self, int argc, V2 *argv) { V2 u = dget(vm, self, "__shared"); (void)argc; (void)argv; if (u.t == T_OBJ) return u; u = a2_construct_class(vm, vm->c_eventdispatcher, 0, NULL); dset(vm, self, "__shared", u); return u; }

/* --- Loader --- */
static V2 ld_li(VM2 *vm, V2 self) {   /* the LoaderInfo exists from allocation on (usable before super()) */
    V2 li = dget(vm, self, "__li");
    if (li.t == T_OBJ || self.t != T_OBJ) return li;
    li = a2_construct_class(vm, vm->c_loaderinfo, 0, NULL);
    dset(vm, li, "__loader", self); dset(vm, self, "__li", li);
    dset(vm, li, "__loaderURL", v2_cstr(vm, movie_url(a2_code_movie(vm))));
    return li;
}
static V2 Loader_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    extern V2 a2_displayobject_ctor_pub(VM2 *vm, V2 self); (void)argc; (void)argv;
    a2_displayobject_ctor_pub(vm, self);
    ld_li(vm, self);
    return v2_undef();
}
static V2 ld_contentLoaderInfo(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return ld_li(vm, self); }
static V2 ld_content(VM2 *vm, V2 self, int argc, V2 *argv) { V2 li = dget(vm, self, "__li"); (void)argc; (void)argv; { V2 c = dget(vm, li, "__content"); return c.t == T_OBJ ? c : v2_null(); } }
static V2 ld_unsupported(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; a2_throw_error(vm, "flash.errors::IllegalOperationError", "The Loader class does not implement this method.", 2069); return v2_undef(); }

/* take the current content away (unload event first, then the removal) */
static void ld_drop(VM2 *vm, V2 self, int fire_unload) {
    V2 li = dget(vm, self, "__li"), c = dget(vm, li, "__content"); extern void a2_container_remove(VM2 *vm, A2Obj *c);
    if (li.t != T_OBJ) return;
    if (c.t == T_OBJ) {
        if (fire_unload) fire(vm, li.u.o, "unload");
        a2_container_remove(vm, c.u.o);
        { A2Disp *cd = a2_disp(c.u.o); (void)cd; }
    }
    dset(vm, li, "__content", v2_null()); dset(vm, li, "__bytes", v2_undef()); dset(vm, li, "__params", v2_undef()); dset(vm, li, "__url", v2_null());
    free(li.u.o->payload); li.u.o->payload = NULL;
}

static char *sdup(const char *s) { size_t n = strlen(s); char *d = (char*)malloc(n + 1); if (d) memcpy(d, s, n + 1); return d; }
/* the test harness's navigator log (-L): built when the request is made, printed when it is fetched */
static void stash_fetch_log(VM2 *vm, A2Obj *holder, V2 req) {
    extern int g_fetch_log; char *buf; size_t cap = 4096, n = 0; V2 u, m, data, hdrs; const char *method; int post;
    char *names[64], *vals[64]; int nh = 0, i;
    if (!g_fetch_log || req.t != T_OBJ) return;
    buf = (char*)malloc(cap); if (!buf) return;
#define APP(str) do { const char *s_ = (str); size_t l_ = strlen(s_); while (n + l_ + 1 > cap) { char *nb_; cap *= 2; nb_ = (char*)realloc(buf, cap); if (!nb_) { free(buf); return; } buf = nb_; } memcpy(buf + n, s_, l_); n += l_; buf[n] = 0; } while (0)
    u = a2_get_s(vm, req, "url"); m = a2_get_s(vm, req, "method"); data = a2_get_s(vm, req, "data"); hdrs = a2_get_s(vm, req, "requestHeaders");
    if (vm->throwing) { vm->throwing = 0; free(buf); return; }
    method = m.t == T_STR ? m.u.s->s : "GET";
    post = !strcmp(method, "POST") || !strcmp(method, "post");
    {   /* headers: later entries replace earlier ones of the same name, keeping the first position */
        if (hdrs.t == T_OBJ && hdrs.u.o->kind == OK_ARRAY) {
            u32 k, len = a2_arr_len(hdrs.u.o);
            for (k = 0; k < len && nh < 64; k++) {
                V2 h = a2_arr_get(hdrs.u.o, (int)k), hn, hv; const char *ns_, *vs_; int j;
                if (h.t != T_OBJ || !h.u.o->cls || strcmp(h.u.o->cls->name->s, "URLRequestHeader")) continue;
                hn = a2_get_s(vm, h, "name"); hv = a2_get_s(vm, h, "value");
                ns_ = v2_tostr(vm, hn)->s; vs_ = v2_tostr(vm, hv)->s;
                for (j = 0; j < nh; j++) if (!strcmp(names[j], ns_)) break;
                if (j < nh) { free(vals[j]); vals[j] = sdup(vs_); }
                else { names[nh] = sdup(ns_); vals[nh] = sdup(vs_); nh++; }
            }
        }
    }
    {
        char *url = sdup(u.t == T_NULL || u.t == T_UNDEF ? "null" : v2_tostr(vm, u)->s);
        int has_body = 0; A2Obj *ba = NULL; const char *sdata = NULL; const char *ctype = "application/x-www-form-urlencoded";
        if (data.t != T_NULL && data.t != T_UNDEF) {
            if (data.t == T_OBJ && data.u.o->kind == OK_BYTEARRAY) ba = data.u.o; else sdata = v2_tostr(vm, data)->s;
            if (!post) {   /* GET appends the data to the URL */
                char *nu = (char*)malloc(strlen(url) + strlen(sdata ? sdata : "") + 2);
                if (nu) { strcpy(nu, url); if (!strchr(url, '?')) strcat(nu, "?"); strcat(nu, sdata ? sdata : ""); free(url); url = nu; }
            } else {
                V2 ct = a2_get_s(vm, req, "contentType"); if (ct.t == T_STR) ctype = ct.u.s->s;
                has_body = ba ? ba->blen > 0 : (sdata && *sdata);
            }
        }
        APP("Navigator::fetch:\n  URL: "); APP(url); APP("\n  Method: "); APP(has_body ? "POST" : "GET");
        if (nh) { APP("\n  Headers:"); for (i = 0; i < nh; i++) { APP("\n"); APP(names[i]); APP(": "); APP(vals[i]); } }
        if (has_body) {
            APP("\n  Mime-Type: "); APP(ctype);
            if (!strcmp(ctype, "application/x-www-form-urlencoded")) { APP("\n  Body: "); if (ba) { char *t = (char*)malloc((size_t)ba->blen + 1); if (t) { memcpy(t, ba->bytes, (size_t)ba->blen); t[ba->blen] = 0; APP(t); free(t); } } else APP(sdata); }
            else {
                int k; char hx[8]; APP("\n  Body: [");
                if (ba) for (k = 0; k < ba->blen; k++) { sprintf(hx, k ? ", %02X" : "%02X", ba->bytes[k]); APP(hx); }
                else for (k = 0; sdata[k]; k++) { sprintf(hx, k ? ", %02X" : "%02X", (u8)sdata[k]); APP(hx); }
                APP("]");
            }
        }
        free(url);
    }
    for (i = 0; i < nh; i++) { free(names[i]); free(vals[i]); }
#undef APP
    a2_dyn_set_hidden(vm, holder, a2_str(vm, "__fetchLog"), v2_cstr(vm, buf));
    free(buf);
}
static void flush_fetch_log(VM2 *vm, A2Obj *holder) {
    extern void avm1_trace_out(const char *s);
    A2Prop *p = a2_dyn_find(holder, a2_str(vm, "__fetchLog"));
    if (p && p->v.t == T_STR) { avm1_trace_out(p->v.u.s->s); p->v = v2_undef(); }
}
/* the ApplicationDomain a loaded SWF defines its classes in: the LoaderContext's, else a new child of the caller's */
static int ld_domain(VM2 *vm, V2 ctx) {
    V2 ad = ctx.t == T_OBJ ? a2_get_s(vm, ctx, "applicationDomain") : v2_null();
    if (ad.t == T_OBJ && ad.u.o->cls == vm->c_applicationdomain) return ad.u.o->ival > 0 ? ad.u.o->ival : 1;
    return a2_new_domain(vm, a2_cur_domain(vm));
}
static int g_net_phase;   /* the network phase at the end of a frame is delivering loads */
/* load timing: Flash's (data arrives in the network phase ending a frame, never during the movie's
   first frame; content constructs next frame and completes after exitFrame; AVM1 content is added
   and completes at once) or, in the test harness, Ruffle's (its test executor completes local fetches
   right after each tick); the harness asks for Flash's timing for tests recorded in Flash */
static int flash_net(void) { static int v = -1; if (v < 0) v = !getenv("FLASHNT_HARNESS") || getenv("FLASHNT_FLASH_TIMING") != NULL; return v; }
static V2 ld_load(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 li = dget(vm, self, "__li"), req = ARG(0), u; char url[1024]; LoadState *st;
    if (req.t != T_OBJ) { a2_throw_error(vm, "TypeError", "Parameter request must be non-null.", 2007); return v2_undef(); }
    if (li.t != T_OBJ) return v2_undef();
    ld_drop(vm, self, 1);
    u = a2_get_s(vm, req, "url");
    url_resolve(movie_url(a2_code_movie(vm)), u.t == T_STR ? u.u.s->s : "", url, sizeof(url));
    st = ls_reset(li.u.o, NULL, 0); if (!st) return v2_undef();
    dset(vm, li, "__dom", v2_int(ld_domain(vm, argc > 1 ? ARG(1) : v2_null())));
    st->status = LS_REQUESTED; st->frame = vm->frame_no + (g_net_phase ? 1 : 0);   /* a load requested while loads are delivered waits for the next frame */
    dset(vm, li, "__reqURL", v2_cstr(vm, url));
    if (flash_net()) dset(vm, li, "__resname", v2_int(++vm->instance_counter));   /* Flash names the loaded root when load() is called */
    stash_fetch_log(vm, li.u.o, req);
    lq_add(vm, li.u.o);
    return v2_undef();
}
static void ld_got_data(VM2 *vm, A2Obj *li, int from_url);
static V2 ld_loadBytes(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 li = dget(vm, self, "__li"); A2Obj *ba = ARG(0).t == T_OBJ ? ARG(0).u.o : NULL; LoadState *st;
    if (!ba) { a2_throw_error(vm, "TypeError", "Parameter bytes must be non-null.", 2007); return v2_undef(); }
    if (li.t != T_OBJ) return v2_undef();
    ld_drop(vm, self, 1);
    st = ls_reset(li.u.o, ba->bytes, (size_t)(ba->blen > 0 ? ba->blen : 0)); if (!st) return v2_undef();
    st->from_bytes = 1;
    dset(vm, li, "__dom", v2_int(ld_domain(vm, argc > 1 ? ARG(1) : v2_null())));
    {   /* the content's url: the loading SWF's url + "/[[DYNAMIC]]/<n>" */
        static int dyn_counter = 0; char b[1200]; const char *mu = movie_url(a2_code_movie(vm));
        sprintf(b, "%.1100s/[[DYNAMIC]]/%d", mu ? mu : "", ++dyn_counter);
        dset(vm, li, "__reqURL", v2_cstr(vm, b));
        dset(vm, li, "__loaderURL", v2_cstr(vm, mu ? mu : ""));
    }
    ld_got_data(vm, li.u.o, 0);
    return v2_undef();
}
static V2 ld_unload(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; ld_drop(vm, self, 1); return v2_undef(); }
static V2 ld_close(VM2 *vm, V2 self, int argc, V2 *argv) { V2 li = dget(vm, self, "__li"); LoadState *st = li.t == T_OBJ ? ls_of(li.u.o) : NULL; UNUSED; if (st && st->status == LS_REQUESTED) st->status = LS_IDLE; return v2_undef(); }

/* the data is here: progress events, then the content (images from a URL at once) */
static void ld_make_content(VM2 *vm, A2Obj *li);
static void ld_finish(VM2 *vm, A2Obj *li);
static void parse_params(VM2 *vm, A2Obj *li, const char *url) {
    const char *q = strchr(url, '?'); V2 p, a[1];
    if (!q) return;
    p = li_parameters(vm, v2_obj(li), 0, NULL);
    { extern V2 a2_urlvars_decode(VM2 *vm, V2 self, int argc, V2 *argv); a[0] = v2_cstr(vm, q + 1); a2_urlvars_decode(vm, p, 1, a); }
}
static void ld_got_data(VM2 *vm, A2Obj *li, int from_url) {
    LoadState *st = ls_of(li); V2 ru = dget(vm, v2_obj(li), "__reqURL"); const char *url = ru.t == T_STR ? ru.u.s->s : "";
    if (!st) return;
    if (from_url) fire(vm, li, "open");
    st = ls_of(li); if (!st) return;
    st->total = (u32)st->len;
    fire_progress(vm, li, 0, st->total);
    st = ls_of(li); if (!st) return;
    st->loaded = st->total;
    st->ctype = sniff(LS_DATA(st), st->len);
    if (st->ctype == LT_UNKNOWN && st->len > 64 && LS_DATA(st)[0] == 0x30) {
        /* .swz (signed Flex library): a PKCS#7 container with the SWF inside */
        const u8 *dd = LS_DATA(st); size_t i;
        for (i = 0; i + 8 < st->len; i++) if ((dd[i] == 'C' || dd[i] == 'F' || dd[i] == 'Z') && dd[i + 1] == 'W' && dd[i + 2] == 'S' && dd[i + 3] >= 6 && dd[i + 3] < 60) {
            int err; SWFMovie *mm = swf_load_mem(dd + i, st->len - i, &err);
            if (mm) { swf_free(mm); memmove(LS_DATA(st), dd + i, st->len - i); st->len -= i; st->ctype = LT_SWF; break; }
        }
    }
    if (st->ctype == LT_SWF) { int err; st->movie = swf_load_mem(LS_DATA(st), st->len, &err); if (!st->movie) st->ctype = LT_UNKNOWN; else { { V2 dv = dget(vm, v2_obj(li), "__dom"); st->movie->a2_domain = dv.t == T_INT ? dv.u.i : 0; } char *mu = (char*)malloc(strlen(url) + 1); if (mu) strcpy(mu, url); st->movie->url = mu; } }
    if (from_url) parse_params(vm, li, url);
    fire_progress(vm, li, st->loaded, st->total);
    st = ls_of(li); if (!st) return;
    if (st->ctype == LT_UNKNOWN || (st->ctype == LT_GIF)) {
        char b[1200];
        st->status = LS_FAILED;
        if (from_url) sprintf(b, "Error #2124: Loaded file is an unknown type. URL: %.1000s", url); else strcpy(b, "Error #2124: Loaded file is an unknown type.");
        ls_drop_data(li);
        fire_ioerror(vm, v2_obj(li), b, 2124);
        return;
    }
    st->status = LS_HAVE_DATA; st->frame = vm->frame_no;
    if (from_url && st->ctype != LT_SWF) { ld_make_content(vm, li); ld_finish(vm, li); return; }
    if (!flash_net() && from_url && st->movie && st->movie->has_abc) {   /* Ruffle constructs the loaded movie when the load completes */
        ld_make_content(vm, li); st = ls_of(li);
        if (st && st->status == LS_BUILT) st->early = 1;
    }
    if (flash_net() && from_url && st->movie && !st->movie->has_abc) {   /* an AVM1 movie is added at once; init and complete follow (its frame 1 runs next frame) */
        ld_make_content(vm, li); st = ls_of(li);
        if (st && st->status == LS_BUILT) ld_finish(vm, li);
        return;
    }
    lq_add(vm, li);
}

A2Obj *a2_bitmap_from_decoded(VM2 *vm, Bitmap *bm, int transparent);
static void ld_make_content(VM2 *vm, A2Obj *li) {
    LoadState *st = ls_of(li); A2Obj *loader = li_loader_obj(vm, v2_obj(li)), *content = NULL; int from_url;
    extern void a2_container_insert(VM2 *vm, A2Obj *p, A2Obj *c, int index); extern void a2_run_clip_frame_scripts(VM2 *vm, MovieClip *c);
    MovieClip *clip = NULL; V2 ru;
    if (!st || !loader) return;
    from_url = !st->from_bytes; (void)from_url;
    st->status = LS_BUILT;
    ru = dget(vm, v2_obj(li), "__reqURL");
    if (st->ctype != LT_SWF) return;     /* images: built when the load completes */
    if (st->ctype == LT_SWF && st->movie) {
        SWFMovie *m = st->movie; extern MovieClip *player_make_movie_clip(Player *p, SWFMovie *m, DisplayObject *self);
        if (!m->has_abc) {
            /* an ActionScript 1/2 movie: its main timeline runs in the AVM1 VM; the AVM2 side
               sees it as an AVM1Movie (Ruffle loader.rs: set_avm1movie, LOADER_INSERTED_AVM1_DEPTH) */
            A2Class *ac = a2_class_find_s(vm, "flash.display::AVM1Movie");
            DisplayObject *rec = (DisplayObject*)calloc(1, sizeof(DisplayObject)); extern void avm1_mix_start(Player *p, MovieClip *c);
            if (!rec) return;
            m->a1mix = 1;
            rec->depth = -61440; rec->char_id = -1; rec->visible = 1;
            swf_matrix_identity(&rec->matrix); swf_cxform_identity(&rec->cxform);
            clip = player_make_movie_clip(vm->player, m, rec);
            if (!clip) { free(rec); return; }
            rec->clip = clip;
            if (ac && ac->abc && !ac->inited) a2_class_init(vm, ac);
            content = a2_new(vm, ac && a2_is_subclass(ac, vm->c_displayobject) ? ac : vm->c_sprite); content->kind = OK_DISPLAY;
            content->dobj = rec; rec->obj2 = content; content->clip = clip; clip->obj2 = content;
            a2_disp(content)->is_movie_root = 1; a2_disp(content)->loader_info = li; a2_disp(content)->owns = 1;
            content->ival |= 1;   /* constructed */
            avm1_mix_start(vm->player, clip);
            st = ls_of(li); if (!st) return;
            /* Flash: a URL-loaded AVM1 movie shows its first frame before init (its scripts run in the next AVM1 frame) */
            if (flash_net() && !st->from_bytes) { player_run_first_frame(clip); }
            st->a1_wait = flash_net() ? st->from_bytes : 1;
            clip = NULL;   /* no AVM2 frame scripts */
        } else {
            DisplayObject *rec = (DisplayObject*)calloc(1, sizeof(DisplayObject)); extern void avm2_object_created(Player *p, DisplayObject *d); extern void avm2_retry_symbols(VM2 *vm);
            if (!rec) return;
            rec->depth = -1; rec->char_id = -1; rec->visible = 1;
            swf_matrix_identity(&rec->matrix); swf_cxform_identity(&rec->cxform);
            clip = player_make_movie_clip(vm->player, m, rec);
            if (!clip) { free(rec); return; }
            rec->clip = clip;
            { V2 rn = dget(vm, v2_obj(li), "__resname"); if (rn.t == T_INT) sprintf(rec->name, "instance%d", (int)rn.u.i); }
            player_run_first_frame(clip);
            clip->ran_this_tick = 0;   /* built after this frame's timelines ran: it advances next frame */
            avm2_retry_symbols(vm);
            content = a2_display_obj(vm, clip, rec);
            a2_disp(content)->is_movie_root = 1; a2_disp(content)->loader_info = li; a2_disp(content)->owns = 1;
            avm2_object_created(vm->player, rec);
            { int nu = vm->nuncaught; a2_construct_display(vm, clip, rec); if (vm->nuncaught != nu) { st = ls_of(li); if (st) st->status = LS_FAILED; ls_drop_data(li); return; } }   /* the constructor threw: the load stops there */
            if (from_url && flash_net()) { a2_dispatch_simple(vm, content, vm->s_added, 1); if (vm->throwing) a2_report_uncaught(vm); }   /* Flash tells a URL-loaded root 'added' once before it joins the Loader */
        }
    }
    st = ls_of(li); if (!st || !content) return;
    st->ready = 1;
    dset(vm, v2_obj(li), "__content", v2_obj(content));
    dset(vm, v2_obj(li), "__url", ru);
    ls_drop_data(li);
    a2_container_insert(vm, loader, content, 0);
    { DisplayObject *cr = a2_dobj_of(content); if (cr && cr->clip && CLIP_A1MIX(cr->clip)) cr->depth = -61440; }   /* the AVM1 _level of the loaded movie */
    if (vm->throwing) a2_report_uncaught(vm);
    /* frame 1 scripts: in the frame script phase when on the display list, here otherwise */
    if (clip && !clip->dead && !a2_on_stage(vm, content)) a2_run_clip_frame_scripts(vm, clip);
    if (vm->throwing) a2_report_uncaught(vm);
}
/* the load completes: images get their Bitmap now; init, httpStatus, complete */
static void ld_finish(VM2 *vm, A2Obj *li) {
    LoadState *st = ls_of(li); A2Obj *loader = li_loader_obj(vm, v2_obj(li)), *content = NULL; int from_url;
    extern void a2_container_insert(VM2 *vm, A2Obj *p, A2Obj *c, int index);
    if (!st || !loader || st->status != LS_BUILT) return;
    from_url = !st->from_bytes;
    st->status = LS_DONE;
    if (st->ctype != LT_SWF) {
        int jxr_alpha = 0;
        Bitmap *bm = st->ctype == LT_JXR ? jxr_decode(LS_DATA(st), st->len, &jxr_alpha) : image_decode_bytes(LS_DATA(st), st->len);
        if (!bm && st->ctype == LT_PNG && st->len >= 24 && !memcmp(LS_DATA(st) + 12, "IHDR", 4)) {   /* a PNG with broken image data still gives a (blank) bitmap of its size */
            const u8 *q = LS_DATA(st) + 16; u32 w = ((u32)q[0] << 24) | ((u32)q[1] << 16) | ((u32)q[2] << 8) | q[3], h = ((u32)q[4] << 24) | ((u32)q[5] << 16) | ((u32)q[6] << 8) | q[7];
            if (w > 0 && h > 0 && w <= 8191 && h <= 8191 && (u64)w * h <= 16777215u) { bm = (Bitmap*)calloc(1, sizeof(Bitmap)); if (bm) { bm->w = (int)w; bm->h = (int)h; bm->px = (u32*)calloc((size_t)w * h, 4); if (!bm->px) { free(bm); bm = NULL; } } }
        }
        if (bm) content = a2_bitmap_from_decoded(vm, bm, st->ctype == LT_PNG ? png_has_alpha(LS_DATA(st), st->len) : st->ctype == LT_JXR ? jxr_alpha : 0);
        if (!content) { st->status = LS_FAILED; ls_drop_data(li); fire_ioerror(vm, v2_obj(li), "Error #2124: Loaded file is an unknown type.", 2124); return; }
        st->ready = 1;
        dset(vm, v2_obj(li), "__content", v2_obj(content));
        dset(vm, v2_obj(li), "__url", dget(vm, v2_obj(li), "__reqURL"));
        ls_drop_data(li);
        a2_container_insert(vm, loader, content, 0);
        if (vm->throwing) a2_report_uncaught(vm);
    }
    fire(vm, li, "init");
    if (from_url) fire_status(vm, li, 0);
    fire(vm, li, "complete");
}

static V2 PrintJobOptions_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { dset(vm, self, "printAsBitmap", v2_bool(argc > 0 && BOOL(0))); dset(vm, self, "printMethod", v2_cstr(vm, "auto")); dset(vm, self, "pixelsPerInch", v2_num(0.0 / 0.0)); return v2_undef(); }
/* no game controllers here */
static V2 gi_getDeviceAt(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; a2_throw_id(vm, "RangeError", 1506, NULL, NULL); return v2_undef(); }
/* --- URLLoader --- */
static V2 ul_load(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 req = ARG(0), u; char url[1024]; LoadState *st;
    if (!SELFO) return v2_undef();
    if (req.t != T_OBJ) { a2_throw_error(vm, "TypeError", "Parameter request must be non-null.", 2007); return v2_undef(); }
    u = a2_get_s(vm, req, "url");
    url_resolve(movie_url(a2_code_movie(vm)), u.t == T_STR ? u.u.s->s : "", url, sizeof(url));
    st = ls_reset(SELFO, NULL, 0); if (!st) return v2_undef();
    st->status = LS_REQUESTED; st->frame = vm->frame_no; st->urlloader = 1;
    dset(vm, self, "__reqURL", v2_cstr(vm, url));
    stash_fetch_log(vm, SELFO, req);
    lq_add(vm, SELFO);
    return v2_undef();
}
/* URLStream: a load whose bytes are read through IDataInput (delegated to a ByteArray) */
static V2 us_load(VM2 *vm, V2 self, int argc, V2 *argv) { LoadState *st; ul_load(vm, self, argc, argv); st = ls_of(SELFO); if (st) st->urlloader = 2; return v2_undef(); }
static V2 us_ba(VM2 *vm, V2 self) { V2 b = dget(vm, self, "__ba"); if (b.t != T_OBJ) { A2Obj *ba = a2_new(vm, vm->c_bytearray); ba->kind = OK_BYTEARRAY; b = v2_obj(ba); if (SELFO) a2_dyn_set_hidden(vm, SELFO, a2_str(vm, "__ba"), b); } return b; }
#define US_DELEGATE(fn, name) static V2 fn(VM2 *vm, V2 self, int argc, V2 *argv) { return a2_call_s(vm, us_ba(vm, self), name, argc, argv); }
US_DELEGATE(us_readByte, "readByte") US_DELEGATE(us_readUnsignedByte, "readUnsignedByte") US_DELEGATE(us_readShort, "readShort") US_DELEGATE(us_readUnsignedShort, "readUnsignedShort")
US_DELEGATE(us_readInt, "readInt") US_DELEGATE(us_readUnsignedInt, "readUnsignedInt") US_DELEGATE(us_readFloat, "readFloat") US_DELEGATE(us_readDouble, "readDouble") US_DELEGATE(us_readBoolean, "readBoolean")
US_DELEGATE(us_readUTF, "readUTF") US_DELEGATE(us_readUTFBytes, "readUTFBytes") US_DELEGATE(us_readMultiByte, "readMultiByte") US_DELEGATE(us_readBytes, "readBytes") US_DELEGATE(us_readObject, "readObject")
static V2 us_bytesAvailable(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return a2_get_s(vm, us_ba(vm, self), "bytesAvailable"); }
static V2 us_endian_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return a2_get_s(vm, us_ba(vm, self), "endian"); }
static V2 us_endian_set(VM2 *vm, V2 self, int argc, V2 *argv) { V2 b = us_ba(vm, self); if (b.t == T_OBJ) a2_set_s(vm, b, "endian", ARG(0)); return v2_undef(); }
static V2 URLLoader_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { dset(vm, self, "data", v2_undef()); dset(vm, self, "dataFormat", v2_cstr(vm, "text")); dset(vm, self, "bytesLoaded", v2_uint(0)); dset(vm, self, "bytesTotal", v2_uint(0)); if (argc && ARG(0).t == T_OBJ) ul_load(vm, self, argc, argv); return v2_undef(); }
static void ul_got_data(VM2 *vm, A2Obj *o) {
    LoadState *st = ls_of(o); V2 fmt = dget(vm, v2_obj(o), "dataFormat"); const char *f = fmt.t == T_STR ? fmt.u.s->s : "text"; V2 data;
    fire(vm, o, "open");
    st = ls_of(o); if (!st) return;
    st->total = st->loaded = (u32)st->len;
    if (st->urlloader == 2) {   /* URLStream: the bytes are readable from the progress event on */
        A2Obj *ba = a2_new(vm, vm->c_bytearray); extern void a2_bytearray_set(VM2 *vm, A2Obj *o, int i, int v);
        ba->kind = OK_BYTEARRAY;
        if (st->len) { a2_bytearray_set(vm, ba, (int)st->len - 1, 0); if (ba->blen >= (int)st->len) memcpy(ba->bytes, LS_DATA(st), st->len); }
        ba->bpos = 0;
        a2_dyn_set_hidden(vm, o, a2_str(vm, "__ba"), v2_obj(ba));
        fire_progress(vm, o, st->loaded, st->total);
        st = ls_of(o); if (!st) return;
        st->status = LS_DONE; ls_drop_data(o);
        fire(vm, o, "complete");
        return;
    }
    dset(vm, v2_obj(o), "bytesTotal", v2_uint(st->total));
    dset(vm, v2_obj(o), "bytesLoaded", v2_uint(st->loaded));
    fire_progress(vm, o, st->loaded, st->total);
    st = ls_of(o); if (!st) return;
    if (st->urlloader == 2 || !strcmp(f, "binary")) {
        A2Obj *ba = a2_new(vm, vm->c_bytearray); extern void a2_bytearray_set(VM2 *vm, A2Obj *o, int i, int v);
        ba->kind = OK_BYTEARRAY;
        if (st->len) { a2_bytearray_set(vm, ba, (int)st->len - 1, 0); if (ba->blen >= (int)st->len) memcpy(ba->bytes, LS_DATA(st), st->len); }
        ba->bpos = 0;
        data = v2_obj(ba);
    } else {
        const u8 *d = LS_DATA(st); size_t n = st->len;
        if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) { d += 3; n -= 3; }
        if (n >= 2 && ((d[0] == 0xFF && d[1] == 0xFE) || (d[0] == 0xFE && d[1] == 0xFF))) {   /* UTF-16 with a byte order mark */
            int le = d[0] == 0xFF; size_t k, m = (n - 2) / 2; u32 *u = (u32*)malloc((m + 1) * sizeof(u32)); extern A2Str *a2_utf8_encode(VM2 *vm, const u32 *u, int n);
            if (u) { for (k = 0; k < m; k++) u[k] = le ? (u32)d[2 + 2 * k] | ((u32)d[3 + 2 * k] << 8) : ((u32)d[2 + 2 * k] << 8) | d[3 + 2 * k]; data = v2_str(a2_utf8_encode(vm, u, (int)m)); free(u); }
            else data = v2_str(vm->s_empty);
        } else data = v2_str(a2_intern(vm, (const char*)d, n));
        if (!strcmp(f, "variables")) { A2Class *uv = a2_class_find_s(vm, "flash.net::URLVariables"); V2 a[1]; a[0] = data; data = a2_construct_class(vm, uv, 1, a); if (vm->throwing) { a2_report_uncaught(vm); data = v2_undef(); } }
    }
    st->status = LS_DONE;
    ls_drop_data(o); st = ls_of(o);   /* the state may have moved */
    if (st && st->urlloader == 2) { a2_dyn_set_hidden(vm, o, a2_str(vm, "__ba"), data); fire(vm, o, "complete"); return; }
    dset(vm, v2_obj(o), "data", data);
    fire_status(vm, o, 0);
    fire(vm, o, "complete");
}

/* end of frame: move the loads along */
static int loader_pass(VM2 *vm);
/* frame construction: loaded SWFs whose data came in an earlier frame get their main timeline built */
void a2_loader_construct(VM2 *vm) {
    V2 q = dget(vm, v2_obj(vm->global), "__loadq"); A2Obj *items[256]; int n = 0, i; u32 k, len;
    if (q.t != T_OBJ) return;
    len = a2_arr_len(q.u.o);
    for (k = 0; k < len && n < 256; k++) { V2 v = a2_arr_get(q.u.o, (int)k); if (v.t == T_OBJ) items[n++] = v.u.o; }
    /* (Flash builds the loads newest first) */
    for (i = 0; i < n; i++) { A2Obj *it = items[flash_net() ? n - 1 - i : i]; LoadState *st = ls_of(it); if (st && !st->urlloader && st->status == LS_HAVE_DATA && st->frame < vm->frame_no) ld_make_content(vm, it); }
}
void a2_loader_tick(VM2 *vm) { int g = 0; while (loader_pass(vm) && g++ < 16) {} }
/* the network phase ending a frame (after the AVM1 frame) */
void a2_net_phase(VM2 *vm) { if (!flash_net()) return; g_net_phase = 1; a2_loader_tick(vm); g_net_phase = 0; }
/* after exitFrame: loaded content built this frame fires init and complete (Ruffle LoadManager::run_exit_frame);
   with Ruffle's timing every load moves along here */
static int g_finish_only;
void a2_loader_exit_frame(VM2 *vm) { g_finish_only = flash_net(); a2_loader_tick(vm); g_finish_only = 0; }
/* after the AVM1 frame (Ruffle Avm1::run_frame -> movie_clip_on_load): loaded AVM1 movies that ran
   their first frame complete now */
void a2_loader_avm1_done(VM2 *vm) {
    V2 q = dget(vm, v2_obj(vm->global), "__loadq"); u32 k, len; int any = 0;
    if (q.t != T_OBJ) return;
    len = a2_arr_len(q.u.o);
    for (k = 0; k < len; k++) { V2 v = a2_arr_get(q.u.o, (int)k); LoadState *st = v.t == T_OBJ ? ls_of(v.u.o) : NULL; if (st && st->a1_wait) { st->a1_wait = 0; any = 1; } }
    if (any) a2_loader_tick(vm);
}
void avm2_avm1_frame_done(Player *p) { if (p->avm2) { a2_loader_avm1_done((VM2*)p->avm2); if (((VM2*)p->avm2)->throwing) a2_report_uncaught((VM2*)p->avm2); } }
/* one pass over the queue; returns whether anything moved */
static int loader_pass(VM2 *vm) {
    V2 q = dget(vm, v2_obj(vm->global), "__loadq"); A2Obj *items[256]; int n = 0, i, moved = 0; u32 k, len;
    if (q.t != T_OBJ) return 0;
    len = a2_arr_len(q.u.o);
    for (k = 0; k < len && n < 256; k++) { V2 v = a2_arr_get(q.u.o, (int)k); if (v.t == T_OBJ) items[n++] = v.u.o; }
    a2_arr_setlen(vm, q.u.o, 0);
    for (i = 0; i < n; i++) {
        A2Obj *o = items[g_finish_only ? n - 1 - i : i]; LoadState *st = ls_of(o);   /* (finished loads complete newest first) */
        if (!st) continue;
        if (g_finish_only && (st->urlloader || st->status != LS_BUILT)) { lq_add(vm, o); continue; }
        if (st->status == LS_REQUESTED && st->frame <= vm->frame_no && (st->urlloader || vm->frame_no >= 1 || !flash_net())) {   /* Flash: nothing arrives during the movie's first frame */
            moved = 1;
            flush_fetch_log(vm, o);
            V2 ru = dget(vm, v2_obj(o), "__reqURL"); char path[1100]; u8 *data = NULL; size_t dl = 0;
            const char *url = ru.t == T_STR ? ru.u.s->s : "";
            if (url_to_path(vm->player, url, path, sizeof(path))) data = read_file(path, &dl);
            if (!data) {
                char b[1200];
                st->status = LS_FAILED;
                if (st->urlloader) {
                    /* the failed load leaves empty data of the requested format */
                    V2 fmt = dget(vm, v2_obj(o), "dataFormat");
                    if (st->urlloader == 1) { if (fmt.t == T_STR && !strcmp(fmt.u.s->s, "binary")) { A2Obj *ba = a2_new(vm, vm->c_bytearray); ba->kind = OK_BYTEARRAY; dset(vm, v2_obj(o), "data", v2_obj(ba)); } else dset(vm, v2_obj(o), "data", v2_str(vm->s_empty)); }
                    fire_status(vm, o, 0); fire_ioerror(vm, v2_obj(o), "Error #2032: Stream Error", 2032);
                }
                else { sprintf(b, "Error #2035: URL Not Found. URL: %.1000s", url); fire_ioerror(vm, v2_obj(o), b, 2035); }
                continue;
            }
            { int ul = st->urlloader; st = ls_reset(o, data, dl); free(data); if (!st) continue; st->urlloader = ul; }
            if (st->urlloader) ul_got_data(vm, o); else ld_got_data(vm, o, 1);
        } else if (st->status == LS_HAVE_DATA && st->frame < vm->frame_no) { moved = 1; ld_make_content(vm, o); st = ls_of(o); if (st && !st->a1_wait) ld_finish(vm, o); }
        else if (st->status == LS_BUILT && !st->a1_wait && !(st->early && st->frame >= vm->frame_no)) { moved = 1; ld_finish(vm, o); }
        if ((st = ls_of(o)) != NULL && (st->status == LS_REQUESTED || st->status == LS_HAVE_DATA || st->status == LS_BUILT)) lq_add(vm, o);
    }
    return moved;
}
V2 a2_urlvars_decode(VM2 *vm, V2 self, int argc, V2 *argv) { A2Str *s = STR(0); const char *p = s->s; while (*p) { const char *amp = strchr(p, '&'); const char *eq; size_t n = amp ? (size_t)(amp - p) : strlen(p); eq = memchr(p, '=', n); if (eq) { A2Str *k = a2_intern(vm, p, (size_t)(eq - p)); V2 v = v2_str(a2_intern(vm, eq + 1, n - (size_t)(eq - p) - 1)); { extern V2 a2_uri_decode_pub(VM2 *vm, A2Str *s); v = a2_uri_decode_pub(vm, v.u.s); k = a2_uri_decode_pub(vm, k).u.s; } if (SELFO) a2_dyn_set(vm, SELFO, k, v); } p = amp ? amp + 1 : p + n; } return v2_undef(); }
V2 a2_uri_decode_pub(VM2 *vm, A2Str *s) { char *buf = (char*)malloc(s->len + 1); size_t o = 0; u32 i; V2 r; for (i = 0; i < s->len; i++) { if (s->s[i] == '%' && i + 2 < s->len + 1) { char h[3]; h[0] = s->s[i + 1]; h[1] = s->s[i + 2]; h[2] = 0; buf[o++] = (char)strtol(h, NULL, 16); i += 2; } else if (s->s[i] == '+') buf[o++] = ' '; else buf[o++] = s->s[i]; } r = v2_str(a2_intern(vm, buf, o)); free(buf); return r; }
/* SharedObject: data is kept as a .sol file (Flash's format: AMF0 or AMF3 body) in the
   local store (FLASHNT_SO_DIR, the player's folder, or memory) under "avm2/<name>" */
static int g_so_default_encoding = 3;
static V2 so_defenc_get(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_uint((u32)g_so_default_encoding); }
static V2 so_defenc_set(VM2 *vm, V2 self, int argc, V2 *argv) { u32 e = v2_touint(vm, ARG(0)); (void)self; if (e == 0 || e == 3) g_so_default_encoding = (int)e; return v2_undef(); }
static int so_enc(VM2 *vm, V2 self) { V2 e = dget(vm, self, "__enc"); return e.t == T_UNDEF ? g_so_default_encoding : (int)v2_touint(vm, e); }
static V2 so_enc_get(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_uint((u32)so_enc(vm, self)); }
static V2 so_enc_set(VM2 *vm, V2 self, int argc, V2 *argv) { u32 e = v2_touint(vm, ARG(0)); (void)argc; if (e == 0 || e == 3) dset(vm, self, "__enc", v2_uint(e)); return v2_undef(); }
static void so_key(const char *name, char *out, size_t n) { sprintf(out, "avm2/%.*s", (int)(n - 8), name); }
/* the .sol bytes of `data` (*empty: no properties to store) */
static A2Obj *so_file(VM2 *vm, const char *name, A2Obj *data, int enc, int *empty) {
    extern void a2_amf_write(VM2 *vm, A2Obj *b, V2 v, int enc); extern void a2_bytearray_write_raw(VM2 *vm, A2Obj *b, const void *p, u32 n); extern A2Obj *a2_bytearray_new(VM2 *vm);
    A2Obj *b = a2_bytearray_new(vm); int idx = 0, any = 0; A2Str *k; V2 v; u8 h[16]; size_t nl = strlen(name); u32 body;
    static const u8 tcso[10] = { 'T', 'C', 'S', 'O', 0, 4, 0, 0, 0, 0 };
    b->ival = 0;
    memset(h, 0, 6); h[1] = 0xBF;
    a2_bytearray_write_raw(vm, b, h, 6);
    a2_bytearray_write_raw(vm, b, tcso, 10);
    h[0] = (u8)(nl >> 8); h[1] = (u8)nl; a2_bytearray_write_raw(vm, b, h, 2); a2_bytearray_write_raw(vm, b, name, (u32)nl);
    h[0] = h[1] = h[2] = 0; h[3] = (u8)enc; a2_bytearray_write_raw(vm, b, h, 4);
    for (;;) {
        a2_dyn_enum(data, &idx, &k, &v); if (!idx) break;
        if (v.t == T_OBJ && v.u.o->kind == OK_FUNCTION) continue;
        any = 1;
        if (enc == 3) { u32 u = (k->len << 1) | 1; int i, nb = u < 0x80 ? 1 : u < 0x4000 ? 2 : u < 0x200000 ? 3 : 4; for (i = nb - 1; i >= 0; i--) { u8 c = (u8)(nb == 4 ? (i == 0 ? u & 0xFF : ((u >> (8 + 7 * (i - 1))) & 0x7F) | 0x80) : (((u >> (7 * i)) & 0x7F) | (i ? 0x80 : 0))); a2_bytearray_write_raw(vm, b, &c, 1); } }
        else { h[0] = (u8)(k->len >> 8); h[1] = (u8)k->len; a2_bytearray_write_raw(vm, b, h, 2); }
        a2_bytearray_write_raw(vm, b, k->s, k->len);
        b->dval = enc; a2_amf_write(vm, b, v, enc);
        h[0] = 0; a2_bytearray_write_raw(vm, b, h, 1);
    }
    body = (u32)b->blen - 6;
    b->bytes[2] = (u8)(body >> 24); b->bytes[3] = (u8)(body >> 16); b->bytes[4] = (u8)(body >> 8); b->bytes[5] = (u8)body;
    *empty = !any;
    return b;
}
static int so_load(VM2 *vm, const char *name, A2Obj *data, int *enc_out) {
    extern V2 a2_amf_read(VM2 *vm, A2Obj *b, int enc); extern A2Obj *a2_bytearray_new(VM2 *vm); extern void a2_bytearray_write_raw(VM2 *vm, A2Obj *b, const void *p, u32 n);
    char key[300]; u8 *d; size_t n; A2Obj *b; int enc; u32 pos, nl;
    so_key(name, key, sizeof(key));
    if (!store_get(key, &d, &n)) return 0;
    if (n < 16 + 2 || d[0] != 0 || d[1] != 0xBF || memcmp(d + 6, "TCSO", 4)) { free(d); return 0; }
    nl = ((u32)d[16] << 8) | d[17]; pos = 18 + nl;
    if (pos + 4 > n) { free(d); return 0; }
    enc = d[pos + 3] == 3 ? 3 : 0; pos += 4;
    b = a2_bytearray_new(vm); a2_bytearray_write_raw(vm, b, d, (u32)n); b->bpos = (int)pos; b->ival = 0;
    free(d);
    while ((u32)b->bpos < (u32)b->blen) {
        A2Str *k; u32 kl = 0; V2 v;
        if (enc == 3) { int i; u32 u = 0; for (i = 0; i < 4 && (u32)b->bpos < (u32)b->blen; i++) { u8 c = b->bytes[b->bpos++]; if (i == 3) { u = (u << 8) | c; break; } u = (u << 7) | (c & 0x7F); if (!(c & 0x80)) break; } kl = u >> 1; }
        else { if ((u32)b->bpos + 2 > (u32)b->blen) break; kl = ((u32)b->bytes[b->bpos] << 8) | b->bytes[b->bpos + 1]; b->bpos += 2; }
        if ((u32)b->bpos + kl > (u32)b->blen) break;
        k = a2_intern(vm, (const char*)b->bytes + b->bpos, kl); b->bpos += (int)kl;
        v = a2_amf_read(vm, b, enc);
        if (vm->throwing) { vm->throwing = 0; vm->exc = v2_undef(); break; }
        a2_dyn_set(vm, data, k, v);
        if ((u32)b->bpos < (u32)b->blen) b->bpos++;   /* trailing 0 */
    }
    if (enc_out) *enc_out = enc;
    return 1;
}
static V2 SharedObject_getLocal(VM2 *vm, V2 self, int argc, V2 *argv) { A2Str *name = STR(0); V2 reg = dget(vm, v2_obj(vm->global), "__sos"), so; A2Obj *data; int enc = -1; (void)self; if (reg.t != T_OBJ) { reg = v2_obj(a2_new_plain(vm)); a2_dyn_set(vm, vm->global, a2_str(vm, "__sos"), reg); } { A2Prop *p = a2_dyn_find(reg.u.o, name); if (p) return p->v; } so = a2_construct_class(vm, vm->c_sharedobject, 0, NULL); data = a2_new_plain(vm);
    so_load(vm, name->s, data, &enc);
    dset(vm, so, "__data", v2_obj(data)); dset(vm, so, "__name", v2_str(name)); dset(vm, so, "__enc", v2_uint((u32)(enc >= 0 ? enc : g_so_default_encoding))); a2_dyn_set(vm, reg.u.o, name, so); return so; }
static V2 so_data(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return dget(vm, self, "__data"); }
static V2 so_flush(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 d = dget(vm, self, "__data"), n = dget(vm, self, "__name"); UNUSED;
    if (d.t == T_OBJ && n.t == T_STR) { char key[300]; int empty; A2Obj *f = so_file(vm, n.u.s->s, d.u.o, so_enc(vm, self), &empty); so_key(n.u.s->s, key, sizeof(key)); if (!empty) store_put(key, f->bytes, (size_t)f->blen); }
    return v2_cstr(vm, "flushed");
}
static V2 so_clear(VM2 *vm, V2 self, int argc, V2 *argv) { V2 n = dget(vm, self, "__name"); UNUSED; dset(vm, self, "__data", v2_obj(a2_new_plain(vm))); if (n.t == T_STR) { char key[300]; so_key(n.u.s->s, key, sizeof(key)); store_remove(key); } return v2_undef(); }
/* size: the .sol file's length (0 without data); AMF0 data: the length of its base64 text
   (the from_shumway expectation; AMF3 matches the file length, avm2/shared_object) */
static V2 so_size(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 d = dget(vm, self, "__data"), n = dget(vm, self, "__name"); int empty, enc = so_enc(vm, self); A2Obj *f; UNUSED;
    if (d.t != T_OBJ || n.t != T_STR) return v2_uint(0);
    f = so_file(vm, n.u.s->s, d.u.o, enc, &empty);
    if (empty) return v2_uint(0);
    if (enc == 0) { extern void a2_amf_write(VM2 *vm, A2Obj *b, V2 v, int enc); extern A2Obj *a2_bytearray_new(VM2 *vm); A2Obj *b = a2_bytearray_new(vm); b->dval = 0; a2_amf_write(vm, b, d, 0); return v2_uint((u32)((b->blen + 2) / 3 * 4)); }
    return v2_uint((u32)f->blen);
}
static V2 so_setProperty(VM2 *vm, V2 self, int argc, V2 *argv) { V2 d = dget(vm, self, "__data"); if (d.t == T_OBJ) a2_dyn_set(vm, d.u.o, STR(0), ARG(1)); return v2_undef(); }
static char *sdup(const char *s);
static V2 navigateToURL(VM2 *vm, V2 self, int argc, V2 *argv) {
    extern int g_fetch_log; extern void avm1_trace_out(const char *s); extern void plat_open_url(const char *url);
    V2 req = ARG(0), u, m, data, tv; const char *target, *ts; char tb[16]; char *url, *log = NULL; size_t cap = 0, n = 0; int post;
    (void)self;
    if (req.t != T_OBJ) { a2_throw_error(vm, "TypeError", "Parameter request must be non-null.", 2007); return v2_undef(); }
    u = a2_get_s(vm, req, "url");
    if (u.t == T_NULL || u.t == T_UNDEF) { a2_throw_error(vm, "TypeError", "Parameter url must be non-null.", 2007); return v2_undef(); }
    tv = ARG(1); target = (tv.t == T_NULL || tv.t == T_UNDEF) ? "null" : v2_tostr(vm, tv)->s;
    ts = target[0] == '_' ? target + 1 : target;   /* normalize_navigate_target */
    if (strlen(ts) == 5 && (ts[0] | 32) == 'b' && (ts[1] | 32) == 'l' && (ts[2] | 32) == 'a' && (ts[3] | 32) == 'n' && (ts[4] | 32) == 'k') { strcpy(tb, "_blank"); target = tb; }
    m = a2_get_s(vm, req, "method"); data = a2_get_s(vm, req, "data");
    post = m.t == T_STR && (!strcmp(m.u.s->s, "POST") || !strcmp(m.u.s->s, "post"));
    url = sdup(v2_tostr(vm, u)->s); if (!url) return v2_undef();
#define LAPP(str) do { const char *s_ = (str); size_t l_ = strlen(s_); if (n + l_ + 1 > cap) { char *nb_; cap = (n + l_ + 1) * 2; nb_ = (char*)realloc(log, cap); if (!nb_) { free(log); free(url); return v2_undef(); } log = nb_; } memcpy(log + n, s_, l_); n += l_; log[n] = 0; } while (0)
    {
        A2Class *uv = a2_class_find_s(vm, "flash.net::URLVariables");
        int is_vars = data.t == T_OBJ && uv && data.u.o->cls && a2_is_subclass(data.u.o->cls, uv);
        if (!is_vars && data.t != T_NULL && data.t != T_UNDEF) {
            const char *sd = v2_tostr(vm, data)->s; char *nu = (char*)malloc(strlen(url) + strlen(sd) + 2);
            if (nu) { strcpy(nu, url); if (!strchr(url, '?')) strcat(nu, "?"); strcat(nu, sd); free(url); url = nu; }
        }
        LAPP("Navigator::navigate_to_url:\n  URL: "); LAPP(url); LAPP("\n  Target: "); LAPP(target); LAPP(post ? "\n  Method: POST" : "\n  Method: GET");
        if (is_vars) { int idx = 0; A2Str *k; V2 v; for (;;) { a2_dyn_enum(data.u.o, &idx, &k, &v); if (!idx) break; LAPP("\n  Param: "); LAPP(k->s); LAPP("="); LAPP(v2_tostr(vm, v)->s); } }
    }
#undef LAPP
    if (g_fetch_log) { if (log) avm1_trace_out(log); }
    else if (!strncmp(url, "http:", 5) || !strncmp(url, "https:", 6) || !strncmp(url, "mailto:", 7) || !strncmp(url, "ftp:", 4)) plat_open_url(url);
    free(log); free(url);
    return v2_undef();
}
static V2 sendToURL(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_undef(); }
extern void (*g_fscommand_hook)(Player *p, const char *cmd, const char *args);
static V2 fscommand(VM2 *vm, V2 self, int argc, V2 *argv) { A2Str *c = STR(0); (void)self; fprintf(stderr, "[avm2] fscommand(%s, %s)\n", c->s, STR(1)->s); if (!strcmp(c->s, "quit")) vm->player->stopped = 1; if (g_fscommand_hook) g_fscommand_hook(vm->player, c->s, STR(1)->s); return v2_undef(); }

/* ---------------------------------------------------------------- */
/* system / ui / external / filters                                 */

static V2 cap_version(VM2 *vm, V2 self, int argc, V2 *argv) { extern const char *g_flash_version; UNUSED; return v2_cstr(vm, g_flash_version); }
static V2 cap_os(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_cstr(vm, "Windows NT"); }
static V2 cap_playerType(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_cstr(vm, "StandAlone"); }
static V2 cap_language(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_cstr(vm, "en"); }
static V2 cap_manufacturer(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_cstr(vm, "Adobe Windows"); }
int a2_screen_w = 1536, a2_screen_h = 864;   /* the platform layer stores the real desktop size here */
static V2 cap_screenResX(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_num(a2_screen_w); }
static V2 cap_screenResY(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_num(a2_screen_h); }
static V2 cap_screenDPI(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_num(72); }
static V2 cap_pixelAspectRatio(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_num(1); }
static V2 cap_serverString(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_cstr(vm, "OS=Windows%20NT&PT=StandAlone&V=WIN%2010%2C0%2C0%2C0"); }
static V2 cap_isDebugger(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_bool(0); }
static V2 cap_cpuArch(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_cstr(vm, "x86"); }
static V2 cap_maxLevelIDC(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_cstr(vm, "5.1"); }
/* memory figures as Ruffle reports them (no real accounting): 90 MB in use, 10 MB free, 100 MB private */
static V2 sys_totalMemory(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_uint(1024u * 1024u * 90u); }
static V2 sys_totalMemoryNumber(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_num(1024.0 * 1024.0 * 90.0); }
static V2 sys_freeMemory(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_num(1024.0 * 1024.0 * 10.0); }
static V2 sys_privateMemory(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_num(1024.0 * 1024.0 * 100.0); }
static V2 sys_gc(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; a2_gc(vm); return v2_undef(); }
void (*a2_set_clipboard_hook)(const char *utf8);   /* set by the platform layer */
static V2 sys_setClipboard(VM2 *vm, V2 self, int argc, V2 *argv) {
    (void)self;
    if (argc < 1 || argv[0].t == T_NULL || argv[0].t == T_UNDEF) { a2_throw_error(vm, "TypeError", "Parameter string must be non-null.", 2007); return v2_undef(); }
    a2_dyn_set(vm, vm->global, a2_str(vm, "__clipboard"), v2_str(STR(0)));
    if (a2_set_clipboard_hook) a2_set_clipboard_hook(STR(0)->s);
    return v2_undef();
}
static V2 sys_exit(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; a2_throw_error(vm, "SecurityError", "Only trusted local files may cause the Flash Player to exit.", 2017); return v2_undef(); }   /* as the release player does */
static V2 sys_ime(VM2 *vm, V2 self, int argc, V2 *argv) { V2 i = dget(vm, v2_obj(vm->global), "__ime"); (void)self; (void)argc; (void)argv; if (i.t == T_OBJ) return i; i = a2_construct_class(vm, a2_class_find_s(vm, "flash.system::IME"), 0, NULL); a2_dyn_set(vm, vm->global, a2_str(vm, "__ime"), i); return i; }
static V2 sd_currentDomain(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Prop *p = a2_dyn_find(vm->global, a2_str(vm, "__secDomain")); UNUSED;
    if (p) return p->v;
    { A2Class *c = a2_class_find_s(vm, "flash.system::SecurityDomain"); V2 d = v2_obj(a2_new(vm, c)); a2_dyn_set_hidden(vm, vm->global, a2_str(vm, "__secDomain"), d); return d; }
}
static V2 sd_domainID(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_cstr(vm, "local"); }
/* the sandbox of the calling code's SWF: loaded over HTTP it is remote */
static V2 sec_sandboxType(VM2 *vm, V2 self, int argc, V2 *argv) {
    SWFMovie *m = a2_code_movie(vm); const char *u; UNUSED;
    if (!m) m = vm->player->movie;
    u = movie_url(m);
    if (u && (!strncmp(u, "http://", 7) || !strncmp(u, "https://", 8))) return v2_cstr(vm, "remote");
    return v2_cstr(vm, (vm->player->movie->file_attr & 1) ? "localWithNetwork" : "localWithFile");   /* local content shares the main movie's sandbox */
}
static V2 mouse_hide(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; a2_dyn_set(vm, vm->global, a2_str(vm, "__mouseHidden"), v2_bool(1)); return v2_undef(); }
static V2 mouse_show(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; a2_dyn_set(vm, vm->global, a2_str(vm, "__mouseHidden"), v2_bool(0)); return v2_undef(); }
static V2 mouse_cursor_get(VM2 *vm, V2 self, int argc, V2 *argv) { V2 c = dget(vm, v2_obj(vm->global), "__cursor"); UNUSED; return c.t == T_STR ? c : v2_cstr(vm, "auto"); }
/* registered cursors live in a plain object; the built-in names are lower case only */
static A2Obj *mouse_cursors(VM2 *vm, int make) {
    V2 c = dget(vm, v2_obj(vm->global), "__cursors");
    if (c.t == T_OBJ) return c.u.o;
    if (!make) return NULL;
    c = v2_obj(a2_new_plain(vm)); a2_dyn_set(vm, vm->global, a2_str(vm, "__cursors"), c);
    return c.u.o;
}
static V2 mouse_cursor_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 v = ARG(0); A2Str *s; A2Obj *reg;
    (void)self;
    if (v.t == T_NULL || v.t == T_UNDEF) { a2_throw_error(vm, "TypeError", "Parameter cursor must be non-null.", 2007); return v2_undef(); }
    s = v2_tostr(vm, v);
    reg = mouse_cursors(vm, 0);
    if (strcmp(s->s, "auto") && strcmp(s->s, "arrow") && strcmp(s->s, "button") && strcmp(s->s, "ibeam") && strcmp(s->s, "hand") && !(reg && dget(vm, v2_obj(reg), s->s).t != T_UNDEF)) {
        a2_throw_error(vm, "ArgumentError", "Parameter cursor must be one of the accepted values.", 2008); return v2_undef();
    }
    a2_dyn_set(vm, vm->global, a2_str(vm, "__cursor"), v2_str(s)); return v2_undef();
}
static V2 mouse_registerCursor(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 n = ARG(0);
    (void)self;
    if (n.t == T_NULL || n.t == T_UNDEF) { a2_throw_error(vm, "TypeError", "Parameter name must be non-null.", 2007); return v2_undef(); }
    a2_dyn_set(vm, mouse_cursors(vm, 1), v2_tostr(vm, n), argc > 1 ? argv[1] : v2_null());
    return v2_undef();
}
static V2 mouse_unregisterCursor(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 n = ARG(0), cur; A2Obj *reg; A2Str *s;
    (void)self;
    if (n.t == T_NULL || n.t == T_UNDEF) { a2_throw_error(vm, "TypeError", "Parameter name must be non-null.", 2007); return v2_undef(); }
    s = v2_tostr(vm, n); reg = mouse_cursors(vm, 0);
    if (reg) a2_dyn_del(reg, s);
    cur = dget(vm, v2_obj(vm->global), "__cursor");
    if (cur.t == T_STR && !strcmp(cur.u.s->s, s->s)) a2_dyn_set(vm, vm->global, a2_str(vm, "__cursor"), v2_cstr(vm, "auto"));   /* the current cursor goes back to auto */
    return v2_undef();
}
static V2 kb_isAccessible(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_bool(1); }
static V2 kb_capsLock(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_bool(0); }
static V2 cm_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { dset(vm, self, "customItems", v2_obj(a2_new_array(vm))); dset(vm, self, "builtInItems", a2_construct_class(vm, a2_class_find_s(vm, "flash.ui::ContextMenuBuiltInItems"), 0, NULL)); UNUSED; return v2_undef(); }
static V2 cmbi_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { const char *k[] = { "save", "zoom", "quality", "play", "loop", "rewind", "forwardAndBack", "print", NULL }; int i; UNUSED; for (i = 0; k[i]; i++) dset(vm, self, k[i], v2_bool(1)); return v2_undef(); }
static V2 cmi_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { dset(vm, self, "caption", argc ? v2_str(STR(0)) : v2_str(vm->s_empty)); dset(vm, self, "separatorBefore", v2_bool(argc > 1 ? BOOL(1) : 0)); dset(vm, self, "enabled", v2_bool(argc > 2 ? BOOL(2) : 1)); dset(vm, self, "visible", v2_bool(argc > 3 ? BOOL(3) : 1)); return v2_undef(); }
static V2 clip_generalClipboard(VM2 *vm, V2 self, int argc, V2 *argv) { V2 c = dget(vm, v2_obj(vm->global), "__gclip"); (void)self; (void)argc; (void)argv; if (c.t == T_OBJ) return c; c = a2_construct_class(vm, a2_class_find_s(vm, "flash.desktop::Clipboard"), 0, NULL); a2_dyn_set(vm, vm->global, a2_str(vm, "__gclip"), c); return c; }
static V2 clip_setData(VM2 *vm, V2 self, int argc, V2 *argv) { dset(vm, self, STR(0)->s, ARG(1)); return v2_bool(1); }
static V2 clip_getData(VM2 *vm, V2 self, int argc, V2 *argv) { V2 v = dget(vm, self, STR(0)->s); return v.t == T_UNDEF ? v2_null() : v; }
static V2 clip_hasFormat(VM2 *vm, V2 self, int argc, V2 *argv) { return v2_bool(dget(vm, self, STR(0)->s).t != T_UNDEF); }
static V2 proxy_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; if (self.t == T_OBJ) self.u.o->kind = OK_PROXY; return v2_undef(); }
/* default implementations throw through Error.throwError so the stack trace matches Flash */
static V2 proxy_throw(VM2 *vm, int id) { V2 args[2]; args[0] = v2_obj(vm->c_error->obj); args[1] = v2_int(id); a2_call_s(vm, v2_obj(vm->c_error->obj), "throwError", 2, args); return v2_undef(); }
static V2 px_getProperty(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return proxy_throw(vm, 2088); }
static V2 px_setProperty(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return proxy_throw(vm, 2089); }
static V2 px_callProperty(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return proxy_throw(vm, 2090); }
static V2 px_hasProperty(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return proxy_throw(vm, 2091); }
static V2 px_deleteProperty(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return proxy_throw(vm, 2092); }
static V2 px_getDescendants(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return proxy_throw(vm, 2093); }
static V2 px_nextNameIndex(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return proxy_throw(vm, 2105); }
static V2 px_nextName(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return proxy_throw(vm, 2106); }
static V2 px_nextValue(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return proxy_throw(vm, 2107); }
static V2 px_isAttribute(VM2 *vm, V2 self, int argc, V2 *argv) { V2 n = ARG(0); (void)self; if (n.t == T_OBJ && n.u.o->kind == OK_QNAME) return v2_bool(a2_qname_attr(n.u.o)); return v2_bool(n.t == T_STR && n.u.s->len && n.u.s->s[0] == '@'); }
static A2Ns g_flash_proxy_ns;
static void px_name(VM2 *vm, A2Name *out, const char *method) { out->name = a2_str(vm, method); out->nss = &g_flash_proxy_ns; out->nns = 1; out->any_ns = 0; out->attr = 0; }
/* the name handed to the proxy methods: a QName (uri "" for unqualified, null for any-namespace) */
static V2 px_qname(VM2 *vm, const A2Name *n) {
    A2Str *uri = vm->s_empty; A2Str *local = n->name; int attr = n->attr;
    if (local->len && local->s[0] == '@') { attr = 1; local = a2_intern(vm, local->s + 1, local->len - 1); }
    if (local->len == 0) local = a2_str(vm, "*");
    if (n->any_ns || (local->len == 1 && local->s[0] == '*')) uri = NULL;
    else if (n->nns == 1 && (n->nss[0].kind == NS_NAMESPACE || n->nss[0].kind == NS_EXPLICIT)) uri = n->nss[0].uri;
    else if (n->nns == 1 && n->nss[0].kind == NS_PACKAGE) uri = n->nss[0].uri;
    return a2_make_qname(vm, uri, local, attr);
}
int a2_proxy_get(VM2 *vm, A2Obj *o, const A2Name *n, V2 *out) { A2Name m; V2 a[1]; px_name(vm, &m, "getProperty"); a[0] = px_qname(vm, n); *out = a2_call_prop(vm, v2_obj(o), &m, 1, a); return 1; }
int a2_proxy_set(VM2 *vm, A2Obj *o, const A2Name *n, V2 v) { A2Name m; V2 a[2]; px_name(vm, &m, "setProperty"); a[0] = px_qname(vm, n); a[1] = v; a2_call_prop(vm, v2_obj(o), &m, 2, a); return 1; }
int a2_proxy_delete(VM2 *vm, A2Obj *o, const A2Name *n) { A2Name m; V2 a[1]; px_name(vm, &m, "deleteProperty"); a[0] = px_qname(vm, n); return v2_tobool(vm, a2_call_prop(vm, v2_obj(o), &m, 1, a)); }
int a2_proxy_has(VM2 *vm, A2Obj *o, const A2Name *n) { A2Name m; V2 a[1]; px_name(vm, &m, "hasProperty"); a[0] = v2_str(n->name); return v2_tobool(vm, a2_call_prop(vm, v2_obj(o), &m, 1, a)); }   /* `in` hands the proxy a String */
static V2 px_hasOwnProperty(VM2 *vm, V2 self, int argc, V2 *argv) { A2Name m; V2 a[1]; px_name(vm, &m, "hasProperty"); a[0] = v2_str(STR(0)); return v2_bool(v2_tobool(vm, a2_call_prop(vm, self, &m, 1, a))); }
V2 a2_proxy_call(VM2 *vm, A2Obj *o, const A2Name *n, int argc, V2 *argv) { A2Name m; V2 *a = (V2*)malloc((size_t)(argc + 1) * sizeof(V2)); V2 r; int i; px_name(vm, &m, "callProperty"); a[0] = px_qname(vm, n); for (i = 0; i < argc; i++) a[i + 1] = argv[i]; r = a2_call_prop(vm, v2_obj(o), &m, argc + 1, a); free(a); return r; }
V2 a2_proxy_descendants(VM2 *vm, A2Obj *o, const A2Name *n) { A2Name m; V2 a[1]; px_name(vm, &m, "getDescendants"); a[0] = px_qname(vm, n); return a2_call_prop(vm, v2_obj(o), &m, 1, a); }
int a2_proxy_next_index(VM2 *vm, A2Obj *o, int idx) { A2Name m; V2 a[1]; px_name(vm, &m, "nextNameIndex"); a[0] = v2_int(idx); return v2_toint(vm, a2_call_prop(vm, v2_obj(o), &m, 1, a)); }
V2 a2_proxy_next_name(VM2 *vm, A2Obj *o, int idx) { A2Name m; V2 a[1]; px_name(vm, &m, "nextName"); a[0] = v2_int(idx); return a2_call_prop(vm, v2_obj(o), &m, 1, a); }
V2 a2_proxy_next_value(VM2 *vm, A2Obj *o, int idx) { A2Name m; V2 a[1]; px_name(vm, &m, "nextValue"); a[0] = v2_int(idx); return a2_call_prop(vm, v2_obj(o), &m, 1, a); }
static V2 printjob_start(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_bool(0); }
static V2 worker_current(VM2 *vm, V2 self, int argc, V2 *argv) { V2 w = dget(vm, v2_obj(vm->global), "__worker"); (void)self; (void)argc; (void)argv; if (w.t == T_OBJ) return w; w = a2_construct_class(vm, a2_class_find_s(vm, "flash.system::Worker"), 0, NULL); a2_dyn_set(vm, vm->global, a2_str(vm, "__worker"), w); return w; }
static V2 worker_isPrimordial(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_bool(1); }
static V2 mt_supportsTouchEvents(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_bool(0); }
static V2 mt_inputMode(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_cstr(vm, "none"); }
static V2 acc_active(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_bool(0); }
static V2 fsc_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_undef(); }

void a2_flash_misc_init(VM2 *vm) {
    A2Class *c, *ed = vm->c_eventdispatcher;
    /* geom */
    vm->c_point = c = a2_def_class(vm, "flash.geom", "Point", vm->c_object, OK_POINT, Point_ctor);
    a2_add_getter(vm, c, "x", g_x_get, g_x_set); a2_add_getter(vm, c, "y", g_y_get, g_y_set); a2_add_getter(vm, c, "length", pt_length, NULL);
    a2_add_method(vm, c, "add", pt_add, 1); a2_add_method(vm, c, "subtract", pt_subtract, 1); a2_add_method(vm, c, "clone", pt_clone, 0); a2_add_method(vm, c, "equals", pt_equals, 1); a2_add_method(vm, c, "normalize", pt_normalize, 1); a2_add_method(vm, c, "offset", pt_offset, 2); a2_add_method(vm, c, "setTo", pt_setTo, 2); a2_add_method(vm, c, "copyFrom", pt_copyFrom, 1); a2_add_method(vm, c, "toString", pt_toString, 0);
    a2_add_static(vm, c, "distance", pt_distance, 2); a2_add_static(vm, c, "interpolate", pt_interpolate, 3); a2_add_static(vm, c, "polar", pt_polar, 2);
    a2_dyn_set_hidden(vm, c->prototype, vm->s_toString, v2_obj(a2_new_native_fn(vm, pt_toString, "toString", 0)));
    vm->c_rectangle = c = a2_def_class(vm, "flash.geom", "Rectangle", vm->c_object, OK_RECT, Rect_ctor);
    a2_add_getter(vm, c, "x", g_x_get, g_x_set); a2_add_getter(vm, c, "y", g_y_get, g_y_set); a2_add_getter(vm, c, "width", g_width_get, g_width_set); a2_add_getter(vm, c, "height", g_height_get, g_height_set);
    a2_add_getter(vm, c, "left", rc_left_get, rc_left_set); a2_add_getter(vm, c, "top", rc_top_get, rc_top_set); a2_add_getter(vm, c, "right", rc_right_get, rc_right_set); a2_add_getter(vm, c, "bottom", rc_bottom_get, rc_bottom_set);
    a2_add_getter(vm, c, "size", rc_size_get, rc_size_set); a2_add_getter(vm, c, "topLeft", rc_topLeft_get, rc_topLeft_set); a2_add_getter(vm, c, "bottomRight", rc_bottomRight_get, rc_bottomRight_set);
    a2_add_method(vm, c, "clone", rc_clone, 0); a2_add_method(vm, c, "contains", rc_contains, 2); a2_add_method(vm, c, "containsPoint", rc_containsPoint, 1); a2_add_method(vm, c, "containsRect", rc_containsRect, 1); a2_add_method(vm, c, "equals", rc_equals, 1);
    a2_add_method(vm, c, "inflate", rc_inflate, 2); a2_add_method(vm, c, "inflatePoint", rc_inflatePoint, 1); a2_add_method(vm, c, "offset", rc_offset, 2); a2_add_method(vm, c, "offsetPoint", rc_offsetPoint, 1); a2_add_method(vm, c, "isEmpty", rc_isEmpty, 0); a2_add_method(vm, c, "setEmpty", rc_setEmpty, 0);
    a2_add_method(vm, c, "setTo", rc_setTo, 4); a2_add_method(vm, c, "copyFrom", rc_copyFrom, 1); a2_add_method(vm, c, "intersection", rc_intersection, 1); a2_add_method(vm, c, "intersects", rc_intersects, 1); a2_add_method(vm, c, "union", rc_union, 1); a2_add_method(vm, c, "toString", rc_toString, 0);
    a2_dyn_set_hidden(vm, c->prototype, vm->s_toString, v2_obj(a2_new_native_fn(vm, rc_toString, "toString", 0)));
    vm->c_matrix = c = a2_def_class(vm, "flash.geom", "Matrix", vm->c_object, OK_MATRIX, Matrix_ctor);
    a2_add_getter(vm, c, "a", g_a_get, g_a_set); a2_add_getter(vm, c, "b", g_b_get, g_b_set); a2_add_getter(vm, c, "c", g_c_get, g_c_set); a2_add_getter(vm, c, "d", g_d_get, g_d_set); a2_add_getter(vm, c, "tx", g_tx_get, g_tx_set); a2_add_getter(vm, c, "ty", g_ty_get, g_ty_set);
    a2_add_method(vm, c, "identity", mx_identity, 0); a2_add_method(vm, c, "clone", mx_clone, 0); a2_add_method(vm, c, "concat", mx_concat, 1); a2_add_method(vm, c, "translate", mx_translate, 2); a2_add_method(vm, c, "scale", mx_scale, 2); a2_add_method(vm, c, "rotate", mx_rotate, 1); a2_add_method(vm, c, "invert", mx_invert, 0);
    a2_add_method(vm, c, "transformPoint", mx_transformPoint, 1); a2_add_method(vm, c, "deltaTransformPoint", mx_deltaTransformPoint, 1); a2_add_method(vm, c, "createBox", mx_createBox, 5); a2_add_method(vm, c, "createGradientBox", mx_createGradientBox, 5); a2_add_method(vm, c, "setTo", mx_setTo, 6); a2_add_method(vm, c, "copyFrom", mx_copyFrom, 1); a2_add_method(vm, c, "toString", mx_toString, 0); a2_add_method(vm, c, "copyRowTo", mx_copyRowTo, 2); a2_add_method(vm, c, "copyColumnTo", mx_copyColumnTo, 2); a2_add_method(vm, c, "copyRowFrom", mx_copyRowFrom, 2); a2_add_method(vm, c, "copyColumnFrom", mx_copyRowFrom, 2);
    a2_add_method(vm, c, "copyColumnsTo", r_noop, 2); a2_add_method(vm, c, "copyRowsTo", r_noop, 2); a2_add_method(vm, c, "copyColumnsFrom", r_noop, 2); a2_add_method(vm, c, "copyRowsFrom", r_noop, 2);
    a2_dyn_set_hidden(vm, c->prototype, vm->s_toString, v2_obj(a2_new_native_fn(vm, mx_toString, "toString", 0)));
    vm->c_colortransform = c = a2_def_class(vm, "flash.geom", "ColorTransform", vm->c_object, OK_COLORTRANSFORM, CT_ctor);
    a2_add_getter(vm, c, "redMultiplier", g_redMultiplier_get, g_redMultiplier_set); a2_add_getter(vm, c, "greenMultiplier", g_greenMultiplier_get, g_greenMultiplier_set); a2_add_getter(vm, c, "blueMultiplier", g_blueMultiplier_get, g_blueMultiplier_set); a2_add_getter(vm, c, "alphaMultiplier", g_alphaMultiplier_get, g_alphaMultiplier_set);
    a2_add_getter(vm, c, "redOffset", g_redOffset_get, g_redOffset_set); a2_add_getter(vm, c, "greenOffset", g_greenOffset_get, g_greenOffset_set); a2_add_getter(vm, c, "blueOffset", g_blueOffset_get, g_blueOffset_set); a2_add_getter(vm, c, "alphaOffset", g_alphaOffset_get, g_alphaOffset_set); a2_add_getter(vm, c, "color", ct_color_get, ct_color_set);
    a2_add_method(vm, c, "concat", ct_concat, 1); a2_add_method(vm, c, "toString", ct_toString, 0);
    a2_dyn_set_hidden(vm, c->prototype, vm->s_toString, v2_obj(a2_new_native_fn(vm, ct_toString, "toString", 0)));
    c = a2_def_class(vm, "flash.geom", "Transform", vm->c_object, OK_PLAIN, Transform_ctor); c->is_dynamic = 1;
    a2_add_getter(vm, c, "matrix", tr_matrix_get, tr_matrix_set); a2_add_getter(vm, c, "concatenatedMatrix", tr_concatenatedMatrix, NULL); a2_add_getter(vm, c, "colorTransform", tr_colorTransform_get, tr_colorTransform_set); a2_add_getter(vm, c, "concatenatedColorTransform", tr_colorTransform_get, NULL); a2_add_getter(vm, c, "pixelBounds", tr_pixelBounds, NULL);
    a2_add_getter(vm, c, "matrix3D", tr_matrix3D_get, tr_matrix3D_set); a2_add_method(vm, c, "getRelativeMatrix3D", tr_getRelativeMatrix3D, 1); a2_add_getter(vm, c, "perspectiveProjection", tr_pp_get, tr_pp_set);
    { extern void a2_geom3d_init(VM2 *vm); a2_geom3d_init(vm); }

    /* media */
    { extern void a2_sound_init(VM2 *vm); a2_sound_init(vm); }   /* avm2_sound.c */
    { extern void a2_video_class(VM2 *vm, A2Class *dobj); a2_video_class(vm, vm->c_displayobject); }
    c = a2_def_class(vm, "flash.media", "Camera", ed, OK_PLAIN, NULL); a2_add_static(vm, c, "getCamera", r_null, 1); a2_static_getter(vm, c, "names", r_null, NULL); a2_static_getter(vm, c, "isSupported", r_false, NULL);
    c = a2_def_class(vm, "flash.media", "Microphone", ed, OK_PLAIN, NULL); a2_add_static(vm, c, "getMicrophone", r_null, 1); a2_static_getter(vm, c, "names", r_null, NULL); a2_static_getter(vm, c, "isSupported", r_false, NULL);
    { const char *m[] = { "StageVideo", "StageVideoAvailability", "VideoStatus", "SoundCodec", "AudioPlaybackMode", "H264Level", "H264Profile", "H264VideoStreamSettings", "VideoStreamSettings", "AVNetworkingParams", "MicrophoneEnhancedMode", "MicrophoneEnhancedOptions", "CameraPosition", NULL }; int i; for (i = 0; m[i]; i++) { c = a2_def_class(vm, "flash.media", m[i], vm->c_object, OK_PLAIN, r_dyn_ctor); c->is_dynamic = 1; } }

    /* BitmapData */
    { extern void a2_bitmapdata_init(VM2 *vm); a2_bitmapdata_init(vm); }

    /* net */
    vm->c_loaderinfo = c = a2_def_class(vm, "flash.display", "LoaderInfo", ed, OK_PLAIN, LI_ctor); c->is_dynamic = 1;
    a2_add_getter(vm, c, "bytesLoaded", li_bytesLoaded, NULL); a2_add_getter(vm, c, "bytesTotal", li_bytesTotal, NULL); a2_add_getter(vm, c, "url", li_url, NULL); a2_add_getter(vm, c, "loaderURL", li_loaderURL, NULL); a2_add_getter(vm, c, "parameters", li_parameters, NULL);
    a2_add_getter(vm, c, "width", li_width, NULL); a2_add_getter(vm, c, "height", li_height, NULL); a2_add_getter(vm, c, "frameRate", li_frameRate, NULL); a2_add_getter(vm, c, "swfVersion", li_swfVersion, NULL); a2_add_getter(vm, c, "actionScriptVersion", li_asVersion, NULL);
    a2_add_getter(vm, c, "content", li_content, NULL); a2_add_getter(vm, c, "loader", li_loader, NULL); a2_add_getter(vm, c, "applicationDomain", li_appDomain, NULL); a2_add_getter(vm, c, "bytes", li_bytes, NULL); a2_add_getter(vm, c, "childAllowsParent", li_allows, NULL); a2_add_getter(vm, c, "parentAllowsChild", li_allows, NULL); a2_add_getter(vm, c, "sameDomain", li_allows, NULL);
    a2_add_getter(vm, c, "contentType", li_contentType, NULL); a2_add_getter(vm, c, "uncaughtErrorEvents", li_uncaughtErrorEvents, NULL); a2_add_getter(vm, c, "sharedEvents", li_sharedEvents, NULL); a2_add_getter(vm, c, "isURLInaccessible", li_isURLInaccessible, NULL); a2_add_getter(vm, c, "childSandboxBridge", r_null, r_noop); a2_add_getter(vm, c, "parentSandboxBridge", r_null, r_noop);
    a2_add_static(vm, c, "getLoaderInfoByDefinition", r_null, 1);
    vm->c_loader = c = a2_def_class(vm, "flash.display", "Loader", vm->c_container, OK_DISPLAY, Loader_ctor);
    a2_add_getter(vm, c, "contentLoaderInfo", ld_contentLoaderInfo, NULL); a2_add_getter(vm, c, "content", ld_content, NULL); a2_add_getter(vm, c, "uncaughtErrorEvents", li_uncaughtErrorEvents, NULL);
    a2_add_method(vm, c, "load", ld_load, 2); a2_add_method(vm, c, "loadBytes", ld_loadBytes, 2); a2_add_method(vm, c, "unload", ld_unload, 0); a2_add_method(vm, c, "unloadAndStop", ld_unload, 1); a2_add_method(vm, c, "close", ld_close, 0);
    { const char *un[] = { "addChild", "addChildAt", "removeChild", "removeChildAt", "setChildIndex", "removeChildren", "swapChildren", "swapChildrenAt", NULL }; int i; for (i = 0; un[i]; i++) a2_add_method(vm, c, un[i], ld_unsupported, 1); }
    c = a2_def_class(vm, "flash.system", "LoaderContext", vm->c_object, OK_PLAIN, LoaderContext_ctor); c->is_dynamic = 1;
    c = a2_def_class(vm, "flash.system", "JPEGLoaderContext", c, OK_PLAIN, JPEGLoaderContext_ctor); c->is_dynamic = 1;
    c = a2_def_class(vm, "flash.system", "SecurityDomain", vm->c_object, OK_PLAIN, NULL); a2_static_getter(vm, c, "currentDomain", sd_currentDomain, NULL); a2_add_getter(vm, c, "domainID", sd_domainID, NULL);
    c = a2_def_class(vm, "flash.net", "URLLoader", ed, OK_PLAIN, URLLoader_ctor); c->is_dynamic = 1; a2_add_method(vm, c, "load", ul_load, 1); a2_add_method(vm, c, "close", r_noop, 0);
    c = a2_def_class(vm, "flash.net", "URLStream", ed, OK_PLAIN, r_dyn_ctor); c->is_dynamic = 1; a2_add_method(vm, c, "load", us_load, 1); a2_add_method(vm, c, "close", r_noop, 0); a2_add_getter(vm, c, "bytesAvailable", us_bytesAvailable, NULL); a2_add_getter(vm, c, "connected", r_false, NULL);
    a2_add_getter(vm, c, "endian", us_endian_get, us_endian_set);
    { struct { const char *n; NativeFn2 f; int a; } um[] = { { "readByte", us_readByte, 0 }, { "readUnsignedByte", us_readUnsignedByte, 0 }, { "readShort", us_readShort, 0 }, { "readUnsignedShort", us_readUnsignedShort, 0 }, { "readInt", us_readInt, 0 }, { "readUnsignedInt", us_readUnsignedInt, 0 }, { "readFloat", us_readFloat, 0 }, { "readDouble", us_readDouble, 0 }, { "readBoolean", us_readBoolean, 0 }, { "readUTF", us_readUTF, 0 }, { "readUTFBytes", us_readUTFBytes, 1 }, { "readMultiByte", us_readMultiByte, 2 }, { "readBytes", us_readBytes, 3 }, { "readObject", us_readObject, 0 }, { NULL, NULL, 0 } }; int i; for (i = 0; um[i].n; i++) a2_add_method(vm, c, um[i].n, um[i].f, um[i].a); }
    c = a2_def_class(vm, "flash.net", "URLRequestMethod", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "GET", "GET"); a2_const_str(vm, c, "POST", "POST"); a2_const_str(vm, c, "PUT", "PUT"); a2_const_str(vm, c, "DELETE", "DELETE"); a2_const_str(vm, c, "HEAD", "HEAD"); a2_const_str(vm, c, "OPTIONS", "OPTIONS");
    c = a2_def_class(vm, "flash.net", "URLLoaderDataFormat", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "TEXT", "text"); a2_const_str(vm, c, "BINARY", "binary"); a2_const_str(vm, c, "VARIABLES", "variables");
    c = a2_def_class(vm, "flash.net", "URLRequestHeader", vm->c_object, OK_PLAIN, r_dyn_ctor); c->is_dynamic = 1;
    vm->c_sharedobject = c = a2_def_class(vm, "flash.net", "SharedObject", ed, OK_SHAREDOBJ, NULL); c->is_dynamic = 1;
    a2_add_static(vm, c, "getLocal", SharedObject_getLocal, 3); a2_add_static(vm, c, "getRemote", SharedObject_getLocal, 4); a2_add_getter(vm, c, "data", so_data, NULL); a2_add_method(vm, c, "flush", so_flush, 1); a2_add_method(vm, c, "clear", so_clear, 0); a2_add_getter(vm, c, "size", so_size, NULL); a2_add_method(vm, c, "setProperty", so_setProperty, 2); a2_add_method(vm, c, "setDirty", r_noop, 1); a2_add_method(vm, c, "close", r_noop, 0); a2_add_method(vm, c, "connect", r_noop, 2); a2_add_method(vm, c, "send", r_noop, 1); a2_add_getter(vm, c, "objectEncoding", so_enc_get, so_enc_set);
    { A2Trait *t; extern void a2_tm_put(A2TraitMap *tm, A2Trait *t); t = (A2Trait*)calloc(1, sizeof(A2Trait)); t->name = a2_str(vm, "defaultObjectEncoding"); t->ns = vm->ns_public; t->kind = TK_GETTER; t->getter = a2_native_method(vm, so_defenc_get, "defaultObjectEncoding", 0); t->setter = a2_native_method(vm, so_defenc_set, "defaultObjectEncoding", 1); t->getter->owner = t->setter->owner = c; a2_tm_put(&c->ctraits, t); }
    a2_add_getter(vm, c, "objectEncoding", r_zero, r_noop); a2_add_getter(vm, c, "client", r_self, r_noop); a2_add_getter(vm, c, "fps", r_zero, r_noop); a2_static_getter(vm, c, "defaultObjectEncoding", r_zero, r_noop); a2_static_getter(vm, c, "preventBackup", r_false, r_noop);
    c = a2_def_class(vm, "flash.net", "SharedObjectFlushStatus", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "FLUSHED", "flushed"); a2_const_str(vm, c, "PENDING", "pending");
    /* flash.net.NetStream: ActionScript (as3/) with natives in avm2_netstream.c */
    c = a2_def_class(vm, "flash.net", "FileFilter", vm->c_object, OK_PLAIN, r_dyn_ctor); c->is_dynamic = 1;
    c = a2_def_class(vm, "flash.net", "NetStreamInfo", vm->c_object, OK_PLAIN, r_dyn_ctor); c->is_dynamic = 1;
    c = a2_def_class(vm, "flash.net", "NetStreamPlayOptions", ed, OK_PLAIN, r_dyn_ctor); c->is_dynamic = 1;
    c = a2_def_class(vm, "flash.net", "NetGroup", ed, OK_PLAIN, r_dyn_ctor); c->is_dynamic = 1;
    c = a2_def_class(vm, "flash.net", "GroupSpecifier", vm->c_object, OK_PLAIN, r_dyn_ctor); c->is_dynamic = 1;
    { A2Trait *t; extern void a2_tm_put(A2TraitMap *tm, A2Trait *t); A2Ns ns; ns.kind = NS_PACKAGE; ns.uri = a2_str(vm, "flash.net");
      t = (A2Trait*)calloc(1, sizeof(A2Trait)); t->name = a2_str(vm, "navigateToURL"); t->ns = ns; t->kind = TK_METHOD; t->method = a2_native_method(vm, navigateToURL, "navigateToURL", 2); a2_tm_put(&vm->global->cls->itraits, t);
      t = (A2Trait*)calloc(1, sizeof(A2Trait)); t->name = a2_str(vm, "sendToURL"); t->ns = ns; t->kind = TK_METHOD; t->method = a2_native_method(vm, sendToURL, "sendToURL", 1); a2_tm_put(&vm->global->cls->itraits, t);
      ns.uri = a2_str(vm, "flash.system");
      t = (A2Trait*)calloc(1, sizeof(A2Trait)); t->name = a2_str(vm, "fscommand"); t->ns = ns; t->kind = TK_METHOD; t->method = a2_native_method(vm, fscommand, "fscommand", 2); a2_tm_put(&vm->global->cls->itraits, t); }

    /* system */
    c = a2_def_class(vm, "flash.system", "Capabilities", vm->c_object, OK_PLAIN, NULL);
    a2_static_getter(vm, c, "version", cap_version, NULL); a2_static_getter(vm, c, "os", cap_os, NULL); a2_static_getter(vm, c, "playerType", cap_playerType, NULL); a2_static_getter(vm, c, "language", cap_language, NULL); a2_static_getter(vm, c, "manufacturer", cap_manufacturer, NULL);
    a2_static_getter(vm, c, "screenResolutionX", cap_screenResX, NULL); a2_static_getter(vm, c, "screenResolutionY", cap_screenResY, NULL); a2_static_getter(vm, c, "screenDPI", cap_screenDPI, NULL); a2_static_getter(vm, c, "pixelAspectRatio", cap_pixelAspectRatio, NULL); a2_static_getter(vm, c, "serverString", cap_serverString, NULL);
    a2_static_getter(vm, c, "isDebugger", cap_isDebugger, NULL); a2_static_getter(vm, c, "cpuArchitecture", cap_cpuArch, NULL); a2_static_getter(vm, c, "hasAudio", r_true, NULL); a2_static_getter(vm, c, "hasMP3", r_true, NULL); a2_static_getter(vm, c, "hasVideoEncoder", r_false, NULL); a2_static_getter(vm, c, "hasAccessibility", r_false, NULL);
    a2_static_getter(vm, c, "hasPrinting", r_false, NULL); a2_static_getter(vm, c, "hasScreenBroadcast", r_false, NULL); a2_static_getter(vm, c, "hasStreamingAudio", r_true, NULL); a2_static_getter(vm, c, "hasStreamingVideo", r_true, NULL); a2_static_getter(vm, c, "hasTLS", r_false, NULL); a2_static_getter(vm, c, "hasIME", r_false, NULL);
    a2_static_getter(vm, c, "avHardwareDisable", r_true, NULL); a2_static_getter(vm, c, "localFileReadDisable", r_false, NULL); a2_static_getter(vm, c, "isEmbeddedInAcrobat", r_false, NULL); a2_static_getter(vm, c, "touchscreenType", r_empty, NULL); a2_static_getter(vm, c, "supports32BitProcesses", r_true, NULL); a2_static_getter(vm, c, "supports64BitProcesses", r_false, NULL);
    a2_static_getter(vm, c, "maxLevelIDC", cap_maxLevelIDC, NULL); a2_static_getter(vm, c, "screenColor", cap_cpuArch, NULL); a2_static_getter(vm, c, "hasAudioEncoder", r_false, NULL); a2_static_getter(vm, c, "hasEmbeddedVideo", r_true, NULL); a2_static_getter(vm, c, "hasMultiChannelAudio", r_false, 0);
    a2_add_static(vm, c, "hasMultiChannelAudio", r_false, 1);
    c = a2_def_class(vm, "flash.system", "System", vm->c_object, OK_PLAIN, NULL);
    a2_static_getter(vm, c, "totalMemory", sys_totalMemory, NULL); a2_static_getter(vm, c, "totalMemoryNumber", sys_totalMemoryNumber, NULL); a2_static_getter(vm, c, "freeMemory", sys_freeMemory, NULL); a2_static_getter(vm, c, "privateMemory", sys_privateMemory, NULL); a2_static_getter(vm, c, "useCodePage", r_false, r_noop); a2_static_getter(vm, c, "ime", sys_ime, NULL); a2_static_getter(vm, c, "vmVersion", cap_version, NULL); a2_static_getter(vm, c, "processCPUUsage", r_zero, NULL);
    a2_add_static(vm, c, "gc", sys_gc, 0); a2_add_static(vm, c, "pauseForGCIfCollectionImminent", r_noop, 1); a2_add_static(vm, c, "setClipboard", sys_setClipboard, 1); a2_add_static(vm, c, "exit", sys_exit, 1); a2_add_static(vm, c, "resume", r_noop, 0); a2_add_static(vm, c, "pause", r_noop, 0); a2_add_static(vm, c, "disposeXML", r_noop, 1);
    c = a2_def_class(vm, "flash.system", "Security", vm->c_object, OK_PLAIN, NULL);
    a2_static_getter(vm, c, "sandboxType", sec_sandboxType, NULL); a2_static_getter(vm, c, "exactSettings", r_true, r_noop); a2_static_getter(vm, c, "disableAVM1Loading", r_false, r_noop); a2_static_getter(vm, c, "pageDomain", r_empty, NULL);
    a2_add_static(vm, c, "allowDomain", r_noop, 1); a2_add_static(vm, c, "allowInsecureDomain", r_noop, 1); a2_add_static(vm, c, "loadPolicyFile", r_noop, 1); a2_add_static(vm, c, "showSettings", r_noop, 1);
    a2_const_str(vm, c, "LOCAL_TRUSTED", "localTrusted"); a2_const_str(vm, c, "LOCAL_WITH_FILE", "localWithFile"); a2_const_str(vm, c, "LOCAL_WITH_NETWORK", "localWithNetwork"); a2_const_str(vm, c, "REMOTE", "remote"); a2_const_str(vm, c, "APPLICATION", "application");
    c = a2_def_class(vm, "flash.system", "SecurityPanel", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "DEFAULT", "default"); a2_const_str(vm, c, "PRIVACY", "privacy"); a2_const_str(vm, c, "LOCAL_STORAGE", "localStorage"); a2_const_str(vm, c, "MICROPHONE", "microphone"); a2_const_str(vm, c, "CAMERA", "camera"); a2_const_str(vm, c, "DISPLAY", "display"); a2_const_str(vm, c, "SETTINGS_MANAGER", "settingsManager");
    c = a2_def_class(vm, "flash.system", "IME", ed, OK_PLAIN, r_dyn_ctor); c->is_dynamic = 1; a2_static_getter(vm, c, "enabled", r_false, r_noop); a2_static_getter(vm, c, "conversionMode", r_empty, r_noop); a2_static_getter(vm, c, "isSupported", r_false, NULL); a2_add_static(vm, c, "setCompositionString", r_noop, 1); a2_add_static(vm, c, "doConversion", r_noop, 0); a2_add_static(vm, c, "compositionSelectionChanged", r_noop, 2); a2_add_static(vm, c, "compositionAbandoned", r_noop, 0);
    c = a2_def_class(vm, "flash.system", "IMEConversionMode", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "ALPHANUMERIC_FULL", "ALPHANUMERIC_FULL"); a2_const_str(vm, c, "ALPHANUMERIC_HALF", "ALPHANUMERIC_HALF"); a2_const_str(vm, c, "CHINESE", "CHINESE"); a2_const_str(vm, c, "JAPANESE_HIRAGANA", "JAPANESE_HIRAGANA"); a2_const_str(vm, c, "JAPANESE_KATAKANA_FULL", "JAPANESE_KATAKANA_FULL"); a2_const_str(vm, c, "JAPANESE_KATAKANA_HALF", "JAPANESE_KATAKANA_HALF"); a2_const_str(vm, c, "KOREAN", "KOREAN"); a2_const_str(vm, c, "UNKNOWN", "UNKNOWN");
    c = a2_def_class(vm, "flash.system", "Worker", ed, OK_PLAIN, r_dyn_ctor); c->is_dynamic = 1; a2_static_getter(vm, c, "current", worker_current, NULL); a2_static_getter(vm, c, "isSupported", r_false, NULL); a2_add_getter(vm, c, "isPrimordial", worker_isPrimordial, NULL); a2_add_getter(vm, c, "state", r_empty, NULL); a2_add_method(vm, c, "start", r_noop, 0); a2_add_method(vm, c, "terminate", r_false, 0); a2_add_method(vm, c, "getSharedProperty", r_null, 1); a2_add_method(vm, c, "setSharedProperty", r_noop, 2); a2_add_method(vm, c, "createMessageChannel", r_null, 1);
    c = a2_def_class(vm, "flash.system", "WorkerDomain", vm->c_object, OK_PLAIN, r_dyn_ctor); a2_static_getter(vm, c, "current", r_null, NULL); a2_static_getter(vm, c, "isSupported", r_false, NULL);
    { const char *sys[] = { "WorkerState", "MessageChannel", "MessageChannelState", "TouchscreenType", "ImageDecodingPolicy", "ApplicationInstaller", "AuthorizedFeatures", "AuthorizedFeaturesLoader", "DomainMemoryWithStageMemory", "SystemUpdater", "SystemUpdaterType", NULL }; int i; for (i = 0; sys[i]; i++) { c = a2_def_class(vm, "flash.system", sys[i], vm->c_object, OK_PLAIN, r_dyn_ctor); c->is_dynamic = 1; } }

    /* ui */
    c = a2_def_class(vm, "flash.ui", "Mouse", vm->c_object, OK_PLAIN, NULL); a2_add_static(vm, c, "hide", mouse_hide, 0); a2_add_static(vm, c, "show", mouse_show, 0); a2_static_getter(vm, c, "cursor", mouse_cursor_get, mouse_cursor_set); a2_static_getter(vm, c, "supportsCursor", r_true, NULL); a2_static_getter(vm, c, "supportsNativeCursor", r_false, NULL); a2_add_static(vm, c, "registerCursor", mouse_registerCursor, 2); a2_add_static(vm, c, "unregisterCursor", mouse_unregisterCursor, 1);
    c = a2_def_class(vm, "flash.ui", "MouseCursor", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "ARROW", "arrow"); a2_const_str(vm, c, "AUTO", "auto"); a2_const_str(vm, c, "BUTTON", "button"); a2_const_str(vm, c, "HAND", "hand"); a2_const_str(vm, c, "IBEAM", "ibeam");
    c = a2_def_class(vm, "flash.ui", "MouseCursorData", vm->c_object, OK_PLAIN, r_dyn_ctor); c->is_dynamic = 1;
    c = a2_def_class(vm, "flash.ui", "Keyboard", vm->c_object, OK_PLAIN, NULL); a2_add_static(vm, c, "isAccessible", kb_isAccessible, 0); a2_static_getter(vm, c, "capsLock", kb_capsLock, NULL); a2_static_getter(vm, c, "numLock", kb_capsLock, NULL); a2_static_getter(vm, c, "hasVirtualKeyboard", r_false, NULL); a2_static_getter(vm, c, "physicalKeyboardType", r_empty, NULL);
    { struct { const char *n; int v; } keys[] = { { "BACKSPACE", 8 }, { "TAB", 9 }, { "ENTER", 13 }, { "SHIFT", 16 }, { "CONTROL", 17 }, { "ALTERNATE", 18 }, { "CAPS_LOCK", 20 }, { "ESCAPE", 27 }, { "SPACE", 32 }, { "PAGE_UP", 33 }, { "PAGE_DOWN", 34 }, { "END", 35 }, { "HOME", 36 }, { "LEFT", 37 }, { "UP", 38 }, { "RIGHT", 39 }, { "DOWN", 40 }, { "INSERT", 45 }, { "DELETE", 46 },
        { "NUMBER_0", 48 }, { "NUMBER_1", 49 }, { "NUMBER_2", 50 }, { "NUMBER_3", 51 }, { "NUMBER_4", 52 }, { "NUMBER_5", 53 }, { "NUMBER_6", 54 }, { "NUMBER_7", 55 }, { "NUMBER_8", 56 }, { "NUMBER_9", 57 },
        { "A", 65 }, { "B", 66 }, { "C", 67 }, { "D", 68 }, { "E", 69 }, { "F", 70 }, { "G", 71 }, { "H", 72 }, { "I", 73 }, { "J", 74 }, { "K", 75 }, { "L", 76 }, { "M", 77 }, { "N", 78 }, { "O", 79 }, { "P", 80 }, { "Q", 81 }, { "R", 82 }, { "S", 83 }, { "T", 84 }, { "U", 85 }, { "V", 86 }, { "W", 87 }, { "X", 88 }, { "Y", 89 }, { "Z", 90 },
        { "NUMPAD_0", 96 }, { "NUMPAD_1", 97 }, { "NUMPAD_2", 98 }, { "NUMPAD_3", 99 }, { "NUMPAD_4", 100 }, { "NUMPAD_5", 101 }, { "NUMPAD_6", 102 }, { "NUMPAD_7", 103 }, { "NUMPAD_8", 104 }, { "NUMPAD_9", 105 }, { "NUMPAD_MULTIPLY", 106 }, { "NUMPAD_ADD", 107 }, { "NUMPAD_ENTER", 108 }, { "NUMPAD_SUBTRACT", 109 }, { "NUMPAD_DECIMAL", 110 }, { "NUMPAD_DIVIDE", 111 },
        { "F1", 112 }, { "F2", 113 }, { "F3", 114 }, { "F4", 115 }, { "F5", 116 }, { "F6", 117 }, { "F7", 118 }, { "F8", 119 }, { "F9", 120 }, { "F10", 121 }, { "F11", 122 }, { "F12", 123 }, { "F13", 124 }, { "F14", 125 }, { "F15", 126 },
        { "SEMICOLON", 186 }, { "EQUAL", 187 }, { "COMMA", 188 }, { "MINUS", 189 }, { "PERIOD", 190 }, { "SLASH", 191 }, { "BACKQUOTE", 192 }, { "LEFTBRACKET", 219 }, { "BACKSLASH", 220 }, { "RIGHTBRACKET", 221 }, { "QUOTE", 222 }, { "COMMAND", 15 }, { "NUMPAD", 21 }, { NULL, 0 } }; int i;
      for (i = 0; keys[i].n; i++) a2_add_const(vm, c, keys[i].n, v2_uint((u32)keys[i].v)); a2_const_str(vm, c, "STRING_BEGIN", ""); }
    c = a2_def_class(vm, "flash.ui", "KeyLocation", vm->c_object, OK_PLAIN, NULL); a2_add_const(vm, c, "STANDARD", v2_uint(0)); a2_add_const(vm, c, "LEFT", v2_uint(1)); a2_add_const(vm, c, "RIGHT", v2_uint(2)); a2_add_const(vm, c, "NUM_PAD", v2_uint(3));
    c = a2_def_class(vm, "flash.ui", "ContextMenu", ed, OK_PLAIN, cm_ctor); c->is_dynamic = 1; a2_add_method(vm, c, "hideBuiltInItems", r_noop, 0); a2_add_method(vm, c, "clone", r_self, 0); a2_static_getter(vm, c, "isSupported", r_true, NULL);
    c = a2_def_class(vm, "flash.ui", "ContextMenuBuiltInItems", vm->c_object, OK_PLAIN, cmbi_ctor); c->is_dynamic = 1; a2_add_method(vm, c, "clone", r_self, 0);
    c = a2_def_class(vm, "flash.ui", "ContextMenuItem", ed, OK_PLAIN, cmi_ctor); c->is_dynamic = 1; a2_add_method(vm, c, "clone", r_self, 0);
    c = a2_def_class(vm, "flash.ui", "Multitouch", vm->c_object, OK_PLAIN, NULL); a2_static_getter(vm, c, "supportsTouchEvents", mt_supportsTouchEvents, NULL); a2_static_getter(vm, c, "supportsGestureEvents", mt_supportsTouchEvents, NULL); a2_static_getter(vm, c, "inputMode", mt_inputMode, r_noop); a2_static_getter(vm, c, "maxTouchPoints", r_zero, NULL); a2_static_getter(vm, c, "supportedGestures", r_null, NULL); a2_static_getter(vm, c, "mapTouchToMouse", r_true, r_noop);
    c = a2_def_class(vm, "flash.ui", "MultitouchInputMode", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "NONE", "none"); a2_const_str(vm, c, "GESTURE", "gesture"); a2_const_str(vm, c, "TOUCH_POINT", "touchPoint");
    { const char *ui[] = { "GameInput", "GameInputDevice", "GameInputControl", "ContextMenuClipboardItems", NULL }; int i; for (i = 0; ui[i]; i++) { c = a2_def_class(vm, "flash.ui", ui[i], ed, OK_PLAIN, r_dyn_ctor); c->is_dynamic = 1; if (i == 1 || i == 2) c->flags |= CF_NOINST; if (i == 0) { a2_static_getter(vm, c, "numDevices", r_zero, NULL); a2_static_getter(vm, c, "isSupported", r_false, NULL); a2_add_static(vm, c, "getDeviceAt", gi_getDeviceAt, 1); } } }
    c = a2_def_class(vm, "flash.external", "ExtensionContext", ed, OK_PLAIN, r_dyn_ctor); a2_add_static(vm, c, "createExtensionContext", r_null, 2);
    { extern void a2_filters_init(VM2 *vm); a2_filters_init(vm); }
    /* desktop / printing / accessibility / sampler / profiler / globalization stubs */
    c = a2_def_class(vm, "flash.desktop", "Clipboard", vm->c_object, OK_PLAIN, r_dyn_ctor); c->is_dynamic = 1; a2_static_getter(vm, c, "generalClipboard", clip_generalClipboard, NULL); a2_add_method(vm, c, "setData", clip_setData, 3); a2_add_method(vm, c, "getData", clip_getData, 2); a2_add_method(vm, c, "hasFormat", clip_hasFormat, 1); a2_add_method(vm, c, "clear", r_noop, 0); a2_add_method(vm, c, "clearData", r_noop, 1); a2_add_method(vm, c, "setDataHandler", r_false, 3); a2_add_getter(vm, c, "formats", r_null, NULL);
    c = a2_def_class(vm, "flash.desktop", "ClipboardFormats", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "TEXT_FORMAT", "air:text"); a2_const_str(vm, c, "HTML_FORMAT", "air:html"); a2_const_str(vm, c, "RICH_TEXT_FORMAT", "air:rtf"); a2_const_str(vm, c, "URL_FORMAT", "air:url"); a2_const_str(vm, c, "BITMAP_FORMAT", "air:bitmap"); a2_const_str(vm, c, "FILE_LIST_FORMAT", "air:file list");
    c = a2_def_class(vm, "flash.desktop", "ClipboardTransferMode", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "ORIGINAL_PREFERRED", "originalPreferred"); a2_const_str(vm, c, "ORIGINAL_ONLY", "originalOnly"); a2_const_str(vm, c, "CLONE_PREFERRED", "clonePreferred"); a2_const_str(vm, c, "CLONE_ONLY", "cloneOnly");
    c = a2_def_class(vm, "flash.printing", "PrintJob", ed, OK_PLAIN, r_dyn_ctor); c->is_dynamic = 1; a2_add_method(vm, c, "start", printjob_start, 0); a2_add_method(vm, c, "addPage", r_noop, 4); a2_add_method(vm, c, "send", r_noop, 0); a2_static_getter(vm, c, "isSupported", r_false, NULL);
    c = a2_def_class(vm, "flash.printing", "PrintJobOptions", vm->c_object, OK_PLAIN, PrintJobOptions_ctor); c->is_dynamic = 1;
    c = a2_def_class(vm, "flash.printing", "PrintJobOrientation", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "LANDSCAPE", "landscape"); a2_const_str(vm, c, "PORTRAIT", "portrait");
    c = a2_def_class(vm, "flash.accessibility", "Accessibility", vm->c_object, OK_PLAIN, NULL); a2_static_getter(vm, c, "active", acc_active, NULL); a2_add_static(vm, c, "updateProperties", r_noop, 0); a2_add_static(vm, c, "sendEvent", r_noop, 4);
    c = a2_def_class(vm, "flash.accessibility", "AccessibilityProperties", vm->c_object, OK_PLAIN, r_dyn_ctor); c->is_dynamic = 1;
    c = a2_def_class(vm, "flash.accessibility", "AccessibilityImplementation", vm->c_object, OK_PLAIN, r_dyn_ctor); c->is_dynamic = 1;
    c = a2_def_class(vm, "flash.utils", "Proxy", vm->c_object, OK_PROXY, proxy_ctor); c->is_dynamic = 0;
    g_flash_proxy_ns.kind = NS_NAMESPACE; g_flash_proxy_ns.uri = a2_str(vm, "http://www.adobe.com/2006/actionscript/flash/proxy");
    { struct { const char *n; NativeFn2 f; int np; } pm[] = { { "getProperty", px_getProperty, 1 }, { "setProperty", px_setProperty, 2 }, { "callProperty", px_callProperty, 1 }, { "hasProperty", px_hasProperty, 1 }, { "deleteProperty", px_deleteProperty, 1 },
        { "getDescendants", px_getDescendants, 1 }, { "nextNameIndex", px_nextNameIndex, 1 }, { "nextName", px_nextName, 1 }, { "nextValue", px_nextValue, 1 }, { "isAttribute", px_isAttribute, 1 }, { NULL, NULL, 0 } }; int i; A2Trait *t; extern void a2_tm_put(A2TraitMap *tm, A2Trait *t);
      for (i = 0; pm[i].n; i++) { t = (A2Trait*)calloc(1, sizeof(A2Trait)); t->name = a2_str(vm, pm[i].n); t->ns = g_flash_proxy_ns; t->kind = TK_METHOD; t->method = a2_native_method(vm, pm[i].f, pm[i].n, pm[i].np); t->method->owner = c; a2_tm_put(&c->itraits, t); }
      a2_add_method(vm, c, "hasOwnProperty", px_hasOwnProperty, 1); }
    { A2Ns *ns = (A2Ns*)calloc(1, sizeof(A2Ns)); A2Trait *t; extern void a2_tm_put(A2TraitMap *tm, A2Trait *t); A2Obj *nso; V2 nv; *ns = g_flash_proxy_ns;
      /* flash.utils.flash_proxy namespace constant */
      nv = a2_construct_class(vm, vm->c_namespace, 0, NULL); nso = nv.u.o; nso->nsval = g_flash_proxy_ns; a2_dyn_set(vm, nso, a2_str(vm, "__prefix"), v2_cstr(vm, "flash_proxy"));
      t = (A2Trait*)calloc(1, sizeof(A2Trait)); t->name = a2_str(vm, "flash_proxy"); t->ns.kind = NS_PACKAGE; t->ns.uri = a2_str(vm, "flash.utils"); t->kind = TK_CONST; t->slot = vm->global->cls->nislots++; t->def = nv; t->has_def = 1; a2_tm_put(&vm->global->cls->itraits, t);
      { V2 *s = (V2*)realloc(vm->global->slots, (size_t)vm->global->cls->nislots * sizeof(V2)); if (s) { vm->global->slots = s; vm->global->nslots = vm->global->cls->nislots; vm->global->slots[t->slot] = nv; } } (void)ns; }
    {   /* the top-level AS3 namespace constant (a.AS3::pop()) */
      A2Trait *t; extern void a2_tm_put(A2TraitMap *tm, A2Trait *t); V2 nv = a2_construct_class(vm, vm->c_namespace, 0, NULL);
      nv.u.o->nsval = vm->ns_as3;
      t = (A2Trait*)calloc(1, sizeof(A2Trait)); t->name = a2_str(vm, "AS3"); t->ns = vm->ns_public; t->kind = TK_CONST; t->slot = vm->global->cls->nislots++; t->def = nv; t->has_def = 1; a2_tm_put(&vm->global->cls->itraits, t);
      { V2 *s = (V2*)realloc(vm->global->slots, (size_t)vm->global->cls->nislots * sizeof(V2)); if (s) { vm->global->slots = s; vm->global->nslots = vm->global->cls->nislots; vm->global->slots[t->slot] = nv; } } }
    c = a2_def_class(vm, "flash.utils", "Timer", vm->c_timer, OK_TIMER, NULL); /* alias safety: real Timer registered in events */ (void)c;
    { const char *u[] = { "CompressionAlgorithm", "ObjectInput", "ObjectOutput", "ObjectEncoding", NULL }; int i; for (i = 0; u[i]; i++) { c = a2_def_class(vm, "flash.utils", u[i], vm->c_object, OK_PLAIN, r_dyn_ctor); c->is_dynamic = 1; } }
    { A2Class *ca = a2_class_find_s(vm, "flash.utils::CompressionAlgorithm"); a2_const_str(vm, ca, "ZLIB", "zlib"); a2_const_str(vm, ca, "DEFLATE", "deflate"); a2_const_str(vm, ca, "LZMA", "lzma"); }
    c = a2_def_class(vm, "flash.sampler", "StackFrame", vm->c_object, OK_PLAIN, r_dyn_ctor); c->is_dynamic = 1;
    c = a2_def_class(vm, "flash.profiler", "Telemetry", vm->c_object, OK_PLAIN, NULL); a2_static_getter(vm, c, "connected", r_false, NULL); a2_add_static(vm, c, "sendMetric", r_noop, 2); a2_add_static(vm, c, "sendSpanMetric", r_noop, 3); a2_add_static(vm, c, "registerCommandHandler", r_false, 2); a2_add_static(vm, c, "unregisterCommandHandler", r_false, 1);
    { extern void a2_stage3d_init(VM2 *vm); a2_stage3d_init(vm); }
    { const char *d3[] = { "Context3DProgramType", "Context3DVertexBufferFormat", "Context3DTextureFormat", "Context3DBlendFactor", "Context3DCompareMode", "Context3DTriangleFace", "Context3DRenderMode", "Context3DProfile", "Context3DClearMask", "Context3DStencilAction", "Context3DWrapMode", "Context3DMipFilter", "Context3DTextureFilter", "Context3DBufferUsage", NULL }; int i; for (i = 0; d3[i]; i++) { c = a2_def_class(vm, "flash.display3D", d3[i], vm->c_object, OK_PLAIN, r_dyn_ctor); c->is_dynamic = 1; } }
      { A2Class *k = a2_class_find_s(vm, "flash.display3D::Context3DProgramType"); if (k) { a2_const_str(vm, k, "VERTEX", "vertex"); a2_const_str(vm, k, "FRAGMENT", "fragment"); } }
      { A2Class *k = a2_class_find_s(vm, "flash.display3D::Context3DVertexBufferFormat"); if (k) { a2_const_str(vm, k, "BYTES_4", "bytes4"); a2_const_str(vm, k, "FLOAT_1", "float1"); a2_const_str(vm, k, "FLOAT_2", "float2"); a2_const_str(vm, k, "FLOAT_3", "float3"); a2_const_str(vm, k, "FLOAT_4", "float4"); } }
      { A2Class *k = a2_class_find_s(vm, "flash.display3D::Context3DTextureFormat"); if (k) { a2_const_str(vm, k, "BGRA", "bgra"); a2_const_str(vm, k, "BGRA_PACKED", "bgraPacked4444"); a2_const_str(vm, k, "BGR_PACKED", "bgrPacked565"); a2_const_str(vm, k, "COMPRESSED", "compressed"); a2_const_str(vm, k, "COMPRESSED_ALPHA", "compressedAlpha"); a2_const_str(vm, k, "RGBA_HALF_FLOAT", "rgbaHalfFloat"); } }
      { A2Class *k = a2_class_find_s(vm, "flash.display3D::Context3DBlendFactor"); if (k) { a2_const_str(vm, k, "DESTINATION_ALPHA", "destinationAlpha"); a2_const_str(vm, k, "DESTINATION_COLOR", "destinationColor"); a2_const_str(vm, k, "ONE", "one"); a2_const_str(vm, k, "ONE_MINUS_DESTINATION_ALPHA", "oneMinusDestinationAlpha"); a2_const_str(vm, k, "ONE_MINUS_DESTINATION_COLOR", "oneMinusDestinationColor"); a2_const_str(vm, k, "ONE_MINUS_SOURCE_ALPHA", "oneMinusSourceAlpha"); a2_const_str(vm, k, "ONE_MINUS_SOURCE_COLOR", "oneMinusSourceColor"); a2_const_str(vm, k, "SOURCE_ALPHA", "sourceAlpha"); a2_const_str(vm, k, "SOURCE_COLOR", "sourceColor"); a2_const_str(vm, k, "ZERO", "zero"); } }
      { A2Class *k = a2_class_find_s(vm, "flash.display3D::Context3DCompareMode"); if (k) { a2_const_str(vm, k, "ALWAYS", "always"); a2_const_str(vm, k, "EQUAL", "equal"); a2_const_str(vm, k, "GREATER", "greater"); a2_const_str(vm, k, "GREATER_EQUAL", "greaterEqual"); a2_const_str(vm, k, "LESS", "less"); a2_const_str(vm, k, "LESS_EQUAL", "lessEqual"); a2_const_str(vm, k, "NEVER", "never"); a2_const_str(vm, k, "NOT_EQUAL", "notEqual"); } }
      { A2Class *k = a2_class_find_s(vm, "flash.display3D::Context3DTriangleFace"); if (k) { a2_const_str(vm, k, "BACK", "back"); a2_const_str(vm, k, "FRONT", "front"); a2_const_str(vm, k, "FRONT_AND_BACK", "frontAndBack"); a2_const_str(vm, k, "NONE", "none"); } }
      { A2Class *k = a2_class_find_s(vm, "flash.display3D::Context3DRenderMode"); if (k) { a2_const_str(vm, k, "AUTO", "auto"); a2_const_str(vm, k, "SOFTWARE", "software"); } }
      { A2Class *k = a2_class_find_s(vm, "flash.display3D::Context3DProfile"); if (k) { a2_const_str(vm, k, "BASELINE", "baseline"); a2_const_str(vm, k, "BASELINE_CONSTRAINED", "baselineConstrained"); a2_const_str(vm, k, "BASELINE_EXTENDED", "baselineExtended"); a2_const_str(vm, k, "STANDARD", "standard"); a2_const_str(vm, k, "STANDARD_CONSTRAINED", "standardConstrained"); a2_const_str(vm, k, "STANDARD_EXTENDED", "standardExtended"); } }
      { A2Class *k = a2_class_find_s(vm, "flash.display3D::Context3DStencilAction"); if (k) { a2_const_str(vm, k, "DECREMENT_SATURATE", "decrementSaturate"); a2_const_str(vm, k, "DECREMENT_WRAP", "decrementWrap"); a2_const_str(vm, k, "INCREMENT_SATURATE", "incrementSaturate"); a2_const_str(vm, k, "INCREMENT_WRAP", "incrementWrap"); a2_const_str(vm, k, "INVERT", "invert"); a2_const_str(vm, k, "KEEP", "keep"); a2_const_str(vm, k, "SET", "set"); a2_const_str(vm, k, "ZERO", "zero"); } }
      { A2Class *k = a2_class_find_s(vm, "flash.display3D::Context3DWrapMode"); if (k) { a2_const_str(vm, k, "CLAMP", "clamp"); a2_const_str(vm, k, "CLAMP_U_REPEAT_V", "clamp_u_repeat_v"); a2_const_str(vm, k, "REPEAT", "repeat"); a2_const_str(vm, k, "REPEAT_U_CLAMP_V", "repeat_u_clamp_v"); } }
      { A2Class *k = a2_class_find_s(vm, "flash.display3D::Context3DMipFilter"); if (k) { a2_const_str(vm, k, "MIPLINEAR", "miplinear"); a2_const_str(vm, k, "MIPNEAREST", "mipnearest"); a2_const_str(vm, k, "MIPNONE", "mipnone"); } }
      { A2Class *k = a2_class_find_s(vm, "flash.display3D::Context3DTextureFilter"); if (k) { a2_const_str(vm, k, "ANISOTROPIC16X", "anisotropic16x"); a2_const_str(vm, k, "ANISOTROPIC2X", "anisotropic2x"); a2_const_str(vm, k, "ANISOTROPIC4X", "anisotropic4x"); a2_const_str(vm, k, "ANISOTROPIC8X", "anisotropic8x"); a2_const_str(vm, k, "LINEAR", "linear"); a2_const_str(vm, k, "NEAREST", "nearest"); } }
      { A2Class *k = a2_class_find_s(vm, "flash.display3D::Context3DBufferUsage"); if (k) { a2_const_str(vm, k, "DYNAMIC_DRAW", "dynamicDraw"); a2_const_str(vm, k, "STATIC_DRAW", "staticDraw"); } }
      { A2Class *k = a2_class_find_s(vm, "flash.display3D::Context3DClearMask"); if (k) { a2_add_const(vm, k, "COLOR", v2_int(1)); a2_add_const(vm, k, "DEPTH", v2_int(2)); a2_add_const(vm, k, "STENCIL", v2_int(4)); a2_add_const(vm, k, "ALL", v2_int(7)); } }
    c = a2_def_class(vm, "flash.concurrent", "Mutex", vm->c_object, OK_PLAIN, r_dyn_ctor); a2_add_method(vm, c, "lock", r_noop, 0); a2_add_method(vm, c, "unlock", r_noop, 0); a2_add_method(vm, c, "tryLock", r_true, 0);
    c = a2_def_class(vm, "flash.concurrent", "Condition", vm->c_object, OK_PLAIN, r_dyn_ctor);

    c = a2_def_class(vm, "flash.sensors", "Accelerometer", ed, OK_PLAIN, r_dyn_ctor); a2_static_getter(vm, c, "isSupported", r_false, NULL); c = a2_def_class(vm, "flash.sensors", "Geolocation", ed, OK_PLAIN, r_dyn_ctor); a2_static_getter(vm, c, "isSupported", r_false, NULL);
    /* flash.xml: written in ActionScript (as3/, avm2_builtin.c) */
    c = a2_def_class(vm, "flash.net.drm", "DRMManager", ed, OK_PLAIN, r_dyn_ctor); c->is_dynamic = 1;
    c = a2_def_class(vm, "flash.automation", "ActionGenerator", vm->c_object, OK_PLAIN, r_dyn_ctor); c->is_dynamic = 1;
}
