/* swf.h - SWF movie model: header, character dictionary, timelines.
 *
 * Define* tags are registered lazily: the dictionary stores a pointer into
 * the (decompressed) body plus a length, and the heavy parse (shape edges,
 * bitmap pixels) happens on first use and is cached.  This keeps load of a
 * 19 MB, 8000-shape movie fast and memory-light. */
#ifndef FLASHNT_SWF_H
#define FLASHNT_SWF_H

#include "flashnt.h"

/* tag codes we care about */
enum {
    TAG_End = 0, TAG_ShowFrame = 1, TAG_DefineShape = 2, TAG_PlaceObject = 4,
    TAG_RemoveObject = 5, TAG_DefineBits = 6, TAG_DefineButton = 7,
    TAG_JPEGTables = 8, TAG_SetBackgroundColor = 9, TAG_DefineFont = 10,
    TAG_DefineText = 11, TAG_DoAction = 12, TAG_DefineFontInfo = 13,
    TAG_DefineSound = 14, TAG_StartSound = 15, TAG_SoundStreamHead = 18,
    TAG_SoundStreamBlock = 19, TAG_DefineBitsLossless = 20,
    TAG_DefineBitsJPEG2 = 21, TAG_DefineShape2 = 22, TAG_PlaceObject2 = 26,
    TAG_RemoveObject2 = 28, TAG_DefineShape3 = 32, TAG_DefineText2 = 33,
    TAG_DefineButton2 = 34, TAG_DefineBitsJPEG3 = 35,
    TAG_DefineBitsLossless2 = 36, TAG_DefineEditText = 37,
    TAG_DefineSprite = 39, TAG_FrameLabel = 43, TAG_SoundStreamHead2 = 45,
    TAG_DefineMorphShape = 46, TAG_DefineFont2 = 48, TAG_ExportAssets = 56, TAG_ImportAssets = 57, TAG_ImportAssets2 = 71,
    TAG_DoInitAction = 59, TAG_DefineVideoStream = 60, TAG_VideoFrame = 61,
    TAG_DefineFontInfo2 = 62, TAG_FileAttributes = 69, TAG_PlaceObject3 = 70,
    TAG_CSMTextSettings = 74, TAG_DefineFont3 = 75, TAG_SymbolClass = 76,
    TAG_Metadata = 77, TAG_DoABC1 = 72, TAG_DoABC = 82, TAG_DefineShape4 = 83,
    TAG_DefineMorphShape2 = 84, TAG_DefineSceneAndFrameLabelData = 86,
    TAG_DefineBinaryData = 87, TAG_StartSound2 = 89, TAG_DefineBitsJPEG4 = 90, TAG_ScriptLimits = 65
};

enum {
    CHAR_NONE = 0, CHAR_SHAPE, CHAR_SPRITE, CHAR_BITMAP, CHAR_FONT,
    CHAR_TEXT, CHAR_EDITTEXT, CHAR_SOUND, CHAR_BUTTON, CHAR_MORPH,
    CHAR_VIDEO, CHAR_OTHER, CHAR_BINARY
};

typedef struct {
    int      id;
    int      type;
    int      tagcode;   /* original Define* code (shape version etc.) */
    const u8 *data;     /* tag body (after tag header) */
    size_t   len;
    void     *parsed;   /* cached parse result, type-specific */
    int       flags;    /* bit0: DoInitAction already executed */
    const u8 *info;     /* DefineFontInfo/2 body for DefineFont v1 */
    size_t    infolen;
    struct SWFMovie *movie;   /* the movie whose dictionary holds this character */
    const u8 *csm;            /* CSMTextSettings body for a text character (NULL: none) */
} SWFChar;

/* one control tag reference inside a timeline */
typedef struct {
    int      code;
    const u8 *data;
    size_t   len;
    int      done;      /* DoABC / SymbolClass: processed once per movie */
} TagRef;

/* a MovieClip definition: ordered control tags, split into frames by
 * ShowFrame.  frame_count counts ShowFrames. */
typedef struct {
    TagRef  *tags;
    int      ntags;
    int      cap;
    int      frame_count;
    int      hdr_frames, has_hdr;   /* frame count declared by the header / DefineSprite */
} Timeline;

typedef struct SWFMovie {
    u8        version;
    char     *url;             /* where it was loaded from (malloc'd, may be NULL) */
    char     *path;            /* file it was read from (malloc'd, may be NULL) */
    int       compressed;      /* 'C'WS */
    int       has_abc;         /* DoABC seen: ActionScript 3 movie (unsupported scripts) */
    size_t    file_len;        /* size of the SWF as stored (LoaderInfo.bytesTotal) */
    u32       hdr_len;         /* uncompressed length declared by the header */
    int       incomplete;      /* the header declares more data than there is: the movie never finishes loading */
    int       file_attr;       /* FileAttributes flags (bit 0: UseNetwork, 3: ActionScript3, 4: HasMetadata) */
    int       has_fa;          /* a FileAttributes tag was seen: its ActionScript3 flag alone picks the VM */
    double    framerate;
    int       framecount;
    SWFRect   stage;
    SWFColor  bg;

    u8       *body;            /* decompressed, owned */
    size_t    bodylen;

    SWFChar  *dict;            /* indexed [0..maxid] */
    int       maxid;

    Timeline  root;

    const u8 *jpeg_tables;     /* shared JPEGTables for DefineBits */
    size_t    jpeg_tables_len;

    /* ExportAssets: linkage name -> character id */
    struct { const char *name; int id; } *exports;
    int       nexports, capexports;
    int       a2_domain;       /* AVM2 ApplicationDomain id (0: not set = the main movie's domain) */
    int       a1mix;           /* an AVM1 movie running inside an AVM2 player (Loader content, AVM1Movie) */
    /* embedded font lookup names in tag order: a font's own name, DefineFontInfo names,
     * and the export names of characters defined by then (is_export) */
    struct SWFFontName { const char *name; int nlen; int id; unsigned char bold, italic, is_export; } *fnames;
    int       nfnames, capfnames;
} SWFMovie;

int       swf_find_export(SWFMovie *m, const char *name);

SWFMovie *swf_load_file(const char *path, int *err);
SWFMovie *swf_load_mem (const u8 *raw, size_t rawlen, int *err);
void      swf_free(SWFMovie *m);

SWFChar  *swf_get_char(SWFMovie *m, int id);
void      swf_free_parsed(SWFChar *ch);

/* parse a DefineSprite body into a fresh Timeline (caller owns tags array;
 * data pointers alias the movie body). */
void      swf_sprite_timeline(const SWFChar *sprite, Timeline *out);

void      timeline_init(Timeline *t);
void      timeline_push(Timeline *t, int code, const u8 *data, size_t len);
void      timeline_free(Timeline *t);

#endif
