/* chars.h - renderers for non-shape characters (text, morph, button). */
#ifndef FLASHNT_CHARS_H
#define FLASHNT_CHARS_H

#include "player.h"

void text_render  (Player *p, DisplayObject *d, const SWFMatrix *wm, const SWFCxform *cx);
void morph_render (Player *p, DisplayObject *d, const SWFMatrix *wm, const SWFCxform *cx);
void button_render(Player *p, DisplayObject *d, const SWFMatrix *wm, const SWFCxform *cx);
void video_render (Player *p, DisplayObject *d, const SWFMatrix *wm, const SWFCxform *cx);
/* createTextField text on an empty clip */
void text_render_dynamic(Player *p, MovieClip *c, const SWFMatrix *wm, const SWFCxform *cx);

/* AVM2 TextField description (filled by avm2_flash.c) */
typedef struct A2TextInfo {
    const char *text;          /* UTF-8 */
    int w, h;                  /* twips (bounds from 0,0) */
    int html, wordwrap, multiline, border, background, autosize, password;
    u32 color, bgcolor, bordercolor;
    int size;                  /* twips */
    int align;                 /* 0 left 1 right 2 center 3 justify */
    int font_id;               /* -1: by name/fallback */
    const char *font_name;
    int leading, indent, left_margin, right_margin;
    int bold, italic;
} A2TextInfo;
int  a2_text_info(DisplayObject *d, A2TextInfo *out);     /* 1 if d is an AVM2 text field */
void text_render_a2(Player *p, DisplayObject *d, const A2TextInfo *ti, const SWFMatrix *wm, const SWFCxform *cx);
void text_measure_a2(Player *p, const A2TextInfo *ti, double *w, double *h, int *lines);
int  text_lines_a2(Player *p, const A2TextInfo *ti, int *starts, int *ends, int max);   /* byte spans of each laid-out line */
void edittext_defaults(SWFChar *ch, A2TextInfo *out);     /* DefineEditText fields -> info */

/* edit text helpers */
int  edittext_editable(SWFChar *ch);
void edittext_get(Player *p, DisplayObject *d, char *buf, size_t cap);
void edittext_set(Player *p, DisplayObject *d, const char *text);

/* button hit-test + event dispatch (mouse in stage px) */
int  button_mouse(Player *p, DisplayObject *d, const SWFMatrix *wm, int mx, int my, int down, int changed);

#endif
