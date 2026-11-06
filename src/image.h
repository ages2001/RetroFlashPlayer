/* image.h - bitmap character decoding (DefineBitsLossless/2 fully;
 * JPEG variants are a seam for a baseline decoder to be dropped in). */
#ifndef FLASHNT_IMAGE_H
#define FLASHNT_IMAGE_H

#include "swf.h"
#include "raster.h"

/* decode (and cache) a bitmap character; NULL if unsupported/failed */
Bitmap *image_get(SWFMovie *m, int id);
void    bitmap_free(Bitmap *b);

#endif
