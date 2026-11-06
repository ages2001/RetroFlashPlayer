/* pbj.h - Pixel Bender (PBJ) bytecode: parser and CPU interpreter.
 *
 * Flash Player 10 runs Pixel Bender kernels for flash.display.Shader
 * (ShaderJob, ShaderFilter, blendShader).  The bytecode is parsed like
 * Ruffle's render/pixel_bender/src/parser.rs (itself after Haxe's
 * format.pbj), and executed per pixel with the semantics of Ruffle's
 * naga-pixelbender translation: every register is a 4-component float
 * or int vector, operations read the destination as their first operand,
 * results are written through the destination mask. */
#ifndef FLASHNT_PBJ_H
#define FLASHNT_PBJ_H

#include "flashnt.h"

/* parse results */
#define PBJ_OK          0
#define PBJ_ERR_EOF     1      /* EOFError #2030 */
#define PBJ_ERR_INVALID 2      /* ArgumentError #2004 */

/* parameter / metadata types */
enum {
    PBJ_FLOAT = 1, PBJ_FLOAT2, PBJ_FLOAT3, PBJ_FLOAT4, PBJ_FLOAT2X2, PBJ_FLOAT3X3, PBJ_FLOAT4X4,
    PBJ_INT, PBJ_INT2, PBJ_INT3, PBJ_INT4, PBJ_STRING, PBJ_BOOL, PBJ_BOOL2, PBJ_BOOL3, PBJ_BOOL4
};

typedef struct PbjValue {
    int   type;
    float f[16];        /* float types (matrices column-major) */
    int   i[4];         /* int / bool types (16-bit in the bytecode) */
    char *s;            /* PBJ_STRING (malloc'd, Latin-1 bytes) */
} PbjValue;

typedef struct PbjMeta { char *key; PbjValue v; } PbjMeta;

/* a register reference: index, int/float file, channels (0 r 1 g 2 b 3 a),
   or a matrix (mat = 2, 3, 4) */
typedef struct PbjReg { int index; int is_int; int n; int ch[4]; int mat; } PbjReg;

typedef struct PbjParam {
    int     texture;            /* 1: image input (PBJParamTexture) */
    int     out;                /* normal: output qualifier */
    int     type;               /* normal: PBJ_* */
    PbjReg  reg;
    char   *name;               /* Latin-1 bytes */
    PbjMeta *meta; int nmeta;
    int     tex_index, channels;   /* texture */
} PbjParam;

enum { PBJ_OP_NORMAL = 0, PBJ_OP_NOP, PBJ_OP_LOADF, PBJ_OP_LOADI, PBJ_OP_IF, PBJ_OP_ELSE, PBJ_OP_ENDIF,
       PBJ_OP_SAMPLE_NEAREST, PBJ_OP_SAMPLE_LINEAR, PBJ_OP_SELECT };

typedef struct PbjOp {
    int    kind, opcode;
    PbjReg dst, src, src2, cond;
    float  fv; s32 iv; int tf;
    int    jump;                /* IF: index after the matching ELSE (or ENDIF); ELSE: index after ENDIF */
} PbjOp;

typedef struct PbjShader {
    int       refs;
    char     *name;             /* UTF-8 */
    s32       version;
    PbjMeta  *meta; int nmeta;
    PbjParam *params; int nparams;
    PbjOp    *ops; int nops;
    int       nfreg, nireg;     /* register file sizes */
} PbjShader;

int  pbj_parse(const u8 *data, size_t len, PbjShader **out);
PbjShader *pbj_ref(PbjShader *s);
void pbj_unref(PbjShader *s);
int  pbj_output_param(const PbjShader *s);        /* the last output parameter, -1 none */
int  pbj_output_channels(const PbjShader *s);     /* 0 none */
const char *pbj_type_name(int type);
int  pbj_type_count(int type);                    /* values the type holds (matrices: 4, 9, 16) */
int  pbj_type_is_float(int type);
void pbj_value_free(PbjValue *v);
void pbj_value_copy(PbjValue *dst, const PbjValue *src);
int  pbj_is_outcoord(const PbjParam *p);

/* an image input: float pixels (ch per pixel) or a premultiplied 0xAARRGGBB
   window (pw x ph at ox, oy) inside a logical w x h image */
typedef struct PbjTex {
    int present;
    int w, h;
    int ch; const float *f;
    const u32 *px; int pw, ph, ox, oy;
} PbjTex;

/* Run the kernel over w x h pixels whose coordinates start at (x0, y0);
   vals[i] is parameter i's value (ignored for outputs, textures,
   _OutCoord), tex[k] the input with texture index k.  zero_oob: samples
   outside the image read transparent black (filters), else clamp.
   out receives w * h * 4 floats (the output with alpha 1 when it has
   fewer than 4 channels).  Returns 0 on failure (memory). */
int pbj_run(const PbjShader *s, const PbjValue *vals, const PbjTex *tex, int ntex, int x0, int y0, int w, int h, int zero_oob, float *out);

/* unit float -> byte as a Rgba8Unorm target stores it */
u8 pbj_unorm8(float v);

/* a ready-to-run snapshot (ShaderFilter / blendShader): the shader, its
   parameter values and its image inputs, copied when the script set them */
typedef struct PbjJob {
    int        refs;
    PbjShader *shader;
    PbjValue  *vals;            /* nparams */
    PbjTex     tex[16];
    void      *own[16];         /* buffers the job owns (tex data) */
    int        ext[4];          /* ShaderFilter extensions: left, top, right, bottom */
} PbjJob;

PbjJob *pbj_job_new(PbjShader *s);
PbjJob *pbj_job_ref(PbjJob *j);
void    pbj_job_unref(PbjJob *j);

/* ShaderFilter on a premultiplied layer (in place); the first image input
   is the layer */
void pbj_filter_layer(PbjJob *j, u32 *px, int w, int h);
/* blendShader: input 0 the surface (opaque 0x00RRGGBB, sw x sh), input 1
   the layer (premultiplied, lw x lh at lx, ly); the result replaces the
   surface pixels under the layer */
void pbj_blend_layer(PbjJob *j, u32 *surf, int sw, int sh, const u32 *layer, int lx, int ly, int lw, int lh);

#endif
