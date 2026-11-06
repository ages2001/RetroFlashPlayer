/* ntmath.c - <math.h> for the NT runtime (ntcrt.c): exp, log, pow, fma,
 * floor, ceil, fmod, fabs, sqrt, ldexp, frexp, and the x87 set-up.
 * sin, cos, tan are in ntmath_trig.c; asin, acos, atan, atan2 in
 * ntmath_atrig.c.
 *
 * The goal is bit-for-bit agreement with glibc's libm, which the test
 * suites run against, independently of the x87 FPU:
 *  - exp, log, pow are the ARM optimized-routines algorithms that glibc
 *    (and musl) use, with the fused multiply-adds placed exactly where GCC
 *    contracts them in glibc's x86-64 FMA variants (__exp_fma, __log_fma,
 *    __pow_fma, selected on any CPU with FMA/AVX2);
 *  - fma is exact (integer arithmetic), so the results do not depend on the
 *    FPU;
 *  - floor, ceil, fmod, fabs, ldexp, frexp are exact integer
 *    implementations; sqrt is the correctly rounded fsqrt.
 *
 * exp/log/pow and their tables: Copyright (c) 2018, Arm Limited,
 * SPDX-License-Identifier: MIT (as distributed in musl libc, Copyright
 * (c) 2005-2020 Rich Felker, et al., MIT); fmod is adapted from musl.
 * See LICENSE-NTMATH.txt for the full licence texts. */
#include "ntmath.h"

/* ---------------------------------------------------------------- */
/* x87 control word                                                 */

/* 53-bit precision, round to nearest, all exceptions masked.  NT already
   starts threads with 0x27F, but set it explicitly: every routine here
   assumes that each operation rounds to double precision. */
void nt_fpu_init(void)
{
#if defined(__i386__)
    unsigned short cw = 0x027F;
    __asm__ __volatile__ ("fldcw %0" : : "m"(cw));
#endif
}

/* ---------------------------------------------------------------- */
/* exact fused multiply-add                                         */

/* shift a 128-bit value right by s >= 0, or-ing the lost bits into bit 0 */
static nm_u128 shr_sticky(nm_u128 a, int s)
{
    nm_u128 r;
    if (s <= 0) return a;
    if (s >= 128) return u128_from64(!u128_iszero(a));
    r = u128_shr(a, s);
    if (!u128_eq(u128_shl(r, s), a)) r.lo |= 1;
    return r;
}

/* exact fma in integer arithmetic: any finite or special inputs */
static __attribute__((noinline)) double fma_int(double x, double y, double z)
{
    nm_u64 ux = nm_asu64(x), uy = nm_asu64(y), uz = nm_asu64(z), mx, my, mz, m, bits;
    int ex = (int)(ux >> 52) & 0x7ff, ey = (int)(uy >> 52) & 0x7ff, ez = (int)(uz >> 52) & 0x7ff;
    int sp, sz, ep, e, d, top, s, E;
    nm_u128 P, Z, A, B, R;

    /* x or y NaN/Inf, or x*y == 0: the plain expression is exact */
    if (ex == 0x7ff || ey == 0x7ff || (ux << 1) == 0 || (uy << 1) == 0)
        return x * y + z;
    if (ez == 0x7ff)
        return z + z;                 /* finite x*y + Inf/NaN (x*y must not overflow first) */
    mx = ux & 0x000fffffffffffffULL; if (ex) mx |= 1ULL << 52; else ex = 1;
    my = uy & 0x000fffffffffffffULL; if (ey) my |= 1ULL << 52; else ey = 1;
    sp = (int)((ux ^ uy) >> 63);
    /* x*y = P * 2^ep, P left-justified to bit 126 */
    P = u128_mul64(mx, my);
    ep = ex + ey - 2150;
    s = u128_clz(P) - 1;
    P = u128_shl(P, s);
    ep -= s;
    if ((uz << 1) == 0) {
        R = P; e = ep; sz = sp;   /* x*y + 0 = x*y (nonzero) */
    } else {
        mz = uz & 0x000fffffffffffffULL; if (ez) mz |= 1ULL << 52; else ez = 1;
        sz = (int)(uz >> 63);
        s = nm_clz64(mz) - 1;
        Z = u128_make(mz << s, 0);     /* z = Z * 2^(ez - 1075 - 64 - s), top bit 126 */
        e = ez - 1075 - 64 - s;
        if (ep >= e) { A = P; B = shr_sticky(Z, ep - e); e = ep; }
        else { A = Z; B = shr_sticky(P, e - ep); d = sp; sp = sz; sz = d; }
        /* now the result sign is sp unless the subtraction flips it */
        if (sp == sz) R = u128_add(A, B);
        else if (u128_lt(A, B)) { R = u128_sub(B, A); sp = sz; }
        else R = u128_sub(A, B);
        if (u128_iszero(R)) return 0.0;   /* exact cancellation: +0 */
        sz = sp;
    }
    /* R * 2^e, R != 0; round to binary64 */
    top = 127 - u128_clz(R);          /* weight of the leading bit: 2^(top+e) */
    E = top + e + 1023;               /* biased exponent if normal */
    s = (E >= 1) ? top - 52 : -1074 - e;   /* weight of the last kept bit is 2^(s+e) */
    if (s <= 0)
        m = u128_shl(R, -s).lo;       /* exact */
    else if (s > 128)
        m = 0;                        /* below half of the smallest subnormal */
    else {
        nm_u128 q = u128_shr(R, s), low = u128_sub(R, u128_shl(q, s));
        nm_u128 h = u128_shl(u128_from64(1), s - 1);
        m = q.lo;
        if (u128_lt(h, low) || (u128_eq(low, h) && (m & 1))) m++;   /* to nearest, ties to even */
    }
    if (E >= 0x7ff)
        bits = 0x7ff0000000000000ULL;
    else if (E >= 1) {
        bits = ((nm_u64)(E - 1) << 52) + m;   /* m in [2^52, 2^53]: a carry bumps the exponent */
        if (bits > 0x7ff0000000000000ULL) bits = 0x7ff0000000000000ULL;
    } else
        bits = m;                     /* subnormal, or 2^-1022 after rounding */
    return nm_asdbl(bits | ((nm_u64)sz << 63));
}

/* any finite or special inputs (the inline nm_fma in ntmath.h handles
   operands of moderate exponents itself) */
double nm_fma_slow(double x, double y, double z) { return fma_int(x, y, z); }
double NM_FN(fma)(double x, double y, double z) { return nm_fma(x, y, z); }

/* ---------------------------------------------------------------- */
/* exact operations                                                 */

double NM_FN(fabs)(double x) { return nm_fabs(x); }

double nm_floor(double x)
{
    nm_u64 u = nm_asu64(x), m;
    int e = (int)((u >> 52) & 0x7ff) - 0x3ff;
    if (e >= 52) return x;                    /* integral, Inf or NaN */
    if (e < 0) {
        if ((u << 1) == 0) return x;          /* +-0 */
        return (u >> 63) ? -1.0 : 0.0;
    }
    m = 0x000fffffffffffffULL >> e;
    if ((u & m) == 0) return x;
    if (u >> 63) u += m;
    return nm_asdbl(u & ~m);
}
double NM_FN(floor)(double x) { return nm_floor(x); }

double NM_FN(ceil)(double x)
{
    nm_u64 u = nm_asu64(x), m;
    int e = (int)((u >> 52) & 0x7ff) - 0x3ff;
    if (e >= 52) return x;
    if (e < 0) {
        if ((u << 1) == 0) return x;
        return (u >> 63) ? -0.0 : 1.0;
    }
    m = 0x000fffffffffffffULL >> e;
    if ((u & m) == 0) return x;
    if (!(u >> 63)) u += m;
    return nm_asdbl(u & ~m);
}

double nm_sqrt(double x)
{
#if defined(__i386__)
    double r;
    __asm__ ("fsqrt" : "=t"(r) : "0"(x));   /* correctly rounded with 53-bit precision control */
    return r;
#else
    return __builtin_sqrt(x);
#endif
}
double NM_FN(sqrt)(double x) { return nm_sqrt(x); }

/* fmod, from musl (exact) */
double NM_FN(fmod)(double x, double y)
{
    nm_u64 uxi = nm_asu64(x), uyi = nm_asu64(y), i;
    int ex = (int)(uxi >> 52 & 0x7ff), ey = (int)(uyi >> 52 & 0x7ff), sx = (int)(uxi >> 63);

    if ((uyi << 1) == 0 || (uyi << 1) > 0xffe0000000000000ULL || ex == 0x7ff)
        return (x * y) / (x * y);
    if ((uxi << 1) <= (uyi << 1)) {
        if ((uxi << 1) == (uyi << 1)) return 0 * x;
        return x;
    }
    if (!ex) {
        for (i = uxi << 12; i >> 63 == 0; ex--, i <<= 1) ;
        uxi <<= -ex + 1;
    } else {
        uxi &= -1ULL >> 12;
        uxi |= 1ULL << 52;
    }
    if (!ey) {
        for (i = uyi << 12; i >> 63 == 0; ey--, i <<= 1) ;
        uyi <<= -ey + 1;
    } else {
        uyi &= -1ULL >> 12;
        uyi |= 1ULL << 52;
    }
    for (; ex > ey; ex--) {
        i = uxi - uyi;
        if (i >> 63 == 0) {
            if (i == 0) return 0 * x;
            uxi = i;
        }
        uxi <<= 1;
    }
    i = uxi - uyi;
    if (i >> 63 == 0) {
        if (i == 0) return 0 * x;
        uxi = i;
    }
    for (; uxi >> 52 == 0; uxi <<= 1, ex--) ;
    if (ex > 0) {
        uxi -= 1ULL << 52;
        uxi |= (nm_u64)ex << 52;
    } else {
        uxi >>= -ex + 1;
    }
    uxi |= (nm_u64)sx << 63;
    return nm_asdbl(uxi);
}

/* x * 2^n, correctly rounded (as glibc's scalbn) */
double NM_FN(ldexp)(double x, int n)
{
    nm_u64 u = nm_asu64(x), sign = u & 0x8000000000000000ULL, m = u & 0x000fffffffffffffULL, q, rem, half;
    int e = (int)((u >> 52) & 0x7ff), E, sh;
    if (e == 0x7ff || (u << 1) == 0) return x;
    if (e == 0) { sh = nm_clz64(m) - 11; m <<= sh; e = 1 - sh; }
    else m |= 1ULL << 52;
    if (n > 4000) n = 4000;
    if (n < -4000) n = -4000;
    E = e + n;
    if (E >= 0x7ff) return nm_asdbl(sign | 0x7ff0000000000000ULL);
    if (E >= 1) return nm_asdbl(sign | ((nm_u64)E << 52) | (m & 0x000fffffffffffffULL));
    sh = 1 - E;                        /* bits to drop */
    if (sh > 54) return nm_asdbl(sign);
    q = m >> sh;
    rem = m & ((1ULL << sh) - 1);
    half = 1ULL << (sh - 1);
    if (rem > half || (rem == half && (q & 1))) q++;
    return nm_asdbl(sign | q);
}
double NM_FN(scalbn)(double x, int n) { return NM_FN(ldexp)(x, n); }

double NM_FN(frexp)(double x, int *e)
{
    nm_u64 u = nm_asu64(x);
    int ee = (int)((u >> 52) & 0x7ff);
    if (ee == 0) {
        double r;
        if ((u << 1) == 0) { *e = 0; return x; }
        r = NM_FN(frexp)(x * 0x1p64, e);     /* exact */
        *e -= 64;
        return r;
    }
    if (ee == 0x7ff) { *e = 0; return x; }
    *e = ee - 1022;
    return nm_asdbl((u & 0x800fffffffffffffULL) | 0x3fe0000000000000ULL);
}

/* ---------------------------------------------------------------- */
/* exp, log, pow (ARM optimized-routines, MIT)                       */

static double nm_xflow(int sign, int over)
{
    nm_u64 b = over ? 0x7ff0000000000000ULL : 0;
    return nm_asdbl(b | ((nm_u64)(sign != 0) << 63));
}
static double nm_invalid(double x) { return (x - x) / (x - x); }

static const nm_u64 exp_tab[256] = {
  0x0ULL, 0x3ff0000000000000ULL,
  0x3c9b3b4f1a88bf6eULL, 0x3feff63da9fb3335ULL,
  0xbc7160139cd8dc5dULL, 0x3fefec9a3e778061ULL,
  0xbc905e7a108766d1ULL, 0x3fefe315e86e7f85ULL,
  0x3c8cd2523567f613ULL, 0x3fefd9b0d3158574ULL,
  0xbc8bce8023f98efaULL, 0x3fefd06b29ddf6deULL,
  0x3c60f74e61e6c861ULL, 0x3fefc74518759bc8ULL,
  0x3c90a3e45b33d399ULL, 0x3fefbe3ecac6f383ULL,
  0x3c979aa65d837b6dULL, 0x3fefb5586cf9890fULL,
  0x3c8eb51a92fdeffcULL, 0x3fefac922b7247f7ULL,
  0x3c3ebe3d702f9cd1ULL, 0x3fefa3ec32d3d1a2ULL,
  0xbc6a033489906e0bULL, 0x3fef9b66affed31bULL,
  0xbc9556522a2fbd0eULL, 0x3fef9301d0125b51ULL,
  0xbc5080ef8c4eea55ULL, 0x3fef8abdc06c31ccULL,
  0xbc91c923b9d5f416ULL, 0x3fef829aaea92de0ULL,
  0x3c80d3e3e95c55afULL, 0x3fef7a98c8a58e51ULL,
  0xbc801b15eaa59348ULL, 0x3fef72b83c7d517bULL,
  0xbc8f1ff055de323dULL, 0x3fef6af9388c8deaULL,
  0x3c8b898c3f1353bfULL, 0x3fef635beb6fcb75ULL,
  0xbc96d99c7611eb26ULL, 0x3fef5be084045cd4ULL,
  0x3c9aecf73e3a2f60ULL, 0x3fef54873168b9aaULL,
  0xbc8fe782cb86389dULL, 0x3fef4d5022fcd91dULL,
  0x3c8a6f4144a6c38dULL, 0x3fef463b88628cd6ULL,
  0x3c807a05b0e4047dULL, 0x3fef3f49917ddc96ULL,
  0x3c968efde3a8a894ULL, 0x3fef387a6e756238ULL,
  0x3c875e18f274487dULL, 0x3fef31ce4fb2a63fULL,
  0x3c80472b981fe7f2ULL, 0x3fef2b4565e27cddULL,
  0xbc96b87b3f71085eULL, 0x3fef24dfe1f56381ULL,
  0x3c82f7e16d09ab31ULL, 0x3fef1e9df51fdee1ULL,
  0xbc3d219b1a6fbffaULL, 0x3fef187fd0dad990ULL,
  0x3c8b3782720c0ab4ULL, 0x3fef1285a6e4030bULL,
  0x3c6e149289cecb8fULL, 0x3fef0cafa93e2f56ULL,
  0x3c834d754db0abb6ULL, 0x3fef06fe0a31b715ULL,
  0x3c864201e2ac744cULL, 0x3fef0170fc4cd831ULL,
  0x3c8fdd395dd3f84aULL, 0x3feefc08b26416ffULL,
  0xbc86a3803b8e5b04ULL, 0x3feef6c55f929ff1ULL,
  0xbc924aedcc4b5068ULL, 0x3feef1a7373aa9cbULL,
  0xbc9907f81b512d8eULL, 0x3feeecae6d05d866ULL,
  0xbc71d1e83e9436d2ULL, 0x3feee7db34e59ff7ULL,
  0xbc991919b3ce1b15ULL, 0x3feee32dc313a8e5ULL,
  0x3c859f48a72a4c6dULL, 0x3feedea64c123422ULL,
  0xbc9312607a28698aULL, 0x3feeda4504ac801cULL,
  0xbc58a78f4817895bULL, 0x3feed60a21f72e2aULL,
  0xbc7c2c9b67499a1bULL, 0x3feed1f5d950a897ULL,
  0x3c4363ed60c2ac11ULL, 0x3feece086061892dULL,
  0x3c9666093b0664efULL, 0x3feeca41ed1d0057ULL,
  0x3c6ecce1daa10379ULL, 0x3feec6a2b5c13cd0ULL,
  0x3c93ff8e3f0f1230ULL, 0x3feec32af0d7d3deULL,
  0x3c7690cebb7aafb0ULL, 0x3feebfdad5362a27ULL,
  0x3c931dbdeb54e077ULL, 0x3feebcb299fddd0dULL,
  0xbc8f94340071a38eULL, 0x3feeb9b2769d2ca7ULL,
  0xbc87deccdc93a349ULL, 0x3feeb6daa2cf6642ULL,
  0xbc78dec6bd0f385fULL, 0x3feeb42b569d4f82ULL,
  0xbc861246ec7b5cf6ULL, 0x3feeb1a4ca5d920fULL,
  0x3c93350518fdd78eULL, 0x3feeaf4736b527daULL,
  0x3c7b98b72f8a9b05ULL, 0x3feead12d497c7fdULL,
  0x3c9063e1e21c5409ULL, 0x3feeab07dd485429ULL,
  0x3c34c7855019c6eaULL, 0x3feea9268a5946b7ULL,
  0x3c9432e62b64c035ULL, 0x3feea76f15ad2148ULL,
  0xbc8ce44a6199769fULL, 0x3feea5e1b976dc09ULL,
  0xbc8c33c53bef4da8ULL, 0x3feea47eb03a5585ULL,
  0xbc845378892be9aeULL, 0x3feea34634ccc320ULL,
  0xbc93cedd78565858ULL, 0x3feea23882552225ULL,
  0x3c5710aa807e1964ULL, 0x3feea155d44ca973ULL,
  0xbc93b3efbf5e2228ULL, 0x3feea09e667f3bcdULL,
  0xbc6a12ad8734b982ULL, 0x3feea012750bdabfULL,
  0xbc6367efb86da9eeULL, 0x3fee9fb23c651a2fULL,
  0xbc80dc3d54e08851ULL, 0x3fee9f7df9519484ULL,
  0xbc781f647e5a3ecfULL, 0x3fee9f75e8ec5f74ULL,
  0xbc86ee4ac08b7db0ULL, 0x3fee9f9a48a58174ULL,
  0xbc8619321e55e68aULL, 0x3fee9feb564267c9ULL,
  0x3c909ccb5e09d4d3ULL, 0x3feea0694fde5d3fULL,
  0xbc7b32dcb94da51dULL, 0x3feea11473eb0187ULL,
  0x3c94ecfd5467c06bULL, 0x3feea1ed0130c132ULL,
  0x3c65ebe1abd66c55ULL, 0x3feea2f336cf4e62ULL,
  0xbc88a1c52fb3cf42ULL, 0x3feea427543e1a12ULL,
  0xbc9369b6f13b3734ULL, 0x3feea589994cce13ULL,
  0xbc805e843a19ff1eULL, 0x3feea71a4623c7adULL,
  0xbc94d450d872576eULL, 0x3feea8d99b4492edULL,
  0x3c90ad675b0e8a00ULL, 0x3feeaac7d98a6699ULL,
  0x3c8db72fc1f0eab4ULL, 0x3feeace5422aa0dbULL,
  0xbc65b6609cc5e7ffULL, 0x3feeaf3216b5448cULL,
  0x3c7bf68359f35f44ULL, 0x3feeb1ae99157736ULL,
  0xbc93091fa71e3d83ULL, 0x3feeb45b0b91ffc6ULL,
  0xbc5da9b88b6c1e29ULL, 0x3feeb737b0cdc5e5ULL,
  0xbc6c23f97c90b959ULL, 0x3feeba44cbc8520fULL,
  0xbc92434322f4f9aaULL, 0x3feebd829fde4e50ULL,
  0xbc85ca6cd7668e4bULL, 0x3feec0f170ca07baULL,
  0x3c71affc2b91ce27ULL, 0x3feec49182a3f090ULL,
  0x3c6dd235e10a73bbULL, 0x3feec86319e32323ULL,
  0xbc87c50422622263ULL, 0x3feecc667b5de565ULL,
  0x3c8b1c86e3e231d5ULL, 0x3feed09bec4a2d33ULL,
  0xbc91bbd1d3bcbb15ULL, 0x3feed503b23e255dULL,
  0x3c90cc319cee31d2ULL, 0x3feed99e1330b358ULL,
  0x3c8469846e735ab3ULL, 0x3feede6b5579fdbfULL,
  0xbc82dfcd978e9db4ULL, 0x3feee36bbfd3f37aULL,
  0x3c8c1a7792cb3387ULL, 0x3feee89f995ad3adULL,
  0xbc907b8f4ad1d9faULL, 0x3feeee07298db666ULL,
  0xbc55c3d956dcaebaULL, 0x3feef3a2b84f15fbULL,
  0xbc90a40e3da6f640ULL, 0x3feef9728de5593aULL,
  0xbc68d6f438ad9334ULL, 0x3feeff76f2fb5e47ULL,
  0xbc91eee26b588a35ULL, 0x3fef05b030a1064aULL,
  0x3c74ffd70a5fddcdULL, 0x3fef0c1e904bc1d2ULL,
  0xbc91bdfbfa9298acULL, 0x3fef12c25bd71e09ULL,
  0x3c736eae30af0cb3ULL, 0x3fef199bdd85529cULL,
  0x3c8ee3325c9ffd94ULL, 0x3fef20ab5fffd07aULL,
  0x3c84e08fd10959acULL, 0x3fef27f12e57d14bULL,
  0x3c63cdaf384e1a67ULL, 0x3fef2f6d9406e7b5ULL,
  0x3c676b2c6c921968ULL, 0x3fef3720dcef9069ULL,
  0xbc808a1883ccb5d2ULL, 0x3fef3f0b555dc3faULL,
  0xbc8fad5d3ffffa6fULL, 0x3fef472d4a07897cULL,
  0xbc900dae3875a949ULL, 0x3fef4f87080d89f2ULL,
  0x3c74a385a63d07a7ULL, 0x3fef5818dcfba487ULL,
  0xbc82919e2040220fULL, 0x3fef60e316c98398ULL,
  0x3c8e5a50d5c192acULL, 0x3fef69e603db3285ULL,
  0x3c843a59ac016b4bULL, 0x3fef7321f301b460ULL,
  0xbc82d52107b43e1fULL, 0x3fef7c97337b9b5fULL,
  0xbc892ab93b470dc9ULL, 0x3fef864614f5a129ULL,
  0x3c74b604603a88d3ULL, 0x3fef902ee78b3ff6ULL,
  0x3c83c5ec519d7271ULL, 0x3fef9a51fbc74c83ULL,
  0xbc8ff7128fd391f0ULL, 0x3fefa4afa2a490daULL,
  0xbc8dae98e223747dULL, 0x3fefaf482d8e67f1ULL,
  0x3c8ec3bc41aa2008ULL, 0x3fefba1bee615a27ULL,
  0x3c842b94c3a9eb32ULL, 0x3fefc52b376bba97ULL,
  0x3c8a64a931d185eeULL, 0x3fefd0765b6e4540ULL,
  0xbc8e37bae43be3edULL, 0x3fefdbfdad9cbe14ULL,
  0x3c77893b4d91cd9dULL, 0x3fefe7c1819e90d8ULL,
  0x3c5305c14160cc89ULL, 0x3feff3c22b8f71f1ULL,
};
/* {invc, logc} pairs */
static const double log_tab[256] = {
  0x1.734f0c3e0de9fp+0, -0x1.7cc7f79e69000p-2,
  0x1.713786a2ce91fp+0, -0x1.76feec20d0000p-2,
  0x1.6f26008fab5a0p+0, -0x1.713e31351e000p-2,
  0x1.6d1a61f138c7dp+0, -0x1.6b85b38287800p-2,
  0x1.6b1490bc5b4d1p+0, -0x1.65d5590807800p-2,
  0x1.69147332f0cbap+0, -0x1.602d076180000p-2,
  0x1.6719f18224223p+0, -0x1.5a8ca86909000p-2,
  0x1.6524f99a51ed9p+0, -0x1.54f4356035000p-2,
  0x1.63356aa8f24c4p+0, -0x1.4f637c36b4000p-2,
  0x1.614b36b9ddc14p+0, -0x1.49da7fda85000p-2,
  0x1.5f66452c65c4cp+0, -0x1.445923989a800p-2,
  0x1.5d867b5912c4fp+0, -0x1.3edf439b0b800p-2,
  0x1.5babccb5b90dep+0, -0x1.396ce448f7000p-2,
  0x1.59d61f2d91a78p+0, -0x1.3401e17bda000p-2,
  0x1.5805612465687p+0, -0x1.2e9e2ef468000p-2,
  0x1.56397cee76bd3p+0, -0x1.2941b3830e000p-2,
  0x1.54725e2a77f93p+0, -0x1.23ec58cda8800p-2,
  0x1.52aff42064583p+0, -0x1.1e9e129279000p-2,
  0x1.50f22dbb2bddfp+0, -0x1.1956d2b48f800p-2,
  0x1.4f38f4734ded7p+0, -0x1.141679ab9f800p-2,
  0x1.4d843cfde2840p+0, -0x1.0edd094ef9800p-2,
  0x1.4bd3ec078a3c8p+0, -0x1.09aa518db1000p-2,
  0x1.4a27fc3e0258ap+0, -0x1.047e65263b800p-2,
  0x1.4880524d48434p+0, -0x1.feb224586f000p-3,
  0x1.46dce1b192d0bp+0, -0x1.f474a7517b000p-3,
  0x1.453d9d3391854p+0, -0x1.ea4443d103000p-3,
  0x1.43a2744b4845ap+0, -0x1.e020d44e9b000p-3,
  0x1.420b54115f8fbp+0, -0x1.d60a22977f000p-3,
  0x1.40782da3ef4b1p+0, -0x1.cc00104959000p-3,
  0x1.3ee8f5d57fe8fp+0, -0x1.c202956891000p-3,
  0x1.3d5d9a00b4ce9p+0, -0x1.b81178d811000p-3,
  0x1.3bd60c010c12bp+0, -0x1.ae2c9ccd3d000p-3,
  0x1.3a5242b75dab8p+0, -0x1.a45402e129000p-3,
  0x1.38d22cd9fd002p+0, -0x1.9a877681df000p-3,
  0x1.3755bc5847a1cp+0, -0x1.90c6d69483000p-3,
  0x1.35dce49ad36e2p+0, -0x1.87120a645c000p-3,
  0x1.34679984dd440p+0, -0x1.7d68fb4143000p-3,
  0x1.32f5cceffcb24p+0, -0x1.73cb83c627000p-3,
  0x1.3187775a10d49p+0, -0x1.6a39a9b376000p-3,
  0x1.301c8373e3990p+0, -0x1.60b3154b7a000p-3,
  0x1.2eb4ebb95f841p+0, -0x1.5737d76243000p-3,
  0x1.2d50a0219a9d1p+0, -0x1.4dc7b8fc23000p-3,
  0x1.2bef9a8b7fd2ap+0, -0x1.4462c51d20000p-3,
  0x1.2a91c7a0c1babp+0, -0x1.3b08abc830000p-3,
  0x1.293726014b530p+0, -0x1.31b996b490000p-3,
  0x1.27dfa5757a1f5p+0, -0x1.2875490a44000p-3,
  0x1.268b39b1d3bbfp+0, -0x1.1f3b9f879a000p-3,
  0x1.2539d838ff5bdp+0, -0x1.160c8252ca000p-3,
  0x1.23eb7aac9083bp+0, -0x1.0ce7f57f72000p-3,
  0x1.22a012ba940b6p+0, -0x1.03cdc49fea000p-3,
  0x1.2157996cc4132p+0, -0x1.f57bdbc4b8000p-4,
  0x1.201201dd2fc9bp+0, -0x1.e370896404000p-4,
  0x1.1ecf4494d480bp+0, -0x1.d17983ef94000p-4,
  0x1.1d8f5528f6569p+0, -0x1.bf9674ed8a000p-4,
  0x1.1c52311577e7cp+0, -0x1.adc79202f6000p-4,
  0x1.1b17c74cb26e9p+0, -0x1.9c0c3e7288000p-4,
  0x1.19e010c2c1ab6p+0, -0x1.8a646b372c000p-4,
  0x1.18ab07bb670bdp+0, -0x1.78d01b3ac0000p-4,
  0x1.1778a25efbcb6p+0, -0x1.674f145380000p-4,
  0x1.1648d354c31dap+0, -0x1.55e0e6d878000p-4,
  0x1.151b990275fddp+0, -0x1.4485cdea1e000p-4,
  0x1.13f0ea432d24cp+0, -0x1.333d94d6aa000p-4,
  0x1.12c8b7210f9dap+0, -0x1.22079f8c56000p-4,
  0x1.11a3028ecb531p+0, -0x1.10e4698622000p-4,
  0x1.107fbda8434afp+0, -0x1.ffa6c6ad20000p-5,
  0x1.0f5ee0f4e6bb3p+0, -0x1.dda8d4a774000p-5,
  0x1.0e4065d2a9fcep+0, -0x1.bbcece4850000p-5,
  0x1.0d244632ca521p+0, -0x1.9a1894012c000p-5,
  0x1.0c0a77ce2981ap+0, -0x1.788583302c000p-5,
  0x1.0af2f83c636d1p+0, -0x1.5715e67d68000p-5,
  0x1.09ddb98a01339p+0, -0x1.35c8a49658000p-5,
  0x1.08cabaf52e7dfp+0, -0x1.149e364154000p-5,
  0x1.07b9f2f4e28fbp+0, -0x1.e72c082eb8000p-6,
  0x1.06ab58c358f19p+0, -0x1.a55f152528000p-6,
  0x1.059eea5ecf92cp+0, -0x1.63d62cf818000p-6,
  0x1.04949cdd12c90p+0, -0x1.228fb8caa0000p-6,
  0x1.038c6c6f0ada9p+0, -0x1.c317b20f90000p-7,
  0x1.02865137932a9p+0, -0x1.419355daa0000p-7,
  0x1.0182427ea7348p+0, -0x1.81203c2ec0000p-8,
  0x1.008040614b195p+0, -0x1.0040979240000p-9,
  0x1.fe01ff726fa1ap-1, 0x1.feff384900000p-9,
  0x1.fa11cc261ea74p-1, 0x1.7dc41353d0000p-7,
  0x1.f6310b081992ep-1, 0x1.3cea3c4c28000p-6,
  0x1.f25f63ceeadcdp-1, 0x1.b9fc114890000p-6,
  0x1.ee9c8039113e7p-1, 0x1.1b0d8ce110000p-5,
  0x1.eae8078cbb1abp-1, 0x1.58a5bd001c000p-5,
  0x1.e741aa29d0c9bp-1, 0x1.95c8340d88000p-5,
  0x1.e3a91830a99b5p-1, 0x1.d276aef578000p-5,
  0x1.e01e009609a56p-1, 0x1.07598e598c000p-4,
  0x1.dca01e577bb98p-1, 0x1.253f5e30d2000p-4,
  0x1.d92f20b7c9103p-1, 0x1.42edd8b380000p-4,
  0x1.d5cac66fb5ccep-1, 0x1.606598757c000p-4,
  0x1.d272caa5ede9dp-1, 0x1.7da76356a0000p-4,
  0x1.cf26e3e6b2ccdp-1, 0x1.9ab434e1c6000p-4,
  0x1.cbe6da2a77902p-1, 0x1.b78c7bb0d6000p-4,
  0x1.c8b266d37086dp-1, 0x1.d431332e72000p-4,
  0x1.c5894bd5d5804p-1, 0x1.f0a3171de6000p-4,
  0x1.c26b533bb9f8cp-1, 0x1.067152b914000p-3,
  0x1.bf583eeece73fp-1, 0x1.147858292b000p-3,
  0x1.bc4fd75db96c1p-1, 0x1.2266ecdca3000p-3,
  0x1.b951e0c864a28p-1, 0x1.303d7a6c55000p-3,
  0x1.b65e2c5ef3e2cp-1, 0x1.3dfc33c331000p-3,
  0x1.b374867c9888bp-1, 0x1.4ba366b7a8000p-3,
  0x1.b094b211d304ap-1, 0x1.5933928d1f000p-3,
  0x1.adbe885f2ef7ep-1, 0x1.66acd2418f000p-3,
  0x1.aaf1d31603da2p-1, 0x1.740f8ec669000p-3,
  0x1.a82e63fd358a7p-1, 0x1.815c0f51af000p-3,
  0x1.a5740ef09738bp-1, 0x1.8e92954f68000p-3,
  0x1.a2c2a90ab4b27p-1, 0x1.9bb3602f84000p-3,
  0x1.a01a01393f2d1p-1, 0x1.a8bed1c2c0000p-3,
  0x1.9d79f24db3c1bp-1, 0x1.b5b515c01d000p-3,
  0x1.9ae2505c7b190p-1, 0x1.c2967ccbcc000p-3,
  0x1.9852ef297ce2fp-1, 0x1.cf635d5486000p-3,
  0x1.95cbaeea44b75p-1, 0x1.dc1bd3446c000p-3,
  0x1.934c69de74838p-1, 0x1.e8c01b8cfe000p-3,
  0x1.90d4f2f6752e6p-1, 0x1.f5509c0179000p-3,
  0x1.8e6528effd79dp-1, 0x1.00e6c121fb800p-2,
  0x1.8bfce9fcc007cp-1, 0x1.071b80e93d000p-2,
  0x1.899c0dabec30ep-1, 0x1.0d46b9e867000p-2,
  0x1.87427aa2317fbp-1, 0x1.13687334bd000p-2,
  0x1.84f00acb39a08p-1, 0x1.1980d67234800p-2,
  0x1.82a49e8653e55p-1, 0x1.1f8ffe0cc8000p-2,
  0x1.8060195f40260p-1, 0x1.2595fd7636800p-2,
  0x1.7e22563e0a329p-1, 0x1.2b9300914a800p-2,
  0x1.7beb377dcb5adp-1, 0x1.3187210436000p-2,
  0x1.79baa679725c2p-1, 0x1.377266dec1800p-2,
  0x1.77907f2170657p-1, 0x1.3d54ffbaf3000p-2,
  0x1.756cadbd6130cp-1, 0x1.432eee32fe000p-2,
};
/* {invc, logc, logctail} triples */
static const double pow_tab[384] = {
  0x1.6a00000000000p+0, -0x1.62c82f2b9c800p-2, 0x1.ab42428375680p-48,
  0x1.6800000000000p+0, -0x1.5d1bdbf580800p-2, -0x1.ca508d8e0f720p-46,
  0x1.6600000000000p+0, -0x1.5767717455800p-2, -0x1.362a4d5b6506dp-45,
  0x1.6400000000000p+0, -0x1.51aad872df800p-2, -0x1.684e49eb067d5p-49,
  0x1.6200000000000p+0, -0x1.4be5f95777800p-2, -0x1.41b6993293ee0p-47,
  0x1.6000000000000p+0, -0x1.4618bc21c6000p-2, 0x1.3d82f484c84ccp-46,
  0x1.5e00000000000p+0, -0x1.404308686a800p-2, 0x1.c42f3ed820b3ap-50,
  0x1.5c00000000000p+0, -0x1.3a64c55694800p-2, 0x1.0b1c686519460p-45,
  0x1.5a00000000000p+0, -0x1.347dd9a988000p-2, 0x1.5594dd4c58092p-45,
  0x1.5800000000000p+0, -0x1.2e8e2bae12000p-2, 0x1.67b1e99b72bd8p-45,
  0x1.5600000000000p+0, -0x1.2895a13de8800p-2, 0x1.5ca14b6cfb03fp-46,
  0x1.5600000000000p+0, -0x1.2895a13de8800p-2, 0x1.5ca14b6cfb03fp-46,
  0x1.5400000000000p+0, -0x1.22941fbcf7800p-2, -0x1.65a242853da76p-46,
  0x1.5200000000000p+0, -0x1.1c898c1699800p-2, -0x1.fafbc68e75404p-46,
  0x1.5000000000000p+0, -0x1.1675cababa800p-2, 0x1.f1fc63382a8f0p-46,
  0x1.4e00000000000p+0, -0x1.1058bf9ae4800p-2, -0x1.6a8c4fd055a66p-45,
  0x1.4c00000000000p+0, -0x1.0a324e2739000p-2, -0x1.c6bee7ef4030ep-47,
  0x1.4a00000000000p+0, -0x1.0402594b4d000p-2, -0x1.036b89ef42d7fp-48,
  0x1.4a00000000000p+0, -0x1.0402594b4d000p-2, -0x1.036b89ef42d7fp-48,
  0x1.4800000000000p+0, -0x1.fb9186d5e4000p-3, 0x1.d572aab993c87p-47,
  0x1.4600000000000p+0, -0x1.ef0adcbdc6000p-3, 0x1.b26b79c86af24p-45,
  0x1.4400000000000p+0, -0x1.e27076e2af000p-3, -0x1.72f4f543fff10p-46,
  0x1.4200000000000p+0, -0x1.d5c216b4fc000p-3, 0x1.1ba91bbca681bp-45,
  0x1.4000000000000p+0, -0x1.c8ff7c79aa000p-3, 0x1.7794f689f8434p-45,
  0x1.4000000000000p+0, -0x1.c8ff7c79aa000p-3, 0x1.7794f689f8434p-45,
  0x1.3e00000000000p+0, -0x1.bc286742d9000p-3, 0x1.94eb0318bb78fp-46,
  0x1.3c00000000000p+0, -0x1.af3c94e80c000p-3, 0x1.a4e633fcd9066p-52,
  0x1.3a00000000000p+0, -0x1.a23bc1fe2b000p-3, -0x1.58c64dc46c1eap-45,
  0x1.3a00000000000p+0, -0x1.a23bc1fe2b000p-3, -0x1.58c64dc46c1eap-45,
  0x1.3800000000000p+0, -0x1.9525a9cf45000p-3, -0x1.ad1d904c1d4e3p-45,
  0x1.3600000000000p+0, -0x1.87fa06520d000p-3, 0x1.bbdbf7fdbfa09p-45,
  0x1.3400000000000p+0, -0x1.7ab890210e000p-3, 0x1.bdb9072534a58p-45,
  0x1.3400000000000p+0, -0x1.7ab890210e000p-3, 0x1.bdb9072534a58p-45,
  0x1.3200000000000p+0, -0x1.6d60fe719d000p-3, -0x1.0e46aa3b2e266p-46,
  0x1.3000000000000p+0, -0x1.5ff3070a79000p-3, -0x1.e9e439f105039p-46,
  0x1.3000000000000p+0, -0x1.5ff3070a79000p-3, -0x1.e9e439f105039p-46,
  0x1.2e00000000000p+0, -0x1.526e5e3a1b000p-3, -0x1.0de8b90075b8fp-45,
  0x1.2c00000000000p+0, -0x1.44d2b6ccb8000p-3, 0x1.70cc16135783cp-46,
  0x1.2c00000000000p+0, -0x1.44d2b6ccb8000p-3, 0x1.70cc16135783cp-46,
  0x1.2a00000000000p+0, -0x1.371fc201e9000p-3, 0x1.178864d27543ap-48,
  0x1.2800000000000p+0, -0x1.29552f81ff000p-3, -0x1.48d301771c408p-45,
  0x1.2600000000000p+0, -0x1.1b72ad52f6000p-3, -0x1.e80a41811a396p-45,
  0x1.2600000000000p+0, -0x1.1b72ad52f6000p-3, -0x1.e80a41811a396p-45,
  0x1.2400000000000p+0, -0x1.0d77e7cd09000p-3, 0x1.a699688e85bf4p-47,
  0x1.2400000000000p+0, -0x1.0d77e7cd09000p-3, 0x1.a699688e85bf4p-47,
  0x1.2200000000000p+0, -0x1.fec9131dbe000p-4, -0x1.575545ca333f2p-45,
  0x1.2000000000000p+0, -0x1.e27076e2b0000p-4, 0x1.a342c2af0003cp-45,
  0x1.2000000000000p+0, -0x1.e27076e2b0000p-4, 0x1.a342c2af0003cp-45,
  0x1.1e00000000000p+0, -0x1.c5e548f5bc000p-4, -0x1.d0c57585fbe06p-46,
  0x1.1c00000000000p+0, -0x1.a926d3a4ae000p-4, 0x1.53935e85baac8p-45,
  0x1.1c00000000000p+0, -0x1.a926d3a4ae000p-4, 0x1.53935e85baac8p-45,
  0x1.1a00000000000p+0, -0x1.8c345d631a000p-4, 0x1.37c294d2f5668p-46,
  0x1.1a00000000000p+0, -0x1.8c345d631a000p-4, 0x1.37c294d2f5668p-46,
  0x1.1800000000000p+0, -0x1.6f0d28ae56000p-4, -0x1.69737c93373dap-45,
  0x1.1600000000000p+0, -0x1.51b073f062000p-4, 0x1.f025b61c65e57p-46,
  0x1.1600000000000p+0, -0x1.51b073f062000p-4, 0x1.f025b61c65e57p-46,
  0x1.1400000000000p+0, -0x1.341d7961be000p-4, 0x1.c5edaccf913dfp-45,
  0x1.1400000000000p+0, -0x1.341d7961be000p-4, 0x1.c5edaccf913dfp-45,
  0x1.1200000000000p+0, -0x1.16536eea38000p-4, 0x1.47c5e768fa309p-46,
  0x1.1000000000000p+0, -0x1.f0a30c0118000p-5, 0x1.d599e83368e91p-45,
  0x1.1000000000000p+0, -0x1.f0a30c0118000p-5, 0x1.d599e83368e91p-45,
  0x1.0e00000000000p+0, -0x1.b42dd71198000p-5, 0x1.c827ae5d6704cp-46,
  0x1.0e00000000000p+0, -0x1.b42dd71198000p-5, 0x1.c827ae5d6704cp-46,
  0x1.0c00000000000p+0, -0x1.77458f632c000p-5, -0x1.cfc4634f2a1eep-45,
  0x1.0c00000000000p+0, -0x1.77458f632c000p-5, -0x1.cfc4634f2a1eep-45,
  0x1.0a00000000000p+0, -0x1.39e87b9fec000p-5, 0x1.502b7f526feaap-48,
  0x1.0a00000000000p+0, -0x1.39e87b9fec000p-5, 0x1.502b7f526feaap-48,
  0x1.0800000000000p+0, -0x1.f829b0e780000p-6, -0x1.980267c7e09e4p-45,
  0x1.0800000000000p+0, -0x1.f829b0e780000p-6, -0x1.980267c7e09e4p-45,
  0x1.0600000000000p+0, -0x1.7b91b07d58000p-6, -0x1.88d5493faa639p-45,
  0x1.0400000000000p+0, -0x1.fc0a8b0fc0000p-7, -0x1.f1e7cf6d3a69cp-50,
  0x1.0400000000000p+0, -0x1.fc0a8b0fc0000p-7, -0x1.f1e7cf6d3a69cp-50,
  0x1.0200000000000p+0, -0x1.fe02a6b100000p-8, -0x1.9e23f0dda40e4p-46,
  0x1.0200000000000p+0, -0x1.fe02a6b100000p-8, -0x1.9e23f0dda40e4p-46,
  0x1.0000000000000p+0, 0x0.0000000000000p+0, 0x0.0000000000000p+0,
  0x1.0000000000000p+0, 0x0.0000000000000p+0, 0x0.0000000000000p+0,
  0x1.fc00000000000p-1, 0x1.0101575890000p-7, -0x1.0c76b999d2be8p-46,
  0x1.f800000000000p-1, 0x1.0205658938000p-6, -0x1.3dc5b06e2f7d2p-45,
  0x1.f400000000000p-1, 0x1.8492528c90000p-6, -0x1.aa0ba325a0c34p-45,
  0x1.f000000000000p-1, 0x1.0415d89e74000p-5, 0x1.111c05cf1d753p-47,
  0x1.ec00000000000p-1, 0x1.466aed42e0000p-5, -0x1.c167375bdfd28p-45,
  0x1.e800000000000p-1, 0x1.894aa149fc000p-5, -0x1.97995d05a267dp-46,
  0x1.e400000000000p-1, 0x1.ccb73cdddc000p-5, -0x1.a68f247d82807p-46,
  0x1.e200000000000p-1, 0x1.eea31c006c000p-5, -0x1.e113e4fc93b7bp-47,
  0x1.de00000000000p-1, 0x1.1973bd1466000p-4, -0x1.5325d560d9e9bp-45,
  0x1.da00000000000p-1, 0x1.3bdf5a7d1e000p-4, 0x1.cc85ea5db4ed7p-45,
  0x1.d600000000000p-1, 0x1.5e95a4d97a000p-4, -0x1.c69063c5d1d1ep-45,
  0x1.d400000000000p-1, 0x1.700d30aeac000p-4, 0x1.c1e8da99ded32p-49,
  0x1.d000000000000p-1, 0x1.9335e5d594000p-4, 0x1.3115c3abd47dap-45,
  0x1.cc00000000000p-1, 0x1.b6ac88dad6000p-4, -0x1.390802bf768e5p-46,
  0x1.ca00000000000p-1, 0x1.c885801bc4000p-4, 0x1.646d1c65aacd3p-45,
  0x1.c600000000000p-1, 0x1.ec739830a2000p-4, -0x1.dc068afe645e0p-45,
  0x1.c400000000000p-1, 0x1.fe89139dbe000p-4, -0x1.534d64fa10afdp-45,
  0x1.c000000000000p-1, 0x1.1178e8227e000p-3, 0x1.1ef78ce2d07f2p-45,
  0x1.be00000000000p-1, 0x1.1aa2b7e23f000p-3, 0x1.ca78e44389934p-45,
  0x1.ba00000000000p-1, 0x1.2d1610c868000p-3, 0x1.39d6ccb81b4a1p-47,
  0x1.b800000000000p-1, 0x1.365fcb0159000p-3, 0x1.62fa8234b7289p-51,
  0x1.b400000000000p-1, 0x1.4913d8333b000p-3, 0x1.5837954fdb678p-45,
  0x1.b200000000000p-1, 0x1.527e5e4a1b000p-3, 0x1.633e8e5697dc7p-45,
  0x1.ae00000000000p-1, 0x1.6574ebe8c1000p-3, 0x1.9cf8b2c3c2e78p-46,
  0x1.ac00000000000p-1, 0x1.6f0128b757000p-3, -0x1.5118de59c21e1p-45,
  0x1.aa00000000000p-1, 0x1.7898d85445000p-3, -0x1.c661070914305p-46,
  0x1.a600000000000p-1, 0x1.8beafeb390000p-3, -0x1.73d54aae92cd1p-47,
  0x1.a400000000000p-1, 0x1.95a5adcf70000p-3, 0x1.7f22858a0ff6fp-47,
  0x1.a000000000000p-1, 0x1.a93ed3c8ae000p-3, -0x1.8724350562169p-45,
  0x1.9e00000000000p-1, 0x1.b31d8575bd000p-3, -0x1.c358d4eace1aap-47,
  0x1.9c00000000000p-1, 0x1.bd087383be000p-3, -0x1.d4bc4595412b6p-45,
  0x1.9a00000000000p-1, 0x1.c6ffbc6f01000p-3, -0x1.1ec72c5962bd2p-48,
  0x1.9600000000000p-1, 0x1.db13db0d49000p-3, -0x1.aff2af715b035p-45,
  0x1.9400000000000p-1, 0x1.e530effe71000p-3, 0x1.212276041f430p-51,
  0x1.9200000000000p-1, 0x1.ef5ade4dd0000p-3, -0x1.a211565bb8e11p-51,
  0x1.9000000000000p-1, 0x1.f991c6cb3b000p-3, 0x1.bcbecca0cdf30p-46,
  0x1.8c00000000000p-1, 0x1.07138604d5800p-2, 0x1.89cdb16ed4e91p-48,
  0x1.8a00000000000p-1, 0x1.0c42d67616000p-2, 0x1.7188b163ceae9p-45,
  0x1.8800000000000p-1, 0x1.1178e8227e800p-2, -0x1.c210e63a5f01cp-45,
  0x1.8600000000000p-1, 0x1.16b5ccbacf800p-2, 0x1.b9acdf7a51681p-45,
  0x1.8400000000000p-1, 0x1.1bf99635a6800p-2, 0x1.ca6ed5147bdb7p-45,
  0x1.8200000000000p-1, 0x1.214456d0eb800p-2, 0x1.a87deba46baeap-47,
  0x1.7e00000000000p-1, 0x1.2bef07cdc9000p-2, 0x1.a9cfa4a5004f4p-45,
  0x1.7c00000000000p-1, 0x1.314f1e1d36000p-2, -0x1.8e27ad3213cb8p-45,
  0x1.7a00000000000p-1, 0x1.36b6776be1000p-2, 0x1.16ecdb0f177c8p-46,
  0x1.7800000000000p-1, 0x1.3c25277333000p-2, 0x1.83b54b606bd5cp-46,
  0x1.7600000000000p-1, 0x1.419b423d5e800p-2, 0x1.8e436ec90e09dp-47,
  0x1.7400000000000p-1, 0x1.4718dc271c800p-2, -0x1.f27ce0967d675p-45,
  0x1.7200000000000p-1, 0x1.4c9e09e173000p-2, -0x1.e20891b0ad8a4p-45,
  0x1.7000000000000p-1, 0x1.522ae0738a000p-2, 0x1.ebe708164c759p-45,
  0x1.6e00000000000p-1, 0x1.57bf753c8d000p-2, 0x1.fadedee5d40efp-46,
  0x1.6c00000000000p-1, 0x1.5d5bddf596000p-2, -0x1.a0b2a08a465dcp-47,
};

#define EXP_N 128
static const double exp_invln2N = 0x1.71547652b82fep0 * EXP_N;
static const double exp_shift = 0x1.8p52;
static const double exp_negln2hiN = -0x1.62e42fefa0000p-8;
static const double exp_negln2loN = -0x1.cf79abc9e3b3ap-47;
static const double exp_C2 = 0x1.ffffffffffdbdp-2, exp_C3 = 0x1.555555555543cp-3,
                    exp_C4 = 0x1.55555cf172b91p-5, exp_C5 = 0x1.1111167a4d017p-7;

/* exp(x) = scale * (1 + tmp) when the scale's exponent is out of range */
static double exp_special(double tmp, nm_u64 sbits, nm_u64 ki)
{
    double scale, y, t, hi, lo;
    if ((ki & 0x80000000) == 0) {
        /* k > 0, the exponent of scale might have overflowed by <= 460 */
        sbits -= 1009ULL << 52;
        scale = nm_asdbl(sbits);
        y = 0x1p1009 * nm_fma(tmp, scale, scale);
        return nm_eval(y);
    }
    /* k < 0: round y to the right precision before scaling it into the
       subnormal range */
    sbits += 1022ULL << 52;
    scale = nm_asdbl(sbits);
    t = tmp * scale;
    y = scale + t;
    if (y < 1.0) {
        lo = t + (scale - y);
        hi = y + 1.0;
        lo = lo + (y + (1.0 - hi));
        y = (hi + lo) - 1.0;
        if (y == 0.0) return 0.0;
    }
    return nm_eval(y * 0x1p-1022);   /* exact */
}

double NM_FN(exp)(double x)
{
    nm_u32 abstop = (nm_u32)(nm_asu64(x) >> 52) & 0x7ff;
    nm_u64 ki, idx, top, sbits;
    double kd, r, r2, tail, tmp, scale;

    if (abstop - 0x3c9 >= 0x408 - 0x3c9) {
        if ((int)(abstop - 0x3c9) < 0)
            return 1.0 + x;                    /* |x| < 2^-54 */
        if (abstop >= 0x409) {                 /* |x| >= 1024 */
            if (nm_asu64(x) == 0xfff0000000000000ULL) return 0.0;
            if (abstop >= 0x7ff) return 1.0 + x;
            return nm_xflow(0, !(nm_asu64(x) >> 63));   /* +inf or +0 */
        }
        abstop = 0;                            /* large x: special case below */
    }
    /* exp(x) = 2^(k/N) * exp(r), with exp(r) in [2^(-1/2N),2^(1/2N)] */
    kd = nm_fma(exp_invln2N, x, exp_shift);
    ki = nm_asu64(kd);
    kd -= exp_shift;
    r = nm_fma(exp_negln2hiN, kd, x);
    r = nm_fma(exp_negln2loN, kd, r);
    idx = 2 * (ki % EXP_N);
    top = ki << (52 - 7);
    tail = nm_asdbl(exp_tab[idx]);
    sbits = exp_tab[idx + 1] + top;
    r2 = r * r;
    tmp = nm_fma(nm_fma(exp_C3, r, exp_C2), r2, r + tail);
    tmp = nm_fma(r2 * r2, nm_fma(exp_C5, r, exp_C4), tmp);
    if (abstop == 0)
        return exp_special(tmp, sbits, ki);
    scale = nm_asdbl(sbits);
    return nm_fma(tmp, scale, scale);
}

static const double log_ln2hi = 0x1.62e42fefa3800p-1, log_ln2lo = 0x1.ef35793c76730p-45;
static const double log_A[5] = {
    -0x1.0000000000001p-1, 0x1.555555551305bp-2, -0x1.fffffffeb459p-3,
    0x1.999b324f10111p-3, -0x1.55575e506c89fp-3
};
static const double log_B[11] = {
    -0x1p-1, 0x1.5555555555577p-2, -0x1.ffffffffffdcbp-3, 0x1.999999995dd0cp-3,
    -0x1.55555556745a7p-3, 0x1.24924a344de3p-3, -0x1.fffffa4423d65p-4, 0x1.c7184282ad6cap-4,
    -0x1.999eb43b068ffp-4, 0x1.78182f7afd085p-4, -0x1.5521375d145cdp-4
};

double NM_FN(log)(double x)
{
    nm_u64 ix = nm_asu64(x), iz, tmp;
    nm_u32 top = (nm_u32)(ix >> 48);
    double w, z, r, r2, r3, y, invc, logc, kd, hi, lo, rhi, rlo, p1, p2, p3, t;
    int k, i;

    if (ix - 0x3fee000000000000ULL < 0x3ff1090000000000ULL - 0x3fee000000000000ULL) {
        /* x close to 1.0 */
        if (ix == 0x3ff0000000000000ULL) return 0;
        r = x - 1.0;
        r2 = r * r;
        r3 = r * r2;
        p1 = nm_fma(log_B[3], r2, nm_fma(log_B[2], r, log_B[1]));
        p2 = nm_fma(log_B[6], r2, nm_fma(log_B[5], r, log_B[4]));
        p3 = nm_fma(log_B[9], r2, nm_fma(log_B[8], r, log_B[7]));
        p3 = nm_fma(log_B[10], r3, p3);
        p2 = nm_fma(p3, r3, p2);
        p1 = nm_fma(p2, r3, p1);
        /* split r: rhi = r + r*2^27 - r*2^27 (both products fused) */
        t = r * 0x1p27 + r;           /* products by 2^27 are exact: these */
        rhi = t - r * 0x1p27;         /* equal the fused forms */
        rlo = r - rhi;
        t = rhi * rhi;
        hi = nm_fma(t, log_B[0], r);
        lo = nm_fma(t, log_B[0], r - hi);
        lo = nm_fma(log_B[0] * rlo, r + rhi, lo);
        y = nm_fma(p1, r3, lo);
        return hi + y;
    }
    if (top - 0x0010 >= 0x7ff0 - 0x0010) {
        /* x < 0x1p-1022 or inf or nan */
        if (ix * 2 == 0) return nm_asdbl(0xfff0000000000000ULL);   /* -inf */
        if (ix == 0x7ff0000000000000ULL) return x;
        if ((top & 0x8000) || (top & 0x7ff0) == 0x7ff0) return nm_invalid(x);
        /* subnormal: normalize */
        ix = nm_asu64(x * 0x1p52);
        ix -= 52ULL << 52;
    }
    tmp = ix - 0x3fe6000000000000ULL;
    i = (int)((tmp >> (52 - 7)) % 128);
    k = (int)((nm_i64)tmp >> 52);
    iz = ix - (tmp & 0xfffULL << 52);
    invc = log_tab[2 * i];
    logc = log_tab[2 * i + 1];
    z = nm_asdbl(iz);
    r = nm_fma(invc, z, -1.0);
    kd = (double)k;
    w = nm_fma(log_ln2hi, kd, logc);
    hi = r + w;
    lo = nm_fma(log_ln2lo, kd, (w - hi) + r);
    r2 = r * r;
    y = nm_fma(log_A[0], r2, lo);
    p1 = nm_fma(nm_fma(log_A[4], r, log_A[3]), r2, nm_fma(log_A[2], r, log_A[1]));
    y = nm_fma(r * r2, p1, y);
    return y + hi;
}

static const double pow_ln2hi = 0x1.62e42fefa3800p-1, pow_ln2lo = 0x1.ef35793c76730p-45;
static const double pow_A[7] = {
    -0x1p-1, 0x1.555555555556p-2 * -2, -0x1.0000000000006p-2 * -2, 0x1.999999959554ep-3 * 4,
    -0x1.555555529a47ap-3 * 4, 0x1.2495b9b4845e9p-3 * -8, -0x1.0002b8b263fc3p-3 * -8
};

/* y + *tail = log(x), where ix is the bit representation of x, normalized
   in the subnormal range using the sign bit for the exponent */
static double pow_log(nm_u64 ix, double *tail)
{
    double z, r, y, invc, logc, logctail, kd, hi, t1, t2, lo, lo1, lo2, lo3, lo4, ar, ar2, ar3, p;
    nm_u64 iz, tmp;
    int k, i;

    tmp = ix - 0x3fe6955500000000ULL;
    i = (int)((tmp >> (52 - 7)) % 128);
    k = (int)((nm_i64)tmp >> 52);
    iz = ix - (tmp & 0xfffULL << 52);
    z = nm_asdbl(iz);
    kd = (double)k;
    invc = pow_tab[3 * i];
    logc = pow_tab[3 * i + 1];
    logctail = pow_tab[3 * i + 2];
    r = nm_fma(z, invc, -1.0);
    t1 = nm_fma(kd, pow_ln2hi, logc);
    t2 = r + t1;
    lo1 = nm_fma(kd, pow_ln2lo, logctail);
    lo2 = r + (t1 - t2);
    ar = r * pow_A[0];
    ar2 = r * ar;
    ar3 = r * ar2;
    hi = t2 + ar2;
    lo3 = nm_fma_err(r, ar, ar2);
    lo4 = ar2 + (t2 - hi);
    p = nm_fma(ar2, nm_fma(ar2, nm_fma(r, pow_A[6], pow_A[5]), nm_fma(r, pow_A[4], pow_A[3])),
               nm_fma(r, pow_A[2], pow_A[1]));
    lo = nm_fma(ar3, p, lo4 + (lo3 + (lo1 + lo2)));
    y = hi + lo;
    *tail = lo + (hi - y);
    return y;
}

static double pow_special(double tmp, nm_u64 sbits, nm_u64 ki)
{
    double scale, y, t, hi, lo, one;
    if ((ki & 0x80000000) == 0) {
        sbits -= 1009ULL << 52;
        scale = nm_asdbl(sbits);
        y = 0x1p1009 * nm_fma(tmp, scale, scale);
        return nm_eval(y);
    }
    sbits += 1022ULL << 52;
    scale = nm_asdbl(sbits);             /* signed scale */
    t = tmp * scale;
    y = scale + t;
    if (nm_fabs(y) < 1.0) {
        one = (y < 0.0) ? -1.0 : 1.0;
        lo = t + (scale - y);
        hi = y + one;
        lo = lo + (y + (one - hi));
        y = (hi + lo) - one;
        if (y == 0.0) y = nm_asdbl(sbits & 0x8000000000000000ULL);
    }
    return nm_eval(y * 0x1p-1022);
}

#define SIGN_BIAS (0x800 << 7)

/* sign * exp(x + xtail) */
static double pow_exp(double x, double xtail, nm_u32 sign_bias)
{
    nm_u32 abstop = (nm_u32)(nm_asu64(x) >> 52) & 0x7ff;
    nm_u64 ki, idx, top, sbits;
    double kd, r, r2, tail, tmp, scale, one;

    if (abstop - 0x3c9 >= 0x408 - 0x3c9) {
        if ((int)(abstop - 0x3c9) < 0) {
            one = 1.0 + x;
            return sign_bias ? -one : one;
        }
        if (abstop >= 0x409)
            return nm_xflow((int)sign_bias, !(nm_asu64(x) >> 63));
        abstop = 0;
    }
    kd = nm_fma(x, exp_invln2N, exp_shift);
    ki = nm_asu64(kd);
    kd -= exp_shift;
    r = nm_fma(kd, exp_negln2hiN, x);
    r = nm_fma(kd, exp_negln2loN, r);
    r = xtail + r;
    idx = 2 * (ki % EXP_N);
    top = (ki + sign_bias) << (52 - 7);
    tail = nm_asdbl(exp_tab[idx]);
    sbits = exp_tab[idx + 1] + top;
    r2 = r * r;
    tmp = nm_fma(r2, nm_fma(r, exp_C3, exp_C2), r + tail);
    tmp = nm_fma(r2 * r2, nm_fma(r, exp_C5, exp_C4), tmp);
    if (abstop == 0)
        return pow_special(tmp, sbits, ki);
    scale = nm_asdbl(sbits);
    return nm_fma(tmp, scale, scale);
}

/* 0: not an integer, 1: odd integer, 2: even integer (finite nonzero iy) */
static int pow_checkint(nm_u64 iy)
{
    int e = (int)(iy >> 52 & 0x7ff);
    if (e < 0x3ff) return 0;
    if (e > 0x3ff + 52) return 2;
    if (iy & ((1ULL << (0x3ff + 52 - e)) - 1)) return 0;
    if (iy & (1ULL << (0x3ff + 52 - e))) return 1;
    return 2;
}
static int pow_zeroinfnan(nm_u64 i) { return 2 * i - 1 >= 2 * 0x7ff0000000000000ULL - 1; }

double NM_FN(pow)(double x, double y)
{
    nm_u32 sign_bias = 0, topx, topy;
    nm_u64 ix = nm_asu64(x), iy = nm_asu64(y);
    double lo, hi, ehi, elo;

    topx = (nm_u32)(ix >> 52);
    topy = (nm_u32)(iy >> 52);
    if (topx - 0x001 >= 0x7ff - 0x001 || (topy & 0x7ff) - 0x3be >= 0x43e - 0x3be) {
        if (pow_zeroinfnan(iy)) {
            if (2 * iy == 0) return 1.0;
            if (ix == 0x3ff0000000000000ULL) return 1.0;
            if (2 * ix > 2 * 0x7ff0000000000000ULL || 2 * iy > 2 * 0x7ff0000000000000ULL) return x + y;
            if (2 * ix == 2 * 0x3ff0000000000000ULL) return 1.0;
            if ((2 * ix < 2 * 0x3ff0000000000000ULL) == !(iy >> 63)) return 0.0;
            return y * y;
        }
        if (pow_zeroinfnan(ix)) {
            double x2 = x * x;
            if ((ix >> 63) && pow_checkint(iy) == 1) x2 = -x2;
            return (iy >> 63) ? 1 / x2 : x2;
        }
        /* x and y are non-zero finite */
        if (ix >> 63) {
            int yint = pow_checkint(iy);
            if (yint == 0) return nm_invalid(x);
            if (yint == 1) sign_bias = SIGN_BIAS;
            ix &= 0x7fffffffffffffffULL;
            topx &= 0x7ff;
        }
        if ((topy & 0x7ff) - 0x3be >= 0x43e - 0x3be) {
            if (ix == 0x3ff0000000000000ULL) return 1.0;
            if ((topy & 0x7ff) < 0x3be)
                return ix > 0x3ff0000000000000ULL ? 1.0 + y : 1.0 - y;
            return nm_xflow(0, (ix > 0x3ff0000000000000ULL) == (topy < 0x800));
        }
        if (topx == 0) {
            /* normalize subnormal x so the exponent becomes negative */
            ix = nm_asu64(x * 0x1p52);
            ix &= 0x7fffffffffffffffULL;
            ix -= 52ULL << 52;
        }
    }
    hi = pow_log(ix, &lo);
    ehi = y * hi;
    elo = nm_fma(y, lo, nm_fma_err(y, hi, ehi));
    return pow_exp(ehi, elo, sign_bias);
}
