/* avm2_shader.c - flash.display.Shader and friends (Pixel Bender), AVM2.
 *
 * Shader, ShaderData, ShaderParameter, ShaderInput, ShaderJob,
 * ShaderPrecision, ShaderParameterType, GraphicsShaderFill, plus the
 * ShaderEvent fields, the snapshots ShaderFilter and blendShader render
 * with, and DisplayObject.blendShader.  The classes follow Ruffle's
 * ActionScript (core/src/avm2/globals/flash/display/Shader*.as) and its
 * shader_data.rs / shader_parameter.rs / shader_job.rs natives; the
 * kernels run on the CPU interpreter of pbj.c. */
#include "avm2_flash_int.h"
#include "bmd.h"
#include "pbj.h"
#include "filters.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

static A2Class *c_shader, *c_shaderdata, *c_shaderparam, *c_shaderinput, *c_shaderjob, *c_shaderevent;
static NativeFn2 shaderevent_base_ctor;

#define OK_ (!vm->throwing)

extern A2Obj *a2_bitmapdata_wrap(VM2 *vm, BitmapData *b);
BitmapData *a2_bitmapdata_of(A2Obj *o);

static V2 sget(VM2 *vm, V2 self, const char *name) { return a2_get_s(vm, self, name); }
static void sset(VM2 *vm, V2 self, const char *name, V2 v) { if (OK_) a2_set_s(vm, self, name, v); }
static int is_a(V2 v, A2Class *c) { return c && v.t == T_OBJ && v.u.o->cls && a2_is_subclass(v.u.o->cls, c); }

/* Latin-1 bytes (names / metadata strings in the bytecode) as a string */
static V2 lat1_str(VM2 *vm, const char *s) {
    size_t n = s ? strlen(s) : 0, i, k = 0; char *b; V2 r;
    b = (char*)malloc(n * 2 + 1);
    if (!b) return v2_cstr(vm, "");
    for (i = 0; i < n; i++) {
        u8 c = (u8)s[i];
        if (c < 0x80) b[k++] = (char)c; else { b[k++] = (char)(0xC0 | (c >> 6)); b[k++] = (char)(0x80 | (c & 0x3F)); }
    }
    b[k] = 0; r = v2_str(a2_intern(vm, b, k)); free(b);
    return r;
}

/* a float as Flash reports metadata: whole numbers become ints */
static V2 meta_num(float f) {
    double d = f;
    if (d == floor(d) && d - d == 0) {   /* finite and whole */
        double m = fmod(d, 4294967296.0); u32 u;
        if (m < 0) m += 4294967296.0;
        u = (u32)m;
        return v2_int((s32)u);
    }
    return v2_num(d);
}
static V2 value_to_as(VM2 *vm, const PbjValue *v, int int_as_int) {
    A2Obj *a; int i, n = pbj_type_count(v->type);
    if (v->type == PBJ_STRING) return lat1_str(vm, v->s);
    if (v->type == PBJ_INT && int_as_int) return v2_int(v->i[0]);
    a = a2_new_array(vm);
    for (i = 0; i < n; i++) a2_arr_push(vm, a, pbj_type_is_float(v->type) ? meta_num(v->f[i]) : v2_int(v->i[i]));
    return v2_obj(a);
}

static void default_value(const PbjParam *p, PbjValue *out) {
    int i;
    memset(out, 0, sizeof(*out)); out->type = p->type;
    for (i = 0; i < p->nmeta; i++) if (!strcmp(p->meta[i].key, "defaultValue")) { pbj_value_copy(out, &p->meta[i].v); out->type = p->type; return; }
}

/* PixelBenderType::from_avm2_value: the parameter's `value` array */
static float to_f32(double d) {
    if (d != d) return (float)d;
    if (d > 3.4028234663852886e38) return (float)HUGE_VAL;
    if (d < -3.4028234663852886e38) return -(float)HUGE_VAL;
    return (float)d;
}
static int to_i16(double d) {   /* Rust `as i16` */
    if (d != d) return 0;
    if (d >= 32767.0) return 32767;
    if (d <= -32768.0) return -32768;
    return (int)d;
}
static int value_from_as(VM2 *vm, V2 val, int type, PbjValue *out) {
    A2Obj *arr = NULL; int i, n = pbj_type_count(type);
    memset(out, 0, sizeof(*out)); out->type = type;
    if (val.t == T_OBJ && (val.u.o->kind == OK_ARRAY || a2_is_subclass(val.u.o->cls, vm->c_array))) arr = val.u.o;
    if (type == PBJ_STRING) {
        if (arr && a2_arr_len(arr) > 0) {
            V2 e = a2_arr_get(arr, 0); A2Str *s;
            V2_NORM(e); s = v2_tostr(vm, e);
            if (!OK_) return 0;
            out->s = (char*)malloc(s->len + 1);
            if (out->s) { memcpy(out->s, s->s, s->len); out->s[s->len] = 0; }
        }
        return 1;
    }
    for (i = 0; i < n; i++) {
        double d = 0;
        if (arr && (u32)i < a2_arr_len(arr)) {
            V2 e = a2_arr_get(arr, i);
            if (e.t == T_HOLE) d = 0;
            else if (e.t == T_INT) d = e.u.i;
            else if (e.t == T_UINT) d = e.u.u;
            else if (e.t == T_NUM) d = e.u.n;
            else { a2_throw_id(vm, "ArgumentError", 2004, NULL, NULL); return 0; }
        }
        if (pbj_type_is_float(type)) out->f[i] = to_f32(d); else out->i[i] = to_i16(d);
    }
    return 1;
}

/* ---------------------------------------------------------------- */
/* ShaderData                                                       */

static PbjShader *shader_of_data(V2 data) { return is_a(data, c_shaderdata) ? (PbjShader*)data.u.o->payload : NULL; }

int a2_shader_free_payload(A2Obj *o) {
    if (!c_shaderdata || !o->cls || o->cls != c_shaderdata) return 0;
    pbj_unref((PbjShader*)o->payload); o->payload = NULL;
    return 1;
}

static V2 make_param(VM2 *vm, const PbjParam *p, int index) {
    V2 a[1]; V2 o; int i;
    if (p->texture) {
        o = a2_construct_class(vm, c_shaderinput, 0, a);
        if (!OK_) return v2_undef();
        sset(vm, o, "_channels", v2_int(p->channels)); sset(vm, o, "_index", v2_int(index));
        a2_dyn_set(vm, o.u.o, a2_str(vm, "name"), lat1_str(vm, p->name));
        return o;
    }
    o = a2_construct_class(vm, c_shaderparam, 0, a);
    if (!OK_) return v2_undef();
    sset(vm, o, "_index", v2_int(index)); sset(vm, o, "_type", v2_cstr(vm, pbj_type_name(p->type)));
    for (i = 0; i < p->nmeta && OK_; i++) {
        V2 v = value_to_as(vm, &p->meta[i].v, 0);
        a2_dyn_set(vm, o.u.o, lat1_str(vm, p->meta[i].key).u.s, v);
        if (!strcmp(p->meta[i].key, "defaultValue")) sset(vm, o, "_value", v);
    }
    a2_dyn_set(vm, o.u.o, a2_str(vm, "name"), lat1_str(vm, p->name));
    return o;
}

static V2 sd_setByteCode(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 b = ARG(0); PbjShader *s = NULL; int err, i, ni = 0, nt = 0; A2Obj *o = SELFO;
    if (b.t != T_OBJ) { a2_throw_id(vm, "TypeError", 2007, "bytecode", NULL); return v2_undef(); }
    if (!o || b.u.o->kind != OK_BYTEARRAY) { a2_throw_id(vm, "ArgumentError", 2004, NULL, NULL); return v2_undef(); }
    err = pbj_parse(b.u.o->bytes, b.u.o->bytes ? (size_t)b.u.o->blen : 0, &s);
    if (err == PBJ_ERR_EOF) { a2_throw_id(vm, "flash.errors::EOFError", 2030, NULL, NULL); return v2_undef(); }
    if (err) { a2_throw_id(vm, "ArgumentError", 2004, NULL, NULL); return v2_undef(); }
    for (i = 0; i < s->nmeta && OK_; i++) a2_dyn_set(vm, o, lat1_str(vm, s->meta[i].key).u.s, value_to_as(vm, &s->meta[i].v, 1));
    a2_dyn_set(vm, o, a2_str(vm, "name"), v2_cstr(vm, s->name ? s->name : ""));
    for (i = 0; i < s->nparams && OK_; i++) {
        const PbjParam *p = &s->params[i]; V2 po;
        if (!p->texture && (p->out || pbj_is_outcoord(p))) continue;
        po = make_param(vm, p, p->texture ? nt++ : ni++);
        if (!OK_) break;
        a2_dyn_set(vm, o, lat1_str(vm, p->name).u.s, po);
    }
    if (!OK_) { pbj_unref(s); return v2_undef(); }
    if (o->payload && o->cls == c_shaderdata) pbj_unref((PbjShader*)o->payload);
    o->payload = s;
    return v2_undef();
}
static V2 ShaderData_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 a[1]; a[0] = argc > 0 ? a2_coerce(vm, argv[0], vm->c_bytearray) : v2_null();
    if (!OK_) return v2_undef();
    a2_call_s(vm, self, "_setByteCode", 1, a);
    return v2_undef();
}

/* ---------------------------------------------------------------- */
/* Shader                                                           */

static V2 sh_set_byteCode(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 a[1], d;
    a[0] = a2_coerce(vm, ARG(0), vm->c_bytearray);
    if (!OK_) return v2_undef();
    d = a2_construct_class(vm, c_shaderdata, 1, a);
    if (OK_) sset(vm, self, "_data", d);
    return v2_undef();
}
static V2 Shader_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 b = argc > 0 ? a2_coerce(vm, argv[0], vm->c_bytearray) : v2_null();
    if (!OK_) return v2_undef();
    sset(vm, self, "_precisionHint", v2_cstr(vm, "full"));
    if (b.t == T_OBJ) a2_set_s(vm, self, "byteCode", b);
    return v2_undef();
}
static V2 sh_get_data(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return sget(vm, self, "_data"); }
static V2 sh_set_data(VM2 *vm, V2 self, int argc, V2 *argv) { V2 v = a2_coerce(vm, ARG(0), c_shaderdata); sset(vm, self, "_data", v); return v2_undef(); }
static V2 sh_get_precision(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return sget(vm, self, "_precisionHint"); }
static V2 sh_set_precision(VM2 *vm, V2 self, int argc, V2 *argv) { sset(vm, self, "_precisionHint", ARG(0)); return v2_undef(); }

/* ---------------------------------------------------------------- */
/* ShaderParameter / ShaderInput                                    */

#define ACC(pfx, field) \
    static V2 pfx##_get_##field(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return sget(vm, self, "_" #field); } \
    static V2 pfx##_set_##field(VM2 *vm, V2 self, int argc, V2 *argv) { sset(vm, self, "_" #field, ARG(0)); return v2_undef(); }
ACC(sp, index) ACC(sp, type) ACC(sp, value)
ACC(si, channels) ACC(si, height) ACC(si, index) ACC(si, input) ACC(si, width)
ACC(sj, shader) ACC(sj, target) ACC(sj, width) ACC(sj, height)
ACC(se, bitmapData) ACC(se, byteArray) ACC(se, vector)

/* ---------------------------------------------------------------- */
/* arguments: parameter values and image inputs                     */

typedef struct {
    PbjShader *sh;
    PbjValue  *vals;
    PbjTex     tex[16];
    void      *own[16];
} Args;

static void args_free(Args *a) {
    int i;
    if (a->vals && a->sh) for (i = 0; i < a->sh->nparams; i++) pbj_value_free(&a->vals[i]);
    free(a->vals); a->vals = NULL;
    for (i = 0; i < 16; i++) { free(a->own[i]); a->own[i] = NULL; }
}

static float le_float(const u8 *p) { u32 v = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); float f; memcpy(&f, &v, 4); return f; }

/* get_shader_args: copy: the inputs are copied (filters keep them) */
static int build_args(VM2 *vm, V2 shader, Args *a, int copy) {
    V2 data; PbjShader *s; int i;
    memset(a, 0, sizeof(*a));
    data = sget(vm, shader, "_data");
    if (!OK_) return 0;
    s = shader_of_data(data);
    if (!s) { a2_throw_id(vm, "ArgumentError", 2171, NULL, NULL); return 0; }
    a->sh = s;
    a->vals = (PbjValue*)calloc((size_t)(s->nparams ? s->nparams : 1), sizeof(PbjValue));
    if (!a->vals) return 0;
    for (i = 0; i < s->nparams; i++) {
        const PbjParam *p = &s->params[i]; A2Prop *pr; V2 pv;
        if (!p->texture && (p->out || pbj_is_outcoord(p))) continue;
        pr = a2_dyn_find(data.u.o, lat1_str(vm, p->name).u.s);
        pv = pr ? pr->v : v2_undef();
        if (!p->texture) {
            if (is_a(pv, c_shaderparam)) { V2 v = sget(vm, pv, "_value"); if (!OK_ || !value_from_as(vm, v, p->type, &a->vals[i])) { args_free(a); return 0; } }
            else default_value(p, &a->vals[i]);
            continue;
        }
        {
            int k = p->tex_index & 15, ch = p->channels, w, h; V2 in; PbjTex *t = &a->tex[k];
            if (!is_a(pv, c_shaderinput)) continue;
            in = sget(vm, pv, "_input"); w = v2_toint(vm, sget(vm, pv, "_width")); h = v2_toint(vm, sget(vm, pv, "_height"));
            if (!OK_) { args_free(a); return 0; }
            if (in.t != T_OBJ) continue;   /* no input: reads as transparent black */
            if (in.u.o->kind == OK_BITMAPDATA) {
                BitmapData *b = a2_bitmapdata_of(in.u.o);
                if (!b || b->disposed) continue;
                t->present = 1; t->w = t->pw = (int)b->w; t->h = t->ph = (int)b->h; t->px = b->px;
                if (copy && b->px) {
                    u32 *c = (u32*)malloc((size_t)b->w * b->h * 4 + 4);
                    if (c) { memcpy(c, b->px, (size_t)b->w * b->h * 4); a->own[k] = c; t->px = c; }
                }
            } else if (in.u.o->kind == OK_BYTEARRAY || in.u.o->kind == OK_VECTOR) {
                double need = (double)(w < 0 ? 0 : w) * (h < 0 ? 0 : h) * ch; size_t n, j; float *f;
                if (ch < 1 || ch > 4) continue;
                n = in.u.o->kind == OK_BYTEARRAY ? (size_t)(in.u.o->bytes ? in.u.o->blen : 0) / 4 : (size_t)in.u.o->alen;
                if ((double)(n / (size_t)ch) * ch < need) { char nb[260]; sprintf(nb, "%.250s", p->name ? p->name : ""); args_free(a); a2_throw_id(vm, "ArgumentError", 2165, nb, NULL); return 0; }
                f = (float*)malloc((size_t)need * sizeof(float) + 4);
                if (!f) continue;
                for (j = 0; j < (size_t)need; j++) {
                    if (in.u.o->kind == OK_BYTEARRAY) f[j] = le_float(in.u.o->bytes + j * 4);
                    else { V2 e = in.u.o->arr[j]; f[j] = to_f32(e.t == T_HOLE || e.t == T_UNDEF ? 0.0 : v2_tonum(vm, e)); }
                }
                a->own[k] = f; t->present = 1; t->w = w; t->h = h; t->ch = ch; t->f = f;
            } else { char nb[260]; sprintf(nb, "%.250s", p->name ? p->name : ""); args_free(a); a2_throw_id(vm, "ArgumentError", 2164, nb, NULL); return 0; }
        }
    }
    return 1;
}

/* a snapshot for ShaderFilter / blendShader; NULL (no error) without a shader */
PbjJob *a2_shader_snapshot(VM2 *vm, V2 shader) {
    Args a; PbjJob *j; int i;
    if (shader.t != T_OBJ) return NULL;
    if (!shader_of_data(sget(vm, shader, "_data"))) return NULL;
    if (!build_args(vm, shader, &a, 1)) return NULL;
    j = pbj_job_new(a.sh);
    if (!j) { args_free(&a); return NULL; }
    for (i = 0; i < a.sh->nparams; i++) { j->vals[i] = a.vals[i]; a.vals[i].s = NULL; }
    for (i = 0; i < 16; i++) { j->tex[i] = a.tex[i]; j->own[i] = a.own[i]; a.own[i] = NULL; }
    args_free(&a);
    return j;
}

/* ---------------------------------------------------------------- */
/* ShaderJob                                                        */

static V2 ShaderJob_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 sh = argc > 0 ? a2_coerce(vm, argv[0], c_shader) : v2_null(), tg = argc > 1 ? argv[1] : v2_null();
    s32 w = argc > 2 ? v2_toint(vm, argv[2]) : 0, h = argc > 3 ? v2_toint(vm, argv[3]) : 0;
    if (!OK_) return v2_undef();
    if (c_shaderjob->super && self.t == T_OBJ) a2_run_ctor(vm, c_shaderjob->super, self.u.o, 0, NULL);
    if (tg.t == T_UNDEF) tg = v2_null();
    sset(vm, self, "_shader", sh); sset(vm, self, "_target", tg); sset(vm, self, "_width", v2_int(w)); sset(vm, self, "_height", v2_int(h));
    return v2_undef();
}

static int write_target(VM2 *vm, V2 target, const float *out, int w, int h, int oc) {
    A2Obj *t = target.t == T_OBJ ? target.u.o : NULL; size_t n = (size_t)w * h, i; int k;
    if (t && t->kind == OK_BITMAPDATA) {
        BitmapData *b = a2_bitmapdata_of(t);
        if (!b || b->disposed) return 1;
        bmd_own(b);
        for (i = 0; i < n; i++) {
            const float *o = out + i * 4;
            u32 a = b->transparent ? pbj_unorm8(o[3]) : 255;
            b->px[i] = (a << 24) | ((u32)pbj_unorm8(o[0]) << 16) | ((u32)pbj_unorm8(o[1]) << 8) | pbj_unorm8(o[2]);
        }
        bmd_changed(b);
        return 1;
    }
    if (t && t->kind == OK_BYTEARRAY) {
        size_t need = n * (size_t)oc * 4;
        if ((size_t)t->bcap < need) { u8 *nb = (u8*)realloc(t->bytes, need); if (!nb) return 1; t->bytes = nb; t->bcap = (int)need; }
        for (i = 0; i < n; i++) for (k = 0; k < oc; k++) { u32 v; u8 *p = t->bytes + (i * (size_t)oc + (size_t)k) * 4; memcpy(&v, &out[i * 4 + (size_t)k], 4); p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
        if ((size_t)t->blen < need) t->blen = (int)need;
        return 1;
    }
    if (t && t->kind == OK_VECTOR) {
        size_t cnt = n * (size_t)oc;
        if ((size_t)t->acap < cnt) { V2 *na = (V2*)realloc(t->arr, (cnt ? cnt : 1) * sizeof(V2)); if (!na) return 1; t->arr = na; t->acap = (int)cnt; }
        for (i = 0; i < n; i++) for (k = 0; k < oc; k++) t->arr[i * (size_t)oc + (size_t)k] = v2_num(out[i * 4 + (size_t)k]);
        t->alen = (int)cnt;
        return 1;
    }
    a2_throw_id(vm, "ArgumentError", 2004, NULL, NULL);
    return 0;
}

/* runs the job now; 1 done */
static int run_job(VM2 *vm, V2 self) {
    V2 shader = sget(vm, self, "_shader"), target; Args a; int w, h, oc; float *out;
    if (!OK_) return 0;
    if (shader.t != T_OBJ) { a2_throw_id(vm, "TypeError", 2007, "shader", NULL); return 0; }
    if (!build_args(vm, shader, &a, 0)) return 0;
    target = sget(vm, self, "_target");
    if (target.t == T_OBJ && target.u.o->kind == OK_BITMAPDATA) {
        BitmapData *b = a2_bitmapdata_of(target.u.o);
        w = b && !b->disposed ? (int)b->w : 0; h = b && !b->disposed ? (int)b->h : 0;
    } else { w = v2_toint(vm, sget(vm, self, "_width")); h = v2_toint(vm, sget(vm, self, "_height")); }
    if (!OK_) { args_free(&a); return 0; }
    oc = pbj_output_channels(a.sh);
    if (oc != 3 && oc != 4) { args_free(&a); a2_throw_id(vm, "ArgumentError", 2162, NULL, NULL); return 0; }
    if (w < 0) w = 0; if (h < 0) h = 0;
    if ((double)w * h > 16777216.0) { args_free(&a); a2_throw_id(vm, "ArgumentError", 2004, NULL, NULL); return 0; }
    out = (float*)malloc((size_t)w * h * 4 * sizeof(float) + 16);
    if (!out) { args_free(&a); return 0; }
    pbj_run(a.sh, a.vals, a.tex, 16, 0, 0, w, h, 0, out);
    args_free(&a);
    write_target(vm, target, out, w, h, oc);
    free(out);
    return OK_;
}

static A2Str *s_pending(VM2 *vm) { return a2_str(vm, "__shaderJobsPending"); }

static V2 sj_start(VM2 *vm, V2 self, int argc, V2 *argv) {
    int wait = argc > 0 && v2_tobool(vm, argv[0]);
    if (wait) { run_job(vm, self); return v2_undef(); }
    {   /* asynchronous: the job finishes at the end of the frame, then dispatches `complete` */
        A2Prop *p = a2_dyn_find(vm->global, s_pending(vm)); A2Obj *arr;
        if (p && p->v.t == T_OBJ) arr = p->v.u.o; else { arr = a2_new_array(vm); a2_dyn_set_hidden(vm, vm->global, s_pending(vm), v2_obj(arr)); }
        a2_arr_push(vm, arr, self);
    }
    return v2_undef();
}
static V2 sj_cancel(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Prop *p = a2_dyn_find(vm->global, s_pending(vm)); int i; UNUSED;
    if (p && p->v.t == T_OBJ) { A2Obj *arr = p->v.u.o; for (i = 0; i < arr->alen; i++) if (arr->arr[i].t == T_OBJ && self.t == T_OBJ && arr->arr[i].u.o == self.u.o) arr->arr[i] = v2_null(); }
    return v2_undef();
}
static V2 sj_progress(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_int(0); }

void a2_shader_frame(VM2 *vm) {
    A2Prop *p = a2_dyn_find(vm->global, s_pending(vm)); A2Obj *arr; int i, n;
    extern void a2_report_uncaught(VM2 *vm);
    if (!p || p->v.t != T_OBJ || !p->v.u.o->alen) return;
    arr = p->v.u.o; n = arr->alen;
    a2_dyn_set_hidden(vm, vm->global, s_pending(vm), v2_obj(a2_new_array(vm)));
    for (i = 0; i < n; i++) {
        V2 job = arr->arr[i], target; A2Obj *ev;
        if (job.t != T_OBJ) continue;
        if (!run_job(vm, job)) { if (vm->throwing) a2_report_uncaught(vm); continue; }
        target = sget(vm, job, "_target");
        ev = a2_make_event(vm, c_shaderevent ? c_shaderevent : vm->c_event, vm->s_complete, 0, 0);
        if (!ev) continue;
        if (c_shaderevent && target.t == T_OBJ) {
            const char *f = target.u.o->kind == OK_BITMAPDATA ? "_bitmapData" : target.u.o->kind == OK_BYTEARRAY ? "_byteArray" : target.u.o->kind == OK_VECTOR ? "_vector" : NULL;
            if (f) sset(vm, v2_obj(ev), f, target);
        }
        a2_dispatch_event_obj(vm, job.u.o, ev);
        if (vm->throwing) a2_report_uncaught(vm);
    }
}

/* ---------------------------------------------------------------- */
/* ShaderEvent                                                      */

static V2 ShaderEvent_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    if (shaderevent_base_ctor) shaderevent_base_ctor(vm, self, argc < 3 ? argc : 3, argv);
    if (!OK_) return v2_undef();
    if (argc > 3) sset(vm, self, "_bitmapData", a2_coerce(vm, argv[3], vm->c_bitmapdata));
    if (argc > 4) sset(vm, self, "_byteArray", a2_coerce(vm, argv[4], vm->c_bytearray));
    if (argc > 5) sset(vm, self, "_vector", argv[5]);
    return v2_undef();
}

/* ---------------------------------------------------------------- */
/* DisplayObject.blendShader                                        */

V2 a2_set_blend_shader(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 sh = a2_coerce(vm, ARG(0), c_shader), data; A2Obj *o = SELFO; DisplayObject *d; PbjJob *j;
    if (!OK_) return v2_undef();
    if (sh.t != T_OBJ) { a2_throw_id(vm, "TypeError", 2007, "shader", NULL); return v2_undef(); }
    data = sget(vm, sh, "_data");
    if (!OK_) return v2_undef();
    if (!shader_of_data(data)) { a2_throw_id(vm, "TypeError", 2007, "data", NULL); return v2_undef(); }
    if (!o || o->kind != OK_DISPLAY) return v2_undef();
    j = a2_shader_snapshot(vm, sh);
    if (!OK_) return v2_undef();
    d = a2_dobj_of(o);
    a2_disp(o)->blend = 15;
    if (d) { pbj_job_unref((PbjJob*)d->blend_shader); d->blend_shader = j; d->blend = 15; }
    else pbj_job_unref(j);
    return v2_undef();
}

/* ---------------------------------------------------------------- */
/* GraphicsShaderFill                                               */

static V2 GraphicsShaderFill_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 sh = argc > 0 ? a2_coerce(vm, argv[0], c_shader) : v2_null(), m = argc > 1 ? a2_coerce(vm, argv[1], vm->c_matrix) : v2_null();
    if (!OK_) return v2_undef();
    sset(vm, self, "shader", sh); sset(vm, self, "matrix", m);
    return v2_undef();
}
static void add_iface(VM2 *vm, A2Class *c, const char *qn) {
    A2Class *ic = a2_class_find_s(vm, qn); A2Class **ni;
    if (!ic) return;
    ni = (A2Class**)realloc(c->ifaces, (size_t)(c->nifaces + 1) * sizeof(A2Class*));
    if (!ni) return;
    c->ifaces = ni; c->ifaces[c->nifaces++] = ic;
}

/* ---------------------------------------------------------------- */

void a2_shader_init(VM2 *vm) {
    A2Class *c;
    (void)sp_set_index; (void)sp_set_type; (void)si_set_channels; (void)si_set_index;

    c_shaderdata = c = a2_def_class(vm, "flash.display", "ShaderData", vm->c_object, OK_PLAIN, ShaderData_ctor);
    c->is_dynamic = 1; c->flags |= CF_FINAL; c->ctor_sig = "(flash.utils::ByteArray)";
    a2_add_method(vm, c, "_setByteCode", sd_setByteCode, 1);

    c_shader = c = a2_def_class(vm, "flash.display", "Shader", vm->c_object, OK_PLAIN, Shader_ctor);
    c->ctor_sig = "(flash.utils::ByteArray=)";
    a2_add_slot(vm, c, "_data", c_shaderdata); a2_add_slot(vm, c, "_precisionHint", vm->c_string);
    a2_add_getter(vm, c, "byteCode", NULL, sh_set_byteCode);
    a2_add_getter(vm, c, "data", sh_get_data, sh_set_data);
    a2_add_getter(vm, c, "precisionHint", sh_get_precision, sh_set_precision);

    c_shaderparam = c = a2_def_class(vm, "flash.display", "ShaderParameter", vm->c_object, OK_PLAIN, NULL);
    c->is_dynamic = 1; c->flags |= CF_FINAL;
    a2_add_slot(vm, c, "_index", vm->c_int); a2_add_slot(vm, c, "_type", vm->c_string); a2_add_slot(vm, c, "_value", vm->c_array);
    a2_add_getter(vm, c, "index", sp_get_index, NULL); a2_add_getter(vm, c, "type", sp_get_type, NULL); a2_add_getter(vm, c, "value", sp_get_value, sp_set_value);

    c_shaderinput = c = a2_def_class(vm, "flash.display", "ShaderInput", vm->c_object, OK_PLAIN, NULL);
    c->is_dynamic = 1; c->flags |= CF_FINAL;
    a2_add_slot(vm, c, "_channels", vm->c_int); a2_add_slot(vm, c, "_height", vm->c_int); a2_add_slot(vm, c, "_index", vm->c_int);
    a2_add_slot(vm, c, "_input", vm->c_object); a2_add_slot(vm, c, "_width", vm->c_int);
    a2_add_getter(vm, c, "channels", si_get_channels, NULL); a2_add_getter(vm, c, "height", si_get_height, si_set_height);
    a2_add_getter(vm, c, "index", si_get_index, NULL); a2_add_getter(vm, c, "input", si_get_input, si_set_input); a2_add_getter(vm, c, "width", si_get_width, si_set_width);

    c_shaderjob = c = a2_def_class(vm, "flash.display", "ShaderJob", vm->c_eventdispatcher, OK_PLAIN, ShaderJob_ctor);
    c->ctor_sig = "(flash.display::Shader=,Object=,int=,int=)";
    a2_add_slot(vm, c, "_shader", c_shader); a2_add_slot(vm, c, "_target", vm->c_object); a2_add_slot(vm, c, "_width", vm->c_int); a2_add_slot(vm, c, "_height", vm->c_int);
    a2_add_method(vm, c, "start", sj_start, 1); a2_add_method(vm, c, "cancel", sj_cancel, 0);
    a2_add_getter(vm, c, "shader", sj_get_shader, sj_set_shader); a2_add_getter(vm, c, "target", sj_get_target, sj_set_target);
    a2_add_getter(vm, c, "width", sj_get_width, sj_set_width); a2_add_getter(vm, c, "height", sj_get_height, sj_set_height);
    a2_add_getter(vm, c, "progress", sj_progress, NULL);

    c = a2_def_class(vm, "flash.display", "ShaderPrecision", vm->c_object, OK_PLAIN, NULL); c->flags |= CF_FINAL;
    a2_const_str(vm, c, "FULL", "full"); a2_const_str(vm, c, "FAST", "fast");
    c = a2_def_class(vm, "flash.display", "ShaderParameterType", vm->c_object, OK_PLAIN, NULL); c->flags |= CF_FINAL;
    {
        static const char *const k[] = { "FLOAT", "float", "FLOAT2", "float2", "FLOAT3", "float3", "FLOAT4", "float4", "INT", "int", "INT2", "int2", "INT3", "int3", "INT4", "int4",
            "BOOL", "bool", "BOOL2", "bool2", "BOOL3", "bool3", "BOOL4", "bool4", "MATRIX2X2", "matrix2x2", "MATRIX3X3", "matrix3x3", "MATRIX4X4", "matrix4x4", NULL };
        int i; for (i = 0; k[i]; i += 2) a2_const_str(vm, c, k[i], k[i + 1]);
    }

    c = a2_def_class(vm, "flash.display", "GraphicsShaderFill", vm->c_object, OK_PLAIN, GraphicsShaderFill_ctor); c->flags |= CF_FINAL;
    a2_add_slot(vm, c, "shader", c_shader); a2_add_slot(vm, c, "matrix", vm->c_matrix);
    c->ctor_sig = "(flash.display::Shader=,flash.geom::Matrix=)";
    add_iface(vm, c, "flash.display::IGraphicsFill"); add_iface(vm, c, "flash.display::IGraphicsData");

    c_shaderevent = a2_class_find_s(vm, "flash.events::ShaderEvent");
    if (c_shaderevent && c_shaderevent->native_ctor != ShaderEvent_ctor) {
        c = c_shaderevent; shaderevent_base_ctor = c->native_ctor; c->native_ctor = ShaderEvent_ctor;
        a2_add_slot(vm, c, "_bitmapData", vm->c_object); a2_add_slot(vm, c, "_byteArray", vm->c_object); a2_add_slot(vm, c, "_vector", vm->c_object);   /* typed by the constructor (BitmapData is defined later) */
        a2_add_getter(vm, c, "bitmapData", se_get_bitmapData, se_set_bitmapData); a2_add_getter(vm, c, "byteArray", se_get_byteArray, se_set_byteArray);
        a2_add_getter(vm, c, "vector", se_get_vector, se_set_vector);
        a2_const_str(vm, c, "COMPLETE", "complete");
        c->ctor_sig = "(String,Boolean=,Boolean=,flash.display::BitmapData=,flash.utils::ByteArray=,__AS3__.vec::Vector.<Number>=)";
    }
}
