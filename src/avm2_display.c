/* avm2_display.c - flash.display: DisplayObject tree bound to the player's
 * display lists, Graphics, Stage, MovieClip timeline control, plus the
 * player-facing hooks (DoABC, SymbolClass, frame events, input). */
#include "avm2_flash_int.h"
#include "bmd.h"
#include "textfield.h"
#include "reader.h"
#include "image.h"
#include "render.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define PI 3.14159265358979323846

/* player.c helpers */
extern void player_free_dobj(DisplayObject *d);
extern void player_insert_dobj(MovieClip *c, DisplayObject *d);
void avm2_object_created(Player *p, DisplayObject *d);
extern MovieClip *player_make_clip(Player *p, MovieClip *parent, SWFChar *ch, DisplayObject *self);
extern void player_run_first_frame(MovieClip *c);
extern void player_resort(MovieClip *c);
extern void player_destroy_detached(DisplayObject *d);
extern void clip_bounds(void *vm, MovieClip *c, SWFRect *out);
extern int clip_hit_test_point(void *vm, MovieClip *c, double sx, double sy, int shape);
extern int clip_dobj_hit_test(void *vmp, DisplayObject *d, const SWFMatrix *pw, double sx, double sy);
extern void a2_report_uncaught(VM2 *vm);
extern void a2_timers_tick(VM2 *vm, double ms);

/* ---------------------------------------------------------------- */
/* state helpers                                                    */

A2Disp *a2_disp(A2Obj *o) {
    A2Disp *d;
    if (!o) return NULL;
    if (o->payload) return (A2Disp*)o->payload;
    d = (A2Disp*)calloc(1, sizeof(A2Disp));
    d->mouse_enabled = 1; d->mouse_children = 1; d->use_hand_cursor = 1; d->tab_enabled = 0; d->alpha_mul = 1; d->sx = d->sy = 1; d->btn_enabled = 1;
    d->color = 0; d->size = 240; d->bgcolor = 0xFFFFFF; d->selectable = 1; d->visible_override = -1; d->max_chars = 0;
    o->payload = d;
    return d;
}
void a2_free_payload(A2Obj *o) {
    extern void a2_regexp_free_payload(A2Obj *o); extern void a2_xml_free_payload(A2Obj *o); extern void a2_bitmapdata_free_payload(A2Obj *o);
    if (o->kind == OK_REGEXP) { a2_regexp_free_payload(o); return; }
    if (o->kind == OK_XML) { a2_xml_free_payload(o); return; }
    if (o->kind == OK_BITMAPDATA) { a2_bitmapdata_free_payload(o); return; }
    if (o->kind == OK_SOUND) { extern void a2_sound_free_payload(A2Obj *o); a2_sound_free_payload(o); return; }
    if (o->kind == OK_NETSTREAM) { extern void a2_netstream_free_payload(A2Obj *o); a2_netstream_free_payload(o); return; }
    if (o->kind == OK_DISPLAY) {
        A2Disp *d = (A2Disp*)o->payload;
        if (d) {
            if (d->drawing) shape_free(d->drawing);
            { extern void a2_textfield_free(A2Disp *st); a2_textfield_free(d); }
            if (d->owns) { DisplayObject *r = a2_dobj_of(o); if (r) { if (o->clip) o->clip->obj2 = NULL; r->obj2 = NULL; player_destroy_detached(r); o->clip = NULL; o->dobj = NULL; } }
            free(d);
        }
        o->payload = NULL;
        return;
    }
    if (o->kind == OK_TIMER || o->kind == OK_EVENT) return;
    { extern int a2_shader_free_payload(A2Obj *o); if (a2_shader_free_payload(o)) return; }
    { extern int a2_stage3d_free_payload(A2Obj *o); if (a2_stage3d_free_payload(o)) return; }
    if (o->payload && o->kind != OK_FUNCTION) { free(o->payload); o->payload = NULL; }
}
void a2_mark_payload(VM2 *vm, A2Obj *o) {
    extern void a2_regexp_mark_payload(VM2 *vm, A2Obj *o); extern void a2_xml_mark_payload(VM2 *vm, A2Obj *o);
    if (o->kind == OK_REGEXP) { a2_regexp_mark_payload(vm, o); return; }
    if (o->kind == OK_XML) { a2_xml_mark_payload(vm, o); return; }
    if (o->kind == OK_DISPLAY && o->payload) {
        A2Disp *d = (A2Disp*)o->payload; DisplayObject *r;
        a2_mark_obj(vm, d->graphics); a2_mark_obj(vm, d->mask_obj); if (d->text) d->text->gc.mark = 1; if (d->html_text) d->html_text->gc.mark = 1; if (d->font) d->font->gc.mark = 1; if (d->name) d->name->gc.mark = 1;
        a2_mark_obj(vm, d->default_format); a2_mark_obj(vm, d->bitmap_data); a2_mark_obj(vm, d->loader_info); a2_mark_obj(vm, d->transform_obj); a2_mark_v(vm, d->opaque_bg);
        a2_mark_obj(vm, d->up); a2_mark_obj(vm, d->over); a2_mark_obj(vm, d->down); a2_mark_obj(vm, d->hit);
        /* children on our own list keep their objects alive */
        r = a2_dobj_of(o);
        if (r && r->clip) { DisplayObject *k; for (k = r->clip->children; k; k = k->next) { if (k->obj2) a2_mark_obj(vm, (A2Obj*)k->obj2); if (k->clip && k->clip->obj2) a2_mark_obj(vm, (A2Obj*)k->clip->obj2); } }
        /* parent keeps us; we keep the parent (needed for events) */
        { A2Obj *p = o->parent_disp; if (p) a2_mark_obj(vm, p); }
    }
}

/* the z translation of a display object with a 3D transform (Ruffle Transform::tz) */
double a2_dobj_tz(DisplayObject *d) {
    A2Obj *o = (A2Obj*)(d->obj2 ? d->obj2 : d->clip ? d->clip->obj2 : NULL);
    return o && o->kind == OK_DISPLAY && a2_disp(o)->has_m3d ? a2_disp(o)->z : 0;
}
DisplayObject *a2_dobj_of(A2Obj *o) { if (!o) return NULL; if (o->dobj) return o->dobj; if (o->clip) return o->clip->self; return NULL; }
static MovieClip *clip_of(A2Obj *o) { DisplayObject *d; if (!o) return NULL; if (o->clip) return o->clip; d = o->dobj; return d ? d->clip : NULL; }

static A2Class *class_for_char(VM2 *vm, SWFChar *ch) {
    int i;
    if (ch) for (i = 0; i < vm->nsymbols; i++) if (vm->symbols[i].id == ch->id && (!ch->movie || vm->symbols[i].movie == ch->movie)) return vm->symbols[i].cls;
    if (!ch) return vm->c_sprite;
    switch (ch->type) {
    case CHAR_SPRITE: return vm->c_movieclip;
    case CHAR_SHAPE: return vm->c_shape;
    case CHAR_EDITTEXT: return vm->c_textfield;
    case CHAR_TEXT: return vm->c_statictext;
    case CHAR_BUTTON: return vm->c_simplebutton;
    case CHAR_MORPH: return vm->c_morphshape;
    case CHAR_BITMAP: return vm->c_bitmap;
    case CHAR_VIDEO: return vm->c_video;
    default: return vm->c_displayobject;
    }
}

/* binding object for a timeline-created object (no ctor run) */
static void bmp_cache_size(A2Obj *o);
A2Obj *a2_display_obj(VM2 *vm, MovieClip *clip, DisplayObject *d) {
    A2Obj *o; A2Class *cls;
    if (clip && clip->obj2) return (A2Obj*)clip->obj2;
    if (d && d->obj2) return (A2Obj*)d->obj2;
    if (d && d->clip && d->clip->obj2) return (A2Obj*)d->clip->obj2;
    if (clip && !d) d = clip->self;
    if (clip && ((!clip->parent && clip == vm->player->root) || (clip->movie && clip->tl == &clip->movie->root && clip->movie != vm->player->movie))) {
        int i; cls = vm->c_movieclip;
        for (i = 0; i < vm->nsymbols; i++) if (vm->symbols[i].id == 0 && vm->symbols[i].movie == clip->movie) cls = vm->symbols[i].cls;
        if (cls != vm->c_movieclip && !a2_is_subclass(cls, vm->c_sprite)) {
            char b[400]; A2Class *bad = cls;
            if (bad->abc && !bad->inited) a2_class_init(vm, bad);
            sprintf(b, "Class %.300s$ must inherit from Sprite to link to the root.", a2_class_name(vm, bad, 0)->s);
            if (!vm->throwing) { a2_throw_error(vm, "TypeError", b, 2023); a2_report_uncaught(vm); }
            cls = vm->c_movieclip;
            for (i = 0; i < vm->nsymbols; i++) if (vm->symbols[i].id == 0 && vm->symbols[i].cls == bad) vm->symbols[i].cls = vm->c_movieclip;
        }
    } else cls = class_for_char(vm, d ? d->ch : NULL);
    { A2Class *bdcls = NULL;
    /* a bitmap symbol linked to a BitmapData subclass: the timeline object is a Bitmap
       whose bitmapData is a new instance of that class (constructed with 1, 1 as Flash does) */
    if (vm->c_bitmapdata && a2_is_subclass(cls, vm->c_bitmapdata)) { bdcls = cls; cls = vm->c_bitmap; }
    if (cls->abc && !cls->inited) a2_class_init(vm, cls);
    o = a2_new(vm, cls); o->kind = OK_DISPLAY;
    if (bdcls && d && d->ch) { extern A2Obj *a2_bitmapdata_from_char_cls(VM2 *vm, SWFChar *ch, A2Class *cls); A2Obj *bd = a2_bitmapdata_from_char_cls(vm, d->ch, bdcls); if (bd) { a2_disp(o)->bitmap_data = bd; bmp_cache_size(o); } } }   /* the symbol's pixels in an instance of the linked class, no constructor call */
    if (d) { o->dobj = d; d->obj2 = o; if (d->clip) { o->clip = d->clip; d->clip->obj2 = o; } }
    else if (clip) { o->clip = clip; clip->obj2 = o; }
    a2_disp(o);
    if (o->clip && o->clip == vm->player->root) a2_disp(o)->is_root = 1;
    if (d && d->owner && (d->tl_inst || (!(d->owner->movie && d->owner->movie->has_abc) && d->depth >= 0 && d->depth < 16384))) a2_disp(o)->timeline_placed = 1;

    if (d && d->ch && d->ch->type == CHAR_EDITTEXT) a2_text_sync_from_char(vm, o);
    return o;
}

static A2Obj *root_obj(VM2 *vm);
A2Obj *a2_parent_of(VM2 *vm, A2Obj *o) {
    DisplayObject *d;
    if (!o || o->kind != OK_DISPLAY) return NULL;
    if (o->parent_disp) return o->parent_disp;
    if (a2_disp(o)->is_root) return vm->root_detached && o == root_obj(vm) ? NULL : vm->stage_obj;
    d = a2_dobj_of(o);
    if (d && d->owner) { A2Obj *po = a2_display_obj(vm, d->owner, NULL); return po; }
    /* SimpleButton states under construction: not inserted yet, but Flash lets them see the stage */
    if (vm->building_button && o != vm->building_button && !a2_disp(o)->is_stage) return vm->building_button;
    return NULL;
}
/* a SimpleButton state object that is not the displayed state (stage/root are null for it) */
int a2_state_hidden(VM2 *vm, A2Obj *o) {
    DisplayObject *d;
    if (!o->payload || !((A2Disp*)o->payload)->button_state) return 0;
    d = a2_dobj_of(o);
    return d && d->state_mask && d->owner && d->owner->self && !(d->state_mask & BTN_STATE_BIT(d->owner->self->btn_state));
}
int a2_on_stage(VM2 *vm, A2Obj *o) {
    int g = 0;
    while (o && g++ < 256) {
        if (o == vm->stage_obj) return 1;
        if (a2_state_hidden(vm, o)) return 0;
        o = a2_parent_of(vm, o);
    }
    return 0;
}

static void dispatch_added_tree(VM2 *vm, A2Obj *o) {
    DisplayObject *k, *next; DisplayObject *r = a2_dobj_of(o);
    if (a2_disp(o)->stage_notified) return;      /* already told (children placed before their parent reached the stage) */
    a2_disp(o)->stage_notified = 1;
    a2_dispatch_simple(vm, o, vm->s_addedToStage, 0);
    /* a button passes it on to its current state only */
    if (r && r->clip) for (k = r->clip->children; k; k = next) { next = k->next; if (k->state_mask && !(k->state_mask & BTN_STATE_BIT(r->btn_state))) continue; if (k->obj2 || (k->clip && k->clip->obj2)) { A2Obj *ko = a2_display_obj(vm, k->clip, k); dispatch_added_tree(vm, ko); } }
}
static void clear_stage_notified(VM2 *vm, A2Obj *o) {
    DisplayObject *k; DisplayObject *r = a2_dobj_of(o);
    a2_disp(o)->stage_notified = 0;
    if (r && r->clip) for (k = r->clip->children; k; k = k->next) if (k->obj2 || (k->clip && k->clip->obj2)) clear_stage_notified(vm, a2_display_obj(vm, k->clip, k));
}
static void dispatch_removed_tree(VM2 *vm, A2Obj *o) {
    DisplayObject *k, *next; DisplayObject *r = a2_dobj_of(o);
    if (!a2_disp(o)->stage_notified) return;
    a2_disp(o)->stage_notified = 0;
    if (vm->focus == o) { vm->focus = NULL; vm->player->focus = NULL; }   /* leaving the stage drops the focus */
    a2_dispatch_simple(vm, o, vm->s_removedFromStage, 0);
    if (r && r->clip) for (k = r->clip->children; k; k = next) { next = k->next; if (k->obj2 || (k->clip && k->clip->obj2)) dispatch_removed_tree(vm, a2_display_obj(vm, k->clip, k)); }
}

/* run the constructor for a bound timeline object */
static void construct_pending(VM2 *vm);
static void construct_pending_under(VM2 *vm, MovieClip *c);
static void construct_pending_ex(VM2 *vm, MovieClip *c, MovieClip *push);
static int owned_under(DisplayObject *d, MovieClip *c);
static void build_button_states(VM2 *vm, A2Obj *btn, DisplayObject *bd);
static void construct_pending(VM2 *vm);
static void run_pending_frame_scripts(VM2 *vm, MovieClip *c);
static int hdr_frames(MovieClip *c);
void a2_orphan_add(VM2 *vm, A2Obj *o, int timeline_removed);
static void orphans_frame_scripts(VM2 *vm);
static void orphans_cleanup(VM2 *vm);
/* number of MovieClips making up the up state (Flash runs one extra frame per such clip) */
static int button_has_mc_state(VM2 *vm, A2Obj *btn) {
    A2Obj *up = a2_disp(btn)->up; MovieClip *uc; DisplayObject *k; int n = 0;
    if (!up || !vm->c_movieclip) return 0;
    if (up->cls && a2_is_subclass(up->cls, vm->c_movieclip)) return 1;
    uc = up->clip;
    if (up->cls && vm->c_sprite && up->cls == vm->c_sprite && uc)   /* a state container: look at its children */
        for (k = uc->children; k; k = k->next) { A2Obj *co = (A2Obj*)k->obj2; if (co && co->cls && a2_is_subclass(co->cls, vm->c_movieclip)) n++; }
    return n;
}
static void run_state_frame_scripts(VM2 *vm, MovieClip *c);
static MovieClip *fo_a1_focus(VM2 *vm);
static int g_btn_frame_follows;   /* build_button_states: a whole frame runs right after */
static int g_swf9_deferred;       /* constructing SWF9 children a goto in frameConstructed left for the script pass */
static MovieClip *g_btn_first_scripts;   /* ...whose script pass starts with this state clip */
static void construct_pending_under(VM2 *vm, MovieClip *c);
/* timeline instance name -> property on the parent (slot or dynamic) */
static void set_parent_field(VM2 *vm, A2Obj *o, DisplayObject *d) {
    if (d && d->name[0] && strncmp(d->name, "instance", 8) != 0) {
        A2Obj *par = a2_parent_of(vm, o);
        if (par && par != vm->stage_obj) {
            A2Str *nm = a2_str(vm, d->name); A2Name n; n.name = nm; n.nss = &vm->ns_public; n.nns = 1; n.any_ns = 1; n.attr = 0;
            if (par->cls && a2_find_trait(&par->cls->itraits, nm, &n)) a2_init(vm, v2_obj(par), &n, v2_obj(o));
            else if (par->cls && par->cls->is_dynamic) a2_dyn_set(vm, par, nm, v2_obj(o));
            if (vm->throwing) { vm->throwing = 0; }
        }
    }
}
void a2_construct_display(VM2 *vm, MovieClip *clip, DisplayObject *d) {
    A2Obj *o = a2_display_obj(vm, clip, d); A2Disp *st = a2_disp(o); int is_btn = d && d->ch && d->ch->type == CHAR_BUTTON && d->clip && !d->clip->children;
    if (o->ival & 1) return;             /* constructed */
    o->ival |= 1;
    (void)st;
    if (vm->player->debug) fprintf(stderr, "[avm2] construct %s %p (char %d)\n", o->cls->name->s, (void*)o, d && d->ch ? d->ch->id : -1);
    if (vm->depth > 800) return;
    if (is_btn) {
        set_parent_field(vm, o, d);   /* the unconstructed button is visible as parent.<name> */
        g_btn_frame_follows = 1; g_btn_first_scripts = NULL; build_button_states(vm, o, d); g_btn_frame_follows = 0;
        /* SWF10+ buttons with a MovieClip state: a whole frame (construct, frameConstructed, frame scripts,
           exitFrame) runs before the button's constructor (Ruffle's weird_framescript_order) */
        if (d->ch->movie && d->ch->movie->version > 9 && vm->root_constructed) {
          int nrun = button_has_mc_state(vm, o);
          while (nrun-- > 0) {
            construct_pending(vm);
            a2_broadcast(vm, vm->s_frameConstructed);
            if (vm->throwing) a2_report_uncaught(vm);
            {   /* a timeline whose constructor has not registered its frame scripts yet keeps its pending frame */
                MovieClip *r = vm->player->root; A2Obj *ro = r ? (A2Obj*)r->obj2 : NULL; int keep = r && r->frame_changed && ro && !ro->nframe_scripts;
                if (g_btn_first_scripts) { MovieClip *fc = g_btn_first_scripts; g_btn_first_scripts = NULL; run_state_frame_scripts(vm, fc); }
                run_pending_frame_scripts(vm, vm->player->root);
                if (keep) r->frame_changed = 1;
            }
            a2_broadcast(vm, vm->s_exitFrame);
            if (vm->throwing) a2_report_uncaught(vm);
          }
          if (g_btn_first_scripts) { MovieClip *fc = g_btn_first_scripts; g_btn_first_scripts = NULL; run_state_frame_scripts(vm, fc); }
        }
    }
    o->ival |= 2;   /* constructor running */
    if (g_swf9_deferred && clip) clip->born_fs = vm->frame_no + 1;   /* its frame was entered inside the script pass */
    a2_run_ctor(vm, o->cls, o, 0, NULL);
    o->ival &= ~2;
    if (vm->throwing) a2_report_uncaught(vm);
    if (a2_parent_of(vm, o) && (!d || d->owner || o->parent_disp)) { a2_dispatch_simple(vm, o, vm->s_added, 1); if (a2_on_stage(vm, o)) dispatch_added_tree(vm, o); }
    /* then the timeline instance name -> property on the parent (Ruffle on_construction_complete;
       buttons set it before their states and constructor) */
    if (!is_btn) set_parent_field(vm, o, d);
}

/* script-created display object: fresh record + clip, owned by the object */
A2Obj *a2_new_display(VM2 *vm, A2Class *cls) {
    DisplayObject *nd = (DisplayObject*)calloc(1, sizeof(DisplayObject)); A2Obj *o; A2Disp *st; SWFChar *ch = NULL; int i;
    o = a2_new(vm, cls); o->kind = OK_DISPLAY;
    for (i = 0; i < vm->nsymbols; i++) if (vm->symbols[i].cls == cls || (cls->super && a2_is_subclass(cls, vm->symbols[i].cls) && vm->symbols[i].id != 0 && !ch)) { ch = swf_get_char(vm->symbols[i].movie ? vm->symbols[i].movie : vm->player->movie, vm->symbols[i].id); if (vm->symbols[i].cls == cls) break; }
    nd->depth = -1; nd->char_id = ch ? ch->id : -1; nd->ch = ch; nd->visible = 1; nd->placed_by_script = 1;
    swf_matrix_identity(&nd->matrix); swf_cxform_identity(&nd->cxform);
    nd->clip = player_make_clip(vm->player, NULL, (ch && ch->type == CHAR_SPRITE) ? ch : NULL, nd);
    nd->obj2 = o; o->dobj = nd; o->clip = nd->clip; if (nd->clip) nd->clip->obj2 = o;
    st = a2_disp(o); st->owns = 1;
    o->ival |= 1;
    { char b[32]; sprintf(b, "instance%d", ++vm->instance_counter); st->name = a2_str(vm, b); }
    if (ch && ch->type == CHAR_SPRITE && nd->clip) { player_run_first_frame(nd->clip); nd->clip->skip_enter = 1; if (vm->player->avm2) avm2_frame_shown(vm->player, nd->clip); }   /* script-created clips skip their next enterFrame (they lag a frame behind timeline objects) */
    if (ch && ch->type == CHAR_EDITTEXT) a2_text_sync_from_char(vm, o);
    if (ch && ch->type == CHAR_BUTTON && nd->clip && cls && vm->c_simplebutton && a2_is_subclass(cls, vm->c_simplebutton)) build_button_states(vm, o, nd);   /* `new` of a button symbol class */
    a2_orphan_add(vm, o, 0);
    return o;
}

/* ---------------------------------------------------------------- */
/* transform                                                        */

static void world_matrix_ex(A2Obj *o, SWFMatrix *out, int own_scroll);
void a2_world_matrix(A2Obj *o, SWFMatrix *out) { world_matrix_ex(o, out, 1); }
/* the object's own space: its scrollRect window, not its content */
void a2_world_matrix_noscroll(A2Obj *o, SWFMatrix *out) { world_matrix_ex(o, out, 0); }
static void world_matrix_ex(A2Obj *o, SWFMatrix *out, int own_scroll) {
    SWFMatrix m; int g = 0;
    swf_matrix_identity(out);
    while (o && g++ < 256) {
        DisplayObject *d = a2_dobj_of(o);
        if (d) { SWFMatrix cm; if (g == 1 && !own_scroll) cm = d->matrix; else dobj_content_matrix(d, &cm); swf_matrix_mul(&m, &cm, out); *out = m; }
        /* stage children render inside the root's record list, so follow the record owner (root) rather than the Stage */
        if (o->payload && ((A2Disp*)o->payload)->stage_child && d && d->owner && d->owner->obj2) o = (A2Obj*)d->owner->obj2;
        else o = o->parent_disp ? o->parent_disp : (d && d->owner && d->owner->obj2 ? (A2Obj*)d->owner->obj2 : NULL);
    }
}
/* the same in Flash's arithmetic (f32 scale/skew, integer twips); own_scroll: include the object's own scrollRect offset */
typedef struct RMat2 { float a, b, c, d; s32 tx, ty; } RMat2;
extern void rm_from(const SWFMatrix *m, void *o); extern void rm_identity(void *o); extern void rm_mul(const void *x, const void *y, void *o);
void a2_world_rmat(A2Obj *o, int own_scroll, void *outp) {
    RMat2 *out = (RMat2*)outp, cm, t; int g = 0;
    rm_identity(out);
    while (o && g++ < 256) {
        DisplayObject *d = a2_dobj_of(o);
        if (d) { SWFMatrix sm; if (g == 1 && (!own_scroll || (d->ch && d->ch->type == CHAR_BUTTON))) sm = d->matrix; else dobj_content_matrix(d, &sm);   /* (a SimpleButton's own bounds space ignores its scrollRect) */ rm_from(&sm, &cm); rm_mul(&cm, out, &t); *out = t; }
        if (o->payload && ((A2Disp*)o->payload)->stage_child && d && d->owner && d->owner->obj2) o = (A2Obj*)d->owner->obj2;
        else o = o->parent_disp ? o->parent_disp : (d && d->owner && d->owner->obj2 ? (A2Obj*)d->owner->obj2 : NULL);
    }
}
/* Flash keeps scaleX/scaleY/rotation plus the y-axis angle (skew) as the authoritative transform:
 *   a = cos(rx)*sx  b = sin(rx)*sx   c = -sin(ry)*sy  d = cos(ry)*sy      (rx = rotation, ry = rx + skew) */
static void sync_xform_from_matrix(A2Obj *o) {
    A2Disp *st = a2_disp(o); DisplayObject *d = a2_dobj_of(o);
    if (st->has_xform || !d) return;
    st->sx = sqrt(d->matrix.sx * d->matrix.sx + d->matrix.r0 * d->matrix.r0);
    st->sy = sqrt(d->matrix.r1 * d->matrix.r1 + d->matrix.sy * d->matrix.sy);
    st->rot = atan2(d->matrix.r0, d->matrix.sx) * 180.0 / PI;
    st->roty = atan2(-d->matrix.r1, d->matrix.sy) * 180.0 / PI;
    /* a negative determinant is expressed as a negative scaleX when possible */
    if (d->matrix.sx * d->matrix.sy - d->matrix.r0 * d->matrix.r1 < 0 && d->matrix.r0 == 0 && d->matrix.r1 == 0) { st->sx = -st->sx; st->rot = 0; }
    st->has_xform = 1;
}
/* sin/cos of an angle in degrees, exact at multiples of 90 */
static void sincos_deg(double deg, double *sn, double *cs) {
    double q = deg / 90.0;
    if (q == floor(q)) { int k = (int)fmod(q, 4.0); if (k < 0) k += 4; *sn = k == 1 ? 1 : k == 3 ? -1 : 0; *cs = k == 0 ? 1 : k == 2 ? -1 : 0; return; }
    *sn = sin(deg * PI / 180.0); *cs = cos(deg * PI / 180.0);
}
void a2_set_matrix_from_props(A2Obj *o) {
    A2Disp *st = a2_disp(o); DisplayObject *d = a2_dobj_of(o); double rx, ry, sx, sy;
    if (!d) return;
    rx = (st->rot != st->rot) ? 0 : st->rot;
    ry = (st->roty != st->roty) ? 0 : st->roty;
    sx = st->sx != st->sx ? 0 : st->sx; sy = st->sy != st->sy ? 0 : st->sy;
    { double cx, sxn, cy, syn; sincos_deg(rx, &sxn, &cx); sincos_deg(ry, &syn, &cy);
      d->matrix.sx = cx * sx; d->matrix.r0 = sxn * sx;
      d->matrix.r1 = -syn * sy; d->matrix.sy = cy * sy; }
}
void a2_local_bounds(VM2 *vm, A2Obj *o, SWFRect *r) {
    /* bounds in the object's own coordinate space (twips) */
    DisplayObject *d = a2_dobj_of(o); MovieClip *c = clip_of(o);
    r->xmin = r->ymin = r->xmax = r->ymax = 0;
    if (d && d->ch && d->ch->type == CHAR_VIDEO) { extern int video_bounds(SWFChar *ch, SWFRect *out); video_bounds(d->ch, r); return; }
    if (c) { SWFRect b; SWFMatrix saved; DisplayObject *s = c->self; if (s) { saved = s->matrix; swf_matrix_identity(&s->matrix); } clip_bounds(vm->player->avm, c, &b); if (s) s->matrix = saved; *r = b; return; }
    if (d && d->ch) {
        if (d->ch->type == CHAR_SHAPE || d->ch->type == CHAR_TEXT || d->ch->type == CHAR_EDITTEXT || d->ch->type == CHAR_MORPH) { extern int dg_char_bounds(SWFChar *ch, SWFRect *out); dg_char_bounds(d->ch, r); }
        else if (d->ch->type == CHAR_BITMAP) { Bitmap *b = image_get(d->ch->movie, d->ch->id); if (b) { r->xmax = b->w * 20; r->ymax = b->h * 20; } }
        else if (d->ch->type == CHAR_VIDEO) { extern int video_bounds(SWFChar *ch, SWFRect *out); video_bounds(d->ch, r); }
    }
}
static void bounds_in_parent(VM2 *vm, A2Obj *o, double *x, double *y, double *w, double *h) {
    SWFRect r; DisplayObject *d = a2_dobj_of(o); double xs[4], ys[4]; int i; double mnx, mny, mxx, mxy;
    a2_local_bounds(vm, o, &r);
    if (!d) { *x = *y = *w = *h = 0; return; }
    {   /* Flash's matrix arithmetic (f32 scale, integer twips); scales beyond 32768 saturate */
        extern void rm_from(const SWFMatrix *m, void *o); extern void rm_apply(const void *m, s32 x, s32 y, s32 *ox, s32 *oy);
        struct { float a, b, c, d; s32 tx, ty; } rm; s32 px[4], py[4];
        rm_from(&d->matrix, &rm);
#define RM_SAT(v) do { if ((v) > 32768.0f) (v) = 32768.0f; else if ((v) < -32768.0f) (v) = -32768.0f; } while (0)
        RM_SAT(rm.a); RM_SAT(rm.b); RM_SAT(rm.c); RM_SAT(rm.d);
#undef RM_SAT
        rm_apply(&rm, r.xmin, r.ymin, &px[0], &py[0]); rm_apply(&rm, r.xmax, r.ymin, &px[1], &py[1]);
        rm_apply(&rm, r.xmin, r.ymax, &px[2], &py[2]); rm_apply(&rm, r.xmax, r.ymax, &px[3], &py[3]);
        for (i = 0; i < 4; i++) { xs[i] = px[i]; ys[i] = py[i]; }
    }
    mnx = mxx = xs[0]; mny = mxy = ys[0];
    for (i = 1; i < 4; i++) { if (xs[i] < mnx) mnx = xs[i]; if (xs[i] > mxx) mxx = xs[i]; if (ys[i] < mny) mny = ys[i]; if (ys[i] > mxy) mxy = ys[i]; }
    *x = mnx / 20; *y = mny / 20; *w = (mxx - mnx) / 20; *h = (mxy - mny) / 20;
}

/* ---------------------------------------------------------------- */
/* DisplayObject properties                                         */

static A2Obj *dself(V2 self) { return (self.t == T_OBJ && self.u.o->kind == OK_DISPLAY) ? self.u.o : NULL; }
#define DOBJ(d, o) DisplayObject *d = a2_dobj_of(o)

A2Str *a2_display_name(VM2 *vm, A2Obj *o) {
    A2Disp *st = a2_disp(o); DOBJ(d, o); char b[48];
    if (st->name) return st->name;
    if (d && d->name[0]) return a2_str(vm, d->name);
    if (st->is_root) return a2_str(vm, "root1");
    sprintf(b, "instance%d", ++vm->instance_counter); st->name = a2_str(vm, b); return st->name;
}
static V2 do_name_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); (void)argc; (void)argv; if (o && a2_disp(o)->is_stage) return v2_null(); return o ? v2_str(a2_display_name(vm, o)) : v2_null(); }
static V2 do_name_set(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); A2Str *s = STR(0); DOBJ(d, o); if (!o) return v2_undef(); if ((d && d->owner && !a2_disp(o)->owns && (d->tl_inst || (!(d->owner->movie && d->owner->movie->has_abc) && d->depth < 16384 && d->depth >= 0))) || a2_disp(o)->timeline_placed || a2_disp(o)->is_root || a2_disp(o)->is_movie_root) { a2_throw_error(vm, "flash.errors::IllegalOperationError", "The name property of a Timeline-placed object cannot be modified.", 2078); return v2_undef(); } a2_disp(o)->name = s; if (d) { strncpy(d->name, s->s, sizeof(d->name) - 1); d->name[sizeof(d->name) - 1] = 0; } return v2_undef(); }
/* a text field's x/y include its bounds offset (scaled), like Flash */
/* a transform set by script: PlaceObject tags no longer move the object (Ruffle transformed_by_script) */
static void mark_script_moved(A2Obj *o) { DisplayObject *d = a2_dobj_of(o); if (d) d->script_moved = 1; }
static double text_off(VM2 *vm, A2Obj *o, DisplayObject *d, int vert) {
    A2Disp *st = o ? a2_disp(o) : NULL; double sc;
    if (!st || !st->is_text || !st->tfs || !d) return 0;
    tfs_apply_autosize(vm->player, st->tfs);
    sc = vert ? sqrt(d->matrix.sy * d->matrix.sy + d->matrix.r1 * d->matrix.r1) : sqrt(d->matrix.sx * d->matrix.sx + d->matrix.r0 * d->matrix.r0);
    return floor((vert ? st->tfs->ymin : st->tfs->xmin) * sc + 0.5);
}
static V2 do_x_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); DOBJ(d, o); UNUSED; return v2_num(d ? (d->matrix.tx + text_off(vm, o, d, 0)) / 20.0 : 0); }
static double to_twips(double v) { double t = v * 20.0; t = t < 0 ? ceil(t) : floor(t); if (t > 2147483647.0) t = 2147483647.0; if (t < -2147483648.0) t = -2147483648.0; return t; }
static V2 do_x_set(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); DOBJ(d, o); double v = NUM(0); if (v != v) v = 0; if (d) { double off = text_off(vm, o, d, 0); d->matrix.tx = to_twips(v) - off; d->script_moved = 1; } return v2_undef(); }
static V2 do_y_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); DOBJ(d, o); UNUSED; return v2_num(d ? (d->matrix.ty + text_off(vm, o, d, 1)) / 20.0 : 0); }
static V2 do_y_set(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); DOBJ(d, o); double v = NUM(0); if (v != v) v = 0; if (d) { double off = text_off(vm, o, d, 1); d->matrix.ty = to_twips(v) - off; d->script_moved = 1; } return v2_undef(); }
static V2 do_scaleX_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); UNUSED; if (!o) return v2_num(1); sync_xform_from_matrix(o); return v2_num(a2_disp(o)->sx); }
static V2 do_scaleX_set(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); double v = NUM(0); if (!o) return v2_undef(); mark_script_moved(o); sync_xform_from_matrix(o); a2_disp(o)->sx = v; a2_set_matrix_from_props(o); return v2_undef(); }
static V2 do_scaleY_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); UNUSED; if (!o) return v2_num(1); sync_xform_from_matrix(o); return v2_num(a2_disp(o)->sy); }
static V2 do_scaleY_set(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); double v = NUM(0); if (!o) return v2_undef(); mark_script_moved(o); sync_xform_from_matrix(o); a2_disp(o)->sy = v; a2_set_matrix_from_props(o); return v2_undef(); }
static V2 do_rotation_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); UNUSED; if (!o) return v2_num(0); sync_xform_from_matrix(o); return v2_num(a2_disp(o)->rot); }
static V2 do_rotation_set(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); double v = NUM(0); if (!o) return v2_undef(); mark_script_moved(o); sync_xform_from_matrix(o); if (v != v) { a2_disp(o)->rot = v; return v2_undef(); } v = fmod(v, 360); if (v > 180) v -= 360; if (v < -180) v += 360; { double old = a2_disp(o)->rot; if (old != old) old = 0; a2_disp(o)->roty += v - old; } a2_disp(o)->rot = v; a2_set_matrix_from_props(o); return v2_undef(); }
/* the Stage's width/height measure its children, not the stage size */
static int cc_count(VM2 *vm, A2Obj *p); static A2Obj *cc_at(VM2 *vm, A2Obj *p, int i);
static void stage_content_size(VM2 *vm, double *w, double *h) {
    int n = cc_count(vm, vm->stage_obj), i, any = 0; double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (i = 0; i < n; i++) { A2Obj *c = cc_at(vm, vm->stage_obj, i); double x, y, cw, ch; if (!c) continue; bounds_in_parent(vm, c, &x, &y, &cw, &ch); if (cw == 0 && ch == 0) continue;
        if (!any) { x0 = x; y0 = y; x1 = x + cw; y1 = y + ch; any = 1; } else { if (x < x0) x0 = x; if (y < y0) y0 = y; if (x + cw > x1) x1 = x + cw; if (y + ch > y1) y1 = y + ch; } }
    *w = any ? (floor(x1 * 20 + 0.5) - floor(x0 * 20 + 0.5)) / 20 : 0; *h = any ? (floor(y1 * 20 + 0.5) - floor(y0 * 20 + 0.5)) / 20 : 0;
}
static V2 do_width_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); double x, y, w, h; (void)argc; (void)argv; if (!o) return v2_num(0); if (a2_disp(o)->is_stage) { stage_content_size(vm, &w, &h); return v2_num(w); } bounds_in_parent(vm, o, &x, &y, &w, &h); return v2_num(w); }
static V2 do_height_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); double x, y, w, h; (void)argc; (void)argv; if (!o) return v2_num(0); if (a2_disp(o)->is_stage) { stage_content_size(vm, &w, &h); return v2_num(h); } bounds_in_parent(vm, o, &x, &y, &w, &h); return v2_num(h); }
/* width / height setters (Ruffle's set_width / set_height: rotated objects scale both axes) */
static void set_wh(VM2 *vm, A2Obj *o, double v, int is_h) {
    SWFRect r; double ow, oh, ar, tx, ty, psx, psy, rot, cs, sn, nx, ny; A2Disp *st;
    mark_script_moved(o);
    a2_local_bounds(vm, o, &r);
    ow = (r.xmax - r.xmin) / 20.0; oh = (r.ymax - r.ymin) / 20.0;
    /* Flash's old size comes out one ulp low (200/65 -> 3.0769230769230775) */
    ow *= 1.0 - 1.0 / 9007199254740992.0; oh *= 1.0 - 1.0 / 9007199254740992.0;
    sync_xform_from_matrix(o); st = a2_disp(o);
    psx = st->sx; psy = st->sy; rot = st->rot * 3.14159265358979323846 / 180.0;
    cs = fabs(cos(rot)); sn = fabs(sin(rot));
    if (!is_h) {
        ar = oh / ow;
        if (ow != 0) { tx = v / ow; ty = v / oh; } else tx = ty = 0;
        nx = ar * (cs * tx + sn * ty) / ((cs + ar * sn) * (ar * cs + sn));
        ny = (sn * psx + ar * cs * psy) / (ar * cs + sn);
    } else {
        ar = ow / oh;
        if (oh != 0) { tx = v / ow; ty = v / oh; } else tx = ty = 0;
        nx = (ar * cs * psx + sn * psy) / (ar * cs + sn);
        ny = ar * (sn * tx + cs * ty) / ((cs + ar * sn) * (ar * cs + sn));
    }
    if (nx != nx || nx > 1e308 || nx < -1e308) nx = 0;
    if (ny != ny || ny > 1e308 || ny < -1e308) ny = 0;
    st->sx = nx; st->sy = ny; a2_set_matrix_from_props(o);
}
static V2 do_width_set(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); double v = NUM(0); if (!o || v != v || v < 0) return v2_undef(); set_wh(vm, o, v, 0); return v2_undef(); }
static V2 do_height_set(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); double v = NUM(0); if (!o || v != v || v < 0) return v2_undef(); set_wh(vm, o, v, 1); return v2_undef(); }
static V2 do_alpha_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); DOBJ(d, o); UNUSED; if (o && a2_disp(o)->has_alpha) return v2_num(a2_disp(o)->alpha); return v2_num(d ? d->cxform.ma / 256.0 : 1); }
static V2 do_alpha_set(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); DOBJ(d, o); double v = NUM(0); if (o) { a2_disp(o)->alpha = v; a2_disp(o)->has_alpha = 1; } if (d) { double m = v * 256; if (m != m) m = 0; if (m > 32767) m = 32767; if (m < -32768) m = -32768; d->cxform.ma = (s16)m; d->script_moved = 1; } return v2_undef(); }   /* Fixed8 multiplier, unclamped (Ruffle) */
static V2 do_visible_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); DOBJ(d, o); UNUSED; return v2_bool(d ? d->visible : 1); }
static V2 do_visible_set(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); DOBJ(d, o); if (d) d->visible = BOOL(0); return v2_undef(); }
static V2 do_parent_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self), *p; DisplayObject *d; (void)argc; (void)argv; if (!o) return v2_null(); if (a2_disp(o)->button_state) return v2_null(); /* SimpleButton states have no parent */ d = a2_dobj_of(o); if (vm->building_button && d && !d->owner && !o->parent_disp && !a2_disp(o)->is_root) return v2_null(); p = a2_parent_of(vm, o); return p ? v2_obj(p) : v2_null(); }
static V2 do_stage_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); (void)argc; (void)argv; return (o && a2_on_stage(vm, o)) ? v2_obj(vm->stage_obj) : v2_null(); }
static V2 do_root_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); int g = 0; (void)argc; (void)argv; if (!o) return v2_null(); if (a2_disp(o)->is_stage) return self; while (o && g++ < 256) { A2Obj *p; if (a2_state_hidden(vm, o)) return v2_null(); if (a2_disp(o)->is_movie_root || a2_disp(o)->is_root) return v2_obj(o); p = a2_parent_of(vm, o); if (!p) return v2_null(); if (p == vm->stage_obj) return v2_obj(o); o = p; } return v2_null(); }
static V2 do_mouseX_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); SWFMatrix w; double a, b, c, d, e, f, sx, sy; (void)argc; (void)argv; if (!o) return v2_num(0); a2_world_matrix(o, &w); sx = vm->mouse_x * 20.0 / vm->player->zoom; sy = vm->mouse_y * 20.0 / vm->player->zoom; if (!matrix_invert(&w, &a, &b, &c, &d, &e, &f)) return v2_num(0); return v2_num((a * sx + b * sy + e) / 20.0); }
static V2 do_mouseY_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); SWFMatrix w; double a, b, c, d, e, f, sx, sy; (void)argc; (void)argv; if (!o) return v2_num(0); a2_world_matrix(o, &w); sx = vm->mouse_x * 20.0 / vm->player->zoom; sy = vm->mouse_y * 20.0 / vm->player->zoom; if (!matrix_invert(&w, &a, &b, &c, &d, &e, &f)) return v2_num(0); return v2_num((c * sx + d * sy + f) / 20.0); }
static V2 do_mask_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); UNUSED; return o && a2_disp(o)->mask_obj ? v2_obj(a2_disp(o)->mask_obj) : v2_null(); }
static V2 do_mask_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = dself(self), *m = dself(ARG(0)); A2Disp *st; if (!o) return v2_undef(); st = a2_disp(o);
    if (st->mask_obj) a2_disp(st->mask_obj)->is_mask = 0;
    st->mask_obj = m; st->mask = m ? clip_of(m) : NULL;
    if (m) a2_disp(m)->is_mask = 1;
    /* a script mask replaces timeline masking: both lose their clip depth (Ruffle set_mask) */
    { DisplayObject *d = a2_dobj_of(o); if (d) d->clip_depth = 0; }
    if (m) { DisplayObject *d = a2_dobj_of(m); if (d) d->clip_depth = 0; }
    return v2_undef();
}
ShapeDef *a2_clip_drawing(MovieClip *c) { A2Obj *o = (A2Obj*)c->obj2; A2Disp *st = o ? (A2Disp*)o->payload : NULL; return st ? st->drawing : NULL; }
MovieClip *a2_clip_mask(MovieClip *c) { A2Obj *o = (A2Obj*)c->obj2; A2Disp *st = o ? (A2Disp*)o->payload : NULL; if (!st || !st->mask_obj) return NULL; return clip_of(st->mask_obj); }
int a2_clip_is_mask(MovieClip *c) { A2Obj *o = (A2Obj*)c->obj2; A2Disp *st = o ? (A2Disp*)o->payload : NULL; return st && st->is_mask; }
/* hit area: like the visible bounds, but text fields that are neither selectable
   nor editable let the mouse through */
int a2_clip_visible_bounds(MovieClip *c, SWFRect *r);
int a2_clip_hit_bounds(MovieClip *c, SWFRect *r) {
    A2Obj *o = (A2Obj*)c->obj2; A2Disp *st = o ? (A2Disp*)o->payload : NULL;
    if (st && st->is_text && st->tfs && st->tfs->was_static && !st->tfs->selectable && !st->tfs->input) return 0;
    return a2_clip_visible_bounds(c, r);
}
int a2_clip_visible_bounds(MovieClip *c, SWFRect *r) {
    A2Obj *o = (A2Obj*)c->obj2; A2Disp *st = o ? (A2Disp*)o->payload : NULL;
    if (!st) return 0;
    if (st->is_text && st->tfs) { tfs_apply_autosize(c->player, st->tfs); r->xmin = st->tfs->xmin; r->ymin = st->tfs->ymin; r->xmax = st->tfs->xmin + st->tfs->w; r->ymax = st->tfs->ymin + st->tfs->h; return 1; }
    if (st->bitmap_data && c->self && (!c->self->ch || c->self->ch->type == CHAR_BITMAP)) { r->xmin = 0; r->ymin = 0; r->xmax = st->bmp_w * 20; r->ymax = st->bmp_h * 20; return 1; }
    if (!st->bitmap_data && c->self && c->self->ch && c->self->ch->type == CHAR_BITMAP) {   /* a bitmap symbol whose bitmapData was not read yet */
        extern Bitmap *image_get(SWFMovie *m, int id); Bitmap *b = image_get(c->self->ch->movie, c->self->ch->id);
        if (b) { r->xmin = 0; r->ymin = 0; r->xmax = b->w * 20; r->ymax = b->h * 20; return 1; }
    }
    return 0;
}
/* bounds of o in t's space (t NULL: stage space) */
static V2 bounds_in(VM2 *vm, A2Obj *o, A2Obj *t) {
    /* Flash's arithmetic: the object's matrix composed with the target's inverse (f32, integer twips), corners rounded */
    SWFRect r; RMat2 w, tw, inv, full; s32 xs[4], ys[4], mnx, mny, mxx, mxy; int i;
    extern void rm_apply(const void *m, s32 x, s32 y, s32 *ox, s32 *oy); extern int rm_inverse(const void *m, void *o);
    a2_local_bounds(vm, o, &r);
    a2_world_rmat(o, 0, &w);
    if (t) { a2_world_rmat(t, 1, &tw); if (!rm_inverse(&tw, &inv)) rm_identity(&inv); } else rm_identity(&inv);
    rm_mul(&inv, &w, &full);
    rm_apply(&full, r.xmin, r.ymin, &xs[0], &ys[0]); rm_apply(&full, r.xmax, r.ymin, &xs[1], &ys[1]);
    rm_apply(&full, r.xmin, r.ymax, &xs[2], &ys[2]); rm_apply(&full, r.xmax, r.ymax, &xs[3], &ys[3]);
    mnx = mxx = xs[0]; mny = mxy = ys[0];
    for (i = 1; i < 4; i++) { if (xs[i] < mnx) mnx = xs[i]; if (xs[i] > mxx) mxx = xs[i]; if (ys[i] < mny) mny = ys[i]; if (ys[i] > mxy) mxy = ys[i]; }
    return v2_obj(a2_rect(vm, mnx / 20.0, mny / 20.0, mxx / 20.0 - mnx / 20.0, mxy / 20.0 - mny / 20.0));
}
static V2 do_getBounds(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = dself(self), *t = dself(ARG(0)); (void)argc;
    if (!o) return v2_null();
    return bounds_in(vm, o, t ? t : o);   /* targetCoordinateSpace null: the object's own space */
}
static V2 do_getRect(VM2 *vm, V2 self, int argc, V2 *argv) { extern int g_dg_nostroke; V2 r; g_dg_nostroke = 1; r = do_getBounds(vm, self, argc, argv); g_dg_nostroke = 0; return r; }
extern void rm_apply(const void *m, s32 x, s32 y, s32 *ox, s32 *oy); extern int rm_inverse(const void *m, void *o); extern s32 tw_from_px(double px);
static V2 do_localToGlobal(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = dself(self); RMat2 w; s32 ox, oy; double x = v2_tonum(vm, a2_get_s(vm, ARG(0), "x")), y = v2_tonum(vm, a2_get_s(vm, ARG(0), "y")); (void)argc;
    if (!o) return v2_null();
    a2_world_rmat(o, 1, &w); rm_apply(&w, tw_from_px(x), tw_from_px(y), &ox, &oy);
    return v2_obj(a2_point(vm, ox / 20.0, oy / 20.0));
}
static V2 do_globalToLocal(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = dself(self); RMat2 w, inv; s32 ox, oy; double x = v2_tonum(vm, a2_get_s(vm, ARG(0), "x")), y = v2_tonum(vm, a2_get_s(vm, ARG(0), "y")); (void)argc;
    if (!o) return v2_null();
    a2_world_rmat(o, 1, &w);
    if (!rm_inverse(&w, &inv)) return v2_obj(a2_point(vm, x, y));
    rm_apply(&inv, tw_from_px(x), tw_from_px(y), &ox, &oy);
    return v2_obj(a2_point(vm, ox / 20.0, oy / 20.0));
}
static V2 do_hitTestPoint(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = dself(self); double x = NUM(0) * 20, y = NUM(1) * 20; int shape = argc > 2 ? BOOL(2) : 0; MovieClip *c; DOBJ(d, o);
    if (!o) return v2_bool(0);
    if (shape && !a2_on_stage(vm, o)) return v2_bool(0);
    {   /* Flash ignores the main timeline's own transform here: the point is moved along with it */
        A2Obj *q = o, *rt = root_obj(vm); int g = 0; while (q && q != rt && g++ < 256) q = a2_parent_of(vm, q);
        if (q && q == rt && vm->player->root->self) { const SWFMatrix *m = &vm->player->root->self->matrix; double nx = m->sx * x + m->r1 * y + m->tx, ny = m->r0 * x + m->sy * y + m->ty; x = nx; y = ny; }
    }
    c = clip_of(o);
    if (c) return v2_bool(clip_hit_test_point(vm->player->avm, c, x, y, shape));
    if (d) { SWFMatrix pw; A2Obj *p = a2_parent_of(vm, o); if (p) a2_world_matrix(p, &pw); else swf_matrix_identity(&pw); if (shape) return v2_bool(clip_dobj_hit_test(vm->player->avm, d, &pw, x, y)); { SWFRect r; SWFMatrix w; double a, b, cc, dd, e, f, lx, ly; a2_local_bounds(vm, o, &r); a2_world_matrix_noscroll(o, &w); (void)a; (void)b; (void)cc; (void)dd; (void)e; (void)f; (void)lx; (void)ly; { extern int dg_world_box_contains(const SWFMatrix *w, const SWFRect *l, double sx, double sy); return v2_bool(dg_world_box_contains(&w, &r, x, y)); } } }
    return v2_bool(0);
}
static V2 do_hitTestObject(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = dself(self), *t = dself(ARG(0)); V2 r1, r2; double ax, ay, aw, ah, bx, by, bw, bh;
    if (!o || !t) return v2_bool(0);
    r1 = bounds_in(vm, o, NULL); r2 = bounds_in(vm, t, NULL);
    ax = v2_tonum(vm, a2_get_s(vm, r1, "x")); ay = v2_tonum(vm, a2_get_s(vm, r1, "y")); aw = v2_tonum(vm, a2_get_s(vm, r1, "width")); ah = v2_tonum(vm, a2_get_s(vm, r1, "height"));
    bx = v2_tonum(vm, a2_get_s(vm, r2, "x")); by = v2_tonum(vm, a2_get_s(vm, r2, "y")); bw = v2_tonum(vm, a2_get_s(vm, r2, "width")); bh = v2_tonum(vm, a2_get_s(vm, r2, "height"));
    return v2_bool(ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah);
}
static V2 do_transform_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self), *t; (void)argc; (void)argv; if (!o) return v2_null(); /* a fresh Transform on every read (Flash) */ t = a2_new(vm, a2_class_find_s(vm, "flash.geom::Transform")); a2_dyn_set(vm, t, a2_str(vm, "__target"), self); return v2_obj(t); }
static V2 do_transform_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = dself(self), *src = NULL; DOBJ(d, o); V2 tv = ARG(0); A2Prop *tp;
    if (tv.t != T_OBJ) { a2_throw_id(vm, "TypeError", 2007, "transform", NULL); return v2_undef(); }
    tp = a2_dyn_find(tv.u.o, a2_str(vm, "__target"));
    if (tp && tp->v.t == T_OBJ && tp->v.u.o->kind == OK_DISPLAY) src = tp->v.u.o;
    if (!o || !d) return v2_undef();
    if (src) { DisplayObject *sd = a2_dobj_of(src); if (sd) { d->matrix = sd->matrix; d->cxform = sd->cxform; } a2_disp(o)->has_m3d = a2_disp(src)->has_m3d; a2_disp(o)->has_xform = 0; return v2_undef(); }
    { V2 m = a2_get_s(vm, tv, "matrix"); if (m.t == T_OBJ) { a2_matrix_from_obj(vm, m, &d->matrix); a2_disp(o)->has_xform = 0; } }
    return v2_undef();
}
static const char *const blend_names[] = { "normal", "normal", "layer", "multiply", "screen", "lighten", "darken", "difference", "add", "subtract", "invert", "alpha", "erase", "overlay", "hardlight", "shader" };
static V2 do_blendMode_get(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = SELFO; int b = 0; DisplayObject *d; UNUSED;
    if (o && o->kind == OK_DISPLAY) { b = a2_disp(o)->blend; if (!b && (d = a2_dobj_of(o)) != NULL) b = d->blend; }
    if (b < 0 || b > 15) b = 0;
    return v2_cstr(vm, blend_names[b]);
}
static V2 do_blendMode_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = SELFO; V2 v = ARG(0); int i;
    if (v.t == T_NULL || v.t == T_UNDEF) { a2_throw_error(vm, "TypeError", "Parameter blendMode must be non-null.", 2007); return v2_undef(); }
    if (!o || o->kind != OK_DISPLAY) return v2_undef();
    for (i = 1; i < 16; i++) if (!strcmp(v2_tostr(vm, v)->s, blend_names[i])) { a2_disp(o)->blend = i; { DisplayObject *d = a2_dobj_of(o); if (d) d->blend = i; } return v2_undef(); }
    a2_throw_error(vm, "ArgumentError", "Parameter blendMode must be one of the accepted values.", 2008);
    return v2_undef();
}
static V2 do_noop_set(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_undef(); }
static V2 do_filters_get(VM2 *vm, V2 self, int argc, V2 *argv) { extern V2 a2_filters_get(VM2 *vm, DisplayObject *d); A2Obj *o = dself(self); UNUSED; return a2_filters_get(vm, o ? a2_dobj_of(o) : NULL); }
static V2 do_filters_set(VM2 *vm, V2 self, int argc, V2 *argv) { extern void a2_filters_set(VM2 *vm, DisplayObject *d, V2 v); A2Obj *o = dself(self); if (o) a2_filters_set(vm, a2_dobj_of(o), ARG(0)); return v2_undef(); }
static double re_px(double v) { double f; if (v != v || v > 1e15 || v < -1e15) return v; f = floor(v); if (v - f > 0.5) return f + 1; if (v - f < 0.5) return f; return fmod(f, 2.0) == 0 ? f : f + 1; }
static s32 px_tw(double v) { v *= 20.0; if (v != v) return 0; if (v > 2147483647.0) return 2147483647; if (v < -2147483648.0) return (s32)0x80000000; return (s32)v; }
static V2 do_scrollRect_get(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = dself(self); DisplayObject *d = o ? a2_dobj_of(o) : NULL; UNUSED;
    if (!d || !d->has_scroll_rect) return v2_null();
    return v2_obj(a2_rect(vm, d->scroll_rect.xmin / 20.0, d->scroll_rect.ymin / 20.0, (d->scroll_rect.xmax - d->scroll_rect.xmin) / 20.0, (d->scroll_rect.ymax - d->scroll_rect.ymin) / 20.0));
}
static V2 do_scrollRect_set(VM2 *vm, V2 self, int argc, V2 *argv) { if (argc > 0) { vm->depth--; a2_coerce(vm, argv[0], vm->c_rectangle); vm->depth++; if (vm->throwing) return v2_undef(); }   /* the argument coercion fails in the caller */
    A2Obj *o = dself(self); DisplayObject *d = o ? a2_dobj_of(o) : NULL; V2 v = ARG(0);
    if (o && a2_disp(o)->is_stage) { a2_throw_error(vm, "Error", "The Stage class does not implement this property or method.", 2071); return v2_undef(); }
    if (!d) return v2_undef();
    if (v.t == T_NULL || v.t == T_UNDEF) { d->has_scroll_rect = 0; return v2_undef(); }
    {
        double x = v2_tonum(vm, a2_get_s(vm, v, "x")), y = v2_tonum(vm, a2_get_s(vm, v, "y"));
        double w = v2_tonum(vm, a2_get_s(vm, v, "width")), h = v2_tonum(vm, a2_get_s(vm, v, "height"));
        /* whole pixels: the edges round half to even */
        d->scroll_rect.xmin = px_tw(re_px(x)); d->scroll_rect.ymin = px_tw(re_px(y));
        d->scroll_rect.xmax = px_tw(re_px(x + w)); d->scroll_rect.ymax = px_tw(re_px(y + h));
        d->has_scroll_rect = 1;
    }
    return v2_undef();
}

static V2 do_cacheAsBitmap_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = SELFO; DisplayObject *d = o && o->kind == OK_DISPLAY ? a2_dobj_of(o) : NULL; UNUSED; return v2_bool(d && (d->cache_bmp || d->nfilters > 0)); }
static V2 do_cacheAsBitmap_set(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = SELFO; DisplayObject *d = o && o->kind == OK_DISPLAY ? a2_dobj_of(o) : NULL; (void)self; if (d) d->cache_bmp = v2_tobool(vm, ARG(0)); return v2_undef(); }
static V2 do_root_get(VM2 *vm, V2 self, int argc, V2 *argv);
static V2 do_loaderInfo_get(VM2 *vm, V2 self, int argc, V2 *argv) {
    extern V2 a2_root_loaderinfo(VM2 *vm); A2Obj *o = dself(self); (void)argc; (void)argv;
    if (!o) return v2_null();
    if (a2_disp(o)->is_stage) {   /* the stage has its own LoaderInfo, which never fires init/complete */
        A2Str *k = a2_str(vm, "__stageLoaderInfo"); A2Prop *p = a2_dyn_find(o, k); V2 li;
        if (p) return p->v;
        li = v2_obj(a2_new(vm, vm->c_loaderinfo)); a2_dyn_set_hidden(vm, o, k, li); a2_dyn_set_hidden(vm, li.u.o, a2_str(vm, "__isStage"), v2_bool(1)); return li;
    }
    { A2Obj *q = o; int g = 0; while (q && g++ < 256) { A2Disp *qd = a2_disp(q); if (qd->is_movie_root) return qd->loader_info ? v2_obj(qd->loader_info) : v2_null(); if (qd->is_root) break; q = a2_parent_of(vm, q); } }
    if (!a2_disp(o)->is_root && do_root_get(vm, self, 0, NULL).t != T_OBJ) return v2_null();   /* off-list objects have no loaderInfo */
    return a2_root_loaderinfo(vm);
}
static V2 do_opaqueBackground_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); UNUSED; if (o && a2_disp(o)->has_opaque_bg) return a2_disp(o)->opaque_bg; return v2_null(); }
static V2 do_opaqueBackground_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = dself(self); V2 v = ARG(0); DisplayObject *d;
    if (!o) return v2_undef();
    d = a2_dobj_of(o);
    if (v.t == T_NULL || v.t == T_UNDEF) { a2_disp(o)->has_opaque_bg = 0; a2_disp(o)->opaque_bg = v2_null(); if (d) d->has_opaque_bg = 0; }
    else { u32 c = v2_touint(vm, v); a2_disp(o)->has_opaque_bg = 1; a2_disp(o)->opaque_bg = v2_uint(c); if (d) { d->has_opaque_bg = 1; d->opaque_bg = c & 0xFFFFFF; } }   /* the renderer's copy */
    return v2_undef();
}
static V2 do_metaData_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); A2Prop *p; UNUSED; if (!o) return v2_null(); p = a2_dyn_find(o, a2_str(vm, "__metaData")); return p ? p->v : v2_null(); }
static V2 do_metaData_set(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); if (!o) return v2_undef(); a2_dyn_set_hidden(vm, o, a2_str(vm, "__metaData"), ARG(0)); return v2_undef(); }
static V2 do_rotationX_get(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_num(0); }
static V2 do_one_get(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_num(1); }
static V2 do_z_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); UNUSED; return v2_num(o ? a2_disp(o)->z : 0); }
static V2 do_z_set(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); double z = NUM(0); if (o) { mark_script_moved(o); a2_disp(o)->z = z; a2_disp(o)->has_m3d = 1; } return v2_undef(); }
static V2 do_scale9Grid_get(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_null(); }
static V2 do_accessibilityProperties_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Prop *p; UNUSED; p = SELFO ? a2_dyn_find(SELFO, a2_str(vm, "__accProps")) : NULL; return p ? p->v : v2_null(); }
static V2 do_accessibilityProperties_set(VM2 *vm, V2 self, int argc, V2 *argv) { if (SELFO) a2_dyn_set(vm, SELFO, a2_str(vm, "__accProps"), ARG(0)); return v2_undef(); }
static V2 io_accImpl_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Prop *p; UNUSED; p = SELFO ? a2_dyn_find(SELFO, a2_str(vm, "__accImpl")) : NULL; return p ? p->v : v2_null(); }
static V2 io_accImpl_set(VM2 *vm, V2 self, int argc, V2 *argv) { if (SELFO) a2_dyn_set(vm, SELFO, a2_str(vm, "__accImpl"), ARG(0)); return v2_undef(); }
/* the native part of a display object created by `new`: built by the DisplayObject
   constructor, or earlier when the object is used before its super() call
   (addChild(this) ahead of super(), as Haxe/OpenFL code does) */
void a2_display_ensure_record(VM2 *vm, A2Obj *o) {
    if (!o->dobj && !o->clip && !(o->payload && ((A2Disp*)o->payload)->is_stage)) {
        /* `new Sprite()` etc: build a fresh record (reuses the object) */
        DisplayObject *nd = (DisplayObject*)calloc(1, sizeof(DisplayObject)); SWFChar *ch = NULL; int i; A2Disp *st;
        o->kind = OK_DISPLAY;
        for (i = 0; i < vm->nsymbols; i++) if (vm->symbols[i].id != 0 && a2_is_subclass(o->cls, vm->symbols[i].cls)) { ch = swf_get_char(vm->symbols[i].movie ? vm->symbols[i].movie : vm->player->movie, vm->symbols[i].id); if (vm->symbols[i].cls == o->cls) break; }
        nd->depth = -1; nd->char_id = ch ? ch->id : -1; nd->ch = ch; nd->visible = 1; nd->placed_by_script = 1;
        swf_matrix_identity(&nd->matrix); swf_cxform_identity(&nd->cxform);
        { SWFMovie *rm = NULL; extern MovieClip *player_make_root_instance(Player *p, SWFMovie *m, DisplayObject *self);
          if (!ch) for (i = 0; i < vm->nsymbols; i++) if (vm->symbols[i].id == 0 && vm->symbols[i].cls == o->cls) rm = vm->symbols[i].movie ? vm->symbols[i].movie : vm->player->movie;
          if (rm) nd->clip = player_make_root_instance(vm->player, rm, nd);
          else nd->clip = player_make_clip(vm->player, NULL, (ch && ch->type == CHAR_SPRITE) ? ch : NULL, nd);
          nd->obj2 = o; o->dobj = nd; o->clip = nd->clip; if (nd->clip) nd->clip->obj2 = o;
          st = a2_disp(o); st->owns = 1; o->ival |= 1;
          if (rm && nd->clip) { player_run_first_frame(nd->clip); nd->clip->skip_enter = 1; avm2_frame_shown(vm->player, nd->clip); construct_pending_ex(vm, nd->clip, nd->clip); } }
        { char b[32]; sprintf(b, "instance%d", ++vm->instance_counter); st->name = a2_str(vm, b); }
        if (ch && ch->type == CHAR_SPRITE && nd->clip) { player_run_first_frame(nd->clip); nd->clip->skip_enter = 1; avm2_frame_shown(vm->player, nd->clip); }   /* script-created clips skip their next enterFrame */
        if (ch && ch->type == CHAR_EDITTEXT) a2_text_sync_from_char(vm, o);
        if (ch && ch->type == CHAR_BUTTON && nd->clip && vm->c_simplebutton && a2_is_subclass(o->cls, vm->c_simplebutton)) build_button_states(vm, o, nd);   /* `new` of a button symbol class */
    }
}
static V2 DisplayObject_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = SELFO; (void)argc; (void)argv;
    if (!o) return v2_undef();
    /* timeline children are constructed inside the implicit super() call of their parent's constructor */
    if (o->payload && ((A2Disp*)o->payload)->is_root && !vm->root_children_done) { vm->root_children_done = 1; construct_pending_ex(vm, NULL, o->clip); }
    else if (o->clip && vm->npending) construct_pending_ex(vm, o->clip, o->clip);
    a2_display_ensure_record(vm, o);
    o->ival &= ~0x40;
    if (!a2_parent_of(vm, o)) a2_orphan_add(vm, o, 0);   /* a parentless movie clip runs its timeline anyway */
    return v2_undef();
}
/* AVM1Movie.call / addCallback: Flash throws #2014 (no ExternalInterface bridge to the AVM1 side) */
static V2 avm1movie_call(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; (void)argc; (void)argv; a2_throw_error(vm, "Error", "Feature is not available at this time.", 2014); return v2_undef(); }
static V2 avm1movie_addCallback(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; (void)argc; (void)argv; a2_throw_error(vm, "Error", "Feature is not available at this time.", 2014); return v2_undef(); }

V2 a2_displayobject_ctor_pub(VM2 *vm, V2 self) { return DisplayObject_ctor(vm, self, 0, NULL); }

/* ---------------------------------------------------------------- */
/* InteractiveObject / container                                    */

static V2 io_mouseEnabled_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); UNUSED; return v2_bool(o ? a2_disp(o)->mouse_enabled : 1); }
static V2 io_mouseEnabled_set(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); if (o) a2_disp(o)->mouse_enabled = BOOL(0); return v2_undef(); }
static V2 io_doubleClickEnabled_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); UNUSED; return v2_bool(o ? a2_disp(o)->double_click : 0); }
static V2 io_doubleClickEnabled_set(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); if (o) a2_disp(o)->double_click = BOOL(0); return v2_undef(); }
static int fo_tab_enabled(VM2 *vm, A2Obj *o);
static V2 io_tabEnabled_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); UNUSED; return v2_bool(o ? fo_tab_enabled(vm, o) : 0); }
static V2 io_tabEnabled_set(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); if (o && a2_disp(o)->is_stage) { a2_throw_error(vm, "Error", "The Stage class does not implement this property or method.", 2071); return v2_undef(); } if (o) { a2_disp(o)->tab_enabled = BOOL(0); a2_disp(o)->tab_enabled_set = 1; } return v2_undef(); }
static V2 io_tabIndex_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); UNUSED; return v2_int(o && a2_disp(o)->tab_index_set ? a2_disp(o)->tab_index : -1); }
static V2 io_tabIndex_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = dself(self); s32 v = INT(0);
    if (o && a2_disp(o)->is_stage) { a2_throw_error(vm, "Error", "The Stage class does not implement this property or method.", 2071); return v2_undef(); }
    if (vm->throwing || !o) return v2_undef();
    if (v < -1) { char nb[16]; sprintf(nb, "%ld", (long)v); a2_throw_id(vm, "RangeError", 2027, "tabIndex", nb); return v2_undef(); }
    a2_disp(o)->tab_index_set = v != -1; a2_disp(o)->tab_index = v;
    return v2_undef();
}
static V2 io_focusRect_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); UNUSED; if (!o || !a2_disp(o)->focus_rect_set) return v2_null(); return v2_bool(a2_disp(o)->focus_rect); }
static V2 io_focusRect_set(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); V2 v = ARG(0); UNUSED; if (!o) return v2_null(); if (a2_disp(o)->is_stage) { a2_throw_error(vm, "Error", "The Stage class does not implement this property or method.", 2071); return v2_undef(); } if (v.t == T_NULL || v.t == T_UNDEF) a2_disp(o)->focus_rect_set = 0; else { a2_disp(o)->focus_rect_set = 1; a2_disp(o)->focus_rect = v.t == T_BOOL && v.u.b; } return v2_null(); }
static V2 io_contextMenu_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Prop *p; UNUSED; p = SELFO ? a2_dyn_find(SELFO, a2_str(vm, "__contextMenu")) : NULL; return p ? p->v : v2_null(); }
static V2 io_contextMenu_set(VM2 *vm, V2 self, int argc, V2 *argv) { if (SELFO) a2_dyn_set(vm, SELFO, a2_str(vm, "__contextMenu"), ARG(0)); return v2_undef(); }

static MovieClip *container_clip(VM2 *vm, A2Obj *o) {
    if (!o) return NULL;
    if (!o->dobj && !o->clip && !(o->payload && ((A2Disp*)o->payload)->is_stage) && o->cls && vm->c_container && a2_is_subclass(o->cls, vm->c_container)) { a2_display_ensure_record(vm, o); o->ival |= 0x40; }   /* used before its super() call: children are not constructed yet */
    if (a2_disp(o)->is_stage) return vm->player->root;
    return clip_of(o);
}
static A2Obj *child_obj(VM2 *vm, DisplayObject *d) { if (d->clip && CLIP_A1MIX(d->clip) && !d->clip->obj2 && !d->obj2) return NULL; return a2_display_obj(vm, d->clip, d); }   /* AVM1 objects in a Loader have no AVM2 side */

static void unlink_record(MovieClip *c, DisplayObject *d) { DisplayObject **pp; for (pp = &c->children; *pp; pp = &(*pp)->next) if (*pp == d) { *pp = d->next; d->next = NULL; return; } }

/* ---- logical container model ----
 * The Stage is a container whose child 0 is the root; objects added with stage.addChild()
 * are flagged stage_child and kept at the end of the root's record list (rendered above
 * the root's own content).  The root's logical children exclude those records.
 * Stage children placed below the root (index 0) are flagged stage_child = 2 and kept
 * at the start of the record list. */
static A2Obj *rec_obj(DisplayObject *d) { return (A2Obj*)(d->obj2 ? d->obj2 : (d->clip ? d->clip->obj2 : NULL)); }
static int rec_is_stage_child(DisplayObject *d) { A2Obj *o = rec_obj(d); return o && o->payload ? ((A2Disp*)o->payload)->stage_child : 0; }
static A2Obj *root_obj(VM2 *vm) { return a2_display_obj(vm, vm->player->root, NULL); }
static int cc_count(VM2 *vm, A2Obj *p) {
    MovieClip *c = container_clip(vm, p); DisplayObject *d; int n = 0, st, rt;
    if (!c) return 0;
    st = a2_disp(p)->is_stage; rt = (c == vm->player->root);
    for (d = c->children; d; d = d->next) { int sc = rt ? rec_is_stage_child(d) : 0; if (st ? sc : !sc) n++; }
    return st && !vm->root_detached ? n + 1 : n;
}
static A2Obj *cc_at(VM2 *vm, A2Obj *p, int i) {
    MovieClip *c = container_clip(vm, p); DisplayObject *d; int st, rt;
    if (!c || i < 0) return NULL;
    st = a2_disp(p)->is_stage; rt = (c == vm->player->root);
    if (st) for (d = c->children; d; d = d->next) if (rt && rec_is_stage_child(d) == 2) { if (i == 0) return child_obj(vm, d); i--; }   /* below the root */
    if (st && !vm->root_detached) { if (i == 0) return root_obj(vm); i--; }
    for (d = c->children; d; d = d->next) { int sc = rt ? rec_is_stage_child(d) : 0; if (st ? sc == 1 : !sc) { if (i == 0) return child_obj(vm, d); i--; } }
    return NULL;
}
static int cc_index(VM2 *vm, A2Obj *p, A2Obj *child) {
    MovieClip *c = container_clip(vm, p); DisplayObject *d, *cd; int st, rt, i = 0;
    if (!c || !child) return -1;
    st = a2_disp(p)->is_stage; rt = (c == vm->player->root);
    cd = a2_dobj_of(child);
    if (st && rt) for (d = c->children; d; d = d->next) if (rec_is_stage_child(d) == 2) { if (d == cd) return i; i++; }   /* below the root */
    if (st && !vm->root_detached) { if (a2_disp(child)->is_root) return i; i++; }
    if (!cd) return -1;
    for (d = c->children; d; d = d->next) { int sc = rt ? rec_is_stage_child(d) : 0; if (st ? sc == 1 : !sc) { if (d == cd) return i; i++; } }
    return -1;
}
/* position in the record list where logical index 'index' of container p goes */
static DisplayObject *cc_record_at_logical(VM2 *vm, A2Obj *p, int index) {
    A2Obj *o = cc_at(vm, p, index); return o ? a2_dobj_of(o) : NULL;
}

/* detach child from whatever parent list it is on (no events) */
static void detach_child(VM2 *vm, A2Obj *child) {
    DOBJ(d, child); A2Obj *p = a2_parent_of(vm, child); MovieClip *pc;
    if (!d) return;
    pc = p ? container_clip(vm, p) : (d->owner ? d->owner : NULL);
    if (pc) unlink_record(pc, d);
    d->owner = NULL; if (d->clip) d->clip->parent = NULL; d->in_dl = 0;
    child->parent_disp = NULL;
    a2_disp(child)->owns = 1; a2_disp(child)->stage_child = 0;
    if (vm->player->focus && d->clip == vm->player->focus) vm->player->focus = NULL;
    a2_orphan_add(vm, child, 0);
}
static void remove_child_events(VM2 *vm, A2Obj *child) {
    int onstage = a2_on_stage(vm, child);
    a2_dispatch_simple(vm, child, vm->s_removed, 1);
    if (onstage) dispatch_removed_tree(vm, child);
}
static void insert_child(VM2 *vm, A2Obj *parent, A2Obj *child, int index) {
    MovieClip *pc = container_clip(vm, parent); DOBJ(d, child); int n; DisplayObject *k, *at, *prev;
    int st = a2_disp(parent)->is_stage;
    if (!pc || !d) return;
    n = cc_count(vm, parent);
    if (index < 0) index = 0; if (index > n) index = n;
    if (st) {
        int nb = 0, below;
        for (k = pc->children; k; k = k->next) if (rec_is_stage_child(k) == 2) nb++;
        below = index <= nb && !vm->root_detached;
        a2_disp(child)->stage_child = below ? 2 : 1;
        if (below) {   /* before the root's content: ahead of the first record that is not below it */
            at = NULL;
            if (index < nb) at = cc_record_at_logical(vm, parent, index);
            else for (k = pc->children; k; k = k->next) if (rec_is_stage_child(k) != 2) { at = k; break; }
            if (!at) { int depth = -16384; for (k = pc->children; k; k = k->next) if (k->depth >= depth) depth = k->depth + 1; d->depth = depth; }
            goto place;
        }
    } else a2_disp(child)->stage_child = 0;
    if (index == n) {
        /* append: after the last logical child of this container */
        at = NULL;
        if (!st && pc == vm->player->root) { for (k = pc->children; k; k = k->next) if (rec_is_stage_child(k) == 1) { at = k; break; } }
        if (!at) { int depth = 16384; for (k = pc->children; k; k = k->next) if (k->depth >= depth) depth = k->depth + 1; d->depth = depth; }
    } else at = cc_record_at_logical(vm, parent, index);
place:
    if (at) {
        /* find record preceding 'at' in the record list */
        prev = NULL; for (k = pc->children; k && k != at; k = k->next) prev = k;
        { int lo = prev ? prev->depth + 1 : at->depth - 1;
          if (lo < at->depth) d->depth = lo;
          else { for (k = at; k; k = k->next) k->depth++; d->depth = at->depth - 1; } }
    }
    player_insert_dobj(pc, d);
    d->owner = pc; if (d->clip) d->clip->parent = pc;
    child->parent_disp = parent;
    a2_disp(child)->owns = 0;
    if (!(vm->c_simplebutton && a2_is_subclass(parent->cls, vm->c_simplebutton)) && a2_disp(child)->button_state) { a2_disp(child)->button_state = 0; d->state_mask = 0; }   /* a button state moved into a container */
}
static V2 dc_addChildAt(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *p = dself(self), *c = dself(ARG(0)); int index = argc > 1 ? INT(1) : -1; MovieClip *pc; int onstage, n, same;
    if (!p || !c) { a2_throw_error(vm, "TypeError", "Parameter child must be non-null.", 2007); return v2_undef(); }
    if (!p->dobj && !p->clip) a2_display_ensure_record(vm, p);   /* used before its super() call */
    if (!c->dobj && !c->clip) a2_display_ensure_record(vm, c);
    pc = container_clip(vm, p);
    if (!pc) return ARG(0);
    { A2Obj *q = p; int g = 0; while (q && g++ < 256) { if (q == c) { a2_throw_error(vm, "ArgumentError", "An object cannot be added as a child to one of it's children (or children's children, etc.).", 2150); return v2_undef(); } q = a2_parent_of(vm, q); } }
    if (a2_disp(p)->is_stage && c == root_obj(vm) && vm->root_detached) { vm->root_detached = 0; a2_dispatch_simple(vm, c, vm->s_added, 1); dispatch_added_tree(vm, c); return ARG(0); }   /* back on the stage */
    if (c == vm->stage_obj || a2_disp(c)->is_root) { a2_throw_error(vm, "ArgumentError", "The object cannot be added as a child of itself.", 2024); return v2_undef(); }
    same = (a2_parent_of(vm, c) == p);
    {   /* an AVM1 movie stays in its Loader (SWF 10+) */
        extern struct SWFMovie *a2_code_movie(VM2 *vm); MovieClip *cc = clip_of(c); struct SWFMovie *cm = a2_code_movie(vm);
        if (cc && CLIP_A1MIX(cc) && !same && a2_parent_of(vm, c) && cm && cm->version >= 10) { a2_throw_id(vm, "ArgumentError", 2180, NULL, NULL); return v2_undef(); }
    }
    n = cc_count(vm, p);
    if (index < 0) index = n - (same ? 1 : 0);
    if (index > n - (same ? 1 : 0)) { a2_throw_error(vm, "RangeError", "The supplied index is out of bounds.", 2006); return v2_undef(); }
    { DisplayObject *cd = a2_dobj_of(c); int indl = cd ? cd->in_dl : 0;
      if (cd) cd->placed_by_script = 1;
      if (a2_parent_of(vm, c)) { if (!same) remove_child_events(vm, c); detach_child(vm, c); }
      insert_child(vm, p, c, index);
      if (same && cd) cd->in_dl = indl; }   /* moved within its parent: still on the depth list */
    if (!same) {   /* re-adding to the same parent dispatches nothing */
        onstage = a2_on_stage(vm, c);
        a2_dispatch_simple(vm, c, vm->s_added, 1);
        if (onstage) dispatch_added_tree(vm, c);
    }
    return ARG(0);
}
/* Loader content: insert / take away with the usual events */
void a2_container_insert(VM2 *vm, A2Obj *p, A2Obj *c, int index) {
    { DisplayObject *cd = a2_dobj_of(c); if (cd) cd->placed_by_script = 1; }
    if (a2_parent_of(vm, c)) { remove_child_events(vm, c); detach_child(vm, c); }
    insert_child(vm, p, c, index);
    a2_dispatch_simple(vm, c, vm->s_added, 1);
    if (a2_on_stage(vm, c)) dispatch_added_tree(vm, c);
}
void a2_container_remove(VM2 *vm, A2Obj *c) {
    if (!a2_parent_of(vm, c)) return;
    remove_child_events(vm, c);
    detach_child(vm, c);
}
static void run_pending_frame_scripts(VM2 *vm, MovieClip *c);
void a2_run_clip_frame_scripts(VM2 *vm, MovieClip *c) { run_pending_frame_scripts(vm, c); }
static V2 dc_addChild(VM2 *vm, V2 self, int argc, V2 *argv) { V2 a[1]; a[0] = ARG(0); (void)argc; return dc_addChildAt(vm, self, 1, a); }
static V2 dc_removeChild(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *p = dself(self), *c = dself(ARG(0));
    if (!p || !c) { a2_throw_error(vm, "TypeError", "Parameter child must be non-null.", 2007); return v2_undef(); }
    if (a2_disp(p)->is_stage && c == root_obj(vm) && !vm->root_detached) {   /* the main timeline leaves the stage */
        a2_dispatch_simple(vm, c, vm->s_removed, 1); dispatch_removed_tree(vm, c); vm->root_detached = 1; return ARG(0); }
    if (a2_parent_of(vm, c) != p || a2_disp(c)->is_root) { a2_throw_error(vm, "ArgumentError", "The supplied DisplayObject must be a child of the caller.", 2025); return v2_undef(); }
    { DisplayObject *cd = a2_dobj_of(c); if (cd) cd->placed_by_script = 1; }
    { int onstage = a2_on_stage(vm, c); a2_dispatch_simple(vm, c, vm->s_removed, 1); if (onstage) dispatch_removed_tree(vm, c); }
    detach_child(vm, c);
    return ARG(0);
}
static V2 dc_removeChildAt(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *p = dself(self), *co; V2 a[1];
    if (!p) return v2_undef();
    co = cc_at(vm, p, INT(0));
    if (!co) { a2_throw_error(vm, "RangeError", "The supplied index is out of bounds.", 2006); return v2_undef(); }
    a[0] = v2_obj(co);
    return dc_removeChild(vm, self, 1, a);
}
static V2 dc_removeChildren(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *p = dself(self); int b = argc > 0 ? INT(0) : 0, e = argc > 1 ? INT(1) : 0x7fffffff, i, n;
    if (!p) return v2_undef();
    n = cc_count(vm, p);
    if (e == 0x7fffffff) { e = n - 1; if (b > e) return v2_undef(); }
    if (b < 0 || b > e || e >= n) { a2_throw_error(vm, "RangeError", "The supplied index is out of bounds.", 2006); return v2_undef(); }
    for (i = e; i >= b; i--) { A2Obj *co = cc_at(vm, p, i); V2 a[1]; if (!co || a2_disp(co)->is_root) continue; a[0] = v2_obj(co); dc_removeChild(vm, self, 1, a); }
    return v2_undef();
}
static V2 dc_getChildAt(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *p = dself(self); A2Obj *co; if (!p) return v2_null(); co = cc_at(vm, p, INT(0)); if (!co) { if (INT(0) >= 0 && INT(0) < cc_count(vm, p)) return v2_null(); a2_throw_error(vm, "RangeError", "The supplied index is out of bounds.", 2006); return v2_undef(); } if (p->ival & 0x40) return v2_null(); if (!(co->ival & 1) && !a2_disp(co)->is_root) return v2_null(); return v2_obj(co); }
int a2_child_by_name(VM2 *vm, A2Obj *o, A2Str *name, V2 *out) {
    MovieClip *pc; DisplayObject *d; int st, rt;
    if (!o->cls || !a2_is_subclass(o->cls, vm->c_container)) return 0;
    pc = container_clip(vm, o);
    if (vm->player->debug > 1) fprintf(stderr, "[avm2] child_by_name %s in %s pc=%p\n", name->s, o->cls->name->s, (void*)pc);
    if (!pc) return 0;
    st = a2_disp(o)->is_stage; rt = (pc == vm->player->root);
    if (st && a2_display_name(vm, root_obj(vm)) == name) { *out = v2_obj(root_obj(vm)); return 1; }
    for (d = pc->children; d; d = d->next) { A2Obj *co = rec_obj(d); int sc = rt ? rec_is_stage_child(d) : 0; if (st ? !sc : sc) continue; if (co ? a2_display_name(vm, co) == name : !strcmp(d->name, name->s)) { A2Obj *k = child_obj(vm, d); *out = k ? v2_obj(k) : v2_null(); return 1; } }
    return 0;
}
static V2 dc_getChildByName(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *p = dself(self); V2 r; if (!p) return v2_null(); if (a2_child_by_name(vm, p, STR(0), &r)) return r; return v2_null(); }
static V2 dc_getChildIndex(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *p = dself(self), *c = dself(ARG(0)); int i; if (!p || !c) return v2_int(-1); i = cc_index(vm, p, c); if (i < 0) { a2_throw_error(vm, "ArgumentError", "The supplied DisplayObject must be a child of the caller.", 2025); return v2_undef(); } return v2_int(i); }
static V2 dc_setChildIndex(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *p = dself(self), *c = dself(ARG(0)); MovieClip *pc = container_clip(vm, p); int idx = INT(1), n;
    if (!pc || !c) return v2_undef();
    n = cc_count(vm, p);
    if (cc_index(vm, p, c) < 0 || a2_disp(c)->is_root) { a2_throw_error(vm, "ArgumentError", "The supplied DisplayObject must be a child of the caller.", 2025); return v2_undef(); }
    if (idx < 0 || idx >= n || (a2_disp(p)->is_stage && idx < 1)) { a2_throw_error(vm, "RangeError", "The supplied index is out of bounds.", 2006); return v2_undef(); }
    { DisplayObject *d = a2_dobj_of(c); int indl = d ? d->in_dl : 0; if (d) d->placed_by_script = 1; unlink_record(pc, d); insert_child(vm, p, c, idx); if (d) d->in_dl = indl; }
    return v2_undef();
}
static V2 dc_swapChildren(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *p = dself(self), *a = dself(ARG(0)), *b = dself(ARG(1)); MovieClip *pc = container_clip(vm, p); DisplayObject *da, *db; int t;
    if (!pc || !a || !b) return v2_undef();
    da = a2_dobj_of(a); db = a2_dobj_of(b);
    if (cc_index(vm, p, a) < 0 || cc_index(vm, p, b) < 0 || a2_disp(a)->is_root || a2_disp(b)->is_root) { a2_throw_error(vm, "ArgumentError", "The supplied DisplayObject must be a child of the caller.", 2025); return v2_undef(); }
    if (!da || !db) return v2_undef();
    da->placed_by_script = db->placed_by_script = 1;
    t = da->depth; da->depth = db->depth; db->depth = t; player_resort(pc);
    return v2_undef();
}
static V2 dc_swapChildrenAt(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *p = dself(self); A2Obj *a, *b; V2 args[2]; if (!p) return v2_undef(); a = cc_at(vm, p, INT(0)); b = cc_at(vm, p, INT(1)); if (!a || !b) { a2_throw_error(vm, "RangeError", "The supplied index is out of bounds.", 2006); return v2_undef(); } args[0] = v2_obj(a); args[1] = v2_obj(b); return dc_swapChildren(vm, self, 2, args); }
static V2 dc_contains(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *p = dself(self), *c = dself(ARG(0)); int g = 0; if (!p || !c) return v2_bool(0); while (c && g++ < 256) { if (c == p) return v2_bool(1); c = a2_parent_of(vm, c); } return v2_bool(0); }
static void stop_all_clips(MovieClip *c, int depth) {
    DisplayObject *d;
    if (!c || depth > 256) return;
    c->playing = 0; c->advanced = 0;
    for (d = c->children; d; d = d->next) if (d->clip) stop_all_clips(d->clip, depth + 1);
}
static V2 dc_stopAllMovieClips(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *p = dself(self); UNUSED; if (p) stop_all_clips(clip_of(p), 0); return v2_undef(); }
static V2 dc_numChildren(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *p = dself(self); UNUSED; return v2_int(p ? cc_count(vm, p) : 0); }
static V2 dc_mouseChildren_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); UNUSED; return v2_bool(o ? a2_disp(o)->mouse_children : 1); }
static V2 dc_mouseChildren_set(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); if (o) a2_disp(o)->mouse_children = BOOL(0); return v2_undef(); }
static V2 dc_tabChildren_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); UNUSED; return v2_bool(o && !a2_disp(o)->is_stage ? !a2_disp(o)->no_tab_children : 1); }
static A2Obj *root_obj(VM2 *vm);
static V2 dc_tabChildren_set(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); if (o && a2_disp(o)->is_stage) o = root_obj(vm); if (o) a2_disp(o)->no_tab_children = !BOOL(0); return v2_undef(); }
/* every descendant, in render order (a container before its children), whose own content is under the point */
static void objects_under(VM2 *vm, A2Obj *p, MovieClip *pc, A2Obj *arr, double x, double y, int depth) {
    extern int clip_dobj_hit_own(DisplayObject *d, const SWFMatrix *pw, double sx, double sy); extern int clip_dobj_children_hittable(DisplayObject *d, double sx, double sy);
    DisplayObject *d; SWFMatrix pw;
    if (!pc || depth > 200) return;
    a2_world_matrix(p, &pw);
    for (d = pc->children; d; d = d->next) {
        A2Obj *co = child_obj(vm, d);
        if (!co || (co->payload && a2_disp(co)->is_mask)) continue;   /* masks are never under the point */
        if (d->state_mask && !(d->state_mask & (pc->self ? BTN_STATE_BIT(pc->self->btn_state) : 1))) continue;   /* a button: its current state only */
        if (clip_dobj_hit_own(d, &pw, x, y)) a2_arr_push(vm, arr, v2_obj(co));
        if (d->clip && clip_dobj_children_hittable(d, x, y)) objects_under(vm, co, d->clip, arr, x, y, depth + 1);
    }
}
static V2 dc_getObjectsUnderPoint(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *p = dself(self); MovieClip *pc = container_clip(vm, p); A2Obj *arr = a2_new_array(vm); double x = v2_tonum(vm, a2_get_s(vm, ARG(0), "x")) * 20, y = v2_tonum(vm, a2_get_s(vm, ARG(0), "y")) * 20;
    (void)argc;
    if (p && !a2_disp(p)->is_stage) {   /* the container itself when its own content (graphics) is under the point */
        extern int clip_dobj_hit_own(DisplayObject *d, const SWFMatrix *pw, double sx, double sy);
        DisplayObject *sd = a2_dobj_of(p); A2Obj *po = a2_parent_of(vm, p); SWFMatrix pw;
        if (po) a2_world_matrix(po, &pw); else swf_matrix_identity(&pw);
        if (sd && clip_dobj_hit_own(sd, &pw, x, y)) a2_arr_push(vm, arr, v2_obj(p));
    }
    objects_under(vm, p, pc, arr, x, y, 0);
    return v2_obj(arr);
}
static V2 dc_areInaccessibleObjectsUnderPoint(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_bool(0); }

/* ---------------------------------------------------------------- */
/* Sprite / MovieClip                                               */

static V2 sp_graphics(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); A2Disp *st; (void)argc; (void)argv; if (!o) return v2_null(); st = a2_disp(o); if (!st->graphics) { st->graphics = a2_new(vm, vm->c_graphics); st->graphics->kind = OK_GRAPHICS; a2_dyn_set(vm, st->graphics, a2_str(vm, "__owner"), self); } return v2_obj(st->graphics); }
static V2 sp_buttonMode_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); UNUSED; return v2_bool(o ? a2_disp(o)->button_mode : 0); }
static V2 sp_buttonMode_set(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); if (o) a2_disp(o)->button_mode = BOOL(0); return v2_undef(); }
static V2 sp_useHandCursor_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); UNUSED; return v2_bool(o ? a2_disp(o)->use_hand_cursor : 1); }
static V2 sp_useHandCursor_set(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); if (o) a2_disp(o)->use_hand_cursor = BOOL(0); return v2_undef(); }
static V2 sp_hitArea_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Prop *p; UNUSED; p = SELFO ? a2_dyn_find(SELFO, a2_str(vm, "__hitArea")) : NULL; return p ? p->v : v2_null(); }
static V2 sp_hitArea_set(VM2 *vm, V2 self, int argc, V2 *argv) { if (SELFO) a2_dyn_set(vm, SELFO, a2_str(vm, "__hitArea"), ARG(0)); return v2_undef(); }
static V2 sp_startDrag(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = dself(self); A2Disp *st; DOBJ(d, o); SWFMatrix w; double a, b, c, dd, e, f, sx, sy, lx, ly;
    if (!o || !d) return v2_undef();
    st = a2_disp(o); st->dragging = 1; st->drag_lock = argc > 0 ? BOOL(0) : 0;
    { A2Obj *p = a2_parent_of(vm, o); if (p) a2_world_matrix(p, &w); else swf_matrix_identity(&w); }
    sx = vm->mouse_x * 20.0 / vm->player->zoom; sy = vm->mouse_y * 20.0 / vm->player->zoom;
    if (matrix_invert(&w, &a, &b, &c, &dd, &e, &f)) { lx = a * sx + b * sy + e; ly = c * sx + dd * sy + f; } else { lx = sx; ly = sy; }
    st->drag_dx = st->drag_lock ? 0 : d->matrix.tx - lx; st->drag_dy = st->drag_lock ? 0 : d->matrix.ty - ly;
    st->has_drag_bounds = 0;
    if (argc > 1 && ARG(1).t == T_OBJ) { V2 r = ARG(1); st->has_drag_bounds = 1; st->drag_x0 = v2_tonum(vm, a2_get_s(vm, r, "x")) * 20; st->drag_y0 = v2_tonum(vm, a2_get_s(vm, r, "y")) * 20; st->drag_x1 = st->drag_x0 + v2_tonum(vm, a2_get_s(vm, r, "width")) * 20; st->drag_y1 = st->drag_y0 + v2_tonum(vm, a2_get_s(vm, r, "height")) * 20; }
    a2_dyn_set(vm, vm->global, a2_str(vm, "__dragging"), self);
    return v2_undef();
}
/* dropTarget: the topmost object under the mouse that is not the dragged sprite or inside it */
static void update_drop_target(VM2 *vm, A2Obj *o) {
    A2Obj *arr = a2_new_array(vm), *t = NULL; int i;
    double x = vm->mouse_x * 20.0 / vm->player->zoom, y = vm->mouse_y * 20.0 / vm->player->zoom;
    objects_under(vm, vm->stage_obj, container_clip(vm, vm->stage_obj), arr, x, y, 0);
    for (i = arr->alen - 1; i >= 0 && !t; i--) {
        A2Obj *c = arr->arr[i].t == T_OBJ ? arr->arr[i].u.o : NULL, *q = c; int g = 0, inside = 0;
        while (q && g++ < 256) { if (q == o) { inside = 1; break; } q = a2_parent_of(vm, q); }
        if (c && !inside) t = c;
    }
    a2_dyn_set_hidden(vm, o, a2_str(vm, "__dropTarget"), t ? v2_obj(t) : v2_null());
}
static V2 sp_stopDrag(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); UNUSED; if (o) { a2_disp(o)->dragging = 0; update_drop_target(vm, o); } a2_dyn_set(vm, vm->global, a2_str(vm, "__dragging"), v2_null()); return v2_undef(); }
static V2 sp_dropTarget(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); A2Prop *p; UNUSED; p = o ? a2_dyn_find(o, a2_str(vm, "__dropTarget")) : NULL; return p ? p->v : v2_null(); }
static V2 sp_soundTransform_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Prop *p; UNUSED; p = SELFO ? a2_dyn_find(SELFO, a2_str(vm, "__soundTransform")) : NULL; if (p) { V2 a2_st_copy(VM2 *vm, V2 st); return a2_st_copy(vm, p->v); } return a2_construct_class(vm, vm->c_soundtransform, 0, NULL); }
static V2 sp_soundTransform_set(VM2 *vm, V2 self, int argc, V2 *argv) { V2 a2_st_quantized(VM2 *vm, V2 st); if (SELFO && ARG(0).t == T_OBJ) a2_dyn_set_hidden(vm, SELFO, a2_str(vm, "__soundTransform"), a2_st_quantized(vm, ARG(0))); return v2_undef(); }
static void drag_update(VM2 *vm) {
    A2Prop *p = a2_dyn_find(vm->global, a2_str(vm, "__dragging")); A2Obj *o; A2Disp *st; DOBJ(d, NULL); SWFMatrix w; double a, b, c, dd, e, f, sx, sy, lx, ly;
    if (!p || p->v.t != T_OBJ) return;
    o = p->v.u.o; st = a2_disp(o); d = a2_dobj_of(o);
    if (!st->dragging || !d) return;
    { A2Obj *pa = a2_parent_of(vm, o); if (pa) a2_world_matrix(pa, &w); else swf_matrix_identity(&w); }
    sx = vm->mouse_x * 20.0 / vm->player->zoom; sy = vm->mouse_y * 20.0 / vm->player->zoom;
    if (!matrix_invert(&w, &a, &b, &c, &dd, &e, &f)) return;
    lx = a * sx + b * sy + e + st->drag_dx; ly = c * sx + dd * sy + f + st->drag_dy;
    if (st->has_drag_bounds) { if (lx < st->drag_x0) lx = st->drag_x0; if (lx > st->drag_x1) lx = st->drag_x1; if (ly < st->drag_y0) ly = st->drag_y0; if (ly > st->drag_y1) ly = st->drag_y1; }
    d->matrix.tx = lx; d->matrix.ty = ly;
}

static V2 mc_play(VM2 *vm, V2 self, int argc, V2 *argv) { MovieClip *c = clip_of(dself(self)); UNUSED; if (c) { c->playing = 1; c->advanced = 1; if (hdr_frames(c) > 1) c->prog_played = 1; } return v2_undef(); }
static V2 mc_stop(VM2 *vm, V2 self, int argc, V2 *argv) { MovieClip *c = clip_of(dself(self)); UNUSED; if (c) { c->playing = 0; c->advanced = 0; } return v2_undef(); }

/* ---- scenes & labels (DefineSceneAndFrameLabelData applies to the main timeline) ---- */
static void parse_scenes(VM2 *vm) {
    MovieClip *root = vm->player->root; int i;
    if (vm->scenes_parsed) return;
    vm->scenes_parsed = 1;
    if (!root || !root->tl) return;
    for (i = 0; i < root->tl->ntags; i++) {
        const TagRef *t = &root->tl->tags[i];
        if (t->code == TAG_DefineSceneAndFrameLabelData) {
            const u8 *p = t->data; const u8 *end = t->data + t->len; u32 n, k;
            vm->scene_tag_present = 1;
            n = abc_read_u30(&p); if (n > 10000) n = 10000;
            vm->scenes = (void*)calloc(n ? n : 1, sizeof(*vm->scenes));
            for (k = 0; k < n && p < end; k++) { u32 off = abc_read_u30(&p); size_t l = strlen((const char*)p); vm->scenes[k].offset = (int)off; vm->scenes[k].name = a2_intern(vm, (const char*)p, l); p += l + 1; vm->nscenes++; }
            if (p < end) {
                n = abc_read_u30(&p); if (n > 100000) n = 100000;
                vm->slabels = (void*)calloc(n ? n : 1, sizeof(*vm->slabels));
                for (k = 0; k < n && p < end; k++) { u32 fr = abc_read_u30(&p); size_t l = strlen((const char*)p); vm->slabels[k].frame = (int)fr; vm->slabels[k].name = a2_intern(vm, (const char*)p, l); p += l + 1; vm->nslabels++; }
            }
            break;
        }
    }
}
static int has_scene_data(VM2 *vm, MovieClip *c) { parse_scenes(vm); return c == vm->player->root && vm->nscenes > 0; }
static int scene_count(VM2 *vm, MovieClip *c) { return has_scene_data(vm, c) ? vm->nscenes : 1; }
static int scene_start(VM2 *vm, MovieClip *c, int si) { if (!has_scene_data(vm, c)) return 0; if (si < 0) si = 0; if (si >= vm->nscenes) si = vm->nscenes - 1; return vm->scenes[si].offset; }
static int scene_end(VM2 *vm, MovieClip *c, int si) { if (!has_scene_data(vm, c) || si + 1 >= vm->nscenes) return c->frame_count; return vm->scenes[si + 1].offset; }
static A2Str *scene_name(VM2 *vm, MovieClip *c, int si) { if (has_scene_data(vm, c)) return vm->scenes[si < 0 ? 0 : si >= vm->nscenes ? vm->nscenes - 1 : si].name; if (c == vm->player->root && vm->scene_tag_present) return vm->s_empty; return a2_str(vm, (c && c->tl && (c == vm->player->root || (c->obj2 && ((A2Obj*)c->obj2)->payload && a2_disp((A2Obj*)c->obj2)->is_movie_root))) ? "Scene 1" : ""); }   /* a sprite's single scene has no name */
static int scene_of_frame(VM2 *vm, MovieClip *c, int frame) { int i, r = 0; if (!has_scene_data(vm, c)) return 0; for (i = 0; i < vm->nscenes; i++) if (vm->scenes[i].offset <= frame) r = i; return r; }
static int scene_by_name(VM2 *vm, MovieClip *c, A2Str *name) { int i; if (!has_scene_data(vm, c)) return (c && c->tl && !strcmp(name->s, "Scene 1")) ? 0 : -1; for (i = 0; i < vm->nscenes; i++) if (vm->scenes[i].name == name) return i; return -1; }
/* labels of clip c: for the root with scene data from the tag, else FrameLabel tags.  cb(frame, name) */
typedef struct { A2Str *name; int frame; } LabelEnt;
static int clip_labels(VM2 *vm, MovieClip *c, LabelEnt *out, int max) {
    int n = 0, i, fi = 0;
    if (has_scene_data(vm, c)) { for (i = 0; i < vm->nslabels && n < max; i++) { out[n].name = vm->slabels[i].name; out[n].frame = vm->slabels[i].frame; n++; } return n; }
    if (!c || !c->tl) return 0;
    for (i = 0; i < c->tl->ntags && n < max; i++) { const TagRef *t = &c->tl->tags[i]; if (t->code == TAG_FrameLabel) { out[n].name = a2_intern(vm, (const char*)t->data, strlen((const char*)t->data)); out[n].frame = fi; n++; } else if (t->code == TAG_ShowFrame) fi++; }
    return n;
}
static int label_frame_in_scene(VM2 *vm, MovieClip *c, A2Str *name, int si) {
    LabelEnt L[1024]; int n = clip_labels(vm, c, L, 1024), i, s0 = scene_start(vm, c, si), s1 = scene_end(vm, c, si);
    for (i = 0; i < n; i++) if (L[i].name == name && L[i].frame >= s0 && L[i].frame < s1) return L[i].frame;
    return -1;
}
/* frame argument (label or number) -> absolute 0-based frame; numbers are relative to the scene */
static int frame_arg2(VM2 *vm, MovieClip *c, V2 v, V2 scenev, int *ok) {
    double d; int si; A2Str *sname;
    *ok = 1;
    if (scenev.t != T_UNDEF && scenev.t != T_NULL) {
        sname = v2_tostr(vm, scenev);
        si = scene_by_name(vm, c, sname);
        if (si < 0) { char b[300]; sprintf(b, "Scene %.120s was not found.", sname->s); a2_throw_error(vm, "ArgumentError", b, 2108); *ok = 0; return 0; }
    } else si = scene_of_frame(vm, c, clip_current_frame(c));
    if (v.t == T_OBJ) v = v2_str(v2_tostr(vm, v));
    if (v.t == T_NUM && v.u.n == v.u.n && v.u.n != floor(v.u.n) && v.u.n > -1e300 && v.u.n < 1e300) v = v2_str(v2_tostr(vm, v));   /* a fractional number is a label ("2.5") */
    if (v.t == T_STR) {
        int f;
        d = v2_tonum(vm, v);                 /* numeric strings are frame numbers, not labels */
        if (d == d && (d == floor(d) || d < -1e300 || d > 1e300)) goto number;
        f = label_frame_in_scene(vm, c, v.u.s, si);
        if (f < 0 && (scenev.t == T_UNDEF || scenev.t == T_NULL)) { int k; for (k = 0; k < scene_count(vm, c) && f < 0; k++) f = label_frame_in_scene(vm, c, v.u.s, k); }
        if (f >= 0) return f;
        if (vm->player->debug) { LabelEnt L[64]; int n = clip_labels(vm, c, L, 64), k; fprintf(stderr, "[avm2] label %s missing on char %d (%d frames):", v.u.s->s, c->self ? c->self->char_id : -1, c->tl ? c->tl->frame_count : -1); for (k = 0; k < n; k++) fprintf(stderr, " %s@%d", L[k].name->s, L[k].frame); fprintf(stderr, "\n"); }
        if (vm->swf_version < 11) return 0;   /* older SWFs silently go to frame 1 */
        { char b[300]; sprintf(b, "Frame label %.120s not found in scene %.120s.", v.u.s->s, scene_name(vm, c, si)->s); a2_throw_error(vm, "ArgumentError", b, 2109); *ok = 0; return 0; }
    } else d = v2_tonum(vm, v);
number:
    if (d != d) return scene_start(vm, c, si);
    if (d != floor(d)) d = ceil(d);
    { s32 n; u32 f; if (d > 2147483647.0) d = 2147483647.0; if (d < -2147483648.0) d = -2147483648.0; n = (s32)d; f = (u32)n - 1u; if ((s32)f < 0) return scene_start(vm, c, si); return scene_start(vm, c, si) + (s32)f; }
}
static int clip_in_frame_script(VM2 *vm, MovieClip *c) { int i; for (i = 0; i < vm->fs_depth; i++) if (vm->fs_stack[i] == c) return 1; return 0; }
static void run_pending_frame_scripts(VM2 *vm, MovieClip *c);
/* An explicit AS3 goto acts like a new frame without enterFrame (SWF10+):
 * construct, frameConstructed, frame scripts, exitFrame -- before the
 * calling code continues.  SWF9 instead skips the clip's next advance. */
static int g_inner_goto_depth;
/* a clip whose current frame has a script that has not run during this visit of the frame becomes pending
   (Ruffle check_has_pending_script + last_queued_script_frame) */
static void mark_script_pending(MovieClip *c) {
    DisplayObject *d; A2Obj *o;
    if (!c || c->dead) return;
    o = (A2Obj*)c->obj2;
    if (o && c->tl && c->cur_frame >= 0) { int f = clip_current_frame(c); if (f >= 0 && f < o->nframe_scripts && o->frame_scripts[f].t == T_OBJ && c->last_sf != f + 1) c->frame_changed = 1; }
    for (d = c->children; d; d = d->next) if (d->clip) mark_script_pending(d->clip);
}
static void inner_goto_frame(VM2 *vm, MovieClip *c) {
    if (!vm->root_constructed) { vm->player->nremoved_fs = 0; return; }
    if (c->movie && c->movie->version <= 9) { c->skip_enter = 1; vm->player->nremoved_fs = 0; return; }
    if (vm->fs_depth >= 64 || g_inner_goto_depth >= 32) { vm->player->nremoved_fs = 0; return; }
    g_inner_goto_depth++;
    {   int oph = vm->fs_phase;
    vm->nfsq = 0;   /* scripts registered earlier in the outer frame-script phase are dropped (Ruffle run_inner_goto_frame) */
    vm->fs_phase = 0;   /* construct phase */
    construct_pending(vm);
    a2_broadcast(vm, vm->s_frameConstructed);
    if (vm->throwing) a2_report_uncaught(vm);
    mark_script_pending(vm->player->root);   /* the inner construct phase rechecks every clip's script for its frame */
    vm->fs_phase = 1;
    run_pending_frame_scripts(vm, vm->player->root);
    orphans_frame_scripts(vm);
    { Player *p = vm->player; int i, n = p->nremoved_fs; MovieClip *rm[32];   /* children the goto removed run their pending frame script */
      memcpy(rm, p->removed_fs, (size_t)n * sizeof(MovieClip*)); p->nremoved_fs = 0;
      for (i = 0; i < n; i++) if (!rm[i]->dead) run_pending_frame_scripts(vm, rm[i]); }
    vm->fs_phase = oph; }
    a2_broadcast(vm, vm->s_exitFrame);
    if (vm->throwing) a2_report_uncaught(vm);
    orphans_cleanup(vm);
    g_inner_goto_depth--;
}
static void goto_now(VM2 *vm, MovieClip *c, int f, int play) {
    c->playing = play;
    if (play) c->advanced = 1;
    if (!(f == clip_current_frame(c) && c->cur_frame >= 0)) {
        int oph = vm->fs_phase;
        if (!(c->movie && c->movie->version <= 9)) vm->fs_phase = 0;   /* the goto's placement constructs in the construct phase */
        clip_goto_frame(c, f, play); c->frame_changed = 1; c->advanced = play;
        vm->fs_phase = oph; }
    inner_goto_frame(vm, c);   /* no-op gotos run it too */
}
static void apply_goto(VM2 *vm, MovieClip *c, int f, int play) {
    goto_now(vm, c, f, play);
    if (c->movie && c->movie->version <= 9) construct_pending_under(vm, c);   /* SWF9: children must exist for the frame script */
}
static void request_goto(VM2 *vm, MovieClip *c, int f, int play) {
    if (f >= c->frame_count) f = c->frame_count - 1; if (f < 0) f = 0;
    if (vm->in_fc && c->movie && c->movie->version <= 9 && f == clip_current_frame(c) && c->cur_frame >= 0 && c->obj2 && vm->nfc_reenter < 16) {
        int k, dup = 0; for (k = 0; k < vm->nfc_reenter; k++) if (vm->fc_reenter[k] == (A2Obj*)c->obj2) dup = 1;
        if (!dup) vm->fc_reenter[vm->nfc_reenter++] = (A2Obj*)c->obj2;   /* SWF9: a goto to the current frame during frameConstructed re-enters it (enterFrame again) */
    }
    if (clip_in_frame_script(vm, c) && !(c->movie && c->movie->version <= 9 && f == clip_current_frame(c))) {
        /* goto from the clip's own frame script: deferred until the script returns (last one wins) */
        c->queued_goto = f; c->queued_play = play;
        return;
    }
    goto_now(vm, c, f, play);
}
static V2 mc_goto_impl(VM2 *vm, V2 self, int argc, V2 *argv, int play) {
    MovieClip *c = clip_of(dself(self)); int f, ok;
    if (!c) return v2_undef();
    if (argc < 1) { a2_throw_error(vm, "ArgumentError", "Too few arguments were specified; got 0, 1 expected.", 2001); return v2_undef(); }
    f = frame_arg2(vm, c, ARG(0), argc > 1 ? ARG(1) : v2_undef(), &ok);
    if (!ok || vm->throwing) return v2_undef();
    request_goto(vm, c, f, play);
    return v2_undef();
}
static V2 mc_gotoAndPlay(VM2 *vm, V2 self, int argc, V2 *argv) { MovieClip *c = clip_of(dself(self)); if (c && hdr_frames(c) > 1) c->prog_played = 1; return mc_goto_impl(vm, self, argc, argv, 1); }
static V2 mc_gotoAndStop(VM2 *vm, V2 self, int argc, V2 *argv) { return mc_goto_impl(vm, self, argc, argv, 0); }
static V2 mc_nextFrame(VM2 *vm, V2 self, int argc, V2 *argv) { MovieClip *c = clip_of(dself(self)); UNUSED; if (c) request_goto(vm, c, clip_current_frame(c) + 1, 0); return v2_undef(); }
static V2 mc_prevFrame(VM2 *vm, V2 self, int argc, V2 *argv) { MovieClip *c = clip_of(dself(self)); UNUSED; if (c) request_goto(vm, c, clip_current_frame(c) - 1, 0); return v2_undef(); }
static V2 mc_nextScene(VM2 *vm, V2 self, int argc, V2 *argv) {
    MovieClip *c = clip_of(dself(self)); int si; UNUSED; if (!c) return v2_undef();
    si = scene_of_frame(vm, c, clip_current_frame(c));
    if (si + 1 < scene_count(vm, c)) si++;             /* last scene: behaves as gotoAndPlay(1) of the scene */
    request_goto(vm, c, scene_start(vm, c, si), 1); return v2_undef();
}
static V2 mc_prevScene(VM2 *vm, V2 self, int argc, V2 *argv) {
    MovieClip *c = clip_of(dself(self)); int si; UNUSED; if (!c) return v2_undef();
    si = scene_of_frame(vm, c, clip_current_frame(c));
    if (si > 0) si--;
    request_goto(vm, c, scene_start(vm, c, si), 1); return v2_undef();
}
static int mc_frame_for_script(VM2 *vm, MovieClip *c) {
    int f = c->goto_busy ? c->goto_target : clip_current_frame(c);   /* during a goto Flash already reports the target frame */
    (void)vm;
    return f;
}
static V2 mc_currentFrame(VM2 *vm, V2 self, int argc, V2 *argv) { MovieClip *c = clip_of(dself(self)); int f; UNUSED; if (!(c && c->tl)) return v2_int(0); f = mc_frame_for_script(vm, c); return v2_int(f - scene_start(vm, c, scene_of_frame(vm, c, f)) + 1); }
/* the frame count the header declares (Ruffle header_frames); the timeline plays its real frames */
static int hdr_frames(MovieClip *c) { return (c->tl && c->tl->has_hdr && c->tl == &c->movie->root ? c->movie->root.hdr_frames : (c->tl && c->tl->has_hdr ? c->tl->hdr_frames : c->frame_count)); }
static V2 mc_totalFrames(VM2 *vm, V2 self, int argc, V2 *argv) { MovieClip *c = clip_of(dself(self)); UNUSED; return v2_int(c && (c->frame_count || c->tl) ? hdr_frames(c) : 1); }
static V2 mc_framesLoaded(VM2 *vm, V2 self, int argc, V2 *argv) { MovieClip *c = clip_of(dself(self)); int h; UNUSED; if (!(c && (c->frame_count || c->tl))) return v2_int(1); h = hdr_frames(c); return v2_int(c->frame_count < h ? c->frame_count : h); }
static V2 mc_isPlaying(VM2 *vm, V2 self, int argc, V2 *argv) { MovieClip *c = clip_of(dself(self)); UNUSED; return v2_bool(c && c->tl && c->prog_played && c->playing); }
static V2 mc_currentLabel(VM2 *vm, V2 self, int argc, V2 *argv) {
    MovieClip *c = clip_of(dself(self)); LabelEnt L[1024]; int n, i, cur, si, s0, best = -1; A2Str *bn = NULL; UNUSED;
    if (!c || !c->tl) return v2_null();
    cur = clip_current_frame(c); si = scene_of_frame(vm, c, cur); s0 = scene_start(vm, c, si);
    n = clip_labels(vm, c, L, 1024);
    (void)s0; for (i = 0; i < n; i++) if (L[i].frame <= cur && L[i].frame >= best) { best = L[i].frame; bn = L[i].name; }
    return bn ? v2_str(bn) : v2_null();
}
static V2 mc_currentFrameLabel(VM2 *vm, V2 self, int argc, V2 *argv) {
    MovieClip *c = clip_of(dself(self)); LabelEnt L[1024]; int n, i, cur; UNUSED;
    if (!c || !c->tl) return v2_null();
    cur = clip_current_frame(c); n = clip_labels(vm, c, L, 1024);
    for (i = n - 1; i >= 0; i--) if (L[i].frame == cur) return v2_str(L[i].name);
    return v2_null();
}
static V2 scene_labels_array(VM2 *vm, MovieClip *c, int si) {
    A2Obj *arr = a2_new_array(vm); LabelEnt L[1024]; int n, i, s0, s1; A2Class *fl = a2_class_find_s(vm, "flash.display::FrameLabel");
    if (!c || !c->tl) return v2_obj(arr);
    s0 = scene_start(vm, c, si); s1 = scene_end(vm, c, si);
    n = clip_labels(vm, c, L, 1024);
    for (i = 0; i < n; i++) if (L[i].frame >= s0 && L[i].frame < s1) { V2 a[2]; a[0] = v2_str(L[i].name); a[1] = v2_int(L[i].frame - s0 + 1); a2_arr_push(vm, arr, a2_construct_class(vm, fl, 2, a)); }
    return v2_obj(arr);
}
static V2 mc_currentLabels(VM2 *vm, V2 self, int argc, V2 *argv) { MovieClip *c = clip_of(dself(self)); UNUSED; if (!c) return v2_obj(a2_new_array(vm)); return scene_labels_array(vm, c, scene_of_frame(vm, c, clip_current_frame(c))); }
static V2 make_scene(VM2 *vm, MovieClip *c, int si) {
    A2Class *sc = a2_class_find_s(vm, "flash.display::Scene"); V2 a[3];
    a[0] = v2_str(scene_name(vm, c, si)); a[1] = scene_labels_array(vm, c, si);
    a[2] = v2_int(c ? (scene_end(vm, c, si) - scene_start(vm, c, si)) : 1);
    if (c && !c->tl) a[2] = v2_int(1);
    return a2_construct_class(vm, sc, 3, a);
}
static V2 mc_currentScene(VM2 *vm, V2 self, int argc, V2 *argv) { MovieClip *c = clip_of(dself(self)); UNUSED; return make_scene(vm, c, c ? scene_of_frame(vm, c, clip_current_frame(c)) : 0); }
static V2 mc_scenes(VM2 *vm, V2 self, int argc, V2 *argv) { MovieClip *c = clip_of(dself(self)); A2Obj *arr = a2_new_array(vm); int i, n = c ? scene_count(vm, c) : 1; UNUSED; for (i = 0; i < n; i++) a2_arr_push(vm, arr, make_scene(vm, c, i)); return v2_obj(arr); }
static V2 mc_enabled_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); UNUSED; return v2_bool(o ? a2_disp(o)->btn_enabled : 1); }
static V2 mc_enabled_set(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); if (o) a2_disp(o)->btn_enabled = BOOL(0); return v2_undef(); }
static V2 mc_trackAsMenu_get(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_bool(0); }
static V2 mc_addFrameScript(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = dself(self); int i;
    if (vm->player->debug > 1) fprintf(stderr, "[avm2] addFrameScript on %s %p\n", SELFO && SELFO->cls ? SELFO->cls->name->s : "?", (void*)SELFO);
    if (argc == 0 || (argc & 1)) {   /* frame/function pairs */
        char b[96]; sprintf(b, "Too few arguments were specified; got %d, %d expected.", argc, argc + 1);
        a2_throw_error(vm, "ArgumentError", b, 2001);
        return v2_undef();
    }
    if (!o) return v2_undef();
    for (i = 0; i + 1 < argc; i += 2) {
        int f = v2_toint(vm, argv[i]);
        if (f < 0 || f > 100000) continue;
        if (f >= o->nframe_scripts) { int n = f + 1, k; V2 *nf = (V2*)realloc(o->frame_scripts, (size_t)n * sizeof(V2)); if (!nf) return v2_undef(); for (k = o->nframe_scripts; k < n; k++) nf[k] = v2_undef(); o->frame_scripts = nf; o->nframe_scripts = n; }
        {   int had = o->frame_scripts[f].t == T_OBJ;
            o->frame_scripts[f] = argv[i + 1];
            /* a script registered for the current frame runs this frame (Ruffle register_frame_script):
               during the frame-script phase it joins the cleanup queue run after the display-list pass */
            if (argv[i + 1].t == T_OBJ) { MovieClip *c = clip_of(o); if (c && c->tl && c->cur_frame >= 0 && f == clip_current_frame(c)) {
                if (vm->fs_phase) { if (vm->nfsq < 256) vm->fsq[vm->nfsq++] = o; if (!had && c->born_fs != vm->frame_no + 1) c->frame_changed = 0; }   /* a clip born in this script pass also runs it from the pass itself */
                else if (!clip_in_frame_script(vm, c)) { c->frame_changed = 1; c->last_sf = 0; } } }
        }
    }
    return v2_undef();
}
void a2_run_frame_scripts(VM2 *vm, A2Obj *mc, int frame) {
    if (vm->player->debug) fprintf(stderr, "[avm2] frame script %s %p frame %d (have %d)\n", mc ? mc->cls->name->s : "?", (void*)mc, frame, mc ? mc->nframe_scripts : 0);
    if (!mc || frame < 0 || frame >= mc->nframe_scripts) return;
    if (mc->frame_scripts[frame].t == T_OBJ) {
        MovieClip *c = clip_of(mc);
        if (c && c->tl && c->last_sf == frame + 1) return;   /* already ran for this visit of the frame (Ruffle last_queued_script_frame) */
        if (c && c->tl) c->last_sf = frame + 1;
        vm->in_frame_script++;
        if (vm->fs_depth < 256) vm->fs_stack[vm->fs_depth++] = c;
        a2_call(vm, mc->frame_scripts[frame], v2_obj(mc), 0, NULL); if (vm->throwing) a2_report_uncaught(vm);
        if (vm->fs_depth > 0) vm->fs_depth--;
        vm->in_frame_script--;
        if (c && c->queued_goto >= 0 && !c->dead) { int f = c->queued_goto, play = c->queued_play; c->queued_goto = -1; apply_goto(vm, c, f, play); }
    }
}

/* ---------------------------------------------------------------- */
/* Graphics                                                         */

static A2Obj *gfx_owner(VM2 *vm, V2 self) { A2Prop *p = SELFO ? a2_dyn_find(SELFO, a2_str(vm, "__owner")) : NULL; return p && p->v.t == T_OBJ ? p->v.u.o : NULL; }
static ShapeDef *gfx_shape(VM2 *vm, V2 self, A2Disp **stp) { A2Obj *o = gfx_owner(vm, self); A2Disp *st; if (!o) return NULL; st = a2_disp(o); if (!st->drawing) { st->drawing = (ShapeDef*)calloc(1, sizeof(ShapeDef)); st->drawing->version = 4; } if (stp) *stp = st; return st->drawing; }
static void add_edge(ShapeDef *s, ShapeEdge *e) { if (s->nedges == s->capedges) { int nc = s->capedges ? s->capedges * 2 : 32; ShapeEdge *ne = (ShapeEdge*)realloc(s->edges, (size_t)nc * sizeof(ShapeEdge)); if (!ne) return; s->edges = ne; s->capedges = nc; } s->edges[s->nedges++] = *e; }
static int add_fill(ShapeDef *d, FillStyle *f) { if (d->nfills == d->capfills) { int nc = d->capfills ? d->capfills * 2 : 8; FillStyle *nf = (FillStyle*)realloc(d->fills, (size_t)nc * sizeof(FillStyle)); if (!nf) return 0; d->fills = nf; d->capfills = nc; } d->fills[d->nfills++] = *f; return d->nfills; }
static int add_line(ShapeDef *d, LineStyle *l) { if (d->nlines == d->caplines) { int nc = d->caplines ? d->caplines * 2 : 8; LineStyle *nl = (LineStyle*)realloc(d->lines, (size_t)nc * sizeof(LineStyle)); if (!nl) return 0; d->lines = nl; d->caplines = nc; } d->lines[d->nlines++] = *l; return d->nlines; }
/* the fill is closed with a segment the current line strokes too (Ruffle Drawing::close_path) */
static void close_fill(ShapeDef *d, A2Disp *st) {
    if (st->draw_open && (st->draw_x != st->start_x || st->draw_y != st->start_y)) { ShapeEdge e; memset(&e, 0, sizeof(e)); e.x0 = st->draw_x; e.y0 = st->draw_y; e.x1 = st->start_x; e.y1 = st->start_y; e.fill1 = st->draw_fill; e.line = st->draw_line; add_edge(d, &e); st->draw_x = st->start_x; st->draw_y = st->start_y; }
    st->draw_open = 0;
}
int a2_drawing_pending_close(MovieClip *c, ShapeEdge *e) {
    A2Obj *o = c ? (A2Obj*)c->obj2 : NULL; A2Disp *st = o ? (A2Disp*)o->payload : NULL;
    if (!st || !st->drawing || !st->draw_fill || !st->draw_open || (st->draw_x == st->start_x && st->draw_y == st->start_y)) return 0;
    memset(e, 0, sizeof(*e));
    e->x0 = st->draw_x; e->y0 = st->draw_y; e->x1 = st->start_x; e->y1 = st->start_y; e->fill1 = st->draw_fill; e->line = st->draw_line;
    return 1;
}
static V2 g_clear(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = gfx_owner(vm, self); A2Disp *st; (void)argc; (void)argv; if (!o) return v2_undef(); st = a2_disp(o); if (st->drawing) { shape_free(st->drawing); st->drawing = NULL; } st->draw_fill = st->draw_line = 0; st->draw_open = 0; st->draw_x = st->draw_y = 0; return v2_undef(); }
static V2 g_beginFill(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Disp *st; ShapeDef *d = gfx_shape(vm, self, &st); FillStyle f; u32 rgb; double a;
    if (!d) return v2_undef();
    close_fill(d, st);
    memset(&f, 0, sizeof(f)); swf_matrix_identity(&f.matrix);
    rgb = v2_touint(vm, ARG(0)); a = argc > 1 ? NUM(1) : 1; if (a < 0) a = 0; if (a > 1) a = 1;
    f.type = FILL_SOLID; f.color.r = (u8)(rgb >> 16); f.color.g = (u8)(rgb >> 8); f.color.b = (u8)rgb; f.color.a = (u8)(a * 255);   /* truncated, as Ruffle */
    st->draw_fill = add_fill(d, &f); st->draw_open = 1; st->start_x = st->draw_x; st->start_y = st->draw_y;
    return v2_undef();
}
static float a2_fixed16(double v) {
    float f = (float)v * 65536.0f;
    if (f != f) return 0;
    if (f >= 2147483647.0f) return 32768.0f;
    if (f <= -2147483648.0f) return -32768.0f;
    return (float)((s32)f) / 65536.0f;
}
/* the gradient of beginGradientFill / lineGradientStyle */
static void gradient_fill_args(VM2 *vm, int argc, V2 *argv, FillStyle *fp) {
    FillStyle f; A2Str *type = STR(0); V2 cols = ARG(1), alphas = ARG(2), ratios = ARG(3); int i, n;
    memset(&f, 0, sizeof(f)); swf_matrix_identity(&f.matrix);
    f.type = !strcmp(type->s, "radial") ? FILL_RADIAL : FILL_LINEAR;
    n = cols.t == T_OBJ ? cols.u.o->alen : 0; if (n > MAX_GRAD) n = MAX_GRAD;
    for (i = 0; i < n; i++) { u32 rgb = v2_touint(vm, cols.u.o->arr[i]); double a = alphas.t == T_OBJ && i < alphas.u.o->alen ? v2_tonum(vm, alphas.u.o->arr[i]) : 1; f.grad.color[i].r = (u8)(rgb >> 16); f.grad.color[i].g = (u8)(rgb >> 8); f.grad.color[i].b = (u8)rgb; f.grad.color[i].a = (u8)(!(a > 0) ? 0 : a > 1 ? 255 : a * 255); f.grad.ratio[i] = (u8)(ratios.t == T_OBJ && i < ratios.u.o->alen ? v2_toint(vm, ratios.u.o->arr[i]) : i * 255 / (n > 1 ? n - 1 : 1)); }
    f.grad.n = n;
    if (argc > 4 && ARG(4).t == T_OBJ) { SWFMatrix m; a2_matrix_from_obj(vm, ARG(4), &m); /* gradient space is 32768 twips wide (-16384..16384) */ f.matrix = m; f.matrix.sx *= 1.0; }
    else { f.matrix.sx = f.matrix.sy = 1.0 / 20.0 * 20.0; }
    if (argc > 5 && ARG(5).t == T_STR) { const char *s = ARG(5).u.s->s; f.grad.spread = !strcmp(s, "reflect") ? 1 : !strcmp(s, "repeat") ? 2 : 0; }
    if (argc > 6 && ARG(6).t == T_STR) f.grad.interp = !strcmp(ARG(6).u.s->s, "linearRGB") ? 1 : 0;
    if (argc > 7) {
        double fp = NUM(7), f8 = fp * 256.0;
        if (fp != 0 && f.type == FILL_RADIAL) f.type = FILL_FOCAL;
        /* stored as Fixed8 (Fixed8::from_f64: truncated, saturated) */
        f.grad.focal = (f8 != f8 ? 0 : f8 >= 32767.0 ? 32767 : f8 <= -32768.0 ? -32768 : (double)(int)f8) / 256.0;
    }
    /* stored as an swf::Matrix: Fixed16 components (Fixed16::from_f32 truncates) */
    f.matrix.sx = a2_fixed16(f.matrix.sx); f.matrix.r0 = a2_fixed16(f.matrix.r0); f.matrix.r1 = a2_fixed16(f.matrix.r1); f.matrix.sy = a2_fixed16(f.matrix.sy);
    *fp = f;
}
static V2 g_beginGradientFill(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Disp *st; ShapeDef *d = gfx_shape(vm, self, &st); FillStyle f;
    if (!d) return v2_undef();
    close_fill(d, st);
    gradient_fill_args(vm, argc, argv, &f);
    st->draw_fill = add_fill(d, &f); st->draw_open = 1; st->start_x = st->draw_x; st->start_y = st->draw_y;
    return v2_undef();
}
static void bitmap_fill_args(VM2 *vm, int argc, V2 *argv, FillStyle *fp);
static V2 g_beginBitmapFill(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Disp *st; ShapeDef *d = gfx_shape(vm, self, &st); FillStyle f;
    if (!d) return v2_undef();
    close_fill(d, st);
    bitmap_fill_args(vm, argc, argv, &f);
    st->draw_fill = add_fill(d, &f); st->draw_open = 1; st->start_x = st->draw_x; st->start_y = st->draw_y;
    return v2_undef();
}
/* the bitmap of beginBitmapFill / lineBitmapStyle (bitmap, matrix, repeat, smooth) */
static void bitmap_fill_args(VM2 *vm, int argc, V2 *argv, FillStyle *fp) {
    FillStyle f; extern int a2_bitmapdata_register(VM2 *vm, A2Obj *bd);
    memset(&f, 0, sizeof(f)); swf_matrix_identity(&f.matrix); f.matrix.sx = f.matrix.sy = 20.0;
    f.type = (argc > 2 && !BOOL(2)) ? FILL_BITMAP_CLIP : FILL_BITMAP_REPEAT;
    if (argc > 3 && BOOL(3)) f.type = f.type == FILL_BITMAP_CLIP ? FILL_BITMAP_CLIP : FILL_BITMAP_REPEAT; else f.type = f.type == FILL_BITMAP_CLIP ? FILL_BITMAP_CLIP_NS : FILL_BITMAP_REPEAT_NS;
    f.bitmap_id = ARG(0).t == T_OBJ ? a2_bitmapdata_register(vm, ARG(0).u.o) : -1;
    if (argc > 1 && ARG(1).t == T_OBJ) { SWFMatrix m; a2_matrix_from_obj(vm, ARG(1), &m); m.tx *= 1; f.matrix = m; f.matrix.sx *= 20; f.matrix.sy *= 20; f.matrix.r0 *= 20; f.matrix.r1 *= 20; f.matrix.tx *= 1; f.matrix.ty *= 1; }
    *fp = f;
}
/* lineGradientStyle / lineBitmapStyle: the current line style with that fill, as a new line (Ruffle set_line_fill_style) */
static void line_fill_style(ShapeDef *d, A2Disp *st, const FillStyle *f) {
    LineStyle l;
    if (!st->draw_line || st->draw_line > d->nlines) return;
    l = d->lines[st->draw_line - 1]; l.has_fill = 1; l.fill = *f;
    st->draw_line = add_line(d, &l);
}
static V2 g_lineBitmapStyle(VM2 *vm, V2 self, int argc, V2 *argv) { A2Disp *st; ShapeDef *d = gfx_shape(vm, self, &st); FillStyle f; if (!d || !st->draw_line) return v2_undef(); bitmap_fill_args(vm, argc, argv, &f); line_fill_style(d, st, &f); return v2_undef(); }
static V2 g_endFill(VM2 *vm, V2 self, int argc, V2 *argv) { A2Disp *st; ShapeDef *d = gfx_shape(vm, self, &st); (void)argc; (void)argv; if (!d) return v2_undef(); close_fill(d, st); st->draw_fill = 0; return v2_undef(); }
static V2 g_lineStyle(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Disp *st; ShapeDef *d = gfx_shape(vm, self, &st); LineStyle l; u32 rgb; double a;
    if (!d) return v2_undef();
    if (argc == 0 || ARG(0).t == T_UNDEF || NUM(0) != NUM(0)) { st->draw_line = 0; return v2_undef(); }
    memset(&l, 0, sizeof(l));
    l.width = (int)(NUM(0) * 20.0); if (l.width < 1 && NUM(0) >= 0) l.width = 1;
    rgb = argc > 1 ? v2_touint(vm, ARG(1)) : 0; a = argc > 2 ? NUM(2) : 1; if (a < 0) a = 0; if (a > 1) a = 1;
    l.color.r = (u8)(rgb >> 16); l.color.g = (u8)(rgb >> 8); l.color.b = (u8)rgb; l.color.a = (u8)(a * 255);
    if (argc > 4 && ARG(4).t == T_STR) { const char *s = ARG(4).u.s->s; if (!strcmp(s, "none")) l.no_hscale = l.no_vscale = 1; else if (!strcmp(s, "vertical")) l.no_hscale = 1; else if (!strcmp(s, "horizontal")) l.no_vscale = 1; }
    if (argc > 5 && ARG(5).t == T_STR) { const char *s = ARG(5).u.s->s; l.start_cap = l.end_cap = !strcmp(s, "none") ? 1 : !strcmp(s, "square") ? 2 : 0; }
    if (argc > 6 && ARG(6).t == T_STR) { const char *s = ARG(6).u.s->s; l.join = !strcmp(s, "bevel") ? 1 : !strcmp(s, "miter") ? 2 : 0; }
    {   /* miterLimit (default 3) as an 8.8 fixed-point number */
        double m = argc > 7 ? NUM(7) : 3.0;
        l.miter = m != m ? 0.0 : m >= 128.0 ? 32767.0 / 256.0 : m <= -128.0 ? -128.0 : (double)(s16)(m * 256.0) / 256.0;
    }
    st->draw_line = add_line(d, &l);
    return v2_undef();
}
static V2 g_lineGradientStyle(VM2 *vm, V2 self, int argc, V2 *argv) { A2Disp *st; ShapeDef *d = gfx_shape(vm, self, &st); FillStyle f; if (!d || !st->draw_line) return v2_undef(); gradient_fill_args(vm, argc, argv, &f); line_fill_style(d, st, &f); return v2_undef(); }
static V2 g_moveTo(VM2 *vm, V2 self, int argc, V2 *argv) { A2Disp *st; ShapeDef *d = gfx_shape(vm, self, &st); if (!d) return v2_undef(); close_fill(d, st); st->draw_x = px_tw(NUM(0)); st->draw_y = px_tw(NUM(1)); if (st->draw_fill) { st->draw_open = 1; st->start_x = st->draw_x; st->start_y = st->draw_y; } return v2_undef(); }
static V2 g_lineTo(VM2 *vm, V2 self, int argc, V2 *argv) { A2Disp *st; ShapeDef *d = gfx_shape(vm, self, &st); ShapeEdge e; if (!d) return v2_undef(); memset(&e, 0, sizeof(e)); e.x0 = st->draw_x; e.y0 = st->draw_y; e.x1 = px_tw(NUM(0)); e.y1 = px_tw(NUM(1)); e.fill1 = st->draw_fill; e.line = st->draw_line; add_edge(d, &e); st->draw_x = e.x1; st->draw_y = e.y1; if (st->draw_fill) st->draw_open = 1; return v2_undef(); }
static V2 g_curveTo(VM2 *vm, V2 self, int argc, V2 *argv) { A2Disp *st; ShapeDef *d = gfx_shape(vm, self, &st); ShapeEdge e; if (!d) return v2_undef(); memset(&e, 0, sizeof(e)); e.x0 = st->draw_x; e.y0 = st->draw_y; e.is_curve = 1; e.cx = px_tw(NUM(0)); e.cy = px_tw(NUM(1)); e.x1 = px_tw(NUM(2)); e.y1 = px_tw(NUM(3)); e.fill1 = st->draw_fill; e.line = st->draw_line; add_edge(d, &e); st->draw_x = e.x1; st->draw_y = e.y1; if (st->draw_fill) st->draw_open = 1; return v2_undef(); }
static V2 g_cubicCurveTo(VM2 *vm, V2 self, int argc, V2 *argv) {
    /* approximate a cubic with 8 line segments */
    A2Disp *st; ShapeDef *d = gfx_shape(vm, self, &st); double x0, y0, x1 = NUM(0), y1 = NUM(1), x2 = NUM(2), y2 = NUM(3), x3 = NUM(4), y3 = NUM(5); int i;
    int first;
    if (!d) return v2_undef();
    x0 = st->draw_x / 20.0; y0 = st->draw_y / 20.0;
    first = d->nedges;
    for (i = 1; i <= 8; i++) { double t = i / 8.0, u = 1 - t; double x = u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3, y = u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3; V2 a[2]; a[0] = v2_num(x); a[1] = v2_num(y); g_lineTo(vm, self, 2, a); }
    if (d->nedges - first == 8) {
        /* the lines stand for the curve in bounds and hit tests; the renderer draws the cubic (as lyon) */
        ShapeEdge *e = &d->edges[first];
        e->cubic = 8; e->cx = px_tw(x1); e->cy = px_tw(y1); e->c2x = px_tw(x2); e->c2y = px_tw(y2);
        for (i = 1; i < 8; i++) d->edges[first + i].cubic = -1;
        d->edges[first + 7].x1 = px_tw(x3); d->edges[first + 7].y1 = px_tw(y3);
    }
    return v2_undef();
}
static void draw_rect_tw(VM2 *vm, V2 self, double x, double y, double w, double h) { V2 a[2]; a[0] = v2_num(x); a[1] = v2_num(y); g_moveTo(vm, self, 2, a); a[0] = v2_num(x + w); g_lineTo(vm, self, 2, a); a[1] = v2_num(y + h); g_lineTo(vm, self, 2, a); a[0] = v2_num(x); g_lineTo(vm, self, 2, a); a[1] = v2_num(y); g_lineTo(vm, self, 2, a); }
static V2 g_drawRect(VM2 *vm, V2 self, int argc, V2 *argv) { draw_rect_tw(vm, self, NUM(0), NUM(1), NUM(2), NUM(3)); return v2_undef(); }
static void arc_quarter(VM2 *vm, V2 self, double cx, double cy, double rx, double ry, double a0, double a1) {
    /* quadratic approximation of an elliptical arc (2 segments per quarter) */
    int i; double n = 2;
    for (i = 0; i < 2; i++) { double t0 = a0 + (a1 - a0) * i / n, t1 = a0 + (a1 - a0) * (i + 1) / n, tm = (t0 + t1) / 2; double k = 1 / cos((t1 - t0) / 2); V2 a[4];
        a[0] = v2_num(cx + rx * cos(tm) * k); a[1] = v2_num(cy + ry * sin(tm) * k); a[2] = v2_num(cx + rx * cos(t1)); a[3] = v2_num(cy + ry * sin(t1)); g_curveTo(vm, self, 4, a); }
}
/* Ruffle's draw_round_rect_internal (Flash's rounded rectangle: from the
   bottom right, eight 45 degree quadratic segments) */
static double rr_min(double a, double b) { return a != a ? b : b != b ? a : a < b ? a : b; }
static void rr_move(VM2 *vm, V2 self, double x, double y) { V2 a[2]; a[0] = v2_num(x); a[1] = v2_num(y); g_moveTo(vm, self, 2, a); }
static void rr_line(VM2 *vm, V2 self, double x, double y) { V2 a[2]; a[0] = v2_num(x); a[1] = v2_num(y); g_lineTo(vm, self, 2, a); }
static void rr_curve(VM2 *vm, V2 self, double cx, double cy, double x, double y) { V2 a[4]; a[0] = v2_num(cx); a[1] = v2_num(cy); a[2] = v2_num(x); a[3] = v2_num(y); g_curveTo(vm, self, 4, a); }
static void round_rect_internal(VM2 *vm, V2 self, double x, double y, double w, double h,
                                double tlw, double tlh, double trw, double trh, double blw, double blh, double brw, double brh) {
    static const double U[5][2] = { { 1.0, 0.0 }, { 1.0, 0.41421356237309503 }, { 0.70710678118654752, 0.70710678118654752 }, { 0.4142135623730951, 1.0 }, { 0.00000000000000006123233995736766, 1.0 } };
    double brx, bry, blx, bly, tlx, tly, trx, try_;
    tlw = rr_min(tlw, w / 2.0); tlh = rr_min(tlh, h / 2.0); trw = rr_min(trw, w / 2.0); trh = rr_min(trh, h / 2.0);
    blw = rr_min(blw, w / 2.0); blh = rr_min(blh, h / 2.0); brw = rr_min(brw, w / 2.0); brh = rr_min(brh, h / 2.0);
    brx = x + w - brw; bry = y + h - brh; blx = x + blw; bly = y + h - blh; tlx = x + tlw; tly = y + tlh; trx = x + w - trw; try_ = y + trh;
    rr_move(vm, self, brx + brw * U[2][0], bry + brh * U[2][1]);
    rr_curve(vm, self, brx + brw * U[3][0], bry + brh * U[3][1], brx + brw * U[4][0], bry + brh * U[4][1]);
    rr_line(vm, self, blx + blw * U[4][0], bly + blh * U[4][1]);
    rr_curve(vm, self, blx - blw * U[3][0], bly + blh * U[3][1], blx - blw * U[2][0], bly + blh * U[2][1]);
    rr_curve(vm, self, blx - blw * U[1][0], bly + blh * U[1][1], blx - blw * U[0][0], bly + blh * U[0][1]);
    rr_line(vm, self, tlx - tlw * U[0][0], tly - tlh * U[0][1]);
    rr_curve(vm, self, tlx - tlw * U[1][0], tly - tlh * U[1][1], tlx - tlw * U[2][0], tly - tlh * U[2][1]);
    rr_curve(vm, self, tlx - tlw * U[3][0], tly - tlh * U[3][1], tlx - tlw * U[4][0], tly - tlh * U[4][1]);
    rr_line(vm, self, trx + trw * U[4][0], try_ - trh * U[4][1]);
    rr_curve(vm, self, trx + trw * U[3][0], try_ - trh * U[3][1], trx + trw * U[2][0], try_ - trh * U[2][1]);
    rr_curve(vm, self, trx + trw * U[1][0], try_ - trh * U[1][1], trx + trw * U[0][0], try_ - trh * U[0][1]);
    rr_line(vm, self, brx + brw * U[0][0], bry + brh * U[0][1]);
    rr_curve(vm, self, brx + brw * U[1][0], bry + brh * U[1][1], brx + brw * U[2][0], bry + brh * U[2][1]);
}
static V2 g_drawEllipse(VM2 *vm, V2 self, int argc, V2 *argv) {
    double x = NUM(0), y = NUM(1), w = NUM(2), h = NUM(3);
    round_rect_internal(vm, self, x, y, w, h, w / 2, h / 2, w / 2, h / 2, w / 2, h / 2, w / 2, h / 2);
    return v2_undef();
}
static V2 g_drawCircle(VM2 *vm, V2 self, int argc, V2 *argv) {
    double x = NUM(0), y = NUM(1), r = NUM(2);
    round_rect_internal(vm, self, x - r, y - r, r * 2, r * 2, r, r, r, r, r, r, r, r);
    return v2_undef();
}
static V2 g_drawRoundRect(VM2 *vm, V2 self, int argc, V2 *argv) {
    double x = NUM(0), y = NUM(1), w = NUM(2), h = NUM(3), ew = NUM(4), eh = argc > 5 ? NUM(5) : ew;
    if (eh != eh) eh = ew;
    round_rect_internal(vm, self, x, y, w, h, ew / 2, eh / 2, ew / 2, eh / 2, ew / 2, eh / 2, ew / 2, eh / 2);
    return v2_undef();
}
static V2 g_drawRoundRectComplex(VM2 *vm, V2 self, int argc, V2 *argv) {
    double tl = NUM(4), tr = NUM(5), bl = NUM(6), br = NUM(7);
    round_rect_internal(vm, self, NUM(0), NUM(1), NUM(2), NUM(3), tl, tl, tr, tr, bl, bl, br, br);
    return v2_undef();
}
static V2 g_drawPath(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 cmds = ARG(0), data = ARG(1); int i, di = 0; A2Obj *c, *d;
    if (argc > 2 && !(argv[2].t == T_STR && (!strcmp(argv[2].u.s->s, "evenOdd") || !strcmp(argv[2].u.s->s, "nonZero")))) { a2_throw_error(vm, "ArgumentError", "Parameter winding must be one of the accepted values.", 2008); return v2_undef(); }
    if (cmds.t != T_OBJ || data.t != T_OBJ) return v2_undef();
    c = cmds.u.o; d = data.u.o;
    if (d->alen == 0) return v2_undef();   /* Flash ignores the commands when there are no coordinates */
    {   /* the winding rule of the path's fill */
        A2Disp *st; ShapeDef *sd = gfx_shape(vm, self, &st);
        if (sd && st->draw_fill > 0 && st->draw_fill <= sd->nfills) sd->fills[st->draw_fill - 1].winding = argc > 2 && argv[2].t == T_STR && !strcmp(argv[2].u.s->s, "nonZero") ? 2 : 1;
    }
    if (c->alen && (d->alen & 1)) { a2_throw_error(vm, "ArgumentError", "One of the parameters is invalid.", 2004); return v2_undef(); }   /* odd coordinate count */
    for (i = 0; i < c->alen; i++) {
        int cmd = v2_toint(vm, c->arr[i]); V2 a[6];
        switch (cmd) {
        case 1: if (di + 1 < d->alen) { a[0] = d->arr[di]; a[1] = d->arr[di + 1]; g_moveTo(vm, self, 2, a); } di += 2; break;
        case 2: if (di + 1 < d->alen) { a[0] = d->arr[di]; a[1] = d->arr[di + 1]; g_lineTo(vm, self, 2, a); } di += 2; break;
        case 3: if (di + 3 < d->alen) { a[0] = d->arr[di]; a[1] = d->arr[di + 1]; a[2] = d->arr[di + 2]; a[3] = d->arr[di + 3]; g_curveTo(vm, self, 4, a); } di += 4; break;
        case 4: if (di + 3 < d->alen) { a[0] = d->arr[di + 2]; a[1] = d->arr[di + 3]; g_moveTo(vm, self, 2, a); } di += 4; break;   /* WIDE_MOVE_TO */
        case 5: if (di + 3 < d->alen) { a[0] = d->arr[di + 2]; a[1] = d->arr[di + 3]; g_lineTo(vm, self, 2, a); } di += 4; break;   /* WIDE_LINE_TO */
        case 6: if (di + 5 < d->alen) { int k; for (k = 0; k < 6; k++) a[k] = d->arr[di + k]; g_cubicCurveTo(vm, self, 6, a); } di += 6; break;
        case 0: break;
        default: return v2_undef();   /* an unknown command ends the path (Flash) */
        }
    }
    return v2_undef();
}
/* ---- IGraphicsData (flash.display.Graphics*) ---- */
static A2Obj *a2_vec_new(VM2 *vm, A2Class *et) { A2Obj *a = a2_new(vm, a2_vector_class_for(vm, et)); a->kind = OK_VECTOR; a->vec_type = et; return a; }
static void add_iface(VM2 *vm, A2Class *c, const char *qn) { A2Class *ic = a2_class_find_s(vm, qn); if (!ic) return; c->ifaces = (A2Class**)realloc(c->ifaces, (size_t)(c->nifaces + 1) * sizeof(A2Class*)); c->ifaces[c->nifaces++] = ic; }
static int is_cls(VM2 *vm, V2 v, const char *qn) { A2Class *c = a2_class_find_s(vm, qn); return c && v.t == T_OBJ && v.u.o->cls && a2_is_subclass(v.u.o->cls, c); }
static V2 call_m(VM2 *vm, V2 obj, const char *name, int argc, V2 *argv) { V2 f = a2_get_s(vm, obj, name); if (vm->throwing || f.t != T_OBJ) return v2_undef(); return a2_call(vm, f, obj, argc, argv); }
static V2 gs(VM2 *vm, V2 o, const char *k) { return a2_get_s(vm, o, k); }
static void apply_fill(VM2 *vm, V2 g, V2 f, int line) {
    V2 a[8];
    if (is_cls(vm, f, "flash.display::GraphicsSolidFill")) {
        if (line) return;   /* solid line colour is set by the stroke itself */
        a[0] = gs(vm, f, "color"); a[1] = gs(vm, f, "alpha"); call_m(vm, g, "beginFill", 2, a);
    } else if (is_cls(vm, f, "flash.display::GraphicsGradientFill")) {
        a[0] = gs(vm, f, "type"); a[1] = gs(vm, f, "colors"); a[2] = gs(vm, f, "alphas"); a[3] = gs(vm, f, "ratios"); a[4] = gs(vm, f, "matrix"); a[5] = gs(vm, f, "spreadMethod"); a[6] = gs(vm, f, "interpolationMethod"); a[7] = gs(vm, f, "focalPointRatio");
        call_m(vm, g, line ? "lineGradientStyle" : "beginGradientFill", 8, a);
    } else if (is_cls(vm, f, "flash.display::GraphicsBitmapFill")) {
        a[0] = gs(vm, f, "bitmapData"); a[1] = gs(vm, f, "matrix"); a[2] = gs(vm, f, "repeat"); a[3] = gs(vm, f, "smooth");
        call_m(vm, g, line ? "lineBitmapStyle" : "beginBitmapFill", 4, a);
    }
}
/* Graphics/drawPathObject: the library's private step between drawGraphicsData and drawPath
   (it shows in stack traces) */
static V2 g_drawPathObject(VM2 *vm, V2 self, int argc, V2 *argv) { V2 it = ARG(0), a[3]; a[0] = gs(vm, it, "commands"); a[1] = gs(vm, it, "data"); a[2] = gs(vm, it, "winding"); return call_m(vm, self, "drawPath", 3, a); }
static V2 g_drawGraphicsData(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 list = ARG(0); int i;
    if (list.t == T_NULL || list.t == T_UNDEF) { a2_throw_error(vm, "TypeError", "Parameter graphicsData must be non-null.", 2007); return v2_undef(); }
    if (list.t != T_OBJ) return v2_undef();
    for (i = 0; i < list.u.o->alen && !vm->throwing; i++) {
        V2 it = list.u.o->arr[i], a[8];
        if (it.t != T_OBJ) continue;
        if (is_cls(vm, it, "flash.display::GraphicsPath")) { static A2Method dpo; dpo.native = g_drawPathObject; dpo.dbg_name = a2_str(vm, "flash.display::Graphics/drawPathObject"); a2_call_method(vm, &dpo, NULL, self, 1, &it); }
        else if (is_cls(vm, it, "flash.display::GraphicsTrianglePath")) { a[0] = gs(vm, it, "vertices"); a[1] = gs(vm, it, "indices"); a[2] = gs(vm, it, "uvtData"); a[3] = gs(vm, it, "culling"); call_m(vm, self, "drawTriangles", 4, a); }
        else if (is_cls(vm, it, "flash.display::GraphicsEndFill")) call_m(vm, self, "endFill", 0, NULL);
        else if (is_cls(vm, it, "flash.display::GraphicsStroke")) {
            V2 fill = gs(vm, it, "fill"); double th = v2_tonum(vm, gs(vm, it, "thickness"));
            if (th != th) { call_m(vm, self, "lineStyle", 0, NULL); continue; }
            a[0] = v2_num(th); a[1] = v2_uint(0); a[2] = v2_num(1.0);
            if (is_cls(vm, fill, "flash.display::GraphicsSolidFill")) { a[1] = gs(vm, fill, "color"); a[2] = gs(vm, fill, "alpha"); }
            a[3] = gs(vm, it, "pixelHinting"); a[4] = gs(vm, it, "scaleMode"); a[5] = gs(vm, it, "caps"); a[6] = gs(vm, it, "joints"); a[7] = gs(vm, it, "miterLimit");
            call_m(vm, self, "lineStyle", 8, a);
            if (fill.t == T_OBJ) apply_fill(vm, self, fill, 1);
        }
        else apply_fill(vm, self, it, 0);
    }
    return v2_undef();
}
static void set_fields(VM2 *vm, V2 self, int argc, V2 *argv, const char **names, const V2 *defs) {
    int i; for (i = 0; names[i]; i++) a2_set_s(vm, self, names[i], i < argc && argv[i].t != T_HOLE ? argv[i] : defs[i]);
}
static V2 GSolidFill_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { const char *n[] = { "color", "alpha", NULL }; V2 d[2]; d[0] = v2_uint(0); d[1] = v2_num(1); set_fields(vm, self, argc, argv, n, d); a2_set_s(vm, self, "color", v2_uint(v2_touint(vm, a2_get_s(vm, self, "color")))); a2_set_s(vm, self, "alpha", v2_num(v2_tonum(vm, a2_get_s(vm, self, "alpha")))); return v2_undef(); }
static V2 GGradFill_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { const char *n[] = { "type", "colors", "alphas", "ratios", "matrix", "spreadMethod", "interpolationMethod", "focalPointRatio", NULL }; V2 d[8]; d[0] = v2_cstr(vm, "linear"); d[1] = d[2] = d[3] = d[4] = v2_null(); d[5] = v2_cstr(vm, "pad"); d[6] = v2_cstr(vm, "rgb"); d[7] = v2_num(0); set_fields(vm, self, argc, argv, n, d); return v2_undef(); }
static V2 GBitmapFill_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { const char *n[] = { "bitmapData", "matrix", "repeat", "smooth", NULL }; V2 d[4]; d[0] = d[1] = v2_null(); d[2] = v2_bool(1); d[3] = v2_bool(0); set_fields(vm, self, argc, argv, n, d); return v2_undef(); }
static V2 GStroke_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { const char *n[] = { "thickness", "pixelHinting", "scaleMode", "caps", "joints", "miterLimit", "fill", NULL }; V2 d[7]; d[0] = v2_num(0.0 / 0.0); d[1] = v2_bool(0); d[2] = v2_cstr(vm, "normal"); d[3] = v2_cstr(vm, "none"); d[4] = v2_cstr(vm, "round"); d[5] = v2_num(3); d[6] = v2_null(); set_fields(vm, self, argc, argv, n, d); return v2_undef(); }
static V2 GTriPath_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    const char *n[] = { "vertices", "indices", "uvtData", "culling", NULL }; V2 d[4];
    if (argc > 3 && !(argv[3].t == T_STR && (!strcmp(argv[3].u.s->s, "none") || !strcmp(argv[3].u.s->s, "positive") || !strcmp(argv[3].u.s->s, "negative")))) { a2_throw_id(vm, "ArgumentError", 2008, "culling", NULL); return v2_undef(); }
    d[0] = v2_obj(a2_vec_new(vm, vm->c_number)); d[1] = d[2] = v2_null(); d[3] = v2_cstr(vm, "none"); set_fields(vm, self, argc, argv, n, d); return v2_undef();
}
static V2 GPath_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    const char *n[] = { "commands", "data", "winding", NULL }; V2 d[3];
    d[0] = v2_null(); d[1] = v2_null(); d[2] = v2_cstr(vm, "evenOdd");
    if (argc > 2) { A2Str *w = argv[2].t == T_STR ? argv[2].u.s : NULL; if (!w || (strcmp(w->s, "evenOdd") && strcmp(w->s, "nonZero"))) { a2_throw_error(vm, "ArgumentError", "Parameter winding must be one of the accepted values.", 2008); return v2_undef(); } }
    set_fields(vm, self, argc, argv, n, d);
    return v2_undef();
}
static void gp_push(VM2 *vm, V2 self, int cmd, int argc, V2 *argv, int n) {
    V2 c = a2_get_s(vm, self, "commands"), d = a2_get_s(vm, self, "data"); int i;
    if (c.t == T_NULL) { c = v2_obj(a2_vec_new(vm, vm->c_int)); a2_set_s(vm, self, "commands", c); }
    if (d.t == T_NULL) { d = v2_obj(a2_vec_new(vm, vm->c_number)); a2_set_s(vm, self, "data", d); }
    if (c.t != T_OBJ || d.t != T_OBJ) return;
    a2_arr_push(vm, c.u.o, v2_int(cmd));
    for (i = 0; i < n; i++) a2_arr_push(vm, d.u.o, v2_num(i < argc ? v2_tonum(vm, argv[i]) : 0));
}
static V2 gp_moveTo(VM2 *vm, V2 self, int argc, V2 *argv) { gp_push(vm, self, 1, argc, argv, 2); return v2_undef(); }
static V2 gp_lineTo(VM2 *vm, V2 self, int argc, V2 *argv) { gp_push(vm, self, 2, argc, argv, 2); return v2_undef(); }
static V2 gp_curveTo(VM2 *vm, V2 self, int argc, V2 *argv) { gp_push(vm, self, 3, argc, argv, 4); return v2_undef(); }
static V2 gp_cubicCurveTo(VM2 *vm, V2 self, int argc, V2 *argv) { gp_push(vm, self, 6, argc, argv, 6); return v2_undef(); }
static V2 gp_wideMoveTo(VM2 *vm, V2 self, int argc, V2 *argv) { V2 a[4]; a[0] = a[1] = v2_num(0); a[2] = ARG(0); a[3] = ARG(1); gp_push(vm, self, 4, 4, a, 4); return v2_undef(); }
static V2 gp_wideLineTo(VM2 *vm, V2 self, int argc, V2 *argv) { V2 a[4]; a[0] = a[1] = v2_num(0); a[2] = ARG(0); a[3] = ARG(1); gp_push(vm, self, 5, 4, a, 4); return v2_undef(); }

static V2 g_copyFrom(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Disp *st; ShapeDef *d = gfx_shape(vm, self, &st); A2Obj *src = gfx_owner(vm, ARG(0)); ShapeDef *sd; int i;
    if (!d || !src) return v2_undef();
    g_clear(vm, self, 0, NULL); d = gfx_shape(vm, self, &st);
    sd = a2_disp(src)->drawing; if (!sd) return v2_undef();
    for (i = 0; i < sd->nfills; i++) add_fill(d, &sd->fills[i]);
    for (i = 0; i < sd->nlines; i++) add_line(d, &sd->lines[i]);
    for (i = 0; i < sd->nedges; i++) add_edge(d, &sd->edges[i]);
    {   /* Ruffle Drawing::copy: the pen, fill and line in use come along */
        A2Disp *ss = a2_disp(src);
        st->draw_fill = ss->draw_fill; st->draw_line = ss->draw_line; st->draw_x = ss->draw_x; st->draw_y = ss->draw_y;
        st->start_x = ss->start_x; st->start_y = ss->start_y; st->draw_open = ss->draw_open;
    }
    return v2_undef();
}
/* Twips::from_pixels: truncated, NaN is 0 */
static double tri_tw(VM2 *vm, V2 x) { double v = v2_tonum(vm, x); return v != v ? 0 : to_twips(v); }
/* one triangle of drawTriangles (Ruffle draw_triangles_internal): culled by its orientation in twips */
static void tri_draw(VM2 *vm, V2 self, A2Obj *v, int i0, int i1, int i2, int cull) {
    V2 a[2]; int k, ix[3];
    ix[0] = i0; ix[1] = i1; ix[2] = i2;
    if (cull) {
        double t[6]; double o;
        for (k = 0; k < 3; k++) { t[k * 2] = tri_tw(vm, v->arr[ix[k]]); t[k * 2 + 1] = tri_tw(vm, v->arr[ix[k] + 1]); }
        o = (t[2] - t[0]) * (t[5] - t[1]) - (t[3] - t[1]) * (t[4] - t[0]);
        if (cull > 0 ? o >= 0 : o <= 0) return;
    }
    for (k = 0; k < 4; k++) {
        a[0] = v->arr[ix[k % 3]]; a[1] = v->arr[ix[k % 3] + 1];
        if (k == 0) g_moveTo(vm, self, 2, a); else g_lineTo(vm, self, 2, a);
    }
}
static V2 g_drawTriangles(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 verts = ARG(0), idx = ARG(1), cull = ARG(3); A2Obj *v; int i, cm = 0;
    if (verts.t != T_OBJ) return v2_undef();
    v = verts.u.o;
    /* coordinate pairs (whole triangles without indices), index triples, a known culling */
    if ((v->alen & 1) || (idx.t != T_OBJ && v->alen % 6) || (idx.t == T_OBJ && idx.u.o->alen % 3)
        || (argc > 3 && !(cull.t == T_STR && (!strcmp(cull.u.s->s, "none") || !strcmp(cull.u.s->s, "positive") || !strcmp(cull.u.s->s, "negative"))))) {
        a2_throw_id(vm, "ArgumentError", 2004, NULL, NULL); return v2_undef();
    }
    if (argc > 3 && cull.t == T_STR) cm = !strcmp(cull.u.s->s, "positive") ? 1 : !strcmp(cull.u.s->s, "negative") ? -1 : 0;
    if (idx.t == T_OBJ) {
        /* Flash stops at the first triangle with an out-of-range index */
        A2Obj *ix = idx.u.o; unsigned nv = (unsigned)(v->alen / 2);
        for (i = 0; i + 2 < ix->alen; i += 3) {
            unsigned a0 = (unsigned)v2_toint(vm, ix->arr[i]), a1 = (unsigned)v2_toint(vm, ix->arr[i + 1]), a2 = (unsigned)v2_toint(vm, ix->arr[i + 2]);
            if (a0 >= nv || a1 >= nv || a2 >= nv) break;
            tri_draw(vm, self, v, (int)a0 * 2, (int)a1 * 2, (int)a2 * 2, cm);
        }
    }
    else for (i = 0; i + 5 < v->alen; i += 6) tri_draw(vm, self, v, i, i + 2, i + 4, cm);
    return v2_undef();
}
static V2 g_noop(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_undef(); }
static V2 Graphics_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = SELFO; UNUSED; if (o) o->kind = OK_GRAPHICS; return v2_undef(); }

/* ---------------------------------------------------------------- */
/* Stage                                                            */

static V2 st_stageWidth(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_int((vm->player->movie->stage.xmax - vm->player->movie->stage.xmin) / 20); }
static V2 st_stageHeight(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_int((vm->player->movie->stage.ymax - vm->player->movie->stage.ymin) / 20); }
static V2 st_frameRate_get(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_num(vm->player->movie->framerate); }
static V2 st_frameRate_set(VM2 *vm, V2 self, int argc, V2 *argv) { double r = NUM(0); UNUSED; if (r != r) { vm->player->movie->framerate = r; return v2_undef(); } if (r < 0.01) r = 0.01; if (r > 1000) r = 1000; vm->player->movie->framerate = r; vm->frame_ms = 1000.0 / r; return v2_undef(); }
static V2 st_str_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; (void)self; return v2_cstr(vm, "showAll"); }
static V2 st_prop_get(VM2 *vm, V2 self, int argc, V2 *argv, const char *key, V2 def) { A2Prop *p = SELFO ? a2_dyn_find(SELFO, a2_str(vm, key)) : NULL; (void)argc; (void)argv; return p ? p->v : def; }
static V2 st_scaleMode_get(VM2 *vm, V2 self, int argc, V2 *argv) { return st_prop_get(vm, self, argc, argv, "__scaleMode", v2_cstr(vm, "showAll")); }
static V2 st_scaleMode_set(VM2 *vm, V2 self, int argc, V2 *argv) { if (SELFO) a2_dyn_set(vm, SELFO, a2_str(vm, "__scaleMode"), v2_str(STR(0))); return v2_undef(); }
static V2 st_align_get(VM2 *vm, V2 self, int argc, V2 *argv) { return st_prop_get(vm, self, argc, argv, "__align", v2_str(vm->s_empty)); }
static V2 st_align_set(VM2 *vm, V2 self, int argc, V2 *argv) { if (SELFO) a2_dyn_set(vm, SELFO, a2_str(vm, "__align"), v2_str(STR(0))); return v2_undef(); }
/* stage quality: the requested name and the anti-aliasing factor the player renders with */
int a2_quality_aa = 4;
static V2 st_quality_get(VM2 *vm, V2 self, int argc, V2 *argv) {
    extern int g_stage_quality; static const char *nm[8] = { "LOW", "MEDIUM", "HIGH", "BEST", "8X8", "8X8LINEAR", "16X16", "16X16LINEAR" };
    UNUSED; return v2_cstr(vm, nm[g_stage_quality >= 0 && g_stage_quality < 8 ? g_stage_quality : 2]);
}
static V2 st_quality_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    extern int g_stage_quality; A2Str *q = STR(0); char u[16]; size_t i; (void)self;
    static const struct { const char *n; int aa, rq; } tab[] = { { "LOW", 1, 0 }, { "MEDIUM", 2, 1 }, { "HIGH", 4, 2 }, { "BEST", 4, 3 }, { "8X8", 8, 4 }, { "8X8LINEAR", 8, 5 }, { "16X16", 16, 6 }, { "16X16LINEAR", 16, 7 }, { NULL, 0, 0 } };
    if (q->len >= sizeof(u)) return v2_undef();
    for (i = 0; i <= q->len; i++) u[i] = (char)(q->s[i] >= 'a' && q->s[i] <= 'z' ? q->s[i] - 32 : q->s[i]);
    for (i = 0; tab[i].n; i++) if (!strcmp(u, tab[i].n)) { a2_quality_aa = tab[i].aa; g_stage_quality = tab[i].rq; break; }
    return v2_undef();
}
static V2 st_displayState_get(VM2 *vm, V2 self, int argc, V2 *argv) { return st_prop_get(vm, self, argc, argv, "__displayState", v2_cstr(vm, "normal")); }
static int ds_icmp(const char *a, const char *b) { for (; *a && *b; a++, b++) { int x = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a, y = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b; if (x != y) return x - y; } return *a - *b; }
/* the player goes full screen at once (desktop player: fullScreen becomes fullScreenInteractive)
   and the stage gets a FullScreenEvent */
static V2 st_displayState_set(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Str *v = STR(0); const char *ns; V2 cur; int full, was;
    if (!SELFO) return v2_undef();
    if (!v || (ds_icmp(v->s, "fullScreen") && ds_icmp(v->s, "fullScreenInteractive") && ds_icmp(v->s, "normal"))) { a2_throw_id(vm, "ArgumentError", 2008, "displayState", NULL); return v2_undef(); }
    full = ds_icmp(v->s, "normal") != 0; ns = full ? "fullScreenInteractive" : "normal";
    cur = st_displayState_get(vm, self, 0, NULL); was = cur.t == T_STR && strcmp(cur.u.s->s, "normal");
    if (full == was) return v2_undef();
    a2_dyn_set(vm, SELFO, a2_str(vm, "__displayState"), v2_cstr(vm, ns));
    {   A2Class *fc = a2_class_find_s(vm, "flash.events::FullScreenEvent"); V2 a[5];
        a[0] = v2_cstr(vm, "fullScreen"); a[1] = v2_bool(0); a[2] = v2_bool(0); a[3] = v2_bool(full); a[4] = v2_bool(1);
        if (fc) { V2 e = a2_construct_class(vm, fc, 5, a); if (e.t == T_OBJ && !vm->throwing) a2_dispatch_event_obj(vm, SELFO, e.u.o); } }
    return v2_undef();
}
static V2 r_false_st(VM2 *vm, V2 self, int argc, V2 *argv) { (void)vm; (void)self; (void)argc; (void)argv; return v2_bool(0); }
static V2 st_color_get(VM2 *vm, V2 self, int argc, V2 *argv) { SWFColor c = vm->player->bg; UNUSED; return v2_uint(0xFF000000u | ((u32)c.r << 16) | ((u32)c.g << 8) | c.b); }   /* opaque ARGB */
static V2 st_color_set(VM2 *vm, V2 self, int argc, V2 *argv) { u32 rgb = v2_touint(vm, ARG(0)); vm->player->bg.r = (u8)(rgb >> 16); vm->player->bg.g = (u8)(rgb >> 8); vm->player->bg.b = (u8)rgb; vm->player->bg.a = 255; vm->player->bg_set = 1; return v2_undef(); }
static V2 st_focus_get(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return vm->focus ? v2_obj(vm->focus) : v2_null(); }
void a2_focus_set(VM2 *vm, A2Obj *o);
static V2 st_focus_set(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; a2_focus_set(vm, dself(ARG(0))); return v2_undef(); }
static V2 st_invalidate(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; a2_dyn_set(vm, vm->global, a2_str(vm, "__renderPending"), v2_bool(1)); return v2_undef(); }
static V2 st_stageFocusRect_get(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_bool(!vm->stage_focus_rect_off); }
static V2 st_stageFocusRect_set(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; vm->stage_focus_rect_off = !BOOL(0); return v2_undef(); }
static V2 st_showDefaultContextMenu_get(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_bool(1); }
static V2 st_fullScreenWidth(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_uint(1024); }
static V2 st_fullScreenHeight(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_uint(768); }
static V2 st_allowsFullScreen(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_bool(1); }
static V2 st_stageVideos(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *a = a2_new(vm, vm->c_vector); UNUSED; a->kind = OK_VECTOR; return v2_obj(a); }
/* Stage3D instances: avm2_stage3d.c */
static V2 st_stage3Ds(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Str *k = a2_str(vm, "__stage3Ds"); V2 v; A2Obj *a; A2Class *c3 = a2_class_find_s(vm, "flash.display::Stage3D"); int i;
    UNUSED;
    if (self.t == T_OBJ && a2_dyn_find(self.u.o, k)) { v = a2_get_s(vm, self, "__stage3Ds"); if (v.t == T_OBJ) return v; }
    a = a2_new(vm, a2_class_find_s(vm, "Vector.<flash.display::Stage3D>") ? a2_class_find_s(vm, "Vector.<flash.display::Stage3D>") : vm->c_vector); a->kind = OK_VECTOR;
    for (i = 0; i < 4 && c3; i++) { extern A2Obj *a2_stage3d_create(VM2 *vm, int index); A2Obj *o = a2_stage3d_create(vm, i); if (o) a2_arr_push(vm, a, v2_obj(o)); }
    if (self.t == T_OBJ) a2_dyn_set_hidden(vm, self.u.o, k, v2_obj(a));
    return v2_obj(a);
}
static V2 st_nativeWindow(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_null(); }
static V2 st_contentsScaleFactor(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_num(vm->player->has_view && vm->player->view_sf > 0 ? vm->player->view_sf : 1); }
static V2 st_softKeyboardRect(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_obj(a2_rect(vm, 0, 0, 0, 0)); }
static V2 Stage_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; a2_throw_error(vm, "ArgumentError", "Stage class cannot be instantiated.", 2012); return v2_undef(); }
static V2 st_size_get(VM2 *vm, V2 self, int which) {
    double w, h; (void)self;
    stage_content_size(vm, &w, &h);
    return v2_num(which ? h : w);
}
static V2 st_width_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return st_size_get(vm, self, 0); }
static V2 st_height_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return st_size_get(vm, self, 1); }
static V2 st_colorCorrection_set(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; if (ARG(0).t == T_NULL || ARG(0).t == T_UNDEF) a2_throw_id(vm, "TypeError", 2007, "colorCorrection", NULL); return v2_undef(); }
static V2 st_bad_set(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; a2_throw_error(vm, "Error", "The Stage class does not implement this property or method.", 2071); return v2_undef(); }

/* ---------------------------------------------------------------- */
/* Shape / Bitmap / SimpleButton / misc display                     */

static V2 Shape_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { return DisplayObject_ctor(vm, self, argc, argv); }
static V2 bmp_bitmapData_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); A2Disp *st; DOBJ(d, o); (void)argc; (void)argv; if (!o) return v2_null(); st = a2_disp(o); if (!st->bitmap_data && d && d->ch && d->ch->type == CHAR_BITMAP) { extern A2Obj *a2_bitmapdata_from_char(VM2 *vm, SWFChar *ch); st->bitmap_data = a2_bitmapdata_from_char(vm, d->ch); bmp_cache_size(o); } return st->bitmap_data ? v2_obj(st->bitmap_data) : v2_null(); }
static void bmp_cache_size(A2Obj *o) { extern int a2_bitmapdata_size(A2Obj *bd, int *w, int *h); A2Disp *st = a2_disp(o); int w = 0, h = 0; if (!st->bitmap_data || !a2_bitmapdata_size(st->bitmap_data, &w, &h)) w = h = 0; st->bmp_w = w; st->bmp_h = h; }
static V2 bmp_bitmapData_set(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); if (o) { a2_disp(o)->bitmap_data = ARG(0).t == T_OBJ ? ARG(0).u.o : NULL; bmp_cache_size(o); } return v2_undef(); }
static V2 bmp_smoothing_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); UNUSED; return v2_bool(o ? a2_disp(o)->smoothing : 0); }
static V2 bmp_smoothing_set(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = dself(self); if (o) a2_disp(o)->smoothing = BOOL(0); return v2_undef(); }
static V2 bmp_pixelSnapping_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Prop *p; UNUSED; p = SELFO ? a2_dyn_find(SELFO, a2_str(vm, "__pixelSnapping")) : NULL; return p ? p->v : v2_cstr(vm, "auto"); }
int a2_check_pixel_snapping(VM2 *vm, V2 v) {
    const char *s = v.t == T_STR ? v.u.s->s : NULL;
    if (!s || (strcmp(s, "never") && strcmp(s, "always") && strcmp(s, "auto"))) { a2_throw_error(vm, "ArgumentError", "Parameter pixelSnapping must be one of the accepted values.", 2008); return 0; }
    return 1;
}
static int snap_mode(V2 v) { const char *s = v.t == T_STR ? v.u.s->s : ""; return !strcmp(s, "never") ? 1 : !strcmp(s, "always") ? 2 : 0; }
static V2 bmp_pixelSnapping_set(VM2 *vm, V2 self, int argc, V2 *argv) { V2 v = ARG(0); UNUSED; if (!a2_check_pixel_snapping(vm, v)) return v2_undef(); if (SELFO) { a2_dyn_set_hidden(vm, SELFO, a2_str(vm, "__pixelSnapping"), v); if (SELFO->kind == OK_DISPLAY) a2_disp(SELFO)->snapping = snap_mode(v); } return v2_undef(); }
int a2_check_pixel_snapping(VM2 *vm, V2 v);
static V2 Bitmap_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { DisplayObject_ctor(vm, self, 0, NULL); if (argc > 1 && argv[1].t != T_HOLE && !a2_check_pixel_snapping(vm, argv[1])) return v2_undef(); if (SELFO) { if (argc > 1 && argv[1].t == T_STR) { a2_dyn_set_hidden(vm, SELFO, a2_str(vm, "__pixelSnapping"), argv[1]); a2_disp(SELFO)->snapping = snap_mode(argv[1]); } if (ARG(0).t == T_OBJ) a2_disp(SELFO)->bitmap_data = ARG(0).u.o;   /* null keeps the data it has (a symbol's, Ruffle Bitmap.init) */
    if (!a2_disp(SELFO)->bitmap_data) { DisplayObject *dd = a2_dobj_of(SELFO); if (dd && dd->ch && dd->ch->type == CHAR_BITMAP) { extern A2Obj *a2_bitmapdata_from_char(VM2 *vm, SWFChar *ch); a2_disp(SELFO)->bitmap_data = a2_bitmapdata_from_char(vm, dd->ch); } }   /* a bitmap symbol's class: its pixels (Ruffle bitmap_allocator) */
    bmp_cache_size(SELFO); if (argc > 2) a2_disp(SELFO)->smoothing = BOOL(2); } return v2_undef(); }
#include "netstream.h"
static V2 r_noop2(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_undef(); }
/* flash.media.Video: the size it is made with (320x240 when either is 0) */
static V2 Video_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    extern SWFChar *video_new_char(SWFMovie *m, int w, int h); DisplayObject *d;
    s32 w = argc > 0 ? v2_toint(vm, argv[0]) : 320, h = argc > 1 ? v2_toint(vm, argv[1]) : 240;
    if (vm->throwing) return v2_undef();
    if (w < 0 || h < 0 || (double)w * h >= 536870912.0 || (double)w * 20 > 2147483647.0 || (double)h * 20 > 2147483647.0) { a2_throw_id(vm, "RangeError", 2006, NULL, NULL); return v2_undef(); }
    if (w == 0 || h == 0) { w = 320; h = 240; }
    DisplayObject_ctor(vm, self, 0, NULL);
    d = SELFO ? a2_dobj_of(SELFO) : NULL;
    if (d && (!d->ch || d->ch->type != CHAR_VIDEO)) { d->ch = video_new_char(PMOV(vm->player), w, h); d->char_id = -1; }
    return v2_undef();
}
static V2 vid_attachNetStream(VM2 *vm, V2 self, int argc, V2 *argv) {
    extern NetStream *a2_netstream_get(VM2 *vm, A2Obj *o); DisplayObject *d = SELFO ? a2_dobj_of(SELFO) : NULL; UNUSED;
    if (!d) return v2_undef();
    d->vstream = ARG(0).t == T_OBJ ? (void*)a2_netstream_get(vm, ARG(0).u.o) : NULL; d->vid_cleared = 0;
    if (ARG(0).t == T_OBJ) a2_dyn_set_hidden(vm, SELFO, a2_str(vm, "__ns"), ARG(0)); else a2_dyn_set_hidden(vm, SELFO, a2_str(vm, "__ns"), v2_null());
    return v2_undef();
}
static V2 vid_clear(VM2 *vm, V2 self, int argc, V2 *argv) { DisplayObject *d = SELFO ? a2_dobj_of(SELFO) : NULL; UNUSED; if (d) d->vid_cleared = 1; return v2_undef(); }
static V2 vid_dim(VM2 *vm, V2 self, int which) { extern void video_dims(Player *p, DisplayObject *d, int *vw, int *vh); DisplayObject *d = SELFO ? a2_dobj_of(SELFO) : NULL; int w = 0, h = 0; if (d) video_dims(vm->player, d, &w, &h); return v2_int(which ? h : w); }
static V2 vid_videoWidth(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return vid_dim(vm, self, 0); }
static V2 vid_videoHeight(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return vid_dim(vm, self, 1); }
static V2 vid_smoothing_get(VM2 *vm, V2 self, int argc, V2 *argv) { DisplayObject *d = SELFO ? a2_dobj_of(SELFO) : NULL; UNUSED; return v2_bool(d && d->vid_smooth); }
static V2 vid_smoothing_set(VM2 *vm, V2 self, int argc, V2 *argv) { DisplayObject *d = SELFO ? a2_dobj_of(SELFO) : NULL; if (d) d->vid_smooth = v2_tobool(vm, ARG(0)); return v2_undef(); }
static V2 vid_deblocking_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Prop *p = SELFO ? a2_dyn_find(SELFO, a2_str(vm, "__deblocking")) : NULL; UNUSED; return p ? p->v : v2_int(0); }
static V2 vid_deblocking_set(VM2 *vm, V2 self, int argc, V2 *argv) { s32 v = v2_toint(vm, ARG(0)); if (SELFO && !vm->throwing) a2_dyn_set_hidden(vm, SELFO, a2_str(vm, "__deblocking"), v2_int(v)); return v2_undef(); }
void a2_video_class(VM2 *vm, A2Class *dobj) {
    A2Class *c = a2_def_class(vm, "flash.media", "Video", dobj, OK_DISPLAY, Video_ctor); vm->c_video = c;
    a2_add_method(vm, c, "attachNetStream", vid_attachNetStream, 1); a2_add_method(vm, c, "attachCamera", r_noop2, 1); a2_add_method(vm, c, "clear", vid_clear, 0);
    a2_add_getter(vm, c, "videoWidth", vid_videoWidth, NULL); a2_add_getter(vm, c, "videoHeight", vid_videoHeight, NULL);
    a2_add_getter(vm, c, "smoothing", vid_smoothing_get, vid_smoothing_set); a2_add_getter(vm, c, "deblocking", vid_deblocking_get, vid_deblocking_set);
}
A2Obj *a2_clip_bitmap_data(MovieClip *c) { A2Obj *o = (A2Obj*)c->obj2; A2Disp *st = o ? (A2Disp*)o->payload : NULL; return st ? st->bitmap_data : NULL; }

static V2 sb_state_get(VM2 *vm, V2 self, int argc, V2 *argv, int which) { A2Obj *o = dself(self); A2Disp *st; (void)argc; (void)argv; (void)vm; if (!o) return v2_null(); st = a2_disp(o); { A2Obj *r = which == 0 ? st->up : which == 1 ? st->over : which == 2 ? st->down : st->hit; return r ? v2_obj(r) : v2_null(); } }
static V2 sb_upState_get(VM2 *vm, V2 self, int argc, V2 *argv) { return sb_state_get(vm, self, argc, argv, 0); }
static V2 sb_overState_get(VM2 *vm, V2 self, int argc, V2 *argv) { return sb_state_get(vm, self, argc, argv, 1); }
static V2 sb_downState_get(VM2 *vm, V2 self, int argc, V2 *argv) { return sb_state_get(vm, self, argc, argv, 2); }
static V2 sb_hitTestState_get(VM2 *vm, V2 self, int argc, V2 *argv) { return sb_state_get(vm, self, argc, argv, 3); }
/* Timeline SimpleButton: build real display objects for the up/over/down/hit states
 * (a Sprite container when a state has several records), as Flash Player does. */
static void build_button_states(VM2 *vm, A2Obj *btn, DisplayObject *bd) {
    extern int button_record_count(SWFChar *ch); extern int button_record_get(SWFChar *ch, int i, int *flags, int *char_id, int *depth, SWFMatrix *m, SWFCxform *cx);
    Player *p = vm->player; MovieClip *bc = bd->clip; int nrec = button_record_count(bd->ch), si; A2Disp *bst = a2_disp(btn);
    A2Obj *states[4]; A2Obj *conts[4]; int ncont = 0;
    A2Obj *objs[4][64]; int nobjs[4];
    /* 1. construct every record of every state (not yet inserted anywhere; while constructing,
     *    orphans resolve to the button so stage/root are visible to constructors) */
    vm->building_button = btn;
    for (si = 0; si < 4; si++) {
        int bit = 1 << si, i, j, count = 0, idx[256];
        states[si] = NULL; nobjs[si] = 0; conts[si] = NULL;
        for (i = 0; i < nrec && count < 256; i++) { int fl, cid, dep; SWFMatrix m; SWFCxform cx; if (button_record_get(bd->ch, i, &fl, &cid, &dep, &m, &cx) && (fl & bit)) idx[count++] = i; }
        for (i = 1; i < count; i++) for (j = i; j > 0; j--) { int fa, ca, da, fb, cb, db; SWFMatrix m; SWFCxform cx; button_record_get(bd->ch, idx[j - 1], &fa, &ca, &da, &m, &cx); button_record_get(bd->ch, idx[j], &fb, &cb, &db, &m, &cx); if (db < da) { int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t; } else break; }
        if (count != 1) {   /* several records, or none: an (empty) Sprite holds the state */
            A2Obj *cont = a2_new_display(vm, vm->c_sprite); vm->instance_counter--;   /* containers are named after their children */
            a2_disp(cont)->name = vm->s_empty; conts[si] = cont; ncont++;
            a2_dobj_of(cont)->state_mask = bit; a2_disp(cont)->button_state = 1;
        }
        for (i = 0; i < count && nobjs[si] < 64; i++) {
            int fl, cid, dep; SWFMatrix m; SWFCxform cx; SWFChar *ch; DisplayObject *nd; A2Obj *co;
            button_record_get(bd->ch, idx[i], &fl, &cid, &dep, &m, &cx);
            ch = swf_get_char(bd->ch->movie ? bd->ch->movie : p->movie, cid); if (!ch) continue;
            nd = (DisplayObject*)calloc(1, sizeof(DisplayObject));
            nd->depth = dep; nd->char_id = cid; nd->ch = ch; nd->matrix = m; nd->cxform = cx; nd->visible = 1; nd->ratio = 0;
            if (ch->type == CHAR_SPRITE || ch->type == CHAR_EDITTEXT) nd->clip = player_make_clip(p, NULL, ch, nd);
            avm2_object_created(p, nd);
            if (!conts[si]) nd->state_mask = bit;
            co = a2_display_obj(vm, nd->clip, nd);
            if (!conts[si]) { a2_disp(co)->button_state = 1; states[si] = co; }
            if (ch->type == CHAR_EDITTEXT) a2_text_sync_from_char(vm, co);
            if (ch->type == CHAR_SPRITE && nd->clip) { player_run_first_frame(nd->clip); nd->clip->frame_changed = 1; }
            a2_construct_display(vm, nd->clip, nd);      /* children are constructed inside super() */
            objs[si][nobjs[si]++] = co;
        }
    }
    vm->building_button = NULL;
    for (si = 0; si < 4; si++) if (conts[si]) { char b[32]; sprintf(b, "instance%d", ++vm->instance_counter); a2_disp(conts[si])->name = a2_str(vm, b); }
    /* 2. insert the records; children of a state container get 'added' (container not yet attached) */
    for (si = 0; si < 4; si++) {
        A2Obj *cont = conts[si]; MovieClip *target = cont ? cont->clip : bc; int i;
        for (i = 0; i < nobjs[si]; i++) {
            A2Obj *co = objs[si][i]; DisplayObject *nd = a2_dobj_of(co);
            player_insert_dobj(target, nd); nd->owner = target; if (nd->clip) nd->clip->parent = target;
            if (cont) a2_dispatch_simple(vm, co, vm->s_added, 1);   /* a state container's children get 'added' */
        }
        if (cont) {
            DisplayObject *cd = a2_dobj_of(cont);
            player_insert_dobj(bc, cd); cd->owner = bc; if (cd->clip) cd->clip->parent = bc; a2_disp(cont)->owns = 0;
            states[si] = cont;
        }
    }
    /* then only the up state itself gets 'added' (Ruffle's Avm2Button::construct_frame) */
    if (states[0]) {
        a2_dispatch_simple(vm, states[0], vm->s_added, 1);
        if (a2_on_stage(vm, states[0])) { clear_stage_notified(vm, states[0]); dispatch_added_tree(vm, states[0]); }   /* children told during construction hear it again */
    }
    bst->up = states[0]; bst->over = states[1]; bst->down = states[2]; bst->hit = states[3];
    (void)ncont;
    /* Flash keeps the state objects in the order up, down, over, hit for frame processing */
    { static const int order[4] = { 0, 2, 1, 3 }; int k; DisplayObject *d;
      for (k = 0; k < 4; k++) { A2Obj *so = states[order[k]]; if (so && (d = a2_dobj_of(so)) != NULL) d->depth = 16384 + k; }
      player_resort(bc); }
    /* the states' first frame scripts run right after construction, before the owner's.
       When a whole frame follows (SWF10+ button with a MovieClip state) and the main timeline
       already has its frame script queued, Flash defers them to that frame's script pass:
       the last-built state's scripts first, then the main timeline, then the others. */
    if (g_btn_frame_follows && bd->ch->movie && bd->ch->movie->version > 9 && vm->root_constructed && button_has_mc_state(vm, btn)
        && p->root && p->root->frame_changed && p->root->obj2 && ((A2Obj*)p->root->obj2)->nframe_scripts) {
        for (si = 3; si >= 0; si--) { A2Obj *so = states[si]; MovieClip *sc; if (so && (sc = clip_of(so)) != NULL && so->cls && a2_is_subclass(so->cls, vm->c_movieclip)) { g_btn_first_scripts = sc; break; } }
    } else
    for (si = 0; si < 4; si++) { A2Obj *so = states[si]; MovieClip *sc; if (!so) continue; sc = clip_of(so); if (sc) run_state_frame_scripts(vm, sc); }
}
/* script buttons (and state reassignment): the button's clip holds every state object,
 * each record tagged with the states it belongs to; the player draws the current state. */
static void sb_refresh(VM2 *vm, A2Obj *o) {
    A2Disp *st = a2_disp(o); MovieClip *c = clip_of(o); DisplayObject *d; A2Obj *objs[4]; int i, k;
    if (!c) return;
    objs[0] = st->up; objs[1] = st->over; objs[2] = st->down; objs[3] = st->hit;
    /* remove state children that are no longer referenced */
    for (d = c->children; d; ) {
        DisplayObject *nx = d->next; A2Obj *co = (A2Obj*)(d->obj2 ? d->obj2 : (d->clip ? d->clip->obj2 : NULL)); int keep = 0;
        if (d->state_mask) { for (i = 0; i < 4; i++) if (objs[i] == co) keep = 1; if (!keep) { if (co) { a2_disp(co)->button_state = 0; detach_child(vm, co); } else { unlink_record(c, d); player_free_dobj(d); } } }
        d = nx;
    }
    for (i = 0; i < 4; i++) {
        A2Obj *so = objs[i]; DisplayObject *sd; int mask = 0;
        if (!so) continue;
        sd = a2_dobj_of(so); if (!sd) continue;
        for (k = 0; k < 4; k++) if (objs[k] == so) mask |= 1 << k;
        if (sd->owner != c) { detach_child(vm, so); insert_child(vm, o, so, cc_count(vm, o)); }
        sd->state_mask = mask; a2_disp(so)->button_state = 1;
    }
}
/* a state object set by script; on the current state it is added with events and a
   frameConstructed / frame scripts / exitFrame pass runs (Ruffle set_state_child) */
static V2 sb_state_set(VM2 *vm, V2 self, V2 v, int which) {
    A2Obj *o = dself(self), *child = dself(v), *old; A2Disp *st; DisplayObject *bd; int cur;
    if (!o) return v2_undef();
    st = a2_disp(o); bd = a2_dobj_of(o);
    old = which == 0 ? st->up : which == 1 ? st->over : which == 2 ? st->down : st->hit;
    cur = which < 3 && bd && (bd->btn_state == 3 ? 2 : bd->btn_state) == which;
    if (child && a2_parent_of(vm, child) && !a2_disp(child)->button_state) { remove_child_events(vm, child); detach_child(vm, child); }
    if (which == 0) st->up = child; else if (which == 1) st->over = child; else if (which == 2) st->down = child; else st->hit = child;
    sb_refresh(vm, o);
    if (cur && vm->root_constructed) {
        if (child && child != old) { a2_dispatch_simple(vm, child, vm->s_added, 1); if (a2_on_stage(vm, o)) dispatch_added_tree(vm, child); }
        if (old && old != child && old != st->up && old != st->over && old != st->down && old != st->hit) a2_dispatch_simple(vm, old, vm->s_removed, 1);
        if (child) {
            a2_broadcast(vm, vm->s_frameConstructed); if (vm->throwing) a2_report_uncaught(vm);
            if (clip_of(child)) run_pending_frame_scripts(vm, clip_of(child));
            a2_broadcast(vm, vm->s_exitFrame); if (vm->throwing) a2_report_uncaught(vm);
        }
    }
    return v2_undef();
}
static V2 sb_upState_set(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; return sb_state_set(vm, self, ARG(0), 0); }
static V2 sb_overState_set(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; return sb_state_set(vm, self, ARG(0), 1); }
static V2 sb_downState_set(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; return sb_state_set(vm, self, ARG(0), 2); }
static V2 sb_hitTestState_set(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; return sb_state_set(vm, self, ARG(0), 3); }
static V2 sb_enabled_get(VM2 *vm, V2 self, int argc, V2 *argv) { return mc_enabled_get(vm, self, argc, argv); }
static V2 sb_enabled_set(VM2 *vm, V2 self, int argc, V2 *argv) { return mc_enabled_set(vm, self, argc, argv); }
static V2 SimpleButton_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { DisplayObject_ctor(vm, self, 0, NULL); if (SELFO) { A2Disp *st = a2_disp(SELFO); DisplayObject *d = a2_dobj_of(SELFO); int timeline = d && d->ch && d->ch->type == CHAR_BUTTON; if (!timeline || argc > 0) { if (argc > 0 || !timeline) { st->up = dself(ARG(0)); st->over = dself(ARG(1)); st->down = dself(ARG(2)); st->hit = dself(ARG(3)); } sb_refresh(vm, SELFO); } } return v2_undef(); }

static V2 FrameLabel_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { if (SELFO) { a2_dyn_set(vm, SELFO, a2_str(vm, "__name"), v2_str(STR(0))); a2_dyn_set(vm, SELFO, a2_str(vm, "__frame"), v2_int(INT(1))); } return v2_undef(); }
static V2 fl_name(VM2 *vm, V2 self, int argc, V2 *argv) { A2Prop *p; UNUSED; p = SELFO ? a2_dyn_find(SELFO, a2_str(vm, "__name")) : NULL; return p ? p->v : v2_null(); }
static V2 fl_frame(VM2 *vm, V2 self, int argc, V2 *argv) { A2Prop *p; UNUSED; p = SELFO ? a2_dyn_find(SELFO, a2_str(vm, "__frame")) : NULL; return p ? p->v : v2_int(0); }
static V2 Scene_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { if (SELFO) { a2_dyn_set(vm, SELFO, a2_str(vm, "__name"), v2_str(STR(0))); a2_dyn_set(vm, SELFO, a2_str(vm, "__labels"), ARG(1)); a2_dyn_set(vm, SELFO, a2_str(vm, "__numFrames"), v2_int(INT(2))); } return v2_undef(); }
static V2 sc_labels(VM2 *vm, V2 self, int argc, V2 *argv) { A2Prop *p; UNUSED; p = SELFO ? a2_dyn_find(SELFO, a2_str(vm, "__labels")) : NULL; return p ? p->v : v2_null(); }
static V2 sc_numFrames(VM2 *vm, V2 self, int argc, V2 *argv) { A2Prop *p; UNUSED; p = SELFO ? a2_dyn_find(SELFO, a2_str(vm, "__numFrames")) : NULL; return p ? p->v : v2_int(0); }

/* ---------------------------------------------------------------- */
/* class setup                                                      */

/* classes the built-in ActionScript library defines (as3/): the C side does not register them */
static int builtin_as3_class(const char *ns, const char *name) {
    extern const char *const g_avm2_builtin_names[]; int i; size_t nl = strlen(ns);
    for (i = 0; g_avm2_builtin_names[i]; i++) { const char *q = g_avm2_builtin_names[i]; if (!strncmp(q, ns, nl) && q[nl] == ':' && q[nl + 1] == ':' && !strcmp(q + nl + 2, name)) return 1; }
    return 0;
}
A2Class *a2_def_class(VM2 *vm, const char *ns, const char *name, A2Class *super, int kind, NativeFn2 ctor) {
    A2Class *c = a2_class_new(vm, ns, name, super, kind); extern void a2_global_class(VM2 *vm, A2Class *c);
    if (ctor) c->native_ctor = ctor;
    if (!builtin_as3_class(ns, name)) a2_global_class(vm, c);
    return c;
}
void a2_static_getter(VM2 *vm, A2Class *c, const char *name, NativeFn2 get, NativeFn2 set) {
    A2Trait *t = (A2Trait*)calloc(1, sizeof(A2Trait)); extern void a2_tm_put(A2TraitMap *tm, A2Trait *t);
    t->name = a2_str(vm, name); t->ns = vm->ns_public; t->kind = TK_GETTER;
    if (get) { t->getter = a2_native_method(vm, get, name, 0); t->getter->owner = c; } if (set) { t->setter = a2_native_method(vm, set, name, 1); t->setter->owner = c; }
    a2_tm_put(&c->ctraits, t);
}
void a2_const_str(VM2 *vm, A2Class *c, const char *name, const char *v) { a2_add_const(vm, c, name, v2_cstr(vm, v)); }

void a2_flash_display_init(VM2 *vm) {
    A2Class *c, *dobj, *io, *dc;
    vm->c_displayobject = dobj = c = a2_def_class(vm, "flash.display", "DisplayObject", vm->c_eventdispatcher, OK_DISPLAY, DisplayObject_ctor);
    a2_add_getter(vm, c, "name", do_name_get, do_name_set); a2_add_getter(vm, c, "x", do_x_get, do_x_set); a2_add_getter(vm, c, "y", do_y_get, do_y_set);
    a2_add_getter(vm, c, "scaleX", do_scaleX_get, do_scaleX_set); a2_add_getter(vm, c, "scaleY", do_scaleY_get, do_scaleY_set); a2_add_getter(vm, c, "rotation", do_rotation_get, do_rotation_set);
    a2_add_getter(vm, c, "width", do_width_get, do_width_set); a2_add_getter(vm, c, "height", do_height_get, do_height_set); a2_add_getter(vm, c, "alpha", do_alpha_get, do_alpha_set); a2_add_getter(vm, c, "visible", do_visible_get, do_visible_set);
    a2_add_getter(vm, c, "parent", do_parent_get, NULL); a2_add_getter(vm, c, "stage", do_stage_get, NULL); a2_add_getter(vm, c, "root", do_root_get, NULL); a2_add_getter(vm, c, "mouseX", do_mouseX_get, NULL); a2_add_getter(vm, c, "mouseY", do_mouseY_get, NULL);
    a2_add_getter(vm, c, "mask", do_mask_get, do_mask_set); a2_add_getter(vm, c, "transform", do_transform_get, do_transform_set); a2_add_getter(vm, c, "blendMode", do_blendMode_get, do_blendMode_set); a2_add_getter(vm, c, "filters", do_filters_get, do_filters_set);
    a2_add_getter(vm, c, "scrollRect", do_scrollRect_get, do_scrollRect_set); a2_add_getter(vm, c, "cacheAsBitmap", do_cacheAsBitmap_get, do_cacheAsBitmap_set); a2_add_getter(vm, c, "loaderInfo", do_loaderInfo_get, NULL); a2_add_getter(vm, c, "opaqueBackground", do_opaqueBackground_get, do_opaqueBackground_set);
    a2_add_getter(vm, c, "rotationX", do_rotationX_get, do_noop_set); a2_add_getter(vm, c, "rotationY", do_rotationX_get, do_noop_set); a2_add_getter(vm, c, "rotationZ", do_rotation_get, do_rotation_set); a2_add_getter(vm, c, "z", do_z_get, do_z_set); a2_add_getter(vm, c, "scaleZ", do_one_get, do_noop_set);
    a2_add_getter(vm, c, "scale9Grid", do_scale9Grid_get, do_noop_set); a2_add_getter(vm, c, "accessibilityProperties", do_accessibilityProperties_get, do_accessibilityProperties_set); a2_add_getter(vm, c, "metaData", do_metaData_get, do_metaData_set); { extern V2 a2_set_blend_shader(VM2 *vm, V2 self, int argc, V2 *argv); a2_add_getter(vm, c, "blendShader", NULL, a2_set_blend_shader); }
    a2_add_method(vm, c, "getBounds", do_getBounds, 1); a2_add_method(vm, c, "getRect", do_getRect, 1); a2_add_method(vm, c, "localToGlobal", do_localToGlobal, 1); a2_add_method(vm, c, "globalToLocal", do_globalToLocal, 1);
    a2_add_method(vm, c, "hitTestPoint", do_hitTestPoint, 3); a2_add_method(vm, c, "hitTestObject", do_hitTestObject, 1); a2_add_method(vm, c, "local3DToGlobal", do_localToGlobal, 1); a2_add_method(vm, c, "globalToLocal3D", do_globalToLocal, 1);
    c = a2_def_class(vm, "flash.display", "IBitmapDrawable", vm->c_object, OK_PLAIN, NULL); c->flags = CF_INTERFACE;
    vm->c_interactive = io = c = a2_def_class(vm, "flash.display", "InteractiveObject", dobj, OK_DISPLAY, DisplayObject_ctor);
    a2_add_getter(vm, c, "mouseEnabled", io_mouseEnabled_get, io_mouseEnabled_set); a2_add_getter(vm, c, "doubleClickEnabled", io_doubleClickEnabled_get, io_doubleClickEnabled_set); a2_add_getter(vm, c, "tabEnabled", io_tabEnabled_get, io_tabEnabled_set); a2_add_getter(vm, c, "accessibilityImplementation", io_accImpl_get, io_accImpl_set); a2_add_getter(vm, c, "tabIndex", io_tabIndex_get, io_tabIndex_set);
    a2_add_getter(vm, c, "focusRect", io_focusRect_get, io_focusRect_set); a2_add_getter(vm, c, "contextMenu", io_contextMenu_get, io_contextMenu_set); a2_add_getter(vm, c, "needsSoftKeyboard", do_cacheAsBitmap_get, do_noop_set); a2_add_getter(vm, c, "softKeyboardInputAreaOfInterest", do_opaqueBackground_get, do_noop_set);
    a2_add_method(vm, c, "requestSoftKeyboard", do_cacheAsBitmap_get, 0);
    vm->c_container = dc = c = a2_def_class(vm, "flash.display", "DisplayObjectContainer", io, OK_DISPLAY, DisplayObject_ctor);
    a2_add_method(vm, c, "addChild", dc_addChild, 1); a2_add_method(vm, c, "addChildAt", dc_addChildAt, 2); a2_add_method(vm, c, "removeChild", dc_removeChild, 1); a2_add_method(vm, c, "removeChildAt", dc_removeChildAt, 1); a2_add_method(vm, c, "removeChildren", dc_removeChildren, 2);
    a2_add_method(vm, c, "getChildAt", dc_getChildAt, 1); a2_add_method(vm, c, "getChildByName", dc_getChildByName, 1); a2_add_method(vm, c, "getChildIndex", dc_getChildIndex, 1); a2_add_method(vm, c, "setChildIndex", dc_setChildIndex, 2);
    a2_add_method(vm, c, "swapChildren", dc_swapChildren, 2); a2_add_method(vm, c, "swapChildrenAt", dc_swapChildrenAt, 2); a2_add_method(vm, c, "contains", dc_contains, 1); a2_add_method(vm, c, "getObjectsUnderPoint", dc_getObjectsUnderPoint, 1); a2_add_method(vm, c, "areInaccessibleObjectsUnderPoint", dc_areInaccessibleObjectsUnderPoint, 1); a2_add_method(vm, c, "stopAllMovieClips", dc_stopAllMovieClips, 0);
    a2_add_getter(vm, c, "numChildren", dc_numChildren, NULL); a2_add_getter(vm, c, "mouseChildren", dc_mouseChildren_get, dc_mouseChildren_set); a2_add_getter(vm, c, "tabChildren", dc_tabChildren_get, dc_tabChildren_set); a2_add_getter(vm, c, "textSnapshot", do_opaqueBackground_get, NULL);
    vm->c_sprite = c = a2_def_class(vm, "flash.display", "Sprite", dc, OK_DISPLAY, DisplayObject_ctor);
    a2_add_getter(vm, c, "graphics", sp_graphics, NULL); a2_add_getter(vm, c, "buttonMode", sp_buttonMode_get, sp_buttonMode_set); a2_add_getter(vm, c, "useHandCursor", sp_useHandCursor_get, sp_useHandCursor_set); a2_add_getter(vm, c, "hitArea", sp_hitArea_get, sp_hitArea_set);
    a2_add_getter(vm, c, "dropTarget", sp_dropTarget, NULL); a2_add_getter(vm, c, "soundTransform", sp_soundTransform_get, sp_soundTransform_set);
    a2_add_method(vm, c, "startDrag", sp_startDrag, 2); a2_add_method(vm, c, "stopDrag", sp_stopDrag, 0); a2_add_method(vm, c, "startTouchDrag", sp_startDrag, 3); a2_add_method(vm, c, "stopTouchDrag", sp_stopDrag, 1);
    vm->c_movieclip = c = a2_def_class(vm, "flash.display", "MovieClip", vm->c_sprite, OK_DISPLAY, DisplayObject_ctor); c->is_dynamic = 1;
    a2_add_method(vm, c, "play", mc_play, 0); a2_add_method(vm, c, "stop", mc_stop, 0); a2_add_method(vm, c, "gotoAndPlay", mc_gotoAndPlay, 2); a2_add_method(vm, c, "gotoAndStop", mc_gotoAndStop, 2); a2_add_method(vm, c, "nextFrame", mc_nextFrame, 0); a2_add_method(vm, c, "prevFrame", mc_prevFrame, 0);
    a2_add_method(vm, c, "nextScene", mc_nextScene, 0); a2_add_method(vm, c, "prevScene", mc_prevScene, 0); a2_add_method(vm, c, "addFrameScript", mc_addFrameScript, 2);
    a2_add_getter(vm, c, "currentFrame", mc_currentFrame, NULL); a2_add_getter(vm, c, "totalFrames", mc_totalFrames, NULL); a2_add_getter(vm, c, "framesLoaded", mc_framesLoaded, NULL); a2_add_getter(vm, c, "isPlaying", mc_isPlaying, NULL);
    a2_add_getter(vm, c, "currentLabel", mc_currentLabel, NULL); a2_add_getter(vm, c, "currentFrameLabel", mc_currentFrameLabel, NULL); a2_add_getter(vm, c, "currentLabels", mc_currentLabels, NULL); a2_add_getter(vm, c, "currentScene", mc_currentScene, NULL); a2_add_getter(vm, c, "scenes", mc_scenes, NULL);
    a2_add_getter(vm, c, "enabled", mc_enabled_get, mc_enabled_set); a2_add_getter(vm, c, "trackAsMenu", mc_trackAsMenu_get, do_noop_set);
    vm->c_shape = c = a2_def_class(vm, "flash.display", "Shape", dobj, OK_DISPLAY, Shape_ctor); a2_add_getter(vm, c, "graphics", sp_graphics, NULL);
    vm->c_graphics = c = a2_def_class(vm, "flash.display", "Graphics", vm->c_object, OK_GRAPHICS, Graphics_ctor);
    a2_add_method(vm, c, "clear", g_clear, 0); a2_add_method(vm, c, "beginFill", g_beginFill, 2); a2_add_method(vm, c, "beginGradientFill", g_beginGradientFill, 8); a2_add_method(vm, c, "beginBitmapFill", g_beginBitmapFill, 4); a2_add_method(vm, c, "beginShaderFill", g_noop, 2); a2_add_method(vm, c, "endFill", g_endFill, 0);
    a2_add_method(vm, c, "lineStyle", g_lineStyle, 8); a2_add_method(vm, c, "lineGradientStyle", g_lineGradientStyle, 8); a2_add_method(vm, c, "lineBitmapStyle", g_lineBitmapStyle, 4); a2_add_method(vm, c, "lineShaderStyle", g_noop, 2);
    a2_add_method(vm, c, "moveTo", g_moveTo, 2); a2_add_method(vm, c, "lineTo", g_lineTo, 2); a2_add_method(vm, c, "curveTo", g_curveTo, 4); a2_add_method(vm, c, "cubicCurveTo", g_cubicCurveTo, 6);
    a2_add_method(vm, c, "drawRect", g_drawRect, 4); a2_add_method(vm, c, "drawRoundRect", g_drawRoundRect, 6); a2_add_method(vm, c, "drawRoundRectComplex", g_drawRoundRectComplex, 8); a2_add_method(vm, c, "drawCircle", g_drawCircle, 3); a2_add_method(vm, c, "drawEllipse", g_drawEllipse, 4);
    a2_add_method(vm, c, "drawPath", g_drawPath, 3); a2_add_method(vm, c, "drawTriangles", g_drawTriangles, 4); a2_add_method(vm, c, "drawGraphicsData", g_drawGraphicsData, 1); a2_add_method(vm, c, "copyFrom", g_copyFrom, 1); a2_add_method(vm, c, "readGraphicsData", g_noop, 1);
    vm->c_stage = c = a2_def_class(vm, "flash.display", "Stage", dc, OK_DISPLAY, Stage_ctor);
    a2_add_getter(vm, c, "stageWidth", st_stageWidth, do_noop_set); a2_add_getter(vm, c, "stageHeight", st_stageHeight, do_noop_set); a2_add_getter(vm, c, "frameRate", st_frameRate_get, st_frameRate_set); a2_add_getter(vm, c, "scaleMode", st_scaleMode_get, st_scaleMode_set);
    a2_add_getter(vm, c, "align", st_align_get, st_align_set); a2_add_getter(vm, c, "quality", st_quality_get, st_quality_set); a2_add_getter(vm, c, "displayState", st_displayState_get, st_displayState_set); a2_add_getter(vm, c, "color", st_color_get, st_color_set);
    a2_add_getter(vm, c, "focus", st_focus_get, st_focus_set); a2_add_getter(vm, c, "stageFocusRect", st_stageFocusRect_get, st_stageFocusRect_set); a2_add_getter(vm, c, "showDefaultContextMenu", st_showDefaultContextMenu_get, do_noop_set);
    a2_add_getter(vm, c, "fullScreenWidth", st_fullScreenWidth, NULL); a2_add_getter(vm, c, "fullScreenHeight", st_fullScreenHeight, NULL); a2_add_getter(vm, c, "allowsFullScreen", st_allowsFullScreen, NULL); a2_add_getter(vm, c, "allowsFullScreenInteractive", r_false_st, NULL);
    a2_add_getter(vm, c, "stage3Ds", st_stage3Ds, NULL); a2_add_getter(vm, c, "stageVideos", st_stageVideos, NULL); a2_add_getter(vm, c, "nativeWindow", st_nativeWindow, NULL); a2_add_getter(vm, c, "contentsScaleFactor", st_contentsScaleFactor, NULL); a2_add_getter(vm, c, "softKeyboardRect", st_softKeyboardRect, NULL);
    a2_add_getter(vm, c, "fullScreenSourceRect", do_opaqueBackground_get, do_noop_set); a2_add_getter(vm, c, "mouseLock", do_cacheAsBitmap_get, do_noop_set); a2_add_getter(vm, c, "colorCorrection", st_str_get, st_colorCorrection_set); a2_add_getter(vm, c, "colorCorrectionSupport", st_str_get, NULL); a2_add_getter(vm, c, "wmodeGPU", do_cacheAsBitmap_get, NULL); a2_add_getter(vm, c, "browserZoomFactor", st_contentsScaleFactor, NULL);
    a2_add_method(vm, c, "invalidate", st_invalidate, 0); a2_add_method(vm, c, "isFocusInaccessible", do_cacheAsBitmap_get, 0);
    {   /* DisplayObject setters the Stage refuses (#2071) */
        static const char *const bad[] = { "accessibilityImplementation", "accessibilityProperties", "alpha", "blendMode", "cacheAsBitmap", "contextMenu", "filters", "focusRect", "height", "mask", "mouseEnabled", "name", "opaqueBackground", "rotation", "rotationX", "rotationY", "rotationZ", "scale9Grid", "scaleX", "scaleY", "scaleZ", "tabEnabled", "tabIndex", "transform", "visible", "width", "x", "y", "z", NULL };
        int i;
        for (i = 0; bad[i]; i++) { A2Trait *t = a2_find_trait_any(&c->itraits, a2_str(vm, bad[i])); if (t && (t->kind == TK_GETTER || t->kind == TK_SETTER)) { t->setter = a2_native_method(vm, st_bad_set, bad[i], 1); t->setter->owner = c; } }
        a2_add_getter(vm, c, "textSnapshot", st_bad_set, NULL);
        a2_add_getter(vm, c, "width", st_width_get, st_bad_set); a2_add_getter(vm, c, "height", st_height_get, st_bad_set);
    }
    c = a2_def_class(vm, "flash.display", "Bitmap", dobj, OK_DISPLAY, Bitmap_ctor); vm->c_bitmap = c;
    a2_add_getter(vm, c, "bitmapData", bmp_bitmapData_get, bmp_bitmapData_set); a2_add_getter(vm, c, "smoothing", bmp_smoothing_get, bmp_smoothing_set); a2_add_getter(vm, c, "pixelSnapping", bmp_pixelSnapping_get, bmp_pixelSnapping_set);
    vm->c_simplebutton = c = a2_def_class(vm, "flash.display", "SimpleButton", io, OK_DISPLAY, SimpleButton_ctor);
    a2_add_getter(vm, c, "upState", sb_upState_get, sb_upState_set); a2_add_getter(vm, c, "overState", sb_overState_get, sb_overState_set); a2_add_getter(vm, c, "downState", sb_downState_get, sb_downState_set); a2_add_getter(vm, c, "hitTestState", sb_hitTestState_get, sb_hitTestState_set);
    { extern V2 a2_sm_soundTransform_get(VM2 *vm, V2 self, int argc, V2 *argv); extern V2 a2_sm_soundTransform_set(VM2 *vm, V2 self, int argc, V2 *argv);   /* SimpleButton: the global transform (Flash, Ruffle) */
    a2_add_getter(vm, c, "enabled", sb_enabled_get, sb_enabled_set); a2_add_getter(vm, c, "useHandCursor", sp_useHandCursor_get, sp_useHandCursor_set); a2_add_getter(vm, c, "trackAsMenu", mc_trackAsMenu_get, do_noop_set); a2_add_getter(vm, c, "soundTransform", a2_sm_soundTransform_get, a2_sm_soundTransform_set);
    }
    vm->c_morphshape = a2_def_class(vm, "flash.display", "MorphShape", dobj, OK_DISPLAY, DisplayObject_ctor);
    c = a2_def_class(vm, "flash.display", "FrameLabel", vm->c_eventdispatcher, OK_PLAIN, FrameLabel_ctor); a2_add_getter(vm, c, "name", fl_name, NULL); a2_add_getter(vm, c, "frame", fl_frame, NULL);
    c = a2_def_class(vm, "flash.display", "Scene", vm->c_object, OK_PLAIN, Scene_ctor); a2_add_getter(vm, c, "name", fl_name, NULL); a2_add_getter(vm, c, "labels", sc_labels, NULL); a2_add_getter(vm, c, "numFrames", sc_numFrames, NULL);
    c = a2_def_class(vm, "flash.display", "StageScaleMode", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "EXACT_FIT", "exactFit"); a2_const_str(vm, c, "NO_BORDER", "noBorder"); a2_const_str(vm, c, "NO_SCALE", "noScale"); a2_const_str(vm, c, "SHOW_ALL", "showAll");
    c = a2_def_class(vm, "flash.display", "StageAlign", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "TOP", "T"); a2_const_str(vm, c, "BOTTOM", "B"); a2_const_str(vm, c, "LEFT", "L"); a2_const_str(vm, c, "RIGHT", "R"); a2_const_str(vm, c, "TOP_LEFT", "TL"); a2_const_str(vm, c, "TOP_RIGHT", "TR"); a2_const_str(vm, c, "BOTTOM_LEFT", "BL"); a2_const_str(vm, c, "BOTTOM_RIGHT", "BR");
    c = a2_def_class(vm, "flash.display", "StageQuality", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "LOW", "low"); a2_const_str(vm, c, "MEDIUM", "medium"); a2_const_str(vm, c, "HIGH", "high"); a2_const_str(vm, c, "BEST", "best"); a2_const_str(vm, c, "HIGH_8X8", "8x8"); a2_const_str(vm, c, "HIGH_8X8_LINEAR", "8x8linear"); a2_const_str(vm, c, "HIGH_16X16", "16x16"); a2_const_str(vm, c, "HIGH_16X16_LINEAR", "16x16linear");
    c = a2_def_class(vm, "flash.display", "StageDisplayState", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "NORMAL", "normal"); a2_const_str(vm, c, "FULL_SCREEN", "fullScreen"); a2_const_str(vm, c, "FULL_SCREEN_INTERACTIVE", "fullScreenInteractive");
    c = a2_def_class(vm, "flash.display", "BlendMode", vm->c_object, OK_PLAIN, NULL); { const char *k[] = { "NORMAL", "normal", "LAYER", "layer", "MULTIPLY", "multiply", "SCREEN", "screen", "LIGHTEN", "lighten", "DARKEN", "darken", "DIFFERENCE", "difference", "ADD", "add", "SUBTRACT", "subtract", "INVERT", "invert", "ALPHA", "alpha", "ERASE", "erase", "OVERLAY", "overlay", "HARDLIGHT", "hardlight", "SHADER", "shader", NULL }; int i; for (i = 0; k[i]; i += 2) a2_const_str(vm, c, k[i], k[i + 1]); }
    c = a2_def_class(vm, "flash.display", "GradientType", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "LINEAR", "linear"); a2_const_str(vm, c, "RADIAL", "radial");
    c = a2_def_class(vm, "flash.display", "SpreadMethod", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "PAD", "pad"); a2_const_str(vm, c, "REFLECT", "reflect"); a2_const_str(vm, c, "REPEAT", "repeat");
    c = a2_def_class(vm, "flash.display", "InterpolationMethod", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "RGB", "rgb"); a2_const_str(vm, c, "LINEAR_RGB", "linearRGB");
    c = a2_def_class(vm, "flash.display", "LineScaleMode", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "NORMAL", "normal"); a2_const_str(vm, c, "NONE", "none"); a2_const_str(vm, c, "VERTICAL", "vertical"); a2_const_str(vm, c, "HORIZONTAL", "horizontal");
    c = a2_def_class(vm, "flash.display", "CapsStyle", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "NONE", "none"); a2_const_str(vm, c, "ROUND", "round"); a2_const_str(vm, c, "SQUARE", "square");
    c = a2_def_class(vm, "flash.display", "JointStyle", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "MITER", "miter"); a2_const_str(vm, c, "ROUND", "round"); a2_const_str(vm, c, "BEVEL", "bevel");
    c = a2_def_class(vm, "flash.display", "PixelSnapping", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "ALWAYS", "always"); a2_const_str(vm, c, "AUTO", "auto"); a2_const_str(vm, c, "NEVER", "never");
    c = a2_def_class(vm, "flash.display", "GraphicsPathCommand", vm->c_object, OK_PLAIN, NULL); a2_add_const(vm, c, "NO_OP", v2_int(0)); a2_add_const(vm, c, "MOVE_TO", v2_int(1)); a2_add_const(vm, c, "LINE_TO", v2_int(2)); a2_add_const(vm, c, "CURVE_TO", v2_int(3)); a2_add_const(vm, c, "WIDE_MOVE_TO", v2_int(4)); a2_add_const(vm, c, "WIDE_LINE_TO", v2_int(5)); a2_add_const(vm, c, "CUBIC_CURVE_TO", v2_int(6));
    c = a2_def_class(vm, "flash.display", "GraphicsPathWinding", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "EVEN_ODD", "evenOdd"); a2_const_str(vm, c, "NON_ZERO", "nonZero");
    c = a2_def_class(vm, "flash.display", "TriangleCulling", vm->c_object, OK_PLAIN, NULL); a2_const_str(vm, c, "NONE", "none"); a2_const_str(vm, c, "POSITIVE", "positive"); a2_const_str(vm, c, "NEGATIVE", "negative");
    c = a2_def_class(vm, "flash.display", "ActionScriptVersion", vm->c_object, OK_PLAIN, NULL); a2_add_const(vm, c, "ACTIONSCRIPT2", v2_uint(2)); a2_add_const(vm, c, "ACTIONSCRIPT3", v2_uint(3));
    c = a2_def_class(vm, "flash.display", "SWFVersion", vm->c_object, OK_PLAIN, NULL); a2_add_const(vm, c, "FLASH9", v2_uint(9)); a2_add_const(vm, c, "FLASH10", v2_uint(10)); a2_add_const(vm, c, "FLASH11", v2_uint(11)); a2_add_const(vm, c, "FLASH12", v2_uint(12));
    c = a2_def_class(vm, "flash.display", "AVM1Movie", vm->c_displayobject, OK_DISPLAY, DisplayObject_ctor);   /* Loader content of an ActionScript 1/2 movie */
    a2_add_method(vm, c, "call", avm1movie_call, 1); a2_add_method(vm, c, "addCallback", avm1movie_addCallback, 2);
    { A2Name n; A2Trait *t; n.nss = &vm->ns_public; n.nns = 1; n.any_ns = 0; n.attr = 0;
      n.name = a2_str(vm, "call"); t = a2_find_trait(&c->itraits, n.name, &n); if (t && t->method) { t->method->argmin = 1; t->method->argmax = -1; }
      n.name = a2_str(vm, "addCallback"); t = a2_find_trait(&c->itraits, n.name, &n); if (t && t->method) { t->method->argmin = 2; t->method->argmax = 2; } }
    { const char *names[] = { "IGraphicsData", "IGraphicsFill", "IGraphicsStroke", "IGraphicsPath", "IDrawCommand", "NativeWindow", "Screen", "StageOrientation", "ColorCorrection", "ColorCorrectionSupport", "FocusDirection", "BitmapDataChannel", "BitmapEncodingColorSpace", "JPEGEncoderOptions", "PNGEncoderOptions", "JPEGXREncoderOptions", NULL }; int i;
      for (i = 0; names[i]; i++) { c = a2_def_class(vm, "flash.display", names[i], vm->c_object, OK_PLAIN, NULL); c->is_dynamic = 1; if (names[i][0] == 'I' && names[i][1] >= 'A' && names[i][1] <= 'Z') c->flags = CF_INTERFACE; } }
    { struct { const char *n; NativeFn2 f; } gd[] = { { "GraphicsPath", GPath_ctor }, { "GraphicsSolidFill", GSolidFill_ctor }, { "GraphicsGradientFill", GGradFill_ctor }, { "GraphicsBitmapFill", GBitmapFill_ctor }, { "GraphicsStroke", GStroke_ctor }, { "GraphicsEndFill", g_noop }, { "GraphicsTrianglePath", GTriPath_ctor }, { NULL, NULL } }; int i;
      for (i = 0; gd[i].n; i++) { c = a2_def_class(vm, "flash.display", gd[i].n, vm->c_object, OK_PLAIN, gd[i].f); c->is_dynamic = 1; c->flags |= CF_FINAL;
        add_iface(vm, c, "flash.display::IGraphicsData");
        if (strstr(gd[i].n, "Fill")) add_iface(vm, c, "flash.display::IGraphicsFill");
        if (!strcmp(gd[i].n, "GraphicsStroke")) add_iface(vm, c, "flash.display::IGraphicsStroke");
        if (!strcmp(gd[i].n, "GraphicsPath") || !strcmp(gd[i].n, "GraphicsTrianglePath")) add_iface(vm, c, "flash.display::IGraphicsPath");
        if (!strcmp(gd[i].n, "GraphicsPath")) { a2_add_method(vm, c, "moveTo", gp_moveTo, 2); a2_add_method(vm, c, "lineTo", gp_lineTo, 2); a2_add_method(vm, c, "curveTo", gp_curveTo, 4); a2_add_method(vm, c, "cubicCurveTo", gp_cubicCurveTo, 6); a2_add_method(vm, c, "wideMoveTo", gp_wideMoveTo, 2); a2_add_method(vm, c, "wideLineTo", gp_wideLineTo, 2); } } }
    { extern void a2_shader_init(VM2 *vm); a2_shader_init(vm); }
    { A2Class *bdc = a2_class_find_s(vm, "flash.display::BitmapDataChannel"); a2_add_const(vm, bdc, "RED", v2_uint(1)); a2_add_const(vm, bdc, "GREEN", v2_uint(2)); a2_add_const(vm, bdc, "BLUE", v2_uint(4)); a2_add_const(vm, bdc, "ALPHA", v2_uint(8)); }
}

/* ---------------------------------------------------------------- */
/* player hooks                                                     */

static void vm_setup(VM2 *vm, Player *p) {
    vm->player = p;
    vm->trace = 1;
    vm->debugger = getenv("FLASHNT_DEBUGGER") != NULL;   /* stack traces as the debug player prints them for SWFs with debug info (test harness option) */
    vm->swf_version = p->movie->version;
    { const char *rtm0 = getenv("FLASHNT_RUNTIME"); vm->air = rtm0 && !strcmp(rtm0, "AIR"); }
    { extern void a2_api_set_swf(int ver); const char *rtm = getenv("FLASHNT_RUNTIME"); a2_api_set_swf(rtm && !strcmp(rtm, "AIR") ? 70 : p->movie->version); }
    a2_lib_init(vm);
    a2_flash_init(vm);
    vm->frame_ms = p->movie->framerate > 0 ? 1000.0 / p->movie->framerate : 1000.0 / 12;
    vm->swf_version = p->movie->version;
    { const char *rtm = getenv("FLASHNT_RUNTIME"); vm->air = rtm && !strcmp(rtm, "AIR"); }
    /* stage object */
    vm->stage_obj = a2_new(vm, vm->c_stage); vm->stage_obj->kind = OK_DISPLAY; a2_disp(vm->stage_obj)->is_stage = 1;
    /* the root timeline gets a placement record so x/y/scale work on it */
    if (!p->root->self) { DisplayObject *rd = (DisplayObject*)calloc(1, sizeof(DisplayObject)); rd->depth = 0; rd->char_id = -1; rd->visible = 1; swf_matrix_identity(&rd->matrix); swf_cxform_identity(&rd->cxform); rd->clip = p->root; p->root->self = rd; }
}
void avm2_create(Player *p) {
    VM2 *vm;
    if (p->avm2) return;
    vm = (VM2*)calloc(1, sizeof(VM2));
    p->avm2 = vm;
    vm_setup(vm, p);
    { extern void a2_bitmapdata_set_player(Player *p); extern const Bitmap *a2_bitmapdata_lookup(int id); extern void render_set_bitmapdata_lookup(const Bitmap *(*fn)(int id)); a2_bitmapdata_set_player(p); render_set_bitmapdata_lookup(a2_bitmapdata_lookup); }
}
void avm2_free(Player *p) {
    VM2 *vm = (VM2*)p->avm2; A2GC *h; int i;
    if (!vm) return;
    /* release everything */
    for (h = vm->gc_list; h; ) { A2GC *n = h->next; if (h->kind == G_OBJ) { A2Obj *o = (A2Obj*)h; if (o->kind == OK_DISPLAY && o->payload) ((A2Disp*)o->payload)->owns = 0; } h = n; }
    for (i = 0; i < vm->capintern; i++) { A2Str *s; for (s = vm->intern ? vm->intern[i] : NULL; s; s = s->inext) s->gc.mark = 0; }
    { A2GC **pp = &vm->gc_list; while (*pp) { A2GC *x = *pp; *pp = x->next; if (x->kind == G_STR) free(((A2Str*)x)->s); else if (x->kind == G_OBJ) { extern void a2_obj_free_for_shutdown(A2Obj *o); a2_obj_free_for_shutdown((A2Obj*)x); } else { extern void a2_class_free_for_shutdown(A2Class *c); a2_class_free_for_shutdown((A2Class*)x); } free(x); } }
    for (i = 0; i < vm->nabcs; i++) { AbcFile *a = vm->abcs[i]; extern void a2_rt_free(AbcFile *a); a2_rt_free(a); abc_free(a); }
    free(vm->abcs); free(vm->intern); free(vm->classes); free(vm->globals); free(vm->bcast); free(vm->bcast_t); free(vm->orphans); free(vm->orphan_tl); free(vm->pending); free(vm->symbols); free(vm->defer_rt);
    free(vm);
    p->avm2 = NULL;
}
void avm2_do_abc(Player *p, const u8 *data, size_t len) {
    VM2 *vm; size_t off = 0; int lazy = 0;
    if (!p->avm2) avm2_create(p);
    vm = (VM2*)p->avm2;
    {   /* code that names Ruffle's __ruffle__ package: keep Ruffle's string bookkeeping (test harness) */
        extern int g_a2_sdep_on; size_t i;
        if (!g_a2_sdep_on && getenv("FLASHNT_HARNESS")) for (i = 0; i + 10 <= len; i++) if (data[i] == '_' && !memcmp(data + i, "__ruffle__", 10)) { g_a2_sdep_on = 1; break; }
    }
    /* DoABC (82): u32 flags, string name, abc.  DoABC1 (72): raw abc */
    if (len >= 4) { u32 flags = data[0] | (data[1] << 8) | (data[2] << 16) | ((u32)data[3] << 24); off = 4; while (off < len && data[off]) off++; off++; lazy = flags & 1; if (off > len) off = len; }
    avm2_load_abc(vm, data + off, len - off, lazy);
}
void avm2_do_abc1(Player *p, const u8 *data, size_t len) { VM2 *vm; if (!p->avm2) avm2_create(p); vm = (VM2*)p->avm2; avm2_load_abc(vm, data, len, 0); }
/* SymbolClass may come before the DoABC that defines its classes (in the same
   frame): unresolved links wait for the next DoABC; whatever is still missing
   when the frame has been processed is reported. */
static struct { int id; char *name; } *g_pend_sym; static int g_npend_sym;
static int bind_symbol(VM2 *vm, int id, const char *name) {
    Player *p = vm->player; A2Class *c = a2_class_find_s(vm, name);
    if (!c) return 0;
    if (vm->player->debug > 1) fprintf(stderr, "[avm2] SymbolClass %s -> id %d\n", name, id);
    /* an id with no character (most SWFs use 0, obfuscated ones other ids) binds the main timeline */
    if (id != 0 && !swf_get_char(PMOV(p), id)) { int k, has_root = 0; for (k = 0; k < vm->nsymbols; k++) if (vm->symbols[k].id == 0 && vm->symbols[k].movie == PMOV(p)) has_root = 1; if (!has_root) id = 0; }
    vm->symbols = realloc(vm->symbols, (size_t)(vm->nsymbols + 1) * sizeof(*vm->symbols));
    vm->symbols[vm->nsymbols].id = id; vm->symbols[vm->nsymbols].cls = c; vm->symbols[vm->nsymbols].movie = PMOV(p); vm->nsymbols++;
    c->symbol_char = id; c->symbol_movie = PMOV(p);
    /* Flash constructs a BitmapData subclass linked to a bitmap symbol once, with (1, 1), when the link is made */
    if (id && vm->c_bitmapdata && a2_is_subclass(c, vm->c_bitmapdata)) { V2 ar[2]; int na = 2; ar[0] = v2_int(1); ar[1] = v2_int(1); if (c->iinit && !c->iinit->native && !c->iinit->has_rest && !c->iinit->need_args && c->iinit->nparams < na) na = c->iinit->nparams; a2_construct_class(vm, c, na, ar); if (vm->throwing) a2_report_uncaught(vm); }
    return 1;
}
void avm2_symbol_class(Player *p, const u8 *data, size_t len) {
    VM2 *vm; int n, i; size_t pos = 2;
    if (!p->avm2) avm2_create(p);
    vm = (VM2*)p->avm2;
    if (len < 2) return;
    n = data[0] | (data[1] << 8);
    for (i = 0; i < n && pos + 2 < len; i++) {
        int id = data[pos] | (data[pos + 1] << 8); const char *name = (const char*)data + pos + 2;
        pos += 2 + strlen(name) + 1;
        if (!bind_symbol(vm, id, name)) {
            g_pend_sym = realloc(g_pend_sym, (size_t)(g_npend_sym + 1) * sizeof(*g_pend_sym));
            g_pend_sym[g_npend_sym].id = id; g_pend_sym[g_npend_sym].name = (char*)malloc(strlen(name) + 1); strcpy(g_pend_sym[g_npend_sym].name, name); g_npend_sym++;
        }
    }
}
void avm2_retry_symbols(VM2 *vm) {
    int i, k = 0;
    for (i = 0; i < g_npend_sym; i++) { if (bind_symbol(vm, g_pend_sym[i].id, g_pend_sym[i].name)) free(g_pend_sym[i].name); else g_pend_sym[k++] = g_pend_sym[i]; }
    g_npend_sym = k;
}
void avm2_report_missing_symbols(VM2 *vm) {
    int i;
    for (i = 0; i < g_npend_sym; i++) {
        char b[300]; const char *name = g_pend_sym[i].name, *ln = strrchr(name, '.');
        fprintf(stderr, "[avm2] SymbolClass: class %s not found for id %d\n", name, g_pend_sym[i].id);
        sprintf(b, "Variable %.250s is not defined.", ln ? ln + 1 : name);
        a2_throw_error(vm, "ReferenceError", b, 1065); a2_report_uncaught(vm);
        free(g_pend_sym[i].name);
    }
    g_npend_sym = 0;
}
void avm2_object_created(Player *p, DisplayObject *d) {
    VM2 *vm = (VM2*)p->avm2;
    if (!vm || d->name[0]) return;
    sprintf(d->name, "instance%d", ++vm->instance_counter);
}
void avm2_object_placed(Player *p, DisplayObject *d) {
    VM2 *vm = (VM2*)p->avm2;
    if (!vm) return;
    { A2Obj *po = d->owner ? (A2Obj*)d->owner->obj2 : NULL; d->manual_construct = !(po && (po->ival & 1)); }
    if (vm->npending == vm->cappending) { int nc = vm->cappending ? vm->cappending * 2 : 64; DisplayObject **np = (DisplayObject**)realloc(vm->pending, (size_t)nc * sizeof(DisplayObject*)); if (!np) return; vm->pending = np; vm->cappending = nc; }
    vm->pending[vm->npending++] = d;
}
static int owned_under(DisplayObject *d, MovieClip *c) { MovieClip *o = d->owner; int g = 0; while (o && g++ < 256) { if (o == c) return 1; o = o->parent; } return 0; }
/* clips whose constructor is constructing their timeline children: a nested
   construction pass (e.g. a goto inside a child constructor) leaves the
   remaining children to that outer pass, as Flash does */
static MovieClip *g_cons_stack[64]; static int g_ncons;
static int cons_owner_busy(DisplayObject *d, int level) { int k; for (k = 0; k < level; k++) if (g_cons_stack[k] == d->owner) return 1; return 0; }
/* display-list order: parents before children, siblings by depth (Flash
   constructs a frame's new objects walking the display list) */
static int dobj_chain(DisplayObject *d, DisplayObject **out, int cap) {
    int n = 0;
    while (d && n < cap) { out[n++] = d; d = (d->owner && d->owner->self) ? d->owner->self : NULL; }
    return n;   /* out[n-1] is the top-most record */
}
static int sibling_index(DisplayObject *d) { int i = 0; DisplayObject *k; if (!d->owner) return 1 << 30; for (k = d->owner->children; k; k = k->next, i++) if (k == d) return i; return 1 << 30; }
static int dobj_before(DisplayObject *a, DisplayObject *b) {
    DisplayObject *ca[64], *cb[64]; int na = dobj_chain(a, ca, 64), nb = dobj_chain(b, cb, 64), i, j;
    if (ca[na - 1]->owner != cb[nb - 1]->owner) return 0;   /* different trees: keep placement order */
    for (i = na - 1, j = nb - 1; i >= 0 && j >= 0; i--, j--) {
        if (ca[i] != cb[j]) return sibling_index(ca[i]) < sibling_index(cb[j]);
    }
    return i < j;   /* the ancestor comes first */
}
static void construct_pending_ex(VM2 *vm, MovieClip *c, MovieClip *push) {
    int guard = 0, i = 0, level = g_ncons;
    if (push && g_ncons < 64) g_cons_stack[g_ncons++] = push;
    while (i < vm->npending && guard++ < 100000) {
        DisplayObject *d = vm->pending[i];
        if ((c && !owned_under(d, c)) || cons_owner_busy(d, level) || (d->manual_construct && !(push && d->owner == push))) { i++; continue; }
        { int k, best = i;   /* pick the first eligible object in display-list order */
          for (k = i + 1; k < vm->npending; k++) { DisplayObject *e = vm->pending[k]; if ((c && !owned_under(e, c)) || cons_owner_busy(e, level) || !e->owner || (e->manual_construct && !(push && e->owner == push))) continue; if (d->owner && dobj_before(e, d)) { d = e; best = k; } }
          i = best; }
        memmove(vm->pending + i, vm->pending + i + 1, (size_t)(vm->npending - i - 1) * sizeof(DisplayObject*)); vm->npending--;
        if (!d->owner || (d->clip && d->clip->dead)) continue;
        a2_construct_display(vm, d->clip, d);
        i = 0;
    }
    g_ncons = level;
}
static void construct_pending_under(VM2 *vm, MovieClip *c) { construct_pending_ex(vm, c, NULL); }
static void construct_pending(VM2 *vm) { construct_pending_under(vm, NULL); }
static int g_goto_shown;
void avm2_frame_shown(Player *p, MovieClip *c) {
    VM2 *vm = (VM2*)p->avm2;
    if (!vm) return;
    c->frame_changed = 1;
    if (c->last_sf != clip_current_frame(c) + 1) c->last_sf = 0;   /* a new frame: its script may run again */
    /* a timeline sprite running its first frame is itself still pending:
     * its children wait for the enclosing frame (classes may still be bound) */
    if (c == p->root && !vm->root_constructed) {
        /* the document class constructor runs first; its timeline children are
         * constructed inside the implicit super() call (DisplayObject_ctor) */
        vm->root_constructed = 1;
        a2_construct_display(vm, p->root, NULL);
    }
    if (c != p->root && c->movie && c->tl == &c->movie->root && c->movie != p->movie && !(c->obj2 && (((A2Obj*)c->obj2)->ival & 1))) return;   /* Loader content: constructed by the loader */
    if (vm->in_enterframe || c->goto_busy) return;   /* timeline advance / goto in progress: construction comes later */
    if (vm->in_fc && c->movie && c->movie->version <= 9) return;   /* SWF9: a goto in frameConstructed constructs its children in the frame-script pass */
    if (c == p->root) construct_pending(vm);
    else if (g_goto_shown && c->obj2 && (((A2Obj*)c->obj2)->ival & 1)) construct_pending_under(vm, c);   /* a goto's construct pass: a nested frame (button) may construct the rest */
    else if ((c->obj2 && (((A2Obj*)c->obj2)->ival & 1)) || (c->self && c->self->depth < 0)) construct_pending_ex(vm, c, c);
}
void avm2_goto_shown(Player *p, MovieClip *c) { g_goto_shown = 1; avm2_frame_shown(p, c); g_goto_shown = 0; }
int avm2_dobj_removed(Player *p, DisplayObject *d) {
    VM2 *vm = (VM2*)p->avm2; A2Obj *o;
    if (!vm) return 0;
    o = (A2Obj*)(d->obj2 ? d->obj2 : (d->clip ? d->clip->obj2 : NULL));
    { int i, was = 0;
      for (i = 0; i < vm->npending; i++) if (vm->pending[i] == d) { memmove(vm->pending + i, vm->pending + i + 1, (size_t)(vm->npending - i - 1) * sizeof(DisplayObject*)); vm->npending--; was = 1; break; }
      /* a never-constructed object takes its pending timeline children with it */
      if (was && d->clip) for (i = 0; i < vm->npending; ) { if (owned_under(vm->pending[i], d->clip)) { memmove(vm->pending + i, vm->pending + i + 1, (size_t)(vm->npending - i - 1) * sizeof(DisplayObject*)); vm->npending--; } else i++; } }
    if (!o) return 0;
    if (!o->parent_disp && d->owner && d->owner->obj2) o->parent_disp = (A2Obj*)d->owner->obj2;
    remove_child_events(vm, o);
    /* the timeline took a named instance away: the parent's property is set to null unless it
       reads null/undefined (getters and setters run; read errors are swallowed) */
    if (d->name[0] && d->explicit_name && d->owner && d->owner->obj2 && !vm->throwing) {
        A2Obj *par = (A2Obj*)d->owner->obj2; A2Name n; V2 cur;
        n.name = a2_str(vm, d->name); n.nss = &vm->ns_public; n.nns = 1; n.any_ns = 0; n.attr = 0;
        cur = v2_undef();
        if (!a2_get(vm, v2_obj(par), &n, &cur) || vm->throwing) { vm->throwing = 0; cur = v2_undef(); }
        if (cur.t != T_UNDEF && cur.t != T_NULL) {
            if (vm->player->debug) fprintf(stderr, "[avm2] timeline removed %s: property cleared\n", d->name);
            a2_set(vm, v2_obj(par), &n, v2_null());
            if (vm->throwing) a2_report_uncaught(vm);
        }
    }
    d->owner = NULL; if (d->clip) d->clip->parent = NULL;
    o->parent_disp = NULL; a2_disp(o)->owns = 1; d->next = NULL;
    a2_orphan_add(vm, o, 1);
    if (p->focus && d->clip == p->focus) p->focus = NULL;
    if (vm->focus == o) vm->focus = NULL;
    return 1;
}
void avm2_dobj_detach(DisplayObject *d) { A2Obj *o = (A2Obj*)d->obj2; if (o) { if (o->dobj == d) o->dobj = NULL; if (o->payload) ((A2Disp*)o->payload)->owns = 0; } d->obj2 = NULL; }
void avm2_clip_detach(MovieClip *c) { A2Obj *o = (A2Obj*)c->obj2; if (o) { if (o->clip == c) o->clip = NULL; if (o->payload) { ((A2Disp*)o->payload)->owns = 0; if (((A2Disp*)o->payload)->mask == c) ((A2Disp*)o->payload)->mask = NULL; } } c->obj2 = NULL; }

static void run_state_frame_scripts(VM2 *vm, MovieClip *c) {
    DisplayObject *d; MovieClip *kids[256]; int n = 0, i;
    if (c->dead) return;
    if (c->frame_changed && c->obj2) { c->frame_changed = 0; a2_run_frame_scripts(vm, (A2Obj*)c->obj2, clip_current_frame(c)); }
    for (d = c->children; d && n < 256; d = d->next) if (d->clip) kids[n++] = d->clip;
    for (i = 0; i < n; i++) if (!kids[i]->dead) run_state_frame_scripts(vm, kids[i]);
}
static void run_pending_frame_scripts(VM2 *vm, MovieClip *c) {
    DisplayObject *d; MovieClip *kids[2048]; int n = 0, i;
    if (c->dead) return;
    if (c->obj2 && !(((A2Obj*)c->obj2)->ival & 1) && c != vm->player->root) return;   /* not constructed yet (placed by a goto): its scripts run once it is */
    if (c->movie && c->movie->version <= 9 && vm->npending) {   /* SWF9: children a goto left unconstructed are constructed before the clip's frame script */
        int k; for (k = 0; k < vm->npending; k++) if (vm->pending[k]->owner == c) { g_swf9_deferred++; construct_pending_under(vm, c); g_swf9_deferred--; break; } }
    { int guard = 0;
      while (!(c->obj2 && (((A2Obj*)c->obj2)->ival & 2) && !((A2Obj*)c->obj2)->nframe_scripts) && c->frame_changed && guard++ < 64 && !clip_in_frame_script(vm, c)) {   /* a goto queued by the script runs the new frame's script first */
          c->frame_changed = 0;
          if (c->obj2) a2_run_frame_scripts(vm, (A2Obj*)c->obj2, clip_current_frame(c));
          if (c->dead) return;
      } }
    for (d = c->children; d && n < 2048; d = d->next) if (d->clip) kids[n++] = d->clip;
    for (i = 0; i < n; i++) if (!kids[i]->dead) run_pending_frame_scripts(vm, kids[i]);
}
/* ---- orphans: parentless movie clips keep running their timelines (Ruffle orphan_manager.rs) ---- */
void a2_orphan_add(VM2 *vm, A2Obj *o, int timeline_removed) {
    MovieClip *c; A2Disp *st;
    if (!o || o->kind != OK_DISPLAY) return;
    c = clip_of(o);
    if (!c || !c->tl || c->dead) return;
    st = a2_disp(o);
    if (st->is_root || st->is_stage || st->button_state) return;
    if (st->orphan_listed) {   /* already listed: a script removal makes the entry permanent */
        int i; if (!timeline_removed) for (i = 0; i < vm->norphans; i++) if (vm->orphans[i] == o) vm->orphan_tl[i] = 0;
        return;
    }
    if (vm->norphans == vm->caporphans) {
        int nc = vm->caporphans ? vm->caporphans * 2 : 32;
        A2Obj **no = (A2Obj**)realloc(vm->orphans, (size_t)nc * sizeof(A2Obj*)); unsigned char *nt;
        if (!no) return; vm->orphans = no;
        nt = (unsigned char*)realloc(vm->orphan_tl, (size_t)nc); if (!nt) return; vm->orphan_tl = nt;
        vm->caporphans = nc;
    }
    vm->orphans[vm->norphans] = o; vm->orphan_tl[vm->norphans] = (unsigned char)(timeline_removed != 0); vm->norphans++;
    st->orphan_listed = 1;
}
static MovieClip *orphan_clip(VM2 *vm, A2Obj *o) {
    MovieClip *c = clip_of(o); DisplayObject *d = a2_dobj_of(o);
    if (!c || c->dead || !c->tl) return NULL;
    if (d && d->owner) return NULL;
    if (o->parent_disp || c->parent) return NULL;
    if (a2_parent_of(vm, o)) return NULL;
    return c;
}
/* a snapshot of the live orphans (additions during the walk wait for the next phase) */
static int orphan_snapshot(VM2 *vm, A2Obj ***out) {
    int i, n = 0; A2Obj **v;
    *out = NULL;
    if (!vm->norphans) return 0;
    v = (A2Obj**)malloc((size_t)vm->norphans * sizeof(A2Obj*));
    if (!v) return 0;
    for (i = 0; i < vm->norphans; i++) v[n++] = vm->orphans[i];
    *out = v; return n;
}
static int orphan_alive(VM2 *vm, A2Obj *o) { int i; for (i = 0; i < vm->norphans; i++) if (vm->orphans[i] == o) return 1; return 0; }
void avm2_orphans_enter(Player *p) {
    extern void player_tick_orphan(MovieClip *c); extern void player_clear_ran(MovieClip *c);
    VM2 *vm = (VM2*)p->avm2; A2Obj **v; int n, i;
    if (!vm) return;
    n = orphan_snapshot(vm, &v);
    for (i = 0; i < n; i++) { MovieClip *c; if (!orphan_alive(vm, v[i])) continue; c = orphan_clip(vm, v[i]); if (c && (v[i]->ival & 1)) { player_tick_orphan(c); player_clear_ran(c); } }
    free(v);
}
static void orphans_frame_scripts(VM2 *vm) {
    A2Obj **v; int n, i;
    n = orphan_snapshot(vm, &v);
    for (i = 0; i < n; i++) { MovieClip *c; if (!orphan_alive(vm, v[i])) continue; c = orphan_clip(vm, v[i]); if (c) run_pending_frame_scripts(vm, c); }
    free(v);
}
static int orphans_frame_changed(VM2 *vm) {
    int i;
    for (i = 0; i < vm->norphans; i++) { MovieClip *c = orphan_clip(vm, vm->orphans[i]); if (c && c->frame_changed && c->obj2 && (((A2Obj*)c->obj2)->ival & 1)) return 1; }
    return 0;
}
/* end of a frame: entries that found a parent, and clips a RemoveObject tag took away, leave the list */
static void orphans_cleanup(VM2 *vm) {
    extern void player_clear_ran(MovieClip *c);
    int i, j = 0;
    for (i = 0; i < vm->norphans; i++) {
        A2Obj *o = vm->orphans[i]; MovieClip *c = orphan_clip(vm, o);
        if (c && !vm->orphan_tl[i]) { player_clear_ran(c); vm->orphans[j] = o; vm->orphan_tl[j++] = 0; }
        else if (o->payload) ((A2Disp*)o->payload)->orphan_listed = 0;
    }
    vm->norphans = j;
}
void a2_prune_orphans(VM2 *vm) {
    int i, j = 0;
    for (i = 0; i < vm->norphans; i++) if (vm->orphans[i]->gc.mark) { vm->orphans[j] = vm->orphans[i]; vm->orphan_tl[j++] = vm->orphan_tl[i]; }
    vm->norphans = j;
}
/* run the frame scripts queued during the frame-script phase, first in first out */
static void run_fs_cleanup(VM2 *vm) {
    int guard = 0;
    while (vm->nfsq > 0 && guard++ < 1024) {
        A2Obj *o = vm->fsq[0]; MovieClip *c;
        memmove(vm->fsq, vm->fsq + 1, (size_t)(vm->nfsq - 1) * sizeof(A2Obj*)); vm->nfsq--;
        c = clip_of(o);
        if (!c || c->dead || !c->tl || clip_in_frame_script(vm, c)) continue;
        c->last_sf = 0; c->frame_changed = 0;
        a2_run_frame_scripts(vm, o, clip_current_frame(c));
    }
}
static int any_frame_changed(MovieClip *c) { DisplayObject *d; if (c->frame_changed) return 1; for (d = c->children; d; d = d->next) if (d->clip && any_frame_changed(d->clip)) return 1; return 0; }

/* frame lifecycle (Flash Player 10+):
 *   enter:      timelines advance (removals and placements, no construction),
 *               then enterFrame is broadcast
 *   construct:  new timeline children are constructed, frameConstructed
 *   scripts:    frame scripts run
 *   exit:       exitFrame                                              */
void avm2_tick_begin(Player *p) {
    VM2 *vm = (VM2*)p->avm2;
    if (!vm) return;
    vm->time_ms += vm->frame_ms; vm->frame_no++;
    a2_timers_tick(vm, vm->frame_ms);
    /* the end of the previous frame (Ruffle Player::tick): streams move along after the timers,
       then the audio is mixed, then finished downloads deliver their data */
    { extern void ns_tick_all(Player *p, int avm2, double dt_ms); extern void ns_process_loads(Player *p, int avm2);
      ns_tick_all(p, 1, vm->frame_ms);
      if (p->mix_hook) p->mix_hook(p);
      ns_process_loads(p, 1); }
    drag_update(vm);
    vm->in_enterframe = 1;   /* timeline advance: placements wait for the construct phase */
}
/* the test harness ends each frame with a timer update (Ruffle: run_frame, then
   update_timers), so timers due during the last frame still fire */
void avm2_final_timers(Player *p) {
    VM2 *vm = p ? (VM2*)p->avm2 : NULL;
    if (!vm || p->stopped) return;
    vm->time_ms += vm->frame_ms;
    a2_timers_tick(vm, vm->frame_ms);
    if (vm->throwing) a2_report_uncaught(vm);
    /* and loads that received their data complete (Ruffle's executor runs after the tick) */
    { extern void a2_loader_tick(VM2 *vm); vm->frame_no++; a2_loader_tick(vm); if (vm->throwing) a2_report_uncaught(vm); }
}
void avm2_tick_entered(Player *p) {
    VM2 *vm = (VM2*)p->avm2;
    if (!vm) return;
    vm->in_enterframe = 0;
    a2_broadcast(vm, vm->s_enterFrame);
    if (vm->throwing) a2_report_uncaught(vm);
}
/* the renderer applies the lazy autoSize bounds of every visible text field */
static void apply_text_bounds(Player *p, MovieClip *c) {
    DisplayObject *d;
    if (!c) return;
    for (d = c->children; d; d = d->next) {
        A2Obj *o; A2Disp *st;
        if (!d->visible || !d->clip) continue;
        o = (A2Obj*)d->clip->obj2; st = o ? (A2Disp*)o->payload : NULL;
        if (st && st->is_text && st->tfs) tfs_apply_autosize(p, st->tfs);
        apply_text_bounds(p, d->clip);
    }
}
/* the end of a frame, after the AVM1 frame: loads move along */
void avm2_net_phase(Player *p) {
    VM2 *vm = p ? (VM2*)p->avm2 : NULL; extern void a2_net_phase(VM2 *vm);
    if (!vm || p->stopped) return;
    a2_net_phase(vm);
    if (vm->throwing) a2_report_uncaught(vm);
}
void avm2_tick_end(Player *p) {
    VM2 *vm = (VM2*)p->avm2; int guard = 0;
    if (!vm) return;
    if (g_npend_sym) avm2_report_missing_symbols(vm);
    { extern void a2_loader_construct(VM2 *vm); a2_loader_construct(vm); }
    construct_pending(vm);
    vm->in_fc = 1; vm->nfc_reenter = 0;
    a2_broadcast(vm, vm->s_frameConstructed);
    vm->in_fc = 0;
    {   int k, nr = vm->nfc_reenter; A2Obj *re[16]; memcpy(re, vm->fc_reenter, (size_t)nr * sizeof(A2Obj*)); vm->nfc_reenter = 0;
        for (k = 0; k < nr; k++) { a2_dispatch_simple(vm, re[k], vm->s_enterFrame, 0); if (vm->throwing) a2_report_uncaught(vm); } }
    vm->fs_phase = 1;
    while ((any_frame_changed(p->root) || orphans_frame_changed(vm) || vm->nfsq) && guard++ < 64) { orphans_frame_scripts(vm); run_pending_frame_scripts(vm, p->root); run_fs_cleanup(vm); construct_pending(vm); }
    vm->fs_phase = 0;
    a2_broadcast(vm, vm->s_exitFrame);
    orphans_cleanup(vm);
    { extern void a2_stage3d_frame(VM2 *vm); a2_stage3d_frame(vm); }   /* context3DCreate */
    /* after the first frame: the root LoaderInfo fires init, then complete */
    if (!a2_dyn_find(vm->global, a2_str(vm, "__rootInitSent"))) {
        extern V2 a2_root_loaderinfo(VM2 *vm); V2 li = a2_root_loaderinfo(vm);
        a2_dyn_set_hidden(vm, vm->global, a2_str(vm, "__rootInitSent"), v2_bool(1));
        if (li.t == T_OBJ) { extern void a2_root_progress(VM2 *vm, A2Obj *li); a2_dispatch_simple(vm, li.u.o, a2_str(vm, "init"), 0); if (vm->throwing) a2_report_uncaught(vm); a2_root_progress(vm, li.u.o); if (vm->throwing) a2_report_uncaught(vm); a2_dispatch_simple(vm, li.u.o, a2_str(vm, "complete"), 0); if (vm->throwing) a2_report_uncaught(vm); }
    }
    { extern void a2_localconn_frame(VM2 *vm); a2_localconn_frame(vm); }
    { extern void a2_netconn_frame(VM2 *vm); a2_netconn_frame(vm); }
    { extern void a2_sound_frame(VM2 *vm); a2_sound_frame(vm); }
    { extern void a2_loader_exit_frame(VM2 *vm); a2_loader_exit_frame(vm); }
    { extern void a2_shader_frame(VM2 *vm); a2_shader_frame(vm); }
    { extern void a2_stage3d_tick(VM2 *vm); a2_stage3d_tick(vm); }
    { extern void a2_sockets_update(VM2 *vm); extern void a2_fileref_run(VM2 *vm); a2_fileref_run(vm); a2_sockets_update(vm); }
    { A2Prop *rp = a2_dyn_find(vm->global, a2_str(vm, "__renderPending")); if (rp && v2_tobool(vm, rp->v)) { a2_dyn_set(vm, vm->global, a2_str(vm, "__renderPending"), v2_bool(0)); a2_broadcast(vm, a2_str(vm, "render")); } }
    apply_text_bounds(p, p->root);   /* rendering applies lazy autoSize bounds, even when no frame is drawn */
    player_flush_graveyard(p);
    { extern size_t g_a2_ext_bytes; if (vm->gc_count > vm->gc_threshold || g_a2_ext_bytes > 160u * 1024 * 1024) a2_gc(vm); }
}
void avm2_resized(Player *p) { VM2 *vm = (VM2*)p->avm2; if (vm && vm->stage_obj) a2_dispatch_simple(vm, vm->stage_obj, vm->s_resize, 0); }

/* ---- mouse ---- */

static A2Obj *hit_target(VM2 *vm, A2Obj *cont, const SWFMatrix *pw, double sx, double sy, int *found_any) {
    /* Flash rule: interactive descendants win over a container's own
     * non-interactive content regardless of depth; non-interactive hits make
     * the (mouseEnabled) container itself the target. */
    MovieClip *c = container_clip(vm, cont); DisplayObject *d; DisplayObject *kids[2048]; int n = 0, i; A2Disp *st = a2_disp(cont); int self_hit = 0;
    if (!c) return NULL;
    unsigned char outside[2048];
    for (d = c->children; d && n < 2048; d = d->next) kids[n++] = d;
    {   /* timeline mask layers: content under a clip layer is hit only inside the mask */
        DisplayObject *md = NULL; int mhit = 0, j;
        for (j = 0; j < n; j++) {
            d = kids[j]; outside[j] = 0;
            if (d->clip_depth > 0) { md = d; mhit = clip_dobj_hit_test(vm->player->avm, d, pw, sx, sy); continue; }
            if (md && d->depth > md->depth && d->depth <= md->clip_depth && !mhit) outside[j] = 1;
        }
    }
    for (i = n - 1; i >= 0; i--) {
        A2Obj *co; SWFMatrix w; int interactive;
        d = kids[i];
        if (!d->visible || d->clip_depth > 0 || outside[i]) continue;
        if (d->clip && CLIP_A1MIX(d->clip) && !d->clip->obj2) {   /* an AVM1 _level inside a Loader: no AVM2 object */
            extern MovieClip *avm1_pick_in(void *vm, MovieClip *c, const SWFMatrix *w, double sx, double sy); extern int avm1_hit_shape_in(void *vm, MovieClip *c, const SWFMatrix *w, double sx, double sy);
            MovieClip *a1;
            if (!dobj_enter(d, pw, sx, sy, &w)) continue;
            a1 = st->mouse_children ? avm1_pick_in(vm->player->avm, d->clip, &w, sx, sy) : NULL;
            if (a1 && a1 != d->clip) { vm->a1_pick = a1; *found_any = 1; return cont; }
            if (avm1_hit_shape_in(vm->player->avm, d->clip, &w, sx, sy)) self_hit = 1;
            continue;
        }
        co = a2_display_obj(vm, d->clip, d);
        if (a2_disp(co)->is_mask) continue;
        if (a2_disp(co)->mask_obj) {   /* a script mask: hit only inside it */
            A2Obj *mo = a2_disp(co)->mask_obj, *mp = a2_parent_of(vm, mo); DisplayObject *mdo = a2_dobj_of(mo); SWFMatrix mpw;
            if (mp) a2_world_matrix(mp, &mpw); else swf_matrix_identity(&mpw);
            if (mdo && !clip_dobj_hit_test(vm->player->avm, mdo, &mpw, sx, sy)) continue;
        }
        if (!dobj_enter(d, pw, sx, sy, &w)) continue;
        if (d->clip && CLIP_A1MIX(d->clip)) {   /* AVM1Movie: AVM1 picking inside it (Ruffle LoaderDisplay::mouse_pick_avm2) */
            extern MovieClip *avm1_pick_in(void *vm, MovieClip *c, const SWFMatrix *w, double sx, double sy);
            MovieClip *a1 = st->mouse_children ? avm1_pick_in(vm->player->avm, d->clip, &w, sx, sy) : NULL;
            if (a1 && a1 != d->clip) { vm->a1_pick = a1; *found_any = 1; return co; }
            { extern int avm1_hit_shape_in(void *vm, MovieClip *c, const SWFMatrix *w, double sx, double sy); if (avm1_hit_shape_in(vm->player->avm, d->clip, &w, sx, sy)) self_hit = 1; }
            continue;
        }
        interactive = co->cls && a2_is_subclass(co->cls, vm->c_interactive);
        if (co->cls && a2_is_subclass(co->cls, vm->c_container)) {
            int sub_any = 0; A2Obj *r;
            if (!st->mouse_children) { if (clip_dobj_hit_test(vm->player->avm, d, pw, sx, sy)) self_hit = 1; continue; }
            r = hit_target(vm, co, &w, sx, sy, &sub_any);
            if (r) { *found_any = 1; return r; }
            if (sub_any) { self_hit = 1; continue; }
            continue;
        }
        if (clip_dobj_hit_test(vm->player->avm, d, pw, sx, sy)) {
            if (interactive && st->mouse_children) { if (a2_disp(co)->mouse_enabled) { *found_any = 1; return co; } self_hit = 1; continue; }
            self_hit = 1;
        }
    }
    /* own drawing (Sprite.graphics) */
    if (!self_hit && a2_disp(cont)->drawing && cont != vm->stage_obj) {
        DisplayObject *sd = a2_dobj_of(cont);
        if (sd) { SWFMatrix saved = sd->matrix; swf_matrix_identity(&sd->matrix); if (clip_dobj_hit_test(vm->player->avm, sd, pw, sx, sy)) self_hit = 1; sd->matrix = saved; }
    }
    if (self_hit) { *found_any = 1; if (st->mouse_enabled && cont != vm->stage_obj && !(st->is_movie_root && !st->is_root)) return cont; }   /* a loaded movie's root passes the hit to its Loader */
    return NULL;
}
static void mouse_event(VM2 *vm, A2Obj *target, A2Str *type, int bubbles, A2Obj *related) {
    A2Obj *e; SWFMatrix w; double a, b, c, d, ee, f, sx, sy, lx, ly;
    if (!target) return;
    /* a selectable field that was static text in the authoring tool gets no mouse events
       at all (nor do its ancestors or the stage) */
    if (target->payload && a2_disp(target)->is_text && a2_disp(target)->tfs && a2_disp(target)->tfs->was_static && a2_disp(target)->tfs->selectable) return;
    e = a2_make_event(vm, vm->c_mouseevent, type, bubbles, 0);
    a2_world_matrix(target, &w);
    sx = vm->mouse_x * 20.0 / vm->player->zoom; sy = vm->mouse_y * 20.0 / vm->player->zoom;
    if (matrix_invert(&w, &a, &b, &c, &d, &ee, &f)) { lx = (a * sx + b * sy + ee) / 20; ly = (c * sx + d * sy + f) / 20; } else { lx = sx / 20; ly = sy / 20; }
    a2_dyn_set(vm, e, a2_str(vm, "localX"), v2_num(lx)); a2_dyn_set(vm, e, a2_str(vm, "localY"), v2_num(ly));
    a2_dyn_set(vm, e, a2_str(vm, "buttonDown"), v2_bool(vm->player->mouse_down || (vm->button_down_mask != 0))); a2_dyn_set(vm, e, a2_str(vm, "relatedObject"), related ? v2_obj(related) : v2_null());
    a2_dyn_set(vm, e, a2_str(vm, "ctrlKey"), v2_bool(0)); a2_dyn_set(vm, e, a2_str(vm, "altKey"), v2_bool(0)); a2_dyn_set(vm, e, a2_str(vm, "shiftKey"), v2_bool(0)); a2_dyn_set(vm, e, a2_str(vm, "delta"), v2_int(type == vm->s_mouseWheel ? vm->wheel_delta : 0));
    a2_dispatch_event_obj(vm, target, e);
}
static int to_local(A2Obj *o, double sx, double sy, double *lx, double *ly) {
    SWFMatrix w; double a, b, c, d, e, f;
    a2_world_matrix(o, &w);
    if (!matrix_invert(&w, &a, &b, &c, &d, &e, &f)) return 0;
    *lx = a * sx + b * sy + e; *ly = c * sx + d * sy + f; return 1;
}
/* a host viewport (device pixels) with the noScale mode: device -> stage pixels
   (the stage keeps its size in points, scaled by the display's scale factor) */
static void view_to_stage(VM2 *vm, double *x, double *y) {
    Player *p = vm->player; V2 sm, al; double sf, mw, mh, wd, hd; const char *a;
    if (!p->has_view || !vm->stage_obj || !p->movie) return;
    sm = st_prop_get(vm, v2_obj(vm->stage_obj), 0, NULL, "__scaleMode", v2_null());
    if (sm.t != T_STR || strcmp(sm.u.s->s, "noScale")) return;
    sf = p->view_sf > 0 ? p->view_sf : 1;
    mw = floor((p->movie->stage.xmax - p->movie->stage.xmin) / 20.0); mh = floor((p->movie->stage.ymax - p->movie->stage.ymin) / 20.0);
    wd = p->view_w - mw * sf; hd = p->view_h - mh * sf;
    al = st_prop_get(vm, v2_obj(vm->stage_obj), 0, NULL, "__align", v2_str(vm->s_empty)); a = al.t == T_STR ? al.u.s->s : "";
    *x = (*x - (strchr(a, 'L') ? 0 : strchr(a, 'R') ? wd : wd / 2)) / sf;
    *y = (*y - (strchr(a, 'T') ? 0 : strchr(a, 'B') ? hd : hd / 2)) / sf;
}
/* the stage scale mode (0 showAll 1 exactFit 2 noBorder 3 noScale) and align bits (1 L 2 T 4 R 8 B) for the renderer */
void a2_stage_view_mode(Player *p, int *mode, int *align) {
    VM2 *vm = (VM2*)p->avm2; V2 sm, al; const char *a, *m;
    *mode = 0; *align = 0;
    if (!vm || !vm->stage_obj) return;
    sm = st_prop_get(vm, v2_obj(vm->stage_obj), 0, NULL, "__scaleMode", v2_null());
    m = sm.t == T_STR ? sm.u.s->s : "showAll";
    *mode = !strcmp(m, "exactFit") ? 1 : !strcmp(m, "noBorder") ? 2 : !strcmp(m, "noScale") ? 3 : 0;
    al = st_prop_get(vm, v2_obj(vm->stage_obj), 0, NULL, "__align", v2_str(vm->s_empty)); a = al.t == T_STR ? al.u.s->s : "";
    *align = (strchr(a, 'L') ? 1 : 0) | (strchr(a, 'T') ? 2 : 0) | (strchr(a, 'R') ? 4 : 0) | (strchr(a, 'B') ? 8 : 0);
}
/* button clip events (Ruffle): SimpleButton states follow them
   (Avm2Button::event_dispatch) and a MovieClip in buttonMode goes to its
   _up / _over / _down frame (MovieClip::event_dispatch) */
enum { BE_ROLLOUT, BE_ROLLOVER, BE_PRESS, BE_RELEASE, BE_RELEASE_OUTSIDE, BE_UP_INSIDE, BE_DRAGOUT, BE_DRAGOVER };
static void button_event(VM2 *vm, A2Obj *o, int ev) {
    static const int sb_state[8] = { 0, 1, 2, 1, 0, 0, 1, 2 };
    static const char *const mc_label[8] = { "_up", "_over", "_down", "_over", "_up", NULL, "_over", "_down" };
    if (!o || !o->cls) return;
    if (vm->c_simplebutton && a2_is_subclass(o->cls, vm->c_simplebutton)) {
        DisplayObject *d = a2_dobj_of(o);
        if (d) d->btn_state = sb_state[ev];
        return;
    }
    if (mc_label[ev] && a2_disp(o)->button_mode) {
        MovieClip *c = clip_of(o); int k, f = -1;
        if (!c || !c->tl) return;
        for (k = 0; k < scene_count(vm, c) && f < 0; k++) f = label_frame_in_scene(vm, c, a2_str(vm, mc_label[ev]), k);
        if (f >= 0) request_goto(vm, c, f, 0);
    }
}
static void avm2_mouse_in(Player *p, int x, int y, int down, int changed) {
    VM2 *vm = (VM2*)p->avm2; A2Obj *t; SWFMatrix id; int any = 0, a1; double sx, sy;
    if (!vm || !vm->stage_obj) return;
    vm->mouse_x = x + p->mouse_fx; vm->mouse_y = y + p->mouse_fy;
    view_to_stage(vm, &vm->mouse_x, &vm->mouse_y);
    swf_matrix_identity(&id);
    sx = vm->mouse_x * 20.0 / p->zoom; sy = vm->mouse_y * 20.0 / p->zoom;
    vm->a1_pick = NULL;
    t = hit_target(vm, vm->stage_obj, &id, sx, sy, &any);
    if (!t) t = vm->stage_obj;
    a1 = vm->a1_pick != NULL;   /* over an AVM1 button or clip: the AVM1 side handles the mouse */
    if (!changed && !a1) mouse_event(vm, t, vm->s_mouseMove, 1, NULL);
    {   /* hover: nothing over the empty stage (run_mouse_pick gives None) */
        A2Obj *nh = (t == vm->stage_obj || a1) ? NULL : t;
        if (nh != vm->hover) {
            A2Obj *old = vm->hover;
            if (old) button_event(vm, old, p->mouse_down && vm->pressed == old ? BE_DRAGOUT : BE_ROLLOUT);
            if (nh) button_event(vm, nh, p->mouse_down && vm->pressed == nh ? BE_DRAGOVER : BE_ROLLOVER);
            if (old) { mouse_event(vm, old, vm->s_mouseOut, 1, nh); /* rollOut up the chain not containing the new object */ { A2Obj *q = old; while (q && q != vm->stage_obj) { A2Obj *r = nh; int inside = 0; while (r) { if (r == q) { inside = 1; break; } r = a2_parent_of(vm, r); } if (inside) break; mouse_event(vm, q, vm->s_rollOut, 0, nh); q = a2_parent_of(vm, q); } } }
            vm->hover = nh;
            if (nh) {
                { A2Obj *q = nh; while (q && q != vm->stage_obj) { A2Obj *r = old; int inside = 0; while (r) { if (r == q) { inside = 1; break; } r = a2_parent_of(vm, r); } if (inside) break; mouse_event(vm, q, vm->s_rollOver, 0, old); q = a2_parent_of(vm, q); } }
                mouse_event(vm, nh, vm->s_mouseOver, 1, old);
            }
        }
    }
    if (!changed) {
        drag_update(vm);
        /* drag-selecting in the field that took the press */
        if (p->mouse_down && vm->pressed && vm->pressed == vm->focus && a2_disp(vm->pressed)->tfs) { double lx, ly; if (to_local(vm->pressed, sx, sy, &lx, &ly)) { extern void a2_text_mouse(VM2 *vm, A2Obj *o, double x, double y, int down, int drag); a2_text_mouse(vm, vm->pressed, lx, ly, 1, 1); } }
        return;
    }
    if (a1) {
        if (!down && vm->pressed) mouse_event(vm, vm->pressed, a2_str(vm, "releaseOutside"), 1, NULL);
        vm->pressed = NULL;
        if (down) { extern void a2_focus_mouse_press(VM2 *vm, A2Obj *t); extern int g_a1_press_took_focus; if (!g_a1_press_took_focus && (vm->focus || fo_a1_focus(vm))) a2_focus_mouse_press(vm, NULL); }
    } else if (down) {
        vm->pressed = t;
        button_event(vm, t, BE_PRESS);
        /* focus follows the press (FocusTracker::set_by_mouse), before mouseDown */
        { extern void a2_focus_mouse_press(VM2 *vm, A2Obj *t); a2_focus_mouse_press(vm, t == vm->stage_obj ? NULL : t); }
        mouse_event(vm, t, vm->s_mouseDown, 1, NULL);
        if (t != vm->stage_obj && a2_disp(t)->is_text && a2_disp(t)->tfs && vm->focus == t) {
            double lx, ly;
            if (to_local(t, sx, sy, &lx, &ly)) { extern void a2_text_mouse(VM2 *vm, A2Obj *o, double x, double y, int down, int drag); a2_text_mouse(vm, t, lx, ly, 1, 0); }
        }
        if (t != vm->stage_obj && a2_disp(t)->is_text && a2_disp(t)->tfs) {
            double lx, ly;
            if (to_local(t, sx, sy, &lx, &ly)) { extern void a2_text_press_link(VM2 *vm, A2Obj *o, double x, double y); a2_text_press_link(vm, t, lx, ly); }
        }
    } else {
        if (vm->pressed == t) button_event(vm, t, BE_RELEASE);
        else { button_event(vm, t, BE_UP_INSIDE); if (vm->pressed) button_event(vm, vm->pressed, BE_RELEASE_OUTSIDE); }
        mouse_event(vm, t, vm->s_mouseUp, 1, NULL);
        if (vm->pressed == t) mouse_event(vm, t, (vm->click_index & 1) && a2_disp(t)->double_click ? a2_str(vm, "doubleClick") : vm->s_click, 1, NULL);
        else if (vm->pressed) mouse_event(vm, vm->pressed, a2_str(vm, "releaseOutside"), 1, NULL);
        vm->pressed = NULL;
    }
    if (vm->throwing) a2_report_uncaught(vm);
}
/* errors thrown by mouse event listeners do not reach the trace log */
void avm2_mouse(Player *p, int x, int y, int down, int changed) {
    VM2 *vm = (VM2*)p->avm2;
    if (vm) vm->quiet_uncaught++;
    avm2_mouse_in(p, x, y, down, changed);
    if (vm) vm->quiet_uncaught--;
}

/* the AVM1 object under a stage point (twips), through the AVM2 display list (NULL: none) */
MovieClip *avm2_pick_avm1(Player *p, double sx, double sy) {
    VM2 *vm = (VM2*)p->avm2; SWFMatrix id; int any = 0;
    if (!vm || !vm->stage_obj) return NULL;
    swf_matrix_identity(&id);
    vm->a1_pick = NULL;
    hit_target(vm, vm->stage_obj, &id, sx, sy, &any);
    return vm->a1_pick;
}
void avm2_click_index(Player *p, int index) { VM2 *vm = (VM2*)p->avm2; if (vm) vm->click_index = index; }
/* mouse wheel: mouseWheel to the object under the pointer, then text fields scroll */
void avm2_mouse_wheel(Player *p, int delta) {
    VM2 *vm = (VM2*)p->avm2; A2Obj *t; SWFMatrix id; int any = 0;
    if (!vm || !vm->stage_obj) return;
    swf_matrix_identity(&id);
    t = hit_target(vm, vm->stage_obj, &id, vm->mouse_x * 20.0 / p->zoom, vm->mouse_y * 20.0 / p->zoom, &any);
    if (!t) t = vm->stage_obj;
    vm->wheel_delta = delta;
    mouse_event(vm, t, vm->s_mouseWheel, 1, NULL);
    if (vm->throwing) a2_report_uncaught(vm);
    if (t != vm->stage_obj && a2_disp(t)->tfs) { extern void a2_text_wheel(VM2 *vm, A2Obj *o, int delta); a2_text_wheel(vm, t, delta); }
}
/* middle (1) / right (2) buttons: Flash Player 11.2+ events (no hover/focus changes) */
void avm2_mouse_button(Player *p, int x, int y, int button, int down) {
    VM2 *vm = (VM2*)p->avm2; A2Obj *t; SWFMatrix id; int any = 0; double sx, sy; A2Obj **pressed;
    const char *dn, *up, *clk;
    if (!vm || !vm->stage_obj) return;
    if (button == 1) { dn = "middleMouseDown"; up = "middleMouseUp"; clk = "middleClick"; pressed = &vm->pressed_mid; }
    else { dn = "rightMouseDown"; up = "rightMouseUp"; clk = "rightClick"; pressed = &vm->pressed_right; }
    vm->mouse_x = x + p->mouse_fx; vm->mouse_y = y + p->mouse_fy;
    view_to_stage(vm, &vm->mouse_x, &vm->mouse_y);
    swf_matrix_identity(&id);
    sx = vm->mouse_x * 20.0 / p->zoom; sy = vm->mouse_y * 20.0 / p->zoom;
    t = hit_target(vm, vm->stage_obj, &id, sx, sy, &any);
    if (!t) t = vm->stage_obj;
    if (down) { *pressed = t; vm->button_down_mask |= (button == 1 ? 2 : 4); mouse_event(vm, t, a2_str(vm, dn), 1, NULL); }
    else {
        vm->button_down_mask &= ~(button == 1 ? 2 : 4);
        mouse_event(vm, t, a2_str(vm, up), 1, NULL);
        if (*pressed == t) mouse_event(vm, t, a2_str(vm, clk), 1, NULL);
        *pressed = NULL;
    }
    if (vm->throwing) a2_report_uncaught(vm);
}

/* ---- keyboard ---- */

/* charCode when the host does not say: a US keyboard layout */
static int key_char(int vk, int shift) {
    static const char pn[] = ";=,-./`", ps[] = ":+<_>?~", bn[] = "[\\]'", bs[] = "{|}\"";
    if (vk >= 'A' && vk <= 'Z') return shift ? vk : vk + 32;
    if (vk >= '0' && vk <= '9') return shift ? ")!@#$%^&*("[vk - '0'] : vk;
    if (vk >= 96 && vk <= 105) return '0' + vk - 96;   /* numpad digits */
    if (vk >= 106 && vk <= 111 && vk != 108) return "*+ -./"[vk - 106];
    if (vk >= 186 && vk <= 192) return shift ? ps[vk - 186] : pn[vk - 186];
    if (vk >= 219 && vk <= 222) return shift ? bs[vk - 219] : bn[vk - 219];
    if (vk == 32 || vk == 13 || vk == 8 || vk == 9 || vk == 27) return vk;
    if (vk == 46) return 127;
    return 0;
}
/* set by a host that sends editing commands itself (avm2_text_control): keys then only make events */
int a2_external_text_control;
void avm2_key(Player *p, int keycode, int down) {
    VM2 *vm = (VM2*)p->avm2; A2Obj *e, *target;
    if (!vm || !vm->stage_obj) return;
    target = vm->focus ? vm->focus : vm->stage_obj;
    e = a2_make_event(vm, vm->c_keyboardevent, down ? vm->s_keyDown : vm->s_keyUp, 1, 1);
    a2_dyn_set(vm, e, a2_str(vm, "keyCode"), v2_uint((u32)keycode)); a2_dyn_set(vm, e, a2_str(vm, "charCode"), v2_uint((u32)(p->key_ev_char >= 0 ? p->key_ev_char : key_char(keycode, p->key_down[16])))); a2_dyn_set(vm, e, a2_str(vm, "keyLocation"), v2_uint((u32)p->key_ev_loc));
    a2_dyn_set(vm, e, a2_str(vm, "ctrlKey"), v2_bool(p->key_down[17])); a2_dyn_set(vm, e, a2_str(vm, "altKey"), v2_bool(p->key_down[18])); a2_dyn_set(vm, e, a2_str(vm, "shiftKey"), v2_bool(p->key_down[16]));
    a2_dispatch_event_obj(vm, target, e);
    if (vm->throwing) a2_report_uncaught(vm);
    if (down && keycode == 9) { extern void a2_focus_cycle(VM2 *vm, int reverse); a2_focus_cycle(vm, p->key_down[16]); if (vm->throwing) a2_report_uncaught(vm); return; }
    if (down && vm->focus_hl == 2 && vm->focus && keycode >= 37 && keycode <= 40) { extern void a2_focus_navigate(VM2 *vm, int dir); a2_focus_navigate(vm, keycode); if (vm->throwing) a2_report_uncaught(vm); }
    if (down && !a2_external_text_control && vm->focus && vm->focus == target && a2_disp(vm->focus)->tfs) { extern int a2_text_key(VM2 *vm, A2Obj *o, int vk, int shift, int ctrl); a2_text_key(vm, vm->focus, keycode, p->key_down[16], p->key_down[17]); }
}
/* clipboard / selection commands from the host (menu or test input) */
void avm2_text_control(Player *p, const char *name) {
    VM2 *vm = (VM2*)p->avm2; int vk = 0; extern int a2_text_key(VM2 *vm, A2Obj *o, int vk, int shift, int ctrl);
    if (!vm || !vm->focus || !a2_disp(vm->focus)->tfs) return;
    if (!strcmp(name, "Paste")) vk = 'V'; else if (!strcmp(name, "Copy")) vk = 'C'; else if (!strcmp(name, "Cut")) vk = 'X'; else if (!strcmp(name, "SelectAll")) vk = 'A';
    if (vk) { a2_text_key(vm, vm->focus, vk, 0, 1); return; }
    {   /* caret movement / selection / deletion commands */
        static const struct { const char *n; int vk, shift, ctrl; } m[] = {
            { "MoveLeft", 37, 0, 0 }, { "MoveLeftWord", 37, 0, 1 }, { "MoveLeftLine", 36, 0, 0 }, { "MoveLeftDocument", 36, 0, 1 },
            { "MoveRight", 39, 0, 0 }, { "MoveRightWord", 39, 0, 1 }, { "MoveRightLine", 35, 0, 0 }, { "MoveRightDocument", 35, 0, 1 },
            { "SelectLeft", 37, 1, 0 }, { "SelectLeftWord", 37, 1, 1 }, { "SelectLeftLine", 36, 1, 0 }, { "SelectLeftDocument", 36, 1, 1 },
            { "SelectRight", 39, 1, 0 }, { "SelectRightWord", 39, 1, 1 }, { "SelectRightLine", 35, 1, 0 }, { "SelectRightDocument", 35, 1, 1 },
            { "Delete", 46, 0, 0 }, { "DeleteWord", 46, 0, 1 }, { NULL, 0, 0, 0 } };
        int i;
        if (!strcmp(name, "Backspace")) { extern void a2_text_input_char(VM2 *vm, A2Obj *t, int ch); a2_text_input_char(vm, vm->focus, 8); return; }
        if (!strcmp(name, "Enter")) { extern void a2_text_input_char(VM2 *vm, A2Obj *t, int ch); a2_text_input_char(vm, vm->focus, 13); return; }
        for (i = 0; m[i].n; i++) if (!strcmp(name, m[i].n)) { a2_text_key(vm, vm->focus, m[i].vk, m[i].shift, m[i].ctrl); break; }
    }
    if (vm->throwing) a2_report_uncaught(vm);
}
void avm2_ime(Player *p, const char *utf8, int from, int to, int commit) {
    VM2 *vm = (VM2*)p->avm2; extern void a2_text_ime_preedit(VM2 *vm, A2Obj *o, const char *u8, int from, int to); extern void a2_text_ime_commit(VM2 *vm, A2Obj *o, const char *u8);
    if (!vm || !vm->focus || !a2_disp(vm->focus)->is_text || !a2_disp(vm->focus)->tfs) return;
    if (commit) a2_text_ime_commit(vm, vm->focus, utf8); else a2_text_ime_preedit(vm, vm->focus, utf8, from, to);
    if (vm->throwing) a2_report_uncaught(vm);
}
int avm2_handles_char(Player *p) { VM2 *vm = (VM2*)p->avm2; return vm && vm->focus && a2_disp(vm->focus)->is_text && a2_disp(vm->focus)->tfs; }
void avm2_char(Player *p, int ch) {
    VM2 *vm = (VM2*)p->avm2; A2Obj *t; A2Disp *st; extern void a2_text_input_char(VM2 *vm, A2Obj *t, int ch);
    if (!vm || !vm->focus) return;
    t = vm->focus; st = a2_disp(t);
    if (!st->is_text || !st->tfs) return;
    if (a2_external_text_control && (ch < 32 || ch == 127)) return;   /* the host sends Backspace/Enter as editing commands */
    a2_text_input_char(vm, t, ch);
}

/* does this clip's AVM2 object have MovieClip behaviour (a timeline that plays)? */
int a2_clip_is_movieclip(MovieClip *c) {
    VM2 *vm = (VM2*)c->player->avm2; A2Obj *o = (A2Obj*)c->obj2;
    if (!vm || !o || !o->cls || !vm->c_movieclip) return 1;
    return a2_is_subclass(o->cls, vm->c_movieclip);
}

/* generic flash lib init */
void a2_flash_init(VM2 *vm) {
    a2_flash_events_init(vm);
    a2_flash_display_init(vm);
    a2_flash_text_init(vm);
    a2_flash_misc_init(vm);
    /* classes whose constructor throws #2012 (the player creates their instances itself) */
    { static const char *const ni[] = { "flash.display::AVM1Movie", "flash.display::DisplayObject", "flash.display::DisplayObjectContainer", "flash.display::Graphics",
        "flash.display::InteractiveObject", "flash.display::LoaderInfo", "flash.display::MorphShape", "flash.display::Stage", "flash.display::Stage3D",
        "flash.accessibility::Accessibility", "flash.system::Capabilities", "flash.display3D::Context3D", "flash.display3D.textures::CubeTexture",
        "flash.net.drm::DRMManager", "flash.net.drm::DRMPlaybackTimeWindow", "flash.net.drm::DRMVoucher", "flash.external::ExternalInterface",
        "flash.ui::GameInputControl", "flash.system::IME", "flash.display3D::IndexBuffer3D", "JSON", "flash.ui::Keyboard", "flash.system::MessageChannel",
        "flash.ui::Mouse", "flash.ui::Multitouch", "flash.net::ObjectEncoding", "flash.display3D::Program3D", "flash.display3D.textures::RectangleTexture",
        "flash.system::Security", "flash.net::SharedObject", "flash.media::StageVideo", "flash.text::StaticText", "flash.system::System",
        "flash.text.engine::TextLineMirrorRegion", "flash.text::TextSnapshot", "flash.display3D.textures::Texture",
        "flash.geom::Utils3D", "flash.display3D::VertexBuffer3D", "flash.system::Worker", "flash.system::WorkerDomain", "flash.filters::BitmapFilter",
        "flash.display3D.textures::TextureBase", NULL };
      static const char *const ni_plain[] = { "flash.system::SecurityDomain", NULL };
      int i; A2Class *c;
      for (i = 0; ni[i]; i++) if ((c = a2_class_find_s(vm, ni[i])) != NULL) c->flags |= CF_NOINST;
      for (i = 0; ni_plain[i]; i++) if ((c = a2_class_find_s(vm, ni_plain[i])) != NULL) c->flags |= CF_NOINST | CF_NOINST_PLAIN; }
    /* interfaces the player's native classes implement (`is` / `as` / describeType) */
    { static const char *const im[][2] = { { "flash.utils::ByteArray", "flash.utils::IDataInput" }, { "flash.utils::ByteArray", "flash.utils::IDataOutput" },
        { "flash.net::Socket", "flash.utils::IDataInput" }, { "flash.net::Socket", "flash.utils::IDataOutput" }, { "flash.net::URLStream", "flash.utils::IDataInput" },
        { "flash.events::EventDispatcher", "flash.events::IEventDispatcher" }, { "flash.display::DisplayObject", "flash.display::IBitmapDrawable" },
        { "flash.display::BitmapData", "flash.display::IBitmapDrawable" }, { NULL, NULL } };
      int i, k; A2Class *c, *ic;
      for (i = 0; im[i][0]; i++) {
          if (!(c = a2_class_find_s(vm, im[i][0])) || c->inst || !(ic = a2_class_find_s(vm, im[i][1]))) continue;
          for (k = 0; k < c->nifaces; k++) if (c->ifaces[k] == ic) break;
          if (k < c->nifaces) continue;
          c->ifaces = (A2Class**)realloc(c->ifaces, (size_t)(c->nifaces + 1) * sizeof(A2Class*)); c->ifaces[c->nifaces++] = ic;
      } }
}

/* draw a script Bitmap (BitmapData-backed clip) */
void a2_render_obj_bitmap(Player *p, void *obj, const SWFMatrix *wm, const SWFCxform *cx);
void a2_render_clip_bitmap(Player *p, MovieClip *c, const SWFMatrix *wm, const SWFCxform *cx) { a2_render_obj_bitmap(p, c->obj2, wm, cx); }
/* a device coordinate (whole twips) rounded to whole pixels, halves away from zero */
static double snap_px(double v) { double t = floor(v * 20 + 0.5) / 20; return t < 0 ? -floor(-t + 0.5) : floor(t + 0.5); }
/* a Bitmap object's pixels (a timeline-placed bitmap symbol has no clip) */
void a2_render_obj_bitmap(Player *p, void *obj, const SWFMatrix *wm, const SWFCxform *cx) {
    extern Bitmap *a2_bitmapdata_bitmap(A2Obj *bd); A2Obj *o = (A2Obj*)obj; A2Disp *st = o ? (A2Disp*)o->payload : NULL; A2Obj *bd = st ? st->bitmap_data : NULL; Bitmap *bm; ShapeDef s; ShapeEdge ed[4]; FillStyle fs; int i; SWFMatrix snapped;
    if (!bd) return;
    bm = a2_bitmapdata_bitmap(bd);
    if (!bm || !bm->px) return;
    memset(&s, 0, sizeof(s)); memset(&fs, 0, sizeof(fs)); swf_matrix_identity(&fs.matrix);
    fs.type = (o && a2_disp(o)->smoothing) ? FILL_BITMAP_CLIP : FILL_BITMAP_CLIP_NS; fs.bitmap_id = -1; fs.matrix.sx = fs.matrix.sy = 20.0;
    {   /* pixelSnapping (Ruffle PixelSnapping::apply): the device translation rounded to whole
           pixels, always or (auto) when the bitmap is drawn unscaled and unrotated */
        int mode = st ? st->snapping : 0; double a = wm->sx * 20, d = wm->sy * 20;
        if (mode == 2 || (mode == 0 && a >= 0.999 && a <= 1.001 && d >= 0.999 && d <= 1.001 && wm->r0 == 0 && wm->r1 == 0)) {
            snapped = *wm;
            if (mode == 0) snapped.sx = snapped.sy = 1.0 / 20;
            snapped.tx = snap_px(wm->tx); snapped.ty = snap_px(wm->ty);
            wm = &snapped;
        }
    }
    render_set_direct_bitmap(bm);
    s.fills = &fs; s.nfills = 1; s.edges = ed; s.nedges = 4; s.version = 3;
    for (i = 0; i < 4; i++) { memset(&ed[i], 0, sizeof(ed[i])); ed[i].fill1 = 1; }
    ed[0].x0 = 0; ed[0].y0 = 0; ed[0].x1 = bm->w * 20; ed[0].y1 = 0;
    ed[1].x0 = bm->w * 20; ed[1].y0 = 0; ed[1].x1 = bm->w * 20; ed[1].y1 = bm->h * 20;
    ed[2].x0 = bm->w * 20; ed[2].y0 = bm->h * 20; ed[2].x1 = 0; ed[2].y1 = bm->h * 20;
    ed[3].x0 = 0; ed[3].y0 = bm->h * 20; ed[3].x1 = 0; ed[3].y1 = 0;
    render_shape(p->surf, p->movie, &s, wm, cx, NULL);
    render_set_direct_bitmap(NULL);
}

/* ---------------------------------------------------------------- */
/* focus tracker (Ruffle's focus_tracker.rs)                        */

static int fo_is_button(VM2 *vm, A2Obj *o) { return vm->c_simplebutton && o->cls && a2_is_subclass(o->cls, vm->c_simplebutton); }
static int fo_is_container(VM2 *vm, A2Obj *o) { return o->cls && a2_is_subclass(o->cls, vm->c_container); }
static int fo_is_interactive(VM2 *vm, A2Obj *o) { return o->kind == OK_DISPLAY && o->cls && a2_is_subclass(o->cls, vm->c_interactive); }
static int fo_editable(A2Obj *o) { A2Disp *st = a2_disp(o); return st->is_text && st->tfs && st->tfs->input; }
static int fo_tab_enabled(VM2 *vm, A2Obj *o) {
    A2Disp *st = a2_disp(o);
    if (st->tab_enabled_set) return st->tab_enabled;
    if (fo_is_button(vm, o)) return 1;
    if (st->is_text) return fo_editable(o);
    if (st->is_stage) return 0;
    return st->button_mode;
}
static int fo_tabbable(VM2 *vm, A2Obj *o) {
    A2Disp *st = a2_disp(o);
    if (st->is_text && !fo_editable(o)) return 0;
    if (st->is_root || st->is_stage) return 0;
    return fo_tab_enabled(vm, o);
}
static int fo_focusable_by_mouse(VM2 *vm, A2Obj *o) { return a2_disp(o)->is_text ? 1 : fo_tab_enabled(vm, o); }
static int fo_highlightable(VM2 *vm, A2Obj *o) {
    A2Disp *st = a2_disp(o);
    if (st->is_text || st->is_stage || st->is_root) return 0;
    return st->focus_rect_set ? st->focus_rect : !vm->stage_focus_rect_off;
}
/* highlight bounds: world bounds (a button: its hit area) */
extern void dg_world_bounds(MovieClip *c, SWFRect *out, int *any);
static int fo_bounds(VM2 *vm, A2Obj *o, SWFRect *out) {
    extern int dg_dobj_bounds(DisplayObject *d, const SWFMatrix *m, SWFRect *out);
    MovieClip *c = clip_of(o); int any = 0;
    out->xmin = out->ymin = out->xmax = out->ymax = 0;
    if (fo_is_button(vm, o)) {
        A2Obj *h = a2_disp(o)->hit; DisplayObject *hd = h ? a2_dobj_of(h) : NULL; SWFMatrix w, m;
        if (!hd) return 0;
        a2_world_matrix(o, &w); swf_matrix_mul(&m, &w, &hd->matrix);
        return dg_dobj_bounds(hd, &m, out);
    }
    if (c) { dg_world_bounds(c, out, &any); return any; }
    { DisplayObject *d = a2_dobj_of(o); SWFMatrix w; if (!d) return 0; a2_world_matrix(o, &w); return dg_dobj_bounds(d, &w, out); }
}
static int fo_calc_highlight(VM2 *vm) {
    SWFRect r;
    if (!vm->focus) return 0;
    if (!fo_highlightable(vm, vm->focus)) return 1;
    if (!fo_bounds(vm, vm->focus, &r) || r.xmin > r.xmax || r.ymin > r.ymax || (r.xmin == r.xmax && r.ymin == r.ymax)) return 1;
    return 2;
}

typedef struct { A2Obj **o; double *key; int n, cap, custom; } FoList;
static void fo_add(VM2 *vm, FoList *t, A2Obj *o) {
    int has = a2_disp(o)->tab_index_set;
    (void)vm;
    if (has && !t->custom) { int i, k = 0; t->custom = 1; for (i = 0; i < t->n; i++) if (a2_disp(t->o[i])->tab_index_set) t->o[k++] = t->o[i]; t->n = k; }
    if (!has && t->custom) return;
    if (t->n == t->cap) { int nc = t->cap ? t->cap * 2 : 32; A2Obj **no = (A2Obj**)realloc(t->o, (size_t)nc * sizeof(A2Obj*)); if (!no) return; t->o = no; t->cap = nc; }
    t->o[t->n++] = o;
}
static void fo_fill(VM2 *vm, A2Obj *p, FoList *t, int depth) {
    int i, n;
    if (depth > 64 || a2_disp(p)->no_tab_children) return;
    n = cc_count(vm, p);
    for (i = 0; i < n; i++) {
        A2Obj *co = cc_at(vm, p, i); DisplayObject *d;
        if (!co || co->kind != OK_DISPLAY) continue;
        d = a2_dobj_of(co);
        if (d && !d->visible) continue;
        if (fo_is_interactive(vm, co) && fo_tabbable(vm, co)) fo_add(vm, t, co);
        if (fo_is_container(vm, co)) fo_fill(vm, co, t, depth + 1);
    }
}
static void fo_order(VM2 *vm, FoList *t) {
    int i, j;
    memset(t, 0, sizeof(*t));
    if (!vm->stage_obj) return;
    fo_fill(vm, vm->stage_obj, t, 0);
    if (!t->n) return;
    t->key = (double*)malloc((size_t)t->n * sizeof(double));
    if (!t->key) { t->n = 0; return; }
    for (i = 0; i < t->n; i++) { if (t->custom) t->key[i] = a2_disp(t->o[i])->tab_index; else { SWFRect r; if (fo_bounds(vm, t->o[i], &r)) t->key[i] = (double)(long)r.ymin * 6 + (double)(long)r.xmin; else t->key[i] = 2147483647.0 * 7; } }   /* no bounds (Rectangle::INVALID) sort last */
    for (i = 1; i < t->n; i++) { A2Obj *x = t->o[i]; double kx = t->key[i]; j = i - 1; while (j >= 0 && t->key[j] > kx) { t->o[j + 1] = t->o[j]; t->key[j + 1] = t->key[j]; j--; } t->o[j + 1] = x; t->key[j + 1] = kx; }
    if (!t->custom) { int k = 0; for (i = 0; i < t->n; i++) if (i == 0 || t->key[i] != t->key[i - 1]) { t->o[k] = t->o[i]; t->key[k] = t->key[i]; k++; } t->n = k; }
}
static void fo_free(FoList *t) { free(t->o); free(t->key); }

static A2Obj *fo_event(VM2 *vm, const char *type, int cancelable, A2Obj *related, int keycode) {
    A2Class *fe = a2_class_find_s(vm, "flash.events::FocusEvent"); A2Obj *e;
    if (!fe) return NULL;
    e = a2_make_event(vm, fe, a2_str(vm, type), 1, cancelable);
    a2_dyn_set(vm, e, a2_str(vm, "relatedObject"), related ? v2_obj(related) : v2_null());
    a2_dyn_set(vm, e, a2_str(vm, "shiftKey"), v2_bool(0));
    a2_dyn_set(vm, e, a2_str(vm, "keyCode"), v2_uint((u32)keycode));
    return e;
}
/* keyFocusChange / mouseFocusChange: 1 when cancelled */
static int fo_change_event(VM2 *vm, const char *type, A2Obj *target, A2Obj *related, int keycode) {
    A2Obj *e = fo_event(vm, type, 1, related, keycode);
    if (!target) target = vm->stage_obj;
    if (!e || !target) return 0;
    a2_dispatch_event_obj(vm, target, e);
    return (e->ival & 4) != 0;   /* EV_PREVENTED */
}
/* AVM1Movie content shares the focus (Ruffle's FocusTracker): the AVM1 object holding it */
static int fo_mixed(VM2 *vm) { extern int avm1_mix_active(Player *p); return avm1_mix_active(vm->player); }
static MovieClip *fo_a1_focus(VM2 *vm) { MovieClip *f = vm->player->focus; return (f && !f->dead && CLIP_A1MIX(f) && fo_mixed(vm)) ? f : NULL; }
static void fo_set_internal(VM2 *vm, A2Obj *o) {
    A2Obj *old = vm->focus; MovieClip *old1 = fo_a1_focus(vm);
    if (o && a2_disp(o)->is_text && a2_disp(o)->tfs) tfs_apply_autosize(vm->player, a2_disp(o)->tfs);   /* focusing reads the bounds */
    if (old == o && !old1) return;
    if (old && a2_disp(old)->is_text && a2_disp(old)->tfs) { extern void a2_text_ime_commit_pending(VM2 *vm, A2Obj *o); a2_text_ime_commit_pending(vm, old); }
    vm->focus = o;
    if (o && a2_disp(o)->is_text) vm->player->caret_blink = 16;   /* a newly focused field shows its caret at once */
    { MovieClip *c = o ? clip_of(o) : NULL; if (c && a2_disp(o)->is_text) vm->player->focus = c; else if (old1 || !(vm->player->focus && CLIP_A1MIX(vm->player->focus))) vm->player->focus = NULL; }
    vm->focus_hl = fo_calc_highlight(vm);
    /* AVM1's handlers and Selection.onSetFocus come before the AVM2 focus events */
    if (fo_mixed(vm)) { extern void avm1_mixed_focus_from_avm2(Player *p, MovieClip *old1); avm1_mixed_focus_from_avm2(vm->player, old1); }
    if (old) { A2Obj *e = fo_event(vm, "focusOut", 0, o, 0); if (e) a2_dispatch_event_obj(vm, old, e); }
    if (o) { A2Obj *e = fo_event(vm, "focusIn", 0, old, 0); if (e) a2_dispatch_event_obj(vm, o, e); }
}
/* hooks for the AVM1 side of mixed content */
int avm2_mixed_has_focus(Player *p) { VM2 *vm = (VM2*)p->avm2; return vm && vm->focus != NULL; }
void avm2_mixed_focus_to_avm1(Player *p) {   /* an AVM1 object took the focus: focusOut without a related object */
    VM2 *vm = (VM2*)p->avm2; A2Obj *old = vm ? vm->focus : NULL;
    if (!old) return;
    if (a2_disp(old)->is_text && a2_disp(old)->tfs) { extern void a2_text_ime_commit_pending(VM2 *vm, A2Obj *o); a2_text_ime_commit_pending(vm, old); }
    vm->focus = NULL; vm->focus_hl = 0;
    { A2Obj *e = fo_event(vm, "focusOut", 0, NULL, 0); if (e) a2_dispatch_event_obj(vm, old, e); }
    if (vm->throwing) a2_report_uncaught(vm);
}
int avm2_mixed_focus_change_event(Player *p, int by_key) {   /* the AVM2 focus hears mouse/keyFocusChange (relatedObject null); 1 if cancelled */
    VM2 *vm = (VM2*)p->avm2; int r;
    if (!vm || !vm->focus) return 0;
    r = fo_change_event(vm, by_key ? "keyFocusChange" : "mouseFocusChange", vm->focus, NULL, by_key ? 9 : 0);
    if (vm->throwing) a2_report_uncaught(vm);
    return r;
}
void a2_focus_set(VM2 *vm, A2Obj *o) { fo_set_internal(vm, o); }
/* the window lost the keyboard focus (FocusTracker::reset_focus) */
void avm2_focus_lost(Player *p) {
    VM2 *vm = (VM2*)p->avm2;
    if (!vm) return;
    fo_set_internal(vm, NULL);
    if (vm->throwing) a2_report_uncaught(vm);
}
static void fo_set_by_key(VM2 *vm, A2Obj *o, int keycode) {
    if (!fo_a1_focus(vm) && fo_change_event(vm, "keyFocusChange", vm->focus, o, keycode)) return;   /* an AVM1 focus gets no AVM2 event */
    fo_set_internal(vm, o);
}
void a2_focus_mouse_press(VM2 *vm, A2Obj *t) {
    extern int avm1_clip_focusable_by_mouse(MovieClip *c);
    A2Obj *old = vm->focus; MovieClip *old1 = fo_a1_focus(vm);
    vm->focus_hl = 0;
    if (t && !fo_is_interactive(vm, t)) t = NULL;
    if (!t && !(old && fo_focusable_by_mouse(vm, old)) && !(old1 && avm1_clip_focusable_by_mouse(old1))) return;
    if (old == t && !old1) return;
    if (!old1 && fo_change_event(vm, "mouseFocusChange", old, t, 0)) return;
    if (t && !fo_focusable_by_mouse(vm, t)) t = NULL;
    fo_set_internal(vm, t);
    vm->focus_hl = 0;
}
/* mixed content: AVM2 and AVM1 tab stops in one automatic order */
static int fo_cycle_mixed(VM2 *vm, int reverse) {
    extern int avm1_mix_tab_entries(Player *p, MovieClip **out, long *keys, int max); extern void avm1_mix_focus_by_key(Player *p, MovieClip *c);
    FoList t; MovieClip *a1[256]; long k1[256]; int n1, n, i, j, k, cur = -1, nx;
    void **o; double *key; char *is1; MovieClip *f1 = fo_a1_focus(vm);
    n1 = avm1_mix_tab_entries(vm->player, a1, k1, 256);
    if (n1 < 0) return 0;
    fo_order(vm, &t);
    if (t.custom) { fo_free(&t); return 0; }
    n = t.n + n1;
    if (!n) { fo_free(&t); return 1; }
    o = (void**)malloc((size_t)n * sizeof(void*)); key = (double*)malloc((size_t)n * sizeof(double)); is1 = (char*)malloc((size_t)n);
    if (!o || !key || !is1) { free(o); free(key); free(is1); fo_free(&t); return 1; }
    for (i = j = k = 0; k < n; k++) {   /* merge (stable: AVM2 first on equal keys) */
        if (j >= n1 || (i < t.n && t.key[i] <= (double)k1[j])) { o[k] = t.o[i]; key[k] = t.key[i]; is1[k] = 0; i++; }
        else { o[k] = a1[j]; key[k] = (double)k1[j]; is1[k] = 1; j++; }
    }
    for (k = 0; k < n; k++) if ((is1[k] && o[k] == (void*)f1 && f1) || (!is1[k] && o[k] == (void*)vm->focus && vm->focus)) { cur = k; break; }
    if (cur < 0) nx = reverse ? n - 1 : 0;
    else if (reverse) nx = cur > 0 ? cur - 1 : n - 1;
    else nx = cur + 1 < n ? cur + 1 : 0;
    if (is1[nx]) {
        MovieClip *c = (MovieClip*)o[nx];
        if (!avm2_mixed_focus_change_event(vm->player, 1)) avm1_mix_focus_by_key(vm->player, c);
        vm->focus_hl = 0;
    } else { fo_set_by_key(vm, (A2Obj*)o[nx], 9); vm->focus_hl = fo_calc_highlight(vm); }
    free(o); free(key); free(is1); fo_free(&t);
    return 1;
}
void a2_focus_cycle(VM2 *vm, int reverse) {
    FoList t; A2Obj *next = NULL; int i, cur = -1;
    if (fo_mixed(vm) && fo_cycle_mixed(vm, reverse)) return;
    fo_order(vm, &t);
    if (t.n) {
        for (i = 0; i < t.n; i++) if (t.o[i] == vm->focus) { cur = i; break; }
        if (cur < 0) next = reverse ? t.o[t.n - 1] : t.o[0];
        else if (reverse) next = cur > 0 ? t.o[cur - 1] : t.o[t.n - 1];
        else next = cur + 1 < t.n ? t.o[cur + 1] : t.o[0];
    }
    fo_free(&t);
    if (next) { fo_set_by_key(vm, next, 9); vm->focus_hl = fo_calc_highlight(vm); }
}
static double fo_nav_key(const SWFRect *o, const SWFRect *r, int dir, int *cat) {
    double dx, dy;
    *cat = 2;
    switch (dir) {
    case 40: if (r->ymax <= o->ymax) { *cat = -1; return 0; } if (r->xmax >= o->xmin && r->xmin <= o->xmax) { *cat = 0; return (double)(long)r->ymin - o->ymin; } if (r->ymin <= o->ymax) { double a1 = (double)(long)o->xmax - r->xmin, b1 = (double)(long)r->xmax - o->xmin; *cat = 1; return a1 > b1 ? a1 : b1; } break;
    case 38: if (r->ymax >= o->ymax) { *cat = -1; return 0; } if (r->xmax >= o->xmin && r->xmin <= o->xmax) { *cat = 0; return (double)(long)o->ymax - r->ymax; } break;
    case 39: if (r->xmax <= o->xmax) { *cat = -1; return 0; } if (r->ymax >= o->ymin && r->ymin <= o->ymax) { *cat = 0; return (double)(long)r->xmin - o->xmin; } break;
    case 37: if (r->xmin >= o->xmin) { *cat = -1; return 0; } if (r->ymax >= o->ymin && r->ymin <= o->ymax) { *cat = 0; return (double)(long)o->xmax - r->xmax; } break;
    }
    { double a1 = (double)(long)o->xmax - r->xmin, b1 = (double)(long)r->xmax - o->xmin; dx = a1 > b1 ? a1 : b1; }
    { double a1 = (double)(long)o->ymax - r->ymin, b1 = (double)(long)r->ymax - o->ymin; dy = a1 > b1 ? a1 : b1; }
    return dx * dx + dy * dy;
}
void a2_focus_navigate(VM2 *vm, int dir) {
    FoList t; int i, best = -1, bcat = 3; double bkey = 0; SWFRect o;
    if (!vm->focus) return;
    fo_bounds(vm, vm->focus, &o);
    memset(&t, 0, sizeof(t));
    if (vm->stage_obj) fo_fill(vm, vm->stage_obj, &t, 0);
    for (i = 0; i < t.n; i++) {
        SWFRect r; int cat; double k;
        fo_bounds(vm, t.o[i], &r);
        k = fo_nav_key(&o, &r, dir, &cat);
        if (cat < 0) continue;
        if (best < 0 || cat < bcat || (cat == bcat && k < bkey)) { best = i; bcat = cat; bkey = k; }
    }
    if (best >= 0) fo_set_by_key(vm, t.o[best], dir);
    free(t.o);
}
void a2_render_highlight(Player *p, const SWFMatrix *sm) {
    extern void player_draw_focus_rect(Player *p, const SWFRect *rr, const SWFMatrix *sm);
    VM2 *vm = (VM2*)p->avm2; SWFRect r;
    if (!vm || vm->focus_hl != 2 || !vm->focus) return;
    if (!fo_bounds(vm, vm->focus, &r)) return;
    player_draw_focus_rect(p, &r, sm);
}

/* FLASHNT_TREE: print the display list (debugging) */
static void dump_clip(VM2 *vm, MovieClip *c, int lvl, FILE *f) {
    DisplayObject *d; int i;
    if (!c || lvl > 40) return;
    for (d = c->children; d; d = d->next) {
        A2Obj *o = (A2Obj*)d->obj2;
        for (i = 0; i < lvl; i++) fputs("  ", f);
        fprintf(f, "depth %d char %d type %d %s vis %d alpha %.2f at (%.1f,%.1f) scale (%.2f,%.2f)", d->depth, d->char_id, d->ch ? d->ch->type : -1,
                vm && o && o->cls ? a2_class_name(vm, o->cls, 1)->s : "-", d->visible, d->cxform.ma / 256.0, d->matrix.tx / 20.0, d->matrix.ty / 20.0, d->matrix.sx, d->matrix.sy);
        if (vm && o && o->payload) { A2Disp *st = (A2Disp*)o->payload; { extern struct BitmapData *a2_bitmapdata_of(A2Obj *o); struct BitmapData *b = st->bitmap_data ? a2_bitmapdata_of(st->bitmap_data) : NULL; if (b) { fprintf(f, " bmd %ux%u px0 %08x pxc %08x", b->w, b->h, bmd_get_pixel32(b, 0, 0), bmd_get_pixel32(b, b->w / 2, b->h / 2)); } } }
        if (d->name[0]) fprintf(f, " name %s", d->name);
        fputc('\n', f);
        if (d->clip) dump_clip(vm, d->clip, lvl + 1, f);
    }
}
void avm2_dump_tree(Player *p, void *fout) {
    VM2 *vm = (VM2*)p->avm2; FILE *f = (FILE*)fout;
    fprintf(f, "display list:\n");
    dump_clip(vm, p->root, 1, f);
}
