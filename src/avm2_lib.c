/* avm2_lib.c - AVM2 builtin classes: Object, Class, Function, Array, String,
 * Number/int/uint/Boolean, Math, Error, Vector, Dictionary, Namespace, QName,
 * JSON, Date and the global functions. */
#include "avm2_int.h"
#include "casemap.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

#define ARG(i) a2_arg(argc, argv, i)
#define ARGM(i) ((i) < argc ? argv[i] : v2_hole())   /* missing -> hole sentinel */
#define NUM(i) v2_tonum(vm, ARG(i))
#define INT(i) v2_toint(vm, ARG(i))
#define STR(i) v2_tostr(vm, ARG(i))
#define BOOL(i) v2_tobool(vm, ARG(i))
#define SELFO (self.t == T_OBJ ? self.u.o : NULL)
#define UNUSED (void)vm; (void)self; (void)argc; (void)argv

/* ---------------------------------------------------------------- */
/* UTF-8 <-> code point helpers (AS3 strings are UTF-16; we treat each
 * code point as one unit, which is exact for the BMP)                */

/* Flash strings are UTF-16: decode to UTF-16 code units (astral characters become surrogate
 * pairs, lone surrogates stored as WTF-8 come back as single units) and encode pairs back. */
int a2_utf8_decode(const char *s, u32 len, u32 **out) {
    u32 *u = (u32*)malloc((len * 2 + 1) * sizeof(u32)); u32 i = 0; int n = 0;
    while (i < len) {
        u8 c = (u8)s[i];
        if (c < 0x80) { u[n++] = c; i++; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < len) { u[n++] = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); i += 2; }
        else if ((c & 0xF0) == 0xE0 && i + 2 < len) { u[n++] = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); i += 3; }
        else if ((c & 0xF8) == 0xF0 && i + 3 < len) {
            u32 cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F); i += 4;
            if (cp >= 0x10000) { cp -= 0x10000; u[n++] = 0xD800 + (cp >> 10); u[n++] = 0xDC00 + (cp & 0x3FF); } else u[n++] = cp;
        }
        else { u[n++] = c; i++; }
    }
    *out = u; return n;
}
static int enc1(u32 c, char *o) {
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 63)); o[2] = (char)(0x80 | (c & 63)); return 3; }
    o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 63)); o[2] = (char)(0x80 | ((c >> 6) & 63)); o[3] = (char)(0x80 | (c & 63)); return 4;
}
A2Str *a2_utf8_encode(VM2 *vm, const u32 *u, int n) {
    char *buf = (char*)malloc((size_t)n * 4 + 1); int i, o = 0; A2Str *r;
    for (i = 0; i < n; i++) {
        u32 c = u[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00); i++; }
        o += enc1(c > 0x10FFFF ? 0xFFFD : c, buf + o);
    }
    r = a2_intern(vm, buf, (size_t)o); free(buf); return r;
}
/* ordering by UTF-16 code units (as Flash compares strings) */
int a2_str_cmp16(A2Str *a, A2Str *b) {
    u32 *ua, *ub; int na, nb, i, len, r = 0;
    if (a == b) return 0;
    na = a2_utf8_decode(a->s, a->len, &ua); nb = a2_utf8_decode(b->s, b->len, &ub);
    len = na < nb ? na : nb;
    for (i = 0; i < len && !r; i++) r = (int)ua[i] - (int)ub[i];
    if (r == 0) r = na - nb;
    free(ua); free(ub);
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}
int a2_str_ulen(A2Str *s) { u32 i, n = 0; for (i = 0; i < s->len; i++) { u8 c = (u8)s->s[i]; if ((c & 0xC0) != 0x80) n++; if ((c & 0xF8) == 0xF0) n++; } return (int)n; }   /* UTF-16 units */
V2 a2_string_char_at(VM2 *vm, A2Str *s, int i) {
    u32 *u; int n = a2_utf8_decode(s->s, s->len, &u); V2 r;
    if (i < 0 || i >= n) r = v2_str(vm->s_empty); else r = v2_str(a2_utf8_encode(vm, u + i, 1));
    free(u); return r;
}

/* ---------------------------------------------------------------- */
/* Object                                                           */

static V2 Object_call(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; if (argc == 0 || argv[0].t == T_UNDEF || argv[0].t == T_NULL) return v2_obj(a2_new_plain(vm)); return argv[0]; }
static V2 Object_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_undef(); }
/* methods of the core classes live in the AS3 namespace: not "own" public properties */
static int core_as3_method(VM2 *vm, A2Class *c, A2Trait *t) {
    extern int a2_is_subclass(A2Class *c, A2Class *base);
    if (!t || t->kind != TK_METHOD || !c || c->abc) return 0;
    return c == vm->c_object || c == vm->c_array || c == vm->c_string || c == vm->c_number || c == vm->c_int || c == vm->c_uint || c == vm->c_boolean || c == vm->c_function || c == vm->c_date || c == vm->c_regexp || c == vm->c_xml || c == vm->c_xmllist || (vm->c_vector && a2_is_subclass(c, vm->c_vector));
}
static V2 obj_hasOwnProperty(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Name n; A2Obj *o; V2 tmp; int ism; A2Ns nss[2];
    nss[0] = vm->ns_public; nss[1] = vm->ns_as3;
    n.name = STR(0); n.nss = nss; n.nns = 2; n.any_ns = 0; n.attr = 0;
    if (self.t != T_OBJ) { A2Class *c = v2_class_of(vm, self); A2Trait *t; if (self.t == T_STR && n.name == vm->s_length) return v2_bool(1); t = c ? a2_find_trait(&c->itraits, n.name, &n) : NULL; return v2_bool(t && !core_as3_method(vm, c, t)); }
    o = self.u.o;
    if (o->cls && o->cls->abc) n.nns = 1;   /* user classes: AS3-namespace members are not "own" public properties */
    if (o->kind == OK_CLASS) { A2Class *c; for (c = o->as_class; c; c = A2_STATIC_SUPER(c)) if (a2_find_trait(&c->ctraits, n.name, &n)) return v2_bool(1); if (n.name == vm->s_prototype) return v2_bool(1); }
    { A2Trait *t = o->cls ? a2_find_trait(&o->cls->itraits, n.name, &n) : NULL; if (t && !core_as3_method(vm, o->cls, t)) return v2_bool(1); }
    if (o->kind == OK_ARRAY || o->kind == OK_VECTOR) { u32 idx; if (n.name == vm->s_length) return v2_bool(1); if (a2_is_index(n.name, &idx)) { if (a2_arr_sealed(vm, o)) return v2_bool(0); if (idx < (u32)o->alen) return v2_bool(o->kind == OK_VECTOR || o->arr[idx].t != T_HOLE); if (o->kind == OK_VECTOR) return v2_bool(0); } }
    if (o->kind == OK_DICT || o->kind == OK_FUNCTION) { extern int a2_get_own_for_has(VM2 *vm, A2Obj *o, const A2Name *n, V2 *out, int *ism); if (a2_get_own_for_has(vm, o, &n, &tmp, &ism)) return v2_bool(1); }
    return v2_bool(a2_dyn_find(o, n.name) != NULL);
}
int a2_get_own_for_has(VM2 *vm, A2Obj *o, const A2Name *n, V2 *out, int *ism) {
    if (o->kind == OK_FUNCTION && (n->name == vm->s_prototype || n->name == vm->s_length)) return 1;
    if (o->kind == OK_DICT) { extern int a2_dict_get(VM2 *vm, A2Obj *o, V2 k, V2 *out); if (a2_dict_get(vm, o, v2_str(n->name), out)) return 1; }
    *ism = 0;
    return a2_dyn_find(o, n->name) != NULL;
}
static V2 obj_isPrototypeOf(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 v = ARG(0); A2Obj *o;
    if (self.t != T_OBJ || v.t == T_UNDEF || v.t == T_NULL) return v2_bool(0);
    if (v.t != T_OBJ) { A2Class *c = v2_is_num(v) ? vm->c_number : v2_class_of(vm, v); o = c ? c->prototype : NULL; }
    else o = v.u.o->proto;
    for (; o; o = o->proto) if (o == self.u.o) return v2_bool(1);
    return v2_bool(0);
}
static V2 obj_propertyIsEnumerable(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Str *k = STR(0); u32 idx;
    if (self.t != T_OBJ) return v2_bool(0);
    if (self.u.o->kind == OK_ARRAY && a2_is_index(k, &idx)) return v2_bool(idx < (u32)self.u.o->alen && self.u.o->arr[idx].t != T_HOLE);
    if (self.u.o->kind == OK_NAMESPACE && (!strcmp(k->s, "prefix") || !strcmp(k->s, "uri"))) return v2_bool(1);   /* E4X 13.2.5 */
    if (self.u.o->kind == OK_DICT) { extern int a2_dict_find(VM2 *vm, A2Obj *o, V2 k); if (a2_dict_find(vm, self.u.o, ARG(0)) >= 0) return v2_bool(1); }
    { A2Prop *p = a2_dyn_find(self.u.o, k); return v2_bool(p && !p->hidden); }
}
static V2 obj_setPropertyIsEnumerable(VM2 *vm, V2 self, int argc, V2 *argv) { A2Prop *p = self.t == T_OBJ ? a2_dyn_find(self.u.o, STR(0)) : NULL; if (p) p->hidden = !(argc > 1 ? BOOL(1) : 1); return v2_undef(); }
static V2 bracket(VM2 *vm, const char *kind, A2Str *name) { return v2_str(a2_concat(vm, a2_concat(vm, a2_str(vm, kind), name), a2_str(vm, "]"))); }
static V2 obj_toString(VM2 *vm, V2 self, int argc, V2 *argv) {
    (void)argc; (void)argv;
    if (self.t == T_OBJ) {
        A2Obj *o = self.u.o;
        if (o->kind == OK_CLASS) return bracket(vm, "[class ", o->as_class->name);
        if (o->kind == OK_FUNCTION) { char b[40]; sprintf(b, "[object Function-%d]", o->method && !o->method->native ? (int)o->method->index : 0); return v2_cstr(vm, b); }
        if (o->kind == OK_PRIM) return v2_str(v2_tostr(vm, o->prim));
        if (vm->c_array && o == vm->c_array->prototype) return v2_cstr(vm, "[object Array]");
        return bracket(vm, "[object ", o->cls ? o->cls->name : a2_str(vm, "Object"));
    }
    if (self.t == T_UNDEF || self.t == T_NULL) return v2_str(v2_tostr(vm, self));
    if (v2_is_num(self)) return v2_cstr(vm, "[object Number]");
    { A2Class *c = v2_class_of(vm, self); return bracket(vm, "[object ", c ? c->name : a2_str(vm, "Object")); }
}
static V2 obj_valueOf(VM2 *vm, V2 self, int argc, V2 *argv) { (void)vm; (void)argc; (void)argv; if (self.t == T_OBJ && self.u.o->kind == OK_PRIM) return self.u.o->prim; return self; }

/* ---------------------------------------------------------------- */
/* Function                                                         */

static V2 fn_call(VM2 *vm, V2 self, int argc, V2 *argv) { if (self.t != T_OBJ) return v2_undef(); return a2_call(vm, self, ARG(0), argc > 0 ? argc - 1 : 0, argv + 1); }
static V2 arr_elem(VM2 *vm, A2Obj *a, u32 i);
static V2 fn_apply(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 arr = ARG(1); V2 *args = NULL; int n = 0, i; V2 r;
    if (arr.t == T_OBJ && (arr.u.o->kind == OK_ARRAY || arr.u.o->kind == OK_VECTOR)) { n = arr.u.o->alen; args = (V2*)malloc((size_t)(n + 1) * sizeof(V2)); for (i = 0; i < n; i++) args[i] = arr.u.o->kind == OK_ARRAY ? arr_elem(vm, arr.u.o, (u32)i) : arr.u.o->arr[i]; }
    else if (arr.t != T_UNDEF && arr.t != T_NULL) { a2_throw_error(vm, "TypeError", "Second argument to Function.prototype.apply must be an Array.", 1116); return v2_undef(); }
    r = a2_call(vm, self, ARG(0), n, args);
    free(args); return r;
}
static V2 fn_length(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; if (self.t == T_OBJ && self.u.o->method) return v2_int(self.u.o->method->nparams); return v2_int(0); }
static V2 fn_noop(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_undef(); }
static V2 Function_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = SELFO; (void)argv; if (argc > 0) { a2_throw_error(vm, "EvalError", "The form function('function body') is not supported.", 1066); return v2_undef(); } if (o && !o->method) { o->kind = OK_FUNCTION; o->method = a2_native_method(vm, fn_noop, "Function", 0); } return v2_undef(); }
static V2 Function_call(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; (void)argv; if (argc > 0) { a2_throw_error(vm, "EvalError", "The form function('function body') is not supported.", 1066); return v2_undef(); } return v2_obj(a2_new_native_fn(vm, fn_noop, "Function", 0)); }
static V2 fn_proto_toString(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_cstr(vm, "function Function() {}"); }

/* ---------------------------------------------------------------- */
/* Array                                                            */

static A2Obj *self_arr(V2 self) { return (self.t == T_OBJ && (self.u.o->kind == OK_ARRAY || self.u.o->kind == OK_VECTOR)) ? self.u.o : NULL; }
/* a sealed (non-dynamic) subclass of Array (bug 654807): SWF <= 12 keeps the dense storage the
   methods use but every element read through the property protocol fails ("semisealed");
   SWF 13+ cannot store elements at all */
int a2_arr_sealed(VM2 *vm, A2Obj *a) { if (!a || a->kind != OK_ARRAY || !a->cls || !a->cls->abc || a->cls->is_dynamic) return 0; return vm->swf_version >= 13 ? 2 : 1; }
static int arr_sealed_read(VM2 *vm, A2Obj *a) {
    A2Name n; if (a2_arr_sealed(vm, a) != 1 || a->alen <= 0) return 0;
    n.name = a2_str(vm, "0"); n.nss = &vm->ns_public; n.nns = 1; n.any_ns = 0; n.attr = 0;
    a2_throw_not_found(vm, v2_obj(a), &n); return 1;
}
static int arr_sealed_write(VM2 *vm, A2Obj *a, u32 idx) {
    A2Name n; char b[16]; if (a2_arr_sealed(vm, a) != 2) return 0;
    sprintf(b, "%u", idx); n.name = a2_str(vm, b); n.nss = &vm->ns_public; n.nns = 1; n.any_ns = 0; n.attr = 0;
    a2_throw_cannot_create(vm, v2_obj(a), &n); return 1;
}

static V2 Array_call(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *a = a2_new_array(vm); int i; (void)self;
    if (argc == 1 && v2_is_num(argv[0])) { double d = v2_tonum(vm, argv[0]); if (d < 0 || d != floor(d) || d > 4294967295.0) { a2_throw_id(vm, "RangeError", 1005, v2_tostr(vm, argv[0])->s, NULL); return v2_undef(); } a->biglen = (u32)d; return v2_obj(a); }
    for (i = 0; i < argc; i++) a2_arr_push(vm, a, argv[i]);
    return v2_obj(a);
}
static V2 Array_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *a = SELFO; int i; if (!a) return v2_undef();
    a->kind = OK_ARRAY;
    if (argc == 1 && v2_is_num(argv[0])) {
        double d = v2_tonum(vm, argv[0]);
        if (d < 0 || d != floor(d) || d > 4294967295.0) { a2_throw_id(vm, "RangeError", 1005, v2_tostr(vm, argv[0])->s, NULL); return v2_undef(); }
        if (a->cls != vm->c_array) { a2_set_s(vm, self, "length", v2_uint((u32)d)); return v2_undef(); }   /* subclasses see the length setter */
        a->biglen = (u32)d; return v2_undef();
    }
    if (a->cls != vm->c_array) {
        /* subclasses: the length setter runs first, then the elements are stored */
        a2_set_s(vm, self, "length", v2_uint((u32)argc));
        if (vm->throwing) return v2_undef();
        for (i = 0; i < argc; i++) a2_arr_set(vm, a, i, argv[i]);
        return v2_undef();
    }
    for (i = 0; i < argc; i++) a2_arr_push(vm, a, argv[i]);
    return v2_undef();
}
static V2 arr_length_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *a = self_arr(self); UNUSED; return v2_uint(a ? a2_arr_len(a) : 0); }
static V2 arr_length_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *a = self_arr(self); double d; u32 nl;
    if (!a) return v2_undef();
    if (a2_arr_sealed(vm, a)) return v2_undef();   /* ignored on a sealed Array subclass */
    if (a->kind != OK_ARRAY) { a2_arr_setlen(vm, a, INT(0)); return v2_undef(); }
    d = NUM(0); if (d != d || d - d != 0) d = 0; d = fmod(d < 0 ? -floor(-d) : floor(d), 4294967296.0); if (d < 0) d += 4294967296.0; nl = (u32)d;   /* length:uint wraps (ToUint32) */
    if (nl > (1u << 26)) { if ((u32)a->alen > nl) a->alen = (int)nl; a->biglen = nl; return v2_undef(); }
    a2_arr_setlen(vm, a, (int)nl); return v2_undef();
}
/* element read with prototype fallback for holes (Array methods use the property protocol) */
static V2 arr_elem(VM2 *vm, A2Obj *a, u32 i) {
    V2 v;
    if (i < (u32)a->alen && a->arr[i].t != T_HOLE) return a->arr[i];
    if (a->kind == OK_VECTOR) return i < (u32)a->alen ? a->arr[i] : v2_undef();
    { A2Name n; char b[16]; sprintf(b, "%lu", (unsigned long)i); n.name = a2_str(vm, b); n.nss = &vm->ns_public; n.nns = 1; n.any_ns = 1; n.attr = 0; if (a2_get(vm, v2_obj(a), &n, &v)) { V2_NORM(v); return v; } }
    return v2_undef();
}
static int arr_ilen(A2Obj *a) { u32 L = a2_arr_len(a); return L > (1u << 26) ? (1 << 26) : (int)L; }
static V2 arr_push(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *a = self_arr(self); int i; if (!a) return v2_undef(); if (argc && arr_sealed_write(vm, a, a2_arr_len(a))) return v2_undef();
    if (a->kind == OK_ARRAY && a2_arr_len(a) > (1u << 26)) {   /* huge sparse length: each value goes to index length (saturating at 2^32-1) */
        for (i = 0; i < argc; i++) { u32 idx = a2_arr_len(a); if (!a2_arr_set_sparse(vm, a, idx, argv[i])) a2_dyn_set(vm, a, a2_str(vm, "4294967295"), argv[i]); }
        return v2_uint(a2_arr_len(a));
    }
    for (i = 0; i < argc; i++) a2_arr_push(vm, a, a->kind == OK_VECTOR ? a2_vec_coerce(vm, argv[i], a->vec_type) : argv[i]); return v2_uint(a2_arr_len(a)); }
static V2 arr_pop(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *a = self_arr(self); V2 r; u32 L; UNUSED; if (!a) return v2_undef();
    L = a2_arr_len(a); if (L == 0) return v2_undef();
    if (a->alen == 0) { a->biglen = L - 1; return v2_undef(); }
    r = a->arr[a->alen - 1]; a->alen--;              /* the last element of the dense part */
    if (a->biglen) { a->biglen = L - 1; if (a->biglen <= (u32)a->alen) a->biglen = 0; }
    V2_NORM(r); return r;
}
static V2 arr_shift(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *a = self_arr(self); V2 r; u32 L; UNUSED; if (!a) return v2_undef();
    L = a2_arr_len(a); if (L == 0) return v2_undef();
    if (a->alen == 0) { a->biglen = L - 1; return v2_undef(); }
    r = a->arr[0]; memmove(a->arr, a->arr + 1, (size_t)(a->alen - 1) * sizeof(V2)); a->alen--;
    if (a->biglen) { a->biglen = L - 1; if (a->biglen <= (u32)a->alen) a->biglen = 0; }
    V2_NORM(r); return r;
}
static V2 arr_unshift(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *a = self_arr(self); int i, n; u32 L; if (!a) return v2_undef();
    if (argc && arr_sealed_write(vm, a, a2_arr_len(a) + (u32)argc - 1)) return v2_undef();
    L = a2_arr_len(a); n = a->alen;
    a2_arr_fill(vm, a, n + argc); memmove(a->arr + argc, a->arr, (size_t)n * sizeof(V2)); for (i = 0; i < argc; i++) a->arr[i] = argv[i];
    if (L > (u32)n) a->biglen = L + (u32)argc;
    if ((double)L + argc > 4294967295.0) { V2 nl = v2_num((double)L + argc); arr_length_set(vm, self, 1, &nl); }   /* the new length wraps (ToUint32) */
    return v2_uint(a2_arr_len(a));
}
/* Array.prototype push/pop/shift/unshift: generic versions working through the property
   protocol on any array-like (ByteArray, objects with a length); public:: calls reach them */
static double gen_u32(double d) { if (d != d || d - d != 0) return 0; d = fmod(d < 0 ? -floor(-d) : floor(d), 4294967296.0); return d < 0 ? d + 4294967296.0 : d; }
static A2Name gen_name(VM2 *vm, double i) { A2Name n; char b[32]; sprintf(b, "%.0f", i); n.name = a2_str(vm, b); n.nss = &vm->ns_public; n.nns = 1; n.any_ns = 0; n.attr = 0; return n; }
static double gen_len(VM2 *vm, V2 o) { return gen_u32(v2_tonum(vm, a2_get_s(vm, o, "length"))); }
static V2 gen_get(VM2 *vm, V2 o, double i) { A2Name n = gen_name(vm, i); V2 v; if (!a2_get(vm, o, &n, &v)) v = v2_undef(); return v; }
static void gen_put(VM2 *vm, V2 o, double i, V2 v) { A2Name n = gen_name(vm, i); a2_set(vm, o, &n, v); }
static void gen_del(VM2 *vm, V2 o, double i) { A2Name n = gen_name(vm, i); a2_delete(vm, o, &n); }
static int gen_has(VM2 *vm, V2 o, double i) { A2Name n = gen_name(vm, i); return a2_has(vm, o, &n); }
static V2 gen_push(VM2 *vm, V2 self, int argc, V2 *argv) {
    double L; int i; A2Obj *a = self_arr(self);
    if (a && a->kind == OK_ARRAY && (double)a2_arr_len(a) + argc > 4294967295.0) {
        double nl = (double)a2_arr_len(a) + argc; V2 v; arr_push(vm, self, argc, argv); v = v2_num(nl); arr_length_set(vm, self, 1, &v); return v2_uint(a2_arr_len(a));
    }
    if (a || self.t != T_OBJ) return arr_push(vm, self, argc, argv);
    L = gen_len(vm, self);
    for (i = 0; i < argc && !vm->throwing; i++) gen_put(vm, self, L + i, argv[i]);
    if (vm->throwing) return v2_undef();
    a2_set_s(vm, self, "length", v2_num(gen_u32(L + argc)));   /* uint arithmetic */
    return v2_uint((u32)gen_u32(L + argc));
}
static V2 gen_pop(VM2 *vm, V2 self, int argc, V2 *argv) {
    double L; V2 v;
    if (self_arr(self) || self.t != T_OBJ) return arr_pop(vm, self, argc, argv);
    L = gen_len(vm, self);
    if (L == 0) { a2_set_s(vm, self, "length", v2_int(0)); return v2_undef(); }
    v = gen_get(vm, self, L - 1); gen_del(vm, self, L - 1); a2_set_s(vm, self, "length", v2_num(L - 1));
    return v;
}
static V2 gen_shift(VM2 *vm, V2 self, int argc, V2 *argv) {
    double L, k; V2 first;
    if (self_arr(self) || self.t != T_OBJ) return arr_shift(vm, self, argc, argv);
    L = gen_len(vm, self);
    if (L == 0) { a2_set_s(vm, self, "length", v2_int(0)); return v2_undef(); }
    if (L >= 2147483648.0) return v2_undef();   /* (avmplus leaves huge array-likes alone) */
    first = gen_get(vm, self, 0);
    for (k = 1; k < L && !vm->throwing; k++) { if (gen_has(vm, self, k)) gen_put(vm, self, k - 1, gen_get(vm, self, k)); else gen_del(vm, self, k - 1); }
    gen_del(vm, self, L - 1); a2_set_s(vm, self, "length", v2_num(L - 1));
    return first;
}
static V2 gen_unshift(VM2 *vm, V2 self, int argc, V2 *argv) {
    double L, k; int j;
    if (self_arr(self) || self.t != T_OBJ) return arr_unshift(vm, self, argc, argv);
    L = gen_len(vm, self);
    if (L >= 2147483648.0) return v2_uint((u32)L);   /* (avmplus leaves huge array-likes alone) */
    for (k = L; k > 0 && !vm->throwing; k--) { if (gen_has(vm, self, k - 1)) gen_put(vm, self, k + argc - 1, gen_get(vm, self, k - 1)); else gen_del(vm, self, k + argc - 1); }
    for (j = 0; j < argc && !vm->throwing; j++) gen_put(vm, self, j, argv[j]);
    a2_set_s(vm, self, "length", v2_num(gen_u32(L + argc)));   /* uint arithmetic */
    return v2_uint((u32)gen_u32(L + argc));
}
static int norm_index(VM2 *vm, V2 v, int len, int def) {
    double d; if (v.t == T_HOLE) return def;   /* T_HOLE marks a missing argument */
    d = v2_tonum(vm, v); if (d != d) d = 0;
    if (d < 0) { d += len; if (d < 0) d = 0; }
    if (d > len) d = len;
    return (int)d;
}
static V2 arr_slice(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *a = self_arr(self), *r; int s, e, i; if (!a) return v2_undef();
    s = norm_index(vm, ARGM(0), arr_ilen(a), 0); e = norm_index(vm, ARGM(1), arr_ilen(a), arr_ilen(a));
    r = a->kind == OK_VECTOR ? a2_new(vm, a->cls) : a2_new_array(vm); r->kind = a->kind; r->vec_type = a->vec_type;
    for (i = s; i < e; i++) { V2 v = i < a->alen ? a->arr[i] : v2_hole(); if (v.t == T_HOLE && a->kind == OK_ARRAY) { V2 pv = arr_elem(vm, a, (u32)i); if (pv.t != T_UNDEF) v = pv; } a2_arr_push(vm, r, v); }
    return v2_obj(r);
}
static V2 arr_splice(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *a = self_arr(self), *r; int s, del, i, ins; if (!a) return v2_undef();
    r = a->kind == OK_VECTOR ? a2_new(vm, a->cls) : a2_new_array(vm); r->kind = a->kind; r->vec_type = a->vec_type;
    if (argc == 0) return v2_undef();
    if (a->kind == OK_ARRAY && a2_arr_len(a) > (u32)a->alen) a2_arr_fill(vm, a, arr_ilen(a));
    s = norm_index(vm, ARG(0), a->alen, 0);
    if (argc > 2 && arr_sealed_write(vm, a, (u32)s)) return v2_undef();
    if (argc > 1 && a2_arr_sealed(vm, a) == 1 && NUM(1) > 0 && arr_sealed_read(vm, a)) return v2_undef();
    /* elements that move are read through the property protocol (holes take prototype values) */
    if (a->kind == OK_ARRAY) { for (i = s; i < a->alen; i++) if (a->arr[i].t == T_HOLE) { V2 pv = arr_elem(vm, a, (u32)i); if (pv.t != T_UNDEF) a->arr[i] = pv; } }
    if (argc < 2) del = a->alen - s; else { double d = NUM(1); if (d != d) d = 0; del = (int)d; if (del < 0) del = 0; if (s + del > a->alen) del = a->alen - s; }
    ins = argc > 2 ? argc - 2 : 0;
    for (i = 0; i < del; i++) a2_arr_push(vm, r, a->arr[s + i]);
    if (ins != del) {
        int newlen = a->alen - del + ins;
        if (ins > del) a2_arr_fill(vm, a, newlen);
        memmove(a->arr + s + ins, a->arr + s + del, (size_t)(a->alen - s - del - (ins > del ? ins - del : 0)) * sizeof(V2));
        a->alen = newlen;
    }
    for (i = 0; i < ins; i++) a->arr[s + i] = a->kind == OK_VECTOR ? a2_vec_coerce(vm, argv[2 + i], a->vec_type) : argv[2 + i];
    a->biglen = 0;
    return v2_obj(r);
}
static V2 arr_concat(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *a = self_arr(self), *r; int i, k; if (!a) return v2_undef();
    r = a->kind == OK_VECTOR ? a2_new(vm, a->cls) : a2_new_array(vm); r->kind = a->kind; r->vec_type = a->vec_type;
    for (i = 0; i < arr_ilen(a); i++) a2_arr_push(vm, r, i < a->alen ? a->arr[i] : v2_hole());
    for (k = 0; k < argc; k++) {
        if (argv[k].t == T_OBJ && (argv[k].u.o->kind == OK_ARRAY || argv[k].u.o->kind == OK_VECTOR)) { A2Obj *b = argv[k].u.o; for (i = 0; i < arr_ilen(b); i++) a2_arr_push(vm, r, i < b->alen ? b->arr[i] : v2_hole()); }
        else a2_arr_push(vm, r, argv[k]);
    }
    return v2_obj(r);
}
static A2Str *join_impl(VM2 *vm, A2Obj *a, A2Str *sep) {
    size_t cap = 256, len = 0; char *buf = (char*)malloc(cap); int i, n = arr_ilen(a); A2Str *r;
    for (i = 0; i < n; i++) {
        V2 el = arr_elem(vm, a, (u32)i);
        A2Str *s = (el.t == T_UNDEF || el.t == T_NULL || el.t == T_HOLE) ? vm->s_empty : v2_tostr(vm, el);
        if (vm->throwing) break;
        if (len + s->len + sep->len + 1 > cap) { while (len + s->len + sep->len + 1 > cap) cap *= 2; buf = (char*)realloc(buf, cap); }
        if (i) { memcpy(buf + len, sep->s, sep->len); len += sep->len; }
        memcpy(buf + len, s->s, s->len); len += s->len;
    }
    r = a2_intern(vm, buf, len); free(buf); return r;
}
static V2 arr_join(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *a = self_arr(self); A2Str *sep; if (!a) return v2_undef(); if (arr_sealed_read(vm, a)) return v2_undef(); sep = (argc > 0 && argv[0].t != T_UNDEF) ? STR(0) : a2_str(vm, ","); return v2_str(join_impl(vm, a, sep)); }
static V2 arr_toString(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *a = self_arr(self); UNUSED; if (!a) return obj_toString(vm, self, 0, NULL); if (arr_sealed_read(vm, a)) return v2_undef(); return v2_str(join_impl(vm, a, a2_str(vm, ","))); }
/* toLocaleString: each element's toLocaleString() (String/Boolean primitives only have Object's) */
static V2 arr_toLocaleString(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *a = self_arr(self); int i, n; A2Str *r = vm->s_empty; UNUSED;
    if (!a) return obj_toString(vm, self, 0, NULL);
    if (arr_sealed_read(vm, a)) return v2_undef();
    n = arr_ilen(a);
    for (i = 0; i < n && !vm->throwing; i++) {
        V2 el = arr_elem(vm, a, (u32)i); A2Str *es;
        if (el.t == T_UNDEF || el.t == T_NULL) es = vm->s_empty;
        else if (el.t == T_STR) es = a2_str(vm, "[object String]");
        else if (el.t == T_BOOL) es = a2_str(vm, "[object Boolean]");
        else if (el.t == T_OBJ) { A2Name nm; V2 rv; nm.name = a2_str(vm, "toLocaleString"); nm.nss = &vm->ns_public; nm.nns = 1; nm.any_ns = 1; nm.attr = 0; rv = a2_call_prop(vm, el, &nm, 0, NULL); es = vm->throwing ? vm->s_empty : v2_tostr(vm, rv); }
        else es = v2_tostr(vm, el);
        if (i) r = a2_concat(vm, r, a2_str(vm, ","));
        r = a2_concat(vm, r, es);
    }
    return v2_str(r);
}
static V2 arr_reverse(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *a = self_arr(self); int i; UNUSED; if (!a) return v2_undef(); for (i = 0; i < a->alen / 2; i++) { V2 t = a->arr[i]; a->arr[i] = a->arr[a->alen - 1 - i]; a->arr[a->alen - 1 - i] = t; } return self; }
static V2 arr_indexOf(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *a = self_arr(self); int i, s, n; if (!a) return v2_int(-1); if (arr_sealed_read(vm, a)) return v2_undef(); n = arr_ilen(a); s = norm_index(vm, ARG(1), n, 0); for (i = s; i < n; i++) if (v2_strict_eq(vm, arr_elem(vm, a, (u32)i), ARG(0))) return v2_int(i); return v2_int(-1); }
static V2 arr_lastIndexOf(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *a = self_arr(self); int i, s, n; if (!a) return v2_int(-1); if (arr_sealed_read(vm, a)) return v2_undef(); n = arr_ilen(a); s = argc > 1 ? norm_index(vm, ARG(1), n, n - 1) : n - 1; if (s >= n) s = n - 1; for (i = s; i >= 0; i--) if (v2_strict_eq(vm, arr_elem(vm, a, (u32)i), ARG(0))) return v2_int(i); return v2_int(-1); }
static V2 call3(VM2 *vm, V2 fn, V2 thisv, V2 item, int i, V2 arr) { V2 args[3]; V2_NORM(item); args[0] = item; args[1] = v2_int(i); args[2] = arr; return a2_call(vm, fn, thisv, 3, args); }
static V2 arr_forEach(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *a = self_arr(self); int i; if (!a) return v2_undef(); if (arr_sealed_read(vm, a)) return v2_undef(); if (ARG(0).t == T_NULL || ARG(0).t == T_UNDEF) { return v2_undef(); } for (i = 0; i < arr_ilen(a) && !vm->throwing; i++) call3(vm, ARG(0), ARG(1), arr_elem(vm, a, (u32)i), i, self); return v2_undef(); }
static V2 arr_map(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *a = self_arr(self), *r; int i; if (!a) return v2_undef(); if (arr_sealed_read(vm, a)) return v2_undef(); r = a->kind == OK_VECTOR ? a2_new(vm, a->cls) : a2_new_array(vm); r->kind = a->kind; r->vec_type = a->vec_type; if (ARG(0).t == T_NULL || ARG(0).t == T_UNDEF) return v2_obj(r); for (i = 0; i < arr_ilen(a) && !vm->throwing; i++) a2_arr_push(vm, r, call3(vm, ARG(0), ARG(1), arr_elem(vm, a, (u32)i), i, self)); return v2_obj(r); }
static V2 arr_filter(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *a = self_arr(self), *r; int i; if (!a) return v2_undef(); if (arr_sealed_read(vm, a)) return v2_undef(); r = a->kind == OK_VECTOR ? a2_new(vm, a->cls) : a2_new_array(vm); r->kind = a->kind; r->vec_type = a->vec_type; if (ARG(0).t == T_NULL || ARG(0).t == T_UNDEF) return v2_obj(r); for (i = 0; i < arr_ilen(a) && !vm->throwing; i++) { V2 el = arr_elem(vm, a, (u32)i); if (v2_tobool(vm, call3(vm, ARG(0), ARG(1), el, i, self))) a2_arr_push(vm, r, el); } return v2_obj(r); }
static V2 arr_every(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *a = self_arr(self); int i; if (!a) return v2_bool(0); if (arr_sealed_read(vm, a)) return v2_undef(); if (ARG(0).t == T_NULL || ARG(0).t == T_UNDEF) { return v2_bool(1); } for (i = 0; i < arr_ilen(a) && !vm->throwing; i++) if (!v2_tobool(vm, call3(vm, ARG(0), ARG(1), arr_elem(vm, a, (u32)i), i, self))) return v2_bool(0); return v2_bool(1); }
static V2 arr_some(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *a = self_arr(self); int i; if (!a) return v2_bool(0); if (arr_sealed_read(vm, a)) return v2_undef(); if (ARG(0).t == T_NULL || ARG(0).t == T_UNDEF) { return v2_bool(0); } for (i = 0; i < arr_ilen(a) && !vm->throwing; i++) if (v2_tobool(vm, call3(vm, ARG(0), ARG(1), arr_elem(vm, a, (u32)i), i, self))) return v2_bool(1); return v2_bool(0); }
static V2 arr_insertAt(VM2 *vm, V2 self, int argc, V2 *argv) { V2 a3[3]; a3[0] = ARG(0); a3[1] = v2_int(0); a3[2] = ARG(1); (void)argc; arr_splice(vm, self, 3, a3); return v2_undef(); }
static V2 arr_removeAt(VM2 *vm, V2 self, int argc, V2 *argv) { V2 a2[2], r; A2Obj *a = self_arr(self); int n; if (!a) return v2_undef(); n = arr_ilen(a); a2[0] = v2_int(norm_index(vm, ARG(0), n, 0)); a2[1] = v2_int(1); (void)argc; r = arr_splice(vm, self, 2, a2); if (r.t == T_OBJ && r.u.o->alen > 0) { V2 e = r.u.o->arr[0]; return e.t == T_HOLE ? v2_undef() : e; } return v2_undef(); }

static V2 str_toLowerCase(VM2 *vm, V2 self, int argc, V2 *argv);
/* sort: a faithful port of the AVM2 (avmplus) ArraySort: index-based, non-stable quicksort
 * with the {defined | undefined | absent} partition, bug-compatibility by SWF version. */
enum { SK_CASEINSENSITIVE = 1, SK_DESCENDING = 2, SK_UNIQUESORT = 4, SK_RETURNINDEXEDARRAY = 8, SK_NUMERIC = 16 };
typedef struct {
    VM2 *vm; A2Obj *d; int options;
    int mode;                   /* 0 string 1 caseinsensitive 2 numeric 3 script 4 field */
    int desc;                   /* DescendingCompareFunc wrapper (swapped args) */
    V2 fn;
    int nfields; A2Str **fields; int *fopts;
    V2 *atoms; u32 *index; V2 *fieldatoms;
} ASort;
static V2 as_get(ASort *s, u32 i) { V2 v = s->atoms[s->index[i]]; V2_NORM(v); return v; }
static int as_defined(V2 v) { V2_NORM(v); return v.t != T_UNDEF; }
static A2Str *as_lower(VM2 *vm, A2Str *str) { V2 r = str_toLowerCase(vm, v2_str(str), 0, NULL); return r.t == T_STR ? r.u.s : str; }
static int as_strcmp(A2Str *a, A2Str *b) { int r = strcmp(a->s, b->s); return r < 0 ? -1 : r > 0 ? 1 : 0; }
static int as_numcmp(double x, double y) {
    double diff = x - y;
    if (diff == diff) return diff < 0 ? -1 : diff > 0 ? 1 : 0;
    if (y == y) return 1;
    if (x == x) return -1;
    return 0;
}
static int as_compare_raw(ASort *s, u32 j, u32 k);
/* would avmplus hold this value as a (29-bit) integer atom? */
static int as_intatom(V2 v) { double d; if (v.t == T_INT) return v.u.i >= -(1 << 28) && v.u.i < (1 << 28); if (v.t == T_UINT) return v.u.u < (1u << 28); if (v.t != T_NUM) return 0; d = v.u.n; return d == floor(d) && d >= -(1 << 28) && d < (1 << 28) && !(d == 0 && 1 / d < 0); }
static int as_field_compare(ASort *s, u32 lhs, u32 rhs) {
    VM2 *vm = s->vm; V2 jv = as_get(s, lhs), kv = as_get(s, rhs); int opt = s->options, result = 0, i;
    A2Obj *oj = jv.t == T_OBJ ? jv.u.o : NULL, *ok = kv.t == T_OBJ ? kv.u.o : NULL;
    if (!(oj && ok)) { result = ok ? 1 : oj ? -1 : 0; return (opt & SK_DESCENDING) ? -result : result; }
    for (i = 0; i < s->nfields; i++) {
        A2Name n; V2 x = v2_undef(), y = v2_undef(); int hx, hy;
        n.name = s->fields[i]; n.nss = &vm->ns_public; n.nns = 1; n.any_ns = 0; n.attr = 0;
        opt = s->fopts[i];
        hx = a2_get(vm, jv, &n, &x); hy = a2_get(vm, kv, &n, &y);
        if (vm->throwing) { vm->throwing = 0; vm->exc = v2_undef(); }
        V2_NORM(x); V2_NORM(y);
        if (!(as_defined(x) && as_defined(y))) {
            if (as_defined(y)) result = 1; else if (as_defined(x)) result = -1;
            else result = (!hx && hy) ? 1 : (hx && !hy) ? -1 : 0;
        } else if (opt & SK_NUMERIC) result = as_numcmp(v2_tonum(vm, x), v2_tonum(vm, y));
        else {
            A2Str *sl = v2_tostr(vm, x), *sr = v2_tostr(vm, y);
            if (opt & SK_CASEINSENSITIVE) { sl = as_lower(vm, sl); sr = as_lower(vm, sr); }
            result = as_strcmp(sl, sr);
        }
        if (result != 0) break;
    }
    return (opt & SK_DESCENDING) ? -result : result;
}
static int as_compare_raw(ASort *s, u32 j, u32 k) {
    VM2 *vm = s->vm;
    switch (s->mode) {
    case 0: return as_strcmp(v2_tostr(vm, as_get(s, j)), v2_tostr(vm, as_get(s, k)));
    case 1: return as_strcmp(as_lower(vm, v2_tostr(vm, as_get(s, j))), as_lower(vm, v2_tostr(vm, as_get(s, k))));
    case 2: {
        V2 a = as_get(s, j), b = as_get(s, k);
        /* bugzilla524122 (SWF < 11): integer atoms are subtracted as 32-bit tagged atoms (value << 3) */
        if (vm->swf_version < 11 && as_intatom(a) && as_intatom(b)) {
            s32 r = (s32)(((u32)(s32)v2_tonum(vm, a) << 3) - ((u32)(s32)v2_tonum(vm, b) << 3)); return r < 0 ? -1 : r > 0 ? 1 : 0;
        }
        return as_numcmp(v2_tonum(vm, a), v2_tonum(vm, b));
    }
    case 3: {
        V2 args[2]; V2 rv; args[0] = as_get(s, j); args[1] = as_get(s, k);
        rv = a2_call(vm, s->fn, v2_obj(s->d), 2, args);
        if (vm->swf_version < 13) { double r = v2_tonum(vm, rv); r = r != r ? 0 : (r < 0 ? ceil(r) : floor(r)); return r > 0 ? 1 : r < 0 ? -1 : 0; }   /* bugzilla532454 */
        { double r = v2_tonum(vm, rv); return r > 0 ? 1 : r < 0 ? -1 : 0; }
    }
    default: return as_field_compare(s, j, k);
    }
}
static int as_compare(ASort *s, u32 j, u32 k) { if (s->vm->throwing) return 0; return s->desc ? as_compare_raw(s, k, j) : as_compare_raw(s, j, k); }
static void as_swap(ASort *s, u32 j, u32 k) { u32 t = s->index[j]; s->index[j] = s->index[k]; s->index[k] = t; }
static void as_qsort(ASort *s, u32 lo, u32 hi) {
    u32 size; struct { u32 lo, hi; } stk[33]; int stkptr = 0;
    if (lo >= hi) return;
recurse:
    size = (hi - lo) + 1;
    if (size < 4) {
        if (size == 3) {
            if (as_compare(s, lo, lo + 1) > 0) { as_swap(s, lo, lo + 1); if (as_compare(s, lo + 1, lo + 2) > 0) { as_swap(s, lo + 1, lo + 2); if (as_compare(s, lo, lo + 1) > 0) as_swap(s, lo, lo + 1); } }
            else { if (as_compare(s, lo + 1, lo + 2) > 0) { as_swap(s, lo + 1, lo + 2); if (as_compare(s, lo, lo + 1) > 0) as_swap(s, lo, lo + 1); } }
        } else if (size == 2) { if (as_compare(s, lo, lo + 1) > 0) as_swap(s, lo, lo + 1); }
    } else {
        u32 pivot = lo + (size / 2), left, right;
        as_swap(s, pivot, lo);
        left = lo; right = hi + 1;
        for (;;) {
            do { left++; } while ((left <= hi) && (as_compare(s, left, lo) <= 0));
            do { right--; } while ((right > lo) && (as_compare(s, right, lo) >= 0));
            if (right < left) break;
            as_swap(s, left, right);
        }
        as_swap(s, lo, right);
        if (s->vm->throwing) return;
        if ((right - 1 - lo) >= (hi - left)) {
            if ((lo + 1) < right) { stk[stkptr].lo = lo; stk[stkptr].hi = right - 1; ++stkptr; }
            if (left < hi) { lo = left; goto recurse; }
        } else {
            if (left < hi) { stk[stkptr].lo = left; stk[stkptr].hi = hi; ++stkptr; }
            if ((lo + 1) < right) { hi = right - 1; goto recurse; }
        }
    }
    if (--stkptr >= 0) { lo = stk[stkptr].lo; hi = stk[stkptr].hi; goto recurse; }
}
static V2 as_run(VM2 *vm, A2Obj *d, ASort *s) {
    u32 len, i, j, newlen, iFirstAbsent; V2 result;
    len = a2_arr_len(d);
    if (len == 0) return v2_obj(d);
    if (len > (1u << 24)) return v2_obj(d);
    if (d->kind == OK_ARRAY && len > (u32)d->alen) a2_arr_fill(vm, d, (int)len);
    s->vm = vm; s->d = d;
    s->index = (u32*)malloc(len * sizeof(u32)); s->atoms = (V2*)malloc(len * sizeof(V2)); s->fieldatoms = NULL;
    j = len; newlen = len;
    if (s->mode == 4 && s->nfields == 1) {
        /* one field: pre-fetch the field values and sort those */
        s->fieldatoms = (V2*)malloc(len * sizeof(V2));
        for (i = len - 1; (i + 1) != 0; i--) {
            V2 a = arr_elem(vm, d, i), x = v2_undef();
            s->index[i] = i; s->fieldatoms[i] = a;
            if (a.t == T_OBJ) { A2Name n; n.name = s->fields[0]; n.nss = &vm->ns_public; n.nns = 1; n.any_ns = 0; n.attr = 0; a2_get(vm, a, &n, &x); if (vm->throwing) { vm->throwing = 0; vm->exc = v2_undef(); x = v2_undef(); } }
            V2_NORM(x); s->atoms[i] = x;
            if (!as_defined(x)) {
                u32 temp; j--; temp = s->index[i]; s->index[i] = s->index[j];
                if (!(i < (u32)d->alen && d->arr[i].t != T_HOLE)) { newlen--; s->index[j] = s->index[newlen]; s->index[newlen] = temp; }
                else s->index[j] = temp;
            }
        }
        { int opt = s->fopts[0]; s->mode = (opt & SK_NUMERIC) ? 2 : (opt & SK_CASEINSENSITIVE) ? 1 : 0; if (opt & SK_DESCENDING) s->desc = 1; }
    } else {
        for (i = len - 1; (i + 1) != 0; i--) {
            V2 a = arr_elem(vm, d, i);
            s->index[i] = i; s->atoms[i] = a;
            if (s->mode == 2 && !v2_is_num(a)) {
                double val = v2_tonum(vm, a);
                if (val != val) { a2_throw_coerce_error(vm, a, vm->c_number); free(s->index); free(s->atoms); return v2_undef(); }
            }
            if (!as_defined(a)) {
                u32 temp; j--; temp = s->index[i]; s->index[i] = s->index[j];
                if (!(i < (u32)d->alen && d->arr[i].t != T_HOLE)) { newlen--; s->index[j] = s->index[newlen]; s->index[newlen] = temp; }
                else s->index[j] = temp;
            }
        }
    }
    iFirstAbsent = newlen;
    if (j > 0) as_qsort(s, 0, j - 1);
    if (vm->throwing) { free(s->index); free(s->atoms); free(s->fieldatoms); return v2_undef(); }
    if (s->options & SK_UNIQUESORT) {
        for (i = 0; i + 1 < len; i++) if (as_compare(s, i, i + 1) == 0) { free(s->index); free(s->atoms); free(s->fieldatoms); return v2_uint(0); }
    }
    if (s->options & SK_RETURNINDEXEDARRAY) {
        A2Obj *r = a2_new_array(vm);
        for (i = 0; i < len; i++) a2_arr_push(vm, r, v2_uint(s->index[i]));
        result = v2_obj(r);
    } else {
        V2 *src = s->fieldatoms ? s->fieldatoms : s->atoms;
        for (i = 0; i < iFirstAbsent; i++) { V2 v = src[s->index[i]]; V2_NORM(v); d->arr[i] = v; }
        for (i = iFirstAbsent; i < len; i++) if (i < (u32)d->alen) d->arr[i] = v2_hole();
        result = v2_obj(d);
    }
    free(s->index); free(s->atoms); free(s->fieldatoms);
    return result;
}
static V2 arr_sort(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *a = self_arr(self); ASort s; int opt = 0; if (!a) return v2_undef();
    if (arr_sealed_read(vm, a)) return v2_undef();
    memset(&s, 0, sizeof(s)); s.fn = v2_undef();
    if (argc >= 1) {
        if (argv[0].t == T_OBJ) {
            if (argv[0].u.o->kind != OK_FUNCTION && argv[0].u.o->kind != OK_CLASS) { a2_throw_coerce_error(vm, argv[0], vm->c_function); return v2_undef(); }
            s.fn = argv[0]; s.mode = 3;
            if (argc >= 2) { if (v2_is_num(argv[1])) opt = v2_toint(vm, argv[1]); else { a2_throw_coerce_error(vm, argv[1], vm->c_number); return v2_undef(); } }
        } else if (v2_is_num(argv[0])) opt = v2_toint(vm, argv[0]);
        else { a2_throw_coerce_error(vm, argv[0], vm->c_function); return v2_undef(); }
    }
    s.options = opt;
    if (s.mode != 3) s.mode = (opt & SK_NUMERIC) ? 2 : (opt & SK_CASEINSENSITIVE) ? 1 : 0;
    if (opt & SK_DESCENDING) s.desc = 1;
    return as_run(vm, a, &s);
}
static V2 arr_sortOn(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *a = self_arr(self); ASort s; A2Str *f1[64]; int o1[64]; int i, options = 0; if (!a) return v2_undef();
    if (arr_sealed_read(vm, a)) return v2_undef();
    memset(&s, 0, sizeof(s)); s.fn = v2_undef(); s.fields = f1; s.fopts = o1; s.mode = 4;
    if (ARG(0).t == T_OBJ && ARG(0).u.o->kind == OK_ARRAY) {
        A2Obj *fa = ARG(0).u.o; int n = (int)a2_arr_len(fa); if (n > 64) n = 64;
        for (i = 0; i < n; i++) { f1[i] = v2_tostr(vm, arr_elem(vm, fa, (u32)i)); o1[i] = 0; }
        s.nfields = n;
        if (ARG(1).t == T_OBJ && ARG(1).u.o->kind == OK_ARRAY) {
            A2Obj *oa = ARG(1).u.o;
            if ((int)a2_arr_len(oa) == n) { options = v2_toint(vm, arr_elem(vm, oa, 0)); for (i = 0; i < n; i++) o1[i] = v2_toint(vm, arr_elem(vm, oa, (u32)i)); }
        } else { options = v2_toint(vm, ARG(1)); for (i = 0; i < n; i++) o1[i] = options; }
    } else {
        f1[0] = v2_tostr(vm, ARG(0)); options = v2_toint(vm, ARG(1)); o1[0] = options; s.nfields = 1;
    }
    s.options = options;
    return as_run(vm, a, &s);
}

/* ---------------------------------------------------------------- */
/* String                                                           */

static A2Str *self_str(VM2 *vm, V2 self) {
    if (self.t == T_STR) return self.u.s;
    if (self.t == T_OBJ && self.u.o->kind == OK_PRIM && self.u.o->prim.t == T_STR) return self.u.o->prim.u.s;
    if (self.t == T_OBJ && self.u.o == vm->c_string->prototype) return vm->s_empty;
    a2_throw_error(vm, "TypeError", "Method String.prototype.toString was invoked on an incompatible object.", 1004);
    return vm->s_empty;
}
static V2 String_call(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; if (argc == 0) return v2_str(vm->s_empty); return v2_str(v2_tostr(vm, argv[0])); }
static V2 String_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = SELFO; if (o) { o->kind = OK_PRIM; o->prim = argc ? v2_str(v2_tostr(vm, argv[0])) : v2_str(vm->s_empty); } return v2_undef(); }
static V2 str_length(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return v2_int(a2_str_ulen(self_str(vm, self))); }
/* AVM2 String helpers (ported from avmplus StringObject.cpp) */
static double math_toint(double v) {   /* MathUtils::toInt */
    if (v != v) return 0;
    if (v == floor(v) || v == 1.0 / 0.0 || v == -1.0 / 0.0 || v == 0) return v;
    return v < 0 ? -floor(-v) : floor(v);
}
static int clamp_index(double v, int len) {   /* NativeObjectHelpers::ClampIndex */
    if (v < 0) { if (v + len < 0) return 0; return (int)(v + len); }
    if (v > len) return len;
    if (v != v) return 0;
    return (int)v;
}
#define INTCLAMP(ipos, dpos, length, nanv) { if ((dpos) <= -1) ipos = -1; else if ((dpos) >= (length)) ipos = (length); else if ((dpos) == (dpos)) ipos = (int)(dpos); else ipos = (nanv); }
static V2 str_charAt(VM2 *vm, V2 self, int argc, V2 *argv) { A2Str *s = self_str(vm, self); double d = (argc && argv[0].t != T_UNDEF) ? NUM(0) : 0; int n, ip; V2 r; u32 *u; n = a2_utf8_decode(s->s, s->len, &u); INTCLAMP(ip, d, n, 0); r = ((unsigned)ip < (unsigned)n) ? v2_str(a2_utf8_encode(vm, u + ip, 1)) : v2_str(vm->s_empty); free(u); return r; }
static V2 str_charCodeAt(VM2 *vm, V2 self, int argc, V2 *argv) { A2Str *s = self_str(vm, self); u32 *u; int n, ip; double d = (argc && argv[0].t != T_UNDEF) ? NUM(0) : 0; V2 r; n = a2_utf8_decode(s->s, s->len, &u); INTCLAMP(ip, d, n, 0); r = ((unsigned)ip < (unsigned)n) ? v2_int((s32)u[ip]) : v2_num(0.0 / 0.0); free(u); return r; }
static V2 str_fromCharCode(VM2 *vm, V2 self, int argc, V2 *argv) { u32 *u = (u32*)malloc((size_t)(argc + 1) * sizeof(u32)); int i; A2Str *r; (void)self; for (i = 0; i < argc; i++) u[i] = (u32)(v2_touint(vm, argv[i]) & 0xFFFF); r = a2_utf8_encode(vm, u, argc); free(u); return v2_str(r); }
static V2 str_concat(VM2 *vm, V2 self, int argc, V2 *argv) { A2Str *r = self_str(vm, self); int i; for (i = 0; i < argc; i++) r = a2_concat(vm, r, v2_tostr(vm, argv[i])); return v2_str(r); }
static int ufind(const u32 *h, int hn, const u32 *n, int nn, int from) { int i, j; if (from < 0) from = 0; for (i = from; i + nn <= hn; i++) { for (j = 0; j < nn; j++) if (h[i + j] != n[j]) break; if (j == nn) return i; } return -1; }
static V2 str_indexOf(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Str *s = self_str(vm, self), *q; u32 *a, *b; int na, nb, from, r; double d = argc > 1 ? NUM(1) : 0;
    if (argc == 0 || argv[0].t == T_UNDEF || argv[0].t == T_NULL) return v2_int(-1);
    q = v2_tostr(vm, argv[0]);
    na = a2_utf8_decode(s->s, s->len, &a); nb = a2_utf8_decode(q->s, q->len, &b);
    INTCLAMP(from, d, na, 0);
    if (from < 0) from = 0;
    if (nb == 0) r = from <= na ? from : na; else r = ufind(a, na, b, nb, from);
    free(a); free(b); return v2_int(r);
}
static V2 str_lastIndexOf(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Str *s = self_str(vm, self), *q; u32 *a, *b; int na, nb, from, i, j, r = -1; double d = argc > 1 ? NUM(1) : 0.0 / 0.0;
    if (argc == 0 || argv[0].t == T_UNDEF || argv[0].t == T_NULL) return v2_int(-1);
    q = v2_tostr(vm, argv[0]);
    na = a2_utf8_decode(s->s, s->len, &a); nb = a2_utf8_decode(q->s, q->len, &b);
    INTCLAMP(from, d, na, na);
    if (from > na - nb) from = na - nb;
    for (i = from; i >= 0; i--) { for (j = 0; j < nb; j++) if (a[i + j] != b[j]) break; if (j == nb) { r = i; break; } }
    free(a); free(b); return v2_int(r);
}
/* Ruffle's string representation (AvmStringRepr), kept for its __ruffle__::isDependent(): substrings
   depend on their owner's buffer, and a concatenation appends in place (a dependent string) when the
   left side ends where its owner's used part ends and the owner has room; otherwise it allocates a new
   buffer of 32 / 2n / n+1M units.  Strings without a record own an exactly sized buffer. */
typedef struct { A2Str *s, *owner; int off, cap, used; } SDep;
int g_a2_sdep_on;   /* only for code that uses __ruffle__ */
static SDep g_sdep[2048];
static SDep *sdep_slot(A2Str *s, int create) {
    unsigned h = (unsigned)(((size_t)s >> 4) * 2654435761u) & 2047u; int k;
    for (k = 0; k < 2048; k++) { SDep *d = &g_sdep[(h + (unsigned)k) & 2047u]; if (d->s == s) return d; if (!d->s) { if (!create) return NULL; d->s = s; return d; } }
    if (!create) return NULL;
    { SDep *d = &g_sdep[h]; memset(d, 0, sizeof(*d)); d->s = s; return d; }
}
static int sdep_units(A2Str *s, int *wide) { u32 *u; int n = a2_utf8_decode(s->s, s->len, &u), i, w = 0, m = n; for (i = 0; i < n; i++) { if (u[i] > 0xff) w = 1; if (u[i] > 0xffff) m++; } free(u); if (wide) *wide = w; return m; }
static void sdep_set(A2Str *s, A2Str *owner, int off, int cap, int used) { SDep *d = sdep_slot(s, 1); d->owner = owner; d->off = off; d->cap = cap; d->used = used; }
static A2Str *sdep_owner(A2Str *s, int *off) { SDep *d = sdep_slot(s, 0); if (d && d->owner && d->owner != s) { *off = d->off; return d->owner; } *off = 0; return s; }
static void sdep_owner_room(A2Str *o, int *cap, int *used) { SDep *d = sdep_slot(o, 0); if (d && !d->owner && d->cap) { *cap = d->cap; *used = d->used; } else { int n = sdep_units(o, NULL); *cap = n; *used = n; } }
/* r = s.substring(st, en) */
static void sdep_substring(VM2 *vm, A2Str *s, A2Str *r, int st, int en, const u32 *u) {
    int off; A2Str *o;
    if (!g_a2_sdep_on) return;
    if (en <= st || (en == st + 1 && u[st] < 128)) { if (r != s) sdep_set(r, NULL, 0, 0, 0); return; }   /* the empty string, shared single ASCII chars */
    o = sdep_owner(s, &off);
    if (o == r && off + st != 0) return;
    sdep_set(r, o, off + st, 0, 0);   /* (the whole string is the same interned string here: it becomes its own dependent) */
    (void)vm;
}
/* r = a + b */
void a2_sdep_concat(VM2 *vm, A2Str *a, A2Str *b, A2Str *r) {
    int wa, wb, la, lb, off, cap, used; A2Str *o;
    (void)vm;
    if (!g_a2_sdep_on || !a->len || !b->len) return;
    la = sdep_units(a, &wa); lb = sdep_units(b, &wb);
    o = sdep_owner(a, &off);
    sdep_owner_room(o, &cap, &used);
    if (wa == wb && off + la == used && cap - used >= lb && o != r) {
        SDep *od = sdep_slot(o, 1); od->owner = NULL; od->cap = cap; od->used = used + lb;
        sdep_set(r, o, off, 0, 0);
        return;
    }
    { int n = la + lb; sdep_set(r, NULL, 0, n < 32 ? 32 : n > 1024 * 1024 ? n + 1024 * 1024 : n * 2, n); }
}
static V2 g_isDependent(VM2 *vm, V2 self, int argc, V2 *argv) { SDep *d; (void)self; if (argc < 1 || argv[0].t != T_STR) return v2_bool(0); (void)vm; d = sdep_slot(argv[0].u.s, 0); return v2_bool(d && d->owner); }
static V2 str_sub_units(VM2 *vm, const u32 *u, int st, int en) { if (en <= st) return v2_str(vm->s_empty); return v2_str(a2_utf8_encode(vm, u + st, en - st)); }
static V2 str_slice(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Str *s = self_str(vm, self); u32 *u; int n, st, en; V2 r;
    n = a2_utf8_decode(s->s, s->len, &u);
    st = clamp_index(math_toint(argc > 0 ? NUM(0) : 0), n);
    en = clamp_index(math_toint(argc > 1 ? NUM(1) : 2147483647.0), n);
    if (en < st) en = st;
    r = str_sub_units(vm, u, st, en); free(u); return r;
}
static V2 str_substring(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Str *s = self_str(vm, self); u32 *u; int n; double st, en, t; V2 r;
    n = a2_utf8_decode(s->s, s->len, &u);
    st = math_toint(argc > 0 ? NUM(0) : 0); en = math_toint(argc > 1 ? NUM(1) : 2147483647.0);
    if (en < 0) en = 0; if (en >= n) en = n; if (st < 0) st = 0; if (st >= n) st = n; if (st > en) { t = st; st = en; en = t; }
    r = str_sub_units(vm, u, (int)st, (int)en); if (r.t == T_STR) sdep_substring(vm, s, r.u.s, (int)st, (int)en, u); free(u); return r;
}
static V2 str_substr(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Str *s = self_str(vm, self); u32 *u; int n, st, en; V2 r;
    n = a2_utf8_decode(s->s, s->len, &u);
    st = clamp_index(math_toint(argc > 0 ? NUM(0) : 0), n);
    en = clamp_index(math_toint(argc > 1 ? NUM(1) : 2147483647.0) + (double)st, n);
    if (en < st) en = st;
    r = str_sub_units(vm, u, st, en); if (r.t == T_STR) sdep_substring(vm, s, r.u.s, st, en, u); free(u); return r;
}
static V2 str_toLowerCase(VM2 *vm, V2 self, int argc, V2 *argv) { A2Str *s = self_str(vm, self); u32 *u; int n, i; V2 r; (void)argc; (void)argv; n = a2_utf8_decode(s->s, s->len, &u); for (i = 0; i < n; i++) if (u[i] < 0x10000) u[i] = cm_to_lower((u16)u[i]); r = v2_str(a2_utf8_encode(vm, u, n)); free(u); return r; }
static V2 str_toUpperCase(VM2 *vm, V2 self, int argc, V2 *argv) { A2Str *s = self_str(vm, self); u32 *u; int n, i; V2 r; (void)argc; (void)argv; n = a2_utf8_decode(s->s, s->len, &u); for (i = 0; i < n; i++) if (u[i] < 0x10000) u[i] = cm_to_upper((u16)u[i]); r = v2_str(a2_utf8_encode(vm, u, n)); free(u); return r; }
static V2 str_toString(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return v2_str(self_str(vm, self)); }
static V2 str_localeCompare(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Str *a = self_str(vm, self), *b; u32 *ua, *ub; int na, nb, i, len, r = 0;
    if (argc > 1) { char m[120]; sprintf(m, "Argument count mismatch on String/localeCompare(). Expected 0, got %d.", argc); a2_throw_error(vm, "ArgumentError", m, 1063); return v2_undef(); }   /* (other:* = undefined) */
    if (argc == 0 || argv[0].t == T_UNDEF || argv[0].t == T_NULL) { if (vm->swf_version < 11) return v2_int(a->len == 0 ? 1 : 0); }
    b = argc ? v2_tostr(vm, argv[0]) : a2_str(vm, "undefined");
    if (a == b) return v2_int(0);
    na = a2_utf8_decode(a->s, a->len, &ua); nb = a2_utf8_decode(b->s, b->len, &ub);
    len = na < nb ? na : nb;
    for (i = 0; i < len && !r; i++) r = (int)ua[i] - (int)ub[i];
    if (r == 0) r = (na - nb) < 0 ? -1 : (na - nb) > 0 ? 1 : 0;
    free(ua); free(ub); return v2_int(r);
}
/* split/match/replace/search implemented with the regexp engine in avm2_regexp.c */
extern V2 a2_str_split(VM2 *vm, A2Str *s, V2 sep, u32 limit);
extern V2 a2_str_match(VM2 *vm, A2Str *s, V2 re);
extern V2 a2_str_replace(VM2 *vm, A2Str *s, V2 pat, V2 rep);
extern V2 a2_str_search(VM2 *vm, A2Str *s, V2 re);
static V2 str_split(VM2 *vm, V2 self, int argc, V2 *argv) { return a2_str_split(vm, self_str(vm, self), ARG(0), (argc > 1 && argv[1].t != T_UNDEF) ? v2_touint(vm, argv[1]) : 0xffffffffUL); }
static V2 str_match(VM2 *vm, V2 self, int argc, V2 *argv) { return a2_str_match(vm, self_str(vm, self), ARG(0)); }
static V2 str_replace(VM2 *vm, V2 self, int argc, V2 *argv) { return a2_str_replace(vm, self_str(vm, self), ARG(0), ARG(1)); }
static V2 str_search(VM2 *vm, V2 self, int argc, V2 *argv) { return a2_str_search(vm, self_str(vm, self), ARG(0)); }

/* ---------------------------------------------------------------- */
/* Number / int / uint / Boolean                                    */

static const char *g_num_proto_cls = "Number";   /* class named by the incompatible-receiver error */
static double self_num(VM2 *vm, V2 self) {
    if (v2_is_num(self)) return v2_tonum(vm, self);
    if (self.t == T_OBJ && self.u.o->kind == OK_PRIM && v2_is_num(self.u.o->prim)) return v2_tonum(vm, self.u.o->prim);
    if (self.t == T_OBJ && (self.u.o == vm->c_number->prototype || self.u.o == vm->c_int->prototype || self.u.o == vm->c_uint->prototype)) return 0;
    { char b[120]; sprintf(b, "Method %s.prototype.toString was invoked on an incompatible object.", g_num_proto_cls); a2_throw_error(vm, "TypeError", b, 1004); }
    return 0;
}
static V2 Number_call(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; return v2_num(argc ? v2_tonum(vm, argv[0]) : 0); }
static V2 int_call(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; return v2_int(argc ? v2_toint(vm, argv[0]) : 0); }
static V2 uint_call(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; return v2_uint(argc ? v2_touint(vm, argv[0]) : 0); }
static V2 Boolean_call(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; return v2_bool(argc ? v2_tobool(vm, argv[0]) : 0); }
static V2 Number_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = SELFO; if (o) { o->kind = OK_PRIM; o->prim = v2_num(argc ? v2_tonum(vm, argv[0]) : 0); } return v2_undef(); }
static V2 int_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = SELFO; if (o) { o->kind = OK_PRIM; o->prim = v2_int(argc ? v2_toint(vm, argv[0]) : 0); } return v2_undef(); }
static V2 uint_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = SELFO; if (o) { o->kind = OK_PRIM; o->prim = v2_uint(argc ? v2_touint(vm, argv[0]) : 0); } return v2_undef(); }
static V2 Boolean_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = SELFO; if (o) { o->kind = OK_PRIM; o->prim = v2_bool(argc ? v2_tobool(vm, argv[0]) : 0); } return v2_undef(); }
/* MathUtils::convertDoubleToStringRadix: integer part only, digits from floating division
 * (which produces Flash's odd digits for huge values) */
static void radix_str(double d, int radix, char *buf) {
    char tmp[1200]; char *end = tmp + sizeof(tmp) - 1, *src = end - 1; int neg = d < 0;
    if (d != d) { strcpy(buf, "NaN"); return; }
    if (radix == 10) { v2_num_to_cstr(d, buf); return; }
    if (d > 1.7976931348623157e308) { strcpy(buf, "Infinity"); return; }
    if (d < -1.7976931348623157e308) { strcpy(buf, "-Infinity"); return; }
    if (neg) d = -d;
    if (d < 1.0) neg = 0;   /* (-0.5).toString(2) is "0" */
    *end = 0;
    /* avmplus convertDoubleToStringRadix: digits from double floor division (huge values give Flash's
       odd digits, out-of-range ones wrap to Latin-1 characters, written here as UTF-8) */
    if (d < 1.0) *src-- = '0';
    else {
        volatile double u = floor(d);
        while (u != 0 && src > tmp + 4) {
            volatile double j = u, q, pr; int c;
            q = floor(u / (double)radix); u = q;
            pr = u * (double)radix; j = j - pr;
            c = (unsigned char)(char)(j < 10 ? (int)j + '0' : (int)j + ('a' - 10));
            if (c >= 0x80) { *src-- = (char)(0x80 | (c & 0x3f)); *src-- = (char)(0xc0 | (c >> 6)); }
            else *src-- = (char)c;
        }
    }
    if (neg) *src-- = '-';
    memcpy(buf, src + 1, (size_t)(end - src));
}
static V2 num_toString(VM2 *vm, V2 self, int argc, V2 *argv) {
    char buf[1300]; int radix = (argc > 0 && argv[0].t != T_UNDEF) ? INT(0) : 10; double d = self_num(vm, self);
    if (radix < 2 || radix > 36) { a2_throw_error(vm, "RangeError", "The radix argument must be between 2 and 36", 1003); return v2_undef(); }
    if (self.t == T_INT && radix != 10) { s32 i = self.u.i; char t[40]; int n = 0, neg = i < 0; u32 u = neg ? (u32)(-(i + 1)) + 1u : (u32)i; if (!u) t[n++] = '0'; while (u) { u32 r = u % (u32)radix; t[n++] = (char)(r < 10 ? '0' + r : 'a' + r - 10); u /= (u32)radix; } { int o = 0; if (neg) buf[o++] = '-'; while (n) buf[o++] = t[--n]; buf[o] = 0; } return v2_cstr(vm, buf); }
    radix_str(d, radix, buf); return v2_cstr(vm, buf);
}
static int num_receiver_is(VM2 *vm, V2 self, int is_uint) {
    double v; V2 p = self;
    if (p.t == T_OBJ && p.u.o->kind == OK_PRIM) p = p.u.o->prim;
    if (!v2_is_num(p)) return 1;     /* self_num reports the other cases */
    v = v2_tonum(vm, p);
    if (!is_uint && v != floor(v)) return 0;
    return is_uint ? 1 : (v >= -2147483648.0 && v <= 2147483647.0);   /* avmplus: uint accepts any number */
}
static V2 int_proto_toString(VM2 *vm, V2 self, int argc, V2 *argv) { V2 r; if (!num_receiver_is(vm, self, 0)) { a2_throw_error(vm, "TypeError", "Method int.prototype.toString was invoked on an incompatible object.", 1004); return v2_undef(); } g_num_proto_cls = "int"; r = num_toString(vm, self, argc, argv); g_num_proto_cls = "Number"; return r; }
static V2 uint_proto_toString(VM2 *vm, V2 self, int argc, V2 *argv) { V2 r; if (!num_receiver_is(vm, self, 1)) { a2_throw_error(vm, "TypeError", "Method uint.prototype.toString was invoked on an incompatible object.", 1004); return v2_undef(); } g_num_proto_cls = "uint"; r = num_toString(vm, self, argc, argv); g_num_proto_cls = "Number"; return r; }
/* X.prototype.valueOf: the prototype itself gives the default, primitives of the type themselves */
static V2 proto_valueOf_k(VM2 *vm, V2 self, int k) {
    static const char *nm[] = { "String", "Boolean", "Number", "int", "uint" };
    A2Class *cls = k == 0 ? vm->c_string : k == 1 ? vm->c_boolean : k == 2 ? vm->c_number : k == 3 ? vm->c_int : vm->c_uint;
    V2 p = self; char b[120];
    if (self.t == T_OBJ && self.u.o == cls->prototype) return k == 0 ? v2_str(vm->s_empty) : k == 1 ? v2_bool(0) : v2_int(0);
    if (p.t == T_OBJ && p.u.o->kind == OK_PRIM) p = p.u.o->prim;
    switch (k) {
    case 0: if (p.t == T_STR) return p; break;
    case 1: if (p.t == T_BOOL) return p; break;
    case 2: if (v2_is_num(p)) return p; break;
    case 3: if (v2_is_num(p)) { double d = v2_tonum(vm, p); if (d == floor(d) && d >= -2147483648.0 && d <= 2147483647.0 && !(d == 0 && 1 / d < 0)) return p; } break;
    case 4: if (v2_is_num(p)) { double d = v2_tonum(vm, p); if (d == floor(d) && d >= 0 && d <= 4294967295.0 && !(d == 0 && 1 / d < 0)) return p; } break;
    }
    sprintf(b, "Method %s.prototype.valueOf was invoked on an incompatible object.", nm[k]);
    a2_throw_error(vm, "TypeError", b, 1004);
    return v2_undef();
}
static V2 pv_string(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return proto_valueOf_k(vm, self, 0); }
static V2 pv_boolean(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return proto_valueOf_k(vm, self, 1); }
static V2 pv_number(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return proto_valueOf_k(vm, self, 2); }
static V2 pv_int(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return proto_valueOf_k(vm, self, 3); }
static V2 pv_uint(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return proto_valueOf_k(vm, self, 4); }
static V2 num_valueOf(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; if (self.t == T_OBJ && self.u.o->kind == OK_PRIM) return self.u.o->prim; if (self.t == T_OBJ && self.u.o->kind != OK_PRIM) { if (self.u.o == vm->c_boolean->prototype) return v2_bool(0); if (self.u.o == vm->c_number->prototype || self.u.o == vm->c_int->prototype || self.u.o == vm->c_uint->prototype) return v2_int(0); a2_throw_error(vm, "TypeError", "Method valueOf was invoked on an incompatible object.", 1004); return v2_undef(); } return self; }
#define NUM_RANGE_MSG "Number.toPrecision has a range of 1 to 21. Number.toFixed and Number.toExponential have a range of 0 to 20. Specified value is not within expected range."
/* the range error comes from Number's AS3 glue: Number$/_convert, called by
 * Number's method (and by int's / uint's, which forward to Number's) */
static void num_range_error(VM2 *vm, const char *meth) {
    static char fr[200]; A2Method *m = vm->depth >= 1 ? vm->cs[vm->depth] : NULL;
    if (m && m->owner && m->owner != vm->c_number) sprintf(fr, "Number$/_convert()\n\tat Number/http://adobe.com/AS3/2006/builtin::%s", meth);
    else strcpy(fr, "Number$/_convert");
    vm->throw_frame = fr;
    a2_throw_error(vm, "RangeError", NUM_RANGE_MSG, 1002);
    vm->throw_frame = NULL;
}
/* int's and uint's toFixed/toPrecision/toExponential call Number's: its frame sits under theirs */
static int num_frame(VM2 *vm, const char *name) {
    A2Method *m = vm->depth >= 1 ? vm->cs[vm->depth] : NULL; A2Trait *t;
    if (!m || !m->owner || m->owner == vm->c_number || vm->depth > 900) return 0;
    t = a2_find_trait_any(&vm->c_number->itraits, a2_str(vm, name));
    if (!t || t->kind != TK_METHOD || !t->method) return 0;
    vm->depth++; vm->cs[vm->depth] = t->method; vm->cs_cls[vm->depth] = NULL; vm->cs_file[vm->depth] = NULL;
    return 1;
}
static int num_digits_arg(VM2 *vm, int argc, V2 *argv) { if (argc == 0 || argv[0].t == T_UNDEF || argv[0].t == T_NULL) return 0; return INT(0); }
static V2 num_toFixed_body(VM2 *vm, V2 self, int argc, V2 *argv);
static V2 num_toFixed(VM2 *vm, V2 self, int argc, V2 *argv) { int f = num_frame(vm, "toFixed"); V2 r = num_toFixed_body(vm, self, argc, argv); if (f) vm->depth--; return r; }
static V2 num_toFixed_body(VM2 *vm, V2 self, int argc, V2 *argv) {
    char buf[480]; int digits = num_digits_arg(vm, argc, argv); double d = self_num(vm, self);
    if (digits < 0 || digits > 20) { num_range_error(vm, "toFixed"); return v2_undef(); }
    if (d == d && fabs(d) >= 1e17 && fabs(d) < 1e21) {
        /* avmplus generates at most 17 integer digits plus the fraction digits; the rest are zeros */
        char e[64]; int intd, sig, i, o = 0; const char *m;
        sprintf(e, "%.20e", fabs(d)); intd = atoi(strchr(e, 'e') + 1) + 1;
        if (intd > 17) {
            sig = 17 + digits; sprintf(e, "%.*e", sig - 1, fabs(d));
            if (atoi(strchr(e, 'e') + 1) + 1 > intd) intd++;   /* rounding carried into a new digit */
            if (d < 0) buf[o++] = '-';
            for (m = e, i = 0; i < intd; i++) { while (*m == '.') m++; buf[o++] = (i < sig && *m != 'e') ? *m++ : '0'; }
            if (digits) { buf[o++] = '.'; for (i = 0; i < digits; i++) buf[o++] = '0'; }
            buf[o] = 0; return v2_cstr(vm, buf);
        }
    }
    if (vm->swf_version <= 14 && d == d && fabs(d) < pow(10.0, -digits)) {
        /* SWF <= 14 (fixed in avmplus for SWF 15): a value below one unit of the last place rounds on its
           first significant digit, and toFixed(0) keeps a trailing point */
        char e[64]; int up = 0, i, o = 0;
        if (d != 0) { sprintf(e, "%.20e", fabs(d)); up = e[0] >= '5'; }
        if (d < 0) buf[o++] = '-';
        if (!digits) { buf[o++] = up ? '1' : '0'; buf[o++] = '.'; }
        else { buf[o++] = '0'; buf[o++] = '.'; for (i = 0; i < digits; i++) buf[o++] = (up && i == digits - 1) ? '1' : '0'; }
        buf[o] = 0; return v2_cstr(vm, buf);
    }
    return v2_cstr(vm, a2_format_number(d, 1, digits, buf));
}
static V2 num_toPrecision_body(VM2 *vm, V2 self, int argc, V2 *argv);
static V2 num_toPrecision(VM2 *vm, V2 self, int argc, V2 *argv) { int f = num_frame(vm, "toPrecision"); V2 r = num_toPrecision_body(vm, self, argc, argv); if (f) vm->depth--; return r; }
static V2 num_toPrecision_body(VM2 *vm, V2 self, int argc, V2 *argv) {
    char buf[480]; int prec; double d = self_num(vm, self);
    if (argc == 0 || argv[0].t == T_UNDEF || argv[0].t == T_NULL) { v2_num_to_cstr(d, buf); return v2_cstr(vm, buf); }
    prec = INT(0);
    if (prec < 1 || prec > 21) { num_range_error(vm, "toPrecision"); return v2_undef(); }
    if (d == 0) { sprintf(buf, "0.%.*s", prec, "000000000000000000000"); return v2_cstr(vm, buf); }   /* Flash quirk: one extra zero */
    return v2_cstr(vm, a2_format_number(d, 2, prec, buf));
}
static V2 num_toExponential_body(VM2 *vm, V2 self, int argc, V2 *argv);
static V2 num_toExponential(VM2 *vm, V2 self, int argc, V2 *argv) { int f = num_frame(vm, "toExponential"); V2 r = num_toExponential_body(vm, self, argc, argv); if (f) vm->depth--; return r; }
static V2 num_toExponential_body(VM2 *vm, V2 self, int argc, V2 *argv) {
    char buf[480]; int digits = num_digits_arg(vm, argc, argv); double d = self_num(vm, self);
    if (digits < 0 || digits > 20) { num_range_error(vm, "toExponential"); return v2_undef(); }
    if (d == 0) {   /* Flash Player quirk */
        if (digits == 0) return v2_cstr(vm, "1e-15");
        sprintf(buf, "0.%.*se-16", digits, "00000000000000000000"); return v2_cstr(vm, buf);
    }
    return v2_cstr(vm, a2_format_number(d, 3, digits, buf));
}
static V2 bool_toString(VM2 *vm, V2 self, int argc, V2 *argv) {
    int b; (void)argc; (void)argv;
    if (self.t == T_BOOL) b = self.u.b;
    else if (self.t == T_OBJ && self.u.o->kind == OK_PRIM && self.u.o->prim.t == T_BOOL) b = self.u.o->prim.u.b;
    else if (self.t == T_OBJ && self.u.o == vm->c_boolean->prototype) b = 0;
    else { a2_throw_error(vm, "TypeError", "Method Boolean.prototype.toString was invoked on an incompatible object.", 1004); return v2_undef(); }
    return v2_cstr(vm, b ? "true" : "false");
}

/* ---------------------------------------------------------------- */
/* Math                                                             */

static V2 m_abs(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; if (ARG(0).t == T_INT && ARG(0).u.i != (s32)0x80000000) return v2_int(ARG(0).u.i < 0 ? -ARG(0).u.i : ARG(0).u.i); return v2_num(fabs(NUM(0))); }
static V2 m_acos(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; return v2_num(acos(NUM(0))); }
static V2 m_asin(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; return v2_num(asin(NUM(0))); }
static V2 m_atan(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; return v2_num(atan(NUM(0))); }
static V2 m_atan2(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; return v2_num(atan2(NUM(0), NUM(1))); }
static V2 m_ceil(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; return v2_num(ceil(NUM(0))); }
static V2 m_cos(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; return v2_num(cos(NUM(0))); }
static V2 m_exp(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; return v2_num(exp(NUM(0))); }
static V2 m_floor(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; if (ARG(0).t == T_INT) return ARG(0); return v2_num(floor(NUM(0))); }
static V2 m_log(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; return v2_num(log(NUM(0))); }
static V2 m_max(VM2 *vm, V2 self, int argc, V2 *argv) { double r = -1e308 * 10; int i; (void)self; for (i = 0; i < argc; i++) { double d = v2_tonum(vm, argv[i]); if (d != d) return v2_num(d); if (d > r || (d == 0 && r == 0 && !(d == 0 && 1.0 / d < 0))) r = d; } return v2_num(r); }
static V2 m_min(VM2 *vm, V2 self, int argc, V2 *argv) { double r = 1e308 * 10; int i; (void)self; for (i = 0; i < argc; i++) { double d = v2_tonum(vm, argv[i]); if (d != d) return v2_num(d); if (d < r || (d == 0 && r == 0 && (d == 0 && 1.0 / d < 0))) r = d; } return v2_num(r); }
static V2 m_pow(VM2 *vm, V2 self, int argc, V2 *argv) { double a = NUM(0), b = NUM(1); (void)self; if (b != b) return v2_num(b); if (b == 0) return v2_num(1); if ((a == 1 || a == -1) && (b > 1e308 || b < -1e308)) return v2_num(0.0 / 0.0); if (a < -1e308 && b < 0 && b > -9.2e18 && b == floor(b) && fmod(b, 2.0) == 0) return v2_num(-0.0); return v2_num(pow(a, b)); }
/* Flash Player's generator (avmplus MathUtils: RandomFastNext + RandomPureHasher) */
static s32 rng_sar(s32 v, int n) { return v < 0 ? (s32)~(~(u32)v >> n) : (s32)((u32)v >> n); }
static s32 rng_pure_hasher(s32 iSeed) {
    s32 r;
    iSeed = (s32)((u32)(((u32)iSeed << 13) ^ (u32)iSeed) - (u32)rng_sar(iSeed, 21));
    r = (s32)(((u32)iSeed * ((u32)iSeed * (u32)iSeed * 15731u + 789221u) + 1376312589u) & 0x7FFFFFFFu);
    r = (s32)((u32)r + (u32)iSeed);
    r = (s32)((u32)(((u32)r << 13) ^ (u32)r) - (u32)rng_sar(r, 21));
    return r;
}
static V2 m_random(VM2 *vm, V2 self, int argc, V2 *argv) {
    s32 n; UNUSED;
    if (vm->rng == 0) {   /* the current time in microseconds, truncated to 32 bits */
        extern double plat_now_ms(void); double us = floor(plat_now_ms() * 1000.0);
        vm->rng = (u32)(u64)(s64)us;
        if (vm->rng == 0) vm->rng = 1;
    }
    vm->rng = (vm->rng & 1) ? (vm->rng >> 1) ^ 0x48000000u : (vm->rng >> 1);
    n = rng_pure_hasher((s32)(vm->rng * 71u)) & 0x7FFFFFFF;
    return v2_num((double)n / 2147483648.0);
}
static V2 m_round(VM2 *vm, V2 self, int argc, V2 *argv) { double d = NUM(0); (void)self; if (ARG(0).t == T_INT) return ARG(0); return v2_num(floor(d + 0.5)); }
static V2 m_sin(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; return v2_num(sin(NUM(0))); }
static V2 m_sqrt(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; return v2_num(sqrt(NUM(0))); }
static V2 m_tan(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; return v2_num(tan(NUM(0))); }

/* ---------------------------------------------------------------- */
/* Error                                                            */

static V2 Error_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = SELFO; if (!o) return v2_undef();
    o->kind = OK_ERROR;
    a2_set_s(vm, self, "message", argc ? v2_str(v2_tostr(vm, argv[0])) : v2_str(vm->s_empty));
    a2_set_s(vm, self, "errorID", v2_int(argc > 1 ? v2_toint(vm, argv[1]) : 0));
    { extern A2Str *a2_stack_trace(VM2 *vm); a2_dyn_set_hidden(vm, o, a2_str(vm, "__stackTrace"), v2_str(a2_stack_trace(vm))); }
    return v2_undef();
}
static V2 Error_call(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *f = self.t == T_OBJ ? self.u.o : NULL; return a2_construct_class(vm, f && f->kind == OK_CLASS ? f->as_class : vm->c_error, argc, argv); }
static V2 err_toString(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 name = a2_get_s(vm, self, "name"), msg = a2_get_s(vm, self, "message"); A2Str *n = v2_tostr(vm, name), *m = v2_tostr(vm, msg); (void)argc; (void)argv;
    if (m->len == 0) return v2_str(n);
    return v2_str(a2_concat(vm, a2_concat(vm, n, a2_str(vm, ": ")), m));
}
static V2 DRMManagerError_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { Error_ctor(vm, self, argc, argv); a2_set_s(vm, self, "subErrorID", v2_int(argc > 2 ? v2_toint(vm, argv[2]) : 0)); return v2_undef(); }
static V2 drm_toString(VM2 *vm, V2 self, int argc, V2 *argv) {
    char b[700]; V2 msg = a2_get_s(vm, self, "message"), id = a2_get_s(vm, self, "errorID"), sub = a2_get_s(vm, self, "subErrorID"); (void)argc; (void)argv;
    sprintf(b, "DRMManagerError: '%.500s', error ID:'%d', subErrorID:'%d'", v2_tostr(vm, msg)->s, v2_toint(vm, id), v2_toint(vm, sub)); return v2_cstr(vm, b);
}
static V2 err_getStackTrace(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Prop *p = self.t == T_OBJ ? a2_dyn_find(self.u.o, a2_str(vm, "__stackTrace")) : NULL; V2 head; extern int g_player_release; (void)argc; (void)argv;
    if (g_player_release && vm->player->movie && vm->player->movie->version < 18) return v2_null();   /* a release player before SWF 18 has no stack traces */
    if (!p || p->v.t != T_STR) return v2_null();
    head = a2_call_s(vm, self, "toString", 0, NULL); if (vm->throwing) return v2_undef();
    return v2_str(a2_concat(vm, v2_tostr(vm, head), p->v.u.s));
}
static V2 err_errorID_get(VM2 *vm, V2 self, int argc, V2 *argv) { V2 v; (void)argc; (void)argv; v = a2_get_s(vm, self, "__errorID"); return v.t == T_UNDEF ? v2_int(0) : v; }
extern const char *a2_error_message(int id);
static V2 err_getErrorMessage(VM2 *vm, V2 self, int argc, V2 *argv) { char b[1200]; int id = INT(0); const char *m = a2_error_message(id); (void)self; if (m) sprintf(b, "Error #%d: %s", id, m); else sprintf(b, "Error #%d", id); return v2_cstr(vm, b); }
/* Flash's throwError converts the arguments in a replace() callback: Function/<anonymous> -> String$/_replace -> toString */
static V2 err_arg_replace(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; (void)argc; return v2_str(v2_tostr(vm, argv[0])); }
static V2 err_arg_anon(VM2 *vm, V2 self, int argc, V2 *argv) {
    static A2Method rm; (void)argc;
    rm.native = err_arg_replace; rm.dbg_name = a2_str(vm, "String$/_replace");
    return a2_call_method(vm, &rm, NULL, self, 1, argv);
}
static A2Str *err_arg_str(VM2 *vm, V2 v) {
    static A2Method am; V2 r;
    if (v.t != T_OBJ) return v2_tostr(vm, v);
    am.native = err_arg_anon; am.dbg_name = a2_str(vm, "Function/<anonymous>");
    r = a2_call_method(vm, &am, NULL, v2_null(), 1, &v);
    return r.t == T_STR ? r.u.s : vm->s_empty;
}
/* Error.throwError(type, index, ...rest): %1..%9 in the message are replaced by the rest args (missing -> "") */
static V2 err_format_message(VM2 *vm, int id, int argc, V2 *argv) {
    char head[40]; const char *m = a2_error_message(id); char *buf; size_t cap, n = 0; const char *p; A2Str *r;
    sprintf(head, "Error #%d", id);
    if (!m) return v2_cstr(vm, head);
    cap = strlen(m) + 64; buf = (char*)malloc(cap); strcpy(buf, head); n = strlen(buf); buf[n++] = ':'; buf[n++] = ' ';
    for (p = m; *p; p++) {
        if (*p == '%' && p[1] >= '0' && p[1] <= '9') {   /* only %1..%6 are substituted (avmplus Error.throwError) */
            int k = p[1] - '1'; A2Str *s = (k >= 0 && k < 6 && k < argc) ? err_arg_str(vm, argv[k]) : vm->s_empty; p++;
            if (vm->throwing) { free(buf); return v2_undef(); }
            if (n + s->len + 2 > cap) { cap = (n + s->len + 2) * 2; buf = (char*)realloc(buf, cap); }
            memcpy(buf + n, s->s, s->len); n += s->len; continue;
        }
        if (n + 2 > cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
        buf[n++] = *p;
    }
    buf[n] = 0; r = a2_intern(vm, buf, n); free(buf); return v2_str(r);
}
static V2 err_throwError(VM2 *vm, V2 self, int argc, V2 *argv) { V2 args[2]; (void)self; args[0] = err_format_message(vm, INT(1), argc > 2 ? argc - 2 : 0, argv + 2); if (vm->throwing) return v2_undef(); args[1] = v2_num(v2_tonum(vm, ARG(1))); a2_throw(vm, a2_construct(vm, ARG(0), 2, args)); return v2_undef(); }

static A2Class *make_error_class(VM2 *vm, const char *ns, const char *name, A2Class *super) {
    A2Class *c = a2_class_new(vm, ns, name, super, OK_ERROR);
    c->native_ctor = Error_ctor; c->native_call = Error_call; c->is_dynamic = 1;
    a2_dyn_set_hidden(vm, c->prototype, vm->s_name, v2_cstr(vm, name));
    a2_add_const(vm, c, "length", v2_int(1));
    return c;
}

/* ---------------------------------------------------------------- */
/* Vector                                                           */

A2Class *a2_vector_class_for(VM2 *vm, A2Class *t);
static A2Class *vector_class_for(VM2 *vm, A2Class *t) { return a2_vector_class_for(vm, t); }
A2Class *a2_vector_class_for(VM2 *vm, A2Class *t) {
    /* one class per element type, named "Vector.<T>" */
    char *buf; A2Str *name, *tn = t ? a2_class_name(vm, t, 1) : NULL; A2Class *c;
    buf = (char*)malloc((tn ? tn->len : 1) + 16);
    if (!buf) return vm->c_vector;
    sprintf(buf, "Vector.<%s>", tn ? tn->s : "*");
    name = a2_str(vm, buf);
    c = a2_class_find(vm, a2_str(vm, "__AS3__.vec"), name);
    if (c) { free(buf); return c; }
    /* object-typed vectors derive from Vector.<*> (avmplus ObjectVector), numeric ones from Vector */
    { A2Class *base = (!t || t == vm->c_int || t == vm->c_uint || t == vm->c_number) ? vm->c_vector : a2_vector_class_for(vm, NULL);
      c = a2_class_new(vm, "__AS3__.vec", buf, base, OK_VECTOR); }
    free(buf);
    c->native_ctor = vm->c_vector->native_ctor; c->native_call = vm->c_vector->native_call; c->flags |= CF_FINAL;
    c->script = -1;
    a2_dyn_set(vm, c->obj, a2_str(vm, "__elemtype"), t ? v2_obj(t->obj) : v2_null());
    return c;
}
A2Class *a2_vec_elem_type(VM2 *vm, A2Class *c);
static A2Class *vec_elem_type(VM2 *vm, A2Class *c) { return a2_vec_elem_type(vm, c); }
A2Class *a2_vec_elem_type(VM2 *vm, A2Class *c) { A2Prop *p = a2_dyn_find(c->obj, a2_str(vm, "__elemtype")); return p && p->v.t == T_OBJ ? p->v.u.o->as_class : NULL; }
V2 a2_vector_apply(VM2 *vm, V2 base, int argc, V2 *argv) {
    A2Class *t = (argc > 0 && argv[0].t == T_OBJ && argv[0].u.o->kind == OK_CLASS) ? argv[0].u.o->as_class : NULL;
    (void)base;
    if (argc != 1) { char b[200]; sprintf(b, "Incorrect number of type parameters for Vector. Expected 1, got %d.", argc); a2_throw_error(vm, "TypeError", b, 1128); return v2_undef(); }
    return v2_obj(vector_class_for(vm, t)->obj);
}
static V2 vec_default(VM2 *vm, A2Obj *a);
static V2 Vector_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = SELFO; int n; if (!o) return v2_undef();
    o->kind = OK_VECTOR; o->vec_type = o->cls ? vec_elem_type(vm, o->cls) : NULL;
    n = argc ? INT(0) : 0;
    if (n > 0) { int i; if (n > (1 << 26)) n = 1 << 26; a2_arr_setlen(vm, o, n); for (i = 0; i < n; i++) o->arr[i] = vec_default(vm, o); }
    o->vec_fixed = argc > 1 ? BOOL(1) : 0;
    return v2_undef();
}
static V2 Vector_call(VM2 *vm, V2 self, int argc, V2 *argv) {
    /* Vector.<T>(x): convert array/vector */
    A2Class *c = (self.t == T_OBJ && self.u.o->kind == OK_CLASS) ? self.u.o->as_class : vm->c_vector; A2Obj *r; V2 src = ARG(0);
    if (src.t == T_OBJ && src.u.o->cls == c) return src;
    r = a2_new(vm, c); r->kind = OK_VECTOR; r->vec_type = vec_elem_type(vm, c);
    if (src.t == T_OBJ && src.u.o->kind == OK_BYTEARRAY) { int i; for (i = 0; i < src.u.o->blen; i++) a2_arr_push(vm, r, a2_vec_coerce(vm, v2_int(src.u.o->bytes[i]), r->vec_type)); }
    else if (src.t == T_OBJ) {
        /* any object: length property, then indexed elements through the property protocol */
        V2 lv = a2_get_s(vm, src, "length"); u32 len = v2_touint(vm, lv), i; if (vm->throwing) return v2_undef();
        for (i = 0; i < len; i++) { char nb[16]; V2 ev; sprintf(nb, "%u", i); ev = a2_get_s(vm, src, nb); if (vm->throwing) return v2_undef(); a2_arr_push(vm, r, a2_vec_coerce(vm, ev, r->vec_type)); if (vm->throwing) return v2_undef(); }
    }
    else { a2_throw_coerce_error(vm, src, c); return v2_undef(); }
    return v2_obj(r);
}
static V2 vec_fixed_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *a = self_arr(self); UNUSED; return v2_bool(a ? a->vec_fixed : 0); }
static V2 vec_fixed_set(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *a = self_arr(self); if (a) a->vec_fixed = BOOL(0); return v2_undef(); }
static int vec_check_fixed(VM2 *vm, A2Obj *a) { if (a->vec_fixed) { a2_throw_error(vm, "RangeError", "Cannot change the length of a fixed Vector.", 1126); return 0; } return 1; }
static void vec_range_error(VM2 *vm, double idx, int len) { char b[120]; char nb[64]; v2_num_to_cstr(idx, nb); sprintf(b, "The index %s is out of range %d.", nb, len); a2_throw_error(vm, "RangeError", b, 1125); }
static int vec_numeric(VM2 *vm, A2Obj *a) { return a->vec_type == vm->c_int || a->vec_type == vm->c_uint || a->vec_type == vm->c_number; }
/* element default: 0 for int/uint/Number vectors (Number too!), null for every object type */
static V2 vec_default(VM2 *vm, A2Obj *a) { return vec_numeric(vm, a) ? (a->vec_type == vm->c_number ? v2_num(0) : a->vec_type == vm->c_uint ? v2_uint(0) : v2_int(0)) : v2_null(); }
static V2 vec_length_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *a = self_arr(self); u32 n; int i, old; if (!a) return v2_undef();
    if (!vec_check_fixed(vm, a)) return v2_undef();
    n = v2_touint(vm, ARG(0)); if (n > (1u << 26)) n = 1u << 26;
    old = a->alen; a2_arr_setlen(vm, a, (int)n); for (i = old; i < (int)n; i++) a->arr[i] = vec_default(vm, a); return v2_undef();
}
/* clamp() from VectorImpl.as */
static u32 vec_clamp(double val, u32 len) {
    if (val < 0.0) { if (val + len < 0.0) return 0; return (u32)(val + len); }
    if (val > len) return len;
    if (val != val) return 0;
    return (u32)val;
}
static V2 vec_push(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *a = self_arr(self); int i; if (!a) return v2_undef(); if (!vec_check_fixed(vm, a)) return v2_undef(); for (i = 0; i < argc; i++) { V2 v = a2_vec_coerce(vm, argv[i], a->vec_type); if (vm->throwing) return v2_undef(); a2_arr_push(vm, a, v); } return v2_uint((u32)a->alen); }
static V2 vec_pop(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *a = self_arr(self); V2 r; UNUSED; if (!a) return v2_undef(); if (!vec_check_fixed(vm, a)) return v2_undef(); if (!a->alen) return vec_numeric(vm, a) ? vec_default(vm, a) : v2_undef(); r = a->arr[a->alen - 1]; a->alen--; V2_NORM(r); return r; }
static V2 vec_shift(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *a = self_arr(self); V2 r; UNUSED; if (!a) return v2_undef(); if (!vec_check_fixed(vm, a)) return v2_undef(); if (!a->alen) return vec_numeric(vm, a) ? vec_default(vm, a) : v2_undef(); r = a->arr[0]; memmove(a->arr, a->arr + 1, (size_t)(a->alen - 1) * sizeof(V2)); a->alen--; V2_NORM(r); return r; }
static V2 vec_unshift(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *a = self_arr(self); int i, n; if (!a) return v2_undef(); if (!vec_check_fixed(vm, a)) return v2_undef(); n = a->alen; a2_arr_fill(vm, a, n + argc); memmove(a->arr + argc, a->arr, (size_t)n * sizeof(V2)); for (i = 0; i < argc; i++) a->arr[i] = a2_vec_coerce(vm, argv[i], a->vec_type); return v2_uint((u32)a->alen); }
static A2Obj *vec_new_like(VM2 *vm, A2Obj *a) { A2Obj *r = a2_new(vm, a->cls); r->kind = OK_VECTOR; r->vec_type = a->vec_type; return r; }
static V2 vec_slice(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *a = self_arr(self), *r; u32 first, limit, i; if (!a) return v2_undef();
    first = vec_clamp(argc > 0 ? NUM(0) : 0, (u32)a->alen); limit = vec_clamp(argc > 1 ? NUM(1) : 2147483647.0, (u32)a->alen);
    if (limit < first) limit = first;
    r = vec_new_like(vm, a);
    for (i = first; i < limit; i++) a2_arr_push(vm, r, a->arr[i]);
    return v2_obj(r);
}
static V2 vec_splice(VM2 *vm, V2 self, int argc, V2 *argv) {
    /* note: splice does not check `fixed` (an avmplus bug that Flash keeps) */
    A2Obj *a = self_arr(self), *r; u32 first, delcnt, i; int ins, newlen; if (!a) return v2_undef();
    first = vec_clamp(argc > 0 ? NUM(0) : 0, (u32)a->alen);
    delcnt = vec_clamp(argc > 1 ? NUM(1) : 0, (u32)a->alen - first);
    r = vec_new_like(vm, a);
    for (i = 0; i < delcnt; i++) a2_arr_push(vm, r, a->arr[first + i]);
    ins = argc > 2 ? argc - 2 : 0;
    newlen = a->alen - (int)delcnt + ins;
    if (ins > (int)delcnt) a2_arr_fill(vm, a, newlen);
    memmove(a->arr + first + ins, a->arr + first + delcnt, (size_t)(a->alen - (int)first - (int)delcnt - (ins > (int)delcnt ? ins - (int)delcnt : 0)) * sizeof(V2));
    a->alen = newlen;
    for (i = 0; i < (u32)ins; i++) a->arr[first + i] = a2_vec_coerce(vm, argv[2 + i], a->vec_type);
    return v2_obj(r);
}
static V2 vec_concat(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *a = self_arr(self), *r; int i, k; if (!a) return v2_undef();
    r = vec_new_like(vm, a);
    for (i = 0; i < a->alen; i++) a2_arr_push(vm, r, a->arr[i]);
    for (k = 0; k < argc; k++) {
        A2Obj *b;
        /* avmplus checks the C++ vector kind only: all object-typed vectors are interchangeable (elements re-coerced) */
        if (!(argv[k].t == T_OBJ && argv[k].u.o->kind == OK_VECTOR && (argv[k].u.o->cls == a->cls || (!vec_numeric(vm, a) && !vec_numeric(vm, argv[k].u.o))))) { a2_throw_coerce_error(vm, argv[k], a->cls); return v2_undef(); }
        b = argv[k].u.o;
        if (b->cls != a->cls) { for (i = 0; i < b->alen; i++) { a2_vec_coerce(vm, b->arr[i], a->vec_type); if (vm->throwing) return v2_undef(); } }
        if (vm->swf_version < 11) {   /* bugzilla504525: arguments land at the original length (reverse order) */
            int at = a->alen, n = b->alen; a2_arr_fill(vm, r, r->alen + n); memmove(r->arr + at + n, r->arr + at, (size_t)(r->alen - at - n) * sizeof(V2)); for (i = 0; i < n; i++) r->arr[at + i] = b->arr[i];
        } else for (i = 0; i < b->alen; i++) a2_arr_push(vm, r, b->arr[i]);
    }
    return v2_obj(r);
}
static V2 vec_join(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *a = self_arr(self); A2Str *sep, *r = vm->s_empty; int i; if (!a) return v2_undef();
    sep = (argc > 0 && argv[0].t != T_UNDEF) ? v2_tostr(vm, argv[0]) : a2_str(vm, ",");
    { int limit = a->alen;   /* the length is sampled once; a toString() that shrinks the vector hits a RangeError */
      for (i = 0; i < limit && !vm->throwing; i++) { V2 el; if (i >= a->alen) { vec_range_error(vm, (double)i, a->alen); break; } el = a->arr[i]; V2_NORM(el); if (i) r = a2_concat(vm, r, sep); r = a2_concat(vm, r, v2_tostr(vm, el)); } }   /* null prints as "null" */
    return v2_str(r);
}
static V2 vec_toString(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return vec_join(vm, self, 0, NULL); }
static V2 vec_sort(VM2 *vm, V2 self, int argc, V2 *argv) { V2 a[1]; a[0] = argc ? argv[0] : v2_undef(); arr_sort(vm, self, 1, a); return self; }   /* Vector.sort returns this (index arrays / 0 are dropped) */
static V2 vec_toLocaleString(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *a = self_arr(self); A2Str *r = vm->s_empty; int i; UNUSED; if (!a) return v2_undef();
    for (i = 0; i < a->alen && !vm->throwing; i++) {
        V2 el = a->arr[i]; V2_NORM(el);
        if (i) r = a2_concat(vm, r, a2_str(vm, ","));
        if (el.t != T_UNDEF && el.t != T_NULL) { A2Name nm; V2 rv; nm.name = a2_str(vm, "toLocaleString"); nm.nss = &vm->ns_public; nm.nns = 1; nm.any_ns = 1; nm.attr = 0; rv = a2_call_prop(vm, el, &nm, 0, NULL); if (!vm->throwing) r = a2_concat(vm, r, v2_tostr(vm, rv)); }
    }
    return v2_str(r);
}
static V2 vec_reverse(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *a = self_arr(self); int i; UNUSED; if (!a) return v2_undef(); for (i = 0; i < a->alen / 2; i++) { V2 t = a->arr[i]; a->arr[i] = a->arr[a->alen - 1 - i]; a->arr[a->alen - 1 - i] = t; } return self; }
static V2 vec_indexOf(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *a = self_arr(self); u32 i, s; if (!a) return v2_int(-1); s = vec_clamp(argc > 1 ? NUM(1) : 0, (u32)a->alen); for (i = s; i < (u32)a->alen; i++) if (v2_strict_eq(vm, a->arr[i], ARG(0))) return v2_int((int)i); return v2_int(-1); }
static V2 vec_lastIndexOf(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *a = self_arr(self); u32 s; int i; if (!a) return v2_int(-1); s = vec_clamp((argc > 1 && argv[1].t != T_UNDEF) ? NUM(1) : 2147483647.0, (u32)a->alen); if (s == (u32)a->alen) { if (!s) return v2_int(-1); s--; } for (i = (int)s; i >= 0; i--) if (v2_strict_eq(vm, a->arr[i], ARG(0))) return v2_int(i); return v2_int(-1); }
static V2 vec_this_arg(V2 v) { return v.t == T_OBJ ? v : v2_null(); }
static int vec_cb_ok(VM2 *vm, V2 self, const char *name, int argc, V2 *argv) {
    if (argc < 1) { char b[600]; sprintf(b, "Argument count mismatch on %.400s/%s(). Expected 1, got 0.", self.t == T_OBJ && self.u.o->cls ? a2_class_name(vm, self.u.o->cls, 1)->s : "Vector", name); a2_throw_error(vm, "ArgumentError", b, 1063); return 0; }
    if (argv[0].t != T_NULL && argv[0].t != T_UNDEF) { a2_coerce(vm, argv[0], vm->c_function); if (vm->throwing) return 0; }
    return 1;
}
static V2 vec_forEach(VM2 *vm, V2 self, int argc, V2 *argv) { if (!vec_cb_ok(vm, self, "forEach", argc, argv)) return v2_undef(); A2Obj *a = self_arr(self); int i; if (!a) return v2_undef(); if (ARG(0).t == T_NULL || ARG(0).t == T_UNDEF) { return v2_undef(); } for (i = 0; i < a->alen && !vm->throwing; i++) call3(vm, ARG(0), vec_this_arg(ARG(1)), a->arr[i], i, self); return v2_undef(); }
static V2 vec_map(VM2 *vm, V2 self, int argc, V2 *argv) { if (!vec_cb_ok(vm, self, "map", argc, argv)) return v2_undef(); A2Obj *a = self_arr(self), *r; int i, n; if (!a) return v2_undef(); r = vec_new_like(vm, a); n = a->alen; if (ARG(0).t == T_NULL || ARG(0).t == T_UNDEF) { a2_arr_fill(vm, r, n); for (i = 0; i < n; i++) r->arr[i] = vec_default(vm, r); return vm->swf_version < 13 ? v2_undef() : v2_obj(r); } for (i = 0; i < n && !vm->throwing; i++) { V2 v = call3(vm, ARG(0), vec_this_arg(ARG(1)), a->arr[i], i, self); a2_arr_push(vm, r, a2_vec_coerce(vm, v, r->vec_type)); } return vm->swf_version < 13 ? v2_undef() : v2_obj(r); }   /* bugzilla574600 */
static V2 vec_filter(VM2 *vm, V2 self, int argc, V2 *argv) { if (!vec_cb_ok(vm, self, "filter", argc, argv)) return v2_undef(); A2Obj *a = self_arr(self), *r; int i, n; if (!a) return v2_undef(); r = vec_new_like(vm, a); n = a->alen; if (ARG(0).t == T_NULL || ARG(0).t == T_UNDEF) return v2_obj(r); for (i = 0; i < n && !vm->throwing; i++) { V2 el = a->arr[i]; V2 v = call3(vm, ARG(0), vec_this_arg(ARG(1)), el, i, self); if (v.t == T_BOOL && v.u.b) a2_arr_push(vm, r, el); } return v2_obj(r); }
static V2 vec_every(VM2 *vm, V2 self, int argc, V2 *argv) { if (!vec_cb_ok(vm, self, "every", argc, argv)) return v2_undef(); A2Obj *a = self_arr(self); int i; if (!a) return v2_bool(0); if (ARG(0).t == T_NULL || ARG(0).t == T_UNDEF) { return v2_bool(1); } for (i = 0; i < a->alen && !vm->throwing; i++) if (!v2_tobool(vm, call3(vm, ARG(0), vec_this_arg(ARG(1)), a->arr[i], i, self))) return v2_bool(0); return v2_bool(1); }
static V2 vec_some(VM2 *vm, V2 self, int argc, V2 *argv) { if (!vec_cb_ok(vm, self, "some", argc, argv)) return v2_undef(); A2Obj *a = self_arr(self); int i; if (!a) return v2_bool(0); if (ARG(0).t == T_NULL || ARG(0).t == T_UNDEF) { return v2_bool(0); } for (i = 0; i < a->alen && !vm->throwing; i++) if (v2_tobool(vm, call3(vm, ARG(0), vec_this_arg(ARG(1)), a->arr[i], i, self))) return v2_bool(1); return v2_bool(0); }
static V2 vec_insertAt(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *a = self_arr(self); int index, len; u32 ip; if (!a) return v2_undef();
    if (!vec_check_fixed(vm, a)) return v2_undef();
    index = INT(0); len = a->alen;
    if (index < 0) ip = (len + index < 0) ? 0 : (u32)(len + index); else ip = (u32)index;
    if (ip > (u32)len) ip = (u32)len;
    a2_arr_fill(vm, a, len + 1); memmove(a->arr + ip + 1, a->arr + ip, (size_t)(len - (int)ip) * sizeof(V2)); a->arr[ip] = a2_vec_coerce(vm, ARG(1), a->vec_type);
    return v2_undef();
}
static V2 vec_removeAt(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *a = self_arr(self); int index, len; u32 rp; V2 r; if (!a) return v2_undef();
    if (!vec_check_fixed(vm, a)) return v2_undef();
    index = INT(0); len = a->alen;
    if (index < 0) rp = (len + index < 0) ? 0 : (u32)(len + index); else rp = (u32)index;
    if (rp >= (u32)len) { vec_range_error(vm, (double)rp, len); return v2_undef(); }
    r = a->arr[rp]; memmove(a->arr + rp, a->arr + rp + 1, (size_t)(len - (int)rp - 1) * sizeof(V2)); a->alen--; V2_NORM(r); return r;
}
/* ---------------------------------------------------------------- */
/* Dictionary                                                       */

/* Dictionary keys: objects by identity; every other value (numbers, strings, null, undefined,
 * booleans, primitive wrappers) by its string form, as in the AVM2 (so d[13] == d["13"]). */
static int dict_is_obj_key(V2 k) { return k.t == T_OBJ && k.u.o->kind != OK_PRIM; }
static int dict_key_eq(VM2 *vm, V2 a, V2 b) {
    int ao = dict_is_obj_key(a), bo = dict_is_obj_key(b);
    if (ao || bo) return ao && bo && a.u.o == b.u.o;
    if (a.t == T_OBJ) a = a.u.o->prim; if (b.t == T_OBJ) b = b.u.o->prim;
    if (a.t == T_STR && b.t == T_STR) return a.u.s == b.u.s;
    return v2_tostr(vm, a) == v2_tostr(vm, b);
}
int a2_dict_find(VM2 *vm, A2Obj *o, V2 k) { int i; for (i = 0; i < o->dn; i++) if (o->dkeys[i].t != T_HOLE && dict_key_eq(vm, o->dkeys[i], k)) return i; return -1; }
int a2_dict_get(VM2 *vm, A2Obj *o, V2 k, V2 *out) { int i = a2_dict_find(vm, o, k); if (i < 0) return 0; *out = o->dvals[i]; return 1; }
void a2_dict_put(VM2 *vm, A2Obj *o, V2 k, V2 v) {
    int i = a2_dict_find(vm, o, k);
    if (i >= 0) { o->dvals[i] = v; return; }
    if (k.t == T_OBJ && k.u.o->kind == OK_PRIM) k = k.u.o->prim;
    for (i = 0; i < o->dn; i++) if (o->dkeys[i].t == T_HOLE) { o->dkeys[i] = k; o->dvals[i] = v; return; }   /* reuse a deleted slot */
    if (o->dn == o->dcap) { int nc = o->dcap ? o->dcap * 2 : 8; o->dkeys = (V2*)realloc(o->dkeys, (size_t)nc * sizeof(V2)); o->dvals = (V2*)realloc(o->dvals, (size_t)nc * sizeof(V2)); o->dcap = nc; }
    o->dkeys[o->dn] = k; o->dvals[o->dn] = v; o->dn++;
}
int a2_dict_delete(VM2 *vm, A2Obj *o, V2 k) {
    int i = a2_dict_find(vm, o, k);
    if (i < 0) return 0;
    /* tombstone: a for-in loop in progress keeps its position */
    o->dkeys[i] = v2_hole(); o->dvals[i] = v2_undef();
    while (o->dn > 0 && o->dkeys[o->dn - 1].t == T_HOLE) o->dn--;
    return 1;
}
static V2 Dictionary_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = SELFO; UNUSED; if (o) o->kind = OK_DICT; return v2_undef(); }

/* ---------------------------------------------------------------- */
/* Namespace / QName                                                */

static int xml_name_ok(A2Str *s) { u32 i; if (!s->len) return 0; for (i = 0; i < s->len; i++) { char c = s->s[i]; if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || ((c == '-' || c == '.' || (c >= '0' && c <= '9')) && i) || (u8)c >= 0x80)) return 0; } return 1; }
/* ECMA-357 13.2.2 as avmplus does it */
static V2 Namespace_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = SELFO; A2Str *kp = a2_str(vm, "__prefix");
    if (!o) return v2_undef();
    o->kind = OK_NAMESPACE; o->nsval.kind = NS_NAMESPACE;
    if (argc == 0) { o->nsval.uri = vm->s_empty; a2_dyn_set_hidden(vm, o, kp, v2_str(vm->s_empty)); return v2_undef(); }
    if (argc == 1) {
        V2 u = argv[0];
        if (u.t == T_OBJ && u.u.o->kind == OK_NAMESPACE) { A2Prop *pp = a2_dyn_find(u.u.o, kp); o->nsval = u.u.o->nsval; a2_dyn_set_hidden(vm, o, kp, pp ? pp->v : v2_undef()); return v2_undef(); }
        if (u.t == T_OBJ && u.u.o->kind == OK_QNAME && u.u.o->nsval.uri) { o->nsval.uri = u.u.o->nsval.uri; a2_dyn_set_hidden(vm, o, kp, v2_undef()); return v2_undef(); }
        o->nsval.uri = v2_tostr(vm, u); if (vm->throwing) return v2_undef();
        a2_dyn_set_hidden(vm, o, kp, o->nsval.uri->len ? v2_undef() : v2_str(vm->s_empty));
        return v2_undef();
    } else {
        V2 pv = argv[0], u = argv[1]; A2Str *ps = NULL;
        if (u.t == T_OBJ && u.u.o->kind == OK_NAMESPACE) o->nsval.uri = u.u.o->nsval.uri;
        else if (u.t == T_OBJ && u.u.o->kind == OK_QNAME && u.u.o->nsval.uri) o->nsval.uri = u.u.o->nsval.uri;
        else { o->nsval.uri = v2_tostr(vm, u); if (vm->throwing) return v2_undef(); }
        if (!o->nsval.uri) o->nsval.uri = vm->s_empty;
        if (pv.t == T_OBJ && pv.u.o->kind == OK_NAMESPACE) ps = pv.u.o->nsval.uri ? pv.u.o->nsval.uri : vm->s_empty;
        else { ps = v2_tostr(vm, pv); if (vm->throwing) return v2_undef(); }
        if (!o->nsval.uri->len) {
            if (pv.t != T_NULL && pv.t != T_UNDEF && !ps->len) { a2_dyn_set_hidden(vm, o, kp, v2_str(vm->s_empty)); return v2_undef(); }
            a2_throw_id(vm, "TypeError", 1098, ps->s, NULL); return v2_undef();
        }
        if (pv.t == T_NULL || pv.t == T_UNDEF) a2_dyn_set_hidden(vm, o, kp, v2_undef());
        else if (!ps->len) a2_dyn_set_hidden(vm, o, kp, v2_str(vm->s_empty));
        else a2_dyn_set_hidden(vm, o, kp, xml_name_ok(ps) ? v2_str(ps) : v2_undef());
    }
    return v2_undef();
}
static V2 Namespace_call(VM2 *vm, V2 self, int argc, V2 *argv) { if (argc == 1 && argv[0].t == T_OBJ && argv[0].u.o->kind == OK_NAMESPACE) return argv[0]; (void)self; return a2_construct_class(vm, vm->c_namespace, argc, argv); }
static V2 ns_uri(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return (self.t == T_OBJ && self.u.o->nsval.uri) ? v2_str(self.u.o->nsval.uri) : v2_str(vm->s_empty); }
static V2 ns_prefix(VM2 *vm, V2 self, int argc, V2 *argv) { A2Prop *p; UNUSED; p = self.t == T_OBJ ? a2_dyn_find(self.u.o, a2_str(vm, "__prefix")) : NULL; if (p) return p->v; if (self.t == T_OBJ && self.u.o->nsval.uri && self.u.o->nsval.uri->len == 0) return v2_str(vm->s_empty); return v2_undef(); }
/* QName: uri in nsval.uri (NULL = null), localName in prim, ival = attribute flag */
#define QN_URI(o) ((o)->nsval.uri)
#define QN_LOCAL(o) ((o)->prim.t == T_STR ? (o)->prim.u.s : vm->s_empty)
static V2 QName_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = SELFO; A2Str *uri = vm->s_empty, *local = vm->s_empty; int attr = 0; if (!o) return v2_undef();
    o->kind = OK_QNAME;
    if (argc == 1 || (argc >= 2 && argv[1].t == T_UNDEF && argc == 2 && 0)) {
        V2 a = argv[0];
        if (a.t == T_OBJ && a.u.o->kind == OK_QNAME) { uri = QN_URI(a.u.o); local = QN_LOCAL(a.u.o); attr = a.u.o->ival; }
        else { local = (a.t == T_UNDEF) ? vm->s_empty : v2_tostr(vm, a); if (local->len == 1 && local->s[0] == '*') uri = NULL; else uri = vm->dxns ? vm->dxns : vm->s_empty; }
    } else if (argc >= 2) {
        V2 ns = argv[0], nm = argv[1];
        if (ns.t == T_NULL) uri = NULL;
        else if (ns.t == T_UNDEF) uri = vm->dxns ? vm->dxns : vm->s_empty;
        else if (ns.t == T_OBJ && ns.u.o->kind == OK_NAMESPACE) uri = ns.u.o->nsval.uri;
        else if (ns.t == T_OBJ && ns.u.o->kind == OK_QNAME) uri = QN_URI(ns.u.o);
        else uri = v2_tostr(vm, ns);
        if (nm.t == T_OBJ && nm.u.o->kind == OK_QNAME) { local = QN_LOCAL(nm.u.o); attr = nm.u.o->ival; }
        else if (nm.t == T_UNDEF) local = vm->s_empty;
        else local = v2_tostr(vm, nm);
    }
    o->nsval.kind = NS_NAMESPACE; o->nsval.uri = uri; o->prim = v2_str(local); o->ival = attr;
    return v2_undef();
}
static V2 QName_call(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; if (argc == 1 && argv[0].t == T_OBJ && argv[0].u.o->kind == OK_QNAME) return argv[0]; return a2_construct_class(vm, vm->c_qname, argc, argv); }
static V2 qn_uri(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; if (self.t != T_OBJ || self.u.o->kind != OK_QNAME) return v2_undef(); return QN_URI(self.u.o) ? v2_str(QN_URI(self.u.o)) : v2_null(); }
static V2 qn_localName(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; if (self.t != T_OBJ || self.u.o->kind != OK_QNAME) return v2_undef(); return v2_str(QN_LOCAL(self.u.o)); }
static V2 qn_toString(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o; (void)argc; (void)argv;
    if (self.t != T_OBJ || self.u.o->kind != OK_QNAME) { a2_throw_error(vm, "TypeError", "Method QName.prototype.toString was invoked on an incompatible object.", 1004); return v2_undef(); }
    o = self.u.o;
    if (!QN_URI(o)) return v2_str(a2_concat(vm, a2_str(vm, "*::"), QN_LOCAL(o)));
    if (QN_URI(o)->len == 0) return v2_str(QN_LOCAL(o));
    return v2_str(a2_concat(vm, a2_concat(vm, QN_URI(o), a2_str(vm, "::")), QN_LOCAL(o)));
}
static V2 qn_valueOf(VM2 *vm, V2 self, int argc, V2 *argv) { (void)vm; (void)argc; (void)argv; return self; }
A2Str *a2_qname_uri(A2Obj *o) { return QN_URI(o); }
A2Str *a2_qname_local(VM2 *vm, A2Obj *o) { return QN_LOCAL(o); }
int a2_qname_attr(A2Obj *o) { return o->ival; }
V2 a2_make_qname(VM2 *vm, A2Str *uri, A2Str *local, int attr) { A2Obj *o = a2_new(vm, vm->c_qname); o->kind = OK_QNAME; o->nsval.kind = NS_NAMESPACE; o->nsval.uri = uri; o->prim = v2_str(local); o->ival = attr; return v2_obj(o); }

/* ---------------------------------------------------------------- */
/* global functions                                                 */

void avm2_trace_out(const char *s);
static V2 g_trace(VM2 *vm, V2 self, int argc, V2 *argv) {
    size_t cap = 256, len = 0; char *buf = (char*)malloc(cap); int i; (void)self;
    for (i = 0; i < argc; i++) { A2Str *s = v2_tostr(vm, argv[i]); if (vm->throwing) break; if (len + s->len + 2 > cap) { while (len + s->len + 2 > cap) cap *= 2; buf = (char*)realloc(buf, cap); } if (i) buf[len++] = ' '; memcpy(buf + len, s->s, s->len); len += s->len; }
    buf[len] = 0;
    if (vm->trace && !vm->throwing) {
        /* output form: NUL characters are dropped, lone surrogates (WTF-8) become U+FFFD */
        char *out = (char*)malloc(len * 2 + 1); size_t i, o = 0;
        for (i = 0; i < len; i++) {
            u8 c = (u8)buf[i];
            if (c == 0) continue;
            if (c == 0xED && i + 2 < len + 1 && ((u8)buf[i + 1] & 0xE0) == 0xA0) { out[o++] = (char)0xEF; out[o++] = (char)0xBF; out[o++] = (char)0xBD; i += 2; continue; }
            out[o++] = (char)c;
        }
        out[o] = 0;
        avm2_trace_out(out); free(out);
    }
    free(buf); return v2_undef();
}
static V2 g_parseInt(VM2 *vm, V2 self, int argc, V2 *argv) {
    /* parseInt(s:String = "NaN", radix:int = 0): undefined/null coerce to the String null, "null" */
    A2Str *s = argc < 1 ? a2_str(vm, "NaN") : (argv[0].t == T_UNDEF || argv[0].t == T_NULL) ? a2_str(vm, "null") : STR(0);
    int radix = (argc > 1 && argv[1].t != T_UNDEF) ? INT(1) : 0; const char *p = s->s; int neg = 0; double v = 0; int any = 0; (void)self;
    for (;;) {   /* white space, including the Unicode spaces */
        const u8 *q = (const u8*)p; unsigned c = q[0]; int l = 1;
        if (c >= 0xC0 && c < 0xE0 && q[1]) { c = ((c & 0x1F) << 6) | (q[1] & 0x3F); l = 2; }
        else if (c >= 0xE0 && c < 0xF0 && q[1] && q[2]) { c = ((c & 0x0F) << 12) | ((q[1] & 0x3F) << 6) | (q[2] & 0x3F); l = 3; }
        if (c == ' ' || (c >= 9 && c <= 13) || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF) p += l; else break;
    }
    if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
    if ((radix == 0 || radix == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { radix = 16; p += 2; }
    if (radix == 0) radix = 10;
    if (radix < 2 || radix > 36) return v2_num(0.0 / 0.0);
    for (;; p++) { int d; if (*p >= '0' && *p <= '9') d = *p - '0'; else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10; else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10; else break; if (d >= radix) break; v = v * radix + d; any = 1; }
    if (!any) return v2_num(0.0 / 0.0);
    return v2_num(neg ? -v : v);
}
static V2 g_parseFloat(VM2 *vm, V2 self, int argc, V2 *argv) {
    extern int a2_str_to_f64(const char *s, int swf_version, int strict, double *out);
    A2Str *s = STR(0); double d; (void)self;
    if (a2_str_to_f64(s->s, vm->swf_version, 0, &d)) return v2_num(d);
    return v2_num(0.0 / 0.0);
}
static V2 g_isNaN(VM2 *vm, V2 self, int argc, V2 *argv) { double d = NUM(0); (void)self; return v2_bool(d != d); }
static V2 g_isFinite(VM2 *vm, V2 self, int argc, V2 *argv) { double d = NUM(0); (void)self; return v2_bool(d == d && d < 1e308 * 10 && d > -1e308 * 10); }
static V2 g_isXMLName(VM2 *vm, V2 self, int argc, V2 *argv) { extern int a2_is_xml_name(const char *s, size_t len); A2Str *s; (void)self; if (!argc || argv[0].t == T_NULL || argv[0].t == T_UNDEF) return v2_bool(0); s = STR(0); return v2_bool(a2_is_xml_name(s->s, s->len)); }
static const char *uri_reserved = ";/?:@&=+$,#";
/* ECMA-262 Encode / Decode (code points; lone surrogates are errors) */
static V2 uri_encode(VM2 *vm, A2Str *s, int component) {
    u32 *u; int n, i; char *buf; size_t o = 0; V2 r;
    n = a2_utf8_decode(s->s, s->len, &u);
    buf = (char*)malloc((size_t)n * 12 + 1);
    if (!buf) { free(u); return v2_str(s); }
    for (i = 0; i < n; i++) {
        u32 c = u[i]; u8 t[4]; int k, m;
        if (c && c < 128 && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-_.!~*'()", (int)c) || (!component && strchr(uri_reserved, (int)c)))) { buf[o++] = (char)c; continue; }
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF) { c = 0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00); i++; }
        else if (c >= 0xD800 && c <= 0xDFFF) { free(buf); free(u); a2_throw_id(vm, "URIError", 1052, component ? "encodeURIComponent" : "encodeURI", NULL); return v2_undef(); }
        if (c < 0x80) { t[0] = (u8)c; m = 1; }
        else if (c < 0x800) { t[0] = (u8)(0xC0 | (c >> 6)); t[1] = (u8)(0x80 | (c & 63)); m = 2; }
        else if (c < 0x10000) { t[0] = (u8)(0xE0 | (c >> 12)); t[1] = (u8)(0x80 | ((c >> 6) & 63)); t[2] = (u8)(0x80 | (c & 63)); m = 3; }
        else { t[0] = (u8)(0xF0 | (c >> 18)); t[1] = (u8)(0x80 | ((c >> 12) & 63)); t[2] = (u8)(0x80 | ((c >> 6) & 63)); t[3] = (u8)(0x80 | (c & 63)); m = 4; }
        for (k = 0; k < m; k++) { sprintf(buf + o, "%%%02X", t[k]); o += 3; }
    }
    r = v2_str(a2_intern(vm, buf, o)); free(buf); free(u); return r;
}
static int hexv(u32 c) { if (c >= '0' && c <= '9') return (int)c - '0'; if (c >= 'a' && c <= 'f') return (int)c - 'a' + 10; if (c >= 'A' && c <= 'F') return (int)c - 'A' + 10; return -1; }
static V2 uri_decode(VM2 *vm, A2Str *s, int component) {
    u32 *u, *out; int n, i, o = 0; V2 r; const char *fn = component ? "decodeURIComponent" : "decodeURI";
    n = a2_utf8_decode(s->s, s->len, &u);
    out = (u32*)malloc(((size_t)n + 1) * sizeof(u32));
    if (!out) { free(u); return v2_str(s); }
    for (i = 0; i < n; i++) {
        int b, k, m, start = i; u32 cp;
        if (u[i] != '%') { out[o++] = u[i]; continue; }
        if (i + 2 >= n || hexv(u[i + 1]) < 0 || hexv(u[i + 2]) < 0) goto bad;
        b = hexv(u[i + 1]) * 16 + hexv(u[i + 2]); i += 2;
        if (b < 0x80) {
            if (b && !component && strchr(uri_reserved, b)) { int k2; for (k2 = start; k2 <= i; k2++) out[o++] = u[k2]; }
            else out[o++] = (u32)b;
            continue;
        }
        if ((b & 0xE0) == 0xC0) { m = 2; cp = (u32)b & 0x1F; } else if ((b & 0xF0) == 0xE0) { m = 3; cp = (u32)b & 0x0F; } else if ((b & 0xF8) == 0xF0) { m = 4; cp = (u32)b & 0x07; } else goto bad;
        for (k = 1; k < m; k++) {
            int c2;
            if (i + 3 >= n || u[i + 1] != '%') goto bad;
            if (hexv(u[i + 2]) < 0 || hexv(u[i + 3]) < 0) goto bad;
            c2 = hexv(u[i + 2]) * 16 + hexv(u[i + 3]);
            if ((c2 & 0xC0) != 0x80) goto bad;
            cp = (cp << 6) | ((u32)c2 & 0x3F); i += 3;
        }
        if ((m == 2 && cp < 0x80) || (m == 3 && cp < 0x800) || (m == 4 && (cp < 0x10000 || cp > 0x10FFFF))) goto bad;   /* avmplus decodes encoded surrogates */
        out[o++] = cp;
    }
    r = v2_str(a2_utf8_encode(vm, out, o)); free(out); free(u); return r;
bad:
    free(out); free(u);
    a2_throw_id(vm, "URIError", 1052, fn, NULL);
    return v2_undef();
}
static V2 uri_arg(VM2 *vm, int argc, V2 *argv, V2 (*f)(VM2*, A2Str*, int), int component) {
    if (argc < 1) return v2_cstr(vm, "undefined");
    if (argv[0].t == T_NULL || argv[0].t == T_UNDEF) return v2_cstr(vm, "null");
    return f(vm, v2_tostr(vm, argv[0]), component);
}
static V2 g_encodeURI(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; return uri_arg(vm, argc, argv, uri_encode, 0); }
static V2 g_encodeURIComponent(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; return uri_arg(vm, argc, argv, uri_encode, 1); }
static V2 g_decodeURI(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; return uri_arg(vm, argc, argv, uri_decode, 0); }
static V2 g_decodeURIComponent(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; return uri_arg(vm, argc, argv, uri_decode, 1); }
static V2 do_escape(VM2 *vm, A2Str *s, int unused) {
    u32 *u; int n, i; char *buf; size_t o = 0; V2 r; (void)unused;
    n = a2_utf8_decode(s->s, s->len, &u); buf = (char*)malloc((size_t)n * 12 + 1);
    for (i = 0; i < n; i++) {
        u32 c = u[i];
        if (c && c < 128 && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("@*_+-./", (int)c))) buf[o++] = (char)c;
        else if (c < 256) { sprintf(buf + o, "%%%02X", c); o += 3; }
        else if (c < 0x10000) { sprintf(buf + o, "%%u%04X", c); o += 6; }
        else { c -= 0x10000; sprintf(buf + o, "%%u%04X%%u%04X", 0xD800 + (c >> 10), 0xDC00 + (c & 0x3FF)); o += 12; }
    }
    r = v2_str(a2_intern(vm, buf, o)); free(buf); free(u); return r;
}
static V2 do_unescape(VM2 *vm, A2Str *s, int unused) {
    u32 *u, *out; int n, i, o = 0; V2 r; (void)unused;
    n = a2_utf8_decode(s->s, s->len, &u);
    out = (u32*)malloc(((size_t)n + 1) * sizeof(u32));
    for (i = 0; i < n; i++) {
        u32 c = u[i];
        if (c == '%') {
            if (i + 5 < n && u[i + 1] == 'u' && hexv(u[i + 2]) >= 0 && hexv(u[i + 3]) >= 0 && hexv(u[i + 4]) >= 0 && hexv(u[i + 5]) >= 0) {
                c = (u32)((hexv(u[i + 2]) << 12) | (hexv(u[i + 3]) << 8) | (hexv(u[i + 4]) << 4) | hexv(u[i + 5])); i += 5;
            } else if (i + 2 < n && hexv(u[i + 1]) >= 0 && hexv(u[i + 2]) >= 0) { c = (u32)(hexv(u[i + 1]) * 16 + hexv(u[i + 2])); i += 2; }
        }
        out[o++] = c;
    }
    r = v2_str(a2_utf8_encode(vm, out, o)); free(out); free(u); return r;
}
static V2 g_escape(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; return uri_arg(vm, argc, argv, do_escape, 0); }
static V2 g_unescape(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; return uri_arg(vm, argc, argv, do_unescape, 0); }
/* flash.utils.escapeMultiByte: UTF-8 bytes (lossy) up to a NUL, alphanumerics kept */
static V2 g_escapeMultiByte(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Str *s = STR(0); u32 *u; int n, i; char *buf; size_t o = 0; V2 r; (void)self;
    n = a2_utf8_decode(s->s, s->len, &u); buf = (char*)malloc((size_t)n * 12 + 1);
    for (i = 0; i < n; i++) {
        u32 c = u[i]; u8 b[4]; int nb, k;
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF) { c = 0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00); i++; }
        else if (c >= 0xD800 && c <= 0xDFFF) c = 0xFFFD;
        if (c == 0) break;
        if (c < 0x80) { b[0] = (u8)c; nb = 1; }
        else if (c < 0x800) { b[0] = (u8)(0xC0 | (c >> 6)); b[1] = (u8)(0x80 | (c & 63)); nb = 2; }
        else if (c < 0x10000) { b[0] = (u8)(0xE0 | (c >> 12)); b[1] = (u8)(0x80 | ((c >> 6) & 63)); b[2] = (u8)(0x80 | (c & 63)); nb = 3; }
        else { b[0] = (u8)(0xF0 | (c >> 18)); b[1] = (u8)(0x80 | ((c >> 12) & 63)); b[2] = (u8)(0x80 | ((c >> 6) & 63)); b[3] = (u8)(0x80 | (c & 63)); nb = 4; }
        for (k = 0; k < nb; k++) {
            if ((b[k] >= '0' && b[k] <= '9') || (b[k] >= 'a' && b[k] <= 'z') || (b[k] >= 'A' && b[k] <= 'Z')) buf[o++] = (char)b[k];
            else { sprintf(buf + o, "%%%02X", b[k]); o += 3; }
        }
    }
    r = v2_str(a2_intern(vm, buf, o)); free(buf); free(u); return r;
}
static int hexval(u32 c) { return c >= '0' && c <= '9' ? (int)(c - '0') : c >= 'a' && c <= 'f' ? (int)(c - 'a' + 10) : c >= 'A' && c <= 'F' ? (int)(c - 'A' + 10) : -1; }
/* lenient UTF-8 (surrogate code points allowed, a bad byte becomes that Latin-1 char) to UTF-16 units */
static int avm_utf8_units(const u8 *b, int n, u32 *out) {
    int i = 0, o = 0;
    while (i < n) {
        u32 c = b[i]; int len = c >= 0xF0 && c <= 0xF4 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 && c < 0xE0 ? 2 : 1, k, ok = 1;
        if (c < 0x80 || len == 1 || i + len > n) { out[o++] = c; i++; continue; }
        for (k = 1; k < len; k++) if ((b[i + k] & 0xC0) != 0x80) ok = 0;
        if (!ok) { out[o++] = c; i++; continue; }
        if (len == 2) c = ((c & 31) << 6) | (b[i + 1] & 63);
        else if (len == 3) c = ((c & 15) << 12) | ((u32)(b[i + 1] & 63) << 6) | (b[i + 2] & 63);
        else c = ((c & 7) << 18) | ((u32)(b[i + 1] & 63) << 12) | ((u32)(b[i + 2] & 63) << 6) | (b[i + 3] & 63);
        if (c >= 0x10000) { c -= 0x10000; out[o++] = 0xD800 + (c >> 10); out[o++] = 0xDC00 + (c & 1023); }
        else out[o++] = c;
        i += len;
    }
    return o;
}
static V2 g_unescapeMultiByte(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Str *s = STR(0); u32 *u, *out; u8 *bytes; int n, i = 0, o = 0; V2 r; (void)self;
    n = a2_utf8_decode(s->s, s->len, &u);
    out = (u32*)malloc(((size_t)n * 2 + 1) * sizeof(u32)); bytes = (u8*)malloc((size_t)n + 1);
    while (i < n) {
        u32 c = u[i++];
        if (c == 0) break;
        if (c == '%') {
            int nb = 0;
            for (;;) {
                int h, l;
                if (i >= n) break;
                h = hexval(u[i++]); if (h < 0) break;
                if (i >= n) break;
                l = hexval(u[i++]); if (l < 0) break;
                bytes[nb++] = (u8)((h << 4) | l);
                if (i < n && u[i] == '%') { i++; continue; }
                break;
            }
            o += avm_utf8_units(bytes, nb, out + o);
            continue;
        }
        if (c >= 0xD800 && c <= 0xDBFF && i < n && u[i] >= 0xDC00 && u[i] <= 0xDFFF) { out[o++] = c; out[o++] = u[i++]; continue; }
        if (c >= 0xD800 && c <= 0xDFFF) c = 0xFFFD;      /* unpaired surrogates in the input */
        out[o++] = c;
    }
    r = v2_str(a2_utf8_encode(vm, out, o)); free(out); free(bytes); free(u); return r;
}
static V2 g_getTimer(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_int((s32)vm->time_ms); }
static V2 g_getQualifiedClassName(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 v = ARG(0); A2Class *c; (void)self;
    if (v.t == T_UNDEF) return v2_cstr(vm, "void"); if (v.t == T_NULL) return v2_cstr(vm, "null");
    if (v.t == T_OBJ && v.u.o->kind == OK_CLASS) return v2_str(a2_class_name(vm, v.u.o->as_class, 1));
    c = v2_class_of(vm, v);
    if (v.t == T_OBJ && v.u.o->kind == OK_VECTOR && c && c->script == -1) return v2_str(a2_class_name(vm, c, 1));
    return v2_str(a2_class_name(vm, c, 1));
}
static V2 g_getQualifiedSuperclassName(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 v = ARG(0); A2Class *c; (void)self;
    if (v.t == T_UNDEF || v.t == T_NULL) return v2_null();
    c = (v.t == T_OBJ && v.u.o->kind == OK_CLASS) ? v.u.o->as_class : v2_class_of(vm, v);
    if (!c || !c->super) return v2_null();
    return v2_str(a2_class_name(vm, c->super, 1));
}
static V2 g_generateRandomBytes(VM2 *vm, V2 self, int argc, V2 *argv) {
    int n = INT(0), i; A2Obj *ba; extern void a2_bytearray_set(VM2 *vm, A2Obj *o, int i, int v); extern double a2_random01(void); (void)self;
    if (n < 1 || n > 1024) { char b[100]; sprintf(b, "Parameter numberRandomBytes must be between 1 and 1024."); a2_throw_error(vm, "RangeError", b, 2004); return v2_undef(); }
    ba = a2_new(vm, vm->c_bytearray); ba->kind = OK_BYTEARRAY;
    for (i = 0; i < n; i++) a2_bytearray_set(vm, ba, i, rand() & 255);
    ba->bpos = 0;
    return v2_obj(ba);
}
/* look a definition up by its qualified name ("pkg.name", "pkg::name", "name",
   "Vector.<T>"): 1 found, 0 not found, -1 a Vector of something not a class */
int a2_definition(VM2 *vm, A2Str *q, V2 *out) {
    const char *s = q->s; size_t n = q->len;
    if (n > 9 && s[n - 1] == '>' && (!strncmp(s, "Vector.<", 8) || !strncmp(s, "__AS3__.vec::Vector.<", 21))) {
        const char *in = s + (s[0] == 'V' ? 8 : 21); V2 iv; int r;
        size_t il = (size_t)(s + n - 1 - in);
        if (il == 1 && in[0] == '*') { *out = v2_obj(vm->c_vector->obj); return 1; }
        if (!il) return 0;
        r = a2_definition(vm, a2_intern(vm, in, il), &iv);
        if (r <= 0) return r;
        if (iv.t == T_OBJ && iv.u.o->kind == OK_CLASS) { A2Class *vc = a2_vector_class_for(vm, iv.u.o->as_class); if (!vc) return 0; *out = v2_obj(vc->obj); return 1; }
        return -1;
    }
    if (((s[0] == ':' && s[1] == ':') || s[0] == '.') && vm->swf_version && vm->swf_version < 13 && !strcmp(s + (s[0] == '.' ? 1 : 2), "JSON")) return 0;   /* API-versioned */
    { A2Class *c = a2_class_find_s(vm, s); if (c) { *out = v2_obj(c->obj); return 1; } }
    {   /* qualified: a package-level function, variable or class */
        const char *sep = strstr(s, "::"); size_t ul = 0;
        if (sep) ul = (size_t)(sep - s); else if ((sep = strrchr(s, '.')) != NULL) ul = (size_t)(sep - s);
        if (sep) {
            A2Ns ns; A2Name nm; V2 o;
            ns.kind = NS_PACKAGE; ns.uri = a2_intern(vm, s, ul); ns.id = 0;
            nm.name = a2_str(vm, sep + (sep[0] == ':' ? 2 : 1)); nm.nss = &ns; nm.nns = 1; nm.any_ns = 0; nm.attr = 0;
            if (a2_find_property(vm, NULL, 0, &nm, &o) && a2_get(vm, o, &nm, out)) return 1;
            vm->throwing = 0;
            return 0;
        }
    }
    {   A2Name nm; V2 o; nm.name = q; nm.nss = &vm->ns_public; nm.nns = 1; nm.any_ns = 1; nm.attr = 0;
        if (a2_find_property(vm, NULL, 0, &nm, &o) && a2_get(vm, o, &nm, out) && !(out->t == T_OBJ && out->u.o->kind == OK_CLASS && out->u.o->as_class->ns.uri->len)) return 1;
        vm->throwing = 0;
    }
    return 0;
}
V2 a2_get_definition(VM2 *vm, A2Str *q) {
    V2 v; int r = a2_definition(vm, q, &v);
    if (r > 0) return v;
    if (r < 0) { a2_throw_error(vm, "VerifyError", "The ABC data is corrupt, attempt to read out of bounds.", 1107); return v2_undef(); }
    { char b[300]; const char *ln = q->s, *p; if ((p = strstr(ln, "::")) != NULL) ln = p + 2; else if ((p = strrchr(ln, '.')) != NULL) ln = p + 1; sprintf(b, "Variable %.250s is not defined.", ln); a2_throw_error(vm, "ReferenceError", b, 1065); }
    return v2_undef();
}
static V2 g_getDefinitionByName(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; return a2_get_definition(vm, STR(0)); }
static V2 g_describeType(VM2 *vm, V2 self, int argc, V2 *argv) { extern V2 a2_describe_type(VM2 *vm, V2 v); (void)self; if (argc > 0 && argv[0].t == T_UNDEF) { a2_throw_error(vm, "TypeError", "A term is undefined and has no properties.", 1010); return v2_undef(); } return a2_describe_type(vm, ARG(0)); }
extern V2 a2_set_timeout(VM2 *vm, V2 fn, double delay, int repeat, int argc, V2 *argv);
extern void a2_clear_timeout(VM2 *vm, int id);
static V2 g_setTimeout(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; return a2_set_timeout(vm, ARG(0), NUM(1), 0, argc > 2 ? argc - 2 : 0, argv + 2); }
static V2 g_setInterval(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; return a2_set_timeout(vm, ARG(0), NUM(1), 1, argc > 2 ? argc - 2 : 0, argv + 2); }
static V2 g_clearTimeout(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; a2_clear_timeout(vm, INT(0)); return v2_undef(); }

/* ---------------------------------------------------------------- */
/* JSON                                                             */

typedef struct { VM2 *vm; const char *p; int err; } JP;
static void jp_ws(JP *j) { while (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r') j->p++; }
static V2 jp_value(JP *j);
static V2 jp_string(JP *j) {
    char *buf; size_t cap = 64, n = 0; V2 r;
    if (*j->p != '"') { j->err = 1; return v2_undef(); }
    j->p++; buf = (char*)malloc(cap);
    while (*j->p && *j->p != '"') {
        u32 c = (u8)*j->p;
        if (n + 8 > cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
        if (c < 0x20) { j->err = 1; free(buf); return v2_undef(); }
        if (c == '\\') { j->p++; c = (u8)*j->p; switch (c) { case 'n': c = '\n'; break; case 't': c = '\t'; break; case 'r': c = '\r'; break; case 'b': c = '\b'; break; case 'f': c = '\f'; break; case '"': case '\\': case '/': break;
            case 'u': { char h[5]; int k; for (k = 0; k < 4; k++) { char x = j->p[1 + k]; if (!((x >= '0' && x <= '9') || (x >= 'a' && x <= 'f') || (x >= 'A' && x <= 'F'))) { j->err = 1; free(buf); return v2_undef(); } h[k] = x; } h[4] = 0; c = (u32)strtol(h, NULL, 16); j->p += 4;
                /* a surrogate pair */
                if (c >= 0xD800 && c < 0xDC00 && j->p[1] == '\\' && j->p[2] == 'u') { char h2[5]; u32 lo; memcpy(h2, j->p + 3, 4); h2[4] = 0; lo = (u32)strtol(h2, NULL, 16); if (lo >= 0xDC00 && lo < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); j->p += 6; } }
                n += (size_t)enc1(c, buf + n); j->p++; continue; }
            default: j->err = 1; free(buf); return v2_undef(); } }
        buf[n++] = (char)c; j->p++;
    }
    if (*j->p != '"') { j->err = 1; free(buf); return v2_undef(); }
    j->p++;
    r = v2_str(a2_intern(j->vm, buf, n)); free(buf); return r;
}
static V2 jp_value(JP *j) {
    VM2 *vm = j->vm;
    jp_ws(j);
    if (*j->p == '{') { A2Obj *o = a2_new_plain(vm); j->p++; jp_ws(j); if (*j->p == '}') { j->p++; return v2_obj(o); }
        for (;;) { V2 k, v; jp_ws(j); k = jp_string(j); if (j->err) return v2_undef(); jp_ws(j); if (*j->p != ':') { j->err = 1; return v2_undef(); } j->p++; v = jp_value(j); if (j->err) return v2_undef(); a2_dyn_set(vm, o, v2_tostr(vm, k), v); jp_ws(j); if (*j->p == ',') { j->p++; continue; } if (*j->p == '}') { j->p++; return v2_obj(o); } j->err = 1; return v2_undef(); } }
    if (*j->p == '[') { A2Obj *a = a2_new_array(vm); j->p++; jp_ws(j); if (*j->p == ']') { j->p++; return v2_obj(a); }
        for (;;) { V2 v = jp_value(j); if (j->err) return v2_undef(); a2_arr_push(vm, a, v); jp_ws(j); if (*j->p == ',') { j->p++; continue; } if (*j->p == ']') { j->p++; return v2_obj(a); } j->err = 1; return v2_undef(); } }
    if (*j->p == '"') return jp_string(j);
    if (!strncmp(j->p, "true", 4)) { j->p += 4; return v2_bool(1); }
    if (!strncmp(j->p, "false", 5)) { j->p += 5; return v2_bool(0); }
    if (!strncmp(j->p, "null", 4)) { j->p += 4; return v2_null(); }
    {   /* strict JSON number: -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)? */
        extern int a2_str_to_f64(const char *s, int swf_version, int strict, double *out);
        const char *q = j->p; char *end; double d; char tmp[512]; size_t n;
        if (*q == '-') q++;
        if (*q >= '0' && *q <= '9') { while (*q >= '0' && *q <= '9') q++; }
        else { j->err = 1; return v2_undef(); }
        if (*q == '.') { q++; if (!(*q >= '0' && *q <= '9')) { j->err = 1; return v2_undef(); } while (*q >= '0' && *q <= '9') q++; }
        if (*q == 'e' || *q == 'E') { q++; if (*q == '+' || *q == '-') q++; if (!(*q >= '0' && *q <= '9')) { j->err = 1; return v2_undef(); } while (*q >= '0' && *q <= '9') q++; }
        n = (size_t)(q - j->p);
        if (n < sizeof(tmp)) { memcpy(tmp, j->p, n); tmp[n] = 0; if (!a2_str_to_f64(tmp, 99, 0, &d)) d = strtod(tmp, &end); }
        else { char *big = (char*)malloc(n + 1); if (!big) { j->err = 1; return v2_undef(); } memcpy(big, j->p, n); big[n] = 0; if (!a2_str_to_f64(big, 99, 0, &d)) d = strtod(big, &end); free(big); }
        j->p = q;
        if (d == floor(d) && fabs(d) < 2147483648.0 && !(d == 0 && 1 / d < 0)) return v2_int((s32)d);
        return v2_num(d);
    }
}
static V2 json_revive(VM2 *vm, V2 reviver, V2 key, V2 val) {
    V2 args[2];
    if (val.t == T_OBJ) {
        A2Obj *o = val.u.o;
        if (o->kind == OK_ARRAY) { int i; for (i = 0; i < o->alen; i++) { char b[16]; V2 nv; sprintf(b, "%d", i); nv = json_revive(vm, reviver, v2_cstr(vm, b), o->arr[i]); if (nv.t == T_UNDEF) o->arr[i] = v2_hole(); else o->arr[i] = nv; } }
        else { int idx = 0; A2Str *k; V2 v; A2Str *keys[1024]; int nk = 0, i; for (;;) { a2_dyn_enum(o, &idx, &k, &v); if (!idx) break; if (nk < 1024) keys[nk++] = k; } for (i = 0; i < nk; i++) { A2Prop *p = a2_dyn_find(o, keys[i]); V2 nv; if (!p) continue; nv = json_revive(vm, reviver, v2_str(keys[i]), p->v); if (nv.t == T_UNDEF) a2_dyn_del(o, keys[i]); else a2_dyn_set(vm, o, keys[i], nv); } }
    }
    args[0] = key; args[1] = val;
    return a2_call(vm, reviver, v2_null(), 2, args);
}
static V2 json_parse(VM2 *vm, V2 self, int argc, V2 *argv) {
    JP j; V2 r; A2Str *s; (void)self;
    if (argc < 1 || argv[0].t == T_NULL || argv[0].t == T_UNDEF) { vm->throw_frame = "Error$/throwError"; a2_throw_error(vm, "SyntaxError", "Invalid JSON parse input.", 1132); vm->throw_frame = NULL; return v2_undef(); }
    s = STR(0);
    j.vm = vm; j.p = s->s; j.err = 0;
    r = jp_value(&j); jp_ws(&j);
    if (j.err || *j.p) { vm->throw_frame = "JSON$/parseCore"; a2_throw_error(vm, "SyntaxError", "Invalid JSON parse input.", 1132); vm->throw_frame = NULL; return v2_undef(); }
    if (argc > 1 && argv[1].t != T_NULL && argv[1].t != T_UNDEF) { a2_coerce(vm, argv[1], vm->c_function); if (vm->throwing) return v2_undef(); }   /* reviver:Function */
    if (argc > 1 && argv[1].t == T_OBJ) r = json_revive(vm, argv[1], v2_str(vm->s_empty), r);
    return r;
}
typedef struct { char *buf; size_t n, cap; } SB;
static void sb_put(SB *b, const char *s, size_t n) { if (b->n + n + 1 > b->cap) { while (b->n + n + 1 > b->cap) b->cap *= 2; b->buf = (char*)realloc(b->buf, b->cap); } memcpy(b->buf + b->n, s, n); b->n += n; b->buf[b->n] = 0; }
static void sb_puts(SB *b, const char *s) { sb_put(b, s, strlen(s)); }
static A2Obj *g_json_stack[4096]; static int g_json_depth;
static void json_obj(VM2 *vm, SB *b, V2 v, A2Obj *o, V2 replacer, A2Str *indent, int depth);
static void json_str(SB *b, A2Str *s) { u32 i; sb_puts(b, "\""); for (i = 0; i < s->len; i++) { char c = s->s[i]; char t[8]; switch (c) { case '"': sb_puts(b, "\\\""); break; case '\\': sb_puts(b, "\\\\"); break; case '\n': sb_puts(b, "\\n"); break; case '\r': sb_puts(b, "\\r"); break; case '\t': sb_puts(b, "\\t"); break; case '\b': sb_puts(b, "\\b"); break; case '\f': sb_puts(b, "\\f"); break; default: if ((u8)c < 0x20) { sprintf(t, "\\u%04x", (u8)c); sb_puts(b, t); } else sb_put(b, &c, 1); } } sb_puts(b, "\""); }
static void json_val(VM2 *vm, SB *b, V2 v, V2 replacer, A2Str *indent, int depth);
static V2 json_apply_replacer(VM2 *vm, V2 replacer, V2 holder, A2Str *key, V2 v) {
    if (replacer.t == T_OBJ && replacer.u.o->kind == OK_FUNCTION) { V2 args[2]; args[0] = v2_str(key); args[1] = v; return a2_call(vm, replacer, holder, 2, args); }
    return v;
}
static int json_key_allowed(VM2 *vm, V2 replacer, A2Str *key) {
    if (replacer.t == T_OBJ && replacer.u.o->kind == OK_ARRAY) { int i; for (i = 0; i < replacer.u.o->alen; i++) if (v2_tostr(vm, replacer.u.o->arr[i]) == key) return 1; return 0; }
    return 1;
}
static void json_val(VM2 *vm, SB *b, V2 v, V2 replacer, A2Str *indent, int depth) {

    if (v.t == T_OBJ && v.u.o->kind != OK_ARRAY && v.u.o->kind != OK_VECTOR) {
        A2Name n; V2 f; A2Str *tj = a2_str(vm, "toJSON");
        n.name = tj; n.nss = &vm->ns_public; n.nns = 1; n.any_ns = 1; n.attr = 0;
        if (a2_get(vm, v, &n, &f) && f.t == T_OBJ && f.u.o->kind == OK_FUNCTION) { V2 args[1]; args[0] = v2_str(vm->s_empty); v = a2_call(vm, f, v, 1, args); }
    }
    switch (v.t) {
    case T_UNDEF: case T_HOLE: sb_puts(b, "null"); return;
    case T_NULL: sb_puts(b, "null"); return;
    case T_BOOL: sb_puts(b, v.u.b ? "true" : "false"); return;
    case T_INT: case T_UINT: case T_NUM: { double d = v2_tonum(vm, v); char t[64]; if (d != d || d > 1e308 || d < -1e308) sb_puts(b, "null"); else { v2_num_to_cstr(d, t); sb_puts(b, t); } return; }
    case T_STR: json_str(b, v.u.s); return;
    case T_OBJ: {
        A2Obj *o = v.u.o;
        if (o->kind == OK_PRIM) { json_val(vm, b, o->prim, v2_undef(), indent, depth); return; }
        if (o->kind == OK_FUNCTION) { sb_puts(b, "null"); return; }
        if (vm->throwing) return;
        { int k; for (k = 0; k < g_json_depth; k++) if (g_json_stack[k] == o) { a2_throw_id(vm, "TypeError", 1129, NULL, NULL); return; } }
        if (g_json_depth >= 4096) { a2_throw_id(vm, "TypeError", 1129, NULL, NULL); return; }
        g_json_stack[g_json_depth++] = o;
        json_obj(vm, b, v, o, replacer, indent, depth);
        g_json_depth--;
        return; }
    }
}
static void json_obj(VM2 *vm, SB *b, V2 v, A2Obj *o, V2 replacer, A2Str *indent, int depth) {
    int i;
    {
        if (o->kind == OK_ARRAY || o->kind == OK_VECTOR) {
            sb_puts(b, "["); for (i = 0; i < o->alen; i++) { char kb[16]; V2 ev; if (i) sb_puts(b, ","); if (indent) { int k; sb_puts(b, "\n"); for (k = 0; k <= depth; k++) sb_puts(b, indent->s); } sprintf(kb, "%d", i); ev = json_apply_replacer(vm, replacer, v, a2_str(vm, kb), a2_arr_get(o, i)); json_val(vm, b, ev, replacer, indent, depth + 1); }
            if (indent && o->alen) { int k; sb_puts(b, "\n"); for (k = 0; k < depth; k++) sb_puts(b, indent->s); } sb_puts(b, "]"); return;
        }
        {
            int first = 1, idx = 0; A2Str *key; V2 val;
            sb_puts(b, "{");
            /* class slots (public) */
            if (o->kind == OK_CLASS) {   /* a class: its public static slots, then the Class getters (prototype) */
                A2Trait *t; V2 sv; A2Name nn; nn.nss = &vm->ns_public; nn.nns = 1; nn.any_ns = 0; nn.attr = 0;
                for (t = o->as_class->ctraits.first; t; t = t->onext) if ((t->kind == TK_SLOT || t->kind == TK_CONST) && t->ns.kind == NS_PACKAGE && t->ns.uri->len == 0) {
                    nn.name = t->name; if (!a2_get(vm, v, &nn, &sv)) continue;
                    if (!json_key_allowed(vm, replacer, t->name)) continue; sv = json_apply_replacer(vm, replacer, v, t->name, sv); if (sv.t == T_UNDEF || (sv.t == T_OBJ && sv.u.o->kind == OK_FUNCTION)) continue;
                    if (!first) sb_puts(b, ","); first = 0; if (indent) { int k; sb_puts(b, "\n"); for (k = 0; k <= depth; k++) sb_puts(b, indent->s); } json_str(b, t->name); sb_puts(b, indent ? ": " : ":"); json_val(vm, b, sv, replacer, indent, depth + 1); }
                nn.name = vm->s_prototype;
                if (a2_get(vm, v, &nn, &sv) && json_key_allowed(vm, replacer, vm->s_prototype)) { sv = json_apply_replacer(vm, replacer, v, vm->s_prototype, sv);
                    if (!(sv.t == T_UNDEF || (sv.t == T_OBJ && sv.u.o->kind == OK_FUNCTION))) { if (!first) sb_puts(b, ","); first = 0; if (indent) { int k; sb_puts(b, "\n"); for (k = 0; k <= depth; k++) sb_puts(b, indent->s); } json_str(b, vm->s_prototype); sb_puts(b, indent ? ": " : ":"); json_val(vm, b, sv, replacer, indent, depth + 1); } }
            } else
            if (o->cls) { int pass; A2Trait *t; for (pass = 0; pass < 2; pass++) for (t = o->cls->itraits.first; t; t = t->onext) if (((pass == 0 && (t->kind == TK_SLOT || t->kind == TK_CONST)) || (pass == 1 && t->kind == TK_GETTER)) && t->ns.kind == NS_PACKAGE && t->ns.uri->len == 0) { V2 sv; A2Name nn; nn.name = t->name; nn.nss = &vm->ns_public; nn.nns = 1; nn.any_ns = 0; nn.attr = 0; if (!a2_get(vm, v, &nn, &sv) || (sv.t == T_OBJ && (sv.u.o->kind == OK_FUNCTION))) continue; if (vm->player->debug) fprintf(stderr, "[json] trait %s kind %d pass %d\n", t->name->s, t->kind, pass); if (!json_key_allowed(vm, replacer, t->name)) continue; sv = json_apply_replacer(vm, replacer, v, t->name, sv); if (sv.t == T_UNDEF || (sv.t == T_OBJ && sv.u.o->kind == OK_FUNCTION)) continue; if (!first) sb_puts(b, ","); first = 0; if (indent) { int k; sb_puts(b, "\n"); for (k = 0; k <= depth; k++) sb_puts(b, indent->s); } json_str(b, t->name); sb_puts(b, indent ? ": " : ":"); json_val(vm, b, sv, replacer, indent, depth + 1); } }
            if (replacer.t == T_OBJ && replacer.u.o->kind == OK_ARRAY) {   /* a replacer array gives the keys and their order (duplicates once) */
                A2Obj *ra = replacer.u.o; int ri, rj;
                for (ri = 0; ri < ra->alen; ri++) {
                    V2 e = ra->arr[ri]; A2Prop *pp; int dup = 0;
                    if (!(e.t == T_STR || v2_is_num(e))) continue;
                    key = v2_tostr(vm, e);
                    for (rj = 0; rj < ri && !dup; rj++) { V2 e2 = ra->arr[rj]; if ((e2.t == T_STR || v2_is_num(e2)) && v2_tostr(vm, e2) == key) dup = 1; }
                    if (dup) continue;
                    pp = a2_dyn_find(o, key); if (!pp || pp->hidden) continue;
                    val = json_apply_replacer(vm, replacer, v, key, pp->v); if (val.t == T_UNDEF || (val.t == T_OBJ && val.u.o->kind == OK_FUNCTION)) continue;
                    if (!first) sb_puts(b, ","); first = 0; if (indent) { int k; sb_puts(b, "\n"); for (k = 0; k <= depth; k++) sb_puts(b, indent->s); } json_str(b, key); sb_puts(b, indent ? ": " : ":"); json_val(vm, b, val, replacer, indent, depth + 1);
                }
            } else
            for (;;) { a2_dyn_enum(o, &idx, &key, &val); if (!idx) break; if (!json_key_allowed(vm, replacer, key)) continue; val = json_apply_replacer(vm, replacer, v, key, val); if (val.t == T_UNDEF || (val.t == T_OBJ && val.u.o->kind == OK_FUNCTION)) continue; if (!first) sb_puts(b, ","); first = 0; if (indent) { int k; sb_puts(b, "\n"); for (k = 0; k <= depth; k++) sb_puts(b, indent->s); } json_str(b, key); sb_puts(b, indent ? ": " : ":"); json_val(vm, b, val, replacer, indent, depth + 1); }
            if (indent && !first) { int k; sb_puts(b, "\n"); for (k = 0; k < depth; k++) sb_puts(b, indent->s); }
            sb_puts(b, "}");
        }
    }
}
static V2 json_stringify(VM2 *vm, V2 self, int argc, V2 *argv) {
    SB b; V2 r; A2Str *indent = NULL; (void)self;
    if (argc > 2) { if (argv[2].t == T_STR) indent = argv[2].u.s->len ? (argv[2].u.s->len > 10 ? a2_intern(vm, argv[2].u.s->s, 10) : argv[2].u.s) : NULL; else if (v2_is_num(argv[2])) { int n = INT(2), i; char t[12]; if (n > 10) n = 10; if (n > 0) { for (i = 0; i < n; i++) t[i] = ' '; t[n] = 0; indent = a2_str(vm, t); } } }
    if (argc > 1 && argv[1].t != T_NULL && !(argv[1].t == T_OBJ && (argv[1].u.o->kind == OK_FUNCTION || argv[1].u.o->kind == OK_ARRAY))) { a2_throw_id(vm, "TypeError", 1131, NULL, NULL); return v2_undef(); }
    b.cap = 256; b.n = 0; b.buf = (char*)malloc(b.cap); b.buf[0] = 0; g_json_depth = 0;
    { V2 top = json_apply_replacer(vm, ARG(1), v2_obj(a2_new_plain(vm)), vm->s_empty, ARG(0));
      if (top.t == T_UNDEF) { free(b.buf); return v2_cstr(vm, "null"); }   /* avmplus serialises a top-level undefined as null */
      json_val(vm, &b, top, ARG(1), indent, 0); }
    if (vm->throwing) { free(b.buf); return v2_undef(); }
    r = v2_str(a2_intern(vm, b.buf, b.n)); free(b.buf); return r;
}

/* ---------------------------------------------------------------- */
/* Date                                                             */

static double days_from_civil(int y, int m, int d) { int era; unsigned yoe, doy, doe; y -= m <= 2; era = (y >= 0 ? y : y - 399) / 400; yoe = (unsigned)(y - era * 400); doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1); doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; return (double)era * 146097.0 + (double)doe - 719468.0; }
static void civil_from_days(double z, int *y, int *m, int *d) { double zz = z + 719468; int era = (int)floor(zz / 146097); unsigned doe = (unsigned)(zz - era * 146097.0); unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; int yy = (int)yoe + era * 400; unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100); unsigned mp = (5 * doy + 2) / 153; *d = (int)(doy - (153 * mp + 2) / 5 + 1); *m = (int)(mp < 10 ? mp + 3 : mp - 9); *y = yy + (*m <= 2); }
static double make_time(double y, double mo, double d, double h, double mi, double s, double ms) {
    double ym, mn, days;
    if (y != y || mo != mo || d != d || h != h || mi != mi || s != s || ms != ms) return 0.0 / 0.0;
    ym = y + floor(mo / 12); mn = fmod(mo, 12); if (mn < 0) mn += 12;
    days = days_from_civil((int)ym, (int)mn + 1, 1) + d - 1;
    return days * 86400000.0 + h * 3600000.0 + mi * 60000.0 + s * 1000.0 + ms;
}
static int tz_offset_min = 0;   /* local = UTC + offset (set from the platform at VM start) */
extern int plat_tz_minutes(void); extern double plat_now_ms(void);
void a2_date_init_tz(void) { tz_offset_min = plat_tz_minutes(); }
static void date_fields(double t, int local, int *y, int *mo, int *d, int *h, int *mi, int *s, int *ms, int *wd) {
    double days, rem;
    if (local) t += tz_offset_min * 60000.0;
    days = floor(t / 86400000.0); rem = t - days * 86400000.0;
    civil_from_days(days, y, mo, d); *mo -= 1;
    *h = (int)(rem / 3600000.0); rem -= *h * 3600000.0; *mi = (int)(rem / 60000.0); rem -= *mi * 60000.0; *s = (int)(rem / 1000.0); *ms = (int)(rem - *s * 1000.0);
    *wd = (int)fmod(days + 4, 7); if (*wd < 0) *wd += 7;
}
/* Flash's Date.parse (Ruffle's parse_full_date): whitespace separated items,
   each one a date (YYYY/MM/DD, MM/DD/YYYY, Mon/DD/YYYY), HH:MM:SS, a day or
   month name, GMT/UTC[+-HHMM], or a number (a year from 70, else the day) */
static int all_digits(const char *p, int n) { int i; if (n <= 0) return 0; for (i = 0; i < n; i++) if (p[i] < '0' || p[i] > '9') return 0; return 1; }
static int num_of(const char *p, int n) { int i, v = 0; for (i = 0; i < n; i++) v = v * 10 + (p[i] - '0'); return v; }
static int mon_of(const char *p, int n) { static const char *mn[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" }; int i; if (n != 3) return -1; for (i = 0; i < 12; i++) if (!strncmp(p, mn[i], 3)) return i; return -1; }
static int split3(const char *p, int n, char sep, const char **f, int *fl) {
    int k = 0, i, st = 0;
    for (i = 0; i <= n; i++) if (i == n || p[i] == sep) { if (k == 3) return 0; f[k] = p + st; fl[k] = i - st; k++; st = i + 1; }
    return k == 3;
}
static double parse_date(const char *s) {
    double y = -1, mo = -1, d = -1, h = -1, mi = -1, sec = -1; int tz_set = 0, tz = 0;
    static const char *days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    const char *p = s;
    for (;;) {
        const char *it; int n, k; const char *f[3]; int fl[3];
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        it = p; while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        n = (int)(p - it);
        if (split3(it, n, '/', f, fl)) {
            int yy, mm, dd;
            if (fl[0] == 4 && fl[1] == 2 && fl[2] == 2 && all_digits(f[0], 4) && all_digits(f[1], 2) && all_digits(f[2], 2)) { yy = num_of(f[0], 4); mm = num_of(f[1], 2) - 1; dd = num_of(f[2], 2); }
            else if (fl[0] == 2 && fl[1] == 2 && fl[2] == 4 && all_digits(f[0], 2) && all_digits(f[1], 2) && all_digits(f[2], 4)) { yy = num_of(f[2], 4); mm = num_of(f[0], 2) - 1; dd = num_of(f[1], 2); }
            else if (fl[0] == 3 && fl[1] == 2 && fl[2] == 4 && mon_of(f[0], 3) >= 0 && all_digits(f[1], 2) && all_digits(f[2], 4)) { yy = num_of(f[2], 4); mm = mon_of(f[0], 3); dd = num_of(f[1], 2); }
            else return 0.0 / 0.0;
            if (mm < 0 || y >= 0 || mo >= 0 || d >= 0) return 0.0 / 0.0;
            y = yy; mo = mm; d = dd; continue;
        }
        if (split3(it, n, ':', f, fl)) {
            if (fl[0] != 2 || fl[1] != 2 || fl[2] != 2 || !all_digits(f[0], 2) || !all_digits(f[1], 2) || !all_digits(f[2], 2)) return 0.0 / 0.0;
            if (h >= 0 || mi >= 0 || sec >= 0) return 0.0 / 0.0;
            h = num_of(f[0], 2); mi = num_of(f[1], 2); sec = num_of(f[2], 2); continue;
        }
        for (k = 0; k < 7; k++) if (n == 3 && !strncmp(it, days[k], 3)) break;
        if (k < 7) continue;
        if ((k = mon_of(it, n)) >= 0) { mo = k; continue; }
        if (n >= 3 && (!strncmp(it, "GMT", 3) || !strncmp(it, "UTC", 3))) {
            if (tz_set) return 0.0 / 0.0;
            tz_set = 1;
            if (n == 3) tz = 0;
            else if (n == 8 && (it[3] == '+' || it[3] == '-') && all_digits(it + 4, 4)) { tz = num_of(it + 4, 2) * 60 + num_of(it + 6, 2); if (it[3] == '-') tz = -tz; }
            else return 0.0 / 0.0;
            continue;
        }
        if (all_digits(it, n) && n <= 9) {
            int v = num_of(it, n);
            if (v >= 70) { if (y >= 0) return 0.0 / 0.0; y = v < 100 ? v + 1900 : v; }
            else { if (d >= 0) return 0.0 / 0.0; d = v; }
            continue;
        }
        return 0.0 / 0.0;
    }
    if (y < 0 || mo < 0 || d < 0) return 0.0 / 0.0;
    if (h < 0) h = 0; if (mi < 0) mi = 0; if (sec < 0) sec = 0;
    return make_time(y, mo, d, h, mi, sec, 0) - (double)(tz_set ? tz : tz_offset_min) * 60000.0;
}
static double now_ms(void) { return plat_now_ms(); }
static V2 Date_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = SELFO; if (!o) return v2_undef();
    o->kind = OK_DATE;
    if (argc == 0) o->dval = now_ms();
    else if (argc == 1) { if (argv[0].t == T_STR) o->dval = parse_date(argv[0].u.s->s); else if (argv[0].t == T_OBJ && argv[0].u.o->kind == OK_DATE) o->dval = argv[0].u.o->dval; else { V2 p = v2_toprim(vm, argv[0], 0); o->dval = v2_tonum(vm, p); } }
    else { double y = NUM(0), mo = NUM(1), d = argc > 2 ? NUM(2) : 1, h = argc > 3 ? NUM(3) : 0, mi = argc > 4 ? NUM(4) : 0, s = argc > 5 ? NUM(5) : 0, ms = argc > 6 ? NUM(6) : 0; if (y >= 0 && y <= 99) y += 1900; o->dval = make_time(y, mo, d, h, mi, s, ms) - tz_offset_min * 60000.0; }
    if (o->dval == o->dval && fabs(o->dval) > 8.64e15) o->dval = 0.0 / 0.0;
    if (o->dval == o->dval) o->dval = o->dval < 0 ? -floor(-o->dval) + 0.0 : floor(o->dval);   /* TimeClip: ToInteger truncates */
    return v2_undef();
}
static V2 Date_call(VM2 *vm, V2 self, int argc, V2 *argv) { V2 d; (void)self; (void)argc; (void)argv; d = a2_construct_class(vm, vm->c_date, 0, NULL); return v2_str(v2_tostr(vm, d)); }
static double self_date(V2 self) { return (self.t == T_OBJ && self.u.o->kind == OK_DATE) ? self.u.o->dval : 0.0 / 0.0; }
#define DGET(name, local, field) static V2 name(VM2 *vm, V2 self, int argc, V2 *argv) { int y, mo, d, h, mi, s, ms, wd; double t = self_date(self); UNUSED; if (t != t) return v2_num(t); date_fields(t, local, &y, &mo, &d, &h, &mi, &s, &ms, &wd); return v2_num(field); }
DGET(d_getFullYear, 1, y) DGET(d_getMonth, 1, mo) DGET(d_getDate, 1, d) DGET(d_getHours, 1, h) DGET(d_getMinutes, 1, mi) DGET(d_getSeconds, 1, s) DGET(d_getMilliseconds, 1, ms) DGET(d_getDay, 1, wd)
DGET(d_getUTCFullYear, 0, y) DGET(d_getUTCMonth, 0, mo) DGET(d_getUTCDate, 0, d) DGET(d_getUTCHours, 0, h) DGET(d_getUTCMinutes, 0, mi) DGET(d_getUTCSeconds, 0, s) DGET(d_getUTCMilliseconds, 0, ms) DGET(d_getUTCDay, 0, wd)
static V2 d_getTime(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_num(self_date(self)); }
static V2 d_valueOf(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_num(self_date(self)); }
static V2 d_getTimezoneOffset(VM2 *vm, V2 self, int argc, V2 *argv) { double t = self_date(self); UNUSED; if (t != t) return v2_num(t); return v2_num(-tz_offset_min); }
static double time_clip(double t) { if (t != t || fabs(t) > 8.64e15) return 0.0 / 0.0; return t < 0 ? -floor(-t) + 0.0 : floor(t); }
static V2 d_setTime(VM2 *vm, V2 self, int argc, V2 *argv) { double t = argc > 0 ? time_clip(NUM(0)) : 0.0 / 0.0; if (self.t == T_OBJ && self.u.o->kind == OK_DATE) self.u.o->dval = t; return v2_num(t); }
static V2 d_set_generic(VM2 *vm, V2 self, int argc, V2 *argv, int local, int which) {
    int y, mo, d, h, mi, s, ms, wd; double t = self_date(self), f[7]; int i;
    if (self.t != T_OBJ) return v2_undef();
    if (t != t) { if (which != 0) return v2_num(t); t = 0; local = local ? 2 : 0; }
    date_fields(t, local == 1, &y, &mo, &d, &h, &mi, &s, &ms, &wd);
    f[0] = y; f[1] = mo; f[2] = d; f[3] = h; f[4] = mi; f[5] = s; f[6] = ms;
    for (i = 0; i < argc && which + i < 7; i++) f[which + i] = v2_tonum(vm, argv[i]);
    t = make_time(f[0], f[1], f[2], f[3], f[4], f[5], f[6]); if (local) t -= tz_offset_min * 60000.0;
    t = time_clip(t); self.u.o->dval = t; return v2_num(t);
}
#define DSET(name, local, which) static V2 name(VM2 *vm, V2 self, int argc, V2 *argv) { return d_set_generic(vm, self, argc, argv, local, which); }
DSET(d_setFullYear, 1, 0) DSET(d_setMonth, 1, 1) DSET(d_setDate, 1, 2) DSET(d_setHours, 1, 3) DSET(d_setMinutes, 1, 4) DSET(d_setSeconds, 1, 5) DSET(d_setMilliseconds, 1, 6)
DSET(d_setUTCFullYear, 0, 0) DSET(d_setUTCMonth, 0, 1) DSET(d_setUTCDate, 0, 2) DSET(d_setUTCHours, 0, 3) DSET(d_setUTCMinutes, 0, 4) DSET(d_setUTCSeconds, 0, 5) DSET(d_setUTCMilliseconds, 0, 6)
static const char *wdn[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static V2 d_toString(VM2 *vm, V2 self, int argc, V2 *argv) {
    int y, mo, d, h, mi, s, ms, wd; double t = self_date(self); char b[80]; UNUSED;
    if (t != t) return v2_cstr(vm, "Invalid Date");
    date_fields(t, 1, &y, &mo, &d, &h, &mi, &s, &ms, &wd);
    sprintf(b, "%s %s %d %02d:%02d:%02d GMT%c%02d%02d %d", wdn[wd], mon[mo], d, h, mi, s, tz_offset_min < 0 ? '-' : '+', abs(tz_offset_min) / 60, abs(tz_offset_min) % 60, y);
    return v2_cstr(vm, b);
}
static V2 d_toDateString(VM2 *vm, V2 self, int argc, V2 *argv) { int y, mo, d, h, mi, s, ms, wd; double t = self_date(self); char b[80]; UNUSED; if (t != t) return v2_cstr(vm, "Invalid Date"); date_fields(t, 1, &y, &mo, &d, &h, &mi, &s, &ms, &wd); sprintf(b, "%s %s %d %d", wdn[wd], mon[mo], d, y); return v2_cstr(vm, b); }
static V2 d_toTimeString(VM2 *vm, V2 self, int argc, V2 *argv) { int y, mo, d, h, mi, s, ms, wd; double t = self_date(self); char b[80]; UNUSED; if (t != t) return v2_cstr(vm, "Invalid Date"); date_fields(t, 1, &y, &mo, &d, &h, &mi, &s, &ms, &wd); sprintf(b, "%02d:%02d:%02d GMT%c%02d%02d", h, mi, s, tz_offset_min < 0 ? '-' : '+', abs(tz_offset_min) / 60, abs(tz_offset_min) % 60); return v2_cstr(vm, b); }
static V2 d_toUTCString(VM2 *vm, V2 self, int argc, V2 *argv) { int y, mo, d, h, mi, s, ms, wd; double t = self_date(self); char b[80]; UNUSED; if (t != t) return v2_cstr(vm, "Invalid Date"); date_fields(t, 0, &y, &mo, &d, &h, &mi, &s, &ms, &wd); sprintf(b, "%s %s %d %02d:%02d:%02d %d UTC", wdn[wd], mon[mo], d, h, mi, s, y); return v2_cstr(vm, b); }
static V2 d_toLocaleTimeString(VM2 *vm, V2 self, int argc, V2 *argv) { int y, mo, d, h, mi, s, ms, wd; double t = self_date(self); char b[80]; UNUSED; if (t != t) return v2_cstr(vm, "Invalid Date"); date_fields(t, 1, &y, &mo, &d, &h, &mi, &s, &ms, &wd); sprintf(b, "%d:%02d:%02d %s", h % 12 == 0 ? 12 : h % 12, mi, s, h < 12 ? "AM" : "PM"); return v2_cstr(vm, b); }
static V2 d_toLocaleDateString(VM2 *vm, V2 self, int argc, V2 *argv) { return d_toDateString(vm, self, argc, argv); }
static V2 d_toLocaleString(VM2 *vm, V2 self, int argc, V2 *argv) { int y, mo, d, h, mi, s, ms, wd; double t = self_date(self); char b[80]; UNUSED; if (t != t) return v2_cstr(vm, "Invalid Date"); date_fields(t, 1, &y, &mo, &d, &h, &mi, &s, &ms, &wd); sprintf(b, "%s %s %d %d %d:%02d:%02d %s", wdn[wd], mon[mo], d, y, h % 12 == 0 ? 12 : h % 12, mi, s, h < 12 ? "AM" : "PM"); return v2_cstr(vm, b); }
static V2 d_UTC(VM2 *vm, V2 self, int argc, V2 *argv) { double y = NUM(0), mo = NUM(1), d = argc > 2 ? NUM(2) : 1, h = argc > 3 ? NUM(3) : 0, mi = argc > 4 ? NUM(4) : 0, s = argc > 5 ? NUM(5) : 0, ms = argc > 6 ? NUM(6) : 0; (void)self; if (y >= 0 && y <= 99) y += 1900; return v2_num(make_time(y, mo, d, h, mi, s, ms)); }
static V2 d_parse(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; return v2_num(parse_date(STR(0)->s)); }
/* property-style accessors (date.fullYear etc) */
#define DPROP(pname, getter, setter) a2_add_getter(vm, c, pname, getter, setter);

/* ---------------------------------------------------------------- */
/* library setup                                                    */

/* prototype.toJSON of Dictionary and Date (avmplus) */
static V2 dict_toJSON(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; (void)argc; (void)argv; return v2_cstr(vm, "Dictionary"); }
static V2 date_toJSON(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return a2_call_s(vm, self, "toString", 0, NULL); }
static void add_proto_fn(VM2 *vm, A2Class *c, const char *name, NativeFn2 fn, int n) { A2Obj *f = a2_new_native_fn(vm, fn, name, n); if (f->method) f->method->dbg_name = a2_str(vm, "Function/<anonymous>"); a2_dyn_set_hidden(vm, c->prototype, a2_str(vm, name), v2_obj(f)); }

/* avmplus defines the methods of the core classes on their prototypes too
   (Date.prototype.getDate etc.): every native method trait not yet there */
static void mirror_to_proto(VM2 *vm, A2Class *c) {
    int b; A2Trait *t;
    for (b = 0; b < c->itraits.nb; b++) for (t = c->itraits.b[b]; t; t = t->next) {
        if (t->kind != TK_METHOD || !t->method || !t->method->native) continue;
        if (a2_dyn_find(c->prototype, t->name)) continue;
        add_proto_fn(vm, c, t->name->s, t->method->native, t->method->nparams);
    }
}
/* String.prototype.* (avmplus): String(this).AS3::method(...) */
static V2 sp_charAt(VM2 *vm, V2 self, int argc, V2 *argv) { return str_charAt(vm, v2_str(v2_tostr(vm, self)), argc, argv); }
static V2 sp_charCodeAt(VM2 *vm, V2 self, int argc, V2 *argv) { return str_charCodeAt(vm, v2_str(v2_tostr(vm, self)), argc, argv); }
static V2 sp_concat(VM2 *vm, V2 self, int argc, V2 *argv) { return str_concat(vm, v2_str(v2_tostr(vm, self)), argc, argv); }
static V2 sp_indexOf(VM2 *vm, V2 self, int argc, V2 *argv) { return str_indexOf(vm, v2_str(v2_tostr(vm, self)), argc, argv); }
static V2 sp_lastIndexOf(VM2 *vm, V2 self, int argc, V2 *argv) { return str_lastIndexOf(vm, v2_str(v2_tostr(vm, self)), argc, argv); }
static V2 sp_localeCompare(VM2 *vm, V2 self, int argc, V2 *argv) { return str_localeCompare(vm, v2_str(v2_tostr(vm, self)), argc, argv); }
static V2 sp_match(VM2 *vm, V2 self, int argc, V2 *argv) { return str_match(vm, v2_str(v2_tostr(vm, self)), argc, argv); }
static V2 sp_replace(VM2 *vm, V2 self, int argc, V2 *argv) { return str_replace(vm, v2_str(v2_tostr(vm, self)), argc, argv); }
static V2 sp_search(VM2 *vm, V2 self, int argc, V2 *argv) { return str_search(vm, v2_str(v2_tostr(vm, self)), argc, argv); }
static V2 sp_slice(VM2 *vm, V2 self, int argc, V2 *argv) { return str_slice(vm, v2_str(v2_tostr(vm, self)), argc, argv); }
static V2 sp_split(VM2 *vm, V2 self, int argc, V2 *argv) { return str_split(vm, v2_str(v2_tostr(vm, self)), argc, argv); }
static V2 sp_substr(VM2 *vm, V2 self, int argc, V2 *argv) { return str_substr(vm, v2_str(v2_tostr(vm, self)), argc, argv); }
static V2 sp_substring(VM2 *vm, V2 self, int argc, V2 *argv) { return str_substring(vm, v2_str(v2_tostr(vm, self)), argc, argv); }
static V2 sp_toLowerCase(VM2 *vm, V2 self, int argc, V2 *argv) { return str_toLowerCase(vm, v2_str(v2_tostr(vm, self)), argc, argv); }
static V2 sp_toUpperCase(VM2 *vm, V2 self, int argc, V2 *argv) { return str_toUpperCase(vm, v2_str(v2_tostr(vm, self)), argc, argv); }
static void string_proto(VM2 *vm, A2Class *c) {
    add_proto_fn(vm, c, "charAt", sp_charAt, 1);
    add_proto_fn(vm, c, "charCodeAt", sp_charCodeAt, 1);
    add_proto_fn(vm, c, "concat", sp_concat, 0);
    add_proto_fn(vm, c, "indexOf", sp_indexOf, 2);
    add_proto_fn(vm, c, "lastIndexOf", sp_lastIndexOf, 2);
    add_proto_fn(vm, c, "localeCompare", sp_localeCompare, 1);
    add_proto_fn(vm, c, "match", sp_match, 1);
    add_proto_fn(vm, c, "replace", sp_replace, 2);
    add_proto_fn(vm, c, "search", sp_search, 1);
    add_proto_fn(vm, c, "slice", sp_slice, 2);
    add_proto_fn(vm, c, "split", sp_split, 2);
    add_proto_fn(vm, c, "substr", sp_substr, 2);
    add_proto_fn(vm, c, "substring", sp_substring, 2);
    add_proto_fn(vm, c, "toLowerCase", sp_toLowerCase, 0);
    add_proto_fn(vm, c, "toUpperCase", sp_toUpperCase, 0);
    add_proto_fn(vm, c, "toLocaleLowerCase", sp_toLowerCase, 0); add_proto_fn(vm, c, "toLocaleUpperCase", sp_toUpperCase, 0);
}
static void math_strict(VM2 *vm, A2Class *c) {
    /* declared Math$/abs(x:Number) etc.: exact argument counts (max/min take any) */
    const char *nm[] = { "abs", "acos", "asin", "atan", "ceil", "cos", "exp", "floor", "log", "round", "sin", "sqrt", "tan", "atan2", "pow", "random", NULL };
    int i; A2Name n; A2Ns ns = vm->ns_public; A2Trait *t;
    n.nss = &ns; n.nns = 1; n.any_ns = 0; n.attr = 0;
    for (i = 0; nm[i]; i++) { n.name = a2_str(vm, nm[i]); t = a2_find_trait(&c->ctraits, n.name, &n); if (t && t->method) { int k = i < 13 ? 1 : i < 15 ? 2 : 0; t->method->argmin = (short)k; t->method->argmax = (short)(k ? k : -1); if (!k) t->method->argmax = -2; } }
}
static void math_statics(VM2 *vm, A2Class *c) {
    a2_add_static(vm, c, "abs", m_abs, 1); a2_add_static(vm, c, "acos", m_acos, 1); a2_add_static(vm, c, "asin", m_asin, 1); a2_add_static(vm, c, "atan", m_atan, 1); a2_add_static(vm, c, "atan2", m_atan2, 2);
    a2_add_static(vm, c, "ceil", m_ceil, 1); a2_add_static(vm, c, "cos", m_cos, 1); a2_add_static(vm, c, "exp", m_exp, 1); a2_add_static(vm, c, "floor", m_floor, 1); a2_add_static(vm, c, "log", m_log, 1);
    a2_add_static(vm, c, "max", m_max, 2); a2_add_static(vm, c, "min", m_min, 2); a2_add_static(vm, c, "pow", m_pow, 2); a2_add_static(vm, c, "random", m_random, 0); a2_add_static(vm, c, "round", m_round, 1);
    a2_add_static(vm, c, "sin", m_sin, 1); a2_add_static(vm, c, "sqrt", m_sqrt, 1); a2_add_static(vm, c, "tan", m_tan, 1);
    a2_add_const(vm, c, "E", v2_num(2.718281828459045)); a2_add_const(vm, c, "LN10", v2_num(2.302585092994046)); a2_add_const(vm, c, "LN2", v2_num(0.6931471805599453)); a2_add_const(vm, c, "LOG10E", v2_num(0.4342944819032518));
    a2_add_const(vm, c, "LOG2E", v2_num(1.4426950408889634)); a2_add_const(vm, c, "PI", v2_num(3.141592653589793)); a2_add_const(vm, c, "SQRT1_2", v2_num(0.7071067811865476)); a2_add_const(vm, c, "SQRT2", v2_num(1.4142135623730951));
}
static V2 np_toExponential(VM2 *vm, V2 self, int argc, V2 *argv) { return num_toExponential(vm, v2_num(v2_tonum(vm, self)), argc, argv); }
static V2 np_toPrecision(VM2 *vm, V2 self, int argc, V2 *argv) { return num_toPrecision(vm, v2_num(v2_tonum(vm, self)), argc, argv); }
static void num_proto(VM2 *vm, A2Class *c, NativeFn2 ts) {
    add_proto_fn(vm, c, "toLocaleString", ts, 1);
    if (!a2_dyn_find(c->prototype, a2_str(vm, "toFixed"))) add_proto_fn(vm, c, "toFixed", num_toFixed, 1);
    add_proto_fn(vm, c, "toExponential", np_toExponential, 1); add_proto_fn(vm, c, "toPrecision", np_toPrecision, 1);
}
static void global_fn(VM2 *vm, const char *name, NativeFn2 fn, int n) { a2_add_method(vm, vm->global->cls, name, fn, n); }
static void global_class_trait(VM2 *vm, A2Class *c) {
    /* classes are reachable through the global object's traits (name + package ns) */
    A2Trait *t = (A2Trait*)calloc(1, sizeof(A2Trait)); extern void a2_tm_put(A2TraitMap *tm, A2Trait *t);
    t->name = c->name; t->ns = c->ns; t->kind = TK_CLASS; t->cls = c; t->slot = vm->global->cls->nislots++;
    a2_tm_put(&vm->global->cls->itraits, t);
    { V2 *ns = (V2*)realloc(vm->global->slots, (size_t)vm->global->cls->nislots * sizeof(V2)); if (ns) { vm->global->slots = ns; vm->global->nslots = vm->global->cls->nislots; vm->global->slots[t->slot] = v2_obj(c->obj); } }
}
void a2_global_class(VM2 *vm, A2Class *c) { global_class_trait(vm, c); }
void a2_global_const(VM2 *vm, const char *name, V2 v) {
    A2Trait *t = (A2Trait*)calloc(1, sizeof(A2Trait)); extern void a2_tm_put(A2TraitMap *tm, A2Trait *t);
    t->name = a2_str(vm, name); t->ns = vm->ns_public; t->kind = TK_CONST; t->slot = vm->global->cls->nislots++;
    a2_tm_put(&vm->global->cls->itraits, t);
    { V2 *ns = (V2*)realloc(vm->global->slots, (size_t)vm->global->cls->nislots * sizeof(V2)); if (ns) { vm->global->slots = ns; vm->global->nslots = vm->global->cls->nislots; vm->global->slots[t->slot] = v; } }
}

void a2_lib_init(VM2 *vm) {
    A2Class *c, *gc;
    /* interned names */
    vm->s_empty = a2_str(vm, ""); vm->s_length = a2_str(vm, "length"); vm->s_prototype = a2_str(vm, "prototype"); vm->s_constructor = a2_str(vm, "constructor");
    vm->s_toString = a2_str(vm, "toString"); vm->s_valueOf = a2_str(vm, "valueOf"); vm->s_undefined = a2_str(vm, "undefined"); vm->s_null = a2_str(vm, "null");
    vm->s_object = a2_str(vm, "object"); vm->s_AS3 = a2_str(vm, "http://adobe.com/AS3/2006/builtin"); vm->s_callee = a2_str(vm, "callee"); vm->s_message = a2_str(vm, "message"); vm->s_name = a2_str(vm, "name");
    vm->s_enterFrame = a2_str(vm, "enterFrame"); vm->s_exitFrame = a2_str(vm, "exitFrame"); vm->s_addedToStage = a2_str(vm, "addedToStage"); vm->s_removedFromStage = a2_str(vm, "removedFromStage");
    vm->s_added = a2_str(vm, "added"); vm->s_removed = a2_str(vm, "removed"); vm->s_frameConstructed = a2_str(vm, "frameConstructed"); vm->s_mouseDown = a2_str(vm, "mouseDown"); vm->s_mouseUp = a2_str(vm, "mouseUp");
    vm->s_mouseMove = a2_str(vm, "mouseMove"); vm->s_mouseWheel = a2_str(vm, "mouseWheel"); vm->s_click = a2_str(vm, "click"); vm->s_keyDown = a2_str(vm, "keyDown"); vm->s_keyUp = a2_str(vm, "keyUp"); vm->s_timer = a2_str(vm, "timer"); vm->s_timerComplete = a2_str(vm, "timerComplete");
    vm->s_rollOver = a2_str(vm, "rollOver"); vm->s_rollOut = a2_str(vm, "rollOut"); vm->s_mouseOver = a2_str(vm, "mouseOver"); vm->s_mouseOut = a2_str(vm, "mouseOut"); vm->s_complete = a2_str(vm, "complete"); vm->s_init = a2_str(vm, "init");
    vm->s_resize = a2_str(vm, "resize"); vm->s_soundComplete = a2_str(vm, "soundComplete"); vm->s_change = a2_str(vm, "change"); vm->s_textInput = a2_str(vm, "textInput"); vm->s_id3 = a2_str(vm, "id3");
    vm->ns_public.kind = NS_PACKAGE; vm->ns_public.uri = vm->s_empty;
    vm->ns_as3.kind = NS_NAMESPACE; vm->ns_as3.uri = vm->s_AS3;
    vm->rng = 0;   /* seeded on first use from the clock (avmplus / Ruffle AvmRng) */
    vm->gc_threshold = 20000;

    /* core classes: Object and Class first (bootstrapping) */
    vm->c_object = c = a2_class_new(vm, "", "Object", NULL, OK_PLAIN);
    c->is_dynamic = 1; c->native_call = Object_call; c->native_ctor = Object_ctor;
    vm->c_class = a2_class_new(vm, "", "Class", vm->c_object, OK_CLASS); vm->c_class->is_dynamic = 1;
    vm->c_object->obj->cls = vm->c_class; vm->c_object->obj->proto = vm->c_class->prototype;
    vm->c_class->obj->cls = vm->c_class; vm->c_class->obj->proto = vm->c_class->prototype;
    vm->c_class->prototype->cls = vm->c_object; vm->c_object->prototype->cls = vm->c_object;
    /* global object */
    gc = a2_class_alloc(vm, vm->s_empty, a2_str(vm, "global"), NULL, OK_GLOBAL, 0); gc->is_dynamic = 1;
    vm->global = a2_new(vm, gc); vm->global->kind = OK_GLOBAL;
    global_class_trait(vm, vm->c_object); global_class_trait(vm, vm->c_class);

    vm->c_function = c = a2_class_new(vm, "", "Function", vm->c_object, OK_FUNCTION); c->native_ctor = Function_ctor; c->native_call = Function_call; c->is_dynamic = 1;
    add_proto_fn(vm, c, "toString", fn_proto_toString, 0); add_proto_fn(vm, c, "toLocaleString", fn_proto_toString, 0);
    add_proto_fn(vm, vm->c_object, "hasOwnProperty", obj_hasOwnProperty, 1);
    add_proto_fn(vm, vm->c_object, "isPrototypeOf", obj_isPrototypeOf, 1);
    add_proto_fn(vm, vm->c_object, "propertyIsEnumerable", obj_propertyIsEnumerable, 1);
    add_proto_fn(vm, vm->c_object, "setPropertyIsEnumerable", obj_setPropertyIsEnumerable, 2);
    add_proto_fn(vm, vm->c_object, "toString", obj_toString, 0);
    add_proto_fn(vm, vm->c_object, "toLocaleString", obj_toString, 0);
    add_proto_fn(vm, vm->c_object, "valueOf", obj_valueOf, 0);
    a2_add_const(vm, vm->c_object, "length", v2_int(1));

    /* AS3-namespace versions as traits too (callproperty AS3::toString) */
    { A2Trait *t; extern void a2_tm_put(A2TraitMap *tm, A2Trait *t);
      t = (A2Trait*)calloc(1, sizeof(A2Trait)); t->name = a2_str(vm, "hasOwnProperty"); t->ns = vm->ns_as3; t->kind = TK_METHOD; t->method = a2_native_method(vm, obj_hasOwnProperty, "hasOwnProperty", 1); t->method->owner = vm->c_object; t->method->sig = "Boolean(*=)"; a2_tm_put(&vm->c_object->itraits, t);
      t = (A2Trait*)calloc(1, sizeof(A2Trait)); t->name = a2_str(vm, "isPrototypeOf"); t->ns = vm->ns_as3; t->kind = TK_METHOD; t->method = a2_native_method(vm, obj_isPrototypeOf, "isPrototypeOf", 1); t->method->owner = vm->c_object; t->method->sig = "Boolean(*=)"; a2_tm_put(&vm->c_object->itraits, t);
      t = (A2Trait*)calloc(1, sizeof(A2Trait)); t->name = a2_str(vm, "propertyIsEnumerable"); t->ns = vm->ns_as3; t->kind = TK_METHOD; t->method = a2_native_method(vm, obj_propertyIsEnumerable, "propertyIsEnumerable", 1); t->method->owner = vm->c_object; t->method->sig = "Boolean(*=)"; a2_tm_put(&vm->c_object->itraits, t); }

    a2_add_method(vm, c, "call", fn_call, 1); a2_add_method(vm, c, "apply", fn_apply, 2); a2_add_getter(vm, c, "length", fn_length, NULL);
    c->prototype->kind = OK_FUNCTION; c->prototype->method = a2_native_method(vm, fn_noop, "Function", 0); c->prototype->cls = c;
    add_proto_fn(vm, c, "call", fn_call, 1); add_proto_fn(vm, c, "apply", fn_apply, 2);
    global_class_trait(vm, c);

    vm->c_array = c = a2_class_new(vm, "", "Array", vm->c_object, OK_ARRAY); c->native_ctor = Array_ctor; c->native_call = Array_call; c->is_dynamic = 1;
    c->prototype->kind = OK_ARRAY;   /* Array.prototype is an (empty) Array (its class traits stay off it: Array.prototype.filter = f is a plain dynamic set) */
    a2_add_getter(vm, c, "length", arr_length_get, arr_length_set);
    a2_add_method(vm, c, "push", arr_push, 1); a2_add_method(vm, c, "pop", arr_pop, 0); a2_add_method(vm, c, "shift", arr_shift, 0); a2_add_method(vm, c, "unshift", arr_unshift, 0);
    a2_add_method(vm, c, "slice", arr_slice, 2); a2_add_method(vm, c, "splice", arr_splice, 2); a2_add_method(vm, c, "concat", arr_concat, 1); a2_add_method(vm, c, "join", arr_join, 1);
    a2_add_method(vm, c, "reverse", arr_reverse, 0); a2_add_method(vm, c, "indexOf", arr_indexOf, 2); a2_add_method(vm, c, "lastIndexOf", arr_lastIndexOf, 2);
    a2_add_method(vm, c, "forEach", arr_forEach, 2); a2_add_method(vm, c, "map", arr_map, 2); a2_add_method(vm, c, "filter", arr_filter, 2); a2_add_method(vm, c, "every", arr_every, 2); a2_add_method(vm, c, "some", arr_some, 2);
    a2_add_method(vm, c, "sort", arr_sort, 1); a2_add_method(vm, c, "sortOn", arr_sortOn, 2); a2_add_method(vm, c, "insertAt", arr_insertAt, 2); a2_add_method(vm, c, "removeAt", arr_removeAt, 1);
    add_proto_fn(vm, c, "toString", arr_toString, 0); add_proto_fn(vm, c, "toLocaleString", arr_toLocaleString, 0); add_proto_fn(vm, c, "join", arr_join, 1); add_proto_fn(vm, c, "push", gen_push, 1); add_proto_fn(vm, c, "pop", gen_pop, 0);
    add_proto_fn(vm, c, "slice", arr_slice, 2); add_proto_fn(vm, c, "splice", arr_splice, 2); add_proto_fn(vm, c, "concat", arr_concat, 1); add_proto_fn(vm, c, "sort", arr_sort, 1); add_proto_fn(vm, c, "indexOf", arr_indexOf, 2); add_proto_fn(vm, c, "reverse", arr_reverse, 0); add_proto_fn(vm, c, "shift", gen_shift, 0); add_proto_fn(vm, c, "unshift", gen_unshift, 0);
    add_proto_fn(vm, c, "forEach", arr_forEach, 2); add_proto_fn(vm, c, "map", arr_map, 2); add_proto_fn(vm, c, "filter", arr_filter, 2); add_proto_fn(vm, c, "every", arr_every, 2); add_proto_fn(vm, c, "some", arr_some, 2); add_proto_fn(vm, c, "sortOn", arr_sortOn, 2); add_proto_fn(vm, c, "lastIndexOf", arr_lastIndexOf, 2);
    a2_add_const(vm, c, "CASEINSENSITIVE", v2_int(1)); a2_add_const(vm, c, "DESCENDING", v2_int(2)); a2_add_const(vm, c, "UNIQUESORT", v2_int(4)); a2_add_const(vm, c, "RETURNINDEXEDARRAY", v2_int(8)); a2_add_const(vm, c, "NUMERIC", v2_int(16)); a2_add_const(vm, c, "length", v2_int(1));
    global_class_trait(vm, c);

    vm->c_string = c = a2_class_new(vm, "", "String", vm->c_object, OK_PRIM); c->ctor_sig = "(*=)"; c->native_ctor = String_ctor; c->native_call = String_call; c->flags |= CF_FINAL;
    a2_add_getter(vm, c, "length", str_length, NULL);
    a2_add_method(vm, c, "charAt", str_charAt, 1); a2_add_method(vm, c, "charCodeAt", str_charCodeAt, 1); a2_add_method(vm, c, "concat", str_concat, 1); a2_add_method(vm, c, "indexOf", str_indexOf, 2);
    a2_add_method(vm, c, "lastIndexOf", str_lastIndexOf, 2); a2_add_method(vm, c, "slice", str_slice, 2); a2_add_method(vm, c, "substring", str_substring, 2); a2_add_method(vm, c, "substr", str_substr, 2);
    a2_add_method(vm, c, "toLowerCase", str_toLowerCase, 0); a2_add_method(vm, c, "toUpperCase", str_toUpperCase, 0); a2_add_method(vm, c, "toLocaleLowerCase", str_toLowerCase, 0); a2_add_method(vm, c, "toLocaleUpperCase", str_toUpperCase, 0);
    a2_add_method(vm, c, "toString", str_toString, 0); a2_add_method(vm, c, "valueOf", str_toString, 0); a2_add_method(vm, c, "localeCompare", str_localeCompare, 1);
    a2_add_method(vm, c, "split", str_split, 2); a2_add_method(vm, c, "match", str_match, 1); a2_add_method(vm, c, "replace", str_replace, 2); a2_add_method(vm, c, "search", str_search, 1);
    a2_add_static(vm, c, "fromCharCode", str_fromCharCode, 0); a2_add_const(vm, c, "length", v2_int(1));

    add_proto_fn(vm, c, "toString", str_toString, 0); add_proto_fn(vm, c, "valueOf", pv_string, 0); string_proto(vm, c);
    global_class_trait(vm, c);

    vm->c_number = c = a2_class_new(vm, "", "Number", vm->c_object, OK_PRIM); c->ctor_sig = "(*=)"; c->native_ctor = Number_ctor; c->native_call = Number_call; c->flags |= CF_FINAL;
    a2_add_method(vm, c, "toString", num_toString, 1); a2_add_method(vm, c, "valueOf", num_valueOf, 0); a2_add_method(vm, c, "toFixed", num_toFixed, 1); a2_add_method(vm, c, "toPrecision", num_toPrecision, 1); a2_add_method(vm, c, "toExponential", num_toExponential, 1);
    add_proto_fn(vm, c, "toString", num_toString, 1); add_proto_fn(vm, c, "valueOf", pv_number, 0); add_proto_fn(vm, c, "toFixed", num_toFixed, 1); num_proto(vm, c, num_toString);
    a2_add_const(vm, c, "MAX_VALUE", v2_num(1.7976931348623157e308)); a2_add_const(vm, c, "MIN_VALUE", v2_num(4.9e-324)); a2_add_const(vm, c, "NaN", v2_num(0.0 / 0.0));
    a2_add_const(vm, c, "NEGATIVE_INFINITY", v2_num(-1e308 * 10)); a2_add_const(vm, c, "POSITIVE_INFINITY", v2_num(1e308 * 10)); a2_add_const(vm, c, "length", v2_int(1));
    global_class_trait(vm, c);
    vm->c_int = c = a2_class_new(vm, "", "int", vm->c_object, OK_PRIM); c->ctor_sig = "(*=)"; c->native_ctor = int_ctor; c->native_call = int_call; c->flags |= CF_FINAL;
    a2_add_method(vm, c, "toString", num_toString, 1); a2_add_method(vm, c, "valueOf", num_valueOf, 0); a2_add_method(vm, c, "toFixed", num_toFixed, 1); a2_add_method(vm, c, "toPrecision", num_toPrecision, 1); a2_add_method(vm, c, "toExponential", num_toExponential, 1);
    add_proto_fn(vm, c, "toString", int_proto_toString, 1); add_proto_fn(vm, c, "valueOf", pv_int, 0); num_proto(vm, c, int_proto_toString);
    a2_add_const(vm, c, "MAX_VALUE", v2_int(2147483647)); a2_add_const(vm, c, "MIN_VALUE", v2_int((s32)0x80000000)); a2_add_const(vm, c, "length", v2_int(1));
    global_class_trait(vm, c);
    vm->c_uint = c = a2_class_new(vm, "", "uint", vm->c_object, OK_PRIM); c->ctor_sig = "(*=)"; c->native_ctor = uint_ctor; c->native_call = uint_call; c->flags |= CF_FINAL;
    a2_add_method(vm, c, "toString", num_toString, 1); a2_add_method(vm, c, "valueOf", num_valueOf, 0); a2_add_method(vm, c, "toFixed", num_toFixed, 1); a2_add_method(vm, c, "toPrecision", num_toPrecision, 1); a2_add_method(vm, c, "toExponential", num_toExponential, 1);
    add_proto_fn(vm, c, "toString", uint_proto_toString, 1); add_proto_fn(vm, c, "valueOf", pv_uint, 0); num_proto(vm, c, uint_proto_toString);
    a2_add_const(vm, c, "MAX_VALUE", v2_uint(4294967295u)); a2_add_const(vm, c, "MIN_VALUE", v2_uint(0)); a2_add_const(vm, c, "length", v2_int(1));
    global_class_trait(vm, c);
    vm->c_boolean = c = a2_class_new(vm, "", "Boolean", vm->c_object, OK_PRIM); c->ctor_sig = "(*=)"; c->native_ctor = Boolean_ctor; c->native_call = Boolean_call; c->flags |= CF_FINAL;
    a2_add_method(vm, c, "toString", bool_toString, 0); a2_add_method(vm, c, "valueOf", num_valueOf, 0); add_proto_fn(vm, c, "toString", bool_toString, 0); add_proto_fn(vm, c, "valueOf", pv_boolean, 0); a2_add_const(vm, c, "length", v2_int(1));
    global_class_trait(vm, c);

    vm->c_math = c = a2_class_new(vm, "", "Math", vm->c_object, OK_PLAIN); c->flags |= CF_FINAL;
    math_statics(vm, c); math_strict(vm, c);
    if (!vm->swf_version || vm->swf_version >= 16) { math_statics(vm, vm->c_number); math_strict(vm, vm->c_number); }   /* Number has them too (avmplus API 680) */
    global_class_trait(vm, c);

    vm->c_error = c = make_error_class(vm, "", "Error", vm->c_object);
    a2_add_method(vm, c, "getStackTrace", err_getStackTrace, 0); add_proto_fn(vm, c, "toString", err_toString, 0);
    a2_add_static(vm, c, "getErrorMessage", err_getErrorMessage, 1); a2_add_static(vm, c, "throwError", err_throwError, 2);
    a2_add_slot(vm, c, "message", NULL); a2_add_slot(vm, c, "name", NULL); a2_add_slot(vm, c, "errorID", NULL);
    { int b; A2Trait *t; for (b = 0; b < c->itraits.nb; b++) for (t = c->itraits.b[b]; t; t = t->next) if (t->name == vm->s_name) { t->def = v2_cstr(vm, "Error"); t->has_def = 1; } }
    /* Error.prototype is itself an Error instance (name/message slots = "Error"); subclass prototypes are plain objects */
    { A2Obj *pr = c->prototype; pr->cls = c; pr->kind = OK_ERROR; pr->nslots = c->nislots; pr->slots = (V2*)calloc((size_t)c->nislots, sizeof(V2)); a2_class_set_defaults(vm, c, pr);
      a2_dyn_del(pr, vm->s_name); a2_set_s(vm, v2_obj(pr), "message", v2_cstr(vm, "Error")); a2_set_s(vm, v2_obj(pr), "name", v2_cstr(vm, "Error")); a2_set_s(vm, v2_obj(pr), "errorID", v2_int(0)); }
    global_class_trait(vm, c);
    { const char *names[] = { "TypeError", "ReferenceError", "ArgumentError", "RangeError", "EvalError", "SecurityError", "SyntaxError", "URIError", "VerifyError", "DefinitionError", "UninitializedError", NULL }; int i;
      for (i = 0; names[i]; i++) { A2Class *e = make_error_class(vm, "", names[i], vm->c_error); { int b; A2Trait *t; for (b = 0; b < e->itraits.nb; b++) for (t = e->itraits.b[b]; t; t = t->next) if (t->name == vm->s_name) { t->def = v2_cstr(vm, names[i]); t->has_def = 1; } } global_class_trait(vm, e); }
      { const char *fe[] = { "IOError", "EOFError", "IllegalOperationError", "MemoryError", "ScriptTimeoutError", "StackOverflowError", "InvalidSWFError", NULL };
        /* flash.errors classes only set prototype.name; instances keep the "Error" name */
        for (i = 0; fe[i]; i++) { A2Class *e = make_error_class(vm, "flash.errors", fe[i], vm->c_error); global_class_trait(vm, e); }
        { A2Class *e = make_error_class(vm, "flash.errors", "DRMManagerError", vm->c_error); a2_dyn_del(e->prototype, vm->s_name); e->native_ctor = DRMManagerError_ctor; a2_add_slot(vm, e, "subErrorID", vm->c_int); a2_add_method(vm, e, "toString", drm_toString, 0); global_class_trait(vm, e); } } }

    vm->c_vector = c = a2_class_new(vm, "__AS3__.vec", "Vector", vm->c_object, OK_VECTOR); c->native_ctor = Vector_ctor; c->native_call = Vector_call; c->flags |= CF_FINAL;
    a2_add_getter(vm, c, "length", arr_length_get, vec_length_set); a2_add_getter(vm, c, "fixed", vec_fixed_get, vec_fixed_set);
    a2_add_method(vm, c, "push", vec_push, 1); a2_add_method(vm, c, "pop", vec_pop, 0); a2_add_method(vm, c, "shift", vec_shift, 0); a2_add_method(vm, c, "unshift", vec_unshift, 1);
    a2_add_method(vm, c, "slice", vec_slice, 2); a2_add_method(vm, c, "splice", vec_splice, 2); a2_add_method(vm, c, "concat", vec_concat, 1); a2_add_method(vm, c, "join", vec_join, 1);
    a2_add_method(vm, c, "reverse", vec_reverse, 0); a2_add_method(vm, c, "indexOf", vec_indexOf, 2); a2_add_method(vm, c, "lastIndexOf", vec_lastIndexOf, 2);
    a2_add_method(vm, c, "forEach", vec_forEach, 2); a2_add_method(vm, c, "map", vec_map, 2); a2_add_method(vm, c, "filter", vec_filter, 2); a2_add_method(vm, c, "every", vec_every, 2); a2_add_method(vm, c, "some", vec_some, 2);
    a2_add_method(vm, c, "sort", vec_sort, 1); a2_add_method(vm, c, "insertAt", vec_insertAt, 2); a2_add_method(vm, c, "removeAt", vec_removeAt, 1); a2_add_method(vm, c, "toString", vec_toString, 0); a2_add_method(vm, c, "toLocaleString", vec_toLocaleString, 0);
    add_proto_fn(vm, c, "toString", vec_toString, 0);
    add_proto_fn(vm, c, "slice", vec_slice, 2); add_proto_fn(vm, c, "splice", vec_splice, 2); add_proto_fn(vm, c, "concat", vec_concat, 1); add_proto_fn(vm, c, "join", vec_join, 1); add_proto_fn(vm, c, "reverse", vec_reverse, 0); add_proto_fn(vm, c, "indexOf", vec_indexOf, 2); add_proto_fn(vm, c, "lastIndexOf", vec_lastIndexOf, 2); add_proto_fn(vm, c, "forEach", vec_forEach, 2); add_proto_fn(vm, c, "map", vec_map, 2); add_proto_fn(vm, c, "filter", vec_filter, 2); add_proto_fn(vm, c, "every", vec_every, 2); add_proto_fn(vm, c, "some", vec_some, 2); add_proto_fn(vm, c, "sort", vec_sort, 1); add_proto_fn(vm, c, "insertAt", vec_insertAt, 2); add_proto_fn(vm, c, "removeAt", vec_removeAt, 1); add_proto_fn(vm, c, "toLocaleString", vec_toLocaleString, 0);
    global_class_trait(vm, c);
    { A2Class *vi = vector_class_for(vm, vm->c_int), *vu = vector_class_for(vm, vm->c_uint), *vn = vector_class_for(vm, vm->c_number), *vo = vector_class_for(vm, NULL); (void)vi; (void)vu; (void)vn; (void)vo; }

    vm->c_dictionary = c = a2_class_new(vm, "flash.utils", "Dictionary", vm->c_object, OK_DICT); c->ctor_sig = "(Boolean=)"; c->native_ctor = Dictionary_ctor; c->is_dynamic = 1; global_class_trait(vm, c);
    add_proto_fn(vm, c, "toJSON", dict_toJSON, 1);
    vm->c_namespace = c = a2_class_new(vm, "", "Namespace", vm->c_object, OK_NAMESPACE); c->native_ctor = Namespace_ctor; c->native_call = Namespace_call; c->flags |= CF_FINAL; a2_add_const(vm, c, "length", v2_int(2));
    a2_add_getter(vm, c, "uri", ns_uri, NULL); a2_add_getter(vm, c, "prefix", ns_prefix, NULL); a2_add_method(vm, c, "toString", ns_uri, 0); a2_add_method(vm, c, "valueOf", ns_uri, 0); add_proto_fn(vm, c, "toString", ns_uri, 0); global_class_trait(vm, c);
    vm->c_qname = c = a2_class_new(vm, "", "QName", vm->c_object, OK_QNAME); c->native_ctor = QName_ctor; c->native_call = QName_call; c->is_dynamic = 0; c->flags |= CF_FINAL; c->ctor_sig = "(*=,*=)";
    a2_add_getter(vm, c, "uri", qn_uri, NULL); a2_add_getter(vm, c, "localName", qn_localName, NULL);
    a2_add_method(vm, c, "toString", qn_toString, 0); a2_add_method(vm, c, "valueOf", qn_valueOf, 0); add_proto_fn(vm, c, "toString", qn_toString, 0); add_proto_fn(vm, c, "valueOf", qn_valueOf, 0); global_class_trait(vm, c);

    c = a2_class_new(vm, "", "JSON", vm->c_object, OK_PLAIN); a2_add_static(vm, c, "parse", json_parse, 2); a2_add_static(vm, c, "stringify", json_stringify, 3); global_class_trait(vm, c); c->flags |= CF_FINAL;

    vm->c_date = c = a2_class_new(vm, "", "Date", vm->c_object, OK_DATE); c->native_ctor = Date_ctor; c->native_call = Date_call; c->is_dynamic = 1; c->flags |= CF_FINAL; a2_add_const(vm, c, "length", v2_int(7));
    c->prototype->kind = OK_DATE; c->prototype->dval = 0.0 / 0.0;   /* Date.prototype is an (invalid) Date */
    add_proto_fn(vm, c, "toJSON", date_toJSON, 1);
    a2_add_method(vm, c, "getFullYear", d_getFullYear, 0); a2_add_method(vm, c, "getMonth", d_getMonth, 0); a2_add_method(vm, c, "getDate", d_getDate, 0); a2_add_method(vm, c, "getHours", d_getHours, 0); a2_add_method(vm, c, "getMinutes", d_getMinutes, 0); a2_add_method(vm, c, "getSeconds", d_getSeconds, 0); a2_add_method(vm, c, "getMilliseconds", d_getMilliseconds, 0); a2_add_method(vm, c, "getDay", d_getDay, 0);
    a2_add_method(vm, c, "getUTCFullYear", d_getUTCFullYear, 0); a2_add_method(vm, c, "getUTCMonth", d_getUTCMonth, 0); a2_add_method(vm, c, "getUTCDate", d_getUTCDate, 0); a2_add_method(vm, c, "getUTCHours", d_getUTCHours, 0); a2_add_method(vm, c, "getUTCMinutes", d_getUTCMinutes, 0); a2_add_method(vm, c, "getUTCSeconds", d_getUTCSeconds, 0); a2_add_method(vm, c, "getUTCMilliseconds", d_getUTCMilliseconds, 0); a2_add_method(vm, c, "getUTCDay", d_getUTCDay, 0);
    a2_add_method(vm, c, "getTime", d_getTime, 0); a2_add_method(vm, c, "valueOf", d_valueOf, 0); a2_add_method(vm, c, "getTimezoneOffset", d_getTimezoneOffset, 0); a2_add_method(vm, c, "setTime", d_setTime, 1);
    a2_add_method(vm, c, "setFullYear", d_setFullYear, 3); a2_add_method(vm, c, "setMonth", d_setMonth, 2); a2_add_method(vm, c, "setDate", d_setDate, 1); a2_add_method(vm, c, "setHours", d_setHours, 4); a2_add_method(vm, c, "setMinutes", d_setMinutes, 3); a2_add_method(vm, c, "setSeconds", d_setSeconds, 2); a2_add_method(vm, c, "setMilliseconds", d_setMilliseconds, 1);
    a2_add_method(vm, c, "setUTCFullYear", d_setUTCFullYear, 3); a2_add_method(vm, c, "setUTCMonth", d_setUTCMonth, 2); a2_add_method(vm, c, "setUTCDate", d_setUTCDate, 1); a2_add_method(vm, c, "setUTCHours", d_setUTCHours, 4); a2_add_method(vm, c, "setUTCMinutes", d_setUTCMinutes, 3); a2_add_method(vm, c, "setUTCSeconds", d_setUTCSeconds, 2); a2_add_method(vm, c, "setUTCMilliseconds", d_setUTCMilliseconds, 1);
    a2_add_method(vm, c, "toString", d_toString, 0); a2_add_method(vm, c, "toDateString", d_toDateString, 0); a2_add_method(vm, c, "toTimeString", d_toTimeString, 0); a2_add_method(vm, c, "toUTCString", d_toUTCString, 0); a2_add_method(vm, c, "toLocaleString", d_toLocaleString, 0); a2_add_method(vm, c, "toLocaleDateString", d_toLocaleDateString, 0); a2_add_method(vm, c, "toLocaleTimeString", d_toLocaleTimeString, 0);
    add_proto_fn(vm, c, "toString", d_toString, 0); add_proto_fn(vm, c, "valueOf", d_valueOf, 0);
    a2_add_static(vm, c, "UTC", d_UTC, 7); a2_add_static(vm, c, "parse", d_parse, 1);
    DPROP("fullYear", d_getFullYear, d_setFullYear) DPROP("month", d_getMonth, d_setMonth) DPROP("date", d_getDate, d_setDate) DPROP("hours", d_getHours, d_setHours) DPROP("minutes", d_getMinutes, d_setMinutes) DPROP("seconds", d_getSeconds, d_setSeconds) DPROP("milliseconds", d_getMilliseconds, d_setMilliseconds) DPROP("day", d_getDay, NULL)
    DPROP("fullYearUTC", d_getUTCFullYear, d_setUTCFullYear) DPROP("monthUTC", d_getUTCMonth, d_setUTCMonth) DPROP("dateUTC", d_getUTCDate, d_setUTCDate) DPROP("hoursUTC", d_getUTCHours, d_setUTCHours) DPROP("minutesUTC", d_getUTCMinutes, d_setUTCMinutes) DPROP("secondsUTC", d_getUTCSeconds, d_setUTCSeconds) DPROP("millisecondsUTC", d_getUTCMilliseconds, d_setUTCMilliseconds) DPROP("dayUTC", d_getUTCDay, NULL)
    DPROP("time", d_getTime, d_setTime) DPROP("timezoneOffset", d_getTimezoneOffset, NULL)
    mirror_to_proto(vm, c);
    global_class_trait(vm, c);

    /* global functions and constants */
    global_fn(vm, "trace", g_trace, 1); global_fn(vm, "parseInt", g_parseInt, 2); global_fn(vm, "parseFloat", g_parseFloat, 1); global_fn(vm, "isNaN", g_isNaN, 1); global_fn(vm, "isFinite", g_isFinite, 1);
    global_fn(vm, "encodeURI", g_encodeURI, 1); global_fn(vm, "encodeURIComponent", g_encodeURIComponent, 1); global_fn(vm, "decodeURI", g_decodeURI, 1); global_fn(vm, "decodeURIComponent", g_decodeURIComponent, 1);
    global_fn(vm, "escape", g_escape, 1); global_fn(vm, "unescape", g_unescape, 1); global_fn(vm, "isXMLName", g_isXMLName, 1);
    a2_global_const(vm, "NaN", v2_num(0.0 / 0.0)); a2_global_const(vm, "Infinity", v2_num(1e308 * 10)); a2_global_const(vm, "undefined", v2_undef());
    /* flash.utils package functions */
    { A2Trait *t; extern void a2_tm_put(A2TraitMap *tm, A2Trait *t); A2Ns ns; ns.kind = NS_PACKAGE; ns.uri = a2_str(vm, "flash.utils");
      struct { const char *n; NativeFn2 f; int np; } fu[] = { { "getTimer", g_getTimer, 0 }, { "getQualifiedClassName", g_getQualifiedClassName, 1 }, { "getQualifiedSuperclassName", g_getQualifiedSuperclassName, 1 }, { "getDefinitionByName", g_getDefinitionByName, 1 }, { "escapeMultiByte", g_escapeMultiByte, 1 }, { "unescapeMultiByte", g_unescapeMultiByte, 1 }, { "describeType", g_describeType, 1 }, { "setTimeout", g_setTimeout, 2 }, { "setInterval", g_setInterval, 2 }, { "clearTimeout", g_clearTimeout, 1 }, { "clearInterval", g_clearTimeout, 1 }, { NULL, NULL, 0 } };
      int i; for (i = 0; fu[i].n; i++) { t = (A2Trait*)calloc(1, sizeof(A2Trait)); t->name = a2_str(vm, fu[i].n); t->ns = ns; t->kind = TK_METHOD; t->method = a2_native_method(vm, fu[i].f, fu[i].n, fu[i].np); { char b[100]; sprintf(b, "global/flash.utils::%s", fu[i].n); t->method->dbg_name = a2_str(vm, b); } a2_tm_put(&vm->global->cls->itraits, t); } }
    if (getenv("FLASHNT_HARNESS")) {   /* the test harness: Ruffle's __ruffle__.isDependent(string) */
      A2Trait *t; extern void a2_tm_put(A2TraitMap *tm, A2Trait *t); A2Ns ns; ns.kind = NS_PACKAGE; ns.uri = a2_str(vm, "__ruffle__");
      t = (A2Trait*)calloc(1, sizeof(A2Trait)); t->name = a2_str(vm, "isDependent"); t->ns = ns; t->kind = TK_METHOD; t->method = a2_native_method(vm, g_isDependent, "isDependent", 1); t->method->dbg_name = a2_str(vm, "global/__ruffle__::isDependent"); a2_tm_put(&vm->global->cls->itraits, t); }
    /* flash.crypto.generateRandomBytes */
    { A2Trait *t; extern void a2_tm_put(A2TraitMap *tm, A2Trait *t); A2Ns ns; ns.kind = NS_PACKAGE; ns.uri = a2_str(vm, "flash.crypto");
      t = (A2Trait*)calloc(1, sizeof(A2Trait)); t->name = a2_str(vm, "generateRandomBytes"); t->ns = ns; t->kind = TK_METHOD; t->method = a2_native_method(vm, g_generateRandomBytes, "generateRandomBytes", 1); t->method->dbg_name = a2_str(vm, "global/flash.crypto::generateRandomBytes"); a2_tm_put(&vm->global->cls->itraits, t); }
    { const char *ln[] = { "Vector$int", "Vector$uint", "Vector$double", "Vector$object", NULL }; A2Class *lt[4]; int i; A2Trait *t; extern void a2_tm_put(A2TraitMap *tm, A2Trait *t); A2Ns ns;
      lt[0] = vm->c_int; lt[1] = vm->c_uint; lt[2] = vm->c_number; lt[3] = NULL; ns.kind = NS_PACKAGE; ns.uri = a2_str(vm, "__AS3__.vec");
      for (i = 0; ln[i]; i++) { A2Class *vc = a2_vector_class_for(vm, lt[i]); t = (A2Trait*)calloc(1, sizeof(A2Trait)); t->name = a2_str(vm, ln[i]); t->ns = ns; t->kind = TK_CONST; t->slot = vm->global->cls->nislots++; t->def = v2_obj(vc->obj); t->has_def = 1; a2_tm_put(&vm->global->cls->itraits, t);
        { V2 *s = (V2*)realloc(vm->global->slots, (size_t)vm->global->cls->nislots * sizeof(V2)); if (s) { vm->global->slots = s; vm->global->nslots = vm->global->cls->nislots; vm->global->slots[t->slot] = t->def; } } } }
    { extern void a2_regexp_init(VM2 *vm); extern void a2_xml_init(VM2 *vm); extern void a2_bytearray_init(VM2 *vm); extern void a2_bytearray_misc_init(VM2 *vm); a2_regexp_init(vm); a2_xml_init(vm); a2_bytearray_init(vm); a2_bytearray_misc_init(vm); { extern void a2_describe_init(VM2 *vm); a2_describe_init(vm); } }
    { extern void a2_natives_init(VM2 *vm); a2_natives_init(vm); }
    {   /* library classes written in ActionScript (as3/): definitions only, scripts run on first use */
        extern const u8 g_avm2_builtin_abc[]; extern const unsigned long g_avm2_builtin_abc_len;
        { extern int g_a2_loading_builtin; g_a2_loading_builtin = 1;
          if (avm2_load_abc(vm, g_avm2_builtin_abc, (size_t)g_avm2_builtin_abc_len, 1) && vm->nabcs) vm->abcs[vm->nabcs - 1]->is_builtin = 1;
          g_a2_loading_builtin = 0; }
    }
}
