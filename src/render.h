/* render.h - draws parsed SWF characters onto a Surface. */
#ifndef FLASHNT_RENDER_H
#define FLASHNT_RENDER_H

#include "swf.h"
#include "shape.h"
#include "raster.h"

/* m maps shape twips -> device pixels (include the 1/20 scale). */
void render_shape(Surface *s, SWFMovie *mv, ShapeDef *sh,
                  const SWFMatrix *m, const SWFCxform *cx,
                  const SWFRect *clip_px);

/* a matrix that maps stage twips to device pixels with the given zoom */
void render_stage_matrix(SWFMatrix *out, const SWFRect *stage, double zoom);

/* draw into a BitmapData buffer instead of the frame buffer (NULL restores) */
void render_set_direct_bitmap(const Bitmap *bm);

/* Ruffle's render Matrix (twips -> twips, f32 scales, i32 translation) and
   its CommandHandler primitives: draw_rect fills the unit square (1 px)
   under the matrix, draw_line / draw_line_rect are 1 px lines emulated
   with rectangles (ruffle_render::lines) */
typedef struct { float a, b, c, d; s32 tx, ty; } RMatrix;
void rmat_from(RMatrix *o, const SWFMatrix *wm);   /* our twips -> device px matrix */
void render_matrix_compose(SWFMatrix *out, const SWFMatrix *wm, const SWFMatrix *local);   /* Ruffle's transform stack product */
extern int g_rmat_compose;
void rmat_mul(RMatrix *o, const RMatrix *l, const RMatrix *r);
void rmat_box(RMatrix *o, float sx, float sy, s32 tx, s32 ty);
void rmat_box_rot(RMatrix *o, float sx, float sy, float rot, s32 tx, s32 ty);
void rmat_point(const RMatrix *m, s32 x, s32 y, s32 *ox, s32 *oy);
s32  twips_round_ties_even_px(s32 t);              /* Twips::round_to_pixel_ties_even */
float f32_round_ties_even(float v);
void render_draw_rect(Surface *s, SWFColor c, const RMatrix *m);
void render_draw_line(Surface *s, SWFColor c, const RMatrix *m);
void render_draw_line_rect(Surface *s, SWFColor c, const RMatrix *m);
#endif
