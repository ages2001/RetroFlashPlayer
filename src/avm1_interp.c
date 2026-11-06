/* avm1_interp.c - AVM1 activations, scopes, function calls and the
 * action interpreter (one function per action, Flash Player semantics). */
#include "avm1_int.h"
static int a1_dbg_calls(void);
#include "audio.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

/* ---------------------------------------------------------------- */
/* environments                                                     */

Env *env_get(VM *vm, int swf_version) {
    Env *e = &vm->env[swf_version >= 7 ? 1 : 0];
    if (!e->ready) { e->ready = 1; lib_create_globals(vm, e); }
    return e;
}

AObj *act_global(Act *a) { return env_get(a->vm, a->swf_version)->global; }
int act_case_sensitive(Act *a) { return a->swf_version >= 7; }

/* ---------------------------------------------------------------- */
/* stack                                                            */

void vm_push(VM *vm, Value v) {
    if (vm->sp == vm->cap) {
        int nc = vm->cap ? vm->cap * 2 : 1024; Value *ns;
        if (nc > STACK_MAX) return;
        ns = (Value*)realloc(vm->stack, (size_t)nc * sizeof(Value));
        if (!ns) return;
        vm->stack = ns; vm->cap = nc;
    }
    vm->stack[vm->sp++] = v;
}
Value vm_pop(VM *vm) { return vm->sp > 0 ? vm->stack[--vm->sp] : v_undef(); }

/* push converting stage objects into movie clip references */
static void stack_push(Act *a, Value v) {
    if (v.t == V_OBJ) v = clipref_value(a, v.u.o);
    vm_push(a->vm, v);
}

static Value pop_arg(Act *a) {
    Value v = vm_pop(a->vm);
    if (v.t == V_CLIP) { AObj *o = v_obj_of(a, v); if (o) return v_obj(o); }
    return v;
}

/* ---------------------------------------------------------------- */
/* scopes                                                           */

static Scope *scope_new(VM *vm, Scope *parent, int cls, AObj *values) {
    Scope *s = (Scope*)gc_alloc(vm, sizeof(Scope), GC_SCOPE);
    if (!s) return parent;
    s->parent = parent; s->cls = (u8)cls; s->values = values;
    return s;
}

/* replace the values of the innermost Target scope (copying the chain above it) */
static Scope *scope_new_target(VM *vm, Scope *parent, AObj *clip) {
    Scope *s;
    if (!parent) return NULL;
    s = scope_new(vm, parent->parent, parent->cls, parent->values);
    if (s->cls == SC_TARGET) s->values = clip;
    else s->parent = scope_new_target(vm, parent->parent, clip);
    return s;
}

static Value scope_resolve(Act *a, Scope *s, AStr *name, AObj **owner) {
    VM *vm = a->vm; Value v;
    for (;;) {
        AObj *values = s->values;
        MovieClip *d = o_display_no_super(values);
        if (!s->parent && a->swf_version < 5) break;   /* SWF4 code sees no globals (Object, Math ...) */
        if (OBJ_IS_DISPLAY(values) && (!d || clip_removed(d))) values = clip_obj(vm, act_target_or_root(a));
        if (values && o_has_property(a, values, name)) {
            v = o_get(a, values, name);
            if (owner) *owner = values;
            return v;
        }
        if (vm->err) return v_undef();
        if (!s->parent) break;
        s = s->parent;
    }
    if (owner) *owner = NULL;
    if (stage_resolve_path_property(a, act_target_or_root(a), name, &v)) return v;
    return v_undef();
}

static void scope_set(Act *a, Scope *s, AStr *name, Value v) {
    extern int stage_prop_unsettable(Act *a, MovieClip *d, AStr *name);
    for (;;) {
        MovieClip *d = o_display_no_super(s->values);
        int removed = OBJ_IS_DISPLAY(s->values) && (!d || clip_removed(d));
        /* with (clip) { _parent = x } sets nothing on the clip (Gnash TextField.as) */
        int skip = s->cls == SC_WITH && d && !removed && stage_prop_unsettable(a, d, name) && !o_has_own_property(a, s->values, name);
        if (!removed && !skip && (s->cls == SC_TARGET || o_has_property(a, s->values, name))) { o_set(a, s->values, name, v); return; }
        if (a->vm->err) return;
        if (!s->parent) { o_set(a, s->values, name, v); return; }
        s = s->parent;
    }
}

static void scope_define_local(Act *a, Scope *s, AStr *name, Value v) {
    while (s->cls == SC_WITH && s->parent) {
        if (o_has_own_property(a, s->values, name)) { o_set(a, s->values, name, v); return; }
        s = s->parent;
    }
    o_set(a, s->values, name, v);
}

static int scope_delete(Act *a, Scope *s, AStr *name) {
    for (; s; s = s->parent) if (o_has_property(a, s->values, name)) return o_delete(a, s->values, name);
    return 0;
}

static int in_local_scope(Act *a) {
    Scope *s;
    for (s = a->scope; s; s = s->parent) {
        if (s->cls == SC_LOCAL) return 1;
        if (s->cls == SC_TARGET) return 0;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* activations                                                      */

static void act_base(Act *a, VM *vm, int swf_version, Scope *scope, CPool *pool, MovieClip *base, Value self, AObj *callee, Value *regs, int nregs) {
    memset(a, 0, sizeof(*a));
    a->vm = vm; a->swf_version = (u8)(swf_version > 0 ? swf_version : 1);
    a->scope = scope; a->pool = pool; a->base_clip = base; a->target_clip = base; a->has_target = base != NULL;
    a->this_ = self; a->callee = callee; a->regs = regs; a->nregs = nregs;
    a->id.reason = ER_SPECIAL;
}

void act_init_nothing(Act *a, VM *vm, MovieClip *base_clip, const char *name) {
    int v; Env *e;
    (void)name;
    if (!base_clip) base_clip = vm->player->root;
    v = clip_swf_version(base_clip);
    e = env_get(vm, v);
    act_base(a, vm, v, e->gscope, vm->cpool, base_clip, v_obj(e->global), NULL, NULL, 0);
}

void act_init_clip(Act *a, VM *vm, MovieClip *clip) {
    int v = clip_swf_version(clip); Env *e = env_get(vm, v); AObj *co = clip_obj(vm, clip);
    act_base(a, vm, v, scope_new(vm, e->gscope, SC_TARGET, co ? co : obj_bare(vm)), vm->cpool, clip, v_obj(co), NULL, NULL, 0);
}

static void act_child(Act *dst, const Act *src) {
    *dst = *src;
    dst->id.parent = &src->id;
    dst->id.depth = (u16)(src->id.depth + 1);
}

MovieClip *act_target_or_root(Act *a) {
    if (a->has_target && a->target_clip && !clip_removed(a->target_clip)) return a->target_clip;
    return clip_avm1_root(a->base_clip ? a->base_clip : a->vm->player->root);
}
MovieClip *act_target_or_base(Act *a) {
    if (a->has_target && a->target_clip && !clip_removed(a->target_clip)) return a->target_clip;
    return a->base_clip;
}
static MovieClip *act_target(Act *a) {
    if (a->has_target && a->target_clip && !clip_removed(a->target_clip)) return a->target_clip;
    return NULL;
}
static void set_target_clip(Act *a, MovieClip *c) {
    if (c && clip_removed(c)) c = NULL;
    a->target_clip = c; a->has_target = c != NULL;
}

int act_this_obj(Act *a, AObj **out) {
    AObj *o = v_obj_of(a, a->this_);
    *out = o;
    return o != NULL;
}

/* the debug player's script warnings: at most 100 per run */
void avm1_warning(VM *vm, const char *msg) {
    static int count;
    if (count > 100) return;
    if (++count > 100) { avm1_trace_out("Warning: Reached warning limit of 100"); return; }
    (void)vm; avm1_trace_out(msg);
}
void act_throw(Act *a, Value v) { a->vm->err = ERR_THROWN; a->vm->thrown = v; }

/* recursion limits (Flash: 256 levels, 65 for implicit conversions and per property) */
static int prop_recursion_over(const ActId *id, u32 prop, int limit) {
    if (limit == 0) return 1;
    if (id->depth < limit) return 0;
    {
        int pl = (id->reason == ER_PROPERTY && id->property_id == prop) ? limit - 1 : limit;
        return id->parent ? prop_recursion_over(id->parent, prop, pl) : 0;
    }
}

static int act_id_function(VM *vm, const ActId *parent, int reason, u32 prop_id, ActId *out) {
    u16 fc = parent->function_count; u8 sc = parent->special_count;
    if (reason == ER_FUNCTION || reason == ER_CONSTRUCTOR) fc++;
    else if (reason == ER_PROPERTY) {
        if (prop_recursion_over(parent, prop_id, 65)) { vm->err = ERR_PROP_RECURSION; return 0; }
        fc++;
    } else {
        if (sc == 65) { vm->err = ERR_SPECIAL_RECURSION; return 0; }
        sc++;
    }
    if (fc >= vm->max_recursion) { vm->err = ERR_FUNC_RECURSION; return 0; }
    out->parent = parent; out->reason = (u8)reason; out->depth = (u16)(parent->depth + 1);
    out->function_count = fc; out->special_count = sc; out->property_id = prop_id;
    return 1;
}

/* ---------------------------------------------------------------- */
/* registers                                                        */

/* players up to 10 do not fall back to the global registers */
extern int avm1_player_major(void);
#define player_major avm1_player_major
static Value get_register(Act *a, int id) {
    if (a->nregs > 0) { if (id < a->nregs) return a->regs[id]; if (player_major() <= 10) return v_undef(); }
    if (id < 4) return a->vm->gregs[id];
    return v_undef();
}
static int set_local_register(Act *a, int id, Value v) {
    if (a->nregs > 0) { if (id < a->nregs) { a->regs[id] = v; return 1; } return player_major() <= 10; }
    return 0;
}
static void set_register(Act *a, int id, Value v) {
    if (!set_local_register(a, id, v) && id < 4) a->vm->gregs[id] = v;
}

/* ---------------------------------------------------------------- */
/* function execution                                               */

static Value act_run_body(Act *a, const u8 *base, u32 base_len, u32 start, u32 len);

Value avm1_exec_action(Act *caller, AObj *callee, AFunc *f, Value self, int depth, int argc, Value *argv, int reason, u32 prop_id) {
    VM *vm = caller->vm; Act fr; ActId id; AObj *this_obj = v_obj_of(caller, self);
    MovieClip *target = act_target_or_root(caller), *this_do, *base_clip; int is_closure = caller->swf_version >= 6;
    Scope *parent_scope; int swf_version; Value local_this; Value regs_local[64], *regs; int preload = 1, i; Value result;
    if (!act_id_function(vm, &caller->id, reason, prop_id, &id)) return v_undef();
    this_do = this_obj ? o_display(this_obj) : NULL;
    if (!this_do) this_do = target;
    if (is_closure || reason == ER_SPECIAL) {
        MovieClip *bd = NULL;
        if (f->base_clip) clipref_resolve(caller, f->base_clip, NULL, &bd);
        base_clip = bd ? bd : this_do;
    } else base_clip = this_do;
    if (is_closure) { swf_version = f->swf_version; parent_scope = f->scope; }
    else {
        AObj *bo = clip_obj(vm, base_clip);
        swf_version = clip_swf_version(base_clip); if (swf_version < 5) swf_version = 5;
        parent_scope = scope_new(vm, env_get(vm, caller->swf_version)->gscope, SC_TARGET, bo ? bo : obj_bare(vm));
    }
    local_this = (f->flags & (FF_PRELOAD_THIS | FF_SUPPRESS_THIS)) ? caller->this_ : self;
    regs = f->nregs <= 64 ? regs_local : (Value*)malloc((size_t)f->nregs * sizeof(Value));
    if (!regs) return v_undef();
    for (i = 0; i < f->nregs; i++) regs[i] = v_undef();
    act_base(&fr, vm, swf_version, scope_new(vm, parent_scope, SC_LOCAL, obj_bare(vm)), f->pool, base_clip, local_this, callee, regs, f->nregs);
    fr.id = id;
    /* this */
    if (f->flags & FF_PRELOAD_THIS) { set_local_register(&fr, preload, (f->flags & FF_SUPPRESS_THIS) ? v_undef() : self); preload++; }
    /* arguments */
    if (!((f->flags & FF_SUPPRESS_ARGUMENTS) && !(f->flags & FF_PRELOAD_ARGUMENTS))) {
        AObj *args = obj_array(&fr, argc, argv);
        args->flags |= AOF_ARGUMENTS;
        o_define(vm, args, vm->s_callee, v_obj(callee), PA_DONT_ENUM);
        o_define(vm, args, vm->s_caller, caller->callee ? v_obj(caller->callee) : v_null(), PA_DONT_ENUM);
        if (f->flags & FF_PRELOAD_ARGUMENTS) { set_local_register(&fr, preload, v_obj(args)); preload++; }
        else o_define(vm, fr.scope->values, vm->s_arguments, v_obj(args), 0);
    }
    /* super */
    {
        /* SWF5 functions have no super */
        AObj *zuper = (this_obj && !(f->flags & FF_SUPPRESS_SUPER) && swf_version >= 6) ? obj_super(vm, this_obj, depth) : NULL;
        if (f->flags & FF_PRELOAD_SUPER) { set_local_register(&fr, preload, zuper ? v_obj(zuper) : v_undef()); preload++; }
        else if (zuper) o_define(vm, fr.scope->values, vm->s_super, v_obj(zuper), 0);
    }
    if (f->flags & FF_PRELOAD_ROOT) { Value r; clip_obj_or_undef_v(vm, clip_avm1_root(base_clip), &r); set_local_register(&fr, preload, r); preload++; }
    if (f->flags & FF_PRELOAD_PARENT) { MovieClip *pc = clip_avm1_parent(base_clip); if (pc) { Value r; clip_obj_or_undef_v(vm, pc, &r); set_local_register(&fr, preload, r); preload++; } }
    if (f->flags & FF_PRELOAD_GLOBAL) { set_local_register(&fr, preload, v_obj(act_global(&fr))); preload++; }
    for (i = 0; i < f->nparams; i++) {
        Value v = i < argc ? argv[i] : v_undef();
        if (f->params[i].reg) set_local_register(&fr, f->params[i].reg, v);
        else o_define(vm, fr.scope->values, f->params[i].name, v, 0);
    }
    result = act_run_body(&fr, f->base, f->base_len, f->start, f->len);
    if (regs != regs_local) free(regs);
    return result;
}

Value vm_call_value(Act *a, Value fn, Value self, int argc, Value *argv) {
    AObj *f = v_obj_of(a, fn);
    if (!f) return v_undef();
    return o_call(a, f, self, argc, argv);
}

/* ---------------------------------------------------------------- */
/* target paths                                                     */

static int seg_eq(const AStr *s, u32 start, u32 end, const char *lit, int cs) {
    u32 i; size_t n = strlen(lit);
    if (end - start != n) return 0;
    for (i = 0; i < n; i++) { u16 c = STR_AT(s, start + i); if (cs ? c != (u8)lit[i] : swf_lower(c) != swf_lower((u8)lit[i])) return 0; }
    return 1;
}

static int parse_level_id(AStr *s, u32 start, u32 end) {
    int neg = 0; u32 i = start; s32 v = 0;
    if (i < end && STR_AT(s, i) == '-') { neg = 1; i++; }
    for (; i < end; i++) { u16 c = STR_AT(s, i); if (c < '0' || c > '9') break; v = (s32)((u32)v * 10u + (u32)(c - '0')); }
    return neg ? -v : v;
}

/* "_levelN" / "_flashN": the level (or undefined); 0 when the name is not a level */
static int parse_level(Act *a, AStr *s, u32 start, u32 end, Value *out) {
    int cs = act_case_sensitive(a);
    if (end - start < 6) return 0;
    if (seg_eq(s, start, start + 6, "_level", cs) || seg_eq(s, start, start + 6, "_flash", cs)) {
        MovieClip *l = act_get_level(a, parse_level_id(s, start + 6, end));
        if (l) clip_obj_or_undef_v(a->vm, l, out); else *out = v_undef();
        return 1;
    }
    return 0;
}

AObj *act_resolve_target_path(Act *a, MovieClip *root, AObj *start, AStr *path, int first_element, int handle_this) {
    VM *vm = a->vm; int is_swf5 = a->swf_version >= 5, slash = 0, cs = act_case_sensitive(a);
    u32 p = 0, n = path->len; AObj *object = start;
    if (n == 0) return start;
    if (STR_AT(path, 0) == '/') { p = 1; first_element = 0; object = clip_obj(vm, root); slash = 1; if (!object) object = obj_bare(vm); }
    while (p < n) {
        Value val; u32 q, rem;
        if (is_swf5) while (p < n && STR_AT(path, p) == ':') p++;
        rem = n - p;     /* trailing ':'s leave an empty name, which fails the lookup */
        if (rem >= 2 && STR_AT(path, p) == '.' && STR_AT(path, p + 1) == '.' &&
            (rem == 2 || STR_AT(path, p + 2) == '/' || (is_swf5 && STR_AT(path, p + 2) == ':'))) {
            /* "..", "../" or "..:" */
            MovieClip *d = o_display(object), *par;
            if (rem >= 3 && STR_AT(path, p + 2) == '/') slash = 1;
            p = p + 3 > n ? n : p + 3;
            par = d ? clip_avm1_parent(d) : NULL;
            if (!par) return NULL;
            clip_obj_or_undef_v(vm, par, &val);
        } else {
            u32 e = p; AStr *name;
            while (e < n) {
                u16 c = STR_AT(path, e);
                if (c == ':' && is_swf5) break;
                if (c == '.' && is_swf5 && !slash) break;
                if (c == '/') { slash = 1; break; }
                e++;
            }
            q = e;
            if (is_swf5 && handle_this && seg_eq(path, p, q, "this", 1)) val = a->this_;
            else if (is_swf5 && first_element && seg_eq(path, p, q, "_root", 1)) { MovieClip *r = clip_avm1_root(a->base_clip); clip_obj_or_undef_v(vm, r, &val); }
            else if (first_element && parse_level(a, path, p, q, &val)) { }
            else {
                MovieClip *d = o_display(object), *child = NULL;
                name = str_slice(vm, path, p, q);
                if (d) child = clip_child_by_name(a, d, name);
                if (child) { AObj *co = clip_obj(vm, child); val = co ? v_obj(co) : v_undef(); }
                else { val = o_get(a, object, name); if (vm->err) return NULL; }
            }
            p = q + 1 > n ? n : q + 1;
            if (q >= n) p = n;
        }
        (void)cs;
        first_element = 0;
        object = v_obj_of(a, val);
        if (!object) return NULL;
    }
    return object;
}

MovieClip *act_resolve_target_display(Act *a, MovieClip *start, Value target, int allow_empty) {
    AStr *path; AObj *o;
    if (target.t == V_OBJ && o_display(target.u.o)) return o_display(target.u.o);
    path = v_str_of(a, target);
    if (a->vm->err) return NULL;
    if (!allow_empty && path->len == 0) return NULL;
    o = act_resolve_target_path(a, clip_avm1_root(start), clip_obj(a->vm, start), path, 1, 0);
    return o ? o_display(o) : NULL;
}

/* split "path:var" / "path.var" at the last separator */
static long var_sep(AStr *path, int dots) { return str_rfind_any(path, dots ? ":." : ":"); }

Value act_resolve(Act *a, AStr *name, AObj **owner) {
    int this_cs = a->swf_version <= 5 ? a->scope->cls == SC_LOCAL : act_case_sensitive(a);
    if (a->swf_version >= 5 && (this_cs ? str_eq_c(name, "this") : str_eq_c_ci(name, "this"))) { if (owner) *owner = NULL; return a->this_; }
    return scope_resolve(a, a->scope, name, owner);
}

Value act_get_variable(Act *a, AStr *path, AObj **owner) {
    VM *vm = a->vm; MovieClip *start = act_target_or_root(a); int is_swf5 = a->swf_version >= 5; long sep;
    if (owner) *owner = NULL;
    sep = var_sep(path, is_swf5);
    if (sep >= 0) {
        AStr *p = str_slice(vm, path, 0, (u32)sep), *var = str_slice(vm, path, (u32)sep + 1, path->len); Scope *s;
        if (p->len == 0) return v_undef();
        for (s = a->scope; s; s = s->parent) {
            AObj *o = act_resolve_target_path(a, clip_avm1_root(start), s->values, p, 1, 1);
            if (vm->err) return v_undef();
            if (o) {
                if (!is_swf5) {   /* SWF4 "path:var" reads variables only, not display properties */
                    Prop *pp = o->nk == NK_SUPER ? NULL : pm_get(&o->props, var, 0);
                    if (owner && pp) *owner = o;
                    return pp ? pp->v : v_undef();
                }
                if (o_has_property(a, o, var)) { Value v = o_get(a, o, var); if (owner) *owner = o; return v; }
            }
        }
        return v_undef();
    }
    if (is_swf5 && str_find_unit(path, '/', 0) >= 0) {
        Scope *s;
        for (s = a->scope; s; s = s->parent) {
            AObj *o = act_resolve_target_path(a, clip_avm1_root(start), s->values, path, 0, 0);
            if (vm->err) return v_undef();
            if (o) return v_obj(o);
        }
    }
    return act_resolve(a, path, owner);
}

void act_set_variable(Act *a, AStr *path, Value v) {
    VM *vm = a->vm; MovieClip *start = act_target_or_root(a); long sep;
    if (path->len == 0) return;
    if (a->swf_version >= 5 && str_eq_c(path, "this")) { a->this_ = v; return; }
    sep = var_sep(path, 1);
    if (sep >= 0) {
        AStr *p = str_slice(vm, path, 0, (u32)sep), *var = str_slice(vm, path, (u32)sep + 1, path->len); Scope *s;
        for (s = a->scope; s; s = s->parent) {
            AObj *o = act_resolve_target_path(a, clip_avm1_root(start), s->values, p, 1, 1);
            if (vm->err) return;
            if (o) { o_set(a, o, var, v); return; }
        }
        return;
    }
    scope_set(a, a->scope, path, v);
}

static void define_local(Act *a, AStr *name, Value v) {
    if (!in_local_scope(a) && str_find_any(name, ":.", 0) >= 0) act_set_variable(a, name, v);
    else scope_define_local(a, a->scope, name, v);
}

MovieClip *act_get_level(Act *a, int level) {
    extern MovieClip *avm1_level(Player *p, int level);
    extern MovieClip *avm1_mix_level(VM *vm, MovieClip *base, int level, int create);
    if (a->vm->player->avm2) return avm1_mix_level(a->vm, a->base_clip, level, 0);   /* levels of an AVM1Movie live in its Loader */
    return avm1_level(a->vm->player, level);
}
MovieClip *act_get_or_create_level(Act *a, int level) {
    extern MovieClip *avm1_level_create(Player *p, int level, SWFMovie *movie);
    extern MovieClip *avm1_mix_level(VM *vm, MovieClip *base, int level, int create);
    MovieClip *l = act_get_level(a, level);
    if (l) return l;
    if (a->vm->player->avm2) return avm1_mix_level(a->vm, a->base_clip, level, 1);
    return avm1_level_create(a->vm->player, level, a->base_clip ? a->base_clip->movie : a->vm->player->movie);
}

/* ---------------------------------------------------------------- */
/* uncaught errors                                                  */

void vm_report_error(Act *a) {
    VM *vm = a->vm; int e = vm->err;
    if (!e) return;
    vm->err = 0;
    if (e == ERR_THROWN) {
        Value t = vm->thrown; AStr *s;
        vm->thrown = v_undef();
        s = v_str_of(a, t);
        if (vm->err) { vm->err = 0; s = str_intern(vm, "[type Object]"); }
        {
            char *buf = (char*)malloc(strlen(str_c(vm, s)) + 64);
            if (buf) { sprintf(buf, "Warning: Uncaught exception, %s", str_c(vm, s)); avm1_trace_out(buf); free(buf); }
        }
        return;
    }
    if (!vm->halted) {
        vm->halted = 1;
        if (vm->player->debug) fprintf(stderr, "[avm1] engine error %d: no more actions will be executed in this movie\n", e);
    }
}

/* ---------------------------------------------------------------- */
/* actions                                                          */

static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

enum { FC_CONTINUE = 0, FC_RETURN };

typedef struct Run {
    const u8 *base; u32 base_len;
    u32 pc, end;
    Value ret;
} Run;

static AStr *pstr(Act *a, const u8 *p, u32 *pos, u32 plen) {
    u32 s = *pos, e = s;
    while (e < plen && p[e]) e++;
    *pos = e < plen ? e + 1 : plen;
    return str_from_swf(a->vm, (const char*)p + s, e - s, a->swf_version);
}

/* after a call: a removed base clip ends the script (explicit return of undefined) */
static int continue_if_base_r(Act *a, Run *r) { if (a->base_clip && clip_removed(a->base_clip)) { r->ret = v_undef(); return FC_RETURN + 1; } return FC_CONTINUE; }
#define continue_if_base(a) continue_if_base_r(a, r)

#define POP() vm_pop(vm)
#define PUSH(v) vm_push(vm, (v))
#define CHK() do { if (vm->err) return FC_RETURN; } while (0)

static void pop_args(Act *a, int n, Value *out) {
    int i, avail = a->vm->sp;
    if (n > avail) n = avail;
    for (i = 0; i < n; i++) out[i] = pop_arg(a);
}

static Value *alloc_args(int n, Value *local, int localcap) {
    if (n <= localcap) return local;
    return (Value*)malloc((size_t)(n ? n : 1) * sizeof(Value));
}

static int frame_call(Act *a, Run *r, Value arg);
static int do_get_url2(Act *a, u8 flags);
static int do_get_url(Act *a, AStr *url, AStr *target);
static void action_trace(Act *a, Value v);
extern int avm1_random(VM *vm, int max);
extern double avm1_get_time(VM *vm);

static int set_target(Act *a, AStr *target) {
    VM *vm = a->vm; MovieClip *base = a->base_clip, *nt = NULL; AObj *o;
    if (target->len == 0) nt = base;
    else {
        o = act_resolve_target_path(a, clip_avm1_root(base), clip_obj(vm, base), target, 1, 0);
        if (vm->err) return FC_RETURN;
        if (o && o_display(o) && !clip_removed(base)) nt = o_display(o);
    }
    set_target_clip(a, nt);
    { AObj *co = clip_obj(vm, act_target_or_root(a)); a->scope = scope_new_target(vm, a->scope, co ? co : obj_bare(vm)); }
    return FC_CONTINUE;
}

static int try_action(Act *a, Run *r, const u8 *p, u32 plen);
static int with_action(Act *a, Run *r, u32 size);
static int define_function(Act *a, Run *r, const u8 *p, u32 plen, int v2);

static int do_action(Act *a, Run *r) {
    VM *vm = a->vm; const u8 *code = r->base; u8 op; u32 plen = 0; const u8 *p = NULL;
    if (r->pc >= r->end || r->pc >= r->base_len) return FC_RETURN;
    if ((++vm->ops & 2047) == 0) {
        if (!vm->exec_start) vm->exec_start = (long)clock();
        else if ((double)((long)clock() - vm->exec_start) / CLOCKS_PER_SEC > (vm->max_exec_secs > 0 ? vm->max_exec_secs : 15.0)) { vm->err = ERR_TIMEOUT; return FC_RETURN; }
    }
    op = code[r->pc];
    if (op >= 0x80) {
        if (r->pc + 3 > r->base_len) { vm->err = ERR_INVALID; return FC_RETURN; }
        plen = rd16(code + r->pc + 1);
        p = code + r->pc + 3;
        if (r->pc + 3 + plen > r->base_len) { vm->err = ERR_INVALID; return FC_RETURN; }
        r->pc += 3 + plen;
    } else r->pc += 1;
    if (vm->player->debug > 2) fprintf(stderr, "  [a1] op %02x sp=%d\n", op, vm->sp);

    switch (op) {
    case 0x00: return FC_RETURN;                                       /* End */
    case 0x04: { MovieClip *c = act_target(a); if (c && clip_is_movieclip(c)) { extern void avm1_next_frame(MovieClip *c); avm1_next_frame(c); } return FC_CONTINUE; }
    case 0x05: { MovieClip *c = act_target(a); if (c && clip_is_movieclip(c)) { extern void avm1_prev_frame(MovieClip *c); avm1_prev_frame(c); } return FC_CONTINUE; }
    case 0x06: { MovieClip *c = act_target(a); if (c && clip_is_movieclip(c)) c->playing = 1; return FC_CONTINUE; }
    case 0x07: { MovieClip *c = act_target(a); if (c && clip_is_movieclip(c)) { extern void avm1_stop_clip(MovieClip *c); avm1_stop_clip(c); } return FC_CONTINUE; }
    case 0x08: { extern void avm1_toggle_quality(Player *p); avm1_toggle_quality(vm->player); return FC_CONTINUE; }
    case 0x09: audio_stop_all(vm->player); return FC_CONTINUE;
    case 0x0A: { Value x = POP(), y = POP(); double b = v_num_of(a, y); CHK(); { double c = v_num_of(a, x); CHK(); PUSH(v_num(b + c)); } return FC_CONTINUE; }
    case 0x0B: { double x = v_num_of(a, POP()); CHK(); { double y = v_num_of(a, POP()); CHK(); PUSH(v_num(y - x)); } return FC_CONTINUE; }
    case 0x0C: { Value x = POP(), y = POP(); double b = v_num_of(a, y); CHK(); { double c = v_num_of(a, x); CHK(); PUSH(v_num(b * c)); } return FC_CONTINUE; }
    case 0x0D: { double x = v_num_of(a, POP()); CHK(); { double y = v_num_of(a, POP()); CHK();
                 if (x == 0.0 && a->swf_version < 5) PUSH(v_str(str_intern(vm, "#ERROR#"))); else PUSH(v_num(y / x)); } return FC_CONTINUE; }
    case 0x0E: { double x = v_num_of(a, POP()); CHK(); { double y = v_num_of(a, POP()); CHK(); PUSH(v_bool(y == x)); } return FC_CONTINUE; }
    case 0x0F: { Value x = POP(), y = POP(); double b = v_num_of(a, y); CHK(); { double c = v_num_of(a, x); CHK(); PUSH(v_bool(b < c)); } return FC_CONTINUE; }
    case 0x10: { Value x = POP(), y = POP(); PUSH(v_bool(v_bool_of(a, y) && v_bool_of(a, x))); return FC_CONTINUE; }
    case 0x11: { Value x = POP(), y = POP(); PUSH(v_bool(v_bool_of(a, y) || v_bool_of(a, x))); return FC_CONTINUE; }
    case 0x12: { Value x = POP(); PUSH(v_bool(!v_bool_of(a, x))); return FC_CONTINUE; }
    case 0x13: { AStr *x = v_str_of(a, POP()); CHK(); { AStr *y = v_str_of(a, POP()); CHK(); PUSH(v_bool(str_eq(y, x))); } return FC_CONTINUE; }
    case 0x14: case 0x31: { AStr *s = v_str_of(a, POP()); CHK(); PUSH(v_num((double)s->len)); return FC_CONTINUE; }
    case 0x15: case 0x35: {                                            /* StringExtract / MBStringExtract */
        s32 len = v_i32(a, POP()), st; AStr *s; u32 start, end; CHK();
        st = v_i32(a, POP()); CHK();
        start = st >= 1 ? (u32)(st - 1) : 0;
        s = v_str_of(a, POP()); CHK();
        if (len >= 0 && (u64)start + (u32)len <= s->len) end = start + (u32)len; else end = s->len;
        if (start > end) start = end;
        PUSH(v_str(str_slice(vm, s, start, end)));
        return FC_CONTINUE; }
    case 0x17: POP(); return FC_CONTINUE;
    case 0x18: { s32 i = v_i32(a, POP()); CHK(); PUSH(v_num(i)); return FC_CONTINUE; }
    case 0x1C: {                                                       /* GetVariable */
        AStr *path = v_str_of(a, POP()); Value v; CHK();
        v = act_get_variable(a, path, NULL); CHK();
        if (a->swf_version <= 4) { if (v.t == V_CLIP || (v.t == V_OBJ && o_display(v.u.o))) v = v_undef(); }
        stack_push(a, v);
        return FC_CONTINUE; }
    case 0x1D: { Value v = POP(); AStr *path = v_str_of(a, POP()); CHK(); act_set_variable(a, path, v); CHK(); return FC_CONTINUE; }
    case 0x20: {                                                       /* SetTarget2 */
        Value t = POP();
        if (a->base_clip && clip_removed(a->base_clip)) { set_target_clip(a, NULL); return FC_CONTINUE; }
        if (t.t == V_UNDEF) {
            if (a->swf_version > 6) set_target_clip(a, NULL); else set_target_clip(a, a->base_clip);
        } else { AStr *s = v_str_of(a, t); CHK(); return set_target(a, s); }
        { AObj *co = clip_obj(vm, act_target_or_base(a)); a->scope = scope_new_target(vm, a->scope, co ? co : obj_bare(vm)); }
        return FC_CONTINUE; }
    case 0x21: { AStr *x = v_str_of(a, POP()); CHK(); { AStr *y = v_str_of(a, POP()); CHK(); PUSH(v_str(str_concat(vm, y, x))); } return FC_CONTINUE; }
    case 0x22: {                                                       /* GetProperty */
        double idx = v_num_of(a, POP()); Value path = POP(), out = v_undef(); MovieClip *c; CHK();
        c = act_target(a) ? act_resolve_target_display(a, act_target(a), path, 1) : act_resolve_target_display(a, a->base_clip, path, 0);
        CHK();
        if (c && idx == idx && idx > -1.0 && idx < 1e9) stage_prop_index_get(a, c, (int)idx, &out);
        CHK();
        stack_push(a, out);
        return FC_CONTINUE; }
    case 0x23: {                                                       /* SetProperty */
        Value v = POP(); double idx = v_num_of(a, POP()); Value path = POP(); MovieClip *c; int valid; CHK();
        c = act_target(a) ? act_resolve_target_display(a, act_target(a), path, 1) : act_resolve_target_display(a, a->base_clip, path, 0);
        CHK();
        valid = idx == idx && idx > -1.0 && idx < 22;
        if (valid && (!c || stage_prop_read_only((int)idx))) { stage_prop_coerce(a, (int)idx, v); CHK(); }
        if (c && valid) { stage_prop_index_set(a, c, (int)idx, v); CHK(); }
        return FC_CONTINUE; }
    case 0x24: {                                                       /* CloneSprite */
        s32 depth = v_i32(a, POP()); AStr *target; Value source; MovieClip *src; CHK();
        target = v_str_of(a, POP()); CHK();
        source = POP();
        src = act_resolve_target_display(a, act_target_or_root(a), source, 1); CHK();
        if (src) clip_clone_sprite(a, src, target, depth, NULL);
        CHK();
        return FC_CONTINUE; }
    case 0x25: {                                                       /* RemoveSprite */
        Value t = POP(); MovieClip *c = act_resolve_target_display(a, act_target_or_root(a), t, 1); CHK();
        if (c) {
            clip_remove_display(a, c);
            if (a->target_clip && clip_removed(a->target_clip)) {
                AObj *co;
                set_target_clip(a, a->base_clip);
                co = clip_obj(vm, act_target_or_root(a));
                a->scope = scope_new_target(vm, a->scope, co ? co : obj_bare(vm));
            }
        }
        return FC_CONTINUE; }
    case 0x26: action_trace(a, POP()); CHK(); return FC_CONTINUE;
    case 0x27: {                                                       /* StartDrag */
        Value t = POP(); MovieClip *c = act_resolve_target_display(a, act_target_or_root(a), t, 1); int lock, constrain; double b[4];
        CHK();
        lock = v_i32(a, POP()) == 1; CHK();
        constrain = v_i32(a, POP()) == 1; CHK();
        if (constrain) { b[3] = v_num_of(a, POP()); b[2] = v_num_of(a, POP()); b[1] = v_num_of(a, POP()); b[0] = v_num_of(a, POP()); CHK(); }
        if (c) clip_start_drag(a, c, lock, constrain ? b : NULL);
        return FC_CONTINUE; }
    case 0x28: { extern void avm1_end_drag(Player *p); avm1_end_drag(vm->player); return FC_CONTINUE; }
    case 0x29: { AStr *x = v_str_of(a, POP()); CHK(); { AStr *y = v_str_of(a, POP()); CHK(); PUSH(v_bool(str_cmp(y, x) < 0)); } return FC_CONTINUE; }
    case 0x2A: { Value v = POP(); act_throw(a, v); return FC_RETURN; }
    case 0x2B: {                                                       /* CastOp */
        Value obj = POP(), ctor = POP(); int is;
        if (v_is_prim(obj)) { v_to_obj(a, obj); CHK(); }
        is = v_instanceof(a, obj, ctor); CHK();
        PUSH(is ? obj : v_null());
        return FC_CONTINUE; }
    case 0x2C: {                                                       /* ImplementsOp */
        AObj *ctor = v_obj_of(a, POP()); Value cnt = POP(); int count = 0, i;
        if (v_is_prim(cnt)) { count = v_i32(a, cnt); CHK(); if (count < 0) count = vm->sp; }
        if (count > vm->sp) count = vm->sp;
        if (count > 0) {
            AObj **ifs = (AObj**)malloc((size_t)count * sizeof(AObj*)); int n = 0;
            for (i = 0; i < count; i++) {
                AObj *o = v_obj_of(a, POP()); Value pv;
                if (o && o_prototype(a, o, &pv) && pv.t == V_OBJ && ifs) ifs[n++] = pv.u.o;
            }
            if (ctor && ifs) { /* Flash honours ImplementsOp in SWF5/6 movies too */
                Value pv; AObj *po;
                if (o_prototype(a, ctor, &pv) && (po = v_obj_of(a, pv)) != NULL) o_set_interfaces(vm, po, ifs, n);
            }
            free(ifs);
        }
        return FC_CONTINUE; }
    case 0x30: {                                                       /* RandomNumber */
        double m = v_num_of(a, POP()); s32 max; CHK();
        if (m != m) max = 0; else if (m >= 2147483647.0) max = 2147483647; else if (m <= -2147483648.0) max = -2147483647 - 1; else max = (s32)m;
        PUSH(v_num(max > 0 ? avm1_random(vm, max) % max : 0)); return FC_CONTINUE; }
    case 0x32: { AStr *s = v_str_of(a, POP()); u16 c; CHK(); c = s->len ? STR_AT(s, 0) : 0;
                 if (c >= 0xD800 && c <= 0xDFFF) c = 0xFFFD;
                 PUSH(v_num(c)); return FC_CONTINUE; }
    case 0x33: {                                                       /* AsciiToChar */
        u16 c = v_u16(a, POP()); CHK();
        if (a->swf_version < 6) c = (u8)c;                             /* SWF5: one 8-bit character */
        if (c == 0) PUSH(v_str(vm->s_empty));
        else { u16 u = (a->swf_version < 6 || !(c >= 0xD800 && c <= 0xDFFF)) ? c : 0xFFFD; PUSH(v_str(str_from_units(vm, &u, 1))); }
        return FC_CONTINUE; }
    case 0x34: { double t = avm1_get_time(vm); PUSH(v_num(t)); return FC_CONTINUE; }
    case 0x36: { AStr *s = v_str_of(a, POP()); u16 c; CHK(); c = s->len ? STR_AT(s, 0) : 0;
                 if (a->swf_version < 6) c = (u8)c; else if (c >= 0xD800 && c <= 0xDFFF) c = 0xFFFD;
                 PUSH(v_num(c)); return FC_CONTINUE; }
    case 0x37: { u16 c = v_u16(a, POP()); CHK();
                 if (c == 0) PUSH(v_str(vm->s_empty)); else { u16 u = (c >= 0xD800 && c <= 0xDFFF) ? 0xFFFD : c; PUSH(v_str(str_from_units(vm, &u, 1))); }
                 return FC_CONTINUE; }
    case 0x3A: {                                                       /* Delete */
        AStr *name = v_str_of(a, POP()); Value ov; AObj *o; CHK();
        ov = POP(); o = v_obj_of(a, ov);
        PUSH(v_bool(o ? o_delete(a, o, name) : 0));
        return FC_CONTINUE; }
    case 0x3B: {                                                       /* Delete2 */
        AStr *name = v_str_of(a, POP()); int ok; long sep; CHK();
        sep = str_rfind_any(name, ":.");
        if (sep >= 0) {
            Value var = act_get_variable(a, str_slice(vm, name, 0, (u32)sep), NULL); AObj *o; CHK();
            o = v_obj_of(a, var);
            if (o) ok = o_delete(a, o, str_slice(vm, name, (u32)sep + 1, name->len));
            else { ok = 0; if (var.t != V_NULL && var.t != V_UNDEF) avm1_trace_out("Parameters of primitive types are no longer coerced into the required type - Object."); }
        } else ok = scope_delete(a, a->scope, name);
        PUSH(v_bool(ok));
        return FC_CONTINUE; }
    case 0x3C: { Value v = POP(); AStr *name = v_str_of(a, POP()); CHK(); define_local(a, name, v); CHK(); return FC_CONTINUE; }
    case 0x3D: {                                                       /* CallFunction */
        Value nv = POP(); AStr *name = v_str_of(a, nv); u32 n; Value local[16], *args, fv, rv; AObj *owner = NULL, *f; CHK();
        n = v_u32(a, POP()); CHK();
        if (n > (u32)vm->sp) n = (u32)vm->sp;
        args = alloc_args((int)n, local, 16); if (!args) return FC_RETURN;
        pop_args(a, (int)n, args);
        fv = act_get_variable(a, name, &owner);
        if (a1_dbg_calls()) fprintf(stderr, "[a1] call %s/%u%s\n", str_c(vm, name), n, fv.t == V_OBJ ? "" : " (not a function)");
        if (vm->err) { if (args != local) free(args); return FC_RETURN; }
        f = v_obj_of(a, fv);
        if (f) {
            Value self;
            if (owner) self = v_obj(owner);
            else { MovieClip *t = act_target_or_root(a); clip_obj_or_undef_v(vm, t, &self); }
            rv = o_call(a, f, self, (int)n, args);
        } else rv = v_undef();
        if (args != local) free(args);
        CHK();
        stack_push(a, rv);
        return continue_if_base(a); }
    case 0x3E: r->ret = POP(); return FC_RETURN + 1;                    /* Return (explicit) */
    case 0x3F: { double x = v_num_of(a, POP()); CHK(); { double y = v_num_of(a, POP()); CHK(); PUSH(v_num(fmod(y, x))); } return FC_CONTINUE; }
    case 0x40: {                                                       /* NewObject */
        AStr *name = v_str_of(a, POP()); u32 n; Value local[16], *args, cv, res; AObj *ctor; CHK();
        n = v_u32(a, POP()); CHK();
        if (n > (u32)vm->sp) n = (u32)vm->sp;
        args = alloc_args((int)n, local, 16); if (!args) return FC_RETURN;
        pop_args(a, (int)n, args);
        cv = act_resolve(a, name, NULL);
        if (vm->err) { if (args != local) free(args); return FC_RETURN; }
        if (a->swf_version < 5 && !(cv.t == V_OBJ && o_as_function(cv.u.o)) && vm->trace) {   /* (the debug player's warning) */
            char *buf = (char*)malloc(name->len * 3 + 40);
            if (buf) { sprintf(buf, "Warning: %s is not a function", str_c(vm, name)); avm1_warning(vm, buf); free(buf); }
        }
        ctor = v_to_obj_or_bare(a, cv);
        res = vm->err ? v_undef() : o_construct(a, ctor, (int)n, args);
        if (args != local) free(args);
        CHK();
        stack_push(a, res);
        return continue_if_base(a); }
    case 0x41: {                                                       /* DefineLocal2 */
        AStr *name = v_str_of(a, POP()); CHK();
        if (!in_local_scope(a) && str_find_any(name, ":.", 0) >= 0) {
            Value v = act_get_variable(a, name, NULL); CHK();
            if (v.t == V_UNDEF) { act_set_variable(a, name, v_undef()); CHK(); }
        } else if (!o_has_property(a, a->scope->values, name)) { scope_define_local(a, a->scope, name, v_undef()); CHK(); }
        return FC_CONTINUE; }
    case 0x42: {                                                       /* InitArray */
        double n = v_num_of(a, POP()); CHK();
        if (n < 0 || n > 2147483647.0 || n != n) PUSH(v_undef());
        else {
            int cnt = (int)n, i; Value *items = (Value*)malloc((size_t)(cnt ? cnt : 1) * sizeof(Value));
            if (!items) return FC_RETURN;
            for (i = 0; i < cnt; i++) items[i] = POP();
            PUSH(v_obj(obj_array(a, cnt, items)));
            free(items);
        }
        return FC_CONTINUE; }
    case 0x43: {                                                       /* InitObject */
        double n = v_num_of(a, POP()); CHK();
        if (n < 0 || n > 2147483647.0 || n != n) stack_push(a, v_undef());
        else {
            AObj *o = a1_new_with_proto(a, "Object"); int i, cnt = (int)n;
            for (i = 0; i < cnt; i++) { Value v = POP(); AStr *k = v_str_of(a, POP()); CHK(); o_set(a, o, k, v); CHK(); }
            stack_push(a, v_obj(o));
        }
        return FC_CONTINUE; }
    case 0x44: { Value v = POP(); PUSH(v_str(v_typeof(a, v))); return FC_CONTINUE; }
    case 0x45: {                                                       /* TargetPath */
        AObj *o = v_to_obj_or_bare(a, POP()); MovieClip *d; CHK();
        d = o_display(o);
        PUSH(d ? v_str(clip_path(vm, d)) : v_undef());
        return FC_CONTINUE; }
    case 0x46: case 0x55: {                                            /* Enumerate / Enumerate2 */
        Value v; AObj *o;
        if (op == 0x46) { AStr *name = v_str_of(a, POP()); CHK(); v = act_get_variable(a, name, NULL); CHK(); }
        else v = POP();
        PUSH(v_undef());
        o = v_obj_of(a, v);
        if (o) {
            int n, i; AStr **keys = o_keys(a, o, 0, &n);
            for (i = n - 1; i >= 0; i--) PUSH(v_str(keys[i]));
            free(keys);
        }
        return FC_CONTINUE; }
    case 0x47: {                                                       /* Add2 */
        Value rx = POP(), ry = POP(), x, y; int shortv;
        /* the unset value of a guarded getter-setter (SWF5/6) is a string of length -1 to Flash:
           a concatenation with it loses its last character */
        shortv = (rx.t == V_UNDEF && rx.u.b == A1_UNSET_MARK) || (ry.t == V_UNDEF && ry.u.b == A1_UNSET_MARK);
        x = v_prim(a, rx); CHK();
        y = v_prim(a, ry); CHK();
        if (x.t == V_STR || y.t == V_STR) {
            AStr *sy = v_str_of(a, y), *sx, *r; CHK();
            sx = v_str_of(a, x); CHK();
            r = str_concat(vm, sy, sx);
            if (shortv && r->len) r = str_slice(vm, r, 0, r->len - 1);
            PUSH(v_str(r));
        } else { double ny = v_num_of(a, y), nx; CHK(); nx = v_num_of(a, x); CHK(); PUSH(v_num(ny + nx)); }
        return FC_CONTINUE; }
    case 0x48: { Value x = POP(), y = POP(); Value r2 = v_lt(a, y, x); CHK(); PUSH(r2); return FC_CONTINUE; }
    case 0x49: { Value x = POP(), y = POP(); int e = v_eq(a, y, x); CHK(); PUSH(v_bool(e)); return FC_CONTINUE; }
    case 0x4A: { double d = v_num_of(a, POP()); CHK(); PUSH(v_num(d)); return FC_CONTINUE; }
    case 0x4B: { AStr *s = v_str_of(a, POP()); CHK(); PUSH(v_str(s)); return FC_CONTINUE; }
    case 0x4C: { Value v = POP(); PUSH(v); PUSH(v); return FC_CONTINUE; }
    case 0x4D: { Value x = POP(), y = POP(); PUSH(x); PUSH(y); return FC_CONTINUE; }
    case 0x4E: {                                                       /* GetMember */
        AStr *name = v_str_of(a, POP()); Value ov, res; AObj *o; CHK();
        ov = POP();
        o = v_to_obj_or_bare(a, ov); CHK();
        res = o_get(a, o, name); CHK();
        stack_push(a, res);
        return FC_CONTINUE; }
    case 0x4F: {                                                       /* SetMember */
        Value v = POP(); AStr *name = v_str_of(a, POP()); AObj *o; CHK();
        o = v_to_obj_or_bare(a, POP()); CHK();
        o_set(a, o, name, v); CHK();
        return FC_CONTINUE; }
    case 0x50: { double d = v_num_of(a, POP()); CHK(); PUSH(v_num(d + 1.0)); return FC_CONTINUE; }
    case 0x51: { double d = v_num_of(a, POP()); CHK(); PUSH(v_num(d - 1.0)); return FC_CONTINUE; }
    case 0x52: case 0x53: {                                            /* CallMethod / NewMethod */
        Value mn = POP(), ov = POP(); u32 n; Value local[16], *args, res = v_undef(); AObj *o; AStr *name;
        n = v_u32(a, POP()); CHK();
        if (n > (u32)vm->sp) n = (u32)vm->sp;
        args = alloc_args((int)n, local, 16); if (!args) return FC_RETURN;
        pop_args(a, (int)n, args);
        if (ov.t == V_UNDEF || ov.t == V_NULL) { if (args != local) free(args); if (op == 0x52) stack_push(a, v_undef()); else PUSH(v_undef()); return FC_CONTINUE; }
        o = v_to_obj_or_bare(a, ov);
        if (!vm->err) {
            name = mn.t == V_UNDEF ? vm->s_empty : v_str_of(a, mn);
            if (a1_dbg_calls() && !vm->err) { fprintf(stderr, "[a1] %s .%s/%u", op == 0x52 ? "callmethod" : "new", str_c(vm, name), n); if (n > 0 && args[0].t == V_STR) fprintf(stderr, " '%s'", str_c(vm, args[0].u.s)); fputc('\n', stderr); }
            if (!vm->err) {
                if (op == 0x52) {
                    if (name->len == 0) res = o_call(a, o, v_undef(), (int)n, args);
                    else res = o_call_method(a, o, name, (int)n, args, ER_FUNCTION);
                } else {
                    if (name->len == 0) res = o_construct(a, o, (int)n, args);
                    else {
                        Value c = o_get(a, o, name);
                        if (!vm->err && c.t == V_OBJ) res = o_construct(a, c.u.o, (int)n, args);
                    }
                }
            }
        }
        if (args != local) free(args);
        CHK();
        stack_push(a, res);
        return continue_if_base(a); }
    case 0x54: { Value c = POP(), o = POP(); int r2 = v_instanceof(a, o, c); CHK(); PUSH(v_bool(r2)); return FC_CONTINUE; }
    case 0x60: { s32 x = v_i32(a, POP()), y; CHK(); y = v_i32(a, POP()); CHK(); PUSH(v_num((double)(x & y))); return FC_CONTINUE; }
    case 0x61: { s32 x = v_i32(a, POP()), y; CHK(); y = v_i32(a, POP()); CHK(); PUSH(v_num((double)(x | y))); return FC_CONTINUE; }
    case 0x62: { s32 x = v_i32(a, POP()), y; CHK(); y = v_i32(a, POP()); CHK(); PUSH(v_num((double)(y ^ x))); return FC_CONTINUE; }
    case 0x63: { s32 x = v_i32(a, POP()) & 31, y; CHK(); y = v_i32(a, POP()); CHK(); PUSH(v_num((double)(s32)((u32)y << x))); return FC_CONTINUE; }
    case 0x64: { u32 x = v_u32(a, POP()) & 31; s32 y; CHK(); y = v_i32(a, POP()); CHK(); PUSH(v_num((double)(y >> x))); return FC_CONTINUE; }
    case 0x65: { u32 x = v_u32(a, POP()) & 31, y, res; CHK(); y = v_u32(a, POP()); CHK(); res = y >> x;
                 PUSH(v_num(a->swf_version == 8 || a->swf_version == 9 ? (double)(s32)res : (double)res)); return FC_CONTINUE; }
    case 0x66: { Value x = POP(), y = POP(); PUSH(v_bool(v_same(x, y))); return FC_CONTINUE; }
    case 0x67: { Value x = POP(), y = POP(); Value r2 = v_lt(a, x, y); CHK(); PUSH(r2); return FC_CONTINUE; }
    case 0x68: { Value x = POP(), y = POP(); AStr *sy = v_str_of(a, y), *sx; CHK(); sx = v_str_of(a, x); CHK(); PUSH(v_bool(str_cmp(sy, sx) > 0)); return FC_CONTINUE; }
    case 0x69: {                                                       /* Extends */
        AObj *sup = v_to_obj_or_bare(a, POP()), *sub, *np; Value sp; CHK();
        sub = v_to_obj_or_bare(a, POP()); CHK();
        sp = o_get(a, sup, vm->s_prototype); CHK();
        np = obj_new_v(vm, sp);
        /* Flash (unlike Ruffle) does not give the new prototype a `constructor` */
        if (a->swf_version >= 6) o_define(vm, np, vm->s___constructor__, v_obj(sup), PA_DONT_ENUM);
        o_set(a, sub, vm->s_prototype, v_obj(np)); CHK();
        return FC_CONTINUE; }

    /* ---- with payload ---- */
    case 0x81: {                                                       /* GotoFrame */
        MovieClip *c = act_target(a);
        if (c && clip_is_movieclip(c) && plen >= 2) { extern void avm1_goto(MovieClip *c, int frame1, int stop); avm1_goto(c, rd16(p) + 1, 1); }
        return FC_CONTINUE; }
    case 0x83: {                                                       /* GetURL */
        u32 pos = 0; AStr *url = pstr(a, p, &pos, plen), *target = pstr(a, p, &pos, plen);
        return do_get_url(a, url, target); }
    case 0x87: { Value v = POP(); PUSH(v); if (plen >= 1) set_register(a, p[0], v); return FC_CONTINUE; }
    case 0x88: {                                                       /* ConstantPool */
        int n = plen >= 2 ? rd16(p) : 0, i; u32 pos = 2; CPool *cp = (CPool*)gc_alloc(vm, sizeof(CPool), GC_POOL);
        if (!cp) return FC_RETURN;
        cp->v = (Value*)calloc((size_t)(n ? n : 1), sizeof(Value)); cp->n = 0;
        if (!cp->v) return FC_RETURN;
        for (i = 0; i < n && pos < plen; i++) { cp->v[i] = v_str(str_intern_str(vm, pstr(a, p, &pos, plen))); cp->n = i + 1; }
        vm->cpool = cp; a->pool = cp;
        return FC_CONTINUE; }
    case 0x8A: {                                                       /* WaitForFrame */
        int frame = plen >= 2 ? rd16(p) : 0, skip = plen >= 3 ? p[2] : 0, loaded = 1;
        MovieClip *c = act_target(a);
        if (frame > 16000) loaded = 0;
        else if (c && clip_is_movieclip(c)) { extern int avm1_frames_loaded(MovieClip *c); int hf = c->frame_count; loaded = avm1_frames_loaded(c) >= (frame < hf ? frame : hf); }
        if (!loaded) { int k; for (k = 0; k < skip && r->pc < r->base_len; k++) { u8 o2 = code[r->pc]; r->pc += o2 >= 0x80 && r->pc + 3 <= r->base_len ? 3u + rd16(code + r->pc + 1) : 1u; } }
        return FC_CONTINUE; }
    case 0x8B: { u32 pos = 0; AStr *t = pstr(a, p, &pos, plen); return set_target(a, t); }
    case 0x8C: {                                                       /* GotoLabel */
        MovieClip *c = act_target(a); u32 pos = 0; AStr *label = pstr(a, p, &pos, plen);
        if (c && clip_is_movieclip(c)) {
            int f = clip_frame_label(c, label, act_case_sensitive(a));
            if (f >= 0) { extern void avm1_goto(MovieClip *c, int frame1, int stop); avm1_goto(c, f + 1, 1); }
        }
        return FC_CONTINUE; }
    case 0x8D: {                                                       /* WaitForFrame2 */
        Value fv = POP(); int skip = plen >= 1 ? p[0] : 0, loaded = 1; s32 fn; MovieClip *c = act_target(a);
        if (fv.t == V_NUM && fv.u.n == floor(fv.u.n)) fn = f64_i32(fv.u.n);
        else { AStr *s = v_str_of(a, fv); double d; CHK(); d = str_parse_float(s, 1); fn = (d == d && d == floor(d)) ? f64_i32(d) : 0; }
        fn = (s32)((u32)fn - 1u);
        fn = fn == 2147483647 ? fn : fn + 1;
        if (fn > 16001) loaded = 0;
        else if (c && clip_is_movieclip(c)) { extern int avm1_frames_loaded(MovieClip *c); int hf = c->frame_count; loaded = avm1_frames_loaded(c) + 1 >= ((u16)fn < hf ? (u16)fn : hf); }
        if (!loaded) { int k; for (k = 0; k < skip && r->pc < r->base_len; k++) { u8 o2 = code[r->pc]; r->pc += o2 >= 0x80 && r->pc + 3 <= r->base_len ? 3u + rd16(code + r->pc + 1) : 1u; } }
        return FC_CONTINUE; }
    case 0x8E: return define_function(a, r, p, plen, 1);
    case 0x9B: return define_function(a, r, p, plen, 0);
    case 0x8F: return try_action(a, r, p, plen);
    case 0x94: return with_action(a, r, plen >= 2 ? rd16(p) : 0);
    case 0x96: {                                                       /* Push */
        u32 pos = 0;
        while (pos < plen) {
            u8 t = p[pos++];
            switch (t) {
            case 0: stack_push(a, v_str(pstr(a, p, &pos, plen))); break;
            case 1: { union { u32 i; float f; } u; if (pos + 4 > plen) { pos = plen; break; } u.i = rd32(p + pos); stack_push(a, v_num(u.f)); pos += 4; break; }
            case 2: PUSH(v_null()); break;
            case 3: PUSH(v_undef()); break;
            case 4: if (pos < plen) { stack_push(a, get_register(a, p[pos])); pos++; } break;
            case 5: if (pos < plen) { PUSH(v_bool(p[pos] != 0)); pos++; } break;
            case 6: { union { u64 i; double d; } u; if (pos + 8 > plen) { pos = plen; break; } u.i = ((u64)rd32(p + pos) << 32) | rd32(p + pos + 4); PUSH(v_num(u.d)); pos += 8; break; }
            case 7: if (pos + 4 <= plen) { PUSH(v_num((double)(s32)rd32(p + pos))); pos += 4; } else pos = plen; break;
            case 8: if (pos < plen) { int i = p[pos]; pos++; stack_push(a, (a->pool && i < a->pool->n) ? a->pool->v[i] : v_undef()); } break;
            case 9: if (pos + 2 <= plen) { int i = rd16(p + pos); pos += 2; stack_push(a, (a->pool && i < a->pool->n) ? a->pool->v[i] : v_undef()); } else pos = plen; break;
            default: vm->err = ERR_INVALID; return FC_RETURN;
            }
        }
        return FC_CONTINUE; }
    case 0x99: { s16 off = (s16)rd16(p); long np = (long)r->pc + off; if (np < 0) np = (long)r->base_len; r->pc = (u32)np; return FC_CONTINUE; }
    case 0x9A: return do_get_url2(a, plen >= 1 ? p[0] : 0);
    case 0x9D: { s16 off = (s16)rd16(p); Value c = POP(); if (v_bool_of(a, c)) { long np = (long)r->pc + off; if (np < 0) np = (long)r->base_len; r->pc = (u32)np; } return FC_CONTINUE; }
    case 0x9E: { Value v = POP(); return frame_call(a, r, v); }
    case 0x9F: {                                                       /* GotoFrame2 */
        MovieClip *c = act_target_or_root(a); Value fv = POP(); u8 flags = plen >= 1 ? p[0] : 0; int bias = (flags & 2) && plen >= 3 ? rd16(p + 1) : 0;
        if (c && clip_is_movieclip(c)) clip_goto_value(a, c, fv, flags & 1, bias);
        CHK();
        return FC_CONTINUE; }
    default:
        return FC_CONTINUE;
    }
}

static Value act_run_body(Act *a, const u8 *base, u32 base_len, u32 start, u32 len) {
    int ret = 0;
    Value v = act_run(a, base, base_len, start, len, &ret);
    return ret ? v : v_undef();
}

/* timeline code runs inside its movie's buffer: obfuscators branch out of the
   action block into neighbouring tag bytes, which Flash Player executes */
/* FLASHNT_A1CALLS=1: log the functions and methods ActionScript 1/2 code calls */
static int a1_dbg_calls(void) { static int v = -1; if (v < 0) v = getenv("FLASHNT_A1CALLS") != NULL; return v; }
Value act_run_code(Act *a, const struct SWFMovie *m, const u8 *code, u32 len, int *returned) {
    if (m && m->body && code >= m->body && code + len <= m->body + m->bodylen && m->bodylen < 0xFFFFFFF0u)
        return act_run(a, m->body, (u32)m->bodylen, (u32)(code - m->body), len, returned);
    return act_run(a, code, len, 0, len, returned);
}
Value act_run(Act *a, const u8 *base, u32 base_len, u32 start, u32 len, int *returned) {
    Run r; int fc;
    r.base = base; r.base_len = base_len; r.pc = start; r.end = start + len; r.ret = v_undef();
    if (r.end > base_len || r.end < start) r.end = base_len;
    *returned = 0;
    for (;;) {
        if (a->vm->halted) { if (!a->vm->err) a->vm->err = ERR_TIMEOUT; return v_undef(); }
        fc = do_action(a, &r);
        if (fc == FC_CONTINUE) continue;
        if (fc == FC_RETURN + 1) { *returned = 1; return r.ret; }
        return v_undef();
    }
}

/* ---------------------------------------------------------------- */
/* complex actions                                                  */

static int define_function(Act *a, Run *r, const u8 *p, u32 plen, int v2) {
    VM *vm = a->vm; AFunc *f = (AFunc*)gc_alloc(vm, sizeof(AFunc), GC_FN); u32 pos = 0; AStr *name; int np, i; u16 codesize;
    AObj *fo, *proto; Value fn_proto;
    if (!f) return FC_RETURN;
    f->kind = FK_ACTION; f->swf_version = a->swf_version;
    name = pstr(a, p, &pos, plen);
    np = pos + 2 <= plen ? rd16(p + pos) : 0; pos += 2;
    f->params = (AParam*)calloc((size_t)(np ? np : 1), sizeof(AParam)); f->nparams = np;
    if (!f->params) return FC_RETURN;
    if (v2) {
        f->nregs = pos < plen ? p[pos] : 0; pos++;
        f->flags = pos + 2 <= plen ? rd16(p + pos) : 0; pos += 2;
        for (i = 0; i < np && pos < plen; i++) { f->params[i].reg = p[pos++]; f->params[i].name = pstr(a, p, &pos, plen); }
    } else {
        f->nregs = 0; f->flags = 0;
        for (i = 0; i < np && pos < plen; i++) f->params[i].name = pstr(a, p, &pos, plen);
    }
    f->nparams = i;
    codesize = pos + 2 <= plen ? rd16(p + pos) : 0;
    f->base = r->base; f->base_len = r->base_len; f->start = r->pc; f->len = codesize;
    if (f->start + f->len > r->base_len) f->len = r->base_len - f->start;
    r->pc += f->len;
    f->name = name->len ? name : NULL;
    f->scope = a->scope; f->pool = a->pool;
    {
        AObj *bc = clip_obj(vm, a->base_clip); Value rv = bc ? clipref_value(a, bc) : v_undef();
        if (rv.t == V_CLIP) f->base_clip = rv.u.c;
        else if (bc && o_display(bc)) {
            /* non movie clip base (button / text): reference its path anyway */
            f->base_clip = clipref_from_path(vm, clip_path(vm, o_display(bc)));
        } else f->base_clip = clipref_from_path(vm, str_intern(vm, "_root"));
    }
    proto = obj_new(vm, PROTOS(a)->object);
    fo = obj_bare(vm);
    /* Flash gives each defined function its own `constructor` (Function), before
     * __proto__ (not the functions of the built-in AS2 library, as Ruffle) */
    {
        extern const unsigned char avm1_playerglobal[]; extern const unsigned long avm1_playerglobal_len;
        extern AObj *a1_resolve_class(Act *a, const char *path);
        int builtin = f->base >= avm1_playerglobal && f->base < avm1_playerglobal + avm1_playerglobal_len;
        AObj *fc = builtin ? NULL : a1_resolve_class(a, "Function");
        if (fc && fc->nk != NK_SUPER) o_define(vm, fo, vm->s_constructor, v_obj(fc), PA_DONT_ENUM);
    }
    if (a1_resolve_proto(a, "Function", &fn_proto)) o_define(vm, fo, vm->s_proto, fn_proto, PA_DONT_ENUM | PA_DONT_DELETE | PA_VERSION_6);
    fo->nk = NK_FUNCTION; fo->nat.fn = f;
    o_define(vm, proto, vm->s_constructor, v_obj(fo), PA_DONT_ENUM);
    o_define(vm, fo, vm->s_prototype, v_obj(proto), PA_DONT_ENUM | PA_DONT_DELETE);
    if (f->name) { define_local(a, f->name, v_obj(fo)); if (vm->err) return FC_RETURN; }
    else vm_push(vm, v_obj(fo));
    return FC_CONTINUE;
}

static int try_action(Act *a, Run *r, const u8 *p, u32 plen) {
    VM *vm = a->vm; u8 flags; u16 tsz, csz, fsz; u32 pos = 7, try_start, catch_start, finally_start;
    int catch_in_reg, catch_reg = 0, has_catch, has_finally, ret = 0, fret = 0; AStr *catch_name = NULL; Value result, fres;
    int orig_sp = vm->sp, err; Value thrown;
    if (plen < 7) return FC_CONTINUE;
    flags = p[0]; tsz = rd16(p + 1); csz = rd16(p + 3); fsz = rd16(p + 5);
    catch_in_reg = (flags & 4) != 0;
    if (catch_in_reg) { catch_reg = pos < plen ? p[pos] : 0; }
    else catch_name = pstr(a, p, &pos, plen);
    has_catch = (flags & 1) != 0; has_finally = (flags & 2) != 0;
    try_start = r->pc; catch_start = try_start + tsz; finally_start = catch_start + (has_catch ? csz : 0);
    r->pc = finally_start + (has_finally ? fsz : 0);
    result = act_run(a, r->base, r->base_len, try_start, tsz, &ret);
    if (has_catch && vm->err == ERR_THROWN) {
        Act ca; Value v = vm->thrown;
        vm->err = 0; vm->thrown = v_undef();
        if (vm->sp > orig_sp) vm->sp = orig_sp;
        act_child(&ca, a);
        ca.target_clip = a->base_clip; ca.has_target = a->base_clip != NULL;
        if (catch_in_reg) set_register(&ca, catch_reg, v);
        else act_set_variable(&ca, catch_name, v);
        if (!vm->err) result = act_run(&ca, r->base, r->base_len, catch_start, csz, &ret);
    }
    if (has_finally) {
        err = vm->err; thrown = vm->thrown;
        vm->err = 0;
        fres = act_run(a, r->base, r->base_len, finally_start, fsz, &fret);
        if (vm->err) return FC_RETURN;
        if (fret) { r->ret = fres; return FC_RETURN + 1; }
        vm->err = err; vm->thrown = thrown;
    }
    if (vm->err) return FC_RETURN;
    if (ret) { r->ret = result; return FC_RETURN + 1; }
    return FC_CONTINUE;
}

static int with_action(Act *a, Run *r, u32 size) {
    VM *vm = a->vm; Value v = vm_pop(vm); u32 start = r->pc; int ret = 0; Value res;
    r->pc = start + size;
    if (v.t == V_UNDEF || v.t == V_NULL) return FC_CONTINUE;
    {
        AObj *o = v_to_obj_or_bare(a, v); Act wa;
        if (vm->err) return FC_RETURN;
        act_child(&wa, a);
        wa.scope = scope_new(vm, a->scope, SC_WITH, o);
        res = act_run(&wa, r->base, r->base_len, start, size, &ret);
        if (vm->err) return FC_RETURN;
        if (ret) { r->ret = res; return FC_RETURN + 1; }
    }
    return FC_CONTINUE;
}

/* FLASHNT_SWF5_TRACE_BYTES (the Linux player): SWF5 strings are byte strings and the
 * log takes their bytes as they are, so a SWF5 string holding UTF-8 bytes (decoded
 * here one byte per character, code page 1252) shows as that UTF-8 text */
static int swf5_trace_bytes(void) { static int v = -1; if (v < 0) v = getenv("FLASHNT_SWF5_TRACE_BYTES") != NULL; return v; }
static int trace_as_bytes(VM *vm, AStr *s) {
    extern int str_unit_to_cp1252(u16 c);
    char *b; u32 i, n = 0; int multi = 0;
    if (!s->len) return 0;
    b = (char*)malloc((size_t)s->len + 1);
    if (!b) return 0;
    for (i = 0; i < s->len; i++) { int k = str_unit_to_cp1252(STR_AT(s, i)); if (k < 0) { free(b); return 0; } b[n++] = (char)k; }
    b[n] = 0;
    /* only when the bytes are valid UTF-8 with at least one multibyte sequence */
    for (i = 0; i < n; ) {
        u8 c = (u8)b[i]; int k, j;
        if (c < 0x80) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) k = 1; else if (c >= 0xE0 && c <= 0xEF) k = 2; else if (c >= 0xF0 && c <= 0xF4) k = 3; else { free(b); return 0; }
        for (j = 1; j <= k; j++) if (i + (u32)j >= n || ((u8)b[i + j] & 0xC0) != 0x80) { free(b); return 0; }
        multi = 1; i += (u32)k + 1;
    }
    if (multi && vm->trace) avm1_trace_out(b);
    free(b);
    return multi;
}
static void action_trace(Act *a, Value v) {
    VM *vm = a->vm;
    if (v.t == V_UNDEF) { avm1_trace_out("undefined"); return; }
    {
        AStr *s = v_str_of(a, v);
        if (vm->err) { avm1_trace_out("[type Object]"); return; }
        if (!(swf5_trace_bytes() && trace_as_bytes(vm, s))) avm1_trace_str(vm, s);
    }
}

static int frame_call(Act *a, Run *r, Value v) {
    extern void avm1_run_frame_actions(Act *a, MovieClip *c, int frame);
    VM *vm = a->vm; MovieClip *target = act_target_or_root(a), *clip = NULL; s32 frame = -1;
    if (v.t == V_NUM) { if (target && clip_is_movieclip(target)) { clip = target; frame = (s32)f64_u32(v.u.n); } }
    else {
        AStr *path = v_str_of(a, v); long sep;
        if (vm->err) return FC_RETURN;
        sep = str_rfind_any(path, ":.");
        {
            AObj *o = NULL; AStr *fr;
            if (sep >= 0) {
                Scope *s; AStr *pp = str_slice(vm, path, 0, (u32)sep);
                fr = str_slice(vm, path, (u32)sep + 1, path->len);
                for (s = a->scope; s; s = s->parent) { o = act_resolve_target_path(a, clip_avm1_root(target), s->values, pp, 1, 1); if (vm->err) return FC_RETURN; if (o) break; }
            } else { o = clip_obj(vm, target); fr = path; }
            if (o && o_display(o) && clip_is_movieclip(o_display(o))) {
                double d = str_parse_float(fr, 1);
                clip = o_display(o);
                if (d == d && fr->len) frame = (s32)f64_u32(d);
                else { int f = clip_frame_label(clip, fr, act_case_sensitive(a)); frame = f >= 0 ? f + 1 : -1; if (f < 0) clip = NULL; }
            }
        }
    }
    if (clip && frame >= 0 && frame <= 65535) avm1_run_frame_actions(a, clip, frame);
    if (vm->err) return FC_RETURN;
    return continue_if_base(a);
}

/* ---- getURL ---- */

extern void avm1_navigate(Act *a, AStr *url, AStr *target, int method, AObj *vars, int normalize);
extern void avm1_navigate_locals(Act *a, AStr *url, AStr *target, int method);
extern void avm1_load_movie(Act *a, MovieClip *target, AStr *url, int method);
extern void avm1_unload_movie(Act *a, MovieClip *target);
extern void avm1_load_vars_into(Act *a, AObj *target, AStr *url, int method);
extern int  avm1_fscommand(Act *a, AStr *url, AStr *target);

static int level_of_target(Act *a, AStr *t) {
    if (str_starts_c(t, "_level") && t->len >= 6) {
        AStr *rest = str_slice(a->vm, t, 6, t->len); double d;
        if (t->len == 6) return 0;
        d = str_parse_float(rest, 1);
        if (d == d) return (int)d;
        return -1;
    }
    return -1;
}

static int do_get_url(Act *a, AStr *url, AStr *target) {
    if (avm1_fscommand(a, url, target)) return FC_CONTINUE;
    if (str_starts_c(target, "_level") && target->len > 6) {
        double d = str_parse_float(str_slice(a->vm, target, 6, target->len), 1);
        if (d == d && d == floor(d)) {
            int lv = (int)d;
            if (url->len == 0) { MovieClip *l = act_get_level(a, lv); if (l) avm1_unload_movie(a, l); }
            else avm1_load_movie(a, act_get_or_create_level(a, lv), url, 0);
            return FC_CONTINUE;
        }
    }
    avm1_navigate(a, url, target, 0, NULL, 1);
    return FC_CONTINUE;
}

static int do_get_url2(Act *a, u8 flags) {
    VM *vm = a->vm; Value tv = vm_pop(vm), uv; AStr *target, *url; int level, method = flags & 3, load_target = (flags & 0x40) != 0, load_vars = (flags & 0x80) != 0;
    MovieClip *clip = NULL;
    target = v_str_of(a, tv); if (vm->err) return FC_RETURN;
    uv = vm_pop(vm);
    url = v_str_of(a, uv); if (vm->err) return FC_RETURN;
    if (avm1_fscommand(a, url, target)) return FC_CONTINUE;
    level = level_of_target(a, target);
    if (level > -1) clip = act_get_level(a, level);
    else if (load_vars || load_target) {
        AObj *to = v_obj_of(a, tv);
        if (to) clip = o_display(to);
        else { clip = act_resolve_target_display(a, act_target_or_root(a), tv, 1); if (vm->err) return FC_RETURN; }
    }
    if (load_vars) {
        int is_lv = 1;
        if (!(load_target || level > -1)) {
            is_lv = 0;
            if ((tv.t == V_OBJ || tv.t == V_CLIP) && clip) is_lv = clip == clip_avm1_root(a->base_clip);
        }
        if (is_lv) { if (clip) avm1_load_vars_into(a, clip_obj(vm, clip), url, method); return FC_CONTINUE; }
    } else if (load_target) {
        if (url->len == 0) { if (clip && clip_is_movieclip(clip)) avm1_unload_movie(a, clip); }
        else {
            if (!clip && level > -1) clip = act_get_or_create_level(a, level);
            if (clip) avm1_load_movie(a, clip, url, method);
        }
        return FC_CONTINUE;
    } else if (level > -1) {
        if (!clip) clip = act_get_or_create_level(a, level);
        if (clip) {
            if (url->len == 0) { if (clip_is_movieclip(clip)) avm1_unload_movie(a, clip); }
            else avm1_load_movie(a, clip, url, 0);
        }
        return FC_CONTINUE;
    }
    avm1_navigate_locals(a, url, target, method);
    return FC_CONTINUE;
}

Value act_super_for(Act *a) { (void)a; return v_undef(); }
