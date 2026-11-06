/* avm1_str.c - AVM1 strings (Latin-1 or UTF-16), interning, number
 * formatting and parsing exactly as the Flash Player does them. */
#include "avm1_int.h"
#include "casemap.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ---------------------------------------------------------------- */
/* GC allocation                                                    */

void *gc_alloc(VM *vm, size_t sz, int kind) {
    GCHdr *h = (GCHdr*)calloc(1, sz);
    if (!h) return NULL;
    h->kind = (u8)kind; h->mark = 0;
    h->next = vm->gc_list; vm->gc_list = h;
    vm->gc_count++;
    return h;
}

/* ---------------------------------------------------------------- */
/* hashing / case mapping                                           */

u16 swf_lower(u16 c) { return cm_to_lower(c); }
u16 swf_upper(u16 c) { return cm_to_upper(c); }

static u32 hash_units(const AStr *s) {
    u32 h = 2166136261u, i;
    for (i = 0; i < s->len; i++) { u16 c = swf_lower(STR_AT(s, i)); h ^= (u8)c; h *= 16777619u; h ^= (u8)(c >> 8); h *= 16777619u; }
    return h;
}

/* ---------------------------------------------------------------- */
/* construction                                                     */

static AStr *str_alloc(VM *vm, size_t len, int wide) {
    AStr *a = (AStr*)gc_alloc(vm, sizeof(AStr), GC_STR);
    if (!a) return NULL;
    a->len = (u32)len; a->wide = (u8)wide;
    if (wide) a->d.w = (u16*)malloc((len ? len : 1) * sizeof(u16));
    else a->d.b = (u8*)malloc(len ? len : 1);
    if (!(wide ? (void*)a->d.w : (void*)a->d.b)) { a->len = 0; }
    return a;
}

AStr *str_latin1(VM *vm, const u8 *s, size_t len) {
    AStr *a = str_alloc(vm, len, 0);
    if (!a) return NULL;
    if (a->len) memcpy(a->d.b, s, len);
    a->hash = hash_units(a);
    return a;
}

AStr *str_utf16(VM *vm, const u16 *s, size_t len) {
    AStr *a = str_alloc(vm, len, 1);
    if (!a) return NULL;
    if (a->len) memcpy(a->d.w, s, len * sizeof(u16));
    a->hash = hash_units(a);
    return a;
}

AStr *str_from_units(VM *vm, const u16 *u, size_t n) {
    size_t i; int wide = 0;
    for (i = 0; i < n; i++) if (u[i] > 0xFF) { wide = 1; break; }
    if (wide) return str_utf16(vm, u, n);
    {
        AStr *a = str_alloc(vm, n, 0);
        if (!a) return NULL;
        for (i = 0; i < a->len; i++) a->d.b[i] = (u8)u[i];
        a->hash = hash_units(a);
        return a;
    }
}

/* decode UTF-8 into UTF-16 units (invalid sequences become U+FFFD) */
static size_t utf8_decode_units(const char *s, size_t len, u16 *out) {
    size_t i = 0, n = 0;
    const u8 *p = (const u8*)s;
    while (i < len) {
        u32 c = p[i], need = 0, k;
        if (c < 0x80) { i++; }
        else if ((c & 0xE0) == 0xC0) { need = 1; c &= 0x1F; }
        else if ((c & 0xF0) == 0xE0) { need = 2; c &= 0x0F; }
        else if ((c & 0xF8) == 0xF0) { need = 3; c &= 0x07; }
        else { c = 0xFFFD; i++; }
        if (need) {
            if (i + need >= len) { c = 0xFFFD; i++; goto put; }
            for (k = 1; k <= need; k++) if ((p[i + k] & 0xC0) != 0x80) break;
            if (k <= need) { c = 0xFFFD; i++; goto put; }
            for (k = 1; k <= need; k++) c = (c << 6) | (p[i + k] & 0x3F);
            i += need + 1;
            if ((need == 1 && c < 0x80) || (need == 2 && c < 0x800) || (need == 3 && c < 0x10000) || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) c = 0xFFFD;
        }
    put:
        if (c >= 0x10000) { if (out) { out[n] = (u16)(0xD800 + ((c - 0x10000) >> 10)); out[n + 1] = (u16)(0xDC00 + ((c - 0x10000) & 0x3FF)); } n += 2; }
        else { if (out) out[n] = (u16)c; n++; }
    }
    return n;
}

AStr *str_utf8(VM *vm, const char *s, size_t len) {
    size_t i, n; u16 *u; AStr *a;
    for (i = 0; i < len; i++) if ((u8)s[i] >= 0x80) break;
    if (i == len) return str_latin1(vm, (const u8*)s, len);
    n = utf8_decode_units(s, len, NULL);
    u = (u16*)malloc((n ? n : 1) * sizeof(u16));
    if (!u) return str_latin1(vm, (const u8*)"", 0);
    utf8_decode_units(s, len, u);
    a = str_from_units(vm, u, n);
    free(u);
    return a;
}

AStr *str_cstr(VM *vm, const char *s) { return str_utf8(vm, s, strlen(s)); }

/* Windows-1252 code points for 0x80..0x9F */
static const u16 cp1252_hi[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
};

/* the code page 1252 byte of a character, -1 when it has none */
int str_unit_to_cp1252(u16 c) {
    int i;
    if (c <= 0xFF) return c;
    for (i = 0; i < 32; i++) if (cp1252_hi[i] == c) return 0x80 + i;
    return -1;
}

AStr *str_from_swf(VM *vm, const char *s, size_t len, int swf_version) {
    size_t i; int wide = 0;
    if (swf_version >= 6) return str_utf8(vm, s, len);
    for (i = 0; i < len; i++) { u8 c = (u8)s[i]; if (c >= 0x80 && c <= 0x9F && cp1252_hi[c - 0x80] > 0xFF) { wide = 1; break; } }
    if (!wide) return str_latin1(vm, (const u8*)s, len);
    {
        u16 *u = (u16*)malloc((len ? len : 1) * sizeof(u16)); AStr *a;
        if (!u) return str_latin1(vm, (const u8*)s, len);
        for (i = 0; i < len; i++) { u8 c = (u8)s[i]; u[i] = (c >= 0x80 && c <= 0x9F) ? cp1252_hi[c - 0x80] : c; }
        a = str_utf16(vm, u, len); free(u);
        return a;
    }
}

/* ---------------------------------------------------------------- */
/* interning                                                        */

static int intern_lookup(VM *vm, const AStr *s, u32 *slot) {
    u32 mask = (u32)vm->capintern - 1, i = s->hash & mask;
    while (vm->intern[i]) {
        if (vm->intern[i]->hash == s->hash && str_eq(vm->intern[i], s)) { *slot = i; return 1; }
        i = (i + 1) & mask;
    }
    *slot = i;
    return 0;
}

static void intern_grow(VM *vm) {
    int nc = vm->capintern ? vm->capintern * 2 : 1024, i;
    AStr **old = vm->intern; int oc = vm->capintern;
    AStr **ni = (AStr**)calloc((size_t)nc, sizeof(AStr*));
    if (!ni) return;
    vm->intern = ni; vm->capintern = nc;
    for (i = 0; i < oc; i++) if (old[i]) { u32 slot; intern_lookup(vm, old[i], &slot); vm->intern[slot] = old[i]; }
    free(old);
}

AStr *str_intern_str(VM *vm, AStr *s) {
    u32 slot;
    if (!s) return NULL;
    if (s->interned) return s;
    if (vm->nintern * 2 >= vm->capintern) intern_grow(vm);
    if (!vm->capintern) return s;
    if (intern_lookup(vm, s, &slot)) return vm->intern[slot];
    s->interned = 1;
    vm->intern[slot] = s; vm->nintern++;
    return s;
}

AStr *str_intern(VM *vm, const char *s) {
    AStr tmp; u32 slot; size_t len = strlen(s), i;
    for (i = 0; i < len; i++) if ((u8)s[i] >= 0x80) break;
    if (i < len) return str_intern_str(vm, str_cstr(vm, s));
    tmp.len = (u32)len; tmp.wide = 0; tmp.d.b = (u8*)s;
    tmp.hash = hash_units(&tmp);
    if (vm->nintern * 2 >= vm->capintern) intern_grow(vm);
    if (vm->capintern && intern_lookup(vm, &tmp, &slot)) return vm->intern[slot];
    return str_intern_str(vm, str_latin1(vm, (const u8*)s, len));
}

/* ---------------------------------------------------------------- */
/* views                                                            */

const char *str_c(VM *vm, AStr *s) {
    size_t n = 0, i; char *o; int pass;
    (void)vm;
    if (!s) return "";
    if (s->utf8) return s->utf8;
    o = NULL;
    for (pass = 0; pass < 2; pass++) {
        if (pass == 1) { o = (char*)malloc(n + 1); if (!o) return ""; n = 0; }
        for (i = 0; i < s->len; i++) {
            u32 c = STR_AT(s, i);
            if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s->len && STR_AT(s, i + 1) >= 0xDC00 && STR_AT(s, i + 1) <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (u32)(STR_AT(s, i + 1) - 0xDC00); i++;
                if (pass) { o[n] = (char)(0xF0 | (c >> 18)); o[n + 1] = (char)(0x80 | ((c >> 12) & 0x3F)); o[n + 2] = (char)(0x80 | ((c >> 6) & 0x3F)); o[n + 3] = (char)(0x80 | (c & 0x3F)); }
                n += 4; continue;
            }
            if (c >= 0xD800 && c <= 0xDFFF) c = 0xFFFD;
            if (c < 0x80) { if (pass) o[n] = (char)c; n++; }
            else if (c < 0x800) { if (pass) { o[n] = (char)(0xC0 | (c >> 6)); o[n + 1] = (char)(0x80 | (c & 0x3F)); } n += 2; }
            else { if (pass) { o[n] = (char)(0xE0 | (c >> 12)); o[n + 1] = (char)(0x80 | ((c >> 6) & 0x3F)); o[n + 2] = (char)(0x80 | (c & 0x3F)); } n += 3; }
        }
    }
    o[n] = 0;
    s->utf8 = o;
    return o;
}

u16 *str_units(VM *vm, AStr *s, u16 *tmp, size_t tmpcap) {
    u16 *u; u32 i;
    (void)vm;
    if (s->wide) return s->d.w;
    u = s->len <= tmpcap ? tmp : (u16*)malloc((s->len ? s->len : 1) * sizeof(u16));
    if (!u) return tmp;
    for (i = 0; i < s->len; i++) u[i] = s->d.b[i];
    return u;
}

/* ---------------------------------------------------------------- */
/* operations                                                       */

AStr *str_concat(VM *vm, AStr *a, AStr *b) {
    AStr *r; u32 i;
    if (!a->len) return b;
    if (!b->len) return a;
    r = str_alloc(vm, (size_t)a->len + b->len, a->wide || b->wide);
    if (!r) return a;
    if (r->wide) {
        for (i = 0; i < a->len; i++) r->d.w[i] = STR_AT(a, i);
        for (i = 0; i < b->len; i++) r->d.w[a->len + i] = STR_AT(b, i);
    } else {
        memcpy(r->d.b, a->d.b, a->len); memcpy(r->d.b + a->len, b->d.b, b->len);
    }
    r->hash = hash_units(r);
    return r;
}

AStr *str_slice(VM *vm, AStr *s, u32 start, u32 end) {
    if (end > s->len) end = s->len;
    if (start > end) start = end;
    if (start == 0 && end == s->len) return s;
    if (s->wide) return str_from_units(vm, s->d.w + start, end - start);
    return str_latin1(vm, s->d.b + start, end - start);
}

int str_eq(const AStr *a, const AStr *b) {
    u32 i;
    if (a == b) return 1;
    if (a->len != b->len || a->hash != b->hash) return 0;
    if (!a->wide && !b->wide) return memcmp(a->d.b, b->d.b, a->len) == 0;
    for (i = 0; i < a->len; i++) if (STR_AT(a, i) != STR_AT(b, i)) return 0;
    return 1;
}

int str_eq_ci(const AStr *a, const AStr *b) {
    u32 i;
    if (a == b) return 1;
    if (a->len != b->len || a->hash != b->hash) return 0;
    for (i = 0; i < a->len; i++) { u16 x = STR_AT(a, i), y = STR_AT(b, i); if (x != y && swf_lower(x) != swf_lower(y)) return 0; }
    return 1;
}

int str_eq_case(const AStr *a, const AStr *b, int cs) { return cs ? str_eq(a, b) : str_eq_ci(a, b); }

int str_eq_c(const AStr *a, const char *s) {
    u32 i; size_t n = strlen(s);
    if (a->len != n) return 0;
    for (i = 0; i < a->len; i++) if (STR_AT(a, i) != (u8)s[i]) return 0;
    return 1;
}

int str_eq_c_ci(const AStr *a, const char *s) {
    u32 i; size_t n = strlen(s);
    if (a->len != n) return 0;
    for (i = 0; i < a->len; i++) if (swf_lower(STR_AT(a, i)) != swf_lower((u8)s[i])) return 0;
    return 1;
}

int str_starts_c(const AStr *s, const char *p) { u32 i; size_t n = strlen(p); if (s->len < n) return 0; for (i = 0; i < n; i++) if (STR_AT(s, i) != (u8)p[i]) return 0; return 1; }
int str_starts_c_ci(const AStr *s, const char *p) { u32 i; size_t n = strlen(p); if (s->len < n) return 0; for (i = 0; i < n; i++) if (swf_lower(STR_AT(s, i)) != swf_lower((u8)p[i])) return 0; return 1; }

int str_cmp(const AStr *a, const AStr *b) {
    u32 i, n = a->len < b->len ? a->len : b->len;
    for (i = 0; i < n; i++) { u16 x = STR_AT(a, i), y = STR_AT(b, i); if (x != y) return x < y ? -1 : 1; }
    return a->len < b->len ? -1 : a->len > b->len ? 1 : 0;
}

long str_find(const AStr *s, const AStr *pat, u32 from) {
    u32 i, j;
    if (pat->len == 0) return from <= s->len ? (long)from : -1;
    if (pat->len > s->len) return -1;
    for (i = from; i + pat->len <= s->len; i++) {
        for (j = 0; j < pat->len; j++) if (STR_AT(s, i + j) != STR_AT(pat, j)) break;
        if (j == pat->len) return (long)i;
    }
    return -1;
}

long str_rfind(const AStr *s, const AStr *pat, long from) {
    long i; u32 j;
    if (pat->len > s->len) return -1;
    if (from > (long)(s->len - pat->len)) from = (long)(s->len - pat->len);
    for (i = from; i >= 0; i--) {
        for (j = 0; j < pat->len; j++) if (STR_AT(s, (u32)i + j) != STR_AT(pat, j)) break;
        if (j == pat->len) return i;
    }
    return -1;
}

long str_find_unit(const AStr *s, u16 c, u32 from) { u32 i; for (i = from; i < s->len; i++) if (STR_AT(s, i) == c) return (long)i; return -1; }
long str_rfind_any(const AStr *s, const char *units) { long i; for (i = (long)s->len - 1; i >= 0; i--) { u16 c = STR_AT(s, (u32)i); if (c < 0x80 && strchr(units, (char)c) && c) return i; } return -1; }
long str_find_any(const AStr *s, const char *units, u32 from) { u32 i; for (i = from; i < s->len; i++) { u16 c = STR_AT(s, i); if (c && c < 0x80 && strchr(units, (char)c)) return (long)i; } return -1; }

AStr *str_lower(VM *vm, AStr *s) {
    SBuf b; u32 i;
    sb_init(&b);
    for (i = 0; i < s->len; i++) sb_unit(&b, swf_lower(STR_AT(s, i)));
    return sb_done(vm, &b);
}
AStr *str_upper(VM *vm, AStr *s) {
    SBuf b; u32 i;
    sb_init(&b);
    for (i = 0; i < s->len; i++) sb_unit(&b, swf_upper(STR_AT(s, i)));
    return sb_done(vm, &b);
}

/* ---------------------------------------------------------------- */
/* builder                                                          */

void sb_init(SBuf *b) { b->u = NULL; b->n = b->cap = 0; }
void sb_unit(SBuf *b, u16 c) {
    if (b->n == b->cap) { size_t nc = b->cap ? b->cap * 2 : 32; u16 *nu = (u16*)realloc(b->u, nc * sizeof(u16)); if (!nu) return; b->u = nu; b->cap = nc; }
    b->u[b->n++] = c;
}
void sb_str(SBuf *b, const AStr *s) { u32 i; for (i = 0; i < s->len; i++) sb_unit(b, STR_AT(s, i)); }
void sb_ascii(SBuf *b, const char *s) { while (*s) sb_unit(b, (u8)*s++); }
void sb_utf8(SBuf *b, const char *s) {
    size_t len = strlen(s), n = utf8_decode_units(s, len, NULL), i;
    u16 *u = (u16*)malloc((n ? n : 1) * sizeof(u16));
    if (!u) return;
    utf8_decode_units(s, len, u);
    for (i = 0; i < n; i++) sb_unit(b, u[i]);
    free(u);
}
AStr *sb_done(VM *vm, SBuf *b) { AStr *s = str_from_units(vm, b->u, b->n); free(b->u); b->u = NULL; b->n = b->cap = 0; return s; }
void sb_free(SBuf *b) { free(b->u); b->u = NULL; b->n = b->cap = 0; }

/* ---------------------------------------------------------------- */
/* numbers -> strings (Flash's AVM1 algorithm, rounding bugs included) */

static double decimal_shift(double value, int exp) {
    double base = 10.0;
    if (exp > 0) {
        while (exp > 0) { if (exp & 1) value *= base; exp >>= 1; base *= base; }
    } else {
        unsigned int e = (unsigned int)(-(long)exp);
        while (e > 0) { if (e & 1) value /= base; e >>= 1; base *= base; }
    }
    return value;
}

AStr *str_int(VM *vm, s32 i) {
    char b[16];
    if (i >= 0 && i < 256 && vm->small_int[i]) return vm->small_int[i];
    sprintf(b, "%ld", (long)i);
    return str_latin1(vm, (const u8*)b, strlen(b));
}

AStr *str_num(VM *vm, double n) {
    char buf[64]; int len = 0, is_negative = 0, exp, exp_base2, i, start = 0;
    double mantissa;
    if (n != n) return vm->s_NaN;
    if (n > 1.7976931348623157e308) return vm->s_Infinity;
    if (n < -1.7976931348623157e308) return vm->s_mInfinity;
    if (n == 0.0) return str_int(vm, 0);
    if (n >= -2147483648.0 && n <= 2147483647.0 && n == floor(n)) return str_int(vm, (s32)n);
    if (n < 0) { n = -n; buf[len++] = '-'; is_negative = 1; }
    {
        union { double d; u64 u; } bits; bits.d = n;
        exp_base2 = (int)((bits.u >> 52) & 0x7ff) - 1023;
        if (exp_base2 == -1023) {
            bits.d = n * 1.801439850948198e16;
            exp_base2 = (int)((bits.u >> 52) & 0x7ff) - 1023 - 54;
        }
    }
    {
        double e = (double)exp_base2 * 0.301029995663981;
        exp = (int)(e < 0 ? -floor(-e + 0.5) : floor(e + 0.5));   /* round half away from zero */
    }
    mantissa = decimal_shift(n, -exp);
    if ((int)mantissa == 0) { exp -= 1; mantissa = decimal_shift(n, -exp); }
    if ((int)mantissa >= 10) { exp += 1; mantissa = decimal_shift(n, -exp); }
#define DIGIT() (i = (int)mantissa, mantissa -= (double)i, mantissa *= 10.0, (char)('0' + i))
    if (exp >= 15) {
        char d = DIGIT(); int k;
        buf[len++] = d; buf[len++] = '.';
        for (k = 0; k < 14; k++) buf[len++] = DIGIT();
    } else if (exp >= 0) {
        int k;
        buf[len++] = '0';
        for (k = 0; k <= exp; k++) buf[len++] = DIGIT();
        buf[len++] = '.';
        for (k = 0; k < 15 - exp - 1; k++) buf[len++] = DIGIT();
        exp = 0;
    } else if (exp >= -5) {
        int k;
        buf[len++] = '0'; buf[len++] = '0'; buf[len++] = '.';
        for (k = 0; k < -exp - 1; k++) buf[len++] = '0';
        for (k = 0; k < 15; k++) buf[len++] = DIGIT();
        exp = 0;
    } else {
        char d; int k;
        buf[len++] = '0';
        d = DIGIT();
        if (d != '0') buf[len++] = d;
        buf[len++] = '.';
        for (k = 0; k < 14; k++) buf[len++] = DIGIT();
    }
    if (DIGIT() >= '5') {
        int k;
        for (k = len - 1; k >= 0; k--) {
            if (buf[k] == '9') buf[k] = '0';
            else if (buf[k] >= '0') { buf[k]++; break; }
        }
    }
#undef DIGIT
    while (len > 0 && buf[len - 1] == '0') len--;
    if (len > 0 && buf[len - 1] == '.') len--;
    if (exp != 0) {
        int pos = 0, rpos;
        while (pos < len && buf[pos] == '0') pos++;
        if (pos) { memmove(buf, buf + pos, (size_t)(len - pos)); len -= pos; }
        if (len == 0) { buf[len++] = '1'; exp += 1; }
        else {
            rpos = len - 1; while (rpos > 0 && buf[rpos] == '0') rpos--;
            if (rpos == 0) { exp += len - 1; len = 1; }
        }
        len += sprintf(buf + len, "e%c%d", exp < 0 ? '-' : '+', exp < 0 ? -exp : exp);
    }
    buf[len] = 0;
    i = is_negative ? 1 : 0;
    if (len > i && buf[i] == '0' && !(len > i + 1 && buf[i + 1] == '.')) {   /* drop a leading zero */
        if (i > 0) buf[i] = buf[i - 1];
        start = 1;
    }
    return str_latin1(vm, (const u8*)buf + start, (size_t)(len - start));
}

/* ---------------------------------------------------------------- */
/* strings -> numbers                                               */

static int sw_ws(u16 c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static int is_digit(u16 c) { return c >= '0' && c <= '9'; }

double str_parse_float(AStr *s, int strict) {
    u32 i = 0, after_sign, p, k;
    int neg = 0, exp; double result = 0.0;
    while (i < s->len && sw_ws(STR_AT(s, i))) i++;
    if (i < s->len && STR_AT(s, i) == '-') { neg = 1; i++; } else if (i < s->len && STR_AT(s, i) == '+') i++;
    after_sign = i; p = i;
    while (p < s->len && is_digit(STR_AT(s, p))) p++;
    exp = (int)(p - after_sign) - 1;
    if (p < s->len && STR_AT(s, p) == '.') { p++; while (p < s->len && is_digit(STR_AT(s, p))) p++; }
    if (p == after_sign) return 0.0 / 0.0;
    if (p < s->len && (STR_AT(s, p) == 'e' || STR_AT(s, p) == 'E')) {
        int eneg = 0; s32 e = 0;
        p++;
        if (p < s->len && STR_AT(s, p) == '-') { eneg = 1; p++; } else if (p < s->len && STR_AT(s, p) == '+') p++;
        while (p < s->len && is_digit(STR_AT(s, p))) { e = (s32)((u32)e * 10u + (u32)(STR_AT(s, p) - '0')); p++; }
        if (eneg) e = (s32)(0u - (u32)e);
        exp = (int)((u32)exp + (u32)e);
    }
    if (strict && p < s->len) return 0.0 / 0.0;
    for (k = after_sign; k < s->len; k++) {
        u16 c = STR_AT(s, k);
        if (is_digit(c)) { result += decimal_shift((double)(c - '0'), exp); exp = (int)((u32)exp - 1u); }
        else if (c == '.') { }
        else break;
    }
    return neg ? -result : result;
}

/* wrapping i32 parse with optional sign (Ruffle's Wrapping<i32>::from_wstr_radix) */
static int parse_i32_radix(AStr *s, u32 from, int radix, s32 *out) {
    u32 i = from; int neg = 0; u32 acc = 0;
    if (i < s->len && STR_AT(s, i) == '-') { neg = 1; i++; } else if (i < s->len && STR_AT(s, i) == '+') i++;
    else if (i >= s->len) return 0;
    if (i >= s->len) return 0;
    for (; i < s->len; i++) {
        u16 c = STR_AT(s, i); int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else return 0;
        if (d >= radix) return 0;
        acc = acc * (u32)radix;
        acc = neg ? acc - (u32)d : acc + (u32)d;
    }
    *out = (s32)acc;
    return 1;
}

static int guess_radix(AStr *s) {
    u32 i = 0;
    if (i < s->len && (STR_AT(s, i) == '+' || STR_AT(s, i) == '-')) i++;
    if (i < s->len && STR_AT(s, i) == '0') {
        u32 k;
        i++;
        if (i < s->len && (STR_AT(s, i) == 'x' || STR_AT(s, i) == 'X')) return 16;
        for (k = i; k < s->len; k++) if (STR_AT(s, k) < '0' || STR_AT(s, k) > '7') break;
        if (k == s->len) return 8;
    }
    return 10;
}

double str_to_num(AStr *s, int swf_version) {
    double r;
    if (swf_version >= 6) {
        int radix = guess_radix(s);
        if (radix != 10) {
            s32 v;
            if (!parse_i32_radix(s, radix == 16 ? 2 : 0, radix, &v)) return 0.0 / 0.0;
            return (double)v;
        }
    }
    r = str_parse_float(s, swf_version >= 5);
    if (swf_version < 5 && r != r) return 0.0;
    return r;
}

int str_to_index(const AStr *s, s32 *out) {
    u32 i; double v = 0;
    if (s->len == 0 || s->len > 10) return 0;
    if (s->len > 1 && STR_AT(s, 0) == '0') return 0;
    for (i = 0; i < s->len; i++) { u16 c = STR_AT(s, i); if (c < '0' || c > '9') return 0; v = v * 10 + (c - '0'); }
    if (v > 2147483647.0) return 0;
    *out = (s32)v;
    return 1;
}
