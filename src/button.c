/* button.c - DefineButton / DefineButton2: states, hit test, actions */
#include "chars.h"
#include "shape.h"
#include "render.h"
#include "reader.h"
#include "avm1.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    int flags; int char_id; int depth; SWFMatrix matrix; SWFCxform cxform;
} ButtonRecord;

typedef struct {
    int cond; int keycode; const u8 *code; size_t len;
} ButtonAction;

typedef struct {
    ButtonRecord *recs; int nrecs;
    ButtonAction *acts; int nacts;
    int menu;
} ButtonDef;

static ButtonDef *button_get(SWFChar *ch) {
    ButtonDef *b; Reader r; int v2 = ch->tagcode == TAG_DefineButton2; int cap = 0;
    u16 action_off = 0; size_t base;
    if (ch->parsed) return (ButtonDef*)ch->parsed;
    b = (ButtonDef*)calloc(1, sizeof(ButtonDef));
    if (!b) return NULL;
    reader_init(&r, ch->data, ch->len);
    r_u16(&r);
    if (v2) { b->menu = r_u8(&r) & 1; base = r.pos; action_off = r_u16(&r); }
    else base = r.pos;
    for (;;) {
        int flags; ButtonRecord rec;
        reader_align(&r);
        flags = r_u8(&r);
        if (flags == 0 || reader_eof(&r)) break;
        rec.flags = flags;
        rec.char_id = r_u16(&r); rec.depth = r_u16(&r);
        r_matrix(&r, &rec.matrix);
        if (v2) r_cxform(&r, &rec.cxform, 1); else swf_cxform_identity(&rec.cxform);
        if (v2 && (flags & 0x10)) {              /* filter list */
            int n = r_u8(&r), i;
            for (i = 0; i < n; i++) {
                int id = r_u8(&r);
                switch (id) { case 0: r_skip(&r, 23); break; case 1: r_skip(&r, 9); break; case 2: r_skip(&r, 15); break; case 3: r_skip(&r, 27); break;
                case 4: case 7: { int nc = r_u8(&r); r_skip(&r, (size_t)nc * 5 + 19); break; }
                case 5: { int mx = r_u8(&r), my = r_u8(&r); r_skip(&r, 8 + (size_t)mx * my * 4 + 5); break; }
                case 6: r_skip(&r, 80); break; default: break; }
            }
        }
        if (v2 && (flags & 0x20)) r_u8(&r);       /* blend */
        if (b->nrecs == cap) { int nc = cap ? cap * 2 : 8; ButtonRecord *nr = (ButtonRecord*)realloc(b->recs, (size_t)nc * sizeof(ButtonRecord)); if (!nr) break; b->recs = nr; cap = nc; }
        b->recs[b->nrecs++] = rec;
    }
    cap = 0;
    if (v2) {
        if (action_off == 0) { ch->parsed = b; return b; }
        r.pos = base + action_off; r.bit = 0;
        for (;;) {
            size_t start = r.pos; u16 size = r_u16(&r); u16 cond = r_u16(&r); ButtonAction a;
            if (r.pos > r.len) break;
            a.cond = cond & 0x1ff; a.keycode = (cond >> 9) & 0x7f;
            a.code = r.buf + r.pos;
            a.len = size ? (size_t)(start + size - r.pos) : (r.len - r.pos);
            if (b->nacts == cap) { int nc = cap ? cap * 2 : 8; ButtonAction *na = (ButtonAction*)realloc(b->acts, (size_t)nc * sizeof(ButtonAction)); if (!na) break; b->acts = na; cap = nc; }
            b->acts[b->nacts++] = a;
            if (size == 0) break;
            r.pos = start + size; r.bit = 0;
        }
    } else {
        ButtonAction a;
        a.cond = 0x08; a.keycode = 0;              /* release */
        a.code = r.buf + r.pos; a.len = r.len - r.pos;
        b->acts = (ButtonAction*)calloc(1, sizeof(ButtonAction));
        if (b->acts) { b->acts[0] = a; b->nacts = 1; }
    }
    ch->parsed = b;
    return b;
}

/* record access for AVM2 SimpleButton state construction */
int button_record_count(SWFChar *ch) { ButtonDef *b = button_get(ch); return b ? b->nrecs : 0; }
int button_record_get(SWFChar *ch, int i, int *flags, int *char_id, int *depth, SWFMatrix *m, SWFCxform *cx) {
    ButtonDef *b = button_get(ch);
    if (!b || i < 0 || i >= b->nrecs) return 0;
    *flags = b->recs[i].flags; *char_id = b->recs[i].char_id; *depth = b->recs[i].depth; *m = b->recs[i].matrix; *cx = b->recs[i].cxform;
    return 1;
}

int button_action_count(SWFChar *ch) { ButtonDef *b = button_get(ch); return b ? b->nacts : 0; }
int button_action_get(SWFChar *ch, int i, int *cond, int *keycode, const u8 **code, size_t *len) {
    ButtonDef *b = button_get(ch);
    if (!b || i < 0 || i >= b->nacts) return 0;
    *cond = b->acts[i].cond; *keycode = b->acts[i].keycode; *code = b->acts[i].code; *len = b->acts[i].len;
    return 1;
}

static int cmp_depth(const void *A, const void *B) { return ((const ButtonRecord*)A)->depth - ((const ButtonRecord*)B)->depth; }

static void render_record(Player *p, ButtonRecord *rec, const SWFMatrix *wm, const SWFCxform *cx) {
    SWFChar *ch = swf_get_char(PMOV(p), rec->char_id);
    SWFMatrix m; SWFCxform c;
    if (!ch) return;
    swf_matrix_mul(&m, wm, &rec->matrix);
    swf_cxform_mul(&c, cx, &rec->cxform);
    switch (ch->type) {
    case CHAR_SHAPE: { ShapeDef *s = shape_get(PMOV(p), ch); if (s) render_shape(p->surf, PMOV(p), s, &m, &c, NULL); break; }
    case CHAR_TEXT: case CHAR_EDITTEXT: { DisplayObject tmp; memset(&tmp, 0, sizeof(tmp)); tmp.ch = ch; tmp.visible = 1; text_render(p, &tmp, &m, &c); break; }
    case CHAR_MORPH: { DisplayObject tmp; memset(&tmp, 0, sizeof(tmp)); tmp.ch = ch; tmp.visible = 1; morph_render(p, &tmp, &m, &c); break; }
    case CHAR_SPRITE: {
        /* sprites inside buttons: render first frame statically */
        extern void button_render_sprite(Player *p, SWFChar *ch, const SWFMatrix *m, const SWFCxform *c);
        button_render_sprite(p, ch, &m, &c);
        break; }
    default: break;
    }
}

void button_render(Player *p, DisplayObject *d, const SWFMatrix *wm, const SWFCxform *cx) {
    ButtonDef *b = button_get(d->ch);
    ButtonRecord *sorted; int i, state_bit;
    if (!b || b->nrecs == 0) return;
    state_bit = (d->btn_state == 2) ? 0x04 : (d->btn_state == 1 ? 0x02 : 0x01);   /* 3 (out-down) draws as up */
    sorted = (ButtonRecord*)malloc((size_t)b->nrecs * sizeof(ButtonRecord));
    if (!sorted) return;
    memcpy(sorted, b->recs, (size_t)b->nrecs * sizeof(ButtonRecord));
    qsort(sorted, (size_t)b->nrecs, sizeof(ButtonRecord), cmp_depth);
    for (i = 0; i < b->nrecs; i++) if (sorted[i].flags & state_bit) render_record(p, &sorted[i], wm, cx);
    free(sorted);
}

static int record_hit(Player *p, ButtonRecord *rec, const SWFMatrix *wm, double sx, double sy) {
    SWFChar *ch = swf_get_char(PMOV(p), rec->char_id);
    SWFMatrix m; double a, b, c, d, e, f, lx, ly;
    if (!ch) return 0;
    swf_matrix_mul(&m, wm, &rec->matrix);
    if (!matrix_invert(&m, &a, &b, &c, &d, &e, &f)) return 0;
    lx = a * sx + b * sy + e; ly = c * sx + d * sy + f;
    if (ch->type == CHAR_SHAPE) {
        ShapeDef *s = shape_get(PMOV(p), ch);
        return s && lx >= s->bounds.xmin && lx <= s->bounds.xmax && ly >= s->bounds.ymin && ly <= s->bounds.ymax;
    }
    {
        Reader r; SWFRect rc;
        if (ch->type == CHAR_SPRITE) {
            extern int button_sprite_hit(Player *p, SWFChar *ch, double lx, double ly);
            return button_sprite_hit(p, ch, lx, ly);
        }
        reader_init(&r, ch->data, ch->len); r_u16(&r); r_rect(&r, &rc);
        return lx >= rc.xmin && lx <= rc.xmax && ly >= rc.ymin && ly <= rc.ymax;
    }
}

/* returns 1 if the mouse is over the button; dispatches actions on transitions */
int button_mouse(Player *p, DisplayObject *d, const SWFMatrix *wm, int mx, int my, int down, int changed) {
    ButtonDef *b = button_get(d->ch);
    int i, hit = 0, old = d->btn_state, nw, cond = 0;
    double sx = mx * TWIPS_PER_PX / p->zoom, sy = my * TWIPS_PER_PX / p->zoom;
    MovieClip *target;
    if (!b) return 0;
    for (i = 0; i < b->nrecs; i++) if ((b->recs[i].flags & 0x08) && record_hit(p, &b->recs[i], wm, sx, sy)) { hit = 1; break; }
    if (hit) nw = down ? 2 : 1; else nw = (down && old == 2) ? 3 : 0;   /* 3 = out-down */
    if (old == 0 && nw == 1) cond = 0x01;                 /* IdleToOverUp */
    else if (old == 1 && nw == 0) cond = 0x02;            /* OverUpToIdle */
    else if (old == 1 && nw == 2 && changed) cond = 0x04; /* OverUpToOverDown: press */
    else if (old == 0 && nw == 2 && changed) cond = 0x04 | 0x80; /* Idle->OverDown (press without hover) */
    else if (old == 2 && nw == 1 && changed) cond = 0x08; /* OverDownToOverUp: release */
    else if (old == 2 && nw == 3) cond = 0x10;            /* OverDownToOutDown */
    else if (old == 3 && nw == 2) cond = 0x20;            /* OutDownToOverDown */
    else if (old == 3 && nw == 0 && changed) cond = 0x40; /* OutDownToIdle: release outside */
    else if (old == 2 && nw == 0 && changed) cond = 0x100;/* OverDownToIdle */
    d->btn_state = nw;
    if (!cond) return hit;
    target = d->owner;
    if (target) for (i = 0; i < b->nacts; i++) {
        if (b->acts[i].cond & cond & 0x1ff) {
            avm1_queue_do_action(p, target, b->acts[i].code, b->acts[i].len);
        }
    }
    /* script handlers on the Button object */
    if (d->clip && p->avm) {
        const char *h = NULL;
        if (cond & 0x01) h = "onRollOver"; else if (cond & 0x02) h = "onRollOut";
        else if (cond & 0x04) h = "onPress"; else if (cond & 0x08) h = "onRelease";
        else if (cond & 0x10) h = "onDragOut"; else if (cond & 0x20) h = "onDragOver";
        else if (cond & 0x40) h = "onReleaseOutside";
        if (h) avm1_button_method(p, d->clip, h);
    }
    return hit;
}

/* key press conditions */
void button_key(Player *p, DisplayObject *d, int keycode) {
    ButtonDef *b = button_get(d->ch); int i;
    MovieClip *target = d->owner;
    if (!b || !target) return;
    for (i = 0; i < b->nacts; i++) if (b->acts[i].keycode && b->acts[i].keycode == keycode) avm1_queue_do_action(p, target, b->acts[i].code, b->acts[i].len);
}

/* ---- static sprite rendering inside button records ---- */

void button_render_sprite(Player *p, SWFChar *ch, const SWFMatrix *m, const SWFCxform *c) {
    Timeline tl; int i;
    timeline_init(&tl);
    swf_sprite_timeline(ch, &tl);
    for (i = 0; i < tl.ntags; i++) {
        const TagRef *t = &tl.tags[i];
        if (t->code == TAG_ShowFrame) break;
        if (t->code == TAG_PlaceObject2 || t->code == TAG_PlaceObject3 || t->code == TAG_PlaceObject) {
            Reader r; int flags, id = -1; SWFMatrix pm; SWFCxform pc; SWFMatrix wm; SWFCxform wc; SWFChar *cch;
            reader_init(&r, t->data, t->len);
            swf_matrix_identity(&pm); swf_cxform_identity(&pc);
            if (t->code == TAG_PlaceObject) { id = r_u16(&r); r_u16(&r); r_matrix(&r, &pm); }
            else {
                int flags2 = 0;
                flags = r_u8(&r); if (t->code == TAG_PlaceObject3) flags2 = r_u8(&r);
                r_u16(&r);
                if (t->code == TAG_PlaceObject3 && ((flags2 & 0x08) || ((flags2 & 0x10) && !(flags & 2)))) r_string(&r);
                if (flags & 0x02) id = r_u16(&r);
                if (flags & 0x04) r_matrix(&r, &pm);
                if (flags & 0x08) r_cxform(&r, &pc, 1);
            }
            if (id < 0) continue;
            cch = swf_get_char(PMOV(p), id);
            if (!cch) continue;
            swf_matrix_mul(&wm, m, &pm); swf_cxform_mul(&wc, c, &pc);
            if (cch->type == CHAR_SHAPE) { ShapeDef *s = shape_get(PMOV(p), cch); if (s) render_shape(p->surf, PMOV(p), s, &wm, &wc, NULL); }
            else if (cch->type == CHAR_TEXT || cch->type == CHAR_EDITTEXT) { DisplayObject tmp; memset(&tmp, 0, sizeof(tmp)); tmp.ch = cch; tmp.visible = 1; text_render(p, &tmp, &wm, &wc); }
            else if (cch->type == CHAR_SPRITE) button_render_sprite(p, cch, &wm, &wc);
        }
    }
    timeline_free(&tl);
}

int button_sprite_hit(Player *p, SWFChar *ch, double lx, double ly) {
    Timeline tl; int i, hit = 0;
    timeline_init(&tl);
    swf_sprite_timeline(ch, &tl);
    for (i = 0; i < tl.ntags && !hit; i++) {
        const TagRef *t = &tl.tags[i];
        if (t->code == TAG_ShowFrame) break;
        if (t->code == TAG_PlaceObject2 || t->code == TAG_PlaceObject3) {
            Reader r; int flags, id = -1; SWFMatrix pm; SWFChar *cch; double a, b, c, d, e, f, x, y;
            reader_init(&r, t->data, t->len);
            swf_matrix_identity(&pm);
            { int flags2 = 0; flags = r_u8(&r); if (t->code == TAG_PlaceObject3) flags2 = r_u8(&r); r_u16(&r);
              if (t->code == TAG_PlaceObject3 && ((flags2 & 0x08) || ((flags2 & 0x10) && !(flags & 2)))) r_string(&r);
              if (flags & 0x02) id = r_u16(&r); if (flags & 0x04) r_matrix(&r, &pm); }
            if (id < 0) continue;
            cch = swf_get_char(PMOV(p), id);
            if (!cch || !matrix_invert(&pm, &a, &b, &c, &d, &e, &f)) continue;
            x = a * lx + b * ly + e; y = c * lx + d * ly + f;
            if (cch->type == CHAR_SHAPE) { ShapeDef *s = shape_get(PMOV(p), cch); if (s && x >= s->bounds.xmin && x <= s->bounds.xmax && y >= s->bounds.ymin && y <= s->bounds.ymax) hit = 1; }
            else if (cch->type == CHAR_SPRITE) hit = button_sprite_hit(p, cch, x, y);
        }
    }
    timeline_free(&tl);
    return hit;
}

void button_free_parsed(void *p) {
    ButtonDef *b = (ButtonDef*)p;
    if (!b) return;
    free(b->recs); free(b->acts); free(b);
}
