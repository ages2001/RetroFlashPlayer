/* richtext.h - TextField content model: text + format spans, Flash HTML in/out.
 *
 * Text is an array of code points; paragraphs end with '\r' (13), as in
 * Flash.  Every character belongs to exactly one span; a span carries a full
 * format.  RTFmt doubles as a TextFormat with optional fields (`has`). */
#ifndef FLASHNT_RICHTEXT_H
#define FLASHNT_RICHTEXT_H

#include "flashnt.h"

enum {
    RTF_FONT = 1 << 0, RTF_SIZE = 1 << 1, RTF_COLOR = 1 << 2, RTF_BOLD = 1 << 3, RTF_ITALIC = 1 << 4,
    RTF_UNDERLINE = 1 << 5, RTF_URL = 1 << 6, RTF_TARGET = 1 << 7, RTF_ALIGN = 1 << 8,
    RTF_LEFTMARGIN = 1 << 9, RTF_RIGHTMARGIN = 1 << 10, RTF_INDENT = 1 << 11, RTF_BLOCKINDENT = 1 << 12,
    RTF_LEADING = 1 << 13, RTF_LETTERSPACING = 1 << 14, RTF_KERNING = 1 << 15, RTF_BULLET = 1 << 16,
    RTF_TABSTOPS = 1 << 17, RTF_DISPLAY = 1 << 18,
    RTF_ALL = (1 << 19) - 1
};
enum { RT_ALIGN_LEFT = 0, RT_ALIGN_RIGHT = 1, RT_ALIGN_CENTER = 2, RT_ALIGN_JUSTIFY = 3, RT_ALIGN_START = 4, RT_ALIGN_END = 5 };
enum { RT_DISPLAY_BLOCK = 0, RT_DISPLAY_INLINE = 1, RT_DISPLAY_NONE = 2 };

#define RT_MAX_TABS 32
typedef struct RTFmt {
    u32    has;                    /* RTF_* fields present */
    char   font[256];
    double size;                   /* pixels */
    u32    color;                  /* 0xRRGGBB */
    u8     bold, italic, underline, kerning, bullet, align, display;
    double letter_spacing, left_margin, right_margin, indent, block_indent, leading;
    char   url[512], target[128];
    int    ntabs; double tabs[RT_MAX_TABS];
    int    img;                    /* an <IMG> placeholder character: rt_image(img) (0: none) */
} RTFmt;

/* <IMG> attributes (kept for htmlText; the images themselves are not loaded yet) */
typedef struct RTImg {
    char   src[512], id[128];
    double width, height, hspace, vspace;
    u8     has_w, has_h, has_hs, has_vs, has_id, align, check_policy_file;   /* align: 0 none, 1 left, 2 right */
} RTImg;
const RTImg *rt_image(int img);

typedef struct { int len; RTFmt f; } RTSpan;

typedef struct RichText {
    u32    *text; int len, cap;
    RTSpan *spans; int nspans, capspans;
} RichText;

void rt_fmt_clear(RTFmt *f);                         /* nothing set */
void rt_fmt_default(RTFmt *f);                       /* Times New Roman 12 black, every field set */
void rt_fmt_apply(RTFmt *dst, const RTFmt *src);     /* copy the fields src has */
int  rt_fmt_same(const RTFmt *a, const RTFmt *b);    /* span-merge equality (all formatting fields) */
void rt_fmt_intersect(RTFmt *acc, const RTFmt *f);   /* keep only fields equal in both */

void rt_init(RichText *rt);
void rt_free(RichText *rt);
void rt_set_plain(RichText *rt, const u32 *s, int n, const RTFmt *fmt);
void rt_replace(RichText *rt, int from, int to, const u32 *s, int n, const RTFmt *deffmt);
/* the same, the new text in format `use` */
void rt_replace_as(RichText *rt, int from, int to, const u32 *s, int n, const RTFmt *use, const RTFmt *deffmt);
void rt_set_format(RichText *rt, int from, int to, const RTFmt *f);
int  rt_get_format(const RichText *rt, int from, int to, RTFmt *out);   /* 0 if the range is empty */
const RTFmt *rt_format_at(const RichText *rt, int pos);                /* span covering pos (or last) */
void rt_copy(RichText *dst, const RichText *src);

/* HTML (Flash's subset).  `css` resolves a style-sheet selector ("p", ".cls")
   into format fields; it may be NULL. */
typedef int (*RTStyleFn)(void *ctx, const char *selector, RTFmt *out);
typedef struct { int multiline, condense_white, swf_version; RTStyleFn css; void *css_ctx; } RTHtmlOpts;
void  rt_from_html(RichText *rt, const char *html, const RTFmt *deffmt, const RTHtmlOpts *o);
char *rt_to_html(const RichText *rt);
char *rt_to_html_v(const RichText *rt, int swf_version);               /* malloc'd UTF-8 */

/* UTF-8 <-> code points */
int   rt_utf8_decode(const char *s, int n, u32 **out);   /* malloc'd, returns count */
char *rt_utf8_encode(const u32 *u, int n, int *outlen);  /* malloc'd, NUL terminated */

#endif
