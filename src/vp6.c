/* vp6.c - On2 VP6 (Flash VP6F / VP6A) video decoder.
 *
 * A C89 port of the VP6 decoder from NihAV (nihav-duck: vp56.rs, vp6.rs,
 * vp6data.rs, vpcommon.rs; nihav-codec-support blockdsp edge_emu), the
 * crate Ruffle uses for VP6.  Bool coder and Huffman coefficient
 * partitions, simple and advanced profile (bilinear / bicubic MC, the
 * VP3.1 loop filter on MC source blocks), interlaced macroblocks, golden
 * frames, and the VP6A alpha stream (decoded into a fourth plane with its
 * own probability models, as NihAV does).
 *
 * Original Rust code:
 *
 * Copyright (c) 2021 Kostya Shishkov
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Output: the whole coded picture (macroblock aligned, mb_w*16 x mb_h*16);
 * like Flash Player (and Ruffle) the "display size" fields of the VP6
 * header are ignored, the picture is to be cropped to the stream size.
 * For VP6A the colour planes are premultiplied by the alpha plane; the
 * output is converted to straight alpha. */
#include "vdec.h"
#include "image.h"
#include <stdlib.h>
#include <string.h>

#include "vp6data.h"

/* arithmetic shift right / truncating division, portable for negatives */
#define ASR(v, n) ((v) >= 0 ? (v) >> (n) : ~(~(v) >> (n)))
static int div_tz(int a, int b) { return a >= 0 ? a / b : -((-a) / b); }
static int iabs(int a) { return a < 0 ? -a : a; }
static u8 clip8(int v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

#define TOKEN_LARGE 5
#define TOKEN_EOB   42

#define VERSION_VP60 6
#define VERSION_VP62 8
#define VP6_SIMPLE_PROFILE   0
#define VP6_ADVANCED_PROFILE 3

/* ---------------------------------------------------------------- */
/* bool coder (vpcommon.rs)                                         */

typedef struct {
    const u8 *src; size_t len, pos;
    u32 value, range; int bits;
} BC;

static int bc_init(BC *c, const u8 *src, size_t len) {
    if (len < 3) return 0;
    c->src = src; c->len = len;
    c->value = ((u32)src[0] << 24) | ((u32)src[1] << 16) | ((u32)src[2] << 8) | (len > 3 ? (u32)src[3] : 0);
    c->pos = 4; c->range = 255; c->bits = 8;
    return 1;
}
static int bc_prob(BC *c, int prob) {
    u32 split; int shift = 0;
    while (c->range < 0x80) { c->range <<= 1; shift++; }
    c->value <<= shift;
    c->bits -= shift;
    if (c->bits <= 0 && c->pos < c->len) {
        c->value |= (u32)c->src[c->pos] << (-c->bits);
        c->pos++;
        c->bits += 8;
    }
    split = 1 + (((c->range - 1) * (u32)prob) >> 8);
    if (c->value < (split << 24)) { c->range = split; return 0; }
    c->range -= split;
    c->value -= split << 24;
    return 1;
}
#define bc_bool(c) bc_prob(c, 128)
static int bc_bits(BC *c, int n) {
    int v = 0;
    while (n-- > 0) v = (v << 1) | bc_prob(c, 128);
    return v;
}
static int bc_probability(BC *c) {
    int v = bc_bits(c, 7);
    return v ? v << 1 : 1;
}
static void bc_skip_bytes(BC *c, size_t n) {
    while (n-- > 0) {
        c->value <<= 8;
        if (c->pos < c->len) c->value |= c->src[c->pos++];
    }
}

/* ---------------------------------------------------------------- */
/* MSB first bit reader (nihav-core BitReader, BE mode)             */

typedef struct { const u8 *p; size_t nbits, pos; int err; } BR;

static void br_init(BR *b, const u8 *p, size_t len) { b->p = p; b->nbits = len * 8; b->pos = 0; b->err = 0; }
static u32 br_peek(BR *b, int n) {
    u32 v = 0; size_t pos = b->pos; int i;
    for (i = 0; i < n; i++, pos++) {
        v <<= 1;
        if (pos < b->nbits) v |= (b->p[pos >> 3] >> (7 - (pos & 7))) & 1;
    }
    return v;
}
static u32 br_read(BR *b, int n) {
    u32 v;
    if (n == 0) return 0;
    if (b->err || b->pos + (size_t)n > b->nbits) { b->err = 1; return 0; }
    v = br_peek(b, n);
    b->pos += (size_t)n;
    return v;
}

/* ---------------------------------------------------------------- */
/* models                                                           */

typedef struct { u8 nz_prob, sign_prob, raw_probs[8], tree_probs[7]; } MVModel;
typedef struct {
    u8 dc_token_probs[3][5];
    u8 dc_value_probs[11];
    u8 ac_val_probs[3][6][11];
} CoeffModel;
#define MAX_HUFF_ELEMS 12
typedef struct { u16 codes[MAX_HUFF_ELEMS]; u8 bits[MAX_HUFF_ELEMS]; int n; } Huff;

typedef struct {
    MVModel mv_models[2];
    u8 mbtype_probs[3][10][10];
    CoeffModel coeff_models[2];
    u8 prob_xmitted[3][20];
    u8 scan_order[64], scan[64], zigzag[64];
    u8 zero_run_probs[2][14];
    Huff dc_token_tree[2], ac_token_tree[2][3][6], zero_run_tree[2];
} Models;

/* Huffman code construction from the bool coder probabilities */
typedef struct { u16 weight; int sym, ch0, ch1; } HNode;

static u8 prob2weight(int a, int b) {
    int w = (a * b) >> 8;
    return (u8)(w == 0 ? 1 : w);
}
static void huff_get_codes(Huff *h, const HNode *nodes, int pos, u16 code, int len, int depth) {
    if (depth > MAX_HUFF_ELEMS * 2) return;
    if (nodes[pos].sym >= 0) {
        h->codes[nodes[pos].sym] = code;
        h->bits[nodes[pos].sym] = (u8)len;
    } else {
        huff_get_codes(h, nodes, nodes[pos].ch0, (u16)(code << 1), len + 1, depth + 1);
        huff_get_codes(h, nodes, nodes[pos].ch1, (u16)((code << 1) | 1), len + 1, depth + 1);
    }
}
static void huff_build(Huff *h, const u8 *weights, int n) {
    HNode nodes[MAX_HUFF_ELEMS * 2];
    int nlen = 0, low, i, j, k;
    memset(nodes, 0, sizeof(nodes));
    for (k = n - 1; k >= 0; k--) {
        u16 weight = weights[k];
        int pos = nlen;
        for (i = 0; i < nlen; i++) if (nodes[i].weight > weight) { pos = i; break; }
        for (j = nlen - 1; j >= pos; j--) nodes[j + 1] = nodes[j];
        nodes[pos].weight = weight;
        nodes[pos].sym = n - nlen - 1;
        nodes[pos].ch0 = nodes[pos].ch1 = 0;
        nlen++;
    }
    low = 0;
    for (k = 0; k < n - 1; k++) {
        HNode nn; int pos;
        nn.weight = (u16)(nodes[low].weight + nodes[low + 1].weight);
        nn.sym = -1; nn.ch0 = low; nn.ch1 = low + 1;
        low += 2;
        pos = low;
        while (pos < nlen && nodes[pos].weight < nn.weight) pos++;
        for (j = nlen - 1; j >= pos; j--) nodes[j + 1] = nodes[j];
        nodes[pos] = nn;
        nlen++;
    }
    h->n = n;
    huff_get_codes(h, nodes, nlen - 1, 0, 0, 0);
}
static void huff_build_codes(Huff *h, const u8 *p) {
    u8 w[12]; u8 lvroot, tworoot, hlroot, root34, c1root, c34root, c3root, c4root;
    w[11] = prob2weight(p[0], p[1]);
    w[0]  = prob2weight(p[0], 255 - p[1]);
    w[1]  = prob2weight(255 - p[0], p[2]);
    lvroot  = prob2weight(255 - p[0], 255 - p[2]);
    tworoot = prob2weight(lvroot, p[3]);
    hlroot  = prob2weight(lvroot, 255 - p[3]);
    w[2]  = prob2weight(tworoot, p[4]);
    root34  = prob2weight(tworoot, 255 - p[4]);
    w[3]  = prob2weight(root34, p[5]);
    w[4]  = prob2weight(root34, 255 - p[5]);
    c1root  = prob2weight(hlroot, p[6]);
    c34root = prob2weight(hlroot, 255 - p[6]);
    w[5]  = prob2weight(c1root, p[7]);
    w[6]  = prob2weight(c1root, 255 - p[7]);
    c3root  = prob2weight(c34root, p[8]);
    c4root  = prob2weight(c34root, 255 - p[8]);
    w[7]  = prob2weight(c3root, p[9]);
    w[8]  = prob2weight(c3root, 255 - p[9]);
    w[9]  = prob2weight(c4root, p[10]);
    w[10] = prob2weight(c4root, 255 - p[10]);
    huff_build(h, w, 12);
}
static void huff_build_codes_zero_run(Huff *h, const u8 *p) {
    u8 w[9]; u8 root, root1, root2;
    root = prob2weight(p[0], p[1]);
    w[0] = prob2weight(root, p[2]);
    w[1] = prob2weight(root, 255 - p[2]);
    root = prob2weight(p[0], 255 - p[1]);
    w[2] = prob2weight(root, p[3]);
    w[3] = prob2weight(root, 255 - p[3]);
    root = prob2weight(255 - p[0], p[4]);
    w[8] = prob2weight(255 - p[0], 255 - p[4]);
    root1 = prob2weight(root, p[5]);
    root2 = prob2weight(root, 255 - p[5]);
    w[4] = prob2weight(root1, p[6]);
    w[5] = prob2weight(root1, 255 - p[6]);
    w[6] = prob2weight(root2, p[7]);
    w[7] = prob2weight(root2, 255 - p[7]);
    huff_build(h, w, 9);
}
/* -1 on a bad code or the end of data */
static int br_huff(BR *b, const Huff *h) {
    u32 peek = br_peek(b, 16); int i;
    for (i = 0; i < h->n; i++) {
        if ((peek >> (16 - h->bits[i])) == (u32)h->codes[i]) {
            br_read(b, h->bits[i]);
            return b->err ? -1 : i;
        }
    }
    b->err = 1;
    return -1;
}

/* ---------------------------------------------------------------- */
/* decoder state                                                    */

enum { MB_INTRA, MB_INTER_NOMV, MB_INTER_MV, MB_INTER_NEAREST, MB_INTER_NEAR, MB_INTER_FOURMV,
       MB_GOLDEN_NOMV, MB_GOLDEN_MV, MB_GOLDEN_NEAREST, MB_GOLDEN_NEAR };
#define VP_REF_INTER  1
#define VP_REF_GOLDEN 2
static int mb_ref_id(int t) { return t == MB_INTRA ? 0 : t <= MB_INTER_FOURMV ? VP_REF_INTER : VP_REF_GOLDEN; }
static const u8 map_mb_type[10] = { 1, 0, 2, 3, 4, 7, 5, 6, 8, 9 };

typedef struct { s16 x, y; } MV;
typedef struct { int type; MV mv; } MBInfo;

typedef struct {
    int is_intra, is_golden, quant, multistream, use_huffman, version, profile, interlaced;
    int offset, mb_w, mb_h, disp_w, disp_h, scale;
} Hdr;

typedef struct {
    int mb_x, mb_y, plane;
    u8 coeff_cat[4];
    int last_idx[4];
    int top_ctx, ctx_idx;
    int dc_quant, ac_quant;
    int dc_zero_run[2], ac_zero_run[2];
} FState;

typedef struct { u8 *buf; u8 *pl[4]; } Frame;
#define NFRAMES 4
#define INVALID_REF 42

typedef struct { int type; BC bc; BR br; } CoeffReader;   /* type 0 none, 1 bool, 2 huffman */

struct VP6Dec {
    int has_alpha;
    /* VP6 header parser state (VP6BR) */
    int vpversion, profile, interlaced, do_pm, loop_mode, autosel_pm, var_thresh, mv_thresh, bicubic, filter_alpha;

    int w, h, mb_w, mb_h;
    Models models, amodels;
    s16 coeffs[6][64];
    int last_mbt;
    int loop_thr, ilace_prob, ilace_mb;
    MBInfo *mb_info;
    FState fs;
    /* DC prediction */
    s16 *dc_y, *dc_u, *dc_v;
    s16 ldc_y[2], ldc_u, ldc_v;
    u8 *ref_y, *ref_c;
    int ref_left, y_idx, c_idx;
    s16 last_dc[3][4];
    u8 *top_ctx[4];

    Frame frm[NFRAMES];
    int last, golden;          /* frame indices, -1 none */
    Bitmap *img;
    u32 *img_pm;   /* VP6A: the premultiplied texture (colour clamped to alpha) */
};

static void free_dims(VP6Dec *d) {
    int i;
    for (i = 0; i < NFRAMES; i++) { free(d->frm[i].buf); d->frm[i].buf = NULL; }
    for (i = 0; i < 4; i++) { free(d->top_ctx[i]); d->top_ctx[i] = NULL; }
    free(d->mb_info); free(d->dc_y); free(d->dc_u); free(d->dc_v); free(d->ref_y); free(d->ref_c);
    d->mb_info = NULL; d->dc_y = d->dc_u = d->dc_v = NULL; d->ref_y = d->ref_c = NULL;
    d->last = d->golden = -1;
    d->w = d->h = d->mb_w = d->mb_h = 0;
}

static int set_dimensions(VP6Dec *d, int mb_w, int mb_h) {
    size_t n = (size_t)mb_w * 2 + 2, nc = (size_t)mb_w + 2;
    free_dims(d);
    d->mb_info = (MBInfo*)calloc((size_t)mb_w * mb_h, sizeof(MBInfo));
    d->dc_y = (s16*)calloc(n, sizeof(s16));
    d->dc_u = (s16*)calloc(nc, sizeof(s16));
    d->dc_v = (s16*)calloc(nc, sizeof(s16));
    d->ref_y = (u8*)malloc(n);
    d->ref_c = (u8*)malloc(nc);
    d->top_ctx[0] = (u8*)calloc((size_t)mb_w * 2, 1);
    d->top_ctx[1] = (u8*)calloc((size_t)mb_w, 1);
    d->top_ctx[2] = (u8*)calloc((size_t)mb_w, 1);
    d->top_ctx[3] = (u8*)calloc((size_t)mb_w * 2, 1);
    if (!d->mb_info || !d->dc_y || !d->dc_u || !d->dc_v || !d->ref_y || !d->ref_c ||
        !d->top_ctx[0] || !d->top_ctx[1] || !d->top_ctx[2] || !d->top_ctx[3]) { free_dims(d); return 0; }
    memset(d->ref_y, INVALID_REF, n);
    memset(d->ref_c, INVALID_REF, nc);
    d->ref_c[0] = 0;
    d->mb_w = mb_w; d->mb_h = mb_h; d->w = mb_w * 16; d->h = mb_h * 16;
    return 1;
}

/* a frame buffer not used as a reference (nor as `excl`) */
static int get_free_frame(VP6Dec *d, int excl) {
    int i;
    for (i = 0; i < NFRAMES; i++) {
        Frame *f; size_t ls, cs;
        if (i == d->last || i == d->golden || i == excl) continue;
        f = &d->frm[i];
        ls = (size_t)d->w * d->h; cs = (size_t)(d->w / 2) * (d->h / 2);
        if (!f->buf) {
            f->buf = (u8*)calloc(ls * (d->has_alpha ? 2 : 1) + cs * 2, 1);
            if (!f->buf) return -1;
        }
        f->pl[0] = f->buf; f->pl[1] = f->buf + ls; f->pl[2] = f->pl[1] + cs;
        f->pl[3] = d->has_alpha ? f->pl[2] + cs : NULL;
        return i;
    }
    return -1;
}
static void plane_dims(const VP6Dec *d, int plane, int *w, int *h) {
    if (plane == 1 || plane == 2) { *w = d->w / 2; *h = d->h / 2; }
    else { *w = d->w; *h = d->h; }
}

/* ---------------------------------------------------------------- */
/* header and model parsing (vp6.rs VP6BR)                          */

static int parse_header(VP6Dec *d, BC *bc, Hdr *hdr) {
    BR br;
    memset(hdr, 0, sizeof(*hdr));
    br_init(&br, bc->src, bc->len);
    hdr->is_intra = !br_read(&br, 1);
    hdr->is_golden = hdr->is_intra;
    hdr->quant = (int)br_read(&br, 6);
    hdr->multistream = (int)br_read(&br, 1);
    if (hdr->is_intra) {
        hdr->version = (int)br_read(&br, 5);
        if (hdr->version < VERSION_VP60 || hdr->version > VERSION_VP62) return 0;
        hdr->profile = (int)br_read(&br, 2);
        if (hdr->profile != VP6_SIMPLE_PROFILE && hdr->profile != VP6_ADVANCED_PROFILE) return 0;
        hdr->interlaced = (int)br_read(&br, 1);
    } else {
        hdr->version = d->vpversion;
        hdr->profile = d->profile;
        hdr->interlaced = d->interlaced;
    }
    if (hdr->multistream || hdr->profile == VP6_SIMPLE_PROFILE) {
        hdr->offset = (int)br_read(&br, 16);
        if (hdr->offset <= (hdr->is_intra ? 6 : 2)) return 0;
        hdr->multistream = 1;
    }
    if (br.err) return 0;
    bc_skip_bytes(bc, br.pos >> 3);
    d->loop_mode = 0;
    if (hdr->is_intra) {
        hdr->mb_h = bc_bits(bc, 8);
        hdr->mb_w = bc_bits(bc, 8);
        hdr->disp_h = bc_bits(bc, 8);
        hdr->disp_w = bc_bits(bc, 8);
        if (hdr->mb_h == 0 || hdr->mb_w == 0) return 0;
        hdr->scale = bc_bits(bc, 2);
    } else {
        hdr->is_golden = bc_bool(bc);
        if (hdr->profile == VP6_ADVANCED_PROFILE) {
            d->loop_mode = bc_bool(bc);
            if (d->loop_mode) {
                d->loop_mode += bc_bool(bc);
                if (d->loop_mode > 1) return 0;
            }
            if (hdr->version == VERSION_VP62) d->do_pm = bc_bool(bc);
        }
    }
    if (hdr->profile == VP6_ADVANCED_PROFILE && (hdr->is_intra || d->do_pm)) {
        d->autosel_pm = bc_bool(bc);
        if (d->autosel_pm) {
            d->var_thresh = bc_bits(bc, 5);
            if (hdr->version != VERSION_VP62) d->var_thresh <<= 5;
            d->mv_thresh = bc_bits(bc, 3);
        } else {
            d->bicubic = bc_bool(bc);
        }
        if (hdr->version == VERSION_VP62) d->filter_alpha = bc_bits(bc, 4);
        else d->filter_alpha = 16;
    }
    hdr->use_huffman = bc_bool(bc);
    d->vpversion = hdr->version;
    d->profile = hdr->profile;
    d->interlaced = hdr->interlaced;
    return 1;
}

static void reset_scan(Models *m, int interlaced) {
    int i;
    memcpy(m->scan_order, interlaced ? VP6_INTERLACED_SCAN_ORDER : VP6_DEFAULT_SCAN_ORDER, 64);
    for (i = 0; i < 64; i++) m->scan[i] = (u8)i;
    memcpy(m->zigzag, ZIGZAG, 64);
}
static void update_scan(Models *m) {
    int idx = 1, band, i;
    for (band = 0; band < 16; band++)
        for (i = 1; i < 64; i++)
            if (m->scan_order[i] == band && idx < 64) m->scan[idx++] = (u8)i;
    for (i = 1; i < 64; i++) m->zigzag[i] = ZIGZAG[m->scan[i] & 63];
}

static void reset_models(VP6Dec *d, Models *m) {
    int i;
    for (i = 0; i < 2; i++) {
        m->mv_models[i].nz_prob = NZ_PROBS[i];
        m->mv_models[i].sign_prob = 128;
        memcpy(m->mv_models[i].raw_probs, RAW_PROBS[i], 8);
        memcpy(m->mv_models[i].tree_probs, TREE_PROBS[i], 7);
    }
    memcpy(m->zero_run_probs, ZERO_RUN_PROBS, sizeof(ZERO_RUN_PROBS));
    reset_scan(m, d->interlaced);
}

static int decode_mv(BC *bc, const MVModel *m) {
    int val;
    if (!bc_prob(bc, m->nz_prob)) {   /* short vector */
        const u8 *t = m->tree_probs;
        if (!bc_prob(bc, t[0])) {
            if (!bc_prob(bc, t[1])) val = bc_prob(bc, t[2]) ? 1 : 0;
            else val = bc_prob(bc, t[3]) ? 3 : 2;
        } else {
            if (!bc_prob(bc, t[4])) val = bc_prob(bc, t[5]) ? 5 : 4;
            else val = bc_prob(bc, t[6]) ? 7 : 6;
        }
    } else {
        int i;
        val = 0;
        for (i = 0; i < 7; i++) val |= bc_prob(bc, m->raw_probs[LONG_VECTOR_ORDER[i]]) << LONG_VECTOR_ORDER[i];
        if (val & 0xF0) val |= bc_prob(bc, m->raw_probs[3]) << 3;
        else val |= 1 << 3;
    }
    if (val != 0 && bc_prob(bc, m->sign_prob)) return -val;
    return val;
}

static void decode_mv_models(BC *bc, MVModel *m) {
    int comp, i;
    for (comp = 0; comp < 2; comp++) {
        if (bc_prob(bc, HAS_NZ_PROB[comp])) m[comp].nz_prob = (u8)bc_probability(bc);
        if (bc_prob(bc, HAS_SIGN_PROB[comp])) m[comp].sign_prob = (u8)bc_probability(bc);
    }
    for (comp = 0; comp < 2; comp++)
        for (i = 0; i < 7; i++)
            if (bc_prob(bc, HAS_TREE_PROB[comp][i])) m[comp].tree_probs[i] = (u8)bc_probability(bc);
    for (comp = 0; comp < 2; comp++)
        for (i = 0; i < 8; i++)
            if (bc_prob(bc, HAS_RAW_PROB[comp][i])) m[comp].raw_probs[i] = (u8)bc_probability(bc);
}

static u8 rescale_prob(int prob, const s16 *w, int maxval) {
    int v = ((prob * w[0] + 128) >> 8) + w[1];
    if (v > maxval) v = maxval;
    if (v < 1) v = 1;
    return (u8)v;
}

static void decode_coeff_models(VP6Dec *d, BC *bc, Models *m, int is_intra) {
    u8 def_prob[11]; int plane, i, k, ctype, group;
    memset(def_prob, 128, sizeof(def_prob));
    for (plane = 0; plane < 2; plane++)
        for (i = 0; i < 11; i++) {
            if (bc_prob(bc, HAS_COEF_PROBS[plane][i])) {
                def_prob[i] = (u8)bc_probability(bc);
                m->coeff_models[plane].dc_value_probs[i] = def_prob[i];
            } else if (is_intra) {
                m->coeff_models[plane].dc_value_probs[i] = def_prob[i];
            }
        }
    if (bc_bool(bc)) {
        for (i = 1; i < 64; i++)
            if (bc_prob(bc, HAS_SCAN_UPD_PROBS[i])) m->scan_order[i] = (u8)bc_bits(bc, 4);
        update_scan(m);
    } else {
        reset_scan(m, d->interlaced);
    }
    for (plane = 0; plane < 2; plane++)
        for (i = 0; i < 14; i++)
            if (bc_prob(bc, HAS_ZERO_RUN_PROBS[plane][i])) m->zero_run_probs[plane][i] = (u8)bc_probability(bc);
    for (ctype = 0; ctype < 3; ctype++)
        for (plane = 0; plane < 2; plane++)
            for (group = 0; group < 6; group++)
                for (i = 0; i < 11; i++) {
                    if (bc_prob(bc, VP6_AC_PROBS[ctype][plane][group][i])) {
                        def_prob[i] = (u8)bc_probability(bc);
                        m->coeff_models[plane].ac_val_probs[ctype][group][i] = def_prob[i];
                    } else if (is_intra) {
                        m->coeff_models[plane].ac_val_probs[ctype][group][i] = def_prob[i];
                    }
                }
    for (plane = 0; plane < 2; plane++) {
        CoeffModel *mdl = &m->coeff_models[plane];
        for (i = 0; i < 3; i++)
            for (k = 0; k < 5; k++)
                mdl->dc_token_probs[i][k] = rescale_prob(mdl->dc_value_probs[k], VP6_DC_WEIGHTS[k][i], 255);
    }
}

static void reset_mbtype_models(Models *m) {
    static const u8 def[3][20] = {
        { 42,  69, 2, 1, 7, 1, 42, 44, 22, 6, 3, 1, 2, 0, 5, 1, 1, 0, 0, 0 },
        {  8, 229, 1, 1, 8, 0,  0,  0,  0, 0, 2, 1, 1, 0, 0, 0, 1, 1, 0, 0 },
        { 35, 122, 1, 1, 6, 1, 34, 46,  0, 0, 2, 1, 1, 0, 1, 0, 1, 1, 0, 0 }
    };
    memcpy(m->prob_xmitted, def, sizeof(def));
}
static u8 rescale_mb_mode_prob(u32 prob, u32 total) { return (u8)(255 * prob / (1 + total)); }

static int decode_mode_prob_models(BC *bc, Models *m) {
    int ctx, i, set, mode;
    for (ctx = 0; ctx < 3; ctx++) {
        if (bc_prob(bc, 174)) {
            int idx = bc_bits(bc, 4);
            for (i = 0; i < 20; i++) m->prob_xmitted[ctx][i ^ 1] = VP56_MODE_VQ[ctx][idx][i];
        }
        if (bc_prob(bc, 254)) {
            for (set = 0; set < 20; set++) {
                if (bc_prob(bc, 205)) {
                    int sign = bc_bool(bc), diff;
                    if (!bc_prob(bc, 171)) diff = bc_prob(bc, 83) ? 1 : 2;
                    else if (!bc_prob(bc, 199)) {
                        if (!bc_prob(bc, 140)) {
                            if (!bc_prob(bc, 125)) diff = bc_prob(bc, 104) ? 5 : 6;
                            else diff = 4;
                        } else diff = 3;
                    } else diff = bc_bits(bc, 7);
                    diff *= 4;
                    if (diff >= 256) return 0;
                    if (!sign) {
                        if (m->prob_xmitted[ctx][set ^ 1] > 255 - diff) return 0;
                        m->prob_xmitted[ctx][set ^ 1] = (u8)(m->prob_xmitted[ctx][set ^ 1] + diff);
                    } else {
                        if (m->prob_xmitted[ctx][set ^ 1] < diff) return 0;
                        m->prob_xmitted[ctx][set ^ 1] = (u8)(m->prob_xmitted[ctx][set ^ 1] - diff);
                    }
                }
            }
        }
    }
    for (ctx = 0; ctx < 3; ctx++) {
        const u8 *px = m->prob_xmitted[ctx];
        for (mode = 0; mode < 10; mode++) {
            u8 *probs = m->mbtype_probs[ctx][mode];
            u32 cnt[10], total = 0, sum, inter_mv0, inter_mv1, gold_mv0, gold_mv1, mix;
            for (i = 0; i < 10; i++) {
                cnt[i] = 0;
                if (i == mode) continue;
                cnt[i] = 100 * (u32)px[i * 2];
                total += cnt[i];
            }
            sum = (u32)px[mode * 2] + px[mode * 2 + 1];
            probs[9] = (u8)(255 - rescale_mb_mode_prob(px[mode * 2 + 1], sum));
            inter_mv0 = cnt[0] + cnt[2];
            inter_mv1 = cnt[3] + cnt[4];
            gold_mv0 = cnt[5] + cnt[6];
            gold_mv1 = cnt[8] + cnt[9];
            mix = cnt[1] + cnt[7];
            probs[0] = (u8)(1 + rescale_mb_mode_prob(inter_mv0 + inter_mv1, total));
            probs[1] = (u8)(1 + rescale_mb_mode_prob(inter_mv0, inter_mv0 + inter_mv1));
            probs[2] = (u8)(1 + rescale_mb_mode_prob(mix, mix + gold_mv0 + gold_mv1));
            probs[3] = (u8)(1 + rescale_mb_mode_prob(cnt[0], inter_mv0));
            probs[4] = (u8)(1 + rescale_mb_mode_prob(cnt[3], inter_mv1));
            probs[5] = (u8)(1 + rescale_mb_mode_prob(cnt[1], mix));
            probs[6] = (u8)(1 + rescale_mb_mode_prob(gold_mv0, gold_mv0 + gold_mv1));
            probs[7] = (u8)(1 + rescale_mb_mode_prob(cnt[5], gold_mv0));
            probs[8] = (u8)(1 + rescale_mb_mode_prob(cnt[8], gold_mv1));
        }
    }
    return 1;
}

/* ---------------------------------------------------------------- */
/* coefficient decoding                                             */

static int expand_token_bc(BC *bc, const u8 *vp, int token) {
    int sign = 0, level;
    if (token < TOKEN_LARGE) {
        if (token != 0) sign = bc_bool(bc);
        level = token;
    } else {
        int cat, add = 0, i;
        if (!bc_prob(bc, vp[6])) cat = bc_prob(bc, vp[7]) ? 1 : 0;
        else if (!bc_prob(bc, vp[8])) cat = bc_prob(bc, vp[9]) ? 3 : 2;
        else cat = bc_prob(bc, vp[10]) ? 5 : 4;
        for (i = 0; i < 12; i++) {
            int p = VP56_COEF_ADD_PROBS[cat][i];
            if (p == 128) break;
            add = (add << 1) | bc_prob(bc, p);
        }
        sign = bc_bool(bc);
        level = VP56_COEF_BASE[cat] + add;
    }
    return sign ? -level : level;
}

static int decode_token_bc(BC *bc, const u8 *probs, int prob34, int is_dc, int has_nnz) {
    if (has_nnz && !bc_prob(bc, probs[0])) {
        if (is_dc || bc_prob(bc, probs[1])) return 0;
        return TOKEN_EOB;
    }
    if (!bc_prob(bc, probs[2])) return 1;
    if (!bc_prob(bc, probs[3])) {
        if (!bc_prob(bc, probs[4])) return 2;
        return bc_prob(bc, prob34) ? 4 : 3;
    }
    return TOKEN_LARGE;
}

static int decode_zero_run_bc(BC *bc, const u8 *p) {
    int val, i;
    if (!bc_prob(bc, p[0])) {
        if (!bc_prob(bc, p[1])) val = bc_prob(bc, p[2]) ? 1 : 0;
        else val = bc_prob(bc, p[3]) ? 3 : 2;
    } else if (!bc_prob(bc, p[4])) {
        if (!bc_prob(bc, p[5])) val = bc_prob(bc, p[6]) ? 5 : 4;
        else val = bc_prob(bc, p[7]) ? 7 : 6;
    } else {
        val = 8;
        for (i = 0; i < 6; i++) val += bc_prob(bc, p[i + 8]) << i;
    }
    return val;
}

static int decode_block_bc(BC *bc, s16 *coeffs, const CoeffModel *m, const Models *vm, FState *fs) {
    int left_ctx = fs->coeff_cat[fs->ctx_idx], dc_mode = fs->top_ctx + left_ctx;
    int token, val, idx, last_val;
    if (dc_mode > 2) dc_mode = 2;
    token = decode_token_bc(bc, m->dc_token_probs[dc_mode], m->dc_value_probs[5], 1, 1);
    val = expand_token_bc(bc, m->dc_value_probs, token);
    coeffs[0] = (s16)val;
    fs->last_idx[fs->ctx_idx] = 0;
    idx = 1; last_val = val;
    while (idx < 64) {
        int ac_band = VP6_IDX_TO_AC_BAND[idx], ac_mode = iabs(last_val);
        const u8 *probs;
        if (ac_mode > 2) ac_mode = 2;
        probs = m->ac_val_probs[ac_mode][ac_band];
        token = decode_token_bc(bc, probs, probs[5], 0, idx == 1 || last_val != 0);
        if (token == TOKEN_EOB) break;
        val = expand_token_bc(bc, probs, token);
        coeffs[vm->zigzag[idx]] = (s16)(val * fs->ac_quant);
        idx++;
        last_val = val;
        if (val == 0) {
            idx += decode_zero_run_bc(bc, vm->zero_run_probs[idx >= 7 ? 1 : 0]);
            if (idx > 64) return 0;
        }
    }
    fs->coeff_cat[fs->ctx_idx] = (u8)(coeffs[0] != 0);
    fs->top_ctx = fs->coeff_cat[fs->ctx_idx];
    fs->last_idx[fs->ctx_idx] = idx;
    return 1;
}

/* value, or sets *eob; 0 on error */
static int decode_token_huff(BR *b, const Huff *h, int *val, int *eob) {
    int tok = br_huff(b, h);
    *eob = 0; *val = 0;
    if (tok < 0) return 0;
    if (tok == 0) return 1;
    if (tok <= 4) { *val = br_read(b, 1) ? -tok : tok; return !b->err; }
    if (tok <= 10) {
        int base = tok - 5, v = VP56_COEF_BASE[base] + (int)br_read(b, VP6_COEF_ADD_BITS[base]);
        *val = br_read(b, 1) ? -v : v;
        return !b->err;
    }
    *eob = 1;
    return 1;
}
static int decode_eob_run_huff(BR *b) {
    int v = (int)br_read(b, 2);
    switch (v) {
    case 0: return 0;
    case 1: return 1;
    case 2: return (int)br_read(b, 2) + 2;
    default:
        if (br_read(b, 1)) return (int)br_read(b, 6) + 10;
        return (int)br_read(b, 2) + 6;
    }
}
static int decode_zero_run_huff(BR *b, const Huff *h) {
    int v = br_huff(b, h);
    if (v < 0) return 0;
    if (v < 8) return v;
    return (int)br_read(b, 6) + 8;
}

static int decode_block_huff(BR *b, s16 *coeffs, const Models *m, FState *fs) {
    int plane = (fs->plane == 0 || fs->plane == 3) ? 0 : 1;
    int last_val, idx, val, eob;
    if (fs->dc_zero_run[plane] == 0) {
        if (!decode_token_huff(b, &m->dc_token_tree[plane], &val, &eob)) return 0;
        if (eob) return 1;
        last_val = val;
        coeffs[0] = (s16)val;
        if (val == 0) {
            fs->dc_zero_run[plane] = decode_eob_run_huff(b);
            if (b->err) return 0;
        }
    } else {
        last_val = 0;
        fs->dc_zero_run[plane]--;
    }
    if (fs->ac_zero_run[plane] > 0) {
        fs->ac_zero_run[plane]--;
        fs->last_idx[fs->ctx_idx] = 0;
        return 1;
    }
    idx = 1;
    while (idx < 64) {
        int ac_band = VP6_IDX_TO_AC_BAND[idx], ac_mode = iabs(last_val);
        if (ac_band > 3) ac_band = 3;
        if (ac_mode > 2) ac_mode = 2;
        if (!decode_token_huff(b, &m->ac_token_tree[plane][ac_mode][ac_band], &val, &eob)) return 0;
        if (eob) {
            if (idx == 1) {
                fs->ac_zero_run[plane] = decode_eob_run_huff(b);
                if (b->err) return 0;
            }
            break;
        }
        coeffs[m->zigzag[idx]] = (s16)(val * fs->ac_quant);
        idx++;
        last_val = val;
        if (val == 0) {
            idx += decode_zero_run_huff(b, &m->zero_run_tree[idx >= 7 ? 1 : 0]);
            if (b->err || idx > 64) return 0;
        }
    }
    fs->last_idx[fs->ctx_idx] = idx;
    return 1;
}

/* ---------------------------------------------------------------- */
/* IDCT and block output (vpcommon.rs)                              */

#define C1S7 64277
#define C2S6 60547
#define C3S5 54491
#define C4S4 46341
#define C5S3 36410
#define C6S2 25080
#define C7S1 12785

static int mul16(int a, int b) { s32 p = (s32)((u32)a * (u32)b); return ASR(p, 16); }

static void idct_step(const int *s, int *dd, int bias, int shift, int first) {
    int t_a, t_b, t_c, t_d, t_a1, t_b1, t_e, t_f, t_g, t_h, t_e1, s04p, s04m;
    t_a  = mul16(C1S7, s[1]) + mul16(C7S1, s[7]);
    t_b  = mul16(C7S1, s[1]) - mul16(C1S7, s[7]);
    t_c  = mul16(C3S5, s[3]) + mul16(C5S3, s[5]);
    t_d  = mul16(C3S5, s[5]) - mul16(C5S3, s[3]);
    t_a1 = mul16(C4S4, t_a - t_c);
    t_b1 = mul16(C4S4, t_b - t_d);
    t_c  = t_a + t_c;
    t_d  = t_b + t_d;
    s04p = s[0] + s[4]; s04m = s[0] - s[4];
    if (first) { s04p = (s16)s04p; s04m = (s16)s04m; }   /* i16 arithmetic in the row pass */
    t_e  = mul16(C4S4, s04p) + bias;
    t_f  = mul16(C4S4, s04m) + bias;
    t_g  = mul16(C2S6, s[2]) + mul16(C6S2, s[6]);
    t_h  = mul16(C6S2, s[2]) - mul16(C2S6, s[6]);
    t_e1 = t_e - t_g;
    t_g  = t_e + t_g;
    t_a  = t_f + t_a1;
    t_f  = t_f - t_a1;
    t_b  = t_b1 - t_h;
    t_h  = t_b1 + t_h;
    dd[0] = ASR(t_g + t_c, shift);
    dd[7] = ASR(t_g - t_c, shift);
    dd[1] = ASR(t_a + t_h, shift);
    dd[2] = ASR(t_a - t_h, shift);
    dd[3] = ASR(t_e1 + t_d, shift);
    dd[4] = ASR(t_e1 - t_d, shift);
    dd[5] = ASR(t_f + t_b, shift);
    dd[6] = ASR(t_f - t_b, shift);
}
static void vp_idct(s16 *c) {
    int tmp[64], s[8], o[8], i, j;
    for (i = 0; i < 8; i++) {
        for (j = 0; j < 8; j++) s[j] = c[i * 8 + j];
        idct_step(s, &tmp[i * 8], 0, 0, 1);
    }
    for (i = 0; i < 8; i++) {
        for (j = 0; j < 8; j++) s[j] = tmp[j * 8 + i];
        idct_step(s, o, 8, 4, 0);
        for (j = 0; j < 8; j++) c[j * 8 + i] = (s16)o[j];
    }
}
static void vp_idct_dc(s16 *c) {
    s16 dc = (s16)ASR(mul16(C4S4, mul16(C4S4, c[0])) + 8, 4);
    int i;
    for (i = 0; i < 64; i++) c[i] = dc;
}
/* put (add == 0) or add the inverse transform at dst; step is the line step */
static void block_out(s16 *c, u8 *dst, int step, int add, int dc_only) {
    int x, y;
    if (dc_only) vp_idct_dc(c); else vp_idct(c);
    for (y = 0; y < 8; y++, dst += step)
        for (x = 0; x < 8; x++)
            dst[x] = clip8(c[y * 8 + x] + (add ? dst[x] : 128));
}

/* ---------------------------------------------------------------- */
/* motion compensation (vp6.rs mc_block)                            */

static void vp31_loop_filter(u8 *data, int off, int step, int stride, int len, int loop_str) {
    int i;
    for (i = 0; i < len; i++, off += stride) {
        int a = data[off - step * 2], b = data[off - step], c = data[off], dd = data[off + step];
        int diff = ASR((a - dd) + 3 * (c - b) + 4, 3);
        if (iabs(diff) >= 2 * loop_str) diff = 0;
        else if (iabs(diff) >= loop_str) diff = diff < 0 ? -diff - 2 * loop_str : -diff + 2 * loop_str;
        if (diff != 0) {
            data[off - step] = clip8(b + diff);
            data[off] = clip8(c - diff);
        }
    }
}

/* the 12x12 block around (dx + mv_x - 2, dy + mv_y - 2), edges replicated */
static void get_block(const VP6Dec *d, u8 *dst, const Frame *src, int comp, int dx, int dy, int mv_x, int mv_y) {
    int w, h, sx = dx + mv_x, sy = dy + mv_y, x, y;
    const u8 *p = src->pl[comp];
    plane_dims(d, comp, &w, &h);
    if (sx - 2 < 0 || sx + 10 > w || sy - 2 < 0 || sy + 10 > h) {
        for (y = 0; y < 12; y++) {
            int yy = sy - 2 + y;
            if (yy < 0) yy = 0; else if (yy >= h) yy = h - 1;
            for (x = 0; x < 12; x++) {
                int xx = sx - 2 + x;
                if (xx < 0) xx = 0; else if (xx >= w) xx = w - 1;
                dst[y * 16 + x] = p[(size_t)yy * w + xx];
            }
        }
    } else {
        const u8 *s = p + (size_t)(sy - 2) * w + (sx - 2);
        for (y = 0; y < 12; y++) memcpy(dst + y * 16, s + (size_t)y * w, 12);
    }
}

static int calc_variance(const u8 *src, int stride) {
    u32 sum = 0, ssum = 0; int y, x;
    for (y = 0; y < 4; y++)
        for (x = 0; x < 8; x += 2) {
            u32 pix = src[y * stride * 2 + x];
            sum += pix; ssum += pix * pix;
        }
    return (int)(u16)((ssum * 16 - sum * sum) >> 8);
}

static u8 bicubic_px(const u8 *s, int off, int step, const s16 *c) {
    int v = s[off - step] * c[0] + s[off] * c[1] + s[off + step] * c[2] + s[off + step * 2] * c[3] + 64;
    return clip8(ASR(v, 7));
}
static void mc_bicubic(u8 *dst, int dstride, const u8 *src, int soff, int sstride, const s16 *cw, const s16 *ch) {
    int x, y;
    if (ch[1] == 128) {
        for (y = 0; y < 8; y++, soff += sstride, dst += dstride)
            for (x = 0; x < 8; x++) dst[x] = bicubic_px(src, soff + x, 1, cw);
    } else if (cw[1] == 128) {
        for (y = 0; y < 8; y++, soff += sstride, dst += dstride)
            for (x = 0; x < 8; x++) dst[x] = bicubic_px(src, soff + x, sstride, ch);
    } else {
        u8 buf[16 * 11]; int boff;
        soff -= sstride;
        for (y = 0; y < 11; y++, soff += sstride)
            for (x = 0; x < 8; x++) buf[y * 16 + x] = bicubic_px(src, soff + x, 1, cw);
        boff = 16;
        for (y = 0; y < 8; y++, boff += 16, dst += dstride)
            for (x = 0; x < 8; x++) dst[x] = bicubic_px(buf, boff + x, 16, ch);
    }
}
#define BILIN(a, b, c) ((u8)(((a) * (8 - (c)) + (b) * (c) + 4) >> 3))
static void mc_bilinear(u8 *dst, int dstride, const u8 *src, int soff, int sstride, int mx, int my) {
    int x, y;
    if (my == 0) {
        for (y = 0; y < 8; y++, soff += sstride, dst += dstride)
            for (x = 0; x < 8; x++) dst[x] = BILIN(src[soff + x], src[soff + x + 1], mx);
    } else if (mx == 0) {
        for (y = 0; y < 8; y++, soff += sstride, dst += dstride)
            for (x = 0; x < 8; x++) dst[x] = BILIN(src[soff + x], src[soff + x + sstride], my);
    } else {
        u8 tmp[8];
        for (x = 0; x < 8; x++) tmp[x] = BILIN(src[soff + x], src[soff + x + 1], mx);
        soff += sstride;
        for (y = 0; y < 8; y++, soff += sstride, dst += dstride)
            for (x = 0; x < 8; x++) {
                u8 cur = BILIN(src[soff + x], src[soff + x + 1], mx);
                dst[x] = BILIN(tmp[x], cur, my);
                tmp[x] = cur;
            }
    }
}

static void mc_block(VP6Dec *d, Frame *dstf, const Frame *src, int plane, int x, int y, MV mv) {
    u8 tmp[16 * 16];
    int is_luma = plane != 1 && plane != 2;
    int sx, sy, mx, my, msx, msy, copy_mode, bicubic, w, h;
    u8 *dbuf;
    if (is_luma) {
        sx = ASR(mv.x, 2); sy = ASR(mv.y, 2); mx = (mv.x & 3) << 1; my = (mv.y & 3) << 1;
        msx = div_tz(mv.x, 4); msy = div_tz(mv.y, 4);
    } else {
        sx = ASR(mv.x, 3); sy = ASR(mv.y, 3); mx = mv.x & 7; my = mv.y & 7;
        msx = div_tz(mv.x, 8); msy = div_tz(mv.y, 8);
    }
    get_block(d, tmp, src, plane, x, y, sx, sy);
    if ((msx & 7) != 0) vp31_loop_filter(tmp, 2 + (8 - (sx & 7)), 1, 16, 12, d->loop_thr);
    if ((msy & 7) != 0) vp31_loop_filter(tmp, (2 + (8 - (sy & 7))) * 16, 16, 1, 12, d->loop_thr);
    copy_mode = mx == 0 && my == 0;
    bicubic = !copy_mode && is_luma && d->bicubic;
    if (is_luma && !copy_mode && d->profile == VP6_ADVANCED_PROFILE) {
        if (!d->autosel_pm) bicubic = 1;
        else {
            int mv_limit = 1 << (d->mv_thresh + 1);
            if (iabs(mv.x) <= mv_limit && iabs(mv.y) <= mv_limit) {
                int var_off = 16 * 2 + 2;
                if (mv.x < 0) var_off += 1;
                if (mv.y < 0) var_off += 16;
                if (calc_variance(tmp + var_off, 16) >= d->var_thresh) bicubic = 1;
            }
        }
    }
    plane_dims(d, plane, &w, &h);
    dbuf = dstf->pl[plane] + (size_t)y * w + x;
    if (copy_mode) {
        int j;
        for (j = 0; j < 8; j++) memcpy(dbuf + (size_t)j * w, tmp + (2 + j) * 16 + 2, 8);
    } else if (bicubic) {
        int fa = d->filter_alpha;
        if (fa < 0 || fa > 16) fa = 16;
        mc_bicubic(dbuf, w, tmp, 16 * 2 + 2, 16, VP6_BICUBIC_COEFFS[fa][mx], VP6_BICUBIC_COEFFS[fa][my]);
    } else {
        mc_bilinear(dbuf, w, tmp, 16 * 2 + 2, 16, mx, my);
    }
}

static const Frame *ref_frame(VP6Dec *d, int ref_id) {
    int i = ref_id == VP_REF_GOLDEN && d->golden >= 0 ? d->golden : d->last;
    return i >= 0 ? &d->frm[i] : NULL;
}

static int do_mc(VP6Dec *d, Frame *cur, int mb_type, MV mv, int alpha) {
    int x = d->fs.mb_x * 16, y = d->fs.mb_y * 16, plane = alpha ? 3 : 0;
    const Frame *src = ref_frame(d, mb_ref_id(mb_type));
    if (!src) return 0;
    mc_block(d, cur, src, plane, x, y, mv);
    mc_block(d, cur, src, plane, x + 8, y, mv);
    mc_block(d, cur, src, plane, x, y + 8, mv);
    mc_block(d, cur, src, plane, x + 8, y + 8, mv);
    if (!alpha) {
        x = d->fs.mb_x * 8; y = d->fs.mb_y * 8;
        mc_block(d, cur, src, 1, x, y, mv);
        mc_block(d, cur, src, 2, x, y, mv);
    }
    return 1;
}
static int do_fourmv(VP6Dec *d, Frame *cur, const MV *mvs, int alpha) {
    int x = d->fs.mb_x * 16, y = d->fs.mb_y * 16, plane = alpha ? 3 : 0, blk;
    const Frame *src = ref_frame(d, VP_REF_INTER);
    if (!src) return 0;
    for (blk = 0; blk < 4; blk++)
        mc_block(d, cur, src, plane, x + (blk & 1) * 8, y + (blk & 2) * 4, mvs[blk]);
    if (!alpha) {
        MV mv; s16 sx, sy;
        sx = (s16)(mvs[0].x + mvs[1].x); sx = (s16)(sx + mvs[2].x); sx = (s16)(sx + mvs[3].x);
        sy = (s16)(mvs[0].y + mvs[1].y); sy = (s16)(sy + mvs[2].y); sy = (s16)(sy + mvs[3].y);
        mv.x = (s16)div_tz(sx, 4); mv.y = (s16)div_tz(sy, 4);
        x = d->fs.mb_x * 8; y = d->fs.mb_y * 8;
        mc_block(d, cur, src, 1, x, y, mv);
        mc_block(d, cur, src, 2, x, y, mv);
    }
    return 1;
}

/* ---------------------------------------------------------------- */
/* macroblocks (vp56.rs)                                            */

static void find_mv_pred(VP6Dec *d, int ref_id, int *num, MV *nearest, MV *nearmv, MV *pred) {
    static const s8 cand[12][2] = {
        {-1, 0}, {0, -1}, {-1, -1}, {-1, 1}, {-2, 0}, {0, -2},
        {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}, {-2, -2}, {-2, 2}
    };
    int i, n = 0;
    nearest->x = nearest->y = 0; nearmv->x = nearmv->y = 0; pred->x = pred->y = 0;
    for (i = 0; i < 12; i++) {
        int cy = d->fs.mb_y + cand[i][0], cx = d->fs.mb_x + cand[i][1];
        const MBInfo *mi;
        if (cx < 0 || cy < 0 || cx >= d->mb_w || cy >= d->mb_h) continue;
        mi = &d->mb_info[cx + cy * d->mb_w];
        if (mb_ref_id(mi->type) != ref_id || (mi->mv.x == 0 && mi->mv.y == 0)) continue;
        if (n == 0) {
            *nearest = mi->mv;
            n++;
            if (i < 2) *pred = mi->mv;     /* version > 5 */
        } else if (mi->mv.x != nearest->x || mi->mv.y != nearest->y) {
            *nearmv = mi->mv;
            n++;
            break;
        }
    }
    *num = n;
}

static MV read_mv(VP6Dec *d, BC *bc, MV pred) {
    MV r; int x = decode_mv(bc, &d->models.mv_models[0]), y = decode_mv(bc, &d->models.mv_models[1]);
    r.x = (s16)(pred.x + x); r.y = (s16)(pred.y + y);
    return r;
}

static int decode_mb_type(VP6Dec *d, BC *bc, int ctx) {
    const u8 *p = d->models.mbtype_probs[ctx][map_mb_type[d->last_mbt]];
    if (!bc_prob(bc, p[9])) {
        int t;
        if (!bc_prob(bc, p[0])) {
            if (!bc_prob(bc, p[1])) t = bc_prob(bc, p[3]) ? MB_INTER_MV : MB_INTER_NOMV;
            else t = bc_prob(bc, p[4]) ? MB_INTER_NEAR : MB_INTER_NEAREST;
        } else {
            if (!bc_prob(bc, p[2])) t = bc_prob(bc, p[5]) ? MB_INTER_FOURMV : MB_INTRA;
            else if (!bc_prob(bc, p[6])) t = bc_prob(bc, p[7]) ? MB_GOLDEN_MV : MB_GOLDEN_NOMV;
            else t = bc_prob(bc, p[8]) ? MB_GOLDEN_NEAR : MB_GOLDEN_NEAREST;
        }
        d->last_mbt = t;
    }
    return d->last_mbt;
}

static void predict_dc(VP6Dec *d, int mb_type, int blk) {
    int is_luma = blk < 4, plane, dc_idx, ref_id = mb_ref_id(mb_type), count = 0;
    s16 *dcs; u8 *dc_ref; s16 dc_pred = 0, dc;
    if (blk == 4) { plane = 1; dcs = d->dc_u; }
    else if (blk == 5) { plane = 2; dcs = d->dc_v; }
    else { plane = 0; dcs = d->dc_y; }
    if (is_luma) { dc_ref = d->ref_y; dc_idx = d->y_idx + (blk & 1); }
    else { dc_ref = d->ref_c; dc_idx = d->c_idx; }
    if ((is_luma && (blk & 1)) || d->ref_left == ref_id) {
        s16 v = blk <= 1 ? d->ldc_y[0] : blk <= 3 ? d->ldc_y[1] : blk == 4 ? d->ldc_u : d->ldc_v;
        dc_pred = (s16)(dc_pred + v);
        count++;
    }
    if (dc_ref[dc_idx] == ref_id) {
        dc_pred = (s16)(dc_pred + dcs[dc_idx]);
        count++;
    }
    if (count == 0) dc_pred = d->last_dc[ref_id][plane];
    else if (count == 2) dc_pred = (s16)div_tz(dc_pred, 2);
    d->coeffs[blk][0] = (s16)(d->coeffs[blk][0] + dc_pred);
    dc = d->coeffs[blk][0];
    if (blk != 4) dc_ref[dc_idx] = (u8)ref_id;
    switch (blk) {
    case 0: case 1: d->ldc_y[0] = dc; break;
    case 2: case 3: d->ldc_y[1] = dc; break;
    case 4: d->ldc_u = dc; break;
    default: d->ldc_v = dc; d->ref_left = ref_id; break;
    }
    dcs[dc_idx] = dc;
    d->last_dc[ref_id][plane] = dc;
    d->coeffs[blk][0] = (s16)(d->coeffs[blk][0] * d->fs.dc_quant);
}

static int read_block(VP6Dec *d, BC *bc, CoeffReader *cr, int blk, int cm) {
    switch (cr->type) {
    case 0: return decode_block_bc(bc, d->coeffs[blk], &d->models.coeff_models[cm], &d->models, &d->fs);
    case 1: return decode_block_bc(&cr->bc, d->coeffs[blk], &d->models.coeff_models[cm], &d->models, &d->fs);
    default: return decode_block_huff(&cr->br, d->coeffs[blk], &d->models, &d->fs);
    }
}

static int decode_mb(VP6Dec *d, Frame *cur, BC *bc, CoeffReader *cr, const Hdr *hdr, int alpha) {
    static const int fourmv_sub[4] = { MB_INTER_NOMV, MB_INTER_MV, MB_INTER_NEAREST, MB_INTER_NEAR };
    int mb_x = d->fs.mb_x, mb_y = d->fs.mb_y, mb_pos = mb_x + mb_y * d->mb_w;
    int num_mv = 0, mb_type, blk, i, intra;
    MV four_mv[4], nearest, nearmv, pred, zero;
    int four_mbt[4];
    MBInfo *mi = &d->mb_info[mb_pos];

    memset(d->coeffs, 0, sizeof(d->coeffs));
    memset(four_mv, 0, sizeof(four_mv));
    zero.x = zero.y = 0;
    nearest = nearmv = pred = zero;

    if (hdr->interlaced) {
        int ip = d->ilace_prob, prob;
        if (mb_x == 0) prob = ip;
        else if (!d->ilace_mb) prob = ip + ((256 - ip) >> 1);
        else prob = ip - (ip >> 1);
        d->ilace_mb = bc_prob(bc, prob);
    }
    if (!hdr->is_intra) find_mv_pred(d, VP_REF_INTER, &num_mv, &nearest, &nearmv, &pred);
    mb_type = hdr->is_intra ? MB_INTRA : decode_mb_type(d, bc, (num_mv + 1) % 3);
    mi->type = mb_type;
    if (mb_ref_id(mb_type) != VP_REF_GOLDEN) {
        switch (mb_type) {
        case MB_INTRA: case MB_INTER_NOMV: mi->mv = zero; break;
        case MB_INTER_MV: mi->mv = read_mv(d, bc, pred); break;
        case MB_INTER_NEAREST: mi->mv = nearest; break;
        case MB_INTER_NEAR: mi->mv = nearmv; break;
        default:   /* four MV */
            for (i = 0; i < 4; i++) four_mbt[i] = fourmv_sub[bc_bits(bc, 2)];
            for (i = 0; i < 4; i++) {
                switch (four_mbt[i]) {
                case MB_INTER_MV: four_mv[i] = read_mv(d, bc, pred); break;
                case MB_INTER_NEAREST: four_mv[i] = nearest; break;
                case MB_INTER_NEAR: four_mv[i] = nearmv; break;
                default: break;
                }
            }
            mi->mv = four_mv[3];
            break;
        }
    } else {
        int n;
        find_mv_pred(d, VP_REF_GOLDEN, &n, &nearest, &nearmv, &pred);
        switch (mb_type) {
        case MB_GOLDEN_NOMV: mi->mv = zero; break;
        case MB_GOLDEN_MV: mi->mv = read_mv(d, bc, pred); break;
        case MB_GOLDEN_NEAREST: mi->mv = nearest; break;
        default: mi->mv = nearmv; break;
        }
    }
    intra = mb_type == MB_INTRA;
    if (!intra && mb_type != MB_INTER_FOURMV) { if (!do_mc(d, cur, mb_type, mi->mv, alpha)) return 0; }
    else if (mb_type == MB_INTER_FOURMV) { if (!do_fourmv(d, cur, four_mv, alpha)) return 0; }

    for (blk = 0; blk < 4; blk++) {
        int bx, by, has_ac, w = d->w;
        u8 *p;
        d->fs.plane = alpha ? 3 : 0;
        d->fs.ctx_idx = blk >> 1;
        d->fs.top_ctx = d->top_ctx[d->fs.plane][mb_x * 2 + (blk & 1)];
        if (!read_block(d, bc, cr, blk, 0)) return 0;
        d->top_ctx[d->fs.plane][mb_x * 2 + (blk & 1)] = (u8)d->fs.top_ctx;
        predict_dc(d, mb_type, blk);
        bx = mb_x * 2 + (blk & 1);
        by = mb_y * 2 + (blk >> 1);
        has_ac = d->fs.last_idx[d->fs.ctx_idx] > 0;
        p = cur->pl[d->fs.plane];
        if (!d->ilace_mb) block_out(d->coeffs[blk], p + (size_t)by * 8 * w + bx * 8, w, !intra, !has_ac);
        else block_out(d->coeffs[blk], p + (size_t)((by & ~1) * 8 + (by & 1)) * w + bx * 8, w * 2, !intra, 0);
    }
    for (blk = 4; blk < 6; blk++) {
        d->fs.plane = blk - 3;
        d->fs.ctx_idx = blk - 2;
        d->fs.top_ctx = d->top_ctx[d->fs.plane][mb_x];
        if (!read_block(d, bc, cr, blk, 1)) return 0;
        d->top_ctx[d->fs.plane][mb_x] = (u8)d->fs.top_ctx;
        predict_dc(d, mb_type, blk);
        if (!alpha) {
            int has_ac = d->fs.last_idx[d->fs.ctx_idx] > 0, cw = d->w / 2;
            block_out(d->coeffs[blk], cur->pl[d->fs.plane] + (size_t)mb_y * 8 * cw + mb_x * 8, cw, !intra, !has_ac);
        }
    }
    return 1;
}

static int decode_planes(VP6Dec *d, Frame *cur, BC *bc, const Hdr *hdr, const u8 *src, size_t srclen, int alpha) {
    CoeffReader cr; int i, j, k, mb_x, mb_y;
    Models *m = &d->models;
    memset(&cr, 0, sizeof(cr));
    if (hdr->multistream) {
        size_t off = (size_t)hdr->offset;
        if (off > srclen) return 0;
        if (!hdr->use_huffman) {
            if (!bc_init(&cr.bc, src + off, srclen - off)) return 0;
            cr.type = 1;
        } else {
            br_init(&cr.br, src + off, srclen - off);
            cr.type = 2;
        }
    }
    if (hdr->is_intra) {
        reset_models(d, m);
        reset_mbtype_models(m);
    } else {
        if (!decode_mode_prob_models(bc, m)) return 0;
        decode_mv_models(bc, m->mv_models);
    }
    decode_coeff_models(d, bc, m, hdr->is_intra);
    if (hdr->use_huffman) {
        for (i = 0; i < 2; i++) huff_build_codes(&m->dc_token_tree[i], m->coeff_models[i].dc_value_probs);
        for (i = 0; i < 2; i++)
            for (j = 0; j < 3; j++)
                for (k = 0; k < 6; k++)
                    huff_build_codes(&m->ac_token_tree[i][j][k], m->coeff_models[i].ac_val_probs[j][k]);
        for (i = 0; i < 2; i++) huff_build_codes_zero_run(&m->zero_run_tree[i], m->zero_run_probs[i]);
    }
    if (hdr->interlaced) d->ilace_prob = bc_bits(bc, 8);

    memset(&d->fs, 0, sizeof(d->fs));
    d->fs.dc_quant = VP56_DC_QUANTS[hdr->quant & 63] * 4;
    d->fs.ac_quant = VP56_AC_QUANTS[hdr->quant & 63] * 4;
    d->loop_thr = VP56_FILTER_LIMITS[hdr->quant & 63];
    d->last_mbt = MB_INTER_NOMV;
    memset(d->top_ctx[0], 0, (size_t)d->mb_w * 2);
    memset(d->top_ctx[1], 0, (size_t)d->mb_w);
    memset(d->top_ctx[2], 0, (size_t)d->mb_w);
    memset(d->top_ctx[3], 0, (size_t)d->mb_w * 2);
    memset(d->last_dc, 0, sizeof(d->last_dc));
    d->last_dc[0][1] = 0x80;
    d->last_dc[0][2] = 0x80;
    /* dc_pred.reset() */
    memset(d->ref_y + 1, INVALID_REF, (size_t)d->mb_w * 2 + 1);
    memset(d->ref_c + 1, INVALID_REF, (size_t)d->mb_w + 1);
    d->ilace_mb = 0;
    for (mb_y = 0; mb_y < d->mb_h; mb_y++) {
        d->fs.mb_y = mb_y;
        memset(d->fs.coeff_cat, 0, sizeof(d->fs.coeff_cat));
        for (i = 0; i < 4; i++) d->fs.last_idx[i] = 24;
        d->y_idx = 1; d->c_idx = 1;
        d->ldc_y[0] = d->ldc_y[1] = 0; d->ldc_u = d->ldc_v = 0;
        d->ref_left = INVALID_REF;
        for (mb_x = 0; mb_x < d->mb_w; mb_x++) {
            d->fs.mb_x = mb_x;
            if (!decode_mb(d, cur, bc, &cr, hdr, alpha)) return 0;
            d->y_idx += 2; d->c_idx += 1;
        }
    }
    return 1;
}

/* ---------------------------------------------------------------- */
/* frames                                                           */

static void copy_alpha(VP6Dec *d, Frame *dst, const Frame *src) {
    memcpy(dst->pl[3], src->pl[3], (size_t)d->w * d->h);
}
static void copy_frame(VP6Dec *d, Frame *dst, const Frame *src) {
    size_t ls = (size_t)d->w * d->h, cs = (size_t)(d->w / 2) * (d->h / 2);
    memcpy(dst->buf, src->buf, ls * (d->has_alpha ? 2 : 1) + cs * 2);
}

static int decode_frame(VP6Dec *d, const u8 *src, size_t len) {
    size_t aoffset; BC bc; Hdr hdr; int ci; Frame *cur;
    const u8 *psrc = src;
    if (d->has_alpha) {
        if (len < 7) return 0;
        aoffset = ((size_t)src[0] << 16) | ((size_t)src[1] << 8) | src[2];
        if (aoffset == 0 || aoffset >= len - 3) return 0;
        if (!bc_init(&bc, src + 3, len - 3)) return 0;
        psrc = src + 3;
    } else {
        if (len < 4) return 0;
        aoffset = len;
        if (!bc_init(&bc, src, len)) return 0;
    }
    if (!parse_header(d, &bc, &hdr)) return 0;
    if ((size_t)hdr.offset >= aoffset) return 0;
    if (hdr.mb_w != 0 && (hdr.mb_w != d->mb_w || hdr.mb_h != d->mb_h))
        if (!set_dimensions(d, hdr.mb_w, hdr.mb_h)) return 0;
    if (d->mb_w == 0) return 0;
    if (hdr.is_intra) d->last = d->golden = -1;
    else if (d->last < 0) return 0;

    ci = get_free_frame(d, -1);
    if (ci < 0) return 0;
    cur = &d->frm[ci];
    if (!decode_planes(d, cur, &bc, &hdr, psrc, aoffset, 0)) return 0;

    if (d->has_alpha) {
        const u8 *asrc = src + aoffset + 3; size_t alen = len - aoffset - 3;
        Hdr ahdr; Models tmp; int ok;
        if (!bc_init(&bc, asrc, alen)) return 0;
        if (!parse_header(d, &bc, &ahdr)) return 0;
        if (ahdr.mb_w != hdr.mb_w || ahdr.mb_h != hdr.mb_h) return 0;
        tmp = d->models; d->models = d->amodels; d->amodels = tmp;
        ok = decode_planes(d, cur, &bc, &ahdr, asrc, alen, 1);
        tmp = d->models; d->models = d->amodels; d->amodels = tmp;
        if (!ok) return 0;
        if (hdr.is_golden && ahdr.is_golden) d->golden = ci;
        else if (hdr.is_golden || ahdr.is_golden) {
            /* only one of the two streams updates its golden frame */
            int gi = d->golden;
            if (gi < 0) d->golden = ci;
            else {
                int ni = get_free_frame(d, ci);
                if (ni < 0) return 0;
                if (hdr.is_golden) { copy_frame(d, &d->frm[ni], cur); copy_alpha(d, &d->frm[ni], &d->frm[gi]); }
                else { copy_frame(d, &d->frm[ni], &d->frm[gi]); copy_alpha(d, &d->frm[ni], cur); }
                d->golden = ni;
            }
        }
    }
    if (hdr.is_golden && !d->has_alpha) d->golden = ci;
    d->last = ci;
    return 1;
}

static int make_picture(VP6Dec *d) {
    const Frame *f; int w = d->w, h = d->h; size_t i, n;
    if (d->last < 0) return 0;
    f = &d->frm[d->last];
    if (!d->img || d->img->w != w || d->img->h != h) {
        bitmap_free(d->img);
        d->img = (Bitmap*)calloc(1, sizeof(Bitmap));
        if (!d->img) return 0;
        d->img->w = w; d->img->h = h;
        d->img->px = (u32*)malloc((size_t)w * h * 4);
        if (!d->img->px) { free(d->img); d->img = NULL; return 0; }
    }
    vdec_yuv420_to_rgba(f->pl[0], w, f->pl[1], f->pl[2], w / 2, f->pl[3], w, w, h, d->img->px);
    d->img->pm = NULL;
    if (d->has_alpha) {
        /* premultiplied colour (clamped to alpha, as Ruffle does) -> straight; the renderer samples the premultiplied copy */
        n = (size_t)w * h;
        { u32 *q = (u32*)realloc(d->img_pm, n * 4 + 4); if (q) { d->img_pm = q; memcpy(q, d->img->px, n * 4); d->img->pm = q; } }
        for (i = 0; i < n; i++) {
            u32 p = d->img->px[i], a = p >> 24, r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
            if (a == 255) continue;
            if (a == 0) { d->img->px[i] = 0; continue; }
            if (r > a) r = a;
            if (g > a) g = a;
            if (b > a) b = a;
            r = (r * 255 + a / 2) / a; g = (g * 255 + a / 2) / a; b = (b * 255 + a / 2) / a;
            d->img->px[i] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    return 1;
}

/* ---------------------------------------------------------------- */
/* interface                                                        */

VP6Dec *vp6_create(int with_alpha) {
    VP6Dec *d = (VP6Dec*)calloc(1, sizeof(VP6Dec));
    if (!d) return NULL;
    d->has_alpha = with_alpha ? 1 : 0;
    d->last = d->golden = -1;
    d->filter_alpha = 16;
    d->last_mbt = MB_INTER_NOMV;
    return d;
}

void vp6_free(VP6Dec *d) {
    if (!d) return;
    free_dims(d);
    free(d->img_pm);
    bitmap_free(d->img);
    free(d);
}

int vp6_decode(VP6Dec *d, const u8 *data, size_t len, Bitmap **out) {
    if (out) *out = NULL;
    if (!d) return 0;
    if (!data || len == 0 || (d->has_alpha && len <= 3)) {
        /* an empty frame repeats the last picture */
        if (out) *out = d->img;
        return d->img != NULL;
    }
    if (!decode_frame(d, data, len)) return 0;
    if (!make_picture(d)) return 0;
    if (out) *out = d->img;
    return 1;
}
