/* avm2_socket.c - natives of flash.net.Socket and XMLSocket (the classes
 * are Ruffle's ActionScript, see as3/) on the socket backend (sock.c).
 *
 * Each Socket keeps two hidden ByteArrays: the read buffer (data received
 * and not read yet) and the write buffer (written, not flushed yet).  The
 * read and write methods are the ByteArray methods on those buffers; the
 * endian and objectEncoding of the socket live on both of them.  Open
 * connections are kept in a table (handle -> object) that the per-frame
 * update uses to deliver connect / ioError / socketData / close events. */
#include "avm2_flash_int.h"
#include "sock.h"
#include <stdlib.h>
#include <string.h>

extern void a2_report_uncaught(VM2 *vm);

typedef struct { A2Obj *o; int handle, connected; } SkConn;
static SkConn *g_sk; static int g_nsk, g_capsk;

static SkConn *sk_by_handle(int h) { int i; for (i = 0; i < g_nsk; i++) if (g_sk[i].handle == h) return &g_sk[i]; return NULL; }
static SkConn *sk_by_obj(A2Obj *o) { int i; for (i = 0; i < g_nsk; i++) if (g_sk[i].o == o) return &g_sk[i]; return NULL; }
static void sk_remove(SkConn *c) { int i = (int)(c - g_sk); g_sk[i] = g_sk[--g_nsk]; }

static A2Obj *buf(VM2 *vm, A2Obj *o, const char *key) {
    A2Str *k = a2_str(vm, key); A2Prop *p = a2_dyn_find(o, k); V2 b;
    if (p && p->v.t == T_OBJ && p->v.u.o->kind == OK_BYTEARRAY) return p->v.u.o;
    b = a2_construct_class(vm, vm->c_bytearray, 0, NULL);
    if (b.t != T_OBJ) return NULL;
    /* a fresh socket: big endian, AMF3 (the read buffer is the reference) */
    { A2Prop *r = strcmp(key, "__sk_w") == 0 ? a2_dyn_find(o, a2_str(vm, "__sk_r")) : NULL;
      if (r && r->v.t == T_OBJ) { b.u.o->ival = r->v.u.o->ival; b.u.o->dval = r->v.u.o->dval; } }
    a2_dyn_set_hidden(vm, o, k, b);
    return b.u.o;
}
static A2Obj *rbuf(VM2 *vm, A2Obj *o) { return buf(vm, o, "__sk_r"); }
static A2Obj *wbuf(VM2 *vm, A2Obj *o) { buf(vm, o, "__sk_r"); return buf(vm, o, "__sk_w"); }
static void clear_buf(A2Obj *b) { if (b) { free(b->bytes); b->bytes = NULL; b->blen = b->bcap = b->bpos = 0; } }

/* the socket of `self` when it is open; Error #2002 otherwise */
static SkConn *open_conn(VM2 *vm, V2 self) {
    SkConn *c = self.t == T_OBJ ? sk_by_obj(self.u.o) : NULL;
    if (!c || !c->connected) { a2_throw_id(vm, "flash.errors::IOError", 2002, NULL, NULL); return NULL; }
    return c;
}

static void sk_close(VM2 *vm, SkConn *c) {
    A2Obj *o = c->o;
    sock_close(vm->player, c->handle);
    sk_remove(c);
    clear_buf(rbuf(vm, o)); clear_buf(wbuf(vm, o));
}

static V2 s_connect(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Str *host; u32 port; A2Prop *tp; int h, timeout = 20000; SkConn *old, *c;
    if (self.t != T_OBJ) return v2_undef();
    host = ARG(0).t == T_NULL || ARG(0).t == T_UNDEF ? a2_str(vm, "null") : v2_tostr(vm, ARG(0));
    port = v2_touint(vm, ARG(1));
    if (vm->throwing) return v2_undef();
    if (port > 65535) { a2_throw_id(vm, "SecurityError", 2003, NULL, NULL); return v2_undef(); }
    tp = a2_dyn_find(self.u.o, a2_str(vm, "__sk_t"));
    if (tp) timeout = (int)v2_touint(vm, tp->v);
    rbuf(vm, self.u.o); wbuf(vm, self.u.o);
    h = sock_connect(vm->player, host->s, (int)port, timeout);
    if (!h) return v2_undef();
    old = sk_by_obj(self.u.o);
    if (old) sk_close(vm, old);      /* a new connection replaces the open one */
    if (g_nsk == g_capsk) { int nc = g_capsk ? g_capsk * 2 : 8; SkConn *n = (SkConn*)realloc(g_sk, (size_t)nc * sizeof(SkConn)); if (!n) return v2_undef(); g_sk = n; g_capsk = nc; }
    c = &g_sk[g_nsk++]; c->o = self.u.o; c->handle = h; c->connected = 0;
    return v2_undef();
}
static V2 s_timeout_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Prop *p; UNUSED; p = self.t == T_OBJ ? a2_dyn_find(self.u.o, a2_str(vm, "__sk_t")) : NULL; return p ? p->v : v2_uint(0); }
static V2 s_timeout_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    u32 t = v2_touint(vm, ARG(0));
    if (t < 250) t = 250;
    if (self.t == T_OBJ && !vm->throwing) a2_dyn_set_hidden(vm, self.u.o, a2_str(vm, "__sk_t"), v2_uint(t));
    return v2_undef();
}
static V2 s_close(VM2 *vm, V2 self, int argc, V2 *argv) { SkConn *c = open_conn(vm, self); UNUSED; if (c) sk_close(vm, c); return v2_undef(); }
static V2 s_bytesAvailable(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *b; UNUSED;
    if (self.t != T_OBJ) return v2_uint(0);
    b = rbuf(vm, self.u.o);
    return v2_uint(b && b->blen > b->bpos ? (u32)(b->blen - b->bpos) : 0);
}
static V2 s_endian_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *b; UNUSED; b = self.t == T_OBJ ? rbuf(vm, self.u.o) : NULL; return v2_cstr(vm, b && b->ival ? "littleEndian" : "bigEndian"); }
static V2 s_endian_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Str *s = v2_tostr(vm, ARG(0)); int little;
    if (vm->throwing || self.t != T_OBJ) return v2_undef();
    if (!strcmp(s->s, "bigEndian")) little = 0;
    else if (!strcmp(s->s, "littleEndian")) little = 1;
    else { a2_throw_id(vm, "ArgumentError", 2008, "endian", NULL); return v2_undef(); }
    rbuf(vm, self.u.o)->ival = little; wbuf(vm, self.u.o)->ival = little;
    return v2_undef();
}
static V2 s_connected(VM2 *vm, V2 self, int argc, V2 *argv) { SkConn *c; UNUSED; c = self.t == T_OBJ ? sk_by_obj(self.u.o) : NULL; return v2_bool(c && c->connected); }
static V2 s_enc_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *b; UNUSED; b = self.t == T_OBJ ? rbuf(vm, self.u.o) : NULL; return v2_uint(b ? (u32)b->dval : 3); }
static V2 s_enc_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    u32 e = v2_touint(vm, ARG(0));
    if (vm->throwing || self.t != T_OBJ) return v2_undef();
    if (e != 0 && e != 3) { a2_throw_id(vm, "ArgumentError", 2008, "objectEncoding", NULL); return v2_undef(); }
    rbuf(vm, self.u.o)->dval = e; wbuf(vm, self.u.o)->dval = e;
    return v2_undef();
}
static V2 s_flush(VM2 *vm, V2 self, int argc, V2 *argv) {
    SkConn *c = open_conn(vm, self); A2Obj *w; UNUSED;
    if (!c) return v2_undef();
    w = wbuf(vm, c->o);
    if (w && w->blen > 0) sock_send(vm->player, c->handle, w->bytes, (size_t)w->blen);
    clear_buf(w);
    return v2_undef();
}

/* reads: the ByteArray method on the read buffer, then the bytes read are
   dropped (the buffer holds only what is still available) */
static V2 s_read(VM2 *vm, V2 self, int argc, V2 *argv) {
    SkConn *c = open_conn(vm, self); A2Obj *r; V2 v;
    if (!c) return v2_undef();
    r = rbuf(vm, c->o);
    v = a2_call_s(vm, v2_obj(r), (const char*)vm->native_data, argc, argv);
    if (r->bpos > 0 && r->bpos <= r->blen) { memmove(r->bytes, r->bytes + r->bpos, (size_t)(r->blen - r->bpos)); r->blen -= r->bpos; r->bpos = 0; }
    return v;
}
static V2 s_write(VM2 *vm, V2 self, int argc, V2 *argv) {
    SkConn *c = open_conn(vm, self); A2Obj *w;
    if (!c) return v2_undef();
    w = wbuf(vm, c->o);
    w->bpos = w->blen;
    a2_call_s(vm, v2_obj(w), (const char*)vm->native_data, argc, argv);
    return v2_undef();
}

static V2 x_domain(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_cstr(vm, "localhost"); }

/* Sockets::update_sockets: deliver the backend events to the scripts */
static void dispatch(VM2 *vm, A2Obj *o, const char *cls, int argc, V2 *argv) {
    A2Class *c = a2_class_find_s(vm, cls); V2 e;
    if (!c) return;
    e = a2_construct_class(vm, c, argc, argv);
    if (vm->throwing) { a2_report_uncaught(vm); return; }
    if (e.t == T_OBJ) a2_dispatch_event_obj(vm, o, e.u.o);
    if (vm->throwing) a2_report_uncaught(vm);
}
void a2_sockets_update(VM2 *vm) {
    Player *p = vm->player; SockEvent ev;
    while (sock_next_event(p, &ev)) {
        SkConn *c = sk_by_handle(ev.handle); A2Obj *o = c ? c->o : NULL; V2 a[5];
        if (!c) { free(ev.data); continue; }
        if (ev.type == SOCK_EV_CONNECT) {
            if (ev.ok) {
                c->connected = 1;
                a[0] = v2_cstr(vm, "connect");
                dispatch(vm, o, "flash.events::Event", 1, a);
            } else {
                a[0] = v2_cstr(vm, "ioError"); a[1] = v2_bool(0); a[2] = v2_bool(0);
                a[3] = v2_cstr(vm, "Error #2031: Socket Error."); a[4] = v2_int(2031);
                dispatch(vm, o, "flash.events::IOErrorEvent", 5, a);
            }
        } else if (ev.type == SOCK_EV_DATA) {
            A2Obj *r = rbuf(vm, o); int pos;
            if (r && ev.n) {
                pos = r->bpos; r->bpos = r->blen;
                /* append at the end, keeping the read position */
                { u8 *nb = (u8*)realloc(r->bytes, (size_t)r->blen + ev.n);
                  if (nb) { r->bytes = nb; memcpy(nb + r->blen, ev.data, ev.n); r->blen += (int)ev.n; if (r->bcap < r->blen) r->bcap = r->blen; } }
                r->bpos = pos;
            }
            a[0] = v2_cstr(vm, "socketData"); a[1] = v2_bool(0); a[2] = v2_bool(0);
            a[3] = v2_num((double)ev.n); a[4] = v2_num(0);
            dispatch(vm, o, "flash.events::ProgressEvent", 5, a);
        } else {
            c->connected = 0;
            sock_close(p, c->handle);
            sk_remove(c);
            clear_buf(rbuf(vm, o)); clear_buf(wbuf(vm, o));
            a[0] = v2_cstr(vm, "close");
            dispatch(vm, o, "flash.events::Event", 1, a);
        }
        free(ev.data);
    }
    sock_pump(p);
}
void a2_sockets_mark(VM2 *vm) { int i; for (i = 0; i < g_nsk; i++) a2_mark_obj(vm, g_sk[i].o); }
void a2_sockets_free(void) { free(g_sk); g_sk = NULL; g_nsk = g_capsk = 0; }

void a2_socket_natives_init(void) {
    static const char *rd[] = { "readBoolean", "readByte", "readBytes", "readDouble", "readFloat", "readInt", "readMultiByte", "readObject", "readShort", "readUnsignedByte", "readUnsignedInt", "readUnsignedShort", "readUTF", "readUTFBytes", NULL };
    static const char *wr[] = { "writeBoolean", "writeByte", "writeBytes", "writeDouble", "writeFloat", "writeInt", "writeMultiByte", "writeObject", "writeShort", "writeUnsignedInt", "writeUTF", "writeUTFBytes", NULL };
    static char keys[32][64]; int i, k = 0;
#define SK "flash.net::Socket/"
    a2_register_native(SK "connect", s_connect);
    a2_register_native(SK "get timeout", s_timeout_get);
    a2_register_native(SK "set timeout", s_timeout_set);
    a2_register_native(SK "close", s_close);
    a2_register_native(SK "get bytesAvailable", s_bytesAvailable);
    a2_register_native(SK "get endian", s_endian_get);
    a2_register_native(SK "set endian", s_endian_set);
    a2_register_native(SK "get connected", s_connected);
    a2_register_native(SK "get objectEncoding", s_enc_get);
    a2_register_native(SK "set objectEncoding", s_enc_set);
    a2_register_native(SK "flush", s_flush);
    for (i = 0; rd[i]; i++, k++) { strcpy(keys[k], SK); strcat(keys[k], rd[i]); a2_register_native_data(keys[k], s_read, (void*)rd[i]); }
    for (i = 0; wr[i]; i++, k++) { strcpy(keys[k], SK); strcat(keys[k], wr[i]); a2_register_native_data(keys[k], s_write, (void*)wr[i]); }
    a2_register_native("flash.net::XMLSocket/get domain", x_domain);
}
