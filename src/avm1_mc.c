/* avm1_mc.c - the AVM1 runtime around the interpreter: VM lifetime, the
 * execution list and the action queue that make up Flash's AVM1 frame,
 * clip events, construction and removal of stage objects, timers, input
 * events and the hooks the player calls.
 *
 * Model (Flash Player, as documented by Ruffle's test suite):
 *  - every movie clip is put on a global execution list when it is
 *    instantiated, newest first; each frame walks the list and every clip
 *    dispatches load / enterFrame and then advances its timeline;
 *  - frame scripts and clip events are queued, not run: the queue is
 *    drained after the whole list ran (initialize events first, then
 *    constructors, then everything else, each in queue order);
 *  - a clip placed by a timeline runs its first frame immediately;
 *  - removing a clip that has an unload handler is delayed to the start of
 *    the next frame (it moves to a negative depth meanwhile). */
#include "avm1_int.h"
#include "audio.h"
#include "textfield.h"
#include "shape.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <math.h>

void avm1_free_all(VM *vm);
void avm1_timer_remove(VM *vm, int i);
extern void a1_tf_bind_pending(VM *vm);
extern void a1_tf_unbind(VM *vm, MovieClip *c);
extern void a1_tf_init(VM *vm, MovieClip *c);
extern void a1_tf_post_instantiation(VM *vm, MovieClip *c);
extern void a1_drawing_free(MovieClip *c);
extern void a1_button_post_instantiation(VM *vm, MovieClip *c);

static VM *vmof(Player *p) { return p ? (VM*)p->avm : NULL; }

/* ---------------------------------------------------------------- */
/* well-known strings                                               */

typedef struct SDef { size_t off; const char *s; } SDef;
#define SD(f, s) { offsetof(VM, f), s }
static const SDef sdefs[] = {
    SD(s_proto, "__proto__"), SD(s_prototype, "prototype"), SD(s_constructor, "constructor"),
    SD(s___constructor__, "__constructor__"), SD(s_length, "length"), SD(s_this, "this"),
    SD(s_arguments, "arguments"), SD(s_callee, "callee"), SD(s_caller, "caller"), SD(s_super, "super"),
    SD(s_root, "_root"), SD(s_parent, "_parent"), SD(s_global, "_global"), SD(s_toString, "toString"),
    SD(s_valueOf, "valueOf"), SD(s___resolve, "__resolve"), SD(s_undefined, "undefined"), SD(s_null, "null"),
    SD(s_empty, ""), SD(s_true, "true"), SD(s_false, "false"), SD(s_NaN, "NaN"), SD(s_Infinity, "Infinity"),
    SD(s_mInfinity, "-Infinity"), SD(s_object_Object, "[object Object]"), SD(s_type_Object, "[type Object]"),
    SD(s_type_Function, "[type Function]"), SD(s_onEnterFrame, "onEnterFrame"), SD(s_onLoad, "onLoad"),
    SD(s_onUnload, "onUnload"), SD(s_onData, "onData"), SD(s_onKeyDown, "onKeyDown"), SD(s_onKeyUp, "onKeyUp"),
    SD(s_onMouseDown, "onMouseDown"), SD(s_onMouseUp, "onMouseUp"), SD(s_onMouseMove, "onMouseMove"),
    SD(s_onMouseWheel, "onMouseWheel"), SD(s_onPress, "onPress"), SD(s_onRelease, "onRelease"),
    SD(s_onReleaseOutside, "onReleaseOutside"), SD(s_onRollOver, "onRollOver"), SD(s_onRollOut, "onRollOut"),
    SD(s_onDragOver, "onDragOver"), SD(s_onDragOut, "onDragOut"), SD(s_onSetFocus, "onSetFocus"),
    SD(s_onKillFocus, "onKillFocus"), SD(s_onChanged, "onChanged"), SD(s_onScroller, "onScroller"),
    SD(s_onResize, "onResize"), SD(s_onSoundComplete, "onSoundComplete"), SD(s_onConstruct, "onConstruct"),
    SD(s_onInitialize, "onInitialize"), SD(s_text, "text"), SD(s_htmlText, "htmlText"), SD(s_name, "name"),
    SD(s_x, "x"), SD(s_y, "y"), SD(s_level0, "_level0"), SD(s_broadcastMessage, "broadcastMessage"),
    SD(s__listeners, "_listeners"), SD(s_addListener, "addListener"), SD(s_removeListener, "removeListener"),
    SD(s_message, "message"), SD(s_Object, "Object"), SD(s_Array, "Array"), SD(s_Function, "Function"),
    SD(s_String, "String"), SD(s_Number, "Number"), SD(s_Boolean, "Boolean")
};
#define NSDEFS ((int)(sizeof(sdefs) / sizeof(sdefs[0])))

/* ---------------------------------------------------------------- */
/* creation                                                         */

void *avm1_create(Player *p) {
    VM *vm = (VM*)calloc(1, sizeof(VM)); int i;
    if (!vm) return NULL;
    vm->player = p;
    vm->max_recursion = 255;
    vm->max_exec_secs = getenv("FLASHNT_MAX_EXEC") ? atof(getenv("FLASHNT_MAX_EXEC")) : 15.0;
    vm->gc_threshold = 65536;
    vm->trace = 1;
    vm->rng = 0x2545F491u;
    vm->swf_version = p && p->movie ? p->movie->version : 6;
    vm->quality = 2; vm->stage_focus_rect = 1; vm->sound_buf_time = 5;
    vm->instance_counter = 1;
    for (i = 0; i < NSDEFS; i++) *(AStr**)((char*)vm + sdefs[i].off) = str_intern(vm, sdefs[i].s);
    for (i = 0; i < 256; i++) { char b[8]; sprintf(b, "%d", i); vm->small_int[i] = str_intern(vm, b); }
    return vm;
}

static void q_clear(QList *q) { int i; for (i = q->head; i < q->n; i++) { free(q->a[i].args); free((void*)q->a[i].ev_code); free(q->a[i].ev_len); } q->n = q->head = 0; }

void avm1_set_script_limits(Player *p, int max_recursion, int timeout_secs) {
    VM *vm = vmof(p);
    if (vm) { vm->max_recursion = max_recursion; if (timeout_secs > 0 && !getenv("FLASHNT_MAX_EXEC")) vm->max_exec_secs = timeout_secs; }
}
/* a new frame (or input event): scripts get a fresh time budget */
void avm1_reset_exec_clock(Player *p) { VM *vm = vmof(p); if (vm) vm->exec_start = 0; }

void avm1_free(Player *p) {
    VM *vm = vmof(p); int i;
    if (!vm) return;
    for (i = 0; i < 3; i++) { q_clear(&vm->queue[i]); free(vm->queue[i].a); }
    for (i = 0; i < MAX_TIMERS; i++) if (vm->timers[i].active) avm1_timer_remove(vm, i);
    { extern void avm1_loads_free(VM *vm); avm1_loads_free(vm); }
    avm1_free_all(vm);
    pm_free(&vm->env[0].registry); pm_free(&vm->env[1].registry); pm_free(&vm->so_map); { extern void avm1_net_free(VM *vm); avm1_net_free(vm); }
    { extern void avm1_sockets_free(VM *vm); avm1_sockets_free(vm); }
    { extern void avm1_fileref_free(void); avm1_fileref_free(); }
    { extern void avm1_netstream_free(void); avm1_netstream_free(); }
    { extern void avm1_ei_free(void); avm1_ei_free(); }
    { extern void avm1_sound_free_all(void); audio_stop_all(p); avm1_sound_free_all(); }
    free(vm->stack); free(vm->intern); free(vm->mix);
    free(vm);
    p->avm = NULL;
}

/* ---------------------------------------------------------------- */
/* trace output                                                     */

void avm1_trace_out(const char *s) {
    /* one "trace: " prefix per line so multi-line traces stay parseable;
       Flash turns every "\r" into a line break */
    fputs("trace: ", stderr);
    for (; *s; s++) {
        if (*s == '\r' || *s == '\n') fputs("\ntrace: ", stderr);
        else fputc(*s, stderr);
    }
    fputc('\n', stderr);
}
void avm2_trace_out(const char *s) { avm1_trace_out(s); }
void avm1_trace_str(VM *vm, AStr *s) { if (vm->trace) avm1_trace_out(str_c(vm, s)); }

/* ---------------------------------------------------------------- */
/* time and random numbers                                          */

double avm1_get_time(VM *vm) { return floor(vm->time_ms); }

static u32 rng_next(VM *vm) {
    u32 x = vm->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    vm->rng = x;
    return x & 0x7FFFFFFFu;
}
int avm1_random(VM *vm, int max) { if (max <= 0) return 0; return (int)(rng_next(vm) % (u32)max); }
double avm1_math_random(VM *vm) { return (double)rng_next(vm) / 2147483648.0; }

/* ---------------------------------------------------------------- */
/* execution list                                                   */

static void exec_add(VM *vm, MovieClip *c) {
    A1Clip *s = a1_of(c);
    if (!s || s->in_exec) return;
    s->in_exec = 1;
    s->next_exec = vm->exec_list;
    vm->exec_list = c;
}

static void exec_unlink(VM *vm, MovieClip *c) {
    MovieClip **pp = &vm->exec_list;
    while (*pp) {
        if (*pp == c) { *pp = c->a1 ? c->a1->next_exec : NULL; if (c->a1) { c->a1->next_exec = NULL; c->a1->in_exec = 0; } return; }
        pp = &(*pp)->a1->next_exec;
    }
}

/* ---------------------------------------------------------------- */
/* action queue                                                     */

static int q_prio(int kind) { return kind == QA_INIT ? 0 : kind == QA_CONSTRUCT ? 1 : 2; }

static QAction *q_push(VM *vm, MovieClip *c, int kind, int is_unload) {
    QList *q = &vm->queue[q_prio(kind)]; QAction *a;
    if (q->head > 0 && q->head == q->n) q->head = q->n = 0;
    if (q->n == q->cap) {
        int nc = q->cap ? q->cap * 2 : 64;
        QAction *na = (QAction*)realloc(q->a, (size_t)nc * sizeof(QAction));
        if (!na) return NULL;
        q->a = na; q->cap = nc;
    }
    a = &q->a[q->n++];
    memset(a, 0, sizeof(*a));
    a->clip = c; a->kind = (u8)kind; a->is_unload = (u8)is_unload;
    return a;
}

void avm1_queue_do_action(Player *p, MovieClip *c, const u8 *code, size_t len) {
    VM *vm = vmof(p); QAction *a;
    if (!vm || !len) return;
    a = q_push(vm, c, QA_NORMAL, 0);
    if (a) { a->code = code; a->len = (u32)len; }
}

static void queue_method(VM *vm, MovieClip *c, AObj *o, AStr *name, int is_unload) {
    QAction *a = q_push(vm, c, QA_METHOD, is_unload);
    if (a) { a->obj = o; a->name = name; }
}

void avm1_queue_method(VM *vm, MovieClip *c, AObj *o, AStr *name, int argc, Value *argv) {
    QAction *a = q_push(vm, c, QA_METHOD, 0);
    if (!a) return;
    a->obj = o; a->name = name;
    if (argc > 0) { a->args = (Value*)malloc((size_t)argc * sizeof(Value)); if (a->args) { memcpy(a->args, argv, (size_t)argc * sizeof(Value)); a->nargs = argc; } }
}

void avm1_queue_notify(VM *vm, MovieClip *c, const char *broadcaster, AStr *method, int argc, Value *argv) {
    QAction *a = q_push(vm, c ? c : avm1_base_root(vm), QA_NOTIFY, 0);
    if (!a) return;
    a->name = str_intern(vm, broadcaster); a->method = method;
    if (argc > 0) { a->args = (Value*)malloc((size_t)argc * sizeof(Value)); if (a->args) { memcpy(a->args, argv, (size_t)argc * sizeof(Value)); a->nargs = argc; } }
}

/* run bytecode in the timeline scope of `c` (Avm1::run_stack_frame_for_action) */
static void run_code(VM *vm, MovieClip *c, const u8 *code, u32 len, int init) {
    Act a; int ret = 0;
    if (vm->halted || !c) return;
    act_init_clip(&a, vm, c);
    if (init) vm_push(vm, v_undef());
    act_run_code(&a, c->movie, code, len, &ret);
    if (vm->err) vm_report_error(&a);
}

void avm1_run_init_action(Player *p, MovieClip *c, const u8 *code, size_t len) {
    VM *vm = vmof(p);
    if (vm) run_code(vm, c, code, (u32)len, 1);
}

static void notify_listeners(VM *vm, MovieClip *c, AStr *bname, AStr *method, int argc, Value *argv) {
    Act a; Value b; AObj *bo; int has = 0;
    act_init_nothing(&a, vm, c, "[System Listeners]");
    b = o_get(&a, act_global(&a), bname);
    if (vm->err) { vm_report_error(&a); return; }
    bo = v_to_obj_or_bare(&a, b);
    if (vm->err) { vm_report_error(&a); return; }
    broadcast_message(&a, bo, method, argc, argv, &has);
    if (vm->err) vm_report_error(&a);
    if (str_eq_c(bname, "Mouse")) vm->has_mouse_listener = has;
}

static void run_one(VM *vm, QAction *q) {
    MovieClip *c = q->clip;
    switch (q->kind) {
    case QA_NORMAL: case QA_INIT:
        run_code(vm, c, q->code, q->len, 0);
        break;
    case QA_CONSTRUCT:
        if (q->ctor) {
            Act a; Value proto;
            act_init_nothing(&a, vm, c, "[Construct]");
            proto = o_get(&a, q->ctor, vm->s_prototype);
            if (vm->err) { vm_report_error(&a); break; }
            if (c->obj) {
                int i;
                o_define(vm, (AObj*)c->obj, vm->s_proto, proto, PA_DONT_ENUM | PA_DONT_DELETE);
                for (i = 0; i < q->nev; i++) {
                    Act ch; int ret = 0;
                    act_init_clip(&ch, vm, c);
                    ch.id = a.id;
                    act_run_code(&ch, c->movie, q->ev_code[i], q->ev_len[i], &ret);
                    if (vm->err) { vm_report_error(&ch); }
                }
                o_construct_on_existing(&a, q->ctor, (AObj*)c->obj, 0, NULL);
                if (vm->err) vm_report_error(&a);
            }
        } else {
            int i;
            for (i = 0; i < q->nev; i++) run_code(vm, c, q->ev_code[i], q->ev_len[i], 0);
        }
        break;
    case QA_METHOD: {
        Act a;
        if (vm->halted) break;
        act_init_nothing(&a, vm, c, "[Method]");
        o_call_method(&a, q->obj, q->name, q->nargs, q->args, ER_SPECIAL);
        if (vm->err) vm_report_error(&a);
        break; }
    case QA_NOTIFY:
        notify_listeners(vm, c, q->name, q->method, q->nargs, q->args);
        break;
    }
}

void avm1_run_actions(VM *vm) {
    int guard = 0;
    for (;;) {
        QList *q = NULL; QAction a; int i;
        for (i = 0; i < 3; i++) if (vm->queue[i].head < vm->queue[i].n) { q = &vm->queue[i]; break; }
        if (!q) break;
        a = q->a[q->head++];
        if (q->head == q->n) q->head = q->n = 0;
        if (++guard > 1000000) break;
        if (!a.clip) { free(a.args); free((void*)a.ev_code); free(a.ev_len); continue; }
        /* actions of clips removed after they were queued do not run (unload ones do) */
        /* (is_unload 2: a load event, which still runs for a clip waiting for removal) */
        if (a.is_unload != 1 && a.clip && a.clip->a1 && (a.clip->a1->removed || (a.clip->a1->pending_removal && a.is_unload != 2))) { free(a.args); free((void*)a.ev_code); free(a.ev_len); continue; }
        if (a.is_unload != 1 && a.clip && a.clip->dead && !(a.clip->a1 && a.clip->a1->pending_removal)) { free(a.args); free((void*)a.ev_code); free(a.ev_len); continue; }
        run_one(vm, &a);
        free(a.args); free((void*)a.ev_code); free(a.ev_len);
    }
}

/* ---------------------------------------------------------------- */
/* clip events                                                      */

static AStr *event_method(VM *vm, u32 flag) {
    switch (flag) {
    case CE_DRAG_OUT: return vm->s_onDragOut;
    case CE_DRAG_OVER: return vm->s_onDragOver;
    case CE_ENTER_FRAME: return vm->s_onEnterFrame;
    case CE_KEY_DOWN: return vm->s_onKeyDown;
    case CE_KEY_UP: return vm->s_onKeyUp;
    case CE_LOAD: return vm->s_onLoad;
    case CE_MOUSE_DOWN: return vm->s_onMouseDown;
    case CE_MOUSE_MOVE: return vm->s_onMouseMove;
    case CE_MOUSE_UP: return vm->s_onMouseUp;
    case CE_PRESS: return vm->s_onPress;
    case CE_ROLL_OUT: return vm->s_onRollOut;
    case CE_ROLL_OVER: return vm->s_onRollOver;
    case CE_RELEASE: return vm->s_onRelease;
    case CE_RELEASE_OUTSIDE: return vm->s_onReleaseOutside;
    case CE_UNLOAD: return vm->s_onUnload;
    default: return NULL;
    }
}

static int is_key_event(u32 flag) { return flag == CE_KEY_DOWN || flag == CE_KEY_UP || flag == CE_KEY_PRESS; }

/* MovieClip event_dispatch: SWF clip events, then the AS handler method;
   returns 1 when a keyPress handler consumed the event */
int avm1_clip_event(VM *vm, MovieClip *c, u32 flag, int keycode) {
    A1Clip *s = c->a1; int handled = 0, i; AObj *o = (AObj*)c->obj;
    int version = clip_swf_version(c);
    if (version >= 5 && o) {
        if (s && s->events) {
            for (i = 0; i < s->nevents; i++) {
                ClipEvent *e = &s->events[i]; QAction *a;
                if (!(e->flags & flag)) continue;
                if (flag == CE_KEY_PRESS) { if (keycode != e->keycode) continue; handled = 1; }
                a = q_push(vm, c, QA_NORMAL, flag == CE_UNLOAD ? 1 : flag == CE_LOAD ? 2 : 0);
                if (a) { a->code = e->code; a->len = e->len; }
            }
        }
        if (version >= 6 && !(is_key_event(flag) && (vm->player->focus != c || !vm->highlight))) {
            AStr *m = event_method(vm, flag);
            if (m) queue_method(vm, c, o, m, flag == CE_UNLOAD ? 1 : flag == CE_LOAD ? 2 : 0);
        }
    }
    return handled;
}

/* PlaceObject CLIPACTIONS */
void avm1_clip_actions(Player *p, MovieClip *c, const u8 *data, size_t len) {
    A1Clip *s = a1_of(c); size_t pos = 0; int v = c->movie ? c->movie->version : 6, wide = v >= 6; u32 all; int n = 0, cap = 0; ClipEvent *list = NULL;
    (void)p;
    if (!s || len < 2) return;
    pos = 2;                                     /* reserved */
#define RD32(q) ((u32)(q)[0] | ((u32)(q)[1] << 8) | ((u32)(q)[2] << 16) | ((u32)(q)[3] << 24))
    if (wide) { if (pos + 4 > len) return; all = RD32(data + pos); pos += 4; }
    else { if (pos + 2 > len) return; all = (u32)(data[pos] | (data[pos + 1] << 8)); pos += 2; }
    for (;;) {
        u32 flags, size; ClipEvent e;
        if (wide) { if (pos + 4 > len) break; flags = RD32(data + pos); pos += 4; }
        else { if (pos + 2 > len) break; flags = (u32)(data[pos] | (data[pos + 1] << 8)); pos += 2; }
        if (!flags) break;
        if (pos + 4 > len) break;
        size = RD32(data + pos); pos += 4;
        if (pos + size > len) size = (u32)(len - pos);
        memset(&e, 0, sizeof(e));
        e.flags = flags & all;
        if (flags & CE_KEY_PRESS) { if (size < 1) break; e.keycode = (e.flags & CE_KEY_PRESS) ? data[pos] : 0; e.code = data + pos + 1; e.len = size - 1; }
        else { e.code = data + pos; e.len = size; }
        pos += size;
        if (n == cap) { int nc = cap ? cap * 2 : 4; ClipEvent *nl = (ClipEvent*)realloc(list, (size_t)nc * sizeof(ClipEvent)); if (!nl) break; list = nl; cap = nc; }
        list[n++] = e;
    }
#undef RD32
    free(s->events);
    s->events = list; s->nevents = n; s->event_flags = all;
}

static int has_unload_handler(MovieClip *c) {
    A1Clip *s = c->a1; int i;
    if (!s) return 0;
    for (i = 0; i < s->nevents; i++) if (s->events[i].flags & CE_UNLOAD) return 1;
    return 0;
}

/* ---------------------------------------------------------------- */
/* construction                                                     */

static AObj *registered_ctor(VM *vm, MovieClip *c) {
    SWFMovie *m = c->movie; int i, id; const char *name = NULL; Env *e; Prop *p; AStr *n; int v;
    if (!c->self || !m) return NULL;
    id = c->self->char_id;
    for (i = 0; i < m->nexports; i++) if (m->exports[i].id == id) name = m->exports[i].name;
    if (!name) return NULL;
    v = m->version;
    e = &vm->env[v >= 7 ? 1 : 0];
    n = str_from_swf(vm, name, strlen(name), v);
    p = pm_get(&e->registry, n, v >= 7);
    return p && p->v.t == V_OBJ ? p->v.u.o : NULL;
}

/* the AS object of a movie clip, with the given prototype */
static AObj *make_clip_obj(VM *vm, MovieClip *c, Value proto) {
    AObj *o = (AObj*)c->obj;
    if (o) {   /* created early by a lookup: keep its identity */
        if (proto.t != V_UNDEF) o_define(vm, o, vm->s_proto, proto, PA_DONT_ENUM | PA_DONT_DELETE);
        return o;
    }
    o = obj_bare(vm);
    if (!o) return NULL;
    o->nk = NK_MOVIECLIP; o->nat.clip = c;
    if (proto.t != V_UNDEF) o_define(vm, o, vm->s_proto, proto, PA_DONT_ENUM | PA_DONT_DELETE);
    c->obj = (struct AObj*)o;
    return o;
}

static void set_default_name(VM *vm, MovieClip *c) {
    if (c->self && !c->self->name[0] && !c->self->explicit_name) {
        sprintf(c->self->name, "instance%d", vm->instance_counter++);
    }
}

void avm1_clip_run_frame(Player *p, MovieClip *c);

/* MovieClip::post_instantiation (+ construct_as_avm1_object) */
void avm1_post_instantiation_ex(VM *vm, MovieClip *c, AObj *init, int by_avm, int run_frame) {
    A1Clip *s = a1_of(c);
    if (!s || s->post_inst) return;
    s->post_inst = 1;
    if (!clip_is_movieclip(c)) {
        if (s->kind != 3) set_default_name(vm, c);     /* videos keep an empty name */
        if (s->kind == 2 || (c->self && c->self->ch && c->self->ch->type == CHAR_EDITTEXT)) a1_tf_post_instantiation(vm, c);
        else if (s->kind == 3) {
            if (!c->obj) {
                Act a; Value proto = v_undef(); AObj *o, *co;
                act_init_nothing(&a, vm, c, "[Construct]");
                co = a1_resolve_class(&a, "Video");
                if (!a1_class_proto(&a, co, &proto)) proto = v_undef();
                o = obj_bare(vm);
                if (!o) return;
                o->nk = NK_VIDEO; o->nat.clip = c;
                if (proto.t != V_UNDEF) o_define(vm, o, vm->s_proto, proto, PA_DONT_ENUM | PA_DONT_DELETE);
                c->obj = (struct AObj*)o;
                if (co) { o_construct_on_existing(&a, co, o, 0, NULL); if (vm->err) vm->err = 0; }
                { extern void a1_video_populate_proto(Act *a, AObj *proto); AObj *po = proto.t == V_OBJ ? proto.u.o : NULL; if (po) a1_video_populate_proto(&a, po); if (vm->err) vm->err = 0; }
            }
        }
        else a1_button_post_instantiation(vm, c);
        return;
    }
    set_default_name(vm, c);
    exec_add(vm, c);
    if (!c->obj) {
        Act a; AObj *ctor, *o; Value proto = v_undef();
        act_init_nothing(&a, vm, c, "[Construct]");
        ctor = registered_ctor(vm, c);
        if (!ctor || !a1_class_proto(&a, ctor, &proto)) {
            AObj *mc = ctor ? NULL : a1_resolve_class(&a, "MovieClip");
            if (!a1_class_proto(&a, mc, &proto)) proto = v_undef();
        }
        o = make_clip_obj(vm, c, proto);
        if (!o) return;
        s->orig_path = clip_path(vm, c);
        if (run_frame) avm1_clip_run_frame(vm->player, c);
        if (init) {
            int n = 0, i; AStr **keys = o_keys(&a, init, 0, &n);
            for (i = n - 1; i >= 0; i--) {
                Value v = o_get(&a, init, keys[i]);
                if (vm->err) { if (vm->err != ERR_THROWN) break; vm->err = 0; continue; }
                o_set(&a, o, keys[i], v);
                if (vm->err) { if (vm->err != ERR_THROWN) break; vm->err = 0; }
            }
            free(keys);
        }
        if (ctor && by_avm) {
            o_construct_on_existing(&a, ctor, o, 0, NULL);
            if (vm->err) vm_report_error(&a);
            return;
        }
        {
            int i, nev = 0; QAction *q;
            for (i = 0; i < s->nevents; i++) {
                if (s->events[i].flags & CE_INITIALIZE) { QAction *qi = q_push(vm, c, QA_INIT, 0); if (qi) { qi->code = s->events[i].code; qi->len = s->events[i].len; } }
                if (s->events[i].flags & CE_CONSTRUCT) nev++;
            }
            q = q_push(vm, c, QA_CONSTRUCT, 0);
            if (q) {
                q->ctor = ctor;
                if (nev) {
                    int k = 0;
                    q->ev_code = (const u8**)malloc((size_t)nev * sizeof(u8*));
                    q->ev_len = (u32*)malloc((size_t)nev * sizeof(u32));
                    if (q->ev_code && q->ev_len) {
                        for (i = 0; i < s->nevents; i++) if (s->events[i].flags & CE_CONSTRUCT) { q->ev_code[k] = s->events[i].code; q->ev_len[k] = s->events[i].len; k++; }
                        q->nev = k;
                    }
                }
            }
        }
    } else if (run_frame) avm1_clip_run_frame(vm->player, c);
    a1_tf_bind_pending(vm);
}

void avm1_post_instantiation(Player *p, MovieClip *c) {
    VM *vm = vmof(p);
    if (vm) avm1_post_instantiation_ex(vm, c, NULL, 0, 0);
}

/* MovieClip::run_frame_avm1 */
void avm1_clip_run_frame(Player *p, MovieClip *c) {
    VM *vm = vmof(p); A1Clip *s;
    if (!vm || !c || c->dead) return;
    if (c->movie && c->movie->has_abc) return;
    s = a1_of(c);
    if (!s) return;
    if (!s->initialized) { avm1_clip_event(vm, c, CE_LOAD, 0); s->initialized = 1; }
    else avm1_clip_event(vm, c, CE_ENTER_FRAME, 0);
    if (c->playing) player_avm1_advance(c);
}

/* ---------------------------------------------------------------- */
/* removal                                                          */

static int should_delay_removal(VM *vm, DisplayObject *d) {
    MovieClip *c = d->clip; DisplayObject *k;
    if (!c) return 0;
    if (clip_is_movieclip(c)) {
        if (has_unload_handler(c)) return 1;
        /* SWF5 movies have no onUnload property handlers to wait for */
        if (c->obj && clip_swf_version(c) >= 6) {
            Act a; int has;
            act_init_nothing(&a, vm, clip_avm1_root(c), "[Unload Handler Check]");
            has = o_has_property(&a, (AObj*)c->obj, vm->s_onUnload);
            if (vm->err) vm->err = 0;
            if (has) return 1;
        }
    }
    for (k = c->children; k; k = k->next) if (should_delay_removal(vm, k)) return 1;
    return 0;
}

static void unload_tree(VM *vm, MovieClip *c);
/* unload events for a tree that waits for removal (children first): going
 * up the depths, children without unload handlers go at once until one with
 * a handler comes; the ones above it stay with it (Gnash MovieClip.as) */
static void queue_unload_tree(VM *vm, MovieClip *c) {
    DisplayObject *k, *nk; A1Clip *s = a1_of(c); int held = 0;
    if (s && s->pending_removal) return;   /* already unloading with its parent: one onUnload */
    for (k = c->children; k; k = nk) {
        nk = k->next;
        if (!k->clip) continue;
        if (should_delay_removal(vm, k)) { queue_unload_tree(vm, k->clip); held = 1; }
        else if (!held) { unload_tree(vm, k->clip); player_unlink_dobj(c, k); }
    }
    if (s) s->pending_removal = 1;
    if (clip_is_movieclip(c)) avm1_clip_event(vm, c, CE_UNLOAD, 0);
}
static void queue_removal(VM *vm, DisplayObject *d) {
    MovieClip *c = d->clip;
    if (c) queue_unload_tree(vm, c);
    /* only the removed object moves to a negative depth */
    d->depth = -d->depth - 1;
    if (c && c->parent) player_resort(c->parent);
}

/* DisplayObject::avm1_unload for a whole tree */
static void unload_tree(VM *vm, MovieClip *c) {
    DisplayObject *k; A1Clip *s = a1_of(c);
    if (!s || s->removed) return;
    for (k = c->children; k; k = k->next) if (k->clip) unload_tree(vm, k->clip);
    if (s->maskee) { if (s->maskee->a1) s->maskee->a1->masker = NULL; s->maskee = NULL; }
    else if (s->masker) { if (s->masker->a1) s->masker->a1->maskee = NULL; s->masker = NULL; }
    a1_tf_unbind(vm, c);
    if (vm->player->focus == c) player_set_focus(vm->player, NULL);
    if (vm->drag == c) vm->drag = NULL;
    if (clip_is_movieclip(c) && !s->pending_removal) avm1_clip_event(vm, c, CE_UNLOAD, 0);
    s->removed = 1;
}

void avm1_unload_clip(VM *vm, MovieClip *c) { unload_tree(vm, c); }

/* Container::remove_child (AVM1) */
void avm1_remove_child(Player *p, MovieClip *parent, DisplayObject *d) {
    VM *vm = vmof(p);
    if (vm && d->clip && should_delay_removal(vm, d)) { queue_removal(vm, d); return; }
    if (vm && d->clip) unload_tree(vm, d->clip);
    player_unlink_dobj(parent, d);
}

/* objects waiting for removal leave at the start of the next frame */
static void remove_pending_in(VM *vm, MovieClip *c) {
    DisplayObject *d = c->children, *n;
    while (d) {
        n = d->next;
        if (d->clip && d->clip->a1 && d->clip->a1->pending_removal) {
            unload_tree(vm, d->clip);
            player_unlink_dobj(c, d);
        } else if (d->clip) remove_pending_in(vm, d->clip);
        d = n;
    }
}

/* a button changing state again: children it removed earlier (waiting on
   their unload handlers) are gone by now in Flash */
void avm1_remove_pending_children(VM *vm, MovieClip *c) { remove_pending_in(vm, c); }

/* replace_at_depth: the object at `depth` is unloaded right away */
void avm1_replace_at_depth(VM *vm, MovieClip *parent, int depth) {
    DisplayObject *d = clip_find_depth(parent, depth);
    if (!d) return;
    if (d->clip) unload_tree(vm, d->clip);
    player_unlink_dobj(parent, d);
}

/* ---------------------------------------------------------------- */
/* the frame                                                        */

static void run_timers(VM *vm, double dt_ms);

void avm1_frame(Player *p) {
    VM *vm = vmof(p); MovieClip *prev = NULL, *c, *next; double fr;
    if (!vm) return;
    vm->frame_running = 1;
    if (p->avm2) { MovieClip *roots[64]; int nr = avm1_stage_roots(vm, roots, 64), i; for (i = 0; i < nr; i++) remove_pending_in(vm, roots[i]); }
    else {
        remove_pending_in(vm, p->root);
        { DisplayObject *ld; for (ld = p->levels; ld; ld = ld->next) if (ld->clip) remove_pending_in(vm, ld->clip); }
    }
    for (c = vm->exec_list; c; c = next) {
        next = c->a1 ? c->a1->next_exec : NULL;
        if (!c->a1 || c->a1->removed || c->dead) {
            if (prev) prev->a1->next_exec = next; else vm->exec_list = next;
            if (c->a1) { c->a1->next_exec = NULL; c->a1->in_exec = 0; }
        } else if (c->a1->mix_wait) {
            c->a1->mix_wait = 0;   /* an AVM1Movie loaded during this frame starts with the next one */
            prev = c;
        } else {
            avm1_clip_run_frame(p, c);
            prev = c;
        }
    }
    { extern void avm1_loads_frame(VM *vm); avm1_loads_frame(vm); }
    vm->sp = 0;
    vm->gregs[0] = vm->gregs[1] = vm->gregs[2] = vm->gregs[3] = v_undef();
    avm1_run_actions(vm);
    { extern void avm1_sound_frame(VM *vm); avm1_sound_frame(vm); }
    { extern void avm1_net_flush(VM *vm); avm1_net_flush(vm); avm1_run_actions(vm); }
    { extern void avm1_net_frame(VM *vm); avm1_net_frame(vm); avm1_run_actions(vm); }
    { extern void avm1_sockets_update(VM *vm); avm1_sockets_update(vm); }
    fr = p->movie && p->movie->framerate > 0 ? p->movie->framerate : 12.0;
    run_timers(vm, 1000.0 / fr);
    { extern void avm1_netstream_tick(VM *vm, double dt); avm1_netstream_tick(vm, 1000.0 / fr); }
    { extern void avm1_loads_run(VM *vm); avm1_loads_run(vm); }
    { extern void avm1_netstream_loads(VM *vm); avm1_netstream_loads(vm); }
    { extern void avm1_sound_loads(VM *vm); avm1_sound_loads(vm); }
    { extern void avm1_fileref_run(VM *vm); avm1_fileref_run(vm); }
    { extern void avm1_sockets_pump(VM *vm); avm1_sockets_pump(vm); }
    vm->frame_running = 0;
    if (vm->gc_count > vm->gc_threshold) gc_collect(vm);
}

/* load: init actions, the root's object, then the first frame */
void avm1_start(Player *p) {
    VM *vm = vmof(p); Timeline *tl; int i;
    if (!vm) return;
    vm->instance_counter = 1;
    if (!p->root->self) {
        /* the root's placement on the stage: _level0 can be moved and scaled */
        DisplayObject *d = (DisplayObject*)calloc(1, sizeof(DisplayObject));
        if (d) { d->char_id = -1; d->visible = 1; d->explicit_name = 1; swf_matrix_identity(&d->matrix); swf_cxform_identity(&d->cxform); d->clip = p->root; p->root->self = d; }
    }
    if (p->root->self) p->root->self->name[0] = 0;
    a1_of(p->root);
    avm1_post_instantiation_ex(vm, p->root, NULL, 0, 0);
    /* the player version, for scripts that read $version */
    if (p->root->obj) { extern const char *g_flash_version; o_define_c(vm, (AObj*)p->root->obj, "$version", v_str(str_intern(vm, g_flash_version)), 0); }
    /* DoInitAction tags run while the movie loads, before frame 1 */
    tl = p->root->tl;
    for (i = 0; tl && i < tl->ntags; i++) {
        const TagRef *t = &tl->tags[i];
        if (t->code == TAG_DoInitAction && t->len > 2) {
            int id = t->data[0] | (t->data[1] << 8);
            SWFChar *sc = swf_get_char(p->root->movie, id);
            if (sc) sc->flags |= 1;
            run_code(vm, p->root, t->data + 2, (u32)(t->len - 2), 1);
            vm->sp = 0;
        } else if (t->code == TAG_ImportAssets || t->code == TAG_ImportAssets2) {
            extern void avm1_import_assets(VM *vm, MovieClip *c, SWFMovie *m, const TagRef *t);
            avm1_run_actions(vm);
            avm1_import_assets(vm, p->root, p->root->movie, t);
        }
    }
    avm1_run_actions(vm);
    avm1_frame(p);
}

/* ---------------------------------------------------------------- */
/* AVM1 movies inside an AVM2 player (flash.display.AVM1Movie)      */

static void mix_register(VM *vm, MovieClip *c);
MovieClip *avm1_base_root(VM *vm) {
    Player *p = vm->player;
    if (p && p->avm2 && vm->nmix > 0) return vm->mix[vm->nmix - 1];
    return p ? p->root : NULL;
}
int avm1_stage_roots(VM *vm, MovieClip **out, int cap) {
    Player *p = vm->player; int n = 0, i; DisplayObject *d;
    if (!p || cap <= 0) return 0;
    if (p->avm2) { for (i = 0; i < vm->nmix && n < cap; i++) out[n++] = vm->mix[i]; }
    else out[n++] = p->root;
    for (d = p->levels; d && n < cap; d = d->next) if (d->clip) out[n++] = d->clip;
    return n;
}
int avm1_mix_active(Player *p) { VM *vm = vmof(p); return vm && vm->nmix > 0; }
/* the main timeline of a loaded AVM1 movie (Ruffle loader.rs: post_instantiation, then the
   preload runs its init actions); its first frame runs in the next AVM1 frame */
void avm1_mix_start(Player *p, MovieClip *c) {
    VM *vm = vmof(p); A1Clip *s; extern void avm1_preload_movie(VM *vm, MovieClip *c, SWFMovie *m);
    if (!vm || !c) return;
    mix_register(vm, c);
    s = a1_of(c);
    if (!s) return;
    s->is_root = 1;
    if (c->self) { c->self->name[0] = 0; c->self->explicit_name = 1; }   /* as a _level: its clips are instance1, instance2 ... */
    avm1_post_instantiation_ex(vm, c, NULL, 0, 0);
    if (c->obj) { extern const char *g_flash_version; o_define_c(vm, (AObj*)c->obj, "$version", v_str(str_intern(vm, g_flash_version)), 0); }
    avm1_preload_movie(vm, c, c->movie);
    avm1_run_actions(vm);
    vm->sp = 0;
}
static void mix_register(VM *vm, MovieClip *c) {
    int i;
    for (i = 0; i < vm->nmix; i++) if (vm->mix[i] == c) return;
    if (vm->nmix == vm->capmix) {
        int nc = vm->capmix ? vm->capmix * 2 : 8; MovieClip **nl = (MovieClip**)realloc(vm->mix, (size_t)nc * sizeof(MovieClip*));
        if (!nl) return;
        vm->mix = nl; vm->capmix = nc;
    }
    vm->mix[vm->nmix++] = c;
}
/* _levelN of AVM1 code in an AVM1Movie: the Loader's children by depth (Ruffle avm1_stage / get_level);
   create: a new level clip in that Loader */
MovieClip *avm1_mix_level(VM *vm, MovieClip *base, int level, int create) {
    extern MovieClip *player_mix_level_create(Player *p, MovieClip *stage, int level, SWFMovie *m);
    MovieClip *r = base, *st; DisplayObject *d; int g = 0;
    if (!r || !CLIP_A1MIX(r)) return NULL;
    while (r->parent && CLIP_A1MIX(r->parent) && g++ < 10000) r = r->parent;
    st = r->parent;
    if (!st) return level == r->self->depth ? r : NULL;
    for (d = st->children; d; d = d->next) if (d->depth == level && d->clip && CLIP_A1MIX(d->clip) && !clip_removed(d->clip)) return d->clip;
    if (!create) return NULL;
    r = player_mix_level_create(vm->player, st, level, base->movie);
    if (!r) return NULL;
    if (r->self) { r->self->name[0] = 0; r->self->explicit_name = 1; }
    a1_of(r);
    mix_register(vm, r);
    avm1_post_instantiation_ex(vm, r, NULL, 0, 0);
    return r;
}

/* the AVM1 frame of a player whose root is AVM2: after the AVM2 phases (Ruffle Player::run_frame) */
void avm1_mix_frame(Player *p) {
    extern void avm1_input_after_frame(Player *p);
    VM *vm = vmof(p);
    if (!vm || !vm->nmix) return;
    avm1_reset_exec_clock(p);
    avm1_frame(p);
    avm1_input_after_frame(p);
}

/* ---------------------------------------------------------------- */
/* timers                                                           */

static void run_timers(VM *vm, double dt_ms) {
    int ticks = 0;
    vm->time_ms += dt_ms;
    for (;;) {
        int i, best = -1; Timer *t; Act a; int id;
        double now = vm->time_ms * 1000.0;
        for (i = 0; i < MAX_TIMERS; i++) {
            Timer *x = &vm->timers[i];
            if (!x->active) continue;
            if (best < 0 || x->next < vm->timers[best].next || (x->next == vm->timers[best].next && x->id < vm->timers[best].id)) best = i;
        }
        if (best < 0 || !(vm->timers[best].next < now)) break;
        if (++ticks > 10) { vm->time_ms = (vm->timers[best].next - 100.0) / 1000.0; break; }
        t = &vm->timers[best]; id = t->id;
        if (avm1_base_root(vm)) {
            act_init_nothing(&a, vm, avm1_base_root(vm), "[Timer Callback]");
            if (t->fn) {
                o_call(&a, t->fn, v_undef(), t->nargs, t->args);
                if (vm->err) vm_report_error(&a);
            } else if (t->self) {
                MovieClip *d = o_display_no_super(t->self);
                if (d && clip_removed(d)) { avm1_timer_remove(vm, best); continue; }
                o_call_method(&a, t->self, t->method, t->nargs, t->args, ER_SPECIAL);
                if (vm->err) vm_report_error(&a);
            }
        }
        avm1_run_actions(vm);
        t = &vm->timers[best];
        if (t->active && t->id == id) {
            if (t->is_timeout) avm1_timer_remove(vm, best);
            else t->next += t->interval;
        }
    }
}

/* ---------------------------------------------------------------- */
/* player hooks                                                     */

void avm1_run(Player *p, MovieClip *target, const u8 *code, size_t len) {
    VM *vm = vmof(p);
    if (vm) run_code(vm, target, code, (u32)len, 0);
}

void avm1_clip_created(Player *p, MovieClip *c) { (void)p; (void)c; }
void avm1_clip_loaded(Player *p, MovieClip *c) { (void)p; (void)c; }
void avm1_clip_load_events(Player *p, MovieClip *c) { (void)p; (void)c; }
void avm1_tick(Player *p) { (void)p; }

void avm1_clip_unload(Player *p, MovieClip *c) {
    VM *vm = vmof(p);
    if (vm && c && c->a1 && !c->a1->removed && CLIP_IS_A1(c)) unload_tree(vm, c);
}
void avm1_clip_destroyed(Player *p, MovieClip *c) { (void)p; (void)c; }

/* the clip's memory is about to be freed */
void avm1_clip_detach(Player *p, MovieClip *c) {
    VM *vm = vmof(p);
    if (vm && c->a1 && c->a1->in_exec) exec_unlink(vm, c);
    if (vm) { int k, i; for (k = 0; k < 3; k++) for (i = vm->queue[k].head; i < vm->queue[k].n; i++) if (vm->queue[k].a[i].clip == c) vm->queue[k].a[i].clip = NULL; }   /* (freed at once by an AVM2 owner) */
    if (vm && vm->nmix) { int i; for (i = 0; i < vm->nmix; i++) if (vm->mix[i] == c) { memmove(vm->mix + i, vm->mix + i + 1, (size_t)(vm->nmix - i - 1) * sizeof(MovieClip*)); vm->nmix--; break; } }
    if (vm && vm->drag == c) vm->drag = NULL;
    if (vm) { extern void avm1_input_forget(VM *vm, MovieClip *c); avm1_input_forget(vm, c); }
    { extern void avm1_button_free(MovieClip *b); avm1_button_free(c); }
    if (c->obj) { AObj *o = (AObj*)c->obj; o->nat.clip = NULL; c->obj = NULL; }
    if (c->a1) {
        A1Clip *s = c->a1;
        if (s->masker && s->masker->a1 && s->masker->a1->maskee == c) s->masker->a1->maskee = NULL;
        if (s->maskee && s->maskee->a1 && s->maskee->a1->masker == c) s->maskee->a1->masker = NULL;
        if (vm) a1_tf_unbind(vm, c);
        a1_drawing_free(c);
        { extern void a1_filters_free(void *p); a1_filters_free(s->filters); s->filters = NULL; }
        if (s->tfs) { tfs_free(s->tfs); free(s->tfs); }
        free(s->events); free(s->bindings);
        free(s);
        c->a1 = NULL;
    }
}

void avm1_bind_textfield(Player *p, MovieClip *c) {
    VM *vm = vmof(p); A1Clip *s;
    if (!CLIP_IS_A1(c)) return;
    s = a1_of(c);
    if (!s) return;
    s->kind = 2;
    if (vm) a1_tf_init(vm, c);
}
void avm1_bind_button(Player *p, MovieClip *c) { A1Clip *s; (void)p; if (!CLIP_IS_A1(c)) return; s = a1_of(c); if (s) s->kind = 1; }
void avm1_bind_video(Player *p, MovieClip *c) { A1Clip *s; (void)p; if (!CLIP_IS_A1(c)) return; s = a1_of(c); if (s) s->kind = 3; }

/* ---------------------------------------------------------------- */
/* levels                                                           */

MovieClip *avm1_level(Player *p, int level) { return player_level(p, level); }

/* get_or_create_level: an empty clip on the stage at depth `level` */
MovieClip *avm1_level_create(Player *p, int level, SWFMovie *movie) {
    VM *vm = vmof(p); MovieClip *c = player_level(p, level);
    if (c) return c;
    c = player_level_create(p, level, movie);
    if (!c || !vm) return c;
    if (c->self) { c->self->name[0] = 0; c->self->explicit_name = 1; }
    a1_of(c);
    avm1_post_instantiation_ex(vm, c, NULL, 0, 0);
    return c;
}

/* ---------------------------------------------------------------- */
/* GC roots                                                         */

static void mark_clip_tree(VM *vm, MovieClip *c) {
    DisplayObject *d;
    if (!c) return;
    if (c->obj) gc_mark_obj(vm, (AObj*)c->obj);
    if (c->a1) {
        A1Clip *s = c->a1; int i;
        gc_mark_str(s->tf_var); gc_mark_str(s->orig_path);
        for (i = 0; i < s->nbindings; i++) gc_mark_str(s->bindings[i].var);
    }
    for (d = c->children; d; d = d->next) if (d->clip) mark_clip_tree(vm, d->clip);
    if (c->a1) {
        if (c->a1->drop_target) gc_mark_obj(vm, c->a1->drop_target);
        if (c->a1->style_sheet) gc_mark_obj(vm, c->a1->style_sheet);
        for (d = c->a1->btn_hit; d; d = d->next) if (d->clip) mark_clip_tree(vm, d->clip);
    }
}

extern void avm1_mark_extra(VM *vm);

void avm1_mark_roots(VM *vm) {
    int i, e; Player *p = vm->player;
    for (i = 0; i < NSDEFS; i++) gc_mark_str(*(AStr**)((char*)vm + sdefs[i].off));
    for (i = 0; i < 256; i++) gc_mark_str(vm->small_int[i]);
    for (e = 0; e < 2; e++) {
        Env *en = &vm->env[e]; AObj **pp = (AObj**)&en->p; int k, np = (int)(sizeof(Protos) / sizeof(AObj*));
        if (!en->ready) continue;
        if (en->global) gc_mark_obj(vm, en->global);
        if (en->gscope) gc_mark_hdr(vm, &en->gscope->gc);
        for (k = 0; k < np; k++) if (pp[k]) gc_mark_obj(vm, pp[k]);
        for (k = 0; k < en->registry.n; k++) if (en->registry.items[k].key) { gc_mark_str(en->registry.items[k].key); gc_mark_value(vm, en->registry.items[k].v); }
    }
    { extern void avm1_net_mark(VM *vm); avm1_net_mark(vm); }
    { extern void avm1_sound_mark(VM *vm); avm1_sound_mark(vm); }
    { extern void avm1_sockets_mark(VM *vm); avm1_sockets_mark(vm); }
    { extern void avm1_netstream_mark(VM *vm); avm1_netstream_mark(vm); }
    { extern void avm1_ei_mark(VM *vm); avm1_ei_mark(vm); }
    { extern void avm1_fileref_mark(VM *vm); avm1_fileref_mark(vm); }
    for (i = 0; i < vm->so_map.n; i++) if (vm->so_map.items[i].key) { gc_mark_str(vm->so_map.items[i].key); gc_mark_value(vm, vm->so_map.items[i].v); }
    for (i = 0; i < vm->sp; i++) gc_mark_value(vm, vm->stack[i]);
    for (i = 0; i < 4; i++) gc_mark_value(vm, vm->gregs[i]);
    if (vm->cpool) gc_mark_hdr(vm, &vm->cpool->gc);
    gc_mark_value(vm, vm->thrown);
    for (i = 0; i < MAX_TIMERS; i++) {
        Timer *t = &vm->timers[i]; int k;
        if (!t->active) continue;
        if (t->fn) gc_mark_obj(vm, t->fn);
        if (t->self) gc_mark_obj(vm, t->self);
        gc_mark_str(t->method);
        for (k = 0; k < t->nargs; k++) gc_mark_value(vm, t->args[k]);
    }
    for (e = 0; e < 3; e++) {
        QList *q = &vm->queue[e];
        for (i = q->head; i < q->n; i++) {
            int k;
            if (q->a[i].obj) gc_mark_obj(vm, q->a[i].obj);
            if (q->a[i].ctor) gc_mark_obj(vm, q->a[i].ctor);
            gc_mark_str(q->a[i].name); gc_mark_str(q->a[i].method);
            for (k = 0; k < q->a[i].nargs; k++) gc_mark_value(vm, q->a[i].args[k]);
        }
    }
    if (p) {
        DisplayObject *d;
        mark_clip_tree(vm, p->root);
        for (i = 0; i < vm->nmix; i++) mark_clip_tree(vm, vm->mix[i]);   /* AVM1Movie content (also off the display list) */
        { DisplayObject *ld; for (ld = p->levels; ld; ld = ld->next) if (ld->clip) mark_clip_tree(vm, ld->clip); }
        { extern void avm1_loads_mark(VM *vm); avm1_loads_mark(vm); }
        /* removed clips that still run their unload events */
        for (d = p->graveyard; d; d = d->next) if (d->clip) mark_clip_tree(vm, d->clip);
        for (d = p->graveyard_old; d; d = d->next) if (d->clip) mark_clip_tree(vm, d->clip);
    }
    avm1_mark_extra(vm);
}

/* ---------------------------------------------------------------- */
/* input                                                            */

static void broadcast_clip_event(VM *vm, MovieClip *c, u32 flag, int keycode) {
    DisplayObject *d; MovieClip *list[512]; int n = 0, i;
    if (!c || c->dead) return;
    for (d = c->children; d; d = d->next) if (d->clip && n < 512) list[n++] = d->clip;
    /* children first, highest depth first */
    for (i = n - 1; i >= 0; i--) broadcast_clip_event(vm, list[i], flag, keycode);
    if (clip_is_movieclip(c) && !clip_removed(c)) avm1_clip_event(vm, c, flag, keycode);
}

static int key_ascii(int keycode, int shift) {
    if (keycode >= 'A' && keycode <= 'Z') return shift ? keycode : keycode + 32;
    if (keycode >= '0' && keycode <= '9') return keycode;
    if (keycode == 32) return 32;
    return 0;
}
int avm1_last_key_code, avm1_last_key_ascii;

void avm1_key_event(Player *p, int keycode, int down) {
    VM *vm = vmof(p);
    if (!vm || p->avm2) return;
    if (down) { avm1_last_key_code = keycode; avm1_last_key_ascii = key_ascii(keycode, p->key_down[16]); }
    broadcast_clip_event(vm, p->root, down ? CE_KEY_DOWN : CE_KEY_UP, 0);
    avm1_queue_notify(vm, p->root, "Key", down ? vm->s_onKeyDown : vm->s_onKeyUp, 0, NULL);
    avm1_run_actions(vm);
}

void avm1_mouse_event(Player *p, int x, int y, int down, int changed) {
    VM *vm = vmof(p);
    (void)x; (void)y;
    if (!vm || p->avm2) return;
    if (changed) {
        broadcast_clip_event(vm, p->root, down ? CE_MOUSE_DOWN : CE_MOUSE_UP, 0);
        avm1_queue_notify(vm, p->root, "Mouse", down ? vm->s_onMouseDown : vm->s_onMouseUp, 0, NULL);
    } else {
        broadcast_clip_event(vm, p->root, CE_MOUSE_MOVE, 0);
        avm1_queue_notify(vm, p->root, "Mouse", vm->s_onMouseMove, 0, NULL);
    }
    avm1_run_actions(vm);
}

void avm1_focus_changed(Player *p, MovieClip *c) { (void)p; (void)c; }

/* ---------------------------------------------------------------- */
/* player state used by stage properties                            */

int avm1_mouse_x(Player *p) { return (int)(p->mouse_x * TWIPS_PER_PX / p->zoom / 20.0); }
int avm1_mouse_y(Player *p) { return (int)(p->mouse_y * TWIPS_PER_PX / p->zoom / 20.0); }
/* the stage quality is the renderer's (g_stage_quality: 0 low 1 medium 2 high 3 best 4.. 8x8 / 16x16) */
extern int g_stage_quality;
int avm1_quality(Player *p) { (void)p; return g_stage_quality > 3 ? 2 : g_stage_quality; }
void avm1_set_quality(Player *p, int q) { (void)p; g_stage_quality = q; }
void avm1_toggle_quality(Player *p) { (void)p; g_stage_quality = g_stage_quality >= 2 ? 0 : 2; }
int avm1_stage_focus_rect(Player *p) { VM *vm = vmof(p); return vm ? vm->stage_focus_rect : 1; }
void avm1_set_stage_focus_rect(Player *p, int v) { VM *vm = vmof(p); if (vm) vm->stage_focus_rect = v; }
int avm1_sound_buf_time(Player *p) { VM *vm = vmof(p); return vm ? vm->sound_buf_time : 5; }
void avm1_set_sound_buf_time(Player *p, int v) { VM *vm = vmof(p); if (vm) vm->sound_buf_time = v; }
MovieClip *avm1_drop_target(MovieClip *c) { return c && c->a1 && c->a1->drop_target ? o_display(c->a1->drop_target) : NULL; }
void avm1_end_drag(Player *p) { VM *vm = vmof(p); if (vm) vm->drag = NULL; }

/* timeline control used by the interpreter */
void avm1_next_frame(MovieClip *c) { int f = clip_current_frame(c) + 1; if (c->cur_frame + 1 < c->frame_count) player_avm1_goto(c, f, 0); }
void avm1_prev_frame(MovieClip *c) { if (c->cur_frame >= 1) player_avm1_goto(c, c->cur_frame - 1, 0); }
void avm1_stop_clip(MovieClip *c) { c->playing = 0; }
void avm1_play_clip(MovieClip *c) { c->playing = 1; }
void avm1_goto(MovieClip *c, int frame1, int stop) { if (frame1 >= 1) player_avm1_goto(c, frame1 - 1, !stop); else if (stop) avm1_stop_clip(c); }
int avm1_frames_loaded(MovieClip *c) { return c->frame_count; }

/* frame labels are case insensitive (ASCII) in AVM1 */
int clip_find_frame_label_ci(MovieClip *c, const char *label) {
    int i, frame = 0;
    if (!c->tl) return -1;
    for (i = 0; i < c->tl->ntags; i++) {
        const TagRef *t = &c->tl->tags[i];
        if (t->code == TAG_FrameLabel) {
            size_t k = 0;
            for (;; k++) {
                int a = k < t->len ? ((const unsigned char*)t->data)[k] : 0, b = (unsigned char)label[k];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) break;
                if (!a) return frame;
            }
        } else if (t->code == TAG_ShowFrame) frame++;
    }
    return -1;
}

/* debug dump (host -d) */
void avm1_debug_dump(Player *p, void *fout) { (void)p; (void)fout; }

/* a button's AS handler (onPress, onRelease ...) */
void avm1_button_method(Player *p, MovieClip *b, const char *handler) {
    VM *vm = vmof(p);
    if (!vm || !b || !b->obj || clip_swf_version(b) < 6) return;
    queue_method(vm, b, (AObj*)b->obj, str_intern(vm, handler), 0);
}
