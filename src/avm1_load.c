/* avm1_load.c - loading content from AVM1: loadMovie / loadMovieNum /
 * MovieClipLoader, loadVariables, LoadVars and XML.load, unloadMovie.
 *
 * Requests are queued while scripts run and processed at the end of the
 * frame, one after the other, the way the Flash Player completes local
 * loads: a movie load unloads the target, enters the "loading" state,
 * fires onLoadStart, then (with the data) replaces the clip's movie and
 * fires onLoadProgress and onLoadComplete.  onLoadInit follows on the
 * next frame, once the new movie has run its first frame.
 *
 * Event order and the states a clip goes through follow the Flash Player
 * as documented by the Ruffle test suite. */
#include "avm1_int.h"
#include "fetch.h"
#include "bmd.h"
#include "image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DE  PA_DONT_ENUM
#define DD  PA_DONT_DELETE
#define RO  PA_READ_ONLY

extern AObj *lib_class(VM *vm, Env *env, NativeFn ctor, NativeFn call, AObj *super_proto, int proto_first, AObj **proto_out);
extern void avm1_unload_clip(VM *vm, MovieClip *c);
extern void avm1_post_instantiation_ex(VM *vm, MovieClip *c, AObj *init, int by_avm, int run_frame);
extern void avm1_queue_method(VM *vm, MovieClip *c, AObj *o, AStr *name, int argc, Value *argv);
extern int  avm1_clip_event(VM *vm, MovieClip *c, u32 flag, int keycode);
extern void avm1_run_init_action(Player *p, MovieClip *c, const u8 *code, size_t len);
extern void avm1_run_actions(VM *vm);
extern MovieClip *avm1_level_create(Player *p, int level, SWFMovie *movie);

enum { RQ_FORM = 0, RQ_DATA, RQ_MOVIE, RQ_UNLOAD, RQ_CSS };
enum { ML_PENDING = 0, ML_PARSING, ML_OK, ML_FAILED };

typedef struct LReq { int kind; AObj *target; AStr *url; int ml; int post; char *body; size_t blen; const char *mime; } LReq;
typedef struct MLoad {
    int used, status;
    AObj *target;           /* the clip being loaded into (its script object) */
    AObj *bc;               /* MovieClipLoader broadcasting the events (NULL: loadMovie) */
    AObj *base;             /* clip whose code started the load */
    SWFMovie *movie;
    u32 seq;
} MLoad;
typedef struct Loads {
    LReq *rq; int nrq, caprq, head;
    MLoad *ml; int nml;
    u32 seq;
} Loads;

static Loads *loads_of(VM *vm) {
    if (!vm->loads) vm->loads = calloc(1, sizeof(Loads));
    return (Loads*)vm->loads;
}

void avm1_loads_free(VM *vm) {
    Loads *L = (Loads*)vm->loads;
    if (!L) return;
    { int i; for (i = L->head; i < L->nrq; i++) free(L->rq[i].body); }
    free(L->rq); free(L->ml); free(L);
    vm->loads = NULL;
}

void avm1_loads_mark(VM *vm) {
    Loads *L = (Loads*)vm->loads; int i;
    if (!L) return;
    for (i = L->head; i < L->nrq; i++) { if (L->rq[i].target) gc_mark_obj(vm, L->rq[i].target); gc_mark_str(L->rq[i].url); }
    for (i = 0; i < L->nml; i++) if (L->ml[i].used) {
        if (L->ml[i].target) gc_mark_obj(vm, L->ml[i].target);
        if (L->ml[i].bc) gc_mark_obj(vm, L->ml[i].bc);
        if (L->ml[i].base) gc_mark_obj(vm, L->ml[i].base);
    }
}

static LReq *push_req(VM *vm, int kind, AObj *target, AStr *url, int ml) {
    Loads *L = loads_of(vm);
    if (!L) return NULL;
    if (L->nrq == L->caprq) {
        int nc = L->caprq ? L->caprq * 2 : 16;
        LReq *n = (LReq*)realloc(L->rq, (size_t)nc * sizeof(LReq));
        if (!n) return NULL;
        L->rq = n; L->caprq = nc;
    }
    memset(&L->rq[L->nrq], 0, sizeof(LReq));
    L->rq[L->nrq].kind = kind; L->rq[L->nrq].target = target; L->rq[L->nrq].url = url; L->rq[L->nrq].ml = ml;
    return &L->rq[L->nrq++];
}

static int new_loader(VM *vm, AObj *target, AObj *bc, AObj *base) {
    Loads *L = loads_of(vm); int i;
    if (!L) return -1;
    for (i = 0; i < L->nml; i++) if (!L->ml[i].used) break;
    if (i == L->nml) {
        MLoad *n = (MLoad*)realloc(L->ml, (size_t)(L->nml + 1) * sizeof(MLoad));
        if (!n) return -1;
        L->ml = n; L->nml++;
    }
    memset(&L->ml[i], 0, sizeof(MLoad));
    L->ml[i].used = 1; L->ml[i].target = target; L->ml[i].bc = bc; L->ml[i].base = base;
    L->ml[i].seq = ++L->seq;
    return i;
}

/* ---------------------------------------------------------------- */
/* URLs and data                                                    */

static const char *root_url(Player *p) { return p->movie && p->movie->url ? p->movie->url : "file:///test.swf"; }

const char *avm1_movie_url(Player *p, SWFMovie *m) { return m && m->url ? m->url : root_url(p); }

static void resolve(VM *vm, AStr *url, char *out, size_t n) {
    fetch_url_resolve(root_url(vm->player), str_c(vm, url), out, n);
}

static u8 *fetch(VM *vm, const char *url, size_t *len) {
    char path[1100];
    if (!fetch_url_to_path(vm->player, url, path, sizeof(path))) return NULL;
    return fetch_read_file(path, len);
}

/* ---------------------------------------------------------------- */
/* requests and form values (Activation::object_into_request)       */

int g_fetch_log = 0;     /* the test harness logs navigation and fetches as trace output */
extern void avm1_trace_out(const char *s);

typedef struct { int n, cap; char **k, **v; } Form;

static void form_free(Form *f) { int i; for (i = 0; i < f->n; i++) { free(f->k[i]); free(f->v[i]); } free(f->k); free(f->v); f->n = f->cap = 0; f->k = f->v = NULL; }

static char *dupz(const char *s) { size_t l = strlen(s); char *d = (char*)malloc(l + 1); if (d) memcpy(d, s, l + 1); return d; }

static void form_put(Form *f, const char *k, const char *v) {
    int i;
    for (i = 0; i < f->n; i++) if (!strcmp(f->k[i], k)) { free(f->v[i]); f->v[i] = dupz(v); return; }
    if (f->n == f->cap) {
        int nc = f->cap ? f->cap * 2 : 8; char **nk = (char**)realloc(f->k, (size_t)nc * sizeof(char*)), **nv;
        if (!nk) return; f->k = nk;
        nv = (char**)realloc(f->v, (size_t)nc * sizeof(char*)); if (!nv) return; f->v = nv;
        f->cap = nc;
    }
    f->k[f->n] = dupz(k); f->v[f->n] = dupz(v); f->n++;
}

/* the enumerable properties of `o` as strings (errors give "undefined") */
static void form_of(Act *a, AObj *o, Form *f) {
    VM *vm = a->vm; int i, n = 0; AStr **keys;
    if (!o) return;
    keys = o_keys(a, o, 0, &n);
    vm->err = 0;
    for (i = 0; keys && i < n; i++) {
        Value v = o_get(a, o, keys[i]); AStr *s;
        if (vm->err) { vm->err = 0; v = v_undef(); }
        s = v_str_of(a, v);
        if (vm->err) { vm->err = 0; s = str_intern(vm, "undefined"); }
        form_put(f, str_c(vm, keys[i]), str_c(vm, s));
    }
    free(keys);
}

/* application/x-www-form-urlencoded (url::form_urlencoded::Serializer) */
static void enc_append(SBuf *b, const char *s) {
    static const char *hex = "0123456789ABCDEF";
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '*' || c == '-' || c == '.' || c == '_') sb_unit(b, c);
        else if (c == ' ') sb_unit(b, '+');
        else { sb_unit(b, '%'); sb_unit(b, (u16)hex[c >> 4]); sb_unit(b, (u16)hex[c & 15]); }
    }
}
static char *form_encode(VM *vm, const Form *f) {
    SBuf b; int i; AStr *r;
    sb_init(&b);
    for (i = 0; i < f->n; i++) { if (i) sb_unit(&b, '&'); enc_append(&b, f->k[i]); sb_unit(&b, '='); enc_append(&b, f->v[i]); }
    r = sb_done(vm, &b);
    return dupz(str_c(vm, r));
}

/* NavigationMethod::from_method_str (1 GET, 2 POST, 0 neither) */
static int method_of(Act *a, AStr *s) {
    const char *m = str_c(a->vm, s);
    if (strlen(m) == 3 && (m[0] | 32) == 'g' && (m[1] | 32) == 'e' && (m[2] | 32) == 't') return 1;
    if (strlen(m) == 4 && (m[0] | 32) == 'p' && (m[1] | 32) == 'o' && (m[2] | 32) == 's' && (m[3] | 32) == 't') return 2;
    return 0;
}

/* the vars of a GET request go in the URL, those of a POST in the body */
static void req_with_form(Act *a, LReq *r, int method, AObj *o) {
    VM *vm = a->vm; Form f; char *q;
    if (!r || !method) return;
    memset(&f, 0, sizeof(f));
    form_of(a, o, &f);
    q = form_encode(vm, &f);
    form_free(&f);
    if (!q) return;
    if (method == 1) {
        const char *u = str_c(vm, r->url); size_t ul = strlen(u), ql = strlen(q); char *nu = (char*)malloc(ul + ql + 2);
        if (nu) { memcpy(nu, u, ul); nu[ul] = strchr(u, '?') ? '&' : '?'; memcpy(nu + ul + 1, q, ql + 1); r->url = str_intern(vm, nu); free(nu); }
        free(q);
    } else {
        r->post = 1; r->body = q; r->blen = strlen(q); r->mime = "application/x-www-form-urlencoded";
    }
}

static AObj *locals_of(Act *a) { return a->scope ? a->scope->values : NULL; }

static void log_line(const char *s) { avm1_trace_out(s); }

void avm1_log_fetch(VM *vm, AStr *url, int post, const char *mime, const u8 *body, size_t blen) {
    char line[256]; SBuf b; size_t i;
    if (!g_fetch_log) return;
    log_line("Navigator::fetch:");
    sb_init(&b); sb_ascii(&b, "  URL: "); sb_str(&b, url); log_line(str_c(vm, sb_done(vm, &b)));
    log_line(post ? "  Method: POST" : "  Method: GET");
    if (body) {
        sprintf(line, "  Mime-Type: %.200s", mime ? mime : "");
        log_line(line);
        if (mime && !strcmp(mime, "application/x-www-form-urlencoded")) {
            char *t = (char*)malloc(blen + 9);
            if (t) { memcpy(t, "  Body: ", 8); memcpy(t + 8, body, blen); t[8 + blen] = 0; log_line(t); free(t); }
        } else {
            char *t = (char*)malloc(blen * 4 + 16), *o;
            if (t) {
                o = t; o += sprintf(o, "  Body: [");
                for (i = 0; i < blen; i++) o += sprintf(o, i ? ", %02X" : "%02X", body[i]);
                strcpy(o, "]"); log_line(t); free(t);
            }
        }
    }
}
static void log_request(VM *vm, LReq *r) { avm1_log_fetch(vm, r->url, r->post, r->mime, (const u8*)r->body, r->blen); }

/* normalize_navigate_target: any spelling of "blank" opens a new window */
static const char *norm_target(const char *t) {
    const char *s = t[0] == '_' ? t + 1 : t;
    if (strlen(s) == 5 && (s[0] | 32) == 'b' && (s[1] | 32) == 'l' && (s[2] | 32) == 'a' && (s[3] | 32) == 'n' && (s[4] | 32) == 'k') return "_blank";
    return t;
}

/* getURL & co. (method: 0 no vars, 1 GET, 2 POST); vars come from `vars` */
void avm1_navigate(Act *a, AStr *url, AStr *target, int method, AObj *vars, int normalize) {
    VM *vm = a->vm; const char *t = str_c(vm, target); Form f; int i; SBuf b;
    if (normalize) t = norm_target(t);
    memset(&f, 0, sizeof(f));
    if (method) form_of(a, vars, &f);
    if (g_fetch_log) {
        log_line("Navigator::navigate_to_url:");
        sb_init(&b); sb_ascii(&b, "  URL: "); sb_str(&b, url); log_line(str_c(vm, sb_done(vm, &b)));
        sb_init(&b); sb_ascii(&b, "  Target: "); sb_ascii(&b, t); log_line(str_c(vm, sb_done(vm, &b)));
        if (method) {
            log_line(method == 1 ? "  Method: GET" : "  Method: POST");
            for (i = 0; i < f.n; i++) {
                char *l = (char*)malloc(strlen(f.k[i]) + strlen(f.v[i]) + 16);
                if (l) { sprintf(l, "  Param: %s=%s", f.k[i], f.v[i]); log_line(l); free(l); }
            }
        }
    } else {
        extern void plat_open_url(const char *url);
        const char *u = str_c(vm, url);
        if (vm->player->debug) fprintf(stderr, "[avm1] getURL %s -> %s\n", u, t);
        if (!strncmp(u, "http:", 5) || !strncmp(u, "https:", 6) || !strncmp(u, "mailto:", 7) || !strncmp(u, "ftp:", 4)) plat_open_url(u);
    }
    form_free(&f);
}
void avm1_navigate_locals(Act *a, AStr *url, AStr *target, int method) { avm1_navigate(a, url, target, method, locals_of(a), 1); }

/* the text of a response (UTF-8, or UTF-16 with a byte order mark) */
static AStr *decode_text(VM *vm, const u8 *d, size_t n) {
    if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) return str_utf8(vm, (const char*)d + 3, n - 3);
    if (n >= 2 && ((d[0] == 0xFF && d[1] == 0xFE) || (d[0] == 0xFE && d[1] == 0xFF))) {
        int le = d[0] == 0xFF; size_t i, k = (n - 2) / 2; u16 *u = (u16*)malloc((k ? k : 1) * sizeof(u16)); AStr *s;
        if (!u) return vm->s_empty;
        for (i = 0; i < k; i++) u[i] = le ? (u16)(d[2 + i * 2] | (d[3 + i * 2] << 8)) : (u16)((d[2 + i * 2] << 8) | d[3 + i * 2]);
        s = str_from_units(vm, u, k);
        free(u);
        return s;
    }
    return str_utf8(vm, (const char*)d, n);
}

static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* application/x-www-form-urlencoded: '+' is a space, %XX a byte (UTF-8) */
static AStr *form_decode(VM *vm, const u8 *s, size_t n) {
    char *b = (char*)malloc(n + 1); size_t i, o = 0; AStr *r;
    if (!b) return vm->s_empty;
    for (i = 0; i < n; i++) {
        if (s[i] == '+') b[o++] = ' ';
        else if (s[i] == '%' && i + 2 < n + 0 && hexv(s[i + 1]) >= 0 && hexv(s[i + 2]) >= 0) { b[o++] = (char)(hexv(s[i + 1]) * 16 + hexv(s[i + 2])); i += 2; }
        else b[o++] = (char)s[i];
    }
    r = str_utf8(vm, b, o);
    free(b);
    return r;
}

/* form_urlencoded::parse into obj (set or define); returns 0 on a script error */
static int form_into(Act *a, AObj *obj, const u8 *s, size_t n, int define) {
    size_t i = 0;
    while (i < n) {
        size_t e = i, eq; AStr *k, *v;
        while (e < n && s[e] != '&') e++;
        if (e > i) {
            eq = i;
            while (eq < e && s[eq] != '=') eq++;
            k = form_decode(a->vm, s + i, eq - i);
            v = eq < e ? form_decode(a->vm, s + eq + 1, e - eq - 1) : a->vm->s_empty;
            if (define) o_define(a->vm, obj, k, v_str(v), 0);
            else { o_set(a, obj, k, v_str(v)); if (a->vm->err) return 0; }
        }
        i = e + 1;
    }
    return 1;
}

static int sniff(const u8 *d, size_t n) {
    if (n >= 8 && (d[0] == 'F' || d[0] == 'C' || d[0] == 'Z') && d[1] == 'W' && d[2] == 'S') return 1;
    if (n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') return 2;
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8) return 2;
    if (n >= 6 && !memcmp(d, "GIF8", 4)) return 2;
    if (n >= 4 && d[0] == 0x49 && d[1] == 0x49 && d[2] == 0xbc && d[3] == 0x01) return 2;   /* JPEG XR */
    return 0;
}

/* ---------------------------------------------------------------- */
/* clip states                                                      */

/* replace_with_movie: the clip's flags start over (lock root stays) */
static void clip_replace(VM *vm, MovieClip *c, SWFMovie *m, int frames_loaded) {
    A1Clip *s = a1_of(c);
    (void)vm;
    player_clip_replace_movie(c, m);
    if (s) {
        s->removed = 0; s->pending_removal = 0; s->initialized = 0; s->post_inst = 0;
        s->script_placed = 0; s->is_root = 1;
        s->has_fl = frames_loaded != -2; s->fl = frames_loaded;
    }
    if (c->self) { c->self->script_moved = 0; c->self->sr_cached = 0; c->self->script_placed = 0; c->self->visible = 1; }
    if (c->self && !c->parent) c->self->name[0] = 0;     /* a level's script-set _name does not survive */
}

static SWFMovie *empty_movie(Player *p, int version, const char *url) { return player_stub_movie(p, version, url, 1); }

static void load_error_state(VM *vm, MovieClip *c, const char *url) {
    SWFMovie *m = player_stub_movie(vm->player, 0, url, 0);
    if (m) clip_replace(vm, c, m, -1);
}

/* ---------------------------------------------------------------- */
/* events                                                           */

static void broadcast(VM *vm, MLoad *m, const char *ev, int argc, Value *extra) {
    Act a; Value args[4]; MovieClip *base, *t; int i;
    if (!m->bc) return;
    base = m->base ? o_display(m->base) : NULL;
    if (!base) base = vm->player->root;
    act_init_nothing(&a, vm, base, "[MovieClipLoader]");
    args[0] = v_str(str_intern(vm, ev));
    t = o_display(m->target);
    args[1] = t ? v_obj(clip_obj(vm, t)) : v_undef();
    for (i = 0; i < argc && i < 2; i++) args[2 + i] = extra[i];
    o_call_method(&a, m->bc, vm->s_broadcastMessage, 2 + (argc < 2 ? argc : 2), args, ER_SPECIAL);
    if (vm->err) vm_report_error(&a);
}

static void progress(VM *vm, MLoad *m, double cur, double total) {
    Value x[2]; x[0] = v_num(cur); x[1] = v_num(total);
    broadcast(vm, m, "onLoadProgress", 2, x);
}

/* ---------------------------------------------------------------- */
/* processing                                                       */

extern int avm1_use_codepage(void);
static void run_form(VM *vm, LReq *r) {
    Act a; char url[1100]; u8 *d; size_t n = 0; MovieClip *c;
    log_request(vm, r);
    resolve(vm, r->url, url, sizeof(url));
    d = fetch(vm, url, &n);
    if (!d) return;
    act_init_nothing(&a, vm, vm->player->root, "[Form Loader]");
    if (avm1_use_codepage()) {
        /* System.useCodepage: the text is in the system code page */
        extern char *plat_codepage_to_utf8(const u8 *d, size_t n, size_t *outlen);
        size_t ul = 0; char *u = plat_codepage_to_utf8(d, n, &ul);
        if (u) { form_into(&a, r->target, (const u8*)u, ul, 0); free(u); }
        else form_into(&a, r->target, d, n, 0);
    } else if (vm->player->movie && vm->player->movie->version <= 5) {
        AStr *s = str_from_swf(vm, (const char*)d, n, 5); const char *u = str_c(vm, s);
        form_into(&a, r->target, (const u8*)u, strlen(u), 0);
    } else if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) form_into(&a, r->target, d + 3, n - 3, 0);   /* UTF-8 byte order mark */
    else form_into(&a, r->target, d, n, 0);
    free(d);
    if (vm->err) { vm_report_error(&a); return; }
    c = o_display(r->target);
    if (c && clip_is_movieclip(c)) {
        avm1_queue_method(vm, c, r->target, vm->s_onData, 0, NULL);
        avm1_clip_event(vm, c, CE_DATA, 0);
    }
}

static void run_data(VM *vm, LReq *r) {
    Act a; char url[1100]; u8 *d; size_t n = 0; AObj *t = r->target; Value v;
    log_request(vm, r);
    resolve(vm, r->url, url, sizeof(url));
    d = fetch(vm, url, &n);
    act_init_nothing(&a, vm, vm->player->root, "[Loader]");
    if (d) {
        o_set(&a, t, str_intern(vm, "_bytesTotal"), v_num((double)n));
        if (vm->err) { free(d); vm_report_error(&a); return; }
        if (n > 0) { o_set(&a, t, str_intern(vm, "_bytesLoaded"), v_num((double)n)); if (vm->err) { free(d); vm_report_error(&a); return; } }
        v = v_num(200);
        o_call_method(&a, t, str_intern(vm, "onHTTPStatus"), 1, &v, ER_SPECIAL);
        vm->err = 0;
        /* a SWF 5 movie reads the bytes as they are (a UTF-8 byte order mark included) */
        v = n == 0 ? v_undef() : v_str(vm->player->movie && vm->player->movie->version <= 5 && !avm1_use_codepage() ? str_from_swf(vm, (const char*)d, n, 5) : decode_text(vm, d, n));
        free(d);
        o_call_method(&a, t, vm->s_onData, 1, &v, ER_SPECIAL);
        vm->err = 0;
    } else {
        v = v_num(0);
        o_call_method(&a, t, str_intern(vm, "onHTTPStatus"), 1, &v, ER_SPECIAL);
        vm->err = 0;
        v = v_undef();
        o_call_method(&a, t, vm->s_onData, 1, &v, ER_SPECIAL);
        vm->err = 0;
    }
}

/* define the URL's query parameters on the loaded clip (FlashVars) */
static void flashvars(VM *vm, MovieClip *c, const char *url) {
    const char *q = strchr(url, '?'), *h; Act a;
    if (!q || !c->obj) return;
    h = strchr(q, '#');
    act_init_nothing(&a, vm, c, "[FlashVars]");
    form_into(&a, (AObj*)c->obj, (const u8*)q + 1, h ? (size_t)(h - q - 1) : strlen(q + 1), 1);
}

/* ImportAssets / ImportAssets2: load the asset movie, copy the characters it
 * exports under the requested names into the importer's dictionary, then run
 * its DoInitActions in the context of the importing clip */
static int ascii_ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
}
static void preload_tags(VM *vm, MovieClip *c, SWFMovie *m, int depth);
static void add_export(SWFMovie *m, const char *name, int id) {
    if (m->nexports == m->capexports) {
        int nc = m->capexports ? m->capexports * 2 : 64;
        void *ne = realloc(m->exports, (size_t)nc * sizeof(*m->exports));
        if (!ne) return;
        m->exports = ne; m->capexports = nc;
    }
    m->exports[m->nexports].name = name; m->exports[m->nexports].id = id; m->nexports++;
}
static void import_assets(VM *vm, MovieClip *c, SWFMovie *m, const TagRef *t, int depth) {
    const u8 *b = t->data; size_t n = t->len, pos, ul; int count, i, err = 0;
    char url[1100]; u8 *d; size_t dn = 0; SWFMovie *im;
    if (depth > 8) return;
    for (ul = 0; ul < n && b[ul]; ul++) ;
    if (ul >= n || ul == 0) return;          /* Flash does not load an empty URL */
    pos = ul + 1;
    if (t->code == TAG_ImportAssets2) pos += 2;
    if (pos + 2 > n) return;
    count = b[pos] | (b[pos + 1] << 8); pos += 2;
    {
        char rel[1024]; size_t k = ul < sizeof(rel) - 1 ? ul : sizeof(rel) - 1;
        memcpy(rel, b, k); rel[k] = 0;
        avm1_log_fetch(vm, str_intern(vm, rel), 0, NULL, NULL, 0);
        fetch_url_resolve(fetch_movie_url(m), rel, url, sizeof(url));
    }
    d = fetch(vm, url, &dn);
    if (!d) return;
    if (sniff(d, dn) != 1) { free(d); return; }
    im = swf_load_mem(d, dn, &err);
    if (im && vm->player->avm2) im->a1mix = 1;
    free(d);
    if (!im) return;
    im->url = (char*)malloc(strlen(url) + 1);
    if (im->url) strcpy(im->url, url);
    { extern void player_keep_movie(Player *p, SWFMovie *mv); player_keep_movie(vm->player, im); }
    for (i = 0; i < count && pos + 2 < n; i++) {
        int id = b[pos] | (b[pos + 1] << 8), e; const char *name = (const char*)b + pos + 2;
        size_t nl = strlen(name);
        pos += 2 + nl + 1;
        for (e = 0; e < im->nexports; e++) {
            SWFChar *src;
            if (!ascii_ieq(im->exports[e].name, name)) continue;
            src = swf_get_char(im, im->exports[e].id);
            if (!src) break;
            if (id > m->maxid) {
                SWFChar *nd = (SWFChar*)realloc(m->dict, (size_t)(id + 1) * sizeof(SWFChar));
                if (!nd) break;
                memset(nd + m->maxid + 1, 0, (size_t)(id - m->maxid) * sizeof(SWFChar));
                m->dict = nd; m->maxid = id;
            }
            if (m->dict[id].type == CHAR_NONE) {
                m->dict[id] = *src;
                m->dict[id].id = id; m->dict[id].parsed = NULL; m->dict[id].flags = 0;
                if (!m->dict[id].movie) m->dict[id].movie = im;
                add_export(m, name, id);
            }
            break;
        }
    }
    preload_tags(vm, c, im, depth + 1);
}

/* the tags a movie runs while it loads: DoInitAction and imports */
static void preload_tags(VM *vm, MovieClip *c, SWFMovie *m, int depth) {
    int i;
    for (i = 0; i < m->root.ntags; i++) {
        const TagRef *t = &m->root.tags[i];
        if (t->code == TAG_DoInitAction && t->len > 2) {
            int id = t->data[0] | (t->data[1] << 8);
            SWFChar *sc = swf_get_char(m, id);
            if (m->has_abc) continue;   /* an ActionScript 3 movie's AVM1 code never runs */
            if (sc) { if (sc->flags & 1) continue; sc->flags |= 1; }
            avm1_run_init_action(vm->player, c, t->data + 2, t->len - 2);
            vm->sp = 0;
            avm1_run_actions(vm);
        } else if (t->code == TAG_ImportAssets || t->code == TAG_ImportAssets2) {
            import_assets(vm, c, m, t, depth);
        }
    }
}
static void run_init_actions(VM *vm, MovieClip *c, SWFMovie *m) { preload_tags(vm, c, m, 0); }
void avm1_preload_movie(VM *vm, MovieClip *c, SWFMovie *m) { preload_tags(vm, c, m, 0); }
void avm1_import_assets(VM *vm, MovieClip *c, SWFMovie *m, const TagRef *t) { import_assets(vm, c, m, t, 0); }

static void run_movie(VM *vm, LReq *r) {
    Loads *L = loads_of(vm); MLoad *m; MovieClip *c; char url[1100]; u8 *d; size_t n = 0; int kind, is_mc;
    if (!L || r->ml < 0 || r->ml >= L->nml) return;
    m = &L->ml[r->ml];
    c = o_display(m->target);
    if (!c || clip_removed(c)) { m->status = ML_FAILED; return; }
    log_request(vm, r);
    resolve(vm, r->url, url, sizeof(url));
    is_mc = clip_is_movieclip(c) && !(c->movie && c->movie->has_abc);
    /* start: unload the old content, clear the clip's properties, enter the loading state */
    if (is_mc) {
        avm1_unload_clip(vm, c);
        if (c->obj && clip_swf_version(c) > 0) {
            Act a; int k, nk = 0; AStr **keys;
            act_init_nothing(&a, vm, c, "unknown");
            keys = o_keys(&a, (AObj*)c->obj, 1, &nk);
            for (k = 0; keys && k < nk; k++) { o_delete(&a, (AObj*)c->obj, keys[k]); vm->err = 0; }
            free(keys);
        }
        {
            SWFMovie *em = empty_movie(vm->player, clip_swf_version(c), url);
            if (em) clip_replace(vm, c, em, 0);
        }
    }
    broadcast(vm, m, "onLoadStart", 0, NULL);
    avm1_run_actions(vm);
    d = fetch(vm, url, &n);
    m = &L->ml[r->ml];
    c = o_display(m->target);
    if (!d) {
        Value x[1];
        if (c && clip_is_movieclip(c)) load_error_state(vm, c, url);
        x[0] = v_str(str_intern(vm, "LoadNeverCompleted"));
        broadcast(vm, m, "onLoadError", 1, x);
        L->ml[r->ml].status = ML_FAILED;
        avm1_run_actions(vm);
        return;
    }
    if (!c || clip_removed(c)) { free(d); m->status = ML_FAILED; return; }
    kind = sniff(d, n);
    if (kind == 1) {
        int err = 0; SWFMovie *nm = swf_load_mem(d, n, &err); int replacing_root = c == vm->player->root;
        free(d);
        if (!nm) return;
        if (replacing_root && nm->has_abc && !vm->player->avm2) {   /* the stage switches to the AVM2 movie after this frame */
            nm->url = (char*)malloc(strlen(url) + 1);
            if (nm->url) strcpy(nm->url, url);
            { extern void player_keep_movie(Player *p, SWFMovie *mv); player_keep_movie(vm->player, nm); }
            vm->player->swap_root = nm;
            m->status = ML_FAILED;
            return;
        }
        if (vm->player->avm2) nm->a1mix = 1;   /* AVM1 content of an AVM1Movie */
        nm->url = (char*)malloc(strlen(url) + 1);
        if (nm->url) strcpy(nm->url, url);
        { extern void player_keep_movie(Player *p, SWFMovie *mv); player_keep_movie(vm->player, nm); }
        m->status = ML_PARSING; m->movie = nm;
        if (is_mc) {
            clip_replace(vm, c, nm, -2);
            if (c->obj) {
                Act a; Value mc;
                act_init_nothing(&a, vm, c, "[Resolve]");
                if (a1_resolve_proto(&a, "MovieClip", &mc)) o_define(vm, (AObj*)c->obj, vm->s_proto, mc, DE | DD);
            }
            run_init_actions(vm, c, nm);
        }
        /* preload done: progress, then completion */
        progress(vm, m, (double)nm->file_len, (double)nm->file_len);
        m = &L->ml[r->ml];
        if (replacing_root) progress(vm, m, (double)nm->file_len, (double)nm->file_len);
        else if (is_mc && (c = o_display(m->target)) != NULL) {
            avm1_post_instantiation_ex(vm, c, NULL, 0, 0);
            flashvars(vm, c, url);
        }
        { Value x[1]; x[0] = v_num(0); broadcast(vm, &L->ml[r->ml], "onLoadComplete", 1, x); }
        L->ml[r->ml].status = ML_OK;
    } else if (kind == 2) {
        /* an image: a one-frame movie showing the bitmap (TODO: the bitmap itself) */
        extern Bitmap *image_decode_bytes(const u8 *d, size_t n);
        extern void a1_show_image(VM *vm, MovieClip *c, BitmapData *b);
        SWFMovie *im = player_stub_movie(vm->player, 0, url, 1);
        Bitmap *bm = image_decode_bytes(d, n);
        free(d);
        if (im) im->file_len = n;
        progress(vm, m, (double)n, (double)n);
        m = &L->ml[r->ml];
        c = o_display(m->target);
        if (im && c && clip_is_movieclip(c)) {
            clip_replace(vm, c, im, 1); c->cur_frame = 0;
            if (bm) { BitmapData *b = bmd_from_straight(bm, 1); if (b) { a1_show_image(vm, c, b); bmd_unref(b); } }
        }
        bitmap_free(bm);
        { Value x[1]; x[0] = v_num(0); broadcast(vm, m, "onLoadComplete", 1, x); }
        L->ml[r->ml].status = ML_OK;
    } else {
        free(d);
        if (is_mc) load_error_state(vm, c, url);
        progress(vm, m, 0, 0);
        { Value x[1]; x[0] = v_num(0); broadcast(vm, &L->ml[r->ml], "onLoadComplete", 1, x); }
        L->ml[r->ml].status = ML_OK;
    }
    avm1_run_actions(vm);
}

static void unloaded_state(VM *vm, MovieClip *c) {
    MovieClip *par = c->parent; SWFMovie *m;
    /* the unloaded state has no frames at all */
    if (par) m = player_stub_movie(vm->player, par->movie ? par->movie->version : vm->swf_version, avm1_movie_url(vm->player, par->movie), 0);
    else m = player_stub_movie(vm->player, c->movie ? c->movie->version : vm->swf_version, NULL, 0);
    if (m) { int root = c->a1 ? c->a1->is_root : 0; clip_replace(vm, c, m, 0); if (c->a1) c->a1->is_root = (u8)root; }
}

static void run_unload(VM *vm, LReq *r) {
    MovieClip *c = o_display(r->target);
    if (!c) return;
    avm1_unload_clip(vm, c);
    unloaded_state(vm, c);
    avm1_run_actions(vm);
}

/* the end of the frame: complete the loads requested so far (and the ones
 * their handlers start) */
/* StyleSheet.load: parse(text) then onLoad(success) */
static void run_css(VM *vm, LReq *r) {
    Act a; char url[1100]; u8 *d; size_t n = 0; Value v, ok;
    log_request(vm, r);
    resolve(vm, r->url, url, sizeof(url));
    d = fetch(vm, url, &n);
    act_init_nothing(&a, vm, vm->player->root, "[Loader]");
    if (d) {
        v = v_str(str_utf8(vm, (const char*)d, n));
        free(d);
        ok = o_call_method(&a, r->target, str_intern(vm, "parse"), 1, &v, ER_SPECIAL);
        if (vm->err) { vm->err = 0; ok = v_bool(0); }
    } else ok = v_bool(0);
    o_call_method(&a, r->target, str_intern(vm, "onLoad"), 1, &ok, ER_SPECIAL);
    vm->err = 0;
}
void avm1_load_css(Act *a, AObj *target, AStr *url) { push_req(a->vm, RQ_CSS, target, url, -1); }

void avm1_loads_run(VM *vm) {
    Loads *L = (Loads*)vm->loads; int guard = 0;
    if (!L) return;
    while (L->head < L->nrq && guard++ < 10000) {
        LReq r = L->rq[L->head++];
        switch (r.kind) {
        case RQ_FORM: run_form(vm, &r); break;
        case RQ_DATA: run_data(vm, &r); break;
        case RQ_MOVIE: run_movie(vm, &r); break;
        case RQ_UNLOAD: run_unload(vm, &r); break;
        case RQ_CSS: run_css(vm, &r); break;
        }
        free(r.body);
        vm->err = 0;
        avm1_run_actions(vm);
    }
    L->nrq = L->head = 0;
}

/* after the execution list: onLoadInit for the movies that ran their first frame */
void avm1_loads_frame(VM *vm) {
    Loads *L = (Loads*)vm->loads; int i, j, n = 0, order[256];
    if (!L) return;
    for (i = 0; i < L->nml && n < 256; i++) if (L->ml[i].used) order[n++] = i;
    /* newest first */
    for (i = 1; i < n; i++) { int k = order[i]; j = i - 1; while (j >= 0 && L->ml[order[j]].seq < L->ml[k].seq) { order[j + 1] = order[j]; j--; } order[j + 1] = k; }
    for (i = 0; i < n; i++) {
        MLoad *m = &L->ml[order[i]];
        if (m->status == ML_FAILED) { m->used = 0; continue; }
        if (m->status != ML_OK) continue;
        if (m->bc) {
            Value args[2]; MovieClip *t = o_display(m->target), *base = m->base ? o_display(m->base) : NULL;
            args[0] = v_str(str_intern(vm, "onLoadInit"));
            args[1] = t ? v_obj(clip_obj(vm, t)) : v_undef();
            avm1_queue_method(vm, base ? base : vm->player->root, m->bc, vm->s_broadcastMessage, 2, args);
        }
        m->used = 0;
    }
}

/* ---------------------------------------------------------------- */
/* entry points used by the interpreter and the classes             */

static AObj *base_obj(Act *a) { return a->base_clip ? clip_obj(a->vm, a->base_clip) : NULL; }

void avm1_load_movie_vars(Act *a, MovieClip *target, AStr *url, int method, AObj *vars) {
    AObj *t; int ml;
    if (!target) return;
    if (a->vm->player->avm2 && target->a1 && target->a1->is_root && CLIP_A1MIX(target) && !(target->parent && CLIP_A1MIX(target->parent))) return;   /* the root of an AVM1Movie keeps its movie */
    t = clip_obj(a->vm, target);
    if (!t) return;
    ml = new_loader(a->vm, t, NULL, base_obj(a));
    if (ml >= 0) req_with_form(a, push_req(a->vm, RQ_MOVIE, t, url, ml), method, vars ? vars : locals_of(a));
}
void avm1_load_movie(Act *a, MovieClip *target, AStr *url, int method) { avm1_load_movie_vars(a, target, url, method, NULL); }

void avm1_unload_movie(Act *a, MovieClip *c) {
    VM *vm = a->vm;
    if (!c) return;
    if ((c->a1 && c->a1->is_root) || c == vm->player->root) { AObj *t = clip_obj(vm, c); if (t) push_req(vm, RQ_UNLOAD, t, NULL, -1); return; }
    avm1_unload_clip(vm, c);
    unloaded_state(vm, c);
}

void avm1_load_vars_into_vars(Act *a, AObj *target, AStr *url, int method, AObj *vars) {
    if (target) req_with_form(a, push_req(a->vm, RQ_FORM, target, url, -1), method, vars ? vars : locals_of(a));
}
void avm1_load_vars_into(Act *a, AObj *target, AStr *url, int method) { avm1_load_vars_into_vars(a, target, url, method, NULL); }
int avm1_method_of(Act *a, AStr *s) { return method_of(a, s); }

/* XML.load / sendAndLoad: `post` is the document sent */
int avm1_load_vars(Act *a, AObj *target, AStr *url, AStr *post, int is_xml) {
    LReq *r;
    if (!target) return 0;
    if (is_xml && a->swf_version >= 8) {
        /* SWF 8: a new XML load replaces the one still waiting (Gnash XML.as: one onLoad) */
        Loads *L = loads_of(a->vm); int i;
        if (L) for (i = L->head; i < L->nrq; i++) if (L->rq[i].kind == RQ_DATA && L->rq[i].target == target) L->rq[i].kind = -1;
    }
    r = push_req(a->vm, RQ_DATA, target, url, -1);
    if (r && post) { r->post = 1; r->body = dupz(str_c(a->vm, post)); r->blen = r->body ? strlen(r->body) : 0; r->mime = "application/x-www-form-urlencoded"; }
    return 1;
}

/* getBytesLoaded / getBytesTotal of a clip */
double avm1_clip_bytes_total(MovieClip *c) {
    if (c->movie && c->movie->incomplete && c->movie->body && c->tl == &c->movie->root) return (double)c->movie->hdr_len;   /* still loading: the length the header promises */
    if (c->a1 && c->a1->is_root) {
        if (!c->movie) return 0;
        if (!c->movie->body) return c->movie->file_len ? (double)c->movie->file_len : c->movie->version == 0 ? -1 : 0;
        return (double)(c->movie->bodylen + 8);
    }
    if (c->movie && !c->movie->body && c->movie != c->player->movie) return 0;   /* replaced by an empty movie (unloaded) */
    if (c->self && c->self->ch && c->self->ch->type == CHAR_SPRITE) return c->self->ch->len >= 4 ? (double)(c->self->ch->len - 4) : 0.0;   /* the tag stream after the sprite header */
    if (c == c->player->root || !c->self) return (double)(c->movie && c->movie->body ? c->movie->bodylen + 8 : 0);
    return 0;
}

/* ---------------------------------------------------------------- */
/* MovieClipLoader                                                  */

static MovieClip *mcl_target(Act *a, Value t, int create_level) {
    switch (t.t) {
    case V_STR: return act_resolve_target_display(a, act_target_or_root(a), t, 1);
    case V_NUM: return create_level ? avm1_level_create(a->vm->player, (int)t.u.n, a->base_clip ? a->base_clip->movie : NULL) : act_get_level(a, (int)t.u.n);
    case V_OBJ: return o_display(t.u.o);
    case V_CLIP: { AObj *o = v_to_obj_or_bare(a, t); return o ? o_display(o) : NULL; }
    default: return NULL;
    }
}

static Value mcl_ctor(Act *a, AObj *self, int argc, Value *argv) {
    Value me = v_obj(self); (void)argc; (void)argv;
    o_define(a->vm, self, a->vm->s__listeners, v_obj(obj_array(a, 1, &me)), DE);
    return v_undef();
}

static Value mcl_load_clip(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c; AObj *t; int ml;
    if (argc < 2) return v_undef();
    if (argv[0].t != V_STR) return v_bool(0);
    c = mcl_target(a, argv[1], 1);
    if (a->vm->err) return v_undef();
    if (!c) return v_bool(0);
    t = clip_obj(a->vm, c);
    if (!t) return v_bool(0);
    ml = new_loader(a->vm, t, self, base_obj(a));
    if (ml >= 0) push_req(a->vm, RQ_MOVIE, t, argv[0].u.s, ml);
    return v_bool(1);
}

static Value mcl_unload_clip(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c; (void)self;
    if (argc < 1) return v_undef();
    c = mcl_target(a, argv[0], 0);
    if (a->vm->err) return v_undef();
    if (!c) return v_bool(0);
    if (clip_is_movieclip(c)) avm1_unload_movie(a, c);
    else avm1_unload_clip(a->vm, c);
    return v_bool(1);
}

static Value mcl_get_progress(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c; AObj *r; Value t; (void)self;
    if (argc < 1) return v_undef();
    t = argv[0];
    if (t.t == V_OBJ && !o_display(t.u.o)) return v_undef();
    if (t.t != V_STR && t.t != V_NUM && t.t != V_OBJ && t.t != V_CLIP) return v_undef();
    c = mcl_target(a, t, 0);
    if (a->vm->err) return v_undef();
    r = obj_bare(a->vm);
    if (c) {
        double n = c->movie ? (double)c->movie->file_len : 0;
        o_define(a->vm, r, str_intern(a->vm, "bytesLoaded"), v_num(n), 0);
        o_define(a->vm, r, str_intern(a->vm, "bytesTotal"), v_num(n), 0);
    }
    return v_obj(r);
}

static void defm(VM *vm, Env *env, AObj *o, const char *name, NativeFn f, u16 attr) { o_define(vm, o, str_intern(vm, name), v_obj(fn_native(vm, env, f, NULL)), attr); }

AObj *lib_mcl(VM *vm, Env *env, AObj *object_proto) {
    AObj *proto, *c = lib_class(vm, env, NULL, mcl_ctor, object_proto, 1, &proto);
    env->p.mcl = proto;
    defm(vm, env, proto, "loadClip", mcl_load_clip, DE | DD);
    defm(vm, env, proto, "getProgress", mcl_get_progress, DE | DD);
    defm(vm, env, proto, "unloadClip", mcl_unload_clip, DE | DD);
    return c;
}

/* ---------------------------------------------------------------- */
/* LoadVars                                                         */

static void define_or_set(Act *a, AObj *o, const char *name, Value v) {
    AStr *k = str_intern(a->vm, name);
    if (!o_has_property(a, o, k)) o_define(a->vm, o, k, v, DE | DD);
    else o_set(a, o, k, v);
}

static int lv_fetch_send(Act *a, AObj *loader, AStr *url, AObj *send, int method) {
    LReq *r = push_req(a->vm, RQ_DATA, loader, url, -1);
    if (send) req_with_form(a, r, method, send);
    define_or_set(a, loader, "_bytesLoaded", v_num(0)); if (a->vm->err) return 0;
    define_or_set(a, loader, "_bytesTotal", v_undef()); if (a->vm->err) return 0;
    define_or_set(a, loader, "loaded", v_bool(0)); if (a->vm->err) return 0;
    return 1;
}

static Value lv_load(Act *a, AObj *self, int argc, Value *argv) {
    AStr *url;
    if (argc < 1) return v_bool(0);
    url = v_str_of(a, argv[0]); if (a->vm->err) return v_undef();
    if (!lv_fetch_send(a, self, url, NULL, 0)) return v_undef();
    return v_bool(1);
}

static Value lv_send_and_load(Act *a, AObj *self, int argc, Value *argv) {
    AStr *url, *ms; Value t = arg(argc, argv, 1); int method;
    url = v_str_of(a, arg(argc, argv, 0)); if (a->vm->err) return v_undef();
    if (t.t != V_OBJ) return v_bool(0);
    ms = v_str_of(a, arg(argc, argv, 2)); if (a->vm->err) return v_undef();
    method = method_of(a, ms); if (!method) method = 2;
    if (!lv_fetch_send(a, t.u.o, url, self, method)) return v_undef();
    return v_bool(1);
}

static Value lv_send(Act *a, AObj *self, int argc, Value *argv) {
    AStr *url, *win = a->vm->s_empty, *ms; int method;
    if (argc < 1) return v_bool(0);
    url = v_str_of(a, argv[0]); if (a->vm->err) return v_undef();
    if (argc > 1) { win = v_str_of(a, argv[1]); if (a->vm->err) return v_undef(); }
    ms = v_str_of(a, arg(argc, argv, 2)); if (a->vm->err) return v_undef();
    method = method_of(a, ms); if (!method) method = 2;
    avm1_navigate(a, url, win, method, self, 0);
    return v_bool(1);
}

static Value lv_decode(Act *a, AObj *self, int argc, Value *argv) {
    AStr *s; const char *u;
    if (argc < 1) return v_bool(0);
    s = v_str_of(a, argv[0]); if (a->vm->err) return v_undef();
    u = str_c(a->vm, s);
    form_into(a, self, (const u8*)u, strlen(u), 0);
    return v_undef();
}

static Value lv_get_bytes_loaded(Act *a, AObj *self, int argc, Value *argv) { (void)argc; (void)argv; return o_get(a, self, str_intern(a->vm, "_bytesLoaded")); }
static Value lv_get_bytes_total(Act *a, AObj *self, int argc, Value *argv) { (void)argc; (void)argv; return o_get(a, self, str_intern(a->vm, "_bytesTotal")); }

/* escape() as the script sees it (_global.escape can be replaced) */
static AStr *lv_escape(Act *a, AObj *esc, AStr *s) {
    Value v, r;
    if (!esc) return escape_str(a->vm, s);
    v = v_str(s);
    r = o_call(a, esc, v_undef(), 1, &v); if (a->vm->err) return NULL;
    return v_str_of(a, r);
}

static Value lv_to_string(Act *a, AObj *self, int argc, Value *argv) {
    int n = 0, i; AStr **keys = o_keys(a, self, 0, &n); SBuf b; AObj *esc = NULL; (void)argc; (void)argv;
    {
        Value ev = o_get(a, ENV(a)->global, str_intern(a->vm, "escape"));
        if (a->vm->err) { free(keys); return v_undef(); }
        if (ev.t == V_OBJ && o_as_function(ev.u.o)) esc = ev.u.o;
    }
    sb_init(&b);
    for (i = 0; i < n; i++) {
        Value v = o_get(a, self, keys[i]); AStr *s, *ek, *es;
        if (a->vm->err) break;
        s = v_str_of(a, v); if (a->vm->err) break;
        ek = lv_escape(a, esc, keys[i]); if (a->vm->err) break;
        es = lv_escape(a, esc, s); if (a->vm->err) break;
        if (i) sb_unit(&b, '&');
        sb_str(&b, ek);
        sb_unit(&b, '=');
        sb_str(&b, es);
    }
    free(keys);
    if (a->vm->err) { sb_free(&b); return v_undef(); }
    return v_str(sb_done(a->vm, &b));
}

static Value lv_on_data(Act *a, AObj *self, int argc, Value *argv) {
    Value v = arg(argc, argv, 0), ok;
    if (v.t == V_UNDEF || v.t == V_NULL) ok = v_bool(0);
    else {
        o_call_method(a, self, str_intern(a->vm, "decode"), 1, &v, ER_FUNCTION);
        if (a->vm->err) return v_undef();
        o_set(a, self, str_intern(a->vm, "loaded"), v_bool(1));
        if (a->vm->err) return v_undef();
        ok = v_bool(1);
    }
    o_call_method(a, self, a->vm->s_onLoad, 1, &ok, ER_FUNCTION);
    return v_undef();
}

static Value lv_noop(Act *a, AObj *self, int argc, Value *argv) { (void)a; (void)self; (void)argc; (void)argv; return v_undef(); }

AObj *lib_loadvars(VM *vm, Env *env, AObj *object_proto) {
    AObj *proto, *c = lib_class(vm, env, NULL, NULL, object_proto, 1, &proto); u16 at = DE | DD;
    env->p.loadvars = proto;
    defm(vm, env, proto, "load", lv_load, at);
    defm(vm, env, proto, "send", lv_send, at);
    defm(vm, env, proto, "sendAndLoad", lv_send_and_load, at);
    defm(vm, env, proto, "decode", lv_decode, at);
    defm(vm, env, proto, "getBytesLoaded", lv_get_bytes_loaded, at);
    defm(vm, env, proto, "getBytesTotal", lv_get_bytes_total, at);
    defm(vm, env, proto, "toString", lv_to_string, at);
    o_define(vm, proto, str_intern(vm, "contentType"), v_str(str_intern(vm, "application/x-www-form-urlencoded")), at);
    defm(vm, env, proto, "onLoad", lv_noop, at);
    defm(vm, env, proto, "onData", lv_on_data, at);
    defm(vm, env, proto, "addRequestHeader", lv_noop, at);
    return c;
}
