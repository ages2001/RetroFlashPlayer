/* flashnt.h - FLASHNT: a from-scratch SWF (Flash) player for Windows NT 3.1+
 *
 * Shared base types.  Strict C89 so this builds under Microsoft C 8/9
 * (Visual C++ 1.5/2.0/4.0), old MinGW, and modern hosted compilers alike.
 *
 * Coordinates: SWF works in "twips" = 1/20 px.  We keep geometry in twips
 * (signed 32-bit) and only convert to device pixels at blit time.
 */
#ifndef FLASHNT_H
#define FLASHNT_H

#include <stddef.h>

/* ---- fixed width integers without <stdint.h> (not on VC 2.0) ---- */
typedef signed char        s8;
typedef unsigned char      u8;
typedef short              s16;
typedef unsigned short     u16;
typedef int                s32;
typedef unsigned int       u32;

#if defined(_MSC_VER) && _MSC_VER < 1300
typedef __int64            s64;
typedef unsigned __int64   u64;
#else
typedef long long          s64;
typedef unsigned long long u64;
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif

#define FLASHNT_OK      0
#define FLASHNT_ENOMEM -1
#define FLASHNT_EFMT   -2
#define FLASHNT_EIO    -3

/* twips <-> pixels */
#define TWIPS_PER_PX 20
#define TW2PX(t)  ((t) / 20.0)
#define PX2TW(p)  ((s32)((p) * 20.0 + 0.5))

/* ---- 2x3 affine matrix (SWF MATRIX).  scale/rotate in 16.16 fixed,
 *      translate in twips.  Stored as doubles for simplicity/accuracy;
 *      an integer fixed-point path can replace this later for speed. ---- */
typedef struct {
    double sx, sy;   /* scale   */
    double r0, r1;   /* rotate/skew (b,c)  */
    double tx, ty;   /* translate, twips */
} SWFMatrix;

void   swf_matrix_identity(SWFMatrix *m);
/* out = a * b  (apply b then a) */
void   swf_matrix_mul(SWFMatrix *out, const SWFMatrix *a, const SWFMatrix *b);
void   swf_matrix_apply(const SWFMatrix *m, double x, double y,
                        double *ox, double *oy);

/* ---- colour transform (CXFORMWITHALPHA) ---- */
typedef struct {
    s16 mr, mg, mb, ma; /* multiply term, 8.8 fixed (256 = 1.0) */
    s16 ar, ag, ab, aa; /* add term, -255..255 */
} SWFCxform;

void swf_cxform_identity(SWFCxform *c);
void swf_cxform_mul(SWFCxform *out, const SWFCxform *parent,
                    const SWFCxform *child);

/* ---- RGBA colour, premultiplied flag kept external ---- */
typedef struct { u8 r, g, b, a; } SWFColor;

/* ---- bounding rect in twips ---- */
typedef struct { s32 xmin, xmax, ymin, ymax; } SWFRect;

#endif /* FLASHNT_H */
