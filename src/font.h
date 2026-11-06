/* font.h - fonts for text rendering: DefineFont/2/3 characters and the
 * built-in device fonts, behind one FontDef type. */
#ifndef FLASHNT_FONT_H
#define FLASHNT_FONT_H

#include "player.h"
#include "shape.h"
#include "devfont.h"

typedef struct FontDef {
    int       version;        /* 1, 2, 3 */
    int       nglyphs;
    ShapeDef **glyphs;        /* parsed lazily */
    const u8 **glyph_data; size_t *glyph_len;
    u16      *codes;          /* char code per glyph */
    u16      *advance;        /* per glyph in font units (u16 in the tag), may be NULL */
    int       ascent, descent, leading;
    int       has_layout;
    double    scale;          /* units per EM: 1024 (v1/2) or 20480 (v3) */
    char      name[64];
    int       bold, italic;
    DevFace  *dev;            /* built-in device font (glyphs from devfont.c) */
    int       slant;          /* draw slanted (italic device text) */
    int       nkern;          /* DefineFont2/3 kerning records (font units) */
    u16      *kern_l, *kern_r; s16 *kern_adj;
} FontDef;

FontDef  *font_get(SWFMovie *m, int id);                  /* DefineFont character (NULL if none) */
FontDef  *font_device(int family, int bold, int italic);   /* built-in device font */
FontDef  *font_device_by_name(const char *name, int bold, int italic);
FontDef  *fallback_font(SWFMovie *m);
ShapeDef *font_glyph(FontDef *f, int idx);
int       font_glyph_for_code(FontDef *f, int code);
double    glyph_advance(FontDef *f, int gi, double height);
int       font_kerning(const FontDef *f, int left, int right);   /* embedded kerning adjustment, font units */   /* twips for a glyph `height` twips high */
void      draw_glyph(Player *p, FontDef *f, int gi, const SWFMatrix *wm, double x, double y, double height, SWFColor color, const SWFCxform *cx);

#endif
