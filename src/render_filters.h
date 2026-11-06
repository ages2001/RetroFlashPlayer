/* render_filters.h - bitmap filters and blend modes on premultiplied layers */
#ifndef FLASHNT_RENDER_FILTERS_H
#define FLASHNT_RENDER_FILTERS_H

#include "flashnt.h"
#include "raster.h"
#include "filters.h"

enum { BLEND_NORMAL0 = 0, BLEND_NORMAL = 1, BLEND_LAYER, BLEND_MULTIPLY, BLEND_SCREEN, BLEND_LIGHTEN, BLEND_DARKEN,
       BLEND_DIFFERENCE, BLEND_ADD, BLEND_SUBTRACT, BLEND_INVERT, BLEND_ALPHA, BLEND_ERASE, BLEND_OVERLAY, BLEND_HARDLIGHT, BLEND_SHADER };

/* margins (px) the filters add around the source */
void flt_extent(const SwfFilter *f, int n, int *l, int *t, int *r, int *b);
/* one filter on a w x h premultiplied layer, in place; (ox, oy): the layer's
   top left relative to the object's origin (device px) */
u32 *flt_apply_rect(const u32 *tex, int w, int h, int sx, int sy, int sw, int sh, const SwfFilter *f);
void flt_apply(u32 *px, int w, int h, const SwfFilter *f, double ox, double oy);
void flt_blur(u32 *px, int w, int h, double bx, double by, int passes);
int  flt_impotent(const SwfFilter *f);
void flt_scale(SwfFilter *f, double kx, double ky);
void flt_dest_rect(const SwfFilter *f, s32 *x0, s32 *y0, s32 *x1, s32 *y1);   /* twips */
/* the layer onto the surface with a blend mode and colour transform;
   bg: inside a layer pass the surface's background colour, else -1 */
void flt_composite(Surface *s, const u32 *layer, int lx, int ly, int lw, int lh, int mode, const SWFCxform *cx, int bg);
/* blendShader: the layer onto the surface through a Pixel Bender kernel (job: a PbjJob) */
void flt_composite_shader(Surface *s, const u32 *layer, int lx, int ly, int lw, int lh, const SWFCxform *cx, void *job);

struct BitmapData;
void bmd_apply_filter(struct BitmapData *dst, struct BitmapData *src, s32 sx, s32 sy, s32 sw, s32 sh, s32 dx, s32 dy, const SwfFilter *f);
void bmd_filter_rect(const SwfFilter *f, double *x, double *y, double *w, double *h);

#endif
