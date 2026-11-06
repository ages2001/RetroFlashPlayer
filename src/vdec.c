/* vdec.c - video decoder front end (see vdec.h): codec dispatch, Screen
 * Video (v1; v2 decoded as v1) and the YUV 4:2:0 to RGB conversion shared
 * by the H.263 and VP6 decoders. */
#include "vdec.h"
#include "image.h"
#include "inflate.h"
#include <stdlib.h>
#include <string.h>

struct VDec {
    int codec, deblocking;
    H263Dec *h263;
    VP6Dec *vp6;
    H264Dec *h264;
    Bitmap *img;            /* screen video picture (owned) */
    const Bitmap *out;      /* the last picture */
};

VDec *vdec_create(int codec, int deblocking) {
    VDec *d;
    if (codec != VCODEC_H263 && codec != VCODEC_SCREEN && codec != VCODEC_SCREEN2 && codec != VCODEC_VP6 && codec != VCODEC_VP6A && codec != VCODEC_H264) return NULL;
    d = (VDec*)calloc(1, sizeof(VDec));
    if (!d) return NULL;
    d->codec = codec; d->deblocking = deblocking;
    if (codec == VCODEC_H263) d->h263 = h263_create();
    if (codec == VCODEC_VP6 || codec == VCODEC_VP6A) d->vp6 = vp6_create(codec == VCODEC_VP6A);
    if (codec == VCODEC_H264) d->h264 = h264_create();
    return d;
}
void vdec_free(VDec *d) {
    if (!d) return;
    if (d->h263) h263_free(d->h263);
    if (d->vp6) vp6_free(d->vp6);
    if (d->h264) h264_free(d->h264);
    bitmap_free(d->img);
    free(d);
}
const Bitmap *vdec_image(VDec *d) { return d ? d->out : NULL; }

/* Screen Video: the packet as in a SWF VideoFrame (an FLV tag without
   its frame-type/codec byte): the block layout, then the blocks */
static int screen_decode(VDec *v, const u8 *d, size_t n) {
    size_t pos; int bw, bh, iw, ih, bx, by, nbx, nby; u8 *tmp;
    if (n < 4) return 0;
    pos = 0;
    bw = ((d[pos] >> 4) + 1) * 16; iw = ((d[pos] & 15) << 8) | d[pos + 1];
    bh = ((d[pos + 2] >> 4) + 1) * 16; ih = ((d[pos + 2] & 15) << 8) | d[pos + 3];
    pos += 4;
    if (v->codec == VCODEC_SCREEN2) { if (pos >= n) return 0; pos++; }   /* v2 flags (palette / diff blocks are not used) */
    if (iw <= 0 || ih <= 0 || iw > 4096 || ih > 4096) return 0;
    if (!v->img || v->img->w != iw || v->img->h != ih) {
        bitmap_free(v->img);
        v->img = (Bitmap*)calloc(1, sizeof(Bitmap));
        if (!v->img) return 0;
        v->img->w = iw; v->img->h = ih; v->img->px = (u32*)calloc((size_t)iw * ih, sizeof(u32));
        if (!v->img->px) { free(v->img); v->img = NULL; return 0; }
    }
    nbx = (iw + bw - 1) / bw; nby = (ih + bh - 1) / bh;
    tmp = (u8*)malloc((size_t)bw * bh * 3);
    if (!tmp) return 0;
    for (by = 0; by < nby; by++) for (bx = 0; bx < nbx; bx++) {
        int size, cw, chh, x, y; long got;
        if (pos + 2 > n) break;
        size = (d[pos] << 8) | d[pos + 1]; pos += 2;
        if (size == 0) continue;                      /* unchanged */
        if (pos + (size_t)size > n) break;
        cw = (bx + 1) * bw > iw ? iw - bx * bw : bw;
        chh = (by + 1) * bh > ih ? ih - by * bh : bh;
        got = swf_inflate_zlib(d + pos, (size_t)size, tmp, (size_t)cw * chh * 3);
        pos += (size_t)size;
        if (got < (long)((size_t)cw * chh * 3)) continue;
        /* blocks go bottom-up from the bottom left; rows bottom-up, pixels BGR */
        for (y = 0; y < chh; y++) {
            int py = ih - 1 - (by * bh + y);
            const u8 *row = tmp + (size_t)y * cw * 3;
            if (py < 0 || py >= ih) continue;
            for (x = 0; x < cw; x++) {
                int px = bx * bw + x;
                if (px >= iw) break;
                v->img->px[(size_t)py * iw + px] = 0xff000000u | ((u32)row[x * 3 + 2] << 16) | ((u32)row[x * 3 + 1] << 8) | row[x * 3];
            }
        }
    }
    free(tmp);
    v->out = v->img;
    return 1;
}

int vdec_decode(VDec *d, const u8 *data, size_t len) {
    Bitmap *bm = NULL;
    if (!d || !data || !len) return 0;
    switch (d->codec) {
    case VCODEC_H263: if (h263_decode(d->h263, data, len, d->deblocking, &bm) && bm) { d->out = bm; return 1; } return 0;
    case VCODEC_VP6: case VCODEC_VP6A: if (vp6_decode(d->vp6, data, len, &bm) && bm) { d->out = bm; return 1; } return 0;
    case VCODEC_SCREEN: case VCODEC_SCREEN2: return screen_decode(d, data, len);
    case VCODEC_H264: if (d->h264 && h264_decode(d->h264, data, len, &bm) && bm) { d->out = bm; return 1; } return 0;
    }
    return 0;
}

int vdec_is_keyframe(int codec, const u8 *data, size_t len) {
    if (!data || !len) return 0;
    switch (codec) {
    case VCODEC_H263:   /* picture type after the 17-bit start code, version, TR and size */
        { size_t bit = 17 + 5 + 8; int fmt, pt; unsigned v = 0; size_t i;
          if (len < 6) return 0;
          for (i = 0; i < 3; i++) v = (v << 1) | ((data[(bit + i) >> 3] >> (7 - ((bit + i) & 7))) & 1);
          fmt = (int)v; bit += 3 + (fmt == 0 ? 16 : fmt == 1 ? 32 : 0);
          if ((bit + 2) / 8 >= len) return 0;
          pt = ((data[bit >> 3] >> (7 - (bit & 7))) & 1) << 1; bit++; pt |= (data[bit >> 3] >> (7 - (bit & 7))) & 1;
          return pt == 0; }
    case VCODEC_VP6: case VCODEC_VP6A: return !(data[codec == VCODEC_VP6A ? 3 : 0] & 0x80);
    case VCODEC_H264: return len > 8 && data[0] == 1 && (data[8] & 31) == 5;   /* first NAL an IDR slice (4-byte lengths) */
    default: return 1;
    }
}

/* BT.601 studio range in 16.16 fixed point, chroma repeated over 2x2 pixels
   (h263-rs-yuv bt601, as Flash Player does).  With an alpha plane the result
   is the premultiplied texture Ruffle makes: colour clamped to alpha. */
static u8 clamp8(long v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }
static long asr16(long v) { return v >= 0 ? v >> 16 : -((-v + 65535) >> 16); }
void vdec_yuv420_to_rgba(const u8 *y, int ystride, const u8 *cb, const u8 *cr, int cstride,
                         const u8 *a, int astride, int w, int h, u32 *out) {
    int i, j;
    for (j = 0; j < h; j++) {
        const u8 *yr = y + (size_t)j * ystride, *br = cb + (size_t)(j >> 1) * cstride, *rr = cr + (size_t)(j >> 1) * cstride;
        u32 *o = out + (size_t)j * w;
        for (i = 0; i < w; i++) {
            long gray = (long)(yr[i] - 16) * 76309, u = br[i >> 1] - 128, v = rr[i >> 1] - 128;
            u32 r = clamp8(asr16(gray + v * 104597 + 32768)), g = clamp8(asr16(gray - v * 53279 - u * 25675 + 32768)), b = clamp8(asr16(gray + u * 132201 + 32768));
            u32 al = a ? a[(size_t)j * astride + i] : 255;
            if (al < 255) { if (r > al) r = al; if (g > al) g = al; if (b > al) b = al; }
            o[i] = (al << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

/* VP6 is decoded by vp6.c (a port of NihAV's decoder) */
