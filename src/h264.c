/* h264.c - H.264 / AVC decoder for FLV video (NetStream), as Ruffle plays
 * it through OpenH264: progressive frames, 4:2:0, 8-bit samples, CABAC
 * (Main / High profiles) and CAVLC (Baseline), I/P/B slices, 8x8 transforms, weighted
 * prediction, spatial and temporal direct prediction.  Pictures come out in
 * the order and with the delay of OpenH264 2.4.1's DecodeFrameNoDelay.
 * Not supported: interlace (field pictures, MBAFF), slice groups, SP/SI
 * slices, 4:0:0 / 4:2:2 / 4:4:4, high bit depths, lossless macroblocks. */
#include "vdec.h"
#include "h264tab.h"
#include <stdlib.h>
#include <string.h>

#define H264_DPB      18
#define H264_MAXREF   32

/* ---------------------------------------------------------------- */
/* bit reader over an RBSP (emulation prevention bytes removed)     */

typedef struct { const u8 *p; u32 len, pos, end; } Bits;   /* pos, end in bits */

static u32 rb1(Bits *b) {
    u32 v = 0;
    if (b->pos < b->end) v = (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
    b->pos++;
    return v;
}
static u32 rbn(Bits *b, int n) { u32 v = 0; while (n-- > 0) v = (v << 1) | rb1(b); return v; }
static u32 rue(Bits *b) {
    int z = 0;
    while (!rb1(b)) { if (++z >= 32 || b->pos > b->end) return 0; }
    return ((1u << z) - 1) + rbn(b, z);
}
static s32 rse(Bits *b) { u32 k = rue(b); return (k & 1) ? (s32)((k + 1) >> 1) : -(s32)(k >> 1); }
static int more_rbsp(Bits *b) {
    /* the last 1 bit of the RBSP is the stop bit */
    u32 last = b->end;
    while (last > 0) { last--; if ((b->p[last >> 3] >> (7 - (last & 7))) & 1) break; }
    return b->pos < last;
}

/* ---------------------------------------------------------------- */
/* parameter sets                                                   */

typedef struct {
    int valid, profile, chroma_format, bit_depth_luma, bit_depth_chroma, bypass;
    int log2_max_frame_num, poc_type, log2_max_poc_lsb, delta_pic_order_always_zero;
    int offset_for_non_ref_pic, offset_for_top_to_bottom, num_ref_frames_in_poc_cycle;
    int offset_for_ref_frame[256];
    int max_num_ref_frames, gaps_allowed, mb_w, mb_h, frame_mbs_only, mbaff, direct_8x8_inference;
    int crop_l, crop_r, crop_t, crop_b;
    int scaling_present;
    u8 sl4[6][16], sl8[6][64];     /* scaling lists in scan order */
} SPS;

typedef struct {
    int valid, sps_id, cabac, bottom_field_pic_order, num_slice_groups, num_ref_idx_default[2];
    int weighted_pred, weighted_bipred, pic_init_qp, chroma_qp_offset[2], deblock_ctrl;
    int constrained_intra, redundant_pic_cnt_present, transform_8x8;
    u8 sl4[6][16], sl8[6][64];     /* the lists in force for this PPS (SPS ones when it has none) */
} PPS;

static const u8 zz4[16] = { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };
static const u8 zz8[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };
static const u8 def4_intra[16] = { 6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 37, 37, 42 };
static const u8 def4_inter[16] = { 10, 14, 14, 20, 20, 20, 24, 24, 24, 24, 27, 27, 27, 30, 30, 34 };
static const u8 def8_intra[64] = {
    6, 10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18, 18, 18, 18, 23, 23, 23, 23, 23, 23, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27,
    27, 27, 27, 27, 29, 29, 29, 29, 29, 29, 29, 31, 31, 31, 31, 31, 31, 33, 33, 33, 33, 33, 36, 36, 36, 36, 38, 38, 38, 40, 40, 42 };
static const u8 def8_inter[64] = {
    9, 13, 13, 15, 13, 15, 17, 17, 17, 17, 19, 19, 19, 19, 19, 21, 21, 21, 21, 21, 21, 22, 22, 22, 22, 22, 22, 22, 24, 24, 24, 24,
    24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27, 27, 28, 28, 28, 28, 28, 30, 30, 30, 30, 32, 32, 32, 33, 33, 35 };

/* scaling_list(): 0 not present, 1 present, 2 use the default list */
static int scaling_list(Bits *b, u8 *list, int n) {
    int last = 8, next = 8, j, use_default = 0;
    for (j = 0; j < n; j++) {
        if (next != 0) {
            next = (last + rse(b) + 256) % 256;
            if (j == 0 && next == 0) { use_default = 1; break; }
        }
        list[j] = (u8)(next == 0 ? last : next);
        last = list[j];
    }
    return use_default ? 2 : 1;
}
/* the scaling matrices with fall-back rule A (SPS, fallback = NULL) or B (PPS, the SPS lists) */
static void parse_scaling(Bits *b, int n8, u8 sl4[6][16], u8 sl8[6][64], const SPS *fallback) {
    int i;
    for (i = 0; i < 6 + n8; i++) {
        int present = (int)rb1(b), r = 0;
        if (i < 6) {
            if (present) r = scaling_list(b, sl4[i], 16);
            if (r == 2) memcpy(sl4[i], i < 3 ? def4_intra : def4_inter, 16);
            else if (r == 0) {
                if (i == 0 || i == 3) memcpy(sl4[i], fallback ? fallback->sl4[i] : (i == 0 ? def4_intra : def4_inter), 16);
                else memcpy(sl4[i], sl4[i - 1], 16);
            }
        } else {
            int k = i - 6;
            if (present) r = scaling_list(b, sl8[k], 64);
            if (r == 2) memcpy(sl8[k], (k & 1) ? def8_inter : def8_intra, 64);
            else if (r == 0) {
                if (k < 2) memcpy(sl8[k], fallback ? fallback->sl8[k] : (k == 0 ? def8_intra : def8_inter), 64);
                else memcpy(sl8[k], sl8[k - 2], 64);
            }
        }
    }
}
static void flat_lists(u8 sl4[6][16], u8 sl8[6][64]) { memset(sl4, 16, 6 * 16); memset(sl8, 16, 6 * 64); }

static void skip_hrd(Bits *b) {
    int n = (int)rue(b) + 1, i;
    rbn(b, 4); rbn(b, 4);
    for (i = 0; i < n && i < 32; i++) { rue(b); rue(b); rb1(b); }
    rbn(b, 5); rbn(b, 5); rbn(b, 5); rbn(b, 5);
}

static int parse_sps(SPS *tab, Bits *b) {
    SPS s; int id, i;
    memset(&s, 0, sizeof(s));
    s.profile = (int)rbn(b, 8); rbn(b, 8); rbn(b, 8);
    id = (int)rue(b);
    if (id >= 32) return 0;
    s.chroma_format = 1; s.bit_depth_luma = s.bit_depth_chroma = 8;
    flat_lists(s.sl4, s.sl8);
    if (s.profile == 100 || s.profile == 110 || s.profile == 122 || s.profile == 244 || s.profile == 44 || s.profile == 83
        || s.profile == 86 || s.profile == 118 || s.profile == 128 || s.profile == 138 || s.profile == 139 || s.profile == 134 || s.profile == 135) {
        s.chroma_format = (int)rue(b);
        if (s.chroma_format == 3) rb1(b);
        s.bit_depth_luma = 8 + (int)rue(b); s.bit_depth_chroma = 8 + (int)rue(b);
        s.bypass = (int)rb1(b);
        s.scaling_present = (int)rb1(b);
        if (s.scaling_present) parse_scaling(b, s.chroma_format != 3 ? 2 : 6, s.sl4, s.sl8, NULL);
    }
    s.log2_max_frame_num = (int)rue(b) + 4;
    s.poc_type = (int)rue(b);
    if (s.poc_type == 0) s.log2_max_poc_lsb = (int)rue(b) + 4;
    else if (s.poc_type == 1) {
        s.delta_pic_order_always_zero = (int)rb1(b);
        s.offset_for_non_ref_pic = rse(b); s.offset_for_top_to_bottom = rse(b);
        s.num_ref_frames_in_poc_cycle = (int)rue(b);
        if (s.num_ref_frames_in_poc_cycle > 255) return 0;
        for (i = 0; i < s.num_ref_frames_in_poc_cycle; i++) s.offset_for_ref_frame[i] = rse(b);
    }
    s.max_num_ref_frames = (int)rue(b);
    s.gaps_allowed = (int)rb1(b);
    s.mb_w = (int)rue(b) + 1; s.mb_h = (int)rue(b) + 1;
    s.frame_mbs_only = (int)rb1(b);
    if (!s.frame_mbs_only) { s.mbaff = (int)rb1(b); s.mb_h *= 2; }
    s.direct_8x8_inference = (int)rb1(b);
    if (rb1(b)) { s.crop_l = (int)rue(b); s.crop_r = (int)rue(b); s.crop_t = (int)rue(b); s.crop_b = (int)rue(b); }
    if (rb1(b)) {   /* VUI: only parsed through to stay in sync */
        int nal_hrd, vcl_hrd;
        if (rb1(b)) { if (rbn(b, 8) == 255) { rbn(b, 16); rbn(b, 16); } }
        if (rb1(b)) rb1(b);
        if (rb1(b)) { rbn(b, 3); rb1(b); if (rb1(b)) { rbn(b, 8); rbn(b, 8); rbn(b, 8); } }
        if (rb1(b)) { rue(b); rue(b); }
        if (rb1(b)) { rbn(b, 32); rbn(b, 32); rb1(b); }
        nal_hrd = (int)rb1(b); if (nal_hrd) skip_hrd(b);
        vcl_hrd = (int)rb1(b); if (vcl_hrd) skip_hrd(b);
        if (nal_hrd || vcl_hrd) rb1(b);
        rb1(b);
    }
    if (s.mb_w > 512 || s.mb_h > 512) return 0;
    s.valid = 1;
    tab[id] = s;
    return 1;
}

static int parse_pps(PPS *tab, const SPS *sps, Bits *b) {
    PPS p; int id;
    memset(&p, 0, sizeof(p));
    id = (int)rue(b);
    if (id >= 256) return 0;
    p.sps_id = (int)rue(b);
    if (p.sps_id >= 32) return 0;
    p.cabac = (int)rb1(b);
    p.bottom_field_pic_order = (int)rb1(b);
    p.num_slice_groups = (int)rue(b) + 1;
    if (p.num_slice_groups > 1) return 0;   /* FMO is not supported */
    p.num_ref_idx_default[0] = (int)rue(b) + 1; p.num_ref_idx_default[1] = (int)rue(b) + 1;
    p.weighted_pred = (int)rb1(b); p.weighted_bipred = (int)rbn(b, 2);
    p.pic_init_qp = 26 + rse(b); rse(b);
    p.chroma_qp_offset[0] = p.chroma_qp_offset[1] = rse(b);
    p.deblock_ctrl = (int)rb1(b); p.constrained_intra = (int)rb1(b); p.redundant_pic_cnt_present = (int)rb1(b);
    memcpy(p.sl4, sps[p.sps_id].sl4, sizeof(p.sl4)); memcpy(p.sl8, sps[p.sps_id].sl8, sizeof(p.sl8));
    if (more_rbsp(b)) {
        p.transform_8x8 = (int)rb1(b);
        if (rb1(b)) {
            if (!sps[p.sps_id].scaling_present) { SPS f; flat_lists(f.sl4, f.sl8); memcpy(f.sl4, def4_intra, 16); memcpy(f.sl4[3], def4_inter, 16); memcpy(f.sl8[0], def8_intra, 64); memcpy(f.sl8[1], def8_inter, 64); parse_scaling(b, p.transform_8x8 ? 2 : 0, p.sl4, p.sl8, &f); }
            else parse_scaling(b, p.transform_8x8 ? 2 : 0, p.sl4, p.sl8, &sps[p.sps_id]);
        }
        p.chroma_qp_offset[1] = rse(b);
    }
    p.valid = 1;
    tab[id] = p;
    return 1;
}

/* ---------------------------------------------------------------- */
/* pictures, macroblocks, decoder state                             */

typedef struct H264Pic {
    u8 *plane[3];
    int poc, frame_num, frame_num_wrap, pic_num, long_term_pic_num, long_term_frame_idx;
    int ref;                    /* 0 unused for reference, 1 short-term, 2 long-term */
    int busy;                   /* the picture being decoded */
    int mmco5;
    /* motion of every 4x4 block (mb * 16 + raster index), for direct prediction */
    s16 (*mv)[2][2];
    s8 (*ref_idx)[2];
    int (*ref_poc)[2][4];       /* per macroblock and 8x8: POC of the picture used (temporal direct) */
    u8 (*ref_lt)[2][4];
    u8 *intra;
} H264Pic;

enum { MB_I4, MB_I8, MB_I16, MB_PCM, MB_P, MB_PSKIP, MB_B, MB_BSKIP, MB_BDIRECT };
#define IS_INTRA(t) ((t) <= MB_PCM)
#define IS_SKIP(t) ((t) == MB_PSKIP || (t) == MB_BSKIP)

typedef struct {
    int slice;                  /* slice number + 1 in this picture; 0 not yet decoded */
    u8 type, t8x8, cbp, chroma_mode, qp, qpc[2], cbf_dc;
    s8 deblock_idc, alpha_off, beta_off;
    u8 nz[16];                  /* luma coefficient counts, raster 4x4 */
    u8 nzc[2][4];               /* chroma AC counts */
    s8 ipred[16];               /* intra 4x4 / 8x8 modes, raster 4x4 (-1: not intra NxN) */
    s8 ref[2][4];               /* reference indices per 8x8 (-1 unused) */
    u8 direct8[4];              /* 8x8 predicted in direct mode */
    s16 mv[2][16][2];           /* motion vectors per raster 4x4 */
    u8 mvd[2][16][2];           /* |mvd| (clipped to 64) for CABAC contexts */
    H264Pic *refpic[2][4];      /* reference pictures per 8x8 (deblocking) */
    s8 last_dqp;                /* mb_qp_delta != 0 (CABAC context of the next one) */
} MB;

typedef struct {
    int first_mb, type, pps_id, frame_num, idr, idr_pic_id, poc_lsb, delta_poc_bottom, delta_poc[2];
    int nal_ref_idc, nal_type, redundant_pic_cnt;
    int direct_spatial, num_ref[2], cabac_init_idc, qp, disable_deblock, alpha_off, beta_off;
    int luma_log2_wd, chroma_log2_wd;
    int lw[2][32], lo[2][32], cw[2][32][2], co[2][32][2];
    int no_output_of_prior, long_term_ref, adaptive, nmmco, mmco[66][3];
    int nmod[2], mod_idc[2][34], mod_val[2][34];
} SliceHdr;

enum { SL_P = 0, SL_B = 1, SL_I = 2, SL_SP = 3, SL_SI = 4 };

typedef struct { int poc, seq; u32 dts; u8 *yuv; int used; } Pending;

struct H264Dec {
    SPS sps[32];
    PPS pps[256];
    int length_size, configured;
    u8 *rbsp; size_t rbsp_cap;
    /* active stream geometry */
    const SPS *s; const PPS *p;
    int mbw, mbh, w, h;         /* coded size in MBs and samples */
    int outw, outh, cropx, cropy;
    H264Pic dpb[H264_DPB];
    H264Pic *cur;
    SliceHdr sh;                /* the current slice */
    SliceHdr first;             /* the picture's first slice */
    int have_pic, nslices, mbs_done;
    MB *mbs;
    /* picture order and frame numbers (previous reference picture etc.) */
    int prev_poc_msb, prev_poc_lsb, prev_frame_num_offset, prev_frame_num, prev_ref_frame_num;
    int prev_has_mmco5, prev_mmco5_poc_top;
    int frame_num_offset, max_long_term_idx;
    /* reference lists of the slice */
    H264Pic *list[2][H264_MAXREF + 1];
    int implicit_w[32][32];     /* implicit bi-prediction weight of list 1 (w0 = 64 - w1), -1 default */
    int dist_scale[32];         /* temporal direct DistScaleFactor per list 0 index */
    int map_col[2][32];         /* temporal direct: list 0 index of each colocated reference */
    /* CABAC */
    u8 cst[460];                /* (pStateIdx << 1) | valMPS */
    u32 crange, coff;
    Bits bits;
    /* slice decoding */
    int qp, last_dqp, mbx, mby, mbaddr;
    /* output re-ordering (OpenH264 2.4.1) */
    Pending pend[16];
    int npend, largest, last_poc, last_seq, has_b, seq;
    u32 dts;
    Bitmap *img;
    int out_ready, reorder_poc;
};

/* ---------------------------------------------------------------- */
/* CABAC decoding engine (9.3.1.2, 9.3.3.2)                         */

static void cabac_init_ctx(H264Dec *d, int slice_type, int idc, int qp) {
    int i, col = slice_type == SL_I ? 0 : idc + 1;
    for (i = 0; i < 460; i++) {
        int m = h264_cabac_init[i][col][0], n = h264_cabac_init[i][col][1], pre = ((m * (qp < 0 ? 0 : qp > 51 ? 51 : qp)) >> 4) + n;
        pre = pre < 1 ? 1 : pre > 126 ? 126 : pre;
        d->cst[i] = (u8)(pre <= 63 ? ((63 - pre) << 1) : (((pre - 64) << 1) | 1));
    }
}
static void cabac_init_engine(H264Dec *d) { d->crange = 510; d->coff = rbn(&d->bits, 9); }
static int cabac_bin(H264Dec *d, int ctx) {
    u32 s = d->cst[ctx], state = s >> 1, mps = s & 1, lps = h264_range_lps[state][(d->crange >> 6) & 3], bin;
    d->crange -= lps;
    if (d->coff >= d->crange) {
        bin = !mps; d->coff -= d->crange; d->crange = lps;
        if (state == 0) mps = !mps;
        state = h264_trans_lps[state];
    } else { bin = mps; state = h264_trans_mps[state]; }
    d->cst[ctx] = (u8)((state << 1) | mps);
    while (d->crange < 256) { d->crange <<= 1; d->coff = (d->coff << 1) | rb1(&d->bits); }
    return (int)bin;
}
static int cabac_bypass(H264Dec *d) {
    d->coff = (d->coff << 1) | rb1(&d->bits);
    if (d->coff >= d->crange) { d->coff -= d->crange; return 1; }
    return 0;
}
static int cabac_term(H264Dec *d) {
    d->crange -= 2;
    if (d->coff >= d->crange) return 1;
    while (d->crange < 256) { d->crange <<= 1; d->coff = (d->coff << 1) | rb1(&d->bits); }
    return 0;
}
/* k-th order Exp-Golomb suffix in bypass bins */
static int cabac_eg(H264Dec *d, int k) {
    int v = 0;
    while (cabac_bypass(d)) { v += 1 << k; if (++k >= 24) return v; }
    while (k-- > 0) v += cabac_bypass(d) << k;
    return v;
}

/* ---------------------------------------------------------------- */
/* slice header (7.3.3)                                             */

static int parse_slice_header(H264Dec *d, Bits *b, SliceHdr *h, int nal_type, int nal_ref_idc) {
    const SPS *s; const PPS *p; int i, l;
    memset(h, 0, sizeof(*h));
    h->nal_type = nal_type; h->nal_ref_idc = nal_ref_idc; h->idr = nal_type == 5;
    h->first_mb = (int)rue(b);
    h->type = (int)rue(b) % 5;
    h->pps_id = (int)rue(b);
    if (h->pps_id >= 256 || !d->pps[h->pps_id].valid) return 0;
    p = &d->pps[h->pps_id]; s = &d->sps[p->sps_id];
    if (!s->valid || s->chroma_format != 1 || s->bit_depth_luma != 8 || s->bit_depth_chroma != 8 || !s->frame_mbs_only || s->bypass) return 0;
    if (h->type == SL_SP || h->type == SL_SI) return 0;
    h->frame_num = (int)rbn(b, s->log2_max_frame_num);
    if (h->idr) h->idr_pic_id = (int)rue(b);
    if (s->poc_type == 0) {
        h->poc_lsb = (int)rbn(b, s->log2_max_poc_lsb);
        if (p->bottom_field_pic_order) h->delta_poc_bottom = rse(b);
    }
    if (s->poc_type == 1 && !s->delta_pic_order_always_zero) {
        h->delta_poc[0] = rse(b);
        if (p->bottom_field_pic_order) h->delta_poc[1] = rse(b);
    }
    if (p->redundant_pic_cnt_present) h->redundant_pic_cnt = (int)rue(b);
    if (h->type == SL_B) h->direct_spatial = (int)rb1(b);
    h->num_ref[0] = p->num_ref_idx_default[0]; h->num_ref[1] = p->num_ref_idx_default[1];
    if (h->type == SL_P || h->type == SL_B) {
        if (rb1(b)) {
            h->num_ref[0] = (int)rue(b) + 1;
            if (h->type == SL_B) h->num_ref[1] = (int)rue(b) + 1;
        }
    }
    if (h->type != SL_B) h->num_ref[1] = 0;
    if (h->type == SL_I) h->num_ref[0] = 0;
    if (h->num_ref[0] > 32 || h->num_ref[1] > 32) return 0;
    for (l = 0; l < 2; l++) {
        if ((l == 0 && h->type != SL_I) || (l == 1 && h->type == SL_B)) {
            if (rb1(b)) {
                for (i = 0; i < 33; i++) {
                    int idc = (int)rue(b);
                    if (idc == 3) break;
                    if (idc > 5) return 0;
                    h->mod_idc[l][i] = idc; h->mod_val[l][i] = (int)rue(b);
                }
                h->nmod[l] = i;
            }
        }
    }
    if ((p->weighted_pred && h->type == SL_P) || (p->weighted_bipred == 1 && h->type == SL_B)) {
        h->luma_log2_wd = (int)rue(b) & 7;
        h->chroma_log2_wd = (int)rue(b) & 7;
        for (l = 0; l < (h->type == SL_B ? 2 : 1); l++) {
            for (i = 0; i < h->num_ref[l]; i++) {
                h->lw[l][i] = 1 << h->luma_log2_wd; h->lo[l][i] = 0;
                h->cw[l][i][0] = h->cw[l][i][1] = 1 << h->chroma_log2_wd; h->co[l][i][0] = h->co[l][i][1] = 0;
                if (rb1(b)) { h->lw[l][i] = rse(b); h->lo[l][i] = rse(b); }
                if (rb1(b)) { int c; for (c = 0; c < 2; c++) { h->cw[l][i][c] = rse(b); h->co[l][i][c] = rse(b); } }
            }
        }
    }
    if (nal_ref_idc) {
        if (h->idr) { h->no_output_of_prior = (int)rb1(b); h->long_term_ref = (int)rb1(b); }
        else {
            h->adaptive = (int)rb1(b);
            if (h->adaptive) {
                for (i = 0; i < 66; i++) {
                    int op = (int)rue(b);
                    if (op == 0) break;
                    if (op > 6) return 0;
                    h->mmco[i][0] = op; h->mmco[i][1] = 0; h->mmco[i][2] = 0;
                    if (op == 1 || op == 3) h->mmco[i][1] = (int)rue(b);
                    if (op == 2) h->mmco[i][1] = (int)rue(b);
                    if (op == 3 || op == 6) h->mmco[i][2] = (int)rue(b);
                    if (op == 4) h->mmco[i][1] = (int)rue(b);
                }
                h->nmmco = i;
            }
        }
    }
    if (p->cabac && h->type != SL_I) { h->cabac_init_idc = (int)rue(b); if (h->cabac_init_idc > 2) return 0; }
    h->qp = p->pic_init_qp + rse(b);
    if (h->qp < 0 || h->qp > 51) return 0;
    if (p->deblock_ctrl) {
        h->disable_deblock = (int)rue(b);
        if (h->disable_deblock != 1) { h->alpha_off = rse(b) * 2; h->beta_off = rse(b) * 2; }
    }
    return 1;
}

/* ---------------------------------------------------------------- */
/* CAVLC (9.2)                                                      */

/* coeff_token code lengths and values, [nC class][TotalCoeff * 4 + TrailingOnes] */
static const u8 ct_len[4][68] = {
    { 1, 0, 0, 0, 6, 2, 0, 0, 8, 6, 3, 0, 9, 8, 7, 5, 10, 9, 8, 6, 11, 10, 9, 7, 13, 11, 10, 8, 13, 13, 11, 9, 13, 13, 13, 10,
      14, 14, 13, 11, 14, 14, 14, 13, 15, 15, 14, 14, 15, 15, 15, 14, 16, 15, 15, 15, 16, 16, 16, 15, 16, 16, 16, 16, 16, 16, 16, 16 },
    { 2, 0, 0, 0, 6, 2, 0, 0, 6, 5, 3, 0, 7, 6, 6, 4, 8, 6, 6, 4, 8, 7, 7, 5, 9, 8, 8, 6, 11, 9, 9, 6, 11, 11, 11, 7,
      12, 11, 11, 9, 12, 12, 12, 11, 12, 12, 12, 11, 13, 13, 13, 12, 13, 13, 13, 13, 13, 14, 13, 13, 14, 14, 14, 13, 14, 14, 14, 14 },
    { 4, 0, 0, 0, 6, 4, 0, 0, 6, 5, 4, 0, 6, 5, 5, 4, 7, 5, 5, 4, 7, 5, 5, 4, 7, 6, 6, 4, 7, 6, 6, 4, 8, 7, 7, 5,
      8, 8, 7, 6, 9, 8, 8, 7, 9, 9, 8, 8, 9, 9, 9, 8, 10, 9, 9, 9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10 },
    { 6, 0, 0, 0, 6, 6, 0, 0, 6, 6, 6, 0, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
      6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6 } };
static const u8 ct_bits[4][68] = {
    { 1, 0, 0, 0, 5, 1, 0, 0, 7, 4, 1, 0, 7, 6, 5, 3, 7, 6, 5, 3, 7, 6, 5, 4, 15, 6, 5, 4, 11, 14, 5, 4, 8, 10, 13, 4,
      15, 14, 9, 4, 11, 10, 13, 12, 15, 14, 9, 12, 11, 10, 13, 8, 15, 1, 9, 12, 11, 14, 13, 8, 7, 10, 9, 12, 4, 6, 5, 8 },
    { 3, 0, 0, 0, 11, 2, 0, 0, 7, 7, 3, 0, 7, 10, 9, 5, 7, 6, 5, 4, 4, 6, 5, 6, 7, 6, 5, 8, 15, 6, 5, 4, 11, 14, 13, 4,
      15, 10, 9, 4, 11, 14, 13, 12, 8, 10, 9, 8, 15, 14, 13, 12, 11, 10, 9, 12, 7, 11, 6, 8, 9, 8, 10, 1, 7, 6, 5, 4 },
    { 15, 0, 0, 0, 15, 14, 0, 0, 11, 15, 13, 0, 8, 12, 14, 12, 15, 10, 11, 11, 11, 8, 9, 10, 9, 14, 13, 9, 8, 10, 9, 8, 15, 14, 13, 13,
      11, 14, 10, 12, 15, 10, 13, 12, 11, 14, 9, 12, 8, 10, 13, 8, 13, 7, 9, 12, 9, 12, 11, 10, 5, 8, 7, 6, 1, 4, 3, 2 },
    { 3, 0, 0, 0, 0, 1, 0, 0, 4, 5, 6, 0, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
      32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63 } };
static const u8 ctc_len[20] = { 2, 0, 0, 0, 6, 1, 0, 0, 6, 6, 3, 0, 6, 7, 7, 6, 6, 8, 8, 7 };
static const u8 ctc_bits[20] = { 1, 0, 0, 0, 7, 1, 0, 0, 4, 6, 1, 0, 3, 3, 2, 5, 2, 3, 2, 0 };
static const u8 tz_len[15][16] = {
    { 1, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 9 }, { 3, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 6, 6, 6, 6 },
    { 4, 3, 3, 3, 4, 4, 3, 3, 4, 5, 5, 6, 5, 6 }, { 5, 3, 4, 4, 3, 3, 3, 4, 3, 4, 5, 5, 5 }, { 4, 4, 4, 3, 3, 3, 3, 3, 4, 5, 4, 5 },
    { 6, 5, 3, 3, 3, 3, 3, 3, 4, 3, 6 }, { 6, 5, 3, 3, 3, 2, 3, 4, 3, 6 }, { 6, 4, 5, 3, 2, 2, 3, 3, 6 }, { 6, 6, 4, 2, 2, 3, 2, 5 },
    { 5, 5, 3, 2, 2, 2, 4 }, { 4, 4, 3, 3, 1, 3 }, { 4, 4, 2, 1, 3 }, { 3, 3, 1, 2 }, { 2, 2, 1 }, { 1, 1 } };
static const u8 tz_bits[15][16] = {
    { 1, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 1 }, { 7, 6, 5, 4, 3, 5, 4, 3, 2, 3, 2, 3, 2, 1, 0 },
    { 5, 7, 6, 5, 4, 3, 4, 3, 2, 3, 2, 1, 1, 0 }, { 3, 7, 5, 4, 6, 5, 4, 3, 3, 2, 2, 1, 0 }, { 5, 4, 3, 7, 6, 5, 4, 3, 2, 1, 1, 0 },
    { 1, 1, 7, 6, 5, 4, 3, 2, 1, 1, 0 }, { 1, 1, 5, 4, 3, 3, 2, 1, 1, 0 }, { 1, 1, 1, 3, 3, 2, 2, 1, 0 }, { 1, 0, 1, 3, 2, 1, 1, 1 },
    { 1, 0, 1, 3, 2, 1, 1 }, { 0, 1, 1, 2, 1, 3 }, { 0, 1, 1, 1, 1 }, { 0, 1, 1, 1 }, { 0, 1, 1 }, { 0, 1 } };
static const u8 tzc_len[3][4] = { { 1, 2, 3, 3 }, { 1, 2, 2, 0 }, { 1, 1, 0, 0 } };
static const u8 tzc_bits[3][4] = { { 1, 1, 1, 0 }, { 1, 1, 0, 0 }, { 1, 0, 0, 0 } };
static const u8 run_len[7][15] = { { 1, 1 }, { 1, 2, 2 }, { 2, 2, 2, 2 }, { 2, 2, 2, 3, 3 }, { 2, 2, 3, 3, 3, 3 }, { 2, 3, 3, 3, 3, 3, 3 },
    { 3, 3, 3, 3, 3, 3, 3, 4, 5, 6, 7, 8, 9, 10, 11 } };
static const u8 run_bits[7][15] = { { 1, 0 }, { 1, 1, 0 }, { 3, 2, 1, 0 }, { 3, 2, 1, 1, 0 }, { 3, 2, 3, 2, 1, 0 }, { 3, 0, 1, 3, 2, 5, 4 },
    { 7, 6, 5, 4, 3, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1 } };
static const u8 cbp_intra[48] = { 47, 31, 15, 0, 23, 27, 29, 30, 7, 11, 13, 14, 39, 43, 45, 46, 16, 3, 5, 10, 12, 19, 21, 26, 28, 35, 37, 42, 44,
    1, 2, 4, 8, 17, 18, 20, 24, 6, 9, 22, 25, 32, 33, 34, 36, 40, 38, 41 };
static const u8 cbp_inter[48] = { 0, 16, 1, 2, 4, 8, 32, 3, 5, 10, 12, 15, 47, 7, 11, 13, 14, 6, 9, 31, 35, 37, 42, 44, 33, 34, 36, 40, 39, 43,
    45, 46, 17, 18, 20, 24, 19, 21, 26, 28, 23, 27, 29, 30, 22, 25, 38, 41 };

static MB *mb_get(H264Dec *d, int x, int y);
static MB *nb_luma(H264Dec *d, int xn, int yn, int *blk);

static u32 peek_bits(Bits *b, int n) { u32 save = b->pos, v = rbn(b, n); b->pos = save; return v; }
/* index of the matching code in a (length, value) table; -1 none */
static int vlc_read(Bits *b, const u8 *len, const u8 *bits, int n) {
    u32 pk = peek_bits(b, 16); int i;
    for (i = 0; i < n; i++) if (len[i] && (pk >> (16 - len[i])) == bits[i]) { b->pos += len[i]; return i; }
    return -1;
}

/* residual_block_cavlc: levels in scan order into out[0..n-1] (nc -1: chroma DC); returns TotalCoeff */
static int cavlc_residual(H264Dec *d, s32 *out, int n, int nc) {
    Bits *b = &d->bits; int tok, total, t1, i, suffix, level[16], run[16], zeros, k;
    memset(out, 0, sizeof(s32) * n);
    if (nc < 0) tok = vlc_read(b, ctc_len, ctc_bits, 20);
    else tok = vlc_read(b, ct_len[nc < 2 ? 0 : nc < 4 ? 1 : nc < 8 ? 2 : 3], ct_bits[nc < 2 ? 0 : nc < 4 ? 1 : nc < 8 ? 2 : 3], 68);
    if (tok < 0) return 0;
    total = tok >> 2; t1 = tok & 3;
    if (!total) return 0;
    if (total > n) total = n;
    suffix = total > 10 && t1 < 3 ? 1 : 0;
    for (i = 0; i < total; i++) {
        if (i < t1) { level[i] = rb1(b) ? -1 : 1; continue; }
        {
            int prefix = 0, size, code;
            while (!rb1(b)) { if (++prefix > 32) return 0; }
            size = (prefix == 14 && suffix == 0) ? 4 : prefix >= 15 ? prefix - 3 : suffix;
            code = ((prefix < 15 ? prefix : 15) << suffix);
            if (size > 0) code += (int)rbn(b, size);
            if (prefix >= 15 && suffix == 0) code += 15;
            if (prefix >= 16) code += (1 << (prefix - 3)) - 4096;
            if (i == t1 && t1 < 3) code += 2;
            level[i] = (code & 1) ? (-code - 1) >> 1 : (code + 2) >> 1;
            if (suffix == 0) suffix = 1;
            if ((level[i] < 0 ? -level[i] : level[i]) > (3 << (suffix - 1)) && suffix < 6) suffix++;
        }
    }
    zeros = 0;
    if (total < n) {
        if (nc < 0) zeros = vlc_read(b, tzc_len[total - 1], tzc_bits[total - 1], 4 - total + 1 > 4 ? 4 : 4 - total + 1);
        else zeros = vlc_read(b, tz_len[total - 1], tz_bits[total - 1], 16 - total + 1 > 16 ? 16 : 16 - total + 1);
        if (zeros < 0) zeros = 0;
    }
    for (i = 0; i < total - 1; i++) {
        int r = 0;
        if (zeros > 0) { int ti = zeros < 7 ? zeros - 1 : 6; r = vlc_read(b, run_len[ti], run_bits[ti], ti < 6 ? ti + 2 : 15); if (r < 0) r = 0; }
        run[i] = r; zeros -= r;
    }
    run[total - 1] = zeros < 0 ? 0 : zeros;
    k = -1;
    for (i = total - 1; i >= 0; i--) { k += run[i] + 1; if (k < n) out[k] = level[i]; }
    return total;
}

/* nC of a luma 4x4 block (raster) or a chroma AC block */
static int nc_luma(H264Dec *d, int blk) {
    int x = (blk & 3) * 4, y = (blk >> 2) * 4, nb, na = -1, nbv = -1; MB *n;
    n = nb_luma(d, x - 1, y, &nb); if (n) na = n->nz[nb];
    n = nb_luma(d, x, y - 1, &nb); if (n) nbv = n->nz[nb];
    if (na >= 0 && nbv >= 0) return (na + nbv + 1) >> 1;
    return na >= 0 ? na : nbv >= 0 ? nbv : 0;
}
static int nc_chroma(H264Dec *d, int c, int blk) {
    int x = blk & 1, y = blk >> 1, na = -1, nbv = -1; MB *n;
    n = x ? &d->mbs[d->mbaddr] : mb_get(d, d->mbx - 1, d->mby); if (n) na = n->nzc[c][y * 2 + (x ^ 1)];
    n = y ? &d->mbs[d->mbaddr] : mb_get(d, d->mbx, d->mby - 1); if (n) nbv = n->nzc[c][(y ^ 1) * 2 + x];
    if (na >= 0 && nbv >= 0) return (na + nbv + 1) >> 1;
    return na >= 0 ? na : nbv >= 0 ? nbv : 0;
}

/* ---------------------------------------------------------------- */
/* macroblock layer (CABAC)                                         */

/* the residual of one macroblock before scaling */
typedef struct {
    int type;                   /* MB_* */
    int i16mode;
    int part_shape;             /* inter: 0 16x16, 1 16x8, 2 8x16, 3 8x8 */
    int part_mode[4];           /* 1 L0, 2 L1, 3 Bi, 0 direct */
    int sub_shape[4];           /* 8x8: 0 8x8, 1 8x4, 2 4x8, 3 4x4 */
    s32 luma[16][16];           /* raster 4x4 blocks, coefficients in raster order */
    s32 luma8[4][64];
    s32 dc[16];                 /* Intra16x16 DC, raster */
    s32 cdc[2][4];
    s32 cac[2][4][16];
    u8 pcm[384];
} MBData;

/* raster 4x4 index of luma4x4BlkIdx */
static const u8 blk2raster[16] = { 0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15 };

static MB *mb_get(H264Dec *d, int x, int y) {
    MB *m;
    if (x < 0 || y < 0 || x >= d->mbw || y >= d->mbh) return NULL;
    m = &d->mbs[y * d->mbw + x];
    return m->slice == d->nslices ? m : NULL;
}
/* the macroblock and raster 4x4 block covering luma sample (xn, yn) relative to the
   current macroblock (-1..16); NULL when not available.  The current macroblock
   itself is returned for samples inside it (the caller checks they are decoded). */
static MB *nb_luma(H264Dec *d, int xn, int yn, int *blk) {
    int mx = d->mbx, my = d->mby;
    if (yn > 15) return NULL;
    if (xn < 0) mx--; else if (xn > 15) { if (yn >= 0) return NULL; mx++; }
    if (yn < 0) my--;
    *blk = (((yn + 16) & 15) >> 2) * 4 + (((xn + 16) & 15) >> 2);
    if (mx == d->mbx && my == d->mby) return &d->mbs[d->mbaddr];
    return mb_get(d, mx, my);
}

static int cabac_mb_skip(H264Dec *d) {
    MB *a = mb_get(d, d->mbx - 1, d->mby), *b = mb_get(d, d->mbx, d->mby - 1);
    int inc = (a && !IS_SKIP(a->type)) + (b && !IS_SKIP(b->type));
    return cabac_bin(d, (d->sh.type == SL_B ? 24 : 11) + inc);
}
/* intra mb_type: 0 I_NxN, 1..24 I_16x16, 25 I_PCM; base 3 (I slice) or 17 / 32 (suffix) */
static int cabac_mb_type_intra(H264Dec *d, int base, int islice) {
    int t, inc = 0;
    if (islice) {
        MB *a = mb_get(d, d->mbx - 1, d->mby), *b = mb_get(d, d->mbx, d->mby - 1);
        inc = (a && a->type != MB_I4 && a->type != MB_I8) + (b && b->type != MB_I4 && b->type != MB_I8);
    }
    if (!cabac_bin(d, base + inc)) return 0;
    if (cabac_term(d)) return 25;
    t = 1 + 12 * cabac_bin(d, base + (islice ? 3 : 1));
    if (cabac_bin(d, base + (islice ? 4 : 2))) t += cabac_bin(d, base + (islice ? 5 : 2)) ? 8 : 4;
    t += 2 * cabac_bin(d, base + (islice ? 6 : 3));
    t += cabac_bin(d, base + (islice ? 7 : 3));
    return t;
}
static int cabac_mb_type_p(H264Dec *d) {
    if (cabac_bin(d, 14)) return 5 + cabac_mb_type_intra(d, 17, 0);
    if (cabac_bin(d, 15)) return cabac_bin(d, 17) ? 1 : 2;
    return cabac_bin(d, 16) ? 3 : 0;
}
static int cabac_mb_type_b(H264Dec *d) {
    MB *a = mb_get(d, d->mbx - 1, d->mby), *b = mb_get(d, d->mbx, d->mby - 1);
    int inc = (a && a->type != MB_BSKIP && a->type != MB_BDIRECT) + (b && b->type != MB_BSKIP && b->type != MB_BDIRECT), bits;
    if (!cabac_bin(d, 27 + inc)) return 0;
    if (!cabac_bin(d, 27 + 3)) return 1 + cabac_bin(d, 27 + 5);
    bits = cabac_bin(d, 27 + 4) << 3;
    bits |= cabac_bin(d, 27 + 5) << 2;
    bits |= cabac_bin(d, 27 + 5) << 1;
    bits |= cabac_bin(d, 27 + 5);
    if (bits < 8) return bits + 3;
    if (bits == 13) return 23 + cabac_mb_type_intra(d, 32, 0);
    if (bits == 14) return 11;
    if (bits == 15) return 22;
    bits = (bits << 1) | cabac_bin(d, 27 + 5);
    return bits - 4;
}
static int cabac_sub_p(H264Dec *d) {
    if (cabac_bin(d, 21)) return 0;
    if (!cabac_bin(d, 22)) return 1;
    return cabac_bin(d, 23) ? 2 : 3;
}
static int cabac_sub_b(H264Dec *d) {
    int t;
    if (!cabac_bin(d, 36)) return 0;
    if (!cabac_bin(d, 37)) return 1 + cabac_bin(d, 39);
    t = 3;
    if (cabac_bin(d, 38)) {
        if (cabac_bin(d, 39)) return 11 + cabac_bin(d, 39);
        t += 4;
    }
    t += 2 * cabac_bin(d, 39);
    t += cabac_bin(d, 39);
    return t;
}
static int cabac_t8x8(H264Dec *d) {
    MB *a = mb_get(d, d->mbx - 1, d->mby), *b = mb_get(d, d->mbx, d->mby - 1);
    return cabac_bin(d, 399 + (a && a->t8x8) + (b && b->t8x8));
}
static int cabac_ipred(H264Dec *d) {
    int v;
    if (cabac_bin(d, 68)) return -1;
    v = cabac_bin(d, 69);
    v |= cabac_bin(d, 69) << 1;
    v |= cabac_bin(d, 69) << 2;
    return v;
}
static int cabac_chroma_mode(H264Dec *d) {
    MB *a = mb_get(d, d->mbx - 1, d->mby), *b = mb_get(d, d->mbx, d->mby - 1);
    int inc = (a && IS_INTRA(a->type) && a->type != MB_PCM && a->chroma_mode) + (b && IS_INTRA(b->type) && b->type != MB_PCM && b->chroma_mode);
    if (!cabac_bin(d, 64 + inc)) return 0;
    if (!cabac_bin(d, 67)) return 1;
    return cabac_bin(d, 67) ? 3 : 2;
}
static int cabac_cbp(H264Dec *d) {
    MB *a = mb_get(d, d->mbx - 1, d->mby), *b = mb_get(d, d->mbx, d->mby - 1);
    int cbp = 0, i, inc, ca, cb;
    for (i = 0; i < 4; i++) {
        int x = i & 1, y = i >> 1, ta, tb;
        if (x == 0) ta = a && a->type != MB_PCM && !((a->cbp >> (i + 1)) & 1);
        else ta = !((cbp >> (i - 1)) & 1);
        if (y == 0) tb = b && b->type != MB_PCM && !((b->cbp >> (i + 2)) & 1);
        else tb = !((cbp >> (i - 2)) & 1);
        if (cabac_bin(d, 73 + ta + 2 * tb)) cbp |= 1 << i;
    }
    ca = a && (a->type == MB_PCM || (a->cbp >> 4)); cb = b && (b->type == MB_PCM || (b->cbp >> 4));
    inc = ca + 2 * cb;
    if (cabac_bin(d, 77 + inc)) {
        ca = a && (a->type == MB_PCM || (a->cbp >> 4) == 2); cb = b && (b->type == MB_PCM || (b->cbp >> 4) == 2);
        cbp |= (1 + cabac_bin(d, 81 + ca + 2 * cb)) << 4;
    }
    return cbp;
}
static int cabac_dqp(H264Dec *d) {
    int k = 0, v;
    if (!cabac_bin(d, 60 + (d->last_dqp != 0))) return 0;
    k = 1;
    if (cabac_bin(d, 62)) { k = 2; while (cabac_bin(d, 63)) { if (++k > 102) break; } }
    v = (k + 1) >> 1;
    return (k & 1) ? v : -v;
}
/* ref_idx: a, b are the neighbouring partitions' context conditions */
static int cabac_ref(H264Dec *d, int ca, int cb) {
    int v = 0;
    if (!cabac_bin(d, 54 + ca + 2 * cb)) return 0;
    v = 1;
    if (!cabac_bin(d, 58)) return 1;
    v = 2;
    while (cabac_bin(d, 59)) { if (++v > 32) break; }
    return v;
}
static int cabac_mvd(H264Dec *d, int comp, int sum) {
    int base = comp ? 47 : 40, k, v;
    if (!cabac_bin(d, base + (sum < 3 ? 0 : sum > 32 ? 2 : 1))) return 0;
    k = 1;
    while (k < 9 && cabac_bin(d, base + (k < 4 ? k + 2 : 6))) k++;
    v = k;
    if (k >= 9) v += cabac_eg(d, 3);
    return cabac_bypass(d) ? -v : v;
}

/* coded_block_flag context conditions; cat: 0 luma DC, 1 luma AC, 2 luma 4x4, 3 chroma DC, 4 chroma AC */
static int cbf_cond_dc(H264Dec *d, MB *n, int bit, int cur_intra) {
    if (!n) return cur_intra;
    if (n->type == MB_PCM) return 1;
    return (n->cbf_dc >> bit) & 1;
}
static int cbf_luma(H264Dec *d, int blk, int cur_intra) {
    int x = (blk & 3) * 4, y = (blk >> 2) * 4, nb, inc = 0, i;
    for (i = 0; i < 2; i++) {
        MB *n = nb_luma(d, i == 0 ? x - 1 : x, i == 0 ? y : y - 1, &nb);
        int c;
        if (!n) c = cur_intra;
        else if (n->type == MB_PCM) c = 1;
        else if (n != &d->mbs[d->mbaddr] && (IS_SKIP(n->type) || !((n->cbp >> ((nb >> 3) * 2 + ((nb & 3) >> 1))) & 1))) c = 0;
        else c = n->nz[nb] != 0;
        inc += c << i;
    }
    return inc;
}
static int cbf_chroma(H264Dec *d, int c, int blk, int cur_intra) {
    int x = blk & 1, y = blk >> 1, inc = 0, i;
    for (i = 0; i < 2; i++) {
        MB *n; int b2, v;
        if (i == 0) { n = x ? &d->mbs[d->mbaddr] : mb_get(d, d->mbx - 1, d->mby); b2 = y * 2 + (x ^ 1); }
        else { n = y ? &d->mbs[d->mbaddr] : mb_get(d, d->mbx, d->mby - 1); b2 = (y ^ 1) * 2 + x; }
        if (!n) v = cur_intra;
        else if (n->type == MB_PCM) v = 1;
        else if (n != &d->mbs[d->mbaddr] && (IS_SKIP(n->type) || (n->cbp >> 4) != 2)) v = 0;
        else v = n->nzc[c][b2] != 0;
        inc += v << i;
    }
    return inc;
}

static const u8 sig8_ctx[64] = {
    0, 1, 2, 3, 4, 5, 5, 4, 4, 3, 3, 4, 4, 4, 5, 5, 4, 4, 4, 4, 3, 3, 6, 7, 7, 7, 8, 9, 10, 9, 8, 7,
    7, 6, 11, 12, 13, 11, 6, 7, 8, 9, 14, 10, 9, 8, 6, 11, 12, 13, 11, 6, 9, 14, 10, 9, 11, 12, 13, 11, 14, 10, 12, 14 };
static const u8 last8_ctx[64] = {
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 8 };
static const u8 cat_cbf[5] = { 0, 4, 8, 12, 16 }, cat_sig[5] = { 0, 15, 29, 44, 47 }, cat_abs[5] = { 0, 10, 20, 30, 39 };

/* residual_block_cabac: levels in scan order into out[0..n-1]; cbf_inc < 0: no coded_block_flag (8x8);
   returns the number of non-zero coefficients */
static int cabac_residual(H264Dec *d, s32 *out, int n, int cat, int cbf_inc) {
    int sig[64], i, nc = 0, last = -1, c1 = 1, c2 = 0, sigb, lastb, absb;
    memset(out, 0, sizeof(s32) * n);
    if (cbf_inc >= 0 && !cabac_bin(d, 85 + cat_cbf[cat] + cbf_inc)) return 0;
    if (cat == 5) { sigb = 402; lastb = 417; absb = 426; }
    else { sigb = 105 + cat_sig[cat]; lastb = 166 + cat_sig[cat]; absb = 227 + cat_abs[cat]; }
    for (i = 0; i < n - 1; i++) {
        int si = cat == 5 ? sig8_ctx[i] : cat == 3 ? (i < 2 ? i : 2) : i;
        sig[i] = cabac_bin(d, sigb + si);
        if (sig[i]) {
            nc++;
            if (cabac_bin(d, lastb + (cat == 5 ? last8_ctx[i] : cat == 3 ? (i < 2 ? i : 2) : i))) { last = i; break; }
        }
    }
    if (last < 0) { last = n - 1; sig[last] = 1; nc++; }
    for (i = last; i >= 0; i--) {
        int lv;
        if (!sig[i]) continue;
        if (!cabac_bin(d, absb + (c2 ? 0 : (c1 < 4 ? c1 : 4)))) lv = 1;
        else {
            int k = 1, cap = cat == 3 ? 3 : 4;
            while (k < 14 && cabac_bin(d, absb + 5 + (c2 < cap ? c2 : cap))) k++;
            lv = k + 1;
            if (k >= 14) lv += cabac_eg(d, 0);
            c2++;
        }
        if (lv == 1 && !c2) c1++;
        out[i] = cabac_bypass(d) ? -lv : lv;
    }
    return nc;
}

/* ---------------------------------------------------------------- */
/* motion vector prediction (8.4.1)                                 */

typedef struct { int avail, ref; int mv[2]; } NbMotion;

/* motion data of the partition covering luma (xn, yn); `done`: decoded 4x4 blocks of the current MB */
static void nb_motion(H264Dec *d, int xn, int yn, int list, u32 done, NbMotion *o) {
    int blk; MB *n = nb_luma(d, xn, yn, &blk);
    o->avail = 0; o->ref = -1; o->mv[0] = o->mv[1] = 0;
    if (!n) return;
    if (n == &d->mbs[d->mbaddr] && !((done >> blk) & 1)) return;
    o->avail = 1;
    if (IS_INTRA(n->type)) return;
    o->ref = n->ref[list][(blk >> 3) * 2 + ((blk & 3) >> 1)];
    if (o->ref < 0) return;
    o->mv[0] = n->mv[list][blk][0]; o->mv[1] = n->mv[list][blk][1];
}
static int median3(int a, int b, int c) {
    int mx = a > b ? a : b, mn = a < b ? a : b;
    return c > mx ? mx : c < mn ? mn : c;
}
static void mv_pred(H264Dec *d, int x, int y, int w, int h, int list, int ref, u32 done, int *mv) {
    NbMotion a, b, c;
    nb_motion(d, x - 1, y, list, done, &a);
    nb_motion(d, x, y - 1, list, done, &b);
    nb_motion(d, x + w, y - 1, list, done, &c);
    if (!c.avail) nb_motion(d, x - 1, y - 1, list, done, &c);
    if (w == 16 && h == 8) {
        if (y == 0 && b.ref == ref) { mv[0] = b.mv[0]; mv[1] = b.mv[1]; return; }
        if (y == 8 && a.ref == ref) { mv[0] = a.mv[0]; mv[1] = a.mv[1]; return; }
    } else if (w == 8 && h == 16) {
        if (x == 0 && a.ref == ref) { mv[0] = a.mv[0]; mv[1] = a.mv[1]; return; }
        if (x == 8 && c.ref == ref) { mv[0] = c.mv[0]; mv[1] = c.mv[1]; return; }
    }
    if (!b.avail && !c.avail && a.avail) { b = a; c = a; }
    if ((a.ref == ref) + (b.ref == ref) + (c.ref == ref) == 1) {
        const NbMotion *m = a.ref == ref ? &a : b.ref == ref ? &b : &c;
        mv[0] = m->mv[0]; mv[1] = m->mv[1];
        return;
    }
    mv[0] = median3(a.mv[0], b.mv[0], c.mv[0]);
    mv[1] = median3(a.mv[1], b.mv[1], c.mv[1]);
}
static void pskip_mv(H264Dec *d, int *mv) {
    NbMotion a, b;
    nb_motion(d, -1, 0, 0, 0, &a);
    nb_motion(d, 0, -1, 0, 0, &b);
    mv[0] = mv[1] = 0;
    if (!a.avail || !b.avail) return;
    if (a.ref == 0 && a.mv[0] == 0 && a.mv[1] == 0) return;
    if (b.ref == 0 && b.mv[0] == 0 && b.mv[1] == 0) return;
    mv_pred(d, 0, 0, 16, 16, 0, 0, 0, mv);
}

/* the colocated block of 4x4 `blk` (raster): its motion in the list 1 picture */
static void colocated(H264Dec *d, int blk, int *ref, int *mv, int *list_used, int *poc, int *lt) {
    const H264Pic *col = d->list[1][0];
    int mb = d->mbaddr, b = blk, l;
    *ref = -1; mv[0] = mv[1] = 0; *list_used = 0; *poc = 0; *lt = 0;
    if (!col || !col->mv) return;
    if (d->s->direct_8x8_inference) { int b8x = (blk & 3) >> 1, b8y = blk >> 3; b = b8y * 12 + b8x * 3; }
    if (col->intra[mb]) return;
    l = col->ref_idx[mb * 16 + b][0] >= 0 ? 0 : 1;
    *ref = col->ref_idx[mb * 16 + b][l];
    if (*ref < 0) return;
    *list_used = l;
    mv[0] = col->mv[mb * 16 + b][l][0]; mv[1] = col->mv[mb * 16 + b][l][1];
    *poc = col->ref_poc[mb][l][(b >> 3) * 2 + ((b & 3) >> 1)];
    *lt = col->ref_lt[mb][l][(b >> 3) * 2 + ((b & 3) >> 1)];
}

static int min_positive(int a, int b) { return (a >= 0 && b >= 0) ? (a < b ? a : b) : (a > b ? a : b); }

/* direct prediction of the 4x4 blocks in `mask` (raster bits) of the current MB */
static void direct_pred(H264Dec *d, MB *m, u32 mask) {
    int blk;
    if (d->sh.direct_spatial) {
        int ref[2], mvp[2][2], l, zero = 0;
        for (l = 0; l < 2; l++) {
            NbMotion a, b, c;
            nb_motion(d, -1, 0, l, 0, &a);
            nb_motion(d, 0, -1, l, 0, &b);
            nb_motion(d, 16, -1, l, 0, &c);
            if (!c.avail) nb_motion(d, -1, -1, l, 0, &c);
            ref[l] = min_positive(a.ref, min_positive(b.ref, c.ref));
            mvp[l][0] = mvp[l][1] = 0;
        }
        if (ref[0] < 0 && ref[1] < 0) { ref[0] = ref[1] = 0; zero = 1; }
        else for (l = 0; l < 2; l++) if (ref[l] >= 0) mv_pred(d, 0, 0, 16, 16, l, ref[l], 0, mvp[l]);
        for (blk = 0; blk < 16; blk++) {
            int cref, cmv[2], cl, cpoc, clt, colzero, b8 = (blk >> 3) * 2 + ((blk & 3) >> 1);
            if (!((mask >> blk) & 1)) continue;
            colocated(d, blk, &cref, cmv, &cl, &cpoc, &clt);
            colzero = d->list[1][0] && d->list[1][0]->ref == 1 && cref == 0 && cmv[0] >= -1 && cmv[0] <= 1 && cmv[1] >= -1 && cmv[1] <= 1;
            (void)cl; (void)cpoc; (void)clt;
            for (l = 0; l < 2; l++) {
                m->ref[l][b8] = (s8)ref[l];
                if (zero || ref[l] < 0 || (ref[l] == 0 && colzero)) { m->mv[l][blk][0] = m->mv[l][blk][1] = 0; }
                else { m->mv[l][blk][0] = (s16)mvp[l][0]; m->mv[l][blk][1] = (s16)mvp[l][1]; }
            }
        }
    } else {
        for (blk = 0; blk < 16; blk++) {
            int cref, cmv[2], cl, cpoc, clt, r0 = 0, b8 = (blk >> 3) * 2 + ((blk & 3) >> 1), i;
            if (!((mask >> blk) & 1)) continue;
            colocated(d, blk, &cref, cmv, &cl, &cpoc, &clt);
            if (cref >= 0) {
                /* the lowest list 0 index of the picture the colocated block referenced */
                for (i = 0; i < d->sh.num_ref[0]; i++) {
                    const H264Pic *p = d->list[0][i];
                    if (p && p->poc == cpoc && (p->ref == 2) == (clt != 0)) { r0 = i; break; }
                }
            }
            m->ref[0][b8] = (s8)r0; m->ref[1][b8] = 0;
            {
                const H264Pic *p0 = d->list[0][r0], *p1 = d->list[1][0];
                int dsf = d->dist_scale[r0];
                if (!p0 || !p1 || p0->ref == 2 || dsf == 9999) {
                    m->mv[0][blk][0] = (s16)cmv[0]; m->mv[0][blk][1] = (s16)cmv[1];
                    m->mv[1][blk][0] = m->mv[1][blk][1] = 0;
                } else {
                    int k;
                    for (k = 0; k < 2; k++) {
                        int v0 = (dsf * cmv[k] + 128) >> 8;
                        m->mv[0][blk][k] = (s16)v0; m->mv[1][blk][k] = (s16)(v0 - cmv[k]);
                    }
                }
            }
        }
    }
}

/* ---------------------------------------------------------------- */
/* macroblock parsing (CABAC)                                       */

static const u8 b_part[23][3] = {
    { 0, 0, 0 }, { 0, 1, 0 }, { 0, 2, 0 }, { 0, 3, 0 }, { 1, 1, 1 }, { 2, 1, 1 }, { 1, 2, 2 }, { 2, 2, 2 },
    { 1, 1, 2 }, { 2, 1, 2 }, { 1, 2, 1 }, { 2, 2, 1 }, { 1, 1, 3 }, { 2, 1, 3 }, { 1, 2, 3 }, { 2, 2, 3 },
    { 1, 3, 1 }, { 2, 3, 1 }, { 1, 3, 2 }, { 2, 3, 2 }, { 1, 3, 3 }, { 2, 3, 3 }, { 3, 0, 0 } };
static const u8 b_sub[13][2] = {
    { 0, 0 }, { 0, 1 }, { 0, 2 }, { 0, 3 }, { 1, 1 }, { 2, 1 }, { 1, 2 }, { 2, 2 }, { 1, 3 }, { 2, 3 }, { 3, 1 }, { 3, 2 }, { 3, 3 } };

/* the 4x4 blocks (raster bit mask) of 8x8 block b8 */
static u32 b8_mask(int b8) { return 0x33u << ((b8 >> 1) * 8 + (b8 & 1) * 2); }

/* mvd context: sum of |mvd| of the partitions left of and above (x, y) */
static int mvd_sum(H264Dec *d, int x, int y, int list, int comp) {
    int s = 0, i;
    for (i = 0; i < 2; i++) {
        int blk; MB *n = nb_luma(d, i ? x : x - 1, i ? y - 1 : y, &blk);
        if (!n || IS_INTRA(n->type) || IS_SKIP(n->type)) continue;
        s += n->mvd[list][blk][comp];
    }
    return s;
}
/* ref_idx context conditions of the partition at (x, y) */
static int ref_ctx(H264Dec *d, int x, int y, int list) {
    int inc = 0, i;
    for (i = 0; i < 2; i++) {
        int blk, b8, c; MB *n = nb_luma(d, i ? x : x - 1, i ? y - 1 : y, &blk);
        if (!n || IS_INTRA(n->type) || IS_SKIP(n->type) || n->type == MB_BDIRECT) continue;
        b8 = (blk >> 3) * 2 + ((blk & 3) >> 1);
        if (n->direct8[b8]) continue;
        c = n->ref[list][b8] > 0;
        inc += c << i;
    }
    return inc;
}
static void set_mvd(MB *m, int list, u32 mask, int mx, int my) {
    int blk;
    mx = mx < 0 ? -mx : mx; my = my < 0 ? -my : my;
    mx = mx > 64 ? 64 : mx; my = my > 64 ? 64 : my;
    for (blk = 0; blk < 16; blk++) if ((mask >> blk) & 1) { m->mvd[list][blk][0] = (u8)mx; m->mvd[list][blk][1] = (u8)my; }
}
static void set_mv(MB *m, int list, u32 mask, const int *mv) {
    int blk;
    for (blk = 0; blk < 16; blk++) if ((mask >> blk) & 1) { m->mv[list][blk][0] = (s16)mv[0]; m->mv[list][blk][1] = (s16)mv[1]; }
}
/* the raster 4x4 mask of a w x h partition at (x, y) */
static u32 part_mask(int x, int y, int w, int h) {
    u32 m = 0; int i, j;
    for (j = y >> 2; j < (y + h) >> 2; j++) for (i = x >> 2; i < (x + w) >> 2; i++) m |= 1u << (j * 4 + i);
    return m;
}

static void intra_pred_modes(H264Dec *d, MB *m, int is8) {
    int i;
    for (i = 0; i < (is8 ? 4 : 16); i++) {
        int x, y, blk, pa, pb, pred, v; MB *na, *nb;
        if (is8) { x = (i & 1) * 8; y = (i >> 1) * 8; }
        else { int r = blk2raster[i]; x = (r & 3) * 4; y = (r >> 2) * 4; }
        na = nb_luma(d, x - 1, y, &blk);
        pa = -1;
        if (na && !(IS_INTRA(na->type) == 0 && d->p->constrained_intra)) pa = (na->type == MB_I4 || na->type == MB_I8) ? na->ipred[blk] : 2;
        nb = nb_luma(d, x, y - 1, &blk);
        pb = -1;
        if (nb && !(IS_INTRA(nb->type) == 0 && d->p->constrained_intra)) pb = (nb->type == MB_I4 || nb->type == MB_I8) ? nb->ipred[blk] : 2;
        pred = (pa < 0 || pb < 0) ? 2 : (pa < pb ? pa : pb);
        if (d->p->cabac) v = cabac_ipred(d);
        else v = rb1(&d->bits) ? -1 : (int)rbn(&d->bits, 3);
        if (v >= 0) pred = v < pred ? v : v + 1;
        if (is8) { int k; for (k = 0; k < 4; k++) m->ipred[(y / 4 + (k >> 1)) * 4 + x / 4 + (k & 1)] = (s8)pred; }
        else m->ipred[(y / 4) * 4 + x / 4] = (s8)pred;
    }
}

/* luma residual of the macroblock (cbp bits, Intra16x16 AC or 4x4 / 8x8 blocks) */
static void parse_luma_residual(H264Dec *d, MB *m, MBData *md) {
    int intra = IS_INTRA(md->type), i8, i;
    if (md->type == MB_I16) {
        MB *a = mb_get(d, d->mbx - 1, d->mby), *b = mb_get(d, d->mbx, d->mby - 1);
        s32 lv[16];
        int inc = cbf_cond_dc(d, a, 0, 1) + 2 * cbf_cond_dc(d, b, 0, 1);
        if (d->p->cabac ? cabac_residual(d, lv, 16, 0, inc) : cavlc_residual(d, lv, 16, nc_luma(d, 0))) m->cbf_dc |= 1;
        for (i = 0; i < 16; i++) md->dc[zz4[i]] = lv[i];
    }
    for (i8 = 0; i8 < 4; i8++) {
        if (!((m->cbp >> i8) & 1)) continue;
        if (m->t8x8 && !d->p->cabac) {   /* four interleaved 4x4 blocks */
            for (i = 0; i < 4; i++) {
                int r = blk2raster[i8 * 4 + i], k; s32 lv[16];
                m->nz[r] = (u8)cavlc_residual(d, lv, 16, nc_luma(d, r));
                for (k = 0; k < 16; k++) md->luma8[i8][zz8[4 * k + i]] = lv[k];
            }
        } else if (m->t8x8) {
            s32 lv[64]; int nc = cabac_residual(d, lv, 64, 5, -1), k, x = (i8 & 1) * 2, y = (i8 >> 1) * 2;
            for (k = 0; k < 64; k++) md->luma8[i8][zz8[k]] = lv[k];
            m->nz[y * 4 + x] = m->nz[y * 4 + x + 1] = m->nz[(y + 1) * 4 + x] = m->nz[(y + 1) * 4 + x + 1] = (u8)nc;
        } else {
            for (i = 0; i < 4; i++) {
                int r = blk2raster[i8 * 4 + i], k, nc; s32 lv[16];
                if (md->type == MB_I16) {
                    nc = d->p->cabac ? cabac_residual(d, lv, 15, 1, cbf_luma(d, r, intra)) : cavlc_residual(d, lv, 15, nc_luma(d, r));
                    for (k = 0; k < 15; k++) md->luma[r][zz4[k + 1]] = lv[k];
                } else {
                    nc = d->p->cabac ? cabac_residual(d, lv, 16, 2, cbf_luma(d, r, intra)) : cavlc_residual(d, lv, 16, nc_luma(d, r));
                    for (k = 0; k < 16; k++) md->luma[r][zz4[k]] = lv[k];
                }
                m->nz[r] = (u8)nc;
            }
        }
    }
}
static void parse_chroma_residual(H264Dec *d, MB *m, MBData *md) {
    int c, i, intra = IS_INTRA(md->type);
    if (!(m->cbp >> 4)) return;
    for (c = 0; c < 2; c++) {
        MB *a = mb_get(d, d->mbx - 1, d->mby), *b = mb_get(d, d->mbx, d->mby - 1);
        int inc = cbf_cond_dc(d, a, 1 + c, intra) + 2 * cbf_cond_dc(d, b, 1 + c, intra);
        if (d->p->cabac ? cabac_residual(d, md->cdc[c], 4, 3, inc) : cavlc_residual(d, md->cdc[c], 4, -1)) m->cbf_dc |= 2 << c;
    }
    if ((m->cbp >> 4) != 2) return;
    for (c = 0; c < 2; c++) for (i = 0; i < 4; i++) {
        s32 lv[15]; int k, nc = d->p->cabac ? cabac_residual(d, lv, 15, 4, cbf_chroma(d, c, i, intra)) : cavlc_residual(d, lv, 15, nc_chroma(d, c, i));
        for (k = 0; k < 15; k++) md->cac[c][i][zz4[k + 1]] = lv[k];
        m->nzc[c][i] = (u8)nc;
    }
}

/* the reference index of one partition (CABAC), 0 when the list has one picture */
static int read_ref(H264Dec *d, int x, int y, int list) {
    int r;
    if (d->sh.num_ref[list] <= 1) return 0;
    if (!d->p->cabac) { r = d->sh.num_ref[list] == 2 ? !rb1(&d->bits) : (int)rue(&d->bits); return r < d->sh.num_ref[list] ? r : 0; }
    r = cabac_ref(d, ref_ctx(d, x, y, list) & 1, ref_ctx(d, x, y, list) >> 1);
    return r < d->sh.num_ref[list] ? r : 0;
}
/* mvd of a partition, the motion vector from its prediction */
static void read_mv(H264Dec *d, MB *m, int x, int y, int w, int h, int list, u32 *done) {
    int mv[2], mx, my, b8 = (y >> 3) * 2 + (x >> 3); u32 mask = part_mask(x, y, w, h);
    if (d->p->cabac) { mx = cabac_mvd(d, 0, mvd_sum(d, x, y, list, 0)); my = cabac_mvd(d, 1, mvd_sum(d, x, y, list, 1)); }
    else { mx = rse(&d->bits); my = rse(&d->bits); }
    mv_pred(d, x, y, w, h, list, m->ref[list][b8], *done, mv);
    mv[0] += mx; mv[1] += my;
    set_mv(m, list, mask, mv);
    set_mvd(m, list, mask, mx, my);
    *done |= mask;
}

static int decode_mb(H264Dec *d, MB *m, MBData *md, int *pcm) {
    int t = 0, i, l, cabac = d->p->cabac, ref0 = 0;
    *pcm = 0;
    memset(md, 0, sizeof(*md));
    /* skip (CAVLC: counted by the slice loop) */
    if (cabac && d->sh.type != SL_I && cabac_mb_skip(d)) {
        md->type = d->sh.type == SL_P ? MB_PSKIP : MB_BSKIP;
        m->type = (u8)md->type;
        return 1;
    }
    if (!cabac) {
        t = (int)rue(&d->bits);
        if (d->sh.type == SL_P && t == 4) { ref0 = 1; t = 3; }
        if (d->sh.type == SL_I) { if (t > 25) t = 25; }
    }
    if (d->sh.type == SL_I) { if (cabac) t = cabac_mb_type_intra(d, 3, 1); }
    else if (d->sh.type == SL_P) { if (cabac) t = cabac_mb_type_p(d); if (t < 5) { md->type = MB_P; md->part_shape = t == 0 ? 0 : t == 1 ? 1 : t == 2 ? 2 : 3; md->part_mode[0] = md->part_mode[1] = 1; } else t -= 5; }
    else { if (cabac) t = cabac_mb_type_b(d); if (t < 23) { md->type = t == 0 ? MB_BDIRECT : MB_B; md->part_shape = b_part[t][0]; md->part_mode[0] = b_part[t][1]; md->part_mode[1] = b_part[t][2]; } else t -= 23; }
    if ((d->sh.type == SL_I) || (d->sh.type == SL_P && md->part_mode[0] == 0) || (d->sh.type == SL_B && md->type != MB_B && md->type != MB_BDIRECT)) {
        /* intra macroblock, t: 0 I_NxN, 1..24 I_16x16, 25 I_PCM */
        if (t == 25) {
            md->type = MB_PCM; m->type = MB_PCM;
            *pcm = 1;
            return 1;
        }
        if (t == 0) {
            md->type = MB_I4;
            if (d->p->transform_8x8 && (cabac ? cabac_t8x8(d) : (int)rb1(&d->bits))) { md->type = MB_I8; m->t8x8 = 1; }
            m->type = (u8)md->type;
            intra_pred_modes(d, m, md->type == MB_I8);
        } else {
            md->type = MB_I16; m->type = MB_I16;
            md->i16mode = (t - 1) & 3;
            m->cbp = (u8)(((((t - 1) >> 2) % 3) << 4) | ((t >= 13) ? 15 : 0));
        }
        m->chroma_mode = (u8)(cabac ? cabac_chroma_mode(d) : (int)(rue(&d->bits) & 3));
        for (l = 0; l < 2; l++) for (i = 0; i < 4; i++) m->ref[l][i] = -1;
    } else {
        u32 done = 0;
        m->type = (u8)md->type;
        if (md->type == MB_BDIRECT) {
            for (i = 0; i < 4; i++) m->direct8[i] = 1;
            direct_pred(d, m, 0xffff);
        } else if (md->part_shape == 3) {
            int sub[4], dmask = 0;
            for (i = 0; i < 4; i++) {
                if (d->sh.type == SL_P) { sub[i] = cabac ? cabac_sub_p(d) : (int)(rue(&d->bits) & 3); md->sub_shape[i] = sub[i]; md->part_mode[i] = 1; }
                else { sub[i] = cabac ? cabac_sub_b(d) : (int)rue(&d->bits); if (sub[i] > 12) sub[i] = 12; md->sub_shape[i] = b_sub[sub[i]][0]; md->part_mode[i] = b_sub[sub[i]][1]; }
                if (d->sh.type == SL_B && sub[i] == 0) { m->direct8[i] = 1; dmask |= (int)b8_mask(i); }
            }
            for (l = 0; l < 2; l++) for (i = 0; i < 4; i++) m->ref[l][i] = -1;
            if (dmask) direct_pred(d, m, (u32)dmask);
            for (l = 0; l < 2; l++) for (i = 0; i < 4; i++)
                if (!m->direct8[i] && (md->part_mode[i] & (1 << l))) m->ref[l][i] = (s8)(ref0 ? 0 : read_ref(d, (i & 1) * 8, (i >> 1) * 8, l));
            for (l = 0; l < 2; l++) {
                done = (u32)dmask;
                for (i = 0; i < 4; i++) {
                    int x = (i & 1) * 8, y = (i >> 1) * 8, s = md->sub_shape[i];
                    if (m->direct8[i]) continue;
                    if (!(md->part_mode[i] & (1 << l))) { done |= b8_mask(i); continue; }
                    if (s == 0) read_mv(d, m, x, y, 8, 8, l, &done);
                    else if (s == 1) { read_mv(d, m, x, y, 8, 4, l, &done); read_mv(d, m, x, y + 4, 8, 4, l, &done); }
                    else if (s == 2) { read_mv(d, m, x, y, 4, 8, l, &done); read_mv(d, m, x + 4, y, 4, 8, l, &done); }
                    else { read_mv(d, m, x, y, 4, 4, l, &done); read_mv(d, m, x + 4, y, 4, 4, l, &done); read_mv(d, m, x, y + 4, 4, 4, l, &done); read_mv(d, m, x + 4, y + 4, 4, 4, l, &done); }
                }
            }
        } else {
            int np = md->part_shape == 0 ? 1 : 2, pw = md->part_shape == 2 ? 8 : 16, ph = md->part_shape == 1 ? 8 : 16;
            for (l = 0; l < 2; l++) for (i = 0; i < 4; i++) m->ref[l][i] = -1;
            for (l = 0; l < 2; l++) for (i = 0; i < np; i++) {
                int x = md->part_shape == 2 ? i * 8 : 0, y = md->part_shape == 1 ? i * 8 : 0, r, k;
                if (!(md->part_mode[i] & (1 << l))) continue;
                r = read_ref(d, x, y, l);
                for (k = 0; k < 4; k++) if (part_mask(x, y, pw, ph) & b8_mask(k)) m->ref[l][k] = (s8)r;
            }
            for (l = 0; l < 2; l++) {
                done = 0;
                for (i = 0; i < np; i++) {
                    int x = md->part_shape == 2 ? i * 8 : 0, y = md->part_shape == 1 ? i * 8 : 0;
                    if (!(md->part_mode[i] & (1 << l))) { done |= part_mask(x, y, pw, ph); continue; }
                    read_mv(d, m, x, y, pw, ph, l, &done);
                }
            }
        }
    }
    /* coded_block_pattern, transform size, qp, residual */
    if (md->type != MB_I16) {
        if (cabac) m->cbp = (u8)cabac_cbp(d);
        else { u32 k = rue(&d->bits); m->cbp = k < 48 ? (IS_INTRA(md->type) ? cbp_intra[k] : cbp_inter[k]) : 0; m->cbp = (u8)((m->cbp & 15) | ((m->cbp >> 4) << 4)); }
        if ((m->cbp & 15) && d->p->transform_8x8 && !IS_INTRA(md->type)) {
            int ok = 1;
            if (md->type == MB_BDIRECT) ok = d->s->direct_8x8_inference;
            else if (md->part_shape == 3) {
                for (i = 0; i < 4; i++) {
                    if (m->direct8[i]) { if (!d->s->direct_8x8_inference) ok = 0; }
                    else if (md->sub_shape[i] != 0) ok = 0;
                }
            }
            if (ok) m->t8x8 = (u8)(cabac ? cabac_t8x8(d) : (int)rb1(&d->bits));
        }
    }
    if (m->cbp || md->type == MB_I16) {
        int dq = cabac ? cabac_dqp(d) : rse(&d->bits);
        d->last_dqp = dq;
        d->qp = (d->qp + dq + 52) % 52;
        parse_luma_residual(d, m, md);
        parse_chroma_residual(d, m, md);
    } else d->last_dqp = 0;
    return 1;
}

/* ---------------------------------------------------------------- */
/* scaling and inverse transforms (8.5)                             */

static const u8 norm4[6][3] = { { 10, 16, 13 }, { 11, 18, 14 }, { 13, 20, 16 }, { 14, 23, 18 }, { 16, 25, 20 }, { 18, 29, 23 } };
static const u8 norm8[6][6] = { { 20, 18, 32, 19, 25, 24 }, { 22, 19, 35, 21, 28, 26 }, { 26, 23, 42, 24, 33, 31 },
                                { 28, 25, 45, 26, 35, 33 }, { 32, 28, 51, 30, 40, 38 }, { 36, 32, 58, 34, 46, 43 } };

static int ls4(const PPS *p, int list, int qp6, int pos) {
    int i = pos >> 2, j = pos & 3, k, n, ws = 16;
    for (k = 0; k < 16; k++) if (zz4[k] == pos) { ws = p->sl4[list][k]; break; }
    n = ((i & 1) == 0 && (j & 1) == 0) ? 0 : ((i & 1) == 1 && (j & 1) == 1) ? 1 : 2;
    return ws * norm4[qp6][n];
}
static int ls8(const PPS *p, int list, int qp6, int pos) {
    int i = pos >> 3, j = pos & 7, k, n, ws = 16;
    for (k = 0; k < 64; k++) if (zz8[k] == pos) { ws = p->sl8[list][k]; break; }
    if ((i & 3) == 0 && (j & 3) == 0) n = 0;
    else if ((i & 1) == 1 && (j & 1) == 1) n = 1;
    else if ((i & 3) == 2 && (j & 3) == 2) n = 2;
    else if (((i & 3) == 0 && (j & 1) == 1) || ((i & 1) == 1 && (j & 3) == 0)) n = 3;
    else if (((i & 3) == 0 && (j & 3) == 2) || ((i & 3) == 2 && (j & 3) == 0)) n = 4;
    else n = 5;
    return ws * norm8[qp6][n];
}

/* LevelScale tables of the active PPS: [list][qp % 6][raster position] */
typedef struct { int l4[6][6][16]; int l8[2][6][64]; const PPS *pps; } LevelScale;
static LevelScale g_ls;
static void level_scale_init(const PPS *p) {
    int l, q, i;
    if (g_ls.pps == p) return;
    for (l = 0; l < 6; l++) for (q = 0; q < 6; q++) for (i = 0; i < 16; i++) g_ls.l4[l][q][i] = ls4(p, l, q, i);
    for (l = 0; l < 2; l++) for (q = 0; q < 6; q++) for (i = 0; i < 64; i++) g_ls.l8[l][q][i] = ls8(p, l, q, i);
    g_ls.pps = p;
}

static void dequant4(s32 *c, int list, int qp, int skip_dc) {
    int i, q6 = qp / 6, qm = qp % 6;
    for (i = skip_dc; i < 16; i++) {
        if (!c[i]) continue;
        if (qp >= 24) c[i] = (c[i] * g_ls.l4[list][qm][i]) << (q6 - 4);
        else c[i] = (c[i] * g_ls.l4[list][qm][i] + (1 << (3 - q6))) >> (4 - q6);
    }
}
static void dequant8(s32 *c, int list, int qp) {
    int i, q6 = qp / 6, qm = qp % 6;
    for (i = 0; i < 64; i++) {
        if (!c[i]) continue;
        if (qp >= 36) c[i] = (c[i] * g_ls.l8[list][qm][i]) << (q6 - 6);
        else c[i] = (c[i] * g_ls.l8[list][qm][i] + (1 << (5 - q6))) >> (6 - q6);
    }
}
static u8 clip1(int v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* inverse 4x4 transform of c (raster), added to the samples at dst */
static void idct4_add(s32 *c, u8 *dst, int stride) {
    int i; s32 t[16];
    for (i = 0; i < 4; i++) {
        s32 *r = c + i * 4, e = r[0] + r[2], f = r[0] - r[2], g = (r[1] >> 1) - r[3], h = r[1] + (r[3] >> 1);
        t[i * 4 + 0] = e + h; t[i * 4 + 1] = f + g; t[i * 4 + 2] = f - g; t[i * 4 + 3] = e - h;
    }
    for (i = 0; i < 4; i++) {
        s32 e = t[i] + t[8 + i], f = t[i] - t[8 + i], g = (t[4 + i] >> 1) - t[12 + i], h = t[4 + i] + (t[12 + i] >> 1);
        dst[0 * stride + i] = clip1(dst[0 * stride + i] + ((e + h + 32) >> 6));
        dst[1 * stride + i] = clip1(dst[1 * stride + i] + ((f + g + 32) >> 6));
        dst[2 * stride + i] = clip1(dst[2 * stride + i] + ((f - g + 32) >> 6));
        dst[3 * stride + i] = clip1(dst[3 * stride + i] + ((e - h + 32) >> 6));
    }
}
static void idct8_1d(s32 *d0, int step) {
    s32 d[8], a0, a1, a2, a3, a4, a5, a6, a7, b0, b1, b2, b3, b4, b5, b6, b7; int i;
    for (i = 0; i < 8; i++) d[i] = d0[i * step];
    a0 = d[0] + d[4]; a4 = d[0] - d[4]; a2 = (d[2] >> 1) - d[6]; a6 = d[2] + (d[6] >> 1);
    b0 = a0 + a6; b2 = a4 + a2; b4 = a4 - a2; b6 = a0 - a6;
    a1 = -d[3] + d[5] - d[7] - (d[7] >> 1);
    a3 = d[1] + d[7] - d[3] - (d[3] >> 1);
    a5 = -d[1] + d[7] + d[5] + (d[5] >> 1);
    a7 = d[3] + d[5] + d[1] + (d[1] >> 1);
    b1 = a1 + (a7 >> 2); b7 = a7 - (a1 >> 2); b3 = a3 + (a5 >> 2); b5 = (a3 >> 2) - a5;
    d0[0] = b0 + b7; d0[step] = b2 + b5; d0[2 * step] = b4 + b3; d0[3 * step] = b6 + b1;
    d0[4 * step] = b6 - b1; d0[5 * step] = b4 - b3; d0[6 * step] = b2 - b5; d0[7 * step] = b0 - b7;
}
static void idct8_add(s32 *c, u8 *dst, int stride) {
    int i, j;
    for (i = 0; i < 8; i++) idct8_1d(c + i * 8, 1);
    for (i = 0; i < 8; i++) idct8_1d(c + i, 8);
    for (j = 0; j < 8; j++) for (i = 0; i < 8; i++) dst[j * stride + i] = clip1(dst[j * stride + i] + ((c[j * 8 + i] + 32) >> 6));
}

/* ---------------------------------------------------------------- */
/* intra prediction (8.3)                                           */

/* neighbour sample availability for intra prediction of the block at (x, y) in the current MB */
static int intra_avail(H264Dec *d, int xn, int yn, u32 done) {
    int blk; MB *n = nb_luma(d, xn, yn, &blk);
    if (!n) return 0;
    if (n == &d->mbs[d->mbaddr]) return (done >> blk) & 1;
    if (d->p->constrained_intra && !IS_INTRA(n->type)) return 0;
    return 1;
}
static int intra_avail_mb(H264Dec *d, int dx, int dy) {
    MB *n = mb_get(d, d->mbx + dx, d->mby + dy);
    if (!n) return 0;
    if (d->p->constrained_intra && !IS_INTRA(n->type)) return 0;
    return 1;
}

/* the directional modes of 4x4 and 8x8 prediction on the edge samples:
   top[-1..2N-1] (top[-1] = top-left), left[0..N-1] */
static void pred_dir(u8 *dst, int stride, int n, int mode, const int *top, const int *left) {
    int x, y;
#define T(i) top[(i)]
#define L(i) ((i) < 0 ? top[-1] : left[(i)])
    for (y = 0; y < n; y++) for (x = 0; x < n; x++) {
        int v = 0;
        switch (mode) {
        case 3:
            if (x == n - 1 && y == n - 1) v = (T(2 * n - 2) + 3 * T(2 * n - 1) + 2) >> 2;
            else v = (T(x + y) + 2 * T(x + y + 1) + T(x + y + 2) + 2) >> 2;
            break;
        case 4:
            if (x > y) v = (T(x - y - 2) + 2 * T(x - y - 1) + T(x - y) + 2) >> 2;
            else if (x < y) v = (L(y - x - 2) + 2 * L(y - x - 1) + L(y - x) + 2) >> 2;
            else v = (T(0) + 2 * T(-1) + L(0) + 2) >> 2;
            break;
        case 5: {
            int z = 2 * x - y;
            if (z >= 0 && !(z & 1)) v = (T(x - (y >> 1) - 1) + T(x - (y >> 1)) + 1) >> 1;
            else if (z > 0) v = (T(x - (y >> 1) - 2) + 2 * T(x - (y >> 1) - 1) + T(x - (y >> 1)) + 2) >> 2;
            else if (z == -1) v = (L(0) + 2 * T(-1) + T(0) + 2) >> 2;
            else v = (L(y - 2 * x - 1) + 2 * L(y - 2 * x - 2) + L(y - 2 * x - 3) + 2) >> 2;
            break; }
        case 6: {
            int z = 2 * y - x;
            if (z >= 0 && !(z & 1)) v = (L(y - (x >> 1) - 1) + L(y - (x >> 1)) + 1) >> 1;
            else if (z > 0) v = (L(y - (x >> 1) - 2) + 2 * L(y - (x >> 1) - 1) + L(y - (x >> 1)) + 2) >> 2;
            else if (z == -1) v = (L(0) + 2 * T(-1) + T(0) + 2) >> 2;
            else v = (T(x - 2 * y - 1) + 2 * T(x - 2 * y - 2) + T(x - 2 * y - 3) + 2) >> 2;
            break; }
        case 7:
            if (!(y & 1)) v = (T(x + (y >> 1)) + T(x + (y >> 1) + 1) + 1) >> 1;
            else v = (T(x + (y >> 1)) + 2 * T(x + (y >> 1) + 1) + T(x + (y >> 1) + 2) + 2) >> 2;
            break;
        case 8: {
            int z = x + 2 * y;
            if (z > 2 * n - 3) v = L(n - 1);
            else if (z == 2 * n - 3) v = (L(n - 2) + 3 * L(n - 1) + 2) >> 2;
            else if (!(z & 1)) v = (L(y + (x >> 1)) + L(y + (x >> 1) + 1) + 1) >> 1;
            else v = (L(y + (x >> 1)) + 2 * L(y + (x >> 1) + 1) + L(y + (x >> 1) + 2) + 2) >> 2;
            break; }
        }
        dst[y * stride + x] = (u8)v;
    }
#undef T
#undef L
}

static void pred_simple(u8 *dst, int stride, int n, int mode, const int *top, const int *left, int has_top, int has_left) {
    int x, y, v = 128, s = 0, i;
    if (mode == 0) { for (y = 0; y < n; y++) for (x = 0; x < n; x++) dst[y * stride + x] = (u8)top[x]; return; }
    if (mode == 1) { for (y = 0; y < n; y++) for (x = 0; x < n; x++) dst[y * stride + x] = (u8)left[y]; return; }
    /* DC */
    if (has_top && has_left) { for (i = 0; i < n; i++) s += top[i] + left[i]; v = (s + n) / (2 * n); }
    else if (has_left) { for (i = 0; i < n; i++) s += left[i]; v = (s + n / 2) / n; }
    else if (has_top) { for (i = 0; i < n; i++) s += top[i]; v = (s + n / 2) / n; }
    for (y = 0; y < n; y++) for (x = 0; x < n; x++) dst[y * stride + x] = (u8)v;
}

static void intra4x4(H264Dec *d, int x, int y, int mode, u32 done) {
    int stride = d->w, top[9], left[4], i, ht, hl, htr, htl;
    u8 *dst = d->cur->plane[0] + (d->mby * 16 + y) * stride + d->mbx * 16 + x;
    ht = intra_avail(d, x, y - 1, done); hl = intra_avail(d, x - 1, y, done);
    htr = intra_avail(d, x + 4, y - 1, done); htl = intra_avail(d, x - 1, y - 1, done);
    for (i = 0; i < 4; i++) { top[1 + i] = ht ? dst[-stride + i] : 128; left[i] = hl ? dst[i * stride - 1] : 128; }
    for (i = 4; i < 8; i++) top[1 + i] = htr ? dst[-stride + i] : top[4];
    top[0] = htl ? dst[-stride - 1] : 128;
    if (mode <= 2) pred_simple(dst, stride, 4, mode, top + 1, left, ht, hl);
    else pred_dir(dst, stride, 4, mode, top + 1, left);
}
static void intra8x8(H264Dec *d, int x, int y, int mode, u32 done) {
    int stride = d->w, p[17], q[17], l[8], lf[8], i, ht, hl, htr, htl, tl, tlf;
    u8 *dst = d->cur->plane[0] + (d->mby * 16 + y) * stride + d->mbx * 16 + x;
    ht = intra_avail(d, x, y - 1, done); hl = intra_avail(d, x - 1, y, done);
    htr = intra_avail(d, x + 8, y - 1, done); htl = intra_avail(d, x - 1, y - 1, done);
    for (i = 0; i < 8; i++) { p[i] = ht ? dst[-stride + i] : 128; l[i] = hl ? dst[i * stride - 1] : 128; }
    for (i = 8; i < 16; i++) p[i] = htr ? dst[-stride + i] : p[7];
    tl = htl ? dst[-stride - 1] : 128;
    /* reference sample filtering (8.3.2.2.1) */
    if (ht) {
        q[0] = htl ? (tl + 2 * p[0] + p[1] + 2) >> 2 : (3 * p[0] + p[1] + 2) >> 2;
        for (i = 1; i < 15; i++) q[i] = (p[i - 1] + 2 * p[i] + p[i + 1] + 2) >> 2;
        q[15] = (p[14] + 3 * p[15] + 2) >> 2;
    } else for (i = 0; i < 16; i++) q[i] = p[i];
    tlf = tl;
    if (htl) {
        if (!ht && hl) tlf = (3 * tl + l[0] + 2) >> 2;
        else if (ht && !hl) tlf = (3 * tl + p[0] + 2) >> 2;
        else if (ht && hl) tlf = (p[0] + 2 * tl + l[0] + 2) >> 2;
    }
    if (hl) {
        lf[0] = htl ? (tl + 2 * l[0] + l[1] + 2) >> 2 : (3 * l[0] + l[1] + 2) >> 2;
        for (i = 1; i < 7; i++) lf[i] = (l[i - 1] + 2 * l[i] + l[i + 1] + 2) >> 2;
        lf[7] = (l[6] + 3 * l[7] + 2) >> 2;
    } else for (i = 0; i < 8; i++) lf[i] = l[i];
    {
        int top[17];
        top[0] = tlf;
        for (i = 0; i < 16; i++) top[1 + i] = q[i];
        if (mode <= 2) pred_simple(dst, stride, 8, mode, top + 1, lf, ht, hl);
        else pred_dir(dst, stride, 8, mode, top + 1, lf);
    }
}
static void intra16x16(H264Dec *d, int mode) {
    int stride = d->w, top[16], left[16], i, x, y, ht = intra_avail_mb(d, 0, -1), hl = intra_avail_mb(d, -1, 0);
    u8 *dst = d->cur->plane[0] + d->mby * 16 * stride + d->mbx * 16;
    for (i = 0; i < 16; i++) { top[i] = ht ? dst[-stride + i] : 128; left[i] = hl ? dst[i * stride - 1] : 128; }
    if (mode == 0) { for (y = 0; y < 16; y++) for (x = 0; x < 16; x++) dst[y * stride + x] = (u8)top[x]; }
    else if (mode == 1) { for (y = 0; y < 16; y++) for (x = 0; x < 16; x++) dst[y * stride + x] = (u8)left[y]; }
    else if (mode == 2) pred_simple(dst, stride, 16, 2, top, left, ht, hl);
    else {
        int tl = dst[-stride - 1], h = 0, v = 0, a, b, c;
        for (i = 0; i < 8; i++) {
            h += (i + 1) * (top[8 + i] - (i == 7 ? tl : top[6 - i]));
            v += (i + 1) * (left[8 + i] - (i == 7 ? tl : left[6 - i]));
        }
        a = 16 * (left[15] + top[15]); b = (5 * h + 32) >> 6; c = (5 * v + 32) >> 6;
        for (y = 0; y < 16; y++) for (x = 0; x < 16; x++) dst[y * stride + x] = clip1((a + b * (x - 7) + c * (y - 7) + 16) >> 5);
    }
}
static void intra_chroma(H264Dec *d, int mode) {
    int stride = d->w / 2, c, ht = intra_avail_mb(d, 0, -1), hl = intra_avail_mb(d, -1, 0), x, y, i;
    for (c = 0; c < 2; c++) {
        u8 *dst = d->cur->plane[1 + c] + d->mby * 8 * stride + d->mbx * 8;
        int top[8], left[8];
        for (i = 0; i < 8; i++) { top[i] = ht ? dst[-stride + i] : 128; left[i] = hl ? dst[i * stride - 1] : 128; }
        if (mode == 0) {
            int by, bx;
            for (by = 0; by < 2; by++) for (bx = 0; bx < 2; bx++) {
                int st = 0, sl = 0, v = 128;
                for (i = 0; i < 4; i++) { st += top[bx * 4 + i]; sl += left[by * 4 + i]; }
                if ((bx == 0 && by == 0) || (bx == 1 && by == 1)) {
                    if (ht && hl) v = (st + sl + 4) >> 3; else if (hl) v = (sl + 2) >> 2; else if (ht) v = (st + 2) >> 2;
                } else if (bx == 1) {
                    if (ht) v = (st + 2) >> 2; else if (hl) v = (sl + 2) >> 2;
                } else {
                    if (hl) v = (sl + 2) >> 2; else if (ht) v = (st + 2) >> 2;
                }
                for (y = 0; y < 4; y++) for (x = 0; x < 4; x++) dst[(by * 4 + y) * stride + bx * 4 + x] = (u8)v;
            }
        } else if (mode == 1) { for (y = 0; y < 8; y++) for (x = 0; x < 8; x++) dst[y * stride + x] = (u8)left[y]; }
        else if (mode == 2) { for (y = 0; y < 8; y++) for (x = 0; x < 8; x++) dst[y * stride + x] = (u8)top[x]; }
        else {
            int tl = dst[-stride - 1], h = 0, v = 0, a, b, cc;
            for (i = 0; i < 4; i++) {
                h += (i + 1) * (top[4 + i] - (i == 3 ? tl : top[2 - i]));
                v += (i + 1) * (left[4 + i] - (i == 3 ? tl : left[2 - i]));
            }
            a = 16 * (left[7] + top[7]); b = (34 * h + 32) >> 6; cc = (34 * v + 32) >> 6;
            for (y = 0; y < 8; y++) for (x = 0; x < 8; x++) dst[y * stride + x] = clip1((a + b * (x - 3) + cc * (y - 3) + 16) >> 5);
        }
    }
}

/* ---------------------------------------------------------------- */
/* inter prediction (8.4.2)                                         */

static int pel(const u8 *p, int w, int h, int x, int y) {
    if (x < 0) x = 0; else if (x >= w) x = w - 1;
    if (y < 0) y = 0; else if (y >= h) y = h - 1;
    return p[y * w + x];
}
/* 6-tap filter sums (not yet rounded) */
static int tap_h(const u8 *p, int w, int h, int x, int y) {
    return pel(p, w, h, x - 2, y) - 5 * pel(p, w, h, x - 1, y) + 20 * pel(p, w, h, x, y) + 20 * pel(p, w, h, x + 1, y) - 5 * pel(p, w, h, x + 2, y) + pel(p, w, h, x + 3, y);
}
static int tap_v(const u8 *p, int w, int h, int x, int y) {
    return pel(p, w, h, x, y - 2) - 5 * pel(p, w, h, x, y - 1) + 20 * pel(p, w, h, x, y) + 20 * pel(p, w, h, x, y + 1) - 5 * pel(p, w, h, x, y + 2) + pel(p, w, h, x, y + 3);
}
static int half_j(const u8 *p, int w, int h, int x, int y) {
    int s = tap_h(p, w, h, x, y - 2) - 5 * tap_h(p, w, h, x, y - 1) + 20 * tap_h(p, w, h, x, y) + 20 * tap_h(p, w, h, x, y + 1) - 5 * tap_h(p, w, h, x, y + 2) + tap_h(p, w, h, x, y + 3);
    return clip1((s + 512) >> 10);
}
static int luma_sample(const u8 *p, int w, int h, int x, int y, int fx, int fy) {
    int G, b, hh, j, s, m;
    if (!fx && !fy) return pel(p, w, h, x, y);
    G = pel(p, w, h, x, y);
    switch (fy * 4 + fx) {
    case 1: b = clip1((tap_h(p, w, h, x, y) + 16) >> 5); return (G + b + 1) >> 1;
    case 2: return clip1((tap_h(p, w, h, x, y) + 16) >> 5);
    case 3: b = clip1((tap_h(p, w, h, x, y) + 16) >> 5); return (pel(p, w, h, x + 1, y) + b + 1) >> 1;
    case 4: hh = clip1((tap_v(p, w, h, x, y) + 16) >> 5); return (G + hh + 1) >> 1;
    case 8: return clip1((tap_v(p, w, h, x, y) + 16) >> 5);
    case 12: hh = clip1((tap_v(p, w, h, x, y) + 16) >> 5); return (pel(p, w, h, x, y + 1) + hh + 1) >> 1;
    case 5: b = clip1((tap_h(p, w, h, x, y) + 16) >> 5); hh = clip1((tap_v(p, w, h, x, y) + 16) >> 5); return (b + hh + 1) >> 1;
    case 7: b = clip1((tap_h(p, w, h, x, y) + 16) >> 5); m = clip1((tap_v(p, w, h, x + 1, y) + 16) >> 5); return (b + m + 1) >> 1;
    case 13: hh = clip1((tap_v(p, w, h, x, y) + 16) >> 5); s = clip1((tap_h(p, w, h, x, y + 1) + 16) >> 5); return (hh + s + 1) >> 1;
    case 15: m = clip1((tap_v(p, w, h, x + 1, y) + 16) >> 5); s = clip1((tap_h(p, w, h, x, y + 1) + 16) >> 5); return (m + s + 1) >> 1;
    case 10: return half_j(p, w, h, x, y);
    case 6: b = clip1((tap_h(p, w, h, x, y) + 16) >> 5); j = half_j(p, w, h, x, y); return (b + j + 1) >> 1;
    case 14: s = clip1((tap_h(p, w, h, x, y + 1) + 16) >> 5); j = half_j(p, w, h, x, y); return (j + s + 1) >> 1;
    case 9: hh = clip1((tap_v(p, w, h, x, y) + 16) >> 5); j = half_j(p, w, h, x, y); return (hh + j + 1) >> 1;
    case 11: m = clip1((tap_v(p, w, h, x + 1, y) + 16) >> 5); j = half_j(p, w, h, x, y); return (j + m + 1) >> 1;
    }
    return G;
}

/* prediction of one list for a 4x4 luma block and its 2x2 chroma blocks */
static void mc_block(H264Dec *d, const H264Pic *ref, int bx, int by, const s16 *mv, int *ly, int *lc) {
    int x, y, c, w = d->w, h = d->h, cw = w / 2, ch = h / 2;
    int x0 = bx + (mv[0] >> 2), y0 = by + (mv[1] >> 2), fx = mv[0] & 3, fy = mv[1] & 3;
    for (y = 0; y < 4; y++) for (x = 0; x < 4; x++) ly[y * 4 + x] = luma_sample(ref->plane[0], w, h, x0 + x, y0 + y, fx, fy);
    for (c = 0; c < 2; c++) {
        int cx0 = bx / 2 + (mv[0] >> 3), cy0 = by / 2 + (mv[1] >> 3), xf = mv[0] & 7, yf = mv[1] & 7;
        const u8 *p = ref->plane[1 + c];
        for (y = 0; y < 2; y++) for (x = 0; x < 2; x++) {
            int A = pel(p, cw, ch, cx0 + x, cy0 + y), B = pel(p, cw, ch, cx0 + x + 1, cy0 + y);
            int C = pel(p, cw, ch, cx0 + x, cy0 + y + 1), D = pel(p, cw, ch, cx0 + x + 1, cy0 + y + 1);
            lc[c * 4 + y * 2 + x] = ((8 - xf) * (8 - yf) * A + xf * (8 - yf) * B + (8 - xf) * yf * C + xf * yf * D + 32) >> 6;
        }
    }
}

static void inter_pred(H264Dec *d, MB *m) {
    int blk, stride = d->w, cstride = d->w / 2;
    int explicit_w = (d->sh.type == SL_P && d->p->weighted_pred) || (d->sh.type == SL_B && d->p->weighted_bipred == 1);
    int implicit_w = d->sh.type == SL_B && d->p->weighted_bipred == 2;
    for (blk = 0; blk < 16; blk++) {
        int b8 = (blk >> 3) * 2 + ((blk & 3) >> 1), bx = d->mbx * 16 + (blk & 3) * 4, by = d->mby * 16 + (blk >> 2) * 4;
        int r0 = m->ref[0][b8], r1 = m->ref[1][b8], y0[16], y1[16], c0[8], c1[8], i, x, y;
        const H264Pic *p0 = r0 >= 0 ? d->list[0][r0] : NULL, *p1 = r1 >= 0 ? d->list[1][r1] : NULL;
        u8 *dy = d->cur->plane[0] + by * stride + bx;
        if (r0 >= 0 && !p0) p0 = d->list[0][0];
        if (r1 >= 0 && !p1) p1 = d->list[1][0];
        if (r0 >= 0 && p0) mc_block(d, p0, bx, by, m->mv[0][blk], y0, c0);
        if (r1 >= 0 && p1) mc_block(d, p1, bx, by, m->mv[1][blk], y1, c1);
        if (!p0 && !p1) { for (i = 0; i < 16; i++) y0[i] = 128; for (i = 0; i < 8; i++) c0[i] = 128; p0 = d->list[0][0]; r0 = 0; }
        for (i = 0; i < 24; i++) {
            int a = 0, b = 0, v, comp = i < 16 ? 0 : 1 + (i - 16) / 4;
            if (i < 16) { a = y0[i]; b = y1[i]; } else { a = c0[i - 16]; b = c1[i - 16]; }
            if (p0 && p1) {
                if (explicit_w) {
                    int lwd = comp ? d->sh.chroma_log2_wd : d->sh.luma_log2_wd;
                    int w0 = comp ? d->sh.cw[0][r0][comp - 1] : d->sh.lw[0][r0], w1 = comp ? d->sh.cw[1][r1][comp - 1] : d->sh.lw[1][r1];
                    int o0 = comp ? d->sh.co[0][r0][comp - 1] : d->sh.lo[0][r0], o1 = comp ? d->sh.co[1][r1][comp - 1] : d->sh.lo[1][r1];
                    v = ((a * w0 + b * w1 + (1 << lwd)) >> (lwd + 1)) + ((o0 + o1 + 1) >> 1);
                } else if (implicit_w && d->implicit_w[r0][r1] >= 0) {
                    int w1 = d->implicit_w[r0][r1], w0 = 64 - w1;
                    v = (a * w0 + b * w1 + 32) >> 6;
                } else v = (a + b + 1) >> 1;
            } else {
                int l = p0 ? 0 : 1, r = p0 ? r0 : r1;
                v = p0 ? a : b;
                if (explicit_w) {
                    int lwd = comp ? d->sh.chroma_log2_wd : d->sh.luma_log2_wd;
                    int w = comp ? d->sh.cw[l][r][comp - 1] : d->sh.lw[l][r], o = comp ? d->sh.co[l][r][comp - 1] : d->sh.lo[l][r];
                    if (lwd >= 1) v = ((v * w + (1 << (lwd - 1))) >> lwd) + o;
                    else v = v * w + o;
                }
            }
            v = clip1(v);
            if (i < 16) dy[(i >> 2) * stride + (i & 3)] = (u8)v;
            else {
                int k = (i - 16) & 3; u8 *dc = d->cur->plane[comp] + (by / 2 + (k >> 1)) * cstride + bx / 2 + (k & 1);
                *dc = (u8)v;
            }
        }
        (void)x; (void)y;
    }
}

/* ---------------------------------------------------------------- */
/* macroblock reconstruction                                        */

static int chroma_qp(const PPS *p, int qp, int c) {
    int q = qp + p->chroma_qp_offset[c];
    q = q < 0 ? 0 : q > 51 ? 51 : q;
    return h264_chroma_qp[q];
}

static void recon_mb(H264Dec *d, MB *m, MBData *md) {
    int stride = d->w, cstride = d->w / 2, i, c, intra = IS_INTRA(md->type), qp = m->qp;
    u8 *ly = d->cur->plane[0] + d->mby * 16 * stride + d->mbx * 16;
    u32 done = 0;
    if (md->type == MB_PCM) {
        for (i = 0; i < 256; i++) ly[(i >> 4) * stride + (i & 15)] = md->pcm[i];
        for (c = 0; c < 2; c++) {
            u8 *dc = d->cur->plane[1 + c] + d->mby * 8 * cstride + d->mbx * 8;
            for (i = 0; i < 64; i++) dc[(i >> 3) * cstride + (i & 7)] = md->pcm[256 + c * 64 + i];
        }
        return;
    }
    if (md->type == MB_I16) {
        s32 f[16], t[16]; int q6 = qp / 6, ls = g_ls.l4[0][qp % 6][0];
        intra16x16(d, md->i16mode);
        for (i = 0; i < 4; i++) {   /* rows */
            s32 *r = md->dc + i * 4;
            t[i * 4 + 0] = r[0] + r[1] + r[2] + r[3]; t[i * 4 + 1] = r[0] + r[1] - r[2] - r[3];
            t[i * 4 + 2] = r[0] - r[1] - r[2] + r[3]; t[i * 4 + 3] = r[0] - r[1] + r[2] - r[3];
        }
        for (i = 0; i < 4; i++) {   /* columns */
            f[0 * 4 + i] = t[i] + t[4 + i] + t[8 + i] + t[12 + i]; f[1 * 4 + i] = t[i] + t[4 + i] - t[8 + i] - t[12 + i];
            f[2 * 4 + i] = t[i] - t[4 + i] - t[8 + i] + t[12 + i]; f[3 * 4 + i] = t[i] - t[4 + i] + t[8 + i] - t[12 + i];
        }
        for (i = 0; i < 16; i++) {
            s32 v = qp >= 36 ? (f[i] * ls) << (q6 - 6) : (f[i] * ls + (1 << (5 - q6))) >> (6 - q6);
            dequant4(md->luma[i], 0, qp, 1);
            md->luma[i][0] = v;
            idct4_add(md->luma[i], ly + (i >> 2) * 4 * stride + (i & 3) * 4, stride);
        }
    } else if (md->type == MB_I4) {
        for (i = 0; i < 16; i++) {
            int r = blk2raster[i], x = (r & 3) * 4, y = (r >> 2) * 4;
            intra4x4(d, x, y, m->ipred[r], done);
            if (m->nz[r]) { dequant4(md->luma[r], 0, qp, 0); idct4_add(md->luma[r], ly + y * stride + x, stride); }
            done |= 1u << r;
        }
    } else if (md->type == MB_I8) {
        for (i = 0; i < 4; i++) {
            int x = (i & 1) * 8, y = (i >> 1) * 8;
            intra8x8(d, x, y, m->ipred[(y / 4) * 4 + x / 4], done);
            if ((m->cbp >> i) & 1) { dequant8(md->luma8[i], 0, qp); idct8_add(md->luma8[i], ly + y * stride + x, stride); }
            done |= b8_mask(i);
        }
    } else {
        inter_pred(d, m);
        if (m->t8x8) {
            for (i = 0; i < 4; i++) if ((m->cbp >> i) & 1) {
                int x = (i & 1) * 8, y = (i >> 1) * 8;
                dequant8(md->luma8[i], 1, qp); idct8_add(md->luma8[i], ly + y * stride + x, stride);
            }
        } else {
            for (i = 0; i < 16; i++) if (m->nz[i]) { dequant4(md->luma[i], 3, qp, 0); idct4_add(md->luma[i], ly + (i >> 2) * 4 * stride + (i & 3) * 4, stride); }
        }
    }
    if (intra) intra_chroma(d, m->chroma_mode);
    if (m->cbp >> 4) {
        for (c = 0; c < 2; c++) {
            int qc = m->qpc[c], list = (intra ? 1 : 4) + c, ls = g_ls.l4[list][qc % 6][0];
            s32 *cd = md->cdc[c], f[4];
            u8 *dst = d->cur->plane[1 + c] + d->mby * 8 * cstride + d->mbx * 8;
            f[0] = cd[0] + cd[1] + cd[2] + cd[3]; f[1] = cd[0] - cd[1] + cd[2] - cd[3];
            f[2] = cd[0] + cd[1] - cd[2] - cd[3]; f[3] = cd[0] - cd[1] - cd[2] + cd[3];
            for (i = 0; i < 4; i++) {
                s32 *blk = md->cac[c][i];
                dequant4(blk, list, qc, 1);
                blk[0] = ((f[i] * ls) << (qc / 6)) >> 5;
                idct4_add(blk, dst + (i >> 1) * 4 * cstride + (i & 1) * 4, cstride);
            }
        }
    }
}

/* ---------------------------------------------------------------- */
/* deblocking (8.7)                                                 */

/* non-zero coefficients in the transform block of 4x4 `b` (8x8 blocks with the 8x8 transform) */
static int nz_blk(const MB *m, int b) {
    int x, y;
    if (!m->t8x8) return m->nz[b] != 0;
    x = b & 2; y = b & 8;
    return m->nz[y + x] || m->nz[y + x + 1] || m->nz[y + 4 + x] || m->nz[y + 4 + x + 1];
}
static int mv_far(const s16 *a, const s16 *b) { return abs(a[0] - b[0]) >= 4 || abs(a[1] - b[1]) >= 4; }

static int edge_bs(H264Dec *d, MB *p, int pb, MB *q, int qb, int mbedge) {
    int bp8 = (pb >> 3) * 2 + ((pb & 3) >> 1), bq8 = (qb >> 3) * 2 + ((qb & 3) >> 1), np, nq;
    const H264Pic *p0, *p1, *q0, *q1;
    if (IS_INTRA(p->type) || IS_INTRA(q->type)) return mbedge ? 4 : 3;
    if (nz_blk(p, pb) || nz_blk(q, qb)) return 2;
    p0 = p->ref[0][bp8] >= 0 ? p->refpic[0][bp8] : NULL; p1 = p->ref[1][bp8] >= 0 ? p->refpic[1][bp8] : NULL;
    q0 = q->ref[0][bq8] >= 0 ? q->refpic[0][bq8] : NULL; q1 = q->ref[1][bq8] >= 0 ? q->refpic[1][bq8] : NULL;
    np = (p->ref[0][bp8] >= 0) + (p->ref[1][bp8] >= 0); nq = (q->ref[0][bq8] >= 0) + (q->ref[1][bq8] >= 0);
    if (np != nq) return 1;
    if (np == 1) {
        const H264Pic *pr = p->ref[0][bp8] >= 0 ? p0 : p1, *qr = q->ref[0][bq8] >= 0 ? q0 : q1;
        const s16 *pm = p->ref[0][bp8] >= 0 ? p->mv[0][pb] : p->mv[1][pb], *qm = q->ref[0][bq8] >= 0 ? q->mv[0][qb] : q->mv[1][qb];
        if (pr != qr) return 1;
        return mv_far(pm, qm);
    }
    /* two motion vectors each */
    if (!((p0 == q0 && p1 == q1) || (p0 == q1 && p1 == q0))) return 1;
    if (p0 != p1) {
        if (p0 == q0) return mv_far(p->mv[0][pb], q->mv[0][qb]) || mv_far(p->mv[1][pb], q->mv[1][qb]);
        return mv_far(p->mv[0][pb], q->mv[1][qb]) || mv_far(p->mv[1][pb], q->mv[0][qb]);
    }
    return (mv_far(p->mv[0][pb], q->mv[0][qb]) || mv_far(p->mv[1][pb], q->mv[1][qb]))
        && (mv_far(p->mv[0][pb], q->mv[1][qb]) || mv_far(p->mv[1][pb], q->mv[0][qb]));
}

/* filter n samples along an edge: pix points at q0 of the first line, `across` steps from p to q,
   `along` to the next line; bs[i] applies to lines i * n / 4 .. */
static void filter_edge(u8 *pix, int across, int along, int n, const int *bs, int qpav, int aoff, int boff, int chroma) {
    int ia = qpav + aoff, ib = qpav + boff, alpha, beta, k;
    ia = ia < 0 ? 0 : ia > 51 ? 51 : ia; ib = ib < 0 ? 0 : ib > 51 ? 51 : ib;
    alpha = h264_alpha[ia]; beta = h264_beta[ib];
    for (k = 0; k < n; k++) {
        u8 *q = pix + k * along;
        int s = bs[k * 4 / n], p0 = q[-across], p1 = q[-2 * across], q0 = q[0], q1 = q[across];
        if (!s) continue;
        if (abs(p0 - q0) >= alpha || abs(p1 - p0) >= beta || abs(q1 - q0) >= beta) continue;
        if (s < 4) {
            int tc0 = h264_tc0[ia][s - 1], tc, delta;
            if (chroma) tc = tc0 + 1;
            else {
                int p2 = q[-3 * across], q2 = q[2 * across], ap = abs(p2 - p0) < beta, aq = abs(q2 - q0) < beta;
                tc = tc0 + ap + aq;
                if (ap) { int v = (p2 + ((p0 + q0 + 1) >> 1) - (p1 << 1)) >> 1; q[-2 * across] = (u8)(p1 + (v < -tc0 ? -tc0 : v > tc0 ? tc0 : v)); }
                if (aq) { int v = (q2 + ((p0 + q0 + 1) >> 1) - (q1 << 1)) >> 1; q[across] = (u8)(q1 + (v < -tc0 ? -tc0 : v > tc0 ? tc0 : v)); }
            }
            delta = (((q0 - p0) << 2) + (p1 - q1) + 4) >> 3;
            delta = delta < -tc ? -tc : delta > tc ? tc : delta;
            q[-across] = clip1(p0 + delta); q[0] = clip1(q0 - delta);
        } else if (chroma) {
            q[-across] = (u8)((2 * p1 + p0 + q1 + 2) >> 2);
            q[0] = (u8)((2 * q1 + q0 + p1 + 2) >> 2);
        } else {
            int p2 = q[-3 * across], q2 = q[2 * across], p3 = q[-4 * across], q3 = q[3 * across];
            int small = abs(p0 - q0) < ((alpha >> 2) + 2);
            if (small && abs(p2 - p0) < beta) {
                q[-across] = (u8)((p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3);
                q[-2 * across] = (u8)((p2 + p1 + p0 + q0 + 2) >> 2);
                q[-3 * across] = (u8)((2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3);
            } else q[-across] = (u8)((2 * p1 + p0 + q1 + 2) >> 2);
            if (small && abs(q2 - q0) < beta) {
                q[0] = (u8)((p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3);
                q[across] = (u8)((p0 + q0 + q1 + q2 + 2) >> 2);
                q[2 * across] = (u8)((2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3);
            } else q[0] = (u8)((2 * q1 + q0 + p1 + 2) >> 2);
        }
    }
}

static void deblock_mb(H264Dec *d, int mbx, int mby) {
    MB *q = &d->mbs[mby * d->mbw + mbx];
    int stride = d->w, cstride = d->w / 2, dir, e, k, c;
    u8 *ly = d->cur->plane[0] + mby * 16 * stride + mbx * 16;
    if (!q->slice || q->deblock_idc == 1) return;
    for (dir = 0; dir < 2; dir++) {   /* 0: vertical edges, 1: horizontal edges */
        for (e = 0; e < 4; e++) {
            MB *p = q; int bs[4], qpav, mbedge = e == 0;
            if (e == 0) {
                if (dir == 0 ? mbx == 0 : mby == 0) continue;
                p = dir == 0 ? q - 1 : q - d->mbw;
                if (!p->slice) continue;
                if (q->deblock_idc == 2 && p->slice != q->slice) continue;
            } else if (q->t8x8 && e != 2) goto chroma_only;
            for (k = 0; k < 4; k++) {
                int qb = dir == 0 ? k * 4 + e : e * 4 + k, pb = dir == 0 ? (e ? qb - 1 : k * 4 + 3) : (e ? qb - 4 : 12 + k);
                bs[k] = edge_bs(d, p, pb, q, qb, mbedge);
            }
            qpav = ((p->type == MB_PCM ? 0 : p->qp) + (q->type == MB_PCM ? 0 : q->qp) + 1) >> 1;
            if (dir == 0) filter_edge(ly + e * 4, 1, stride, 16, bs, qpav, q->alpha_off, q->beta_off, 0);
            else filter_edge(ly + e * 4 * stride, stride, 1, 16, bs, qpav, q->alpha_off, q->beta_off, 0);
            goto chroma;
        chroma_only:
            if (e != 2) continue;
            for (k = 0; k < 4; k++) {
                int qb = dir == 0 ? k * 4 + e : e * 4 + k, pb = dir == 0 ? qb - 1 : qb - 4;
                bs[k] = edge_bs(d, p, pb, q, qb, 0);
            }
        chroma:
            if (e == 0 || e == 2) {
                for (c = 0; c < 2; c++) {
                    u8 *cp = d->cur->plane[1 + c] + mby * 8 * cstride + mbx * 8;
                    int qpp = p->type == MB_PCM ? chroma_qp(d->p, 0, c) : p->qpc[c], qpq = q->type == MB_PCM ? chroma_qp(d->p, 0, c) : q->qpc[c];
                    int cav = (qpp + qpq + 1) >> 1;
                    if (dir == 0) filter_edge(cp + e * 2, 1, cstride, 8, bs, cav, q->alpha_off, q->beta_off, 1);
                    else filter_edge(cp + e * 2 * cstride, cstride, 1, 8, bs, cav, q->alpha_off, q->beta_off, 1);
                }
            }
        }
    }
}

/* ---------------------------------------------------------------- */
/* pictures and reference marking (8.2)                             */

#define POC_NONE (-2147483647 - 1)

static void pic_free(H264Pic *p) {
    free(p->plane[0]); free(p->mv); free(p->ref_idx); free(p->ref_poc); free(p->ref_lt); free(p->intra);
    memset(p, 0, sizeof(*p));
}
static int pic_alloc(H264Dec *d, H264Pic *p) {
    size_t n = (size_t)d->w * d->h, nmb = (size_t)d->mbw * d->mbh;
    if (!p->plane[0]) {
        p->plane[0] = (u8*)malloc(n * 3 / 2);
        p->mv = (s16(*)[2][2])malloc(nmb * 16 * sizeof(*p->mv));
        p->ref_idx = (s8(*)[2])malloc(nmb * 16 * sizeof(*p->ref_idx));
        p->ref_poc = (int(*)[2][4])malloc(nmb * sizeof(*p->ref_poc));
        p->ref_lt = (u8(*)[2][4])malloc(nmb * sizeof(*p->ref_lt));
        p->intra = (u8*)malloc(nmb);
        if (!p->plane[0] || !p->mv || !p->ref_idx || !p->ref_poc || !p->ref_lt || !p->intra) { pic_free(p); return 0; }
    }
    p->plane[1] = p->plane[0] + n; p->plane[2] = p->plane[1] + n / 4;
    return 1;
}

static int max_frame_num(H264Dec *d) { return 1 << d->s->log2_max_frame_num; }

/* PicNum / LongTermPicNum of the references relative to the current frame_num */
static void update_pic_nums(H264Dec *d, int frame_num) {
    int i;
    for (i = 0; i < H264_DPB; i++) {
        H264Pic *p = &d->dpb[i];
        if (p->ref == 1) { p->frame_num_wrap = p->frame_num > frame_num ? p->frame_num - max_frame_num(d) : p->frame_num; p->pic_num = p->frame_num_wrap; }
        else if (p->ref == 2) p->long_term_pic_num = p->long_term_frame_idx;
    }
}

static int cmp_desc_picnum(const void *a, const void *b) { return (*(H264Pic* const*)b)->pic_num - (*(H264Pic* const*)a)->pic_num; }
static int cmp_asc_ltnum(const void *a, const void *b) { return (*(H264Pic* const*)a)->long_term_pic_num - (*(H264Pic* const*)b)->long_term_pic_num; }
static int cmp_desc_poc(const void *a, const void *b) { int x = (*(H264Pic* const*)a)->poc, y = (*(H264Pic* const*)b)->poc; return x > y ? -1 : x < y; }
static int cmp_asc_poc(const void *a, const void *b) { int x = (*(H264Pic* const*)a)->poc, y = (*(H264Pic* const*)b)->poc; return x < y ? -1 : x > y; }

static void init_ref_lists(H264Dec *d) {
    H264Pic *st[H264_DPB], *lt[H264_DPB], *before[H264_DPB], *after[H264_DPB];
    H264Pic *init[2][H264_DPB * 2];
    int nst = 0, nlt = 0, nb = 0, na = 0, i, l, n[2] = { 0, 0 }, cur_poc = d->cur->poc;
    for (i = 0; i < H264_DPB; i++) {
        H264Pic *p = &d->dpb[i];
        if (p->busy) continue;
        if (p->ref == 1) st[nst++] = p; else if (p->ref == 2) lt[nlt++] = p;
    }
    qsort(lt, (size_t)nlt, sizeof(H264Pic*), cmp_asc_ltnum);
    if (d->sh.type == SL_P) {
        qsort(st, (size_t)nst, sizeof(H264Pic*), cmp_desc_picnum);
        for (i = 0; i < nst; i++) init[0][n[0]++] = st[i];
        for (i = 0; i < nlt; i++) init[0][n[0]++] = lt[i];
    } else {
        for (i = 0; i < nst; i++) { if (st[i]->poc < cur_poc) before[nb++] = st[i]; else after[na++] = st[i]; }
        qsort(before, (size_t)nb, sizeof(H264Pic*), cmp_desc_poc);
        qsort(after, (size_t)na, sizeof(H264Pic*), cmp_asc_poc);
        for (i = 0; i < nb; i++) init[0][n[0]++] = before[i];
        for (i = 0; i < na; i++) init[0][n[0]++] = after[i];
        for (i = 0; i < nlt; i++) init[0][n[0]++] = lt[i];
        for (i = 0; i < na; i++) init[1][n[1]++] = after[i];
        for (i = 0; i < nb; i++) init[1][n[1]++] = before[i];
        for (i = 0; i < nlt; i++) init[1][n[1]++] = lt[i];
        if (n[1] > 1 && n[0] == n[1]) {
            int same = 1;
            for (i = 0; i < n[1]; i++) if (init[0][i] != init[1][i]) same = 0;
            if (same) { H264Pic *t = init[1][0]; init[1][0] = init[1][1]; init[1][1] = t; }
        }
    }
    for (l = 0; l < 2; l++) {
        int nr = d->sh.num_ref[l], k;
        for (i = 0; i <= H264_MAXREF; i++) d->list[l][i] = i < n[l] && i < nr ? init[l][i] : NULL;
        /* modification (8.2.4.3) */
        {
            int pred = d->sh.frame_num, maxpn = max_frame_num(d), idx = 0;
            for (k = 0; k < d->sh.nmod[l]; k++) {
                int idc = d->sh.mod_idc[l][k], val = d->sh.mod_val[l][k], c, nidx;
                H264Pic *pic = NULL;
                if (idc == 0 || idc == 1) {
                    int num, j;
                    if (idc == 0) { pred -= val + 1; if (pred < 0) pred += maxpn; }
                    else { pred += val + 1; if (pred >= maxpn) pred -= maxpn; }
                    num = pred > d->sh.frame_num ? pred - maxpn : pred;
                    for (j = 0; j < H264_DPB; j++) if (d->dpb[j].ref == 1 && !d->dpb[j].busy && d->dpb[j].pic_num == num) { pic = &d->dpb[j]; break; }
                } else if (idc == 2) {
                    int j;
                    for (j = 0; j < H264_DPB; j++) if (d->dpb[j].ref == 2 && d->dpb[j].long_term_pic_num == val) { pic = &d->dpb[j]; break; }
                } else continue;
                if (idx >= nr) break;
                for (c = nr; c > idx; c--) d->list[l][c] = d->list[l][c - 1];
                d->list[l][idx++] = pic;
                nidx = idx;
                for (c = idx; c <= nr; c++) {
                    H264Pic *q = d->list[l][c];
                    int same = q && pic && q == pic;
                    if (!same) d->list[l][nidx++] = q;
                }
                d->list[l][nr] = NULL;
            }
        }
    }
}

static int clip3(int lo, int hi, int v) { return v < lo ? lo : v > hi ? hi : v; }

static void init_weights(H264Dec *d) {
    int i, j, cur = d->cur->poc;
    for (i = 0; i < 32; i++) for (j = 0; j < 32; j++) d->implicit_w[i][j] = -1;
    for (i = 0; i < 32; i++) d->dist_scale[i] = 9999;
    if (d->sh.type != SL_B) return;
    for (i = 0; i < d->sh.num_ref[0]; i++) {
        const H264Pic *p0 = d->list[0][i], *p1 = d->list[1][0];
        if (p0 && p1 && p0->ref != 2) {
            int tb = clip3(-128, 127, cur - p0->poc), td = clip3(-128, 127, p1->poc - p0->poc);
            if (td) { int tx = (16384 + abs(td / 2)) / td; d->dist_scale[i] = clip3(-1024, 1023, (tb * tx + 32) >> 6); }
        }
        for (j = 0; j < d->sh.num_ref[1]; j++) {
            const H264Pic *q1 = d->list[1][j];
            int tb, td, tx, dsf;
            if (!p0 || !q1 || p0->ref == 2 || q1->ref == 2) continue;
            td = clip3(-128, 127, q1->poc - p0->poc);
            if (!td) continue;
            tb = clip3(-128, 127, cur - p0->poc);
            tx = (16384 + abs(td / 2)) / td;
            dsf = clip3(-1024, 1023, (tb * tx + 32) >> 6);
            if ((dsf >> 2) < -64 || (dsf >> 2) > 128) continue;
            d->implicit_w[i][j] = dsf >> 2;
        }
    }
}

static void mark_refs(H264Dec *d) {
    H264Pic *cur = d->cur; int i, k;
    const SliceHdr *h = &d->first;
    if (!h->nal_ref_idc) return;
    if (h->idr) {
        for (i = 0; i < H264_DPB; i++) if (&d->dpb[i] != cur) d->dpb[i].ref = 0;
        if (h->long_term_ref) { cur->ref = 2; cur->long_term_frame_idx = 0; d->max_long_term_idx = 0; }
        else { cur->ref = 1; d->max_long_term_idx = -1; }
        return;
    }
    update_pic_nums(d, h->frame_num);
    cur->ref = 0;
    if (h->adaptive) {
        for (k = 0; k < h->nmmco; k++) {
            int op = h->mmco[k][0], v1 = h->mmco[k][1], v2 = h->mmco[k][2];
            if (op == 1 || op == 3) {
                int num = h->frame_num - (v1 + 1);
                for (i = 0; i < H264_DPB; i++) {
                    H264Pic *p = &d->dpb[i];
                    if (p == cur || p->ref != 1 || p->pic_num != num) continue;
                    if (op == 1) p->ref = 0;
                    else {
                        int j;
                        for (j = 0; j < H264_DPB; j++) if (d->dpb[j].ref == 2 && d->dpb[j].long_term_frame_idx == v2) d->dpb[j].ref = 0;
                        p->ref = 2; p->long_term_frame_idx = v2; p->long_term_pic_num = v2;
                    }
                }
            } else if (op == 2) {
                for (i = 0; i < H264_DPB; i++) if (&d->dpb[i] != cur && d->dpb[i].ref == 2 && d->dpb[i].long_term_pic_num == v1) d->dpb[i].ref = 0;
            } else if (op == 4) {
                d->max_long_term_idx = v1 - 1;
                for (i = 0; i < H264_DPB; i++) if (&d->dpb[i] != cur && d->dpb[i].ref == 2 && d->dpb[i].long_term_frame_idx > d->max_long_term_idx) d->dpb[i].ref = 0;
            } else if (op == 5) {
                for (i = 0; i < H264_DPB; i++) if (&d->dpb[i] != cur) d->dpb[i].ref = 0;
                d->max_long_term_idx = -1;
                cur->mmco5 = 1;
            } else if (op == 6) {
                for (i = 0; i < H264_DPB; i++) if (&d->dpb[i] != cur && d->dpb[i].ref == 2 && d->dpb[i].long_term_frame_idx == v2) d->dpb[i].ref = 0;
                cur->ref = 2; cur->long_term_frame_idx = v2; cur->long_term_pic_num = v2;
            }
        }
    } else {
        int nst = 0, nlt = 0, maxr = d->s->max_num_ref_frames < 1 ? 1 : d->s->max_num_ref_frames;
        for (i = 0; i < H264_DPB; i++) { if (&d->dpb[i] == cur) continue; if (d->dpb[i].ref == 1) nst++; else if (d->dpb[i].ref == 2) nlt++; }
        if (nst + nlt >= maxr && nst > 0) {
            H264Pic *old = NULL;
            for (i = 0; i < H264_DPB; i++) {
                H264Pic *p = &d->dpb[i];
                if (p != cur && p->ref == 1 && (!old || p->frame_num_wrap < old->frame_num_wrap)) old = p;
            }
            if (old) old->ref = 0;
        }
    }
    if (cur->ref != 2) cur->ref = 1;
    if (cur->mmco5) { cur->poc = 0; cur->frame_num = 0; }
}

/* picture order count of the picture starting with slice h (8.2.1); *reorder: the value OpenH264 keeps */
static int compute_poc(H264Dec *d, const SliceHdr *h, int *reorder) {
    const SPS *s = d->s; int top, bottom, mfn = max_frame_num(d);
    *reorder = 0;
    if (s->poc_type == 0) {
        int max_lsb = 1 << s->log2_max_poc_lsb, pmsb = d->prev_poc_msb, plsb = d->prev_poc_lsb, msb;
        if (h->idr) pmsb = plsb = 0;
        if (h->poc_lsb < plsb && plsb - h->poc_lsb >= max_lsb / 2) msb = pmsb + max_lsb;
        else if (h->poc_lsb > plsb && h->poc_lsb - plsb > max_lsb / 2) msb = pmsb - max_lsb;
        else msb = pmsb;
        top = msb + h->poc_lsb;
        bottom = top + h->delta_poc_bottom;
        *reorder = top + (d->p->bottom_field_pic_order ? h->delta_poc_bottom : 0);
        d->frame_num_offset = 0;
        if (h->nal_ref_idc) { d->prev_poc_msb = msb; d->prev_poc_lsb = h->poc_lsb; }
    } else {
        int fno;
        if (h->idr) fno = 0;
        else if (d->prev_frame_num > h->frame_num) fno = d->prev_frame_num_offset + mfn;
        else fno = d->prev_frame_num_offset;
        d->frame_num_offset = fno;
        if (s->poc_type == 1) {
            int abs_fn = s->num_ref_frames_in_poc_cycle ? fno + h->frame_num : 0, expected = 0, i;
            if (!h->nal_ref_idc && abs_fn > 0) abs_fn--;
            if (abs_fn > 0) {
                int cyc = (abs_fn - 1) / s->num_ref_frames_in_poc_cycle, in = (abs_fn - 1) % s->num_ref_frames_in_poc_cycle, delta = 0;
                for (i = 0; i < s->num_ref_frames_in_poc_cycle; i++) delta += s->offset_for_ref_frame[i];
                expected = cyc * delta;
                for (i = 0; i <= in; i++) expected += s->offset_for_ref_frame[i];
            }
            if (!h->nal_ref_idc) expected += s->offset_for_non_ref_pic;
            top = expected + h->delta_poc[0];
            bottom = top + s->offset_for_top_to_bottom + h->delta_poc[1];
        } else {
            top = bottom = h->idr ? 0 : (!h->nal_ref_idc ? 2 * (fno + h->frame_num) - 1 : 2 * (fno + h->frame_num));
        }
    }
    return top < bottom ? top : bottom;
}

/* ---------------------------------------------------------------- */
/* output (OpenH264 2.4.1 DecodeFrameNoDelay re-ordering)           */

static void emit(H264Dec *d, const u8 *yuv) {
    int w = d->outw, h = d->outh, cw = d->w / 2;
    const u8 *y = yuv + d->cropy * d->w + d->cropx, *cb = yuv + d->w * d->h + (d->cropy / 2) * cw + d->cropx / 2, *cr = cb + (d->w / 2) * (d->h / 2);
    if (!d->img || d->img->w != w || d->img->h != h) {
        if (d->img) { free(d->img->px); free(d->img); }
        d->img = (Bitmap*)calloc(1, sizeof(Bitmap));
        if (!d->img) return;
        d->img->w = w; d->img->h = h;
        d->img->px = (u32*)malloc((size_t)w * h * 4);
        if (!d->img->px) { free(d->img); d->img = NULL; return; }
    }
    vdec_yuv420_to_rgba(y, d->w, cb, cr, cw, NULL, 0, w, h, d->img->px);
    d->out_ready = 1;
}
static void release_reorder(H264Dec *d, int cur_poc, int cur_seq) {
    int i, mi = -1;
    for (i = 0; i <= d->largest; i++) {
        Pending *p = &d->pend[i];
        if (!p->used) continue;
        if (mi < 0) { mi = i; continue; }
        if (p->seq == d->pend[mi].seq ? p->poc < d->pend[mi].poc : p->seq - d->pend[mi].seq < 0) mi = i;
    }
    if (mi < 0) return;
    if ((d->last_poc != POC_NONE && d->pend[mi].poc - d->last_poc <= 1) || d->pend[mi].poc < cur_poc || d->pend[mi].seq - cur_seq < 0) {
        d->last_poc = d->pend[mi].poc; d->last_seq = d->pend[mi].seq;
        emit(d, d->pend[mi].yuv);
        d->pend[mi].used = 0; d->npend--;
    }
}
static void release_noreorder(H264Dec *d) {
    int i, mi = -1;
    for (i = 0; i <= d->largest; i++) if (d->pend[i].used && (mi < 0 || d->pend[i].dts < d->pend[mi].dts)) mi = i;
    if (mi < 0 || !d->pend[mi].dts) return;
    d->last_poc = d->pend[mi].poc; d->last_seq = d->pend[mi].seq;
    emit(d, d->pend[mi].yuv);
    d->pend[mi].used = 0; d->npend--;
}
static void output_picture(H264Dec *d, int poc, int is_b) {
    int i; size_t n = (size_t)d->w * d->h * 3 / 2;
    if (d->s->profile == 66 || d->s->profile == 83) { emit(d, d->cur->plane[0]); return; }
    if (is_b && (d->seq == d->last_seq ? (d->last_poc != POC_NONE && poc <= d->last_poc + 2) : (d->seq - d->last_seq == 1 && poc == 0))) {
        d->last_poc = poc; d->last_seq = d->seq;
        emit(d, d->cur->plane[0]);
        return;
    }
    for (i = 0; i < 16; i++) if (!d->pend[i].used) break;
    if (i == 16) { emit(d, d->cur->plane[0]); return; }
    if (!d->pend[i].yuv) d->pend[i].yuv = (u8*)malloc(n);
    if (!d->pend[i].yuv) return;
    memcpy(d->pend[i].yuv, d->cur->plane[0], n);
    d->pend[i].used = 1; d->pend[i].poc = poc; d->pend[i].seq = d->seq; d->pend[i].dts = d->dts;
    if (is_b) d->has_b = 1;
    d->npend++;
    if (i > d->largest) d->largest = i;
    if (!d->has_b && d->npend > 1) release_noreorder(d);
    else release_reorder(d, poc, d->seq);
}

/* ---------------------------------------------------------------- */
/* pictures and slices                                              */

static int start_picture(H264Dec *d, const SliceHdr *h) {
    const PPS *p = &d->pps[h->pps_id]; const SPS *s = &d->sps[p->sps_id]; int i, rpoc;
    if (d->s != s || d->mbw != s->mb_w || d->mbh != s->mb_h) {
        if (d->mbw != s->mb_w || d->mbh != s->mb_h) {
            for (i = 0; i < H264_DPB; i++) pic_free(&d->dpb[i]);
            free(d->mbs);
            d->mbw = s->mb_w; d->mbh = s->mb_h; d->w = d->mbw * 16; d->h = d->mbh * 16;
            d->mbs = (MB*)calloc((size_t)d->mbw * d->mbh, sizeof(MB));
            if (!d->mbs) { d->mbw = d->mbh = 0; return 0; }
        }
        d->s = s;
        d->cropx = s->crop_l * 2; d->cropy = s->crop_t * 2;
        d->outw = d->w - 2 * (s->crop_l + s->crop_r); d->outh = d->h - 2 * (s->crop_t + s->crop_b);
        if (d->outw <= 0 || d->outh <= 0 || d->cropx >= d->w || d->cropy >= d->h) { d->outw = d->w; d->outh = d->h; d->cropx = d->cropy = 0; }
    }
    d->p = p;
    level_scale_init(p);
    d->cur = NULL;
    for (i = 0; i < H264_DPB; i++) if (!d->dpb[i].ref && !d->dpb[i].busy) { d->cur = &d->dpb[i]; break; }
    if (!d->cur) {   /* no room: drop the oldest short-term reference */
        H264Pic *old = NULL;
        for (i = 0; i < H264_DPB; i++) if (d->dpb[i].ref == 1 && (!old || d->dpb[i].frame_num_wrap < old->frame_num_wrap)) old = &d->dpb[i];
        if (!old) return 0;
        old->ref = 0; d->cur = old;
    }
    if (!pic_alloc(d, d->cur)) return 0;
    if (h->idr) d->seq++;
    d->cur->busy = 1; d->cur->ref = 0; d->cur->mmco5 = 0;
    d->cur->frame_num = h->frame_num;
    d->first = *h;
    d->sh = *h;
    d->cur->poc = compute_poc(d, h, &rpoc);
    d->reorder_poc = s->poc_type == 0 ? rpoc : 0;
    memset(d->mbs, 0, (size_t)d->mbw * d->mbh * sizeof(MB));
    /* concealment base: grey */
    memset(d->cur->plane[0], 0, (size_t)d->w * d->h);
    memset(d->cur->plane[1], 128, (size_t)d->w * d->h / 2);
    for (i = 0; i < d->mbw * d->mbh; i++) d->cur->intra[i] = 1;
    memset(d->cur->ref_idx, -1, (size_t)d->mbw * d->mbh * 16 * sizeof(*d->cur->ref_idx));
    d->nslices = 0; d->mbs_done = 0;
    d->have_pic = 1;
    return 1;
}

static void finish_picture(H264Dec *d) {
    int x, y, is_b = d->first.type == SL_B;
    if (!d->have_pic) return;
    d->have_pic = 0;
    for (y = 0; y < d->mbh; y++) for (x = 0; x < d->mbw; x++) deblock_mb(d, x, y);
    mark_refs(d);
    if (d->first.nal_ref_idc) {
        if (d->cur->mmco5) { d->prev_poc_msb = 0; d->prev_poc_lsb = 0; }
    }
    d->prev_frame_num = d->cur->mmco5 ? 0 : d->first.frame_num;
    d->prev_frame_num_offset = d->cur->mmco5 ? 0 : d->frame_num_offset;
    d->cur->busy = 0;
    if (d->mbs_done >= d->mbw * d->mbh) output_picture(d, d->reorder_poc, is_b);
}

static int new_picture(const SliceHdr *a, const SliceHdr *b) {
    return a->frame_num != b->frame_num || a->pps_id != b->pps_id || (a->nal_ref_idc == 0) != (b->nal_ref_idc == 0)
        || a->poc_lsb != b->poc_lsb || a->delta_poc_bottom != b->delta_poc_bottom || a->delta_poc[0] != b->delta_poc[0]
        || a->delta_poc[1] != b->delta_poc[1] || a->idr != b->idr || (a->idr && a->idr_pic_id != b->idr_pic_id) || b->first_mb == 0;
}

static void store_motion(H264Dec *d, MB *m) {
    int mb = d->mbaddr, blk, l, b8;
    H264Pic *cur = d->cur;
    cur->intra[mb] = (u8)IS_INTRA(m->type);
    for (l = 0; l < 2; l++) for (b8 = 0; b8 < 4; b8++) {
        int r = IS_INTRA(m->type) ? -1 : m->ref[l][b8];
        H264Pic *rp = r >= 0 ? d->list[l][r] : NULL;
        if (r >= 0 && !rp) rp = d->list[l][0];
        m->refpic[l][b8] = rp;
        cur->ref_poc[mb][l][b8] = rp ? rp->poc : 0;
        cur->ref_lt[mb][l][b8] = (u8)(rp && rp->ref == 2);
    }
    for (blk = 0; blk < 16; blk++) {
        b8 = (blk >> 3) * 2 + ((blk & 3) >> 1);
        for (l = 0; l < 2; l++) {
            cur->ref_idx[mb * 16 + blk][l] = (s8)(IS_INTRA(m->type) ? -1 : m->ref[l][b8]);
            cur->mv[mb * 16 + blk][l][0] = m->mv[l][blk][0]; cur->mv[mb * 16 + blk][l][1] = m->mv[l][blk][1];
        }
    }
}

/* a macroblock of the slice: cleared, slice parameters set */
static MB *begin_mb(H264Dec *d, const SliceHdr *h) {
    MB *m; int i;
    d->mbx = d->mbaddr % d->mbw; d->mby = d->mbaddr / d->mbw;
    m = &d->mbs[d->mbaddr];
    memset(m, 0, sizeof(*m));
    m->slice = d->nslices;
    m->deblock_idc = (s8)h->disable_deblock; m->alpha_off = (s8)h->alpha_off; m->beta_off = (s8)h->beta_off;
    for (i = 0; i < 16; i++) m->ipred[i] = -1;
    memset(m->ref, -1, sizeof(m->ref));
    return m;
}
/* skipped-MB motion, qp, motion store and reconstruction */
static void end_mb(H264Dec *d, MB *m, MBData *md) {
    int i;
    if (md->type == MB_PSKIP) {
        int mv[2];
        pskip_mv(d, mv);
        memset(m->ref[0], 0, 4);
        set_mv(m, 0, 0xffff, mv);
        d->last_dqp = 0;
    } else if (md->type == MB_BSKIP) {
        for (i = 0; i < 4; i++) m->direct8[i] = 1;
        direct_pred(d, m, 0xffff);
        d->last_dqp = 0;
    }
    m->qp = (u8)d->qp;
    m->qpc[0] = (u8)chroma_qp(d->p, d->qp, 0); m->qpc[1] = (u8)chroma_qp(d->p, d->qp, 1);
    store_motion(d, m);
    recon_mb(d, m, md);
    d->mbs_done++;
}

static void decode_slice(H264Dec *d, Bits *b, int nal_type, int nal_ref_idc) {
    SliceHdr h; int total;
    if (!parse_slice_header(d, b, &h, nal_type, nal_ref_idc)) return;
    if (!d->have_pic || new_picture(&d->first, &h)) {
        finish_picture(d);
        if (!start_picture(d, &h)) return;
    }
    d->sh = h;
    d->p = &d->pps[h.pps_id];
    d->nslices++;
    total = d->mbw * d->mbh;
    if (h.first_mb >= total) return;
    update_pic_nums(d, h.frame_num);
    if (h.type != SL_I) init_ref_lists(d);
    else { memset(d->list, 0, sizeof(d->list)); }
    init_weights(d);
    d->qp = h.qp; d->last_dqp = 0;
    d->mbaddr = h.first_mb;
    if (d->p->cabac) {   /* cabac_alignment_one_bit, then the arithmetic decoder */
        b->pos = (b->pos + 7) & ~7u;
        d->bits = *b;
        cabac_init_ctx(d, h.type, h.cabac_init_idc, h.qp);
        cabac_init_engine(d);
    } else d->bits = *b;
    for (;;) {
        MB *m; MBData md; int pcm, i, skip = 0;
        if (d->bits.pos > d->bits.end + 64) break;
        if (!d->p->cabac && h.type != SL_I) {
            int run = (int)rue(&d->bits);
            while (run-- > 0 && d->mbaddr < total) {
                begin_mb(d, &h);
                memset(&md, 0, sizeof(md));
                md.type = h.type == SL_P ? MB_PSKIP : MB_BSKIP;
                d->mbs[d->mbaddr].type = (u8)md.type;
                end_mb(d, &d->mbs[d->mbaddr], &md);
                d->mbaddr++;
                skip = 1;
            }
            if (d->mbaddr >= total) break;
            if (skip && !more_rbsp(&d->bits)) break;
        }
        m = begin_mb(d, &h);
        decode_mb(d, m, &md, &pcm);
        if (pcm) {
            Bits *bb = &d->bits;
            bb->pos = (bb->pos + 7) & ~7u;
            for (i = 0; i < 384; i++) md.pcm[i] = (u8)rbn(bb, 8);
            if (d->p->cabac) cabac_init_engine(d);
            m->cbp = 0x2f; memset(m->nz, 16, 16); memset(m->nzc, 16, sizeof(m->nzc));
            d->last_dqp = 0;
        }
        end_mb(d, m, &md);
        if (d->p->cabac) { if (cabac_term(d)) break; }
        else if (!more_rbsp(&d->bits)) break;
        if (++d->mbaddr >= total) break;
    }
}

/* ---------------------------------------------------------------- */
/* NAL units and the decoder interface                              */

static void decode_nal(H264Dec *d, const u8 *nal, size_t len) {
    size_t i, n = 0; int zeros = 0, type, ref_idc; Bits b;
    if (len < 1) return;
    if (d->rbsp_cap < len) {
        u8 *nb = (u8*)realloc(d->rbsp, len + 16);
        if (!nb) return;
        d->rbsp = nb; d->rbsp_cap = len + 16;
    }
    for (i = 1; i < len; i++) {   /* drop emulation prevention bytes */
        if (zeros >= 2 && nal[i] == 3) { zeros = 0; continue; }
        d->rbsp[n++] = nal[i];
        zeros = nal[i] == 0 ? zeros + 1 : 0;
    }
    type = nal[0] & 31; ref_idc = (nal[0] >> 5) & 3;
    b.p = d->rbsp; b.len = (u32)n; b.pos = 0; b.end = (u32)n * 8;
    switch (type) {
    case 7: parse_sps(d->sps, &b); break;
    case 8: parse_pps(d->pps, d->sps, &b); break;
    case 1: case 5: decode_slice(d, &b, type, ref_idc); break;
    default: break;
    }
}

H264Dec *h264_create(void) {
    H264Dec *d = (H264Dec*)calloc(1, sizeof(H264Dec));
    int i;
    if (!d) return NULL;
    d->last_poc = POC_NONE;
    for (i = 0; i < 16; i++) d->pend[i].poc = POC_NONE;
    d->max_long_term_idx = -1;
    return d;
}
void h264_free(H264Dec *d) {
    int i;
    if (!d) return;
    for (i = 0; i < H264_DPB; i++) pic_free(&d->dpb[i]);
    for (i = 0; i < 16; i++) free(d->pend[i].yuv);
    free(d->mbs); free(d->rbsp);
    if (d->img) { free(d->img->px); free(d->img); }
    free(d);
}

/* an FLV AVC packet (AVCPacketType, composition time, data): 1 when a picture comes out */
int h264_decode(H264Dec *d, const u8 *data, size_t len, Bitmap **out) {
    size_t i;
    if (!d || len < 4) return 0;
    d->out_ready = 0;
    if (data[0] == 0) {   /* AVCDecoderConfigurationRecord */
        const u8 *c = data + 4; size_t n = len - 4, pos; int ns, np, k;
        if (n < 7 || c[0] != 1) return 0;
        d->dts++;
        d->length_size = (c[4] & 3) + 1;
        ns = c[5] & 31; pos = 6;
        for (k = 0; k < ns && pos + 2 <= n; k++) {
            size_t l = ((size_t)c[pos] << 8) | c[pos + 1]; pos += 2;
            if (pos + l > n) break;
            decode_nal(d, c + pos, l); pos += l;
        }
        if (pos < n) {
            np = c[pos++];
            for (k = 0; k < np && pos + 2 <= n; k++) {
                size_t l = ((size_t)c[pos] << 8) | c[pos + 1]; pos += 2;
                if (pos + l > n) break;
                decode_nal(d, c + pos, l); pos += l;
            }
        }
        d->configured = 1;
        return 0;
    }
    if (data[0] != 1 || !d->configured) return 0;
    d->dts++;
    i = 4;
    while (i + (size_t)d->length_size <= len) {
        size_t l = 0; int k;
        for (k = 0; k < d->length_size; k++) l = (l << 8) | data[i + k];
        i += (size_t)d->length_size;
        if (l > len - i) break;
        decode_nal(d, data + i, l);
        i += l;
    }
    /* the access unit ends with the packet (DecodeFrameNoDelay) */
    finish_picture(d);
    if (d->out_ready && d->img) { *out = d->img; return 1; }
    return 0;
}
