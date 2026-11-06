/* jxr.h - JPEG XR images (jxr.c) */
#ifndef FLASHNT_JXR_H
#define FLASHNT_JXR_H

#include "flashnt.h"

/* size and pixel format id (the last byte of the format GUID); 0 if not JPEG XR */
int jxr_info(const u8 *data, size_t len, int *w, int *h, int *fmt);
/* straight RGBA (malloc'd, w * h * 4), NULL on failure */
u8 *jxr_decode_rgba(const u8 *data, size_t len, int *w, int *h);
/* the format's own pixels, w * bpp / 8 bytes a row (malloc'd) */
u8 *jxr_decode_raw(const u8 *data, size_t len, int *w, int *h, int *fmt, int *bpp);

#endif
