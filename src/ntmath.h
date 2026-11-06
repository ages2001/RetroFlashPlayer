/* ntmath.h - internal helpers shared by the NT runtime's <math.h>
 * implementation (ntmath*.c).  Not used by the Linux build, which links
 * glibc's libm.
 *
 * The code assumes binary64 doubles, a 32-bit or 64-bit int, and that every
 * floating-point operation rounds to double precision (on x87 the precision
 * control is set to 53 bits by nt_fpu_init() at startup, see ntmath.c).
 * Only the exponent range of x87 registers is wider than binary64; routines
 * that may produce subnormal or overflowing intermediate results build the
 * result from its bits or force it through memory (nm_eval).
 *
 * 128-bit integer arithmetic (unsigned __int128 in the original CORE-MATH
 * sources) is emulated with two 64-bit halves, since i386 GCC has no 128-bit
 * integer type. */
#ifndef NTMATH_H
#define NTMATH_H

typedef unsigned long long nm_u64;
typedef long long nm_i64;
typedef unsigned int nm_u32;
typedef union { double f; nm_u64 u; } nm_b64;
/* short names used by the ported CORE-MATH code */
typedef nm_u64 u64;
typedef nm_i64 i64;

/* public names: the NT build defines the libc functions themselves; the test
   harness (NM_TEST) builds them as nmt_sin, nmt_exp, ... next to the host libm */
#ifdef NM_TEST
#define NM_FN(n) nmt_##n
#else
#define NM_FN(n) n
#endif

#define NM_INLINE static __inline__

/* acceptance test of a fast path; the test harness can force the accurate
   paths with -DNM_TEST_SLOW */
#ifdef NM_TEST_SLOW
#define NM_FAST(c) ((c) && 0)
#else
#define NM_FAST(c) __builtin_expect(!!(c), 1)
#endif

NM_INLINE nm_u64 nm_asu64(double x) { nm_b64 v; v.f = x; return v.u; }
NM_INLINE double nm_asdbl(nm_u64 u) { nm_b64 v; v.u = u; return v.f; }

/* force rounding of x to the binary64 range (x87 registers have a wider
   exponent range); only needed where a result may overflow or underflow */
NM_INLINE double nm_eval(double x) { volatile double v = x; return v; }


NM_INLINE double nm_fabs(double x) { return nm_asdbl(nm_asu64(x) & 0x7fffffffffffffffULL); }
NM_INLINE double nm_copysign(double x, double y)
{
    return nm_asdbl((nm_asu64(x) & 0x7fffffffffffffffULL) | (nm_asu64(y) & 0x8000000000000000ULL));
}

/* x rounded to an integer, ties to even (finite x) */
NM_INLINE double nm_roundeven(double x)
{
    double a = nm_fabs(x), r;
    if (a >= 0x1p52) return x;
    r = (a + 0x1p52) - 0x1p52;
    return nm_copysign(r, x);
}
/* x rounded to an integer, ties away from zero (finite x) */
NM_INLINE double nm_round(double x)
{
    double a = nm_fabs(x), r;
    if (a >= 0x1p52) return x;
    r = (a + 0x1p52) - 0x1p52;
    if (r - a == -0.5) r += 1.0;   /* a tie rounded down to even: go up */
    return nm_copysign(r, x);
}
double nm_floor(double x);
double nm_sqrt(double x);

NM_INLINE int nm_clz64(nm_u64 x)   /* x != 0 */
{
    nm_u32 h = (nm_u32)(x >> 32);
    return h ? __builtin_clz(h) : 32 + __builtin_clz((nm_u32)x);
}
NM_INLINE int nm_ctz64(nm_u64 x)   /* x != 0 */
{
    nm_u32 l = (nm_u32)x;
    return l ? __builtin_ctz(l) : 32 + __builtin_ctz((nm_u32)(x >> 32));
}

/* ---- fused multiply-add in software ---- */

/* correctly rounded x*y + z for any inputs, in integer arithmetic (ntmath.c) */
double nm_fma_slow(double x, double y, double z);

/* the fast paths below need |x|, |y| < 2^961, 2^-900 <= |x*y| < 2^901 and
   z = 0 or 2^-900 <= |z| < 2^901: then no operation overflows or underflows */
NM_INLINE int nm_fma_args_ok(nm_u64 ux, nm_u64 uy)
{
    int ex = (int)((ux >> 52) & 0x7ff) - 0x3ff, ey = (int)((uy >> 52) & 0x7ff) - 0x3ff;
    return ex >= -960 && ex <= 960 && ey >= -960 && ey <= 960 && ex + ey >= -900 && ex + ey <= 900;
}

/* Dekker's product: x*y - p exactly, for p = RN(x*y) (Veltkamp splitting;
   needs nm_fma_args_ok) */
NM_INLINE double nm_dekker_err(double x, double y, double p)
{
    double cx = 134217729.0 * x, cy = 134217729.0 * y;
    double xh = cx - (cx - x), xl = x - xh, yh = cy - (cy - y), yl = y - yh;
    return ((xh * yh - p) + xh * yl + xl * yh) + xl * yl;
}

/* correctly rounded x*y + z.  For operands of moderate exponents this is
   the emulation of Boldo and Melquiond ("Emulation of a FMA and correctly
   rounded sums: proved algorithms using rounding to odd", IEEE Trans.
   Computers 57(4), 2008): RN(th + RO(tl + ul)) where uh + ul = x*y and
   th + tl = z + uh exactly; round-to-odd comes from an exact sum by
   adjusting the last bit.  It needs every operation to round to binary64
   without underflow or overflow, which the exponent bounds guarantee.
   Other inputs take the integer path. */
NM_INLINE double nm_fma(double x, double y, double z)
{
    nm_u64 ux = nm_asu64(x), uy = nm_asu64(y), uz = nm_asu64(z);
    int ez = (int)((uz >> 52) & 0x7ff) - 0x3ff;
    if (nm_fma_args_ok(ux, uy) && ((uz << 1) == 0 || (ez >= -900 && ez <= 900))) {
        double uh = x * y, ul = nm_dekker_err(x, y, uh), th, tl, v, t, bb;
        nm_u64 bv;
        /* th + tl = z + uh (2Sum) */
        th = z + uh;
        bb = th - z;
        tl = (z - (th - bb)) + (uh - bb);
        /* v = RO(tl + ul) */
        v = tl + ul;
        bb = v - tl;
        t = (tl - (v - bb)) + (ul - bb);
        if (t != 0) {
            bv = nm_asu64(v);
            if (!(bv & 1)) {
                if ((t > 0) == (v > 0)) bv++; else bv--;
                v = nm_asdbl(bv);
            }
        }
        return th + v;
    }
    return nm_fma_slow(x, y, z);
}

/* x*y - p exactly (= fma(x, y, -p)) for p = RN(x*y) */
NM_INLINE double nm_fma_err(double x, double y, double p)
{
    if (nm_fma_args_ok(nm_asu64(x), nm_asu64(y)))
        return nm_dekker_err(x, y, p);
    return nm_fma_slow(x, y, -p);
}

/* ---- 128-bit unsigned integers ---- */
typedef struct { nm_u64 lo, hi; } nm_u128;

NM_INLINE nm_u128 u128_make(nm_u64 hi, nm_u64 lo) { nm_u128 r; r.hi = hi; r.lo = lo; return r; }
NM_INLINE nm_u128 u128_from64(nm_u64 lo) { nm_u128 r; r.hi = 0; r.lo = lo; return r; }

/* full 64x64 -> 128 product */
NM_INLINE nm_u128 u128_mul64(nm_u64 a, nm_u64 b)
{
    nm_u64 a0 = (nm_u32)a, a1 = a >> 32, b0 = (nm_u32)b, b1 = b >> 32;
    nm_u64 p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    nm_u64 mid = (p00 >> 32) + (nm_u32)p01 + (nm_u32)p10;
    nm_u128 r;
    r.lo = (mid << 32) | (nm_u32)p00;
    r.hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    return r;
}
/* high 64 bits of a 64x64 product */
NM_INLINE nm_u64 u64_mulhi(nm_u64 a, nm_u64 b) { return u128_mul64(a, b).hi; }

NM_INLINE nm_u128 u128_add(nm_u128 a, nm_u128 b)
{
    nm_u128 r;
    r.lo = a.lo + b.lo;
    r.hi = a.hi + b.hi + (r.lo < a.lo);
    return r;
}
NM_INLINE nm_u128 u128_add64(nm_u128 a, nm_u64 b)
{
    nm_u128 r;
    r.lo = a.lo + b;
    r.hi = a.hi + (r.lo < b);
    return r;
}
NM_INLINE nm_u128 u128_sub(nm_u128 a, nm_u128 b)
{
    nm_u128 r;
    r.lo = a.lo - b.lo;
    r.hi = a.hi - b.hi - (a.lo < b.lo);
    return r;
}
NM_INLINE nm_u128 u128_neg(nm_u128 a) { return u128_sub(u128_make(0, 0), a); }
/* shifts are defined for any count 0 <= s (s >= 128 gives 0) */
NM_INLINE nm_u128 u128_shl(nm_u128 a, int s)
{
    nm_u128 r;
    if (s <= 0) return a;
    if (s >= 128) return u128_make(0, 0);
    if (s >= 64) { r.hi = a.lo << (s - 64); r.lo = 0; return r; }
    r.hi = (a.hi << s) | (a.lo >> (64 - s));
    r.lo = a.lo << s;
    return r;
}
NM_INLINE nm_u128 u128_shr(nm_u128 a, int s)
{
    nm_u128 r;
    if (s <= 0) return a;
    if (s >= 128) return u128_make(0, 0);
    if (s >= 64) { r.lo = a.hi >> (s - 64); r.hi = 0; return r; }
    r.lo = (a.lo >> s) | (a.hi << (64 - s));
    r.hi = a.hi >> s;
    return r;
}
NM_INLINE int u128_lt(nm_u128 a, nm_u128 b) { return a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo); }
NM_INLINE int u128_eq(nm_u128 a, nm_u128 b) { return a.hi == b.hi && a.lo == b.lo; }
NM_INLINE int u128_iszero(nm_u128 a) { return (a.hi | a.lo) == 0; }
NM_INLINE nm_u128 u128_and(nm_u128 a, nm_u128 b) { return u128_make(a.hi & b.hi, a.lo & b.lo); }
/* low 128 bits of a 128x128 product */
NM_INLINE nm_u128 u128_mullo(nm_u128 a, nm_u128 b)
{
    nm_u128 r = u128_mul64(a.lo, b.lo);
    r.hi += a.lo * b.hi + a.hi * b.lo;
    return r;
}
/* low 128 bits of a 128x64 product */
NM_INLINE nm_u128 u128_mul_64(nm_u128 a, nm_u64 b)
{
    nm_u128 r = u128_mul64(a.lo, b);
    r.hi += a.hi * b;
    return r;
}
NM_INLINE int u128_clz(nm_u128 a)   /* a != 0 */
{
    return a.hi ? nm_clz64(a.hi) : 64 + nm_clz64(a.lo);
}

#endif
