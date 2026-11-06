/* avm2_fileref.c - natives of flash.net.FileReference (the class, like
 * FileReferenceList, is Ruffle's ActionScript, see as3/).
 *
 * browse() and save() open the platform file dialog later, at the end of
 * the frame (Flash runs them asynchronously); the test navigator (-T)
 * simulates the dialogs like Ruffle's test UI: an open dialog with a
 * "debug-select-success" filter selects test.txt ("Hello, World!") and a
 * save dialog for "debug-success.txt" succeeds; anything else is cancelled.
 *
 * The selection is kept in hidden properties of the FileReference. */
#include "avm2_flash_int.h"
#include "fetch.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

extern void a2_report_uncaught(VM2 *vm);
extern int g_test_navigator;
int plat_file_dialog(int save, const char *filter, const char *name, char *out, size_t cap);

enum { FOP_BROWSE = 1, FOP_SAVE };
typedef struct { int kind; A2Obj *o; char *filter; int test_ok; char *name; u8 *data; size_t n; } FOp;
static FOp *g_fops; static int g_nfops;

static V2 hget(VM2 *vm, A2Obj *o, const char *k) { A2Prop *p = a2_dyn_find(o, a2_str(vm, k)); return p ? p->v : v2_undef(); }
static void hset(VM2 *vm, A2Obj *o, const char *k, V2 v) { a2_dyn_set_hidden(vm, o, a2_str(vm, k), v); }
static int has_ref(VM2 *vm, A2Obj *o) { return v2_tobool(vm, hget(vm, o, "__fr_ref")); }
static A2Obj *selfo(V2 self) { return self.t == T_OBJ ? self.u.o : NULL; }

static A2Obj *need_ref(VM2 *vm, V2 self) {
    A2Obj *o = selfo(self);
    if (!o || !has_ref(vm, o)) { a2_throw_id(vm, "Error", 2037, NULL, NULL); return NULL; }
    return o;
}

static V2 f_creationDate(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return need_ref(vm, self) ? v2_null() : v2_undef(); }
static V2 f_name(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = need_ref(vm, self); UNUSED; return o ? hget(vm, o, "__fr_name") : v2_undef(); }
static V2 f_size(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = need_ref(vm, self); UNUSED; return o ? v2_num(v2_tonum(vm, hget(vm, o, "__fr_size"))) : v2_undef(); }
static V2 f_type(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = need_ref(vm, self); UNUSED; return o ? hget(vm, o, "__fr_type") : v2_undef(); }
static V2 f_data(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = selfo(self), *src; V2 b, d; UNUSED;
    if (!o || !has_ref(vm, o) || !v2_tobool(vm, hget(vm, o, "__fr_loaded"))) return v2_null();
    d = hget(vm, o, "__fr_bytes"); src = d.t == T_OBJ ? d.u.o : NULL;
    b = a2_construct_class(vm, vm->c_bytearray, 0, NULL);
    if (b.t == T_OBJ && src && src->blen > 0) {
        b.u.o->bytes = (u8*)malloc((size_t)src->blen);
        if (b.u.o->bytes) { memcpy(b.u.o->bytes, src->bytes, (size_t)src->blen); b.u.o->blen = b.u.o->bcap = src->blen; }
    }
    return b;
}

/* the selection: name, type (".ext"), size and the contents */
static void set_ref(VM2 *vm, A2Obj *o, const char *name, const u8 *data, size_t n, double size) {
    const char *dot = strrchr(name, '.'); V2 b;
    hset(vm, o, "__fr_ref", v2_bool(1));
    hset(vm, o, "__fr_name", v2_cstr(vm, name));
    hset(vm, o, "__fr_type", dot ? v2_cstr(vm, dot) : v2_null());
    hset(vm, o, "__fr_size", v2_num(size));
    hset(vm, o, "__fr_loaded", v2_bool(0));
    b = a2_construct_class(vm, vm->c_bytearray, 0, NULL);
    if (b.t == T_OBJ && n) { b.u.o->bytes = (u8*)malloc(n); if (b.u.o->bytes) { memcpy(b.u.o->bytes, data, n); b.u.o->blen = b.u.o->bcap = (int)n; } }
    hset(vm, o, "__fr_bytes", b);
}

static void push(int kind, A2Obj *o, char *filter, int test_ok, const char *name, u8 *data, size_t n) {
    FOp *nf = (FOp*)realloc(g_fops, (size_t)(g_nfops + 1) * sizeof(FOp)); FOp *f;
    if (!nf) { free(filter); free(data); return; }
    g_fops = nf; f = &g_fops[g_nfops++];
    f->kind = kind; f->o = o; f->filter = filter; f->test_ok = test_ok; f->data = data; f->n = n;
    f->name = NULL;
    if (name) { size_t l = strlen(name); f->name = (char*)malloc(l + 1); if (f->name) memcpy(f->name, name, l + 1); }
}

static V2 f_browse(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = selfo(self); char *buf = NULL; size_t bn = 0; int test_ok = 0;
    A2Class *ff = a2_class_find_s(vm, "flash.net::FileFilter");
    if (!o) return v2_bool(0);
    if (ARG(0).t == T_OBJ && ARG(0).u.o->kind == OK_ARRAY) {
        A2Obj *arr = ARG(0).u.o; u32 i, n = a2_arr_len(arr);
        for (i = 0; i < n; i++) {
            V2 e = a2_arr_get(arr, (int)i), d, x; A2Str *ds, *xs; char *nb;
            if (e.t != T_OBJ || !ff || !e.u.o->cls || !a2_is_subclass(e.u.o->cls, ff)) { free(buf); a2_throw_id(vm, "ArgumentError", 2097, NULL, NULL); return v2_undef(); }
            d = a2_get_s(vm, e, "description"); x = a2_get_s(vm, e, "extension");
            if (vm->throwing) { free(buf); return v2_undef(); }
            if (d.t != T_STR || x.t != T_STR || !d.u.s->len || !x.u.s->len) { free(buf); a2_throw_id(vm, "ArgumentError", 2097, NULL, NULL); return v2_undef(); }
            ds = d.u.s; xs = x.u.s;
            if (!strcmp(ds->s, "debug-select-success")) test_ok = 1;
            nb = (char*)realloc(buf, bn + ds->len + xs->len + 3); if (!nb) break;
            buf = nb; memcpy(buf + bn, ds->s, ds->len + 1); bn += ds->len + 1; memcpy(buf + bn, xs->s, xs->len + 1); bn += xs->len + 1; buf[bn] = 0;
        }
    }
    push(FOP_BROWSE, o, buf, test_ok, NULL, NULL, 0);
    return v2_bool(1);
}

static void fire(VM2 *vm, A2Obj *o, const char *type) { a2_dispatch_simple(vm, o, a2_str(vm, type), 0); if (vm->throwing) a2_report_uncaught(vm); }
static void progress(VM2 *vm, A2Obj *o, double loaded, double total) {
    A2Class *c = a2_class_find_s(vm, "flash.events::ProgressEvent"); V2 a[5], e;
    if (!c) return;
    a[0] = v2_cstr(vm, "progress"); a[1] = v2_bool(0); a[2] = v2_bool(0); a[3] = v2_num(loaded); a[4] = v2_num(total);
    e = a2_construct_class(vm, c, 5, a);
    if (!vm->throwing && e.t == T_OBJ) a2_dispatch_event_obj(vm, o, e.u.o);
    if (vm->throwing) a2_report_uncaught(vm);
}

static V2 f_load(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = need_ref(vm, self); double size; UNUSED;
    if (!o) return v2_undef();
    if (v2_tobool(vm, hget(vm, o, "__fr_saved"))) { a2_throw_id(vm, "Error", 2037, NULL, NULL); return v2_undef(); }
    size = v2_tonum(vm, hget(vm, o, "__fr_size"));
    fire(vm, o, "open"); progress(vm, o, 0, size);
    fire(vm, o, "open"); progress(vm, o, size, size);
    hset(vm, o, "__fr_loaded", v2_bool(1));
    fire(vm, o, "complete");
    return v2_undef();
}

static V2 f_saveInternal(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = selfo(self), *b = ARG(0).t == T_OBJ && ARG(0).u.o->kind == OK_BYTEARRAY ? ARG(0).u.o : NULL;
    A2Str *name = v2_tostr(vm, ARG(1)); u8 *data; size_t n;
    if (!o || !b || vm->throwing) return v2_undef();
    data = b->bytes; n = (size_t)b->blen;              /* the ByteArray is emptied */
    b->bytes = NULL; b->blen = b->bcap = b->bpos = 0;
    push(FOP_SAVE, o, NULL, 0, name->s, data, n);
    return v2_undef();
}

static void run(VM2 *vm, FOp *f) {
    A2Obj *o = f->o; char path[1100];
    if (f->kind == FOP_BROWSE) {
        if (g_test_navigator) {
            if (!f->test_ok) { fire(vm, o, "cancel"); return; }
            set_ref(vm, o, "test.txt", (const u8*)"Hello, World!", 13, 13);
        } else {
            u8 *d; size_t n = 0; const char *b;
            if (!plat_file_dialog(0, f->filter, NULL, path, sizeof(path))) { fire(vm, o, "cancel"); return; }
            d = fetch_read_file(path, &n);
            b = path + strlen(path); while (b > path && b[-1] != '/' && b[-1] != '\\') b--;
            set_ref(vm, o, b, d, n, (double)n);
            free(d);
        }
        hset(vm, o, "__fr_saved", v2_bool(0));
        fire(vm, o, "select");
    } else {
        const char *b;
        if (g_test_navigator) {
            if (!f->name || strcmp(f->name, "debug-success.txt")) { fire(vm, o, "cancel"); return; }
            strcpy(path, f->name);
        } else {
            FILE *fp;
            if (!plat_file_dialog(1, NULL, f->name, path, sizeof(path))) { fire(vm, o, "cancel"); return; }
            fp = fopen(path, "wb");
            if (fp) { if (f->n) fwrite(f->data, 1, f->n, fp); fclose(fp); }
        }
        b = path + strlen(path); while (b > path && b[-1] != '/' && b[-1] != '\\') b--;
        set_ref(vm, o, b, NULL, 0, 0);
        hset(vm, o, "__fr_saved", v2_bool(1));
        fire(vm, o, "select");
        fire(vm, o, "open");
        progress(vm, o, (double)f->n, (double)f->n);
        hset(vm, o, "__fr_size", v2_num((double)f->n));
        fire(vm, o, "complete");
    }
}

void a2_fileref_run(VM2 *vm) {
    int guard = 0;
    while (g_nfops && guard++ < 64) {
        FOp *ops = g_fops; int n = g_nfops, i;
        g_fops = NULL; g_nfops = 0;
        for (i = 0; i < n; i++) { run(vm, &ops[i]); free(ops[i].filter); free(ops[i].name); free(ops[i].data); }
        free(ops);
    }
}
void a2_fileref_mark(VM2 *vm) { int i; for (i = 0; i < g_nfops; i++) a2_mark_obj(vm, g_fops[i].o); }

void a2_fileref_natives_init(void) {
#define FR "flash.net::FileReference/"
    a2_register_native(FR "get creationDate", f_creationDate);
    a2_register_native(FR "get modificationDate", f_creationDate);
    a2_register_native(FR "get name", f_name);
    a2_register_native(FR "get size", f_size);
    a2_register_native(FR "get type", f_type);
    a2_register_native(FR "get data", f_data);
    a2_register_native(FR "browse", f_browse);
    a2_register_native(FR "load", f_load);
    a2_register_native(FR "saveInternal", f_saveInternal);
}
