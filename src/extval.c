/* extval.c - ExternalInterface values (see extval.h). */
#include "extval.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

ExtProviderFn g_ext_provider = NULL;

static char *xdup(const char *s) { size_t n = s ? strlen(s) : 0; char *d = (char*)malloc(n + 1); if (d) { if (n) memcpy(d, s, n); d[n] = 0; } return d; }

void xv_free(ExtVal *v) {
    int i;
    if (!v) return;
    free(v->s);
    for (i = 0; i < v->count; i++) { xv_free(&v->items[i]); if (v->keys) free(v->keys[i]); }
    free(v->items); free(v->keys);
    memset(v, 0, sizeof(*v));
}
ExtVal xv_undef(void) { ExtVal v; memset(&v, 0, sizeof(v)); v.t = XV_UNDEF; return v; }
ExtVal xv_null(void) { ExtVal v = xv_undef(); v.t = XV_NULL; return v; }
ExtVal xv_bool(int b) { ExtVal v = xv_undef(); v.t = XV_BOOL; v.b = b != 0; return v; }
ExtVal xv_num(double n) { ExtVal v = xv_undef(); v.t = XV_NUM; v.n = n; return v; }
ExtVal xv_str(const char *s) { ExtVal v = xv_undef(); v.t = XV_STR; v.s = xdup(s); return v; }
ExtVal xv_copy(const ExtVal *v) {
    ExtVal c = *v; int i;
    c.s = v->s ? xdup(v->s) : NULL; c.items = NULL; c.keys = NULL; c.count = 0;
    if (v->count) {
        c.items = (ExtVal*)malloc((size_t)v->count * sizeof(ExtVal));
        if (v->keys) c.keys = (char**)malloc((size_t)v->count * sizeof(char*));
        if (!c.items || (v->keys && !c.keys)) { free(c.items); free(c.keys); c.items = NULL; c.keys = NULL; return c; }
        for (i = 0; i < v->count; i++) { c.items[i] = xv_copy(&v->items[i]); if (c.keys) c.keys[i] = xdup(v->keys[i]); }
        c.count = v->count;
    }
    return c;
}
void xv_list_push(ExtVal *list, ExtVal v) {
    ExtVal *ni = (ExtVal*)realloc(list->items, (size_t)(list->count + 1) * sizeof(ExtVal));
    if (!ni) { xv_free(&v); return; }
    list->items = ni; list->items[list->count++] = v;
}
void xv_obj_put(ExtVal *obj, const char *key, ExtVal v) {
    int i, at;
    ExtVal *ni; char **nk;
    for (i = 0; i < obj->count; i++) if (!strcmp(obj->keys[i], key)) { xv_free(&obj->items[i]); obj->items[i] = v; return; }
    ni = (ExtVal*)realloc(obj->items, (size_t)(obj->count + 1) * sizeof(ExtVal));
    if (!ni) { xv_free(&v); return; }
    obj->items = ni;
    nk = (char**)realloc(obj->keys, (size_t)(obj->count + 1) * sizeof(char*));
    if (!nk) { xv_free(&v); return; }
    obj->keys = nk;
    for (at = 0; at < obj->count && strcmp(obj->keys[at], key) < 0; at++) {}
    memmove(obj->items + at + 1, obj->items + at, (size_t)(obj->count - at) * sizeof(ExtVal));
    memmove(obj->keys + at + 1, obj->keys + at, (size_t)(obj->count - at) * sizeof(char*));
    obj->items[at] = v; obj->keys[at] = xdup(key); obj->count++;
}

/* ---- Rust Debug output ---- */

typedef struct { char *p; size_t n, cap; } XB;
static void xb_put(XB *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) { size_t nc = (b->n + n + 1) * 2; char *np = (char*)realloc(b->p, nc); if (!np) return; b->p = np; b->cap = nc; }
    memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}
static void xb_s(XB *b, const char *s) { xb_put(b, s, strlen(s)); }
static void xb_quoted(XB *b, const char *s) {
    xb_s(b, "\"");
    for (; s && *s; s++) {
        char e[8];
        switch (*s) {
        case '"': xb_s(b, "\\\""); break;
        case '\\': xb_s(b, "\\\\"); break;
        case '\n': xb_s(b, "\\n"); break;
        case '\r': xb_s(b, "\\r"); break;
        case '\t': xb_s(b, "\\t"); break;
        case '\0': xb_s(b, "\\0"); break;
        default:
            if ((u8)*s < 0x20 || *s == 0x7f) { sprintf(e, "\\u{%x}", (u8)*s); xb_s(b, e); }
            else xb_put(b, s, 1);
        }
    }
    xb_s(b, "\"");
}
/* f64 Debug: shortest round-trip digits, ".0" on integral values */
static void xb_f64(XB *b, double d) {
    char buf[64]; int prec;
    if (d != d) { xb_s(b, "NaN"); return; }
    if (d == HUGE_VAL) { xb_s(b, "inf"); return; }
    if (d == -HUGE_VAL) { xb_s(b, "-inf"); return; }
    if (d == floor(d) && fabs(d) < 1e16) { sprintf(buf, "%.1f", d); xb_s(b, buf); return; }
    for (prec = 1; prec <= 17; prec++) { sprintf(buf, "%.*g", prec, d); if (strtod(buf, NULL) == d) break; }
    if (strchr(buf, 'e')) {
        /* Rust writes 1e-7 style without zero padding: 1e-7, 1.5e20 */
        char m[64], *e = strchr(buf, 'e'); int ex = atoi(e + 1);
        memcpy(m, buf, (size_t)(e - buf)); m[e - buf] = 0;
        sprintf(buf, "%se%d", m, ex);
    }
    xb_s(b, buf);
}
static void xb_val(XB *b, const ExtVal *v) {
    int i;
    switch (v->t) {
    case XV_UNDEF: xb_s(b, "Undefined"); break;
    case XV_NULL: xb_s(b, "Null"); break;
    case XV_BOOL: xb_s(b, v->b ? "Bool(true)" : "Bool(false)"); break;
    case XV_NUM: xb_s(b, "Number("); xb_f64(b, v->n); xb_s(b, ")"); break;
    case XV_STR: xb_s(b, "String("); xb_quoted(b, v->s); xb_s(b, ")"); break;
    case XV_LIST:
        xb_s(b, "List([");
        for (i = 0; i < v->count; i++) { if (i) xb_s(b, ", "); xb_val(b, &v->items[i]); }
        xb_s(b, "])"); break;
    case XV_OBJ:
        xb_s(b, "Object({");
        for (i = 0; i < v->count; i++) { if (i) xb_s(b, ", "); xb_quoted(b, v->keys[i]); xb_s(b, ": "); xb_val(b, &v->items[i]); }
        xb_s(b, "})"); break;
    }
}
char *xv_debug(const ExtVal *v) {
    XB b; b.p = NULL; b.n = b.cap = 0;
    xb_val(&b, v);
    if (!b.p) return xdup("");
    return b.p;
}
