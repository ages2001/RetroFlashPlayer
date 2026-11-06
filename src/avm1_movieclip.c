/* avm1_movieclip.c - the MovieClip and Button classes of AVM1: timeline
 * control, attachMovie / duplicateMovieClip / createEmptyMovieClip,
 * depths, bounds, hit tests, drag, masks and the drawing API. */
#include "avm1_int.h"
#include "bmd.h"
#include "shape.h"
#include "textfield.h"
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

#define AVM_DEPTH_BIAS 16384
#define AVM_MAX_DEPTH 2130706428
#define AVM_MAX_REMOVE_DEPTH 2130706416

extern AObj *lib_class(VM *vm, Env *env, NativeFn ctor, NativeFn call, AObj *super_proto, int proto_first, AObj **proto_out);
extern void avm1_post_instantiation_ex(VM *vm, MovieClip *c, AObj *init, int by_avm, int run_frame);
extern void avm1_remove_child(Player *p, MovieClip *parent, DisplayObject *d);
extern void avm1_replace_at_depth(VM *vm, MovieClip *parent, int depth);
extern void avm1_unload_clip(VM *vm, MovieClip *c);
extern int  clip_find_frame_label_ci(MovieClip *c, const char *label);
extern void dg_local_bounds(MovieClip *c, SWFRect *out, int *any);
extern void dg_union(SWFRect *acc, int *any, const SWFRect *r, const SWFMatrix *m);
extern int  clip_hit_test_point(void *vm, MovieClip *c, double sx, double sy, int shape);
extern void a1_tf_create(VM *vm, MovieClip *c, double x, double y, double w, double h);
extern void a1_tf_copy_editable(MovieClip *dst, MovieClip *src);

static double dnan(void) { double z = 0; return z / z; }

static void defm(VM *vm, Env *env, AObj *o, const char *name, NativeFn f, u16 attr) { o_define(vm, o, str_intern(vm, name), v_obj(fn_native(vm, env, f, NULL)), attr); }
static void defp(VM *vm, Env *env, AObj *o, const char *name, NativeFn g, NativeFn s, u16 attr) { o_add_property(vm, o, str_intern(vm, name), fn_native(vm, env, g, NULL), s ? fn_native(vm, env, s, NULL) : NULL, attr); }

/* the live movie clip behind `this` (also through super) */
static MovieClip *mc_of(AObj *self) {
    MovieClip *c = o_display(self);
    if (!c || clip_removed(c) || !clip_is_movieclip(c)) return NULL;
    return c;
}
static MovieClip *dobj_of(AObj *self) {
    MovieClip *c = o_display(self);
    if (!c || clip_removed(c)) return NULL;
    return c;
}

/* ---------------------------------------------------------------- */
/* frames                                                           */

/* Rust's str::parse::<f64> (with Ruffle's NaN / inf spellings) */
int a1_parse_f64(VM *vm, AStr *s, double *out) {
    const char *u; u32 i = 0, n = s->len; int digits = 0;
    if (n == 3 && str_eq_c(s, "NaN")) { *out = dnan(); return 1; }
    if (n == 3 && str_eq_c(s, "inf")) { double z = 0; *out = 1.0 / z; return 1; }
    if (n == 4 && (STR_AT(s, 0) == '+' || STR_AT(s, 0) == '-')) {
        AStr *t = str_slice(vm, s, 1, 4); double z = 0;
        if (str_eq_c(t, "NaN")) { *out = z / z; return 1; }
        if (str_eq_c(t, "inf")) { *out = STR_AT(s, 0) == '-' ? -1.0 / z : 1.0 / z; return 1; }
        return 0;
    }
    if (i < n && (STR_AT(s, i) == '+' || STR_AT(s, i) == '-')) i++;
    while (i < n && STR_AT(s, i) >= '0' && STR_AT(s, i) <= '9') { i++; digits++; }
    if (i < n && STR_AT(s, i) == '.') { i++; while (i < n && STR_AT(s, i) >= '0' && STR_AT(s, i) <= '9') { i++; digits++; } }
    if (!digits) return 0;
    if (i < n && (STR_AT(s, i) == 'e' || STR_AT(s, i) == 'E')) {
        int ed = 0; i++;
        if (i < n && (STR_AT(s, i) == '+' || STR_AT(s, i) == '-')) i++;
        while (i < n && STR_AT(s, i) >= '0' && STR_AT(s, i) <= '9') { i++; ed++; }
        if (!ed) return 0;
    }
    if (i != n) return 0;
    u = str_c(vm, s);
    *out = strtod(u, NULL);
    return 1;
}

static int frame_label(VM *vm, MovieClip *c, AStr *label) { return clip_find_frame_label_ci(c, str_c(vm, label)); }

/* resolve "path:frame" (ActivationExt::resolve_variable_path) */
int a1_resolve_variable_path(Act *a, MovieClip *start, AStr *path, AObj **obj, AStr **var) {
    long sep = str_rfind_any(path, a->swf_version >= 5 ? ":." : ":");
    MovieClip *root = clip_avm1_root(start);
    if (sep >= 0) {
        AStr *p = str_slice(a->vm, path, 0, (u32)sep);
        AObj *o = act_resolve_target_path(a, root, clip_obj(a->vm, start), p, 1, 1);
        if (a->vm->err || !o) return 0;
        *obj = o; *var = str_slice(a->vm, path, (u32)sep + 1, path->len);
        return 1;
    }
    *obj = clip_obj(a->vm, start); *var = path;
    return *obj != NULL;
}

/* gotoAndPlay / gotoAndStop / scene gotos */
void a1_goto_frame(Act *a, MovieClip *c, Value v, int play, int scene_offset) {
    MovieClip *clip = NULL; s32 frame = 0; int have = 0;
    if (v.t == V_NUM && v.u.n == floor(v.u.n) && v.u.n - v.u.n == 0) { clip = c; frame = f64_i32(v.u.n); have = 1; }
    else {
        AStr *path = v_str_of(a, v); AObj *o = NULL; AStr *fr = NULL;
        if (a->vm->err) return;
        {   /* no target path in it: a frame number or a label of this clip ("4.8" is a label) */
            u32 i; int plain = 1; double num;
            for (i = 0; i < path->len; i++) { u16 ch = STR_AT(path, i); if (ch == ':' || ch == '/') { plain = 0; break; } }
            if (plain) {
                if (v.t != V_NUM && a1_parse_f64(a->vm, path, &num) && num == floor(num)) { clip = c; frame = f64_i32(num); have = 1; }
                else { int f = frame_label(a->vm, c, path); if (f >= 0) { clip = c; frame = f + 1; have = 1; } }
                if (have) goto go;
                /* "_root.mc1.4": a dotted target path, its last part the frame */
                for (i = 0; i < path->len; i++) if (STR_AT(path, i) == '.') break;
                if (i == path->len) return;
            }
        }
        if (a1_resolve_variable_path(a, c, path, &o, &fr)) {
            MovieClip *d = o_display(o); double num;
            if (d && clip_is_movieclip(d) && !clip_removed(d)) {
                if (a1_parse_f64(a->vm, fr, &num) && num == floor(num)) { clip = d; frame = f64_i32(num); have = 1; }   /* "4.8" is a label */
                else { int f = frame_label(a->vm, d, fr); if (f >= 0) { clip = d; frame = f + 1; have = 1; } }
            }
        }
    }
    if (!have) return;
go:
    frame = (s32)((u32)frame - 1u);
    frame = (s32)((u32)frame + (u32)scene_offset);
    if (frame < 2147483647) frame += 1;
    if (frame > 0) {
        if (frame > 65535) frame = 65535;
        player_avm1_goto(clip, frame - 1, play);
    }
}

static Value mc_goto_and_play(Act *a, AObj *self, int argc, Value *argv) { MovieClip *c = mc_of(self); if (c) a1_goto_frame(a, c, arg(argc, argv, 0), 1, 0); return v_undef(); }
static Value mc_goto_and_stop(Act *a, AObj *self, int argc, Value *argv) { MovieClip *c = mc_of(self); if (c) a1_goto_frame(a, c, arg(argc, argv, 0), 0, 0); return v_undef(); }
static Value mc_play(Act *a, AObj *self, int argc, Value *argv) { MovieClip *c = mc_of(self); (void)a; (void)argc; (void)argv; if (c) c->playing = 1; return v_undef(); }
static Value mc_stop(Act *a, AObj *self, int argc, Value *argv) { MovieClip *c = mc_of(self); (void)a; (void)argc; (void)argv; if (c) c->playing = 0; return v_undef(); }
static Value mc_next_frame(Act *a, AObj *self, int argc, Value *argv) { MovieClip *c = mc_of(self); (void)a; (void)argc; (void)argv; if (c && c->cur_frame + 1 < c->frame_count) player_avm1_goto(c, c->cur_frame + 1, 0); return v_undef(); }
static Value mc_prev_frame(Act *a, AObj *self, int argc, Value *argv) { MovieClip *c = mc_of(self); (void)a; (void)argc; (void)argv; if (c && c->cur_frame >= 1) player_avm1_goto(c, c->cur_frame - 1, 0); return v_undef(); }

/* ---------------------------------------------------------------- */
/* creating and removing clips                                      */

static int highest_depth(MovieClip *c) { DisplayObject *d; int h = -1; int any = 0; for (d = c->children; d; d = d->next) { if (!any || d->depth > h) h = d->depth; any = 1; } return any ? h : -1; }

static DisplayObject *new_child(VM *vm, MovieClip *parent, SWFChar *ch, int char_id, AStr *name, int depth) {
    DisplayObject *d = (DisplayObject*)calloc(1, sizeof(DisplayObject));
    if (!d) return NULL;
    d->depth = depth; d->char_id = char_id; d->ch = ch; d->visible = 1; d->owner = parent;
    d->script_placed = 1; d->explicit_name = 1;
    swf_matrix_identity(&d->matrix); swf_cxform_identity(&d->cxform);
    if (name) { strncpy(d->name, str_c(vm, name), sizeof(d->name) - 1); d->name[sizeof(d->name) - 1] = 0; }
    avm1_replace_at_depth(vm, parent, depth);
    player_insert_dobj(parent, d);
    if (!ch || ch->type == CHAR_SPRITE) d->clip = player_make_clip(vm->player, parent, ch, d);
    else if (ch->type == CHAR_EDITTEXT) { d->clip = player_make_clip(vm->player, parent, NULL, d); avm1_bind_textfield(vm->player, d->clip); }
    else if (ch->type == CHAR_BUTTON) { d->clip = player_make_clip(vm->player, parent, NULL, d); avm1_bind_button(vm->player, d->clip); }
    else if (ch->type == CHAR_VIDEO) { d->clip = player_make_clip(vm->player, parent, NULL, d); avm1_bind_video(vm->player, d->clip); }
    return d;
}

static int export_id(MovieClip *c, AStr *name, VM *vm) {
    /* export names are case-insensitive; a later export of the same name replaces the earlier one */
    SWFMovie *m = c->movie; int i, id = -1;
    if (!m) return -1;
    for (i = 0; i < m->nexports; i++) {
        AStr *e = str_from_swf(vm, m->exports[i].name, strlen(m->exports[i].name), m->version);
        if (str_eq_ci(e, name)) id = m->exports[i].id;
    }
    return id;
}

static Value mc_attach_movie(Act *a, AObj *self, int argc, Value *argv) {
    VM *vm = a->vm; MovieClip *c = mc_of(self); AStr *ex, *name; s32 depth; int id; SWFChar *ch; DisplayObject *d; AObj *init = NULL;
    if (!c || argc < 3) return v_undef();
    ex = v_str_of(a, argv[0]); if (vm->err) return v_undef();
    name = v_str_of(a, argv[1]); if (vm->err) return v_undef();
    depth = (s32)((u32)v_i32(a, argv[2]) + AVM_DEPTH_BIAS); if (vm->err) return v_undef();
    if (argc > 3 && argv[3].t == V_OBJ) init = argv[3].u.o;
    if (depth < 0 || depth > AVM_MAX_DEPTH) return v_undef();
    id = export_id(c, ex, vm);
    ch = id >= 0 ? swf_get_char(c->movie, id) : NULL;
    if (!ch || (ch->type != CHAR_SPRITE && ch->type != CHAR_EDITTEXT && ch->type != CHAR_BUTTON && ch->type != CHAR_SHAPE)) return v_undef();
    d = new_child(vm, c, ch, id, name, depth);
    if (!d) return v_undef();
    if (d->clip) {
        avm1_post_instantiation_ex(vm, d->clip, init, 1, 1);
        return d->clip->obj ? v_obj((AObj*)d->clip->obj) : v_undef();
    }
    return v_undef();
}

static Value mc_attach_bitmap(Act *a, AObj *self, int argc, Value *argv) {
    extern BitmapData *a1_bitmapdata_of(AObj *o);
    VM *vm = a->vm; MovieClip *c = mc_of(self); BitmapData *b; s32 depth; int smooth; SWFChar *ch; DisplayObject *d;
    if (!c || argc < 2 || argv[0].t != V_OBJ || argv[0].u.o->nk != NK_BITMAPDATA) return v_undef();
    b = a1_bitmapdata_of(argv[0].u.o);
    depth = (s32)((u32)v_i32(a, argv[1]) + AVM_DEPTH_BIAS); if (vm->err) return v_undef();
    (void)v_bool_of(a, arg(argc, argv, 2));
    smooth = v_bool_of(a, arg(argc, argv, 3));
    c = mc_of(self);
    if (!c || !b) return v_undef();
    ch = (SWFChar*)calloc(1, sizeof(SWFChar));
    if (!ch) return v_undef();
    ch->type = CHAR_BITMAP; ch->tagcode = -1; ch->parsed = b; ch->movie = c->movie;
    d = new_child(vm, c, ch, 0, NULL, depth);
    if (!d) { free(ch); return v_undef(); }
    d->bmd = bmd_ref(b); d->bmd_smooth = smooth; d->explicit_name = 0;
    return v_undef();
}

/* a loaded image: the clip shows it as a bitmap child at depth 1 */
void a1_show_image(VM *vm, MovieClip *c, BitmapData *b) {
    SWFChar *ch; DisplayObject *d;
    if (!c || !b) return;
    ch = (SWFChar*)calloc(1, sizeof(SWFChar));
    if (!ch) return;
    ch->type = CHAR_BITMAP; ch->tagcode = -1; ch->parsed = b; ch->movie = c->movie;
    d = new_child(vm, c, ch, 0, NULL, 1);
    if (!d) { free(ch); return; }
    d->bmd = bmd_ref(b); d->bmd_smooth = 0; d->explicit_name = 0;
}

static Value mc_create_empty(Act *a, AObj *self, int argc, Value *argv) {
    VM *vm = a->vm; MovieClip *c = mc_of(self); AStr *name; s32 depth; DisplayObject *d;
    if (!c || argc < 2) return v_undef();
    name = v_str_of(a, argv[0]); if (vm->err) return v_undef();
    depth = (s32)((u32)v_i32(a, argv[1]) + AVM_DEPTH_BIAS); if (vm->err) return v_undef();
    d = new_child(vm, c, NULL, 0, name, depth);
    if (!d || !d->clip) return v_undef();
    d->clip->movie = c->movie;
    avm1_post_instantiation_ex(vm, d->clip, NULL, 1, 1);
    if (d->clip->obj) { Act b; act_init_nothing(&b, vm, d->clip, "[onConstruct]"); o_call_method(&b, (AObj*)d->clip->obj, vm->s_onConstruct, 0, NULL, ER_SPECIAL); if (vm->err) vm_report_error(&b); }
    return d->clip->obj ? v_obj((AObj*)d->clip->obj) : v_undef();
}

static Value mc_create_text_field(Act *a, AObj *self, int argc, Value *argv) {
    VM *vm = a->vm; MovieClip *c = mc_of(self); double depth; s32 x, y, w, h; DisplayObject *d; AStr *name;
    if (!c || argc < 6) return v_undef();   /* (Gnash TextField.as: one argument missing, no field) */
    depth = v_num_of(a, arg(argc, argv, 1)); if (vm->err) return v_undef();
    x = v_i32(a, arg(argc, argv, 2)); if (vm->err) return v_undef();
    y = v_i32(a, arg(argc, argv, 3)); if (vm->err) return v_undef();
    w = v_i32(a, arg(argc, argv, 4)); if (vm->err) return v_undef();
    h = v_i32(a, arg(argc, argv, 5)); if (vm->err) return v_undef();
    name = v_str_of(a, arg(argc, argv, 0)); if (vm->err) return v_undef();
    {
        s32 di = depth != depth ? 0 : depth >= 2147483647.0 ? 2147483647 : depth <= -2147483648.0 ? (s32)0x80000000u : (s32)depth;
        d = new_child(vm, c, NULL, 0, name, (s32)((u32)di + AVM_DEPTH_BIAS));
    }
    if (!d) return v_undef();
    d->clip = player_make_clip(vm->player, c, NULL, d);
    if (!d->clip) return v_undef();
    avm1_bind_textfield(vm->player, d->clip);
    a1_tf_create(vm, d->clip, x, y, w, h);
    avm1_post_instantiation_ex(vm, d->clip, NULL, 1, 0);
    if (a->swf_version >= 8) return d->clip->obj ? v_obj((AObj*)d->clip->obj) : v_undef();
    return v_undef();
}

/* duplicateMovieClip / MovieClip clone_sprite */
MovieClip *a1_clone_sprite(Act *a, MovieClip *src, AStr *name, s32 depth, AObj *init) {
    VM *vm = a->vm; MovieClip *parent = clip_avm1_parent(src), *nc; DisplayObject *d; SWFChar *ch = NULL; int id = 0;
    if (!parent || !clip_is_movieclip(parent) || !src->self) return NULL;
    if (depth < 0 || depth > AVM_MAX_DEPTH) return NULL;
    if (src->self->char_id > 0) { id = src->self->char_id; ch = src->self->ch; }
    if (src->a1 && src->a1->kind == 2 && !(ch && ch->type == CHAR_EDITTEXT)) {   /* a dynamically created TextField */
        d = new_child(vm, parent, NULL, 0, name, depth);
        if (!d) return NULL;
        d->clip = player_make_clip(vm->player, parent, NULL, d);
        if (!d->clip) return NULL;
        avm1_bind_textfield(vm->player, d->clip);
        a1_tf_create(vm, d->clip, src->self->matrix.tx / 20.0, src->self->matrix.ty / 20.0, 0, 0);
        a1_tf_copy_editable(d->clip, src);
    } else {
        if (ch && ch->type != CHAR_SPRITE && ch->type != CHAR_EDITTEXT && ch->type != CHAR_BUTTON && ch->type != CHAR_VIDEO) ch = NULL;
        if (!ch && !clip_is_movieclip(src)) return NULL;
        d = new_child(vm, parent, ch, id, name, depth);
        if (!d || !d->clip) return NULL;
        if (!ch) d->clip->movie = parent->movie;
    }
    nc = d->clip;
    d->matrix = src->self->matrix; d->cxform = src->self->cxform;
    if (clip_is_movieclip(nc) && src->a1) {
        A1Clip *ns = a1_of(nc), *os = src->a1;
        if (ns && os->nevents) {
            ns->events = (ClipEvent*)malloc((size_t)os->nevents * sizeof(ClipEvent));
            if (ns->events) { memcpy(ns->events, os->events, (size_t)os->nevents * sizeof(ClipEvent)); ns->nevents = os->nevents; ns->event_flags = os->event_flags; }
        }
        if (ns && os->drawing) { extern void a1_drawing_copy(MovieClip *dst, MovieClip *src); a1_drawing_copy(nc, src); }
    }
    avm1_post_instantiation_ex(vm, nc, init, 1, 1);
    return nc;
}

void clip_clone_sprite(Act *a, MovieClip *src, AStr *name, s32 depth, AObj *init) { a1_clone_sprite(a, src, name, depth, init); }

static Value mc_duplicate(Act *a, AObj *self, int argc, Value *argv) {
    VM *vm = a->vm; MovieClip *c = mc_of(self), *n; AStr *name; s32 depth = 0; AObj *init = NULL;
    if (!c || argc < 1) return v_undef();
    name = v_str_of(a, argv[0]); if (vm->err) return v_undef();
    if (argc > 1) { depth = v_i32(a, argv[1]); if (vm->err) return v_undef(); }
    if (argc > 2) { init = v_to_obj_or_bare(a, argv[2]); if (vm->err) return v_undef(); }
    n = a1_clone_sprite(a, c, name, (s32)((u32)depth + AVM_DEPTH_BIAS), init);
    if (a->swf_version < 6) return v_undef();
    return n && n->obj ? v_obj((AObj*)n->obj) : v_undef();
}

/* globals::remove_display_object */
void a1_remove_display_object(Act *a, MovieClip *c) {
    int depth;
    if (!c || !c->self || clip_removed(c)) return;
    depth = c->self->depth;
    if (depth >= AVM_DEPTH_BIAS && depth < AVM_MAX_REMOVE_DEPTH) {
        MovieClip *par = clip_avm1_parent(c);
        if (par && clip_is_movieclip(par)) avm1_remove_child(a->vm->player, par, c->self);
    }
}
void clip_remove_display(Act *a, MovieClip *c) { a1_remove_display_object(a, c); }

static Value mc_remove_movie_clip(Act *a, AObj *self, int argc, Value *argv) { (void)argc; (void)argv; a1_remove_display_object(a, dobj_of(self)); return v_undef(); }

static Value get_depth(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = o_display(self); (void)a; (void)argc; (void)argv;
    if (c && c->self) return v_num((s32)((u32)c->self->depth - AVM_DEPTH_BIAS));
    return v_undef();
}

static Value mc_get_next_highest_depth(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = mc_of(self); s32 d; (void)a; (void)argc; (void)argv;
    if (!c) return v_undef();
    d = (s32)((u32)highest_depth(c) - (AVM_DEPTH_BIAS - 1));
    return v_num(d < 0 ? 0 : d);
}

static Value mc_get_instance_at_depth(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = mc_of(self); s32 depth; DisplayObject *d;
    if (!c) return v_undef();
    if (argc < 1) return v_undef();
    depth = (s32)((u32)v_i32(a, argv[0]) + AVM_DEPTH_BIAS); if (a->vm->err) return v_undef();
    d = clip_find_depth(c, depth);
    if (!d) return v_undef();
    if (d->clip && d->clip->obj) return v_obj((AObj*)d->clip->obj);
    if (d->clip) { AObj *o = clip_obj(a->vm, d->clip); if (o) return v_obj(o); }
    return v_obj(self);
}

static Value mc_swap_depths(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = mc_of(self), *par; Value v = arg(argc, argv, 0); s32 depth = 0; int have = 0; DisplayObject *other;
    if (!c || !c->self) return v_undef();
    par = clip_avm1_parent(c);
    if (!par || !clip_is_movieclip(par)) return v_undef();
    if (v.t == V_NUM) { depth = (s32)((u32)f64_i32(v.u.n) + AVM_DEPTH_BIAS); have = 1; }
    else {
        MovieClip *t = act_resolve_target_display(a, c, v, 0);
        if (a->vm->err) return v_undef();
        if (t && t->self && clip_avm1_parent(t) == par && !clip_removed(t)) { depth = t->self->depth; have = 1; }
    }
    if (!have || depth < 0 || depth > AVM_MAX_DEPTH) return v_undef();
    if (depth == c->self->depth) return v_undef();
    other = clip_find_depth(par, depth);
    if (other) { other->depth = c->self->depth; other->clip_depth = 0; other->script_moved = 1; c->self->clip_depth = 0; }
    c->self->depth = depth;
    c->self->script_moved = 1;
    player_resort(par);
    return v_undef();
}

/* ---------------------------------------------------------------- */
/* geometry                                                         */

extern void clip_world_matrix(MovieClip *c, SWFMatrix *out);

static Value mc_local_to_global(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = mc_of(self); Value px, py; AObj *pt; RMat m; s32 gx, gy;
    if (!c || argc < 1 || argv[0].t != V_OBJ) return v_undef();
    pt = argv[0].u.o;
    if (!o_get_local_stored(a, pt, a->vm->s_x, &px)) px = v_undef();
    if (!o_get_local_stored(a, pt, a->vm->s_y, &py)) py = v_undef();
    if (px.t != V_NUM || py.t != V_NUM) return v_undef();
    rm_local_to_global(c, &m);
    rm_apply(&m, tw_from_px(px.u.n), tw_from_px(py.u.n), &gx, &gy);
    o_set(a, pt, a->vm->s_x, v_num(gx / 20.0)); if (a->vm->err) return v_undef();
    o_set(a, pt, a->vm->s_y, v_num(gy / 20.0));
    return v_undef();
}

static Value mc_global_to_local(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = mc_of(self); Value px, py; AObj *pt; RMat m, inv; s32 x, y, lx, ly;
    if (!c || argc < 1 || argv[0].t != V_OBJ) return v_undef();
    pt = argv[0].u.o;
    if (!o_get_local_stored(a, pt, a->vm->s_x, &px)) px = v_undef();
    if (!o_get_local_stored(a, pt, a->vm->s_y, &py)) py = v_undef();
    if (px.t != V_NUM || py.t != V_NUM) return v_undef();
    x = tw_from_px(px.u.n); y = tw_from_px(py.u.n);
    rm_local_to_global(c, &m);
    if (rm_inverse(&m, &inv)) rm_apply(&inv, x, y, &lx, &ly); else { lx = x; ly = y; }
    o_set(a, pt, a->vm->s_x, v_num(lx / 20.0)); if (a->vm->err) return v_undef();
    o_set(a, pt, a->vm->s_y, v_num(ly / 20.0));
    return v_undef();
}

static Value make_bounds(Act *a, double x0, double x1, double y0, double y1) {
    AObj *o = obj_new(a->vm, PROTOS(a)->object);
    o_set(a, o, str_intern(a->vm, "xMin"), v_num(x0));
    o_set(a, o, str_intern(a->vm, "xMax"), v_num(x1));
    o_set(a, o, str_intern(a->vm, "yMin"), v_num(y0));
    o_set(a, o, str_intern(a->vm, "yMax"), v_num(y1));
    return v_obj(o);
}

static Value mc_get_bounds(Act *a, AObj *self, int argc, Value *argv) {
    VM *vm = a->vm; MovieClip *c = mc_of(self), *target; SWFRect b; int any = 0;
    if (!c) { c = o_display(self); if (c && !(c->a1 && c->a1->kind == 2)) c = NULL; }   /* also text fields (Gnash TextField.as) */
    if (!c) return v_undef();
    if (argc > 0) { target = act_resolve_target_display(a, c, argv[0], 0); if (vm->err) return v_undef(); }
    else target = c;
    if (!target) return v_undef();
    if (!vm->use_new_invalid_bounds && (a->swf_version >= 8 || vm->swf_version >= 8)) vm->use_new_invalid_bounds = 1;
    b.xmin = b.xmax = b.ymin = b.ymax = 0;
    dg_local_bounds(c, &b, &any);
    if (!any) { b.xmin = b.xmax = b.ymin = b.ymax = 0x7FFFFFF; }
    if (target != c) {
        /* Flash's matrix arithmetic (f32, rounded twips): to_target * to_global, then the four corners */
        if (!any) {
            /* an invalid rectangle stays invalid through any transform */
            if (vm->use_new_invalid_bounds) return make_bounds(a, 0x8000000 / 20.0, 0x8000000 / 20.0, 0x8000000 / 20.0, 0x8000000 / 20.0);
        } else {
            RMat g, tg, inv, m; s32 xs[4], ys[4]; int k;
            rm_local_to_global(c, &g); rm_local_to_global(target, &tg);
            if (!rm_inverse(&tg, &inv)) rm_identity(&inv);
            rm_mul(&inv, &g, &m);
            rm_apply(&m, b.xmin, b.ymin, &xs[0], &ys[0]); rm_apply(&m, b.xmin, b.ymax, &xs[1], &ys[1]);
            rm_apply(&m, b.xmax, b.ymin, &xs[2], &ys[2]); rm_apply(&m, b.xmax, b.ymax, &xs[3], &ys[3]);
            b.xmin = b.xmax = xs[0]; b.ymin = b.ymax = ys[0];
            for (k = 1; k < 4; k++) {
                if (xs[k] < b.xmin) b.xmin = xs[k]; if (xs[k] > b.xmax) b.xmax = xs[k];
                if (ys[k] < b.ymin) b.ymin = ys[k]; if (ys[k] > b.ymax) b.ymax = ys[k];
            }
        }
    }
    if (avm1_player_major() <= 9)   /* Flash 9 multiplies by 0.05 (Gnash MovieClip.as: 6710886.35 is not quite that) */
        return make_bounds(a, b.xmin * 0.05, b.xmax * 0.05, b.ymin * 0.05, b.ymax * 0.05);
    return make_bounds(a, b.xmin / 20.0, b.xmax / 20.0, b.ymin / 20.0, b.ymax / 20.0);
}

static Value mc_hit_test(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = mc_of(self);
    if (!c) return v_undef();
    if (argc > 1) {
        double x = v_num_of(a, argv[0]), y; int shape;
        if (a->vm->err) return v_undef();
        y = v_num_of(a, argv[1]); if (a->vm->err) return v_undef();
        shape = argc > 2 && v_as_bool(argv[2], a->swf_version);
        if (x - x == 0 && y - y == 0) {
            /* the point is in the coordinate space of the (unlocked) root */
            extern void dg_world_bounds(MovieClip *c, SWFRect *out, int *any);
            MovieClip *r = c; RMat rm; s32 gx, gy;
            while (clip_avm1_parent(r)) r = clip_avm1_parent(r);
            rm_local_to_global(r, &rm);
            rm_apply(&rm, tw_from_px(x), tw_from_px(y), &gx, &gy);
            if (shape) return v_bool(clip_hit_test_point(a->vm, c, gx, gy, 1));
            {
                SWFRect wb; int any = 0;
                dg_world_bounds(c, &wb, &any);
                return v_bool(any && gx >= wb.xmin && gx <= wb.xmax && gy >= wb.ymin && gy <= wb.ymax);
            }
        }
    } else if (argc == 1) {
        MovieClip *o = act_resolve_target_display(a, c, argv[0], 0);
        if (a->vm->err) return v_undef();
        if (o) {
            extern void dg_world_bounds(MovieClip *c, SWFRect *out, int *any);
            SWFRect b1, b2; int a1 = 0, a2 = 0;
            dg_world_bounds(c, &b1, &a1); dg_world_bounds(o, &b2, &a2);
            if (!a1 || !a2) return v_bool(0);
            return v_bool(b1.xmin <= b2.xmax && b1.xmax >= b2.xmin && b1.ymin <= b2.ymax && b1.ymax >= b2.ymin);
        }
    }
    return v_bool(0);
}

/* ---------------------------------------------------------------- */
/* drag and masks                                                   */

static Value mc_start_drag(Act *a, AObj *self, int argc, Value *argv) {
    VM *vm = a->vm; MovieClip *c = dobj_of(self); int lock;   /* text fields can be dragged too */
    if (!c) return v_undef();
    lock = argc > 0 && v_as_bool(argv[0], a->swf_version);
    vm->drag_constrained = 0;
    if (argc > 1) {
        int i;
        for (i = 0; i < 4; i++) { double d = v_num_of(a, arg(argc, argv, i + 1)); if (vm->err) return v_undef(); vm->drag_rect[i] = d - d == 0 ? d : 0; }
        if (vm->drag_rect[2] < vm->drag_rect[0]) { double t = vm->drag_rect[0]; vm->drag_rect[0] = vm->drag_rect[2]; vm->drag_rect[2] = t; }
        if (vm->drag_rect[3] < vm->drag_rect[1]) { double t = vm->drag_rect[1]; vm->drag_rect[1] = vm->drag_rect[3]; vm->drag_rect[3] = t; }
        vm->drag_constrained = 1;
    }
    vm->drag = c; vm->drag_lock = lock;
    { extern void avm1_start_drag_at(VM *vm); avm1_start_drag_at(vm); }
    return v_undef();
}
static Value mc_stop_drag(Act *a, AObj *self, int argc, Value *argv) { extern void avm1_update_drag(VM *vm); (void)self; (void)argc; (void)argv; avm1_update_drag(a->vm); a->vm->drag = NULL; return v_undef(); }

void clip_start_drag(Act *a, MovieClip *c, int lock_center, double *bounds) {
    VM *vm = a->vm; int i;
    vm->drag = c; vm->drag_lock = lock_center; vm->drag_constrained = bounds != NULL;
    { extern void avm1_start_drag_at(VM *vm); avm1_start_drag_at(vm); }
    if (bounds) for (i = 0; i < 4; i++) vm->drag_rect[i] = bounds[i] - bounds[i] == 0 ? bounds[i] : 0;
}

static Value mc_set_mask(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = mc_of(self), *m = NULL; A1Clip *s;
    if (!c || argc < 1) return v_undef();
    if (argv[0].t != V_UNDEF && argv[0].t != V_NULL) {
        m = act_resolve_target_display(a, act_target_or_root(a), argv[0], 0);
        if (a->vm->err) return v_undef();
        if (!m) return v_bool(0);
    }
    s = a1_of(c);
    if (!s) return v_undef();
    if (s->masker && s->masker->a1) s->masker->a1->maskee = NULL;
    s->masker = m;
    /* a script mask replaces timeline masking: both lose their clip depth (Ruffle set_mask) */
    if (c->self) c->self->clip_depth = 0;
    if (m && m->self) m->self->clip_depth = 0;
    if (m) {
        A1Clip *ms = a1_of(m);
        if (ms) { if (ms->maskee && ms->maskee->a1) ms->maskee->a1->masker = NULL; ms->maskee = c; }
    }
    return v_bool(1);
}

MovieClip *clip_script_mask(void *vmp, MovieClip *c) {
    extern MovieClip *a2_clip_mask(MovieClip *c);
    (void)vmp;
    if (c->obj2) return a2_clip_mask(c);
    return c->a1 ? c->a1->masker : NULL;
}
int clip_is_script_mask(void *vmp, MovieClip *c) {
    extern int a2_clip_is_mask(MovieClip *c);
    (void)vmp;
    if (c->obj2) return a2_clip_is_mask(c);
    return c->a1 && c->a1->maskee != NULL;
}

/* ---------------------------------------------------------------- */
/* drawing API                                                      */

ShapeDef *clip_drawing(void *vmp, MovieClip *c) {
    extern ShapeDef *a2_clip_drawing(MovieClip *c);
    (void)vmp;
    if (c->obj2) { ShapeDef *d = a2_clip_drawing(c); if (d) return d; }
    return c->a1 ? (ShapeDef*)c->a1->drawing : NULL;
}

void a1_drawing_free(MovieClip *c) { if (c->a1 && c->a1->drawing) { shape_free((ShapeDef*)c->a1->drawing); c->a1->drawing = NULL; } }

void a1_drawing_copy(MovieClip *dst, MovieClip *src) {
    ShapeDef *s = src->a1 ? (ShapeDef*)src->a1->drawing : NULL, *d; A1Clip *ds = a1_of(dst);
    if (!s || !ds) return;
    d = (ShapeDef*)calloc(1, sizeof(ShapeDef));
    if (!d) return;
    *d = *s;
    d->edges = s->nedges ? (ShapeEdge*)malloc((size_t)s->nedges * sizeof(ShapeEdge)) : NULL; d->capedges = s->nedges;
    if (d->edges) memcpy(d->edges, s->edges, (size_t)s->nedges * sizeof(ShapeEdge));
    d->fills = s->nfills ? (FillStyle*)malloc((size_t)s->nfills * sizeof(FillStyle)) : NULL; d->capfills = s->nfills;
    if (d->fills) memcpy(d->fills, s->fills, (size_t)s->nfills * sizeof(FillStyle));
    d->lines = s->nlines ? (LineStyle*)malloc((size_t)s->nlines * sizeof(LineStyle)) : NULL; d->caplines = s->nlines;
    if (d->lines) memcpy(d->lines, s->lines, (size_t)s->nlines * sizeof(LineStyle));
    ds->drawing = d;
    ds->draw_fill = src->a1->draw_fill; ds->draw_line = src->a1->draw_line; ds->draw_open = src->a1->draw_open;
    ds->draw_x = src->a1->draw_x; ds->draw_y = src->a1->draw_y; ds->draw_start_x = src->a1->draw_start_x; ds->draw_start_y = src->a1->draw_start_y;
}

static ShapeDef *drawing_of(MovieClip *c) {
    A1Clip *s = a1_of(c);
    if (!s) return NULL;
    if (!s->drawing) { ShapeDef *n = (ShapeDef*)calloc(1, sizeof(ShapeDef)); if (n) n->version = 4; s->drawing = n; }
    return (ShapeDef*)s->drawing;
}

static void add_edge(ShapeDef *s, ShapeEdge *e) {
    if (s->nedges == s->capedges) {
        int nc = s->capedges ? s->capedges * 2 : 32;
        ShapeEdge *ne = (ShapeEdge*)realloc(s->edges, (size_t)nc * sizeof(ShapeEdge));
        if (!ne) return;
        s->edges = ne; s->capedges = nc;
    }
    s->edges[s->nedges++] = *e;
}

static s32 tw(double px) { double t = px * 20.0; if (t != t) return 0; if (t >= 2147483647.0) return 2147483647; if (t <= -2147483648.0) return (s32)0x80000000u; return (s32)(t < 0 ? ceil(t) : floor(t)); }

/* close the open fill path back to its start */
static void close_fill(A1Clip *s, ShapeDef *d) {
    if (s->draw_fill && s->draw_open && (s->draw_x != s->draw_start_x || s->draw_y != s->draw_start_y)) {
        ShapeEdge e; memset(&e, 0, sizeof(e));
        e.x0 = s->draw_x; e.y0 = s->draw_y; e.x1 = s->draw_start_x; e.y1 = s->draw_start_y;
        e.fill1 = s->draw_fill; e.line = s->draw_line;    /* the closing segment is stroked too */
        add_edge(d, &e);
    }
    s->draw_open = 0;
}
/* the segment that would close the fill still being drawn (hit tests close it) */
int a1_drawing_pending_close(MovieClip *c, ShapeEdge *e) {
    A1Clip *s = c ? c->a1 : NULL;
    if (!s || !s->draw_fill || !s->draw_open || (s->draw_x == s->draw_start_x && s->draw_y == s->draw_start_y)) return 0;
    memset(e, 0, sizeof(*e));
    e->x0 = s->draw_x; e->y0 = s->draw_y; e->x1 = s->draw_start_x; e->y1 = s->draw_start_y; e->fill1 = s->draw_fill;
    return 1;
}

/* rendering a drawing whose fill is still open: Ruffle closes the fill and strokes the closing segment */
int clip_drawing_pending_close(void *vmp, MovieClip *c, ShapeEdge *e) {
    extern int a2_drawing_pending_close(MovieClip *c, ShapeEdge *e);
    (void)vmp;
    if (c && c->obj2) return a2_drawing_pending_close(c, e);
    if (!a1_drawing_pending_close(c, e)) return 0;
    e->line = c->a1->draw_line;
    return 1;
}

static u32 rgb_of(Act *a, Value v) { return v_u32(a, v); }

static Value mc_begin_fill(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = mc_of(self); ShapeDef *d; A1Clip *s; FillStyle f; u32 rgb; double alpha = 100;
    if (!c) return v_undef();
    d = drawing_of(c); s = a1_of(c);
    if (!d || !s) return v_undef();
    close_fill(s, d);
    s->draw_fill = 0;
    if (argc < 1) return v_undef();
    rgb = rgb_of(a, argv[0]); if (a->vm->err) return v_undef();
    if (argc > 1) { alpha = v_num_of(a, argv[1]); if (a->vm->err) return v_undef(); if (alpha != alpha) alpha = 0; if (alpha < 0) alpha = 0; if (alpha > 100) alpha = 100; }
    memset(&f, 0, sizeof(f)); swf_matrix_identity(&f.matrix);
    f.type = FILL_SOLID; f.color.r = (u8)(rgb >> 16); f.color.g = (u8)(rgb >> 8); f.color.b = (u8)rgb;
    f.color.a = (u8)(int)((float)(alpha / 100.0) * 255.0f);
    if (d->nfills == d->capfills) { int nc = d->capfills ? d->capfills * 2 : 8; FillStyle *nf = (FillStyle*)realloc(d->fills, (size_t)nc * sizeof(FillStyle)); if (!nf) return v_undef(); d->fills = nf; d->capfills = nc; }
    d->fills[d->nfills++] = f;
    s->draw_fill = d->nfills; s->draw_open = 0; s->draw_start_x = s->draw_x; s->draw_start_y = s->draw_y;
    return v_undef();
}
/* beginGradientFill / lineGradientStyle arguments (Ruffle movie_clip.rs):
   returns 0 when the call is to be ignored */
static double f64_sat_trunc(double v, double lo, double hi) { if (v != v) return 0; if (v < lo) return lo; if (v > hi) return hi; return v < 0 ? ceil(v) : floor(v); }
static s32 px_twips(double px) { return (s32)f64_sat_trunc(px * 20.0, -2147483648.0, 2147483647.0); }
static float fixed16_f32(double v) {
    float f = (float)v * 65536.0f;
    if (f != f) return 0;
    if (f >= 2147483647.0f) return 32768.0f;
    if (f <= -2147483648.0f) return -32768.0f;
    return (float)((s32)f) / 65536.0f;
}
static int gradient_args(Act *a, int argc, Value *argv, FillStyle *f) {
    AStr *ty; AObj *colors, *alphas, *ratios, *mo; int nc, na, nr, i, n; double focal = 0;
    memset(f, 0, sizeof(*f)); swf_matrix_identity(&f->matrix);
    ty = v_str_of(a, argv[0]); if (a->vm->err) return 0;
    if (str_eq_c(ty, "linear")) f->type = FILL_LINEAR;
    else if (str_eq_c(ty, "radial")) f->type = FILL_RADIAL;
    else return 0;
    colors = v_to_obj_or_bare(a, argv[1]); alphas = v_to_obj_or_bare(a, argv[2]); ratios = v_to_obj_or_bare(a, argv[3]);
    if (a->vm->err || !colors || !alphas || !ratios) return 0;
    nc = o_length(a, colors); na = o_length(a, alphas); nr = o_length(a, ratios);
    if (a->vm->err || nc != na || nc != nr) return 0;
    n = 0;
    for (i = 0; i < nc; i++) {
        u32 col = v_u32(a, o_get_element(a, colors, i)); double al, ra;
        if (a->vm->err) return 0;
        al = v_num_of(a, o_get_element(a, alphas, i)); if (a->vm->err) return 0;
        if (al != al) al = 0; if (al < 0) al = 0; if (al > 100) al = 100;   /* f64::clamp keeps NaN, `as u8` makes it 0 */
        ra = v_num_of(a, o_get_element(a, ratios, i)); if (a->vm->err) return 0;
        if (ra <= -1.0 || ra >= 256.0) return 0;
        if (n < MAX_GRAD) {
            f->grad.ratio[n] = (u8)f64_sat_trunc(ra, 0, 255);
            f->grad.color[n].r = (u8)(col >> 16); f->grad.color[n].g = (u8)(col >> 8); f->grad.color[n].b = (u8)col;
            f->grad.color[n].a = (u8)f64_sat_trunc(al / 100.0 * 255.0, 0, 255);
            n++;
        }
    }
    f->grad.n = n;
    mo = v_to_obj_or_bare(a, argv[4]); if (a->vm->err || !mo) return 0;
    {
        Value mt = o_get(a, mo, str_intern(a->vm, "matrixType")); AStr *ms = v_str_of(a, mt);
        if (a->vm->err) return 0;
        if (str_eq_c(ms, "box")) {
            double w = v_num_of(a, o_get(a, mo, str_intern(a->vm, "w"))), h = v_num_of(a, o_get(a, mo, str_intern(a->vm, "h")));
            double r = v_num_of(a, o_get(a, mo, str_intern(a->vm, "r"))), x = v_num_of(a, o_get(a, mo, str_intern(a->vm, "x"))), y = v_num_of(a, o_get(a, mo, str_intern(a->vm, "y")));
            float sx, sy, rot;
            if (a->vm->err) return 0;
            /* Matrix::create_gradient_box */
            sx = (float)w / 1638.4f; sy = (float)h / 1638.4f; rot = (float)r;
            f->matrix.sx = (float)cos(rot) * sx; f->matrix.r1 = -(float)sin(rot) * sx;
            f->matrix.r0 = (float)sin(rot) * sy; f->matrix.sy = (float)cos(rot) * sy;
            f->matrix.tx = (double)px_twips(x) + px_twips((double)((float)w / 2.0f));
            f->matrix.ty = (double)px_twips(y) + px_twips((double)((float)h / 2.0f));
        } else {
            f->matrix.sx = (float)v_num_of(a, o_get(a, mo, str_intern(a->vm, "a"))); f->matrix.r0 = (float)v_num_of(a, o_get(a, mo, str_intern(a->vm, "b")));
            f->matrix.r1 = (float)v_num_of(a, o_get(a, mo, str_intern(a->vm, "c"))); f->matrix.sy = (float)v_num_of(a, o_get(a, mo, str_intern(a->vm, "d")));
            f->matrix.tx = px_twips(v_num_of(a, o_get(a, mo, str_intern(a->vm, "tx")))); f->matrix.ty = px_twips(v_num_of(a, o_get(a, mo, str_intern(a->vm, "ty"))));
            if (a->vm->err) return 0;
        }
    }
    /* stored as an swf::Matrix: Fixed16 components (Fixed16::from_f32 truncates) */
    f->matrix.sx = fixed16_f32(f->matrix.sx); f->matrix.r0 = fixed16_f32(f->matrix.r0);
    f->matrix.r1 = fixed16_f32(f->matrix.r1); f->matrix.sy = fixed16_f32(f->matrix.sy);
    if (argc > 5) { AStr *sp = v_str_of(a, argv[5]); if (!a->vm->err) f->grad.spread = str_eq_c(sp, "reflect") ? 1 : str_eq_c(sp, "repeat") ? 2 : 0; a->vm->err = 0; }
    if (argc > 6) { AStr *ip = v_str_of(a, argv[6]); if (!a->vm->err) f->grad.interp = str_eq_c(ip, "linearRGB") ? 1 : 0; a->vm->err = 0; }
    if (argc > 7) { focal = v_num_of(a, argv[7]); if (a->vm->err) { a->vm->err = 0; focal = 0; } }
    if (f->type == FILL_RADIAL && focal != 0) { f->type = FILL_FOCAL; f->grad.focal = (s16)f64_sat_trunc(focal * 256.0, -32768, 32767) / 256.0; }
    return 1;
}
static Value mc_begin_gradient_fill(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = mc_of(self); ShapeDef *d; A1Clip *s; FillStyle f;
    if (!c) return v_undef();
    d = drawing_of(c); s = a1_of(c);
    if (!d || !s) return v_undef();
    if (argc < 1 || argv[0].t == V_UNDEF) { close_fill(s, d); s->draw_fill = 0; return v_undef(); }   /* no fill */
    if (argc < 5) return v_undef();
    if (argc > 8 || (argc > 5 && a->swf_version < 8)) return v_undef();   /* too many arguments: ignored */
    if (!gradient_args(a, argc, argv, &f)) return v_undef();
    close_fill(s, d);
    if (d->nfills == d->capfills) { int nc = d->capfills ? d->capfills * 2 : 8; FillStyle *nf = (FillStyle*)realloc(d->fills, (size_t)nc * sizeof(FillStyle)); if (!nf) return v_undef(); d->fills = nf; d->capfills = nc; }
    d->fills[d->nfills++] = f;
    s->draw_fill = d->nfills; s->draw_open = 0; s->draw_start_x = s->draw_x; s->draw_start_y = s->draw_y;
    return v_undef();
}
/* beginBitmapFill(bitmap, matrix, repeating = true, smoothed = false) (Ruffle movie_clip.rs) */
static Value mc_begin_bitmap_fill(Act *a, AObj *self, int argc, Value *argv) {
    extern BitmapData *a1_bitmapdata_of(AObj *o);
    MovieClip *c = mc_of(self); ShapeDef *d; A1Clip *s; FillStyle f; BitmapData *b;
    if (!c) return v_undef();
    d = drawing_of(c); s = a1_of(c);
    if (!d || !s) return v_undef();
    b = (argc > 0 && argv[0].t == V_OBJ) ? a1_bitmapdata_of(argv[0].u.o) : NULL;
    if (b) {
        AObj *mo; Value mv[6]; int k, all = 1; static const char *mn[6] = { "a", "b", "c", "d", "tx", "ty" }; double m[6];
        memset(&f, 0, sizeof(f)); swf_matrix_identity(&f.matrix);
        mo = v_to_obj_or_bare(a, argc > 1 ? argv[1] : v_undef()); if (a->vm->err) return v_undef();
        for (k = 0; k < 6 && mo; k++) if (!o_get_local_stored(a, mo, str_intern(a->vm, mn[k]), &mv[k])) { all = 0; break; }
        if (mo && all) {
            for (k = 0; k < 6; k++) { m[k] = v_num_of(a, mv[k]); if (a->vm->err) return v_undef(); }
            f.matrix.sx = (float)m[0]; f.matrix.r0 = (float)m[1]; f.matrix.r1 = (float)m[2]; f.matrix.sy = (float)m[3];
            f.matrix.tx = px_twips(m[4]); f.matrix.ty = px_twips(m[5]);
        }
        /* the matrix is in pixels: scale from twips */
        f.matrix.sx = (float)f.matrix.sx * 20.0f; f.matrix.r0 = (float)f.matrix.r0 * 20.0f; f.matrix.r1 = (float)f.matrix.r1 * 20.0f; f.matrix.sy = (float)f.matrix.sy * 20.0f;
        {
            int rep = argc > 2 ? v_as_bool(argv[2], a->swf_version) : 1, sm = argc > 3 ? v_as_bool(argv[3], a->swf_version) : 0;
            f.type = rep ? (sm ? FILL_BITMAP_REPEAT : FILL_BITMAP_REPEAT_NS) : (sm ? FILL_BITMAP_CLIP : FILL_BITMAP_CLIP_NS);
        }
        f.bitmap_id = bmd_fill_register(b);
    }
    close_fill(s, d);
    s->draw_fill = 0;
    if (!b) return v_undef();
    if (d->nfills == d->capfills) { int nc = d->capfills ? d->capfills * 2 : 8; FillStyle *nf = (FillStyle*)realloc(d->fills, (size_t)nc * sizeof(FillStyle)); if (!nf) return v_undef(); d->fills = nf; d->capfills = nc; }
    d->fills[d->nfills++] = f;
    s->draw_fill = d->nfills; s->draw_open = 0; s->draw_start_x = s->draw_x; s->draw_start_y = s->draw_y;
    return v_undef();
}
/* lineGradientStyle: the current line style is stroked with a gradient from now on */
static Value mc_line_gradient_style(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = mc_of(self); ShapeDef *d; A1Clip *s; FillStyle f; LineStyle l;
    if (!c || argc < 5 || argc > 8) return v_undef();
    d = drawing_of(c); s = a1_of(c);
    if (!d || !s || s->draw_line <= 0 || s->draw_line > d->nlines) return v_undef();
    if (!gradient_args(a, argc, argv, &f)) return v_undef();
    l = d->lines[s->draw_line - 1];
    l.has_fill = 1; l.fill = f;
    if (d->nlines == d->caplines) { int nc = d->caplines ? d->caplines * 2 : 8; LineStyle *nl = (LineStyle*)realloc(d->lines, (size_t)nc * sizeof(LineStyle)); if (!nl) return v_undef(); d->lines = nl; d->caplines = nc; }
    d->lines[d->nlines++] = l;
    s->draw_line = d->nlines;
    return v_undef();
}
static Value mc_end_fill(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = mc_of(self); ShapeDef *d; A1Clip *s; (void)a; (void)argc; (void)argv;
    if (!c) return v_undef();
    d = drawing_of(c); s = a1_of(c);
    if (!d || !s) return v_undef();
    close_fill(s, d);
    s->draw_fill = 0;
    return v_undef();
}
static Value mc_line_style(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = mc_of(self); ShapeDef *d; A1Clip *s; LineStyle l; double w; u32 rgb = 0; double alpha = 100;
    if (!c) return v_undef();
    d = drawing_of(c); s = a1_of(c);
    if (!d || !s) return v_undef();
    if (argc < 1) { s->draw_line = 0; return v_undef(); }
    w = v_num_of(a, argv[0]); if (a->vm->err) return v_undef();
    if (w != w) w = 0;
    if (w < 0) w = 0;
    if (w > 255) w = 255;
    if (argc > 1) { rgb = rgb_of(a, argv[1]); if (a->vm->err) return v_undef(); }
    if (argc > 2) { alpha = v_num_of(a, argv[2]); if (a->vm->err) return v_undef(); if (alpha != alpha) alpha = 0; if (alpha < 0) alpha = 0; if (alpha > 100) alpha = 100; }
    memset(&l, 0, sizeof(l));
    l.width = tw(w);
    l.color.r = (u8)(rgb >> 16); l.color.g = (u8)(rgb >> 8); l.color.b = (u8)rgb;
    l.color.a = (u8)(int)((float)(alpha / 100.0) * 255.0f);
    /* scale mode, caps, joints and miter limit (Ruffle avm1 line_style; "vertical" keeps the x scale) */
    l.miter = 3.0;
    if (argc > 4) { AStr *t = v_str_of(a, argv[4]); if (!a->vm->err) { if (str_eq_c(t, "none")) l.no_hscale = l.no_vscale = 1; else if (str_eq_c(t, "vertical")) l.no_vscale = 1; else if (str_eq_c(t, "horizontal")) l.no_hscale = 1; } a->vm->err = 0; }
    if (argc > 5) { AStr *t = v_str_of(a, argv[5]); if (!a->vm->err) l.start_cap = l.end_cap = str_eq_c(t, "square") ? 2 : str_eq_c(t, "none") ? 1 : 0; a->vm->err = 0; }
    if (argc > 6) {
        AStr *t = v_str_of(a, argv[6]);
        if (!a->vm->err) {
            if (str_eq_c(t, "miter")) {
                l.join = 2;
                if (argc > 7) { double m = v_num_of(a, argv[7]); if (m != m) m = 0; if (m < 0) m = 0; if (m > 255) m = 255; l.miter = (double)(s16)(m * 256.0) / 256.0; }
            } else if (str_eq_c(t, "bevel")) l.join = 1;
        }
        a->vm->err = 0;
    }
    if (d->nlines == d->caplines) { int nc = d->caplines ? d->caplines * 2 : 8; LineStyle *nl = (LineStyle*)realloc(d->lines, (size_t)nc * sizeof(LineStyle)); if (!nl) return v_undef(); d->lines = nl; d->caplines = nc; }
    d->lines[d->nlines++] = l;
    s->draw_line = d->nlines;
    return v_undef();
}
static Value mc_move_to(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = mc_of(self); A1Clip *s; ShapeDef *d; double x, y;
    if (!c || argc < 2) return v_undef();
    x = v_num_of(a, argv[0]); if (a->vm->err) return v_undef();
    y = v_num_of(a, argv[1]); if (a->vm->err) return v_undef();
    d = drawing_of(c); s = a1_of(c);
    if (!d || !s) return v_undef();
    close_fill(s, d);
    s->draw_x = tw(x); s->draw_y = tw(y);
    s->draw_start_x = s->draw_x; s->draw_start_y = s->draw_y;
    return v_undef();
}
static void draw_to(MovieClip *c, int curve, s32 cx, s32 cy, s32 x, s32 y) {
    A1Clip *s = a1_of(c); ShapeDef *d = drawing_of(c); ShapeEdge e;
    if (!s || !d) return;
    if (!s->draw_open) { s->draw_start_x = s->draw_x; s->draw_start_y = s->draw_y; s->draw_open = 1; }
    memset(&e, 0, sizeof(e));
    e.x0 = s->draw_x; e.y0 = s->draw_y; e.x1 = x; e.y1 = y; e.is_curve = curve; e.cx = cx; e.cy = cy;
    e.fill1 = s->draw_fill; e.line = s->draw_line;
    add_edge(d, &e);
    s->draw_x = x; s->draw_y = y;
}
static Value mc_line_to(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = mc_of(self); double x, y;
    if (!c || argc < 2) return v_undef();
    x = v_num_of(a, argv[0]); if (a->vm->err) return v_undef();
    y = v_num_of(a, argv[1]); if (a->vm->err) return v_undef();
    draw_to(c, 0, 0, 0, tw(x), tw(y));
    return v_undef();
}
static Value mc_curve_to(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = mc_of(self); double v[4]; int i;
    if (!c || argc < 4) return v_undef();
    for (i = 0; i < 4; i++) { v[i] = v_num_of(a, argv[i]); if (a->vm->err) return v_undef(); }
    draw_to(c, 1, tw(v[0]), tw(v[1]), tw(v[2]), tw(v[3]));
    return v_undef();
}
static Value mc_clear(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = mc_of(self); A1Clip *s; (void)a; (void)argc; (void)argv;
    if (!c) return v_undef();
    s = a1_of(c);
    a1_drawing_free(c);
    if (s) { s->draw_fill = s->draw_line = 0; s->draw_open = 0; s->draw_x = s->draw_y = 0; }
    return v_undef();
}

/* ---------------------------------------------------------------- */
/* misc                                                             */

static Value mc_get_bytes_total(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = mc_of(self); (void)a; (void)argc; (void)argv;
    extern double avm1_clip_bytes_total(MovieClip *c);
    if (!c) return v_undef();
    return v_num(avm1_clip_bytes_total(c));
}
/* bytes loaded: everything once loaded, 0 for an error state (total -1) */
static Value mc_get_bytes_loaded(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = mc_of(self); double t; (void)a; (void)argc; (void)argv;
    extern double avm1_clip_bytes_total(MovieClip *c);
    if (!c) return v_undef();
    t = avm1_clip_bytes_total(c);
    if (c->movie && c->movie->incomplete && c->tl == &c->movie->root && c->movie->body) t = (double)(c->movie->bodylen + 8);   /* what actually arrived */
    return v_num(t > 0 ? t : 0);
}
static Value mc_get_swf_version(Act *a, AObj *self, int argc, Value *argv) { MovieClip *c = dobj_of(self); int v; (void)a; (void)argc; (void)argv; if (!c) return v_num(-1); v = c->movie ? c->movie->version : 0; return v_num(v > 0 ? v : -1); }
static Value mc_noop(Act *a, AObj *self, int argc, Value *argv) { (void)a; (void)self; (void)argc; (void)argv; return v_undef(); }
static Value mc_get_force_smoothing(Act *a, AObj *self, int argc, Value *argv) { (void)a; (void)argc; (void)argv; return mc_of(self) ? v_bool(0) : v_undef(); }

/* MovieClip.prototype.meth(m): 1 for "get", 2 for "post" (through m.toLowerCase()), else 0 */
static Value mc_meth(Act *a, AObj *self, int argc, Value *argv) {
    AObj *o; Value lc; AStr *s; (void)self;
    if (argc < 1 || argv[0].t == V_UNDEF || argv[0].t == V_NULL) return v_num(0);
    o = v_to_obj(a, argv[0]); if (!o || a->vm->err) return v_num(0);
    lc = o_call_method(a, o, str_intern(a->vm, "toLowerCase"), 0, NULL, ER_FUNCTION); if (a->vm->err) return v_undef();
    s = v_str_of(a, lc); if (a->vm->err) return v_undef();
    if (!strcmp(str_c(a->vm, s), "get")) return v_num(1);
    if (!strcmp(str_c(a->vm, s), "post")) return v_num(2);
    return v_num(0);
}
/* getURL / loadVariables / loadMovie always ask this.meth() for the method */
static int call_meth(Act *a, AObj *self, int argc, Value *argv, int k) {
    AStr *mn = str_intern(a->vm, "meth"); Value r, f; int m;
    if (o_get_opt(a, self, mn, 0, &f)) r = o_call_method(a, self, mn, argc > k ? 1 : 0, argc > k ? argv + k : NULL, ER_FUNCTION);
    else r = a->vm->err ? v_undef() : mc_meth(a, self, argc > k ? 1 : 0, argc > k ? argv + k : NULL);
    if (a->vm->err) return -1;
    m = v_i32(a, r); if (a->vm->err) return -1;
    return m == 1 || m == 2 ? m : 0;
}

static Value mc_get_url(Act *a, AObj *self, int argc, Value *argv) {
    extern int avm1_fscommand(Act *a, AStr *url, AStr *target);
    extern void avm1_navigate_locals(Act *a, AStr *url, AStr *target, int method);
    AStr *url, *win; int method = call_meth(a, self, argc, argv, 2);
    if (method < 0) return v_undef();
    if (argc < 1) return v_undef();
    url = v_str_of(a, argv[0]); if (a->vm->err) return v_undef();
    if (str_starts_c_ci(url, "fscommand:")) {
        win = v_str_of(a, arg(argc, argv, 1)); if (a->vm->err) return v_undef();
        avm1_fscommand(a, url, win);
        return v_undef();
    }
    win = argc > 1 ? v_str_of(a, argv[1]) : a->vm->s_empty;
    if (a->vm->err) return v_undef();
    avm1_navigate_locals(a, url, win, method);
    return v_undef();
}
static Value mc_load_movie(Act *a, AObj *self, int argc, Value *argv) {
    extern void avm1_load_movie_vars(Act *a, MovieClip *target, AStr *url, int method, AObj *vars);
    MovieClip *c = mc_of(self); AStr *url; int method;
    if (!c) return v_undef();
    method = call_meth(a, self, argc, argv, 1); if (method < 0) return v_undef();
    if (argc < 1) return v_undef();
    url = v_str_of(a, arg(argc, argv, 0)); if (a->vm->err) return v_undef();
    c = mc_of(self); if (!c) return v_undef();
    avm1_load_movie_vars(a, c, url, method, self);
    return v_undef();
}
static Value mc_unload_movie(Act *a, AObj *self, int argc, Value *argv) {
    extern void avm1_unload_movie(Act *a, MovieClip *target);
    MovieClip *c = mc_of(self); (void)argc; (void)argv;
    if (c) avm1_unload_movie(a, c);
    return v_undef();
}
static Value mc_load_variables(Act *a, AObj *self, int argc, Value *argv) {
    extern void avm1_load_vars_into_vars(Act *a, AObj *target, AStr *url, int method, AObj *vars);
    MovieClip *c = mc_of(self); AStr *url; int method;
    if (!c) return v_undef();
    method = call_meth(a, self, argc, argv, 1); if (method < 0) return v_undef();
    if (argc < 1) return v_undef();
    url = v_str_of(a, arg(argc, argv, 0)); if (a->vm->err) return v_undef();
    avm1_load_vars_into_vars(a, self, url, method, self);
    return v_undef();
}

/* property getters / setters */
/* tabIndex of movie clips and buttons: an i32 (strings and objects become i32::MIN), -1 unsets it */
int a1_set_tab_index_i32(Act *a, MovieClip *c, Value v) {
    A1Clip *s = c ? a1_of(c) : NULL;
    if (!s) return 1;
    if (v.t == V_UNDEF || v.t == V_NULL) { s->has_tab = 0; return 1; }
    if (v.t == V_BOOL || v.t == V_NUM) { s32 i = v_i32(a, v); if (a->vm->err) return 0; s->has_tab = i != -1; s->tab_index = i; return 1; }
    s->has_tab = 1; s->tab_index = (s32)0x80000000;
    return 1;
}
static Value mc_get_tab_index(Act *a, AObj *self, int argc, Value *argv) { MovieClip *c = mc_of(self); (void)a; (void)argc; (void)argv; if (!c || !c->a1 || !c->a1->has_tab) return v_undef(); return v_num(c->a1->tab_index); }
static Value mc_set_tab_index(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = mc_of(self);
    if (c) a1_set_tab_index_i32(a, c, arg(argc, argv, 0));
    return v_undef();
}
static Value mc_get_lock_root(Act *a, AObj *self, int argc, Value *argv) { MovieClip *c = mc_of(self); (void)a; (void)argc; (void)argv; if (!c) return v_undef(); return v_bool(c->a1 && c->a1->lock_root); }
static Value mc_set_lock_root(Act *a, AObj *self, int argc, Value *argv) { MovieClip *c = mc_of(self); A1Clip *s; if (!c || !(s = a1_of(c))) return v_undef(); s->lock_root = (u8)v_as_bool(arg(argc, argv, 0), a->swf_version); return v_undef(); }
static Value mc_get_cache_as_bitmap(Act *a, AObj *self, int argc, Value *argv) { MovieClip *c = mc_of(self); (void)a; (void)argc; (void)argv; if (!c) return v_undef(); return v_bool(c->a1 && c->a1->draw_has & 0x100); }
static Value mc_set_cache_as_bitmap(Act *a, AObj *self, int argc, Value *argv) { MovieClip *c = mc_of(self); A1Clip *s; if (!c || !(s = a1_of(c))) return v_undef(); if (v_as_bool(arg(argc, argv, 0), a->swf_version)) s->draw_has |= 0x100; else s->draw_has &= ~0x100; return v_undef(); }
static Value mc_get_opaque_bg(Act *a, AObj *self, int argc, Value *argv) { MovieClip *c = mc_of(self); (void)a; (void)argc; (void)argv; return c && c->self && c->self->has_opaque_bg ? v_num((double)c->self->opaque_bg) : v_undef(); }
static Value mc_set_opaque_bg(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = mc_of(self); Value v = argc > 0 ? argv[0] : v_undef(); u32 rgb;
    if (!c || !c->self) return v_undef();
    if (v.t == V_UNDEF || v.t == V_NULL) { c->self->has_opaque_bg = 0; return v_undef(); }
    rgb = v_u32(a, v); if (a->vm->err) return v_undef();
    c->self->has_opaque_bg = 1; c->self->opaque_bg = rgb & 0xFFFFFF;
    return v_undef();
}
/* object_to_rectangle: x, y, width and height as stored (local) properties */
static int obj_to_rect(Act *a, AObj *o, SWFRect *r) {
    static const char *names[4] = { "x", "y", "width", "height" };
    s32 v[4]; int i;
    for (i = 0; i < 4; i++) {
        Value x;
        if (!o_get_local_stored(a, o, str_intern(a->vm, names[i]), &x)) return 0;
        v[i] = v_i32(a, x); if (a->vm->err) return 0;
    }
    r->xmin = (s32)((u32)v[0] * 20u); r->ymin = (s32)((u32)v[1] * 20u);
    r->xmax = (s32)((u32)(v[0] + v[2]) * 20u); r->ymax = (s32)((u32)(v[1] + v[3]) * 20u);
    return 1;
}
static Value rect_value(Act *a, const SWFRect *r) {
    Value args[4];
    args[0] = v_num(r->xmin / 20.0); args[1] = v_num(r->ymin / 20.0);
    args[2] = v_num((r->xmax - r->xmin) / 20.0); args[3] = v_num((r->ymax - r->ymin) / 20.0);
    return a1_instantiate_as_script(a, "flash.geom.Rectangle", 4, args);
}
static Value mc_get_scroll_rect(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = mc_of(self); (void)argc; (void)argv;
    if (!c || !c->self || !c->self->has_scroll_rect) return v_undef();
    return rect_value(a, &c->self->scroll_rect);
}
static Value mc_set_scroll_rect(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = mc_of(self); Value v = arg(argc, argv, 0);
    if (!c || !c->self) return v_undef();
    if (v.t == V_OBJ) { SWFRect r; c->self->has_scroll_rect = 1; if (obj_to_rect(a, v.u.o, &r)) c->self->scroll_rect = r; }
    else c->self->has_scroll_rect = 0;
    return v_undef();
}
static Value mc_get_scale9(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = mc_of(self); (void)argc; (void)argv;
    if (!c || !c->self || !c->self->has_scale9) return v_undef();
    return rect_value(a, &c->self->scale9);
}
static Value mc_set_scale9(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = mc_of(self); Value v = arg(argc, argv, 0);
    if (!c || !c->self) return v_undef();
    if (v.t == V_OBJ) { SWFRect r; if (obj_to_rect(a, v.u.o, &r)) { c->self->scale9 = r; c->self->has_scale9 = r.xmin <= r.xmax && r.ymin <= r.ymax; } }
    else c->self->has_scale9 = 0;
    return v_undef();
}
static Value mc_get_undef(Act *a, AObj *self, int argc, Value *argv) { (void)a; (void)self; (void)argc; (void)argv; return v_undef(); }
static Value mc_get_filters(Act *a, AObj *self, int argc, Value *argv) {
    extern Value a1_get_filters(Act *a, MovieClip *c);
    MovieClip *c = o_display(self); (void)argc; (void)argv;
    if (!c) return v_undef();
    return a1_get_filters(a, c);
}
static Value mc_set_filters(Act *a, AObj *self, int argc, Value *argv) {
    extern void a1_set_filters(Act *a, MovieClip *c, Value v);
    MovieClip *c = o_display(self);
    if (c) a1_set_filters(a, c, arg(argc, argv, 0));
    return v_undef();
}
static Value mc_get_transform(Act *a, AObj *self, int argc, Value *argv) {
    extern Value a1_make_transform(Act *a, AObj *clip);
    MovieClip *c = mc_of(self); (void)argc; (void)argv;
    if (!c) return v_undef();
    return a1_make_transform(a, self);
}
static Value mc_set_transform(Act *a, AObj *self, int argc, Value *argv) {
    extern void a1_apply_transform(Act *a, MovieClip *c, Value t);
    MovieClip *c = mc_of(self);
    if (c) a1_apply_transform(a, c, arg(argc, argv, 0));
    return v_undef();
}
static const char *blend_names[] = { "normal", "normal", "layer", "multiply", "screen", "lighten", "darken", "difference", "add", "subtract", "invert", "alpha", "erase", "overlay", "hardlight", NULL };
static Value mc_get_blend_mode(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = mc_of(self); int b; (void)argc; (void)argv;
    if (!c || !c->self) return v_undef();
    b = c->self->blend;
    if (b < 0) return v_undef();                       /* set to an unknown mode (Flash 9) */
    if (b == 15) return v_str(a->vm->s_empty);         /* "shader" has no AVM1 name */
    if (b > 14) b = 0;
    return v_str(str_intern(a->vm, blend_names[b]));
}
static Value mc_set_blend_mode(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = mc_of(self); Value v = arg(argc, argv, 0);
    if (!c || !c->self) return v_undef();
    /* Value::as_blend_mode: undefined/null -> normal, numbers wrap to a byte, strings by name, others ignored */
    if (v.t == V_UNDEF || v.t == V_NULL) c->self->blend = 1;
    else if (v.t == V_NUM) {
        /* numbers wrap to a byte; Flash 9 takes any number (an unknown one reads as
           undefined), later players ignore unknown ones and know 15 (shader) */
        int i = (int)((u32)f64_i32(v.u.n) & 0xFF);
        if (i >= 1 && i <= 14) c->self->blend = i;
        else if (avm1_player_major() <= 9) c->self->blend = -1;
        else if (i == 15) c->self->blend = 15;
    }
    else if (v.t == V_STR) { int i; for (i = 1; blend_names[i]; i++) if (str_eq_c(v.u.s, blend_names[i])) { c->self->blend = i; break; } }
    return v_undef();
}

static Value mc_to_string(Act *a, AObj *self, int argc, Value *argv);

/* ---------------------------------------------------------------- */
/* class                                                            */

AObj *lib_movieclip(VM *vm, Env *env, AObj *object_proto) {
    AObj *proto, *c = lib_class(vm, env, NULL, NULL, object_proto, 0, &proto);
    u16 m = DE | DD;
    env->p.movieclip = proto;
    o_define_c(vm, proto, "useHandCursor", v_bool(1), DE);
    o_define_c(vm, proto, "enabled", v_bool(1), DE);
    defp(vm, env, proto, "tabIndex", mc_get_tab_index, mc_set_tab_index, DE | V6);
    defp(vm, env, proto, "_lockroot", mc_get_lock_root, mc_set_lock_root, DD | DE);
    defp(vm, env, proto, "cacheAsBitmap", mc_get_cache_as_bitmap, mc_set_cache_as_bitmap, DD | DE | V8);
    defp(vm, env, proto, "opaqueBackground", mc_get_opaque_bg, mc_set_opaque_bg, DD | DE | V8);
    defp(vm, env, proto, "scrollRect", mc_get_scroll_rect, mc_set_scroll_rect, DD | DE | V8);
    defp(vm, env, proto, "filters", mc_get_filters, mc_set_filters, DD | DE | V8);
    defp(vm, env, proto, "transform", mc_get_transform, mc_set_transform, DE | V8);
    defp(vm, env, proto, "blendMode", mc_get_blend_mode, mc_set_blend_mode, DD | DE | V8);
    defp(vm, env, proto, "scale9Grid", mc_get_scale9, mc_set_scale9, DD | DE | V8);
    defm(vm, env, proto, "getURL", mc_get_url, m);
    defm(vm, env, proto, "unloadMovie", mc_unload_movie, m);
    defm(vm, env, proto, "loadVariables", mc_load_variables, m);
    defm(vm, env, proto, "loadMovie", mc_load_movie, m);
    defm(vm, env, proto, "attachMovie", mc_attach_movie, m);
    defm(vm, env, proto, "swapDepths", mc_swap_depths, m);
    defm(vm, env, proto, "localToGlobal", mc_local_to_global, m);
    defm(vm, env, proto, "globalToLocal", mc_global_to_local, m);
    defm(vm, env, proto, "hitTest", mc_hit_test, m);
    defm(vm, env, proto, "getBounds", mc_get_bounds, m);
    defm(vm, env, proto, "getBytesTotal", mc_get_bytes_total, m);
    defm(vm, env, proto, "getBytesLoaded", mc_get_bytes_loaded, m);
    defm(vm, env, proto, "attachAudio", mc_noop, m | V6);
    defm(vm, env, proto, "getDepth", get_depth, m | RO | V6);
    defm(vm, env, proto, "setMask", mc_set_mask, m | V6);
    defm(vm, env, proto, "play", mc_play, m);
    defm(vm, env, proto, "stop", mc_stop, m);
    defm(vm, env, proto, "nextFrame", mc_next_frame, m);
    defm(vm, env, proto, "prevFrame", mc_prev_frame, m);
    defm(vm, env, proto, "gotoAndPlay", mc_goto_and_play, m);
    defm(vm, env, proto, "gotoAndStop", mc_goto_and_stop, m);
    defm(vm, env, proto, "duplicateMovieClip", mc_duplicate, m);
    defm(vm, env, proto, "removeMovieClip", mc_remove_movie_clip, m);
    defm(vm, env, proto, "startDrag", mc_start_drag, m);
    defm(vm, env, proto, "stopDrag", mc_stop_drag, m);
    defm(vm, env, proto, "getNextHighestDepth", mc_get_next_highest_depth, m | V7);
    defm(vm, env, proto, "getInstanceAtDepth", mc_get_instance_at_depth, m | V7);
    defm(vm, env, proto, "getSWFVersion", mc_get_swf_version, m);
    defm(vm, env, proto, "attachBitmap", mc_attach_bitmap, m | V8);
    defm(vm, env, proto, "getRect", mc_get_bounds, m | V8);
    defm(vm, env, proto, "createEmptyMovieClip", mc_create_empty, m | V6);
    defm(vm, env, proto, "beginFill", mc_begin_fill, m | V6);
    defm(vm, env, proto, "beginGradientFill", mc_begin_gradient_fill, m | V6);
    defm(vm, env, proto, "moveTo", mc_move_to, m | V6);
    defm(vm, env, proto, "lineTo", mc_line_to, m | V6);
    defm(vm, env, proto, "curveTo", mc_curve_to, m | V6);
    defm(vm, env, proto, "lineStyle", mc_line_style, m | V6);
    defm(vm, env, proto, "endFill", mc_end_fill, m | V6);
    defm(vm, env, proto, "clear", mc_clear, m | V6);
    defm(vm, env, proto, "lineGradientStyle", mc_line_gradient_style, m | V8);
    defm(vm, env, proto, "beginBitmapFill", mc_begin_bitmap_fill, m | V8);
    defm(vm, env, proto, "createTextField", mc_create_text_field, m);
    { extern Value a1_get_text_snapshot(Act *a, AObj *self, int argc, Value *argv); defm(vm, env, proto, "getTextSnapshot", a1_get_text_snapshot, m); }
    /* Flash also has these; Ruffle's global_proto_decls expectation (a SWF10 movie)
     * lacks them: they are left out of a player whose main movie is SWF9+ */
    {
        extern int avm1_root_pre9(VM *vm);
        if (avm1_root_pre9(vm)) {
            defm(vm, env, proto, "attachVideo", mc_noop, m | V6);
            defm(vm, env, proto, "beginMeshFill", mc_noop, m | V8);
            defp(vm, env, proto, "forceSmoothing", mc_get_force_smoothing, mc_noop, DD | DE | V8);
            defm(vm, env, proto, "meth", mc_meth, m);
        }
    }
    (void)mc_to_string;
    return c;
}

/* ASnative(900, n) / ASnative(901, n): the MovieClip natives (Gnash MovieClip_as.cpp ids) */
Value mc_native900(Act *a, AObj *self, int argc, Value *argv, int index) {
    static const NativeFn t[] = { mc_attach_movie, mc_swap_depths, mc_local_to_global, mc_global_to_local, mc_hit_test,
        mc_get_bounds, mc_get_bytes_total, mc_get_bytes_loaded, mc_noop, mc_noop, get_depth, mc_set_mask, mc_play, mc_stop,
        mc_next_frame, mc_prev_frame, mc_goto_and_play, mc_goto_and_stop, mc_duplicate, mc_remove_movie_clip,
        mc_start_drag, mc_stop_drag, mc_get_next_highest_depth, mc_get_instance_at_depth, mc_get_swf_version,
        mc_attach_bitmap, mc_get_bounds };
    if (index < 0 || index >= (int)(sizeof(t) / sizeof(t[0]))) return v_undef();
    return t[index](a, self, argc, argv);
}
Value mc_native901(Act *a, AObj *self, int argc, Value *argv, int index) {
    static const NativeFn t[] = { mc_create_empty, mc_begin_fill, mc_begin_gradient_fill, mc_move_to, mc_line_to,
        mc_curve_to, mc_line_style, mc_end_fill, mc_clear, mc_noop, mc_noop, mc_noop };
    if (index < 0 || index >= (int)(sizeof(t) / sizeof(t[0]))) return v_undef();
    return t[index](a, self, argc, argv);
}

static Value mc_to_string(Act *a, AObj *self, int argc, Value *argv) { (void)argc; (void)argv; return v_str(v_str_of(a, v_obj(self))); }

/* ---------------------------------------------------------------- */
/* Button                                                           */

static MovieClip *btn_of(AObj *self) { MovieClip *c = o_display(self); if (!c || clip_removed(c) || !c->a1 || c->a1->kind != 1) return NULL; return c; }
static Value btn_get_enabled_tab(Act *a, AObj *self, int argc, Value *argv) { MovieClip *c = btn_of(self); (void)a; (void)argc; (void)argv; if (!c || !c->a1->has_tab) return v_undef(); return v_num(c->a1->tab_index); }
static Value btn_set_tab(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = btn_of(self);
    if (c) a1_set_tab_index_i32(a, c, arg(argc, argv, 0));
    return v_undef();
}
/* Button.prototype.getDepth works on buttons only (Gnash MovieClip.as: undefined for a clip) */
static Value btn_get_depth(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = o_display(self);
    if (!c || !c->self || !c->self->ch || c->self->ch->type != CHAR_BUTTON) return v_undef();
    return get_depth(a, self, argc, argv);
}

AObj *lib_button(VM *vm, Env *env, AObj *object_proto) {
    AObj *proto, *c = lib_class(vm, env, NULL, NULL, object_proto, 1, &proto);
    env->p.button = proto;
    o_define_c(vm, proto, "useHandCursor", v_bool(1), 0);
    o_define_c(vm, proto, "enabled", v_bool(1), 0);
    /* Ruffle avm1/globals/button.rs PROTO_DECLS */
    defm(vm, env, proto, "getDepth", btn_get_depth, V6);
    defp(vm, env, proto, "scale9Grid", mc_get_undef, mc_noop, RO | V8);
    defp(vm, env, proto, "filters", mc_get_filters, mc_set_filters, RO | V8);
    defp(vm, env, proto, "cacheAsBitmap", mc_get_cache_as_bitmap, mc_set_cache_as_bitmap, RO | V8);
    defp(vm, env, proto, "blendMode", mc_get_blend_mode, mc_set_blend_mode, RO | V8);
    defp(vm, env, proto, "tabIndex", btn_get_enabled_tab, btn_set_tab, V6);
    return c;
}

/* the AS object of a button (Avm1Button::post_instantiation) */
void a1_button_post_instantiation(VM *vm, MovieClip *c) {
    extern void avm1_button_init(VM *vm, MovieClip *b);
    if (!c->obj) {
        Act a; Value ctor = v_undef(), proto = v_undef(); AObj *o, *co;
        act_init_nothing(&a, vm, c, "[Construct]");
        co = a1_resolve_class(&a, "Button");
        if (co) ctor = v_obj(co);
        if (!a1_class_proto(&a, co, &proto)) proto = v_undef();
        o = obj_bare(vm);
        if (!o) return;
        o->nk = NK_BUTTON; o->nat.clip = c;
        if (proto.t != V_UNDEF) o_define(vm, o, vm->s_proto, proto, PA_DONT_ENUM | PA_DONT_DELETE);
        c->obj = (struct AObj*)o;
        if (ctor.t == V_OBJ) { o_construct_on_existing(&a, ctor.u.o, o, 0, NULL); if (vm->err) vm->err = 0; }
    }
    avm1_button_init(vm, c);
}

/* GotoFrame2 */
void clip_goto_value(Act *a, MovieClip *c, Value frame, int play, int scene_offset) { a1_goto_frame(a, c, frame, play, scene_offset); }

/* Call: run the DoAction tags of a frame (1-based) now, in the clip's context */
void avm1_run_frame_actions(Act *a, MovieClip *c, int frame) {
    Timeline *tl = c->tl; int i, f = 1;
    if (!tl || frame < 1) return;
    for (i = 0; i < tl->ntags && f <= frame; i++) {
        const TagRef *t = &tl->tags[i];
        if (t->code == TAG_ShowFrame) { f++; continue; }
        if (f == frame && t->code == TAG_DoAction && t->len) {
            Act ch; int ret = 0;
            act_init_clip(&ch, a->vm, c);
            ch.id.parent = &a->id; ch.id.depth = (u16)(a->id.depth + 1);
            ch.id.function_count = a->id.function_count; ch.id.special_count = a->id.special_count;
            act_run_code(&ch, c->movie, t->data, (u32)t->len, &ret);
            if (a->vm->err) return;
        }
    }
}
