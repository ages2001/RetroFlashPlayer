/* textfield.c - TextField layout and drawing (rich text, word wrap, alignment,
 * margins, bullets, tab stops, scrolling, selection and caret).
 *
 * Units are twips.  The text area sits 2px (40 twips) inside the field bounds
 * on each side (Flash's gutter); line boxes stack from the top with
 * ascent + descent + leading. */
#include "textfield.h"
#include "font.h"
#include "render.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#define GUTTER 40.0

void tfs_init(TextFieldState *t, int swf_version) {
    memset(t, 0, sizeof(*t));
    rt_init(&t->rt);
    rt_fmt_default(&t->deffmt);
    rt_set_plain(&t->rt, NULL, 0, &t->deffmt);
    t->w = t->h = 100 * 20; t->selectable = 1; t->mouse_wheel = 1; t->scroll_v = 1; t->bgcolor = 0xFFFFFF; t->bordercolor = 0; t->swf_version = swf_version;
    t->grid_fit = 1;
}
/* render settings from a CSMTextSettings tag body */
void tfs_csm(TextFieldState *t, const u8 *b) {
    union { u32 u; float f; } th, sh;
    if (!b) return;
    t->aa_advanced = (b[2] >> 6) == 1;
    t->grid_fit = (b[2] >> 3) & 7;
    th.u = (u32)b[3] | ((u32)b[4] << 8) | ((u32)b[5] << 16) | ((u32)b[6] << 24);
    sh.u = (u32)b[7] | ((u32)b[8] << 8) | ((u32)b[9] << 16) | ((u32)b[10] << 24);
    t->thickness = th.f; t->sharpness = sh.f;
}
static void lay_free(TFLayout *l) { free(l->lines); free(l->runs); free(l->cx); free(l->cw); free(l->cline); free(l->cmiss); memset(l, 0, sizeof(*l)); }
void tfs_free(TextFieldState *t) { rt_free(&t->rt); lay_free(&t->lay); free(t->restrict_chars); free(t->html_source); free(t->ime_text); memset(t, 0, sizeof(*t)); }
void tfs_invalidate(TextFieldState *t) { t->lay_valid = 0; t->as_pending = 1; }

/* ---- fonts ---- */
extern int g_devfont_registry;   /* the image harness's Ruffle font registry (FLASHNT_RUFFLE_FONTS) */
static int strncasecmp_c89(const char *a, const char *b, int n) { int i; for (i = 0; i < n; i++) { char x = a[i], y = b[i]; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) return 1; } return 0; }
static int name_eq_ci(const char *a, const char *b) { while (*a && *b) { char x = *a, y = *b; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) return 0; a++; b++; } return *a == *b; }
FontDef *tf_font_for(Player *p, const RTFmt *f, int embed) { return tf_font_for_movie(p, NULL, f, embed); }
FontDef *tf_font_for_movie(Player *p, SWFMovie *mv, const RTFmt *f, int embed) {
    if (embed) {
        SWFMovie *m = mv ? mv : p->movie; int i, pass; char name[256]; const char *n = f->font; int k = 0;
        /* the name as it is (a style sheet's fontFamily arrives trimmed) */
        while (n[k] && n[k] != ',' && k < 255) { name[k] = n[k]; k++; }
        name[k] = 0;
        { extern FontDef *font_global_find(Player *p, const char *name, int bold, int italic); FontDef *g = font_global_find(p, name, f->bold, f->italic); if (g && g->nglyphs) return g; }   /* Font.registerFont fonts come first */
        if (m->nfnames && !g_devfont_registry) {   /* (the image harness resolves fonts as Ruffle, which has no export-name lookup) */
            /* Flash's order: an export name (the last one, a non-font export finds nothing),
             * then the names fonts registered (the first one per name and style) with
             * Flash's style fallbacks */
            int nl = (int)strlen(name), e = -1, q; const struct SWFFontName *fn = m->fnames;
            static const int seq_one[3][2] = { { 1, 1 }, { 0, 0 }, { -1, -1 } };
            int tries[6][2], nt = 0, b = !!f->bold, it = !!f->italic;
            for (i = 0; i < m->nfnames; i++) if (fn[i].is_export && fn[i].nlen == nl && !strncasecmp_c89(fn[i].name, name, nl)) e = i;
            if (e >= 0) { int id = fn[e].id; FontDef *g = m->dict[id].type == CHAR_FONT ? font_get(m, id) : NULL; return g && g->nglyphs ? g : NULL; }
            tries[nt][0] = b; tries[nt][1] = it; nt++;
            if (b ^ it) { for (q = 0; q < 3; q++) { tries[nt][0] = seq_one[q][0] < 0 ? !b : seq_one[q][0]; tries[nt][1] = seq_one[q][1] < 0 ? !it : seq_one[q][1]; nt++; } }
            else {
                if (b && it) { tries[nt][0] = 0; tries[nt][1] = 0; nt++; }
                tries[nt][0] = 1; tries[nt][1] = 0; nt++;
                tries[nt][0] = 0; tries[nt][1] = 1; nt++;
                if (!b && !it) { tries[nt][0] = 1; tries[nt][1] = 1; nt++; }
            }
            for (q = 0; q < nt; q++)
                for (i = 0; i < m->nfnames; i++)
                    if (!fn[i].is_export && fn[i].nlen == nl && fn[i].bold == tries[q][0] && fn[i].italic == tries[q][1] && !strncasecmp_c89(fn[i].name, name, nl)) {
                        FontDef *g = font_get(m, fn[i].id);
                        return g && g->nglyphs ? g : NULL;
                    }
            return NULL;
        }
        if (!g_devfont_registry) {
            /* exact style first, then any style of that name */
            for (pass = 0; pass < 2; pass++)
                for (i = 0; i <= m->maxid; i++) if (m->dict[i].type == CHAR_FONT) {
                    FontDef *g = font_get(m, i);
                    if (g && g->nglyphs && name_eq_ci(g->name, name) && (pass || ((!!g->bold == !!f->bold) && (!!g->italic == !!f->italic)))) return g;
                }
            return NULL;   /* missing embedded font: nothing is drawn */
        }
        /* Ruffle FontMap::find: the exact style, then its fallback order */
        {
            int b = !!f->bold, it = !!f->italic, order[5][2], no = 0; FontDef *g;
            order[no][0] = b; order[no][1] = it; no++;
            if (b ^ it) { order[no][0] = 1; order[no][1] = 1; no++; order[no][0] = 0; order[no][1] = 0; no++; order[no][0] = !b; order[no][1] = !it; no++; }
            else {
                if (b && it) { order[no][0] = 0; order[no][1] = 0; no++; }
                order[no][0] = 1; order[no][1] = 0; no++; order[no][0] = 0; order[no][1] = 1; no++;
                if (!b && !it) { order[no][0] = 1; order[no][1] = 1; no++; }
            }
            for (pass = 0; pass < no; pass++)
                for (i = 0; i <= m->maxid; i++) if (m->dict[i].type == CHAR_FONT) {
                    g = font_get(m, i);
                    if (g && g->nglyphs && name_eq_ci(g->name, name) && !!g->bold == order[pass][0] && !!g->italic == order[pass][1]) return g;
                }
        }
        /* no embedded font of that name falls back to the device font (Ruffle
           resolve_font), which the harness has only when fonts were registered;
           another named font that is missing draws nothing (Flash) */
        if (!g_devfont_registry || (name[0] && !name_eq_ci(name, "Times New Roman"))) return NULL;
    }
    return font_device_by_name(f->font, f->bold, f->italic);
}
/* embedded font metrics are truncated to whole twips, device ones rounded */
/* (harness TrueType device faces, which have a name: whole pixels, as Ruffle) */
#define F_NAMED(f) ((f)->dev && ((const DevFace*)(f)->dev)->name[0])
/* Linux player device metrics (harness option): whole twips */
static double lin_metric(double v) { return floor(v + 1e-9); }
static double f_ascent(FontDef *f, double size) { if (!f) return 0;   /* a missing embedded font lays out nothing */ if (f->dev && devfont_linux_rules() && f->has_layout && (f->ascent || f->descent)) return lin_metric(f->ascent * size / f->scale); if (f->dev && f->scale != 1024.0) return (double)(long)((float)f->ascent * ((float)size / (float)f->scale)); if (f->has_layout && (f->ascent || f->descent)) return f->dev && !g_devfont_registry ? 20 * floor(f->ascent * size / f->scale / 20 + 0.5) : f->dev ? floor(f->ascent * size / f->scale + 0.5 - 1e-9) : floor(f->ascent * size / f->scale + 1e-9); return floor(size * 0.9 + 0.5); }
static double f_descent(FontDef *f, double size) { if (!f) return 0; if (f->dev && devfont_linux_rules() && f->has_layout && (f->ascent || f->descent)) return lin_metric(f->descent * size / f->scale); if (f->dev && f->scale != 1024.0) return (double)(long)((float)f->descent * ((float)size / (float)f->scale)); if (f->has_layout && (f->ascent || f->descent)) return f->dev && !g_devfont_registry ? 20 * floor(f->descent * size / f->scale / 20 + 0.5) : f->dev ? floor(f->descent * size / f->scale + 0.5 - 1e-9) : floor(f->descent * size / f->scale + 1e-9); return floor(size * 0.22 + 0.5); }

/* ---- layout ---- */
static void ensure_cap(TFLayout *l, int len) {
    l->cx = (double*)realloc(l->cx, ((size_t)len + 1) * sizeof(double));
    l->cw = (double*)realloc(l->cw, ((size_t)len + 1) * sizeof(double));
    l->cline = (int*)realloc(l->cline, ((size_t)len + 1) * sizeof(int));
    l->cmiss = (unsigned char*)realloc(l->cmiss, (size_t)len + 1); memset(l->cmiss, 0, (size_t)len + 1);
}
static TFLine *new_line(TFLayout *l) { if (l->nlines == l->caplines) { l->caplines = l->caplines * 2 + 8; l->lines = (TFLine*)realloc(l->lines, (size_t)l->caplines * sizeof(TFLine)); } memset(&l->lines[l->nlines], 0, sizeof(TFLine)); return &l->lines[l->nlines++]; }
static TFRun *new_run(TFLayout *l) { if (l->nruns == l->capruns) { l->capruns = l->capruns * 2 + 16; l->runs = (TFRun*)realloc(l->runs, (size_t)l->capruns * sizeof(TFRun)); } memset(&l->runs[l->nruns], 0, sizeof(TFRun)); return &l->runs[l->nruns++]; }

typedef struct { const RTFmt *f; FontDef *font; double size, ls, sx, sy; } CFmt;

static void cfmt_at(Player *p, TextFieldState *t, int i, CFmt *c, const RTFmt **lastf) {
    const RTFmt *f = rt_format_at(&t->rt, i);
    if (!f) f = &t->deffmt;
    if (f != *lastf) { c->f = f; c->font = tf_font_for_movie(p, t->movie, f, t->embed); c->size = floor(f->size * 20 + 0.5); c->ls = f->letter_spacing * 20; *lastf = f; }
    c->sx = t->dev_sx > 0 ? t->dev_sx : 1; c->sy = t->dev_sy > 0 ? t->dev_sy : 1;
}

static double char_adv(CFmt *c, u32 ch, u32 next, int password, unsigned char *miss) {
    int gi; double a;
    if (password) ch = '*';
    if (!c->font) { *miss = 1; return 0; }
    gi = font_glyph_for_code(c->font, (int)ch);
    if (gi < 0 && ch == 0xA0) gi = font_glyph_for_code(c->font, ' ');
    if (gi < 0 && (!c->font->dev || ch != ' ')) { *miss = 1; return 0; }   /* no glyph: no width, no bounds */
    if (c->font->dev && (c->sx != 1 || c->sy != 1) && !g_devfont_registry) {   /* scaled device text: pixel advances at the screen size, back in field units */
        CFmt sc = *c; double r; sc.size = c->size * c->sy; sc.ls = c->ls * c->sx; sc.sx = sc.sy = 1;
        r = char_adv(&sc, ch, next, 0, miss);
        return r / c->sx;
    }
    a = gi >= 0 ? glyph_advance(c->font, gi, c->size) : c->size * 0.25;
    if (!c->font->dev) {
        /* embedded: (advance + kerning) * (size / scale) in f32, truncated (Ruffle FontLike::evaluate) */
        if (gi >= 0 && c->font->advance) {
            int units = c->font->advance[gi];
            if (next && c->f->kerning && c->font->nkern) units += font_kerning(c->font, (int)ch, (int)next);
            a = (double)(long)((float)units * ((float)c->size / (float)c->font->scale));
        } else a = floor(a + 1e-9);
        return a + c->ls;
    }
    if (devfont_linux_rules()) {
        /* the font's 'hdmx' width at this pixel size, else the advance in whole
           quarter pixels rounded up to pixels */
        int hw = (fmod(c->size, 20.0) == 0 && gi >= 0) ? devfont_hdmx_advance(c->font->dev, gi, (int)(c->size / 20)) : -1;
        a = hw >= 0 ? hw * 20.0 : ceil(floor(a / 5.0 + 0.5) / 4.0 - 1e-9) * 20.0;
        return a + floor(c->ls / 20.0 + 0.5) * 20.0;
    }
    /* device text: kerned advance snapped to whole pixels; letter spacing is
       rounded to pixels too and only applies while the advance stays positive */
    if (c->font->scale != 1024.0) {
        /* a font in its own units (Ruffle's): (advance + kerning) * (size / EM) in f32, truncated */
        float sc = (float)c->size / (float)c->font->scale; int units = gi >= 0 && c->font->advance ? c->font->advance[gi] : 0;
        if (next) units += devfont_kerning(c->font->dev, (int)ch, (int)next);
        a = gi >= 0 && c->font->advance ? (double)(long)((float)units * sc) : a;
    } else if (next) a += devfont_kerning(c->font->dev, (int)ch, (int)next) * c->size / 1024.0;
    {
        double un, px, lsp, sp;
        un = (double)(long)a;                         /* truncated to twips */
        px = un / 20.0; px = px < 0 ? -floor(-px + 0.5) : floor(px + 0.5); un = px * 20.0;
        lsp = c->ls / 20.0;
        { double fl = floor(lsp), d = lsp - fl; lsp = d > 0.5 ? fl + 1 : d < 0.5 ? fl : (fmod(fl, 2.0) == 0 ? fl : fl + 1); }
        sp = un + lsp * 20.0;
        return sp > 0 ? sp : un;
    }
}

/* x offset of a line's content from the left of the text area */
static double left_offset(const RTFmt *f, int first) {
    double x = (f->left_margin + f->block_indent + (first ? f->indent : 0)) * 20;
    if (f->bullet) x += 36 * 20;
    return x < 0 ? 0 : x;
}

static void fmt_metrics2(Player *p, TextFieldState *t, const RTFmt *ff, const RTFmt *fs, double *a, double *d) {
    FontDef *font = tf_font_for_movie(p, t->movie, ff, t->embed); double size = floor(fs->size * 20 + 0.5);
    *a = f_ascent(font, size); *d = f_descent(font, size);
}
static void fmt_metrics(Player *p, TextFieldState *t, const RTFmt *f, double *a, double *d) { fmt_metrics2(p, t, f, f, a, d); }

/* may a line wrap between prev and curr? (Ruffle html/line_wrapping.rs find_allowed_breaks:
   after spaces and '-', and around CJK-like characters unless an opening/closing one binds) */
static int wrap_opening(u32 c) {
    static const u32 o[] = { '(', '[', '{', 0xFF08, 0xFF3B, 0xFF5B, 0x3008, 0x300A, 0x300C, 0x2045, 0x300E, 0x3010, 0x3016, 0x301A, 0xFD3E, 0xFE59, 0x301D, 0xFE3B, 0xFE41, 0xFE43, 0xFE35, 0 };
    int i; for (i = 0; o[i]; i++) if (o[i] == c) return 1; return 0;
}
static int wrap_closing(u32 c) {
    static const u32 o[] = { ')', ']', '}', 0xFF09, 0xFF3D, 0xFF5D, 0x3009, 0x300B, '?', '!', ';', ':', ',', '.', 0x300D, 0x2046, 0x300F, 0x3011, 0x3015, 0x3019, 0xFE5E, 0xFD3F, 0xFE5A,
        0xFF1F, 0xFF01, 0xFF1B, 0xFF1A, 0xFF0C, 0xFF0E, 0x3001, 0x3002, 0x301C, 0xFE3A, 0xFE40, 0xFE42, 0xFE44, 0xFE36, 0x301F, 0 };
    int i; for (i = 0; o[i]; i++) if (o[i] == c) return 1; return 0;
}
static int wrap_allowed(u32 prev, u32 curr, int swf8) {
    if (swf8 && curr == ' ') return 0;   /* only the last space of a run breaks */
    if (prev == ' ') return 1;
    if (prev == '-' || (!swf8 && curr == '-')) return 1;
    if (prev >= 0x2300 || curr >= 0x2300) return !wrap_opening(prev) && !wrap_closing(curr);
    return 0;
}
/* one layout pass; forced >= 0: the width non-wrapping autosized text aligns in */
static void lay_pass(Player *p, TextFieldState *t, double forced) {
    TFLayout *l = &t->lay; RichText *rt = &t->rt; int i = 0, n = rt->len, pw = t->password;
    double maxw = t->wordwrap ? t->w - 2 * GUTTER : 1e18, y = 0;
    const RTFmt *lastf = NULL; CFmt c; int prev_wrap_soft = 0;
    const RTFmt *cls = NULL;   /* LayoutBuilder::current_line_span: the span that last began at a line start */
    double nl_lead = 0;        /* leading of the span the last line break happened in */
    const RTFmt *nl_font = NULL;  /* that span: its font sizes the next line with current_line_span's size */
    l->nlines = 0; l->nruns = 0; l->len = n; l->text_w = 0; l->text_h = 0; l->text_right = 0;
    ensure_cap(l, n);
    memset(&c, 0, sizeof(c));
    if (maxw < 0) maxw = 0;
    for (;;) {
        /* one line from i */
        TFLine *ln; int start = i, end, brk = -1, k; double x, lx, rightm; const RTFmt *pf; int first;
        cfmt_at(p, t, i < n ? i : (n ? n - 1 : 0), &c, &lastf); pf = c.f;
        first = !prev_wrap_soft;
        lx = left_offset(pf, first);
        rightm = pf->right_margin * 20;
        x = lx;
        for (k = i; k < n; k++) {
            u32 ch = rt->text[k]; double a;
            if (ch == '\r' || ch == '\n') break;
            cfmt_at(p, t, k, &c, &lastf);
            if (ch == '\t' && !pw) {
                double nx = x;
                if (c.f->ntabs) { int ti; for (ti = 0; ti < c.f->ntabs; ti++) if (c.f->tabs[ti] * 20 > x - lx + 0.5) { nx = lx + c.f->tabs[ti] * 20; break; } }
                else {
                    /* default stops: embedded fonts about twice the line height (ascent + descent,
                       fitted to Flash); device fonts every size * 2.7 px (Ruffle LayoutBuilder::tab) */
                    double mod = (c.font && !c.font->dev && c.font->has_layout && (c.font->ascent || c.font->descent))
                        ? floor((f_ascent(c.font, c.size) + f_descent(c.font, c.size)) * 1.9832 + 0.5) : (double)(s32)(c.f->size * 2.7 * 20.0);
                    if (mod > 0) nx = lx + (floor(floor(x - lx) / mod) + 1) * mod; }
                a = nx - x;
            } else a = char_adv(&c, ch, k + 1 < n ? rt->text[k + 1] : 0, pw, &l->cmiss[k]);
            /* Flash (SWF 8 and older): a line whose indent leaves no room at all is not wrapped (it all goes off the field) */
            if (t->wordwrap && k > i && x + a > maxw - rightm + 0.5 && ch != ' ' && (lx <= 0 || lx < maxw - rightm || t->swf_version >= 9)) {
                /* wrap: after the last space on the line, else before this character */
                int q; brk = -1;
                for (q = k; q > i; q--) if (wrap_allowed(rt->text[q - 1], rt->text[q], t->swf_version >= 8)) { brk = q; break; }
                if (brk <= i) brk = k;
                break;
            }
            l->cx[k] = x; l->cw[k] = a; x += a;
        }
        if (brk > 0) { end = brk; prev_wrap_soft = 1; }
        else { end = k; if (end < n) { l->cx[end] = x; l->cw[end] = 0; end++; } prev_wrap_soft = 0; }
        ln = new_line(l);
        ln->start = start; ln->end = end; ln->para_start = first;
        /* metrics, runs and width (trailing spaces/newline excluded from the width) */
        { int q, vis_end = end; double asc = 0, desc = 0, lead, bas = 0, bds = 0, adv_lead; const RTFmt *rf = NULL; TFRun *run = NULL; int nbox = 0;
          while (vis_end > start && (rt->text[vis_end - 1] == '\r' || rt->text[vis_end - 1] == '\n')) vis_end--;
          /* line metrics (LayoutBuilder::newline / newspan): a span starting at the line start
             resets them, a span continuing from the previous line keeps current_line_span's;
             spans starting inside the line (its line break included) raise them */
          {
              const RTFmt *f0 = start < n ? rt_format_at(rt, start) : NULL;
              int begins = f0 && (start == 0 || rt_format_at(rt, start - 1) != f0);
              if (!n) { cls = &t->deffmt; fmt_metrics(p, t, cls, &asc, &desc); lead = cls->leading * 20; }
              else if (begins || !cls) { if (!f0) f0 = rt_format_at(rt, n - 1); cls = f0; fmt_metrics(p, t, cls, &asc, &desc); lead = cls->leading * 20; }
              else { fmt_metrics2(p, t, nl_font ? nl_font : cls, cls, &asc, &desc); lead = nl_lead; }
              for (q = start + 1; q < end; q++) {
                  const RTFmt *fq = rt_format_at(rt, q);
                  if (fq != rt_format_at(rt, q - 1)) { double a2, d2; fmt_metrics(p, t, fq, &a2, &d2); if (a2 > asc) asc = a2; if (d2 > desc) desc = d2; if (g_devfont_registry && fq->leading * 20 > lead) lead = fq->leading * 20; }   /* leading: the line start's (Flash; not the largest; Ruffle's harness: the largest) */
              }
              adv_lead = cls->leading * 20;
          }
          ln->first_run = l->nruns;
          for (q = start; q < vis_end; q++) {
              const RTFmt *f = rt_format_at(rt, q);
              if (!run || f != rf) {
                  cfmt_at(p, t, q, &c, &lastf);
                  run = new_run(l); run->start = q; run->end = q; run->x = l->cx[q]; run->font = c.font; run->size = c.size; run->color = c.f->color; run->underline = c.f->underline; run->letter_spacing = c.ls; run->kerning = c.f->kerning;
                  rf = f;
                  { double a = f_ascent(c.font, c.size), d = f_descent(c.font, c.size); if (!nbox || a > bas) bas = a; if (!nbox || d > bds) bds = d; nbox++; }
              }
              run->end = q + 1;
          }
          if (!nbox) {   /* an empty line: one empty box in the format of its line break (or the text's end) */
              const RTFmt *fb = !n ? &t->deffmt : rt_format_at(rt, end > start ? end - 1 : (start < n ? start : n - 1));
              fmt_metrics(p, t, fb, &bas, &bds);
          }
          ln->nruns = l->nruns - ln->first_run;
          { int ve = vis_end; while (ve > start && rt->text[ve - 1] == ' ' && t->swf_version >= 8 && pf->align != RT_ALIGN_LEFT) ve--;
            ln->width = ve > start ? l->cx[ve - 1] + l->cw[ve - 1] - lx : 0;
            /* Linux player: a line break measures as a space */
            if (devfont_linux_rules() && ve > start && vis_end < end && (rt->text[vis_end] == '\r' || rt->text[vis_end] == '\n')) {
                unsigned char m2 = 0; cfmt_at(p, t, vis_end, &c, &lastf); ln->width += char_adv(&c, ' ', 0, pw, &m2);
            } }
          ln->ascent = asc; ln->descent = desc; ln->leading = lead;
          ln->x = lx; ln->y = y;
          ln->btop = y + asc - bas; ln->bbot = y + asc + bds;
          if (l->nlines == 1) ln->bbot += lead;   /* the very first line always gets the leading */
          /* alignment */
          { double avail = (t->wordwrap || t->autosize == 0 ? t->w - 2 * GUTTER : forced >= 0 ? forced : ln->width + lx + rightm) - lx - rightm, mis = avail - ln->width, off = 0;
            if (g_devfont_registry) { if (!pf->bullet) { if (pf->align == RT_ALIGN_CENTER) off = mis / 2; else if (pf->align == RT_ALIGN_RIGHT) off = mis; } }
            else {
            if (!pf->bullet) { if (pf->align == RT_ALIGN_CENTER) off = mis / 2; else if (pf->align == RT_ALIGN_RIGHT) off = mis - 1; }   /* right: a twip of slack */
            if (!pf->bullet && !t->embed && off > 0 && (pf->align == RT_ALIGN_CENTER || pf->align == RT_ALIGN_RIGHT)) {
                /* device text: whole screen pixels (a right-aligned line keeps a twip of slack) */
                double sx = t->dev_sx > 0 ? t->dev_sx : 1;
                off = pf->align == RT_ALIGN_RIGHT ? floor((mis * sx - 1) / 20 + 1e-9) * 20 / sx : floor(mis * sx / 40 + 0.5) * 20 / sx;
            }
            }
            if (off < 0) off = 0;
            if (off) { for (q = start; q < end; q++) l->cx[q] += off; for (q = ln->first_run; q < l->nruns; q++) l->runs[q].x += off; ln->x += off; }
            /* justify: spread the spaces of non-final lines */
            if (pf->align == RT_ALIGN_JUSTIFY && brk > 0 && mis > 0) {
                int spaces = 0, s2 = 0; double add;
                for (q = start; q < vis_end; q++) if (rt->text[q] == ' ' && q + 1 < vis_end) spaces++;
                if (spaces) { add = mis / spaces; for (q = start; q < end; q++) { l->cx[q] += add * s2; if (rt->text[q] == ' ' && q + 1 < vis_end) { s2++; l->cw[q] += add; } } for (q = ln->first_run; q < l->nruns; q++) l->runs[q].x = l->cx[l->runs[q].start]; }
            } }
          for (q = start; q < end; q++) l->cline[q] = l->nlines - 1;
          { double right = ln->x + ln->width; if (ln->width > 0 && right > l->text_right) l->text_right = right; if (ln->width > l->text_w) l->text_w = ln->width; }
          y += asc + desc + adv_lead;
          nl_font = brk > 0 ? (end < n ? rt_format_at(rt, end) : NULL) : (end > start && end <= n ? rt_format_at(rt, end - 1) : NULL);
          nl_lead = nl_font ? nl_font->leading * 20 : lead;
        }
        i = end;
        if (i >= n) {
            /* a final '\r' starts one more (empty) line */
            if (n && (rt->text[n - 1] == '\r' || rt->text[n - 1] == '\n') && !prev_wrap_soft) {
                TFLine *e = new_line(l); const RTFmt *f2 = rt_format_at(rt, n - 1); double ba, bd;
                if (!cls) cls = f2;
                e->start = e->end = n; e->para_start = 1; e->x = left_offset(f2, 1); e->y = y; e->first_run = l->nruns;
                fmt_metrics2(p, t, nl_font ? nl_font : f2, cls, &e->ascent, &e->descent); e->leading = nl_lead;
                fmt_metrics(p, t, f2, &ba, &bd);
                e->btop = y + e->ascent - ba; e->bbot = y + e->ascent + bd;
                if (l->nlines == 1) e->bbot += e->leading;
                y += e->ascent + e->descent + cls->leading * 20;
            }
            break;
        }
    }
    l->cx[n] = n ? l->cx[n - 1] + l->cw[n - 1] : (l->nlines ? l->lines[0].x : 0); l->cw[n] = 0; l->cline[n] = l->nlines ? l->nlines - 1 : 0;
    if (n && (rt->text[n - 1] == '\r' || rt->text[n - 1] == '\n') && l->nlines) { l->cx[n] = l->lines[l->nlines - 1].x; }
    /* height: every line box except the last line's leading; an empty last line (after a final newline) does not count unless editable,
     * but the line before it keeps its leading */
    /* height: the union of the lines' box extents; a non-input field skips an empty last line */
    { int cnt = l->nlines, q; double top = 0, bot = 0;
      if (cnt > 1 && l->lines[cnt - 1].start == l->lines[cnt - 1].end && n && !t->input) cnt--;
      for (q = 0; q < cnt; q++) {
          if (!q || l->lines[q].btop < top) top = l->lines[q].btop;
          if (!q || l->lines[q].bbot > bot) bot = l->lines[q].bbot;
      }
      l->text_h = cnt ? bot - top : 0;
      if (n == 0) l->text_h = t->input && l->nlines ? l->lines[0].ascent + l->lines[0].descent + l->lines[0].leading : 0;   /* an empty input field still has its caret line */
    }
    if (l->text_w < 0) l->text_w = 0;
    t->lay_valid = 1;
}
void tfs_layout(Player *p, TextFieldState *t) {
    TFLayout *l = &t->lay; int q; double mw = 0;
    if (t->lay_valid && l->len == t->rt.len) return;
    lay_pass(p, t, -1);
    if (!t->autosize || t->wordwrap) return;
    /* Ruffle lays out text of unknown width twice: the widest line is the width the lines align in */
    for (q = 0; q < l->nlines; q++) if (l->lines[q].width > mw) mw = l->lines[q].width;
    lay_pass(p, t, mw);
}

void tfs_autosize(Player *p, TextFieldState *t) {
    double nw, nh;
    if (!t->autosize) return;
    tfs_layout(p, t);
    nh = t->lay.text_h + 2 * GUTTER;
    if (!t->wordwrap) {
        int ow = t->w;
        nw = t->lay.text_right + 2 * GUTTER + (t->input && !devfont_linux_rules() ? 50 : 0);   /* Linux player: no input slack */
        nw = floor(nw + 0.5);
        if (t->autosize == 2) t->xmin += (int)((ow - nw) / 2);
        else if (t->autosize == 3) t->xmin += (int)(ow - nw);
        t->w = (int)nw;
        if (ow != t->w) t->lay_valid = 0;
    }
    t->h = (int)floor(nh + 0.5);
    if (t->lay_valid == 0) tfs_layout(p, t);
}

/* Flash computes the autoSize bounds on every relayout but only applies them
   when something reads or depends on the bounds (x, y, width, height, bounds
   queries, rendering), so later property changes still affect the result */
void tfs_apply_autosize(Player *p, TextFieldState *t) {
    if (!t->as_pending) return;
    t->as_pending = 0;
    tfs_autosize(p, t);
}

/* ---- scrolling ---- */
int tfs_maxscroll_v(Player *p, TextFieldState *t) {
    TFLayout *l; double target; int i;
    tfs_layout(p, t); l = &t->lay;
    if (l->nlines == 0) return 1;
    target = l->text_h - (t->h - 2 * GUTTER);
    if (target <= 0) return 1;
    for (i = 0; i < l->nlines; i++) if (l->lines[i].y >= target - 0.5) return i + 1;
    return l->nlines;
}
int tfs_bottom_scroll_v(Player *p, TextFieldState *t) {
    TFLayout *l; double target; int i, top;
    tfs_layout(p, t); l = &t->lay;
    if (l->nlines == 0) return 1;
    top = t->scroll_v - 1; if (top < 0) top = 0; if (top >= l->nlines) top = l->nlines - 1;
    target = t->h - 2 * GUTTER + l->lines[top].y;
    for (i = top; i < l->nlines; i++) if (l->lines[i].y + l->lines[i].ascent + l->lines[i].descent > target + 0.5) return i > top ? i : top + 1;
    return l->nlines;
}
double tfs_maxscroll_h(Player *p, TextFieldState *t) {
    double v;
    if (t->wordwrap) return 0;
    tfs_layout(p, t);
    { double vw = t->w - 2 * GUTTER > 0 ? t->w - 2 * GUTTER : 0;   /* input fields may scroll a quarter of the view further */
      v = t->lay.text_w - vw;
      if (t->input) v += vw / 4; }
    v = floor(v / 20) * 20;
    return v < 0 ? 0 : v;
}

/* ---- hit testing (field-local twips) ---- */
static double line_box_bot(const TFLayout *l, int k) { return l->lines[k].bbot - (k == 0 ? l->lines[0].leading : 0); }
static double top_y(TextFieldState *t) { int s = t->scroll_v - 1; if (s < 0) s = 0; return s < t->lay.nlines ? t->lay.lines[s].y : 0; }
int tfs_line_at_point(Player *p, TextFieldState *t, double x, double y) {
    TFLayout *l; int i; double ty;
    tfs_layout(p, t); l = &t->lay;
    /* the point must lie inside the bounds shrunk by the gutter */
    if (x < t->xmin + GUTTER || x > t->xmin + t->w - GUTTER || y < t->ymin + GUTTER || y > t->ymin + t->h - GUTTER) return -1;
    ty = y - t->ymin - GUTTER + top_y(t);
    if (ty < 0 || l->nlines == 0) return 0;
    /* Ruffle find_line_index_by_y: Rust's binary search over the lines' box
       bounds (the first line's without its leading), the leading below counting */
    { int size = l->nlines, base = 0, cmp;
#define LCMP(k) (line_box_bot(l, k) + l->lines[k].leading <= ty ? -1 : ty < l->lines[k].btop ? 1 : 0)
      while (size > 1) { int half = size / 2, mid = base + half; if (LCMP(mid) != 1) base = mid; size -= half; }
      cmp = LCMP(base);
#undef LCMP
      i = cmp == 0 ? base : base + (cmp == -1);
      return i < l->nlines ? i : l->nlines - 1; }
}
int tfs_char_at_point(Player *p, TextFieldState *t, double x, double y) {
    int li = tfs_line_at_point(p, t, x, y), q; TFLine *ln; double tx;
    if (li < 0 || t->lay.nlines == 0) return -1;
    ln = &t->lay.lines[li];
    tx = x - GUTTER;   /* Flash ignores the bounds offset and scrollH here */
    if (tx == 0) return ln->start;   /* even an empty line yields its newline */
    for (q = ln->start; q < ln->end; q++) { if (t->rt.text[q] == '\r') continue; if (t->lay.cx[q] < tx && tx <= t->lay.cx[q] + t->lay.cw[q]) return q; }
    return -1;
}
int tfs_caret_at_point(Player *p, TextFieldState *t, double x, double y) {
    TFLayout *l; int li, q; double ty, tx;
    tfs_layout(p, t); l = &t->lay;
    if (!l->nlines) return 0;
    ty = y - t->ymin - GUTTER + top_y(t);
    for (li = 0; li < l->nlines - 1; li++) if (ty < l->lines[li].y + l->lines[li].ascent + l->lines[li].descent + l->lines[li].leading) break;
    tx = x - t->xmin - GUTTER + t->scroll_h;
    { TFLine *ln = &l->lines[li]; int end = ln->end;
      while (end > ln->start && (t->rt.text[end - 1] == '\r' || t->rt.text[end - 1] == '\n')) end--;
      for (q = ln->start; q < end; q++) if (tx < l->cx[q] + l->cw[q] / 2) return q;
      return end; }
}

/* device text reports its vertical metrics at the screen size mapped back with scaleX */
double tfs_dev_vscale(TextFieldState *t) { return t->dev_sx > 0 && t->dev_sy > 0 && !t->embed ? t->dev_sy / t->dev_sx : 1; }
void tfs_line_metrics(Player *p, TextFieldState *t, int line, double out[6]) {
    TFLine *ln; tfs_layout(p, t);
    ln = &t->lay.lines[line];
    out[0] = (ln->x + GUTTER) / 20; out[1] = ln->width / 20; out[2] = (ln->ascent + ln->descent + ln->leading) / 20;
    out[3] = ln->ascent / 20; out[4] = ln->descent / 20; out[5] = ln->leading / 20;
    if (!t->embed) {   /* device text: x in screen space / scaleX; heights from the screen size, / scaleX */
        double f = tfs_dev_vscale(t); out[0] += t->dev_tx / 20 / (t->dev_sx > 0 ? t->dev_sx : 1); out[2] *= f; out[3] *= f; out[4] *= f; out[5] *= f;
    }
}
int tfs_char_bounds(Player *p, TextFieldState *t, int i, double out[4]) {
    TFLine *ln; tfs_layout(p, t);
    if (i < 0 || i >= t->rt.len || t->rt.text[i] == '\r' || (t->lay.cmiss[i] && t->lay.cw[i] == 0)) return 0;   /* no glyph and no room: no bounds */
    ln = &t->lay.lines[t->lay.cline[i]];
    if (t->lay.cline[i] < t->scroll_v - 1) return 0;
    if (t->lay.cline[i] > tfs_bottom_scroll_v(p, t)) return 0;   /* below the line after the last visible one: culled */
    out[0] = ((g_devfont_registry ? 0 : t->xmin) + t->lay.cx[i] + GUTTER) / 20;   /* field space (bounds origin); Flash does not apply scrollH here */
    out[1] = ((g_devfont_registry ? 0 : t->ymin) + ln->y - top_y(t) + GUTTER) / 20;
    out[2] = t->lay.cw[i] / 20;
    out[3] = (ln->ascent + ln->descent) / 20;
    return 1;
}

/* ---- drawing ---- */
static void fill_rect(Player *p, double x0, double y0, double x1, double y1, u32 rgb, int line, const SWFMatrix *wm, const SWFCxform *cx, const SWFRect *clip) {
    ShapeDef s; ShapeEdge ed[4]; FillStyle fs; LineStyle ls; int i;
    memset(&s, 0, sizeof(s)); memset(&fs, 0, sizeof(fs)); memset(&ls, 0, sizeof(ls)); swf_matrix_identity(&fs.matrix);
    fs.type = FILL_SOLID; fs.color.r = (u8)(rgb >> 16); fs.color.g = (u8)(rgb >> 8); fs.color.b = (u8)rgb; fs.color.a = 255;
    ls.width = 20; ls.color = fs.color;
    if (line) { s.lines = &ls; s.nlines = 1; } else { s.fills = &fs; s.nfills = 1; }
    s.edges = ed; s.nedges = 4;
    for (i = 0; i < 4; i++) { memset(&ed[i], 0, sizeof(ed[i])); if (line) ed[i].line = 1; else ed[i].fill1 = 1; }
    ed[0].x0 = (s32)x0; ed[0].y0 = (s32)y0; ed[0].x1 = (s32)x1; ed[0].y1 = (s32)y0;
    ed[1].x0 = (s32)x1; ed[1].y0 = (s32)y0; ed[1].x1 = (s32)x1; ed[1].y1 = (s32)y1;
    ed[2].x0 = (s32)x1; ed[2].y0 = (s32)y1; ed[2].x1 = (s32)x0; ed[2].y1 = (s32)y1;
    ed[3].x0 = (s32)x0; ed[3].y0 = (s32)y1; ed[3].x1 = (s32)x0; ed[3].y1 = (s32)y0;
    render_shape(p->surf, p->movie, &s, wm, cx, clip);
}

/* EditTextPixelSnapping: low quality rounds the position; higher qualities
   truncate it and round the scales so the box stays sharp */
static void tf_snap(RMatrix *m) {
    extern int g_stage_quality;
    if (g_stage_quality == 0) { m->tx = twips_round_ties_even_px(m->tx); m->ty = twips_round_ties_even_px(m->ty); return; }
    m->tx = (m->tx + 2) / 20 * 20; m->ty = (m->ty + 2) / 20 * 20;
    if (fabs(m->c) < 0.001f || fabs(m->d) < 0.001f) { m->a = f32_round_ties_even(m->a - 0.35f); m->b = f32_round_ties_even(m->b - 0.35f); }
    if (fabs(m->a) < 0.001f || fabs(m->b) < 0.001f) { m->c = f32_round_ties_even(m->c - 0.35f); m->d = f32_round_ties_even(m->d - 0.35f); }
}
static SWFColor tf_color(u32 rgb, const SWFCxform *cx) {
    SWFColor c; c.r = (u8)(rgb >> 16); c.g = (u8)(rgb >> 8); c.b = (u8)rgb; c.a = 255;
    cxform_apply(cx, &c.r, &c.g, &c.b, &c.a);
    return c;
}
/* the border and background (Ruffle draw_device_text_box / draw_text_box) */
static void tf_draw_box(Player *p, TextFieldState *t, const SWFMatrix *wm, const SWFCxform *cx) {
    RMatrix m, b, tb; s32 ox = t->xmin - ((t->box_flip & 1) ? t->w : 0), oy = t->ymin - ((t->box_flip & 2) ? t->h : 0);
    if (!t->background && !t->border) return;
    rmat_from(&m, wm);
    if (!t->embed) {
        /* device fonts: aliased, on whole pixels, 1 px lines, no bottom right corner */
        s32 xs[4], ys[4], bx0, by0, bx1, by1, wt, ht, x0, y0, x1, y1; int i; float w, h;
        rmat_point(&m, ox, oy, &xs[0], &ys[0]); rmat_point(&m, ox + t->w, oy, &xs[1], &ys[1]);
        rmat_point(&m, ox, oy + t->h, &xs[2], &ys[2]); rmat_point(&m, ox + t->w, oy + t->h, &xs[3], &ys[3]);
        bx0 = bx1 = xs[0]; by0 = by1 = ys[0];
        for (i = 1; i < 4; i++) { if (xs[i] < bx0) bx0 = xs[i]; if (xs[i] > bx1) bx1 = xs[i]; if (ys[i] < by0) by0 = ys[i]; if (ys[i] > by1) by1 = ys[i]; }
        wt = twips_round_ties_even_px(bx1 - bx0); ht = twips_round_ties_even_px(by1 - by0);
        x0 = twips_round_ties_even_px(bx0); y0 = twips_round_ties_even_px(by0); x1 = x0 + wt; y1 = y0 + ht;
        w = (float)(wt / 20.0); h = (float)(ht / 20.0);
        if (t->background) { rmat_box(&b, w, h, x0, y0); render_draw_rect(p->surf, tf_color(t->bgcolor, cx), &b); }
        if (t->border) {
            SWFColor c = tf_color(t->bordercolor, cx); const float hp = 1.57079632679489661923f;
            rmat_box(&b, w, 1.0f, x0 - 10, y0); render_draw_line(p->surf, c, &b);
            rmat_box(&b, w, 1.0f, x0 - 10, y1); render_draw_line(p->surf, c, &b);
            rmat_box_rot(&b, 1.0f, h, hp, x0, y0 - 10); render_draw_line(p->surf, c, &b);
            rmat_box_rot(&b, 1.0f, h, hp, x1, y0 - 10); render_draw_line(p->surf, c, &b);
        }
        return;
    }
    rmat_box(&b, (float)(t->w / 20.0), (float)(t->h / 20.0), ox, oy);
    rmat_mul(&tb, &m, &b);
    tf_snap(&tb);
    if (t->background) render_draw_rect(p->surf, tf_color(t->bgcolor, cx), &tb);
    if (t->border) render_draw_line_rect(p->surf, tf_color(t->bordercolor, cx), &tb);
}
/* the text mask: the bounds less the gutter on the left and right */
static void tf_mask_rect(Player *p, TextFieldState *t, const SWFMatrix *wm) {
    RMatrix m, b, mb; SWFColor white; s32 x0 = t->xmin, x1 = t->xmin + t->w;
    if (x0 <= x1) { x0 += 40; x1 -= 40; }
    white.r = white.g = white.b = white.a = 255;
    rmat_from(&m, wm); rmat_box(&b, (float)((x1 - x0) / 20.0), (float)(t->h / 20.0), x0, t->ymin); rmat_mul(&mb, &m, &b);
    render_draw_rect(p->surf, white, &mb);
}
/* a 1 px line from (x, y) (field twips), `len` px long, horizontal or down, snapped like Ruffle */
static void tf_line(Player *p, const SWFMatrix *wm, double x, double y, double len, int down, u32 rgb, const SWFCxform *cx) {
    RMatrix m, b, lm;
    rmat_from(&m, wm);
    if (down) rmat_box_rot(&b, 1.0f, (float)len, 1.57079632679489661923f, (s32)floor(x + 0.5), (s32)floor(y + 0.5));
    else rmat_box(&b, (float)len, 1.0f, (s32)floor(x + 0.5), (s32)floor(y + 0.5));
    rmat_mul(&lm, &m, &b);
    tf_snap(&lm);
    if (down) lm.ty -= 10;
    render_draw_line(p->surf, tf_color(rgb, cx), &lm);
}

void tfs_render(Player *p, TextFieldState *t, const SWFMatrix *wm, const SWFCxform *cx, int focused) {
    TFLayout *l; int li, first; double ox, oy, top, bottom; SWFRect clip, *clipp = NULL; int cull = 0; SWFMatrix dwm; const SWFMatrix *bwm = wm;
    double x0, y0, x1;
    tfs_apply_autosize(p, t);
    x0 = t->xmin; y0 = t->ymin; x1 = t->xmin + t->w;
    tfs_layout(p, t); l = &t->lay;
    /* device text is not drawn rotated, sheared or mirrored */
    if (!t->embed && !(fabs(wm->r0 * 20) < 0.006 && fabs(wm->r1 * 20) < 0.006 && wm->sx > 0 && wm->sy > 0)) return;
    tf_draw_box(p, t, wm, cx);
    surf_push_mask(p->surf); tf_mask_rect(p, t, wm); surf_activate_mask(p->surf);
    (void)clip; cull = fabs(wm->r0) < 1e-9 && fabs(wm->r1) < 1e-9 && p->surf->mstate != MS_DRAW_STENCIL && p->surf->mstate != MS_CLEAR_STENCIL;   /* a mask is not clipped by its field */
    first = t->scroll_v - 1; if (first < 0) first = 0; if (first >= l->nlines) first = l->nlines ? l->nlines - 1 : 0;
    top = l->nlines ? l->lines[first].y : 0;
    ox = x0 + GUTTER - t->scroll_h; oy = y0 + GUTTER - top;
    if (!t->embed && wm->sx != 0) {
        /* device text cannot be scaled independently in x and y: the content keeps the
           y scale in both directions (Ruffle layout_to_local_matrix: scale(d / a, 1) at the text origin) */
        float k = (float)(wm->sy * 20.0) / (float)(wm->sx * 20.0);
        if (k != 1.0f) {
            dwm = *wm;
            dwm.sx = wm->sx * k; dwm.r0 = wm->r0 * k;
            dwm.tx = wm->tx + wm->sx * ox * (1.0 - k); dwm.ty = wm->ty + wm->r0 * ox * (1.0 - k);
            wm = &dwm;
        }
    }
    bottom = t->h - 2 * GUTTER + top;
    /* selection highlight */
    if (t->sel_begin != t->sel_end && (focused || t->always_show_selection)) {
        /* Ruffle render_selection_background_for_line: one box per line from the
           first selected character to the end of the last one (a line break
           ends at the line's extent), over the leading unless the selection
           ends in that line */
        int a = t->sel_begin < t->sel_end ? t->sel_begin : t->sel_end, b = t->sel_begin < t->sel_end ? t->sel_end : t->sel_begin, k;
        for (k = first; k < l->nlines; k++) {
            TFLine *ln = &l->lines[k]; int ls = a < ln->start ? ln->start : a > ln->end ? ln->end : a, le = b < ln->start ? ln->start : b > ln->end ? ln->end : b;
            double xs, xe, lead;
            if (ls >= le) continue;
            lead = le == b ? 0 : ln->leading;
            xs = (ls < t->rt.len && t->rt.text[ls] != '\r' && t->rt.text[ls] != '\n') ? l->cx[ls] : ln->x;
            xe = (le - 1 < t->rt.len && t->rt.text[le - 1] != '\r' && t->rt.text[le - 1] != '\n') ? l->cx[le - 1] + l->cw[le - 1] : ln->x + ln->width;
            fill_rect(p, ox + xs, oy + ln->y, ox + xe, oy + ln->y + ln->ascent + ln->descent + lead, focused ? 0x000000 : 0x555555, 0, wm, cx, clipp);
        }
    }
    for (li = first; li < l->nlines; li++) {
        TFLine *ln = &l->lines[li]; int r; double base = oy + ln->y + ln->ascent;
        /* bullet */
        if (ln->para_start && ln->start < t->rt.len) {
            const RTFmt *f = rt_format_at(&t->rt, ln->start);
            if (f && f->bullet && ln->nruns) { TFRun *r0 = &l->runs[ln->first_run]; int gi = r0->font ? font_glyph_for_code(r0->font, 0x2022) : -1; SWFColor col; col.r = (u8)(r0->color >> 16); col.g = (u8)(r0->color >> 8); col.b = (u8)r0->color; col.a = 255;
                if (gi >= 0) draw_glyph(p, r0->font, gi, wm, ox + left_offset(f, 1) - 18 * 20, base, r0->size, col, cx); }
        }
        for (r = ln->first_run; r < ln->first_run + ln->nruns; r++) {
            TFRun *run = &l->runs[r]; int q; SWFColor col;
            if (!run->font) continue;
            /* Ruffle render_layout_box: a box whose top is below the field's bottom is not drawn */
            if (ln->y + ln->ascent - f_ascent(run->font, run->size) - top + GUTTER > t->h) continue;
            col.r = (u8)(run->color >> 16); col.g = (u8)(run->color >> 8); col.b = (u8)run->color; col.a = 255;
            for (q = run->start; q < run->end; q++) {
                u32 ch = t->password ? '*' : t->rt.text[q]; int gi; SWFColor cc = col;
                int selected = t->sel_begin != t->sel_end && (focused || t->always_show_selection) && q >= (t->sel_begin < t->sel_end ? t->sel_begin : t->sel_end) && q < (t->sel_begin < t->sel_end ? t->sel_end : t->sel_begin);
                if (ch == ' ' || ch == '\t' || ch == 0xA0) continue;
                gi = font_glyph_for_code(run->font, (int)ch);
                if (gi < 0) continue;
                if (selected) { cc.r = cc.g = cc.b = 255; }   /* selected glyphs are white (Ruffle: identity colour transform) */
                if (cull) {   /* skip glyphs far outside the field */
                    double gx = ox + l->cx[q];
                    if (gx + l->cw[q] < x0 - 200 || gx > x1 + 200) continue;
                }
                {   /* Ruffle's stack: layout_to_local, the text box at its origin, then the glyph */
                    SWFMatrix t1, m1, m2; double rasc = f_ascent(run->font, run->size);
                    swf_matrix_identity(&t1); t1.tx = ox; t1.ty = oy; render_matrix_compose(&m1, wm, &t1);
                    swf_matrix_identity(&t1); t1.tx = run->x; t1.ty = ln->y + ln->ascent - rasc; render_matrix_compose(&m2, &m1, &t1);
                    draw_glyph(p, run->font, gi, &m2, l->cx[q] - run->x, rasc, run->size, cc, cx);
                }
            }
            if (run->underline && run->end > run->start) {
                double ux0 = ox + l->cx[run->start], ux1 = ox + l->cx[run->end - 1] + l->cw[run->end - 1], uy = base + floor(ln->descent / 2.0);   /* Ruffle: ascent + max_descent / 2 */
                tf_line(p, wm, ux0, uy, (ux1 - ux0) / 20.0, 0, run->color, cx);
            }
        }
    }
    surf_deactivate_mask(p->surf); tf_mask_rect(p, t, bwm); surf_pop_mask(p->surf);
    /* caret (outside the mask) */
    if (focused && t->input && t->sel_begin == t->sel_end && (p->caret_blink & 16)) {
        int c = t->sel_end; TFLine *ln; double cxp;
        if (c < 0) c = 0; if (c > t->rt.len) c = t->rt.len;
        if (l->nlines) {
            int lnum = l->cline[c];
            if (c == t->rt.len && c > 0 && (t->rt.text[c - 1] == '\r' || t->rt.text[c - 1] == '\n')) lnum = l->nlines - 1;
            ln = &l->lines[lnum];
            cxp = c < t->rt.len || lnum != l->cline[c] ? l->cx[c] : l->cx[c];
            if (lnum == l->nlines - 1 && ln->start == ln->end) cxp = ln->x;
            if (lnum >= first) {
                u32 col = 0; const RTFmt *f = rt_format_at(&t->rt, c > 0 ? c - 1 : 0); if (f) col = f->color;
                tf_line(p, wm, ox + cxp, oy + ln->y, (ln->ascent + ln->descent) / 20.0, 1, col, cx);
            }
        }
    }
}
