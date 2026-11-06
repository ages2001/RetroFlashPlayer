/* avm2_extif.c - flash.external.ExternalInterface natives (the class is
 * Ruffle's ActionScript, see as3/): calls go to the container
 * (g_ext_provider); callbacks added by the movie can be called by the
 * container (player_call_internal). */
#include "avm2_flash_int.h"
#include "extval.h"
#include <stdlib.h>
#include <string.h>

extern void a2_report_uncaught(VM2 *vm);

typedef struct { char *name; A2Obj *fn; } EICb;
static EICb *g_cb; static int g_ncb;

static ExtVal to_ext(VM2 *vm, V2 v, int depth) {
    if (depth > 64) return xv_null();
    switch (v.t) {
    case T_UNDEF: case T_HOLE: return xv_undef();
    case T_NULL: return xv_null();
    case T_BOOL: return xv_bool(v.u.b);
    case T_INT: case T_UINT: case T_NUM: return xv_num(v2_tonum(vm, v));
    case T_STR: return xv_str(v.u.s->s);
    default: break;
    }
    if (v.t != T_OBJ) return xv_null();
    if (v.u.o->kind == OK_ARRAY) {
        ExtVal l = xv_undef(); u32 i, n = a2_arr_len(v.u.o);
        l.t = XV_LIST;
        for (i = 0; i < n; i++) { V2 e = a2_arr_get(v.u.o, (int)i); xv_list_push(&l, e.t == T_HOLE ? xv_null() : to_ext(vm, e, depth + 1)); }
        return l;
    }
    if (v.u.o->kind == OK_PLAIN || v.u.o->kind == OK_FUNCTION) {
        ExtVal o = xv_undef(); int idx = 0; A2Str *k; V2 val;
        o.t = XV_OBJ;
        for (;;) { a2_dyn_enum(v.u.o, &idx, &k, &val); if (!idx) break; xv_obj_put(&o, k->s, to_ext(vm, val, depth + 1)); }
        return o;
    }
    return xv_null();
}
static V2 from_ext(VM2 *vm, const ExtVal *x) {
    int i;
    switch (x->t) {
    case XV_UNDEF: return v2_undef();
    case XV_NULL: return v2_null();
    case XV_BOOL: return v2_bool(x->b);
    case XV_NUM: return v2_num(x->n);
    case XV_STR: return v2_cstr(vm, x->s ? x->s : "");
    case XV_LIST: { A2Obj *a = a2_new_array(vm); for (i = 0; i < x->count; i++) a2_arr_push(vm, a, from_ext(vm, &x->items[i])); return v2_obj(a); }
    case XV_OBJ: { A2Obj *o = a2_new_plain(vm); for (i = 0; i < x->count; i++) a2_dyn_set(vm, o, a2_str(vm, x->keys[i]), from_ext(vm, &x->items[i])); return v2_obj(o); }
    }
    return v2_undef();
}

static int check(VM2 *vm) { if (!g_ext_provider) { a2_throw_id(vm, "Error", 2067, NULL, NULL); return 0; } return 1; }

static V2 e_available(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_bool(g_ext_provider != NULL); }
static V2 e_objectID(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_null(); }
static V2 e_call(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Str *name = v2_tostr(vm, ARG(0)); ExtVal *args, out; int i, n = argc > 1 ? argc - 1 : 0; V2 r;
    (void)self;
    if (vm->throwing || !check(vm)) return v2_undef();
    args = (ExtVal*)calloc((size_t)n + 1, sizeof(ExtVal));
    if (!args) return v2_null();
    for (i = 0; i < n; i++) args[i] = to_ext(vm, argv[i + 1], 0);
    out = xv_null();
    if (!g_ext_provider(vm->player, name->s, n, args, &out)) out = xv_null();
    for (i = 0; i < n; i++) xv_free(&args[i]);
    free(args);
    r = from_ext(vm, &out); xv_free(&out);
    return r;
}
static V2 e_addCallback(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Str *name = v2_tostr(vm, ARG(0)); int i; A2Obj *fn = ARG(1).t == T_OBJ && ARG(1).u.o->kind == OK_FUNCTION ? ARG(1).u.o : NULL;
    (void)self;
    if (vm->throwing || !check(vm)) return v2_undef();
    for (i = 0; i < g_ncb; i++) if (!strcmp(g_cb[i].name, name->s)) break;
    if (!fn) { if (i < g_ncb) { free(g_cb[i].name); g_cb[i] = g_cb[--g_ncb]; } return v2_undef(); }
    if (i < g_ncb) { g_cb[i].fn = fn; return v2_undef(); }
    { EICb *n = (EICb*)realloc(g_cb, (size_t)(g_ncb + 1) * sizeof(EICb)); size_t l = name->len;
      if (!n) return v2_undef();
      g_cb = n; g_cb[g_ncb].name = (char*)malloc(l + 1); if (!g_cb[g_ncb].name) return v2_undef();
      memcpy(g_cb[g_ncb].name, name->s, l + 1); g_cb[g_ncb].fn = fn; g_ncb++; }
    return v2_undef();
}

/* the container calls a callback: 1 when it exists */
int avm2_ei_invoke(Player *p, const char *name, int argc, const ExtVal *args, ExtVal *out) {
    VM2 *vm = (VM2*)p->avm2; int i; V2 *av, r;
    if (!vm) return 0;
    for (i = 0; i < g_ncb; i++) if (!strcmp(g_cb[i].name, name)) break;
    if (i == g_ncb) return 0;
    av = (V2*)calloc((size_t)argc + 1, sizeof(V2));
    if (!av) return 0;
    { int k; for (k = 0; k < argc; k++) av[k] = from_ext(vm, &args[k]); }
    r = a2_call(vm, v2_obj(g_cb[i].fn), v2_null(), argc, av);
    free(av);
    if (vm->throwing) { a2_report_uncaught(vm); *out = xv_null(); return 1; }
    *out = to_ext(vm, r, 0);
    return 1;
}
void a2_extif_mark(VM2 *vm) { int i; for (i = 0; i < g_ncb; i++) a2_mark_obj(vm, g_cb[i].fn); }

void a2_extif_natives_init(void) {
#define EI "flash.external::ExternalInterface$/"
    a2_register_native(EI "get available", e_available);
    a2_register_native(EI "get objectID", e_objectID);
    a2_register_native(EI "call", e_call);
    a2_register_native(EI "addCallback", e_addCallback);
}
