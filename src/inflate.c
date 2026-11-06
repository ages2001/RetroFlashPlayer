/* inflate.c - compact DEFLATE decoder, after Mark Adler's "puff"
 * (zlib/contrib, public domain).  Rewritten in C89 for FLASHNT. */
#include "inflate.h"
#include <string.h>

#define MAXBITS   15
#define MAXLCODES 286
#define MAXDCODES 30
#define MAXCODES  (MAXLCODES + MAXDCODES)
#define FIXLCODES 288

typedef struct {
    const u8 *in;
    size_t    inlen;
    size_t    incnt;
    u32       bitbuf;       /* up to 32 buffered input bits, LSB first */
    int       bitcnt;
    u8       *out;
    size_t    outcap;
    size_t    outcnt;
} State;

/* Codes of up to FASTBITS bits decode with one table lookup; longer ones
 * (rare) walk the canonical code bit by bit, as puff does. */
#define FASTBITS 9

typedef struct {
    short *count;   /* number of symbols of each length */
    short *symbol;  /* canonically ordered symbols */
    u16    fast[1 << FASTBITS];   /* (symbol << 4) | length, 0 = slow path */
} Huffman;

/* top up the bit buffer with whole input bytes (never past the input) */
static void refill(State *s) {
    while (s->bitcnt <= 24 && s->incnt < s->inlen) {
        s->bitbuf |= (u32)s->in[s->incnt++] << s->bitcnt;
        s->bitcnt += 8;
    }
}

/* read need (0..16) bits, LSB-first; -1 when the input runs out */
static int bits(State *s, int need) {
    int v;
    if (s->bitcnt < need) { refill(s); if (s->bitcnt < need) return -1; }
    v = (int)(s->bitbuf & ((1UL << need) - 1));
    s->bitbuf = need >= 32 ? 0 : s->bitbuf >> need;
    s->bitcnt -= need;
    return v;
}

static int stored(State *s) {
    unsigned len;
    /* discard to the byte boundary and give back whole buffered bytes */
    s->incnt -= (size_t)(s->bitcnt >> 3);
    s->bitbuf = 0; s->bitcnt = 0;
    if (s->incnt + 4 > s->inlen) return 2;
    len  = s->in[s->incnt++];
    len |= s->in[s->incnt++] << 8;
    s->incnt += 2;                                /* skip one's complement */
    if (s->incnt + len > s->inlen) return 2;
    if (s->outcnt + len > s->outcap) return 1;
    memcpy(s->out + s->outcnt, s->in + s->incnt, len);
    s->outcnt += len; s->incnt += len;
    return 0;
}

static int decode_slow(State *s, const Huffman *h) {
    int code = 0, first = 0, index = 0, len = 1, count;
    while (len <= MAXBITS) {
        int b = bits(s, 1);
        if (b < 0) return -10;
        code |= b;
        count = h->count[len];
        if (code - first < count) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code  <<= 1;
        len++;
    }
    return -9;
}

static int decode(State *s, const Huffman *h) {
    unsigned e;
    if (s->bitcnt < FASTBITS) refill(s);
    if (s->bitcnt >= FASTBITS) {
        e = h->fast[s->bitbuf & ((1u << FASTBITS) - 1)];
        if (e) { s->bitbuf >>= (e & 15); s->bitcnt -= (int)(e & 15); return (int)(e >> 4); }
    }
    return decode_slow(s, h);
}

static int construct(Huffman *h, const short *length, int n) {
    int symbol, len, left, idx, j;
    unsigned code;
    short offs[MAXBITS + 1];
    for (len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (symbol = 0; symbol < n; symbol++) h->count[length[symbol]]++;
    memset(h->fast, 0, sizeof(h->fast));
    if (h->count[0] == n) return 0;               /* all zero -> complete */
    left = 1;
    for (len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;                /* over-subscribed */
    }
    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++)
        offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (symbol = 0; symbol < n; symbol++)
        if (length[symbol] != 0) h->symbol[offs[length[symbol]]++] = (short)symbol;
    /* lookup table: canonical codes, bit-reversed (the stream is LSB first) */
    code = 0; idx = 0;
    for (len = 1; len <= FASTBITS; len++) {
        for (j = 0; j < h->count[len]; j++, idx++, code++) {
            unsigned r = 0, c = code, k;
            for (k = 0; k < (unsigned)len; k++) { r = (r << 1) | (c & 1); c >>= 1; }
            for (; r < (1u << FASTBITS); r += 1u << len)
                h->fast[r] = (u16)((h->symbol[idx] << 4) | len);
        }
        code <<= 1;
    }
    return left;                                  /* >0 means incomplete */
}

static const short lens_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const short lens_ext[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const short dist_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const short dist_ext[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static int inflate_codes(State *s, const Huffman *lc, const Huffman *dc) {
    int sym;
    do {
        sym = decode(s, lc);
        if (sym < 0) return sym;
        if (sym < 256) {
            if (s->outcnt == s->outcap) return 1;
            s->out[s->outcnt++] = (u8)sym;
        } else if (sym > 256) {
            unsigned len, dist;
            size_t from;
            sym -= 257;
            if (sym >= 29) return -11;
            {   int x = bits(s, lens_ext[sym]); if (x < 0) return -10; len = lens_base[sym] + x; }
            sym = decode(s, dc);
            if (sym < 0) return sym;
            if (sym >= 30) return -11;
            {   int x = bits(s, dist_ext[sym]); if (x < 0) return -10; dist = dist_base[sym] + x; }
            if (dist > s->outcnt) return -12;
            if (s->outcnt + len > s->outcap) return 1;
            from = s->outcnt - dist;
            {   u8 *o = s->out + s->outcnt, *f = s->out + from;
                s->outcnt += len;
                while (len--) *o++ = *f++;   /* overlapping copies repeat */
            }
        }
    } while (sym != 256);
    return 0;
}

static int fixed_block(State *s) {
    static short lencode_sym[FIXLCODES], lencode_cnt[MAXBITS + 1];
    static short distcode_sym[MAXDCODES], distcode_cnt[MAXBITS + 1];
    static int   built = 0;
    static Huffman lc, dc;
    if (!built) {
        short lengths[FIXLCODES];
        int sym;
        for (sym = 0;   sym < 144; sym++) lengths[sym] = 8;
        for (;          sym < 256; sym++) lengths[sym] = 9;
        for (;          sym < 280; sym++) lengths[sym] = 7;
        for (;          sym < 288; sym++) lengths[sym] = 8;
        lc.count = lencode_cnt; lc.symbol = lencode_sym;
        construct(&lc, lengths, FIXLCODES);
        for (sym = 0; sym < MAXDCODES; sym++) lengths[sym] = 5;
        dc.count = distcode_cnt; dc.symbol = distcode_sym;
        construct(&dc, lengths, MAXDCODES);
        built = 1;
    }
    return inflate_codes(s, &lc, &dc);
}

static int dynamic_block(State *s) {
    static const short order[19] =
        {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    int nlen, ndist, ncode, index, err;
    short lengths[MAXCODES];
    short lencnt[MAXBITS + 1], lensym[MAXLCODES];
    short distcnt[MAXBITS + 1], distsym[MAXDCODES];
    Huffman lc, dc;
    lc.count = lencnt; lc.symbol = lensym;
    dc.count = distcnt; dc.symbol = distsym;

    nlen  = bits(s, 5) + 257;
    ndist = bits(s, 5) + 1;
    ncode = bits(s, 4) + 4;
    if (nlen > MAXLCODES || ndist > MAXDCODES) return -3;
    for (index = 0; index < ncode; index++) lengths[order[index]] = (short)bits(s, 3);
    for (; index < 19; index++) lengths[order[index]] = 0;
    err = construct(&lc, lengths, 19);
    if (err != 0) return -4;
    index = 0;
    while (index < nlen + ndist) {
        int sym = decode(s, &lc);
        if (sym < 0) return sym;
        if (sym < 16) {
            lengths[index++] = (short)sym;
        } else {
            int len = 0;
            if (sym == 16) {
                if (index == 0) return -5;
                len = lengths[index - 1];
                sym = 3 + bits(s, 2);
            } else if (sym == 17) {
                sym = 3 + bits(s, 3);
            } else {
                sym = 11 + bits(s, 7);
            }
            if (index + sym > nlen + ndist) return -6;
            while (sym--) lengths[index++] = (short)len;
        }
    }
    if (lengths[256] == 0) return -9;
    err = construct(&lc, lengths, nlen);
    if (err < 0) return -7;
    err = construct(&dc, lengths + nlen, ndist);
    if (err < 0) return -8;
    return inflate_codes(s, &lc, &dc);
}

long swf_inflate(const u8 *src, size_t srclen, u8 *out, size_t outcap) {
    State s;
    int last, type, err;
    s.in = src; s.inlen = srclen; s.incnt = 0;
    s.bitbuf = 0; s.bitcnt = 0;
    s.out = out; s.outcap = outcap; s.outcnt = 0;
    do {
        last = bits(&s, 1);
        type = bits(&s, 2);
        if (last < 0 || type < 0) return FLASHNT_EFMT;
        if (type == 0)      err = stored(&s);
        else if (type == 1) err = fixed_block(&s);
        else if (type == 2) err = dynamic_block(&s);
        else                return FLASHNT_EFMT;
        if (err != 0) return (err > 0) ? FLASHNT_ENOMEM : FLASHNT_EFMT;
    } while (!last);
    return (long)s.outcnt;
}

long swf_inflate_zlib(const u8 *src, size_t srclen, u8 *out, size_t outcap) {
    size_t off = 2;
    if (srclen < 2) return FLASHNT_EFMT;
    if ((src[0] & 0x0f) != 8) return FLASHNT_EFMT;       /* not deflate */
    if (src[1] & 0x20) off += 4;                        /* FDICT present */
    return swf_inflate(src + off, srclen - off, out, outcap);
}
