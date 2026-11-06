/* chars_free.c - type-aware release of cached parses (ch->parsed) */
#include "swf.h"
#include "shape.h"
#include "image.h"
#include "audio.h"
#include <stdlib.h>

void font_free_parsed(void *p);
void morph_free_parsed(void *p);
void button_free_parsed(void *p);
void video_free_parsed(void *p);

void swf_free_parsed(SWFChar *ch) {
    if (!ch->parsed) return;
    switch (ch->type) {
    case CHAR_SHAPE:  shape_free((ShapeDef*)ch->parsed); break;
    case CHAR_SPRITE: timeline_free((Timeline*)ch->parsed); free(ch->parsed); break;
    case CHAR_BITMAP: bitmap_free((Bitmap*)ch->parsed); break;
    case CHAR_SOUND:  { SoundDef *s = (SoundDef*)ch->parsed; free(s->pcm); free(s); break; }
    case CHAR_FONT:   font_free_parsed(ch->parsed); break;
    case CHAR_MORPH:  morph_free_parsed(ch->parsed); break;
    case CHAR_BUTTON: button_free_parsed(ch->parsed); break;
    case CHAR_VIDEO:  video_free_parsed(ch->parsed); break;
    default: free(ch->parsed); break;
    }
    ch->parsed = NULL;
}
