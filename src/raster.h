/* raster.h - software rasterizer modelled on a GPU pipeline (Ruffle's wgpu
 * renderer): every pixel holds `ns` coverage samples at the standard MSAA
 * positions (1 at low quality, 2 medium, 4 high/best); a fill covers the
 * samples inside its outline, its colour is evaluated once per pixel at the
 * pixel centre and blended into each covered sample (premultiplied alpha).
 * Masks use a per-sample stencil like the GPU stencil buffer.
 *
 * Pixel format: premultiplied 0xAARRGGBB in a native u32.  The resolved
 * buffer `px` (the samples averaged) of an opaque stage is B,G,R,X in
 * memory on x86, a 32bpp BI_RGB DIB row, so the Win32 layer can hand it
 * straight to SetDIBitsToDevice. */
#ifndef FLASHNT_RASTER_H
#define FLASHNT_RASTER_H

#include "flashnt.h"

/* the Windows NT build carries x87 and SSE2 copies of the hot pixel loops
   and picks one at startup (g_cpu_sse2) */
#if defined(FLASHNT_NT31) && defined(__GNUC__) && defined(__i386__)
#define FLASHNT_SSE2_DISPATCH 1
extern int g_cpu_sse2;
#endif

/* mask (stencil) states, as Ruffle's MaskState */
enum { MS_NONE, MS_DRAW_STENCIL, MS_CONTENT, MS_CLEAR_STENCIL };
/* blends a draw can do directly (Ruffle's TrivialBlend) */
enum { TB_NORMAL, TB_ADD, TB_SUBTRACT, TB_SCREEN, TB_REPLACE };

typedef struct {
    int  w, h;
    int  ns;           /* samples per pixel */
    u32 *px;           /* resolved pixels, premultiplied 0xAARRGGBB (w * h) */
    u32 *sp;           /* samples (w * h * ns); the same buffer as px when ns == 1 */
    u8  *stencil;      /* per sample, allocated by the first mask */
    int  nmasks, mstate;
    int  maskers;      /* masks being drawn (only the outermost one is) */
    int  nostencil;    /* pushes refused for lack of memory */
    int  blend;        /* TB_* used by fills */
    int  dirty;        /* samples changed since the last resolve */
} Surface;

/* stage quality: 0 low 1 medium 2 high 3 best 4 8x8 5 8x8linear 6 16x16 7 16x16linear */
extern int g_stage_quality;
extern int g_draw_quality;   /* BitmapData.drawWithQuality quality, -1: the stage's */
extern int raster_quality;   /* samples per pixel of the stage (from g_stage_quality) */
int      quality_samples(int q);
Surface *surf_create(int w, int h);
Surface *surf_create_ns(int w, int h, int ns);
void     surf_free(Surface *s);
void     surf_set_samples(Surface *s, int ns);   /* reallocates (content lost) when it changes */
void     surf_clear(Surface *s, SWFColor c);      /* opaque colour */
void     surf_clear_argb(Surface *s, u32 premul); /* any premultiplied colour */
void     surf_fill_from(Surface *s, const u32 *premul);   /* every sample = the given pixel */
void     surf_resolve(Surface *s);                /* px = average of the samples */
/* masks */
void surf_push_mask(Surface *s);
void surf_activate_mask(Surface *s);
void surf_deactivate_mask(Surface *s);
void surf_pop_mask(Surface *s);
/* a whole-pixel draw of premultiplied `src` (lw x lh at lx, ly) through the mask state
   and the trivial blend `tb` (the GPU drawing a pixel-aligned textured quad) */
void surf_draw_pixels(Surface *s, const u32 *src, int lx, int ly, int lw, int lh, int tb);
void surf_draw_pixels_where(Surface *s, const u32 *src, const u32 *where, int lx, int ly, int lw, int lh);

/* decoded bitmap, straight (non-premultiplied) alpha, 0xAARRGGBB;
   pm: the premultiplied texels when known exactly (BitmapData), else NULL */
typedef struct {
    int  w, h;
    u32 *px;
    const u32 *pm;
    u32 *pm_own;       /* pm when this bitmap owns it (a decoded symbol's premultiplied data) */
} Bitmap;

/* JPEG XR file (Loader content); *has_alpha tells whether it has an alpha plane */
Bitmap *jxr_decode(const u8 *d, size_t n, int *has_alpha);

/* an edge in device pixel space.  dir = +1 if original went downward
 * (y increasing), -1 otherwise; y0 < y1 always after normalisation. */
typedef struct {
    float x0, y0, x1, y1;
    int   dir;
} REdge;

/* growable edge list */
typedef struct {
    REdge *e; int n, cap;
} EdgeList;

void edgelist_init(EdgeList *l);
void edgelist_free(EdgeList *l);
void edgelist_add(EdgeList *l, float x0, float y0, float x1, float y1);
void edgelist_clear(EdgeList *l);

enum { PAINT_SOLID, PAINT_LINEAR, PAINT_RADIAL, PAINT_FOCAL, PAINT_BITMAP };

typedef struct {
    int       kind;
    SWFColor  color;              /* solid */
    /* device px -> paint space (gradient: [-16384, 16384] square; bitmap: texels) */
    double    ia, ib, ic, id, ie, iff;
    u8        lut[256][4];        /* gradient texture (r, g, b, a straight) */
    int       spread;             /* 0 pad 1 reflect 2 repeat */
    int       linear_rgb;         /* interpolation in linear RGB */
    double    focal;
    const Bitmap *bmp;
    int       bmp_repeat;
    int       bmp_smooth;
    SWFCxform cx;                 /* applied to result */
} Paint;

/* fill the closed polygon(s) described by edges.  rule: 0 nonzero, 1 evenodd,
   2 nonzero blended once per covering polygon (consistently oriented triangles) */
void raster_fill(Surface *s, const EdgeList *edges, int rule, const Paint *p,
                 const SWFRect *clip_px /* may be NULL */);

/* helper: apply cxform to a colour (Flash integer semantics) */
void cxform_apply(const SWFCxform *cx, u8 *r, u8 *g, u8 *b, u8 *a);

/* helper: invert 2x3 matrix (returns 0 if singular) */
int  matrix_invert(const SWFMatrix *m, double *a, double *b, double *c,
                   double *d, double *e, double *f);

/* premultiplied texel (x, y) of a bitmap (Ruffle rounding) */
u32 bitmap_texel_pm(const Bitmap *bm, int x, int y);

#endif
