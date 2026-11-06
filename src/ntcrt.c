/* ntcrt.c - a tiny C runtime for the NT 3.1 build.
 *
 * NT 3.1 ships neither msvcrt.dll nor a modern CRT, so FLASHNT links with
 * -nostdlib and provides, on top of kernel32 + x87, exactly the subset of
 * libc the engine uses: heap, mem/str functions, qsort, sprintf/fprintf (own
 * formatter incl. %g), stdio on CreateFile, strtod/strtol; <math.h> is in
 * ntmath*.c.
 * Only compiled into the Win32 target (plat_win32.c pulls it in). */
#include <stdarg.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

/* ---------------------------------------------------------------- */
/* heap                                                             */

static HANDLE g_heap;
static HANDLE heap(void) { if (!g_heap) g_heap = GetProcessHeap(); return g_heap; }

void *malloc(size_t n) { return HeapAlloc(heap(), 0, n ? n : 1); }
void *calloc(size_t a, size_t b) { return HeapAlloc(heap(), HEAP_ZERO_MEMORY, (a * b) ? a * b : 1); }
void *realloc(void *p, size_t n) { if (!p) return malloc(n); if (!n) { HeapFree(heap(), 0, p); return NULL; } return HeapReAlloc(heap(), 0, p, n); }
void  free(void *p) { if (p) HeapFree(heap(), 0, p); }

/* ---------------------------------------------------------------- */
/* memory / strings                                                 */

void *memcpy(void *d, const void *s, size_t n) { unsigned char *dd = d; const unsigned char *ss = s; while (n--) *dd++ = *ss++; return d; }
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *dd = d; const unsigned char *ss = s;
    if (dd < ss) { while (n--) *dd++ = *ss++; }
    else { dd += n; ss += n; while (n--) *--dd = *--ss; }
    return d;
}
void *memset(void *d, int c, size_t n) { unsigned char *dd = d; while (n--) *dd++ = (unsigned char)c; return d; }
int   memcmp(const void *a, const void *b, size_t n) { const unsigned char *x = a, *y = b; while (n--) { if (*x != *y) return *x - *y; x++; y++; } return 0; }
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }
int   strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
int   strncmp(const char *a, const char *b, size_t n) { while (n && *a && *a == *b) { a++; b++; n--; } return n ? (unsigned char)*a - (unsigned char)*b : 0; }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) ; return r; }
char *strncpy(char *d, const char *s, size_t n) { char *r = d; while (n && *s) { *d++ = *s++; n--; } while (n--) *d++ = 0; return r; }
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
char *strncat(char *d, const char *s, size_t n) { char *e = d + strlen(d); while (n && *s) { *e++ = *s++; n--; } *e = 0; return d; }
static unsigned long g_rand_next = 1;
int rand(void) { g_rand_next = g_rand_next * 1103515245UL + 12345UL; return (int)((g_rand_next >> 16) & 0x7FFF); }
void srand(unsigned int seed) { g_rand_next = seed; }
char *strchr(const char *s, int c) { for (;; s++) { if (*s == (char)c) return (char*)s; if (!*s) return NULL; } }
char *strrchr(const char *s, int c) { const char *r = NULL; for (;; s++) { if (*s == (char)c) r = s; if (!*s) return (char*)r; } }
char *strpbrk(const char *s, const char *set) { for (; *s; s++) { const char *t; for (t = set; *t; t++) if (*s == *t) return (char*)s; } return NULL; }
char *strstr(const char *h, const char *n) { size_t nl = strlen(n); if (!nl) return (char*)h; for (; *h; h++) if (!strncmp(h, n, nl)) return (char*)h; return NULL; }
char *strtok(char *s, const char *delim) {
    static char *save;
    char *tok;
    if (!s) s = save;
    if (!s) return NULL;
    while (*s && strchr(delim, *s)) s++;
    if (!*s) { save = NULL; return NULL; }
    tok = s;
    while (*s && !strchr(delim, *s)) s++;
    if (*s) { *s = 0; save = s + 1; } else save = NULL;
    return tok;
}
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }

/* ---------------------------------------------------------------- */
/* qsort: stable top-down merge sort, as glibc's qsort (msort): the same  */
/* halving (n/2 first) and the same merge rule (cmp <= 0 takes the left   */
/* element), so equal keys keep their order and every comparator call    */
/* matches glibc's.                                                      */

static void msort_rec(unsigned char *b, size_t n, size_t sz, int (*cmp)(const void*, const void*), unsigned char *t) {
    size_t n1, n2;
    unsigned char *b1, *b2, *tp;
    if (n <= 1) return;
    n1 = n / 2; n2 = n - n1;
    b1 = b; b2 = b + n1 * sz;
    msort_rec(b1, n1, sz, cmp, t);
    msort_rec(b2, n2, sz, cmp, t);
    tp = t;
    while (n1 > 0 && n2 > 0) {
        if (cmp(b1, b2) <= 0) { memcpy(tp, b1, sz); b1 += sz; n1--; }
        else { memcpy(tp, b2, sz); b2 += sz; n2--; }
        tp += sz;
    }
    if (n1 > 0) memcpy(tp, b1, n1 * sz);
    memcpy(b, t, (n - n2) * sz);
}
void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void*, const void*)) {
    unsigned char *t, buf[1024];
    if (n <= 1 || !sz) return;
    t = (n * sz <= sizeof(buf)) ? buf : (unsigned char*)HeapAlloc(heap(), 0, n * sz);
    if (t) { msort_rec((unsigned char*)base, n, sz, cmp, t); if (t != buf) HeapFree(heap(), 0, t); return; }
    {   /* out of memory: stable insertion sort */
        unsigned char *b = (unsigned char*)base, tmp[256];
        size_t i, j, k;
        for (i = 1; i < n; i++)
            for (j = i; j > 0 && cmp(b + (j - 1) * sz, b + j * sz) > 0; j--)
                for (k = 0; k < sz; k += sizeof(tmp)) {
                    size_t c = sz - k < sizeof(tmp) ? sz - k : sizeof(tmp);
                    memcpy(tmp, b + j * sz + k, c); memcpy(b + j * sz + k, b + (j - 1) * sz + k, c); memcpy(b + (j - 1) * sz + k, tmp, c);
                }
    }
}

/* ---------------------------------------------------------------- */
/* <math.h>: ntmath*.c (bit-compatible with glibc's libm, see ntmath.c)  */

/* ---------------------------------------------------------------- */
/* number parsing                                                   */

/* Integer parsing as glibc does it on LP64 Linux, where the engine is
   tested: the value is accumulated and saturated in 64 bits, then returned
   in the 32-bit long of Win32 (callers here only keep 32 bits or less).
   Whitespace is isspace(); "0x" without a hex digit parses as "0"; with no
   digits at all *end = s; strtoul accepts a sign and negates. */
static int nt_isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static unsigned long long nt_strtou64(const char *s, char **end, int base, int *neg, int *ovf) {
    const char *p = s;
    unsigned long long v = 0, lim;
    int any = 0, d, last;
    *neg = 0; *ovf = 0;
    if (base < 0 || base == 1 || base > 36) { if (end) *end = (char*)s; return 0; }
    while (nt_isspace((unsigned char)*p)) p++;
    if (*p == '-') { *neg = 1; p++; } else if (*p == '+') p++;
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        int h = p[2];
        if ((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F')) { p += 2; base = 16; }
        else { if (end) *end = (char*)(p + 1); return 0; }   /* just the "0" */
    }
    if (base == 0) base = (*p == '0') ? 8 : 10;
    lim = ~0ULL / (unsigned)base; last = (int)(~0ULL % (unsigned)base);
    for (;; p++) {
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10;
        else break;
        if (d >= base) break;
        any = 1;
        if (v > lim || (v == lim && d > last)) *ovf = 1;
        else v = v * (unsigned)base + (unsigned)d;
    }
    if (end) *end = (char*)(any ? p : s);
    return v;
}
static long long nt_strtoll(const char *s, char **end, int base) {
    int neg, ovf;
    unsigned long long v = nt_strtou64(s, end, base, &neg, &ovf);
    if (neg) { if (ovf || v > 0x8000000000000000ULL) return (long long)0x8000000000000000ULL; return (long long)(0 - v); }
    if (ovf || v > 0x7fffffffffffffffULL) return 0x7fffffffffffffffLL;
    return (long long)v;
}
long strtol(const char *s, char **end, int base) { return (long)nt_strtoll(s, end, base); }
unsigned long strtoul(const char *s, char **end, int base) {
    int neg, ovf;
    unsigned long long v = nt_strtou64(s, end, base, &neg, &ovf);
    if (ovf) return (unsigned long)~0ULL;
    return (unsigned long)(neg ? 0 - v : v);
}
void *memchr(const void *s, int c, size_t n) { const unsigned char *p = s; while (n--) { if (*p == (unsigned char)c) return (void*)p; p++; } return NULL; }
char *getenv(const char *name) { static char buf[512]; DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf)); return (n && n < sizeof(buf)) ? buf : NULL; }
/* seconds since 1970 (GetSystemTime exists on NT 3.1) */
static long days_from_civil(int y, int m, int d) { long era; unsigned yoe, doy, doe; y -= m <= 2; era = (y >= 0 ? y : y - 399) / 400; yoe = (unsigned)(y - era * 400); doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1); doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; return era * 146097 + (long)doe - 719468; }
long _time32(long *t) { SYSTEMTIME st; long r; GetSystemTime(&st); r = days_from_civil(st.wYear, st.wMonth, st.wDay) * 86400L + st.wHour * 3600L + st.wMinute * 60L + st.wSecond; if (t) *t = r; return r; }
long time(long *t) { return _time32(t); }
long clock(void) { return (long)GetTickCount(); }   /* CLOCKS_PER_SEC is 1000 on Windows */
/* minimal sscanf: %d %u %x %s %c with width and '*' suppression, literals, whitespace */
static int nt_vsscanf(const char *s, const char *f, va_list ap) {
    int n = 0; const char *p = s;
    while (*f) {
        if (*f == ' ' || *f == '\t' || *f == '\n') { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; f++; continue; }
        if (*f != '%') { if (*p != *f) break; p++; f++; continue; }
        f++;
        { int skip = 0, width = 0; char conv;
          if (*f == '*') { skip = 1; f++; }
          while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
          if (*f == 'l' || *f == 'h') f++;
          conv = *f++;
          if (conv == '%') { if (*p != '%') break; p++; continue; }
          if (conv != 'c') while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
          if (!*p) break;
          if (conv == 'd' || conv == 'u' || conv == 'x' || conv == 'i') {
              char tmp[40]; int k = 0; char *e; long v;
              while (*p && k < 39 && (!width || k < width) && ((*p >= '0' && *p <= '9') || (k == 0 && (*p == '-' || *p == '+')) || (conv == 'x' && ((*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))))) tmp[k++] = *p++;
              tmp[k] = 0; if (!k || (k == 1 && (tmp[0] == '-' || tmp[0] == '+'))) break;
              v = strtol(tmp, &e, conv == 'x' ? 16 : 10);
              if (!skip) { *va_arg(ap, int*) = (int)v; n++; }
          } else if (conv == 's') {
              char *d = skip ? NULL : va_arg(ap, char*); int k = 0;
              while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && (!width || k < width)) { if (d) d[k] = *p; k++; p++; }
              if (d) { d[k] = 0; n++; }
          } else if (conv == 'c') {
              char *d = skip ? NULL : va_arg(ap, char*); int k, w = width ? width : 1;
              for (k = 0; k < w && *p; k++) { if (d) d[k] = *p; p++; }
              if (d) n++;
          } else break;
        }
    }
    return n;
}
int sscanf(const char *s, const char *f, ...) { va_list ap; int r; va_start(ap, f); r = nt_vsscanf(s, f, ap); va_end(ap); return r; }
int atoi(const char *s) { return (int)nt_strtoll(s, NULL, 10); }

extern double fl_strtod(const char *s, char **end);
static int hexval(int c) { return (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1; }
/* hexadecimal floating constant after "0x" (C99 / glibc), rounded to nearest even */
static double hex_strtod(const char *p, const char **endp, int neg) {
    unsigned long long m = 0, bits, mant;
    long e = 0, ex = 0;
    int any = 0, sticky = 0, d, top, sh, esign = 1;
    union { double f; unsigned long long u; } r;
    for (; (d = hexval((unsigned char)*p)) >= 0; p++) { any = 1; if (m >> 56) { sticky |= d != 0; e += 4; } else m = m * 16 + (unsigned)d; }
    if (*p == '.') {
        for (p++; (d = hexval((unsigned char)*p)) >= 0; p++) { any = 1; if (m >> 56) sticky |= d != 0; else { m = m * 16 + (unsigned)d; e -= 4; } }
    }
    if (!any) { *endp = NULL; return 0; }
    if ((*p == 'p' || *p == 'P') && ((p[1] >= '0' && p[1] <= '9') || ((p[1] == '+' || p[1] == '-') && p[2] >= '0' && p[2] <= '9'))) {
        p++;
        if (*p == '-') { esign = -1; p++; } else if (*p == '+') p++;
        for (; *p >= '0' && *p <= '9'; p++) if (ex < 100000) ex = ex * 10 + (*p - '0');
        e += esign * ex;
    }
    *endp = p;
    if (!m) { r.u = (unsigned long long)neg << 63; return r.f; }
    m = (m << 1) | (unsigned)sticky; e -= 1;          /* sticky bit below the digits */
    for (top = 63; !(m >> top); top--) ;
    if (top + e > 1023) { r.u = ((unsigned long long)neg << 63) | 0x7ff0000000000000ULL; return r.f; }
    sh = (top + e >= -1022) ? top - 52 : (int)(-1074 - e);   /* weight of the last kept bit */
    if (sh <= 0) mant = m << -sh;
    else if (sh < 64) {
        unsigned long long rem = m & ((1ULL << sh) - 1), half = 1ULL << (sh - 1);
        mant = m >> sh;
        if (rem > half || (rem == half && (mant & 1))) mant++;
    } else   /* below 2^-1074: rounds to 2^-1074 only above half of it */
        mant = (sh == 64 && m > (1ULL << 63)) ? 1 : 0;
    if (top + e >= -1022) bits = ((unsigned long long)(top + e + 1022) << 52) + mant;   /* mant in [2^52, 2^53] */
    else bits = mant;
    if (bits >= 0x7ff0000000000000ULL) bits = 0x7ff0000000000000ULL;
    r.u = bits | ((unsigned long long)neg << 63);
    return r.f;
}
/* correctly rounded (numconv.c for decimal input); also hexadecimal input
   and "nan(chars)" like glibc */
double strtod(const char *s, char **end) {
    const char *p = s, *q;
    int neg = 0;
    double r;
    char *e;
    while (nt_isspace((unsigned char)*p)) p++;
    if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        r = hex_strtod(p + 2, &q, neg);
        if (q) { if (end) *end = (char*)q; return r; }
        /* "0x" without hex digits: just the 0 */
    }
    r = fl_strtod(s, &e);
    if (r != r && *e == '(') {
        q = e + 1;
        while ((*q >= '0' && *q <= '9') || (*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || *q == '_') q++;
        if (*q == ')') e = (char*)q + 1;
    }
    if (end) *end = e;
    return r;
}
double atof(const char *s) { return strtod(s, NULL); }

/* ---------------------------------------------------------------- */
/* formatting                                                       */

typedef struct { char *buf; size_t cap, len; } Out;
static void outc(Out *o, char c) { if (o->len + 1 < o->cap) o->buf[o->len] = c; o->len++; }
static void outs(Out *o, const char *s, size_t n) { while (n--) outc(o, *s++); }

/* integer conversion as glibc prints it: sign or prefix, then zero
   padding from the precision or the 0 flag, then the digits */
static void fmt_uint(Out *o, unsigned long long v, int base, int upper, int width, int prec, int zero, const char *sign, int left) {
    char tmp[72]; int n = 0, pad, nz = 0, ns = (int)strlen(sign);
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    while (v) { tmp[n++] = digits[v % (unsigned)base]; v /= (unsigned)base; }
    if (prec < 0) { prec = 1; } else zero = 0;   /* a precision disables the 0 flag */
    if (n < prec) nz = prec - n;
    pad = width - n - nz - ns;
    if (zero && !left && pad > 0) { nz += pad; pad = 0; }
    if (!left) while (pad-- > 0) outc(o, ' ');
    outs(o, sign, (size_t)ns);
    while (nz-- > 0) outc(o, '0');
    while (n) outc(o, tmp[--n]);
    if (left) while (pad-- > 0) outc(o, ' ');
}

/* Exact decimal expansion of a finite double (like glibc): v = m * 2^e2 is
   turned into a big integer (base 1e9 limbs) and printed in full, so %e/%f/%g
   with any precision round the true value (round-half-even on exact ties). */
#define XD_LIMBS 130                         /* 5^1074 * 2^53 < 1e1170: 130 limbs of 9 digits */
static int xd_digits(double v, char *d, int cap, int *point) {
    /* d: all digits of the integer N, *point: N's digits stand for N * 10^-(*point) */
    unsigned int limb[XD_LIMBS]; int nl = 1, i, k, nd = 0, e2; double fr; unsigned long long m;
    { int ex; fr = frexp(v, &ex); m = (unsigned long long)ldexp(fr, 53); e2 = ex - 53; }
    while (m && !(m & 1)) { m >>= 1; e2++; }
    limb[0] = (unsigned int)(m % 1000000000u); limb[1] = (unsigned int)((m / 1000000000u) % 1000000000u); limb[2] = (unsigned int)(m / 1000000000000000000ull);
    nl = limb[2] ? 3 : limb[1] ? 2 : 1;
    *point = 0;
    if (e2 > 0) {                            /* N = m * 2^e2 */
        for (k = 0; k < e2; k++) { unsigned int c = 0; for (i = 0; i < nl; i++) { unsigned int t = limb[i] * 2u + c; c = t >= 1000000000u; limb[i] = c ? t - 1000000000u : t; } if (c && nl < XD_LIMBS) limb[nl++] = 1; }
    } else if (e2 < 0) {                     /* m / 2^k = m * 5^k / 10^k */
        for (k = 0; k < -e2; k++) { unsigned long long c = 0; for (i = 0; i < nl; i++) { unsigned long long t = (unsigned long long)limb[i] * 5u + c; limb[i] = (unsigned int)(t % 1000000000u); c = t / 1000000000u; } if (c && nl < XD_LIMBS) limb[nl++] = (unsigned int)c; }
        *point = -e2;
    }
    { char t[10]; int n0;
      n0 = 0; { unsigned int x = limb[nl - 1]; char r[10]; int q = 0; do { r[q++] = (char)('0' + x % 10); x /= 10; } while (x); while (q) t[n0++] = r[--q]; }
      for (i = 0; i < n0 && nd < cap; i++) d[nd++] = t[i];
      for (k = nl - 2; k >= 0; k--) { unsigned int x = limb[k]; for (i = 8; i >= 0; i--) { t[i] = (char)('0' + x % 10); x /= 10; } for (i = 0; i < 9 && nd < cap; i++) d[nd++] = t[i]; } }
    return nd;
}
/* round the digit string d[0..nd) (value = 0.d * 10^dexp after normalising) to keep `keep` digits;
   returns the new digit count, may bump *dexp on carry-out */
static int xd_round(char *d, int nd, int keep, int *dexp) {
    int i, up;
    if (keep >= nd) return nd;
    if (keep < 0) { return 0; }
    up = 0;
    if (d[keep] > '5') up = 1;
    else if (d[keep] == '5') { for (i = keep + 1; i < nd; i++) if (d[i] != '0') { up = 1; break; } if (!up) up = keep > 0 ? ((d[keep - 1] - '0') & 1) : 0; }
    nd = keep;
    if (up) {
        for (i = nd - 1; i >= 0; i--) { if (d[i] == '9') d[i] = '0'; else { d[i]++; break; } }
        if (i < 0) { if (nd > 0) { memmove(d + 1, d, (size_t)nd); } d[0] = '1'; nd++; (*dexp)++; }
    }
    return nd;
}
#define XD_CAP 1200
static int g_fmt_upper;   /* %E / %G / %F */
static int g_fmt_zero;    /* 0 flag for a double conversion */
static void fmt_double(Out *o, double v, char conv, int prec, int width, int left, char sgnc) {
    static char d[XD_CAP + 2], body[XD_CAP + 400]; int n = 0, i, nd, point, dexp, X;
    union { double f; unsigned long long u; } bits;
    int neg;
    bits.f = v;
    neg = (int)(bits.u >> 63);                       /* sign bit, as glibc (also "-nan") */
    if ((bits.u & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL) {   /* inf, nan: no zero padding */
        const char *t = (bits.u & 0x000fffffffffffffULL) ? (g_fmt_upper ? "NAN" : "nan") : (g_fmt_upper ? "INF" : "inf");
        int pad = width - 3 - (neg || sgnc);
        if (!left) while (pad-- > 0) outc(o, ' ');
        if (neg) outc(o, '-'); else if (sgnc) outc(o, sgnc);
        outs(o, t, 3);
        if (left) while (pad-- > 0) outc(o, ' ');
        return;
    }
    if (neg) v = -v;
    if (prec < 0) prec = 6;
    if (prec > 1100) prec = 1100;
    if (v == 0) { nd = 1; d[0] = '0'; dexp = 1; }
    else {
        nd = xd_digits(v, d, XD_CAP, &point);
        { int z = 0; while (z < nd - 1 && d[z] == '0') z++; if (z) { memmove(d, d + z, (size_t)(nd - z)); nd -= z; } }
        dexp = nd - point;                  /* value = 0.d1d2... * 10^dexp */
        while (nd > 1 && d[nd - 1] == '0') nd--;
    }
    if (conv == 'g') {
        int P = prec ? prec : 1; int dd = dexp; char t[XD_CAP + 2]; int tn;
        memcpy(t, d, (size_t)nd); tn = xd_round(t, nd, P, &dd);
        if (v == 0) dd = 1;
        X = dd - 1;
        if (P > X && X >= -4) { conv = 'F'; prec = P - 1 - X; } else { conv = 'G'; prec = P - 1; }
    }
    if (conv == 'f' || conv == 'F') {
        int keep = dexp + prec; char strip = conv == 'F';
        if (v != 0) nd = xd_round(d, nd, keep, &dexp);
        if (v != 0 && keep < 0) { nd = 0; }
        if (dexp <= 0) body[n++] = '0'; else for (i = 0; i < dexp; i++) body[n++] = i < nd ? d[i] : '0';
        if (prec > 0) {
            int start = n; body[n++] = '.';
            for (i = 0; i < prec; i++) { int k = dexp + i; body[n++] = (v != 0 && k >= 0 && k < nd) ? d[k] : '0'; }
            if (strip) { while (n > start + 1 && body[n - 1] == '0') n--; if (body[n - 1] == '.') n--; }
        }
    } else {                                 /* 'e' or 'G' (scientific) */
        char strip = conv == 'G'; int ex;
        if (v != 0) nd = xd_round(d, nd, prec + 1, &dexp);
        ex = v == 0 ? 0 : dexp - 1;
        body[n++] = d[0];
        if (prec > 0) {
            int start = n; body[n++] = '.';
            for (i = 1; i <= prec; i++) body[n++] = i < nd ? d[i] : '0';
            if (strip) { while (n > start + 1 && body[n - 1] == '0') n--; if (body[n - 1] == '.') n--; }
        }
        body[n++] = g_fmt_upper ? 'E' : 'e'; body[n++] = ex < 0 ? '-' : '+';
        { int ae = ex < 0 ? -ex : ex; if (ae >= 100) body[n++] = (char)('0' + ae / 100); body[n++] = (char)('0' + (ae / 10) % 10); body[n++] = (char)('0' + ae % 10); }
    }
    {
        int pad = width - n - (neg || sgnc);
        if (!left && !g_fmt_zero) while (pad-- > 0) outc(o, ' ');
        if (neg) outc(o, '-'); else if (sgnc) outc(o, sgnc);
        if (!left && g_fmt_zero) while (pad-- > 0) outc(o, '0');
        outs(o, body, (size_t)n);
        if (left) while (pad-- > 0) outc(o, ' ');
    }
}

static int vformat(Out *o, const char *f, va_list ap) {
    while (*f) {
        int left = 0, zero = 0, width = 0, prec = -1, lng = 0, plus = 0, space = 0, alt = 0;
        char sg[4];
        if (*f != '%') { outc(o, *f++); continue; }
        f++;
        for (;;) { if (*f == '-') left = 1; else if (*f == '0') zero = 1; else if (*f == '+') plus = 1; else if (*f == ' ') space = 1; else if (*f == '#') alt = 1; else break; f++; }
        if (*f == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } f++; } else while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == '.') { f++; prec = 0; if (*f == '*') { prec = va_arg(ap, int); f++; } else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0'); }
        while (*f == 'l' || *f == 'h' || *f == 'z' || *f == 'L') { if (*f == 'l') lng++; f++; }
        if (left) zero = 0;
        sg[0] = 0;
        switch (*f) {
        case 'd': case 'i': {
            long long v = lng >= 2 ? va_arg(ap, long long) : lng ? (long long)va_arg(ap, long) : (long long)va_arg(ap, int);
            if (v < 0) strcpy(sg, "-"); else if (plus) strcpy(sg, "+"); else if (space) strcpy(sg, " ");
            fmt_uint(o, v < 0 ? 0 - (unsigned long long)v : (unsigned long long)v, 10, 0, width, prec, zero, sg, left); break; }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long v = lng >= 2 ? va_arg(ap, unsigned long long) : lng ? (unsigned long long)va_arg(ap, unsigned long) : (unsigned long long)va_arg(ap, unsigned);
            int base = *f == 'u' ? 10 : *f == 'o' ? 8 : 16;
            if (alt && v && base == 16) strcpy(sg, *f == 'X' ? "0X" : "0x");
            if (alt && base == 8 && v) strcpy(sg, "0");
            fmt_uint(o, v, base, *f == 'X', width, prec, zero, sg, left); break; }
        case 'c': { int pad = width - 1; if (!left) while (pad-- > 0) outc(o, ' '); outc(o, (char)va_arg(ap, int)); if (left) while (pad-- > 0) outc(o, ' '); break; }
        case 's': { const char *s = va_arg(ap, const char*); size_t n; if (!s) s = "(null)"; n = strlen(s); if (prec >= 0 && (size_t)prec < n) n = (size_t)prec;
                    if (!left) { int pad = width - (int)n; while (pad-- > 0) outc(o, ' '); } outs(o, s, n); if (left) { int pad = width - (int)n; while (pad-- > 0) outc(o, ' '); } break; }
        case 'f': case 'F': case 'g': case 'G': case 'e': case 'E': {
            char c = *f, sgnc = plus ? '+' : space ? ' ' : 0;
            g_fmt_upper = c == 'G' || c == 'E' || c == 'F'; g_fmt_zero = zero;
            fmt_double(o, va_arg(ap, double), c == 'F' ? 'f' : c == 'G' ? 'g' : c == 'E' ? 'e' : c, prec, width, left, sgnc);
            g_fmt_upper = 0; g_fmt_zero = 0; break; }
        case 'p': { void *v = va_arg(ap, void*); if (!v) { const char *s = "(nil)"; int pad = width - 5; if (!left) while (pad-- > 0) outc(o, ' '); outs(o, s, 5); if (left) while (pad-- > 0) outc(o, ' '); }
                    else fmt_uint(o, (unsigned long long)(size_t)v, 16, 0, width, prec, zero, "0x", left); break; }
        case '%': outc(o, '%'); break;
        default: outc(o, '%'); outc(o, *f); break;
        }
        f++;
    }
    if (o->cap) o->buf[o->len < o->cap ? o->len : o->cap - 1] = 0;
    return (int)o->len;
}

/* mingw's inline snprintf/vsnprintf wrappers call these */
int __mingw_vsnprintf(char *buf, size_t cap, const char *f, va_list ap) { Out o; o.buf = buf; o.cap = cap; o.len = 0; return vformat(&o, f, ap); }
int __mingw_snprintf(char *buf, size_t cap, const char *f, ...) { va_list ap; int r; va_start(ap, f); r = __mingw_vsnprintf(buf, cap, f, ap); va_end(ap); return r; }
int __mingw_vsprintf(char *buf, const char *f, va_list ap) { return __mingw_vsnprintf(buf, 1u << 30, f, ap); }
int sprintf(char *buf, const char *f, ...) { va_list ap; int r; va_start(ap, f); r = __mingw_vsnprintf(buf, 1u << 30, f, ap); va_end(ap); return r; }
int vsprintf(char *buf, const char *f, va_list ap) { return __mingw_vsnprintf(buf, 1u << 30, f, ap); }
#define vsnprintf __mingw_vsnprintf

/* ---------------------------------------------------------------- */
/* stdio on CreateFile                                              */

typedef struct NTFile { HANDLE h; int eof; int err; int is_log; } NTFile;

static NTFile nt_stdout = { INVALID_HANDLE_VALUE, 0, 0, 1 };
static NTFile nt_stderr = { INVALID_HANDLE_VALUE, 0, 0, 1 };
static HANDLE g_log = INVALID_HANDLE_VALUE;

/* mingw's <stdio.h> resolves stderr/stdout to __acrt_iob_func(n) */
FILE *__acrt_iob_func(unsigned i) { return (FILE*)(i == 2 ? &nt_stderr : &nt_stdout); }

static void log_write(const char *s, size_t n) {
    DWORD w;
    if (g_log == INVALID_HANDLE_VALUE) g_log = CreateFileA("retrofp.log", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_log != INVALID_HANDLE_VALUE) WriteFile(g_log, s, (DWORD)n, &w, NULL);
}

static NTFile *nf(FILE *f) { return (NTFile*)f; }

FILE *fopen(const char *path, const char *mode) {
    NTFile *f; HANDLE h; int wr = (mode[0] == 'w' || mode[0] == 'a');
    h = CreateFileA(path, wr ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ, NULL,
                    mode[0] == 'w' ? CREATE_ALWAYS : (mode[0] == 'a' ? OPEN_ALWAYS : OPEN_EXISTING), FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (mode[0] == 'a') SetFilePointer(h, 0, NULL, FILE_END);
    f = (NTFile*)calloc(1, sizeof(NTFile));
    if (!f) { CloseHandle(h); return NULL; }
    f->h = h;
    return (FILE*)f;
}
int fclose(FILE *fp) { NTFile *f = nf(fp); if (f->is_log) return 0; CloseHandle(f->h); free(f); return 0; }
size_t fread(void *buf, size_t sz, size_t n, FILE *fp) { NTFile *f = nf(fp); DWORD got = 0; if (f->is_log || !sz) return 0; if (!ReadFile(f->h, buf, (DWORD)(sz * n), &got, NULL)) { f->err = 1; return 0; } if (got < sz * n) f->eof = 1; return got / sz; }
size_t fwrite(const void *buf, size_t sz, size_t n, FILE *fp) { NTFile *f = nf(fp); DWORD w = 0; if (f->is_log) { log_write((const char*)buf, sz * n); return n; } WriteFile(f->h, buf, (DWORD)(sz * n), &w, NULL); return w / (sz ? sz : 1); }
int fseek(FILE *fp, long off, int whence) { NTFile *f = nf(fp); if (f->is_log) return -1; f->eof = 0; return SetFilePointer(f->h, off, NULL, whence == SEEK_SET ? FILE_BEGIN : (whence == SEEK_CUR ? FILE_CURRENT : FILE_END)) == 0xFFFFFFFF ? -1 : 0; }
long ftell(FILE *fp) { NTFile *f = nf(fp); if (f->is_log) return -1; return (long)SetFilePointer(f->h, 0, NULL, FILE_CURRENT); }
int fputc(int c, FILE *fp) { char ch = (char)c; fwrite(&ch, 1, 1, fp); return c; }
int fputs(const char *s, FILE *fp) { fwrite(s, 1, strlen(s), fp); return 0; }
int feof(FILE *fp) { return nf(fp)->eof; }
int fflush(FILE *fp) { (void)fp; return 0; }
char *fgets(char *buf, int n, FILE *fp) {
    int i = 0; char c;
    while (i < n - 1) { if (fread(&c, 1, 1, fp) != 1) break; buf[i++] = c; if (c == '\n') break; }
    if (i == 0) return NULL;
    buf[i] = 0; return buf;
}
int vfprintf(FILE *fp, const char *f, va_list ap) { char buf[2048]; int n = vsnprintf(buf, sizeof(buf), f, ap); if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1; fwrite(buf, 1, (size_t)n, fp); return n; }
int fprintf(FILE *fp, const char *f, ...) { va_list ap; int r; va_start(ap, f); r = vfprintf(fp, f, ap); va_end(ap); return r; }
int printf(const char *f, ...) { va_list ap; int r; va_start(ap, f); r = vfprintf((FILE*)&nt_stdout, f, ap); va_end(ap); return r; }
int puts(const char *s) { fputs(s, (FILE*)&nt_stdout); fputc('\n', (FILE*)&nt_stdout); return 0; }

void exit(int code) { ExitProcess((UINT)code); }
void abort(void) {   /* leaves a trace in retrofp.log: who aborted */
    char b[64]; unsigned long ra = (unsigned long)__builtin_return_address(0); int i; const char *hx = "0123456789ABCDEF";
    memcpy(b, "abort() called from 0x", 22); for (i = 0; i < 8; i++) b[22 + i] = hx[(ra >> (28 - 4 * i)) & 15]; b[30] = '\r'; b[31] = '\n';
    log_write(b, 32);
    ExitProcess(3);
}
/* unhandled exceptions (access violation, stack overflow, FPU) are written to retrofp.log
   (code and address; build/obj/flashnt.map maps the address to a function) */
static LONG WINAPI nt_crash_filter(EXCEPTION_POINTERS *ep) {
    char b[96]; unsigned long c = ep->ExceptionRecord->ExceptionCode, a = (unsigned long)ep->ExceptionRecord->ExceptionAddress; int i, n; const char *hx = "0123456789ABCDEF";
    memcpy(b, "crash: exception 0x", 19); n = 19;
    for (i = 0; i < 8; i++) b[n++] = hx[(c >> (28 - 4 * i)) & 15];
    memcpy(b + n, " at 0x", 6); n += 6;
    for (i = 0; i < 8; i++) b[n++] = hx[(a >> (28 - 4 * i)) & 15];
    b[n++] = '\r'; b[n++] = '\n';
    log_write(b, (size_t)n);
    return EXCEPTION_CONTINUE_SEARCH;
}
void nt_install_crash_log(void) { SetUnhandledExceptionFilter(nt_crash_filter); }

int remove(const char *path) { return DeleteFileA(path) ? 0 : -1; }
