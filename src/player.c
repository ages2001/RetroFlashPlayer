#include "player.h"
#include "reader.h"
#include "shape.h"
#include "render.h"
#include "avm1.h"
#include "avm2.h"
#include "chars.h"
#include "audio.h"
#include "bmd.h"
#include "filters.h"
#include "render_filters.h"
#include "pbj.h"
#include <stdlib.h>
#define VM_FOCUS_HOOK(p, c) avm1_focus_changed(p, c)
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ---------------------------------------------------------------- */
/* display list                                                     */

/* AS3 timelines keep a depth list apart from the render list (Ruffle ChildContainer):
   timeline tags address objects by depth, scripts by render position */
#define DL_MODE(c) ((c)->player->avm2 && (c)->movie && (c)->movie->has_abc)
DisplayObject *clip_find_depth(MovieClip *c, int depth) {
    DisplayObject *d;
    if (DL_MODE(c)) { for (d = c->children; d; d = d->next) if (d->in_dl && d->tl_depth == depth) return d; return NULL; }
    for (d = c->children; d; d = d->next) if (d->depth == depth) return d;
    return NULL;
}

static void clip_retire(MovieClip *c);
static void clip_unload_tree(MovieClip *c);

/* unlink + retire: memory is released by player_flush_graveyard() */
static void dobj_free(DisplayObject *d) {
    Player *p = d->owner ? d->owner->player : (d->clip ? d->clip->player : NULL);
    if (p && p->avm2 && avm2_dobj_removed(p, d)) return;      /* script keeps the object */
    if (d->clip) clip_retire(d->clip);
    if (p) { d->next = p->graveyard; p->graveyard = d; }
    else free(d);
}

static void dobj_really_free(DisplayObject *d);

static void clip_really_free(MovieClip *c) {
    DisplayObject *d = c->children;
    audio_forget_dobj(c->player, c);
    avm1_clip_detach(c->player, c);
    avm2_clip_detach(c);
    while (d) { DisplayObject *n = d->next; dobj_really_free(d); d = n; }
    if (c->owns_tl && c->tl) { timeline_free(c->tl); free(c->tl); }
    free(c);
}
static void dobj_really_free(DisplayObject *d) {
    avm2_dobj_detach(d);
    if (d->clip) clip_really_free(d->clip);
    if (d->bmd) { bmd_unref(d->bmd); free(d->ch); }
    free(d->fcache_px);
    swf_filters_free(d->filters, d->nfilters);
    pbj_job_unref((PbjJob*)d->blend_shader);
    free(d);
}
/* a record owned by an AVM2 object that is being collected */
void player_destroy_detached(DisplayObject *d) {
    if (d->clip) { clip_retire(d->clip); clip_unload_tree(d->clip); }
    dobj_really_free(d);
}

static void clip_unload_tree(MovieClip *c);

/* fire unload for everything removed since the last call, keep it one
 * more tick, free what was already unloaded */
void player_flush_graveyard(Player *p) {
    DisplayObject *d = p->graveyard_old;
    p->graveyard_old = NULL;
    while (d) { DisplayObject *n = d->next; dobj_really_free(d); d = n; }
    d = p->graveyard;
    p->graveyard = NULL;
    while (d) {
        DisplayObject *n = d->next;
        if (d->clip) clip_unload_tree(d->clip);
        d->next = p->graveyard_old; p->graveyard_old = d;
        d = n;
    }
}

static void clip_remove_depth(MovieClip *c, int depth) {
    DisplayObject **pp = &c->children;
    if (DL_MODE(c)) {
        DisplayObject *d = clip_find_depth(c, depth);
        if (!d) return;
        if (d->placed_by_script) { d->in_dl = 0; return; }   /* moved by script: only leaves the depth list */
        for (; *pp; pp = &(*pp)->next) if (*pp == d) { *pp = d->next; dobj_free(d); return; }
        return;
    }
    while (*pp) {
        if ((*pp)->depth == depth) {
            DisplayObject *d = *pp;
            if (CLIP_IS_A1(c)) { avm1_remove_child(c->player, c, d); return; }   /* AVM1: unload handlers may delay it */
            *pp = d->next;
            dobj_free(d);
            return;
        }
        pp = &(*pp)->next;
    }
}

/* unlink a child and retire it (memory is released after the graveyard) */
void player_unlink_dobj(MovieClip *c, DisplayObject *d) {
    DisplayObject **pp = &c->children;
    while (*pp) {
        if (*pp == d) { *pp = d->next; d->next = NULL; dobj_free(d); return; }
        pp = &(*pp)->next;
    }
}

void clip_remove_all(MovieClip *c) {
    DisplayObject *d = c->children;
    while (d) { DisplayObject *n = d->next; dobj_free(d); d = n; }
    c->children = NULL;
}

static void clip_insert(MovieClip *c, DisplayObject *d) {
    DisplayObject **pp = &c->children;
    while (*pp && (*pp)->depth < d->depth) pp = &(*pp)->next;
    d->next = *pp;
    *pp = d;
}

/* AS3: a new timeline object goes into the render list just below the next
   object of the depth list, or at the end (Ruffle replace_at_depth) */
static void clip_insert_timeline(MovieClip *c, DisplayObject *nd, int depth) {
    DisplayObject *k, *above = NULL, *prev = NULL, *last = NULL;
    nd->tl_depth = depth; nd->in_dl = 1; nd->tl_inst = 1;
    for (k = c->children; k; k = k->next) { if (k->in_dl && k->tl_depth > depth && (!above || k->tl_depth < above->tl_depth)) above = k; last = k; }
    if (!above) { nd->depth = (last && last->depth >= depth) ? last->depth + 1 : depth; clip_insert(c, nd); return; }
    for (k = c->children; k && k != above; k = k->next) prev = k;
    if (prev && prev->depth + 1 >= above->depth) for (k = above; k; k = k->next) k->depth++;
    if ((!prev || depth > prev->depth) && depth < above->depth) nd->depth = depth;
    else nd->depth = prev ? prev->depth + 1 : above->depth - 1;
    nd->next = above;
    if (prev) prev->next = nd; else c->children = nd;
}
/* exported for the AVM1 binding */
void player_free_dobj(DisplayObject *d) { dobj_free(d); }
void player_insert_dobj(MovieClip *c, DisplayObject *d) { clip_insert(c, d); }
void player_resort(MovieClip *c) {
    /* insertion re-sort by depth (lists are short) */
    DisplayObject *sorted = NULL, *d = c->children;
    while (d) {
        DisplayObject *n = d->next, **pp = &sorted;
        while (*pp && (*pp)->depth < d->depth) pp = &(*pp)->next;
        d->next = *pp; *pp = d;
        d = n;
    }
    c->children = sorted;
}

/* ---------------------------------------------------------------- */
/* MovieClip instances                                              */

static Timeline *sprite_timeline(SWFChar *ch) {
    Timeline *tl;
    if (ch->parsed) return (Timeline*)ch->parsed;
    tl = (Timeline*)calloc(1, sizeof(Timeline));
    if (!tl) return NULL;
    swf_sprite_timeline(ch, tl);
    ch->parsed = tl;
    return tl;
}

static MovieClip *clip_create(Player *p, MovieClip *parent, Timeline *tl, DisplayObject *self) {
    MovieClip *c = (MovieClip*)calloc(1, sizeof(MovieClip));
    if (!c) return NULL;
    c->player = p;
    c->movie = (self && self->ch && self->ch->movie) ? self->ch->movie : (parent ? parent->movie : p->movie);
    c->tl = tl;
    c->cur_frame = -1; c->queued_goto = -1;
    c->tag_cursor = 0;
    c->playing = 1;
    c->frame_count = tl ? tl->frame_count : 0;
    c->parent = parent;
    c->self = self;
    avm1_clip_created(p, c);
    return c;
}

static void run_first_frame(MovieClip *c);
MovieClip *player_make_clip(Player *p, MovieClip *parent, SWFChar *ch, DisplayObject *self) {
    Timeline *tl = ch ? sprite_timeline(ch) : NULL;
    return clip_create(p, parent, tl, self);
}
void player_run_first_frame(MovieClip *c) { run_first_frame(c); }
/* main timeline of a loaded SWF; the player keeps the movie until it is freed */
/* a clip running a movie's main timeline (script-created instance of a document class) */
MovieClip *player_make_root_instance(Player *p, SWFMovie *m, DisplayObject *self) {
    MovieClip *c = clip_create(p, NULL, &m->root, self);
    if (c) { c->movie = m; c->root_copy = 1; }
    return c;
}
/* ---------------------------------------------------------------- */
/* AVM1 levels and loaded movies                                    */

MovieClip *player_level(Player *p, int level) {
    DisplayObject *d;
    if (level == 0) return p->root;
    for (d = p->levels; d; d = d->next) if (d->depth == level) return d->clip;
    return NULL;
}

/* an empty movie (no tags): the state of a clip while its movie loads,
 * after an error (version 0, no frames) or after unloadMovie */
SWFMovie *player_stub_movie(Player *p, int version, const char *url, int frames) {
    extern void player_keep_movie(Player *p, SWFMovie *m);
    SWFMovie *m = (SWFMovie*)calloc(1, sizeof(SWFMovie));
    if (!m) return NULL;
    m->version = (u8)version;
    m->maxid = -1;
    m->a1mix = p->avm2 != NULL;   /* only AVM1 loads use stub movies */
    m->framerate = p->movie ? p->movie->framerate : 12;
    m->framecount = frames;
    m->root.frame_count = frames;
    if (url) { m->url = (char*)malloc(strlen(url) + 1); if (m->url) strcpy(m->url, url); }
    player_keep_movie(p, m);
    return m;
}

MovieClip *player_level_create(Player *p, int level, SWFMovie *m) {
    DisplayObject *d, **pp; MovieClip *c;
    if (level == 0) return p->root;
    if ((c = player_level(p, level)) != NULL) return c;
    d = (DisplayObject*)calloc(1, sizeof(DisplayObject));
    if (!d) return NULL;
    d->depth = level; d->char_id = -1; d->visible = 1;
    swf_matrix_identity(&d->matrix); swf_cxform_identity(&d->cxform);
    c = clip_create(p, NULL, NULL, d);
    if (!c) { free(d); return NULL; }
    c->movie = m ? m : p->movie;
    c->tl = (Timeline*)calloc(1, sizeof(Timeline));
    if (c->tl) { c->owns_tl = 1; c->tl->frame_count = 1; }
    c->frame_count = 1;
    d->clip = c;
    for (pp = &p->levels; *pp && (*pp)->depth < level; pp = &(*pp)->next) ;
    d->next = *pp; *pp = d;
    return c;
}

/* an AVM1 _level inside the Loader of an AVM1Movie (no AVM2 object; depth order) */
MovieClip *player_mix_level_create(Player *p, MovieClip *stage, int level, SWFMovie *m) {
    DisplayObject *d; MovieClip *c;
    d = (DisplayObject*)calloc(1, sizeof(DisplayObject));
    if (!d) return NULL;
    d->depth = level; d->char_id = -1; d->visible = 1; d->owner = stage;
    swf_matrix_identity(&d->matrix); swf_cxform_identity(&d->cxform);
    c = clip_create(p, stage, NULL, d);
    if (!c) { free(d); return NULL; }
    c->movie = m;
    c->tl = (Timeline*)calloc(1, sizeof(Timeline));
    if (c->tl) { c->owns_tl = 1; c->tl->frame_count = 1; }
    c->frame_count = 1;
    d->clip = c;
    clip_insert(stage, d);
    return c;
}

/* MovieClip::replace_with_movie: the clip starts over with another movie
 * (its children are dropped without further events) */
void player_clip_replace_movie(MovieClip *c, SWFMovie *m) {
    DisplayObject *d = c->children;
    while (d) { DisplayObject *n = d->next; d->next = NULL; dobj_free(d); d = n; }
    c->children = NULL;
    d = c->goto_pool;
    while (d) { DisplayObject *n = d->next; d->next = NULL; dobj_free(d); d = n; }
    c->goto_pool = NULL;
    if (c->owns_tl && c->tl) { timeline_free(c->tl); free(c->tl); }
    c->movie = m;
    c->tl = &m->root; c->owns_tl = 0;
    c->frame_count = m->root.frame_count;
    c->cur_frame = -1; c->tag_cursor = 0;
    c->playing = 1;
    c->goto_busy = 0;
}

void player_keep_movie(Player *p, SWFMovie *m) {
    SWFMovie **nl = (SWFMovie**)realloc(p->loaded, (size_t)(p->nloaded + 1) * sizeof(SWFMovie*));
    if (!nl) return;
    p->loaded = nl; p->loaded[p->nloaded++] = m;
}
MovieClip *player_make_movie_clip(Player *p, SWFMovie *m, DisplayObject *self) {
    MovieClip *c;
    player_keep_movie(p, m);
    c = clip_create(p, NULL, &m->root, self);
    if (c) c->movie = m;
    return c;
}

static void clip_retire(MovieClip *c) {
    DisplayObject *d;
    if (!c || c->dead) return;
    if (c->player->focus == c) c->player->focus = NULL;
    c->dead = 1;
    c->playing = 0;
    for (d = c->children; d; d = d->next) if (d->clip) clip_retire(d->clip);
}

/* next tick: dispatch unload events, then detach script objects */
static void clip_unload_tree(MovieClip *c) {
    DisplayObject *d;
    if (!c) return;
    avm1_clip_unload(c->player, c);
    for (d = c->children; d; d = d->next) if (d->clip) clip_unload_tree(d->clip);
    avm1_clip_destroyed(c->player, c);
}

int clip_current_frame(const MovieClip *c) { return c->cur_frame < 0 ? 0 : c->cur_frame; }

MovieClip *clip_find_child(MovieClip *c, const char *name) {
    DisplayObject *d;
    for (d = c->children; d; d = d->next)
        if (d->clip && d->name[0] && strcmp(d->name, name) == 0) return d->clip;
    return NULL;
}

MovieClip *clip_resolve_path(MovieClip *c, const char *path) {
    char buf[256];
    char *tok, *save;
    MovieClip *cur = c;
    size_t n;
    if (!path || !*path) return c;
    n = strlen(path);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, path, n); buf[n] = 0;
    if (buf[0] == '/') { while (cur->parent) cur = cur->parent; }
    save = buf;
    for (tok = strtok(save, "/."); tok; tok = strtok(NULL, "/.")) {
        if (strcmp(tok, "_root") == 0 || strcmp(tok, "_level0") == 0) {
            while (cur->parent) cur = cur->parent;
        } else if (strcmp(tok, "_parent") == 0 || strcmp(tok, "..") == 0) {
            if (cur->parent) cur = cur->parent;
        } else if (strcmp(tok, "this") == 0) {
            /* stay */
        } else {
            MovieClip *ch = clip_find_child(cur, tok);
            if (!ch) return NULL;
            cur = ch;
        }
    }
    return cur;
}

int clip_find_frame_label(MovieClip *c, const char *label) {
    int i, frame = 0;
    if (!c->tl) return -1;
    for (i = 0; i < c->tl->ntags; i++) {
        const TagRef *t = &c->tl->tags[i];
        if (t->code == TAG_FrameLabel) {
            if (strncmp((const char*)t->data, label, t->len) == 0 &&
                strlen(label) < t->len + 1 && (t->len == 0 || ((const char*)t->data)[strlen(label)] == 0))
                return frame;
        } else if (t->code == TAG_ShowFrame) frame++;
    }
    return -1;
}

/* ---------------------------------------------------------------- */
/* tag execution                                                    */

static void dobj_place_filters(DisplayObject *d, const u8 *data, size_t len) {
    SwfFilter *f = NULL; int n = swf_filters_parse(data, len, &f);
    swf_filters_free(d->filters, d->nfilters);
    d->filters = f; d->nfilters = n;
}
static void skip_filter_list(Reader *r) {
    int n = r_u8(r), i;
    for (i = 0; i < n; i++) {
        int id = r_u8(r);
        switch (id) {
        case 0: r_skip(r, 23); break;                        /* DropShadow */
        case 1: r_skip(r, 9);  break;                        /* Blur */
        case 2: r_skip(r, 15); break;                        /* Glow */
        case 3: r_skip(r, 27); break;                        /* Bevel */
        case 4: case 7: { int nc = r_u8(r); r_skip(r, (size_t)nc * 5 + 19); break; } /* GradientGlow/Bevel */
        case 5: { int mx = r_u8(r), my = r_u8(r); r_skip(r, 8 + (size_t)mx * my * 4 + 4 + 1); break; } /* Convolution */
        case 6: r_skip(r, 80); break;                        /* ColorMatrix */
        default: return;
        }
    }
}

static void run_first_frame(MovieClip *c);

static int g_place_modify_only;
static int g_goto_create;          /* goto commands: a "replace" on an empty depth creates the object */
static void place_object(MovieClip *c, int version, const u8 *data, size_t len) {
    Reader r;
    Player *p = c->player;
    int a2 = CLIP_IS_A2(c);
    unsigned flags = 0, flags2 = 0;
    int depth, char_id = -1, has_char = 0, move = 0;
    DisplayObject *d;
    SWFMatrix mat; SWFCxform cx;
    int has_matrix = 0, has_cx = 0, ratio = -1, clip_depth = -1, visible = -1, blend = -1, cache_bmp = -1, opaque_bg = -1; u32 opaque_rgb = 0;
    const char *name = NULL;
    const u8 *flt = NULL; size_t fltlen = 0;

    reader_init(&r, data, len);
    swf_matrix_identity(&mat); swf_cxform_identity(&cx);

    if (version == 1) {
        char_id = r_u16(&r); depth = r_u16(&r);
        r_matrix(&r, &mat); has_matrix = 1; has_char = 1;
        if (r.pos < r.len) { r_cxform(&r, &cx, 0); has_cx = 1; }
    } else {
        flags = r_u8(&r);
        if (version == 3) flags2 = r_u8(&r);
        depth = r_u16(&r);
        if (version == 3 && ((flags2 & 0x08) || ((flags2 & 0x10) && !(flags & 0x02)))) r_string(&r);
        if (flags & 0x02) { char_id = r_u16(&r); has_char = 1; }
        if (flags & 0x04) { r_matrix(&r, &mat); has_matrix = 1; }
        if (flags & 0x08) { r_cxform(&r, &cx, 1); has_cx = 1; }
        if (flags & 0x10) ratio = r_u16(&r);
        if (flags & 0x20) name = r_string(&r);
        if (flags & 0x40) clip_depth = r_u16(&r);
        if (version == 3) {
            if (flags2 & 0x01) { size_t f0 = r.pos; skip_filter_list(&r); flt = r.buf + f0; fltlen = r.pos - f0; }
            if (flags2 & 0x02) blend = r_u8(&r);          /* blend mode */
            if (flags2 & 0x04) cache_bmp = r.pos < r.len ? r_u8(&r) != 0 : 1;   /* cache as bitmap */
            if (flags2 & 0x20) visible = r_u8(&r);
            if (flags2 & 0x40) { SWFColor bgc; r_rgba(&r, &bgc); opaque_bg = bgc.a > 0; opaque_rgb = ((u32)bgc.r << 16) | ((u32)bgc.g << 8) | bgc.b; }   /* opaque background (alpha 0: none) */
        }
        move = flags & 0x01;
        /* clip actions (flags & 0x80) follow; parsed lazily by AVM1 later */
    }

    if (g_place_modify_only) { has_char = 0; char_id = -1; }   /* goto: re-applying a placement to a surviving object */
    d = clip_find_depth(c, depth);
    /* placing a new character (no move flag) on an occupied depth fails */
    if (has_char && d && !move) {
        /* the debug player's warning (FLASHNT_NO_WARNINGS: a test with log_warnings = false) */
        if (!a2 && !getenv("FLASHNT_NO_WARNINGS")) { char w[64]; sprintf(w, "Warning: Failed to place object at depth %d.", depth); avm1_trace_out(w); }
        return;
    }
    /* AVM1: "replace" with nothing at the depth does nothing (except when a goto creates it) */
    if (has_char && !d && move && !a2 && !g_goto_create) return;
    if (has_char && (!d || move || d->char_id != char_id)) {
        /* new or replaced character at this depth */
        SWFChar *ch = swf_get_char(c->movie, char_id);
        DisplayObject *nd;
        if (!ch) { return; }
        if (ch->type == CHAR_SPRITE) {
            /* a sprite may not (directly or indirectly) contain itself */
            MovieClip *q = c; int lvl = 0;
            for (; q; q = q->parent, lvl++) {
                if (q->self && q->self->char_id == char_id) return;
                if (lvl > 64) return;
            }
        }
        /* AVM2 "replace" (move + character): the existing object stays; shapes,
           morph shapes and static text swap their graphic in place, anything
           else keeps its current content (Flash dispatches no events here) */
        if (d && move) {
            if (!d->clip && d->ch && ch->type == d->ch->type && (ch->type == CHAR_SHAPE || ch->type == CHAR_MORPH || ch->type == CHAR_TEXT)) { d->ch = ch; d->char_id = char_id; }
            d->place_frame = c->cur_frame + 1;
            goto modify;
        }
        /* backward goto: an identical object parked in the pool is kept alive */
        {
            DisplayObject **pp = &c->goto_pool;
            while (*pp) {
                /* AVM2: objects placed after the target frame never survive a rewind */
                if ((*pp)->depth == depth && (*pp)->char_id == char_id && !(c->movie->has_abc && (*pp)->place_frame > c->goto_target)) {
                    nd = *pp; *pp = nd->next; nd->next = NULL;
                    if (d) clip_remove_depth(c, depth);
                    clip_insert(c, nd);
                    if (has_matrix) nd->matrix = mat;
                    if (has_cx) nd->cxform = cx;
                    if (ratio >= 0) nd->ratio = ratio;
                    if (clip_depth >= 0) nd->clip_depth = clip_depth;
                    if (visible >= 0) nd->visible = visible;
                    if (blend >= 0) nd->blend = blend;
                    if (cache_bmp >= 0) nd->cache_bmp = cache_bmp;
                    return;
                }
                pp = &(*pp)->next;
            }
        }
        nd = (DisplayObject*)calloc(1, sizeof(DisplayObject));
        if (!nd) return;
        nd->depth = depth; nd->char_id = char_id; nd->ch = ch; nd->place_frame = c->cur_frame + 1;
        nd->visible = 1; nd->ratio = 0; nd->owner = c;
        swf_matrix_identity(&nd->matrix); swf_cxform_identity(&nd->cxform);
        if (d) {
            /* keep matrix/cxform from the replaced object when not given */
            nd->matrix = d->matrix; nd->cxform = d->cxform;
            strcpy(nd->name, d->name);
            clip_remove_depth(c, depth);
        }
        d = nd;
        if (DL_MODE(c)) clip_insert_timeline(c, d, depth); else clip_insert(c, d);
        if (ch->type == CHAR_SPRITE) {
            Timeline *tl = sprite_timeline(ch);
            d->clip = clip_create(p, c, tl, d);
        } else if (ch->type == CHAR_EDITTEXT) {
            d->clip = clip_create(p, c, NULL, d);
            avm1_bind_textfield(p, d->clip);
        } else if (ch->type == CHAR_BUTTON) {
            d->clip = clip_create(p, c, NULL, d);
            avm1_bind_button(p, d->clip);
        } else if (ch->type == CHAR_VIDEO && !a2) {
            d->clip = clip_create(p, c, NULL, d);
            if (d->clip) avm1_bind_video(p, d->clip);
        }
        if (name) { strncpy(d->name, name, sizeof(d->name) - 1); d->name[sizeof(d->name) - 1] = 0; d->explicit_name = 1; }
        if (a2) { extern void avm2_object_created(Player *p, DisplayObject *d); avm2_object_created(p, d); }
        if (has_matrix) d->matrix = mat;
        if (has_cx) d->cxform = cx;
        if (ratio >= 0) d->ratio = ratio;
        if (clip_depth >= 0) d->clip_depth = clip_depth;
        if (visible >= 0 && (a2 || c->movie->version >= 11)) d->visible = visible;
        if (blend >= 0) d->blend = blend;
        if (cache_bmp >= 0) d->cache_bmp = cache_bmp;
        if (opaque_bg >= 0 && (a2 || c->movie->version >= 11)) { d->has_opaque_bg = opaque_bg; d->opaque_bg = opaque_rgb; }
        if (flt) dobj_place_filters(d, flt, fltlen);
        if (!a2) {
            /* AVM1 (Ruffle instantiate_child): clip events, construction, then the first frame */
            if (flt && d->clip) { extern void a1_place_filters(MovieClip *c, const u8 *data, size_t len); a1_place_filters(d->clip, flt, fltlen); dobj_place_filters(d, flt, fltlen); }   /* the renderer keeps the tag's own flags */
            if ((flags & 0x80) && d->clip && ch->type == CHAR_SPRITE) avm1_clip_actions(p, d->clip, r.buf + r.pos, r.len - r.pos);
            if (d->clip) avm1_post_instantiation(p, d->clip);
            if (d->clip && ch->type == CHAR_SPRITE && !d->clip->dead) avm1_clip_run_frame(p, d->clip);
            return;
        }
        /* parse clip actions now that the object exists */
        if ((flags & 0x80) && d->clip) {
            avm1_clip_actions(p, d->clip, r.buf + r.pos, r.len - r.pos);
        }
        if (d->clip && ch->type == CHAR_SPRITE) { run_first_frame(d->clip); avm1_clip_loaded(p, d->clip); }
        if (a2) avm2_object_placed(p, d);
        return;
    }
    if (!d) return;
modify:
    /* modify existing: objects moved by script ignore the timeline (AVM1) */
    if (a2 && name && !d->name[0]) { strncpy(d->name, name, sizeof(d->name) - 1); }
    if (a2 && clip_depth >= 0) d->clip_depth = clip_depth;
    if (d->script_moved) return;
    if (!a2 && flt && d->clip) { extern void a1_place_filters(MovieClip *c, const u8 *data, size_t len); a1_place_filters(d->clip, flt, fltlen); }
    if (has_matrix) { d->matrix = mat; d->sr_cached = 0; }
    if (has_cx) d->cxform = cx;
    if (ratio >= 0) d->ratio = ratio;
    if (visible >= 0 && (a2 || c->movie->version >= 11)) d->visible = visible;
    if (blend >= 0) d->blend = blend;
    if (cache_bmp >= 0) d->cache_bmp = cache_bmp;
    if (opaque_bg >= 0 && (a2 || c->movie->version >= 11)) { d->has_opaque_bg = opaque_bg; d->opaque_bg = opaque_rgb; }
    if (flt) dobj_place_filters(d, flt, fltlen);
}

/* a new display object for a character (not inserted anywhere yet) */
DisplayObject *player_instantiate(Player *p, MovieClip *parent, SWFChar *ch, int char_id, int depth) {
    DisplayObject *nd = (DisplayObject*)calloc(1, sizeof(DisplayObject));
    if (!nd) return NULL;
    nd->depth = depth; nd->char_id = char_id; nd->ch = ch;
    nd->place_frame = parent ? parent->cur_frame + 1 : 0;
    nd->visible = 1; nd->owner = parent;
    swf_matrix_identity(&nd->matrix); swf_cxform_identity(&nd->cxform);
    if (ch->type == CHAR_SPRITE) nd->clip = clip_create(p, parent, sprite_timeline(ch), nd);
    else if (ch->type == CHAR_EDITTEXT) { nd->clip = clip_create(p, parent, NULL, nd); if (nd->clip) avm1_bind_textfield(p, nd->clip); }
    else if (ch->type == CHAR_BUTTON) { nd->clip = clip_create(p, parent, NULL, nd); if (nd->clip) avm1_bind_button(p, nd->clip); }
    return nd;
}
/* free an object that never was in a display list (button hit areas) */
void player_destroy_dobj(DisplayObject *d) { if (d->clip) clip_retire(d->clip); dobj_really_free(d); }

extern int avm1_button_ready(MovieClip *c);
static void queue_action(MovieClip *c, const u8 *data, size_t len) {
    Player *p = c->player;
    if (p->nqueue == p->capqueue) {
        int nc = p->capqueue ? p->capqueue * 2 : 256;
        QueuedAction *nq = (QueuedAction*)realloc(p->queue, (size_t)nc * sizeof(QueuedAction));
        if (!nq) return;
        p->queue = nq; p->capqueue = nc;
    }
    p->queue[p->nqueue].clip = c; p->queue[p->nqueue].data = data; p->queue[p->nqueue].len = len; p->queue[p->nqueue].kind = 0;
    p->nqueue++;
}
void player_queue_load(Player *p, MovieClip *c) {
    queue_action(c, NULL, 0);
    p->queue[p->nqueue - 1].kind = 1;
}

/* execute tags up to and including the next ShowFrame.
 * returns 0 if the timeline ended before a ShowFrame. */
static int run_frame_tags_in(MovieClip *c, int queue_actions);
static int run_frame_tags(MovieClip *c, int queue_actions) {
    Player *p = c->player; SWFMovie *saved = p->cur_movie; int r;
    p->cur_movie = c->movie;
    r = run_frame_tags_in(c, queue_actions);
    p->cur_movie = saved;
    return r;
}
/* AVM2 gotos aggregate the display-list deltas of the frames they pass
   (Flash never creates objects that would be gone by the target frame) */
typedef struct GotoCmd { int depth, index, frame, ntags, cap; const TagRef **tags; } GotoCmd;
typedef struct GotoList { GotoCmd *cmd; int n, cap, next_index, rewind, from_frame; } GotoList;
static GotoList *g_goto;
static int g_avm1_no_display, g_avm1_no_sounds;
static int tag_place_depth(const TagRef *t, int *action) {
    /* action: 0 modify, 1 place, 2 replace */
    if (t->code == TAG_PlaceObject) { *action = 1; return t->len >= 4 ? t->data[2] | (t->data[3] << 8) : -1; }
    if (t->len < 3) return -1;
    { int f = t->data[0], off = t->code == TAG_PlaceObject3 ? 2 : 1;
      if (t->len < (size_t)off + 2) return -1;
      *action = (f & 2) ? ((f & 1) ? 2 : 1) : 0;
      return t->data[off] | (t->data[off + 1] << 8); }
}
/* the ratio a PlaceObject tag sets (-1: none) */
static int tag_place_ratio(const TagRef *t) {
    Reader r; unsigned flags, flags2 = 0; SWFMatrix m; SWFCxform cx;
    if (t->code == TAG_PlaceObject) return -1;
    reader_init(&r, t->data, t->len);
    flags = r_u8(&r);
    if (t->code == TAG_PlaceObject3) flags2 = r_u8(&r);
    r_u16(&r);
    if (t->code == TAG_PlaceObject3 && ((flags2 & 0x08) || ((flags2 & 0x10) && !(flags & 0x02)))) r_string(&r);
    if (flags & 0x02) r_u16(&r);
    if (flags & 0x04) r_matrix(&r, &m);
    if (flags & 0x08) r_cxform(&r, &cx, 1);
    return (flags & 0x10) ? (int)r_u16(&r) : -1;
}
/* the clip depth a PlaceObject tag sets (-1: none) */
static int tag_place_clip_depth(const TagRef *t) {
    Reader r; unsigned flags, flags2 = 0; SWFMatrix m; SWFCxform cx;
    if (t->code == TAG_PlaceObject) return -1;
    reader_init(&r, t->data, t->len);
    flags = r_u8(&r);
    if (t->code == TAG_PlaceObject3) flags2 = r_u8(&r);
    r_u16(&r);
    if (t->code == TAG_PlaceObject3 && ((flags2 & 0x08) || ((flags2 & 0x10) && !(flags & 0x02)))) r_string(&r);
    if (flags & 0x02) r_u16(&r);
    if (flags & 0x04) r_matrix(&r, &m);
    if (flags & 0x08) r_cxform(&r, &cx, 1);
    if (flags & 0x10) r_u16(&r);
    if (flags & 0x20) r_string(&r);
    return (flags & 0x40) ? (int)r_u16(&r) : -1;
}
static void goto_collect_place(MovieClip *c, const TagRef *t) {
    GotoList *g = g_goto; int action, depth = tag_place_depth(t, &action), i; GotoCmd *cm = NULL;
    if (depth < 0) return;
    for (i = 0; i < g->n; i++) if (g->cmd[i].depth == depth) { cm = &g->cmd[i]; break; }
    if (!cm) {
        if (g->n == g->cap) { int nc = g->cap ? g->cap * 2 : 16; GotoCmd *nn = (GotoCmd*)realloc(g->cmd, (size_t)nc * sizeof(GotoCmd)); if (!nn) return; g->cmd = nn; g->cap = nc; }
        cm = &g->cmd[g->n++]; memset(cm, 0, sizeof(*cm)); cm->depth = depth; cm->index = g->next_index; cm->frame = c->cur_frame + 1;
    }
    g->next_index++;
    if (action != 0) cm->frame = c->cur_frame + 1;
    if (cm->ntags == cm->cap) { int nc = cm->cap ? cm->cap * 2 : 4; const TagRef **nt = (const TagRef**)realloc((void*)cm->tags, (size_t)nc * sizeof(TagRef*)); if (!nt) return; cm->tags = nt; cm->cap = nc; }
    cm->tags[cm->ntags++] = t;
}
static void goto_collect_remove(MovieClip *c, int depth) {
    GotoList *g = g_goto; int i;
    for (i = 0; i < g->n; i++) if (g->cmd[i].depth == depth) { free((void*)g->cmd[i].tags); g->cmd[i] = g->cmd[g->n - 1]; g->n--; break; }
    if (!g->rewind) {
        /* fast-forward: an object that existed before the goto goes right away
           (its events still see the frame being left) */
        int busy = c->goto_busy, tgt = c->goto_target;
        c->goto_target = g->from_frame; c->goto_busy = 1;
        { DisplayObject *d = clip_find_depth(c, depth); Player *p = c->player;
          if (d && d->clip && d->clip->frame_changed && p->nremoved_fs < 32) p->removed_fs[p->nremoved_fs++] = d->clip; }   /* its frame script still runs */
        clip_remove_depth(c, depth);
        c->goto_busy = busy; c->goto_target = tgt;
    }
}
static int run_frame_tags_in(MovieClip *c, int queue_actions) {
    Timeline *tl = c->tl; int a2 = CLIP_IS_A2(c);
    if (!tl) return 0;
    while (c->tag_cursor < tl->ntags) {
        const TagRef *t = &tl->tags[c->tag_cursor++];
        if (g_goto && (t->code == TAG_PlaceObject || t->code == TAG_PlaceObject2 || t->code == TAG_PlaceObject3)) { goto_collect_place(c, t); continue; }
        if (g_goto && (t->code == TAG_RemoveObject || t->code == TAG_RemoveObject2)) {
            int depth = t->code == TAG_RemoveObject ? (t->len >= 4 ? t->data[2] | (t->data[3] << 8) : -1) : (t->len >= 2 ? t->data[0] | (t->data[1] << 8) : -1);
            if (depth >= 0) goto_collect_remove(c, depth);
            continue;
        }
        /* AVM1 goto scans only look at display list tags */
        if (g_goto && !a2 && t->code != TAG_ShowFrame && t->code != TAG_End) continue;
        /* AVM1 re-run of a goto's target frame: no display list tags */
        if (g_avm1_no_display && (t->code == TAG_PlaceObject || t->code == TAG_PlaceObject2 || t->code == TAG_PlaceObject3 || t->code == TAG_RemoveObject || t->code == TAG_RemoveObject2)) continue;
        if (g_avm1_no_sounds && (t->code == TAG_StartSound || t->code == TAG_SoundStreamBlock)) continue;
        switch (t->code) {
        case TAG_PlaceObject:  place_object(c, 1, t->data, t->len); break;
        case TAG_PlaceObject2: place_object(c, 2, t->data, t->len); break;
        case TAG_PlaceObject3: place_object(c, 3, t->data, t->len); break;
        case TAG_RemoveObject: case TAG_RemoveObject2: {
            /* removals inside a forward goto still see the frame being left */
            int busy = c->goto_busy, depth = t->code == TAG_RemoveObject ? (t->len >= 4 ? t->data[2] | (t->data[3] << 8) : -1) : (t->len >= 2 ? t->data[0] | (t->data[1] << 8) : -1);
            c->goto_busy = 0;
            if (depth >= 0) clip_remove_depth(c, depth);
            c->goto_busy = busy;
            break; }
        case TAG_DoAction:
            if (queue_actions) { if (!a2) avm1_queue_do_action(c->player, c, t->data, t->len); else if (!(c->movie && c->movie->has_abc)) queue_action(c, t->data, t->len); }   /* AVM2 movies ignore DoAction */
            break;
        case TAG_DoInitAction:
            /* u16 sprite id then actions: runs immediately, once (AVM1 runs the main
             * timeline's at load; a sprite's run when the sprite plays its frame) */
            if (!a2) {
                if (c != c->player->root && t->len > 2) {
                    int id = t->data[0] | (t->data[1] << 8);
                    SWFChar *sc = swf_get_char(c->movie, id);
                    if (sc && !(sc->flags & 1)) {
                        extern void avm1_run_init_action(Player *p, MovieClip *c, const u8 *code, size_t len);
                        sc->flags |= 1;
                        avm1_run_init_action(c->player, c, t->data + 2, t->len - 2);
                    }
                }
                break;
            }
            if (t->len > 2) {
                int id = t->data[0] | (t->data[1] << 8);
                SWFChar *sc = swf_get_char(c->movie, id);
                if (!sc || !(sc->flags & 1)) {
                    if (sc) sc->flags |= 1;
                    avm1_run(c->player, c, t->data + 2, t->len - 2);
                }
            }
            break;
        case TAG_ScriptLimits:
            if (t->len >= 2 && !a2) avm1_set_script_limits(c->player, t->data[0] | (t->data[1] << 8), t->len >= 4 ? (t->data[2] | (t->data[3] << 8)) : 0);
            break;
        case TAG_SetBackgroundColor:
            if (t->len >= 3 && !CLIP_A1MIX(c) && !c->player->bg_set) { c->player->bg.r = t->data[0]; c->player->bg.g = t->data[1]; c->player->bg.b = t->data[2]; c->player->bg.a = 255; c->player->bg_set = 1; }   /* only the first one counts (Ruffle) */
            break;
        case TAG_StartSound:
            audio_start_sound(c->player, c, t->data, t->len);
            break;
        case TAG_SoundStreamHead: case TAG_SoundStreamHead2:
            audio_stream_head(c->player, c, t->data, t->len);
            break;
        case TAG_SoundStreamBlock:
            audio_stream_block(c->player, c, t->data, t->len);
            break;
        case TAG_DoABC:
            if (!c->root_copy && !CLIP_A1MIX(c) && !t->done) { ((TagRef*)t)->done = 1; avm2_do_abc(c->player, t->data, t->len); }   /* once: a timeline looping back here does not load the code again */
            break;
        case TAG_DoABC1:
            if (!c->root_copy && !CLIP_A1MIX(c) && !t->done) { extern void avm2_do_abc1(Player *p, const u8 *data, size_t len); ((TagRef*)t)->done = 1; avm2_do_abc1(c->player, t->data, t->len); }
            break;
        case TAG_SymbolClass:
            if (!c->root_copy && !CLIP_A1MIX(c) && !t->done && c->movie && c->movie->has_abc) { ((TagRef*)t)->done = 1; avm2_symbol_class(c->player, t->data, t->len); }   /* AVM1 movies ignore SymbolClass */
            break;
        case TAG_ShowFrame:
            if (a2) { extern void avm2_run_deferred_scripts(Player *p); avm2_run_deferred_scripts(c->player); }
            c->cur_frame++;
            if (a2) avm2_frame_shown(c->player, c);
            return 1;
        case TAG_End:
            if (a2) { extern void avm2_run_deferred_scripts(Player *p); avm2_run_deferred_scripts(c->player); }
            return 0;
        default: break;
        }
    }
    return 0;
}

static int tl_has_end(Timeline *tl);
/* advance one frame, looping at the end */
static void clip_advance(MovieClip *c) {
    if (!c->tl || c->tl->ntags == 0) return;
    c->advanced = 1;
    if (!run_frame_tags(c, 1)) {
        /* reached end: loop back to frame 1 */
        if (c->frame_count <= 1) { c->cur_frame = 0; return; }
        if (!tl_has_end(c->tl)) return;   /* a timeline without End tag is still loading: it stays on its last frame (Ruffle NextFrame::Same) */
        if (!c->parent && !c->player->loop) { c->playing = 0; c->cur_frame = c->frame_count - 1; return; }
        c->cur_frame = clip_current_frame(c);
        clip_goto_frame(c, 0, 1);
    }
}

static void run_first_frame(MovieClip *c) {
    c->ran_this_tick = 1;
    run_frame_tags(c, 1);
}

static int place_version(const TagRef *t) { return t->code == TAG_PlaceObject ? 1 : t->code == TAG_PlaceObject2 ? 2 : 3; }
static void goto_run_cmd(MovieClip *c, GotoCmd *cm, int rewind) {
    DisplayObject *d = clip_find_depth(c, cm->depth); int i, action;
    if (d && rewind) {
        /* a survivor keeps its identity; a re-placement resets its transform */
        tag_place_depth(cm->tags[0], &action);
        if (!d->script_moved) for (i = 0; i < cm->ntags; i++) { tag_place_depth(cm->tags[i], &action); if (action) { swf_matrix_identity(&d->matrix); swf_cxform_identity(&d->cxform); d->ratio = 0; d->blend = 0; d->cache_bmp = 0; if (d->nfilters) dobj_place_filters(d, NULL, 0); } }   /* AVM1: a clip moved by script keeps its transform */
        g_place_modify_only = 1;
        for (i = 0; i < cm->ntags; i++) place_object(c, place_version(cm->tags[i]), cm->tags[i]->data, cm->tags[i]->len);
        g_place_modify_only = 0;
        return;
    }
    for (i = 0; i < cm->ntags; i++) place_object(c, place_version(cm->tags[i]), cm->tags[i]->data, cm->tags[i]->len);
    d = clip_find_depth(c, cm->depth);
    if (d) d->place_frame = cm->frame;
}
static int goto_cmd_cmp(const void *a, const void *b) { return ((const GotoCmd*)a)->index - ((const GotoCmd*)b)->index; }
/* does a timeline object survive a rewinding goto? (Ruffle MovieClip::survives_rewind):
   candidates must be placed again by the replayed frames, with the same
   character (not compared for movie clips), ratio and clip depth */
static int survives_rewind(DisplayObject *d, GotoList *gl, int frame, int as3) {
    int i, action; GotoCmd *cm = NULL; const TagRef *t;
    int candidate = (as3 ? (d->place_frame > frame || d->placed_by_script) : d->depth < 16384) || (d->ch && d->ch->type == CHAR_MORPH);
    int dd = as3 ? d->tl_depth : d->depth;
    if (!candidate) return 1;
    if (as3 && !d->in_dl) return 0;
    for (i = 0; i < gl->n; i++) if (gl->cmd[i].depth == dd) { cm = &gl->cmd[i]; break; }
    if (!cm) return 0;
    if (!as3 && d->script_placed && d->depth < 16384) return 0;
    /* the final placement must place the same character */
    {
        int id = -1, j;
        for (j = 0; j < cm->ntags; j++) {
            t = cm->tags[j];
            tag_place_depth(t, &action);
            if (action != 0) id = t->code == TAG_PlaceObject ? (t->data[0] | (t->data[1] << 8)) : (t->len >= (size_t)(t->code == TAG_PlaceObject3 ? 6 : 5) ? (t->data[t->code == TAG_PlaceObject3 ? 4 : 3] | (t->data[t->code == TAG_PlaceObject3 ? 5 : 4] << 8)) : -1);
        }
        if (!d->clip || (d->ch && d->ch->type != CHAR_SPRITE)) { if (id != d->char_id) return 0; }
    }
    /* ... and the same ratio (a rewound Place defaults it to 0) */
    {
        int ratio = -1, j, r;
        for (j = 0; j < cm->ntags; j++) {
            t = cm->tags[j];
            tag_place_depth(t, &action);
            r = tag_place_ratio(t);
            if (action == 1) ratio = r >= 0 ? r : 0;
            else if (action == 2) ratio = r;
            else if (r >= 0) ratio = r;
        }
        if (ratio >= 0 && ratio != d->ratio) return 0;
    }
    {   /* ... and the same clip depth */
        int cd = -1, j;
        for (j = 0; j < cm->ntags; j++) { int v = tag_place_clip_depth(cm->tags[j]); if (v >= 0) cd = v; }
        if (cd >= 0 && (!d->clip || (d->ch && d->ch->type != CHAR_SPRITE)) && cd != d->clip_depth) return 0;
    }
    (void)frame;
    return 1;
}

static void clip_goto_frame_as3(MovieClip *c, int frame) {
    GotoList gl, *saved = g_goto; int guard = 0, i, rewind = frame < clip_current_frame(c);
    memset(&gl, 0, sizeof(gl)); gl.rewind = rewind; gl.from_frame = clip_current_frame(c);
    c->goto_target = frame; c->goto_busy++;
    if (rewind) { c->tag_cursor = 0; c->cur_frame = -1; }
    g_goto = &gl;
    while (clip_current_frame(c) < frame || c->cur_frame < 0) {
        if (!run_frame_tags(c, 0)) break;
        if (++guard > 100000) break;
    }
    g_goto = saved;
    if (rewind) {
        /* objects placed after the target frame are gone (depth order);
           script-placed objects (depth >= 16384) stay */
        DisplayObject **pp = &c->children;
        while (*pp) {
            DisplayObject *d = *pp;
            if (!survives_rewind(d, &gl, frame, 1)) {
                if (d->placed_by_script) { d->in_dl = 0; pp = &d->next; continue; }   /* script-placed: only leaves the depth list */
                *pp = d->next; d->next = NULL; dobj_free(d); continue; }
            pp = &d->next;
        }
    }
    qsort(gl.cmd, (size_t)gl.n, sizeof(GotoCmd), goto_cmd_cmp);
    for (i = 0; i < gl.n; i++) if (gl.cmd[i].frame < frame) goto_run_cmd(c, &gl.cmd[i], rewind);
    for (i = 0; i < gl.n; i++) if (gl.cmd[i].frame >= frame) goto_run_cmd(c, &gl.cmd[i], rewind);
    for (i = 0; i < gl.n; i++) free((void*)gl.cmd[i].tags);
    free(gl.cmd);
    c->goto_busy--;
    if (c->player->avm2) avm2_goto_shown(c->player, c);   /* construct what the goto placed */
}
/* AVM1 goto (Ruffle MovieClip::run_goto); frame is 0-based */
static void avm1_run_goto(MovieClip *c, int frame, int implicit) {
    GotoList gl, *saved = g_goto; int guard = 0, i, rewind, before = clip_current_frame(c), hit, target_cursor = -1;
    if (!c->tl) return;
    if (frame < 0) frame = 0;
    rewind = frame <= clip_current_frame(c) && c->cur_frame >= 0;
    memset(&gl, 0, sizeof(gl)); gl.rewind = rewind; gl.from_frame = clip_current_frame(c);
    (void)implicit;
    c->goto_target = frame;
    if (rewind) { c->tag_cursor = 0; c->cur_frame = -1; }
    g_goto = &gl;
    while (c->cur_frame < frame) {
        int at = c->tag_cursor;
        if (!run_frame_tags(c, 0)) break;
        target_cursor = at;
        if (++guard > 100000) break;
    }
    g_goto = saved;
    hit = c->cur_frame == frame;
    if (rewind) {
        DisplayObject *d = c->children, *n; DisplayObject *list[1024]; int nl = 0;
        for (; d; d = d->next) if (nl < 1024 && d->depth >= 0) list[nl++] = d;
        for (i = 0; i < nl; i++) {
            for (n = c->children; n && n != list[i]; n = n->next) ;
            if (!n) continue;
            if (!survives_rewind(list[i], &gl, frame, 0)) avm1_remove_child(c->player, c, list[i]);
        }
    }
    qsort(gl.cmd, (size_t)gl.n, sizeof(GotoCmd), goto_cmd_cmp);
    g_goto_create = 1;
    for (i = 0; i < gl.n; i++) if (gl.cmd[i].frame < frame) goto_run_cmd(c, &gl.cmd[i], rewind);
    g_goto_create = 0;
    if (hit && target_cursor >= 0) {
        /* run the target frame again without its display list tags (actions, sounds) */
        int nd = g_avm1_no_display, ns = g_avm1_no_sounds;
        c->cur_frame = frame - 1; c->tag_cursor = target_cursor;
        g_avm1_no_display = 1; g_avm1_no_sounds = frame == before;
        run_frame_tags(c, 1);
        g_avm1_no_display = nd; g_avm1_no_sounds = ns;
    } else if (!hit) {
        if (c->cur_frame < 0) c->cur_frame = 0;
    }
    g_goto_create = 1;
    for (i = 0; i < gl.n; i++) if (gl.cmd[i].frame >= frame) goto_run_cmd(c, &gl.cmd[i], rewind);
    g_goto_create = 0;
    for (i = 0; i < gl.n; i++) free((void*)gl.cmd[i].tags);
    free(gl.cmd);
}

/* a button hit area sprite: its first frame's shapes, without scripts or names */
void player_place_first_frame(MovieClip *c) {
    if (!c->tl || c->cur_frame >= 0) return;
    run_frame_tags(c, 0);
}

/* AVM1 frame advance (Ruffle MovieClip::run_frame_internal) */
static int tl_has_end(Timeline *tl) { int i; for (i = tl->ntags - 1; i >= 0; i--) if (tl->tags[i].code == TAG_End) return 1; return 0; }
void player_avm1_advance(MovieClip *c) {
    int fc;
    if (!c->tl) return;
    fc = c->frame_count < 1 ? 1 : c->frame_count;
    if (c->cur_frame + 1 < fc) { c->advanced = 1; run_frame_tags(c, 1); return; }
    if (fc <= 1 || !tl_has_end(c->tl) || (c->movie && c->movie->incomplete && c->tl == &c->movie->root) || (!c->parent && !c->player->loop)) { c->playing = 0; run_frame_tags(c, 1); return; }   /* a movie still loading waits on its last frame */
    avm1_run_goto(c, 0, 1);
}

/* AVM1 gotoAndPlay / gotoAndStop (frame is 0-based) */
void player_avm1_goto(MovieClip *c, int frame, int play) {
    if (!c->tl) { c->playing = play; return; }
    c->playing = play;
    if (frame < 0) frame = 0;
    if (frame == clip_current_frame(c) && c->cur_frame >= 0) return;   /* no-op goto */
    avm1_run_goto(c, frame, 0);
}

void clip_goto_frame(MovieClip *c, int frame, int play) {
    int guard = 0;
    if (CLIP_IS_A1(c)) { player_avm1_goto(c, frame, play); return; }
    if (!c->tl) return;
    if (c->player->debug) fprintf(stderr, "[goto] clip %p frame %d -> %d (play %d, count %d)\n", (void*)c, clip_current_frame(c) + 1, frame + 1, play, c->frame_count);
    if (frame < 0) frame = 0;
    if (frame >= c->frame_count) frame = c->frame_count - 1;
    if (frame < 0) return;
    c->playing = play;
    if (frame == clip_current_frame(c) && c->cur_frame >= 0) return;
    c->skip_enter = 0;   /* a goto cancels a pending skipped enterFrame (Ruffle run_goto) */
    if (c->movie && c->movie->has_abc && c->cur_frame >= 0) { clip_goto_frame_as3(c, frame); return; }
    c->goto_target = frame; c->goto_busy++;
    if (frame < clip_current_frame(c)) {
        /* park timeline-placed objects; script-created ones (depth >= 16384) stay */
        DisplayObject **pp = &c->children;
        int as3 = c->movie && c->movie->has_abc;
        while (*pp) {
            if ((*pp)->depth < 16384) {
                DisplayObject *d = *pp;
                *pp = d->next;
                /* AVM2: objects placed after the target frame are removed before
                   the timeline is replayed (in depth order) */
                if (as3 && d->place_frame > frame) { d->next = NULL; dobj_free(d); continue; }
                d->next = c->goto_pool; c->goto_pool = d;
            } else pp = &(*pp)->next;
        }
        c->tag_cursor = 0;
        c->cur_frame = -1;
    }
    while (clip_current_frame(c) < frame || c->cur_frame < 0) {
        int last = (c->cur_frame + 1 == frame);
        if (!run_frame_tags(c, last)) break;
        if (++guard > 100000) break;
    }
    /* whatever was not re-placed is gone */
    while (c->goto_pool) { DisplayObject *d = c->goto_pool; c->goto_pool = d->next; dobj_free(d); }
    c->goto_busy--;
}

/* ---------------------------------------------------------------- */
/* tick                                                             */

/* run queued frame actions in FIFO order; actions may queue more */
static void flush_actions(Player *p) {
    int guard = 0;
    while (p->qhead < p->nqueue && guard++ < 100000) {
        QueuedAction qa = p->queue[p->qhead++];
        if (qa.clip->dead) continue;
        if (qa.kind == 1) avm1_clip_load_events(p, qa.clip);
        else avm1_run(p, qa.clip, qa.data, qa.len);
    }
    p->qhead = 0; p->nqueue = 0;
}

/* children first, highest depth first, then this clip's own timeline */
extern int a2_clip_is_movieclip(MovieClip *c);
static void tick_clip(MovieClip *c) {
    DisplayObject *d;
    MovieClip *kids[1024]; int n = 0, i;
    int skip = c->skip_enter;
    if (c->dead || CLIP_A1MIX(c)) return;   /* AVM1 content (AVM1Movie) runs in the AVM1 frame */
    for (d = c->children; d; d = d->next) if (d->clip && n < 1024) kids[n++] = d->clip;
    for (i = n - 1; i >= 0; i--) if (!kids[i]->dead) { if (skip) kids[i]->skip_enter = 1; tick_clip(kids[i]); }
    if (c->dead) return;
    if (skip) { c->skip_enter = 0; return; }   /* a SWF9 goto this frame: the playhead stays */
    if (c->ran_this_tick) { c->ran_this_tick = 0; }
    else if (c->playing && !(CLIP_IS_A2(c) && c->obj2 && c != c->player->root && !a2_clip_is_movieclip(c))) clip_advance(c);   /* a Sprite-class symbol shows its first frame only (the root still plays) */
}

static void clear_ran(MovieClip *c) {
    DisplayObject *d;
    c->ran_this_tick = 0;
    for (d = c->children; d; d = d->next) if (d->clip) clear_ran(d->clip);
}
/* AVM2 orphans (parentless clips) run their timelines too */
void player_tick_orphan(MovieClip *c) { if (!c->dead) tick_clip(c); }
void player_clear_ran(MovieClip *c) { clear_ran(c); }

/* the frame is shown: scrollRects set since the last frame take effect for transforms and hit tests */
static void apply_scroll_rects(MovieClip *c, int depth) {
    DisplayObject *d;
    if (!c || depth > 200) return;
    for (d = c->children; d; d = d->next) { d->has_scroll_applied = d->has_scroll_rect; d->scroll_applied = d->scroll_rect; if (d->clip) apply_scroll_rects(d->clip, depth + 1); }
}
void player_swap_root(Player *p);
int g_dbg_frame;
void player_tick(Player *p) {
    int guard = 0;
    if (p->stopped) return;
    g_dbg_frame++;
    if (!p->avm2) {
        /* AVM1 movies: Flash's AVM1 frame (execution list, action queue, timers) */
        player_flush_graveyard(p);
        avm1_reset_exec_clock(p);
        avm1_frame(p);
        { extern void avm1_input_after_frame(Player *p); avm1_input_after_frame(p); }
        p->tick++;
        p->caret_blink++;
        if (p->swap_root) player_swap_root(p);
        return;
    }
    p->nremoved_fs = 0;
    player_flush_graveyard(p);          /* unload events from last tick's script removals */
    avm1_tick(p);
    if (p->avm2) { extern void avm2_orphans_enter(Player *p); avm2_tick_begin(p); avm2_orphans_enter(p); }
    tick_clip(p->root);
    clear_ran(p->root);
    player_flush_graveyard(p);          /* timeline removals unload before frame scripts */
    if (p->avm2) avm2_tick_entered(p);  /* enterFrame sees the advanced timelines */
    flush_actions(p);
    if (p->avm2) { avm2_tick_end(p); if (p->movie->has_abc) clear_ran(p->root); }   /* AVM2: clips created later this tick advance on the next one */
    { extern int avm1_mix_active(Player *p); extern void avm1_mix_frame(Player *p); extern void avm2_avm1_frame_done(Player *p); if (p->avm2 && avm1_mix_active(p)) { avm1_mix_frame(p); avm2_avm1_frame_done(p); } }   /* AVM1Movie content: the AVM1 frame follows the AVM2 phases */
    { extern void avm2_net_phase(Player *p); if (p->avm2) avm2_net_phase(p); }
    apply_scroll_rects(p->root, 0);
    (void)guard;
    p->tick++;
    p->caret_blink++;
}

/* ---------------------------------------------------------------- */
/* render                                                           */

static void render_clip(Player *p, MovieClip *c, const SWFMatrix *pm, const SWFCxform *pcx);

/* a bitmap as a 1x1-pixel-per-unit rectangle under `wm` (bitmap px -> device px) */
static void render_bitmap_rect(Player *p, const Bitmap *bm, const SWFMatrix *wm, const SWFCxform *cx, int smooth) {
    ShapeDef s; ShapeEdge ed[4]; FillStyle fs; int i;
    if (!bm || !bm->px) return;
    memset(&s, 0, sizeof(s)); memset(&fs, 0, sizeof(fs)); swf_matrix_identity(&fs.matrix);
    fs.type = smooth ? FILL_BITMAP_CLIP : FILL_BITMAP_CLIP_NS; fs.bitmap_id = -1; fs.matrix.sx = fs.matrix.sy = 20.0;
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

/* a solid rectangle (0,0)-(w,h) under `wm` (scrollRect masks) */
static void render_rect_shape(Player *p, const SWFMatrix *wm, s32 w, s32 h) {
    ShapeDef s; ShapeEdge ed[4]; FillStyle fs; SWFCxform cx; int i;
    if (w <= 0 || h <= 0) return;
    memset(&s, 0, sizeof(s)); memset(&fs, 0, sizeof(fs)); swf_matrix_identity(&fs.matrix); swf_cxform_identity(&cx);
    fs.type = FILL_SOLID; fs.color.a = 255;
    s.fills = &fs; s.nfills = 1; s.edges = ed; s.nedges = 4; s.version = 3;
    for (i = 0; i < 4; i++) { memset(&ed[i], 0, sizeof(ed[i])); ed[i].fill1 = 1; }
    ed[0].x1 = w; ed[1].x0 = w; ed[1].x1 = w; ed[1].y1 = h; ed[2].x0 = w; ed[2].y0 = h; ed[2].y1 = h; ed[3].y0 = h;
    s.bounds.xmax = s.edge_bounds.xmax = w; s.bounds.ymax = s.edge_bounds.ymax = h;
    render_shape(p->surf, p->movie, &s, wm, &cx, NULL);
}

static void render_dobj_at(Player *p, DisplayObject *d, const SWFMatrix *wmp, const SWFCxform *wcxp);
static void render_content(Player *p, DisplayObject *d, const SWFMatrix *wmp, const SWFCxform *wcxp) {
    SWFMatrix wm = *wmp;
    d->has_scroll_applied = d->has_scroll_rect; d->scroll_applied = d->scroll_rect;
    if (d->has_scroll_rect) {
        /* scrollRect: the rectangle's content shows at the origin, clipped to its size */
        SWFRect r = d->scroll_rect; SWFMatrix t, cm;
        surf_push_mask(p->surf); render_rect_shape(p, &wm, r.xmax - r.xmin, r.ymax - r.ymin); surf_activate_mask(p->surf);
        swf_matrix_identity(&t); t.tx = -(double)r.xmin; t.ty = -(double)r.ymin;
        swf_matrix_mul(&cm, &wm, &t);
        render_dobj_at(p, d, &cm, wcxp);
        surf_deactivate_mask(p->surf); render_rect_shape(p, &wm, r.xmax - r.xmin, r.ymax - r.ymin); surf_pop_mask(p->surf);
        return;
    }
    render_dobj_at(p, d, &wm, wcxp);
}
void player_view_matrix(Player *p, SWFMatrix *out);
static int g_no_bitmap_cache;   /* BitmapData.draw renders without bitmap caches (Ruffle use_bitmap_cache) */

/* a cached bitmap (premultiplied, w x h) drawn pixel aligned at (dx, dy) with
   a colour transform the way Ruffle's bitmap shader applies it */
static void draw_cached(Player *p, u32 *px, int w, int h, int dx, int dy, const SWFCxform *cx) {
    int ident = cx->ma == 256 && cx->mr == 256 && cx->mg == 256 && cx->mb == 256 && !cx->aa && !cx->ar && !cx->ag && !cx->ab;
    if (!ident) {
        /* the colour transform goes into a temporary, never the input buffer:
           `px` may be a cached bitmap reused on later frames (render_cached's
           filter cache), so mutating it in place would corrupt the cache */
        size_t i, n = (size_t)w * h; u32 *out = (u32*)malloc(n * 4);
        float mr = cx->mr / 256.0f, mg = cx->mg / 256.0f, mb = cx->mb / 256.0f, ma = cx->ma / 256.0f;
        float ar = cx->ar / 255.0f, ag = cx->ag / 255.0f, ab = cx->ab / 255.0f, aa = cx->aa / 255.0f;
        if (!out) { surf_draw_pixels(p->surf, px, dx, dy, w, h, TB_NORMAL); return; }   /* OOM: draw untransformed */
        for (i = 0; i < n; i++) {
            u32 c = px[i]; float a = (c >> 24) / 255.0f, r, g, b;
            if (a <= 0) { out[i] = c; continue; }
            r = ((c >> 16) & 255) / 255.0f / a; g = ((c >> 8) & 255) / 255.0f / a; b = (c & 255) / 255.0f / a;
            r = r * mr + ar; g = g * mg + ag; b = b * mb + ab; a = a * ma + aa;
            r = r < 0 ? 0 : r > 1 ? 1 : r; g = g < 0 ? 0 : g > 1 ? 1 : g; b = b < 0 ? 0 : b > 1 ? 1 : b; a = a < 0 ? 0 : a > 1 ? 1 : a;
            out[i] = ((u32)(a * 255.0f + 0.5f) << 24) | ((u32)(r * a * 255.0f + 0.5f) << 16) | ((u32)(g * a * 255.0f + 0.5f) << 8) | (u32)(b * a * 255.0f + 0.5f);
        }
        surf_draw_pixels(p->surf, out, dx, dy, w, h, TB_NORMAL);
        free(out);
        return;
    }
    surf_draw_pixels(p->surf, px, dx, dy, w, h, TB_NORMAL);
}

/* children with filters grow their parents' render bounds (twips, m: to device twips) */
static void grow_child_filters(MovieClip *c, const SWFMatrix *m, double vx, double vy, SWFRect *b, int depth) {
    extern int dg_dobj_bounds(DisplayObject *d, const SWFMatrix *m, SWFRect *out);
    DisplayObject *ch;
    if (!c || depth > 64) return;
    for (ch = c->children; ch; ch = ch->next) {
        SWFMatrix cm; SWFRect r; int k;
        swf_matrix_mul(&cm, m, &ch->matrix);
        if (ch->nfilters > 0 && dg_dobj_bounds(ch, &cm, &r)) {
            for (k = 0; k < ch->nfilters; k++) { SwfFilter f = ch->filters[k]; flt_scale(&f, vx, vy); flt_dest_rect(&f, &r.xmin, &r.ymin, &r.xmax, &r.ymax); }
            if (r.xmin < b->xmin) b->xmin = r.xmin; if (r.ymin < b->ymin) b->ymin = r.ymin;
            if (r.xmax > b->xmax) b->xmax = r.xmax; if (r.ymax > b->ymax) b->ymax = r.ymax;
        }
        if (ch->clip) grow_child_filters(ch->clip, &cm, vx, vy, b, depth + 1);
    }
}

/* cacheAsBitmap / filters (Ruffle render_base with a BitmapCache): the
   object is drawn without its colour transform into a bitmap the size of
   its pixel bounds plus the filters' growth, its bounds' top left on a whole
   pixel; the filters run on the bitmap, which is drawn back with the colour
   transform at the rounded position. Returns 0 when there is no cache
   (empty or oversized): the object is then drawn normally. */
static int g_cache_probe;   /* render_cached only tells whether the cache would fit */
static int render_cached(Player *p, DisplayObject *d, const SWFMatrix *wm, const SWFCxform *wcx) {
    extern int dg_dobj_bounds(DisplayObject *d, const SWFMatrix *m, SWFRect *out);
    SWFMatrix w20, cm, vm; SWFRect b; SwfFilter *fl = NULL; int nf = 0, k, width, height, tw, th, ok, dx, dy;
    s32 fx0, fy0, fx1, fy1; double fdx0, fdy0, vx, vy, ox, oy; Surface *ls, *saved = p->surf; SWFCxform idc; u32 *drawsrc;
    w20 = *wm; w20.sx *= 20; w20.r0 *= 20; w20.r1 *= 20; w20.sy *= 20; w20.tx *= 20; w20.ty *= 20;
    {   /* render bounds: the content, not the scroll rect */
        int sa = d->has_scroll_applied, r; d->has_scroll_applied = 0;
        r = dg_dobj_bounds(d, &w20, &b); d->has_scroll_applied = sa;
        if (!r) return g_cache_probe;
    }
    player_view_matrix(p, &vm); vx = vm.sx * 20.0; vy = vm.sy * 20.0;
    if (d->clip) { SWFMatrix cw = w20; grow_child_filters(d->clip, &cw, vx, vy, &b, 0); }
    width = (int)ceil((b.xmax - b.xmin) / 20.0); height = (int)ceil((b.ymax - b.ymin) / 20.0);
    if (width < 0 || height < 0 || width > 65535 || height > 65535) return 0;
    fx0 = 0; fy0 = 0; fx1 = width * 20; fy1 = height * 20;
    if (d->nfilters > 0) {
        fl = (SwfFilter*)malloc((size_t)d->nfilters * sizeof(SwfFilter));
        if (fl) for (k = 0; k < d->nfilters; k++) {
            if (flt_impotent(&d->filters[k])) continue;
            fl[nf] = d->filters[k]; flt_scale(&fl[nf], vx, vy);
            flt_dest_rect(&fl[nf], &fx0, &fy0, &fx1, &fy1);
            nf++;
        }
    }
    fdx0 = floor(fx0 / 20.0); fdy0 = floor(fy0 / 20.0);
    tw = (int)(ceil(fx1 / 20.0) - fdx0); th = (int)(ceil(fy1 / 20.0) - fdy0);
    ok = tw > 0 && th > 0 && (p->movie->version > 9 ? (tw < 8191 && th < 8191 && (double)tw * th < 16777215.0) : (tw < 2880 && th < 2880));
    if (g_cache_probe) { free(fl); return ok || tw <= 0 || th <= 0; }
    if (!ok || !(ls = surf_create_ns(tw, th, saved->ns))) { free(fl); return 0; }
    /* the bounds' top left at (-draw offset) in the bitmap */
    cm = *wm; cm.tx = wm->tx - b.xmin / 20.0 - fdx0; cm.ty = wm->ty - b.ymin / 20.0 - fdy0;
    swf_cxform_identity(&idc);
    if (d->has_opaque_bg) { SWFColor bg; bg.r = (u8)(d->opaque_bg >> 16); bg.g = (u8)(d->opaque_bg >> 8); bg.b = (u8)d->opaque_bg; bg.a = 255; surf_clear(ls, bg); }   /* the cache's clear colour */
    p->surf = ls;
    render_dobj_at(p, d, &cm, &idc);
    p->surf = saved;
    surf_resolve(ls);
    /* filter-result cache: the colour transform is applied on draw-back (never
       baked in, see draw_cached), so a fade reuses the same filtered bitmap.
       Keyed by a hash of the rendered source and of the inline filter
       parameters, so it is never stale; used only when every filter keeps its
       data inside SwfFilter (no convolution matrix, displacement map or
       shader, which sit behind a pointer). */
    drawsrc = ls->px;
    {
        int cacheable = nf > 0;
        for (k = 0; k < nf && cacheable; k++)
            if (fl[k].kind == SF_CONVOLUTION || fl[k].kind == SF_DISPLACEMENT || fl[k].kind == SF_SHADER) cacheable = 0;
        if (cacheable) {
            u64 sh = 1469598103934665603ULL, fh = 1469598103934665603ULL; size_t cn = (size_t)tw * th, i; const u8 *fb = (const u8*)fl;
            for (i = 0; i < cn; i++) { sh ^= ls->px[i]; sh *= 1099511628211ULL; }
            for (i = 0; i < nf * sizeof(SwfFilter); i++) { fh ^= fb[i]; fh *= 1099511628211ULL; }
            fh ^= (u64)nf; fh *= 1099511628211ULL;
            if (d->fcache_px && d->fcache_w == tw && d->fcache_h == th && d->fcache_sh == sh && d->fcache_fh == fh) {
                drawsrc = d->fcache_px;   /* unchanged: reuse the filtered bitmap, skip the filters */
            } else {
                for (k = 0; k < nf; k++) flt_apply(ls->px, tw, th, &fl[k], -cm.tx, -cm.ty);
                if (d->fcache_w != tw || d->fcache_h != th) { free(d->fcache_px); d->fcache_px = (u32*)malloc(cn * 4); d->fcache_w = tw; d->fcache_h = th; }
                if (d->fcache_px) { memcpy(d->fcache_px, ls->px, cn * 4); d->fcache_sh = sh; d->fcache_fh = fh; }
                else { d->fcache_w = d->fcache_h = 0; }
            }
        } else {
            for (k = 0; k < nf; k++) flt_apply(ls->px, tw, th, &fl[k], -cm.tx, -cm.ty);
        }
    }
    free(fl);
    /* drawn back at the translation only, snapped to whole pixels (scrollRect: translated, clipped) */
    ox = b.xmin / 20.0 - wm->tx + fdx0; oy = b.ymin / 20.0 - wm->ty + fdy0;
    if (d->has_scroll_rect) {
        SWFRect r = d->scroll_rect; SWFMatrix t, sm2;
        d->has_scroll_applied = 1; d->scroll_applied = r;
        swf_matrix_identity(&t); t.tx = -(double)r.xmin; t.ty = -(double)r.ymin;
        swf_matrix_mul(&sm2, wm, &t);
        dx = (int)(sm2.tx + ox < 0 ? -floor(-(sm2.tx + ox) + 0.5) : floor(sm2.tx + ox + 0.5));
        dy = (int)(sm2.ty + oy < 0 ? -floor(-(sm2.ty + oy) + 0.5) : floor(sm2.ty + oy + 0.5));
        surf_push_mask(p->surf); render_rect_shape(p, wm, r.xmax - r.xmin, r.ymax - r.ymin); surf_activate_mask(p->surf);
        draw_cached(p, drawsrc, tw, th, dx, dy, wcx);
        surf_deactivate_mask(p->surf); render_rect_shape(p, wm, r.xmax - r.xmin, r.ymax - r.ymin); surf_pop_mask(p->surf);
    } else {
        d->has_scroll_applied = 0;
        dx = (int)(wm->tx + ox < 0 ? -floor(-(wm->tx + ox) + 0.5) : floor(wm->tx + ox + 0.5));
        dy = (int)(wm->ty + oy < 0 ? -floor(-(wm->ty + oy) + 0.5) : floor(wm->ty + oy + 0.5));
        draw_cached(p, drawsrc, tw, th, dx, dy, wcx);
    }
    surf_free(ls);
    return 1;
}
/* global (stage twips) -> device: the stage's view matrix, or in BitmapData.draw
   the draw matrix with the drawn object's global transform undone */
static SWFMatrix g_mask_base; static DisplayObject *g_draw_root; static SWFMatrix g_draw_matrix;
static void dobj_global(MovieClip *q, SWFMatrix *acc) {   /* a clip's local -> global (its own matrix included) */
    swf_matrix_identity(acc);
    while (q && q->self) { SWFMatrix t; swf_matrix_mul(&t, &q->self->matrix, acc); *acc = t; q = q->parent; }
}
static void render_body_inner(Player *p, DisplayObject *d, const SWFMatrix *wm, const SWFCxform *wcx);
/* a script mask (DisplayObject.mask / setMask) around the object (Ruffle
   apply_standard_mask_and_scroll): the mask's content at its global position */
static void render_body(Player *p, DisplayObject *d, const SWFMatrix *wm, const SWFCxform *wcx) {
    extern MovieClip *clip_script_mask(void *vm, MovieClip *c);
    MovieClip *mc = d->clip && p->avm ? clip_script_mask(p->avm, d->clip) : NULL;
    if (mc) {
        SWFMatrix mg, mw, mdev; SWFCxform idc;
        if (!mc->self || mc->dead) return;   /* masked by a mask that is gone: nothing */
        dobj_global(mc, &mg);
        if (d == g_draw_root) swf_matrix_mul(&mdev, &g_draw_matrix, &mg);
        else swf_matrix_mul(&mdev, &g_mask_base, &mg);
        mw = mdev; swf_cxform_identity(&idc);
        if (!g_no_bitmap_cache && mc->self->cache_bmp && mc->self->nfilters == 0) {
            /* Flash: a cached mask whose bitmap would be too big masks nothing */
            int ok; g_cache_probe = 1; ok = render_cached(p, mc->self, &mw, &idc); g_cache_probe = 0;
            if (!ok) { render_body_inner(p, d, wm, wcx); return; }
        }
        if (!g_no_bitmap_cache && (d->cache_bmp || d->nfilters > 0) && (mc->self->cache_bmp || mc->self->nfilters > 0)
            && p->surf->mstate != MS_DRAW_STENCIL && p->surf->mstate != MS_CLEAR_STENCIL) {
            /* both cached: an alpha mask (Ruffle RenderMask::Alpha): the maskee times the mask's alpha */
            Surface *saved = p->surf, *a = surf_create_ns(saved->w, saved->h, saved->ns), *m = surf_create_ns(saved->w, saved->h, saved->ns);
            if (a && m) {
                size_t i, n = (size_t)saved->w * saved->h;
                p->surf = a; render_body_inner(p, d, wm, wcx);
                p->surf = m; render_body_inner(p, mc->self, &mw, &mc->self->cxform);
                p->surf = saved;
                surf_resolve(a); surf_resolve(m);
                for (i = 0; i < n; i++) {
                    u32 c = a->px[i], k = m->px[i] >> 24;
                    if (k == 255) continue;
                    /* Flash: (value * alpha) >> 8 */
                    a->px[i] = (((c >> 24) * k) >> 8 << 24) | ((((c >> 16) & 255) * k) >> 8 << 16) | ((((c >> 8) & 255) * k) >> 8 << 8) | (((c & 255) * k) >> 8);
                }
                surf_draw_pixels(saved, a->px, 0, 0, saved->w, saved->h, TB_NORMAL);
            }
            surf_free(a); surf_free(m);
            return;
        }
        surf_push_mask(p->surf); render_content(p, mc->self, &mw, &idc); surf_activate_mask(p->surf);
        render_body_inner(p, d, wm, wcx);
        surf_deactivate_mask(p->surf); render_content(p, mc->self, &mw, &idc); surf_pop_mask(p->surf);
        return;
    }
    render_body_inner(p, d, wm, wcx);
}
/* the object with its bitmap cache if it has one, else directly (an opaque
   background first fills its render bounds, filters included) */
static void render_body_inner(Player *p, DisplayObject *d, const SWFMatrix *wm, const SWFCxform *wcx) {
    extern int dg_dobj_bounds(DisplayObject *d, const SWFMatrix *m, SWFRect *out);
    if (!g_no_bitmap_cache && (d->cache_bmp || d->nfilters > 0) && render_cached(p, d, wm, wcx)) return;
    if (d->has_opaque_bg) {
        SWFMatrix w20 = *wm, vm; SWFRect b; int k;
        w20.sx *= 20; w20.r0 *= 20; w20.r1 *= 20; w20.sy *= 20; w20.tx *= 20; w20.ty *= 20;
        if (dg_dobj_bounds(d, &w20, &b)) {
            RMatrix bm; SWFColor c; double vx, vy;
            player_view_matrix(p, &vm); vx = vm.sx * 20.0; vy = vm.sy * 20.0;
            if (d->clip) grow_child_filters(d->clip, &w20, vx, vy, &b, 0);
            for (k = 0; k < d->nfilters; k++) { SwfFilter f = d->filters[k]; flt_scale(&f, vx, vy); flt_dest_rect(&f, &b.xmin, &b.ymin, &b.xmax, &b.ymax); }
            c.r = (u8)(d->opaque_bg >> 16); c.g = (u8)(d->opaque_bg >> 8); c.b = (u8)d->opaque_bg; c.a = 255;
            rmat_box(&bm, (float)((b.xmax - b.xmin) / 20.0), (float)((b.ymax - b.ymin) / 20.0), b.xmin, b.ymin);
            render_draw_rect(p->surf, c, &bm);
        }
    }
    render_content(p, d, wm, wcx);
}
/* blend modes: the object is drawn into a transparent layer the size of the
   surface (with its samples) and composited with its blend mode (Ruffle
   Command::Blend); alpha and erase need a Layer above them */
static void render_blended(Player *p, DisplayObject *d, const SWFMatrix *wm, const SWFCxform *wcx) {
    Surface *ls, *saved = p->surf; int saved_bg = p->layer_bg;
    ls = surf_create_ns(saved->w, saved->h, saved->ns);
    if (!ls) { render_body(p, d, wm, wcx); return; }
    p->surf = ls; p->layer_bg = d->blend == BLEND_LAYER ? 0 : saved_bg;
    render_body(p, d, wm, wcx);
    p->surf = saved; p->layer_bg = saved_bg;
    surf_resolve(ls);
    if (d->blend == BLEND_SHADER && d->blend_shader) flt_composite_shader(p->surf, ls->px, 0, 0, ls->w, ls->h, NULL, d->blend_shader);
    else flt_composite(p->surf, ls->px, 0, 0, ls->w, ls->h, d->blend, NULL, p->layer_bg >= 0);
    surf_free(ls);
}
/* Ruffle's 3D: the summed z of the transform stack, projected with the fixed
   perspective (field of view 55, centred on the viewport): a scale about the
   viewport's centre by f / (f + z / (width / 2)) */
static double g_render_tz;
static double tz_scale(double tz, double vw) { double f = tan((3.14159265358979323846 - 55.0 * 3.14159265358979323846 / 180.0) / 2.0); float ff = (float)f, w = ff + (float)(tz / vw); return w > 0 ? (double)(ff / w) : 0; }   /* 0: behind the eye, clipped */
static void render_dobj(Player *p, DisplayObject *d, const SWFMatrix *pm, const SWFCxform *pcx) {
    SWFMatrix wm; SWFCxform wcx; double tz = 0, saved_tz = g_render_tz;
    if (!d->visible && p->surf->maskers == 0) return;   /* a mask's descendants are drawn even when invisible (Ruffle drawing_mask) */
    if (d->clip_depth > 0) return;             /* masks handled by render_clip */
    render_matrix_compose(&wm, pm, &d->matrix);
    swf_cxform_mul(&wcx, pcx, &d->cxform);
    if (p->avm2) { extern double a2_dobj_tz(DisplayObject *d); tz = a2_dobj_tz(d); }
    if (tz != 0) {
        double vw = p->surf->w / 2.0, vh = p->surf->h / 2.0, s1 = tz_scale(g_render_tz + tz, vw), s0 = tz_scale(g_render_tz, vw), r;
        if (s1 <= 0 || s0 <= 0) return;
        r = s1 / s0;
        wm.sx *= r; wm.r0 *= r; wm.r1 *= r; wm.sy *= r; wm.tx = vw + (wm.tx - vw) * r; wm.ty = vh + (wm.ty - vh) * r;
        g_render_tz += tz;
    }
    if ((p->surf->mstate == MS_NONE || p->surf->mstate == MS_CONTENT) && d->blend >= BLEND_LAYER) render_blended(p, d, &wm, &wcx);
    else render_body(p, d, &wm, &wcx);
    g_render_tz = saved_tz;
}
static void render_dobj_at(Player *p, DisplayObject *d, const SWFMatrix *wmp, const SWFCxform *wcxp) {
    SWFMatrix wm = *wmp; SWFCxform wcx = *wcxp;
    if (!d->ch) { if (d->clip) render_clip(p, d->clip, &wm, &wcx); return; }
    switch (d->ch->type) {
    case CHAR_SHAPE: {
        ShapeDef *sh = shape_get(d->ch->movie, d->ch);
        if (sh) render_shape(p->surf, d->ch->movie, sh, &wm, &wcx, NULL);
        break;
    }
    case CHAR_SPRITE:
        if (d->clip) render_clip(p, d->clip, &wm, &wcx);
        break;
    case CHAR_TEXT: case CHAR_EDITTEXT:
        text_render(p, d, &wm, &wcx);
        break;
    case CHAR_MORPH:
        morph_render(p, d, &wm, &wcx);
        break;
    case CHAR_BUTTON:
        if (p->avm2 && d->clip && CLIP_IS_A2(d->clip) && d->clip->children) render_clip(p, d->clip, &wm, &wcx);   /* AVM2: constructed state objects */
        else if (d->clip && CLIP_IS_A1(d->clip) && avm1_button_ready(d->clip)) render_clip(p, d->clip, &wm, &wcx);   /* AVM1: state children */
        else button_render(p, d, &wm, &wcx);
        break;
    case CHAR_VIDEO:
        video_render(p, d, &wm, &wcx);
        break;
    case CHAR_BITMAP:
        if (d->bmd) {   /* attachBitmap: pixelSnapping "auto" (Ruffle PixelSnapping::apply) */
            SWFMatrix sm = wm; double a = wm.sx * 20, dd = wm.sy * 20;
            if (a >= 0.999 && a <= 1.001 && dd >= 0.999 && dd <= 1.001 && wm.r0 == 0 && wm.r1 == 0) {
                double tx = floor(wm.tx * 20 + 0.5) / 20, ty = floor(wm.ty * 20 + 0.5) / 20;
                sm.sx = sm.sy = 1.0 / 20;
                sm.tx = tx < 0 ? -floor(-tx + 0.5) : floor(tx + 0.5); sm.ty = ty < 0 ? -floor(-ty + 0.5) : floor(ty + 0.5);
            }
            render_bitmap_rect(p, bmd_view(d->bmd), &sm, &wcx, d->bmd_smooth);
        }
        else if (d->clip && d->clip->obj2) { extern void a2_render_clip_bitmap(Player *p, MovieClip *c, const SWFMatrix *wm, const SWFCxform *cx); a2_render_clip_bitmap(p, d->clip, &wm, &wcx); }   /* AVM2: a bitmap symbol's Bitmap */
        else if (d->obj2) { extern void a2_render_obj_bitmap(Player *p, void *obj, const SWFMatrix *wm, const SWFCxform *cx); a2_render_obj_bitmap(p, d->obj2, &wm, &wcx); }   /* a bitmap placed by the timeline (PlaceObject3 HasImage) */
        break;
    default: break;
    }
}

/* BitmapData.draw (the renderer path): the source is drawn on black and on
 * white; the two results give its premultiplied colour and coverage,
 * which is then composited over the bitmap. */
static u8 *render_mask_of(Player *p, DisplayObject *d, const SWFMatrix *pm, const SWFCxform *pcx);
void player_bmd_draw_blend(Player *p, BitmapData *t, DisplayObject *d, BitmapData *src, const SWFMatrix *m, const SWFCxform *cx, const s32 *clip, int smoothing, int blend);
void player_bmd_draw(Player *p, BitmapData *t, DisplayObject *d, BitmapData *src, const SWFMatrix *m, const SWFCxform *cx, const s32 *clip, int smoothing) { player_bmd_draw_blend(p, t, d, src, m, cx, clip, smoothing, 0); }
void player_bmd_draw_blend(Player *p, BitmapData *t, DisplayObject *d, BitmapData *src, const SWFMatrix *m, const SWFCxform *cx, const s32 *clip, int smoothing, int blend) {
    extern int dg_dobj_bounds(DisplayObject *d, const SWFMatrix *m, SWFRect *out);
    SWFRect b; PxRegion area; Surface *ls, *saved = p->surf; SWFMatrix pm; u32 *layer; double fx0, fy0, fx1, fy1; int direct;
    if (!t || t->disposed || !t->w || !t->h) return;
    if (src) {
        double xs[4], ys[4]; int k;
        swf_matrix_apply(m, 0, 0, &xs[0], &ys[0]); swf_matrix_apply(m, src->w * 20.0, 0, &xs[1], &ys[1]);
        swf_matrix_apply(m, 0, src->h * 20.0, &xs[2], &ys[2]); swf_matrix_apply(m, src->w * 20.0, src->h * 20.0, &xs[3], &ys[3]);
        fx0 = fx1 = xs[0]; fy0 = fy1 = ys[0];
        for (k = 1; k < 4; k++) { if (xs[k] < fx0) fx0 = xs[k]; if (xs[k] > fx1) fx1 = xs[k]; if (ys[k] < fy0) fy0 = ys[k]; if (ys[k] > fy1) fy1 = ys[k]; }
    } else if (!d) {
        /* the stage: everything on it */
        fx0 = 0; fy0 = 0; fx1 = t->w * 20.0; fy1 = t->h * 20.0;
    } else {
        if (!dg_dobj_bounds(d, m, &b)) return;
        fx0 = b.xmin; fy0 = b.ymin; fx1 = b.xmax; fy1 = b.ymax;
    }
    fx0 = floor(fx0 / 20.0); fy0 = floor(fy0 / 20.0); fx1 = ceil(fx1 / 20.0); fy1 = ceil(fy1 / 20.0);
    area.x0 = fx0 > 0 ? (fx0 > 4e9 ? 0xFFFFFFFFu : (u32)fx0) : 0; area.y0 = fy0 > 0 ? (fy0 > 4e9 ? 0xFFFFFFFFu : (u32)fy0) : 0;
    area.x1 = fx1 > 0 ? (fx1 > 4e9 ? 0xFFFFFFFFu : (u32)fx1) : 0; area.y1 = fy1 > 0 ? (fy1 > 4e9 ? 0xFFFFFFFFu : (u32)fy1) : 0;
    reg_clamp(&area, t->w, t->h);
    if (area.x1 <= area.x0 || area.y1 <= area.y0) return;
    if (clip) {
        PxRegion c = reg_i32(clip[0], clip[1], clip[2], clip[3]);
        if (c.x0 > area.x0) area.x0 = c.x0; if (c.y0 > area.y0) area.y0 = c.y0;
        if (c.x1 < area.x1) area.x1 = c.x1; if (c.y1 < area.y1) area.y1 = c.y1;
        if (area.x1 <= area.x0 || area.y1 <= area.y0) return;
    }
    /* render into buffers covering just the affected area (integer offset: same rasterisation) */
    ls = surf_create_ns((int)(area.x1 - area.x0), (int)(area.y1 - area.y0), quality_samples(g_draw_quality >= 0 ? g_draw_quality : g_stage_quality));
    if (!ls) return;
    direct = !(blend == 11 || blend == 12 || (blend >= BLEND_MULTIPLY && blend <= BLEND_HARDLIGHT));
    if (direct) {
        /* Ruffle renders onto the bitmap itself (FreshWithTexture): every sample starts as the pixel */
        int aw = (int)(area.x1 - area.x0), ah = (int)(area.y1 - area.y0), x, y; u32 *reg = (u32*)malloc((size_t)aw * ah * 4 + 4);
        if (reg) {
            for (y = 0; y < ah; y++) for (x = 0; x < aw; x++) reg[(size_t)y * aw + x] = t->px[(size_t)(area.y0 + y) * t->w + area.x0 + x];
            surf_draw_pixels(ls, reg, 0, 0, aw, ah, TB_REPLACE);
            free(reg);
        } else direct = 0;
    }
    pm.sx = m->sx / 20.0; pm.r0 = m->r0 / 20.0; pm.r1 = m->r1 / 20.0; pm.sy = m->sy / 20.0; pm.tx = m->tx / 20.0 - (double)area.x0; pm.ty = m->ty / 20.0 - (double)area.y0;
    {
        int saved_bg = p->layer_bg, saved_nc = g_no_bitmap_cache;
        p->surf = ls; p->layer_bg = -1;   /* Flash applies filters here (Ruffle does not use bitmap caches in draw) */
        {
            if (src) {
                SWFMatrix bm2 = pm;
                render_bitmap_rect(p, bmd_view(src), &bm2, cx, smoothing);
            } else if (!d) {
                /* the stage's display list (no focus highlight) */
                SWFMatrix saved_base = g_mask_base; DisplayObject *q;
                g_mask_base = pm;
                if (p->root->self) { SWFMatrix rm; SWFCxform rc; swf_matrix_mul(&rm, &pm, &p->root->self->matrix); swf_cxform_mul(&rc, cx, &p->root->self->cxform); if (p->root->self->visible) render_clip(p, p->root, &rm, &rc); }
                else render_clip(p, p->root, &pm, cx);
                for (q = p->levels; q; q = q->next) {
                    SWFMatrix lm; SWFCxform lc;
                    if (!q->clip || !q->visible) continue;
                    swf_matrix_mul(&lm, &pm, &q->matrix); swf_cxform_mul(&lc, cx, &q->cxform);
                    render_clip(p, q->clip, &lm, &lc);
                }
                g_mask_base = saved_base;
            } else {
                SWFMatrix sm = d->matrix; SWFCxform sc = d->cxform; int sv = d->visible, scd = d->clip_depth;
                SWFMatrix saved_base = g_mask_base, saved_dm = g_draw_matrix, groot; DisplayObject *saved_root = g_draw_root;
                /* masks: the drawn object's own at the draw matrix, its children's relative to it */
                g_draw_root = d; g_draw_matrix = pm;
                if (d->clip) dobj_global(d->clip, &groot); else { MovieClip *q = d->owner; dobj_global(q, &groot); { SWFMatrix t; swf_matrix_mul(&t, &groot, &d->matrix); groot = t; } }
                {
                    double ia, ib, ic, id2, ie, iff; SWFMatrix inv;
                    if (matrix_invert(&groot, &ia, &ib, &ic, &id2, &ie, &iff)) {
                        inv.sx = ia; inv.r1 = ib; inv.r0 = ic; inv.sy = id2; inv.tx = ie; inv.ty = iff;
                        swf_matrix_mul(&g_mask_base, &pm, &inv);
                    }
                }
                swf_matrix_identity(&d->matrix); swf_cxform_identity(&d->cxform); d->visible = 1; d->clip_depth = 0;
                render_dobj(p, d, &pm, cx);
                d->matrix = sm; d->cxform = sc; d->visible = sv; d->clip_depth = scd;
                g_mask_base = saved_base; g_draw_matrix = saved_dm; g_draw_root = saved_root;
            }
        }
        p->surf = saved; p->layer_bg = saved_bg; g_no_bitmap_cache = saved_nc;
    }
    surf_resolve(ls);
    layer = ls->px;
    if (blend == 11 || blend == 12) {
        /* alpha / erase: the layer's alpha scales (or cuts) the bitmap */
        u32 x, y;
        for (y = area.y0; y < area.y1; y++) for (x = area.x0; x < area.x1; x++) {
            u32 la = layer[(y - area.y0) * (area.x1 - area.x0) + (x - area.x0)] >> 24, f = blend == 11 ? la : 255 - la, c = t->px[y * t->w + x];
            u32 a = ((c >> 24) * f + 127) / 255, r = (((c >> 16) & 255) * f + 127) / 255, g = (((c >> 8) & 255) * f + 127) / 255, bl = ((c & 255) * f + 127) / 255;
            if (t->transparent) t->px[y * t->w + x] = (a << 24) | (r << 16) | (g << 8) | bl;
        }
        bmd_changed(t);
    } else if (blend >= BLEND_MULTIPLY && blend <= BLEND_HARDLIGHT) {
        /* a blend mode: composited onto the bitmap's pixels as onto a parent layer */
        int aw = (int)(area.x1 - area.x0), ah = (int)(area.y1 - area.y0), x, y; u32 *reg = (u32*)malloc((size_t)aw * ah * 4 + 4);
        Surface *ts = surf_create_ns(aw, ah, 1); SWFCxform idc;
        if (reg && ts) {
            for (y = 0; y < ah; y++) for (x = 0; x < aw; x++) reg[(size_t)y * aw + x] = t->px[(size_t)(area.y0 + y) * t->w + area.x0 + x];
            surf_draw_pixels(ts, reg, 0, 0, aw, ah, TB_REPLACE);
            swf_cxform_identity(&idc);
            flt_composite(ts, layer, 0, 0, aw, ah, blend, &idc, 1);
            surf_resolve(ts);
            for (y = 0; y < ah; y++) for (x = 0; x < aw; x++) {
                u32 c = ts->px[(size_t)y * aw + x];
                if (!t->transparent) c = 0xFF000000u | (c & 0xFFFFFFu);
                t->px[(size_t)(area.y0 + y) * t->w + area.x0 + x] = c;
            }
            bmd_changed(t);
        }
        free(reg); surf_free(ts);
    } else if (direct) {
        u32 x, y, aw = area.x1 - area.x0;
        for (y = area.y0; y < area.y1; y++) for (x = area.x0; x < area.x1; x++) {
            u32 c = layer[(size_t)(y - area.y0) * aw + (x - area.x0)];
            if (!t->transparent) c = 0xFF000000u | (c & 0xFFFFFFu);
            t->px[(size_t)y * t->w + x] = c;
        }
        bmd_changed(t);
    } else bmd_draw_layer_area(t, layer, area);
    surf_free(ls);
}

/* draw the mask object `d` (into the stencil: the surface is in a mask state) */
static void render_mask_draw(Player *p, DisplayObject *d, const SWFMatrix *pm, const SWFCxform *pcx) {
    int saved_depth = d->clip_depth;
    d->clip_depth = 0;
    render_dobj(p, d, pm, pcx);
    d->clip_depth = saved_depth;
}

static void render_clip_in(Player *p, MovieClip *c, const SWFMatrix *pm, const SWFCxform *pcx);
/* draw a clip with its own movie's dictionary current (loaded SWFs) */
static void render_clip(Player *p, MovieClip *c, const SWFMatrix *pm, const SWFCxform *pcx) {
    SWFMovie *saved = p->cur_movie;
    p->cur_movie = c->movie;
    render_clip_in(p, c, pm, pcx);
    p->cur_movie = saved;
}
static void render_clip_in(Player *p, MovieClip *c, const SWFMatrix *pm, const SWFCxform *pcx) {
    DisplayObject *d;
    DisplayObject *mask_obj[64]; int mask_depth[64]; int nmask = 0;
    {
        extern ShapeDef *clip_drawing(void *vm, MovieClip *c);
        ShapeDef *dr = p->avm ? clip_drawing(p->avm, c) : NULL;
        if (dr && dr->nedges) {
            extern int clip_drawing_pending_close(void *vm, MovieClip *c, ShapeEdge *e);
            ShapeEdge pe; int added = 0;
            if (clip_drawing_pending_close(p->avm, c, &pe) && dr->nedges < dr->capedges) { dr->edges[dr->nedges++] = pe; added = 1; }
            render_shape(p->surf, p->movie, dr, pm, pcx, NULL);
            if (added) dr->nedges--;
        }
        if (c->obj2) { extern void a2_render_clip_bitmap(Player *p, MovieClip *c, const SWFMatrix *wm, const SWFCxform *cx); a2_render_clip_bitmap(p, c, pm, pcx); }
    }
    for (d = c->children; d; d = d->next) {
        /* SimpleButton state children: only the current state's objects are drawn */
        if (d->state_mask && !(d->state_mask & (c->self ? BTN_STATE_BIT(c->self->btn_state) : 1))) continue;
        /* pop masks whose range ended */
        while (nmask > 0 && d->depth > mask_depth[nmask - 1]) {
            nmask--;
            surf_deactivate_mask(p->surf); render_mask_draw(p, mask_obj[nmask], pm, pcx); surf_pop_mask(p->surf);
        }
        if (d->clip_depth > 0 && d->visible) {
            if (nmask < 64) {
                surf_push_mask(p->surf); render_mask_draw(p, d, pm, pcx); surf_activate_mask(p->surf);
                mask_obj[nmask] = d; mask_depth[nmask] = d->clip_depth; nmask++;
            }
            continue;
        }
        {
            extern int clip_is_script_mask(void *vm, MovieClip *c);
            if (d->clip && p->avm && clip_is_script_mask(p->avm, d->clip)) continue;  /* masks are invisible */
        }
        render_dobj(p, d, pm, pcx);
    }
    while (nmask > 0) { nmask--; surf_deactivate_mask(p->surf); render_mask_draw(p, mask_obj[nmask], pm, pcx); surf_pop_mask(p->surf); }
    text_render_dynamic(p, c, pm, pcx);
}

/* stage twips -> device pixels: the zoom, or the host viewport with the stage
   scale mode and alignment (Stage::build_matrices) */
void player_view_matrix(Player *p, SWFMatrix *out) {
    SWFMovie *m = p->movie; double mw, mh, vw, vh, sx, sy, wd, hd, tx, ty; int mode = 0, align = 0;
    if (!p->has_view) { render_stage_matrix(out, &m->stage, p->zoom); return; }
    if (p->avm2) { extern void a2_stage_view_mode(Player *p, int *mode, int *align); a2_stage_view_mode(p, &mode, &align); }
    else { extern void avm1_stage_view_mode(int *mode, int *align); avm1_stage_view_mode(&mode, &align); }
    mw = floor((m->stage.xmax - m->stage.xmin) / 20.0); mh = floor((m->stage.ymax - m->stage.ymin) / 20.0);
    if (mw <= 0) mw = 1; if (mh <= 0) mh = 1;
    vw = p->view_w; vh = p->view_h;
    switch (mode) {
    case 1: sx = vw / mw; sy = vh / mh; break;
    case 2: sx = sy = vw / vh < mw / mh ? vh / mh : vw / mw; break;
    case 3: sx = sy = p->view_sf > 0 ? p->view_sf : 1; break;
    default: sx = sy = vw / vh > mw / mh ? vh / mh : vw / mw; break;
    }
    wd = vw - mw * sx; hd = vh - mh * sy;
    tx = (align & 1) ? 0 : (align & 4) ? wd : wd / 2;
    ty = (align & 2) ? 0 : (align & 8) ? hd : hd / 2;
    swf_matrix_identity(out);
    out->sx = sx / TWIPS_PER_PX; out->sy = sy / TWIPS_PER_PX;
    out->tx = tx - m->stage.xmin * out->sx; out->ty = ty - m->stage.ymin * out->sy;
}

Surface *g_stage_surface;   /* the surface the stage is being drawn on (culling applies only there) */
void player_render(Player *p) {
    SWFMatrix sm; SWFCxform cx;
    g_stage_surface = p->surf;
    player_view_matrix(p, &g_mask_base);
    raster_quality = quality_samples(g_stage_quality);
    surf_set_samples(p->surf, raster_quality);
    surf_clear(p->surf, p->bg);
    player_view_matrix(p, &sm);
    if (p->avm2) { extern void a2_stage3d_render(Player *p, const SWFMatrix *sm); a2_stage3d_render(p, &sm); }   /* Stage3D layers under the display list */
    if (p->has_stage_mat) { SWFMatrix t = sm; render_matrix_compose(&sm, &t, &p->stage_mat); }   /* Ruffle: the stage's own transform over its children */
    swf_cxform_identity(&cx);
    if (p->root->self) { SWFMatrix rm; render_matrix_compose(&rm, &sm, &p->root->self->matrix); swf_cxform_mul(&cx, &cx, &p->root->self->cxform); if (p->root->self->visible) {
            extern MovieClip *clip_script_mask(void *vm, MovieClip *c);
            if (p->avm && clip_script_mask(p->avm, p->root)) render_body(p, p->root->self, &rm, &cx);   /* the root masked by script */
            else render_clip(p, p->root, &rm, &cx);
        } }
    else render_clip(p, p->root, &sm, &cx);
    {
        DisplayObject *d;
        for (d = p->levels; d; d = d->next) {
            SWFMatrix lm; SWFCxform lc;
            if (!d->clip || !d->visible) continue;
            swf_cxform_identity(&lc);
            render_matrix_compose(&lm, &sm, &d->matrix); swf_cxform_mul(&lc, &lc, &d->cxform);
            render_clip(p, d->clip, &lm, &lc);
        }
    }
    if (!p->avm2) { extern void avm1_render_highlight(Player *p, const SWFMatrix *sm); avm1_render_highlight(p, &sm); }
    else { extern void a2_render_highlight(Player *p, const SWFMatrix *sm); a2_render_highlight(p, &sm); }
    surf_resolve(p->surf);
}

/* ---------------------------------------------------------------- */

void (*g_fscommand_hook)(Player *p, const char *cmd, const char *args);   /* host hook (test harness image captures) */
int g_player_release;   /* release (not debugger) player: set by hosts */
int g_view_w, g_view_h; double g_view_sf = 1;   /* a host viewport set before player_create (0: the movie's size) */
static void player_setup(Player *p, SWFMovie *m);
static void player_free_parts(Player *p);
Player *player_create(SWFMovie *m, double zoom) {
    Player *p = (Player*)calloc(1, sizeof(Player));
    if (!p) return NULL;
    p->zoom = zoom;
    player_setup(p, m);
    return p;
}
/* replace the root movie by an AVM2 movie loaded into _level0 of an AVM1 movie (Ruffle replace_root_movie):
   the player starts over with it; the old movie stays owned by its creator */
void player_swap_root(Player *p) {
    SWFMovie *nm = p->swap_root, *old = p->movie; double zoom = p->zoom; int i, j = 0, dbg = p->debug; void (*hook)(Player *p) = p->mix_hook;
    p->swap_root = NULL;
    for (i = 0; i < p->nloaded; i++) if (p->loaded[i] != nm) p->loaded[j++] = p->loaded[i];
    p->nloaded = j;
    player_free_parts(p);
    memset(p, 0, sizeof(*p));
    p->zoom = zoom;
    player_setup(p, nm);
    p->debug = dbg; p->mix_hook = hook;
    player_keep_movie(p, nm);   /* freed with the player */
    (void)old;
}
static void player_setup(Player *p, SWFMovie *m) {
    int w, h;
    double zoom = p->zoom;
    p->movie = m;
    p->zoom = zoom;
    if (g_view_w > 0 && g_view_h > 0) { p->has_view = 1; p->view_w = g_view_w; p->view_h = g_view_h; p->view_sf = g_view_sf; }
    p->bg = m->bg; p->bg.a = 255;
    p->loop = 1;
#ifndef FLASHNT_NT31
    p->debug = getenv("FLASHNT_DEBUG") ? atoi(getenv("FLASHNT_DEBUG")) : 0;
#endif
    w = (int)((m->stage.xmax - m->stage.xmin) * zoom / TWIPS_PER_PX + 0.5);
    h = (int)((m->stage.ymax - m->stage.ymin) * zoom / TWIPS_PER_PX + 0.5);
    if (p->has_view) { w = p->view_w; h = p->view_h; }
    if (w < 1) w = 1; if (h < 1) h = 1;
    p->surf = surf_create(w, h); p->layer_bg = -1;
    p->avm = avm1_create(p);
    p->audio = audio_create(p);
    p->root = clip_create(p, NULL, &m->root, NULL);
    if (m->has_abc) avm2_create(p);
    if (!p->avm2) {
        /* AVM1: init actions, the root object, then frame 1 */
        avm1_start(p);
        return;
    }
    /* show frame 1 */
    run_frame_tags(p->root, 1);
    avm1_clip_loaded(p, p->root);
    clear_ran(p->root);
    flush_actions(p);
    if (p->avm2) { avm2_tick_end(p); clear_ran(p->root); }   /* clips created during frame 1 advance on the next tick */
    player_flush_graveyard(p);
    apply_scroll_rects(p->root, 0);
}

void player_free(Player *p) {
    if (!p) return;
    player_free_parts(p);
    free(p);
}
static void player_free_parts(Player *p) {
    clip_retire(p->root);
    player_flush_graveyard(p);
    player_flush_graveyard(p);
    if (p->root && p->root->self && p->root->self->owner == NULL && p->root->self->char_id == -1) { extern void avm2_dobj_detach(DisplayObject *d); if (p->avm2) avm2_dobj_detach(p->root->self); free(p->root->self); p->root->self = NULL; }
    clip_really_free(p->root);
    p->root = NULL;
    while (p->levels) { DisplayObject *n = p->levels->next; if (p->levels->clip) { clip_retire(p->levels->clip); clip_really_free(p->levels->clip); } free(p->levels); p->levels = n; }
    free(p->queue);
    avm2_free(p);
    { int i; for (i = 0; i < p->nloaded; i++) swf_free(p->loaded[i]); free(p->loaded); }
    free(p->fref_m); free(p->fref_id); free(p->gfont);
    avm1_free(p);
    audio_free(p);
    surf_free(p->surf);
}


/* ---------------------------------------------------------------- */
/* input                                                            */

static void mouse_buttons(Player *p, MovieClip *c, const SWFMatrix *pm, int x, int y, int down, int changed) {
    DisplayObject *d, *next;
    for (d = c->children; d; d = next) {
        SWFMatrix wm;
        next = d->next;
        if (d->clip && CLIP_A1MIX(d->clip)) continue;   /* AVM1Movie: the AVM1 input handles it */
        if (!d->ch) { if (d->clip) { swf_matrix_mul(&wm, pm, &d->matrix); mouse_buttons(p, d->clip, &wm, x, y, down, changed); } continue; }
        swf_matrix_mul(&wm, pm, &d->matrix);
        if (d->ch->type == CHAR_BUTTON) button_mouse(p, d, &wm, x, y, down, changed);
        else if (d->clip && d->ch->type == CHAR_SPRITE) mouse_buttons(p, d->clip, &wm, x, y, down, changed);
    }
}

static void key_buttons(Player *p, MovieClip *c, int keycode);

/* find the topmost editable text field under stage point (twips) */
static MovieClip *find_edit_at(Player *p, MovieClip *c, const SWFMatrix *pm, double sx, double sy) {
    DisplayObject *d; MovieClip *hit = NULL;
    for (d = c->children; d; d = d->next) {
        SWFMatrix wm;
        if (!d->visible || (d->clip && CLIP_A1MIX(d->clip))) continue;
        swf_matrix_mul(&wm, pm, &d->matrix);
        if (d->clip && (!d->ch || d->ch->type == CHAR_SPRITE)) { MovieClip *h = find_edit_at(p, d->clip, &wm, sx, sy); if (h) hit = h; }
        else if (d->ch && d->ch->type == CHAR_EDITTEXT && d->clip && edittext_editable(d->ch)) {
            Reader r; SWFRect b; double a, bb, cc, dd, e, f, lx, ly;
            reader_init(&r, d->ch->data, d->ch->len); r_u16(&r); r_rect(&r, &b);
            if (matrix_invert(&wm, &a, &bb, &cc, &dd, &e, &f)) {
                lx = a * sx + bb * sy + e; ly = cc * sx + dd * sy + f;
                if (lx >= b.xmin && lx <= b.xmax && ly >= b.ymin && ly <= b.ymax) hit = d->clip;
            }
        }
    }
    return hit;
}

/* an editing command (MoveLeft, SelectAll, Paste, ...) for the focused text field */
void player_text_control(Player *p, const char *name) {
    if (p->avm2 && !(p->focus && !p->focus->dead && CLIP_A1MIX(p->focus))) { extern void avm2_text_control(Player *p, const char *name); avm2_text_control(p, name); return; }
    if (p->avm && p->focus && !p->focus->dead) { extern void a1_tf_control(void *vm, MovieClip *c, const char *name); a1_tf_control(p->avm, p->focus, name); }
}
/* IME composition / commit for the focused text field (AVM1) */
void player_ime_preedit(Player *p, const char *utf8, int from, int to) {
    if (p->avm && p->focus && !p->focus->dead && CLIP_IS_A1(p->focus)) { extern void a1_tf_ime_preedit(void *vm, MovieClip *c, const char *u8, int from, int to); a1_tf_ime_preedit(p->avm, p->focus, utf8, from, to); return; }
    if (p->avm2) { extern void avm2_ime(Player *p, const char *utf8, int from, int to, int commit); avm2_ime(p, utf8, from, to, 0); }
}
void player_ime_commit(Player *p, const char *utf8) {
    if (p->avm && p->focus && !p->focus->dead && CLIP_IS_A1(p->focus)) { extern void a1_tf_ime_commit(void *vm, MovieClip *c, const char *u8); a1_tf_ime_commit(p->avm, p->focus, utf8); return; }
    if (p->avm2) { extern void avm2_ime(Player *p, const char *utf8, int from, int to, int commit); avm2_ime(p, utf8, 0, 0, 1); }
}
/* the container calls into the movie (ExternalInterface.addCallback functions) */
#include "extval.h"
void player_call_internal(Player *p, const char *name, int argc, const ExtVal *args, ExtVal *out) {
    extern int avm1_ei_invoke(Player *p, const char *name, int argc, const ExtVal *args, ExtVal *out);
    *out = xv_null();
    extern int avm2_ei_invoke(Player *p, const char *name, int argc, const ExtVal *args, ExtVal *out);
    if (!p->avm2 && p->avm && avm1_ei_invoke(p, name, argc, args, out)) return;
    if (p->avm2 && avm2_ei_invoke(p, name, argc, args, out)) return;
}
void player_focus_lost(Player *p) {
    if (!p->avm2 && p->avm) { extern void avm1_focus_lost(Player *p); avm1_focus_lost(p); return; }
    if (p->avm2) { extern void avm2_focus_lost(Player *p); avm2_focus_lost(p); return; }
    player_set_focus(p, NULL);
}
void player_set_focus(Player *p, MovieClip *c) {
    if (!p->avm2 && p->avm) { extern void avm1_player_set_focus(Player *p, MovieClip *c); avm1_player_set_focus(p, c); return; }
    VM_FOCUS_HOOK(p, c);
    p->focus = c;
    p->caret_blink = 16;
}

void player_char(Player *p, int ch) {
    char buf[4096]; size_t n;
    DisplayObject *d;
    if (p->avm2 && p->focus && !p->focus->dead && CLIP_A1MIX(p->focus)) { extern void avm1_input_char(Player *p, int ch); avm1_input_char(p, ch); return; }   /* a text field of an AVM1Movie */
    { extern int avm1_mix_active(Player *p); extern void avm1_input_char(Player *p, int ch); if (p->avm2 && avm1_mix_active(p)) avm1_input_char(p, ch); }   /* keyPress handlers of an AVM1Movie */
    if (p->avm2 && avm2_handles_char(p)) { avm2_char(p, ch); return; }
    if (!p->avm2) { extern void avm1_input_char(Player *p, int ch); avm1_input_char(p, ch); return; }
    if (ch >= 32 && ch < 127) key_buttons(p, p->root, ch);     /* on(keyPress "a") */
    if (!p->focus || p->focus->dead || !p->focus->self) return;
    if (!p->avm2) { extern void a1_tf_typed(void *vm, MovieClip *c, int ch); a1_tf_typed(p->avm, p->focus, ch); p->caret_blink = 16; return; }
    d = p->focus->self;
    if (!d->ch || d->ch->type != CHAR_EDITTEXT) return;
    edittext_get(p, d, buf, sizeof(buf));
    n = strlen(buf);
    if (ch == 8) { if (n) { n--; while (n && (buf[n] & 0xC0) == 0x80) n--; buf[n] = 0; } }
    else if (ch == 13 || ch == 10) { if (n + 1 < sizeof(buf)) { buf[n++] = '\n'; buf[n] = 0; } }
    else if (ch >= 32 && ch < 127) { if (n + 1 < sizeof(buf)) { buf[n++] = (char)ch; buf[n] = 0; } }
    else if (ch >= 128 && ch < 256) { if (n + 2 < sizeof(buf)) { buf[n++] = (char)(0xC0 | (ch >> 6)); buf[n++] = (char)(0x80 | (ch & 63)); buf[n] = 0; } }
    else return;
    edittext_set(p, d, buf);
    p->caret_blink = 16;
}

static void click_series(Player *p, int x, int y);
void player_mouse(Player *p, int x, int y, int down) {
    int changed = (down != p->mouse_down);
    SWFMatrix sm;
    p->mouse_x = x; p->mouse_y = y; p->mouse_down = down;
    if (changed) { if (down) p->key_toggled[1] ^= 1; p->key_down[1] = down ? 1 : 0; }   /* the left button is key 1 */
    { extern int g_a1_press_took_focus; g_a1_press_took_focus = 0; }
    if (changed && down) click_series(p, x, y);
    render_stage_matrix(&sm, &p->movie->stage, p->zoom);
    if (!p->avm2) { extern void avm1_input_mouse(Player *p, int x, int y, int down); avm1_input_mouse(p, x, y, down); return; }
    { extern int avm1_mix_active(Player *p); extern void avm1_input_mouse(Player *p, int x, int y, int down); if (avm1_mix_active(p)) avm1_input_mouse(p, x, y, down); }   /* AVM1Movie content first (its focus changes come before AVM2 events) */
    if (changed && down && !(p->focus && !p->focus->dead && CLIP_A1MIX(p->focus))) {
        SWFMatrix id; swf_matrix_identity(&id);
        player_set_focus(p, find_edit_at(p, p->root, &id, x * TWIPS_PER_PX / p->zoom, y * TWIPS_PER_PX / p->zoom));
    }
    mouse_buttons(p, p->root, &sm, x, y, down, changed);
    if (p->avm2) avm2_mouse(p, x, y, down, changed);
}

void player_mouse_button(Player *p, int x, int y, int button, int down) {
    int vk = button == 1 ? 4 : button == 2 ? 2 : 0;   /* middle = key 4, right = key 2 */
    p->mouse_x = x; p->mouse_y = y;
    if (vk) { if (down && !p->key_down[vk]) p->key_toggled[vk] ^= 1; p->key_down[vk] = down ? 1 : 0; }
    if (p->avm2) avm2_mouse_button(p, x, y, button, down);
    else if (button == 2 && p->movie->version < 9) { extern void avm1_reset_highlight(Player *p); avm1_reset_highlight(p); }
}

void player_click_index(Player *p, int index) { p->click_set = index + 1; }
/* a left press: its number in a click series (Ruffle InputManager::update_last_click:
   within 500 ms and 2 px of the previous press continues the series) */
static void click_series(Player *p, int x, int y) {
    extern double plat_now_ms(void); extern void avm2_click_index(Player *p, int index);
    double now = plat_now_ms(); int idx;
    if (p->click_set > 0) idx = p->click_set - 1;
    else if (p->click_have && fabs(now - p->click_last_ms) < 500 && (double)(x - p->click_last_x) * (x - p->click_last_x) + (double)(y - p->click_last_y) * (y - p->click_last_y) < 4.0 * p->zoom * p->zoom) idx = p->click_last_idx + 1;
    else idx = 0;
    p->click_set = 0; p->click_have = 1; p->click_last_idx = idx; p->click_last_x = x; p->click_last_y = y; p->click_last_ms = now;
    if (p->avm2) avm2_click_index(p, idx);
}
/* mouse wheel: delta in lines (positive = up / away from the user) */
void player_mouse_wheel(Player *p, int delta) {
    extern void avm2_mouse_wheel(Player *p, int delta);
    extern void avm1_input_wheel(Player *p, int delta);
    { extern int avm1_mix_active(Player *p); if (p->avm2 && avm1_mix_active(p)) avm1_input_wheel(p, delta); }
    if (p->avm2) avm2_mouse_wheel(p, delta); else avm1_input_wheel(p, delta);
}

/* Flash keyPress codes for non-printable keys */
static int keypress_code(int vk) {
    switch (vk) {
    case 37: return 1; case 39: return 2; case 36: return 3; case 35: return 4; case 45: return 5;
    case 46: return 6; case 8: return 8; case 13: return 13; case 38: return 14; case 40: return 15;
    case 33: return 16; case 34: return 17; case 9: return 18; case 27: return 19; case 32: return 32;
    default: return 0;
    }
}

static void key_buttons(Player *p, MovieClip *c, int keycode) {
    DisplayObject *d, *next;
    extern void button_key(Player *p, DisplayObject *d, int keycode);
    for (d = c->children; d; d = next) {
        next = d->next;
        if (d->clip && CLIP_A1MIX(d->clip)) continue;   /* AVM1Movie: the AVM1 input handles it */
        if (d->ch && d->ch->type == CHAR_BUTTON) button_key(p, d, keycode);
        else if (d->clip) key_buttons(p, d->clip, keycode);
    }
}

void player_key(Player *p, int keycode, int down) { player_key_ex(p, keycode, down, 0, -1); }
void player_key_ex(Player *p, int keycode, int down, int location, int ch) {
    if (keycode < 0 || keycode > 255) return;
    p->key_ev_loc = location; p->key_ev_char = ch;
    if (down && !p->key_down[keycode]) p->key_toggled[keycode] ^= 1;
    p->key_down[keycode] = down ? 1 : 0;
    if (!p->avm2) { extern void avm1_input_key(Player *p, int keycode, int down); if (down) p->last_key = keycode; avm1_input_key(p, keycode, down); return; }
    if (down) { int kp = keypress_code(keycode); p->last_key = keycode; if (kp) key_buttons(p, p->root, kp); }
    { extern int avm1_mix_active(Player *p); extern void avm1_input_key(Player *p, int keycode, int down); if (avm1_mix_active(p)) avm1_input_key(p, keycode, down); }   /* AVM1Movie content: Key listeners, clip key events */
    if (p->avm2) avm2_key(p, keycode, down);
}

