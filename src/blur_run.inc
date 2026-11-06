/* blur_run.inc - one pass of the blur along a row or column (included by
 * render_filters.c and, for the SSE2 copy, by raster_sse2.c) */
BLUR_RUN_DECL(const u32 *src, u32 *dst, int len, size_t stride, double full) {
    float radius = (float)((full - 1.0) / 2.0), mf = (float)ceil(radius) - 1.0f;
    float alpha = (float)floor((radius - mf) * 255.0f) / 255.0f;
    int m = (int)mf, x, k, c;
    long in[4] = { 0, 0, 0, 0 };   /* the inner box (weight 1), texels x-m .. x+m */
    double al = alpha, lt[4] = { -1, -1, -1, -1 }; u32 lo[4] = { 0, 0, 0, 0 };
#define BTX(K) src[(size_t)((K) < 0 ? 0 : (K) >= len ? len - 1 : (K)) * stride]
    /* A sliding window: every term is an 8-bit value times 1 or alpha (a
       float), so each partial sum is exact in a double and the order of the
       additions does not change the total. */
    for (k = -m; k <= m; k++) { u32 v = BTX(k); in[0] += v >> 24; in[1] += (v >> 16) & 255; in[2] += (v >> 8) & 255; in[3] += v & 255; }
    for (x = 0; x < len; x++) {
        u32 va = BTX(x - m - 1), vb = BTX(x + m + 1), o[4];
        double tot[4];
        tot[0] = (double)in[0] + (al * (double)(va >> 24) + al * (double)(vb >> 24));
        tot[1] = (double)in[1] + (al * (double)((va >> 16) & 255) + al * (double)((vb >> 16) & 255));
        tot[2] = (double)in[2] + (al * (double)((va >> 8) & 255) + al * (double)((vb >> 8) & 255));
        tot[3] = (double)in[3] + (al * (double)(va & 255) + al * (double)(vb & 255));
        for (c = 0; c < 4; c++) {
            if (tot[c] == lt[c]) { o[c] = lo[c]; continue; }   /* runs of equal sums (flat areas) */
            { double r = (float)(tot[c] / 255.0 / full); int f = ifloor(r * 255.0 + 1e-7); o[c] = f <= 0 ? 0 : f >= 255 ? 255 : (u32)f; }
            lt[c] = tot[c]; lo[c] = o[c];
        }
        dst[x] = (o[0] << 24) | (o[1] << 16) | (o[2] << 8) | o[3];
        {   u32 vo = BTX(x - m), vi = BTX(x + m + 1);
            in[0] += (long)(vi >> 24) - (long)(vo >> 24); in[1] += (long)((vi >> 16) & 255) - (long)((vo >> 16) & 255);
            in[2] += (long)((vi >> 8) & 255) - (long)((vo >> 8) & 255); in[3] += (long)(vi & 255) - (long)(vo & 255); }
    }
#undef BTX
}
