/* h263.c - Sorenson Spark (Flash's H.263 variant) video decoder.
 *
 * A C89 port of the relevant parts of h263-rs (Ruffle's H.263 decoder,
 * MIT / Apache-2.0): picture header, macroblock and block layers, motion
 * vector prediction, half-pel motion compensation, the floating point
 * IDCT, and the optional deblocking filter.  Only what Sorenson Spark
 * uses is implemented (no PB frames, UMV, advanced prediction, GOBs). */
#include "vdec.h"
#include "image.h"
#include <stdlib.h>
#include <string.h>

#include "h263tab.h"

/* ---------------------------------------------------------------- */
/* bit reader                                                       */

typedef struct { const u8 *p; size_t len, bit; int eof; } BR;

static unsigned br_bits(BR *b, int n) {
    unsigned v = 0;
    while (n-- > 0) {
        size_t byte = b->bit >> 3;
        if (byte >= b->len) { b->eof = 1; v <<= 1; continue; }
        v = (v << 1) | ((b->p[byte] >> (7 - (b->bit & 7))) & 1);
        b->bit++;
    }
    return v;
}
static unsigned br_peek(BR *b, int n) { BR c = *b; return br_bits(&c, n); }
static int br_sbits(BR *b, int n) {
    unsigned v = br_bits(b, n);
    if (n > 0 && (v & (1u << (n - 1)))) return (int)v - (int)(1u << n);
    return (int)v;
}
/* walk a VLC tree: returns the leaf value, -1 on a bad code */
static int br_vlc(BR *b, const unsigned short (*t)[2], int n) {
    int i = 0, guard = 0;
    while (t[i][0] != 0xFFFF) {
        int bit = (int)br_bits(b, 1);
        if (b->eof) return -1;
        i = t[i][bit];
        if (i >= n || ++guard > 32) return -1;
    }
    return t[i][1];
}

/* ---------------------------------------------------------------- */
/* decoder state                                                    */

struct H263Dec {
    int w, h, cw, ch, mbw, mbh;
    u8 *cur[3], *ref[3];       /* Y, Cb, Cr planes (w*h, cw*ch) */
    int have_ref;
    s16 (*mvs)[4][2];          /* per macroblock: 4 vectors (half-pel x, y) */
    u8 *scratch;               /* deblocked copy for output */
    Bitmap *img;
};

H263Dec *h263_create(void) { return (H263Dec*)calloc(1, sizeof(H263Dec)); }

static void planes_free(H263Dec *d) {
    int i;
    for (i = 0; i < 3; i++) { free(d->cur[i]); free(d->ref[i]); d->cur[i] = d->ref[i] = NULL; }
    free(d->mvs); d->mvs = NULL;
    free(d->scratch); d->scratch = NULL;
}
void h263_free(H263Dec *d) {
    if (!d) return;
    planes_free(d);
    bitmap_free(d->img);
    free(d);
}

static int alloc_planes(H263Dec *d, int w, int h) {
    int i;
    if (d->w == w && d->h == h && d->cur[0]) return 1;
    planes_free(d);
    d->w = w; d->h = h; d->cw = (w + 1) / 2; d->ch = (h + 1) / 2;
    d->mbw = (w + 15) / 16; d->mbh = (h + 15) / 16;
    d->have_ref = 0;
    for (i = 0; i < 3; i++) {
        size_t n = i ? (size_t)d->cw * d->ch : (size_t)w * h;
        d->cur[i] = (u8*)calloc(n ? n : 1, 1); d->ref[i] = (u8*)calloc(n ? n : 1, 1);
        if (!d->cur[i] || !d->ref[i]) { planes_free(d); d->w = d->h = 0; return 0; }
    }
    d->mvs = (s16(*)[4][2])calloc((size_t)d->mbw * d->mbh, sizeof(*d->mvs));
    d->scratch = (u8*)malloc((size_t)w * h + 2 * (size_t)d->cw * d->ch);
    if (!d->mvs || !d->scratch) { planes_free(d); d->w = d->h = 0; return 0; }
    return 1;
}

/* ---------------------------------------------------------------- */
/* motion vectors                                                   */

static int median3(int a, int b, int c) {
    if (a > b) { if (c > b) return c > a ? a : c; return b; }
    if (b > c) return c > a ? c : a;
    return b;
}

/* predictor for vector `idx` of macroblock `mb` (h263-rs predict_candidate) */
static void predict(H263Dec *d, int mb, s16 cur[4][2], int idx, int out[2]) {
    int col = mb % d->mbw, line = mb / d->mbw, k;
    int end = col == d->mbw - 1;
    for (k = 0; k < 2; k++) {
        int m1, m2, m3;
        if (idx == 0 || idx == 2) m1 = col == 0 ? 0 : d->mvs[mb - 1][idx + 1][k];
        else m1 = cur[idx - 1][k];
        if (idx <= 1) m2 = line == 0 ? m1 : d->mvs[mb - d->mbw][idx + 2][k];
        else m2 = cur[0][k];
        if (idx <= 1) m3 = end ? 0 : line == 0 ? m1 : d->mvs[mb - d->mbw + 1][2][k];
        else m3 = cur[1][k];
        out[k] = median3(m1, m2, m3);
    }
}

/* add a decoded difference; out of range wraps (standard range +-32) */
static int mv_add(int pred, int mvd) {
    int out = pred + mvd;
    if (out < -32 || out >= 32) {
        int inv = mvd > 0 ? mvd - 64 : mvd < 0 ? mvd + 64 : 0;
        out = pred + inv;
    }
    return out;
}

static int read_mvd(BR *b) {
    int v = br_vlc(b, vlc_mvd, (int)(sizeof(vlc_mvd) / sizeof(vlc_mvd[0])));
    if (v < 0 || v == 255) return 1000;
    return v - 64;
}

/* H.263 table 16: chroma vector from the sum of four luma vectors */
static int chroma_mv(int sum) {
    int whole = (sum >> 4) << 1, frac = sum & 15;
    if (frac <= 2) return whole;
    if (frac >= 14) return whole + 2;
    return whole + 1;
}

/* ---------------------------------------------------------------- */
/* motion compensation (h263-rs gather_block)                       */

static int sample(const u8 *p, int w, int h, int x, int y) {
    if (x < 0) x = 0; if (x >= w) x = w - 1;
    if (y < 0) y = 0; if (y >= h) y = h - 1;
    return p[y * w + x];
}

static void mc_block(const u8 *ref, u8 *dst, int w, int h, int px, int py, int mvx, int mvy) {
    int dx = mvx >> 1, dy = mvy >> 1, ix = mvx & 1, iy = mvy & 1;
    int cols = w - px, rows = h - py, i, j, sx = px + dx, sy = py + dy;
    if (cols > 8) cols = 8; if (rows > 8) rows = 8;
    if (cols <= 0 || rows <= 0) return;
    for (j = 0; j < rows; j++) for (i = 0; i < cols; i++) {
        int u = sx + i, v = sy + j, s;
        int a = sample(ref, w, h, u, v);
        if (!ix && !iy) s = a;
        else {
            int b = sample(ref, w, h, u + 1, v), c = sample(ref, w, h, u, v + 1), e = sample(ref, w, h, u + 1, v + 1);
            if (ix && iy) s = (a + b + c + e + 2) / 4;
            else if (ix) s = (a + b + 1) / 2;
            else s = (a + c + 1) / 2;
        }
        dst[(py + j) * w + px + i] = (u8)s;
    }
}

/* ---------------------------------------------------------------- */
/* inverse DCT (h263-rs idct, float)                                */

static const float basis[8][8] = {
    { 0.70710677f,  0.70710677f,  0.70710677f,  0.70710677f,  0.70710677f,  0.70710677f,  0.70710677f,  0.70710677f },
    { 0.98078525f,  0.8314696f,   0.5555702f,   0.19509023f, -0.19509032f, -0.55557036f, -0.83146966f, -0.9807853f },
    { 0.9238795f,   0.38268343f, -0.38268352f, -0.9238796f,  -0.9238795f,  -0.38268313f,  0.3826836f,   0.92387956f },
    { 0.8314696f,  -0.19509032f, -0.9807853f,  -0.55557f,     0.55557007f,  0.98078525f,  0.19509007f, -0.8314698f },
    { 0.70710677f, -0.70710677f, -0.70710665f,  0.707107f,    0.70710677f, -0.70710725f, -0.70710653f,  0.7071068f },
    { 0.5555702f,  -0.9807853f,   0.19509041f,  0.83146936f, -0.8314698f,  -0.19508928f,  0.9807853f,  -0.55557007f },
    { 0.38268343f, -0.9238795f,   0.92387974f, -0.3826839f,  -0.38268384f,  0.9238793f,  -0.92387974f,  0.3826839f },
    { 0.19509023f, -0.55557f,     0.83146936f, -0.9807852f,   0.98078525f, -0.83147013f,  0.55557114f, -0.19508967f }
};

static const u8 zigzag[64][2] = {   /* (x, y) */
    {0,0},{1,0},{0,1},{0,2},{1,1},{2,0},{3,0},{2,1},{1,2},{0,3},{0,4},{1,3},{2,2},{3,1},{4,0},{5,0},
    {4,1},{3,2},{2,3},{1,4},{0,5},{0,6},{1,5},{2,4},{3,3},{4,2},{5,1},{6,0},{7,0},{6,1},{5,2},{4,3},
    {3,4},{2,5},{1,6},{0,7},{1,7},{2,6},{3,5},{4,4},{5,3},{6,2},{7,1},{7,2},{6,3},{5,4},{4,5},{3,6},
    {2,7},{3,7},{4,6},{5,5},{6,4},{7,3},{7,4},{6,5},{5,6},{4,7},{5,7},{6,6},{7,5},{7,6},{6,7},{7,7}
};

/* add the IDCT of `blk` (row-major [y][x]) to the plane at (px, py) */
static void idct_add(const float *blk, u8 *dst, int w, int h, int px, int py) {
    float tmp[64], out[64];
    int x, y, f, cols = w - px, rows = h - py;
    if (cols > 8) cols = 8; if (rows > 8) rows = 8;
    if (cols <= 0 || rows <= 0) return;
    /* rows: horizontal frequencies -> pixels */
    for (y = 0; y < 8; y++) for (x = 0; x < 8; x++) {
        float s = 0;
        for (f = 0; f < 8; f++) s += blk[y * 8 + f] * basis[f][x];
        tmp[y * 8 + x] = s;
    }
    /* columns */
    for (x = 0; x < 8; x++) for (y = 0; y < 8; y++) {
        float s = 0;
        for (f = 0; f < 8; f++) s += tmp[f * 8 + x] * basis[f][y];
        out[y * 8 + x] = s;
    }
    for (y = 0; y < rows; y++) for (x = 0; x < cols; x++) {
        float t = out[y * 8 + x] / 4.0f;
        int v = (int)(t + (t >= 0 ? 0.5f : -0.5f)), p;
        if (v < -256) v = -256; if (v > 255) v = 255;
        p = dst[(py + y) * w + px + x] + v;
        dst[(py + y) * w + px + x] = (u8)(p < 0 ? 0 : p > 255 ? 255 : p);
    }
}

/* one block: returns 0 on a bitstream error */
static int decode_block(BR *b, int version, int intra, int coded, int quant, u8 *dst, int w, int h, int px, int py) {
    float blk[64]; int idx = 0, any = 0;
    memset(blk, 0, sizeof(blk));
    if (intra) {
        int dc = (int)br_bits(b, 8);
        if (b->eof || dc == 0 || dc == 128) return 0;
        blk[0] = (float)(dc == 255 ? 1024 : dc << 3);
        idx = 1; any = 1;
    }
    while (coded) {
        int v = br_vlc(b, vlc_tcoef, (int)(sizeof(vlc_tcoef) / sizeof(vlc_tcoef[0]))), last, run, level;
        if (v < 0 || v == 0xFFFF) return 0;
        if (v == 0xFFFE) {
            int width = 8;
            if (version == 1) width = br_bits(b, 1) ? 11 : 7;
            last = (int)br_bits(b, 1);
            run = (int)br_bits(b, 6);
            level = br_sbits(b, width);
            if (level == 0) return 0;
        } else {
            last = (v >> 15) & 1; run = (v >> 8) & 0x3F; level = v & 0xFF;
            if (br_bits(b, 1)) level = -level;
        }
        if (b->eof) return 0;
        idx += run;
        if (idx >= 64) { any = 0; break; }          /* h263-rs leaves such a block empty */
        {
            int a = level < 0 ? -level : level, val = quant * (2 * a + 1) + ((quant & 1) ? 0 : -1);
            if (level < 0) val = -val;
            if (val < -2048) val = -2048; if (val > 2047) val = 2047;
            blk[zigzag[idx][1] * 8 + zigzag[idx][0]] = (float)val;
            any = 1;
        }
        idx++;
        coded = !last;
    }
    if (any) idct_add(blk, dst, w, h, px, py);
    return 1;
}

/* ---------------------------------------------------------------- */
/* deblocking (h263-rs-deblock)                                     */

static const u8 quant_to_strength[32] = { 0,1,1,2,2,3,3,4,4,4,5,5,6,6,7,7,7,8,8,8,9,9,9,10,10,10,11,11,11,12,12,12 };

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static void db_process(u8 *A, u8 *B, u8 *C, u8 *D, int strength) {
    int a = *A, b = *B, c = *C, e = *D, dd, d1, d2, ad, lim;
    dd = (a - 4 * b + 4 * c - e) >> 3;
    ad = dd < 0 ? -dd : dd;
    { int r = ad - 2 * (ad - strength > 0 ? ad - strength : 0); if (r < 0) r = 0; d1 = dd < 0 ? -r : r; }
    lim = d1 >> 1; if (lim < 0) lim = -lim;
    d2 = clampi((a - e) >> 2, -lim, lim);
    *A = (u8)(a - d2); *B = (u8)clampi(b + d1, 0, 255); *C = (u8)clampi(c - d1, 0, 255); *D = (u8)(e + d2);
}
static void deblock_plane(u8 *p, int w, int h, int strength) {
    int x, y;
    for (y = 8; y <= h - 2; y += 8)
        for (x = 0; x < w; x++) db_process(&p[(y - 2) * w + x], &p[(y - 1) * w + x], &p[y * w + x], &p[(y + 1) * w + x], strength);
    if (w >= 10)
        for (y = 0; y < h; y++)
            for (x = 8; x + 2 <= w && x + 2 <= 2 + ((w - 2) / 8) * 8; x += 8)
                db_process(&p[y * w + x - 2], &p[y * w + x - 1], &p[y * w + x], &p[y * w + x + 1], strength);
}

/* ---------------------------------------------------------------- */
/* picture                                                          */

int h263_decode(H263Dec *d, const u8 *data, size_t len, int deblocking, Bitmap **out) {
    BR b; int version, fmt, w = 0, h = 0, ptype, deblock_flag, quant, mb, nmb, i;
    static const int fmts[7][2] = { {0,0},{0,0},{352,288},{176,144},{128,96},{320,240},{160,120} };
    if (!d || !data || len < 4) return 0;
    b.p = data; b.len = len; b.bit = 0; b.eof = 0;
    if (br_bits(&b, 17) != 1) return 0;
    version = (int)br_bits(&b, 5);
    if (version > 1) return 0;
    br_bits(&b, 8);                                  /* temporal reference */
    fmt = (int)br_bits(&b, 3);
    if (fmt == 0) { w = (int)br_bits(&b, 8); h = (int)br_bits(&b, 8); }
    else if (fmt == 1) { w = (int)br_bits(&b, 16); h = (int)br_bits(&b, 16); }
    else if (fmt <= 6) { w = fmts[fmt][0]; h = fmts[fmt][1]; }
    else return 0;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return 0;
    ptype = (int)br_bits(&b, 2);                     /* 0 I, 1 P, 2 disposable P */
    if (ptype == 3) return 0;
    deblock_flag = (int)br_bits(&b, 1);
    quant = (int)br_bits(&b, 5);
    while (br_bits(&b, 1)) { br_bits(&b, 8); if (b.eof) return 0; }   /* PEI / PSUPP */
    if (b.eof) return 0;
    if (!alloc_planes(d, w, h)) return 0;
    if (ptype != 0 && !d->have_ref) return 0;
    for (i = 0; i < 3; i++) memset(d->cur[i], 0, i ? (size_t)d->cw * d->ch : (size_t)w * h);
    memset(d->mvs, 0, (size_t)d->mbw * d->mbh * sizeof(*d->mvs));
    nmb = d->mbw * d->mbh;
    for (mb = 0; mb < nmb; mb++) {
        int mbx = (mb % d->mbw) * 16, mby = (mb / d->mbw) * 16, type, cbpc, cbpy, intra, k;
        s16 mv[4][2];
        memset(mv, 0, sizeof(mv));
        if (ptype != 0 && br_bits(&b, 1)) {
            /* not coded: a copy of the reference */
            if (b.eof) break;
            for (k = 0; k < 4; k++) mc_block(d->ref[0], d->cur[0], w, h, mbx + (k & 1) * 8, mby + (k >> 1) * 8, 0, 0);
            mc_block(d->ref[1], d->cur[1], d->cw, d->ch, mbx / 2, mby / 2, 0, 0);
            mc_block(d->ref[2], d->cur[2], d->cw, d->ch, mbx / 2, mby / 2, 0, 0);
            continue;
        }
        for (;;) {
            int v = ptype == 0 ? br_vlc(&b, vlc_mcbpc_i, (int)(sizeof(vlc_mcbpc_i) / sizeof(vlc_mcbpc_i[0])))
                               : br_vlc(&b, vlc_mcbpc_p, (int)(sizeof(vlc_mcbpc_p) / sizeof(vlc_mcbpc_p[0])));
            if (v < 0 || v == 101) goto done;
            if (v == 100) { if (ptype != 0 && br_bits(&b, 1)) { v = -2; } else continue; }
            if (v == -2) { type = -1; cbpc = 0; break; }
            type = v >> 2; cbpc = v & 3;
            break;
        }
        if (type == -1) {   /* stuffing followed by an uncoded flag */
            for (k = 0; k < 4; k++) mc_block(d->ref[0], d->cur[0], w, h, mbx + (k & 1) * 8, mby + (k >> 1) * 8, 0, 0);
            mc_block(d->ref[1], d->cur[1], d->cw, d->ch, mbx / 2, mby / 2, 0, 0);
            mc_block(d->ref[2], d->cur[2], d->cw, d->ch, mbx / 2, mby / 2, 0, 0);
            continue;
        }
        intra = type == 3 || type == 4;
        cbpy = br_vlc(&b, vlc_cbpy, (int)(sizeof(vlc_cbpy) / sizeof(vlc_cbpy[0])));
        if (cbpy < 0 || cbpy == 255) goto done;
        if (!intra) cbpy ^= 15;
        if (type == 1 || type == 4 || type == 5) {
            static const int dq[4] = { -1, -2, 1, 2 };
            quant += dq[br_bits(&b, 2)];
            if (quant < 1) quant = 1; if (quant > 31) quant = 31;
        }
        if (!intra) {
            int nv = (type == 2 || type == 5) ? 4 : 1, p[2];
            for (k = 0; k < nv; k++) {
                int mx = read_mvd(&b), my = read_mvd(&b);
                if (mx == 1000 || my == 1000) goto done;
                predict(d, mb, mv, k, p);
                mv[k][0] = (s16)mv_add(p[0], mx); mv[k][1] = (s16)mv_add(p[1], my);
            }
            if (nv == 1) for (k = 1; k < 4; k++) { mv[k][0] = mv[0][0]; mv[k][1] = mv[0][1]; }
            memcpy(d->mvs[mb], mv, sizeof(mv));
            for (k = 0; k < 4; k++) mc_block(d->ref[0], d->cur[0], w, h, mbx + (k & 1) * 8, mby + (k >> 1) * 8, mv[k][0], mv[k][1]);
            {
                int cx = chroma_mv(mv[0][0] + mv[1][0] + mv[2][0] + mv[3][0]);
                int cy = chroma_mv(mv[0][1] + mv[1][1] + mv[2][1] + mv[3][1]);
                mc_block(d->ref[1], d->cur[1], d->cw, d->ch, mbx / 2, mby / 2, cx, cy);
                mc_block(d->ref[2], d->cur[2], d->cw, d->ch, mbx / 2, mby / 2, cx, cy);
            }
        }
        for (k = 0; k < 4; k++)
            if (!decode_block(&b, version, intra, (cbpy >> (3 - k)) & 1, quant, d->cur[0], w, h, mbx + (k & 1) * 8, mby + (k >> 1) * 8)) goto done;
        if (!decode_block(&b, version, intra, (cbpc >> 1) & 1, quant, d->cur[1], d->cw, d->ch, mbx / 2, mby / 2)) goto done;
        if (!decode_block(&b, version, intra, cbpc & 1, quant, d->cur[2], d->cw, d->ch, mbx / 2, mby / 2)) goto done;
    }
done:
    /* output (optionally deblocked) */
    {
        const u8 *py = d->cur[0], *pb = d->cur[1], *pr = d->cur[2];
        int use_db = deblocking == VDEBLOCK_LEVEL1 || (deblocking == VDEBLOCK_PACKET && deblock_flag);
        if (use_db && quant_to_strength[quant & 31]) {
            u8 *sy = d->scratch, *sb = sy + (size_t)w * h, *sr = sb + (size_t)d->cw * d->ch;
            int st = quant_to_strength[quant & 31];
            memcpy(sy, py, (size_t)w * h); memcpy(sb, pb, (size_t)d->cw * d->ch); memcpy(sr, pr, (size_t)d->cw * d->ch);
            deblock_plane(sy, w, h, st); deblock_plane(sb, d->cw, d->ch, st); deblock_plane(sr, d->cw, d->ch, st);
            py = sy; pb = sb; pr = sr;
        }
        if (!d->img || d->img->w != w || d->img->h != h) {
            bitmap_free(d->img);
            d->img = (Bitmap*)calloc(1, sizeof(Bitmap));
            if (d->img) { d->img->w = w; d->img->h = h; d->img->px = (u32*)malloc((size_t)w * h * 4); if (!d->img->px) { free(d->img); d->img = NULL; } }
        }
        if (d->img) vdec_yuv420_to_rgba(py, w, pb, pr, d->cw, NULL, 0, w, h, d->img->px);
    }
    /* the picture becomes the reference unless it is disposable */
    if (ptype != 2) {
        for (i = 0; i < 3; i++) { u8 *t = d->ref[i]; d->ref[i] = d->cur[i]; d->cur[i] = t; }
        d->have_ref = 1;
    }
    if (out) *out = d->img;
    return d->img != NULL;
}
