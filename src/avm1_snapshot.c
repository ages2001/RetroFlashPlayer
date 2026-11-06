/* avm1_snapshot.c - TextSnapshot: the static text of a movie clip
 * (Ruffle's display_object/text.rs TextSnapshot). */
#include "avm1_int.h"
#include <stdlib.h>
#include <string.h>

#define DE  PA_DONT_ENUM
#define DD  PA_DONT_DELETE
#define V6  PA_VERSION_6

extern AObj *lib_class(VM *vm, Env *env, NativeFn ctor, NativeFn call, AObj *super_proto, int proto_first, AObj **proto_out);
extern int text_static_chars(SWFChar *ch, SWFMovie *m, u16 *out, int cap);

typedef struct { u16 *u; u32 *start, *len; int n; u32 count; } Snap;

static void snap_free(AObj *o) { Snap *s = (Snap*)o->nat.p; if (s) { free(s->u); free(s->start); free(s->len); free(s); } o->nat.p = NULL; }
static Snap *snap_of(AObj *o) { if (o && o->nk == NK_SUPER) o = o->nat.sup.self; return o && o->nk == NK_TEXTSNAPSHOT ? (Snap*)o->nat.p : NULL; }

static Snap *snap_build(Player *p, MovieClip *c) {
    Snap *s = (Snap*)calloc(1, sizeof(Snap)); DisplayObject *d; int cap = 0;
    if (!s) return NULL;
    for (d = c->children; d; d = d->next) {
        int n; u16 *u; u32 *st, *ln;
        if (d->clip || !d->ch || d->ch->type != CHAR_TEXT) continue;
        n = text_static_chars(d->ch, c->movie ? c->movie : p->movie, NULL, 0);
        if (n <= 0) continue;
        u = (u16*)realloc(s->u, (s->count + (u32)n + 1) * sizeof(u16));
        if (!u) break; s->u = u;
        text_static_chars(d->ch, c->movie ? c->movie : p->movie, s->u + s->count, n);
        if (s->n == cap) {
            cap = cap ? cap * 2 : 8;
            st = (u32*)realloc(s->start, (size_t)cap * sizeof(u32)); if (!st) break; s->start = st;
            ln = (u32*)realloc(s->len, (size_t)cap * sizeof(u32)); if (!ln) break; s->len = ln;
        }
        s->start[s->n] = s->count; s->len[s->n] = (u32)n; s->n++;
        s->count += (u32)n;
    }
    return s;
}

static Value ts_ctor(Act *a, AObj *self, int argc, Value *argv) {
    AObj *o, *t; MovieClip *c;
    if (argc != 1 || argv[0].t == V_UNDEF || argv[0].t == V_NULL) return v_undef();
    o = v_to_obj(a, argv[0]); if (a->vm->err || !o) return v_undef();
    c = o_display(o);
    if (!c || !clip_is_movieclip(c)) return v_undef();
    t = o_native_target(self);
    if (t) { Snap *s = snap_build(a->vm->player, c); if (s) { t->nk = NK_TEXTSNAPSHOT; t->nat.p = s; t->nfree = snap_free; } }
    return v_undef();
}

static Value ts_count(Act *a, AObj *self, int argc, Value *argv) { Snap *s = snap_of(self); (void)a; (void)argv; if (!s || argc) return v_undef(); return v_num(s->count); }

static Value ts_get_text(Act *a, AObj *self, int argc, Value *argv) {
    Snap *s = snap_of(self); s32 from, to; int nl, i, first = 1; u32 start, end; SBuf b;
    if (!s || argc < 2 || argc > 3) return v_undef();
    from = v_i32(a, argv[0]); if (a->vm->err) return v_undef();
    to = v_i32(a, argv[1]); if (a->vm->err) return v_undef();
    nl = v_bool_of(a, arg(argc, argv, 2));
    s = snap_of(self); if (!s) return v_undef();
    if (!s->count) return v_str(a->vm->s_empty);
    start = from < 0 ? 0 : (u32)from; if (start > s->count - 1) start = s->count - 1;
    end = to < 0 ? 0 : (u32)to; if (end > s->count) end = s->count; if (end < start + 1) end = start + 1;
    sb_init(&b);
    for (i = 0; i < s->n; i++) {
        u32 cs = s->start[i], ce = cs + s->len[i], x0, x1, k;
        if (cs >= end || ce <= start) continue;
        if (!first && nl) sb_unit(&b, '\n');
        first = 0;
        x0 = start > cs ? start - cs : 0; x1 = end - cs; if (x1 > s->len[i]) x1 = s->len[i];
        for (k = x0; k < x1; k++) sb_unit(&b, s->u[cs + k]);
    }
    return v_str(sb_done(a->vm, &b));
}

static u16 lc(u16 c) { return c >= 'A' && c <= 'Z' ? (u16)(c + 32) : c; }

static Value ts_find_text(Act *a, AObj *self, int argc, Value *argv) {
    Snap *s = snap_of(self); s32 from; AStr *t; int cs; u32 i, j, tl;
    if (!s || argc != 3) return v_undef();
    from = v_i32(a, argv[0]); if (a->vm->err) return v_undef();
    t = v_str_of(a, argv[1]); if (a->vm->err) return v_undef();
    cs = v_bool_of(a, argv[2]);
    s = snap_of(self); if (!s) return v_undef();
    tl = t->len;
    if (!tl || from < 0) return v_num(-1);
    for (i = (u32)from; i + tl <= s->count; i++) {
        for (j = 0; j < tl; j++) { u16 x = s->u[i + j], y = STR_AT(t, j); if (cs ? x != y : lc(x) != lc(y)) break; }
        if (j == tl) return v_num(i);
    }
    return v_num(-1);
}

static Value ts_noop(Act *a, AObj *self, int argc, Value *argv) { (void)a; (void)self; (void)argc; (void)argv; return v_undef(); }

static void defm(VM *vm, Env *env, AObj *o, const char *name, NativeFn f, u16 attr) { o_define(vm, o, str_intern(vm, name), v_obj(fn_native(vm, env, f, NULL)), attr); }

AObj *lib_textsnapshot(VM *vm, Env *env, AObj *object_proto) {
    AObj *proto, *c = lib_class(vm, env, NULL, ts_ctor, object_proto, 1, &proto);
    env->p.textsnapshot = proto;
    defm(vm, env, proto, "getCount", ts_count, V6);
    defm(vm, env, proto, "setSelected", ts_noop, V6);
    defm(vm, env, proto, "getSelected", ts_noop, V6);
    defm(vm, env, proto, "getText", ts_get_text, V6);
    defm(vm, env, proto, "getSelectedText", ts_noop, V6);
    defm(vm, env, proto, "hitTestTextNearPos", ts_noop, V6);
    defm(vm, env, proto, "findText", ts_find_text, V6);
    defm(vm, env, proto, "setSelectColor", ts_noop, V6);
    defm(vm, env, proto, "getTextRunInfo", ts_noop, V6);
    return c;
}

/* MovieClip.getTextSnapshot: new TextSnapshot(this) through the global */
Value a1_get_text_snapshot(Act *a, AObj *self, int argc, Value *argv) {
    Value cls; AObj *co, *owner = NULL; Value arg0 = v_obj(self);
    (void)argc; (void)argv;
    cls = act_resolve(a, str_intern(a->vm, "TextSnapshot"), &owner);
    if (a->vm->err) return v_undef();
    co = v_to_obj_or_bare(a, cls); if (a->vm->err || !co) return v_undef();
    return o_construct(a, co, 1, &arg0);
}
