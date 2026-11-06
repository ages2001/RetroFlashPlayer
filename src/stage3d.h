/* stage3d.h - software Stage3D: Context3D state, AGAL bytecode parser and
 * interpreter, triangle rasterizer.  Independent of the AVM; the
 * ActionScript classes live in avm2_stage3d.c.
 *
 * The behaviour follows Ruffle's wgpu Context3D backend
 * (render/wgpu/src/context3d) and its AGAL front end (render/naga-agal),
 * running the programs on the CPU instead of a GPU. */
#ifndef FLASHNT_STAGE3D_H
#define FLASHNT_STAGE3D_H

#include "flashnt.h"

#define S3D_MAX_VC   256     /* vertex program constants */
#define S3D_MAX_FC   64      /* fragment program constants */
#define S3D_MAX_TEMP 32
#define S3D_MAX_VARY 10
#define S3D_MAX_ATTR 8
#define S3D_MAX_SAMP 8

enum { S3D_FMT_BGRA = 0, S3D_FMT_BGRA_PACKED, S3D_FMT_BGR_PACKED, S3D_FMT_COMPRESSED, S3D_FMT_COMPRESSED_ALPHA, S3D_FMT_RGBA_HALF };
enum { S3D_VF_FLOAT1 = 0, S3D_VF_FLOAT2, S3D_VF_FLOAT3, S3D_VF_FLOAT4, S3D_VF_BYTES4 };
enum { S3D_BF_ZERO = 0, S3D_BF_ONE, S3D_BF_SRC_COLOR, S3D_BF_ONE_MINUS_SRC_COLOR, S3D_BF_SRC_ALPHA, S3D_BF_ONE_MINUS_SRC_ALPHA,
       S3D_BF_DST_COLOR, S3D_BF_ONE_MINUS_DST_COLOR, S3D_BF_DST_ALPHA, S3D_BF_ONE_MINUS_DST_ALPHA };
enum { S3D_CMP_NEVER = 0, S3D_CMP_LESS, S3D_CMP_EQUAL, S3D_CMP_LESS_EQUAL, S3D_CMP_GREATER, S3D_CMP_NOT_EQUAL, S3D_CMP_GREATER_EQUAL, S3D_CMP_ALWAYS };
enum { S3D_FACE_NONE = 0, S3D_FACE_FRONT, S3D_FACE_BACK, S3D_FACE_BOTH };
enum { S3D_ST_KEEP = 0, S3D_ST_ZERO, S3D_ST_SET, S3D_ST_INC_SAT, S3D_ST_DEC_SAT, S3D_ST_INVERT, S3D_ST_INC_WRAP, S3D_ST_DEC_WRAP };
enum { S3D_WRAP_CLAMP = 0, S3D_WRAP_REPEAT, S3D_WRAP_CLAMP_U_REPEAT_V, S3D_WRAP_REPEAT_U_CLAMP_V };
enum { S3D_FILTER_NEAREST = 0, S3D_FILTER_LINEAR, S3D_FILTER_ANISO2, S3D_FILTER_ANISO4, S3D_FILTER_ANISO8, S3D_FILTER_ANISO16 };
enum { S3D_TEX_2D = 0, S3D_TEX_CUBE, S3D_TEX_RECT };

typedef struct S3DVBuf { int refs, pending; u32 nverts, d32; u8 *data; } S3DVBuf;          /* nverts * d32 * 4 bytes */
typedef struct S3DIBuf { int refs, pending; u32 n; u16 *data; } S3DIBuf;
typedef struct S3DTex { int refs, pending; int kind, w, h, format, nfaces; u8 *px[6]; } S3DTex;   /* RGBA8 per face */

typedef struct { u8 wrap, filter, mip; } S3DSampler;

typedef struct { u8 type, indirect, itype, isel, off; u16 reg; u8 swz[4]; u8 isconst, slot, islot, ident; } AgalSrc;   /* slot/islot: interpreter register (see stage3d.c) */
typedef struct {
    u8 op, dtype, dmask; u16 dreg; u8 dslot, single;
    AgalSrc a, b;
    u8 sreg, sdim, sfilter, smip, swrap, signore;   /* tex sampler field */
} AgalIns;
typedef struct { int type; int n; AgalIns *ins; u8 va_used; u16 vary_used; u8 samp_used; int has_deriv; } AgalProg;

/* a validation failure: the Flash error id, whether it is an ArgumentError,
   and the message arguments (%1 shader type, %2 operand / value, %3 token) */
typedef struct { int id, argerr; const char *shader; char a2[40]; int token; } AgalErr;

typedef struct S3DProg {
    int refs, pending, ok;
    AgalProg vp, fp;
    int has_samp[S3D_MAX_SAMP]; S3DSampler samp[S3D_MAX_SAMP];
} S3DProg;

typedef struct { int w, h, ns; u8 *color; float *depth; u8 *stencil; } S3DSurf;   /* color: ((y*w+x)*ns+s)*4 RGBA */

typedef struct { int cmp, fail, dfail, pass; } S3DStencilFace;

/* the state a draw uses (snapshots are shared by the queued draws) */
typedef struct S3DState {
    int refs;
    S3DVBuf *va_buf[S3D_MAX_ATTR]; u32 va_off[S3D_MAX_ATTR]; int va_fmt[S3D_MAX_ATTR];
    S3DProg *prog;
    float vc[S3D_MAX_VC * 4], fc[S3D_MAX_FC * 4];
    S3DTex *tex[S3D_MAX_SAMP];
    S3DSampler samp[S3D_MAX_SAMP];
    int blend_src, blend_dst;
    int cmask[4];
    int depth_mask, depth_cmp, cull;
    S3DStencilFace st_front, st_back; int st_front_on, st_back_on;
    u32 st_ref, st_rmask, st_wmask;
    int scissor_on; double sc_x, sc_y, sc_w, sc_h;
} S3DState;

/* a queued command: commands run when their result is needed (the
   presented frame is shown, drawToBitmapData, a resource they use is
   changed), so frames nobody sees are not rendered */
typedef struct S3DCmd {
    int kind, back;
    double cl[5]; u32 st, mask;
    S3DState *s; S3DIBuf *ib; u32 first; s32 ntri;
    S3DTex *t; int ds, face, aa;
} S3DCmd;

typedef struct S3DCtx {
    int refs, profile;
    int configured, aa, depth_stencil;
    /* the device */
    S3DSurf back;                 /* back buffer (multisampled) */
    u8 *front; int fw, fh;        /* presented, resolved RGBA */
    S3DSurf rtt; S3DTex *rtt_tex; int rtt_face;
    int to_texture;
    /* the API side: current state (fields may be set directly, then set `dirty`) */
    S3DState st;
    S3DState *snap; int dirty;
    S3DCmd *q; int nq, capq; int api_rtt;
    int cleared;
    struct S3DCtx *next_ctx;
} S3DCtx;

S3DCtx *s3d_ctx_new(int profile);
void s3d_ctx_unref(S3DCtx *c);
void s3d_configure(S3DCtx *c, int w, int h, int aa, int depth_stencil);
void s3d_clear(S3DCtx *c, double r, double g, double b, double a, double depth, u32 stencil, u32 mask);
void s3d_present(S3DCtx *c);
void s3d_set_render_to_texture(S3DCtx *c, S3DTex *t, int depth_stencil, int face, int aa);
void s3d_set_render_to_back_buffer(S3DCtx *c);
void s3d_set_vertex_buffer(S3DCtx *c, int i, S3DVBuf *b, u32 off, int fmt);
void s3d_set_program(S3DCtx *c, S3DProg *p);
void s3d_set_texture(S3DCtx *c, int i, S3DTex *t);
void s3d_set_constants(S3DCtx *c, int fragment, u32 first, const float *v, u32 nfloats);
/* returns 0 drawn, -1 skipped; *missing_stream = attribute read but not set (or -1) */
int  s3d_draw(S3DCtx *c, S3DIBuf *ib, u32 first, s32 ntri, int *missing_stream);
/* the back buffer resolved (RGBA, w*h*4, malloc'd) or NULL */
u8  *s3d_read_back(S3DCtx *c, int *w, int *h);
/* run the queued commands (all contexts); before changing a resource with `pending` set */
void s3d_flush(S3DCtx *c);
void s3d_flush_all(void);
#define S3D_BEFORE_WRITE(res) do { if ((res)->pending) s3d_flush_all(); } while (0)
/* the presented frame (after running what is queued) */
const u8 *s3d_front(S3DCtx *c);
/* before changing a buffer / texture: queued commands keep the old contents */
void s3d_vbuf_before_write(S3DVBuf *b);
void s3d_ibuf_before_write(S3DIBuf *b);
void s3d_tex_before_write(S3DTex *t);

S3DVBuf *s3d_vbuf_new(u32 nverts, u32 d32);
void s3d_vbuf_unref(S3DVBuf *b);
S3DIBuf *s3d_ibuf_new(u32 n);
void s3d_ibuf_unref(S3DIBuf *b);
S3DTex *s3d_tex_new(int kind, int w, int h, int format);
void s3d_tex_unref(S3DTex *t);
S3DProg *s3d_prog_new(void);
void s3d_prog_unref(S3DProg *p);
int  s3d_prog_upload(S3DProg *p, const u8 *vp, u32 vlen, const u8 *fp, u32 flen, AgalErr *err);   /* 1 ok */

/* ATF: header fields; 1 ok */
typedef struct { int w, h, cube, format, mips, version; const u8 *data; u32 len; } S3DAtf;
int  s3d_atf_parse(const u8 *b, u32 len, S3DAtf *out);
/* decode the first mip level of face `face` into RGBA (DXT1 / DXT5 raw formats); 1 ok */
int  s3d_atf_decode(const S3DAtf *a, int face, u8 *rgba);

#endif
