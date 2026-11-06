/* devfont.c - built-in device fonts (see tools/mkfonts.py, builtin_fonts.c).
 *
 * Flash draws text in a non-embedded font ("device text") with the system's
 * fonts.  FLASHNT ships sans/serif/mono outlines with Arial/Times New
 * Roman/Courier New metrics so device text renders and measures the same on
 * NT 3.1, Windows 95 and later.  The compressed blob is unpacked on first
 * use; glyph outlines become ShapeDefs lazily. */
#include "devfont.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

extern const unsigned long builtin_fonts_raw_size, builtin_fonts_z_size;
extern const u8 builtin_fonts_z[];
long swf_inflate_zlib(const u8 *src, size_t srclen, u8 *out, size_t outcap);

static u8 *g_blob; static int g_tried;
/* the host can substitute another font set (the test harness uses Ruffle's default font) */
const u8 *g_devfont_alt_z; unsigned long g_devfont_alt_z_size, g_devfont_alt_raw_size; int g_devfont_alt_em;
static DevFace g_faces[16]; static int g_nfaces;
static void load_env_fonts(void);

static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

static int load(void) {
    int i; const u8 *h;
    if (g_blob) return 1;
    if (g_tried) return 0;
    g_tried = 1;
    {
        const u8 *z = g_devfont_alt_z ? g_devfont_alt_z : builtin_fonts_z;
        unsigned long zn = g_devfont_alt_z ? g_devfont_alt_z_size : builtin_fonts_z_size, raw = g_devfont_alt_z ? g_devfont_alt_raw_size : builtin_fonts_raw_size;
        g_blob = (u8*)malloc(raw);
        if (!g_blob) return 0;
        if (swf_inflate_zlib(z, zn, g_blob, raw) != (long)raw) { free(g_blob); g_blob = NULL; return 0; }
    }
    g_nfaces = rd16(g_blob); if (g_nfaces > 8) g_nfaces = 8;
    h = g_blob + 2;
    for (i = 0; i < g_nfaces; i++, h += 20) {
        DevFace *f = &g_faces[i];
        f->family = h[0]; f->style = h[1]; f->em = g_devfont_alt_z && g_devfont_alt_em > 0 ? g_devfont_alt_em : 1024;
        f->ascent = (s16)rd16(h + 2); f->descent = (s16)rd16(h + 4); f->leading = (s16)rd16(h + 6);
        f->nglyphs = rd16(h + 8); f->table = g_blob + rd32(h + 10); f->kern = g_blob + rd32(h + 14); f->nkern = rd16(h + 18); f->base = g_blob;
        f->shapes = (ShapeDef**)calloc((size_t)f->nglyphs + 1, sizeof(ShapeDef*));
        f->base = g_blob;
    }
    load_env_fonts();
    return 1;
}

/* ---- TrueType files as device fonts (the test harness installs a test.toml's [fonts]) ---- */
static u16 be16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
typedef struct { u8 *b; size_t n, cap; } Buf;
static void bput(Buf *b, const void *p, size_t n) { if (b->n + n > b->cap) { size_t c = b->cap ? b->cap * 2 : 4096; u8 *nb; while (c < b->n + n) c *= 2; nb = (u8*)realloc(b->b, c); if (!nb) return; b->b = nb; b->cap = c; } memcpy(b->b + b->n, p, n); b->n += n; }
static void bvar(Buf *b, u32 v) { u8 t[5]; int k = 0; while (v >= 0x80) { t[k++] = (u8)(v | 0x80); v >>= 7; } t[k++] = (u8)v; bput(b, t, (size_t)k); }
static u32 zz(int v) { return v < 0 ? (u32)(-v) * 2 - 1 : (u32)v * 2; }
static const u8 *ttf_table(const u8 *d, size_t n, const char *tag, u32 *len) {
    int i, nt; if (n < 12) return NULL; nt = be16(d + 4);
    for (i = 0; i < nt && 12 + 16 * (size_t)i + 16 <= n; i++) { const u8 *e = d + 12 + 16 * i; if (!memcmp(e, tag, 4)) { u32 o = be32(e + 8), l = be32(e + 12); if ((size_t)o + l > n) return NULL; *len = l; return d + o; } }
    return NULL;
}
/* glyph gi's points (font units) appended as contours; composites place their parts */
static int ttf_points(const u8 *glyf, u32 glen, const u8 *loca, int locfmt, int nglyphs, int gi, int dx, int dy, int depth,
                      int **px, int **py, int **on, int *np, int **ends, int *nc) {
    u32 o0, o1; const u8 *g; int ncont;
    if (gi < 0 || gi >= nglyphs || depth > 8) return 0;
    o0 = locfmt ? be32(loca + gi * 4) : (u32)be16(loca + gi * 2) * 2; o1 = locfmt ? be32(loca + gi * 4 + 4) : (u32)be16(loca + gi * 2 + 2) * 2;
    if (o1 <= o0 || o1 > glen) return 0;
    g = glyf + o0; ncont = (s16)be16(g);
    if (ncont >= 0) {
        const u8 *p = g + 10 + ncont * 2, *fl; int npts = ncont ? be16(g + 10 + (ncont - 1) * 2) + 1 : 0, i, x = 0, y = 0, base = *np; u8 *flags;
        p += 2 + be16(p);   /* instructions */
        flags = (u8*)malloc((size_t)npts + 1); if (!flags) return 0;
        for (i = 0; i < npts; ) { u8 f = *p++; int r = 1; if (f & 8) r += *p++; while (r-- && i < npts) flags[i++] = f; }
        *px = (int*)realloc(*px, (size_t)(*np + npts) * sizeof(int)); *py = (int*)realloc(*py, (size_t)(*np + npts) * sizeof(int)); *on = (int*)realloc(*on, (size_t)(*np + npts) * sizeof(int));
        *ends = (int*)realloc(*ends, (size_t)(*nc + ncont) * sizeof(int));
        if (!*px || !*py || !*on || (ncont && !*ends)) { free(flags); return 0; }
        for (i = 0; i < npts; i++) { u8 f = flags[i]; if (f & 2) { int v = *p++; x += (f & 16) ? v : -v; } else if (!(f & 16)) { x += (s16)be16(p); p += 2; } (*px)[base + i] = x + dx; (*on)[base + i] = f & 1; }
        for (i = 0; i < npts; i++) { u8 f = flags[i]; if (f & 4) { int v = *p++; y += (f & 32) ? v : -v; } else if (!(f & 32)) { y += (s16)be16(p); p += 2; } (*py)[base + i] = y + dy; }
        for (i = 0; i < ncont; i++) (*ends)[*nc + i] = base + be16(g + 10 + i * 2);
        *np += npts; *nc += ncont;
        free(flags); (void)fl;
        return 1;
    } else {   /* composite */
        const u8 *p = g + 10; u16 f;
        do {
            int sub, ax, ay;
            f = be16(p); sub = be16(p + 2); p += 4;
            if (f & 1) { ax = (s16)be16(p); ay = (s16)be16(p + 2); p += 4; } else { ax = (signed char)p[0]; ay = (signed char)p[1]; p += 2; }
            if (f & 8) p += 2; else if (f & 0x40) p += 4; else if (f & 0x80) p += 8;
            if (!(f & 2)) ax = ay = 0;   /* point matching: not supported */
            ttf_points(glyf, glen, loca, locfmt, nglyphs, sub, dx + ax, dy + ay, depth + 1, px, py, on, np, ends, nc);
        } while (f & 0x20);
        return 1;
    }
}
int devfont_add_ttf(const char *family, const u8 *d, size_t n, int bold) {
    u32 l, hl, ml, cl, gl, ll, ol, hhl; const u8 *head = ttf_table(d, n, "head", &hl), *maxp = ttf_table(d, n, "maxp", &ml), *cmap = ttf_table(d, n, "cmap", &cl), *hmtx = ttf_table(d, n, "hmtx", &l);
    const u8 *glyf = ttf_table(d, n, "glyf", &gl), *loca = ttf_table(d, n, "loca", &ll), *os2 = ttf_table(d, n, "OS/2", &ol), *hhea = ttf_table(d, n, "hhea", &hhl), *sub = NULL;
    int upem, locfmt, ng, nhm, i, nmap = 0; double k; u16 *codes; int *gids; Buf tb, ob; DevFace *f;
    if (!head || !maxp || !cmap || !hmtx || !glyf || !loca || !hhea || g_nfaces >= 16) return -1;
    upem = be16(head + 18); locfmt = (s16)be16(head + 50); ng = be16(maxp + 4); nhm = be16(hhea + 34);
    if (upem <= 0 || ng <= 0) return -1;
    k = 1024.0 / upem;
    for (i = 0; i < be16(cmap + 2); i++) { const u8 *e = cmap + 4 + i * 8; u16 pl = be16(e), en = be16(e + 2); const u8 *s = cmap + be32(e + 4); if (be16(s) == 4 && ((pl == 3 && (en == 1 || en == 0)) || pl == 0)) { sub = s; if (pl == 3 && en == 1) break; } }
    if (!sub) return -1;
    codes = (u16*)malloc(65536 * sizeof(u16)); gids = (int*)malloc(65536 * sizeof(int));
    if (!codes || !gids) { free(codes); free(gids); return -1; }
    {   int segx2 = be16(sub + 6), s, nseg = segx2 / 2; const u8 *ends = sub + 14, *starts = ends + segx2 + 2, *deltas = starts + segx2, *ros = deltas + segx2;
        for (s = 0; s < nseg; s++) {
            u32 c, e = be16(ends + s * 2), st = be16(starts + s * 2); u16 dl = be16(deltas + s * 2), ro = be16(ros + s * 2);
            for (c = st; c <= e && c < 0xFFFF; c++) {
                int gi;
                if (!ro) gi = (int)((c + dl) & 0xFFFF);
                else { const u8 *gp = ros + s * 2 + ro + (c - st) * 2; gi = be16(gp); if (gi) gi = (gi + dl) & 0xFFFF; }
                if (gi > 0 && gi < ng && nmap < 65536) { codes[nmap] = (u16)c; gids[nmap] = gi; nmap++; }
            }
        }
    }
    memset(&tb, 0, sizeof(tb)); memset(&ob, 0, sizeof(ob));
    bput(&tb, "", 0);
    {   u8 *table = (u8*)calloc((size_t)nmap + 1, 8);
        if (!table) { free(codes); free(gids); return -1; }
        for (i = 0; i < nmap; i++) {
            int gi = gids[i], adv = be16(hmtx + 4 * (gi < nhm ? gi : nhm - 1)), *px = NULL, *py = NULL, *on = NULL, *ce = NULL, np = 0, nc = 0, c, x = 0, y = 0, a = (int)(adv * k + 0.5);
            u32 off = (u32)ob.n;
            ttf_points(glyf, gl, loca, locfmt, ng, gi, 0, 0, 0, &px, &py, &on, &np, &ce, &nc);
            bvar(&ob, (u32)nc);
            for (c = 0; c < nc; c++) {
                int st = c ? ce[c - 1] + 1 : 0, en = ce[c], j;
                bvar(&ob, (u32)(en - st + 1));
                for (j = st; j <= en; j++) { int qx = (int)(px[j] * k + (px[j] < 0 ? -0.5 : 0.5)), qy = (int)(py[j] * k + (py[j] < 0 ? -0.5 : 0.5)); bvar(&ob, zz(qx - x) * 2 | (u32)(on[j] & 1)); bvar(&ob, zz(qy - y)); x = qx; y = qy; }
            }
            free(px); free(py); free(on); free(ce);
            table[i * 8] = (u8)codes[i]; table[i * 8 + 1] = (u8)(codes[i] >> 8); table[i * 8 + 2] = (u8)a; table[i * 8 + 3] = (u8)(a >> 8);
            table[i * 8 + 4] = (u8)off; table[i * 8 + 5] = (u8)(off >> 8); table[i * 8 + 6] = (u8)(off >> 16); table[i * 8 + 7] = (u8)(off >> 24);
        }
        f = &g_faces[g_nfaces];
        memset(f, 0, sizeof(*f));
        f->family = 100 + g_nfaces; f->style = bold ? 1 : 0; f->em = 1024;
        f->ascent = os2 && ol >= 78 ? (int)(be16(os2 + 74) * k + 0.5) : (int)((s16)be16(hhea + 4) * k + 0.5);
        f->descent = os2 && ol >= 78 ? (int)(be16(os2 + 76) * k + 0.5) : (int)(-(s16)be16(hhea + 6) * k + 0.5);
        f->leading = 0;
        f->nglyphs = nmap; f->table = table; f->kern = table; f->nkern = 0;
        f->base = ob.b; f->shapes = (ShapeDef**)calloc((size_t)nmap + 1, sizeof(ShapeDef*));
        strncpy(f->name, family, 63);
        g_nfaces++;
    }
    free(codes); free(gids);
    return 100 + g_nfaces - 1;
}
/* FLASHNT_DEVICE_FONTS="Family=path[;Family=path...]" (a "+bold" suffix on the family marks a bold face;
 * the family "*" stands for every name the system has no font for) */
static void load_env_fonts(void) {
    const char *e = getenv("FLASHNT_DEVICE_FONTS"); char buf[2048], *p, *q;
    if (!e) return;
    strncpy(buf, e, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    for (p = buf; p && *p; p = q) {
        char *eq, *nm; FILE *fp; long len; u8 *d; int bold = 0;
        q = strchr(p, ';'); if (q) *q++ = 0;
        eq = strchr(p, '='); if (!eq) continue; *eq = 0; nm = p;
        { char *pl = strstr(nm, "+bold"); if (pl) { *pl = 0; bold = 1; } }
        fp = fopen(eq + 1, "rb"); if (!fp) continue;
        fseek(fp, 0, SEEK_END); len = ftell(fp); fseek(fp, 0, SEEK_SET);
        d = (u8*)malloc((size_t)len + 1);
        if (d && fread(d, 1, (size_t)len, fp) == (size_t)len) devfont_add_ttf(nm, d, (size_t)len, bold);
        free(d); fclose(fp);
    }
}

int devfont_hdmx_advance(const DevFace *f, int gi, int ppem) {
    int r;
    if (!f || !f->hdmx_w || gi < 0 || gi >= f->nglyphs) return -1;
    for (r = 0; r < f->nhdmx; r++) if (f->hdmx_ppem[r] == ppem) { int w = f->hdmx_w[(size_t)r * f->nglyphs + gi]; return w ? w : -1; }
    return -1;
}
int devfont_linux_rules(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("FLASHNT_DEVICE_LINUX"); v = e && *e == '1'; }
    return v;
}

/* Flash device font names -> family.  Unknown names get Times New Roman's
   look (the Windows player's default), as do empty names. */
int devfont_family_for_name(const char *name) {
    char b[64]; int i;
    if (!name) return DEVFONT_SERIF;
    while (*name == ' ') name++;
    for (i = 0; i < 63 && name[i] && name[i] != ','; i++) { char c = name[i]; b[i] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c); }
    while (i > 0 && b[i - 1] == ' ') i--;
    b[i] = 0;
    if (load()) { int k; for (k = 0; k < g_nfaces; k++) if (g_faces[k].name[0]) { char nb[64]; int j; for (j = 0; g_faces[k].name[j] && j < 63; j++) { char c = g_faces[k].name[j]; nb[j] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c); } nb[j] = 0; if (!strcmp(nb, b)) return g_faces[k].family; } }
    if (!strcmp(b, "_sans") || !strcmp(b, "arial") || !strcmp(b, "helvetica") || !strcmp(b, "verdana") || !strcmp(b, "tahoma") ||
        !strcmp(b, "trebuchet ms") || !strcmp(b, "segoe ui") || !strcmp(b, "ms sans serif") || !strcmp(b, "microsoft sans serif") ||
        !strcmp(b, "lucida sans unicode") || !strcmp(b, "lucida grande") || !strcmp(b, "calibri") || !strcmp(b, "century gothic") ||
        !strcmp(b, "franklin gothic medium") || !strcmp(b, "impact") || !strcmp(b, "arial black") || !strcmp(b, "comic sans ms") ||
        !strcmp(b, "sans") || !strcmp(b, "sans-serif") || !strcmp(b, "dejavu sans") || !strcmp(b, "liberation sans") || !strcmp(b, "noto sans"))
        return DEVFONT_SANS;
    if (!strcmp(b, "_typewriter") || !strcmp(b, "courier new") || !strcmp(b, "courier") || !strcmp(b, "consolas") || !strcmp(b, "lucida console") ||
        !strcmp(b, "monospace") || !strcmp(b, "fixedsys") || !strcmp(b, "terminal") || !strcmp(b, "nsimsun") || !strcmp(b, "liberation mono"))
        return DEVFONT_MONO;
    /* FLASHNT_DEVICE_FONTS="*=file": the face for names the system does not have */
    if (load()) { int k; for (k = 0; k < g_nfaces; k++) if (!strcmp(g_faces[k].name, "*")) return g_faces[k].family; }
    return DEVFONT_SERIF;
}

DevFace *devfont_get(int family, int bold) {
    int i; DevFace *any = NULL;
    if (!load()) return NULL;
    for (i = 0; i < g_nfaces; i++) if (g_faces[i].family == family) { if ((g_faces[i].style & 1) == (bold ? 1 : 0)) return &g_faces[i]; if (!any) any = &g_faces[i]; }
    return any ? any : (g_nfaces ? &g_faces[0] : NULL);
}

int devfont_glyph(const DevFace *f, int code) {
    int lo = 0, hi = f->nglyphs - 1;
    while (lo <= hi) { int mid = (lo + hi) / 2, c = rd16(f->table + mid * 8); if (c == code) return mid; if (c < code) lo = mid + 1; else hi = mid - 1; }
    return -1;
}
int devfont_code(const DevFace *f, int gi) { return gi >= 0 && gi < f->nglyphs ? rd16(f->table + gi * 8) : 0; }
int devfont_advance(const DevFace *f, int gi) { return gi >= 0 && gi < f->nglyphs ? rd16(f->table + gi * 8 + 2) : 0; }
int devfont_kerning(const DevFace *f, int left, int right) {
    int lo = 0, hi = f->nkern - 1; u32 key = ((u32)left << 16) | (u32)right;
    while (lo <= hi) { int mid = (lo + hi) / 2; const u8 *e = f->kern + mid * 6; u32 k = ((u32)rd16(e) << 16) | rd16(e + 2); if (k == key) return (s16)rd16(e + 4); if (k < key) lo = mid + 1; else hi = mid - 1; }
    return 0;
}

static u32 varint(const u8 **p) { u32 v = 0; int sh = 0; while (**p & 0x80) { v |= (u32)(**p & 0x7F) << sh; sh += 7; (*p)++; } v |= (u32)**p << sh; (*p)++; return v; }
static int unzz(u32 v) { return (v & 1) ? -(int)((v + 1) >> 1) : (int)(v >> 1); }

/* outline points to glyph space: y flipped, truncated to whole units (Ruffle's GlyphToDrawing) */
static void add_edge(ShapeDef *s, float x0, float y0, float cx, float cy, float x1, float y1, int curve) {
    ShapeEdge *e;
    if (s->nedges == s->capedges) { s->capedges = s->capedges ? s->capedges * 2 : 64; s->edges = (ShapeEdge*)realloc(s->edges, (size_t)s->capedges * sizeof(ShapeEdge)); }
    e = &s->edges[s->nedges++]; memset(e, 0, sizeof(*e));
    e->x0 = (s32)x0; e->y0 = (s32)(-y0); e->x1 = (s32)x1; e->y1 = (s32)(-y1);
    if (curve) { e->cx = (s32)cx; e->cy = (s32)(-cy); e->is_curve = 1; }
    e->fill0 = 1;
}

/* ttf-parser's contour walk (glyf.rs push_point / finish_contour): the
   contour starts at its first on-curve point, or between two leading
   off-curve points; implied on-curve points are f32 midpoints */
typedef struct { ShapeDef *s; float cx, cy; int has_first_on, has_first_off, has_last_off; float fon_x, fon_y, foff_x, foff_y, loff_x, loff_y; } TtfWalk;
static void tw_line(TtfWalk *w, float x, float y) { add_edge(w->s, w->cx, w->cy, 0, 0, x, y, 0); w->cx = x; w->cy = y; }
static void tw_quad(TtfWalk *w, float qx, float qy, float x, float y) { add_edge(w->s, w->cx, w->cy, qx, qy, x, y, 1); w->cx = x; w->cy = y; }
static void tw_mid(float ax, float ay, float bx, float by, float *mx, float *my) { *mx = ax + 0.5f * (bx - ax); *my = ay + 0.5f * (by - ay); }
static void tw_point(TtfWalk *w, float x, float y, int on) {
    if (!w->has_first_on) {
        if (on) { w->has_first_on = 1; w->fon_x = x; w->fon_y = y; w->cx = x; w->cy = y; }
        else if (w->has_first_off) { float mx, my; tw_mid(w->foff_x, w->foff_y, x, y, &mx, &my); w->has_first_on = 1; w->fon_x = mx; w->fon_y = my; w->has_last_off = 1; w->loff_x = x; w->loff_y = y; w->cx = mx; w->cy = my; }
        else { w->has_first_off = 1; w->foff_x = x; w->foff_y = y; }
    } else if (w->has_last_off && on) { w->has_last_off = 0; tw_quad(w, w->loff_x, w->loff_y, x, y); }
    else if (w->has_last_off) { float mx, my, ox = w->loff_x, oy = w->loff_y; w->loff_x = x; w->loff_y = y; tw_mid(ox, oy, x, y, &mx, &my); tw_quad(w, ox, oy, mx, my); }
    else if (on) tw_line(w, x, y);
    else { w->has_last_off = 1; w->loff_x = x; w->loff_y = y; }
}
static void tw_finish(TtfWalk *w) {
    if (w->has_first_off && w->has_last_off) { float mx, my; w->has_last_off = 0; tw_mid(w->loff_x, w->loff_y, w->foff_x, w->foff_y, &mx, &my); tw_quad(w, w->loff_x, w->loff_y, mx, my); }
    if (w->has_first_on && w->has_first_off) tw_quad(w, w->foff_x, w->foff_y, w->fon_x, w->fon_y);
    else if (w->has_first_on && w->has_last_off) tw_quad(w, w->loff_x, w->loff_y, w->fon_x, w->fon_y);
    else if (w->has_first_on && (w->cx != w->fon_x || w->cy != w->fon_y)) tw_line(w, w->fon_x, w->fon_y);
    w->has_first_on = w->has_first_off = w->has_last_off = 0;
}

static ShapeDef *ttf_glyph_shape(DevFace *f, int gi);
static void shape_bounds_of_edges(ShapeDef *s);
ShapeDef *devfont_shape(DevFace *f, int gi) {
    const u8 *p; int nc, c, x = 0, y = 0; ShapeDef *s;
    if (gi < 0 || gi >= f->nglyphs) return NULL;
    if (f->shapes[gi]) return f->shapes[gi];
    if (f->ttf) return f->shapes[gi] = ttf_glyph_shape(f, gi);
    s = (ShapeDef*)calloc(1, sizeof(ShapeDef));
    if (!s) return NULL;
    s->version = 1;
    s->fills = (FillStyle*)calloc(1, sizeof(FillStyle));
    if (s->fills) { s->nfills = 1; s->capfills = 1; s->fills[0].type = 0; s->fills[0].color.a = 255; swf_matrix_identity(&s->fills[0].matrix); }
    p = (f->base ? f->base : g_blob) + rd32(f->table + gi * 8 + 4);
    nc = (int)varint(&p);
    for (c = 0; c < nc; c++) {
        int np = (int)varint(&p), i, k; int *px, *py, *on;
        px = (int*)malloc((size_t)np * 3 * sizeof(int)); if (!px) break;
        py = px + np; on = py + np;
        for (i = 0; i < np; i++) { u32 a = varint(&p), b = varint(&p); on[i] = (int)(a & 1); x += unzz(a >> 1); y += unzz(b); px[i] = x; py[i] = y; }
        { TtfWalk w; memset(&w, 0, sizeof(w)); w.s = s;
          for (i = 0; i < np; i++) tw_point(&w, (float)px[i], (float)py[i], on[i]);
          tw_finish(&w); (void)k; }
        free(px);
    }
    shape_bounds_of_edges(s);
    f->shapes[gi] = s;
    return s;
}

/* ---------------------------------------------------------------- */
/* TrueType files -> faces (same layout as the built-in blob)       */

#include <stdio.h>
static u16 tf_be16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static u32 tf_be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

typedef struct { u8 *d; size_t n, cap; } DBuf;
static void db_put(DBuf *b, const void *p, size_t n) {
    if (b->n + n > b->cap) { size_t nc = (b->cap + n) * 2 + 256; u8 *nd = (u8*)realloc(b->d, nc); if (!nd) return; b->d = nd; b->cap = nc; }
    memcpy(b->d + b->n, p, n); b->n += n;
}
static void db_varint(DBuf *b, u32 v) { u8 c; while (v >= 0x80) { c = (u8)((v & 0x7F) | 0x80); db_put(b, &c, 1); v >>= 7; } c = (u8)v; db_put(b, &c, 1); }
static u32 tf_zz(int v) { return v >= 0 ? (u32)v << 1 : (((u32)(-v)) << 1) - 1; }

typedef struct {
    u8 *d; size_t n;
    const u8 *cmap, *hmtx, *loca, *glyf, *kern; u32 glyf_len, loca_len, kern_len, cmap_len;
    int upem, nglyphs, nhm, loc32, asc, desc;
} TTF;

static const u8 *tf_ttf_table(TTF *t, const char *tag, u32 *len) {
    int n, i;
    if (t->n < 12) return NULL;
    n = tf_be16(t->d + 4);
    for (i = 0; i < n && 12 + i * 16 + 16 <= (int)t->n; i++) {
        const u8 *e = t->d + 12 + i * 16; u32 off = tf_be32(e + 8), l = tf_be32(e + 12);
        if (!memcmp(e, tag, 4) && off + l <= t->n && off + l >= off) { if (len) *len = l; return t->d + off; }
    }
    return NULL;
}
static int tf_ttf_open(TTF *t, const char *path) {
    FILE *f = fopen(path, "rb"); long n; const u8 *head, *hhea, *maxp, *os2; u32 l;
    memset(t, 0, sizeof(*t));
    if (!f) return 0;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 12) { fclose(f); return 0; }
    t->d = (u8*)malloc((size_t)n);
    if (!t->d || fread(t->d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(t->d); t->d = NULL; return 0; }
    fclose(f); t->n = (size_t)n;
    head = tf_ttf_table(t, "head", &l); if (!head || l < 54) return 0;
    t->upem = tf_be16(head + 18); t->loc32 = (s16)tf_be16(head + 50) != 0;
    if (t->upem <= 0) return 0;
    hhea = tf_ttf_table(t, "hhea", &l); if (!hhea || l < 36) return 0;
    t->nhm = tf_be16(hhea + 34);
    maxp = tf_ttf_table(t, "maxp", &l); if (!maxp || l < 6) return 0;
    t->nglyphs = tf_be16(maxp + 4);
    /* Flash on Windows lays device text out with the usWin metrics */
    os2 = tf_ttf_table(t, "OS/2", &l);
    if (os2 && l >= 78) { t->asc = tf_be16(os2 + 74); t->desc = tf_be16(os2 + 76); }
    else { t->asc = (s16)tf_be16(hhea + 4); t->desc = -(s16)tf_be16(hhea + 6); }
    t->cmap = tf_ttf_table(t, "cmap", &t->cmap_len); t->hmtx = tf_ttf_table(t, "hmtx", &l);
    t->loca = tf_ttf_table(t, "loca", &t->loca_len); t->glyf = tf_ttf_table(t, "glyf", &t->glyf_len);
    t->kern = tf_ttf_table(t, "kern", &t->kern_len);
    return t->cmap && t->hmtx && t->loca && t->glyf;
}
/* the glyph of a code (cmap 3,10 / 0,x format 12, or 3,1 / 0,x format 4) */
static int tf_ttf_glyph_of(TTF *t, u32 code) {
    int n = tf_be16(t->cmap + 2), i, pass;
    for (pass = 0; pass < 2; pass++)
    for (i = 0; i < n; i++) {
        const u8 *e = t->cmap + 4 + i * 8; u32 off = tf_be32(e + 4); const u8 *s; int fmt;
        if (off + 8 > t->cmap_len) continue;
        s = t->cmap + off; fmt = tf_be16(s);
        if (pass == 0 && fmt == 12) {
            u32 ng = tf_be32(s + 12), k;
            for (k = 0; k < ng && off + 16 + (k + 1) * 12 <= t->cmap_len; k++) {
                const u8 *g = s + 16 + k * 12;
                if (code >= tf_be32(g) && code <= tf_be32(g + 4)) return (int)(tf_be32(g + 8) + code - tf_be32(g));
            }
        } else if (pass == 1 && fmt == 4 && code < 0x10000) {
            int segx2 = tf_be16(s + 6), k; const u8 *ends = s + 14, *starts = ends + segx2 + 2, *deltas = starts + segx2, *ranges = deltas + segx2;
            for (k = 0; k < segx2 / 2; k++) {
                u32 end = tf_be16(ends + k * 2), st = tf_be16(starts + k * 2);
                if (code > end) continue;
                if (code < st) break;
                if (tf_be16(ranges + k * 2) == 0) return (int)((code + tf_be16(deltas + k * 2)) & 0xFFFF);
                { const u8 *gp = ranges + k * 2 + tf_be16(ranges + k * 2) + (code - st) * 2; u32 g;
                  if (gp + 2 > t->cmap + t->cmap_len) return 0;
                  g = tf_be16(gp); return g ? (int)((g + tf_be16(deltas + k * 2)) & 0xFFFF) : 0; }
            }
        }
    }
    return 0;
}
static int tf_ttf_advance(TTF *t, int g) { int i = g < t->nhm ? g : t->nhm - 1; return i >= 0 ? tf_be16(t->hmtx + i * 4) : 0; }
static int tf_ttf_glyph_range(TTF *t, int g, u32 *off, u32 *len) {
    u32 a, b;
    if (g < 0 || g >= t->nglyphs) return 0;
    if (t->loc32) { if ((u32)(g + 2) * 4 > t->loca_len) return 0; a = tf_be32(t->loca + g * 4); b = tf_be32(t->loca + g * 4 + 4); }
    else { if ((u32)(g + 2) * 2 > t->loca_len) return 0; a = tf_be16(t->loca + g * 2) * 2u; b = tf_be16(t->loca + g * 2 + 2) * 2u; }
    if (b < a || b > t->glyf_len) return 0;
    *off = a; *len = b - a; return 1;
}
/* contour points (font units) of a glyph, composites flattened */
typedef struct { int *x, *y; u8 *on; int n, cap; int *ends; int ne, ecap; } Pts;
static void tf_pts_add(Pts *p, int x, int y, int on) {
    if (p->n == p->cap) { p->cap = p->cap * 2 + 64; p->x = (int*)realloc(p->x, (size_t)p->cap * sizeof(int)); p->y = (int*)realloc(p->y, (size_t)p->cap * sizeof(int)); p->on = (u8*)realloc(p->on, (size_t)p->cap); }
    if (!p->x || !p->y || !p->on) return;
    p->x[p->n] = x; p->y[p->n] = y; p->on[p->n] = (u8)on; p->n++;
}
static void tf_pts_end(Pts *p) { if (p->ne == p->ecap) { p->ecap = p->ecap * 2 + 16; p->ends = (int*)realloc(p->ends, (size_t)p->ecap * sizeof(int)); } if (p->ends) p->ends[p->ne++] = p->n; }
static void tf_ttf_points(TTF *t, int g, Pts *p, double m[6], int depth) {
    u32 off, len; const u8 *s, *e; int nc;
    if (depth > 8 || !tf_ttf_glyph_range(t, g, &off, &len) || len < 10) return;
    s = t->glyf + off; e = s + len; nc = (s16)tf_be16(s);
    if (nc >= 0) {
        const u8 *ends = s + 10, *q; int np, i, k, x = 0, y = 0; u8 *fl;
        if (ends + nc * 2 + 2 > e || nc == 0) return;
        np = tf_be16(ends + (nc - 1) * 2) + 1;
        q = ends + nc * 2; q += 2 + tf_be16(q);
        fl = (u8*)malloc((size_t)np + 1); if (!fl) return;
        for (i = 0; i < np && q < e; ) { u8 f = *q++; fl[i++] = f; if (f & 8) { int r = q < e ? *q++ : 0; while (r-- > 0 && i < np) fl[i++] = f; } }
        { int *xs = (int*)malloc((size_t)np * 2 * sizeof(int)), *ys;
          if (!xs) { free(fl); return; } ys = xs + np;
          for (i = 0; i < np; i++) { u8 f = fl[i]; if (f & 2) { int d = q < e ? *q++ : 0; x += (f & 16) ? d : -d; } else if (!(f & 16)) { if (q + 2 > e) break; x += (s16)tf_be16(q); q += 2; } xs[i] = x; }
          for (i = 0; i < np; i++) { u8 f = fl[i]; if (f & 4) { int d = q < e ? *q++ : 0; y += (f & 32) ? d : -d; } else if (!(f & 32)) { if (q + 2 > e) break; y += (s16)tf_be16(q); q += 2; } ys[i] = y; }
          for (k = 0, i = 0; k < nc; k++) {
              int last = tf_be16(ends + k * 2);
              for (; i <= last && i < np; i++) { double fx = m[0] * xs[i] + m[2] * ys[i] + m[4], fy = m[1] * xs[i] + m[3] * ys[i] + m[5]; tf_pts_add(p, (int)floor(fx + 0.5), (int)floor(fy + 0.5), fl[i] & 1); }
              tf_pts_end(p);
          }
          free(xs); }
        free(fl);
    } else {
        const u8 *q = s + 10; int more = 1;
        while (more && q + 4 <= e) {
            int fl = tf_be16(q), gi = tf_be16(q + 2); double dx, dy, a = 1, b = 0, c = 0, d = 1, cm[6];
            q += 4;
            if (fl & 1) { if (q + 4 > e) break; dx = (s16)tf_be16(q); dy = (s16)tf_be16(q + 2); q += 4; } else { if (q + 2 > e) break; dx = (signed char)q[0]; dy = (signed char)q[1]; q += 2; }
            if (fl & 8) { a = d = (s16)tf_be16(q) / 16384.0; q += 2; }
            else if (fl & 0x40) { a = (s16)tf_be16(q) / 16384.0; d = (s16)tf_be16(q + 2) / 16384.0; q += 4; }
            else if (fl & 0x80) { a = (s16)tf_be16(q) / 16384.0; b = (s16)tf_be16(q + 2) / 16384.0; c = (s16)tf_be16(q + 4) / 16384.0; d = (s16)tf_be16(q + 6) / 16384.0; q += 8; }
            cm[0] = m[0] * a + m[2] * b; cm[1] = m[1] * a + m[3] * b; cm[2] = m[0] * c + m[2] * d; cm[3] = m[1] * c + m[3] * d;
            cm[4] = m[0] * dx + m[2] * dy + m[4]; cm[5] = m[1] * dx + m[3] * dy + m[5];
            tf_ttf_points(t, gi, p, cm, depth + 1);
            more = (fl & 0x20) != 0;
        }
    }
}

typedef struct { u32 code; int adv; DBuf out; int src, gid; } NGlyph;
static DevFace g_named[16]; static int g_nnamed;

static int cmp_code(const void *a, const void *b) { u32 x = ((const NGlyph*)a)->code, y = ((const NGlyph*)b)->code; return x < y ? -1 : x > y; }

DevFace *devfont_register_ttf(const char *name, const char *const *paths, int npaths, int bold, int italic) {
    TTF tt[4]; int nt = 0, i, k, ng = 0, cap = 0; NGlyph *gl = NULL; DBuf blob; DevFace *f; double scale0 = 1;
    u32 code; size_t tbl;
    if (g_nnamed >= 16 || npaths < 1) return NULL;
    for (i = 0; i < npaths && nt < 4; i++) if (tf_ttf_open(&tt[nt], paths[i])) nt++; else free(tt[nt].d);
    if (!nt) return NULL;
    scale0 = 1024.0 / tt[0].upem;
    /* the codes each file maps: the first file wins */
    for (code = 1; code < 0x10000; code++) {
        for (k = 0; k < nt; k++) {
            int g = tf_ttf_glyph_of(&tt[k], code); double sc = 1024.0 / tt[k].upem; Pts p; double m[6]; int c, j, px = 0, py = 0;
            if (g <= 0) continue;
            if (ng == cap) { cap = cap * 2 + 64; gl = (NGlyph*)realloc(gl, (size_t)cap * sizeof(NGlyph)); if (!gl) return NULL; }
            memset(&gl[ng], 0, sizeof(NGlyph)); gl[ng].code = code; gl[ng].src = k; gl[ng].gid = g;
            gl[ng].adv = (int)floor(tf_ttf_advance(&tt[k], g) * sc + 0.5);
            memset(&p, 0, sizeof(p)); m[0] = sc; m[1] = 0; m[2] = 0; m[3] = sc; m[4] = 0; m[5] = 0;
            tf_ttf_points(&tt[k], g, &p, m, 0);
            { int nc = 0, st = 0; for (c = 0; c < p.ne; c++) { if (p.ends[c] - st >= 2) nc++; st = p.ends[c]; } db_varint(&gl[ng].out, (u32)nc); }
            { int st = 0; for (c = 0; c < p.ne; c++) { int en = p.ends[c]; if (en - st >= 2) { db_varint(&gl[ng].out, (u32)(en - st)); for (j = st; j < en; j++) { db_varint(&gl[ng].out, tf_zz(p.x[j] - px) * 2 | p.on[j]); db_varint(&gl[ng].out, tf_zz(p.y[j] - py)); px = p.x[j]; py = p.y[j]; } } st = en; } }
            free(p.x); free(p.y); free(p.on); free(p.ends);
            ng++;
            break;
        }
    }
    qsort(gl, (size_t)ng, sizeof(NGlyph), cmp_code);
    /* blob: glyph table (u16 code, u16 advance, u32 outline offset), outlines, kerning */
    memset(&blob, 0, sizeof(blob));
    tbl = (size_t)ng * 8;
    { u8 z[8]; memset(z, 0, 8); for (i = 0; i < ng; i++) db_put(&blob, z, 8); }
    for (i = 0; i < ng; i++) {
        u32 o = (u32)blob.n; u8 *e = blob.d + i * 8;
        e[0] = (u8)gl[i].code; e[1] = (u8)(gl[i].code >> 8); e[2] = (u8)gl[i].adv; e[3] = (u8)(gl[i].adv >> 8);
        e[4] = (u8)o; e[5] = (u8)(o >> 8); e[6] = (u8)(o >> 16); e[7] = (u8)(o >> 24);
        db_put(&blob, gl[i].out.d, gl[i].out.n);
        e = blob.d + i * 8; e[4] = (u8)o; e[5] = (u8)(o >> 8); e[6] = (u8)(o >> 16); e[7] = (u8)(o >> 24);
    }
    f = &g_named[g_nnamed];
    memset(f, 0, sizeof(*f));
    f->kern = NULL; f->nkern = 0;
    /* legacy 'kern' table (format 0) of the first file, between mapped codes */
    if (tt[0].kern && tt[0].kern_len >= 4) {
        size_t kstart = blob.n; int nk = 0, nsub = tf_be16(tt[0].kern + 2), si; const u8 *q = tt[0].kern + 4, *ke = tt[0].kern + tt[0].kern_len;
        u32 *gcode = (u32*)calloc((size_t)tt[0].nglyphs + 1, sizeof(u32));
        if (gcode) for (i = 0; i < ng; i++) if (gl[i].src == 0) { int g = tf_ttf_glyph_of(&tt[0], gl[i].code); if (g > 0 && g < tt[0].nglyphs && !gcode[g]) gcode[g] = gl[i].code; }
        for (si = 0; gcode && si < nsub && q + 6 <= ke; si++) {
            int slen = tf_be16(q + 2), cov = tf_be16(q + 4); const u8 *pr = q + 14; int np = q + 14 <= ke ? tf_be16(q + 6) : 0, j;
            if ((cov >> 8) == 0 && (cov & 1)) {
                for (j = 0; j < np && pr + 6 <= ke; j++, pr += 6) {
                    int l = tf_be16(pr), r = tf_be16(pr + 2), v = (s16)tf_be16(pr + 4);
                    if (l < tt[0].nglyphs && r < tt[0].nglyphs && gcode[l] && gcode[r] && v) {
                        int kv = (int)floor(v * scale0 + 0.5); u8 b6[6];
                        b6[0] = (u8)gcode[l]; b6[1] = (u8)(gcode[l] >> 8); b6[2] = (u8)gcode[r]; b6[3] = (u8)(gcode[r] >> 8); b6[4] = (u8)kv; b6[5] = (u8)(kv >> 8);
                        db_put(&blob, b6, 6); nk++;
                    }
                }
            }
            if (slen < 6) break;
            q += slen;
        }
        free(gcode);
        /* sorted by (left, right) for the binary search */
        if (nk > 1) {
            int a, b2; u8 tmp[6];
            for (a = 1; a < nk; a++) for (b2 = a; b2 > 0; b2--) {
                u8 *x = blob.d + kstart + (b2 - 1) * 6, *y = blob.d + kstart + b2 * 6;
                u32 kx = ((u32)(x[0] | (x[1] << 8)) << 16) | (u32)(x[2] | (x[3] << 8)), ky = ((u32)(y[0] | (y[1] << 8)) << 16) | (u32)(y[2] | (y[3] << 8));
                if (kx <= ky) break;
                memcpy(tmp, x, 6); memcpy(x, y, 6); memcpy(y, tmp, 6);
            }
        }
        f->nkern = nk;
        f->kern = (const u8*)(size_t)kstart;   /* fixed up below */
    }
    /* 'hdmx' of the first file: device advances per pixel size */
    {
        u32 hl = 0; const u8 *h = tf_ttf_table(&tt[0], "hdmx", &hl);
        if (h && hl >= 8) {
            int nrec = (s16)tf_be16(h + 2), r; u32 rs = tf_be32(h + 4);
            if (nrec > 64) nrec = 64;
            if (nrec > 0 && rs >= 2 && 8 + (u32)nrec * rs <= hl && (f->hdmx_w = (u8*)calloc((size_t)nrec * (ng ? ng : 1), 1)) != NULL) {
                for (r = 0; r < nrec; r++) {
                    const u8 *rec = h + 8 + r * rs;
                    f->hdmx_ppem[r] = rec[0];
                    for (i = 0; i < ng; i++) f->hdmx_w[(size_t)r * ng + i] = (gl[i].src == 0 && gl[i].gid >= 0 && (u32)gl[i].gid + 2 < rs) ? rec[2 + gl[i].gid] : 0;
                }
                f->nhdmx = nrec;
            }
        }
    }
    for (i = 0; i < ng; i++) free(gl[i].out.d);
    free(gl);
    f->family = DEVFONT_SANS; f->style = (bold ? 1 : 0) | (italic ? 2 : 0); f->em = 1024;
    f->ascent = (int)floor(tt[0].asc * scale0 + 0.5); f->descent = (int)floor(tt[0].desc * scale0 + 0.5); f->leading = 0;
    f->nglyphs = ng; f->base = blob.d; f->table = blob.d; f->kern = blob.d + (size_t)f->kern;
    f->shapes = (ShapeDef**)calloc((size_t)ng + 1, sizeof(ShapeDef*));
    strncpy(f->name, name, sizeof(f->name) - 1);
    for (i = 0; i < nt; i++) free(tt[i].d);
    (void)tbl;
    g_nnamed++;
    return f;
}

DevFace *devfont_find_named(const char *name, int len, int bold, int italic) {
    int i, pass; (void)italic;
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < g_nnamed; i++) {
            const char *a = g_named[i].name; int k;
            for (k = 0; k < len && a[k]; k++) { char x = a[k], y = name[k]; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) break; }
            if (k != len || a[k]) continue;
            if (pass == 0 && (g_named[i].style & 1) != (bold ? 1 : 0)) continue;
            return &g_named[i];
        }
    return NULL;
}
static void shape_bounds_of_edges(ShapeDef *s) {
    int i; s32 x0 = 0x7fffffff, y0 = 0x7fffffff, x1 = -0x7fffffff, y1 = -0x7fffffff;
    for (i = 0; i < s->nedges; i++) { ShapeEdge *e = &s->edges[i];
        if (e->x0 < x0) x0 = e->x0; if (e->x0 > x1) x1 = e->x0; if (e->y0 < y0) y0 = e->y0; if (e->y0 > y1) y1 = e->y0;
        if (e->x1 < x0) x0 = e->x1; if (e->x1 > x1) x1 = e->x1; if (e->y1 < y0) y0 = e->y1; if (e->y1 > y1) y1 = e->y1; }
    if (s->nedges) { s->bounds.xmin = x0; s->bounds.xmax = x1; s->bounds.ymin = y0; s->bounds.ymax = y1; s->edge_bounds = s->bounds; }
}

/* ---------------------------------------------------------------- */
/* device fonts from TrueType files (read as ttf-parser does)       */

static u16 mbe16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static u32 mbe32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static u32 mttf_table(const u8 *d, u32 n, const char *tag, u32 *len) {
    u32 i, nt;
    if (n < 12) return 0;
    nt = mbe16(d + 4);
    for (i = 0; i < nt && 12 + i * 16 + 16 <= n; i++) {
        const u8 *r = d + 12 + i * 16; u32 off = mbe32(r + 8), l = mbe32(r + 12);
        if (!memcmp(r, tag, 4) && off <= n && l <= n - off) { *len = l; return off; }
    }
    *len = 0; return 0;
}
/* the outline of glyph `g` under transform t (a b c d e f), with composite glyphs expanded */
typedef struct { float a, b, c, d, e, f; } TtfXf;
static void ttf_outline(const DevFace *f, u32 g, TtfXf t, int depth, TtfWalk *w) {
    u32 o0, o1, nglyf; const u8 *d = f->ttf; const u8 *gp; s16 nc; int identity = t.a == 1 && t.b == 0 && t.c == 0 && t.d == 1 && t.e == 0 && t.f == 0;
    if (depth > 32) return;
    if (f->loca_long) { o0 = mbe32(d + f->loca + g * 4); o1 = mbe32(d + f->loca + g * 4 + 4); }
    else { o0 = 2u * mbe16(d + f->loca + g * 2); o1 = 2u * mbe16(d + f->loca + g * 2 + 2); }
    nglyf = f->ttf_size - f->glyf;
    if (o1 <= o0 || o1 > nglyf || o1 - o0 < 10) return;
    gp = d + f->glyf + o0; nc = (s16)mbe16(gp);
    if (nc > 0) {
        const u8 *p = gp + 10, *end = d + f->glyf + o1; int np, i, j, k; u16 *ends; u8 *fl; s32 *xs, *ys; s32 v;
        ends = (u16*)malloc((size_t)nc * sizeof(u16)); if (!ends) return;
        for (i = 0; i < nc; i++) ends[i] = mbe16(p + i * 2);
        np = ends[nc - 1] + 1; p += nc * 2; p += 2 + mbe16(p);
        fl = (u8*)malloc((size_t)np); xs = (s32*)malloc((size_t)np * sizeof(s32)); ys = (s32*)malloc((size_t)np * sizeof(s32));
        if (!fl || !xs || !ys || p > end) { free(ends); free(fl); free(xs); free(ys); return; }
        for (i = 0; i < np && p < end; ) { u8 c = *p++; fl[i++] = c; if (c & 8) { int r = p < end ? *p++ : 0; while (r-- > 0 && i < np) fl[i++] = c; } }
        for (v = 0, i = 0; i < np; i++) { u8 c = fl[i]; if (c & 2) { int dv = p < end ? *p++ : 0; v += (c & 16) ? dv : -dv; } else if (!(c & 16)) { v += (s16)(p + 1 < end ? mbe16(p) : 0); p += 2; } xs[i] = v; }
        for (v = 0, i = 0; i < np; i++) { u8 c = fl[i]; if (c & 4) { int dv = p < end ? *p++ : 0; v += (c & 32) ? dv : -dv; } else if (!(c & 32)) { v += (s16)(p + 1 < end ? mbe16(p) : 0); p += 2; } ys[i] = v; }
        for (k = 0, j = 0; k < nc; k++) {
            for (; j <= ends[k] && j < np; j++) {
                float x = (float)xs[j], y = (float)ys[j];
                if (!identity) { float tx = t.a * x + t.c * y + t.e, ty = t.b * x + t.d * y + t.f; x = tx; y = ty; }
                tw_point(w, x, y, fl[j] & 1);
            }
            tw_finish(w);
        }
        free(ends); free(fl); free(xs); free(ys);
    } else if (nc < 0) {
        const u8 *p = gp + 10, *end = d + f->glyf + o1; u16 flags;
        do {
            TtfXf c, r; u16 cg; float ax = 0, ay = 0;
            if (p + 4 > end) break;
            flags = mbe16(p); cg = mbe16(p + 2); p += 4;
            if (flags & 1) { if (p + 4 > end) break; if (flags & 2) { ax = (float)(s16)mbe16(p); ay = (float)(s16)mbe16(p + 2); } p += 4; }
            else { if (p + 2 > end) break; if (flags & 2) { ax = (float)(signed char)p[0]; ay = (float)(signed char)p[1]; } p += 2; }
            c.a = 1; c.b = 0; c.c = 0; c.d = 1; c.e = ax; c.f = ay;
            if (flags & 8) { if (p + 2 > end) break; c.a = c.d = (float)(s16)mbe16(p) / 16384.0f; p += 2; }
            else if (flags & 0x40) { if (p + 4 > end) break; c.a = (float)(s16)mbe16(p) / 16384.0f; c.d = (float)(s16)mbe16(p + 2) / 16384.0f; p += 4; }
            else if (flags & 0x80) { if (p + 8 > end) break; c.a = (float)(s16)mbe16(p) / 16384.0f; c.b = (float)(s16)mbe16(p + 2) / 16384.0f; c.c = (float)(s16)mbe16(p + 4) / 16384.0f; c.d = (float)(s16)mbe16(p + 6) / 16384.0f; p += 8; }
            r.a = t.a * c.a + t.c * c.b; r.b = t.b * c.a + t.d * c.b; r.c = t.a * c.c + t.c * c.d; r.d = t.b * c.c + t.d * c.d;
            r.e = t.a * c.e + t.c * c.f + t.e; r.f = t.b * c.e + t.d * c.f + t.f;
            ttf_outline(f, cg, r, depth + 1, w);
        } while (flags & 0x20);
    }
}
static ShapeDef *ttf_glyph_shape(DevFace *f, int gi) {
    ShapeDef *s = (ShapeDef*)calloc(1, sizeof(ShapeDef)); TtfWalk w; TtfXf t;
    if (!s) return NULL;
    s->version = 1;
    s->fills = (FillStyle*)calloc(1, sizeof(FillStyle));
    if (s->fills) { s->nfills = 1; s->capfills = 1; s->fills[0].type = 0; s->fills[0].color.a = 255; swf_matrix_identity(&s->fills[0].matrix); }
    memset(&w, 0, sizeof(w)); w.s = s;
    t.a = 1; t.b = 0; t.c = 0; t.d = 1; t.e = 0; t.f = 0;
    ttf_outline(f, f->gid[gi], t, 0, &w);
    shape_bounds_of_edges(s);
    return s;
}

typedef struct { u16 code, gid; } CodeGid;
static int cmp_cg(const void *a, const void *b) { return (int)((const CodeGid*)a)->code - (int)((const CodeGid*)b)->code; }
static int cmp_kern(const void *a, const void *b) { u32 x = ((u32)rd16((const u8*)a) << 16) | rd16((const u8*)a + 2), y = ((u32)rd16((const u8*)b) << 16) | rd16((const u8*)b + 2); return x < y ? -1 : x > y; }
static void le16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }

DevFace *devfont_load_ttf(const u8 *d, u32 n) {
    u32 hl, ml, hhl, osl, hml, ll, gl, cl, kl, head, maxp, hhea, os2, hmtx, loca, glyf, cmap, kern, i, nglyphs, nhm, ncg = 0, capcg = 0;
    DevFace *f; CodeGid *cg = NULL; s16 asc, desc, gap; int use_typo = 0;
    head = mttf_table(d, n, "head", &hl); maxp = mttf_table(d, n, "maxp", &ml); hhea = mttf_table(d, n, "hhea", &hhl);
    os2 = mttf_table(d, n, "OS/2", &osl); hmtx = mttf_table(d, n, "hmtx", &hml); loca = mttf_table(d, n, "loca", &ll);
    glyf = mttf_table(d, n, "glyf", &gl); cmap = mttf_table(d, n, "cmap", &cl); kern = mttf_table(d, n, "kern", &kl);
    if (!head || hl < 54 || !maxp || ml < 6 || !hhea || hhl < 36 || !hmtx || !loca || !glyf || !cmap) return NULL;
    nglyphs = mbe16(d + maxp + 4); nhm = mbe16(d + hhea + 34);
    if (!nhm || hml < nhm * 4) return NULL;
    /* metrics as ttf-parser reports them */
    if (os2 && osl >= 78) use_typo = (mbe16(d + os2 + 62) & 0x80) != 0;
    if (use_typo) { asc = (s16)mbe16(d + os2 + 68); desc = (s16)mbe16(d + os2 + 70); gap = (s16)mbe16(d + os2 + 72); }
    else {
        asc = (s16)mbe16(d + hhea + 4); desc = (s16)mbe16(d + hhea + 6); gap = (s16)mbe16(d + hhea + 8);
        if (os2 && osl >= 78) {
            s16 ta = (s16)mbe16(d + os2 + 68), td = (s16)mbe16(d + os2 + 70);
            if ((s16)mbe16(d + hhea + 4) == 0) asc = ta ? ta : (s16)mbe16(d + os2 + 74);
            if ((s16)mbe16(d + hhea + 6) == 0) desc = td ? td : (s16)-(s16)mbe16(d + os2 + 76);
            if ((s16)mbe16(d + hhea + 4) == 0 || (s16)mbe16(d + hhea + 6) == 0) gap = (ta || td) ? (s16)mbe16(d + os2 + 72) : 0;
        }
    }
    /* the Unicode cmap subtables, the first mapping of a code wins */
    {
        u32 nsub = mbe16(d + cmap + 2), k;
        u8 *seen = (u8*)calloc(65536, 1);
        if (!seen) return NULL;
        for (k = 0; k < nsub && 4 + k * 8 + 8 <= cl; k++) {
            const u8 *r = d + cmap + 4 + k * 8; u16 plat = mbe16(r), enc = mbe16(r + 2); u32 so = cmap + mbe32(r + 4), fmt;
            if (!(plat == 0 || (plat == 3 && (enc == 1 || enc == 10)))) continue;
            if (so + 4 > n) continue;
            fmt = mbe16(d + so);
            if (fmt == 4) {
                u32 segx2 = mbe16(d + so + 6), sg; const u8 *ends = d + so + 14, *starts = ends + segx2 + 2, *deltas = starts + segx2, *ros = deltas + segx2;
                if (ros + segx2 > d + n) continue;
                for (sg = 0; sg < segx2 / 2; sg++) {
                    u32 st = mbe16(starts + sg * 2), en = mbe16(ends + sg * 2), c; u16 delta = mbe16(deltas + sg * 2), ro = mbe16(ros + sg * 2);
                    for (c = st; c <= en && c < 0xFFFF; c++) {
                        u32 gidv;
                        if (ro) { const u8 *gp = ros + sg * 2 + ro + (c - st) * 2; if (gp + 2 > d + n) continue; gidv = mbe16(gp); if (gidv) gidv = (gidv + delta) & 0xFFFF; }
                        else gidv = (c + delta) & 0xFFFF;
                        if (!gidv || seen[c] || gidv >= nglyphs) continue;
                        if (ncg == capcg) { capcg = capcg ? capcg * 2 : 256; cg = (CodeGid*)realloc(cg, capcg * sizeof(CodeGid)); if (!cg) { free(seen); return NULL; } }
                        seen[c] = 1; cg[ncg].code = (u16)c; cg[ncg].gid = (u16)gidv; ncg++;
                    }
                }
            } else if (fmt == 12) {
                u32 ng = mbe32(d + so + 12), gi2;
                for (gi2 = 0; gi2 < ng && so + 16 + gi2 * 12 + 12 <= n; gi2++) {
                    const u8 *gr = d + so + 16 + gi2 * 12; u32 st = mbe32(gr), en = mbe32(gr + 4), sgid = mbe32(gr + 8), c;
                    for (c = st; c <= en && c < 0x10000; c++) {
                        u32 gidv = sgid + (c - st);
                        if (!gidv || seen[c] || gidv >= nglyphs) continue;
                        if (ncg == capcg) { capcg = capcg ? capcg * 2 : 256; cg = (CodeGid*)realloc(cg, capcg * sizeof(CodeGid)); if (!cg) { free(seen); return NULL; } }
                        seen[c] = 1; cg[ncg].code = (u16)c; cg[ncg].gid = (u16)gidv; ncg++;
                    }
                }
            }
        }
        free(seen);
    }
    if (ncg) qsort(cg, ncg, sizeof(CodeGid), cmp_cg);
    f = (DevFace*)calloc(1, sizeof(DevFace));
    if (!f) { free(cg); return NULL; }
    f->em = mbe16(d + head + 18); if (!f->em) f->em = 1000;
    f->ascent = asc; f->descent = -desc; f->leading = gap;
    f->nglyphs = (int)ncg; f->ttf = d; f->ttf_size = n; f->glyf = glyf; f->loca = loca; f->loca_long = mbe16(d + head + 50) != 0;
    {
        u8 *tab = (u8*)calloc((size_t)ncg * 8 + 8, 1);
        f->gid = (u16*)calloc((size_t)ncg + 1, sizeof(u16)); f->shapes = (ShapeDef**)calloc((size_t)ncg + 1, sizeof(ShapeDef*));
        if (!tab || !f->gid || !f->shapes) { free(tab); free(f->gid); free(f->shapes); free(f); free(cg); return NULL; }
        for (i = 0; i < ncg; i++) {
            u32 g = cg[i].gid, hm = g < nhm ? g : nhm - 1;
            le16(tab + i * 8, cg[i].code); le16(tab + i * 8 + 2, mbe16(d + hmtx + hm * 4)); f->gid[i] = (u16)g;
        }
        f->table = tab;
    }
    /* kerning: the horizontal format-0 subtables of 'kern', by character codes */
    if (kern && kl >= 4 && mbe16(d + kern) == 0) {
        u32 nt = mbe16(d + kern + 2), off = kern + 4, t2, nk = 0, capk = 0; u8 *kt = NULL;
        for (t2 = 0; t2 < nt && off + 6 <= kern + kl; t2++) {
            u32 len = mbe16(d + off + 2); u16 cov = mbe16(d + off + 4);
            if ((cov & 1) && (cov >> 8) == 0 && off + 14 <= n) {
                u32 np2 = mbe16(d + off + 6), q;
                for (q = 0; q < np2 && off + 14 + q * 6 + 6 <= n; q++) {
                    const u8 *pr = d + off + 14 + q * 6; u16 lg = mbe16(pr), rg = mbe16(pr + 2); s16 val = (s16)mbe16(pr + 4); u32 a, b;
                    for (a = 0; a < ncg; a++) if (cg[a].gid == lg) for (b = 0; b < ncg; b++) if (cg[b].gid == rg) {
                        u8 *e;
                        if (nk == capk) { capk = capk ? capk * 2 : 64; kt = (u8*)realloc(kt, capk * 6); if (!kt) { nk = capk = 0; break; } }
                        e = kt + nk * 6; le16(e, cg[a].code); le16(e + 2, cg[b].code); le16(e + 4, (u16)val); nk++;
                    }
                }
            }
            off += len ? len : 6;
        }
        if (kt && nk) {
            /* the first subtable's pair wins: stable sort, then drop repeats */
            u32 a, m2 = 0;
            for (a = 1; a < nk; a++) { u8 tmp[6]; u32 b = a; memcpy(tmp, kt + a * 6, 6); while (b > 0 && cmp_kern(kt + (b - 1) * 6, tmp) > 0) { memcpy(kt + b * 6, kt + (b - 1) * 6, 6); b--; } memcpy(kt + b * 6, tmp, 6); }
            for (a = 0; a < nk; a++) if (!m2 || cmp_kern(kt + (m2 - 1) * 6, kt + a * 6) != 0) { if (m2 != a) memcpy(kt + m2 * 6, kt + a * 6, 6); m2++; }
            f->kern = kt; f->nkern = (int)m2;
        } else free(kt);
    }
    free(cg);
    return f;
}

/* ---- the registry and Ruffle's device font resolution ---- */
int g_devfont_registry;
typedef struct { char name[64]; int bold, italic; DevFace *face; } RegFont;
static RegFont g_reg[32]; static int g_nreg;
static char g_defnames[3][8][64]; static int g_ndef[3];
static int lc_eq(const char *a, const char *b) { while (*a && *b) { char x = *a, y = *b; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) return 0; a++; b++; } return *a == *b; }
void devfont_register(const char *family, int bold, int italic, DevFace *f) {
    if (!f || g_nreg >= 32) return;
    strncpy(g_reg[g_nreg].name, family, 63); g_reg[g_nreg].name[63] = 0; g_reg[g_nreg].bold = bold ? 1 : 0; g_reg[g_nreg].italic = italic ? 1 : 0; g_reg[g_nreg].face = f; g_nreg++;
    g_devfont_registry = 1;
}
void devfont_set_default(int which, const char *names) {
    const char *p = names;
    if (which < 0 || which > 2) return;
    g_ndef[which] = 0;
    while (p && *p && g_ndef[which] < 8) {
        int k = 0; while (*p == ' ') p++;
        while (p[k] && p[k] != ',' && k < 63) k++;
        memcpy(g_defnames[which][g_ndef[which]], p, (size_t)k); g_defnames[which][g_ndef[which]][k] = 0;
        while (k > 0 && g_defnames[which][g_ndef[which]][k - 1] == ' ') g_defnames[which][g_ndef[which]][--k] = 0;
        if (k) g_ndef[which]++;
        p += k; while (*p && *p != ',') p++; if (*p == ',') p++;
    }
    g_devfont_registry = 1;
}
static DevFace *reg_get(const char *name, int bold, int italic) {
    int i; for (i = 0; i < g_nreg; i++) if (g_reg[i].bold == bold && g_reg[i].italic == italic && lc_eq(g_reg[i].name, name)) return g_reg[i].face;
    return NULL;
}
/* Library::find: exact, then the style fallbacks in Ruffle's order */
static DevFace *reg_find(const char *name, int bold, int italic) {
    DevFace *f;
    if ((f = reg_get(name, bold, italic)) != NULL) return f;
    if (bold ^ italic) {
        if ((f = reg_get(name, 1, 1)) != NULL) return f;
        if ((f = reg_get(name, 0, 0)) != NULL) return f;
        return reg_get(name, !bold, !italic);
    }
    if (bold && italic && (f = reg_get(name, 0, 0)) != NULL) return f;
    if ((f = reg_get(name, 1, 0)) != NULL) return f;
    if ((f = reg_get(name, 0, 1)) != NULL) return f;
    if (!bold && !italic) return reg_get(name, 1, 1);
    return NULL;
}
static DevFace *default_font(int which, int bold, int italic) {
    int i; DevFace *f;
    for (i = 0; i < g_ndef[which]; i++) if ((f = reg_get(g_defnames[which][i], bold, italic)) != NULL) return f;
    for (i = 0; i < g_ndef[which]; i++) if ((f = reg_find(g_defnames[which][i], bold, italic)) != NULL) return f;
    return NULL;
}
/* layout's resolve_font for device text; *found 0: the text is missing */
DevFace *devfont_lookup(const char *names, int bold, int italic, int *found) {
    char nm[256], first[256]; const char *p = names ? names : ""; int list = strchr(p, ',') != NULL, which; DevFace *f;
    first[0] = 0;
    for (;;) {
        int k = 0;
        while (p[k] && p[k] != ',' && k < 255) { nm[k] = p[k]; k++; }
        nm[k] = 0;
        if (list) { char *a = nm, *e; while (*a == ' ') a++; e = a + strlen(a); while (e > a && e[-1] == ' ') *--e = 0; memmove(nm, a, strlen(a) + 1); }
        if (!first[0]) strcpy(first, nm);
        which = !strcmp(nm, "_sans") ? 0 : !strcmp(nm, "_serif") ? 1 : !strcmp(nm, "_typewriter") ? 2 : -1;
        if (which >= 0) { f = default_font(which, bold, italic); *found = f != NULL; return f; }
        if ((f = reg_find(nm, bold, italic)) != NULL) { *found = 1; return f; }
        p += k; if (*p != ',') break; p++;
    }
    which = (!strcmp(first, "Times New Roman")) ? 1 : (!strcmp(first, "Consolas") || !strcmp(first, "Courier") || !strcmp(first, "Courier New") || !strcmp(first, "NSimSun")) ? 2 : 0;
    f = default_font(which, bold, italic);
    *found = f != NULL;
    return f;
}
