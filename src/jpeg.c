/* jpeg.c - baseline (sequential, Huffman) JPEG decoder for SWF bitmaps
 * plus the SWF-specific wrappers (DefineBits + JPEGTables, JPEG2/3/4
 * with zlib alpha plane, PNG payloads inside DefineBitsJPEG2). */
#include "image.h"
#include "inflate.h"
#include "jxr.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---------------------------------------------------------------- */

typedef struct {
    u8  bits[17];     /* counts per length */
    u8  vals[256];
    int mincode[17], maxcode[18], valptr[17];
} HuffTable;

typedef struct {
    int id, h, v, tq;
    int td, ta;
    int dc_pred;
    int bw, bh;              /* blocks per row / column actually used (ceil(comp dims / 8)) */
    int pbw, pbh;            /* padded to MCU multiples */
    int *coef;               /* pbw*pbh*64 coefficients */
    u8 *plane; int pw, ph;   /* component plane after IDCT */
} Component;

typedef struct {
    const u8 *p, *end;
    u32 bitbuf; int bitcnt;
    int marker_hit;
    u16 qt[4][64];
    HuffTable hdc[4], hac[4];
    Component comp[4]; int ncomp;
    int width, height;
    int hmax, vmax, mcux, mcuy;
    int restart_interval;
    int progressive;
    int adobe_transform;
    int eobrun;
    /* current scan */
    int scomp[4], nscomp, ss, se, ah, al;
} JDec;

static const u8 zigzag[64] = {
    0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63 };

static int rd8(JDec *j) { return (j->p < j->end) ? *j->p++ : 0; }
static int rd16(JDec *j) { int a = rd8(j); return (a << 8) | rd8(j); }

static void build_huff(HuffTable *h) {
    int code = 0, k = 0, l;
    for (l = 1; l <= 16; l++) {
        h->valptr[l] = k;
        h->mincode[l] = code;
        code += h->bits[l];
        k += h->bits[l];
        h->maxcode[l] = h->bits[l] ? code - 1 : -1;
        code <<= 1;
    }
    h->maxcode[17] = 0x7fffffff;
}

static void fill_bits(JDec *j) {
    while (j->bitcnt <= 24) {
        int b = 0;
        if (!j->marker_hit && j->p < j->end) {
            b = *j->p++;
            if (b == 0xFF) {
                int b2 = (j->p < j->end) ? *j->p : 0;
                if (b2 == 0) j->p++;
                else { j->p--; j->marker_hit = 1; b = 0; }
            }
        }
        j->bitbuf |= (u32)b << (24 - j->bitcnt);
        j->bitcnt += 8;
    }
}
static int get_bit(JDec *j) { int b; if (j->bitcnt < 1) fill_bits(j); b = (j->bitbuf >> 31) & 1; j->bitbuf <<= 1; j->bitcnt--; return b; }
static int get_bits(JDec *j, int n) { int v; if (n == 0) return 0; if (j->bitcnt < n) fill_bits(j); v = (int)(j->bitbuf >> (32 - n)); j->bitbuf <<= n; j->bitcnt -= n; return v; }
static int extend(int v, int n) { return (n && v < (1 << (n - 1))) ? v - (1 << n) + 1 : v; }

static int decode_huff(JDec *j, HuffTable *h) {
    int code = 0, l;
    for (l = 1; l <= 16; l++) {
        code = (code << 1) | get_bit(j);
        if (h->maxcode[l] >= 0 && code <= h->maxcode[l] && code >= h->mincode[l])
            return h->vals[h->valptr[l] + code - h->mincode[l]];
    }
    return 0;
}

/* libjpeg's jpeg_idct_islow (accurate integer IDCT, 13 + 2 bits) */
#define IDCT_CB 13
#define IDCT_P1 2
#define IDCT_DESCALE(x, n) (((x) + ((long)1 << ((n) - 1))) >> (n))
static void idct_block(const int *in, u8 *out, int stride) {
    long ws[64], tmp0, tmp1, tmp2, tmp3, tmp10, tmp11, tmp12, tmp13, z1, z2, z3, z4, z5; int i;
    for (i = 0; i < 8; i++) {
        const int *c = in + i; long *w = ws + i;
        if (!c[8] && !c[16] && !c[24] && !c[32] && !c[40] && !c[48] && !c[56]) {
            long dc = (long)c[0] << IDCT_P1;
            w[0] = w[8] = w[16] = w[24] = w[32] = w[40] = w[48] = w[56] = dc;
            continue;
        }
        z2 = c[16]; z3 = c[48];
        z1 = (z2 + z3) * 4433; tmp2 = z1 + z3 * (-15137); tmp3 = z1 + z2 * 6270;
        z2 = c[0]; z3 = c[32];
        tmp0 = (z2 + z3) * 8192; tmp1 = (z2 - z3) * 8192;
        tmp10 = tmp0 + tmp3; tmp13 = tmp0 - tmp3; tmp11 = tmp1 + tmp2; tmp12 = tmp1 - tmp2;
        tmp0 = c[56]; tmp1 = c[40]; tmp2 = c[24]; tmp3 = c[8];
        z1 = tmp0 + tmp3; z2 = tmp1 + tmp2; z3 = tmp0 + tmp2; z4 = tmp1 + tmp3;
        z5 = (z3 + z4) * 9633;
        tmp0 *= 2446; tmp1 *= 16819; tmp2 *= 25172; tmp3 *= 12299;
        z1 *= -7373; z2 *= -20995; z3 *= -16069; z4 *= -3196;
        z3 += z5; z4 += z5;
        tmp0 += z1 + z3; tmp1 += z2 + z4; tmp2 += z2 + z3; tmp3 += z1 + z4;
        w[0] = IDCT_DESCALE(tmp10 + tmp3, IDCT_CB - IDCT_P1); w[56] = IDCT_DESCALE(tmp10 - tmp3, IDCT_CB - IDCT_P1);
        w[8] = IDCT_DESCALE(tmp11 + tmp2, IDCT_CB - IDCT_P1); w[48] = IDCT_DESCALE(tmp11 - tmp2, IDCT_CB - IDCT_P1);
        w[16] = IDCT_DESCALE(tmp12 + tmp1, IDCT_CB - IDCT_P1); w[40] = IDCT_DESCALE(tmp12 - tmp1, IDCT_CB - IDCT_P1);
        w[24] = IDCT_DESCALE(tmp13 + tmp0, IDCT_CB - IDCT_P1); w[32] = IDCT_DESCALE(tmp13 - tmp0, IDCT_CB - IDCT_P1);
    }
    for (i = 0; i < 8; i++) {
        const long *w = ws + i * 8; u8 *o = out + (size_t)i * stride; long v[8]; int k;
        if (!w[1] && !w[2] && !w[3] && !w[4] && !w[5] && !w[6] && !w[7]) {
            long dc = IDCT_DESCALE(w[0], IDCT_P1 + 3) + 128;
            u8 q = (u8)(dc < 0 ? 0 : dc > 255 ? 255 : dc);
            for (k = 0; k < 8; k++) o[k] = q;
            continue;
        }
        z2 = w[2]; z3 = w[6];
        z1 = (z2 + z3) * 4433; tmp2 = z1 + z3 * (-15137); tmp3 = z1 + z2 * 6270;
        tmp0 = (w[0] + w[4]) * 8192; tmp1 = (w[0] - w[4]) * 8192;
        tmp10 = tmp0 + tmp3; tmp13 = tmp0 - tmp3; tmp11 = tmp1 + tmp2; tmp12 = tmp1 - tmp2;
        tmp0 = w[7]; tmp1 = w[5]; tmp2 = w[3]; tmp3 = w[1];
        z1 = tmp0 + tmp3; z2 = tmp1 + tmp2; z3 = tmp0 + tmp2; z4 = tmp1 + tmp3;
        z5 = (z3 + z4) * 9633;
        tmp0 *= 2446; tmp1 *= 16819; tmp2 *= 25172; tmp3 *= 12299;
        z1 *= -7373; z2 *= -20995; z3 *= -16069; z4 *= -3196;
        z3 += z5; z4 += z5;
        tmp0 += z1 + z3; tmp1 += z2 + z4; tmp2 += z2 + z3; tmp3 += z1 + z4;
        v[0] = tmp10 + tmp3; v[7] = tmp10 - tmp3; v[1] = tmp11 + tmp2; v[6] = tmp11 - tmp2;
        v[2] = tmp12 + tmp1; v[5] = tmp12 - tmp1; v[3] = tmp13 + tmp0; v[4] = tmp13 - tmp0;
        for (k = 0; k < 8; k++) { long q = IDCT_DESCALE(v[k], IDCT_CB + IDCT_P1 + 3) + 128; o[k] = (u8)(q < 0 ? 0 : q > 255 ? 255 : q); }
    }
}

/* ---- block decoders (coefficients in natural order, not dequantized) ---- */

static void decode_block_baseline(JDec *j, Component *c, int *coef) {
    int k, t, r, s;
    t = decode_huff(j, &j->hdc[c->td]);
    s = extend(get_bits(j, t), t);
    c->dc_pred += s;
    coef[0] = c->dc_pred;
    for (k = 1; k < 64; ) {
        int rs = decode_huff(j, &j->hac[c->ta]);
        r = rs >> 4; s = rs & 15;
        if (s == 0) { if (r == 15) { k += 16; continue; } break; }
        k += r;
        if (k > 63) break;
        coef[zigzag[k]] = extend(get_bits(j, s), s);
        k++;
    }
}

static void decode_dc_first(JDec *j, Component *c, int *coef) {
    int t = decode_huff(j, &j->hdc[c->td]);
    int s = extend(get_bits(j, t), t);
    c->dc_pred += s;
    coef[0] = c->dc_pred << j->al;
}
static void decode_dc_refine(JDec *j, int *coef) {
    if (get_bit(j)) coef[0] |= (1 << j->al);
}
static void decode_ac_first(JDec *j, Component *c, int *coef) {
    int k;
    if (j->eobrun > 0) { j->eobrun--; return; }
    for (k = j->ss; k <= j->se; ) {
        int rs = decode_huff(j, &j->hac[c->ta]);
        int r = rs >> 4, s = rs & 15;
        if (s == 0) {
            if (r < 15) { j->eobrun = (1 << r) - 1; if (r) j->eobrun += get_bits(j, r); break; }
            k += 16; continue;
        }
        k += r;
        if (k > 63) break;
        coef[zigzag[k]] = extend(get_bits(j, s), s) * (1 << j->al);
        k++;
    }
}
static void decode_ac_refine(JDec *j, Component *c, int *coef) {
    int k = j->ss, p1 = 1 << j->al, m1 = -1 << j->al;
    if (j->eobrun <= 0) {
        for (; k <= j->se; ) {
            int rs = decode_huff(j, &j->hac[c->ta]);
            int r = rs >> 4, s = rs & 15, val = 0;
            if (s == 0) {
                if (r < 15) { j->eobrun = (1 << r); if (r) j->eobrun += get_bits(j, r); break; }
            } else {
                val = get_bit(j) ? p1 : m1;
            }
            while (k <= j->se) {
                int z = zigzag[k];
                if (coef[z] != 0) {
                    if (get_bit(j)) { if ((coef[z] & p1) == 0) coef[z] += (coef[z] >= 0) ? p1 : m1; }
                } else {
                    if (r == 0) { if (val) coef[z] = val; k++; break; }
                    r--;
                }
                k++;
            }
        }
    }
    if (j->eobrun > 0) {
        for (; k <= j->se; k++) {
            int z = zigzag[k];
            if (coef[z] != 0) { if (get_bit(j)) { if ((coef[z] & p1) == 0) coef[z] += (coef[z] >= 0) ? p1 : m1; } }
        }
        j->eobrun--;
    }
}

static void reset_bits(JDec *j) {
    j->bitbuf = 0; j->bitcnt = 0; j->marker_hit = 0;
    while (j->p < j->end && *j->p == 0xFF) j->p++;
    if (j->p < j->end && *j->p >= 0xD0 && *j->p <= 0xD7) j->p++;
}

static void decode_scan(JDec *j) {
    int c, mcu = 0;
    j->eobrun = 0;
    j->bitbuf = 0; j->bitcnt = 0; j->marker_hit = 0;
    for (c = 0; c < j->ncomp; c++) j->comp[c].dc_pred = 0;
    if (j->nscomp == 1) {
        /* non-interleaved: iterate the component's own blocks */
        Component *cp = &j->comp[j->scomp[0]];
        int by, bx;
        for (by = 0; by < cp->bh; by++) for (bx = 0; bx < cp->bw; bx++) {
            int *coef = cp->coef + ((size_t)by * cp->pbw + bx) * 64;
            if (j->restart_interval && mcu && (mcu % j->restart_interval) == 0) { reset_bits(j); cp->dc_pred = 0; j->eobrun = 0; }
            if (!j->progressive) decode_block_baseline(j, cp, coef);
            else if (j->ss == 0) { if (j->ah == 0) decode_dc_first(j, cp, coef); else decode_dc_refine(j, coef); }
            else { if (j->ah == 0) decode_ac_first(j, cp, coef); else decode_ac_refine(j, cp, coef); }
            mcu++;
        }
    } else {
        int mx, my, k, by, bx;
        for (my = 0; my < j->mcuy; my++) for (mx = 0; mx < j->mcux; mx++) {
            if (j->restart_interval && mcu && (mcu % j->restart_interval) == 0) { reset_bits(j); for (c = 0; c < j->ncomp; c++) j->comp[c].dc_pred = 0; j->eobrun = 0; }
            for (k = 0; k < j->nscomp; k++) {
                Component *cp = &j->comp[j->scomp[k]];
                for (by = 0; by < cp->v; by++) for (bx = 0; bx < cp->h; bx++) {
                    int *coef = cp->coef + ((size_t)(my * cp->v + by) * cp->pbw + (mx * cp->h + bx)) * 64;
                    if (!j->progressive) decode_block_baseline(j, cp, coef);
                    else if (j->ah == 0) decode_dc_first(j, cp, coef);
                    else decode_dc_refine(j, coef);
                }
            }
            mcu++;
        }
    }
    /* skip to the next marker */
    while (j->p < j->end && !(j->p[0] == 0xFF && j->p + 1 < j->end && j->p[1] != 0 && !(j->p[1] >= 0xD0 && j->p[1] <= 0xD7))) j->p++;
}

static int parse_markers(JDec *j, int *scan_started) {
    for (;;) {
        int m, len;
        if (j->p >= j->end) return 0;
        if (*j->p != 0xFF) { j->p++; continue; }
        while (j->p < j->end && *j->p == 0xFF) j->p++;
        if (j->p >= j->end) return 0;
        m = *j->p++;
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) continue;
        if (m == 0xD9) { if (*scan_started) return 0; continue; }   /* EOI before SOS: tables segment ended (SWF quirk) */
        len = rd16(j) - 2;
        if (len < 0) return 0;
        switch (m) {
        case 0xDB: {                                 /* DQT */
            const u8 *e = j->p + len;
            while (j->p < e) {
                int pq = rd8(j), tq = pq & 3, i;
                for (i = 0; i < 64; i++) j->qt[tq][i] = (u16)((pq >> 4) ? rd16(j) : rd8(j));
            }
            break; }
        case 0xC4: {                                 /* DHT */
            const u8 *e = j->p + len;
            while (j->p < e) {
                int tc = rd8(j), th = tc & 3, i, total = 0;
                HuffTable *h = (tc >> 4) ? &j->hac[th] : &j->hdc[th];
                memset(h, 0, sizeof(*h));
                for (i = 1; i <= 16; i++) { h->bits[i] = (u8)rd8(j); total += h->bits[i]; }
                for (i = 0; i < total && i < 256; i++) h->vals[i] = (u8)rd8(j);
                build_huff(h);
            }
            break; }
        case 0xC0: case 0xC1: case 0xC2: {           /* SOF */
            int i;
            if (m == 0xC2) j->progressive = 1;
            rd8(j);
            j->height = rd16(j); j->width = rd16(j);
            j->ncomp = rd8(j);
            if (j->ncomp > 4) return 0;
            for (i = 0; i < j->ncomp; i++) {
                int hv;
                j->comp[i].id = rd8(j); hv = rd8(j); j->comp[i].h = hv >> 4; j->comp[i].v = hv & 15; j->comp[i].tq = rd8(j) & 3;
                if (j->comp[i].h < 1 || j->comp[i].v < 1) return 0;
            }
            break; }
        case 0xDD: j->restart_interval = rd16(j); break;
        case 0xEE: {                                 /* APP14 Adobe */
            if (len >= 12 && memcmp(j->p, "Adobe", 5) == 0) j->adobe_transform = j->p[11];
            j->p += len; break; }
        case 0xDA: {                                 /* SOS */
            int n = rd8(j), i;
            j->nscomp = 0;
            for (i = 0; i < n; i++) {
                int cid = rd8(j), tt = rd8(j), k;
                for (k = 0; k < j->ncomp; k++) if (j->comp[k].id == cid) { j->comp[k].td = tt >> 4; j->comp[k].ta = tt & 15; if (j->nscomp < 4) j->scomp[j->nscomp++] = k; }
            }
            j->ss = rd8(j); j->se = rd8(j); { int a = rd8(j); j->ah = a >> 4; j->al = a & 15; }
            if (!j->progressive) { j->ss = 0; j->se = 63; j->ah = j->al = 0; }
            *scan_started = 1;
            return 1; }
        default: j->p += len; break;
        }
    }
}

static Bitmap *jpeg_decode(const u8 *data, size_t len) {
    JDec j; int started = 0, i, mx, my, c, allocated = 0;
    Bitmap *bm = NULL;
    memset(&j, 0, sizeof(j));
    j.p = data; j.end = data + len;
    /* scans */
    while (parse_markers(&j, &started)) {
        if (!allocated) {
            if (j.width <= 0 || j.height <= 0) goto fail;
            j.hmax = j.vmax = 1;
            for (i = 0; i < j.ncomp; i++) { if (j.comp[i].h > j.hmax) j.hmax = j.comp[i].h; if (j.comp[i].v > j.vmax) j.vmax = j.comp[i].v; }
            j.mcux = (j.width + 8 * j.hmax - 1) / (8 * j.hmax);
            j.mcuy = (j.height + 8 * j.vmax - 1) / (8 * j.vmax);
            for (i = 0; i < j.ncomp; i++) {
                Component *cp = &j.comp[i];
                int cw = (j.width * cp->h + j.hmax - 1) / j.hmax, chh = (j.height * cp->v + j.vmax - 1) / j.vmax;
                cp->bw = (cw + 7) / 8; cp->bh = (chh + 7) / 8;
                cp->pbw = j.mcux * cp->h; cp->pbh = j.mcuy * cp->v;
                cp->coef = (int*)calloc((size_t)cp->pbw * cp->pbh * 64, sizeof(int));
                cp->pw = cp->pbw * 8; cp->ph = cp->pbh * 8;
                cp->plane = (u8*)malloc((size_t)cp->pw * cp->ph);
                if (!cp->coef || !cp->plane) goto fail;
            }
            allocated = 1;
        }
        decode_scan(&j);
        started = 0;
    }
    if (!allocated) return NULL;
    /* dequantize + IDCT */
    for (c = 0; c < j.ncomp; c++) {
        Component *cp = &j.comp[c]; int by, bx, k, tmp[64];
        for (by = 0; by < cp->pbh; by++) for (bx = 0; bx < cp->pbw; bx++) {
            int *coef = cp->coef + ((size_t)by * cp->pbw + bx) * 64;
            for (k = 0; k < 64; k++) tmp[zigzag[k]] = coef[zigzag[k]] * j.qt[cp->tq][k];
            idct_block(tmp, cp->plane + (size_t)by * 8 * cp->pw + bx * 8, cp->pw);
        }
    }
    bm = (Bitmap*)calloc(1, sizeof(Bitmap));
    if (!bm) goto fail;
    bm->w = j.width; bm->h = j.height;
    bm->px = (u32*)malloc((size_t)bm->w * bm->h * sizeof(u32));
    if (!bm->px) { free(bm); bm = NULL; goto fail; }
    {
        /* full-size component planes: libjpeg's fancy upsampling (triangle filter) for 2x1 and 2x2 */
        u8 *full[4] = { NULL, NULL, NULL, NULL }; int k;
        for (k = 0; k < j.ncomp && k < 4; k++) {
            Component *cp = &j.comp[k]; int fx = j.hmax / cp->h, fy = j.vmax / cp->v;
            int cw = (j.width * cp->h + j.hmax - 1) / j.hmax, ch = (j.height * cp->v + j.vmax - 1) / j.vmax, x, y;
            u8 *f = (u8*)malloc((size_t)j.width * j.height + 1);
            if (!f) break;
            full[k] = f;
            if (cw > cp->pw) cw = cp->pw; if (ch > cp->ph) ch = cp->ph; if (cw < 1) cw = 1; if (ch < 1) ch = 1;
            #define CPX(xx, yy) cp->plane[(size_t)(yy) * cp->pw + (xx)]
            if (fx == 2 && fy == 2 && j.hmax * cp->h > 0) {
                for (y = 0; y < j.height; y++) {
                    int sy = y >> 1, ny = (y & 1) ? sy + 1 : sy - 1, last, this_, next;
                    if (ny < 0) ny = 0; if (ny >= ch) ny = ch - 1; if (sy >= ch) sy = ch - 1;
                    for (x = 0; x < j.width; x++) {
                        int sx = x >> 1;
                        this_ = CPX(sx, sy) * 3 + CPX(sx, ny);
                        if (x & 1) {
                            if (sx + 1 < cw) { next = CPX(sx + 1, sy) * 3 + CPX(sx + 1, ny); f[(size_t)y * j.width + x] = (u8)((this_ * 3 + next + 7) >> 4); }
                            else f[(size_t)y * j.width + x] = (u8)((this_ * 4 + 7) >> 4);
                        } else {
                            if (sx > 0) { last = CPX(sx - 1, sy) * 3 + CPX(sx - 1, ny); f[(size_t)y * j.width + x] = (u8)((this_ * 3 + last + 8) >> 4); }
                            else f[(size_t)y * j.width + x] = (u8)((this_ * 4 + 8) >> 4);
                        }
                    }
                }
            } else if (fx == 2 && fy == 1) {
                for (y = 0; y < j.height; y++) {
                    int sy = y < ch ? y : ch - 1;
                    for (x = 0; x < j.width; x++) {
                        int sx = x >> 1, v = CPX(sx, sy) * 3;
                        if (x & 1) f[(size_t)y * j.width + x] = (u8)(sx + 1 < cw ? (v + CPX(sx + 1, sy) + 2) >> 2 : CPX(sx, sy));
                        else f[(size_t)y * j.width + x] = (u8)(sx > 0 ? (v + CPX(sx - 1, sy) + 1) >> 2 : CPX(sx, sy));
                    }
                }
            } else {
                for (y = 0; y < j.height; y++) for (x = 0; x < j.width; x++)
                    f[(size_t)y * j.width + x] = CPX(x * cp->h / j.hmax, y * cp->v / j.vmax);
            }
            #undef CPX
        }
        for (k = 0; k < j.ncomp && k < 4; k++) if (!full[k]) break;
        if (k < (j.ncomp < 4 ? j.ncomp : 4)) { for (k = 0; k < 4; k++) free(full[k]); free(bm->px); free(bm); bm = NULL; goto fail; }
        for (my = 0; my < j.height; my++) for (mx = 0; mx < j.width; mx++) {
            size_t q = (size_t)my * j.width + mx; int Y = full[0][q], r, g, b;
            if (j.ncomp >= 3) {
                int Cb = full[1][q] - 128, Cr = full[2][q] - 128;
                if (j.adobe_transform == 0 && j.ncomp == 3 && j.comp[0].id == 'R') { r = Y; g = full[1][q]; b = full[2][q]; }
                else {
                    /* libjpeg ycc_rgb_convert (16 bit fixed point) */
                    r = Y + (int)((91881L * Cr + 32768) >> 16);
                    g = Y + (int)((-22554L * Cb - 46802L * Cr + 32768) >> 16);
                    b = Y + (int)((116130L * Cb + 32768) >> 16);
                }
            } else r = g = b = Y;
            if (r < 0) r = 0; if (r > 255) r = 255; if (g < 0) g = 0; if (g > 255) g = 255; if (b < 0) b = 0; if (b > 255) b = 255;
            bm->px[q] = 0xff000000u | ((u32)r << 16) | ((u32)g << 8) | (u32)b;
        }
        for (k = 0; k < 4; k++) free(full[k]);
    }
fail:
    for (i = 0; i < 4; i++) { free(j.comp[i].plane); free(j.comp[i].coef); }
    return bm;
}

/* ---- PNG (for DefineBitsJPEG2 payloads that are really PNG) ---- */

static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

static int paeth(int a, int b, int c) { int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c); return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c); }

/* PNG: all colour types and bit depths, Adam7 interlacing, tRNS */
static unsigned png_sample(const u8 *row, size_t i, int depth) {
    if (depth == 8) return row[i];
    if (depth == 16) return ((unsigned)row[i * 2] << 8) | row[i * 2 + 1];
    { size_t bit = i * (size_t)depth; return (row[bit >> 3] >> (8 - depth - (int)(bit & 7))) & ((1u << depth) - 1); }
}
static Bitmap *png_decode(const u8 *d, size_t n) {
    static const int ax0[7] = { 0, 4, 0, 2, 0, 1, 0 }, ay0[7] = { 0, 0, 4, 0, 2, 0, 1 }, adx[7] = { 8, 8, 4, 4, 2, 2, 1 }, ady[7] = { 8, 8, 8, 4, 4, 2, 2 };
    size_t pos = 8; u32 w = 0, h = 0; int depth = 0, ctype = 0, interlace = 0;
    u8 *idat = NULL; size_t idatlen = 0, idatcap = 0; u8 *raw; u32 pal[256]; int npal = 0; u8 trns[256]; int ntrns = 0; unsigned tkey[3] = { 0, 0, 0 }; int has_tkey = 0;
    int chans, bits, fbpp, pass, npass; size_t rawlen = 0, off; long got; Bitmap *bm = NULL; unsigned maxv;
    memset(trns, 255, sizeof(trns));
    while (pos + 8 <= n) {
        u32 clen = be32(d + pos); const u8 *ct = d + pos + 4; const u8 *cd = d + pos + 8;
        if (pos + 12 + clen > n) break;
        if (!memcmp(ct, "IHDR", 4) && clen >= 13) { w = be32(cd); h = be32(cd + 4); depth = cd[8]; ctype = cd[9]; interlace = cd[12]; }
        else if (!memcmp(ct, "PLTE", 4)) { int i; npal = (int)(clen / 3); if (npal > 256) npal = 256; for (i = 0; i < npal; i++) pal[i] = 0xff000000u | ((u32)cd[i * 3] << 16) | ((u32)cd[i * 3 + 1] << 8) | cd[i * 3 + 2]; }
        else if (!memcmp(ct, "tRNS", 4)) {
            if (ctype == 3) { ntrns = (int)clen; if (ntrns > 256) ntrns = 256; memcpy(trns, cd, (size_t)ntrns); }
            else if (ctype == 0 && clen >= 2) { tkey[0] = ((unsigned)cd[0] << 8) | cd[1]; has_tkey = 1; }
            else if (ctype == 2 && clen >= 6) { int k; for (k = 0; k < 3; k++) tkey[k] = ((unsigned)cd[k * 2] << 8) | cd[k * 2 + 1]; has_tkey = 1; }
        }
        else if (!memcmp(ct, "IDAT", 4)) {
            if (idatlen + clen > idatcap) { size_t nc = idatcap ? idatcap * 2 : 65536; u8 *ni; while (nc < idatlen + clen) nc *= 2; ni = (u8*)realloc(idat, nc); if (!ni) { free(idat); return NULL; } idat = ni; idatcap = nc; }
            memcpy(idat + idatlen, cd, clen); idatlen += clen;
        } else if (!memcmp(ct, "IEND", 4)) break;
        pos += 12 + clen;
    }
    switch (ctype) { case 0: chans = 1; break; case 2: chans = 3; break; case 3: chans = 1; break; case 4: chans = 2; break; case 6: chans = 4; break; default: free(idat); return NULL; }
    if (!w || !h || !idat || (double)w * h > 67108864.0 || (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16)) { free(idat); return NULL; }
    bits = chans * depth; fbpp = bits >= 8 ? bits / 8 : 1; maxv = (1u << depth) - 1;
    npass = interlace ? 7 : 1;
    for (pass = 0; pass < npass; pass++) {
        u32 pw = interlace ? (w > (u32)ax0[pass] ? (w - ax0[pass] + adx[pass] - 1) / adx[pass] : 0) : w;
        u32 ph = interlace ? (h > (u32)ay0[pass] ? (h - ay0[pass] + ady[pass] - 1) / ady[pass] : 0) : h;
        if (pw && ph) rawlen += (((size_t)pw * bits + 7) / 8 + 1) * ph;
    }
    raw = (u8*)malloc(rawlen ? rawlen : 1);
    if (!raw) { free(idat); return NULL; }
    got = swf_inflate_zlib(idat, idatlen, raw, rawlen);
    free(idat);
    if (got < (long)rawlen) { free(raw); return NULL; }
    bm = (Bitmap*)calloc(1, sizeof(Bitmap));
    if (!bm) { free(raw); return NULL; }
    bm->w = (int)w; bm->h = (int)h; bm->px = (u32*)calloc((size_t)w * h, sizeof(u32));
    if (!bm->px) { free(bm); free(raw); return NULL; }
    off = 0;
    for (pass = 0; pass < npass; pass++) {
        u32 pw = interlace ? (w > (u32)ax0[pass] ? (w - ax0[pass] + adx[pass] - 1) / adx[pass] : 0) : w;
        u32 ph = interlace ? (h > (u32)ay0[pass] ? (h - ay0[pass] + ady[pass] - 1) / ady[pass] : 0) : h;
        size_t rowbytes = ((size_t)pw * bits + 7) / 8; u32 y;
        if (!pw || !ph) continue;
        for (y = 0; y < ph; y++) {
            u8 *row = raw + off + y * (rowbytes + 1), *prev = y ? raw + off + (y - 1) * (rowbytes + 1) + 1 : NULL; int ft = row[0]; size_t x;
            u32 oy = interlace ? ay0[pass] + y * ady[pass] : y;
            row++;
            for (x = 0; x < rowbytes; x++) {
                int a = x >= (size_t)fbpp ? row[x - fbpp] : 0, b = prev ? prev[x] : 0, c = (prev && x >= (size_t)fbpp) ? prev[x - fbpp] : 0;
                switch (ft) { case 1: row[x] = (u8)(row[x] + a); break; case 2: row[x] = (u8)(row[x] + b); break; case 3: row[x] = (u8)(row[x] + ((a + b) >> 1)); break; case 4: row[x] = (u8)(row[x] + paeth(a, b, c)); break; default: break; }
            }
            for (x = 0; x < pw; x++) {
                unsigned s0 = png_sample(row, x * chans, depth), s1 = chans > 1 ? png_sample(row, x * chans + 1, depth) : 0, s2 = chans > 2 ? png_sample(row, x * chans + 2, depth) : 0, s3 = chans > 3 ? png_sample(row, x * chans + 3, depth) : 0;
                u32 ox = interlace ? ax0[pass] + (u32)x * adx[pass] : (u32)x, v;
                #define SC(q) ((u32)(depth == 16 ? ((q) >> 8) : depth == 8 ? (q) : ((q) * 255u / maxv)))
                switch (ctype) {
                case 0: v = 0xff000000u | (SC(s0) << 16) | (SC(s0) << 8) | SC(s0); if (has_tkey && s0 == tkey[0]) v &= 0x00ffffffu; break;
                case 2: v = 0xff000000u | (SC(s0) << 16) | (SC(s1) << 8) | SC(s2); if (has_tkey && s0 == tkey[0] && s1 == tkey[1] && s2 == tkey[2]) v &= 0x00ffffffu; break;
                case 3: v = ((int)s0 < npal ? pal[s0] : 0xff000000u); v = (v & 0x00ffffffu) | ((u32)trns[s0 & 255] << 24); break;
                case 4: v = (SC(s1) << 24) | (SC(s0) << 16) | (SC(s0) << 8) | SC(s0); break;
                default: v = (SC(s3) << 24) | (SC(s0) << 16) | (SC(s1) << 8) | SC(s2); break;
                }
                #undef SC
                if (ox < w && oy < h) bm->px[(size_t)oy * w + ox] = v;
            }
        }
        off += (rowbytes + 1) * ph;
    }
    free(raw);
    return bm;
}

/* ---- GIF (the first frame on the logical screen, transparent elsewhere) ---- */

static Bitmap *gif_decode(const u8 *d, size_t n) {
    size_t pos = 13; int sw, sh, flags, gct_n = 0, trans = -1, x, y;
    const u8 *gct = NULL, *ct; Bitmap *bm;
    if (n < 13) return NULL;
    sw = d[6] | (d[7] << 8); sh = d[8] | (d[9] << 8); flags = d[10];
    if (sw <= 0 || sh <= 0 || (double)sw * sh > 16777216.0) return NULL;
    if (flags & 0x80) { gct_n = 2 << (flags & 7); gct = d + pos; pos += (size_t)gct_n * 3; if (pos > n) return NULL; }
    bm = (Bitmap*)calloc(1, sizeof(Bitmap));
    if (!bm) return NULL;
    bm->w = sw; bm->h = sh; bm->px = (u32*)calloc((size_t)sw * sh, 4);
    if (!bm->px) { free(bm); return NULL; }
    while (pos < n) {
        int b = d[pos++];
        if (b == 0x21) {                     /* extension */
            int label; if (pos >= n) break; label = d[pos++];
            if (label == 0xF9 && pos + 5 < n && d[pos] >= 4) { if (d[pos + 1] & 1) trans = d[pos + 4]; }
            while (pos < n && d[pos]) pos += (size_t)d[pos] + 1;
            pos++;
        } else if (b == 0x2C) {              /* image descriptor */
            int ix, iy, iw, ih, f2, lct_n = 0, interlace, mcs, ct_n;
            u8 *idx; size_t npx, outp = 0;
            if (pos + 9 > n) break;
            ix = d[pos] | (d[pos + 1] << 8); iy = d[pos + 2] | (d[pos + 3] << 8); iw = d[pos + 4] | (d[pos + 5] << 8); ih = d[pos + 6] | (d[pos + 7] << 8); f2 = d[pos + 8]; pos += 9;
            interlace = (f2 & 0x40) != 0;
            ct = gct; ct_n = gct_n;
            if (f2 & 0x80) { lct_n = 2 << (f2 & 7); ct = d + pos; ct_n = lct_n; pos += (size_t)lct_n * 3; if (pos > n) break; }
            if (pos >= n || iw <= 0 || ih <= 0) break;
            mcs = d[pos++];
            if (mcs < 1 || mcs > 11) break;
            npx = (size_t)iw * ih;
            idx = (u8*)calloc(npx ? npx : 1, 1);
            if (!idx) break;
            {   /* LZW over the sub-blocks */
                static u16 prefix[4096]; static u8 suffix[4096], stack[4097];
                int clear = 1 << mcs, eoi = clear + 1, avail = clear + 2, csize = mcs + 1, cmask = (1 << csize) - 1, old = -1, first = 0, k;
                u32 acc = 0; int bits = 0; size_t blk = pos; int blen = 0;
                for (k = 0; k < clear; k++) { prefix[k] = 0; suffix[k] = (u8)k; }
                for (;;) {
                    int code, in, sp = 0;
                    while (bits < csize) {
                        if (blen == 0) { if (blk >= n || d[blk] == 0) goto lzw_done; blen = d[blk++]; }
                        if (blk >= n) goto lzw_done;
                        acc |= (u32)d[blk++] << bits; bits += 8; blen--;
                    }
                    code = (int)(acc & (u32)cmask); acc >>= csize; bits -= csize;
                    if (code == clear) { csize = mcs + 1; cmask = (1 << csize) - 1; avail = clear + 2; old = -1; continue; }
                    if (code == eoi) break;
                    if (old == -1) { if (code >= clear) break; if (outp < npx) idx[outp++] = (u8)code; old = first = code; continue; }
                    in = code;
                    if (code > avail) break;
                    if (code == avail) { stack[sp++] = (u8)first; code = old; }
                    while (code >= clear && sp < 4096) { stack[sp++] = suffix[code]; code = prefix[code]; }
                    first = suffix[code]; stack[sp++] = (u8)first;
                    if (avail < 4096) { prefix[avail] = (u16)old; suffix[avail] = (u8)first; avail++; if ((avail & cmask) == 0 && avail < 4096) { csize++; cmask = (1 << csize) - 1; } }
                    old = in;
                    while (sp > 0 && outp < npx) idx[outp++] = stack[--sp];
                }
            lzw_done:
                while (pos < n && d[pos]) pos += (size_t)d[pos] + 1;   /* skip the data sub-blocks */
                pos++;
            }
            /* paint the frame on the screen (interlaced rows in passes) */
            {
                int pass_start[4] = { 0, 4, 2, 1 }, pass_step[4] = { 8, 8, 4, 2 }, pass = 0, row = 0, r;
                for (r = 0; r < ih; r++) {
                    int yy;
                    if (interlace) {
                        while (pass < 4 && pass_start[pass] + row * pass_step[pass] >= ih) { pass++; row = 0; }
                        if (pass >= 4) break;
                        yy = pass_start[pass] + row * pass_step[pass]; row++;
                    } else yy = r;
                    y = iy + yy;
                    if (y < 0 || y >= sh) continue;
                    for (x = 0; x < iw; x++) {
                        int c = idx[(size_t)r * iw + x], xx = ix + x;
                        if (xx < 0 || xx >= sw || c == trans || !ct || c >= ct_n) continue;
                        bm->px[(size_t)y * sw + xx] = 0xFF000000u | ((u32)ct[c * 3] << 16) | ((u32)ct[c * 3 + 1] << 8) | ct[c * 3 + 2];
                    }
                }
            }
            free(idx);
            break;                           /* the first frame only */
        } else break;                        /* trailer or garbage */
    }
    return bm;
}

/* PNG / GIF pixels as Ruffle premultiplies them (f32, truncated), kept as the exact texels */
static void premul_trunc(Bitmap *bm) {
    size_t i, n; int any = 0;
    if (!bm || !bm->px) return;
    n = (size_t)bm->w * bm->h;
    for (i = 0; i < n; i++) if ((bm->px[i] >> 24) != 255) { any = 1; break; }
    if (!any || !(bm->pm_own = (u32*)malloc(n * 4))) return;
    for (i = 0; i < n; i++) {
        u32 c = bm->px[i]; float a = (float)(c >> 24) / 255.0f;
        bm->pm_own[i] = (c & 0xFF000000u) | ((u32)(u8)((float)((c >> 16) & 255) * a) << 16) | ((u32)(u8)((float)((c >> 8) & 255) * a) << 8) | (u32)(u8)((float)(c & 255) * a);
    }
    bm->pm = bm->pm_own;
}

/* ---- SWF wrappers ---- */

/* strip the "FF D9 FF D8" erroneous header some tools emit, and locate SOI */
/* JPEG XR (Ruffle decodes it to RGBA through a TIFF and uses those bytes as
   they are, as if premultiplied) */
/* a JPEG XR image (jxr.c): straight colour, the decoded texels as the premultiplied data
   (Ruffle uploads them as they are); *has_alpha: the pixel format carries alpha */
Bitmap *jxr_decode(const u8 *d, size_t n, int *has_alpha) {
    int w, h, fmt = 0; size_t i, np; u8 *rgba; Bitmap *bm;
    if (has_alpha) *has_alpha = 0;
    rgba = jxr_decode_rgba(d, n, &w, &h);
    if (!rgba) return NULL;
    if (has_alpha && jxr_info(d, n, &w, &h, &fmt)) *has_alpha = fmt == 0x0f || fmt == 0x10 || fmt == 0x16 || fmt == 0x17;
    np = (size_t)w * (size_t)h;
    bm = (Bitmap*)calloc(1, sizeof(Bitmap));
    if (!bm || !(bm->px = (u32*)malloc(np * 4)) || !(bm->pm_own = (u32*)malloc(np * 4))) { if (bm) { free(bm->px); free(bm); } free(rgba); return NULL; }
    bm->w = w; bm->h = h;
    for (i = 0; i < np; i++) {
        const u8 *q = rgba + i * 4; u32 c = ((u32)q[3] << 24) | ((u32)q[0] << 16) | ((u32)q[1] << 8) | q[2];
        bm->pm_own[i] = c; bm->px[i] = c;
        if (q[3] != 255) {   /* the straight colour of those texels */
            u32 a = q[3], r = a ? ((u32)q[0] * 255 + a / 2) / a : 0, g = a ? ((u32)q[1] * 255 + a / 2) / a : 0, b = a ? ((u32)q[2] * 255 + a / 2) / a : 0;
            bm->px[i] = (a << 24) | ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
        }
    }
    bm->pm = bm->pm_own;
    free(rgba);
    return bm;
}

static Bitmap *decode_jpeg_data(const u8 *d, size_t n) {
    size_t i;
    if (n >= 4 && d[0] == 0x49 && d[1] == 0x49 && d[2] == 0xbc && d[3] == 0x01) return jxr_decode(d, n, NULL);
    if (n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') { Bitmap *b = png_decode(d, n); premul_trunc(b); return b; }
    if (n >= 6 && !memcmp(d, "GIF8", 4)) { Bitmap *b = gif_decode(d, n); premul_trunc(b); return b; }
    for (i = 0; i + 1 < n; i++) if (d[i] == 0xFF && d[i + 1] == 0xD8) {
        /* skip SOI immediately followed by EOI+SOI */
        if (i + 3 < n && d[i + 2] == 0xFF && d[i + 3] == 0xD9) { i += 2; continue; }
        return jpeg_decode(d + i, n - i);
    }
    return NULL;
}

Bitmap *jpeg_char_decode(SWFMovie *m, SWFChar *ch) {
    const u8 *d = ch->data + 2; size_t n = ch->len - 2;
    Bitmap *bm = NULL;
    if (ch->len < 2) return NULL;
    switch (ch->tagcode) {
    case TAG_DefineBits: {
        /* JPEGTables (DQT/DHT ... EOI) + image (SOI ... EOI): join them */
        u8 *buf; size_t tl = m->jpeg_tables_len, pos = 0, k;
        if (!m->jpeg_tables || tl < 4) { bm = decode_jpeg_data(d, n); break; }
        buf = (u8*)malloc(tl + n);
        if (!buf) return NULL;
        /* tables without trailing EOI */
        memcpy(buf, m->jpeg_tables, tl); pos = tl;
        if (pos >= 2 && buf[pos - 2] == 0xFF && buf[pos - 1] == 0xD9) pos -= 2;
        /* image without leading SOI */
        k = 0; if (n >= 2 && d[0] == 0xFF && d[1] == 0xD8) k = 2;
        memcpy(buf + pos, d + k, n - k); pos += n - k;
        bm = decode_jpeg_data(buf, pos);
        free(buf);
        break; }
    case TAG_DefineBitsJPEG2:
        bm = decode_jpeg_data(d, n);
        break;
    case TAG_DefineBitsJPEG3: case TAG_DefineBitsJPEG4: {
        u32 off; size_t hdr = 4;
        if (ch->tagcode == TAG_DefineBitsJPEG4) hdr = 6;
        if (n < hdr) return NULL;
        off = (u32)d[0] | ((u32)d[1] << 8) | ((u32)d[2] << 16) | ((u32)d[3] << 24);
        if (off > n - hdr) off = (u32)(n - hdr);
        bm = decode_jpeg_data(d + hdr, off);
        if (bm && n > hdr + off) {
            size_t npx = (size_t)bm->w * bm->h; u8 *alpha = (u8*)malloc(npx); long got;
            if (alpha) {
                got = swf_inflate_zlib(d + hdr + off, n - hdr - off, alpha, npx);
                if (got > 0) {
                    /* the JPEG colours are premultiplied, clamped to the alpha (Ruffle / Flash) */
                    size_t i; bm->pm_own = (u32*)malloc(npx * 4);
                    for (i = 0; i < npx; i++) {
                        u32 a = (i < (size_t)got) ? alpha[i] : 255, c = bm->px[i], r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
                        if (r > a) r = a; if (g > a) g = a; if (b > a) b = a;
                        if (bm->pm_own) bm->pm_own[i] = (a << 24) | (r << 16) | (g << 8) | b;
                        if (a && a < 255) { r = r * 255 / a; g = g * 255 / a; b = b * 255 / a; }
                        else if (!a) r = g = b = 0;
                        bm->px[i] = (a << 24) | (r << 16) | (g << 8) | b;
                    }
                    bm->pm = bm->pm_own;
                }
                free(alpha);
            }
        }
        break; }
    default: break;
    }
    return bm;
}

Bitmap *jpeg_decode_file_for_test(const u8 *d, size_t n) { return decode_jpeg_data(d, n); }

Bitmap *image_decode_bytes(const u8 *d, size_t n) {
    if (n >= 4 && d[0] == 'I' && d[1] == 'I' && d[2] == 0xBC) return jxr_decode(d, n, NULL);
    return decode_jpeg_data(d, n);
}
