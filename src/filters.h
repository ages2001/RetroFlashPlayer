/* filters.h - bitmap filters in their SWF form (PlaceObject3 FILTERLIST).
 *
 * Display objects keep their filters in this representation, which is
 * what Flash stores: colours with 8-bit alpha, 16.16 fixed distances and
 * blur, 8.8 fixed strength.  Reading DisplayObject.filters back therefore
 * rounds values exactly like Flash does.  Both script VMs convert to and
 * from these records; the renderer applies them. */
#ifndef FLASHNT_FILTERS_H
#define FLASHNT_FILTERS_H

#include "flashnt.h"

enum {
    SF_DROPSHADOW = 0, SF_BLUR, SF_GLOW, SF_BEVEL, SF_GRADIENTGLOW,
    SF_CONVOLUTION, SF_COLORMATRIX, SF_GRADIENTBEVEL, SF_DISPLACEMENT, SF_SHADER
};

/* flags byte (drop shadow / glow / bevel / gradient) */
#define SFF_INNER     0x80
#define SFF_KNOCKOUT  0x40
#define SFF_COMPOSITE 0x20     /* composite the source (drop shadow: !hideObject) */
#define SFF_ONTOP     0x10     /* bevel / gradient: type "full" */
/* convolution flags */
#define SFF_CLAMP     0x02
#define SFF_PRESERVE  0x01

typedef struct SwfFilter {
    int   kind;
    u8    color[4];            /* RGBA: drop shadow / glow colour, bevel shadow, convolution / displacement default */
    u8    color2[4];           /* bevel highlight */
    s32   blur_x, blur_y;      /* 16.16 */
    s32   angle;               /* 16.16 radians */
    s32   distance;            /* 16.16 */
    s16   strength;            /* 8.8 */
    u8    flags;
    u8    passes;
    int   ncolors;             /* gradient records */
    u8    gcolor[16][4];
    u8    ratio[16];
    int   mx, my;              /* convolution */
    float divisor, bias;
    float *cmatrix;            /* ncm values (malloc'd) */
    int   ncm;                 /* convolution matrix length (mx*my) */
    float m[20];               /* colour matrix */
    int   comp_x, comp_y, map_x, map_y, mode;   /* displacement: mode 0 wrap 1 clamp 2 ignore 3 color */
    float scale_x, scale_y;
    float view_x, view_y;      /* displacement: the view scale of the filtered texture (Ruffle viewscale), 0 = 1 */
    void *map_bitmap;          /* displacement map (VM-owned BitmapData object, not freed here) */
    struct BitmapData *map_bmd;   /* its pixels (a reference, released by swf_filters_free) */
    void *shader_obj;          /* ShaderFilter: the script's Shader object (kept alive by the VM) */
    struct PbjJob *pbj;        /* ShaderFilter: the shader and its inputs when assigned (ref-counted) */
} SwfFilter;

/* parse a FILTERLIST (count byte first); *out is malloc'd, returns the count */
int  swf_filters_parse(const u8 *data, size_t len, SwfFilter **out);
void swf_filters_free(SwfFilter *f, int n);
SwfFilter *swf_filters_copy(const SwfFilter *f, int n);

/* Rust-style saturating conversions Flash uses for the fixed fields */
s32  sf_fixed16(double v);
s16  sf_fixed8(double v);
u8   sf_u8_sat(double v);

#endif
