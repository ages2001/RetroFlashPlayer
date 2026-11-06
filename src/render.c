/* render.c - draws shapes the way Ruffle's renderer does: a shape becomes
 * an ordered list of draw paths (shape_utils::ShapeConverter: per style
 * layer, the fills in style order, then each stroke segment), fills are
 * filled with their winding rule, strokes are widened like lyon's stroke
 * tessellator (caps, joins, a minimum width of one pixel at the
 * tessellation scale), and both are rasterized by raster_fill. */
#include "render.h"
#include "stroke.h"
#include "image.h"
#include "bmd.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

void render_stage_matrix(SWFMatrix *out, const SWFRect *stage, double zoom) {
    swf_matrix_identity(out);
    out->sx = out->sy = zoom / TWIPS_PER_PX;
    out->tx = -stage->xmin * out->sx;
    out->ty = -stage->ymin * out->sy;
}

/* ---------------------------------------------------------------- */
/* draw paths                                                       */

typedef struct { s32 x, y; int ctl; } PPt;          /* twips; ctl: quadratic control point */
typedef struct { PPt *p; int n, cap; } Seg;
typedef struct {
    int  fill;          /* 1 fill, 0 stroke */
    int  style;         /* 1-based index into the shape's fills / lines */
    int  closed;        /* strokes: the last subpath is closed */
    Seg *segs; int nsegs;   /* subpaths */
} DPath;
typedef struct { DPath *p; int n, cap; } DPathList;

static void seg_push(Seg *s, s32 x, s32 y, int ctl) {
    if (s->n == s->cap) {
        int nc = s->cap ? s->cap * 2 : 8; PPt *np = (PPt*)realloc(s->p, (size_t)nc * sizeof(PPt));
        if (!np) return;
        s->p = np; s->cap = nc;
    }
    s->p[s->n].x = x; s->p[s->n].y = y; s->p[s->n].ctl = ctl; s->n++;
}
static void seg_reset(Seg *s, s32 x, s32 y) { s->n = 0; seg_push(s, x, y, 0); }
static Seg seg_copy(const Seg *s) {
    Seg c; c.n = 0; c.cap = 0; c.p = NULL;
    if (s->n) { c.p = (PPt*)malloc((size_t)s->n * sizeof(PPt)); if (c.p) { memcpy(c.p, s->p, (size_t)s->n * sizeof(PPt)); c.n = c.cap = s->n; } }
    return c;
}
static void seg_flip(Seg *s) { int i; for (i = 0; i < s->n / 2; i++) { PPt t = s->p[i]; s->p[i] = s->p[s->n - 1 - i]; s->p[s->n - 1 - i] = t; } }

typedef struct { Seg *s; int n, cap; } Pending;
static void pend_push(Pending *pp, Seg s) {
    if (pp->n == pp->cap) {
        int nc = pp->cap ? pp->cap * 2 : 4; Seg *ns = (Seg*)realloc(pp->s, (size_t)nc * sizeof(Seg));
        if (!ns) { free(s.p); return; }
        pp->s = ns; pp->cap = nc;
    }
    pp->s[pp->n++] = s;
}
static int pt_eq(const PPt *a, const PPt *b) { return a->x == b->x && a->y == b->y; }
/* PendingPath::add_segment: link onto segments with a matching end point */
static void pend_add_linked(Pending *pp, Seg ns) {
    int start_open = 1, end_open = 1, i = 0;
    if (ns.n <= 1) { free(ns.p); return; }
    while ((start_open || end_open) && i < pp->n) {
        Seg *o = &pp->s[i];
        if (start_open && pt_eq(&o->p[o->n - 1], &ns.p[0])) {
            int k; for (k = 1; k < ns.n; k++) seg_push(o, ns.p[k].x, ns.p[k].y, ns.p[k].ctl);
            free(ns.p); ns = *o; pp->s[i] = pp->s[--pp->n];   /* swap_remove */
            start_open = 0;
        } else if (end_open && pt_eq(&ns.p[ns.n - 1], &o->p[0])) {
            int k; Seg t = *o;
            for (k = 1; k < t.n; k++) seg_push(&ns, t.p[k].x, t.p[k].y, t.p[k].ctl);
            free(t.p); pp->s[i] = pp->s[--pp->n];
            end_open = 0;
        } else i++;
    }
    pend_push(pp, ns);
}

static DPath *dpl_add(DPathList *l) {
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 8; DPath *np = (DPath*)realloc(l->p, (size_t)nc * sizeof(DPath));
        if (!np) return NULL;
        l->p = np; l->cap = nc;
    }
    memset(&l->p[l->n], 0, sizeof(DPath));
    return &l->p[l->n++];
}
static void dpl_free(DPathList *l) {
    int i, k;
    for (i = 0; i < l->n; i++) { for (k = 0; k < l->p[i].nsegs; k++) free(l->p[i].segs[k].p); free(l->p[i].segs); }
    free(l->p); l->p = NULL; l->n = l->cap = 0;
}

typedef struct { int style; Seg seg; } Active;

typedef struct {
    DPathList out;
    Pending *fills, *strokes; int nf, nl;
    Active f0, f1, ln;
    s32 cx, cy;
} Conv;

static void act_flush_fill(Conv *c, Active *a, int flip) {
    if (a->style > 0 && a->seg.n > 1 && a->style <= c->nf) {
        Seg s = seg_copy(&a->seg);
        if (flip) seg_flip(&s);
        pend_add_linked(&c->fills[a->style - 1], s);
    }
    seg_reset(&a->seg, c->cx, c->cy);
}
static void act_flush_stroke(Conv *c, Active *a) {
    if (a->style > 0 && a->seg.n > 1 && a->style <= c->nl) pend_push(&c->strokes[a->style - 1], seg_copy(&a->seg));
    seg_reset(&a->seg, c->cx, c->cy);
}
static void conv_flush_paths(Conv *c) { act_flush_fill(c, &c->f1, 0); act_flush_fill(c, &c->f0, 1); act_flush_stroke(c, &c->ln); }
static void conv_flush_layer(Conv *c) {
    int i, k;
    conv_flush_paths(c);
    for (i = 0; i < c->nf; i++) {
        DPath *d;
        if (!c->fills[i].n) continue;
        d = dpl_add(&c->out);
        if (!d) break;
        d->fill = 1; d->style = i + 1; d->segs = c->fills[i].s; d->nsegs = c->fills[i].n;
        c->fills[i].s = NULL; c->fills[i].n = c->fills[i].cap = 0;
    }
    for (i = 0; i < c->nl; i++) {
        for (k = 0; k < c->strokes[i].n; k++) {
            Seg *s = &c->strokes[i].s[k]; DPath *d;
            if (s->n <= 1) { free(s->p); continue; }
            d = dpl_add(&c->out);
            if (!d) { free(s->p); continue; }
            d->fill = 0; d->style = i + 1; d->closed = pt_eq(&s->p[0], &s->p[s->n - 1]);
            d->segs = (Seg*)malloc(sizeof(Seg));
            if (d->segs) { d->segs[0] = *s; d->nsegs = 1; } else free(s->p);
        }
        free(c->strokes[i].s); c->strokes[i].s = NULL; c->strokes[i].n = c->strokes[i].cap = 0;
    }
}
static void conv_visit(Conv *c, int ctl) {
    if (c->f1.style > 0) seg_push(&c->f1.seg, c->cx, c->cy, ctl);
    if (c->f0.style > 0) seg_push(&c->f0.seg, c->cx, c->cy, ctl);
    if (c->ln.style > 0) seg_push(&c->ln.seg, c->cx, c->cy, ctl);
}

/* ShapeConverter over the parsed edge list */
static void shape_to_paths(const ShapeDef *sh, DPathList *out) {
    Conv c; int i, layer = -1;
    memset(&c, 0, sizeof(c));
    c.nf = sh->nfills; c.nl = sh->nlines;
    c.fills = (Pending*)calloc((size_t)c.nf + 1, sizeof(Pending));
    c.strokes = (Pending*)calloc((size_t)c.nl + 1, sizeof(Pending));
    if (!c.fills || !c.strokes) { free(c.fills); free(c.strokes); return; }
    seg_reset(&c.f0.seg, 0, 0); seg_reset(&c.f1.seg, 0, 0); seg_reset(&c.ln.seg, 0, 0);
    for (i = 0; i < sh->nedges; i++) {
        const ShapeEdge *e = &sh->edges[i];
        int brk = sh->has_brk ? e->brk : 0;
        if (!sh->has_brk) {
            /* built shapes: a jump of the pen or a style change starts new segments */
            if (i == 0 || e->x0 != c.cx || e->y0 != c.cy) brk |= BRK_MOVE;
            if (e->fill0 != c.f0.style) brk |= BRK_FILL0;
            if (e->fill1 != c.f1.style) brk |= BRK_FILL1;
            if (e->line != c.ln.style) brk |= BRK_LINE;
        }
        if (e->layer != layer) {
            if (layer >= 0) conv_flush_layer(&c);
            layer = e->layer;
        }
        if (brk & BRK_MOVE) { c.cx = e->x0; c.cy = e->y0; conv_flush_paths(&c); }
        else if (e->x0 != c.cx || e->y0 != c.cy) { c.cx = e->x0; c.cy = e->y0; conv_flush_paths(&c); }
        if (brk & BRK_FILL1) { act_flush_fill(&c, &c.f1, 0); c.f1.style = e->fill1; }
        if (brk & BRK_FILL0) { act_flush_fill(&c, &c.f0, 1); c.f0.style = e->fill0; }
        if (brk & BRK_LINE) { act_flush_stroke(&c, &c.ln); c.ln.style = e->line; }
        if (e->cubic > 0 && i + e->cubic - 1 < sh->nedges) {
            /* a drawing API cubic: its control points (ctl 2) and anchor, not the lines standing for it */
            const ShapeEdge *last = &sh->edges[i + e->cubic - 1];
            c.cx = e->cx; c.cy = e->cy; conv_visit(&c, 2);
            c.cx = e->c2x; c.cy = e->c2y; conv_visit(&c, 2);
            c.cx = last->x1; c.cy = last->y1; conv_visit(&c, 0);
            i += e->cubic - 1;
            continue;
        }
        if (e->is_curve) { c.cx = e->cx; c.cy = e->cy; conv_visit(&c, 1); }
        c.cx = e->x1; c.cy = e->y1; conv_visit(&c, 0);
    }
    conv_flush_layer(&c);
    free(c.f0.seg.p); free(c.f1.seg.p); free(c.ln.seg.p);
    free(c.fills); free(c.strokes);
    *out = c.out;
}

void shape_free_paths(ShapeDef *s) {
    if (s && s->paths) { dpl_free((DPathList*)s->paths); free(s->paths); s->paths = NULL; }
}

/* ---------------------------------------------------------------- */
/* geometry                                                         */

typedef struct { double x, y; } DPt;
typedef struct { DPt *p; int n, cap; } Poly;
static void poly_push(Poly *l, double x, double y) {
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 64; DPt *np = (DPt*)realloc(l->p, (size_t)nc * sizeof(DPt));
        if (!np) return;
        l->p = np; l->cap = nc;
    }
    l->p[l->n].x = x; l->p[l->n].y = y; l->n++;
}

/* lyon_geom's quadratic flattening (Raph Levien's parabola method, f32), as
   the fill tessellator runs it: the curve is oriented downward first (the
   points are then emitted in the original direction) */
static float lyon_pint(float x) { const float d = 0.67f; return x / (1.0f - d + (float)sqrt(sqrt((double)(d * d * d * d + 0.25f * x * x)))); }
static float lyon_pinv(float x) { const float b = 0.39f; return x * (1.0f - b + (float)sqrt((double)(b * b + 0.25f * x * x))); }
static int lyon_is_after(float ax, float ay, float bx, float by) { return ay > by || (ay == by && ax > bx); }
static int g_flat_fill;   /* flattening for the fill tessellator (curves oriented downward) */
static void flatten_quad(Poly *o, DPt a, DPt c, DPt b, double told) {
    float fx = (float)a.x, fy = (float)a.y, cx = (float)c.x, cy = (float)c.y, tx = (float)b.x, ty = (float)b.y, tol = (float)told;
    int swap = g_flat_fill && lyon_is_after(fx, fy, tx, ty), n, i, k0;
    float count;
    if (swap) { float t; t = fx; fx = tx; tx = t; t = fy; fy = ty; ty = t; }
    k0 = o->n;
    {
        /* is_linear: the control point near the baseline */
        float vx = tx - fx, vy = ty - fy, wx = cx - fx, wy = cy - fy, cr = vx * wy - vy * wx, d;
        if (fx == tx && fy == ty) count = 0;
        else {
            d = (cr * cr) / (vx * vx + vy * vy);
            if (d <= tol * tol * 4.0f) count = 0;
            else {
                float ddx = 2.0f * cx - fx - tx, ddy = 2.0f * cy - fy - ty, cross = (tx - fx) * ddy - (ty - fy) * ddx, inv = 1.0f / cross;
                float pf = ((cx - fx) * ddx + (cy - fy) * ddy) * inv, pt = ((tx - cx) * ddx + (ty - cy) * ddy) * inv;
                float scale = (float)fabs(cross) / ((float)sqrt((double)(ddx * ddx + ddy * ddy)) * (float)fabs(pt - pf));
                float i0 = lyon_pint(pf), i1 = lyon_pint(pt), idiff = i1 - i0;
                float u0 = lyon_pinv(i0), u1 = lyon_pinv(i1), divd = 1.0f / (u1 - u0), step;
                count = (float)ceil(0.5f * (float)fabs(idiff) * (float)sqrt((double)(scale / tol)));
                if (!(count == count) || count > 1e6f || count < -1e6f) count = 0;
                step = idiff / count;
                n = (int)count;
                for (i = 1; i < n; i++) {
                    float u = lyon_pinv(i0 + step * (float)i), t = (u - u0) * divd, t2 = t * t, ot = 1.0f - t, ot2 = ot * ot;
                    poly_push(o, fx * ot2 + cx * 2.0f * ot * t + tx * t2, fy * ot2 + cy * 2.0f * ot * t + ty * t2);
                }
            }
        }
    }
    poly_push(o, tx, ty);
    if (swap) {
        /* emitted from the far end: reverse, ending at the original end point */
        int m = o->n - k0, j;
        /* the points are: interior (from the swapped start), then the swapped end (= original start) */
        o->n--;   /* drop the original start */
        m--;
        for (j = 0; j < m / 2; j++) { DPt t = o->p[k0 + j]; o->p[k0 + j] = o->p[k0 + m - 1 - j]; o->p[k0 + m - 1 - j] = t; }
        poly_push(o, (float)b.x, (float)b.y);
    }
}
/* lyon_geom's cubic flattening: split into quadratics (tolerance x 0.4), each
   flattened with tolerance x 0.8; the fill tessellator orients it downward */
typedef struct { float x, y; } FP;
static FP fp(float x, float y) { FP p; p.x = x; p.y = y; return p; }
static FP cub_sample(FP f, FP a, FP b, FP t2p, float t) {
    float tt = t * t, t3 = tt * t, o = 1.0f - t, o2 = o * o, o3 = o2 * o;
    return fp(f.x * o3 + a.x * 3.0f * o2 * t + b.x * 3.0f * o * tt + t2p.x * t3, f.y * o3 + a.y * 3.0f * o2 * t + b.y * 3.0f * o * tt + t2p.y * t3);
}
static FP quad_samplef(FP f, FP c, FP t2p, float t) { float tt = t * t, o = 1.0f - t, o2 = o * o; return fp(f.x * o2 + c.x * 2.0f * o * t + t2p.x * tt, f.y * o2 + c.y * 2.0f * o * t + t2p.y * tt); }
static void flatten_cubic(Poly *o, DPt a, DPt c1, DPt c2, DPt b, double told) {
    FP f = fp((float)a.x, (float)a.y), p1 = fp((float)c1.x, (float)c1.y), p2 = fp((float)c2.x, (float)c2.y), t = fp((float)b.x, (float)b.y);
    float tol = (float)told, qt = tol * 0.4f, ft = tol * 0.8f, x, y, err, nq, step, t0 = 0.0f; int n, i, swap, k0 = o->n;
    swap = g_flat_fill && lyon_is_after(f.x, f.y, t.x, t.y);
    if (swap) { FP q = f; f = t; t = q; q = p1; p1 = p2; p2 = q; }
    x = f.x - 3.0f * p1.x + 3.0f * p2.x - t.x; y = f.y - 3.0f * p1.y + 3.0f * p2.y - t.y;
    err = x * x + y * y;
    nq = (float)ceil(pow((double)(err / (432.0f * qt * qt)), 1.0 / 6.0)); if (!(nq >= 1.0f)) nq = 1.0f;
    step = 1.0f / nq; n = (int)nq;
    for (i = 0; i < n; i++) {
        float t1 = i < n - 1 ? t0 + step : 1.0f, dt = t1 - t0; FP s0, s1, d0, d1, dq0, dq1, dq2, k1, k2, qc; DPt A, C, B; int sv = g_flat_fill;
        s0 = cub_sample(f, p1, p2, t, t0); s1 = cub_sample(f, p1, p2, t, t1);
        dq0 = fp(p1.x - f.x, p1.y - f.y); dq1 = fp(p2.x - p1.x, p2.y - p1.y); dq2 = fp(t.x - p2.x, t.y - p2.y);
        d0 = quad_samplef(dq0, dq1, dq2, t0); d1 = quad_samplef(dq0, dq1, dq2, t1);
        k1 = fp(s0.x + d0.x * dt, s0.y + d0.y * dt); k2 = fp(s1.x - d1.x * dt, s1.y - d1.y * dt);
        /* to_quadratic */
        { FP a1 = fp((k1.x * 3.0f - s0.x) * 0.5f, (k1.y * 3.0f - s0.y) * 0.5f), a2 = fp((k2.x * 3.0f - s1.x) * 0.5f, (k2.y * 3.0f - s1.y) * 0.5f); qc = fp((a1.x + a2.x) * 0.5f, (a1.y + a2.y) * 0.5f); }
        A.x = s0.x; A.y = s0.y; C.x = qc.x; C.y = qc.y; B.x = s1.x; B.y = s1.y;
        g_flat_fill = 0; flatten_quad(o, A, C, B, ft); g_flat_fill = sv;
        t0 = t1;
    }
    if (swap) {
        /* emitted from the far end: reverse, ending at the original end point */
        int m = o->n - k0, j;
        o->n--; m--;
        for (j = 0; j < m / 2; j++) { DPt q = o->p[k0 + j]; o->p[k0 + j] = o->p[k0 + m - 1 - j]; o->p[k0 + m - 1 - j] = q; }
        poly_push(o, (float)b.x, (float)b.y);
    }
}
/* a subpath (twips points) flattened to shape px */
static void flatten_seg(const Seg *s, Poly *o, double tol) {
    int i; DPt prev;
    o->n = 0;
    if (s->n < 1) return;
    prev.x = s->p[0].x / 20.0; prev.y = s->p[0].y / 20.0;
    poly_push(o, prev.x, prev.y);
    for (i = 1; i < s->n; i++) {
        if (s->p[i].ctl == 2 && i + 2 < s->n) {
            DPt c1, c2, b; c1.x = s->p[i].x / 20.0; c1.y = s->p[i].y / 20.0; c2.x = s->p[i + 1].x / 20.0; c2.y = s->p[i + 1].y / 20.0; b.x = s->p[i + 2].x / 20.0; b.y = s->p[i + 2].y / 20.0;
            flatten_cubic(o, prev, c1, c2, b, tol); prev = b; i += 2;
        } else if (s->p[i].ctl && i + 1 < s->n) {
            DPt c, b; c.x = s->p[i].x / 20.0; c.y = s->p[i].y / 20.0; b.x = s->p[i + 1].x / 20.0; b.y = s->p[i + 1].y / 20.0;
            flatten_quad(o, prev, c, b, tol); prev = b; i++;
        } else { prev.x = s->p[i].x / 20.0; prev.y = s->p[i].y / 20.0; poly_push(o, prev.x, prev.y); }
    }
}

/* shape px -> device px */
extern int g_rmat_compose;
typedef struct { double a, b, c, d, tx, ty; } XF;
static void xf_of(const SWFMatrix *m, XF *x) { x->a = m->sx * 20; x->b = m->r0 * 20; x->c = m->r1 * 20; x->d = m->sy * 20; x->tx = m->tx; x->ty = m->ty; }
/* device positions snap to the rasterizer's sub-pixel grid (8 bits, as GPUs do) */
static void xf_pt(const XF *x, double px, double py, float *ox, float *oy) {
    if (g_rmat_compose) {
        /* the vertex shader's f32 world transform */
        volatile float fx = (float)px, fy = (float)py, vx, vy;
        vx = (float)x->a * fx; vx = vx + (float)x->c * fy; vx = vx + (float)x->tx;
        vy = (float)x->b * fx; vy = vy + (float)x->d * fy; vy = vy + (float)x->ty;
        *ox = (float)(floor((double)vx * 256.0 + 0.5) / 256.0);
        *oy = (float)(floor((double)vy * 256.0 + 0.5) / 256.0);
        return;
    }
    *ox = (float)(floor((x->a * px + x->c * py + x->tx) * 256.0 + 0.5) / 256.0);
    *oy = (float)(floor((x->b * px + x->d * py + x->ty) * 256.0 + 0.5) / 256.0);
}

static void add_poly_closed(EdgeList *l, const XF *x, const DPt *p, int n) {
    int i; float ax, ay, bx, by;
    if (n < 2) return;
    for (i = 0; i < n; i++) {
        const DPt *q = &p[i], *r = &p[(i + 1) % n];
        xf_pt(x, q->x, q->y, &ax, &ay); xf_pt(x, r->x, r->y, &bx, &by);
        edgelist_add(l, ax, ay, bx, by);
    }
}

/* ---------------------------------------------------------------- */
/* paint setup                                                      */

static float srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : (float)pow((c + 0.055f) / 1.055f, 2.4); }
static u8 sat_u8(float v) { return v <= 0 ? 0 : v >= 255 ? 255 : (u8)v; }   /* Rust `as u8` (truncating, saturating) */

/* CommonGradient::new: the 256-texel gradient texture */
static void build_lut(Paint *p, const Gradient *g) {
    int t, last = 0, next, n = g->n;
    p->linear_rgb = g->interp == 1;
    if (n == 0) { memset(p->lut, 0, sizeof(p->lut)); return; }
    for (t = 0; t < 256; t++) {
        float a; int k;
        if (last + 1 < n && t > g->ratio[last + 1]) last++;
        next = last + 1 < n ? last + 1 : n - 1;
        if (t <= g->ratio[last] || g->ratio[last] == g->ratio[next]) a = 0;
        else if (t > g->ratio[next]) a = 1;
        else a = (float)(t - g->ratio[last]) / (float)(g->ratio[next] - g->ratio[last]);
        for (k = 0; k < 4; k++) {
            const SWFColor *c0 = &g->color[last], *c1 = &g->color[next];
            float v0 = k == 0 ? c0->r : k == 1 ? c0->g : k == 2 ? c0->b : c0->a;
            float v1 = k == 0 ? c1->r : k == 1 ? c1->g : k == 2 ? c1->b : c1->a;
            if (p->linear_rgb && k < 3) { v0 = srgb_to_linear(v0 / 255.0f) * 255.0f; v1 = srgb_to_linear(v1 / 255.0f) * 255.0f; }
            p->lut[t][k] = sat_u8(v0 + (v1 - v0) * a);
        }
    }
}

static const Bitmap *g_direct_bitmap;
static const Bitmap *(*g_bd_lookup)(int id);
void render_set_bitmapdata_lookup(const Bitmap *(*fn)(int id)) { g_bd_lookup = fn; }
void render_set_direct_bitmap(const Bitmap *bm) { g_direct_bitmap = bm; }

/* returns 0: nothing to draw (singular matrix, missing bitmap) */
static int setup_paint(Paint *p, SWFMovie *mv, const FillStyle *f,
                       const SWFMatrix *m, const SWFCxform *cx) {
    SWFMatrix full;
    memset(p, 0, sizeof(*p));
    p->cx = *cx;
    switch (f->type) {
    case FILL_SOLID:
        p->kind = PAINT_SOLID; p->color = f->color; return 1;
    case FILL_LINEAR: case FILL_RADIAL: case FILL_FOCAL:
        p->kind = (f->type == FILL_LINEAR) ? PAINT_LINEAR : (f->type == FILL_RADIAL ? PAINT_RADIAL : PAINT_FOCAL);
        p->spread = f->grad.spread == 3 ? 0 : f->grad.spread;
        p->focal = f->grad.focal < -0.98 ? -0.98 : f->grad.focal > 0.98 ? 0.98 : f->grad.focal;   /* Ruffle clamps the focal point */
        build_lut(p, &f->grad);
        swf_matrix_mul(&full, m, &f->matrix);
        if (!matrix_invert(&full, &p->ia, &p->ib, &p->ic, &p->id, &p->ie, &p->iff)) return 0;
        return 1;
    default: {
        const Bitmap *bm = (f->bitmap_id == -1 && g_direct_bitmap) ? g_direct_bitmap : (f->bitmap_id <= -2 ? NULL : image_get(mv, f->bitmap_id));
        if (f->bitmap_id <= BMD_FILL_ID0) bm = bmd_fill_lookup(f->bitmap_id);
        else if (f->bitmap_id <= -2 && g_bd_lookup) bm = g_bd_lookup(f->bitmap_id);
        if (!bm || bm->w <= 0 || bm->h <= 0 || !bm->px) return 0;   /* Ruffle skips a missing bitmap */
        p->kind = PAINT_BITMAP;
        p->bmp = bm;
        p->bmp_repeat = (f->type == FILL_BITMAP_REPEAT || f->type == FILL_BITMAP_REPEAT_NS);
        p->bmp_smooth = (f->type == FILL_BITMAP_REPEAT || f->type == FILL_BITMAP_CLIP);
        swf_matrix_mul(&full, m, &f->matrix);
        if (!matrix_invert(&full, &p->ia, &p->ib, &p->ic, &p->id, &p->ie, &p->iff)) return 0;
        return 1;
    }
    }
}

/* ---------------------------------------------------------------- */
/* main entry                                                       */

/* the tessellation scale of a timeline shape (Ruffle: Graphic::render_self + TessellationCache) */
static double tess_scale(ShapeDef *sh, const SWFMatrix *m) {
    double sxx = fabs((m->sx + m->r1) * 20.0), syy = fabs((m->r0 + m->sy) * 20.0);
    float cur = (float)sqrt((sxx * sxx + syy * syy) / 2.0);
    int i, best = -1; float bestdev = 1e30f;
    if (!sh->scaled_tess) return 1.0;
    for (i = 0; i < sh->ntess; i++) {
        float ratio = (float)fabs(cur / sh->tess[i]);
        if (ratio <= 2.0f && ratio >= 0.5f) { float dev = (float)fabs(ratio - 1.0f); if (dev < bestdev) { bestdev = dev; best = i; } }
    }
    if (best >= 0) {
        float v = sh->tess[best];
        for (i = best + 1; i < sh->ntess; i++) sh->tess[i - 1] = sh->tess[i];
        sh->tess[sh->ntess - 1] = v;
        return v;
    }
    if (sh->ntess < 4) sh->tess[sh->ntess++] = cur;
    else { for (i = 1; i < 4; i++) sh->tess[i - 1] = sh->tess[i]; sh->tess[3] = cur; }
    return cur;
}

/* lyon's fill tessellator splits crossing edges at their intersection (f64,
   then rounded to f32); that vertex is snapped like the others, so the edges
   on either side of it are not quite the original line */
typedef struct { float x0, y0, x1, y1, xmin, xmax, ymin, ymax; int ring, idx; } XEdge;
typedef struct { int edge; double t; float x, y; } XIns;
static int cmp_xedge(const void *a, const void *b) { float d = ((const XEdge*)a)->xmin - ((const XEdge*)b)->xmin; return d < 0 ? -1 : d > 0; }
static int cmp_xins(const void *a, const void *b) { const XIns *p = (const XIns*)a, *q = (const XIns*)b; if (p->edge != q->edge) return p->edge - q->edge; return p->t < q->t ? -1 : p->t > q->t; }
static int lyon_after(float ax, float ay, float bx, float by) { return ay > by || (ay == by && ax > bx); }
static void split_crossings(Poly *rings, int nr) {
    XEdge *e; XIns *ins = NULL; int ne = 0, i, j, k, nins = 0, capins = 0, *base;
    for (i = 0; i < nr; i++) ne += rings[i].n;
    if (ne < 4 || ne > 20000) return;
    e = (XEdge*)malloc((size_t)ne * sizeof(XEdge)); base = (int*)malloc((size_t)(nr + 1) * sizeof(int));
    if (!e || !base) { free(e); free(base); return; }
    for (i = 0, k = 0; i < nr; i++) {
        base[i] = k;
        for (j = 0; j < rings[i].n; j++) {
            const DPt *a = &rings[i].p[j], *b = &rings[i].p[(j + 1) % rings[i].n]; XEdge *x = &e[k++];
            x->x0 = (float)a->x; x->y0 = (float)a->y; x->x1 = (float)b->x; x->y1 = (float)b->y; x->ring = i; x->idx = k - 1;
            x->xmin = x->x0 < x->x1 ? x->x0 : x->x1; x->xmax = x->x0 < x->x1 ? x->x1 : x->x0; x->ymin = x->y0 < x->y1 ? x->y0 : x->y1; x->ymax = x->y0 < x->y1 ? x->y1 : x->y0;
        }
    }
    base[nr] = k;
    qsort(e, (size_t)ne, sizeof(XEdge), cmp_xedge);
    for (i = 0; i < ne; i++) for (j = i + 1; j < ne && e[j].xmin <= e[i].xmax; j++) {
        const XEdge *A = &e[i], *B = &e[j]; double afx, afy, atx, aty, bfx, bfy, btx, bty, v1x, v1y, v2x, v2y, c, sg, ac, v3x, v3y, t, u; float px, py;
        if (B->ymin > A->ymax || A->ymin > B->ymax) continue;
        if (A->x0 == A->x1 && A->y0 == A->y1) continue;
        if (B->x0 == B->x1 && B->y0 == B->y1) continue;
        /* both downward; the segment starting later in the sweep is the one "below" */
        if (lyon_after(A->x0, A->y0, A->x1, A->y1)) { afx = A->x1; afy = A->y1; atx = A->x0; aty = A->y0; } else { afx = A->x0; afy = A->y0; atx = A->x1; aty = A->y1; }
        if (lyon_after(B->x0, B->y0, B->x1, B->y1)) { bfx = B->x1; bfy = B->y1; btx = B->x0; bty = B->y0; } else { bfx = B->x0; bfy = B->y0; btx = B->x1; bty = B->y1; }
        if (lyon_after((float)afx, (float)afy, (float)bfx, (float)bfy)) { double q; q = afx; afx = bfx; bfx = q; q = afy; afy = bfy; bfy = q; q = atx; atx = btx; btx = q; q = aty; aty = bty; bty = q; }
        if ((atx == btx && aty == bty) || (afx == bfx && afy == bfy) || (afx == btx && afy == bty) || (atx == bfx && aty == bfy)) continue;
        v1x = atx - afx; v1y = aty - afy; v2x = btx - bfx; v2y = bty - bfy;
        c = v1x * v2y - v1y * v2x;
        if (c == 0) continue;
        sg = c > 0 ? 1.0 : -1.0; ac = fabs(c);
        v3x = bfx - afx; v3y = bfy - afy;
        t = (v3x * v2y - v3y * v2x) * sg; u = (v3x * v1y - v3y * v1x) * sg;
        if (t < 0 || t > ac || u < 0 || u > ac) continue;
        t /= ac; u /= ac;
        if (!(u > 0 && u < 1 && t > 0 && t <= 1)) continue;
        px = (float)(bfx + (btx - bfx) * u); py = (float)(bfy + (bty - bfy) * u);
        for (k = 0; k < 2; k++) {
            const XEdge *E = k ? B : A; double dx = E->x1 - E->x0, dy = E->y1 - E->y0, tt = ((px - E->x0) * dx + (py - E->y0) * dy) / (dx * dx + dy * dy);
            if (!(tt > 0 && tt < 1)) continue;
            if ((px == E->x0 && py == E->y0) || (px == E->x1 && py == E->y1)) continue;
            if (nins == capins) { capins = capins ? capins * 2 : 64; ins = (XIns*)realloc(ins, (size_t)capins * sizeof(XIns)); if (!ins) { free(e); free(base); return; } }
            ins[nins].edge = E->idx; ins[nins].t = tt; ins[nins].x = px; ins[nins].y = py; nins++;
        }
    }
    if (nins) {
        int q = 0;
        qsort(ins, (size_t)nins, sizeof(XIns), cmp_xins);
        for (i = 0; i < nr; i++) {
            Poly np; memset(&np, 0, sizeof(np));
            for (j = 0; j < rings[i].n; j++) {
                int ei = base[i] + j;
                poly_push(&np, rings[i].p[j].x, rings[i].p[j].y);
                while (q < nins && ins[q].edge < ei) q++;
                while (q < nins && ins[q].edge == ei) { poly_push(&np, ins[q].x, ins[q].y); q++; }
            }
            free(rings[i].p); rings[i] = np;
        }
    }
    free(ins); free(e); free(base);
}

void render_shape(Surface *s, SWFMovie *mv, ShapeDef *sh,
                  const SWFMatrix *m, const SWFCxform *cx, const SWFRect *clip) {
    DPathList local, *pl;
    EdgeList el; Poly poly; XF x; int i, k, as_mask;
    double scale, tol;

    if (!sh || sh->nedges == 0) return;
    if (sh->cache_paths && sh->paths) pl = (DPathList*)sh->paths;
    else {
        memset(&local, 0, sizeof(local));
        shape_to_paths(sh, &local);
        if (sh->cache_paths && (pl = (DPathList*)malloc(sizeof(DPathList))) != NULL) { *pl = local; sh->paths = pl; }
        else pl = &local;
    }
    scale = tess_scale(sh, m);
    if (!(scale > 1e-7)) scale = 1.0;
    tol = 0.1 / scale;   /* lyon's default tolerance at the tessellation scale */
    as_mask = s->mstate == MS_DRAW_STENCIL || s->mstate == MS_CLEAR_STENCIL;
    xf_of(m, &x);
    edgelist_init(&el); memset(&poly, 0, sizeof(poly));
    for (i = 0; i < pl->n; i++) {
        const DPath *d = &pl->p[i]; Paint p; const FillStyle *fs;
        edgelist_clear(&el);
        if (d->fill) {
            if (d->style < 1 || d->style > sh->nfills) continue;
            fs = &sh->fills[d->style - 1];
            {
                Poly *rings = (Poly*)calloc((size_t)d->nsegs + 1, sizeof(Poly));
                if (rings) {
                    for (k = 0; k < d->nsegs; k++) {
                        g_flat_fill = 1; flatten_seg(&d->segs[k], &rings[k], tol); g_flat_fill = 0;
                    }
                    split_crossings(rings, d->nsegs);
                    for (k = 0; k < d->nsegs; k++) { add_poly_closed(&el, &x, rings[k].p, rings[k].n); free(rings[k].p); }
                    free(rings);
                }
            }
            if (setup_paint(&p, mv, fs, m, cx)) raster_fill(s, &el, fs->winding ? (fs->winding == 2 ? 0 : 1) : (sh->nonzero ? 0 : 1), &p, clip);
        } else {
            const LineStyle *ls; StrokeStyle st; float w, fscale = (float)scale; StrokeMesh mesh; StrokeEv *ev = NULL; int nev = 0, cap = 0, t;
            if (d->style < 1 || d->style > sh->nlines) continue;
            ls = &sh->lines[d->style - 1];
            /* solid strokes are left out of masks (Ruffle's mask_index_count) */
            if (as_mask && !ls->has_fill) continue;
            /* Ruffle: lyon's stroke tessellator, hairlines at least 1 px on screen */
            w = (float)(ls->width / 20.0);
            if (w < 1.0f / fscale) w = 1.0f / fscale;
            st.width = w; st.tol = 0.1f / fscale; st.miter_limit = 4.0f;
            st.start_cap = ls->start_cap == 1 ? LC_BUTT : ls->start_cap == 2 ? LC_SQUARE : LC_ROUND;
            st.end_cap = ls->end_cap == 1 ? LC_BUTT : ls->end_cap == 2 ? LC_SQUARE : LC_ROUND;
            if (ls->join == 0) st.join = LJ_ROUND;
            else if (ls->join == 2 && (float)ls->miter >= 1.0f) { st.join = LJ_MITERCLIP; st.miter_limit = (float)ls->miter; }
            else st.join = LJ_BEVEL;   /* bevel, or a miter limit below lyon's minimum */
            for (k = 0; k < d->nsegs; k++) {
                const Seg *sg = &d->segs[k]; int j;
                if (sg->n < 2) continue;
                if (nev + sg->n + 2 > cap) { int nc = (nev + sg->n + 2) * 2; StrokeEv *ne = (StrokeEv*)realloc(ev, (size_t)nc * sizeof(StrokeEv)); if (!ne) break; ev = ne; cap = nc; }
                ev[nev].kind = SE_BEGIN; ev[nev].x = (float)(sg->p[0].x / 20.0); ev[nev].y = (float)(sg->p[0].y / 20.0); nev++;
                for (j = 1; j < sg->n; j++) {
                    if (sg->p[j].ctl == 2 && j + 2 < sg->n) {
                        ev[nev].kind = SE_CUBIC; ev[nev].cx = (float)(sg->p[j].x / 20.0); ev[nev].cy = (float)(sg->p[j].y / 20.0);
                        ev[nev].c2x = (float)(sg->p[j + 1].x / 20.0); ev[nev].c2y = (float)(sg->p[j + 1].y / 20.0);
                        ev[nev].x = (float)(sg->p[j + 2].x / 20.0); ev[nev].y = (float)(sg->p[j + 2].y / 20.0); nev++; j += 2;
                    } else if (sg->p[j].ctl && j + 1 < sg->n) {
                        ev[nev].kind = SE_QUAD; ev[nev].cx = (float)(sg->p[j].x / 20.0); ev[nev].cy = (float)(sg->p[j].y / 20.0);
                        ev[nev].x = (float)(sg->p[j + 1].x / 20.0); ev[nev].y = (float)(sg->p[j + 1].y / 20.0); nev++; j++;
                    } else { ev[nev].kind = SE_LINE; ev[nev].x = (float)(sg->p[j].x / 20.0); ev[nev].y = (float)(sg->p[j].y / 20.0); nev++; }
                }
                ev[nev].kind = (k == d->nsegs - 1 && d->closed) ? SE_CLOSE : SE_END; nev++;
            }
            memset(&mesh, 0, sizeof(mesh));
            lyon_stroke(&mesh, ev, nev, &st);
            free(ev);
            for (t = 0; t < mesh.nt; t++) {
                float ax, ay, bx, by, qx, qy; const int *tri = mesh.t + t * 3; double area;
                xf_pt(&x, mesh.v[tri[0] * 2], mesh.v[tri[0] * 2 + 1], &ax, &ay);
                xf_pt(&x, mesh.v[tri[1] * 2], mesh.v[tri[1] * 2 + 1], &bx, &by);
                xf_pt(&x, mesh.v[tri[2] * 2], mesh.v[tri[2] * 2 + 1], &qx, &qy);
                area = ((double)bx - ax) * ((double)qy - ay) - ((double)by - ay) * ((double)qx - ax);
                if (area == 0) continue;
                if (area > 0) { edgelist_add(&el, ax, ay, bx, by); edgelist_add(&el, bx, by, qx, qy); edgelist_add(&el, qx, qy, ax, ay); }
                else { edgelist_add(&el, ax, ay, qx, qy); edgelist_add(&el, qx, qy, bx, by); edgelist_add(&el, bx, by, ax, ay); }
            }
            stroke_mesh_free(&mesh);
            if (ls->has_fill) { if (!setup_paint(&p, mv, &ls->fill, m, cx)) continue; }
            else { memset(&p, 0, sizeof(p)); p.kind = PAINT_SOLID; p.color = ls->color; p.cx = *cx; }
            raster_fill(s, &el, 2, &p, clip);
        }
    }
    edgelist_free(&el); free(poly.p);
    if (pl == &local) dpl_free(&local);
}

/* ---------------------------------------------------------------- */
/* Ruffle matrices and rectangle / line primitives                  */

float f32_round_ties_even(float v) {
    double f = floor(v), d = v - f;
    if (d > 0.5) f += 1; else if (d == 0.5 && fmod(f, 2.0) != 0) f += 1;
    return (float)f;
}
static s32 round_to_i32(float f) {
    if (!(f == f) || f > 3.0e38f || f < -3.0e38f) return 0;
    if (f >= 2147483648.0f) return (s32)0x80000000;
    if (f < -2147483648.0f) return (s32)0x80000000;
    return (s32)f32_round_ties_even(f);
}
s32 twips_round_ties_even_px(s32 t) { return (s32)(f32_round_ties_even((float)(t / 20.0)) * 20.0); }
void rmat_from(RMatrix *o, const SWFMatrix *wm) {
    o->a = (float)(wm->sx * 20.0); o->b = (float)(wm->r0 * 20.0); o->c = (float)(wm->r1 * 20.0); o->d = (float)(wm->sy * 20.0);
    o->tx = (s32)floor(wm->tx * 20.0 + 0.5); o->ty = (s32)floor(wm->ty * 20.0 + 0.5);
}
void rmat_mul(RMatrix *o, const RMatrix *l, const RMatrix *r) {
    RMatrix t; float rtx = (float)r->tx, rty = (float)r->ty;
    t.tx = round_to_i32(l->a * rtx + l->c * rty) + l->tx;
    t.ty = round_to_i32(l->b * rtx + l->d * rty) + l->ty;
    t.a = l->a * r->a + l->c * r->b; t.b = l->b * r->a + l->d * r->b;
    t.c = l->a * r->c + l->c * r->d; t.d = l->b * r->c + l->d * r->d;
    *o = t;
}
/* a world matrix (twips -> device px) times a local one (twips -> twips) as
   Ruffle's transform stack multiplies them: f32 scales, translation rounded to
   whole twips */
int g_rmat_compose = 1;
void render_matrix_compose(SWFMatrix *out, const SWFMatrix *wm, const SWFMatrix *local) {
    RMatrix l, r, t;
    if (!g_rmat_compose) { swf_matrix_mul(out, wm, local); return; }
    rmat_from(&l, wm);
    r.a = (float)local->sx; r.b = (float)local->r0; r.c = (float)local->r1; r.d = (float)local->sy;
    r.tx = (s32)floor(local->tx + 0.5); r.ty = (s32)floor(local->ty + 0.5);
    rmat_mul(&t, &l, &r);
    out->sx = t.a / 20.0; out->r0 = t.b / 20.0; out->r1 = t.c / 20.0; out->sy = t.d / 20.0;
    out->tx = t.tx / 20.0; out->ty = t.ty / 20.0;
}
void rmat_box(RMatrix *o, float sx, float sy, s32 tx, s32 ty) { o->a = sx; o->b = 0; o->c = 0; o->d = sy; o->tx = tx; o->ty = ty; }
void rmat_box_rot(RMatrix *o, float sx, float sy, float rot, s32 tx, s32 ty) {
    o->a = (float)cos(rot) * sx; o->c = -(float)sin(rot) * sx; o->b = (float)sin(rot) * sy; o->d = (float)cos(rot) * sy; o->tx = tx; o->ty = ty;
}
void rmat_point(const RMatrix *m, s32 x, s32 y, s32 *ox, s32 *oy) {
    *ox = round_to_i32(m->a * (float)x + m->c * (float)y) + m->tx;
    *oy = round_to_i32(m->b * (float)x + m->d * (float)y) + m->ty;
}
/* the unit square under m (the renderer's quad: px coordinates, translation in px) */
void render_draw_rect(Surface *s, SWFColor c, const RMatrix *m) {
    EdgeList el; Paint p; float xs[4], ys[4]; int i; static const float u[4] = { 0, 1, 1, 0 }, v[4] = { 0, 0, 1, 1 };
    double tx = m->tx / 20.0, ty = m->ty / 20.0;
    for (i = 0; i < 4; i++) {
        double x = m->a * u[i] + m->c * v[i] + tx, y = m->b * u[i] + m->d * v[i] + ty;
        xs[i] = (float)(floor(x * 256.0 + 0.5) / 256.0); ys[i] = (float)(floor(y * 256.0 + 0.5) / 256.0);
    }
    edgelist_init(&el);
    for (i = 0; i < 4; i++) edgelist_add(&el, xs[i], ys[i], xs[(i + 1) & 3], ys[(i + 1) & 3]);
    memset(&p, 0, sizeof(p)); p.kind = PAINT_SOLID; p.color = c; swf_cxform_identity(&p.cx);
    raster_fill(s, &el, 0, &p, NULL);
    edgelist_free(&el);
}
/* emulate_line_as_rect: a 1 px wide rectangle from a to b (device twips), half a pixel in */
static void line_as_rect(Surface *s, SWFColor c, s32 ax, s32 ay, s32 bx, s32 by) {
    double dx = (bx - ax) / 20.0, dy = (by - ay) / 20.0;
    float len = (float)sqrt(dx * dx + dy * dy), ang = (float)atan2(dy, dx);
    RMatrix rot, tr, box, t1, t2;
    rot.a = (float)cos(ang); rot.b = (float)sin(ang); rot.c = -(float)sin(ang); rot.d = (float)cos(ang); rot.tx = rot.ty = 0;
    rmat_box(&tr, 1, 1, ax + 10, ay + 10);
    rmat_box(&box, len, 1, 0, -10);
    rmat_mul(&t1, &rot, &box); rmat_mul(&t2, &tr, &t1);
    render_draw_rect(s, c, &t2);
}
void render_draw_line(Surface *s, SWFColor c, const RMatrix *m) {
    s32 ax, ay, bx, by;
    rmat_point(m, 0, 0, &ax, &ay); rmat_point(m, 20, 0, &bx, &by);
    line_as_rect(s, c, ax, ay, bx, by);
}
void render_draw_line_rect(Surface *s, SWFColor c, const RMatrix *m) {
    s32 ax, ay, bx, by, cx, cy, dx, dy;
    rmat_point(m, 0, 0, &ax, &ay); rmat_point(m, 20, 0, &bx, &by); rmat_point(m, 20, 20, &cx, &cy); rmat_point(m, 0, 20, &dx, &dy);
    line_as_rect(s, c, ax, ay, bx, by); line_as_rect(s, c, bx, by, cx, cy);
    line_as_rect(s, c, cx, cy, dx, dy); line_as_rect(s, c, dx, dy, ax, ay);
}
