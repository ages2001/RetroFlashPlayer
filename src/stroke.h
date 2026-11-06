#ifndef STROKE_H
#define STROKE_H
/* lyon's stroke tessellator (fixed width), as Ruffle uses it */

enum { LJ_MITER, LJ_MITERCLIP, LJ_ROUND, LJ_BEVEL };
enum { LC_BUTT, LC_SQUARE, LC_ROUND };
enum { SE_BEGIN, SE_LINE, SE_QUAD, SE_END, SE_CLOSE, SE_CUBIC };

typedef struct { int kind; float x, y, cx, cy, c2x, c2y; } StrokeEv;   /* path events in shape px (f32, as lyon gets them) */
typedef struct { float width, tol, miter_limit; int join, start_cap, end_cap; } StrokeStyle;
typedef struct { float *v; int nv, capv; int *t; int nt, capt; } StrokeMesh;   /* vertices (x, y) and triangles */

void lyon_stroke(StrokeMesh *out, const StrokeEv *ev, int nev, const StrokeStyle *st);
void stroke_mesh_free(StrokeMesh *m);

#endif
