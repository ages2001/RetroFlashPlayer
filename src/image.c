#include "image.h"
#include "reader.h"
#include "inflate.h"
#include <stdlib.h>
#include <string.h>

void bitmap_free(Bitmap *b) { extern void bmd_forget_symbol_image(const Bitmap *src); if (b) { bmd_forget_symbol_image(b); free(b->px); free(b->pm_own); free(b); } }

static Bitmap *decode_lossless(const SWFChar *ch, int v2) {
    Reader r;
    int fmt, w, h, ncolors = 0, stride, i, x, y;
    size_t need, got_len;
    const u8 *zdata; size_t zlen;
    u8 *raw;
    Bitmap *bm;
    long got;

    reader_init(&r, ch->data, ch->len);
    r_u16(&r);
    fmt = r_u8(&r);
    w = r_u16(&r); h = r_u16(&r);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return NULL;
    if (fmt == 3) ncolors = r_u8(&r) + 1;
    zdata = ch->data + r.pos; zlen = ch->len - r.pos;

    if (fmt == 3) {
        stride = (w + 3) & ~3;
        need = (size_t)ncolors * (v2 ? 4 : 3) + (size_t)stride * h;
    } else if (fmt == 4) {
        stride = ((w * 2) + 3) & ~3;
        need = (size_t)stride * h;
    } else if (fmt == 5) {
        stride = w * 4;
        need = (size_t)stride * h;
    } else return NULL;

    raw = (u8*)malloc(need);
    if (!raw) return NULL;
    got = swf_inflate_zlib(zdata, zlen, raw, need);
    {   /* more data than the format needs: inflated whole, the start is used (Ruffle) */
        size_t cap = need;
        while (got == FLASHNT_ENOMEM && cap < need * 16 + 65536) {
            u8 *nr = (u8*)realloc(raw, cap * 2);
            if (!nr) break;
            raw = nr; cap *= 2;
            got = swf_inflate_zlib(zdata, zlen, raw, cap);
        }
        if (got > (long)need) got = (long)need;
    }
    if (got < 0) { free(raw); return NULL; }
    got_len = (size_t)got;
    if (got_len < need) memset(raw + got_len, 0, need - got_len);

    bm = (Bitmap*)calloc(1, sizeof(Bitmap));
    if (!bm) { free(raw); return NULL; }
    bm->w = w; bm->h = h;
    bm->px = (u32*)malloc((size_t)w * h * sizeof(u32));
    if (!bm->px) { free(bm); free(raw); return NULL; }

    if (fmt == 3) {
        u32 pal[256];
        const u8 *p = raw;
        for (i = 0; i < ncolors && i < 256; i++) {
            if (v2) { pal[i] = ((u32)p[3] << 24) | ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2]; p += 4; }
            else    { pal[i] = 0xff000000u | ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2]; p += 3; }
        }
        for (; i < 256; i++) pal[i] = 0;
        if (v2 && (bm->pm_own = (u32*)malloc((size_t)w * h * sizeof(u32))) != NULL) {
            /* the premultiplied texels exactly as stored (Ruffle keeps them) */
            for (y = 0; y < h; y++) { const u8 *row = p + (size_t)y * stride; for (x = 0; x < w; x++) bm->pm_own[y * w + x] = pal[row[x]]; }
            bm->pm = bm->pm_own;
        }
        /* v2 palette is premultiplied: unpremultiply */
        if (v2) for (i = 0; i < 256; i++) {
            u32 a = pal[i] >> 24;
            if (a && a < 255) {
                u32 rr = ((pal[i] >> 16) & 255) * 255 / a, gg = ((pal[i] >> 8) & 255) * 255 / a, bb = (pal[i] & 255) * 255 / a;
                if (rr > 255) rr = 255; if (gg > 255) gg = 255; if (bb > 255) bb = 255;
                pal[i] = (a << 24) | (rr << 16) | (gg << 8) | bb;
            }
        }
        for (y = 0; y < h; y++) {
            const u8 *row = p + (size_t)y * stride;
            for (x = 0; x < w; x++) bm->px[y * w + x] = pal[row[x]];
        }
    } else if (fmt == 4) {
        for (y = 0; y < h; y++) {
            const u8 *row = raw + (size_t)y * stride;
            for (x = 0; x < w; x++) {
                u16 v = (u16)((row[x * 2] << 8) | row[x * 2 + 1]);  /* big-endian PIX15 */
                u32 rr = (((v >> 10) & 31) * 255 + 15) / 31, gg = (((v >> 5) & 31) * 255 + 15) / 31, bb = ((v & 31) * 255 + 15) / 31;   /* rounded (Ruffle) */
                bm->px[y * w + x] = 0xff000000u | (rr << 16) | (gg << 8) | bb;
            }
        }
    } else {
        if (v2 && (bm->pm_own = (u32*)malloc((size_t)w * h * sizeof(u32))) != NULL) {
            for (y = 0; y < h; y++) { const u8 *row = raw + (size_t)y * stride; for (x = 0; x < w; x++) bm->pm_own[y * w + x] = ((u32)row[x * 4] << 24) | ((u32)row[x * 4 + 1] << 16) | ((u32)row[x * 4 + 2] << 8) | row[x * 4 + 3]; }
            bm->pm = bm->pm_own;
        }
        for (y = 0; y < h; y++) {
            const u8 *row = raw + (size_t)y * stride;
            for (x = 0; x < w; x++) {
                u32 a = v2 ? row[x * 4] : 255;
                u32 rr = row[x * 4 + 1], gg = row[x * 4 + 2], bb = row[x * 4 + 3];
                if (v2 && a && a < 255) {       /* unpremultiply */
                    rr = rr * 255 / a; gg = gg * 255 / a; bb = bb * 255 / a;
                    if (rr > 255) rr = 255; if (gg > 255) gg = 255; if (bb > 255) bb = 255;
                }
                if (v2 && a == 0) rr = gg = bb = 0;
                bm->px[y * w + x] = (a << 24) | (rr << 16) | (gg << 8) | bb;
            }
        }
    }
    free(raw);
    return bm;
}

Bitmap *image_get(SWFMovie *m, int id) {
    SWFChar *ch = swf_get_char(m, id);
    Bitmap *bm = NULL;
    if (!ch || ch->type != CHAR_BITMAP) return NULL;
    if (ch->parsed) return (Bitmap*)ch->parsed;
    switch (ch->tagcode) {
    case TAG_DefineBitsLossless:  bm = decode_lossless(ch, 0); break;
    case TAG_DefineBitsLossless2: bm = decode_lossless(ch, 1); break;
    default: {
        extern Bitmap *jpeg_char_decode(SWFMovie *m, SWFChar *ch);
        bm = jpeg_char_decode(m, ch);
        break; }
    }
    ch->parsed = bm;
    return bm;
}
