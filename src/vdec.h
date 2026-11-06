/* vdec.h - video decoders shared by DefineVideoStream and NetStream.
 *
 * Codec ids are the SWF/FLV ones: 2 Sorenson H.263, 3 Screen Video,
 * 4 VP6, 5 VP6 with alpha, 6 Screen Video 2, 7 H.264.  Input is the codec
 * bitstream as stored in a VideoFrame tag (for FLV the caller strips the
 * leading frame-type/codec byte and, for VP6, the size-adjustment byte). */
#ifndef FLASHNT_VDEC_H
#define FLASHNT_VDEC_H

#include "flashnt.h"
#include "raster.h"

#define VCODEC_H263    2
#define VCODEC_SCREEN  3
#define VCODEC_VP6     4
#define VCODEC_VP6A    5
#define VCODEC_SCREEN2 6
#define VCODEC_H264    7

/* DefineVideoStream deblocking field */
#define VDEBLOCK_PACKET 0      /* use the value the frame carries */
#define VDEBLOCK_OFF    1
#define VDEBLOCK_LEVEL1 2

typedef struct VDec VDec;

VDec   *vdec_create(int codec, int deblocking);
/* decode one frame; 1 when a new picture is available */
int     vdec_decode(VDec *d, const u8 *data, size_t len);
/* the last decoded picture (straight alpha 0xAARRGGBB), NULL before any;
   width/height are the coded size, crop to the stream size when drawing */
const Bitmap *vdec_image(VDec *d);
/* 1 when the frame needs no earlier frame (keyframe) */
int     vdec_is_keyframe(int codec, const u8 *data, size_t len);
void    vdec_free(VDec *d);

/* YUV 4:2:0 -> 0xAARRGGBB with the BT.601 studio-range matrix */
void    vdec_yuv420_to_rgba(const u8 *y, int ystride, const u8 *cb, const u8 *cr, int cstride,
                            const u8 *a, int astride, int w, int h, u32 *out);

/* codec back ends */
typedef struct H263Dec H263Dec;
H263Dec *h263_create(void);
int      h263_decode(H263Dec *d, const u8 *data, size_t len, int deblocking, Bitmap **out);
void     h263_free(H263Dec *d);

typedef struct H264Dec H264Dec;
H264Dec *h264_create(void);
/* an FLV AVC packet (AVCPacketType, composition time, data); 1 when a picture comes out */
int      h264_decode(H264Dec *d, const u8 *data, size_t len, Bitmap **out);
void     h264_free(H264Dec *d);

typedef struct VP6Dec VP6Dec;
VP6Dec  *vp6_create(int with_alpha);
int      vp6_decode(VP6Dec *d, const u8 *data, size_t len, Bitmap **out);
void     vp6_free(VP6Dec *d);

#endif
