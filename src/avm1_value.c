/* avm1_value.c - AVM1 value conversions and comparisons (SWF version dependent) */
#include "avm1_int.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

Value v_undef(void) { Value v; v.t = V_UNDEF; v.u.n = 0; return v; }
Value v_null(void) { Value v; v.t = V_NULL; v.u.n = 0; return v; }
Value v_bool(int b) { Value v; v.t = V_BOOL; v.u.n = 0; v.u.b = !!b; return v; }
Value v_num(double n) { Value v; v.t = V_NUM; v.u.n = n; return v; }
Value v_str(AStr *s) { Value v; if (!s) return v_undef(); v.t = V_STR; v.u.s = s; return v; }
Value v_obj(AObj *o) { Value v; if (!o) return v_undef(); v.t = V_OBJ; v.u.o = o; return v; }
Value v_cstr(VM *vm, const char *s) { return v_str(str_cstr(vm, s)); }
Value arg(int argc, Value *argv, int i) { return (i < argc) ? argv[i] : v_undef(); }

int v_is_prim(Value v) { return v.t != V_OBJ && v.t != V_CLIP; }

int v_same(Value a, Value b) {
    if (a.t != b.t) return 0;
    switch (a.t) {
    case V_UNDEF: case V_NULL: return 1;
    case V_BOOL: return a.u.b == b.u.b;
    case V_NUM: return a.u.n == b.u.n || (a.u.n != a.u.n && b.u.n != b.u.n);
    case V_STR: return str_eq(a.u.s, b.u.s);
    case V_OBJ: return a.u.o == b.u.o;
    case V_CLIP: return str_eq(a.u.c->path, b.u.c->path);
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* ECMA integer conversions                                         */

double d_wrap32(double d) {
    double m;
    if (d != d || d > 1.7976931348623157e308 || d < -1.7976931348623157e308) return 0;
    m = fmod(d < 0 ? ceil(d) : floor(d), 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return m;
}
s32 f64_i32(double d) { double m = d_wrap32(d); return m >= 2147483648.0 ? (s32)(m - 4294967296.0) : (s32)m; }
u32 f64_u32(double d) { return (u32)d_wrap32(d); }

/* ---------------------------------------------------------------- */
/* primitives                                                       */

static double prim_as_number(Act *a, Value v) {
    switch (v.t) {
    case V_UNDEF: case V_NULL: return a->swf_version < 7 ? 0.0 : 0.0 / 0.0;
    case V_BOOL: return v.u.b ? 1.0 : 0.0;
    case V_NUM: return v.u.n;
    case V_STR: return str_to_num(v.u.s, a->swf_version);
    case V_OBJ: return a->swf_version < 5 ? 0.0 : 0.0 / 0.0;
    case V_CLIP: return 0.0 / 0.0;
    }
    return 0.0 / 0.0;
}
double v_prim_as_num(Act *a, Value v) { return prim_as_number(a, v); }

Value v_prim_num(Act *a, Value v) {
    if (v.t == V_OBJ && !OBJ_IS_DISPLAY(v.u.o) && !(v.u.o->nk == NK_SUPER && o_display(v.u.o)))
        return o_call_method(a, v.u.o, a->vm->s_valueOf, 0, NULL, ER_SPECIAL);
    return v;
}

double v_num_of(Act *a, Value v) {
    if (v.t == V_OBJ) { Value p = v_prim_num(a, v); if (a->vm->err) return 0.0 / 0.0; return prim_as_number(a, p); }
    if (v.t == V_CLIP) return 0.0 / 0.0;
    return prim_as_number(a, v);
}

s32 v_i32(Act *a, Value v) { return f64_i32(v_num_of(a, v)); }
u32 v_u32(Act *a, Value v) { return f64_u32(v_num_of(a, v)); }
u16 v_u16(Act *a, Value v) { return (u16)f64_u32(v_num_of(a, v)); }
s16 v_i16(Act *a, Value v) { return (s16)(u16)f64_u32(v_num_of(a, v)); }
u8 v_u8(Act *a, Value v) { return (u8)f64_u32(v_num_of(a, v)); }

Value v_prim(Act *a, Value v) {
    VM *vm = a->vm;
    if (v.t == V_OBJ) {
        AObj *o = v.u.o; Value r;
        int is_date = o->nk == NK_DATE || (o->nk == NK_SUPER && o->nat.sup.self && o->nat.sup.self->nk == NK_DATE);
        if (a->swf_version > 5 && is_date) r = o_call_method(a, o, vm->s_toString, 0, NULL, ER_SPECIAL);
        else r = o_call_method(a, o, vm->s_valueOf, 0, NULL, ER_SPECIAL);
        if (vm->err) return v_undef();
        return v_is_prim(r) ? r : v;
    }
    if (v.t == V_CLIP) {
        AObj *o = v_to_obj_or_bare(a, v); Value r;
        r = o_call_method(a, o, vm->s_valueOf, 0, NULL, ER_SPECIAL);
        if (vm->err) return v_undef();
        if (r.t == V_UNDEF) return v_str(v_str_of(a, v));
        return v;
    }
    return v;
}

AStr *v_str_of(Act *a, Value v) {
    VM *vm = a->vm;
    switch (v.t) {
    case V_UNDEF: return a->swf_version < 7 ? vm->s_empty : vm->s_undefined;
    case V_NULL: return vm->s_null;
    case V_BOOL:
        if (a->swf_version < 5) return str_int(vm, v.u.b ? 1 : 0);
        return v.u.b ? vm->s_true : vm->s_false;
    case V_NUM: return str_num(vm, v.u.n);
    case V_STR: return v.u.s;
    case V_CLIP: return clipref_string(a, v.u.c);
    case V_OBJ: {
        AObj *o = v.u.o; Value r;
        if (o->nk == NK_STRING) return o->nat.s;
        if (o->nk == NK_SUPER && o->nat.sup.self && o->nat.sup.self->nk == NK_STRING) return o->nat.sup.self->nat.s;
        { MovieClip *d = o_display_no_super(o); if (d || OBJ_IS_DISPLAY(o)) return d ? clip_path(vm, d) : vm->s_empty; }
        r = o_call_method(a, o, vm->s_toString, 0, NULL, ER_SPECIAL);
        if (vm->err) return vm->s_empty;
        if (r.t == V_STR) return r.u.s;
        return o_as_function(o) ? vm->s_type_Function : vm->s_type_Object;
    }
    }
    return vm->s_empty;
}

int v_as_bool(Value v, int swf_version) {
    switch (v.t) {
    case V_BOOL: return v.u.b;
    case V_NUM: return v.u.n == v.u.n && v.u.n != 0.0;
    case V_STR:
        if (swf_version >= 7) return v.u.s->len > 0;
        { double d = str_to_num(v.u.s, swf_version); return d == d && d != 0.0; }
    case V_OBJ: case V_CLIP: return 1;
    default: return 0;
    }
}
int v_bool_of(Act *a, Value v) { return v_as_bool(v, a->swf_version); }

AObj *v_obj_of(Act *a, Value v) {
    if (v.t == V_OBJ) return v.u.o;
    if (v.t == V_CLIP) return clipref_resolve(a, v.u.c, NULL, NULL);
    return NULL;
}

AObj *v_to_obj(Act *a, Value v) {
    VM *vm = a->vm; AStr *cls; Value c, r;
    switch (v.t) {
    case V_OBJ: return v.u.o;
    case V_CLIP: return clipref_resolve(a, v.u.c, NULL, NULL);
    case V_NULL: case V_UNDEF: return NULL;
    case V_BOOL: cls = vm->s_Boolean; break;
    case V_NUM: cls = vm->s_Number; break;
    default: cls = vm->s_String; break;
    }
    if (!o_get_opt(a, act_global(a), cls, 0, &c) || c.t != V_OBJ) return NULL;
    if (vm->err) return NULL;
    r = o_construct(a, c.u.o, 1, &v);
    if (vm->err) return NULL;
    return r.t == V_OBJ ? r.u.o : NULL;
}

AObj *v_to_obj_or_bare(Act *a, Value v) {
    AObj *o = v_to_obj(a, v);
    return o ? o : obj_bare(a->vm);
}

AStr *v_typeof(Act *a, Value v) {
    VM *vm = a->vm;
    switch (v.t) {
    case V_UNDEF: return vm->s_undefined;
    case V_NULL: return vm->s_null;
    case V_NUM: return str_intern(vm, "number");
    case V_BOOL: return str_intern(vm, "boolean");
    case V_STR: return str_intern(vm, "string");
    case V_CLIP: {
        /* a reference bound to a text field of a SWF 6+ movie is an object; dangling ones are movieclips */
        extern AObj *clipref_resolve(Act *a, ClipRef *r, int *cached, MovieClip **dobj);
        MovieClip *d = NULL; AObj *o = clipref_resolve(a, v.u.c, NULL, &d);
        if (a->vm->err) a->vm->err = 0;
        if (o && d && d->a1 && d->a1->kind == 2 && !(d->movie && d->movie->version < 6)) return str_intern(vm, "object");
        return str_intern(vm, "movieclip"); }
    case V_OBJ: return o_as_function(v.u.o) ? str_intern(vm, "function") : str_intern(vm, "object");
    }
    return vm->s_undefined;
}

/* ---------------------------------------------------------------- */
/* comparisons                                                      */

static int is_plain_object(Value v) { return v.t == V_OBJ && !o_display(v.u.o) && !OBJ_IS_DISPLAY(v.u.o); }

Value v_lt(Act *a, Value x, Value y) {
    Value px, py; double nx, ny;
    px = v_prim_num(a, x); if (a->vm->err) return v_undef();
    if (is_plain_object(px)) return v_bool(0);
    py = v_prim_num(a, y); if (a->vm->err) return v_undef();
    if (is_plain_object(py)) return v_bool(0);
    if (px.t == V_STR && py.t == V_STR) return v_bool(str_cmp(px.u.s, py.u.s) < 0);
    nx = prim_as_number(a, px); ny = prim_as_number(a, py);
    if (nx != nx || ny != ny) return v_undef();
    return v_bool(nx < ny);
}

int v_eq(Act *a, Value self, Value other) {
    Value x, y;
    if (a->swf_version > 5) { x = other; y = self; }
    else {
        x = v_prim_num(a, other); if (a->vm->err) return 0;
        y = v_prim_num(a, self); if (a->vm->err) return 0;
    }
    if ((x.t == V_UNDEF || x.t == V_NULL) && (y.t == V_UNDEF || y.t == V_NULL)) return 1;
    if (x.t == V_BOOL && y.t == V_BOOL) return x.u.b == y.u.b;
    if (x.t == V_STR && y.t == V_STR) return str_eq(x.u.s, y.u.s);
    if (x.t == V_OBJ && y.t == V_OBJ) return x.u.o == y.u.o;
    if (x.t == V_NUM && y.t == V_NUM) return x.u.n == y.u.n || (x.u.n != x.u.n && y.u.n != y.u.n);
    if (x.t == V_BOOL) return v_eq(a, y, v_num(x.u.b ? 1 : 0));
    if (y.t == V_BOOL) return v_eq(a, x, v_num(y.u.b ? 1 : 0));
    if (x.t == V_NUM && y.t == V_STR) return x.u.n == prim_as_number(a, y);
    if (x.t == V_STR && y.t == V_NUM) return y.u.n == prim_as_number(a, x);
    if (x.t == V_CLIP && y.t == V_CLIP) return str_eq(clipref_string(a, x.u.c), clipref_string(a, y.u.c));
    if (x.t == V_OBJ || y.t == V_OBJ) {
        Value obj = x.t == V_OBJ ? x : y, val = x.t == V_OBJ ? y : x, ov;
        ov = v_prim_num(a, obj); if (a->vm->err) return 0;
        return v_is_prim(ov) && v_eq(a, ov, val);
    }
    return 0;
}

int v_instanceof(Act *a, Value v, Value cls) {
    AObj *c, *p, *o; Value pv;
    if (v_is_prim(v)) return 0;
    c = v_to_obj(a, cls); if (!c || a->vm->err) return 0;
    /* Flash reads `prototype` as an own data property on the first frame, but
     * through the __proto__ chain afterwards: there a cyclic chain spins until the
     * script is aborted (the prototype recursion error stops the action list) */
    if (a->vm->player && a->vm->player->tick > 0) {
        AObj *k = c; int depth = 0; pv = v_undef();
        while (k && !o_prototype(a, k, &pv)) {
            Value up;
            if (++depth == 256) { a->vm->err = ERR_PROTO_RECURSION; return 0; }
            up = o_proto(a, k); k = up.t == V_OBJ ? up.u.o : NULL;
        }
        if (!k) return 0;
    }
    else if (!o_prototype(a, c, &pv)) return 0;
    p = v_to_obj(a, pv); if (!p || a->vm->err) return 0;
    o = v_obj_of(a, v); if (!o) return 0;
    return o_is_instance_of(a, o, p);
}
