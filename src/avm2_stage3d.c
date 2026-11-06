/* avm2_stage3d.c - flash.display.Stage3D and flash.display3D.*, AVM2.
 *
 * Stage3D, Context3D, Program3D, VertexBuffer3D, IndexBuffer3D and the
 * textures (TextureBase, Texture, CubeTexture, RectangleTexture,
 * VideoTexture), following Ruffle's ActionScript and natives
 * (core/src/avm2/globals/flash/display3D, display/stage_3d.rs and
 * object/context3d_object.rs).  The contexts are software ones
 * (stage3d.c); a context requested by requestContext3D is created at the
 * end of the frame, after exitFrame, when context3DCreate is dispatched.
 * Presented back buffers are drawn under the display list at the Stage3D's
 * x/y (a2_stage3d_render). */
#include "avm2_flash_int.h"
#include "stage3d.h"
#include "bmd.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#define OK_ (!vm->throwing)

static A2Class *c_stage3d, *c_context3d, *c_program3d, *c_vbuf3d, *c_ibuf3d, *c_texbase, *c_texture, *c_cubetex, *c_recttex, *c_videotex;

BitmapData *a2_bitmapdata_of(A2Obj *o);
extern int a2_m3_raw(A2Obj *o, float *out);

/* native state of the objects (A2Obj.payload) */
enum { PK_STAGE = 0x53334401, PK_CTX, PK_VBUF, PK_IBUF, PK_PROG, PK_TEX };
typedef struct S3DStage {
    int pk; VM2 *vm; A2Obj *self; int index;
    double x, y; int visible;
    int requested, profile;
    S3DCtx *ctx;
} S3DStage;
typedef struct { int pk; S3DCtx *ctx; int error_checking; A2Obj *stage3d; } PCtx;
typedef struct { int pk; S3DVBuf *b; } PVBuf;
typedef struct { int pk; S3DIBuf *b; } PIBuf;
typedef struct { int pk; S3DProg *p; } PProg;
typedef struct { int pk; S3DTex *t; int format; } PTex;

#define MAX_STAGES 16
static S3DStage *g_stages[MAX_STAGES];

static void *payload(A2Obj *o, int pk) { return (o && o->payload && *(int*)o->payload == pk && o->kind == OK_PLAIN && o->cls && (o->cls == c_stage3d || o->cls == c_context3d || o->cls == c_program3d || o->cls == c_vbuf3d || o->cls == c_ibuf3d || a2_is_subclass(o->cls, c_texbase))) ? o->payload : NULL; }
static void *payload_v(V2 v, int pk) { return v.t == T_OBJ ? payload(v.u.o, pk) : NULL; }

int a2_stage3d_free_payload(A2Obj *o) {
    int pk, i;
    if (!o->payload || !o->cls || !c_stage3d) return 0;
    if (!(o->cls == c_stage3d || o->cls == c_context3d || o->cls == c_program3d || o->cls == c_vbuf3d || o->cls == c_ibuf3d || a2_is_subclass(o->cls, c_texbase))) return 0;
    pk = *(int*)o->payload;
    switch (pk) {
    case PK_STAGE: { S3DStage *s = (S3DStage*)o->payload; for (i = 0; i < MAX_STAGES; i++) if (g_stages[i] == s) g_stages[i] = NULL; s3d_ctx_unref(s->ctx); break; }
    case PK_CTX: s3d_ctx_unref(((PCtx*)o->payload)->ctx); break;
    case PK_VBUF: s3d_vbuf_unref(((PVBuf*)o->payload)->b); break;
    case PK_IBUF: s3d_ibuf_unref(((PIBuf*)o->payload)->b); break;
    case PK_PROG: s3d_prog_unref(((PProg*)o->payload)->p); break;
    case PK_TEX: s3d_tex_unref(((PTex*)o->payload)->t); break;
    default: return 0;
    }
    free(o->payload); o->payload = NULL;
    return 1;
}

/* ---------------------------------------------------------------- */
/* argument helpers                                                 */

/* a String parameter that must not be null; NULL after throwing #2007 */
static const char *str_arg(VM2 *vm, int argc, V2 *argv, int i, const char *def, const char *name) {
    V2 v;
    if (i >= argc) return def;
    v = argv[i];
    if (v.t == T_NULL || v.t == T_UNDEF) { a2_throw_id(vm, "TypeError", 2007, name, NULL); return NULL; }
    return v2_tostr(vm, v)->s;
}
static int lookup(const char *s, const char *const *names) { int i; for (i = 0; names[i]; i++) if (!strcmp(s, names[i])) return i; return -1; }
/* an enumerated String parameter: its index, or -1 after throwing */
static int enum_arg(VM2 *vm, int argc, V2 *argv, int i, const char *def, const char *name, const char *const *names) {
    const char *s = str_arg(vm, argc, argv, i, def, name); int k;
    if (!s) return -1;
    k = lookup(s, names);
    if (k < 0) a2_throw_id(vm, "ArgumentError", 2008, name, NULL);
    return k;
}
static u32 u32_arg(VM2 *vm, int argc, V2 *argv, int i, u32 def) { return i < argc ? v2_touint(vm, argv[i]) : def; }
static s32 i32_arg(VM2 *vm, int argc, V2 *argv, int i, s32 def) { return i < argc ? v2_toint(vm, argv[i]) : def; }
static int bool_arg(VM2 *vm, int argc, V2 *argv, int i, int def) { return i < argc ? v2_tobool(vm, argv[i]) : def; }
static double num_arg(VM2 *vm, int argc, V2 *argv, int i, double def) { return i < argc ? v2_tonum(vm, argv[i]) : def; }
static A2Obj *obj_arg(int argc, V2 *argv, int i) { return (i < argc && argv[i].t == T_OBJ) ? argv[i].u.o : NULL; }
static A2Obj *nonnull_arg(VM2 *vm, int argc, V2 *argv, int i, const char *name) {
    A2Obj *o = obj_arg(argc, argv, i);
    if (!o) a2_throw_id(vm, "TypeError", 2007, name, NULL);
    return o;
}

static const char *const NAMES_FACE[] = { "none", "front", "back", "frontAndBack", NULL };
static const char *const NAMES_CMP[] = { "never", "less", "equal", "lessEqual", "greater", "notEqual", "greaterEqual", "always", NULL };
static const char *const NAMES_STENCIL[] = { "keep", "zero", "set", "incrementSaturate", "decrementSaturate", "invert", "incrementWrap", "decrementWrap", NULL };
static const char *const NAMES_BLEND[] = { "zero", "one", "sourceColor", "oneMinusSourceColor", "sourceAlpha", "oneMinusSourceAlpha", "destinationColor", "oneMinusDestinationColor", "destinationAlpha", "oneMinusDestinationAlpha", NULL };
static const char *const NAMES_VFMT[] = { "float1", "float2", "float3", "float4", "bytes4", NULL };
static const char *const NAMES_PROG[] = { "vertex", "fragment", NULL };
static const char *const NAMES_WRAP[] = { "clamp", "repeat", "clamp_u_repeat_v", "repeat_u_clamp_v", NULL };
static const char *const NAMES_FILTER[] = { "nearest", "linear", "anisotropic2x", "anisotropic4x", "anisotropic8x", "anisotropic16x", NULL };
static const char *const NAMES_TFMT[] = { "bgra", "bgraPacked4444", "bgrPacked565", "compressed", "compressedAlpha", "rgbaHalfFloat", NULL };
static const char *const NAMES_PROFILE[] = { "baseline", "baselineConstrained", "baselineExtended", "standard", "standardConstrained", "standardExtended", NULL };

/* "Error #id" with the message's %1..%3 replaced */
static void throw_fmt(VM2 *vm, const char *cls, int id, const char *a1, const char *a2, const char *a3) {
    extern const char *a2_error_message(int id); const char *m = a2_error_message(id), *p; char buf[1024]; size_t n = 0;
    if (!m) m = "";
    for (p = m; *p && n < 900; p++) {
        if (*p == '%' && p[1] >= '1' && p[1] <= '3') {
            const char *s = p[1] == '1' ? a1 : p[1] == '2' ? a2 : a3;
            if (s) { size_t l = strlen(s); if (l > 100) l = 100; memcpy(buf + n, s, l); n += l; }
            p++; continue;
        }
        buf[n++] = *p;
    }
    buf[n] = 0;
    a2_throw_error(vm, cls, buf, id);
}

/* ---------------------------------------------------------------- */
/* Stage3D                                                          */

static S3DStage *stage_of(VM2 *vm, V2 self) {
    A2Obj *o = SELFO; S3DStage *s;
    if (!o || o->kind != OK_PLAIN || !o->cls || !a2_is_subclass(o->cls, c_stage3d)) return NULL;
    if (o->payload) return (S3DStage*)o->payload;
    s = (S3DStage*)calloc(1, sizeof(S3DStage));
    if (!s) return NULL;
    s->pk = PK_STAGE; s->vm = vm; s->self = o; s->visible = 1;
    o->payload = s;
    { int i; for (i = 0; i < MAX_STAGES; i++) if (!g_stages[i]) { g_stages[i] = s; break; } }
    return s;
}

A2Obj *a2_stage3d_create(VM2 *vm, int index) {
    A2Obj *o; S3DStage *s;
    if (!c_stage3d) return NULL;
    o = a2_new(vm, c_stage3d);
    s = stage_of(vm, v2_obj(o));
    if (s) s->index = index;
    return o;
}

static V2 s3d_get_x(VM2 *vm, V2 self, int argc, V2 *argv) { S3DStage *s = stage_of(vm, self); (void)argc; (void)argv; return v2_num(s ? s->x : 0); }
static V2 s3d_get_y(VM2 *vm, V2 self, int argc, V2 *argv) { S3DStage *s = stage_of(vm, self); (void)argc; (void)argv; return v2_num(s ? s->y : 0); }
static V2 s3d_set_xy(VM2 *vm, V2 self, int argc, V2 *argv, int y) {
    S3DStage *s = stage_of(vm, self); double v = num_arg(vm, argc, argv, 0, 0);
    if (!OK_) return v2_undef();
    /* the docs say -8191, Flash accepts -8192 */
    if (v != v || v < -8192.0 || v > 8191.0) { a2_throw_id(vm, "ArgumentError", 2006, NULL, NULL); return v2_undef(); }
    if (s) { if (y) s->y = v; else s->x = v; }
    return v2_undef();
}
static V2 s3d_set_x(VM2 *vm, V2 self, int argc, V2 *argv) { return s3d_set_xy(vm, self, argc, argv, 0); }
static V2 s3d_set_y(VM2 *vm, V2 self, int argc, V2 *argv) { return s3d_set_xy(vm, self, argc, argv, 1); }
static V2 s3d_get_visible(VM2 *vm, V2 self, int argc, V2 *argv) { S3DStage *s = stage_of(vm, self); (void)argc; (void)argv; return v2_bool(s ? s->visible : 1); }
static V2 s3d_set_visible(VM2 *vm, V2 self, int argc, V2 *argv) { S3DStage *s = stage_of(vm, self); int b = bool_arg(vm, argc, argv, 0, 0); if (s) s->visible = b; return v2_undef(); }
static V2 s3d_get_context3D(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Prop *p; (void)argc; (void)argv;
    if (self.t != T_OBJ) return v2_null();
    p = a2_dyn_find(self.u.o, a2_str(vm, "__context3D"));
    return p && p->v.t == T_OBJ ? p->v : v2_null();
}
/* Stage3D.requestContext3D_internal: the highest profile asked for */
static void request(VM2 *vm, V2 self, int profile) {
    S3DStage *s = stage_of(vm, self);
    if (!s) return;
    s->profile = profile;
    s->requested = 1;
}
static const int PROFILE_RANK[] = { 1, 0, 2, 4, 3, 5 };   /* baseline < baselineExtended < standardConstrained < standard < standardExtended; constrained baseline lowest */
static V2 s3d_requestContext3D(VM2 *vm, V2 self, int argc, V2 *argv) {
    int k = 0;
    if (argc > 1) {
        V2 v = argv[1];
        k = (v.t == T_NULL || v.t == T_UNDEF) ? -1 : lookup(v2_tostr(vm, v)->s, NAMES_PROFILE);
        if (!OK_) return v2_undef();
        if (k < 0) { a2_throw_id(vm, "ArgumentError", 2008, "profile", NULL); return v2_undef(); }
    }
    request(vm, self, k);
    return v2_undef();
}
static V2 s3d_requestMatching(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *v = nonnull_arg(vm, argc, argv, 0, "profiles"); int i, best = -1;
    if (!v) return v2_undef();
    if (!v->alen) { a2_throw_id(vm, "ArgumentError", 2008, "profiles", NULL); return v2_undef(); }
    for (i = 0; i < v->alen; i++) {
        V2 e = v->arr[i]; int k = (e.t == T_NULL || e.t == T_UNDEF) ? -1 : lookup(v2_tostr(vm, e)->s, NAMES_PROFILE);
        if (k < 0) { a2_throw_id(vm, "ArgumentError", 2008, "profile", NULL); return v2_undef(); }
        if (best < 0 || PROFILE_RANK[k] > PROFILE_RANK[best]) best = k;
    }
    request(vm, self, best);
    return v2_undef();
}

/* end of frame: create the requested contexts and dispatch context3DCreate */
void a2_stage3d_frame(VM2 *vm) {
    int i; extern void a2_report_uncaught(VM2 *vm);
    for (i = 0; i < MAX_STAGES; i++) {
        S3DStage *s = g_stages[i]; A2Obj *co; PCtx *pc; S3DCtx *ctx;
        if (!s || s->vm != vm || !s->requested) continue;
        s->requested = 0;
        ctx = s3d_ctx_new(s->profile);
        pc = (PCtx*)calloc(1, sizeof(PCtx));
        if (!ctx || !pc) { s3d_ctx_unref(ctx); free(pc); continue; }
        co = a2_new(vm, c_context3d);
        pc->pk = PK_CTX; pc->ctx = ctx; pc->stage3d = s->self;
        co->payload = pc;
        s3d_ctx_unref(s->ctx); ctx->refs++; s->ctx = ctx;
        a2_dyn_set_hidden(vm, s->self, a2_str(vm, "__context3D"), v2_obj(co));
        a2_dispatch_simple(vm, s->self, a2_str(vm, "context3DCreate"), 0);
        if (vm->throwing) a2_report_uncaught(vm);
    }
}

/* draw the presented back buffers under the display list */
void a2_stage3d_render(Player *p, const SWFMatrix *sm) {
    int i, order;
    Surface *dst = p->surf;
    for (order = 0; order < 4 * MAX_STAGES; order++) for (i = 0; i < MAX_STAGES; i++) {
        S3DStage *s = g_stages[i]; S3DCtx *c; double dx0, dy0, sc; int x0, y0, x1, y1, x, y;
        if (!s || s->index != order || s->vm != (VM2*)p->avm2 || !s->visible || !s->ctx) continue;
        c = s->ctx;
        if (!s3d_front(c)) continue;
        sc = sm->sx; if (sc <= 0) sc = 1;
        dx0 = sm->tx + s->x * 20.0 * sm->sx; dy0 = sm->ty + s->y * 20.0 * sm->sy;
        sc = sm->sx * 20.0;
        x0 = (int)floor(dx0 + 0.5); y0 = (int)floor(dy0 + 0.5);
        x1 = (int)floor(dx0 + c->fw * sc + 0.5); y1 = (int)floor(dy0 + c->fh * sm->sy * 20.0 + 0.5);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > dst->w) x1 = dst->w;
        if (y1 > dst->h) y1 = dst->h;
        for (y = y0; y < y1; y++) {
            int sy = (int)((y + 0.5 - dy0) / (sm->sy * 20.0)); u32 *row = dst->px + (size_t)y * dst->w;
            if (sy < 0) sy = 0;
            if (sy >= c->fh) sy = c->fh - 1;
            for (x = x0; x < x1; x++) {
                int sx = (int)((x + 0.5 - dx0) / sc); const u8 *q;
                if (sx < 0) sx = 0;
                if (sx >= c->fw) sx = c->fw - 1;
                q = c->front + ((size_t)sy * c->fw + sx) * 4;
                row[x] = 0xFF000000u | ((u32)q[0] << 16) | ((u32)q[1] << 8) | q[2];
            }
        }
        surf_fill_from(dst, dst->px);   /* every sample of the stage gets the presented pixels */
    }
}

/* ---------------------------------------------------------------- */
/* Context3D                                                        */

static PCtx *ctx_of(V2 self) { return (PCtx*)payload_v(self, PK_CTX); }
#define CTX_PRE PCtx *pc = ctx_of(self); S3DCtx *c; if (!pc) return v2_undef(); c = pc->ctx; (void)c

static A2Obj *wrap(VM2 *vm, A2Class *cls, void *pl) { A2Obj *o = a2_new(vm, cls); o->payload = pl; return o; }

static V2 c3_clear(VM2 *vm, V2 self, int argc, V2 *argv) {
    double r, g, b, a, d; u32 st, mask; CTX_PRE;
    r = num_arg(vm, argc, argv, 0, 0); g = num_arg(vm, argc, argv, 1, 0); b = num_arg(vm, argc, argv, 2, 0); a = num_arg(vm, argc, argv, 3, 1);
    d = num_arg(vm, argc, argv, 4, 1); st = u32_arg(vm, argc, argv, 5, 0); mask = u32_arg(vm, argc, argv, 6, 0xFFFFFFFFu);
    if (!OK_) return v2_undef();
    s3d_clear(c, r, g, b, a, d, st, mask);
    return v2_undef();
}
static V2 c3_configureBackBuffer(VM2 *vm, V2 self, int argc, V2 *argv) {
    u32 w, h, aa; int ds, old; CTX_PRE;
    w = u32_arg(vm, argc, argv, 0, 0); h = u32_arg(vm, argc, argv, 1, 0); aa = u32_arg(vm, argc, argv, 2, 0); ds = bool_arg(vm, argc, argv, 3, 1);
    if (!OK_) return v2_undef();
    old = vm->swf_version < 30;
    if (old && !w && !h && !aa && !ds) return v2_undef();
    if (w < 32 || w > 16384) { if (old) a2_throw_id(vm, "Error", 3669, NULL, NULL); else a2_throw_id(vm, "Error", 3780, "32", "16384"); return v2_undef(); }
    if (h < 32 || h > 16384) { if (old) a2_throw_id(vm, "Error", 3669, NULL, NULL); else a2_throw_id(vm, "Error", 3781, "32", "16384"); return v2_undef(); }
    s3d_configure(c, (int)w, (int)h, (int)aa, ds);
    return v2_undef();
}
static V2 c3_createIndexBuffer(VM2 *vm, V2 self, int argc, V2 *argv) {
    u32 n; PIBuf *pb; CTX_PRE;
    n = u32_arg(vm, argc, argv, 0, 0);
    if (!OK_) return v2_undef();
    if (!n) { a2_throw_id(vm, "ArgumentError", 3671, NULL, NULL); return v2_undef(); }
    if (n > 0x1000000) { a2_throw_id(vm, "ArgumentError", 3670, NULL, NULL); return v2_undef(); }
    pb = (PIBuf*)calloc(1, sizeof(PIBuf));
    if (!pb || !(pb->b = s3d_ibuf_new(n))) { free(pb); a2_throw_id(vm, "Error", 3672, NULL, NULL); return v2_undef(); }
    pb->pk = PK_IBUF;
    return v2_obj(wrap(vm, c_ibuf3d, pb));
}
static V2 c3_createVertexBuffer(VM2 *vm, V2 self, int argc, V2 *argv) {
    u32 n, d32; PVBuf *pb; CTX_PRE;
    n = u32_arg(vm, argc, argv, 0, 0); d32 = u32_arg(vm, argc, argv, 1, 0);
    if (!OK_) return v2_undef();
    if (d32 > 64) { a2_throw_id(vm, "ArgumentError", 3670, NULL, NULL); return v2_undef(); }
    if (!d32) { a2_throw_id(vm, "ArgumentError", 3671, NULL, NULL); return v2_undef(); }
    if (n > 0x100000) { a2_throw_id(vm, "ArgumentError", 3670, NULL, NULL); return v2_undef(); }
    pb = (PVBuf*)calloc(1, sizeof(PVBuf));
    if (!pb || !(pb->b = s3d_vbuf_new(n, d32))) { free(pb); a2_throw_id(vm, "Error", 3672, NULL, NULL); return v2_undef(); }
    pb->pk = PK_VBUF;
    return v2_obj(wrap(vm, c_vbuf3d, pb));
}
static V2 c3_setVertexBufferAt(VM2 *vm, V2 self, int argc, V2 *argv) {
    u32 idx, off; PVBuf *pb = NULL; int fmt = S3D_VF_FLOAT4; A2Obj *bo; CTX_PRE;
    idx = u32_arg(vm, argc, argv, 0, 0);
    bo = obj_arg(argc, argv, 1);
    if (!OK_) return v2_undef();
    if (bo) {
        pb = (PVBuf*)payload(bo, PK_VBUF);
        if (!pb) { a2_throw_coerce_error(vm, argv[1], c_vbuf3d); return v2_undef(); }
        /* the format is only checked with a buffer */
        fmt = enum_arg(vm, argc, argv, 3, "float4", "vertexStreamFormat", NAMES_VFMT);
        if (fmt < 0) return v2_undef();
    }
    off = u32_arg(vm, argc, argv, 2, 0);
    if (!OK_ || idx >= S3D_MAX_ATTR) return v2_undef();
    s3d_set_vertex_buffer(c, (int)idx, pb ? pb->b : NULL, off, fmt);
    return v2_undef();
}
static V2 c3_createProgram(VM2 *vm, V2 self, int argc, V2 *argv) {
    PProg *pp; CTX_PRE; (void)argc; (void)argv;
    pp = (PProg*)calloc(1, sizeof(PProg));
    if (!pp || !(pp->p = s3d_prog_new())) { free(pp); return v2_null(); }
    pp->pk = PK_PROG;
    return v2_obj(wrap(vm, c_program3d, pp));
}
static V2 c3_setProgram(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *po = obj_arg(argc, argv, 0); PProg *pp = NULL; CTX_PRE;
    if (po && !(pp = (PProg*)payload(po, PK_PROG))) { a2_throw_coerce_error(vm, argv[0], c_program3d); return v2_undef(); }
    s3d_set_program(c, pp ? pp->p : NULL);
    return v2_undef();
}
static V2 c3_drawTriangles(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *io; PIBuf *pi; u32 first; s32 n; int missing; CTX_PRE;
    io = nonnull_arg(vm, argc, argv, 0, "indexBuffer");
    if (!io) return v2_undef();
    if (!(pi = (PIBuf*)payload(io, PK_IBUF))) { a2_throw_coerce_error(vm, argv[0], c_ibuf3d); return v2_undef(); }
    first = u32_arg(vm, argc, argv, 1, 0); n = i32_arg(vm, argc, argv, 2, -1);
    if (!OK_) return v2_undef();
    s3d_draw(c, pi->b, first, n, &missing);
    if (missing >= 0 && pc->error_checking) { char b[16]; sprintf(b, "%d", missing); a2_throw_id(vm, "Error", 3611, b, NULL); }
    return v2_undef();
}
static V2 c3_present(VM2 *vm, V2 self, int argc, V2 *argv) { CTX_PRE; (void)vm; (void)argc; (void)argv; s3d_present(c); return v2_undef(); }
static V2 c3_setCulling(VM2 *vm, V2 self, int argc, V2 *argv) {
    int f; CTX_PRE;
    f = enum_arg(vm, argc, argv, 0, NULL, "triangleFaceToCull", NAMES_FACE);
    if (f < 0) return v2_undef();
    c->st.cull = f == 0 ? S3D_FACE_NONE : f == 1 ? S3D_FACE_FRONT : f == 2 ? S3D_FACE_BACK : S3D_FACE_BOTH;
    return v2_undef();
}
static V2 make_texture(VM2 *vm, A2Class *cls, int kind, int w, int h, int fmt) {
    PTex *pt = (PTex*)calloc(1, sizeof(PTex));
    if (!pt || !(pt->t = s3d_tex_new(kind, w, h, fmt))) { free(pt); a2_throw_id(vm, "Error", 3672, NULL, NULL); return v2_undef(); }
    pt->pk = PK_TEX; pt->format = fmt;
    return v2_obj(wrap(vm, cls, pt));
}
static V2 c3_createTexture(VM2 *vm, V2 self, int argc, V2 *argv) {
    s32 w, h; int fmt; CTX_PRE;
    w = i32_arg(vm, argc, argv, 0, 0); h = i32_arg(vm, argc, argv, 1, 0);
    if (!OK_) return v2_undef();
    fmt = enum_arg(vm, argc, argv, 2, NULL, "textureFormat", NAMES_TFMT);
    if (fmt < 0) return v2_undef();
    return make_texture(vm, c_texture, S3D_TEX_2D, w, h, fmt);
}
static V2 c3_createRectangleTexture(VM2 *vm, V2 self, int argc, V2 *argv) {
    s32 w, h; int fmt; CTX_PRE;
    w = i32_arg(vm, argc, argv, 0, 0); h = i32_arg(vm, argc, argv, 1, 0);
    if (!OK_) return v2_undef();
    fmt = enum_arg(vm, argc, argv, 2, NULL, "textureFormat", NAMES_TFMT);
    if (fmt < 0) return v2_undef();
    return make_texture(vm, c_recttex, S3D_TEX_RECT, w, h, fmt);
}
static V2 c3_createCubeTexture(VM2 *vm, V2 self, int argc, V2 *argv) {
    s32 sz; int fmt; CTX_PRE;
    sz = i32_arg(vm, argc, argv, 0, 0);
    if (!OK_) return v2_undef();
    fmt = enum_arg(vm, argc, argv, 1, NULL, "textureFormat", NAMES_TFMT);
    if (fmt < 0) return v2_undef();
    return make_texture(vm, c_cubetex, S3D_TEX_CUBE, sz, sz, fmt);
}
static V2 c3_createVideoTexture(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_null(); }
static V2 c3_supportsVideoTexture(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_bool(0); }
const char *g_driver_info;   /* a host override of Context3D.driverInfo (the test harness names Ruffle's) */
static V2 c3_driverInfo(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_cstr(vm, g_driver_info ? g_driver_info : "Software (Direct blitting)"); }
static V2 c3_get_errorChecking(VM2 *vm, V2 self, int argc, V2 *argv) { PCtx *pc = ctx_of(self); UNUSED; return v2_bool(pc && pc->error_checking); }
static V2 c3_set_errorChecking(VM2 *vm, V2 self, int argc, V2 *argv) { PCtx *pc = ctx_of(self); int b = bool_arg(vm, argc, argv, 0, 0); if (pc) pc->error_checking = b; return v2_undef(); }
static int prog_type_arg(VM2 *vm, int argc, V2 *argv) { return enum_arg(vm, argc, argv, 0, NULL, "programType", NAMES_PROG); }
static V2 c3_setProgramConstantsFromMatrix(VM2 *vm, V2 self, int argc, V2 *argv) {
    int type; u32 first; A2Obj *mo; float raw[16], t[16]; int tr, i, j; CTX_PRE;
    type = prog_type_arg(vm, argc, argv);
    if (type < 0) return v2_undef();
    first = u32_arg(vm, argc, argv, 1, 0);
    if (!OK_) return v2_undef();
    mo = nonnull_arg(vm, argc, argv, 2, "matrix");
    if (!mo) return v2_undef();
    tr = bool_arg(vm, argc, argv, 3, 0);
    if (!a2_m3_raw(mo, raw)) return v2_undef();
    /* the registers get rawData (column major); transposedMatrix transposes it first */
    if (tr) { for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) t[i * 4 + j] = raw[j * 4 + i]; memcpy(raw, t, sizeof(raw)); }
    s3d_set_constants(c, type, first, raw, 16);
    return v2_undef();
}
static V2 c3_setProgramConstantsFromVector(VM2 *vm, V2 self, int argc, V2 *argv) {
    int type; u32 first, take, i; s32 nreg; A2Obj *v; float *f; CTX_PRE;
    type = prog_type_arg(vm, argc, argv);
    if (type < 0) return v2_undef();
    first = u32_arg(vm, argc, argv, 1, 0);
    if (!OK_) return v2_undef();
    v = nonnull_arg(vm, argc, argv, 2, "vector");
    if (!v) return v2_undef();
    nreg = i32_arg(vm, argc, argv, 3, -1);
    if (!OK_) return v2_undef();
    if (nreg != -1) {
        u32 req = (u32)nreg * 4u;
        if ((u32)v->alen < req) { a2_throw_id(vm, "Error", 3669, NULL, NULL); return v2_undef(); }
        take = req;
    } else take = (u32)v->alen;
    if (!take) return v2_undef();
    f = (float*)malloc(take * sizeof(float));
    if (!f) return v2_undef();
    for (i = 0; i < take; i++) f[i] = (float)v2_tonum(vm, v->arr[i]);
    s3d_set_constants(c, type, first, f, take);
    free(f);
    return v2_undef();
}
static V2 c3_setProgramConstantsFromByteArray(VM2 *vm, V2 self, int argc, V2 *argv) {
    int type; u32 first, off, i; s32 nreg; A2Obj *ba; float *f; CTX_PRE;
    type = prog_type_arg(vm, argc, argv);
    if (type < 0) return v2_undef();
    first = u32_arg(vm, argc, argv, 1, 0); nreg = i32_arg(vm, argc, argv, 2, 0);
    if (!OK_) return v2_undef();
    ba = nonnull_arg(vm, argc, argv, 3, "data");
    if (!ba) return v2_undef();
    off = u32_arg(vm, argc, argv, 4, 0);
    if (!OK_) return v2_undef();
    /* little-endian floats whatever the ByteArray's endian */
    if (nreg < 0 || ba->kind != OK_BYTEARRAY || off > (u32)ba->blen || (u32)(ba->blen - off) / 16 < (u32)nreg) { a2_throw_id(vm, "Error", 3669, NULL, NULL); return v2_undef(); }
    if (!nreg) return v2_undef();
    f = (float*)malloc((size_t)nreg * 4 * sizeof(float));
    if (!f) return v2_undef();
    for (i = 0; i < (u32)nreg * 4; i++) { const u8 *p = ba->bytes + off + i * 4; u32 u = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); memcpy(&f[i], &u, 4); }
    s3d_set_constants(c, type, first, f, (u32)nreg * 4);
    free(f);
    return v2_undef();
}
static V2 c3_setColorMask(VM2 *vm, V2 self, int argc, V2 *argv) {
    int i; CTX_PRE;
    for (i = 0; i < 4; i++) c->st.cmask[i] = bool_arg(vm, argc, argv, i, 0);
    c->dirty = 1;
    return v2_undef();
}
static V2 c3_setDepthTest(VM2 *vm, V2 self, int argc, V2 *argv) {
    int m, cmp; CTX_PRE;
    m = bool_arg(vm, argc, argv, 0, 0);
    cmp = enum_arg(vm, argc, argv, 1, NULL, "passCompareMode", NAMES_CMP);
    if (cmp < 0) return v2_undef();
    c->st.depth_mask = m; c->st.depth_cmp = cmp; c->dirty = 1;
    return v2_undef();
}
static V2 c3_setScissorRectangle(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *r = obj_arg(argc, argv, 0); CTX_PRE;
    if (!r) { c->st.scissor_on = 0; c->dirty = 1; return v2_undef(); }
    { double x = v2_tonum(vm, a2_get_s(vm, v2_obj(r), "x")), y = v2_tonum(vm, a2_get_s(vm, v2_obj(r), "y")), w = v2_tonum(vm, a2_get_s(vm, v2_obj(r), "width")), h = v2_tonum(vm, a2_get_s(vm, v2_obj(r), "height"));
      if (!OK_) return v2_undef();
      /* Ruffle keeps twips */
      c->st.sc_x = floor(x * 20) / 20; c->st.sc_y = floor(y * 20) / 20; c->st.sc_w = floor(w * 20) / 20; c->st.sc_h = floor(h * 20) / 20; }
    c->st.scissor_on = 1; c->dirty = 1;
    return v2_undef();
}
static V2 c3_setRenderToBackBuffer(VM2 *vm, V2 self, int argc, V2 *argv) { CTX_PRE; (void)vm; (void)argc; (void)argv; s3d_set_render_to_back_buffer(c); return v2_undef(); }
static V2 c3_setBlendFactors(VM2 *vm, V2 self, int argc, V2 *argv) {
    static const int map[] = { S3D_BF_ZERO, S3D_BF_ONE, S3D_BF_SRC_COLOR, S3D_BF_ONE_MINUS_SRC_COLOR, S3D_BF_SRC_ALPHA, S3D_BF_ONE_MINUS_SRC_ALPHA, S3D_BF_DST_COLOR, S3D_BF_ONE_MINUS_DST_COLOR, S3D_BF_DST_ALPHA, S3D_BF_ONE_MINUS_DST_ALPHA };
    int s, d; CTX_PRE;
    s = enum_arg(vm, argc, argv, 0, NULL, "sourceFactor", NAMES_BLEND);
    if (s < 0) return v2_undef();
    d = enum_arg(vm, argc, argv, 1, NULL, "destinationFactor", NAMES_BLEND);
    if (d < 0) return v2_undef();
    c->st.blend_src = map[s]; c->st.blend_dst = map[d]; c->dirty = 1;
    return v2_undef();
}
static V2 c3_setTextureAt(VM2 *vm, V2 self, int argc, V2 *argv) {
    s32 i; A2Obj *to; PTex *pt = NULL; CTX_PRE;
    i = i32_arg(vm, argc, argv, 0, 0);
    to = obj_arg(argc, argv, 1);
    if (!OK_) return v2_undef();
    if (to && !(pt = (PTex*)payload(to, PK_TEX))) { a2_throw_coerce_error(vm, argv[1], c_texbase); return v2_undef(); }
    if (i < 0 || i >= S3D_MAX_SAMP) return v2_undef();
    s3d_set_texture(c, i, pt ? pt->t : NULL);
    return v2_undef();
}
static V2 c3_profile(VM2 *vm, V2 self, int argc, V2 *argv) { PCtx *pc = ctx_of(self); UNUSED; return v2_cstr(vm, NAMES_PROFILE[pc ? pc->ctx->profile : 0]); }
static V2 c3_backBufferWidth(VM2 *vm, V2 self, int argc, V2 *argv) { PCtx *pc = ctx_of(self); UNUSED; return v2_int(pc && pc->ctx->configured ? pc->ctx->back.w : 0); }
static V2 c3_backBufferHeight(VM2 *vm, V2 self, int argc, V2 *argv) { PCtx *pc = ctx_of(self); UNUSED; return v2_int(pc && pc->ctx->configured ? pc->ctx->back.h : 0); }
static V2 c3_maxBackBuffer(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_int(2048); }
static V2 c3_noop(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_undef(); }
static V2 c3_setStencilReferenceValue(VM2 *vm, V2 self, int argc, V2 *argv) {
    CTX_PRE;
    c->st.st_ref = u32_arg(vm, argc, argv, 0, 0); c->st.st_rmask = u32_arg(vm, argc, argv, 1, 255); c->st.st_wmask = u32_arg(vm, argc, argv, 2, 255); c->dirty = 1;
    return v2_undef();
}
static V2 c3_setSamplerStateAt(VM2 *vm, V2 self, int argc, V2 *argv) {
    s32 i; int wrap, filter; const char *mip; CTX_PRE;
    i = i32_arg(vm, argc, argv, 0, 0);
    if (!OK_) return v2_undef();
    wrap = enum_arg(vm, argc, argv, 1, NULL, "wrap", NAMES_WRAP);
    if (wrap < 0) return v2_undef();
    filter = enum_arg(vm, argc, argv, 2, NULL, "filter", NAMES_FILTER);
    if (filter < 0) return v2_undef();
    mip = str_arg(vm, argc, argv, 3, NULL, "mipfilter");
    if (!OK_) return v2_undef();
    (void)mip;
    if (i < 0 || i >= S3D_MAX_SAMP) return v2_undef();
    c->st.samp[i].wrap = (u8)wrap; c->st.samp[i].filter = (u8)filter; c->st.samp[i].mip = 0; c->dirty = 1;
    return v2_undef();
}
static V2 c3_setRenderToTexture(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *to; PTex *pt; int ds; u32 aa, surf; CTX_PRE;
    to = nonnull_arg(vm, argc, argv, 0, "texture");
    if (!to) return v2_undef();
    if (!(pt = (PTex*)payload(to, PK_TEX))) { a2_throw_coerce_error(vm, argv[0], c_texbase); return v2_undef(); }
    ds = bool_arg(vm, argc, argv, 1, 0); aa = u32_arg(vm, argc, argv, 2, 0); surf = u32_arg(vm, argc, argv, 3, 0);
    if (!OK_) return v2_undef();
    if (pt->t->kind == S3D_TEX_CUBE) { if (surf > 5) { a2_throw_id(vm, "ArgumentError", 3772, NULL, NULL); return v2_undef(); } }
    else if (pt->t->kind == S3D_TEX_RECT) { if (surf != 0) { a2_throw_id(vm, "ArgumentError", 3773, NULL, NULL); return v2_undef(); } }
    else if (surf != 0) { a2_throw_id(vm, "ArgumentError", 3771, NULL, NULL); return v2_undef(); }
    s3d_set_render_to_texture(c, pt->t, ds, (int)surf, aa > 16 ? 16 : (int)aa);
    return v2_undef();
}
static V2 c3_setStencilActions(VM2 *vm, V2 self, int argc, V2 *argv) {
    int face, cmp, both, dfail, sfail; S3DStencilFace st; CTX_PRE;
    face = enum_arg(vm, argc, argv, 0, "frontAndBack", "triangleFace", NAMES_FACE); if (face < 0) return v2_undef();
    cmp = enum_arg(vm, argc, argv, 1, "always", "compareMode", NAMES_CMP); if (cmp < 0) return v2_undef();
    both = enum_arg(vm, argc, argv, 2, "keep", "actionOnBothPass", NAMES_STENCIL); if (both < 0) return v2_undef();
    dfail = enum_arg(vm, argc, argv, 3, "keep", "actionOnDepthFail", NAMES_STENCIL); if (dfail < 0) return v2_undef();
    sfail = enum_arg(vm, argc, argv, 4, "keep", "actionOnDepthPassStencilFail", NAMES_STENCIL); if (sfail < 0) return v2_undef();
    st.cmp = cmp; st.pass = both; st.dfail = dfail; st.fail = sfail;
    c->st.st_front_on = c->st.st_back_on = 0;
    if (face == 1 || face == 3) { c->st.st_front = st; c->st.st_front_on = 1; }
    if (face == 2 || face == 3) { c->st.st_back = st; c->st.st_back_on = 1; }
    c->dirty = 1;
    return v2_undef();
}
static V2 c3_drawToBitmapData(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *bo; BitmapData *bd; u8 *px; int w, h, x, y, mw, mh; CTX_PRE;
    bo = nonnull_arg(vm, argc, argv, 0, "destination");
    if (!bo) return v2_undef();
    if (!c->cleared) { a2_throw_id(vm, "Error", 3692, NULL, NULL); return v2_undef(); }
    bd = a2_bitmapdata_of(bo); if (bd) bmd_own(bd);
    if (!bd || bd->disposed || !bd->px) return v2_undef();
    px = s3d_read_back(c, &w, &h);
    if (!px) return v2_undef();
    /* the back buffer is drawn over the BitmapData from its top-left corner (premultiplied source over) */
    mw = w < (int)bd->w ? w : (int)bd->w; mh = h < (int)bd->h ? h : (int)bd->h;
    for (y = 0; y < mh; y++) for (x = 0; x < mw; x++) {
        const u8 *s = px + ((size_t)y * w + x) * 4; u32 *d = bd->px + (size_t)y * bd->w + x; u32 dv = *d;
        int sa = s[3], inv = 255 - sa, k; u32 out = 0; int dc[4], sc[4];
        dc[0] = (int)((dv >> 24) & 255); dc[1] = (int)((dv >> 16) & 255); dc[2] = (int)((dv >> 8) & 255); dc[3] = (int)(dv & 255);
        sc[0] = sa; sc[1] = s[0]; sc[2] = s[1]; sc[3] = s[2];
        for (k = 0; k < 4; k++) { int v = sc[k] + (dc[k] * inv + 127) / 255; if (v > 255) v = 255; out = (out << 8) | (u32)v; }
        if (!bd->transparent) out |= 0xFF000000u;
        *d = out;
    }
    free(px);
    bmd_changed(bd);
    return v2_undef();
}
static V2 c3_dispose(VM2 *vm, V2 self, int argc, V2 *argv) {
    CTX_PRE; (void)argc; (void)argv;
    if (pc->stage3d) {
        S3DStage *s = (S3DStage*)payload(pc->stage3d, PK_STAGE);
        a2_dyn_set_hidden(vm, pc->stage3d, a2_str(vm, "__context3D"), v2_null());
        if (s && s->ctx == c) { s3d_ctx_unref(s->ctx); s->ctx = NULL; }
    }
    return v2_undef();
}

/* ---------------------------------------------------------------- */
/* Program3D, buffers                                               */

static V2 p3_upload(VM2 *vm, V2 self, int argc, V2 *argv) {
    PProg *pp = (PProg*)payload_v(self, PK_PROG); A2Obj *vo, *fo; AgalErr e;
    if (!pp) return v2_undef();
    vo = nonnull_arg(vm, argc, argv, 0, "vertexProgram"); if (!vo) return v2_undef();
    fo = nonnull_arg(vm, argc, argv, 1, "fragmentProgram"); if (!fo) return v2_undef();
    if (vo->kind != OK_BYTEARRAY) { a2_throw_coerce_error(vm, argv[0], vm->c_bytearray); return v2_undef(); }
    if (fo->kind != OK_BYTEARRAY) { a2_throw_coerce_error(vm, argv[1], vm->c_bytearray); return v2_undef(); }
    S3D_BEFORE_WRITE(pp->p);
    if (!s3d_prog_upload(pp->p, vo->bytes, (u32)vo->blen, fo->bytes, (u32)fo->blen, &e)) {
        char tok[16]; sprintf(tok, "%d", e.token);
        throw_fmt(vm, e.argerr ? "ArgumentError" : "Error", e.id, e.shader, e.a2, tok);
    }
    return v2_undef();
}
static V2 res_dispose(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_undef(); }

static V2 vb_uploadFromVector(VM2 *vm, V2 self, int argc, V2 *argv) {
    PVBuf *pb = (PVBuf*)payload_v(self, PK_VBUF); A2Obj *v; u32 start, n, i, total, cap;
    if (!pb) return v2_undef();
    v = nonnull_arg(vm, argc, argv, 0, "data"); if (!v) return v2_undef();
    start = u32_arg(vm, argc, argv, 1, 0); n = u32_arg(vm, argc, argv, 2, 0);
    if (!OK_) return v2_undef();
    total = n * pb->b->d32; if ((u32)v->alen < total) total = (u32)v->alen;
    if (start >= pb->b->nverts) return v2_undef();
    cap = (pb->b->nverts - start) * pb->b->d32; if (total > cap) total = cap;
    s3d_vbuf_before_write(pb->b);
    for (i = 0; i < total; i++) { float f = (float)v2_tonum(vm, v->arr[i]); memcpy(pb->b->data + ((size_t)start * pb->b->d32 + i) * 4, &f, 4); }
    return v2_undef();
}
static V2 vb_uploadFromByteArray(VM2 *vm, V2 self, int argc, V2 *argv) {
    PVBuf *pb = (PVBuf*)payload_v(self, PK_VBUF); A2Obj *ba; u32 off, start, n; size_t bytes, cap;
    if (!pb) return v2_undef();
    ba = nonnull_arg(vm, argc, argv, 0, "data"); if (!ba) return v2_undef();
    off = u32_arg(vm, argc, argv, 1, 0); start = u32_arg(vm, argc, argv, 2, 0); n = u32_arg(vm, argc, argv, 3, 0);
    if (!OK_ || ba->kind != OK_BYTEARRAY) return v2_undef();
    bytes = (size_t)n * 4 * pb->b->d32;
    if ((size_t)off > (size_t)ba->blen || (size_t)ba->blen - off < bytes) { a2_throw_id(vm, "flash.errors::EOFError", 2030, NULL, NULL); return v2_undef(); }
    if (start >= pb->b->nverts) return v2_undef();
    cap = ((size_t)pb->b->nverts - start) * pb->b->d32 * 4; if (bytes > cap) bytes = cap;
    s3d_vbuf_before_write(pb->b);
    memcpy(pb->b->data + (size_t)start * pb->b->d32 * 4, ba->bytes + off, bytes);
    return v2_undef();
}
static V2 ib_uploadFromVector(VM2 *vm, V2 self, int argc, V2 *argv) {
    PIBuf *pb = (PIBuf*)payload_v(self, PK_IBUF); A2Obj *v; u32 start, n, i;
    if (!pb) return v2_undef();
    v = nonnull_arg(vm, argc, argv, 0, "data"); if (!v) return v2_undef();
    start = u32_arg(vm, argc, argv, 1, 0); n = u32_arg(vm, argc, argv, 2, 0);
    if (!OK_) return v2_undef();
    if ((u32)v->alen < n) n = (u32)v->alen;
    s3d_ibuf_before_write(pb->b);
    for (i = 0; i < n && start + i < pb->b->n; i++) pb->b->data[start + i] = (u16)v2_touint(vm, v->arr[i]);
    return v2_undef();
}
static V2 ib_uploadFromByteArray(VM2 *vm, V2 self, int argc, V2 *argv) {
    PIBuf *pb = (PIBuf*)payload_v(self, PK_IBUF); A2Obj *ba; u32 off, start, n, i;
    if (!pb) return v2_undef();
    ba = nonnull_arg(vm, argc, argv, 0, "data"); if (!ba) return v2_undef();
    off = u32_arg(vm, argc, argv, 1, 0); start = u32_arg(vm, argc, argv, 2, 0); n = u32_arg(vm, argc, argv, 3, 0);
    if (!OK_ || ba->kind != OK_BYTEARRAY) return v2_undef();
    if ((size_t)off > (size_t)ba->blen || ((size_t)ba->blen - off) / 2 < n) { a2_throw_id(vm, "flash.errors::EOFError", 2030, NULL, NULL); return v2_undef(); }
    s3d_ibuf_before_write(pb->b);
    for (i = 0; i < n && start + i < pb->b->n; i++) pb->b->data[start + i] = (u16)(ba->bytes[off + i * 2] | (ba->bytes[off + i * 2 + 1] << 8));
    return v2_undef();
}

/* ---------------------------------------------------------------- */
/* textures                                                         */

static void upload_bitmap(S3DTex *t, int face, BitmapData *bd) {
    u32 x, y, w, h;
    if (!bd || bd->disposed || !bd->px || face < 0 || face >= t->nfaces) return;
    s3d_tex_before_write(t);
    w = bd->w < (u32)t->w ? bd->w : (u32)t->w; h = bd->h < (u32)t->h ? bd->h : (u32)t->h;
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
        u32 c = bd->px[(size_t)y * bd->w + x]; u8 *d = t->px[face] + ((size_t)y * t->w + x) * 4;
        d[0] = (u8)(c >> 16); d[1] = (u8)(c >> 8); d[2] = (u8)c; d[3] = (u8)(c >> 24);
    }
}
static void upload_bgra(VM2 *vm, S3DTex *t, int face, A2Obj *ba, u32 off) {
    size_t n = (size_t)t->w * t->h * 4, i; u8 *d;
    if (ba->kind != OK_BYTEARRAY || face < 0 || face >= t->nfaces) return;
    if ((size_t)off > (size_t)ba->blen || (size_t)ba->blen - off < n) { a2_throw_id(vm, "RangeError", 2006, NULL, NULL); return; }
    s3d_tex_before_write(t);
    d = t->px[face];
    for (i = 0; i < n; i += 4) { const u8 *s = ba->bytes + off + i; d[i] = s[2]; d[i + 1] = s[1]; d[i + 2] = s[0]; d[i + 3] = s[3]; }
}
static V2 tx_uploadFromBitmapData(VM2 *vm, V2 self, int argc, V2 *argv) {
    PTex *pt = (PTex*)payload_v(self, PK_TEX); A2Obj *bo; u32 side = 0, mip;
    if (!pt) return v2_undef();
    bo = nonnull_arg(vm, argc, argv, 0, "source"); if (!bo) return v2_undef();
    if (pt->t->kind == S3D_TEX_CUBE) { side = u32_arg(vm, argc, argv, 1, 0); mip = u32_arg(vm, argc, argv, 2, 0); }
    else mip = pt->t->kind == S3D_TEX_RECT ? 0 : u32_arg(vm, argc, argv, 1, 0);
    if (!OK_) return v2_undef();
    if (side > 5) { a2_throw_id(vm, "ArgumentError", 3673, NULL, NULL); return v2_undef(); }
    if (mip != 0) return v2_undef();   /* mip levels are not sampled */
    upload_bitmap(pt->t, (int)side, a2_bitmapdata_of(bo));
    return v2_undef();
}
static V2 tx_uploadFromByteArray(VM2 *vm, V2 self, int argc, V2 *argv) {
    PTex *pt = (PTex*)payload_v(self, PK_TEX); A2Obj *ba; u32 off, side = 0, mip = 0;
    if (!pt) return v2_undef();
    ba = nonnull_arg(vm, argc, argv, 0, "data"); if (!ba) return v2_undef();
    off = u32_arg(vm, argc, argv, 1, 0);
    if (pt->t->kind == S3D_TEX_CUBE) { side = u32_arg(vm, argc, argv, 2, 0); mip = u32_arg(vm, argc, argv, 3, 0); }
    else if (pt->t->kind == S3D_TEX_2D) mip = u32_arg(vm, argc, argv, 2, 0);
    if (!OK_) return v2_undef();
    if (side > 5) { a2_throw_id(vm, "ArgumentError", 3673, NULL, NULL); return v2_undef(); }
    if (mip != 0 || pt->format != S3D_FMT_BGRA) return v2_undef();
    upload_bgra(vm, pt->t, (int)side, ba, off);
    return v2_undef();
}
static void compressed_upload(VM2 *vm, PTex *pt, A2Obj *ba, u32 off) {
    S3DAtf a; int f;
    if (ba->kind != OK_BYTEARRAY || off > (u32)ba->blen || !s3d_atf_parse(ba->bytes + off, (u32)ba->blen - off, &a)) { a2_throw_id(vm, "Error", 3678, NULL, NULL); return; }
    if (a.w != pt->t->w || a.h != pt->t->h) { a2_throw_id(vm, "ArgumentError", 3679, NULL, NULL); return; }
    if ((pt->t->kind == S3D_TEX_CUBE) != (a.cube != 0)) { a2_throw_id(vm, "ArgumentError", 3675, NULL, NULL); return; }
    s3d_tex_before_write(pt->t);
    for (f = 0; f < pt->t->nfaces; f++) s3d_atf_decode(&a, f, pt->t->px[f]);
}
static V2 tx_uploadCompressed(VM2 *vm, V2 self, int argc, V2 *argv) {
    PTex *pt = (PTex*)payload_v(self, PK_TEX); A2Obj *ba; u32 off; int async;
    if (!pt) return v2_undef();
    ba = nonnull_arg(vm, argc, argv, 0, "data"); if (!ba) return v2_undef();
    off = u32_arg(vm, argc, argv, 1, 0); async = bool_arg(vm, argc, argv, 2, 0);
    if (!OK_) return v2_undef();
    if (pt->t->kind == S3D_TEX_CUBE ? pt->format != S3D_FMT_BGRA : !(pt->format == S3D_FMT_BGRA || pt->format == S3D_FMT_COMPRESSED_ALPHA)) return v2_undef();
    compressed_upload(vm, pt, ba, off);
    if (OK_ && async && self.t == T_OBJ) {
        /* textureReady comes later, from the frame loop */
        A2Obj *q; A2Prop *p = a2_dyn_find(vm->global, a2_str(vm, "__s3dTexReady"));
        if (!p || p->v.t != T_OBJ) { q = a2_new_array(vm); a2_dyn_set_hidden(vm, vm->global, a2_str(vm, "__s3dTexReady"), v2_obj(q)); } else q = p->v.u.o;
        a2_arr_push(vm, q, self);
    }
    return v2_undef();
}
void a2_stage3d_tick(VM2 *vm) {
    A2Prop *p = a2_dyn_find(vm->global, a2_str(vm, "__s3dTexReady")); A2Obj *q; int i, n; extern void a2_report_uncaught(VM2 *vm);
    if (!p || p->v.t != T_OBJ || !p->v.u.o->alen) return;
    q = p->v.u.o; n = q->alen;
    a2_dyn_set_hidden(vm, vm->global, a2_str(vm, "__s3dTexReady"), v2_obj(a2_new_array(vm)));
    for (i = 0; i < n; i++) if (q->arr[i].t == T_OBJ) { a2_dispatch_simple(vm, q->arr[i].u.o, a2_str(vm, "textureReady"), 0); if (vm->throwing) a2_report_uncaught(vm); }
}

/* ---------------------------------------------------------------- */

void a2_stage3d_init(VM2 *vm) {
    A2Class *c, *ed = vm->c_eventdispatcher;

    c_stage3d = c = a2_def_class(vm, "flash.display", "Stage3D", ed, OK_PLAIN, NULL);
    a2_add_method(vm, c, "requestContext3D", s3d_requestContext3D, 0);
    a2_add_method(vm, c, "requestContext3DMatchingProfiles", s3d_requestMatching, 1);
    a2_add_getter(vm, c, "context3D", s3d_get_context3D, NULL);
    a2_add_getter(vm, c, "x", s3d_get_x, s3d_set_x); a2_add_getter(vm, c, "y", s3d_get_y, s3d_set_y);
    a2_add_getter(vm, c, "visible", s3d_get_visible, s3d_set_visible);

    c_context3d = c = a2_def_class(vm, "flash.display3D", "Context3D", ed, OK_PLAIN, NULL); c->flags |= CF_FINAL;
    {
        static const struct { const char *n; NativeFn2 f; int np; } m[] = {
            { "clear", c3_clear, 0 }, { "configureBackBuffer", c3_configureBackBuffer, 3 }, { "createIndexBuffer", c3_createIndexBuffer, 1 },
            { "createVertexBuffer", c3_createVertexBuffer, 2 }, { "setVertexBufferAt", c3_setVertexBufferAt, 2 }, { "createProgram", c3_createProgram, 0 },
            { "setProgram", c3_setProgram, 1 }, { "drawTriangles", c3_drawTriangles, 1 }, { "present", c3_present, 0 }, { "setCulling", c3_setCulling, 1 },
            { "createTexture", c3_createTexture, 4 }, { "createCubeTexture", c3_createCubeTexture, 3 }, { "createRectangleTexture", c3_createRectangleTexture, 4 },
            { "createVideoTexture", c3_createVideoTexture, 0 }, { "setProgramConstantsFromMatrix", c3_setProgramConstantsFromMatrix, 3 },
            { "setProgramConstantsFromVector", c3_setProgramConstantsFromVector, 3 }, { "setProgramConstantsFromByteArray", c3_setProgramConstantsFromByteArray, 5 },
            { "setColorMask", c3_setColorMask, 4 }, { "setDepthTest", c3_setDepthTest, 2 }, { "setScissorRectangle", c3_setScissorRectangle, 1 },
            { "setRenderToBackBuffer", c3_setRenderToBackBuffer, 0 }, { "setBlendFactors", c3_setBlendFactors, 2 }, { "setTextureAt", c3_setTextureAt, 2 },
            { "setStencilReferenceValue", c3_setStencilReferenceValue, 1 }, { "setSamplerStateAt", c3_setSamplerStateAt, 4 },
            { "setRenderToTexture", c3_setRenderToTexture, 1 }, { "setStencilActions", c3_setStencilActions, 0 }, { "drawToBitmapData", c3_drawToBitmapData, 1 },
            { "dispose", c3_dispose, 0 }, { NULL, NULL, 0 } };
        int i; for (i = 0; m[i].n; i++) a2_add_method(vm, c, m[i].n, m[i].f, m[i].np);
    }
    a2_add_getter(vm, c, "driverInfo", c3_driverInfo, NULL);
    a2_add_getter(vm, c, "enableErrorChecking", c3_get_errorChecking, c3_set_errorChecking);
    a2_add_getter(vm, c, "profile", c3_profile, NULL);
    a2_add_getter(vm, c, "backBufferWidth", c3_backBufferWidth, NULL); a2_add_getter(vm, c, "backBufferHeight", c3_backBufferHeight, NULL);
    a2_add_getter(vm, c, "maxBackBufferWidth", c3_maxBackBuffer, c3_noop); a2_add_getter(vm, c, "maxBackBufferHeight", c3_maxBackBuffer, c3_noop);
    a2_static_getter(vm, c, "supportsVideoTexture", c3_supportsVideoTexture, NULL);

    c_program3d = c = a2_def_class(vm, "flash.display3D", "Program3D", vm->c_object, OK_PLAIN, NULL); c->flags |= CF_FINAL;
    a2_add_method(vm, c, "upload", p3_upload, 2); a2_add_method(vm, c, "dispose", res_dispose, 0);
    c_vbuf3d = c = a2_def_class(vm, "flash.display3D", "VertexBuffer3D", vm->c_object, OK_PLAIN, NULL); c->flags |= CF_FINAL;
    a2_add_method(vm, c, "uploadFromVector", vb_uploadFromVector, 3); a2_add_method(vm, c, "uploadFromByteArray", vb_uploadFromByteArray, 4); a2_add_method(vm, c, "dispose", res_dispose, 0);
    c_ibuf3d = c = a2_def_class(vm, "flash.display3D", "IndexBuffer3D", vm->c_object, OK_PLAIN, NULL); c->flags |= CF_FINAL;
    a2_add_method(vm, c, "uploadFromVector", ib_uploadFromVector, 3); a2_add_method(vm, c, "uploadFromByteArray", ib_uploadFromByteArray, 4); a2_add_method(vm, c, "dispose", res_dispose, 0);

    c_texbase = c = a2_def_class(vm, "flash.display3D.textures", "TextureBase", ed, OK_PLAIN, NULL);
    a2_add_method(vm, c, "dispose", res_dispose, 0);
    c_texture = c = a2_def_class(vm, "flash.display3D.textures", "Texture", c_texbase, OK_PLAIN, NULL); c->flags |= CF_FINAL;
    a2_add_method(vm, c, "uploadFromBitmapData", tx_uploadFromBitmapData, 1); a2_add_method(vm, c, "uploadFromByteArray", tx_uploadFromByteArray, 2);
    a2_add_method(vm, c, "uploadCompressedTextureFromByteArray", tx_uploadCompressed, 2);
    c_cubetex = c = a2_def_class(vm, "flash.display3D.textures", "CubeTexture", c_texbase, OK_PLAIN, NULL); c->flags |= CF_FINAL;
    a2_add_method(vm, c, "uploadFromBitmapData", tx_uploadFromBitmapData, 2); a2_add_method(vm, c, "uploadFromByteArray", tx_uploadFromByteArray, 3);
    a2_add_method(vm, c, "uploadCompressedTextureFromByteArray", tx_uploadCompressed, 2);
    c_recttex = c = a2_def_class(vm, "flash.display3D.textures", "RectangleTexture", c_texbase, OK_PLAIN, NULL); c->flags |= CF_FINAL;
    a2_add_method(vm, c, "uploadFromBitmapData", tx_uploadFromBitmapData, 1); a2_add_method(vm, c, "uploadFromByteArray", tx_uploadFromByteArray, 2);
    c_videotex = c = a2_def_class(vm, "flash.display3D.textures", "VideoTexture", c_texbase, OK_PLAIN, NULL); c->flags |= CF_FINAL;
    a2_add_method(vm, c, "attachNetStream", c3_noop, 1); a2_add_method(vm, c, "attachCamera", c3_noop, 1);
    a2_add_getter(vm, c, "videoWidth", c3_backBufferWidth, NULL); a2_add_getter(vm, c, "videoHeight", c3_backBufferHeight, NULL);
}
