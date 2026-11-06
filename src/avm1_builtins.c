/* avm1_builtins.c - Array, String, Number, Boolean and Math. */
#include "avm1_int.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define DE  PA_DONT_ENUM
#define DD  PA_DONT_DELETE
#define RO  PA_READ_ONLY

extern AObj *lib_class(VM *vm, Env *env, NativeFn ctor, NativeFn call, AObj *super_proto, int proto_first, AObj **proto_out);

static double dnan(void) { double z = 0; return z / z; }
static double dinf(void) { double z = 0; return 1.0 / z; }

/* the native object behind `super` */
static AObj *nat_of(AObj *o) { o = o && o->nk == NK_SUPER ? o->nat.sup.self : o; return o && o->relay ? o->relay : o; }

static void defm(VM *vm, Env *env, AObj *o, const char *name, NativeFn f, u16 attr) { o_define(vm, o, str_intern(vm, name), v_obj(fn_native(vm, env, f, NULL)), attr); }
static void deft(VM *vm, Env *env, AObj *o, const char *name, TableFn f, int index, u16 attr) { o_define(vm, o, str_intern(vm, name), v_obj(fn_table(vm, env, f, index)), attr); }

/* ---------------------------------------------------------------- */
/* ArrayBuilder: arrays created by natives use the current _global.Array */

typedef struct ABuild { Value proto; AObj *ctor; int has_proto; } ABuild;

static void ab_new(Act *a, ABuild *b) {
    /* raw data properties only: no getters, no prototype chain */
    VM *vm = a->vm; Value c, p;
    extern int avm1_player_major(void);
    b->has_proto = 0; b->ctor = NULL; b->proto = v_undef();
    if (!o_get_data(a, act_global(a), vm->s_Array, &c) || c.t != V_OBJ) return;
    b->ctor = c.u.o;
    if (o_get_data(a, c.u.o, vm->s_prototype, &p)) { b->proto = p; b->has_proto = 1; }
    /* Flash Player 9-17 leaves out `constructor` when Array has no prototype */
    if (!b->has_proto && avm1_player_major() <= 17) b->ctor = NULL;
}

static void ab_init(Act *a, AObj *o, int n, const Value *items) {
    VM *vm = a->vm; int i;
    for (i = 0; i < n; i++) o_define(vm, o, str_int(vm, i), items[i], 0);
    o_define(vm, o, vm->s_length, v_num(n), DE | DD);
    { AObj *t = o_native_target(o); if (t) t->nk = NK_ARRAY; }
}

static AObj *ab_with(Act *a, ABuild *b, int n, const Value *items) {
    VM *vm = a->vm; AObj *o = obj_bare(vm);
    if (!o) return NULL;
    if (b->has_proto) o_define(vm, o, vm->s_proto, b->proto, DE | DD);
    if (b->ctor) o_define(vm, o, vm->s_constructor, v_obj(b->ctor), DE | DD);
    ab_init(a, o, n, items);
    return o;
}

AObj *obj_array(Act *a, int n, Value *items) { ABuild b; ab_new(a, &b); return ab_with(a, &b, n, items); }

/* growable value list */
typedef struct VList { Value *v; int n, cap; } VList;
static void vl_push(VList *l, Value v) {
    if (l->n == l->cap) { int nc = l->cap ? l->cap * 2 : 16; Value *nv = (Value*)realloc(l->v, (size_t)nc * sizeof(Value)); if (!nv) return; l->v = nv; l->cap = nc; }
    l->v[l->n++] = v;
}

/* ---------------------------------------------------------------- */
/* Array                                                            */

enum { AM_ARRAY = 0, AM_PUSH, AM_POP, AM_CONCAT, AM_SHIFT, AM_UNSHIFT, AM_SLICE, AM_JOIN, AM_SPLICE,
       AM_TO_STRING, AM_SORT, AM_REVERSE, AM_SORT_ON };
enum { SO_CASE_INSENSITIVE = 1, SO_DESCENDING = 2, SO_UNIQUE = 4, SO_RETURN_INDEXED = 8, SO_NUMERIC = 16 };

static s32 clamp_to_i32(double d) { if (d >= -2147483648.0 && d <= 2147483647.0) return (s32)d; return (s32)0x80000000u; }

static Value arr_constructor(Act *a, AObj *self, int argc, Value *argv) {
    {   /* an object of another native type that cannot be retyped stays as it is */
        AObj *real = self->nk == NK_SUPER ? self->nat.sup.self : self;
        if (real->nk != NK_ARRAY && !o_native_retarget(self, NK_ARRAY) && real->nk != NK_NONE) return v_obj(self);
    }
    if (argc == 1 && argv[0].t == V_NUM) {
        ab_init(a, self, 0, NULL);
        o_set_length(a, self, clamp_to_i32(argv[0].u.n));
    } else ab_init(a, self, argc, argv);
    return v_obj(self);
}

static Value arr_function(Act *a, AObj *self, int argc, Value *argv) {
    ABuild b; AObj *o;
    (void)self;
    ab_new(a, &b);
    if (argc == 1 && argv[0].t == V_NUM) {
        o = ab_with(a, &b, 0, NULL);
        if (o) o_set_length(a, o, clamp_to_i32(argv[0].u.n));
        return o ? v_obj(o) : v_undef();
    }
    o = ab_with(a, &b, argc, argv);
    return o ? v_obj(o) : v_undef();
}

static Value arr_push(Act *a, AObj *self, int argc, Value *argv) {
    int old = o_length(a, self), i;
    if (a->vm->err) return v_undef();
    for (i = 0; i < argc; i++) { o_set_element(a, self, old + i, argv[i]); if (a->vm->err) return v_undef(); }
    o_set_length(a, self, old + argc);
    return v_num(old + argc);
}

static Value arr_unshift(Act *a, AObj *self, int argc, Value *argv) {
    int old = o_length(a, self), nl, i;
    if (a->vm->err) return v_undef();
    nl = old + argc;
    for (i = 0; i < old; i++) {
        int from = old - i - 1, to = nl - i - 1;
        Value e = o_get_element(a, self, from);
        o_delete_element(a, self, to);
        o_set_element(a, self, to, e);
        if (a->vm->err) return v_undef();
    }
    for (i = 0; i < argc; i++) { o_delete_element(a, self, i); o_set_element(a, self, i, argv[i]); if (a->vm->err) return v_undef(); }
    if (nat_of(self)->nk == NK_ARRAY) o_set_length(a, self, nl);
    return v_num(nl);
}

static Value arr_shift(Act *a, AObj *self, int argc, Value *argv) {
    int len = o_length(a, self), i; Value first;
    (void)argc; (void)argv;
    if (a->vm->err || len == 0) return v_undef();
    first = o_get_element(a, self, 0);
    for (i = 1; i < len; i++) {
        Value e = o_get_element(a, self, i);
        o_delete_element(a, self, i - 1);
        o_set_element(a, self, i - 1, e);
        if (a->vm->err) return v_undef();
    }
    if (nat_of(self)->nk == NK_ARRAY) o_set_length(a, self, len - 1);
    return first;
}

static Value arr_pop(Act *a, AObj *self, int argc, Value *argv) {
    int len = o_length(a, self); Value last;
    (void)argc; (void)argv;
    if (a->vm->err || len == 0) return v_undef();
    last = o_get_element(a, self, len - 1);
    o_delete_element(a, self, len - 1);
    if (nat_of(self)->nk == NK_ARRAY) o_set_length(a, self, len - 1);
    return last;
}

static Value arr_reverse(Act *a, AObj *self, int argc, Value *argv) {
    int len = o_length(a, self), lo;
    (void)argc; (void)argv;
    if (a->vm->err) return v_undef();
    for (lo = 0; lo < len / 2; lo++) {
        int hi = len - lo - 1;
        Value lv = o_get_element(a, self, lo), hv = o_get_element(a, self, hi);
        o_delete_element(a, self, lo);
        o_delete_element(a, self, hi);
        o_set_element(a, self, lo, hv);
        o_set_element(a, self, hi, lv);
        if (a->vm->err) return v_undef();
    }
    return v_obj(self);
}

static Value join_len(Act *a, AObj *o, int len, AStr *sep) {
    VM *vm = a->vm; int i; SBuf b;
    if (len <= 0) return v_str(vm->s_empty);
    sb_init(&b);
    for (i = 0; i < len; i++) {
        AStr *s = v_str_of(a, o_get_element(a, o, i));
        if (vm->err) { sb_free(&b); return v_undef(); }
        if (i) sb_str(&b, sep);
        sb_str(&b, s);
    }
    return v_str(sb_done(vm, &b));
}

Value array_join(Act *a, AObj *o, AStr *sep) {
    int len = o_length(a, o);
    if (a->vm->err) return v_undef();
    return join_len(a, o, len, sep);
}

static Value arr_join(Act *a, AObj *self, int argc, Value *argv) {
    AStr *sep = str_intern(a->vm, ",");
    int len = o_length(a, self);
    if (a->vm->err) return v_undef();
    if (argc > 0) { sep = v_str_of(a, argv[0]); if (a->vm->err) return v_undef(); }
    return join_len(a, self, len, sep);
}

static int abs_index(int index, int len) {
    if (index < 0) { int r = index + len; return r < 0 ? 0 : r; }
    return index < len ? index : len;
}

static Value arr_slice(Act *a, AObj *self, int argc, Value *argv) {
    int len = o_length(a, self), start, end, i; VList l = { NULL, 0, 0 }; ABuild b; AObj *r;
    if (a->vm->err) return v_undef();
    start = abs_index(v_i32(a, arg(argc, argv, 0)), len); if (a->vm->err) return v_undef();
    if (argc > 1 && argv[1].t != V_UNDEF) { end = abs_index(v_i32(a, argv[1]), len); if (a->vm->err) return v_undef(); }
    else end = len;
    ab_new(a, &b);
    for (i = start; i < end; i++) vl_push(&l, o_get_element(a, self, i));
    r = ab_with(a, &b, l.n, l.v);
    free(l.v);
    return r ? v_obj(r) : v_undef();
}

static void splice_move(Act *a, AObj *self, int del, int nitems, int i) {
    if (o_has_element(a, self, i)) {
        Value e = o_get_element(a, self, i);
        o_set_element(a, self, i - del + nitems, e);
    } else o_delete_element(a, self, i - del + nitems);
}

/* splice (all SWF versions in Flash): every element is read and written back, so holes become undefined
 * properties (trailing old elements stay) */
static Value arr_splice_old(Act *a, AObj *self, int len, int start, int del, int nitems, Value *items) {
    VList all = { NULL, 0, 0 }, l = { NULL, 0, 0 }; ABuild b; AObj *res; int i, nl = len - del + nitems;
    for (i = 0; i < len; i++) { vl_push(&all, o_get_element(a, self, i)); if (a->vm->err) { free(all.v); return v_undef(); } }
    for (i = 0; i < del; i++) vl_push(&l, all.v[start + i]);
    ab_new(a, &b);
    res = ab_with(a, &b, l.n, l.v);
    free(l.v);
    for (i = 0; i < nl && !a->vm->err; i++) {
        Value v = i < start ? all.v[i] : i < start + nitems ? items[i - start] : all.v[i - nitems + del];
        o_set_element(a, self, i, v);
    }
    free(all.v);
    if (!a->vm->err) o_set_length(a, self, nl);
    return res ? v_obj(res) : v_undef();
}

static Value arr_splice(Act *a, AObj *self, int argc, Value *argv) {
    int start, len, del;
    if (argc < 1 || argv[0].t == V_UNDEF) return v_undef();
    start = v_i32(a, argv[0]); if (a->vm->err) return v_undef();
    len = o_length(a, self); if (a->vm->err) return v_undef();
    start = abs_index(start, len);
    if (argc > 1) {
        if (argv[1].t == V_UNDEF) return v_undef();
        del = v_i32(a, argv[1]); if (a->vm->err) return v_undef();
        if (del > len - start) del = len - start;
    } else del = len - start;
    if (del < 0) return v_undef();
    return arr_splice_old(a, self, len, start, del, argc > 2 ? argc - 2 : 0, argc > 2 ? argv + 2 : NULL);
}

static Value arr_concat(Act *a, AObj *self, int argc, Value *argv) {
    VList l = { NULL, 0, 0 }; int k; ABuild b; AObj *r;
    for (k = -1; k < argc; k++) {
        Value v = k < 0 ? v_obj(self) : argv[k];
        if (v.t == V_OBJ) {
            AObj *o = v.u.o; int nk = nat_of(o)->nk;
            if ((nk == NK_NONE && k < 0) || nk == NK_ARRAY) {
                int len = o_length(a, o), i;
                if (a->vm->err) { free(l.v); return v_undef(); }
                for (i = 0; i < len; i++) vl_push(&l, o_get_element(a, o, i));
                continue;
            }
        }
        vl_push(&l, v);
    }
    ab_new(a, &b);
    r = ab_with(a, &b, l.n, l.v);
    free(l.v);
    return r ? v_obj(r) : v_undef();
}

/* sorting */
typedef struct SortKey { AStr *name; int opts; } SortKey;
typedef struct SortCtx {
    Act *a; AObj *custom; SortKey *fields; int nfields; int sort_on;
    unsigned char *touched;      /* SWF5/6: positions the in-place sort wrote */
} SortCtx;
typedef struct SortEl { int index; Value v; } SortEl;

static int cmp_ci(const AStr *x, const AStr *y) {
    u32 i, n = x->len < y->len ? x->len : y->len;
    for (i = 0; i < n; i++) {
        u16 a = swf_lower(STR_AT(x, i)), b = swf_lower(STR_AT(y, i));
        if (a != b) return a < b ? -1 : 1;
    }
    return x->len < y->len ? -1 : x->len > y->len ? 1 : 0;
}

static int sort_compare(Act *a, Value x, Value y, int opts) {
    int r;
    if (x.t == V_NUM && y.t == V_NUM && (opts & SO_NUMERIC)) {
        r = x.u.n < y.u.n ? -1 : x.u.n > y.u.n ? 1 : 0;
    } else {
        AStr *sx = v_str_of(a, x), *sy;
        if (a->vm->err) return 0;
        sy = v_str_of(a, y);
        if (a->vm->err) return 0;
        r = (opts & SO_CASE_INSENSITIVE) ? cmp_ci(sx, sy) : str_cmp(sx, sy);
        r = r < 0 ? -1 : r > 0 ? 1 : 0;
    }
    return (opts & SO_DESCENDING) ? -r : r;
}

static int sort_cmp(SortCtx *c, Value x, Value y, int opts) {
    Act *a = c->a;
    if (c->custom) {
        Value args[2], r; double d;
        args[0] = x; args[1] = y;
        r = o_call(a, c->custom, v_undef(), 2, args);
        if (a->vm->err) return 0;
        d = v_num_of(a, r);
        if (a->vm->err) return 0;
        return d < 0 ? -1 : d > 0 ? 1 : 0;
    }
    if (c->sort_on && c->fields) {
        if (x.t == V_OBJ && y.t == V_OBJ) {
            int i;
            for (i = 0; i < c->nfields; i++) {
                Value ap, bp; int r;
                if (!o_get_local_stored(a, x.u.o, c->fields[i].name, &ap)) ap = v_undef();
                if (!o_get_local_stored(a, y.u.o, c->fields[i].name, &bp)) bp = v_undef();
                r = sort_compare(a, ap, bp, c->fields[i].opts);
                if (a->vm->err) return 0;
                if (r) return r;
            }
            return 0;
        }
        return sort_compare(a, x, y, opts);
    }
    return sort_compare(a, x, y, opts);
}

static void qsort_flash(SortCtx *c, SortEl *e, int n, int opts) {
    int *stack, sp = 0, cap;
    if (n < 2) return;
    cap = 2 * n + 4;
    stack = (int*)malloc((size_t)cap * sizeof(int));
    if (!stack) return;
    stack[sp++] = 0; stack[sp++] = n - 1;
    while (sp > 0) {
        int high = stack[--sp], low = stack[--sp], left, right; Value pivot; SortEl t;
        if (low >= high) continue;
        pivot = e[low].v; left = low + 1; right = high;
        for (;;) {
            while (left < right) { int r = sort_cmp(c, pivot, e[left].v, opts); if (c->a->vm->err) goto done; if (r <= 0) break; left++; }
            while (right > low) { int r = sort_cmp(c, pivot, e[right].v, opts); if (c->a->vm->err) goto done; if (r > 0) break; right--; }
            if (left >= right) break;
            t = e[left]; e[left] = e[right]; e[right] = t;
            if (c->touched) c->touched[left] = c->touched[right] = 1;
        }
        t = e[low]; e[low] = e[right]; e[right] = t;
        if (c->touched && low != right) c->touched[low] = c->touched[right] = 1;
        if (sp + 4 > cap) { int nc = cap * 2; int *ns = (int*)realloc(stack, (size_t)nc * sizeof(int)); if (!ns) goto done; stack = ns; cap = nc; }
        stack[sp++] = right + 1; stack[sp++] = high;
        if (right > 0) { stack[sp++] = low; stack[sp++] = right - 1; }
    }
done:
    free(stack);
}

static Value sort_internal(Act *a, AObj *self, SortCtx *c, int opts, int is_sort_on) {
    VM *vm = a->vm; int len = o_length(a, self), i, desc; SortEl *e; Value result;
    if (vm->err) return v_undef();
    if (len < 0) len = 0;
    e = (SortEl*)malloc((size_t)(len ? len : 1) * sizeof(SortEl));
    if (!e) return v_undef();
    for (i = 0; i < len; i++) { e[i].index = i; e[i].v = o_get_element(a, self, i); }
    desc = (opts & SO_DESCENDING) != 0;
    if (!is_sort_on) opts &= ~SO_DESCENDING;
    if (a->swf_version < 7 && !(opts & SO_RETURN_INDEXED)) c->touched = (unsigned char*)calloc((size_t)(len ? len : 1), 1);
    qsort_flash(c, e, len, opts);
    if (vm->err) { free(e); free(c->touched); c->touched = NULL; return v_undef(); }
    if (!is_sort_on && desc) for (i = 0; i < len / 2; i++) { SortEl t = e[i]; e[i] = e[len - 1 - i]; e[len - 1 - i] = t; }
    if (opts & SO_UNIQUE) {
        SortCtx plain; memset(&plain, 0, sizeof(plain)); plain.a = a;
        for (i = 0; i + 1 < len; i++) {
            int r = sort_cmp(is_sort_on ? c : &plain, e[i].v, e[i + 1].v, opts);
            if (vm->err) { free(e); free(c->touched); c->touched = NULL; return v_undef(); }
            if (r == 0) { free(e); free(c->touched); c->touched = NULL; return v_num(0); }
        }
    }
    if (opts & SO_RETURN_INDEXED) {
        ABuild b; Value *iv = (Value*)malloc((size_t)(len ? len : 1) * sizeof(Value)); AObj *r;
        if (!iv) { free(e); free(c->touched); c->touched = NULL; return v_undef(); }
        for (i = 0; i < len; i++) iv[i] = v_num(e[i].index);
        ab_new(a, &b);
        r = ab_with(a, &b, len, iv);
        free(iv); free(e);
        return r ? v_obj(r) : v_undef();
    }
    if (c->touched) {
        /* SWF5/6 sort in place: only the positions it swapped are written (re-added
         * after the existing properties); undefined only overwrites existing elements */
        int cur = o_length(a, self);
        if (desc && !is_sort_on) memset(c->touched, 1, (size_t)len);
        for (i = 0; i < len && !vm->err; i++) {
            if (!c->touched[i]) continue;
            if (e[i].v.t == V_UNDEF && !o_has_element(a, self, i)) continue;
            o_delete_element(a, self, i);
            o_set_element(a, self, i, e[i].v);
        }
        (void)cur;
        free(c->touched); c->touched = NULL;
    } else {
        /* positions whose value changed from the one read before sorting are re-added
           (delete + set); of the undefined values sorted to the end the first is always written */
        int first_undef = 1; Value *orig = (Value*)malloc((size_t)(len ? len : 1) * sizeof(Value));
        if (orig) { for (i = 0; i < len; i++) orig[i] = v_undef(); for (i = 0; i < len; i++) if (e[i].index >= 0 && e[i].index < len) orig[e[i].index] = e[i].v; }
        for (i = 0; i < len; i++) {
            if (e[i].v.t == V_UNDEF && first_undef) first_undef = 0;
            else if (orig && v_same(orig[i], e[i].v)) continue;
            o_delete_element(a, self, i);
            o_set_element(a, self, i, e[i].v); if (vm->err) break;
        }
        free(orig);
    }
    free(e);
    result = v_obj(self);
    return result;
}

static Value arr_sort(Act *a, AObj *self, int argc, Value *argv) {
    SortCtx c; int opts = 0;
    memset(&c, 0, sizeof(c)); c.a = a;
    if (argc >= 1 && argv[0].t == V_OBJ) {
        c.custom = argv[0].u.o;
        if (argc >= 2 && argv[1].t == V_NUM) opts = f64_i32(argv[1].u.n);
    } else if (argc >= 2 && argv[0].t == V_NUM && argv[1].t == V_NUM) opts = f64_i32(argv[1].u.n);
    else if (argc >= 1 && argv[0].t == V_NUM) opts = f64_i32(argv[0].u.n);
    else if (argc >= 1) return v_undef();
    opts &= 31;
    return sort_internal(a, self, &c, opts, 0);
}

static Value arr_sort_on(Act *a, AObj *self, int argc, Value *argv) {
    VM *vm = a->vm; SortCtx c; SortKey *f = NULL; int nf = 0, i; Value r;
    memset(&c, 0, sizeof(c)); c.a = a; c.sort_on = 1;
    if (argc < 1) return v_undef();
    if (argv[0].t == V_OBJ) {
        AObj *names = argv[0].u.o; int len;
        if (nat_of(names)->nk != NK_ARRAY) return sort_internal(a, self, &c, 0, 1);
        len = o_length(a, names); if (vm->err) return v_undef();
        if (len <= 0) return v_obj(self);
        f = (SortKey*)calloc((size_t)len, sizeof(SortKey));
        if (!f) return v_undef();
        for (i = 0; i < len; i++) { f[i].name = v_str_of(a, o_get_element(a, names, i)); if (vm->err) { free(f); return v_undef(); } }
        nf = len;
        if (argc > 1 && argv[1].t == V_OBJ && nat_of(argv[1].u.o)->nk == NK_ARRAY) {
            int ol = o_length(a, argv[1].u.o);
            if (vm->err) { free(f); return v_undef(); }
            if (ol == len) for (i = 0; i < len; i++) { f[i].opts = v_i32(a, o_get_element(a, argv[1].u.o, i)) & 31; if (vm->err) { free(f); return v_undef(); } }
        } else if (argc > 1 && v_is_prim(argv[1])) {
            int o = v_i32(a, argv[1]) & 31;
            if (vm->err) { free(f); return v_undef(); }
            for (i = 0; i < len; i++) f[i].opts = o;
        }
    } else {
        f = (SortKey*)calloc(1, sizeof(SortKey));
        if (!f) return v_undef();
        f[0].name = v_str_of(a, argv[0]); if (vm->err) { free(f); return v_undef(); }
        f[0].opts = (argc > 1 && argv[1].t == V_NUM) ? (f64_i32(argv[1].u.n) & 31) : 0;
        nf = 1;
    }
    c.fields = f; c.nfields = nf;
    r = sort_internal(a, self, &c, f[0].opts, 1);
    free(f);
    return r;
}

Value array_method(Act *a, AObj *self, int argc, Value *argv, int index) {
    switch (index) {
    case AM_ARRAY: return arr_function(a, self, argc, argv);
    case AM_PUSH: return arr_push(a, self, argc, argv);
    case AM_POP: return arr_pop(a, self, argc, argv);
    case AM_CONCAT: return arr_concat(a, self, argc, argv);
    case AM_SHIFT: return arr_shift(a, self, argc, argv);
    case AM_UNSHIFT: return arr_unshift(a, self, argc, argv);
    case AM_SLICE: return arr_slice(a, self, argc, argv);
    case AM_JOIN: return arr_join(a, self, argc, argv);
    case AM_SPLICE: return arr_splice(a, self, argc, argv);
    case AM_TO_STRING: return arr_join(a, self, 0, NULL);
    case AM_SORT: return arr_sort(a, self, argc, argv);
    case AM_REVERSE: return arr_reverse(a, self, argc, argv);
    case AM_SORT_ON: return arr_sort_on(a, self, argc, argv);
    default: return v_undef();
    }
}

static Value arr_m_push(Act *a, AObj *s, int c, Value *v) { return arr_push(a, s, c, v); }
static Value arr_m_pop(Act *a, AObj *s, int c, Value *v) { return arr_pop(a, s, c, v); }
static Value arr_m_concat(Act *a, AObj *s, int c, Value *v) { return arr_concat(a, s, c, v); }
static Value arr_m_shift(Act *a, AObj *s, int c, Value *v) { return arr_shift(a, s, c, v); }
static Value arr_m_unshift(Act *a, AObj *s, int c, Value *v) { return arr_unshift(a, s, c, v); }
static Value arr_m_slice(Act *a, AObj *s, int c, Value *v) { return arr_slice(a, s, c, v); }
static Value arr_m_join(Act *a, AObj *s, int c, Value *v) { return arr_join(a, s, c, v); }
static Value arr_m_splice(Act *a, AObj *s, int c, Value *v) { return arr_splice(a, s, c, v); }
static Value arr_m_to_string(Act *a, AObj *s, int c, Value *v) { (void)c; (void)v; return arr_join(a, s, 0, NULL); }
static Value arr_m_sort(Act *a, AObj *s, int c, Value *v) { return arr_sort(a, s, c, v); }
static Value arr_m_reverse(Act *a, AObj *s, int c, Value *v) { return arr_reverse(a, s, c, v); }
static Value arr_m_sort_on(Act *a, AObj *s, int c, Value *v) { return arr_sort_on(a, s, c, v); }

AObj *lib_array(VM *vm, Env *env) {
    AObj *proto, *c = lib_class(vm, env, arr_constructor, arr_function, env->p.object, 1, &proto);
    u16 at = DE | DD;
    env->p.array = proto; env->p.array_ctor = c;
    defm(vm, env, proto, "push", arr_m_push, at);
    defm(vm, env, proto, "pop", arr_m_pop, at);
    defm(vm, env, proto, "concat", arr_m_concat, at);
    defm(vm, env, proto, "shift", arr_m_shift, at);
    defm(vm, env, proto, "unshift", arr_m_unshift, at);
    defm(vm, env, proto, "slice", arr_m_slice, at);
    defm(vm, env, proto, "join", arr_m_join, at);
    defm(vm, env, proto, "splice", arr_m_splice, at);
    defm(vm, env, proto, "toString", arr_m_to_string, at);
    defm(vm, env, proto, "sort", arr_m_sort, at);
    defm(vm, env, proto, "reverse", arr_m_reverse, at);
    defm(vm, env, proto, "sortOn", arr_m_sort_on, at);
    o_define_c(vm, c, "CASEINSENSITIVE", v_num(SO_CASE_INSENSITIVE), 0);
    o_define_c(vm, c, "DESCENDING", v_num(SO_DESCENDING), 0);
    o_define_c(vm, c, "UNIQUESORT", v_num(SO_UNIQUE), 0);
    o_define_c(vm, c, "RETURNINDEXEDARRAY", v_num(SO_RETURN_INDEXED), 0);
    o_define_c(vm, c, "NUMERIC", v_num(SO_NUMERIC), 0);
    return c;
}

/* ---------------------------------------------------------------- */
/* String                                                           */

static AStr *this_string(Act *a, AObj *self) { return v_str_of(a, v_obj(self)); }

static Value str_constructor(Act *a, AObj *self, int argc, Value *argv) {
    AStr *s;
    if (argc < 1) s = a->vm->s_empty;
    else if (argv[0].t == V_STR) s = argv[0].u.s;
    else { s = v_str_of(a, argv[0]); if (a->vm->err) return v_undef(); }
    { AObj *t = o_native_relay(a, self, NK_STRING); if (t) { t->nk = NK_STRING; t->nat.s = s; } }
    o_define(a->vm, self, a->vm->s_length, v_num(s->len), DE | DD);
    return v_obj(self);
}
static Value str_function(Act *a, AObj *self, int argc, Value *argv) {
    (void)self;
    if (argc < 1) return v_str(a->vm->s_empty);
    if (argv[0].t == V_STR) return argv[0];
    return v_str(v_str_of(a, argv[0]));
}
static Value str_value_of(Act *a, AObj *self, int argc, Value *argv) {
    AObj *n = nat_of(self);
    (void)a; (void)argc; (void)argv;
    if (n && n->nk == NK_STRING) return v_str(n->nat.s);
    /* not a String: Flash converts `this` the generic way (without its toString) */
    if (!self) return v_undef();
    if (o_display(self)) return v_str(v_str_of(a, v_obj(self)));
    if (n && n->nk == NK_BOOL) return v_str(str_intern(a->vm, n->nat.b ? "true" : "false"));
    if (n && n->nk == NK_NUMBER) return v_str(v_str_of(a, v_num(n->nat.n)));
    return v_str(str_intern(a->vm, self->nk == NK_FUNCTION ? "[type Function]" : "[object Object]"));
}
static Value str_to_upper(Act *a, AObj *self, int argc, Value *argv) {
    AStr *s = this_string(a, self); (void)argc; (void)argv;
    if (a->vm->err) return v_undef();
    return v_str(str_upper(a->vm, s));
}
static Value str_to_lower(Act *a, AObj *self, int argc, Value *argv) {
    AStr *s = this_string(a, self); (void)argc; (void)argv;
    if (a->vm->err) return v_undef();
    return v_str(str_lower(a->vm, s));
}
static Value str_char_at(Act *a, AObj *self, int argc, Value *argv) {
    AStr *s = this_string(a, self); s32 i;
    if (a->vm->err) return v_undef();
    i = v_i32(a, arg(argc, argv, 0)); if (a->vm->err) return v_undef();
    if (i < 0 || (u32)i >= s->len) return v_str(a->vm->s_empty);
    return v_str(str_slice(a->vm, s, (u32)i, (u32)i + 1));
}
static Value str_char_code_at(Act *a, AObj *self, int argc, Value *argv) {
    AStr *s = this_string(a, self); s32 i;
    if (a->vm->err) return v_undef();
    i = v_i32(a, arg(argc, argv, 0)); if (a->vm->err) return v_undef();
    if (i >= 0) {
        if ((u32)i < s->len) return v_num(STR_AT(s, i));
        return a->swf_version == 5 ? v_num(0) : v_num(dnan());
    }
    return v_num(dnan());
}
static Value str_concat_m(Act *a, AObj *self, int argc, Value *argv) {
    SBuf b; int i; AStr *s = this_string(a, self);
    if (a->vm->err) return v_undef();
    sb_init(&b); sb_str(&b, s);
    for (i = 0; i < argc; i++) { AStr *x = v_str_of(a, argv[i]); if (a->vm->err) { sb_free(&b); return v_undef(); } sb_str(&b, x); }
    return v_str(sb_done(a->vm, &b));
}
static Value str_from_char_code(Act *a, AObj *self, int argc, Value *argv) {
    SBuf b; int i;
    (void)self;
    sb_init(&b);
    for (i = 0; i < argc; i++) { u16 c = v_u16(a, argv[i]); if (a->vm->err) { sb_free(&b); return v_undef(); } if (c == 0) break; sb_unit(&b, c); }
    return v_str(sb_done(a->vm, &b));
}
static Value str_index_of(Act *a, AObj *self, int argc, Value *argv) {
    AStr *s = this_string(a, self), *pat; u32 start = 0; long r;
    if (a->vm->err) return v_undef();
    if (argc < 1) return v_undef();
    pat = v_str_of(a, argv[0]); if (a->vm->err) return v_undef();
    if (argc > 1 && argv[1].t != V_UNDEF) { s32 n = v_i32(a, argv[1]); if (a->vm->err) return v_undef(); start = n < 0 ? 0 : (u32)n; }
    if (start > s->len) return v_num(-1);
    r = str_find(s, pat, start);
    return v_num(r < 0 ? -1 : (double)r);
}
static Value str_last_index_of(Act *a, AObj *self, int argc, Value *argv) {
    AStr *s = this_string(a, self), *pat, *hay; u32 end; long r;
    if (a->vm->err) return v_undef();
    if (argc < 1) return v_undef();
    pat = v_str_of(a, argv[0]); if (a->vm->err) return v_undef();
    if (argc > 1 && argv[1].t != V_UNDEF) {
        s32 n = v_i32(a, argv[1]); if (a->vm->err) return v_undef();
        if (n < 0) return v_num(-1);
        end = (u32)n + pat->len;
    } else end = s->len;
    hay = end <= s->len ? str_slice(a->vm, s, 0, end) : s;
    r = str_rfind(hay, pat, (long)hay->len);
    return v_num(r < 0 ? -1 : (double)r);
}
static u32 wrap_index(s32 i, u32 len) {
    if (i < 0) { u32 off = (u32)(-(double)i > 4294967295.0 ? 4294967295u : (u32)(-(double)i)); return off >= len ? 0 : len - off; }
    return (u32)i < len ? (u32)i : len;
}
static u32 clamp_index(s32 i, u32 len) { if (i < 0) return 0; return (u32)i < len ? (u32)i : len; }
static Value str_slice_m(Act *a, AObj *self, int argc, Value *argv) {
    AStr *s; u32 st, en;
    if (argc < 1) return v_undef();
    s = this_string(a, self); if (a->vm->err) return v_undef();
    st = wrap_index(v_i32(a, argv[0]), s->len); if (a->vm->err) return v_undef();
    if (argc > 1 && argv[1].t != V_UNDEF) { en = wrap_index(v_i32(a, argv[1]), s->len); if (a->vm->err) return v_undef(); }
    else en = s->len;
    if (st < en) return v_str(str_slice(a->vm, s, st, en));
    return v_str(a->vm->s_empty);
}
static Value str_substr(Act *a, AObj *self, int argc, Value *argv) {
    AStr *s; u32 st, en; s32 len;
    if (argc < 1) return v_undef();
    s = this_string(a, self); if (a->vm->err) return v_undef();
    st = wrap_index(v_i32(a, argv[0]), s->len); if (a->vm->err) return v_undef();
    if (argc > 1 && argv[1].t != V_UNDEF) { len = v_i32(a, argv[1]); if (a->vm->err) return v_undef(); }
    else len = (s32)s->len;
    en = wrap_index((s32)((u32)st + (u32)len), s->len);
    if (st < en) return v_str(str_slice(a->vm, s, st, en));
    return v_str(a->vm->s_empty);
}
static Value str_substring(Act *a, AObj *self, int argc, Value *argv) {
    AStr *s; u32 st, en, t;
    if (argc < 1) return v_undef();
    s = this_string(a, self); if (a->vm->err) return v_undef();
    st = clamp_index(v_i32(a, argv[0]), s->len); if (a->vm->err) return v_undef();
    if (argc > 1 && argv[1].t != V_UNDEF) { en = clamp_index(v_i32(a, argv[1]), s->len); if (a->vm->err) return v_undef(); }
    else en = s->len;
    if (en < st) { t = en; en = st; st = t; }
    return v_str(str_slice(a->vm, s, st, en));
}
static Value str_split(Act *a, AObj *self, int argc, Value *argv) {
    VM *vm = a->vm; AStr *s = this_string(a, self), *delim = NULL; u32 limit = 0xFFFFFFFFu; int swf5 = a->swf_version == 5; VList l = { NULL, 0, 0 }; ABuild b; AObj *r;
    if (vm->err) return v_undef();
    if (argc > 1 && argv[1].t != V_UNDEF) { s32 n = v_i32(a, argv[1]); if (vm->err) return v_undef(); limit = n < 0 ? 0 : (u32)n; }
    if (argc < 1 || argv[0].t == V_UNDEF) { if (swf5) delim = str_intern(vm, ","); }
    else {
        delim = v_str_of(a, argv[0]); if (vm->err) return v_undef();
        if (swf5 && delim->len == 0) delim = NULL;
    }
    ab_new(a, &b);
    /* SWF5: a limit below 1 gives an empty array (an empty delimiter aside), and a
     * delimiter longer than one character does not split */
    if (swf5 && delim && argc > 1 && argv[1].t != V_UNDEF && limit < 1) { r = ab_with(a, &b, 0, NULL); return r ? v_obj(r) : v_undef(); }
    if (swf5 && delim && delim->len > 1) delim = NULL;
    /* SWF6+: an empty string gives [""] for a delimiter, [] for "" (limit aside) */
    if (!swf5 && delim && s->len == 0) { Value e = v_str(s); r = ab_with(a, &b, delim->len ? 1 : 0, &e); return r ? v_obj(r) : v_undef(); }
    if (delim) {
        if (delim->len == 0) {
            u32 i;
            for (i = 0; i < s->len && i < limit; i++) vl_push(&l, v_str(str_slice(vm, s, i, i + 1)));
        } else {
            u32 start = 0, cnt = 0;
            for (;;) {
                long f;
                if (cnt >= limit) break;
                f = str_find(s, delim, start);
                if (f < 0) { vl_push(&l, v_str(str_slice(vm, s, start, s->len))); break; }
                vl_push(&l, v_str(str_slice(vm, s, start, (u32)f)));
                cnt++;
                start = (u32)f + delim->len;
            }
        }
    } else vl_push(&l, v_str(s));
    r = ab_with(a, &b, l.n, l.v);
    free(l.v);
    return r ? v_obj(r) : v_undef();
}

AObj *lib_string(VM *vm, Env *env) {
    AObj *proto, *c = lib_class(vm, env, str_constructor, str_function, env->p.object, 1, &proto);
    u16 at = DE | DD;
    env->p.string = proto;
    defm(vm, env, proto, "valueOf", str_value_of, at);
    defm(vm, env, proto, "toString", str_value_of, at);
    defm(vm, env, proto, "toUpperCase", str_to_upper, at);
    defm(vm, env, proto, "toLowerCase", str_to_lower, at);
    defm(vm, env, proto, "charAt", str_char_at, at);
    defm(vm, env, proto, "charCodeAt", str_char_code_at, at);
    defm(vm, env, proto, "concat", str_concat_m, at);
    defm(vm, env, proto, "indexOf", str_index_of, at);
    defm(vm, env, proto, "lastIndexOf", str_last_index_of, at);
    defm(vm, env, proto, "slice", str_slice_m, at);
    defm(vm, env, proto, "substring", str_substring, at);
    defm(vm, env, proto, "split", str_split, at);
    defm(vm, env, proto, "substr", str_substr, at);
    defm(vm, env, c, "fromCharCode", str_from_char_code, at);
    return c;
}

/* ---------------------------------------------------------------- */
/* Number                                                           */

static Value num_constructor(Act *a, AObj *self, int argc, Value *argv) {
    double d = 0;
    if (argc > 0) { d = v_num_of(a, argv[0]); if (a->vm->err) return v_undef(); }
    { AObj *t = o_native_relay(a, self, NK_NUMBER); if (t) { t->nk = NK_NUMBER; t->nat.n = d; } }
    return v_obj(self);
}
static Value num_function(Act *a, AObj *self, int argc, Value *argv) {
    double d = 0;
    (void)self;
    if (argc > 0) { d = v_num_of(a, argv[0]); if (a->vm->err) return v_undef(); }
    return v_num(d);
}
static Value num_value_of(Act *a, AObj *self, int argc, Value *argv) {
    AObj *n = nat_of(self); (void)a; (void)argc; (void)argv;
    if (n && n->nk == NK_NUMBER) return v_num(n->nat.n);
    return v_undef();
}
static Value num_to_string(Act *a, AObj *self, int argc, Value *argv) {
    AObj *n = nat_of(self); double num; int radix = 10;
    if (!n || n->nk != NK_NUMBER) return v_undef();
    num = n->nat.n;
    if (argc > 0) {
        double r = v_num_of(a, argv[0]); s32 ri;
        if (a->vm->err) return v_undef();
        ri = r != r ? 0 : r >= 2147483647.0 ? 2147483647 : r <= -2147483648.0 ? (s32)0x80000000u : (s32)r;
        radix = (ri >= 2 && ri <= 36) ? ri : 10;
    }
    if (radix == 10) return v_str(v_str_of(a, v_num(num)));
    {
        s32 x = clamp_to_i32(num); u32 m; char digits[34]; int i = 33, neg = 0;
        if (x == 0) return v_str(str_intern(a->vm, "0"));
        if (x < 0) { neg = 1; m = 0u - (u32)x; } else m = (u32)x;
        digits[33] = 0;
        if (m == 0x80000000u) {
            /* -INT_MIN overflows: Flash keeps dividing the negative value, so the
             * (negative) remainders come out as characters below '0' */
            while (m) { u32 d = m % (u32)radix; m /= (u32)radix; digits[--i] = (char)('0' - (int)d); }
        } else
        while (m) { u32 d = m % (u32)radix; m /= (u32)radix; digits[--i] = (char)(d < 10 ? '0' + d : 'a' + d - 10); }
        if (neg) digits[--i] = '-';
        return v_str(str_cstr(a->vm, digits + i));
    }
}

AObj *lib_number(VM *vm, Env *env) {
    AObj *proto, *c = lib_class(vm, env, num_constructor, num_function, env->p.object, 1, &proto);
    u16 at = DE | DD | RO;
    env->p.number = proto;
    defm(vm, env, proto, "valueOf", num_value_of, DE | DD);
    defm(vm, env, proto, "toString", num_to_string, DE | DD);
    o_define_c(vm, c, "MAX_VALUE", v_num(1.7976931348623157e308), at);
    o_define_c(vm, c, "MIN_VALUE", v_num(4.9406564584124654e-324), at);
    o_define_c(vm, c, "NaN", v_num(dnan()), at);
    o_define_c(vm, c, "NEGATIVE_INFINITY", v_num(-dinf()), at);
    o_define_c(vm, c, "POSITIVE_INFINITY", v_num(dinf()), at);
    return c;
}

/* ---------------------------------------------------------------- */
/* Boolean                                                          */

static Value bool_constructor(Act *a, AObj *self, int argc, Value *argv) {
    { AObj *t = o_native_relay(a, self, NK_BOOL); if (t) { t->nk = NK_BOOL; t->nat.b = argc > 0 && v_as_bool(argv[0], a->swf_version); } }
    return v_obj(self);
}
static Value bool_function(Act *a, AObj *self, int argc, Value *argv) {
    (void)self;
    if (argc < 1) return v_undef();
    return v_bool(v_as_bool(argv[0], a->swf_version));
}
static Value bool_to_string(Act *a, AObj *self, int argc, Value *argv) {
    AObj *n = nat_of(self); (void)argc; (void)argv;
    if (n && n->nk == NK_BOOL) return v_str(n->nat.b ? a->vm->s_true : a->vm->s_false);
    return v_undef();
}
static Value bool_value_of(Act *a, AObj *self, int argc, Value *argv) {
    AObj *n = nat_of(self); (void)a; (void)argc; (void)argv;
    if (n && n->nk == NK_BOOL) return v_bool(n->nat.b);
    return v_undef();
}

AObj *lib_boolean(VM *vm, Env *env) {
    AObj *proto, *c = lib_class(vm, env, bool_constructor, bool_function, env->p.object, 1, &proto);
    env->p.boolean = proto;
    defm(vm, env, proto, "valueOf", bool_value_of, DE | DD);
    defm(vm, env, proto, "toString", bool_to_string, DE | DD);
    return c;
}

/* ---------------------------------------------------------------- */
/* ASnative tables 251 (String), 102 (SWF5 case), 106 (Number), 107 (Boolean) */

/* ASnative(251, 0) / (106, 2) / (107, 2): the class functions, constructing under new */
Value string_native(Act *a, AObj *self, int argc, Value *argv, int index) {
    static const NativeFn t[] = { NULL, str_value_of, str_value_of, str_to_upper, str_to_lower, str_char_at,
        str_char_code_at, str_concat_m, str_index_of, str_last_index_of, str_slice_m, str_substring, str_split,
        str_substr, str_from_char_code };
    if (index == 0) return a->vm->table_ctor ? str_constructor(a, self, argc, argv) : str_function(a, self, argc, argv);
    if (index < 0 || index >= (int)(sizeof(t) / sizeof(t[0]))) return v_undef();
    return t[index](a, self, argc, argv);
}
/* ASnative(102, 0/1): the SWF5 case conversions (ASCII letters only) */
Value string_native_old(Act *a, AObj *self, int argc, Value *argv, int index) {
    AStr *s; u16 *u; u32 i; AStr *r; (void)argc; (void)argv;
    if (index != 0 && index != 1) return v_undef();
    s = this_string(a, self); if (a->vm->err) return v_undef();
    u = (u16*)malloc((size_t)(s->len ? s->len : 1) * sizeof(u16));
    if (!u) return v_undef();
    for (i = 0; i < s->len; i++) {
        u16 c = STR_AT(s, i);
        if (index == 0 && c >= 'a' && c <= 'z') c = (u16)(c - 32);
        else if (index == 1 && c >= 'A' && c <= 'Z') c = (u16)(c + 32);
        u[i] = c;
    }
    r = str_from_units(a->vm, u, s->len); free(u);
    return v_str(r);
}
Value number_native(Act *a, AObj *self, int argc, Value *argv, int index) {
    if (index == 0) return num_value_of(a, self, argc, argv);
    if (index == 1) return num_to_string(a, self, argc, argv);
    if (index == 2) return a->vm->table_ctor ? num_constructor(a, self, argc, argv) : num_function(a, self, argc, argv);
    return v_undef();
}
Value boolean_native(Act *a, AObj *self, int argc, Value *argv, int index) {
    if (index == 0) return bool_value_of(a, self, argc, argv);
    if (index == 1) return bool_to_string(a, self, argc, argv);
    if (index == 2) return a->vm->table_ctor ? bool_constructor(a, self, argc, argv) : bool_function(a, self, argc, argv);
    return v_undef();
}

/* ---------------------------------------------------------------- */
/* Math                                                             */

enum { MM_ABS = 0, MM_MIN, MM_MAX, MM_SIN, MM_COS, MM_ATAN2, MM_TAN, MM_EXP, MM_LOG, MM_SQRT, MM_ROUND,
       MM_RANDOM, MM_FLOOR, MM_CEIL, MM_ATAN, MM_ASIN, MM_ACOS, MM_POW, MM_ISNAN, MM_ISFINITE };

extern double avm1_math_random(VM *vm);

static double rust_pow(double x, double y) {
    /* Rust powf follows C pow, except pow(-1, +-inf) and pow(1, NaN) are 1 in both */
    return pow(x, y);
}

Value math_method(Act *a, AObj *self, int argc, Value *argv, int index) {
    double x, y, r;
    (void)self;
    x = v_num_of(a, arg(argc, argv, 0)); if (a->vm->err) return v_undef();
    y = v_num_of(a, arg(argc, argv, 1)); if (a->vm->err) return v_undef();
    /* ASnative(200, 18/19): isNaN / isFinite */
    if (index == MM_ISNAN) return v_bool(x != x);
    if (index == MM_ISFINITE) return v_bool(x == x && x != dinf() && x != -dinf());
    if (a->swf_version <= 6) {
        int valid;
        if (index == MM_RANDOM) valid = 1;
        else if ((index == MM_MIN || index == MM_MAX) && argc == 0) valid = 1;
        else if (index == MM_POW && argc == 1 && x == 1.0) valid = 1;
        else if ((index == MM_MIN || index == MM_MAX || index == MM_POW || index == MM_ATAN2) && argc <= 1) valid = 0;
        else if (argc == 0) valid = 0;
        else valid = 1;
        if (!valid) return v_num(dnan());
    }
    switch (index) {
    case MM_ABS: r = fabs(x); break;
    case MM_MIN: if (argc == 0) r = dinf(); else if (x != x || y != y) r = dnan(); else r = x < y ? x : (y < x ? y : (x == 0 && y == 0 ? ((1 / x < 0 || 1 / y < 0) ? -0.0 : 0.0) : x)); break;
    case MM_MAX: if (argc == 0) r = -dinf(); else if (x != x || y != y) r = dnan(); else r = x > y ? x : (y > x ? y : (x == 0 && y == 0 ? ((1 / x > 0 || 1 / y > 0) ? 0.0 : -0.0) : x)); break;
    case MM_SIN: r = sin(x); break;
    case MM_COS: r = cos(x); break;
    case MM_ATAN2: r = atan2(x, y); break;
    case MM_TAN: r = tan(x); break;
    case MM_EXP: r = exp(x); break;
    case MM_LOG: r = log(x); break;
    case MM_SQRT: r = sqrt(x); break;
    case MM_ROUND: r = floor(x + 0.5); break;
    case MM_RANDOM: r = avm1_math_random(a->vm); break;
    case MM_FLOOR: r = floor(x); break;
    case MM_CEIL: r = ceil(x); break;
    case MM_ATAN: r = atan(x); break;
    /* the x87 player derives these from fpatan */
    case MM_ASIN: r = (x >= -1.0 && x <= 1.0) ? atan2(x, sqrt(1.0 - x * x)) : asin(x); break;
    case MM_ACOS: r = (x >= -1.0 && x <= 1.0) ? atan2(sqrt(1.0 - x * x), x) : acos(x); break;
    case MM_POW: r = rust_pow(x, y); break;
    default: r = dnan(); break;
    }
    return v_num(r);
}

AObj *lib_math(VM *vm, Env *env) {
    static const char *names[18] = { "abs", "min", "max", "sin", "cos", "atan2", "tan", "exp", "log", "sqrt",
                                     "round", "random", "floor", "ceil", "atan", "asin", "acos", "pow" };
    AObj *m = obj_new(vm, env->p.object); u16 at = DE | DD | RO; int i;
    o_define_c(vm, m, "SQRT2", v_num(1.41421356237309504880), at);
    o_define_c(vm, m, "SQRT1_2", v_num(0.70710678118654752440), at);
    o_define_c(vm, m, "PI", v_num(3.14159265358979323846), at);
    o_define_c(vm, m, "LOG2E", v_num(1.4426950408889634074), at);
    o_define_c(vm, m, "LOG10E", v_num(0.43429448190325182765), at);
    o_define_c(vm, m, "LN2", v_num(0.69314718055994530942), at);
    o_define_c(vm, m, "LN10", v_num(2.30258509299404568402), at);
    o_define_c(vm, m, "E", v_num(2.7182818284590452354), at);
    for (i = 0; i < 18; i++) deft(vm, env, m, names[i], math_method, i, at);
    return m;
}
