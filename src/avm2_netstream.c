/* avm2_netstream.c - natives of flash.net.NetStream (the class is Ruffle's
 * ActionScript, see as3/) on the shared stream core (netstream.c).  Status
 * changes are NetStatusEvents on the stream; FLV script data calls the
 * method of the same name on `client` (the stream itself by default). */
#include "avm2_flash_int.h"
#include "netstream.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

extern void a2_report_uncaught(VM2 *vm);
#define OK_NETSTREAM_PAYLOAD 0x4e53   /* marks the payload as a stream */

typedef struct { int magic; NetStream *ns; A2Obj *self; VM2 *vm; } NSPay;

static NSPay *pay(V2 v) {
    A2Obj *o = v.t == T_OBJ ? v.u.o : NULL;
    if (!o || o->kind != OK_NETSTREAM || !o->payload) return NULL;
    return ((NSPay*)o->payload)->magic == OK_NETSTREAM_PAYLOAD ? (NSPay*)o->payload : NULL;
}
void a2_netstream_free_payload(A2Obj *o) { NSPay *p = (NSPay*)o->payload; if (p && p->magic == OK_NETSTREAM_PAYLOAD) { ns_free(p->ns); free(p); } o->payload = NULL; }
NetStream *a2_netstream_of(A2Obj *o) { NSPay *p = pay(v2_obj(o)); return p ? p->ns : NULL; }

static void cb_status(void *ud, const char *code, const char *level, const char *desc) {
    NSPay *p = (NSPay*)ud; VM2 *vm = p->vm; A2Class *nse = a2_class_find_s(vm, "flash.events::NetStatusEvent"); A2Obj *e, *info;
    if (!nse) return;
    info = a2_new_plain(vm);
    if (desc) { a2_dyn_set(vm, info, a2_str(vm, "description"), v2_cstr(vm, desc)); a2_dyn_set(vm, info, a2_str(vm, "level"), v2_cstr(vm, level)); a2_dyn_set(vm, info, a2_str(vm, "code"), v2_cstr(vm, code)); }
    else { a2_dyn_set(vm, info, a2_str(vm, "code"), v2_cstr(vm, code)); a2_dyn_set(vm, info, a2_str(vm, "level"), v2_cstr(vm, level)); }
    e = a2_make_event(vm, nse, a2_str(vm, "netStatus"), 0, 0);
    a2_dyn_set(vm, e, a2_str(vm, "info"), v2_obj(info));
    a2_dispatch_event_obj(vm, p->self, e);
    if (vm->throwing) a2_report_uncaught(vm);
}

/* FLV script values (AMF0): objects and ECMA arrays become Objects */
static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static double be64(const u8 *p) { union { double d; u8 b[8]; } u; int i; u16 t = 1; if (*(u8*)&t) for (i = 0; i < 8; i++) u.b[i] = p[7 - i]; else memcpy(u.b, p, 8); return u.d; }
static V2 flv_value(VM2 *vm, const u8 **pp, const u8 *end, int depth);
static void flv_props(VM2 *vm, A2Obj *o, const u8 **pp, const u8 *end, int depth) {
    const u8 *p = *pp;
    while (p + 2 <= end) {
        u32 n = ((u32)p[0] << 8) | p[1]; A2Str *k; V2 v;
        p += 2;
        if (n == 0 && p < end && *p == 9) { p++; break; }   /* object end */
        if (p + n > end) { p = end; break; }
        k = a2_intern(vm, (const char*)p, n); p += n;
        v = flv_value(vm, &p, end, depth + 1);
        a2_dyn_set(vm, o, k, v);
    }
    *pp = p;
}
static V2 flv_value(VM2 *vm, const u8 **pp, const u8 *end, int depth) {
    const u8 *p = *pp; int t; V2 r = v2_undef();
    if (p >= end || depth > 64) { *pp = end; return v2_undef(); }
    t = *p++;
    switch (t) {
    case 0: if (p + 8 <= end) { r = v2_num(be64(p)); p += 8; } else p = end; break;
    case 1: if (p < end) r = v2_bool(*p++ != 0); break;
    case 2: if (p + 2 <= end) { u32 n = ((u32)p[0] << 8) | p[1]; p += 2; if (p + n > end) n = (u32)(end - p); r = v2_str(a2_intern(vm, (const char*)p, n)); p += n; } break;
    case 12: if (p + 4 <= end) { u32 n = be32(p); p += 4; if ((size_t)(end - p) < n) n = (u32)(end - p); r = v2_str(a2_intern(vm, (const char*)p, n)); p += n; } break;
    case 3: { A2Obj *o = a2_new_plain(vm); flv_props(vm, o, &p, end, depth); r = v2_obj(o); break; }
    case 8: { A2Obj *o = a2_new_plain(vm); if (p + 4 <= end) p += 4; flv_props(vm, o, &p, end, depth); r = v2_obj(o); break; }
    case 10: { A2Obj *a = a2_new_array(vm); u32 n = p + 4 <= end ? be32(p) : 0, i; p += 4; for (i = 0; i < n && p < end; i++) a2_arr_push(vm, a, flv_value(vm, &p, end, depth + 1)); r = v2_obj(a); break; }
    case 11: if (p + 10 <= end) { V2 a[1]; a[0] = v2_num(be64(p)); p += 10; r = a2_construct_class(vm, vm->c_date, 1, a); } else p = end; break;
    case 5: r = v2_null(); break;
    case 6: r = v2_undef(); break;
    default: p = end; break;
    }
    *pp = p;
    return r;
}
static const u8 *cb_script(void *ud, const char *name, const u8 *p, const u8 *end) {
    NSPay *pl = (NSPay*)ud; VM2 *vm = pl->vm; V2 v, client, a[1];
    v = flv_value(vm, &p, end, 0);
    client = a2_get_s(vm, v2_obj(pl->self), "client");
    if (vm->throwing) { a2_report_uncaught(vm); return p; }
    if (client.t == T_OBJ) {
        a[0] = v;
        a2_call_s(vm, client, name, 1, a);
        if (vm->throwing) a2_report_uncaught(vm);
    }
    return p;
}

static NSPay *ensure(VM2 *vm, V2 self) {
    A2Obj *o = self.t == T_OBJ ? self.u.o : NULL; NSPay *p; NSCallbacks cb;
    if (!o) return NULL;
    if ((p = pay(self)) != NULL) return p;
    if (o->payload) return NULL;
    p = (NSPay*)calloc(1, sizeof(NSPay));
    if (!p) return NULL;
    memset(&cb, 0, sizeof(cb)); cb.status = cb_status; cb.script = cb_script;
    p->magic = OK_NETSTREAM_PAYLOAD; p->self = o; p->vm = vm;
    p->ns = ns_new(vm->player, 1, &cb, p);
    if (!p->ns) { free(p); return NULL; }
    o->kind = OK_NETSTREAM; o->payload = p;
    return p;
}

NetStream *a2_netstream_get(VM2 *vm, A2Obj *o) { NSPay *p = ensure(vm, v2_obj(o)); return p ? p->ns : NULL; }
static V2 n_init(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; ensure(vm, self); return v2_undef(); }
static V2 n_play(VM2 *vm, V2 self, int argc, V2 *argv) {
    NSPay *p = ensure(vm, self);
    if (!p) return v2_undef();
    if (argc > 0 && argv[0].t != T_NULL && argv[0].t != T_UNDEF) { A2Str *s = v2_tostr(vm, argv[0]); if (vm->throwing) return v2_undef(); ns_play(p->ns, s->s); }
    else ns_play(p->ns, NULL);
    return v2_undef();
}
static V2 n_pause(VM2 *vm, V2 self, int argc, V2 *argv) { NSPay *p = ensure(vm, self); UNUSED; if (p) ns_pause(p->ns, 1); return v2_undef(); }
static V2 n_resume(VM2 *vm, V2 self, int argc, V2 *argv) { NSPay *p = ensure(vm, self); UNUSED; if (p) ns_resume(p->ns); return v2_undef(); }
static V2 n_togglePause(VM2 *vm, V2 self, int argc, V2 *argv) { NSPay *p = ensure(vm, self); UNUSED; if (p) ns_toggle(p->ns); return v2_undef(); }
static V2 n_seek(VM2 *vm, V2 self, int argc, V2 *argv) { NSPay *p = ensure(vm, self); double s = NUM(0); if (p && !vm->throwing) ns_seek(p->ns, s * 1000.0, 1); return v2_undef(); }
static V2 n_bytesLoaded(VM2 *vm, V2 self, int argc, V2 *argv) { NSPay *p = ensure(vm, self); UNUSED; return v2_uint(p ? (u32)ns_bytes_loaded(p->ns) : 0); }
static V2 n_bytesTotal(VM2 *vm, V2 self, int argc, V2 *argv) { NSPay *p = ensure(vm, self); UNUSED; return v2_uint(p ? (u32)ns_bytes_total(p->ns) : 0); }
static V2 n_time(VM2 *vm, V2 self, int argc, V2 *argv) { NSPay *p = ensure(vm, self); UNUSED; return v2_num(p ? p->ns->stream_time / 1000.0 : 0); }
static V2 n_client_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Prop *c; UNUSED; c = self.t == T_OBJ ? a2_dyn_find(self.u.o, a2_str(vm, "__client")) : NULL; return c ? c->v : self; }
static V2 n_client_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    if (ARG(0).t != T_OBJ) { a2_throw_id(vm, "TypeError", 2004, NULL, NULL); return v2_undef(); }
    if (self.t == T_OBJ) a2_dyn_set_hidden(vm, self.u.o, a2_str(vm, "__client"), ARG(0));
    return v2_undef();
}

static void mark_cb(void *ctx, void *ud) { a2_mark_obj((VM2*)ctx, ((NSPay*)ud)->self); }
void a2_netstream_mark(VM2 *vm) { ns_mark_active(vm->player, 1, mark_cb, vm); }

void a2_netstream_natives_init(void) {
#define NS "flash.net::NetStream/"
    a2_register_native(NS "play", n_play);
    a2_register_native(NS "pause", n_pause);
    a2_register_native(NS "resume", n_resume);
    a2_register_native(NS "togglePause", n_togglePause);
    a2_register_native(NS "seek", n_seek);
    a2_register_native(NS "get bytesLoaded", n_bytesLoaded);
    a2_register_native(NS "get bytesTotal", n_bytesTotal);
    a2_register_native(NS "get time", n_time);
    a2_register_native(NS "get client", n_client_get);
    a2_register_native(NS "set client", n_client_set);
    (void)n_init;
}
