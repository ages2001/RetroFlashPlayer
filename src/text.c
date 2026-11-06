/* text.c - fonts (DefineFont/2/3), static text, edit text */
#include "chars.h"
#include "shape.h"
#include "render.h"
#include "reader.h"
#include "devfont.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#include "font.h"
#include "textfield.h"

/* device fonts as FontDefs: family x bold x italic */
static FontDef *g_devfonts[19][2][2];   /* sans, serif, mono, then fonts loaded from TrueType files (family 100+) */
FontDef *font_device(int family, int bold, int italic) {
    FontDef *f; DevFace *d; int i, slot;
    if (!((family >= 0 && family <= 2) || (family >= 100 && family < 116))) family = DEVFONT_SERIF;
    slot = family >= 100 ? family - 97 : family;
    bold = bold ? 1 : 0; italic = italic ? 1 : 0;
    if (g_devfonts[slot][bold][italic]) return g_devfonts[slot][bold][italic];
    d = devfont_get(family, bold);
    if (!d) return NULL;
    f = (FontDef*)calloc(1, sizeof(FontDef));
    if (!f) return NULL;
    f->version = 2; f->scale = d->em; f->dev = d; f->slant = italic; f->bold = bold; f->italic = italic;
    f->nglyphs = d->nglyphs; f->has_layout = 1; f->ascent = d->ascent; f->descent = d->descent; f->leading = d->leading;
    f->codes = (u16*)calloc((size_t)d->nglyphs + 1, sizeof(u16)); f->advance = (u16*)calloc((size_t)d->nglyphs + 1, sizeof(u16));
    if (f->codes && f->advance) for (i = 0; i < d->nglyphs; i++) { f->codes[i] = (u16)devfont_code(d, i); f->advance[i] = (u16)devfont_advance(d, i); }
    strcpy(f->name, d->name[0] ? d->name : family == DEVFONT_SANS ? "_sans" : family == DEVFONT_MONO ? "_typewriter" : "_serif");
    g_devfonts[slot][bold][italic] = f;
    return f;
}
/* named device faces (the harness's TrueType fonts) as FontDefs */
static FontDef *font_named(DevFace *d, int italic) {
    static struct { DevFace *d; int italic; FontDef *f; } cache[64]; static int n;
    FontDef *f; int i;
    for (i = 0; i < n; i++) if (cache[i].d == d && cache[i].italic == italic) return cache[i].f;
    f = (FontDef*)calloc(1, sizeof(FontDef));
    if (!f) return NULL;
    f->version = 2; f->scale = 1024.0; f->dev = d; f->slant = italic && !(d->style & 2); f->bold = d->style & 1; f->italic = italic;
    f->nglyphs = d->nglyphs; f->has_layout = 1; f->ascent = d->ascent; f->descent = d->descent; f->leading = d->leading;
    f->codes = (u16*)calloc((size_t)d->nglyphs + 1, sizeof(u16)); f->advance = (u16*)calloc((size_t)d->nglyphs + 1, sizeof(u16));
    if (f->codes && f->advance) for (i = 0; i < d->nglyphs; i++) { f->codes[i] = (u16)devfont_code(d, i); f->advance[i] = (u16)devfont_advance(d, i); }
    { size_t k; for (k = 0; k + 1 < sizeof(f->name) && d->name[k]; k++) f->name[k] = d->name[k]; f->name[k] = 0; }
    if (n < 64) { cache[n].d = d; cache[n].italic = italic; cache[n].f = f; n++; }
    return f;
}

/* FLASHNT_FONTS / FLASHNT_DEFAULT_FONTS: the test harness's device fonts
 * ("Family|bold|italic|file[|fallback files...]" separated by ';', and
 * "sans=Family;serif=Family;typewriter=Family") */
static int g_hfonts = -1;
static char g_hdef[3][64];
static void harness_fonts(void) {
    const char *e; char buf[4096]; char *p, *q;
    if (g_hfonts >= 0) return;
    g_hfonts = 0;
    e = getenv("FLASHNT_FONTS");
    if (e && strlen(e) < sizeof(buf)) {
        strcpy(buf, e);
        for (p = buf; p && *p; p = q) {
            char *fld[8]; int nf = 0; char *r;
            q = strchr(p, ';'); if (q) *q++ = 0;
            for (r = p; r && nf < 8; ) { fld[nf++] = r; r = strchr(r, '|'); if (r) *r++ = 0; }
            if (nf >= 4) { const char *paths[4]; int k, np = 0; for (k = 3; k < nf && np < 4; k++) paths[np++] = fld[k]; if (devfont_register_ttf(fld[0], paths, np, atoi(fld[1]), atoi(fld[2]))) g_hfonts = 1; }
        }
    }
    e = getenv("FLASHNT_DEFAULT_FONTS");
    if (e && strlen(e) < sizeof(buf)) {
        strcpy(buf, e);
        for (p = buf; p && *p; p = q) {
            char *v; int k;
            q = strchr(p, ';'); if (q) *q++ = 0;
            v = strchr(p, '='); if (!v) continue; *v++ = 0;
            k = !strcmp(p, "sans") ? 0 : !strcmp(p, "serif") ? 1 : !strcmp(p, "typewriter") ? 2 : -1;
            if (k >= 0) { strncpy(g_hdef[k], v, 63); g_hdef[k][63] = 0; }
        }
    }
}
/* the harness's default face for a family (sans, serif, typewriter, then any) */
static FontDef *harness_default(int family, int bold, int italic) {
    int order[3], i; order[0] = family == DEVFONT_SANS ? 0 : family == DEVFONT_MONO ? 2 : 1; order[1] = 0; order[2] = 1;
    for (i = 0; i < 3; i++) if (g_hdef[order[i]][0]) { DevFace *d = devfont_find_named(g_hdef[order[i]], (int)strlen(g_hdef[order[i]]), bold, italic); if (d) return font_named(d, italic); }
    return NULL;
}

/* a device font registered from a font file, as a FontDef (no synthesized styles) */
static FontDef *font_registered(DevFace *d, int bold, int italic) {
    static struct { DevFace *d; FontDef *f; } cache[32]; static int n; int i;
    FontDef *f;
    for (i = 0; i < n; i++) if (cache[i].d == d) return cache[i].f;
    f = (FontDef*)calloc(1, sizeof(FontDef));
    if (!f) return NULL;
    f->version = 2; f->scale = d->em; f->dev = d; f->bold = bold; f->italic = italic;
    f->nglyphs = d->nglyphs; f->has_layout = 1; f->ascent = d->ascent; f->descent = d->descent; f->leading = d->leading;
    f->codes = (u16*)calloc((size_t)d->nglyphs + 1, sizeof(u16)); f->advance = (u16*)calloc((size_t)d->nglyphs + 1, sizeof(u16));
    if (f->codes && f->advance) for (i = 0; i < d->nglyphs; i++) { f->codes[i] = (u16)devfont_code(d, i); f->advance[i] = (u16)devfont_advance(d, i); }
    strcpy(f->name, "_device");
    if (n < 32) { cache[n].d = d; cache[n].f = f; n++; }
    return f;
}
FontDef *font_device_by_name(const char *name, int bold, int italic) {
    if (g_devfont_registry) {
        /* fonts registered as Ruffle's harness does (FLASHNT_RUFFLE_FONTS): its resolution; nothing found leaves the text out */
        int found; DevFace *d = devfont_lookup(name, bold, italic, &found);
        return d ? font_registered(d, bold, italic) : NULL;
    }
    harness_fonts();
    if (g_hfonts > 0 && name) {
        /* a comma list: the first name that resolves (entries trimmed; a single name is
         * looked up as it is), "_sans" & co. are the default fonts */
        const char *p = name; int list = strchr(name, ',') != NULL;
        while (*p) {
            const char *e = strchr(p, ','), *b = p, *t; int len;
            if (!e) e = p + strlen(p);
            t = e;
            if (list) { while (b < t && (*b == ' ' || *b == '\t')) b++; while (t > b && (t[-1] == ' ' || t[-1] == '\t')) t--; }
            len = (int)(t - b);
            if (len == 6 && !strncmp(b, "_serif", 6)) { FontDef *f = harness_default(DEVFONT_SERIF, bold, italic); if (f) return f; }
            else if (len == 5 && !strncmp(b, "_sans", 5)) { FontDef *f = harness_default(DEVFONT_SANS, bold, italic); if (f) return f; }
            else if (len == 11 && !strncmp(b, "_typewriter", 11)) { FontDef *f = harness_default(DEVFONT_MONO, bold, italic); if (f) return f; }
            else { DevFace *d = devfont_find_named(b, len, bold, italic); if (d) return font_named(d, italic); }
            p = *e ? e + 1 : e;
        }
        { FontDef *f = harness_default(devfont_family_for_name(name), bold, italic); if (f) return f; }
    }
    return font_device(devfont_family_for_name(name), bold, italic);
}

FontDef *font_get(SWFMovie *m, int id) {
    SWFChar *ch = swf_get_char(m, id);
    FontDef *f; Reader r; int i;
    if (!ch || ch->type != CHAR_FONT) return NULL;
    if (ch->parsed) return (FontDef*)ch->parsed;
    f = (FontDef*)calloc(1, sizeof(FontDef));
    if (!f) return NULL;
    reader_init(&r, ch->data, ch->len);
    r_u16(&r);
    if (ch->tagcode == TAG_DefineFont) {
        size_t base = r.pos; int first; u16 *offs;
        f->version = 1; f->scale = 1024.0;
        first = r_u16(&r);
        f->nglyphs = first / 2;
        offs = (u16*)malloc((size_t)f->nglyphs * sizeof(u16));
        f->glyph_data = (const u8**)calloc((size_t)f->nglyphs, sizeof(u8*));
        f->glyph_len = (size_t*)calloc((size_t)f->nglyphs, sizeof(size_t));
        f->glyphs = (ShapeDef**)calloc((size_t)f->nglyphs, sizeof(ShapeDef*));
        f->codes = (u16*)calloc((size_t)f->nglyphs, sizeof(u16));
        if (!offs || !f->glyph_data || !f->glyph_len || !f->glyphs || !f->codes) { free(offs); return f; }
        offs[0] = (u16)first;
        for (i = 1; i < f->nglyphs; i++) offs[i] = r_u16(&r);
        for (i = 0; i < f->nglyphs; i++) {
            f->glyph_data[i] = ch->data + base + offs[i];
            f->glyph_len[i] = (i + 1 < f->nglyphs ? offs[i + 1] : (u16)(ch->len - base)) - offs[i];
            f->codes[i] = (u16)i;
        }
        free(offs);
        /* DefineFontInfo: name + flags + code table */
        if (ch->info) {
            Reader ir; int nl, fl, wide, v2 = 0;
            reader_init(&ir, ch->info, ch->infolen);
            r_u16(&ir);
            nl = r_u8(&ir);
            { const u8 *nm = r_bytes(&ir, (size_t)nl); if (nm) { int n = nl < 63 ? nl : 63; memcpy(f->name, nm, (size_t)n); f->name[n] = 0; } }
            fl = r_u8(&ir);
            /* DefineFontInfo2 has a language byte after flags */
            if (ch->infolen > (size_t)(4 + nl) && (ch->infolen - 4 - nl) == (size_t)f->nglyphs * ((fl & 1) ? 2 : 1) + 1) { r_u8(&ir); v2 = 1; }
            wide = fl & 1;
            for (i = 0; i < f->nglyphs && !reader_eof(&ir); i++) f->codes[i] = wide ? r_u16(&ir) : r_u8(&ir);
            (void)v2;
        }
    } else {
        int flags = r_u8(&r), wide_off, wide_codes, namelen; size_t base; u32 *offs; u32 code_off;
        f->version = (ch->tagcode == TAG_DefineFont3) ? 3 : 2;
        f->scale = (f->version == 3) ? 1024.0 * 20.0 : 1024.0;
        wide_off = flags & 0x08; wide_codes = flags & 0x04;
        f->bold = flags & 1; f->italic = flags & 2;
        f->has_layout = flags & 0x80;
        r_u8(&r);                                   /* language */
        namelen = r_u8(&r);
        { const u8 *nm = r_bytes(&r, (size_t)namelen); if (nm) { int n = namelen < 63 ? namelen : 63; memcpy(f->name, nm, (size_t)n); f->name[n] = 0; } }
        f->nglyphs = r_u16(&r);
        base = r.pos;
        offs = (u32*)malloc(((size_t)f->nglyphs + 1) * sizeof(u32));
        f->glyph_data = (const u8**)calloc((size_t)f->nglyphs + 1, sizeof(u8*));
        f->glyph_len = (size_t*)calloc((size_t)f->nglyphs + 1, sizeof(size_t));
        f->glyphs = (ShapeDef**)calloc((size_t)f->nglyphs + 1, sizeof(ShapeDef*));
        f->codes = (u16*)calloc((size_t)f->nglyphs + 1, sizeof(u16));
        if (!offs || !f->glyph_data || !f->glyph_len || !f->glyphs || !f->codes) { free(offs); return f; }
        for (i = 0; i < f->nglyphs; i++) offs[i] = wide_off ? r_u32(&r) : r_u16(&r);
        code_off = wide_off ? r_u32(&r) : r_u16(&r);
        for (i = 0; i < f->nglyphs; i++) {
            f->glyph_data[i] = ch->data + base + offs[i];
            f->glyph_len[i] = (i + 1 < f->nglyphs ? offs[i + 1] : code_off) - offs[i];
        }
        r.pos = base + code_off; r.bit = 0;
        for (i = 0; i < f->nglyphs; i++) f->codes[i] = wide_codes ? r_u16(&r) : r_u8(&r);
        if (f->has_layout && r.pos < r.len) {
            f->ascent = r_s16(&r); f->descent = r_s16(&r); f->leading = r_s16(&r);
            f->advance = (u16*)calloc((size_t)f->nglyphs + 1, sizeof(u16));
            if (f->advance) for (i = 0; i < f->nglyphs; i++) f->advance[i] = r_u16(&r);
            {   /* glyph bounds, then the kerning table */
                SWFRect rc; int nk;
                for (i = 0; i < f->nglyphs && !reader_eof(&r); i++) { r_rect(&r, &rc); reader_align(&r); }
                nk = reader_eof(&r) ? 0 : r_u16(&r);
                if (nk > 0) {
                    f->kern_l = (u16*)malloc((size_t)nk * sizeof(u16)); f->kern_r = (u16*)malloc((size_t)nk * sizeof(u16)); f->kern_adj = (s16*)malloc((size_t)nk * sizeof(s16));
                    if (f->kern_l && f->kern_r && f->kern_adj) {
                        for (i = 0; i < nk && !reader_eof(&r); i++) {
                            f->kern_l[i] = wide_codes ? r_u16(&r) : r_u8(&r); f->kern_r[i] = wide_codes ? r_u16(&r) : r_u8(&r); f->kern_adj[i] = r_s16(&r);
                        }
                        f->nkern = i;
                    }
                }
            }
        }
        free(offs);
    }
    ch->parsed = f;
    return f;
}

ShapeDef *font_glyph(FontDef *f, int idx) {
    Reader r; ShapeDef *s;
    if (!f || idx < 0 || idx >= f->nglyphs) return NULL;
    if (f->dev) return devfont_shape(f->dev, idx);
    if (f->glyphs[idx]) return f->glyphs[idx];
    s = (ShapeDef*)calloc(1, sizeof(ShapeDef));
    if (!s) return NULL;
    s->version = 1;
    /* glyph SHAPE: single implicit fill style 1 */
    s->fills = (FillStyle*)calloc(1, sizeof(FillStyle));
    if (s->fills) { s->nfills = 1; s->capfills = 1; s->fills[0].type = FILL_SOLID; s->fills[0].color.a = 255; swf_matrix_identity(&s->fills[0].matrix); }
    reader_init(&r, f->glyph_data[idx], f->glyph_len[idx]);
    shape_parse_records(&r, s, 1, 0);
    s->has_brk = 1; s->cache_paths = 1;
    s->nonzero = 1;   /* Ruffle swf_glyph_to_shape: glyphs fill with the non-zero rule */
    f->glyphs[idx] = s;
    return s;
}

int font_glyph_for_code(FontDef *f, int code) {
    int i;
    if (!f) return -1;
    if (f->dev) return devfont_glyph(f->dev, code);
    for (i = 0; i < f->nglyphs; i++) if (f->codes[i] == code) return i;
    return -1;
}

/* draw one glyph: em-space -> text space via height, placed at (x,y) twips */
void draw_glyph(Player *p, FontDef *f, int gi, const SWFMatrix *wm, double x, double y, double height, SWFColor color, const SWFCxform *cx) {
    ShapeDef *g = font_glyph(f, gi);
    SWFMatrix gm, full;
    if (!g || g->nedges == 0) return;
    swf_matrix_identity(&gm);
    gm.sx = gm.sy = (double)((float)height / (float)f->scale);   /* Ruffle: height / font scale in f32 */
    if (f->slant) gm.r1 = -0.21 * gm.sx;   /* synthetic italic */
    gm.tx = x; gm.ty = y;
    render_matrix_compose(&full, wm, &gm);
    {   /* Ruffle: a white glyph under a colour transform multiplying by the text colour (8.8 fixed) */
        SWFCxform tc, all; SWFColor white;
        swf_cxform_identity(&tc);
        tc.mr = (s16)((float)color.r / 255.0f * 256.0f); tc.mg = (s16)((float)color.g / 255.0f * 256.0f);
        tc.mb = (s16)((float)color.b / 255.0f * 256.0f); tc.ma = (s16)((float)color.a / 255.0f * 256.0f);
        swf_cxform_mul(&all, cx, &tc);
        white.r = white.g = white.b = white.a = 255;
        g->fills[0].color = white;
        render_shape(p->surf, p->movie, g, &full, &all, NULL);
    }
}

/* ---- DefineText ---- */

void text_render(Player *p, DisplayObject *d, const SWFMatrix *wm, const SWFCxform *cx) {
    Reader r; SWFRect bounds; SWFMatrix tm, full; int gbits, abits;
    if (p->debug > 1) fprintf(stderr, "[text] char %d type %d\n", d->char_id, d->ch->type);
    FontDef *font = NULL; SWFColor color; double x = 0, y = 0, height = 240;
    int is_text2;
    if (d->ch->type == CHAR_EDITTEXT) { extern void edittext_render(Player*, DisplayObject*, const SWFMatrix*, const SWFCxform*); edittext_render(p, d, wm, cx); return; }
    is_text2 = d->ch->tagcode == TAG_DefineText2;
    reader_init(&r, d->ch->data, d->ch->len);
    r_u16(&r);
    r_rect(&r, &bounds);
    r_matrix(&r, &tm);
    gbits = r_u8(&r); abits = r_u8(&r);
    {   /* static text whose stored bounds miss the stage is not drawn (only on screen) */
        extern Surface *g_stage_surface;
        if (p->surf == g_stage_surface) {
            double xs[4], ys[4], x0, y0, x1, y1; int k;
            swf_matrix_apply(wm, bounds.xmin, bounds.ymin, &xs[0], &ys[0]); swf_matrix_apply(wm, bounds.xmax, bounds.ymin, &xs[1], &ys[1]);
            swf_matrix_apply(wm, bounds.xmin, bounds.ymax, &xs[2], &ys[2]); swf_matrix_apply(wm, bounds.xmax, bounds.ymax, &xs[3], &ys[3]);
            x0 = x1 = xs[0]; y0 = y1 = ys[0];
            for (k = 1; k < 4; k++) { if (xs[k] < x0) x0 = xs[k]; if (xs[k] > x1) x1 = xs[k]; if (ys[k] < y0) y0 = ys[k]; if (ys[k] > y1) y1 = ys[k]; }
            if (x1 < 0 || y1 < 0 || x0 > p->surf->w || y0 > p->surf->h) return;
        }
    }
    render_matrix_compose(&full, wm, &tm);   /* Ruffle: the text transform pushed on the stack */
    color.r = color.g = color.b = 0; color.a = 255;
    for (;;) {
        int flags, n, i;
        reader_align(&r);
        flags = r_u8(&r);
        if (flags == 0 || reader_eof(&r)) break;
        if (flags & 0x08) { font = font_get(d->ch->movie ? d->ch->movie : PMOV(p), r_u16(&r)); }
        if (flags & 0x04) { if (is_text2) r_rgba(&r, &color); else r_rgb(&r, &color); }
        if (flags & 0x01) x = r_s16(&r);
        if (flags & 0x02) y = r_s16(&r);
        if (flags & 0x08) height = r_u16(&r);
        n = r_u8(&r);
        for (i = 0; i < n; i++) {
            int gi = (int)rb_u(&r, (unsigned)gbits);
            int adv = rb_s(&r, (unsigned)abits);
            if (font) draw_glyph(p, font, gi, &full, x, y, height, color, cx);
            x += adv;
        }
        reader_align(&r);
    }
}

/* static text hit test (Ruffle Text::hit_test_shape): the glyph outlines,
   or the bounding box for "advanced" (FlashType) text; (lx, ly) is in the
   text's local space, inside its bounds */
int text_hit_glyphs(SWFMovie *mv, SWFChar *ch, double lx, double ly) {
    extern int dg_shape_hit(ShapeDef *s, double x, double y, double min_stroke);
    Reader r; SWFRect bounds; SWFMatrix tm; double a, b, c, d, e, f, px, py, x = 0, y = 0, height = 240; int gbits, abits, is_text2; FontDef *font = NULL;
    if (!ch || ch->type != CHAR_TEXT) return 0;
    if (ch->csm && ((ch->csm[2] >> 6) & 3) == 1) return 1;            /* advanced anti-aliasing: bounds */
    is_text2 = ch->tagcode == TAG_DefineText2;
    reader_init(&r, ch->data, ch->len);
    r_u16(&r); r_rect(&r, &bounds); r_matrix(&r, &tm);
    if (!matrix_invert(&tm, &a, &b, &c, &d, &e, &f)) return 0;
    px = a * lx + b * ly + e; py = c * lx + d * ly + f;
    gbits = r_u8(&r); abits = r_u8(&r);
    for (;;) {
        int flags, n, i; SWFColor col;
        reader_align(&r);
        flags = r_u8(&r);
        if (flags == 0 || reader_eof(&r)) break;
        if (flags & 0x08) font = font_get(ch->movie ? ch->movie : mv, r_u16(&r));
        if (flags & 0x04) { if (is_text2) r_rgba(&r, &col); else r_rgb(&r, &col); }
        if (flags & 0x01) x = r_s16(&r);
        if (flags & 0x02) y = r_s16(&r);
        if (flags & 0x08) height = r_u16(&r);
        n = r_u8(&r);
        for (i = 0; i < n; i++) {
            int gi = (int)rb_u(&r, (unsigned)gbits), adv = rb_s(&r, (unsigned)abits);
            if (font && height > 0) {
                ShapeDef *g = font_glyph(font, gi); double sc = height / font->scale;
                if (g && g->nedges && dg_shape_hit(g, (px - x) / sc, (py - y) / sc, 0)) return 1;
            }
            x += adv;
        }
        reader_align(&r);
    }
    return 0;
}

/* ---- DefineEditText ---- */

/* DefineEditText flags as they land in a little-endian u16 (the spec lists
 * them in bit-stream order: first byte = HasText..HasFont, MSB first) */
#define ET_HASTEXT    0x0080
#define ET_WORDWRAP   0x0040
#define ET_MULTILINE  0x0020
#define ET_PASSWORD   0x0010
#define ET_READONLY   0x0008
#define ET_HASCOLOR   0x0004
#define ET_HASMAXLEN  0x0002
#define ET_HASFONT    0x0001
#define ET_HASFONTCLS 0x8000
#define ET_AUTOSIZE   0x4000
#define ET_HASLAYOUT  0x2000
#define ET_NOSELECT   0x1000
#define ET_BORDER     0x0800
#define ET_WASSTATIC  0x0400
#define ET_HTML       0x0200
#define ET_OUTLINES   0x0100

typedef struct {
    SWFRect bounds; u16 flags; int font_id, font_height; SWFColor color;
    int align, left, right, indent, leading; const char *var; const char *text;
} EditText;

static void parse_edittext(const SWFChar *ch, EditText *e) {
    Reader r;
    memset(e, 0, sizeof(*e));
    reader_init(&r, ch->data, ch->len);
    r_u16(&r);
    r_rect(&r, &e->bounds);
    e->flags = r_u16(&r);
    e->font_height = 240; e->color.a = 255;
    if (e->flags & ET_HASFONT) { e->font_id = r_u16(&r); e->font_height = r_u16(&r); }
    if (e->flags & ET_HASFONTCLS) r_string(&r);
    if (e->flags & ET_HASCOLOR) r_rgba(&r, &e->color);
    if (e->flags & ET_HASMAXLEN) r_u16(&r);
    if (e->flags & ET_HASLAYOUT) { e->align = r_u8(&r); e->left = r_u16(&r); e->right = r_u16(&r); e->indent = r_s16(&r); e->leading = r_s16(&r); }
    e->var = r_string(&r);
    e->text = (e->flags & ET_HASTEXT) ? r_string(&r) : "";
}

/* pick an embedded font with glyphs for text when the requested one is a
 * device font (no glyphs) */
FontDef *fallback_font(SWFMovie *m) {
    int i; FontDef *best = NULL;
    { FontDef *d = font_device(DEVFONT_SERIF, 0, 0); if (d) return d; }
    for (i = 0; i <= m->maxid; i++) {
        if (m->dict[i].type == CHAR_FONT) {
            FontDef *f = font_get(m, i);
            if (f && f->nglyphs > 0) { if (!best || f->nglyphs > best->nglyphs) best = f; }
        }
    }
    return best;
}

static void strip_html(const char *in, char *out, size_t cap) {
    size_t o = 0; int intag = 0;
    while (*in && o + 1 < cap) {
        if (*in == '<') { intag = 1; if (!strncmp(in, "<br", 3) || !strncmp(in, "<BR", 3) || !strncmp(in, "</p", 3) || !strncmp(in, "</P", 3)) out[o++] = '\n'; }
        else if (*in == '>') intag = 0;
        else if (!intag) {
            if (*in == '&') {
                if (!strncmp(in, "&amp;", 5)) { out[o++] = '&'; in += 4; }
                else if (!strncmp(in, "&lt;", 4)) { out[o++] = '<'; in += 3; }
                else if (!strncmp(in, "&gt;", 4)) { out[o++] = '>'; in += 3; }
                else if (!strncmp(in, "&nbsp;", 6)) { out[o++] = ' '; in += 5; }
                else if (!strncmp(in, "&quot;", 6)) { out[o++] = '"'; in += 5; }
                else if (!strncmp(in, "&apos;", 6)) { out[o++] = '\''; in += 5; }
                else out[o++] = '&';
            } else out[o++] = *in;
        }
        in++;
    }
    out[o] = 0;
}

/* decode one UTF-8 code point */
static int utf8_next(const char **s) {
    const u8 *p = (const u8*)*s; int c;
    if (p[0] < 0x80) { c = p[0]; *s += 1; }
    else if ((p[0] & 0xE0) == 0xC0 && p[1]) { c = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); *s += 2; }
    else if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) { c = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); *s += 3; }
    else { c = p[0]; *s += 1; }
    return c;
}

/* Ruffle's kerning_pairs map: the last record of a pair wins */
int font_kerning(const FontDef *f, int left, int right) {
    int i;
    if (!f || !f->nkern) return 0;
    for (i = f->nkern - 1; i >= 0; i--) if (f->kern_l[i] == (u16)left && f->kern_r[i] == (u16)right) return f->kern_adj[i];
    return 0;
}
double glyph_advance(FontDef *f, int gi, double height) {
    if (!f) return height * 0.6;
    /* a DefineFont3 advance of 32768 (1.6 EM) overflows the signed field: Flash reads it unsigned */
    if (f->advance && gi >= 0) return (f->version == 3 && f->advance[gi] == -32768 ? 32768.0 : (double)f->advance[gi]) * height / f->scale;
    {
        ShapeDef *g = font_glyph(f, gi);
        if (g && g->nedges) return (g->bounds.xmax > 0 ? g->bounds.xmax : height * 0.6 * f->scale / height) * height / f->scale + height * 0.1;
    }
    return height * 0.6;
}

static double text_width(FontDef *f, const char *s, size_t n, double height) {
    double w = 0; const char *e = s + n;
    while (s < e) { int c = utf8_next(&s); int gi = font_glyph_for_code(f, c); w += glyph_advance(f, gi, height); }
    return w;
}


/* Break text into lines the way Flash's TextField does: a line ends after a '\n' (or "\r\n"),
 * or, with word wrap, after the last space that fits (the space stays on the line) or at the
 * last character that fits when a word is wider than the field.  A trailing newline produces
 * an extra empty final line.  Returns the number of lines (>= 1); ends are exclusive byte offsets. */
typedef struct { int start, end; } TextLineSpan;
static int break_lines(FontDef *f, const char *buf, int wordwrap, double width, double height, TextLineSpan *out, int max) {
    const char *s = buf; int n = 0;
    for (;;) {
        const char *line_end = s, *last_space = NULL; double w = 0; int hard = 0;
        while (*line_end && *line_end != '\n' && *line_end != '\r') {
            const char *nx = line_end; int c = utf8_next(&nx);
            double cw = glyph_advance(f, font_glyph_for_code(f, c), height);
            if (wordwrap && w + cw > width && line_end > s) { if (last_space) line_end = last_space; break; }
            if (c == ' ') last_space = nx;
            w += cw; line_end = nx;
        }
        if (*line_end == '\r') { line_end++; hard = 1; }
        if (*line_end == '\n') { line_end++; hard = 1; }
        if (n < max) { out[n].start = (int)(s - buf); out[n].end = (int)(line_end - buf); }
        n++;
        s = line_end;
        if (!*s) { if (hard) { if (n < max) { out[n].start = out[n].end = (int)(s - buf); } n++; } break; }
    }
    return n;
}

static void layout_and_draw(Player *p, FontDef *f, const char *text, const EditText *e, const SWFMatrix *wm, SWFColor color, const SWFCxform *cx, double height) {
    char buf[4096]; const char *s; double y;
    double width = e->bounds.xmax - e->bounds.xmin - e->left - e->right - 80;
    double line_h = height * 1.2 + e->leading;
    if (f && f->has_layout && f->ascent) line_h = (f->ascent + f->descent) * height / f->scale + e->leading;
    if (e->flags & ET_HTML) strip_html(text, buf, sizeof(buf)); else { strncpy(buf, text, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0; }
    y = e->bounds.ymin + 40 + (f && f->has_layout && f->ascent ? f->ascent * height / f->scale : height * 0.9);
    {
        TextLineSpan spans[512]; int nl = break_lines(f, buf, (e->flags & ET_WORDWRAP) != 0, width, height, spans, 512), li;
        if (nl > 512) nl = 512;
        for (li = 0; li < nl; li++) {
            const char *ls = buf + spans[li].start, *le = buf + spans[li].end; size_t n; double lw, x;
            while (le > ls && (le[-1] == '\n' || le[-1] == '\r')) le--;
            n = (size_t)(le - ls); lw = text_width(f, ls, n, height);
            if (e->align == 1) x = e->bounds.xmax - e->right - 40 - lw;
            else if (e->align == 2) x = e->bounds.xmin + (e->bounds.xmax - e->bounds.xmin - lw) / 2;
            else x = e->bounds.xmin + e->left + e->indent + 40;
            s = ls;
            while (s < le) {
                int c = utf8_next(&s); int gi = font_glyph_for_code(f, c);
                if (gi >= 0 && c != ' ') draw_glyph(p, f, gi, wm, x, y, height, color, cx);
                x += glyph_advance(f, gi, height);
            }
            y += line_h;
            if (y > e->bounds.ymax + line_h && !(e->flags & ET_AUTOSIZE)) break;
        }
    }
}

/* the variable name of a DefineEditText (NULL: none) */
const char *edittext_variable(SWFChar *ch) { EditText e; parse_edittext(ch, &e); return e.var; }

static const char *clip_text_value(Player *p, DisplayObject *d, const EditText *e, char *tmp, size_t cap) {
    (void)p; (void)d; (void)tmp; (void)cap;
    return e->text;
}

void edittext_render(Player *p, DisplayObject *d, const SWFMatrix *wm, const SWFCxform *cx) {
    EditText e; FontDef *f; char tmp[4096]; const char *text; SWFColor color;
    if (d->clip && d->clip->obj2) { TextFieldState *t = a2_textfield_state(d); if (t) { tfs_render(p, t, wm, cx, p->focus == d->clip); return; } }
    { extern TextFieldState *a1_textfield_state(MovieClip *c); TextFieldState *t = d->clip ? a1_textfield_state(d->clip) : NULL; if (t) { tfs_render(p, t, wm, cx, p->focus == d->clip); return; } }
    parse_edittext(d->ch, &e);
    f = font_get(d->ch->movie ? d->ch->movie : PMOV(p), e.font_id);
    if (!f || f->nglyphs == 0) { FontDef *dv = font_device_by_name(f ? f->name : NULL, f ? f->bold : 0, f ? f->italic : 0); f = dv ? dv : fallback_font(PMOV(p)); }
    if (!f) return;
    text = clip_text_value(p, d, &e, tmp, sizeof(tmp));
    if (p->debug > 1) fprintf(stderr, "[edittext] char %d font %d(%d glyphs) var='%s' text='%s' h=%d bounds %d,%d-%d,%d\n", d->char_id, e.font_id, f->nglyphs, e.var ? e.var : "", text ? text : "", e.font_height, e.bounds.xmin, e.bounds.ymin, e.bounds.xmax, e.bounds.ymax);
    if (!text || !*text) return;
    color = e.color;
    if (e.flags & ET_BORDER) {                   /* border */
        ShapeDef s; ShapeEdge ed[4]; LineStyle ls; int i;
        memset(&s, 0, sizeof(s)); memset(&ls, 0, sizeof(ls)); ls.width = 20; ls.color.a = 255;
        s.lines = &ls; s.nlines = 1; s.edges = ed; s.nedges = 4;
        for (i = 0; i < 4; i++) { memset(&ed[i], 0, sizeof(ed[i])); ed[i].line = 1; }
        ed[0].x0 = e.bounds.xmin; ed[0].y0 = e.bounds.ymin; ed[0].x1 = e.bounds.xmax; ed[0].y1 = e.bounds.ymin;
        ed[1].x0 = e.bounds.xmax; ed[1].y0 = e.bounds.ymin; ed[1].x1 = e.bounds.xmax; ed[1].y1 = e.bounds.ymax;
        ed[2].x0 = e.bounds.xmax; ed[2].y0 = e.bounds.ymax; ed[2].x1 = e.bounds.xmin; ed[2].y1 = e.bounds.ymax;
        ed[3].x0 = e.bounds.xmin; ed[3].y0 = e.bounds.ymax; ed[3].x1 = e.bounds.xmin; ed[3].y1 = e.bounds.ymin;
        render_shape(p->surf, p->movie, &s, wm, cx, NULL);
    }
    layout_and_draw(p, f, text, &e, wm, color, cx, (double)e.font_height);
    if (p->focus && d->clip == p->focus && (p->caret_blink & 16)) {
        /* caret after the last character of the last line */
        ShapeDef s; ShapeEdge ed; LineStyle ls; double x, y, h = (double)e.font_height;
        const char *last = strrchr(text, '\n'); size_t n; int lines = 0; const char *q;
        for (q = text; *q; q++) if (*q == '\n') lines++;
        last = last ? last + 1 : text; n = strlen(last);
        x = e.bounds.xmin + e.left + 40 + text_width(f, last, n, h);
        if (e.align == 2) x = e.bounds.xmin + (e.bounds.xmax - e.bounds.xmin + text_width(f, last, n, h)) / 2;
        y = e.bounds.ymin + 40 + lines * (h * 1.2 + e.leading);
        memset(&s, 0, sizeof(s)); memset(&ls, 0, sizeof(ls)); memset(&ed, 0, sizeof(ed));
        ls.width = 20; ls.color = color;
        ed.x0 = (s32)x; ed.y0 = (s32)y; ed.x1 = (s32)x; ed.y1 = (s32)(y + h); ed.line = 1;
        s.lines = &ls; s.nlines = 1; s.edges = &ed; s.nedges = 1;
        render_shape(p->surf, p->movie, &s, wm, cx, NULL);
    }
}

/* createTextField() clips: text prop + default font */
void text_render_dynamic(Player *p, MovieClip *c, const SWFMatrix *wm, const SWFCxform *cx) {
    extern TextFieldState *a1_textfield_state(MovieClip *c);
    if (c->obj2 && c->self && !c->self->ch) { TextFieldState *t = a2_textfield_state(c->self); if (t) tfs_render(p, t, wm, cx, p->focus == c); return; }
    if (c->self && !c->self->ch) { TextFieldState *t = a1_textfield_state(c); if (t) tfs_render(p, t, wm, cx, p->focus == c); }
}

void font_free_parsed(void *p) {
    FontDef *f = (FontDef*)p; int i;
    if (!f) return;
    if (f->glyphs) for (i = 0; i < f->nglyphs; i++) if (f->glyphs[i]) shape_free(f->glyphs[i]);
    free(f->glyphs); free(f->glyph_data); free(f->glyph_len); free(f->codes); free(f->advance); free(f->kern_l); free(f->kern_r); free(f->kern_adj);
    free(f);
}

/* ---- editing support ---- */

int edittext_editable(SWFChar *ch) {
    EditText e;
    if (!ch || ch->type != CHAR_EDITTEXT) return 0;
    parse_edittext(ch, &e);
    return !(e.flags & ET_READONLY);
}

void edittext_get(Player *p, DisplayObject *d, char *buf, size_t cap) {
    EditText e; const char *t;
    parse_edittext(d->ch, &e);
    t = clip_text_value(p, d, &e, buf, cap);
    if (t != buf) { strncpy(buf, t, cap - 1); buf[cap - 1] = 0; }
}

void edittext_set(Player *p, DisplayObject *d, const char *text) {
    (void)p; (void)d; (void)text;
}


/* ---- AVM2 TextField rendering ---- */

static void info_to_edittext(const A2TextInfo *ti, EditText *e) {
    memset(e, 0, sizeof(*e));
    e->bounds.xmin = 0; e->bounds.ymin = 0; e->bounds.xmax = ti->w; e->bounds.ymax = ti->h;
    e->flags = (ti->wordwrap ? ET_WORDWRAP : 0) | (ti->multiline ? ET_MULTILINE : 0) | (ti->html ? ET_HTML : 0) | (ti->border ? ET_BORDER : 0) | (ti->autosize ? ET_AUTOSIZE : 0) | (ti->password ? ET_PASSWORD : 0);
    e->font_id = ti->font_id; e->font_height = ti->size > 0 ? ti->size : 240;
    e->color.r = (u8)(ti->color >> 16); e->color.g = (u8)(ti->color >> 8); e->color.b = (u8)ti->color; e->color.a = 255;
    e->align = ti->align; e->left = ti->left_margin; e->right = ti->right_margin; e->indent = ti->indent; e->leading = ti->leading;
    e->text = ti->text;
}
void edittext_defaults(SWFChar *ch, A2TextInfo *out) {
    EditText e; parse_edittext(ch, &e);
    memset(out, 0, sizeof(*out));
    out->text = e.text ? e.text : ""; out->w = e.bounds.xmax - e.bounds.xmin; out->h = e.bounds.ymax - e.bounds.ymin;
    out->html = !!(e.flags & ET_HTML); out->wordwrap = !!(e.flags & ET_WORDWRAP); out->multiline = !!(e.flags & ET_MULTILINE); out->border = !!(e.flags & ET_BORDER);
    out->autosize = !!(e.flags & ET_AUTOSIZE); out->password = !!(e.flags & ET_PASSWORD);
    out->color = ((u32)e.color.r << 16) | ((u32)e.color.g << 8) | e.color.b; out->size = e.font_height; out->align = e.align; out->font_id = (e.flags & ET_HASFONT) ? e.font_id : -1;
    out->leading = e.leading; out->indent = e.indent; out->left_margin = e.left; out->right_margin = e.right;
    out->bgcolor = 0xFFFFFF; out->bordercolor = 0;
}
static FontDef *font_for_info(Player *p, const A2TextInfo *ti) {
    FontDef *f = NULL; int i;
    if (ti->font_id >= 0) f = font_get(p->movie, ti->font_id);
    if ((!f || f->nglyphs == 0) && ti->font_name && ti->font_name[0]) {
        for (i = 0; i <= p->movie->maxid; i++) if (p->movie->dict[i].type == CHAR_FONT) { FontDef *g = font_get(p->movie, i); if (g && g->nglyphs && !strcmp(g->name, ti->font_name) && (!!g->bold == !!ti->bold) && (!!g->italic == !!ti->italic)) { f = g; break; } }
        if (!f) for (i = 0; i <= p->movie->maxid; i++) if (p->movie->dict[i].type == CHAR_FONT) { FontDef *g = font_get(p->movie, i); if (g && g->nglyphs && !strcmp(g->name, ti->font_name)) { f = g; break; } }
    }
    if (!f || f->nglyphs == 0) { FontDef *dv = font_device_by_name(f && f->name[0] ? f->name : ti->font_name, ti->bold, ti->italic); f = dv ? dv : fallback_font(p->movie); }
    return f;
}
static void rect_fill(Player *p, int x0, int y0, int x1, int y1, u32 rgb, int line, const SWFMatrix *wm, const SWFCxform *cx) {
    ShapeDef s; ShapeEdge ed[4]; FillStyle fs; LineStyle ls; int i;
    memset(&s, 0, sizeof(s)); memset(&fs, 0, sizeof(fs)); memset(&ls, 0, sizeof(ls)); swf_matrix_identity(&fs.matrix);
    fs.type = FILL_SOLID; fs.color.r = (u8)(rgb >> 16); fs.color.g = (u8)(rgb >> 8); fs.color.b = (u8)rgb; fs.color.a = 255;
    ls.width = 20; ls.color = fs.color;
    if (line) { s.lines = &ls; s.nlines = 1; } else { s.fills = &fs; s.nfills = 1; }
    s.edges = ed; s.nedges = 4;
    for (i = 0; i < 4; i++) { memset(&ed[i], 0, sizeof(ed[i])); if (line) ed[i].line = 1; else ed[i].fill1 = 1; }
    ed[0].x0 = x0; ed[0].y0 = y0; ed[0].x1 = x1; ed[0].y1 = y0;
    ed[1].x0 = x1; ed[1].y0 = y0; ed[1].x1 = x1; ed[1].y1 = y1;
    ed[2].x0 = x1; ed[2].y0 = y1; ed[2].x1 = x0; ed[2].y1 = y1;
    ed[3].x0 = x0; ed[3].y0 = y1; ed[3].x1 = x0; ed[3].y1 = y0;
    render_shape(p->surf, p->movie, &s, wm, cx, NULL);
}
void text_render_a2(Player *p, DisplayObject *d, const A2TextInfo *ti, const SWFMatrix *wm, const SWFCxform *cx) {
    EditText e; FontDef *f; SWFColor color; char pw[4096];
    info_to_edittext(ti, &e);
    if (ti->background) rect_fill(p, 0, 0, ti->w, ti->h, ti->bgcolor, 0, wm, cx);
    if (ti->border) rect_fill(p, 0, 0, ti->w, ti->h, ti->bordercolor, 1, wm, cx);
    if (!ti->text || !ti->text[0]) { if (p->focus && d->clip == p->focus && (p->caret_blink & 16)) rect_fill(p, 40, 40, 60, 40 + (ti->size > 0 ? ti->size : 240), ti->color, 1, wm, cx); return; }
    f = font_for_info(p, ti);
    if (!f) return;
    color = e.color;
    if (ti->password) { size_t i, n = strlen(ti->text); if (n > 4000) n = 4000; for (i = 0; i < n; i++) pw[i] = '*'; pw[n] = 0; e.text = pw; }
    layout_and_draw(p, f, e.text, &e, wm, color, cx, (double)e.font_height);
    if (p->focus && d->clip == p->focus && (p->caret_blink & 16)) {
        const char *last = strrchr(e.text, '\n'); size_t n; int lines = 0; const char *q; double x, y, h = (double)e.font_height;
        for (q = e.text; *q; q++) if (*q == '\n') lines++;
        last = last ? last + 1 : e.text; n = strlen(last);
        x = e.left + 40 + text_width(f, last, n, h); y = 40 + lines * (h * 1.2 + e.leading);
        rect_fill(p, (int)x, (int)y, (int)x + 20, (int)(y + h), ti->color, 1, wm, cx);
    }
}
void text_measure_a2(Player *p, const A2TextInfo *ti, double *w, double *h, int *lines) {
    EditText e; FontDef *f; char buf[4096]; const char *s; double height, line_h, width, maxw = 0; int nl = 0;
    info_to_edittext(ti, &e);
    f = font_for_info(p, ti);
    height = (double)e.font_height;
    line_h = height * 1.2 + e.leading;
    if (f && f->has_layout && f->ascent) line_h = (f->ascent + f->descent) * height / f->scale + e.leading;
    width = e.bounds.xmax - e.bounds.xmin - e.left - e.right - 80;
    if (!ti->text) { *w = 0; *h = 0; *lines = 1; return; }
    if (e.flags & ET_HTML) strip_html(ti->text, buf, sizeof(buf)); else { strncpy(buf, ti->text, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0; }
    {
        TextLineSpan spans[512]; int li; nl = break_lines(f, buf, (e.flags & ET_WORDWRAP) != 0, width, height, spans, 512);
        for (li = 0; li < nl && li < 512; li++) {
            const char *ls = buf + spans[li].start, *le = buf + spans[li].end; double lw;
            while (le > ls && (le[-1] == '\n' || le[-1] == '\r' || le[-1] == ' ')) le--;
            lw = text_width(f, ls, (size_t)(le - ls), height);
            if (lw > maxw) maxw = lw;
        }
    }
    (void)s;
    *w = maxw; *h = nl * line_h; *lines = nl;
}
/* Line spans (byte offsets into the plain text) as Flash reports them via getLineOffset/getLineLength. */
int text_lines_a2(Player *p, const A2TextInfo *ti, int *starts, int *ends, int max) {
    EditText e; FontDef *f; static char buf[4096]; double height, width; TextLineSpan spans[512]; int nl, i;
    info_to_edittext(ti, &e);
    f = font_for_info(p, ti);
    height = (double)e.font_height;
    width = e.bounds.xmax - e.bounds.xmin - e.left - e.right - 80;
    if (!ti->text) { if (max > 0) { starts[0] = ends[0] = 0; } return 1; }
    if (e.flags & ET_HTML) strip_html(ti->text, buf, sizeof(buf)); else { strncpy(buf, ti->text, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0; }
    nl = break_lines(f, buf, (e.flags & ET_WORDWRAP) != 0, width, height, spans, 512);
    if (getenv("FLASHNT_DEBUG")) fprintf(stderr, "[text] lines=%d wrap=%d width=%g height=%g font=%s\n", nl, (e.flags & ET_WORDWRAP) != 0, width, height, f ? f->name : "(none)");
    for (i = 0; i < nl && i < max && i < 512; i++) { starts[i] = spans[i].start; ends[i] = spans[i].end; }
    return nl;
}

/* DefineEditText -> TextField state (AVM2 timeline text fields) */
void edittext_init_state(Player *p, SWFChar *ch, TextFieldState *t) {
    EditText e; Reader r; u16 flags; int maxlen = 0; const char *fontcls = NULL;
    parse_edittext(ch, &e);
    t->movie = ch->movie;
    reader_init(&r, ch->data, ch->len); r_u16(&r); { SWFRect b; r_rect(&r, &b); } flags = r_u16(&r);
    if (flags & ET_HASFONT) { r_u16(&r); r_u16(&r); }
    if (flags & ET_HASFONTCLS) { fontcls = r_string(&r); r_u16(&r); }
    if (flags & ET_HASCOLOR) { SWFColor c; r_rgba(&r, &c); }
    if (flags & ET_HASMAXLEN) maxlen = r_u16(&r);
    t->xmin = e.bounds.xmin; t->ymin = e.bounds.ymin; t->w = e.bounds.xmax - e.bounds.xmin; t->h = e.bounds.ymax - e.bounds.ymin;
    t->wordwrap = (flags & ET_WORDWRAP) != 0; t->multiline = (flags & ET_MULTILINE) != 0; t->password = (flags & ET_PASSWORD) != 0;
    t->input = !(flags & ET_READONLY); t->selectable = !(flags & ET_NOSELECT); t->border = t->background = (flags & ET_BORDER) != 0;
    t->html = (flags & ET_HTML) != 0; t->embed = (flags & ET_OUTLINES) != 0; t->was_static = (flags & ET_WASSTATIC) != 0; t->autosize = (flags & ET_AUTOSIZE) ? 1 : 0; t->max_chars = maxlen;
    rt_fmt_default(&t->deffmt);
    if (flags & ET_HASFONT) {
        FontDef *f = font_get(ch->movie ? ch->movie : PMOV(p), e.font_id);
        if (f && f->name[0]) strncpy(t->deffmt.font, f->name, sizeof(t->deffmt.font) - 1);
        if (f && !t->html) { t->deffmt.bold = (u8)f->bold; t->deffmt.italic = (u8)f->italic; }   /* HTML fields take the style from the markup */
    } else if (fontcls) { strncpy(t->deffmt.font, fontcls, sizeof(t->deffmt.font) - 1); }
    t->deffmt.size = e.font_height / 20.0;
    t->deffmt.color = ((u32)e.color.r << 16) | ((u32)e.color.g << 8) | e.color.b;
    t->deffmt.align = (u8)(e.align == 1 ? RT_ALIGN_RIGHT : e.align == 2 ? RT_ALIGN_CENTER : e.align == 3 ? RT_ALIGN_JUSTIFY : RT_ALIGN_LEFT);
    t->deffmt.left_margin = e.left / 20.0; t->deffmt.right_margin = e.right / 20.0; t->deffmt.leading = e.leading / 20.0;
    { double ind = e.indent / 20.0, fl = floor(ind), fr = ind - fl;   /* rounded half to even */
      t->deffmt.indent = fr > 0.5 ? fl + 1 : fr < 0.5 ? fl : (fmod(fl, 2.0) == 0 ? fl : fl + 1); }
    /* an HTML field's align is left, unless it has initial text in SWF 8+ */
    { int ver = ch->movie ? ch->movie->version : p->movie->version; if (t->html && (ver < 8 || !(e.flags & ET_HASTEXT))) t->deffmt.align = RT_ALIGN_LEFT; }
    if (t->html) {
        RTHtmlOpts o; RTFmt d = t->deffmt;
        o.multiline = t->multiline; o.condense_white = 0; o.swf_version = ch->movie ? ch->movie->version : p->movie->version; o.css = NULL; o.css_ctx = NULL;
        if (o.swf_version < 8) d.align = RT_ALIGN_LEFT;
        rt_from_html(&t->rt, e.text ? e.text : "", &d, &o);
    } else {
        u32 *u; int n = rt_utf8_decode(e.text ? e.text : "", e.text ? (int)strlen(e.text) : 0, &u), i, k = 0;
        for (i = 0; i < n; i++) { if (u[i] == '\r' && i + 1 < n && u[i + 1] == '\n') { u[k++] = '\r'; i++; } else if (u[i] == '\n') u[k++] = '\r'; else u[k++] = u[i]; }
        rt_set_plain(&t->rt, u, k, &t->deffmt); free(u);
    }
    tfs_invalidate(t);
}

/* ---- AVM2 helpers ---- */
/* fonts that script Font objects refer to, and the registered (global) ones */
int font_ref(Player *p, SWFMovie *m, int id) {
    int i; SWFMovie **nm; int *ni;
    for (i = 0; i < p->nfref; i++) if (p->fref_m[i] == m && p->fref_id[i] == id) return i;
    nm = (SWFMovie**)realloc(p->fref_m, (size_t)(p->nfref + 1) * sizeof(SWFMovie*)); if (!nm) return -1; p->fref_m = nm;
    ni = (int*)realloc(p->fref_id, (size_t)(p->nfref + 1) * sizeof(int)); if (!ni) return -1; p->fref_id = ni;
    p->fref_m[p->nfref] = m; p->fref_id[p->nfref] = id;
    return p->nfref++;
}
FontDef *font_of_ref(Player *p, int ref) { return ref >= 0 && ref < p->nfref ? font_get(p->fref_m[ref], p->fref_id[ref]) : NULL; }
static int name_ieq(const char *a, const char *b) { while (*a && *b) { char x = *a, y = *b; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) return 0; a++; b++; } return *a == *b; }
/* Font.registerFont: one font per name and style (a later one replaces it) */
void font_register_global(Player *p, int ref) {
    FontDef *f = font_of_ref(p, ref), *g; int i, *ng;
    if (!f) return;
    for (i = 0; i < p->ngfont; i++) { g = font_of_ref(p, p->gfont[i]); if (g && !strcmp(g->name, f->name) && !!g->bold == !!f->bold && !!g->italic == !!f->italic) { p->gfont[i] = ref; return; } }
    ng = (int*)realloc(p->gfont, (size_t)(p->ngfont + 1) * sizeof(int)); if (!ng) return;
    p->gfont = ng; p->gfont[p->ngfont++] = ref;
}
/* a registered font by name (Ruffle FontSet::find: the exact style, then the nearest) */
FontDef *font_global_find(Player *p, const char *name, int bold, int italic) {
    static const int pref[4][4] = { { 0, 1, 2, 3 }, { 1, 3, 0, 2 }, { 2, 3, 0, 1 }, { 3, 0, 1, 2 } };   /* style = bold | italic << 1 */
    int k, i, want = (bold ? 1 : 0) | (italic ? 2 : 0);
    if (!p->ngfont) return NULL;
    for (k = 0; k < 4; k++) for (i = 0; i < p->ngfont; i++) {
        FontDef *g = font_of_ref(p, p->gfont[i]);
        if (g && name_ieq(g->name, name) && ((g->bold ? 1 : 0) | (g->italic ? 2 : 0)) == pref[want][k]) return g;
    }
    return NULL;
}
int font_char_name(Player *p, int id, char *buf, size_t cap, int *bold, int *italic) {
    FontDef *f = font_get(p->movie, id);
    if (!f) return 0;
    strncpy(buf, f->name[0] ? f->name : "Font", cap - 1); buf[cap - 1] = 0; *bold = f->bold; *italic = f->italic;
    return 1;
}
/* text of a DefineText character via glyph codes (StaticText.text) */
int text_static_string(Player *p, SWFChar *ch, char *buf, size_t cap) {
    Reader r; SWFRect bounds; SWFMatrix tm; int gbits, abits; FontDef *font = NULL; size_t o = 0;
    if (!ch || ch->type != CHAR_TEXT) return 0;
    reader_init(&r, ch->data, ch->len);
    r_u16(&r); r_rect(&r, &bounds); r_matrix(&r, &tm);
    gbits = r_u8(&r); abits = r_u8(&r);
    for (;;) {
        int flags, n, i;
        reader_align(&r);
        flags = r_u8(&r);
        if (flags == 0 || reader_eof(&r)) break;
        if (flags & 0x08) font = font_get(ch->movie ? ch->movie : PMOV(p), r_u16(&r));
        if (flags & 0x04) { if (ch->tagcode == TAG_DefineText2) r_u32(&r); else { r_u8(&r); r_u8(&r); r_u8(&r); } }
        if (flags & 0x01) r_s16(&r);
        if (flags & 0x02) r_s16(&r);
        if (flags & 0x08) r_u16(&r);
        n = r_u8(&r);
        if (!font) return 0;   /* a run without a font: no text (Ruffle Text::text) */
        for (i = 0; i < n; i++) {
            int gi = (int)rb_u(&r, (unsigned)gbits); int code;
            rb_s(&r, (unsigned)abits);
            if (!font->codes || gi < 0 || gi >= font->nglyphs) continue;   /* no such glyph */
            code = font->codes[gi];
            if (o + 4 < cap) { char t[4]; int k; if (code < 0x80) { t[0] = (char)code; k = 1; } else if (code < 0x800) { t[0] = (char)(0xC0 | (code >> 6)); t[1] = (char)(0x80 | (code & 63)); k = 2; } else { t[0] = (char)(0xE0 | (code >> 12)); t[1] = (char)(0x80 | ((code >> 6) & 63)); t[2] = (char)(0x80 | (code & 63)); k = 3; } memcpy(buf + o, t, (size_t)k); o += (size_t)k; }
        }
        reader_align(&r);
    }
    buf[o] = 0;
    return o > 0;   /* empty: null */
}

/* the characters of a DefineText (TextSnapshot): -1 when a glyph run has
 * no font, otherwise the number of UTF-16 units written to out */
int text_static_chars(SWFChar *ch, SWFMovie *m, u16 *out, int cap) {
    Reader r; SWFRect bounds; SWFMatrix tm; int gbits, abits, n = 0, is_text2; FontDef *font = NULL; int have_font = 0;
    if (!ch || (ch->type != CHAR_TEXT)) return -1;
    is_text2 = ch->tagcode == TAG_DefineText2;
    reader_init(&r, ch->data, ch->len);
    r_u16(&r); r_rect(&r, &bounds); r_matrix(&r, &tm);
    gbits = r_u8(&r); abits = r_u8(&r);
    for (;;) {
        int flags, cnt, i; SWFColor c;
        reader_align(&r);
        flags = r_u8(&r);
        if (flags == 0 || reader_eof(&r)) break;
        if (flags & 0x08) { font = font_get(ch->movie ? ch->movie : m, r_u16(&r)); have_font = 1; }
        if (flags & 0x04) { if (is_text2) r_rgba(&r, &c); else r_rgb(&r, &c); }
        if (flags & 0x01) r_s16(&r);
        if (flags & 0x02) r_s16(&r);
        if (flags & 0x08) r_u16(&r);
        if (!have_font || !font) return -1;
        cnt = r_u8(&r);
        for (i = 0; i < cnt; i++) {
            int gi = (int)rb_u(&r, (unsigned)gbits);
            rb_s(&r, (unsigned)abits);
            if (gi >= 0 && gi < font->nglyphs && font->codes) { if (n < cap) out[n] = font->codes[gi]; n++; }
        }
        reader_align(&r);
    }
    return n ? n : -1;
}
