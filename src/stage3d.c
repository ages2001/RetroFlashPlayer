/* stage3d.c - software Stage3D: Context3D state, AGAL parser/validator and
 * CPU interpreter, triangle rasterizer (see stage3d.h).
 *
 * The pipeline mirrors the GPU one Ruffle builds with wgpu:
 *  - AGAL programs are validated as naga-agal does (same error ids) and then
 *    interpreted: one lane per vertex, four lanes per 2x2 fragment quad (so
 *    ddx/ddy work and helper lanes exist like on a GPU);
 *  - clip space is D3D style (0 <= z <= w), y up, front faces are the ones
 *    that appear clockwise on screen;
 *  - triangles are clipped against the view volume, rasterized with the
 *    top-left rule on a 1/256 pixel grid, with optional 2x/4x multisampling
 *    at the standard sample positions (shading once per pixel, at its
 *    centre) and perspective-correct varyings;
 *  - per sample: stencil test, depth test, blending, colour mask, into an
 *    RGBA8 colour buffer (float depth, 8-bit stencil). */
#include "stage3d.h"
#include "jxr.h"
#include "inflate.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* ---------------------------------------------------------------- */
/* resources                                                        */

S3DVBuf *s3d_vbuf_new(u32 nverts, u32 d32) {
    S3DVBuf *b = (S3DVBuf*)calloc(1, sizeof(S3DVBuf)); size_t n = (size_t)nverts * d32 * 4;
    if (!b) return NULL;
    b->refs = 1; b->nverts = nverts; b->d32 = d32;
    b->data = (u8*)calloc(n ? n : 1, 1);
    if (!b->data) { free(b); return NULL; }
    return b;
}
void s3d_vbuf_unref(S3DVBuf *b) { if (b && --b->refs <= 0) { free(b->data); free(b); } }

S3DIBuf *s3d_ibuf_new(u32 n) {
    S3DIBuf *b = (S3DIBuf*)calloc(1, sizeof(S3DIBuf));
    if (!b) return NULL;
    b->refs = 1; b->n = n; b->data = (u16*)calloc(n ? n : 1, sizeof(u16));
    if (!b->data) { free(b); return NULL; }
    return b;
}
void s3d_ibuf_unref(S3DIBuf *b) { if (b && --b->refs <= 0) { free(b->data); free(b); } }

S3DTex *s3d_tex_new(int kind, int w, int h, int format) {
    S3DTex *t = (S3DTex*)calloc(1, sizeof(S3DTex)); int i;
    if (!t) return NULL;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > 8192) w = 8192;
    if (h > 8192) h = 8192;
    t->refs = 1; t->kind = kind; t->w = w; t->h = h; t->format = format; t->nfaces = kind == S3D_TEX_CUBE ? 6 : 1;
    for (i = 0; i < t->nfaces; i++) {
        t->px[i] = (u8*)calloc((size_t)w * h, 4);
        if (!t->px[i]) { while (--i >= 0) free(t->px[i]); free(t); return NULL; }
    }
    return t;
}
void s3d_tex_unref(S3DTex *t) { int i; if (t && --t->refs <= 0) { for (i = 0; i < t->nfaces; i++) free(t->px[i]); free(t); } }

S3DProg *s3d_prog_new(void) { S3DProg *p = (S3DProg*)calloc(1, sizeof(S3DProg)); if (p) p->refs = 1; return p; }
static void prog_clear(S3DProg *p) { free(p->vp.ins); free(p->fp.ins); memset(&p->vp, 0, sizeof(p->vp)); memset(&p->fp, 0, sizeof(p->fp)); p->ok = 0; }
void s3d_prog_unref(S3DProg *p) { if (p && --p->refs <= 0) { prog_clear(p); free(p); } }

/* ---------------------------------------------------------------- */
/* AGAL parsing (naga-agal builder.rs parse_bytecode)               */

enum { RT_ATTR = 0, RT_CONST, RT_TEMP, RT_OUT, RT_VARY, RT_SAMPLER, RT_FRAG };
enum {
    OP_MOV = 0x00, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_RCP, OP_MIN, OP_MAX, OP_FRC, OP_SQT, OP_RSQ, OP_POW, OP_LOG, OP_EXP,
    OP_NRM, OP_SIN, OP_COS, OP_CRS, OP_DP3, OP_DP4, OP_ABS, OP_NEG, OP_SAT, OP_M33, OP_M44, OP_M34,
    OP_DDX = 0x1a, OP_DDY, OP_IFE, OP_INE, OP_IFG, OP_IFL, OP_ELS, OP_EIF,
    OP_KIL = 0x27, OP_TEX, OP_SGE, OP_SLT, OP_SEQ = 0x2c, OP_SNE
};
static int op_valid(u32 op) { return op <= 0x21 || op == 0x27 || op == 0x28 || op == 0x29 || op == 0x2a || op == 0x2c || op == 0x2d; }

static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

static void parse_src(const u8 *p, AgalSrc *s) {
    u32 lo = rd32(p), hi = rd32(p + 4); int i;
    s->reg = (u16)(lo & 0xFFFF); s->off = (u8)((lo >> 16) & 0xFF);
    for (i = 0; i < 4; i++) s->swz[i] = (u8)((lo >> (24 + 2 * i)) & 3);
    s->type = (u8)(hi & 0xF); s->itype = (u8)((hi >> 8) & 0xF); s->isel = (u8)((hi >> 16) & 3); s->indirect = (u8)((hi >> 31) & 1);
}

static void set_err(AgalErr *e, int id, int argerr, const char *shader, const char *a2, int token) {
    e->id = id; e->argerr = argerr; e->shader = shader; e->token = token;
    e->a2[0] = 0; if (a2) { strncpy(e->a2, a2, sizeof(e->a2) - 1); e->a2[sizeof(e->a2) - 1] = 0; }
}

/* the interpreter keeps a lane's registers in one array: temporaries,
   inputs (vertex attributes / fragment varyings), varyings written by a
   vertex program, the output, and an always-zero register */
#define R_IN   S3D_MAX_TEMP
#define R_VOUT (R_IN + S3D_MAX_VARY)
#define R_O    (R_VOUT + S3D_MAX_VARY)
#define R_ZERO (R_O + 1)
#define NREG   (R_ZERO + 1)
static int reg_slot(int type, int reg, int fragment, int dest) {
    switch (type) {
    case RT_TEMP: return reg < S3D_MAX_TEMP ? reg : (dest ? -1 : R_ZERO);
    case RT_ATTR: return (!fragment && !dest && reg < S3D_MAX_ATTR) ? R_IN + reg : (dest ? -1 : R_ZERO);
    case RT_VARY: if (reg >= S3D_MAX_VARY) return dest ? -1 : R_ZERO; return (fragment && !dest) ? R_IN + reg : (fragment ? R_IN + reg : R_VOUT + reg);
    case RT_OUT: return dest ? R_O : R_ZERO;
    default: return dest ? -1 : R_ZERO;
    }
}
static void set_slots(AgalSrc *s, int fragment) {
    int r;
    s->isconst = s->type == RT_CONST;
    r = reg_slot(s->type, s->reg, fragment, 0); s->slot = (u8)r;
    r = reg_slot(s->itype, s->reg, fragment, 0); s->islot = (u8)r;
    s->ident = s->swz[0] == 0 && s->swz[1] == 1 && s->swz[2] == 2 && s->swz[3] == 3;
}

static int agal_parse(const u8 *b, u32 len, AgalProg *out, AgalErr *err) {
    u32 ver, pos; int type, token = 0, cap = 0; const char *sn; AgalIns *ins = NULL; int n = 0;
    memset(out, 0, sizeof(*out));
    if (len == 0) { set_err(err, 3615, 1, "", NULL, 0); return 0; }
    if (len < 7) { set_err(err, 3612, 1, "", NULL, 0); return 0; }
    if (b[0] != 0xa0) { set_err(err, 3612, 1, "", NULL, 0); return 0; }
    ver = rd32(b + 1);
    if (ver != 1 && ver != 2) { set_err(err, 3615, 0, "fragment", NULL, 0); return 0; }
    if (b[5] != 0xa1) { set_err(err, 3612, 1, "", NULL, 0); return 0; }
    if (b[6] > 1) { set_err(err, 3615, 0, "fragment", NULL, 0); return 0; }
    type = b[6]; sn = type ? "fragment" : "vertex";
    pos = 7;
    while (pos < len) {
        const u8 *t = b + pos; AgalIns in; u32 op, dest; int has_dest, has_src, k, nsrc;
        if (len - pos < 24) { free(ins); set_err(err, 3612, 1, "", NULL, 0); return 0; }
        pos += 24; token++;
        op = rd32(t);
        if (!op_valid(op)) { char v[24]; sprintf(v, "%lu", (unsigned long)op); free(ins); set_err(err, 3620, 0, sn, v, token); return 0; }
        memset(&in, 0, sizeof(in));
        in.op = (u8)op;
        dest = rd32(t + 4);
        in.dreg = (u16)(dest & 0xFFFF); in.dmask = (u8)((dest >> 16) & 0xF); in.dtype = (u8)((dest >> 24) & 0xF);
        parse_src(t + 8, &in.a);
        if (op == OP_TEX) {
            u32 lo = rd32(t + 16), hi = rd32(t + 20);
            in.sreg = (u8)(lo & 0xFF);
            in.sdim = (u8)((hi >> 12) & 0xF); in.signore = (u8)(((hi >> 16) & 0xF) & 4 ? 1 : 0);
            in.swrap = (u8)((hi >> 20) & 0xF); in.smip = (u8)((hi >> 24) & 0xF); in.sfilter = (u8)((hi >> 28) & 0xF);
            if (in.sreg >= S3D_MAX_SAMP) in.sreg = S3D_MAX_SAMP - 1;
            if (in.swrap > 3) in.swrap = 0; if (in.smip > 2) in.smip = 0; if (in.sfilter > 5) in.sfilter = 1; if (in.sdim > 1) in.sdim = 0;
        } else parse_src(t + 16, &in.b);
        has_dest = !(op == OP_KIL || (op >= OP_IFE && op <= OP_EIF));
        if (has_dest) {
            int id = 0;
            switch (in.dtype) {
            case RT_OUT: case RT_VARY: case RT_TEMP: break;
            case RT_CONST: id = 3652; break;
            case RT_ATTR: id = 3651; break;
            case RT_SAMPLER: id = 3649; break;
            case RT_FRAG: id = 3749; break;
            default: id = 3655; break;
            }
            if (id) { free(ins); set_err(err, id, 0, sn, "destination operand", token); return 0; }
        }
        has_src = !(op == OP_ELS || op == OP_EIF);
        nsrc = (op == OP_TEX || op == OP_KIL) ? 1 : 2;
        for (k = 0; has_src && k < nsrc; k++) {
            const AgalSrc *s = k ? &in.b : &in.a; int id = 0; char a2[24];
            sprintf(a2, "source operand %d", k + 1);
            switch (s->type) {
            case RT_ATTR: case RT_CONST: case RT_TEMP: case RT_VARY: break;
            case RT_OUT: id = 3646; break;
            case RT_SAMPLER: id = 3638; break;
            case RT_FRAG: id = 3749; break;
            default: id = 3655; break;
            }
            if (!id && s->indirect) {
                if (type == 1) id = 3639;
                else if (s->type != RT_CONST) id = 3640;
            }
            if (id) { free(ins); set_err(err, id, 0, sn, a2, token); return 0; }
        }
        if (n == cap) { AgalIns *ni; cap = cap ? cap * 2 : 16; ni = (AgalIns*)realloc(ins, (size_t)cap * sizeof(AgalIns)); if (!ni) { free(ins); set_err(err, 3612, 1, "", NULL, 0); return 0; } ins = ni; }
        ins[n++] = in;
    }
    out->type = type; out->n = n; out->ins = ins;
    {   /* interpreter registers */
        int i;
        for (i = 0; i < n; i++) {
            AgalIns *in = &ins[i];
            set_slots(&in->a, type); set_slots(&in->b, type);
            in->dslot = (u8)reg_slot(in->dtype, in->dreg, type, 1);
            in->single = in->dmask == 1 || in->dmask == 2 || in->dmask == 4 || in->dmask == 8;
        }
    }
    /* register usage */
    {
        int i, k;
        for (i = 0; i < n; i++) {
            const AgalIns *in = &ins[i]; int nsrc = (in->op == OP_TEX || in->op == OP_KIL) ? 1 : 2;
            if (in->op == OP_ELS || in->op == OP_EIF) continue;
            if (in->op == OP_DDX || in->op == OP_DDY) out->has_deriv = 1;
            if (in->op == OP_TEX && !in->signore) out->samp_used |= (u8)(1 << in->sreg);
            for (k = 0; k < nsrc; k++) {
                const AgalSrc *s = k ? &in->b : &in->a;
                if (in->op >= OP_M33 && in->op <= OP_M34 && k == 1) {
                    if (!s->indirect && s->type == RT_VARY) { int r; for (r = 0; r < 4; r++) if (s->reg + r < S3D_MAX_VARY) out->vary_used |= (u16)(1 << (s->reg + r)); }
                    if (!s->indirect && s->type == RT_ATTR) { int r; for (r = 0; r < 4; r++) if (s->reg + r < S3D_MAX_ATTR) out->va_used |= (u8)(1 << (s->reg + r)); }
                }
                if (s->indirect) { if (s->itype == RT_ATTR && s->reg < S3D_MAX_ATTR) out->va_used |= (u8)(1 << s->reg); if (s->itype == RT_VARY && s->reg < S3D_MAX_VARY) out->vary_used |= (u16)(1 << s->reg); continue; }
                if (s->type == RT_ATTR && s->reg < S3D_MAX_ATTR) out->va_used |= (u8)(1 << s->reg);
                if (s->type == RT_VARY && s->reg < S3D_MAX_VARY) out->vary_used |= (u16)(1 << s->reg);
            }
        }
    }
    return 1;
}

/* naga-agal extract_sampler_configs */
static int sampler_configs(const AgalProg *p, int *has, S3DSampler *cfg, AgalErr *err) {
    int i;
    for (i = 0; i < S3D_MAX_SAMP; i++) has[i] = 0;
    for (i = 0; i < p->n; i++) {
        const AgalIns *in = &p->ins[i]; S3DSampler s;
        if (in->op != OP_TEX || in->signore) continue;
        s.wrap = in->swrap; s.filter = in->sfilter; s.mip = in->smip;
        if (!has[in->sreg]) { has[in->sreg] = 1; cfg[in->sreg] = s; }
        else if (cfg[in->sreg].wrap != s.wrap || cfg[in->sreg].filter != s.filter || cfg[in->sreg].mip != s.mip) {
            set_err(err, 3696, 0, p->type ? "fragment" : "vertex", "", i + 1); return 0;
        }
    }
    return 1;
}

int s3d_prog_upload(S3DProg *p, const u8 *vp, u32 vlen, const u8 *fp, u32 flen, AgalErr *err) {
    AgalProg v, f; int has[S3D_MAX_SAMP]; S3DSampler cfg[S3D_MAX_SAMP];
    memset(err, 0, sizeof(*err));
    if (!agal_parse(vp, vlen, &v, err)) return 0;
    if (!agal_parse(fp, flen, &f, err)) { free(v.ins); return 0; }
    memset(cfg, 0, sizeof(cfg));
    if (!sampler_configs(&f, has, cfg, err)) { free(v.ins); free(f.ins); return 0; }
    prog_clear(p);
    p->vp = v; p->fp = f; p->ok = 1;
    memcpy(p->has_samp, has, sizeof(has)); memcpy(p->samp, cfg, sizeof(cfg));
    return 1;
}

/* ---------------------------------------------------------------- */
/* texture sampling                                                 */

static int wrap_coord(int i, int n, int repeat) {
    if (repeat) { i %= n; if (i < 0) i += n; return i; }
    return i < 0 ? 0 : i >= n ? n - 1 : i;
}
static void texel(const u8 *px, int w, int x, int y, float *o) {
    const u8 *p = px + ((size_t)y * w + x) * 4;
    o[0] = p[0] / 255.0f; o[1] = p[1] / 255.0f; o[2] = p[2] / 255.0f; o[3] = p[3] / 255.0f;
}
static void sample_face(const u8 *px, int w, int h, double u, double v, int rep_u, int rep_v, int linear, float *out) {
    if (!linear) {
        int x = (int)floor(u * w), y = (int)floor(v * h);
        texel(px, w, wrap_coord(x, w, rep_u), wrap_coord(y, h, rep_v), out);
    } else {
        double fx = u * w - 0.5, fy = v * h - 0.5, ax, ay; int x0 = (int)floor(fx), y0 = (int)floor(fy), k;
        float t00[4], t10[4], t01[4], t11[4];
        ax = fx - x0; ay = fy - y0;
        texel(px, w, wrap_coord(x0, w, rep_u), wrap_coord(y0, h, rep_v), t00);
        texel(px, w, wrap_coord(x0 + 1, w, rep_u), wrap_coord(y0, h, rep_v), t10);
        texel(px, w, wrap_coord(x0, w, rep_u), wrap_coord(y0 + 1, h, rep_v), t01);
        texel(px, w, wrap_coord(x0 + 1, w, rep_u), wrap_coord(y0 + 1, h, rep_v), t11);
        for (k = 0; k < 4; k++) {
            double a = t00[k] + (t10[k] - t00[k]) * ax, b = t01[k] + (t11[k] - t01[k]) * ax;
            out[k] = (float)(a + (b - a) * ay);
        }
    }
}
static void tex_sample(const S3DState *c, int unit, int dim, const float *co, float *out) {
    S3DTex *t = c->tex[unit]; const S3DSampler *s = &c->samp[unit]; int linear, ru, rv;
    out[0] = out[1] = out[2] = 0; out[3] = 0;
    if (!t) return;
    linear = s->filter != S3D_FILTER_NEAREST;
    ru = s->wrap == S3D_WRAP_REPEAT || s->wrap == S3D_WRAP_REPEAT_U_CLAMP_V;
    rv = s->wrap == S3D_WRAP_REPEAT || s->wrap == S3D_WRAP_CLAMP_U_REPEAT_V;
    if (t->kind == S3D_TEX_CUBE && (dim == 1 || t->nfaces == 6)) {
        double x = co[0], y = co[1], z = co[2], ax = fabs(x), ay = fabs(y), az = fabs(z), sc, tc, ma; int face;
        if (dim != 1) { sample_face(t->px[0], t->w, t->h, co[0], co[1], ru, rv, linear, out); return; }
        if (ax >= ay && ax >= az) { ma = ax; if (x >= 0) { face = 0; sc = -z; tc = -y; } else { face = 1; sc = z; tc = -y; } }
        else if (ay >= az) { ma = ay; if (y >= 0) { face = 2; sc = x; tc = z; } else { face = 3; sc = x; tc = -z; } }
        else { ma = az; if (z >= 0) { face = 4; sc = x; tc = -y; } else { face = 5; sc = -x; tc = -y; } }
        if (ma == 0) ma = 1;
        sample_face(t->px[face], t->w, t->h, (sc / ma + 1) * 0.5, (tc / ma + 1) * 0.5, 0, 0, linear, out);
        return;
    }
    sample_face(t->px[0], t->w, t->h, co[0], co[1], ru, rv, linear, out);
}

/* ---------------------------------------------------------------- */
/* AGAL interpreter                                                 */

typedef struct {
    int nl;
    float r[4][NREG][4];             /* per lane, see R_* */
    const float *c; int nc;
    const int *va_fmt;
    const S3DState *st;
    int killed;
    int lanes;                       /* lanes to run (bit mask) */
} AgalVM;

static const float zero4[4] = { 0, 0, 0, 0 };

/* load a source operand (reg_add selects the following registers for m33/m44) */
static void load_src(AgalVM *m, int fragment, const AgalSrc *s, int reg_add, int l, float *out) {
    const float *p;
    if (s->indirect) {
        const float *ib = m->r[l][s->islot]; double v; long idx;
        v = (s->itype == RT_ATTR && s->reg < S3D_MAX_ATTR && m->va_fmt && m->va_fmt[s->reg] == S3D_VF_FLOAT1) ? ib[0] : ib[s->isel];
        if (v != v || v < 0) idx = 0; else if (v > 1e9) idx = 1000000000L; else idx = (long)v;
        idx += s->off + reg_add;
        p = (s->type == RT_CONST && idx < m->nc) ? m->c + idx * 4 : zero4;
    } else if (s->isconst) {
        int r = s->reg + reg_add;
        p = r < m->nc ? m->c + r * 4 : zero4;
    } else if (!reg_add) p = m->r[l][s->slot];
    else { int sl = reg_slot(s->type, s->reg + reg_add, fragment, 0); p = m->r[l][sl]; }
    if (s->ident) { out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3]; }
    else { out[0] = p[s->swz[0]]; out[1] = p[s->swz[1]]; out[2] = p[s->swz[2]]; out[3] = p[s->swz[3]]; }
}

static void store_dst(AgalVM *m, const AgalIns *in, int l, const float *r) {
    float *d; int mask = in->dmask;
    if (in->dslot >= R_ZERO) return;
    d = m->r[l][in->dslot];
    if (in->single) { int k = mask == 1 ? 0 : mask == 2 ? 1 : mask == 4 ? 2 : 3; d[k] = r[0]; return; }
    if (mask & 1) d[0] = r[0];
    if (mask & 2) d[1] = r[1];
    if (mask & 4) d[2] = r[2];
    if (mask & 8) d[3] = r[3];
}

static float fl(double d) { return (float)d; }

static void agal_exec(AgalVM *m, const AgalProg *p) {
    int fragment = p->type == 1, i, l, k;
    int exec = m->lanes;
    int sstack[32][2]; int sp = 0;   /* saved exec mask, condition mask */
    for (i = 0; i < p->n; i++) {
        const AgalIns *in = &p->ins[i];
        float a[4][4], b[4][4], r[4];
        switch (in->op) {
        case OP_IFE: case OP_INE: case OP_IFG: case OP_IFL: {
            int cond = 0;
            for (l = 0; l < m->nl; l++) {
                int ok = 1;
                load_src(m, fragment, &in->a, 0, l, a[l]); load_src(m, fragment, &in->b, 0, l, b[l]);
                for (k = 0; k < 4; k++) {
                    int c = in->op == OP_IFE ? a[l][k] == b[l][k] : in->op == OP_INE ? a[l][k] != b[l][k] : in->op == OP_IFG ? a[l][k] > b[l][k] : a[l][k] < b[l][k];
                    if (!c) ok = 0;
                }
                if (ok) cond |= 1 << l;
            }
            if (sp < 32) { sstack[sp][0] = exec; sstack[sp][1] = cond; sp++; }
            exec &= cond;
            continue;
        }
        case OP_ELS: if (sp > 0) exec = sstack[sp - 1][0] & ~sstack[sp - 1][1]; continue;
        case OP_EIF: if (sp > 0) { sp--; exec = sstack[sp][0]; } continue;
        case OP_DDX: case OP_DDY:
            for (l = 0; l < m->nl; l++) load_src(m, fragment, &in->a, 0, l, a[l]);
            for (l = 0; l < m->nl; l++) {
                if (!(exec & (1 << l))) continue;
                for (k = 0; k < 4; k++) {
                    if (m->nl < 4) r[k] = 0;
                    else if (in->op == OP_DDX) r[k] = (l & 2) ? a[3][k] - a[2][k] : a[1][k] - a[0][k];
                    else r[k] = (l & 1) ? a[3][k] - a[1][k] : a[2][k] - a[0][k];
                }
                store_dst(m, in, l, r);
            }
            continue;
        default: break;
        }
        for (l = 0; l < m->nl; l++) {
            float *x = a[0], *y = b[0];
            if (!(exec & (1 << l))) continue;
            load_src(m, fragment, &in->a, 0, l, x);
            switch (in->op) {
            case OP_MOV: memcpy(r, x, sizeof(r)); break;
            case OP_ADD: load_src(m, fragment, &in->b, 0, l, y); for (k = 0; k < 4; k++) r[k] = x[k] + y[k]; break;
            case OP_SUB: load_src(m, fragment, &in->b, 0, l, y); for (k = 0; k < 4; k++) r[k] = x[k] - y[k]; break;
            case OP_MUL: load_src(m, fragment, &in->b, 0, l, y); for (k = 0; k < 4; k++) r[k] = x[k] * y[k]; break;
            case OP_DIV: load_src(m, fragment, &in->b, 0, l, y); for (k = 0; k < 4; k++) r[k] = x[k] / y[k]; break;
            case OP_RCP: for (k = 0; k < 4; k++) r[k] = 1.0f / x[k]; break;
            case OP_MIN: load_src(m, fragment, &in->b, 0, l, y); for (k = 0; k < 4; k++) r[k] = x[k] < y[k] ? x[k] : y[k]; break;
            case OP_MAX: load_src(m, fragment, &in->b, 0, l, y); for (k = 0; k < 4; k++) r[k] = x[k] > y[k] ? x[k] : y[k]; break;
            case OP_FRC: for (k = 0; k < 4; k++) r[k] = fl(x[k] - floor(x[k])); break;
            case OP_SQT: for (k = 0; k < 4; k++) r[k] = fl(sqrt(x[k])); break;
            case OP_RSQ: for (k = 0; k < 4; k++) r[k] = fl(1.0 / sqrt(x[k])); break;
            case OP_POW: load_src(m, fragment, &in->b, 0, l, y); for (k = 0; k < 4; k++) r[k] = fl(pow(x[k], y[k])); break;
            case OP_LOG: for (k = 0; k < 4; k++) r[k] = fl(x[k] > 0 ? log(x[k]) / log(2.0) : (x[k] == 0 ? -HUGE_VAL : sqrt(-1.0 + 0 * x[k]))); break;
            case OP_EXP: for (k = 0; k < 4; k++) r[k] = fl(pow(2.0, x[k])); break;
            case OP_NRM: { double len = sqrt((double)x[0] * x[0] + (double)x[1] * x[1] + (double)x[2] * x[2]); for (k = 0; k < 3; k++) r[k] = fl(x[k] / len); r[3] = 0; break; }
            case OP_SIN: for (k = 0; k < 4; k++) r[k] = fl(sin(x[k])); break;
            case OP_COS: for (k = 0; k < 4; k++) r[k] = fl(cos(x[k])); break;
            case OP_CRS: load_src(m, fragment, &in->b, 0, l, y);
                r[0] = x[1] * y[2] - x[2] * y[1]; r[1] = x[2] * y[0] - x[0] * y[2]; r[2] = x[0] * y[1] - x[1] * y[0]; r[3] = 1; break;
            case OP_DP3: load_src(m, fragment, &in->b, 0, l, y); r[0] = r[1] = r[2] = r[3] = x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; break;
            case OP_DP4: load_src(m, fragment, &in->b, 0, l, y); r[0] = r[1] = r[2] = r[3] = x[0] * y[0] + x[1] * y[1] + x[2] * y[2] + x[3] * y[3]; break;
            case OP_ABS: for (k = 0; k < 4; k++) r[k] = x[k] < 0 ? -x[k] : x[k]; break;
            case OP_NEG: for (k = 0; k < 4; k++) r[k] = -x[k]; break;
            case OP_SAT: for (k = 0; k < 4; k++) r[k] = x[k] < 0 ? 0 : x[k] > 1 ? 1 : (x[k] != x[k] ? 0 : x[k]); break;
            case OP_M33: case OP_M44: case OP_M34: {
                int rows = in->op == OP_M44 ? 4 : 3, cols = in->op == OP_M33 ? 3 : 4, j;
                for (j = 0; j < rows; j++) {
                    float row[4]; float s = 0;
                    load_src(m, fragment, &in->b, j, l, row);
                    for (k = 0; k < cols; k++) s += row[k] * x[k];
                    r[j] = (float)s;
                }
                if (rows == 3) r[3] = 1;
                break;
            }
            case OP_KIL: if (x[0] < 0) m->killed |= 1 << l; continue;
            case OP_TEX: tex_sample(m->st, in->sreg, in->sdim, x, r); break;
            case OP_SGE: load_src(m, fragment, &in->b, 0, l, y); for (k = 0; k < 4; k++) r[k] = x[k] >= y[k] ? 1.0f : 0.0f; break;
            case OP_SLT: load_src(m, fragment, &in->b, 0, l, y); for (k = 0; k < 4; k++) r[k] = x[k] < y[k] ? 1.0f : 0.0f; break;
            case OP_SEQ: load_src(m, fragment, &in->b, 0, l, y); for (k = 0; k < 4; k++) r[k] = x[k] == y[k] ? 1.0f : 0.0f; break;
            case OP_SNE: load_src(m, fragment, &in->b, 0, l, y); for (k = 0; k < 4; k++) r[k] = x[k] != y[k] ? 1.0f : 0.0f; break;
            default: continue;
            }
            store_dst(m, in, l, r);
        }
    }
}

/* ---------------------------------------------------------------- */
/* state snapshots                                                  */

static void state_init(S3DState *s) {
    int i;
    memset(s, 0, sizeof(*s));
    s->blend_src = S3D_BF_ONE; s->blend_dst = S3D_BF_ZERO;
    for (i = 0; i < 4; i++) s->cmask[i] = 1;
    s->depth_mask = 1; s->depth_cmp = S3D_CMP_LESS_EQUAL; s->cull = S3D_FACE_NONE;
    s->st_rmask = s->st_wmask = 0xFFFFFFFFu;
    for (i = 0; i < S3D_MAX_SAMP; i++) { s->samp[i].wrap = S3D_WRAP_CLAMP; s->samp[i].filter = S3D_FILTER_NEAREST; s->samp[i].mip = 0; }
}
/* drop the references a state holds (pend: also the pending counts) */
static void state_release(S3DState *s, int pend) {
    int i;
    for (i = 0; i < S3D_MAX_ATTR; i++) if (s->va_buf[i]) { if (pend) s->va_buf[i]->pending--; s3d_vbuf_unref(s->va_buf[i]); s->va_buf[i] = NULL; }
    for (i = 0; i < S3D_MAX_SAMP; i++) if (s->tex[i]) { if (pend) s->tex[i]->pending--; s3d_tex_unref(s->tex[i]); s->tex[i] = NULL; }
    if (s->prog) { if (pend) s->prog->pending--; s3d_prog_unref(s->prog); s->prog = NULL; }
}
static void state_unref(S3DState *s) { if (s && --s->refs <= 0) { state_release(s, 1); free(s); } }
static S3DState *state_snapshot(const S3DState *cur) {
    S3DState *s = (S3DState*)malloc(sizeof(S3DState)); int i;
    if (!s) return NULL;
    memcpy(s, cur, sizeof(*s));
    s->refs = 1;
    for (i = 0; i < S3D_MAX_ATTR; i++) if (s->va_buf[i]) { s->va_buf[i]->refs++; s->va_buf[i]->pending++; }
    for (i = 0; i < S3D_MAX_SAMP; i++) if (s->tex[i]) { s->tex[i]->refs++; s->tex[i]->pending++; }
    if (s->prog) { s->prog->refs++; s->prog->pending++; }
    return s;
}

/* ---------------------------------------------------------------- */
/* context                                                          */

static S3DCtx *g_ctxs;

S3DCtx *s3d_ctx_new(int profile) {
    S3DCtx *c = (S3DCtx*)calloc(1, sizeof(S3DCtx));
    if (!c) return NULL;
    c->refs = 1; c->profile = profile;
    state_init(&c->st); c->dirty = 1;
    c->next_ctx = g_ctxs; g_ctxs = c;
    return c;
}
static void surf_free_bufs(S3DSurf *s, int own_color) { if (own_color) free(s->color); free(s->depth); free(s->stencil); memset(s, 0, sizeof(*s)); }
static void cmd_release(S3DCmd *k) {
    state_unref(k->s); k->s = NULL;
    if (k->ib) { k->ib->pending--; s3d_ibuf_unref(k->ib); k->ib = NULL; }
    if (k->t) { k->t->pending--; s3d_tex_unref(k->t); k->t = NULL; }
}
static void queue_clear(S3DCtx *c) { int i; for (i = 0; i < c->nq; i++) cmd_release(&c->q[i]); c->nq = 0; }
static void exec_back(S3DCtx *c);
void s3d_ctx_unref(S3DCtx *c) {
    S3DCtx **pp;
    if (!c || --c->refs > 0) return;
    for (pp = &g_ctxs; *pp; pp = &(*pp)->next_ctx) if (*pp == c) { *pp = c->next_ctx; break; }
    queue_clear(c); free(c->q);
    exec_back(c);
    surf_free_bufs(&c->back, 1); free(c->front);
    state_release(&c->st, 0);
    state_unref(c->snap);
    free(c);
}

enum { CMD_CLEAR = 1, CMD_DRAW, CMD_PRESENT, CMD_RTT, CMD_BACK };
static void exec_draw(S3DCtx *c, const S3DState *st, S3DIBuf *ib, u32 first, s32 ntri);
static S3DSurf *cur_surf(S3DCtx *c) { return c->to_texture ? &c->rtt : &c->back; }

static void resolve(const S3DSurf *s, u8 *out);
static void exec_back(S3DCtx *c) {
    if (c->to_texture) {
        int own = c->rtt.ns > 1;
        if (own && c->rtt.color && c->rtt_tex) resolve(&c->rtt, c->rtt_tex->px[c->rtt_face]);   /* the multisampled target resolves into the texture */
        surf_free_bufs(&c->rtt, own); s3d_tex_unref(c->rtt_tex); c->rtt_tex = NULL;
    }
    c->to_texture = 0;
}
static void exec_rtt(S3DCtx *c, S3DTex *t, int depth_stencil, int face, int aa) {
    size_t np; int ns = aa >= 4 ? 4 : aa >= 2 ? 2 : 1;
    exec_back(c);
    if (!t) return;
    if (face < 0 || face >= t->nfaces) face = 0;
    t->refs++; c->rtt_tex = t; c->rtt_face = face; c->to_texture = 1;
    np = (size_t)t->w * t->h;
    c->rtt.w = t->w; c->rtt.h = t->h; c->rtt.ns = ns;
    /* antiAlias: a new multisampled target (Ruffle), else the texture itself */
    c->rtt.color = ns > 1 ? (u8*)calloc(np * ns, 4) : t->px[face];
    if (ns > 1 && !c->rtt.color) { c->rtt.ns = 1; ns = 1; c->rtt.color = t->px[face]; }
    /* a new depth/stencil buffer each time (zeroed, like a new wgpu texture) */
    if (depth_stencil) { c->rtt.depth = (float*)calloc(np * ns, sizeof(float)); c->rtt.stencil = (u8*)calloc(np * ns, 1); }
}

static u8 unorm(double v) { if (!(v > 0)) return 0; if (v >= 1) return 255; return (u8)floor(v * 255.0 + 0.5); }

static void exec_clear(S3DCtx *c, const double *cl, u32 stencil, u32 mask) {
    S3DSurf *s = cur_surf(c); size_t n, i;
    if (!s->color) return;
    n = (size_t)s->w * s->h * s->ns;
    if (mask & 1) { u8 px[4]; px[0] = unorm(cl[0]); px[1] = unorm(cl[1]); px[2] = unorm(cl[2]); px[3] = unorm(cl[3]); for (i = 0; i < n; i++) memcpy(s->color + i * 4, px, 4); }
    if ((mask & 2) && s->depth) { float d = (float)(cl[4] < 0 ? 0 : cl[4] > 1 ? 1 : cl[4]); for (i = 0; i < n; i++) s->depth[i] = d; }
    if ((mask & 4) && s->stencil) memset(s->stencil, (int)(stencil & 0xFF), n);
}

static void resolve(const S3DSurf *s, u8 *out) {
    size_t np = (size_t)s->w * s->h, i; int k, j, ns = s->ns;
    if (ns == 1) { memcpy(out, s->color, np * 4); return; }
    for (i = 0; i < np; i++) {
        const u8 *p = s->color + i * ns * 4;
        for (k = 0; k < 4; k++) { int sum = 0; for (j = 0; j < ns; j++) sum += p[j * 4 + k]; out[i * 4 + k] = (u8)((sum + ns / 2) / ns); }
    }
}
static void exec_present(S3DCtx *c) {
    exec_back(c);
    if (c->configured && c->front) resolve(&c->back, c->front);
}

/* run the queue; back buffer work for frames replaced by a later present is skipped */
void s3d_flush(S3DCtx *c) {
    int i, npres = 0, seen = 0;
    if (!c->nq) return;
    for (i = 0; i < c->nq; i++) if (c->q[i].kind == CMD_PRESENT) npres++;
    for (i = 0; i < c->nq; i++) {
        S3DCmd *k = &c->q[i]; int after = npres - seen;   /* presents at or after this command */
        switch (k->kind) {
        case CMD_CLEAR: if (!(k->back && after >= 2)) exec_clear(c, k->cl, k->st, k->mask); break;
        case CMD_DRAW: if (!(k->back && after >= 2)) exec_draw(c, k->s, k->ib, k->first, k->ntri); break;
        case CMD_PRESENT: seen++; if (after >= 2) exec_back(c); else exec_present(c); break;
        case CMD_RTT: exec_rtt(c, k->t, k->ds, k->face, k->aa); break;
        case CMD_BACK: exec_back(c); break;
        }
    }
    queue_clear(c);
}
void s3d_flush_all(void) { S3DCtx *c; for (c = g_ctxs; c; c = c->next_ctx) s3d_flush(c); }

static S3DCmd *enqueue(S3DCtx *c, int kind) {
    S3DCmd *k;
    if (c->nq >= 4096) s3d_flush(c);
    if (c->nq == c->capq) {
        int nc = c->capq ? c->capq * 2 : 64; S3DCmd *nq = (S3DCmd*)realloc(c->q, (size_t)nc * sizeof(S3DCmd));
        if (!nq) { s3d_flush(c); if (!c->capq) return NULL; }
        else { c->q = nq; c->capq = nc; }
    }
    k = &c->q[c->nq++];
    memset(k, 0, sizeof(*k));
    k->kind = kind; k->back = !c->api_rtt;
    return k;
}

void s3d_set_render_to_back_buffer(S3DCtx *c) { if (c->api_rtt) { enqueue(c, CMD_BACK); c->api_rtt = 0; } }
void s3d_set_render_to_texture(S3DCtx *c, S3DTex *t, int depth_stencil, int face, int aa) {
    S3DCmd *k = enqueue(c, CMD_RTT);
    if (!k) return;
    k->t = t; if (t) { t->refs++; t->pending++; }
    k->ds = depth_stencil; k->face = face; k->aa = aa;
    c->api_rtt = t != NULL;
}
void s3d_configure(S3DCtx *c, int w, int h, int aa, int depth_stencil) {
    int ns = aa >= 4 ? 4 : aa >= 2 ? 2 : 1; size_t np = (size_t)w * h;
    s3d_flush(c);
    exec_back(c); c->api_rtt = 0;
    surf_free_bufs(&c->back, 1); free(c->front); c->front = NULL;
    c->back.w = w; c->back.h = h; c->back.ns = ns;
    c->back.color = (u8*)calloc(np * ns, 4);
    if (depth_stencil) { c->back.depth = (float*)calloc(np * ns, sizeof(float)); c->back.stencil = (u8*)calloc(np * ns, 1); }
    c->front = (u8*)calloc(np, 4); c->fw = w; c->fh = h;
    if (!c->back.color || !c->front) { surf_free_bufs(&c->back, 1); free(c->front); c->front = NULL; c->configured = 0; return; }
    c->configured = 1; c->aa = aa; c->depth_stencil = depth_stencil;
}
void s3d_clear(S3DCtx *c, double r, double g, double b, double a, double depth, u32 stencil, u32 mask) {
    S3DCmd *k = enqueue(c, CMD_CLEAR);
    c->cleared = 1;
    if (!k) return;
    k->cl[0] = r; k->cl[1] = g; k->cl[2] = b; k->cl[3] = a; k->cl[4] = depth; k->st = stencil; k->mask = mask;
}
void s3d_present(S3DCtx *c) {
    int i, last = -1, n = 0;
    enqueue(c, CMD_PRESENT);
    c->api_rtt = 0;
    c->cleared = 0;
    /* the frame of an earlier, never shown present is dead: drop its back buffer work */
    for (i = 0; i < c->nq - 1; i++) if (c->q[i].kind == CMD_PRESENT) last = i;
    if (last < 0) return;
    for (i = 0; i < c->nq; i++) {
        S3DCmd *k = &c->q[i];
        if (i <= last && (k->kind == CMD_PRESENT || ((k->kind == CMD_CLEAR || k->kind == CMD_DRAW) && k->back))) {
            if (k->kind == CMD_PRESENT) { S3DCmd b; memset(&b, 0, sizeof(b)); b.kind = CMD_BACK; c->q[n++] = b; continue; }
            cmd_release(k); continue;
        }
        c->q[n++] = *k;
    }
    c->nq = n;
}
/* copy-on-write: before a buffer or texture that queued commands use is
   changed, the queued commands get a copy of its current contents */
static int queued_uses(S3DCtx *c, const void *res, int kind) {
    int i, j;
    for (i = 0; i < c->nq; i++) {
        const S3DCmd *k = &c->q[i];
        if (kind == 1 && k->ib == res) return 1;
        if (kind == 2 && k->t == res) return 2;   /* render target */
        if (!k->s) continue;
        if (kind == 0) { for (j = 0; j < S3D_MAX_ATTR; j++) if (k->s->va_buf[j] == res) return 1; }
        if (kind == 2) { for (j = 0; j < S3D_MAX_SAMP; j++) if (k->s->tex[j] == res) return 1; }
    }
    return 0;
}
static void retarget(void *res, void *cp, int kind) {
    S3DCtx *c; int i, j;
    for (c = g_ctxs; c; c = c->next_ctx) for (i = 0; i < c->nq; i++) {
        S3DCmd *k = &c->q[i];
        if (kind == 1 && k->ib == res) { k->ib->pending--; k->ib->refs--; k->ib = (S3DIBuf*)cp; k->ib->refs++; k->ib->pending++; }
        if (!k->s) continue;
        for (j = 0; kind == 0 && j < S3D_MAX_ATTR; j++) if (k->s->va_buf[j] == res) { S3DVBuf *o = k->s->va_buf[j]; o->pending--; o->refs--; k->s->va_buf[j] = (S3DVBuf*)cp; ((S3DVBuf*)cp)->refs++; ((S3DVBuf*)cp)->pending++; if (k->s == c->snap) c->dirty = 1; }
        for (j = 0; kind == 2 && j < S3D_MAX_SAMP; j++) if (k->s->tex[j] == res) { S3DTex *o = k->s->tex[j]; o->pending--; o->refs--; k->s->tex[j] = (S3DTex*)cp; ((S3DTex*)cp)->refs++; ((S3DTex*)cp)->pending++; if (k->s == c->snap) c->dirty = 1; }
    }
}
static int any_queued_use(const void *res, int kind) { S3DCtx *c; int u = 0, r; for (c = g_ctxs; c; c = c->next_ctx) if ((r = queued_uses(c, res, kind)) > u) u = r; return u; }
void s3d_vbuf_before_write(S3DVBuf *b) {
    S3DVBuf *cp;
    if (!b->pending || !any_queued_use(b, 0)) return;
    cp = s3d_vbuf_new(b->nverts, b->d32);
    if (!cp) { s3d_flush_all(); return; }
    memcpy(cp->data, b->data, (size_t)b->nverts * b->d32 * 4);
    retarget(b, cp, 0);
    s3d_vbuf_unref(cp);
}
void s3d_ibuf_before_write(S3DIBuf *b) {
    S3DIBuf *cp;
    if (!b->pending || !any_queued_use(b, 1)) return;
    cp = s3d_ibuf_new(b->n);
    if (!cp) { s3d_flush_all(); return; }
    memcpy(cp->data, b->data, (size_t)b->n * sizeof(u16));
    retarget(b, cp, 1);
    s3d_ibuf_unref(cp);
}
void s3d_tex_before_write(S3DTex *t) {
    S3DTex *cp; int u, f;
    if (!t->pending || !(u = any_queued_use(t, 2))) return;
    if (u == 2 || !(cp = s3d_tex_new(t->kind, t->w, t->h, t->format))) { s3d_flush_all(); return; }
    for (f = 0; f < t->nfaces; f++) memcpy(cp->px[f], t->px[f], (size_t)t->w * t->h * 4);
    retarget(t, cp, 2);
    s3d_tex_unref(cp);
}

const u8 *s3d_front(S3DCtx *c) { s3d_flush(c); return c->configured ? c->front : NULL; }
u8 *s3d_read_back(S3DCtx *c, int *w, int *h) {
    u8 *o;
    s3d_flush(c);
    if (!c->configured || !c->back.color) return NULL;
    o = (u8*)malloc((size_t)c->back.w * c->back.h * 4);
    if (!o) return NULL;
    resolve(&c->back, o); *w = c->back.w; *h = c->back.h;
    return o;
}

void s3d_set_vertex_buffer(S3DCtx *c, int i, S3DVBuf *b, u32 off, int fmt) {
    if (i < 0 || i >= S3D_MAX_ATTR) return;
    if (b) b->refs++;
    s3d_vbuf_unref(c->st.va_buf[i]);
    c->st.va_buf[i] = b; c->st.va_off[i] = off; c->st.va_fmt[i] = fmt;
    c->dirty = 1;
}
void s3d_set_program(S3DCtx *c, S3DProg *p) {
    int i;
    if (p) p->refs++;
    s3d_prog_unref(c->st.prog);
    c->st.prog = p;
    if (p && p->ok) for (i = 0; i < S3D_MAX_SAMP; i++) if (p->has_samp[i]) c->st.samp[i] = p->samp[i];
    c->dirty = 1;
}
void s3d_set_texture(S3DCtx *c, int i, S3DTex *t) {
    if (i < 0 || i >= S3D_MAX_SAMP) return;
    if (t) t->refs++;
    s3d_tex_unref(c->st.tex[i]);
    c->st.tex[i] = t;
    c->dirty = 1;
}
void s3d_set_constants(S3DCtx *c, int fragment, u32 first, const float *v, u32 nfloats) {
    float *dst = fragment ? c->st.fc : c->st.vc; u32 cap = (fragment ? S3D_MAX_FC : S3D_MAX_VC) * 4, off = first * 4, i;
    if (first >= cap / 4) return;
    for (i = 0; i < nfloats && off + i < cap; i++) dst[off + i] = v[i];
    c->dirty = 1;
}

int s3d_draw(S3DCtx *c, S3DIBuf *ib, u32 first, s32 ntri, int *missing) {
    S3DProg *p = c->st.prog; S3DCmd *k; int a;
    *missing = -1;
    if (!p || !p->ok || !ib) return -1;
    /* Flash skips the draw when the vertex program reads a stream that is not set */
    for (a = 0; a < S3D_MAX_ATTR; a++) if ((p->vp.va_used & (1 << a)) && !c->st.va_buf[a]) { *missing = a; return -1; }
    if (c->dirty || !c->snap) {
        S3DState *s = state_snapshot(&c->st);
        if (!s) return -1;
        state_unref(c->snap); c->snap = s; c->dirty = 0;
    }
    k = enqueue(c, CMD_DRAW);
    if (!k) return -1;
    k->s = c->snap; c->snap->refs++;
    k->ib = ib; ib->refs++; ib->pending++;
    k->first = first; k->ntri = ntri;
    return 0;
}

/* ---------------------------------------------------------------- */
/* drawing                                                          */

#define MAXV (4 + 4 * S3D_MAX_VARY)

typedef struct {
    const S3DState *st; S3DSurf *s; const S3DProg *p;
    int nvary; int vary_idx[S3D_MAX_VARY];
    int stride;
    int x0, y0, x1, y1;            /* pixel bounds (scissor & target) */
    AgalVM *fvm;
} DrawState;

static void fetch_attr(const S3DState *c, int i, u32 vi, float *o) {
    S3DVBuf *b = c->va_buf[i]; int fmt = c->va_fmt[i], n, k; size_t base, size;
    o[0] = o[1] = o[2] = 0; o[3] = 1;
    if (!b) { o[3] = 0; return; }
    n = fmt == S3D_VF_FLOAT1 ? 1 : fmt == S3D_VF_FLOAT2 ? 2 : fmt == S3D_VF_FLOAT3 ? 3 : 4;
    if (vi >= b->nverts) { o[3] = 0; return; }
    base = ((size_t)vi * b->d32 + c->va_off[i]) * 4; size = (size_t)b->nverts * b->d32 * 4;
    if (fmt == S3D_VF_BYTES4) {
        if (base + 4 > size) return;
        for (k = 0; k < 4; k++) o[k] = b->data[base + k] / 255.0f;
        return;
    }
    for (k = 0; k < n; k++) {
        size_t at = base + (size_t)k * 4; float f;
        if (at + 4 > size) { o[k] = 0; continue; }
        memcpy(&f, b->data + at, 4); o[k] = f;
    }
}

static int cmp_test(int cmp, double a, double b) {
    switch (cmp) {
    case S3D_CMP_NEVER: return 0;
    case S3D_CMP_LESS: return a < b;
    case S3D_CMP_EQUAL: return a == b;
    case S3D_CMP_LESS_EQUAL: return a <= b;
    case S3D_CMP_GREATER: return a > b;
    case S3D_CMP_NOT_EQUAL: return a != b;
    case S3D_CMP_GREATER_EQUAL: return a >= b;
    default: return 1;
    }
}
static u8 stencil_op(int op, u8 v, u8 ref) {
    switch (op) {
    case S3D_ST_ZERO: return 0;
    case S3D_ST_SET: return ref;
    case S3D_ST_INC_SAT: return v == 255 ? 255 : (u8)(v + 1);
    case S3D_ST_DEC_SAT: return v == 0 ? 0 : (u8)(v - 1);
    case S3D_ST_INVERT: return (u8)~v;
    case S3D_ST_INC_WRAP: return (u8)(v + 1);
    case S3D_ST_DEC_WRAP: return (u8)(v - 1);
    default: return v;
    }
}
static double blend_factor(int f, const float *src, const double *dst, int k) {
    int alpha = k == 3;
    switch (f) {
    case S3D_BF_ZERO: return 0;
    case S3D_BF_ONE: return 1;
    case S3D_BF_SRC_COLOR: return alpha ? src[3] : src[k];
    case S3D_BF_ONE_MINUS_SRC_COLOR: return 1 - (alpha ? src[3] : src[k]);
    case S3D_BF_SRC_ALPHA: return src[3];
    case S3D_BF_ONE_MINUS_SRC_ALPHA: return 1 - src[3];
    case S3D_BF_DST_COLOR: return alpha ? dst[3] : dst[k];
    case S3D_BF_ONE_MINUS_DST_COLOR: return 1 - (alpha ? dst[3] : dst[k]);
    case S3D_BF_DST_ALPHA: return dst[3];
    case S3D_BF_ONE_MINUS_DST_ALPHA: return 1 - dst[3];
    default: return 0;
    }
}

/* write one sample */
static void output_sample(DrawState *ds, size_t si, const float *col, double z, int front) {
    const S3DState *c = ds->st; S3DSurf *s = ds->s; u8 *px; int k;
    const S3DStencilFace *sf = front ? &c->st_front : &c->st_back; int st_on = s->stencil && (front ? c->st_front_on : c->st_back_on);
    u8 ref = (u8)(c->st_ref & 0xFF), rm = (u8)(c->st_rmask & 0xFF), wm = (u8)(c->st_wmask & 0xFF);
    if (st_on) {
        u8 v = s->stencil[si];
        if (!cmp_test(sf->cmp, ref & rm, v & rm)) { u8 nv = stencil_op(sf->fail, v, ref); s->stencil[si] = (u8)((v & ~wm) | (nv & wm)); return; }
    }
    if (s->depth) {
        if (!cmp_test(c->depth_cmp, z, s->depth[si])) {
            if (st_on) { u8 v = s->stencil[si], nv = stencil_op(sf->dfail, v, ref); s->stencil[si] = (u8)((v & ~wm) | (nv & wm)); }
            return;
        }
        if (c->depth_mask) s->depth[si] = (float)z;
    }
    if (st_on) { u8 v = s->stencil[si], nv = stencil_op(sf->pass, v, ref); s->stencil[si] = (u8)((v & ~wm) | (nv & wm)); }
    px = s->color + si * 4;
    if (c->blend_src == S3D_BF_ONE && c->blend_dst == S3D_BF_ZERO) {
        for (k = 0; k < 4; k++) if (c->cmask[k]) px[k] = unorm(col[k]);
    } else {
        double dst[4]; float src[4];
        for (k = 0; k < 4; k++) { dst[k] = px[k] / 255.0; src[k] = col[k] != col[k] ? 0 : col[k] < 0 ? 0 : col[k] > 1 ? 1 : col[k]; }
        for (k = 0; k < 4; k++) {
            double v;
            if (!c->cmask[k]) continue;
            v = src[k] * blend_factor(c->blend_src, src, dst, k) + dst[k] * blend_factor(c->blend_dst, src, dst, k);
            px[k] = unorm(v);
        }
    }
}

typedef struct { double x, y, z, iw; const float *v; } SVert;   /* screen-space vertex; v: varyings (premultiplied by 1/w) */

static const double samp4[4][2] = { { -2.0 / 16, -6.0 / 16 }, { 6.0 / 16, -2.0 / 16 }, { -6.0 / 16, 2.0 / 16 }, { 2.0 / 16, 6.0 / 16 } };
static const double samp2[2][2] = { { 4.0 / 16, 4.0 / 16 }, { -4.0 / 16, -4.0 / 16 } };
static const double samp1[1][2] = { { 0, 0 } };

static double edge(const SVert *a, const SVert *b, double px, double py) { return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x); }

static void raster_tri(DrawState *ds, const SVert *v0, const SVert *v1, const SVert *v2, int front) {
    const SVert *A = v0, *B = v1, *C = v2; double area, minx, maxx, miny, maxy; int x0, y0, x1, y1, qx, qy, ns = ds->s->ns, k;
    const double (*so)[2] = ns == 4 ? samp4 : ns == 2 ? samp2 : samp1;
    int tl[3]; const SVert *ea[3], *eb[3];
    area = edge(A, B, C->x, C->y);
    if (area == 0 || area != area) return;
    if (area < 0) { const SVert *t = B; B = C; C = t; area = -area; }
    ea[0] = B; eb[0] = C; ea[1] = C; eb[1] = A; ea[2] = A; eb[2] = B;    /* edge k is opposite vertex k */
    for (k = 0; k < 3; k++) { double dx = eb[k]->x - ea[k]->x, dy = eb[k]->y - ea[k]->y; tl[k] = dy < 0 || (dy == 0 && dx > 0); }
    minx = A->x; if (B->x < minx) minx = B->x; if (C->x < minx) minx = C->x;
    maxx = A->x; if (B->x > maxx) maxx = B->x; if (C->x > maxx) maxx = C->x;
    miny = A->y; if (B->y < miny) miny = B->y; if (C->y < miny) miny = C->y;
    maxy = A->y; if (B->y > maxy) maxy = B->y; if (C->y > maxy) maxy = C->y;
    if (!(minx < 1e7 && maxx > -1e7 && miny < 1e7 && maxy > -1e7)) return;
    x0 = (int)floor(minx - 1); y0 = (int)floor(miny - 1); x1 = (int)ceil(maxx + 1); y1 = (int)ceil(maxy + 1);
    if (x0 < ds->x0) x0 = ds->x0;
    if (y0 < ds->y0) y0 = ds->y0;
    if (x1 > ds->x1) x1 = ds->x1;
    if (y1 > ds->y1) y1 = ds->y1;
    if (x0 >= x1 || y0 >= y1) return;
    x0 &= ~1; y0 &= ~1;
    for (qy = y0; qy < y1; qy += 2) for (qx = x0; qx < x1; qx += 2) {
        int cov[4], any = 0, l, j, vi;
        double w[4][3];
        AgalVM *m = ds->fvm;
        for (l = 0; l < 4; l++) {
            int px = qx + (l & 1), py = qy + (l >> 1);
            double cx = px + 0.5, cy = py + 0.5;
            cov[l] = 0;
            if (px < ds->x0 || px >= ds->x1 || py < ds->y0 || py >= ds->y1) continue;
            for (j = 0; j < ns; j++) {
                double sx = cx + so[j][0], sy = cy + so[j][1]; int in = 1;
                for (k = 0; k < 3 && in; k++) { double e = edge(ea[k], eb[k], sx, sy); if (!(e > 0 || (e == 0 && tl[k]))) in = 0; }
                if (in) cov[l] |= 1 << j;
            }
            if (cov[l]) any = 1;
        }
        if (!any) continue;
        /* varyings at the pixel centres (perspective correct); helper lanes extrapolate */
        for (l = 0; l < 4; l++) {
            double cx = qx + (l & 1) + 0.5, cy = qy + (l >> 1) + 0.5, iw, inv;
            for (k = 0; k < 3; k++) w[l][k] = edge(ea[k], eb[k], cx, cy) / area;
            iw = w[l][0] * A->iw + w[l][1] * B->iw + w[l][2] * C->iw; inv = iw != 0 ? 1.0 / iw : 0;
            for (vi = 0; vi < ds->nvary; vi++) {
                int r = ds->vary_idx[vi]; float *d = m->r[l][R_IN + r];
                for (k = 0; k < 4; k++) d[k] = (float)((w[l][0] * A->v[vi * 4 + k] + w[l][1] * B->v[vi * 4 + k] + w[l][2] * C->v[vi * 4 + k]) * inv);
            }
        }
        m->killed = 0;
        /* helper lanes only matter for derivatives */
        m->lanes = ds->p->fp.has_deriv ? 15 : (cov[0] ? 1 : 0) | (cov[1] ? 2 : 0) | (cov[2] ? 4 : 0) | (cov[3] ? 8 : 0);
        agal_exec(m, &ds->p->fp);
        for (l = 0; l < 4; l++) {
            int px = qx + (l & 1), py = qy + (l >> 1); size_t base;
            if (!cov[l] || (m->killed & (1 << l))) continue;
            base = ((size_t)py * ds->s->w + px) * ns;
            for (j = 0; j < ns; j++) {
                double sx, sy, z;
                if (!(cov[l] & (1 << j))) continue;
                sx = px + 0.5 + so[j][0]; sy = py + 0.5 + so[j][1];
                z = (edge(ea[0], eb[0], sx, sy) * A->z + edge(ea[1], eb[1], sx, sy) * B->z + edge(ea[2], eb[2], sx, sy) * C->z) / area;
                output_sample(ds, base + j, m->r[l][R_O], z, front);
            }
        }
    }
}

/* clip a polygon (vertices of `stride` floats: x y z w, varyings) against one plane */
static int clip_plane(const float *in, int n, float *out, int stride, int plane) {
    int i, k, no = 0;
    for (i = 0; i < n; i++) {
        const float *a = in + i * stride, *b = in + ((i + 1) % n) * stride; double da, db;
        switch (plane) {
        case 0: da = a[0] + a[3]; db = b[0] + b[3]; break;
        case 1: da = a[3] - a[0]; db = b[3] - b[0]; break;
        case 2: da = a[1] + a[3]; db = b[1] + b[3]; break;
        case 3: da = a[3] - a[1]; db = b[3] - b[1]; break;
        case 4: da = a[2]; db = b[2]; break;
        case 5: da = a[3] - a[2]; db = b[3] - b[2]; break;
        default: da = a[3] - 1e-6; db = b[3] - 1e-6; break;
        }
        if (da >= 0) { memcpy(out + no * stride, a, (size_t)stride * sizeof(float)); no++; }
        if ((da >= 0) != (db >= 0)) {
            double t = da / (da - db); float *o = out + no * stride;
            for (k = 0; k < stride; k++) o[k] = (float)(a[k] + (b[k] - a[k]) * t);
            no++;
        }
    }
    return no;
}

static double snap(double v) { return floor(v * 256.0 + 0.5) / 256.0; }

static void draw_triangle(DrawState *ds, const float *a, const float *b, const float *c3) {
    float buf0[16 * MAXV], buf1[16 * MAXV]; float *cur = buf0, *nxt = buf1; int n = 3, pl, i, k, stride = ds->stride, front;
    SVert sv[16]; float vv[16][4 * S3D_MAX_VARY]; double W = ds->s->w, H = ds->s->h, area2 = 0;
    const float *tri[3]; int inside = 1;
    tri[0] = a; tri[1] = b; tri[2] = c3;
    for (i = 0; i < 3; i++) {
        const float *p = tri[i];
        memcpy(cur + i * stride, p, (size_t)stride * sizeof(float));
        if (!(p[0] >= -p[3] && p[0] <= p[3] && p[1] >= -p[3] && p[1] <= p[3] && p[2] >= 0 && p[2] <= p[3] && p[3] >= 1e-6)) inside = 0;
    }
    if (!inside) {
        for (pl = 0; pl < 7 && n >= 3; pl++) { float *t; n = clip_plane(cur, n, nxt, stride, pl); t = cur; cur = nxt; nxt = t; }
        if (n < 3) return;
    }
    for (i = 0; i < n; i++) {
        const float *p = cur + i * stride; double iw = 1.0 / p[3];
        sv[i].x = snap((p[0] * iw * 0.5 + 0.5) * W);
        sv[i].y = snap((0.5 - p[1] * iw * 0.5) * H);
        sv[i].z = p[2] * iw; sv[i].iw = iw;
        for (k = 0; k < ds->nvary * 4; k++) vv[i][k] = (float)(p[4 + k] * iw);
        sv[i].v = vv[i];
    }
    /* facing: clockwise on screen (y down: positive area) is front */
    for (i = 1; i + 1 < n; i++) area2 += edge(&sv[0], &sv[i], sv[i + 1].x, sv[i + 1].y);
    if (area2 == 0) return;
    front = area2 > 0;
    if (ds->st->cull == S3D_FACE_BOTH) return;
    if (ds->st->cull == S3D_FACE_BACK && !front) return;
    if (ds->st->cull == S3D_FACE_FRONT && front) return;
    for (i = 1; i + 1 < n; i++) raster_tri(ds, &sv[0], &sv[i], &sv[i + 1], front);
}

static void exec_draw(S3DCtx *c, const S3DState *st, S3DIBuf *ib, u32 first, s32 ntri) {
    const S3DProg *p = st->prog; S3DSurf *s = cur_surf(c); DrawState ds; AgalVM *vvm = NULL, *fvm = NULL;
    u32 nidx, i, maxv = 0; float *vout = NULL; u8 *done = NULL; int k;
    if (!p || !p->ok || !ib || !s->color) return;
    for (k = 0; k < S3D_MAX_ATTR; k++) if ((p->vp.va_used & (1 << k)) && !st->va_buf[k]) return;
    for (k = 0; k < S3D_MAX_SAMP; k++) if ((p->fp.samp_used & (1 << k)) && !st->tex[k]) return;   /* unbound texture: skipped (Ruffle) */
    if (first >= ib->n) return;
    if (ntri < 0) ntri = (s32)((ib->n - first) / 3);
    nidx = (u32)ntri * 3;
    if (nidx > ib->n - first) nidx = (ib->n - first) / 3 * 3;
    if (!nidx) return;
    memset(&ds, 0, sizeof(ds));
    ds.st = st; ds.s = s; ds.p = p;
    for (k = 0; k < S3D_MAX_VARY; k++) if (p->fp.vary_used & (1 << k)) ds.vary_idx[ds.nvary++] = k;
    ds.stride = 4 + 4 * ds.nvary;
    ds.x0 = 0; ds.y0 = 0; ds.x1 = s->w; ds.y1 = s->h;
    if (st->scissor_on) {
        int sx0 = (int)st->sc_x, sy0 = (int)st->sc_y, sx1 = (int)(st->sc_x + st->sc_w), sy1 = (int)(st->sc_y + st->sc_h);
        /* an invalid rectangle is ignored (Ruffle) */
        if (!(st->sc_x < 0 || st->sc_y < 0 || sx1 > s->w || sy1 > s->h || st->sc_w <= 0 || st->sc_h <= 0)) { ds.x0 = sx0; ds.y0 = sy0; ds.x1 = sx1; ds.y1 = sy1; }
    }
    for (i = 0; i < nidx; i++) { u32 v = ib->data[first + i]; if (v > maxv) maxv = v; }
    vout = (float*)malloc(((size_t)maxv + 1) * ds.stride * sizeof(float));
    done = (u8*)calloc((size_t)maxv + 1, 1);
    vvm = (AgalVM*)calloc(1, sizeof(AgalVM)); fvm = (AgalVM*)calloc(1, sizeof(AgalVM));
    if (!vout || !done || !vvm || !fvm) goto out;
    vvm->nl = 1; vvm->lanes = 1; vvm->c = st->vc; vvm->nc = S3D_MAX_VC; vvm->va_fmt = st->va_fmt; vvm->st = st;
    fvm->nl = 4; fvm->c = st->fc; fvm->nc = S3D_MAX_FC; fvm->st = st;
    ds.fvm = fvm;
    for (i = 0; i + 2 < nidx; i += 3) {
        const float *tv[3]; int j;
        for (j = 0; j < 3; j++) {
            u32 vi = ib->data[first + i + j]; float *o = vout + (size_t)vi * ds.stride;
            if (!done[vi]) {
                int a;
                for (a = 0; a < S3D_MAX_ATTR; a++) if (p->vp.va_used & (1 << a)) fetch_attr(st, a, vi, vvm->r[0][R_IN + a]);
                memset(vvm->r[0][R_VOUT], 0, sizeof(float) * 4 * (S3D_MAX_VARY + 1));
                agal_exec(vvm, &p->vp);
                memcpy(o, vvm->r[0][R_O], 4 * sizeof(float));
                for (a = 0; a < ds.nvary; a++) memcpy(o + 4 + a * 4, vvm->r[0][R_VOUT + ds.vary_idx[a]], 4 * sizeof(float));
                done[vi] = 1;
            }
            tv[j] = o;
        }
        draw_triangle(&ds, tv[0], tv[1], tv[2]);
    }
out:
    free(vout); free(done); free(vvm); free(fvm);
}

/* ---------------------------------------------------------------- */
/* ATF                                                              */

static u32 be24(const u8 *p) { return ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2]; }
static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

int s3d_atf_parse(const u8 *b, u32 len, S3DAtf *a) {
    u32 p;
    memset(a, 0, sizeof(*a));
    if (len < 6 || b[0] != 'A' || b[1] != 'T' || b[2] != 'F') return 0;
    if (b[6] == 0xFF) {
        if (len < 16) return 0;
        a->mips = b[5] >> 1; a->version = b[7]; p = 12;
    } else { a->version = 0; p = 6; }
    if (p + 4 > len) return 0;
    a->cube = b[p] >> 7; a->format = b[p] & 0x7f;
    if (!(a->format <= 5 || a->format == 0xc || a->format == 0xd)) return 0;
    a->w = 1 << (b[p + 1] & 31); a->h = 1 << (b[p + 2] & 31);
    if (!a->mips) a->mips = b[p + 3];
    a->data = b + p + 4; a->len = len - (p + 4);
    return 1;
}

static int atf_records(int format) {
    switch (format) { case 0: case 1: return 1; case 2: return 11; case 3: case 5: return 4; case 4: return 16; case 0xc: return 12; case 0xd: return 17; }
    return 0;
}

static void rgb565(u16 c, int *r, int *g, int *b) { *r = ((c >> 11) & 31) * 255 / 31; *g = ((c >> 5) & 63) * 255 / 63; *b = (c & 31) * 255 / 31; }
static void dxt_color_block(const u8 *blk, u8 out[16][4], int dxt1) {
    u16 c0 = (u16)(blk[0] | (blk[1] << 8)), c1 = (u16)(blk[2] | (blk[3] << 8)); int pal[4][4], i;
    rgb565(c0, &pal[0][0], &pal[0][1], &pal[0][2]); rgb565(c1, &pal[1][0], &pal[1][1], &pal[1][2]);
    pal[0][3] = pal[1][3] = 255;
    if (c0 > c1 || !dxt1) {
        for (i = 0; i < 3; i++) { pal[2][i] = (2 * pal[0][i] + pal[1][i]) / 3; pal[3][i] = (pal[0][i] + 2 * pal[1][i]) / 3; }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (i = 0; i < 3; i++) { pal[2][i] = (pal[0][i] + pal[1][i]) / 2; pal[3][i] = 0; }
        pal[2][3] = 255; pal[3][3] = 0;
    }
    for (i = 0; i < 16; i++) { int idx = (blk[4 + i / 4] >> ((i % 4) * 2)) & 3; out[i][0] = (u8)pal[idx][0]; out[i][1] = (u8)pal[idx][1]; out[i][2] = (u8)pal[idx][2]; out[i][3] = (u8)pal[idx][3]; }
}
static void dxt5_alpha_block(const u8 *blk, u8 out[16][4]) {
    int a[8], i; u32 lo, hi;
    a[0] = blk[0]; a[1] = blk[1];
    if (a[0] > a[1]) { for (i = 1; i < 7; i++) a[i + 1] = ((7 - i) * a[0] + i * a[1]) / 7; }
    else { for (i = 1; i < 5; i++) a[i + 1] = ((5 - i) * a[0] + i * a[1]) / 5; a[6] = 0; a[7] = 255; }
    lo = (u32)blk[2] | ((u32)blk[3] << 8) | ((u32)blk[4] << 16); hi = (u32)blk[5] | ((u32)blk[6] << 8) | ((u32)blk[7] << 16);
    for (i = 0; i < 16; i++) { int idx = i < 8 ? (int)((lo >> (3 * i)) & 7) : (int)((hi >> (3 * (i - 8))) & 7); out[i][3] = (u8)a[idx]; }
}

/* DXT1 / DXT5 blocks of the texture's size into RGBA */
static void atf_dxt_decode(const S3DAtf *a, const u8 *first, int dxt5, u8 *rgba) {
    int bw = (a->w + 3) / 4, bh = (a->h + 3) / 4, bx, by;
    for (by = 0; by < bh; by++) for (bx = 0; bx < bw; bx++) {
        u8 px[16][4]; int i;
        if (dxt5) { const u8 *blk = first + (by * bw + bx) * 16; dxt_color_block(blk + 8, px, 0); dxt5_alpha_block(blk, px); }
        else dxt_color_block(first + (by * bw + bx) * 8, px, 1);
        for (i = 0; i < 16; i++) {
            int x = bx * 4 + i % 4, y = by * 4 + i / 4;
            if (x < a->w && y < a->h) memcpy(rgba + ((size_t)y * a->w + x) * 4, px[i], 4);
        }
    }
}
/* the n-th data record of face `face`, first mip level */
static const u8 *atf_record(const S3DAtf *a, int face, int rec, u32 *len) {
    const u8 *p = a->data, *end = a->data + a->len; int f, m, r, nrec = atf_records(a->format), nfaces = a->cube ? 6 : 1;
    for (f = 0; f < nfaces; f++) for (m = 0; m < a->mips; m++) for (r = 0; r < nrec; r++) {
        u32 l;
        if (a->version == 0) { if (end - p < 3) return NULL; l = be24(p); p += 3; }
        else { if (end - p < 4) return NULL; l = be32(p); p += 4; }
        if ((u32)(end - p) < l) return NULL;
        if (f == face && m == 0 && r == rec) { *len = l; return p; }
        p += l;
    }
    return NULL;
}
/* ATF's LZMA: the 5 header bytes, no size, an end marker */
static u8 *atf_lzma(const u8 *d, u32 n, u32 cap, u32 *outn) {
    u8 *out; long r;
    if (n < 5 || !(out = (u8*)malloc(cap + 16))) return NULL;
    r = swf_lzma_decode(d, d + 5, n - 5, out, cap);
    if (r < 0) { free(out); return NULL; }
    *outn = (u32)r;
    return out;
}
/* Ruffle atf_jpegxr: each DXT5 block from its alpha end points (8-bit gray JPEG-XR, the two
   values in the upper and lower halves), the DXT1 alpha table, the colour end points
   (16-bit JPEG-XR pixels, same halves) and the DXT5 colour table */
static u8 *atf_compressed_alpha(const S3DAtf *a, int face) {
    u32 l0, l1, l2, l3, na, nc, i, half, nblk; const u8 *r0 = atf_record(a, face, 0, &l0), *r1 = atf_record(a, face, 1, &l1), *r2 = atf_record(a, face, 2, &l2), *r3 = atf_record(a, face, 3, &l3);
    u8 *alpha_tab = NULL, *rgb_tab = NULL, *jalpha = NULL, *jbgr = NULL, *out = NULL; int aw, ah, bw2, bh2, fmt, bpp;
    nblk = (u32)((a->w / 4) * (a->h / 4));
    if (!r0 || !r1 || !r2 || !r3 || !nblk) return NULL;
    alpha_tab = atf_lzma(r0, l0, nblk * 6, &na);
    rgb_tab = atf_lzma(r2, l2, nblk * 4, &nc);
    jalpha = jxr_decode_rgba(r1, l1, &aw, &ah);
    jbgr = jxr_decode_raw(r3, l3, &bw2, &bh2, &fmt, &bpp);
    half = nblk;   /* (width / 4) * (height / 4), used as a byte offset as Ruffle does */
    if (alpha_tab && rgb_tab && jalpha && jbgr && na / 6 == nc / 4 && aw == a->w / 4 && ah == a->h / 2 && bw2 == a->w / 4 && bh2 == a->h / 2 && bpp == 16
        && (u32)(aw * ah * 4) / 8 == na / 6 && (out = (u8*)malloc((size_t)nblk * 16)) != NULL) {
        u32 nb = na / 6, alen = (u32)(aw * ah * 4), blen = (u32)(bw2 * bh2 * 2);
        if (nb > nblk) nb = nblk;
        memset(out, 0, (size_t)nblk * 16);
        for (i = 0; i < nb; i++) {
            u8 *o = out + (size_t)i * 16;
            if (i * 4 < alen) o[0] = jalpha[i * 4];
            if (half + i * 4 < alen) o[1] = jalpha[half + i * 4];
            memcpy(o + 2, alpha_tab + i * 6, 6);
            if (i * 2 + 1 < blen) { o[8] = jbgr[i * 2]; o[9] = jbgr[i * 2 + 1]; }
            if (half * 2 + i * 2 + 1 < blen) { o[10] = jbgr[half * 2 + i * 2]; o[11] = jbgr[half * 2 + i * 2 + 1]; }
            memcpy(o + 12, rgb_tab + i * 4, 4);
        }
    }
    free(alpha_tab); free(rgb_tab); free(jalpha); free(jbgr);
    return out;
}

int s3d_atf_decode(const S3DAtf *a, int face, u8 *rgba) {
    const u8 *p = a->data, *end = a->data + a->len; int f, m, r, nrec = atf_records(a->format), nfaces = a->cube ? 6 : 1;
    const u8 *first = NULL; u32 flen = 0; int bw, bh, bx, by;
    if (!nrec || face >= nfaces) return 0;
    for (f = 0; f < nfaces; f++) for (m = 0; m < a->mips; m++) for (r = 0; r < nrec; r++) {
        u32 l;
        if (a->version == 0) { if (end - p < 3) return 0; l = be24(p); p += 3; }
        else { if (end - p < 4) return 0; l = be32(p); p += 4; }
        if ((u32)(end - p) < l) return 0;
        if (f == face && m == 0 && r == 0) { first = p; flen = l; }
        p += l;
    }
    if (!first) return 0;
    if (a->format == 0 || a->format == 1) {   /* a JPEG-XR image (Ruffle: through a TIFF to RGBA) */
        int w, h; u8 *px = jxr_decode_rgba(first, flen, &w, &h);
        if (!px) return 0;
        if (w == a->w && h == a->h) memcpy(rgba, px, (size_t)w * h * 4);
        free(px);
        return w == a->w && h == a->h;
    }
    if (a->format == 4) {   /* compressed alpha: DXT5 rebuilt from LZMA tables and JPEG-XR end points */
        u8 *dxt = atf_compressed_alpha(a, face);
        if (!dxt) return 0;
        atf_dxt_decode(a, dxt, 1, rgba);
        free(dxt);
        return 1;
    }
    if (a->format != 3 && a->format != 5) return 0;
    bw = (a->w + 3) / 4; bh = (a->h + 3) / 4;
    if (flen < (u32)(bw * bh * (a->format == 5 ? 16 : 8))) return 0;
    atf_dxt_decode(a, first, a->format == 5, rgba);
    return 1;
}
