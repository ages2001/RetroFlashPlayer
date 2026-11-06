/* avm2_text.c - flash.text: TextField, TextFormat, StaticText, Font, consts.
 * TextField content, layout and drawing live in richtext.c / textfield.c;
 * this file is the ActionScript 3 face of it. */
#include "avm2_flash_int.h"
#include "reader.h"
#include "textfield.h"
#include "font.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

int a2_str_ulen_pub(A2Str *s);
static A2Obj *tself(V2 self) { return (self.t == T_OBJ && self.u.o->kind == OK_DISPLAY) ? self.u.o : NULL; }
void edittext_init_state(Player *p, SWFChar *ch, TextFieldState *t);
void a2_report_uncaught(VM2 *vm);

/* the field state, created on first use */
static TextFieldState *tfs_of(VM2 *vm, A2Obj *o) {
    A2Disp *st = a2_disp(o);
    if (!st->tfs) {
        DisplayObject *d = a2_dobj_of(o);
        st->tfs = (TextFieldState*)malloc(sizeof(TextFieldState));
        tfs_init(st->tfs, vm->swf_version ? vm->swf_version : 10);
        st->tfs->player = vm->player;
        st->tfs->html = 1;
        if (d && d->ch && d->ch->type == CHAR_EDITTEXT) {
            edittext_init_state(vm->player, d->ch, st->tfs);
            tfs_csm(st->tfs, d->ch->csm);
            if (d->depth < 0) rt_set_plain(&st->tfs->rt, NULL, 0, &st->tfs->deffmt);   /* `new LinkedText()`: the symbol's settings, but no text */
            st->tfs->sel_begin = st->tfs->sel_end = st->tfs->rt.len;   /* AS3 fields start with the caret after their text */
        }
    }
    st->is_text = 1;
    {   /* device text follows the field's scale */
        DisplayObject *d = a2_dobj_of(o);
        if (d) { double sx = sqrt(d->matrix.sx * d->matrix.sx + d->matrix.r0 * d->matrix.r0), sy = sqrt(d->matrix.r1 * d->matrix.r1 + d->matrix.sy * d->matrix.sy);
                 if (sx > 0 && sy > 0 && (sx != (st->tfs->dev_sx > 0 ? st->tfs->dev_sx : 1) || sy != (st->tfs->dev_sy > 0 ? st->tfs->dev_sy : 1)) && !st->tfs->embed) { st->tfs->dev_sx = sx; st->tfs->dev_sy = sy; tfs_invalidate(st->tfs); }
                 st->tfs->dev_tx = d->matrix.tx; }
    }
    return st->tfs;
}
void a2_text_sync_from_char(VM2 *vm, A2Obj *o) { tfs_of(vm, o); }
TextFieldState *a2_textfield_state(DisplayObject *d) {
    A2Obj *o = (A2Obj*)(d->obj2 ? d->obj2 : (d->clip ? d->clip->obj2 : NULL)); A2Disp *st;
    if (!o || !o->payload) return NULL;
    st = (A2Disp*)o->payload;
    return st->is_text ? st->tfs : NULL;
}
void a2_textfield_free(A2Disp *st) { if (st->tfs) { tfs_free(st->tfs); free(st->tfs); st->tfs = NULL; } }

static void changed(VM2 *vm, TextFieldState *t) { (void)vm; tfs_invalidate(t); }
static void clamp_sel(TextFieldState *t) { if (t->sel_begin > t->rt.len) t->sel_begin = t->rt.len; if (t->sel_end > t->rt.len) t->sel_end = t->rt.len; if (t->sel_begin < 0) t->sel_begin = 0; if (t->sel_end < 0) t->sel_end = 0; }
static void clamp_scroll(VM2 *vm, TextFieldState *t) { int m = tfs_maxscroll_v(vm->player, t); if (t->scroll_v > m) t->scroll_v = m; if (t->scroll_v < 1) t->scroll_v = 1; }

/* ---- string conversions ---- */
static u32 *to_cps(A2Str *s, int *n) { u32 *u; *n = rt_utf8_decode(s->s, (int)s->len, &u); return u; }
/* text assigned by script: every "\n" becomes "\r" ("\r\n" is two line breaks) */
static u32 *to_field_text(A2Str *s, int *n) {
    u32 *u = to_cps(s, n); int i, k = 0;
    for (i = 0; i < *n; i++) u[k++] = u[i] == '\n' ? '\r' : u[i];
    *n = k; return u;
}
static V2 cps_str(VM2 *vm, const u32 *u, int n) { int bl; char *b = rt_utf8_encode(u, n, &bl); V2 r = v2_str(a2_intern(vm, b, (size_t)bl)); free(b); return r; }

/* ---- TextFormat <-> RTFmt ---- */
static V2 tfmt_get(VM2 *vm, V2 f, const char *k) { return a2_get_s(vm, f, k); }
static int tfmt_set_field(VM2 *vm, V2 v) { (void)vm; return v.t != T_NULL && v.t != T_UNDEF; }
void a2_textformat_to_rt(VM2 *vm, V2 f, RTFmt *o) {
    V2 v;
    rt_fmt_clear(o);
    v = tfmt_get(vm, f, "font"); if (tfmt_set_field(vm, v)) { A2Str *s = v2_tostr(vm, v); strncpy(o->font, s->s, sizeof(o->font) - 1); o->font[sizeof(o->font) - 1] = 0; o->has |= RTF_FONT; }
    v = tfmt_get(vm, f, "size"); if (tfmt_set_field(vm, v)) { o->size = v2_tonum(vm, v); o->has |= RTF_SIZE; }
    v = tfmt_get(vm, f, "color"); if (tfmt_set_field(vm, v)) { o->color = v2_touint(vm, v) & 0xFFFFFF; o->has |= RTF_COLOR; }
    v = tfmt_get(vm, f, "bold"); if (tfmt_set_field(vm, v)) { o->bold = (u8)v2_tobool(vm, v); o->has |= RTF_BOLD; }
    v = tfmt_get(vm, f, "italic"); if (tfmt_set_field(vm, v)) { o->italic = (u8)v2_tobool(vm, v); o->has |= RTF_ITALIC; }
    v = tfmt_get(vm, f, "underline"); if (tfmt_set_field(vm, v)) { o->underline = (u8)v2_tobool(vm, v); o->has |= RTF_UNDERLINE; }
    v = tfmt_get(vm, f, "url"); if (tfmt_set_field(vm, v)) { A2Str *s = v2_tostr(vm, v); strncpy(o->url, s->s, sizeof(o->url) - 1); o->url[sizeof(o->url) - 1] = 0; o->has |= RTF_URL; }
    v = tfmt_get(vm, f, "target"); if (tfmt_set_field(vm, v)) { A2Str *s = v2_tostr(vm, v); strncpy(o->target, s->s, sizeof(o->target) - 1); o->target[sizeof(o->target) - 1] = 0; o->has |= RTF_TARGET; }
    v = tfmt_get(vm, f, "align"); if (tfmt_set_field(vm, v)) { const char *a = v2_tostr(vm, v)->s; o->align = !strcmp(a, "right") ? RT_ALIGN_RIGHT : !strcmp(a, "center") ? RT_ALIGN_CENTER : !strcmp(a, "justify") ? RT_ALIGN_JUSTIFY : !strcmp(a, "start") ? RT_ALIGN_START : !strcmp(a, "end") ? RT_ALIGN_END : RT_ALIGN_LEFT; if (o->align == RT_ALIGN_START) o->align = RT_ALIGN_LEFT; if (o->align == RT_ALIGN_END) o->align = RT_ALIGN_RIGHT; o->has |= RTF_ALIGN; }
    v = tfmt_get(vm, f, "leftMargin"); if (tfmt_set_field(vm, v)) { o->left_margin = v2_tonum(vm, v); o->has |= RTF_LEFTMARGIN; }
    v = tfmt_get(vm, f, "rightMargin"); if (tfmt_set_field(vm, v)) { o->right_margin = v2_tonum(vm, v); o->has |= RTF_RIGHTMARGIN; }
    v = tfmt_get(vm, f, "indent"); if (tfmt_set_field(vm, v)) { o->indent = v2_tonum(vm, v); o->has |= RTF_INDENT; }
    v = tfmt_get(vm, f, "blockIndent"); if (tfmt_set_field(vm, v)) { o->block_indent = v2_tonum(vm, v); o->has |= RTF_BLOCKINDENT; }
    v = tfmt_get(vm, f, "leading"); if (tfmt_set_field(vm, v)) { o->leading = v2_tonum(vm, v); o->has |= RTF_LEADING; }
    v = tfmt_get(vm, f, "letterSpacing"); if (tfmt_set_field(vm, v)) { o->letter_spacing = v2_tonum(vm, v); o->has |= RTF_LETTERSPACING; }
    v = tfmt_get(vm, f, "kerning"); if (tfmt_set_field(vm, v)) { o->kerning = (u8)v2_tobool(vm, v); o->has |= RTF_KERNING; }
    v = tfmt_get(vm, f, "bullet"); if (tfmt_set_field(vm, v)) { o->bullet = (u8)v2_tobool(vm, v); o->has |= RTF_BULLET; }
    v = tfmt_get(vm, f, "display"); if (tfmt_set_field(vm, v)) { const char *a = v2_tostr(vm, v)->s; o->display = !strcmp(a, "inline") ? RT_DISPLAY_INLINE : !strcmp(a, "none") ? RT_DISPLAY_NONE : RT_DISPLAY_BLOCK; o->has |= RTF_DISPLAY; }
    v = tfmt_get(vm, f, "tabStops"); if (tfmt_set_field(vm, v) && v.t == T_OBJ) { int i; o->ntabs = 0; for (i = 0; i < v.u.o->alen && i < RT_MAX_TABS; i++) o->tabs[o->ntabs++] = v2_tonum(vm, v.u.o->arr[i]); o->has |= RTF_TABSTOPS; }
}
static V2 numv(double d) { if (d == floor(d) && fabs(d) < 2147483647.0) return v2_int((s32)d); return v2_num(d); }
static void tfm_raw(VM2 *vm, V2 o, const char *name, V2 v) { char k[40]; sprintf(k, "__tf_%.120s", name); a2_dyn_set_hidden(vm, o.u.o, a2_str(vm, k), v); }
V2 a2_textformat_from_rt(VM2 *vm, const RTFmt *f) {
    V2 o = a2_construct_class(vm, vm->c_textformat, 0, NULL); u32 h = f->has;
    if (o.t != T_OBJ) return o;
    if (h & RTF_FONT) tfm_raw(vm, o, "font", v2_cstr(vm, f->font));
    if (h & RTF_SIZE) tfm_raw(vm, o, "size", numv(f->size));
    if (h & RTF_COLOR) tfm_raw(vm, o, "color", v2_uint(f->color));
    if (h & RTF_BOLD) tfm_raw(vm, o, "bold", v2_bool(f->bold));
    if (h & RTF_ITALIC) tfm_raw(vm, o, "italic", v2_bool(f->italic));
    if (h & RTF_UNDERLINE) tfm_raw(vm, o, "underline", v2_bool(f->underline));
    if (h & RTF_URL) tfm_raw(vm, o, "url", v2_cstr(vm, f->url));
    if (h & RTF_TARGET) tfm_raw(vm, o, "target", v2_cstr(vm, f->target));
    if (h & RTF_ALIGN) tfm_raw(vm, o, "align", v2_cstr(vm, f->align == RT_ALIGN_RIGHT ? "right" : f->align == RT_ALIGN_CENTER ? "center" : f->align == RT_ALIGN_JUSTIFY ? "justify" : "left"));
    if (h & RTF_LEFTMARGIN) tfm_raw(vm, o, "leftMargin", numv(f->left_margin));
    if (h & RTF_RIGHTMARGIN) tfm_raw(vm, o, "rightMargin", numv(f->right_margin));
    if (h & RTF_INDENT) tfm_raw(vm, o, "indent", numv(f->indent));
    if (h & RTF_BLOCKINDENT) tfm_raw(vm, o, "blockIndent", numv(f->block_indent));
    if (h & RTF_LEADING) tfm_raw(vm, o, "leading", numv(f->leading));
    if (h & RTF_LETTERSPACING) tfm_raw(vm, o, "letterSpacing", numv(f->letter_spacing));
    if (h & RTF_KERNING) tfm_raw(vm, o, "kerning", v2_bool(f->kerning));
    if (h & RTF_BULLET) tfm_raw(vm, o, "bullet", v2_bool(f->bullet));
    if (h & RTF_DISPLAY) tfm_raw(vm, o, "display", v2_cstr(vm, f->display == RT_DISPLAY_INLINE ? "inline" : f->display == RT_DISPLAY_NONE ? "none" : "block"));
    if (h & RTF_TABSTOPS) { A2Obj *a = a2_new_array(vm); int i; for (i = 0; i < f->ntabs; i++) a2_arr_push(vm, a, numv(f->tabs[i])); tfm_raw(vm, o, "tabStops", v2_obj(a)); }
    return o;
}

/* ---- style sheets ---- */
static A2Obj *ss_table(VM2 *vm, A2Obj *ss); static A2Str *lower_str(VM2 *vm, A2Str *s);
static A2Obj *ss_fmts(VM2 *vm, A2Obj *ss);
static int css_lookup(void *ctx, const char *sel, RTFmt *out) {
    VM2 *vm = (VM2*)((void**)ctx)[0]; A2Obj *ss = (A2Obj*)((void**)ctx)[1]; A2Prop *p;
    p = a2_dyn_find(ss_fmts(vm, ss), lower_str(vm, a2_str(vm, sel)));
    if (!p || p->v.t != T_OBJ) return 0;
    a2_textformat_to_rt(vm, p->v, out);   /* the TextFormat transform() made for the style */
    return 1;   /* the style exists, even with nothing usable in it */
}
static A2Obj *stylesheet_of(VM2 *vm, A2Obj *o) { A2Prop *p = a2_dyn_find(o, a2_str(vm, "__styleSheet")); return p && p->v.t == T_OBJ ? p->v.u.o : NULL; }

static void set_html(VM2 *vm, A2Obj *o, TextFieldState *t, A2Str *html) {
    RTHtmlOpts op; void *ctx[2]; A2Obj *ss = stylesheet_of(vm, o);
    op.multiline = t->multiline; op.condense_white = t->condense_white; op.swf_version = vm->swf_version ? vm->swf_version : 10; op.css = NULL; op.css_ctx = NULL;
    if (ss && t->html_source && !strcmp(t->html_source, html->s) && !t->restyle) return;   /* the same HTML again: nothing to do */
    t->restyle = 0;
    if (ss) { ctx[0] = vm; ctx[1] = ss; op.css = css_lookup; op.css_ctx = ctx; free(t->html_source); t->html_source = (char*)malloc(html->len + 1); if (t->html_source) memcpy(t->html_source, html->s, html->len + 1); }
    rt_from_html(&t->rt, html->s, &t->deffmt, &op);
    clamp_sel(t);
    changed(vm, t);
}

/* ---- properties ---- */
#define TFSELF(fail) A2Obj *o = tself(self); TextFieldState *t; if (!o) return fail; t = tfs_of(vm, o); (void)t
#define TBOOL(get, set, field, extra) \
    static V2 get(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_undef()); (void)argc; (void)argv; return v2_bool(t->field); } \
    static V2 set(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_undef()); t->field = BOOL(0) ? 1 : 0; extra; changed(vm, t); return v2_undef(); }
TBOOL(tf_wordWrap_get, tf_wordWrap_set, wordwrap, (void)0)
TBOOL(tf_multiline_get, tf_multiline_set, multiline, (void)0)
TBOOL(tf_border_get, tf_border_set, border, (void)0)
TBOOL(tf_background_get, tf_background_set, background, (void)0)
TBOOL(tf_selectable_get, tf_selectable_set, selectable, (void)0)
TBOOL(tf_displayAsPassword_get, tf_displayAsPassword_set, password, (void)0)
TBOOL(tf_embedFonts_get, tf_embedFonts_set, embed, (void)0)
TBOOL(tf_condenseWhite_get, tf_condenseWhite_set, condense_white, (void)0)
TBOOL(tf_alwaysShowSelection_get, tf_alwaysShowSelection_set, always_show_selection, (void)0)
TBOOL(tf_mouseWheelEnabled_get, tf_mouseWheelEnabled_set, mouse_wheel, (void)0)
TBOOL(tf_useRichTextClipboard_get, tf_useRichTextClipboard_set, use_rich_clipboard, (void)0)

static V2 tf_text_get(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_undef()); UNUSED; return cps_str(vm, t->rt.text, t->rt.len); }
static void set_text(VM2 *vm, TextFieldState *t, A2Str *s) {
    int n; u32 *u = to_field_text(s, &n);
    if (n == t->rt.len && (n == 0 || !memcmp(u, t->rt.text, (size_t)n * sizeof(u32)))) { free(u); return; }   /* same text: formatting stays */
    rt_set_plain(&t->rt, u, n, &t->deffmt); free(u);
    free(t->html_source); t->html_source = NULL;
    clamp_sel(t);
    changed(vm, t);
}
static V2 tf_text_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    TFSELF(v2_undef());
    if (argc < 1 || argv[0].t == T_NULL || argv[0].t == T_UNDEF) { a2_throw_error(vm, "TypeError", "Parameter text must be non-null.", 2007); return v2_undef(); }
    { A2Obj *ss = stylesheet_of(vm, o); if (ss) { set_html(vm, o, t, STR(0)); return v2_undef(); } }
    set_text(vm, t, STR(0));
    return v2_undef();
}
static V2 tf_htmlText_get(VM2 *vm, V2 self, int argc, V2 *argv) {
    char *h; V2 r; TFSELF(v2_undef()); UNUSED;
    if (!t->html) return cps_str(vm, t->rt.text, t->rt.len);
    if (t->html_source) return v2_cstr(vm, t->html_source);
    h = rt_to_html(&t->rt); r = v2_cstr(vm, h ? h : ""); free(h); return r;
}
static V2 tf_htmlText_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    TFSELF(v2_undef());
    if (argc < 1 || argv[0].t == T_NULL || argv[0].t == T_UNDEF) { a2_throw_error(vm, "TypeError", "Parameter htmlText must be non-null.", 2007); return v2_undef(); }
    if (t->html && !t->html_source) { char *cur = rt_to_html(&t->rt); int same = cur && !strcmp(cur, STR(0)->s); free(cur); if (same) return v2_undef(); }   /* the same HTML again changes nothing */
    t->html = 1;
    set_html(vm, o, t, STR(0));
    return v2_undef();
}
/* width/height are the transformed bounds' extents (corners rounded to twips) */
static double tf_extent(VM2 *vm, A2Obj *o, TextFieldState *t, int vert) {
    DisplayObject *d = a2_dobj_of(o); double xs[4], ys[4], mn, mx; int i;
    tfs_apply_autosize(vm->player, t);
    if (!d) return (vert ? t->h : t->w) / 20.0;
    xs[0] = t->xmin; ys[0] = t->ymin; xs[1] = t->xmin + t->w; ys[1] = t->ymin; xs[2] = t->xmin; ys[2] = t->ymin + t->h; xs[3] = t->xmin + t->w; ys[3] = t->ymin + t->h;
    for (i = 0; i < 4; i++) { double x = xs[i], y = ys[i]; xs[i] = floor(d->matrix.sx * x + d->matrix.r1 * y + 0.5); ys[i] = floor(d->matrix.r0 * x + d->matrix.sy * y + 0.5); }
    mn = mx = vert ? ys[0] : xs[0];
    for (i = 1; i < 4; i++) { double v = vert ? ys[i] : xs[i]; if (v < mn) mn = v; if (v > mx) mx = v; }
    return (mx - mn) / 20.0;
}
static V2 tf_width_get(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_undef()); UNUSED; return v2_num(tf_extent(vm, o, t, 0)); }
static V2 tf_height_get(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_undef()); UNUSED; return v2_num(tf_extent(vm, o, t, 1)); }
static V2 tf_width_set(VM2 *vm, V2 self, int argc, V2 *argv) { double v = NUM(0); TFSELF(v2_undef()); if (v != v) return v2_undef(); if (v < 0) v = 0; tfs_apply_autosize(vm->player, t); t->w = (int)floor(v * 20 + 0.5); changed(vm, t); return v2_undef(); }
static V2 tf_height_set(VM2 *vm, V2 self, int argc, V2 *argv) { double v = NUM(0); TFSELF(v2_undef()); if (v != v) return v2_undef(); if (v < 0) v = 0; tfs_apply_autosize(vm->player, t); t->h = (int)floor(v * 20 + 0.5); changed(vm, t); return v2_undef(); }
static V2 tf_textColor_get(VM2 *vm, V2 self, int argc, V2 *argv) { const RTFmt *f; TFSELF(v2_undef()); UNUSED; f = t->rt.len ? rt_format_at(&t->rt, 0) : &t->deffmt; return v2_uint(f ? f->color : 0); }
static V2 tf_textColor_set(VM2 *vm, V2 self, int argc, V2 *argv) { RTFmt f; TFSELF(v2_undef()); rt_fmt_clear(&f); f.color = v2_touint(vm, ARG(0)) & 0xFFFFFF; f.has = RTF_COLOR; rt_set_format(&t->rt, 0, t->rt.len, &f); t->deffmt.color = f.color; changed(vm, t); return v2_undef(); }
static V2 tf_backgroundColor_get(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_undef()); UNUSED; return v2_uint(t->bgcolor); }
static V2 tf_backgroundColor_set(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_undef()); t->bgcolor = v2_touint(vm, ARG(0)) & 0xFFFFFF; return v2_undef(); }
static V2 tf_borderColor_get(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_undef()); UNUSED; return v2_uint(t->bordercolor); }
static V2 tf_borderColor_set(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_undef()); t->bordercolor = v2_touint(vm, ARG(0)) & 0xFFFFFF; return v2_undef(); }
static V2 tf_maxChars_get(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_undef()); UNUSED; return v2_int(t->max_chars); }
static V2 tf_maxChars_set(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_undef()); t->max_chars = INT(0); if (t->max_chars < 0) t->max_chars = 0; return v2_undef(); }
static V2 tf_type_get(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_undef()); UNUSED; return v2_cstr(vm, t->input ? "input" : "dynamic"); }
static V2 tf_type_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    TFSELF(v2_undef());
    if (argc < 1 || argv[0].t == T_NULL || argv[0].t == T_UNDEF) { a2_throw_error(vm, "TypeError", "Parameter type must be non-null.", 2007); return v2_undef(); }
    { const char *s = STR(0)->s; if (!strcmp(s, "input")) t->input = 1; else if (!strcmp(s, "dynamic")) t->input = 0; else { a2_throw_error(vm, "ArgumentError", "Parameter type must be one of the accepted values.", 2008); return v2_undef(); } }
    changed(vm, t); return v2_undef();
}
static V2 tf_autoSize_get(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_undef()); UNUSED; return v2_cstr(vm, t->autosize == 0 ? "none" : t->autosize == 2 ? "center" : t->autosize == 3 ? "right" : "left"); }
static V2 tf_autoSize_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    TFSELF(v2_undef());
    if (argc < 1 || argv[0].t == T_NULL || argv[0].t == T_UNDEF) { a2_throw_error(vm, "TypeError", "Parameter autoSize must be non-null.", 2007); return v2_undef(); }
    { const char *s = STR(0)->s; int a = !strcmp(s, "none") ? 0 : !strcmp(s, "left") ? 1 : !strcmp(s, "center") ? 2 : !strcmp(s, "right") ? 3 : -1;
      if (a < 0) { a2_throw_error(vm, "ArgumentError", "Parameter autoSize must be one of the accepted values.", 2008); return v2_undef(); }
      t->autosize = a; }
    changed(vm, t); return v2_undef();
}
static V2 tf_length_get(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_undef()); UNUSED; return v2_int(t->rt.len); }
static V2 tf_numLines(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_int(1)); UNUSED; tfs_layout(vm->player, t); return v2_int(t->lay.nlines ? t->lay.nlines : 1); }
static double tw_px(double tw) { return floor(tw + 0.5) / 20.0; }
static V2 tf_textWidth(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_num(0)); UNUSED; tfs_layout(vm->player, t); return v2_num(tw_px(t->lay.text_w)); }
static V2 tf_textHeight(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_num(0)); UNUSED; tfs_layout(vm->player, t); return v2_num(tw_px(t->lay.text_h * tfs_dev_vscale(t))); }
static V2 tf_scrollV_get(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_int(1)); UNUSED; clamp_scroll(vm, t); return v2_int(t->scroll_v); }
static V2 tf_scrollV_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    double v = NUM(0); int old, nv; TFSELF(v2_undef());
    old = t->scroll_v;
    nv = (v != v || v < 1) ? 1 : (v > 2147483647.0 ? 1 : (int)v);
    t->scroll_v = nv; clamp_scroll(vm, t);
    if (t->scroll_v != old) a2_dispatch_simple(vm, o, a2_str(vm, "scroll"), 0);
    return v2_undef();
}
static V2 tf_maxScrollV(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_int(1)); UNUSED; return v2_int(tfs_maxscroll_v(vm->player, t)); }
static V2 tf_bottomScrollV(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_int(1)); UNUSED; clamp_scroll(vm, t); return v2_int(tfs_bottom_scroll_v(vm->player, t)); }
static V2 tf_scrollH_get(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_int(0)); UNUSED; return v2_int((int)(t->scroll_h / 20)); }
static V2 tf_scrollH_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    int iv = INT(0); double v, m; double old; TFSELF(v2_undef());
    old = t->scroll_h; m = tfs_maxscroll_h(vm->player, t);
    v = iv < 0 ? -(double)iv : (double)iv;   /* Flash takes the magnitude */
    t->scroll_h = v * 20; if (t->scroll_h > m) t->scroll_h = m;
    if (t->scroll_h != old) a2_dispatch_simple(vm, o, a2_str(vm, "scroll"), 0);
    return v2_undef();
}
static V2 tf_maxScrollH(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_int(0)); UNUSED; return v2_int((int)(tfs_maxscroll_h(vm->player, t) / 20)); }
static V2 tf_zero(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_int(0); }
static V2 tf_noop(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_undef(); }
static V2 tf_null(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_null(); }

static int range_args(VM2 *vm, TextFieldState *t, int argc, V2 *argv, int *a, int *b, int single) {
    int len = t->rt.len, bi = argc > 0 && argv[0].t != T_HOLE ? v2_toint(vm, argv[0]) : -1, ei = argc > 1 && argv[1].t != T_HOLE ? v2_toint(vm, argv[1]) : -1;
    if (bi == -1 && ei == -1) { *a = 0; *b = len; return 1; }
    if (bi < -1 || ei < -1 || bi > len || ei > len) { a2_throw_error(vm, "RangeError", "The supplied index is out of bounds.", 2006); return 0; }
    if (ei == -1) { if (!single) { *a = bi; *b = len; } else { if (bi >= len) { a2_throw_error(vm, "RangeError", "The supplied index is out of bounds.", 2006); return 0; } *a = bi; *b = bi + 1; } return 1; }
    if (bi == -1) bi = 0;
    if (bi > ei) { a2_throw_error(vm, "RangeError", "The supplied index is out of bounds.", 2006); return 0; }
    *a = bi; *b = ei; return 1;
}
/* getTextRuns(beginIndex = 0, endIndex = int.MAX): one TextRun per format span */
static V2 tf_getTextRuns(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *arr = a2_new_array(vm); int bi = argc > 0 ? v2_toint(vm, argv[0]) : 0, ei = argc > 1 ? v2_toint(vm, argv[1]) : 0x7fffffff, i, pos = 0;
    A2Class *trc = a2_class_find_s(vm, "flash.text::TextRun"); TFSELF(v2_obj(arr));
    for (i = 0; i < t->rt.nspans; i++) {
        int a = pos, b = pos + t->rt.spans[i].len; pos = b;
        if (a < bi) a = bi; if (b > ei) b = ei;
        if (a >= b) continue;
        { V2 args[3]; args[0] = v2_int(a); args[1] = v2_int(b); args[2] = a2_textformat_from_rt(vm, &t->rt.spans[i].f);
          a2_arr_push(vm, arr, trc ? a2_construct_class(vm, trc, 3, args) : v2_null()); }
    }
    return v2_obj(arr);
}
static V2 tf_getTextFormat(VM2 *vm, V2 self, int argc, V2 *argv) {
    RTFmt f; int a, b, len; TFSELF(v2_null());
    len = t->rt.len;
    a = argc > 0 ? v2_toint(vm, argv[0]) : -1; b = argc > 1 ? v2_toint(vm, argv[1]) : -1;
    if (a == -1 && b == -1) { a = 0; b = len; if (!len) { rt_fmt_clear(&f); return a2_textformat_from_rt(vm, &f); } }
    else {
        if (a < -1 || b < -1) goto oob;
        if (b == -1) b = a + 1;
        if (a == -1) a = 0;
        if (a >= len || b > len || b == 0) goto oob;
        if (a >= b) { rt_fmt_clear(&f); return a2_textformat_from_rt(vm, &f); }
    }
    if (!rt_get_format(&t->rt, a, b, &f)) rt_fmt_clear(&f);
    return a2_textformat_from_rt(vm, &f);
oob:
    a2_throw_error(vm, "RangeError", "The supplied index is out of bounds.", 2006);
    return v2_undef();
}
/* paragraph-level fields apply to whole paragraphs */
static void para_range(TextFieldState *t, int *a, int *b) {
    while (*a > 0 && t->rt.text[*a - 1] != '\r') (*a)--;
    while (*b < t->rt.len && (*b == 0 || t->rt.text[*b - 1] != '\r')) (*b)++;
}
static V2 tf_setTextFormat(VM2 *vm, V2 self, int argc, V2 *argv) {
    RTFmt f, pf; int a, b; V2 rest[2]; TFSELF(v2_undef());
    if (argc < 1 || ARG(0).t == T_NULL || ARG(0).t == T_UNDEF) { a2_throw_error(vm, "TypeError", "Parameter format must be non-null.", 2007); return v2_undef(); }
    if (ARG(0).t != T_OBJ) return v2_undef();
    rest[0] = argc > 1 ? argv[1] : v2_int(-1); rest[1] = argc > 2 ? argv[2] : v2_int(-1);
    if (!range_args(vm, t, 2, rest, &a, &b, 1)) return v2_undef();
    a2_textformat_to_rt(vm, ARG(0), &f);
    pf = f; pf.has &= RTF_ALIGN | RTF_LEFTMARGIN | RTF_RIGHTMARGIN | RTF_INDENT | RTF_BLOCKINDENT | RTF_LEADING | RTF_TABSTOPS | RTF_BULLET;
    f.has &= ~(RTF_ALIGN | RTF_LEFTMARGIN | RTF_RIGHTMARGIN | RTF_INDENT | RTF_BLOCKINDENT | RTF_LEADING | RTF_TABSTOPS | RTF_BULLET);
    if (f.has) rt_set_format(&t->rt, a, b, &f);
    if (pf.has && t->rt.len) { int pa = a, pb = b > a ? b : a + 1; if (pb > t->rt.len) pb = t->rt.len; para_range(t, &pa, &pb); rt_set_format(&t->rt, pa, pb, &pf); }
    free(t->html_source); t->html_source = NULL;
    changed(vm, t);
    return v2_undef();
}
static V2 tf_defaultTextFormat_get(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_null()); UNUSED; return a2_textformat_from_rt(vm, &t->deffmt); }
static V2 tf_defaultTextFormat_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    RTFmt f; TFSELF(v2_undef());
    if (argc < 1 || ARG(0).t == T_NULL || ARG(0).t == T_UNDEF) { a2_throw_error(vm, "TypeError", "Parameter format must be non-null.", 2007); return v2_undef(); }
    if (ARG(0).t != T_OBJ) return v2_undef();
    a2_textformat_to_rt(vm, ARG(0), &f);
    if ((f.has & RTF_FONT) && !f.font[0]) f.has &= ~RTF_FONT;
    rt_fmt_apply(&t->deffmt, &f);
    if (t->rt.len == 0) rt_set_plain(&t->rt, NULL, 0, &t->deffmt);
    changed(vm, t);
    return v2_undef();
}

static void replace_range(VM2 *vm, TextFieldState *t, int a, int b, A2Str *s) {
    int n; u32 *u = to_field_text(s, &n);
    {   /* Flash: replaced text takes the format of the character after the range (the first
           replaced one at the end of the text), inserted text the default format */
        const RTFmt *fa = a < b ? rt_format_at(&t->rt, b < t->rt.len ? b : a) : NULL; RTFmt use = fa ? *fa : t->deffmt;
        rt_replace_as(&t->rt, a, b, u, n, &use, &t->deffmt); free(u);
    }
    free(t->html_source); t->html_source = NULL;
    clamp_sel(t);
    changed(vm, t);
}
static V2 tf_appendText(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_undef()); if (argc < 1 || argv[0].t == T_NULL) { a2_throw_error(vm, "TypeError", "Parameter newText must be non-null.", 2007); return v2_undef(); } replace_range(vm, t, t->rt.len, t->rt.len, STR(0)); return v2_undef(); }
static V2 tf_replaceText(VM2 *vm, V2 self, int argc, V2 *argv) {
    int a = INT(0), b = INT(1); TFSELF(v2_undef());
    if (argc < 3 || argv[2].t == T_NULL) { a2_throw_error(vm, "TypeError", "Parameter newText must be non-null.", 2007); return v2_undef(); }
    if (a < 0 || b < a || a > t->rt.len) return v2_undef();
    if (b > t->rt.len) b = t->rt.len;
    replace_range(vm, t, a, b, STR(2));
    return v2_undef();
}
static V2 tf_replaceSelectedText(VM2 *vm, V2 self, int argc, V2 *argv) {
    int a, b; TFSELF(v2_undef());
    if (argc < 1 || argv[0].t == T_NULL) { a2_throw_error(vm, "TypeError", "Parameter value must be non-null.", 2007); return v2_undef(); }
    a = t->sel_begin < t->sel_end ? t->sel_begin : t->sel_end; b = t->sel_begin < t->sel_end ? t->sel_end : t->sel_begin;
    replace_range(vm, t, a, b, STR(0)); clamp_sel(t);   /* the selection stays (Ruffle EditText::replace_text) */
    return v2_undef();
}

/* ---- lines and characters ---- */
static int line_check(VM2 *vm, TextFieldState *t, int i) { tfs_layout(vm->player, t); if (i < 0 || i >= t->lay.nlines) { a2_throw_error(vm, "RangeError", "The supplied index is out of bounds.", 2006); return 0; } return 1; }
static V2 tf_getLineText(VM2 *vm, V2 self, int argc, V2 *argv) { int i = INT(0); TFSELF(v2_undef()); if (!line_check(vm, t, i)) return v2_undef(); return cps_str(vm, t->rt.text + t->lay.lines[i].start, t->lay.lines[i].end - t->lay.lines[i].start); }
static V2 tf_getLineLength(VM2 *vm, V2 self, int argc, V2 *argv) { int i = INT(0); TFSELF(v2_undef()); if (!line_check(vm, t, i)) return v2_undef(); return v2_int(t->lay.lines[i].end - t->lay.lines[i].start); }
static V2 tf_getLineOffset(VM2 *vm, V2 self, int argc, V2 *argv) { int i = INT(0); TFSELF(v2_undef()); if (!line_check(vm, t, i)) return v2_undef(); return v2_int(t->lay.lines[i].start); }
static V2 tf_getLineIndexOfChar(VM2 *vm, V2 self, int argc, V2 *argv) { int i = INT(0); TFSELF(v2_undef()); tfs_layout(vm->player, t); if (i < 0 || i >= t->rt.len) return v2_int(-1); return v2_int(t->lay.cline[i]); }
/* paragraphs end with '\r'; the end of the text counts as a position of the last paragraph (one longer there) */
static int para_start(TextFieldState *t, int i) { int k; for (k = i; k > 0; k--) if (t->rt.text[k - 1] == '\r') break; return k; }
static V2 tf_getFirstCharInParagraph(VM2 *vm, V2 self, int argc, V2 *argv) { int i = INT(0); TFSELF(v2_undef()); if (i < 0 || i > t->rt.len) return v2_int(-1); return v2_int(para_start(t, i)); }
static V2 tf_getParagraphLength(VM2 *vm, V2 self, int argc, V2 *argv) {
    int i = INT(0), b, k; TFSELF(v2_undef());
    if (i < 0 || i > t->rt.len) return v2_int(-1);
    b = para_start(t, i);
    for (k = b; k < t->rt.len; k++) if (t->rt.text[k] == '\r') return v2_int(k - b + 1);
    return v2_int(t->rt.len - b + (i == t->rt.len ? 1 : 0));
}
static V2 tf_getLineMetrics(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Class *c = a2_class_find_s(vm, "flash.text::TextLineMetrics"); V2 a[6]; double m[6]; int i = INT(0), k; TFSELF(v2_null());
    if (!line_check(vm, t, i)) return v2_undef();
    tfs_line_metrics(vm->player, t, i, m);
    for (k = 0; k < 6; k++) a[k] = v2_num(m[k]);
    return a2_construct_class(vm, c, 6, a);
}
static V2 tf_getCharBoundaries(VM2 *vm, V2 self, int argc, V2 *argv) {
    double b[4]; TFSELF(v2_null());
    if (!tfs_char_bounds(vm->player, t, INT(0), b)) return v2_null();
    return v2_obj(a2_rect(vm, b[0], b[1], b[2], b[3]));
}
/* Twips::from_pixels; Flash moves the point 1 px right (Ruffle text_field.rs) */
static double pt_twips(double px) { double v = px * 20.0; if (v != v) return 0; if (v >= 2147483647.0) return 2147483647.0; if (v <= -2147483648.0) return -2147483648.0; return v < 0 ? ceil(v) : floor(v); }
static V2 tf_getCharIndexAtPoint(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_int(-1)); return v2_int(tfs_char_at_point(vm->player, t, pt_twips(NUM(0) + 1.0), pt_twips(NUM(1)))); }
static V2 tf_getLineIndexAtPoint(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_int(-1)); return v2_int(tfs_line_at_point(vm->player, t, pt_twips(NUM(0) + 1.0), pt_twips(NUM(1)))); }

/* ---- selection ---- */
static V2 tf_setSelection(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_undef()); t->sel_begin = INT(0); t->sel_end = INT(1); clamp_sel(t); return v2_undef(); }
static V2 tf_selectionBeginIndex(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_int(0)); UNUSED; clamp_sel(t); return v2_int(t->sel_begin < t->sel_end ? t->sel_begin : t->sel_end); }
static V2 tf_selectionEndIndex(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_int(0)); UNUSED; clamp_sel(t); return v2_int(t->sel_begin < t->sel_end ? t->sel_end : t->sel_begin); }
static V2 tf_selectedText(VM2 *vm, V2 self, int argc, V2 *argv) { int a, b; TFSELF(v2_undef()); UNUSED; clamp_sel(t); a = t->sel_begin < t->sel_end ? t->sel_begin : t->sel_end; b = t->sel_begin < t->sel_end ? t->sel_end : t->sel_begin; return cps_str(vm, t->rt.text + a, b - a); }
static V2 tf_caretIndex(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_int(0)); UNUSED; clamp_sel(t); return v2_int(t->sel_end); }
static V2 tf_restrict_get(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_null()); UNUSED; return t->restrict_chars ? v2_cstr(vm, t->restrict_chars) : v2_null(); }
static V2 tf_restrict_set(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_undef()); free(t->restrict_chars); t->restrict_chars = NULL; if (argc > 0 && argv[0].t != T_NULL && argv[0].t != T_UNDEF) { A2Str *s = STR(0); t->restrict_chars = (char*)malloc(s->len + 1); if (t->restrict_chars) memcpy(t->restrict_chars, s->s, s->len + 1); } return v2_undef(); }
static V2 tf_styleSheet_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Prop *p; UNUSED; p = SELFO ? a2_dyn_find(SELFO, a2_str(vm, "__styleSheet")) : NULL; return p ? p->v : v2_null(); }
static V2 tf_styleSheet_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    if (!SELFO) return v2_undef();
    a2_dyn_set_hidden(vm, SELFO, a2_str(vm, "__styleSheet"), ARG(0));
    { TextFieldState *t = tfs_of(vm, SELFO);
      if (t && ARG(0).t != T_OBJ) { free(t->html_source); t->html_source = NULL; }   /* htmlText comes from the content again */
      else if (t && t->html_source) { A2Str *h = a2_str(vm, t->html_source); t->restyle = 1; set_html(vm, SELFO, t, h); } }   /* a (new) style sheet restyles the HTML */
    return v2_undef();
}
static V2 tf_antiAliasType_get(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_cstr(vm, "normal"); }
static V2 tf_gridFitType_get(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_cstr(vm, "pixel"); }
/* render settings (Ruffle TextRenderSettings; CSMTextSettings for timeline fields) */
static V2 tf_aa_get(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_cstr(vm, "normal")); UNUSED; return v2_cstr(vm, t->aa_advanced ? "advanced" : "normal"); }
static V2 tf_aa_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    TFSELF(v2_undef());
    if (argc < 1 || argv[0].t == T_NULL || argv[0].t == T_UNDEF) { a2_throw_error(vm, "TypeError", "Parameter antiAliasType must be non-null.", 2007); return v2_undef(); }
    if (!strcmp(STR(0)->s, "advanced")) t->aa_advanced = 1; else if (!strcmp(STR(0)->s, "normal")) t->aa_advanced = 0;
    return v2_undef();
}
static V2 tf_gridfit_get(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_cstr(vm, "pixel")); UNUSED; return v2_cstr(vm, t->grid_fit == 1 ? "pixel" : t->grid_fit == 2 ? "subpixel" : "none"); }
static V2 tf_gridfit_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    TFSELF(v2_undef());
    if (argc < 1 || argv[0].t == T_NULL || argv[0].t == T_UNDEF) { a2_throw_error(vm, "TypeError", "Parameter gridFitType must be non-null.", 2007); return v2_undef(); }
    t->grid_fit = !strcmp(STR(0)->s, "pixel") ? 1 : !strcmp(STR(0)->s, "subpixel") ? 2 : 0;   /* anything else is none */
    return v2_undef();
}
static double clampd(double v, double lo, double hi) { if (v != v) return v; return v < lo ? lo : v > hi ? hi : v; }
static V2 tf_thickness_get(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_num(0)); UNUSED; return v2_num((double)t->thickness); }
static V2 tf_thickness_set(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_undef()); t->thickness = (float)clampd(argc ? NUM(0) : 0.0 / 0.0, -200, 200); return v2_undef(); }
static V2 tf_sharpness_get(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_num(0)); UNUSED; return v2_num((double)t->sharpness); }
static V2 tf_sharpness_set(VM2 *vm, V2 self, int argc, V2 *argv) { TFSELF(v2_undef()); t->sharpness = (float)clampd(argc ? NUM(0) : 0.0 / 0.0, -400, 400); return v2_undef(); }
static V2 tf_isFontCompatible(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_bool(1); }
static V2 TextField_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    extern V2 a2_displayobject_ctor_pub(VM2 *vm, V2 self);
    A2Obj *o = SELFO; (void)argc; (void)argv;
    a2_displayobject_ctor_pub(vm, self);
    if (!o) return v2_undef();
    tfs_of(vm, o);
    return v2_undef();
}

/* wheel over a field: scroll by the delta in lines */
void a2_text_wheel(VM2 *vm, A2Obj *o, int delta) {
    TextFieldState *t = tfs_of(vm, o); int old;
    if (!t || !t->mouse_wheel) return;
    old = t->scroll_v; t->scroll_v -= delta; clamp_scroll(vm, t);
    if (t->scroll_v != old) a2_dispatch_simple(vm, o, a2_str(vm, "scroll"), 0);
}
/* ---- editing ---- */
/* restrict: ranges (a-z), escapes (\x), carets toggling between allowing and
   disallowing; a character not allowed may still go in with the other case */
typedef struct { u32 lo, hi; } RInt;
static int in_ints(const RInt *v, int n, u32 c) { int i; for (i = 0; i < n; i++) if (c >= v[i].lo && c <= v[i].hi) return 1; return 0; }
static int restrict_allows(const u32 *u, int n, u32 ch) {
    RInt al[256], dis[256], cur[256]; int nal = 0, ndis = 0, ncur = 0, i = 0, allowing = 1, k; int have_last = 0; u32 last = 0;
    /* tokens: 0 char, 1 range, 2 caret */
    while (i < n) {
        int tok; u32 c = 0;
        if (u[i] == '\\') { if (i + 1 >= n) break; c = u[i + 1]; i += 2; tok = 0; }
        else if (u[i] == '^') { i++; tok = 2; }
        else if (u[i] == '-') { i++; tok = 1; }
        else { c = u[i++]; tok = 0; }
        if (tok == 0) { if (ncur < 256) { cur[ncur].lo = cur[ncur].hi = c; ncur++; } have_last = 1; last = c; }
        else if (tok == 2) {
            if (allowing) { if (!ncur && !nal) { al[0].lo = 0; al[0].hi = 0x10FFFF; nal = 1; } else for (k = 0; k < ncur && nal < 256; k++) al[nal++] = cur[k]; }
            else for (k = 0; k < ncur && ndis < 256; k++) dis[ndis++] = cur[k];
            ncur = 0; allowing = !allowing; have_last = 0;
        } else {
            u32 lo, hi;
            if (have_last) { lo = last; if (ncur) ncur--; } else lo = 0;
            /* the range end: the next plain character (an escaped one counts too) */
            if (i < n && u[i] != '^' && u[i] != '-') { if (u[i] == '\\') { if (i + 1 < n) { hi = u[i + 1]; i += 2; } else { hi = lo; i++; } } else hi = u[i++]; }
            else hi = lo;
            if (hi < lo) hi = lo;
            if (ncur < 256) { cur[ncur].lo = lo; cur[ncur].hi = hi; ncur++; }
            have_last = 0;
        }
    }
    if (allowing) for (k = 0; k < ncur && nal < 256; k++) al[nal++] = cur[k];
    else for (k = 0; k < ncur && ndis < 256; k++) dis[ndis++] = cur[k];
    return in_ints(al, nal, ch) && !in_ints(dis, ndis, ch);
}
/* the character to insert for ch, or 0 when it is refused (shared with AVM1) */
u32 tfs_restrict_char(TextFieldState *t, u32 ch);
static u32 allowed_char(TextFieldState *t, u32 ch) { return tfs_restrict_char(t, ch); }
u32 tfs_restrict_char(TextFieldState *t, u32 ch) {
    const char *r = t->restrict_chars; u32 *u; int n; u32 res = 0;
    if (!r) return ch;
    if (!*r) return 0;
    n = rt_utf8_decode(r, (int)strlen(r), &u); if (!u) return ch;
    if (restrict_allows(u, n, ch)) res = ch;
    else if (ch >= 'a' && ch <= 'z' && restrict_allows(u, n, ch - 32)) res = ch - 32;
    else if (ch >= 'A' && ch <= 'Z' && restrict_allows(u, n, ch + 32)) res = ch + 32;
    free(u);
    return res;
}
static void sel_range(TextFieldState *t, int *a, int *b) { *a = t->sel_begin < t->sel_end ? t->sel_begin : t->sel_end; *b = t->sel_begin < t->sel_end ? t->sel_end : t->sel_begin; }
/* scroll so the caret is visible (after typing or caret moves) */
static void caret_into_view(VM2 *vm, TextFieldState *t) {
    int c = t->sel_end, cl, bottom; double cx, vw;
    tfs_layout(vm->player, t);
    if (!t->lay.nlines) return;
    if (c > t->rt.len) c = t->rt.len;
    cl = t->lay.cline[c];
    if (c == t->rt.len && c > 0 && t->rt.text[c - 1] == '\r') cl = t->lay.nlines - 1;
    if (cl + 1 < t->scroll_v) t->scroll_v = cl + 1;
    else { int guard = 0; while (cl + 1 > (bottom = tfs_bottom_scroll_v(vm->player, t)) && t->scroll_v < t->lay.nlines && guard++ < 10000) t->scroll_v++; }
    if (!t->wordwrap) {
        cx = t->lay.cx[c]; vw = t->w - 80;
        if (cx - t->scroll_h > vw) t->scroll_h = cx - vw + vw / 4;
        if (cx < t->scroll_h) t->scroll_h = cx - vw / 4;
        if (t->scroll_h < 0) t->scroll_h = 0;
    }
}
/* type text (keyboard or paste) into the field at the selection */
static void insert_typed(VM2 *vm, A2Obj *o, TextFieldState *t, const u32 *in, int n, int fire_text_input) {
    u32 *buf = (u32*)malloc(((size_t)n + 1) * sizeof(u32)); int k = 0, i, a, b, avail;
    if (!buf) return;
    for (i = 0; i < n; i++) {
        u32 c = in[i];
        if (c == '\n') c = '\r';
        if (c == '\r' && !t->multiline) continue;
        if ((c < 32 && c != '\r' && c != '\t') || c == 127) continue;
        if (c == '\t' && fire_text_input) continue;   /* Tab moves the focus instead */
        buf[k++] = c;
    }
    sel_range(t, &a, &b);
    if (t->max_chars > 0 && t->max_chars - (t->rt.len - (b - a)) <= 0) { free(buf); return; }   /* full: no events */
    if (!k) { free(buf); return; }
    if (fire_text_input) {
        A2Obj *e = a2_make_event(vm, vm->c_textevent, vm->s_textInput, 1, 1); int bl; char *s;
        u32 *tmp = (u32*)malloc((size_t)k * sizeof(u32)); for (i = 0; i < k; i++) tmp[i] = buf[i] == '\r' ? '\n' : buf[i];
        s = rt_utf8_encode(tmp, k, &bl); free(tmp);
        a2_dyn_set(vm, e, a2_str(vm, "text"), v2_str(a2_intern(vm, s, (size_t)bl))); free(s);
        a2_dispatch_event_obj(vm, o, e);
        if (vm->throwing) a2_report_uncaught(vm);
        if (e->ival & 4) { free(buf); return; }
    }
    /* restrict, then maxChars */
    { int m = 0; for (i = 0; i < k; i++) { u32 c2 = buf[i] == '\r' ? '\r' : allowed_char(t, buf[i]); if (c2) buf[m++] = c2; } k = m; }
    if (t->max_chars > 0) { avail = t->max_chars - (t->rt.len - (b - a)); if (avail < 0) avail = 0; if (k > avail) k = avail; }
    rt_replace(&t->rt, a, b, buf, k, &t->deffmt); free(buf);   /* the selection goes even if every character was filtered out */
    t->sel_begin = t->sel_end = a + k;
    changed(vm, t);
    caret_into_view(vm, t);
    vm->player->caret_blink = 16;
    a2_dispatch_simple(vm, o, vm->s_change, 1);
}
static void delete_range(VM2 *vm, A2Obj *o, TextFieldState *t, int a, int b) {
    if (a < 0) a = 0; if (b > t->rt.len) b = t->rt.len;
    if (a >= b) return;
    rt_replace(&t->rt, a, b, NULL, 0, &t->deffmt);
    t->sel_begin = t->sel_end = a;
    changed(vm, t);
    caret_into_view(vm, t);
    vm->player->caret_blink = 16;
    a2_dispatch_simple(vm, o, vm->s_change, 1);
}
/* typed character (WM_CHAR) into the focused field */
void a2_text_input_char(VM2 *vm, A2Obj *o, int ch) {
    TextFieldState *t = tfs_of(vm, o); int a, b;
    if (!t->input) return;
    clamp_sel(t);
    if (ch == 8) { sel_range(t, &a, &b); if (a != b) delete_range(vm, o, t, a, b); else if (a > 0) delete_range(vm, o, t, a - 1, a); return; }
    if (ch == 13 || ch == 10) { u32 c = '\r'; if (t->multiline) insert_typed(vm, o, t, &c, 1, 1); return; }
    if (ch == 127) return;
    if (ch >= 32 || ch == 9) { u32 c = (u32)ch; if (ch == 9) return; insert_typed(vm, o, t, &c, 1, 1); }
}
/* IME (Ruffle EditText::ime): composition text, then the committed text is typed */
void a2_text_ime_preedit(VM2 *vm, A2Obj *o, const char *u8, int from, int to) {
    TextFieldState *t = tfs_of(vm, o);
    if (!t->input) return;
    clamp_sel(t);
    tfs_ime_preedit(t, u8, from, to);
    caret_into_view(vm, t);
}
void a2_text_ime_commit(VM2 *vm, A2Obj *o, const char *u8) {
    TextFieldState *t = tfs_of(vm, o); u32 *u = NULL; int n;
    if (!t->input || !u8) return;
    clamp_sel(t);
    n = rt_utf8_decode(u8, (int)strlen(u8), &u);
    if (u && n) insert_typed(vm, o, t, u, n, 1);
    free(u);
}
/* focus lost: the composition is committed */
void a2_text_ime_commit_pending(VM2 *vm, A2Obj *o) {
    TextFieldState *t = tfs_of(vm, o); char *s = tfs_ime_take(t);
    if (s) { a2_text_ime_commit(vm, o, s); free(s); }
}
extern void (*a2_set_clipboard_hook)(const char *utf8);
char *(*a2_get_clipboard_hook)(void);   /* malloc'd UTF-8 or NULL; set by the platform layer */
static int word_char(u32 c) { return c > 127 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
/* editing keys (keyDown) for the focused field; returns 1 if handled */
int a2_text_key(VM2 *vm, A2Obj *o, int vk, int shift, int ctrl) {
    TextFieldState *t = tfs_of(vm, o); int a, b, c, len = t->rt.len;
    clamp_sel(t);
    sel_range(t, &a, &b); c = t->sel_end;
    if (ctrl && (vk == 'A')) { if (t->selectable) { t->sel_begin = 0; t->sel_end = len; } return 1; }
    if (ctrl && (vk == 'C' || vk == 'X')) {
        if (a != b && !t->password) {
            int bl; char *s = rt_utf8_encode(t->rt.text + a, b - a, &bl);
            if (s) { int i; for (i = 0; i < bl; i++) if (s[i] == '\r') s[i] = '\n'; if (a2_set_clipboard_hook) a2_set_clipboard_hook(s); free(s); }
            if (vk == 'X' && t->input) delete_range(vm, o, t, a, b);
        }
        return 1;
    }
    if (ctrl && vk == 'V') {
        if (t->input && a2_get_clipboard_hook) { char *s = a2_get_clipboard_hook(); if (s) { u32 *u; int n = rt_utf8_decode(s, (int)strlen(s), &u); free(s); if (u) { int i, k = 0; for (i = 0; i < n; i++) { if (u[i] == '\r' && i + 1 < n && u[i + 1] == '\n') continue; u[k++] = u[i]; } insert_typed(vm, o, t, u, k, 1); free(u); } } }
        return 1;
    }
    switch (vk) {
    case 46:   /* Delete */
        if (!t->input) return 0;
        if (a != b) delete_range(vm, o, t, a, b); else if (c < len) { int e = c + 1; if (ctrl) { while (e < len && word_char(t->rt.text[e])) e++; } delete_range(vm, o, t, c, e); }
        return 1;
    case 37: case 39: {   /* Left / Right */
        int n = c;
        if (!shift && a != b) n = vk == 37 ? a : b;
        else if (vk == 37 && n > 0) { n--; if (ctrl) { while (n > 0 && !word_char(t->rt.text[n])) n--; while (n > 0 && word_char(t->rt.text[n - 1])) n--; } }
        else if (vk == 39 && n < len) { n++; if (ctrl) { while (n < len && word_char(t->rt.text[n])) n++; while (n < len && !word_char(t->rt.text[n])) n++; } }
        if (shift && t->selectable) t->sel_end = n; else t->sel_begin = t->sel_end = n;
        caret_into_view(vm, t);
        vm->player->caret_blink = 16;
        return 1; }
    case 36: case 35: {   /* Home / End */
        int n;
        tfs_layout(vm->player, t);
        if (ctrl) n = vk == 36 ? 0 : len;
        else { TFLine *ln = &t->lay.lines[t->lay.cline[c < len ? c : len]]; n = vk == 36 ? ln->start : ln->end; if (vk == 35 && n > ln->start && n <= len && n > 0 && t->rt.text[n - 1] == '\r') n--; }
        if (shift && t->selectable) t->sel_end = n; else t->sel_begin = t->sel_end = n;
        caret_into_view(vm, t);
        vm->player->caret_blink = 16;
        return 1; }
    case 38: case 40: {   /* Up / Down */
        int li, n; double x;
        tfs_layout(vm->player, t);
        if (!t->lay.nlines) return 1;
        li = t->lay.cline[c < len ? c : len];
        if (c == len && len && t->rt.text[len - 1] == '\r') li = t->lay.nlines - 1;
        x = t->lay.cx[c];
        li += vk == 38 ? -1 : 1;
        if (li < 0) n = 0; else if (li >= t->lay.nlines) n = len;
        else { TFLine *ln = &t->lay.lines[li]; int e = ln->end; while (e > ln->start && t->rt.text[e - 1] == '\r') e--; for (n = ln->start; n < e; n++) if (x < t->lay.cx[n] + t->lay.cw[n] / 2) break; }
        if (shift && t->selectable) t->sel_end = n; else t->sel_begin = t->sel_end = n;
        /* keep the caret line visible */
        { int cl = t->lay.cline[n < len ? n : len]; if (cl + 1 < t->scroll_v) t->scroll_v = cl + 1; else if (cl + 1 > tfs_bottom_scroll_v(vm->player, t)) { t->scroll_v += cl + 1 - tfs_bottom_scroll_v(vm->player, t); clamp_scroll(vm, t); } }
        vm->player->caret_blink = 16;
        return 1; }
    default: return 0;
    }
}
/* a press on a link (Ruffle EditText::open_url): "event:" sends TextEvent.LINK, others are opened */
void a2_text_press_link(VM2 *vm, A2Obj *o, double x, double y) {
    TextFieldState *t = tfs_of(vm, o); int ci; const RTFmt *f;
    ci = tfs_char_at_point(vm->player, t, x, y);
    if (ci < 0 || ci >= t->rt.len) return;
    f = rt_format_at(&t->rt, ci);
    if (!f || !f->url[0]) return;
    if (!strncmp(f->url, "event:", 6)) {
        A2Obj *e = a2_make_event(vm, vm->c_textevent, a2_str(vm, "link"), 1, 0);
        a2_dyn_set(vm, e, a2_str(vm, "text"), v2_str(a2_str(vm, f->url + 6)));
        a2_dispatch_event_obj(vm, o, e);
    } else if (strncmp(f->url, "asfunction:", 11)) { extern void plat_open_url(const char *url); plat_open_url(f->url); }
}
/* mouse in a field: place the caret / extend the selection (field-local twips) */
/* word segments for selection (an approximation of UAX #29: letters, digits and '_'
   join, as do ' and . between them; runs of spaces; anything else alone) */
static int wordish(u32 c) { return c > 127 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int seg_end(const u32 *s, int n, int i) {
    if (i >= n) return n;
    if (wordish(s[i])) { i++; while (i < n && (wordish(s[i]) || ((s[i] == '\'' || s[i] == '.') && i + 1 < n && wordish(s[i + 1]) && wordish(s[i - 1])))) i++; return i; }
    if (s[i] == ' ' || s[i] == '\t') { while (i < n && (s[i] == ' ' || s[i] == '\t')) i++; return i; }
    return i + 1;
}
static int is_blank(u32 c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == 0xA0; }
static int word_prev(TextFieldState *t, int pos) {
    const u32 *s = t->rt.text; int i = 0, last = 0;
    if (pos > 0 && is_blank(s[pos - 1])) return pos;
    while (i < pos) { int e = seg_end(s, pos, i); if (!is_blank(s[i])) last = i; i = e; }
    return last;
}
static int word_next(TextFieldState *t, int pos) {
    const u32 *s = t->rt.text; int n = t->rt.len, i = pos;
    if (pos < n && is_blank(s[pos])) return pos;
    while (i < n && is_blank(s[i])) i = seg_end(s, n, i);
    return i < n ? seg_end(s, n, i) : n;
}
static void sel_for(TextFieldState *t, int pos, int mode, int *a, int *b) {
    if (mode == 0) { *a = *b = pos; return; }
    if (mode == 1) { *a = word_prev(t, pos); *b = word_next(t, pos); return; }
    { int i = pos, j = pos; while (i > 0 && t->rt.text[i - 1] != '\r') i--; while (j < t->rt.len && t->rt.text[j] != '\r') j++; *a = i; *b = j; }
}
void a2_text_mouse(VM2 *vm, A2Obj *o, double x, double y, int down, int drag) {
    TextFieldState *t = tfs_of(vm, o); int pos, mode = vm->click_index > 2 ? 2 : vm->click_index;
    if (!t->selectable) return;
    pos = tfs_caret_at_point(vm->player, t, x, y);
    if (down && !drag) { int a, b; t->click_pos = pos; t->click_mode = mode; sel_for(t, pos, mode, &a, &b); t->sel_begin = a; t->sel_end = b; }
    else if (drag) {
        int fa, fb, ca, cb;   /* span from the first click's selection to the current one (by words / lines) */
        sel_for(t, t->click_pos, t->click_mode, &fa, &fb); sel_for(t, pos, t->click_mode, &ca, &cb);
        if (fa < ca && fb < cb) { t->sel_begin = fa; t->sel_end = cb; }
        else if (ca < fa && cb < fb) { t->sel_begin = fb; t->sel_end = ca; }
        else { t->sel_begin = fa < ca ? fa : ca; t->sel_end = fb > cb ? fb : cb; }
    }
    vm->player->caret_blink = 16;
}
int a2_str_ulen_pub(A2Str *s) { extern int a2_str_ulen(A2Str *s); return s ? a2_str_ulen(s) : 0; }

/* ---- TextFormat (plain dynamic object with typed fields) ---- */
/* ---- TextFormat: typed properties, null when unset ---- */
static double round_half_even(double x) { double f = floor(x), d = x - f; if (d > 0.5) return f + 1; if (d < 0.5) return f; return fmod(f, 2.0) == 0 ? f : f + 1; }
static V2 tfm_coerce(VM2 *vm, char kind, V2 v) {
    if (v.t == T_NULL || v.t == T_UNDEF) return v2_null();
    switch (kind) {
    case 'S': case 'A': case 'D': { A2Str *s = v2_tostr(vm, v); return v2_str(s); }
    case 'B': return v2_bool(v2_tobool(vm, v));
    case 'F': return v2_num(v2_tonum(vm, v));
    case 'C': return v2_int(v2_toint(vm, v));
    case 'N': { double d = v2_tonum(vm, v), r; if (d != d) return v2_int((s32)0x80000000u); r = round_half_even(d); if (r < -2147483648.0 || r > 2147483647.0) return v2_int((s32)0x80000000u); return v2_int((s32)r); }
    case 'T': if (v.t == T_OBJ && v.u.o->kind == OK_ARRAY) { A2Obj *a = a2_new_array(vm); int i; for (i = 0; i < v.u.o->alen; i++) a2_arr_push(vm, a, tfm_coerce(vm, 'N', v.u.o->arr[i])); return v2_obj(a); } return v2_null();
    }
    return v;
}
static V2 tfm_get(VM2 *vm, V2 self, const char *name) { char k[40]; A2Prop *p; if (self.t != T_OBJ) return v2_null(); sprintf(k, "__tf_%.120s", name); p = a2_dyn_find(self.u.o, a2_str(vm, k)); return p ? p->v : v2_null(); }
static void tfm_set(VM2 *vm, V2 self, const char *name, char kind, V2 v) {
    char k[40]; V2 c;
    if (self.t != T_OBJ) return;
    if (kind == 'D') {   /* display: block / inline / none; anything else reads back as null */
        if (v.t == T_NULL || v.t == T_UNDEF) { a2_throw_error(vm, "TypeError", "Parameter display must be non-null.", 2007); return; }
        c = v2_str(v2_tostr(vm, v)); if (vm->throwing) return;
        if (strcmp(c.u.s->s, "block") && strcmp(c.u.s->s, "inline") && strcmp(c.u.s->s, "none")) c = v2_null();
        a2_dyn_set_hidden(vm, self.u.o, a2_str(vm, "__tf_display"), c); return;
    }
    c = tfm_coerce(vm, kind, v); if (vm->throwing) return;
    if (kind == 'S' && !strcmp(name, "font") && c.t == T_STR) { u32 *u; int n = rt_utf8_decode(c.u.s->s, (int)c.u.s->len, &u); if (n > 64) c = cps_str(vm, u, 64); free(u); }   /* font names: 64 characters at most */
    sprintf(k, "__tf_%.120s", name); a2_dyn_set_hidden(vm, self.u.o, a2_str(vm, k), c);
}
static V2 tfm_font_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return tfm_get(vm, self, "font"); }
static V2 tfm_font_set(VM2 *vm, V2 self, int argc, V2 *argv) { tfm_set(vm, self, "font", 'S', ARG(0)); return v2_undef(); }
static V2 tfm_size_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return tfm_get(vm, self, "size"); }
static V2 tfm_size_set(VM2 *vm, V2 self, int argc, V2 *argv) { tfm_set(vm, self, "size", 'N', ARG(0)); return v2_undef(); }
static V2 tfm_color_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return tfm_get(vm, self, "color"); }
static V2 tfm_color_set(VM2 *vm, V2 self, int argc, V2 *argv) { tfm_set(vm, self, "color", 'C', ARG(0)); return v2_undef(); }
static V2 tfm_bold_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return tfm_get(vm, self, "bold"); }
static V2 tfm_bold_set(VM2 *vm, V2 self, int argc, V2 *argv) { tfm_set(vm, self, "bold", 'B', ARG(0)); return v2_undef(); }
static V2 tfm_italic_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return tfm_get(vm, self, "italic"); }
static V2 tfm_italic_set(VM2 *vm, V2 self, int argc, V2 *argv) { tfm_set(vm, self, "italic", 'B', ARG(0)); return v2_undef(); }
static V2 tfm_underline_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return tfm_get(vm, self, "underline"); }
static V2 tfm_underline_set(VM2 *vm, V2 self, int argc, V2 *argv) { tfm_set(vm, self, "underline", 'B', ARG(0)); return v2_undef(); }
static V2 tfm_url_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return tfm_get(vm, self, "url"); }
static V2 tfm_url_set(VM2 *vm, V2 self, int argc, V2 *argv) { tfm_set(vm, self, "url", 'S', ARG(0)); return v2_undef(); }
static V2 tfm_target_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return tfm_get(vm, self, "target"); }
static V2 tfm_target_set(VM2 *vm, V2 self, int argc, V2 *argv) { tfm_set(vm, self, "target", 'S', ARG(0)); return v2_undef(); }
static V2 tfm_align_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return tfm_get(vm, self, "align"); }
static V2 tfm_align_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 v = ARG(0);
    if (v.t != T_NULL && v.t != T_UNDEF) {
        A2Str *as = v2_tostr(vm, v); const char *a = as->s;
        if (vm->throwing) return v2_undef();
        v = v2_str(as);   /* converted once */
        if (strcmp(a, "left") && strcmp(a, "center") && strcmp(a, "right") && strcmp(a, "justify") && strcmp(a, "start") && strcmp(a, "end")) { a2_throw_error(vm, "ArgumentError", "Parameter align must be one of the accepted values.", 2008); return v2_undef(); }
    }
    tfm_set(vm, self, "align", 'A', v); return v2_undef();
}
static V2 tfm_leftMargin_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return tfm_get(vm, self, "leftMargin"); }
static V2 tfm_leftMargin_set(VM2 *vm, V2 self, int argc, V2 *argv) { tfm_set(vm, self, "leftMargin", 'N', ARG(0)); return v2_undef(); }
static V2 tfm_rightMargin_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return tfm_get(vm, self, "rightMargin"); }
static V2 tfm_rightMargin_set(VM2 *vm, V2 self, int argc, V2 *argv) { tfm_set(vm, self, "rightMargin", 'N', ARG(0)); return v2_undef(); }
static V2 tfm_indent_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return tfm_get(vm, self, "indent"); }
static V2 tfm_indent_set(VM2 *vm, V2 self, int argc, V2 *argv) { tfm_set(vm, self, "indent", 'N', ARG(0)); return v2_undef(); }
static V2 tfm_leading_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return tfm_get(vm, self, "leading"); }
static V2 tfm_leading_set(VM2 *vm, V2 self, int argc, V2 *argv) { tfm_set(vm, self, "leading", 'N', ARG(0)); return v2_undef(); }
static V2 tfm_blockIndent_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return tfm_get(vm, self, "blockIndent"); }
static V2 tfm_blockIndent_set(VM2 *vm, V2 self, int argc, V2 *argv) { tfm_set(vm, self, "blockIndent", 'N', ARG(0)); return v2_undef(); }
static V2 tfm_bullet_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return tfm_get(vm, self, "bullet"); }
static V2 tfm_bullet_set(VM2 *vm, V2 self, int argc, V2 *argv) { tfm_set(vm, self, "bullet", 'B', ARG(0)); return v2_undef(); }
static V2 tfm_kerning_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return tfm_get(vm, self, "kerning"); }
static V2 tfm_kerning_set(VM2 *vm, V2 self, int argc, V2 *argv) { tfm_set(vm, self, "kerning", 'B', ARG(0)); return v2_undef(); }
static V2 tfm_letterSpacing_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return tfm_get(vm, self, "letterSpacing"); }
static V2 tfm_letterSpacing_set(VM2 *vm, V2 self, int argc, V2 *argv) { tfm_set(vm, self, "letterSpacing", 'F', ARG(0)); return v2_undef(); }
static V2 tfm_tabStops_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return tfm_get(vm, self, "tabStops"); }
static V2 tfm_tabStops_set(VM2 *vm, V2 self, int argc, V2 *argv) { tfm_set(vm, self, "tabStops", 'T', ARG(0)); return v2_undef(); }
static V2 tfm_display_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return tfm_get(vm, self, "display"); }
static V2 tfm_display_set(VM2 *vm, V2 self, int argc, V2 *argv) { tfm_set(vm, self, "display", 'D', ARG(0)); return v2_undef(); }
static V2 TextFormat_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    static const char *const keys[] = { "font", "size", "color", "bold", "italic", "underline", "url", "target", "align", "leftMargin", "rightMargin", "indent", "leading" };
    static const char kinds[] = "SNCBBBSSANNNN"; int i;
    if (self.t != T_OBJ) return v2_undef();
    for (i = 0; i < 13 && i < argc; i++) tfm_set(vm, self, keys[i], kinds[i], argv[i]);
    a2_dyn_set_hidden(vm, self.u.o, a2_str(vm, "__tf_display"), v2_cstr(vm, "block"));
    return v2_undef();
}
/* StaticText */
static V2 stext_text(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = tself(self); DisplayObject *d = o ? a2_dobj_of(o) : NULL; extern int text_static_string(Player *p, SWFChar *ch, char *buf, size_t cap); char buf[2048]; UNUSED; if (!d || !d->ch) return v2_str(vm->s_empty); if (text_static_string(vm->player, d->ch, buf, sizeof(buf))) return v2_cstr(vm, buf); return v2_null(); }
/* Font */
/* Font objects carry a reference to a DefineFont character (Ruffle FontObject) */
extern int font_ref(Player *p, SWFMovie *m, int id);
extern FontDef *font_of_ref(Player *p, int ref);
extern void font_register_global(Player *p, int ref);
static void font_bind(VM2 *vm, A2Obj *o, int ref) {
    FontDef *f = font_of_ref(vm->player, ref);
    if (!o || !f) return;
    a2_dyn_set_hidden(vm, o, a2_str(vm, "__fontRef"), v2_int(ref));
}
static FontDef *font_self(VM2 *vm, V2 self) { A2Prop *p = self.t == T_OBJ ? a2_dyn_find(self.u.o, a2_str(vm, "__fontRef")) : NULL; return p && p->v.t == T_INT ? font_of_ref(vm->player, p->v.u.i) : NULL; }
/* the font character a class is linked to (SymbolClass), or -1 */
static int font_class_ref(VM2 *vm, A2Class *c) {
    for (; c; c = c->super) if (c->symbol_char >= 0 && c->symbol_movie) {
        if (c->symbol_char == 0) return -2;   /* linked to a character this player does not keep (DefineFont4) */
        SWFMovie *m = c->symbol_movie ? c->symbol_movie : vm->player->movie; SWFChar *ch = swf_get_char(m, c->symbol_char);
        if (!ch) return -2;   /* a character this player does not keep (DefineFont4) */
        return ch->type == CHAR_FONT ? font_ref(vm->player, m, c->symbol_char) : -1;
    }
    return -1;
}
static int font_name_cmp(FontDef *a, FontDef *b) {
    const char *x = a->name, *y = b->name;
    for (;; x++, y++) { int cx = (*x >= 'A' && *x <= 'Z') ? *x + 32 : (u8)*x, cy = (*y >= 'A' && *y <= 'Z') ? *y + 32 : (u8)*y; if (cx != cy || !cx) return cx - cy; }
}
static V2 font_enumerateFonts(VM2 *vm, V2 self, int argc, V2 *argv) {
    extern struct SWFMovie *a2_code_movie(VM2 *vm);
    Player *p = vm->player; A2Obj *arr = a2_new_array(vm); A2Class *fc = a2_class_find_s(vm, "flash.text::Font");
    SWFMovie *m = (SWFMovie*)a2_code_movie(vm); int *refs, n = 0, cap, i, j;
    UNUSED;
    cap = p->ngfont + (m ? m->maxid + 1 : 0) + 1;
    refs = (int*)malloc((size_t)cap * sizeof(int)); if (!refs) return v2_obj(arr);
    for (i = 0; i < p->ngfont; i++) refs[n++] = p->gfont[i];
    if (m) for (i = 0; i <= m->maxid; i++) if (m->dict[i].type == CHAR_FONT) { FontDef *f = font_get(m, i); if (f && f->has_layout && f->version >= 2) refs[n++] = font_ref(p, m, i); }
    /* sorted by name, ignoring case (stable) */
    for (i = 1; i < n; i++) { int r = refs[i]; FontDef *fr = font_of_ref(p, r); j = i - 1; while (j >= 0 && font_name_cmp(font_of_ref(p, refs[j]), fr) > 0) { refs[j + 1] = refs[j]; j--; } refs[j + 1] = r; }
    for (i = 0; i < n; i++) { A2Obj *f = a2_new(vm, fc); font_bind(vm, f, refs[i]); a2_arr_push(vm, arr, v2_obj(f)); }
    free(refs);
    return v2_obj(arr);
}
static V2 font_fontName(VM2 *vm, V2 self, int argc, V2 *argv) { FontDef *f = font_self(vm, self); UNUSED; return f ? v2_cstr(vm, f->name) : v2_null(); }
static V2 font_fontStyle(VM2 *vm, V2 self, int argc, V2 *argv) { FontDef *f = font_self(vm, self); UNUSED; if (!f) return v2_null(); return v2_cstr(vm, f->bold && f->italic ? "boldItalic" : f->bold ? "bold" : f->italic ? "italic" : "regular"); }
static V2 font_fontType(VM2 *vm, V2 self, int argc, V2 *argv) { FontDef *f = font_self(vm, self); UNUSED; return f ? v2_cstr(vm, "embedded") : v2_null(); }
static V2 font_hasGlyphs(VM2 *vm, V2 self, int argc, V2 *argv) {
    FontDef *f = font_self(vm, self); u32 *u; int n, i, ok = 1;
    if (!f) return v2_bool(0);
    if (argc < 1 || argv[0].t == T_NULL || argv[0].t == T_UNDEF) return v2_bool(1);
    n = rt_utf8_decode(STR(0)->s, (int)STR(0)->len, &u);
    for (i = 0; i < n && ok; i++) if (font_glyph_for_code(f, (int)u[i]) < 0) ok = 0;
    free(u);
    return v2_bool(ok);
}
static V2 font_registerFont(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 c = ARG(0); int ref = -1;
    (void)self;
    if (c.t == T_NULL || c.t == T_UNDEF) { a2_throw_error(vm, "TypeError", "Parameter font must be non-null.", 2007); return v2_undef(); }
    if (c.t == T_OBJ && c.u.o->kind == OK_CLASS && c.u.o->as_class) ref = font_class_ref(vm, c.u.o->as_class);
    if (ref == -2) return v2_undef();
    if (ref < 0) { a2_throw_error(vm, "ArgumentError", "The value specified for argument font is invalid.", 1508); return v2_undef(); }
    font_register_global(vm->player, ref);
    return v2_undef();
}
static V2 Font_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; if (self.t == T_OBJ && self.u.o->cls) { int ref = font_class_ref(vm, self.u.o->cls); if (ref >= 0) font_bind(vm, self.u.o, ref); } return v2_undef(); }
static V2 tlm_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = SELFO; const char *k[] = { "x", "width", "height", "ascent", "descent", "leading" }; int i; if (!o) return v2_undef(); for (i = 0; i < 6; i++) a2_dyn_set(vm, o, a2_str(vm, k[i]), i < argc ? argv[i] : v2_num(0)); return v2_undef(); }
static V2 ss_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_undef(); }
/* StyleSheet (Ruffle's StyleSheet.as): lower-cased names map to shallow copies of
   the style objects (getStyle) and to the TextFormats transform() made of them */
static A2Obj *ss_hidden(VM2 *vm, A2Obj *ss, const char *k) { A2Prop *p = a2_dyn_find(ss, a2_str(vm, k)); A2Obj *t; if (p && p->v.t == T_OBJ) return p->v.u.o; t = a2_new_plain(vm); a2_dyn_set_hidden(vm, ss, a2_str(vm, k), v2_obj(t)); return t; }
static A2Obj *ss_table(VM2 *vm, A2Obj *ss) { return ss_hidden(vm, ss, "__css"); }
static A2Obj *ss_fmts(VM2 *vm, A2Obj *ss) { return ss_hidden(vm, ss, "__cssfmt"); }
static A2Str *lower_str(VM2 *vm, A2Str *s) { char *b = (char*)malloc(s->len + 1); u32 i; A2Str *r; if (!b) return s; for (i = 0; i < s->len; i++) b[i] = (char)(s->s[i] >= 'A' && s->s[i] <= 'Z' ? s->s[i] + 32 : s->s[i]); b[s->len] = 0; r = a2_intern(vm, b, s->len); free(b); return r; }
/* for (key in o) copy[key] = o[key] */
static V2 copy_style(VM2 *vm, V2 o) {
    A2Obj *r = a2_new_plain(vm); int idx = 0; A2Str *k; V2 v;
    if (o.t == T_OBJ) for (;;) { a2_dyn_enum(o.u.o, &idx, &k, &v); if (!idx) break; a2_dyn_set(vm, r, k, v); }
    return v2_obj(r);
}
static V2 ss_setStyle(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = SELFO; A2Str *ln; V2 fmt, cp;
    if (!o) return v2_undef();
    if (argc < 1 || argv[0].t == T_NULL || argv[0].t == T_UNDEF) { a2_throw_error(vm, "TypeError", "Error #1009: Cannot access a property or method of a null object reference.", 1009); return v2_undef(); }
    ln = lower_str(vm, STR(0));
    a2_dyn_set(vm, ss_table(vm, o), ln, copy_style(vm, ARG(1)));
    cp = copy_style(vm, ARG(1));
    fmt = a2_call_s(vm, self, "transform", 1, &cp);   /* may be overridden */
    if (vm->throwing) return v2_undef();
    if (fmt.t == T_OBJ && fmt.u.o->cls && a2_is_subclass(fmt.u.o->cls, vm->c_textformat)) a2_dyn_set(vm, ss_fmts(vm, o), ln, fmt);
    else a2_dyn_del(ss_fmts(vm, o), ln);
    { A2Prop *p = a2_dyn_find(o, a2_str(vm, "__ssgen")); a2_dyn_set_hidden(vm, o, a2_str(vm, "__ssgen"), v2_int(p && p->v.t == T_INT ? p->v.u.i + 1 : 1)); }
    return v2_undef();
}
static V2 ss_getStyle(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Prop *p;
    if (!SELFO) return v2_null();
    if (argc < 1 || argv[0].t == T_NULL || argv[0].t == T_UNDEF) { a2_throw_error(vm, "TypeError", "Error #1009: Cannot access a property or method of a null object reference.", 1009); return v2_undef(); }
    p = a2_dyn_find(ss_table(vm, SELFO), lower_str(vm, STR(0)));
    return copy_style(vm, p ? p->v : v2_undef());
}
static V2 ss_clear(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; if (SELFO) { a2_dyn_set_hidden(vm, SELFO, a2_str(vm, "__css"), v2_obj(a2_new_plain(vm))); a2_dyn_set_hidden(vm, SELFO, a2_str(vm, "__cssfmt"), v2_obj(a2_new_plain(vm))); } return v2_undef(); }
/* Ruffle html/style_sheet.rs CssStream: selectors -> properties (names camelCased, values as written) */
typedef struct { const char *s; int n, pos; } CssIn;
static int css_peek(CssIn *c) { return c->pos < c->n ? (u8)c->s[c->pos] : -1; }
static int css_ws(int ch) { return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t'; }
static int css_skip(CssIn *c) {
    int found = 0;
    for (;;) {
        if (c->pos + 1 < c->n && c->s[c->pos] == '/' && c->s[c->pos + 1] == '*') {
            int p = c->pos + 2, closed = 0;
            while (p + 1 < c->n) { if (c->s[p] == '*' && c->s[p + 1] == '/') { closed = 1; break; } p++; }
            if (!closed) { c->pos = c->n; return found; }   /* EOF inside the comment */
            c->pos = p + 2; found = 1; continue;
        }
        if (css_ws(css_peek(c))) { c->pos++; found = 1; continue; }
        return found;
    }
}
static int css_until(CssIn *c, const char *stop) { int st = c->pos; while (c->pos < c->n && !strchr(stop, c->s[c->pos])) c->pos++; return st; }
static V2 css_str(VM2 *vm, const char *s, int n) { return v2_str(a2_intern(vm, s, (size_t)n)); }
static A2Str *css_camel(VM2 *vm, const char *s, int n) {
    char *b = (char*)malloc((size_t)n + 1); int i, k = 0, up = 0; A2Str *r;
    if (!b) return a2_intern(vm, s, (size_t)n);
    for (i = 0; i < n; i++) { char ch = s[i]; if (ch == '-' && !up) { up = 1; continue; } if (up) { up = 0; if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32); } b[k++] = ch; }
    r = a2_intern(vm, b, (size_t)k); free(b); return r;
}
/* returns 0 on a syntax error (the whole text is then ignored) */
static int css_parse(VM2 *vm, CssIn *c, A2Obj *result) {
    for (;;) {
        int sel[64][2], nsel = 0, i; A2Obj *props;
        css_skip(c);
        if (css_peek(c) < 0) return 1;
        for (;;) {   /* selectors */
            int st; css_skip(c); st = css_until(c, "{, \n\r\t");
            if (c->pos > st && nsel < 64) { sel[nsel][0] = st; sel[nsel][1] = c->pos - st; nsel++; }
            css_skip(c);
            if (css_peek(c) == '{') { c->pos++; if (!nsel) { sel[0][0] = c->pos; sel[0][1] = 0; nsel = 1; } break; }
            if (css_peek(c) == ',') { c->pos++; continue; }
            return 0;
        }
        props = a2_new_plain(vm);
        for (;;) {   /* properties */
            int ns, ne, vs, ve, ch;
            css_skip(c);
            ch = css_peek(c);
            if (ch == '}') { c->pos++; break; }
            if (ch < 0) break;
            ns = css_until(c, ": \n\r\t"); ne = c->pos;
            if (css_skip(c)) { ch = css_peek(c); if (ch == ':') ne = c->pos; else if (ch >= 0) return 0; }
            if (css_peek(c) < 0) return 0;
            c->pos++;   /* the colon */
            css_skip(c);
            vs = css_until(c, ";:}"); ve = c->pos;
            for (;;) {
                ch = css_peek(c);
                if (ch == ':') {
                    int pv; c->pos = vs; pv = css_until(c, "\n\r");
                    if (css_peek(c) == '\n' || css_peek(c) == '\r') { a2_dyn_set(vm, props, css_camel(vm, c->s + ns, ne - ns), css_str(vm, c->s + pv, c->pos - pv)); c->pos++; goto next_prop; }
                    c->pos = vs; vs = css_until(c, ";}"); ve = c->pos; continue;
                }
                if (ch == ';') { c->pos++; a2_dyn_set(vm, props, css_camel(vm, c->s + ns, ne - ns), css_str(vm, c->s + vs, ve - vs)); goto next_prop; }
                if (ch == '}') {
                    int e = vs; c->pos++;
                    while (e < ve && c->s[e] != '\n' && c->s[e] != '\r') e++;
                    a2_dyn_set(vm, props, css_camel(vm, c->s + ns, ne - ns), css_str(vm, c->s + vs, e - vs));
                    goto block_done;
                }
                return 0;
            }
        next_prop: ;
        }
    block_done:
        for (i = 0; i < nsel; i++) a2_dyn_set(vm, result, a2_intern(vm, c->s + sel[i][0], (size_t)sel[i][1]), v2_obj(props));
    }
}
static V2 ss_parseCSS(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Str *css; CssIn c; A2Obj *res; int idx = 0; A2Str *k; V2 v;
    if (!SELFO) return v2_undef();
    if (argc < 1 || argv[0].t == T_NULL || argv[0].t == T_UNDEF) return v2_undef();
    css = STR(0); c.s = css->s; c.n = (int)css->len; c.pos = 0;
    res = a2_new_plain(vm);
    if (!css_parse(vm, &c, res)) return v2_undef();   /* silently ignored */
    for (;;) {
        V2 a[2]; a2_dyn_enum(res, &idx, &k, &v); if (!idx) break;
        a[0] = v2_str(k); a[1] = v;
        a2_call_s(vm, self, "setStyle", 2, a);
        if (vm->throwing) break;
    }
    return v2_undef();
}
/* StyleSheet.transform: a style object -> TextFormat */
static double css_parse_int(VM2 *vm, V2 v) { V2 r, a[1]; A2Obj *g = vm->global; a[0] = v; r = a2_call_s(vm, v2_obj(g), "parseInt", 1, a); return v2_tonum(vm, r); }
static double css_parse_float(VM2 *vm, V2 v) { V2 r, a[1]; A2Obj *g = vm->global; a[0] = v; r = a2_call_s(vm, v2_obj(g), "parseFloat", 1, a); return v2_tonum(vm, r); }
static int css_is(VM2 *vm, V2 v, const char *s) { return v.t == T_STR && !strcmp(v.u.s->s, s); (void)vm; }
static V2 ss_transform(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 o = ARG(0), v, r; A2Obj *tf;
    (void)self;
    if (!v2_tobool(vm, o)) return v2_null();
    if (o.t != T_OBJ) {   /* formatObject.color on a sealed primitive */
        char b[200]; A2Class *pc = v2_class_of(vm, o);
        sprintf(b, "Property color not found on %.100s and there is no default value.", pc ? a2_class_name(vm, pc, 0)->s : "Object");
        a2_throw_error(vm, "ReferenceError", b, 1069); return v2_undef();
    }
    r = a2_construct_class(vm, vm->c_textformat, 0, NULL); tf = r.u.o;
    if (r.t != T_OBJ) return v2_null();
#define CSSV(name) (v = a2_get_s(vm, o, name), v)
    if (v2_tobool(vm, CSSV("color"))) {
        A2Str *cs = v2_tostr(vm, v); double col = 0;
        if (cs->s[0] == '#') {
            int n = (int)cs->len - 1, i, ok = 1; u32 x = 0; const char *h = cs->s + 1;
            while (n > 0 && css_ws((u8)h[n - 1])) n--;
            if (n > 6 || n == 0) ok = 0;
            for (i = 0; i < n && ok; i++) { char ch = h[i]; int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1; if (d < 0) ok = 0; else x = x * 16 + (u32)d; }
            if (ok) col = x;
        }
        a2_set_s(vm, r, "color", v2_num(col));
    }
    if (v2_tobool(vm, CSSV("display"))) a2_set_s(vm, r, "display", v);
    if (v2_tobool(vm, CSSV("fontFamily"))) {
        A2Str *fs = v2_tostr(vm, v); const char *p = fs->s; char *out = (char*)malloc(fs->len + 32); size_t on = 0;
        if (out) {
            while (*p) {
                const char *st; size_t n;
                while (*p == ' ') p++;
                st = p; while (*p && *p != ',') p++;
                n = (size_t)(p - st); if (*p == ',') p++;
                if (n == 4 && !strncmp(st, "mono", 4)) { st = "_typewriter"; n = 11; } else if (n == 10 && !strncmp(st, "sans-serif", 10)) { st = "_sans"; n = 5; } else if (n == 5 && !strncmp(st, "serif", 5)) { st = "_serif"; n = 6; }
                if (n) { if (on) out[on++] = ','; if (on + n + 1 > fs->len + 31) break; memcpy(out + on, st, n); on += n; }
            }
            out[on] = 0; a2_set_s(vm, r, "font", v2_cstr(vm, out)); free(out);
        }
    }
    if (v2_tobool(vm, CSSV("fontSize"))) { double sz = css_parse_int(vm, v); if (sz > 0) a2_set_s(vm, r, "size", v2_num(sz)); }
    CSSV("fontStyle"); if (css_is(vm, v, "italic")) a2_set_s(vm, r, "italic", v2_bool(1)); else if (css_is(vm, v, "normal")) a2_set_s(vm, r, "italic", v2_bool(0));
    CSSV("fontWeight"); if (css_is(vm, v, "bold")) a2_set_s(vm, r, "bold", v2_bool(1)); else if (css_is(vm, v, "normal")) a2_set_s(vm, r, "bold", v2_bool(0));
    CSSV("kerning"); if (css_is(vm, v, "true")) a2_set_s(vm, r, "kerning", v2_bool(1)); else if (css_is(vm, v, "false")) a2_set_s(vm, r, "kerning", v2_bool(0)); else a2_set_s(vm, r, "kerning", v2_num(css_parse_int(vm, v)));
    if (v2_tobool(vm, CSSV("leading"))) a2_set_s(vm, r, "leading", v2_num(css_parse_int(vm, v)));
    if (v2_tobool(vm, CSSV("letterSpacing"))) a2_set_s(vm, r, "letterSpacing", v2_num(css_parse_float(vm, v)));
    if (v2_tobool(vm, CSSV("marginLeft"))) a2_set_s(vm, r, "leftMargin", v2_num(css_parse_float(vm, v)));
    if (v2_tobool(vm, CSSV("marginRight"))) a2_set_s(vm, r, "rightMargin", v2_num(css_parse_float(vm, v)));
    if (v2_tobool(vm, CSSV("textAlign"))) a2_set_s(vm, r, "align", v);
    CSSV("textDecoration"); if (css_is(vm, v, "underline")) a2_set_s(vm, r, "underline", v2_bool(1)); else if (css_is(vm, v, "none")) a2_set_s(vm, r, "underline", v2_bool(0));
    if (v2_tobool(vm, CSSV("textIndent"))) a2_set_s(vm, r, "indent", v2_num(css_parse_int(vm, v)));
#undef CSSV
    (void)tf;
    return r;
}
static V2 ss_styleNames(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *arr = a2_new_array(vm), *t; int idx = 0; A2Str *k; V2 v; UNUSED; if (!SELFO) return v2_obj(arr); t = ss_table(vm, SELFO); for (;;) { a2_dyn_enum(t, &idx, &k, &v); if (!idx) break; a2_arr_push(vm, arr, v2_str(k)); } return v2_obj(arr); }

static V2 TextRun_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { if (SELFO) { a2_dyn_set(vm, SELFO, a2_str(vm, "beginIndex"), v2_int(argc > 0 ? v2_toint(vm, ARG(0)) : 0)); a2_dyn_set(vm, SELFO, a2_str(vm, "endIndex"), v2_int(argc > 1 ? v2_toint(vm, ARG(1)) : 0)); a2_dyn_set(vm, SELFO, a2_str(vm, "textFormat"), argc > 2 ? ARG(2) : v2_null()); } return v2_undef(); }
void a2_flash_text_init(VM2 *vm) {
    A2Class *c;
    vm->c_textfield = c = a2_def_class(vm, "flash.text", "TextField", vm->c_interactive, OK_DISPLAY, TextField_ctor);
    a2_add_getter(vm, c, "text", tf_text_get, tf_text_set); a2_add_getter(vm, c, "htmlText", tf_htmlText_get, tf_htmlText_set); a2_add_getter(vm, c, "width", tf_width_get, tf_width_set); a2_add_getter(vm, c, "height", tf_height_get, tf_height_set);
    a2_add_getter(vm, c, "textColor", tf_textColor_get, tf_textColor_set); a2_add_getter(vm, c, "wordWrap", tf_wordWrap_get, tf_wordWrap_set); a2_add_getter(vm, c, "multiline", tf_multiline_get, tf_multiline_set); a2_add_getter(vm, c, "border", tf_border_get, tf_border_set);
    a2_add_getter(vm, c, "background", tf_background_get, tf_background_set); a2_add_getter(vm, c, "backgroundColor", tf_backgroundColor_get, tf_backgroundColor_set); a2_add_getter(vm, c, "borderColor", tf_borderColor_get, tf_borderColor_set); a2_add_getter(vm, c, "selectable", tf_selectable_get, tf_selectable_set);
    a2_add_getter(vm, c, "displayAsPassword", tf_displayAsPassword_get, tf_displayAsPassword_set); a2_add_getter(vm, c, "embedFonts", tf_embedFonts_get, tf_embedFonts_set); a2_add_getter(vm, c, "maxChars", tf_maxChars_get, tf_maxChars_set); a2_add_getter(vm, c, "type", tf_type_get, tf_type_set);
    a2_add_getter(vm, c, "autoSize", tf_autoSize_get, tf_autoSize_set); a2_add_getter(vm, c, "length", tf_length_get, NULL); a2_add_getter(vm, c, "numLines", tf_numLines, NULL); a2_add_getter(vm, c, "textWidth", tf_textWidth, NULL); a2_add_getter(vm, c, "textHeight", tf_textHeight, NULL);
    a2_add_getter(vm, c, "scrollV", tf_scrollV_get, tf_scrollV_set); a2_add_getter(vm, c, "scrollH", tf_scrollH_get, tf_scrollH_set); a2_add_getter(vm, c, "maxScrollV", tf_maxScrollV, NULL); a2_add_getter(vm, c, "maxScrollH", tf_maxScrollH, NULL); a2_add_getter(vm, c, "bottomScrollV", tf_bottomScrollV, NULL);
    a2_add_getter(vm, c, "defaultTextFormat", tf_defaultTextFormat_get, tf_defaultTextFormat_set); a2_add_getter(vm, c, "caretIndex", tf_caretIndex, NULL); a2_add_getter(vm, c, "selectedText", tf_selectedText, NULL); a2_add_getter(vm, c, "selectionBeginIndex", tf_selectionBeginIndex, NULL); a2_add_getter(vm, c, "selectionEndIndex", tf_selectionEndIndex, NULL);
    a2_add_getter(vm, c, "restrict", tf_restrict_get, tf_restrict_set); a2_add_getter(vm, c, "styleSheet", tf_styleSheet_get, tf_styleSheet_set); a2_add_getter(vm, c, "antiAliasType", tf_aa_get, tf_aa_set); a2_add_getter(vm, c, "gridFitType", tf_gridfit_get, tf_gridfit_set);
    a2_add_getter(vm, c, "condenseWhite", tf_condenseWhite_get, tf_condenseWhite_set); a2_add_getter(vm, c, "alwaysShowSelection", tf_alwaysShowSelection_get, tf_alwaysShowSelection_set); a2_add_getter(vm, c, "mouseWheelEnabled", tf_mouseWheelEnabled_get, tf_mouseWheelEnabled_set); a2_add_getter(vm, c, "useRichTextClipboard", tf_useRichTextClipboard_get, tf_useRichTextClipboard_set); a2_add_getter(vm, c, "sharpness", tf_sharpness_get, tf_sharpness_set); a2_add_getter(vm, c, "thickness", tf_thickness_get, tf_thickness_set);
    a2_add_getter(vm, c, "textInteractionMode", tf_antiAliasType_get, NULL);
    a2_add_method(vm, c, "appendText", tf_appendText, 1); a2_add_method(vm, c, "replaceText", tf_replaceText, 3); a2_add_method(vm, c, "replaceSelectedText", tf_replaceSelectedText, 1); a2_add_method(vm, c, "getLineText", tf_getLineText, 1); a2_add_method(vm, c, "getLineLength", tf_getLineLength, 1);
    a2_add_method(vm, c, "getTextFormat", tf_getTextFormat, 2); a2_add_method(vm, c, "getTextRuns", tf_getTextRuns, 2); a2_add_method(vm, c, "setTextFormat", tf_setTextFormat, 3); a2_add_method(vm, c, "getCharBoundaries", tf_getCharBoundaries, 1); a2_add_method(vm, c, "getLineMetrics", tf_getLineMetrics, 1);
    a2_add_method(vm, c, "setSelection", tf_setSelection, 2); a2_add_method(vm, c, "getCharIndexAtPoint", tf_getCharIndexAtPoint, 2); a2_add_method(vm, c, "getLineIndexAtPoint", tf_getLineIndexAtPoint, 2); a2_add_method(vm, c, "getLineIndexOfChar", tf_getLineIndexOfChar, 1); a2_add_method(vm, c, "getLineOffset", tf_getLineOffset, 1); a2_add_method(vm, c, "getFirstCharInParagraph", tf_getFirstCharInParagraph, 1); a2_add_method(vm, c, "getParagraphLength", tf_getParagraphLength, 1); a2_add_method(vm, c, "getImageReference", tf_null, 1);
    a2_add_static(vm, c, "isFontCompatible", tf_isFontCompatible, 2);
    vm->c_textformat = c = a2_def_class(vm, "flash.text", "TextFormat", vm->c_object, OK_PLAIN, TextFormat_ctor); c->is_dynamic = 0;
    a2_add_getter(vm, c, "font", tfm_font_get, tfm_font_set); a2_add_getter(vm, c, "size", tfm_size_get, tfm_size_set); a2_add_getter(vm, c, "color", tfm_color_get, tfm_color_set); a2_add_getter(vm, c, "bold", tfm_bold_get, tfm_bold_set); a2_add_getter(vm, c, "italic", tfm_italic_get, tfm_italic_set); a2_add_getter(vm, c, "underline", tfm_underline_get, tfm_underline_set); a2_add_getter(vm, c, "url", tfm_url_get, tfm_url_set); a2_add_getter(vm, c, "target", tfm_target_get, tfm_target_set); a2_add_getter(vm, c, "align", tfm_align_get, tfm_align_set); a2_add_getter(vm, c, "leftMargin", tfm_leftMargin_get, tfm_leftMargin_set); a2_add_getter(vm, c, "rightMargin", tfm_rightMargin_get, tfm_rightMargin_set); a2_add_getter(vm, c, "indent", tfm_indent_get, tfm_indent_set); a2_add_getter(vm, c, "leading", tfm_leading_get, tfm_leading_set); a2_add_getter(vm, c, "blockIndent", tfm_blockIndent_get, tfm_blockIndent_set); a2_add_getter(vm, c, "bullet", tfm_bullet_get, tfm_bullet_set); a2_add_getter(vm, c, "kerning", tfm_kerning_get, tfm_kerning_set); a2_add_getter(vm, c, "letterSpacing", tfm_letterSpacing_get, tfm_letterSpacing_set); a2_add_getter(vm, c, "tabStops", tfm_tabStops_get, tfm_tabStops_set); a2_add_getter(vm, c, "display", tfm_display_get, tfm_display_set);
    vm->c_statictext = c = a2_def_class(vm, "flash.text", "StaticText", vm->c_displayobject, OK_DISPLAY, NULL); a2_add_getter(vm, c, "text", stext_text, NULL);
    c = a2_def_class(vm, "flash.text", "Font", vm->c_object, OK_PLAIN, Font_ctor); a2_add_static(vm, c, "enumerateFonts", font_enumerateFonts, 1); a2_add_static(vm, c, "registerFont", font_registerFont, 1); a2_add_getter(vm, c, "fontName", font_fontName, NULL); a2_add_getter(vm, c, "fontStyle", font_fontStyle, NULL); a2_add_getter(vm, c, "fontType", font_fontType, NULL); a2_add_method(vm, c, "hasGlyphs", font_hasGlyphs, 1);
    c = a2_def_class(vm, "flash.text", "TextLineMetrics", vm->c_object, OK_PLAIN, tlm_ctor); c->is_dynamic = 1;
    c = a2_def_class(vm, "flash.text", "StyleSheet", vm->c_eventdispatcher, OK_PLAIN, ss_ctor); c->is_dynamic = 1; a2_add_method(vm, c, "setStyle", ss_setStyle, 2); a2_add_method(vm, c, "getStyle", ss_getStyle, 1); a2_add_method(vm, c, "parseCSS", ss_parseCSS, 1); a2_add_method(vm, c, "clear", ss_clear, 0); a2_add_method(vm, c, "transform", ss_transform, 1); a2_add_getter(vm, c, "styleNames", ss_styleNames, NULL);
    c = a2_def_class(vm, "flash.text", "TextFieldType", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "DYNAMIC", "dynamic"); a2_const_str(vm, c, "INPUT", "input");
    c = a2_def_class(vm, "flash.text", "TextFieldAutoSize", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "NONE", "none"); a2_const_str(vm, c, "LEFT", "left"); a2_const_str(vm, c, "CENTER", "center"); a2_const_str(vm, c, "RIGHT", "right");
    c = a2_def_class(vm, "flash.text", "TextFormatAlign", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "LEFT", "left"); a2_const_str(vm, c, "CENTER", "center"); a2_const_str(vm, c, "RIGHT", "right"); a2_const_str(vm, c, "JUSTIFY", "justify"); a2_const_str(vm, c, "START", "start"); a2_const_str(vm, c, "END", "end");
    c = a2_def_class(vm, "flash.text", "AntiAliasType", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "NORMAL", "normal"); a2_const_str(vm, c, "ADVANCED", "advanced");
    c = a2_def_class(vm, "flash.text", "GridFitType", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "NONE", "none"); a2_const_str(vm, c, "PIXEL", "pixel"); a2_const_str(vm, c, "SUBPIXEL", "subpixel");
    c = a2_def_class(vm, "flash.text", "FontStyle", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "REGULAR", "regular"); a2_const_str(vm, c, "BOLD", "bold"); a2_const_str(vm, c, "ITALIC", "italic"); a2_const_str(vm, c, "BOLD_ITALIC", "boldItalic");
    c = a2_def_class(vm, "flash.text", "FontType", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "EMBEDDED", "embedded"); a2_const_str(vm, c, "EMBEDDED_CFF", "embeddedCFF"); a2_const_str(vm, c, "DEVICE", "device");
    c = a2_def_class(vm, "flash.text", "TextInteractionMode", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "NORMAL", "normal"); a2_const_str(vm, c, "SELECTION", "selection");
    c = a2_def_class(vm, "flash.text", "TextDisplayMode", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "DEFAULT", "default"); a2_const_str(vm, c, "CRT", "crt"); a2_const_str(vm, c, "LCD", "lcd");
    c = a2_def_class(vm, "flash.text", "TextColorType", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "DARK_COLOR", "dark"); a2_const_str(vm, c, "LIGHT_COLOR", "light");
    c = a2_def_class(vm, "flash.text", "TextRenderer", vm->c_object, OK_PLAIN, NULL); a2_add_static(vm, c, "setAdvancedAntiAliasingTable", tf_noop, 4); a2_static_getter(vm, c, "maxLevel", tf_zero, tf_noop); a2_static_getter(vm, c, "displayMode", tf_antiAliasType_get, tf_noop);
    c = a2_def_class(vm, "flash.text", "TextSnapshot", vm->c_object, OK_PLAIN, NULL); a2_add_method(vm, c, "getText", tf_null, 3); a2_add_getter(vm, c, "charCount", tf_zero, NULL);
    c = a2_def_class(vm, "flash.text", "CSMSettings", vm->c_object, OK_PLAIN, NULL); c->is_dynamic = 1;
    c = a2_def_class(vm, "flash.text", "TextRun", vm->c_object, OK_PLAIN, TextRun_ctor); c->is_dynamic = 1;
    c = a2_def_class(vm, "flash.text", "TextExtent", vm->c_object, OK_PLAIN, NULL); c->is_dynamic = 1;
    c = a2_def_class(vm, "flash.text", "FontAsset", vm->c_object, OK_PLAIN, NULL); c->is_dynamic = 1;
    /* flash.text.engine: ActionScript classes (as3/) with natives in avm2_fte.c */
}

/* flash.text.engine.TextLine: the line's text is laid out by the object's own
 * text field state (Ruffle's "fallback" EditText) */
void a2_fte_line_setup(VM2 *vm, A2Obj *o, A2Str *text, const RTFmt *f, double width) {
    TextFieldState *t = tfs_of(vm, o); int n; u32 *u = to_field_text(text, &n);
    rt_fmt_apply(&t->deffmt, f);
    rt_set_plain(&t->rt, u, n, &t->deffmt); free(u);
    free(t->html_source); t->html_source = NULL;
    t->wordwrap = 1; t->selectable = 0; t->input = 0;
    t->w = (int)floor(width * 20 + 0.5); t->h = 15 * 20;
    changed(vm, t);
    tfs_layout(vm->player, t);
    t->h = (int)t->lay.text_h;
    changed(vm, t);
}
void a2_fte_line_measure(VM2 *vm, A2Obj *o, double *tw, double *th, double *asc, double *desc) {
    TextFieldState *t = a2_disp(o)->tfs;
    *tw = *th = *asc = *desc = 0;
    if (!t) return;
    tfs_layout(vm->player, t);
    *tw = tw_px(t->lay.text_w); *th = tw_px(t->lay.text_h);
    if (t->lay.nlines > 0) { double m[6]; tfs_line_metrics(vm->player, t, 0, m); *asc = m[3]; *desc = m[4]; }
}
