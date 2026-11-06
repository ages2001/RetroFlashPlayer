/* FLASHNT - flash.utils.ByteArray (avmplus ByteArrayGlue/DataIO semantics)
 * and the AMF0 / AMF3 object encodings used by readObject / writeObject,
 * SharedObject and LocalConnection.
 *
 * ByteArray fields on A2Obj: bytes/blen/bcap/bpos, ival = little endian,
 * dval = objectEncoding (0 or 3). */
#include "avm2_int.h"
#include "inflate.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define ARG(i) a2_arg(argc, argv, i)
#define NUM(i) v2_tonum(vm, ARG(i))
#define INT(i) v2_toint(vm, ARG(i))
#define STR(i) v2_tostr(vm, ARG(i))
#define UNUSED (void)vm; (void)self; (void)argc; (void)argv

static int g_default_encoding = 3;
int g_amf0_strict_arrays = 0;   /* AMF0 dense Arrays as strict arrays (NetConnection) */
#define BA_MAX_LEN 0x7FFFFFF0u

/* ---------------------------------------------------------------- */
/* low level buffer                                                 */

static A2Obj *ba_of(V2 self) { return (self.t == T_OBJ && self.u.o->kind == OK_BYTEARRAY) ? self.u.o : NULL; }
static int ba_reserve(VM2 *vm, A2Obj *b, u32 n) {
    u8 *nb; u32 nc;
    if (n <= (u32)b->bcap) return 1;
    if (n > BA_MAX_LEN) { if (vm->player && vm->player->debug) fprintf(stderr, "[avm2] ByteArray of %lu bytes: too large\n", (unsigned long)n); a2_throw_id(vm, "flash.errors::MemoryError", 1000, NULL, NULL); return 0; }
    nc = b->bcap ? (u32)b->bcap : 64; while (nc < n) nc = nc > 0x3FFFFFFFu ? n : nc * 2;
    nb = (u8*)realloc(b->bytes, nc);
    if (!nb) { if (vm->player && vm->player->debug) fprintf(stderr, "[avm2] ByteArray of %lu bytes: out of memory\n", (unsigned long)nc); a2_throw_id(vm, "flash.errors::MemoryError", 1000, NULL, NULL); return 0; }
    b->bytes = nb; b->bcap = (int)nc; return 1;
}
static int ba_setlen(VM2 *vm, A2Obj *b, u32 n) {
    if (!ba_reserve(vm, b, n)) return 0;
    if ((int)n > b->blen) memset(b->bytes + b->blen, 0, n - (u32)b->blen);
    b->blen = (int)n; if (b->bpos > b->blen) b->bpos = b->blen;
    return 1;
}
void a2_bytearray_set(VM2 *vm, A2Obj *o, int i, int v) { if (i < 0) return; if (i >= o->blen && !ba_setlen(vm, o, (u32)i + 1)) return; o->bytes[i] = (u8)v; }
A2Obj *a2_bytearray_new(VM2 *vm) { A2Obj *b = a2_new(vm, vm->c_bytearray); b->kind = OK_BYTEARRAY; b->dval = g_default_encoding; return b; }

static int need(VM2 *vm, A2Obj *b, u32 n) { if (b->bpos < 0 || (u32)(b->blen - (b->bpos > b->blen ? b->blen : b->bpos)) < n || b->bpos > b->blen) { a2_throw_id(vm, "flash.errors::EOFError", 2030, NULL, NULL); return 0; } return 1; }
static u32 rd_be(A2Obj *b, int n) { u32 v = 0; int i; for (i = 0; i < n; i++) v = (v << 8) | b->bytes[b->bpos + i]; b->bpos += n; return v; }
static u32 rd_e(A2Obj *b, int n) { u32 v = 0; int i; if (!b->ival) return rd_be(b, n); for (i = n - 1; i >= 0; i--) v = (v << 8) | b->bytes[b->bpos + i]; b->bpos += n; return v; }
static int wr_raw(VM2 *vm, A2Obj *b, const void *p, u32 n) {
    u32 end = (u32)b->bpos + n;
    if (end < (u32)b->bpos || !ba_reserve(vm, b, end)) { if (!vm->throwing) a2_throw_id(vm, "flash.errors::MemoryError", 1000, NULL, NULL); return 0; }
    if ((int)end > b->blen) { if (b->bpos > b->blen) memset(b->bytes + b->blen, 0, (size_t)(b->bpos - b->blen)); b->blen = (int)end; }
    if (n) memcpy(b->bytes + b->bpos, p, n); b->bpos = (int)end; return 1;
}
static void wr_be(VM2 *vm, A2Obj *b, u32 v, int n) { u8 t[4]; int i; for (i = 0; i < n; i++) t[i] = (u8)(v >> (8 * (n - 1 - i))); wr_raw(vm, b, t, (u32)n); }
void a2_bytearray_write_raw(VM2 *vm, A2Obj *b, const void *p, u32 n) { wr_raw(vm, b, p, n); }
static void wr_e(VM2 *vm, A2Obj *b, u32 v, int n) { u8 t[4]; int i; if (!b->ival) { wr_be(vm, b, v, n); return; } for (i = 0; i < n; i++) t[i] = (u8)(v >> (8 * i)); wr_raw(vm, b, t, (u32)n); }
int a2_ba_write_u32(VM2 *vm, A2Obj *b, u32 v) { wr_e(vm, b, v, 4); return !vm->throwing; }
int a2_ba_read_u32(VM2 *vm, A2Obj *b, u32 *out) { if (!need(vm, b, 4)) return 0; *out = rd_e(b, 4); return 1; }
static void d2u(double d, u32 *hi, u32 *lo) { union { double d; u8 b[8]; } u; u16 t = 1; u.d = d; if (*(u8*)&t) { *lo = u.b[0] | ((u32)u.b[1] << 8) | ((u32)u.b[2] << 16) | ((u32)u.b[3] << 24); *hi = u.b[4] | ((u32)u.b[5] << 8) | ((u32)u.b[6] << 16) | ((u32)u.b[7] << 24); } else { *hi = u.b[3] | ((u32)u.b[2] << 8) | ((u32)u.b[1] << 16) | ((u32)u.b[0] << 24); *lo = u.b[7] | ((u32)u.b[6] << 8) | ((u32)u.b[5] << 16) | ((u32)u.b[4] << 24); } }
static double u2d(u32 hi, u32 lo) { union { double d; u8 b[8]; } u; u16 t = 1; if (*(u8*)&t) { u.b[0] = (u8)lo; u.b[1] = (u8)(lo >> 8); u.b[2] = (u8)(lo >> 16); u.b[3] = (u8)(lo >> 24); u.b[4] = (u8)hi; u.b[5] = (u8)(hi >> 8); u.b[6] = (u8)(hi >> 16); u.b[7] = (u8)(hi >> 24); } else { u.b[7] = (u8)lo; u.b[6] = (u8)(lo >> 8); u.b[5] = (u8)(lo >> 16); u.b[4] = (u8)(lo >> 24); u.b[3] = (u8)hi; u.b[2] = (u8)(hi >> 8); u.b[1] = (u8)(hi >> 16); u.b[0] = (u8)(hi >> 24); } return u.d; }
static void wr_double_be(VM2 *vm, A2Obj *b, double d) { u32 hi, lo; d2u(d, &hi, &lo); wr_be(vm, b, hi, 4); wr_be(vm, b, lo, 4); }
static double rd_double_be(A2Obj *b) { u32 hi = rd_be(b, 4), lo = rd_be(b, 4); return u2d(hi, lo); }

static int check_null(VM2 *vm, V2 v, const char *pname) { if (v.t == T_NULL || v.t == T_UNDEF) { a2_throw_id(vm, "TypeError", 2007, pname, NULL); return 0; } return 1; }

/* ---------------------------------------------------------------- */
/* charsets                                                         */

enum { CS_UTF8, CS_UTF16LE, CS_UTF16BE, CS_LATIN1, CS_ASCII, CS_CP1252, CS_CODEPAGE };
/* platform code page converter (Win32: MultiByteToWideChar); NULL = unavailable.
 * dir 1: bytes -> UTF-16 units, dir 0: UTF-16 -> bytes; returns output count or -1 */
int (*a2_codepage_hook)(int dir, u32 cp, const void *src, int n, void *dst, int cap);
static u32 g_cs_cp;    /* code page of the last CS_CODEPAGE lookup */
static const struct { const char *n; u32 cp; } cs_tab[] = {
    { "shift-jis", 932 }, { "shift_jis", 932 }, { "sjis", 932 }, { "x-sjis", 932 }, { "ms_kanji", 932 }, { "csshiftjis", 932 }, { "windows-31j", 932 },
    { "euc-jp", 20932 }, { "iso-2022-jp", 50220 }, { "gb2312", 936 }, { "gbk", 936 }, { "cp936", 936 }, { "gb18030", 54936 }, { "big5", 950 }, { "big5-hkscs", 950 },
    { "euc-kr", 949 }, { "ks_c_5601-1987", 949 }, { "korean", 949 }, { "iso-2022-kr", 50225 },
    { "windows-1250", 1250 }, { "windows-1251", 1251 }, { "windows-1253", 1253 }, { "windows-1254", 1254 }, { "windows-1255", 1255 }, { "windows-1256", 1256 }, { "windows-1257", 1257 }, { "windows-1258", 1258 }, { "windows-874", 874 },
    { "iso-8859-2", 28592 }, { "iso-8859-3", 28593 }, { "iso-8859-4", 28594 }, { "iso-8859-5", 28595 }, { "iso-8859-6", 28596 }, { "iso-8859-7", 28597 }, { "iso-8859-8", 28598 }, { "iso-8859-9", 28599 }, { "iso-8859-15", 28605 },
    { "koi8-r", 20866 }, { "koi8-u", 21866 }, { "ibm437", 437 }, { "cp437", 437 }, { "ibm850", 850 }, { "ibm866", 866 }, { "macintosh", 10000 }, { "x-mac-japanese", 10001 }, { NULL, 0 } };
static int charset_of(A2Str *s) {
    char b[32]; u32 i;
    if (s->len >= sizeof(b)) return CS_UTF8;
    for (i = 0; i <= s->len; i++) b[i] = (char)(s->s[i] >= 'A' && s->s[i] <= 'Z' ? s->s[i] + 32 : s->s[i]);
    if (!strcmp(b, "utf-8") || !strcmp(b, "utf8")) return CS_UTF8;
    if (!strcmp(b, "unicode") || !strcmp(b, "utf-16") || !strcmp(b, "utf-16le") || !strcmp(b, "utf16") || !strcmp(b, "ucs-2")) return CS_UTF16LE;
    if (!strcmp(b, "utf-16be") || !strcmp(b, "unicodefffe") || !strcmp(b, "unicodefeff")) return CS_UTF16BE;
    if (!strcmp(b, "iso-8859-1") || !strcmp(b, "latin1") || !strcmp(b, "iso_8859-1") || !strcmp(b, "l1")) return CS_LATIN1;
    if (!strcmp(b, "us-ascii") || !strcmp(b, "ascii")) return CS_ASCII;
    if (!strcmp(b, "windows-1252") || !strcmp(b, "cp1252")) return CS_CP1252;
    if (a2_codepage_hook) {
        for (i = 0; cs_tab[i].n; i++) if (!strcmp(b, cs_tab[i].n)) { g_cs_cp = cs_tab[i].cp; return CS_CODEPAGE; }
        g_cs_cp = 0;  /* unknown: the system code page (CP_ACP) */
        return CS_CODEPAGE;
    }
    return CS_UTF8;   /* no converter: UTF-8 */
}
static const u16 cp1252_hi[32] = { 0x20AC,0x81,0x201A,0x0192,0x201E,0x2026,0x2020,0x2021,0x02C6,0x2030,0x0160,0x2039,0x0152,0x8D,0x017D,0x8F,0x90,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x02DC,0x2122,0x0161,0x203A,0x0153,0x9D,0x017E,0x0178 };
extern int a2_utf8_decode(const char *s, u32 len, u32 **out);
extern A2Str *a2_utf8_encode(VM2 *vm, const u32 *u, int n);

/* avmplus non-strict UTF-8 (UnicodeUtils::Utf8ToUtf16): a lead byte from 0xF0
   up starts a 4-byte sequence, encoded surrogates are kept (and pair up),
   overlong or truncated sequences give their bytes as U+0080..U+00FF */
static A2Str *utf8_lenient(VM2 *vm, const u8 *p, u32 n) {
    u32 i = 0, *u; int o = 0; A2Str *r; int clean = 1;
    for (i = 0; i < n; i++) if (p[i] >= 0x80) { clean = 0; break; }
    if (clean) return a2_intern(vm, (const char*)p, n);
    u = (u32*)malloc(((size_t)n * 2 + 1) * sizeof(u32));
    if (!u) return vm->s_empty;
    for (i = 0; i < n; ) {
        u8 c = p[i]; int len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : c >= 0xF0 ? 4 : 0, k, ok = len > 0 && i + (u32)len <= n; u32 cp = 0;
        for (k = 1; ok && k < len; k++) if ((p[i + k] & 0xC0) != 0x80) ok = 0;
        if (ok) {
            if (len == 1) cp = c;
            else if (len == 2) { cp = ((u32)(c & 0x1F) << 6) | (p[i + 1] & 0x3F); if (cp < 0x80) ok = 0; }
            else if (len == 3) { cp = ((u32)(c & 0x0F) << 12) | ((u32)(p[i + 1] & 0x3F) << 6) | (p[i + 2] & 0x3F); if (cp < 0x800) ok = 0; }
            else { cp = ((u32)(c & 0x07) << 18) | ((u32)(p[i + 1] & 0x3F) << 12) | ((u32)(p[i + 2] & 0x3F) << 6) | (p[i + 3] & 0x3F); if (cp < 0x10000) ok = 0; }
        }
        if (ok) {
            if (cp >= 0x10000) { cp -= 0x10000; u[o++] = 0xD800 + (cp >> 10); u[o++] = 0xDC00 + (cp & 0x3FF); }
            else u[o++] = cp;
            i += (u32)len;
        } else { u[o++] = c; i++; }
    }
    r = a2_utf8_encode(vm, u, o); free(u); return r;
}
static A2Str *decode_bytes(VM2 *vm, const u8 *p, u32 n, int cs) {
    u32 *cp, k = 0, i; A2Str *r;
    if (cs == CS_UTF8) { u32 j = 0; while (j < n && p[j]) j++; return utf8_lenient(vm, p, j); }
    if (cs == CS_CODEPAGE) {
        u32 j = 0; u16 *w; int m, k2; u32 *cps; A2Str *res;
        while (j < n && p[j]) j++;
        w = (u16*)malloc((j + 1) * sizeof(u16));
        m = a2_codepage_hook(1, g_cs_cp, p, (int)j, w, (int)j + 1);
        if (m < 0) { free(w); return utf8_lenient(vm, p, j); }
        cps = (u32*)malloc(((size_t)m + 1) * sizeof(u32)); k = 0;
        for (k2 = 0; k2 < m; k2++) { u32 c = w[k2]; if (c >= 0xD800 && c < 0xDC00 && k2 + 1 < m && w[k2 + 1] >= 0xDC00 && w[k2 + 1] < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (w[k2 + 1] - 0xDC00); k2++; } cps[k++] = c; }
        res = a2_utf8_encode(vm, cps, (int)k); free(cps); free(w); return res;
    }
    cp = (u32*)malloc((n + 1) * sizeof(u32));
    if (cs == CS_UTF16LE || cs == CS_UTF16BE) {
        for (i = 0; i + 1 < n; i += 2) {
            u32 c = cs == CS_UTF16LE ? (u32)p[i] | ((u32)p[i + 1] << 8) : ((u32)p[i] << 8) | p[i + 1];
            if (!c) break;
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) { u32 d = cs == CS_UTF16LE ? (u32)p[i + 2] | ((u32)p[i + 3] << 8) : ((u32)p[i + 2] << 8) | p[i + 3]; if (d >= 0xDC00 && d < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); i += 2; } }
            cp[k++] = c;
        }
    } else {
        for (i = 0; i < n && p[i]; i++) { u32 c = p[i]; if (cs == CS_CP1252 && c >= 0x80 && c < 0xA0) c = cp1252_hi[c - 0x80]; cp[k++] = c; }
    }
    r = a2_utf8_encode(vm, cp, (int)k); free(cp); return r;
}
static void encode_string(VM2 *vm, A2Obj *b, A2Str *s, int cs) {
    u32 *cp; int n, i;
    if (cs == CS_UTF8) { wr_raw(vm, b, s->s, s->len); return; }
    n = a2_utf8_decode(s->s, s->len, &cp);
    if (cs == CS_CODEPAGE) {
        u16 *w = (u16*)malloc(((size_t)n * 2 + 1) * sizeof(u16)); int m = 0, r; u8 *o;
        for (i = 0; i < n; i++) { u32 c = cp[i]; if (c >= 0x10000) { c -= 0x10000; w[m++] = (u16)(0xD800 + (c >> 10)); w[m++] = (u16)(0xDC00 + (c & 0x3FF)); } else w[m++] = (u16)c; }
        o = (u8*)malloc((size_t)m * 4 + 4);
        r = a2_codepage_hook(0, g_cs_cp, w, m, o, m * 4 + 4);
        if (r >= 0) wr_raw(vm, b, o, (u32)r); else wr_raw(vm, b, s->s, s->len);
        free(o); free(w); free(cp); return;
    }
    for (i = 0; i < n; i++) {
        u32 c = cp[i];
        if (cs == CS_UTF16LE || cs == CS_UTF16BE) {
            u16 u[2]; int m = 1, k;
            if (c >= 0x10000) { c -= 0x10000; u[0] = (u16)(0xD800 + (c >> 10)); u[1] = (u16)(0xDC00 + (c & 0x3FF)); m = 2; } else u[0] = (u16)c;
            for (k = 0; k < m; k++) { u8 t[2]; if (cs == CS_UTF16LE) { t[0] = (u8)u[k]; t[1] = (u8)(u[k] >> 8); } else { t[0] = (u8)(u[k] >> 8); t[1] = (u8)u[k]; } wr_raw(vm, b, t, 2); }
        } else {
            u8 t = '?';
            if (c < 0x80 || (cs != CS_ASCII && c < 0x100 && !(cs == CS_CP1252 && c >= 0x80 && c < 0xA0))) t = (u8)c;
            else if (cs == CS_CP1252) { int k; for (k = 0; k < 32; k++) if (cp1252_hi[k] == c) { t = (u8)(0x80 + k); break; } }
            wr_raw(vm, b, &t, 1);
        }
    }
    free(cp);
}

/* ---------------------------------------------------------------- */
/* class aliases                                                    */

typedef struct { A2Str *alias; A2Class *cls; } AliasEnt;
static AliasEnt *g_alias; static int g_nalias, g_calias;
void a2_mark_aliases(VM2 *vm) { int i; extern void a2_mark_class(VM2 *vm, A2Class *c); for (i = 0; i < g_nalias; i++) { g_alias[i].alias->gc.mark = 1; a2_mark_class(vm, g_alias[i].cls); } }
A2Class *a2_class_for_alias(VM2 *vm, A2Str *alias) { int i; (void)vm; for (i = g_nalias - 1; i >= 0; i--) if (g_alias[i].alias == alias) return g_alias[i].cls; return NULL; }
static A2Str *alias_for_class(A2Class *c) { int i; for (i = g_nalias - 1; i >= 0; i--) if (g_alias[i].cls == c) { /* a later registration of the same alias for another class wins */ int j; for (j = i + 1; j < g_nalias; j++) if (g_alias[j].alias == g_alias[i].alias) break; if (j == g_nalias) return g_alias[i].alias; } return NULL; }
static V2 g_registerClassAlias(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 c = ARG(1); A2Str *a; (void)self;
    if (!check_null(vm, ARG(0), "aliasName") || !check_null(vm, c, "classObject")) return v2_undef();
    a = STR(0);
    if (c.t == T_OBJ && c.u.o->kind == OK_CLASS) {
        if (g_nalias == g_calias) { g_calias = g_calias ? g_calias * 2 : 16; g_alias = (AliasEnt*)realloc(g_alias, (size_t)g_calias * sizeof(AliasEnt)); }
        g_alias[g_nalias].alias = a; g_alias[g_nalias].cls = c.u.o->as_class; g_nalias++;
    }
    return v2_undef();
}
static V2 g_getClassByAlias(VM2 *vm, V2 self, int argc, V2 *argv) { A2Class *c; (void)self; if (!check_null(vm, ARG(0), "aliasName")) return v2_undef(); c = a2_class_for_alias(vm, STR(0)); if (!c) { a2_throw_id(vm, "ReferenceError", 1014, STR(0)->s, NULL); return v2_undef(); } return v2_obj(c->obj); }

/* ---------------------------------------------------------------- */
/* AMF shared helpers                                               */

typedef struct {
    A2Str **strs; int ns, cs;            /* AMF3 string table */
    A2Obj **objs; int no, co;            /* object table (both encodings) */
    struct Traits { A2Class *cls; A2Str *name; int ext, dyn; A2Str **props; int np; } *tr; int nt, ct;   /* AMF3 traits table */
} AmfCtx;
static void ctx_free(AmfCtx *c) { int i; for (i = 0; i < c->nt; i++) free(c->tr[i].props); free(c->tr); free(c->strs); free(c->objs); }
static int obj_ref(AmfCtx *c, A2Obj *o) { int i; for (i = 0; i < c->no; i++) if (c->objs[i] == o) return i; return -1; }
static void obj_add(AmfCtx *c, A2Obj *o) { if (c->no == c->co) { c->co = c->co ? c->co * 2 : 16; c->objs = (A2Obj**)realloc(c->objs, (size_t)c->co * sizeof(A2Obj*)); } c->objs[c->no++] = o; }

static int is_function(V2 v) { return v.t == T_OBJ && (v.u.o->kind == OK_FUNCTION); }
static A2Class *iext_class(VM2 *vm) { static A2Class *c; if (!c) c = a2_class_find_s(vm, "flash.utils::IExternalizable"); return c; }
static int is_xmldoc(VM2 *vm, A2Obj *o) { static A2Class *c; if (!c) c = a2_class_find_s(vm, "flash.xml::XMLDocument"); return c && o->cls && a2_is_subclass(o->cls, c); }

/* sealed (public, read-write) properties of a class, sorted by name */
typedef struct { A2Str *name; } SealedProp;
static int cmp_str(const void *a, const void *b) { A2Str *x = *(A2Str* const*)a, *y = *(A2Str* const*)b; u32 n = x->len < y->len ? x->len : y->len; int r = memcmp(x->s, y->s, n); return r ? r : (x->len < y->len ? -1 : x->len > y->len); }
static A2Str **sealed_props(VM2 *vm, A2Class *c, int *np) {
    A2Str **v = NULL; int n = 0, cap = 0; A2Trait *t;
    (void)vm;
    if (c && c != vm->c_object) for (t = c->itraits.first; t; t = t->onext) {
        if (t->ns.kind != NS_PACKAGE || t->ns.uri->len) continue;
        if (t->kind == TK_SLOT || t->kind == TK_CONST) { if (t->final_ == 2) continue; }
        else if (t->kind == TK_GETTER || t->kind == TK_SETTER) { if (!t->getter || !t->setter) continue; }
        else continue;
        if (n == cap) { cap = cap ? cap * 2 : 8; v = (A2Str**)realloc(v, (size_t)cap * sizeof(A2Str*)); }
        v[n++] = t->name;
    }
    if (n > 1) qsort(v, (size_t)n, sizeof(A2Str*), cmp_str);
    *np = n; return v;
}
static V2 get_public(VM2 *vm, V2 o, A2Str *name) { A2Name n; V2 r; n.name = name; n.nss = &vm->ns_public; n.nns = 1; n.any_ns = 0; n.attr = 0; if (!a2_get(vm, o, &n, &r)) return v2_undef(); return r; }
/* the trace of an error thrown while restoring a property */
static void set_public_logged(VM2 *vm, V2 o, A2Str *name, V2 v) {
    A2Name n; n.name = name; n.nss = &vm->ns_public; n.nns = 1; n.any_ns = 0; n.attr = 0;
    a2_init(vm, o, &n, v);   /* constants are restored too (initialisation semantics) */
    if (vm->throwing) {
        V2 e = vm->exc; A2Str *s; extern void avm1_trace_out(const char *s);
        vm->throwing = 0; vm->exc = v2_undef();
        s = v2_tostr(vm, e);              /* may itself throw: that propagates */
        if (!vm->throwing) avm1_trace_out(s->s);
    }
}

/* ---------------------------------------------------------------- */
/* AMF3 writer                                                      */

static void w3_u29(VM2 *vm, A2Obj *b, u32 v) {
    u8 t[4]; int n;
    v &= 0x1FFFFFFF;
    if (v < 0x80) { t[0] = (u8)v; n = 1; }
    else if (v < 0x4000) { t[0] = (u8)(0x80 | (v >> 7)); t[1] = (u8)(v & 0x7F); n = 2; }
    else if (v < 0x200000) { t[0] = (u8)(0x80 | (v >> 14)); t[1] = (u8)(0x80 | ((v >> 7) & 0x7F)); t[2] = (u8)(v & 0x7F); n = 3; }
    else { t[0] = (u8)(0x80 | (v >> 22)); t[1] = (u8)(0x80 | ((v >> 15) & 0x7F)); t[2] = (u8)(0x80 | ((v >> 8) & 0x7F)); t[3] = (u8)v; n = 4; }
    wr_raw(vm, b, t, (u32)n);
}
static void w3_str(VM2 *vm, A2Obj *b, AmfCtx *c, A2Str *s) {
    int i;
    if (s->len == 0) { w3_u29(vm, b, 1); return; }
    for (i = 0; i < c->ns; i++) if (c->strs[i] == s) { w3_u29(vm, b, (u32)i << 1); return; }
    if (c->ns == c->cs) { c->cs = c->cs ? c->cs * 2 : 16; c->strs = (A2Str**)realloc(c->strs, (size_t)c->cs * sizeof(A2Str*)); }
    c->strs[c->ns++] = s;
    w3_u29(vm, b, (s->len << 1) | 1); wr_raw(vm, b, s->s, s->len);
}
static int int_fits29(double d) { return d == floor(d) && d >= -268435456.0 && d <= 268435455.0 && !(d == 0 && 1 / d < 0); }
static void w3_value(VM2 *vm, A2Obj *b, AmfCtx *c, V2 v, int depth);
static void w3_dyn_props(VM2 *vm, A2Obj *b, AmfCtx *c, A2Obj *o, int depth) {
    int idx = 0; A2Str *key; V2 val;
    if (o->kind == OK_PROXY) {   /* a Proxy enumerates through nextNameIndex / nextName / nextValue */
        int guard = 0;
        for (;;) {
            V2 nm;
            idx = a2_proxy_next_index(vm, o, idx);
            if (vm->throwing || idx <= 0 || guard++ > 1000000) return;
            nm = a2_proxy_next_name(vm, o, idx); if (vm->throwing) return;
            val = a2_proxy_next_value(vm, o, idx); if (vm->throwing) return;
            if (is_function(val)) continue;
            w3_str(vm, b, c, v2_tostr(vm, nm)); w3_value(vm, b, c, val, depth + 1);
            if (vm->throwing) return;
        }
    }
    {   /* Flash writes an object's index-like keys before its named keys */
        int pass; u32 ix;
        for (pass = 0; pass < 2; pass++) {
            idx = 0;
            for (;;) { a2_dyn_enum(o, &idx, &key, &val); if (!idx) break; if (is_function(val)) continue; if ((o->kind != OK_ARRAY && a2_is_index(key, &ix)) == pass) continue; w3_str(vm, b, c, key); w3_value(vm, b, c, val, depth + 1); if (vm->throwing) return; }
        }
    }
}
/* ObjectEncoding.dynamicPropertyWriter: its writeDynamicProperties(obj, output) chooses the
   dynamic properties through output.writeDynamicProperty(name, value) */
static struct { A2Obj *b; AmfCtx *c; int depth; } g_dpw;
static V2 dpw_write(VM2 *vm, V2 self, int argc, V2 *argv) {
    (void)self;
    if (!g_dpw.b || argc < 2) return v2_undef();
    w3_str(vm, g_dpw.b, g_dpw.c, v2_tostr(vm, argv[0])); if (!vm->throwing) w3_value(vm, g_dpw.b, g_dpw.c, argv[1], g_dpw.depth + 1);
    return v2_undef();
}
static int w3_dyn_custom(VM2 *vm, A2Obj *b, AmfCtx *c, A2Obj *o, int depth) {
    static const char *cn[2] = { "flash.net::ObjectEncoding", "flash.utils::ObjectEncoding" };
    V2 w = v2_undef(), a[2]; A2Obj *out; A2Obj *sb = g_dpw.b; AmfCtx *sc = g_dpw.c; int sd = g_dpw.depth, k;
    for (k = 0; k < 2 && w.t != T_OBJ; k++) {
        A2Class *oe = a2_class_find_s(vm, cn[k]); A2Prop *p;
        if (!oe || !oe->obj) continue;
        p = a2_dyn_find(oe->obj, a2_str(vm, "dynamicPropertyWriter")); if (p) w = p->v;
    }
    if (w.t != T_OBJ) return 0;
    out = a2_new_plain(vm); a2_dyn_set(vm, out, a2_str(vm, "writeDynamicProperty"), v2_obj(a2_new_native_fn(vm, dpw_write, "writeDynamicProperty", 2)));
    g_dpw.b = b; g_dpw.c = c; g_dpw.depth = depth;
    a[0] = v2_obj(o); a[1] = v2_obj(out); a2_call_s(vm, w, "writeDynamicProperties", 2, a);
    g_dpw.b = sb; g_dpw.c = sc; g_dpw.depth = sd;
    return 1;
}
static void w3_value(VM2 *vm, A2Obj *b, AmfCtx *c, V2 v, int depth) {
    A2Obj *o; int r, i;
    if (vm->throwing) return;
    if (depth > 256) { a2_throw_id(vm, "Error", 1023, NULL, NULL); return; }
    switch (v.t) {
    case T_UNDEF: case T_HOLE: wr_be(vm, b, 0, 1); return;
    case T_NULL: wr_be(vm, b, 1, 1); return;
    case T_BOOL: wr_be(vm, b, v.u.b ? 3 : 2, 1); return;
    case T_INT: if (v.u.i >= -268435456 && v.u.i <= 268435455) { wr_be(vm, b, 4, 1); w3_u29(vm, b, (u32)v.u.i); return; } wr_be(vm, b, 5, 1); wr_double_be(vm, b, v.u.i); return;
    case T_UINT: if (v.u.u <= 268435455u) { wr_be(vm, b, 4, 1); w3_u29(vm, b, v.u.u); return; } wr_be(vm, b, 5, 1); wr_double_be(vm, b, v.u.u); return;
    case T_NUM: if (int_fits29(v.u.n)) { wr_be(vm, b, 4, 1); w3_u29(vm, b, (u32)(s32)v.u.n); return; } wr_be(vm, b, 5, 1); wr_double_be(vm, b, v.u.n); return;
    case T_STR: wr_be(vm, b, 6, 1); w3_str(vm, b, c, v.u.s); return;
    default: break;
    }
    o = v.u.o;
    if (o->kind == OK_PRIM && o->prim.t != T_UNDEF) { w3_value(vm, b, c, o->prim, depth); return; }
    if (o->kind == OK_FUNCTION || o->kind == OK_DISPLAY || o->kind == OK_CLASS) { wr_be(vm, b, 0, 1); return; }
    /* marker first; reference check */
    {
        int marker = o->kind == OK_ARRAY ? 9 : o->kind == OK_DATE ? 8 : o->kind == OK_XML ? 11 : o->kind == OK_BYTEARRAY ? 12 : o->kind == OK_DICT ? 17 :
                     o->kind == OK_VECTOR ? (o->vec_type == vm->c_int ? 13 : o->vec_type == vm->c_uint ? 14 : o->vec_type == vm->c_number ? 15 : 16) : (is_xmldoc(vm, o) ? 7 : 10);
        wr_be(vm, b, (u32)marker, 1);
        r = obj_ref(c, o);
        if (r >= 0) { w3_u29(vm, b, (u32)r << 1); return; }
        obj_add(c, o);
        switch (marker) {
        case 8: w3_u29(vm, b, 1); wr_double_be(vm, b, o->dval); return;
        case 7: case 11: { A2Str *s = v2_tostr(vm, marker == 11 ? a2_call_s(vm, v, "toXMLString", 0, NULL) : v); if (vm->throwing) return; w3_u29(vm, b, (s->len << 1) | 1); wr_raw(vm, b, s->s, s->len); return; }
        case 12: w3_u29(vm, b, ((u32)o->blen << 1) | 1); wr_raw(vm, b, o->bytes, (u32)o->blen); return;
        case 9: {
            /* dense part = leading elements 0..k-1 (functions skipped), everything else associative */
            int dense = 0, n = o->alen;
            while (dense < n && o->arr[dense].t != T_HOLE && !is_function(o->arr[dense])) dense++;
            w3_u29(vm, b, ((u32)dense << 1) | 1);
            for (i = dense; i < n; i++) { V2 e = o->arr[i]; char k[16]; if (e.t == T_HOLE || is_function(e)) continue; sprintf(k, "%d", i); w3_str(vm, b, c, a2_str(vm, k)); w3_value(vm, b, c, e, depth + 1); }
            w3_dyn_props(vm, b, c, o, depth);
            w3_u29(vm, b, 1);
            for (i = 0; i < dense; i++) w3_value(vm, b, c, o->arr[i], depth + 1);
            return; }
        case 13: case 14: case 15:
            w3_u29(vm, b, ((u32)o->alen << 1) | 1); wr_be(vm, b, o->vec_fixed ? 1 : 0, 1);
            for (i = 0; i < o->alen; i++) { if (marker == 15) wr_double_be(vm, b, v2_tonum(vm, o->arr[i])); else wr_be(vm, b, marker == 13 ? (u32)v2_toint(vm, o->arr[i]) : v2_touint(vm, o->arr[i]), 4); }
            return;
        case 16: {
            A2Str *tn = o->vec_type ? alias_for_class(o->vec_type) : NULL;
            w3_u29(vm, b, ((u32)o->alen << 1) | 1); wr_be(vm, b, o->vec_fixed ? 1 : 0, 1); w3_str(vm, b, c, tn ? tn : vm->s_empty);
            for (i = 0; i < o->alen; i++) w3_value(vm, b, c, o->arr[i], depth + 1);
            return; }
        case 17:
            w3_u29(vm, b, ((u32)o->dn << 1) | 1); wr_be(vm, b, 0, 1);
            for (i = 0; i < o->dn; i++) { if (o->dkeys[i].t == T_HOLE) continue; w3_value(vm, b, c, o->dkeys[i], depth + 1); w3_value(vm, b, c, o->dvals[i], depth + 1); }
            return;
        default: break;
        }
    }
    /* plain / typed object */
    {
        A2Class *cls = o->cls; A2Str *alias = (cls && cls != vm->c_object) ? alias_for_class(cls) : NULL;
        int ext = cls && iext_class(vm) && a2_is_subclass(cls, iext_class(vm));
        int dyn = !cls || cls->is_dynamic || cls == vm->c_object;
        int ti;
        if (ext && !alias) { a2_throw_id(vm, "ArgumentError", 2004, NULL, NULL); return; }
        for (ti = 0; ti < c->nt; ti++) if (c->tr[ti].cls == cls) break;
        if (ti < c->nt) w3_u29(vm, b, ((u32)ti << 2) | 1);
        else {
            struct Traits t; t.cls = cls; t.name = alias ? alias : vm->s_empty; t.ext = ext; t.dyn = dyn; t.np = 0; t.props = ext ? NULL : sealed_props(vm, cls, &t.np);
            if (c->nt == c->ct) { c->ct = c->ct ? c->ct * 2 : 8; c->tr = (struct Traits*)realloc(c->tr, (size_t)c->ct * sizeof(struct Traits)); }
            c->tr[c->nt++] = t;
            if (ext) w3_u29(vm, b, 7);
            else w3_u29(vm, b, ((u32)t.np << 4) | (dyn ? 0x08 : 0) | 0x03);
            w3_str(vm, b, c, t.name);
            for (i = 0; i < t.np; i++) w3_str(vm, b, c, t.props[i]);
            ti = c->nt - 1;
        }
        if (c->tr[ti].ext) { V2 a[1]; a[0] = v2_obj(b); a2_call_s(vm, v, "writeExternal", 1, a); return; }
        for (i = 0; i < c->tr[ti].np && !vm->throwing; i++) { V2 pv = get_public(vm, v, c->tr[ti].props[i]); if (vm->throwing) return; w3_value(vm, b, c, is_function(pv) ? v2_undef() : pv, depth + 1); }
        if (c->tr[ti].dyn) { if (!w3_dyn_custom(vm, b, c, o, depth)) w3_dyn_props(vm, b, c, o, depth); if (vm->throwing) return; w3_u29(vm, b, 1); }
    }
}

/* ---------------------------------------------------------------- */
/* AMF3 reader                                                      */

static int r3_u8(VM2 *vm, A2Obj *b, u32 *out) { if (!need(vm, b, 1)) return 0; *out = b->bytes[b->bpos++]; return 1; }
static int r3_u29(VM2 *vm, A2Obj *b, u32 *out) {
    u32 v = 0, c; int i;
    for (i = 0; i < 3; i++) { if (!r3_u8(vm, b, &c)) return 0; v = (v << 7) | (c & 0x7F); if (!(c & 0x80)) { *out = v; return 1; } }
    if (!r3_u8(vm, b, &c)) return 0; *out = (v << 8) | c; return 1;
}
static A2Str *r3_str(VM2 *vm, A2Obj *b, AmfCtx *c) {
    u32 h; A2Str *s;
    if (!r3_u29(vm, b, &h)) return NULL;
    if (!(h & 1)) { if ((int)(h >> 1) >= c->ns) { a2_throw_id(vm, "RangeError", 1506, NULL, NULL); return NULL; } return c->strs[h >> 1]; }
    h >>= 1; if (h == 0) return vm->s_empty;
    if (!need(vm, b, h)) return NULL;
    s = a2_intern(vm, (const char*)b->bytes + b->bpos, h); b->bpos += (int)h;
    if (c->ns == c->cs) { c->cs = c->cs ? c->cs * 2 : 16; c->strs = (A2Str**)realloc(c->strs, (size_t)c->cs * sizeof(A2Str*)); }
    c->strs[c->ns++] = s; return s;
}
static V2 r3_value(VM2 *vm, A2Obj *b, AmfCtx *c, int depth);
#define R3_REF(h) do { if (!((h) & 1)) { if ((int)((h) >> 1) >= c->no) { a2_throw_id(vm, "RangeError", 1506, NULL, NULL); return v2_undef(); } return v2_obj(c->objs[(h) >> 1]); } } while (0)
static V2 r3_value(VM2 *vm, A2Obj *b, AmfCtx *c, int depth) {
    u32 m, h, i; A2Obj *o;
    if (vm->throwing) return v2_undef();
    if (depth > 256) { a2_throw_id(vm, "Error", 1023, NULL, NULL); return v2_undef(); }
    if (!r3_u8(vm, b, &m)) return v2_undef();
    switch (m) {
    case 0: return v2_undef();
    case 1: return v2_null();
    case 2: return v2_bool(0);
    case 3: return v2_bool(1);
    case 4: if (!r3_u29(vm, b, &h)) return v2_undef(); if (h & 0x10000000) h |= 0xE0000000; return v2_int((s32)h);
    case 5: if (!need(vm, b, 8)) return v2_undef(); return v2_num(rd_double_be(b));
    case 6: { A2Str *s = r3_str(vm, b, c); return s ? v2_str(s) : v2_undef(); }
    case 8: if (!r3_u29(vm, b, &h)) return v2_undef(); R3_REF(h); if (!need(vm, b, 8)) return v2_undef();
        { V2 a[1]; V2 d; a[0] = v2_num(rd_double_be(b)); d = a2_construct_class(vm, vm->c_date, 1, a); if (d.t == T_OBJ) obj_add(c, d.u.o); return d; }
    case 7: case 11: {
        V2 x, a[1]; if (!r3_u29(vm, b, &h)) return v2_undef(); R3_REF(h); h >>= 1; if (!need(vm, b, h)) return v2_undef();
        a[0] = v2_str(a2_intern(vm, (const char*)b->bytes + b->bpos, h)); b->bpos += (int)h;
        if (m == 11) x = a2_construct_class(vm, vm->c_xml, 1, a);
        else { A2Class *xd = a2_class_find_s(vm, "flash.xml::XMLDocument"); x = xd ? a2_construct_class(vm, xd, 1, a) : a2_construct_class(vm, vm->c_xml, 1, a); }
        if (x.t == T_OBJ) obj_add(c, x.u.o); return x; }
    case 12: if (!r3_u29(vm, b, &h)) return v2_undef(); R3_REF(h); h >>= 1; if (!need(vm, b, h)) return v2_undef();
        o = a2_bytearray_new(vm); obj_add(c, o); if (!ba_setlen(vm, o, h)) return v2_undef(); memcpy(o->bytes, b->bytes + b->bpos, h); b->bpos += (int)h; return v2_obj(o);
    case 9: {
        u32 n; if (!r3_u29(vm, b, &h)) return v2_undef(); R3_REF(h); n = h >> 1;
        o = a2_new_array(vm); obj_add(c, o);
        for (;;) {
            A2Str *k = r3_str(vm, b, c); V2 val; u32 idx;
            if (!k) return v2_undef(); if (k->len == 0) break;
            val = r3_value(vm, b, c, depth + 1); if (vm->throwing) return v2_undef();
            if (a2_is_index(k, &idx)) a2_arr_set_sparse(vm, o, idx, val) || (a2_dyn_set(vm, o, k, val), 1); else a2_dyn_set(vm, o, k, val);
        }
        for (i = 0; i < n; i++) { V2 val = r3_value(vm, b, c, depth + 1); if (vm->throwing) return v2_undef(); a2_arr_set(vm, o, (int)i, val); }
        return v2_obj(o); }
    case 10: {
        struct Traits *t, tmp; V2 ov; A2Class *cls = NULL; int ti;
        if (!r3_u29(vm, b, &h)) return v2_undef(); R3_REF(h);
        if (!(h & 2)) { ti = (int)(h >> 2); if (ti >= c->nt) { a2_throw_id(vm, "RangeError", 1506, NULL, NULL); return v2_undef(); } }
        else {
            A2Str *nm; memset(&tmp, 0, sizeof(tmp));
            tmp.ext = (h >> 2) & 1; tmp.dyn = (h >> 3) & 1; tmp.np = tmp.ext ? 0 : (int)(h >> 4);
            nm = r3_str(vm, b, c); if (!nm) return v2_undef(); tmp.name = nm;
            if (tmp.np) { tmp.props = (A2Str**)calloc((size_t)tmp.np, sizeof(A2Str*)); for (ti = 0; ti < tmp.np; ti++) { tmp.props[ti] = r3_str(vm, b, c); if (!tmp.props[ti]) { free(tmp.props); return v2_undef(); } } }
            if (c->nt == c->ct) { c->ct = c->ct ? c->ct * 2 : 8; c->tr = (struct Traits*)realloc(c->tr, (size_t)c->ct * sizeof(struct Traits)); }
            c->tr[c->nt++] = tmp; ti = c->nt - 1;
        }
        t = &c->tr[ti];
        if (t->name->len) cls = a2_class_for_alias(vm, t->name);
        if (t->ext) {
            if (!cls) { a2_throw_id(vm, "ReferenceError", 1065, t->name->s, NULL); return v2_undef(); }
            if (!iext_class(vm) || !a2_is_subclass(cls, iext_class(vm))) { a2_throw_id(vm, "TypeError", 2173, t->name->s, NULL); return v2_undef(); }
        }
        ov = a2_construct_class(vm, cls ? cls : vm->c_object, 0, NULL);
        if (vm->throwing || ov.t != T_OBJ) return v2_undef();
        obj_add(c, ov.u.o);
        if (t->ext) { V2 a[1]; a[0] = v2_obj(b); a2_call_s(vm, ov, "readExternal", 1, a); return ov; }
        for (i = 0; i < (u32)t->np; i++) { A2Str *pn = c->tr[ti].props[i]; V2 val = r3_value(vm, b, c, depth + 1); if (vm->throwing) return v2_undef(); set_public_logged(vm, ov, pn, val); if (vm->throwing) return v2_undef(); }
        if (c->tr[ti].dyn) for (;;) {
            A2Str *k = r3_str(vm, b, c); V2 val;
            if (!k) return v2_undef(); if (k->len == 0) break;
            val = r3_value(vm, b, c, depth + 1); if (vm->throwing) return v2_undef();
            set_public_logged(vm, ov, k, val); if (vm->throwing) return v2_undef();
        }
        return ov; }
    case 13: case 14: case 15: case 16: {
        u32 n, fixed; A2Class *et = m == 13 ? vm->c_int : m == 14 ? vm->c_uint : m == 15 ? vm->c_number : NULL;
        if (!r3_u29(vm, b, &h)) return v2_undef(); R3_REF(h); n = h >> 1; if (!r3_u8(vm, b, &fixed)) return v2_undef();
        if (m == 16) { A2Str *tn = r3_str(vm, b, c); if (!tn) return v2_undef(); et = tn->len ? a2_class_for_alias(vm, tn) : NULL; if (!et) et = vm->c_object; }
        o = a2_new(vm, a2_vector_class_for(vm, et == vm->c_object ? vm->c_object : et)); o->kind = OK_VECTOR; o->vec_type = et; obj_add(c, o);
        for (i = 0; i < n; i++) {
            V2 val;
            if (m == 16) { val = r3_value(vm, b, c, depth + 1); if (vm->throwing) return v2_undef(); if (val.t == T_UNDEF) val = v2_null(); val = a2_coerce(vm, val, et == vm->c_object ? NULL : et); if (vm->throwing) return v2_undef(); }
            else { if (!need(vm, b, m == 15 ? 8 : 4)) return v2_undef(); val = m == 13 ? v2_int((s32)rd_be(b, 4)) : m == 14 ? v2_uint(rd_be(b, 4)) : v2_num(rd_double_be(b)); }
            a2_arr_push(vm, o, val);
        }
        o->vec_fixed = (int)fixed;
        return v2_obj(o); }
    case 17: {
        u32 n, weak; extern void a2_dict_put(VM2 *vm, A2Obj *o, V2 k, V2 v);
        if (!r3_u29(vm, b, &h)) return v2_undef(); R3_REF(h); n = h >> 1; if (!r3_u8(vm, b, &weak)) return v2_undef();
        o = a2_new(vm, vm->c_dictionary); o->kind = OK_DICT; obj_add(c, o);
        for (i = 0; i < n; i++) { V2 k = r3_value(vm, b, c, depth + 1), val; if (vm->throwing) return v2_undef(); val = r3_value(vm, b, c, depth + 1); if (vm->throwing) return v2_undef(); a2_dict_put(vm, o, k.t == T_OBJ ? k : v2_str(v2_tostr(vm, k)), val); }
        return v2_obj(o); }
    default:
        a2_throw_id(vm, "Error", 2006, NULL, NULL);   /* unknown marker */
        return v2_undef();
    }
}

/* ---------------------------------------------------------------- */
/* AMF0                                                             */

static void w0_value(VM2 *vm, A2Obj *b, AmfCtx *c, V2 v, int depth);
static void w0_utf8(VM2 *vm, A2Obj *b, A2Str *s) { wr_be(vm, b, s->len, 2); wr_raw(vm, b, s->s, s->len); }
static void w0_props(VM2 *vm, A2Obj *b, AmfCtx *c, A2Obj *o, int depth, int sealed) {
    if (sealed) { int np, i; A2Str **p = sealed_props(vm, o->cls, &np); for (i = 0; i < np && !vm->throwing; i++) { V2 pv = get_public(vm, v2_obj(o), p[i]); if (is_function(pv)) continue; w0_utf8(vm, b, p[i]); w0_value(vm, b, c, pv, depth + 1); } free(p); }
    { int idx, pass; u32 ix; A2Str *key; V2 val;   /* index-like keys before named keys, as Flash writes them */
      for (pass = 0; pass < 2; pass++) { idx = 0; for (;;) { a2_dyn_enum(o, &idx, &key, &val); if (!idx || vm->throwing) break; if (is_function(val)) continue; if ((o->kind != OK_ARRAY && a2_is_index(key, &ix)) == pass) continue; w0_utf8(vm, b, key); w0_value(vm, b, c, val, depth + 1); } } }
    wr_be(vm, b, 0, 2); wr_be(vm, b, 9, 1);
}
static void w0_value(VM2 *vm, A2Obj *b, AmfCtx *c, V2 v, int depth) {
    A2Obj *o; int r, i;
    if (vm->throwing) return;
    if (depth > 256) { a2_throw_id(vm, "Error", 1023, NULL, NULL); return; }
    switch (v.t) {
    case T_UNDEF: case T_HOLE: wr_be(vm, b, 6, 1); return;
    case T_NULL: wr_be(vm, b, 5, 1); return;
    case T_BOOL: wr_be(vm, b, 1, 1); wr_be(vm, b, v.u.b ? 1 : 0, 1); return;
    case T_INT: case T_UINT: case T_NUM: wr_be(vm, b, 0, 1); wr_double_be(vm, b, v2_tonum(vm, v)); return;
    case T_STR: if (v.u.s->len > 0xFFFF) { wr_be(vm, b, 12, 1); wr_be(vm, b, v.u.s->len, 4); wr_raw(vm, b, v.u.s->s, v.u.s->len); } else { wr_be(vm, b, 2, 1); w0_utf8(vm, b, v.u.s); } return;
    default: break;
    }
    o = v.u.o;
    if (o->kind == OK_PRIM && o->prim.t != T_UNDEF) { w0_value(vm, b, c, o->prim, depth); return; }
    if (o->kind == OK_FUNCTION || o->kind == OK_DISPLAY || o->kind == OK_CLASS) { wr_be(vm, b, 6, 1); return; }
    if (o->kind == OK_BYTEARRAY || o->kind == OK_VECTOR || o->kind == OK_DICT) { AmfCtx c3; memset(&c3, 0, sizeof(c3)); wr_be(vm, b, 17, 1); w3_value(vm, b, &c3, v, depth); ctx_free(&c3); return; }
    if (o->kind == OK_XML) { A2Str *s = v2_tostr(vm, a2_call_s(vm, v, "toXMLString", 0, NULL)); if (vm->throwing) return; wr_be(vm, b, 15, 1); wr_be(vm, b, s->len, 4); wr_raw(vm, b, s->s, s->len); return; }
    if (o->kind == OK_DATE) { wr_be(vm, b, 11, 1); wr_double_be(vm, b, o->dval); wr_be(vm, b, 0, 2); return; }
    r = obj_ref(c, o);
    if (r >= 0 && r <= 0xFFFF) { wr_be(vm, b, 7, 1); wr_be(vm, b, (u32)r, 2); return; }
    obj_add(c, o);
    if (o->kind == OK_ARRAY && g_amf0_strict_arrays && !o->dcount && !o->biglen) {   /* NetConnection: arrays without named properties are strict arrays (holes are undefined) */
        int n = o->alen;
        wr_be(vm, b, 10, 1); wr_be(vm, b, (u32)n, 4); for (i = 0; i < n && !vm->throwing; i++) { if (o->arr[i].t == T_HOLE) wr_be(vm, b, 6, 1); else w0_value(vm, b, c, o->arr[i], depth + 1); } return;
    }
    if (o->kind == OK_ARRAY) {
        int n = o->alen;
        wr_be(vm, b, 8, 1); wr_be(vm, b, (u32)n, 4);
        for (i = 0; i < n && !vm->throwing; i++) { char k[16]; V2 e = o->arr[i]; if (e.t == T_HOLE || is_function(e)) continue; sprintf(k, "%d", i); w0_utf8(vm, b, a2_str(vm, k)); w0_value(vm, b, c, e, depth + 1); }
        w0_props(vm, b, c, o, depth, 0);
        return;
    }
    if (is_xmldoc(vm, o)) { A2Str *s = v2_tostr(vm, v); if (vm->throwing) return; wr_be(vm, b, 15, 1); wr_be(vm, b, s->len, 4); wr_raw(vm, b, s->s, s->len); return; }
    {
        A2Class *cls = o->cls; A2Str *alias = (cls && cls != vm->c_object) ? alias_for_class(cls) : NULL;
        if (alias) { wr_be(vm, b, 16, 1); w0_utf8(vm, b, alias); } else wr_be(vm, b, 3, 1);
        w0_props(vm, b, c, o, depth, cls && cls != vm->c_object);
    }
}

static V2 r0_value(VM2 *vm, A2Obj *b, AmfCtx *c, int depth);
static A2Str *r0_utf8(VM2 *vm, A2Obj *b, int lng) { u32 n; A2Str *s; if (!need(vm, b, lng ? 4 : 2)) return NULL; n = rd_be(b, lng ? 4 : 2); if (!need(vm, b, n)) return NULL; s = a2_intern(vm, (const char*)b->bytes + b->bpos, n); b->bpos += (int)n; return s; }
/* object properties until the 00 00 09 terminator */
static int r0_props(VM2 *vm, A2Obj *b, AmfCtx *c, V2 target, int depth, int logged) {
    for (;;) {
        A2Str *k = r0_utf8(vm, b, 0); V2 val; u32 idx;
        if (!k) return 0;
        if (k->len == 0) { u32 end; if (!need(vm, b, 1)) return 0; end = b->bytes[b->bpos]; if (end == 9) { b->bpos++; return 1; } }
        val = r0_value(vm, b, c, depth + 1); if (vm->throwing) return 0;
        if (target.u.o->kind == OK_ARRAY && a2_is_index(k, &idx)) { if (!a2_arr_set_sparse(vm, target.u.o, idx, val)) a2_dyn_set(vm, target.u.o, k, val); }
        else if (logged) { set_public_logged(vm, target, k, val); if (vm->throwing) return 0; }
        else a2_dyn_set(vm, target.u.o, k, val);
    }
}
static V2 r0_value(VM2 *vm, A2Obj *b, AmfCtx *c, int depth) {
    u32 m; A2Obj *o;
    if (vm->throwing) return v2_undef();
    if (depth > 256) { a2_throw_id(vm, "Error", 1023, NULL, NULL); return v2_undef(); }
    if (!need(vm, b, 1)) return v2_undef();
    m = b->bytes[b->bpos++];
    switch (m) {
    case 0: if (!need(vm, b, 8)) return v2_undef(); return v2_num(rd_double_be(b));
    case 1: if (!need(vm, b, 1)) return v2_undef(); return v2_bool(b->bytes[b->bpos++] != 0);
    case 2: case 12: { A2Str *s = r0_utf8(vm, b, m == 12); return s ? v2_str(s) : v2_undef(); }
    case 3: o = a2_new_plain(vm); obj_add(c, o); r0_props(vm, b, c, v2_obj(o), depth, 0); return vm->throwing ? v2_undef() : v2_obj(o);
    case 5: return v2_null();
    case 6: case 13: return v2_undef();
    case 7: { u32 r; if (!need(vm, b, 2)) return v2_undef(); r = rd_be(b, 2); if ((int)r >= c->no) { a2_throw_id(vm, "RangeError", 1506, NULL, NULL); return v2_undef(); } return v2_obj(c->objs[r]); }
    case 8: { if (!need(vm, b, 4)) return v2_undef(); rd_be(b, 4); o = a2_new_array(vm); obj_add(c, o); r0_props(vm, b, c, v2_obj(o), depth, 0); return vm->throwing ? v2_undef() : v2_obj(o); }
    case 9: return v2_undef();
    case 10: { u32 n, i; if (!need(vm, b, 4)) return v2_undef(); n = rd_be(b, 4); o = a2_new_array(vm); obj_add(c, o); for (i = 0; i < n; i++) { V2 val = r0_value(vm, b, c, depth + 1); if (vm->throwing) return v2_undef(); a2_arr_push(vm, o, val); } return v2_obj(o); }
    case 11: { V2 a[1]; double d; if (!need(vm, b, 10)) return v2_undef(); d = rd_double_be(b); rd_be(b, 2); a[0] = v2_num(d); return a2_construct_class(vm, vm->c_date, 1, a); }
    case 15: { A2Str *s = r0_utf8(vm, b, 1); V2 a[1]; if (!s) return v2_undef(); a[0] = v2_str(s); return a2_construct_class(vm, vm->c_xml, 1, a); }
    case 16: { A2Str *nm = r0_utf8(vm, b, 0); A2Class *cls; V2 ov; if (!nm) return v2_undef(); cls = a2_class_for_alias(vm, nm); ov = a2_construct_class(vm, cls ? cls : vm->c_object, 0, NULL); if (vm->throwing || ov.t != T_OBJ) return v2_undef(); obj_add(c, ov.u.o); r0_props(vm, b, c, ov, depth, 1); return vm->throwing ? v2_undef() : ov; }
    case 17: { AmfCtx c3; V2 r; memset(&c3, 0, sizeof(c3)); r = r3_value(vm, b, &c3, depth); ctx_free(&c3); return r; }
    default: a2_throw_id(vm, "Error", 2006, NULL, NULL); return v2_undef();
    }
}

/* nested writeObject/readObject calls (IExternalizable) keep the outer reference tables */
static AmfCtx *g_wctx, *g_rctx;
void a2_amf_write(VM2 *vm, A2Obj *b, V2 v, int enc) {
    AmfCtx c, *saved = g_wctx;
    if (saved) { if (enc == 3) w3_value(vm, b, saved, v, 0); else w0_value(vm, b, saved, v, 0); return; }
    memset(&c, 0, sizeof(c)); g_wctx = &c;
    if (enc == 3) w3_value(vm, b, &c, v, 0); else w0_value(vm, b, &c, v, 0);
    g_wctx = NULL; ctx_free(&c);
}
V2 a2_amf_read(VM2 *vm, A2Obj *b, int enc) {
    AmfCtx c, *saved = g_rctx; V2 r;
    if (saved) return enc == 3 ? r3_value(vm, b, saved, 0) : r0_value(vm, b, saved, 0);
    memset(&c, 0, sizeof(c)); g_rctx = &c;
    r = enc == 3 ? r3_value(vm, b, &c, 0) : r0_value(vm, b, &c, 0);
    g_rctx = NULL; ctx_free(&c);
    return r;
}
/* SharedObject helpers: a value <-> standalone AMF3 bytes */
V2 a2_amf_encode(VM2 *vm, V2 v) { A2Obj *b = a2_bytearray_new(vm); a2_amf_write(vm, b, v, 3); return v2_obj(b); }
V2 a2_amf_decode(VM2 *vm, const u8 *data, int len) { A2Obj *b = a2_bytearray_new(vm); V2 r; if (!ba_setlen(vm, b, (u32)len)) return v2_undef(); memcpy(b->bytes, data, (size_t)len); r = a2_amf_read(vm, b, 3); if (vm->throwing) { vm->throwing = 0; vm->exc = v2_undef(); return v2_undef(); } return r; }

/* ---------------------------------------------------------------- */
/* compression                                                      */

static u32 adler32(const u8 *d, size_t n) { u32 a = 1, b = 0; size_t i; for (i = 0; i < n; i++) { a = (a + d[i]) % 65521; b = (b + a) % 65521; } return (b << 16) | a; }
extern size_t a2_deflate_raw(const u8 *src, size_t n, u8 **out);
static void do_compress(VM2 *vm, A2Obj *b, int alg) {   /* 0 zlib 1 deflate 2 lzma */
    size_t n = (size_t)b->blen, o; u8 *raw = NULL, *out; size_t rn;
    extern size_t a2_lzma_encode(const u8 *src, size_t n, u8 **out);
    if (alg == 2) {
        rn = a2_lzma_encode(b->bytes, n, &raw);
        out = (u8*)malloc(rn + 13); memcpy(out, raw, 5);   /* props + dict size */
        { int i; for (i = 0; i < 8; i++) out[5 + i] = (u8)(i < 4 ? (n >> (8 * i)) : 0); }
        memcpy(out + 13, raw + 5, rn - 5); o = rn + 8; free(raw);
    } else {
        rn = a2_deflate_raw(b->bytes, n, &raw);
        out = (u8*)malloc(rn + 8); o = 0;
        if (alg == 0) { out[o++] = 0x78; out[o++] = 0xDA; }
        memcpy(out + o, raw, rn); o += rn; free(raw);
        if (alg == 0) { u32 a = adler32(b->bytes, n); out[o++] = (u8)(a >> 24); out[o++] = (u8)(a >> 16); out[o++] = (u8)(a >> 8); out[o++] = (u8)a; }
    }
    (void)vm;
    free(b->bytes); b->bytes = out; b->blen = (int)o; b->bcap = (int)o; b->bpos = (int)o;
}
static void do_uncompress(VM2 *vm, A2Obj *b, int alg) {
    size_t cap; u8 *out; long got;
    if (b->blen == 0) return;
    if (alg == 2) {
        u32 n; if (b->blen < 13) { a2_throw_id(vm, "flash.errors::IOError", 2058, NULL, NULL); return; }
        n = b->bytes[5] | ((u32)b->bytes[6] << 8) | ((u32)b->bytes[7] << 16) | ((u32)b->bytes[8] << 24);
        if (n > BA_MAX_LEN) { a2_throw_id(vm, "flash.errors::IOError", 2058, NULL, NULL); return; }
        out = (u8*)malloc(n ? n : 1);
        got = swf_lzma_decode(b->bytes, b->bytes + 13, (size_t)b->blen - 13, out, n);
        if (got < 0 || (u32)got != n) { free(out); a2_throw_id(vm, "flash.errors::IOError", 2058, NULL, NULL); return; }
        free(b->bytes); b->bytes = out; b->blen = (int)n; b->bcap = (int)(n ? n : 1); b->bpos = 0; return;
    }
    cap = (size_t)b->blen * 4 + 1024;
    for (;;) {
        out = (u8*)malloc(cap);
        got = alg == 0 ? swf_inflate_zlib(b->bytes, (size_t)b->blen, out, cap) : swf_inflate(b->bytes, (size_t)b->blen, out, cap);
        if (got == FLASHNT_ENOMEM && cap < (1u << 28)) { free(out); cap *= 4; continue; }
        if (got < 0) { free(out); a2_throw_id(vm, "flash.errors::IOError", 2058, NULL, NULL); return; }
        break;
    }
    free(b->bytes); b->bytes = out; b->blen = (int)got; b->bcap = (int)cap; b->bpos = 0;
}
static int algorithm(VM2 *vm, int argc, V2 *argv) {
    A2Str *s;
    if (argc == 0) return 0;
    if (!check_null(vm, argv[0], "algorithm")) return -1;
    s = v2_tostr(vm, argv[0]);
    if (!strcmp(s->s, "zlib")) return 0; if (!strcmp(s->s, "deflate")) return 1; if (!strcmp(s->s, "lzma")) return 2;
    a2_throw_id(vm, "flash.errors::IOError", 2058, NULL, NULL); return -1;
}

/* ---------------------------------------------------------------- */
/* ByteArray methods                                                */

#define BA(b) A2Obj *b = ba_of(self); if (!b) return v2_undef()
void a2_bytearray_preinit(VM2 *vm, A2Obj *o) {
    A2Class *c;
    o->kind = OK_BYTEARRAY; o->ival = 0; o->dval = g_default_encoding;
    /* a subclass linked to DefineBinaryData starts with that data */
    for (c = o->cls; c && c != vm->c_bytearray; c = c->super) if (c->symbol_char > 0 && c->symbol_movie) {
        SWFChar *ch = swf_get_char(c->symbol_movie, c->symbol_char);
        if (ch && ch->type == CHAR_BINARY && ch->len >= 6) { u32 n = (u32)ch->len - 6; if (n && ba_setlen(vm, o, n)) memcpy(o->bytes, ch->data + 6, n); o->bpos = 0; }
        break;
    }
}
static V2 ByteArray_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = self.t == T_OBJ ? self.u.o : NULL; (void)argc; (void)argv;
    if (!o) return v2_undef();
    if (o->kind != OK_BYTEARRAY || (o->blen == 0 && !o->bytes)) a2_bytearray_preinit(vm, o);
    return v2_undef();
}
static V2 ba_length_get(VM2 *vm, V2 self, int argc, V2 *argv) { BA(b); (void)vm; (void)argc; (void)argv; return v2_uint((u32)b->blen); }
static V2 ba_length_set(VM2 *vm, V2 self, int argc, V2 *argv) { BA(b); ba_setlen(vm, b, v2_touint(vm, ARG(0))); return v2_undef(); }
static V2 ba_available(VM2 *vm, V2 self, int argc, V2 *argv) { BA(b); (void)vm; (void)argc; (void)argv; return v2_uint(b->blen > b->bpos ? (u32)(b->blen - b->bpos) : 0); }
static V2 ba_position_get(VM2 *vm, V2 self, int argc, V2 *argv) { BA(b); (void)vm; (void)argc; (void)argv; return v2_uint((u32)b->bpos); }
static V2 ba_position_set(VM2 *vm, V2 self, int argc, V2 *argv) { BA(b); { u32 p = v2_touint(vm, ARG(0)); b->bpos = p > 0x7FFFFFFFu ? 0x7FFFFFFF : (int)p; } return v2_undef(); }
static V2 ba_endian_get(VM2 *vm, V2 self, int argc, V2 *argv) { BA(b); (void)argc; (void)argv; return v2_cstr(vm, b->ival ? "littleEndian" : "bigEndian"); }
static V2 ba_endian_set(VM2 *vm, V2 self, int argc, V2 *argv) { A2Str *s; BA(b); if (!check_null(vm, ARG(0), "endian")) return v2_undef(); s = STR(0); if (!strcmp(s->s, "littleEndian")) b->ival = 1; else if (!strcmp(s->s, "bigEndian")) b->ival = 0; else a2_throw_id(vm, "ArgumentError", 2008, "type", NULL); return v2_undef(); }
static V2 ba_enc_get(VM2 *vm, V2 self, int argc, V2 *argv) { BA(b); (void)vm; (void)argc; (void)argv; return v2_uint((u32)b->dval); }
static V2 ba_enc_set(VM2 *vm, V2 self, int argc, V2 *argv) { u32 e; BA(b); e = v2_touint(vm, ARG(0)); if (e != 0 && e != 3) { a2_throw_id(vm, "ArgumentError", 2008, "objectEncoding", NULL); return v2_undef(); } b->dval = e; return v2_undef(); }
static V2 ba_defenc_get(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_uint((u32)g_default_encoding); }
static V2 ba_defenc_set(VM2 *vm, V2 self, int argc, V2 *argv) { u32 e = v2_touint(vm, ARG(0)); (void)self; if (e != 0 && e != 3) { a2_throw_id(vm, "ArgumentError", 2008, "version", NULL); return v2_undef(); } g_default_encoding = (int)e; return v2_undef(); }
static V2 ba_shareable_get(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_bool(0); }
static V2 ba_noop(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_undef(); }
static V2 ba_clear(VM2 *vm, V2 self, int argc, V2 *argv) { BA(b); (void)vm; (void)argc; (void)argv; free(b->bytes); b->bytes = NULL; b->bcap = 0; b->blen = 0; b->bpos = 0; return v2_undef(); }
static V2 ba_readBoolean(VM2 *vm, V2 self, int argc, V2 *argv) { BA(b); (void)argc; (void)argv; if (!need(vm, b, 1)) return v2_undef(); return v2_bool(b->bytes[b->bpos++] != 0); }
static V2 ba_readByte(VM2 *vm, V2 self, int argc, V2 *argv) { BA(b); (void)argc; (void)argv; if (!need(vm, b, 1)) return v2_undef(); return v2_int((s8)b->bytes[b->bpos++]); }
static V2 ba_readUnsignedByte(VM2 *vm, V2 self, int argc, V2 *argv) { BA(b); (void)argc; (void)argv; if (!need(vm, b, 1)) return v2_undef(); return v2_uint(b->bytes[b->bpos++]); }
static V2 ba_readShort(VM2 *vm, V2 self, int argc, V2 *argv) { BA(b); (void)argc; (void)argv; if (!need(vm, b, 2)) return v2_undef(); return v2_int((s16)rd_e(b, 2)); }
static V2 ba_readUnsignedShort(VM2 *vm, V2 self, int argc, V2 *argv) { BA(b); (void)argc; (void)argv; if (!need(vm, b, 2)) return v2_undef(); return v2_uint(rd_e(b, 2)); }
static V2 ba_readInt(VM2 *vm, V2 self, int argc, V2 *argv) { BA(b); (void)argc; (void)argv; if (!need(vm, b, 4)) return v2_undef(); return v2_int((s32)rd_e(b, 4)); }
static V2 ba_readUnsignedInt(VM2 *vm, V2 self, int argc, V2 *argv) { BA(b); (void)argc; (void)argv; if (!need(vm, b, 4)) return v2_undef(); return v2_uint(rd_e(b, 4)); }
static V2 ba_readFloat(VM2 *vm, V2 self, int argc, V2 *argv) { u32 u; float f; BA(b); (void)argc; (void)argv; if (!need(vm, b, 4)) return v2_undef(); u = rd_e(b, 4); memcpy(&f, &u, 4); return v2_num(f); }
static V2 ba_readDouble(VM2 *vm, V2 self, int argc, V2 *argv) { u32 hi, lo; BA(b); (void)argc; (void)argv; if (!need(vm, b, 8)) return v2_undef(); if (b->ival) { lo = rd_e(b, 4); hi = rd_e(b, 4); } else { hi = rd_be(b, 4); lo = rd_be(b, 4); } return v2_num(u2d(hi, lo)); }
/* readUTFBytes: skip a UTF-8 BOM, stop at NUL, always consume `length` bytes */
static V2 read_utf_bytes(VM2 *vm, A2Obj *b, u32 n) {
    const u8 *p; u32 skip = 0, j; V2 r;
    if (!need(vm, b, n)) return v2_undef();
    p = b->bytes + b->bpos;
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) skip = 3;
    for (j = skip; j < n && p[j]; j++) ;
    r = v2_str(utf8_lenient(vm, p + skip, j - skip));
    b->bpos += (int)n; return r;
}
static V2 ba_readUTFBytes(VM2 *vm, V2 self, int argc, V2 *argv) { BA(b); (void)argc; return read_utf_bytes(vm, b, v2_touint(vm, ARG(0))); }
static V2 ba_readUTF(VM2 *vm, V2 self, int argc, V2 *argv) { BA(b); (void)argc; (void)argv; if (!need(vm, b, 2)) return v2_undef(); return read_utf_bytes(vm, b, rd_e(b, 2)); }
static V2 ba_readMultiByte(VM2 *vm, V2 self, int argc, V2 *argv) {
    u32 n; V2 r; int cs; BA(b);
    if (!check_null(vm, ARG(1), "charSet")) return v2_undef();
    n = v2_touint(vm, ARG(0)); cs = charset_of(STR(1));
    if (!need(vm, b, n)) return v2_undef();
    { const u8 *p = b->bytes + b->bpos; u32 skip = 0;
      if (cs == CS_UTF8 && n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) skip = 3;
      r = v2_str(decode_bytes(vm, p + skip, n - skip, cs)); }
    b->bpos += (int)n; return r;
}
static V2 ba_readBytes(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *d; u32 off, len, avail; BA(b);
    if (!check_null(vm, ARG(0), "bytes")) return v2_undef();
    d = ba_of(ARG(0)); if (!d) return v2_undef();
    off = argc > 1 ? v2_touint(vm, argv[1]) : 0; len = argc > 2 ? v2_touint(vm, argv[2]) : 0;
    avail = b->blen > b->bpos ? (u32)(b->blen - b->bpos) : 0;
    if (len == 0) len = avail;
    if (len > avail) { a2_throw_id(vm, "flash.errors::EOFError", 2030, NULL, NULL); return v2_undef(); }
    if ((u64)off + len > 0xFFFFFFFFu) { a2_throw_id(vm, "RangeError", 2006, NULL, NULL); return v2_undef(); }
    if (off + len >= (u32)d->blen && !ba_setlen(vm, d, off + len)) return v2_undef();
    memmove(d->bytes + off, b->bytes + b->bpos, len); b->bpos += (int)len;
    return v2_undef();
}
static V2 ba_readObject(VM2 *vm, V2 self, int argc, V2 *argv) { BA(b); (void)argc; (void)argv; return a2_amf_read(vm, b, (int)b->dval); }
static V2 ba_writeBoolean(VM2 *vm, V2 self, int argc, V2 *argv) { BA(b); wr_be(vm, b, v2_tobool(vm, ARG(0)) ? 1 : 0, 1); return v2_undef(); }
static V2 ba_writeByte(VM2 *vm, V2 self, int argc, V2 *argv) { BA(b); wr_be(vm, b, (u32)INT(0) & 0xFF, 1); return v2_undef(); }
static V2 ba_writeShort(VM2 *vm, V2 self, int argc, V2 *argv) { BA(b); wr_e(vm, b, (u32)INT(0) & 0xFFFF, 2); return v2_undef(); }
static V2 ba_writeInt(VM2 *vm, V2 self, int argc, V2 *argv) { BA(b); wr_e(vm, b, (u32)INT(0), 4); return v2_undef(); }
static V2 ba_writeUnsignedInt(VM2 *vm, V2 self, int argc, V2 *argv) { BA(b); wr_e(vm, b, v2_touint(vm, ARG(0)), 4); return v2_undef(); }
static V2 ba_writeFloat(VM2 *vm, V2 self, int argc, V2 *argv) { float f; u32 u; BA(b); f = (float)NUM(0); memcpy(&u, &f, 4); wr_e(vm, b, u, 4); return v2_undef(); }
static V2 ba_writeDouble(VM2 *vm, V2 self, int argc, V2 *argv) { u32 hi, lo; BA(b); d2u(NUM(0), &hi, &lo); if (b->ival) { wr_e(vm, b, lo, 4); wr_e(vm, b, hi, 4); } else { wr_be(vm, b, hi, 4); wr_be(vm, b, lo, 4); } return v2_undef(); }
static V2 ba_writeUTFBytes(VM2 *vm, V2 self, int argc, V2 *argv) { BA(b); if (!check_null(vm, ARG(0), "value")) return v2_undef(); { A2Str *s = STR(0); wr_raw(vm, b, s->s, s->len); } return v2_undef(); }
static V2 ba_writeUTF(VM2 *vm, V2 self, int argc, V2 *argv) { A2Str *s; BA(b); if (!check_null(vm, ARG(0), "value")) return v2_undef(); s = STR(0); if (s->len > 65535) { a2_throw_id(vm, "RangeError", 2006, NULL, NULL); return v2_undef(); } wr_e(vm, b, s->len, 2); wr_raw(vm, b, s->s, s->len); return v2_undef(); }
static V2 ba_writeMultiByte(VM2 *vm, V2 self, int argc, V2 *argv) { BA(b); if (!check_null(vm, ARG(0), "value") || !check_null(vm, ARG(1), "charSet")) return v2_undef(); encode_string(vm, b, STR(0), charset_of(STR(1))); return v2_undef(); }
static V2 ba_writeBytes(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *s; u32 off, len, slen; BA(b);
    if (!check_null(vm, ARG(0), "bytes")) return v2_undef();
    s = ba_of(ARG(0)); if (!s) return v2_undef();
    off = argc > 1 ? v2_touint(vm, argv[1]) : 0; len = argc > 2 ? v2_touint(vm, argv[2]) : 0; slen = (u32)s->blen;
    if (off > slen) off = slen;
    if (len == 0) len = slen - off;
    if (len > slen - off) { a2_throw_id(vm, "RangeError", 2006, NULL, NULL); return v2_undef(); }
    if (len) { u8 *tmp = (u8*)malloc(len); memcpy(tmp, s->bytes + off, len); wr_raw(vm, b, tmp, len); free(tmp); }
    return v2_undef();
}
static V2 ba_writeObject(VM2 *vm, V2 self, int argc, V2 *argv) { BA(b); (void)argc; a2_amf_write(vm, b, ARG(0), (int)b->dval); return v2_undef(); }
static V2 ba_toString(VM2 *vm, V2 self, int argc, V2 *argv) {
    const u8 *c; u32 len; A2Obj *b = ba_of(self); (void)argc; (void)argv;
    if (!b) return v2_str(vm->s_empty);
    c = b->bytes; len = (u32)b->blen;
    if (len >= 3 && c[0] == 0xEF && c[1] == 0xBB && c[2] == 0xBF) return v2_str(utf8_lenient(vm, c + 3, len - 3));
    if (len >= 2 && c[0] == 0xFE && c[1] == 0xFF) return v2_str(decode_bytes(vm, c + 2, len - 2, CS_UTF16BE));
    if (len >= 2 && c[0] == 0xFF && c[1] == 0xFE) return v2_str(decode_bytes(vm, c + 2, len - 2, CS_UTF16LE));
    return v2_str(utf8_lenient(vm, c, len));
}
static V2 ba_compress(VM2 *vm, V2 self, int argc, V2 *argv) { int a; BA(b); a = algorithm(vm, argc, argv); if (a < 0) return v2_undef(); if (b->blen) do_compress(vm, b, a); return v2_undef(); }
static V2 ba_uncompress(VM2 *vm, V2 self, int argc, V2 *argv) { int a; BA(b); a = algorithm(vm, argc, argv); if (a < 0) return v2_undef(); do_uncompress(vm, b, a); return v2_undef(); }
static V2 ba_deflate(VM2 *vm, V2 self, int argc, V2 *argv) { BA(b); (void)argc; (void)argv; if (b->blen) do_compress(vm, b, 1); return v2_undef(); }
static V2 ba_inflate(VM2 *vm, V2 self, int argc, V2 *argv) { BA(b); (void)argc; (void)argv; do_uncompress(vm, b, 1); return v2_undef(); }
static V2 ba_cas_int(VM2 *vm, V2 self, int argc, V2 *argv) {
    s32 idx = INT(0), expv = INT(1), nv = INT(2), cur; BA(b); (void)argc;
    if (idx < 0 || idx % 4 || idx + 4 > b->blen) { a2_throw_id(vm, "RangeError", 1506, NULL, NULL); return v2_undef(); }
    cur = (s32)(b->bytes[idx] | ((u32)b->bytes[idx + 1] << 8) | ((u32)b->bytes[idx + 2] << 16) | ((u32)b->bytes[idx + 3] << 24));
    if (cur == expv) { b->bytes[idx] = (u8)nv; b->bytes[idx + 1] = (u8)(nv >> 8); b->bytes[idx + 2] = (u8)(nv >> 16); b->bytes[idx + 3] = (u8)(nv >> 24); }
    return v2_int(cur);
}
static V2 ba_cas_len(VM2 *vm, V2 self, int argc, V2 *argv) { s32 e = INT(0), n = INT(1), cur; BA(b); (void)argc; cur = b->blen; if (cur == e) ba_setlen(vm, b, (u32)n); return v2_int(cur); }

static V2 ba_toJSON(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; (void)argc; (void)argv; return v2_cstr(vm, "ByteArray"); }
void a2_bytearray_init(VM2 *vm) {
    A2Class *c; extern void a2_global_class(VM2 *vm, A2Class *c); extern void a2_sig(VM2 *vm, A2Class *c, const char *name, const char *sig);
    struct { const char *n; NativeFn2 f; int np; } m[] = {
        { "clear", ba_clear, 0 }, { "readBoolean", ba_readBoolean, 0 }, { "readByte", ba_readByte, 0 }, { "readUnsignedByte", ba_readUnsignedByte, 0 }, { "readShort", ba_readShort, 0 }, { "readUnsignedShort", ba_readUnsignedShort, 0 },
        { "readInt", ba_readInt, 0 }, { "readUnsignedInt", ba_readUnsignedInt, 0 }, { "readFloat", ba_readFloat, 0 }, { "readDouble", ba_readDouble, 0 }, { "readUTF", ba_readUTF, 0 }, { "readUTFBytes", ba_readUTFBytes, 1 },
        { "readMultiByte", ba_readMultiByte, 2 }, { "readBytes", ba_readBytes, 3 }, { "readObject", ba_readObject, 0 }, { "writeBoolean", ba_writeBoolean, 1 }, { "writeByte", ba_writeByte, 1 }, { "writeShort", ba_writeShort, 1 },
        { "writeInt", ba_writeInt, 1 }, { "writeUnsignedInt", ba_writeUnsignedInt, 1 }, { "writeFloat", ba_writeFloat, 1 }, { "writeDouble", ba_writeDouble, 1 }, { "writeUTF", ba_writeUTF, 1 }, { "writeUTFBytes", ba_writeUTFBytes, 1 },
        { "writeMultiByte", ba_writeMultiByte, 2 }, { "writeBytes", ba_writeBytes, 3 }, { "writeObject", ba_writeObject, 1 }, { "compress", ba_compress, 1 }, { "uncompress", ba_uncompress, 1 }, { "deflate", ba_deflate, 0 }, { "inflate", ba_inflate, 0 },
        { "toString", ba_toString, 0 }, { "atomicCompareAndSwapIntAt", ba_cas_int, 3 }, { "atomicCompareAndSwapLength", ba_cas_len, 2 }, { NULL, NULL, 0 } };
    int i;
    vm->c_bytearray = c = a2_class_new(vm, "flash.utils", "ByteArray", vm->c_object, OK_BYTEARRAY); c->native_ctor = ByteArray_ctor; c->ctor_sig = "()";
    a2_dyn_set_hidden(vm, c->prototype, a2_str(vm, "toJSON"), v2_obj(a2_new_native_fn(vm, ba_toJSON, "toJSON", 1)));   /* avmplus: "ByteArray" */
    a2_add_getter(vm, c, "length", ba_length_get, ba_length_set); a2_add_getter(vm, c, "bytesAvailable", ba_available, NULL); a2_add_getter(vm, c, "position", ba_position_get, ba_position_set);
    a2_add_getter(vm, c, "endian", ba_endian_get, ba_endian_set); a2_add_getter(vm, c, "objectEncoding", ba_enc_get, ba_enc_set); a2_add_getter(vm, c, "shareable", ba_shareable_get, ba_noop);
    for (i = 0; m[i].n; i++) a2_add_method(vm, c, m[i].n, m[i].f, m[i].np);
    a2_dyn_set_hidden(vm, c->prototype, vm->s_toString, v2_obj(a2_new_native_fn(vm, ba_toString, "toString", 0)));
    { A2Trait *t; extern void a2_tm_put(A2TraitMap *tm, A2Trait *t); t = (A2Trait*)calloc(1, sizeof(A2Trait)); t->name = a2_str(vm, "defaultObjectEncoding"); t->ns = vm->ns_public; t->kind = TK_GETTER; t->getter = a2_native_method(vm, ba_defenc_get, "defaultObjectEncoding", 0); t->setter = a2_native_method(vm, ba_defenc_set, "defaultObjectEncoding", 1); t->getter->owner = t->setter->owner = c; a2_tm_put(&c->ctraits, t); }
    a2_sig(vm, c, "length", "uint"); a2_sig(vm, c, "position", "uint"); a2_sig(vm, c, "bytesAvailable", "uint"); a2_sig(vm, c, "endian", "String"); a2_sig(vm, c, "objectEncoding", "uint");
    a2_global_class(vm, c);
    { A2Trait *t; extern void a2_tm_put(A2TraitMap *tm, A2Trait *t); A2Ns ns; ns.kind = NS_PACKAGE; ns.uri = a2_str(vm, "flash.net");
      t = (A2Trait*)calloc(1, sizeof(A2Trait)); t->name = a2_str(vm, "registerClassAlias"); t->ns = ns; t->kind = TK_METHOD; t->method = a2_native_method(vm, g_registerClassAlias, "registerClassAlias", 2); a2_tm_put(&vm->global->cls->itraits, t);
      t = (A2Trait*)calloc(1, sizeof(A2Trait)); t->name = a2_str(vm, "getClassByAlias"); t->ns = ns; t->kind = TK_METHOD; t->method = a2_native_method(vm, g_getClassByAlias, "getClassByAlias", 1); a2_tm_put(&vm->global->cls->itraits, t); }
}

/* index access ba[i] (from the property protocol) */
int a2_bytearray_get_index(A2Obj *o, u32 i, V2 *out) { *out = i < (u32)o->blen ? v2_int(o->bytes[i]) : v2_undef(); return 1; }
