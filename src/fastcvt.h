/* fastcvt.h - floor/ceil to int without touching the FPU control word.
 *
 * On the x87 a C cast (truncation) makes the compiler switch the rounding
 * mode around every FIST (FNSTCW/FLDCW), which stalls the pipeline; the
 * renderer converts a few such values per pixel.  FISTP in the current
 * mode (round to nearest, as set by the runtime) and a compare give the
 * same floor/ceil.  Values outside the int range use floor()/ceil(). */
#ifndef FLASHNT_FASTCVT_H
#define FLASHNT_FASTCVT_H

#include <math.h>

#ifdef __GNUC__
#define FC_UNUSED __attribute__((unused))
#else
#define FC_UNUSED
#endif

#if defined(__GNUC__) && defined(__i386__) && !defined(__SSE2_MATH__) && !defined(FC_SSE)
static __inline__ FC_UNUSED int fc_round(double t) { int r; __asm__ ("fistpl %0" : "=m" (r) : "t" (t) : "st"); return r; }
#else
static FC_UNUSED int fc_round(double t) { int i = (int)t; double f = t - i; return f >= 0.5 ? i + 1 : f <= -0.5 ? i - 1 : i; }
#endif

#define FC_LIMIT 1073741824.0
static FC_UNUSED int ifloor(double t) {
    int r;
    if (!(t > -FC_LIMIT && t < FC_LIMIT)) return (int)floor(t);
#if defined(__GNUC__) && defined(__i386__) && !defined(__SSE2_MATH__) && !defined(FC_SSE)
    r = fc_round(t); return (double)r > t ? r - 1 : r;
#else
    r = (int)t; return (double)r > t ? r - 1 : r;
#endif
}
static FC_UNUSED int iceil(double t) {
    int r;
    if (!(t > -FC_LIMIT && t < FC_LIMIT)) return (int)ceil(t);
#if defined(__GNUC__) && defined(__i386__) && !defined(__SSE2_MATH__) && !defined(FC_SSE)
    r = fc_round(t); return (double)r < t ? r + 1 : r;
#else
    r = (int)t; return (double)r < t ? r + 1 : r;
#endif
}
#endif
