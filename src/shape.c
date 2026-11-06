#include "shape.h"
#include "reader.h"
#include <stdlib.h>
#include <string.h>

static void *grow(void *p, int *cap, int need, size_t elem) {
    int nc;
    void *np;
    if (need <= *cap) return p;
    nc = *cap ? *cap : 16;
    while (nc < need) nc *= 2;
    np = realloc(p, (size_t)nc * elem);
    if (!np) return p;
    *cap = nc;
    return np;
}

static void read_color(Reader *r, SWFColor *c, int version) {
    if (version >= 3) r_rgba(r, c); else r_rgb(r, c);
}

static void read_gradient(Reader *r, Gradient *g, int version, int focal) {
    int i;
    reader_align(r);
    g->spread = (int)rb_u(r, 2);
    g->interp = (int)rb_u(r, 2);
    g->n      = (int)rb_u(r, 4);
    if (g->n > MAX_GRAD) g->n = MAX_GRAD;
    for (i = 0; i < g->n; i++) {
        g->ratio[i] = r_u8(r);
        read_color(r, &g->color[i], version);
    }
    g->focal = focal ? r_fixed8(r) : 0.0;
}

static void read_fillstyle(Reader *r, FillStyle *f, int version) {
    memset(f, 0, sizeof(*f));
    swf_matrix_identity(&f->matrix);
    f->type = r_u8(r);
    f->color.a = 255;
    switch (f->type) {
    case FILL_SOLID:
        read_color(r, &f->color, version);
        break;
    case FILL_LINEAR: case FILL_RADIAL: case FILL_FOCAL:
        r_matrix(r, &f->matrix);
        read_gradient(r, &f->grad, version, f->type == FILL_FOCAL);
        break;
    case FILL_BITMAP_REPEAT: case FILL_BITMAP_CLIP:
    case FILL_BITMAP_REPEAT_NS: case FILL_BITMAP_CLIP_NS:
        f->bitmap_id = r_u16(r);
        r_matrix(r, &f->matrix);
        break;
    default:
        /* unknown: treat as transparent solid */
        f->type = FILL_SOLID; f->color.a = 0;
        break;
    }
}

static int read_count(Reader *r, int version) {
    int n = r_u8(r);
    if (n == 0xff && version >= 2) n = r_u16(r);
    return n;
}

static void read_fillstyles(Reader *r, ShapeDef *s, int version) {
    int n = read_count(r, version), i;
    s->fills = (FillStyle*)grow(s->fills, &s->capfills, s->nfills + n, sizeof(FillStyle));
    for (i = 0; i < n; i++) {
        if (s->nfills < s->capfills)
            read_fillstyle(r, &s->fills[s->nfills++], version);
        else { FillStyle tmp; read_fillstyle(r, &tmp, version); }
    }
}

static void read_linestyles(Reader *r, ShapeDef *s, int version) {
    int n = read_count(r, version), i;
    s->lines = (LineStyle*)grow(s->lines, &s->caplines, s->nlines + n, sizeof(LineStyle));
    for (i = 0; i < n; i++) {
        LineStyle tmp, *l;
        memset(&tmp, 0, sizeof(tmp));
        l = (s->nlines < s->caplines) ? &s->lines[s->nlines++] : &tmp;
        memset(l, 0, sizeof(*l));
        l->width = r_u16(r);
        if (version >= 4) {
            int join;
            reader_align(r);
            l->start_cap = (int)rb_u(r, 2);
            join = (int)rb_u(r, 2);
            l->join = join;
            l->has_fill  = (int)rb_u(r, 1);
            l->no_hscale = (int)rb_u(r, 1);
            l->no_vscale = (int)rb_u(r, 1);
            rb_u(r, 1);               /* pixel hinting */
            rb_u(r, 5);               /* reserved */
            l->no_close  = (int)rb_u(r, 1);
            l->end_cap   = (int)rb_u(r, 2);
            reader_align(r);
            if (join == 2) l->miter = r_fixed8(r); /* miter limit */
            if (l->has_fill) {
                read_fillstyle(r, &l->fill, version);
                l->color = l->fill.color;
            } else {
                r_rgba(r, &l->color);
            }
        } else {
            read_color(r, &l->color, version);
        }
    }
}

int shape_parse_records(Reader *r, ShapeDef *s, int version, int with_styles) {
    unsigned nfillbits, nlinebits;
    int fill_base = 0, line_base = 0;
    s32 x = 0, y = 0;
    int fill0 = 0, fill1 = 0, line = 0, layer = 0, brk = 0;

    if (with_styles) {
        read_fillstyles(r, s, version);
        read_linestyles(r, s, version);
    }
    reader_align(r);
    nfillbits = rb_u(r, 4);
    nlinebits = rb_u(r, 4);

    for (;;) {
        unsigned typeflag = rb_u(r, 1);
        if (!typeflag) {
            unsigned flags = rb_u(r, 5);
            if (flags == 0) break;                  /* end of shape */
            if (flags & 0x01) {                     /* MoveTo */
                unsigned nb = rb_u(r, 5);
                x = rb_s(r, nb);
                y = rb_s(r, nb);
                brk |= BRK_MOVE;
            }
            /* style indices past the current style lists are no style (Ruffle) */
            if (flags & 0x02) { int v = (int)rb_u(r, nfillbits); fill0 = v && (!with_styles || v <= s->nfills - fill_base) ? v + fill_base : 0; brk |= BRK_FILL0; }
            if (flags & 0x04) { int v = (int)rb_u(r, nfillbits); fill1 = v && (!with_styles || v <= s->nfills - fill_base) ? v + fill_base : 0; brk |= BRK_FILL1; }
            if (flags & 0x08) { int v = (int)rb_u(r, nlinebits); line  = v && (!with_styles || v <= s->nlines - line_base) ? v + line_base : 0; brk |= BRK_LINE; }
            if (flags & 0x10) {                     /* NewStyles */
                brk |= BRK_MOVE | BRK_FILL0 | BRK_FILL1 | BRK_LINE;
                fill_base = s->nfills;
                line_base = s->nlines;
                layer++;
                reader_align(r);
                read_fillstyles(r, s, version);
                read_linestyles(r, s, version);
                reader_align(r);
                nfillbits = rb_u(r, 4);
                nlinebits = rb_u(r, 4);
            }
        } else {
            unsigned straight = rb_u(r, 1);
            unsigned nb = rb_u(r, 4) + 2;
            ShapeEdge e;
            memset(&e, 0, sizeof(e));
            e.x0 = x; e.y0 = y;
            if (straight) {
                unsigned general = rb_u(r, 1);
                if (general) {
                    x += rb_s(r, nb); y += rb_s(r, nb);
                } else {
                    unsigned vert = rb_u(r, 1);
                    if (vert) y += rb_s(r, nb); else x += rb_s(r, nb);
                }
                e.is_curve = 0;
            } else {
                s32 cdx = rb_s(r, nb), cdy = rb_s(r, nb);
                s32 adx, ady;
                e.cx = x + cdx; e.cy = y + cdy;
                adx = rb_s(r, nb); ady = rb_s(r, nb);
                x = e.cx + adx; y = e.cy + ady;
                e.is_curve = 1;
            }
            e.x1 = x; e.y1 = y;
            e.fill0 = fill0; e.fill1 = fill1; e.line = line; e.layer = layer; e.brk = brk; brk = 0;
            s->edges = (ShapeEdge*)grow(s->edges, &s->capedges, s->nedges + 1, sizeof(ShapeEdge));
            if (s->nedges < s->capedges) s->edges[s->nedges++] = e;
        }
        if (reader_eof(r)) break;
    }
    reader_align(r);
    return FLASHNT_OK;
}

ShapeDef *shape_get(SWFMovie *m, SWFChar *ch) {
    ShapeDef *s;
    Reader r;
    int version;
    if (!ch || ch->type != CHAR_SHAPE) return NULL;
    if (ch->parsed) return (ShapeDef*)ch->parsed;

    switch (ch->tagcode) {
    case TAG_DefineShape:  version = 1; break;
    case TAG_DefineShape2: version = 2; break;
    case TAG_DefineShape3: version = 3; break;
    default:               version = 4; break;
    }
    s = (ShapeDef*)calloc(1, sizeof(ShapeDef));
    if (!s) return NULL;
    s->version = version;
    reader_init(&r, ch->data, ch->len);
    r_u16(&r);                       /* id */
    r_rect(&r, &s->bounds);
    s->edge_bounds = s->bounds;
    if (version == 4) {
        r_rect(&r, &s->edge_bounds);
        s->nonzero = (r_u8(&r) & 0x04) != 0;   /* flags: winding rule, scaling strokes */
    }
    shape_parse_records(&r, s, version, 1);
    s->has_brk = 1; s->cache_paths = 1; s->scaled_tess = 1;
    {   /* bitmap fills are smoothed only from SWF 8 on */
        SWFMovie *mv = ch->movie ? ch->movie : m; int i;
        if (mv && mv->version < 8) {
            for (i = 0; i < s->nfills; i++) { if (s->fills[i].type == FILL_BITMAP_REPEAT) s->fills[i].type = FILL_BITMAP_REPEAT_NS; else if (s->fills[i].type == FILL_BITMAP_CLIP) s->fills[i].type = FILL_BITMAP_CLIP_NS; }
            for (i = 0; i < s->nlines; i++) if (s->lines[i].has_fill) { FillStyle *f = &s->lines[i].fill; if (f->type == FILL_BITMAP_REPEAT) f->type = FILL_BITMAP_REPEAT_NS; else if (f->type == FILL_BITMAP_CLIP) f->type = FILL_BITMAP_CLIP_NS; }
        }
    }
    ch->parsed = s;
    return s;
}

void shape_free(ShapeDef *s) {
    if (!s) return;
    shape_free_paths(s);
    free(s->fills); free(s->lines); free(s->edges);
    free(s);
}
