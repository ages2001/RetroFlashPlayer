/* avm2_bytes.c - flash.utils.ByteArray (+ AMF3 object encoding) and
 * ApplicationDomain.domainMemory (li8/si8... alchemy opcodes). */
#include "avm2_int.h"
#include "inflate.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define ARG(i) a2_arg(argc, argv, i)
#define NUM(i) v2_tonum(vm, ARG(i))
#define INT(i) v2_toint(vm, ARG(i))
#define STR(i) v2_tostr(vm, ARG(i))

static A2Obj *ba_of(V2 self) { return (self.t == T_OBJ && self.u.o->kind == OK_BYTEARRAY) ? self.u.o : NULL; }

/* ---- domain memory (alchemy li8/si8...): always read through the ByteArray, whose buffer may move ---- */
extern A2Obj *a2_bytearray_new(VM2 *vm);
static A2Obj *dom_ba(VM2 *vm) {
    if (vm->dom_mem) return vm->dom_mem;
    if (!vm->dom_default) {   /* without domainMemory the opcodes use a private zeroed 1024-byte buffer */
        A2Obj *b = a2_bytearray_new(vm);
        b->bytes = (u8*)calloc(1024, 1);
        if (b->bytes) { b->blen = b->bcap = 1024; }
        vm->dom_default = b;
    }
    return vm->dom_default;
}
V2 a2_domain_memory_load(VM2 *vm, int op, u32 addr) {
    int n = op == 0x35 ? 1 : op == 0x36 ? 2 : op == 0x37 ? 4 : op == 0x38 ? 4 : 8; u32 v = 0; int i; A2Obj *b = dom_ba(vm); u8 *m;
    if (!b || (u64)addr + (u32)n > (u64)(u32)b->blen) { a2_throw_id(vm, "RangeError", 1506, NULL, NULL); return v2_undef(); }
    m = b->bytes;
    for (i = 0; i < n && i < 4; i++) v |= (u32)m[addr + i] << (8 * i);
    switch (op) {
    case 0x35: return v2_int(m[addr]);
    case 0x36: return v2_int((s32)(v & 0xFFFF));
    case 0x37: return v2_int((s32)v);
    case 0x38: { float f; memcpy(&f, m + addr, 4); return v2_num(f); }
    default: { double d; memcpy(&d, m + addr, 8); return v2_num(d); }
    }
}
void a2_domain_memory_store(VM2 *vm, int op, u32 addr, V2 v) {
    int n = op == 0x3a ? 1 : op == 0x3b ? 2 : op == 0x3c ? 4 : op == 0x3d ? 4 : 8; int i; u32 u; A2Obj *b = dom_ba(vm); u8 *m;
    if (!b || (u64)addr + (u32)n > (u64)(u32)b->blen) { a2_throw_id(vm, "RangeError", 1506, NULL, NULL); return; }
    m = b->bytes;
    if (op == 0x3d) { float f = (float)v2_tonum(vm, v); memcpy(m + addr, &f, 4); return; }
    if (op == 0x3e) { double d = v2_tonum(vm, v); memcpy(m + addr, &d, 8); return; }
    u = (u32)v2_toint(vm, v);
    for (i = 0; i < n; i++) m[addr + i] = (u8)(u >> (8 * i));
}
static V2 ad_domainMemory_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *b = ba_of(ARG(0)); (void)self;
    if (!b && ARG(0).t != T_NULL && ARG(0).t != T_UNDEF) { a2_coerce(vm, ARG(0), vm->c_bytearray); return v2_undef(); }   /* value:ByteArray */
    if (b) { if (b->blen < 1024) { a2_throw_id(vm, "Error", 1504, NULL, NULL); return v2_undef(); } vm->dom_mem = b; }
    else vm->dom_mem = NULL;
    return v2_undef();
}
static V2 ad_domainMemory_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; (void)argc; (void)argv; return vm->dom_mem ? v2_obj(vm->dom_mem) : v2_null(); }
/* a new wrapper each time, as in Flash (currentDomain !== currentDomain) */
V2 a2_domain_wrapper(VM2 *vm, int d) { A2Obj *o = a2_new(vm, vm->c_applicationdomain); o->ival = d; return v2_obj(o); }
static int ad_id(VM2 *vm, V2 self) { (void)vm; return self.t == T_OBJ ? self.u.o->ival : 1; }
static V2 ad_currentDomain(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; (void)argc; (void)argv; return a2_domain_wrapper(vm, a2_cur_domain(vm)); }
extern V2 a2_get_definition(VM2 *vm, A2Str *q); extern int a2_definition(VM2 *vm, A2Str *q, V2 *out);
static V2 ad_getDefinition(VM2 *vm, V2 self, int argc, V2 *argv) { int saved = vm->lookup_domain; V2 r; A2Str *q = STR(0); if (vm->throwing) return v2_undef(); vm->lookup_domain = ad_id(vm, self) > 0 ? ad_id(vm, self) : 1; r = a2_get_definition(vm, q); vm->lookup_domain = saved; return r; }
static V2 ad_hasDefinition(VM2 *vm, V2 self, int argc, V2 *argv) { V2 v; int saved = vm->lookup_domain, r; A2Str *q; if (argc < 1 || argv[0].t == T_NULL || argv[0].t == T_UNDEF) return v2_bool(0); q = STR(0); vm->lookup_domain = ad_id(vm, self) > 0 ? ad_id(vm, self) : 1; r = a2_definition(vm, q, &v) != 0; vm->lookup_domain = saved; return v2_bool(r); }
static V2 ad_parentDomain(VM2 *vm, V2 self, int argc, V2 *argv) { int p = a2_domain_parent(vm, ad_id(vm, self)); (void)argc; (void)argv; return p <= 0 ? v2_null() : a2_domain_wrapper(vm, p); }   /* the system domain is not exposed */
static V2 ad_minDomainMemoryLength(VM2 *vm, V2 self, int argc, V2 *argv) { (void)vm; (void)self; (void)argc; (void)argv; return v2_uint(1024); }
static V2 ad_getQualifiedDefinitionNames(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *a = a2_new(vm, a2_vector_class_for(vm, vm->c_string)); int i; (void)argc; (void)argv; a->kind = OK_VECTOR; a->vec_type = vm->c_string; (void)i; { extern void a2_push_definition_names(VM2 *vm, A2Obj *vec); int saved = vm->lookup_domain; vm->lookup_domain = ad_id(vm, self) > 0 ? ad_id(vm, self) : 1; a2_push_definition_names(vm, a); vm->lookup_domain = saved; } return v2_obj(a); }
static V2 AppDomain_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {   /* new ApplicationDomain(parent = system domain) */
    int parent = argc > 0 && argv[0].t == T_OBJ && argv[0].u.o->cls == vm->c_applicationdomain ? argv[0].u.o->ival : 0;
    if (self.t == T_OBJ) self.u.o->ival = a2_new_domain(vm, parent);
    return v2_undef(); }

void a2_bytearray_misc_init(VM2 *vm) {
    A2Class *c; extern void a2_global_class(VM2 *vm, A2Class *c);
    c = a2_class_new(vm, "flash.utils", "Endian", vm->c_object, OK_PLAIN); a2_add_const(vm, c, "BIG_ENDIAN", v2_cstr(vm, "bigEndian")); a2_add_const(vm, c, "LITTLE_ENDIAN", v2_cstr(vm, "littleEndian")); a2_global_class(vm, c);
    c = a2_class_new(vm, "flash.net", "ObjectEncoding", vm->c_object, OK_PLAIN); a2_add_const(vm, c, "AMF0", v2_uint(0)); a2_add_const(vm, c, "AMF3", v2_uint(3)); a2_add_const(vm, c, "DEFAULT", v2_uint(3)); a2_global_class(vm, c);
    c = a2_class_new(vm, "flash.utils", "IDataInput", vm->c_object, OK_PLAIN); c->flags = CF_INTERFACE; a2_global_class(vm, c);
    c = a2_class_new(vm, "flash.utils", "IDataOutput", vm->c_object, OK_PLAIN); c->flags = CF_INTERFACE; a2_global_class(vm, c);
    c = a2_class_new(vm, "flash.utils", "IExternalizable", vm->c_object, OK_PLAIN); c->flags = CF_INTERFACE; a2_global_class(vm, c);
    vm->c_applicationdomain = c = a2_class_new(vm, "flash.system", "ApplicationDomain", vm->c_object, OK_PLAIN); c->native_ctor = AppDomain_ctor;
    a2_add_getter(vm, c, "domainMemory", ad_domainMemory_get, ad_domainMemory_set); a2_add_getter(vm, c, "parentDomain", ad_parentDomain, NULL);
    a2_add_method(vm, c, "getDefinition", ad_getDefinition, 1); a2_add_method(vm, c, "hasDefinition", ad_hasDefinition, 1); a2_add_method(vm, c, "getQualifiedDefinitionNames", ad_getQualifiedDefinitionNames, 0);
    { A2Trait *t; extern void a2_tm_put(A2TraitMap *tm, A2Trait *t); t = (A2Trait*)calloc(1, sizeof(A2Trait)); t->name = a2_str(vm, "currentDomain"); t->ns = vm->ns_public; t->kind = TK_GETTER; t->getter = a2_native_method(vm, ad_currentDomain, "currentDomain", 0); a2_tm_put(&c->ctraits, t);
      t = (A2Trait*)calloc(1, sizeof(A2Trait)); t->name = a2_str(vm, "MIN_DOMAIN_MEMORY_LENGTH"); t->ns = vm->ns_public; t->kind = TK_GETTER; t->getter = a2_native_method(vm, ad_minDomainMemoryLength, "MIN_DOMAIN_MEMORY_LENGTH", 0); a2_tm_put(&c->ctraits, t); }
    a2_global_class(vm, c);
}

/* avm2.intrinsics.memory (concurrent.as): casi32, mfence (no workers: plain) */
static V2 mem_casi32(VM2 *vm, V2 self, int argc, V2 *argv) {
    s32 addr = INT(0), expv = INT(1), newv = INT(2), old; A2Obj *b = dom_ba(vm); u8 *m; (void)self;
    if (vm->throwing) return v2_undef();
    if (!b || addr < 0 || b->blen < 4 || (u32)addr > (u32)b->blen - 4 || (addr & 3)) { a2_throw_id(vm, "RangeError", 1506, NULL, NULL); return v2_undef(); }
    m = b->bytes + addr;
    old = (s32)((u32)m[0] | ((u32)m[1] << 8) | ((u32)m[2] << 16) | ((u32)m[3] << 24));
    if (old == expv) { m[0] = (u8)newv; m[1] = (u8)(newv >> 8); m[2] = (u8)(newv >> 16); m[3] = (u8)(newv >> 24); }
    return v2_int(old);
}
static V2 mem_mfence(VM2 *vm, V2 self, int argc, V2 *argv) { (void)vm; (void)self; (void)argc; (void)argv; return v2_undef(); }
void a2_memory_natives_init(void) {
    a2_register_native("avm2.intrinsics.memory::casi32", mem_casi32);
    a2_register_native("avm2.intrinsics.memory::mfence", mem_mfence);
}
