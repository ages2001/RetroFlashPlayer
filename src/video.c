/* video.c - video display objects.
 *
 * A Video shows either the frames of its DefineVideoStream (VideoFrame tags
 * on the timeline, selected by the display object's ratio as Flash does) or
 * the pictures of a NetStream attached by script (Video.attachNetStream /
 * AVM1 attachVideo).  Frames are decoded by vdec.c (H.263, Screen Video,
 * VP6); inter frames decode on from the nearest keyframe.  The picture is
 * scaled to the object's size (the stream's declared size, or the size a
 * script-created Video was made with). */
#include "chars.h"
#include "image.h"
#include "render.h"
#include "shape.h"
#include "reader.h"
#include "vdec.h"
#include "netstream.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    int codec, w, h, nframes, deblocking, smoothing;
    VDec *dec;
    int cur_frame;            /* the frame `dec` shows (-1 none) */
    /* all VideoFrame tags of this stream, indexed by frame number */
    const u8 **frames; size_t *lens; int nalloc;
} VideoDef;

static VideoDef *video_get(SWFMovie *m, SWFChar *ch) {
    VideoDef *v; Reader r; int flags;
    if (ch->parsed) return (VideoDef*)ch->parsed;
    v = (VideoDef*)calloc(1, sizeof(VideoDef));
    if (!v) return NULL;
    v->cur_frame = -1;
    if (ch->tagcode == -1 || !ch->data) { ch->parsed = v; return v; }   /* script-created (size set by the caller) */
    reader_init(&r, ch->data, ch->len);
    r_u16(&r);
    v->nframes = r_u16(&r); v->w = r_u16(&r); v->h = r_u16(&r);
    flags = r_u8(&r); v->codec = r_u8(&r);
    v->deblocking = (flags >> 1) & 7; v->smoothing = flags & 1;
    if (v->nframes > 0 && v->nframes < 200000) {
        v->frames = (const u8**)calloc((size_t)v->nframes, sizeof(u8*));
        v->lens = (size_t*)calloc((size_t)v->nframes, sizeof(size_t));
        v->nalloc = v->nframes;
    }
    /* collect VideoFrame tags from the whole movie (they live on timelines) */
    {
        extern void swf_collect_video_frames(SWFMovie *m, int stream_id, const u8 **frames, size_t *lens, int n);
        if (v->frames) swf_collect_video_frames(m, ch->id, v->frames, v->lens, v->nalloc);
    }
    ch->parsed = v;
    return v;
}

void video_free_parsed(void *p) {
    VideoDef *v = (VideoDef*)p;
    if (!v) return;
    vdec_free(v->dec); free(v->frames); free(v->lens); free(v);
}

/* a script-created Video (AVM2 new Video(w, h)): a private character */
SWFChar *video_new_char(SWFMovie *m, int w, int h) {
    SWFChar *ch = (SWFChar*)calloc(1, sizeof(SWFChar)); VideoDef *v;
    if (!ch) return NULL;
    ch->type = CHAR_VIDEO; ch->tagcode = -1; ch->id = -1; ch->movie = m;
    v = video_get(m, ch);
    if (v) { v->w = w; v->h = h; }
    return ch;
}
void video_size(SWFChar *ch, int *w, int *h) {
    VideoDef *v = ch && ch->type == CHAR_VIDEO ? video_get(ch->movie, ch) : NULL;
    *w = v ? v->w : 0; *h = v ? v->h : 0;
}
int video_bounds(SWFChar *ch, SWFRect *out) {
    int w, h;
    if (!ch || ch->type != CHAR_VIDEO) return 0;
    if (ch->tagcode != -1 && ch->data && ch->len >= 8) { w = ch->data[4] | (ch->data[5] << 8); h = ch->data[6] | (ch->data[7] << 8); }
    else video_size(ch, &w, &h);
    out->xmin = out->ymin = 0; out->xmax = w * 20; out->ymax = h * 20;
    return 1;
}

/* decode frames up to `frame` (inter frames need the ones since the last keyframe) */
static void video_seek(VideoDef *v, int frame) {
    int f, start;
    if (!v->frames || frame < 0) return;
    if (frame >= v->nalloc) frame = v->nalloc - 1;
    if (frame == v->cur_frame) return;
    if (!v->dec) { v->dec = vdec_create(v->codec, v->deblocking); if (!v->dec) return; }
    /* forward from the current frame when no keyframe is in between, else from the last keyframe */
    for (start = frame; start > 0; start--) if (v->frames[start] && vdec_is_keyframe(v->codec, v->frames[start], v->lens[start])) break;
    if (v->cur_frame >= start && v->cur_frame < frame) start = v->cur_frame + 1;
    for (f = start; f <= frame; f++) if (v->frames[f]) vdec_decode(v->dec, v->frames[f], v->lens[f]);
    v->cur_frame = frame;
}

/* the picture a Video object shows now, with the size to draw it at (px) */
static const Bitmap *video_picture(Player *p, DisplayObject *d, int *w, int *h) {
    VideoDef *v = video_get(d->ch->movie ? d->ch->movie : PMOV(p), d->ch);
    extern int ns_is_live(NetStream *s);
    *w = v ? v->w : 0; *h = v ? v->h : 0;
    if (d->vstream) { NetStream *s = (NetStream*)d->vstream; if (!ns_is_live(s)) { d->vstream = NULL; return NULL; } return d->vid_cleared ? NULL : ns_frame(s); }
    if (!v || !v->frames) return NULL;
    video_seek(v, d->ratio);
    return d->vid_cleared ? NULL : vdec_image(v->dec);
}
void video_dims(Player *p, DisplayObject *d, int *vw, int *vh) {
    int w, h; const Bitmap *b = d && d->ch ? video_picture(p, d, &w, &h) : NULL;
    *vw = b ? b->w : 0; *vh = b ? b->h : 0;
    if (b && d->vstream) { extern void ns_video_size(NetStream *s, int *w, int *h); ns_video_size((NetStream*)d->vstream, vw, vh); }
}

void video_render(Player *p, DisplayObject *d, const SWFMatrix *wm, const SWFCxform *cx) {
    ShapeDef s; ShapeEdge ed[4]; FillStyle fs; int i, w, h; const Bitmap *pic = video_picture(p, d, &w, &h);
    extern void render_set_direct_bitmap(const Bitmap *bm);
    int pw, ph;
    if (!pic || w <= 0 || h <= 0) return;   /* nothing decoded yet: Flash shows nothing */
    /* the shown part of the picture: VP6 codes whole macroblocks, the stream size crops them */
    pw = pic->w; ph = pic->h;
    if (d->vstream) { extern void ns_video_size(NetStream *s, int *w, int *h); ns_video_size((NetStream*)d->vstream, &pw, &ph); }
    else { if (pw > w) pw = w; if (ph > h) ph = h; }
    if (pw < 1 || ph < 1) return;
    memset(&s, 0, sizeof(s)); memset(&fs, 0, sizeof(fs)); swf_matrix_identity(&fs.matrix);
    fs.type = d->vid_smooth ? FILL_BITMAP_CLIP : FILL_BITMAP_CLIP_NS; fs.bitmap_id = -1;
    fs.matrix.sx = 20.0 * w / pw; fs.matrix.sy = 20.0 * h / ph;   /* picture px -> object twips */
    render_set_direct_bitmap(pic);
    s.fills = &fs; s.nfills = 1; s.edges = ed; s.nedges = 4; s.version = 3;
    for (i = 0; i < 4; i++) { memset(&ed[i], 0, sizeof(ed[i])); ed[i].fill1 = 1; }
    ed[0].x0 = 0; ed[0].y0 = 0; ed[0].x1 = w * 20; ed[0].y1 = 0;
    ed[1].x0 = w * 20; ed[1].y0 = 0; ed[1].x1 = w * 20; ed[1].y1 = h * 20;
    ed[2].x0 = w * 20; ed[2].y0 = h * 20; ed[2].x1 = 0; ed[2].y1 = h * 20;
    ed[3].x0 = 0; ed[3].y0 = h * 20; ed[3].x1 = 0; ed[3].y1 = 0;
    render_shape(p->surf, PMOV(p), &s, wm, cx, NULL);
    render_set_direct_bitmap(NULL);
}
