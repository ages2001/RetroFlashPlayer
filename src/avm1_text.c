/* avm1_text.c - TextField and TextFormat for AVM1, on top of the shared
 * text field engine (textfield.c / richtext.c), and the text field
 * variable bindings ("var" of DefineEditText). */
#include "avm1_int.h"
#include "textfield.h"
#include "devfont.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define DE  PA_DONT_ENUM
#define DD  PA_DONT_DELETE
#define RO  PA_READ_ONLY
#define V6  PA_VERSION_6
#define V7  PA_VERSION_7
#define V8  PA_VERSION_8

extern AObj *lib_class(VM *vm, Env *env, NativeFn ctor, NativeFn call, AObj *super_proto, int proto_first, AObj **proto_out);
extern void edittext_init_state(Player *p, SWFChar *ch, TextFieldState *t);
extern const char *edittext_variable(SWFChar *ch);
extern int a1_resolve_variable_path(Act *a, MovieClip *start, AStr *path, AObj **obj, AStr **var);
extern void a1_remove_display_object(Act *a, MovieClip *c);

static void defm(VM *vm, Env *env, AObj *o, const char *name, NativeFn f, u16 attr) { o_define(vm, o, str_intern(vm, name), v_obj(fn_native(vm, env, f, NULL)), attr); }
static void defp(VM *vm, Env *env, AObj *o, const char *name, NativeFn g, NativeFn s, u16 attr) { o_add_property(vm, o, str_intern(vm, name), fn_native(vm, env, g, NULL), s ? fn_native(vm, env, s, NULL) : NULL, attr); }

static TextFieldState *tfs_of(MovieClip *c) { return c && c->a1 ? c->a1->tfs : NULL; }
static MovieClip *tf_of(AObj *self) {
    MovieClip *c = o_display(self);
    if (!c || clip_removed(c) || !tfs_of(c)) return NULL;
    return c;
}

static void *ss_data_of(AObj *o);

/* ---------------------------------------------------------------- */
/* strings                                                          */

static u32 *astr_cps(VM *vm, AStr *s, int *n) { const char *u = str_c(vm, s); u32 *o; *n = rt_utf8_decode(u, (int)strlen(u), &o); return o; }
static AStr *cps_astr(VM *vm, const u32 *u, int n) { int bl; char *b = rt_utf8_encode(u, n, &bl); AStr *r = str_utf8(vm, b ? b : "", b ? (size_t)bl : 0); free(b); return r; }

static AStr *tf_text(VM *vm, TextFieldState *t) { return cps_astr(vm, t->rt.text, t->rt.len); }

static AStr *tf_html_text(VM *vm, TextFieldState *t) {
    char *h; AStr *r;
    if (!t->html && !t->css) return tf_text(vm, t);
    if (t->html_source) return str_cstr(vm, t->html_source);
    h = rt_to_html_v(&t->rt, t->swf_version);
    r = str_cstr(vm, h ? h : "");
    free(h);
    return r;
}

static void clamp_sel(TextFieldState *t) {
    if (t->sel_begin > t->rt.len) t->sel_begin = t->rt.len;
    if (t->sel_end > t->rt.len) t->sel_end = t->rt.len;
    if (t->sel_begin < 0) t->sel_begin = 0;
    if (t->sel_end < 0) t->sel_end = 0;
}

static void parse_html_css(VM *vm, TextFieldState *t, AStr *s);
static void set_text(VM *vm, TextFieldState *t, AStr *s) {
    int n, i, k = 0; u32 *u;
    if (t->css) { if (!str_eq(tf_text(vm, t), s)) parse_html_css(vm, t, s); return; }
    u = astr_cps(vm, s, &n);
    for (i = 0; i < n; i++) u[k++] = u[i] == '\n' ? '\r' : u[i];
    n = k;
    if (n == t->rt.len && (n == 0 || !memcmp(u, t->rt.text, (size_t)n * sizeof(u32)))) { free(u); return; }
    rt_set_plain(&t->rt, u, n, &t->deffmt);
    free(u);
    free(t->html_source); t->html_source = NULL;
    clamp_sel(t);
    tfs_invalidate(t);
}

static int ss_lookup(void *ctx, const char *sel, RTFmt *out);
/* EditText::parse_html with a style sheet: the source is kept for htmlText */
static void parse_html_css(VM *vm, TextFieldState *t, AStr *s) {
    RTHtmlOpts op; const char *u = str_c(vm, s);
    op.multiline = t->multiline; op.condense_white = t->condense_white; op.swf_version = t->swf_version; op.css = ss_lookup; op.css_ctx = t->css;
    rt_from_html(&t->rt, u, &t->deffmt, &op);
    if (t->swf_version < 8) t->deffmt.align = RT_ALIGN_LEFT;
    free(t->html_source); t->html_source = NULL;
    if (t->css) { t->html_source = (char*)malloc(strlen(u) + 1); if (t->html_source) strcpy(t->html_source, u); }
    clamp_sel(t);
    tfs_invalidate(t);
}
static void set_html_text(VM *vm, TextFieldState *t, AStr *s) {
    AStr *cur = tf_html_text(vm, t);
    if (str_eq(cur, s)) return;
    if (t->css) { parse_html_css(vm, t, s); return; }
    if (t->html) {
        RTHtmlOpts op;
        op.multiline = t->multiline; op.condense_white = t->condense_white; op.swf_version = t->swf_version; op.css = NULL; op.css_ctx = NULL;
        rt_from_html(&t->rt, str_c(vm, s), &t->deffmt, &op);
        if (t->swf_version < 8) t->deffmt.align = RT_ALIGN_LEFT;   /* (the parse's default format stays) */
        clamp_sel(t);
        tfs_invalidate(t);
    } else set_text(vm, t, s);
}

/* ---------------------------------------------------------------- */
/* creation                                                         */

static TextFieldState *new_tfs(VM *vm, MovieClip *c) {
    A1Clip *s = a1_of(c); TextFieldState *t;
    if (!s) return NULL;
    if (s->tfs) return s->tfs;
    t = (TextFieldState*)calloc(1, sizeof(TextFieldState));
    if (!t) return NULL;
    tfs_init(t, clip_swf_version(c));
    t->player = vm->player;
    s->tfs = t;
    return t;
}

/* a DefineEditText instance */
void a1_tf_init(VM *vm, MovieClip *c) {
    TextFieldState *t = new_tfs(vm, c); A1Clip *s = c->a1;
    if (!t) return;
    if (c->self && c->self->ch && c->self->ch->type == CHAR_EDITTEXT) {
        const char *var = edittext_variable(c->self->ch);
        edittext_init_state(vm->player, c->self->ch, t);
        tfs_csm(t, c->self->ch->csm);
        t->swf_version = clip_swf_version(c);
        if (var && *var) s->tf_var = str_from_swf(vm, var, strlen(var), clip_swf_version(c));
    }
}

/* createTextField / EditText::new */
void a1_tf_create(VM *vm, MovieClip *c, double x, double y, double w, double h) {
    TextFieldState *t = new_tfs(vm, c);
    if (!t) return;
    rt_fmt_default(&t->deffmt);
    t->xmin = 0; t->ymin = 0;
    t->w = (int)floor(w * 20.0); t->h = (int)floor(h * 20.0);
    t->box_flip = (t->w < 0 ? 1 : 0) | (t->h < 0 ? 2 : 0);
    if (t->w < 0) t->w = -t->w;   /* a negative size gives the same extent */
    if (t->h < 0) t->h = -t->h;
    t->selectable = 1; t->input = 0; t->html = 0; t->wordwrap = 0; t->multiline = 0;
    t->swf_version = clip_swf_version(c);
    t->movie = c->movie;
    if (c->self) { c->self->matrix.tx = floor(x * 20.0); c->self->matrix.ty = floor(y * 20.0); }
    tfs_invalidate(t);
}

void a1_tf_copy_editable(MovieClip *dst, MovieClip *src) {
    TextFieldState *a = tfs_of(dst), *b = tfs_of(src);
    if (a && b) a->input = b->input;
}

/* ---------------------------------------------------------------- */
/* variable bindings                                                */

static MovieClip **g_unbound; static int g_nunbound, g_capunbound;

static void unbound_add(MovieClip *tf) {
    int i;
    for (i = 0; i < g_nunbound; i++) if (g_unbound[i] == tf) return;
    if (g_nunbound == g_capunbound) { int nc = g_capunbound ? g_capunbound * 2 : 16; MovieClip **n = (MovieClip**)realloc(g_unbound, (size_t)nc * sizeof(MovieClip*)); if (!n) return; g_unbound = n; g_capunbound = nc; }
    g_unbound[g_nunbound++] = tf;
}
static void unbound_remove(MovieClip *tf) {
    int i;
    for (i = 0; i < g_nunbound; i++) if (g_unbound[i] == tf) { g_unbound[i] = g_unbound[--g_nunbound]; return; }
}

static void register_binding(MovieClip *holder, MovieClip *tf, AStr *var) {
    A1Clip *s = a1_of(holder);
    if (!s) return;
    {
        TFBinding *nb = (TFBinding*)realloc(s->bindings, (size_t)(s->nbindings + 1) * sizeof(TFBinding));
        if (!nb) return;
        s->bindings = nb;
        s->bindings[s->nbindings].tf = tf; s->bindings[s->nbindings].var = var;
        s->nbindings++;
    }
}
static void clear_binding(MovieClip *holder, MovieClip *tf) {
    A1Clip *s = holder ? holder->a1 : NULL; int i, k = 0;
    if (!s) return;
    for (i = 0; i < s->nbindings; i++) if (s->bindings[i].tf != tf) s->bindings[k++] = s->bindings[i];
    s->nbindings = k;
}

/* EditText::try_bind_text_field_variable */
static int try_bind(VM *vm, MovieClip *tf, int set_initial) {
    A1Clip *s = tf->a1; MovieClip *parent; TextFieldState *t = tfs_of(tf); Act a; AObj *obj = NULL; AStr *prop = NULL; int bound = 0;
    if (!s || !s->tf_var || !t) return 1;
    parent = clip_avm1_parent(tf);
    while (parent && parent->a1 && parent->a1->kind == 1) parent = clip_avm1_parent(parent);
    if (!parent) return 0;
    act_init_clip(&a, vm, parent);
    a.swf_version = (u8)clip_swf_version(tf);
    if (a1_resolve_variable_path(&a, parent, s->tf_var, &obj, &prop) && obj) {
        if (set_initial) {
            if (o_has_property(&a, obj, prop)) {
                Value v = o_get(&a, obj, prop); AStr *str;
                if (vm->err) vm->err = 0;
                str = v_str_of(&a, v);
                if (vm->err) { vm->err = 0; str = vm->s_empty; }
                set_html_text(vm, t, str);
            } else if (t->rt.len && set_initial == 1) {
                o_set(&a, obj, prop, v_str(tf_text(vm, t)));
                if (vm->err) vm->err = 0;
            }
        }
        {
            MovieClip *d = o_display(obj);
            if (d) { s->tf_bound = d; register_binding(d, tf, prop); bound = 1; }
        }
    }
    if (vm->err) vm->err = 0;
    return bound;
}

void a1_tf_bind_pending(VM *vm) {
    int i = 0;
    while (i < g_nunbound) {
        MovieClip *tf = g_unbound[i];
        if (!tf || clip_removed(tf)) { g_unbound[i] = g_unbound[--g_nunbound]; continue; }
        if (try_bind(vm, tf, 0)) { g_unbound[i] = g_unbound[--g_nunbound]; continue; }
        i++;
    }
}

/* unload: drop the bindings that point at `c` (and c's own binding) */
void a1_tf_unbind(VM *vm, MovieClip *c) {
    A1Clip *s = c->a1; int i;
    (void)vm;
    if (!s) return;
    for (i = 0; i < s->nbindings; i++) {
        MovieClip *tf = s->bindings[i].tf;
        if (tf && tf->a1) { tf->a1->tf_bound = NULL; if (!clip_removed(tf) && tf != c) unbound_add(tf); }
    }
    s->nbindings = 0;
    if (s->tf_bound) { clear_binding(s->tf_bound, c); s->tf_bound = NULL; }
    unbound_remove(c);
}

/* a variable bound to a text field changed */
void a1_textfield_set_bound(Act *a, MovieClip *tf, Value v) {
    TextFieldState *t = tfs_of(tf); AStr *s;
    if (!t) return;
    s = v_str_of(a, v);
    if (a->vm->err) return;
    set_html_text(a->vm, t, s);
}

/* the text changed from script or typing: update the bound variable */
static void propagate_binding(VM *vm, MovieClip *tf) {
    A1Clip *s = tf->a1; MovieClip *parent; Act a; AObj *obj = NULL; AStr *prop = NULL; TextFieldState *t = tfs_of(tf);
    if (!s || !s->tf_var || !t || (s->draw_has & 0x200)) return;
    parent = clip_avm1_parent(tf);
    if (!parent) return;
    s->draw_has |= 0x200;
    act_init_clip(&a, vm, parent);
    a.swf_version = (u8)clip_swf_version(tf);
    if (a1_resolve_variable_path(&a, parent, s->tf_var, &obj, &prop) && obj) {
        o_set(&a, obj, prop, v_str(tf_html_text(vm, t)));
    }
    if (vm->err) vm->err = 0;
    if (tf->a1) tf->a1->draw_has &= ~0x200;
}

/* EditText::post_instantiation */
static void a1_tf_populate_proto(Act *a, AObj *proto);
void a1_tf_post_instantiation(VM *vm, MovieClip *c) {
    Act a;
    if (!tfs_of(c)) a1_tf_init(vm, c);
    act_init_nothing(&a, vm, c, "[Construct]");
    if (!c->obj) {
        Value ctor = v_undef(), proto = v_undef(); AObj *o, *co = a1_resolve_class(&a, "TextField");
        if (co) ctor = v_obj(co);
        if (!a1_class_proto(&a, co, &proto)) proto = v_undef();
        o = obj_bare(vm);
        if (!o) return;
        o->nk = NK_EDITTEXT; o->nat.clip = c;
        if (proto.t != V_UNDEF) o_define(vm, o, vm->s_proto, proto, PA_DONT_ENUM | PA_DONT_DELETE);
        c->obj = (struct AObj*)o;
        /* (a text field of a SWF 5 movie is no TextField: Gnash TextField.as and its Dejagnu.swf) */
        if (proto.t == V_OBJ && !(c->movie && c->movie->version < 6)) { a1_tf_populate_proto(&a, proto.u.o); if (vm->err) vm_report_error(&a); }
        if (ctor.t == V_OBJ) { o_construct_on_existing(&a, ctor.u.o, o, 0, NULL); if (vm->err) vm_report_error(&a); }
    }
    if (!try_bind(vm, c, 1)) unbound_add(c);
    a1_tf_bind_pending(vm);
    if (c->obj) {
        Value self = v_obj((AObj*)c->obj);
        AObj *l = obj_array(&a, 1, &self);
        if (l) o_define(vm, (AObj*)c->obj, vm->s__listeners, v_obj(l), PA_DONT_ENUM | PA_DONT_DELETE);
    }
}

/* ---------------------------------------------------------------- */
/* TextFormat                                                       */

static RTFmt *fmt_of(AObj *o) { o = o && o->nk == NK_SUPER ? o->nat.sup.self : o; return o && o->nk == NK_TEXTFORMAT ? (RTFmt*)o->nat.p : NULL; }
static void fmt_free(AObj *o) { free(o->nat.p); o->nat.p = NULL; }

/* ecma_conversions::round_to_even: ties to even, out of range -> INT_MIN */
static double round_even(double v) {
    double f = floor(v), d = v - f, r;
    if (v != v || v - v != 0) return -2147483648.0;
    if (d > 0.5) r = f + 1; else if (d < 0.5) r = f; else r = fmod(f, 2.0) == 0 ? f : f + 1;
    if (r > 2147483647.0 || r < -2147483648.0) return -2147483648.0;
    return r;
}

AObj *a1_new_textformat(Act *a, const RTFmt *f) {
    VM *vm = a->vm; AObj *o; RTFmt *p;
    o = a1_new_with_proto(a, "TextFormat");
    if (!o) return NULL;
    p = (RTFmt*)malloc(sizeof(RTFmt));
    if (!p) return o;
    if (f) *p = *f; else rt_fmt_clear(p);
    o->nk = NK_TEXTFORMAT; o->nat.p = p; o->nfree = fmt_free;
    return o;
}

static Value numv(double d) { return v_num(d); }

#define FMT_GET(fn, flag, expr) static Value fn(Act *a, AObj *self, int argc, Value *argv) { RTFmt *f = fmt_of(self); (void)a; (void)argc; (void)argv; if (!f) return v_undef(); if (!(f->has & flag)) return v_null(); return expr; }
FMT_GET(tfm_get_font, RTF_FONT, v_str(str_cstr(a->vm, f->font)))
FMT_GET(tfm_get_size, RTF_SIZE, numv(f->size))
FMT_GET(tfm_get_color, RTF_COLOR, v_num((double)f->color))
FMT_GET(tfm_get_url, RTF_URL, v_str(str_cstr(a->vm, f->url)))
FMT_GET(tfm_get_target, RTF_TARGET, v_str(str_cstr(a->vm, f->target)))
FMT_GET(tfm_get_bold, RTF_BOLD, v_bool(f->bold))
FMT_GET(tfm_get_italic, RTF_ITALIC, v_bool(f->italic))
FMT_GET(tfm_get_underline, RTF_UNDERLINE, v_bool(f->underline))
FMT_GET(tfm_get_align, RTF_ALIGN, v_str(str_intern(a->vm, f->align == RT_ALIGN_RIGHT ? "right" : f->align == RT_ALIGN_CENTER ? "center" : f->align == RT_ALIGN_JUSTIFY ? "justify" : "left")))
FMT_GET(tfm_get_left_margin, RTF_LEFTMARGIN, numv(f->left_margin))
FMT_GET(tfm_get_right_margin, RTF_RIGHTMARGIN, numv(f->right_margin))
FMT_GET(tfm_get_indent, RTF_INDENT, numv(f->indent))
FMT_GET(tfm_get_leading, RTF_LEADING, numv(f->leading))
FMT_GET(tfm_get_block_indent, RTF_BLOCKINDENT, numv(f->block_indent))
FMT_GET(tfm_get_bullet, RTF_BULLET, v_bool(f->bullet))
FMT_GET(tfm_get_display, RTF_DISPLAY, v_str(str_intern(a->vm, f->display == RT_DISPLAY_INLINE ? "inline" : f->display == RT_DISPLAY_NONE ? "none" : "block")))
FMT_GET(tfm_get_kerning, RTF_KERNING, v_bool(f->kerning))
FMT_GET(tfm_get_letter_spacing, RTF_LETTERSPACING, numv(f->letter_spacing))

static Value tfm_get_tab_stops(Act *a, AObj *self, int argc, Value *argv) {
    RTFmt *f = fmt_of(self); Value v[RT_MAX_TABS]; int i; (void)argc; (void)argv;
    if (!f) return v_undef();
    if (!(f->has & RTF_TABSTOPS)) return v_null();
    for (i = 0; i < f->ntabs; i++) v[i] = v_num(f->tabs[i]);
    return v_obj(obj_array(a, f->ntabs, v));
}

static int unset_v(Value v) { return v.t == V_UNDEF || v.t == V_NULL; }

static int set_str_field(Act *a, Value v, char *dst, size_t cap, u32 *has, u32 flag, int limit64) {
    AStr *s;
    if (unset_v(v)) { *has &= ~flag; return 1; }
    s = v_str_of(a, v);
    if (a->vm->err) return 0;
    if (limit64 && s->len > 64) s = str_slice(a->vm, s, 0, 64);
    strncpy(dst, str_c(a->vm, s), cap - 1); dst[cap - 1] = 0;
    *has |= flag;
    return 1;
}
static int set_bool_field(Act *a, Value v, u8 *dst, u32 *has, u32 flag) {
    if (unset_v(v)) { *has &= ~flag; return 1; }
    *dst = (u8)v_as_bool(v, a->swf_version); *has |= flag;
    return 1;
}
/* sizes and margins: integers before SWF 8, rounded to even after */
static int set_metric(Act *a, Value v, double *dst, u32 *has, u32 flag, int clamp0) {
    if (unset_v(v)) { *has &= ~flag; return 1; }
    if (a->swf_version < 8) { s32 i = v_i32(a, v); if (a->vm->err) return 0; if (clamp0 && i < 0) i = 0; *dst = i; }
    else { double d = v_num_of(a, v); if (a->vm->err) return 0; if (clamp0 && (d < 0 || d != d)) d = 0; *dst = round_even(d); }   /* f64::max(0.0) drops NaN */
    *has |= flag;
    return 1;
}

#define FMT_SETTER(fn, body) static Value fn(Act *a, AObj *self, int argc, Value *argv) { RTFmt *f = fmt_of(self); Value v = arg(argc, argv, 0); if (!f) return v_undef(); body; return v_undef(); }
FMT_SETTER(tfm_set_font, set_str_field(a, v, f->font, sizeof(f->font), &f->has, RTF_FONT, 1))
FMT_SETTER(tfm_set_size, set_metric(a, v, &f->size, &f->has, RTF_SIZE, 0))
static Value tfm_set_color(Act *a, AObj *self, int argc, Value *argv) {
    RTFmt *f = fmt_of(self); Value v = arg(argc, argv, 0);
    if (!f) return v_undef();
    if (unset_v(v)) f->has &= ~RTF_COLOR;
    else { u32 c = v_u32(a, v); if (!a->vm->err) { f->color = c; f->has |= RTF_COLOR; } }
    return v_undef();
}
FMT_SETTER(tfm_set_url, set_str_field(a, v, f->url, sizeof(f->url), &f->has, RTF_URL, 0))
FMT_SETTER(tfm_set_target, set_str_field(a, v, f->target, sizeof(f->target), &f->has, RTF_TARGET, 0))
FMT_SETTER(tfm_set_bold, set_bool_field(a, v, &f->bold, &f->has, RTF_BOLD))
FMT_SETTER(tfm_set_italic, set_bool_field(a, v, &f->italic, &f->has, RTF_ITALIC))
FMT_SETTER(tfm_set_underline, set_bool_field(a, v, &f->underline, &f->has, RTF_UNDERLINE))
static Value tfm_set_align(Act *a, AObj *self, int argc, Value *argv) {
    RTFmt *f = fmt_of(self); Value v = arg(argc, argv, 0);
    if (!f) return v_undef();
    if (!unset_v(v)) {
        AStr *s = v_str_of(a, v); int al;
        if (a->vm->err) return v_undef();
        al = str_eq_c_ci(s, "left") ? RT_ALIGN_LEFT : str_eq_c_ci(s, "center") ? RT_ALIGN_CENTER : str_eq_c_ci(s, "right") ? RT_ALIGN_RIGHT : str_eq_c_ci(s, "justify") ? RT_ALIGN_JUSTIFY : -1;
        if (al >= 0) { f->align = (u8)al; f->has |= RTF_ALIGN; }
    }
    return v_undef();
}
FMT_SETTER(tfm_set_left_margin, set_metric(a, v, &f->left_margin, &f->has, RTF_LEFTMARGIN, 1))
FMT_SETTER(tfm_set_right_margin, set_metric(a, v, &f->right_margin, &f->has, RTF_RIGHTMARGIN, 1))
FMT_SETTER(tfm_set_indent, set_metric(a, v, &f->indent, &f->has, RTF_INDENT, a->swf_version < 8))
FMT_SETTER(tfm_set_leading, set_metric(a, v, &f->leading, &f->has, RTF_LEADING, a->swf_version < 8))
FMT_SETTER(tfm_set_block_indent, set_metric(a, v, &f->block_indent, &f->has, RTF_BLOCKINDENT, a->swf_version < 8))
FMT_SETTER(tfm_set_bullet, set_bool_field(a, v, &f->bullet, &f->has, RTF_BULLET))
static Value tfm_set_display(Act *a, AObj *self, int argc, Value *argv) {
    RTFmt *f = fmt_of(self); Value v = arg(argc, argv, 0);
    if (!f) return v_undef();
    if (unset_v(v)) { f->display = RT_DISPLAY_BLOCK; f->has |= RTF_DISPLAY; return v_undef(); }
    { AStr *s = v_str_of(a, v); if (a->vm->err) return v_undef(); f->display = (u8)(str_eq_c(s, "inline") ? RT_DISPLAY_INLINE : str_eq_c(s, "none") ? RT_DISPLAY_NONE : RT_DISPLAY_BLOCK); f->has |= RTF_DISPLAY; }
    return v_undef();
}
FMT_SETTER(tfm_set_kerning, set_bool_field(a, v, &f->kerning, &f->has, RTF_KERNING))
static Value tfm_set_letter_spacing(Act *a, AObj *self, int argc, Value *argv) {
    RTFmt *f = fmt_of(self); Value v = arg(argc, argv, 0);
    if (!f) return v_undef();
    if (unset_v(v)) f->has &= ~RTF_LETTERSPACING;
    else { double d = v_num_of(a, v); if (!a->vm->err) { f->letter_spacing = d; f->has |= RTF_LETTERSPACING; } }
    return v_undef();
}
static Value tfm_set_tab_stops(Act *a, AObj *self, int argc, Value *argv) {
    RTFmt *f = fmt_of(self); Value v = arg(argc, argv, 0);
    if (!f) return v_undef();
    if (v.t == V_OBJ) {
        int n = o_length(a, v.u.o), i;
        if (a->vm->err) return v_undef();
        f->ntabs = 0;
        for (i = 0; i < n && f->ntabs < RT_MAX_TABS; i++) { double d = v_num_of(a, o_get_element(a, v.u.o, i)); if (a->vm->err) return v_undef(); f->tabs[f->ntabs++] = round_even(d); }
        f->has |= RTF_TABSTOPS;
    } else f->has &= ~RTF_TABSTOPS;
    return v_undef();
}

static int int_arg(Act *a, Value v, double *dst, u32 *has, u32 flag) {
    if (unset_v(v)) return 1;
    { s32 i = v_i32(a, v); if (a->vm->err) return 0; *dst = i; *has |= flag; }
    return 1;
}

static void align_into(Act *a, Value v, RTFmt *f) {
    AStr *s; int al;
    if (unset_v(v)) return;
    s = v_str_of(a, v);
    if (a->vm->err) return;
    al = str_eq_c_ci(s, "left") ? RT_ALIGN_LEFT : str_eq_c_ci(s, "center") ? RT_ALIGN_CENTER : str_eq_c_ci(s, "right") ? RT_ALIGN_RIGHT : str_eq_c_ci(s, "justify") ? RT_ALIGN_JUSTIFY : -1;
    if (al >= 0) { f->align = (u8)al; f->has |= RTF_ALIGN; }
}

static Value tfm_get_text_extent(Act *a, AObj *self, int argc, Value *argv);
static Value tfm_ctor(Act *a, AObj *self, int argc, Value *argv) {
    RTFmt tmp, *f = &tmp; AObj *t;
    rt_fmt_clear(f);
    f->display = RT_DISPLAY_BLOCK; f->has |= RTF_DISPLAY;   /* a new TextFormat is display:block */
    if (!set_str_field(a, arg(argc, argv, 0), f->font, sizeof(f->font), &f->has, RTF_FONT, 1)) return v_undef();
    if (!int_arg(a, arg(argc, argv, 1), &f->size, &f->has, RTF_SIZE)) return v_undef();
    { Value v = arg(argc, argv, 2); if (!unset_v(v)) { u32 c = v_u32(a, v); if (a->vm->err) return v_undef(); f->color = c; f->has |= RTF_COLOR; } }
    set_bool_field(a, arg(argc, argv, 3), &f->bold, &f->has, RTF_BOLD);
    set_bool_field(a, arg(argc, argv, 4), &f->italic, &f->has, RTF_ITALIC);
    set_bool_field(a, arg(argc, argv, 5), &f->underline, &f->has, RTF_UNDERLINE);
    if (!set_str_field(a, arg(argc, argv, 6), f->url, sizeof(f->url), &f->has, RTF_URL, 0)) return v_undef();
    if (!set_str_field(a, arg(argc, argv, 7), f->target, sizeof(f->target), &f->has, RTF_TARGET, 0)) return v_undef();
    align_into(a, arg(argc, argv, 8), f);
    if (a->vm->err) return v_undef();
    if (!int_arg(a, arg(argc, argv, 9), &f->left_margin, &f->has, RTF_LEFTMARGIN)) return v_undef();
    if (!int_arg(a, arg(argc, argv, 10), &f->right_margin, &f->has, RTF_RIGHTMARGIN)) return v_undef();
    if (!int_arg(a, arg(argc, argv, 11), &f->indent, &f->has, RTF_INDENT)) return v_undef();
    if (!int_arg(a, arg(argc, argv, 12), &f->leading, &f->has, RTF_LEADING)) return v_undef();
    t = o_native_target(self);
    if (t) {
        RTFmt *p = (RTFmt*)malloc(sizeof(RTFmt));
        if (!p) return v_undef();
        *p = tmp;
        t->nk = NK_TEXTFORMAT; t->nat.p = p; t->nfree = fmt_free;
    }
    /* getTextExtent is an own (dontEnum) member of every TextFormat, not on the prototype */
    if (self && !avm1_ruffle_shapes()) o_define(a->vm, self, str_intern(a->vm, "getTextExtent"), v_obj(fn_native(a->vm, env_get(a->vm, a->swf_version), tfm_get_text_extent, NULL)), DE);
    return v_obj(self);
}

static Value tfm_get_text_extent(Act *a, AObj *self, int argc, Value *argv) {
    VM *vm = a->vm; RTFmt *f = fmt_of(self); TextFieldState t; AStr *s; AObj *o; double w = 0; int has_w = 0;
    if (!f) return v_undef();
    s = v_str_of(a, arg(argc, argv, 0)); if (vm->err) return v_undef();
    if (argc > 1 && argv[1].t != V_UNDEF) { w = v_num_of(a, argv[1]); if (vm->err) return v_undef(); has_w = 1; }
    if (has_w && a->swf_version < 8 && devfont_linux_rules()) {
        /* SWF7: no wrapping below the field width of the first two characters */
        Value e[1]; Value r; AStr *two;
        two = str_slice(vm, s, 0, s->len < 2 ? s->len : 2);
        e[0] = v_str(two);
        r = tfm_get_text_extent(a, self, 1, e); if (vm->err) return v_undef();
        if (r.t == V_OBJ) { double tw2 = v_num_of(a, o_get(a, r.u.o, str_intern(vm, "textFieldWidth"))); if (vm->err) return v_undef(); if (w < tw2) has_w = 2; }
    }
    tfs_init(&t, a->swf_version);
    t.player = vm->player;
    rt_fmt_default(&t.deffmt);
    rt_fmt_apply(&t.deffmt, f);
    t.w = (int)(w * 20); t.h = 0; t.autosize = 1; t.wordwrap = has_w == 1;
    if (has_w == 1 && a->swf_version >= 8 && devfont_linux_rules()) t.w -= 20;   /* Linux player, SWF8: lines stay a pixel narrower */
    set_text(vm, &t, s);
    tfs_layout(vm->player, &t);
    tfs_apply_autosize(vm->player, &t);
    o = obj_bare(vm);
    {
        double asc = 0, desc = 0, lw = 0, lh = 0, tfh = t.h, tfw = (has_w == 2 || (has_w && devfont_linux_rules())) ? w * 20 : t.w;
        if (t.lay.nlines > 0) { asc = t.lay.lines[0].ascent; desc = t.lay.lines[0].descent; lw = t.lay.lines[0].width; lh = asc + desc + t.lay.lines[0].leading; }
        if (t.lay.nlines > 1) { lw = t.lay.text_w; lh = t.lay.text_h; }   /* wrapped: the whole text block */
        if (devfont_linux_rules()) {
            /* Linux player 9: twips * 0.05 (later: / 20); SWF7 reports whole pixels */
            if (avm1_player_major() <= 9) { asc *= 0.05; desc *= 0.05; lw *= 0.05; lh *= 0.05; }
            else { asc /= 20.0; desc /= 20.0; lw /= 20.0; lh /= 20.0; }
            tfh /= 20.0; tfw /= 20.0;
            if (a->swf_version < 8) { asc = floor(asc); desc = floor(desc); lw = floor(lw); lh = floor(lh); tfh = floor(tfh); tfw = floor(tfw); }
        } else { asc /= 20.0; desc /= 20.0; lw /= 20.0; lh /= 20.0; tfh /= 20.0; tfw /= 20.0; }
        o_set_data(a, o, str_intern(vm, "ascent"), v_num(asc));
        o_set_data(a, o, str_intern(vm, "descent"), v_num(desc));
        o_set_data(a, o, str_intern(vm, "width"), v_num(lw));
        o_set_data(a, o, str_intern(vm, "height"), v_num(lh));
        o_set_data(a, o, str_intern(vm, "textFieldHeight"), v_num(tfh));
        o_set_data(a, o, str_intern(vm, "textFieldWidth"), v_num(tfw));
    }
    tfs_free(&t);
    return v_obj(o);
}

AObj *lib_textformat(VM *vm, Env *env, AObj *object_proto) {
    AObj *proto, *c = lib_class(vm, env, tfm_ctor, NULL, object_proto, 1, &proto);
    env->p.textformat = proto;
    if (avm1_ruffle_shapes()) defm(vm, env, proto, "getTextExtent", tfm_get_text_extent, DE | DD);
    defp(vm, env, proto, "font", tfm_get_font, tfm_set_font, 0);
    defp(vm, env, proto, "size", tfm_get_size, tfm_set_size, 0);
    defp(vm, env, proto, "color", tfm_get_color, tfm_set_color, 0);
    defp(vm, env, proto, "url", tfm_get_url, tfm_set_url, 0);
    defp(vm, env, proto, "target", tfm_get_target, tfm_set_target, 0);
    defp(vm, env, proto, "bold", tfm_get_bold, tfm_set_bold, 0);
    defp(vm, env, proto, "italic", tfm_get_italic, tfm_set_italic, 0);
    defp(vm, env, proto, "underline", tfm_get_underline, tfm_set_underline, 0);
    defp(vm, env, proto, "align", tfm_get_align, tfm_set_align, 0);
    defp(vm, env, proto, "leftMargin", tfm_get_left_margin, tfm_set_left_margin, 0);
    defp(vm, env, proto, "rightMargin", tfm_get_right_margin, tfm_set_right_margin, 0);
    defp(vm, env, proto, "indent", tfm_get_indent, tfm_set_indent, 0);
    defp(vm, env, proto, "leading", tfm_get_leading, tfm_set_leading, 0);
    defp(vm, env, proto, "blockIndent", tfm_get_block_indent, tfm_set_block_indent, 0);
    defp(vm, env, proto, "tabStops", tfm_get_tab_stops, tfm_set_tab_stops, 0);
    defp(vm, env, proto, "bullet", tfm_get_bullet, tfm_set_bullet, 0);
    defp(vm, env, proto, "display", tfm_get_display, tfm_set_display, 0);
    defp(vm, env, proto, "kerning", tfm_get_kerning, tfm_set_kerning, 0);
    defp(vm, env, proto, "letterSpacing", tfm_get_letter_spacing, tfm_set_letter_spacing, 0);
    return c;
}

/* ---------------------------------------------------------------- */
/* TextField                                                        */

#define TF(fail) MovieClip *c = tf_of(self); TextFieldState *t; if (!c) return fail; t = c->a1->tfs
#define TF_BOOL(get, set, field) \
    static Value get(Act *a, AObj *self, int argc, Value *argv) { TF(v_undef()); (void)a; (void)argc; (void)argv; return v_bool(t->field); } \
    static Value set(Act *a, AObj *self, int argc, Value *argv) { TF(v_undef()); t->field = v_as_bool(arg(argc, argv, 0), a->swf_version) ? 1 : 0; tfs_invalidate(t); return v_undef(); }
TF_BOOL(tf_get_html, tf_set_html, html)
TF_BOOL(tf_get_border, tf_set_border, border)
TF_BOOL(tf_get_background, tf_set_background, background)
TF_BOOL(tf_get_word_wrap, tf_set_word_wrap, wordwrap)
TF_BOOL(tf_get_password, tf_set_password, password)
TF_BOOL(tf_get_multiline, tf_set_multiline, multiline)
TF_BOOL(tf_get_selectable, tf_set_selectable, selectable)
TF_BOOL(tf_get_condense_white, tf_set_condense_white, condense_white)
TF_BOOL(tf_get_mouse_wheel, tf_set_mouse_wheel, mouse_wheel)
TF_BOOL(tf_get_embed_fonts, tf_set_embed_fonts, embed)

static Value tf_get_text(Act *a, AObj *self, int argc, Value *argv) { TF(v_undef()); (void)argc; (void)argv; return v_str(tf_text(a->vm, t)); }
static Value tf_set_text(Act *a, AObj *self, int argc, Value *argv) {
    AStr *s; TF(v_undef());
    s = v_str_of(a, arg(argc, argv, 0)); if (a->vm->err) return v_undef();
    set_text(a->vm, t, s);
    propagate_binding(a->vm, c);
    return v_undef();
}
static Value tf_get_html_text(Act *a, AObj *self, int argc, Value *argv) { TF(v_undef()); (void)argc; (void)argv; return v_str(tf_html_text(a->vm, t)); }
static Value tf_set_html_text(Act *a, AObj *self, int argc, Value *argv) {
    AStr *s; TF(v_undef());
    s = v_str_of(a, arg(argc, argv, 0)); if (a->vm->err) return v_undef();
    set_html_text(a->vm, t, s);
    return v_undef();
}
static Value tf_get_text_color(Act *a, AObj *self, int argc, Value *argv) { TF(v_undef()); (void)a; (void)argc; (void)argv; return v_num((double)(t->deffmt.color & 0xFFFFFF)); }
static Value tf_set_text_color(Act *a, AObj *self, int argc, Value *argv) {
    RTFmt f; u32 rgb; TF(v_undef());
    rgb = v_u32(a, arg(argc, argv, 0)); if (a->vm->err) return v_undef();
    rt_fmt_clear(&f); f.color = rgb & 0xFFFFFF; f.has = RTF_COLOR;
    rt_set_format(&t->rt, 0, t->rt.len, &f);
    t->deffmt.color = f.color;
    tfs_invalidate(t);
    return v_undef();
}
static Value tf_get_bg_color(Act *a, AObj *self, int argc, Value *argv) { TF(v_undef()); (void)a; (void)argc; (void)argv; return v_num((double)(t->bgcolor & 0xFFFFFF)); }
static Value tf_set_bg_color(Act *a, AObj *self, int argc, Value *argv) { u32 v; TF(v_undef()); v = v_u32(a, arg(argc, argv, 0)); if (a->vm->err) return v_undef(); t->bgcolor = v & 0xFFFFFF; return v_undef(); }
static Value tf_get_border_color(Act *a, AObj *self, int argc, Value *argv) { TF(v_undef()); (void)a; (void)argc; (void)argv; return v_num((double)(t->bordercolor & 0xFFFFFF)); }
static Value tf_set_border_color(Act *a, AObj *self, int argc, Value *argv) { u32 v; TF(v_undef()); v = v_u32(a, arg(argc, argv, 0)); if (a->vm->err) return v_undef(); t->bordercolor = v & 0xFFFFFF; return v_undef(); }
static Value tf_get_length(Act *a, AObj *self, int argc, Value *argv) { TF(v_undef()); (void)a; (void)argc; (void)argv; return v_num(t->rt.len); }
static Value tf_get_text_width(Act *a, AObj *self, int argc, Value *argv) {
    double w; TF(v_undef()); (void)argc; (void)argv;
    tfs_layout(a->vm->player, t);
    w = t->lay.text_w / 20.0;
    if (devfont_linux_rules() && !t->embed && c->self) {
        /* Linux player: device text width divided by the field's horizontal scale */
        double sx = sqrt(c->self->matrix.sx * c->self->matrix.sx + c->self->matrix.r0 * c->self->matrix.r0);
        if (sx > 0) w /= sx;
    }
    return v_num(floor(w));
}
static Value tf_get_text_height(Act *a, AObj *self, int argc, Value *argv) { TF(v_undef()); (void)argc; (void)argv; tfs_layout(a->vm->player, t); return v_num(floor(t->lay.text_h / 20.0)); }
static Value tf_get_scroll(Act *a, AObj *self, int argc, Value *argv) { int m; TF(v_undef()); (void)argc; (void)argv; m = tfs_maxscroll_v(a->vm->player, t); if (t->scroll_v > m) t->scroll_v = m; if (t->scroll_v < 1) t->scroll_v = 1; return v_num(t->scroll_v); }
/* EditText::set_scroll: 1 when it changed; a user scroll broadcasts onScroller */
int a1_tf_set_scroll(VM *vm, MovieClip *c, double scroll, int programmatic) {
    TextFieldState *t = tfs_of(c); int m, lines;
    if (!t) return 0;
    /* (Flash overflows somewhere above 767100486418432.9) */
    lines = (scroll != scroll || scroll < 0 || scroll >= 767100486418433.0) ? 1 : (scroll > 2147483647.0 ? 2147483647 : (int)scroll);
    m = tfs_maxscroll_v(vm->player, t);
    if (lines > m) lines = m;
    if (lines < 1) lines = 1;
    if (t->scroll_v == lines) return 0;
    t->scroll_v = lines;
    if (!programmatic && c->obj) {
        Act a; Value args[2];
        act_init_nothing(&a, vm, c, "[On Scroller]");
        args[0] = v_str(str_intern(vm, "onScroller")); args[1] = v_obj((AObj*)c->obj);
        o_call_method(&a, (AObj*)c->obj, vm->s_broadcastMessage, 2, args, ER_SPECIAL);
        if (vm->err) vm_report_error(&a);
    }
    return 1;
}
static Value tf_set_scroll(Act *a, AObj *self, int argc, Value *argv) {
    double v; TF(v_undef()); (void)t;
    v = v_num_of(a, arg(argc, argv, 0)); if (a->vm->err) return v_undef();
    a1_tf_set_scroll(a->vm, c, v, 1);
    return v_undef();
}
static Value tf_get_maxscroll(Act *a, AObj *self, int argc, Value *argv) { TF(v_undef()); (void)argc; (void)argv; return v_num(tfs_maxscroll_v(a->vm->player, t)); }
static Value tf_get_bottom_scroll(Act *a, AObj *self, int argc, Value *argv) { TF(v_undef()); (void)argc; (void)argv; return v_num(tfs_bottom_scroll_v(a->vm->player, t)); }
static Value tf_get_hscroll(Act *a, AObj *self, int argc, Value *argv) { TF(v_undef()); (void)a; (void)argc; (void)argv; return v_num(floor(t->scroll_h / 20.0)); }
static Value tf_set_hscroll(Act *a, AObj *self, int argc, Value *argv) {
    s32 v; double m; TF(v_undef());
    v = v_i32(a, arg(argc, argv, 0)); if (a->vm->err) return v_undef();
    m = tfs_maxscroll_h(a->vm->player, t) / 20.0;
    { double d = v < 0 ? 0 : (double)v; if (d > m) d = m; t->scroll_h = d * 20.0; }
    return v_undef();
}
static Value tf_get_maxhscroll(Act *a, AObj *self, int argc, Value *argv) { TF(v_undef()); (void)argc; (void)argv; return v_num(floor(tfs_maxscroll_h(a->vm->player, t) / 20.0)); }
static Value tf_get_max_chars(Act *a, AObj *self, int argc, Value *argv) { TF(v_undef()); (void)a; (void)argc; (void)argv; if (!t->max_chars) return v_null(); return v_num(t->max_chars); }
/* (a negative maxChars is kept, and limits nothing: Gnash TextField.as) */
static Value tf_set_max_chars(Act *a, AObj *self, int argc, Value *argv) { s32 v; TF(v_undef()); v = v_i32(a, arg(argc, argv, 0)); if (a->vm->err) return v_undef(); t->max_chars = v; return v_undef(); }
static Value tf_get_type(Act *a, AObj *self, int argc, Value *argv) { TF(v_undef()); (void)argc; (void)argv; return v_str(str_intern(a->vm, t->input ? "input" : "dynamic")); }
static Value tf_set_type(Act *a, AObj *self, int argc, Value *argv) {
    AStr *s; TF(v_undef());
    s = v_str_of(a, arg(argc, argv, 0)); if (a->vm->err) return v_undef();
    if (str_eq_c_ci(s, "input")) t->input = 1; else if (str_eq_c_ci(s, "dynamic")) t->input = 0;
    return v_undef();
}
static Value tf_get_auto_size(Act *a, AObj *self, int argc, Value *argv) { TF(v_undef()); (void)argc; (void)argv; return v_str(str_intern(a->vm, t->autosize == 0 ? "none" : t->autosize == 2 ? "center" : t->autosize == 3 ? "right" : "left")); }
static Value tf_set_auto_size(Act *a, AObj *self, int argc, Value *argv) {
    Value v = arg(argc, argv, 0); TF(v_undef());
    if (v.t == V_BOOL) t->autosize = v.u.b ? 1 : 0;
    else {
        AStr *s = v_str_of(a, v); if (a->vm->err) return v_undef();
        t->autosize = str_eq_c_ci(s, "left") ? 1 : str_eq_c_ci(s, "center") ? 2 : str_eq_c_ci(s, "right") ? 3 : 0;
    }
    tfs_invalidate(t);
    return v_undef();
}
static Value tf_get_variable(Act *a, AObj *self, int argc, Value *argv) { TF(v_undef()); (void)a; (void)t; (void)argc; (void)argv; return c->a1->tf_var ? v_str(c->a1->tf_var) : v_null(); }
static Value tf_set_variable(Act *a, AObj *self, int argc, Value *argv) {
    Value v = arg(argc, argv, 0); A1Clip *s; TF(v_undef());
    s = c->a1;
    if (s->tf_bound) { clear_binding(s->tf_bound, c); s->tf_bound = NULL; }
    else unbound_remove(c);
    if (avm1_player_major() <= 17) {
        /* Flash Player 17 and older keep the text and do not write it to the
           variable (Gnash TextField.as); later players start over from the
           field's initial text (Ruffle's textfield_variable) */
        if (v.t == V_UNDEF || v.t == V_NULL) s->tf_var = NULL;
        else { s->tf_var = v_str_of(a, v); if (a->vm->err) return v_undef(); }
        if (!try_bind(a->vm, c, 2)) unbound_add(c);
        return v_undef();
    }
    if (c->self && c->self->ch && c->self->ch->type == CHAR_EDITTEXT) {
        TextFieldState tmp; tfs_init(&tmp, t->swf_version);
        edittext_init_state(a->vm->player, c->self->ch, &tmp);
        set_text(a->vm, t, tf_text(a->vm, &tmp));
        tfs_free(&tmp);
    } else set_text(a->vm, t, a->vm->s_empty);
    if (v.t == V_UNDEF || v.t == V_NULL) s->tf_var = NULL;
    else { s->tf_var = v_str_of(a, v); if (a->vm->err) return v_undef(); }
    if (!try_bind(a->vm, c, 1)) unbound_add(c);
    return v_undef();
}
static Value tf_get_restrict(Act *a, AObj *self, int argc, Value *argv) { TF(v_undef()); (void)argc; (void)argv; return t->restrict_chars ? v_str(str_cstr(a->vm, t->restrict_chars)) : v_null(); }
static Value tf_set_restrict(Act *a, AObj *self, int argc, Value *argv) {
    Value v = arg(argc, argv, 0); TF(v_undef());
    free(t->restrict_chars); t->restrict_chars = NULL;
    if (v.t != V_UNDEF && v.t != V_NULL) {
        AStr *s = v_str_of(a, v); const char *u;
        if (a->vm->err) return v_undef();
        /* "" is null for Flash Player 18 and later (Ruffle's edittext_restrict), kept by older ones (Gnash TextField.as) */
        if (s->len || avm1_player_major() <= 17) { u = str_c(a->vm, s); t->restrict_chars = (char*)malloc(strlen(u) + 1); if (t->restrict_chars) strcpy(t->restrict_chars, u); }
    }
    return v_undef();
}
static Value tf_get_tab_index(Act *a, AObj *self, int argc, Value *argv) { TF(v_undef()); (void)a; (void)t; (void)argc; (void)argv; if (!c->a1->has_tab) return v_undef(); return v_num((double)(u32)c->a1->tab_index); }
static Value tf_set_tab_index(Act *a, AObj *self, int argc, Value *argv) {
    Value v = arg(argc, argv, 0); TF(v_undef()); (void)t;
    if (v.t == V_UNDEF || v.t == V_NULL) c->a1->has_tab = 0;
    else { u32 i = v_u32(a, v); if (a->vm->err) return v_undef(); c->a1->tab_index = (s32)i; c->a1->has_tab = (s32)i != -1; }
    return v_undef();
}
static Value tf_get_style_sheet(Act *a, AObj *self, int argc, Value *argv) { TF(v_undef()); (void)a; (void)t; (void)argc; (void)argv; return c->a1->style_sheet ? v_obj(c->a1->style_sheet) : v_undef(); }
static Value tf_set_style_sheet(Act *a, AObj *self, int argc, Value *argv) {
    Value v = arg(argc, argv, 0); TF(v_undef());
    c->a1->style_sheet = v.t == V_OBJ ? v.u.o : NULL;
    t->css = c->a1->style_sheet ? ss_data_of(c->a1->style_sheet) : NULL;
    if (!c->a1->style_sheet) { free(t->html_source); t->html_source = NULL; }
    if (t->html_source) { AStr *h = str_cstr(a->vm, t->html_source); parse_html_css(a->vm, t, h); }
    tfs_invalidate(t);
    return v_undef();
}
static Value tf_get_grid_fit(Act *a, AObj *self, int argc, Value *argv) { static const char *const n[] = { "none", "pixel", "subpixel" }; TF(v_undef()); (void)argc; (void)argv; return v_str(str_intern(a->vm, n[t->grid_fit >= 0 && t->grid_fit <= 2 ? t->grid_fit : 0])); }
static Value tf_set_grid_fit(Act *a, AObj *self, int argc, Value *argv) {
    AStr *s; TF(v_undef());
    if (argc < 1) return v_undef();
    s = v_str_of(a, argv[0]); if (a->vm->err) return v_undef();
    if (str_eq_c(s, "none")) t->grid_fit = 0; else if (str_eq_c(s, "pixel")) t->grid_fit = 1; else if (str_eq_c(s, "subpixel")) t->grid_fit = 2;
    return v_undef();
}
static Value tf_get_aa(Act *a, AObj *self, int argc, Value *argv) { TF(v_undef()); (void)argc; (void)argv; return v_str(str_intern(a->vm, t->aa_advanced ? "advanced" : "normal")); }
static Value tf_set_aa(Act *a, AObj *self, int argc, Value *argv) {
    AStr *s; TF(v_undef());
    if (argc < 1) return v_undef();
    s = v_str_of(a, argv[0]); if (a->vm->err) return v_undef();
    if (str_eq_c(s, "normal")) t->aa_advanced = 0; else if (str_eq_c(s, "advanced")) t->aa_advanced = 1;
    return v_undef();
}
static Value tf_get_thickness(Act *a, AObj *self, int argc, Value *argv) { TF(v_undef()); (void)a; (void)argc; (void)argv; return v_num(t->thickness); }
static Value tf_get_sharpness(Act *a, AObj *self, int argc, Value *argv) { TF(v_undef()); (void)a; (void)argc; (void)argv; return v_num(t->sharpness); }
static Value tf_set_thickness(Act *a, AObj *self, int argc, Value *argv) { double d; TF(v_undef()); if (argc < 1) return v_undef(); d = v_num_of(a, argv[0]); if (a->vm->err) return v_undef(); t->thickness = (float)d; return v_undef(); }
static Value tf_set_sharpness(Act *a, AObj *self, int argc, Value *argv) { double d; TF(v_undef()); if (argc < 1) return v_undef(); d = v_num_of(a, argv[0]); if (a->vm->err) return v_undef(); t->sharpness = (float)d; return v_undef(); }
static Value tf_get_filters(Act *a, AObj *self, int argc, Value *argv) { extern Value a1_get_filters(Act *a, MovieClip *c); TF(v_undef()); (void)t; (void)argc; (void)argv; return a1_get_filters(a, c); }
static Value tf_set_filters(Act *a, AObj *self, int argc, Value *argv) { extern void a1_set_filters(Act *a, MovieClip *c, Value v); TF(v_undef()); (void)t; a1_set_filters(a, c, arg(argc, argv, 0)); return v_undef(); }

static Value tf_get_text_format(Act *a, AObj *self, int argc, Value *argv) {
    s32 b = 0, e = 0x7FFFFFFF; RTFmt f; AObj *o; TF(v_undef());
    if (argc >= 2) { b = v_i32(a, argv[0]); if (a->vm->err) return v_undef(); e = v_i32(a, argv[1]); if (a->vm->err) return v_undef(); }
    else if (argc == 1) { b = v_i32(a, argv[0]); if (a->vm->err) return v_undef(); e = (s32)((u32)b + 1u); }
    if (b < 0) b = 0; if (b > t->rt.len) b = t->rt.len;
    if (e < 0) e = 0; if (e > t->rt.len) e = t->rt.len;
    rt_fmt_clear(&f);
    if (b < e) rt_get_format(&t->rt, b, e, &f);
    else { f.display = RT_DISPLAY_BLOCK; f.has |= RTF_DISPLAY; }   /* an empty range: TextFormat::default() */
    o = a1_new_textformat(a, &f);
    return o ? v_obj(o) : v_undef();
}
static Value tf_set_text_format(Act *a, AObj *self, int argc, Value *argv) {
    s32 b, e; Value fv, thrown = v_undef(); RTFmt *f; int err = 0; TF(v_undef());
    if (argc >= 3) { b = v_i32(a, argv[0]); if (a->vm->err) return v_undef(); e = v_i32(a, argv[1]); if (a->vm->err) return v_undef(); fv = argv[2]; if (b < 0) b = 0; if (e < 0) e = 0; }
    else if (argc == 2) {
        /* a throwing index still formats index 0, then the error goes on */
        b = v_i32(a, argv[0]);
        if (a->vm->err) { if (a->vm->err != ERR_THROWN) return v_undef(); err = a->vm->err; thrown = a->vm->thrown; a->vm->err = 0; b = 0; }
        e = (s32)((u32)b + 1u); if (e < 0) e = 0; if (b < 0) b = 0; fv = argv[1];
    }
    else if (argc == 1) { b = 0; e = t->rt.len; fv = argv[0]; }
    else return v_undef();
    if (fv.t == V_OBJ && (f = fmt_of(fv.u.o)) != NULL) {
        if (b > t->rt.len) b = t->rt.len;
        if (e > t->rt.len) e = t->rt.len;
        if (b < e) rt_set_format(&t->rt, b, e, f);
        tfs_invalidate(t);
    }
    if (err) { a->vm->err = err; a->vm->thrown = thrown; }
    return v_undef();
}
static Value tf_get_new_text_format(Act *a, AObj *self, int argc, Value *argv) { AObj *o; TF(v_undef()); (void)argc; (void)argv; o = a1_new_textformat(a, &t->deffmt); return o ? v_obj(o) : v_undef(); }
static Value tf_set_new_text_format(Act *a, AObj *self, int argc, Value *argv) {
    RTFmt *f; TF(v_undef()); (void)a;
    if (argc < 1 || argv[0].t != V_OBJ || !(f = fmt_of(argv[0].u.o))) return v_undef();
    rt_fmt_apply(&t->deffmt, f);
    return v_undef();
}
static void replace_range(VM *vm, TextFieldState *t, int from, int to, AStr *s) {
    int n, i, k = 0; u32 *u = astr_cps(vm, s, &n);
    for (i = 0; i < n; i++) u[k++] = u[i] == '\n' ? '\r' : u[i];
    if (from < 0) from = 0; if (to > t->rt.len) to = t->rt.len; if (from > to) from = to;
    rt_replace(&t->rt, from, to, u, k, &t->deffmt);
    free(u);
    tfs_invalidate(t);
}
static Value tf_replace_sel(Act *a, AObj *self, int argc, Value *argv) {
    AStr *s; int b, e; TF(v_undef());
    if (argc < 1) return v_undef();   /* (Gnash TextField.as: replaceSel() changes nothing) */
    s = v_str_of(a, argv[0]); if (a->vm->err) return v_undef();
    if (!s->len && a->swf_version < 8) return v_undef();   /* SWF 7 and older: "" changes nothing either */
    clamp_sel(t);
    b = t->sel_begin < t->sel_end ? t->sel_begin : t->sel_end; e = t->sel_begin < t->sel_end ? t->sel_end : t->sel_begin;
    replace_range(a->vm, t, b, e, s);
    t->sel_begin = t->sel_end = b + (int)s->len; t->has_sel = 1;
    clamp_sel(t);
    propagate_binding(a->vm, c);
    return v_undef();
}
static Value tf_replace_text(Act *a, AObj *self, int argc, Value *argv) {
    double f, to; AStr *s; TF(v_undef());
    if (argc < 3) return v_undef();   /* (Gnash TextField.as) */
    f = v_num_of(a, arg(argc, argv, 0)); if (a->vm->err) return v_undef();
    to = v_num_of(a, arg(argc, argv, 1)); if (a->vm->err) return v_undef();
    s = v_str_of(a, arg(argc, argv, 2)); if (a->vm->err) return v_undef();
    if (f < 0 || to < 0) return v_undef();   /* a negative index changes nothing */
    replace_range(a->vm, t, f != f || f < 0 ? 0 : f > 2147483647.0 ? 2147483647 : (int)f, to != to || to < 0 ? 0 : to > 2147483647.0 ? 2147483647 : (int)to, s);
    return v_undef();
}
static Value tf_remove_text_field(Act *a, AObj *self, int argc, Value *argv) { TF(v_undef()); (void)t; (void)argc; (void)argv; a1_remove_display_object(a, c); return v_undef(); }
static Value tf_get_depth(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = o_display(self); (void)a; (void)argc; (void)argv;
    if (c && c->self) return v_num((s32)((u32)c->self->depth - 16384u));
    return v_undef();
}

/* TextField.getFontList(): the device fonts (the Windows player's names for the ones we draw) */
static Value tf_get_font_list(Act *a, AObj *self, int argc, Value *argv) {
    Value v[3]; (void)self; (void)argc; (void)argv;
    v[0] = v_str(str_intern(a->vm, "Arial")); v[1] = v_str(str_intern(a->vm, "Courier New")); v[2] = v_str(str_intern(a->vm, "Times New Roman"));
    return v_obj(obj_array(a, 3, v));
}

/* new TextField / super(): not a real text field; it only removes a retypable native type */
static Value tf_ctor(Act *a, AObj *self, int argc, Value *argv) {
    Value pv; AObj *po; (void)argc; (void)argv;
    o_native_retarget(self, NK_EDITTEXT);
    /* (the constructor gives the prototype its text properties too: Ruffle's textfield_props) */
    if (!o_display(self) && o_get_local_stored(a, self, a->vm->s_proto, &pv) && (po = v_obj_of(a, pv)) != NULL) a1_tf_populate_proto(a, po);
    if (a->vm->err) a->vm->err = 0;
    {   /* the Linux player: "new TextField()" has its own listener list */
        extern int devfont_linux_rules(void);
        if (devfont_linux_rules() && !o_display(self)) { AObj *l = obj_array(a, 0, NULL); if (l) o_define(a->vm, self, a->vm->s__listeners, v_obj(l), PA_DONT_ENUM | PA_DONT_DELETE); }
    }
    return v_obj(self);
}
/* Flash gives TextField.prototype its text properties when the first text
   field is made (Gnash TextField.as; Flash's own TextField.prototype listing
   has only the methods, filters and the anti-aliasing settings) */
static const char *const tf_pnames[30] = { "scroll", "maxscroll", "borderColor", "backgroundColor", "textColor", "tabIndex", "autoSize", "text", "type", "htmlText", "variable", "hscroll", "maxhscroll", "maxChars", "embedFonts", "html", "border", "background", "wordWrap", "password", "multiline", "selectable", "length", "bottomScroll", "textWidth", "textHeight", "restrict", "condenseWhite", "mouseWheelEnabled", "styleSheet" };
static const NativeFn tf_pget[30] = { tf_get_scroll, tf_get_maxscroll, tf_get_border_color, tf_get_bg_color, tf_get_text_color, tf_get_tab_index, tf_get_auto_size, tf_get_text, tf_get_type, tf_get_html_text, tf_get_variable, tf_get_hscroll, tf_get_maxhscroll, tf_get_max_chars, tf_get_embed_fonts, tf_get_html, tf_get_border, tf_get_background, tf_get_word_wrap, tf_get_password, tf_get_multiline, tf_get_selectable, tf_get_length, tf_get_bottom_scroll, tf_get_text_width, tf_get_text_height, tf_get_restrict, tf_get_condense_white, tf_get_mouse_wheel, tf_get_style_sheet };
static const NativeFn tf_pset[30] = { tf_set_scroll, NULL, tf_set_border_color, tf_set_bg_color, tf_set_text_color, tf_set_tab_index, tf_set_auto_size, tf_set_text, tf_set_type, tf_set_html_text, tf_set_variable, tf_set_hscroll, NULL, tf_set_max_chars, tf_set_embed_fonts, tf_set_html, tf_set_border, tf_set_background, tf_set_word_wrap, tf_set_password, tf_set_multiline, tf_set_selectable, NULL, NULL, NULL, NULL, tf_set_restrict, tf_set_condense_white, tf_set_mouse_wheel, tf_set_style_sheet };
static void a1_tf_populate_proto(Act *a, AObj *proto) {
    VM *vm = a->vm; Env *env = ENV(a); int i;
    if (!proto) return;
    for (i = 0; i < 30; i++) {
        AStr *n = str_intern(vm, tf_pnames[i]);
        if (o_has_own_property(a, proto, n)) continue;
        o_add_property(vm, proto, n, fn_native(vm, env, tf_pget[i], NULL), tf_pset[i] ? fn_native(vm, env, tf_pset[i], NULL) : NULL, 0);
    }
}

AObj *lib_textfield(VM *vm, Env *env, AObj *object_proto) {
    AObj *proto, *c = lib_class(vm, env, tf_ctor, NULL, object_proto, 1, &proto);
    u16 m = DE | DD | V6;
    env->p.textfield = proto;
    defm(vm, env, proto, "replaceSel", tf_replace_sel, m);
    defm(vm, env, proto, "getTextFormat", tf_get_text_format, m);
    defm(vm, env, proto, "setTextFormat", tf_set_text_format, m);
    defm(vm, env, proto, "removeTextField", tf_remove_text_field, m);
    defm(vm, env, proto, "getNewTextFormat", tf_get_new_text_format, m);
    defm(vm, env, proto, "setNewTextFormat", tf_set_new_text_format, m);
    defm(vm, env, proto, "getDepth", tf_get_depth, DE | DD | RO | V6);
    defm(vm, env, proto, "replaceText", tf_replace_text, DE | DD | V7);
    defp(vm, env, proto, "gridFitType", tf_get_grid_fit, tf_set_grid_fit, V8);
    defp(vm, env, proto, "antiAliasType", tf_get_aa, tf_set_aa, V8);
    defp(vm, env, proto, "thickness", tf_get_thickness, tf_set_thickness, V8);
    defp(vm, env, proto, "sharpness", tf_get_sharpness, tf_set_sharpness, V8);
    defp(vm, env, proto, "filters", tf_get_filters, tf_set_filters, DD | V8);
    /* the text properties come with the first text field (a1_tf_populate_proto);
       Ruffle declares them up front and has no getFontList */
    if (avm1_ruffle_shapes()) { int i; for (i = 0; i < 30; i++) o_add_property(vm, proto, str_intern(vm, tf_pnames[i]), fn_native(vm, env, tf_pget[i], NULL), tf_pset[i] ? fn_native(vm, env, tf_pset[i], NULL) : NULL, 0); }
    /* Flash runs ASSetPropFlags(TextField, null, 131): SWF5 movies see no TextField
     * prototype, and TextField objects there inherit nothing from Object */
    else defm(vm, env, c, "getFontList", tf_get_font_list, DE);
    o_set_attributes(vm, c, NULL, DE | DD | V6, 0);
    o_set_attributes(vm, proto, vm->s_proto, V6, 0);
    return c;
}

/* ---------------------------------------------------------------- */
/* editing from the player (typing into a focused field)            */

static void a1_changed(VM *vm, MovieClip *c);
/* text typed or pasted at the selection: restrict, then maxChars */
static void a1_insert(VM *vm, MovieClip *c, const u32 *in, int n) {
    extern u32 tfs_restrict_char(TextFieldState *t, u32 ch);
    TextFieldState *t = tfs_of(c); u32 *buf; int k = 0, i, b, e, avail;
    clamp_sel(t);
    b = t->sel_begin < t->sel_end ? t->sel_begin : t->sel_end; e = t->sel_begin < t->sel_end ? t->sel_end : t->sel_begin;
    buf = (u32*)malloc(((size_t)n + 1) * sizeof(u32));
    if (!buf) return;
    for (i = 0; i < n; i++) {
        u32 ch = in[i];
        if (ch == '\n') ch = '\r';
        if (ch == '\r') { if (!t->multiline) continue; buf[k++] = ch; continue; }
        if ((ch < 32 && ch != '\t') || ch == 127) continue;
        ch = tfs_restrict_char(t, ch);
        if (ch) buf[k++] = ch;
    }
    if (t->max_chars > 0) { avail = t->max_chars - (t->rt.len - (e - b)); if (avail < 0) avail = 0; if (k > avail) k = avail; }
    if (!k && b == e) { free(buf); return; }
    rt_replace(&t->rt, b, e, buf, k, &t->deffmt);
    free(buf);
    t->sel_begin = t->sel_end = b + k; t->has_sel = 1;
    a1_changed(vm, c);
}

void a1_tf_typed(VM *vm, MovieClip *c, int ch) {
    TextFieldState *t = c ? tfs_of(c) : NULL; int b, e; u32 u = (u32)ch;
    if (!vm || !t) return;
    clamp_sel(t);
    b = t->sel_begin < t->sel_end ? t->sel_begin : t->sel_end; e = t->sel_begin < t->sel_end ? t->sel_end : t->sel_begin;
    if (ch == 8) {
        if (b == e && b > 0) b--;
        if (b == e) return;
        rt_replace(&t->rt, b, e, NULL, 0, &t->deffmt); t->sel_begin = t->sel_end = b; t->has_sel = 1;
        a1_changed(vm, c);
        return;
    }
    if ((ch == 13 || ch == 10) && !t->multiline) return;
    if (t->max_chars && t->rt.len - (e - b) >= t->max_chars) return;
    a1_insert(vm, c, &u, 1);
}

TextFieldState *a1_textfield_state(MovieClip *c) { return tfs_of(c); }

/* a press on a link: asfunction:name[,arg] calls the function (resolved
   from the field's parent, `this` is the object it was found on), other
   URLs are opened like getURL */
/* is there a link under stage point (sx, sy) (twips)? */
extern void clip_world_matrix(MovieClip *c, SWFMatrix *out);
int a1_tf_link_url_at(VM *vm, MovieClip *c, double sx, double sy) {
    TextFieldState *t = tfs_of(c); SWFMatrix w; double a, b, cc, d, e, f2, lx, ly; int ci; const RTFmt *f;
    if (!t) return 0;
    clip_world_matrix(c, &w);
    if (!matrix_invert(&w, &a, &b, &cc, &d, &e, &f2)) return 0;
    lx = a * sx + b * sy + e; ly = cc * sx + d * sy + f2;
    ci = tfs_char_at_point(vm->player, t, lx - t->xmin + t->scroll_h, ly - t->ymin);   /* (horizontal scrolling counts) */
    if (ci < 0 || ci >= t->rt.len) return 0;
    f = rt_format_at(&t->rt, ci);
    return f && f->url[0];
}
extern void avm1_run_actions(VM *vm);
void a1_tf_link_at(VM *vm, MovieClip *c, int pos) {
    extern void avm1_navigate(Act *a, AStr *url, AStr *target, int method, AObj *vars, int normalize);
    TextFieldState *t = tfs_of(c); const RTFmt *f; MovieClip *parent; Act a;
    if (!t || pos < 0 || pos >= t->rt.len) return;
    f = rt_format_at(&t->rt, pos);
    if (!f || !f->url[0]) return;
    parent = clip_avm1_parent(c);
    if (!parent) return;
    act_init_clip(&a, vm, parent);
    if (!strncmp(f->url, "asfunction:", 11)) {
        const char *addr = f->url + 11, *comma = strchr(addr, ',');
        AStr *name = comma ? str_from_swf(vm, addr, (size_t)(comma - addr), 6) : str_intern(vm, addr);
        AObj *owner = NULL; Value fn, arg;
        fn = act_get_variable(&a, name, &owner);
        if (!vm->err && fn.t == V_OBJ && fn.u.o) {
            Value self = owner ? v_obj(owner) : v_undef();
            if (comma) { arg = v_str(str_intern(vm, comma + 1)); o_call(&a, fn.u.o, self, 1, &arg); }
            else o_call(&a, fn.u.o, self, 0, NULL);
        }
        if (vm->err) vm_report_error(&a);
    } else {
        avm1_navigate(&a, str_intern(vm, f->url), str_intern(vm, f->target), 0, NULL, 1);
        if (vm->err) vm_report_error(&a);
    }
    avm1_run_actions(vm);
}

/* IME composition (Ruffle EditText::ime): the preedit text lives in the
   field between ime_start and ime_end without change notifications */
static void ime_replace(TextFieldState *t, int b, int e, const char *u8) {
    u32 *u = NULL; int n = u8 && *u8 ? rt_utf8_decode(u8, (int)strlen(u8), &u) : 0;
    if (b > t->rt.len) b = t->rt.len; if (e > t->rt.len) e = t->rt.len; if (e < b) e = b;
    rt_replace(&t->rt, b, e, u, n, &t->deffmt);
    free(u);
    tfs_invalidate(t);
}
static int ime_len(const char *u8) { u32 *u = NULL; int n = u8 && *u8 ? rt_utf8_decode(u8, (int)strlen(u8), &u) : 0; free(u); return n; }
static void ime_finish(TextFieldState *t) {
    if (!t->ime_on) return;
    ime_replace(t, t->ime_start, t->ime_end, NULL);
    t->sel_begin = t->sel_end = t->ime_start; t->has_sel = 1;
    t->ime_on = 0; free(t->ime_text); t->ime_text = NULL;
}
void a1_tf_ime_preedit(VM *vm, MovieClip *c, const char *u8, int from, int to) {
    TextFieldState *t = c ? tfs_of(c) : NULL;
    if (!vm || !t) return;
    tfs_ime_preedit(t, u8, from, to);
}
/* the composition ends: its text leaves the field; returns it (malloc'd) or NULL */
char *tfs_ime_take(TextFieldState *t) {
    char *txt;
    if (!t || !t->ime_on) return NULL;
    txt = t->ime_text; t->ime_text = NULL;
    ime_finish(t);
    return txt;
}
void tfs_ime_preedit(TextFieldState *t, const char *u8, int from, int to) {
    int n;
    if (!u8 || !*u8) { ime_finish(t); return; }
    if (!t->ime_on) {
        int b, e;
        if (t->has_sel) { clamp_sel(t); b = t->sel_begin < t->sel_end ? t->sel_begin : t->sel_end; e = t->sel_begin < t->sel_end ? t->sel_end : t->sel_begin; }
        else b = e = t->rt.len;
        ime_replace(t, b, e, NULL);
        t->ime_on = 1; t->ime_start = t->ime_end = b;
    }
    n = ime_len(u8);
    ime_replace(t, t->ime_start, t->ime_end, u8);
    t->ime_end = t->ime_start + n;
    free(t->ime_text); t->ime_text = (char*)malloc(strlen(u8) + 1); if (t->ime_text) strcpy(t->ime_text, u8);
    if (from >= 0) { t->sel_begin = t->ime_start + from; t->sel_end = t->ime_start + to; t->has_sel = 1; }
    else t->has_sel = 0;
}
void a1_tf_ime_commit(VM *vm, MovieClip *c, const char *u8) {
    TextFieldState *t = c ? tfs_of(c) : NULL; u32 *u = NULL; int n;
    if (!vm || !t || !u8) return;
    n = rt_utf8_decode(u8, (int)strlen(u8), &u);
    if (u && n) a1_insert(vm, c, u, n);
    free(u);
}
/* focus lost: the composition is committed as typed text */
void a1_tf_ime_commit_pending(VM *vm, MovieClip *c) {
    TextFieldState *t = c ? tfs_of(c) : NULL; char *txt;
    if (!vm || !t || !t->ime_on) return;
    txt = t->ime_text; t->ime_text = NULL;
    ime_finish(t);
    if (txt) { a1_tf_ime_commit(vm, c, txt); free(txt); }
}

/* editing commands from the host (Ruffle TextControlCode) on the focused field */
static int a1_word_char(u32 c) { return c > 127 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static void a1_changed(VM *vm, MovieClip *c) {
    TextFieldState *t = tfs_of(c); Act a;
    tfs_invalidate(t);
    propagate_binding(vm, c);
    if (c->obj) {
        Value args[2];
        act_init_nothing(&a, vm, c, "[onChanged]");
        args[0] = v_str(vm->s_onChanged); args[1] = v_obj((AObj*)c->obj);
        o_call_method(&a, (AObj*)c->obj, vm->s_broadcastMessage, 2, args, ER_SPECIAL);
        if (vm->err) vm_report_error(&a);
    }
}
void a1_tf_control(VM *vm, MovieClip *c, const char *name) {
    static const struct { const char *n; int vk, shift, ctrl; } m[] = {
        { "MoveLeft", 37, 0, 0 }, { "MoveLeftWord", 37, 0, 1 }, { "MoveLeftLine", 36, 0, 0 }, { "MoveLeftDocument", 36, 0, 1 },
        { "MoveRight", 39, 0, 0 }, { "MoveRightWord", 39, 0, 1 }, { "MoveRightLine", 35, 0, 0 }, { "MoveRightDocument", 35, 0, 1 },
        { "SelectLeft", 37, 1, 0 }, { "SelectLeftWord", 37, 1, 1 }, { "SelectLeftLine", 36, 1, 0 }, { "SelectLeftDocument", 36, 1, 1 },
        { "SelectRight", 39, 1, 0 }, { "SelectRightWord", 39, 1, 1 }, { "SelectRightLine", 35, 1, 0 }, { "SelectRightDocument", 35, 1, 1 },
        { "Delete", 46, 0, 0 }, { "DeleteWord", 46, 0, 1 }, { NULL, 0, 0, 0 } };
    TextFieldState *t = c ? tfs_of(c) : NULL; int i, vk = 0, shift = 0, ctrl = 0, a, b, cur, len;
    if (!vm || !t) return;
    if (!strcmp(name, "Backspace")) { a1_tf_typed(vm, c, 8); return; }
    if (!strcmp(name, "Enter")) { a1_tf_typed(vm, c, 13); return; }
    clamp_sel(t);
    len = t->rt.len;
    a = t->sel_begin < t->sel_end ? t->sel_begin : t->sel_end; b = t->sel_begin < t->sel_end ? t->sel_end : t->sel_begin; cur = t->sel_end;
    if (!strcmp(name, "SelectAll")) { if (t->selectable) { t->sel_begin = 0; t->sel_end = len; t->has_sel = 1; } return; }
    if (!strcmp(name, "Copy") || !strcmp(name, "Cut")) {
        extern void (*a2_set_clipboard_hook)(const char *utf8);
        if (a != b && !t->password) {
            int bl; char *u8 = rt_utf8_encode(t->rt.text + a, b - a, &bl);
            if (u8) { int k; for (k = 0; k < bl; k++) if (u8[k] == '\r') u8[k] = '\n'; if (a2_set_clipboard_hook) a2_set_clipboard_hook(u8); free(u8); }
            if (name[1] == 'u' && t->input) { rt_replace(&t->rt, a, b, NULL, 0, &t->deffmt); t->sel_begin = t->sel_end = a; a1_changed(vm, c); }
        }
        return;
    }
    if (!strcmp(name, "Paste")) {
        extern char *(*a2_get_clipboard_hook)(void);
        char *u8;
        if (!t->input || !a2_get_clipboard_hook) return;
        u8 = a2_get_clipboard_hook();
        if (u8) {
            u32 *u; int n = rt_utf8_decode(u8, (int)strlen(u8), &u);
            free(u8);
            if (u && !n) { free(u); return; }   /* an empty clipboard pastes nothing */
            if (u) {
                int k2 = 0, q;
                for (q = 0; q < n; q++) { if (u[q] == '\r' && q + 1 < n && u[q + 1] == '\n') continue; u[k2++] = u[q]; }
                a1_insert(vm, c, u, k2);
                free(u);
            }
        }
        return;
    }
    for (i = 0; m[i].n; i++) if (!strcmp(name, m[i].n)) { vk = m[i].vk; shift = m[i].shift; ctrl = m[i].ctrl; break; }
    if (!vk) return;
    if (vk == 46) {
        if (!t->input) return;
        if (a != b) rt_replace(&t->rt, a, b, NULL, 0, &t->deffmt);
        else if (cur < len) { int e = cur + 1; if (ctrl) while (e < len && a1_word_char(t->rt.text[e])) e++; rt_replace(&t->rt, cur, e, NULL, 0, &t->deffmt); b = a = cur; }
        else return;
        t->sel_begin = t->sel_end = a; t->has_sel = 1;
        a1_changed(vm, c);
        return;
    }
    {
        int n = cur;
        if (vk == 37 || vk == 39) {
            if (!shift && a != b) n = vk == 37 ? a : b;
            else if (vk == 37 && n > 0) { n--; if (ctrl) { while (n > 0 && !a1_word_char(t->rt.text[n])) n--; while (n > 0 && a1_word_char(t->rt.text[n - 1])) n--; } }
            else if (vk == 39 && n < len) { n++; if (ctrl) { while (n < len && a1_word_char(t->rt.text[n])) n++; while (n < len && !a1_word_char(t->rt.text[n])) n++; } }
        } else {
            if (ctrl) n = vk == 36 ? 0 : len;
            else {
                tfs_layout(vm->player, t);
                if (t->lay.nlines) {
                    TFLine *ln = &t->lay.lines[t->lay.cline[cur < len ? cur : len]];
                    n = vk == 36 ? ln->start : ln->end;
                    if (vk == 35 && n > ln->start && n <= len && n > 0 && t->rt.text[n - 1] == '\r') n--;
                } else n = vk == 36 ? 0 : len;
            }
        }
        if (shift && t->selectable) t->sel_end = n; else t->sel_begin = t->sel_end = n;
        t->has_sel = 1;
        vm->player->caret_blink = 16;
    }
}

/* ---------------------------------------------------------------- */
/* TextField.StyleSheet (Ruffle avm1/globals/style_sheet.rs)        */

typedef struct { int n; char **sel; RTFmt *fmt; } SSData;

static void ss_free(AObj *o) { SSData *d = (SSData*)o->nat.p; int i; if (d) { for (i = 0; i < d->n; i++) free(d->sel[i]); free(d->sel); free(d->fmt); free(d); } o->nat.p = NULL; }
static SSData *ss_of(AObj *o) { o = o && o->nk == NK_SUPER ? o->nat.sup.self : o; return o && o->nk == NK_STYLESHEET ? (SSData*)o->nat.p : NULL; }

static void *ss_data_of(AObj *o) { return ss_of(o); }
static void ss_put(SSData *d, const char *sel, const RTFmt *f) {
    int i; char **ns; RTFmt *nf;
    for (i = 0; i < d->n; i++) if (!strcmp(d->sel[i], sel)) { d->fmt[i] = *f; return; }
    ns = (char**)realloc(d->sel, (size_t)(d->n + 1) * sizeof(char*)); if (!ns) return; d->sel = ns;
    nf = (RTFmt*)realloc(d->fmt, (size_t)(d->n + 1) * sizeof(RTFmt)); if (!nf) return; d->fmt = nf;
    d->sel[d->n] = (char*)malloc(strlen(sel) + 1); if (!d->sel[d->n]) return;
    strcpy(d->sel[d->n], sel); d->fmt[d->n] = *f; d->n++;
}

/* resolves "p" / ".cls" for the HTML parser */
static int ss_lookup(void *ctx, const char *sel, RTFmt *out) {
    SSData *d = (SSData*)ctx; int i; char low[300]; size_t k;
    for (k = 0; sel[k] && k + 1 < sizeof(low); k++) low[k] = (char)(sel[k] >= 'A' && sel[k] <= 'Z' ? sel[k] + 32 : sel[k]);
    low[k] = 0;
    for (i = 0; d && i < d->n; i++) if (!strcmp(d->sel[i], low)) { rt_fmt_apply(out, &d->fmt[i]); return 1; }
    return 0;
}

static Value ss_ctor(Act *a, AObj *self, int argc, Value *argv) {
    AObj *t = o_native_target(self); (void)a; (void)argc; (void)argv;
    if (t) { SSData *d = (SSData*)calloc(1, sizeof(SSData)); if (d) { t->nk = NK_STYLESHEET; t->nat.p = d; t->nfree = ss_free; } }
    return v_obj(self);
}

static Value shallow_copy(Act *a, Value v) {
    VM *vm = a->vm; AObj *o, *r; AStr **keys; int n = 0, i;
    if (v.t != V_OBJ) return v_null();
    o = v.u.o;
    r = obj_new(vm, PROTOS(a)->object);
    keys = o_keys(a, o, 0, &n); if (vm->err) { free(keys); return v_undef(); }
    for (i = 0; keys && i < n; i++) {
        Value x = o_get_stored(a, o, keys[i]); if (vm->err) break;
        o_set(a, r, keys[i], x); if (vm->err) break;
    }
    free(keys);
    return v_obj(r);
}

static Value ss_set_style(Act *a, AObj *self, int argc, Value *argv) {
    VM *vm = a->vm; AStr *k_styles = str_intern(vm, "_styles"), *k_css = str_intern(vm, "_css"), *name; AObj *css, *styles; Value v, tf, cp; SSData *d;
    if (!o_has_property(a, self, k_styles)) { o_set(a, self, k_styles, v_obj(obj_array(a, 0, NULL))); if (vm->err) return v_undef(); }
    if (!o_has_property(a, self, k_css)) { o_set(a, self, k_css, v_obj(obj_array(a, 0, NULL))); if (vm->err) return v_undef(); }
    v = o_get_stored(a, self, k_css); if (vm->err) return v_undef();
    css = v_to_obj_or_bare(a, v); if (vm->err) return v_undef();
    v = o_get_stored(a, self, k_styles); if (vm->err) return v_undef();
    styles = v_to_obj_or_bare(a, v); if (vm->err) return v_undef();
    name = v_str_of(a, arg(argc, argv, 0)); if (vm->err) return v_undef();
    cp = shallow_copy(a, arg(argc, argv, 1)); if (vm->err) return v_undef();
    o_set(a, css, name, cp); if (vm->err) return v_undef();
    cp = shallow_copy(a, arg(argc, argv, 1)); if (vm->err) return v_undef();
    tf = o_call_method(a, self, str_intern(vm, "transform"), 1, &cp, ER_SPECIAL); if (vm->err) return v_undef();
    o_set(a, styles, name, tf); if (vm->err) return v_undef();
    d = ss_of(self);
    if (d && tf.t == V_OBJ && fmt_of(tf.u.o)) {
        char low[512]; const char *u = str_c(vm, name); size_t k;
        for (k = 0; u[k] && k + 1 < sizeof(low); k++) low[k] = (char)(u[k] >= 'A' && u[k] <= 'Z' ? u[k] + 32 : u[k]);
        low[k] = 0;
        ss_put(d, low, fmt_of(tf.u.o));
    }
    return v_undef();
}

static Value ss_get_style(Act *a, AObj *self, int argc, Value *argv) {
    VM *vm = a->vm; Value v; AObj *css; AStr *name;
    v = o_get_stored(a, self, str_intern(vm, "_css")); if (vm->err) return v_undef();
    css = v_to_obj_or_bare(a, v); if (vm->err) return v_undef();
    name = v_str_of(a, arg(argc, argv, 0)); if (vm->err) return v_undef();
    v = o_get(a, css, name); if (vm->err) return v_undef();
    return shallow_copy(a, v);
}

static Value ss_get_style_names(Act *a, AObj *self, int argc, Value *argv) {
    VM *vm = a->vm; Value v, *items; AObj *css, *r; AStr **keys; int n = 0, i; (void)argc; (void)argv;
    v = o_get_stored(a, self, str_intern(vm, "_css")); if (vm->err) return v_undef();
    css = v_to_obj_or_bare(a, v); if (vm->err) return v_undef();
    keys = o_keys(a, css, 0, &n); if (vm->err) { free(keys); return v_undef(); }
    items = (Value*)calloc((size_t)n + 1, sizeof(Value));
    for (i = 0; items && i < n; i++) items[i] = v_str(keys[i]);
    r = obj_array(a, items ? n : 0, items);
    free(items); free(keys);
    return v_obj(r);
}

static Value ss_clear(Act *a, AObj *self, int argc, Value *argv) {
    VM *vm = a->vm; (void)argc; (void)argv;
    o_set(a, self, str_intern(vm, "_styles"), v_obj(obj_array(a, 0, NULL))); if (vm->err) return v_undef();
    o_set(a, self, str_intern(vm, "_css"), v_obj(obj_array(a, 0, NULL)));
    return v_undef();
}

static Value ss_load(Act *a, AObj *self, int argc, Value *argv) {
    extern void avm1_load_css(Act *a, AObj *target, AStr *url);
    AStr *url;
    if (argc < 1) return v_bool(0);
    url = v_str_of(a, argv[0]); if (a->vm->err) return v_undef();
    avm1_load_css(a, self, url);
    return v_bool(1);
}

static Value style_get(Act *a, AObj *o, const char *name) {
    Value v = o_get_stored(a, o, str_intern(a->vm, name));
    if (a->vm->err) { a->vm->err = 0; return v_undef(); }
    return v;
}

static int suffixed_i32(Act *a, Value v, s32 *out) {
    extern Value parse_int_value(Act *a, Value sv, Value *radix_v);
    Value r = parse_int_value(a, v, NULL); if (a->vm->err) return 0;
    *out = v_i32(a, r); return !a->vm->err;
}
static int suffixed_f64(Act *a, Value v, double *out) {
    extern Value parse_int_value(Act *a, Value sv, Value *radix_v);
    Value r = parse_int_value(a, v, NULL); if (a->vm->err) return 0;
    *out = v_num_of(a, r); return !a->vm->err;
}

static int str_is(Act *a, Value v, const char *s) { AStr *x = v_str_of(a, v); if (a->vm->err) return 0; return !strcmp(str_c(a->vm, x), s); }

static Value ss_transform(Act *a, AObj *self, int argc, Value *argv) {
    VM *vm = a->vm; Value tfv, cls; AObj *owner = NULL, *so = NULL; RTFmt f; RTFmt *dst; Value v;
    (void)self;
    cls = act_resolve(a, str_intern(vm, "TextFormat"), &owner); if (vm->err) return v_undef();
    { AObj *co = v_to_obj_or_bare(a, cls); if (vm->err) return v_undef(); tfv = co ? o_construct(a, co, 0, NULL) : v_undef(); if (vm->err) return v_undef(); }
    rt_fmt_clear(&f); f.kerning = 0; f.has |= RTF_KERNING; f.display = RT_DISPLAY_BLOCK; f.has |= RTF_DISPLAY;
    if (argc < 1) return v_null();
    if (argv[0].t == V_UNDEF || argv[0].t == V_NULL) return v_null();
    if (argv[0].t == V_OBJ) so = argv[0].u.o;
    if (so) {
        v = style_get(a, so, "color");
        if (v.t == V_STR) {
            const char *c = str_c(vm, v.u.s);
            if (c[0] == '#' && strlen(c) == 7) { char *e; unsigned long n = strtoul(c + 1, &e, 16); if (!*e) { f.color = (u32)n & 0xFFFFFF; f.has |= RTF_COLOR; } }
        }
        v = style_get(a, so, "display");
        if (v.t != V_UNDEF) { AStr *d = v_str_of(a, v); const char *ds; if (vm->err) return v_undef(); ds = str_c(vm, d); f.display = !strcmp(ds, "none") ? RT_DISPLAY_NONE : !strcmp(ds, "inline") ? RT_DISPLAY_INLINE : RT_DISPLAY_BLOCK; f.has |= RTF_DISPLAY; }
        v = style_get(a, so, "fontFamily");
        if (v.t != V_UNDEF && v_as_bool(v, a->swf_version)) {
            AStr *fs = v_str_of(a, v); const char *in; char out[256]; size_t o = 0, p = 0, L; if (vm->err) return v_undef();
            in = str_c(vm, fs); L = strlen(in);
            while (p < L) {
                size_t st, l; const char *val;
                while (in[p] == ' ') p++;
                st = p; while (p < L && in[p] != ',') p++;
                l = p - st; val = in + st;
                if (p < L) p++;
                if (l == 4 && !strncmp(val, "mono", 4)) { val = "_typewriter"; l = 11; }
                else if (l == 10 && !strncmp(val, "sans-serif", 10)) { val = "_sans"; l = 5; }
                else if (l == 5 && !strncmp(val, "serif", 5)) { val = "_serif"; l = 6; }
                if (l) { if (o && o + 1 < sizeof(out)) out[o++] = ','; if (o + l < sizeof(out)) { memcpy(out + o, val, l); o += l; } }
            }
            out[o] = 0;
            strncpy(f.font, out, sizeof(f.font) - 1); f.font[sizeof(f.font) - 1] = 0; f.has |= RTF_FONT;
        }
        v = style_get(a, so, "fontSize");
        if (v.t != V_UNDEF) { s32 n; if (!suffixed_i32(a, v, &n)) return v_undef(); if (n > 0) { f.size = n; f.has |= RTF_SIZE; } }
        v = o_get_stored(a, so, str_intern(vm, "fontStyle"));
        if (!vm->err) { if (str_is(a, v, "normal")) { f.italic = 0; f.has |= RTF_ITALIC; } else if (str_is(a, v, "italic")) { f.italic = 1; f.has |= RTF_ITALIC; } if (vm->err) return v_undef(); } else vm->err = 0;
        v = o_get_stored(a, so, str_intern(vm, "fontWeight"));
        if (!vm->err) { if (str_is(a, v, "normal")) { f.bold = 0; f.has |= RTF_BOLD; } else if (str_is(a, v, "bold")) { f.bold = 1; f.has |= RTF_BOLD; } if (vm->err) return v_undef(); } else vm->err = 0;
        v = style_get(a, so, "kerning");
        if (v.t != V_UNDEF) {
            int k; s32 n;
            if (v.t == V_STR && !strcmp(str_c(vm, v.u.s), "true")) k = 1;
            else { if (!suffixed_i32(a, v, &n)) return v_undef(); k = n != 0; }
            f.kerning = (u8)k; f.has |= RTF_KERNING;
        }
        v = style_get(a, so, "leading");
        if (v.t != V_UNDEF && v_as_bool(v, a->swf_version)) { s32 n; if (!suffixed_i32(a, v, &n)) return v_undef(); f.leading = n; f.has |= RTF_LEADING; }
        v = style_get(a, so, "letterSpacing");
        if (v.t != V_UNDEF && v_as_bool(v, a->swf_version)) { double n; if (!suffixed_f64(a, v, &n)) return v_undef(); f.letter_spacing = n; f.has |= RTF_LETTERSPACING; }
        v = style_get(a, so, "marginLeft");
        if (v.t != V_UNDEF && v_as_bool(v, a->swf_version)) { s32 n; if (!suffixed_i32(a, v, &n)) return v_undef(); f.left_margin = n > 0 ? n : 0; f.has |= RTF_LEFTMARGIN; }
        v = style_get(a, so, "marginRight");
        if (v.t != V_UNDEF && v_as_bool(v, a->swf_version)) { s32 n; if (!suffixed_i32(a, v, &n)) return v_undef(); f.right_margin = n > 0 ? n : 0; f.has |= RTF_RIGHTMARGIN; }
        v = style_get(a, so, "textAlign");
        if (v.t != V_UNDEF) {
            AStr *al = v_str_of(a, v); char low[32]; const char *s; size_t k; if (vm->err) return v_undef();
            s = str_c(vm, al);
            for (k = 0; s[k] && k + 1 < sizeof(low); k++) low[k] = (char)(s[k] >= 'A' && s[k] <= 'Z' ? s[k] + 32 : s[k]);
            low[k] = 0;
            if (!strcmp(low, "left")) { f.align = RT_ALIGN_LEFT; f.has |= RTF_ALIGN; }
            else if (!strcmp(low, "center")) { f.align = RT_ALIGN_CENTER; f.has |= RTF_ALIGN; }
            else if (!strcmp(low, "right")) { f.align = RT_ALIGN_RIGHT; f.has |= RTF_ALIGN; }
            else if (!strcmp(low, "justify")) { f.align = RT_ALIGN_JUSTIFY; f.has |= RTF_ALIGN; }
        }
        v = style_get(a, so, "textDecoration");
        if (v.t != V_UNDEF) { if (str_is(a, v, "none")) { f.underline = 0; f.has |= RTF_UNDERLINE; } else if (str_is(a, v, "underline")) { f.underline = 1; f.has |= RTF_UNDERLINE; } if (vm->err) return v_undef(); }
        v = style_get(a, so, "textIndent");
        if (v.t != V_UNDEF && v_as_bool(v, a->swf_version)) { s32 n; if (!suffixed_i32(a, v, &n)) return v_undef(); f.indent = n; f.has |= RTF_INDENT; }
    }
    if (tfv.t == V_OBJ && (dst = fmt_of(tfv.u.o)) != NULL) *dst = f;
    return tfv;
}

/* ---- CSS (html/style_sheet.rs CssStream) ---- */

typedef struct { const AStr *s; u32 pos; } Css;
typedef struct { u32 a, b; } Span;
typedef struct { Span name; Span val; } CssProp;
typedef struct { Span sel; CssProp *p; int np; } CssRule;

static int cpeek(Css *c) { return c->pos < c->s->len ? (int)STR_AT(c->s, c->pos) : -1; }
static int cpeek_at(Css *c, u32 i) { return i < c->s->len ? (int)STR_AT(c->s, i) : -1; }
static int is_ws(int ch) { return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t'; }
static int skip_comment(Css *c) {
    if (cpeek(c) == '/' && cpeek_at(c, c->pos + 1) == '*') {
        c->pos += 2;
        while (!(cpeek(c) == '*' && cpeek_at(c, c->pos + 1) == '/')) { c->pos++; if (cpeek(c) < 0) return 0; }
        c->pos += 2; return 1;
    }
    return 0;
}
static int skip_ws(Css *c) { int f = 0; while (skip_comment(c) || (is_ws(cpeek(c)) && (c->pos++, 1))) f = 1; return f; }
static Span until_any(Css *c, const char *set) { Span s; s.a = c->pos; while (cpeek(c) >= 0 && !strchr(set, cpeek(c))) c->pos++; s.b = c->pos; return s; }

/* 1 ok, 0 error; rules appended (properties: later duplicates replace earlier ones) */
static int css_parse(Css *c, CssRule **rules, int *nr) {
    for (;;) {
        Span sels[64]; int ns = 0, i; CssProp *props = NULL; int np = 0;
        skip_ws(c);
        if (cpeek(c) < 0) return 1;
        /* selectors */
        for (;;) {
            Span nm; int ch;
            skip_ws(c);
            nm = until_any(c, "{, \n\r\t");
            if (nm.b > nm.a && ns < 64) sels[ns++] = nm;
            skip_ws(c);
            ch = cpeek(c);
            if (ch == '{') { c->pos++; if (!ns) { sels[0].a = sels[0].b = 0; ns = 1; } break; }
            if (ch == ',') { c->pos++; continue; }
            return 0;
        }
        /* properties */
        for (;;) {
            Span name, value; u32 name_start, value_start; int ch, done = 0;
            skip_ws(c);
            ch = cpeek(c);
            if (ch == '}') { c->pos++; break; }
            if (ch < 0) break;
            name_start = c->pos;
            name = until_any(c, ": \n\r\t");
            if (skip_ws(c)) {
                ch = cpeek(c);
                if (ch == ':') { name.a = name_start; name.b = c->pos; }
                else if (ch >= 0) { free(props); return 0; }
            }
            if (cpeek(c) < 0) { free(props); return 0; }
            c->pos++;   /* ':' */
            skip_ws(c);
            value_start = c->pos;
            value = until_any(c, ";:}");
            for (;;) {
                ch = cpeek(c);
                if (ch == ':') {
                    Span pv; c->pos = value_start; pv = until_any(c, "\n\r");
                    if (cpeek(c) == '\n' || cpeek(c) == '\r') { c->pos++; value = pv; break; }
                    c->pos = value_start; value = until_any(c, ";}");
                    continue;
                }
                if (ch == ';') { c->pos++; break; }
                if (ch == '}') {
                    u32 k; c->pos++;
                    for (k = value.a; k < value.b; k++) { int x = (int)STR_AT(c->s, k); if (x == '\n' || x == '\r') { value.b = k; break; } }
                    done = 1; break;
                }
                free(props); return 0;
            }
            {
                int j, found = -1;
                for (j = 0; j < np; j++) {
                    u32 l = props[j].name.b - props[j].name.a, k;
                    if (l != name.b - name.a) continue;
                    for (k = 0; k < l && STR_AT(c->s, props[j].name.a + k) == STR_AT(c->s, name.a + k); k++) ;
                    if (k == l) { found = j; break; }
                }
                if (found >= 0) props[found].val = value;
                else { CssProp *np2 = (CssProp*)realloc(props, (size_t)(np + 1) * sizeof(CssProp)); if (!np2) { free(props); return 0; } props = np2; props[np].name = name; props[np].val = value; np++; }
            }
            if (done) break;
        }
        for (i = 0; i < ns; i++) {
            int j, found = -1; CssRule *nr2;
            for (j = 0; j < *nr; j++) {
                u32 l = (*rules)[j].sel.b - (*rules)[j].sel.a, k;
                if (l != sels[i].b - sels[i].a) continue;
                for (k = 0; k < l && STR_AT(c->s, (*rules)[j].sel.a + k) == STR_AT(c->s, sels[i].a + k); k++) ;
                if (k == l) { found = j; break; }
            }
            if (found >= 0) { free((*rules)[found].p); (*rules)[found].p = NULL; (*rules)[found].np = 0; }
            else {
                nr2 = (CssRule*)realloc(*rules, (size_t)(*nr + 1) * sizeof(CssRule)); if (!nr2) { free(props); return 0; }
                *rules = nr2; found = (*nr)++; (*rules)[found].sel = sels[i];
            }
            (*rules)[found].p = (CssProp*)malloc((size_t)(np ? np : 1) * sizeof(CssProp));
            if ((*rules)[found].p) memcpy((*rules)[found].p, props, (size_t)np * sizeof(CssProp));
            (*rules)[found].np = np;
        }
        free(props);
    }
}

static AStr *span_str(VM *vm, const AStr *s, Span sp) { return str_slice(vm, (AStr*)s, sp.a, sp.b); }

/* transform_dashes_to_camel_case */
static AStr *camel(VM *vm, AStr *s) {
    SBuf b; u32 i; int up = 0;
    sb_init(&b);
    for (i = 0; i < s->len; i++) {
        u16 ch = STR_AT(s, i);
        if (ch == '-' && !up) { up = 1; continue; }
        if (up) { up = 0; if (ch >= 'a' && ch <= 'z') ch = (u16)(ch - 32); }
        sb_unit(&b, ch);
    }
    return sb_done(vm, &b);
}

static Value ss_parse_css(Act *a, AObj *self, int argc, Value *argv) {
    VM *vm = a->vm; AStr *src; Css c; CssRule *rules = NULL; int nr = 0, ok, i, j;
    src = v_str_of(a, arg(argc, argv, 0)); if (vm->err) return v_undef();
    c.s = src; c.pos = 0;
    ok = css_parse(&c, &rules, &nr);
    if (ok) {
        for (i = 0; i < nr && !vm->err; i++) {
            AObj *o; Value args[2];
            if (rules[i].sel.b == rules[i].sel.a) continue;
            o = obj_new(vm, PROTOS(a)->object);
            for (j = 0; j < rules[i].np; j++) {
                o_set(a, o, camel(vm, span_str(vm, src, rules[i].p[j].name)), v_str(span_str(vm, src, rules[i].p[j].val)));
                if (vm->err) break;
            }
            if (vm->err) break;
            args[0] = v_str(span_str(vm, src, rules[i].sel)); args[1] = v_obj(o);
            ss_set_style(a, self, 2, args);
        }
    }
    for (i = 0; i < nr; i++) free(rules[i].p);
    free(rules);
    if (vm->err) return v_undef();
    return v_bool(ok);
}

AObj *lib_stylesheet(VM *vm, Env *env, AObj *object_proto) {
    AObj *proto, *c = lib_class(vm, env, ss_ctor, NULL, object_proto, 1, &proto); u16 at = PA_DONT_ENUM | PA_DONT_DELETE | PA_READ_ONLY | PA_VERSION_7;
    env->p.stylesheet = proto;
    defm(vm, env, proto, "setStyle", ss_set_style, at);
    defm(vm, env, proto, "clear", ss_clear, at);
    defm(vm, env, proto, "getStyleNames", ss_get_style_names, at);
    defm(vm, env, proto, "load", ss_load, at);
    defm(vm, env, proto, "getStyle", ss_get_style, at);
    defm(vm, env, proto, "transform", ss_transform, at);
    defm(vm, env, proto, "parseCSS", ss_parse_css, at);
    defm(vm, env, proto, "parse", ss_parse_css, at);
    return c;
}
