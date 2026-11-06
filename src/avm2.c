/* avm2.c - ActionScript 3 runtime: ABC loading, classes, property protocol,
 * calls and the bytecode interpreter. */
#include "avm2_int.h"
void throw_null_ref(VM2 *vm, V2 v) {
    if (v.t == T_NULL) a2_throw_error(vm, "TypeError", "Cannot access a property or method of a null object reference.", 1009);
    else a2_throw_error(vm, "TypeError", "A term is undefined and has no properties.", 1010);
}
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ---------------------------------------------------------------- */
/* per-ABC runtime data                                             */

typedef struct RTName { A2Str *name; A2Ns *nss; int nns; int kind; int rt_ns, rt_name, attr; A2Class *type; int type_tried; int null_ns; } RTName;

typedef struct AbcRT {
    AbcFile *abc; VM2 *vm;
    A2Ns *nss;               /* per ABC namespace */
    RTName *names;           /* per multiname */
    A2Str **strs;            /* per string */
    A2Method **methods;      /* per method */
    A2Class **classes;       /* per class (after newclass) */
    A2Class **bodycls;       /* activation class per body */
    A2Class **catchcls;      /* catch scope class per body (one per exception) */
    A2Obj **globals; A2Class **gcls; int *gstate;   /* per script: global object, class, 0 none / 1 running / 2 done */
    int fresh;               /* building a second class object of a class: its methods are bound anew */
    A2Method **extra; int nextra;   /* those method copies */
} AbcRT;

#define RT(abc) ((AbcRT*)(abc)->rt)

static A2Str *rt_str(AbcRT *rt, u32 i) {
    if (i >= rt->abc->string_count) return rt->vm->s_empty;
    if (!rt->strs[i]) rt->strs[i] = a2_intern(rt->vm, rt->abc->strings[i], rt->abc->string_lens[i]);
    return rt->strs[i];
}

static void rt_build_names(AbcRT *rt) {
    AbcFile *a = rt->abc; u32 i;
    rt->nss = (A2Ns*)calloc(a->ns_count + 1, sizeof(A2Ns));
    for (i = 0; i < a->ns_count; i++) {
        rt->nss[i].kind = a->namespaces[i].kind ? a->namespaces[i].kind : NS_PACKAGE;
        rt->nss[i].uri = rt_str(rt, a->namespaces[i].name);
        if (rt->nss[i].kind == NS_PRIVATE) { static int next_private_id; rt->nss[i].id = ++next_private_id; }
    }
    if (a->ns_count == 0) { rt->nss[0] = rt->vm->ns_public; }
    rt->names = (RTName*)calloc(a->mn_count + 1, sizeof(RTName));
    for (i = 0; i < a->mn_count; i++) {
        AbcMultiname *m = &a->multinames[i]; RTName *n = &rt->names[i];
        n->kind = m->kind;
        switch (m->kind) {
        case MN_QNAME: case MN_QNAME_A:
            n->name = rt_str(rt, m->name); n->nss = &rt->nss[m->ns < a->ns_count ? m->ns : 0]; n->nns = 1; n->null_ns = (m->ns == 0); break;
        case MN_RTQNAME: case MN_RTQNAME_A:
            n->name = rt_str(rt, m->name); n->rt_ns = 1; break;
        case MN_RTQNAME_L: case MN_RTQNAME_LA:
            n->rt_ns = 1; n->rt_name = 1; break;
        case MN_MULTINAME: case MN_MULTINAME_A:
        case MN_MULTINAME_L: case MN_MULTINAME_LA: {
            AbcNsSet *set = m->ns_set < a->nsset_count ? &a->nssets[m->ns_set] : NULL;
            u32 k;
            if (m->kind == MN_MULTINAME || m->kind == MN_MULTINAME_A) n->name = rt_str(rt, m->name); else n->rt_name = 1;
            if (set && set->count) {
                n->nss = (A2Ns*)calloc(set->count, sizeof(A2Ns)); n->nns = (int)set->count;
                for (k = 0; k < set->count; k++) n->nss[k] = rt->nss[set->ns[k] < a->ns_count ? set->ns[k] : 0];
            } else { n->nss = &rt->vm->ns_public; n->nns = 1; }
            break; }
        case MN_TYPENAME: {
            RTName *b = &rt->names[m->type_base < a->mn_count ? m->type_base : 0];
            *n = *b; n->kind = MN_TYPENAME; n->type = NULL; n->type_tried = 0;
            break; }
        default: n->name = rt->vm->s_empty; n->nss = &rt->vm->ns_public; n->nns = 1;
        }
        if (m->kind == MN_QNAME_A || m->kind == MN_RTQNAME_A || m->kind == MN_RTQNAME_LA || m->kind == MN_MULTINAME_A || m->kind == MN_MULTINAME_LA) n->attr = 1;
        if (!n->name) n->name = rt->vm->s_empty;
        if (!n->nss) { n->nss = &rt->vm->ns_public; n->nns = 1; }
    }
}

static void rt_to_name(RTName *r, A2Name *n) { n->name = r->name; n->nss = r->nss; n->nns = r->nns; n->any_ns = r->null_ns ? 2 : 0; n->attr = r->attr; }

/* resolve a type multiname to a class (NULL = any / unknown) */
static A2Obj *find_global_def(VM2 *vm, const A2Name *n);
static A2Obj *find_global_def_dom(VM2 *vm, const A2Name *n, int dom);
static void rt_to_name(RTName *r, A2Name *n);
static A2Class *rt_type(AbcRT *rt, u32 mn) {
    RTName *n;
    if (mn == 0 || mn >= rt->abc->mn_count) return NULL;
    n = &rt->names[mn];
    if (n->type_tried) return n->type;
    n->type_tried = 1;
    if (n->kind == MN_TYPENAME) { extern A2Class *a2_vector_class_for(VM2 *vm, A2Class *t); AbcMultiname *m = &rt->abc->multinames[mn]; A2Class *et = NULL;
        if (m->type_param_count > 0) { u32 pm = m->type_params[0]; if (pm && pm < rt->abc->mn_count && strcmp(rt->names[pm].name->s, "*")) { et = rt_type(rt, pm); if (!et) { n->type_tried = 0; return NULL; } } }
        n->type = et ? a2_vector_class_for(rt->vm, et) : rt->vm->c_vector; return n->type; }
    { int i, dom = a2_abc_domain(rt->abc); for (i = 0; i < n->nns; i++) { A2Class *c = a2_class_find_dom(rt->vm, n->nss[i].uri, n->name, dom); if (c) { n->type = c; return c; } } }
    n->type = a2_class_find_dom(rt->vm, NULL, n->name, a2_abc_domain(rt->abc));
    if (n->type && n->nns && n->type->ns.uri && n->type->ns.uri->len) {
        /* a same-named class of another package (flash.media.Camera for a game's Camera) is not
           this type: the right one may be defined by a script that has not run yet */
        int i, ok = 0; for (i = 0; i < n->nns; i++) if (n->nss[i].uri == n->type->ns.uri) ok = 1;
        if (!ok) {
            A2Name q; n->type = NULL; rt_to_name(n, &q);
            if (!rt->vm->throwing && find_global_def_dom(rt->vm, &q, a2_abc_domain(rt->abc))) for (i = 0; i < n->nns && !n->type; i++) n->type = a2_class_find_dom(rt->vm, n->nss[i].uri, n->name, a2_abc_domain(rt->abc));
        }
    }
    if (!n->type) n->type_tried = 0;   /* may be defined later */
    return n->type;
}

static int rt_name_str(AbcRT *rt, u32 mn, char *buf, size_t cap);
A2Str *a2_rt_type_name(VM2 *vm, AbcFile *abc, u32 mn) {
    char *buf; AbcRT *rt = abc ? RT(abc) : NULL; A2Str *r;
    if (!rt || mn == 0 || mn >= abc->mn_count) return a2_str(vm, "*");
    buf = (char*)malloc(65536); if (!buf) return a2_str(vm, "*");
    rt_name_str(rt, mn, buf, 65536);
    r = a2_str(vm, buf); free(buf);
    return r;
}
static int rt_name_str(AbcRT *rt, u32 mn, char *buf, size_t cap) {
    AbcMultiname *m; RTName *n; A2Class *c;
    if (mn == 0 || mn >= rt->abc->mn_count) { strcpy(buf, "*"); return 0; }
    m = &rt->abc->multinames[mn]; n = &rt->names[mn];
    if (m->kind == MN_TYPENAME) {
        char *inner = (char*)malloc(cap);
        if (!inner || cap < 32) { free(inner); strcpy(buf, "*"); return 0; }
        if (m->type_param_count > 0 && m->type_params[0] != mn) rt_name_str(rt, m->type_params[0], inner, cap - 24); else strcpy(inner, "*");
        sprintf(buf, "__AS3__.vec::Vector.<%s>", inner); free(inner); return 1;
    }
    if (n->name == rt->vm->s_empty || !strcmp(n->name->s, "*")) { strcpy(buf, "*"); return 0; }
    c = rt_type(rt, mn);
    if (c) { A2Str *s = a2_class_name(rt->vm, c, 1); strncpy(buf, s->s, cap - 1); buf[cap - 1] = 0; return 1; }
    if (n->nns == 1 && n->nss[0].uri->len && n->nss[0].uri->len + n->name->len + 3 < cap) sprintf(buf, "%s::%s", n->nss[0].uri->s, n->name->s); else { strncpy(buf, n->name->s, cap - 1); buf[cap - 1] = 0; }
    return 1;
}
void a2_report_uncaught(VM2 *vm);
/* a property name a Vector reads as a number (Rust's f64 parse, as Ruffle) */
static int vec_name_num(const A2Str *s, double *out) {
    const char *p = s->s, *q; char *e; size_t n = s->len, i;
    if (!n || n > 64) return 0;
    q = (*p == '+' || *p == '-') ? p + 1 : p;
    if (!strcmp(q, "inf") || !strcmp(q, "infinity") || !strcmp(q, "Infinity") || !strcmp(q, "NaN") || !strcmp(q, "nan")) { *out = strtod(p, NULL); if (!strcmp(q, "NaN") || !strcmp(q, "nan")) *out = 0.0 / 0.0; else *out = *p == '-' ? -1e308 * 10 : 1e308 * 10; return 1; }
    for (i = 0; i < n; i++) if (!strchr("0123456789+-.eE", p[i])) return 0;
    *out = strtod(p, &e);
    return e == p + n && e != p;
}
static void vec_oob(VM2 *vm, double idx, int len) { char b[120], nb[40]; v2_num_to_cstr(idx, nb); sprintf(b, "The index %s is out of range %d.", nb, len); a2_throw_error(vm, "RangeError", b, 1125); }
static A2Obj *find_global_def(VM2 *vm, const A2Name *n);
static void rt_to_name(RTName *r, A2Name *n);
/* interfaces may be defined (newclass) after the class that implements them: re-resolve lazily */
void a2_resolve_ifaces(A2Class *c) {
    AbcRT *rt; u32 i;
    if (!c || !c->inst || !c->abc || !RT(c->abc) || c->nifaces >= (int)c->inst->interface_count) return;
    rt = RT(c->abc);
    free(c->ifaces); c->ifaces = NULL; c->nifaces = 0;
    for (i = 0; i < c->inst->interface_count; i++) {
        u32 mn = c->inst->interfaces[i];
        A2Class *ic = rt_type(rt, mn);
        if (!ic && mn < rt->abc->mn_count) { A2Name n; rt_to_name(&rt->names[mn], &n); if (find_global_def_dom(rt->vm, &n, a2_abc_domain(rt->abc))) ic = rt_type(rt, mn); }
        if (ic) { c->ifaces = (A2Class**)realloc(c->ifaces, (size_t)(c->nifaces + 1) * sizeof(A2Class*)); c->ifaces[c->nifaces++] = ic; }
    }
}
A2Class *a2_resolve_trait_type(VM2 *vm, A2Trait *t) {
    (void)vm;
    if (!t->type && t->type_mn && t->abc && RT(t->abc)) t->type = rt_type(RT(t->abc), t->type_mn);
    return t->type;
}
A2Method *a2_rt_method_peek(void *rtp, u32 i) { AbcRT *rt = (AbcRT*)rtp; return rt && i < rt->abc->method_count ? rt->methods[i] : NULL; }
static A2Method *rt_method(AbcRT *rt, u32 i) {
    A2Method *m; AbcMethod *am;
    if (i >= rt->abc->method_count) return NULL;
    if (rt->methods[i] && rt->fresh) {
        /* newclass run again on the same class: the new class object gets its own copies
           of the methods so they see its scope and superclass */
        A2Method **nx = (A2Method**)realloc(rt->extra, (size_t)(rt->nextra + 1) * sizeof(A2Method*));
        if (!nx) return rt->methods[i];
        rt->extra = nx;
        m = (A2Method*)malloc(sizeof(A2Method));
        if (!m) return rt->methods[i];
        *m = *rt->methods[i]; m->owner = NULL; m->verify_state = 0;
        rt->extra[rt->nextra++] = m;
        return m;
    }
    if (rt->methods[i]) return rt->methods[i];
    am = &rt->abc->methods[i];
    m = (A2Method*)calloc(1, sizeof(A2Method));
    m->abc = rt->abc; m->index = i; m->name = rt_str(rt, am->name); m->rt = rt;
    m->nparams = (int)am->param_count;
    m->has_rest = !!(am->flags & MF_NEED_REST); m->need_args = !!(am->flags & MF_NEED_ARGUMENTS); m->need_activation = !!(am->flags & MF_NEED_ACTIVATION);
    rt->methods[i] = m;
    return m;
}

static V2 const_value(AbcRT *rt, u8 kind, u32 idx) {
    AbcFile *a = rt->abc;
    switch (kind) {
    case CK_INT: return v2_int(idx < a->int_count ? a->ints[idx] : 0);
    case CK_UINT: return v2_uint(idx < a->uint_count ? a->uints[idx] : 0);
    case CK_DOUBLE: return v2_num(idx < a->double_count ? a->doubles[idx] : 0);
    case CK_UTF8: return v2_str(rt_str(rt, idx));
    case CK_TRUE: return v2_bool(1);
    case CK_FALSE: return v2_bool(0);
    case CK_NULL: return v2_null();
    case CK_UNDEFINED: return v2_undef();
    case CK_NAMESPACE: case CK_PACKAGE_NS: case CK_PKG_INTERNAL_NS: case CK_PROTECTED_NS: case CK_EXPLICIT_NS: case CK_STATIC_PROTECTED_NS: case CK_PRIVATE_NS: {
        A2Obj *o = a2_new(rt->vm, rt->vm->c_namespace); o->kind = OK_NAMESPACE;
        o->nsval = rt->nss[idx < a->ns_count ? idx : 0];
        return v2_obj(o); }
    }
    return v2_undef();
}

/* ---------------------------------------------------------------- */
/* errors                                                           */

A2Str *a2_stack_trace(VM2 *vm);
A2Obj *a2_new_error(VM2 *vm, const char *cls, const char *msg, int id) {
    A2Class *c = a2_class_find_s(vm, cls); A2Obj *o; V2 args[2];
    if (!c) c = vm->c_error;
    args[0] = v2_cstr(vm, msg); args[1] = v2_int(id);
    if (vm->in_throw || vm->depth > A2_MAX_DEPTH - 50) {
        /* no room to run constructors: build the error by hand */
        o = a2_new(vm, c); o->kind = OK_ERROR;
        a2_dyn_set(vm, o, vm->s_message, args[0]); a2_dyn_set(vm, o, a2_str(vm, "errorID"), args[1]);
        a2_dyn_set_hidden(vm, o, a2_str(vm, "__stackTrace"), v2_str(a2_stack_trace(vm)));
        { A2Trait *t = a2_find_trait_any(&c->itraits, vm->s_message); if (t && t->slot < o->nslots) o->slots[t->slot] = args[0]; t = a2_find_trait_any(&c->itraits, a2_str(vm, "errorID")); if (t && t->slot < o->nslots) o->slots[t->slot] = args[1]; }
        return o;
    }
    vm->in_throw = 1;
    { V2 r = a2_construct_class(vm, c, 2, args); o = r.t == T_OBJ ? r.u.o : a2_new(vm, c); }
    vm->in_throw = 0;
    return o;
}
A2Str *a2_method_trace_name(VM2 *vm, A2Method *m);
static char *g_nonerr_stack;   /* the stack trace where a non-Error value was last thrown */
const char *g_a2_stack_base;  /* native frames below the AS3 stack (a setTimeout callback's timer) */
void a2_throw(VM2 *vm, V2 v) {
    if (vm->throwing) return;
    if (!(v.t == T_OBJ && v.u.o->kind == OK_ERROR)) { A2Str *st = a2_stack_trace(vm); free(g_nonerr_stack); g_nonerr_stack = (char*)malloc(st->len + 1); if (g_nonerr_stack) { memcpy(g_nonerr_stack, st->s, st->len); g_nonerr_stack[st->len] = 0; } }
    vm->throwing = 1; vm->exc = v;
    if (vm->player && vm->player->debug > 1 && vm->depth >= 0) { A2Method *cm = vm->cs[vm->depth]; fprintf(stderr, "[avm2] throw in %s", cm ? a2_method_trace_name(vm, cm)->s : (vm->cur && vm->cur->m ? a2_method_trace_name(vm, vm->cur->m)->s : "?")); if (v.t == T_OBJ && v.u.o->cls && vm->c_error && a2_is_subclass(v.u.o->cls, vm->c_error)) { V2 mv; vm->throwing = 0; mv = a2_get_s(vm, v, "message"); vm->throwing = 1; vm->exc = v; if (mv.t == T_STR) fprintf(stderr, ": %.200s", mv.u.s->s); if (vm->player->debug > 2) { A2Str *st = a2_stack_trace(vm); if (st) fprintf(stderr, "%.600s", st->s); } } fprintf(stderr, "\n"); }
}
/* throw error `id` with its standard message, %1/%2 substituted */
void a2_throw_id(VM2 *vm, const char *cls, int id, const char *a1, const char *a2) {
    extern const char *a2_error_message(int id); const char *m = a2_error_message(id); char buf[900]; const char *p; size_t n = 0;
    if (!m) { a2_throw_error(vm, cls, "", id); return; }
    for (p = m; *p && n < 800; p++) {
        if (*p == '%' && (p[1] == '1' || p[1] == '2')) { const char *s = p[1] == '1' ? a1 : a2; if (s) { size_t l = strlen(s); if (l > 380) l = 380; memcpy(buf + n, s, l); n += l; } p++; continue; }
        buf[n++] = *p;
    }
    buf[n] = 0; a2_throw_error(vm, cls, buf, id);
}
/* a multiname as avmplus prints it in error messages: "uri::name" for explicit namespaces */
static const char *name_fmt(VM2 *vm, const A2Name *n, char *buf) {
    if (n->nns == 1 && (n->nss[0].kind == NS_NAMESPACE || n->nss[0].kind == NS_EXPLICIT) && n->nss[0].uri && n->nss[0].uri->len && n->nss[0].uri != vm->s_AS3) { sprintf(buf, "%.180s::%.180s", n->nss[0].uri->s, n->name->s); return buf; }
    return n->name->s;
}
static const char *obj_cls_fmt(VM2 *vm, V2 obj) {
    A2Class *c;
    if (obj.t == T_OBJ && obj.u.o->kind == OK_CLASS) { static char b[400]; sprintf(b, "%.390s$", a2_class_name_dotted(vm, obj.u.o->as_class)->s); return b; }
    if (obj.t == T_INT || obj.t == T_UINT || obj.t == T_NUM) return "Number";   /* numeric values report as Number */
    c = v2_class_of(vm, obj); return a2_class_name_dotted(vm, c)->s;
}
void a2_throw_readonly(VM2 *vm, V2 obj, const A2Name *n) { char nb[400]; a2_throw_id(vm, "ReferenceError", 1074, name_fmt(vm, n, nb), obj_cls_fmt(vm, obj)); }
/* named with the trait's own namespace when it has a user namespace */
static void throw_readonly_t(VM2 *vm, V2 obj, const A2Name *n, const A2Trait *t) {
    A2Name tn = *n; A2Ns un;
    if (t && t->ns.kind == NS_NAMESPACE) { tn.nss = (A2Ns*)&t->ns; tn.nns = 1; }
    /* a user namespace the compiler emitted as package-internal ("pkg:ns") */
    else if (t && t->ns.kind == NS_PACKAGE_INTERNAL && t->ns.uri && memchr(t->ns.uri->s, ':', t->ns.uri->len)) { un = t->ns; un.kind = NS_NAMESPACE; tn.nss = &un; tn.nns = 1; }
    a2_throw_readonly(vm, obj, &tn);
}
void a2_throw_cannot_create(VM2 *vm, V2 obj, const A2Name *n) { char nb[400]; a2_throw_id(vm, "ReferenceError", 1056, name_fmt(vm, n, nb), obj_cls_fmt(vm, obj)); }
static int name_has_public(VM2 *vm, const A2Name *n) { int i; if (n->any_ns) return 1; for (i = 0; i < n->nns; i++) if (n->nss[i].uri == vm->s_empty && n->nss[i].kind != NS_PRIVATE) return 1; return 0; }
void a2_throw_not_found(VM2 *vm, V2 obj, const A2Name *n) { char nb[400]; a2_throw_id(vm, "ReferenceError", 1069, name_fmt(vm, n, nb), obj_cls_fmt(vm, obj)); }

/* debugger-style method names: Class/name, Class$/static, Class/get name, Class(), Class$cinit, global$init, MethodInfo-N */
static const char *trace_class_name(VM2 *vm, A2Class *c, char *buf) {
    if (a2_is_subclass(c, vm->c_vector)) {
        A2Class *et = c == vm->c_vector ? NULL : a2_vec_elem_type(vm, c);
        return et == vm->c_int ? "Vector$int" : et == vm->c_uint ? "Vector$uint" : et == vm->c_number ? "Vector$double" : "Vector$object";
    }
    strncpy(buf, a2_class_name(vm, c, 1)->s, 255); buf[255] = 0; return buf;
}
A2Str *a2_method_trace_name(VM2 *vm, A2Method *m) {
    char buf[600], cb[256]; A2Class *c; int b; A2Trait *t;
    if (m->dbg_name) return m->dbg_name;
    c = m->owner;
    if (m->is_script_init) { m->dbg_name = a2_str(vm, "global$init"); return m->dbg_name; }
    if (c && c->kind != OK_GLOBAL) {
        const char *cn = trace_class_name(vm, c, cb);
        if (m == c->iinit) { m->dbg_name = a2_str(vm, cn); return m->dbg_name; }
        if (m == c->cinit) { sprintf(buf, "%.120s$cinit", cn); m->dbg_name = a2_str(vm, buf); return m->dbg_name; }
        for (b = 0; b < c->itraits.nb; b++) for (t = c->itraits.b[b]; t; t = t->next) if (t->method == m || t->getter == m || t->setter == m) {
            const char *uri = t->ns.uri && t->ns.uri->len && (t->ns.kind == NS_NAMESPACE || t->ns.kind == NS_EXPLICIT) ? t->ns.uri->s : (m->native && t->kind == TK_METHOD && (a2_is_subclass(c, vm->c_vector) || c == vm->c_array || c == vm->c_string || c == vm->c_number || c == vm->c_int || c == vm->c_uint || c == vm->c_boolean || c == vm->c_object || c == vm->c_function || c == vm->c_date || c == vm->c_regexp || c == vm->c_xml || c == vm->c_xmllist) ? "http://adobe.com/AS3/2006/builtin" : NULL);
            sprintf(buf, "%.120s/%.120s%.120s%.120s%.120s", cn, t->getter == m ? "get " : t->setter == m ? "set " : "", uri ? uri : "", uri ? "::" : "", t->name->s);
            m->dbg_name = a2_str(vm, buf); return m->dbg_name; }
        for (b = 0; b < c->ctraits.nb; b++) for (t = c->ctraits.b[b]; t; t = t->next) if (t->method == m || t->getter == m || t->setter == m) {
            const char *uri = t->ns.uri && t->ns.uri->len && (t->ns.kind == NS_NAMESPACE || t->ns.kind == NS_EXPLICIT) ? t->ns.uri->s : NULL;
            sprintf(buf, "%.120s$/%.120s%.120s%.120s%.120s", cn, t->getter == m ? "get " : t->setter == m ? "set " : "", uri ? uri : "", uri ? "::" : "", t->name->s);
            m->dbg_name = a2_str(vm, buf); return m->dbg_name; }
    }
    if (m->native) { sprintf(buf, "global/%.120s", m->name ? m->name->s : "?"); m->dbg_name = a2_str(vm, buf); return m->dbg_name; }
    /* closures: declaring traits are Function's; name comes from the method_info name (index 0 = none) */
    if (m->abc && m->index < m->abc->method_count && m->abc->methods[m->index].name != 0) {
        if (m->name->len == 0) m->dbg_name = a2_str(vm, "Function/<anonymous>");
        else { sprintf(buf, "Function/%.500s", m->name->s); m->dbg_name = a2_str(vm, buf); }
        return m->dbg_name;
    }
    sprintf(buf, "MethodInfo-%u", m->index); m->dbg_name = a2_str(vm, buf); return m->dbg_name;
}
/* movie of the innermost running ABC code (the main movie outside scripts) */
struct SWFMovie *a2_code_movie(VM2 *vm) {
    int d;
    for (d = vm->depth; d >= 1; d--) { A2Method *m = vm->cs[d]; if (m && m->abc && m->abc->movie) return (struct SWFMovie*)m->abc->movie; }
    return PMOV(vm->player);
}
A2Str *a2_stack_trace(VM2 *vm) {
    char *buf = NULL; size_t n = 0, cap = 0; int d; A2Str *r;
    if (vm->throw_frame) { cap = 128; buf = (char*)malloc(cap); n = (size_t)sprintf(buf, "\n\tat %.80s()", vm->throw_frame); }
    else if (vm->throw_from_native) { cap = 64; buf = (char*)malloc(cap); n = (size_t)sprintf(buf, "\n\tat Error$/throwError()"); }
    for (d = vm->depth; d >= 1; d--) {
        A2Method *m = vm->cs[d]; A2Str *nm; size_t need;
        if (!m) {
            char cb[300]; const char *cn;
            if (!vm->cs_cls[d] || d == vm->depth_ctor_skip || a2_is_subclass(vm->cs_cls[d], vm->c_error)) continue;
            /* C++-only core classes have no constructor frame */
            if (vm->cs_cls[d] == vm->c_xml || vm->cs_cls[d] == vm->c_xmllist || vm->cs_cls[d] == vm->c_regexp || vm->cs_cls[d] == vm->c_date) continue;
            cn = trace_class_name(vm, vm->cs_cls[d], cb);
            need = n + strlen(cn) + 12;
            if (need + 1 > cap) { cap = need * 2 + 64; buf = (char*)realloc(buf, cap); }
            n += (size_t)sprintf(buf + n, "\n\tat %s()", cn);
            continue;
        }
        if (m->owner && m == m->owner->iinit && m->owner->kind != OK_GLOBAL && a2_is_subclass(m->owner, vm->c_error)) continue;   /* constructors of Error subclasses are not in the trace */
        nm = a2_method_trace_name(vm, m);
        need = n + nm->len + 8 + (vm->debugger && vm->cs_file[d] ? vm->cs_file[d]->len + 24 : 0);
        if (need + 1 > cap) { cap = need * 2 + 64; buf = (char*)realloc(buf, cap); }
        n += (size_t)sprintf(buf + n, "\n\tat %s()", nm->s);
        if (vm->debugger && vm->cs_file[d] && !m->native) {   /* the debug player: [source file:line] ("dir;;file.as" -> "dir\file.as") */
            const char *f = vm->cs_file[d]->s; buf[n++] = '[';
            while (*f) { if (f[0] == ';' && f[1] == ';') { buf[n++] = '\\'; f += 2; } else buf[n++] = *f++; }
            n += (size_t)sprintf(buf + n, ":%d]", vm->cs_line[d]);
        }
    }
    if (g_a2_stack_base) { size_t l = strlen(g_a2_stack_base); if (n + l + 1 > cap) { cap = n + l + 64; buf = (char*)realloc(buf, cap); } memcpy(buf + n, g_a2_stack_base, l + 1); n += l; }
    r = buf ? a2_intern(vm, buf, n) : vm->s_empty; free(buf); return r;
}
void a2_throw_error(VM2 *vm, const char *cls, const char *msg, int id) {
    char buf[4096]; A2Obj *o;
    if (vm->throwing) return;
    if (id) sprintf(buf, "Error #%d: %.4000s", id, msg); else { strncpy(buf, msg, 4095); buf[4095] = 0; }
    /* errors raised by natives come from Error.throwError in Flash's glue code */
    {   /* error ids Flash raises through Error.throwError in its AS3 glue (the others come from C++) */
        static const int glue[] = { 12, 15, 1004, 1005, 1044, 1074, 1075, 1234, 2014, 2066, 2071, 2088, 2089, 2090, 2091, 2092, 2093, 2105, 2106, 2107, 2143, 2144, 2181, 3683, 3723, 0 };
        int k, g = 0;
        for (k = 0; glue[k]; k++) if (glue[k] == id) { g = 1; break; }
        vm->throw_from_native = g && vm->depth >= 1 && (vm->cs[vm->depth] ? vm->cs[vm->depth]->native != NULL : vm->cs_cls[vm->depth] != NULL);
    }
    vm->depth_ctor_skip = vm->depth + 1;   /* the error's own constructor frame */
    o = a2_new_error(vm, cls, buf, id);
    vm->throw_from_native = 0; vm->depth_ctor_skip = 0;
    a2_throw(vm, v2_obj(o));
}

/* ---------------------------------------------------------------- */
/* scopes                                                           */

/* saved scope chain; ival holds the `with` flags (bit i = objs[i] was pushed with pushwith) */
A2Obj *a2_new_scope(VM2 *vm, A2Obj **objs, int n) {
    A2Obj *s = a2_new(vm, NULL); int i;
    s->kind = OK_SCOPE;
    for (i = 0; i < n; i++) {
        a2_arr_push(vm, s, v2_obj(objs[i]));
        if (objs + i >= vm->scope_stack && objs + i < vm->scope_stack + A2_SCOPES && vm->scope_with[objs + i - vm->scope_stack] && i < 31) s->ival |= 1 << i;
    }
    return s;
}

V2 a2_bound_fn(VM2 *vm, A2Method *m, A2Obj *scope, V2 self) {
    A2Obj *f = a2_new_fn(vm, m, scope);
    f->receiver = self; f->is_bound = 1;
    return v2_obj(f);
}

/* ---------------------------------------------------------------- */
/* property protocol                                                */

static A2Class *class_scope_owner(A2Method *m) { return m ? m->owner : NULL; }

static A2Obj *scope_for_method(VM2 *vm, A2Method *m, A2Obj *given) {
    (void)vm;
    if (m && m->is_static && m->owner && m->owner->cscope && (!given || given == m->owner->scope)) return m->owner->cscope;
    if (given) return given;
    if (m && m->owner) return m->owner->scope;
    return NULL;
}

/* own property of an object: traits then dynamic.  returns 1 found. */
/* duplicate slot names along the class chain: code of a superclass reads its own slot on `this` (Flash binds it early) */
extern int a2_ns_eq(const A2Ns *a, const A2Ns *b);
static A2Trait *own_trait(VM2 *vm, A2Obj *o, const A2Name *n) {
    A2Trait *t;
    if (o->cls->shadow_slots > 0 && vm->cur && vm->cur->self.t == T_OBJ && vm->cur->self.u.o == o && vm->cur->m && vm->cur->m->owner && vm->cur->m->owner != o->cls && !vm->cur->m->is_static) {
        A2Trait *t0 = a2_find_trait(&o->cls->itraits, n->name, n);
        t = a2_find_trait(&vm->cur->m->owner->itraits, n->name, n);
        if (t && t0 && t != t0 && (t->kind == TK_SLOT || t->kind == TK_CONST) && (t0->kind == TK_SLOT || t0->kind == TK_CONST) && a2_ns_eq(&t->ns, &t0->ns)) return t;
        return t0;
    }
    return a2_find_trait(&o->cls->itraits, n->name, n);
}
/* Date.prototype and Array.prototype are instances of their class in avmplus: an explicit
   AS3-namespace reference on them reaches the class's methods */
static A2Trait *proto_as3_method(VM2 *vm, A2Obj *o, const A2Name *n, A2Class **pc) {
    A2Class *c = NULL; A2Trait *t; int i, as3 = 0;
    if (vm->c_date && o == vm->c_date->prototype) c = vm->c_date; else if (vm->c_array && o == vm->c_array->prototype) c = vm->c_array;
    if (!c || n->any_ns) return NULL;
    for (i = 0; i < n->nns; i++) if (n->nss[i].kind == NS_NAMESPACE && n->nss[i].uri == vm->ns_as3.uri) as3 = 1;
    if (!as3) return NULL;
    t = a2_find_trait(&c->itraits, n->name, n);
    if (!t || t->kind != TK_METHOD) return NULL;
    *pc = c; return t;
}
/* arr.public::push(): the generic prototype function, not the AS3 method (push/pop/shift/unshift) */
static int arr_public_generic(VM2 *vm, A2Obj *o, const A2Name *n) {
    const char *s;
    if (o->kind != OK_ARRAY || n->any_ns || n->nns != 1 || n->nss[0].kind != NS_PACKAGE || n->nss[0].uri->len) return 0;
    s = n->name->s; (void)vm;
    return !strcmp(s, "push") || !strcmp(s, "pop") || !strcmp(s, "shift") || !strcmp(s, "unshift");
}
static int get_own(VM2 *vm, A2Obj *o, const A2Name *n, V2 *out, int *is_method) {
    A2Trait *t; A2Prop *p;
    *is_method = 0;
    { A2Class *pc; t = proto_as3_method(vm, o, n, &pc); if (t) { *out = a2_bound_fn(vm, t->method, NULL, v2_obj(o)); *is_method = 1; return 1; } }
    if (o->kind == OK_CLASS) {
        A2Class *c;
        for (c = o->as_class; c; c = A2_STATIC_SUPER(c)) {
            t = a2_find_trait(&c->ctraits, n->name, n);
            if (t) {
                switch (t->kind) {
                case TK_SLOT: case TK_CONST: case TK_CLASS: *out = t->slot < c->obj->nslots ? c->obj->slots[t->slot] : v2_undef(); return 1;
                case TK_METHOD: *out = a2_bound_fn(vm, t->method, t->method->owner ? t->method->owner->scope : NULL, v2_obj(o)); *is_method = 1; return 1;
                case TK_GETTER: case TK_SETTER:
                    if (!t->getter) { char nb[400]; a2_throw_id(vm, "ReferenceError", 1077, name_fmt(vm, n, nb), obj_cls_fmt(vm, v2_obj(o))); return 1; }
                    *out = a2_call_method(vm, t->getter, t->getter->owner ? t->getter->owner->scope : NULL, v2_obj(o), 0, NULL); return 1;
                }
            }
        }
        if (n->name == vm->s_prototype) { *out = v2_obj(o->as_class->prototype); return 1; }
        if (n->name == vm->s_length) {
            A2Class *lc = o->as_class;   /* the constructor's arity (Date and Namespace declare more than they need) */
            *out = v2_int(lc->iinit ? lc->iinit->nparams : 0); return 1;
        }
    }
    if (o->cls && !arr_public_generic(vm, o, n)) {
        t = own_trait(vm, o, n);
        if (t) {
            if (vm->sb_str && o == vm->sb_obj && t->slot == vm->sb_slot) a2_sb_publish(vm);
            switch (t->kind) {
            case TK_SLOT: case TK_CONST: case TK_CLASS: *out = t->slot < o->nslots ? o->slots[t->slot] : v2_undef();
                if (t->kind == TK_CLASS && out->t == T_UNDEF) *out = v2_null();   /* a class slot is typed Class: null until initialised */
                return 1;
            case TK_METHOD: *out = a2_bound_fn(vm, t->method, t->method->owner ? t->method->owner->scope : NULL, v2_obj(o)); *is_method = 1; return 1;
            case TK_GETTER: case TK_SETTER:
                if (!t->getter) { char nb[400]; a2_throw_id(vm, "ReferenceError", 1077, name_fmt(vm, n, nb), obj_cls_fmt(vm, v2_obj(o))); *out = v2_undef(); return 1; }
                *out = a2_call_method(vm, t->getter, t->getter->owner ? t->getter->owner->scope : NULL, v2_obj(o), 0, NULL); return 1;
            }
        }
    }
    if (o->kind == OK_BYTEARRAY) { u32 idx; if (a2_is_index(n->name, &idx)) { if (idx < (u32)o->blen) { *out = v2_int(o->bytes[idx]); return 1; } } }
    if (o->kind == OK_ARRAY || o->kind == OK_VECTOR) {
        u32 idx;
        if (n->name == vm->s_length) { *out = a2_arr_length_val(o); return 1; }
        if (a2_is_index(n->name, &idx)) {
            if (a2_arr_sealed(vm, o)) return 0;   /* sealed Array subclass: elements are not properties */
            if (idx < (u32)o->alen && (o->arr[idx].t != T_HOLE || o->kind == OK_VECTOR)) { *out = o->arr[idx]; return 1; }
            if (o->kind == OK_VECTOR) { vec_oob(vm, (double)idx, o->alen); *out = v2_undef(); return 1; }
            if (idx < (u32)o->alen) return 0;   /* hole: not own */
            p = a2_dyn_find(o, n->name); if (p) { *out = p->v; return 1; }
            return 0;
        }
        if (o->kind == OK_VECTOR) {
            double d;
            if (vec_name_num(n->name, &d)) {
                if (d >= 0 && d < 4294967296.0 && d == floor(d)) { idx = (u32)d; if (idx < (u32)o->alen) { *out = o->arr[idx]; return 1; } vec_oob(vm, d, o->alen); *out = v2_undef(); return 1; }
                if (vm->swf_version >= 11) { vec_oob(vm, d, o->alen); *out = v2_undef(); return 1; }
                a2_throw_not_found(vm, v2_obj(o), n); *out = v2_undef(); return 1;   /* numeric non-index names never reach the prototype */
            }
        }
    }
    if (o->kind == OK_FUNCTION) {
        if (n->name == vm->s_prototype) {
            p = a2_dyn_find(o, vm->s_prototype);
            if (!p) { A2Obj *pr = a2_new_plain(vm); a2_dyn_set_hidden(vm, pr, vm->s_constructor, v2_obj(o)); a2_dyn_set(vm, o, vm->s_prototype, v2_obj(pr)); p = a2_dyn_find(o, vm->s_prototype); }
            *out = p->v.t == T_NULL ? v2_undef() : p->v; return 1;   /* a null prototype reads back as undefined */
        }
        if (n->name == vm->s_length) { *out = v2_int(o->method ? o->method->nparams : 0); return 1; }
    }
    if (o->kind == OK_DICT) { extern int a2_dict_get(VM2 *vm, A2Obj *o, V2 k, V2 *out); if (!name_has_public(vm, n)) return 0; return a2_dict_get(vm, o, v2_str(n->name), out); }
    p = a2_dyn_find(o, n->name);
    if (p) { *out = p->v; return 1; }
    return 0;
}

static int proto_get(VM2 *vm, A2Obj *proto, const A2Name *n, V2 *out) {
    int guard = 0;
    for (; proto && guard < 64; proto = proto->proto, guard++) {
        A2Prop *p = a2_dyn_find(proto, n->name);
        if (p) { *out = p->v; return 1; }
        if (proto->kind == OK_ARRAY || proto->kind == OK_VECTOR) { u32 idx; if (a2_is_index(n->name, &idx) && idx < (u32)proto->alen && proto->arr[idx].t != T_HOLE) { *out = proto->arr[idx]; return 1; } }   /* holes continue up the chain */
    }
    (void)vm;
    return 0;
}

/* FLASHNT_TRACEPROP=name: log reads and writes of that property (debugging) */
static const char *traceprop(void) { static const char *t; static int init; if (!init) { init = 1; t = getenv("FLASHNT_TRACEPROP"); } return t; }
static int a2_get_impl(VM2 *vm, V2 obj, const A2Name *n, V2 *out);
int a2_get(VM2 *vm, V2 obj, const A2Name *n, V2 *out) {
    int r = a2_get_impl(vm, obj, n, out);
    if (traceprop() && n->name && !strcmp(n->name->s, traceprop())) { A2Str *vs = v2_tostr(vm, *out); int th = vm->throwing; vm->throwing = 0; fprintf(stderr, "[prop] get %s -> %s (found %d)\n", n->name->s, vs ? vs->s : "?", r); vm->throwing = th; }
    return r;
}
static int a2_get_impl(VM2 *vm, V2 obj, const A2Name *n, V2 *out) {
    A2Obj *o; int ism;
    *out = v2_undef();
    if (obj.t == T_OBJ) {
        o = obj.u.o;
        if (o->kind == OK_PROXY && n->any_ns == 2) return a2_proxy_get(vm, o, n, out);   /* *::name never binds to traits */
        if (get_own(vm, o, n, out, &ism)) return 1;
        if (o->kind == OK_PROXY) return a2_proxy_get(vm, o, n, out);
        if (proto_get(vm, o->proto, n, out)) return 1;
        if (o->kind == OK_CLASS && vm->c_class) {
            A2Trait *t = a2_find_trait(&vm->c_class->itraits, n->name, n);
            if (t && t->kind == TK_METHOD) { *out = a2_bound_fn(vm, t->method, NULL, obj); return 1; }
        }
        return 0;
    }
    if (obj.t == T_UNDEF || obj.t == T_NULL) {
        (void)n; throw_null_ref(vm, obj);
        return 0;
    }
    /* primitive: class traits, then prototype chain */
    {
        A2Class *c = (obj.t == T_INT || obj.t == T_UINT || obj.t == T_NUM) ? vm->c_number : v2_class_of(vm, obj); A2Trait *t;   /* numbers share Number's traits and prototype */
        if (obj.t == T_STR) {
            u32 idx;
            if (n->name == vm->s_length) { extern int a2_str_ulen(A2Str *s); *out = v2_int(a2_str_ulen(obj.u.s)); return 1; }
            if (a2_is_index(n->name, &idx)) { extern V2 a2_string_char_at(VM2 *vm, A2Str *s, int i); *out = a2_string_char_at(vm, obj.u.s, (int)idx); return 1; }
        }
        t = c ? a2_find_trait(&c->itraits, n->name, n) : NULL;
        if (t) {
            if (t->kind == TK_METHOD) { *out = a2_bound_fn(vm, t->method, NULL, obj); return 1; }
            if (t->kind == TK_GETTER && t->getter) { *out = a2_call_method(vm, t->getter, NULL, obj, 0, NULL); return 1; }
        }
        if (c && proto_get(vm, c->prototype, n, out)) return 1;
        return 0;
    }
}

static int set_own(VM2 *vm, A2Obj *o, const A2Name *n, V2 v, int init) {
    A2Trait *t;
    { A2Class *pc; t = proto_as3_method(vm, o, n, &pc); if (t) { char nb[400]; a2_throw_id(vm, "ReferenceError", 1037, name_fmt(vm, n, nb), a2_class_name(vm, pc, 0)->s); return 1; } }
    if (o->kind == OK_CLASS) {
        A2Class *c;
        if (n->name == vm->s_prototype && !init) { a2_throw_readonly(vm, v2_obj(o), n); return 1; }
        for (c = o->as_class; c; c = A2_STATIC_SUPER(c)) {
            t = a2_find_trait(&c->ctraits, n->name, n);
            if (t) {
                switch (t->kind) {
                case TK_SLOT: if (t->slot < c->obj->nslots) { int was = vm->throwing; V2 cv = a2_coerce(vm, v, a2_resolve_trait_type(vm, t)); if (was || !vm->throwing) c->obj->slots[t->slot] = cv; } return 1;
                case TK_CONST: case TK_CLASS:
                    if (!init && !c->initing) { throw_readonly_t(vm, v2_obj(o), n, t); return 1; }
                    if (t->slot < c->obj->nslots) c->obj->slots[t->slot] = v; return 1;
                case TK_METHOD: { char nb[400]; a2_throw_id(vm, "ReferenceError", 1037, name_fmt(vm, n, nb), obj_cls_fmt(vm, v2_obj(o))); } return 1;
                case TK_GETTER: case TK_SETTER:
                    if (!t->setter) { throw_readonly_t(vm, v2_obj(o), n, t); return 1; }
                    a2_call_method(vm, t->setter, t->setter->owner ? t->setter->owner->scope : NULL, v2_obj(o), 1, &v); return 1;
                }
            }
        }
    }
    if (o->cls) {
        t = own_trait(vm, o, n);
        if (t) {
            switch (t->kind) {
            case TK_SLOT: if (t->slot < o->nslots) { int was = vm->throwing; V2 cv = a2_coerce(vm, v, a2_resolve_trait_type(vm, t)); if (was || !vm->throwing) o->slots[t->slot] = cv; } return 1;   /* a failed coercion leaves the slot unchanged */
            case TK_CONST: case TK_CLASS:
                if (!init) { throw_readonly_t(vm, v2_obj(o), n, t); return 1; }
                if (t->slot < o->nslots) o->slots[t->slot] = v; return 1;
            case TK_METHOD: { char nb[400]; a2_throw_id(vm, "ReferenceError", 1037, name_fmt(vm, n, nb), obj_cls_fmt(vm, v2_obj(o))); } return 1;
            case TK_GETTER: case TK_SETTER:
                if (!t->setter) { throw_readonly_t(vm, v2_obj(o), n, t); return 1; }
                a2_call_method(vm, t->setter, t->setter->owner ? t->setter->owner->scope : NULL, v2_obj(o), 1, &v); return 1;
            }
        }
    }
    if (o->kind == OK_PROXY) return a2_proxy_set(vm, o, n, v);
    if (o->kind == OK_BYTEARRAY) { u32 idx; if (a2_is_index(n->name, &idx) && idx < (1u << 30)) { extern void a2_bytearray_set(VM2 *vm, A2Obj *o, int i, int v); a2_bytearray_set(vm, o, (int)idx, v2_toint(vm, v)); return 1; } }   /* ba["3"] = v */
    if (o->kind == OK_ARRAY || o->kind == OK_VECTOR) {
        u32 idx;
        if (n->name == vm->s_length) {
            if (o->kind == OK_VECTOR && o->vec_fixed) { a2_throw_error(vm, "RangeError", "Cannot change the length of a fixed Vector", 1126); return 1; }
            if (a2_arr_sealed(vm, o)) return 1;   /* ignored on a sealed Array subclass */
            if (o->kind == OK_ARRAY) { double d = v2_tonum(vm, v); u32 nl; if (d != d || d - d != 0) d = 0; d = fmod(d < 0 ? -floor(-d) : floor(d), 4294967296.0); if (d < 0) d += 4294967296.0; nl = (u32)d; if (nl > (1u << 26)) { if ((u32)o->alen > nl) o->alen = (int)nl; o->biglen = nl; return 1; } a2_arr_setlen(vm, o, (int)nl); return 1; }
            a2_arr_setlen(vm, o, v2_toint(vm, v)); return 1;
        }
        if (a2_is_index(n->name, &idx)) {
            if (a2_arr_sealed(vm, o)) { a2_throw_cannot_create(vm, v2_obj(o), n); return 1; }
            if (o->kind == OK_ARRAY) { if (a2_arr_set_sparse(vm, o, idx, v)) return 1; a2_dyn_set(vm, o, n->name, v); return 1; }
            if (o->kind == OK_VECTOR) {
                if (idx > (u32)o->alen || (o->vec_fixed && idx >= (u32)o->alen)) { vec_oob(vm, (double)idx, o->alen); return 1; }
                v = a2_vec_coerce(vm, v, o->vec_type);
            }
            a2_arr_set(vm, o, (int)idx, v); return 1;
        }
        if (o->kind == OK_VECTOR) {
            double d;
            if (vec_name_num(n->name, &d)) {
                if (d >= 0 && d < 4294967296.0 && d == floor(d)) { idx = (u32)d; if (idx > (u32)o->alen || (o->vec_fixed && idx >= (u32)o->alen)) { vec_oob(vm, d, o->alen); return 1; } a2_arr_set(vm, o, (int)idx, a2_vec_coerce(vm, v, o->vec_type)); return 1; }
                if (vm->swf_version >= 11) { vec_oob(vm, d, o->alen); return 1; }
            }
        }
    }
    if (o->kind == OK_DICT) { extern void a2_dict_put(VM2 *vm, A2Obj *o, V2 k, V2 v); if (!name_has_public(vm, n) || (o->cls && o->cls->abc && !o->cls->is_dynamic)) {   /* sealed subclasses: no string keys */ a2_throw_cannot_create(vm, v2_obj(o), n); return 1; } a2_dict_put(vm, o, v2_str(n->name), v); return 1; }
    if (o->cls && !o->cls->is_dynamic && (o->kind != OK_PLAIN || o->cls->abc) && o->kind != OK_FUNCTION && o->kind != OK_CLASS && o->kind != OK_GLOBAL && o->kind != OK_SCOPE && (o->kind != OK_ERROR || o->cls->abc)) {
        a2_throw_cannot_create(vm, v2_obj(o), n); return 1;
    }
    a2_dyn_set(vm, o, n->name, v);
    return 1;
}

int a2_set(VM2 *vm, V2 obj, const A2Name *n, V2 v) {
    if (traceprop() && n->name && !strcmp(n->name->s, traceprop())) fprintf(stderr, "[prop] set %s = %s\n", n->name->s, v.t == T_OBJ ? "[object]" : v2_tostr(vm, v)->s);
    if (obj.t == T_OBJ) return set_own(vm, obj.u.o, n, v, 0);
    if (obj.t == T_UNDEF || obj.t == T_NULL) { throw_null_ref(vm, obj); return 0; }
    {   /* a primitive: its class's fixed properties are read-only methods/getters */
        A2Class *c = (obj.t == T_INT || obj.t == T_UINT || obj.t == T_NUM) ? vm->c_number : v2_class_of(vm, obj);
        A2Trait *t = c ? a2_find_trait(&c->itraits, n->name, n) : NULL;
        if (!t && obj.t == T_STR && n->name == vm->s_length) { a2_throw_readonly(vm, obj, n); return 0; }
        if (t && t->kind == TK_METHOD) { char nb[400]; a2_throw_id(vm, "ReferenceError", 1037, name_fmt(vm, n, nb), obj_cls_fmt(vm, obj)); return 0; }
        if (t && (t->kind == TK_GETTER || t->kind == TK_SETTER || t->kind == TK_CONST)) { a2_throw_readonly(vm, obj, n); return 0; }
    }
    a2_throw_cannot_create(vm, obj, n);
    return 0;
}
int a2_init(VM2 *vm, V2 obj, const A2Name *n, V2 v) {
    if (traceprop() && n->name && !strcmp(n->name->s, traceprop())) fprintf(stderr, "[prop] init %s = %s\n", n->name->s, v.t == T_OBJ ? "[object]" : v2_tostr(vm, v)->s);
    if (obj.t == T_OBJ) return set_own(vm, obj.u.o, n, v, 1);
    return a2_set(vm, obj, n, v);
}

int a2_has(VM2 *vm, V2 obj, const A2Name *n) {
    V2 tmp; int ism; A2Obj *o;
    if (obj.t != T_OBJ) {
        A2Class *c = v2_class_of(vm, obj);
        if (!c) return 0;
        if (a2_find_trait(&c->itraits, n->name, n)) return 1;
        return proto_get(vm, c->prototype, n, &tmp);
    }
    o = obj.u.o;
    if (o->kind == OK_XML) { extern int a2_xml_has(VM2 *vm, V2 obj, const A2Name *n); return a2_xml_has(vm, obj, n); }
    if (o->cls && a2_find_trait(&o->cls->itraits, n->name, n)) return 1;
    if (o->kind == OK_PROXY) return a2_proxy_has(vm, o, n);
    if (o->kind == OK_CLASS) { A2Class *c; for (c = o->as_class; c; c = A2_STATIC_SUPER(c)) if (a2_find_trait(&c->ctraits, n->name, n)) return 1; if (n->name == vm->s_prototype) return 1; }
    if (o->kind == OK_ARRAY || o->kind == OK_VECTOR) { u32 idx; if (n->name == vm->s_length) return 1; if (a2_is_index(n->name, &idx)) { if (idx < (u32)o->alen) return o->arr[idx].t != T_HOLE; if (o->kind == OK_VECTOR) return 0; return a2_dyn_find(o, n->name) != NULL; } }
    if (o->kind == OK_BYTEARRAY) { u32 idx; if (a2_is_index(n->name, &idx)) return idx < (u32)o->blen; }   /* index in byteArray */
    if (o->kind == OK_DICT) { int saved = vm->throwing; int r = get_own(vm, o, n, &tmp, &ism); vm->throwing = saved; return r; }
    if (a2_dyn_find(o, n->name)) return 1;
    return proto_get(vm, o->proto, n, &tmp);
}

int a2_delete(VM2 *vm, V2 obj, const A2Name *n) {
    A2Obj *o;
    if (obj.t != T_OBJ) return 0;
    o = obj.u.o;
    if (o->cls && a2_find_trait(&o->cls->itraits, n->name, n)) return 0;   /* fixed properties are never deletable */
    if (o->kind == OK_CLASS && n->name == vm->s_prototype) return 0;
    if (o->kind == OK_CLASS) { A2Class *c; for (c = o->as_class; c; c = A2_STATIC_SUPER(c)) if (a2_find_trait(&c->ctraits, n->name, n)) return 0; }
    if (o->kind == OK_PROXY) return a2_proxy_delete(vm, o, n);
    if (o->kind == OK_ARRAY && a2_arr_sealed(vm, o)) return 0;
    if (o->kind == OK_ARRAY) { u32 idx; if (a2_is_index(n->name, &idx)) { if (idx < (u32)o->alen) { o->arr[idx] = v2_hole(); return 1; } } }
    if (o->kind == OK_DICT && o->cls && o->cls->abc && !o->cls->is_dynamic) return 0;
    if (o->kind == OK_DICT) { extern int a2_dict_delete(VM2 *vm, A2Obj *o, V2 k); a2_dict_delete(vm, o, v2_str(n->name)); if (!a2_dyn_find(o, n->name)) return 1; }
    if (a2_dyn_del(o, n->name)) return 1;
    if (o->kind == OK_VECTOR) return 1;   /* delete on a Vector is a no-op that reports success */
    if (o->kind == OK_BYTEARRAY) return 0;
    if (o->kind == OK_PLAIN && o->cls && o->cls->abc && !o->cls->is_dynamic) return 0;   /* sealed user class instance */
    return (o->cls && o->cls->is_dynamic) || o->kind == OK_PLAIN || o->kind == OK_FUNCTION || o->kind == OK_GLOBAL || o->kind == OK_ACTIVATION;
}

V2 a2_call_prop(VM2 *vm, V2 obj, const A2Name *n, int argc, V2 *argv) {
    V2 f; A2Obj *o; A2Trait *t;
    if (obj.t == T_OBJ) {
        o = obj.u.o;
        /* fast path: method trait */
        if (o->kind == OK_CLASS) {
            A2Class *c;
            for (c = o->as_class; c; c = A2_STATIC_SUPER(c)) { t = a2_find_trait(&c->ctraits, n->name, n); if (t) break; }
            if (t && t->kind == TK_METHOD) return a2_call_method(vm, t->method, t->method->owner ? t->method->owner->scope : NULL, obj, argc, argv);
        } else if (o->cls && !arr_public_generic(vm, o, n)) {
            t = a2_find_trait(&o->cls->itraits, n->name, n);
            if (t && t->kind == TK_METHOD) return a2_call_method(vm, t->method, t->method->owner ? t->method->owner->scope : NULL, obj, argc, argv);
            if (!t && o->kind == OK_PROXY) return a2_proxy_call(vm, o, n, argc, argv);
            if (!t && o->kind == OK_XML) { extern int a2_xml_call_fallback(VM2 *vm, V2 obj, A2Name *n, int argc, V2 *argv, V2 *out); V2 r; if (a2_xml_call_fallback(vm, obj, (A2Name*)n, argc, argv, &r)) return r; if (vm->throwing) return v2_undef(); }
        }
    } else if (obj.t != T_UNDEF && obj.t != T_NULL) {
        /* numbers bind to the class of their (static) kind: int, uint or Number */
        A2Class *c = obj.t == T_INT ? vm->c_int : obj.t == T_UINT ? vm->c_uint : obj.t == T_NUM ? vm->c_number : v2_class_of(vm, obj);
        t = c ? a2_find_trait(&c->itraits, n->name, n) : NULL;
        if (t && t->kind == TK_METHOD) return a2_call_method(vm, t->method, NULL, obj, argc, argv);
    }
    if (!a2_get(vm, obj, n, &f)) {
        if (vm->throwing) return v2_undef();
        if (obj.t == T_OBJ && ((obj.u.o->kind == OK_PLAIN && (!obj.u.o->cls || obj.u.o->cls->is_dynamic || !obj.u.o->cls->abc)) || obj.u.o->kind == OK_ARRAY || obj.u.o->kind == OK_FUNCTION || obj.u.o->kind == OK_GLOBAL || (obj.u.o->cls && obj.u.o->cls->is_dynamic))) { a2_throw_error(vm, "TypeError", "value is not a function.", 1006); return v2_undef(); }
        a2_throw_not_found(vm, obj, n);
        return v2_undef();
    }
    if (vm->throwing) return v2_undef();
    if (f.t != T_OBJ || (f.u.o->kind != OK_FUNCTION && f.u.o->kind != OK_CLASS)) {
        char b[300]; sprintf(b, "value is not a function."); (void)n;
        a2_throw_error(vm, "TypeError", b, 1006);
        return v2_undef();
    }
    return a2_call(vm, f, obj, argc, argv);
}

V2 a2_construct_prop(VM2 *vm, V2 obj, const A2Name *n, int argc, V2 *argv) {
    V2 f;
    if (!a2_get(vm, obj, n, &f)) {
        if (vm->throwing) return v2_undef();
        if (obj.t == T_OBJ && !(obj.u.o->cls && obj.u.o->cls->is_dynamic)) { a2_throw_not_found(vm, obj, n); return v2_undef(); }
        f = v2_undef();     /* primitives and dynamic objects: undefined (then "not a constructor") */
    }
    if (vm->throwing) return v2_undef();
    if (!(f.t == T_OBJ && (f.u.o->kind == OK_CLASS || f.u.o->kind == OK_FUNCTION)) && vm->swf_version < 11 && vm->cs[vm->depth] && vm->cs[vm->depth]->is_script_init && obj.t == T_OBJ && obj.u.o->cls) {
        /* avmplus: interpreted code of SWF9/10 movies reports a non-constructible slot as 1115 */
        A2Trait *t = a2_find_trait(&obj.u.o->cls->itraits, n->name, n);
        if (t && (t->kind == TK_SLOT || t->kind == TK_CONST)) { a2_throw_id(vm, "TypeError", 1115, "value", NULL); return v2_undef(); }
    }
    return a2_construct(vm, f, argc, argv);
}

static void simple_name(VM2 *vm, A2Name *n, const char *name) { n->name = a2_str(vm, name); n->nss = &vm->ns_public; n->nns = 1; n->any_ns = 1; n->attr = 0; }
V2 a2_get_s(VM2 *vm, V2 obj, const char *name) { A2Name n; V2 r; simple_name(vm, &n, name); a2_get(vm, obj, &n, &r); return r; }
void a2_set_s(VM2 *vm, V2 obj, const char *name, V2 v) { A2Name n; simple_name(vm, &n, name); a2_set(vm, obj, &n, v); }
V2 a2_call_s(VM2 *vm, V2 obj, const char *name, int argc, V2 *argv) { A2Name n; simple_name(vm, &n, name); return a2_call_prop(vm, obj, &n, argc, argv); }

/* ---------------------------------------------------------------- */
/* coercion                                                         */

/* avmplus AvmCore::format(): how values print inside error messages */
A2Str *a2_error_fmt(VM2 *vm, V2 v) {
    char b[600]; A2Obj *o;
    switch (v.t) {
    case T_UNDEF: return a2_str(vm, "undefined");
    case T_NULL: return a2_str(vm, "null");
    case T_STR: { A2Str *s = v.u.s; char *t = (char*)malloc(s->len + 3); A2Str *r; t[0] = '"'; memcpy(t + 1, s->s, s->len); t[s->len + 1] = '"'; t[s->len + 2] = 0; r = a2_intern(vm, t, s->len + 2); free(t); return r; }
    case T_OBJ: break;
    default: return v2_tostr(vm, v);
    }
    o = v.u.o;
    if (o->kind == OK_PRIM && o->prim.t != T_UNDEF) return a2_error_fmt(vm, o->prim);
    if (o->kind == OK_CLASS) { sprintf(b, "%.500s$", a2_class_name(vm, o->as_class, 1)->s); return a2_str(vm, b); }
    if (o->kind == OK_NAMESPACE) return o->nsval.uri ? o->nsval.uri : vm->s_empty;
    if (o->kind == OK_FUNCTION) {
        if (o->is_bound && o->method) {   /* MethodClosure: receiver and method as in a stack trace (private namespaces hidden) */
            extern A2Str *a2_method_trace_name(VM2 *vm, A2Method *m); const char *mn = a2_method_trace_name(vm, o->method)->s, *pm = strstr(mn, "::");
            A2Class *oc = o->method->owner;
            if (pm && oc && oc->ns_private && !strchr(pm + 2, ':')) mn = pm + 2;
            sprintf(b, "MC{%.250s %.250s()}@00000000000", a2_error_fmt(vm, o->receiver)->s, mn); return a2_str(vm, b); }
        return a2_str(vm, "Function");
    }
    if (o->kind == OK_ARRAY) return a2_str(vm, "[]@00000000000");
    if (o->kind == OK_DATE) { sprintf(b, "<%.500s>@00000000000", v2_tostr(vm, v)->s); return a2_str(vm, b); }
    { A2Class *vc = v2_class_of(vm, v);
      sprintf(b, "%.500s@00000000000", vc && vc->ns_private ? vc->name->s : a2_class_name(vm, vc, 1)->s); }   /* a file-private class shows its bare name */
    return a2_str(vm, b);
}
/* class name with '.' between package and name (Multiname::format style used by the verifier) */
A2Str *a2_class_name_dotted(VM2 *vm, A2Class *c) {
    char b[600];
    if (!c) return a2_str(vm, "*");
    if (c->ns.uri->len == 0) return c->name;
    sprintf(b, "%.280s.%.280s", c->ns.uri->s, c->name->s); return a2_str(vm, b);
}
void a2_throw_coerce_error(VM2 *vm, V2 v, A2Class *type) {
    char b[1300];
    sprintf(b, "Type Coercion failed: cannot convert %.600s to %.600s.", a2_error_fmt(vm, v)->s, a2_class_name_dotted(vm, type)->s);
    a2_throw_error(vm, "TypeError", b, 1034);
}
/* element coercion for Vector.<T>; Vector.<*> stores null for undefined */
V2 a2_vec_coerce(VM2 *vm, V2 v, A2Class *type) { if (!type) { V2_NORM(v); return v.t == T_UNDEF ? v2_null() : v; } return a2_coerce(vm, v, type); }
V2 a2_coerce(VM2 *vm, V2 v, A2Class *type) {
    if (!type) return v;
    if (type == vm->c_int) return v2_int(v2_toint(vm, v));
    if (type == vm->c_uint) return v2_uint(v2_touint(vm, v));
    if (type == vm->c_number) return v.t == T_NUM ? v : v2_num(v2_tonum(vm, v));
    if (type == vm->c_boolean) return v2_bool(v2_tobool(vm, v));
    if (type == vm->c_string) { if (v.t == T_UNDEF || v.t == T_NULL) return v2_null(); return v2_str(v2_tostr(vm, v)); }
    if (type == vm->c_object) { if (v.t == T_UNDEF) return v2_null(); return v; }
    if (v.t == T_UNDEF || v.t == T_NULL) return v2_null();
    if (type == vm->c_vector && v.t == T_OBJ && v.u.o->kind == OK_VECTOR && v.u.o->cls != vm->c_vector && (v.u.o->vec_type == vm->c_int || v.u.o->vec_type == vm->c_uint || v.u.o->vec_type == vm->c_number)) { a2_throw_coerce_error(vm, v, type); return v2_null(); }   /* Vector.<int/uint/Number> are not Vector.<*> */
    if (a2_instanceof(vm, v, type)) return v;
    if (v.t == T_OBJ && v.u.o->kind == OK_CLASS && (type == vm->c_class)) return v;
    /* untyped native-made Vector matched against an applied Vector.<T>: adopt the class */
    if (v.t == T_OBJ && v.u.o->kind == OK_VECTOR && v.u.o->cls == vm->c_vector && type != vm->c_vector && a2_is_subclass(type, vm->c_vector) && (!v.u.o->vec_type || v.u.o->vec_type == a2_vec_elem_type(vm, type))) { v.u.o->cls = type; v.u.o->vec_type = a2_vec_elem_type(vm, type); v.u.o->proto = type->prototype; return v; }
    if (type->flags & CF_INTERFACE) {
        /* objects implementing by duck typing (natives) are accepted */
        if (v.t == T_OBJ) return v;
    }
    a2_throw_coerce_error(vm, v, type);
    return v2_null();
}

/* ---------------------------------------------------------------- */
/* calls                                                            */

static V2 interp(VM2 *vm, A2Method *m, A2Obj *scope, V2 self, int argc, V2 *argv);

/* ---------------------------------------------------------------- */
/* string builder: `slot += "x"` on a global slot grows one buffer in place instead of
   interning every intermediate string (3,000,000 appends must not be quadratic).  The built
   string stays out of the intern table while only that slot holds it; any other access to
   the slot's object, any call and any other read publishes (interns) it first. */
A2Str *a2_sb_publish(VM2 *vm) {
    A2Str *r = vm->sb_str, *e; A2Obj *o = vm->sb_obj; int k = vm->sb_slot;
    if (!r) return NULL;
    vm->sb_str = NULL; vm->sb_obj = NULL;
    e = a2_intern_adopt(vm, r);
    if (e != r) {   /* an equal string already exists: the slot takes it, the builder is dropped */
        if (o && k >= 0 && k < o->nslots && o->slots[k].t == T_STR && o->slots[k].u.s == r) o->slots[k] = v2_str(e);
        free(r->s); r->s = (char*)calloc(1, 1); r->len = 0;
    } else { char *t = (char*)realloc(r->s, (size_t)r->len + 1); if (t) r->s = t; }
    return e;
}
#define SB_PUBLISH(vm) do { if ((vm)->sb_str) a2_sb_publish(vm); } while (0)
/* `pushstring s; add` follows */
static int sb_push_add(const u8 *p) { int i; if (*p++ != 0x2c) return 0; for (i = 0; i < 5 && (*p++ & 0x80); i++) ; return *p == 0xa0; }
/* after an add at p: [coerce_s|convert_s] getglobalscope swap setslot k  -> k, else 0 */
static u32 sb_pattern_after(const u8 *p) {
    u32 k = 0; int sh = 0;
    if (*p == 0x85 || *p == 0x70) p++;
    if (p[0] != 0x64 || p[1] != 0x2b || p[2] != 0x6d) return 0;
    p += 3;
    do { k |= (u32)(*p & 0x7f) << sh; sh += 7; } while ((*p++ & 0x80) && sh < 35);
    return k;
}
/* a + b stored right back into slot k of g: grow the builder (or start one); 1 if done */
static int sb_append(VM2 *vm, A2Obj *g, u32 k, A2Str *a, A2Str *b, V2 *out) {
    size_t n = (size_t)a->len + b->len;
    if (!k || (int)k > g->nslots || !b->len) return 0;
    if (a->len >= 3 && b->len >= 3) { const u8 *x = (const u8*)a->s + a->len - 3, *y = (const u8*)b->s; if (x[0] == 0xED && x[1] >= 0xA0 && x[1] <= 0xAF && y[0] == 0xED && y[1] >= 0xB0 && y[1] <= 0xBF) return 0; }   /* surrogate pair joining: a2_concat */
    if (vm->sb_str == a && vm->sb_obj == g && vm->sb_slot == (int)k - 1 && g->slots[k - 1].t == T_STR && g->slots[k - 1].u.s == a) {
        if (n + 1 > vm->sb_cap) { size_t cap = (n + 1) * 2; char *t = (char*)realloc(a->s, cap); if (!t) return 0; a->s = t; vm->sb_cap = cap; }
        memcpy(a->s + a->len, b->s, b->len); a->len = (u32)n; a->s[n] = 0;
        *out = v2_str(a); return 1;
    }
    if (a == vm->sb_str) return 0;
    SB_PUBLISH(vm);
    {   A2Str *r; size_t cap = (n + 1) * 2; char *buf = (char*)malloc(cap);
        if (!buf) return 0;
        r = (A2Str*)a2_gc_alloc_str(vm);
        if (!r) { free(buf); return 0; }
        memcpy(buf, a->s, a->len); memcpy(buf + a->len, b->s, b->len); buf[n] = 0;
        r->s = buf; r->len = (u32)n; r->hash = 0; r->inext = NULL;
        vm->sb_str = r; vm->sb_obj = g; vm->sb_slot = (int)k - 1; vm->sb_cap = cap;
        *out = v2_str(r); return 1;
    }
}

V2 a2_call_method(VM2 *vm, A2Method *m, A2Obj *scope, V2 self, int argc, V2 *argv) {
    V2 r;
    SB_PUBLISH(vm);
    if (!m) return v2_undef();
    if (vm->throwing) return v2_undef();
    if (m->native) {
        vm->callee_obj = NULL;        /* only the interpreter consumes it */
        if (vm->depth > A2_MAX_DEPTH) { a2_throw_error(vm, "Error", "Stack overflow occurred.", 1023); return v2_undef(); }
        if (m->argmax && (argc < m->argmin || (m->argmax != -1 && argc > (m->argmax == -2 ? 0 : m->argmax)))) {
            char b[700]; sprintf(b, "Argument count mismatch on %.500s(). Expected %d, got %d.", a2_method_trace_name(vm, m)->s, argc < m->argmin ? m->argmin : (m->argmax == -2 ? 0 : m->argmax), argc);
            a2_throw_error(vm, "ArgumentError", b, 1063); return v2_undef();
        }
        vm->depth++; vm->cs[vm->depth] = m; vm->cs_cls[vm->depth] = NULL; vm->cs_file[vm->depth] = NULL;
        r = m->native(vm, self, argc, argv);
        vm->depth--;
        return r;
    }
    return interp(vm, m, scope_for_method(vm, m, scope), self, argc, argv);
}

V2 a2_call(VM2 *vm, V2 fn, V2 self, int argc, V2 *argv) {
    A2Obj *f;
    if (fn.t != T_OBJ) { a2_throw_error(vm, "TypeError", "value is not a function.", 1006); return v2_undef(); }
    f = fn.u.o;
    if (f->kind == OK_CLASS) {
        A2Class *c = f->as_class;
        if (c->native_call) {
            V2 r;
            if (vm->depth > A2_MAX_DEPTH) { a2_throw_error(vm, "Error", "Stack overflow occurred.", 1023); return v2_undef(); }
            vm->depth++; vm->cs[vm->depth] = NULL; vm->cs_cls[vm->depth] = c; vm->cs_file[vm->depth] = NULL;
            SB_PUBLISH(vm); r = c->native_call(vm, v2_obj(f), argc, argv);
            vm->depth--;
            return r;
        }
        if (c == vm->c_math) { a2_throw_id(vm, "TypeError", 1075, NULL, NULL); return v2_undef(); }
        if (argc != 1) { char b[24]; sprintf(b, "%d", argc); a2_throw_id(vm, "ArgumentError", 1112, b, NULL); return v2_undef(); }
        return a2_coerce(vm, argv[0], c);
    }
    if (f->kind != OK_FUNCTION) { a2_throw_error(vm, "TypeError", "value is not a function.", 1006); return v2_undef(); }
    if (f->is_bound) self = f->receiver;
    else if (self.t == T_UNDEF || self.t == T_NULL) {
        if (f->scope && f->scope->alen) self = f->scope->arr[0]; else self = v2_obj(vm->global);
    }
    vm->callee_obj = f;
    return a2_call_method(vm, f->method, f->scope, self, argc, argv);
}

void a2_run_ctor(VM2 *vm, A2Class *c, A2Obj *self, int argc, V2 *argv) {
    if (!c) return;
    if (c->iinit && !c->iinit->native) { a2_call_method(vm, c->iinit, c->scope, v2_obj(self), argc, argv); return; }
    if (c->native_ctor) { SB_PUBLISH(vm); c->native_ctor(vm, v2_obj(self), argc, argv); return; }
    if (c->super) a2_run_ctor(vm, c->super, self, argc, argv);
}

V2 a2_construct_class(VM2 *vm, A2Class *c, int argc, V2 *argv) {
    A2Obj *o;
    if (c->flags & CF_INTERFACE) { { char b[300]; sprintf(b, "The method %.250s() is not implemented.", a2_class_name(vm, c, 0)->s); a2_throw_error(vm, "VerifyError", b, 1001); } return v2_undef(); }
    if (c->abc && !c->inited) a2_class_init(vm, c);
    if (c == vm->c_object && argc > 0 && argv[0].t != T_UNDEF && argv[0].t != T_NULL) return argv[0];   /* new Object(v) is v */
    o = a2_new(vm, c);
    if (vm->c_simplebutton && c->symbol_char > 0 && a2_is_subclass(c, vm->c_simplebutton)) { extern void a2_display_ensure_record(VM2 *vm, A2Obj *o); a2_display_ensure_record(vm, o); }   /* a button symbol's states exist before its constructor runs */
    if (vm->depth > A2_MAX_DEPTH) { a2_throw_error(vm, "Error", "Stack overflow occurred.", 1023); return v2_undef(); }
    vm->depth++; vm->cs[vm->depth] = NULL; vm->cs_cls[vm->depth] = (c->iinit && !c->iinit->native) ? NULL : c; vm->cs_file[vm->depth] = NULL;
    a2_run_ctor(vm, c, o, argc, argv);
    vm->depth--;
    if (o->kind == OK_PRIM && o->prim.t != T_UNDEF && (c == vm->c_number || c == vm->c_int || c == vm->c_uint || c == vm->c_string || c == vm->c_boolean)) return o->prim.t == T_UNDEF ? v2_obj(o) : o->prim;
    return v2_obj(o);
}

V2 a2_construct(VM2 *vm, V2 ctor, int argc, V2 *argv) {
    A2Obj *f;
    if (ctor.t != T_OBJ) { a2_throw_error(vm, "TypeError", "Instantiation attempted on a non-constructor.", 1007); return v2_undef(); }
    f = ctor.u.o;
    if (f->kind == OK_CLASS) {
        A2Class *c = f->as_class;
        if (c == vm->c_vector) { a2_throw_error(vm, "TypeError", "Instantiation attempted on a non-constructor.", 1007); return v2_undef(); }   /* unspecialized Vector */
        /* Number("1"), String(x) ... as constructors: natives decide */
        if (c->native_call && (c == vm->c_number || c == vm->c_int || c == vm->c_uint || c == vm->c_string || c == vm->c_boolean || c == vm->c_array || c == vm->c_vector)) {
            V2 r;
            if (vm->depth > A2_MAX_DEPTH) { a2_throw_error(vm, "Error", "Stack overflow occurred.", 1023); return v2_undef(); }
            vm->depth++; vm->cs[vm->depth] = NULL; vm->cs_cls[vm->depth] = c; vm->cs_file[vm->depth] = NULL;
            SB_PUBLISH(vm); r = c->native_call(vm, ctor, argc, argv);
            vm->depth--;
            return r;
        }
        if (c == vm->c_class) { a2_throw_id(vm, "TypeError", 1115, "Class$", NULL); return v2_undef(); }
        if (c == vm->c_math) { a2_throw_id(vm, "TypeError", 1076, NULL, NULL); return v2_undef(); }
        /* script `new` of a class the player instantiates itself (#2012) */
        { A2Class *n = c; while (n && n->abc && !n->abc->is_builtin) n = n->super;   /* nearest library ancestor */
          if (n && (n->flags & CF_NOINST)) { char b[400]; if (c->abc && !c->inited) a2_class_init(vm, c); sprintf(b, (n->flags & CF_NOINST_PLAIN) ? "%.300s class cannot be instantiated." : "%.300s$ class cannot be instantiated.", a2_class_name(vm, c, 0)->s); a2_throw_error(vm, "ArgumentError", b, 2012); return v2_undef(); } }
        /* a ByteArray subclass linked to a symbol that is not binary data (#2136) */
        if (vm->c_bytearray && c != vm->c_bytearray && a2_is_subclass(c, vm->c_bytearray)) {
            A2Class *k; for (k = c; k && k != vm->c_bytearray; k = k->super) if (k->symbol_char > 0 && k->symbol_movie) {
                SWFChar *ch = swf_get_char(k->symbol_movie, k->symbol_char);
                if (ch && ch->type != CHAR_BINARY) { char b[1200]; { const char *u = k->symbol_movie->url ? k->symbol_movie->url : ""; const char *dq = strstr(u, "/[[DYNAMIC]]/"); sprintf(b, "The SWF file %.*s contains invalid data.", (int)(dq ? (size_t)(dq - u) : strlen(u) > 1000 ? 1000 : strlen(u)), u); } a2_throw_error(vm, "Error", b, 2136); return v2_undef(); }
                break;
            } }
        return a2_construct_class(vm, c, argc, argv);
    }
    if (f->kind == OK_FUNCTION && f->is_bound) {   /* method closures are not constructors */
        char b[300]; sprintf(b, "Cannot call method %.250s as constructor.", f->method && f->method->name ? f->method->name->s : ""); a2_throw_error(vm, "TypeError", b, 1064); return v2_undef();
    }
    if (f->kind == OK_FUNCTION) {
        A2Obj *o = a2_new_plain(vm); V2 proto, r;
        o->hash_enum = f->method && f->method->abc && !f->method->abc->is_builtin;
        proto = a2_get_s(vm, ctor, "prototype");
        if (proto.t != T_OBJ) {   /* no object prototype: `new` gives the function a fresh one */
            A2Obj *pr = a2_new_plain(vm); a2_dyn_set_hidden(vm, pr, vm->s_constructor, ctor); a2_dyn_set(vm, f, vm->s_prototype, v2_obj(pr)); proto = v2_obj(pr);
        }
        o->proto = proto.u.o;
        r = a2_call_method(vm, f->method, f->scope, v2_obj(o), argc, argv);
        if (r.t == T_OBJ) return r;
        return v2_obj(o);
    }
    a2_throw_error(vm, "TypeError", "Instantiation attempted on a non-constructor.", 1007);
    return v2_undef();
}

/* ---------------------------------------------------------------- */
/* class construction from ABC                                      */

static A2Class *global_class_for(AbcRT *rt, int script);

/* natives of the built-in ActionScript library */
typedef struct NatEnt { const char *key; NativeFn2 fn; const void *data; struct NatEnt *next; } NatEnt;
static NatEnt *g_natives;
void a2_register_native_data(const char *key, NativeFn2 fn, const void *data) { NatEnt *e = (NatEnt*)malloc(sizeof(NatEnt)); if (!e) return; e->key = key; e->fn = fn; e->data = data; e->next = g_natives; g_natives = e; }
void a2_register_native(const char *key, NativeFn2 fn) { a2_register_native_data(key, fn, NULL); }
static V2 native_missing(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; (void)argc; (void)argv; if (vm->player && vm->player->debug) fprintf(stderr, "[avm2] native not implemented\n"); return v2_undef(); }
static void bind_native(AbcRT *rt, A2Method *m, A2Class *owner, A2Str *name, const A2Ns *ns, int kind, int is_static) {
    char key[300]; NatEnt *e; AbcMethod *am;
    if (!m || !rt->abc->is_builtin || m->abc_native) return;
    am = &rt->abc->methods[m->index];
    if (!(am->flags & MF_NATIVE)) return;
    if (owner)
        sprintf(key, "%.120s::%.80s%s/%s%.80s", owner->ns.uri ? owner->ns.uri->s : "", owner->name->s, is_static ? "$" : "", kind == TK_GETTER ? "get " : kind == TK_SETTER ? "set " : "", name->s);
    else {
        char tn[300];
        sprintf(key, "%.120s::%.80s", ns && ns->uri ? ns->uri->s : "", name->s);
        if (ns && ns->uri && ns->uri->len) sprintf(tn, "global/%.120s::%.80s", ns->uri->s, name->s); else sprintf(tn, "global/%.80s", name->s);
        m->dbg_name = a2_str(rt->vm, tn);   /* package-level functions trace as global/pkg::name */
    }
    for (e = g_natives; e; e = e->next) if (!strcmp(e->key, key)) { m->abc_native = e->fn; m->native_data = e->data; return; }
    if (rt->vm->player && rt->vm->player->debug) fprintf(stderr, "[avm2] no native for %s\n", key);
    m->abc_native = native_missing;
}
static int air_only_api(AbcFile *a, AbcTrait *tr);
static void build_traits(AbcRT *rt, A2TraitMap *tm, AbcTrait *traits, u32 n, A2Class *owner, int *nslots, int is_static) {
    u32 i; VM2 *vm = rt->vm;
    for (i = 0; i < n; i++) {
        AbcTrait *at = &traits[i]; RTName *rn = &rt->names[at->name < rt->abc->mn_count ? at->name : 0];
        A2Trait *t;
        if (owner && rt->abc->is_builtin && !vm->air && at->kind != TRAIT_CLASS && air_only_api(rt->abc, at)) continue;   /* AIR's API */
        t = (A2Trait*)calloc(1, sizeof(A2Trait));
        t->name = rn->name; t->ns = rn->nss ? rn->nss[0] : vm->ns_public;
        t->final_ = !!(at->attrs & TATTR_FINAL);
        t->abc = rt->abc; t->at = at;
        switch (at->kind) {
        case TRAIT_SLOT: case TRAIT_CONST:
            t->kind = at->kind == TRAIT_SLOT ? TK_SLOT : TK_CONST;
            t->slot = at->slot_id ? (int)at->slot_id - 1 : (*nslots);
            if (t->slot >= *nslots) *nslots = t->slot + 1;
            t->type_mn = at->type_name; t->type = rt_type(rt, at->type_name);
            if (at->vkind) { t->def = const_value(rt, at->vkind, at->vindex); t->has_def = 1; }
            break;
        case TRAIT_CLASS:
            t->kind = TK_CLASS;
            t->slot = at->slot_id ? (int)at->slot_id - 1 : (*nslots);
            if (t->slot >= *nslots) *nslots = t->slot + 1;
            break;
        case TRAIT_METHOD: case TRAIT_FUNCTION:
            t->kind = TK_METHOD; t->method = rt_method(rt, at->method); if (t->method && at->kind == TRAIT_METHOD) t->method->trait_bound = 1; if (t->method && owner) { t->method->owner = owner; t->method->is_static = is_static; } bind_native(rt, t->method, owner, t->name, &t->ns, TK_METHOD, is_static);
            if (at->kind == TRAIT_FUNCTION) { /* function trait: behaves as a slot holding a closure */
                t->kind = TK_SLOT; t->slot = at->slot_id ? (int)at->slot_id - 1 : (*nslots); if (t->slot >= *nslots) *nslots = t->slot + 1;
                t->has_def = 0; t->method = rt_method(rt, at->method);   /* filled at object creation */
                t->type = NULL; t->kind = TK_SLOT; t->final_ = 2;         /* 2 marks function trait */
            }
            break;
        case TRAIT_GETTER: t->kind = TK_GETTER; t->getter = rt_method(rt, at->method); if (t->getter) t->getter->trait_bound = 1; if (t->getter && owner) { t->getter->owner = owner; t->getter->is_static = is_static; } bind_native(rt, t->getter, owner, t->name, &t->ns, TK_GETTER, is_static); break;
        case TRAIT_SETTER: t->kind = TK_SETTER; t->setter = rt_method(rt, at->method); if (t->setter) t->setter->trait_bound = 1; if (t->setter && owner) { t->setter->owner = owner; t->setter->is_static = is_static; } t->at2 = at; t->abc2 = rt->abc; t->at = NULL; bind_native(rt, t->setter, owner, t->name, &t->ns, TK_SETTER, is_static); break;
        default: free(t); continue;
        }
        (void)is_static;
        /* tm_put semantics: merge getter/setter, override */
        {
            extern void a2_tm_put(A2TraitMap *tm, A2Trait *t);
            a2_tm_put(tm, t);
            /* an override in this class's protected namespace also replaces the
             * methods inherited under the ancestors' protected namespaces */
            if (!is_static && owner && (at->attrs & TATTR_OVERRIDE) && t->ns.kind == NS_PROTECTED && (t->kind == TK_METHOD || t->kind == TK_GETTER || t->kind == TK_SETTER)) {
                A2Class *sc;
                for (sc = owner->super; sc; sc = sc->super) if (sc->protected_ns.uri && sc->protected_ns.uri != t->ns.uri) {
                    A2Trait *e, *cp; int found = 0;
                    if (tm->nb) for (e = tm->b[t->name->hash % (u32)tm->nb]; e; e = e->next) if (e->name == t->name && e->ns.kind == NS_PROTECTED && e->ns.uri == sc->protected_ns.uri) { found = 1; break; }
                    if (!found) continue;
                    cp = (A2Trait*)malloc(sizeof(A2Trait)); if (!cp) continue;
                    *cp = *t; cp->next = NULL; cp->onext = NULL; cp->ns = sc->protected_ns;
                    a2_tm_put(tm, cp);
                }
            }
        }
    }
}

static void init_function_traits(VM2 *vm, A2Class *c, A2Obj *o, A2Obj *scope) {
    int i; A2Trait *t;
    for (i = 0; i < c->itraits.nb; i++) for (t = c->itraits.b[i]; t; t = t->next)
        if (t->kind == TK_SLOT && t->final_ == 2 && t->method && t->slot < o->nslots) o->slots[t->slot] = v2_obj(a2_new_fn(vm, t->method, scope));
}

static int air_only_api(AbcFile *a, AbcTrait *tr);
extern size_t g_a2_ext_bytes;
static A2Trait *a2_find_trait_exact(A2TraitMap *tm, A2Str *name, const A2Ns *ns) {
    A2Trait *t; if (!tm->nb) return NULL;
    for (t = tm->b[name->hash % (u32)tm->nb]; t; t = t->next) if (t->name == name && t->ns.kind == ns->kind && t->ns.uri == ns->uri) return t;
    return NULL;
}
static A2Class *build_class(AbcRT *rt, u32 ci, A2Class *super, A2Obj **scope_objs, int nscope) {
    VM2 *vm = rt->vm; AbcFile *a = rt->abc;
    AbcInstance *in = &a->instances[ci]; AbcClass *ac = &a->classes[ci];
    RTName *rn = &rt->names[in->name < a->mn_count ? in->name : 0];
    A2Class *c; int kind = super ? super->kind : OK_PLAIN; u32 i;
    if (rt->classes[ci] && !rt->fresh) return rt->classes[ci];
    c = a2_class_alloc(vm, rn->nss ? rn->nss[0].uri : vm->s_empty, rn->name, super, kind, 1);
    c->ns = rn->nss ? rn->nss[0] : vm->ns_public;
    if (c->ns.kind == NS_PRIVATE) { c->ns.kind = NS_PACKAGE; c->ns_private = 1; }
    c->abc = a; c->inst = in; c->cls = ac; c->cindex = ci; c->rt = rt;
    c->flags = in->flags;
    if (a->is_builtin) {   /* [Ruffle(Abstract)]: the player's library class cannot be instantiated (#2012) */
        u32 si, ti, mi, k;
        for (si = 0; si < a->script_count; si++) for (ti = 0; ti < a->scripts[si].trait_count; ti++) {
            AbcTrait *tr = &a->scripts[si].traits[ti];
            if ((tr->kind & 15) != TRAIT_CLASS || tr->classi != ci) continue;
            if (air_only_api(a, tr)) c->api_air = 1;
            for (mi = 0; mi < tr->metadata_count; mi++) {
                AbcMetadata *md; if (tr->metadata[mi] >= a->metadata_count) continue;
                md = &a->metadata[tr->metadata[mi]];
                if (md->name < a->string_count && !strcmp(a->strings[md->name], "Ruffle"))
                    for (k = 0; k < md->item_count; k++) if (md->vals[k] < a->string_count && !strcmp(a->strings[md->vals[k]], "Abstract")) c->flags |= CF_NOINST;
            }
        }
    }
    c->is_dynamic = !(in->flags & CF_SEALED);
    if (in->flags & CF_PROTECTED_NS) c->protected_ns = rt->nss[in->protected_ns < a->ns_count ? in->protected_ns : 0];
    if (!rt->classes[ci]) rt->classes[ci] = c;
    build_traits(rt, &c->itraits, in->traits, in->trait_count, c, &c->nislots, 0);
    if (c->protected_ns.uri && super) {   /* inherited protected members are reachable through this class's protected namespace too */
        extern void a2_tm_put(A2TraitMap *tm, A2Trait *t);
        A2Trait *t, *add = NULL; int b;
        for (b = 0; b < c->itraits.nb; b++) for (t = c->itraits.b[b]; t; t = t->next) {
            A2Class *sc; int anc = 0;
            if (t->ns.kind != NS_PROTECTED || t->ns.uri == c->protected_ns.uri) continue;
            for (sc = super; sc; sc = sc->super) if (sc->protected_ns.uri == t->ns.uri) { anc = 1; break; }
            if (anc && !a2_find_trait_exact(&c->itraits, t->name, &c->protected_ns)) { A2Trait *cp = (A2Trait*)malloc(sizeof(A2Trait)); if (cp) { *cp = *t; cp->ns = c->protected_ns; cp->onext = add; add = cp; } }
        }
        while (add) { A2Trait *nx = add->onext; add->next = NULL; add->onext = NULL; a2_tm_put(&c->itraits, add); add = nx; }
    }
    if (super) { u32 q; c->shadow_slots = super->shadow_slots > 0; for (q = 0; q < in->trait_count && !c->shadow_slots; q++) {
        AbcTrait *at = &in->traits[q]; RTName *tn = &rt->names[at->name < a->mn_count ? at->name : 0]; A2Trait *st;
        if ((at->kind & 15) != TRAIT_SLOT && (at->kind & 15) != TRAIT_CONST) continue;
        st = tn->name ? a2_find_trait_any(&super->itraits, tn->name) : NULL;
        if (st && (st->kind == TK_SLOT || st->kind == TK_CONST)) c->shadow_slots = 1; } }
    c->ncslots = 0;
    build_traits(rt, &c->ctraits, ac->traits, ac->trait_count, c, &c->ncslots, 1);
    c->iinit = rt_method(rt, in->iinit); if (c->iinit) c->iinit->owner = c;
    c->cinit = rt_method(rt, ac->cinit); if (c->cinit) { c->cinit->owner = c; c->cinit->is_static = 1; }
    for (i = 0; i < in->interface_count; i++) {
        A2Class *ic = rt_type(rt, in->interfaces[i]);
        if (ic) { c->ifaces = (A2Class**)realloc(c->ifaces, (size_t)(c->nifaces + 1) * sizeof(A2Class*)); c->ifaces[c->nifaces++] = ic; }
    }
    /* static slots live on the class object */
    if (c->ncslots) { int k; c->obj->slots = (V2*)calloc((size_t)c->ncslots, sizeof(V2)); c->obj->nslots = c->ncslots; for (k = 0; k < c->ncslots; k++) c->obj->slots[k] = v2_undef();
        { A2Trait *t; int b; for (b = 0; b < c->ctraits.nb; b++) for (t = c->ctraits.b[b]; t; t = t->next) if ((t->kind == TK_SLOT || t->kind == TK_CONST) && t->slot < c->obj->nslots) {
            if (t->has_def) c->obj->slots[t->slot] = t->def;
            else if (t->type == vm->c_int || t->type == vm->c_uint) c->obj->slots[t->slot] = v2_int(0);
            else if (t->type == vm->c_number) c->obj->slots[t->slot] = v2_num(0.0 / 0.0);
            else if (t->type == vm->c_boolean) c->obj->slots[t->slot] = v2_bool(0);
            else if (t->type) c->obj->slots[t->slot] = v2_null();
            if (t->kind == TK_SLOT && t->final_ == 2 && t->method) c->obj->slots[t->slot] = v2_obj(a2_new_fn(vm, t->method, NULL));
        } }
    }
    /* scope: captured chain + the class object itself */
    {
        A2Obj **objs = (A2Obj**)malloc((size_t)(nscope + 1) * sizeof(A2Obj*)); int k;
        for (k = 0; k < nscope; k++) objs[k] = scope_objs[k];
        objs[nscope] = c->obj;
        c->scope = a2_new_scope(vm, objs, nscope + 1);
        c->cscope = a2_new_scope(vm, objs, nscope);   /* static methods and the class initialiser: without the class object (avmplus class scope) */
        free(objs);
    }
    /* static function traits need the scope */
    { A2Trait *t; int b; for (b = 0; b < c->ctraits.nb; b++) for (t = c->ctraits.b[b]; t; t = t->next) if (t->kind == TK_SLOT && t->final_ == 2 && t->slot < c->obj->nslots && c->obj->slots[t->slot].t == T_OBJ) c->obj->slots[t->slot].u.o->scope = c->scope; }
    /* prototype.constructor */
    a2_dyn_set_hidden(vm, c->prototype, vm->s_constructor, v2_obj(c->obj));
    return c;
}

void a2_class_init(VM2 *vm, A2Class *c) {
    if (c->inited || c->initing) return;
    c->initing = 1;
    if (c->super && !c->super->inited) a2_class_init(vm, c->super);
    if (c->cinit) a2_call_method(vm, c->cinit, c->scope, v2_obj(c->obj), 0, NULL);
    c->initing = 0; c->inited = 1;
}

/* ---------------------------------------------------------------- */
/* scripts / globals                                                */

static void vm_add_global(VM2 *vm, A2Obj *g) {
    if (vm->nglobals == vm->capglobals) { int nc = vm->capglobals ? vm->capglobals * 2 : 16; A2Obj **ng = (A2Obj**)realloc(vm->globals, (size_t)nc * sizeof(A2Obj*)); if (!ng) return; vm->globals = ng; vm->capglobals = nc; }
    vm->globals[vm->nglobals++] = g;
}

static A2Class *global_class_for(AbcRT *rt, int script) {
    VM2 *vm = rt->vm; A2Class *c; AbcScript *s = &rt->abc->scripts[script]; A2Obj *g;
    if (rt->gcls[script]) return rt->gcls[script];
    c = a2_class_alloc(vm, vm->s_empty, a2_str(vm, "global"), NULL, OK_GLOBAL, 0);
    c->script = script; c->rt = rt; c->abc = rt->abc; c->is_dynamic = 1;
    build_traits(rt, &c->itraits, s->traits, s->trait_count, NULL, &c->nislots, 0);
    rt->gcls[script] = c;
    g = a2_new(vm, c); g->kind = OK_GLOBAL;
    rt->globals[script] = g;
    vm_add_global(vm, g);
    return c;
}

static void run_script(AbcRT *rt, int script) {
    VM2 *vm = rt->vm; A2Method *init; A2Obj *g; A2Obj *scope;
    if (rt->gstate[script]) return;
    global_class_for(rt, script);
    g = rt->globals[script];
    rt->gstate[script] = 1;
    init = rt_method(rt, rt->abc->scripts[script].init); if (init) init->is_script_init = 1;
    init_function_traits(vm, rt->gcls[script], g, NULL);
    scope = a2_new_scope(vm, &g, 1);
    { int b; A2Trait *t; for (b = 0; b < rt->gcls[script]->itraits.nb; b++) for (t = rt->gcls[script]->itraits.b[b]; t; t = t->next) if (t->kind == TK_SLOT && t->final_ == 2 && t->slot < g->nslots && g->slots[t->slot].t == T_OBJ) g->slots[t->slot].u.o->scope = scope; }
    /* the script initialiser starts with an empty outer scope (it pushes the global itself) */
    if (init) a2_call_method(vm, init, a2_new_scope(vm, NULL, 0), v2_obj(g), 0, NULL);
    rt->gstate[script] = 2;
}

/* find the script global that defines `name` (runs its init lazily) */
/* [API("N")] on a library definition: N - 660 indexes Ruffle's ApiVersion;
   the AIR-only versions are hidden when the runtime is Flash Player */
/* [API("NNN")] versioning of the player's library: NNN - 660 is an avmplus ApiVersion; the trait is
   visible to Flash Player content when its Flash Player release is not newer than the root SWF's */
static int api_swf_limit = 70;
static int api_fp_of(int v) {
    if (v <= 0) return 0;
    if (v == 2 || v == 5 || v == 7 || v == 10 || v == 12) return v;
    if (v >= 14 && v < 70 && !(v & 1)) return v;
    return 70;   /* AIR only */
}
static int air_only_api(AbcFile *a, AbcTrait *tr) {
    u32 mi, k; int need = 0;
    for (mi = 0; mi < tr->metadata_count; mi++) {
        AbcMetadata *md; if (tr->metadata[mi] >= a->metadata_count) continue;
        md = &a->metadata[tr->metadata[mi]];
        if (md->name >= a->string_count || strcmp(a->strings[md->name], "API")) continue;
        for (k = 0; k < md->item_count; k++) if (md->vals[k] < a->string_count) {
            int f = api_fp_of(atoi(a->strings[md->vals[k]]) - 660);
            if (!need || f < need) need = f;
        }
    }
    return need > api_swf_limit;
}
void a2_api_set_swf(int ver) {
    api_swf_limit = ver <= 9 ? 0 : ver == 10 ? 7 : ver == 11 ? 10 : ver >= 12 ? 12 + 2 * (ver - 12) : 70;
    if (api_swf_limit > 69) api_swf_limit = 69;
}
static A2Obj *find_global_def_dom(VM2 *vm, const A2Name *n, int dom);
/* a definition in exactly the name's namespace wins over a public one reached through the
   interface-namespace fallback (a user namespace `Token` must not find the public class Namespace) */
extern int a2_trait_exact;
static A2Trait *gd_trait(A2TraitMap *tm, A2Str *name, const A2Name *n, int exact) { A2Trait *t; int was = a2_trait_exact; a2_trait_exact = exact; t = a2_find_trait(tm, name, n); a2_trait_exact = was; return t; }
static A2Obj *find_global_def(VM2 *vm, const A2Name *n) { return find_global_def_dom(vm, n, a2_cur_domain(vm)); }
extern int a2_domain_visible(VM2 *vm, int d, int cur);
static A2Obj *find_global_def_dom(VM2 *vm, const A2Name *n, int dom) {
    int i, k, chain[32], nch = 0, exact;
    { int d = dom; while (d >= 0 && nch < 32) { chain[nch++] = d; d = a2_domain_parent(vm, d); } }
    for (exact = 1; exact >= 0; exact--) {
        /* builtin global first (JSON is versioned API: SWF 13 / Flash Player 11 and later) */
        if (vm->global && vm->global->cls && gd_trait(&vm->global->cls->itraits, n->name, n, exact) && !(vm->swf_version && vm->swf_version < 13 && (!strcmp(n->name->s, "JSON") || !strcmp(n->name->s, "generateRandomBytes")))) return vm->global;
        for (k = 0; k < 2 * vm->nabcs; k++) {   /* the player's library first, then in load order, within the domain chain */
            AbcRT *rt = RT(vm->abcs[k % vm->nabcs]); int d, vis = 0;
            if (!rt || (k < vm->nabcs) != !!rt->abc->is_builtin) continue;
            d = a2_abc_domain(rt->abc); for (i = 0; i < nch; i++) if (chain[i] == d) { vis = 1; break; }
            if (!vis) continue;
            for (i = 0; i < (int)rt->abc->script_count; i++) {   /* first definition wins (duplicate_defs) */
                A2Class *gc = global_class_for(rt, i); A2Trait *t = gd_trait(&gc->itraits, n->name, n, exact);
                if (t && rt->abc->is_builtin && !vm->air && t->at && air_only_api(rt->abc, t->at)) continue;   /* AIR's API: hidden in Flash Player */
                if (t) { if (!rt->gstate[i]) run_script(rt, i); return rt->globals[i]; }
            }
        }
    }
    /* dynamic props on the builtin global */
    if (vm->global && a2_dyn_find(vm->global, n->name)) return vm->global;
    for (i = 0; i < vm->nglobals; i++) if (a2_dyn_find(vm->globals[i], n->name)) return vm->globals[i];
    /* the global object inherits from Object.prototype */
    if (vm->c_object && vm->global) { A2Obj *p; int g = 0; for (p = vm->c_object->prototype; p && g < 32; p = p->proto, g++) if (a2_dyn_find(p, n->name)) return vm->global; }
    return NULL;
}

/* public definitions of every loaded ABC, in load order (ApplicationDomain.getQualifiedDefinitionNames) */
void a2_push_definition_names(VM2 *vm, A2Obj *vec) {
    int k, i, b; A2Trait *t;
    for (k = 0; k < vm->nabcs; k++) {
        AbcRT *rt = RT(vm->abcs[k]);
        if (!rt || vm->abcs[k]->is_builtin || a2_abc_domain(vm->abcs[k]) != a2_cur_domain(vm)) continue;   /* this domain's own definitions */
        for (i = 0; i < (int)rt->abc->script_count; i++) {
            A2Class *gc = global_class_for(rt, i);
            for (b = 0; b < gc->itraits.nb; b++) for (t = gc->itraits.b[b]; t; t = t->next) {
                char buf[600];
                if (t->ns.kind != NS_PACKAGE && t->ns.kind != NS_PACKAGE_INTERNAL) continue;
                if (t->ns.uri && t->ns.uri->len) sprintf(buf, "%.290s::%.290s", t->ns.uri->s, t->name->s); else sprintf(buf, "%.290s", t->name->s);
                a2_arr_push(vm, vec, v2_cstr(vm, buf));
            }
        }
    }
}

/* verifier: the class a plain public-package QName names, if it is already defined (no side effects) */
A2Class *a2_verify_qname_class(VM2 *vm, AbcFile *abc, u32 mn) {
    AbcRT *rt = abc ? RT(abc) : NULL; RTName *n; AbcMultiname *am; A2Class *c;
    if (!rt || !rt->names || mn == 0 || mn >= abc->mn_count) return NULL;
    am = &abc->multinames[mn];
    if (am->kind != MN_QNAME || am->ns == 0 || am->ns >= abc->ns_count || am->name == 0) return NULL;
    if (abc->namespaces[am->ns].kind != NS_PACKAGE) return NULL;
    n = &rt->names[mn];
    if (!n->name || !n->nss || n->nns != 1) return NULL;
    c = a2_class_find(vm, n->nss[0].uri, n->name);
    if (c && c->api_air && !vm->air) return NULL;
    return c;
}

/* class lookup that runs the defining script lazily when needed */
A2Class *a2_class_lookup(VM2 *vm, A2Str *ns, A2Str *name) {
    A2Class *c = a2_class_find(vm, ns, name); A2Name n; A2Ns pns;
    if (c && c->api_air && !vm->air) return NULL;
    if (c) return c;
    pns.kind = NS_PACKAGE; pns.uri = ns ? ns : vm->s_empty;
    n.name = name; n.nss = &pns; n.nns = 1; n.any_ns = 0; n.attr = 0;
    if (find_global_def(vm, &n)) c = a2_class_find(vm, ns, name);
    if (c && c->api_air && !vm->air) return NULL;
    return c;
}
int a2_find_property(VM2 *vm, A2Obj **scopes, int n, const A2Name *name, V2 *out) {
    int i;
    for (i = n - 1; i >= 0; i--) {
        A2Obj *s = scopes[i];
        if (s->kind == OK_GLOBAL) { if (s->cls && a2_find_trait(&s->cls->itraits, name->name, name)) { *out = v2_obj(s); return 1; } continue; }
        /* only `with` scopes are searched dynamically; ordinary scopes bind through their traits */
        if (scopes + i >= vm->scope_stack && scopes + i < vm->scope_stack + A2_SCOPES && !vm->scope_with[scopes + i - vm->scope_stack]) {
            /* the verifier binds through the static type of the scope value (a coerce before pushscope) */
            A2Class *dc = vm->scope_decl[scopes + i - vm->scope_stack];
            if (dc && !dc->is_dynamic && s->kind != OK_CLASS) { if (a2_find_trait(&dc->itraits, name->name, name)) { *out = v2_obj(s); return 1; } continue; }
            if (s->cls && a2_find_trait(&s->cls->itraits, name->name, name)) { *out = v2_obj(s); return 1; }
            if (s->kind == OK_CLASS) { A2Class *c; for (c = s->as_class; c; c = A2_STATIC_SUPER(c)) if (a2_find_trait(&c->ctraits, name->name, name)) { *out = v2_obj(s); return 1; }
                if (name->name == vm->s_prototype && name_has_public(vm, name)) { *out = v2_obj(s); return 1; } }
            continue;
        }
        if (a2_has(vm, v2_obj(s), name)) { *out = v2_obj(s); return 1; }
    }
    { A2Obj *g = find_global_def(vm, name); if (g) { *out = v2_obj(g); return 1; } }
    return 0;
}

/* ---------------------------------------------------------------- */
/* loading                                                          */

int g_a2_loading_builtin = 0;   /* the next ABC is the player's library (its natives bind while it loads) */
int avm2_load_abc(VM2 *vm, const u8 *data, size_t len, int lazy) {
    AbcFile *a; AbcRT *rt; u32 i;
    abc_error = 0;
    a = abc_parse(data, len);
    if (a) { a->movie = PMOV(vm->player); if (g_a2_loading_builtin) a->is_builtin = 1; }
    if (!a) {
        if (vm->player->debug) { extern int abc_fail_line; fprintf(stderr, "[avm2] abc_parse failed (abc.c:%d)\n", abc_fail_line); }
        if (abc_error == 1027) { char b[160]; sprintf(b, "Method_info %lu exceeds method_count=%lu.", (unsigned long)abc_error_arg, (unsigned long)abc_error_arg2); a2_throw_error(vm, "VerifyError", b, 1027); a2_report_uncaught(vm); }
        return 0;
    }
    if (a->dup_body) {   /* #1121: name the method as Class/name() */
        u32 mi = (u32)a->dup_body - 1, c, t; char b[400]; const char *cn = NULL, *tn = NULL;
        for (c = 0; c < a->class_count && !tn; c++) for (t = 0; t < a->instances[c].trait_count; t++) {
            AbcTrait *tr = &a->instances[c].traits[t];
            if ((tr->kind == 1 || tr->kind == 2 || tr->kind == 3) && tr->method == mi) {
                AbcMultiname *cm = &a->multinames[a->instances[c].name], *tm = &a->multinames[tr->name];
                cn = a->strings[cm->name]; tn = a->strings[tm->name]; break;
            }
        }
        if (tn) sprintf(b, "Method %.180s/%.180s() has a duplicate method body.", cn, tn); else sprintf(b, "Method MethodInfo-%lu() has a duplicate method body.", (unsigned long)mi);
        abc_free(a);
        a2_throw_error(vm, "VerifyError", b, 1121); a2_report_uncaught(vm);
        return 0;
    }
    rt = (AbcRT*)calloc(1, sizeof(AbcRT));
    rt->abc = a; rt->vm = vm; a->rt = rt;
    rt->strs = (A2Str**)calloc(a->string_count + 1, sizeof(A2Str*));
    rt->methods = (A2Method**)calloc(a->method_count + 1, sizeof(A2Method*));
    rt->classes = (A2Class**)calloc(a->class_count + 1, sizeof(A2Class*));
    rt->bodycls = (A2Class**)calloc(a->body_count + 1, sizeof(A2Class*));
    rt->catchcls = (A2Class**)calloc(a->body_count + 1, sizeof(A2Class*));
    rt->globals = (A2Obj**)calloc(a->script_count + 1, sizeof(A2Obj*));
    rt->gcls = (A2Class**)calloc(a->script_count + 1, sizeof(A2Class*));
    rt->gstate = (int*)calloc(a->script_count + 1, sizeof(int));
    rt_build_names(rt);
    vm->abcs = (AbcFile**)realloc(vm->abcs, (size_t)(vm->nabcs + 1) * sizeof(AbcFile*));
    vm->abcs[vm->nabcs++] = a;
    for (i = 0; i < a->script_count; i++) global_class_for(rt, (int)i);
    /* the last script runs eagerly (others lazily on first reference) */
    /* DoABC with kDoAbcLazyInitializeFlag only registers the definitions: scripts
       run when something (SymbolClass, code) first references them.  Otherwise
       the entry point (last script) runs now. */
    /* the entry point runs once the frame's tags are processed (after its SymbolClass bindings) */
    if (a->script_count && !lazy) {
        vm->defer_rt = (void**)realloc(vm->defer_rt, (size_t)(vm->ndefer + 1) * sizeof(void*));
        vm->defer_rt[vm->ndefer++] = rt;
    }
    { extern void avm2_retry_symbols(VM2 *vm); avm2_retry_symbols(vm); }
    return 1;
}
void avm2_run_deferred_scripts(Player *p) {
    VM2 *vm = (VM2*)p->avm2; int i, n;
    if (!vm || !vm->ndefer) return;
    n = vm->ndefer; vm->ndefer = 0;
    for (i = 0; i < n; i++) {
        AbcRT *rt = (AbcRT*)vm->defer_rt[i];
        run_script(rt, (int)rt->abc->script_count - 1);
        if (vm->throwing) { extern void a2_report_uncaught(VM2 *vm); a2_report_uncaught(vm); }
    }
    if (vm->ndefer) avm2_run_deferred_scripts(p);
}

void a2_report_uncaught(VM2 *vm) {
    V2 e = vm->exc; A2Str *s; extern void avm2_trace_out(const char *s);
    vm->nuncaught++;
    vm->throwing = 0;
    if (e.t == T_OBJ && e.u.o->kind == OK_ERROR) {
        V2 msg = a2_get_s(vm, e, "message"); V2 name = a2_get_s(vm, e, "name"), st, fn;
        s = v2_tostr(vm, msg);
        fprintf(stderr, "[avm2] uncaught %s: %s\n", v2_tostr(vm, name)->s, s->s);
        /* the debug player writes uncaught errors (with their stack trace) to the trace log */
        fn = a2_get_s(vm, e, "getStackTrace"); st = v2_null();
        if (!vm->throwing && fn.t == T_OBJ) st = a2_call(vm, fn, e, 0, NULL);
        vm->throwing = 0;
        if (st.t != T_STR) st = v2_str(v2_tostr(vm, e));
        vm->throwing = 0;
        if (!vm->quiet_uncaught) avm2_trace_out(v2_tostr(vm, st)->s);
    } else {
        /* any other value: its string (when toString does not throw) and the stack where it was thrown */
        s = v2_tostr(vm, e);
        if (!vm->throwing) {
            char *b = (char*)malloc(s->len + (g_nonerr_stack ? strlen(g_nonerr_stack) : 0) + 1);
            fprintf(stderr, "[avm2] uncaught exception: %s\n", s->s);
            if (b) { memcpy(b, s->s, s->len); strcpy(b + s->len, g_nonerr_stack ? g_nonerr_stack : ""); avm2_trace_out(b); free(b); }
        }
        vm->throwing = 0;
    }
    /* the error is stringified a second time (for the error dialog) */
    if (e.t == T_OBJ) (void)v2_tostr(vm, e);
    vm->throwing = 0;
}

/* ---------------------------------------------------------------- */
/* interpreter                                                      */

static A2Class *activation_class(AbcRT *rt, AbcBody *body) {
    u32 bi = (u32)(body - rt->abc->bodies); A2Class *c;
    if (rt->bodycls[bi]) return rt->bodycls[bi];
    c = a2_class_alloc(rt->vm, rt->vm->s_empty, a2_str(rt->vm, "activation"), NULL, OK_ACTIVATION, 0);
    c->is_dynamic = 0; c->rt = rt;   /* activations are sealed */
    build_traits(rt, &c->itraits, body->traits, body->trait_count, NULL, &c->nislots, 0);
    rt->bodycls[bi] = c;
    return c;
}

static A2Class *catch_class(AbcRT *rt, AbcBody *body, u32 ei) {
    u32 bi = (u32)(body - rt->abc->bodies); A2Class *c; A2Trait *t; AbcException *e = &body->exceptions[ei];
    (void)bi;
    /* the catch scope object is a sealed one-slot object named after the catch variable,
       with the slot typed as the caught type */
    c = a2_class_alloc(rt->vm, rt->vm->s_empty, e->var_name && e->var_name < rt->abc->mn_count && rt->names[e->var_name].name ? rt->names[e->var_name].name : a2_str(rt->vm, "catch"), NULL, OK_CATCH, 0);
    c->is_dynamic = 0; c->nislots = 1;
    if (e->var_name) {
        RTName *rn = &rt->names[e->var_name < rt->abc->mn_count ? e->var_name : 0];
        t = (A2Trait*)calloc(1, sizeof(A2Trait)); t->name = rn->name; t->ns = rn->nss ? rn->nss[0] : rt->vm->ns_public; t->kind = TK_SLOT; t->slot = 0;
        t->type_mn = e->exc_type; t->abc = rt->abc;
        { extern void a2_tm_put(A2TraitMap *tm, A2Trait *t); a2_tm_put(&c->itraits, t); }
    }
    return c;
}

#define PUSH(v) (stk[sp++] = (v))
#define POP() (stk[--sp])
#define TOP() (stk[sp - 1])
#define RD30() abc_read_u30(&p)
#define RDS24() abc_read_s24(&p)


/* pop runtime parts of a multiname */
static void resolve_name(VM2 *vm, RTName *rn, A2Name *n, V2 *stk, int *psp, A2Ns *tmpns) {
    rt_to_name(rn, n);
    if (rn->rt_name) {
        V2 v = stk[--(*psp)];
        if (v.t == T_OBJ && v.u.o->kind == OK_QNAME) {
            A2Str *u = a2_qname_uri(v.u.o);
            n->name = a2_qname_local(vm, v.u.o);
            if (!rn->rt_ns) { if (!u) n->any_ns = 1; else { tmpns->kind = NS_NAMESPACE; tmpns->uri = u; n->nss = tmpns; n->nns = 1; } }
            if (a2_qname_attr(v.u.o)) n->attr = 1;
        }
        else n->name = v2_tostr(vm, v);
    }
    if (rn->rt_ns) {
        V2 v = stk[--(*psp)];
        if (v.t == T_OBJ && v.u.o->kind == OK_NAMESPACE) { *tmpns = v.u.o->nsval; if (tmpns->kind != NS_PRIVATE) tmpns->id = A2_NSID_RUNTIME; }
        else {
            A2Class *vc = v.t == T_OBJ ? v.u.o->cls : NULL;
            a2_throw_id(vm, "VerifyError", 1058, v.t == T_NULL ? "null" : v.t == T_UNDEF ? "void" : vc ? a2_class_name(vm, vc, 0)->s : "Object", "Namespace");
            tmpns->kind = NS_NAMESPACE; tmpns->uri = vm->s_empty;
        }
        n->nss = tmpns; n->nns = 1;
    }
}

/* a runtime-name (MultinameL) whose namespace set lacks the public namespace:
   the JIT in Flash Player does not take the index/dictionary fast paths for
   these and fails the dynamic lookup (1081 on read, 1056 on write); the
   interpreter (used for script initialisers) ignores the namespaces. */
static int rn_is_nsset(const RTName *rn) { return rn->kind == MN_MULTINAME || rn->kind == MN_MULTINAME_A || rn->kind == MN_MULTINAME_L || rn->kind == MN_MULTINAME_LA; }
static int rn_nopub_strict(VM2 *vm, RTName *rn, A2Method *m) {
    int i;
    if (!rn->rt_name || rn->rt_ns || m->is_script_init || (m->owner && m->owner->cinit == m) || !rn->nss) return 0;   /* initialisers run interpreted */
    for (i = 0; i < rn->nns; i++) if ((rn->nss[i].kind == NS_PACKAGE || rn->nss[i].kind == NS_NAMESPACE) && (!rn->nss[i].uri || !rn->nss[i].uri->len)) return 0;
    (void)vm; return 1;
}

static int is_index_val(VM2 *vm, V2 v, u32 *idx) {
    if (v.t == T_INT && v.u.i >= 0) { *idx = (u32)v.u.i; return 1; }
    if (v.t == T_UINT) { *idx = v.u.u; return 1; }
    if (v.t == T_NUM && v.u.n >= 0 && v.u.n == floor(v.u.n) && v.u.n < 4294967295.0) { *idx = (u32)v.u.n; return 1; }
    (void)vm;
    return 0;
}

/* fast path for obj[int] on arrays/vectors */
static int fast_index_get(VM2 *vm, V2 obj, V2 key, V2 *out) {
    u32 idx; A2Obj *o;
    if (obj.t != T_OBJ) return 0;
    o = obj.u.o;
    if (o->kind == OK_ARRAY && a2_arr_sealed(vm, o)) return 0;
    if (o->kind == OK_ARRAY) { if (is_index_val(vm, key, &idx)) { *out = idx < (u32)o->alen ? o->arr[idx] : v2_hole(); if (out->t == T_HOLE) { V2 t; A2Name n; *out = v2_undef(); n.name = v2_tostr(vm, key); n.nss = &vm->ns_public; n.nns = 1; n.any_ns = 1; n.attr = 0; if (a2_get(vm, obj, &n, &t)) *out = t; } return 1; } return 0; }
    if (o->kind == OK_VECTOR) { if (is_index_val(vm, key, &idx)) { if (idx < (u32)o->alen) *out = o->arr[idx]; else { vec_oob(vm, (double)idx, o->alen); *out = v2_undef(); } return 1; } return 0; }
    if (o->kind == OK_DICT) { extern int a2_dict_get(VM2 *vm, A2Obj *o, V2 k, V2 *out); if (key.t == T_OBJ && key.u.o->kind != OK_QNAME) { if (!a2_dict_get(vm, o, key, out)) *out = v2_undef(); return 1; } return 0; }
    if (o->kind == OK_BYTEARRAY) { if (is_index_val(vm, key, &idx)) { *out = idx < (u32)o->blen ? v2_int(o->bytes[idx]) : v2_undef(); return 1; } }
    return 0;
}
static int fast_index_set(VM2 *vm, V2 obj, V2 key, V2 v) {
    u32 idx; A2Obj *o;
    if (obj.t != T_OBJ) return 0;
    o = obj.u.o;
    if (o->kind == OK_ARRAY && a2_arr_sealed(vm, o)) return 0;
    if (o->kind == OK_ARRAY) { if (is_index_val(vm, key, &idx)) { if (!a2_arr_set_sparse(vm, o, idx, v)) a2_dyn_set(vm, o, v2_tostr(vm, key), v); return 1; } return 0; }
    if (o->kind == OK_VECTOR) { if (is_index_val(vm, key, &idx)) { if (idx > (u32)o->alen || (o->vec_fixed && idx >= (u32)o->alen)) vec_oob(vm, (double)idx, o->alen); else a2_arr_set(vm, o, (int)idx, a2_vec_coerce(vm, v, o->vec_type)); return 1; } return 0; }
    if (o->kind == OK_DICT) { extern void a2_dict_put(VM2 *vm, A2Obj *o, V2 k, V2 v); if (key.t == T_OBJ && key.u.o->kind != OK_QNAME) { a2_dict_put(vm, o, key, v); return 1; } return 0; }
    if (o->kind == OK_BYTEARRAY) { if (is_index_val(vm, key, &idx)) { extern void a2_bytearray_set(VM2 *vm, A2Obj *o, int i, int v); a2_bytearray_set(vm, o, (int)idx, v2_toint(vm, v)); return 1; } }
    return 0;
}

static A2Str *typeof_str(VM2 *vm, V2 v) {
    switch (v.t) {
    case T_UNDEF: return vm->s_undefined;
    case T_NULL: return vm->s_object;
    case T_BOOL: return a2_str(vm, "boolean");
    case T_INT: case T_UINT: case T_NUM: return a2_str(vm, "number");
    case T_STR: return a2_str(vm, "string");
    case T_OBJ: if (v.u.o->kind == OK_FUNCTION) return a2_str(vm, "function"); if (v.u.o->kind == OK_XML) return a2_str(vm, "xml"); return vm->s_object;
    }
    return vm->s_undefined;
}

/* abstract relational comparison; returns -1 undefined (NaN) / 0 false / 1 true for a < b */
static int less_than(VM2 *vm, V2 a, V2 b) {
    V2 pa = v2_toprim(vm, a, 0), pb = v2_toprim(vm, b, 0);
    if (pa.t == T_STR && pb.t == T_STR) { extern int a2_str_cmp16(A2Str *a, A2Str *b); return a2_str_cmp16(pa.u.s, pb.u.s) < 0; }
    { double x = v2_tonum(vm, pa), y = v2_tonum(vm, pb); if (x != x || y != y) return -1; return x < y; }
}

static V2 do_add(VM2 *vm, V2 a, V2 b) {
    if (v2_is_num(a) && v2_is_num(b)) {
        if (a.t == T_INT && b.t == T_INT) { double d = (double)a.u.i + (double)b.u.i; if (d >= -2147483648.0 && d <= 2147483647.0) return v2_int((s32)d); return v2_num(d); }
        return v2_num(v2_tonum(vm, a) + v2_tonum(vm, b));
    }
    if (a.t == T_OBJ && a.u.o->kind == OK_XML && b.t == T_OBJ && b.u.o->kind == OK_XML) { extern V2 a2_xml_add(VM2 *vm, V2 a, V2 b); return a2_xml_add(vm, a, b); }
    if (a.t == T_STR || b.t == T_STR) { extern void a2_sdep_concat(VM2 *vm, A2Str *a, A2Str *b, A2Str *r); A2Str *sa = v2_tostr(vm, a), *sb = v2_tostr(vm, b), *r = a2_concat(vm, sa, sb); a2_sdep_concat(vm, sa, sb, r); return v2_str(r); }
    /* ToPrimitive without hint: number first, except Date (string) */
    a = v2_toprim(vm, a, a.t == T_OBJ && a.u.o->kind == OK_DATE); b = v2_toprim(vm, b, b.t == T_OBJ && b.u.o->kind == OK_DATE);
    if (a.t == T_STR || b.t == T_STR) return v2_str(a2_concat(vm, v2_tostr(vm, a), v2_tostr(vm, b)));
    return v2_num(v2_tonum(vm, a) + v2_tonum(vm, b));
}

static int hasnext(VM2 *vm, V2 obj, int idx) {
    /* returns next enumeration index (>0) or 0 */
    A2Obj *o; int i, n;
    if (obj.t != T_OBJ) return 0;
    o = obj.u.o;
    if (o->kind == OK_ARRAY || o->kind == OK_VECTOR) {
        for (i = idx; i < o->alen; i++) if (o->arr[i].t != T_HOLE || o->kind == OK_VECTOR) return i + 1;
        idx = idx > o->alen ? idx - o->alen : 0;
        n = o->alen;
    } else if (o->kind == OK_DICT) {
        while (idx < o->dn && o->dkeys[idx].t == T_HOLE) idx++;
        return idx < o->dn ? idx + 1 : 0;
    } else if (o->kind == OK_PROXY) { return a2_proxy_next_index(vm, o, idx);
    } else if (o->kind == OK_QNAME || o->kind == OK_NAMESPACE) { return idx < 2 ? idx + 1 : 0;
    } else if (o->kind == OK_XML || o->kind == OK_BYTEARRAY) {
        extern int a2_xml_hasnext(VM2 *vm, A2Obj *o, int idx);
        if (o->kind == OK_XML) return a2_xml_hasnext(vm, o, idx);
        return idx < o->blen ? idx + 1 : 0;
    } else n = 0;
    { int k = idx; A2Str *key; V2 val; (void)key; (void)val;
      if (o->hash_enum) a2_dyn_enum_hashed(o, &k, &key, &val); else a2_dyn_enum(o, &k, &key, &val);
      if (k) return n + k; }
    return 0;
}
/* avmplus hands out array indices and index-like dynamic keys as int atoms */
static V2 index_key(VM2 *vm, A2Str *key) { u32 i; if (a2_is_index(key, &i) && i < 0x10000000u) return v2_int((s32)i); (void)vm; return v2_str(key); }   /* int atoms hold 29 bits */
static V2 nextname(VM2 *vm, V2 obj, int idx) {
    A2Obj *o;
    if (obj.t != T_OBJ || idx <= 0) return v2_undef();
    o = obj.u.o;
    if (o->kind == OK_ARRAY || o->kind == OK_VECTOR) { if (idx <= o->alen) return v2_int(idx - 1); idx -= o->alen; }
    else if (o->kind == OK_DICT) { V2 k; if (idx > o->dn) return v2_undef(); k = o->dkeys[idx - 1]; if (k.t == T_OBJ) return k; return index_key(vm, v2_tostr(vm, k)); }
    else if (o->kind == OK_BYTEARRAY) return v2_int(idx - 1);
    else if (o->kind == OK_XML) { extern V2 a2_xml_nextname(VM2 *vm, A2Obj *o, int idx); return a2_xml_nextname(vm, o, idx); }
    else if (o->kind == OK_QNAME) return idx == 1 ? v2_cstr(vm, "uri") : idx == 2 ? v2_cstr(vm, "localName") : v2_undef();
    else if (o->kind == OK_NAMESPACE) return idx == 1 ? v2_cstr(vm, "uri") : idx == 2 ? v2_cstr(vm, "prefix") : v2_undef();
    else if (o->kind == OK_PROXY) return a2_proxy_next_name(vm, o, idx);
    { int k = idx - 1; A2Str *key = NULL; V2 val; if (o->hash_enum) a2_dyn_enum_hashed(o, &k, &key, &val); else a2_dyn_enum(o, &k, &key, &val); if (k && key) return index_key(vm, key); }
    return v2_undef();
}
static V2 nextvalue(VM2 *vm, V2 obj, int idx) {
    A2Obj *o;
    if (obj.t != T_OBJ || idx <= 0) return v2_undef();
    o = obj.u.o;
    if (o->kind == OK_ARRAY || o->kind == OK_VECTOR) { if (idx <= o->alen) { V2 v = o->arr[idx - 1]; V2_NORM(v); return v; } idx -= o->alen; }
    else if (o->kind == OK_DICT) return idx <= o->dn ? o->dvals[idx - 1] : v2_undef();
    else if (o->kind == OK_BYTEARRAY) return idx <= o->blen ? v2_int(o->bytes[idx - 1]) : v2_undef();
    else if (o->kind == OK_XML) { extern V2 a2_xml_nextvalue(VM2 *vm, A2Obj *o, int idx); return a2_xml_nextvalue(vm, o, idx); }
    else if (o->kind == OK_PROXY) return a2_proxy_next_value(vm, o, idx);
    else if (o->kind == OK_NAMESPACE) { V2 r = v2_undef(); if (idx == 1 || idx == 2) r = a2_get_s(vm, obj, idx == 1 ? "uri" : "prefix"); return r; }
    else if (o->kind == OK_QNAME) { /* Flash quirk: values come localName first, uri second */ if (idx == 1) return v2_str(a2_qname_local(vm, o)); if (idx == 2) return a2_qname_uri(o) ? v2_str(a2_qname_uri(o)) : v2_null(); return v2_undef(); }
    { int k = idx - 1; A2Str *key = NULL; V2 val; val.t = T_UNDEF; if (o->hash_enum) a2_dyn_enum_hashed(o, &k, &key, &val); else a2_dyn_enum(o, &k, &key, &val); if (k) return val; }
    return v2_undef();
}

static V2 make_arguments(VM2 *vm, int argc, V2 *argv, A2Obj *callee) {
    A2Obj *a = a2_new_array(vm); int i;
    for (i = 0; i < argc; i++) a2_arr_push(vm, a, argv[i]);
    if (callee) a2_dyn_set_hidden(vm, a, vm->s_callee, v2_obj(callee));
    return v2_obj(a);
}

/* a `native` method of the built-in library: arguments are checked, coerced and
 * defaulted as for ActionScript code, then the C body runs */
static V2 call_abc_native(VM2 *vm, A2Method *m, V2 self, int argc, V2 *argv) {
    AbcRT *rt = RT(m->abc); AbcMethod *am = &m->abc->methods[m->index]; V2 small[16], *args = small, r; int n = (int)am->param_count, total, i;
    int req = n - ((am->flags & MF_HAS_OPTIONAL) ? (int)am->option_count : 0);
    if (argc < req || (argc > n && !(am->flags & (MF_NEED_REST | MF_NEED_ARGUMENTS)))) {
        char b[700]; sprintf(b, "Argument count mismatch on %.500s(). Expected %d, got %d.", a2_method_trace_name(vm, m)->s, argc < req ? req : n, argc);
        a2_throw_error(vm, "ArgumentError", b, 1063); return v2_undef();
    }
    total = argc > n ? argc : n;
    if (total > 16) { args = (V2*)malloc((size_t)total * sizeof(V2)); if (!args) return v2_undef(); }
    for (i = 0; i < n; i++) {
        A2Class *pt = rt_type(rt, am->param_types[i]);
        if (i < argc) args[i] = a2_coerce(vm, argv[i], pt);
        else { int oi = i - (n - (int)am->option_count); args[i] = a2_coerce(vm, (am->flags & MF_HAS_OPTIONAL) && oi >= 0 && oi < (int)am->option_count ? const_value(rt, am->options[oi].kind, am->options[oi].val) : v2_undef(), pt); }
    }
    for (; i < total; i++) args[i] = argv[i];
    if (vm->throwing) { if (args != small) free(args); return v2_undef(); }
    if (vm->depth > A2_MAX_DEPTH) { if (args != small) free(args); a2_throw_error(vm, "Error", "Stack overflow occurred.", 1023); return v2_undef(); }
    vm->depth++; vm->cs[vm->depth] = m; vm->cs_cls[vm->depth] = NULL; vm->cs_file[vm->depth] = NULL;
    vm->native_data = m->native_data;
    r = m->abc_native(vm, self, total, args);
    vm->depth--;
    if (args != small) free(args);
    if (vm->throwing) return v2_undef();
    { A2Class *rtp = rt_type(rt, am->return_type); if (rtp) r = a2_coerce(vm, r, rtp); }
    return r;
}
char *g_a2_cstack_base = NULL;  /* an address near the bottom of the thread's stack (set by the front end) */
size_t g_a2_stack_limit = 0;    /* bytes of native stack AS3 calls may use (0: unchecked) */
static V2 interp(VM2 *vm, A2Method *m, A2Obj *scope, V2 self, int argc, V2 *argv) {
    AbcFile *abc = m->abc; AbcRT *rt = RT(abc); AbcMethod *am = &abc->methods[m->index];
    AbcBody *body; V2 *locals, *stk; int nloc, sp, i, scope_base, nouter;
    const u8 *code, *p; u32 pc, ipc, coerced_pc = (u32)-1; A2Class *coerced_t = NULL; A2Frame fr;
    V2 ret = v2_undef(); A2Ns tmpns; A2Name nm; A2Obj *callee; int trops = 0;
    SB_PUBLISH(vm);
    if (am->body < 0 && m->abc_native) return call_abc_native(vm, m, self, argc, argv);
    if (am->body < 0) {   /* a method declared without a body */
        char b[600]; sprintf(b, "The method %.500s() is not implemented.", a2_method_trace_name(vm, m)->s);
        a2_throw_error(vm, "VerifyError", b, 1001); return v2_undef();
    }
    body = &abc->bodies[am->body];
    if (m->verify_state <= 0) {
        char vmsg[256]; int rc = a2_verify_body(vm, m, vmsg, sizeof(vmsg));
        if (rc) { m->verify_state = -1; vm->callee_obj = NULL; a2_throw_error(vm, "VerifyError", vmsg, rc); return v2_undef(); }
        m->verify_state = 1;
    }
    callee = vm->callee_obj; vm->callee_obj = NULL;
    nloc = (int)body->local_count; if (nloc < (int)am->param_count + 2) nloc = (int)am->param_count + 2;
    if (vm->sp + nloc + (int)body->max_stack + 8 >= A2_STACK || vm->depth > A2_MAX_DEPTH) { a2_throw_error(vm, "Error", "Stack overflow occurred.", 1023); return v2_undef(); }
    if (vm->scope_sp + (int)body->max_scope_depth + 64 >= A2_SCOPES) { a2_throw_error(vm, "Error", "Stack overflow occurred.", 1023); return v2_undef(); }
    {   /* the native (C) stack: the interpreter recurses, so a deep AS3 call chain must stop before the thread's stack does */
        char probe; size_t used = g_a2_cstack_base ? (size_t)(g_a2_cstack_base > &probe ? g_a2_cstack_base - &probe : &probe - g_a2_cstack_base) : 0;
        if (g_a2_stack_limit && used > g_a2_stack_limit) { a2_throw_error(vm, "Error", "Stack overflow occurred.", 1023); return v2_undef(); }
    }
    {   /* argument count (checked before the callee's frame exists) */
        int req = (int)am->param_count - ((am->flags & MF_HAS_OPTIONAL) ? (int)am->option_count : 0);
        int unchecked = m->is_function && !(am->flags & MF_HAS_OPTIONAL), over, k;
        for (k = 0; unchecked && k < (int)am->param_count; k++) if (am->param_types[k]) unchecked = 0;
        over = argc > (int)am->param_count && !(am->flags & (MF_NEED_REST | MF_NEED_ARGUMENTS)) && !unchecked;
        if ((argc < req && !unchecked) || over) {
            char exp[16], got[16];
            sprintf(exp, "%d", over ? (int)am->param_count : req); sprintf(got, "%d", argc);
            a2_throw_id(vm, "ArgumentError", 1063, a2_method_trace_name(vm, m)->s, NULL);
            if (vm->throwing && vm->exc.t == T_OBJ) {
                /* %2 / %3: rebuild the message with the counts */
                char b[700]; sprintf(b, "Error #1063: Argument count mismatch on %.500s(). Expected %s, got %s.", a2_method_trace_name(vm, m)->s, exp, got);
                a2_set_s(vm, vm->exc, "message", v2_cstr(vm, b));
            }
            return v2_undef();
        }
    }
    fr.saved_dxns = vm->dxns; vm->dxns = NULL;
    locals = &vm->stack[vm->sp]; vm->sp += nloc;
    for (i = 0; i < nloc; i++) locals[i] = v2_undef();
    locals[0] = self;
    for (i = 0; i < (int)am->param_count; i++) {
        A2Class *pt = rt_type(rt, am->param_types[i]);
        if (i < argc) locals[i + 1] = a2_coerce(vm, argv[i], pt);
        else if (am->flags & MF_HAS_OPTIONAL) {
            int oi = i - ((int)am->param_count - (int)am->option_count);
            if (oi >= 0 && oi < (int)am->option_count) locals[i + 1] = a2_coerce(vm, const_value(rt, am->options[oi].kind, am->options[oi].val), pt);
            else locals[i + 1] = a2_coerce(vm, v2_undef(), pt);
        } else if (am->flags & MF_NEED_REST) {
            locals[i + 1] = a2_coerce(vm, v2_undef(), pt);     /* ...rest functions accept fewer arguments */
        } else {
            locals[i + 1] = a2_coerce(vm, v2_undef(), pt);
        }
    }
    if (vm->throwing) { vm->sp -= nloc; vm->dxns = fr.saved_dxns; return v2_undef(); }   /* argument coercion fails in the caller's frame */
    vm->depth++; vm->cs[vm->depth] = m; vm->cs_cls[vm->depth] = NULL; vm->cs_line[vm->depth] = 0; vm->cs_file[vm->depth] = NULL;
    if (am->flags & MF_NEED_REST) {
        A2Obj *a = a2_new_array(vm);
        for (i = (int)am->param_count; i < argc; i++) a2_arr_push(vm, a, argv[i]);
        locals[am->param_count + 1] = v2_obj(a);
    } else if (am->flags & MF_NEED_ARGUMENTS) {
        if (!callee) { V2 bf = a2_bound_fn(vm, m, scope, self); callee = bf.u.o; }
        locals[am->param_count + 1] = make_arguments(vm, argc, argv, callee);
    }
    stk = &vm->stack[vm->sp]; sp = 0;
    scope_base = vm->scope_sp;
    nouter = 0;
    if (scope) { for (i = 0; i < scope->alen; i++) if (scope->arr[i].t == T_OBJ) { vm->scope_with[vm->scope_sp] = (i < 31 && (scope->ival & (1 << i))) ? 1 : 0; vm->scope_decl[vm->scope_sp] = NULL; vm->scope_stack[vm->scope_sp++] = scope->arr[i].u.o; } nouter = vm->scope_sp - scope_base; }
    fr.caller = vm->cur; fr.m = m; fr.abc = abc; fr.body = body; fr.locals = locals; fr.nlocals = nloc; fr.stack = stk; fr.sp = 0;
    fr.scope = &vm->scope_stack[scope_base]; fr.nscope = nouter; fr.scope_base = scope_base; fr.self = self; fr.saved_scope = scope; fr.activation = NULL;
    vm->cur = &fr;
    code = body->code; pc = 0;
    { static const char *tro; static int troi; if (!troi) { troi = 1; tro = getenv("FLASHNT_TRACEOPS"); } if (tro && strstr(a2_method_trace_name(vm, m)->s, tro)) { const char *tsf = getenv("FLASHNT_TRACESELF"); trops = !tsf || (self.t == T_OBJ && self.u.o->cls && strstr(self.u.o->cls->name->s, tsf)); if (trops) fprintf(stderr, "[op-enter] %s\n", a2_method_trace_name(vm, m)->s); } }   /* debugging: op trace of matching methods */
    if (vm->gc_count > vm->gc_threshold || g_a2_ext_bytes > 160u * 1024 * 1024) { vm->sp += sp; a2_gc(vm); vm->sp -= sp; }

    for (;;) {
        int op;
        if (vm->throwing) {
            /* find handler */
            u32 k; int found = 0;
            for (k = 0; k < body->exception_count; k++) {
                AbcException *e = &body->exceptions[k];
                if (ipc >= e->from && ipc < e->to) {
                    A2Class *et = e->exc_type ? rt_type(rt, e->exc_type) : NULL;
                    if (e->exc_type && !et && rt->names[e->exc_type].name && strcmp(rt->names[e->exc_type].name->s, "*")) {
                        /* the catch type's script has not run yet: define it now; an unknown type catches nothing */
                        V2 sx = vm->exc; A2Name q; vm->throwing = 0; vm->exc = v2_undef();
                        rt_to_name(&rt->names[e->exc_type], &q); find_global_def_dom(vm, &q, a2_abc_domain(rt->abc));
                        vm->throwing = 1; vm->exc = sx;
                        et = rt_type(rt, e->exc_type);
                        if (!et) continue;
                    }
                    if (!et || a2_instanceof(vm, vm->exc, et) || (vm->exc.t != T_OBJ && vm->exc.t != T_NULL && vm->exc.t != T_UNDEF && et == vm->c_object)) {
                        if (vm->player->debug) fprintf(stderr, "[avm2] catch at %u: exc type %d kind %d\n", ipc, vm->exc.t, vm->exc.t == T_OBJ ? vm->exc.u.o->kind : -1);
                        sp = 0; PUSH(vm->exc); vm->throwing = 0; vm->exc = v2_undef();
                        vm->scope_sp = scope_base + nouter;
                        pc = e->target; found = 1; break;
                    }
                }
            }
            if (!found) { if (vm->player->debug) fprintf(stderr, "[avm2] exception leaves %s at pc %u (op 0x%02x)\n", a2_method_trace_name(vm, m)->s, ipc, code[ipc]); break; }
        }
        if (pc >= body->code_len) break;
        ipc = pc; p = code + pc; op = *p++;
        if (trops) { A2Str *ts = NULL; int th = vm->throwing; if (sp > 0) { V2 tv = stk[sp - 1]; ts = tv.t == T_OBJ ? (tv.u.o->cls ? tv.u.o->cls->name : NULL) : v2_tostr(vm, tv); } vm->throwing = th; fprintf(stderr, "[op] %u 0x%02x sp %d top %s\n", ipc, op, sp, ts ? ts->s : "-"); }
        vm->sp = (int)(stk - vm->stack) + sp;   /* keep GC/nested calls above our operands */
        switch (op) {
        case 0x01: case 0x02: case 0x09: case 0xef: case 0xf0: case 0xf1: case 0xf2: case 0xf3:
            if (op == 0xef) { p++; RD30(); p++; RD30(); }
            else if (op == 0xf0) { u32 ln = RD30(); if (vm->debugger) vm->cs_line[vm->depth] = (int)ln; }
            else if (op == 0xf1) { u32 si = RD30(); if (vm->debugger) vm->cs_file[vm->depth] = rt_str(rt, si); }
            break;
        case 0x03: { V2 v = POP(); a2_throw(vm, v); break; }
        case 0x04: case 0x05: case 0x45: case 0x4e: { /* getsuper / setsuper / callsuper / callsupervoid */
            u32 mi = RD30(); int nargs = (op == 0x45 || op == 0x4e) ? (int)RD30() : 0; V2 *args = &stk[sp - nargs]; V2 obj, sv, r; A2Class *sc; A2Trait *t;
            sp -= nargs;
            if (op == 0x05) sv = POP();
            resolve_name(vm, &rt->names[mi], &nm, stk, &sp, &tmpns);
            obj = POP();
            /* methods bound to no class (script initialisers, functions) see Object as their super class;
               class-side (static) methods see Class */
            sc = m->owner ? m->owner->super : vm->c_object;
            /* the receiver is coerced to the declaring class (null -> 1009, foreign object -> 1034) */
            if (obj.t == T_UNDEF || obj.t == T_NULL) { throw_null_ref(vm, v2_null()); break; }
            if (m->owner && obj.t == T_OBJ && obj.u.o->kind == OK_CLASS && obj.u.o->as_class == m->owner) sc = vm->c_class;
            else if (m->owner && m->owner->kind != OK_GLOBAL && !a2_instanceof(vm, obj, m->owner)) { a2_throw_coerce_error(vm, obj, m->owner); break; }
            t = sc ? a2_find_trait(&sc->itraits, nm.name, &nm) : NULL;
            if (!t) { if (!sc) { a2_throw_error(vm, "ReferenceError", "super not found", 1070); break; }
                      /* dynamic prototype fallback */
                      if (op == 0x04) { V2 o2; if (proto_get(vm, sc->prototype, &nm, &o2)) { PUSH(o2); break; } }
                      if (op == 0x04 && sc == vm->c_class && obj.t == T_OBJ && obj.u.o->kind == OK_CLASS && nm.name == vm->s_prototype) { PUSH(obj.u.o->as_class->prototype ? v2_obj(obj.u.o->as_class->prototype) : v2_null()); break; }   /* Class.prototype is a getter */
                      a2_throw_error(vm, "ReferenceError", "Method not found on super", 1070); break; }
            switch (op) {
            case 0x04:
                if (t->kind == TK_GETTER || t->kind == TK_SETTER) { if (!t->getter) { a2_throw_error(vm, "ReferenceError", "Illegal read", 1077); break; } r = a2_call_method(vm, t->getter, NULL, obj, 0, NULL); }
                else if (t->kind == TK_METHOD) r = a2_bound_fn(vm, t->method, NULL, obj);
                else { if (vm->sb_str && obj.t == T_OBJ && obj.u.o == vm->sb_obj) a2_sb_publish(vm); r = (obj.t == T_OBJ && t->slot < obj.u.o->nslots) ? obj.u.o->slots[t->slot] : v2_undef(); }
                PUSH(r); break;
            case 0x05:
                /* setsuper on a getter-only accessor or a const is silently ignored (Flash quirk) */
                if (t->kind == TK_GETTER || t->kind == TK_SETTER) { if (t->setter) a2_call_method(vm, t->setter, NULL, obj, 1, &sv); }
                else if (t->kind == TK_SLOT) { if (obj.t == T_OBJ && t->slot < obj.u.o->nslots) { int was = vm->throwing; V2 cv = a2_coerce(vm, sv, t->type); if (was || !vm->throwing) obj.u.o->slots[t->slot] = cv; } }
                else if (t->kind == TK_CONST) { }
                else a2_throw_error(vm, "ReferenceError", "Cannot assign to super method", 1037);
                break;
            default:
                if (t->kind == TK_METHOD) r = a2_call_method(vm, t->method, NULL, obj, nargs, args);
                else { V2 f; if (t->kind == TK_GETTER && t->getter) f = a2_call_method(vm, t->getter, NULL, obj, 0, NULL); else f = (obj.t == T_OBJ && t->slot < obj.u.o->nslots) ? obj.u.o->slots[t->slot] : v2_undef(); r = a2_call(vm, f, obj, nargs, args); }
                if (op == 0x45) PUSH(r);
            }
            break; }
        case 0x06: { u32 si = RD30(); vm->dxns = rt_str(rt, si); break; }            /* dxns */
        case 0x07: { V2 v = POP(); vm->dxns = (v.t == T_OBJ && v.u.o->kind == OK_NAMESPACE) ? v.u.o->nsval.uri : v2_tostr(vm, v); break; }   /* dxnslate */
        case 0x08: { u32 r = RD30(); if ((int)r < nloc) locals[r] = v2_undef(); break; }
        case 0x0c: case 0x0d: case 0x0e: case 0x0f: case 0x15: case 0x16: case 0x17: case 0x18: {
            s32 off = RDS24(); V2 b = POP(), a = POP(); int r = 0, lt;
            switch (op) {
            case 0x0c: lt = less_than(vm, a, b); r = !(lt == 1); break;         /* ifnlt */
            case 0x0d: lt = less_than(vm, b, a); r = !(lt == 0); break;         /* ifnle: !(a<=b) = !(!(b<a)) */
            case 0x0e: lt = less_than(vm, b, a); r = !(lt == 1); break;         /* ifngt */
            case 0x0f: lt = less_than(vm, a, b); r = !(lt == 0); break;         /* ifnge */
            case 0x15: lt = less_than(vm, a, b); r = (lt == 1); break;
            case 0x16: lt = less_than(vm, b, a); r = (lt == 0); break;
            case 0x17: lt = less_than(vm, b, a); r = (lt == 1); break;
            case 0x18: lt = less_than(vm, a, b); r = (lt == 0); break;
            }
            if (r) { pc = (u32)((s32)(p - code) + off); continue; }
            break; }
        case 0x10: { s32 off = RDS24(); pc = (u32)((s32)(p - code) + off); continue; }
        case 0x11: { s32 off = RDS24(); V2 v = POP(); if (v2_tobool(vm, v)) { pc = (u32)((s32)(p - code) + off); continue; } break; }
        case 0x12: { s32 off = RDS24(); V2 v = POP(); if (!v2_tobool(vm, v)) { pc = (u32)((s32)(p - code) + off); continue; } break; }
        case 0x13: case 0x14: case 0x19: case 0x1a: {
            s32 off = RDS24(); V2 b = POP(), a = POP(); int eq = (op == 0x13 || op == 0x14) ? v2_eq(vm, a, b) : v2_strict_eq(vm, a, b);
            if (op == 0x14 || op == 0x1a) eq = !eq;
            if (eq) { pc = (u32)((s32)(p - code) + off); continue; }
            break; }
        case 0x1b: { /* lookupswitch */
            const u8 *base = code + ipc; s32 def = RDS24(); u32 n = RD30() + 1, k; s32 idx = v2_toint(vm, POP()); s32 off = def;
            for (k = 0; k < n; k++) { s32 o2 = RDS24(); if ((u32)idx == k) off = o2; }
            pc = (u32)((s32)(base - code) + off); continue; }
        case 0x1c: { V2 v = POP(); if (v.t == T_UNDEF || v.t == T_NULL) { throw_null_ref(vm, v); break; } vm->scope_with[vm->scope_sp] = 1; vm->scope_decl[vm->scope_sp] = NULL; vm->scope_stack[vm->scope_sp++] = v2_toobj(vm, v); break; }
        case 0x1d: if (vm->scope_sp > scope_base + nouter) vm->scope_sp--; break;
        case 0x1e: { V2 idx = POP(), obj = POP(); PUSH(nextname(vm, obj, v2_toint(vm, idx))); break; }
        case 0x1f: { V2 idx = POP(), obj = POP(); PUSH(v2_int(hasnext(vm, obj, v2_toint(vm, idx)))); break; }
        case 0x20: PUSH(v2_null()); break;
        case 0x21: PUSH(v2_undef()); break;
        case 0x23: { V2 idx = POP(), obj = POP(); PUSH(nextvalue(vm, obj, v2_toint(vm, idx))); break; }
        case 0x24: PUSH(v2_int((s8)*p++)); break;
        case 0x25: { u32 v = RD30(); PUSH(v2_int((s32)(s16)v)); break; }
        case 0x26: PUSH(v2_bool(1)); break;
        case 0x27: PUSH(v2_bool(0)); break;
        case 0x28: PUSH(v2_num(0.0 / 0.0)); break;
        case 0x29: sp--; break;
        case 0x2a: { V2 v = TOP(); PUSH(v); break; }
        case 0x2b: { V2 a = stk[sp - 1]; stk[sp - 1] = stk[sp - 2]; stk[sp - 2] = a; break; }
        case 0x2c: { u32 si = RD30(); PUSH(v2_str(rt_str(rt, si))); break; }
        case 0x2d: { u32 k = RD30(); PUSH(v2_int(k < abc->int_count ? abc->ints[k] : 0)); break; }
        case 0x2e: { u32 k = RD30(); PUSH(v2_uint(k < abc->uint_count ? abc->uints[k] : 0)); break; }
        case 0x2f: { u32 k = RD30(); PUSH(v2_num(k < abc->double_count ? abc->doubles[k] : 0)); break; }
        case 0x30: { V2 v = POP(); if (v.t == T_UNDEF || v.t == T_NULL) { throw_null_ref(vm, v); break; } vm->scope_with[vm->scope_sp] = 0; vm->scope_decl[vm->scope_sp] = (ipc == coerced_pc && coerced_t && !(coerced_t->flags & CF_INTERFACE) && v.t == T_OBJ) ? coerced_t : NULL; vm->scope_stack[vm->scope_sp++] = v2_toobj(vm, v); break; }
        case 0x31: { u32 k = RD30(); PUSH(const_value(rt, CK_NAMESPACE, k)); break; }
        case 0x32: { /* hasnext2 */
            u32 oreg = RD30(), ireg = RD30(); int nxt;
            if ((int)oreg >= nloc || (int)ireg >= nloc) { PUSH(v2_bool(0)); break; }
            nxt = hasnext(vm, locals[oreg], v2_toint(vm, locals[ireg]));
            if (!nxt) {
                /* walk prototype chain */
                V2 o = locals[oreg];
                if (o.t == T_INT || o.t == T_UINT || o.t == T_NUM || o.t == T_STR || o.t == T_BOOL) {   /* a primitive enumerates its class prototype */
                    A2Class *pc = v2_class_of(vm, o); if (pc == vm->c_int || pc == vm->c_uint) pc = vm->c_number;
                    o = pc ? v2_obj(pc->prototype) : v2_null(); if (o.t == T_OBJ) { nxt = hasnext(vm, o, 0); if (nxt) locals[oreg] = o; }
                }
                while (!nxt && o.t == T_OBJ && o.u.o->proto && o.u.o->kind != OK_ARRAY && o.u.o->kind != OK_DICT && o.u.o->kind != OK_VECTOR) { o = v2_obj(o.u.o->proto); nxt = hasnext(vm, o, 0); if (nxt) locals[oreg] = o; }
            }
            if (!nxt) { locals[oreg] = v2_null(); locals[ireg] = v2_int(0); PUSH(v2_bool(0)); }
            else { locals[ireg] = v2_int(nxt); PUSH(v2_bool(1)); }
            break; }
        case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: { /* li8 li16 li32 lf32 lf64 */
            extern V2 a2_domain_memory_load(VM2 *vm, int op, u32 addr); u32 addr = v2_touint(vm, POP()); PUSH(a2_domain_memory_load(vm, op, addr)); break; }
        case 0x3a: case 0x3b: case 0x3c: case 0x3d: case 0x3e: { extern void a2_domain_memory_store(VM2 *vm, int op, u32 addr, V2 v); u32 addr = v2_touint(vm, POP()); V2 v = POP(); a2_domain_memory_store(vm, op, addr, v); break; }
        case 0x40: { /* newfunction */
            u32 mi = RD30(); A2Method *fm = rt_method(rt, mi); A2Obj *sc = a2_new_scope(vm, &vm->scope_stack[scope_base], vm->scope_sp - scope_base); A2Obj *f;
            if (fm) fm->is_function = 1;
            f = a2_new_fn(vm, fm, sc);
            PUSH(v2_obj(f)); break; }
        case 0x41: { /* call */
            u32 n = RD30(); V2 *args = &stk[sp - n]; V2 recv, fn, r; sp -= (int)n; recv = POP(); fn = POP();
            r = a2_call(vm, fn, recv, (int)n, args); PUSH(r); break; }
        case 0x42: { u32 n = RD30(); V2 *args = &stk[sp - n]; V2 ctor, r; sp -= (int)n; ctor = POP(); r = a2_construct(vm, ctor, (int)n, args); PUSH(r); break; }
        case 0x43: { /* callmethod (disp id): unsupported - treat as no-op returning undefined */ RD30(); { u32 n = RD30(); sp -= (int)n; } POP(); PUSH(v2_undef()); break; }
        case 0x44: { u32 mi = RD30(), n = RD30(); V2 *args = &stk[sp - n]; V2 recv, r; sp -= (int)n; recv = POP(); { A2Method *cm = rt_method(rt, mi); if (cm && cm->owner && !(cm->is_static && recv.t == T_OBJ && recv.u.o->kind == OK_CLASS && recv.u.o->as_class->inst == cm->owner->inst)) { recv = a2_coerce(vm, recv, cm->owner); if (vm->throwing) { PUSH(v2_undef()); break; } } r = a2_call_method(vm, cm, NULL, recv, (int)n, args); } PUSH(r); break; }
        case 0x46: case 0x4c: case 0x4f: { /* callproperty / callproplex / callpropvoid */
            u32 mi = RD30(), n = RD30(); V2 *args = &stk[sp - n]; V2 obj, r; int base_sp;
            sp -= (int)n; base_sp = sp;
            resolve_name(vm, &rt->names[mi], &nm, stk, &sp, &tmpns);
            obj = POP();
            if (obj.t == T_UNDEF || obj.t == T_NULL) { throw_null_ref(vm, obj); break; }
            /* keep args reachable for GC during the call */
            { int save = sp; sp = base_sp; vm->sp = (int)(stk - vm->stack) + base_sp + (int)n; (void)save;
              if (op == 0x4c) { V2 f; if (a2_get(vm, obj, &nm, &f)) r = a2_call(vm, f, v2_null(), (int)n, args); else { if (!vm->throwing) { char b[300]; sprintf(b, "Property %.120s not found", nm.name->s); a2_throw_error(vm, "ReferenceError", b, 1069); } r = v2_undef(); } }
              else r = a2_call_prop(vm, obj, &nm, (int)n, args);
              sp = base_sp - 1 - (rt->names[mi].rt_name ? 1 : 0) - (rt->names[mi].rt_ns ? 1 : 0); }
            if (op != 0x4f) PUSH(r);
            break; }
        case 0x47: ret = v2_undef(); { A2Class *rtp = rt_type(rt, am->return_type); if (rtp) ret = a2_coerce(vm, ret, rtp); } if (vm->throwing) break; goto done;
        case 0x48: ret = POP(); { A2Class *rtp = rt_type(rt, am->return_type); if (rtp) ret = a2_coerce(vm, ret, rtp); } if (vm->throwing) break; goto done;
        case 0x49: { /* constructsuper */
            u32 n = RD30(); V2 *args = &stk[sp - n]; V2 obj; A2Class *sc; int top = sp; sp -= (int)n; obj = POP();
            if (obj.t == T_UNDEF || obj.t == T_NULL) { throw_null_ref(vm, obj); break; }
            sc = m->owner && !m->is_static ? m->owner->super : NULL;   /* a class initialiser's super is Object's constructor (nothing to do) */
            if (sc && obj.t == T_OBJ) {
                int native = !(sc->iinit && !sc->iinit->native);
                vm->sp = (int)(stk - vm->stack) + top;
                if (native) { vm->depth++; vm->cs[vm->depth] = NULL; vm->cs_cls[vm->depth] = sc; vm->cs_file[vm->depth] = NULL; }
                a2_run_ctor(vm, sc, obj.u.o, (int)n, args);
                if (native) vm->depth--;
            }
            break; }
        case 0x4a: { /* constructprop */
            u32 mi = RD30(), n = RD30(); V2 *args = &stk[sp - n]; V2 obj, r; int base_sp;
            sp -= (int)n; base_sp = sp;
            resolve_name(vm, &rt->names[mi], &nm, stk, &sp, &tmpns);
            obj = POP();
            if (obj.t == T_UNDEF || obj.t == T_NULL) { throw_null_ref(vm, obj); break; }
            vm->sp = (int)(stk - vm->stack) + base_sp + (int)n;
            r = a2_construct_prop(vm, obj, &nm, (int)n, args);
            PUSH(r); break; }
        case 0x50: { s32 v = v2_toint(vm, POP()); PUSH(v2_int((v & 1) ? -1 : 0)); break; }            /* sxi1 */
        case 0x51: { s32 v = v2_toint(vm, POP()); PUSH(v2_int((s32)(s8)(u8)v)); break; }               /* sxi8 */
        case 0x52: { s32 v = v2_toint(vm, POP()); PUSH(v2_int((s32)(s16)(u16)v)); break; }             /* sxi16 */
        case 0x53: { /* applytype */
            u32 n = RD30(); V2 *args = &stk[sp - n]; V2 base; sp -= (int)n; base = POP();
            { extern V2 a2_vector_apply(VM2 *vm, V2 base, int argc, V2 *argv); PUSH(a2_vector_apply(vm, base, (int)n, args)); }
            break; }
        case 0x55: { /* newobject */
            u32 n = RD30(), k; A2Obj *o = a2_new_plain(vm); o->hash_enum = !rt->abc->is_builtin;   /* (the player's own AS library keeps insertion order) */
            for (k = n; k-- > 0;) { V2 v = stk[sp - 2 * (n - k) + 1], key = stk[sp - 2 * (n - k)]; a2_dyn_set(vm, o, v2_tostr(vm, key), v); }   /* avmplus pops the pairs: the last one is set first */
            sp -= 2 * (int)n; PUSH(v2_obj(o)); break; }
        case 0x56: { u32 n = RD30(), k; A2Obj *a = a2_new_array(vm); for (k = 0; k < n; k++) a2_arr_push(vm, a, stk[sp - (int)n + (int)k]); sp -= (int)n; PUSH(v2_obj(a)); break; }
        case 0x57: { A2Class *ac = activation_class(rt, body); A2Obj *o = a2_new(vm, ac); o->kind = OK_ACTIVATION; o->proto = NULL; fr.activation = o;
            init_function_traits(vm, ac, o, NULL);
            PUSH(v2_obj(o)); break; }
        case 0x58: { /* newclass */
            u32 ci = RD30(); V2 basev = POP(); A2Class *base = (basev.t == T_OBJ && basev.u.o->kind == OK_CLASS) ? basev.u.o->as_class : NULL; A2Class *c;
            if (ci >= abc->class_count) { PUSH(v2_null()); break; }
            {   /* the base must be a Class matching the declared superclass */
                AbcInstance *inst = &abc->instances[ci];
                if (basev.t != T_NULL && basev.t != T_UNDEF && !(basev.t == T_OBJ && basev.u.o->kind == OK_CLASS)) { a2_throw_coerce_error(vm, basev, vm->c_class); PUSH(v2_null()); break; }
                if (!base && inst->super_name) { a2_throw_error(vm, "TypeError", "Cannot access a property or method of a null object reference.", 1009); PUSH(v2_null()); break; }
                if (base && inst->super_name && inst->super_name < abc->mn_count) {
                    RTName *sn = &rt->names[inst->super_name];
                    if (sn->name && base->name && strcmp(sn->name->s, base->name->s)) { a2_throw_error(vm, "VerifyError", "The OP_newclass opcode was used with the incorrect base class.", 1108); PUSH(v2_null()); break; }
                }
            }
            if (basev.t == T_OBJ && basev.u.o->kind == OK_CLASS && (base->flags & CF_FINAL) && !(base == vm->c_object)) { /* allowed anyway */ }
            /* newclass run twice on one class makes a second, independent class object */
            rt->fresh = rt->classes[ci] != NULL;
            c = build_class(rt, ci, base, &vm->scope_stack[scope_base], vm->scope_sp - scope_base);
            rt->fresh = 0;
            vm->sp = (int)(stk - vm->stack) + sp;
            a2_class_init(vm, c);
            PUSH(v2_obj(c->obj)); break; }
        case 0x59: { u32 mi = RD30(); V2 obj; extern V2 a2_xml_descendants(VM2 *vm, V2 obj, A2Name *n); resolve_name(vm, &rt->names[mi], &nm, stk, &sp, &tmpns); obj = POP(); if (obj.t == T_OBJ && obj.u.o->kind == OK_PROXY) { PUSH(a2_proxy_descendants(vm, obj.u.o, &nm)); break; } PUSH(a2_xml_descendants(vm, obj, &nm)); break; }
        case 0x5a: { u32 ei = RD30(); A2Class *cc = catch_class(rt, body, ei); A2Obj *o = a2_new(vm, cc); o->kind = OK_CATCH; o->proto = NULL; PUSH(v2_obj(o)); break; }
        case 0x5d: case 0x5e: case 0x60: { /* findpropstrict / findproperty / getlex */
            u32 mi = RD30(); V2 holder; int found;
            resolve_name(vm, &rt->names[mi], &nm, stk, &sp, &tmpns);
            found = a2_find_property(vm, &vm->scope_stack[scope_base], vm->scope_sp - scope_base, &nm, &holder);
            if (vm->player->debug > 2) fprintf(stderr, "[avm2] findprop %s (nss=%d kind=%d) found=%d holder kind=%d nscope=%d\n", nm.name->s, nm.nns, nm.nss ? nm.nss[0].kind : -1, found, found && holder.t == T_OBJ ? holder.u.o->kind : -1, vm->scope_sp - scope_base);
            if (vm->throwing) break;
            if (!found) {
                if (op == 0x5e) { PUSH(v2_obj(vm->scope_sp > scope_base ? vm->scope_stack[scope_base] : vm->global)); break; }
                { char b[300]; sprintf(b, "Variable %.120s is not defined.", nm.name->s); a2_throw_error(vm, "ReferenceError", b, 1065); }
                break;
            }
            if (op == 0x60 && holder.t == T_OBJ && holder.u.o->kind == OK_XML) { extern V2 a2_xml_get(VM2 *vm, V2 obj, A2Name *n); PUSH(a2_xml_get(vm, holder, &nm)); break; }
            if (op == 0x60) { V2 v; if (!a2_get(vm, holder, &nm, &v) && !vm->throwing) { char b[300]; sprintf(b, "Variable %.120s is not defined.", nm.name->s); a2_throw_error(vm, "ReferenceError", b, 1065); break; } PUSH(v); }
            else PUSH(holder);
            break; }
        case 0x5f: { u32 mi = RD30(); A2Obj *g; resolve_name(vm, &rt->names[mi], &nm, stk, &sp, &tmpns); g = find_global_def(vm, &nm); if (!g) { char b[300]; sprintf(b, "Variable %.250s is not defined.", nm.name->s); a2_throw_error(vm, "ReferenceError", b, 1065); break; } PUSH(v2_obj(g)); break; }
        case 0x61: case 0x68: { /* setproperty / initproperty */
            u32 mi = RD30(); V2 v = POP(), obj; RTName *rn = &rt->names[mi];
            int nopub = rn_nopub_strict(vm, rn, m);
            if (rn->rt_name && !rn->rt_ns && !nopub) { V2 key = stk[sp - 1]; if (fast_index_set(vm, stk[sp - 2], key, v)) { sp -= 2; break; } }
            resolve_name(vm, rn, &nm, stk, &sp, &tmpns);
            obj = POP();
            if (nopub && obj.t == T_OBJ && obj.u.o->kind != OK_XML && obj.u.o->kind != OK_PROXY && obj.u.o->kind != OK_CLASS && obj.u.o->cls) { if (!a2_find_trait(&obj.u.o->cls->itraits, nm.name, &nm)) { if (!vm->throwing) a2_throw_cannot_create(vm, obj, &nm); break; } }
            if (obj.t == T_OBJ && obj.u.o->kind == OK_XML) { extern void a2_xml_set(VM2 *vm, V2 obj, A2Name *n, V2 v); a2_xml_set(vm, obj, &nm, v); break; }
            if (op == 0x61) a2_set(vm, obj, &nm, v); else a2_init(vm, obj, &nm, v);
            break; }
        case 0x62: { u32 r = RD30(); PUSH((int)r < nloc ? locals[r] : v2_undef()); break; }
        case 0x63: { u32 r = RD30(); V2 v = POP(); if ((int)r < nloc) locals[r] = v; break; }
        case 0x64: PUSH(v2_obj(vm->scope_sp > scope_base ? vm->scope_stack[scope_base] : vm->global)); break;
        case 0x65: { int k = *p++; PUSH(scope_base + nouter + k < vm->scope_sp ? v2_obj(vm->scope_stack[scope_base + nouter + k]) : v2_undef()); break; }
        case 0x67: { u32 k = RD30(); PUSH(scope && (int)k < scope->alen ? scope->arr[k] : v2_undef()); break; }   /* getouterscope */
        case 0x66: { /* getproperty */
            u32 mi = RD30(); V2 obj, v; RTName *rn = &rt->names[mi];
            int nopub = rn_nopub_strict(vm, rn, m);
            if (rn->rt_name && !rn->rt_ns && !nopub) { V2 key = stk[sp - 1]; if (fast_index_get(vm, stk[sp - 2], key, &v)) { sp -= 2; PUSH(v); break; } }
            resolve_name(vm, rn, &nm, stk, &sp, &tmpns);
            obj = POP();
            if (nopub && obj.t == T_OBJ && obj.u.o->kind != OK_XML && obj.u.o->kind != OK_PROXY && obj.u.o->kind != OK_CLASS && obj.u.o->cls && !a2_find_trait(&obj.u.o->cls->itraits, nm.name, &nm)) { if (!vm->throwing) { char nb[400]; a2_throw_id(vm, "ReferenceError", 1081, name_fmt(vm, &nm, nb), obj_cls_fmt(vm, obj)); } break; }
            if (obj.t == T_OBJ && obj.u.o->kind == OK_XML) { extern V2 a2_xml_get(VM2 *vm, V2 obj, A2Name *n); PUSH(a2_xml_get(vm, obj, &nm)); break; }
            /* obj.* on a non-XML object: the any-name is never a valid dynamic name */
            if (!rn->rt_name && nm.name->len == 0 && rn_is_nsset(rn) && obj.t == T_OBJ && obj.u.o->kind != OK_PROXY) { a2_throw_id(vm, "ReferenceError", 1081, "*", obj_cls_fmt(vm, obj)); break; }
            if (!a2_get(vm, obj, &nm, &v)) {
                if (vm->throwing) break;
                if ((obj.t == T_OBJ && obj.u.o->kind == OK_DICT && !name_has_public(vm, &nm)) || (obj.t == T_OBJ && obj.u.o->cls && !obj.u.o->cls->is_dynamic && (obj.u.o->kind != OK_PLAIN || obj.u.o->cls->abc) && obj.u.o->kind != OK_CLASS && obj.u.o->kind != OK_FUNCTION && obj.u.o->kind != OK_GLOBAL) || obj.t == T_BOOL || obj.t == T_INT || obj.t == T_UINT || obj.t == T_NUM || obj.t == T_STR) {
                    /* a namespace set without the public namespace: ReadSealedErrorNs */
                    if (rn_is_nsset(rn) && nm.nss != &tmpns && obj.t == T_OBJ && (!name_has_public(vm, &nm) || nm.attr)) {   /* (a runtime QName makes it a QName) */ char nb[400]; a2_throw_id(vm, "ReferenceError", 1081, name_fmt(vm, &nm, nb), obj_cls_fmt(vm, obj)); break; }
                    a2_throw_not_found(vm, obj, &nm); break;
                }
                v = v2_undef();
            }
            PUSH(v); break; }
        case 0x6a: { u32 mi = RD30(); V2 obj;
            if (rt->names[mi].rt_name && !rt->names[mi].rt_ns && sp >= 2 && stk[sp - 1].t == T_OBJ && stk[sp - 1].u.o->kind != OK_PRIM && stk[sp - 1].u.o->kind != OK_QNAME && stk[sp - 2].t == T_OBJ && stk[sp - 2].u.o->kind == OK_DICT) {
                extern int a2_dict_delete(VM2 *vm, A2Obj *o, V2 k); V2 key = POP(); obj = POP(); a2_dict_delete(vm, obj.u.o, key); PUSH(v2_bool(1)); break;   /* Dictionary object keys */
            }
            /* delete x.(filter): a runtime XMLList name is not supported */
            if (rt->names[mi].rt_name && !rt->names[mi].rt_ns && sp >= 1 && stk[sp - 1].t == T_OBJ && stk[sp - 1].u.o->kind == OK_XML && stk[sp - 1].u.o->cls == vm->c_xmllist) { sp -= 2; a2_throw_id(vm, "TypeError", 1119, "XMLList", NULL); break; }
            resolve_name(vm, &rt->names[mi], &nm, stk, &sp, &tmpns); obj = POP();
            if (obj.t == T_UNDEF || obj.t == T_NULL) { throw_null_ref(vm, obj); break; }
            if (obj.t != T_OBJ) { char nb[400]; a2_throw_id(vm, "ReferenceError", 1120, name_fmt(vm, &nm, nb), obj_cls_fmt(vm, obj)); break; }
            if (obj.t == T_OBJ && obj.u.o->kind == OK_XML) { extern int a2_xml_delete(VM2 *vm, V2 obj, A2Name *n); PUSH(v2_bool(a2_xml_delete(vm, obj, &nm))); break; }
            PUSH(v2_bool(a2_delete(vm, obj, &nm))); break; }
        case 0x6c: { u32 k = RD30(); V2 obj = POP();
            if (obj.t != T_OBJ) { throw_null_ref(vm, obj); break; }
            /* the builder's slot read by `getslot k; pushstring s; add` stays unpublished */
            if (vm->sb_str && obj.u.o == vm->sb_obj && (int)k - 1 == vm->sb_slot && !sb_push_add(p)) a2_sb_publish(vm);
            PUSH((int)k >= 1 && (int)k <= obj.u.o->nslots ? obj.u.o->slots[k - 1] : v2_undef()); break; }
        case 0x6d: { u32 k = RD30(); V2 v = POP(), obj = POP();
            if (obj.t != T_OBJ) { throw_null_ref(vm, obj); break; }
            if ((int)k >= 1 && (int)k <= obj.u.o->nslots) {
                A2Trait *t = NULL; A2Class *c = obj.u.o->cls; int b;
                if (c) for (b = 0; b < c->itraits.nb && !t; b++) { A2Trait *q; for (q = c->itraits.b[b]; q; q = q->next) if ((q->kind == TK_SLOT || q->kind == TK_CONST) && q->slot == (int)k - 1) { t = q; break; } }
                { int was = vm->throwing; V2 cv = t && a2_resolve_trait_type(vm, t) ? a2_coerce(vm, v, t->type) : v; if (was || !vm->throwing) obj.u.o->slots[k - 1] = cv; }
            }
            break; }
        case 0x6e: { u32 k = RD30(); A2Obj *g = vm->scope_sp > scope_base ? vm->scope_stack[scope_base] : vm->global; if (vm->sb_str && g == vm->sb_obj) a2_sb_publish(vm); PUSH((int)k >= 1 && (int)k <= g->nslots ? g->slots[k - 1] : v2_undef()); break; }
        case 0x6f: { u32 k = RD30(); V2 v = POP(); A2Obj *g = vm->scope_sp > scope_base ? vm->scope_stack[scope_base] : vm->global; if ((int)k >= 1 && (int)k <= g->nslots) g->slots[k - 1] = v; break; }
        case 0x70: { V2 v = POP(); PUSH(v2_str(v2_tostr(vm, v))); break; }
        case 0x71: { V2 v = POP(); extern V2 a2_xml_escape(VM2 *vm, V2 v, int attr); PUSH(a2_xml_escape(vm, v, 0)); break; }
        case 0x72: { V2 v = POP(); extern V2 a2_xml_escape(VM2 *vm, V2 v, int attr); PUSH(a2_xml_escape(vm, v, 1)); break; }
        case 0x73: case 0x83: { V2 v = POP(); PUSH(v2_int(v2_toint(vm, v))); break; }
        case 0x74: case 0x88: { V2 v = POP(); PUSH(v2_uint(v2_touint(vm, v))); break; }
        case 0x75: case 0x84: { V2 v = POP(); PUSH(v.t == T_NUM ? v : v2_num(v2_tonum(vm, v))); break; }
        case 0x76: case 0x81: { V2 v = POP(); PUSH(v2_bool(v2_tobool(vm, v))); break; }
        case 0x77: case 0x89: { V2 v = TOP(); if (v.t == T_UNDEF || v.t == T_NULL) { if (op == 0x77) { POP(); throw_null_ref(vm, v); } else { POP(); PUSH(v2_null()); } } break; }
        case 0x78: { V2 v = TOP(); if (v.t == T_NULL || v.t == T_UNDEF) { POP(); throw_null_ref(vm, v); break; } if (!(v.t == T_OBJ && v.u.o->kind == OK_XML)) { char b[400]; A2Class *vc = (v.t == T_INT || v.t == T_UINT || v.t == T_NUM) ? vm->c_number : v2_class_of(vm, v); POP(); sprintf(b, "Filter operator not supported on type %.250s.", vc ? a2_class_name(vm, vc, 0)->s : (v.t == T_NULL ? "null" : "void")); a2_throw_error(vm, "TypeError", b, 1123); } break; }
        case 0x80: { u32 mi = RD30(); A2Class *t = rt_type(rt, mi); V2 v = POP();
            coerced_t = t; coerced_pc = (u32)(p - code);   /* a following pushscope binds names through this type */
            PUSH(a2_coerce(vm, v, t)); break; }
        case 0x82: break;
        case 0x85: { V2 v = POP(); if (v.t == T_UNDEF || v.t == T_NULL) PUSH(v2_null()); else PUSH(v2_str(v2_tostr(vm, v))); break; }
        case 0x86: { u32 mi = RD30(); A2Class *t = rt_type(rt, mi); V2 v = POP(); PUSH(t && a2_instanceof(vm, v, t) ? v : v2_null()); break; }
        case 0x87: { V2 tv = POP(), v = POP(); A2Class *t = (tv.t == T_OBJ && tv.u.o->kind == OK_CLASS) ? tv.u.o->as_class : NULL; if (!t) { if (tv.t == T_OBJ) a2_throw_id(vm, "TypeError", 1041, NULL, NULL); else throw_null_ref(vm, tv.t == T_UNDEF ? tv : v2_null()); break; } PUSH((t == vm->c_object && v.t != T_UNDEF) || a2_instanceof(vm, v, t) ? v : v2_null()); break; }
        case 0x90: { V2 v = POP(); PUSH(v2_num(-v2_tonum(vm, v))); break; }
        case 0x91: { V2 v = POP(); PUSH(v2_num(v2_tonum(vm, v) + 1)); break; }
        case 0x92: { u32 r = RD30(); if ((int)r < nloc) locals[r] = v2_num(v2_tonum(vm, locals[r]) + 1); break; }
        case 0x93: { V2 v = POP(); PUSH(v2_num(v2_tonum(vm, v) - 1)); break; }
        case 0x94: { u32 r = RD30(); if ((int)r < nloc) locals[r] = v2_num(v2_tonum(vm, locals[r]) - 1); break; }
        case 0x95: { V2 v = POP(); PUSH(v2_str(typeof_str(vm, v))); break; }
        case 0x96: { V2 v = POP(); PUSH(v2_bool(!v2_tobool(vm, v))); break; }
        case 0x97: { V2 v = POP(); PUSH(v2_int(~v2_toint(vm, v))); break; }
        case 0xa0: { V2 b = POP(), a = POP();
            if (a.t == T_STR && b.t == T_STR && a.u.s->len >= 4096) {
                u32 k = sb_pattern_after(p); V2 r;
                if (k && sb_append(vm, vm->scope_sp > scope_base ? vm->scope_stack[scope_base] : vm->global, k, a.u.s, b.u.s, &r)) { PUSH(r); break; }
            }
            if (vm->sb_str && ((a.t == T_STR && a.u.s == vm->sb_str) || (b.t == T_STR && b.u.s == vm->sb_str))) {
                A2Str *old = vm->sb_str, *e = a2_sb_publish(vm);
                if (a.t == T_STR && a.u.s == old) a.u.s = e;
                if (b.t == T_STR && b.u.s == old) b.u.s = e;
            }
            PUSH(do_add(vm, a, b)); break; }
        case 0xa1: { V2 b = POP(), a = POP(); if (a.t == T_INT && b.t == T_INT) { double d = (double)a.u.i - (double)b.u.i; if (d >= -2147483648.0 && d <= 2147483647.0) { PUSH(v2_int((s32)d)); break; } } PUSH(v2_num(v2_tonum(vm, a) - v2_tonum(vm, b))); break; }
        case 0xa2: { V2 b = POP(), a = POP(); if (a.t == T_INT && b.t == T_INT) { double d = (double)a.u.i * (double)b.u.i; if (d >= -2147483648.0 && d <= 2147483647.0 && !(d == 0 && (a.u.i < 0 || b.u.i < 0))) { PUSH(v2_int((s32)d)); break; } } PUSH(v2_num(v2_tonum(vm, a) * v2_tonum(vm, b))); break; }
        case 0xa3: { V2 b = POP(), a = POP(); PUSH(v2_num(v2_tonum(vm, a) / v2_tonum(vm, b))); break; }
        case 0xa4: { V2 b = POP(), a = POP(); double x = v2_tonum(vm, a), y = v2_tonum(vm, b); PUSH(v2_num(fmod(x, y))); break; }
        case 0xa5: { V2 b = POP(), a = POP(); PUSH(v2_int((s32)((u32)v2_toint(vm, a) << (v2_touint(vm, b) & 31)))); break; }
        case 0xa6: { V2 b = POP(), a = POP(); PUSH(v2_int(v2_toint(vm, a) >> (v2_touint(vm, b) & 31))); break; }
        case 0xa7: { V2 b = POP(), a = POP(); PUSH(v2_uint(v2_touint(vm, a) >> (v2_touint(vm, b) & 31))); break; }
        case 0xa8: { V2 b = POP(), a = POP(); PUSH(v2_int(v2_toint(vm, a) & v2_toint(vm, b))); break; }
        case 0xa9: { V2 b = POP(), a = POP(); PUSH(v2_int(v2_toint(vm, a) | v2_toint(vm, b))); break; }
        case 0xaa: { V2 b = POP(), a = POP(); PUSH(v2_int(v2_toint(vm, a) ^ v2_toint(vm, b))); break; }
        case 0xab: { V2 b = POP(), a = POP(); PUSH(v2_bool(v2_eq(vm, a, b))); break; }
        case 0xac: { V2 b = POP(), a = POP(); PUSH(v2_bool(v2_strict_eq(vm, a, b))); break; }
        case 0xad: { V2 b = POP(), a = POP(); PUSH(v2_bool(less_than(vm, a, b) == 1)); break; }
        case 0xae: { V2 b = POP(), a = POP(); PUSH(v2_bool(less_than(vm, b, a) == 0)); break; }
        case 0xaf: { V2 b = POP(), a = POP(); PUSH(v2_bool(less_than(vm, b, a) == 1)); break; }
        case 0xb0: { V2 b = POP(), a = POP(); PUSH(v2_bool(less_than(vm, a, b) == 0)); break; }
        case 0xb1: { /* instanceof: prototype chain */
            V2 tv = POP(), v = POP(); int r = 0;
            if (v.t == T_UNDEF) { throw_null_ref(vm, v); break; }
            if (tv.t != T_OBJ || (tv.u.o->kind != OK_CLASS && tv.u.o->kind != OK_FUNCTION)) { a2_throw_id(vm, "TypeError", 1040, NULL, NULL); break; }
            if (tv.u.o->kind == OK_CLASS && v.t == T_OBJ && !(v.u.o->cls && v.u.o->cls->prototype == v.u.o) && v.u.o != (vm->c_function ? vm->c_function->prototype : NULL)) { A2Class *vc = v2_class_of(vm, v), *tc = tv.u.o->as_class; for (; vc; vc = vc->super) if (vc == tc) { r = 1; break; } }
            if (v.t == T_OBJ) {
                A2Obj *proto = NULL; V2 pv; A2Obj *o;
                if (tv.u.o->kind == OK_CLASS) proto = tv.u.o->as_class->prototype; else { pv = a2_get_s(vm, tv, "prototype"); if (pv.t == T_OBJ) proto = pv.u.o; }
                for (o = v.u.o->proto; o && proto; o = o->proto) if (o == proto) { r = 1; break; }
                if (tv.u.o->kind == OK_CLASS && v.u.o->cls == tv.u.o->as_class && v.u.o->cls->prototype != v.u.o && v.u.o != (vm->c_function ? vm->c_function->prototype : NULL)) r = 1;
            } else if (v.t != T_NULL && v.t != T_UNDEF) {
                /* primitives: the prototype chain of Number / String / Boolean (3 instanceof int is false) */
                A2Class *pc = (v.t == T_STR) ? vm->c_string : (v.t == T_BOOL) ? vm->c_boolean : vm->c_number;
                A2Obj *proto = NULL, *o; V2 pv;
                if (tv.u.o->kind == OK_CLASS) proto = tv.u.o->as_class->prototype; else { pv = a2_get_s(vm, tv, "prototype"); if (pv.t == T_OBJ) proto = pv.u.o; }
                for (o = pc ? pc->prototype : NULL; o && proto; o = o->proto) if (o == proto) { r = 1; break; }
            }
            PUSH(v2_bool(r)); break; }
        case 0xb2: { u32 mi = RD30(); A2Class *t = rt_type(rt, mi); V2 v = POP(); PUSH(v2_bool(t && a2_instanceof(vm, v, t))); break; }
        case 0xb3: { V2 tv = POP(), v = POP(); A2Class *t = (tv.t == T_OBJ && tv.u.o->kind == OK_CLASS) ? tv.u.o->as_class : NULL; if (!t) { if (tv.t == T_NULL || tv.t == T_UNDEF) throw_null_ref(vm, tv); else a2_throw_id(vm, "TypeError", 1041, NULL, NULL); break; } PUSH(v2_bool((t == vm->c_object && v.t != T_UNDEF && v.t != T_NULL) || a2_instanceof(vm, v, t))); break; }
        case 0xb4: { V2 obj = POP(), key = POP(); if (obj.t == T_UNDEF || obj.t == T_NULL) { throw_null_ref(vm, obj); break; }
            if (obj.t == T_OBJ && obj.u.o->kind == OK_DICT) { extern int a2_dict_get(VM2 *vm, A2Obj *o, V2 k, V2 *out); V2 tmp; PUSH(v2_bool(a2_dict_get(vm, obj.u.o, key, &tmp))); break; }
            nm.name = v2_tostr(vm, key); nm.nss = &vm->ns_public; nm.nns = 1; nm.any_ns = 0; nm.attr = 0; PUSH(v2_bool(a2_has(vm, obj, &nm))); break; }
        case 0xc0: { V2 v = POP(); PUSH(v2_int(v2_toint(vm, v) + 1)); break; }
        case 0xc1: { V2 v = POP(); PUSH(v2_int(v2_toint(vm, v) - 1)); break; }
        case 0xc2: { u32 r = RD30(); if ((int)r < nloc) locals[r] = v2_int(v2_toint(vm, locals[r]) + 1); break; }
        case 0xc3: { u32 r = RD30(); if ((int)r < nloc) locals[r] = v2_int(v2_toint(vm, locals[r]) - 1); break; }
        case 0xc4: { V2 v = POP(); PUSH(v2_int((s32)(0u - (u32)v2_toint(vm, v)))); break; }
        case 0xc5: { V2 b = POP(), a = POP(); PUSH(v2_int((s32)((u32)v2_toint(vm, a) + (u32)v2_toint(vm, b)))); break; }
        case 0xc6: { V2 b = POP(), a = POP(); PUSH(v2_int((s32)((u32)v2_toint(vm, a) - (u32)v2_toint(vm, b)))); break; }
        case 0xc7: { V2 b = POP(), a = POP(); PUSH(v2_int((s32)((u32)v2_toint(vm, a) * (u32)v2_toint(vm, b)))); break; }
        case 0xd0: case 0xd1: case 0xd2: case 0xd3: PUSH(locals[op - 0xd0]); break;
        case 0xd4: case 0xd5: case 0xd6: case 0xd7: { V2 v = POP(); if (op - 0xd4 < nloc) locals[op - 0xd4] = v; break; }
        default:
            { char b[80]; sprintf(b, "Unknown opcode 0x%02x in %.120s", op, m->name->s); a2_throw_error(vm, "VerifyError", b, 1011); }
            break;
        }
        pc = (u32)(p - code);
        if (sp < 0) sp = 0;
    }
done:
    vm->cur = fr.caller;
    vm->dxns = fr.saved_dxns;
    vm->scope_sp = scope_base;
    vm->sp = (int)(locals - vm->stack);
    vm->depth--;
    return ret;
}

/* ---------------------------------------------------------------- */
/* trait map insert (shared with avm2_obj.c's static helper)        */

void a2_tm_put(A2TraitMap *tm, A2Trait *t) {
    extern void a2_tm_put_impl(A2TraitMap *tm, A2Trait *t);
    a2_tm_put_impl(tm, t);
}

/* GC roots held by the per-ABC runtime tables */
void a2_rt_mark_all(VM2 *vm) {
    int k; u32 i;
    for (k = 0; k < vm->nabcs; k++) {
        AbcFile *a = vm->abcs[k]; AbcRT *rt = RT(a);
        if (!rt) continue;
        for (i = 0; i < a->class_count; i++) if (rt->classes[i]) a2_mark_class(vm, rt->classes[i]);
        for (i = 0; i < a->body_count; i++) if (rt->bodycls[i]) a2_mark_class(vm, rt->bodycls[i]);
        for (i = 0; i < a->script_count; i++) { if (rt->globals[i]) a2_mark_obj(vm, rt->globals[i]); if (rt->gcls[i]) a2_mark_class(vm, rt->gcls[i]); }
    }
}
void a2_rt_free(AbcFile *a) {
    AbcRT *rt = RT(a); u32 i;
    if (!rt) return;
    for (i = 0; i < a->mn_count; i++) if (rt->names[i].nns > 1 && rt->names[i].kind != MN_TYPENAME) free(rt->names[i].nss);
    for (i = 0; i < a->method_count; i++) free(rt->methods[i]);
    for (i = 0; i < (u32)rt->nextra; i++) free(rt->extra[i]);
    free(rt->extra);
    free(rt->nss); free(rt->names); free(rt->strs); free(rt->methods); free(rt->classes); free(rt->bodycls); free(rt->catchcls); free(rt->globals); free(rt->gcls); free(rt->gstate);
    free(rt); a->rt = NULL;
}
