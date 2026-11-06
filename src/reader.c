#include "reader.h"
#include <string.h>

void reader_init(Reader *r, const u8 *buf, size_t len) {
    r->buf = buf; r->len = len; r->pos = 0; r->bit = 0;
}

void reader_align(Reader *r) {
    if (r->bit) { r->bit = 0; r->pos++; }
}

int reader_eof(const Reader *r) {
    return r->pos >= r->len;
}

u8 r_u8(Reader *r) {
    reader_align(r);
    if (r->pos >= r->len) return 0;
    return r->buf[r->pos++];
}

u16 r_u16(Reader *r) {
    u16 lo = r_u8(r);
    u16 hi = r_u8(r);
    return (u16)(lo | (hi << 8));
}

u32 r_u32(Reader *r) {
    u32 b0 = r_u8(r);
    u32 b1 = r_u8(r);
    u32 b2 = r_u8(r);
    u32 b3 = r_u8(r);
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

s16 r_s16(Reader *r) { return (s16)r_u16(r); }
s32 r_s32(Reader *r) { return (s32)r_u32(r); }

float r_f32(Reader *r) {
    union { u32 i; float f; } u;
    u.i = r_u32(r);
    return u.f;
}

double r_fixed(Reader *r) {
    s32 v = r_s32(r);
    return (double)v / 65536.0;
}

double r_fixed8(Reader *r) {
    s16 v = r_s16(r);
    return (double)v / 256.0;
}

const u8 *r_bytes(Reader *r, size_t n) {
    const u8 *p;
    reader_align(r);
    if (r->pos + n > r->len) return NULL;
    p = r->buf + r->pos;
    r->pos += n;
    return p;
}

void r_skip(Reader *r, size_t n) {
    reader_align(r);
    r->pos += n;
    if (r->pos > r->len) r->pos = r->len;
}

const char *r_string(Reader *r) {
    const char *s;
    reader_align(r);
    if (r->pos >= r->len) return "";
    s = (const char *)(r->buf + r->pos);
    while (r->pos < r->len && r->buf[r->pos]) r->pos++;
    if (r->pos < r->len) r->pos++; /* skip NUL */
    return s;
}

u32 rb_u(Reader *r, unsigned nbits) {
    u32 v = 0;
    unsigned i;
    for (i = 0; i < nbits; i++) {
        unsigned bitval;
        if (r->pos >= r->len) return v << (nbits - i - 1);
        bitval = (r->buf[r->pos] >> (7 - r->bit)) & 1u;
        v = (v << 1) | bitval;
        if (++r->bit == 8) { r->bit = 0; r->pos++; }
    }
    return v;
}

s32 rb_s(Reader *r, unsigned nbits) {
    u32 v = rb_u(r, nbits);
    if (nbits && (v & (1u << (nbits - 1))))
        v |= ~((nbits >= 32) ? 0u : ((1u << nbits) - 1u)); /* sign extend */
    return (s32)v;
}

double rb_fb(Reader *r, unsigned nbits) {
    s32 v = rb_s(r, nbits);
    return (double)v / 65536.0;
}

void r_rect(Reader *r, SWFRect *out) {
    unsigned nb;
    reader_align(r);
    nb = rb_u(r, 5);
    out->xmin = rb_s(r, nb);
    out->xmax = rb_s(r, nb);
    out->ymin = rb_s(r, nb);
    out->ymax = rb_s(r, nb);
    reader_align(r);
}

void r_matrix(Reader *r, SWFMatrix *out) {
    swf_matrix_identity(out);
    reader_align(r);
    if (rb_u(r, 1)) {           /* HasScale */
        unsigned nb = rb_u(r, 5);
        out->sx = rb_fb(r, nb);
        out->sy = rb_fb(r, nb);
    }
    if (rb_u(r, 1)) {           /* HasRotate */
        unsigned nb = rb_u(r, 5);
        out->r0 = rb_fb(r, nb);
        out->r1 = rb_fb(r, nb);
    }
    {
        unsigned nb = rb_u(r, 5);
        out->tx = rb_s(r, nb);
        out->ty = rb_s(r, nb);
    }
    reader_align(r);
}

void r_cxform(Reader *r, SWFCxform *out, int with_alpha) {
    unsigned has_add, has_mul, nb;
    swf_cxform_identity(out);
    reader_align(r);
    has_add = rb_u(r, 1);
    has_mul = rb_u(r, 1);
    nb = rb_u(r, 4);
    if (has_mul) {
        out->mr = (s16)rb_s(r, nb);
        out->mg = (s16)rb_s(r, nb);
        out->mb = (s16)rb_s(r, nb);
        out->ma = with_alpha ? (s16)rb_s(r, nb) : 256;
    }
    if (has_add) {
        out->ar = (s16)rb_s(r, nb);
        out->ag = (s16)rb_s(r, nb);
        out->ab = (s16)rb_s(r, nb);
        out->aa = with_alpha ? (s16)rb_s(r, nb) : 0;
    }
    reader_align(r);
}

void r_rgb(Reader *r, SWFColor *out) {
    out->r = r_u8(r); out->g = r_u8(r); out->b = r_u8(r); out->a = 255;
}

void r_rgba(Reader *r, SWFColor *out) {
    out->r = r_u8(r); out->g = r_u8(r); out->b = r_u8(r); out->a = r_u8(r);
}
