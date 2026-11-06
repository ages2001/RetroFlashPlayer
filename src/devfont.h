/* devfont.h - built-in device fonts (sans / serif / mono, 1024-unit EM) */
#ifndef FLASHNT_DEVFONT_H
#define FLASHNT_DEVFONT_H

#include "flashnt.h"
#include "shape.h"

enum { DEVFONT_SANS = 0, DEVFONT_SERIF = 1, DEVFONT_MONO = 2 };

typedef struct DevFace {
    int family, style;                 /* style bit 0: bold */
    int em;                            /* units per EM (1024; the test font keeps its own) */
    int ascent, descent, leading;      /* EM units */
    int nglyphs;
    const u8 *table, *kern; int nkern;
    ShapeDef **shapes;                 /* lazily built outlines */
    const u8 *base;                    /* outline offsets are relative to this blob */
    char name[64];                     /* a font loaded from a TrueType file: its family name */
    int nhdmx; u8 hdmx_ppem[64]; u8 *hdmx_w;   /* 'hdmx' device widths: nhdmx rows of nglyphs pixels */
    const u8 *ttf; u32 ttf_size;       /* a face read from a font file: its data, */
    u16 *gid; u32 glyf, loca, loca_long;   /* each table entry's glyph id, the outline tables */
} DevFace;

int       devfont_family_for_name(const char *name);
int       devfont_add_ttf(const char *family, const u8 *ttf, size_t len, int bold);   /* a TrueType device font (family id or -1) */
DevFace  *devfont_get(int family, int bold);        /* NULL if the blob cannot be unpacked */
int       devfont_glyph(const DevFace *f, int code); /* glyph index or -1 */
int       devfont_code(const DevFace *f, int gi);
int       devfont_advance(const DevFace *f, int gi); /* EM units */
int       devfont_kerning(const DevFace *f, int left_code, int right_code);
ShapeDef *devfont_shape(DevFace *f, int gi);
/* harness option FLASHNT_DEVICE_LINUX=1: device text measured as the Linux
 * player does (whole-twip ascent/descent, advances rounded up to pixels, no kerning) */
int       devfont_linux_rules(void);
int       devfont_hdmx_advance(const DevFace *f, int gi, int ppem);   /* whole pixels, or -1 */

/* named device fonts from TrueType files (the test harness's [fonts]): the first
 * file's glyphs, then those the fallback files add (a font sort) */
DevFace  *devfont_register_ttf(const char *name, const char *const *paths, int npaths, int bold, int italic);
DevFace  *devfont_find_named(const char *name, int len, int bold, int italic);   /* case-insensitive exact name */
/* device fonts registered from font files (the Ruffle test harness's [fonts]);
   once any is registered, device text resolves its font as Ruffle does */
extern int g_devfont_registry;
DevFace  *devfont_load_ttf(const u8 *data, u32 size);   /* NULL when unreadable; the data must stay */
void      devfont_register(const char *family, int bold, int italic, DevFace *f);
void      devfont_set_default(int which, const char *names);   /* 0 sans, 1 serif, 2 typewriter; comma list */
DevFace  *devfont_lookup(const char *name, int bold, int italic, int *found);

#endif
