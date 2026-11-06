/* FLASHNT - AVM2 describeType / describeTypeJSON (port of avmplus TypeDescriber) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "avm2_int.h"
#include "abc.h"

#define ARG(i) a2_arg(argc, argv, i)

enum {
    DT_HIDE_NSURI = 0x001, DT_BASES = 0x002, DT_IFACES = 0x004, DT_VARS = 0x008, DT_ACC = 0x010,
    DT_METHODS = 0x020, DT_META = 0x040, DT_CTOR = 0x080, DT_TRAITS = 0x100, DT_ITRAITS = 0x200, DT_HIDE_OBJECT = 0x400
};
#define DT_FLASH10 (DT_BASES | DT_IFACES | DT_VARS | DT_ACC | DT_METHODS | DT_META | DT_CTOR | DT_TRAITS | DT_HIDE_NSURI | DT_HIDE_OBJECT)

extern A2Str *a2_rt_type_name(VM2 *vm, AbcFile *abc, u32 mn);
extern A2Class *a2_resolve_trait_type(VM2 *vm, A2Trait *t);
extern void a2_arr_push(VM2 *vm, A2Obj *a, V2 v);
extern V2 a2_xml_from_string(VM2 *vm, A2Str *s);
extern void a2_global_const(VM2 *vm, const char *name, V2 v);
extern void a2_tm_put(A2TraitMap *tm, A2Trait *t);

/* ---- helpers ---------------------------------------------------- */
static void setp(VM2 *vm, A2Obj *o, const char *k, V2 v) { a2_dyn_set(vm, o, a2_str(vm, k), v); }
static V2 sv(VM2 *vm, const char *s) { return s ? v2_cstr(vm, s) : v2_null(); }
static V2 ov(A2Obj *o) { return o ? v2_obj(o) : v2_null(); }

static A2Str *cname(VM2 *vm, A2Class *c) { return c ? a2_class_name(vm, c, 1) : a2_str(vm, "*"); }

/* parse a native signature "ret(p1,p2=,...)" -> ret / params; '=' suffix = optional */
typedef struct { char ret[96]; char par[16][96]; int opt[16]; int n; } Sig;
static int parse_sig(const char *s, Sig *g) {
    const char *p; int k = 0;
    memset(g, 0, sizeof(*g));
    if (!s) return 0;
    p = strchr(s, '(');
    if (!p) { strncpy(g->ret, s, 95); return 1; }
    { size_t n = (size_t)(p - s); if (n > 95) n = 95; memcpy(g->ret, s, n); g->ret[n] = 0; }
    p++;
    while (*p && *p != ')' && k < 16) {
        const char *e = p; size_t n;
        while (*e && *e != ',' && *e != ')') e++;
        n = (size_t)(e - p);
        if (n && p[n - 1] == '=') { g->opt[k] = 1; n--; }
        if (n > 95) n = 95; memcpy(g->par[k], p, n); g->par[k][n] = 0;
        if (!g->par[k][0]) strcpy(g->par[k], "*");
        k++;
        p = *e == ',' ? e + 1 : e;
    }
    g->n = k;
    if (!g->ret[0]) strcpy(g->ret, "*");
    return 1;
}

/* method signature access */
static AbcMethod *abc_method(A2Method *m) { return (m && m->abc && m->index < m->abc->method_count) ? &m->abc->methods[m->index] : NULL; }
static A2Str *m_return_type(VM2 *vm, A2Method *m) {
    AbcMethod *am = abc_method(m); Sig g;
    if (am) return a2_rt_type_name(vm, m->abc, am->return_type);
    if (m && m->sig && parse_sig(m->sig, &g)) return a2_str(vm, g.ret);
    return a2_str(vm, "*");
}
static A2Obj *describe_params(VM2 *vm, A2Method *m) {
    A2Obj *a = a2_new_array(vm); AbcMethod *am = abc_method(m); int i;
    if (am) {
        int req = (int)am->param_count - (int)am->option_count;
        for (i = 0; i < (int)am->param_count; i++) {
            A2Obj *v = a2_new_plain(vm);
            setp(vm, v, "type", v2_str(a2_rt_type_name(vm, m->abc, am->param_types ? am->param_types[i] : 0)));
            setp(vm, v, "optional", v2_bool(i >= req));
            a2_arr_push(vm, a, v2_obj(v));
        }
    } else if (m) {
        Sig g;
        if (m->sig && parse_sig(m->sig, &g)) {
            for (i = 0; i < g.n; i++) { A2Obj *v = a2_new_plain(vm); setp(vm, v, "type", sv(vm, g.par[i])); setp(vm, v, "optional", v2_bool(g.opt[i])); a2_arr_push(vm, a, v2_obj(v)); }
        } else {
            for (i = 0; i < m->nparams; i++) { A2Obj *v = a2_new_plain(vm); setp(vm, v, "type", sv(vm, "*")); setp(vm, v, "optional", v2_bool(0)); a2_arr_push(vm, a, v2_obj(v)); }
        }
    }
    return a;
}
static A2Str *accessor_type(VM2 *vm, A2Trait *t) {
    A2Method *m = t->getter ? t->getter : t->setter; AbcMethod *am = abc_method(m); Sig g;
    if (!m) return a2_str(vm, "*");
    if (am) {
        if (t->getter) return a2_rt_type_name(vm, m->abc, am->return_type);
        return am->param_count >= 1 && am->param_types ? a2_rt_type_name(vm, m->abc, am->param_types[0]) : a2_str(vm, "*");
    }
    if (m->sig && parse_sig(m->sig, &g)) { if (t->getter) return a2_str(vm, g.ret); return g.n ? a2_str(vm, g.par[0]) : a2_str(vm, "*"); }
    return a2_str(vm, "*");
}
static A2Str *slot_type(VM2 *vm, A2Trait *t, A2Class *holder, int is_class) {
    V2 d;
    if (t->abc) return a2_rt_type_name(vm, t->abc, t->type_mn);
    if (t->type) return cname(vm, t->type);
    d = t->has_def ? t->def : v2_undef();
    if (!t->has_def && is_class && holder && holder->obj && t->slot >= 0 && t->slot < holder->obj->nslots) d = holder->obj->slots[t->slot];
    {
        switch (d.t) { case T_INT: return a2_str(vm, "int"); case T_UINT: return a2_str(vm, "uint"); case T_NUM: return a2_str(vm, "Number"); case T_STR: return a2_str(vm, "String"); case T_BOOL: return a2_str(vm, "Boolean"); default: break; }
    }
    return a2_str(vm, "*");
}

/* ---- metadata --------------------------------------------------- */
static A2Str *pool_str(VM2 *vm, AbcFile *a, u32 i) { return i < a->string_count ? a2_intern(vm, a->strings[i], a->string_lens[i]) : vm->s_empty; }
static void add_metadata(VM2 *vm, A2Obj *arr, AbcFile *a, const AbcTrait *at) {
    u32 i, j;
    if (!a || !at || !at->metadata) return;
    for (i = 0; i < at->metadata_count; i++) {
        u32 mi = at->metadata[i]; AbcMetadata *m; A2Obj *o, *vals;
        if (mi >= a->metadata_count || !a->metadata) continue;
        m = &a->metadata[mi];
        o = a2_new_plain(vm); vals = a2_new_array(vm);
        for (j = 0; j < m->item_count; j++) {
            A2Obj *kv = a2_new_plain(vm);
            setp(vm, kv, "key", v2_str(pool_str(vm, a, m->keys[j])));
            setp(vm, kv, "value", v2_str(pool_str(vm, a, m->vals[j])));
            a2_arr_push(vm, vals, v2_obj(kv));
        }
        setp(vm, o, "name", v2_str(pool_str(vm, a, m->name)));
        setp(vm, o, "value", v2_obj(vals));
        a2_arr_push(vm, arr, v2_obj(o));
    }
}

/* ---- interfaces ------------------------------------------------- */
static void collect_ifaces(A2Class *c, A2Class **out, int *n, int cap) {
    int i, k; extern void a2_resolve_ifaces(A2Class *c);
    for (; c; c = c->super) {
        a2_resolve_ifaces(c);
        for (i = 0; i < c->nifaces; i++) {
            A2Class *f = c->ifaces[i]; int dup = 0;
            for (k = 0; k < *n; k++) if (out[k] == f) { dup = 1; break; }
            if (!dup && *n < cap) out[(*n)++] = f;
            if (!dup) collect_ifaces(f, out, n, cap);
        }
    }
}

/* ---- "builtin-ness" of a namespace (emulates versioned builtin namespaces) ---- */
static int ns_has_uri(const A2Ns *ns) { return ns->uri && ns->uri->len > 0; }
static int trait_is_builtin(A2Trait *t) { return t->abc == NULL; }
typedef struct { A2Str *uri; int builtin; } NsKey;
static int nskey_in(NsKey *ks, int n, A2Str *uri, int builtin) { int i; for (i = 0; i < n; i++) if (ks[i].uri == uri && ks[i].builtin == builtin) return 1; return 0; }

/* core ECMAScript classes whose native instance methods live in the AS3 namespace */
static int as3_native_class(VM2 *vm, A2Class *c) {
    return c == vm->c_object || c == vm->c_array || c == vm->c_string || c == vm->c_number || c == vm->c_int || c == vm->c_uint || c == vm->c_boolean ||
           c == vm->c_vector || c == vm->c_function || c == vm->c_xml || c == vm->c_xmllist || c == vm->c_date || c == vm->c_regexp ||
           (c && a2_is_subclass(c, vm->c_vector));
}
static int builtin_length_class(VM2 *vm, A2Class *c, int *len) {
    *len = 1;
    if (c == vm->c_object || c == vm->c_class || c == vm->c_function || c == vm->c_boolean || c == vm->c_number || c == vm->c_int || c == vm->c_uint ||
        c == vm->c_string || c == vm->c_array || c == vm->c_regexp || c == vm->c_xml || c == vm->c_xmllist) return 1;
    if (c == vm->c_date) { *len = 7; return 1; }
    return 0;
}

/* effective namespace of a trait for description purposes */
static void trait_ns(VM2 *vm, A2Trait *t, A2Class *holder, A2Ns *out) {
    *out = t->ns;
    if (trait_is_builtin(t) && (t->kind == TK_METHOD) && as3_native_class(vm, holder) && out->kind == NS_PACKAGE && out->uri->len == 0) *out = vm->ns_as3;
}
static int ns_is_public_kind(const A2Ns *ns) { return ns->kind == NS_PACKAGE || ns->kind == NS_NAMESPACE; }

/* declaring class of a trait */
static A2Class *trait_owner(A2Trait *t, A2Class *holder) {
    if (t->kind == TK_METHOD && t->method && t->method->owner) return t->method->owner;
    if ((t->kind == TK_GETTER || t->kind == TK_SETTER)) { A2Method *m = t->getter ? t->getter : t->setter; if (m && m->owner) return m->owner; }
    return holder;
}

/* ---- describeTraits --------------------------------------------- */
typedef struct { A2Str *name; A2Ns ns; A2Trait *t; A2Class *holder; } Bind;

static int bind_find(Bind *b, int n, A2Str *name, const A2Ns *ns) {
    int i; extern int a2_ns_eq(const A2Ns *a, const A2Ns *b);
    for (i = 0; i < n; i++) if (b[i].name == name && a2_ns_eq(&b[i].ns, ns)) return i;
    return -1;
}

static A2Obj *describe_traits(VM2 *vm, A2Class *c, int is_class, u32 flags) {
    A2Obj *o, *bases = NULL, *metadata = NULL, *ifaces = NULL, *methods = NULL, *accessors = NULL, *variables = NULL, *ctor = NULL;
    A2Class *allif[64]; int nif = 0;
    if (!(flags & DT_TRAITS)) return NULL;
    o = a2_new_plain(vm);
    if (flags & DT_META) {
        metadata = a2_new_array(vm);
        if (!is_class && c && c->abc && c->inst) {
            /* class-level metadata is attached to the class trait in the script */
            u32 si, ti; AbcFile *a = c->abc;
            for (si = 0; si < a->script_count; si++) for (ti = 0; ti < a->scripts[si].trait_count; ti++) {
                AbcTrait *at = &a->scripts[si].traits[ti];
                if (at->kind == TRAIT_CLASS && at->classi == c->cindex) add_metadata(vm, metadata, a, at);
            }
        }
    }
    if (flags & DT_BASES) {
        bases = a2_new_array(vm);
        if (is_class) { a2_arr_push(vm, bases, v2_str(cname(vm, vm->c_class))); a2_arr_push(vm, bases, v2_str(cname(vm, vm->c_object))); }
        else { A2Class *b; for (b = c ? c->super : NULL; b; b = b->super) a2_arr_push(vm, bases, v2_str(cname(vm, b))); }
    }
    if (c && !is_class) collect_ifaces(c, allif, &nif, 64);
    if (flags & DT_IFACES) {
        int i; ifaces = a2_new_array(vm);
        for (i = 0; i < nif; i++) a2_arr_push(vm, ifaces, v2_str(cname(vm, allif[i])));
    }
    if ((flags & DT_CTOR) && c && !is_class) {
        if (c->iinit && !c->iinit->native) { AbcMethod *am = abc_method(c->iinit); if (am && am->param_count > 0) ctor = describe_params(vm, c->iinit); }
        else if (c->ctor_sig) { Sig g; if (parse_sig(c->ctor_sig, &g) && g.n > 0) { A2Method tmp; memset(&tmp, 0, sizeof(tmp)); tmp.sig = c->ctor_sig; ctor = describe_params(vm, &tmp); } }
    }
    if ((flags & (DT_ACC | DT_METHODS | DT_VARS)) && c) {
        Bind *binds = NULL; int nb = 0, cap = 0, i;
        NsKey removal[64]; int nrem = 0;
        A2Trait *t;
#define ADD_BIND(tr, hc) do { A2Ns ns_; int bi_; trait_ns(vm, (tr), (hc), &ns_); bi_ = bind_find(binds, nb, (tr)->name, &ns_); \
            if (bi_ >= 0) { binds[bi_].t = (tr); binds[bi_].holder = (hc); } \
            else { if (nb == cap) { cap = cap ? cap * 2 : 64; binds = (Bind*)realloc(binds, (size_t)cap * sizeof(Bind)); } \
                   binds[nb].name = (tr)->name; binds[nb].ns = ns_; binds[nb].t = (tr); binds[nb].holder = (hc); nb++; } } while (0)
        if (is_class) {
            /* class traits: own statics + Class's instance traits (prototype); Object's hidden only with HIDE_OBJECT */
            if (!(flags & DT_HIDE_OBJECT)) for (t = vm->c_object->itraits.first; t; t = t->onext) ADD_BIND(t, vm->c_object);
            { static A2Trait proto_t; static int proto_init = 0;
              if (!proto_init) { memset(&proto_t, 0, sizeof(proto_t)); proto_t.name = a2_str(vm, "prototype"); proto_t.ns = vm->ns_public; proto_t.kind = TK_GETTER; proto_init = 1; }
              ADD_BIND(&proto_t, vm->c_class); }
            { int len; static A2Trait len_t; if (builtin_length_class(vm, c, &len)) { memset(&len_t, 0, sizeof(len_t)); len_t.name = vm->s_length; len_t.ns = vm->ns_public; len_t.kind = TK_CONST; len_t.def = v2_int(len); len_t.has_def = 1; ADD_BIND(&len_t, c); } }
            for (t = c->ctraits.first; t; t = t->onext) ADD_BIND(t, c);
        } else {
            if (c == vm->c_class) { static A2Trait proto_i; memset(&proto_i, 0, sizeof(proto_i)); proto_i.name = a2_str(vm, "prototype"); proto_i.ns = vm->ns_public; proto_i.kind = TK_GETTER; ADD_BIND(&proto_i, vm->c_class); }
            for (t = c->itraits.first; t; t = t->onext) {
                A2Class *own = trait_owner(t, c);
                if ((flags & DT_HIDE_OBJECT) && own == vm->c_object && !(c->flags & CF_INTERFACE)) continue;
                ADD_BIND(t, c);
            }
            /* wipe interface bindings */
            for (i = 0; i < nif; i++) for (t = allif[i]->itraits.first; t; t = t->onext) { int bi = bind_find(binds, nb, t->name, &t->ns); if (bi >= 0) binds[bi].t = NULL; }
            /* FP9/10 bug: namespaces with a URI used by any base class hide members in that namespace */
            if (flags & DT_HIDE_NSURI) {
                A2Class *b;
                for (b = c->super; b; b = b->super) for (t = b->itraits.first; t; t = t->onext) {
                    A2Ns ns; trait_ns(vm, t, b, &ns);
                    if (ns_has_uri(&ns) && !nskey_in(removal, nrem, ns.uri, trait_is_builtin(t)) && nrem < 64) { removal[nrem].uri = ns.uri; removal[nrem].builtin = trait_is_builtin(t); nrem++; }
                }
            }
        }
        for (i = 0; i < nb; i++) {
            A2Trait *bt = binds[i].t; A2Ns ns = binds[i].ns; A2Obj *v; A2Obj *md = NULL; A2Class *holder = binds[i].holder;
            const AbcTrait *md1 = NULL, *md2 = NULL; AbcFile *md1a = NULL, *md2a = NULL;
            if (!bt) continue;
            if (!ns_is_public_kind(&ns)) continue;
            if ((flags & DT_HIDE_NSURI) && ns_has_uri(&ns) && nskey_in(removal, nrem, ns.uri, trait_is_builtin(bt))) continue;
            switch (bt->kind) {
            case TK_SLOT: case TK_CONST:
                if (!(flags & DT_VARS)) continue;
                if (bt->final_ == 2) continue;   /* function trait */
                v = a2_new_plain(vm);
                setp(vm, v, "access", sv(vm, bt->kind == TK_CONST ? "readonly" : "readwrite"));
                setp(vm, v, "type", v2_str(slot_type(vm, bt, holder, is_class)));
                if (!variables) variables = a2_new_array(vm);
                a2_arr_push(vm, variables, v2_obj(v));
                md1 = bt->at; md1a = bt->abc;
                break;
            case TK_METHOD:
                if (!(flags & DT_METHODS)) continue;
                v = a2_new_plain(vm);
                setp(vm, v, "declaredBy", v2_str(cname(vm, trait_owner(bt, holder))));
                setp(vm, v, "returnType", v2_str(m_return_type(vm, bt->method)));
                setp(vm, v, "parameters", v2_obj(describe_params(vm, bt->method)));
                if (!methods) methods = a2_new_array(vm);
                a2_arr_push(vm, methods, v2_obj(v));
                md1 = bt->at; md1a = bt->abc;
                break;
            case TK_GETTER: case TK_SETTER:
                if (!(flags & DT_ACC)) continue;
                v = a2_new_plain(vm);
                setp(vm, v, "declaredBy", v2_str(cname(vm, trait_owner(bt, holder))));
                setp(vm, v, "access", sv(vm, bt->getter && bt->setter ? "readwrite" : bt->getter ? "readonly" : bt->setter ? "writeonly" : "readonly"));
                setp(vm, v, "type", v2_str(accessor_type(vm, bt)));
                if (!accessors) accessors = a2_new_array(vm);
                a2_arr_push(vm, accessors, v2_obj(v));
                if (bt->getter) { md1 = bt->at; md1a = bt->abc; }
                if (bt->setter) { md2 = bt->at2; md2a = bt->abc2; }
                break;
            default: continue;
            }
            if ((flags & DT_META) && ((md1 && md1->metadata_count) || (md2 && md2->metadata_count))) {
                md = a2_new_array(vm);
                add_metadata(vm, md, md1a, md1); add_metadata(vm, md, md2a, md2);
            }
            setp(vm, v, "name", v2_str(bt->name));
            setp(vm, v, "uri", ns_has_uri(&ns) ? v2_str(ns.uri) : v2_null());
            setp(vm, v, "metadata", ov(md));
        }
        free(binds);
#undef ADD_BIND
    }
    setp(vm, o, "bases", ov(bases)); setp(vm, o, "interfaces", ov(ifaces)); setp(vm, o, "metadata", ov(metadata));
    setp(vm, o, "accessors", ov(accessors)); setp(vm, o, "methods", ov(methods)); setp(vm, o, "variables", ov(variables)); setp(vm, o, "constructor", ov(ctor));
    return o;
}

/* ---- describeTypeJSON ------------------------------------------- */
V2 a2_describe_type_json(VM2 *vm, V2 value, u32 flags) {
    A2Class *c; int is_class; A2Obj *o; const char *special = NULL;
    if (value.t == T_UNDEF) { c = NULL; special = "void"; is_class = 0; }
    else if (value.t == T_NULL) { c = NULL; special = "null"; is_class = 0; }
    else if (value.t == T_OBJ && value.u.o->kind == OK_CLASS) { c = value.u.o->as_class; is_class = 1; }
    else { c = v2_class_of(vm, value); is_class = 0; }
    if (flags & DT_ITRAITS) { if (!is_class) return v2_null(); is_class = 0; }
    o = a2_new_plain(vm);
    setp(vm, o, "name", special ? sv(vm, special) : v2_str(cname(vm, c)));
    setp(vm, o, "isDynamic", v2_bool(is_class ? 1 : (c ? c->is_dynamic : 0)));
    setp(vm, o, "isFinal", v2_bool(is_class ? 1 : (c ? !!(c->flags & CF_FINAL) : 1)));
    setp(vm, o, "isStatic", v2_bool(is_class));
    setp(vm, o, "traits", special ? v2_null() : ov(describe_traits(vm, c, is_class, flags)));
    return v2_obj(o);
}

/* ---- XML builder (mirrors DescribeType.as) ---------------------- */
typedef struct { char *buf; size_t n, cap; } XB;
static void xb_put(XB *b, const char *s) { size_t l = strlen(s); if (b->n + l + 1 > b->cap) { b->cap = (b->n + l + 1) * 2; b->buf = (char*)realloc(b->buf, b->cap); } memcpy(b->buf + b->n, s, l + 1); b->n += l; }
static void xb_attr_esc(XB *b, const char *s) {
    for (; *s; s++) { switch (*s) { case '&': xb_put(b, "&amp;"); break; case '<': xb_put(b, "&lt;"); break; case '>': xb_put(b, "&gt;"); break; case '"': xb_put(b, "&quot;"); break; default: { char t[2]; t[0] = *s; t[1] = 0; xb_put(b, t); } } }
}
static V2 getp(VM2 *vm, V2 o, const char *k) { V2 r; A2Name n; n.name = a2_str(vm, k); n.nss = &vm->ns_public; n.nns = 1; n.any_ns = 0; n.attr = 0; if (o.t != T_OBJ || !a2_get(vm, o, &n, &r)) return v2_undef(); return r; }
static void xb_attr(XB *b, VM2 *vm, const char *k, V2 v) { xb_put(b, " "); xb_put(b, k); xb_put(b, "=\""); xb_attr_esc(b, v2_tostr(vm, v)->s); xb_put(b, "\""); }
static int arr_len(V2 a) { return (a.t == T_OBJ && (a.u.o->kind == OK_ARRAY)) ? a.u.o->alen : 0; }
static V2 arr_at(V2 a, int i) { return a.u.o->arr[i]; }

static void x_params(XB *b, VM2 *vm, V2 params) {
    int i, n = arr_len(params);
    for (i = 0; i < n; i++) { V2 p = arr_at(params, i); xb_put(b, "<parameter"); xb_attr(b, vm, "index", v2_int(i + 1)); xb_attr(b, vm, "type", getp(vm, p, "type")); xb_attr(b, vm, "optional", getp(vm, p, "optional")); xb_put(b, "/>"); }
}
static void x_metadata(XB *b, VM2 *vm, V2 md) {
    int i, j, n = arr_len(md);
    for (i = 0; i < n; i++) {
        V2 m = arr_at(md, i), vals = getp(vm, m, "value"); int nv = arr_len(vals);
        xb_put(b, "<metadata"); xb_attr(b, vm, "name", getp(vm, m, "name"));
        if (!nv) { xb_put(b, "/>"); continue; }
        xb_put(b, ">");
        for (j = 0; j < nv; j++) { V2 a = arr_at(vals, j); xb_put(b, "<arg"); xb_attr(b, vm, "key", getp(vm, a, "key")); xb_attr(b, vm, "value", getp(vm, a, "value")); xb_put(b, "/>"); }
        xb_put(b, "</metadata>");
    }
}
static void x_finish(XB *b, VM2 *vm, V2 e, const char *tag) {
    V2 uri = getp(vm, e, "uri"), md = getp(vm, e, "metadata");
    if (uri.t != T_NULL && uri.t != T_UNDEF) xb_attr(b, vm, "uri", uri);
    if (arr_len(md)) { xb_put(b, ">"); x_metadata(b, vm, md); xb_put(b, "</"); xb_put(b, tag); xb_put(b, ">"); }
    else xb_put(b, "/>");
}
static void x_traits(XB *b, VM2 *vm, V2 tr) {
    int i, n; V2 a;
    a = getp(vm, tr, "bases"); n = arr_len(a);
    for (i = 0; i < n; i++) { xb_put(b, "<extendsClass"); xb_attr(b, vm, "type", arr_at(a, i)); xb_put(b, "/>"); }
    a = getp(vm, tr, "interfaces"); n = arr_len(a);
    for (i = 0; i < n; i++) { xb_put(b, "<implementsInterface"); xb_attr(b, vm, "type", arr_at(a, i)); xb_put(b, "/>"); }
    a = getp(vm, tr, "constructor");
    if (a.t == T_OBJ) { xb_put(b, "<constructor>"); x_params(b, vm, a); xb_put(b, "</constructor>"); }
    a = getp(vm, tr, "variables"); n = arr_len(a);
    for (i = 0; i < n; i++) { V2 v = arr_at(a, i); const char *tag = !strcmp(v2_tostr(vm, getp(vm, v, "access"))->s, "readonly") ? "constant" : "variable";
        xb_put(b, "<"); xb_put(b, tag); xb_attr(b, vm, "name", getp(vm, v, "name")); xb_attr(b, vm, "type", getp(vm, v, "type")); x_finish(b, vm, v, tag); }
    a = getp(vm, tr, "accessors"); n = arr_len(a);
    for (i = 0; i < n; i++) { V2 v = arr_at(a, i);
        xb_put(b, "<accessor"); xb_attr(b, vm, "name", getp(vm, v, "name")); xb_attr(b, vm, "access", getp(vm, v, "access")); xb_attr(b, vm, "type", getp(vm, v, "type")); xb_attr(b, vm, "declaredBy", getp(vm, v, "declaredBy")); x_finish(b, vm, v, "accessor"); }
    a = getp(vm, tr, "methods"); n = arr_len(a);
    for (i = 0; i < n; i++) { V2 v = arr_at(a, i), params = getp(vm, v, "parameters"), uri = getp(vm, v, "uri"), md = getp(vm, v, "metadata");
        xb_put(b, "<method"); xb_attr(b, vm, "name", getp(vm, v, "name")); xb_attr(b, vm, "declaredBy", getp(vm, v, "declaredBy")); xb_attr(b, vm, "returnType", getp(vm, v, "returnType"));
        if (uri.t != T_NULL && uri.t != T_UNDEF) xb_attr(b, vm, "uri", uri);
        if (arr_len(params) || arr_len(md)) { xb_put(b, ">"); x_params(b, vm, params); x_metadata(b, vm, md); xb_put(b, "</method>"); } else xb_put(b, "/>"); }
    x_metadata(b, vm, getp(vm, tr, "metadata"));
}

V2 a2_describe_type_flags(VM2 *vm, V2 value, u32 flags) {
    V2 o = a2_describe_type_json(vm, value, flags), oi, tr, bases; XB b; V2 r;
    if (o.t != T_OBJ) return v2_null();
    b.cap = 1024; b.n = 0; b.buf = (char*)malloc(b.cap); b.buf[0] = 0;
    xb_put(&b, "<type"); xb_attr(&b, vm, "name", getp(vm, o, "name"));
    tr = getp(vm, o, "traits"); bases = getp(vm, tr, "bases");
    if (arr_len(bases)) xb_attr(&b, vm, "base", arr_at(bases, 0));
    xb_attr(&b, vm, "isDynamic", getp(vm, o, "isDynamic")); xb_attr(&b, vm, "isFinal", getp(vm, o, "isFinal")); xb_attr(&b, vm, "isStatic", getp(vm, o, "isStatic"));
    xb_put(&b, ">");
    if (tr.t == T_OBJ) x_traits(&b, vm, tr);
    oi = a2_describe_type_json(vm, value, flags | DT_ITRAITS);
    if (oi.t == T_OBJ) {
        V2 tri = getp(vm, oi, "traits");
        xb_put(&b, "<factory"); xb_attr(&b, vm, "type", getp(vm, oi, "name")); xb_put(&b, ">");
        if (tri.t == T_OBJ) x_traits(&b, vm, tri);
        xb_put(&b, "</factory>");
    }
    xb_put(&b, "</type>");
    r = a2_xml_from_string(vm, a2_intern(vm, b.buf, b.n)); free(b.buf);
    return r;
}
V2 a2_describe_type(VM2 *vm, V2 v) { return a2_describe_type_flags(vm, v, DT_FLASH10); }

/* ---- avmplus package -------------------------------------------- */
static V2 ap_describeTypeJSON(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; return a2_describe_type_json(vm, ARG(0), (u32)v2_touint(vm, ARG(1))); }
static V2 ap_describeType(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; return a2_describe_type_flags(vm, ARG(0), (u32)v2_touint(vm, ARG(1))); }
static V2 ap_getQualifiedClassName(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 v = ARG(0); A2Class *c; (void)self;
    if (v.t == T_UNDEF) return v2_cstr(vm, "void"); if (v.t == T_NULL) return v2_cstr(vm, "null");
    if (v.t == T_OBJ && v.u.o->kind == OK_CLASS) return v2_str(a2_class_name(vm, v.u.o->as_class, 1));
    c = v2_class_of(vm, v); return v2_str(a2_class_name(vm, c, 1));
}
static V2 ap_getQualifiedSuperclassName(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 v = ARG(0); A2Class *c; (void)self;
    if (v.t == T_UNDEF || v.t == T_NULL) return v2_null();
    c = (v.t == T_OBJ && v.u.o->kind == OK_CLASS) ? v.u.o->as_class : v2_class_of(vm, v);
    if (!c || !c->super) return v2_null();
    return v2_str(a2_class_name(vm, c->super, 1));
}

void a2_describe_init(VM2 *vm) {
    A2Ns ns; A2Trait *t; int i;
    struct { const char *n; NativeFn2 f; int np; } fu[] = {
        { "describeTypeJSON", ap_describeTypeJSON, 2 }, { "describeType", ap_describeType, 2 },
        { "getQualifiedClassName", ap_getQualifiedClassName, 1 }, { "getQualifiedSuperclassName", ap_getQualifiedSuperclassName, 1 }, { NULL, NULL, 0 } };
    struct { const char *n; int v; } ku[] = {
        { "HIDE_NSURI_METHODS", DT_HIDE_NSURI }, { "INCLUDE_BASES", DT_BASES }, { "INCLUDE_INTERFACES", DT_IFACES }, { "INCLUDE_VARIABLES", DT_VARS },
        { "INCLUDE_ACCESSORS", DT_ACC }, { "INCLUDE_METHODS", DT_METHODS }, { "INCLUDE_METADATA", DT_META }, { "INCLUDE_CONSTRUCTOR", DT_CTOR },
        { "INCLUDE_TRAITS", DT_TRAITS }, { "USE_ITRAITS", DT_ITRAITS }, { "HIDE_OBJECT", DT_HIDE_OBJECT }, { "FLASH10_FLAGS", DT_FLASH10 }, { NULL, 0 } };
    ns.kind = NS_PACKAGE; ns.uri = a2_str(vm, "avmplus");
    for (i = 0; fu[i].n; i++) { t = (A2Trait*)calloc(1, sizeof(A2Trait)); t->name = a2_str(vm, fu[i].n); t->ns = ns; t->kind = TK_METHOD; t->method = a2_native_method(vm, fu[i].f, fu[i].n, fu[i].np); a2_tm_put(&vm->global->cls->itraits, t); }
    for (i = 0; ku[i].n; i++) {
        t = (A2Trait*)calloc(1, sizeof(A2Trait)); t->name = a2_str(vm, ku[i].n); t->ns = ns; t->kind = TK_CONST; t->slot = vm->global->cls->nislots++;
        t->def = v2_uint((u32)ku[i].v); t->has_def = 1;
        a2_tm_put(&vm->global->cls->itraits, t);
        { V2 *s = (V2*)realloc(vm->global->slots, (size_t)vm->global->cls->nislots * sizeof(V2)); if (s) { vm->global->slots = s; vm->global->nslots = vm->global->cls->nislots; vm->global->slots[t->slot] = t->def; } }
    }
}

/* ---- native signature registration ------------------------------ */
void a2_sig(VM2 *vm, A2Class *c, const char *name, const char *sig) {
    A2Trait *t = a2_find_trait_any(&c->itraits, a2_str(vm, name));
    if (!t) t = a2_find_trait_any(&c->ctraits, a2_str(vm, name));
    if (!t) return;
    if (t->method) t->method->sig = sig;
    if (t->getter) t->getter->sig = sig;
    if (t->setter) t->setter->sig = sig;
}
