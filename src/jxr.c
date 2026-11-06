/* jxr.c - JPEG XR (HD Photo) images: the container, then jxrlib's decoder
 * (jxr_sys.c, jxr_dec.c) the way the jxrlib glue library drives it
 * (PKImageDecode_Initialize_WMP / PKImageDecode_Copy_WMP, banded decoding),
 * and the pixel conversions Ruffle gets by going through a TIFF. */
#include "jxr_int.h"
#include "jxr.h"

/* the pixel formats of the {6FDDC324-4E03-4BFE-B185-3D77768DC9xx} family we know */
typedef struct { u8 id; int channels, bits, bd, alpha, bgr, cf; } JxrFmt;
static const JxrFmt g_jxr_fmts[] = {
    { 0x08, 1,  8, BD_8,   0, 0, Y_ONLY },   /* 8bppGray */
    { 0x09, 3, 16, BD_5,   0, 0, CF_RGB },   /* 16bppRGB555 */
    { 0x0a, 3, 16, BD_565, 0, 0, CF_RGB },   /* 16bppRGB565 */
    { 0x0b, 1, 16, BD_16,  0, 0, Y_ONLY },   /* 16bppGray */
    { 0x0c, 3, 24, BD_8,   0, 1, CF_RGB },   /* 24bppBGR */
    { 0x0d, 3, 24, BD_8,   0, 0, CF_RGB },   /* 24bppRGB */
    { 0x0e, 3, 32, BD_8,   0, 1, CF_RGB },   /* 32bppBGR */
    { 0x0f, 4, 32, BD_8,   1, 1, CF_RGB },   /* 32bppBGRA */
    { 0x10, 4, 32, BD_8,   1, 1, CF_RGB },   /* 32bppPBGRA */
    { 0x15, 3, 48, BD_16,  0, 0, CF_RGB },   /* 48bppRGB */
    { 0x16, 4, 64, BD_16,  1, 0, CF_RGB },   /* 64bppRGBA */
    { 0x17, 4, 64, BD_16,  1, 0, CF_RGB },   /* 64bppPRGBA */
    { 0, 0, 0, 0, 0, 0, 0 }
};
static const u8 g_jxr_guid_prefix[15] = { 0x24, 0xc3, 0xdd, 0x6f, 0x03, 0x4e, 0xfe, 0x4b, 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9 };

int jxr_noprint(const char *f, ...) { (void)f; return 0; }
int jxr_nofprint(void *fp, const char *f, ...) { (void)fp; (void)f; return 0; }

static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

typedef struct {
    const JxrFmt *fmt;
    u32 image_off, image_len, alpha_off, alpha_len;
    int has_orientation, orientation;
} JxrContainer;

/* the "II" 0xBC container and its directory (ReadContainer / ParsePFD) */
static int jxr_container(const u8 *d, size_t n, JxrContainer *c) {
    u32 pfd; u16 cnt, i; int fmt_alpha = 0;
    memset(c, 0, sizeof(*c));
    if (n < 8 || d[0] != 'I' || d[1] != 'I' || d[2] != 0xbc || (d[3] != 0 && d[3] != 1)) return 0;
    pfd = rd32(d + 4);
    if ((size_t)pfd + 2 > n) return 0;
    cnt = rd16(d + pfd);
    if (cnt == 0 || cnt == 0xffff || (size_t)pfd + 2 + (size_t)cnt * 12 > n) return 0;
    for (i = 0; i < cnt; i++) {
        const u8 *e = d + pfd + 2 + (size_t)i * 12;
        u16 tag = rd16(e); u32 count = rd32(e + 4), val = rd32(e + 8);
        switch (tag) {
        case 0xbc01:   /* pixel format GUID */
            if ((size_t)val + 16 <= n && !memcmp(d + val, g_jxr_guid_prefix, 15)) {
                const JxrFmt *f;
                for (f = g_jxr_fmts; f->id; f++) if (f->id == d[val + 15]) { c->fmt = f; fmt_alpha = f->alpha; }
            }
            break;
        case 0xbc02: if (count == 1) { c->has_orientation = 1; c->orientation = (int)val; } break;   /* transformation */
        case 0xbcc0: if (count == 1) c->image_off = val; break;
        case 0xbcc1: if (count == 1) c->image_len = val; break;
        case 0xbcc2: if (count == 1) c->alpha_off = val; break;
        case 0xbcc3: if (count == 1) c->alpha_len = val; break;
        default: break;
        }
    }
    /* planar alpha only when the format has alpha and the alpha image is there */
    if (!(fmt_alpha && c->alpha_off && c->alpha_len)) { c->alpha_off = c->alpha_len = 0; }
    return c->fmt != NULL && c->image_off > 0 && c->image_off < n;
}

/* decode into the format's own pixels (PKImageDecode_Copy_WMP in REENTRANT_MODE,
   the whole image in one call); alpha_mode 2 adds the planar alpha */
static int jxr_copy(const u8 *data, size_t len, const JxrContainer *c, int alpha_mode, u8 *pb, u32 stride, int *pw, int *ph) {
    struct WMPStream *ws = NULL; CWMImageInfo ii, ii_a; CWMIStrCodecParam *scp, *scp_a; CTXSTRCODEC ctx = NULL, ctx_a = NULL;
    CWMImageBufferInfo bi; u32 rows, row, linesper = 16; size_t lines; int ok = 0, has_alpha = c->alpha_off != 0;
    u32 cropped = 0; int first_nonzero = 0; u32 cmb;
    scp = (CWMIStrCodecParam*)calloc(1, sizeof(CWMIStrCodecParam)); scp_a = (CWMIStrCodecParam*)calloc(1, sizeof(CWMIStrCodecParam));
    if (!scp || !scp_a) { free(scp); free(scp_a); return 0; }
    memset(&ii, 0, sizeof(ii));
    if (CreateWS_Memory(&ws, (void*)data, len) != WMP_errSuccess) goto done;
    if (ws->SetPos(ws, c->image_off) != WMP_errSuccess) goto done;
    ii.cBitsPerUnit = (size_t)c->fmt->bits; ii.bRGB = !c->fmt->bgr;
    scp->pWStream = ws;
    if (ImageStrDecGetInfo(&ii, scp) != ICERR_OK) goto done;
    ii.oOrientation = c->has_orientation ? (ORIENTATION)c->orientation : O_NONE;
    if (ii.oOrientation >= O_RCW) goto done;   /* rotations need the whole frame (not banded) */
    *pw = (int)ii.cWidth; *ph = (int)ii.cHeight;
    if (!pb) { ok = 1; goto done; }
    scp->uAlphaMode = (U8)(has_alpha ? 0 : alpha_mode);   /* interleaved alpha with the image; planar alpha separately */
    scp->fMeasurePerf = TRUE;
    if (ImageStrDecInit(&ii, scp, &ctx) != ICERR_OK) { ctx = NULL; goto done; }
    memset(&bi, 0, sizeof(bi));
    bi.pv = pb; bi.cLine = (size_t)*ph; bi.cbStride = stride;
    /* MB rows up to the last one (REENTRANT_MODE: one row late) */
    cmb = (u32)((cropped + (u32)*ph + (linesper - 1)) / linesper + 1);
    for (row = 0; row < cmb; row++) {
        bi.uiFirstMBRow = row; bi.uiLastMBRow = row;
        if (ImageStrDecDecode(ctx, &bi, &lines) != ICERR_OK) goto done;
        if (!first_nonzero && lines > 0) {
            cropped += (u32)(linesper - lines); first_nonzero = 1;
            cmb = (u32)((cropped + (u32)*ph + (linesper - 1)) / linesper + 1);
        }
        if (lines == 0 && row > 0) { cropped += linesper; cmb++; }
    }
    rows = cmb;
    if (linesper * (rows - 1) >= cropped + (u32)ii.cROIHeight) { ImageStrDecTerm(ctx); ctx = NULL; }
    if (has_alpha && alpha_mode != 0) {
        ii_a = ii; *scp_a = *scp; scp_a->uAlphaMode = (U8)alpha_mode;
        ii_a.cfColorFormat = Y_ONLY;
        switch (ii.bdBitDepth) {
        case BD_8: ii_a.cLeadingPadding += (ii.cBitsPerUnit >> 3) - 1; break;
        case BD_16: case BD_16S: case BD_16F: ii_a.cLeadingPadding += (ii.cBitsPerUnit >> 3) / sizeof(U16) - 1; break;
        case BD_32: case BD_32S: case BD_32F: ii_a.cLeadingPadding += (ii.cBitsPerUnit >> 3) / sizeof(float) - 1; break;
        default: break;
        }
        scp_a->fMeasurePerf = TRUE;
        if (ws->SetPos(ws, c->alpha_off) != WMP_errSuccess) goto done;
        if (ImageStrDecInit(&ii_a, scp_a, &ctx_a) != ICERR_OK) { ctx_a = NULL; goto done; }
        cmb = (u32)((cropped + (u32)*ph + (linesper - 1)) / linesper + 1);
        for (row = 0; row < cmb; row++) {
            bi.uiFirstMBRow = row; bi.uiLastMBRow = row;
            if (ImageStrDecDecode(ctx_a, &bi, &lines) != ICERR_OK) goto done;
        }
        if (linesper * (cmb - 1) >= cropped + (u32)ii.cROIHeight) { ImageStrDecTerm(ctx_a); ctx_a = NULL; }
    }
    ok = 1;
done:
    if (ctx) ImageStrDecTerm(ctx);
    if (ctx_a) ImageStrDecTerm(ctx_a);
    if (ws) ws->Close(&ws);
    free(scp); free(scp_a);
    return ok;
}

int jxr_info(const u8 *data, size_t len, int *w, int *h, int *fmt) {
    JxrContainer c;
    if (!jxr_container(data, len, &c)) return 0;
    if (fmt) *fmt = c.fmt->id;
    return jxr_copy(data, len, &c, 0, NULL, 0, w, h);
}

/* the format's pixels as stored (Ruffle's copy_all) */
u8 *jxr_decode_raw(const u8 *data, size_t len, int *w, int *h, int *fmt, int *bpp) {
    JxrContainer c; u8 *buf; u32 stride;
    if (!jxr_container(data, len, &c) || !jxr_copy(data, len, &c, 0, NULL, 0, w, h)) return NULL;
    if (*w <= 0 || *h <= 0 || (double)*w * *h > 16777216.0) return NULL;
    stride = (u32)*w * (u32)c.fmt->bits / 8;
    buf = (u8*)calloc((size_t)stride * (size_t)*h + 64, 1);
    if (!buf) return NULL;
    if (!jxr_copy(data, len, &c, 0, buf, stride, w, h)) { free(buf); return NULL; }
    if (fmt) *fmt = c.fmt->id;
    if (bpp) *bpp = c.fmt->bits;
    return buf;
}

/* straight RGBA, 8 bits a channel (Ruffle: the jxrlib TIFF read back by the
   image crate: 16-bit samples keep their high byte, gray and RGB get alpha 255) */
u8 *jxr_decode_rgba(const u8 *data, size_t len, int *w, int *h) {
    JxrContainer c; u8 *buf, *out; u32 stride; size_t i, np;
    if (!jxr_container(data, len, &c) || !jxr_copy(data, len, &c, 0, NULL, 0, w, h)) return NULL;
    if (*w <= 0 || *h <= 0 || (double)*w * *h > 16777216.0) return NULL;
    if (c.fmt->bd == BD_565 || c.fmt->bd == BD_5) return NULL;
    stride = (u32)*w * (u32)c.fmt->bits / 8;
    buf = (u8*)calloc((size_t)stride * (size_t)*h + 64, 1);
    np = (size_t)*w * (size_t)*h;
    out = (u8*)malloc(np * 4 + 4);
    if (!buf || !out) { free(buf); free(out); return NULL; }
    if (!jxr_copy(data, len, &c, 2, buf, stride, w, h)) { free(buf); free(out); return NULL; }
    for (i = 0; i < np; i++) {
        size_t y = i / (size_t)*w, x = i % (size_t)*w; const u8 *p = buf + y * stride; u8 *o = out + i * 4;
        if (c.fmt->bits == 8) { o[0] = o[1] = o[2] = p[x]; o[3] = 255; }
        else if (c.fmt->bits == 16) { o[0] = o[1] = o[2] = p[x * 2 + 1]; o[3] = 255; }
        else if (c.fmt->bd == BD_16) {   /* 48 / 64 bpp: little-endian 16-bit samples */
            int ch = c.fmt->bits / 16; const u8 *q = p + x * (size_t)ch * 2;
            o[0] = q[1]; o[1] = q[3]; o[2] = q[5]; o[3] = ch == 4 ? q[7] : 255;
        } else {
            int ch = c.fmt->bits / 8; const u8 *q = p + x * (size_t)ch;
            if (c.fmt->bgr) { o[0] = q[2]; o[1] = q[1]; o[2] = q[0]; } else { o[0] = q[0]; o[1] = q[1]; o[2] = q[2]; }
            o[3] = c.fmt->alpha ? q[3] : 255;
        }
    }
    free(buf);
    return out;
}
