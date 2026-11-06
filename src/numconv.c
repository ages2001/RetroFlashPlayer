/* numconv.c - correctly rounded decimal -> double conversion.
 *
 * fl_strtod is a strtod with exact (round-half-even) results, whatever the
 * C runtime: it is the "simple decimal conversion" algorithm (as in Go's
 * strconv), a big decimal shifted by powers of two until the binary
 * mantissa can be read off.  The NT runtime's strtod uses it. */
#include "flashnt.h"
#include <string.h>

#define DEC_MAX 800

typedef struct { u8 d[DEC_MAX]; int nd, dp, neg, trunc; } Dec;

static void dec_trim(Dec *a) { while (a->nd > 0 && a->d[a->nd - 1] == 0) a->nd--; if (a->nd == 0) a->dp = 0; }

static void dec_rshift(Dec *a, unsigned k) {
    int r = 0, w = 0; u64 n = 0, mask = ((u64)1 << k) - 1;
    for (; (n >> k) == 0; r++) {
        if (r >= a->nd) {
            if (n == 0) { a->nd = 0; return; }
            while ((n >> k) == 0) { n *= 10; r++; }
            break;
        }
        n = n * 10 + a->d[r];
    }
    a->dp -= r - 1;
    for (; r < a->nd; r++) {
        u64 dig = n >> k; n &= mask;
        a->d[w++] = (u8)dig;
        n = n * 10 + a->d[r];
    }
    while (n > 0) {
        u64 dig = n >> k; n &= mask;
        if (w < DEC_MAX) a->d[w++] = (u8)dig; else if (dig > 0) a->trunc = 1;
        n *= 10;
    }
    a->nd = w;
    dec_trim(a);
}

static void dec_lshift(Dec *a, unsigned k) {
    static u8 tmp[DEC_MAX + 32];
    int r, w = DEC_MAX + 32, len, i; u64 n = 0;
    for (r = a->nd - 1; r >= 0; r--) {
        u64 quo, rem;
        n += (u64)a->d[r] << k;
        quo = n / 10; rem = n - 10 * quo;
        tmp[--w] = (u8)rem; n = quo;
    }
    while (n > 0) { u64 quo = n / 10, rem = n - 10 * quo; tmp[--w] = (u8)rem; n = quo; }
    len = DEC_MAX + 32 - w;
    a->dp += len - a->nd;
    for (i = 0; i < len && i < DEC_MAX; i++) a->d[i] = tmp[w + i];
    for (; i < len; i++) if (tmp[w + i]) a->trunc = 1;
    a->nd = len < DEC_MAX ? len : DEC_MAX;
    dec_trim(a);
}

static void dec_shift(Dec *a, int k) {
    if (a->nd == 0) return;
    if (k > 0) { while (k > 60) { dec_lshift(a, 60); k -= 60; } dec_lshift(a, (unsigned)k); }
    else if (k < 0) { while (k < -60) { dec_rshift(a, 60); k += 60; } dec_rshift(a, (unsigned)-k); }
}

static int dec_round_up(const Dec *a, int nd) {
    if (nd < 0 || nd >= a->nd) return 0;
    if (a->d[nd] == 5 && nd + 1 == a->nd) {
        if (a->trunc) return 1;
        return nd > 0 && (a->d[nd - 1] % 2) == 1;
    }
    return a->d[nd] >= 5;
}

static u64 dec_rounded_int(const Dec *a) {
    int i; u64 n = 0;
    if (a->dp > 20) return ~(u64)0;
    for (i = 0; i < a->dp && i < a->nd; i++) n = n * 10 + a->d[i];
    for (; i < a->dp; i++) n *= 10;
    if (dec_round_up(a, a->dp)) n++;
    return n;
}

static double dec_to_double(Dec *a) {
    static const int powtab[9] = { 1, 3, 6, 9, 13, 16, 19, 23, 26 };
    const int mantbits = 52, expbits = 11, bias = -1023;
    int exp = 0; u64 mant = 0, bits; union { u64 u; double d; } cv;
    if (a->nd == 0) { exp = bias; goto out; }
    if (a->dp > 310) goto overflow;
    if (a->dp < -330) { exp = bias; goto out; }
    while (a->dp > 0) { int n = a->dp >= 9 ? 27 : powtab[a->dp]; dec_shift(a, -n); exp += n; }
    while (a->dp < 0 || (a->dp == 0 && a->d[0] < 5)) { int n = -a->dp >= 9 ? 27 : powtab[-a->dp]; dec_shift(a, n); exp -= n; }
    exp--;
    if (exp < bias + 1) { int n = bias + 1 - exp; dec_shift(a, -n); exp += n; }
    if (exp - bias >= (1 << expbits) - 1) goto overflow;
    dec_shift(a, 1 + mantbits);
    mant = dec_rounded_int(a);
    if (mant == ((u64)2 << mantbits)) { mant >>= 1; exp++; if (exp - bias >= (1 << expbits) - 1) goto overflow; }
    if ((mant & ((u64)1 << mantbits)) == 0) exp = bias;
    goto out;
overflow:
    mant = 0; exp = (1 << expbits) - 1 + bias;
out:
    bits = mant & (((u64)1 << mantbits) - 1);
    bits |= (u64)((exp - bias) & ((1 << expbits) - 1)) << mantbits;
    if (a->neg) bits |= (u64)1 << 63;
    cv.u = bits;
    return cv.d;
}

static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

double fl_strtod(const char *s, char **end) {
    static Dec a;
    const char *p = s; int sawdot = 0, sawdigits = 0, ndig = 0;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') p++;
    a.nd = a.dp = a.neg = a.trunc = 0;
    if (*p == '-') { a.neg = 1; p++; } else if (*p == '+') p++;
    if (lc(p[0]) == 'i' && lc(p[1]) == 'n' && lc(p[2]) == 'f') {
        union { u64 u; double d; } cv;
        p += 3;
        if (lc(p[0]) == 'i' && lc(p[1]) == 'n' && lc(p[2]) == 'i' && lc(p[3]) == 't' && lc(p[4]) == 'y') p += 5;
        if (end) *end = (char*)p;
        cv.u = (u64)0x7FF << 52; if (a.neg) cv.u |= (u64)1 << 63;
        return cv.d;
    }
    if (lc(p[0]) == 'n' && lc(p[1]) == 'a' && lc(p[2]) == 'n') {
        union { u64 u; double d; } cv;
        if (end) *end = (char*)p + 3;
        cv.u = ((u64)0x7FF << 52) | ((u64)1 << 51);
        return cv.d;
    }
    for (;; p++) {
        int c = *p;
        if (c == '.') { if (sawdot) break; sawdot = 1; a.dp = ndig; continue; }
        if (c >= '0' && c <= '9') {
            sawdigits = 1;
            if (c == '0' && a.nd == 0) { a.dp--; continue; }   /* leading zeros */
            ndig++;
            if (a.nd < DEC_MAX) a.d[a.nd++] = (u8)(c - '0'); else if (c != '0') a.trunc = 1;
            continue;
        }
        break;
    }
    if (!sawdigits) { if (end) *end = (char*)s; return 0.0; }
    if (!sawdot) a.dp = ndig;
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1; int eneg = 0, e = 0, any = 0;
        if (*q == '-') { eneg = 1; q++; } else if (*q == '+') q++;
        while (*q >= '0' && *q <= '9') { if (e < 100000) e = e * 10 + (*q - '0'); q++; any = 1; }
        if (any) { p = q; a.dp += eneg ? -e : e; }
    }
    if (end) *end = (char*)p;
    dec_trim(&a);
    return dec_to_double(&a);
}
