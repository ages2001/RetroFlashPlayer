/* bmd.h - BitmapData pixel storage and operations, shared by AVM1 and AVM2.
 *
 * Pixels are kept premultiplied, 0xAARRGGBB, which is what the Flash
 * Player does internally: many operations show the rounding that comes
 * with it, and scripts can observe that rounding through getPixel32.
 * The operations follow Ruffle's core/src/bitmap/operations.rs. */
#ifndef FLASHNT_BMD_H
#define FLASHNT_BMD_H

#include "flashnt.h"
#include "raster.h"

typedef struct BitmapData {
    u32  w, h;
    int  transparent;
    int  disposed;
    u32 *px;            /* premultiplied 0xAARRGGBB, w * h */
    int  refs;          /* owners: the script object, display objects showing it */
    Bitmap view;        /* straight-alpha copy for the renderer */
    int  view_ok;
    int  view_shared;   /* view.px borrows a decoded symbol image (not owned) */
    int  px_shared;     /* px is a reference-counted buffer shared with other copies (bmd_own before writing) */
    u32  version;       /* bumped on every change */
} BitmapData;

typedef struct { u32 x0, y0, x1, y1; } PxRegion;

/* colours */
u32  bmd_premul(u32 c, int transparent);       /* Color::to_premultiplied_alpha */
u32  bmd_unmul(u32 c);                         /* Color::to_un_multiplied_alpha */
u32  bmd_blend_over(u32 dst, u32 src);         /* src over dst, both premultiplied */

/* lifetime */
BitmapData *bmd_new(u32 w, u32 h, int transparent, u32 fill);
BitmapData *bmd_new_pixels(u32 w, u32 h, int transparent, u32 *px);   /* takes px */
BitmapData *bmd_from_straight(const Bitmap *b, int transparent);     /* copy of a decoded bitmap */
BitmapData *bmd_clone(const BitmapData *b);
BitmapData *bmd_ref(BitmapData *b);
void        bmd_unref(BitmapData *b);
void        bmd_dispose(BitmapData *b);
void        bmd_changed(BitmapData *b);
const Bitmap *bmd_view(BitmapData *b);        /* NULL when disposed / empty */
int         bmd_size_valid(int swf_version, u32 w, u32 h);
/* drawing fills that refer to a BitmapData: ids from BMD_FILL_ID0 down */
#define BMD_FILL_ID0 (-1000000)
int         bmd_fill_register(BitmapData *b);
const Bitmap *bmd_fill_lookup(int id);

/* regions */
PxRegion reg_whole(u32 w, u32 h);
PxRegion reg_i32(s32 x, s32 y, s32 w, s32 h);
void     reg_clamp(PxRegion *r, u32 w, u32 h);
void     reg_clamp_with_intersection(PxRegion *self, s32 sx, s32 sy, s32 ox, s32 oy, s32 w, s32 h, PxRegion *other);

/* operations (Ruffle bitmap::operations) */
void bmd_fill_rect(BitmapData *t, s32 x, s32 y, s32 w, s32 h, u32 color);
void bmd_set_pixel32(BitmapData *t, u32 x, u32 y, u32 color);
u32  bmd_get_pixel32(BitmapData *t, u32 x, u32 y);
void bmd_set_pixel(BitmapData *t, u32 x, u32 y, u32 color);
u32  bmd_get_pixel(BitmapData *t, u32 x, u32 y);
int  bmd_flood_fill(BitmapData *t, u32 x, u32 y, u32 color);
void bmd_noise(BitmapData *t, s32 seed, u8 low, u8 high, int channels, int gray);
void bmd_perlin_noise(BitmapData *t, double bx, double by, int octaves, s32 seed, int stitch, int fractal, int channels, int gray, const double *offsets /* 2 * octaves */);
void bmd_copy_channel(BitmapData *t, s32 dx, s32 dy, s32 sx, s32 sy, s32 sw, s32 sh, BitmapData *src, s32 src_channel, s32 dst_channel);
void bmd_color_transform(BitmapData *t, u32 x0, u32 y0, u32 x1, u32 y1, const SWFCxform *cx);
u32  bmd_threshold(BitmapData *t, BitmapData *src, s32 sx, s32 sy, s32 sw, s32 sh, s32 dx, s32 dy, int op, u32 threshold, u32 colour, u32 mask, int copy_source);
int  bmd_threshold_op(const char *s);          /* -1 unknown */
void bmd_scroll(BitmapData *t, s32 x, s32 y);
void bmd_palette_map(BitmapData *t, BitmapData *src, s32 sx, s32 sy, s32 sw, s32 sh, s32 dx, s32 dy, const u32 *chans /* 4 x 256 */);
BitmapData *bmd_compare(BitmapData *l, BitmapData *r);   /* NULL when equivalent */
int  bmd_hit_test_point(BitmapData *t, u8 threshold, s32 x, s32 y);
int  bmd_hit_test_rect(BitmapData *t, u8 threshold, s32 x, s32 y, s32 w, s32 h);
int  bmd_hit_test_bitmap(BitmapData *t, s32 px, s32 py, u8 th, BitmapData *o, s32 ox, s32 oy, u8 oth);
void bmd_color_bounds(BitmapData *t, int find, u32 mask, u32 color, u32 *out /* x y w h */);
void bmd_merge(BitmapData *t, BitmapData *src, s32 sx, s32 sy, s32 sw, s32 sh, s32 dx, s32 dy, s32 rm, s32 gm, s32 bm, s32 am);
void bmd_copy_pixels(BitmapData *t, BitmapData *src, s32 sx, s32 sy, s32 sw, s32 sh, s32 dx, s32 dy, int merge_alpha);
void bmd_copy_pixels_alpha(BitmapData *t, BitmapData *src, s32 sx, s32 sy, s32 sw, s32 sh, s32 dx, s32 dy, BitmapData *alpha, s32 ax, s32 ay, int merge_alpha);
void bmd_copy_region(BitmapData *src, BitmapData *dst, PxRegion sr, PxRegion dr, int blend);   /* copy_on_cpu */
s32  bmd_pixel_dissolve(BitmapData *t, BitmapData *src, s32 sx, s32 sy, s32 sw, s32 sh, s32 dx, s32 dy, s32 seed, s32 num, u32 fill);
s32  bmd_pixel_dissolve_ex(BitmapData *t, BitmapData *src, s32 sx, s32 sy, s32 sw, s32 sh, s32 dx, s32 dy, s32 seed, s32 num, u32 fill, int *empty);

/* BitmapData.draw: `layer` holds the rendered source (premultiplied,
 * t->w * t->h); it is composited over the pixels of `area`. */
void bmd_draw_layer(BitmapData *t, const u32 *layer, PxRegion area);
void bmd_own(BitmapData *b);   /* make px private before writing */
BitmapData *bmd_from_symbol_image(const Bitmap *src, int transparent);   /* cached premultiplication, view shared */
void bmd_draw_layer_area(BitmapData *t, const u32 *layer, PxRegion area);
/* draw a BitmapData with an integer translation and a colour transform
 * (the CPU paths of operations::draw); returns 0 when the general
 * renderer path must be used */
int  bmd_draw_bitmap_fast(BitmapData *t, BitmapData *src, const SWFMatrix *m, const SWFCxform *cx, int blend_mode, const s32 *clip /* x y w h px or NULL */);

/* channel bits */
enum { BMD_RED = 1, BMD_GREEN = 2, BMD_BLUE = 4, BMD_ALPHA = 8 };
enum { TH_EQ, TH_NE, TH_LT, TH_LE, TH_GT, TH_GE };

#endif
