/* avm2_fte.c - natives of flash.text.engine (the classes themselves are
 * ActionScript, from Ruffle's playerglobal; see as3/).  Follows Ruffle's
 * avm2/globals/flash/text/engine/*.rs: ElementFormat / FontDescription /
 * TextBlock properties, content elements, and TextBlock.createTextLine, which
 * lays each line out (up to the next line break) with a text field state
 * held by the TextLine itself. */
#include "avm2_flash_int.h"
#include "richtext.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

extern V2 a2_vector_apply(VM2 *vm, V2 base, int argc, V2 *argv);
extern void a2_fte_line_setup(VM2 *vm, A2Obj *o, A2Str *text, const RTFmt *f, double width);
extern void a2_fte_line_measure(VM2 *vm, A2Obj *o, double *tw, double *th, double *asc, double *desc);
extern void a2_report_uncaught(VM2 *vm);

static V2 hget(VM2 *vm, V2 self, const char *k, V2 def) { A2Prop *p = self.t == T_OBJ ? a2_dyn_find(self.u.o, a2_str(vm, k)) : NULL; return p ? p->v : def; }
static void hset(VM2 *vm, V2 self, const char *k, V2 v) { if (self.t == T_OBJ) a2_dyn_set_hidden(vm, self.u.o, a2_str(vm, k), v); }
static A2Obj *hobj(VM2 *vm, V2 self, const char *k) { V2 v = hget(vm, self, k, v2_null()); return v.t == T_OBJ ? v.u.o : NULL; }
static double hnum(VM2 *vm, V2 self, const char *k, double def) { V2 v = hget(vm, self, k, v2_num(def)); return v2_tonum(vm, v); }
static int non_null(VM2 *vm, V2 v, const char *name) { if (v.t == T_NULL || v.t == T_UNDEF) { a2_throw_id(vm, "TypeError", 2007, name, NULL); return 0; } return 1; }
static void err2004(VM2 *vm) { a2_throw_id(vm, "ArgumentError", 2004, NULL, NULL); }
static void err2006(VM2 *vm) { a2_throw_id(vm, "RangeError", 2006, NULL, NULL); }
static A2Class *cls(VM2 *vm, const char *n) { return a2_class_find_s(vm, n); }
static int is_a(VM2 *vm, V2 v, const char *cn) { A2Class *c = cls(vm, cn); return v.t == T_OBJ && c && v.u.o->cls && a2_is_subclass(v.u.o->cls, c); }

/* ---------------------------------------------------------------- */
/* table-driven properties                                          */

enum { FP_ENUM, FP_NUM, FP_UINT, FP_BOOL, FP_STR, FP_INTNN, FP_FD, FP_FDNULL };
enum { F_NAN0 = 1, F_CLAMP01 = 2, F_NONNEG = 4 };
typedef struct {
    const char *key;            /* native key minus "get "/"set " */
    const char *name;           /* property / parameter name */
    int type, flags;
    const char *const *vals;    /* FP_ENUM: accepted values */
    const char *exclude;        /* FP_ENUM: a value of the list the setter refuses */
    const char *def_s; double def_n;
    char store[100];
} FProp;

static const char *const v_baseline[] = { "ascent", "descent", "ideographicBottom", "ideographicCenter", "ideographicTop", "roman", "useDominantBaseline", NULL };
static const char *const v_breakop[] = { "all", "any", "auto", "none", NULL };
static const char *const v_digitcase[] = { "default", "lining", "oldStyle", NULL };
static const char *const v_digitwidth[] = { "default", "proportional", "tabular", NULL };
static const char *const v_kerning[] = { "auto", "off", "on", NULL };
static const char *const v_ligature[] = { "common", "exotic", "minimum", "none", "uncommon", NULL };
static const char *const v_rotation[] = { "auto", "rotate0", "rotate180", "rotate270", "rotate90", NULL };
static const char *const v_typocase[] = { "caps", "capsAndSmallCaps", "default", "lowercase", "smallCaps", "title", "uppercase", NULL };
static const char *const v_weight[] = { "normal", "bold", NULL };
static const char *const v_posture[] = { "normal", "italic", NULL };
static const char *const v_lookup[] = { "device", "embeddedCFF", NULL };
static const char *const v_rendering[] = { "normal", "cff", NULL };
static const char *const v_hinting[] = { "none", "horizontalStem", NULL };

#define EF "flash.text.engine::ElementFormat/"
#define FD "flash.text.engine::FontDescription/"
#define TB "flash.text.engine::TextBlock/"
#define CE "flash.text.engine::ContentElement/"
static FProp g_props[] = {
    { EF "alignmentBaseline", "alignmentBaseline", FP_ENUM, 0, v_baseline, NULL, "useDominantBaseline", 0, "" },
    { EF "alpha", "alpha", FP_NUM, F_NAN0 | F_CLAMP01, NULL, NULL, NULL, 1, "" },
    { EF "baselineShift", "baselineShift", FP_NUM, 0, NULL, NULL, NULL, 0, "" },
    { EF "breakOpportunity", "breakOpportunity", FP_ENUM, 0, v_breakop, NULL, "auto", 0, "" },
    { EF "color", "color", FP_UINT, 0, NULL, NULL, NULL, 0, "" },
    { EF "digitCase", "digitCase", FP_ENUM, 0, v_digitcase, NULL, "default", 0, "" },
    { EF "digitWidth", "digitWidth", FP_ENUM, 0, v_digitwidth, NULL, "default", 0, "" },
    { EF "dominantBaseline", "dominantBaseline", FP_ENUM, 0, v_baseline, "useDominantBaseline", "roman", 0, "" },
    { EF "fontDescription", "fontDescription", FP_FD, 0, NULL, NULL, NULL, 0, "" },
    { EF "fontSize", "fontSize", FP_NUM, F_NAN0 | F_NONNEG, NULL, NULL, NULL, 12, "" },
    { EF "kerning", "kerning", FP_ENUM, 0, v_kerning, NULL, "on", 0, "" },
    { EF "ligatureLevel", "ligatureLevel", FP_ENUM, 0, v_ligature, NULL, "common", 0, "" },
    { EF "locale", "locale", FP_STR, 0, NULL, NULL, "en", 0, "" },
    { EF "textRotation", "textRotation", FP_ENUM, 0, v_rotation, NULL, "auto", 0, "" },
    { EF "trackingLeft", "trackingLeft", FP_NUM, F_NAN0, NULL, NULL, NULL, 0, "" },
    { EF "trackingRight", "trackingRight", FP_NUM, F_NAN0, NULL, NULL, NULL, 0, "" },
    { EF "typographicCase", "typographicCase", FP_ENUM, 0, v_typocase, NULL, "default", 0, "" },
    { EF "locked", "locked", FP_BOOL, 0, NULL, NULL, NULL, 0, "" },
    { FD "fontName", "fontName", FP_STR, 0, NULL, NULL, "_serif", 0, "" },
    { FD "fontWeight", "fontWeight", FP_ENUM, 0, v_weight, NULL, "normal", 0, "" },
    { FD "fontPosture", "fontPosture", FP_ENUM, 0, v_posture, NULL, "normal", 0, "" },
    { FD "fontLookup", "fontLookup", FP_ENUM, 0, v_lookup, NULL, "device", 0, "" },
    { FD "renderingMode", "renderingMode", FP_ENUM, 0, v_rendering, NULL, "cff", 0, "" },
    { FD "cffHinting", "cffHinting", FP_ENUM, 0, v_hinting, NULL, "horizontalStem", 0, "" },
    { FD "locked", "locked", FP_BOOL, 0, NULL, NULL, NULL, 0, "" },
    { TB "applyNonLinearFontScaling", "applyNonLinearFontScaling", FP_BOOL, 0, NULL, NULL, NULL, 1, "" },
    { TB "baselineFontDescription", "baselineFontDescription", FP_FDNULL, 0, NULL, NULL, NULL, 0, "" },
    { TB "baselineFontSize", "baselineFontSize", FP_NUM, F_NONNEG, NULL, NULL, NULL, 12, "" },
    { TB "baselineZero", "baselineZero", FP_ENUM, 0, v_baseline, "useDominantBaseline", "roman", 0, "" },
    { TB "bidiLevel", "bidiLevel", FP_INTNN, 0, NULL, NULL, NULL, 0, "" },
    { TB "lineRotation", "lineRotation", FP_ENUM, 0, v_rotation, "auto", "rotate0", 0, "" },
    { CE "textRotation", "textRotation", FP_ENUM, 0, v_rotation, "auto", "rotate0", 0, "" },
    { NULL, NULL, 0, 0, NULL, NULL, NULL, 0, "" }
};

static V2 prop_get(VM2 *vm, V2 self, int argc, V2 *argv) {
    const FProp *p = (const FProp*)vm->native_data; V2 def;
    (void)argc; (void)argv;
    if (!p) return v2_undef();
    switch (p->type) {
    case FP_ENUM: case FP_STR: def = v2_cstr(vm, p->def_s); break;
    case FP_BOOL: def = v2_bool(p->def_n != 0); break;
    case FP_UINT: def = v2_uint((u32)p->def_n); break;
    case FP_INTNN: def = v2_int((s32)p->def_n); break;
    case FP_FD: case FP_FDNULL: def = v2_null(); break;
    default: def = v2_num(p->def_n); break;
    }
    return hget(vm, self, p->store, def);
}
static V2 prop_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    const FProp *p = (const FProp*)vm->native_data; V2 v = ARG(0);
    if (!p) return v2_undef();
    switch (p->type) {
    case FP_ENUM: {
        A2Str *s; int i, ok = 0;
        if (!non_null(vm, v, p->name)) return v2_undef();
        s = v2_tostr(vm, v);
        for (i = 0; p->vals[i]; i++) if (!strcmp(s->s, p->vals[i])) ok = 1;
        if (!ok || (p->exclude && !strcmp(s->s, p->exclude))) { a2_throw_id(vm, "ArgumentError", 2008, p->name, NULL); return v2_undef(); }
        hset(vm, self, p->store, v2_str(s)); break; }
    case FP_STR:
        if (!non_null(vm, v, p->name)) return v2_undef();
        hset(vm, self, p->store, v2_str(v2_tostr(vm, v))); break;
    case FP_BOOL: hset(vm, self, p->store, v2_bool(v2_tobool(vm, v))); break;
    case FP_UINT: hset(vm, self, p->store, v2_uint(v2_touint(vm, v))); break;
    case FP_INTNN: { s32 n = v2_toint(vm, v); if (n < 0) { err2004(vm); return v2_undef(); } hset(vm, self, p->store, v2_int(n)); break; }
    case FP_FD:
        if (!non_null(vm, v, p->name)) return v2_undef();
        hset(vm, self, p->store, v); break;
    case FP_FDNULL: hset(vm, self, p->store, v.t == T_OBJ ? v : v2_null()); break;
    default: {
        double d = v2_tonum(vm, v);
        if ((p->flags & F_NONNEG) && d < 0) { err2004(vm); return v2_undef(); }
        if ((p->flags & F_NAN0) && d != d) d = 0;
        if (p->flags & F_CLAMP01) d = d < 0 ? 0 : d > 1 ? 1 : d;
        hset(vm, self, p->store, v2_num(d)); break; }
    }
    return v2_undef();
}

/* ---------------------------------------------------------------- */
/* content elements                                                 */
/* "__ce_kind": 1 text, 2 group, 3 graphic (absent: a user subclass) */

static int ce_kind(VM2 *vm, V2 o) { return (int)hnum(vm, o, "__ce_kind", 0); }
static V2 ce_text_of(VM2 *vm, V2 o, int depth) {
    int k = ce_kind(vm, o);
    if (k == 1) return hget(vm, o, "__ce_text", v2_null());
    if (k == 3) return v2_str(vm->s_empty);
    if (k == 2 && depth < 64) {
        A2Obj *els = hobj(vm, o, "__ce_elements"); V2 result = v2_null(); int i;
        for (i = 0; els && i < els->alen; i++) {
            V2 t = ce_text_of(vm, els->arr[i], depth + 1);
            if (t.t == T_STR) result = result.t == T_STR ? v2_str(a2_concat(vm, result.u.s, t.u.s)) : t;
            else if (result.t == T_STR && result.u.s->len == 0) result = v2_null();
        }
        return result;
    }
    return v2_null();
}
static V2 ce_get_text(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return ce_text_of(vm, self, 0); }
static V2 ce_get_textBlock(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_null(); }
static V2 ce_get_textBlockBeginIndex(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_int(-1); }
static V2 ce_get_groupElement(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_null(); }
static V2 ce_get_elementFormat(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return hget(vm, self, "__ce_format", v2_null()); }
static V2 ce_set_elementFormat(VM2 *vm, V2 self, int argc, V2 *argv) { V2 v = ARG(0); hset(vm, self, "__ce_format", is_a(vm, v, "flash.text.engine::ElementFormat") ? v : v2_null()); return v2_undef(); }
static V2 ce_get_eventMirror(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return hget(vm, self, "__ce_mirror", v2_null()); }
static V2 ce_set_eventMirror(VM2 *vm, V2 self, int argc, V2 *argv) { hset(vm, self, "__ce_mirror", ARG(0).t == T_OBJ ? ARG(0) : v2_null()); return v2_undef(); }

static V2 te_init(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; hset(vm, self, "__ce_kind", v2_int(1)); hset(vm, self, "__ce_text", v2_null()); return v2_undef(); }
static V2 te_set_text(VM2 *vm, V2 self, int argc, V2 *argv) { V2 v = ARG(0); hset(vm, self, "__ce_text", v.t == T_NULL || v.t == T_UNDEF ? v2_null() : v2_str(v2_tostr(vm, v))); return v2_undef(); }
static V2 gre_init(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; hset(vm, self, "__ce_kind", v2_int(3)); return v2_undef(); }

static A2Obj *ge_list(VM2 *vm, V2 self) { A2Obj *l = hobj(vm, self, "__ce_elements"); if (!l) { l = a2_new_array(vm); hset(vm, self, "__ce_elements", v2_obj(l)); } return l; }
static V2 ge_init(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; hset(vm, self, "__ce_kind", v2_int(2)); ge_list(vm, self); return v2_undef(); }
static V2 ge_get_elementCount(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_int(ge_list(vm, self)->alen); }
static V2 ge_getElementAt(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *l = ge_list(vm, self); s32 i = v2_toint(vm, ARG(0)); if (i < 0 || i >= l->alen) { err2006(vm); return v2_undef(); } return l->arr[i]; }
static A2Obj *ce_vector(VM2 *vm) {
    A2Class *ce = cls(vm, "flash.text.engine::ContentElement"); V2 a[1], cv; A2Obj *v;
    a[0] = ce ? v2_obj(ce->obj) : v2_null(); cv = a2_vector_apply(vm, v2_obj(vm->c_vector->obj), 1, a);
    v = a2_new(vm, cv.t == T_OBJ ? cv.u.o->as_class : vm->c_vector); v->kind = OK_VECTOR; v->vec_type = ce;
    return v;
}
static V2 ge_replace_impl(VM2 *vm, V2 self, int b, int e, V2 nv) {
    A2Obj *l = ge_list(vm, self), *removed, *nl; int i, n = 0; V2 *add = NULL;
    if (b > l->alen || e > l->alen) { err2006(vm); return v2_undef(); }
    if (nv.t == T_OBJ && nv.u.o->kind == OK_VECTOR) {
        A2Obj *vo = nv.u.o; n = vo->alen; add = vo->arr;
        for (i = 0; i < n; i++) {
            V2 el = add[i];
            if (el.t != T_OBJ || !is_a(vm, el, "flash.text.engine::ContentElement")) { err2004(vm); return v2_undef(); }
            if (el.u.o == self.u.o) { err2004(vm); return v2_undef(); }
            if (!ce_kind(vm, el)) { err2004(vm); return v2_undef(); }
        }
    }
    if (e < b) e = b;
    removed = ce_vector(vm);
    for (i = b; i < e; i++) a2_arr_push(vm, removed, l->arr[i]);
    nl = a2_new_array(vm);
    for (i = 0; i < b; i++) a2_arr_push(vm, nl, l->arr[i]);
    for (i = 0; i < n; i++) a2_arr_push(vm, nl, add[i]);
    for (i = e; i < l->alen; i++) a2_arr_push(vm, nl, l->arr[i]);
    hset(vm, self, "__ce_elements", v2_obj(nl));
    return v2_obj(removed);
}
static V2 ge_setElements(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *l = ge_list(vm, self); ge_replace_impl(vm, self, 0, l->alen, ARG(0)); return v2_undef(); }
static V2 ge_replaceElements(VM2 *vm, V2 self, int argc, V2 *argv) {
    s32 b = v2_toint(vm, ARG(0)), e = v2_toint(vm, ARG(1)); V2 nv = ARG(2);
    if (b < 0 || e < 0) { err2006(vm); return v2_undef(); }
    if (b == e && nv.t != T_OBJ) return v2_null();
    return ge_replace_impl(vm, self, b, e, nv);
}

/* ---------------------------------------------------------------- */
/* TextBlock                                                        */

static void tl_set_validity(VM2 *vm, A2Obj *l, const char *v);
static V2 tb_get_content(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return hget(vm, self, "__tb_content", v2_null()); }
static V2 tb_set_content(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 v = ARG(0);
    A2Obj *l; int g = 0;
    if (v.t == T_OBJ && !ce_kind(vm, v)) { err2004(vm); return v2_undef(); }
    for (l = hobj(vm, self, "__tb_first"); l && g++ < 100000; l = hobj(vm, v2_obj(l), "__tl_next")) tl_set_validity(vm, l, "invalid");
    hset(vm, self, "__tb_content", v.t == T_OBJ ? v : v2_null());
    return v2_undef();
}
static V2 tb_get_tabStops(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *v = hobj(vm, self, "__tb_tabs"), *c; int i; UNUSED;
    if (!v) return v2_null();
    c = a2_new(vm, v->cls); c->kind = OK_VECTOR; c->vec_type = v->vec_type;
    for (i = 0; i < v->alen; i++) a2_arr_push(vm, c, v->arr[i]);
    return v2_obj(c);
}
static V2 tb_set_tabStops(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 v = ARG(0); A2Obj *c = NULL; int i;
    if (v.t == T_OBJ && v.u.o->kind == OK_VECTOR) { c = a2_new(vm, v.u.o->cls); c->kind = OK_VECTOR; c->vec_type = v.u.o->vec_type; for (i = 0; i < v.u.o->alen; i++) a2_arr_push(vm, c, v.u.o->arr[i]); }
    hset(vm, self, "__tb_tabs", c ? v2_obj(c) : v2_null());
    return v2_undef();
}
static V2 tb_get_textJustifier(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return hget(vm, self, "__tb_justifier", v2_null()); }
static V2 tb_setTextJustifier(VM2 *vm, V2 self, int argc, V2 *argv) { if (!non_null(vm, ARG(0), "textJustifier")) return v2_undef(); hset(vm, self, "__tb_justifier", ARG(0)); return v2_undef(); }
static V2 tb_get_textLineCreationResult(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return hget(vm, self, "__tb_result", v2_null()); }
static V2 tb_get_firstInvalidLine(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_null(); }
static V2 tb_get_firstLine(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return hget(vm, self, "__tb_first", v2_null()); }
static V2 tb_get_lastLine(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *l = hobj(vm, self, "__tb_first"); int g = 0; UNUSED;
    if (!l) return v2_null();
    while (g++ < 100000) { A2Obj *n = hobj(vm, v2_obj(l), "__tl_next"); if (!n) break; l = n; }
    return v2_obj(l);
}

/* lines */
static void tl_set_validity(VM2 *vm, A2Obj *l, const char *v) { if (!strcmp(v, "static")) hset(vm, v2_obj(l), "__tl_hide", v2_bool(1)); hset(vm, v2_obj(l), "__tl_validity", v2_cstr(vm, v)); }
static void tl_release(VM2 *vm, V2 block, A2Obj *line) {
    A2Obj *prev = hobj(vm, v2_obj(line), "__tl_prev"), *next = hobj(vm, v2_obj(line), "__tl_next"), *n; int g = 0;
    if (hobj(vm, block, "__tb_first") == line) hset(vm, block, "__tb_first", next ? v2_obj(next) : v2_null());
    tl_set_validity(vm, line, "invalid");
    for (n = next; n && g++ < 100000; n = hobj(vm, v2_obj(n), "__tl_next")) tl_set_validity(vm, n, "invalid");
    if (prev) hset(vm, v2_obj(prev), "__tl_next", next ? v2_obj(next) : v2_null());
    if (next) hset(vm, v2_obj(next), "__tl_prev", prev ? v2_obj(prev) : v2_null());
    hset(vm, v2_obj(line), "__tl_block", v2_null());
    hset(vm, v2_obj(line), "__tl_prev", v2_null());
    hset(vm, v2_obj(line), "__tl_next", v2_null());
}
static V2 tb_releaseLines(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *l1, *l2, *l, *list[4096]; int n = 0, first = -1, count = -1, i, g = 0;
    if (!non_null(vm, ARG(0), "firstLine") || !non_null(vm, ARG(1), "lastLine")) return v2_undef();
    l1 = ARG(0).u.o; l2 = ARG(1).u.o;
    for (l = hobj(vm, self, "__tb_first"); l && g++ < 4096 && n < 4096; l = hobj(vm, v2_obj(l), "__tl_next")) list[n++] = l;
    for (i = 0; i < n; i++) if (list[i] == l1 || list[i] == l2) { first = i; break; }
    if (first < 0) { err2004(vm); return v2_undef(); }
    if (l1 == l2) { tl_release(vm, self, l1); return v2_undef(); }
    for (i = first + 1; i < n; i++) if (list[i] == l1 || list[i] == l2) { count = i - first + 1; break; }
    if (count < 0) { err2004(vm); return v2_undef(); }
    for (i = first; i < first + count; i++) tl_release(vm, self, list[i]);
    return v2_undef();
}

/* the content as text spans: the formats of the text elements */
typedef struct { A2Str *text; V2 fmt[256]; int fpos[256]; int nf; int null_format; } Spans;
static void spans_add(VM2 *vm, Spans *sp, V2 el, int depth) {
    int k = ce_kind(vm, el);
    if (depth > 64 || sp->null_format) return;
    if (k == 1) {
        V2 t = hget(vm, el, "__ce_text", v2_null()), f;
        if (t.t != T_STR) return;   /* null text: the element is ignored, format and all */
        f = hget(vm, el, "__ce_format", v2_null());
        if (f.t != T_OBJ) { sp->null_format = 1; return; }
        if (sp->nf < 256) { sp->fmt[sp->nf] = f; sp->fpos[sp->nf] = (int)sp->text->len; sp->nf++; }
        sp->text = a2_concat(vm, sp->text, t.u.s);
    } else if (k == 2) {
        A2Obj *els = hobj(vm, el, "__ce_elements"); int i;
        for (i = 0; els && i < els->alen; i++) spans_add(vm, sp, els->arr[i], depth + 1);
    }
}
/* UTF-16 helpers (FTE indices count UTF-16 units) */
static int u16_len(const A2Str *s) { int n = 0; u32 i = 0; while (i < s->len) { u8 c = (u8)s->s[i]; int k = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4; n += k == 4 ? 2 : 1; i += (u32)k; } return n; }
static u32 u16_byte_at(const A2Str *s, int pos) { int n = 0; u32 i = 0; while (i < s->len && n < pos) { u8 c = (u8)s->s[i]; int k = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4; n += k == 4 ? 2 : 1; i += (u32)k; } return i; }
static u32 cp_at(const A2Str *s, u32 i, int *k) { u8 c = (u8)s->s[i]; if (c < 0x80) { *k = 1; return c; } if (c < 0xE0) { *k = 2; return ((u32)(c & 31) << 6) | (s->s[i + 1] & 63); } if (c < 0xF0) { *k = 3; return ((u32)(c & 15) << 12) | ((u32)(s->s[i + 1] & 63) << 6) | (s->s[i + 2] & 63); } *k = 4; return 0x10000; }
static int next_line_break(const A2Str *s, int start) {
    u32 i = u16_byte_at(s, start); int pos = start;
    while (i < s->len) {
        int k; u32 c = cp_at(s, i, &k);
        i += (u32)k; pos += k == 4 ? 2 : 1;
        if (c == 0x0A || c == 0x2028 || c == 0x2029) return pos;
        if (c == 0x0D) { if (i < s->len && s->s[i] == 0x0A) pos++; return pos; }
    }
    return pos;
}
static A2Str *u16_substr(VM2 *vm, const A2Str *s, int a, int b) { u32 ba = u16_byte_at(s, a), bb = u16_byte_at(s, b); return a2_intern(vm, s->s + ba, bb - ba); }

/* ElementFormat -> text format (Ruffle's as_text_format) */
static void fmt_of(VM2 *vm, V2 ef, RTFmt *f) {
    V2 fd = hget(vm, ef, "__p_" EF "fontDescription", v2_null());
    rt_fmt_clear(f);
    f->has = RTF_FONT | RTF_SIZE | RTF_COLOR | RTF_BOLD | RTF_ITALIC;
    f->size = hnum(vm, ef, "__p_" EF "fontSize", 12);
    f->color = v2_touint(vm, hget(vm, ef, "__p_" EF "color", v2_uint(0))) & 0xFFFFFF;
    { A2Str *n = fd.t == T_OBJ ? v2_tostr(vm, hget(vm, fd, "__p_" FD "fontName", v2_cstr(vm, "_serif"))) : a2_str(vm, "_serif"); size_t l = n->len < sizeof(f->font) - 1 ? n->len : sizeof(f->font) - 1; memcpy(f->font, n->s, l); f->font[l] = 0; }
    f->bold = fd.t == T_OBJ && !strcmp(v2_tostr(vm, hget(vm, fd, "__p_" FD "fontWeight", v2_cstr(vm, "normal")))->s, "bold");
    f->italic = fd.t == T_OBJ && !strcmp(v2_tostr(vm, hget(vm, fd, "__p_" FD "fontPosture", v2_cstr(vm, "normal")))->s, "italic");
}

/* a recreated line loses its display state (TextBlock.recreateTextLine) */
static void tl_reset(VM2 *vm, A2Obj *line) {
    V2 o = v2_obj(line); A2Listener *l;
    a2_set_s(vm, o, "blendMode", v2_cstr(vm, "normal")); a2_set_s(vm, o, "alpha", v2_num(1)); a2_set_s(vm, o, "mask", v2_null());
    a2_set_s(vm, o, "scaleX", v2_num(1)); a2_set_s(vm, o, "scaleY", v2_num(1)); a2_set_s(vm, o, "rotation", v2_num(0));
    a2_set_s(vm, o, "visible", v2_bool(1)); a2_set_s(vm, o, "cacheAsBitmap", v2_bool(0)); a2_set_s(vm, o, "opaqueBackground", v2_null());
    a2_set_s(vm, o, "x", v2_num(0)); a2_set_s(vm, o, "y", v2_num(0));
    if (line->kind == OK_DISPLAY) { a2_disp(line)->z = 0; a2_disp(line)->has_m3d = 0; }
    a2_call_s(vm, o, "removeChildren", 0, NULL);
    while ((l = line->listeners) != NULL) { line->listeners = l->next; free(l); }
    if (vm->throwing) vm->throwing = 0, vm->exc = v2_undef();
}
/* link a (re)created line after `prev` (Flash's list rules): a line already
 * in that slot keeps its links and its successors become invalid; otherwise
 * the line leaves its own list and replaces the slot's occupant */
static int g_tl_same_width;   /* tb_DoCreateTextLine: a reused line keeps its specified width */
static void tl_link(VM2 *vm, V2 block, A2Obj *line, A2Obj *prev) {
    A2Obj *slot = prev ? hobj(vm, v2_obj(prev), "__tl_next") : hobj(vm, block, "__tb_first"), *n; V2 lb = hget(vm, v2_obj(line), "__tl_block", v2_null()); int g = 0;
    hset(vm, v2_obj(line), "__tl_validity", v2_cstr(vm, "valid")); hset(vm, v2_obj(line), "__tl_hide", v2_bool(0));
    if (slot == line && lb.t == T_OBJ && lb.u.o == block.u.o) {
        /* recreated in place with an unchanged width: Flash links the line straight to the
           block's last line; the lines in between leave as a separate (invalid) chain */
        A2Obj *nx = hobj(vm, v2_obj(line), "__tl_next"), *last = line, *k;
        while (g++ < 100000 && (k = hobj(vm, v2_obj(last), "__tl_next")) != NULL) last = k;
        if (nx && nx != last && g_tl_same_width) {
            A2Obj *r1 = hobj(vm, v2_obj(last), "__tl_prev");
            for (n = nx; n && g++ < 200000; n = hobj(vm, v2_obj(n), "__tl_next")) { tl_set_validity(vm, n, "invalid"); if (n == r1) break; }
            hset(vm, v2_obj(nx), "__tl_prev", v2_null());
            if (r1) hset(vm, v2_obj(r1), "__tl_next", v2_null());
            hset(vm, v2_obj(line), "__tl_next", v2_obj(last)); hset(vm, v2_obj(last), "__tl_prev", v2_obj(line));
        }
        for (n = hobj(vm, v2_obj(line), "__tl_next"); n && g++ < 300000; n = hobj(vm, v2_obj(n), "__tl_next")) tl_set_validity(vm, n, "invalid");
        return;
    }
    if (lb.t == T_OBJ) {   /* leave the current list */
        A2Obj *lp = hobj(vm, v2_obj(line), "__tl_prev"), *ln = hobj(vm, v2_obj(line), "__tl_next");
        if (hobj(vm, lb, "__tb_first") == line) hset(vm, lb, "__tb_first", ln ? v2_obj(ln) : v2_null());
        for (n = ln; n && g++ < 100000; n = hobj(vm, v2_obj(n), "__tl_next")) tl_set_validity(vm, n, "invalid");
        if (lp) hset(vm, v2_obj(lp), "__tl_next", ln ? v2_obj(ln) : v2_null());
        if (ln) hset(vm, v2_obj(ln), "__tl_prev", lp ? v2_obj(lp) : v2_null());
        slot = prev ? hobj(vm, v2_obj(prev), "__tl_next") : hobj(vm, block, "__tb_first");
    }
    hset(vm, v2_obj(line), "__tl_block", block);
    hset(vm, v2_obj(line), "__tl_prev", prev ? v2_obj(prev) : v2_null());
    if (slot && slot != line) {
        A2Obj *sn = hobj(vm, v2_obj(slot), "__tl_next");
        hset(vm, v2_obj(line), "__tl_next", sn ? v2_obj(sn) : v2_null());
        if (sn) hset(vm, v2_obj(sn), "__tl_prev", v2_obj(line));
        tl_set_validity(vm, slot, "invalid");
        hset(vm, v2_obj(slot), "__tl_prev", v2_null()); hset(vm, v2_obj(slot), "__tl_next", v2_null());
    } else hset(vm, v2_obj(line), "__tl_next", v2_null());
    if (prev) hset(vm, v2_obj(prev), "__tl_next", v2_obj(line)); else hset(vm, block, "__tb_first", v2_obj(line));
}
static V2 tb_DoCreateTextLine(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *use = ARG(0).t == T_OBJ ? ARG(0).u.o : NULL, *prev = ARG(1).t == T_OBJ ? ARG(1).u.o : NULL, *line;
    double width = v2_tonum(vm, ARG(2)); V2 content = hget(vm, self, "__tb_content", v2_null());
    static Spans sp; int ppos, npos, len, lidx; A2Str *sub; RTFmt f;
    if (content.t != T_OBJ) return v2_null();
    sp.text = vm->s_empty; sp.nf = 0; sp.null_format = 0;
    spans_add(vm, &sp, content, 0);
    if (sp.null_format) {
        a2_throw_id(vm, "Error", 2175, NULL, NULL);
        a2_report_uncaught(vm);
        return v2_null();
    }
    ppos = prev ? (int)hnum(vm, v2_obj(prev), "__tl_end", 0) : 0;
    len = u16_len(sp.text);
    if (ppos > len) { hset(vm, self, "__tb_result", v2_cstr(vm, "complete")); return v2_null(); }
    npos = next_line_break(sp.text, ppos);
    if (len == 0 || (npos == len && ppos == npos)) { hset(vm, self, "__tb_result", v2_cstr(vm, "complete")); return v2_null(); }
    lidx = prev ? (int)hnum(vm, v2_obj(prev), "__tl_index", 0) + 1 : 0;
    if (use) {
        line = use;
        tl_reset(vm, line);
    } else {
        A2Class *tlc = cls(vm, "flash.text.engine::TextLine");
        if (!tlc) return v2_null();
        line = a2_new_display(vm, tlc);
        a2_disp(line)->no_tab_children = 1;
    }
    sub = u16_substr(vm, sp.text, ppos, npos);
    fmt_of(vm, sp.fmt[0], &f);
    g_tl_same_width = use && hnum(vm, v2_obj(line), "__tl_width", -1) == width;
    a2_fte_line_setup(vm, line, sub, &f, width);
    hset(vm, v2_obj(line), "__tl_width", v2_num(width));
    hset(vm, v2_obj(line), "__tl_rawlen", v2_int(u16_len(sub)));
    hset(vm, v2_obj(line), "__tl_begin", v2_int(ppos));
    hset(vm, v2_obj(line), "__tl_end", v2_int(npos));
    hset(vm, v2_obj(line), "__tl_index", v2_int(lidx));
    hset(vm, v2_obj(line), "__tl_size", v2_num(f.size));
    hset(vm, v2_obj(line), "__tl_hastabs", v2_bool(strchr(sub->s, '\t') != NULL));
    tl_link(vm, self, line, prev);
    hset(vm, self, "__tb_result", v2_cstr(vm, "success"));
    return v2_obj(line);
}

/* ---------------------------------------------------------------- */
/* TextLine                                                         */

static V2 tl_get_rawTextLength(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return hget(vm, self, "__tl_rawlen", v2_int(0)); }
static V2 tl_get_textBlockBeginIndex(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return hget(vm, self, "__tl_begin", v2_int(0)); }
static V2 tl_get_specifiedWidth(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return hget(vm, self, "__tl_width", v2_num(0)); }
static V2 tl_get_textBlock(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; if (v2_tobool(vm, hget(vm, self, "__tl_hide", v2_bool(0)))) return v2_null(); return hget(vm, self, "__tl_block", v2_null()); }
static V2 tl_get_previousLine(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return hget(vm, self, "__tl_prev", v2_null()); }
static V2 tl_get_nextLine(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return hget(vm, self, "__tl_next", v2_null()); }
static V2 tl_get_hasTabs(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return hget(vm, self, "__tl_hastabs", v2_bool(0)); }
static V2 tl_measure(VM2 *vm, V2 self, int which) {
    double m[4]; if (self.t != T_OBJ) return v2_num(0);
    a2_fte_line_measure(vm, self.u.o, &m[0], &m[1], &m[2], &m[3]);
    /* ascent / descent: the em box of the line's font size (0.8 / 0.2) */
    if (which >= 2) { double sz = hnum(vm, self, "__tl_size", 12); return v2_num(which == 2 ? sz * 0.8 : sz * 0.2); }
    return v2_num(m[which]);
}
static V2 tl_get_textWidth(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return tl_measure(vm, self, 0); }
static V2 tl_get_textHeight(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return tl_measure(vm, self, 1); }
static V2 tl_get_ascent(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return tl_measure(vm, self, 2); }
static V2 tl_get_descent(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return tl_measure(vm, self, 3); }
static V2 tl_get_validity(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return hget(vm, self, "__tl_validity", v2_cstr(vm, "valid")); }
static V2 tl_set_validity_n(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Str *nv, *pv; int same, ok;
    if (!non_null(vm, ARG(0), "validity")) return v2_undef();
    nv = v2_tostr(vm, ARG(0)); pv = v2_tostr(vm, hget(vm, self, "__tl_validity", v2_cstr(vm, "valid")));
    same = nv->len == pv->len && !memcmp(nv->s, pv->s, nv->len);
    if (same) ok = 1;
    else if (!strcmp(nv->s, "possiblyInvalid")) ok = 0;
    else if (!strcmp(nv->s, "static")) ok = 1;
    else if (!strcmp(pv->s, "static") || !strcmp(pv->s, "invalid")) ok = 0;
    else ok = 1;
    if (!ok) { a2_throw_id(vm, "ArgumentError", 2008, "validity", NULL); return v2_undef(); }
    if (self.t == T_OBJ) tl_set_validity(vm, self.u.o, nv->s);
    return v2_undef();
}

void a2_fte_init(VM2 *vm) {
    static int done; int i;
    (void)vm;
    if (done) return;
    done = 1;
    for (i = 0; g_props[i].key; i++) {
        FProp *p = &g_props[i]; char *gk, *sk; const char *sl = strrchr(p->key, '/'); size_t pre = (size_t)(sl - p->key) + 1;
        sprintf(p->store, "__p_%.90s", p->key);
        gk = (char*)malloc(strlen(p->key) + 5); sk = (char*)malloc(strlen(p->key) + 5);
        if (!gk || !sk) return;
        memcpy(gk, p->key, pre); strcpy(gk + pre, "get "); strcat(gk, sl + 1);
        memcpy(sk, p->key, pre); strcpy(sk + pre, "set "); strcat(sk, sl + 1);
        a2_register_native_data(gk, prop_get, p); a2_register_native_data(sk, prop_set, p);
    }
    a2_register_native(CE "get text", ce_get_text);
    a2_register_native(CE "get rawText", ce_get_text);
    a2_register_native(CE "get textBlock", ce_get_textBlock);
    a2_register_native(CE "get textBlockBeginIndex", ce_get_textBlockBeginIndex);
    a2_register_native(CE "get groupElement", ce_get_groupElement);
    a2_register_native(CE "get elementFormat", ce_get_elementFormat);
    a2_register_native(CE "set elementFormat", ce_set_elementFormat);
    a2_register_native(CE "get eventMirror", ce_get_eventMirror);
    a2_register_native(CE "set eventMirror", ce_set_eventMirror);
    a2_register_native("flash.text.engine::TextElement/init", te_init);
    a2_register_native("flash.text.engine::TextElement/set text", te_set_text);
    a2_register_native("flash.text.engine::GraphicElement/init", gre_init);
    a2_register_native("flash.text.engine::GroupElement/init", ge_init);
    a2_register_native("flash.text.engine::GroupElement/get elementCount", ge_get_elementCount);
    a2_register_native("flash.text.engine::GroupElement/getElementAt", ge_getElementAt);
    a2_register_native("flash.text.engine::GroupElement/setElements", ge_setElements);
    a2_register_native("flash.text.engine::GroupElement/replaceElements", ge_replaceElements);
    a2_register_native(TB "get content", tb_get_content);
    a2_register_native(TB "set content", tb_set_content);
    a2_register_native(TB "get tabStops", tb_get_tabStops);
    a2_register_native(TB "set tabStops", tb_set_tabStops);
    a2_register_native(TB "get textJustifier", tb_get_textJustifier);
    a2_register_native(TB "setTextJustifier", tb_setTextJustifier);
    a2_register_native(TB "get textLineCreationResult", tb_get_textLineCreationResult);
    a2_register_native(TB "get firstInvalidLine", tb_get_firstInvalidLine);
    a2_register_native(TB "get firstLine", tb_get_firstLine);
    a2_register_native(TB "get lastLine", tb_get_lastLine);
    a2_register_native(TB "releaseLines", tb_releaseLines);
    a2_register_native(TB "DoCreateTextLine", tb_DoCreateTextLine);
#define TL "flash.text.engine::TextLine/"
    a2_register_native(TL "get rawTextLength", tl_get_rawTextLength);
    a2_register_native(TL "get textBlockBeginIndex", tl_get_textBlockBeginIndex);
    a2_register_native(TL "get specifiedWidth", tl_get_specifiedWidth);
    a2_register_native(TL "get textBlock", tl_get_textBlock);
    a2_register_native(TL "get ascent", tl_get_ascent);
    a2_register_native(TL "get descent", tl_get_descent);
    a2_register_native(TL "get textWidth", tl_get_textWidth);
    a2_register_native(TL "get textHeight", tl_get_textHeight);
    a2_register_native(TL "get validity", tl_get_validity);
    a2_register_native(TL "set validity", tl_set_validity_n);
    a2_register_native(TL "get previousLine", tl_get_previousLine);
    a2_register_native(TL "get nextLine", tl_get_nextLine);
    a2_register_native(TL "get hasTabs", tl_get_hasTabs);
}
