/* avm2_netconn.c - natives of flash.net.NetConnection and Responder (the
 * classes are Ruffle's ActionScript, see as3/), Ruffle's NetConnections:
 *
 *   connect(null)     a local connection (connected, protocol "rtmp")
 *   connect("http..") Flash Remoting: call() queues AMF0 messages that go
 *                     out as one POST per frame; the response packet's
 *                     "/N/onResult" and "/N/onStatus" messages go to the
 *                     responder of call N.
 *
 * Open connections live in a table (object -> state). */
#include "avm2_flash_int.h"
#include "fetch.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

extern void a2_report_uncaught(VM2 *vm);
extern A2Obj *a2_bytearray_new(VM2 *vm);
extern void a2_amf_write(VM2 *vm, A2Obj *b, V2 v, int enc);
extern V2 a2_amf_read(VM2 *vm, A2Obj *b, int enc);
extern int g_amf0_strict_arrays;
extern int g_fetch_log;
extern void avm1_trace_out(const char *s);

typedef struct { char *name; int must; u8 *val; size_t n; } NHdr;
typedef struct { char *target; u8 *val; size_t n; A2Obj *responder; int enc; } NMsg;
typedef struct { A2Obj *o; int kind; char *url; NHdr *h; int nh; NMsg *m; int nm; } NConn;   /* kind 1 local, 2 remoting */
static NConn *g_nc; static int g_nnc;

static char *dupz(const char *s) { size_t l = strlen(s); char *d = (char*)malloc(l + 1); if (d) memcpy(d, s, l + 1); return d; }
static NConn *nc_of(V2 self) { int i; if (self.t != T_OBJ) return NULL; for (i = 0; i < g_nnc; i++) if (g_nc[i].o == self.u.o) return &g_nc[i]; return NULL; }
static void nc_free_state(NConn *c) {
    int i;
    for (i = 0; i < c->nh; i++) { free(c->h[i].name); free(c->h[i].val); }
    for (i = 0; i < c->nm; i++) { free(c->m[i].target); free(c->m[i].val); }
    free(c->h); free(c->m); free(c->url);
}

/* NetStatusEvent with info built from key/value pairs (in that order) */
static void net_status(VM2 *vm, A2Obj *target, const char **kv) {
    A2Class *nse = a2_class_find_s(vm, "flash.events::NetStatusEvent"); A2Obj *info, *e;
    if (!nse) return;
    info = a2_new_plain(vm);
    for (; kv[0]; kv += 2) a2_dyn_set(vm, info, a2_str(vm, kv[0]), v2_cstr(vm, kv[1]));
    e = a2_make_event(vm, nse, a2_str(vm, "netStatus"), 0, 0);
    a2_dyn_set(vm, e, a2_str(vm, "info"), v2_obj(info));
    a2_dispatch_event_obj(vm, target, e);
    if (vm->throwing) a2_report_uncaught(vm);
}

static void nc_close(VM2 *vm, NConn *c, int explicit_) {
    A2Obj *o = c->o; int kind = c->kind; int i = (int)(c - g_nc);
    static const char *closed[] = { "code", "NetConnection.Connect.Closed", "level", "status", NULL };
    static const char *empty[] = { "code", "", "description", "", "details", "", "level", "status", NULL };
    nc_free_state(c);
    g_nc[i] = g_nc[--g_nnc];
    net_status(vm, o, closed);
    if (explicit_ && kind == 2) net_status(vm, o, empty);
}

static NConn *nc_open(VM2 *vm, A2Obj *o, int kind, const char *url) {
    NConn *old = nc_of(v2_obj(o)), *n;
    if (old) nc_close(vm, old, 0);
    n = (NConn*)realloc(g_nc, (size_t)(g_nnc + 1) * sizeof(NConn));
    if (!n) return NULL;
    g_nc = n; n = &g_nc[g_nnc++];
    memset(n, 0, sizeof(*n)); n->o = o; n->kind = kind; n->url = url ? dupz(url) : NULL;
    return n;
}

static V2 n_connect(VM2 *vm, V2 self, int argc, V2 *argv) {
    if (self.t != T_OBJ) return v2_undef();
    if (ARG(0).t == T_NULL || ARG(0).t == T_UNDEF) {
        static const char *ok[] = { "code", "NetConnection.Connect.Success", "level", "status", NULL };
        if (nc_open(vm, self.u.o, 1, NULL)) net_status(vm, self.u.o, ok);
    } else {
        A2Str *u = v2_tostr(vm, ARG(0)); const char *s;
        if (vm->throwing) return v2_undef();
        s = u->s;
        if (!strncmp(s, "http://", 7) || !strncmp(s, "https://", 8) || !strncmp(s, "HTTP://", 7) || !strncmp(s, "HTTPS://", 8))
            nc_open(vm, self.u.o, 2, s);
    }
    return v2_undef();
}
static V2 n_close(VM2 *vm, V2 self, int argc, V2 *argv) { NConn *c = nc_of(self); UNUSED; if (c) nc_close(vm, c, 1); return v2_undef(); }

static NConn *need_conn(VM2 *vm, V2 self, int local_only) {
    NConn *c = nc_of(self);
    if (!c || (local_only && c->kind != 1)) { a2_throw_id(vm, "ArgumentError", 2126, NULL, NULL); return NULL; }
    return c;
}
static V2 n_connected(VM2 *vm, V2 self, int argc, V2 *argv) { NConn *c = nc_of(self); UNUSED; return v2_bool(c && c->kind == 1); }
static V2 n_proxyType(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return need_conn(vm, self, 1) ? v2_cstr(vm, "none") : v2_undef(); }
static V2 n_id(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return need_conn(vm, self, 1) ? v2_cstr(vm, "") : v2_undef(); }
static V2 n_nonce(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return need_conn(vm, self, 1) ? v2_cstr(vm, "0000000000000000000000000000000000000000000000000000000000000000") : v2_undef(); }
static V2 n_protocol(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return need_conn(vm, self, 1) ? v2_cstr(vm, "rtmp") : v2_undef(); }
static V2 n_tls(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return need_conn(vm, self, 1) ? v2_bool(0) : v2_undef(); }
static V2 n_uri(VM2 *vm, V2 self, int argc, V2 *argv) { NConn *c = nc_of(self); UNUSED; if (!c) return v2_null(); return v2_cstr(vm, c->kind == 1 ? "null" : c->url); }

static u8 *amf0(VM2 *vm, V2 v, size_t *n) {
    A2Obj *b = a2_bytearray_new(vm); u8 *r = NULL;
    *n = 0;
    if (!b) return NULL;
    g_amf0_strict_arrays = 1;
    a2_amf_write(vm, b, v, 0);
    g_amf0_strict_arrays = 0;
    if (vm->throwing) { vm->throwing = 0; vm->exc = v2_undef(); }
    if (b->blen > 0 && (r = (u8*)malloc((size_t)b->blen)) != NULL) { memcpy(r, b->bytes, (size_t)b->blen); *n = (size_t)b->blen; }
    return r;
}

/* AMF3 call arguments: a strict array of AMF3 values, each behind the AVM+ marker */
extern void a2_bytearray_write_raw(VM2 *vm, A2Obj *b, const void *p, u32 n);
static u8 *amf3_args(VM2 *vm, int argc, V2 *argv, size_t *n) {
    A2Obj *b = a2_bytearray_new(vm); u8 *r = NULL; int i;
    *n = 0;
    if (!b) return NULL;
    { u8 h[5]; h[0] = 10; h[1] = (u8)(argc >> 24); h[2] = (u8)(argc >> 16); h[3] = (u8)(argc >> 8); h[4] = (u8)argc; a2_bytearray_write_raw(vm, b, h, 5); }
    for (i = 0; i < argc; i++) { u8 m = 17; a2_bytearray_write_raw(vm, b, &m, 1); a2_amf_write(vm, b, argv[i], 3); }
    if (vm->throwing) { vm->throwing = 0; vm->exc = v2_undef(); }
    if (b->blen > 0 && (r = (u8*)malloc((size_t)b->blen)) != NULL) { memcpy(r, b->bytes, (size_t)b->blen); *n = (size_t)b->blen; }
    return r;
}
static V2 n_call(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Str *cmd = v2_tostr(vm, ARG(0)); NConn *c; A2Obj *arr, *resp = NULL; int i; NMsg *nm;
    A2Class *rc = a2_class_find_s(vm, "flash.net::Responder");
    if (vm->throwing) return v2_undef();
    if (ARG(1).t == T_OBJ && rc && ARG(1).u.o->cls && a2_is_subclass(ARG(1).u.o->cls, rc)) resp = ARG(1).u.o;
    arr = a2_new_array(vm);
    for (i = 2; i < argc; i++) a2_arr_push(vm, arr, argv[i]);
    if (!(c = need_conn(vm, self, 0))) return v2_undef();
    if (c->kind != 2) return v2_undef();
    nm = (NMsg*)realloc(c->m, (size_t)(c->nm + 1) * sizeof(NMsg));
    if (!nm) return v2_undef();
    c->m = nm; nm = &c->m[c->nm++];
    nm->target = dupz(cmd->s); nm->responder = resp;
    { V2 e = a2_get_s(vm, self, "objectEncoding"); nm->enc = (vm->throwing ? 0 : v2_toint(vm, e)) == 3 ? 3 : 0; vm->throwing = 0; }
    if (nm->enc == 3) nm->val = amf3_args(vm, argc > 2 ? argc - 2 : 0, argv + 2, &nm->n);
    else nm->val = amf0(vm, v2_obj(arr), &nm->n);
    return v2_undef();
}

static int ieq(const char *a, const char *b) { for (; *a && *b; a++, b++) { int x = *a, y = *b; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) return 0; } return !*a && !*b; }
static V2 n_addHeader(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Str *name = v2_tostr(vm, ARG(0)); int must = v2_tobool(vm, ARG(1)), i; NConn *c; NHdr *nh; u8 *val; size_t n;
    if (vm->throwing) return v2_undef();
    val = amf0(vm, argc > 2 ? argv[2] : v2_null(), &n);
    c = nc_of(self);
    if (!c || c->kind != 2) { free(val); return v2_undef(); }
    for (i = 0; i < c->nh; i++) if (ieq(c->h[i].name, name->s)) { free(c->h[i].name); free(c->h[i].val); memmove(&c->h[i], &c->h[i + 1], (size_t)(c->nh - i - 1) * sizeof(NHdr)); c->nh--; break; }
    nh = (NHdr*)realloc(c->h, (size_t)(c->nh + 1) * sizeof(NHdr));
    if (!nh) { free(val); return v2_undef(); }
    c->h = nh; c->h[c->nh].name = dupz(name->s); c->h[c->nh].must = must; c->h[c->nh].val = val; c->h[c->nh].n = n; c->nh++;
    return v2_undef();
}

/* Responder */
static V2 r_init(VM2 *vm, V2 self, int argc, V2 *argv) {
    if (self.t != T_OBJ) return v2_undef();
    a2_dyn_set_hidden(vm, self.u.o, a2_str(vm, "__r_result"), ARG(0));
    a2_dyn_set_hidden(vm, self.u.o, a2_str(vm, "__r_status"), ARG(1));
    return v2_undef();
}
static void responder_call(VM2 *vm, A2Obj *r, int status, V2 v) {
    A2Prop *p = a2_dyn_find(r, a2_str(vm, status ? "__r_status" : "__r_result"));
    if (!p || p->v.t != T_OBJ || p->v.u.o->kind != OK_FUNCTION) return;
    a2_call(vm, p->v, v2_obj(r), 1, &v);
    if (vm->throwing) a2_report_uncaught(vm);
}

/* packet buffer */
typedef struct { u8 *d; size_t n, cap; } PB;
static void pb_put(PB *b, const void *p, size_t n) { if (b->n + n > b->cap) { size_t nc = b->cap ? b->cap * 2 : 256; u8 *nd; while (nc < b->n + n) nc *= 2; nd = (u8*)realloc(b->d, nc); if (!nd) return; b->d = nd; b->cap = nc; } memcpy(b->d + b->n, p, n); b->n += n; }
static void pb_u8(PB *b, unsigned v) { u8 t = (u8)v; pb_put(b, &t, 1); }
static void pb_u16(PB *b, unsigned v) { pb_u8(b, v >> 8); pb_u8(b, v); }
static void pb_u32(PB *b, u32 v) { pb_u16(b, v >> 16); pb_u16(b, v & 0xFFFF); }
static void pb_str(PB *b, const char *s) { size_t l = strlen(s); pb_u16(b, (unsigned)l); pb_put(b, s, l); }

static void log_fetch(const char *url, const u8 *body, size_t n) {
    char *t, *o; size_t i;
    if (!g_fetch_log) return;
    avm1_trace_out("Navigator::fetch:");
    t = (char*)malloc(strlen(url) + 16); if (t) { sprintf(t, "  URL: %s", url); avm1_trace_out(t); free(t); }
    avm1_trace_out("  Method: POST");
    avm1_trace_out("  Mime-Type: application/x-amf");
    t = (char*)malloc(n * 4 + 16);
    if (t) { o = t; o += sprintf(o, "  Body: ["); for (i = 0; i < n; i++) o += sprintf(o, i ? ", %02X" : "%02X", body[i]); strcpy(o, "]"); avm1_trace_out(t); free(t); }
}

static void response(VM2 *vm, const u8 *d, size_t n, NMsg *msgs, int nmsg) {
    A2Obj *b = a2_bytearray_new(vm); u32 nh, nm, i;
    if (!b || n < 6) return;
    b->bytes = (u8*)malloc(n); if (!b->bytes) return;
    memcpy(b->bytes, d, n); b->blen = b->bcap = (int)n; b->bpos = 2;
#define NEED(k) if ((size_t)b->bpos + (k) > n) return
#define U16() (b->bpos += 2, ((u32)b->bytes[b->bpos - 2] << 8) | b->bytes[b->bpos - 1])
    NEED(2); nh = U16();
    for (i = 0; i < nh; i++) { u32 l; NEED(2); l = U16(); NEED(l + 5); b->bpos += (int)l + 5; a2_amf_read(vm, b, 0); if (vm->throwing) { vm->throwing = 0; vm->exc = v2_undef(); return; } }
    NEED(2); nm = U16();
    for (i = 0; i < nm; i++) {
        u32 l; char t[300]; V2 v; int idx = 0, status = -1; const char *q;
        NEED(2); l = U16(); NEED(l);
        { u32 c = l < sizeof(t) - 1 ? l : (u32)sizeof(t) - 1; memcpy(t, b->bytes + b->bpos, c); t[c] = 0; }
        b->bpos += (int)l;
        NEED(2); l = U16(); NEED(l + 4); b->bpos += (int)l + 4;
        v = a2_amf_read(vm, b, 0);
        if (vm->throwing) { vm->throwing = 0; vm->exc = v2_undef(); return; }
        if (t[0] != '/') continue;
        q = t + 1; while (*q >= '0' && *q <= '9') idx = idx * 10 + (*q++ - '0');
        if (q == t + 1) continue;
        if (!strcmp(q, "/onResult")) status = 0; else if (!strcmp(q, "/onStatus")) status = 1; else continue;
        if (idx >= 1 && idx <= nmsg && msgs[idx - 1].responder) responder_call(vm, msgs[idx - 1].responder, status, v);
    }
#undef NEED
#undef U16
}

/* NetConnections::update_connections: send the queued calls, one packet per connection */
void a2_netconn_frame(VM2 *vm) {
    int k;
    for (k = 0; k < g_nnc; k++) {
        NConn *c = &g_nc[k]; PB b; int i, nmsg; NMsg *msgs; char path[1100], *url; u8 *resp = NULL; size_t rn = 0; A2Obj *o = c->o;
        if (c->kind != 2 || !c->nm) continue;
        memset(&b, 0, sizeof(b));
        { int v3 = 0; for (i = 0; i < c->nm; i++) if (c->m[i].enc == 3) v3 = 1; pb_u16(&b, v3 ? 3 : 0); } pb_u16(&b, (unsigned)c->nh);
        for (i = 0; i < c->nh; i++) { pb_str(&b, c->h[i].name); pb_u8(&b, c->h[i].must ? 1 : 0); pb_u32(&b, (u32)c->h[i].n); pb_put(&b, c->h[i].val, c->h[i].n); }
        pb_u16(&b, (unsigned)c->nm);
        for (i = 0; i < c->nm; i++) { char r[16]; sprintf(r, "/%d", i + 1); pb_str(&b, c->m[i].target); pb_str(&b, r); pb_u32(&b, (u32)c->m[i].n); pb_put(&b, c->m[i].val, c->m[i].n); }
        log_fetch(c->url, b.d, b.n);
        free(b.d);
        msgs = c->m; nmsg = c->nm; c->m = NULL; c->nm = 0;
        url = dupz(c->url);
        if (url && fetch_url_to_path(vm->player, url, path, sizeof(path))) resp = fetch_read_file(path, &rn);
        if (!resp) {
            const char *kv[] = { "code", "NetConnection.Call.Failed", "level", "error", "details", url ? url : "", "description", "HTTP: Failed", NULL };
            net_status(vm, o, kv);
        } else response(vm, resp, rn, msgs, nmsg);
        free(resp); free(url);
        for (i = 0; i < nmsg; i++) { free(msgs[i].target); free(msgs[i].val); }
        free(msgs);
    }
}
void a2_netconn_mark(VM2 *vm) {
    int i, j;
    for (i = 0; i < g_nnc; i++) { a2_mark_obj(vm, g_nc[i].o); for (j = 0; j < g_nc[i].nm; j++) if (g_nc[i].m[j].responder) a2_mark_obj(vm, g_nc[i].m[j].responder); }
}

void a2_netconn_natives_init(void) {
#define NC "flash.net::NetConnection/"
    a2_register_native(NC "connect", n_connect);
    a2_register_native(NC "close", n_close);
    a2_register_native(NC "call", n_call);
    a2_register_native(NC "addHeader", n_addHeader);
    a2_register_native(NC "get connected", n_connected);
    a2_register_native(NC "get connectedProxyType", n_proxyType);
    a2_register_native(NC "get farID", n_id);
    a2_register_native(NC "get nearID", n_id);
    a2_register_native(NC "get farNonce", n_nonce);
    a2_register_native(NC "get nearNonce", n_nonce);
    a2_register_native(NC "get protocol", n_protocol);
    a2_register_native(NC "get uri", n_uri);
    a2_register_native(NC "get usingTLS", n_tls);
    a2_register_native("flash.net::Responder/init", r_init);
}
