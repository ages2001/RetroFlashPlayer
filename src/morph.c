/* morph.c - DefineMorphShape / DefineMorphShape2: interpolate at ratio */
#include "chars.h"
#include "shape.h"
#include "render.h"
#include "reader.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    ShapeDef start, end;           /* edges + styles (start styles) */
    FillStyle *efills; int nefills;
    LineStyle *elines; int nelines;
} MorphDef;

static void read_morph_gradient(Reader *r, Gradient *a, Gradient *b) {
    int n = r_u8(r), i;
    memset(a, 0, sizeof(*a)); memset(b, 0, sizeof(*b));
    if (n > MAX_GRAD) n = MAX_GRAD;
    a->n = b->n = n;
    for (i = 0; i < n; i++) {
        a->ratio[i] = r_u8(r); r_rgba(r, &a->color[i]);
        b->ratio[i] = r_u8(r); r_rgba(r, &b->color[i]);
    }
}

static void read_morph_fill(Reader *r, FillStyle *a, FillStyle *b) {
    memset(a, 0, sizeof(*a)); memset(b, 0, sizeof(*b));
    swf_matrix_identity(&a->matrix); swf_matrix_identity(&b->matrix);
    a->type = b->type = r_u8(r);
    switch (a->type) {
    case FILL_SOLID: r_rgba(r, &a->color); r_rgba(r, &b->color); break;
    case FILL_LINEAR: case FILL_RADIAL: case FILL_FOCAL:
        r_matrix(r, &a->matrix); r_matrix(r, &b->matrix);
        read_morph_gradient(r, &a->grad, &b->grad);
        if (a->type == FILL_FOCAL) { a->grad.focal = r_fixed8(r); b->grad.focal = r_fixed8(r); }
        break;
    default:
        a->bitmap_id = b->bitmap_id = r_u16(r);
        r_matrix(r, &a->matrix); r_matrix(r, &b->matrix);
        break;
    }
}

static MorphDef *morph_get(SWFMovie *m, SWFChar *ch) {
    MorphDef *d; Reader r; int v2 = ch->tagcode == TAG_DefineMorphShape2, n, i;
    u32 offset; size_t end_pos;
    if (ch->parsed) return (MorphDef*)ch->parsed;
    d = (MorphDef*)calloc(1, sizeof(MorphDef));
    if (!d) return NULL;
    reader_init(&r, ch->data, ch->len);
    r_u16(&r);
    r_rect(&r, &d->start.bounds); r_rect(&r, &d->end.bounds);
    if (v2) { SWFRect t; r_rect(&r, &t); r_rect(&r, &t); r_u8(&r); }
    offset = r_u32(&r);
    end_pos = r.pos + offset;
    /* fill styles */
    n = r_u8(&r); if (n == 0xff) n = r_u16(&r);
    d->start.fills = (FillStyle*)calloc((size_t)n + 1, sizeof(FillStyle)); d->efills = (FillStyle*)calloc((size_t)n + 1, sizeof(FillStyle));
    d->start.nfills = d->start.capfills = d->nefills = n;
    for (i = 0; i < n; i++) read_morph_fill(&r, &d->start.fills[i], &d->efills[i]);
    /* line styles */
    n = r_u8(&r); if (n == 0xff) n = r_u16(&r);
    d->start.lines = (LineStyle*)calloc((size_t)n + 1, sizeof(LineStyle)); d->elines = (LineStyle*)calloc((size_t)n + 1, sizeof(LineStyle));
    d->start.nlines = d->start.caplines = d->nelines = n;
    for (i = 0; i < n; i++) {
        LineStyle *a = &d->start.lines[i], *b = &d->elines[i];
        a->width = r_u16(&r); b->width = r_u16(&r);
        if (v2) {
            int has_fill, join;
            reader_align(&r);
            a->start_cap = b->start_cap = (int)rb_u(&r, 2); join = (int)rb_u(&r, 2); a->join = b->join = join; has_fill = (int)rb_u(&r, 1); a->no_hscale = b->no_hscale = (int)rb_u(&r, 1); a->no_vscale = b->no_vscale = (int)rb_u(&r, 1); rb_u(&r, 1); rb_u(&r, 5); a->no_close = b->no_close = (int)rb_u(&r, 1); a->end_cap = b->end_cap = (int)rb_u(&r, 2);
            reader_align(&r);
            if (join == 2) a->miter = b->miter = r_fixed8(&r);
            if (has_fill) { read_morph_fill(&r, &a->fill, &b->fill); a->has_fill = b->has_fill = 1; a->color = a->fill.color; b->color = b->fill.color; }
            else { r_rgba(&r, &a->color); r_rgba(&r, &b->color); }
        } else { r_rgba(&r, &a->color); r_rgba(&r, &b->color); }
    }
    d->start.version = 3;
    shape_parse_records(&r, &d->start, 3, 0);
    r.pos = end_pos; r.bit = 0;
    d->end.version = 3;
    shape_parse_records(&r, &d->end, 3, 0);
    ch->parsed = d;
    return d;
}

/* Ruffle's morph interpolation: a = 1 - b, b = ratio / 65535 in f32 */
static u8 lerp8(u8 s, u8 e, float a, float b) { float v = a * (float)s + b * (float)e; return v <= 0 ? 0 : v >= 255 ? 255 : (u8)v; }   /* truncated, saturating */
static s32 lerp_tw(s32 s, s32 e, float a, float b) { float v = (float)s * a + (float)e * b; return (s32)(v < 0 ? -floor(-v + 0.5) : floor(v + 0.5)); }
/* Fixed16 * Fixed16 (i64 product >> 16), summed */
static double lerp_fx16(double s, double e, s32 ia, s32 ib) {
    double fs = floor(s * 65536.0 + 0.5), fe = floor(e * 65536.0 + 0.5);
    return (floor(fs * (double)ia / 65536.0) + floor(fe * (double)ib / 65536.0)) / 65536.0;
}
static void lerp_matrix(SWFMatrix *o, const SWFMatrix *s, const SWFMatrix *e, float a, float b) {
    s32 ia = (s32)(a * 65536.0f), ib = (s32)(b * 65536.0f);
    o->sx = lerp_fx16(s->sx, e->sx, ia, ib); o->sy = lerp_fx16(s->sy, e->sy, ia, ib);
    o->r0 = lerp_fx16(s->r0, e->r0, ia, ib); o->r1 = lerp_fx16(s->r1, e->r1, ia, ib);
    o->tx = lerp_tw((s32)floor(s->tx + 0.5), (s32)floor(e->tx + 0.5), a, b); o->ty = lerp_tw((s32)floor(s->ty + 0.5), (s32)floor(e->ty + 0.5), a, b);
}
static void lerp_color(SWFColor *o, const SWFColor *s, const SWFColor *e, float a, float b) {
    o->r = lerp8(s->r, e->r, a, b); o->g = lerp8(s->g, e->g, a, b); o->b = lerp8(s->b, e->b, a, b); o->a = lerp8(s->a, e->a, a, b);
}

/* the shape at a ratio (0..65535); release with morph_shape_release */
int morph_shape_at(SWFMovie *mv, SWFChar *ch, int ratio, ShapeDef *out) {
    MorphDef *m = morph_get(mv, ch);
    ShapeDef s; float fb, fa; int i, j;
    if (!m || m->start.nedges == 0) return 0;
    fb = (float)ratio / 65535.0f; fa = 1.0f - fb;
    memset(&s, 0, sizeof(s));
    s.version = 3; s.bounds = m->start.bounds; s.has_brk = 1;
    s.nfills = m->start.nfills; s.fills = (FillStyle*)malloc((size_t)(s.nfills + 1) * sizeof(FillStyle));
    s.nlines = m->start.nlines; s.lines = (LineStyle*)malloc((size_t)(s.nlines + 1) * sizeof(LineStyle));
    s.nedges = m->start.nedges; s.edges = (ShapeEdge*)malloc((size_t)(s.nedges + 1) * sizeof(ShapeEdge));
    if (!s.fills || !s.lines || !s.edges) { free(s.fills); free(s.lines); free(s.edges); return 0; }
    for (i = 0; i < s.nfills; i++) {
        FillStyle *a = &m->start.fills[i], *b = &m->efills[i], *o = &s.fills[i];
        *o = *a;
        lerp_color(&o->color, &a->color, &b->color, fa, fb);
        lerp_matrix(&o->matrix, &a->matrix, &b->matrix, fa, fb);
        for (j = 0; j < a->grad.n; j++) { o->grad.ratio[j] = lerp8(a->grad.ratio[j], b->grad.ratio[j], fa, fb); lerp_color(&o->grad.color[j], &a->grad.color[j], &b->grad.color[j], fa, fb); }
    }
    for (i = 0; i < s.nlines; i++) {
        LineStyle *a = &m->start.lines[i], *b = &m->elines[i], *o = &s.lines[i];
        *o = *a;
        o->width = lerp_tw(a->width, b->width, fa, fb);
        lerp_color(&o->color, &a->color, &b->color, fa, fb);
        if (a->has_fill) {
            lerp_color(&o->fill.color, &a->fill.color, &b->fill.color, fa, fb);
            lerp_matrix(&o->fill.matrix, &a->fill.matrix, &b->fill.matrix, fa, fb);
            for (j = 0; j < a->fill.grad.n; j++) { o->fill.grad.ratio[j] = lerp8(a->fill.grad.ratio[j], b->fill.grad.ratio[j], fa, fb); lerp_color(&o->fill.grad.color[j], &a->fill.grad.color[j], &b->fill.grad.color[j], fa, fb); }
        }
    }
    /* edges: the end shape has the same edge count; style indices come from start */
    {
        int ne = m->end.nedges;
        s32 cx0 = 0, cy0 = 0;
        for (i = 0; i < s.nedges; i++) {
            ShapeEdge *a = &m->start.edges[i], *o = &s.edges[i];
            const ShapeEdge *b = (i < ne) ? &m->end.edges[i] : a;
            *o = *a;
            o->x0 = lerp_tw(a->x0, b->x0, fa, fb); o->y0 = lerp_tw(a->y0, b->y0, fa, fb);
            o->x1 = lerp_tw(a->x1, b->x1, fa, fb); o->y1 = lerp_tw(a->y1, b->y1, fa, fb);
            if (a->is_curve || b->is_curve) {
                /* a straight edge against a curve: its control point is the pen plus half the delta (truncated) */
                s32 acx = a->is_curve ? a->cx : a->x0 + (a->x1 - a->x0) / 2, acy = a->is_curve ? a->cy : a->y0 + (a->y1 - a->y0) / 2;
                s32 bcx = b->is_curve ? b->cx : b->x0 + (b->x1 - b->x0) / 2, bcy = b->is_curve ? b->cy : b->y0 + (b->y1 - b->y0) / 2;
                o->is_curve = 1;
                o->cx = lerp_tw(acx, bcx, fa, fb); o->cy = lerp_tw(acy, bcy, fa, fb);
            }
            (void)cx0; (void)cy0;
        }
    }
    *out = s;
    return 1;
}
void morph_shape_release(ShapeDef *s) { free(s->fills); free(s->lines); free(s->edges); }

void morph_render(Player *p, DisplayObject *d, const SWFMatrix *wm, const SWFCxform *cx) {
    ShapeDef s;
    if (!morph_shape_at(PMOV(p), d->ch, d->ratio, &s)) return;
    render_shape(p->surf, PMOV(p), &s, wm, cx, NULL);
    morph_shape_release(&s);
}

void morph_free_parsed(void *p) {
    MorphDef *m = (MorphDef*)p;
    if (!m) return;
    free(m->start.fills); free(m->start.lines); free(m->start.edges);
    free(m->end.fills); free(m->end.lines); free(m->end.edges);
    free(m->efills); free(m->elines);
    free(m);
}
