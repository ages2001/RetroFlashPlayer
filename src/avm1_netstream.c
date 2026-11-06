/* avm1_netstream.c - AVM1 NetStream: progressive FLV playback events.
 *
 * Follows Ruffle's core/src/streams.rs: play() loads the file, the stream
 * ticks with the player and walks the FLV tags by timestamp, script tags
 * (onMetaData, ...) call methods on the NetStream object, and onStatus
 * reports Play.Start, Buffer.Full/Flush/Empty, Play.Stop and Seek.Notify.
 * Video frames are not decoded yet; audio is not played yet. */
#include "avm1_int.h"
#include "fetch.h"
#include "netstream.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

extern void avm1_run_actions(VM *vm);

#define DE  PA_DONT_ENUM
#define DD  PA_DONT_DELETE

extern AObj *lib_class(VM *vm, Env *env, NativeFn ctor, NativeFn call, AObj *super_proto, int proto_first, AObj **proto_out);
extern Value avm1_amf0_value(Act *a, const u8 **pp, const u8 *end);
extern int avm1_netconn_connected(AObj *o);

typedef struct NSData {
    AObj *self;
    NetStream *ns;
} NSData;

static void ns_free_cb(AObj *o) {
    NSData *d = (NSData*)o->nat.p;
    if (d) { ns_free(d->ns); free(d); }
    o->nat.p = NULL;
}
static NSData *ns_of(AObj *o) {
    if (o && o->nk == NK_SUPER) o = o->nat.sup.self;
    if (!o || o->nk != NK_NETSTREAM || !o->nat.p || o->nfree != ns_free_cb) return NULL;
    return (NSData*)o->nat.p;
}
NetStream *avm1_netstream_of(AObj *o) { NSData *d = ns_of(o); return d ? d->ns : NULL; }

/* onStatus({code, level} or {description, level, code}) */
static void cb_status(void *ud, const char *code, const char *level, const char *desc) {
    NSData *d = (NSData*)ud; VM *vm = (VM*)d->ns->player->avm; Act a; AObj *info; Value v;
    act_init_nothing(&a, vm, vm->player->root, "[NetStream Status Event]");
    info = obj_new(vm, PROTOS(&a)->object);
    if (!info) return;
    if (desc) { o_set(&a, info, str_intern(vm, "description"), v_str(str_intern(vm, desc))); o_set(&a, info, str_intern(vm, "level"), v_str(str_intern(vm, level))); o_set(&a, info, str_intern(vm, "code"), v_str(str_intern(vm, code))); }
    else { o_set(&a, info, str_intern(vm, "code"), v_str(str_intern(vm, code))); o_set(&a, info, str_intern(vm, "level"), v_str(str_intern(vm, level))); }
    v = v_obj(info);
    o_call_method(&a, d->self, str_intern(vm, "onStatus"), 1, &v, ER_SPECIAL);
    if (vm->err) vm_report_error(&a);
    avm1_run_actions(vm);
}
/* script data: the method of that name is called with the value */
static const u8 *cb_script(void *ud, const char *nm, const u8 *p, const u8 *end) {
    NSData *d = (NSData*)ud; VM *vm = (VM*)d->ns->player->avm; AStr *name; Value v; Act a; char an[96];
    name = str_from_swf(vm, nm, strlen(nm), 6);
    sprintf(an, "[FLV %.80s]", nm);
    act_init_nothing(&a, vm, vm->player->root, an);
    v = avm1_amf0_value(&a, &p, end);
    o_call_method(&a, d->self, name, 1, &v, ER_SPECIAL);
    if (vm->err) vm_report_error(&a);
    avm1_run_actions(vm);
    return p;
}

void avm1_netstream_tick(VM *vm, double dt) { ns_tick_all(vm->player, 0, dt); }
/* queued downloads finish after the frame's scripts (like Ruffle's futures) */
void avm1_netstream_loads(VM *vm) { ns_process_loads(vm->player, 0); }
static void mark_cb(void *ctx, void *ud) { gc_mark_obj((VM*)ctx, ((NSData*)ud)->self); }
void avm1_netstream_mark(VM *vm) { ns_mark_active(vm->player, 0, mark_cb, vm); }
void avm1_netstream_free(void) { }

/* ---------------------------------------------------------------- */
/* the class                                                        */

static void populate_proto(Act *a, AObj *proto);
static Value ns_ctor(Act *a, AObj *self, int argc, Value *argv) {
    AObj *t = o_native_target(self);
    /* a NetStream on a connected NetConnection gives NetStream.prototype its
       properties (Gnash NetStream.as: "Here's when !") */
    if (argc > 0 && argv[0].t == V_OBJ && avm1_netconn_connected(argv[0].u.o)) {
        Value pv; AObj *po;
        if (o_get_local_stored(a, self, a->vm->s_proto, &pv) && (po = v_obj_of(a, pv)) != NULL) populate_proto(a, po);
        if (a->vm->err) a->vm->err = 0;
    }
    if (t && t->nk == NK_NONE) {
        NSData *d = (NSData*)calloc(1, sizeof(NSData)); NSCallbacks cb;
        memset(&cb, 0, sizeof(cb)); cb.status = cb_status; cb.script = cb_script;
        if (d) { d->self = t; d->ns = ns_new(a->vm->player, 0, &cb, d); if (!d->ns) { free(d); return v_undef(); } t->nk = NK_NETSTREAM; t->nat.p = d; t->nfree = ns_free_cb; }
    }
    return v_undef();
}

static Value ns_play_m(Act *a, AObj *self, int argc, Value *argv) {
    NSData *d = ns_of(self); VM *vm = a->vm;
    if (!d) return v_undef();
    /* play() without a name does nothing (Gnash NetStream-SquareTest) */
    if (argc > 0 && argv[0].t != V_UNDEF && argv[0].t != V_NULL) {
        AStr *name = v_str_of(a, argv[0]);
        if (vm->err) return v_undef();
        ns_play(d->ns, str_c(vm, name));
    }
    return v_undef();
}
static Value ns_pause_m(Act *a, AObj *self, int argc, Value *argv) {
    NSData *d = ns_of(self);
    if (!d) return v_undef();
    if (argc < 1 || argv[0].t == V_UNDEF) ns_toggle(d->ns);
    else if (v_as_bool(argv[0], a->swf_version)) ns_pause(d->ns, 0);   /* no Pause.Notify in AVM1 */
    else ns_resume(d->ns);
    return v_undef();
}
static Value ns_seek_m(Act *a, AObj *self, int argc, Value *argv) {
    NSData *d = ns_of(self); double s;
    if (!d) return v_undef();
    s = v_num_of(a, argc > 0 ? argv[0] : v_undef()); if (a->vm->err) return v_undef();
    ns_seek(d->ns, s * 1000.0, 0);
    return v_undef();
}
static Value ns_set_buffer_time(Act *a, AObj *self, int argc, Value *argv) {
    NSData *d = ns_of(self); double s;
    if (!d) return v_undef();
    s = v_num_of(a, argc > 0 ? argv[0] : v_undef()); if (a->vm->err) return v_undef();
    d->ns->buffer_time = s;
    return v_undef();
}
static Value ns_true(Act *a, AObj *self, int argc, Value *argv) { (void)a; (void)self; (void)argc; (void)argv; return v_bool(1); }
static Value ns_noop(Act *a, AObj *self, int argc, Value *argv) { (void)a; (void)self; (void)argc; (void)argv; return v_undef(); }
static Value ns_buffer_length_m(Act *a, AObj *self, int argc, Value *argv) { NSData *d = ns_of(self); (void)a; (void)argc; (void)argv; return d ? v_num(ns_buffer_length(d->ns)) : v_undef(); }
static Value ns_zero(Act *a, AObj *self, int argc, Value *argv) { NSData *d = ns_of(self); (void)a; (void)argc; (void)argv; return d ? v_num(0) : v_undef(); }
static Value ns_buffer_time(Act *a, AObj *self, int argc, Value *argv) { NSData *d = ns_of(self); (void)a; (void)argc; (void)argv; return d ? v_num(d->ns->buffer_time) : v_undef(); }
static Value ns_bytes_loaded_m(Act *a, AObj *self, int argc, Value *argv) { NSData *d = ns_of(self); (void)a; (void)argc; (void)argv; return d ? v_num((double)ns_bytes_loaded(d->ns)) : v_undef(); }
static Value ns_bytes_total_m(Act *a, AObj *self, int argc, Value *argv) { NSData *d = ns_of(self); (void)a; (void)argc; (void)argv; return d ? v_num((double)ns_bytes_total(d->ns)) : v_undef(); }
static Value ns_time(Act *a, AObj *self, int argc, Value *argv) { NSData *d = ns_of(self); (void)a; (void)argc; (void)argv; return d ? v_num(d->ns->stream_time / 1000.0) : v_undef(); }

static void defm(VM *vm, Env *env, AObj *o, const char *name, NativeFn f, u16 attr) { o_define(vm, o, str_intern(vm, name), v_obj(fn_native(vm, env, f, NULL)), attr); }
static void defp(VM *vm, Env *env, AObj *o, const char *name, NativeFn g, NativeFn s, u16 attr) { o_add_property(vm, o, str_intern(vm, name), fn_native(vm, env, g, NULL), s ? fn_native(vm, env, s, NULL) : NULL, attr); }

AObj *lib_netstream(VM *vm, Env *env, AObj *object_proto) {
    AObj *proto, *c = lib_class(vm, env, NULL, ns_ctor, object_proto, 1, &proto);
    env->p.netstream = proto;
    /* Ruffle avm1/globals/netstream.rs PROTO_DECLS */
    defm(vm, env, proto, "publish", ns_noop, DE | DD);
    defm(vm, env, proto, "play", ns_play_m, DE | DD);
    defm(vm, env, proto, "play2", ns_noop, DE | DD);
    defm(vm, env, proto, "receiveAudio", ns_noop, DE | DD);
    defm(vm, env, proto, "receiveVideo", ns_noop, DE | DD);
    defm(vm, env, proto, "pause", ns_pause_m, DE | DD);
    defm(vm, env, proto, "seek", ns_seek_m, DE | DD);
    defm(vm, env, proto, "onPeerConnect", ns_true, DE | DD);
    defm(vm, env, proto, "close", ns_noop, DE | DD);
    defm(vm, env, proto, "attachAudio", ns_noop, DE | DD);
    defm(vm, env, proto, "attachVideo", ns_noop, DE | DD);
    defm(vm, env, proto, "send", ns_noop, DE | DD);
    defm(vm, env, proto, "setBufferTime", ns_set_buffer_time, DE | DD);
    defm(vm, env, proto, "getInfo", ns_noop, DE | DD);
    defp(vm, env, proto, "checkPolicyFile", ns_noop, NULL, DE | DD);
    defp(vm, env, proto, "maxPauseBufferTime", ns_noop, NULL, DE | DD);
    defp(vm, env, proto, "backBufferTime", ns_noop, NULL, DE | DD);
    if (avm1_ruffle_shapes()) {   /* Ruffle has these from the start */
        defp(vm, env, proto, "bufferLength", ns_buffer_length_m, NULL, 0);
        defp(vm, env, proto, "bufferTime", ns_buffer_time, NULL, 0);
        defp(vm, env, proto, "bytesLoaded", ns_bytes_loaded_m, NULL, 0);
        defp(vm, env, proto, "bytesTotal", ns_bytes_total_m, NULL, 0);
        defp(vm, env, proto, "time", ns_time, NULL, 0);
    }
    /* bufferLength, bufferTime, bytesLoaded, bytesTotal, time, currentFps and
       liveDelay come when a NetStream is made for a connected NetConnection */
    return c;
}

static void populate_proto(Act *a, AObj *proto) {
    static const char *const names[7] = { "currentFps", "bufferLength", "bufferTime", "liveDelay", "time", "bytesLoaded", "bytesTotal" };
    static const NativeFn getters[7] = { ns_zero, ns_buffer_length_m, ns_buffer_time, ns_zero, ns_time, ns_bytes_loaded_m, ns_bytes_total_m };
    VM *vm = a->vm; Env *env = ENV(a); int i;
    for (i = 0; i < 7; i++) {
        AStr *n = str_intern(vm, names[i]);
        if (o_has_own_property(a, proto, n)) continue;
        defp(vm, env, proto, names[i], getters[i], NULL, DE);
    }
}
