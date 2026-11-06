/* avm1_amf.c - AMF0 for AVM1: SharedObject (.sol files) and the values
 * NetConnection / LocalConnection send.
 *
 * Serialization follows Ruffle's core/src/avm1/amf.rs and the flash-lso
 * writer it uses: properties in insertion order, getters written as
 * undefined, functions skipped, display objects as undefined, objects of
 * a class registered with Object.registerClass as typed objects, and
 * references counted the way the Flash Player counts them (every value
 * written takes a reference number, not just objects). */
#include "avm1_int.h"
#include "fetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DE  PA_DONT_ENUM
#define DD  PA_DONT_DELETE

extern AObj *lib_class(VM *vm, Env *env, NativeFn ctor, NativeFn call, AObj *super_proto, int proto_first, AObj **proto_out);
extern AStr *avm1_xml_string(Act *a, AObj *o);
extern const char *avm1_movie_url(Player *p, SWFMovie *m);
extern void avm1_run_actions(VM *vm);

/* ---------------------------------------------------------------- */
/* byte buffer                                                      */

typedef struct { u8 *d; size_t n, cap; int oom; } ABuf;

static void ab_put(ABuf *b, const void *p, size_t n) {
    if (b->oom) return;
    if (b->n + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256; u8 *nd;
        while (nc < b->n + n) nc *= 2;
        nd = (u8*)realloc(b->d, nc);
        if (!nd) { b->oom = 1; return; }
        b->d = nd; b->cap = nc;
    }
    memcpy(b->d + b->n, p, n); b->n += n;
}
static void ab_u8(ABuf *b, unsigned v) { u8 c = (u8)v; ab_put(b, &c, 1); }
static void ab_u16(ABuf *b, unsigned v) { u8 c[2]; c[0] = (u8)(v >> 8); c[1] = (u8)v; ab_put(b, c, 2); }
static void ab_u32(ABuf *b, u32 v) { u8 c[4]; c[0] = (u8)(v >> 24); c[1] = (u8)(v >> 16); c[2] = (u8)(v >> 8); c[3] = (u8)v; ab_put(b, c, 4); }
static void ab_f64(ABuf *b, double d) {
    u8 c[8], *s = (u8*)&d; int i;
    for (i = 0; i < 8; i++) c[i] = s[7 - i];   /* little-endian hosts */
    ab_put(b, c, 8);
}
static void ab_str16(ABuf *b, const char *s) { size_t n = strlen(s); ab_u16(b, (unsigned)(n & 0xFFFF)); ab_put(b, s, n & 0xFFFF); }

enum { M_NUM = 0, M_BOOL, M_STR, M_OBJ, M_MC, M_NULL, M_UNDEF, M_REF, M_ECMA, M_END, M_STRICT, M_DATE, M_LSTR, M_UNSUP, M_RSET, M_XML, M_TYPED, M_AMF3 };

/* ---------------------------------------------------------------- */
/* writer (flash-lso Amf0Writer, streamed)                          */

typedef struct { AObj **o; u16 *ref; int n, cap; u16 next; } RefCache;

typedef struct { Act *a; ABuf *b; RefCache *rc; } AW;

static int rc_find(RefCache *rc, AObj *o) { int i; for (i = 0; i < rc->n; i++) if (rc->o[i] == o) return rc->ref[i]; return -1; }
static u16 rc_add(RefCache *rc, AObj *o) {
    u16 r = rc->next++;
    if (rc->n == rc->cap) {
        int nc = rc->cap ? rc->cap * 2 : 16; AObj **no = (AObj**)realloc(rc->o, (size_t)nc * sizeof(AObj*)); u16 *nr;
        if (!no) return r; rc->o = no;
        nr = (u16*)realloc(rc->ref, (size_t)nc * sizeof(u16)); if (!nr) return r; rc->ref = nr;
        rc->cap = nc;
    }
    rc->o[rc->n] = o; rc->ref[rc->n] = r; rc->n++;
    return r;
}
static void rc_free(RefCache *rc) { free(rc->o); free(rc->ref); rc->o = NULL; rc->ref = NULL; rc->n = rc->cap = 0; }


/* Object.registerClass name of the object's constructor (amf.rs object_class_name) */
static AStr *class_name_of(Act *a, AObj *o) {
    VM *vm = a->vm; Value ctor = v_undef(); Env *e; int i, cs = a->swf_version >= 7;
    if (o_has_own_property(a, o, vm->s_constructor)) {
        if (o_has_own_virtual(a, o, vm->s_constructor)) return NULL;
        ctor = o_get(a, o, vm->s_constructor);
        if (vm->err) { vm->err = 0; return NULL; }
    } else {
        AObj *cur = o; int guard = 0;
        while (cur && guard++ < 256) {
            if (o_has_own_property(a, cur, vm->s___constructor__)) {
                if (o_has_own_virtual(a, cur, vm->s___constructor__)) return NULL;
                if (!o_get_local_stored(a, cur, vm->s___constructor__, &ctor)) return NULL;
                break;
            }
            { Value p = o_proto(a, cur); cur = p.t == V_OBJ ? p.u.o : NULL; }
        }
    }
    if (ctor.t != V_OBJ) return NULL;
    e = &vm->env[cs ? 1 : 0];
    for (i = e->registry.n - 1; i >= 0; i--) {
        Prop *p = &e->registry.items[i];
        if (p->key && p->v.t == V_OBJ && p->v.u.o == ctor.u.o) return p->key;
    }
    return NULL;
}

static void w_props(AW *w, AObj *o);

/* serialize_value_to_writer: one named element */
static void w_element(AW *w, AStr *name, Value v) {
    Act *a = w->a; VM *vm = a->vm; ABuf *b = w->b; const char *nm = str_c(vm, name);
    if (v.t == V_OBJ && !OBJ_IS_DISPLAY(v.u.o)) {
        AObj *o = v.u.o; int r;
        if (o_as_function(o)) return;                       /* functions are skipped */
        if (o->nk == NK_ARRAY) {
            int len;
            ab_str16(b, nm);
            if ((r = rc_find(w->rc, o)) >= 0) { ab_u8(b, M_REF); ab_u16(b, (unsigned)r); return; }
            rc_add(w->rc, o);
            len = o_length(a, o); if (vm->err) { vm->err = 0; len = 0; }
            ab_u8(b, M_ECMA); ab_u32(b, (u32)len);
            w_props(w, o);
            ab_u16(b, 0); ab_u8(b, M_END);
            return;
        }
        if (o->nk == NK_XML || o->nk == NK_XMLNODE) {
            AStr *s = avm1_xml_string(a, o); const char *u; size_t n;
            if (vm->err) vm->err = 0;
            u = s ? str_c(vm, s) : ""; n = strlen(u);
            ab_str16(b, nm); ab_u8(b, M_XML); ab_u32(b, (u32)n); ab_put(b, u, n);
            w->rc->next++;
            return;
        }
        if (o->nk == NK_DATE) {
            ab_str16(b, nm); ab_u8(b, M_DATE); ab_f64(b, o->nat.n); ab_u16(b, 0);
            w->rc->next++;
            return;
        }
        {
            AStr *cls = class_name_of(a, o);
            ab_str16(b, nm);
            if ((r = rc_find(w->rc, o)) >= 0) { ab_u8(b, M_REF); ab_u16(b, (unsigned)r); return; }
            rc_add(w->rc, o);
            if (cls) { ab_u8(b, M_TYPED); ab_str16(b, str_c(vm, cls)); }
            else ab_u8(b, M_OBJ);
            w_props(w, o);
            ab_u16(b, 0); ab_u8(b, M_END);
        }
        return;
    }
    ab_str16(b, nm);
    switch (v.t) {
    case V_NUM: ab_u8(b, M_NUM); ab_f64(b, v.u.n); break;
    case V_STR: {
        const char *u = str_c(vm, v.u.s); size_t n = strlen(u);
        if (n > 65535) { ab_u8(b, M_LSTR); ab_u32(b, (u32)n); ab_put(b, u, n); }
        else { ab_u8(b, M_STR); ab_u16(b, (unsigned)n); ab_put(b, u, n); }
        break;
    }
    case V_NULL: ab_u8(b, M_NULL); break;
    case V_BOOL: ab_u8(b, M_BOOL); ab_u8(b, v.u.b ? 1 : 0); break;
    default: ab_u8(b, M_UNDEF); break;                 /* undefined, display objects */
    }
    w->rc->next++;
}

/* recursive_serialize: the properties in insertion order */
static void w_props(AW *w, AObj *o) {
    Act *a = w->a; VM *vm = a->vm; int n = 0, i; AStr **keys = o_keys(a, o, 0, &n);
    if (vm->err) { vm->err = 0; free(keys); return; }
    for (i = n - 1; keys && i >= 0; i--) {
        Value v; AObj *cur = o; int virt = 0, guard = 0;
        while (cur && guard++ < 256) {
            if (o_has_own_property(a, cur, keys[i])) { virt = o_has_own_virtual(a, cur, keys[i]); break; }
            { Value p = o_proto(a, cur); cur = p.t == V_OBJ ? p.u.o : NULL; }
        }
        if (virt) v = v_undef();
        else {
            v = o_get(a, o, keys[i]);
            if (vm->err) { vm->err = 0; continue; }
        }
        w_element(w, keys[i], v);
    }
    free(keys);
}

/* the LSO body of `data`: one element per property, each followed by a 0 byte */
static void lso_body(Act *a, AObj *data, ABuf *b) {
    VM *vm = a->vm; int n = 0, i; AStr **keys = o_keys(a, data, 0, &n); RefCache rc; AW w;
    memset(&rc, 0, sizeof(rc));
    w.a = a; w.b = b; w.rc = &rc;
    if (vm->err) { vm->err = 0; free(keys); return; }
    for (i = n - 1; keys && i >= 0; i--) {
        Value v; size_t before = b->n; AObj *cur = data; int virt = 0, guard = 0;
        while (cur && guard++ < 256) {
            if (o_has_own_property(a, cur, keys[i])) { virt = o_has_own_virtual(a, cur, keys[i]); break; }
            { Value p = o_proto(a, cur); cur = p.t == V_OBJ ? p.u.o : NULL; }
        }
        if (virt) v = v_undef();
        else { v = o_get(a, data, keys[i]); if (vm->err) { vm->err = 0; continue; } }
        w_element(&w, keys[i], v);
        if (b->n != before) ab_u8(b, 0);
    }
    free(keys);
    rc_free(&rc);
}

/* a complete .sol file; *empty when there are no properties */
static u8 *lso_file(Act *a, AObj *data, const char *name, size_t *len, int *empty) {
    ABuf body, f; size_t hl; const char *sl = strrchr(name, '/'); const char *nm = sl ? sl + 1 : name;
    static const u8 sig[10] = { 0x54, 0x43, 0x53, 0x4f, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00 };
    memset(&body, 0, sizeof(body)); memset(&f, 0, sizeof(f));
    if (data) lso_body(a, data, &body);
    *empty = body.n == 0;
    hl = 10 + (2 + strlen(nm) + 3 + 1);
    ab_u8(&f, 0x00); ab_u8(&f, 0xbf);
    ab_u32(&f, (u32)(body.n + hl));
    ab_put(&f, sig, 10);
    ab_str16(&f, nm);
    ab_u8(&f, 0); ab_u8(&f, 0); ab_u8(&f, 0); ab_u8(&f, 0);
    if (body.n) ab_put(&f, body.d, body.n);
    free(body.d);
    if (f.oom) { free(f.d); return NULL; }
    *len = f.n;
    return f.d;
}

/* ---------------------------------------------------------------- */
/* reader                                                           */

typedef struct {
    Act *a; const u8 *p, *end; int bad;
    Value *slots; int nslots, cap;      /* one per value read: what a reference points at */
} AR;

static u32 rd_u8(AR *r) { if (r->p + 1 > r->end) { r->bad = 1; return 0; } return *r->p++; }
static u32 rd_u16(AR *r) { u32 v; if (r->p + 2 > r->end) { r->bad = 1; return 0; } v = ((u32)r->p[0] << 8) | r->p[1]; r->p += 2; return v; }
static u32 rd_u32(AR *r) { u32 v; if (r->p + 4 > r->end) { r->bad = 1; return 0; } v = ((u32)r->p[0] << 24) | ((u32)r->p[1] << 16) | ((u32)r->p[2] << 8) | r->p[3]; r->p += 4; return v; }
static double rd_f64(AR *r) { double d; u8 *o = (u8*)&d; int i; if (r->p + 8 > r->end) { r->bad = 1; return 0; } for (i = 0; i < 8; i++) o[7 - i] = r->p[i]; r->p += 8; return d; }
static AStr *rd_bytes(AR *r, u32 n) { AStr *s; if ((size_t)(r->end - r->p) < n) { r->bad = 1; return r->a->vm->s_empty; } s = str_utf8(r->a->vm, (const char*)r->p, n); r->p += n; return s; }
static AStr *rd_str16(AR *r) { u32 n = rd_u16(r); if (r->bad) return r->a->vm->s_empty; return rd_bytes(r, n); }

static int slot_new(AR *r) {
    if (r->nslots == r->cap) {
        int nc = r->cap ? r->cap * 2 : 32; Value *ns = (Value*)realloc(r->slots, (size_t)nc * sizeof(Value));
        if (!ns) { r->bad = 1; return 0; }
        r->slots = ns; r->cap = nc;
    }
    r->slots[r->nslots] = v_undef();
    return r->nslots++;
}

static Value rd_value(AR *r, int depth);

/* "name"/value pairs up to the empty name + ObjectEnd marker */
static void rd_members(AR *r, AObj *o, int array, int depth) {
    Act *a = r->a; VM *vm = a->vm;
    while (!r->bad) {
        AStr *name; Value v;
        if (r->p + 3 <= r->end && r->p[0] == 0 && r->p[1] == 0 && r->p[2] == M_END) { r->p += 3; return; }
        name = rd_str16(r);
        if (r->bad) return;
        if (r->p < r->end && *r->p == M_END) { r->p++; return; }
        v = rd_value(r, depth + 1);
        if (r->bad) return;
        if (array) {
            /* Rust str::parse::<i32>: optional sign and digits only */
            const char *s = str_c(vm, name); const char *q = s; int ok = 1; double d = 0; int neg = 0;
            if (*q == '+' || *q == '-') { neg = *q == '-'; q++; }
            if (!*q) ok = 0;
            for (; *q && ok; q++) { if (*q < '0' || *q > '9') ok = 0; else d = d * 10 + (*q - '0'); }
            if (neg) d = -d;
            if (ok && d >= -2147483648.0 && d <= 2147483647.0) { o_set_element(a, o, (int)d, v); vm->err = 0; continue; }
        }
        o_define(vm, o, name, v, 0);
    }
}

static Value construct1(Act *a, AObj *ctor, Value arg0) {
    Value v;
    if (!ctor) return v_undef();
    v = o_construct(a, ctor, 1, &arg0);
    if (a->vm->err) { a->vm->err = 0; return v_undef(); }
    return v;
}

static Value rd_value(AR *r, int depth) {
    Act *a = r->a; VM *vm = a->vm; Protos *P = PROTOS(a); u32 m; int slot;
    if (depth > 512) { r->bad = 1; return v_undef(); }
    m = rd_u8(r);
    if (r->bad) return v_undef();
    slot = slot_new(r);
    switch (m) {
    case M_NUM: return v_num(rd_f64(r));
    case M_BOOL: return v_bool(rd_u8(r) != 0);
    case M_STR: return v_str(rd_str16(r));
    case M_LSTR: { u32 n = rd_u32(r); return v_str(rd_bytes(r, n)); }
    case M_NULL: return v_null();
    case M_UNDEF: case M_UNSUP: return v_undef();
    case M_REF: { u32 i = rd_u16(r); return (int)i < r->nslots ? r->slots[i] : v_undef(); }
    case M_DATE: { double t = rd_f64(r); rd_u16(r); return construct1(a, P->date_ctor, v_num(t)); }
    case M_XML: { u32 n = rd_u32(r); AStr *s = rd_bytes(r, n); return construct1(a, P->xml_ctor, v_str(s)); }
    case M_OBJ: case M_TYPED: {
        AObj *o;
        if (m == M_TYPED) rd_str16(r);
        o = obj_new(vm, P->object);
        r->slots[slot] = v_obj(o);
        rd_members(r, o, 0, depth);
        return v_obj(o);
    }
    case M_ECMA: {
        u32 len = rd_u32(r); Value v = construct1(a, P->array_ctor, v_num((double)len)); AObj *o = v.t == V_OBJ ? v.u.o : NULL;
        if (!o) { r->bad = 1; return v_undef(); }
        r->slots[slot] = v;
        rd_members(r, o, 1, depth);
        return v;
    }
    case M_STRICT: {
        u32 n = rd_u32(r), i; Value v; AObj *o;
        if ((size_t)(r->end - r->p) < n) { r->bad = 1; return v_undef(); }
        v = construct1(a, P->array_ctor, v_num((double)n)); o = v.t == V_OBJ ? v.u.o : NULL;
        if (!o) { r->bad = 1; return v_undef(); }
        r->slots[slot] = v;
        for (i = 0; i < n && !r->bad; i++) { Value e = rd_value(r, depth + 1); o_set_element(a, o, (int)i, e); vm->err = 0; }
        return v;
    }
    default: r->bad = 1; return v_undef();
    }
}

/* the data object of a .sol file (NULL when it does not parse) */
static AObj *lso_read(Act *a, const u8 *d, size_t n) {
    static const u8 sig[10] = { 0x54, 0x43, 0x53, 0x4f, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00 };
    AR r; AObj *o; u32 nl;
    memset(&r, 0, sizeof(r));
    r.a = a; r.p = d; r.end = d + n;
    if (n < 16 || d[0] != 0 || d[1] != 0xbf || memcmp(d + 6, sig, 10)) return NULL;
    r.p = d + 16;
    nl = rd_u16(&r);
    if (r.bad || (size_t)(r.end - r.p) < nl + 4) return NULL;
    r.p += nl;
    if (r.p[0] || r.p[1] || r.p[2] || r.p[3] != 0) return NULL;     /* padding, AMF0 */
    r.p += 4;
    o = obj_new(a->vm, PROTOS(a)->object);
    while (r.p < r.end && !r.bad) {
        AStr *name = rd_str16(&r); Value v;
        if (r.bad) break;
        v = rd_value(&r, 0);
        if (r.bad) break;
        if (rd_u8(&r) != 0 || r.bad) break;
        o_define(a->vm, o, name, v, 0);
    }
    free(r.slots);
    return o;
}

/* ---------------------------------------------------------------- */
/* SharedObject                                                     */

typedef struct { char *name; } SOData;

static void so_free(AObj *o) { SOData *s = (SOData*)o->nat.p; if (s) { free(s->name); free(s); } o->nat.p = NULL; }

static SOData *so_of(AObj *o) { if (o && o->nk == NK_SUPER) o = o->nat.sup.self; return o && o->nk == NK_SHAREDOBJECT ? (SOData*)o->nat.p : NULL; }

static Value so_ctor(Act *a, AObj *self, int argc, Value *argv) {
    AObj *t = o_native_target(self); (void)a; (void)argc; (void)argv;
    if (t) {
        SOData *s = (SOData*)calloc(1, sizeof(SOData));
        if (s) { if (t->nfree) t->nfree(t); t->nk = NK_SHAREDOBJECT; t->nat.p = s; t->nfree = so_free; }
    }
    return v_undef();
}

static int has_char(const char *s, const char *set) { for (; *s; s++) if (strchr(set, *s)) return 1; return 0; }

/* target: the object ASnative(2106, 204) fills (NULL: getLocal creates or reuses one) */
static Value so_local(Act *a, AObj *target, int argc, Value *argv) {
    VM *vm = a->vm; AStr *nm; const char *name, *url, *p, *q; char path[1024], host[256], local[1024], full[2600];
    size_t pl; int secure, i; Prop *cached; AObj *so; Value data = v_undef(), ctorv; SOData *sd; AStr *key;
    nm = v_str_of(a, arg(argc, argv, 0)); if (vm->err) return v_undef();
    name = str_c(vm, nm);
    if (has_char(name, "~%&\\;:\"',<>?# ")) return v_null();
    url = avm1_movie_url(vm->player, a->base_clip ? a->base_clip->movie : NULL);
    secure = v_bool_of(a, arg(argc, argv, 2));
    p = strstr(url, "://");
    if (!p) return v_null();
    if (secure && strncmp(url, "https:", 6)) return v_null();
    p += 3;
    /* host and path (query and fragment dropped) */
    q = p; while (*q && *q != '/' && *q != '?' && *q != '#') q++;
    if (!strncmp(url, "file:", 5)) strcpy(host, "localhost");
    else { size_t hl = (size_t)(q - p); const char *c = memchr(p, ':', hl); if (c) hl = (size_t)(c - p); if (hl >= sizeof(host)) hl = sizeof(host) - 1; memcpy(host, p, hl); host[hl] = 0; }
    pl = 0;
    while (*q && *q != '?' && *q != '#' && pl + 1 < sizeof(path)) path[pl++] = *q++;
    path[pl] = 0;
    p = path; if (*p == '/') p++;
    pl = strlen(p); if (pl && p[pl - 1] == '/') pl--;
    memmove(path, p, pl); path[pl] = 0;
    if (!strncmp(url, "file:", 5) && pl >= 3 && path[1] == ':' && path[2] == '/') memmove(path, path + 3, pl - 2);
    if (argc > 1 && argv[1].t == V_STR) {
        const char *lp = str_c(vm, argv[1].u.s); size_t ll;
        if (!*lp) return v_null();
        strncpy(local, lp, sizeof(local) - 1); local[sizeof(local) - 1] = 0;
        ll = strlen(local);
        if (ll && local[ll - 1] == '/') local[--ll] = 0;
        if (local[0] == '/') { memmove(local, local + 1, ll); ll--; }
        if (strncmp(path, local, ll) || !(ll == 0 || strlen(path) == ll || path[ll] == '/')) return v_null();
    } else strcpy(local, path);
    sprintf(full, "%.255s/%.1000s/%s%.1000s", host, local, strchr(name, '/') ? "#" : "", name);
    /* no path segment may start with a dot */
    for (i = 0; full[i]; i++) if (full[i] == '.' && (i == 0 || full[i - 1] == '/')) return v_null();
    key = str_intern(vm, full);
    cached = target ? NULL : pm_get(&vm->so_map, key, 1);
    if (cached) return cached->v;
    if (target) so = target;
    else {
        ctorv = v_obj(PROTOS(a)->sharedobject_ctor);
        if (ctorv.u.o == NULL) return v_undef();
        ctorv = o_construct(a, ctorv.u.o, 0, NULL);
        if (vm->err) return v_undef();
        so = v_to_obj_or_bare(a, ctorv); if (vm->err || !so) return v_undef();
    }
    sd = so_of(so);
    if (sd) { free(sd->name); sd->name = (char*)malloc(strlen(full) + 1); if (sd->name) strcpy(sd->name, full); }
    {
        u8 *d; size_t n;
        if (store_get(full, &d, &n)) { AObj *o = lso_read(a, d, n); free(d); if (o) data = v_obj(o); }
    }
    if (data.t == V_UNDEF) data = v_obj(obj_new(vm, PROTOS(a)->object));
    o_define(vm, so, str_intern(vm, "data"), data, DD);
    if (target) return v_bool(1);
    { Prop *pp = pm_insert(vm, &vm->so_map, key, 1); if (pp) pp->v = v_obj(so); }
    return v_obj(so);
}
static Value so_get_local(Act *a, AObj *self, int argc, Value *argv) { (void)self; return so_local(a, NULL, argc, argv); }

static Value so_flush(Act *a, AObj *self, int argc, Value *argv);
static Value so_get_size(Act *a, AObj *self, int argc, Value *argv);
static Value so_clear(Act *a, AObj *self, int argc, Value *argv);
/* ASnative(2106, n): the SharedObject natives; 202 is the class's getLocal, 204 fills
 * the given object (false when the name or path is refused) */
Value shared_object_native(Act *a, AObj *self, int argc, Value *argv, int index) {
    switch (index) {
    case 2: return so_flush(a, self, argc, argv);
    case 4: return so_get_size(a, self, argc, argv);
    case 6: return so_clear(a, self, argc, argv);
    case 202: return self == PROTOS(a)->sharedobject_ctor ? so_local(a, NULL, argc, argv) : v_null();
    case 204: {
        AObj *t = argc > 0 && argv[0].t == V_OBJ ? argv[0].u.o : NULL; Value r;
        if (!t) return v_bool(0);
        r = so_local(a, t, argc - 1, argv + 1);
        return r.t == V_BOOL ? r : v_bool(0);
    }
    default: return v_undef();
    }
}

static AObj *so_data(Act *a, AObj *self) {
    Value v = o_get(a, self, str_intern(a->vm, "data"));
    if (a->vm->err) return NULL;
    return v_to_obj_or_bare(a, v);
}

static Value so_flush(Act *a, AObj *self, int argc, Value *argv) {
    SOData *s = so_of(self); AObj *d; u8 *f; size_t n; int empty, ok; (void)argc; (void)argv;
    if (!s) return v_undef();
    d = so_data(a, self); if (a->vm->err) return v_undef();
    s = so_of(self); if (!s) return v_undef();
    f = lso_file(a, d, s->name ? s->name : "", &n, &empty);
    if (!f) return v_bool(0);
    if (empty) { free(f); return v_bool(1); }
    ok = store_put(s->name ? s->name : "", f, n);
    free(f);
    return v_bool(ok);
}

static Value so_get_size(Act *a, AObj *self, int argc, Value *argv) {
    SOData *s = so_of(self); AObj *d; u8 *f; size_t n; int empty; (void)argc; (void)argv;
    if (!s) return v_undef();
    d = so_data(a, self); if (a->vm->err) return v_undef();
    s = so_of(self); if (!s) return v_undef();
    f = lso_file(a, d, s->name ? s->name : "", &n, &empty);
    free(f);
    return v_num(empty ? 0 : (double)n);
}

static Value so_clear(Act *a, AObj *self, int argc, Value *argv) {
    AObj *d; SOData *s; int n = 0, i; AStr **keys; (void)argc; (void)argv;
    d = so_data(a, self); if (a->vm->err || !d) return v_undef();
    keys = o_keys(a, d, 0, &n);
    for (i = 0; keys && i < n; i++) { o_delete(a, d, keys[i]); a->vm->err = 0; }
    free(keys);
    s = so_of(self);
    if (s && s->name) store_remove(s->name);
    return v_undef();
}

static Value so_noop(Act *a, AObj *self, int argc, Value *argv) { (void)a; (void)self; (void)argc; (void)argv; return v_undef(); }

static void defm(VM *vm, Env *env, AObj *o, const char *name, NativeFn f, u16 attr) { o_define(vm, o, str_intern(vm, name), v_obj(fn_native(vm, env, f, NULL)), attr); }

AObj *lib_sharedobject(VM *vm, Env *env, AObj *object_proto) {
    AObj *proto, *c = lib_class(vm, env, NULL, so_ctor, object_proto, 0, &proto);
    env->p.sharedobject = proto; env->p.sharedobject_ctor = c;
    defm(vm, env, proto, "connect", so_noop, DE | DD);
    defm(vm, env, proto, "send", so_noop, DE | DD);
    defm(vm, env, proto, "flush", so_flush, DE | DD);
    defm(vm, env, proto, "close", so_noop, DE | DD);
    defm(vm, env, proto, "getSize", so_get_size, DE | DD);
    defm(vm, env, proto, "setFps", so_noop, DE | DD);
    defm(vm, env, proto, "clear", so_clear, DE | DD);
    defm(vm, env, proto, "onStatus", so_noop, DE | DD);
    defm(vm, env, proto, "onSync", so_noop, DE | DD);
    defm(vm, env, c, "deleteAll", so_noop, DE);
    defm(vm, env, c, "getDiskUsage", so_noop, DE);
    defm(vm, env, c, "getLocal", so_get_local, 0);
    defm(vm, env, c, "getRemote", so_noop, 0);
    return c;
}

/* ---------------------------------------------------------------- */
/* serialize (NetConnection / LocalConnection values, amf.rs)       */

static void s_value(Act *a, Value v, ABuf *b, int depth);

static int usize_key(const char *s, double *out) {
    double d = 0; if (*s == '+') s++;
    if (!*s) return 0;
    for (; *s; s++) { if (*s < '0' || *s > '9') return 0; d = d * 10 + (*s - '0'); }
    *out = d; return 1;
}

static void s_array(Act *a, AObj *o, ABuf *b, int depth) {
    VM *vm = a->vm; int n = 0, i, custom = 0; double len; AStr **keys;
    len = o_length(a, o); if (vm->err) { vm->err = 0; len = 0; }
    if (len < 0) len = 0;
    keys = o_keys(a, o, 0, &n); vm->err = 0;
    for (i = 0; keys && i < n; i++) { double k; if (usize_key(str_c(vm, keys[i]), &k)) { if (k + 1 > len) len = k + 1; } else custom = 1; }
    if (custom) {
        ab_u8(b, M_ECMA); ab_u32(b, (u32)len);
        for (i = n - 1; keys && i >= 0; i--) {
            Value v = o_get(a, o, keys[i]); if (vm->err) { vm->err = 0; v = v_undef(); }
            ab_str16(b, str_c(vm, keys[i]));
            s_value(a, v, b, depth + 1);
        }
        ab_u16(b, 0); ab_u8(b, M_END);
    } else {
        u32 k;
        ab_u8(b, M_STRICT); ab_u32(b, (u32)len);
        for (k = 0; k < (u32)len; k++) {
            Value v = o_get_element(a, o, (int)k); if (vm->err) { vm->err = 0; v = v_undef(); }
            s_value(a, v, b, depth + 1);
        }
    }
    free(keys);
}

static void s_value(Act *a, Value v, ABuf *b, int depth) {
    VM *vm = a->vm;
    if (depth > 256) { ab_u8(b, M_UNDEF); return; }
    switch (v.t) {
    case V_NUM: ab_u8(b, M_NUM); ab_f64(b, v.u.n); return;
    case V_BOOL: ab_u8(b, M_BOOL); ab_u8(b, v.u.b ? 1 : 0); return;
    case V_NULL: ab_u8(b, M_NULL); return;
    case V_STR: {
        const char *u = str_c(vm, v.u.s); size_t n = strlen(u);
        if (n > 65535) { ab_u8(b, M_LSTR); ab_u32(b, (u32)n); ab_put(b, u, n); }
        else { ab_u8(b, M_STR); ab_u16(b, (unsigned)n); ab_put(b, u, n); }
        return;
    }
    case V_OBJ: {
        AObj *o = v.u.o;
        if (OBJ_IS_DISPLAY(o)) { ab_u8(b, M_UNDEF); return; }
        if (o->nk == NK_ARRAY) { s_array(a, o, b, depth); return; }
        if (o->nk == NK_XML || o->nk == NK_XMLNODE) {
            AStr *s = avm1_xml_string(a, o); const char *u; size_t n;
            vm->err = 0; u = s ? str_c(vm, s) : ""; n = strlen(u);
            ab_u8(b, M_XML); ab_u32(b, (u32)n); ab_put(b, u, n); return;
        }
        if (o->nk == NK_DATE) { ab_u8(b, M_DATE); ab_f64(b, o->nat.n); ab_u16(b, 0); return; }
        {
            AStr *cls = class_name_of(a, o); RefCache rc; AW w;
            memset(&rc, 0, sizeof(rc)); w.a = a; w.b = b; w.rc = &rc;
            if (cls) { ab_u8(b, M_TYPED); ab_str16(b, str_c(vm, cls)); } else ab_u8(b, M_OBJ);
            w_props(&w, o);
            ab_u16(b, 0); ab_u8(b, M_END);
            rc_free(&rc);
        }
        return;
    }
    default: ab_u8(b, M_UNDEF); return;
    }
}

/* one serialized value as a malloc'd buffer */
static u8 *serialize_value(Act *a, Value v, size_t *n) {
    ABuf b; memset(&b, 0, sizeof(b));
    s_value(a, v, &b, 0);
    if (b.oom) { free(b.d); return NULL; }
    *n = b.n; return b.d;
}

static Value deserialize(Act *a, const u8 *d, size_t n) {
    AR r; Value v;
    memset(&r, 0, sizeof(r)); r.a = a; r.p = d; r.end = d + n;
    v = rd_value(&r, 0);
    free(r.slots);
    return r.bad ? v_undef() : v;
}

/* one AMF0 value at *pp (FLV script data), advancing it */
Value avm1_amf0_value(Act *a, const u8 **pp, const u8 *end) {
    AR r; Value v;
    memset(&r, 0, sizeof(r)); r.a = a; r.p = *pp; r.end = end;
    v = rd_value(&r, 0);
    free(r.slots);
    *pp = r.bad ? end : r.p;
    return r.bad ? v_undef() : v;
}

/* ---------------------------------------------------------------- */
/* NetConnection                                                    */

typedef struct { char *name; int must; u8 *val; size_t n; } NCHdr;
typedef struct { char *target; char resp[16]; u8 *val; size_t n; AObj *responder; } NCMsg;
typedef struct { int kind; char *url; NCHdr *h; int nh; NCMsg *m; int nm; } NCData;   /* kind: 0 none, 1 local, 2 remoting */

#include "lc_hub.h"
typedef struct Net {
    AObj **ncpend; int nncpend;
} Net;

static Net *net_of(VM *vm) { if (!vm->net) vm->net = calloc(1, sizeof(Net)); return (Net*)vm->net; }

static char *dupz(const char *s) { size_t l = strlen(s); char *d = (char*)malloc(l + 1); if (d) memcpy(d, s, l + 1); return d; }

static void nc_clear_queue(NCData *d) { int i; for (i = 0; i < d->nm; i++) { free(d->m[i].target); free(d->m[i].val); } free(d->m); d->m = NULL; d->nm = 0; }
static void nc_reset(NCData *d) {
    int i;
    nc_clear_queue(d);
    for (i = 0; i < d->nh; i++) { free(d->h[i].name); free(d->h[i].val); }
    free(d->h); d->h = NULL; d->nh = 0;
    free(d->url); d->url = NULL; d->kind = 0;
}
static void nc_free(AObj *o) { NCData *d = (NCData*)o->nat.p; if (d) { nc_reset(d); free(d); } o->nat.p = NULL; }
static void nc_mark(VM *vm, AObj *o) { NCData *d = (NCData*)o->nat.p; int i; if (!d) return; for (i = 0; i < d->nm; i++) if (d->m[i].responder) gc_mark_obj(vm, d->m[i].responder); }
static NCData *nc_of(AObj *o) { if (o && o->nk == NK_SUPER) o = o->nat.sup.self; return o && o->nk == NK_NETCONN ? (NCData*)o->nat.p : NULL; }

static void status_event(Act *a, AObj *target, const char *code) {
    VM *vm = a->vm; AObj *ev = obj_new(vm, PROTOS(a)->object); Value v;
    o_set(a, ev, str_intern(vm, "code"), v_str(str_intern(vm, code))); vm->err = 0;
    o_set(a, ev, str_intern(vm, "level"), v_str(str_intern(vm, "status"))); vm->err = 0;
    v = v_obj(ev);
    o_call_method(a, target, str_intern(vm, "onStatus"), 1, &v, ER_SPECIAL);
    if (vm->err) vm_report_error(a);
}

static void nc_close(Act *a, AObj *self, int explicit_) {
    NCData *d = nc_of(self); int kind;
    if (!d || !d->kind) return;
    kind = d->kind;
    nc_reset(d);
    status_event(a, self, "NetConnection.Connect.Closed");
    if (explicit_ && kind == 2) { o_call_method(a, self, str_intern(a->vm, "onStatus"), 0, NULL, ER_SPECIAL); if (a->vm->err) vm_report_error(a); }
}

static Value nc_is_connected(Act *a, AObj *self, int argc, Value *argv);
static Value nc_ignore_set(Act *a, AObj *self, int argc, Value *argv) { (void)a; (void)self; (void)argc; (void)argv; return v_undef(); }
static Value nc_ctor(Act *a, AObj *self, int argc, Value *argv) {
    AObj *t = o_native_target(self); (void)a; (void)argc; (void)argv;
    if (t) { NCData *d = (NCData*)calloc(1, sizeof(NCData)); if (d) { t->nk = NK_NETCONN; t->nat.p = d; t->nfree = nc_free; t->nmark = nc_mark; } }
    {   /* the Linux player gives each connection its own contentType (and, 9-14, isConnected) */
        extern int devfont_linux_rules(void); extern int avm1_linux_ns_cat(void);
        if (t && devfont_linux_rules()) {
            Env *e = env_get(a->vm, a->swf_version);
            if (avm1_linux_ns_cat() <= 2) o_add_property(a->vm, t, str_intern(a->vm, "isConnected"), fn_native(a->vm, e, nc_is_connected, NULL), fn_native(a->vm, e, nc_ignore_set, NULL), PA_DONT_ENUM | PA_DONT_DELETE);
            o_define(a->vm, t, str_intern(a->vm, "contentType"), v_undef(), PA_DONT_ENUM);
        }
    }
    return v_undef();
}

static Value nc_connect(Act *a, AObj *self, int argc, Value *argv) {
    NCData *d = nc_of(self); AStr *u; const char *s;
    if (argc < 1 || argv[0].t == V_UNDEF || argv[0].t == V_NULL) {
        if (!d) return v_undef();
        if (d->kind) nc_close(a, self, 0);
        d = nc_of(self); if (!d) return v_undef();
        d->kind = 1;
        status_event(a, self, "NetConnection.Connect.Success");
        return v_undef();
    }
    u = v_str_of(a, argv[0]); if (a->vm->err) return v_undef();
    s = str_c(a->vm, u);
    if (!d) return v_undef();
    if (!strncmp(s, "http://", 7) || !strncmp(s, "https://", 8) || !strncmp(s, "HTTP://", 7) || !strncmp(s, "HTTPS://", 8)) {
        if (d->kind) nc_close(a, self, 0);
        d = nc_of(self); if (!d) return v_undef();
        d->kind = 2; d->url = dupz(s);
    }
    return v_undef();
}

static Value nc_close_m(Act *a, AObj *self, int argc, Value *argv) { (void)argc; (void)argv; nc_close(a, self, 1); return v_undef(); }

static Value nc_call(Act *a, AObj *self, int argc, Value *argv) {
    VM *vm = a->vm; NCData *d = nc_of(self); AStr *cmd; ABuf b; int i; AObj *resp = NULL; NCMsg *nm;
    if (!d) return v_undef();
    cmd = v_str_of(a, arg(argc, argv, 0)); if (vm->err) return v_undef();
    memset(&b, 0, sizeof(b));
    ab_u8(&b, M_STRICT); ab_u32(&b, (u32)(argc > 2 ? argc - 2 : 0));
    for (i = 2; i < argc; i++) s_value(a, argv[i], &b, 0);
    d = nc_of(self);
    if (!d || d->kind != 2) { free(b.d); if (argc > 1) { v_to_obj_or_bare(a, argv[1]); } return v_undef(); }
    if (argc > 1) { resp = v_to_obj_or_bare(a, argv[1]); if (vm->err) { free(b.d); return v_undef(); } }
    nm = (NCMsg*)realloc(d->m, (size_t)(d->nm + 1) * sizeof(NCMsg));
    if (!nm || b.oom) { free(b.d); return v_undef(); }
    d->m = nm; nm = &d->m[d->nm];
    nm->target = dupz(str_c(vm, cmd)); sprintf(nm->resp, "/%d", d->nm + 1); nm->val = b.d; nm->n = b.n; nm->responder = resp;
    d->nm++;
    if (d->nm == 1) {
        Net *N = net_of(vm); AObj **np = N ? (AObj**)realloc(N->ncpend, (size_t)(N->nncpend + 1) * sizeof(AObj*)) : NULL;
        if (np) { N->ncpend = np; np[N->nncpend++] = self->nk == NK_SUPER ? self->nat.sup.self : self; }
    }
    return v_undef();
}

static Value nc_add_header(Act *a, AObj *self, int argc, Value *argv) {
    VM *vm = a->vm; NCData *d = nc_of(self); AStr *name; int must = 1, i; u8 *val; size_t n = 0; NCHdr *nh;
    if (!d) return v_undef();
    name = v_str_of(a, arg(argc, argv, 0)); if (vm->err) return v_undef();
    if (argc > 1) must = v_bool_of(a, argv[1]);
    val = serialize_value(a, argc > 2 ? argv[2] : v_null(), &n);
    d = nc_of(self);
    if (!d || d->kind != 2 || !val) { free(val); return v_undef(); }
    for (i = 0; i < d->nh; i++) {
        const char *x = d->h[i].name, *y = str_c(vm, name);
        while (*x && *y && ((*x | 32) == (*y | 32) || *x == *y)) { x++; y++; }
        if (!*x && !*y) { free(d->h[i].name); free(d->h[i].val); memmove(&d->h[i], &d->h[i + 1], (size_t)(d->nh - i - 1) * sizeof(NCHdr)); d->nh--; break; }
    }
    nh = (NCHdr*)realloc(d->h, (size_t)(d->nh + 1) * sizeof(NCHdr));
    if (!nh) { free(val); return v_undef(); }
    d->h = nh; d->h[d->nh].name = dupz(str_c(vm, name)); d->h[d->nh].must = must; d->h[d->nh].val = val; d->h[d->nh].n = n; d->nh++;
    return v_undef();
}

int avm1_netconn_connected(AObj *o) { NCData *d = nc_of(o); return d && d->kind == 1; }
static Value nc_is_connected(Act *a, AObj *self, int argc, Value *argv) { NCData *d = nc_of(self); (void)a; (void)argc; (void)argv; return d ? v_bool(d->kind == 1) : v_undef(); }
static Value nc_protocol(Act *a, AObj *self, int argc, Value *argv) { NCData *d = nc_of(self); (void)argc; (void)argv; return d && d->kind == 1 ? v_str(str_intern(a->vm, "rtmp")) : v_undef(); }
static Value nc_uri(Act *a, AObj *self, int argc, Value *argv) {
    NCData *d = nc_of(self); (void)argc; (void)argv;
    if (!d || !d->kind) return v_undef();
    return v_str(str_intern(a->vm, d->kind == 1 ? "null" : d->url));
}

extern void avm1_log_fetch(VM *vm, AStr *url, int post, const char *mime, const u8 *body, size_t blen);

/* an AMF response packet: "/N/onResult" and "/N/onStatus" go to the responder of call N
 * (invalid packets are ignored, like Flash does) */
static void nc_response(Act *a, const u8 *d, size_t n, NCMsg *msgs, int nmsg) {
    VM *vm = a->vm; AR r; u32 nh, nm, i;
    memset(&r, 0, sizeof(r)); r.a = a; r.p = d; r.end = d + n;
    rd_u16(&r); nh = rd_u16(&r);
    for (i = 0; i < nh && !r.bad; i++) { u32 l; rd_str16(&r); rd_u8(&r); l = rd_u32(&r); if (l != 0xFFFFFFFFu && (size_t)(r.end - r.p) >= l) r.p += l; else rd_value(&r, 0); }
    nm = rd_u16(&r);
    if (r.bad) { free(r.slots); return; }
    {
        typedef struct { char *t; Value v; } RM; RM *list = (RM*)calloc(nm + 1, sizeof(RM)); u32 k = 0;
        if (!list) { free(r.slots); return; }
        for (i = 0; i < nm && !r.bad; i++) {
            AStr *t = rd_str16(&r); rd_str16(&r); rd_u32(&r);
            list[k].v = rd_value(&r, 0);
            if (!r.bad) list[k++].t = dupz(str_c(vm, t));
        }
        if (!r.bad) {
            for (i = 0; i < k; i++) {
                const char *t = list[i].t; int idx = 0; const char *cb = NULL;
                if (t[0] == '/') {
                    const char *q = t + 1; while (*q >= '0' && *q <= '9') idx = idx * 10 + (*q++ - '0');
                    if (q > t + 1 && !strcmp(q, "/onResult")) cb = "onResult";
                    else if (q > t + 1 && !strcmp(q, "/onStatus")) cb = "onStatus";
                }
                if (cb && idx >= 1 && idx <= nmsg && msgs[idx - 1].responder) {
                    o_call_method(a, msgs[idx - 1].responder, str_intern(vm, cb), 1, &list[i].v, ER_SPECIAL);
                    if (vm->err) vm_report_error(a);
                }
            }
        }
        for (i = 0; i < k; i++) free(list[i].t);
        free(list);
    }
    free(r.slots);
}

/* send the queued calls of every remoting connection as one AMF packet each */
void avm1_net_flush(VM *vm) {
    Net *N = (Net*)vm->net; int k, cnt; AObj **pend;
    if (!N || !N->nncpend) return;
    pend = N->ncpend; cnt = N->nncpend; N->ncpend = NULL; N->nncpend = 0;
    for (k = 0; k < cnt; k++) {
        AObj *o = pend[k]; NCData *d = nc_of(o); ABuf b; int i; Act a;
        if (!d || d->kind != 2 || !d->nm) continue;
        memset(&b, 0, sizeof(b));
        ab_u8(&b, 0); ab_u8(&b, 0);
        ab_u16(&b, (unsigned)d->nh);
        for (i = 0; i < d->nh; i++) { ab_str16(&b, d->h[i].name); ab_u8(&b, d->h[i].must ? 1 : 0); ab_u32(&b, (u32)d->h[i].n); ab_put(&b, d->h[i].val, d->h[i].n); }
        ab_u16(&b, (unsigned)d->nm);
        for (i = 0; i < d->nm; i++) { ab_str16(&b, d->m[i].target); ab_str16(&b, d->m[i].resp); ab_u32(&b, (u32)d->m[i].n); ab_put(&b, d->m[i].val, d->m[i].n); }
        avm1_log_fetch(vm, str_intern(vm, d->url), 1, "application/x-amf", b.d, b.n);
        free(b.d);
        {
            NCMsg *msgs = d->m; int nmsg = d->nm; char path[1100]; u8 *resp = NULL; size_t rn = 0;
            d->m = NULL; d->nm = 0;
            act_init_nothing(&a, vm, vm->player->root, "[NetConnection response]");
            if (fetch_url_to_path(vm->player, d->url, path, sizeof(path))) resp = fetch_read_file(path, &rn);
            if (!resp) {
                /* the request failed: Flash reports it with an empty onStatus */
                o_call_method(&a, o, str_intern(vm, "onStatus"), 0, NULL, ER_SPECIAL);
                if (vm->err) vm_report_error(&a);
            } else nc_response(&a, resp, rn, msgs, nmsg);
            free(resp);
            for (i = 0; i < nmsg; i++) { free(msgs[i].target); free(msgs[i].val); }
            free(msgs);
        }
    }
    free(pend);
}

AObj *lib_netconnection(VM *vm, Env *env, AObj *object_proto) {
    AObj *proto, *c = lib_class(vm, env, NULL, nc_ctor, object_proto, 1, &proto);
    env->p.netconnection = proto;
    o_add_property(vm, proto, str_intern(vm, "protocol"), fn_native(vm, env, nc_protocol, NULL), NULL, 0);
    defm(vm, env, proto, "connect", nc_connect, DE | DD);
    defm(vm, env, proto, "close", nc_close_m, DE | DD);
    defm(vm, env, proto, "call", nc_call, DE | DD);
    defm(vm, env, proto, "addHeader", nc_add_header, DE | DD);
    o_add_property(vm, proto, str_intern(vm, "isConnected"), fn_native(vm, env, nc_is_connected, NULL), NULL, 0);
    o_add_property(vm, proto, str_intern(vm, "uri"), fn_native(vm, env, nc_uri, NULL), NULL, 0);
    return c;
}

/* ---------------------------------------------------------------- */
/* LocalConnection                                                  */

typedef struct { char *key; } LCData;

static void lc_free(AObj *o) { LCData *d = (LCData*)o->nat.p; if (d) { if (d->key) { lch_close(d->key); free(d->key); } free(d); } o->nat.p = NULL; }
static LCData *lc_of(AObj *o) { if (o && o->nk == NK_SUPER) o = o->nat.sup.self; return o && o->nk == NK_LOCALCONN ? (LCData*)o->nat.p : NULL; }

static void lc_superdomain(VM *vm, char *out, size_t n) { lch_superdomain(avm1_movie_url(vm->player, vm->player->movie), out, n); }

static void lc_status_cb(void *ctx, void *src, const char *level);
static void lc_call_cb(void *ctx, void *obj, const char *method, int nargs, u8 **args, size_t *argn);
static void lc_hook(VM *vm) { static VM *hooked; if (hooked != vm) { LCHandler h; h.ctx = vm; h.status = lc_status_cb; h.call = lc_call_cb; lch_set_handler(0, &h); hooked = vm; } }

static Value lc_ctor(Act *a, AObj *self, int argc, Value *argv) {
    AObj *t = o_native_target(self); (void)argc; (void)argv;
    lc_hook(a->vm);
    if (t) { LCData *d = (LCData*)calloc(1, sizeof(LCData)); if (d) { t->nk = NK_LOCALCONN; t->nat.p = d; t->nfree = lc_free; } }
    return v_obj(self);
}

static Value lc_connect(Act *a, AObj *self, int argc, Value *argv) {
    VM *vm = a->vm; LCData *d = lc_of(self); const char *name; char *key, sd[256];
    if (argc < 1 || argv[0].t != V_STR) return v_bool(0);
    name = str_c(vm, argv[0].u.s);
    if (!*name || strchr(name, ':')) return v_bool(0);
    if (!d) return v_undef();
    if (d->key) return v_bool(0);
    lc_hook(vm);
    lc_superdomain(vm, sd, sizeof(sd));
    key = lch_key(sd, name, 0);
    if (!key) return v_bool(0);
    if (!lch_connect(key, 0, self->nk == NK_SUPER ? self->nat.sup.self : self)) { free(key); return v_bool(0); }
    d->key = key;
    return v_bool(1);
}

static void lc_disconnect(VM *vm, LCData *d) { (void)vm; if (!d || !d->key) return; lch_close(d->key); free(d->key); d->key = NULL; }

static Value lc_close(Act *a, AObj *self, int argc, Value *argv) { (void)argc; (void)argv; lc_disconnect(a->vm, lc_of(self)); return v_undef(); }

static Value lc_send(Act *a, AObj *self, int argc, Value *argv) {
    VM *vm = a->vm; const char *cn, *mn; static const char *bad[] = { "send", "connect", "close", "allowDomain", "allowInsecureDomain", "domain", NULL };
    char *conn, sd[256]; int i, nargs; u8 **args; size_t *argn;
    if (argc < 1 || argv[0].t != V_STR || argc < 2 || argv[1].t != V_STR) return v_bool(0);
    cn = str_c(vm, argv[0].u.s); mn = str_c(vm, argv[1].u.s);
    if (!*cn || !*mn) return v_bool(0);
    for (i = 0; bad[i]; i++) if (!strcmp(mn, bad[i])) return v_bool(0);
    lc_hook(vm);
    { char *m2 = dupz(mn); nargs = argc - 2;
      args = (u8**)calloc((size_t)nargs + 1, sizeof(u8*)); argn = (size_t*)calloc((size_t)nargs + 1, sizeof(size_t));
      if (!args || !argn || !m2) { free(args); free(argn); free(m2); return v_bool(0); }
      for (i = 0; i < nargs; i++) args[i] = serialize_value(a, argv[2 + i], &argn[i]);
      lc_superdomain(vm, sd, sizeof(sd));
      conn = lch_key(sd, cn, 1);
      lch_send(0, self->nk == NK_SUPER ? self->nat.sup.self : self, conn ? conn : "", m2, nargs, args, argn);
      free(conn); free(m2); }
    return v_bool(1);
}

static Value lc_domain(Act *a, AObj *self, int argc, Value *argv) {
    const char *url = avm1_movie_url(a->vm->player, a->base_clip ? a->base_clip->movie : NULL), *p = strstr(url, "://"), *q; char dom[256]; size_t l;
    (void)self; (void)argc; (void)argv;
    if (!p || !strncmp(url, "file:", 5)) return v_str(str_intern(a->vm, "localhost"));
    p += 3; q = p; while (*q && *q != '/' && *q != ':' && *q != '?' && *q != '#') q++;
    l = (size_t)(q - p); if (l >= sizeof(dom)) l = sizeof(dom) - 1; memcpy(dom, p, l); dom[l] = 0;
    return v_str(str_intern(a->vm, l ? dom : "localhost"));
}

static void lc_status_cb(void *ctx, void *src, const char *level) {
    VM *vm = (VM*)ctx; Act a; AObj *ev; Value v;
    act_init_nothing(&a, vm, vm->player->root, "[LocalConnection call]");
    ev = obj_new(vm, PROTOS(&a)->object);
    o_set(&a, ev, str_intern(vm, "level"), v_str(str_intern(vm, level))); vm->err = 0;
    v = v_obj(ev);
    o_call_method(&a, (AObj*)src, str_intern(vm, "onStatus"), 1, &v, ER_SPECIAL);
    if (vm->err) vm_report_error(&a);
    avm1_run_actions(vm);
}
static void lc_call_cb(void *ctx, void *obj, const char *method, int nargs, u8 **args, size_t *argn) {
    VM *vm = (VM*)ctx; Act a; Value *av = (Value*)calloc((size_t)nargs + 1, sizeof(Value)); int i;
    if (!av) return;
    act_init_nothing(&a, vm, vm->player->root, "[LocalConnection call]");
    for (i = 0; i < nargs; i++) av[i] = args[i] ? deserialize(&a, args[i], argn[i]) : v_undef();
    o_call_method(&a, (AObj*)obj, str_utf8(vm, method, strlen(method)), nargs, av, ER_SPECIAL);
    if (vm->err) vm_report_error(&a);
    free(av);
    avm1_run_actions(vm);
}

/* LocalConnections::update_connections: deliver the messages sent this frame */
void avm1_net_frame(VM *vm) { (void)vm; lch_deliver(); }

static void lc_mark_cb(void *ctx, void *obj) { gc_mark_obj((VM*)ctx, (AObj*)obj); }
void avm1_net_mark(VM *vm) {
    Net *N = (Net*)vm->net; int i;
    lch_mark(0, lc_mark_cb, vm);
    if (!N) return;
    for (i = 0; i < N->nncpend; i++) gc_mark_obj(vm, N->ncpend[i]);
}

void avm1_net_free(VM *vm) {
    Net *N = (Net*)vm->net;
    lch_forget_vm(0);
    if (!N) return;
    free(N->ncpend); free(N);
    vm->net = NULL;
}

static Value lc_is_per_user(Act *a, AObj *self, int argc, Value *argv) { (void)a; (void)self; (void)argc; (void)argv; return v_undef(); }
AObj *lib_localconnection(VM *vm, Env *env, AObj *object_proto) {
    AObj *proto, *c = lib_class(vm, env, lc_ctor, NULL, object_proto, 1, &proto);
    env->p.localconnection = proto;
    defm(vm, env, proto, "connect", lc_connect, DD | DE);
    defm(vm, env, proto, "send", lc_send, DD | DE);
    defm(vm, env, proto, "close", lc_close, DD | DE);
    defm(vm, env, proto, "domain", lc_domain, DD | DE);
    o_add_property(vm, proto, str_intern(vm, "isPerUser"), fn_native(vm, env, lc_is_per_user, NULL), NULL, DD | DE);
    return c;
}

/* ---------------------------------------------------------------- */
/* XMLSocket                                                        */

#include "sock.h"

typedef struct { int handle; u32 timeout; u8 *buf; size_t n; } XSData;
typedef struct { int handle; AObj *obj; } XSConn;
static XSConn *g_xs; static int g_nxs;

static void xs_free(AObj *o) { XSData *d = (XSData*)o->nat.p; if (d) { free(d->buf); free(d); } o->nat.p = NULL; }
static XSData *xs_of(AObj *o) { if (o && o->nk == NK_SUPER) o = o->nat.sup.self; return o && o->nk == NK_XMLSOCKET ? (XSData*)o->nat.p : NULL; }

static void xs_forget(int h) { int i; for (i = 0; i < g_nxs; i++) if (g_xs[i].handle == h) { g_xs[i] = g_xs[--g_nxs]; return; } }
static AObj *xs_find(int h) { int i; for (i = 0; i < g_nxs; i++) if (g_xs[i].handle == h) return g_xs[i].obj; return NULL; }

static Value xs_ctor(Act *a, AObj *self, int argc, Value *argv) {
    AObj *t = o_native_target(self); (void)a; (void)argc; (void)argv;
    if (t) { XSData *d = (XSData*)calloc(1, sizeof(XSData)); if (d) { d->timeout = 20000; t->nk = NK_XMLSOCKET; t->nat.p = d; t->nfree = xs_free; } }
    return v_undef();
}

static Value xs_get_timeout(Act *a, AObj *self, int argc, Value *argv) { XSData *d = xs_of(self); (void)a; (void)argc; (void)argv; return d ? v_num(d->timeout) : v_undef(); }
static Value xs_set_timeout(Act *a, AObj *self, int argc, Value *argv) { XSData *d = xs_of(self); u32 t; if (!d) return v_undef(); t = v_u32(a, arg(argc, argv, 0)); if (a->vm->err) return v_undef(); d = xs_of(self); if (d) d->timeout = t; return v_undef(); }

static Value xs_close(Act *a, AObj *self, int argc, Value *argv) {
    XSData *d = xs_of(self); (void)argc; (void)argv;
    if (d && d->handle) { sock_close(a->vm->player, d->handle); xs_forget(d->handle); d->handle = 0; d->n = 0; }
    return v_undef();
}

static Value xs_connect(Act *a, AObj *self, int argc, Value *argv) {
    VM *vm = a->vm; XSData *d = xs_of(self); Value hv; AStr *host; u16 port; int h, old; XSConn *nx; char hb[512];
    if (!d) return v_undef();
    hv = argc > 0 ? argv[0] : v_null();
    if (hv.t == V_NULL || hv.t == V_UNDEF) {
        const char *url = avm1_movie_url(vm->player, a->base_clip ? a->base_clip->movie : NULL), *p = strstr(url, "://"), *q;
        if (!p || !strncmp(url, "file:", 5)) hv = v_str(str_intern(vm, "localhost"));
        else { size_t l; p += 3; q = p; while (*q && *q != '/' && *q != ':' && *q != '?') q++; l = (size_t)(q - p); if (l >= sizeof(hb)) l = sizeof(hb) - 1; memcpy(hb, p, l); hb[l] = 0; hv = v_str(str_intern(vm, l ? hb : "localhost")); }
    }
    host = v_str_of(a, hv); if (vm->err) return v_undef();
    {   /* the Linux player refuses a port that does not fit 16 bits */
        extern int devfont_linux_rules(void);
        if (devfont_linux_rules()) { double pn = v_num_of(a, arg(argc, argv, 1)); if (vm->err) return v_undef(); if (!(pn >= 0 && pn <= 65535)) return v_bool(0); }
    }
    port = v_u16(a, arg(argc, argv, 1)); if (vm->err) return v_undef();
    d = xs_of(self); if (!d) return v_undef();
    strncpy(hb, str_c(vm, host), sizeof(hb) - 1); hb[sizeof(hb) - 1] = 0;   /* a NUL ends the host */
    h = sock_connect(vm->player, hb, port, (int)d->timeout);
    nx = (XSConn*)realloc(g_xs, (size_t)(g_nxs + 1) * sizeof(XSConn));
    if (nx) { g_xs = nx; g_xs[g_nxs].handle = h; g_xs[g_nxs].obj = self->nk == NK_SUPER ? self->nat.sup.self : self; g_nxs++; }
    old = d->handle; d->handle = h;
    if (old) { sock_close(vm->player, old); xs_forget(old); d->n = 0; }
    return v_bool(1);
}

static Value xs_send(Act *a, AObj *self, int argc, Value *argv) {
    XSData *d = xs_of(self); AStr *s; const char *u; size_t n; u8 *b;
    if (!d || !d->handle) return v_undef();
    s = v_str_of(a, arg(argc, argv, 0)); if (a->vm->err) return v_undef();
    d = xs_of(self); if (!d || !d->handle) return v_undef();
    u = str_c(a->vm, s); n = strlen(u);
    b = (u8*)malloc(n + 1); if (!b) return v_undef();
    memcpy(b, u, n); b[n] = 0;
    sock_send(a->vm->player, d->handle, b, n + 1);
    free(b);
    return v_undef();
}

static Value xs_on_data(Act *a, AObj *self, int argc, Value *argv) {
    Value x = o_construct(a, PROTOS(a)->xml_ctor, argc, argv);
    if (a->vm->err) { a->vm->err = 0; return v_undef(); }
    o_call_method(a, self, str_intern(a->vm, "onXML"), 1, &x, ER_SPECIAL);
    return v_undef();
}
static Value xs_noop(Act *a, AObj *self, int argc, Value *argv) { (void)a; (void)self; (void)argc; (void)argv; return v_undef(); }

/* Sockets::update_sockets */
void avm1_sockets_update(VM *vm) {
    SockEvent ev; Player *p = vm->player;
    while (sock_next_event(p, &ev)) {
        AObj *o = xs_find(ev.handle); XSData *d = o ? xs_of(o) : NULL; Act a;
        if (!o || !d) { free(ev.data); continue; }
        act_init_nothing(&a, vm, p->root, "[XMLSocket]");
        if (ev.type == SOCK_EV_CONNECT) {
            Value v = v_bool(ev.ok);
            o_call_method(&a, o, str_intern(vm, "onConnect"), 1, &v, ER_SPECIAL); vm->err = 0;
        } else if (ev.type == SOCK_EV_DATA) {
            int has_null = memchr(ev.data, 0, ev.n) != NULL; u8 *nb = (u8*)realloc(d->buf, d->n + ev.n + 1);
            if (nb) { d->buf = nb; memcpy(d->buf + d->n, ev.data, ev.n); d->n += ev.n; }
            while (has_null) {
                u8 *z; size_t l; AStr *msg; Value v;
                d = xs_of(o); if (!d || !d->buf) break;
                z = (u8*)memchr(d->buf, 0, d->n);
                if (!z) break;
                l = (size_t)(z - d->buf);
                msg = str_utf8(vm, (const char*)d->buf, l);
                memmove(d->buf, z + 1, d->n - l - 1); d->n -= l + 1;
                v = v_str(msg);
                o_call_method(&a, o, vm->s_onData, 1, &v, ER_SPECIAL); vm->err = 0;
            }
        } else {
            xs_forget(ev.handle);
            sock_close(p, ev.handle);
            d->handle = 0; d->n = 0;
            o_call_method(&a, o, str_intern(vm, "onClose"), 0, NULL, ER_SPECIAL); vm->err = 0;
        }
        free(ev.data);
        avm1_run_actions(vm);
    }
}
void avm1_sockets_pump(VM *vm) { sock_pump(vm->player); }
void avm1_sockets_mark(VM *vm) { int i; for (i = 0; i < g_nxs; i++) gc_mark_obj(vm, g_xs[i].obj); }
void avm1_sockets_free(VM *vm) { sock_free_all(vm->player); free(g_xs); g_xs = NULL; g_nxs = 0; }

AObj *lib_xmlsocket(VM *vm, Env *env, AObj *object_proto) {
    AObj *proto, *c = lib_class(vm, env, NULL, xs_ctor, object_proto, 0, &proto);
    env->p.xmlsocket = proto;
    o_add_property(vm, proto, str_intern(vm, "timeout"), fn_native(vm, env, xs_get_timeout, NULL), fn_native(vm, env, xs_set_timeout, NULL), 0);
    defm(vm, env, proto, "close", xs_close, 0);
    defm(vm, env, proto, "connect", xs_connect, 0);
    defm(vm, env, proto, "send", xs_send, 0);
    defm(vm, env, proto, "onConnect", xs_noop, DE | DD);
    defm(vm, env, proto, "onClose", xs_noop, DE | DD);
    defm(vm, env, proto, "onData", xs_on_data, DE | DD);
    defm(vm, env, proto, "onXML", xs_noop, DE | DD);
    return c;
}
