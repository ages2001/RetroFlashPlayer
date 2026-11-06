/* shape.h - DefineShape(1..4) parsing into a flat edge list with
 * global (1-based) fill/line style indices.  Also used by DefineMorphShape
 * (start shape only, for now) and by font glyphs (DefineFont2/3). */
#ifndef FLASHNT_SHAPE_H
#define FLASHNT_SHAPE_H

#include "flashnt.h"
#include "swf.h"
#include "reader.h"

enum {
    FILL_SOLID = 0x00, FILL_LINEAR = 0x10, FILL_RADIAL = 0x12,
    FILL_FOCAL = 0x13, FILL_BITMAP_REPEAT = 0x40, FILL_BITMAP_CLIP = 0x41,
    FILL_BITMAP_REPEAT_NS = 0x42, FILL_BITMAP_CLIP_NS = 0x43
};

#define MAX_GRAD 16

typedef struct {
    int      spread;      /* 0 pad, 1 reflect, 2 repeat */
    int      interp;      /* 0 rgb, 1 linear rgb */
    int      n;
    u8       ratio[MAX_GRAD];
    SWFColor color[MAX_GRAD];
    double   focal;
} Gradient;

typedef struct {
    int       type;
    SWFColor  color;
    SWFMatrix matrix;     /* gradient / bitmap matrix */
    Gradient  grad;
    int       bitmap_id;
    int       winding;    /* drawing API paths: 0 the shape's rule, 1 even-odd, 2 non-zero */
} FillStyle;

typedef struct {
    int       width;      /* twips */
    SWFColor  color;
    int       has_fill;
    FillStyle fill;
    int       no_hscale, no_vscale, no_close;
    int       start_cap, end_cap, join;   /* caps: 0 round 1 none 2 square; join: 0 round 1 bevel 2 miter */
    double    miter;
} LineStyle;

typedef struct {
    s32 x0, y0;           /* twips */
    s32 x1, y1;
    s32 cx, cy;           /* control point if is_curve */
    int is_curve;
    int fill0, fill1;     /* global 1-based style index, 0 = none */
    int line;
    int layer;            /* NewStyles records start a new layer */
    int brk;              /* style change record before this edge: BRK_* */
    int cubic;            /* drawing API cubic: > 0 on the first of the `cubic` line edges approximating it
                             (cx, cy and c2x, c2y its control points; the last edge ends at its anchor),
                             -1 on the others; the renderer draws the curve itself */
    s32 c2x, c2y;
} ShapeEdge;
enum { BRK_MOVE = 1, BRK_FILL0 = 2, BRK_FILL1 = 4, BRK_LINE = 8 };

typedef struct {
    int        version;   /* 1..4 */
    SWFRect    bounds;
    SWFRect    edge_bounds;
    FillStyle *fills;  int nfills,  capfills;
    LineStyle *lines;  int nlines,  caplines;
    ShapeEdge *edges;  int nedges,  capedges;
    int        nonzero;   /* DefineShape4 UsesFillWindingRule (else even-odd) */
    int        has_brk;   /* edges carry style change records (parsed shapes) */
    float      tess[4];   /* tessellation scales in use (Ruffle's TessellationCache, LRU first) */
    int        ntess;
    int        cache_paths;   /* the converted draw paths may be kept in `paths` */
    int        scaled_tess;   /* tessellated at the drawn scale (timeline shapes), else at scale 1 */
    void      *paths;
} ShapeDef;
void shape_free_paths(ShapeDef *s);

/* parse (and cache in ch->parsed) a shape character; NULL on failure */
ShapeDef *shape_get(SWFMovie *m, SWFChar *ch);
void      shape_free(ShapeDef *s);

/* Low level: parse a SHAPEWITHSTYLE (or SHAPE for glyphs when
 * with_styles == 0) from the reader into s.  version controls colour
 * format (3+: RGBA) and LINESTYLE2 (4). */
int shape_parse_records(Reader *r, ShapeDef *s, int version, int with_styles);

#endif
