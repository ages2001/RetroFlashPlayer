/* dobj_geom.c - display object geometry shared by both script engines:
 * world matrices, bounds and point hit tests. */
#include "player.h"
#include "bmd.h"
#include "shape.h"
#include "reader.h"
#include "textfield.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/* drawing API shapes and script text fields of either VM */
extern ShapeDef *a1_clip_drawing(MovieClip *c);
extern struct TextFieldState *a1_clip_textfield(MovieClip *c);
extern ShapeDef *a2_clip_drawing(MovieClip *c);
extern int a2_clip_visible_bounds(MovieClip *c, SWFRect *r);
extern int a2_clip_hit_bounds(MovieClip *c, SWFRect *r);

static int to_local(const SWFMatrix *w, double sx, double sy, double *lx, double *ly);
/* the matrix of an object's content: its matrix, then its scrollRect's translation */
void dobj_content_matrix(const DisplayObject *d, SWFMatrix *out) {
    *out = d->matrix;
    if (d->has_scroll_applied) {
        SWFMatrix t;
        swf_matrix_identity(&t); t.tx = -(double)d->scroll_applied.xmin; t.ty = -(double)d->scroll_applied.ymin;
        swf_matrix_mul(out, &d->matrix, &t);
    }
}
/* parent world matrix -> content world matrix of `d`; 0 when the stage point
 * (sx, sy) lies outside d's scrollRect */
int dobj_enter(const DisplayObject *d, const SWFMatrix *pw, double sx, double sy, SWFMatrix *w) {
    SWFMatrix cm;
    dobj_content_matrix(d, &cm);
    swf_matrix_mul(w, pw, &cm);
    if (d->has_scroll_applied) {
        double lx, ly;
        if (!to_local(w, sx, sy, &lx, &ly)) return 0;
        if (lx < d->scroll_applied.xmin || lx >= d->scroll_applied.xmax || ly < d->scroll_applied.ymin || ly >= d->scroll_applied.ymax) return 0;
    }
    return 1;
}

void clip_world_matrix(MovieClip *c, SWFMatrix *out) {
    SWFMatrix m;
    swf_matrix_identity(out);
    while (c && c->self) {
        SWFMatrix cm; dobj_content_matrix(c->self, &cm);
        swf_matrix_mul(&m, &cm, out);
        *out = m;
        c = c->parent;
    }
}

/* Flash's twips transform: f32 products, round half to even, integer translation */
static s32 round_even_f(float f) {
    double d = f, fl;
    if (f != f || f > 3.4e38f || f < -3.4e38f) return 0;
    if (f >= 2147483648.0f) return (s32)0x80000000;
    if (f < -2147483648.0f) return (s32)0x80000000;
    fl = floor(d);
    if (d - fl > 0.5) fl += 1.0;
    else if (d - fl == 0.5) { if (fmod(fl, 2.0) != 0.0) fl += 1.0; }
    return (s32)fl;
}
void dg_xform(const SWFMatrix *m, s32 x, s32 y, s32 *ox, s32 *oy) {
    volatile float a = (float)m->sx, b = (float)m->r0, c = (float)m->r1, d = (float)m->sy;
    volatile float fx = (float)x, fy = (float)y, p1, p2, sx_, sy_;
    s32 tx = (s32)floor(m->tx + 0.5), ty = (s32)floor(m->ty + 0.5);
    p1 = a * fx; p2 = c * fy; sx_ = p1 + p2;
    p1 = b * fx; p2 = d * fy; sy_ = p1 + p2;
    *ox = (s32)((u32)round_even_f(sx_) + (u32)tx);
    *oy = (s32)((u32)round_even_f(sy_) + (u32)ty);
}

void dg_union(SWFRect *acc, int *any, const SWFRect *r, const SWFMatrix *m) {
    s32 xs[4], ys[4]; int i;
    dg_xform(m, r->xmin, r->ymin, &xs[0], &ys[0]);
    dg_xform(m, r->xmin, r->ymax, &xs[1], &ys[1]);
    dg_xform(m, r->xmax, r->ymin, &xs[2], &ys[2]);
    dg_xform(m, r->xmax, r->ymax, &xs[3], &ys[3]);
    for (i = 0; i < 4; i++) {
        s32 x = xs[i], y = ys[i];
        if (!*any) { acc->xmin = acc->xmax = x; acc->ymin = acc->ymax = y; *any = 1; }
        else {
            if (x < acc->xmin) acc->xmin = x;
            if (x > acc->xmax) acc->xmax = x;
            if (y < acc->ymin) acc->ymin = y;
            if (y > acc->ymax) acc->ymax = y;
        }
    }
}

int g_dg_nostroke = 0;   /* getRect: bounds without strokes */
int dg_char_bounds(SWFChar *ch, SWFRect *out) {
    Reader r;
    if (!ch) return 0;
    switch (ch->type) {
    case CHAR_SHAPE: { ShapeDef *s = shape_get(ch->movie, ch); if (!s) return 0; *out = g_dg_nostroke ? s->edge_bounds : s->bounds; return 1; }
    case CHAR_BITMAP:
        if (ch->tagcode == -1) {   /* AVM1 attachBitmap */
            BitmapData *b = (BitmapData*)ch->parsed;
            if (!b || b->disposed) return 0;
            out->xmin = out->ymin = 0; out->xmax = (s32)b->w * 20; out->ymax = (s32)b->h * 20; return 1;
        }
        return 0;
    case CHAR_TEXT: case CHAR_EDITTEXT: case CHAR_MORPH:
        reader_init(&r, ch->data, ch->len); r_u16(&r); r_rect(&r, out);
        if (ch->type == CHAR_MORPH && g_dg_nostroke && ch->tagcode == TAG_DefineMorphShape2) { r_rect(&r, out); r_rect(&r, out); }   /* start edge bounds */
        return 1;
    case CHAR_VIDEO: { extern int video_bounds(SWFChar *ch, SWFRect *out); return video_bounds(ch, out); }
    default: return 0;
    }
}

/* full_width: SWF5-7 movies (before Flash 8's strokes) widen the drawing API bounds
 * by the whole stroke width, control points included */
static void drawing_bounds_w(ShapeDef *dr, SWFRect *out, int *any, int full_width) {
    int i; SWFRect r; SWFMatrix id;
    if (!dr || !dr->nedges) return;
    swf_matrix_identity(&id);
    r.xmin = r.xmax = dr->edges[0].x0; r.ymin = r.ymax = dr->edges[0].y0;
    for (i = 0; i < dr->nedges; i++) {
        ShapeEdge *e = &dr->edges[i];
        if (!g_dg_nostroke && e->line > 0 && e->line <= dr->nlines) {   /* strokes widen the bounds by half their width */
            double hw = dr->lines[e->line - 1].width / (full_width ? 1.0 : 2.0), mnx = e->x0 < e->x1 ? e->x0 : e->x1, mxx = e->x0 > e->x1 ? e->x0 : e->x1, mny = e->y0 < e->y1 ? e->y0 : e->y1, mxy = e->y0 > e->y1 ? e->y0 : e->y1;
            if (full_width && e->is_curve) { if (e->cx < mnx) mnx = e->cx; if (e->cx > mxx) mxx = e->cx; if (e->cy < mny) mny = e->cy; if (e->cy > mxy) mxy = e->cy; }
            if (mnx - hw < r.xmin) r.xmin = mnx - hw;
            if (mxx + hw > r.xmax) r.xmax = mxx + hw;
            if (mny - hw < r.ymin) r.ymin = mny - hw;
            if (mxy + hw > r.ymax) r.ymax = mxy + hw;
        }
        if (e->x0 < r.xmin) r.xmin = e->x0;
        if (e->x0 > r.xmax) r.xmax = e->x0;
        if (e->y0 < r.ymin) r.ymin = e->y0;
        if (e->y0 > r.ymax) r.ymax = e->y0;
        if (e->x1 < r.xmin) r.xmin = e->x1;
        if (e->x1 > r.xmax) r.xmax = e->x1;
        if (e->y1 < r.ymin) r.ymin = e->y1;
        if (e->y1 > r.ymax) r.ymax = e->y1;
        if (e->is_curve && full_width) {   /* SWF < 8: the control point counts */
            if (e->cx < r.xmin) r.xmin = e->cx;
            if (e->cx > r.xmax) r.xmax = e->cx;
            if (e->cy < r.ymin) r.ymin = e->cy;
            if (e->cy > r.ymax) r.ymax = e->cy;
        } else if (e->is_curve) {
            /* Ruffle quadratic_curve_bounds: the curve's extremes in pixels, back to twips */
            double ext[2][2]; int k; double hw = 0;
            if (!g_dg_nostroke && e->line > 0 && e->line <= dr->nlines) hw = (double)((s32)dr->lines[e->line - 1].width / 2);
            for (k = 0; k < 2; k++) {
                double f = (k ? e->y0 : e->x0) / 20.0, a = (k ? e->y1 : e->x1) / 20.0, c = (k ? e->cy : e->cx) / 20.0;
                double mn = f < a ? f : a, mx = f > a ? f : a;
                if (c < mn || c > mx) {
                    double t = (f - c) / (f - c * 2.0 + a), q, sv;
                    if (t != t) t = 0; if (t < 0) t = 0; if (t > 1) t = 1;
                    sv = 1.0 - t; q = sv * sv * f + (sv * 2.0) * t * c + t * t * a;
                    if (q < mn) mn = q; if (q > mx) mx = q;
                }
                ext[k][0] = (double)(s32)(mn * 20.0) - hw; ext[k][1] = (double)(s32)(mx * 20.0) + hw;
            }
            if (ext[0][0] < r.xmin) r.xmin = ext[0][0];
            if (ext[0][1] > r.xmax) r.xmax = ext[0][1];
            if (ext[1][0] < r.ymin) r.ymin = ext[1][0];
            if (ext[1][1] > r.ymax) r.ymax = ext[1][1];
        }
    }
    dg_union(out, any, &r, &id);
}
static void drawing_bounds(ShapeDef *dr, SWFRect *out, int *any) { drawing_bounds_w(dr, out, any, 0); }

/* bounds of a clip's content in its own coordinate space (twips) */
/* (Flash composes the matrices down the tree and transforms each leaf's
   rectangle once: DisplayObject::bounds_with_transform) */
struct RMat;
static void bounds_rm(MovieClip *c, const struct RMat *m, SWFRect *out, int *any);
void dg_local_bounds(MovieClip *c, SWFRect *out, int *any) {
    struct { float a, b, c, d; s32 tx, ty; } id = { 1.0f, 0.0f, 0.0f, 1.0f, 0, 0 };
    if (c->self && c->self->has_scroll_applied) {   /* a scroll rect replaces the content's bounds */
        out->xmin = out->ymin = 0; out->xmax = c->self->scroll_applied.xmax - c->self->scroll_applied.xmin; out->ymax = c->self->scroll_applied.ymax - c->self->scroll_applied.ymin;
        *any = 1; return;
    }
    bounds_rm(c, (const struct RMat*)&id, out, any);
}

/* bounds of a placed object in its parent's space; 0 when empty */
int dg_dobj_bounds(DisplayObject *d, const SWFMatrix *m, SWFRect *out) {
    int any = 0; SWFRect l;
    l.xmin = l.xmax = l.ymin = l.ymax = 0;
    if (d->clip) dg_local_bounds(d->clip, &l, &any);
    else if (dg_char_bounds(d->ch, &l)) any = 1;
    if (!any) { out->xmin = out->xmax = out->ymin = out->ymax = 0; return 0; }
    any = 0;
    dg_union(out, &any, &l, m ? m : &d->matrix);
    return 1;
}

void clip_bounds(void *vm, MovieClip *c, SWFRect *out) {
    int any = 0; SWFRect l;
    (void)vm;
    l.xmin = l.xmax = l.ymin = l.ymax = 0;
    dg_local_bounds(c, &l, &any);
    if (!any) { out->xmin = out->xmax = out->ymin = out->ymax = 0; return; }
    if (c->self) { int a2 = 0; SWFRect o; dg_union(&o, &a2, &l, &c->self->matrix); *out = o; }
    else *out = l;
}

/* point-in-shape (shape space twips), after Ruffle's shape_hit_test: edges
   with a fill on exactly one side add to one winding number (even-odd, or
   non-zero for DefineShape4 UsesFillWindingRule); strokes hit within half
   their width (at least min_width, one pixel in stage space) */
static int wind_line(double px, double py, double x0, double y0, double x1, double y1) {
    double d0x = px - x0, d0y = py - y0, d1x = x1 - x0, d1y = y1 - y0;
    if (y0 <= py && py < y1 && d1x * d0y >= d1y * d0x) return 1;
    if (y1 <= py && py < y0 && d1x * d0y <= d1y * d0x) return -1;
    return 0;
}
static int stroke_line(double px, double py, double x0, double y0, double x1, double y1, double w, double w2) {
    double abx, aby, apx, apy, dot_a, dist;
    if (px < (x0 < x1 ? x0 : x1) - w || px > (x0 > x1 ? x0 : x1) + w) return 0;
    if (py < (y0 < y1 ? y0 : y1) - w || py > (y0 > y1 ? y0 : y1) + w) return 0;
    abx = x1 - x0; aby = y1 - y0; apx = px - x0; apy = py - y0;
    dot_a = abx * apx + aby * apy;
    if (dot_a <= 0.0) dist = apx * apx + apy * apy;
    else {
        double bpx = px - x1, bpy = py - y1, dot_b = abx * bpx + aby * bpy;
        if (dot_b >= 0.0) dist = bpx * bpx + bpy * bpy;
        else { double len = abx * abx + aby * aby, ex = apx - dot_a * abx / len, ey = apy - dot_a * aby / len; dist = ex * ex + ey * ey; }
    }
    return dist <= w2;
}
/* per_fill: drawing API shapes count every fill path on its own */
int dg_shape_hit_ex(ShapeDef *s, double px, double py, double min_width, int per_fill) {
    int i, ng = 1, hit = 0, *wind;
    px = floor(px + 0.5); py = floor(py + 0.5);
    for (i = 0; i < s->nedges; i++) {
        int g = per_fill ? (s->edges[i].fill0 > s->edges[i].fill1 ? s->edges[i].fill0 : s->edges[i].fill1) : s->edges[i].layer;
        if (g + 1 > ng) ng = g + 1;
    }
    wind = (int*)calloc((size_t)ng, sizeof(int));
    if (!wind) return 0;
    for (i = 0; i < s->nedges && !hit; i++) {
        ShapeEdge *e = &s->edges[i];
        int k, n = e->is_curve ? 16 : 1;
        double lx = e->x0, ly = e->y0, w = 0, w2 = 0;
        if (e->line > 0 && e->line <= s->nlines) {
            /* the stroke hits with its width in whole stage pixels (at least one) */
            double lw = s->lines[e->line - 1].width;
            if (min_width > 0) { double px = floor(lw / min_width + 1e-9); lw = (px < 1 ? 1 : px) * min_width; }
            w = 0.5 * lw; w2 = w * w;
        }
        for (k = 1; k <= n; k++) {
            double t = (double)k / n, u = 1 - t, nx, ny;
            if (e->is_curve) { nx = u * u * e->x0 + 2 * u * t * e->cx + t * t * e->x1; ny = u * u * e->y0 + 2 * u * t * e->cy + t * t * e->y1; }
            else { nx = e->x1; ny = e->y1; }
            if (per_fill) {
                if (e->fill1) wind[e->fill1] += wind_line(px, py, lx, ly, nx, ny);
                if (e->fill0 && e->fill0 != e->fill1) wind[e->fill0] += wind_line(px, py, nx, ny, lx, ly);
            } else if (e->fill1 && !e->fill0) wind[e->layer] += wind_line(px, py, lx, ly, nx, ny);
            else if (e->fill0 && !e->fill1) wind[e->layer] += wind_line(px, py, nx, ny, lx, ly);
            if (w > 0 && stroke_line(px, py, lx, ly, nx, ny, w, w2)) { hit = 1; break; }
            lx = nx; ly = ny;
        }
    }
    for (i = 0; i < ng && !hit; i++) if (s->nonzero ? wind[i] != 0 : (wind[i] & 1) != 0) hit = 1;
    free(wind);
    return hit;
}
int dg_shape_hit(ShapeDef *s, double px, double py, double min_width) { return dg_shape_hit_ex(s, px, py, min_width, 0); }
int dg_shape_contains(ShapeDef *s, double px, double py) { return dg_shape_hit(s, px, py, 20.0); }
/* one stage pixel in the local space of world matrix w */
double dg_min_stroke(const SWFMatrix *w) {
    double a, b, c, d, e, f, sx, sy;
    extern int matrix_invert(const SWFMatrix *m, double *a, double *b, double *c, double *d, double *e, double *f);
    if (!matrix_invert(w, &a, &b, &c, &d, &e, &f)) return 20.0;
    sx = sqrt(a * a + c * c); sy = sqrt(b * b + d * d);
    return 20.0 * (sx > sy ? sx : sy);
}

static int dobj_hit(DisplayObject *d, const SWFMatrix *parent_world, double sx, double sy, int shape);

static int to_local(const SWFMatrix *w, double sx, double sy, double *lx, double *ly) {
    double a, b, c, d, e, f;
    if (!matrix_invert(w, &a, &b, &c, &d, &e, &f)) return 0;
    *lx = a * sx + b * sy + e; *ly = c * sx + d * sy + f;
    return 1;
}

static int clip_hit_children(MovieClip *c, const SWFMatrix *world, double sx, double sy, int shape) {
    DisplayObject *d; double lx, ly;
    {
        struct TextFieldState *t = a1_clip_textfield(c);
        if (t) { if (!to_local(world, sx, sy, &lx, &ly)) return 0; return lx >= t->xmin && lx <= t->xmin + t->w && ly >= t->ymin && ly <= t->ymin + t->h; }
    }
    for (d = c->children; d; d = d->next) {
        if (d->state_mask && !(d->state_mask & 8)) continue;    /* buttons hit-test with the hit state only */
        if (d->clip_depth > 0) continue;
        if (dobj_hit(d, world, sx, sy, shape)) return 1;
    }
    {
        ShapeDef *dr = a1_clip_drawing(c);
        if (!dr && c->obj2) dr = a2_clip_drawing(c);
        if (dr && to_local(world, sx, sy, &lx, &ly)) {
            extern int a1_drawing_pending_close(MovieClip *c, ShapeEdge *e);
            ShapeEdge pe; int hit, added = 0;
            if (dr == a1_clip_drawing(c) && a1_drawing_pending_close(c, &pe) && dr->nedges < dr->capedges) { dr->edges[dr->nedges++] = pe; added = 1; }
            hit = dg_shape_hit_ex(dr, lx, ly, dg_min_stroke(world), 1);
            if (added) dr->nedges--;
            if (hit) return 1;
        }
        if (c->obj2) { SWFRect r; if (a2_clip_hit_bounds(c, &r) && to_local(world, sx, sy, &lx, &ly)) { if (lx >= r.xmin && lx <= r.xmax && ly >= r.ymin && ly <= r.ymax) return 1; } }
    }
    return 0;
}

/* AVM2 mouse targeting: does `d` (placed under parent world matrix pw) contain the stage point? */
int clip_dobj_hit_test(void *vmp, DisplayObject *d, const SWFMatrix *pw, double sx, double sy) {
    SWFMatrix w;
    (void)vmp;
    if (!d->ch && d->clip && !CLIP_A1MIX(d->clip)) { if (!dobj_enter(d, pw, sx, sy, &w)) return 0; return clip_hit_children(d->clip, &w, sx, sy, 1); }
    return dobj_hit(d, pw, sx, sy, 1);
}

static int dobj_hit(DisplayObject *d, const SWFMatrix *pw, double sx, double sy, int shape) {
    SWFMatrix w; double lx, ly; SWFRect r;
    if (!d->ch && !d->clip) return 0;
    if (!dobj_enter(d, pw, sx, sy, &w)) return 0;
    if (d->clip && d->clip->player->avm2 && CLIP_A1MIX(d->clip)) { extern int avm1_hit_shape_in(void *vm, MovieClip *c, const SWFMatrix *w, double sx, double sy); return avm1_hit_shape_in(d->clip->player->avm, d->clip, &w, sx, sy); }   /* AVM1Movie content */
    if (d->clip) return clip_hit_children(d->clip, &w, sx, sy, shape);
    if (!to_local(&w, sx, sy, &lx, &ly)) return 0;
    if (d->ch->type == CHAR_SHAPE && shape) {
        ShapeDef *s = shape_get(d->ch->movie, d->ch);
        return s ? dg_shape_hit(s, lx, ly, dg_min_stroke(&w)) : 0;
    }
    if (!dg_char_bounds(d->ch, &r)) return 0;
    if (!(lx >= r.xmin && lx <= r.xmax && ly >= r.ymin && ly <= r.ymax)) return 0;
    if (d->ch->type == CHAR_TEXT && shape) { extern int text_hit_glyphs(SWFMovie *mv, SWFChar *ch, double lx, double ly); return text_hit_glyphs(d->ch->movie, d->ch, lx, ly); }
    return 1;
}

/* AVM1 hitTest(x, y, true) (MovieClip::hit_test_shape with AVM_HIT_TEST):
   masks limit what can hit, clip layers hide the depths they mask, objects
   used as masks never hit */
enum { HT_SKIP_MASK = 1, HT_SKIP_INVISIBLE = 2, HT_SKIP_CHILDREN = 4 };
extern MovieClip *a1_clip_masker(MovieClip *c);
extern MovieClip *a2_clip_mask(MovieClip *c);
extern int a1_clip_is_mask(MovieClip *c);
static int ht_clip(MovieClip *c, const SWFMatrix *w, double sx, double sy, int opts);
static void aabb_of(const SWFMatrix *m, const SWFRect *r, SWFRect *out) {
    double xs[4], ys[4]; int i; double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    xs[0] = r->xmin; ys[0] = r->ymin; xs[1] = r->xmax; ys[1] = r->ymin; xs[2] = r->xmin; ys[2] = r->ymax; xs[3] = r->xmax; ys[3] = r->ymax;
    for (i = 0; i < 4; i++) {
        double x = m->sx * xs[i] + m->r1 * ys[i] + m->tx, y = m->r0 * xs[i] + m->sy * ys[i] + m->ty;
        if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
    }
    out->xmin = (s32)floor(x0); out->ymin = (s32)floor(y0); out->xmax = (s32)ceil(x1); out->ymax = (s32)ceil(y1);
}
static int ht_obj(DisplayObject *d, const SWFMatrix *pw, double sx, double sy, int opts) {
    SWFMatrix w; double lx, ly; SWFRect r;
    if (!d->ch && !d->clip) return 0;
    if (d->clip) {
        if ((opts & HT_SKIP_INVISIBLE) && !d->visible && !a1_clip_is_mask(d->clip)) return 0;
        if ((opts & HT_SKIP_MASK) && a1_clip_is_mask(d->clip)) return 0;
    } else if ((opts & HT_SKIP_INVISIBLE) && !d->visible) return 0;
    if (!dobj_enter(d, pw, sx, sy, &w)) return 0;
    if (d->clip) return ht_clip(d->clip, &w, sx, sy, opts);
    if (!to_local(&w, sx, sy, &lx, &ly)) return 0;
    if (d->ch->type == CHAR_SHAPE) { ShapeDef *s = shape_get(d->ch->movie, d->ch); return s ? dg_shape_hit(s, lx, ly, dg_min_stroke(&w)) : 0; }
    if (d->ch->type == CHAR_MORPH) {
        extern int morph_shape_at(SWFMovie *mv, SWFChar *ch, int ratio, ShapeDef *out);
        extern void morph_shape_release(ShapeDef *s);
        ShapeDef ms; int hit;
        if (!morph_shape_at(d->ch->movie, d->ch, d->ratio, &ms)) return 0;
        hit = dg_shape_hit(&ms, lx, ly, dg_min_stroke(&w));
        morph_shape_release(&ms);
        return hit;
    }
    if (!dg_char_bounds(d->ch, &r)) return 0;
    if (d->ch->type == CHAR_TEXT) {
        /* Flash first tests the axis-aligned world bounds */
        SWFRect wb; aabb_of(&w, &r, &wb);
        if (!(sx >= wb.xmin && sx <= wb.xmax && sy >= wb.ymin && sy <= wb.ymax)) return 0;
    } else if (!(lx >= r.xmin && lx <= r.xmax && ly >= r.ymin && ly <= r.ymax)) return 0;
    if (d->ch->type == CHAR_TEXT) { extern int text_hit_glyphs(SWFMovie *mv, SWFChar *ch, double lx, double ly); return text_hit_glyphs(d->ch->movie, d->ch, lx, ly); }
    return 1;
}
static int ht_clip(MovieClip *c, const SWFMatrix *w, double sx, double sy, int opts) {
    DisplayObject *d; double lx, ly; MovieClip *m; int clip_depth = 0;
    {
        struct TextFieldState *t = a1_clip_textfield(c);
        if (t) { SWFRect lb, wb; lb.xmin = t->xmin; lb.ymin = t->ymin; lb.xmax = t->xmin + t->w; lb.ymax = t->ymin + t->h; aabb_of(w, &lb, &wb); (void)lx; (void)ly; return sx >= wb.xmin && sx <= wb.xmax && sy >= wb.ymin && sy <= wb.ymax; }   /* world bounds */
    }
    if ((m = a1_clip_masker(c)) != NULL) {
        SWFMatrix mw; clip_world_matrix(m, &mw);
        if (!ht_clip(m, &mw, sx, sy, HT_SKIP_INVISIBLE)) return 0;
    }
    if (!(opts & HT_SKIP_CHILDREN)) for (d = c->children; d; d = d->next) {
        if (d->state_mask && !(d->state_mask & 8)) continue;    /* buttons hit-test with the hit state only */
        if (d->clip_depth > 0) {
            if (ht_obj(d, w, sx, sy, HT_SKIP_MASK | HT_SKIP_INVISIBLE)) clip_depth = 0; else clip_depth = d->clip_depth;
        } else if (d->depth >= clip_depth && ht_obj(d, w, sx, sy, opts)) return 1;
    }
    {
        ShapeDef *dr = a1_clip_drawing(c);
        if (!dr && c->obj2) dr = a2_clip_drawing(c);
        if (dr && to_local(w, sx, sy, &lx, &ly)) {
            extern int a1_drawing_pending_close(MovieClip *c, ShapeEdge *e);
            ShapeEdge pe; int hit, added = 0;
            if (dr == a1_clip_drawing(c) && a1_drawing_pending_close(c, &pe) && dr->nedges < dr->capedges) { dr->edges[dr->nedges++] = pe; added = 1; }
            hit = dg_shape_hit_ex(dr, lx, ly, dg_min_stroke(w), 1);
            if (added) dr->nedges--;
            if (hit) return 1;
        }
        if (c->obj2) { SWFRect r; if (a2_clip_hit_bounds(c, &r) && to_local(w, sx, sy, &lx, &ly) && lx >= r.xmin && lx <= r.xmax && ly >= r.ymin && ly <= r.ymax) return 1; }   /* bitmaps, text */
    }
    return 0;
}

/* getObjectsUnderPoint: may the object's children be under the point? (not when it is invisible
   or its mask does not cover the point) */
int clip_dobj_children_hittable(DisplayObject *d, double sx, double sy) {
    MovieClip *m;
    if (!d->visible && !(d->clip && a1_clip_is_mask(d->clip))) return 0;
    if (d->clip && ((m = a1_clip_masker(d->clip)) != NULL || (d->clip->obj2 && (m = a2_clip_mask(d->clip)) != NULL))) {
        SWFMatrix mw; clip_world_matrix(m, &mw);
        if (!ht_clip(m, &mw, sx, sy, 0)) return 0;
    }
    return 1;
}
/* getObjectsUnderPoint: the object's own content (not its children, not masks or invisible objects) */
int clip_dobj_hit_own(DisplayObject *d, const SWFMatrix *pw, double sx, double sy) { return ht_obj(d, pw, sx, sy, HT_SKIP_MASK | HT_SKIP_INVISIBLE | HT_SKIP_CHILDREN); }

/* hitTestPoint without shapeFlag tests the axis-aligned box of the bounds in
   stage space (a rotated object's box is larger than the object) */
int dg_world_box_contains(const SWFMatrix *w, const SWFRect *l, double sx, double sy) {
    double xs[4], ys[4], x0, x1, y0, y1; int i;
    swf_matrix_apply(w, l->xmin, l->ymin, &xs[0], &ys[0]); swf_matrix_apply(w, l->xmax, l->ymin, &xs[1], &ys[1]);
    swf_matrix_apply(w, l->xmin, l->ymax, &xs[2], &ys[2]); swf_matrix_apply(w, l->xmax, l->ymax, &xs[3], &ys[3]);
    x0 = x1 = xs[0]; y0 = y1 = ys[0];
    for (i = 1; i < 4; i++) { if (xs[i] < x0) x0 = xs[i]; if (xs[i] > x1) x1 = xs[i]; if (ys[i] < y0) y0 = ys[i]; if (ys[i] > y1) y1 = ys[i]; }
    return sx >= x0 && sx <= x1 && sy >= y0 && sy <= y1;
}
int clip_hit_test_point(void *vm, MovieClip *c, double sx, double sy, int shape) {
    SWFMatrix w;
    (void)vm;
    clip_world_matrix(c, &w);
    if (!shape) {
        SWFRect l; int any = 0;
        if (c->self && c->self->has_scroll_applied) { SWFMatrix pw, t; clip_world_matrix(c->parent, &pw); if (!c->parent) swf_matrix_identity(&pw); swf_matrix_mul(&t, &pw, &c->self->matrix); w = t; }   /* the scroll window lies in the object's own space */
        l.xmin = l.ymin = l.xmax = l.ymax = 0;
        dg_local_bounds(c, &l, &any);
        if (!any) return 0;
        return dg_world_box_contains(&w, &l, sx, sy);
    }
    if (c->self && a1_clip_is_mask(c)) return 0;
    if (c->self && c->self->has_scroll_applied) {   /* only inside its scrollRect window */
        double lx, ly; if (!to_local(&w, sx, sy, &lx, &ly)) return 0;
        if (lx < c->self->scroll_applied.xmin || lx >= c->self->scroll_applied.xmax || ly < c->self->scroll_applied.ymin || ly >= c->self->scroll_applied.ymax) return 0;
    }
    return ht_clip(c, &w, sx, sy, HT_SKIP_MASK);
}

/* ---------------------------------------------------------------- */
/* Flash's matrix arithmetic: f32 scale/skew, integer twips translation,
   products rounded half to even (used for script-visible coordinates) */

typedef struct RMat { float a, b, c, d; s32 tx, ty; } RMat;   /* also in avm1_int.h */

static s32 rm_round(float f) {
    if (f != f || f > 3.4e38f || f < -3.4e38f) return 0;
    if (f >= 2147483648.0f) return (s32)0x80000000;
    return round_even_f(f);
}
void rm_from(const SWFMatrix *m, RMat *o) {
    o->a = (float)m->sx; o->b = (float)m->r0; o->c = (float)m->r1; o->d = (float)m->sy;
    o->tx = (s32)floor(m->tx + 0.5); o->ty = (s32)floor(m->ty + 0.5);
}
void rm_identity(RMat *o) { o->a = o->d = 1.0f; o->b = o->c = 0.0f; o->tx = o->ty = 0; }
void rm_mul(const RMat *x, const RMat *y, RMat *o) {
    RMat r; volatile float t1, t2;
    t1 = x->a * (float)y->tx; t2 = x->c * (float)y->ty; r.tx = (s32)((u32)rm_round(t1 + t2) + (u32)x->tx);
    t1 = x->b * (float)y->tx; t2 = x->d * (float)y->ty; r.ty = (s32)((u32)rm_round(t1 + t2) + (u32)x->ty);
    t1 = x->a * y->a; t2 = x->c * y->b; r.a = t1 + t2;
    t1 = x->b * y->a; t2 = x->d * y->b; r.b = t1 + t2;
    t1 = x->a * y->c; t2 = x->c * y->d; r.c = t1 + t2;
    t1 = x->b * y->c; t2 = x->d * y->d; r.d = t1 + t2;
    *o = r;
}
int rm_inverse(const RMat *m, RMat *o) {
    volatile float t1, t2, det; float tx = (float)m->tx, ty = (float)m->ty; RMat r;
    t1 = m->a * m->d; t2 = m->b * m->c; det = t1 - t2;
    if (!((det < 0 ? -det : det) > 1.1920929e-07f)) return 0;
    r.a = m->d / det; r.b = m->b / -det; r.c = m->c / -det; r.d = m->a / det;
    t1 = m->d * tx; t2 = m->c * ty; r.tx = rm_round((t1 - t2) / -det);
    t1 = m->b * tx; t2 = m->a * ty; r.ty = rm_round((t1 - t2) / det);
    *o = r;
    return 1;
}
void rm_apply(const RMat *m, s32 x, s32 y, s32 *ox, s32 *oy) {
    volatile float t1, t2;
    t1 = m->a * (float)x; t2 = m->c * (float)y; *ox = (s32)((u32)rm_round(t1 + t2) + (u32)m->tx);
    t1 = m->b * (float)x; t2 = m->d * (float)y; *oy = (s32)((u32)rm_round(t1 + t2) + (u32)m->ty);
}
/* DisplayObject::local_to_global_matrix */
void rm_local_to_global(MovieClip *c, RMat *o) {
    RMat m, p;
    if (!c || !c->self) { rm_identity(o); return; }
    rm_from(&c->self->matrix, &m);
    for (c = c->parent; c && c->self; c = c->parent) { rm_from(&c->self->matrix, &p); rm_mul(&p, &m, &m); }
    *o = m;
}
s32 tw_from_px(double px) { double t = px * 20.0; if (t != t) return 0; if (t >= 2147483647.0) return 2147483647; if (t <= -2147483648.0) return (s32)0x80000000; return (s32)t; }

/* the rectangle r through m (corners, rounded twips) into the accumulated bounds */
static void rm_union(SWFRect *acc, int *any, const SWFRect *r, const RMat *m) {
    s32 xs[4], ys[4]; int i;
    rm_apply(m, r->xmin, r->ymin, &xs[0], &ys[0]); rm_apply(m, r->xmin, r->ymax, &xs[1], &ys[1]);
    rm_apply(m, r->xmax, r->ymin, &xs[2], &ys[2]); rm_apply(m, r->xmax, r->ymax, &xs[3], &ys[3]);
    for (i = 0; i < 4; i++) {
        if (!*any) { acc->xmin = acc->xmax = xs[i]; acc->ymin = acc->ymax = ys[i]; *any = 1; }
        else {
            if (xs[i] < acc->xmin) acc->xmin = xs[i];
            if (xs[i] > acc->xmax) acc->xmax = xs[i];
            if (ys[i] < acc->ymin) acc->ymin = ys[i];
            if (ys[i] > acc->ymax) acc->ymax = ys[i];
        }
    }
}
static void bounds_rm(MovieClip *c, const RMat *m, SWFRect *out, int *any) {
    DisplayObject *d; SWFRect r; int rany;
    {
        struct TextFieldState *t = a1_clip_textfield(c);
        if (t) { tfs_apply_autosize(c->player, t); r.xmin = t->xmin; r.ymin = t->ymin; r.xmax = t->xmin + t->w; r.ymax = t->ymin + t->h; rm_union(out, any, &r, m); return; }
    }
    if (c->self && c->self->ch && c->self->ch->type == CHAR_VIDEO) {   /* a Video: its declared size (Ruffle self_bounds) */
        extern int video_bounds(SWFChar *ch, SWFRect *out);
        if (video_bounds(c->self->ch, &r)) rm_union(out, any, &r, m);
        return;
    }
    rany = 0; { extern int clip_swf_version(MovieClip *c); drawing_bounds_w(a1_clip_drawing(c), &r, &rany, c->movie && c->movie->version < 8); }
    if (c->obj2) drawing_bounds(a2_clip_drawing(c), &r, &rany);
    if (rany) rm_union(out, any, &r, m);
    for (d = c->children; d; d = d->next) {
        RMat cm, nm;
        if (d->state_mask && !(d->state_mask & (c->self ? BTN_STATE_BIT(c->self->btn_state) : 1))) continue;
        rm_from(&d->matrix, &cm); rm_mul(m, &cm, &nm);
        if (d->has_scroll_applied) { r.xmin = r.ymin = 0; r.xmax = d->scroll_applied.xmax - d->scroll_applied.xmin; r.ymax = d->scroll_applied.ymax - d->scroll_applied.ymin; rm_union(out, any, &r, &nm); }
        else if (d->clip) bounds_rm(d->clip, &nm, out, any);
        else if (dg_char_bounds(d->ch, &r)) rm_union(out, any, &r, &nm);
    }
    if (c->obj2 && a2_clip_visible_bounds(c, &r)) rm_union(out, any, &r, m);
}

/* bounds in stage space (DisplayObject::world_bounds) */
void dg_world_bounds(MovieClip *c, SWFRect *out, int *any) {
    RMat m; rm_local_to_global(c, &m); bounds_rm(c, &m, out, any);
}
