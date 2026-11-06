/* raster_sse2.c - the SSE2 copies of the renderer's hot loops for the
 * Windows NT build (see FLASHNT_SSE2_DISPATCH in raster.h): raster.c's
 * scanline fill and the blur pass, compiled for SSE2 arithmetic.  The
 * platform layer enables them only when the CPU and the OS support SSE2. */
#if defined(FLASHNT_NT31) && defined(__GNUC__) && defined(__i386__)
#pragma GCC target("sse2,fpmath=sse")
#define FC_SSE 1
#define RASTER_SSE2_COPY 1
#include "raster.c"

#define BLUR_RUN_DECL void blur_run_sse2
#include "blur_run.inc"
#else
typedef int raster_sse2_unused;
#endif
