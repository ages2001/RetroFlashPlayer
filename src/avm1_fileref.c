/* avm1_fileref.c - flash.net.FileReference and FileReferenceList for AVM1
 * (Ruffle avm1/globals/file_reference.rs and the file dialog loaders).
 *
 * Dialogs are the platform's (GetOpenFileName / GetSaveFileName on
 * Windows); the test harness gets the scripted dialogs and URLs of
 * Ruffle's test backends. Everything completes at the end of the frame,
 * like the dialog futures of the Flash Player do. */
#include "avm1_int.h"
#include "fetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DE  PA_DONT_ENUM
#define DD  PA_DONT_DELETE
#define RO  PA_READ_ONLY

extern AObj *lib_class(VM *vm, Env *env, NativeFn ctor, NativeFn call, AObj *super_proto, int proto_first, AObj **proto_out);
extern int avm1_broadcast(Act *a, AObj *self, int argc, Value *argv, const char *method);
extern void avm1_trace_out(const char *s);
extern void avm1_run_actions(VM *vm);
extern int g_test_navigator;

/* platform dialogs (plat_*.c): 1 when a file was chosen */
int plat_file_dialog(int save, const char *filter, const char *name, char *out, size_t cap);

typedef struct {
    int init;
    AStr *name, *type, *creator, *post_data;
    int has_size; double size;
    u8 *data; size_t n;
} FRData;

static void fr_free(AObj *o) { FRData *d = (FRData*)o->nat.p; if (d) { free(d->data); free(d); } o->nat.p = NULL; }
static void fr_mark(VM *vm, AObj *o) { FRData *d = (FRData*)o->nat.p; (void)vm; if (!d) return; if (d->name) gc_mark_str(d->name); if (d->type) gc_mark_str(d->type); if (d->creator) gc_mark_str(d->creator); if (d->post_data) gc_mark_str(d->post_data); }
static FRData *fr_of(AObj *o) { if (o && o->nk == NK_SUPER) o = o->nat.sup.self; return o && o->nk == NK_FILEREF ? (FRData*)o->nat.p : NULL; }

static void fr_set_native(AObj *t) { FRData *d = (FRData*)calloc(1, sizeof(FRData)); if (d) { t->nk = NK_FILEREF; t->nat.p = d; t->nfree = fr_free; t->nmark = fr_mark; } }

/* a selected file */
typedef struct { char name[512]; char type[64]; u8 *data; size_t n; char path[1100]; } Sel;

static void sel_from_path(Sel *s, const char *path) {
    const char *b = path + strlen(path), *dot;
    while (b > path && b[-1] != '/' && b[-1] != '\\') b--;
    strncpy(s->name, b, sizeof(s->name) - 1); s->name[sizeof(s->name) - 1] = 0;
    dot = strrchr(s->name, '.');
    if (dot) { strncpy(s->type, dot, sizeof(s->type) - 1); s->type[sizeof(s->type) - 1] = 0; } else s->type[0] = 0;
    strncpy(s->path, path, sizeof(s->path) - 1); s->path[sizeof(s->path) - 1] = 0;
    s->data = fetch_read_file(path, &s->n);
}
static void sel_test(Sel *s, const char *name) {
    memset(s, 0, sizeof(*s));
    strcpy(s->name, name); strcpy(s->type, ".txt");
    s->data = (u8*)malloc(13); if (s->data) { memcpy(s->data, "Hello, World!", 13); s->n = 13; }
}

static void fr_init_from(VM *vm, FRData *d, const Sel *s) {
    d->init = 1;
    d->type = s->type[0] ? str_utf8(vm, s->type, strlen(s->type)) : NULL;
    d->name = str_utf8(vm, s->name, strlen(s->name));
    d->creator = NULL;
    d->has_size = 1; d->size = (double)s->n;
    free(d->data); d->data = NULL; d->n = 0;
    if (s->n) { d->data = (u8*)malloc(s->n); if (d->data) { memcpy(d->data, s->data, s->n); d->n = s->n; } }
}

/* ---- pending operations ---- */

enum { OP_SELECT = 1, OP_SELECT_MULTI, OP_DOWNLOAD, OP_UPLOAD };
typedef struct { int kind; AObj *obj; char *filter; int test_ok; char *url; char *name; } FROp;
static FROp *g_ops; static int g_nops;

static char *dupz(const char *s) { size_t l = strlen(s); char *d = (char*)malloc(l + 1); if (d) memcpy(d, s, l + 1); return d; }

static void push_op(int kind, AObj *o, char *filter, int test_ok, const char *url, const char *name) {
    FROp *n = (FROp*)realloc(g_ops, (size_t)(g_nops + 1) * sizeof(FROp));
    if (!n) { free(filter); return; }
    g_ops = n;
    g_ops[g_nops].kind = kind; g_ops[g_nops].obj = o; g_ops[g_nops].filter = filter; g_ops[g_nops].test_ok = test_ok;
    g_ops[g_nops].url = url ? dupz(url) : NULL; g_ops[g_nops].name = name ? dupz(name) : NULL;
    g_nops++;
}

void avm1_fileref_mark(VM *vm) { int i; for (i = 0; i < g_nops; i++) gc_mark_obj(vm, g_ops[i].obj); }
void avm1_fileref_free(void) { int i; for (i = 0; i < g_nops; i++) { free(g_ops[i].filter); free(g_ops[i].url); free(g_ops[i].name); } free(g_ops); g_ops = NULL; g_nops = 0; }

/* ---- filters: [{description, extension, macType}] ---- */

/* 1 ok (*out: "desc\0ext\0...\0" or NULL for none), 0 invalid; *test_ok: a "debug-select-success" description */
static int parse_filters(Act *a, int argc, Value *argv, char **out, int *test_ok) {
    VM *vm = a->vm; AObj *arr; int len, i; SBuf dummy; char *buf = NULL; size_t bn = 0;
    (void)dummy;
    *out = NULL; *test_ok = 0;
    if (argc < 1) return 1;
    if (argv[0].t != V_OBJ) return 0;
    arr = argv[0].u.o;
    len = o_length(a, arr); if (vm->err) return -1;
    if (len <= 0) return 0;
    for (i = 0; i < len; i++) {
        Value e = o_get_element(a, arr, i); AObj *eo; AStr *k_desc = str_intern(vm, "description"), *k_ext = str_intern(vm, "extension"), *ds, *es; Value dv, ev; const char *dc, *ec; size_t dl, el; char *nb;
        if (vm->err) { free(buf); return -1; }
        if (e.t != V_OBJ) { free(buf); return 0; }
        eo = e.u.o;
        {
            AStr *k_mac = str_intern(vm, "macType"); int hd, he, hm; Value mv = v_undef();
            hd = o_has_own_property(a, eo, k_desc); he = o_has_own_property(a, eo, k_ext); hm = o_has_own_property(a, eo, k_mac);
            if (hd) { dv = o_get(a, eo, k_desc); if (vm->err) { vm->err = 0; hd = 0; } }
            if (he) { ev = o_get(a, eo, k_ext); if (vm->err) { vm->err = 0; he = 0; } }
            if (hm) { mv = o_get(a, eo, k_mac); if (vm->err) { vm->err = 0; hm = 0; } }
            if (!hd || !he) { free(buf); return 0; }
            ds = v_str_of(a, dv); if (vm->err) { free(buf); return -1; }
            es = v_str_of(a, ev); if (vm->err) { free(buf); return -1; }
            if (hm) { v_str_of(a, mv); if (vm->err) { free(buf); return -1; } }
        }
        if (!ds->len || !es->len) { free(buf); return 0; }
        dc = str_c(vm, ds); ec = str_c(vm, es); dl = strlen(dc); el = strlen(ec);
        if (!strcmp(dc, "debug-select-success")) *test_ok = 1;
        nb = (char*)realloc(buf, bn + dl + el + 3); if (!nb) { free(buf); return 0; }
        buf = nb; memcpy(buf + bn, dc, dl + 1); bn += dl + 1; memcpy(buf + bn, ec, el + 1); bn += el + 1; buf[bn] = 0;
    }
    *out = buf;
    return 1;
}

/* ---- the class ---- */

static Value fr_name(Act *a, AObj *self, int argc, Value *argv);
static Value fr_type(Act *a, AObj *self, int argc, Value *argv);
static Value fr_creator(Act *a, AObj *self, int argc, Value *argv);
static Value fr_size(Act *a, AObj *self, int argc, Value *argv);
static Value fr_undef(Act *a, AObj *self, int argc, Value *argv);
static Value fr_post_data(Act *a, AObj *self, int argc, Value *argv);
static Value fr_set_post_data(Act *a, AObj *self, int argc, Value *argv);
/* the Linux player's constructors give each object its members (and listener list) */
static void fr_linux_members(Act *a, AObj *t) {
    static const char *const nm[7] = { "name", "type", "size", "modificationDate", "creationDate", "creator", "postData" };
    static const NativeFn g[7] = { fr_name, fr_type, fr_size, fr_undef, fr_undef, fr_creator, fr_post_data };
    VM *vm = a->vm; Env *env = env_get(vm, a->swf_version); int i; AObj *l;
    for (i = 0; i < 7; i++) o_add_property(vm, t, str_intern(vm, nm[i]), fn_native(vm, env, g[i], NULL), i == 6 ? fn_native(vm, env, fr_set_post_data, NULL) : NULL, PA_DONT_ENUM);
    l = obj_array(a, 0, NULL); if (l) o_define(vm, t, vm->s__listeners, v_obj(l), PA_DONT_ENUM);
}
static Value fr_ctor(Act *a, AObj *self, int argc, Value *argv) {
    extern int devfont_linux_rules(void);
    AObj *t = o_native_target(self); (void)argc; (void)argv;
    if (t) { fr_set_native(t); if (devfont_linux_rules()) fr_linux_members(a, t); }
    return v_undef();
}

static Value fr_name(Act *a, AObj *self, int argc, Value *argv) { FRData *d = fr_of(self); (void)a; (void)argc; (void)argv; return d && d->name ? v_str(d->name) : v_undef(); }
static Value fr_type(Act *a, AObj *self, int argc, Value *argv) { FRData *d = fr_of(self); (void)a; (void)argc; (void)argv; return d && d->type ? v_str(d->type) : v_undef(); }
static Value fr_creator(Act *a, AObj *self, int argc, Value *argv) { FRData *d = fr_of(self); (void)a; (void)argc; (void)argv; return d && d->creator ? v_str(d->creator) : v_undef(); }
static Value fr_size(Act *a, AObj *self, int argc, Value *argv) { FRData *d = fr_of(self); (void)a; (void)argc; (void)argv; return d && d->has_size ? v_num(d->size) : v_undef(); }
static Value fr_undef(Act *a, AObj *self, int argc, Value *argv) { (void)a; (void)self; (void)argc; (void)argv; return v_undef(); }
static Value fr_post_data(Act *a, AObj *self, int argc, Value *argv) { FRData *d = fr_of(self); (void)argc; (void)argv; if (!d) return v_undef(); return v_str(d->post_data ? d->post_data : a->vm->s_empty); }
static Value fr_set_post_data(Act *a, AObj *self, int argc, Value *argv) { AStr *s = v_str_of(a, arg(argc, argv, 0)); FRData *d; if (a->vm->err) return v_undef(); d = fr_of(self); if (d) d->post_data = s; return v_undef(); }

static Value fr_browse(Act *a, AObj *self, int argc, Value *argv) {
    char *f; int ok, r;
    if (!fr_of(self)) return v_undef();
    r = parse_filters(a, argc, argv, &f, &ok);
    if (r < 0) return v_undef();
    if (!r) return v_bool(0);
    push_op(OP_SELECT, self->nk == NK_SUPER ? self->nat.sup.self : self, f, ok, NULL, NULL);
    return v_bool(1);
}

/* url::Url::parse would accept it: a scheme then ':' */
static int url_ok(const char *u) {
    const char *p = u;
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) return 0;
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '+' || *p == '-' || *p == '.') p++;
    return *p == ':';
}

static Value fr_download(Act *a, AObj *self, int argc, Value *argv) {
    VM *vm = a->vm; AStr *us; const char *u; char name[512];
    if (argc < 1) return v_bool(0);
    us = v_str_of(a, argv[0]); if (vm->err) return v_undef();
    u = str_c(vm, us);
    if (!url_ok(u)) return v_bool(0);
    if (argc > 1) { AStr *n = v_str_of(a, argv[1]); if (vm->err) return v_undef(); strncpy(name, str_c(vm, n), sizeof(name) - 1); name[sizeof(name) - 1] = 0; }
    else {
        const char *p = strstr(u, "://"), *q, *e, *sl;
        p = p ? p + 3 : strchr(u, ':') + 1;
        q = strchr(p, '/'); e = q ? q : p + strlen(p);
        { const char *qq = strpbrk(q ? q : "", "?#"); e = qq ? qq : (q ? q + strlen(q) : e); }
        sl = e; while (q && sl > q && sl[-1] != '/') sl--;
        if (!q) sl = e;
        { size_t l = (size_t)(e - sl); if (l >= sizeof(name)) l = sizeof(name) - 1; memcpy(name, sl, l); name[l] = 0; }
    }
    push_op(OP_DOWNLOAD, self->nk == NK_SUPER ? self->nat.sup.self : self, NULL, 0, u, name);
    return v_bool(1);
}

static Value fr_upload(Act *a, AObj *self, int argc, Value *argv) {
    VM *vm = a->vm; FRData *d = fr_of(self); AStr *us; const char *u;
    if (!d) return v_undef();
    if (!d->init) return v_bool(0);
    if (argc < 1) return v_bool(0);
    us = v_str_of(a, argv[0]); if (vm->err) return v_undef();
    u = str_c(vm, us);
    if (!url_ok(u)) return v_bool(0);
    if (strncmp(u, "http://", 7) && strncmp(u, "https://", 8)) return v_bool(0);
    push_op(OP_UPLOAD, self->nk == NK_SUPER ? self->nat.sup.self : self, NULL, 0, u, NULL);
    return v_bool(1);
}

static Value frl_ctor(Act *a, AObj *self, int argc, Value *argv) {
    extern int devfont_linux_rules(void);
    AObj *t = o_native_target(self); (void)argc; (void)argv;
    if (t && devfont_linux_rules()) {   /* the Linux player: its own file list and listener list */
        AObj *l;
        o_define(a->vm, t, str_intern(a->vm, "fileList"), v_undef(), PA_DONT_ENUM);
        l = obj_array(a, 0, NULL); if (l) o_define(a->vm, t, a->vm->s__listeners, v_obj(l), PA_DONT_ENUM);
    }
    return v_undef();
}
static Value frl_browse(Act *a, AObj *self, int argc, Value *argv) {
    char *f; int ok, r;
    r = parse_filters(a, argc, argv, &f, &ok);
    if (r < 0) return v_undef();
    if (!r) return v_bool(0);
    push_op(OP_SELECT_MULTI, self->nk == NK_SUPER ? self->nat.sup.self : self, f, ok, NULL, NULL);
    return v_bool(1);
}

/* ---- completing the operations ---- */

static void ev(Act *a, AObj *o, const char *name) { Value t = v_obj(o); avm1_broadcast(a, o, 1, &t, name); a->vm->err = 0; }
static void ev3(Act *a, AObj *o, const char *name, double n) { Value t[3]; t[0] = v_obj(o); t[1] = v_num(n); t[2] = v_num(n); avm1_broadcast(a, o, 3, t, name); a->vm->err = 0; }

/* the test navigator's magic URLs: 0 ok (body), 1 HTTP error, 2 DNS error, 3 other failure */
static int fetch_url(VM *vm, const char *url, u8 **body, size_t *n) {
    char full[1100], path[1100]; const char *base = vm->player->movie && vm->player->movie->url ? vm->player->movie->url : "file:///test.swf";
    *body = NULL; *n = 0;
    if (g_test_navigator) {
        if (strstr(url, "?debug-success")) { *body = (u8*)malloc(13); if (*body) { memcpy(*body, "Hello, World!", 13); *n = 13; } return 0; }
        if (strstr(url, "?debug-error-statuscode")) return 1;
        if (strstr(url, "?debug-error-dns")) return 2;
    }
    fetch_url_resolve(base, url, full, sizeof(full));
    if (fetch_url_to_path(vm->player, full, path, sizeof(path))) { *body = fetch_read_file(path, n); if (*body) return 0; }
    return strncmp(full, "file:", 5) ? 2 : 3;
}

static int choose(int save, const char *filter, int test_ok, const char *name, Sel *s) {
    char path[1100];
    memset(s, 0, sizeof(*s));
    if (g_test_navigator) {
        if (save) { if (name && !strcmp(name, "debug-success.txt")) { sel_test(s, name); return 1; } return 0; }
        if (test_ok) { sel_test(s, "test.txt"); return 1; }
        return 0;
    }
    if (!plat_file_dialog(save, filter, name, path, sizeof(path))) return 0;
    if (save) {
        const char *b = path + strlen(path), *dot;
        while (b > path && b[-1] != '/' && b[-1] != '\\') b--;
        strncpy(s->name, b, sizeof(s->name) - 1);
        dot = strrchr(s->name, '.'); if (dot) strncpy(s->type, dot, sizeof(s->type) - 1);
        memcpy(s->path, path, sizeof(s->path) - 1);
    } else sel_from_path(s, path);
    return 1;
}

static void run_op(VM *vm, FROp *op) {
    Act a; AObj *o = op->obj; FRData *d = fr_of(o); Sel s;
    act_init_nothing(&a, vm, vm->player->root, "[File Dialog]");
    switch (op->kind) {
    case OP_SELECT:
        if (!d) break;
        if (choose(0, op->filter, op->test_ok, NULL, &s)) { fr_init_from(vm, d, &s); free(s.data); ev(&a, o, "onSelect"); }
        else ev(&a, o, "onCancel");
        break;
    case OP_SELECT_MULTI: {
        if (g_test_navigator ? op->test_ok : 0) {
            static const char *names[3] = { "test1.txt", "test2.txt", "test3.txt" }; Value items[3]; int i; AObj *arr;
            for (i = 0; i < 3; i++) {
                AObj *fo = obj_new(vm, PROTOS(&a)->fileref); FRData *fd;
                fr_set_native(fo); fd = fr_of(fo);
                sel_test(&s, names[i]); if (fd) fr_init_from(vm, fd, &s); free(s.data);
                items[i] = v_obj(fo);
            }
            arr = obj_array(&a, 3, items);
            o_define(vm, o, str_intern(vm, "fileList"), v_obj(arr), DE | DD | RO);
            ev(&a, o, "onSelect");
        } else if (!g_test_navigator && choose(0, op->filter, 0, NULL, &s)) {
            AObj *fo = obj_new(vm, PROTOS(&a)->fileref), *arr; FRData *fd; Value it;
            fr_set_native(fo); fd = fr_of(fo); if (fd) fr_init_from(vm, fd, &s); free(s.data);
            it = v_obj(fo); arr = obj_array(&a, 1, &it);
            o_define(vm, o, str_intern(vm, "fileList"), v_obj(arr), DE | DD | RO);
            ev(&a, o, "onSelect");
        } else ev(&a, o, "onCancel");
        break;
    }
    case OP_DOWNLOAD: {
        u8 *body; size_t n; int r;
        if (!d) break;
        if (!choose(1, NULL, 0, op->name, &s)) { ev(&a, o, "onCancel"); break; }
        fr_init_from(vm, d, &s);
        ev(&a, o, "onSelect");
        r = fetch_url(vm, op->url, &body, &n);
        if (r == 0) {
            ev(&a, o, "onOpen");
            if (s.path[0]) { FILE *f = fopen(s.path, "wb"); if (f) { if (n) fwrite(body, 1, n, f); fclose(f); } }
            free(s.data); s.data = body; s.n = n; body = NULL;
            d = fr_of(o); if (d) fr_init_from(vm, d, &s);
            ev3(&a, o, "onProgress", (double)n);
            ev(&a, o, "onComplete");
        } else {
            char line[1200];
            sprintf(line, "Error opening URL '%.1100s'", op->url);
            if (r == 2) { avm1_trace_out(line); ev(&a, o, "onIOError"); }
            else if (r == 1) { ev(&a, o, "onOpen"); avm1_trace_out(line); ev(&a, o, "onIOError"); ev3(&a, o, "onProgress", 0); }
            else { ev(&a, o, "onOpen"); avm1_trace_out(line); ev(&a, o, "onIOError"); }
        }
        free(body); free(s.data);
        break;
    }
    case OP_UPLOAD: {
        double total = d ? (double)d->n : 0; u8 *body; size_t n; int r;
        r = fetch_url(vm, op->url, &body, &n);   /* (the file itself is not sent anywhere) */
        free(body);
        ev(&a, o, "onOpen");
        if (r == 0) { ev3(&a, o, "onProgress", total); ev(&a, o, "onComplete"); }
        else if (r == 1) { ev3(&a, o, "onProgress", total); ev(&a, o, "onHTTPError"); }
        else ev(&a, o, "onIOError");
        break;
    }
    }
    avm1_run_actions(vm);
}

void avm1_fileref_run(VM *vm) {
    int guard = 0;
    while (g_nops && guard++ < 64) {
        FROp *ops = g_ops; int n = g_nops, i;
        g_ops = NULL; g_nops = 0;
        for (i = 0; i < n; i++) { run_op(vm, &ops[i]); free(ops[i].filter); free(ops[i].url); free(ops[i].name); }
        free(ops);
    }
}

static void defm(VM *vm, Env *env, AObj *o, const char *name, NativeFn f, u16 attr) { o_define(vm, o, str_intern(vm, name), v_obj(fn_native(vm, env, f, NULL)), attr); }
static void defp(VM *vm, Env *env, AObj *o, const char *name, NativeFn g, NativeFn s, u16 attr) { o_add_property(vm, o, str_intern(vm, name), fn_native(vm, env, g, NULL), s ? fn_native(vm, env, s, NULL) : NULL, attr); }

AObj *lib_fileref(VM *vm, Env *env, AObj *object_proto) {
    AObj *proto, *c = lib_class(vm, env, NULL, fr_ctor, object_proto, 0, &proto);
    env->p.fileref = proto;
    defp(vm, env, proto, "name", fr_name, NULL, DE);
    defp(vm, env, proto, "type", fr_type, NULL, DE);
    defp(vm, env, proto, "size", fr_size, NULL, DE);
    defp(vm, env, proto, "modificationDate", fr_undef, NULL, DE);
    defp(vm, env, proto, "creationDate", fr_undef, NULL, DE);
    defp(vm, env, proto, "creator", fr_creator, NULL, DE);
    defp(vm, env, proto, "postData", fr_post_data, fr_set_post_data, DE);
    defm(vm, env, proto, "browse", fr_browse, DE);
    defm(vm, env, proto, "upload", fr_upload, DE);
    defm(vm, env, proto, "download", fr_download, DE);
    defm(vm, env, proto, "cancel", fr_undef, DE);
    return c;
}

AObj *lib_filereflist(VM *vm, Env *env, AObj *object_proto) {
    AObj *proto, *c = lib_class(vm, env, NULL, frl_ctor, object_proto, 0, &proto);
    defm(vm, env, proto, "browse", frl_browse, DE);
    return c;
}
