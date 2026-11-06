/* textfield.h - TextField state shared by the AVM2 API and the renderer:
 * rich text, field flags, layout (lines/runs/char positions), selection. */
#ifndef FLASHNT_TEXTFIELD_H
#define FLASHNT_TEXTFIELD_H

#include "richtext.h"
#include "player.h"

struct FontDef;

typedef struct TFLine {
    int    start, end;        /* characters [start, end), including the paragraph's '\r' */
    double x, y;              /* left of the first glyph / top of the line (twips, text space) */
    double width;             /* advance width of the visible characters */
    double ascent, descent, leading;
    double btop, bbot;        /* extent of the line's text boxes (twips, text space) */
    int    first_run, nruns;
    int    para_start;        /* first line of its paragraph */
} TFLine;

typedef struct TFRun {
    int    start, end;
    double x;                 /* x of the first character (twips, text space) */
    struct FontDef *font;
    double size;              /* twips */
    u32    color;
    int    underline;
    double letter_spacing;    /* twips */
    int    kerning;
} TFRun;

typedef struct TFLayout {
    TFLine *lines; int nlines, caplines;
    TFRun  *runs;  int nruns, capruns;
    double *cx;               /* x of each character (len + 1) */
    double *cw;               /* advance of each character */
    int    *cline;            /* line of each character (len + 1) */
    unsigned char *cmiss;     /* the font has no glyph for the character */
    double  text_w, text_h;   /* twips; text_w: the widest line (textWidth) */
    double  text_right;       /* right edge of the furthest line, margins and alignment included */
    int     len;
} TFLayout;

typedef struct TextFieldState {
    RichText rt;
    RTFmt    deffmt;                 /* defaultTextFormat (all fields set) */
    int      xmin, ymin, w, h;       /* bounds (twips); timeline fields may not start at 0,0 */
    int      box_flip;               /* createTextField with a negative width (1) / height (2): the box lies before the origin */
    int      html, wordwrap, multiline, border, background, password, selectable, input, embed;
    int      condense_white, always_show_selection, mouse_wheel, use_rich_clipboard;
    int      was_static;             /* DefineEditText WasStatic: not selectable -> mouse-transparent */
    int      aa_advanced, grid_fit;  /* antiAliasType "advanced"; gridFitType 0 none, 1 pixel, 2 subpixel */
    float    thickness, sharpness;
    int      autosize;               /* 0 none, 1 left, 2 center, 3 right */
    int      as_pending;             /* autoSize bounds are applied lazily, see tfs_apply_autosize */
    u32      bgcolor, bordercolor;
    int      max_chars;
    char    *restrict_chars;         /* NULL: anything */
    int      scroll_v;               /* 1-based first visible line */
    double   scroll_h;               /* twips */
    int      sel_begin, sel_end;     /* selection (sel_end is the caret) */
    int      has_sel;                /* AVM1: a selection exists (Selection.* report -1 otherwise) */
    int      ime_on, ime_start, ime_end; /* IME composition (preedit) range */
    char    *ime_text;               /* preedit text (UTF-8), committed on focus loss */
    int      swf_version;
    TFLayout lay; int lay_valid;
    char    *html_source;            /* htmlText as set, kept when a style sheet applies */
    int      restyle;
    void    *css;                    /* AVM1: style sheet data (SSData) while TextField.styleSheet is set */
    int      click_pos, click_mode;    /* last press: position and 0 char / 1 word / 2 line selection */                /* parse html_source again even if unchanged */
    void    *player;
    SWFMovie *movie;                 /* dictionary for embedded fonts (NULL: the main movie) */
    double   dev_sx, dev_sy;         /* the field's scale: device text is laid out in pixels at size * scaleY (0: 1) */
    double   dev_tx;                 /* the field's x translation (twips): device text line metrics report x in screen space / scaleX */
} TextFieldState;

void  tfs_init(TextFieldState *t, int swf_version);
void  tfs_csm(TextFieldState *t, const u8 *csm_body);           /* CSMTextSettings render settings */
void  tfs_free(TextFieldState *t);
void  tfs_invalidate(TextFieldState *t);
void  tfs_layout(Player *p, TextFieldState *t);               /* refresh t->lay if needed */
void  tfs_autosize(Player *p, TextFieldState *t);             /* apply autoSize to the bounds */
void  tfs_apply_autosize(Player *p, TextFieldState *t);       /* apply pending autoSize bounds (only on bounds reads and rendering) */
void  tfs_render(Player *p, TextFieldState *t, const SWFMatrix *wm, const SWFCxform *cx, int focused);
int   tfs_maxscroll_v(Player *p, TextFieldState *t);
int   tfs_bottom_scroll_v(Player *p, TextFieldState *t);
double tfs_maxscroll_h(Player *p, TextFieldState *t);
int   tfs_char_at_point(Player *p, TextFieldState *t, double x, double y);   /* field-local twips; -1 if none */
int   tfs_line_at_point(Player *p, TextFieldState *t, double x, double y);
int   tfs_caret_at_point(Player *p, TextFieldState *t, double x, double y); /* nearest insertion point */
double tfs_dev_vscale(TextFieldState *t);   /* device text: scaleY / scaleX for the reported heights */
void  tfs_line_metrics(Player *p, TextFieldState *t, int line, double out[6]);  /* x, width, height, ascent, descent, leading (px) */
void  tfs_ime_preedit(TextFieldState *t, const char *u8, int from, int to);   /* IME composition text (empty: composition ends) */
char *tfs_ime_take(TextFieldState *t);                        /* end the composition; its text (malloc'd) or NULL */
int   tfs_char_bounds(Player *p, TextFieldState *t, int i, double out[4]);      /* x, y, w, h (px) */

/* font lookup for a format (embedded fonts by name/style when embed is set) */
struct FontDef *tf_font_for(Player *p, const RTFmt *f, int embed);
struct FontDef *tf_font_for_movie(Player *p, SWFMovie *m, const RTFmt *f, int embed);

/* AVM2 binding */
TextFieldState *a2_textfield_state(DisplayObject *d);

#endif
