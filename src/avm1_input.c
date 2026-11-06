/* avm1_input.c - mouse and keyboard input for AVM1 movies, and AVM1
 * buttons.
 *
 * Buttons are containers: the children of the current state (up, over,
 * down) are real display objects that run their timelines, and the hit
 * area is a separate set of objects used only for mouse picking.
 *
 * Input follows the Flash Player's order: the clip event (mouseDown,
 * keyDown ...) travels through every clip, the Mouse / Key listeners are
 * notified, keyPress button events follow, the action queue runs, and then
 * the mouse state is updated: the object under the mouse is picked and
 * gets rollOver / rollOut / press / release / dragOver / dragOut events.
 * After every frame the hover state is checked again, as objects can move
 * under a still mouse.
 *
 * The model is a port of the Flash Player behaviour documented by the
 * Ruffle test suite (InteractiveObject, Avm1Button, Player::update_mouse_state). */
#include "avm1_int.h"
#include "shape.h"
#include "render.h"
#include "textfield.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern void clip_world_matrix(MovieClip *c, SWFMatrix *out);
extern int  dg_shape_contains(ShapeDef *s, double px, double py);
extern int  dg_char_bounds(SWFChar *ch, SWFRect *out);
extern void dg_local_bounds(MovieClip *c, SWFRect *out, int *any);
extern ShapeDef *a1_clip_drawing(MovieClip *c);
extern struct TextFieldState *a1_clip_textfield(MovieClip *c);
extern int  avm1_clip_event(VM *vm, MovieClip *c, u32 flag, int keycode);
extern void avm1_queue_do_action(Player *p, MovieClip *c, const u8 *code, size_t len);
extern void avm1_queue_method(VM *vm, MovieClip *c, AObj *o, AStr *name, int argc, Value *argv);
extern void avm1_queue_notify(VM *vm, MovieClip *c, const char *broadcaster, AStr *method, int argc, Value *argv);
extern void avm1_run_actions(VM *vm);
extern void avm1_post_instantiation(Player *p, MovieClip *c);
extern void avm1_clip_run_frame(Player *p, MovieClip *c);
extern void avm1_replace_at_depth(VM *vm, MovieClip *parent, int depth);
extern void avm1_remove_child(Player *p, MovieClip *parent, DisplayObject *d);
extern int  clip_find_frame_label_ci(MovieClip *c, const char *label);
extern int  button_record_count(SWFChar *ch);
extern int  button_record_get(SWFChar *ch, int i, int *flags, int *char_id, int *depth, SWFMatrix *m, SWFCxform *cx);
extern int  button_action_count(SWFChar *ch);
extern int  button_action_get(SWFChar *ch, int i, int *cond, int *keycode, const u8 **code, size_t *len);
extern int  matrix_invert(const SWFMatrix *m, double *a, double *b, double *c, double *d, double *e, double *f);

#define VMP(p) ((VM*)(p)->avm)

/* events */
enum { EV_NONE = 0, EV_MOUSE_MOVE, EV_MOUSE_DOWN, EV_MOUSE_UP, EV_KEY_DOWN, EV_KEY_UP, EV_KEY_PRESS,
       EV_PRESS, EV_RELEASE, EV_RELEASE_OUTSIDE, EV_ROLL_OVER, EV_ROLL_OUT, EV_DRAG_OVER, EV_DRAG_OUT, EV_MOUSE_WHEEL };

static u32 ev_flag(int ev) {
    switch (ev) {
    case EV_MOUSE_MOVE: return CE_MOUSE_MOVE; case EV_MOUSE_DOWN: return CE_MOUSE_DOWN; case EV_MOUSE_UP: return CE_MOUSE_UP;
    case EV_KEY_DOWN: return CE_KEY_DOWN; case EV_KEY_UP: return CE_KEY_UP; case EV_KEY_PRESS: return CE_KEY_PRESS;
    case EV_PRESS: return CE_PRESS; case EV_RELEASE: return CE_RELEASE; case EV_RELEASE_OUTSIDE: return CE_RELEASE_OUTSIDE;
    case EV_ROLL_OVER: return CE_ROLL_OVER; case EV_ROLL_OUT: return CE_ROLL_OUT; case EV_DRAG_OVER: return CE_DRAG_OVER;
    case EV_DRAG_OUT: return CE_DRAG_OUT;
    default: return 0;
    }
}
#define BUTTON_FLAGS (CE_DRAG_OUT | CE_DRAG_OVER | CE_KEY_PRESS | CE_PRESS | CE_ROLL_OUT | CE_ROLL_OVER | CE_RELEASE | CE_RELEASE_OUTSIDE)
static int ev_propagates(int ev) { return ev == EV_MOUSE_MOVE || ev == EV_MOUSE_DOWN || ev == EV_MOUSE_UP || ev == EV_KEY_PRESS || ev == EV_KEY_DOWN || ev == EV_KEY_UP; }
static int ev_is_button(int ev) { return (ev_flag(ev) & BUTTON_FLAGS) != 0; }
static int ev_is_mouse(int ev) { return ev == EV_MOUSE_MOVE || ev == EV_MOUSE_DOWN || ev == EV_MOUSE_UP || (ev >= EV_PRESS && ev <= EV_MOUSE_WHEEL); }
static int ev_is_key(int ev) { return ev == EV_KEY_DOWN || ev == EV_KEY_UP || ev == EV_KEY_PRESS; }

/* kinds of interactive objects */
static int is_button(MovieClip *c) { return c && c->a1 && c->a1->kind == 1; }
static int is_text(MovieClip *c) { return c && c->a1 && c->a1->kind == 2; }
static int is_mc(MovieClip *c) { return c && clip_is_movieclip(c); }
static int visible(MovieClip *c) { return !c->self || c->self->visible; }

int avm1_button_ready(MovieClip *c) { return c && c->a1 && c->a1->btn_inited; }

/* a boolean property of the clip's object (enabled, useHandCursor) */
static int bool_prop(VM *vm, MovieClip *c, const char *name, int def) {
    Act a; Value v; int r;
    if (!c->obj) return 0;
    act_init_nothing(&a, vm, clip_avm1_root(c), "[AVM1 Boolean Property]");
    v = o_get(&a, (AObj*)c->obj, str_intern(vm, name));
    if (vm->err) { vm->err = 0; return def; }
    if (v.t == V_UNDEF) return def;
    r = v_as_bool(v, a.swf_version);
    return r;
}
static int enabled(VM *vm, MovieClip *c) { return bool_prop(vm, c, "enabled", 1); }

/* MovieClip::is_button_mode */
static int button_mode(VM *vm, MovieClip *c) {
    static const char *methods[] = { "onDragOver", "onDragOut", "onPress", "onRelease", "onReleaseOutside", "onRollOut", "onRollOver" };
    Act a; int i;
    if (c->a1 && (c->a1->event_flags & BUTTON_FLAGS)) {
        int k;
        for (k = 0; k < c->a1->nevents; k++) if (c->a1->events[k].flags & BUTTON_FLAGS) return 1;
    }
    if (!clip_avm1_parent(c) || !c->obj) return 0;
    act_init_nothing(&a, vm, clip_avm1_root(c), "[Mouse Pick]");
    for (i = 0; i < 7; i++) {
        int has = o_has_property(&a, (AObj*)c->obj, str_intern(vm, methods[i]));
        if (vm->err) vm->err = 0;
        if (has) return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* hit tests (stage twips)                                          */

#define HT_SKIP_INVISIBLE 1
#define HT_SKIP_MASK 2
#define HT_SKIP_CHILDREN 4
#define HT_MOUSE_PICK (HT_SKIP_INVISIBLE | HT_SKIP_MASK)

static int to_local(const SWFMatrix *w, double sx, double sy, double *lx, double *ly) {
    double a, b, c, d, e, f;
    if (!matrix_invert(w, &a, &b, &c, &d, &e, &f)) return 0;
    *lx = a * sx + b * sy + e; *ly = c * sx + d * sy + f;
    return 1;
}

static int hit_obj(VM *vm, DisplayObject *d, const SWFMatrix *pw, double sx, double sy, int opts);

static int hit_children(VM *vm, MovieClip *c, const SWFMatrix *w, double sx, double sy, int opts) {
    DisplayObject *k; int clip_depth = 0;
    for (k = c->children; k; k = k->next) {
        if (k->depth < 0) continue;                  /* waiting for removal */
        if (k->clip_depth > 0) {
            if (hit_obj(vm, k, w, sx, sy, HT_SKIP_MASK | HT_SKIP_INVISIBLE)) clip_depth = 0;
            else clip_depth = k->clip_depth;
        } else if (k->depth >= clip_depth && hit_obj(vm, k, w, sx, sy, opts)) return 1;
    }
    return 0;
}

/* the clip `c` placed with world matrix w */
static int hit_clip_w(VM *vm, MovieClip *c, const SWFMatrix *w, double sx, double sy, int opts) {
    A1Clip *s = c->a1; double lx, ly;
    if (is_button(c)) {
        DisplayObject *k;
        for (k = c->children; k; k = k->next) if (k->depth >= 0 && hit_obj(vm, k, w, sx, sy, opts)) return 1;
        return 0;
    }
    if (is_text(c)) {
        struct TextFieldState *t = a1_clip_textfield(c);
        if ((opts & HT_SKIP_INVISIBLE) && !visible(c)) return 0;
        if (!t || !to_local(w, sx, sy, &lx, &ly)) return 0;
        return lx >= t->xmin && lx <= t->xmin + t->w && ly >= t->ymin && ly <= t->ymin + t->h;
    }
    if ((opts & HT_SKIP_INVISIBLE) && !visible(c) && !(s && s->maskee)) return 0;
    if ((opts & HT_SKIP_MASK) && s && s->maskee) return 0;
    if (s && s->masker) {
        SWFMatrix mw; clip_world_matrix(s->masker, &mw);
        if (!hit_clip_w(vm, s->masker, &mw, sx, sy, HT_SKIP_INVISIBLE)) return 0;
    }
    if (!(opts & HT_SKIP_CHILDREN) && hit_children(vm, c, w, sx, sy, opts)) return 1;
    {
        ShapeDef *dr = a1_clip_drawing(c);
        if (dr && to_local(w, sx, sy, &lx, &ly) && dg_shape_contains(dr, lx, ly)) return 1;
    }
    return 0;
}

static int hit_obj(VM *vm, DisplayObject *d, const SWFMatrix *pw, double sx, double sy, int opts) {
    SWFMatrix w; double lx, ly; SWFRect r;
    if (!dobj_enter(d, pw, sx, sy, &w)) return 0;
    if (d->clip) return hit_clip_w(vm, d->clip, &w, sx, sy, opts);
    if (!d->ch) return 0;
    if ((opts & HT_SKIP_INVISIBLE) && !d->visible) return 0;
    if (!to_local(&w, sx, sy, &lx, &ly)) return 0;
    if (d->ch->type == CHAR_SHAPE) {
        ShapeDef *sh = shape_get(d->ch->movie, d->ch);
        if (!sh || lx < sh->bounds.xmin || lx > sh->bounds.xmax || ly < sh->bounds.ymin || ly > sh->bounds.ymax) return 0;
        return dg_shape_contains(sh, lx, ly);
    }
    if (d->ch->type == CHAR_MORPH) {   /* the shape at the current ratio */
        extern int morph_shape_at(SWFMovie *mv, SWFChar *ch, int ratio, ShapeDef *out);
        extern void morph_shape_release(ShapeDef *s);
        ShapeDef ms; int hit;
        if (!morph_shape_at(d->ch->movie, d->ch, d->ratio, &ms)) return 0;
        hit = dg_shape_contains(&ms, lx, ly);
        morph_shape_release(&ms);
        return hit;
    }
    if (!dg_char_bounds(d->ch, &r)) return 0;
    if (!(lx >= r.xmin && lx <= r.xmax && ly >= r.ymin && ly <= r.ymax)) return 0;
    /* static text hits on its glyph outlines */
    if (d->ch->type == CHAR_TEXT) { extern int text_hit_glyphs(SWFMovie *mv, SWFChar *ch, double lx, double ly); return text_hit_glyphs(d->ch->movie, d->ch, lx, ly); }
    return 1;
}

static int hit_clip(VM *vm, MovieClip *c, double sx, double sy, int opts) {
    SWFMatrix w; clip_world_matrix(c, &w);
    return hit_clip_w(vm, c, &w, sx, sy, opts);
}

/* ---------------------------------------------------------------- */
/* mouse picking                                                    */

static MovieClip *pick(VM *vm, MovieClip *c, const SWFMatrix *w, double sx, double sy, int require_button_mode);

static MovieClip *hit_area_of(VM *vm, MovieClip *c) {
    Act a; Value v; AObj *o; MovieClip *h;
    if (!c->obj) return NULL;
    act_init_nothing(&a, vm, clip_avm1_root(c), "[Mouse Pick]");
    v = o_get(&a, (AObj*)c->obj, str_intern(vm, "hitArea"));
    if (vm->err) { vm->err = 0; return NULL; }
    o = v_obj_of(&a, v);
    if (vm->err) { vm->err = 0; return NULL; }
    h = o ? o_display(o) : NULL;
    return h;
}

static MovieClip *pick_mc(VM *vm, MovieClip *c, const SWFMatrix *w, double sx, double sy, int rbm) {
    A1Clip *s = c->a1; DisplayObject *list[1024], *k; int n = 0, i, hit_depth = 0; MovieClip *result = NULL; SWFMatrix fresh;
    if (c->movie && c->movie->has_abc) return NULL;
    if (clip_removed(c)) return NULL;
    if (!visible(c)) return NULL;
    if (s && s->masker && !hit_clip(vm, s->masker, sx, sy, HT_SKIP_INVISIBLE)) return NULL;
    if (button_mode(vm, c)) {
        MovieClip *area = hit_area_of(vm, c); int hit;
        /* the hitArea getter may have moved this clip: test where it is now */
        clip_world_matrix(c, &fresh); w = &fresh;
        if (!rbm) area = NULL;
        if (area && !clip_removed(area)) hit = hit_clip(vm, area, sx, sy, HT_SKIP_MASK);
        else hit = hit_clip_w(vm, c, w, sx, sy, HT_SKIP_INVISIBLE | ((s && s->maskee) ? 0 : HT_SKIP_MASK));
        if (hit) return c;
    }
    for (k = c->children; k && n < 1024; k = k->next) list[n++] = k;
    for (i = n - 1; i >= 0; i--) {
        k = list[i];
        if (k->depth < 0 && !k->clip) continue;
        if (k->clip_depth > 0) {
            if (result && k->clip_depth >= hit_depth) {
                if (hit_obj(vm, k, w, sx, sy, HT_MOUSE_PICK)) return result;
                result = NULL;
            }
        } else if (!result) {
            if (k->clip) {
                SWFMatrix cw;
                result = dobj_enter(k, w, sx, sy, &cw) ? pick(vm, k->clip, &cw, sx, sy, rbm) : NULL;
            } else if (!rbm && hit_obj(vm, k, w, sx, sy, HT_SKIP_INVISIBLE | ((s && s->maskee) ? 0 : HT_SKIP_MASK))) result = c;
            if (result) hit_depth = k->depth;
        }
    }
    if (result) return result;
    if (!rbm) {
        ShapeDef *dr = a1_clip_drawing(c); double lx, ly;
        if (dr && to_local(w, sx, sy, &lx, &ly) && dg_shape_contains(dr, lx, ly)) return c;
    }
    return NULL;
}

static MovieClip *pick_button(VM *vm, MovieClip *c, const SWFMatrix *w, double sx, double sy, int rbm) {
    DisplayObject *list[256], *k; int n = 0, i;
    if (!visible(c)) return NULL;
    for (k = c->children; k && n < 256; k = k->next) list[n++] = k;
    for (i = n - 1; i >= 0; i--) {
        k = list[i];
        if (k->clip && k->depth >= 0) {
            SWFMatrix cw; MovieClip *r;
            r = dobj_enter(k, w, sx, sy, &cw) ? pick(vm, k->clip, &cw, sx, sy, rbm) : NULL;
            if (r) return r;
        }
    }
    for (k = c->a1 ? c->a1->btn_hit : NULL; k; k = k->next) if (hit_obj(vm, k, w, sx, sy, HT_MOUSE_PICK)) return c;
    return NULL;
}

static MovieClip *pick_text(VM *vm, MovieClip *c, const SWFMatrix *w, double sx, double sy) {
    struct TextFieldState *t = a1_clip_textfield(c);
    if (!t || !visible(c)) return NULL;
    if (!t->selectable) {
        /* a non-selectable field still takes the mouse over a link */
        extern int a1_tf_link_url_at(VM *vm, MovieClip *c, double sx, double sy);
        if (!a1_tf_link_url_at(vm, c, sx, sy)) return NULL;
    }
    return hit_clip_w(vm, c, w, sx, sy, HT_MOUSE_PICK) ? c : NULL;
}

static MovieClip *pick(VM *vm, MovieClip *c, const SWFMatrix *w, double sx, double sy, int rbm) {
    if (!c || !c->a1 || c->dead) return NULL;
    if (is_button(c)) return pick_button(vm, c, w, sx, sy, rbm);
    if (is_text(c)) return pick_text(vm, c, w, sx, sy);
    return pick_mc(vm, c, w, sx, sy, rbm);
}

/* AVM1Movie content inside an AVM2 player: the AVM2 display list decides what is on top */
static int g_pick_rbm = 1;
MovieClip *avm1_pick_in(void *vmp, MovieClip *c, const SWFMatrix *w, double sx, double sy) {
    VM *vm = (VM*)vmp;
    if (!vm || !c) return NULL;
    return pick(vm, c, w, sx, sy, g_pick_rbm);
}

/* shape hit test of an AVM1Movie's content (Ruffle hit_test_shape with SKIP_INVISIBLE | SKIP_MASK) */
int avm1_hit_shape_in(void *vmp, MovieClip *c, const SWFMatrix *w, double sx, double sy) {
    VM *vm = (VM*)vmp;
    if (!vm || !c) return 0;
    return hit_clip_w(vm, c, w, sx, sy, HT_SKIP_INVISIBLE | HT_SKIP_MASK);
}

static MovieClip *mouse_pick(VM *vm, int rbm) {
    Player *p = vm->player; MovieClip *levels[64]; int n = 0, i; DisplayObject *d;
    double sx = vm->mouse_tx, sy = vm->mouse_ty;
    if (p->avm2) {
        extern MovieClip *avm2_pick_avm1(Player *p, double sx, double sy);
        MovieClip *r; int saved = g_pick_rbm;
        g_pick_rbm = rbm;
        r = avm2_pick_avm1(p, sx - p->movie->stage.xmin, sy - p->movie->stage.ymin);
        g_pick_rbm = saved;
        return r;
    }
    levels[n++] = p->root;
    for (d = p->levels; d && n < 64; d = d->next) if (d->clip) levels[n++] = d->clip;
    for (i = n - 1; i >= 0; i--) {
        SWFMatrix w; MovieClip *r;
        clip_world_matrix(levels[i], &w);
        r = pick(vm, levels[i], &w, sx, sy, rbm);
        if (r) return r;
    }
    return NULL;
}

/* ---------------------------------------------------------------- */
/* buttons                                                          */

static int state_bit(int state) { return state == 2 ? 0x04 : state == 1 ? 0x02 : 0x01; }

static void button_run_actions(VM *vm, MovieClip *b, int cond, int keycode, int *handled) {
    SWFChar *ch = b->self ? b->self->ch : NULL; int i, n;
    MovieClip *parent = b->parent;
    if (!ch || !parent) return;
    n = button_action_count(ch);
    for (i = 0; i < n; i++) {
        int c, k; const u8 *code; size_t len;
        if (!button_action_get(ch, i, &c, &k, &code, &len)) continue;
        if (keycode ? (k == keycode) : ((c & cond) != 0)) {
            if (handled) *handled = 1;
            avm1_queue_do_action(vm->player, parent, code, len);
        }
    }
}

/* Avm1Button::set_state: keep the children both states share, create the new ones */
static void button_set_state(VM *vm, MovieClip *b, int state) {
    Player *p = vm->player; SWFChar *ch = b->self ? b->self->ch : NULL; int i, n, bit = state_bit(state);
    DisplayObject *newc[64]; int nn = 0; int keep[256]; int nkeep = 0; DisplayObject *k, *next;
    if (!ch || !b->a1) return;
    { extern void avm1_remove_pending_children(VM *vm, MovieClip *c); avm1_remove_pending_children(vm, b); }
    b->a1->btn_state = state;
    if (b->self) b->self->btn_state = state;
    n = button_record_count(ch);
    for (i = 0; i < n; i++) {
        int flags, id, depth; SWFMatrix m; SWFCxform cx; DisplayObject *child = NULL;
        if (!button_record_get(ch, i, &flags, &id, &depth, &m, &cx) || !(flags & bit)) continue;
        depth++;   /* Flash: a button record at depth d reports getDepth() == d - 16383 */
        if (nkeep < 256) keep[nkeep++] = depth;
        for (k = b->children; k; k = k->next) if (k->depth == depth) break;
        if (k && k->char_id == id) child = k;
        else {
            SWFChar *cc = swf_get_char(b->movie ? b->movie : p->movie, id);
            if (!cc || nn >= 64) continue;
            child = player_instantiate(p, b, cc, id, depth);
            if (!child) continue;
            newc[nn++] = child;
        }
        child->matrix = m; child->cxform = cx; child->sr_cached = 0;
    }
    for (k = b->children; k; k = next) {
        int j, kept = 0;
        next = k->next;
        if (k->depth < 0) continue;
        for (j = 0; j < nkeep; j++) if (keep[j] == k->depth) { kept = 1; break; }
        if (!kept) avm1_remove_child(p, b, k);
    }
    for (i = 0; i < nn; i++) {
        DisplayObject *c = newc[i];
        if (c->clip) {
            avm1_post_instantiation(p, c->clip);
            if (c->ch->type == CHAR_SPRITE && !c->clip->dead) avm1_clip_run_frame(p, c->clip);
        }
        avm1_replace_at_depth(vm, b, c->depth);
        player_insert_dobj(b, c);
    }
}

/* Avm1Button::post_instantiation: the up state and the hit area */
void avm1_button_init(VM *vm, MovieClip *b) {
    Player *p = vm->player; SWFChar *ch = b->self ? b->self->ch : NULL; int i, n; DisplayObject **tail;
    A1Clip *s = a1_of(b);
    if (!ch || !s || s->btn_inited) return;
    button_set_state(vm, b, 0);
    s->btn_inited = 1;
    n = button_record_count(ch);
    tail = &s->btn_hit;
    for (i = 0; i < n; i++) {
        int flags, id, depth; SWFMatrix m; SWFCxform cx; SWFChar *cc; DisplayObject *h, **pp;
        if (!button_record_get(ch, i, &flags, &id, &depth, &m, &cx) || !(flags & 0x08)) continue;
        depth++;
        cc = swf_get_char(b->movie ? b->movie : p->movie, id);
        if (!cc) continue;
        h = player_instantiate(p, b, cc, id, depth);
        if (!h) continue;
        h->matrix = m;
        /* hit areas are not script objects (no name, no frame scripts): a sprite only lays out its first frame */
        if (h->clip && h->ch->type == CHAR_SPRITE) { extern void player_place_first_frame(MovieClip *c); player_place_first_frame(h->clip); }
        /* one object per depth: a later record replaces an earlier one */
        for (pp = &s->btn_hit; *pp; pp = &(*pp)->next) if ((*pp)->depth == depth) break;
        if (*pp) { DisplayObject *old = *pp; h->next = old->next; *pp = h; player_destroy_dobj(old); if (!h->next) tail = &h->next; continue; }
        *tail = h; tail = &h->next;
    }
}

void avm1_button_free(MovieClip *b) {
    A1Clip *s = b->a1; DisplayObject *d;
    if (!s) return;
    d = s->btn_hit; s->btn_hit = NULL;
    while (d) { DisplayObject *n = d->next; d->next = NULL; player_destroy_dobj(d); d = n; }
}


static void forget_object(VM *vm, MovieClip *c) {
    if (vm->m_hovered == c) vm->m_hovered = NULL;
    if (vm->m_pressed == c) vm->m_pressed = NULL;
}

static int button_dispatch(VM *vm, MovieClip *b, int ev, int key) {
    int new_state, cond = 0, en, update, handled = 0;
    A1Clip *s = b->a1;
    switch (ev) {
    case EV_DRAG_OUT: new_state = 1; cond = 0x10; break;
    case EV_DRAG_OVER: new_state = 2; cond = 0x20; break;
    case EV_PRESS: new_state = 2; cond = 0x04; break;
    case EV_RELEASE: new_state = 1; cond = 0x08; break;
    case EV_RELEASE_OUTSIDE: new_state = 0; cond = 0x40; break;
    case EV_ROLL_OUT: new_state = 0; cond = 0x02; break;
    case EV_ROLL_OVER: new_state = 1; cond = 0x01; break;
    case EV_KEY_PRESS: button_run_actions(vm, b, 0, key, &handled); return handled;
    case EV_KEY_UP: case EV_KEY_DOWN: new_state = s->btn_state; break;
    default: return 0;
    }
    en = enabled(vm, b);
    if (en) {
        /* Flash: the new state's children (and their first frame scripts) come
           before the button's own actions */
        if (s->btn_state != new_state) button_set_state(vm, b, new_state);
        if (cond) button_run_actions(vm, b, cond, 0, NULL);
        if (b->obj && clip_swf_version(b) >= 6 && !(ev_is_key(ev) && (vm->player->focus != b || !vm->highlight))) {
            AStr *m = NULL;
            switch (ev) {
            case EV_DRAG_OUT: m = vm->s_onDragOut; break; case EV_DRAG_OVER: m = vm->s_onDragOver; break;
            case EV_PRESS: m = vm->s_onPress; break; case EV_RELEASE: m = vm->s_onRelease; break;
            case EV_RELEASE_OUTSIDE: m = vm->s_onReleaseOutside; break; case EV_ROLL_OUT: m = vm->s_onRollOut; break;
            case EV_ROLL_OVER: m = vm->s_onRollOver; break; case EV_KEY_UP: m = vm->s_onKeyUp; break;
            case EV_KEY_DOWN: m = vm->s_onKeyDown; break;
            }
            if (m) avm1_queue_method(vm, b, (AObj*)b->obj, m, 0, NULL);
        }
        update = 0;
    } else {
        forget_object(vm, b);
        update = new_state != 1;
        new_state = 0;
    }
    if (update) button_set_state(vm, b, new_state);
    return 0;
}

/* ---------------------------------------------------------------- */
/* clip events                                                      */

static int filter_event(VM *vm, MovieClip *c, int ev) {
    if (is_button(c)) {
        if (!visible(c) && c->a1->btn_state == 0) return 0;
        if (ev == EV_KEY_PRESS && c->parent && is_button(c->parent)) return 0;
        return 1;
    }
    if (is_text(c)) return ev == EV_PRESS || ev == EV_MOUSE_WHEEL || ev == EV_MOUSE_MOVE;
    if (ev_is_button(ev)) {
        if (!visible(c) && ev != EV_RELEASE_OUTSIDE) return 0;
        if (ev != EV_KEY_PRESS && !enabled(vm, c)) return 0;
    }
    return 1;
}

static int mc_dispatch(VM *vm, MovieClip *c, int ev, int key) {
    const char *label = NULL; u32 flag = ev_flag(ev);
    switch (ev) {
    case EV_ROLL_OUT: case EV_RELEASE_OUTSIDE: label = "_up"; break;
    case EV_ROLL_OVER: case EV_RELEASE: case EV_DRAG_OUT: label = "_over"; break;
    case EV_PRESS: case EV_DRAG_OVER: label = "_down"; break;
    }
    if (label) {
        int f = clip_find_frame_label_ci(c, label);
        if (f >= 0 && button_mode(vm, c)) player_avm1_goto(c, f, 0);
    }
    if (!flag) return 0;
    return avm1_clip_event(vm, c, flag, key);
}

static MovieClip *g_tf_drag;   /* selectable text field being drag-selected */
static int handle_event(VM *vm, MovieClip *c, int ev, int key) {
    if (!c || c->dead || !c->a1) return 0;
    if (!filter_event(vm, c, ev)) return 0;
    if (ev_propagates(ev) && !is_text(c)) {
        DisplayObject *k; MovieClip *stack_list[256], **list = stack_list; int n = 0, cap = 256, i;
        for (k = c->children; k; k = k->next) {
            if (!k->clip) continue;
            if (n == cap) {
                MovieClip **nl = (MovieClip**)malloc((size_t)cap * 2 * sizeof(MovieClip*));
                if (!nl) break;
                memcpy(nl, list, (size_t)n * sizeof(MovieClip*));
                if (list != stack_list) free(list);
                list = nl; cap *= 2;
            }
            list[n++] = k->clip;
        }
        if (ev_is_mouse(ev)) { for (i = n - 1; i >= 0; i--) if (handle_event(vm, list[i], ev, key)) { if (list != stack_list) free(list); return 1; } }
        else { for (i = 0; i < n; i++) if (handle_event(vm, list[i], ev, key)) { if (list != stack_list) free(list); return 1; } }
        if (list != stack_list) free(list);
    }
    if (is_button(c)) return button_dispatch(vm, c, ev, key);
    if (is_text(c)) {
        if (ev == EV_PRESS) {
            struct TextFieldState *t = a1_clip_textfield(c); SWFMatrix w; double lx, ly; int pos = -1;
            if (t) {
                clip_world_matrix(c, &w);
                if (to_local(&w, vm->mouse_tx, vm->mouse_ty, &lx, &ly)) pos = tfs_caret_at_point(vm->player, t, lx, ly);   /* (field-local: it takes the bounds offset itself) */
                if (pos < 0) pos = t->rt.len;
                if (t->selectable) { t->sel_begin = t->sel_end = pos; t->has_sel = 1; g_tf_drag = c; }
                {   /* links open on press (the character under the mouse) */
                    extern void a1_tf_link_at(VM *vm, MovieClip *c, int pos);
                    int ci = -1;
                    /* (the character under the mouse: horizontal scrolling counts here) */
                    if (to_local(&w, vm->mouse_tx, vm->mouse_ty, &lx, &ly)) ci = tfs_char_at_point(vm->player, t, lx - t->xmin + t->scroll_h, ly - t->ymin);
                    if (ci >= 0) a1_tf_link_at(vm, c, ci);
                }
            }
            return 1;
        }
        if (ev == EV_MOUSE_WHEEL) {
            /* the mouse wheel scrolls the field (by lines) */
            extern int a1_tf_set_scroll(VM *vm, MovieClip *c, double scroll, int programmatic);
            struct TextFieldState *t = a1_clip_textfield(c);
            if (t && t->mouse_wheel) return a1_tf_set_scroll(vm, c, (double)t->scroll_v - key, 0);
        }
        return 0;
    }
    return mc_dispatch(vm, c, ev, key);
}

/* the stage: every level, the highest first for mouse events */
static int stage_event(VM *vm, int ev, int key) {
    MovieClip *levels[64]; int n = avm1_stage_roots(vm, levels, 64), i;
    if (ev_is_mouse(ev)) { for (i = n - 1; i >= 0; i--) if (handle_event(vm, levels[i], ev, key)) return 1; }
    else { for (i = 0; i < n; i++) if (handle_event(vm, levels[i], ev, key)) return 1; }
    return 0;
}

/* ---------------------------------------------------------------- */
/* dragging                                                         */

void avm1_update_drag(VM *vm) {
    MovieClip *c = vm->drag; SWFMatrix pw; double a, b, cc, d, e, f, nx, ny, px, py;
    if (!c || vm->drag_updating) return;
    if (clip_removed(c) || !c->self) { vm->drag = NULL; return; }
    if (c->parent) clip_world_matrix(c->parent, &pw); else swf_matrix_identity(&pw);
    if (!matrix_invert(&pw, &a, &b, &cc, &d, &e, &f)) { a = d = 1; b = cc = e = f = 0; }
    px = c->self->matrix.tx; py = c->self->matrix.ty;
    if (vm->drag_lock) {
        nx = a * vm->mouse_tx + b * vm->mouse_ty + e;
        ny = cc * vm->mouse_tx + d * vm->mouse_ty + f;
    } else {
        double dx = vm->mouse_tx - vm->drag_last_x, dy = vm->mouse_ty - vm->drag_last_y;
        nx = px + (a * dx + b * dy);
        ny = py + (cc * dx + d * dy);
    }
    nx = floor(nx + 0.5); ny = floor(ny + 0.5);
    if (vm->drag_constrained) {
        double x0 = floor(vm->drag_rect[0] * 20 + 0.5), y0 = floor(vm->drag_rect[1] * 20 + 0.5), x1 = floor(vm->drag_rect[2] * 20 + 0.5), y1 = floor(vm->drag_rect[3] * 20 + 0.5);
        if (nx < x0) nx = x0; if (nx > x1) nx = x1;
        if (ny < y0) ny = y0; if (ny > y1) ny = y1;
    }
    if (!vm->drag_lock) {
        double mdx = nx - px, mdy = ny - py;
        vm->drag_last_x += pw.sx * mdx + pw.r1 * mdy;
        vm->drag_last_y += pw.r0 * mdx + pw.sy * mdy;
    }
    c->self->matrix.tx = nx; c->self->matrix.ty = ny;
    c->self->script_moved = 1;
    if (clip_is_movieclip(c) && c->a1) {
        int was = c->self->visible; MovieClip *t;
        c->self->visible = 0;
        vm->drag_updating = 1;
        t = mouse_pick(vm, 0);
        vm->drag_updating = 0;
        c->self->visible = was;
        c->a1->drop_target = t ? clip_obj(vm, t) : NULL;
    }
}

void avm1_start_drag_at(VM *vm) { vm->drag_last_x = vm->mouse_tx; vm->drag_last_y = vm->mouse_ty; }

/* ---------------------------------------------------------------- */
/* mouse state                                                      */

static int same_character(MovieClip *a, MovieClip *b) {
    return a->self && b->self && a->self->depth == b->self->depth && a->self->char_id == b->self->char_id && a->movie == b->movie;
}

static MovieClip *find_instance(MovieClip *c, MovieClip *prev) {
    DisplayObject *d;
    for (d = c->children; d; d = d->next) {
        if (d->clip && same_character(d->clip, prev)) return d->clip;
        if (d->clip) { MovieClip *r = find_instance(d->clip, prev); if (r) return r; }
    }
    return NULL;
}

static void set_state_of(VM *vm, MovieClip *c, int state) { if (is_button(c) && c->a1->btn_state != state) button_set_state(vm, c, state); }

typedef struct PEv { MovieClip *obj; int ev; } PEv;

static void focus_by_mouse(VM *vm, MovieClip *c);
static int focusable_by_mouse(MovieClip *c);
static void focus_cycle(VM *vm, int reverse);
static void focus_navigate(VM *vm, int dir);

int g_a1_press_took_focus;   /* mixed content: the AVM1 press handled the focus */
static int focus_mixed(VM *vm);
static void focus_on_press(VM *vm, MovieClip *c) {
    Player *p = vm->player;
    if (c && focusable_by_mouse(c)) { focus_by_mouse(vm, c); if (focus_mixed(vm)) g_a1_press_took_focus = 1; }
    else if (focus_mixed(vm)) {}   /* the AVM2 side clears the focus */
    else if (p->focus && focusable_by_mouse(p->focus)) focus_by_mouse(vm, NULL);
}

static void update_mouse_state(VM *vm, int changed, int moved) {
    MovieClip *new_over, *cur_over; PEv evs[16]; int ne = 0, i, skip, updated = 0;
    skip = !moved && !changed && vm->m_hovered != NULL;
    new_over = vm->mouse_in_stage ? mouse_pick(vm, 1) : NULL;
    if (vm->m_hovered) {
        MovieClip *h = vm->m_hovered;
        if (clip_removed(h)) {
            vm->m_hovered = NULL;
            if (new_over && same_character(new_over, h)) {
                if (is_button(h)) set_state_of(vm, new_over, h->a1->btn_state);
                vm->m_hovered = new_over; updated = 1;
            }
        }
        if (h->self && !h->self->visible) skip = 0;
    }
    if (vm->m_pressed && clip_removed(vm->m_pressed)) {
        MovieClip *old = vm->m_pressed, *n = find_instance(avm1_base_root(vm), old);
        vm->m_pressed = NULL;
        if (n) { if (is_button(old)) set_state_of(vm, n, old->a1->btn_state); vm->m_pressed = n; }
    }
    cur_over = vm->m_hovered;
    if (!skip && cur_over != new_over) {
        if (vm->mouse_down) {
            vm->m_hovered = new_over;
            if (vm->m_pressed) {
                if (vm->m_pressed == cur_over) { evs[ne].obj = vm->m_pressed; evs[ne++].ev = EV_DRAG_OUT; }
                else if (vm->m_pressed == new_over) { evs[ne].obj = vm->m_pressed; evs[ne++].ev = EV_DRAG_OVER; }
            }
            if (!vm->drag) {
                if (cur_over && vm->m_pressed != cur_over) { evs[ne].obj = cur_over; evs[ne++].ev = EV_DRAG_OUT; }
                if (new_over && vm->m_pressed != new_over) { evs[ne].obj = new_over; evs[ne++].ev = EV_DRAG_OVER; }
            }
        } else {
            if (cur_over) { evs[ne].obj = cur_over; evs[ne++].ev = EV_ROLL_OUT; }
            if (new_over) { evs[ne].obj = new_over; evs[ne++].ev = EV_ROLL_OVER; }
        }
    }
    if (!skip && !updated) vm->m_hovered = new_over;
    if (changed) {
        if (vm->mouse_down) {
            if (vm->m_hovered) { evs[ne].obj = vm->m_hovered; evs[ne++].ev = EV_PRESS; vm->m_pressed = vm->m_hovered; }
            else focus_on_press(vm, NULL);
        } else {
            int inside = vm->m_pressed == vm->m_hovered;
            if (!inside && vm->m_pressed && vm->m_hovered) inside = same_character(vm->m_pressed, vm->m_hovered);
            if (inside) { if (vm->m_pressed) { evs[ne].obj = vm->m_pressed; evs[ne++].ev = EV_RELEASE; } }
            else {
                if (vm->m_pressed) { evs[ne].obj = vm->m_pressed; evs[ne++].ev = EV_RELEASE_OUTSIDE; }
                if (vm->m_hovered) { evs[ne].obj = vm->m_hovered; evs[ne++].ev = EV_ROLL_OVER; }
            }
            vm->m_pressed = NULL;
        }
    }
    for (i = 0; i < ne; i++) {
        MovieClip *o = evs[i].obj;
        if (!o || clip_removed(o)) continue;
        handle_event(vm, o, evs[i].ev, 0);
        if (evs[i].ev == EV_PRESS) focus_on_press(vm, o);
    }
    avm1_run_actions(vm);
}

/* ---------------------------------------------------------------- */
/* entry points                                                     */

void avm1_input_mouse(Player *p, int x, int y, int down) {
    VM *vm = VMP(p); int changed, moved; double tx, ty;
    if (!vm || (p->avm2 && !vm->nmix)) return;
    vm->exec_start = 0;
    /* window pixels -> stage twips (the frame rectangle may not start at 0) */
    if (p->has_view) {
        extern void avm1_view_matrix(Player *p, double *sx, double *sy, double *tx, double *ty);
        double vsx, vsy, vtx, vty; avm1_view_matrix(p, &vsx, &vsy, &vtx, &vty);
        tx = floor((x - vtx) / vsx * TWIPS_PER_PX + 0.5) + p->movie->stage.xmin; ty = floor((y - vty) / vsy * TWIPS_PER_PX + 0.5) + p->movie->stage.ymin;
    } else { tx = floor(x * TWIPS_PER_PX / p->zoom + 0.5) + p->movie->stage.xmin; ty = floor(y * TWIPS_PER_PX / p->zoom + 0.5) + p->movie->stage.ymin; }
    changed = (down != 0) != (vm->mouse_down != 0);
    moved = tx != vm->mouse_tx || ty != vm->mouse_ty;
    vm->mouse_down = down != 0;
    if (changed) {
        stage_event(vm, down ? EV_MOUSE_DOWN : EV_MOUSE_UP, 0);
        avm1_queue_notify(vm, avm1_base_root(vm), "Mouse", down ? vm->s_onMouseDown : vm->s_onMouseUp, 0, NULL);
    } else {
        /* every move event fires onMouseMove, even at the same position */
        stage_event(vm, EV_MOUSE_MOVE, 0);
        avm1_queue_notify(vm, avm1_base_root(vm), "Mouse", vm->s_onMouseMove, 0, NULL);
    }
    avm1_run_actions(vm);
    vm->mouse_tx = tx; vm->mouse_ty = ty;
    vm->mouse_in_stage = 1;
    /* dragging over a pressed text field extends its selection */
    if (g_tf_drag && (g_tf_drag->dead || !vm->mouse_down)) g_tf_drag = NULL;
    else if (g_tf_drag && moved) {
        struct TextFieldState *t = a1_clip_textfield(g_tf_drag); SWFMatrix w; double lx, ly; int pos = -1;
        if (t) {
            clip_world_matrix(g_tf_drag, &w);
            if (to_local(&w, tx, ty, &lx, &ly)) pos = tfs_caret_at_point(vm->player, t, lx, ly);   /* (field-local: it takes the bounds offset itself) */
            if (pos >= 0) { t->sel_end = pos; t->has_sel = 1; }
        }
    }
    avm1_update_drag(vm);
    update_mouse_state(vm, changed, moved);
    if ((changed && down) || clip_swf_version(avm1_base_root(vm)) < 9) vm->highlight = 0;
}

void avm1_input_wheel(Player *p, int delta) {
    VM *vm = VMP(p); Value d;
    if (!vm || (p->avm2 && !vm->nmix)) return;
    d = v_num(delta);
    avm1_queue_notify(vm, avm1_base_root(vm), "Mouse", vm->s_onMouseWheel, 1, &d);
    if (vm->m_hovered && !clip_removed(vm->m_hovered)) handle_event(vm, vm->m_hovered, EV_MOUSE_WHEEL, delta);
    avm1_run_actions(vm);
}

/* keyPress codes of special keys */
static int keypress_code(int vk) {
    switch (vk) {
    case 37: return 1; case 39: return 2; case 36: return 3; case 35: return 4; case 45: return 5;
    case 46: return 6; case 8: return 8; case 13: return 13; case 38: return 14; case 40: return 15;
    case 33: return 16; case 34: return 17; case 9: return 18; case 27: return 19;
    default: return 0;
    }
}

extern int avm1_last_key_code, avm1_last_key_ascii;
static int key_ascii(int keycode, int shift) {
    if (keycode >= 'A' && keycode <= 'Z') return shift ? keycode : keycode + 32;
    if (keycode >= '0' && keycode <= '9') return keycode;
    if (keycode == 32) return 32;
    if (keycode == 13) return 13;
    if (keycode == 8) return 8;
    if (keycode == 9) return 9;
    if (keycode == 27) return 27;
    return 0;
}

void avm1_input_key(Player *p, int keycode, int down) {
    VM *vm = VMP(p); int kp, handled;
    if (!vm || (p->avm2 && !vm->nmix)) return;
    avm1_last_key_code = keycode; avm1_last_key_ascii = p->key_ev_char >= 0 ? p->key_ev_char : key_ascii(keycode, p->key_down[16]);   /* key up updates them too */
    stage_event(vm, down ? EV_KEY_DOWN : EV_KEY_UP, 0);
    avm1_queue_notify(vm, avm1_base_root(vm), "Key", down ? vm->s_onKeyDown : vm->s_onKeyUp, 0, NULL);
    kp = down ? keypress_code(keycode) : 0;
    handled = kp ? stage_event(vm, EV_KEY_PRESS, kp) : 0;
    if (!handled && down && keycode == 9 && !p->avm2) focus_cycle(vm, p->key_down[16]);   /* mixed content: the AVM2 side cycles through both */
    if (!handled && down && vm->highlight == 2 && p->focus && !p->focus->dead) {
        MovieClip *f = p->focus;
        if (keycode == 13) { handle_event(vm, f, EV_PRESS, 0); if (!f->dead) handle_event(vm, f, EV_RELEASE, 0); }
        if (keycode >= 37 && keycode <= 40) focus_navigate(vm, keycode);
    }
    avm1_run_actions(vm);
}

/* a typed character: keyPress first, then the focused text field */
void avm1_input_char(Player *p, int ch) {
    VM *vm = VMP(p); int handled = 0;
    if (!vm || (p->avm2 && !vm->nmix)) return;
    if (ch >= 32 && ch <= 126) handled = stage_event(vm, EV_KEY_PRESS, ch);
    if (!handled && p->focus && !p->focus->dead && is_text(p->focus)) {
        extern void a1_tf_typed(void *vm, MovieClip *c, int ch);
        a1_tf_typed(vm, p->focus, ch);
        p->caret_blink = 16;
    } else if (!handled && ch == ' ' && vm->highlight == 2 && p->focus && !p->focus->dead) {
        MovieClip *f = p->focus;
        handle_event(vm, f, EV_PRESS, 0); if (!f->dead) handle_event(vm, f, EV_RELEASE, 0);
    }
    avm1_run_actions(vm);
}

/* after every frame: drag, then hover (objects move under a still mouse) */
void avm1_input_after_frame(Player *p) {
    VM *vm = VMP(p);
    if (!vm || (p->avm2 && !vm->nmix)) return;
    avm1_update_drag(vm);
    update_mouse_state(vm, 0, 0);
}

/* a clip's memory is going away */
void avm1_input_forget(VM *vm, MovieClip *c) {
    forget_object(vm, c);
}

/* ---------------------------------------------------------------- */
/* focus (FocusTracker)                                             */

enum { HL_INACTIVE = 0, HL_HIDDEN, HL_VISIBLE };

static int clip_is_root(MovieClip *c) { return c == c->player->root || (c->a1 && c->a1->is_root) || !c->parent; }

static int prop_bool(VM *vm, MovieClip *c, const char *name, int def) { return bool_prop(vm, c, name, def); }

static int has_tab(MovieClip *c) { return c->a1 && c->a1->has_tab; }
static int tab_index(MovieClip *c) { return c->a1 ? c->a1->tab_index : 0; }

static int tf_editable(MovieClip *c) { struct TextFieldState *t = a1_clip_textfield(c); return t && t->input; }
static int tf_selectable(MovieClip *c) { struct TextFieldState *t = a1_clip_textfield(c); return t && t->selectable; }

static int tab_enabled_default(VM *vm, MovieClip *c) {
    if (is_button(c)) return 1;
    if (is_text(c)) return tf_editable(c);
    return button_mode(vm, c) || has_tab(c);
}
static int tab_enabled(VM *vm, MovieClip *c) {
    Act a; Value v;
    if (!c->obj) return 0;
    act_init_nothing(&a, vm, clip_avm1_root(c), "[AVM1 Boolean Property]");
    v = o_get(&a, (AObj*)c->obj, str_intern(vm, "tabEnabled"));
    if (vm->err) { vm->err = 0; return tab_enabled_default(vm, c); }
    if (v.t == V_UNDEF) return tab_enabled_default(vm, c);
    return v_as_bool(v, a.swf_version);
}
static int is_tabbable(VM *vm, MovieClip *c) {
    if (is_text(c)) return tf_editable(c) && tab_enabled(vm, c);
    if (is_mc(c) && clip_is_root(c)) return 0;
    return tab_enabled(vm, c);
}
static int is_focusable(VM *vm, MovieClip *c) {
    if (is_mc(c)) { if (clip_is_root(c)) return 0; if (button_mode(vm, c)) return 1; return prop_bool(vm, c, "focusEnabled", 0); }
    return 1;
}
static int focusable_by_mouse(MovieClip *c) { return is_text(c) && (tf_editable(c) || tf_selectable(c)); }

static int highlight_enabled(VM *vm, MovieClip *c) {
    if (clip_swf_version(c) >= 6 && c->a1 && c->a1->focus_rect >= 0) return c->a1->focus_rect;
    return vm->stage_focus_rect;
}
static int highlightable(VM *vm, MovieClip *c) {
    if (is_text(c)) return 0;
    if (is_mc(c) && clip_is_root(c)) return 0;
    return highlight_enabled(vm, c);
}

/* world bounds used for the highlight and the automatic tab order */
static int highlight_bounds(MovieClip *c, SWFRect *out) {
    extern void dg_union(SWFRect *acc, int *any, const SWFRect *r, const SWFMatrix *m);
    extern int dg_dobj_bounds(DisplayObject *d, const SWFMatrix *m, SWFRect *out);
    SWFMatrix w; SWFRect l; int any = 0, any2 = 0;
    clip_world_matrix(c, &w);
    l.xmin = l.ymin = l.xmax = l.ymax = 0;
    if (is_button(c)) {
        DisplayObject *d;
        for (d = c->a1->btn_hit; d; d = d->next) {
            SWFRect r;
            if (dg_dobj_bounds(d, &d->matrix, &r)) {
                SWFMatrix id; swf_matrix_identity(&id);
                dg_union(&l, &any, &r, &id);
            }
        }
    } else dg_local_bounds(c, &l, &any);
    if (!any) { out->xmin = out->ymin = out->xmax = out->ymax = 0; return 0; }
    dg_union(out, &any2, &l, &w);
    return 1;
}

static int calc_highlight(VM *vm) {
    MovieClip *f = vm->player->focus; SWFRect r;
    if (!f) return HL_INACTIVE;
    if (!highlightable(vm, f)) return HL_HIDDEN;
    if (!highlight_bounds(f, &r) || (r.xmin == r.xmax && r.ymin == r.ymax)) return HL_HIDDEN;
    return HL_VISIBLE;
}

static void roll_over(VM *vm, MovieClip *n) {
    MovieClip *old = vm->m_hovered;
    vm->m_hovered = n;
    if (old && !old->dead) handle_event(vm, old, EV_ROLL_OUT, 0);
    if (n) handle_event(vm, n, EV_ROLL_OVER, 0);
}

static void focus_handler(VM *vm, MovieClip *c, int focused, MovieClip *other) {
    Act a; Value v;
    if (!c->obj) return;
    act_init_nothing(&a, vm, c, "[Focus]");
    v = other && other->obj ? v_obj((AObj*)other->obj) : v_null();
    o_call_method(&a, (AObj*)c->obj, focused ? vm->s_onSetFocus : vm->s_onKillFocus, 1, &v, ER_SPECIAL);
    if (vm->err) vm_report_error(&a);
}

/* AVM1 side of a focus change: the focus handlers, then Selection.onSetFocus */
static void focus_change_notify(VM *vm, MovieClip *old, MovieClip *n) {
    if (old && !old->dead && old->obj) {
        if (is_text(old)) { struct TextFieldState *t; { extern void a1_tf_ime_commit_pending(VM *vm, MovieClip *c); a1_tf_ime_commit_pending(vm, old); } t = a1_clip_textfield(old); if (t) { t->sel_begin = t->sel_end = 0; t->has_sel = 0; } }
        focus_handler(vm, old, 0, n);
    }
    if (n && n->obj) focus_handler(vm, n, 1, old);
    {
        Act a; Value args[2]; AObj *sel; int had = 0;
        act_init_nothing(&a, vm, avm1_base_root(vm), "[System Listeners]");
        args[0] = old && old->obj ? v_obj((AObj*)old->obj) : v_null();
        args[1] = n && n->obj ? v_obj((AObj*)n->obj) : v_null();
        sel = v_obj_of(&a, o_get(&a, act_global(&a), str_intern(vm, "Selection")));
        if (!vm->err && sel) broadcast_message(&a, sel, vm->s_onSetFocus, 2, args, &had);
        if (vm->err) vm_report_error(&a);
    }
}
/* AVM1 content inside an AVM2 movie (AVM1Movie): one focus is shared with the AVM2 side
   (Ruffle's FocusTracker); p->focus may then hold an AVM2 text field */
static int focus_mixed(VM *vm) { return vm->player->avm2 && vm->nmix > 0; }
static MovieClip *a1_focus(VM *vm) { MovieClip *f = vm->player->focus; if (f && focus_mixed(vm) && !CLIP_A1MIX(f)) return NULL; return f; }
extern int avm2_mixed_has_focus(Player *p);
extern void avm2_mixed_focus_to_avm1(Player *p);
extern int avm2_mixed_focus_change_event(Player *p, int by_key);
static void set_internal(VM *vm, MovieClip *n, int run_actions) {
    Player *p = vm->player; MovieClip *old; int had2;
    roll_over(vm, n);
    if (run_actions) avm1_run_actions(vm);
    old = a1_focus(vm);
    had2 = focus_mixed(vm) && avm2_mixed_has_focus(p);
    if (old != n || had2) {
        p->focus = n; p->caret_blink = 16;
        vm->highlight = calc_highlight(vm);
        focus_change_notify(vm, old, n);
        if (had2) avm2_mixed_focus_to_avm1(p);   /* the AVM2 object loses the focus (focusOut) */
    }
}
/* the AVM2 side took the focus from an AVM1 object (or moved it between AVM2 objects) */
void avm1_mixed_focus_from_avm2(Player *p, MovieClip *old1) {
    VM *vm = VMP(p);
    if (!vm || !vm->nmix) return;
    vm->highlight = HL_INACTIVE;
    focus_change_notify(vm, old1, NULL);
}
int avm1_clip_focusable_by_mouse(MovieClip *c) { return c && focusable_by_mouse(c); }

static void select_all_focused(VM *vm) {
    MovieClip *f = vm->player->focus;
    if (f && is_text(f)) { struct TextFieldState *t = a1_clip_textfield(f); if (t) { t->sel_begin = 0; t->sel_end = t->rt.len; t->has_sel = 1; } }
}

/* Selection.setFocus and friends */
void avm1_focus_set(VM *vm, MovieClip *c) { set_internal(vm, c, 0); select_all_focused(vm); }
int  avm1_focus_set_checked(VM *vm, MovieClip *c) { if (!c || !is_focusable(vm, c)) return 0; avm1_focus_set(vm, c); return 1; }

static void focus_by_mouse(VM *vm, MovieClip *c) {
    if (vm->player->focus == c) return;
    if (focus_mixed(vm) && avm2_mixed_focus_change_event(vm->player, 0)) return;   /* mouseFocusChange on the AVM2 focus, cancelable */
    if (c && !focusable_by_mouse(c)) c = NULL;
    set_internal(vm, c, 0);
}

static void focus_by_key(VM *vm, MovieClip *c) { set_internal(vm, c, 1); select_all_focused(vm); }

/* tab order */
typedef struct TabList { MovieClip **o; int n, cap, custom; } TabList;

static void tl_add(TabList *t, MovieClip *c) {
    int has = has_tab(c);
    if (has && !t->custom) {
        int i, k = 0;
        t->custom = 1;
        for (i = 0; i < t->n; i++) if (has_tab(t->o[i])) t->o[k++] = t->o[i];
        t->n = k;
    }
    if (!has && t->custom) return;
    if (t->n == t->cap) { int nc = t->cap ? t->cap * 2 : 32; MovieClip **no = (MovieClip**)realloc(t->o, (size_t)nc * sizeof(MovieClip*)); if (!no) return; t->o = no; t->cap = nc; }
    t->o[t->n++] = c;
}

static void fill_tab(VM *vm, MovieClip *c, TabList *t) {
    DisplayObject *d;
    if (is_mc(c) && !prop_bool(vm, c, "tabChildren", 1)) return;
    for (d = c->children; d; d = d->next) {
        if (!d->visible || d->depth < 0) continue;
        if (d->clip && d->clip->a1 && !d->clip->dead) {
            if (is_tabbable(vm, d->clip)) tl_add(t, d->clip);
            if (!is_text(d->clip)) fill_tab(vm, d->clip, t);
        }
    }
}

static long tab_key(MovieClip *c) {
    SWFRect r; highlight_bounds(c, &r);
    return (long)r.ymin * 6 + (long)r.xmin;
}

static void tab_order(VM *vm, TabList *t) {
    Player *p = vm->player; DisplayObject *d; int i, j;
    memset(t, 0, sizeof(*t));
    if (p->avm2) { int k; for (k = 0; k < vm->nmix; k++) fill_tab(vm, vm->mix[k], t); }
    else if (p->root->self && !p->root->self->visible) {} else fill_tab(vm, p->root, t);
    for (d = p->levels; d; d = d->next) if (d->clip && d->visible) fill_tab(vm, d->clip, t);
    /* stable insertion sort */
    if (t->custom) {
        for (i = 1; i < t->n; i++) { MovieClip *x = t->o[i]; j = i - 1; while (j >= 0 && tab_index(t->o[j]) > tab_index(x)) { t->o[j + 1] = t->o[j]; j--; } t->o[j + 1] = x; }
    } else {
        long *keys = (long*)malloc((size_t)(t->n ? t->n : 1) * sizeof(long)); int k = 0;
        if (!keys) return;
        for (i = 0; i < t->n; i++) keys[i] = tab_key(t->o[i]);
        for (i = 1; i < t->n; i++) { MovieClip *x = t->o[i]; long kx = keys[i]; j = i - 1; while (j >= 0 && keys[j] > kx) { t->o[j + 1] = t->o[j]; keys[j + 1] = keys[j]; j--; } t->o[j + 1] = x; keys[j + 1] = kx; }
        for (i = 0; i < t->n; i++) if (i == 0 || keys[i] != keys[i - 1]) { t->o[k] = t->o[i]; keys[k] = keys[i]; k++; }
        t->n = k;
        free(keys);
    }
}

/* mixed content: AVM1 tab stops (automatic order keys) and focusing one by key */
int avm1_mix_tab_entries(Player *p, MovieClip **out, long *keys, int max) {
    VM *vm = VMP(p); TabList t; int i, n = 0;
    if (!vm || !vm->nmix) return 0;
    tab_order(vm, &t);
    if (t.custom) { free(t.o); return -1; }
    for (i = 0; i < t.n && n < max; i++) { out[n] = t.o[i]; keys[n] = tab_key(t.o[i]); n++; }
    free(t.o);
    return n;
}
void avm1_mix_focus_by_key(Player *p, MovieClip *c) { VM *vm = VMP(p); if (vm) { focus_by_key(vm, c); vm->highlight = calc_highlight(vm); } }
static void focus_cycle(VM *vm, int reverse) {
    TabList t; int i, cur = -1; MovieClip *next = NULL;
    tab_order(vm, &t);
    if (t.n) {
        for (i = 0; i < t.n; i++) if (t.o[i] == vm->player->focus) { cur = i; break; }
        if (cur < 0) next = reverse ? t.o[t.n - 1] : t.o[0];
        else if (reverse) next = cur > 0 ? t.o[cur - 1] : t.o[t.n - 1];
        else next = cur + 1 < t.n ? t.o[cur + 1] : t.o[0];
    }
    free(t.o);
    if (next) { focus_by_key(vm, next); vm->highlight = calc_highlight(vm); }
}

static long nav_key(const SWFRect *o, const SWFRect *r, int dir, int *cat) {
    long dx, dy;
    *cat = 2;
    switch (dir) {
    case 40:  /* down */
        if (r->ymax <= o->ymax) return -1;
        if (r->xmax >= o->xmin && r->xmin <= o->xmax) { *cat = 0; return (long)r->ymin - o->ymin; }
        if (r->ymin <= o->ymax) { long a1 = (long)o->xmax - r->xmin, b1 = (long)r->xmax - o->xmin; *cat = 1; return a1 > b1 ? a1 : b1; }
        break;
    case 38:  /* up */
        if (r->ymax >= o->ymax) return -1;
        if (r->xmax >= o->xmin && r->xmin <= o->xmax) { *cat = 0; return (long)o->ymax - r->ymax; }
        break;
    case 39:  /* right */
        if (r->xmax <= o->xmax) return -1;
        if (r->ymax >= o->ymin && r->ymin <= o->ymax) { *cat = 0; return (long)r->xmin - o->xmin; }
        break;
    case 37:  /* left */
        if (r->xmin >= o->xmin) return -1;
        if (r->ymax >= o->ymin && r->ymin <= o->ymax) { *cat = 0; return (long)o->xmax - r->xmax; }
        break;
    }
    { long a1 = (long)o->xmax - r->xmin, b1 = (long)r->xmax - o->xmin; dx = a1 > b1 ? a1 : b1; }
    { long a1 = (long)o->ymax - r->ymin, b1 = (long)r->ymax - o->ymin; dy = a1 > b1 ? a1 : b1; }
    return dx * dx + dy * dy;
}

static void focus_navigate(VM *vm, int dir) {
    TabList t; int i, best = -1, bcat = 3; long bkey = 0; SWFRect o;
    MovieClip *f = vm->player->focus;
    if (!f) return;
    highlight_bounds(f, &o);
    memset(&t, 0, sizeof(t));
    { MovieClip *roots[64]; int nr = avm1_stage_roots(vm, roots, 64), k; for (k = 0; k < nr; k++) fill_tab(vm, roots[k], &t); }
    for (i = 0; i < t.n; i++) {
        SWFRect r; int cat; long k;
        highlight_bounds(t.o[i], &r);
        k = nav_key(&o, &r, dir, &cat);
        if (k < 0 && cat != 2) continue;
        if (k < 0) continue;
        if (best < 0 || cat < bcat || (cat == bcat && k < bkey)) { best = i; bcat = cat; bkey = k; }
    }
    if (best >= 0) focus_by_key(vm, t.o[best]);
    free(t.o);
}

int avm1_highlight_active(VM *vm) { return vm->highlight != HL_INACTIVE; }
/* SWF 8 and older: right button presses and releases reset the highlight too (Ruffle should_reset_highlight) */
void avm1_reset_highlight(Player *p) { VM *vm = VMP(p); if (vm) vm->highlight = HL_INACTIVE; }

/* the yellow focus rectangle */
void player_draw_focus_rect(Player *p, const SWFRect *rr, const SWFMatrix *sm);
void avm1_render_highlight(Player *p, const SWFMatrix *sm) {
    VM *vm = VMP(p); SWFRect r;
    if (!vm || p->avm2 || vm->highlight != HL_VISIBLE || !p->focus || p->focus->dead) return;
    if (!highlight_bounds(p->focus, &r)) return;
    player_draw_focus_rect(p, &r, sm);
}
/* the focus highlight: a 3 px yellow outline inside the object's bounds in
   device space (Ruffle's draw_rect_outline) */
void player_draw_focus_rect(Player *p, const SWFRect *rr, const SWFMatrix *sm) {
    ShapeDef s; ShapeEdge ed[16]; FillStyle fs; SWFCxform cx; SWFMatrix dm; int i, k = 0; double t = 60, xs[4], ys[4], x0, y0, x1, y1;
    memset(&s, 0, sizeof(s)); memset(&fs, 0, sizeof(fs)); swf_matrix_identity(&fs.matrix);
    fs.type = FILL_SOLID; fs.color.r = 255; fs.color.g = 255; fs.color.b = 0; fs.color.a = 255;
    s.fills = &fs; s.nfills = 1; s.edges = ed; s.nonzero = 1;   /* four rectangles drawn one over another */
    /* the bounds in device twips */
    swf_matrix_apply(sm, rr->xmin, rr->ymin, &xs[0], &ys[0]); swf_matrix_apply(sm, rr->xmax, rr->ymin, &xs[1], &ys[1]);
    swf_matrix_apply(sm, rr->xmin, rr->ymax, &xs[2], &ys[2]); swf_matrix_apply(sm, rr->xmax, rr->ymax, &xs[3], &ys[3]);
    x0 = x1 = xs[0]; y0 = y1 = ys[0];
    for (i = 1; i < 4; i++) { if (xs[i] < x0) x0 = xs[i]; if (xs[i] > x1) x1 = xs[i]; if (ys[i] < y0) y0 = ys[i]; if (ys[i] > y1) y1 = ys[i]; }
    x0 = floor(x0 * 20 + 0.5); y0 = floor(y0 * 20 + 0.5); x1 = floor(x1 * 20 + 0.5); y1 = floor(y1 * 20 + 0.5);
    {
        double rx[4][4];
        rx[0][0] = x0; rx[0][1] = y0; rx[0][2] = x1; rx[0][3] = y0 + t;
        rx[1][0] = x0; rx[1][1] = y1 - t; rx[1][2] = x1; rx[1][3] = y1;
        rx[2][0] = x0; rx[2][1] = y0; rx[2][2] = x0 + t; rx[2][3] = y1;
        rx[3][0] = x1 - t; rx[3][1] = y0; rx[3][2] = x1; rx[3][3] = y1;
        for (i = 0; i < 4; i++) {
            s32 a0 = (s32)rx[i][0], b0 = (s32)rx[i][1], a1 = (s32)rx[i][2], b1 = (s32)rx[i][3]; int j;
            for (j = 0; j < 4; j++) { memset(&ed[k + j], 0, sizeof(ShapeEdge)); ed[k + j].fill1 = 1; }
            ed[k].x0 = a0; ed[k].y0 = b0; ed[k].x1 = a1; ed[k].y1 = b0;
            ed[k + 1].x0 = a1; ed[k + 1].y0 = b0; ed[k + 1].x1 = a1; ed[k + 1].y1 = b1;
            ed[k + 2].x0 = a1; ed[k + 2].y0 = b1; ed[k + 2].x1 = a0; ed[k + 2].y1 = b1;
            ed[k + 3].x0 = a0; ed[k + 3].y0 = b1; ed[k + 3].x1 = a0; ed[k + 3].y1 = b0;
            k += 4;
        }
    }
    s.nedges = k;
    swf_cxform_identity(&cx);
    swf_matrix_identity(&dm); dm.sx = dm.sy = 1.0 / 20.0;   /* device twips -> px */
    render_shape(p->surf, p->movie, &s, &dm, &cx, NULL);
}

/* the player window lost the keyboard focus (FocusTracker::reset_focus) */
void avm1_focus_lost(Player *p) { VM *vm = VMP(p); if (!vm || (p->avm2 && !vm->nmix)) return; set_internal(vm, NULL, 1); avm1_run_actions(vm); }
void avm1_player_set_focus(Player *p, MovieClip *c) { VM *vm = VMP(p); if (vm) avm1_focus_set(vm, c); else p->focus = c; }
