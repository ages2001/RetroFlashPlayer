/* avm2_localconn.c - natives of flash.net.LocalConnection (the class is
 * Ruffle's ActionScript, see as3/) on the registry shared with AVM1
 * (lc_hub.c).  The connection name of an object is a hidden property;
 * the client (the object itself by default) receives the method calls. */
#include "avm2_flash_int.h"
#include "lc_hub.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

extern void a2_report_uncaught(VM2 *vm);
extern const char *avm1_movie_url(Player *p, SWFMovie *m);
extern A2Obj *a2_bytearray_new(VM2 *vm);
extern void a2_amf_write(VM2 *vm, A2Obj *b, V2 v, int enc);
extern V2 a2_amf_read(VM2 *vm, A2Obj *b, int enc);

static VM2 *g_vm;

static void superdomain(VM2 *vm, char *out, size_t n) { lch_superdomain(avm1_movie_url(vm->player, vm->player->movie), out, n); }

static void status_cb(void *ctx, void *src, const char *level) {
    VM2 *vm = (VM2*)ctx; A2Class *c = a2_class_find_s(vm, "flash.events::StatusEvent"); V2 a[5], e;
    if (!c) return;
    a[0] = v2_cstr(vm, "status"); a[1] = v2_bool(0); a[2] = v2_bool(0); a[3] = v2_null(); a[4] = v2_cstr(vm, level);
    e = a2_construct_class(vm, c, 5, a);
    if (!vm->throwing && e.t == T_OBJ) a2_dispatch_event_obj(vm, (A2Obj*)src, e.u.o);
    if (vm->throwing) a2_report_uncaught(vm);
}
static void call_cb(void *ctx, void *obj, const char *method, int nargs, u8 **args, size_t *argn) {
    VM2 *vm = (VM2*)ctx; A2Obj *o = (A2Obj*)obj; V2 *av, client; A2Prop *cp; int i;
    av = (V2*)calloc((size_t)nargs + 1, sizeof(V2));
    if (!av) return;
    for (i = 0; i < nargs; i++) {
        A2Obj *b = a2_bytearray_new(vm);
        av[i] = v2_undef();
        if (!args[i] || !b) continue;
        b->bytes = (u8*)malloc(argn[i] ? argn[i] : 1);
        if (!b->bytes) continue;
        memcpy(b->bytes, args[i], argn[i]); b->blen = b->bcap = (int)argn[i]; b->bpos = 0;
        av[i] = a2_amf_read(vm, b, 0);
        if (vm->throwing) { vm->throwing = 0; vm->exc = v2_undef(); av[i] = v2_undef(); }
    }
    cp = a2_dyn_find(o, a2_str(vm, "__lc_client"));
    client = cp ? cp->v : v2_obj(o);
    a2_call_s(vm, client, method, nargs, av);
    free(av);
    if (vm->throwing) {
        A2Class *c = a2_class_find_s(vm, "flash.events::AsyncErrorEvent"); V2 err = vm->exc, a[5], e; char text[400];
        vm->throwing = 0; vm->exc = v2_undef();
        if (!c) return;
        sprintf(text, "Error #2095: flash.net.LocalConnection was unable to invoke callback %.300s.", method);
        a[0] = v2_cstr(vm, "asyncError"); a[1] = v2_bool(0); a[2] = v2_bool(0); a[3] = v2_cstr(vm, text); a[4] = err;
        e = a2_construct_class(vm, c, 5, a);
        if (!vm->throwing && e.t == T_OBJ) a2_dispatch_event_obj(vm, o, e.u.o);
        if (vm->throwing) a2_report_uncaught(vm);
    }
}
static void hook(VM2 *vm) {
    if (g_vm != vm) { LCHandler h; h.ctx = vm; h.status = status_cb; h.call = call_cb; lch_set_handler(1, &h); g_vm = vm; }
}

static A2Str *nonnull(VM2 *vm, V2 v, const char *name) {
    if (v.t == T_NULL || v.t == T_UNDEF) { a2_throw_id(vm, "TypeError", 2007, name, NULL); return NULL; }
    return v2_tostr(vm, v);
}
static const char *key_of(VM2 *vm, A2Obj *o) { A2Prop *p = a2_dyn_find(o, a2_str(vm, "__lc_key")); return p && p->v.t == T_STR ? p->v.u.s->s : NULL; }

static V2 l_domain(VM2 *vm, V2 self, int argc, V2 *argv) {
    const char *url = avm1_movie_url(vm->player, vm->player->movie), *p = url ? strstr(url, "://") : NULL, *q; char dom[256]; size_t l; UNUSED;
    if (!p || !strncmp(url, "file:", 5)) return v2_cstr(vm, "localhost");
    p += 3; q = p; while (*q && *q != '/' && *q != ':' && *q != '?' && *q != '#') q++;
    l = (size_t)(q - p); if (l >= sizeof(dom)) l = sizeof(dom) - 1; memcpy(dom, p, l); dom[l] = 0;
    return v2_cstr(vm, l ? dom : "localhost");
}
static V2 l_connect(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Str *name = nonnull(vm, ARG(0), "connectionName"); char sd[256], *key;
    if (!name || self.t != T_OBJ) return v2_undef();
    if (!name->len) { a2_throw_id(vm, "ArgumentError", 2085, "connectionName", NULL); return v2_undef(); }
    if (strchr(name->s, ':')) { a2_throw_id(vm, "ArgumentError", 2004, NULL, NULL); return v2_undef(); }
    hook(vm);
    if (key_of(vm, self.u.o)) { a2_throw_id(vm, "ArgumentError", 2082, NULL, NULL); return v2_undef(); }
    superdomain(vm, sd, sizeof(sd));
    key = lch_key(sd, name->s, 0);
    if (!key || !lch_connect(key, 1, self.u.o)) { free(key); a2_throw_id(vm, "ArgumentError", 2082, NULL, NULL); return v2_undef(); }
    a2_dyn_set_hidden(vm, self.u.o, a2_str(vm, "__lc_key"), v2_cstr(vm, key));
    free(key);
    return v2_undef();
}
static V2 l_close(VM2 *vm, V2 self, int argc, V2 *argv) {
    const char *k; UNUSED;
    if (self.t != T_OBJ) return v2_undef();
    k = key_of(vm, self.u.o);
    if (!k) { a2_throw_id(vm, "ArgumentError", 2083, NULL, NULL); return v2_undef(); }
    lch_close(k);
    a2_dyn_set_hidden(vm, self.u.o, a2_str(vm, "__lc_key"), v2_undef());
    return v2_undef();
}
static V2 l_send(VM2 *vm, V2 self, int argc, V2 *argv) {
    static const char *bad[] = { "send", "connect", "close", "allowDomain", "allowInsecureDomain", "domain", NULL };
    A2Str *cn = nonnull(vm, ARG(0), "connectionName"), *mn; char sd[256], *conn; int i, nargs; u8 **args; size_t *argn;
    if (!cn) return v2_undef();
    mn = nonnull(vm, ARG(1), "methodName");
    if (!mn || self.t != T_OBJ) return v2_undef();
    if (!cn->len) { a2_throw_id(vm, "ArgumentError", 2085, "connectionName", NULL); return v2_undef(); }
    if (!mn->len) { a2_throw_id(vm, "ArgumentError", 2085, "methodName", NULL); return v2_undef(); }
    for (i = 0; bad[i]; i++) if (!strcmp(mn->s, bad[i])) { a2_throw_id(vm, "ArgumentError", 2004, NULL, NULL); return v2_undef(); }
    hook(vm);
    nargs = argc > 2 ? argc - 2 : 0;
    args = (u8**)calloc((size_t)nargs + 1, sizeof(u8*)); argn = (size_t*)calloc((size_t)nargs + 1, sizeof(size_t));
    if (!args || !argn) { free(args); free(argn); return v2_undef(); }
    for (i = 0; i < nargs; i++) {
        A2Obj *b = a2_bytearray_new(vm);
        if (!b) continue;
        a2_amf_write(vm, b, argv[2 + i], 0);
        if (vm->throwing) { vm->throwing = 0; vm->exc = v2_undef(); }
        if (b->blen > 0) { args[i] = (u8*)malloc((size_t)b->blen); if (args[i]) { memcpy(args[i], b->bytes, (size_t)b->blen); argn[i] = (size_t)b->blen; } }
    }
    superdomain(vm, sd, sizeof(sd));
    conn = lch_key(sd, cn->s, 1);
    lch_send(1, self.u.o, conn ? conn : "", mn->s, nargs, args, argn);
    free(conn);
    return v2_undef();
}
static V2 l_client_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Prop *c; UNUSED; c = self.t == T_OBJ ? a2_dyn_find(self.u.o, a2_str(vm, "__lc_client")) : NULL; return c ? c->v : self; }
static V2 l_client_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    if (ARG(0).t != T_OBJ) { a2_throw_id(vm, "TypeError", 2004, NULL, NULL); return v2_undef(); }
    if (self.t == T_OBJ) a2_dyn_set_hidden(vm, self.u.o, a2_str(vm, "__lc_client"), ARG(0));
    return v2_undef();
}

void a2_localconn_frame(VM2 *vm) { hook(vm); lch_deliver(); }
static void mark_cb(void *ctx, void *obj) { a2_mark_obj((VM2*)ctx, (A2Obj*)obj); }
void a2_localconn_mark(VM2 *vm) { lch_mark(1, mark_cb, vm); }

void a2_localconn_natives_init(void) {
#define LC "flash.net::LocalConnection/"
    a2_register_native(LC "get domain", l_domain);
    a2_register_native(LC "connect", l_connect);
    a2_register_native(LC "close", l_close);
    a2_register_native(LC "send", l_send);
    a2_register_native(LC "get client", l_client_get);
    a2_register_native(LC "set client", l_client_set);
}
