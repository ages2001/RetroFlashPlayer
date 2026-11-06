/* reader.h - bit/byte reader over an in-memory SWF buffer (LSB-first bytes,
 * MSB-first bit fields, exactly as the SWF spec defines). */
#ifndef FLASHNT_READER_H
#define FLASHNT_READER_H

#include "flashnt.h"

typedef struct {
    const u8 *buf;
    size_t    len;
    size_t    pos;   /* byte cursor */
    unsigned  bit;   /* 0..7, bits already consumed from buf[pos] */
} Reader;

void reader_init(Reader *r, const u8 *buf, size_t len);

/* byte-aligned primitives (align first if mid-byte) */
void reader_align(Reader *r);
int  reader_eof(const Reader *r);
u8   r_u8 (Reader *r);
u16  r_u16(Reader *r);
u32  r_u32(Reader *r);
s16  r_s16(Reader *r);
s32  r_s32(Reader *r);
float r_f32(Reader *r);           /* IEEE 32-bit little endian */
double r_fixed(Reader *r);        /* 16.16 signed */
double r_fixed8(Reader *r);       /* 8.8 signed */

/* raw access */
const u8 *r_bytes(Reader *r, size_t n);  /* returns ptr, advances; NULL if short */
void      r_skip(Reader *r, size_t n);

/* null-terminated string (advances past the NUL) */
const char *r_string(Reader *r);

/* bit fields, MSB first */
u32 rb_u(Reader *r, unsigned nbits);     /* unsigned */
s32 rb_s(Reader *r, unsigned nbits);     /* signed (sign-extended) */
double rb_fb(Reader *r, unsigned nbits); /* 16.16 fixed bit field */

/* SWF structured readers */
void r_rect  (Reader *r, SWFRect *out);
void r_matrix(Reader *r, SWFMatrix *out);
void r_cxform(Reader *r, SWFCxform *out, int with_alpha);
void r_rgb   (Reader *r, SWFColor *out);
void r_rgba  (Reader *r, SWFColor *out);

#endif
