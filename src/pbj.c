/* pbj.c - Pixel Bender bytecode parser and CPU interpreter (see pbj.h).
 *
 * The parser follows Ruffle's render/pixel_bender/src/parser.rs (with the
 * validation Flash Player does when a Shader is created: conditions must be
 * int registers, select operands of one kind, ceil/sign on floats, an
 * output parameter must exist).  Execution follows Ruffle's
 * naga-pixelbender, which models what Flash Player 10 does:
 *   - registers are vec4 (float file f0.., int file i0..), zero at the start
 *     of every pixel; a source operand is the register swizzled by its
 *     channels, a destination operand is written through its mask, the
 *     value converted to the register's kind (float -> int saturates)
 *   - binary operations use the destination as their first operand
 *   - comparisons write their result to i0.r
 *   - `if` tests the first channel != 0, `select` tests it == 1
 *   - float->int rounds to nearest even, bool->float gives 0 (a Flash bug),
 *     logical not is a bitwise not (another one)
 *   - sampling divides the coordinate by the image size; nearest/linear
 *     with clamp to edge, or transparent black outside the image when the
 *     shader runs as a filter */
#include "pbj.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* opcodes */
enum {
    OP_NOP = 0x00, OP_ADD, OP_SUB, OP_MUL, OP_RCP, OP_DIV, OP_ATAN2, OP_POW, OP_MOD, OP_MIN, OP_MAX, OP_STEP,
    OP_SIN, OP_COS, OP_TAN, OP_ASIN, OP_ACOS, OP_ATAN, OP_EXP, OP_EXP2, OP_LOG, OP_LOG2, OP_SQRT, OP_RSQRT,
    OP_ABS, OP_SIGN, OP_FLOOR, OP_CEIL, OP_FRACT, OP_MOV, OP_FTOI, OP_ITOF, OP_MATMATMUL, OP_VECMATMUL,
    OP_MATVECMUL, OP_NORMALIZE, OP_LENGTH, OP_DISTANCE, OP_DOT, OP_CROSS, OP_EQUAL, OP_NOTEQUAL, OP_LESS,
    OP_LESSEQUAL, OP_LNOT, OP_LAND, OP_LOR, OP_LXOR, OP_SAMPLENEAREST, OP_SAMPLELINEAR, OP_LOADIORF,
    OP_SELECT, OP_IF, OP_ELSE, OP_ENDIF, OP_FTOB, OP_BTOF, OP_ITOB, OP_BTOI, OP_VEQUAL, OP_VNOTEQUAL,
    OP_ANY, OP_ALL,
    OP_META1 = 0xA0, OP_PARAM = 0xA1, OP_META2 = 0xA2, OP_TEXTURE = 0xA3, OP_NAME = 0xA4, OP_VERSION = 0xA5
};

/* ---------------------------------------------------------------- */
/* types                                                            */

const char *pbj_type_name(int t) {
    static const char *const n[] = { "", "float", "float2", "float3", "float4", "matrix2x2", "matrix3x3", "matrix4x4",
        "int", "int2", "int3", "int4", "string", "bool", "bool2", "bool3", "bool4" };
    return t >= 1 && t <= 16 ? n[t] : "";
}
int pbj_type_count(int t) {
    switch (t) {
    case PBJ_FLOAT: case PBJ_INT: case PBJ_BOOL: return 1;
    case PBJ_FLOAT2: case PBJ_INT2: case PBJ_BOOL2: return 2;
    case PBJ_FLOAT3: case PBJ_INT3: case PBJ_BOOL3: return 3;
    case PBJ_FLOAT4: case PBJ_INT4: case PBJ_BOOL4: case PBJ_FLOAT2X2: return 4;
    case PBJ_FLOAT3X3: return 9;
    case PBJ_FLOAT4X4: return 16;
    default: return 0;
    }
}
int pbj_type_is_float(int t) { return t >= PBJ_FLOAT && t <= PBJ_FLOAT4X4; }
void pbj_value_free(PbjValue *v) { if (v) { free(v->s); v->s = NULL; } }
void pbj_value_copy(PbjValue *dst, const PbjValue *src) {
    *dst = *src; dst->s = NULL;
    if (src->s) { size_t n = strlen(src->s); dst->s = (char*)malloc(n + 1); if (dst->s) memcpy(dst->s, src->s, n + 1); }
}
int pbj_is_outcoord(const PbjParam *p) { return !p->texture && p->name && !strcmp(p->name, "_OutCoord"); }

/* ---------------------------------------------------------------- */
/* parser                                                           */

typedef struct { const u8 *p; size_t n, pos; int eof; } PR;

static u32 rd8(PR *r) { if (r->pos >= r->n) { r->eof = 1; return 0; } return r->p[r->pos++]; }
static u32 rd16(PR *r) { u32 a = rd8(r); return a | (rd8(r) << 8); }
static u32 rd24(PR *r) { u32 a = rd16(r); return a | (rd8(r) << 16); }
static u32 rd32(PR *r) { u32 a = rd16(r); return a | (rd16(r) << 16); }
static s32 rdi16(PR *r) { u32 v = rd16(r); return v >= 0x8000u ? (s32)v - 65536 : (s32)v; }
static float rdfloat(PR *r) {   /* big endian */
    u32 v = rd8(r) << 24; float f;
    v |= rd8(r) << 16; v |= rd8(r) << 8; v |= rd8(r);
    memcpy(&f, &v, 4);
    return f;
}
static void skip(PR *r, int n) { while (n-- > 0) rd8(r); }
static char *rdstr(PR *r) {
    size_t start = r->pos, len; char *s;
    while (r->pos < r->n && r->p[r->pos]) r->pos++;
    if (r->pos >= r->n) { r->eof = 1; return NULL; }
    len = r->pos - start; r->pos++;
    s = (char*)malloc(len + 1);
    if (!s) return NULL;
    memcpy(s, r->p + start, len); s[len] = 0;
    return s;
}

static int utf8_valid(const u8 *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        u8 c = s[i]; int k, m; u32 cp;
        if (c < 0x80) { i++; continue; }
        if ((c & 0xE0) == 0xC0) { k = 1; cp = c & 0x1F; m = 0x80; }
        else if ((c & 0xF0) == 0xE0) { k = 2; cp = c & 0x0F; m = 0x800; }
        else if ((c & 0xF8) == 0xF0) { k = 3; cp = c & 0x07; m = 0x10000; }
        else return 0;
        if (i + (size_t)k >= n) return 0;
        { int j; for (j = 1; j <= k; j++) { if ((s[i + j] & 0xC0) != 0x80) return 0; cp = (cp << 6) | (s[i + j] & 0x3F); } }
        if (cp < (u32)m || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
        i += (size_t)k + 1;
    }
    return 1;
}

static void src_reg(u32 val, int size, PbjReg *r) {
    u32 sw = val >> 16; int i;
    memset(r, 0, sizeof(*r));
    r->index = (int)(val & 0x7FFF); r->is_int = (val & 0x8000) != 0; r->n = size;
    for (i = 0; i < size; i++) r->ch[i] = (int)((sw >> (6 - i * 2)) & 3);
}
static void dst_reg(u32 val, u32 mask, PbjReg *r) {
    memset(r, 0, sizeof(*r));
    r->index = (int)(val & 0x7FFF); r->is_int = (val & 0x8000) != 0;
    if (mask & 8) r->ch[r->n++] = 0;
    if (mask & 4) r->ch[r->n++] = 1;
    if (mask & 2) r->ch[r->n++] = 2;
    if (mask & 1) r->ch[r->n++] = 3;
}
static void mat_reg(u32 val, int dim, PbjReg *r) {
    memset(r, 0, sizeof(*r));
    r->index = (int)(val & 0x7FFF); r->is_int = (val & 0x8000) != 0; r->mat = dim; r->n = 1;
}

static int read_value(PR *r, int type, PbjValue *v) {
    int i, n;
    memset(v, 0, sizeof(*v)); v->type = type;
    if (type < PBJ_FLOAT || type > PBJ_BOOL4) return 0;
    if (type == PBJ_STRING) { v->s = rdstr(r); return v->s != NULL || r->eof; }
    n = pbj_type_count(type);
    if (pbj_type_is_float(type)) for (i = 0; i < n; i++) v->f[i] = rdfloat(r);
    else for (i = 0; i < n; i++) v->i[i] = rdi16(r);
    return 1;
}

typedef struct { PbjMeta *m; int n, cap; } MetaList;
static int meta_push(MetaList *l, char *key, const PbjValue *v) {
    if (l->n == l->cap) { int nc = l->cap ? l->cap * 2 : 8; PbjMeta *nm = (PbjMeta*)realloc(l->m, (size_t)nc * sizeof(PbjMeta)); if (!nm) return 0; l->m = nm; l->cap = nc; }
    l->m[l->n].key = key; l->m[l->n].v = *v; l->n++;
    return 1;
}
static void meta_free(PbjMeta *m, int n) { int i; for (i = 0; i < n; i++) { free(m[i].key); pbj_value_free(&m[i].v); } free(m); }

/* the metadata read so far belongs to the last parameter (or the kernel) */
static void apply_meta(PbjShader *s, MetaList *l) {
    if (s->nparams == 0) { meta_free(s->meta, s->nmeta); s->meta = l->m; s->nmeta = l->n; }
    else {
        PbjParam *p = &s->params[s->nparams - 1];
        if (p->texture) meta_free(l->m, l->n);
        else { meta_free(p->meta, p->nmeta); p->meta = l->m; p->nmeta = l->n; }
    }
    l->m = NULL; l->n = l->cap = 0;
}

static void shader_free(PbjShader *s) {
    int i;
    if (!s) return;
    free(s->name); meta_free(s->meta, s->nmeta);
    for (i = 0; i < s->nparams; i++) { free(s->params[i].name); meta_free(s->params[i].meta, s->params[i].nmeta); }
    free(s->params); free(s->ops); free(s);
}
PbjShader *pbj_ref(PbjShader *s) { if (s) s->refs++; return s; }
void pbj_unref(PbjShader *s) { if (s && --s->refs <= 0) shader_free(s); }

int pbj_output_param(const PbjShader *s) {
    int i;
    for (i = s->nparams - 1; i >= 0; i--) if (!s->params[i].texture && s->params[i].out) return i;
    return -1;
}
int pbj_output_channels(const PbjShader *s) { int i = pbj_output_param(s); return i < 0 ? 0 : s->params[i].reg.n; }

static int push_op(PbjShader *s, int *cap, const PbjOp *op) {
    if (s->nops == *cap) { int nc = *cap ? *cap * 2 : 32; PbjOp *no = (PbjOp*)realloc(s->ops, (size_t)nc * sizeof(PbjOp)); if (!no) return 0; s->ops = no; *cap = nc; }
    s->ops[s->nops++] = *op;
    return 1;
}
static int push_param(PbjShader *s, int *cap, const PbjParam *p) {
    if (s->nparams == *cap) { int nc = *cap ? *cap * 2 : 8; PbjParam *np = (PbjParam*)realloc(s->params, (size_t)nc * sizeof(PbjParam)); if (!np) return 0; s->params = np; *cap = nc; }
    s->params[s->nparams++] = *p;
    return 1;
}

static void reg_extent(PbjShader *s, const PbjReg *r) {
    int top = r->index + (r->mat > 2 ? r->mat : 1);
    if (r->is_int) { if (top > s->nireg) s->nireg = top; }
    else if (top > s->nfreg) s->nfreg = top;
}

static int parse_body(PR *r, PbjShader *s) {
    MetaList ml; int opcap = 0, pcap = 0, err = PBJ_OK;
    memset(&ml, 0, sizeof(ml));
    while (r->pos < r->n) {
        u32 raw = rd8(r); PbjOp op;
        memset(&op, 0, sizeof(op)); op.kind = -1;
        switch (raw) {
        case OP_NOP:
            rd32(r); rd16(r); op.kind = PBJ_OP_NOP;
            break;
        case OP_META1: case OP_META2: {
            int type = (int)rd8(r); char *key = rdstr(r); PbjValue v;
            if (r->eof) { free(key); break; }
            if (!read_value(r, type, &v)) { free(key); err = PBJ_ERR_INVALID; break; }
            if (r->eof) { free(key); pbj_value_free(&v); break; }
            if (!meta_push(&ml, key, &v)) { free(key); pbj_value_free(&v); err = PBJ_ERR_INVALID; }
            break; }
        case OP_PARAM: {
            PbjParam p; u32 q = rd8(r), type = rd8(r), reg = rd16(r), mask = rd8(r);
            memset(&p, 0, sizeof(p));
            p.name = rdstr(r);
            if (r->eof) { free(p.name); break; }
            if (type < PBJ_FLOAT || type > PBJ_BOOL4) { free(p.name); err = PBJ_ERR_INVALID; break; }
            p.type = (int)type; p.out = q == 2;
            if (type == PBJ_FLOAT2X2) mat_reg(reg, 2, &p.reg);
            else if (type == PBJ_FLOAT3X3) mat_reg(reg, 3, &p.reg);
            else if (type == PBJ_FLOAT4X4) mat_reg(reg, 4, &p.reg);
            else dst_reg(reg, mask & 15, &p.reg);
            p.reg.is_int = (reg & 0x8000) != 0;
            if (p.reg.mat) p.reg.is_int = 0;
            apply_meta(s, &ml);
            if (!push_param(s, &pcap, &p)) { free(p.name); err = PBJ_ERR_INVALID; break; }
            if (type != PBJ_STRING) reg_extent(s, &p.reg);
            break; }
        case OP_TEXTURE: {
            PbjParam p; memset(&p, 0, sizeof(p));
            p.texture = 1; p.tex_index = (int)rd8(r); p.channels = (int)rd8(r); p.name = rdstr(r);
            if (r->eof) { free(p.name); break; }
            apply_meta(s, &ml);
            if (!push_param(s, &pcap, &p)) { free(p.name); err = PBJ_ERR_INVALID; }
            break; }
        case OP_NAME: {
            u32 len = rd16(r);
            if (r->eof) break;
            if (r->pos + len > r->n) { r->pos = r->n; r->eof = 1; break; }
            if (!utf8_valid(r->p + r->pos, len)) { err = PBJ_ERR_INVALID; break; }
            free(s->name); s->name = (char*)malloc(len + 1);
            if (s->name) { memcpy(s->name, r->p + r->pos, len); s->name[len] = 0; }
            r->pos += len;
            break; }
        case OP_VERSION:
            s->version = (s32)rd32(r);
            break;
        case OP_IF: {
            u32 src;
            skip(r, 3); src = rd24(r); skip(r, 1);
            if (r->eof) break;
            src_reg(src, 1, &op.src);
            if (!op.src.is_int) { err = PBJ_ERR_INVALID; break; }
            op.kind = PBJ_OP_IF; op.opcode = (int)raw;
            break; }
        case OP_ELSE: skip(r, 7); op.kind = PBJ_OP_ELSE; break;
        case OP_ENDIF: skip(r, 7); op.kind = PBJ_OP_ENDIF; break;
        case OP_LOADIORF: {
            u32 d = rd16(r), mask = rd8(r);
            dst_reg(d, mask >> 4, &op.dst);
            if (op.dst.is_int) { op.kind = PBJ_OP_LOADI; op.iv = (s32)rd32(r); }
            else { op.kind = PBJ_OP_LOADF; op.fv = rdfloat(r); }
            break; }
        case OP_SAMPLENEAREST: case OP_SAMPLELINEAR: {
            u32 d = rd16(r), mask = rd8(r), src = rd24(r);
            op.tf = (int)rd8(r);
            dst_reg(d, mask >> 4, &op.dst); src_reg(src, 2, &op.src);
            op.kind = raw == OP_SAMPLENEAREST ? PBJ_OP_SAMPLE_NEAREST : PBJ_OP_SAMPLE_LINEAR;
            break; }
        case OP_SELECT: {
            u32 d = rd16(r), mask = rd8(r), c, s1, s2;
            c = rd24(r); skip(r, 1); s1 = rd24(r); skip(r, 1); s2 = rd24(r); skip(r, 1);
            if (r->eof) break;
            dst_reg(d, mask >> 4, &op.dst); src_reg(c, 1, &op.cond); src_reg(s1, 1, &op.src); src_reg(s2, 1, &op.src2);
            if (!op.cond.is_int || op.dst.is_int != op.src.is_int || op.src.is_int != op.src2.is_int) { err = PBJ_ERR_INVALID; break; }
            op.kind = PBJ_OP_SELECT;
            break; }
        default: {
            u32 d, mask, src, size, matrix;
            if (raw > OP_ALL) { err = PBJ_ERR_INVALID; break; }
            d = rd16(r); mask = rd8(r); src = rd24(r); skip(r, 1);
            if (r->eof) break;
            size = (mask & 3) + 1; matrix = (mask >> 2) & 3; mask >>= 4;
            if (matrix) {
                if (mask == 0) mat_reg(d, (int)matrix + 1, &op.dst); else dst_reg(d, mask, &op.dst);
                mat_reg(src & 0xFFFF, (int)matrix + 1, &op.src);
            } else { dst_reg(d, mask, &op.dst); src_reg(src, (int)size, &op.src); }
            if ((raw == OP_CEIL || raw == OP_SIGN) && (op.dst.is_int || op.src.is_int)) { err = PBJ_ERR_INVALID; break; }
            op.kind = PBJ_OP_NORMAL; op.opcode = (int)raw;
            break; }
        }
        if (err) break;
        if (r->eof) { err = PBJ_ERR_EOF; break; }
        if (op.kind >= 0) {
            reg_extent(s, &op.dst); reg_extent(s, &op.src);
            if (op.kind == PBJ_OP_SELECT) { reg_extent(s, &op.src2); reg_extent(s, &op.cond); }
            if (!push_op(s, &opcap, &op)) { err = PBJ_ERR_INVALID; break; }
        }
    }
    if (err) { meta_free(ml.m, ml.n); return err; }
    apply_meta(s, &ml);
    if (pbj_output_param(s) < 0) return PBJ_ERR_INVALID;
    /* match if / else / endif */
    {
        int *stack = (int*)malloc((size_t)(s->nops + 1) * sizeof(int)), sp = 0, i;
        if (!stack) return PBJ_ERR_INVALID;
        for (i = 0; i < s->nops; i++) {
            PbjOp *o = &s->ops[i];
            if (o->kind == PBJ_OP_IF) { stack[sp++] = i; o->jump = -1; }
            else if (o->kind == PBJ_OP_ELSE) {
                if (!sp || s->ops[stack[sp - 1]].jump != -1) { free(stack); return PBJ_ERR_INVALID; }
                s->ops[stack[sp - 1]].jump = i + 1; stack[sp - 1] = -2 - i;   /* now waiting for endif */
            } else if (o->kind == PBJ_OP_ENDIF) {
                int t;
                if (!sp) { free(stack); return PBJ_ERR_INVALID; }
                t = stack[--sp];
                if (t >= 0) s->ops[t].jump = i + 1;
                else s->ops[-2 - t].jump = i + 1;
            }
        }
        free(stack);
        if (sp) return PBJ_ERR_INVALID;
    }
    if (s->nfreg < 1) s->nfreg = 1;
    if (s->nireg < 1) s->nireg = 1;
    return PBJ_OK;
}

int pbj_parse(const u8 *data, size_t len, PbjShader **out) {
    PR r; PbjShader *s = (PbjShader*)calloc(1, sizeof(PbjShader)); int err;
    *out = NULL;
    if (!s) return PBJ_ERR_INVALID;
    s->refs = 1;
    r.p = data; r.n = len; r.pos = 0; r.eof = 0;
    err = parse_body(&r, s);
    if (err) { shader_free(s); return err; }
    if (!s->name) { s->name = (char*)malloc(1); if (s->name) s->name[0] = 0; }
    *out = s;
    return PBJ_OK;
}

/* ---------------------------------------------------------------- */
/* interpreter                                                      */

typedef struct { int isint; float f[4]; s32 i[4]; } Vec;
typedef struct { int n; float m[16]; } Mat;     /* column-major m[c * n + r] */

typedef struct {
    const PbjShader *s;
    float *fr; s32 *ir;
    const PbjTex *tex; int ntex; int zero_oob;
} Ctx;

static float f_inf(void) { return (float)HUGE_VAL; }
static float f_nan(void) { static float n; static int init = 0; if (!init) { double z = 0.0; n = (float)(z / z); init = 1; } return n; }

static s32 f2i(float v) {
    if (v != v) return 0;
    if (v >= 2147483520.0f) return 2147483647;
    if (v <= -2147483648.0f) return (s32)0x80000000u;
    return (s32)v;
}
static float round_even(float v) {
    double r = floor((double)v + 0.5);
    if (v != v) return v;
    if (r - (double)v == 0.5 && fmod(r, 2.0) != 0.0) r -= 1.0;
    return (float)r;
}

static void load_vec(Ctx *c, const PbjReg *r, Vec *v) {
    int k;
    if (r->is_int) {
        const s32 *R = c->ir + (size_t)r->index * 4;
        v->isint = 1;
        for (k = 0; k < 4; k++) { v->i[k] = R[k < r->n ? r->ch[k] : 3]; v->f[k] = (float)v->i[k]; }
    } else {
        const float *R = c->fr + (size_t)r->index * 4;
        v->isint = 0;
        for (k = 0; k < 4; k++) { v->f[k] = R[k < r->n ? r->ch[k] : 3]; v->i[k] = 0; }
    }
}
static void to_float(Vec *v) { int k; if (v->isint) { for (k = 0; k < 4; k++) v->f[k] = (float)v->i[k]; v->isint = 0; } }
static void store_vec(Ctx *c, const PbjReg *r, const Vec *v) {
    int k;
    if (r->mat) return;
    if (r->is_int) { s32 *R = c->ir + (size_t)r->index * 4; for (k = 0; k < r->n; k++) R[r->ch[k]] = v->isint ? v->i[k] : f2i(v->f[k]); }
    else { float *R = c->fr + (size_t)r->index * 4; for (k = 0; k < r->n; k++) R[r->ch[k]] = v->isint ? (float)v->i[k] : v->f[k]; }
}
static void load_mat(Ctx *c, int index, int n, Mat *m) {
    int col, row;
    m->n = n;
    if (n == 2) { const float *R = c->fr + (size_t)index * 4; for (row = 0; row < 4; row++) m->m[row] = R[row]; return; }
    for (col = 0; col < n; col++) { const float *R = c->fr + (size_t)(index + col) * 4; for (row = 0; row < n; row++) m->m[col * n + row] = R[row]; }
}
static void store_mat(Ctx *c, int index, const Mat *m) {
    int col, row, n = m->n;
    if (n == 2) { float *R = c->fr + (size_t)index * 4; for (row = 0; row < 4; row++) R[row] = m->m[row]; return; }
    for (col = 0; col < n; col++) { float *R = c->fr + (size_t)(index + col) * 4; for (row = 0; row < n; row++) R[row] = m->m[col * n + row]; }
}

/* one texel as the GPU reads it (premultiplied for bitmaps) */
static void texel(const PbjTex *t, int x, int y, float out[4]) {
    if (t->f) {
        const float *p = t->f + ((size_t)y * t->w + x) * (size_t)t->ch;
        out[0] = p[0]; out[1] = t->ch > 1 ? p[1] : 0.0f; out[2] = t->ch > 2 ? p[2] : 0.0f;
        out[3] = t->ch > 3 ? p[3] : t->ch == 3 ? 0.0f : 1.0f;
        return;
    }
    x -= t->ox; y -= t->oy;
    if (!t->px || x < 0 || y < 0 || x >= t->pw || y >= t->ph) { out[0] = out[1] = out[2] = out[3] = 0.0f; return; }
    {
        u32 v = t->px[(size_t)y * t->pw + x];
        out[0] = ((v >> 16) & 255) / 255.0f; out[1] = ((v >> 8) & 255) / 255.0f; out[2] = (v & 255) / 255.0f; out[3] = (v >> 24) / 255.0f;
    }
}
static int clampi(double v, int hi) { if (v != v || v < 0) return 0; if (v > hi) return hi; return (int)v; }
static void sample(Ctx *c, int tf, float x, float y, int linear, float out[4]) {
    const PbjTex *t = tf >= 0 && tf < c->ntex ? &c->tex[tf] : NULL; double u, v;
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (!t || !t->present || t->w <= 0 || t->h <= 0) return;
    u = (double)x / t->w; v = (double)y / t->h;
    if (c->zero_oob && (u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0)) return;
    if (!linear) { texel(t, clampi(floor((double)x), t->w - 1), clampi(floor((double)y), t->h - 1), out); return; }
    {
        double fx = (double)x - 0.5, fy = (double)y - 0.5, ax, ay; float a[4], b[4], cc[4], d[4]; int x0, y0, x1, y1, k;
        if (fx != fx) fx = 0; if (fy != fy) fy = 0;
        ax = floor(fx); ay = floor(fy);
        x0 = clampi(ax, t->w - 1); x1 = clampi(ax + 1, t->w - 1); y0 = clampi(ay, t->h - 1); y1 = clampi(ay + 1, t->h - 1);
        fx -= ax; fy -= ay;
        texel(t, x0, y0, a); texel(t, x1, y0, b); texel(t, x0, y1, cc); texel(t, x1, y1, d);
        for (k = 0; k < 4; k++) out[k] = (float)((a[k] * (1 - fx) + b[k] * fx) * (1 - fy) + (cc[k] * (1 - fx) + d[k] * fx) * fy);
    }
}

static float fsign(float v) { return v > 0 ? 1.0f : v < 0 ? -1.0f : 0.0f; }

static void op_normal(Ctx *c, const PbjOp *o) {
    Vec a, b, r; int k, n, both_int; const PbjReg *dst = &o->dst; PbjReg cmp;
    /* matrix forms */
    if (o->src.mat) {
        Mat M; int dim = o->src.mat;
        load_mat(c, o->src.index, dim, &M);
        if (o->dst.mat) {
            Mat L, P; int i, j, q;
            if (o->opcode == OP_MOV) { store_mat(c, o->dst.index, &M); return; }
            load_mat(c, o->dst.index, dim, &L); P.n = dim;
            for (i = 0; i < dim * dim; i++) P.m[i] = L.m[i];
            switch (o->opcode) {
            case OP_MUL: case OP_MATMATMUL:
                for (j = 0; j < dim; j++) for (i = 0; i < dim; i++) { double s = 0; for (q = 0; q < dim; q++) s += (double)L.m[q * dim + i] * M.m[j * dim + q]; P.m[j * dim + i] = (float)s; }
                break;
            case OP_ADD: for (i = 0; i < dim * dim; i++) P.m[i] = L.m[i] + M.m[i]; break;
            case OP_SUB: for (i = 0; i < dim * dim; i++) P.m[i] = L.m[i] - M.m[i]; break;
            case OP_DIV: for (i = 0; i < dim * dim; i++) P.m[i] = L.m[i] / M.m[i]; break;
            default: break;
            }
            store_mat(c, o->dst.index, &P);
            return;
        }
        load_vec(c, dst, &a); to_float(&a);
        memset(&r, 0, sizeof(r));
        n = dst->n < dim ? dst->n : dim;
        if (o->opcode == OP_MATVECMUL) {
            int row, col;
            for (row = 0; row < dim && row < 4; row++) { double s = 0; for (col = 0; col < n; col++) s += (double)M.m[col * dim + row] * a.f[col]; r.f[row] = (float)s; }
        } else {
            int row, col;
            for (col = 0; col < dim && col < 4; col++) { double s = 0; for (row = 0; row < n; row++) s += (double)a.f[row] * M.m[col * dim + row]; r.f[col] = (float)s; }
        }
        store_vec(c, dst, &r);
        return;
    }
    load_vec(c, &o->src, &b);
    load_vec(c, dst, &a);
    both_int = a.isint && b.isint;
    memset(&r, 0, sizeof(r));
    switch (o->opcode) {
    case OP_NOP: return;
    case OP_MOV: r = b; break;
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: case OP_MIN: case OP_MAX:
        if (both_int) {
            r.isint = 1;
            for (k = 0; k < 4; k++) {
                s32 x = a.i[k], y = b.i[k];
                switch (o->opcode) {
                case OP_ADD: r.i[k] = (s32)((u32)x + (u32)y); break;
                case OP_SUB: r.i[k] = (s32)((u32)x - (u32)y); break;
                case OP_MUL: r.i[k] = (s32)((u32)x * (u32)y); break;
                case OP_DIV: r.i[k] = y == 0 ? x : (x == (s32)0x80000000u && y == -1) ? x : x / y; break;
                case OP_MOD: r.i[k] = y == 0 || (x == (s32)0x80000000u && y == -1) ? 0 : x % y; break;
                case OP_MIN: r.i[k] = x < y ? x : y; break;
                default: r.i[k] = x > y ? x : y; break;
                }
            }
        } else {
            to_float(&a); to_float(&b);
            for (k = 0; k < 4; k++) {
                float x = a.f[k], y = b.f[k];
                switch (o->opcode) {
                case OP_ADD: r.f[k] = x + y; break;
                case OP_SUB: r.f[k] = x - y; break;
                case OP_MUL: r.f[k] = x * y; break;
                case OP_DIV: r.f[k] = x / y; break;
                case OP_MOD: r.f[k] = (float)((double)x - (double)y * floor((double)x / y)); break;
                case OP_MIN: r.f[k] = y < x ? y : x; break;
                default: r.f[k] = y > x ? y : x; break;
                }
            }
        }
        break;
    case OP_RCP: to_float(&b); for (k = 0; k < 4; k++) r.f[k] = 1.0f / b.f[k]; break;
    case OP_ATAN2: to_float(&a); to_float(&b); for (k = 0; k < 4; k++) r.f[k] = (float)atan2(a.f[k], b.f[k]); break;
    case OP_POW: to_float(&a); to_float(&b); for (k = 0; k < 4; k++) r.f[k] = (a.f[k] < 0 && b.f[k] != floor(b.f[k])) ? f_nan() : (float)pow(a.f[k], b.f[k]); break;
    case OP_STEP: to_float(&a); to_float(&b); for (k = 0; k < 4; k++) r.f[k] = a.f[k] >= b.f[k] ? 1.0f : 0.0f; break;
    case OP_SIN: case OP_COS: case OP_TAN: case OP_ASIN: case OP_ACOS: case OP_ATAN: case OP_EXP: case OP_EXP2:
    case OP_LOG: case OP_LOG2: case OP_SQRT: case OP_RSQRT: case OP_FLOOR: case OP_CEIL: case OP_FRACT: case OP_SIGN:
        to_float(&b);
        for (k = 0; k < 4; k++) {
            double x = b.f[k], y;
            switch (o->opcode) {
            case OP_SIN: y = sin(x); break;
            case OP_COS: y = cos(x); break;
            case OP_TAN: y = tan(x); break;
            case OP_ASIN: y = x < -1 || x > 1 ? f_nan() : asin(x); break;
            case OP_ACOS: y = x < -1 || x > 1 ? f_nan() : acos(x); break;
            case OP_ATAN: y = atan(x); break;
            case OP_EXP: y = exp(x); break;
            case OP_EXP2: y = pow(2.0, x); break;
            case OP_LOG: y = x < 0 ? f_nan() : x == 0 ? -f_inf() : log(x); break;
            case OP_LOG2: y = x < 0 ? f_nan() : x == 0 ? -f_inf() : log(x) / log(2.0); break;
            case OP_SQRT: y = x < 0 ? f_nan() : sqrt(x); break;
            case OP_RSQRT: y = x < 0 ? f_nan() : x == 0 ? f_inf() : 1.0 / sqrt(x); break;
            case OP_FLOOR: y = floor(x); break;
            case OP_CEIL: y = ceil(x); break;
            case OP_FRACT: y = x - floor(x); break;
            default: y = fsign((float)x); break;
            }
            r.f[k] = (float)y;
        }
        break;
    case OP_ABS:
        if (b.isint) { r.isint = 1; for (k = 0; k < 4; k++) r.i[k] = b.i[k] < 0 ? (s32)(0u - (u32)b.i[k]) : b.i[k]; }
        else for (k = 0; k < 4; k++) r.f[k] = (float)fabs(b.f[k]);
        break;
    case OP_FTOI:
        if (b.isint) r = b; else for (k = 0; k < 4; k++) r.f[k] = round_even(b.f[k]);
        break;
    case OP_ITOF: to_float(&b); r = b; break;
    case OP_NORMALIZE: case OP_LENGTH: {
        double s = 0; to_float(&b); n = o->src.n;
        for (k = 0; k < n; k++) s += (double)b.f[k] * b.f[k];
        s = sqrt(s);
        if (o->opcode == OP_LENGTH) for (k = 0; k < 4; k++) r.f[k] = (float)s;
        else for (k = 0; k < 4; k++) r.f[k] = (float)(n == 1 ? (b.f[0] / s) : (b.f[k] / s));
        break; }
    case OP_DISTANCE: case OP_DOT: {
        double s = 0; to_float(&a); to_float(&b); n = o->src.n < dst->n ? o->src.n : dst->n;
        for (k = 0; k < n; k++) { double d = o->opcode == OP_DOT ? (double)a.f[k] * b.f[k] : (double)a.f[k] - b.f[k]; s += o->opcode == OP_DOT ? d : d * d; }
        if (o->opcode == OP_DISTANCE) s = sqrt(s);
        for (k = 0; k < 4; k++) r.f[k] = (float)s;
        break; }
    case OP_CROSS:
        to_float(&a); to_float(&b);
        r.f[0] = a.f[1] * b.f[2] - a.f[2] * b.f[1]; r.f[1] = a.f[2] * b.f[0] - a.f[0] * b.f[2]; r.f[2] = a.f[0] * b.f[1] - a.f[1] * b.f[0];
        break;
    case OP_EQUAL: case OP_NOTEQUAL: case OP_LESS: case OP_LESSEQUAL: {
        int t;
        if (both_int) {
            s32 x = a.i[0], y = b.i[0];
            t = o->opcode == OP_EQUAL ? x == y : o->opcode == OP_NOTEQUAL ? x != y : o->opcode == OP_LESS ? x < y : x <= y;
        } else {
            float x, y; to_float(&a); to_float(&b); x = a.f[0]; y = b.f[0];
            t = o->opcode == OP_EQUAL ? x == y : o->opcode == OP_NOTEQUAL ? x != y : o->opcode == OP_LESS ? x < y : x <= y;
        }
        memset(&cmp, 0, sizeof(cmp)); cmp.is_int = 1; cmp.n = 1; cmp.ch[0] = 0;
        r.isint = 1; r.i[0] = t;
        store_vec(c, &cmp, &r);
        return; }
    case OP_VEQUAL: case OP_VNOTEQUAL: {
        int all = 1; n = o->src.n;
        for (k = 0; k < n; k++) { int e = both_int ? a.i[k] == b.i[k] : a.f[k] == b.f[k]; if (!e) all = 0; }
        if (o->opcode == OP_VNOTEQUAL) all = !all;
        memset(&cmp, 0, sizeof(cmp)); cmp.is_int = 1; cmp.n = 1; cmp.ch[0] = 0;
        r.isint = 1; r.i[0] = all;
        store_vec(c, &cmp, &r);
        return; }
    case OP_ANY: case OP_ALL: {
        int any = 0, all = 1; n = o->src.n;
        for (k = 0; k < n; k++) { int t = b.isint ? b.i[k] != 0 : b.f[k] != 0; if (t) any = 1; else all = 0; }
        r.isint = 1; for (k = 0; k < 4; k++) r.i[k] = o->opcode == OP_ANY ? any : all;
        break; }
    case OP_LNOT:
        r.isint = 1; for (k = 0; k < 4; k++) r.i[k] = ~(b.isint ? b.i[k] : f2i(b.f[k]));
        break;
    case OP_LAND: case OP_LOR: case OP_LXOR:
        for (k = 0; k < 4; k++) {
            int x = a.isint ? a.i[k] != 0 : a.f[k] != 0, y = b.isint ? b.i[k] != 0 : b.f[k] != 0;
            r.f[k] = (o->opcode == OP_LAND ? (x && y) : o->opcode == OP_LOR ? (x || y) : (x != y)) ? 1.0f : 0.0f;
        }
        break;
    case OP_FTOB:
        r.isint = 1; for (k = 0; k < 4; k++) r.i[k] = b.isint ? b.i[k] != 0 : b.f[k] != 0;
        break;
    case OP_BTOF: break;   /* Flash Player gives 0 */
    case OP_ITOB: case OP_BTOI: r = b; break;
    default: return;
    }
    store_vec(c, dst, &r);
}

static void run_ops(Ctx *c) {
    const PbjShader *s = c->s; int pc = 0, guard = 0;
    while (pc < s->nops && guard++ < 1000000) {
        const PbjOp *o = &s->ops[pc];
        switch (o->kind) {
        case PBJ_OP_NORMAL: op_normal(c, o); break;
        case PBJ_OP_LOADF: { Vec v; int k; v.isint = 0; for (k = 0; k < 4; k++) { v.f[k] = o->fv; v.i[k] = 0; } store_vec(c, &o->dst, &v); break; }
        case PBJ_OP_LOADI: { Vec v; int k; v.isint = 1; for (k = 0; k < 4; k++) { v.i[k] = o->iv; v.f[k] = 0; } store_vec(c, &o->dst, &v); break; }
        case PBJ_OP_IF: {
            Vec v; load_vec(c, &o->src, &v);
            if (!(v.isint ? v.i[0] != 0 : v.f[0] != 0)) { pc = o->jump; continue; }
            break; }
        case PBJ_OP_ELSE: pc = o->jump; continue;
        case PBJ_OP_SELECT: {
            Vec cv, v1, v2; load_vec(c, &o->cond, &cv); load_vec(c, &o->src, &v1); load_vec(c, &o->src2, &v2);
            store_vec(c, &o->dst, (cv.isint ? cv.i[0] == 1 : cv.f[0] == 1.0f) ? &v1 : &v2);
            break; }
        case PBJ_OP_SAMPLE_NEAREST: case PBJ_OP_SAMPLE_LINEAR: {
            Vec co, v; int k;
            load_vec(c, &o->src, &co); to_float(&co);
            v.isint = 0; sample(c, o->tf, co.f[0], co.f[1], o->kind == PBJ_OP_SAMPLE_LINEAR, v.f);
            for (k = 0; k < 4; k++) v.i[k] = 0;
            store_vec(c, &o->dst, &v);
            break; }
        default: break;
        }
        pc++;
    }
}

static void store_param(Ctx *c, const PbjParam *p, const PbjValue *val) {
    int k, n = pbj_type_count(p->type);
    if (p->type == PBJ_STRING) return;
    if (p->reg.mat) {
        Mat m; int dim = p->reg.mat;
        m.n = dim;
        for (k = 0; k < dim * dim; k++) m.m[k] = val && pbj_type_is_float(val->type) ? val->f[k] : 0.0f;
        store_mat(c, p->reg.index, &m);
        return;
    }
    {
        Vec v; int isf = pbj_type_is_float(p->type);
        v.isint = !isf;
        for (k = 0; k < 4; k++) {
            if (isf) { v.f[k] = val && k < n ? val->f[k] : 0.0f; v.i[k] = 0; }
            else { v.i[k] = val && k < n ? val->i[k] : 0; v.f[k] = 0; }
        }
        store_vec(c, &p->reg, &v);
    }
}

int pbj_run(const PbjShader *s, const PbjValue *vals, const PbjTex *tex, int ntex, int x0, int y0, int w, int h, int zero_oob, float *out) {
    Ctx c; int x, y, i, outp = pbj_output_param(s), oc = -1; size_t nf, ni;
    if (outp < 0 || w <= 0 || h <= 0) return 1;
    nf = (size_t)s->nfreg * 4; ni = (size_t)s->nireg * 4;
    c.s = s; c.tex = tex; c.ntex = ntex; c.zero_oob = zero_oob;
    c.fr = (float*)calloc(nf, sizeof(float)); c.ir = (s32*)calloc(ni, sizeof(s32));
    if (!c.fr || !c.ir) { free(c.fr); free(c.ir); return 0; }
    for (i = 0; i < s->nparams; i++) if (pbj_is_outcoord(&s->params[i]) && !s->params[i].out) oc = i;
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
        Vec v; float *o = out + ((size_t)y * w + x) * 4; const PbjParam *op = &s->params[outp];
        memset(c.fr, 0, nf * sizeof(float)); memset(c.ir, 0, ni * sizeof(s32));
        for (i = 0; i < s->nparams; i++) {
            const PbjParam *p = &s->params[i];
            if (p->texture || p->out || i == oc) continue;
            store_param(&c, p, vals ? &vals[i] : NULL);
        }
        if (oc >= 0) {
            v.isint = 0; v.f[0] = (float)(x0 + x) + 0.5f; v.f[1] = (float)(y0 + y) + 0.5f; v.f[2] = 0.0f; v.f[3] = 1.0f;
            v.i[0] = v.i[1] = v.i[2] = v.i[3] = 0;
            store_vec(&c, &s->params[oc].reg, &v);
        }
        run_ops(&c);
        if (op->reg.mat) { Mat m; load_mat(&c, op->reg.index, op->reg.mat, &m); for (i = 0; i < 4; i++) o[i] = m.m[i]; continue; }
        load_vec(&c, &op->reg, &v); to_float(&v);
        o[0] = v.f[0]; o[1] = v.f[1]; o[2] = v.f[2]; o[3] = v.f[3];
        if (op->reg.n < 4) o[3] = 1.0f;
    }
    free(c.fr); free(c.ir);
    return 1;
}

u8 pbj_unorm8(float v) {
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f) return 255;
    return (u8)(int)floor((double)v * 255.0 + 0.5);
}

/* ---------------------------------------------------------------- */
/* jobs (filters, blend shaders)                                    */

PbjJob *pbj_job_new(PbjShader *s) {
    PbjJob *j = (PbjJob*)calloc(1, sizeof(PbjJob));
    if (!j) return NULL;
    j->refs = 1; j->shader = pbj_ref(s);
    j->vals = (PbjValue*)calloc((size_t)(s->nparams ? s->nparams : 1), sizeof(PbjValue));
    if (!j->vals) { pbj_unref(s); free(j); return NULL; }
    return j;
}
PbjJob *pbj_job_ref(PbjJob *j) { if (j) j->refs++; return j; }
void pbj_job_unref(PbjJob *j) {
    int i;
    if (!j || --j->refs > 0) return;
    for (i = 0; i < j->shader->nparams; i++) pbj_value_free(&j->vals[i]);
    for (i = 0; i < 16; i++) free(j->own[i]);
    free(j->vals);
    pbj_unref(j->shader);
    free(j);
}

static int first_texture(const PbjShader *s) {
    int i;
    for (i = 0; i < s->nparams; i++) if (s->params[i].texture) return s->params[i].tex_index & 15;
    return -1;
}

void pbj_filter_layer(PbjJob *j, u32 *px, int w, int h) {
    PbjTex tex[16]; float *out; int t, i; size_t n;
    if (!j || !px || w <= 0 || h <= 0 || (double)w * h > 16777216.0) return;
    n = (size_t)w * h;
    out = (float*)malloc(n * 4 * sizeof(float));
    if (!out) return;
    memcpy(tex, j->tex, sizeof(tex));
    t = first_texture(j->shader);
    if (t >= 0) { memset(&tex[t], 0, sizeof(PbjTex)); tex[t].present = 1; tex[t].w = tex[t].pw = w; tex[t].h = tex[t].ph = h; tex[t].px = px; }
    if (pbj_run(j->shader, j->vals, tex, 16, 0, 0, w, h, 1, out))
        for (i = 0; i < (int)n; i++) {
            const float *o = out + (size_t)i * 4;
            px[i] = ((u32)pbj_unorm8(o[3]) << 24) | ((u32)pbj_unorm8(o[0]) << 16) | ((u32)pbj_unorm8(o[1]) << 8) | pbj_unorm8(o[2]);
        }
    free(out);
}

void pbj_blend_layer(PbjJob *j, u32 *surf, int sw, int sh, const u32 *layer, int lx, int ly, int lw, int lh) {
    PbjTex tex[16]; float *out; u32 *bg; int x, y, x0, y0, x1, y1, w, h; size_t n, i;
    if (!j || !surf || !layer || sw <= 0 || sh <= 0) return;
    x0 = lx < 0 ? 0 : lx; y0 = ly < 0 ? 0 : ly; x1 = lx + lw > sw ? sw : lx + lw; y1 = ly + lh > sh ? sh : ly + lh;
    if (x1 <= x0 || y1 <= y0) return;
    w = x1 - x0; h = y1 - y0;
    n = (size_t)sw * sh;
    bg = (u32*)malloc(n * 4); out = (float*)malloc((size_t)w * h * 4 * sizeof(float));
    if (!bg || !out) { free(bg); free(out); return; }
    for (i = 0; i < n; i++) bg[i] = surf[i];
    memcpy(tex, j->tex, sizeof(tex));
    memset(&tex[0], 0, sizeof(PbjTex)); tex[0].present = 1; tex[0].w = tex[0].pw = sw; tex[0].h = tex[0].ph = sh; tex[0].px = bg;
    memset(&tex[1], 0, sizeof(PbjTex)); tex[1].present = 1; tex[1].w = sw; tex[1].h = sh; tex[1].px = layer; tex[1].pw = lw; tex[1].ph = lh; tex[1].ox = lx; tex[1].oy = ly;
    if (pbj_run(j->shader, j->vals, tex, 16, x0, y0, w, h, 1, out))
        for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
            const float *o = out + ((size_t)y * w + x) * 4; u32 *d = &surf[(size_t)(y0 + y) * sw + x0 + x];
            /* Flash: the kernel's (premultiplied) result replaces the pixel */
            u32 ca = pbj_unorm8(o[3]), cr = pbj_unorm8(o[0]), cg = pbj_unorm8(o[1]), cb = pbj_unorm8(o[2]);
            if (cr > ca) cr = ca; if (cg > ca) cg = ca; if (cb > ca) cb = ca;
            *d = (ca << 24) | (cr << 16) | (cg << 8) | cb;
        }
    free(bg); free(out);
}
