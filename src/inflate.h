/* inflate.h - self-contained DEFLATE / zlib decompressor (public-domain
 * puff-style implementation).  No external zlib: NT 3.1 has none. */
#ifndef FLASHNT_INFLATE_H
#define FLASHNT_INFLATE_H

#include "flashnt.h"

/* Raw DEFLATE.  Returns number of output bytes, or negative on error.
 * out must be pre-sized (SWF gives us the uncompressed size up front). */
long swf_inflate(const u8 *src, size_t srclen, u8 *out, size_t outcap);

/* zlib-wrapped DEFLATE (2-byte header + adler32 trailer). */
long swf_inflate_zlib(const u8 *src, size_t srclen, u8 *out, size_t outcap);

/* LZMA1 (ZWS movies): props = 5-byte header (lc/lp/pb + dict size) */
long swf_lzma_decode(const u8 *props, const u8 *src, size_t srclen, u8 *out, size_t outcap);

#endif
