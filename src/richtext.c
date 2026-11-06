/* richtext.c - TextField content: format spans and Flash's HTML dialect.
 * The span operations and the HTML writer follow the behaviour of Flash
 * Player's TextField as documented by its observable output (htmlText,
 * getTextFormat after setTextFormat/replaceText, ...). */
#include "richtext.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------- */
/* formats                                                          */

void rt_fmt_clear(RTFmt *f) { memset(f, 0, sizeof(*f)); }
void rt_fmt_default(RTFmt *f) {
    memset(f, 0, sizeof(*f));
    strcpy(f->font, "Times New Roman"); f->size = 12;
    f->has = RTF_ALL;
}
void rt_fmt_apply(RTFmt *d, const RTFmt *s) {
    u32 h = s->has;
    if (h & RTF_FONT) strcpy(d->font, s->font);
    if (h & RTF_SIZE) d->size = s->size;
    if (h & RTF_COLOR) d->color = s->color;
    if (h & RTF_BOLD) d->bold = s->bold;
    if (h & RTF_ITALIC) d->italic = s->italic;
    if (h & RTF_UNDERLINE) d->underline = s->underline;
    if (h & RTF_URL) strcpy(d->url, s->url);
    if (h & RTF_TARGET) strcpy(d->target, s->target);
    if (h & RTF_ALIGN) d->align = s->align;
    if (h & RTF_LEFTMARGIN) d->left_margin = s->left_margin;
    if (h & RTF_RIGHTMARGIN) d->right_margin = s->right_margin;
    if (h & RTF_INDENT) d->indent = s->indent;
    if (h & RTF_BLOCKINDENT) d->block_indent = s->block_indent;
    if (h & RTF_LEADING) d->leading = s->leading;
    if (h & RTF_LETTERSPACING) d->letter_spacing = s->letter_spacing;
    if (h & RTF_KERNING) d->kerning = s->kerning;
    if (h & RTF_BULLET) d->bullet = s->bullet;
    if (h & RTF_TABSTOPS) { d->ntabs = s->ntabs; memcpy(d->tabs, s->tabs, sizeof(d->tabs)); }
    if (h & RTF_DISPLAY) d->display = s->display;
    d->has |= h;
}
static int tabs_eq(const RTFmt *a, const RTFmt *b) { int i; if (a->ntabs != b->ntabs) return 0; for (i = 0; i < a->ntabs; i++) if (a->tabs[i] != b->tabs[i]) return 0; return 1; }
static int font_eq(const RTFmt *a, const RTFmt *b) { return !strcmp(a->font, b->font) && a->size == b->size && a->color == b->color && a->letter_spacing == b->letter_spacing && a->kerning == b->kerning; }
static RTImg *g_imgs; static int g_nimgs, g_capimgs;
const RTImg *rt_image(int img) { return img > 0 && img <= g_nimgs ? &g_imgs[img - 1] : NULL; }
static int rt_image_new(const RTImg *im) {
    if (g_nimgs == g_capimgs) { int nc = g_capimgs ? g_capimgs * 2 : 16; RTImg *n = (RTImg*)realloc(g_imgs, (size_t)nc * sizeof(RTImg)); if (!n) return 0; g_imgs = n; g_capimgs = nc; }
    g_imgs[g_nimgs++] = *im;
    return g_nimgs;
}
static int img_eq(int x, int y) {
    const RTImg *a = rt_image(x), *b = rt_image(y);
    if (!a || !b) return a == b;
    return !strcmp(a->src, b->src) && !strcmp(a->id, b->id) && a->has_id == b->has_id && a->has_w == b->has_w && a->has_h == b->has_h &&
        a->has_hs == b->has_hs && a->has_vs == b->has_vs && a->width == b->width && a->height == b->height && a->hspace == b->hspace &&
        a->vspace == b->vspace && a->align == b->align && a->check_policy_file == b->check_policy_file;
}
int rt_fmt_same(const RTFmt *a, const RTFmt *b) {
    if (a->img != b->img && !img_eq(a->img, b->img)) return 0;
    return font_eq(a, b) && a->bold == b->bold && a->italic == b->italic && a->underline == b->underline &&
        a->align == b->align && a->left_margin == b->left_margin && a->right_margin == b->right_margin && a->indent == b->indent &&
        a->block_indent == b->block_indent && a->leading == b->leading && tabs_eq(a, b) && a->bullet == b->bullet &&
        !strcmp(a->url, b->url) && !strcmp(a->target, b->target) && a->display == b->display;
}
void rt_fmt_intersect(RTFmt *a, const RTFmt *f) {
    u32 h = a->has & f->has;
    if ((h & RTF_FONT) && strcmp(a->font, f->font)) h &= ~RTF_FONT;
    if ((h & RTF_SIZE) && a->size != f->size) h &= ~RTF_SIZE;
    if ((h & RTF_COLOR) && a->color != f->color) h &= ~RTF_COLOR;
    if ((h & RTF_BOLD) && a->bold != f->bold) h &= ~RTF_BOLD;
    if ((h & RTF_ITALIC) && a->italic != f->italic) h &= ~RTF_ITALIC;
    if ((h & RTF_UNDERLINE) && a->underline != f->underline) h &= ~RTF_UNDERLINE;
    if ((h & RTF_URL) && strcmp(a->url, f->url)) h &= ~RTF_URL;
    if ((h & RTF_TARGET) && strcmp(a->target, f->target)) h &= ~RTF_TARGET;
    if ((h & RTF_ALIGN) && a->align != f->align) h &= ~RTF_ALIGN;
    if ((h & RTF_LEFTMARGIN) && a->left_margin != f->left_margin) h &= ~RTF_LEFTMARGIN;
    if ((h & RTF_RIGHTMARGIN) && a->right_margin != f->right_margin) h &= ~RTF_RIGHTMARGIN;
    if ((h & RTF_INDENT) && a->indent != f->indent) h &= ~RTF_INDENT;
    if ((h & RTF_BLOCKINDENT) && a->block_indent != f->block_indent) h &= ~RTF_BLOCKINDENT;
    if ((h & RTF_LEADING) && a->leading != f->leading) h &= ~RTF_LEADING;
    if ((h & RTF_LETTERSPACING) && a->letter_spacing != f->letter_spacing) h &= ~RTF_LETTERSPACING;
    if ((h & RTF_KERNING) && a->kerning != f->kerning) h &= ~RTF_KERNING;
    if ((h & RTF_BULLET) && a->bullet != f->bullet) h &= ~RTF_BULLET;
    if ((h & RTF_TABSTOPS) && !tabs_eq(a, f)) h &= ~RTF_TABSTOPS;
    if ((h & RTF_DISPLAY) && a->display != f->display) h &= ~RTF_DISPLAY;
    a->has = h;
}

/* ---------------------------------------------------------------- */
/* spans                                                            */

void rt_init(RichText *rt) { memset(rt, 0, sizeof(*rt)); }
void rt_free(RichText *rt) { free(rt->text); free(rt->spans); memset(rt, 0, sizeof(*rt)); }
static void text_reserve(RichText *rt, int n) { if (n + 1 > rt->cap) { rt->cap = (n + 1) * 2 + 16; rt->text = (u32*)realloc(rt->text, (size_t)rt->cap * sizeof(u32)); } }
static void span_reserve(RichText *rt, int n) { if (n > rt->capspans) { rt->capspans = n * 2 + 4; rt->spans = (RTSpan*)realloc(rt->spans, (size_t)rt->capspans * sizeof(RTSpan)); } }
static void span_insert(RichText *rt, int at, int len, const RTFmt *f) {
    span_reserve(rt, rt->nspans + 1);
    memmove(rt->spans + at + 1, rt->spans + at, (size_t)(rt->nspans - at) * sizeof(RTSpan));
    rt->spans[at].len = len; rt->spans[at].f = *f; rt->nspans++;
}
static void span_remove(RichText *rt, int at, int count) {
    memmove(rt->spans + at, rt->spans + at + count, (size_t)(rt->nspans - at - count) * sizeof(RTSpan));
    rt->nspans -= count;
}
void rt_copy(RichText *d, const RichText *s) {
    rt_free(d);
    text_reserve(d, s->len); if (s->len) memcpy(d->text, s->text, (size_t)s->len * sizeof(u32)); d->len = s->len;
    span_reserve(d, s->nspans); if (s->nspans) memcpy(d->spans, s->spans, (size_t)s->nspans * sizeof(RTSpan)); d->nspans = s->nspans;
}

/* the format every span gets for the parts nothing else covers */
static void normalize(RichText *rt, const RTFmt *fill) {
    int sum = 0, i;
    for (i = 0; i < rt->nspans; i++) sum += rt->spans[i].len;
    if (sum < rt->len) { RTFmt f; if (fill) f = *fill; else if (rt->nspans) f = rt->spans[rt->nspans - 1].f; else rt_fmt_default(&f); span_insert(rt, rt->nspans, rt->len - sum, &f); }
    else if (sum > rt->len) {
        int extra = sum - rt->len;
        for (i = rt->nspans - 1; i >= 0 && extra > 0; i--) { int t = rt->spans[i].len < extra ? rt->spans[i].len : extra; rt->spans[i].len -= t; extra -= t; }
    }
    /* drop empty spans (keep one so an empty field still has a format) and merge equal neighbours */
    for (i = 0; i < rt->nspans && rt->nspans > 1; ) {
        if (rt->spans[i].len == 0) { span_remove(rt, i, 1); continue; }
        if (i > 0 && rt_fmt_same(&rt->spans[i - 1].f, &rt->spans[i].f)) { rt->spans[i - 1].len += rt->spans[i].len; span_remove(rt, i, 1); continue; }
        i++;
    }
    if (!rt->nspans) { RTFmt f; if (fill) f = *fill; else rt_fmt_default(&f); span_insert(rt, 0, rt->len, &f); }
}

/* index of the span starting at pos (splitting one if needed); nspans when pos == len */
static int break_at(RichText *rt, int pos) {
    int i, start = 0;
    for (i = 0; i < rt->nspans; i++) {
        int l = rt->spans[i].len;
        if (pos == start) return i;
        if (pos < start + l) {
            RTFmt f = rt->spans[i].f;
            rt->spans[i].len = pos - start;
            span_insert(rt, i + 1, start + l - pos, &f);
            return i + 1;
        }
        start += l;
    }
    return rt->nspans;
}

void rt_set_plain(RichText *rt, const u32 *s, int n, const RTFmt *fmt) {
    text_reserve(rt, n); if (n) memcpy(rt->text, s, (size_t)n * sizeof(u32)); rt->len = n;
    rt->nspans = 0; span_insert(rt, 0, n, fmt);
}

void rt_replace(RichText *rt, int from, int to, const u32 *s, int n, const RTFmt *deffmt) {
    RTFmt f; int a, b;
    if (from < 0) from = 0; if (to > rt->len) to = rt->len;
    if (to < from) return;
    if (from < rt->len) {
        a = break_at(rt, from); b = break_at(rt, to);
        f = b < rt->nspans ? rt->spans[b].f : *deffmt;   /* the format of what follows the range */
        span_remove(rt, a, b - a);
        span_insert(rt, a, n, &f);
    } else {
        span_insert(rt, rt->nspans, n, deffmt);
    }
    text_reserve(rt, rt->len - (to - from) + n);
    memmove(rt->text + from + n, rt->text + to, (size_t)(rt->len - to) * sizeof(u32));
    if (n) memcpy(rt->text + from, s, (size_t)n * sizeof(u32));
    rt->len = rt->len - (to - from) + n;
    normalize(rt, deffmt);
}

void rt_replace_as(RichText *rt, int from, int to, const u32 *s, int n, const RTFmt *use, const RTFmt *deffmt) {
    int a, b;
    if (from < 0) from = 0; if (to > rt->len) to = rt->len;
    if (to < from) return;
    if (from < rt->len) { a = break_at(rt, from); b = break_at(rt, to); span_remove(rt, a, b - a); span_insert(rt, a, n, use); }
    else span_insert(rt, rt->nspans, n, use);
    text_reserve(rt, rt->len - (to - from) + n);
    memmove(rt->text + from + n, rt->text + to, (size_t)(rt->len - to) * sizeof(u32));
    if (n) memcpy(rt->text + from, s, (size_t)n * sizeof(u32));
    rt->len = rt->len - (to - from) + n;
    normalize(rt, deffmt);
}

void rt_set_format(RichText *rt, int from, int to, const RTFmt *f) {
    int a, b, i;
    if (from < 0) from = 0; if (to > rt->len) to = rt->len;
    if (from >= to) return;
    a = break_at(rt, from); b = break_at(rt, to);
    for (i = a; i < b; i++) {
        RTFmt g = *f;
        if ((g.has & RTF_FONT) && !g.font[0]) g.has &= ~RTF_FONT;   /* an empty face leaves the font alone */
        rt_fmt_apply(&rt->spans[i].f, &g);
    }
    normalize(rt, NULL);
}

const RTFmt *rt_format_at(const RichText *rt, int pos) {
    int i, start = 0;
    if (!rt->nspans) return NULL;
    for (i = 0; i < rt->nspans; i++) { if (pos < start + rt->spans[i].len) return &rt->spans[i].f; start += rt->spans[i].len; }
    return &rt->spans[rt->nspans - 1].f;
}

int rt_get_format(const RichText *rt, int from, int to, RTFmt *out) {
    int i, start = 0, first = 1;
    if (from < 0) from = 0; if (to > rt->len) to = rt->len;
    if (from >= to) return 0;
    for (i = 0; i < rt->nspans; i++) {
        int l = rt->spans[i].len, end = start + l;
        if (l && end > from && start < to) { if (first) { *out = rt->spans[i].f; first = 0; } else rt_fmt_intersect(out, &rt->spans[i].f); }
        start = end;
        if (start >= to) break;
    }
    return !first;
}

/* ---------------------------------------------------------------- */
/* UTF-8                                                            */

int rt_utf8_decode(const char *s, int n, u32 **out) {
    u32 *u = (u32*)malloc(((size_t)n + 1) * sizeof(u32)); int i = 0, k = 0;
    if (!u) { *out = NULL; return 0; }
    while (i < n) {
        unsigned c = (unsigned char)s[i];
        if (c < 0x80) { u[k++] = c; i++; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < n) { u[k++] = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); i += 2; }
        else if ((c & 0xF0) == 0xE0 && i + 2 < n) { u[k++] = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); i += 3; }
        else if ((c & 0xF8) == 0xF0 && i + 3 < n) { u[k++] = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F); i += 4; }
        else { u[k++] = c; i++; }
    }
    *out = u; return k;
}
char *rt_utf8_encode(const u32 *u, int n, int *outlen) {
    char *b = (char*)malloc((size_t)n * 4 + 1); int i, k = 0;
    if (!b) { if (outlen) *outlen = 0; return NULL; }
    for (i = 0; i < n; i++) {
        u32 c = u[i];
        if (c < 0x80) b[k++] = (char)c;
        else if (c < 0x800) { b[k++] = (char)(0xC0 | (c >> 6)); b[k++] = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { b[k++] = (char)(0xE0 | (c >> 12)); b[k++] = (char)(0x80 | ((c >> 6) & 63)); b[k++] = (char)(0x80 | (c & 63)); }
        else { b[k++] = (char)(0xF0 | (c >> 18)); b[k++] = (char)(0x80 | ((c >> 12) & 63)); b[k++] = (char)(0x80 | ((c >> 6) & 63)); b[k++] = (char)(0x80 | (c & 63)); }
    }
    b[k] = 0; if (outlen) *outlen = k; return b;
}

/* ---------------------------------------------------------------- */
/* HTML reader                                                      */

typedef struct { char *b; int n, cap; } SB;
static void sb_putn(SB *s, const char *p, int n) { if (s->n + n + 1 > s->cap) { s->cap = (s->n + n + 1) * 2 + 64; s->b = (char*)realloc(s->b, (size_t)s->cap); } memcpy(s->b + s->n, p, (size_t)n); s->n += n; s->b[s->n] = 0; }
static void sb_puts(SB *s, const char *p) { sb_putn(s, p, (int)strlen(p)); }

static int lc_eq(const char *a, int n, const char *lit) { int i; for (i = 0; i < n; i++) { char c = a[i]; if (c >= 'A' && c <= 'Z') c = (char)(c + 32); if (c != lit[i]) return 0; } return lit[n] == 0; }
static int is_ws(u32 c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

typedef struct { char name[32]; int p_open; RTFmt f; int block_break; } HStack;
typedef struct {
    RichText *rt; const RTHtmlOpts *o; const RTFmt *def;
    HStack *st; int nst, capst;
    RTFmt last_closed_font; int has_last_font;
    int pending_break;
    int p_open;
} HP;

static RTFmt *cur(HP *h) { return &h->st[h->nst - 1].f; }
static void append(HP *h, const u32 *u, int n, const RTFmt *f0) {
    RichText *rt = h->rt; RTFmt fb; const RTFmt *f = f0;
    if (n <= 0) return;
    if ((f0->has & RTF_DISPLAY) && f0->display != RT_DISPLAY_BLOCK) { fb = *f0; fb.display = RT_DISPLAY_BLOCK; f = &fb; }   /* only the parser uses display: text always reads back as block (Ruffle) */
    text_reserve(rt, rt->len + n); memcpy(rt->text + rt->len, u, (size_t)n * sizeof(u32)); rt->len += n;
    if (rt->nspans && rt_fmt_same(&rt->spans[rt->nspans - 1].f, f)) rt->spans[rt->nspans - 1].len += n;
    else span_insert(rt, rt->nspans, n, f);
}
static void append_cr(HP *h, const RTFmt *f) { u32 c = '\r'; append(h, &c, 1, f); }
static int last_is_cr(HP *h) { return h->rt->len > 0 && h->rt->text[h->rt->len - 1] == '\r'; }

static double parse_num(const char *s) { return atof(s); }
static void style_for(HP *h, const char *sel, RTFmt *f) { if (h->o && h->o->css) { RTFmt g; rt_fmt_clear(&g); if (h->o->css(h->o->css_ctx, sel, &g)) rt_fmt_apply(f, &g); } }

/* attribute lookup in the raw tag text */
/* the value of the k-th occurrence of an attribute (k < 0: the last one) */
static int get_attr_k(const char *a, int n, const char *name, char *out, int cap, int k);
static int get_attr(const char *a, int n, const char *name, char *out, int cap) { return get_attr_k(a, n, name, out, cap, -1); }
static int get_attr_k(const char *a, int n, const char *name, char *out, int cap, int want) {
    int i = 0, nl = (int)strlen(name), found = 0, seen = 0;
    while (i < n) {
        int ks, ke, vs, ve; char q = 0;
        while (i < n && (is_ws((u8)a[i]) || a[i] == '/')) i++;
        ks = i; while (i < n && a[i] != '=' && !is_ws((u8)a[i]) && a[i] != '/') i++; ke = i;
        while (i < n && is_ws((u8)a[i])) i++;
        if (i < n && a[i] == '=') {
            i++; while (i < n && is_ws((u8)a[i])) i++;
            if (i < n && (a[i] == '"' || a[i] == '\'')) { q = a[i++]; vs = i; while (i < n && a[i] != q) i++; ve = i; if (i < n) i++; }
            else { vs = i; while (i < n && !is_ws((u8)a[i])) i++; ve = i; }
        } else { vs = ve = i; }
        if (ke - ks == nl && lc_eq(a + ks, nl, name) && (want < 0 || seen++ == want)) {
            int k, o = 0;
            for (k = vs; k < ve && o < cap - 1; k++) {   /* decode the common entities */
                if (a[k] == '&') {
                    if (!strncmp(a + k, "&amp;", 5)) { out[o++] = '&'; k += 4; continue; }
                    if (!strncmp(a + k, "&lt;", 4)) { out[o++] = '<'; k += 3; continue; }
                    if (!strncmp(a + k, "&gt;", 4)) { out[o++] = '>'; k += 3; continue; }
                    if (!strncmp(a + k, "&quot;", 6)) { out[o++] = '"'; k += 5; continue; }
                    if (!strncmp(a + k, "&apos;", 6)) { out[o++] = '\''; k += 5; continue; }
                }
                out[o++] = a[k];
            }
            out[o] = 0; found = 1;
            if (want >= 0) return 1;
        }
        if (ks == ke && i < n) i++;
    }
    return found;
}

static void push_tag(HP *h, const char *name, int nlen) {
    HStack *t;
    if (h->nst == h->capst) { h->capst = h->capst * 2 + 8; h->st = (HStack*)realloc(h->st, (size_t)h->capst * sizeof(HStack)); }
    t = &h->st[h->nst]; t->f = h->st[h->nst - 1].f; t->p_open = 0; t->block_break = 0;
    if (nlen > 31) nlen = 31; { int i; for (i = 0; i < nlen; i++) { char c = name[i]; t->name[i] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c); } t->name[nlen] = 0; }
    h->nst++;
}

static void start_tag(HP *h, const char *name, int nlen, const char *attrs, int alen) {
    char v[600]; RTFmt *f; int ml = h->o ? h->o->multiline : 1; int ver = h->o ? h->o->swf_version : 10;
    if (lc_eq(name, nlen, "br")) { if (ml) append_cr(h, cur(h)); return; }
    if (lc_eq(name, nlen, "sbr")) { append_cr(h, cur(h)); return; }
    if (lc_eq(name, nlen, "img")) {
        /* a space of size 2 standing for the image; the formatting stack is unchanged */
        RTImg im; RTFmt g; u32 sp = ' ';
        if (ver < 7) return;
        memset(&im, 0, sizeof(im));
        if (!get_attr(attrs, alen, "src", im.src, sizeof(im.src))) return;
        if (get_attr(attrs, alen, "id", im.id, sizeof(im.id))) im.has_id = 1;
        if (get_attr(attrs, alen, "width", v, sizeof(v))) { char *e; double d = strtod(v, &e); if (e != v && !*e) { im.width = d; im.has_w = 1; } }
        if (get_attr(attrs, alen, "height", v, sizeof(v))) { char *e; double d = strtod(v, &e); if (e != v && !*e) { im.height = d; im.has_h = 1; } }
        if (get_attr(attrs, alen, "align", v, sizeof(v))) im.align = !strcmp(v, "right") ? 2 : 1;
        if (get_attr(attrs, alen, "hspace", v, sizeof(v))) { char *e; double d = strtod(v, &e); if (e != v && !*e) { im.hspace = d; im.has_hs = 1; } }
        if (get_attr(attrs, alen, "vspace", v, sizeof(v))) { char *e; double d = strtod(v, &e); if (e != v && !*e) { im.vspace = d; im.has_vs = 1; } }
        if (get_attr(attrs, alen, "checkpolicyfile", v, sizeof(v))) { int k; for (k = 0; v[k]; k++) if (v[k] >= 'A' && v[k] <= 'Z') v[k] += 32; im.check_policy_file = !strcmp(v, "true"); }
        g = *cur(h); g.size = 2; g.img = rt_image_new(&im);
        append(h, &sp, 1, &g);
        return;
    }
    push_tag(h, name, nlen); f = cur(h);
    if (lc_eq(name, nlen, "p")) {
        h->p_open = 1;   /* one flag for all <p>s: a </p> ends a paragraph only after a <p> */
        style_for(h, "p", f); if (get_attr(attrs, alen, "class", v, sizeof(v))) { char sel[620]; sprintf(sel, ".%s", v); style_for(h, sel, f); }
        if (get_attr(attrs, alen, "align", v, sizeof(v))) {
            if (lc_eq(v, (int)strlen(v), "left")) f->align = RT_ALIGN_LEFT; else if (lc_eq(v, (int)strlen(v), "right")) f->align = RT_ALIGN_RIGHT;
            else if (lc_eq(v, (int)strlen(v), "center")) f->align = RT_ALIGN_CENTER; else if (lc_eq(v, (int)strlen(v), "justify")) f->align = RT_ALIGN_JUSTIFY;
        }
    } else if (lc_eq(name, nlen, "a")) {
        style_for(h, "a", f);
        if (get_attr(attrs, alen, "href", v, sizeof(v))) { strncpy(f->url, v, sizeof(f->url) - 1); f->url[sizeof(f->url) - 1] = 0; }
        if (get_attr(attrs, alen, "target", v, sizeof(v))) { strncpy(f->target, v, sizeof(f->target) - 1); f->target[sizeof(f->target) - 1] = 0; }
        if (get_attr(attrs, alen, "class", v, sizeof(v))) { char sel[620]; sprintf(sel, ".%s", v); style_for(h, sel, f); }
    } else if (lc_eq(name, nlen, "font")) {
        /* a repeated attribute: the last one counts */
        if (get_attr(attrs, alen, "face", v, sizeof(v))) { strncpy(f->font, v, sizeof(f->font) - 1); f->font[sizeof(f->font) - 1] = 0; }
        if (get_attr(attrs, alen, "size", v, sizeof(v))) {
            const char *p = v; int sign = 0; double n = 0; int any = 0;
            while (*p == ' ') p++;
            if (*p == '+') { sign = 1; p++; } else if (*p == '-') { sign = -1; p++; }
            while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; any = 1; }
            if (any) {
                double s = sign ? f->size + sign * n : n;
                if (s < 1) s = 1; if (ver < 13 && s > 127) s = 127;
                f->size = s;
            }
        }
        if (get_attr(attrs, alen, "color", v, sizeof(v)) && v[0] == '#') {
            /* the hex digits after '#' (the last six of them); none: no color */
            u32 c = 0; int nd = 0; const char *p = v + 1;
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) { int d = *p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10; c = ((c << 4) | (u32)d) & 0xFFFFFF; p++; nd++; }
            if (nd) f->color = c;
        }
        if (get_attr(attrs, alen, "letterspacing", v, sizeof(v))) f->letter_spacing = parse_num(v);
        if (get_attr(attrs, alen, "kerning", v, sizeof(v))) { if (!strcmp(v, "1")) { if (ver >= 8) f->kerning = 1; } else if (!strcmp(v, "0")) f->kerning = 0; }
    } else if (lc_eq(name, nlen, "b")) f->bold = 1;
    else if (lc_eq(name, nlen, "i")) f->italic = 1;
    else if (lc_eq(name, nlen, "u")) f->underline = 1;
    else if (lc_eq(name, nlen, "li")) {
        style_for(h, "li", f);
        if (ml && h->rt->len && !last_is_cr(h)) append_cr(h, &h->st[h->nst - 2].f);
        f->bullet = 1;
    } else if (lc_eq(name, nlen, "textformat")) {
        if (get_attr(attrs, alen, "leftmargin", v, sizeof(v))) f->left_margin = parse_num(v);
        if (get_attr(attrs, alen, "rightmargin", v, sizeof(v))) f->right_margin = parse_num(v);
        if (get_attr(attrs, alen, "indent", v, sizeof(v))) f->indent = parse_num(v);
        if (get_attr(attrs, alen, "blockindent", v, sizeof(v))) f->block_indent = parse_num(v);
        if (get_attr(attrs, alen, "leading", v, sizeof(v))) f->leading = parse_num(v);
        if (get_attr(attrs, alen, "tabstops", v, sizeof(v))) {
            char *p = v; f->ntabs = 0;
            while (*p && f->ntabs < RT_MAX_TABS) { char *e; double d; while (*p == ' ') p++; d = strtod(p, &e); if (e != p) f->tabs[f->ntabs++] = d; p = e; while (*p && *p != ',') p++; if (*p == ',') p++; }
        }
    } else if (lc_eq(name, nlen, "span")) {
        if (get_attr(attrs, alen, "class", v, sizeof(v))) { char sel[620]; sprintf(sel, ".%s", v); style_for(h, sel, f); }
    } else {
        /* other tags: inline unless a style sheet says otherwise */
        char sel[40]; int i; for (i = 0; i < nlen && i < 38; i++) sel[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]); sel[i] = 0;
        /* a tag with a style is a block unless the style says otherwise; unstyled tags are inline */
        if (h->o && h->o->css) { RTFmt g; rt_fmt_clear(&g); if (h->o->css(h->o->css_ctx, sel, &g)) { if (!(g.has & RTF_DISPLAY)) { g.has |= RTF_DISPLAY; g.display = RT_DISPLAY_BLOCK; } rt_fmt_apply(f, &g); if (g.display != RT_DISPLAY_INLINE) h->pending_break = 1; } }
        if (get_attr(attrs, alen, "class", v, sizeof(v))) { char s2[620]; sprintf(s2, ".%s", v); style_for(h, s2, f); }
    }
}

static void end_tag(HP *h, const char *name, int nlen) {
    int ml = h->o ? h->o->multiline : 1; HStack *t;
    if (lc_eq(name, nlen, "br") || lc_eq(name, nlen, "sbr")) return;
    if (h->nst <= 1) return;
    t = &h->st[h->nst - 1];
    { char lname[32]; int i; for (i = 0; i < nlen && i < 31; i++) lname[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]); lname[i] = 0;
      if (strcmp(lname, t->name)) return; }     /* only the innermost open tag can be closed */
    if (h->pending_break) { append_cr(h, cur(h)); h->pending_break = 0; }   /* a block-styled tag ends with a newline at the next end tag */
    if (!strcmp(t->name, "li")) { if (ml) append_cr(h, &t->f); }
    else if (!strcmp(t->name, "p")) {
        if (ml && h->p_open) {
            RTFmt f = t->f;
            f.bold = f.italic = f.underline = 0; f.url[0] = 0; f.target[0] = 0;
            { const RTFmt *src = h->has_last_font ? &h->last_closed_font : h->def;
              strcpy(f.font, src->font); f.size = src->size; f.color = src->color; f.letter_spacing = src->letter_spacing; f.kerning = src->kerning; }
            append_cr(h, &f);
            h->p_open = 0;
        }
    } else if (!strcmp(t->name, "font")) { h->last_closed_font = t->f; h->has_last_font = 1; }
    h->nst--;
}

static void text_node(HP *h, const char *s, int n) {
    u32 *u; int k, i, o = 0, ver = h->o ? h->o->swf_version : 10, cw = h->o ? h->o->condense_white : 0;
    RTFmt *f = cur(h);
    if (n <= 0 || f->display == RT_DISPLAY_NONE) return;
    k = rt_utf8_decode(s, n, &u); if (!u) return;
    /* entities */
    { u32 *d = u; int j;
      for (i = 0, j = 0; i < k; ) {
          if (u[i] == '&' && i + 1 < k && u[i + 1] == '#') {
              /* numeric: &#[x] [ws] [+-]digits, anything after the digits up to ';' is
               * ignored, the value wraps to 16 bits (Flash's XML entity parser) */
              int p = i + 2, hex = 0, neg = 0, nd = 0; u32 v2 = 0;
              if (p < k && u[p] == 'x') { hex = 1; p++; }
              while (p < k && (u[p] == ' ' || u[p] == '\t' || u[p] == '\n' || u[p] == '\r')) p++;
              if (p < k && (u[p] == '+' || u[p] == '-')) { neg = u[p] == '-'; p++; }
              for (; p < k; p++, nd++) {
                  u32 c = u[p], dv;
                  if (c >= '0' && c <= '9') dv = c - '0';
                  else if (hex && c >= 'a' && c <= 'f') dv = c - 'a' + 10;
                  else if (hex && c >= 'A' && c <= 'F') dv = c - 'A' + 10;
                  else break;
                  v2 = v2 * (hex ? 16u : 10u) + dv;
              }
              if (nd > 0) {
                  while (p < k && u[p] != ';' && u[p] != '&' && u[p] != '<') p++;
                  if (p < k && u[p] == ';') { if (neg) v2 = 0u - v2; d[j++] = v2 & 0xFFFF; i = p + 1; continue; }
              }
              d[j++] = u[i++];
              continue;
          }
          if (u[i] == '&') {
              int e = i + 1; while (e < k && e - i < 12 && u[e] != ';' && u[e] != '&' && u[e] != '<') e++;
              if (e < k && u[e] == ';') {
                  char nm[16]; int m, l = e - i - 1; u32 val = 0; int ok = 0;
                  for (m = 0; m < l && m < 15; m++) nm[m] = (char)u[i + 1 + m]; nm[m] = 0;
                  if (lc_eq(nm, l, "amp")) { val = '&'; ok = 1; } else if (lc_eq(nm, l, "lt")) { val = '<'; ok = 1; } else if (lc_eq(nm, l, "gt")) { val = '>'; ok = 1; }
                  else if (lc_eq(nm, l, "quot")) { val = '"'; ok = 1; } else if (lc_eq(nm, l, "apos")) { val = '\''; ok = 1; } else if (lc_eq(nm, l, "nbsp")) { val = 0xA0; ok = 1; }
                  if (ok) { d[j++] = val; i = e + 1; continue; }
              }
          }
          d[j++] = u[i++];
      }
      k = j; }
    if (ver <= 7) { int allws = 1; for (i = 0; i < k; i++) if (!is_ws(u[i])) { allws = 0; break; } if (allws) { free(u); return; } }
    for (i = 0; i < k; i++) {
        u32 c = u[i];
        if (cw) { if (is_ws(c)) { if (o > 0 && u[o - 1] == ' ' && is_ws(u[i - 1])) continue; c = ' '; } }   /* runs of white space in this text become one space */
        else if (c == '\n') c = '\r';
        u[o++] = c;
    }
    append(h, u, o, f);
    free(u);
}

void rt_from_html(RichText *rt, const char *html, const RTFmt *deffmt_in, const RTHtmlOpts *o) {
    HP h; const char *p = html, *e = html + strlen(html), *t;
    RTFmt dl = *deffmt_in; const RTFmt *deffmt = &dl; RTHtmlOpts ol;
    /* SWF < 8: HTML is always left aligned by default */
    if (o && o->swf_version < 8) dl.align = RT_ALIGN_LEFT;
    /* SWF 6 ignores `multiline`: fields always behave as multiline */
    if (o && o->swf_version <= 6 && !o->multiline) { ol = *o; ol.multiline = 1; o = &ol; }
    memset(&h, 0, sizeof(h));
    rt->len = 0; rt->nspans = 0;
    h.rt = rt; h.o = o; h.def = deffmt;
    h.capst = 16; h.st = (HStack*)malloc((size_t)h.capst * sizeof(HStack)); h.nst = 1; h.st[0].f = *deffmt; h.st[0].name[0] = 0; h.st[0].p_open = 0; h.st[0].block_break = 0;
    while (p < e) {
        if (*p == '<') {
            if (!strncmp(p, "<!--", 4)) { const char *q = strstr(p + 4, "-->"); p = q ? q + 3 : e; continue; }
            if (p[1] == '!' || p[1] == '?') { const char *q = strchr(p, '>'); p = q ? q + 1 : e; continue; }
            t = p + 1;
            {
                const char *q = t; int closing = 0, selfclose = 0; const char *nb, *ne, *ab, *ae;
                while (q < e && *q != '>') { if (*q == '"' || *q == '\'') { char qq = *q++; while (q < e && *q != qq) q++; if (q < e) q++; continue; } q++; }
                if (q >= e) break;   /* unterminated tag: an XML error, nothing after it is kept */
                if (*t == '/') { closing = 1; t++; }
                nb = t; while (t < q && !is_ws((u8)*t) && *t != '/') t++; ne = t;
                ab = t; ae = q; if (ae > ab && ae[-1] == '/') { selfclose = 1; ae--; }
                if (ne > nb && !closing) {
                    /* an unquoted attribute value is an XML error: Flash keeps nothing after it */
                    const char *x = ab; int bad = 0;
                    while (x < ae) {
                        if (*x == '"' || *x == '\'') { char qq = *x++; while (x < ae && *x != qq) x++; if (x < ae) x++; continue; }
                        if (*x == '=') { x++; while (x < ae && is_ws((u8)*x)) x++; if (x < ae && *x != '"' && *x != '\'') { bad = 1; break; } continue; }
                        x++;
                    }
                    if (bad) break;
                }
                if (ne > nb) {
                    if (closing) end_tag(&h, nb, (int)(ne - nb));
                    else { start_tag(&h, nb, (int)(ne - nb), ab, (int)(ae - ab)); if (selfclose) end_tag(&h, nb, (int)(ne - nb)); }
                }
                p = q + 1;
            }
        } else {
            const char *q = p; while (q < e && *q != '<') q++;
            text_node(&h, p, (int)(q - p)); p = q;
        }
    }
    free(h.st);
    if (o && o->condense_white && o->swf_version >= 8) {
        /* spaces at the start, after a space and after a newline go */
        int i, k = 0, prev_ws = 1; RichText *r = rt; int *olen = (int*)malloc((size_t)(r->nspans + 1) * sizeof(int));
        for (i = 0; i < r->nspans; i++) olen[i] = r->spans[i].len;
        for (i = 0; i < r->len; i++) {
            int sp = r->text[i] == ' ', nl = r->text[i] == '\r', drop, img = 0;
            { int s2, start = 0; for (s2 = 0; s2 < r->nspans; s2++) { if (i < start + olen[s2]) { img = r->spans[s2].f.img != 0; break; } start += olen[s2]; } }
            drop = sp && prev_ws && !img;   /* an image placeholder stays (and counts as a space) */
            if (!drop) prev_ws = sp || nl;
            if (drop) { int s2, start = 0; for (s2 = 0; s2 < r->nspans; s2++) { if (i < start + olen[s2]) { r->spans[s2].len--; break; } start += olen[s2]; } continue; }
            r->text[k++] = r->text[i];
        }
        r->len = k;
        free(olen);
    }
    normalize(rt, deffmt);
}

/* ---------------------------------------------------------------- */
/* HTML writer                                                      */

enum { T_TEXTFORMAT, T_P, T_LI, T_FONT, T_A, T_B, T_I, T_U, T_COUNT };
static const char *const TAGNAME[T_COUNT] = { "TEXTFORMAT", "P", "LI", "FONT", "A", "B", "I", "U" };
typedef struct {
    SB sb; int open[T_COUNT]; int nopen;   /* open tags in ascending order */
    int ver;
    RTFmt fonts[16]; int nfonts;
    RTFmt cur; int has_cur;
} HW;

static void num(char *b, double v) {
    if (v == (double)(long)v) sprintf(b, "%ld", (long)v);
    else { int k; sprintf(b, "%.15g", v); for (k = 0; b[k]; k++) if (b[k] == ',') b[k] = '.'; }
}
static void esc_put(SB *s, const char *t) {
    for (; *t; t++) {
        switch (*t) {
        case '&': sb_puts(s, "&amp;"); break; case '<': sb_puts(s, "&lt;"); break; case '>': sb_puts(s, "&gt;"); break;
        case '"': sb_puts(s, "&quot;"); break; case '\'': sb_puts(s, "&apos;"); break;
        default: sb_putn(s, t, 1);
        }
    }
}
static void close_one(HW *w) {
    int t = w->open[--w->nopen];
    if (t == T_FONT) { int i; for (i = 0; i < w->nfonts; i++) sb_puts(&w->sb, "</FONT>"); w->nfonts = 0; }
    else { sb_puts(&w->sb, "</"); sb_puts(&w->sb, TAGNAME[t]); sb_puts(&w->sb, ">"); }
}
static void close_till(HW *w, int tag) { while (w->nopen && w->open[w->nopen - 1] >= tag) close_one(w); }
static int is_open(HW *w, int tag) { int i; for (i = 0; i < w->nopen; i++) if (w->open[i] == tag) return 1; return 0; }
static int can_open(HW *w, int tag) { return !is_open(w, tag) && !(w->nopen && w->open[w->nopen - 1] > tag); }
static void font_attrs(HW *w, const RTFmt *f, const RTFmt *prev) {
    char b[64];
    if (!prev || strcmp(prev->font, f->font)) { sb_puts(&w->sb, " FACE=\""); esc_put(&w->sb, f->font); sb_puts(&w->sb, "\""); }
    if (!prev || prev->size != f->size) { num(b, f->size); sb_puts(&w->sb, " SIZE=\""); sb_puts(&w->sb, b); sb_puts(&w->sb, "\""); }
    if (!prev || prev->color != f->color) { sprintf(b, " COLOR=\"#%02X%02X%02X\"", (unsigned)((f->color >> 16) & 255), (unsigned)((f->color >> 8) & 255), (unsigned)(f->color & 255)); sb_puts(&w->sb, b); }
    if (!prev || prev->letter_spacing != f->letter_spacing) { num(b, f->letter_spacing); sb_puts(&w->sb, " LETTERSPACING=\""); sb_puts(&w->sb, b); sb_puts(&w->sb, "\""); }
    if (!prev || prev->kerning != f->kerning) { sb_puts(&w->sb, f->kerning ? " KERNING=\"1\"" : " KERNING=\"0\""); }
}
static void put_img(HW *w, int img) {
    const RTImg *im = rt_image(img); char b[64];
    if (!im) return;
    sb_puts(&w->sb, "<IMG SRC=\""); sb_puts(&w->sb, im->src); sb_puts(&w->sb, "\"");
    if (im->has_w && im->width - im->width == 0) { sprintf(b, " WIDTH=\"%ld\"", (long)im->width); sb_puts(&w->sb, b); }
    if (im->has_h && im->height - im->height == 0) { sprintf(b, " HEIGHT=\"%ld\"", (long)im->height); sb_puts(&w->sb, b); }
    if (im->has_id) { sb_puts(&w->sb, " ID=\""); sb_puts(&w->sb, im->id); sb_puts(&w->sb, "\""); }
    if (im->align) sb_puts(&w->sb, im->align == 2 ? " ALIGN=\"right\"" : " ALIGN=\"left\"");
    if (im->has_vs && im->vspace - im->vspace == 0) { sprintf(b, " VSPACE=\"%ld\"", (long)im->vspace); sb_puts(&w->sb, b); }
    if (im->has_hs && im->hspace - im->hspace == 0) { sprintf(b, " HSPACE=\"%ld\"", (long)im->hspace); sb_puts(&w->sb, b); }
    if (im->check_policy_file) sb_puts(&w->sb, " CHECKPOLICYFILE=\"true\"");
    sb_puts(&w->sb, ">");
}
static void open_tags_for(HW *w, const RTFmt *f) {
    char b[64];
    if ((f->left_margin || f->right_margin || f->indent || f->leading || f->block_indent || f->ntabs) && can_open(w, T_TEXTFORMAT)) {
        sb_puts(&w->sb, "<TEXTFORMAT");
        if (f->left_margin) { num(b, f->left_margin); sb_puts(&w->sb, " LEFTMARGIN=\""); sb_puts(&w->sb, b); sb_puts(&w->sb, "\""); }
        if (f->right_margin) { num(b, f->right_margin); sb_puts(&w->sb, " RIGHTMARGIN=\""); sb_puts(&w->sb, b); sb_puts(&w->sb, "\""); }
        if (f->indent) { num(b, f->indent); sb_puts(&w->sb, " INDENT=\""); sb_puts(&w->sb, b); sb_puts(&w->sb, "\""); }
        if (f->leading) { num(b, f->leading); sb_puts(&w->sb, " LEADING=\""); sb_puts(&w->sb, b); sb_puts(&w->sb, "\""); }
        if (f->block_indent) { num(b, f->block_indent); sb_puts(&w->sb, " BLOCKINDENT=\""); sb_puts(&w->sb, b); sb_puts(&w->sb, "\""); }
        if (f->ntabs) { int i; sb_puts(&w->sb, " TABSTOPS=\""); for (i = 0; i < f->ntabs; i++) { if (i) sb_puts(&w->sb, ","); num(b, f->tabs[i]); sb_puts(&w->sb, b); } sb_puts(&w->sb, "\""); }
        sb_puts(&w->sb, ">"); w->open[w->nopen++] = T_TEXTFORMAT;
    }
    if (!is_open(w, T_P) && !is_open(w, T_LI)) {
        if (f->bullet) { if (can_open(w, T_LI)) { sb_puts(&w->sb, "<LI>"); w->open[w->nopen++] = T_LI; } }
        else if (can_open(w, T_P)) {
            sb_puts(&w->sb, "<P ALIGN=\""); sb_puts(&w->sb, f->align == RT_ALIGN_RIGHT ? "RIGHT" : f->align == RT_ALIGN_CENTER ? "CENTER" : f->align == RT_ALIGN_JUSTIFY ? "JUSTIFY" : "LEFT"); sb_puts(&w->sb, "\">");
            w->open[w->nopen++] = T_P;
        }
    }
    /* font */
    if (!w->nfonts) {
        if (can_open(w, T_FONT)) { sb_puts(&w->sb, "<FONT"); font_attrs(w, f, NULL); sb_puts(&w->sb, ">"); w->fonts[w->nfonts++] = *f; w->open[w->nopen++] = T_FONT; }
    } else if (!font_eq(&w->fonts[w->nfonts - 1], f) && w->nfonts < 16) {
        close_till(w, T_A);
        sb_puts(&w->sb, "<FONT"); font_attrs(w, f, &w->fonts[w->nfonts - 1]); sb_puts(&w->sb, ">"); w->fonts[w->nfonts++] = *f;
    }
    if (f->img && w->ver < 8) put_img(w, f->img);
    if (f->url[0] && can_open(w, T_A)) {
        sb_puts(&w->sb, "<A HREF=\""); esc_put(&w->sb, f->url); sb_puts(&w->sb, "\" TARGET=\""); esc_put(&w->sb, f->target); sb_puts(&w->sb, "\">");
        w->open[w->nopen++] = T_A;
    }
    if (f->img && w->ver >= 8) put_img(w, f->img);
    if (f->bold && can_open(w, T_B)) { sb_puts(&w->sb, "<B>"); w->open[w->nopen++] = T_B; }
    if (f->italic && can_open(w, T_I)) { sb_puts(&w->sb, "<I>"); w->open[w->nopen++] = T_I; }
    if (f->underline && can_open(w, T_U)) { sb_puts(&w->sb, "<U>"); w->open[w->nopen++] = T_U; }
}
static void set_span(HW *w, const RTFmt *f) {
    if (w->has_cur) {
        if (w->cur.bold != f->bold || w->cur.italic != f->italic || w->cur.underline != f->underline) close_till(w, T_B);
        if (strcmp(w->cur.url, f->url) || strcmp(w->cur.target, f->target)) close_till(w, T_A);
    }
    /* back to an enclosing FONT if the span uses it again */
    if (w->nfonts > 1) {
        int i, pos = -1;
        for (i = 0; i < w->nfonts; i++) if (font_eq(&w->fonts[i], f)) { pos = i; break; }
        if (pos >= 0 && pos < w->nfonts - 1) {
            int k; close_till(w, T_A);
            for (k = 0; k < w->nfonts - pos - 1; k++) sb_puts(&w->sb, "</FONT>");
            w->nfonts = pos + 1;
        }
    }
    w->cur = *f; w->has_cur = 1;
    open_tags_for(w, f);
}

char *rt_to_html(const RichText *rt) { return rt_to_html_v(rt, 8); }
char *rt_to_html_v(const RichText *rt, int swf_version) {
    HW w; int s, pos = 0;
    memset(&w, 0, sizeof(w));
    w.ver = swf_version;
    sb_puts(&w.sb, "");
    if (rt->len == 0) return w.sb.b;
    for (s = 0; s < rt->nspans; s++) {
        int l = rt->spans[s].len, i, segstart;
        if (!l) continue;
        set_span(&w, &rt->spans[s].f);
        segstart = pos;
        for (i = pos; i < pos + l; i++) {
            if (rt->text[i] == '\r' || rt->text[i] == '\n') {
                if (i > segstart) { int n; char *u = rt_utf8_encode(rt->text + segstart, i - segstart, &n); esc_put(&w.sb, u); free(u); }
                close_till(&w, T_TEXTFORMAT);
                segstart = i + 1;
                if (i + 1 < pos + l) open_tags_for(&w, &rt->spans[s].f);
            }
        }
        if (pos + l > segstart) { int n; char *u = rt_utf8_encode(rt->text + segstart, pos + l - segstart, &n); esc_put(&w.sb, u); free(u); }
        pos += l;
    }
    close_till(&w, T_TEXTFORMAT);
    return w.sb.b;
}
