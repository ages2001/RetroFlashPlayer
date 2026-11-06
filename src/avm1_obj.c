/* avm1_obj.c - AVM1 objects: ordered property maps, attributes, virtual
 * properties, watchers, prototype search, functions, super objects, GC. */
#include "avm1_int.h"
#include "audio.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
int devfont_linux_rules(void);
int avm1_linux_ns_cat(void);
void avm1_prop_to_front(VM *vm, AObj *o, AStr *k);

/* ---------------------------------------------------------------- */
/* property map                                                     */

static void pm_reindex(PropMap *m, int min_cap) {
    int cap = m->icap ? m->icap : 16, i;
    while (cap < min_cap * 2) cap *= 2;
    if (cap != m->icap) { int *ni = (int*)malloc((size_t)cap * sizeof(int)); if (!ni) return; free(m->index); m->index = ni; m->icap = cap; }
    memset(m->index, 0, (size_t)m->icap * sizeof(int));
    for (i = 0; i < m->n; i++) {
        u32 j;
        if (!m->items[i].key) continue;
        j = m->items[i].key->hash & (u32)(m->icap - 1);
        while (m->index[j]) j = (j + 1) & (u32)(m->icap - 1);
        m->index[j] = i + 1;
    }
}

static void pm_compact(PropMap *m) {
    int i, k = 0;
    for (i = 0; i < m->n; i++) if (m->items[i].key) m->items[k++] = m->items[i];
    m->n = k;
    pm_reindex(m, m->n + 1);
}

Prop *pm_get(PropMap *m, const AStr *key, int cs) {
    u32 j;
    if (!m->icap) return NULL;
    j = key->hash & (u32)(m->icap - 1);
    while (m->index[j]) {
        Prop *p = &m->items[m->index[j] - 1];
        if (p->key && p->key->hash == key->hash && (cs ? str_eq(p->key, key) : str_eq_ci(p->key, key))) return p;
        j = (j + 1) & (u32)(m->icap - 1);
    }
    return NULL;
}

Prop *pm_insert(VM *vm, PropMap *m, AStr *key, int cs) {
    Prop *p = pm_get(m, key, cs);
    if (p) return p;
    if (m->n == m->cap) {
        int nc = m->cap ? m->cap * 2 : 8; Prop *ni;
        if (m->n > m->live * 2 && m->n > 8) { pm_compact(m); return pm_insert(vm, m, key, cs); }
        ni = (Prop*)realloc(m->items, (size_t)nc * sizeof(Prop));
        if (!ni) return NULL;
        m->items = ni; m->cap = nc;
    }
    p = &m->items[m->n++];
    memset(p, 0, sizeof(*p));
    p->key = key; p->v = v_undef(); p->id = ++vm->next_prop_id;
    m->live++;
    if (m->n * 2 > m->icap) pm_reindex(m, m->n);
    else {
        u32 j = key->hash & (u32)(m->icap - 1);
        while (m->index[j]) j = (j + 1) & (u32)(m->icap - 1);
        m->index[j] = m->n;
    }
    return p;
}

int pm_remove(PropMap *m, const AStr *key, int cs) {
    Prop *p = pm_get(m, key, cs);
    if (!p) return 0;
    p->key = NULL; p->v = v_undef(); p->getter = p->setter = NULL;
    m->live--;
    if (m->n > 16 && m->live * 2 < m->n) pm_compact(m);
    return 1;
}

void pm_free(PropMap *m) { free(m->items); free(m->index); memset(m, 0, sizeof(*m)); }

/* ---------------------------------------------------------------- */
/* creation                                                         */

AObj *obj_bare(VM *vm) {
    AObj *o = (AObj*)gc_alloc(vm, sizeof(AObj), GC_OBJ);
    return o;
}

AObj *obj_new_v(VM *vm, Value proto) {
    AObj *o = obj_bare(vm);
    if (o) o_define(vm, o, vm->s_proto, proto, PA_DONT_ENUM | PA_DONT_DELETE);
    return o;
}

AObj *obj_new(VM *vm, AObj *proto) {
    AObj *o = obj_bare(vm);
    if (o && proto) o_define(vm, o, vm->s_proto, v_obj(proto), PA_DONT_ENUM | PA_DONT_DELETE);
    return o;
}

/* Object::set_native: the object that takes a native payload (NULL: it already has one) */
AObj *o_native_target(AObj *o) {
    if (o && o->nk == NK_SUPER) o = o->nat.sup.self;
    if (!o || o->nk != NK_NONE) return NULL;
    return o;
}

/* a native constructor (Array, Date, Boolean, Number, String, TextField: nk) run on an
   object that already has a native type (super() with another __constructor__):
   Flash 9-12 and 14 retype it, Flash 16 retypes only arrays (not into a TextField),
   the other releases keep the first type */
AObj *o_native_retarget(AObj *o, int nk) {
    int v;
    if (o && o->nk == NK_SUPER) o = o->nat.sup.self;
    if (!o) return NULL;
    if (o->nk == NK_NONE) return o;
    if (o->nk != NK_ARRAY && o->nk != NK_DATE && o->nk != NK_BOOL && o->nk != NK_NUMBER && o->nk != NK_STRING) return NULL;
    v = avm1_player_major();
    if (v <= 12 || v == 14 || (v == 16 && o->nk == NK_ARRAY && nk != NK_EDITTEXT)) {
        obj_free_payload(o); o->nk = NK_NONE; o->nat.p = NULL;
        return o;
    }
    return NULL;
}

/* A movie clip keeps its clip data and can take a Date, Boolean, Number or
   String as separate "relay" data (Gnash MovieClip.as: mc.getTime() after
   super() with Date as __constructor__). Flash 9-12 and 14 replace the relay,
   13 and 15-22 keep the first one, 23 and later take no Date. */
AObj *o_native_relay(Act *a, AObj *o, int nk) {
    AObj *t = o_native_retarget(o, nk); int v;
    if (t) return t;
    if (o && o->nk == NK_SUPER) o = o->nat.sup.self;
    if (!o || o->nk != NK_MOVIECLIP) return NULL;
    v = avm1_player_major();
    if (o->relay && !(v <= 12 || v == 14)) return NULL;
    if (!o->relay && v >= 23 && nk == NK_DATE) return NULL;
    if (!o->relay) { o->relay = obj_bare(a->vm); if (!o->relay) return NULL; }
    t = o->relay;
    obj_free_payload(t); t->nk = NK_NONE; t->nat.p = NULL;
    return t;
}

void obj_free_payload(AObj *o) {
    if (o->nfree) o->nfree(o);
    o->nfree = NULL;
}

/* ---------------------------------------------------------------- */
/* attributes                                                       */

static const u16 version_masks[10] = {
    0x7FF8, 0x7FF8, 0x7FF8, 0x7FF8, 0x7FF8, 0x7480, 0x7500, 0x7000, 0x6000, 0x4000
};
static int allow_version(const Prop *p, int v) {
    u16 mask = v < 10 ? version_masks[v] : 0;
    return (p->attr & mask) == 0;
}

/* a write makes the property visible again; SWF6 movies keep the SWF7 bit (0x400) */
static void prop_set_data(Prop *p, Value v, int swf_version) {
    if (!(p->attr & PA_READ_ONLY)) { p->v = v; p->attr &= (u16)~(swf_version == 6 ? (PA_VERSION_MASK & ~0x0400) : PA_VERSION_MASK); }
}

/* ---------------------------------------------------------------- */
/* natives                                                          */

AObj *o_super_self(AObj *o) { return o->nk == NK_SUPER ? o->nat.sup.self : NULL; }

AObj *o_as_function(AObj *o) { return o && o->nk == NK_FUNCTION ? o : NULL; }

MovieClip *o_display_no_super(AObj *o) {
    if (!o || !OBJ_IS_DISPLAY(o)) return NULL;
    return o->nat.clip;
}
MovieClip *o_display(AObj *o) {
    if (o && o->nk == NK_SUPER) o = o->nat.sup.self;
    return o_display_no_super(o);
}

/* ---------------------------------------------------------------- */
/* raw data                                                         */

int o_get_data(Act *a, AObj *o, AStr *name, Value *out) {
    Prop *p = pm_get(&o->props, name, CASE_SENSITIVE(a));
    if (!p) return 0;
    *out = p->v;
    return 1;
}

void o_set_data(Act *a, AObj *o, AStr *name, Value v) {
    Prop *p = pm_get(&o->props, name, CASE_SENSITIVE(a));
    if (p) { prop_set_data(p, v, a->swf_version); return; }
    p = pm_insert(a->vm, &o->props, name, CASE_SENSITIVE(a));
    if (p) p->v = v;
}

void o_define(VM *vm, AObj *o, AStr *name, Value v, u16 attr) {
    Prop *p;
    if (o->nk == NK_SUPER) return;
    p = pm_get(&o->props, name, 1);
    if (p) { p->v = v; p->getter = p->setter = NULL; p->attr = attr; p->id = ++vm->next_prop_id; return; }
    p = pm_insert(vm, &o->props, name, 1);
    if (p) { p->v = v; p->attr = attr; }
}

void o_define_c(VM *vm, AObj *o, const char *name, Value v, u16 attr) { o_define(vm, o, str_intern(vm, name), v, attr); }

void o_add_property(VM *vm, AObj *o, AStr *name, AObj *getter, AObj *setter, u16 attr) {
    Prop *p;
    if (o->nk == NK_SUPER) return;
    p = pm_get(&o->props, name, 0);
    if (p) { p->getter = getter; p->setter = setter; return; }
    p = pm_insert(vm, &o->props, name, 0);
    if (p) { p->getter = getter; p->setter = setter; p->attr = attr; }
}

static int call_watcher(Act *a, AObj *o, AStr *name, Value *value, AObj *self);
static int parse_array_index(const AStr *s, s32 *out, int frac);
static Watch *find_watch(Act *a, AObj *o, AStr *name);
void o_add_property_case(Act *a, AObj *o, AStr *name, AObj *getter, AObj *setter, u16 attr) {
    Prop *p; Value u = v_undef(); int watched;
    if (o->nk == NK_SUPER) return;
    p = pm_get(&o->props, name, CASE_SENSITIVE(a));
    if (p) { p->getter = getter; p->setter = setter; return; }
    /* a new getter-setter on a watched name: the watcher gives its underlying value */
    watched = find_watch(a, o, name) != NULL;
    if (watched) { int err = call_watcher(a, o, name, &u, o); if (err) { a->vm->err = err; return; } if (a->vm->err) return; }
    p = pm_insert(a->vm, &o->props, name, CASE_SENSITIVE(a));
    if (p) { p->getter = getter; p->setter = setter; p->attr = attr; p->v = u; }
    /* an index property of an array extends its length */
    if (o->nk == NK_ARRAY) {
        s32 index;
        if (parse_array_index(name, &index, a->swf_version < 7) && index >= o_length(a, o)) o_set_length(a, o, (int)((u32)index + 1u));
    }
}

void o_set_attributes(VM *vm, AObj *o, AStr *name, u16 set, u16 clear) {
    int i;
    (void)vm;
    if (o->nk == NK_SUPER) return;
    if (!name) {
        for (i = 0; i < o->props.n; i++) if (o->props.items[i].key) o->props.items[i].attr = (u16)((o->props.items[i].attr & ~clear) | set);
        return;
    }
    { Prop *p = pm_get(&o->props, name, 0); if (p) p->attr = (u16)((p->attr & ~clear) | set); }
}

/* ---------------------------------------------------------------- */
/* prototype chain                                                  */

Value o_proto(Act *a, AObj *o) {
    Value v;
    if (o->nk == NK_SUPER) {
        AObj *base = o->nat.sup.self; int i;
        for (i = 0; i < o->nat.sup.depth && base; i++) { Value p = o_proto(a, base); base = p.t == V_OBJ ? p.u.o : NULL; }
        if (!base) return v_undef();
        return o_proto(a, base);
    }
    /* a version-flagged __proto__ (functions) only hides the chain from SWF5 movies */
    { Prop *p = pm_get(&o->props, a->vm->s_proto, CASE_SENSITIVE(a));
      if (p && (p->attr & PA_PROTO_SWF7) && a->swf_version < 7) return v_undef();   /* Linux player: chains SWF6 movies do not see */
      if (p && (a->swf_version >= 6 || allow_version(p, a->swf_version))) return p->v; }
    (void)v;
    return v_undef();
}

int o_prototype(Act *a, AObj *o, Value *out) { return o_get_data(a, o, a->vm->s_prototype, out); }

int o_get_local_stored(Act *a, AObj *o, AStr *name, Value *out) {
    Prop *p; MovieClip *d;
    if (o->nk == NK_SUPER) return 0;
    p = pm_get(&o->props, name, CASE_SENSITIVE(a));
    if (p && allow_version(p, a->swf_version)) { *out = p->v; return 1; }
    if (o->nk == NK_SOUND) { extern int avm1_sound_virtual(Act *a, AObj *o, AStr *name, Value *out); return avm1_sound_virtual(a, o, name, out); }
    d = o_display_no_super(o);
    if (d && !clip_removed(d)) return stage_get_property(a, d, name, out);
    return 0;
}

AObj *a1_resolve_class(Act *a, const char *path) {
    AObj *o = env_get(a->vm, a->swf_version)->global;
    char seg[64];
    while (o && *path) {
        size_t n = 0; Prop *p;
        while (path[n] && path[n] != '.' && n < sizeof(seg) - 1) { seg[n] = path[n]; n++; }
        seg[n] = 0; path += n; if (*path == '.') path++;
        if (o->nk == NK_SUPER) return NULL;
        p = pm_get(&o->props, str_intern(a->vm, seg), CASE_SENSITIVE(a));
        if (!p || p->v.t != V_OBJ) return NULL;
        o = p->v.u.o;
    }
    return o;
}
int a1_class_proto(Act *a, AObj *c, Value *out) {
    Prop *p;
    if (!c || c->nk == NK_SUPER) return 0;
    p = pm_get(&c->props, a->vm->s_prototype, CASE_SENSITIVE(a));
    if (!p) return 0;
    *out = p->v;
    return 1;
}
int a1_resolve_proto(Act *a, const char *path, Value *out) {
    AObj *c = a1_resolve_class(a, path); Prop *p;
    if (!c || c->nk == NK_SUPER) return 0;
    p = pm_get(&c->props, a->vm->s_prototype, CASE_SENSITIVE(a));
    if (!p) return 0;
    *out = p->v;
    return 1;
}
AObj *a1_new_with_proto(Act *a, const char *path) {
    Value pv;
    if (a1_resolve_proto(a, path, &pv)) return obj_new_v(a->vm, pv);
    return obj_bare(a->vm);
}
Value a1_instantiate_as_script(Act *a, const char *path, int argc, Value *argv) {
    AObj *o = env_get(a->vm, a->swf_version)->global;
    char seg[64];
    while (*path) {
        size_t n = 0; Value v;
        while (path[n] && path[n] != '.' && n < sizeof(seg) - 1) { seg[n] = path[n]; n++; }
        seg[n] = 0; path += n; if (*path == '.') path++;
        v = o_get(a, o, str_intern(a->vm, seg));
        if (a->vm->err) return v_undef();
        o = v_to_obj_or_bare(a, v);
        if (a->vm->err || !o) return v_undef();
    }
    return o_construct(a, o, argc, argv);
}

/* SWF5/6: while a property's getter, setter or watcher runs, the property is plain
   data to the code it calls (SWF7+ recurse up to the property recursion limit) */
static int prop_guarded(Act *a, const Prop *p) {
    const ActId *id;
    if (a->swf_version >= 7 || !p || !p->id) return 0;
    for (id = &a->id; id; id = id->parent) if (id->reason == ER_PROPERTY && id->property_id == p->id) return 1;
    return 0;
}

static int call_getter(Act *a, AObj *o, AStr *name, Value self, Value *out) {
    Prop *p;
    if (o->nk == NK_SUPER) return call_getter(a, o->nat.sup.self, name, self, out);
    p = pm_get(&o->props, name, CASE_SENSITIVE(a));
    if (!p || !allow_version(p, a->swf_version) || !p->getter || !o_as_function(p->getter)) return 0;
    if (prop_guarded(a, p)) { *out = p->v; if (out->t == V_UNDEF) out->u.b = A1_UNSET_MARK; return 1; }
    *out = fn_exec(a, p->getter, self, 1, 0, NULL, ER_PROPERTY, p->id);
    return 1;
}

/* an inherited setter hidden by the "ignore in SWF6" (0x100) or SWF7 (0x400) bits still
 * runs; the SWF8+ bits hide it */
static int setter_visible(const Prop *p, int v) {
    u16 mask = v < 10 ? version_masks[v] : 0;
    return (p->attr & mask & ~0x0500) == 0;
}
static int call_setter(Act *a, AObj *o, AStr *name, Value self, Value v) {
    Prop *p;
    if (o->nk == NK_SUPER) return call_setter(a, o->nat.sup.self, name, self, v);
    p = pm_get(&o->props, name, CASE_SENSITIVE(a));
    if (!p || !setter_visible(p, a->swf_version) || !p->setter || !o_as_function(p->setter)) return 0;
    fn_exec(a, p->setter, self, 1, 1, &v, ER_PROPERTY, p->id);
    return 1;
}

static AObj *find_resolve(Act *a, Value proto) {
    int depth = 0;
    while (proto.t == V_OBJ) {
        Value r;
        if (depth == 255) { a->vm->err = ERR_PROTO_RECURSION; return NULL; }
        if (o_get_local_stored(a, proto.u.o, a->vm->s___resolve, &r) && r.t == V_OBJ) return r.u.o;
        proto = o_proto(a, proto.u.o);
        depth++;
    }
    return NULL;
}

int o_search_proto(Act *a, Value proto, AStr *name, AObj *self, int call_resolve, Value *out, int *depth_out) {
    VM *vm = a->vm; int depth = 0; Value orig = proto;
    while (proto.t == V_OBJ) {
        AObj *p = proto.u.o; Value v;
        if (depth == 255) { vm->err = ERR_PROTO_RECURSION; return 0; }
        /* display objects are not part of the prototype recursion */
        if (depth > 0 && OBJ_IS_DISPLAY(p)) break;
        if (call_getter(a, p, name, v_obj(self), &v)) {
            if (vm->err == ERR_PROP_RECURSION) { vm->err = ERR_NONE; }
            else if (vm->err == ERR_THROWN || vm->err == ERR_FUNC_RECURSION) return 0;
            else { if (vm->err) { vm->err = ERR_NONE; v = v_undef(); } *out = v; if (depth_out) *depth_out = depth; return 1; }
        }
        if (o_get_local_stored(a, p, name, &v)) { *out = v; if (depth_out) *depth_out = depth; return 1; }
        proto = o_proto(a, p);
        depth++;
    }
    if (call_resolve) {
        AObj *r = find_resolve(a, orig);
        if (vm->err) return 0;
        if (r) {
            Value nm = v_str(name);
            *out = o_call(a, r, v_obj(self), 1, &nm);
            if (depth_out) *depth_out = 0;
            return vm->err ? 0 : 1;
        }
    }
    return 0;
}

int o_get_opt(Act *a, AObj *o, AStr *name, int call_resolve, Value *out) {
    if (o->nk == NK_SUPER) {
        Value proto = o_proto(a, o);
        return o_search_proto(a, proto, name, o->nat.sup.self, call_resolve, out, NULL);
    }
    return o_search_proto(a, v_obj(o), name, o, call_resolve, out, NULL);
}

Value o_get(Act *a, AObj *o, AStr *name) {
    Value v;
    if (!o_get_opt(a, o, name, 1, &v)) return v_undef();
    return v;
}

Value o_get_stored(Act *a, AObj *o, AStr *name) {
    int depth = 0; Value proto = v_obj(o), v;
    while (proto.t == V_OBJ) {
        if (depth == 255) { a->vm->err = ERR_PROTO_RECURSION; return v_undef(); }
        if (o_get_local_stored(a, proto.u.o, name, &v)) return v;
        proto = o_proto(a, proto.u.o);
        depth++;
    }
    return v_undef();
}

int o_has_own_property(Act *a, AObj *o, AStr *name) {
    if (o->nk == NK_SUPER) return o_has_own_property(a, o->nat.sup.self, name);
    return pm_get(&o->props, name, CASE_SENSITIVE(a)) != NULL;
}

int o_has_own_virtual(Act *a, AObj *o, AStr *name) {
    Prop *p;
    if (o->nk == NK_SUPER) return o_has_own_virtual(a, o->nat.sup.self, name);
    p = pm_get(&o->props, name, CASE_SENSITIVE(a));
    return p && p->getter && allow_version(p, a->swf_version);
}

int o_is_enumerable(Act *a, AObj *o, AStr *name) {
    Prop *p;
    if (o->nk == NK_SUPER) return o_is_enumerable(a, o->nat.sup.self, name);
    p = pm_get(&o->props, name, CASE_SENSITIVE(a));
    return p && !(p->attr & PA_DONT_ENUM);
}

int o_has_property(Act *a, AObj *o, AStr *name) {
    MovieClip *d; int guard = 0;
    if (o->nk == NK_SUPER) return o_has_property(a, o->nat.sup.self, name);
    d = o_display_no_super(o);
    if (!d || !clip_removed(d)) {
        AObj *p = o;
        while (p && guard++ < 256) {
            Value pr;
            if (o_has_own_property(a, p, name)) return 1;
            pr = o_proto(a, p);
            p = pr.t == V_OBJ ? pr.u.o : NULL;
            if (p && p->nk == NK_SUPER) break;
        }
    }
    return d && !clip_removed(d) && stage_has_property(a, d, name);
}

int o_delete(Act *a, AObj *o, AStr *name) {
    Prop *p;
    if (o->nk == NK_SUPER) return 0;
    p = pm_get(&o->props, name, CASE_SENSITIVE(a));
    if (!p || (p->attr & PA_DONT_DELETE)) return 0;
    return pm_remove(&o->props, name, CASE_SENSITIVE(a));
}

/* ---------------------------------------------------------------- */
/* watchers                                                         */

static Watch *find_watch(Act *a, AObj *o, AStr *name) {
    int i;
    for (i = 0; i < o->nwatch; i++) if (str_eq_case(o->watch[i].name, name, CASE_SENSITIVE(a))) return &o->watch[i];
    return NULL;
}

int o_watch(Act *a, AObj *o, AStr *name, AObj *cb, Value user) {
    Watch *w;
    if (o->nk == NK_SUPER) return 0;
    w = find_watch(a, o, name);
    if (!w) {
        Watch *nw = (Watch*)realloc(o->watch, (size_t)(o->nwatch + 1) * sizeof(Watch));
        if (!nw) return 0;
        o->watch = nw; w = &o->watch[o->nwatch++];
    }
    w->name = name; w->cb = cb; w->user = user;
    return 1;
}

int o_unwatch(Act *a, AObj *o, AStr *name) {
    Watch *w; Prop *p;
    if (o->nk == NK_SUPER) return 0;
    /* a getter-setter cannot be unwatched */
    p = pm_get(&o->props, name, CASE_SENSITIVE(a));
    if (p && p->getter) return 0;
    w = find_watch(a, o, name);
    if (!w) return 0;
    *w = o->watch[--o->nwatch];
    return 1;
}

/* returns an error state to report after the set (0 none) */
static int call_watcher(Act *a, AObj *o, AStr *name, Value *value, AObj *self) {
    VM *vm = a->vm; Watch *w; Prop *p; u32 id; Value old, args[4], r; AObj *cb; Value user; int err = 0; Value thrown = v_undef();
    if (o->nk == NK_SUPER) return call_watcher(a, o->nat.sup.self, name, value, self);
    w = find_watch(a, o, name);
    if (!w) return 0;
    cb = w->cb; user = w->user;
    p = pm_get(&o->props, name, CASE_SENSITIVE(a));
    if (prop_guarded(a, p)) return 0;
    id = p ? p->id : 0;
    old = o_get_stored(a, o, name);
    if (vm->err) return 0;
    args[0] = v_str(name); args[1] = old; args[2] = *value; args[3] = user;
    if (!o_as_function(cb)) return 0;
    r = fn_exec(a, cb, v_obj(self), 0, 4, args, ER_PROPERTY, id);
    if (!vm->err) *value = r;
    else if (vm->err == ERR_THROWN) { *value = v_undef(); err = ERR_THROWN; thrown = vm->thrown; vm->err = 0; }
    else if (vm->err == ERR_PROP_RECURSION) vm->err = 0;
    else { err = vm->err; vm->err = 0; }
    if (err) vm->thrown = thrown;
    return err;
}

/* ---------------------------------------------------------------- */
/* set                                                              */

/* Ruffle parse_array_index: leading ASCII whitespace, optional sign, digits, wrapping */
/* (SWF5/6 also take a decimal fraction: a[2.2] counts as index 2) */
static int parse_array_index(const AStr *s, s32 *out, int frac) {
    u32 i = 0; u32 acc = 0; int neg = 0;
    while (i < s->len) { u16 c = STR_AT(s, i); if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 12) i++; else break; }
    if (i < s->len && (STR_AT(s, i) == '-' || STR_AT(s, i) == '+')) { neg = STR_AT(s, i) == '-'; i++; }
    if (i >= s->len) return 0;
    for (; i < s->len; i++) {
        u16 c = STR_AT(s, i);
        if (c == '.' && frac && i + 1 < s->len) { u32 k; for (k = i + 1; k < s->len; k++) if (STR_AT(s, k) < '0' || STR_AT(s, k) > '9') return 0; break; }
        if (c < '0' || c > '9') return 0;
        acc = acc * 10u + (u32)(c - '0');
    }
    *out = (s32)(neg ? (u32)(0u - acc) : acc);
    return 1;
}

void o_set_local(Act *a, AObj *o, AStr *name, Value v, AObj *self) {
    VM *vm = a->vm; Prop *p; MovieClip *d;
    if (o->nk == NK_SUPER) return;
    if (o->nk == NK_ARRAY) {
        s32 index;
        if (str_eq(name, vm->s_length)) {
            s32 nl = v_i32(a, v); Value ol;
            if (vm->err) return;
            if (o_get_data(a, o, vm->s_length, &ol) && ol.t == V_NUM) {
                s32 i, old = f64_i32(ol.u.n);
                for (i = nl > 0 ? nl : 0; i < old; i++) o_delete_element(a, o, i);
            }
        } else if (parse_array_index(name, &index, a->swf_version < 7)) {
            int len = o_length(a, o);
            if (vm->err) return;
            if (index >= len) o_set_length(a, o, (int)((u32)index + 1u));
        }
    } else if ((d = o_display_no_super(o)) != NULL) {
        stage_notify_change(a, d, name, v);
        if (vm->err) return;
        if (!o_has_own_property(a, o, name) && stage_set_property(a, d, name, v)) return;
        if (vm->err) return;
    }
    p = pm_get(&o->props, name, CASE_SENSITIVE(a));
    if (p && p->setter && o_as_function(p->setter) && !prop_guarded(a, p)) {
        AObj *setter = p->setter; u32 id = p->id;
        fn_exec(a, setter, v_obj(self), 1, 1, &v, ER_PROPERTY, id);
        if (vm->err == ERR_THROWN) return;
        vm->err = 0;
        p = pm_get(&o->props, name, CASE_SENSITIVE(a));
    }
    if (p) prop_set_data(p, v, a->swf_version);
    else { p = pm_insert(vm, &o->props, name, CASE_SENSITIVE(a)); if (p) p->v = v; }
}

void o_set(Act *a, AObj *o, AStr *name, Value v) {
    VM *vm = a->vm; AObj *self; Value proto; int werr; Value wthrown;
    if (name->len == 0) return;
    if (o->nk == NK_SUPER) { self = o->nat.sup.self; proto = o_proto(a, o); }
    else { self = o; proto = v_obj(o); }
    werr = call_watcher(a, o, name, &v, self);
    wthrown = vm->thrown;
    if (vm->err) return;
    if (!o_has_own_property(a, o, name)) {
        int guard = 0;
        while (proto.t == V_OBJ && guard++ < 256) {
            AObj *tp = proto.u.o;
            Prop *vp = tp->nk == NK_SUPER ? NULL : pm_get(&tp->props, name, CASE_SENSITIVE(a));
            if (vp ? vp->getter != NULL && setter_visible(vp, a->swf_version) : o_has_own_virtual(a, tp, name)) {
                call_setter(a, tp, name, v_obj(self), v);
                if (vm->err == ERR_PROP_RECURSION) vm->err = 0;
                if (!vm->err && werr) { vm->err = werr; vm->thrown = wthrown; }
                return;
            }
            proto = o_proto(a, tp);
        }
    }
    o_set_local(a, o, name, v, self);
    if (!vm->err && werr) { vm->err = werr; vm->thrown = wthrown; }
}

/* ---------------------------------------------------------------- */
/* keys                                                             */

static void keys_push(AStr ***keys, int *n, int *cap, AStr *k) {
    if (*n == *cap) { int nc = *cap ? *cap * 2 : 16; AStr **nk = (AStr**)realloc(*keys, (size_t)nc * sizeof(AStr*)); if (!nk) return; *keys = nk; *cap = nc; }
    (*keys)[(*n)++] = k;
}

AStr **o_keys(Act *a, AObj *o, int include_hidden, int *n_out) {
    AStr **keys = NULL; int n = 0, cap = 0, i; Value proto; MovieClip *d;
    *n_out = 0;
    if (o->nk == NK_SUPER) return NULL;
    proto = o_proto(a, o);
    if (proto.t == V_OBJ && proto.u.o != o) {
        int pn, lin_rules = devfont_linux_rules(); AStr **pk;
        static int depth = 0;
        if (depth > 255) return NULL;
        depth++;
        pk = o_keys(a, proto.u.o, include_hidden, &pn);
        depth--;
        /* the Linux player: a hidden member does not hide an enumerable one further up the chain */
        for (i = 0; i < pn; i++) {
            Prop *own = pm_get(&o->props, pk[i], CASE_SENSITIVE(a));
            if (!own || (lin_rules && !include_hidden && pk[i] != a->vm->s_proto && (own->attr & (PA_DONT_ENUM | PA_NEVER_ENUM)))) keys_push(&keys, &n, &cap, pk[i]);
        }
        free(pk);
    }
    for (i = o->props.n - 1; i >= 0; i--) {
        Prop *p = &o->props.items[i];
        if (!p->key) continue;
        if ((p->attr & PA_NEVER_ENUM) || ((p->attr & PA_ENUM_BY_VERSION) && !allow_version(p, a->swf_version))) continue;
        /* the Linux player 13+ does not enumerate the length of an array or a string */
        if ((o->nk == NK_ARRAY || o->nk == NK_STRING) && p->key == a->vm->s_length && devfont_linux_rules() && avm1_linux_ns_cat() >= 3) continue;
        if (include_hidden || !(p->attr & PA_DONT_ENUM)) keys_push(&keys, &n, &cap, p->key);
    }
    d = o_display_no_super(o);
    if (d && !clip_removed(d)) stage_enumerate(a, d, &keys, &n, &cap);
    *n_out = n;
    return keys;
}

/* ---------------------------------------------------------------- */
/* interfaces / instanceof                                          */

void o_set_interfaces(VM *vm, AObj *o, AObj **list, int n) {
    (void)vm;
    if (o->nk == NK_SUPER || o->has_ifaces) return;
    o->ifaces = (AObj**)malloc((size_t)(n ? n : 1) * sizeof(AObj*));
    if (!o->ifaces) return;
    memcpy(o->ifaces, list, (size_t)n * sizeof(AObj*));
    o->nifaces = n; o->has_ifaces = 1;
}

int o_is_instance_of(Act *a, AObj *o, AObj *proto) {
    AObj *stack[256]; int sp; AObj *self = o; int guard = 0;
    for (;;) {
        Value p = o_proto(a, self);
        if (p.t != V_OBJ || guard++ > 255) break;
        sp = 0; stack[sp++] = p.u.o;
        while (sp > 0) {
            AObj *it = stack[--sp]; int k;
            if (it == proto) return 1;
            if (it->nk != NK_SUPER) for (k = 0; k < it->nifaces && sp < 256; k++) stack[sp++] = it->ifaces[k];
        }
        self = p.u.o;
    }
    return 0;
}

int o_is_prototype_of(Act *a, AObj *o, AObj *other) {
    Value p = o_proto(a, other); int guard = 0;
    while (p.t == V_OBJ && guard++ < 256) {
        if (p.u.o == o) return 1;
        p = o_proto(a, p.u.o);
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* array-like helpers                                               */

/* Ruffle / Flash: array-like access goes through the raw own data */
int o_length(Act *a, AObj *o) {
    Value v;
    if (o->nk == NK_SUPER) return 0;
    if (!o_get_data(a, o, a->vm->s_length, &v)) v = v_undef();
    return (int)v_i32(a, v);
}

void o_set_length(Act *a, AObj *o, int n) {
    Value old;
    if (o->nk == NK_SUPER) return;
    if (o->nk == NK_ARRAY && o_get_data(a, o, a->vm->s_length, &old) && old.t == V_NUM) {
        s32 i, end = f64_i32(old.u.n);
        for (i = n > 0 ? n : 0; i < end; i++) o_delete_element(a, o, i);
    }
    o_set_data(a, o, a->vm->s_length, v_num(n));
}

Value o_get_element(Act *a, AObj *o, int i) {
    Value v;
    if (o->nk == NK_SUPER) return v_undef();
    if (!o_get_data(a, o, str_int(a->vm, i), &v)) return v_undef();
    return v;
}
void o_set_element(Act *a, AObj *o, int i, Value v) {
    if (o->nk == NK_SUPER) return;
    if (o->nk == NK_ARRAY) {
        int len = o_length(a, o);
        if (a->vm->err) return;
        if (i >= len) o_set_length(a, o, (int)((u32)i + 1u));
    }
    o_set_data(a, o, str_int(a->vm, i), v);
}
void o_delete_element(Act *a, AObj *o, int i) { if (o->nk != NK_SUPER) o_delete(a, o, str_int(a->vm, i)); }
int o_has_element(Act *a, AObj *o, int i) { if (o->nk == NK_SUPER) return 0; return o_has_own_property(a, o, str_int(a->vm, i)); }

/* ---------------------------------------------------------------- */
/* functions                                                        */

/* a function's __proto__ is hidden from SWF5 movies (they see no Function methods) */
static AObj *fn_object(VM *vm, AFunc *f, Value fn_proto, AObj *prototype) {
    AObj *o = obj_bare(vm);
    if (!o) return NULL;
    o_define(vm, o, vm->s_proto, fn_proto, PA_DONT_ENUM | PA_DONT_DELETE | PA_VERSION_6);
    o->nk = NK_FUNCTION; o->nat.fn = f;
    if (prototype) {
        o_define(vm, prototype, vm->s_constructor, v_obj(o), PA_DONT_ENUM);
        o_define(vm, o, vm->s_prototype, v_obj(prototype), PA_DONT_ENUM | PA_DONT_DELETE);
    }
    return o;
}

/* native functions carry their own `constructor` (Function, defined before __proto__),
   which SWF5 movies see although __proto__ is hidden from them.  Only done for the SWF5/6
   globals: Flash does it for SWF7+ too, but the SWF7+ globals still follow Ruffle's
   declarations (global_proto_decls) */
AObj *fn_native_ctor(VM *vm, Env *env, AObj *o) {
    Prop *pp; Value pv; u16 pa;
    if (!o || !env->p.function_ctor || env != &vm->env[0] || pm_get(&o->props, vm->s_constructor, 1)) return o;
    pp = pm_get(&o->props, vm->s_proto, 1);
    if (pp) { pv = pp->v; pa = pp->attr; pm_remove(&o->props, vm->s_proto, 1); }
    o_define(vm, o, vm->s_constructor, v_obj(env->p.function_ctor), PA_DONT_ENUM);
    if (pp) o_define(vm, o, vm->s_proto, pv, pa);
    return o;
}

AObj *fn_native(VM *vm, Env *env, NativeFn f, NativeFn ctor) {
    AFunc *fn = (AFunc*)gc_alloc(vm, sizeof(AFunc), GC_FN);
    if (!fn) return NULL;
    fn->kind = FK_NATIVE; fn->native = f; fn->ctor = ctor;
    return fn_native_ctor(vm, env, fn_object(vm, fn, env->p.function ? v_obj(env->p.function) : v_undef(), NULL));
}

AObj *fn_table(VM *vm, Env *env, TableFn f, int index) {
    AFunc *fn = (AFunc*)gc_alloc(vm, sizeof(AFunc), GC_FN);
    if (!fn) return NULL;
    fn->kind = FK_TABLE; fn->tnative = f; fn->index = index;
    return fn_native_ctor(vm, env, fn_object(vm, fn, env->p.function ? v_obj(env->p.function) : v_undef(), NULL));
}

/* class object: no __proto__ chain through Object::new; properties in Flash's order */
AObj *fn_class(VM *vm, Env *env, NativeFn ctor, NativeFn call, AObj *proto, int proto_first) {
    AFunc *fn = (AFunc*)gc_alloc(vm, sizeof(AFunc), GC_FN); AObj *o; u16 at = PA_DONT_ENUM | PA_DONT_DELETE;
    if (!fn) return NULL;
    fn->kind = FK_NATIVE; fn->native = call; fn->ctor = ctor;
    o = obj_bare(vm);
    if (!o) return NULL;
    o->nk = NK_FUNCTION; o->nat.fn = fn;
    if (proto_first) {
        o_define(vm, o, vm->s_prototype, v_obj(proto), at);
        o_define(vm, o, vm->s_constructor, env->p.function_ctor ? v_obj(env->p.function_ctor) : v_obj(env->p.object), at);
        o_define(vm, o, vm->s_proto, v_obj(env->p.function), at | PA_VERSION_6);
        o_define(vm, proto, vm->s_constructor, v_obj(o), at);
    } else {
        o_define(vm, o, vm->s_constructor, env->p.function_ctor ? v_obj(env->p.function_ctor) : v_obj(env->p.object), at);
        o_define(vm, o, vm->s_proto, v_obj(env->p.function), at | PA_VERSION_6);
        o_define(vm, proto, vm->s_constructor, v_obj(o), at);
        o_define(vm, o, vm->s_prototype, v_obj(proto), at);
    }
    return o;
}

AObj *obj_super(VM *vm, AObj *self, int depth) {
    AObj *o = obj_bare(vm);
    if (!o) return NULL;
    o->nk = NK_SUPER; o->nat.sup.self = self; o->nat.sup.depth = depth;
    return o;
}

/* run a bytecode function */
extern Value avm1_exec_action(Act *caller, AObj *callee, AFunc *f, Value self, int depth, int argc, Value *argv, int reason, u32 prop_id);

Value fn_exec(Act *a, AObj *callee, Value self, int depth, int argc, Value *argv, int reason, u32 prop_id) {
    AFunc *f = callee->nat.fn;
    switch (f->kind) {
    case FK_NATIVE: {
        /* a built-in function is arguments.caller of the functions it calls
           (Gnash MovieClip.as: MovieClip.meth), except Function.call/apply */
        extern int avm1_fn_is_call_apply(NativeFn f);
        AObj *t = v_to_obj_or_bare(a, self), *saved = a->callee; Value r;
        if (a->vm->err) return v_undef();
        if (!f->native) return v_undef();
        if (!avm1_fn_is_call_apply(f->native)) a->callee = callee;
        r = f->native(a, t, argc, argv);
        a->callee = saved;
        return r;
    }
    case FK_TABLE: {
        AObj *t = v_to_obj_or_bare(a, self), *saved = a->callee; Value r; int was = a->vm->table_ctor;
        if (a->vm->err) return v_undef();
        a->vm->table_ctor = 0;
        a->callee = callee;
        r = f->tnative(a, t, argc, argv, f->index);
        a->callee = saved;
        a->vm->table_ctor = was;
        return r;
    }
    default:
        return avm1_exec_action(a, callee, f, self, depth, argc, argv, reason, prop_id);
    }
}

Value fn_exec_ctor(Act *a, AObj *callee, Value self, int depth, int argc, Value *argv, int reason) {
    AFunc *f = callee->nat.fn;
    if (f->kind == FK_NATIVE && f->ctor) {
        AObj *t = v_to_obj_or_bare(a, self);
        if (a->vm->err) return v_undef();
        return f->ctor(a, t, argc, argv);
    }
    if (f->kind == FK_TABLE) {
        AObj *t = v_to_obj_or_bare(a, self); Value r; int was = a->vm->table_ctor;
        if (a->vm->err) return v_undef();
        a->vm->table_ctor = 1;
        r = f->tnative(a, t, argc, argv, f->index);
        a->vm->table_ctor = was;
        return r;
    }
    return fn_exec(a, callee, self, depth, argc, argv, reason, 0);
}

static Value super_call(Act *a, AObj *sup, int argc, Value *argv) {
    AObj *base = sup->nat.sup.self; int i; Value c;
    for (i = 0; i < sup->nat.sup.depth && base; i++) { Value p = o_proto(a, base); base = p.t == V_OBJ ? p.u.o : NULL; }
    if (!base) return v_undef();
    if (!o_get_opt(a, base, a->vm->s___constructor__, 0, &c) || c.t != V_OBJ) return v_undef();
    if (a->vm->err) return v_undef();
    {
        AObj *ctor = c.u.o;
        if (ctor->nk == NK_SUPER) ctor = ctor->nat.sup.self;
        if (!o_as_function(ctor)) return v_undef();
        return fn_exec_ctor(a, ctor, v_obj(sup->nat.sup.self), sup->nat.sup.depth + 1, argc, argv, ER_CONSTRUCTOR);
    }
}

Value o_call(Act *a, AObj *f, Value self, int argc, Value *argv) {
    if (f->nk == NK_SUPER) return super_call(a, f, argc, argv);
    if (f->nk == NK_FUNCTION) return fn_exec(a, f, self, 0, argc, argv, ER_FUNCTION, 0);
    return v_undef();
}

Value o_call_method(Act *a, AObj *o, AStr *name, int argc, Value *argv, int reason) {
    Value m; int depth = 0; MovieClip *d;
    if (o->nk == NK_SUPER) {
        AObj *self = o->nat.sup.self; Value proto = o_proto(a, o);
        if (!o_search_proto(a, proto, name, self, reason != ER_SPECIAL, &m, &depth) || m.t != V_OBJ) return v_undef();
        /* the new super base (Gnash as_super::get_super, matching Flash): SWF6 always
         * steps one prototype up; SWF7+ uses the object holding the method when it is
         * one step past the super prototype, else the object just before it */
        if (a->swf_version <= 6 || depth == 0) depth = o->nat.sup.depth + 1;
        else if (depth == 1) depth = o->nat.sup.depth + 2;
        else depth = o->nat.sup.depth + depth;
        if (o_as_function(m.u.o)) return fn_exec(a, m.u.o, v_obj(self), depth, argc, argv, reason, 0);
        return o_call(a, m.u.o, v_obj(self), argc, argv);
    }
    d = o_display_no_super(o);
    if (OBJ_IS_DISPLAY(o) && (!d || clip_removed(d))) return v_undef();
    if (!o_search_proto(a, v_obj(o), name, o, reason != ER_SPECIAL, &m, &depth) || m.t != V_OBJ) return v_undef();
    /* SWF6: super is always this.__proto__.__proto__, wherever the method was found */
    if (depth < 1 || a->swf_version <= 6) depth = 1;
    if (o_as_function(m.u.o)) return fn_exec(a, m.u.o, v_obj(o), depth, argc, argv, reason, 0);
    return o_call(a, m.u.o, v_obj(o), argc, argv);
}

static void define_ctor_props(Act *a, AObj *self, AObj *callee) {
    /* SWF5 movies do not see __constructor__ */
    /* the Linux player enumerates __constructor__ before constructor (gnash argstest) */
    int lin_rules = devfont_linux_rules();
    if (lin_rules && a->swf_version < 7) o_define(a->vm, self, a->vm->s_constructor, v_obj(callee), PA_DONT_ENUM);
    o_define(a->vm, self, a->vm->s___constructor__, v_obj(callee), PA_DONT_ENUM | (a->swf_version < 6 ? PA_VERSION_6 : 0));
    if (!lin_rules && a->swf_version < 7) o_define(a->vm, self, a->vm->s_constructor, v_obj(callee), PA_DONT_ENUM);
}

Value o_construct(Act *a, AObj *ctor, int argc, Value *argv) {
    VM *vm = a->vm; Value proto, ret; AObj *self; int propagate;
    if (ctor->nk == NK_SUPER) ctor = ctor->nat.sup.self;
    if (!o_as_function(ctor)) return v_undef();
    if (!o_prototype(a, ctor, &proto)) { self = obj_bare(vm); proto = v_undef(); }
    else self = obj_new_v(vm, proto);
    {   /* the Linux player 13+ builds these natively: its members, then constructor and __proto__ */
        Env *e = env_get(vm, a->swf_version); int k, nat = 0;
        for (k = 0; k < e->nlnx; k++) if (e->lnx_native[k] == ctor) nat = 1;
        if (e->lnx_xml && e->lnx_xml == ctor) {
            Prop *pp;
            fn_exec_ctor(a, ctor, v_obj(self), 1, argc, argv, ER_CONSTRUCTOR);
            if (vm->err) return v_undef();
            if ((pp = pm_get(&self->props, vm->s_proto, 1)) != NULL) pp->attr |= PA_NEVER_ENUM;
            return v_obj(self);
        }
        if (nat) {
            Prop *lp;
            propagate = ctor->nat.fn->kind == FK_NATIVE && ctor->nat.fn->ctor != NULL;
            ret = fn_exec_ctor(a, ctor, v_obj(self), 1, argc, argv, ER_CONSTRUCTOR);
            if (vm->err) return v_undef();
            if (propagate) { if (ret.t != V_OBJ) return ret; self = ret.u.o; }
            /* (__constructor__ is there, but never enumerated) */
            o_define(vm, self, vm->s___constructor__, v_obj(ctor), PA_DONT_ENUM | PA_NEVER_ENUM);
            /* (SWF7+: an inherited constructor to hasOwnProperty) */
            o_define(vm, self, vm->s_constructor, v_obj(ctor), PA_DONT_ENUM | (a->swf_version >= 7 ? PA_NOT_OWN : 0));
            avm1_prop_to_front(vm, self, vm->s_proto);
            /* (its listener list shows where its prototype's is) */
            if ((lp = pm_get(&self->props, vm->s__listeners, 1)) != NULL && proto.t == V_OBJ) {
                Prop *pl = pm_get(&proto.u.o->props, vm->s__listeners, 1);
                if (pl && !(pl->attr & PA_NEVER_ENUM)) lp->attr |= PA_NEVER_ENUM;
            }
            return v_obj(self);
        }
    }
    define_ctor_props(a, self, ctor);
    propagate = ctor->nat.fn->kind == FK_NATIVE && ctor->nat.fn->ctor != NULL;
    ret = fn_exec_ctor(a, ctor, v_obj(self), 1, argc, argv, ER_CONSTRUCTOR);
    if (vm->err) return v_undef();
    return propagate ? ret : v_obj(self);
}

void o_construct_on_existing(Act *a, AObj *ctor, AObj *self, int argc, Value *argv) {
    if (ctor->nk == NK_SUPER) ctor = ctor->nat.sup.self;
    if (!o_as_function(ctor)) return;
    define_ctor_props(a, self, ctor);
    fn_exec_ctor(a, ctor, v_obj(self), 1, argc, argv, ER_CONSTRUCTOR);
}

/* ---------------------------------------------------------------- */
/* declarations                                                     */

void decl_apply(VM *vm, Env *env, AObj *o, const Decl *d) {
    for (; d->kind != D_END; d++) {
        AStr *name = str_intern(vm, d->name);
        switch (d->kind) {
        case D_METHOD: o_define(vm, o, name, v_obj(fn_native(vm, env, d->fn, NULL)), d->attr); break;
        case D_PROPERTY: o_add_property(vm, o, name, fn_native(vm, env, d->fn, NULL), d->set ? fn_native(vm, env, d->set, NULL) : NULL, d->attr); break;
        case D_VALUE_NUM: o_define(vm, o, name, v_num(d->num), d->attr); break;
        case D_VALUE_STR: o_define(vm, o, name, v_str(str_intern(vm, d->str)), d->attr); break;
        case D_VALUE_BOOL: o_define(vm, o, name, v_bool(d->num != 0), d->attr); break;
        case D_VALUE_NULL: o_define(vm, o, name, v_null(), d->attr); break;
        case D_VALUE_UNDEF: o_define(vm, o, name, v_undef(), d->attr); break;
        default: break;
        }
    }
}

void decl_table(VM *vm, Env *env, AObj *o, TableFn f, const TDecl *d) {
    for (; d->name; d++) {
        AStr *name = str_intern(vm, d->name);
        if (d->set_index == -1) o_define(vm, o, name, v_obj(fn_table(vm, env, f, d->index)), d->attr);
        else o_add_property(vm, o, name, fn_table(vm, env, f, d->index), d->set_index >= 0 ? fn_table(vm, env, f, d->set_index) : NULL, d->attr);
    }
}

/* ---------------------------------------------------------------- */
/* GC                                                               */

void gc_mark_str(AStr *s) { if (s) s->gc.mark = 1; }

static void mark_props(VM *vm, PropMap *m) {
    int i;
    for (i = 0; i < m->n; i++) {
        Prop *p = &m->items[i];
        if (!p->key) continue;
        gc_mark_str(p->key);
        gc_mark_value(vm, p->v);
        if (p->getter) gc_mark_obj(vm, p->getter);
        if (p->setter) gc_mark_obj(vm, p->setter);
    }
}

static void mark_fn(VM *vm, AFunc *f) {
    int i;
    if (!f || f->gc.mark) return;
    f->gc.mark = 1;
    gc_mark_str(f->name);
    for (i = 0; i < f->nparams; i++) gc_mark_str(f->params[i].name);
    if (f->scope) gc_mark_hdr(vm, &f->scope->gc);
    if (f->pool) gc_mark_hdr(vm, &f->pool->gc);
    if (f->base_clip) gc_mark_hdr(vm, &f->base_clip->gc);
}

void gc_mark_hdr(VM *vm, GCHdr *h) {
    if (!h || h->mark) return;
    switch (h->kind) {
    case GC_STR: h->mark = 1; break;
    case GC_OBJ: gc_mark_obj(vm, (AObj*)h); break;
    case GC_FN: mark_fn(vm, (AFunc*)h); break;
    case GC_SCOPE: {
        Scope *s = (Scope*)h;
        while (s && !s->gc.mark) { s->gc.mark = 1; if (s->values) gc_mark_obj(vm, s->values); s = s->parent; }
        break; }
    case GC_POOL: {
        CPool *c = (CPool*)h; int i;
        h->mark = 1;
        for (i = 0; i < c->n; i++) gc_mark_value(vm, c->v[i]);
        break; }
    case GC_REF: {
        ClipRef *r = (ClipRef*)h; int i;
        h->mark = 1;
        gc_mark_str(r->path);
        for (i = 0; i < r->nseg; i++) gc_mark_str(r->seg[i]);
        if (r->cached) gc_mark_obj(vm, r->cached);
        break; }
    }
}

void gc_mark_obj(VM *vm, AObj *o) {
    int i;
    /* iterative over the __proto__ chain to bound recursion */
    while (o && !o->gc.mark) {
        o->gc.mark = 1;
        mark_props(vm, &o->props);
        for (i = 0; i < o->nwatch; i++) { gc_mark_str(o->watch[i].name); gc_mark_obj(vm, o->watch[i].cb); gc_mark_value(vm, o->watch[i].user); }
        for (i = 0; i < o->nifaces; i++) gc_mark_obj(vm, o->ifaces[i]);
        switch (o->nk) {
        case NK_SUPER: gc_mark_obj(vm, o->nat.sup.self); break;
        case NK_STRING: gc_mark_str(o->nat.s); break;
        case NK_FUNCTION: mark_fn(vm, o->nat.fn); break;
        default: break;
        }
        if (o->nmark) o->nmark(vm, o);
        if (o->relay) gc_mark_obj(vm, o->relay);
        o = NULL;
    }
}

void gc_mark_value(VM *vm, Value v) {
    if (v.t == V_STR) gc_mark_str(v.u.s);
    else if (v.t == V_OBJ) gc_mark_obj(vm, v.u.o);
    else if (v.t == V_CLIP) gc_mark_hdr(vm, &v.u.c->gc);
}

static void gc_free_node(GCHdr *h) {
    switch (h->kind) {
    case GC_STR: { AStr *s = (AStr*)h; if (s->wide) free(s->d.w); else free(s->d.b); free(s->utf8); break; }
    case GC_OBJ: {
        AObj *o = (AObj*)h;
        if (OBJ_IS_DISPLAY(o) && o->nat.clip && o->nat.clip->obj == (struct AObj*)o) o->nat.clip->obj = NULL;
        obj_free_payload(o);
        pm_free(&o->props); free(o->watch); free(o->ifaces);
        break; }
    case GC_FN: { AFunc *f = (AFunc*)h; free(f->params); break; }
    case GC_POOL: free(((CPool*)h)->v); break;
    case GC_REF: free(((ClipRef*)h)->seg); break;
    default: break;
    }
    free(h);
}

extern void avm1_mark_roots(VM *vm);

void gc_collect(VM *vm) {
    GCHdr **pp; int i;
    avm1_mark_roots(vm);
    /* weak intern table: drop dead entries */
    for (i = 0; i < vm->capintern; i++) if (vm->intern[i] && !vm->intern[i]->gc.mark) { vm->intern[i]->interned = 0; vm->intern[i] = NULL; vm->nintern--; }
    if (vm->capintern) {
        AStr **old = vm->intern; int oc = vm->capintern;
        vm->intern = (AStr**)calloc((size_t)oc, sizeof(AStr*));
        if (vm->intern) {
            for (i = 0; i < oc; i++) if (old[i]) {
                u32 mask = (u32)oc - 1, j = old[i]->hash & mask;
                while (vm->intern[j]) j = (j + 1) & mask;
                vm->intern[j] = old[i];
            }
            free(old);
        } else vm->intern = old;
    }
    pp = &vm->gc_list;
    while (*pp) {
        GCHdr *h = *pp;
        if (h->mark) { h->mark = 0; pp = &h->next; continue; }
        *pp = h->next;
        gc_free_node(h);
        vm->gc_count--;
    }
    vm->gc_threshold = vm->gc_count * 2 + 16384;
}

void avm1_free_all(VM *vm) {
    GCHdr *h = vm->gc_list;
    while (h) { GCHdr *n = h->next; gc_free_node(h); h = n; }
    vm->gc_list = NULL;
}
