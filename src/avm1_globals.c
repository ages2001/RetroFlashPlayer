/* avm1_globals.c - the AVM1 global object: Object, Function, the global
 * functions (escape, parseInt, setInterval, ASnative ...) and the
 * construction of a global environment.
 *
 * The class layout (property order, attributes, which functions carry a
 * prototype) follows the Flash Player as observed through the Ruffle test
 * suite.  Part of Flash's playerglobals is written in ActionScript; that
 * part is compiled into avm1_playerglobal.c and run after the native
 * classes exist (see run_playerglobal). */
#include "avm1_int.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define DE  PA_DONT_ENUM
#define DD  PA_DONT_DELETE
#define RO  PA_READ_ONLY
#define V6  PA_VERSION_6
#define V7  PA_VERSION_7
#define V8  PA_VERSION_8

AObj *this_or(AObj *self) { return self; }

/* ---------------------------------------------------------------- */
/* ASnative tables                                                  */

Value object_method(Act *a, AObj *self, int argc, Value *argv, int index);
Value globals_method(Act *a, AObj *self, int argc, Value *argv, int index);
Value asnew_method(Act *a, AObj *self, int argc, Value *argv, int index);
Value as_set_native_method(Act *a, AObj *self, int argc, Value *argv, int index);
extern Value array_method(Act *a, AObj *self, int argc, Value *argv, int index);
extern Value math_method(Act *a, AObj *self, int argc, Value *argv, int index);
extern Value date_method(Act *a, AObj *self, int argc, Value *argv, int index);
extern Value transform_method(Act *a, AObj *self, int argc, Value *argv, int index);
extern Value filter_method(Act *a, AObj *self, int argc, Value *argv, int index);
extern TableFn filter_table(int category);

static Value g_as_set_prop_flags(Act *a, AObj *self, int argc, Value *argv);
/* ASnative(1, 0): ASSetPropFlags */
static Value asnative1_method(Act *a, AObj *self, int argc, Value *argv, int index) {
    if (index == 0) return g_as_set_prop_flags(a, self, argc, argv);
    return v_undef();
}

static TableFn asnative_table(u32 category) {
    switch (category) {
    case 1: return asnative1_method;
    case 2: return asnew_method;
    case 4: return as_set_native_method;
    case 100: return globals_method;
    case 101: return object_method;
    case 103: { extern Value date_native_method(Act *a, AObj *self, int argc, Value *argv, int index); return date_native_method; }
    case 200: return math_method;
    case 252: return array_method;
    case 1106: return transform_method;
    case 251: { extern Value string_native(Act *a, AObj *self, int argc, Value *argv, int index); return string_native; }
    case 102: { extern Value string_native_old(Act *a, AObj *self, int argc, Value *argv, int index); return string_native_old; }
    case 106: { extern Value number_native(Act *a, AObj *self, int argc, Value *argv, int index); return number_native; }
    case 107: { extern Value boolean_native(Act *a, AObj *self, int argc, Value *argv, int index); return boolean_native; }
    case 666: { extern Value stage_native(Act *a, AObj *self, int argc, Value *argv, int index); return stage_native; }
    case 2106: { extern Value shared_object_native(Act *a, AObj *self, int argc, Value *argv, int index); return shared_object_native; }
    case 900: { extern Value mc_native900(Act *a, AObj *self, int argc, Value *argv, int index); return mc_native900; }
    case 901: { extern Value mc_native901(Act *a, AObj *self, int argc, Value *argv, int index); return mc_native901; }
    case 2700: { extern Value automation_capture_method(Act *a, AObj *self, int argc, Value *argv, int index); return automation_capture_method; }
    case 2800: { extern Value automation_generator_method(Act *a, AObj *self, int argc, Value *argv, int index); return automation_generator_method; }
    case 2900: { extern Value automation_config_method(Act *a, AObj *self, int argc, Value *argv, int index); return automation_config_method; }
    default: return filter_table((int)category);
    }
}

static int g_in_playerglobal;
static Value asnative_make(Act *a, u32 category, u32 index) {
    TableFn f = asnative_table(category);
    Value fp;
    AObj *fo;
    if (!f) return v_undef();
    fo = fn_table(a->vm, ENV(a), f, index > 65535u ? 65535 : (int)index);
    if (!fo) return v_undef();
    /* __proto__ is _global.Function.prototype and constructor _global.Function, whatever
       those are now (data properties only, Function itself is not called); neither is set
       when _global.Function is not a function (Flash 24+: an object still gets constructor) */
    fp = v_undef();
    pm_remove(&fo->props, a->vm->s_proto, 1);
    pm_remove(&fo->props, a->vm->s_constructor, 1);
    {
        AObj *c = a1_resolve_class(a, "Function");
        if (c && c->nk != NK_SUPER && (o_as_function(c) || avm1_player_major() >= 24)) {
            if (o_as_function(c) && a1_class_proto(a, c, &fp)) o_define(a->vm, fo, a->vm->s_proto, fp, DE | DD | PA_VERSION_6);
            /* (the built-in library's natives follow Ruffle's declarations: no constructor) */
            if (!g_in_playerglobal) o_define(a->vm, fo, a->vm->s_constructor, v_obj(c), DE | DD);
        }
    }
    return v_obj(fo);
}

static Value g_asnative(Act *a, AObj *self, int argc, Value *argv) {
    u32 cat, idx;
    (void)self;
    if (argc != 2) return v_undef();
    cat = v_u32(a, argv[0]); if (a->vm->err) return v_undef();
    idx = v_u32(a, argv[1]); if (a->vm->err) return v_undef();
    return asnative_make(a, cat, idx);
}

/* ASconstructor(category, index): the native as a class, with a fresh prototype */
static Value g_asconstructor(Act *a, AObj *self, int argc, Value *argv) {
    Value f; AObj *proto;
    f = g_asnative(a, self, argc, argv);
    if (f.t != V_OBJ) return f;
    proto = obj_new(a->vm, PROTOS(a)->object);
    if (!proto) return f;
    o_define(a->vm, proto, a->vm->s_constructor, f, DE);
    o_define(a->vm, f.u.o, a->vm->s_prototype, v_obj(proto), DE | DD);
    return f;
}

/* the main movie is SWF8 or older */
int avm1_root_pre9(VM *vm) { return !(vm->player && vm->player->movie && vm->player->movie->version >= 9); }

Value asnew_method(Act *a, AObj *self, int argc, Value *argv, int index) {
    (void)self; (void)argc; (void)argv;
    if (index == 0) return v_bool(a->id.reason == ER_CONSTRUCTOR);
    return v_undef();
}

Value as_set_native_method(Act *a, AObj *self, int argc, Value *argv, int index) {
    VM *vm = a->vm; AObj *obj; s32 major, minor = 0; AStr *props; u32 i, start;
    (void)self;
    if (index < 0 || index > 1 || argc < 3) return v_undef();
    obj = v_to_obj_or_bare(a, argv[0]); if (vm->err) return v_undef();
    major = v_i32(a, argv[1]); if (vm->err) return v_undef();
    props = v_str_of(a, argv[2]); if (vm->err) return v_undef();
    if (argc > 3) { minor = v_i32(a, argv[3]); if (vm->err) return v_undef(); }
    start = 0;
    for (i = 0; i <= props->len; i++) {
        u16 attr = 0; AStr *prop; u32 s;
        if (i < props->len && STR_AT(props, i) != ',') continue;
        s = start;
        if (s < i) {
            u16 c = STR_AT(props, s);
            if (c == '6') { s++; attr = PA_VERSION_6; }
            else if (c == '7') { s++; attr = PA_VERSION_7; }
            else if (c == '8') { s++; attr = PA_VERSION_8; }
            else if (c == '9') { s++; attr = PA_VERSION_9; }
            else if (c == '1') { s++; if (s < i && STR_AT(props, s) == '0') { s++; attr = PA_VERSION_10; } }
        }
        prop = str_slice(vm, props, s, i);
        if (index == 0) {
            Value f = asnative_make(a, (u32)major, (u32)minor);
            minor++;
            if (prop->len) {
                int had = o_has_own_property(a, obj, prop);
                o_set(a, obj, prop, f);
                if (vm->err) return v_undef();
                if (!had) o_set_attributes(vm, obj, prop, attr, 0);
            }
        } else {
            Value g = asnative_make(a, (u32)major, (u32)minor), st;
            minor++;
            st = asnative_make(a, (u32)major, (u32)minor);
            minor++;
            if (prop->len && g.t == V_OBJ) {
                o_add_property(vm, obj, prop, g.u.o, st.t == V_OBJ ? st.u.o : NULL, 0);
                o_set_attributes(vm, obj, prop, attr, 0);
            }
        }
        start = i + 1;
    }
    return v_undef();
}

/* ---------------------------------------------------------------- */
/* Object                                                           */

enum { OM_WATCH = 0, OM_UNWATCH, OM_ADD_PROPERTY, OM_VALUE_OF, OM_TO_STRING, OM_HAS_OWN_PROPERTY,
       OM_IS_PROTOTYPE_OF, OM_IS_PROPERTY_ENUMERABLE, OM_REGISTER_CLASS, OM_CONSTRUCTOR,
       OM_BROADCAST_MESSAGE = 12 };

static Value obj_constructor(Act *a, AObj *self, int argc, Value *argv) {
    Value v = arg(argc, argv, 0);
    if (v.t == V_UNDEF || v.t == V_NULL) return v_obj(self);
    /* boxing a primitive needs its class: without _global.String / Number / Boolean
       the older players construct nothing */
    if (avm1_fp_box_needs_class() && (v.t == V_STR || v.t == V_NUM || v.t == V_BOOL)) {
        AObj *c = a1_resolve_class(a, v.t == V_STR ? "String" : v.t == V_NUM ? "Number" : "Boolean");
        if (!c) return v_undef();
    }
    return v_obj(v_to_obj_or_bare(a, v));
}
static Value obj_function(Act *a, AObj *self, int argc, Value *argv) {
    Value v = arg(argc, argv, 0);
    (void)self;
    if (v.t == V_UNDEF || v.t == V_NULL) return v_obj(obj_bare(a->vm));
    return v_obj(v_to_obj_or_bare(a, v));
}

static Value obj_add_property(Act *a, AObj *self, int argc, Value *argv) {
    AStr *name; Value g, s;
    if (argc < 1) return v_bool(0);
    name = v_str_of(a, argv[0]); if (a->vm->err) return v_undef();
    g = arg(argc, argv, 1); s = arg(argc, argv, 2);
    if (g.t != V_OBJ || !o_as_function(g.u.o) || !name->len) return v_bool(0);
    if (s.t == V_OBJ && o_as_function(s.u.o)) o_add_property_case(a, self, name, g.u.o, s.u.o, 0);
    else if (s.t == V_NULL) o_add_property_case(a, self, name, g.u.o, NULL, 0);   /* no setter: assignments still store the underlying value */
    else return v_bool(0);
    return v_bool(1);
}

static Value obj_watch(Act *a, AObj *self, int argc, Value *argv) {
    AStr *name; AObj *cb;
    if (argc < 1) return v_bool(0);
    name = v_str_of(a, argv[0]); if (a->vm->err) return v_undef();
    cb = v_to_obj_or_bare(a, arg(argc, argv, 1)); if (a->vm->err) return v_undef();
    if (!o_as_function(cb)) return v_bool(0);
    o_watch(a, self, name, cb, arg(argc, argv, 2));
    return v_bool(1);
}

static Value obj_unwatch(Act *a, AObj *self, int argc, Value *argv) {
    AStr *name;
    if (argc < 1) return v_bool(0);
    name = v_str_of(a, argv[0]); if (a->vm->err) return v_undef();
    return v_bool(o_unwatch(a, self, name));
}

static int broadcast_internal(Act *a, AObj *self, int argc, Value *argv, AStr *method);
int avm1_broadcast(Act *a, AObj *self, int argc, Value *argv, const char *method) { return broadcast_internal(a, self, argc, argv, str_intern(a->vm, method)); }

/* Flash: true if a listener exists, false otherwise */
static int broadcast_internal(Act *a, AObj *self, int argc, Value *argv, AStr *method) {
    VM *vm = a->vm; Value lv; AObj *list; int len, i, has = 0; Value small[8], *items;
    lv = o_get(a, self, vm->s__listeners);
    if (vm->err || lv.t != V_OBJ) return 0;
    list = lv.u.o;
    len = o_length(a, list); if (vm->err) return 0;
    if (len < 0) len = 0;
    items = len <= 8 ? small : (Value*)malloc((size_t)len * sizeof(Value));
    if (!items) return 0;
    for (i = 0; i < len; i++) { items[i] = o_get_element(a, list, i); if (vm->err) { if (items != small) free(items); return 0; } }
    for (i = 0; i < len; i++) {
        AObj *lo = v_to_obj(a, items[i]);
        if (vm->err) break;
        if (!lo) continue;
        has = 1;
        if (!method->len) o_call(a, lo, items[i], argc, argv);    /* empty name: call the listener itself */
        else o_call_method(a, lo, method, argc, argv, ER_SPECIAL);
        if (vm->err) break;
    }
    if (items != small) free(items);
    return has;
}

Value broadcast_message(Act *a, AObj *broadcaster, AStr *method, int argc, Value *argv, int *had_listener) {
    int h = broadcast_internal(a, broadcaster, argc, argv, method);
    if (had_listener) *had_listener = h;
    return v_bool(h);
}

static Value obj_broadcast_message(Act *a, AObj *self, int argc, Value *argv) {
    AStr *ev;
    if (argc < 1) return v_undef();
    ev = v_str_of(a, argv[0]); if (a->vm->err) return v_undef();
    return broadcast_internal(a, self, argc - 1, argv + 1, ev) ? v_bool(1) : v_undef();
}

static Value obj_register_class(Act *a, AObj *self, int argc, Value *argv) {
    AObj *ctor = NULL; AStr *name; Env *e; MovieClip *bc = a->base_clip; int v;
    (void)self;
    if (argc < 2) return v_bool(0);
    if (argv[1].t == V_OBJ) { if (!o_as_function(argv[1].u.o)) return v_bool(0); ctor = argv[1].u.o; }
    else if (argv[1].t != V_NULL && argv[1].t != V_UNDEF) return v_bool(0);
    name = v_str_of(a, argv[0]); if (a->vm->err) return v_undef();
    v = bc ? clip_swf_version(bc) : a->swf_version;
    e = &a->vm->env[v >= 7 ? 1 : 0];
    /* case insensitive: Flash also replaces the key's spelling (AMF0 typed object names) */
    if (ctor) { Prop *p; if (v < 7) pm_remove(&e->registry, name, 0); p = pm_insert(a->vm, &e->registry, name, v >= 7); if (p) p->v = v_obj(ctor); }
    else pm_remove(&e->registry, name, v >= 7);
    return v_bool(1);
}

Value object_method(Act *a, AObj *self, int argc, Value *argv, int index) {
    switch (index) {
    case OM_CONSTRUCTOR: return obj_constructor(a, self, argc, argv);
    case OM_WATCH: return obj_watch(a, self, argc, argv);
    case OM_UNWATCH: return obj_unwatch(a, self, argc, argv);
    case OM_ADD_PROPERTY: return obj_add_property(a, self, argc, argv);
    case OM_VALUE_OF: return v_obj(self);
    case OM_TO_STRING: return v_str(str_intern(a->vm, o_as_function(self) ? "[type Function]" : "[object Object]"));
    case OM_HAS_OWN_PROPERTY: {
        AStr *n;
        if (argc < 1) return v_bool(0);
        n = v_str_of(a, argv[0]); if (a->vm->err) return v_undef();
        if (self && self->nk != NK_SUPER) { Prop *pp = pm_get(&self->props, n, CASE_SENSITIVE(a)); if (pp && (pp->attr & PA_NOT_OWN)) return v_bool(0); }
        return v_bool(o_has_own_property(a, self, n)); }
    case OM_IS_PROTOTYPE_OF: {
        AObj *ob;
        if (argc < 1) return v_bool(0);
        ob = v_to_obj_or_bare(a, argv[0]); if (a->vm->err) return v_undef();
        return v_bool(o_is_prototype_of(a, self, ob)); }
    case OM_IS_PROPERTY_ENUMERABLE: {
        AStr *n;
        if (argc < 1) return v_bool(0);
        n = v_str_of(a, argv[0]); if (a->vm->err) return v_undef();
        return v_bool(o_is_enumerable(a, self, n)); }
    case OM_REGISTER_CLASS: return obj_register_class(a, self, argc, argv);
    case OM_BROADCAST_MESSAGE: return obj_broadcast_message(a, self, argc, argv);
    default: return v_undef();
    }
}

static Value obj_ctor_native(Act *a, AObj *self, int argc, Value *argv) { return obj_constructor(a, self, argc, argv); }

static Value obj_to_locale_string(Act *a, AObj *self, int argc, Value *argv) {
    (void)argc; (void)argv;
    return v_str(v_str_of(a, v_obj(self)));
}

/* ASSetPropFlags(object, names | null, set, clear) */
static Value g_as_set_prop_flags(Act *a, AObj *self, int argc, Value *argv) {
    VM *vm = a->vm; AObj *o; u16 set, clear; double d;
    (void)self;
    if (argc < 1) return v_undef();
    o = v_to_obj_or_bare(a, argv[0]); if (vm->err) return v_undef();
    if (o->nk == NK_ARRAY && (o->flags & AOF_ARGUMENTS) && avm1_fp_sealed_arguments()) return v_undef();
    d = argc > 2 ? v_num_of(a, argv[2]) : 0; if (vm->err) return v_undef();
    set = (u16)(d != d ? 0 : (s32)(long)fmod(d, 65536.0));
    d = argc > 3 ? v_num_of(a, argv[3]) : 0; if (vm->err) return v_undef();
    clear = (u16)(d != d ? 0 : (s32)(long)fmod(d, 65536.0));
    if (argc < 2) return v_undef();
    if (argv[1].t == V_NULL) { o_set_attributes(vm, o, NULL, set, clear); return v_undef(); }
    {
        AStr *props = v_str_of(a, argv[1]); u32 i, start = 0;
        if (vm->err) return v_undef();
        for (i = 0; i <= props->len; i++) {
            if (i < props->len && STR_AT(props, i) != ',') continue;
            o_set_attributes(vm, o, str_slice(vm, props, start, i), set, clear);
            start = i + 1;
        }
    }
    return v_undef();
}

/* ---------------------------------------------------------------- */
/* Function                                                         */

static Value fn_constructor(Act *a, AObj *self, int argc, Value *argv) { (void)a; return argc > 0 ? argv[0] : v_obj(self); }
static Value fn_function(Act *a, AObj *self, int argc, Value *argv) { (void)self; return argc > 0 ? argv[0] : v_obj(obj_bare(a->vm)); }

static Value fn_call(Act *a, AObj *self, int argc, Value *argv) {
    AObj *t = NULL;
    if (argc > 0) { t = v_to_obj(a, argv[0]); if (a->vm->err) return v_undef(); }
    if (!t) t = act_global(a);
    if (!o_as_function(self)) return v_undef();
    return fn_exec(a, self, v_obj(t), 1, argc > 1 ? argc - 1 : 0, argc > 1 ? argv + 1 : NULL, ER_FUNCTION, 0);
}

static Value fn_apply(Act *a, AObj *self, int argc, Value *argv) {
    AObj *t = NULL; Value ao = arg(argc, argv, 1); int len = 0, i; Value small[16], *items, r;
    if (argc > 0) { t = v_to_obj(a, argv[0]); if (a->vm->err) return v_undef(); }
    if (!t) t = act_global(a);
    if (ao.t == V_OBJ) { len = o_length(a, ao.u.o); if (a->vm->err) return v_undef(); }
    if (len < 0) len = 0;
    items = len <= 16 ? small : (Value*)malloc((size_t)len * sizeof(Value));
    if (!items) return v_undef();
    for (i = 0; i < len; i++) {
        AObj *args = v_to_obj_or_bare(a, ao);
        if (a->vm->err) { if (items != small) free(items); return v_undef(); }
        items[i] = o_get(a, args, str_int(a->vm, i));
        if (a->vm->err) { if (items != small) free(items); return v_undef(); }
    }
    r = o_as_function(self) ? fn_exec(a, self, v_obj(t), 1, len, items, ER_FUNCTION, 0) : v_undef();
    if (items != small) free(items);
    return r;
}

/* ---------------------------------------------------------------- */
/* global functions (table 100)                                     */

AStr *escape_str(VM *vm, AStr *s) {
    const char *u = str_c(vm, s); SBuf b; static const char hx[] = "0123456789ABCDEF";
    sb_init(&b);
    for (; *u; u++) {
        unsigned char c = (unsigned char)*u;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) sb_unit(&b, c);
        else { sb_unit(&b, '%'); sb_unit(&b, (u16)hx[c >> 4]); sb_unit(&b, (u16)hx[c & 15]); }
    }
    return sb_done(vm, &b);
}

static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static AStr *unescape_str(VM *vm, AStr *s) {
    const char *u = str_c(vm, s); size_t n = strlen(u), k = 0; char *out = (char*)malloc(n + 1); int remain = 0; char hex[2]; int nh = 0; AStr *r;
    if (!out) return vm->s_empty;
    for (; *u; u++) {
        unsigned char c = (unsigned char)*u;
        if (c == '%') { remain = 2; nh = 0; continue; }
        if (remain > 0 && hexv(c) >= 0) {
            remain--; hex[nh++] = (char)c;
            if (remain == 0) { out[k++] = (char)(hexv(hex[0]) * 16 + hexv(hex[1])); nh = 0; }
            continue;
        }
        if (remain > 0) { remain = 0; nh = 0; continue; }
        if (c == '+') out[k++] = ' ';
        else out[k++] = (char)c;
    }
    r = str_utf8(vm, out, k);
    free(out);
    return r;
}

static int digit_in(u16 c, int radix) {
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
    else return -1;
    return d < radix ? d : -1;
}

Value parse_int_value(Act *a, Value sv, Value *radix_v) {
    int radix = 0, ignore_sign = 0, has_sign, zero, hex; AStr *s; u32 off, pos, i; double sign = 1, result = 0; int empty = 1;
    if (radix_v) {
        s32 r = v_i32(a, *radix_v); if (a->vm->err) return v_undef();
        if (r < 2 || r > 36) { double z = 0; return v_num(z / z); }
        radix = r;
    }
    s = v_str_of(a, sv); if (a->vm->err) return v_undef();
    has_sign = s->len > 0 && (STR_AT(s, 0) == '+' || STR_AT(s, 0) == '-');
    off = has_sign ? 1 : 0;
    zero = s->len > off && STR_AT(s, off) == '0';
    hex = zero && s->len > off + 1 && (STR_AT(s, off + 1) == 'x' || STR_AT(s, off + 1) == 'X');
    pos = 0;
    if (hex) {
        if (has_sign) {
            if (radix == 0 || radix <= 33) { double z = 0; return v_num(z / z); }
            ignore_sign = 1;
        } else { if (!radix) radix = 16; pos = 2; }
    } else if (zero && !radix_v) {
        int all = 1;
        for (i = 1; i < s->len; i++) { u16 c = STR_AT(s, i); if (c < '0' || c > '7') { all = 0; break; } }
        radix = all ? 8 : 10;
    } else if (!radix) radix = 10;
    while (pos < s->len) { u16 c = STR_AT(s, pos); if (c == '\t' || c == '\n' || c == '\r' || c == ' ') pos++; else break; }
    if (pos < s->len && (STR_AT(s, pos) == '+' || STR_AT(s, pos) == '-')) {
        if (!ignore_sign && STR_AT(s, pos) == '-') sign = -1;
        pos++;
    }
    for (; pos < s->len; pos++) {
        int d = digit_in(STR_AT(s, pos), radix);
        if (d < 0) break;
        result = result * radix + d; empty = 0;
    }
    if (empty) { double z = 0; return v_num(z / z); }
    return v_num(sign < 0 ? -result : result);
}

Value globals_method(Act *a, AObj *self, int argc, Value *argv, int index) {
    (void)self;
    switch (index) {
    case 0: if (argc < 1) return v_undef(); { AStr *s = v_str_of(a, argv[0]); if (a->vm->err) return v_undef(); return v_str(escape_str(a->vm, s)); }
    case 1: if (argc < 1) return v_undef(); { AStr *s = v_str_of(a, argv[0]); if (a->vm->err) return v_undef(); return v_str(unescape_str(a->vm, s)); }
    case 2: if (argc < 1) return v_undef(); return parse_int_value(a, argv[0], argc > 1 ? &argv[1] : NULL);
    case 3: if (argc < 1) return v_undef(); { AStr *s = v_str_of(a, argv[0]); if (a->vm->err) return v_undef(); return v_num(str_parse_float(s, 0)); }
    case 4: {
        AStr *s = v_str_of(a, arg(argc, argv, 0));
        if (a->vm->err) return v_undef();
        avm1_trace_str(a->vm, s);
        return v_undef(); }
    default: return v_undef();
    }
}

static Value g_is_finite(Act *a, AObj *self, int argc, Value *argv) {
    double d; (void)self;
    if (argc < 1) return v_bool(0);
    d = v_num_of(a, argv[0]);
    return v_bool(d - d == 0);
}
static Value g_is_nan(Act *a, AObj *self, int argc, Value *argv) {
    double d; (void)self;
    if (argc < 1) return v_bool(1);
    d = v_num_of(a, argv[0]);
    return v_bool(d != d);
}
static Value g_get_nan(Act *a, AObj *self, int argc, Value *argv) { double z = 0; (void)self; (void)argc; (void)argv; return a->swf_version > 4 ? v_num(z / z) : v_undef(); }
static Value g_get_infinity(Act *a, AObj *self, int argc, Value *argv) { double z = 0; (void)self; (void)argc; (void)argv; return a->swf_version > 4 ? v_num(1.0 / z) : v_undef(); }
static Value g_update_after_event(Act *a, AObj *self, int argc, Value *argv) { (void)a; (void)self; (void)argc; (void)argv; return v_undef(); }

/* timers */
#define TIMER_MIN_INTERVAL 10
static Value create_timer(Act *a, int argc, Value *argv, int is_timeout) {
    VM *vm = a->vm; Value iv; AObj *fn = NULL, *self = NULL; AStr *method = NULL; int first, i, slot = -1; s32 interval; Timer *t;
    if (argc < 1 || argv[0].t != V_OBJ) return v_undef();
    if (o_as_function(argv[0].u.o)) { fn = argv[0].u.o; iv = arg(argc, argv, 1); first = 2; }
    else {
        self = argv[0].u.o;
        method = v_str_of(a, arg(argc, argv, 1)); if (vm->err) return v_undef();
        iv = arg(argc, argv, 2); first = 3;
    }
    if (iv.t == V_UNDEF) return v_undef();
    interval = v_i32(a, iv); if (vm->err) return v_undef();
    for (i = 0; i < MAX_TIMERS; i++) if (!vm->timers[i].active) { slot = i; break; }
    if (slot < 0) return v_undef();
    if (interval < TIMER_MIN_INTERVAL) interval = TIMER_MIN_INTERVAL;
    t = &vm->timers[slot];
    memset(t, 0, sizeof(*t));
    t->active = 1; t->is_timeout = is_timeout;
    t->id = ++vm->next_timer_id;
    t->fn = fn; t->self = self; t->method = method;
    t->nargs = argc > first ? argc - first : 0;
    if (t->nargs) { t->args = (Value*)malloc((size_t)t->nargs * sizeof(Value)); if (t->args) memcpy(t->args, argv + first, (size_t)t->nargs * sizeof(Value)); else t->nargs = 0; }
    t->interval = (double)interval * 1000.0;
    t->next = vm->time_ms * 1000.0 + t->interval;
    return v_num(t->id);
}
static Value g_set_interval(Act *a, AObj *self, int argc, Value *argv) { (void)self; return create_timer(a, argc, argv, 0); }
static Value g_set_timeout(Act *a, AObj *self, int argc, Value *argv) { (void)self; return create_timer(a, argc, argv, 1); }
void avm1_timer_remove(VM *vm, int i) { Timer *t = &vm->timers[i]; free(t->args); memset(t, 0, sizeof(*t)); }
static Value g_clear_interval(Act *a, AObj *self, int argc, Value *argv) {
    s32 id = v_i32(a, arg(argc, argv, 0)); int i;
    (void)self;
    if (a->vm->err) return v_undef();
    for (i = 0; i < MAX_TIMERS; i++) if (a->vm->timers[i].active && a->vm->timers[i].id == id) { avm1_timer_remove(a->vm, i); break; }
    return v_undef();
}

/* ---------------------------------------------------------------- */
/* class construction helpers                                       */

/* a native class whose prototype inherits from super_proto (DeclContext::native_class) */
AObj *lib_class(VM *vm, Env *env, NativeFn ctor, NativeFn call, AObj *super_proto, int proto_first, AObj **proto_out) {
    AObj *proto = obj_bare(vm), *c;
    c = fn_class(vm, env, ctor, call, proto, proto_first);
    if (super_proto) o_define(vm, proto, vm->s_proto, v_obj(super_proto), DE | DD);
    if (proto_out) *proto_out = proto;
    return c;
}

/* a class with a plain (bytecode-like) constructor: `new` returns the new object */
AObj *lib_plain_class(VM *vm, Env *env, NativeFn fn, AObj *super_proto, int proto_first, AObj **proto_out) {
    return lib_class(vm, env, NULL, fn, super_proto, proto_first, proto_out);
}
/* Function.call / apply hand the call on: they do not become arguments.caller */
int avm1_fn_is_call_apply(NativeFn f) { return f == fn_call || f == fn_apply; }


/* a bare object inheriting from Object.prototype */
AObj *lib_object_of(VM *vm, Env *env) { return obj_new(vm, env->p.object); }

/* ---------------------------------------------------------------- */
/* the global environment                                           */

extern AObj *lib_movieclip(VM *vm, Env *env, AObj *object_proto);
extern AObj *lib_button(VM *vm, Env *env, AObj *object_proto);
extern AObj *lib_sound(VM *vm, Env *env, AObj *object_proto);
extern AObj *lib_stylesheet(VM *vm, Env *env, AObj *object_proto);
extern AObj *lib_textfield(VM *vm, Env *env, AObj *object_proto);
extern AObj *lib_textformat(VM *vm, Env *env, AObj *object_proto);
extern AObj *lib_color(VM *vm, Env *env, AObj *object_proto);
extern AObj *lib_xmlnode(VM *vm, Env *env, AObj *object_proto);
extern AObj *lib_xml(VM *vm, Env *env, AObj *xmlnode_proto);
extern AObj *lib_external_interface(VM *vm, Env *env, AObj *object_proto);
extern AObj *lib_loadvars(VM *vm, Env *env, AObj *object_proto);
extern AObj *lib_localconnection(VM *vm, Env *env, AObj *object_proto);
extern AObj *lib_colortransform(VM *vm, Env *env, AObj *object_proto);
extern AObj *lib_mcl(VM *vm, Env *env, AObj *object_proto);
extern AObj *lib_video(VM *vm, Env *env, AObj *object_proto);
extern AObj *lib_netstream(VM *vm, Env *env, AObj *object_proto);
extern AObj *lib_netconnection(VM *vm, Env *env, AObj *object_proto);
extern AObj *lib_xmlsocket(VM *vm, Env *env, AObj *object_proto);
extern AObj *lib_transform(VM *vm, Env *env, AObj *object_proto);
extern void  lib_filters(VM *vm, Env *env, AObj *object_proto, AObj *filters_pkg);
extern AObj *lib_bitmapdata(VM *vm, Env *env, AObj *object_proto);
extern AObj *lib_fileref(VM *vm, Env *env, AObj *object_proto);
extern AObj *lib_filereflist(VM *vm, Env *env, AObj *object_proto);
extern AObj *lib_sharedobject(VM *vm, Env *env, AObj *object_proto);
extern AObj *lib_selection(VM *vm, Env *env);
extern AObj *lib_camera(VM *vm, Env *env, AObj *object_proto);
extern AObj *lib_microphone(VM *vm, Env *env, AObj *object_proto);
extern AObj *lib_printjob(VM *vm, Env *env, AObj *object_proto);
extern AObj *lib_textsnapshot(VM *vm, Env *env, AObj *object_proto);
extern AObj *lib_system(VM *vm, Env *env, AObj **security, AObj **capabilities, AObj **ime, AObj **product);
extern AObj *lib_mouse(VM *vm, Env *env);
extern AObj *lib_key(VM *vm, Env *env);
extern AObj *lib_stage(VM *vm, Env *env);
extern AObj *lib_accessibility(VM *vm, Env *env);
extern AObj *lib_textrenderer(VM *vm, Env *env, AObj *object_proto);
extern AObj *lib_soundcodec(VM *vm, Env *env);
extern AObj *lib_empty_class(VM *vm, Env *env, AObj *object_proto);

static Value classv(AObj *c) { return c ? v_obj(c) : v_undef(); }

static void defv(VM *vm, AObj *o, const char *name, Value v, u16 attr) { o_define(vm, o, str_intern(vm, name), v, attr); }
static void defm(VM *vm, Env *env, AObj *o, const char *name, NativeFn f, u16 attr) { o_define(vm, o, str_intern(vm, name), v_obj(fn_native(vm, env, f, NULL)), attr); }
static void deft(VM *vm, Env *env, AObj *o, const char *name, TableFn f, int index, u16 attr) { o_define(vm, o, str_intern(vm, name), v_obj(fn_table(vm, env, f, index)), attr); }
static void defp(VM *vm, Env *env, AObj *o, const char *name, NativeFn g, NativeFn s, u16 attr) { o_add_property(vm, o, str_intern(vm, name), fn_native(vm, env, g, NULL), s ? fn_native(vm, env, s, NULL) : NULL, attr); }

extern const unsigned char avm1_playerglobal[];
extern const unsigned long avm1_playerglobal_len;
static void run_playerglobal(VM *vm, Env *env, int swf_version);


/* ---------------------------------------------------------------- */
/* the Linux player's namespace                                     */
/* Gnash's expectations come from the Linux Flash Player: its globals hold a
 * few more (and a few less) members than the Windows player's, in another
 * order (gnash argstest-v6/7/8 enumerate everything).  Each line: object path,
 * player-version set (bit 0: 9, 1: 10, 2: 11/12/14, 3: 13/15-18, 4: 19+) and
 * the members in enumeration order (newest first) with their types. */
static const char *const linux_ns[] = {
    "_global|1|TextSnapshot:f PrintJob:f MovieClipLoader:f textRenderer:f flash:u System:o Accessibility:o Video:f Stage:o TextFormat:f TextField:f Button:f Key:o Mouse:o Selection:o LoadVars:f XML:f XMLNode:f Sound:f Math:o Array:f String:f Date:f Boolean:f Number:f o:z showRedrawRegions:f setTimeout:f clearInterval:f clearTimeout:f setInterval:f isFinite:f isNaN:f updateAfterEvent:f trace:f parseFloat:f parseInt:f unescape:f escape:f ASSetNativeAccessor:f ASSetNative:f ASSetPropFlags:f RemoteLSOUsage:f AssetCache:f AsSetupError:f Error:f ContextMenu:f ContextMenuItem:f LocalConnection:f SharedObject:f Microphone:f Camera:f NetStream:f NetConnection:f Color:f AsBroadcaster:f XMLSocket:f MovieClip:f Infinity:n NaN:n enableDebugConsole:f Function:f Object:f ASconstructor:f ASnative:f",
    "_global|2|TextSnapshot:f PrintJob:f MovieClipLoader:f LocalConnection:f textRenderer:f flash:u System:o SoundCodec:o Accessibility:o Video:f Stage:o TextFormat:f TextField:f Button:f Key:o Mouse:o Selection:o LoadVars:f XML:f XMLNode:f Sound:f Math:o Array:f String:f Date:f Boolean:f Number:f o:z showRedrawRegions:f setTimeout:f clearInterval:f clearTimeout:f setInterval:f isFinite:f isNaN:f updateAfterEvent:f trace:f parseFloat:f parseInt:f unescape:f escape:f ASSetNativeAccessor:f ASSetNative:f ASSetPropFlags:f RemoteLSOUsage:f AssetCache:f AsSetupError:f Error:f ContextMenu:f ContextMenuItem:f SharedObject:f Microphone:f Camera:f NetStream:f NetConnection:f Color:f AsBroadcaster:f XMLSocket:f MovieClip:f Infinity:n NaN:n enableDebugConsole:f Function:f Object:f ASconstructor:f ASnative:f",
    "_global|28|TextSnapshot:f PrintJob:f MovieClipLoader:f LocalConnection:f textRenderer:f flash:u System:o SoundCodec:o Accessibility:o Video:f Stage:o TextFormat:f TextField:f Button:f Key:o Mouse:o Selection:o LoadVars:f XML:f XMLNode:f Sound:f Math:o Array:f String:f Date:f Boolean:f Number:f o:z clearRequestHeaders:f addRequestHeader:f showRedrawRegions:f setTimeout:f clearInterval:f clearTimeout:f setInterval:f isFinite:f isNaN:f updateAfterEvent:f trace:f parseFloat:f parseInt:f unescape:f escape:f ASSetNativeAccessor:f ASSetNative:f ASSetPropFlags:f RemoteLSOUsage:f AssetCache:f AsSetupError:f Error:f ContextMenu:f ContextMenuItem:f SharedObject:f Microphone:f Camera:f NetStream:f NetConnection:f Color:f AsBroadcaster:f XMLSocket:f MovieClip:f Infinity:n NaN:n enableDebugConsole:f Function:f Object:f ASconstructor:f ASnative:f",
    "_global.Accessibility|31|updateProperties:f sendEvent:f isActive:f",
    "_global.Key|31|_listeners:o removeListener:f addListener:f broadcastMessage:f isAccessible:f isToggled:f isDown:f getCode:f getAscii:f ALT:n ENTER:n SPACE:n UP:n DOWN:n LEFT:n RIGHT:n PGUP:n PGDN:n HOME:n END:n TAB:n CONTROL:n SHIFT:n ESCAPE:n INSERT:n DELETEKEY:n BACKSPACE:n CAPSLOCK:n",
    "_global.Math|31|pow:f acos:f asin:f atan:f ceil:f floor:f random:f round:f sqrt:f log:f exp:f tan:f atan2:f cos:f sin:f max:f min:f abs:f E:n LN10:n LN2:n LOG10E:n LOG2E:n PI:n SQRT1_2:n SQRT2:n",
    "_global.Mouse|3|_listeners:o removeListener:f addListener:f broadcastMessage:f hide:f show:f",
    "_global.Mouse|28|_listeners:o removeListener:f addListener:f broadcastMessage:f setTrailerMode:f setTrailerPosition:f setTrailer:f hide:f show:f",
    "_global.Selection|31|_listeners:o removeListener:f addListener:f broadcastMessage:f setSelection:f setFocus:f getFocus:f getCaretIndex:f getEndIndex:f getBeginIndex:f",
    "_global.SoundCodec|30|NELLYMOSER:s SPEEX:s",
    "_global.Stage|1|_listeners:o removeListener:f addListener:f broadcastMessage:f fullScreenWidth:u fullScreenHeight:u fullScreenSourceRect:u displayState:s showMenu:b width:n height:n scaleMode:s align:s",
    "_global.Stage|30|_listeners:o removeListener:f addListener:f broadcastMessage:f wmodeGPU:u fullScreenWidth:u fullScreenHeight:u fullScreenSourceRect:u displayState:s showMenu:b width:n height:n scaleMode:s align:s",
    "_global.System|1|IME:o setClipboard:f security:o useCodepage:b exactSettings:b showSettings:f Product:f capabilities:o",
    "_global.System|30|privateBytes:n IME:o setClipboard:f security:o useCodepage:b exactSettings:b showSettings:f Product:f capabilities:o",
    "_global.System.IME|3|_listeners:o removeListener:f addListener:f broadcastMessage:f doConversion:u setCompositionString:u setConversionMode:u getConversionMode:u setEnabled:u getEnabled:u ALPHANUMERIC_FULL:s ALPHANUMERIC_HALF:s CHINESE:s JAPANESE_HIRAGANA:s JAPANESE_KATAKANA_FULL:s JAPANESE_KATAKANA_HALF:s KOREAN:s UNKNOWN:s",
    "_global.System.IME|28|_listeners:o removeListener:f addListener:f broadcastMessage:f doConversion:f setCompositionString:f setConversionMode:f getConversionMode:f setEnabled:f getEnabled:f ALPHANUMERIC_FULL:s ALPHANUMERIC_HALF:s CHINESE:s JAPANESE_HIRAGANA:s JAPANESE_KATAKANA_FULL:s JAPANESE_KATAKANA_HALF:s KOREAN:s UNKNOWN:s",
    "_global.System.capabilities|1|_internal:n hasIME:b language:s os:s manufacturer:s isEmbeddedInAcrobat:b windowlessDisable:b localFileReadDisable:b avHardwareDisable:b playerType:s isDebugger:b hasScreenBroadcast:b hasScreenPlayback:b hasPrinting:b hasEmbeddedVideo:b hasStreamingVideo:b hasStreamingAudio:b version:s serverString:s hasAudio:b hasMP3:b hasAudioEncoder:b hasVideoEncoder:b hasTLS:b screenResolutionX:n screenResolutionY:n screenDPI:n screenColor:s pixelAspectRatio:n hasAccessibility:b",
    "_global.System.capabilities|14|_internal:n supports64BitProcesses:b supports32BitProcesses:b hasIME:b language:s cpuArchitecture:s os:s manufacturer:s isEmbeddedInAcrobat:b maxLevelIDC:s windowlessDisable:b localFileReadDisable:b avHardwareDisable:b playerType:s isDebugger:b hasScreenBroadcast:b hasScreenPlayback:b hasPrinting:b hasEmbeddedVideo:b hasStreamingVideo:b hasStreamingAudio:b version:s serverString:s hasAudio:b hasMP3:b hasAudioEncoder:b hasVideoEncoder:b hasTLS:b screenResolutionX:n screenResolutionY:n screenDPI:n screenColor:s pixelAspectRatio:n hasAccessibility:b",
    "_global.System.capabilities|16|_internal:n supports64BitProcesses:b supports32BitProcesses:b hasIME:b language:s cpuAddressSize:n cpuArchitecture:s os:s manufacturer:s isEmbeddedInAcrobat:b maxLevelIDC:s windowlessDisable:b localFileReadDisable:b avHardwareDisable:b playerType:s isDebugger:b hasScreenBroadcast:b hasScreenPlayback:b hasPrinting:b hasEmbeddedVideo:b hasStreamingVideo:b hasStreamingAudio:b version:s serverString:s hasAudio:b hasMP3:b hasAudioEncoder:b hasVideoEncoder:b hasTLS:b screenResolutionX:n screenResolutionY:n screenDPI:n screenColor:s pixelAspectRatio:n hasAccessibility:b",
    "_global.System.security|31|PolicyFileResolver:f sandboxType:s escapeDomain:f chooseLocalSwfPath:f loadPolicyFile:f allowInsecureDomain:f allowDomain:f __constructor__:f",
    "_global.flash|28|automation:o external:o net:o geom:o filters:o display:o text:o",
    "_global.flash.automation|28|Configuration:f ActionGenerator:f StageCapture:f",
    "_global.flash.display|28|BitmapData:f",
    "_global.flash.external|28|ExternalInterface:f",
    "_global.flash.filters|28|DisplacementMapFilter:f ColorMatrixFilter:f ConvolutionFilter:f GradientBevelFilter:f GradientGlowFilter:f BevelFilter:f GlowFilter:f BlurFilter:f DropShadowFilter:f BitmapFilter:f",
    "_global.flash.geom|28|Transform:f ColorTransform:f Matrix:f Point:f Rectangle:f",
    "_global.flash.net|28|FileReferenceList:f FileReference:f",
    "_global.flash.text|28|TextRenderer:f",
    NULL
};

int avm1_linux_ns_cat(void) {
    int v = avm1_player_major();
    if (v <= 9) return 0;
    if (v == 10) return 1;
    if (v == 11 || v == 12 || v == 14) return 2;
    if (v <= 18) return 3;
    return 4;
}

/* make a property the newest one of its object */
void avm1_prop_to_front(VM *vm, AObj *o, AStr *k) {
    Prop *p = pm_get(&o->props, k, 1), save;
    if (!p) return;
    save = *p;
    pm_remove(&o->props, k, 1);
    p = pm_insert(vm, &o->props, save.key, 1);
    if (p) *p = save;
}

static AObj *ns_child(VM *vm, AObj *o, const char *name, int len) {
    char b[64]; Prop *p;
    if (!o || len <= 0 || len >= (int)sizeof(b)) return NULL;
    memcpy(b, name, (size_t)len); b[len] = 0;
    p = pm_get(&o->props, str_intern(vm, b), 1);
    return p && !p->getter && p->v.t == V_OBJ ? p->v.u.o : NULL;
}

static Value ns_noop(Act *a, AObj *s, int c, Value *v) { (void)a; (void)s; (void)c; (void)v; return v_undef(); }

static Value ns_null_ctor(Act *a, AObj *s, int c, Value *v) { (void)a; (void)s; (void)c; (void)v; return v_null(); }
static void ns_set_attr(VM *vm, AObj *o, const char *name, u16 add) { Prop *p = o ? pm_get(&o->props, str_intern(vm, name), 1) : NULL; if (p) p->attr |= add; }

static void ns_give_prototype(VM *vm, Env *env, AObj *o, const char *name) {
    Prop *tp = pm_get(&o->props, str_intern(vm, name), 1);
    if (tp && tp->v.t == V_OBJ && !pm_get(&tp->v.u.o->props, vm->s_prototype, 1)) {
        AObj *pr = obj_new(vm, env->p.object);
        o_define(vm, pr, vm->s_constructor, tp->v, PA_DONT_ENUM);
        o_define(vm, tp->v.u.o, vm->s_prototype, v_obj(pr), PA_DONT_ENUM);
    }
}
static void ns_proto_swf7(VM *vm, AObj *g, const char *name) {
    Prop *p = pm_get(&g->props, str_intern(vm, name), 1), *pp;
    if (!p || p->v.t != V_OBJ) return;
    pp = pm_get(&p->v.u.o->props, vm->s_prototype, 1);
    if (!pp || pp->v.t != V_OBJ) return;
    pp = pm_get(&pp->v.u.o->props, vm->s_proto, 1);
    if (pp) pp->attr |= PA_PROTO_SWF7;
}

static void linux_namespace(VM *vm, Env *env, AObj *g) {
    int cat = avm1_linux_ns_cat(), li;
    AObj *sys = ns_child(vm, g, "System", 6), *o;
    Act a;
    memset(&a, 0, sizeof(a));
    a.vm = vm; a.swf_version = (u8)(env == &vm->env[1] ? 8 : 6); a.scope = env->gscope; a.pool = vm->cpool;
    a.base_clip = vm->player ? vm->player->root : NULL; a.target_clip = a.base_clip; a.has_target = a.base_clip != NULL;
    a.this_ = v_null(); a.id.reason = ER_SPECIAL;
    /* functions the Linux player writes in ActionScript have a prototype */
    ns_give_prototype(vm, env, env->p.object, "toLocaleString");

    ns_proto_swf7(vm, g, "PrintJob"); ns_proto_swf7(vm, g, "MovieClipLoader");
    ns_proto_swf7(vm, g, "ContextMenu"); ns_proto_swf7(vm, g, "ContextMenuItem");
    /* _global.trace prints nothing */
    if (pm_get(&g->props, str_intern(vm, "trace"), 1))
        o_define(vm, g, str_intern(vm, "trace"), v_obj(fn_native(vm, env, ns_noop, NULL)), PA_DONT_ENUM);
    /* "new textRenderer()" gives nothing */
    if (pm_get(&g->props, str_intern(vm, "textRenderer"), 1))
        o_define(vm, g, str_intern(vm, "textRenderer"), v_obj(fn_native(vm, env, ns_noop, ns_null_ctor)), 0);
    if (sys) {
        if ((o = ns_child(vm, sys, "IME", 3)) != NULL) {
            static const char *const m[] = { "doConversion", "setCompositionString", "setConversionMode", "getConversionMode", "setEnabled", "getEnabled", NULL };
            int k; for (k = 0; m[k]; k++) ns_set_attr(vm, o, m[k], PA_VERSION_8);
        }
        if ((o = ns_child(vm, sys, "security", 8)) != NULL) {
            AObj *ab = ns_child(vm, g, "AsBroadcaster", 13), *proto, *c;
            ns_set_attr(vm, o, "allowInsecureDomain", PA_VERSION_7);
            o_define(vm, o, str_intern(vm, "chooseLocalSwfPath"), v_obj(fn_native(vm, env, ns_noop, NULL)), 0);
            /* PolicyFileResolver: a class whose prototype is an AsBroadcaster */
            c = lib_class(vm, env, NULL, ns_noop, env->p.object, 1, &proto);
            o_define(vm, proto, str_intern(vm, "resolve"), v_obj(fn_native(vm, env, ns_noop, NULL)), 0);
            if (ab) { Value pv = v_obj(proto); o_call_method(&a, ab, str_intern(vm, "initialize"), 1, &pv, ER_SPECIAL); vm->err = ERR_NONE; }
            o_define(vm, o, str_intern(vm, "PolicyFileResolver"), v_obj(c), 0);
        }
    }
    if (sys) {
        /* System.security is made by "new Object()" (SWF6+ code: __constructor__ is set) */
        if ((o = ns_child(vm, sys, "security", 8)) != NULL) {
            o_define(vm, o, vm->s___constructor__, v_obj(env->p.object_ctor), PA_DONT_ENUM);
            avm1_prop_to_front(vm, o, vm->s___constructor__);
        }
        if ((o = ns_child(vm, sys, "capabilities", 12)) != NULL) {
            o_define(vm, o, str_intern(vm, "_internal"), v_num(0), PA_DONT_ENUM);
            if (cat == 4) o_define(vm, o, str_intern(vm, "cpuAddressSize"), v_num(32), PA_DONT_ENUM);
        }
        if (cat >= 1) o_define(vm, sys, str_intern(vm, "privateBytes"), v_num(0), PA_DONT_ENUM);
    }
    for (li = 0; linux_ns[li]; li++) {
        const char *s = linux_ns[li], *bar = strchr(s, '|'), *q, *e;
        AStr *names[160]; char types[160]; int n = 0, i, mask;
        AObj *o = g;
        mask = atoi(bar + 1);
        if (!(mask & (1 << cat))) continue;
        /* resolve the path (after "_global") */
        for (q = s + 7; q < bar && *q == '.'; q = e) {
            q++; e = q; while (e < bar && *e != '.') e++;
            o = ns_child(vm, o, q, (int)(e - q));
        }
        if (!o) continue;
        q = strchr(bar + 1, '|') + 1;
        while (*q && n < 160) {
            char b[64]; int l = 0;
            while (*q == ' ') q++;
            if (!*q) break;
            while (*q && *q != ':' && l < 63) b[l++] = *q++;
            b[l] = 0;
            types[n] = q[0] == ':' ? q[1] : 'u';
            names[n++] = str_intern(vm, b);
            while (*q && *q != ' ') q++;
        }
        /* drop the members the Linux player does not have */
        for (i = 0; i < o->props.n; i++) {
            Prop *p = &o->props.items[i]; int j;
            if (!p->key) continue;
            for (j = 0; j < n; j++) if (str_eq(p->key, names[j])) break;
            if (j < n || str_eq(p->key, vm->s_proto) || str_eq(p->key, vm->s_constructor) || str_eq(p->key, vm->s___constructor__)) continue;
            if (getenv("FLASHNT_NSDEBUG")) fprintf(stderr, "ns: hide %.*s.%s\n", (int)(bar - s), s, str_c(vm, p->key));
            /* a method the Linux player does not have goes; other members (native
               properties, lists) only stop being enumerated */
            if (!p->getter && p->v.t == V_OBJ && p->v.u.o->nk == NK_FUNCTION) { pm_remove(&o->props, p->key, 1); i = -1; }
            else p->attr |= PA_NEVER_ENUM;
        }
        /* add the missing ones, then order them all */
        for (i = n - 1; i >= 0; i--) {
            if (!pm_get(&o->props, names[i], 1)) {
                Value v = v_undef();
                if (types[i] == 'f') v = v_obj(fn_native(vm, env, ns_noop, NULL));
                else if (types[i] == 'z') v = v_null();
                else if (types[i] != 'u' && getenv("FLASHNT_NSDEBUG")) fprintf(stderr, "ns: missing %.*s.%s (%c)\n", (int)(bar - s), s, str_c(vm, names[i]), types[i]);
                o_define(vm, o, names[i], v, PA_DONT_ENUM);
            }
            avm1_prop_to_front(vm, o, names[i]);
        }
    }
    ns_give_prototype(vm, env, g, "clearRequestHeaders");
    ns_give_prototype(vm, env, g, "addRequestHeader");
    {   /* the filters' prototypes are BitmapFilters: no constructor of their own */
        AObj *fl = ns_child(vm, g, "flash", 5), *fs = fl ? ns_child(vm, fl, "filters", 7) : NULL; int i;
        if (fs) for (i = 0; i < fs->props.n; i++) {
            Prop *fp = &fs->props.items[i], *pp;
            if (!fp->key || fp->v.t != V_OBJ || !strcmp(str_c(vm, fp->key), "BitmapFilter")) continue;
            pp = pm_get(&fp->v.u.o->props, vm->s_prototype, 1);
            if (pp && pp->v.t == V_OBJ) pm_remove(&pp->v.u.o->props, vm->s_constructor, 1);
        }
        /* flash.text.TextRenderer is _global.textRenderer */
        if (fl && (fs = ns_child(vm, fl, "text", 4)) != NULL && pm_get(&fs->props, str_intern(vm, "TextRenderer"), 1)) {
            Prop *tr = pm_get(&g->props, str_intern(vm, "textRenderer"), 1);
            if (tr) o_define(vm, fs, str_intern(vm, "TextRenderer"), tr->v, 0);
        }
    }
    if (cat >= 3) {   /* 13+: these classes build their objects natively (no __constructor__) */
        static const char *const nat[] = { "PrintJob", "MovieClipLoader", "LocalConnection", "TextFormat", "TextField", "Button", "LoadVars", "Sound", "Array", "String", "Date", "Boolean", "Number", "NetConnection", NULL };
        int k; env->nlnx = 0;
        for (k = 0; nat[k] && env->nlnx < 24; k++) { AObj *c = ns_child(vm, g, nat[k], (int)strlen(nat[k])); if (c) env->lnx_native[env->nlnx++] = c; }
        env->lnx_xml = ns_child(vm, g, "XML", 3);
        {   AObj *fl = ns_child(vm, g, "flash", 5), *pk, *c;
            if (fl && (pk = ns_child(vm, fl, "net", 3)) != NULL && (c = ns_child(vm, pk, "FileReference", 13)) != NULL && env->nlnx < 24) env->lnx_native[env->nlnx++] = c;
            if (fl && (pk = ns_child(vm, fl, "geom", 4)) != NULL && (c = ns_child(vm, pk, "ColorTransform", 14)) != NULL && env->nlnx < 24) env->lnx_native[env->nlnx++] = c;
        }
    }
    {   /* XML.prototype is an XMLNode: no constructor of its own */
        AObj *x = ns_child(vm, g, "XML", 3); Prop *pp = x ? pm_get(&x->props, vm->s_prototype, 1) : NULL;
        if (pp && pp->v.t == V_OBJ) pm_remove(&pp->v.u.o->props, vm->s_constructor, 1);
    }
}


/* the Linux player's class prototypes: path of the class, player-version set,
 * SWF set (1: the SWF6 globals, 2: the SWF7+ ones), the members an instance
 * enumerates (prototype chain first, newest first) */
static const char *const linux_protos[] = {
    "_global.flash.net.FileReference|31|2|deleteConvertedPPT convertToPPT cancel download upload browse removeListener addListener broadcastMessage",
    "_global.Array|31|1|toLocaleString isPropertyEnumerable isPrototypeOf hasOwnProperty valueOf addProperty unwatch watch sortOn reverse sort toString splice join slice unshift shift concat pop push",
    "_global.Array|31|2|toLocaleString isPropertyEnumerable isPrototypeOf hasOwnProperty valueOf addProperty unwatch watch sortOn reverse sort toString splice join slice unshift shift concat pop push",
    "_global.Boolean|31|1|toLocaleString isPropertyEnumerable isPrototypeOf hasOwnProperty addProperty unwatch watch toString valueOf",
    "_global.Boolean|31|2|toLocaleString isPropertyEnumerable isPrototypeOf hasOwnProperty addProperty unwatch watch toString valueOf",
    "_global.Button|31|1|tabIndex blendMode cacheAsBitmap filters scale9Grid getDepth enabled useHandCursor",
    "_global.Button|31|2|tabIndex blendMode cacheAsBitmap filters scale9Grid getDepth enabled useHandCursor",
    "_global.Camera|31|1|setCursor setLoopback setMotionLevel setKeyFrameInterval setQuality setMode",
    "_global.Camera|31|2|setCursor setLoopback setMotionLevel setKeyFrameInterval setQuality setMode",
    "_global.Color|31|1|getTransform getRGB setTransform setRGB",
    "_global.Color|31|2|getTransform getRGB setTransform setRGB",
    "_global.ContextMenu|31|1|hideBuiltInItems copy",
    "_global.ContextMenu|31|2|hideBuiltInItems copy",
    "_global.ContextMenuItem|31|1|copy",
    "_global.ContextMenuItem|31|2|copy",
    "_global.Date|31|1|toLocaleString isPropertyEnumerable isPrototypeOf hasOwnProperty addProperty unwatch watch valueOf setUTCMilliseconds setUTCSeconds setUTCMinutes setUTCHours setUTCDate setUTCMonth setUTCFullYear getUTCMilliseconds getUTCSeconds getUTCMinutes getUTCHours getUTCDay getUTCDate getUTCMonth getUTCYear getUTCFullYear setYear toString getTimezoneOffset setTime getTime setMilliseconds setSeconds setMinutes setHours setDate setMonth setFullYear getMilliseconds getSeconds getMinutes getHours getDay getDate getMonth getYear getFullYear",
    "_global.Date|31|2|toLocaleString isPropertyEnumerable isPrototypeOf hasOwnProperty addProperty unwatch watch valueOf setUTCMilliseconds setUTCSeconds setUTCMinutes setUTCHours setUTCDate setUTCMonth setUTCFullYear getUTCMilliseconds getUTCSeconds getUTCMinutes getUTCHours getUTCDay getUTCDate getUTCMonth getUTCYear getUTCFullYear setYear toString getTimezoneOffset setTime getTime setMilliseconds setSeconds setMinutes setHours setDate setMonth setFullYear getMilliseconds getSeconds getMinutes getHours getDay getDate getMonth getYear getFullYear",
    "_global.Error|31|1|toLocaleString isPropertyEnumerable isPrototypeOf hasOwnProperty valueOf addProperty unwatch watch toString name message",
    "_global.Error|31|2|toLocaleString isPropertyEnumerable isPrototypeOf hasOwnProperty valueOf addProperty unwatch watch toString name message",
    "_global.Function|31|1|apply call",
    "_global.Function|31|2|apply call",
    "_global.LoadVars|31|1|toLocaleString isPropertyEnumerable isPrototypeOf hasOwnProperty valueOf addProperty unwatch watch addRequestHeader onData onLoad contentType toString getBytesTotal getBytesLoaded decode sendAndLoad send load",
    "_global.LoadVars|31|2|toLocaleString isPropertyEnumerable isPrototypeOf hasOwnProperty valueOf addProperty unwatch watch addRequestHeader onData onLoad contentType toString getBytesTotal getBytesLoaded decode sendAndLoad send load",
    "_global.LocalConnection|1|1|domain close send connect",
    "_global.LocalConnection|30|1|isPerUser domain close send connect",
    "_global.LocalConnection|1|2|domain close send connect",
    "_global.LocalConnection|30|2|isPerUser domain close send connect",
    "_global.Microphone|1|1|setUseEchoSuppression setGain setRate setSilenceLevel",
    "_global.Microphone|30|1|setEncodeQuality setFramesPerPacket setCodec setUseEchoSuppression setGain setRate setSilenceLevel",
    "_global.Microphone|1|2|setUseEchoSuppression setGain setRate setSilenceLevel",
    "_global.Microphone|30|2|setEncodeQuality setFramesPerPacket setCodec setUseEchoSuppression setGain setRate setSilenceLevel",
    "_global.MovieClip|31|1|getTextSnapshot createTextField beginBitmapFill beginMeshFill lineGradientStyle clear endFill lineStyle curveTo lineTo moveTo beginGradientFill beginFill createEmptyMovieClip getRect attachBitmap getSWFVersion getInstanceAtDepth getNextHighestDepth stopDrag startDrag removeMovieClip duplicateMovieClip gotoAndStop gotoAndPlay prevFrame nextFrame stop play setMask getDepth attachVideo attachAudio getBytesLoaded getBytesTotal getBounds hitTest globalToLocal localToGlobal swapDepths attachMovie loadMovie loadVariables unloadMovie getURL meth scale9Grid forceSmoothing blendMode transform filters scrollRect opaqueBackground cacheAsBitmap _lockroot tabIndex enabled useHandCursor",
    "_global.MovieClip|31|2|getTextSnapshot createTextField beginBitmapFill beginMeshFill lineGradientStyle clear endFill lineStyle curveTo lineTo moveTo beginGradientFill beginFill createEmptyMovieClip getRect attachBitmap getSWFVersion getInstanceAtDepth getNextHighestDepth stopDrag startDrag removeMovieClip duplicateMovieClip gotoAndStop gotoAndPlay prevFrame nextFrame stop play setMask getDepth attachVideo attachAudio getBytesLoaded getBytesTotal getBounds hitTest globalToLocal localToGlobal swapDepths attachMovie loadMovie loadVariables unloadMovie getURL meth scale9Grid forceSmoothing blendMode transform filters scrollRect opaqueBackground cacheAsBitmap _lockroot tabIndex enabled useHandCursor",
    "_global.MovieClipLoader|7|1|removeListener addListener broadcastMessage unloadClip getProgress loadClip",
    "_global.MovieClipLoader|24|1|_listeners removeListener addListener broadcastMessage unloadClip getProgress loadClip",
    "_global.MovieClipLoader|7|2|removeListener addListener broadcastMessage unloadClip getProgress loadClip",
    "_global.MovieClipLoader|24|2|_listeners removeListener addListener broadcastMessage unloadClip getProgress loadClip",
    "_global.NetConnection|1|1|addHeader call close connect usingTLS connectedProxyType proxyType",
    "_global.NetConnection|30|1|farNonce nearNonce addHeader call close connect protocol usingTLS connectedProxyType proxyType",
    "_global.NetConnection|1|2|addHeader call close connect usingTLS connectedProxyType proxyType",
    "_global.NetConnection|30|2|farNonce nearNonce addHeader call close connect protocol usingTLS connectedProxyType proxyType",
    "_global.NetStream|1|1|checkPolicyFile setBufferTime send attachVideo attachAudio close seek pause receiveVideo receiveAudio play publish",
    "_global.NetStream|30|1|backBufferTime maxPauseBufferTime checkPolicyFile getInfo setBufferTime send attachVideo attachAudio close onPeerConnect seek pause receiveVideo receiveAudio play2 play publish",
    "_global.NetStream|1|2|checkPolicyFile setBufferTime send attachVideo attachAudio close seek pause receiveVideo receiveAudio play publish",
    "_global.NetStream|30|2|backBufferTime maxPauseBufferTime checkPolicyFile getInfo setBufferTime send attachVideo attachAudio close onPeerConnect seek pause receiveVideo receiveAudio play2 play publish",
    "_global.Number|31|1|toLocaleString isPropertyEnumerable isPrototypeOf hasOwnProperty addProperty unwatch watch toString valueOf",
    "_global.Number|31|2|toLocaleString isPropertyEnumerable isPrototypeOf hasOwnProperty addProperty unwatch watch toString valueOf",
    "_global.PrintJob|31|1|orientation pageWidth pageHeight paperWidth paperHeight send addPage start",
    "_global.PrintJob|31|2|orientation pageWidth pageHeight paperWidth paperHeight send addPage start",
    "_global.SharedObject|31|1|clear setFps getSize close flush send connect",
    "_global.SharedObject|31|2|clear setFps getSize close flush send connect",
    "_global.Sound|31|1|checkPolicyFile areSoundsInaccessible getBytesTotal getBytesLoaded loadSound setPosition getPosition setDuration getDuration start attachSound stop setVolume setTransform setPan getVolume getTransform getPan",
    "_global.Sound|31|2|checkPolicyFile areSoundsInaccessible getBytesTotal getBytesLoaded loadSound setPosition getPosition setDuration getDuration start attachSound stop setVolume setTransform setPan getVolume getTransform getPan",
    "_global.String|31|1|toLocaleString isPropertyEnumerable isPrototypeOf hasOwnProperty addProperty unwatch watch substr split substring slice lastIndexOf indexOf concat charCodeAt charAt toLowerCase toUpperCase toString valueOf",
    "_global.String|31|2|toLocaleString isPropertyEnumerable isPrototypeOf hasOwnProperty addProperty unwatch watch substr split substring slice lastIndexOf indexOf concat charCodeAt charAt toLowerCase toUpperCase toString valueOf",
    "_global.System.security.PolicyFileResolver|31|1|_listeners removeListener addListener broadcastMessage resolve",
    "_global.System.security.PolicyFileResolver|31|2|_listeners removeListener addListener broadcastMessage resolve",
    "_global.TextField|7|1|styleSheet mouseWheelEnabled condenseWhite restrict textHeight textWidth bottomScroll length selectable multiline password wordWrap background border html embedFonts maxChars maxhscroll hscroll variable htmlText type text autoSize tabIndex textColor backgroundColor borderColor maxscroll scroll filters sharpness thickness antiAliasType gridFitType removeListener addListener broadcastMessage replaceText getDepth setNewTextFormat getNewTextFormat removeTextField setTextFormat getTextFormat replaceSel",
    "_global.TextField|24|1|styleSheet mouseWheelEnabled condenseWhite restrict textHeight textWidth bottomScroll length selectable multiline password wordWrap background border html embedFonts maxChars maxhscroll hscroll variable htmlText type text autoSize tabIndex textColor backgroundColor borderColor maxscroll scroll filters sharpness thickness antiAliasType gridFitType _listeners removeListener addListener broadcastMessage replaceText getDepth setNewTextFormat getNewTextFormat removeTextField setTextFormat getTextFormat replaceSel",
    "_global.TextField|7|2|styleSheet mouseWheelEnabled condenseWhite restrict textHeight textWidth bottomScroll length selectable multiline password wordWrap background border html embedFonts maxChars maxhscroll hscroll variable htmlText type text autoSize tabIndex textColor backgroundColor borderColor maxscroll scroll filters sharpness thickness antiAliasType gridFitType removeListener addListener broadcastMessage replaceText getDepth setNewTextFormat getNewTextFormat removeTextField setTextFormat getTextFormat replaceSel",
    "_global.TextField|24|2|styleSheet mouseWheelEnabled condenseWhite restrict textHeight textWidth bottomScroll length selectable multiline password wordWrap background border html embedFonts maxChars maxhscroll hscroll variable htmlText type text autoSize tabIndex textColor backgroundColor borderColor maxscroll scroll filters sharpness thickness antiAliasType gridFitType _listeners removeListener addListener broadcastMessage replaceText getDepth setNewTextFormat getNewTextFormat removeTextField setTextFormat getTextFormat replaceSel",
    "_global.TextFormat|31|1|display bullet tabStops blockIndent leading indent rightMargin leftMargin align underline italic bold target url color size font",
    "_global.TextFormat|3|2|display bullet tabStops blockIndent leading indent rightMargin leftMargin align underline italic bold target url color size font",
    "_global.TextFormat|28|2|letterSpacing kerning display bullet tabStops blockIndent leading indent rightMargin leftMargin align underline italic bold target url color size font",
    "_global.TextSnapshot|31|1|getTextRunInfo setSelectColor findText hitTestTextNearPos getSelectedText getText getSelected setSelected getCount",
    "_global.TextSnapshot|31|2|getTextRunInfo setSelectColor findText hitTestTextNearPos getSelectedText getText getSelected setSelected getCount",
    "_global.Video|31|1|clear attachVideo",
    "_global.Video|31|2|clear attachVideo",
    "_global.XML|31|1|toLocaleString isPropertyEnumerable isPrototypeOf hasOwnProperty valueOf addProperty unwatch watch namespaceURI localName prefix previousSibling parentNode nodeValue nodeType nodeName nextSibling lastChild firstChild childNodes attributes getPrefixForNamespace getNamespaceForPrefix toString hasChildNodes appendChild insertBefore removeNode cloneNode xmlDecl status loaded ignoreWhite docTypeDecl contentType addRequestHeader getBytesTotal getBytesLoaded onData onLoad sendAndLoad send load parseXML createTextNode createElement",
    "_global.XML|31|2|toLocaleString isPropertyEnumerable isPrototypeOf hasOwnProperty valueOf addProperty unwatch watch namespaceURI localName prefix previousSibling parentNode nodeValue nodeType nodeName nextSibling lastChild firstChild childNodes attributes getPrefixForNamespace getNamespaceForPrefix toString hasChildNodes appendChild insertBefore removeNode cloneNode xmlDecl status loaded ignoreWhite docTypeDecl contentType addRequestHeader getBytesTotal getBytesLoaded onData onLoad sendAndLoad send load parseXML createTextNode createElement",
    "_global.XMLNode|31|1|toLocaleString isPropertyEnumerable isPrototypeOf hasOwnProperty valueOf addProperty unwatch watch namespaceURI localName prefix previousSibling parentNode nodeValue nodeType nodeName nextSibling lastChild firstChild childNodes attributes getPrefixForNamespace getNamespaceForPrefix toString hasChildNodes appendChild insertBefore removeNode cloneNode",
    "_global.XMLNode|31|2|toLocaleString isPropertyEnumerable isPrototypeOf hasOwnProperty valueOf addProperty unwatch watch namespaceURI localName prefix previousSibling parentNode nodeValue nodeType nodeName nextSibling lastChild firstChild childNodes attributes getPrefixForNamespace getNamespaceForPrefix toString hasChildNodes appendChild insertBefore removeNode cloneNode",
    "_global.XMLSocket|1|1|onData close send connect",
    "_global.XMLSocket|30|1|setTimeout getTimeout onData close send connect",
    "_global.XMLSocket|1|2|onData close send connect",
    "_global.XMLSocket|30|2|setTimeout getTimeout onData close send connect",
    "_global.flash.automation.ActionGenerator|28|2|toString valueOf generateActions generateAction",
    "_global.flash.automation.Configuration|28|2|toString valueOf setDeviceConfiguration getDeviceConfiguration getTestAutomationConfiguration",
    "_global.flash.automation.StageCapture|28|2|toString valueOf listenForStageCapture setClipRect getClipRect setFileNameBase getFileNameBase cancel capture",
    "_global.flash.filters.BevelFilter|28|2|clone type blurY blurX knockout strength quality shadowAlpha shadowColor highlightAlpha highlightColor angle distance",
    "_global.flash.filters.BitmapFilter|28|2|clone",
    "_global.flash.filters.BlurFilter|28|2|clone quality blurY blurX",
    "_global.flash.filters.ColorMatrixFilter|28|2|clone matrix",
    "_global.flash.filters.ConvolutionFilter|28|2|clone alpha color clamp preserveAlpha bias divisor matrix matrixY matrixX",
    "_global.flash.filters.DisplacementMapFilter|28|2|clone alpha color mode scaleY scaleX componentY componentX mapPoint mapBitmap",
    "_global.flash.filters.DropShadowFilter|28|2|clone hideObject strength blurY blurX knockout inner quality alpha color angle distance",
    "_global.flash.filters.GlowFilter|28|2|clone strength blurY blurX knockout inner quality alpha color",
    "_global.flash.filters.GradientBevelFilter|28|2|clone type knockout strength quality blurY blurX ratios alphas colors angle distance",
    "_global.flash.filters.GradientGlowFilter|28|2|clone type knockout strength quality blurY blurX ratios alphas colors angle distance",
    "_global.flash.geom.ColorTransform|28|2|toString concat rgb blueOffset greenOffset redOffset alphaOffset blueMultiplier greenMultiplier redMultiplier alphaMultiplier",
    "_global.flash.geom.Matrix|28|2|toString transformPoint deltaTransformPoint scale translate rotate identity clone createGradientBox createBox invert concat",
    "_global.flash.geom.Point|28|2|toString normalize add subtract equals offset clone length",
    "_global.flash.geom.Rectangle|28|2|toString equals union intersects intersection containsRectangle containsPoint contains offsetPoint offset inflatePoint inflate size bottomRight topLeft bottom top right left isEmpty setEmpty clone",
    "_global.flash.net.FileReferenceList|28|2|browse removeListener addListener broadcastMessage",
    NULL
};

static int ns_chain_has(VM *vm, AObj *o, AStr *k) {
    int guard = 0;
    while (o && guard++ < 64) {
        Prop *p = pm_get(&o->props, k, 1), *pp;
        if (p) return 1;
        pp = pm_get(&o->props, vm->s_proto, 1);
        o = pp && pp->v.t == V_OBJ ? pp->v.u.o : NULL;
    }
    return 0;
}

static void linux_prototypes(VM *vm, Env *env, AObj *g) {
    int cat = avm1_linux_ns_cat(), grp = env == &vm->env[1] ? 2 : 1, li;
    for (li = 0; linux_protos[li]; li++) {
        const char *s = linux_protos[li], *bar = strchr(s, '|'), *q, *e;
        AStr *names[160]; int n = 0, i, mask, gm, lazy;
        AObj *o = g, *proto, *up; Prop *pp;
        mask = atoi(bar + 1); q = strchr(bar + 1, '|') + 1; gm = atoi(q); q = strchr(q, '|') + 1;
        if (!(mask & (1 << cat)) || !(gm & grp)) continue;
        lazy = !strncmp(s, "_global.TextField|", 18) || !strncmp(s, "_global.XML|", 12);
        for (e = s + 7; e < bar && *e == '.'; ) {
            const char *b0 = ++e; while (e < bar && *e != '.') e++;
            o = ns_child(vm, o, b0, (int)(e - b0));
        }
        if (!o) continue;
        pp = pm_get(&o->props, vm->s_prototype, 1);
        if (!pp || pp->getter || pp->v.t != V_OBJ) continue;
        proto = pp->v.u.o;
        pp = pm_get(&proto->props, vm->s_proto, 1);
        up = pp && pp->v.t == V_OBJ ? pp->v.u.o : NULL;
        while (*q && n < 160) {
            char b[64]; int l = 0;
            while (*q == ' ') q++;
            if (!*q) break;
            while (*q && *q != ' ' && l < 63) b[l++] = *q++;
            b[l] = 0; names[n++] = str_intern(vm, b);
        }
        for (i = 0; i < proto->props.n; i++) {
            Prop *p = &proto->props.items[i]; int j;
            if (!p->key) continue;
            for (j = 0; j < n; j++) if (str_eq(p->key, names[j])) break;
            if (j < n || str_eq(p->key, vm->s_proto) || str_eq(p->key, vm->s_constructor) || str_eq(p->key, vm->s___constructor__)) continue;
            if (getenv("FLASHNT_NSDEBUG")) fprintf(stderr, "ns: hide %.*s.prototype.%s\n", (int)(bar - s), s, str_c(vm, p->key));
            /* a method the Linux player does not have goes; other members (native
               properties, lists) only stop being enumerated */
            if (!p->getter && p->v.t == V_OBJ && p->v.u.o->nk == NK_FUNCTION) { pm_remove(&proto->props, p->key, 1); i = -1; }
            else p->attr |= PA_NEVER_ENUM;
        }
        for (i = n - 1; i >= 0; i--) {
            if (!pm_get(&proto->props, names[i], 1)) {
                if (ns_chain_has(vm, up, names[i])) continue;   /* inherited */
                if (lazy) continue;   /* made by the first construction, as the newest members */
                if (getenv("FLASHNT_NSDEBUG")) fprintf(stderr, "ns: add %.*s.prototype.%s\n", (int)(bar - s), s, str_c(vm, names[i]));
                o_define(vm, proto, names[i], v_undef(), PA_DONT_ENUM);   /* only its name matters */
            }
            avm1_prop_to_front(vm, proto, names[i]);
        }
    }
}

void lib_create_globals(VM *vm, Env *env) {
    AObj *object_proto, *fn_proto, *object, *function, *g;
    AObj *flash, *external, *geom, *filters, *display, *net, *text, *automation;
    AObj *security, *capabilities, *ime, *product, *textfield, *filereference, *filereflist, *system;
    int version = env == &vm->env[1] ? 8 : 6;

    /* Object and Function (DeclContext::new) */
    object_proto = obj_bare(vm);
    fn_proto = obj_bare(vm);
    env->p.object = object_proto;
    env->p.function = fn_proto;
    env->p.function_ctor = NULL;
    object = fn_class(vm, env, obj_ctor_native, obj_function, object_proto, 1);
    o_define(vm, object_proto, str_intern(vm, "watch"), v_obj(fn_table(vm, env, object_method, OM_WATCH)), DE | DD | V6);
    o_define(vm, object_proto, str_intern(vm, "unwatch"), v_obj(fn_table(vm, env, object_method, OM_UNWATCH)), DE | DD | V6);
    o_define(vm, object_proto, str_intern(vm, "addProperty"), v_obj(fn_table(vm, env, object_method, OM_ADD_PROPERTY)), DE | DD | V6);
    o_define(vm, object_proto, str_intern(vm, "valueOf"), v_obj(fn_table(vm, env, object_method, OM_VALUE_OF)), DE | DD);
    o_define(vm, object_proto, str_intern(vm, "toString"), v_obj(fn_table(vm, env, object_method, OM_TO_STRING)), DE | DD);
    o_define(vm, object_proto, str_intern(vm, "hasOwnProperty"), v_obj(fn_table(vm, env, object_method, OM_HAS_OWN_PROPERTY)), DE | DD | V6);
    o_define(vm, object_proto, str_intern(vm, "isPrototypeOf"), v_obj(fn_table(vm, env, object_method, OM_IS_PROTOTYPE_OF)), DE | DD | V6);
    o_define(vm, object_proto, str_intern(vm, "isPropertyEnumerable"), v_obj(fn_table(vm, env, object_method, OM_IS_PROPERTY_ENUMERABLE)), DE | DD | V6);
    defm(vm, env, object_proto, "toLocaleString", obj_to_locale_string, DE | DD);
    o_define(vm, object, str_intern(vm, "registerClass"), v_obj(fn_table(vm, env, object_method, OM_REGISTER_CLASS)), DE | DD | RO);
    env->p.object_ctor = object;
    /* Object.prototype cannot be replaced (Flash: Object's own properties ignore writes);
       the SWF7+ globals keep Ruffle's declarations (global_proto_decls) */
    if (env == &vm->env[0]) o_set_attributes(vm, object, vm->s_prototype, RO, 0);

    function = fn_class(vm, env, fn_constructor, fn_function, fn_proto, 1);
    defm(vm, env, fn_proto, "call", fn_call, DE | DD);
    defm(vm, env, fn_proto, "apply", fn_apply, DE | DD);
    env->p.function_ctor = function;
    {   /* the natives made before Function existed */
        extern AObj *fn_native_ctor(VM *vm, Env *env, AObj *o); int i;
        for (i = 0; i < object_proto->props.n; i++) { Prop *pp = &object_proto->props.items[i]; if (pp->key && pp->v.t == V_OBJ && pp->v.u.o->nk == NK_FUNCTION) fn_native_ctor(vm, env, pp->v.u.o); }
        for (i = 0; i < object->props.n; i++) { Prop *pp = &object->props.items[i]; if (pp->key && pp->v.t == V_OBJ && pp->v.u.o->nk == NK_FUNCTION && pp->v.u.o != object) fn_native_ctor(vm, env, pp->v.u.o); }
        for (i = 0; i < fn_proto->props.n; i++) { Prop *pp = &fn_proto->props.items[i]; if (pp->key && pp->v.t == V_OBJ && pp->v.u.o->nk == NK_FUNCTION && pp->v.u.o != function) fn_native_ctor(vm, env, pp->v.u.o); }
    }
    o_define(vm, object, vm->s_constructor, v_obj(function), DE | DD);
    o_define(vm, function, vm->s_constructor, v_obj(function), DE | DD);
    o_define(vm, fn_proto, vm->s_proto, v_obj(object_proto), DE | DD);
    o_set_attributes(vm, fn_proto, vm->s_constructor, 0, DD);   /* Function.prototype.constructor can be deleted */
    /* functions created so far point at the right Function.prototype already */

    flash = obj_new(vm, object_proto); external = obj_new(vm, object_proto); geom = obj_new(vm, object_proto);
    filters = obj_new(vm, object_proto); display = obj_new(vm, object_proto); net = obj_new(vm, object_proto);
    text = obj_new(vm, object_proto); automation = obj_new(vm, object_proto);

    {
        AObj *button = lib_button(vm, env, object_proto);
        AObj *movieclip = lib_movieclip(vm, env, object_proto);
        AObj *sound = lib_sound(vm, env, object_proto);
        AObj *stylesheet = lib_stylesheet(vm, env, object_proto);
        AObj *textformat, *array, *color, *xmlnode, *string, *number, *boolean, *loadvars, *localconn, *colortransform;
        AObj *extif, *mcl, *video, *netstream, *netconnection, *xmlsocket, *xml, *date, *transform, *bitmapdata;
        AObj *sharedobject, *selection, *camera, *microphone, *printjob, *textsnapshot, *math, *soundcodec, *mouse, *key;
        AObj *stage, *accessibility, *textrenderer, *empty;
        textfield = lib_textfield(vm, env, object_proto);
        textformat = lib_textformat(vm, env, object_proto);
        array = lib_array(vm, env);
        color = lib_color(vm, env, object_proto);
        xmlnode = lib_xmlnode(vm, env, object_proto);
        string = lib_string(vm, env);
        number = lib_number(vm, env);
        boolean = lib_boolean(vm, env);
        loadvars = lib_loadvars(vm, env, object_proto);
        localconn = lib_localconnection(vm, env, object_proto);
        colortransform = lib_colortransform(vm, env, object_proto);
        extif = lib_external_interface(vm, env, object_proto);
        mcl = lib_mcl(vm, env, object_proto);
        video = lib_video(vm, env, object_proto);
        netstream = lib_netstream(vm, env, object_proto);
        netconnection = lib_netconnection(vm, env, object_proto);
        xmlsocket = lib_xmlsocket(vm, env, object_proto);
        xml = lib_xml(vm, env, env->p.xmlnode);
        date = lib_date(vm, env);
        transform = lib_transform(vm, env, object_proto);
        lib_filters(vm, env, object_proto, filters);
        bitmapdata = lib_bitmapdata(vm, env, object_proto);
        filereference = lib_fileref(vm, env, object_proto);
        filereflist = lib_filereflist(vm, env, object_proto);
        sharedobject = lib_sharedobject(vm, env, object_proto);
        selection = lib_selection(vm, env);
        camera = lib_camera(vm, env, object_proto);
        microphone = lib_microphone(vm, env, object_proto);
        printjob = lib_printjob(vm, env, object_proto);
        textsnapshot = lib_textsnapshot(vm, env, object_proto);
        system = lib_system(vm, env, &security, &capabilities, &ime, &product);
        math = lib_math(vm, env);
        soundcodec = lib_soundcodec(vm, env);
        mouse = lib_mouse(vm, env);
        key = lib_key(vm, env);
        stage = lib_stage(vm, env);
        accessibility = lib_accessibility(vm, env);
        textrenderer = lib_textrenderer(vm, env, object_proto);
        empty = NULL;
        (void)empty;

        g = obj_bare(vm);
        env->global = g;
        defm(vm, env, g, "ASnative", g_asnative, DE);
        /* Flash has it everywhere; Ruffle's global_proto_decls expectation (a SWF10
         * movie) lacks it: it is left out of a player whose main movie is SWF9+ */
        if (avm1_root_pre9(vm)) defm(vm, env, g, "ASconstructor", g_asconstructor, DE);
        defv(vm, g, "Object", v_obj(object), DE);
        defv(vm, g, "Function", v_obj(function), DE | V6);
        defp(vm, env, g, "NaN", g_get_nan, NULL, DE);
        defp(vm, env, g, "Infinity", g_get_infinity, NULL, DE);
        defv(vm, g, "MovieClip", classv(movieclip), DE);
        defv(vm, g, "XMLSocket", classv(xmlsocket), DE);
        defv(vm, g, "AsBroadcaster", v_null(), DE);
        defv(vm, g, "Color", classv(color), DE);
        defv(vm, g, "NetConnection", classv(netconnection), DE);
        defv(vm, g, "NetStream", classv(netstream), DE);
        defv(vm, g, "Camera", classv(camera), DE);
        defv(vm, g, "Microphone", classv(microphone), DE);
        defv(vm, g, "SharedObject", classv(sharedobject), DE);
        defv(vm, g, "ContextMenuItem", v_null(), DE);
        defv(vm, g, "ContextMenu", v_null(), DE);
        defv(vm, g, "Error", v_null(), DE);
        defv(vm, g, "AsSetupError", v_null(), DE);
        { extern AObj *lib_asset_cache(VM *vm, Env *env, AObj *object_proto); defv(vm, g, "AssetCache", classv(lib_asset_cache(vm, env, object_proto)), DE); }
        { extern AObj *lib_remote_lso_usage(VM *vm, Env *env, AObj *object_proto); defv(vm, g, "RemoteLSOUsage", classv(lib_remote_lso_usage(vm, env, object_proto)), DE); }
        defm(vm, env, g, "ASSetPropFlags", g_as_set_prop_flags, DE);
        deft(vm, env, g, "ASSetNative", as_set_native_method, 0, DE);
        deft(vm, env, g, "ASSetNativeAccessor", as_set_native_method, 1, DE);
        deft(vm, env, g, "escape", globals_method, 0, DE);
        deft(vm, env, g, "unescape", globals_method, 1, DE);
        deft(vm, env, g, "parseInt", globals_method, 2, DE);
        deft(vm, env, g, "parseFloat", globals_method, 3, DE);
        defv(vm, g, "trace", v_null(), DE);
        defm(vm, env, g, "updateAfterEvent", g_update_after_event, DE);
        defm(vm, env, g, "isNaN", g_is_nan, DE);
        defm(vm, env, g, "isFinite", g_is_finite, DE);
        defm(vm, env, g, "setInterval", g_set_interval, DE);
        defm(vm, env, g, "clearTimeout", g_clear_interval, DE);
        defm(vm, env, g, "clearInterval", g_clear_interval, DE);
        defm(vm, env, g, "setTimeout", g_set_timeout, DE);
        defv(vm, g, "o", v_null(), 0);
        defv(vm, g, "Number", classv(number), DE);
        defv(vm, g, "Boolean", classv(boolean), DE);
        defv(vm, g, "Date", classv(date), DE);
        defv(vm, g, "String", classv(string), DE);
        defv(vm, g, "Array", classv(array), DE);
        defv(vm, g, "Math", classv(math), DE);
        defv(vm, g, "Sound", classv(sound), DE);
        defv(vm, g, "XMLNode", classv(xmlnode), DE);
        defv(vm, g, "XML", classv(xml), DE);
        defv(vm, g, "LoadVars", classv(loadvars), DE);
        defv(vm, g, "Selection", classv(selection), DE);
        defv(vm, g, "Mouse", classv(mouse), DE);
        defv(vm, g, "Key", classv(key), DE);
        defv(vm, g, "Button", classv(button), DE);
        defv(vm, g, "TextField", classv(textfield), DE);
        defv(vm, g, "TextFormat", classv(textformat), DE);
        defv(vm, g, "Stage", classv(stage), DE);
        defv(vm, g, "Video", classv(video), DE);
        defv(vm, g, "Accessibility", classv(accessibility), DE);
        defv(vm, g, "SoundCodec", classv(soundcodec), DE);
        defv(vm, g, "System", classv(system), DE);
        defv(vm, g, "flash", v_obj(flash), DE);
        defv(vm, g, "textRenderer", classv(textrenderer), 0);
        defv(vm, g, "LocalConnection", classv(localconn), DE);
        defv(vm, g, "MovieClipLoader", classv(mcl), DE);
        defv(vm, g, "PrintJob", classv(printjob), DE);
        defv(vm, g, "TextSnapshot", classv(textsnapshot), DE);

        defv(vm, flash, "text", v_obj(text), 0);
        defv(vm, flash, "display", v_obj(display), 0);
        defv(vm, flash, "filters", v_obj(filters), 0);
        defv(vm, flash, "geom", v_obj(geom), 0);
        defv(vm, flash, "net", v_obj(net), 0);
        defv(vm, flash, "external", v_obj(external), 0);
        defv(vm, flash, "automation", v_obj(automation), 0);
        {
            /* flash.automation stubs (Ruffle automation_*.rs): table natives 2700/2800/2900 */
            extern Value automation_capture_method(Act *a, AObj *self, int argc, Value *argv, int index);
            extern Value automation_generator_method(Act *a, AObj *self, int argc, Value *argv, int index);
            extern Value automation_config_method(Act *a, AObj *self, int argc, Value *argv, int index);
            extern Value automation_capture_ctor(Act *a, AObj *self, int argc, Value *argv);
            AObj *p, *c;
            static const char *const cap[] = { "capture", "cancel", "getFileNameBase", "setFileNameBase", "getClipRect", "setClipRect", "listenForStageCapture", "valueOf", "toString" };
            int k;
            c = lib_class(vm, env, automation_capture_ctor, NULL, object_proto, 1, &p);
            for (k = 0; k < 9; k++) deft(vm, env, p, cap[k], automation_capture_method, k, k < 7 ? DE | DD : DD);
            defv(vm, automation, "StageCapture", classv(c), 0);
            c = lib_class(vm, env, NULL, NULL, object_proto, 0, &p);
            deft(vm, env, p, "generateAction", automation_generator_method, 0, 0);
            deft(vm, env, p, "generateActions", automation_generator_method, 1, 0);
            deft(vm, env, p, "valueOf", automation_generator_method, 3, 0);
            deft(vm, env, p, "toString", automation_generator_method, 4, 0);
            defv(vm, automation, "ActionGenerator", classv(c), 0);
            c = lib_class(vm, env, NULL, NULL, object_proto, 0, &p);
            deft(vm, env, p, "getTestAutomationConfiguration", automation_config_method, 0, 0);
            deft(vm, env, p, "getDeviceConfiguration", automation_config_method, 1, 0);
            deft(vm, env, p, "setDeviceConfiguration", automation_config_method, 2, 0);
            deft(vm, env, p, "valueOf", automation_config_method, 4, 0);
            deft(vm, env, p, "toString", automation_config_method, 5, 0);
            defv(vm, automation, "Configuration", classv(c), 0);
        }
        defv(vm, display, "BitmapData", classv(bitmapdata), 0);
        defv(vm, external, "ExternalInterface", classv(extif), 0);
        defv(vm, geom, "Rectangle", v_null(), 0);
        defv(vm, geom, "Point", v_null(), 0);
        defv(vm, geom, "Matrix", v_null(), 0);
        defv(vm, geom, "ColorTransform", classv(colortransform), 0);
        defv(vm, geom, "Transform", classv(transform), 0);
        defv(vm, net, "FileReference", classv(filereference), 0);
        defv(vm, net, "FileReferenceList", classv(filereflist), 0);
        defv(vm, text, "TextRenderer", classv(textrenderer), 0);
        if (system) {
            defv(vm, system, "capabilities", classv(capabilities), 0);
            defv(vm, system, "Product", classv(product), 0);
            defv(vm, system, "security", classv(security), 0);
            defv(vm, system, "IME", classv(ime), 0);
        }
        if (textfield) defv(vm, textfield, "StyleSheet", classv(stylesheet), DE | V7);
        (void)array; (void)string; (void)number; (void)boolean; (void)extif;
    }
    env->gscope = (Scope*)gc_alloc(vm, sizeof(Scope), GC_SCOPE);
    env->gscope->cls = SC_GLOBAL; env->gscope->values = g; env->gscope->parent = NULL;
    run_playerglobal(vm, env, version);
    { extern int devfont_linux_rules(void); if (devfont_linux_rules()) {
        linux_namespace(vm, env, env->global); linux_prototypes(vm, env, env->global);
        {   /* SWF7 movies do not see (nor enumerate) the SWF8 text format members */
            AObj *tf = ns_child(vm, env->global, "TextFormat", 10); Prop *pp = tf ? pm_get(&tf->props, vm->s_prototype, 1) : NULL;
            if (pp && pp->v.t == V_OBJ) { ns_set_attr(vm, pp->v.u.o, "letterSpacing", PA_VERSION_8 | PA_ENUM_BY_VERSION); ns_set_attr(vm, pp->v.u.o, "kerning", PA_VERSION_8 | PA_ENUM_BY_VERSION); }
        }
    } }
}

/* ---------------------------------------------------------------- */
/* playerglobals written in ActionScript                            */

/* g_in_playerglobal: the AS2 part of the built-in library is being set up */
static void run_playerglobal(VM *vm, Env *env, int swf_version) {
    /* avm1_playerglobal holds the concatenated DoAction bodies, each
       preceded by its u32 length */
    const unsigned char *p = avm1_playerglobal; unsigned long left = avm1_playerglobal_len;
    while (left >= 4) {
        u32 n = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
        Act a; int ret = 0;
        p += 4; left -= 4;
        if (n > left) break;
        memset(&a, 0, sizeof(a));
        a.vm = vm; a.swf_version = (u8)swf_version; a.scope = env->gscope; a.pool = vm->cpool;
        a.base_clip = vm->player ? vm->player->root : NULL; a.target_clip = a.base_clip; a.has_target = a.base_clip != NULL;
        a.this_ = v_null(); a.id.reason = ER_SPECIAL;
        g_in_playerglobal++;
        act_run(&a, p, n, 0, n, &ret);
        g_in_playerglobal--;
        if (vm->err) { vm_report_error(&a); vm->err = ERR_NONE; }
        p += n; left -= n;
    }
}
