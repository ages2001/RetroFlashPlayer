/* filters.c - SWF filter records (see filters.h). */
#include "filters.h"
#include "bmd.h"
#include "pbj.h"
#include "reader.h"
#include <stdlib.h>
#include <string.h>

s32 sf_fixed16(double v) {
    double x = v * 65536.0;
    if (x != x) return 0;
    if (x >= 2147483647.0) return 2147483647;
    if (x <= -2147483648.0) return (s32)0x80000000;
    return (s32)x;
}
s16 sf_fixed8(double v) {
    double x = v * 256.0;
    if (x != x) return 0;
    if (x >= 32767.0) return 32767;
    if (x <= -32768.0) return -32768;
    return (s16)x;
}
u8 sf_u8_sat(double v) {
    if (v != v || v <= 0) return 0;
    if (v >= 255) return 255;
    return (u8)v;
}

static void rd_rgba(Reader *r, u8 *c) { c[0] = r_u8(r); c[1] = r_u8(r); c[2] = r_u8(r); c[3] = r_u8(r); }
static float rd_float(Reader *r) { union { u32 u; float f; } x; x.u = r_u32(r); return x.f; }

int swf_filters_parse(const u8 *data, size_t len, SwfFilter **out) {
    Reader r; int n, i, k, cnt = 0; SwfFilter *list;
    *out = NULL;
    if (!data || len < 1) return 0;
    reader_init(&r, data, len);
    n = r_u8(&r);
    if (n <= 0) return 0;
    list = (SwfFilter*)calloc((size_t)n, sizeof(SwfFilter));
    if (!list) return 0;
    for (i = 0; i < n && !reader_eof(&r); i++) {
        SwfFilter *f = &list[cnt];
        memset(f, 0, sizeof(*f));
        f->kind = r_u8(&r);
        switch (f->kind) {
        case SF_DROPSHADOW:
            rd_rgba(&r, f->color); f->blur_x = (s32)r_u32(&r); f->blur_y = (s32)r_u32(&r);
            f->angle = (s32)r_u32(&r); f->distance = (s32)r_u32(&r); f->strength = (s16)r_u16(&r);
            f->flags = r_u8(&r); f->passes = f->flags & 0x1F;
            break;
        case SF_BLUR:
            f->blur_x = (s32)r_u32(&r); f->blur_y = (s32)r_u32(&r); f->flags = r_u8(&r); f->passes = f->flags >> 3;
            break;
        case SF_GLOW:
            rd_rgba(&r, f->color); f->blur_x = (s32)r_u32(&r); f->blur_y = (s32)r_u32(&r);
            f->strength = (s16)r_u16(&r); f->flags = r_u8(&r); f->passes = f->flags & 0x1F;
            break;
        case SF_BEVEL:
            /* the spec has the colours swapped: highlight comes first */
            rd_rgba(&r, f->color2); rd_rgba(&r, f->color);
            f->blur_x = (s32)r_u32(&r); f->blur_y = (s32)r_u32(&r); f->angle = (s32)r_u32(&r); f->distance = (s32)r_u32(&r);
            f->strength = (s16)r_u16(&r); f->flags = r_u8(&r); f->passes = f->flags & 0x0F;
            break;
        case SF_GRADIENTGLOW: case SF_GRADIENTBEVEL: {
            int nc = r_u8(&r);
            for (k = 0; k < nc; k++) { u8 c[4]; rd_rgba(&r, c); if (k < 16) memcpy(f->gcolor[k], c, 4); }
            for (k = 0; k < nc; k++) { u8 ra = r_u8(&r); if (k < 16) f->ratio[k] = ra; }
            f->ncolors = nc < 16 ? nc : 16;
            f->blur_x = (s32)r_u32(&r); f->blur_y = (s32)r_u32(&r); f->angle = (s32)r_u32(&r); f->distance = (s32)r_u32(&r);
            f->strength = (s16)r_u16(&r); f->flags = r_u8(&r); f->passes = f->flags & 0x0F;
            break; }
        case SF_CONVOLUTION:
            f->mx = r_u8(&r); f->my = r_u8(&r); f->divisor = rd_float(&r); f->bias = rd_float(&r);
            if (f->mx * f->my > 0) f->cmatrix = (float*)calloc((size_t)(f->mx * f->my), sizeof(float));
            for (k = 0; k < f->mx * f->my; k++) { float x = rd_float(&r); if (f->cmatrix) f->cmatrix[k] = x; }
            if (!f->cmatrix) f->mx = f->my = 0;
            f->ncm = f->mx * f->my;
            rd_rgba(&r, f->color); f->flags = r_u8(&r);
            break;
        case SF_COLORMATRIX:
            for (k = 0; k < 20; k++) f->m[k] = rd_float(&r);
            break;
        default:
            i = n; continue;        /* unknown: stop */
        }
        cnt++;
    }
    if (!cnt) { free(list); return 0; }
    *out = list;
    return cnt;
}

void swf_filters_free(SwfFilter *f, int n) {
    int i;
    if (!f) return;
    for (i = 0; i < n; i++) { free(f[i].cmatrix); if (f[i].map_bmd) bmd_unref(f[i].map_bmd); pbj_job_unref(f[i].pbj); }
    free(f);
}

SwfFilter *swf_filters_copy(const SwfFilter *f, int n) {
    SwfFilter *c; int i;
    if (!f || n <= 0) return NULL;
    c = (SwfFilter*)malloc((size_t)n * sizeof(SwfFilter));
    if (!c) return NULL;
    memcpy(c, f, (size_t)n * sizeof(SwfFilter));
    for (i = 0; i < n; i++) if (f[i].cmatrix && f[i].ncm > 0) {
        c[i].cmatrix = (float*)malloc((size_t)f[i].ncm * sizeof(float));
        if (c[i].cmatrix) memcpy(c[i].cmatrix, f[i].cmatrix, (size_t)f[i].ncm * sizeof(float));
        else c[i].ncm = 0;
    } else c[i].cmatrix = NULL;
    for (i = 0; i < n; i++) { if (c[i].map_bmd) bmd_ref(c[i].map_bmd); pbj_job_ref(c[i].pbj); }
    return c;
}
