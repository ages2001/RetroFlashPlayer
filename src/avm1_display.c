/* avm1_display.c - AVM1 stage objects: the AS objects of movie clips,
 * buttons and text fields, paths, movie clip references and the
 * display properties (_x, _y, _xscale ...). */
#include "avm1_int.h"
#include "devfont.h"
#include "textfield.h"
#include "shape.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define PI 3.14159265358979323846

extern void dg_local_bounds(MovieClip *c, SWFRect *out, int *any);
extern int dg_dobj_bounds(DisplayObject *d, const SWFMatrix *m, SWFRect *out);
extern int dg_char_bounds(SWFChar *ch, SWFRect *out);
extern void dg_xform(const SWFMatrix *m, s32 x, s32 y, s32 *ox, s32 *oy);
extern void clip_world_matrix(MovieClip *c, SWFMatrix *out);

/* ---------------------------------------------------------------- */
/* per-instance state                                               */

A1Clip *a1_of(MovieClip *c) {
    if (!c) return NULL;
    if (!c->a1) {
        c->a1 = (A1Clip*)calloc(1, sizeof(A1Clip));
        if (c->a1) { c->a1->focus_rect = -1; c->a1->enabled_cache = 1; c->a1->use_hand_cursor = 1; c->a1->tab_index = 0; c->a1->has_tab = 0; }
    }
    return c->a1;
}

MovieClip *a1_clip_masker(MovieClip *c) { return c && c->a1 && c->a1->masker && !c->a1->masker->dead ? c->a1->masker : NULL; }
int a1_clip_is_mask(MovieClip *c) { return c && c->a1 && c->a1->maskee != NULL; }
ShapeDef *a1_clip_drawing(MovieClip *c) { return c && c->a1 ? (ShapeDef*)c->a1->drawing : NULL; }
struct TextFieldState *a1_clip_textfield(MovieClip *c) { return c && c->a1 ? c->a1->tfs : NULL; }

/* ---------------------------------------------------------------- */
/* clips <-> objects                                                */

int clip_is_movieclip(MovieClip *c) {
    if (!c) return 0;
    if (c->a1 && c->a1->kind != 0) return 0;
    if (c->self && c->self->ch && (c->self->ch->type == CHAR_BUTTON || c->self->ch->type == CHAR_EDITTEXT)) return 0;
    return 1;
}

int clip_removed(MovieClip *c) { return !c || c->dead || (c->a1 && c->a1->removed); }

int clip_swf_version(MovieClip *c) {
    if (c && c->movie) return c->movie->version;
    return 6;
}

AObj *clip_obj(VM *vm, MovieClip *c) {
    AObj *o; Env *e; AObj *proto;
    if (!c) return NULL;
    if (c->obj) return (AObj*)c->obj;
    if (vm->player->avm2 && c->obj2 && !c->a1) return NULL;
    e = env_get(vm, clip_swf_version(c));
    o = obj_bare(vm);
    if (!o) return NULL;
    if (c->a1 && c->a1->kind == 1) { o->nk = NK_BUTTON; proto = e->p.button; }
    else if (c->a1 && c->a1->kind == 2) { o->nk = NK_EDITTEXT; proto = e->p.textfield; }
    else if (c->a1 && c->a1->kind == 3) { o->nk = NK_VIDEO; proto = e->p.video; }
    else if (c->self && c->self->ch && c->self->ch->type == CHAR_BUTTON) { o->nk = NK_BUTTON; proto = e->p.button; }
    else if (c->self && c->self->ch && c->self->ch->type == CHAR_EDITTEXT) { o->nk = NK_EDITTEXT; proto = e->p.textfield; }
    else { o->nk = NK_MOVIECLIP; proto = e->p.movieclip; }
    o_define(vm, o, vm->s_proto, proto ? v_obj(proto) : v_undef(), PA_DONT_ENUM | PA_DONT_DELETE);
    o->nat.clip = c;
    c->obj = (struct AObj*)o;
    return o;
}

AObj *clip_obj_or_undef_v(VM *vm, MovieClip *c, Value *out) {
    AObj *o = clip_obj(vm, c);
    *out = o ? v_obj(o) : v_undef();
    return o;
}

MovieClip *obj_clip(AObj *o) {
    MovieClip *c = o_display(o);
    return clip_removed(c) ? NULL : c;
}

/* ---------------------------------------------------------------- */
/* hierarchy                                                        */

static int clip_level(MovieClip *c) { return c->self ? c->self->depth : 0; }

MovieClip *clip_avm1_parent(MovieClip *c) {
    if (!c || !c->parent) return NULL;
    if (c->parent->movie && c->parent->movie->has_abc) return NULL;
    return c->parent;
}

MovieClip *clip_avm1_root(MovieClip *c) {
    MovieClip *r = c;
    if (!r) return NULL;
    for (;;) {
        MovieClip *par;
        if (r->a1 && r->a1->lock_root) break;
        par = clip_avm1_parent(r);
        if (!par) break;
        r = par;
    }
    return r;
}

AStr *clip_get_name(VM *vm, MovieClip *c) {
    if (!c->self) return vm->s_empty;
    return str_utf8(vm, c->self->name, strlen(c->self->name));
}

AStr *clip_path(VM *vm, MovieClip *c) {
    MovieClip *par = clip_avm1_parent(c);
    if (par) {
        AStr *p = clip_path(vm, par), *n = clip_get_name(vm, c);
        return str_concat(vm, str_concat(vm, p, str_intern(vm, ".")), n);
    } else {
        char b[32]; sprintf(b, "_level%d", clip_level(c));
        return str_intern(vm, b);
    }
}

static AStr *build_slash(VM *vm, MovieClip *c) {
    MovieClip *par = clip_avm1_parent(c);
    if (par) return str_concat(vm, str_concat(vm, build_slash(vm, par), str_intern(vm, "/")), clip_get_name(vm, c));
    if (clip_level(c) == 0) return vm->s_empty;
    { char b[32]; sprintf(b, "_level%d", clip_level(c)); return str_intern(vm, b); }
}

AStr *clip_slash_path(VM *vm, MovieClip *c) {
    if (clip_avm1_parent(c)) return build_slash(vm, c);
    return str_intern(vm, "/");
}

MovieClip *clip_child_by_name(Act *a, MovieClip *c, AStr *name) {
    DisplayObject *d; const char *n = str_c(a->vm, name); int cs = act_case_sensitive(a);
    if (!c) return NULL;
    for (d = c->children; d; d = d->next) {
        if (!d->name[0]) continue;
        if (cs ? strcmp(d->name, n) == 0 : str_eq_ci(str_utf8(a->vm, d->name, strlen(d->name)), name)) {
            if (d->clip) return d->clip;
            return c;   /* shapes etc. resolve to their parent */
        }
    }
    return NULL;
}

int clip_frame_label(MovieClip *c, AStr *label, int case_sensitive) {
    extern int clip_find_frame_label(MovieClip *c, const char *label);
    extern int clip_find_frame_label_ci(MovieClip *c, const char *label);
    VM *vm = VMOF(c->player);
    if (!c->tl) return -1;
    (void)case_sensitive;
    return clip_find_frame_label_ci(c, str_c(vm, label));
}

/* ---------------------------------------------------------------- */
/* movie clip references                                            */

ClipRef *clipref_from_path(VM *vm, AStr *path) {
    ClipRef *r = (ClipRef*)gc_alloc(vm, sizeof(ClipRef), GC_REF); u32 i, start; int nseg = 0;
    if (!r) return NULL;
    r->path = path;
    for (i = 0; i < path->len; i++) if (STR_AT(path, i) == '.') nseg++;
    r->seg = (AStr**)malloc((size_t)(nseg + 1) * sizeof(AStr*));
    r->nseg = 0;
    start = 0;
    for (i = 0; i <= path->len; i++) {
        if (i == path->len || STR_AT(path, i) == '.') {
            AStr *part = str_slice(vm, path, start, i);
            if (start == 0) {
                if (str_starts_c(part, "_level")) {
                    double d = str_parse_float(str_slice(vm, part, 6, part->len), 1);
                    r->level = (d == d) ? (int)d : 0;
                } else if (r->seg) r->seg[r->nseg++] = part;
            } else if (r->seg) r->seg[r->nseg++] = part;
            start = i + 1;
        }
    }
    return r;
}

static MovieClip *swf5_ref_target(Act *a, MovieClip *d) {
    if (a->swf_version <= 5) while (d && !clip_is_movieclip(d)) d = clip_avm1_parent(d);
    return d;
}

Value clipref_value(Act *a, AObj *o) {
    VM *vm = a->vm; MovieClip *d = o_display_no_super(o); ClipRef *r;
    if (!d) return v_obj(o);
    if (!clip_is_movieclip(d)) {
        /* SWF 6 and later: a text field is a character reference too
           (Gnash TextField.as: a dangling one reads as a movieclip, then rebinds) */
        if (a->swf_version > 5) { if (!(d->a1 && d->a1->kind == 2)) return v_obj(o); }
        else {
            d = swf5_ref_target(a, d);
            if (!d) return v_obj(o);
        }
    }
    if (!d->obj) return v_obj(o);
    /* a reference that loses its clip looks for the path the clip had when
       its object was made (Gnash MovieClip.as: sr63_bis after renames) */
    r = clipref_from_path(vm, d->a1 && d->a1->orig_path ? d->a1->orig_path : clip_path(vm, d));
    if (!r) return v_obj(o);
    r->cached = (AObj*)d->obj;
    {
        Value v; v.t = V_CLIP; v.u.c = r; return v;
    }
}

AObj *clipref_resolve(Act *a, ClipRef *r, int *cached, MovieClip **dobj) {
    VM *vm = a->vm; MovieClip *start; int i;
    if (r->cached) {
        MovieClip *d = o_display_no_super(r->cached);
        if (d && !clip_removed(d)) {
            d = swf5_ref_target(a, d);
            if (!d) return NULL;
            if (cached) *cached = 1;
            if (dobj) *dobj = d;
            return r->cached;
        }
    }
    r->cached = NULL;
    start = act_get_or_create_level(a, r->level);
    for (i = 0; i < r->nseg && start; i++) {
        MovieClip *c = clip_child_by_name(a, start, r->seg[i]);
        if (c == start) c = NULL;
        start = c;
    }
    if (!start) return NULL;
    start = swf5_ref_target(a, start);
    if (!start) return NULL;
    if (cached) *cached = 0;
    if (dobj) *dobj = start;
    {
        AObj *o = clip_obj(vm, start);
        return o ? o : obj_bare(vm);
    }
}

AStr *clipref_string(Act *a, ClipRef *r) {
    int cached = 0; MovieClip *d = NULL;
    AObj *o = clipref_resolve(a, r, &cached, &d);
    if (!o) return a->vm->s_empty;
    if (cached && d) return clip_path(a->vm, d);
    return r->path;
}

/* ---------------------------------------------------------------- */
/* scale / rotation cache                                           */

static double f32(double v) { volatile float f = (float)v; return (double)f; }

static void cache_sr(DisplayObject *d) {
    if (!d->sr_cached) {
        double a = f32(d->matrix.sx), b = f32(d->matrix.r0), c = f32(d->matrix.r1), dd = f32(d->matrix.sy);
        double rx = atan2(b, a), ry = atan2(-c, dd);
        d->sr_rot = rx * (180.0 / PI);   /* f64::to_degrees */
        d->sr_xs = sqrt(a * a + b * b) * 100.0;   /* scales are kept as percentages (Ruffle's Percent) */
        d->sr_ys = sqrt(c * c + dd * dd) * 100.0;
        d->sr_skew = ry - rx;
        d->sr_cached = 1;
    }
}

/* cosine and sine of an angle in degrees (plus a skew in radians): right
   angles give exact values (Gnash MovieClip.as: _rotation -90 leaves a == 0) */
static void deg_trig(double degrees, double skew, double *c, double *s) {
    double r = degrees * (PI / 180.0);      /* f64::to_radians */
    double total = degrees + skew * (180.0 / PI), q90 = floor(total / 90.0 + 0.5);
    if (total == total && total > -1e15 && total < 1e15 && fabs(total - q90 * 90.0) < 1e-9) {
        long q = ((long)q90) % 4; if (q < 0) q += 4;
        *c = q == 0 ? 1.0 : q == 2 ? -1.0 : 0.0;
        *s = q == 1 ? 1.0 : q == 3 ? -1.0 : 0.0;
        return;
    }
    *c = cos(r + skew); *s = sin(r + skew);
}

static void set_rotation(DisplayObject *d, double degrees) {
    double r, cx, sx, cy, sy;
    cache_sr(d);
    d->sr_rot = degrees;
    r = degrees * (PI / 180.0);      /* f64::to_radians */
    if (r != r) return;
    deg_trig(degrees, 0.0, &cx, &sx); deg_trig(degrees, d->sr_skew, &cy, &sy);
    d->matrix.sx = f32(d->sr_xs / 100.0 * cx); d->matrix.r0 = f32(d->sr_xs / 100.0 * sx);
    d->matrix.r1 = f32(d->sr_ys / 100.0 * -sy); d->matrix.sy = f32(d->sr_ys / 100.0 * cy);
}

/* a matrix component Flash stores as 16.16 fixed point: out of range, Flash 9 wraps
   the 32-bit value and later players get the x86 overflow value (-32768) */
static double fx_range(double v) {
    double f = v * 65536.0;
    if (f > -2147483648.0 && f < 2147483648.0) return v;
    if (avm1_player_major() <= 9) { double w; if (f != f || f > 1e300 || f < -1e300) return 0.0; f = f < 0 ? ceil(f) : floor(f); w = fmod(f, 4294967296.0); if (w < 0) w += 4294967296.0; if (w >= 2147483648.0) w -= 4294967296.0; return w / 65536.0; }
    return -32768.0;
}
static void set_scale_x(DisplayObject *d, double percent) {
    double unit, rot;
    d->script_moved = 1;
    cache_sr(d);
    d->sr_xs = percent;
    unit = percent != percent ? 0.0 : percent / 100.0;
    rot = d->sr_rot * (PI / 180.0);
    if (rot != rot) rot = 0.0;
    { double c, sn; if (rot == 0.0) { c = 1.0; sn = 0.0; } else deg_trig(d->sr_rot, 0.0, &c, &sn);
      d->matrix.sx = f32(fx_range(c * unit)); d->matrix.r0 = f32(fx_range(sn * unit)); }
}

static void set_scale_y(DisplayObject *d, double percent) {
    double unit, rot;
    d->script_moved = 1;
    cache_sr(d);
    d->sr_ys = percent;
    unit = percent != percent ? 0.0 : percent / 100.0;
    rot = d->sr_rot * (PI / 180.0);
    if (rot != rot) rot = 0.0;
    { double c, sn; deg_trig(rot == 0.0 ? 0.0 : d->sr_rot, d->sr_skew, &c, &sn);
      d->matrix.r1 = f32(fx_range(-sn * unit)); d->matrix.sy = f32(fx_range(c * unit)); }
}

/* ---------------------------------------------------------------- */
/* bounds                                                           */

/* the object's own bounds (no matrix), twips */
static int self_bounds(MovieClip *c, SWFRect *out) {
    int any = 0;
    out->xmin = out->ymin = out->xmax = out->ymax = 0;
    dg_local_bounds(c, out, &any);
    return any;
}

static int local_bounds(MovieClip *c, SWFRect *out) {
    SWFRect s; int any = 0;
    if (!self_bounds(c, &s)) { out->xmin = out->ymin = out->xmax = out->ymax = 0; return 0; }
    if (!c->self) { *out = s; return 1; }
    { extern void dg_union(SWFRect *acc, int *any, const SWFRect *r, const SWFMatrix *m); dg_union(out, &any, &s, &c->self->matrix); }
    return 1;
}

static double twips_px(s32 t) { return t / 20.0; }

static double clip_width(MovieClip *c) { SWFRect r; local_bounds(c, &r); return twips_px(r.xmax - r.xmin); }
static double clip_height(MovieClip *c) { SWFRect r; local_bounds(c, &r); return twips_px(r.ymax - r.ymin); }

static void clip_set_wh(MovieClip *c, double value, int height) {
    DisplayObject *d = c->self; SWFRect b; double ow, oh, ar, tx, ty, psx, psy, rot, cs, sn, nsx, nsy;
    if (!d) return;
    self_bounds(c, &b);
    ow = twips_px(b.xmax - b.xmin); oh = twips_px(b.ymax - b.ymin);
    cache_sr(d);
    /* (the scales come back positive: Gnash MovieClip.as "reset to positive on setting _width") */
    psx = fabs(d->sr_xs) / 100.0; psy = fabs(d->sr_ys) / 100.0; rot = d->sr_rot * PI / 180.0;
    cs = fabs(cos(rot)); sn = fabs(sin(rot));
    if (!height) {
        ar = oh / ow;
        if (ow != 0.0) { tx = value / ow; ty = value / oh; } else { tx = ty = 0.0; }
        nsx = ar * (cs * tx + sn * ty) / ((cs + ar * sn) * (ar * cs + sn));
        nsy = (sn * psx + ar * cs * psy) / (ar * cs + sn);
    } else {
        ar = ow / oh;
        if (oh != 0.0) { tx = value / ow; ty = value / oh; } else { tx = ty = 0.0; }
        nsx = (ar * cs * psx + sn * psy) / (ar * cs + sn);
        nsy = ar * (sn * tx + cs * ty) / ((cs + ar * sn) * (ar * cs + sn));
    }
    if (!(nsx == nsx && nsx < 1e308 && nsx > -1e308)) nsx = 0.0;
    if (!(nsy == nsy && nsy < 1e308 && nsy > -1e308)) nsy = 0.0;
    set_scale_x(d, nsx * 100.0);
    set_scale_y(d, nsy * 100.0);
}

/* ---------------------------------------------------------------- */
/* display properties                                               */

typedef int (*DPGet)(Act *a, MovieClip *c, Value *out);
typedef int (*DPSet)(Act *a, MovieClip *c, Value v);

/* NaN / undefined / null leave the property alone */
static int prop_num(Act *a, Value v, double *out) {
    double n;
    if (v.t == V_UNDEF || v.t == V_NULL) return 0;
    if (a->swf_version < 5) {   /* SWF4: strings must be whole numbers; NaN and infinities are ignored */
        if (v.t == V_OBJ || v.t == V_CLIP) return 0;
        n = v.t == V_STR ? str_parse_float(v.u.s, 1) : v_num_of(a, v);
        if (a->vm->err || n != n || n > 1e308 || n < -1e308) return 0;
        *out = n;
        return 1;
    }
    if (v.t == V_OBJ && avm1_player_major() == 19) {
        /* Flash Player 19 refused objects here (a debugger warning; 20 took it back) */
        avm1_trace_out("This method no longer accepts Objects implementing valueOf() or toString() as values.");
        return 0;
    }
    n = v_num_of(a, v);
    if (a->vm->err || n != n) return 0;
    *out = n;
    return 1;
}

/* pixels -> twips as Flash's 32-bit conversion: out of range, Flash 9 wraps and later
   players get the x86 overflow value (INT_MIN) */
static double to_twips_d(double px) {
    double t = px * 20.0;
    if (t != t) return 0;
    t = t < 0 ? ceil(t) : floor(t);
    if (t > -2147483649.0 && t < 2147483648.0) return t;
    if (avm1_player_major() <= 9) { double w; if (t > 1e300 || t < -1e300) return 0; w = fmod(t, 4294967296.0); if (w < 0) w += 4294967296.0; if (w >= 2147483648.0) w -= 4294967296.0; return w; }
    return -2147483648.0;
}

extern int avm1_mouse_x(Player *p), avm1_mouse_y(Player *p);
extern int avm1_quality(Player *p);     /* 0 low 1 medium 2 high 3 best */
extern void avm1_set_quality(Player *p, int q);

/* a text field's position includes its bounds' offset (EditText::x) */
static s32 tf_offset(MovieClip *c, int vert) {
    struct TextFieldState *t = c->a1 ? c->a1->tfs : NULL; double sc, px;
    if (!t || !c->self) return 0;
    tfs_apply_autosize(c->player, t);
    cache_sr(c->self);
    sc = (vert ? c->self->sr_ys : c->self->sr_xs) / 100.0;
    px = sc * ((vert ? t->ymin : t->xmin) / 20.0) * 20.0;
    if (px != px) return 0;
    if (px >= 2147483647.0) return 2147483647; if (px <= -2147483648.0) return (s32)0x80000000;
    return (s32)px;
}
static int g_x(Act *a, MovieClip *c, Value *o) { (void)a; *o = v_num(c->self ? (double)(s32)((u32)(s32)c->self->matrix.tx + (u32)tf_offset(c, 0)) / 20.0 : 0); return 1; }
static int g_y(Act *a, MovieClip *c, Value *o) { (void)a; *o = v_num(c->self ? (double)(s32)((u32)(s32)c->self->matrix.ty + (u32)tf_offset(c, 1)) / 20.0 : 0); return 1; }
static int s_x(Act *a, MovieClip *c, Value v) { double x; if (prop_num(a, v, &x) && c->self) { c->self->script_moved = 1; if (x > 1e308 || x < -1e308) x = avm1_player_major() <= 9 ? 0.0 : -1.0 / 0.0;   /* Flash 9: infinities place at 0 */ c->self->matrix.tx = (double)(s32)((u32)(s32)to_twips_d(x) - (u32)tf_offset(c, 0)); } return 1; }
static int s_y(Act *a, MovieClip *c, Value v) { double y; if (prop_num(a, v, &y) && c->self) { c->self->script_moved = 1; if (y > 1e308 || y < -1e308) y = avm1_player_major() <= 9 ? 0.0 : -1.0 / 0.0; c->self->matrix.ty = (double)(s32)((u32)(s32)to_twips_d(y) - (u32)tf_offset(c, 1)); } return 1; }
/* SWF4: the scale comes from the 16.16 fixed point matrix (magnitude, truncated) */
static double swf4_scale(double a, double b) { return floor(sqrt(a * a + b * b) * 65536.0) / 65536.0 * 100.0; }
static int g_xscale(Act *a, MovieClip *c, Value *o) { if (!c->self) { *o = v_num(100); return 1; } if (a->swf_version < 5) { *o = v_num(swf4_scale(c->self->matrix.sx, c->self->matrix.r0)); return 1; } cache_sr(c->self); *o = v_num(c->self->sr_xs); return 1; }
static int g_yscale(Act *a, MovieClip *c, Value *o) { if (!c->self) { *o = v_num(100); return 1; } if (a->swf_version < 5) { *o = v_num(swf4_scale(c->self->matrix.r1, c->self->matrix.sy)); return 1; } cache_sr(c->self); *o = v_num(c->self->sr_ys); return 1; }
static int s_xscale(Act *a, MovieClip *c, Value v) { double x; if (prop_num(a, v, &x) && c->self) set_scale_x(c->self, x); return 1; }
static int s_yscale(Act *a, MovieClip *c, Value v) { double x; if (prop_num(a, v, &x) && c->self) set_scale_y(c->self, x); return 1; }
static int g_currentframe(Act *a, MovieClip *c, Value *o) { (void)a; if (!clip_is_movieclip(c)) { *o = v_undef(); return 1; } *o = v_num(c->cur_frame + 1); return 1; }
/* a sprite reports its DefineSprite header's frame count (even without ShowFrame tags) */
static int sprite_frames(MovieClip *c) { return c->tl && c->tl->has_hdr && c->movie && c->tl != &c->movie->root ? c->tl->hdr_frames : c->frame_count; }
static int g_totalframes(Act *a, MovieClip *c, Value *o) { (void)a; if (!clip_is_movieclip(c)) { *o = v_undef(); return 1; } *o = v_num(c->tl ? sprite_frames(c) : 1); return 1; }
static int g_alpha(Act *a, MovieClip *c, Value *o) { (void)a; *o = v_num(c->self ? c->self->cxform.ma / 256.0 * 100.0 : 100.0); return 1; }
static int s_alpha(Act *a, MovieClip *c, Value v) {
    double x;
    if (prop_num(a, v, &x) && c->self) {
        c->self->script_moved = 1;
        double f;
        if (x > 1e308 || x < -1e308) x = 0;
        f = x / 100.0 * 256.0;
        if (f >= 32767.0) f = 32767.0; else if (f <= -32768.0) f = -32768.0;
        c->self->cxform.ma = (s16)(f < 0 ? ceil(f) : floor(f));
    }
    return 1;
}
static int g_visible(Act *a, MovieClip *c, Value *o) { (void)a; *o = v_bool(c->self ? c->self->visible : 1); return 1; }
/* hiding a visible object takes the focus away (an already hidden one keeps it) */
static int s_visible(Act *a, MovieClip *c, Value v) { double x; if (prop_num(a, v, &x) && c->self) { int was = c->self->visible; c->self->visible = x != 0.0; if (was && !c->self->visible && c->player->focus == c) player_set_focus(c->player, NULL); } return 1; }
static int g_width(Act *a, MovieClip *c, Value *o) { (void)a; *o = v_num(clip_width(c)); return 1; }
static int g_height(Act *a, MovieClip *c, Value *o) { (void)a; *o = v_num(clip_height(c)); return 1; }
static int s_width(Act *a, MovieClip *c, Value v) {
    double x;
    if (prop_num(a, v, &x)) {
        struct TextFieldState *t = c->a1 ? c->a1->tfs : NULL;
        if (t) {
            if (x < 0) x = 0;
            tfs_apply_autosize(c->player, t); t->w = (int)to_twips_d(x); t->box_flip &= ~1;
            /* Linux player, SWF7 and older: the text keeps its layout until it changes */
            if (devfont_linux_rules() && t->swf_version < 8) t->as_pending = 1; else tfs_invalidate(t);
        } else clip_set_wh(c, x, 0);
    }
    return 1;
}
static int s_height(Act *a, MovieClip *c, Value v) { double x; if (prop_num(a, v, &x)) { struct TextFieldState *t = c->a1 ? c->a1->tfs : NULL; if (t) { if (x < 0) x = 0; tfs_apply_autosize(c->player, t); t->h = (int)to_twips_d(x); t->box_flip &= ~2; tfs_invalidate(t); } else clip_set_wh(c, x, 1); } return 1; }
static int g_rotation(Act *a, MovieClip *c, Value *o) {
    if (!c->self) { *o = v_num(0); return 1; }
    if (a->swf_version < 5) {   /* SWF4: from the matrix; a zero x scale reads as +-90 */
        double sx = c->self->matrix.sx, r0 = c->self->matrix.r0;
        *o = v_num(sx == 0 ? (r0 < 0 ? -90.0 : 90.0) : atan2(r0, sx) * 180.0 / PI);
        return 1;
    }
    cache_sr(c->self); *o = v_num(c->self->sr_rot); return 1; }
static int s_rotation(Act *a, MovieClip *c, Value v) {
    double deg;
    if (prop_num(a, v, &deg) && c->self) {
        c->self->script_moved = 1;
        deg = fmod(deg, 360.0);
        if (deg < -180.0) deg += 360.0; else if (deg > 180.0) deg -= 360.0;
        set_rotation(c->self, deg);
    }
    return 1;
}
static int g_target(Act *a, MovieClip *c, Value *o) { *o = v_str(clip_slash_path(a->vm, c)); return 1; }
static int g_framesloaded(Act *a, MovieClip *c, Value *o) { (void)a; if (!clip_is_movieclip(c)) { *o = v_undef(); return 1; } if (c->a1 && c->a1->has_fl) { *o = v_num(c->a1->fl); return 1; } *o = v_num(c->tl ? sprite_frames(c) : 1); return 1; }
static int g_name(Act *a, MovieClip *c, Value *o) { *o = v_str(clip_get_name(a->vm, c)); return 1; }
static int s_name(Act *a, MovieClip *c, Value v) {
    AStr *n;
    if (v.t == V_NUM && v.u.n != v.u.n && a->swf_version >= 7) v = v_num(0);
    n = v.t == V_BOOL ? str_intern(a->vm, v.u.b ? "true" : "false") : v_str_of(a, v);   /* (even in SWF4) */
    if (a->vm->err) return 1;
    if (c->self) { strncpy(c->self->name, str_c(a->vm, n), sizeof(c->self->name) - 1); c->self->name[sizeof(c->self->name) - 1] = 0; }
    return 1;
}
static int g_droptarget(Act *a, MovieClip *c, Value *o) {
    extern MovieClip *avm1_drop_target(MovieClip *c);
    MovieClip *t = clip_is_movieclip(c) ? avm1_drop_target(c) : NULL;
    if (t) *o = v_str(clip_slash_path(a->vm, t));
    else *o = v_str(a->vm->s_empty);      /* Flash: "" in SWF5 movies too */
    return 1;
}
static int g_url(Act *a, MovieClip *c, Value *o) {
    extern const char *avm1_movie_url(Player *p, SWFMovie *m);
    /* (a text field has the URL of the movie that made it: Gnash TextField.as) */
    if (clip_is_movieclip(c) || (c->a1 && c->a1->kind == 2)) *o = v_cstr(a->vm, avm1_movie_url(c->player, c->movie));
    else *o = v_str(a->vm->s_empty);
    return 1;
}
static int g_highquality(Act *a, MovieClip *c, Value *o) { int q = avm1_quality(c->player); (void)a; *o = v_num(q == 3 ? 2 : q == 2 ? 1 : 0); return 1; }
static int s_highquality(Act *a, MovieClip *c, Value v) {
    double x;
    if (v.t == V_NULL || v.t == V_UNDEF) return 1;
    if (a->swf_version < 5) { if (!prop_num(a, v, &x)) return 1; }
    else x = v_num_of(a, v);
    if (a->vm->err || x != x) return 1;
    avm1_set_quality(c->player, x > 1.5 ? 3 : x == 0.0 ? 0 : 2);
    return 1;
}
extern int avm1_stage_focus_rect(Player *p);
extern void avm1_set_stage_focus_rect(Player *p, int v);
static int refers_stage_focus(Act *a, MovieClip *c) { return a->swf_version <= 5 || !c->parent; }
static int g_focusrect(Act *a, MovieClip *c, Value *o) {
    if (refers_stage_focus(a, c)) {
        int v = avm1_stage_focus_rect(c->player);
        *o = a->swf_version <= 5 ? v_num(v ? 1 : 0) : v_bool(v);
    } else {
        A1Clip *s = a1_of(c);
        *o = !s || s->focus_rect < 0 ? v_null() : v_bool(s->focus_rect);
    }
    return 1;
}
static int s_focusrect(Act *a, MovieClip *c, Value v) {
    if (refers_stage_focus(a, c)) {
        double x;
        if (v.t == V_UNDEF || v.t == V_NULL) return 1;
        if (a->swf_version < 5) { if (!prop_num(a, v, &x)) return 1; }
        else x = (v.t == V_OBJ || v.t == V_CLIP) ? 0.0 : v_num_of(a, v);
        if (a->vm->err || x != x) return 1;
        avm1_set_stage_focus_rect(c->player, x != 0.0);
    } else {
        A1Clip *s = a1_of(c);
        if (s) s->focus_rect = (v.t == V_UNDEF || v.t == V_NULL) ? -1 : v_bool_of(a, v);
    }
    return 1;
}
extern int avm1_sound_buf_time(Player *p);
extern void avm1_set_sound_buf_time(Player *p, int v);
static int g_soundbuftime(Act *a, MovieClip *c, Value *o) { (void)a; *o = v_num(avm1_sound_buf_time(c->player)); return 1; }
static int s_soundbuftime(Act *a, MovieClip *c, Value v) {
    double x;
    if (v.t == V_UNDEF || v.t == V_NULL) return 1;
    if (a->swf_version < 5 || (v.t == V_OBJ && avm1_player_major() == 19)) { if (!prop_num(a, v, &x)) return 1; }
    else x = v_num_of(a, v);
    if (a->vm->err || x != x) return 1;
    /* x86 cvttsd2si: out-of-range values (incl. +-Infinity) become INT_MIN */
    avm1_set_sound_buf_time(c->player, (x >= 2147483648.0 || x < -2147483648.0) ? (-2147483647 - 1) : (int)x);
    return 1;
}
static int g_quality(Act *a, MovieClip *c, Value *o) {
    static const char *q[4] = { "LOW", "MEDIUM", "HIGH", "BEST" };
    int v = avm1_quality(c->player);
    *o = v_str(str_intern(a->vm, q[v < 0 || v > 3 ? 2 : v]));
    return 1;
}
static int s_quality(Act *a, MovieClip *c, Value v) {
    AStr *s = v_str_of(a, v);
    if (a->vm->err) return 1;
    if (str_eq_c_ci(s, "low")) avm1_set_quality(c->player, 0);
    else if (str_eq_c_ci(s, "medium")) avm1_set_quality(c->player, 1);
    else if (str_eq_c_ci(s, "high")) avm1_set_quality(c->player, 2);
    else if (str_eq_c_ci(s, "best")) avm1_set_quality(c->player, 3);
    return 1;
}
/* DisplayObject::local_mouse_position (device pixels through the inverse) */
static void local_mouse(MovieClip *c, double *x, double *y) {
    Player *p = c->player; RMat l2g, t2d, m, inv; s32 gx, gy, lx, ly; double z = p->zoom > 0 ? p->zoom : 1.0;
    float zf = (float)z;
    if (p->has_view) {
        /* device pixels through the letterbox matrix (Stage::build_matrices) */
        extern void avm1_view_matrix(Player *p, double *sx, double *sy, double *tx, double *ty);
        double vsx, vsy, vtx, vty; RMat v;
        avm1_view_matrix(p, &vsx, &vsy, &vtx, &vty);
        gx = (s32)floor(p->mouse_x + 0.5); gy = (s32)floor(p->mouse_y + 0.5);
        v.a = (float)vsx * 0.05f; v.d = (float)vsy * 0.05f; v.b = v.c = 0;
        v.tx = (s32)floor(vtx - p->movie->stage.xmin * vsx * 0.05 + 0.5); v.ty = (s32)floor(vty - p->movie->stage.ymin * vsy * 0.05 + 0.5);
        t2d = v;
    } else {
    /* global device pixels, stored as twips values */
    gx = (s32)floor(p->mouse_x * 20.0 / z + 0.5); gy = (s32)floor(p->mouse_y * 20.0 / z + 0.5);
    { RMat v; v.a = zf * 0.05f; v.d = zf * 0.05f; v.b = v.c = 0; v.tx = v.ty = 0; rm_apply(&v, gx, gy, &gx, &gy);
      /* the stage shows the frame rectangle from (xmin, ymin) */
      v.tx = (s32)floor(-p->movie->stage.xmin * z * 0.05 + 0.5); v.ty = (s32)floor(-p->movie->stage.ymin * z * 0.05 + 0.5); t2d = v; }
    }
    rm_local_to_global(c, &l2g);
    rm_mul(&t2d, &l2g, &m);
    if (!rm_inverse(&m, &inv)) rm_identity(&inv);
    rm_apply(&inv, gx, gy, &lx, &ly);
    *x = lx; *y = ly;
}
static int g_xmouse(Act *a, MovieClip *c, Value *o) { double x, y; (void)a; local_mouse(c, &x, &y); *o = v_num(x / 20.0); return 1; }
static int g_ymouse(Act *a, MovieClip *c, Value *o) { double x, y; (void)a; local_mouse(c, &x, &y); *o = v_num(y / 20.0); return 1; }

typedef struct DProp { const char *name; DPGet get; DPSet set; } DProp;
static const DProp dprops[22] = {
    { "_x", g_x, s_x }, { "_y", g_y, s_y }, { "_xscale", g_xscale, s_xscale }, { "_yscale", g_yscale, s_yscale },
    { "_currentframe", g_currentframe, NULL }, { "_totalframes", g_totalframes, NULL }, { "_alpha", g_alpha, s_alpha },
    { "_visible", g_visible, s_visible }, { "_width", g_width, s_width }, { "_height", g_height, s_height },
    { "_rotation", g_rotation, s_rotation }, { "_target", g_target, NULL }, { "_framesloaded", g_framesloaded, NULL },
    { "_name", g_name, s_name }, { "_droptarget", g_droptarget, NULL }, { "_url", g_url, NULL },
    { "_highquality", g_highquality, s_highquality }, { "_focusrect", g_focusrect, s_focusrect },
    { "_soundbuftime", g_soundbuftime, s_soundbuftime }, { "_quality", g_quality, s_quality },
    { "_xmouse", g_xmouse, NULL }, { "_ymouse", g_ymouse, NULL }
};

static int dprop_index(AStr *name) {
    int i;
    if (name->len < 2 || STR_AT(name, 0) != '_') return -1;
    for (i = 0; i < 22; i++) if (str_eq_c_ci(name, dprops[i].name)) return i;
    return -1;
}

int stage_prop_index_get(Act *a, MovieClip *d, int index, Value *out) {
    if (index < 0 || index >= 22) { *out = v_undef(); return 0; }
    return dprops[index].get(a, d, out);
}
int stage_prop_index_set(Act *a, MovieClip *d, int index, Value v) {
    if (index < 0 || index >= 22 || !dprops[index].set) return 0;
    return dprops[index].set(a, d, v);
}
int stage_prop_read_only(int index) { return index < 0 || index >= 22 || dprops[index].set == NULL; }

Value stage_prop_coerce(Act *a, int index, Value v) {
    double n;
    if ((index >= 0 && index <= 10) || index == 12) { if (prop_num(a, v, &n)) return v_num(n); return v; }
    if (index == 16 || index == 18 || index == 20 || index == 21) return v_num(v_num_of(a, v));
    if (index == 13 || index == 19) return v_str(v_str_of(a, v));
    return v;
}

/* ---------------------------------------------------------------- */
/* stage object properties                                          */

int stage_resolve_path_property(Act *a, MovieClip *d, AStr *name, Value *out) {
    VM *vm = a->vm; int cs = act_case_sensitive(a);
    if (a->swf_version < 5 || !d) return 0;
    if (cs ? str_eq_c(name, "_root") : str_eq_c_ci(name, "_root")) { clip_obj_or_undef_v(vm, clip_avm1_root(d), out); return 1; }
    if (cs ? str_eq_c(name, "_parent") : str_eq_c_ci(name, "_parent")) {
        MovieClip *p = clip_avm1_parent(d);
        if (p) { AObj *po = clip_obj(vm, p); *out = po ? v_obj(po) : v_obj(obj_bare(vm)); return 1; }
        /* no parent: an own member of that name shows (Gnash TextField.as: _root._parent),
           an inherited one does not (Ruffle's issue_768) */
        { AObj *o = clip_obj(vm, d); if (o && o_has_own_property(a, o, name)) { if (vm->err) vm->err = 0; return 0; } }
        *out = v_undef();
        return 1;
    }
    if (a->swf_version >= 6 && (cs ? str_eq_c(name, "_global") : str_eq_c_ci(name, "_global"))) { *out = v_obj(act_global(a)); return 1; }
    if (name->len >= 6 && (cs ? (str_starts_c(name, "_level") || str_starts_c(name, "_flash")) : (str_starts_c_ci(name, "_level") || str_starts_c_ci(name, "_flash")))) {
        int neg = 0, i = 6; s32 lv = 0; MovieClip *l;
        if ((u32)i < name->len && STR_AT(name, i) == '-') { neg = 1; i++; }
        for (; (u32)i < name->len; i++) { u16 ch = STR_AT(name, i); if (ch < '0' || ch > '9') break; lv = (s32)((u32)lv * 10u + (u32)(ch - '0')); }
        if (neg) lv = -lv;
        l = act_get_level(a, lv);
        if (l) clip_obj_or_undef_v(vm, l, out); else *out = v_undef();
        return 1;
    }
    return 0;
}

int stage_get_property(Act *a, MovieClip *d, AStr *name, Value *out) {
    VM *vm = a->vm; int magic = name->len > 0 && STR_AT(name, 0) == '_'; MovieClip *child; int idx;
    if (magic && stage_resolve_path_property(a, d, name, out)) return 1;
    child = clip_child_by_name(a, d, name);
    if (child) { AObj *co = clip_obj(vm, child); *out = co ? v_obj(co) : v_undef(); return 1; }
    if (magic && (idx = dprop_index(name)) >= 0) return dprops[idx].get(a, d, out);
    return 0;
}

int stage_has_property(Act *a, MovieClip *d, AStr *name) {
    Value tmp; int magic = name->len > 0 && STR_AT(name, 0) == '_';
    if (magic && dprop_index(name) >= 0) return 1;
    if (clip_child_by_name(a, d, name)) return 1;
    if (magic && stage_resolve_path_property(a, d, name, &tmp)) return 1;
    return 0;
}

/* a property scripts cannot set on the object (read-only display properties,
   _parent, _root, _global, _levelN): an assignment in a with block goes past it */
int stage_prop_unsettable(Act *a, MovieClip *d, AStr *name) {
    Value tmp; int idx;
    if (name->len < 2 || STR_AT(name, 0) != '_') return 0;
    idx = dprop_index(name);
    if (idx >= 0) return dprops[idx].set == NULL;
    if (str_eq_c_ci(name, "_parent")) return 1;
    return stage_resolve_path_property(a, d, name, &tmp);
}

int stage_set_property(Act *a, MovieClip *d, AStr *name, Value v) {
    int idx = dprop_index(name);
    if (idx < 0) return 0;
    if (dprops[idx].set) dprops[idx].set(a, d, v);
    return 1;
}

void stage_enumerate(Act *a, MovieClip *d, AStr ***keys, int *n, int *cap) {
    DisplayObject *x; int cnt = 0, i; DisplayObject **list;
    for (x = d->children; x; x = x->next) cnt++;
    if (!cnt) return;
    list = (DisplayObject**)malloc((size_t)cnt * sizeof(DisplayObject*));
    if (!list) return;
    i = 0; for (x = d->children; x; x = x->next) list[i++] = x;
    for (i = cnt - 1; i >= 0; i--) {
        if (!list[i]->name[0]) continue;
        if (*n == *cap) { int nc = *cap ? *cap * 2 : 16; AStr **nk = (AStr**)realloc(*keys, (size_t)nc * sizeof(AStr*)); if (!nk) break; *keys = nk; *cap = nc; }
        (*keys)[(*n)++] = str_utf8(a->vm, list[i]->name, strlen(list[i]->name));
    }
    free(list);
}

/* text fields bound to a variable of this clip */
void stage_notify_change(Act *a, MovieClip *d, AStr *name, Value v) {
    extern void a1_textfield_set_bound(Act *a, MovieClip *tf, Value v);
    A1Clip *s = d->a1; int i, cs = act_case_sensitive(a);
    if (!s || !s->nbindings) return;
    for (i = 0; i < s->nbindings; i++) {
        TFBinding *b = &s->bindings[i];
        if (b->tf && str_eq_case(b->var, name, cs)) a1_textfield_set_bound(a, b->tf, v);
        if (a->vm->err) return;
    }
}
