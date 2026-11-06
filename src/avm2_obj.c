/* avm2_obj.c - AVM2 values, strings, objects, traits, classes, GC */
#include "avm2_int.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ---------------------------------------------------------------- */
/* allocation                                                       */

static void *gcalloc(VM2 *vm, size_t sz, int kind) {
    A2GC *h = (A2GC*)calloc(1, sz);
    if (!h) return NULL;
    h->kind = (u8)kind; h->next = vm->gc_list; vm->gc_list = h; vm->gc_count++;
    return h;
}

/* ---------------------------------------------------------------- */
/* strings (interned)                                               */

static u32 hash_bytes(const char *s, size_t n) { u32 h = 2166136261u; size_t i; for (i = 0; i < n; i++) { h ^= (u8)s[i]; h *= 16777619u; } return h; }

A2Str *a2_intern(VM2 *vm, const char *s, size_t len) {
    u32 h = hash_bytes(s, len); int b;
    A2Str *p;
    if (!vm->intern) { vm->capintern = 4096; vm->intern = (A2Str**)calloc((size_t)vm->capintern, sizeof(A2Str*)); }
    b = (int)(h % (u32)vm->capintern);
    for (p = vm->intern[b]; p; p = p->inext) if (p->hash == h && p->len == len && memcmp(p->s, s, len) == 0) return p;
    p = (A2Str*)gcalloc(vm, sizeof(A2Str), G_STR);
    if (!p) return NULL;
    p->s = (char*)malloc(len + 1); memcpy(p->s, s, len); p->s[len] = 0;
    p->len = (u32)len; p->hash = h;
    p->inext = vm->intern[b]; vm->intern[b] = p; vm->nintern++;
    return p;
}
A2Str *a2_str(VM2 *vm, const char *s) { return a2_intern(vm, s, strlen(s)); }
void *a2_gc_alloc_str(VM2 *vm) { return gcalloc(vm, sizeof(A2Str), G_STR); }
/* a string built outside the table (a2_sb_*): the equal interned string if there is one,
   else r itself, now interned */
A2Str *a2_intern_adopt(VM2 *vm, A2Str *r) {
    u32 h = hash_bytes(r->s, r->len); int b; A2Str *p;
    if (!vm->intern) { vm->capintern = 4096; vm->intern = (A2Str**)calloc((size_t)vm->capintern, sizeof(A2Str*)); }
    b = (int)(h % (u32)vm->capintern);
    for (p = vm->intern[b]; p; p = p->inext) if (p->hash == h && p->len == r->len && memcmp(p->s, r->s, r->len) == 0) return p;
    r->hash = h; r->inext = vm->intern[b]; vm->intern[b] = r; vm->nintern++;
    return r;
}
A2Str *a2_concat(VM2 *vm, A2Str *a, A2Str *b) {
    char *buf; A2Str *r;
    if (a->len == 0) return b;
    if (b->len == 0) return a;
    buf = (char*)malloc((size_t)a->len + b->len + 1);
    if (!buf) return a;
    {   /* a lone high surrogate meeting a lone low one (WTF-8) makes one character */
        const u8 *x = (const u8*)a->s + a->len - 3, *y = (const u8*)b->s;
        if (a->len >= 3 && b->len >= 3 && x[0] == 0xED && x[1] >= 0xA0 && x[1] <= 0xAF && y[0] == 0xED && y[1] >= 0xB0 && y[1] <= 0xBF) {
            u32 hi = 0xD000 | ((u32)(x[1] & 0x3F) << 6) | (x[2] & 0x3F), lo = 0xD000 | ((u32)(y[1] & 0x3F) << 6) | (y[2] & 0x3F);
            u32 cp = 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00); size_t n = a->len - 3;
            memcpy(buf, a->s, n);
            buf[n++] = (char)(0xF0 | (cp >> 18)); buf[n++] = (char)(0x80 | ((cp >> 12) & 63)); buf[n++] = (char)(0x80 | ((cp >> 6) & 63)); buf[n++] = (char)(0x80 | (cp & 63));
            memcpy(buf + n, b->s + 3, b->len - 3); n += b->len - 3; buf[n] = 0;
            r = a2_intern(vm, buf, n); free(buf); return r;
        }
    }
    memcpy(buf, a->s, a->len); memcpy(buf + a->len, b->s, b->len); buf[a->len + b->len] = 0;
    r = a2_intern(vm, buf, (size_t)a->len + b->len);
    free(buf);
    return r;
}

/* ---------------------------------------------------------------- */
/* values                                                           */

V2 v2_undef(void) { V2 v; v.t = T_UNDEF; v.u.n = 0; return v; }
V2 v2_hole(void) { V2 v; v.t = T_HOLE; v.u.n = 0; return v; }
V2 v2_null(void) { V2 v; v.t = T_NULL; v.u.n = 0; return v; }
V2 v2_bool(int b) { V2 v; v.t = T_BOOL; v.u.b = !!b; return v; }
V2 v2_int(s32 i) { V2 v; v.t = T_INT; v.u.i = i; return v; }
V2 v2_uint(u32 u) { V2 v; v.t = T_UINT; v.u.u = u; return v; }
V2 v2_num(double d) { V2 v; v.t = T_NUM; v.u.n = d; return v; }
V2 v2_str(A2Str *s) { V2 v; if (!s) return v2_null(); v.t = T_STR; v.u.s = s; return v; }
V2 v2_obj(A2Obj *o) { V2 v; if (!o) return v2_null(); v.t = T_OBJ; v.u.o = o; return v; }
V2 v2_cstr(VM2 *vm, const char *s) { return v2_str(a2_str(vm, s)); }
V2 a2_arg(int argc, V2 *argv, int i) { return i < argc ? argv[i] : v2_undef(); }
int v2_is_num(V2 v) { return v.t == T_INT || v.t == T_UINT || v.t == T_NUM; }

/* ---- Number formatting: a port of avmplus MathUtils::convertDoubleToString ----
 * modes: 0 NORMAL (toString), 1 FIXED, 2 PRECISION, 3 EXPONENTIAL.  Digits come from a
 * generator: the shortest round-trip expansion for NORMAL, the exact expansion otherwise. */
typedef struct { char d[420]; int n, pos, finished; } DGen;
static int dg_next(DGen *g) { if (g->pos >= g->n) { g->finished = 1; return -1; } { int c = g->d[g->pos++] - '0'; if (g->pos >= g->n) g->finished = 1; return c; } }
/* fills g with the significant digits of v (> 0) and returns exp10 = floor(log10(v)) */
/* |cand - v| <= half, evaluated in decimal with ~70 digits (denormal formatting) */
#define DW 80
static void dec_load(const char *sci, int top, signed char *out) {
    /* sci = "d.ddddde[+-]x": place digits into out[k] = coefficient of 10^(top-k) */
    char buf[520]; char *e; int ex, i, k = 0, pos; memset(out, 0, DW);
    strncpy(buf, sci, 519); buf[519] = 0; e = strchr(buf, 'e'); if (!e) return; ex = atoi(e + 1); *e = 0;
    pos = top - ex;   /* index of the first digit */
    for (i = 0; buf[i]; i++) if (buf[i] >= '0' && buf[i] <= '9') { if (pos + k >= 0 && pos + k < DW) out[pos + k] = (signed char)(buf[i] - '0'); k++; }
}
static int dec_cmp(const signed char *a, const signed char *b) { int i; for (i = 0; i < DW; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1; return 0; }
static void dec_sub(const signed char *a, const signed char *b, signed char *r) { int i, borrow = 0; for (i = DW - 1; i >= 0; i--) { int d = a[i] - b[i] - borrow; if (d < 0) { d += 10; borrow = 1; } else borrow = 0; r[i] = (signed char)d; } }
/* decimal form of 2^-k as a sci string ("d.ddd...e-N"), for k up to 1100: 2^-k = 5^k / 10^k */
static void pow2neg_sci(int k, char *out) {
    static unsigned char dig[800]; int n = 1, i, j, carry;
    dig[0] = 1;
    for (i = 0; i < k; i++) { carry = 0; for (j = 0; j < n; j++) { int t = dig[j] * 5 + carry; dig[j] = (unsigned char)(t % 10); carry = t / 10; } while (carry) { dig[n++] = (unsigned char)(carry % 10); carry /= 10; } }
    /* dig[] holds 5^k little-endian, n digits; value = 5^k * 10^-k */
    out[0] = (char)('0' + dig[n - 1]); out[1] = '.';
    for (i = 0, j = 2; i < 72 && n - 2 - i >= 0; i++) out[j++] = (char)('0' + dig[n - 2 - i]);
    sprintf(out + j, "e%d", n - 1 - k);
}
static int within_half(const char *cand, double v, int halfexp, int pow2) {
    char vs[520], hs[520]; signed char C[DW], V[DW], H[DW], D[DW]; int top, below; char *e;
    sprintf(vs, "%.70e", v); e = strchr(vs, 'e'); top = atoi(e + 1) + 1;
    dec_load(cand, top, C); dec_load(vs, top, V);
    below = dec_cmp(C, V) < 0;
    pow2neg_sci(-(halfexp - (pow2 && below ? 1 : 0)), hs);   /* a power of two has a narrower interval below */
    dec_load(hs, top, H);
    if (!below) dec_sub(C, V, D); else dec_sub(V, C, D);
    return dec_cmp(D, H) <= 0;
}
static int dg_init(DGen *g, double v, int shortest) {
    char tmp[480]; int exp10, i; char *e;
    if (shortest) {
        /* free-format digit generation (Burger & Dybvig as in avmplus): the shortest digit string
         * inside the rounding interval, ties resolved upward; denormals are treated as if they
         * carried a full 53-bit significand (so they print with more digits than necessary) */
        char ex[480]; int exd[480]; int nex, prec, ee; int halfexp = 0, pow2 = 0; int denormal = v < 2.2250738585072014e-308;
        sprintf(ex, "%.*e", 60, v); e = strchr(ex, 'e'); ee = atoi(e + 1); *e = 0;
        nex = 0; for (i = 0; ex[i]; i++) if (ex[i] >= '0' && ex[i] <= '9') exd[nex++] = ex[i] - '0';
        { int bexp; double f = frexp(v, &bexp); pow2 = (f == 0.5); halfexp = bexp - 54; }
        for (prec = 1; prec <= 17; prec++) {
            int k, carry, up; double x;
            /* candidate: first prec digits, rounded (half rounds up) */
            for (k = 0; k < prec; k++) g->d[k] = (char)('0' + exd[k]);
            up = 0;
            if (exd[prec] > 5) up = 1;
            else if (exd[prec] == 5) up = 1;    /* exact tie or above: round up */
            if (exd[prec] == 5) { int allz = 1; for (k = prec + 1; k < nex; k++) if (exd[k]) { allz = 0; break; } (void)allz; }
            exp10 = ee;
            if (up) { carry = 1; for (k = prec - 1; k >= 0 && carry; k--) { if (g->d[k] == '9') g->d[k] = '0'; else { g->d[k]++; carry = 0; } } if (carry) { memmove(g->d + 1, g->d, (size_t)prec); g->d[0] = '1'; exp10++; } }
            g->d[prec] = 0;
            sprintf(tmp, "%c.%se%d", g->d[0], prec > 1 ? g->d + 1 : "", exp10);
            x = strtod(tmp, NULL);
            if (denormal ? within_half(tmp, v, halfexp, pow2) : x == v) break;
            /* also try the truncated (lower) candidate when the rounded one overshoots */
            if (up) {
                for (k = 0; k < prec; k++) g->d[k] = (char)('0' + exd[k]); g->d[prec] = 0; exp10 = ee;
                sprintf(tmp, "%c.%se%d", g->d[0], prec > 1 ? g->d + 1 : "", exp10);
                x = strtod(tmp, NULL);
                if (denormal ? within_half(tmp, v, halfexp, pow2) : x == v) break;
            }
        }
        if (prec > 17) { sprintf(tmp, "%.16e", v); e = strchr(tmp, 'e'); exp10 = atoi(e + 1); *e = 0; g->n = 0; for (i = 0; tmp[i]; i++) if (tmp[i] >= '0' && tmp[i] <= '9' && g->n < 418) g->d[g->n++] = tmp[i]; }
        else g->n = prec;
        while (g->n > 1 && g->d[g->n - 1] == '0') g->n--;
        g->pos = 0; g->finished = (g->n == 0);
        return exp10;
    }
    sprintf(tmp, "%.*e", 400, v);
    e = strchr(tmp, 'e'); exp10 = atoi(e + 1); *e = 0;
    g->n = 0; for (i = 0; tmp[i]; i++) if (tmp[i] >= '0' && tmp[i] <= '9' && g->n < 418) g->d[g->n++] = tmp[i];
    while (g->n > 1 && g->d[g->n - 1] == '0') g->n--;
    g->pos = 0; g->finished = (g->n == 0);
    return exp10;
}
char *a2_format_number(double value, int mode, int precision, char *buffer) {
    DGen gen, *d2a = &gen; int exp10, negative, zero, noFraction, round = 1, wroteDecimal = 0, format; char *s = buffer, *sentinel;
    enum { kNormal, kExponential, kFraction, kFixedFraction };
    if (value != value) { strcpy(buffer, "NaN"); return buffer; }
    if (value > 1.7976931348623157e308) { strcpy(buffer, "Infinity"); return buffer; }
    if (value < -1.7976931348623157e308) { strcpy(buffer, "-Infinity"); return buffer; }
    if (mode == 0) { int iv = (value >= -2147483648.0 && value <= 2147483647.0) ? (int)value : 0; if (value == (double)iv && !(value == 0 && 1 / value < 0 && 0)) { sprintf(buffer, "%d", iv); return buffer; } if (value == 0) { strcpy(buffer, "0"); return buffer; } }
    negative = value < 0; zero = value == 0; noFraction = (precision == 0);
    if (negative) { value = -value; s++; }
    if (zero) { gen.n = 1; gen.d[0] = '0'; gen.pos = 0; gen.finished = 0; exp10 = 0; }
    else exp10 = dg_init(&gen, value, mode == 0);
    sentinel = s;
    switch (mode) {
    case 1: if (exp10 < 0) format = kFixedFraction; else { format = kNormal; precision++; } break;
    case 2: if (exp10 < 0) format = kFraction; else if (exp10 >= precision) format = kExponential; else format = kNormal; break;
    case 3: format = kExponential; precision++; break;
    default:
        if (exp10 < 0 && exp10 > -7) { if (exp10 < -precision) exp10 = -precision - 1; format = kFraction; }
        else if (exp10 > 20) {
            format = kExponential;
            /* avmplus digit-generator bug: exact powers of two print as 1e+(exp10+1) */
            { int ex; double f = frexp(value, &ex); if (f == 0.5 && value >= 4.49423283715578e307) { sprintf(buffer, "%s1e+%d", negative ? "-" : "", exp10 + 1); return buffer; } }   /* value*4 overflows in the digit generator */
        }
        else format = kNormal;
    }
    switch (format) {
    case kNormal: {
        int digits = 0, digit;
        *s++ = '0';
        digit = dg_next(d2a);
        if (digit > 0) *s++ = (char)(digit + '0');
        while (exp10 > 0) { digit = d2a->finished ? 0 : dg_next(d2a); if (digit < 0) digit = 0; *s++ = (char)(digit + '0'); exp10--; digits++; }
        if (mode == 1) digits = 0;
        if (mode == 0) { if (!d2a->finished) { *s++ = '.'; wroteDecimal = 1; while (!d2a->finished) { int c = dg_next(d2a); if (c < 0) break; *s++ = (char)(c + '0'); } } }
        else if (digits < precision - 1) { *s++ = '.'; wroteDecimal = 1; for (; digits < precision - 1; digits++) { digit = d2a->finished ? 0 : dg_next(d2a); if (digit < 0) digit = 0; *s++ = (char)(digit + '0'); } }
        break; }
    case kFixedFraction: {
        int digits = 0;
        *s++ = '0'; *s++ = '0'; *s++ = '.'; wroteDecimal = 1;
        if (exp10 > 0) { while (++exp10 < 10 && digits < precision) { *s++ = '0'; digits++; } }
        else if (exp10 < 0) {
            /* bugzilla513039 (SWF 15+) is the fixed version; older players mis-rounded toFixed(0) */
            while ((exp10 < -1) && (precision > 0)) { exp10++; precision--; *s++ = '0'; }
            if (precision == 0 && exp10 != 0) round = 0;
        }
        for (; digits < precision; digits++) { if (d2a->finished) { if (mode == 0) break; *s++ = '0'; } else { int c = dg_next(d2a); *s++ = (char)((c < 0 ? 0 : c) + '0'); } }
        exp10 = 0;
        break; }
    case kFraction: {
        int i = 0, k;
        *s++ = '0'; *s++ = '0'; *s++ = '.'; wroteDecimal = 1;
        if (!zero) for (k = exp10; k < -1; k++) *s++ = '0';
        while (!d2a->finished) { int c = dg_next(d2a); if (c < 0) break; *s++ = (char)(c + '0'); if (mode != 0 && ++i >= precision) break; }
        if (mode == 2) while (i++ < precision) { int c = d2a->finished ? -1 : dg_next(d2a); *s++ = (char)((c < 0 ? 0 : c) + '0'); }
        exp10 = 0;
        break; }
    case kExponential: {
        int digit = d2a->finished ? 0 : dg_next(d2a); int i;
        if (digit < 0) digit = 0;
        *s++ = (char)(digit + '0');
        if ((mode == 0 && !d2a->finished) || (mode != 0 && precision > 1)) {
            *s++ = '.'; wroteDecimal = 1;
            for (i = 0; i < precision - 1; i++) { if (d2a->finished) { if (mode == 0) break; *s++ = '0'; } else { int c = dg_next(d2a); *s++ = (char)((c < 0 ? 0 : c) + '0'); } }
        }
        break; }
    }
    /* rounding: avmplus rounds only in FIXED/PRECISION modes (the "fast estimate" never applies) */
    if (round && (mode == 1 || mode == 2)) {
        if (dg_next(d2a) > 4) {
            char *ptr;
            for (ptr = s - 1; ptr >= sentinel; ptr--) { if (*ptr == '.') continue; *ptr = (char)(*ptr + 1); if (*ptr != ('9' + 1)) break; *ptr = '0'; }
        }
    }
    if (exp10) {
        char *firstNonZero = sentinel;
        while (firstNonZero < s && *firstNonZero == '0') firstNonZero++;
        if (s == firstNonZero) { *s++ = '1'; exp10++; }
        else if (!zero) {
            char *lastNonZero = s;
            while (lastNonZero > firstNonZero && *--lastNonZero == '0') ;
            if (firstNonZero == lastNonZero) { exp10 += (int)(s - firstNonZero - 1); s = lastNonZero + 1; }
        }
        *s++ = 'e'; if (exp10 > 0) *s++ = '+';
        sprintf(s, "%d", exp10); s += strlen(s);
    }
    *s = 0;
    s = sentinel;
    if (sentinel[0] == '0' && sentinel[1] != '.' && sentinel[1] != 0) s = sentinel + 1;
    if (negative) *--s = '-';
    if (mode == 1 && noFraction && wroteDecimal) { char *p = strrchr(s, '.'); if (p) *p = 0; }   /* bugzilla513039 */
    if (s != buffer) memmove(buffer, s, strlen(s) + 1);
    return buffer;
}
char *v2_num_to_cstr(double d, char *buf) { return a2_format_number(d, 0, 15, buf); }

/* ---- string -> number, after avmplus (Ruffle's string_to_f64 / string_to_int) ---- */
static const char *a2_skip_spaces(const char *p) {
    for (;;) {
        const u8 *q = (const u8*)p; unsigned c = q[0]; int l = 1;
        if (c >= 0xE0 && c < 0xF0 && q[1] && q[2]) { c = ((c & 0x0F) << 12) | ((q[1] & 0x3F) << 6) | (q[2] & 0x3F); l = 3; }
        if (c == 0x20 || c == 0x09 || c == 0x0d || c == 0x0a || c == 0x0c || c == 0x0b || (c >= 0x2000 && c <= 0x200b) || c == 0x2028 || c == 0x2029 || c == 0x205f || c == 0x3000) p += l;
        else return p;
    }
}
/* LLVM's powi (compiler-rt __powidf2): repeated squaring, inexact like Flash's */
static double a2_powi(double a, int b) {
    int recip = b < 0; double r = 1;
    if (recip) b = -b;
    for (;;) { if (b & 1) r *= a; b /= 2; if (!b) break; a *= a; }
    return recip ? 1 / r : r;
}
int a2_str_to_f64(const char *s, int swf_version, int strict, double *out) {
    extern double fl_strtod(const char *s, char **end);
    const char *p = a2_skip_spaces(s), *after_sign, *num_end; int neg = 0, total = 0; s32 exponent = 0; double result;
    if (!*p) { if (strict) { *out = 0; return 1; } return 0; }
    if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
    after_sign = p;
    while (*p >= '0' && *p <= '9') { p++; total++; }
    if (*p == '.') { p++; while (*p >= '0' && *p <= '9') { p++; total++; } }
    if (*p == 'e' || *p == 'E') {
        int eneg = 0;
        p++;
        if (*p == '-') { eneg = 1; p++; } else if (*p == '+') p++;
        if (eneg && !*p) return 0;
        while (*p >= '0' && *p <= '9') { exponent = (s32)((u32)exponent * 10u + (u32)(*p - '0')); p++; }
        if (eneg) exponent = (s32)(0u - (u32)exponent);
    }
    num_end = p;
    p = a2_skip_spaces(p);
    if (total == 0) {
        if (!strncmp(p, "Infinity", 8)) {
            const char *ai = p + 8;
            if (*ai) { const char *q = a2_skip_spaces(ai); if (q == ai) return 0; }
            *out = neg ? -1e308 * 10 : 1e308 * 10;
            return 1;
        }
        return 0;
    }
    if (strict && *p) return 0;
    {
        /* swf >= 11 stops at the parsed number; older ones read on (a second '.' restarts the fraction) */
        const char *q = after_sign, *lim = swf_version >= 11 ? num_end : NULL;
        int dec = -1;
        if (total > 15) {
            /* avmplus switches to exact integer arithmetic: the digits as a big integer */
            char *digits = (char*)malloc(strlen(after_sign) + 1); int nd = 0;
            if (!digits) return 0;
            for (; (!lim || q < lim) && *q; q++) {
                if (*q >= '0' && *q <= '9') { if (dec != -1) dec++; digits[nd++] = *q; }
                else if (*q == '.') dec = 0;
                else break;
            }
            digits[nd] = 0;
            if (dec > 0) exponent -= dec;
            if (exponent > 0) {
                int allzero = 1, k; for (k = 0; k < nd; k++) if (digits[k] != '0') { allzero = 0; break; }
                if (allzero) result = 0;
                else if (exponent > 400) result = 1e308 * 10;
                else result = fl_strtod(digits, NULL) * a2_powi(10.0, exponent);   /* scaled in doubles: the largest finite values overflow (Flash) */
            } else result = fl_strtod(nd ? digits : "0", NULL);
            free(digits);
        } else {
            result = 0;
            for (; (!lim || q < lim) && *q; q++) {
                if (*q >= '0' && *q <= '9') { if (dec != -1) dec++; result = result * 10.0 + (*q - '0'); }
                else if (*q == '.') dec = 0;
                else break;
            }
            if (dec > 0) exponent -= dec;
            if (exponent > 0) result *= a2_powi(10.0, exponent);
        }
    }
    if (exponent < 0) {
        if (exponent < -307) { s32 diff = exponent + 307; result /= a2_powi(10.0, -diff); exponent = -307; }
        result /= a2_powi(10.0, -exponent);
    }
    *out = neg ? -result : result;
    return 1;
}
double a2_str_to_int(const char *s, int radix, int strict) {
    const char *p = a2_skip_spaces(s), *start; int neg = 0; double result = 0;
    if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
    if (radix == 16 || radix == 0) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; radix = 16; }
        else if (radix == 0) radix = 10;
    }
    if (radix < 2 || radix > 36 || !*p) return 0.0 / 0.0;
    start = p;
    for (;; p++) {
        int d = (*p >= '0' && *p <= '9') ? *p - '0' : (*p >= 'a' && *p <= 'z') ? *p - 'a' + 10 : (*p >= 'A' && *p <= 'Z') ? *p - 'A' + 10 : 99;
        if (d >= radix) break;
        result = result * radix + d;
    }
    if (p == start) return 0.0 / 0.0;
    if (strict) { p = a2_skip_spaces(p); if (*p) return 0.0 / 0.0; }
    return neg ? -result : result;
}
static double str2num(VM2 *vm, const char *s) {
    double d;
    if (a2_str_to_f64(s, vm ? vm->swf_version : 10, 1, &d)) return d;
    return a2_str_to_int(s, 0, 1);
}

double v2_tonum(VM2 *vm, V2 v) {
    V2_NORM(v);
    switch (v.t) {
    case T_UNDEF: return 0.0 / 0.0;
    case T_NULL: return 0;
    case T_BOOL: return v.u.b;
    case T_INT: return v.u.i;
    case T_UINT: return v.u.u;
    case T_NUM: return v.u.n;
    case T_STR: return str2num(vm, v.u.s->s);
    case T_OBJ: { V2 p = v2_toprim(vm, v, 0); if (p.t == T_OBJ) { if (!vm->throwing) a2_throw_error(vm, "TypeError", "Cannot convert Object to primitive.", 1050); return 0.0 / 0.0; } return v2_tonum(vm, p); }
    }
    return 0.0 / 0.0;
}
s32 v2_toint(VM2 *vm, V2 v) {
    double d, m;
    if (v.t == T_INT) return v.u.i;
    if (v.t == T_UINT) return (s32)v.u.u;
    d = v2_tonum(vm, v);
    if (d != d || d > 1e300 || d < -1e300) return 0;
    m = fmod(floor(fabs(d)), 4294967296.0); if (d < 0) m = -m;
    if (m >= 2147483648.0) m -= 4294967296.0;
    if (m < -2147483648.0) m += 4294967296.0;
    return (s32)m;
}
u32 v2_touint(VM2 *vm, V2 v) { return (u32)v2_toint(vm, v); }
int v2_tobool(VM2 *vm, V2 v) {
    (void)vm; V2_NORM(v);
    switch (v.t) {
    case T_UNDEF: case T_NULL: return 0;
    case T_BOOL: return v.u.b;
    case T_INT: return v.u.i != 0;
    case T_UINT: return v.u.u != 0;
    case T_NUM: return v.u.n != 0 && v.u.n == v.u.n;
    case T_STR: return v.u.s->len > 0;
    case T_OBJ: return 1;
    }
    return 0;
}

A2Str *v2_tostr(VM2 *vm, V2 v) {
    char buf[64];
    V2_NORM(v);
    switch (v.t) {
    case T_UNDEF: return vm->s_undefined;
    case T_NULL: return vm->s_null;
    case T_BOOL: return a2_str(vm, v.u.b ? "true" : "false");
    case T_INT: sprintf(buf, "%d", (int)v.u.i); return a2_str(vm, buf);
    case T_UINT: sprintf(buf, "%u", (unsigned)v.u.u); return a2_str(vm, buf);
    case T_NUM: v2_num_to_cstr(v.u.n, buf); return a2_str(vm, buf);
    case T_STR: return v.u.s;
    case T_OBJ: {
        V2 p = v2_toprim(vm, v, 1);
        if (p.t == T_OBJ) {
            if (!vm->throwing) {
                A2Obj *o = v.u.o;
                if (o->kind == OK_CATCH && !o->proto && o->cls) { char b[400]; sprintf(b, "Property toString not found on %.250s and there is no default value.", o->cls->name->s); a2_throw_error(vm, "ReferenceError", b, 1069); }
                else a2_throw_error(vm, "TypeError", "Cannot convert Object to primitive.", 1050);
            }
            return a2_str(vm, "[object Object]");
        }
        return v2_tostr(vm, p);
    }
    }
    return vm->s_empty;
}

V2 v2_toprim(VM2 *vm, V2 v, int hint_str) {
    A2Obj *o; int i;
    V2_NORM(v);
    if (v.t != T_OBJ) return v;
    o = v.u.o;
    if (o->kind == OK_PRIM) return o->prim;
    for (i = 0; i < 2; i++) {
        A2Str *name = ((i == 0) == !!hint_str) ? vm->s_toString : vm->s_valueOf;
        A2Name n; V2 f, r;
        n.name = name; n.nss = &vm->ns_public; n.nns = 1; n.any_ns = 0; n.attr = 0;
        (void)f;
        r = a2_call_prop(vm, v, &n, 0, NULL);   /* Proxy objects see callProperty */
        if (vm->throwing) return v;
        V2_NORM(r);
        if (r.t != T_OBJ) return r;
    }
    { A2Class *c = v2_class_of(vm, v); a2_throw_id(vm, "TypeError", 1050, c ? a2_class_name(vm, c, 0)->s : "Object", NULL); }
    return v2_undef();
}

int v2_strict_eq(VM2 *vm, V2 a, V2 b) {
    (void)vm; V2_NORM(a); V2_NORM(b);
    if (v2_is_num(a) && v2_is_num(b)) return v2_tonum(vm, a) == v2_tonum(vm, b);
    if (a.t != b.t) return 0;
    switch (a.t) {
    case T_UNDEF: case T_NULL: return 1;
    case T_BOOL: return a.u.b == b.u.b;
    case T_STR: return a.u.s == b.u.s;
    case T_OBJ:
        if (a.u.o == b.u.o) return 1;
        /* method closures of the same method on the same receiver compare equal */
        if (a.u.o->kind == OK_FUNCTION && b.u.o->kind == OK_FUNCTION && a.u.o->is_bound && b.u.o->is_bound && a.u.o->method == b.u.o->method && a.u.o->method)
            return v2_strict_eq(vm, a.u.o->receiver, b.u.o->receiver);
        return 0;
    default: return 0;
    }
}
int v2_eq(VM2 *vm, V2 a, V2 b) {
    V2_NORM(a); V2_NORM(b);
    if ((a.t == T_OBJ && a.u.o->kind == OK_XML) || (b.t == T_OBJ && b.u.o->kind == OK_XML)) { extern int a2_xml_equals(VM2 *vm, V2 a, V2 b); if (a.t == T_OBJ && b.t == T_OBJ && a.u.o == b.u.o) return 1; return a2_xml_equals(vm, a, b); }
    if ((a.t == T_UNDEF || a.t == T_NULL) && (b.t == T_UNDEF || b.t == T_NULL)) return 1;
    if (a.t == T_UNDEF || a.t == T_NULL || b.t == T_UNDEF || b.t == T_NULL) return 0;
    if (v2_is_num(a) && v2_is_num(b)) return v2_tonum(vm, a) == v2_tonum(vm, b);
    if (a.t == T_STR && b.t == T_STR) return a.u.s == b.u.s;
    if (a.t == T_BOOL && b.t == T_BOOL) return a.u.b == b.u.b;
    if (a.t == T_OBJ && b.t == T_OBJ) {
        if (a.u.o == b.u.o) return 1;
        if (a.u.o->kind == OK_PRIM && b.u.o->kind == OK_PRIM) return v2_eq(vm, a.u.o->prim, b.u.o->prim);
        if (a.u.o->kind == OK_FUNCTION && b.u.o->kind == OK_FUNCTION) return v2_strict_eq(vm, a, b);   /* method closures */
        /* Namespaces compare by URI, QNames by URI and local name */
        if (a.u.o->kind == OK_NAMESPACE && b.u.o->kind == OK_NAMESPACE) return (a.u.o->nsval.uri ? a.u.o->nsval.uri : vm->s_empty) == (b.u.o->nsval.uri ? b.u.o->nsval.uri : vm->s_empty);
        if (a.u.o->kind == OK_QNAME && b.u.o->kind == OK_QNAME) return a2_qname_uri(a.u.o) == a2_qname_uri(b.u.o) && v2_eq(vm, a.u.o->prim, b.u.o->prim);
        return 0;
    }
    if (a.t == T_BOOL) return v2_eq(vm, v2_num(a.u.b), b);
    if (b.t == T_BOOL) return v2_eq(vm, a, v2_num(b.u.b));
    if (a.t == T_OBJ) return v2_eq(vm, v2_toprim(vm, a, b.t == T_STR), b);
    if (b.t == T_OBJ) return v2_eq(vm, a, v2_toprim(vm, b, a.t == T_STR));
    /* number vs string */
    return v2_tonum(vm, a) == v2_tonum(vm, b);
}

A2Class *v2_class_of(VM2 *vm, V2 v) {
    V2_NORM(v);
    switch (v.t) {
    case T_BOOL: return vm->c_boolean;
    case T_INT: return (v.u.i >= -268435456 && v.u.i <= 268435455) ? vm->c_int : vm->c_number;
    case T_UINT: return v.u.u <= 268435455u ? vm->c_int : vm->c_number;
    case T_NUM: { double d = v.u.n; if (d == floor(d) && d >= -268435456.0 && d <= 268435455.0 && !(d == 0 && 1.0 / d < 0)) return vm->c_int; return vm->c_number; }
    case T_STR: return vm->c_string;
    case T_OBJ: return v.u.o->cls ? v.u.o->cls : vm->c_object;
    default: return NULL;
    }
}

A2Obj *v2_toobj(VM2 *vm, V2 v) {
    A2Obj *o;
    if (v.t == T_OBJ) return v.u.o;
    if (v.t == T_UNDEF || v.t == T_NULL) return NULL;
    o = a2_new(vm, v2_class_of(vm, v));
    o->kind = OK_PRIM; o->prim = v;
    return o;
}

int a2_is_index(A2Str *s, u32 *idx) {
    u32 i, v = 0;
    if (s->len == 0 || s->len > 10) return 0;
    for (i = 0; i < s->len; i++) {
        u32 d;
        if (s->s[i] < '0' || s->s[i] > '9') return 0;
        d = (u32)(s->s[i] - '0');
        if (v > 429496729u || (v == 429496729u && d > 4)) return 0;   /* > 4294967294 is not an index */
        v = v * 10 + d;
    }
    if (s->len > 1 && s->s[0] == '0') return 0;
    *idx = v; return 1;
}

/* ---------------------------------------------------------------- */
/* objects                                                          */

A2Obj *a2_new(VM2 *vm, A2Class *cls) {
    A2Obj *o = (A2Obj*)gcalloc(vm, sizeof(A2Obj), G_OBJ);
    int n;
    if (!o) return NULL;
    o->cls = cls;
    o->kind = cls ? (u8)cls->kind : OK_PLAIN;
    o->proto = cls ? cls->prototype : NULL;
    n = cls ? cls->nislots : 0;
    if (n) { int i; o->slots = (V2*)calloc((size_t)n, sizeof(V2)); o->nslots = n; for (i = 0; i < n; i++) o->slots[i] = v2_undef(); }
    o->receiver = v2_undef(); o->prim = v2_undef();
    if (cls && n) a2_class_set_defaults(vm, cls, o);
    if (o->kind == OK_BYTEARRAY) { extern void a2_bytearray_preinit(VM2 *vm, A2Obj *o); a2_bytearray_preinit(vm, o); }   /* usable before super() */
    return o;
}
A2Obj *a2_new_plain(VM2 *vm) { A2Obj *o = a2_new(vm, vm->c_object); o->kind = OK_PLAIN; return o; }
A2Obj *a2_new_array(VM2 *vm) { A2Obj *o = a2_new(vm, vm->c_array); o->kind = OK_ARRAY; return o; }
A2Obj *a2_new_fn(VM2 *vm, A2Method *m, A2Obj *scope) {
    A2Obj *o = a2_new(vm, vm->c_function);
    o->kind = OK_FUNCTION; o->method = m; o->scope = scope;
    return o;
}
A2Method *a2_native_method(VM2 *vm, NativeFn2 fn, const char *name, int nparams) {
    A2Method *m = (A2Method*)calloc(1, sizeof(A2Method));
    m->native = fn; m->name = a2_str(vm, name); m->nparams = nparams;
    return m;
}
A2Obj *a2_new_native_fn(VM2 *vm, NativeFn2 fn, const char *name, int nparams) {
    return a2_new_fn(vm, a2_native_method(vm, fn, name, nparams), NULL);
}

static void arr_reserve(A2Obj *a, int n) {
    int nc; V2 *nv;
    if (n <= a->acap) return;
    nc = a->acap ? a->acap : 8; while (nc < n) nc *= 2;
    nv = (V2*)realloc(a->arr, (size_t)nc * sizeof(V2));
    if (!nv) return;
    a->arr = nv; a->acap = nc;
}
/* dense fill: make arr[0..n) exist (new slots are holes) */
void a2_arr_fill(VM2 *vm, A2Obj *a, int n) {
    int i; (void)vm;
    if (n < 0) n = 0; if (n > (1 << 26)) n = 1 << 26;
    arr_reserve(a, n); if (n > a->acap) return;
    for (i = a->alen; i < n; i++) a->arr[i] = v2_hole();
    if (n > a->alen) a->alen = n;
}
/* logical length: Arrays keep a dense part [0..alen) plus a logical length that may exceed it
 * (`new Array(5)`); Vectors are always dense */
u32 a2_arr_len(A2Obj *a) { return (a->kind == OK_ARRAY && a->biglen > (u32)a->alen) ? a->biglen : (u32)a->alen; }
void a2_arr_setlen(VM2 *vm, A2Obj *a, int n) {
    if (n < 0) n = 0;
    if (a->kind == OK_VECTOR) { if (n < a->alen) a->alen = n; else a2_arr_fill(vm, a, n); a->biglen = 0; return; }
    if (n <= a->alen) { a->alen = n; a->biglen = 0; return; }
    a->biglen = (u32)n;
}
void a2_arr_push(VM2 *vm, A2Obj *a, V2 v) {
    u32 L = a2_arr_len(a);
    if (L > (u32)a->alen && L <= (1u << 26)) a2_arr_fill(vm, a, (int)L);
    arr_reserve(a, a->alen + 1); if (a->alen < a->acap) a->arr[a->alen++] = v;
    if (a->biglen && a->biglen <= (u32)a->alen) a->biglen = 0; else if (a->biglen) a->biglen++;
}
V2 a2_arr_get(A2Obj *a, int i) { V2 v = (i >= 0 && i < a->alen) ? a->arr[i] : v2_undef(); V2_NORM(v); return v; }
void a2_arr_set(VM2 *vm, A2Obj *a, int i, V2 v) { if (i < 0) return; if (i >= a->alen) a2_arr_fill(vm, a, i + 1); if (i < a->alen) a->arr[i] = v; if (a->biglen && a->biglen <= (u32)a->alen) a->biglen = 0; }
/* Flash arrays may be sparse up to index 2^32-2; huge indices are stored as dynamic properties. */
int a2_arr_set_sparse(VM2 *vm, A2Obj *a, u32 idx, V2 v) {
    char b[16];
    if (idx == 0xFFFFFFFFu) return 0;                        /* not an array index: plain dynamic property */
    if (idx < (u32)a->alen + (1u << 20) && idx < (1u << 26)) { a2_arr_set(vm, a, (int)idx, v); return 1; }
    sprintf(b, "%lu", (unsigned long)idx);
    a2_dyn_set(vm, a, a2_str(vm, b), v);
    if (idx + 1 > a->biglen) a->biglen = idx + 1;
    return 1;
}
V2 a2_arr_length_val(A2Obj *a) {
    if (a->biglen > (u32)a->alen) return a->biglen > 0x7FFFFFFFu ? v2_uint(a->biglen) : v2_int((int)a->biglen);
    return v2_int(a->alen);
}

A2Prop *a2_dyn_find(A2Obj *o, A2Str *key) {
    A2Prop *p;
    if (!o->ndyn) return NULL;
    for (p = o->dyn[key->hash % (u32)o->ndyn]; p; p = p->next) if (p->key == key) return p;
    return NULL;
}
void a2_dyn_set(VM2 *vm, A2Obj *o, A2Str *key, V2 v) {
    A2Prop *p = a2_dyn_find(o, key); int b;
    (void)vm;
    if (p) { p->v = v; return; }
    if (!o->ndyn || o->dcount >= o->ndyn * 2) {
        int nb = o->ndyn ? o->ndyn * 4 : 8, i; A2Prop **nbk = (A2Prop**)calloc((size_t)nb, sizeof(A2Prop*));
        if (!nbk) return;
        for (i = 0; i < o->ndyn; i++) { A2Prop *q = o->dyn[i]; while (q) { A2Prop *n = q->next; int k = (int)(q->key->hash % (u32)nb); q->next = nbk[k]; nbk[k] = q; q = n; } }
        free(o->dyn); o->dyn = nbk; o->ndyn = nb;
    }
    p = (A2Prop*)calloc(1, sizeof(A2Prop)); if (!p) return;
    p->key = key; p->v = v; b = (int)(key->hash % (u32)o->ndyn); p->next = o->dyn[b]; o->dyn[b] = p; o->dcount++;
    p->oprev = o->dlast; p->onext = NULL; if (o->dlast) o->dlast->onext = p; else o->dfirst = p; o->dlast = p;
}
/* enumerate in insertion order: *idx is 1-based position of the previous entry (0 = start) */
void a2_dyn_enum(A2Obj *o, int *idx, A2Str **key, V2 *val) {
    A2Prop *p = o->dfirst; int i;
    for (i = 0; p && i < *idx; i++) p = p->onext;
    while (p && p->hidden) { p = p->onext; (*idx)++; }
    if (!p) { *idx = 0; return; }
    *idx = *idx + 1; *key = p->key; *val = p->v;
}
/* for-in over objects made by script (object literals, `new` of a function): avmplus keeps
 * their dynamic properties in an open-addressing hashtable (InlineHashtable: power-of-two
 * size starting at 2 entries, grown at 80% load, quadratic probe from 7, int-like keys
 * hashed by value) and enumerates it in slot order. String atoms hash by address, which
 * a scrambled content hash stands in for. Small objects only: bigger ones keep insertion
 * order. *idx is the 1-based slot of the previous entry (0 = start). */
#define HE_MAX 32
static u32 he_hash(A2Str *k) {
    u32 i, h;
    if (a2_is_index(k, &i) && i < 0x10000000u) return i;
    h = k->hash ^ 845u;
    h ^= h >> 16; h *= 0x85ebca6bu; h ^= h >> 13; h *= 0xc2b2ae35u; h ^= h >> 16;
    return h;
}
static void he_put(A2Prop **t, u32 m, A2Prop *p) {
    u32 i = he_hash(p->key) & (m - 1), n = 7;
    while (t[i]) { i = (i + n++) & (m - 1); }
    t[i] = p;
}
void a2_dyn_enum_hashed(A2Obj *o, int *idx, A2Str **key, V2 *val) {
    A2Prop *t[2 * HE_MAX], *tmp[2 * HE_MAX], *p; u32 m = 2, size = 0, i, j; int n = 0;
    for (p = o->dfirst; p; p = p->onext) if (++n > HE_MAX) { a2_dyn_enum(o, idx, key, val); return; }
    memset(t, 0, sizeof(t));
    for (p = o->dfirst; p; p = p->onext) {
        if ((size + 1) * 5 >= m * 4) {   /* grow: rehash in slot order */
            for (i = 0, j = 0; i < m; i++) if (t[i]) tmp[j++] = t[i];
            m *= 2; memset(t, 0, sizeof(t));
            for (i = 0; i < j; i++) he_put(t, m, tmp[i]);
        }
        he_put(t, m, p); size++;
    }
    for (i = (u32)(*idx > 0 ? *idx : 0); i < m; i++) if (t[i] && !t[i]->hidden) { *idx = (int)i + 1; *key = t[i]->key; *val = t[i]->v; return; }
    *idx = 0;
}
void a2_dyn_set_hidden(VM2 *vm, A2Obj *o, A2Str *key, V2 v) { A2Prop *p; a2_dyn_set(vm, o, key, v); p = a2_dyn_find(o, key); if (p) p->hidden = 1; }
static void dyn_unlink_order(A2Obj *o, A2Prop *p) {
    if (p->oprev) p->oprev->onext = p->onext; else o->dfirst = p->onext;
    if (p->onext) p->onext->oprev = p->oprev; else o->dlast = p->oprev;
}
/* delete: the node stays in the enumeration order list as a tombstone so that a for-in
 * loop deleting keys keeps its position; tombstones are purged when they pile up */
int a2_dyn_del(A2Obj *o, A2Str *key) {
    A2Prop **pp;
    if (!o->ndyn) return 0;
    for (pp = &o->dyn[key->hash % (u32)o->ndyn]; *pp; pp = &(*pp)->next) if ((*pp)->key == key) {
        A2Prop *p = *pp; *pp = p->next; p->next = NULL;
        p->deleted = 1; p->hidden = 1; p->v = v2_undef(); o->dcount--; o->ndeleted++;
        if (o->ndeleted > 64 && o->ndeleted > o->dcount) {
            A2Prop *q = o->dfirst;
            while (q) { A2Prop *nx = q->onext; if (q->deleted) { dyn_unlink_order(o, q); free(q); } q = nx; }
            o->ndeleted = 0;
        }
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* traits                                                           */

static void tm_insert(A2TraitMap *tm, A2Trait *t) {
    int b;
    if (!tm->nb) { tm->nb = 16; tm->b = (A2Trait**)calloc(16, sizeof(A2Trait*)); }
    else if (tm->n >= tm->nb * 2) {
        int nb = tm->nb * 4, i; A2Trait **nbk = (A2Trait**)calloc((size_t)nb, sizeof(A2Trait*));
        for (i = 0; i < tm->nb; i++) { A2Trait *q = tm->b[i]; while (q) { A2Trait *n = q->next; int k = (int)(q->name->hash % (u32)nb); q->next = nbk[k]; nbk[k] = q; q = n; } }
        free(tm->b); tm->b = nbk; tm->nb = nb;
    }
    b = (int)(t->name->hash % (u32)tm->nb);
    t->next = tm->b[b]; tm->b[b] = t; tm->n++;
    t->onext = NULL; if (tm->last) tm->last->onext = t; else tm->first = t; tm->last = t;
}

static int ns_open(int k) { return k == NS_PACKAGE || k == NS_NAMESPACE || k == NS_PACKAGE_INTERNAL || k == NS_EXPLICIT; }
int a2_ns_eq(const A2Ns *a, const A2Ns *b) { if (a->kind == NS_PRIVATE && b->kind == NS_PRIVATE && a->id != b->id) return 0; return a->uri == b->uri && (a->kind == b->kind || (ns_open(a->kind) && ns_open(b->kind))); }
static int ns_match(const A2Ns *t, const A2Ns *q) {
    if (t->kind == NS_PRIVATE && q->kind == NS_PRIVATE && t->id != q->id) return 0;
    if (t->uri == q->uri) return t->kind == q->kind || (ns_open(t->kind) && ns_open(q->kind));
    /* AS3 namespace calls resolve to public natives */
    if (q->kind == NS_NAMESPACE && q->uri->len == 33 && t->kind == NS_PACKAGE && t->uri->len == 0 && !strcmp(q->uri->s, "http://adobe.com/AS3/2006/builtin")) return 1;
    return 0;
}

/* a trait of user ABC code: its package-internal namespace is distinct from the public
   namespace of the same package (the player's own definitions stay lenient) */
static int ns_match_t(const A2Trait *t, const A2Ns *q) {
    if (t->abc && !t->abc->is_builtin && t->ns.uri == q->uri && t->ns.kind != q->kind && (t->ns.kind == NS_PACKAGE_INTERNAL || q->kind == NS_PACKAGE_INTERNAL)) {
        /* compilers emit a user namespace as package-internal; a Namespace value of it still matches */
        if (t->ns.kind == NS_PACKAGE_INTERNAL && q->kind == NS_NAMESPACE) return 1;
        if (q->kind == NS_PACKAGE_INTERNAL && t->ns.kind == NS_NAMESPACE) return 1;
        return 0;
    }
    return ns_match(&t->ns, q);
}

/* an interface's namespace ("pkg:IName", or "IName" in the top-level package), not a package
   namespace like new Namespace("flash.utils") that must not fall back to public members */
static int iface_like_ns(const A2Str *u) {
    if (!u || !u->len) return 0;
    if (memchr(u->s, ':', u->len)) return 1;
    return !memchr(u->s, '.', u->len) && u->s[0] >= 'A' && u->s[0] <= 'Z';
}
#define TK_IS_VAR(k) ((k) == TK_SLOT || (k) == TK_CONST)
int a2_trait_exact = 0;   /* set: no loose (public fallback) matches */
A2Trait *a2_find_trait(A2TraitMap *tm, A2Str *name, const A2Name *n) {
    A2Trait *t, *loose = NULL, *found = NULL;
    if (!tm->nb) return NULL;
    for (t = tm->b[name->hash % (u32)tm->nb]; t; t = t->next) {
        int i;
        if (t->name != name) continue;
        if (!n || n->any_ns) { if (t->ns.kind != NS_PRIVATE) return t; if (!loose) loose = t; continue; }
        for (i = 0; i < n->nns; i++) if (ns_match_t(t, &n->nss[i])) break;
        if (i < n->nns) {
            if (n->nns == 1) return t;
            /* several namespaces of the set bind: the most derived class's variable wins
               (bindings are searched from the subclass up; later slots belong to subclasses) */
            if (!found || (TK_IS_VAR(t->kind) && TK_IS_VAR(found->kind) && t->slot > found->slot)) found = t;
            continue;
        }
        /* interface namespaces / explicit namespaces resolve to public members */
        if (t->ns.kind == NS_PACKAGE && t->ns.uri->len == 0 && !loose && !a2_trait_exact) {
            for (i = 0; i < n->nns; i++) if ((n->nss[i].kind == NS_NAMESPACE && iface_like_ns(n->nss[i].uri)) || n->nss[i].kind == NS_PROTECTED || n->nss[i].kind == NS_STATIC_PROTECTED) { loose = t; break; }
        }
    }
    return found ? found : loose;
}
A2Trait *a2_find_trait_any(A2TraitMap *tm, A2Str *name) {
    A2Trait *t;
    if (!tm->nb) return NULL;
    for (t = tm->b[name->hash % (u32)tm->nb]; t; t = t->next) if (t->name == name) return t;
    return NULL;
}

/* replace-or-add (overrides) */
void a2_tm_put_impl(A2TraitMap *tm, A2Trait *t);
static void tm_put(A2TraitMap *tm, A2Trait *t) { a2_tm_put_impl(tm, t); }
void a2_tm_put_impl(A2TraitMap *tm, A2Trait *t) {
    A2Trait *e;
    if (tm->nb) for (e = tm->b[t->name->hash % (u32)tm->nb]; e; e = e->next) {
        if (e->name == t->name && (ns_match_t(e, &t->ns) || ns_match_t(t, &e->ns))) {
            /* override: merge getter/setter pairs, else replace */
            if ((t->kind == TK_GETTER || t->kind == TK_SETTER) && (e->kind == TK_SETTER || e->kind == TK_GETTER)) {
                if (t->getter) { e->getter = t->getter; e->at = t->at; e->abc = t->abc; }
                if (t->setter) { e->setter = t->setter; e->at2 = t->at2; e->abc2 = t->abc2; }
                if (e->kind == TK_SETTER && e->getter) e->kind = TK_GETTER;
                return;
            }
            e->kind = t->kind; e->slot = t->slot; e->method = t->method; e->getter = t->getter; e->setter = t->setter; e->type = t->type; e->cls = t->cls;
            e->at = t->at; e->at2 = t->at2; e->abc = t->abc; e->abc2 = t->abc2; e->type_mn = t->type_mn; e->def = t->def; e->has_def = t->has_def; e->final_ = t->final_;
            return;
        }
    }
    tm_insert(tm, t);
}

static void tm_copy(A2TraitMap *dst, const A2TraitMap *src) {
    int i;
    { A2Trait *t; for (t = src->first; t; t = t->onext) { A2Trait *c = (A2Trait*)malloc(sizeof(A2Trait)); *c = *t; c->next = NULL; c->onext = NULL; tm_insert(dst, c); } }
}

/* ---------------------------------------------------------------- */
/* classes                                                          */

A2Class *a2_class_new(VM2 *vm, const char *ns, const char *name, A2Class *super, int kind) {
    return a2_class_alloc(vm, a2_str(vm, ns ? ns : ""), a2_str(vm, name), super, kind, 1);
}
A2Class *a2_class_alloc(VM2 *vm, A2Str *ns, A2Str *name, A2Class *super, int kind, int reg) {
    A2Class *c = (A2Class*)gcalloc(vm, sizeof(A2Class), G_CLASS);
    c->name = name;
    c->ns.kind = NS_PACKAGE; c->ns.uri = ns;
    c->super = super;
    c->kind = kind;
    c->symbol_char = -1; c->shadow_slots = -1;
    c->depth = super ? super->depth + 1 : 0;
    if (super) { tm_copy(&c->itraits, &super->itraits); c->nislots = super->nislots; }
    c->prototype = (A2Obj*)gcalloc(vm, sizeof(A2Obj), G_OBJ);
    c->prototype->kind = OK_PLAIN; c->prototype->cls = vm->c_object; c->prototype->proto = super ? super->prototype : (vm->c_object ? vm->c_object->prototype : NULL);
    c->prototype->receiver = v2_undef(); c->prototype->prim = v2_undef();
    c->obj = (A2Obj*)gcalloc(vm, sizeof(A2Obj), G_OBJ);
    c->obj->kind = OK_CLASS; c->obj->as_class = c; c->obj->cls = vm->c_class; c->obj->proto = vm->c_class ? vm->c_class->prototype : NULL;
    c->obj->receiver = v2_undef(); c->obj->prim = v2_undef();
    if (vm->s_constructor) a2_dyn_set_hidden(vm, c->prototype, vm->s_constructor, v2_obj(c->obj));
    if (super) { c->native_ctor = super->native_ctor; c->native_call = NULL; }
    if (reg) a2_class_register(vm, c);
    return c;
}
void a2_class_set_defaults(VM2 *vm, A2Class *c, A2Obj *o) {
    int i; A2Trait *t; (void)vm;
    /* walk the super chain too when a subclass shadows a base var with its own slot */
    if (c->shadow_slots < 0) { int cnt = 0; for (i = 0; i < c->itraits.nb; i++) for (t = c->itraits.b[i]; t; t = t->next) if (t->kind == TK_SLOT || t->kind == TK_CONST) cnt++; c->shadow_slots = cnt < c->nislots; }
    for (; c; c = c->shadow_slots > 0 ? c->super : NULL)
    for (i = 0; i < c->itraits.nb; i++) for (t = c->itraits.b[i]; t; t = t->next) {
        if ((t->kind == TK_SLOT || t->kind == TK_CONST) && t->slot < o->nslots) {
            { extern A2Class *a2_resolve_trait_type(VM2 *vm, A2Trait *t); a2_resolve_trait_type(vm, t); }
            if (t->has_def) o->slots[t->slot] = t->def;
            else if (t->type == vm->c_int || t->type == vm->c_uint) o->slots[t->slot] = v2_int(0);
            else if (t->type == vm->c_number) o->slots[t->slot] = v2_num(0.0 / 0.0);
            else if (t->type == vm->c_boolean) o->slots[t->slot] = v2_bool(0);
            else if (t->type || (t->type_mn && t->abc)) o->slots[t->slot] = v2_null();   /* a class not defined yet: still an object type */
        }
    }
}

void a2_class_register(VM2 *vm, A2Class *c) {
    if (vm->nclasses == vm->capclasses) {
        int nc = vm->capclasses ? vm->capclasses * 2 : 256;
        void *n = realloc(vm->classes, (size_t)nc * sizeof(*vm->classes));
        if (!n) return;
        vm->classes = n; vm->capclasses = nc;
    }
    vm->classes[vm->nclasses].name = c->name; vm->classes[vm->nclasses].ns = c->ns.uri; vm->classes[vm->nclasses].cls = c;
    vm->nclasses++;
}
/* ApplicationDomains: a tree rooted at the system domain (0, the player's library); a name
   resolves in the domain chain from the root down, so a parent's definition wins */
static void dom_init(VM2 *vm) { if (!vm->ndom) { vm->dom_parent = (int*)malloc(16 * sizeof(int)); if (!vm->dom_parent) return; vm->dom_parent[0] = -1; vm->dom_parent[1] = 0; vm->ndom = 2; } }
int a2_new_domain(VM2 *vm, int parent) {
    dom_init(vm);
    if ((vm->ndom & 15) == 0) { int *np = (int*)realloc(vm->dom_parent, (size_t)(vm->ndom + 16) * sizeof(int)); if (!np) return 1; vm->dom_parent = np; }
    vm->dom_parent[vm->ndom] = parent >= 0 && parent < vm->ndom ? parent : 0;
    return vm->ndom++;
}
int a2_domain_parent(VM2 *vm, int d) { dom_init(vm); return d > 0 && d < vm->ndom ? vm->dom_parent[d] : -1; }
int a2_abc_domain(AbcFile *a) {
    SWFMovie *m;
    if (!a || a->is_builtin) return 0;
    m = (SWFMovie*)a->movie;
    return m && m->a2_domain > 0 ? m->a2_domain : 1;
}
int a2_cur_domain(VM2 *vm) {
    A2Frame *f;
    if (vm->lookup_domain > 0) return vm->lookup_domain;
    for (f = vm->cur; f; f = f->caller) if (f->m && f->m->abc && !f->m->abc->is_builtin) return a2_abc_domain(f->m->abc);
    { SWFMovie *m = vm->player ? PMOV(vm->player) : NULL; return m && m->a2_domain > 0 ? m->a2_domain : 1; }
}
/* position of domain d in the chain of cur (0 = the root); -1 when not visible */
static int dom_level(VM2 *vm, int d, int cur) {
    int chain[32], n = 0, i;
    dom_init(vm);
    while (cur >= 0 && n < 32) { chain[n++] = cur; if (cur >= vm->ndom) break; cur = vm->dom_parent[cur]; }
    for (i = 0; i < n; i++) if (chain[i] == d) return n - 1 - i;
    return -1;
}
A2Class *a2_class_find_dom(VM2 *vm, A2Str *ns, A2Str *name, int dom) {
    /* the first definition wins (a loaded SWF uses its parent domain's classes),
       except that the player library's ActionScript replaces a C placeholder */
    int i, best_lvl = 1 << 30; A2Class *best = NULL;
    for (i = 0; i < vm->nclasses; i++) if (vm->classes[i].name == name && (!ns || vm->classes[i].ns == ns)) {
        A2Class *c = vm->classes[i].cls; int lvl;
        if (!c->abc) lvl = 0;
        else { lvl = dom_level(vm, a2_abc_domain(c->abc), dom); if (lvl < 0) continue; }
        (void)best_lvl;   /* a domain keeps the definition it saw first: earlier ones win, ancestors' included */
        if (!best) { best = c; if (c->abc) return c; continue; }
        if (!best->abc && c->abc && c->abc->is_builtin) return c;
    }
    return best;
}
A2Class *a2_class_find(VM2 *vm, A2Str *ns, A2Str *name) { return a2_class_find_dom(vm, ns, name, a2_cur_domain(vm)); }
int a2_domain_visible(VM2 *vm, int d, int cur) { return dom_level(vm, d, cur) >= 0; }
extern A2Class *a2_class_lookup(VM2 *vm, A2Str *ns, A2Str *name);
A2Class *a2_class_find_s(VM2 *vm, const char *q) {
    const char *sep = strstr(q, "::"); char ns[1024];
    /* applied Vector types: "Vector.<T>" / "__AS3__.vec::Vector.<T>" (T may be dotted, qualified or nested) */
    { const char *v = strstr(q, "Vector.<");
      if (v && (v == q || (v == q + 13 && !strncmp(q, "__AS3__.vec::", 13))) && q[strlen(q) - 1] == '>') {
          char inner[512]; size_t n = strlen(v + 8); A2Class *et = NULL;
          if (n < 2 || n - 1 >= sizeof(inner)) return NULL;
          memcpy(inner, v + 8, n - 1); inner[n - 1] = 0;
          if (strcmp(inner, "*")) { et = a2_class_find_s(vm, inner); if (!et) return NULL; }
          return a2_vector_class_for(vm, et);
      } }
    if (!sep) { sep = strrchr(q, '.'); if (sep) { size_t n = (size_t)(sep - q); if (n > 1023) n = 1023; memcpy(ns, q, n); ns[n] = 0; return a2_class_lookup(vm, a2_str(vm, ns), a2_str(vm, sep + 1)); } return a2_class_lookup(vm, vm->s_empty, a2_str(vm, q)); }
    { size_t n = (size_t)(sep - q); if (n > 1023) n = 1023; memcpy(ns, q, n); ns[n] = 0; }
    return a2_class_lookup(vm, a2_str(vm, ns), a2_str(vm, sep + 2));
}
int a2_is_subclass(A2Class *c, A2Class *base) {
    int i; extern void a2_resolve_ifaces(A2Class *c);
    for (; c; c = c->super) {
        if (c == base) return 1;
        if (c->inst && c->inst == base->inst) return 1;   /* class objects of one ABC class share its type */
        a2_resolve_ifaces(c);
        for (i = 0; i < c->nifaces; i++) if (a2_is_subclass(c->ifaces[i], base)) return 1;
    }
    return 0;
}
int a2_instanceof(VM2 *vm, V2 v, A2Class *c) {
    A2Class *vc = v2_class_of(vm, v);
    if (!vc || !c) return 0;
    if (v2_is_num(v)) {
        double d = v2_tonum(vm, v);
        if (c == vm->c_number) return 1;
        if (c == vm->c_int) return d == floor(d) && d >= -2147483648.0 && d <= 2147483647.0;
        if (c == vm->c_uint) return d == floor(d) && d >= 0 && d <= 4294967295.0;
        return c == vm->c_object;
    }
    if (v.t == T_OBJ && v.u.o->kind == OK_CLASS) return c == vm->c_class || c == vm->c_object;
    return a2_is_subclass(vc, c);
}

static A2Trait *trait_alloc(VM2 *vm, A2Class *c, const char *name, int kind) {
    A2Trait *t = (A2Trait*)calloc(1, sizeof(A2Trait));
    t->name = a2_str(vm, name); t->ns = vm->ns_public; t->kind = kind;
    (void)c;
    return t;
}
void a2_add_method(VM2 *vm, A2Class *c, const char *name, NativeFn2 fn, int nparams) {
    A2Trait *t = trait_alloc(vm, c, name, TK_METHOD);
    t->method = a2_native_method(vm, fn, name, nparams); t->method->owner = c;
    tm_put(&c->itraits, t);
    /* also on prototype for dynamic lookup through Object */
}
void a2_add_static(VM2 *vm, A2Class *c, const char *name, NativeFn2 fn, int nparams) {
    A2Trait *t = trait_alloc(vm, c, name, TK_METHOD);
    t->method = a2_native_method(vm, fn, name, nparams); t->method->owner = c;
    tm_put(&c->ctraits, t);
}
void a2_add_getter(VM2 *vm, A2Class *c, const char *name, NativeFn2 get, NativeFn2 set) {
    A2Trait *t = trait_alloc(vm, c, name, TK_GETTER);
    if (get) { t->getter = a2_native_method(vm, get, name, 0); t->getter->owner = c; }
    if (set) { t->setter = a2_native_method(vm, set, name, 1); t->setter->owner = c; }
    tm_put(&c->itraits, t);
}
void a2_add_const(VM2 *vm, A2Class *c, const char *name, V2 v) {
    A2Trait *t = trait_alloc(vm, c, name, TK_CONST);
    t->slot = c->ncslots++;
    tm_put(&c->ctraits, t);
    { int i; V2 *ns = (V2*)realloc(c->obj->slots, (size_t)c->ncslots * sizeof(V2)); if (ns) { c->obj->slots = ns; for (i = c->obj->nslots; i < c->ncslots; i++) c->obj->slots[i] = v2_undef(); c->obj->nslots = c->ncslots; c->obj->slots[t->slot] = v; } }
}
void a2_add_slot(VM2 *vm, A2Class *c, const char *name, A2Class *type) {
    A2Trait *t = trait_alloc(vm, c, name, TK_SLOT);
    t->slot = c->nislots++; t->type = type;
    tm_put(&c->itraits, t);
}
A2Obj *a2_native_fn_obj(VM2 *vm, A2Class *c, const char *name) {
    A2Trait *t = a2_find_trait_any(&c->itraits, a2_str(vm, name));
    if (!t || !t->method) return NULL;
    return a2_new_fn(vm, t->method, NULL);
}

A2Str *a2_class_name(VM2 *vm, A2Class *c, int qualified) {
    char *buf; A2Str *r;
    if (!c) return vm->s_null;
    if (!qualified || c->ns.uri->len == 0) return c->name;
    buf = (char*)malloc(c->ns.uri->len + c->name->len + 3);
    if (!buf) return c->name;
    sprintf(buf, "%s::%s", c->ns.uri->s, c->name->s);
    r = a2_str(vm, buf); free(buf);
    return r;
}

/* ---------------------------------------------------------------- */
/* GC                                                               */

static void mark_tm(VM2 *vm, A2TraitMap *tm) {
    int i; A2Trait *t;
    for (i = 0; i < tm->nb; i++) for (t = tm->b[i]; t; t = t->next) { t->name->gc.mark = 1; if (t->ns.uri) t->ns.uri->gc.mark = 1; if (t->type) a2_mark_class(vm, t->type); if (t->cls) a2_mark_class(vm, t->cls); if (t->has_def) a2_mark_v(vm, t->def); if (t->method && t->method->name) t->method->name->gc.mark = 1; if (t->getter && t->getter->name) t->getter->name->gc.mark = 1; if (t->setter && t->setter->name) t->setter->name->gc.mark = 1; }
}
void a2_mark_class(VM2 *vm, A2Class *c) {
    int i;
    if (!c || c->gc.mark) return;
    c->gc.mark = 1;
    c->name->gc.mark = 1; if (c->ns.uri) c->ns.uri->gc.mark = 1; if (c->protected_ns.uri) c->protected_ns.uri->gc.mark = 1;
    if (c->super) a2_mark_class(vm, c->super);
    mark_tm(vm, &c->itraits); mark_tm(vm, &c->ctraits);
    a2_mark_obj(vm, c->obj); a2_mark_obj(vm, c->prototype); a2_mark_obj(vm, c->scope); a2_mark_obj(vm, c->cscope);
    for (i = 0; i < c->nifaces; i++) a2_mark_class(vm, c->ifaces[i]);
    if (c->iinit && c->iinit->name) c->iinit->name->gc.mark = 1;
    if (c->cinit && c->cinit->name) c->cinit->name->gc.mark = 1;
}
void a2_mark_obj(VM2 *vm, A2Obj *o) {
    int i;
    if (!o || o->gc.mark) return;
    o->gc.mark = 1;
    if (o->cls) a2_mark_class(vm, o->cls);
    a2_mark_obj(vm, o->proto);
    for (i = 0; i < o->nslots; i++) a2_mark_v(vm, o->slots[i]);
    for (i = 0; i < o->ndyn; i++) { A2Prop *p; for (p = o->dyn[i]; p; p = p->next) { p->key->gc.mark = 1; a2_mark_v(vm, p->v); } }
    a2_mark_obj(vm, o->scope); a2_mark_v(vm, o->receiver);
    if (o->as_class) a2_mark_class(vm, o->as_class);
    for (i = 0; i < o->alen; i++) a2_mark_v(vm, o->arr[i]);
    if (o->vec_type) a2_mark_class(vm, o->vec_type);
    if (o->nsval.uri) o->nsval.uri->gc.mark = 1;
    a2_mark_v(vm, o->prim);
    a2_mark_obj(vm, o->parent_disp);
    { A2Listener *l; for (l = o->listeners; l; l = l->next) { a2_mark_v(vm, l->fn); l->type->gc.mark = 1; } }
    for (i = 0; i < o->nframe_scripts; i++) a2_mark_v(vm, o->frame_scripts[i]);
    for (i = 0; i < o->dn; i++) { a2_mark_v(vm, o->dkeys[i]); a2_mark_v(vm, o->dvals[i]); }
    if (o->method && o->method->name) o->method->name->gc.mark = 1;
    if (o->method && o->method->owner) a2_mark_class(vm, o->method->owner);
    { extern void a2_mark_payload(VM2 *vm, A2Obj *o); a2_mark_payload(vm, o); }
}
void a2_mark_v(VM2 *vm, V2 v) {
    if (v.t == T_STR) v.u.s->gc.mark = 1;
    else if (v.t == T_OBJ) a2_mark_obj(vm, v.u.o);
}

static void gc_mark_owner_cb(void *ctx, void *owner) { a2_mark_obj((VM2*)ctx, (A2Obj*)owner); }
static void mark_clip_tree(VM2 *vm, MovieClip *c) {
    DisplayObject *d;
    if (!c) return;
    if (c->obj2) a2_mark_obj(vm, (A2Obj*)c->obj2);
    for (d = c->children; d; d = d->next) { if (d->obj2) a2_mark_obj(vm, (A2Obj*)d->obj2); if (d->clip) mark_clip_tree(vm, d->clip); }
}

static void obj_free(A2Obj *o) {
    int i;
    free(o->slots);
    { A2Prop *p = o->dfirst; while (p) { A2Prop *n = p->onext; free(p); p = n; } }   /* every property (incl. tombstones) is on the order list */
    (void)i;
    free(o->dyn); free(o->arr); free(o->frame_scripts); free(o->dkeys); free(o->dvals); free(o->bytes);
    { A2Listener *l = o->listeners; while (l) { A2Listener *n = l->next; free(l); l = n; } }
    { extern void a2_free_payload(A2Obj *o); a2_free_payload(o); }
    if (o->kind == OK_DISPLAY) { if (o->clip && o->clip->obj2 == o) o->clip->obj2 = NULL; if (o->dobj && o->dobj->obj2 == o) o->dobj->obj2 = NULL; }
}
static void class_free(A2Class *c) {
    int i; A2Trait *t;
    for (i = 0; i < c->itraits.nb; i++) { t = c->itraits.b[i]; while (t) { A2Trait *n = t->next; free(t); t = n; } }
    for (i = 0; i < c->ctraits.nb; i++) { t = c->ctraits.b[i]; while (t) { A2Trait *n = t->next; free(t); t = n; } }
    free(c->itraits.b); free(c->ctraits.b); free(c->ifaces);
}

void a2_obj_free_for_shutdown(A2Obj *o) { obj_free(o); }
void a2_class_free_for_shutdown(A2Class *c) { class_free(c); }

void a2_gc(VM2 *vm) {
    A2GC **pp; int i; A2Frame *f;
    /* roots */
    for (i = 0; i < vm->capintern; i++) { A2Str *s; for (s = vm->intern ? vm->intern[i] : NULL; s; s = s->inext) s->gc.mark = 1; }
    for (i = 0; i < vm->nclasses; i++) a2_mark_class(vm, vm->classes[i].cls);
    { extern void a2_rt_mark_all(VM2 *vm); extern void a2_mark_aliases(VM2 *vm); a2_rt_mark_all(vm); a2_mark_aliases(vm); }
    a2_mark_obj(vm, vm->global); if (vm->dom_mem) a2_mark_obj(vm, vm->dom_mem); if (vm->dom_default) a2_mark_obj(vm, vm->dom_default); a2_mark_obj(vm, vm->stage_obj); a2_mark_obj(vm, vm->root_obj);
    for (i = 0; i < vm->nglobals; i++) a2_mark_obj(vm, vm->globals[i]);
    for (i = 0; i < vm->sp; i++) a2_mark_v(vm, vm->stack[i]);
    for (i = 0; i < vm->scope_sp; i++) a2_mark_obj(vm, vm->scope_stack[i]);
    for (f = vm->cur; f; f = f->caller) { int k; for (k = 0; k < f->nlocals; k++) a2_mark_v(vm, f->locals[k]); a2_mark_v(vm, f->self); a2_mark_obj(vm, f->saved_scope); a2_mark_obj(vm, f->activation); }
    a2_mark_v(vm, vm->exc);
    if (vm->sb_str) vm->sb_str->gc.mark = 1;   /* the unpublished string builder */
    for (i = 0; i < vm->ntimers; i++) a2_mark_obj(vm, vm->timers[i].timer);
    { extern void a2_netstream_mark(VM2 *vm); a2_netstream_mark(vm); }
    { extern void a2_sockets_mark(VM2 *vm); a2_sockets_mark(vm); }
    { extern void a2_fileref_mark(VM2 *vm); a2_fileref_mark(vm); }
    { extern void a2_localconn_mark(VM2 *vm); a2_localconn_mark(vm); }
    { extern void a2_netconn_mark(VM2 *vm); a2_netconn_mark(vm); }
    { extern void a2_extif_mark(VM2 *vm); a2_extif_mark(vm); }
    a2_mark_obj(vm, vm->hover); a2_mark_obj(vm, vm->pressed); a2_mark_obj(vm, vm->pressed_mid); a2_mark_obj(vm, vm->pressed_right);
    mark_clip_tree(vm, vm->player->root);
    for (i = 0; i < vm->nfsq; i++) a2_mark_obj(vm, vm->fsq[i]);
    for (i = 0; i < vm->nfc_reenter; i++) a2_mark_obj(vm, vm->fc_reenter[i]);
    { extern void audio_foreach_owner2(Player *p, void (*fn)(void *ctx, void *owner), void *ctx); audio_foreach_owner2(vm->player, gc_mark_owner_cb, vm); }
    { extern void a2_prune_bcast(VM2 *vm); extern void a2_prune_orphans(VM2 *vm); a2_prune_bcast(vm); a2_prune_orphans(vm); }
    /* sweep */
    pp = &vm->gc_list;
    while (*pp) {
        A2GC *h = *pp;
        if (h->mark) { h->mark = 0; pp = &h->next; continue; }
        *pp = h->next;
        if (h->kind == G_STR) { free(((A2Str*)h)->s); }       /* interned strings are always marked; defensive */
        else if (h->kind == G_OBJ) obj_free((A2Obj*)h);
        else class_free((A2Class*)h);
        free(h); vm->gc_count--;
    }
    vm->gc_threshold = vm->gc_count * 2 + 8192;
    { extern size_t g_a2_ext_bytes; g_a2_ext_bytes = 0; }
    if (getenv("FLASHNT_GCDBG")) { A2GC *h; unsigned long arr = 0, by = 0, sl = 0, nstr = 0, strb = 0, mx = 0; for (h = vm->gc_list; h; h = h->next) { if (h->kind == G_OBJ) { A2Obj *o = (A2Obj*)h; arr += (unsigned long)o->acap * sizeof(V2); by += (unsigned long)o->bcap; sl += (unsigned long)o->nslots * sizeof(V2); if ((unsigned long)o->acap > mx) mx = (unsigned long)o->acap; } else if (h->kind == G_STR) { nstr++; strb += ((A2Str*)h)->len; } }
        if (getenv("FLASHNT_GCDBG")[0] == '2') { struct { A2Class *c; unsigned long n; } tab[64]; int nt = 0, k; for (h = vm->gc_list; h; h = h->next) if (h->kind == G_OBJ) { A2Class *c = ((A2Obj*)h)->cls; for (k = 0; k < nt; k++) if (tab[k].c == c) { tab[k].n++; break; } if (k == nt && nt < 64) { tab[nt].c = c; tab[nt].n = 1; nt++; } }
            for (k = 0; k < nt; k++) if (tab[k].n > 5000) fprintf(stderr, "[gc]   %s: %lu\n", tab[k].c && tab[k].c->name ? tab[k].c->name->s : "?", tab[k].n);
            nt = 0; for (h = vm->gc_list; h; h = h->next) if (h->kind == G_OBJ && ((A2Obj*)h)->kind == OK_CLASS) { A2Class *c = ((A2Obj*)h)->as_class; A2Str *nm = c ? c->name : NULL; for (k = 0; k < nt; k++) if (tab[k].c && tab[k].c->name == nm) { tab[k].n++; break; } if (k == nt && nt < 64) { tab[nt].c = c; tab[nt].n = 1; nt++; } }
            for (k = 0; k < nt; k++) if (tab[k].n > 500) fprintf(stderr, "[gc]   class %s: %lu\n", tab[k].c && tab[k].c->name ? tab[k].c->name->s : "?", tab[k].n); }
        { extern size_t g_a2_ext_bytes; fprintf(stderr, "[gc] ext %lu frame %d\n", (unsigned long)g_a2_ext_bytes, vm->frame_no); } fprintf(stderr, "[gc] live %lu threshold %lu arrays %lu (max cap %lu) bytes %lu slots %lu strs %lu/%lu interned %d\n", (unsigned long)vm->gc_count, (unsigned long)vm->gc_threshold, arr, mx, by, sl, nstr, strb, vm->nintern); }
}
