#include "swf.h"
#include "reader.h"
#include "inflate.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int char_type_for(int code) {
    switch (code) {
    case TAG_DefineShape: case TAG_DefineShape2:
    case TAG_DefineShape3: case TAG_DefineShape4: return CHAR_SHAPE;
    case TAG_DefineSprite: return CHAR_SPRITE;
    case TAG_DefineBits: case TAG_DefineBitsJPEG2: case TAG_DefineBitsJPEG3:
    case TAG_DefineBitsJPEG4: case TAG_DefineBitsLossless:
    case TAG_DefineBitsLossless2: return CHAR_BITMAP;
    case TAG_DefineFont: case TAG_DefineFont2: case TAG_DefineFont3: return CHAR_FONT;
    case TAG_DefineText: case TAG_DefineText2: return CHAR_TEXT;
    case TAG_DefineEditText: return CHAR_EDITTEXT;
    case TAG_DefineSound: return CHAR_SOUND;
    case TAG_DefineButton: case TAG_DefineButton2: return CHAR_BUTTON;
    case TAG_DefineMorphShape: case TAG_DefineMorphShape2: return CHAR_MORPH;
    case TAG_DefineVideoStream: return CHAR_VIDEO;
    case TAG_DefineBinaryData: return CHAR_BINARY;
    default: return CHAR_NONE;
    }
}

static int tag_defines_id(int code) {
    return char_type_for(code) != CHAR_NONE;
}

static int is_control_tag(int code) {
    switch (code) {
    case TAG_PlaceObject: case TAG_PlaceObject2: case TAG_PlaceObject3:
    case TAG_RemoveObject: case TAG_RemoveObject2:
    case TAG_ShowFrame: case TAG_DoAction: case TAG_DoInitAction: case TAG_StartSound: case TAG_VideoFrame:
    case TAG_StartSound2: case TAG_FrameLabel: case TAG_SetBackgroundColor:
    case TAG_SoundStreamHead: case TAG_SoundStreamHead2:
    case TAG_SoundStreamBlock: case TAG_End:
    case TAG_DoABC: case TAG_DoABC1: case TAG_SymbolClass: case TAG_DefineSceneAndFrameLabelData:
    case TAG_ImportAssets: case TAG_ImportAssets2: case TAG_ScriptLimits:
        return 1;
    default: return 0;
    }
}

void timeline_init(Timeline *t) {
    t->tags = NULL; t->ntags = 0; t->cap = 0; t->frame_count = 0; t->hdr_frames = 0; t->has_hdr = 0;
}
void timeline_push(Timeline *t, int code, const u8 *data, size_t len) {
    if (t->ntags == t->cap) {
        int nc = t->cap ? t->cap * 2 : 64;
        TagRef *nt = (TagRef*)realloc(t->tags, (size_t)nc * sizeof(TagRef));
        if (!nt) return;
        t->tags = nt; t->cap = nc;
    }
    t->tags[t->ntags].code = code;
    t->tags[t->ntags].data = data;
    t->tags[t->ntags].len  = len;
    t->tags[t->ntags].done = 0;
    t->ntags++;
    if (code == TAG_ShowFrame) t->frame_count++;
}
void timeline_free(Timeline *t) {
    free(t->tags); timeline_init(t);
}

/* iterate the raw tag stream starting at r; invoke per tag.
 * Returns on End tag or EOF. */
typedef void (*TagFn)(void *ctx, int code, const u8 *body, size_t len);

static void walk_tags(Reader *r, TagFn fn, void *ctx) {
    for (;;) {
        u16 th;
        int code; size_t length;
        const u8 *body;
        reader_align(r);
        if (r->pos + 2 > r->len) break;
        th = r_u16(r);
        code = th >> 6;
        length = th & 0x3f;
        if (length == 0x3f) length = r_u32(r);
        if (r->pos + length > r->len) length = r->len - r->pos;
        body = r->buf + r->pos;
        fn(ctx, code, body, length);
        r->pos += length;
        if (code == TAG_End) break;
    }
}

typedef struct { SWFMovie *m; int pass; } LoadCtx;

static void pass_maxid(void *vctx, int code, const u8 *body, size_t len) {
    LoadCtx *c = (LoadCtx*)vctx;
    if (tag_defines_id(code) && len >= 2) {
        int id = body[0] | (body[1] << 8);
        if (id > c->m->maxid) c->m->maxid = id;
    }
    (void)len;
}

static void add_fname(SWFMovie *m, const char *name, int nlen, int id, int bold, int italic, int is_export) {
    if (m->nfnames == m->capfnames) {
        int nc = m->capfnames ? m->capfnames * 2 : 32; void *nf = realloc(m->fnames, (size_t)nc * sizeof(*m->fnames));
        if (!nf) return;
        m->fnames = (struct SWFFontName*)nf; m->capfnames = nc;
    }
    m->fnames[m->nfnames].name = name; m->fnames[m->nfnames].nlen = nlen; m->fnames[m->nfnames].id = id;
    m->fnames[m->nfnames].bold = (unsigned char)bold; m->fnames[m->nfnames].italic = (unsigned char)italic; m->fnames[m->nfnames].is_export = (unsigned char)is_export;
    m->nfnames++;
}
static void pass_build(void *vctx, int code, const u8 *body, size_t len) {
    LoadCtx *c = (LoadCtx*)vctx;
    SWFMovie *m = c->m;
    /* embedded font names, in the order Flash registers them */
    if ((code == TAG_DefineFont2 || code == TAG_DefineFont3) && len >= 5 && (size_t)5 + body[4] <= len) {
        int nl = body[4]; while (nl > 0 && body[4 + nl] == 0) nl--;
        add_fname(m, (const char*)body + 5, nl, body[0] | (body[1] << 8), body[2] & 1, (body[2] >> 1) & 1, 0);
    } else if ((code == TAG_DefineFontInfo || code == TAG_DefineFontInfo2) && len >= 4 && (size_t)4 + body[2] <= len) {
        int id = body[0] | (body[1] << 8), nl = body[2];
        if (id <= m->maxid && m->dict[id].type == CHAR_FONT) {
            const u8 *fb = m->dict[id].data; int v1 = m->dict[id].tagcode == TAG_DefineFont, fl = body[3 + nl];
            while (nl > 0 && body[2 + nl] == 0) nl--;
            /* the font's own style counts, not the info's (DefineFont has none of its own) */
            add_fname(m, (const char*)body + 3, nl, id, v1 ? (fl >> 1) & 1 : (fb && m->dict[id].len > 2 ? fb[2] & 1 : 0), v1 ? (fl >> 2) & 1 : (fb && m->dict[id].len > 2 ? (fb[2] >> 1) & 1 : 0), 0);
        }
    } else if (code == TAG_ExportAssets && len >= 2) {
        int n = body[0] | (body[1] << 8), i; size_t pos = 2;
        for (i = 0; i < n && pos + 2 < len; i++) {
            int id = body[pos] | (body[pos + 1] << 8); const char *name = (const char*)body + pos + 2; size_t nl = strlen(name);
            if (id <= m->maxid && m->dict[id].type) add_fname(m, name, (int)nl, id, 0, 0, 1);
            pos += 2 + nl + 1;
        }
    }
    if (tag_defines_id(code) && len >= 2) {
        int id = body[0] | (body[1] << 8);
        if (id >= 0 && id <= m->maxid) {
            SWFChar *ch = &m->dict[id];
            ch->id = id;
            ch->type = char_type_for(code);
            ch->tagcode = code;
            ch->data = body;
            ch->len = len;
            ch->parsed = NULL;
        }
    } else if ((code == TAG_DefineFontInfo || code == TAG_DefineFontInfo2) && len >= 2) {
        int id = body[0] | (body[1] << 8);
        if (id >= 0 && id <= m->maxid) { m->dict[id].info = body; m->dict[id].infolen = len; }
    } else if (code == TAG_ExportAssets && len >= 2) {
        int n = body[0] | (body[1] << 8), i;
        size_t pos = 2;
        for (i = 0; i < n && pos + 2 < len; i++) {
            int id = body[pos] | (body[pos + 1] << 8);
            const char *name = (const char*)body + pos + 2;
            size_t nl = strlen(name);
            pos += 2 + nl + 1;
            if (m->nexports == m->capexports) {
                int nc = m->capexports ? m->capexports * 2 : 64;
                void *ne = realloc(m->exports, (size_t)nc * sizeof(*m->exports));
                if (!ne) break;
                m->exports = ne; m->capexports = nc;
            }
            m->exports[m->nexports].name = name;
            m->exports[m->nexports].id = id;
            m->nexports++;
        }
    } else if (code == TAG_CSMTextSettings && len >= 12) {
        int id = body[0] | (body[1] << 8);
        if (id >= 0 && id <= m->maxid) m->dict[id].csm = body;
    } else if (code == TAG_FileAttributes && len >= 1) {
        m->file_attr = body[0]; m->has_fa = 1;
        if (body[0] & 8) m->has_abc = 1;   /* the ActionScript 3 flag picks AVM2, with or without code */
    } else if (code == TAG_DoABC || code == TAG_DoABC1) {
        if (!m->has_fa) m->has_abc = 1;   /* with FileAttributes, DoABC in an AVM1 movie is ignored */
    } else if (code == TAG_JPEGTables) {
        m->jpeg_tables = body; m->jpeg_tables_len = len;
    } else if (code == TAG_SetBackgroundColor && len >= 3 && m->bg.a == 254) {   /* the first one (254: not seen yet) */
        m->bg.r = body[0]; m->bg.g = body[1]; m->bg.b = body[2]; m->bg.a = 255;
    }
    if (is_control_tag(code))
        timeline_push(&m->root, code, body, len);
}

void swf_sprite_timeline(const SWFChar *sprite, Timeline *out) {
    Reader r;
    timeline_init(out);
    /* sprite body: u16 spriteID, u16 framecount, then tag list */
    if (sprite->len < 4) return;
    out->hdr_frames = sprite->data[2] | (sprite->data[3] << 8); out->has_hdr = 1;
    reader_init(&r, sprite->data + 4, sprite->len - 4);
    for (;;) {
        u16 th; int code; size_t length; const u8 *body;
        reader_align(&r);
        if (r.pos + 2 > r.len) break;
        th = r_u16(&r);
        code = th >> 6;
        length = th & 0x3f;
        if (length == 0x3f) length = r_u32(&r);
        if (r.pos + length > r.len) length = r.len - r.pos;
        body = r.buf + r.pos;
        if (is_control_tag(code))
            timeline_push(out, code, body, length);
        r.pos += length;
        if (code == TAG_End) break;
    }
}

int swf_find_export(SWFMovie *m, const char *name) {
    int i;
    for (i = 0; i < m->nexports; i++)
        if (strcmp(m->exports[i].name, name) == 0) return m->exports[i].id;
    return -1;
}

SWFChar *swf_get_char(SWFMovie *m, int id) {
    if (id < 0 || id > m->maxid) return NULL;
    if (m->dict[id].type == CHAR_NONE) return NULL;
    return &m->dict[id];
}

SWFMovie *swf_load_mem(const u8 *raw, size_t rawlen, int *err) {
    SWFMovie *m;
    u8 sig0, sig1, sig2, ver;
    u32 flen;
    size_t ubodylen;
    Reader r;
    LoadCtx ctx;

    if (err) *err = FLASHNT_OK;
    if (rawlen < 8) { if (err) *err = FLASHNT_EFMT; return NULL; }
    sig0 = raw[0]; sig1 = raw[1]; sig2 = raw[2]; ver = raw[3];
    flen = raw[4] | (raw[5]<<8) | (raw[6]<<16) | ((u32)raw[7]<<24);
    if (sig1 != 'W' || sig2 != 'S') { if (err) *err = FLASHNT_EFMT; return NULL; }
    if (flen < 8) { if (err) *err = FLASHNT_EFMT; return NULL; }   /* Flash refuses a movie whose header length is 0 */

    m = (SWFMovie*)calloc(1, sizeof(SWFMovie));
    if (!m) { if (err) *err = FLASHNT_ENOMEM; return NULL; }
    m->version = ver;
    m->file_len = rawlen;
    m->bg.r = m->bg.g = m->bg.b = 255; m->bg.a = 254;   /* white until a SetBackgroundColor tag */
    timeline_init(&m->root);

    /* body = everything after the 8-byte header, uncompressed */
    ubodylen = (flen >= 8) ? (flen - 8) : 0;
    if (sig0 == 'F') {                 /* FWS: already uncompressed */
        m->compressed = 0;
        if (ubodylen == 0 || ubodylen > rawlen - 8) ubodylen = rawlen - 8;
        m->body = (u8*)malloc(ubodylen);
        if (!m->body) { free(m); if (err) *err = FLASHNT_ENOMEM; return NULL; }
        memcpy(m->body, raw + 8, ubodylen);
        m->bodylen = ubodylen;
    } else if (sig0 == 'C') {          /* CWS: zlib compressed body */
        long got;
        m->compressed = 1;
        if (ubodylen == 0) { free(m); if (err) *err = FLASHNT_EFMT; return NULL; }
        m->body = (u8*)malloc(ubodylen);
        if (!m->body) { free(m); if (err) *err = FLASHNT_ENOMEM; return NULL; }
        got = swf_inflate_zlib(raw + 8, rawlen - 8, m->body, ubodylen);
        if (got < 0) { free(m->body); free(m); if (err) *err = FLASHNT_EFMT; return NULL; }
        m->bodylen = (size_t)got;
    } else if (sig0 == 'Z') {          /* ZWS: u32 compressed len, 5 byte props, LZMA stream */
        long got;
        m->compressed = 2;
        if (ubodylen == 0 || rawlen < 17) { free(m); if (err) *err = FLASHNT_EFMT; return NULL; }
        m->body = (u8*)malloc(ubodylen);
        if (!m->body) { free(m); if (err) *err = FLASHNT_ENOMEM; return NULL; }
        got = swf_lzma_decode(raw + 12, raw + 17, rawlen - 17, m->body, ubodylen);
        if (got < 0) { free(m->body); free(m); if (err) *err = FLASHNT_EFMT; return NULL; }
        m->bodylen = (size_t)got;
    } else {
        free(m); if (err) *err = FLASHNT_EFMT; return NULL;
    }

    m->hdr_len = flen;
    m->incomplete = (size_t)flen > m->bodylen + 8;
    /* header fields from body */
    reader_init(&r, m->body, m->bodylen);
    r_rect(&r, &m->stage);
    m->framerate = r_u16(&r) / 256.0;
    m->framecount = r_u16(&r);

    /* pass 1: find max character id */
    ctx.m = m; ctx.pass = 1;
    {
        Reader r2 = r;
        walk_tags(&r2, pass_maxid, &ctx);
    }
    m->dict = (SWFChar*)calloc((size_t)m->maxid + 1, sizeof(SWFChar));
    if (!m->dict) { free(m->body); free(m); if (err) *err = FLASHNT_ENOMEM; return NULL; }

    /* pass 2: register characters + build root timeline */
    ctx.pass = 2;
    walk_tags(&r, pass_build, &ctx);
    { int i; for (i = 0; i <= m->maxid; i++) m->dict[i].movie = m; }
    m->root.hdr_frames = m->framecount; m->root.has_hdr = 1;

    return m;
}

SWFMovie *swf_load_file(const char *path, int *err) {
    FILE *f;
    long n;
    u8 *buf;
    SWFMovie *m;
    if (err) *err = FLASHNT_EIO;
    f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    buf = (u8*)malloc((size_t)n);
    if (!buf) { fclose(f); if (err) *err = FLASHNT_ENOMEM; return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    m = swf_load_mem(buf, (size_t)n, err);
    free(buf);
    if (m) {
        /* file: URL (relative paths: just the file name, as in a site root) */
        size_t l = strlen(path); const char *b = path + l;
        m->path = (char*)malloc(l + 1); if (m->path) strcpy(m->path, path);
        m->url = (char*)malloc(l + 16);
        if (m->url) {
            if (path[0] == '/') sprintf(m->url, "file://%s", path);
            else if (l > 2 && path[1] == ':') { char *q; sprintf(m->url, "file:///%s", path); for (q = m->url; *q; q++) if (*q == '\\') *q = '/'; }
            else { while (b > path && b[-1] != '/' && b[-1] != '\\') b--; sprintf(m->url, "file:///%s", b); }
        }
    }
    return m;
}

void swf_free(SWFMovie *m) {
    int i;
    if (!m) return;
    if (m->dict) {
        for (i = 0; i <= m->maxid; i++)
            if (m->dict[i].parsed) swf_free_parsed(&m->dict[i]);
        free(m->dict);
    }
    timeline_free(&m->root);
    free(m->exports);
    free(m->fnames);
    free(m->body); free(m->url); free(m->path);
    free(m);
}

/* gather VideoFrame tags for a stream from the root timeline and all sprites */
static void collect_from(const Timeline *tl, int stream_id, const u8 **frames, size_t *lens, int n) {
    int i;
    for (i = 0; i < tl->ntags; i++) {
        const TagRef *t = &tl->tags[i];
        if (t->code == TAG_VideoFrame && t->len >= 4) {
            int sid = t->data[0] | (t->data[1] << 8), fn = t->data[2] | (t->data[3] << 8);
            if (sid == stream_id && fn >= 0 && fn < n) { frames[fn] = t->data + 4; lens[fn] = t->len - 4; }
        }
    }
}
void swf_collect_video_frames(SWFMovie *m, int stream_id, const u8 **frames, size_t *lens, int n) {
    int i;
    collect_from(&m->root, stream_id, frames, lens, n);
    for (i = 0; i <= m->maxid; i++) {
        if (m->dict[i].type == CHAR_SPRITE) {
            Timeline tl; timeline_init(&tl);
            swf_sprite_timeline(&m->dict[i], &tl);
            collect_from(&tl, stream_id, frames, lens, n);
            timeline_free(&tl);
        }
    }
}
