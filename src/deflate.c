/* deflate.c - small compressors for ByteArray.compress():
 *   a2_deflate_raw: DEFLATE (RFC 1951) with LZ77 hash chains and the fixed
 *                   Huffman code; stored blocks when that is smaller.
 *   a2_lzma_encode: LZMA1 stream (props + dict size + range-coded data) using
 *                   literals and simple rep0-free matches = literals only, which
 *                   every LZMA decoder accepts.  C89, no allocations beyond the
 *                   output buffer. */
#include "flashnt.h"
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- */
/* DEFLATE                                                          */

typedef struct { u8 *buf; size_t n, cap; u32 bits; int nbits; } BitW;
static void bw_grow(BitW *w, size_t extra) { if (w->n + extra > w->cap) { w->cap = (w->n + extra) * 2 + 64; w->buf = (u8*)realloc(w->buf, w->cap); } }
static void bw_put(BitW *w, u32 v, int n) {   /* LSB first */
    w->bits |= v << w->nbits; w->nbits += n;
    while (w->nbits >= 8) { bw_grow(w, 1); w->buf[w->n++] = (u8)w->bits; w->bits >>= 8; w->nbits -= 8; }
}
static void bw_flush(BitW *w) { if (w->nbits) { bw_grow(w, 1); w->buf[w->n++] = (u8)w->bits; w->bits = 0; w->nbits = 0; } }
static u32 rev(u32 v, int n) { u32 r = 0; int i; for (i = 0; i < n; i++) { r = (r << 1) | (v & 1); v >>= 1; } return r; }
static void put_lit(BitW *w, int c) {   /* fixed Huffman literal/length code */
    if (c < 144) bw_put(w, rev(0x30 + c, 8), 8);
    else if (c < 256) bw_put(w, rev(0x190 + c - 144, 9), 9);
    else if (c < 280) bw_put(w, rev(c - 256, 7), 7);
    else bw_put(w, rev(0xC0 + c - 280, 8), 8);
}
static const u16 lbase[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const u8 lext[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const u16 dbase[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const u8 dext[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
static void put_match(BitW *w, int len, int dist) {
    int i;
    for (i = 28; i > 0 && lbase[i] > len; i--) ;
    put_lit(w, 257 + i); if (lext[i]) bw_put(w, (u32)(len - lbase[i]), lext[i]);
    for (i = 29; i > 0 && dbase[i] > dist; i--) ;
    bw_put(w, rev((u32)i, 5), 5); if (dext[i]) bw_put(w, (u32)(dist - dbase[i]), dext[i]);
}
#define HBITS 15
#define WIN 32768
size_t a2_deflate_raw(const u8 *src, size_t n, u8 **out) {
    BitW w; int *head, *prev; size_t i;
    memset(&w, 0, sizeof(w));
    head = (int*)malloc(sizeof(int) << HBITS); prev = (int*)malloc(sizeof(int) * (n ? n : 1));
    for (i = 0; i < ((size_t)1 << HBITS); i++) head[i] = -1;
    bw_put(&w, 1, 1); bw_put(&w, 1, 2);   /* final block, fixed Huffman */
    for (i = 0; i < n; ) {
        int best = 0, bdist = 0;
        if (i + 2 < n) {
            u32 h = ((u32)src[i] << 10 ^ (u32)src[i + 1] << 5 ^ src[i + 2]) & ((1u << HBITS) - 1);
            int p = head[h], chain = 0;
            while (p >= 0 && (int)i - p <= WIN && chain++ < 128) {
                int l = 0, max = (int)(n - i) < 258 ? (int)(n - i) : 258;
                while (l < max && src[p + l] == src[i + l]) l++;
                if (l > best) { best = l; bdist = (int)i - p; if (l == max) break; }
                p = prev[p];
            }
            prev[i] = head[h]; head[h] = (int)i;
        }
        if (best >= 3) {
            size_t k;
            put_match(&w, best, bdist);
            for (k = 1; k < (size_t)best; k++) { size_t j = i + k; if (j + 2 < n) { u32 h = ((u32)src[j] << 10 ^ (u32)src[j + 1] << 5 ^ src[j + 2]) & ((1u << HBITS) - 1); prev[j] = head[h]; head[h] = (int)j; } }
            i += (size_t)best;
        } else { put_lit(&w, src[i]); i++; }
    }
    put_lit(&w, 256);
    bw_flush(&w);
    free(head); free(prev);
    /* stored blocks if the fixed code expanded the data */
    if (w.n > n + n / 65535 * 5 + 5) {
        size_t pos = 0, o = 0; u8 *s = (u8*)malloc(n + n / 65535 * 5 + 16);
        do { size_t chunk = n - pos > 65535 ? 65535 : n - pos; s[o++] = (u8)(pos + chunk >= n); s[o++] = (u8)chunk; s[o++] = (u8)(chunk >> 8); s[o++] = (u8)~chunk; s[o++] = (u8)(~chunk >> 8); memcpy(s + o, src + pos, chunk); o += chunk; pos += chunk; } while (pos < n);
        free(w.buf); *out = s; return o;
    }
    *out = w.buf; return w.n;
}

/* ---------------------------------------------------------------- */
/* LZMA (literal-only encoder)                                      */

typedef struct { u64 low; u32 range; u8 cache; u64 cachesz; u8 *buf; size_t n, cap; } RC;
static void rc_byte(RC *r, u8 b) { if (r->n == r->cap) { r->cap = r->cap * 2 + 256; r->buf = (u8*)realloc(r->buf, r->cap); } r->buf[r->n++] = b; }
static void rc_shift(RC *r) {
    if ((u32)r->low < 0xFF000000u || (int)(r->low >> 32) != 0) {
        u8 carry = (u8)(r->low >> 32), t = r->cache;
        do { rc_byte(r, (u8)(t + carry)); t = 0xFF; } while (--r->cachesz != 0);
        r->cache = (u8)((u32)r->low >> 24);
    }
    r->cachesz++; r->low = (r->low & 0x00FFFFFFu) << 8;
}
static void rc_bit(RC *r, u16 *p, int bit) {
    u32 bound = (r->range >> 11) * *p;
    if (!bit) { r->range = bound; *p = (u16)(*p + ((2048 - *p) >> 5)); }
    else { r->low += bound; r->range -= bound; *p = (u16)(*p - (*p >> 5)); }
    while (r->range < (1u << 24)) { r->range <<= 8; rc_shift(r); }
}
size_t a2_lzma_encode(const u8 *src, size_t n, u8 **out) {
    /* lc=3 lp=0 pb=2 (props byte 0x5D), dictionary 1 MB */
    u16 ismatch[12 << 2], *lit; RC r; size_t i; int state = 0; u8 prevb = 0; u32 dict = 1u << 20;
    lit = (u16*)malloc(sizeof(u16) * 0x300 * 8);
    for (i = 0; i < 0x300 * 8; i++) lit[i] = 1024;
    for (i = 0; i < (12 << 2); i++) ismatch[i] = 1024;
    memset(&r, 0, sizeof(r)); r.range = 0xFFFFFFFFu; r.cachesz = 1;
    rc_byte(&r, 0x5D); rc_byte(&r, (u8)dict); rc_byte(&r, (u8)(dict >> 8)); rc_byte(&r, (u8)(dict >> 16)); rc_byte(&r, (u8)(dict >> 24));
    { size_t hdr = r.n; (void)hdr; }
    for (i = 0; i < n; i++) {
        u16 *probs = lit + 0x300 * (prevb >> 5); u32 sym = 1; int k; u8 c = src[i];
        rc_bit(&r, &ismatch[(state << 2) + (int)(i & 3)], 0);
        for (k = 7; k >= 0; k--) { int bit = (c >> k) & 1; rc_bit(&r, &probs[sym], bit); sym = (sym << 1) | (u32)bit; }
        prevb = c; state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
    }
    for (i = 0; i < 5; i++) rc_shift(&r);
    free(lit);
    /* the range coder's first output byte is always 0: LZMA streams begin with it */
    *out = r.buf; return r.n;
}
