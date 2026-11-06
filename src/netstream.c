/* netstream.c - progressive FLV playback shared by AVM1 and AVM2 NetStream.
 *
 * Follows Ruffle's core/src/streams.rs: play() queues a download (it
 * completes after the frame's scripts), the stream ticks with the player
 * and walks the FLV tags by timestamp: script tags (onMetaData, ...) go to
 * the VM, video tags are decoded (vdec), audio tags feed the stream's
 * sound.  Status events: Play.Start, Buffer.Full / Flush / Empty, Play.Stop,
 * Seek.Notify (and for AVM2 SeekStart.Notify / Seek.Complete), Pause.Notify. */
#include "netstream.h"
#include "audio.h"
#include "fetch.h"
#include "vdec.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static NetStream **g_active; static int g_nactive, g_capactive;
static NetStream **g_all; static int g_nall, g_capall;

static void list_add(NetStream ***l, int *n, int *cap, NetStream *s) {
    int i;
    for (i = 0; i < *n; i++) if ((*l)[i] == s) return;
    if (*n == *cap) { int nc = *cap ? *cap * 2 : 8; NetStream **nl = (NetStream**)realloc(*l, (size_t)nc * sizeof(NetStream*)); if (!nl) return; *l = nl; *cap = nc; }
    (*l)[(*n)++] = s;
}
static void list_remove(NetStream **l, int *n, NetStream *s) { int i; for (i = 0; i < *n; i++) if (l[i] == s) { memmove(l + i, l + i + 1, (size_t)(*n - i - 1) * sizeof(NetStream*)); (*n)--; return; } }

static void activate(NetStream *s) { list_add(&g_active, &g_nactive, &g_capactive, s); }
static void deactivate(NetStream *s) { list_remove(g_active, &g_nactive, s); }

NetStream *ns_new(Player *p, int avm2, const NSCallbacks *cb, void *ud) {
    NetStream *s = (NetStream*)calloc(1, sizeof(NetStream));
    if (!s) return NULL;
    s->player = p; s->avm2 = avm2; s->cb = *cb; s->ud = ud;
    s->buffer_time = 0.1; s->volume = 100;
    s->expected_finished = 0; s->expected_len = 0;
    list_add(&g_all, &g_nall, &g_capall, s);
    return s;
}
static void audio_reset(NetStream *s) {
    audio_xstream_stop(s->player, s);
    if (s->mp3) { mp3s_free(s->mp3); s->mp3 = NULL; }
    if (s->aac) { aac_free((AACDec*)s->aac); s->aac = NULL; }
    s->audio_pos = 0;
}
/* an FLV audio tag: decoded and queued on the stream's voice */
static const int flv_rates[4] = { 5512, 11025, 22050, 44100 };
static void audio_tag(NetStream *s, const u8 *d, u32 n) {
    int fmt, rate, bits16, stereo, frames = 0; s16 *pcm = NULL;
    if (n < 2) return;
    fmt = d[0] >> 4; rate = flv_rates[(d[0] >> 2) & 3]; bits16 = (d[0] >> 1) & 1; stereo = d[0] & 1;
    d++; n--;
    switch (fmt) {
    case 0: case 3: frames = decode_pcm(d, n, bits16, stereo, 0, &pcm); break;
    case 1: frames = decode_adpcm(d, n, stereo, &pcm); break;
    case 2: case 14:
        if (!s->mp3) s->mp3 = mp3s_new();
        frames = mp3s_decode(s->mp3, d, n, &rate, &stereo, &pcm);
        break;
    case 7: case 8: frames = decode_g711(d, n, fmt == 7, &pcm) / (stereo ? 2 : 1); rate = 8000; break;   /* G.711 is 8 kHz */
    case 10:   /* AAC: a packet type byte, then the AudioSpecificConfig or a raw frame */
        if (n < 1) break;
        if (!s->aac) s->aac = aac_new();
        if (!s->aac) break;
        if (d[0] == 0) { aac_config((AACDec*)s->aac, d + 1, n - 1); break; }
        { int ch = 1; frames = aac_decode((AACDec*)s->aac, d + 1, n - 1, &pcm, &rate, &ch); stereo = ch == 2; }
        break;
    default: break;   /* Nellymoser, Speex: not decoded */
    }
    if (frames > 0 && pcm) audio_xstream_push(s->player, s, pcm, frames, rate, stereo);
    if (s->volume != 100) audio_xstream_volume(s->player, s, s->volume);
    free(pcm);
}
static void reset_video(NetStream *s) { if (s->vdec) { vdec_free(s->vdec); s->vdec = NULL; } s->vcodec = 0; s->frame_id = 0; }
static void reset_source(NetStream *s) {
    free(s->buf); s->buf = NULL; s->len = 0;
    s->offset = 0; s->preload_offset = 0; s->stream_time = 0;
    s->sniffed = 0; s->has_seek = 0;
    s->expected_finished = 0; s->expected_len = 0;
    s->generation++;
    reset_video(s);
    audio_reset(s);
    if (s->cb.audio_reset) s->cb.audio_reset(s->ud);
}
void ns_free(NetStream *s) {
    if (!s) return;
    deactivate(s); list_remove(g_all, &g_nall, s);
    audio_reset(s);
    free(s->buf); free(s->url); reset_video(s); free(s);
}

static void status(NetStream *s, const char *code, const char *level, const char *desc) {
    if (s->cb.status) s->cb.status(s->ud, code, level ? level : "status", desc);
}

/* ---------------------------------------------------------------- */
/* FLV                                                              */

typedef struct { int type; s32 ts; const u8 *data; u32 size; size_t next; } FTag;
static int flv_tag(NetStream *s, size_t pos, FTag *t) {
    const u8 *b = s->buf;
    if (!b || pos + 4 + 11 > s->len) return 1;
    b += pos + 4;
    t->type = b[0] & 0x1f;
    t->size = ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
    t->ts = (s32)(((u32)b[7] << 24) | ((u32)b[4] << 16) | ((u32)b[5] << 8) | b[6]);
    t->data = b + 11;
    if (pos + 15 + t->size > s->len) return 1;
    t->next = pos + 15 + t->size;
    return 0;
}
static int sniff(NetStream *s) {
    if (s->preload_offset > 0) return 0;
    if (!s->buf || s->len < 3) return 0;
    if (s->buf[0] == 'F' && s->buf[1] == 'L' && s->buf[2] == 'V') {
        u32 off;
        if (s->len < 9) return 0;
        off = ((u32)s->buf[5] << 24) | ((u32)s->buf[6] << 16) | ((u32)s->buf[7] << 8) | s->buf[8];
        if (off < 9) off = 9;
        s->offset = s->preload_offset = off; s->sniffed = 1;
        return 1;
    }
    s->preload_offset = 3;   /* unrecognized: never sniffed again */
    return 0;
}

static void script_tag(NetStream *s, const FTag *t) {
    const u8 *p = t->data, *end = t->data + t->size; int gen = s->generation;
    while (p + 3 <= end && *p == 2) {
        u32 n = ((u32)p[1] << 8) | p[2]; char name[256]; const u8 *val;
        p += 3;
        if (p + n > end) break;
        memcpy(name, p, n < 255 ? n : 255); name[n < 255 ? n : 255] = 0;
        p += n; val = p;
        if (!s->cb.script) break;
        p = s->cb.script(s->ud, name, val, end);
        if (!p || s->generation != gen) break;
    }
}

static void video_tag(NetStream *s, const FTag *t) {
    int codec, hdr = 1;
    if (t->size < 1) return;
    codec = t->data[0] & 15;
    if ((t->data[0] >> 4) == 5) { s->frame_id++; return; }   /* command frame */
    if (codec == VCODEC_VP6 || codec == VCODEC_VP6A) hdr = 2;   /* size adjustment byte */
    if (codec == VCODEC_VP6A) hdr = 2;
    if (!s->vdec || s->vcodec != codec) { reset_video(s); s->vdec = vdec_create(codec, VDEBLOCK_PACKET); s->vcodec = codec; }
    if (s->vdec && t->size > (u32)hdr) {
        const u8 *d = t->data + hdr; size_t n = t->size - (size_t)hdr;
        if (hdr == 2) s->vadj = t->data[1];
        if (vdec_decode(s->vdec, d, n)) { s->has_frame = 1; s->frame_version++; }
    }
    s->frame_id++;
}

static void execute_seek(NetStream *s, double offset) {
    size_t pos; FTag t; int back;
    if (!s->avm2 && s->ended) {
        /* AVM1: a seek after Play.Stop starts playback again (Gnash NetStream-SquareTest) */
        s->ended = 0; s->empty_pending = 0; s->playing = 1;
        status(s, "NetStream.Buffer.Full", "status", NULL);
        status(s, "NetStream.Seek.Complete", "status", NULL);
    } else status(s, "NetStream.Seek.Notify", "status", NULL);
    if (!s->sniffed && !sniff(s)) return;
    if (s->stream_time == offset) return;
    if (s->cb.audio_reset) s->cb.audio_reset(s->ud);
    audio_reset(s);
    pos = s->offset;
    back = s->stream_time > offset;
    for (;;) {
        size_t old;
        if (back) {
            u32 prev;
            if (pos + 4 > s->len) break;
            prev = ((u32)s->buf[pos] << 24) | ((u32)s->buf[pos + 1] << 16) | ((u32)s->buf[pos + 2] << 8) | s->buf[pos + 3];
            if (prev == 0 || (size_t)prev + 4 > pos) break;
            pos -= prev + 4;
        }
        old = pos;
        if (flv_tag(s, pos, &t)) break;
        if (!back) pos = t.next;
        s->stream_time = t.ts;
        if ((back && t.ts > offset) || (!back && t.ts < offset)) continue;
        if (t.type == 9 && t.size > 0 && (t.data[0] >> 4) == 1) { pos = old; break; }   /* keyframe */
    }
    s->offset = pos;
    if (s->avm2) status(s, "NetStream.Seek.Complete", "status", "Seek Complete -1");
}

static void tick(NetStream *s, double dt) {
    double max_time; int underrun = 0, lookahead_audio = 5, lookahead = 0, gen, any = 0;
    size_t pos;
    if (s->has_seek) { s->has_seek = 0; execute_seek(s, s->seek_time); }
    if (!s->avm2 && s->empty_pending) {
        s->empty_pending = 0; s->playing = 0;
        status(s, "NetStream.Buffer.Empty", "status", NULL);
        deactivate(s);
        return;
    }
    if (!s->playing) { deactivate(s); return; }
    if (!s->sniffed && !sniff(s)) return;
    if (!s->avm2 && s->full_pending && !s->loading) { s->full_pending = 0; gen = s->generation; status(s, "NetStream.Buffer.Full", "status", NULL); if (s->generation != gen || !s->playing) return; }
    if (!s->avm2 && s->ended) return;
    gen = s->generation;
    max_time = s->stream_time + dt;
    pos = s->offset;
    for (;;) {
        FTag t; int preload;
        if (flv_tag(s, pos, &t)) { if (!lookahead) underrun = 1; break; }
        /* AVM1: a jump in the timestamps (a "time warp") is played through at once */
        if (!s->avm2 && !any && t.ts > max_time + 1000.0) { s->stream_time = t.ts; max_time = t.ts + dt; }
        any = 1;
        lookahead = t.ts >= max_time;
        if (!lookahead && !s->avm2) s->last_ts = t.ts;
        if (lookahead && lookahead_audio == 0) break;
        preload = t.next >= s->preload_offset;
        (void)preload;
        if (t.type == 8) {
            if (lookahead) lookahead_audio--;
            if (pos >= s->audio_pos) { audio_tag(s, t.data, t.size); if (s->cb.audio_tag) s->cb.audio_tag(s->ud, t.data, t.size, t.ts); s->audio_pos = t.next; }
        }
        else if (t.type == 9 && !lookahead) video_tag(s, &t);
        else if (t.type == 18 && !lookahead) script_tag(s, &t);
        if (s->generation != gen) return;
        pos = t.next;
        if (!lookahead) { s->offset = pos; if (s->offset > s->preload_offset) s->preload_offset = s->offset; }
    }
    s->stream_time = max_time;
    if (underrun && !s->avm2) {
        /* AVM1: the play head stays on the last tag; Buffer.Empty comes a tick later */
        if (s->expected_finished) {
            s->stream_time = s->last_ts;
            s->ended = 1;
            status(s, "NetStream.Buffer.Flush", "status", NULL);
            if (s->generation == gen) status(s, "NetStream.Play.Stop", "status", NULL);
            if (s->generation == gen && s->ended && !s->has_seek) s->empty_pending = 1;
        }
        return;
    }
    if (underrun) {
        int end = s->expected_finished;
        status(s, "NetStream.Buffer.Flush", "status", NULL);
        if (end) status(s, "NetStream.Play.Stop", "status", NULL);
        if (s->generation == gen) {
            status(s, "NetStream.Buffer.Empty", "status", NULL);
            if (end) s->playing = 0;
        }
    }
}

void ns_tick_all(Player *p, int avm2, double dt_ms) {
    int i, n = g_nactive; NetStream **l;
    if (!n) return;
    l = (NetStream**)malloc((size_t)n * sizeof(NetStream*));
    if (!l) return;
    memcpy(l, g_active, (size_t)n * sizeof(NetStream*));
    for (i = 0; i < n; i++) {
        int k, still = 0;
        for (k = 0; k < g_nactive; k++) if (g_active[k] == l[i]) { still = 1; break; }
        if (still && l[i]->player == p && l[i]->avm2 == avm2) tick(l[i], dt_ms);
    }
    free(l);
}

/* queued downloads finish after the frame's scripts (Ruffle's futures) */
void ns_process_loads(Player *p, int avm2) {
    int i;
    for (i = 0; i < g_nall; i++) {
        NetStream *s = g_all[i]; char path[1100]; int gen;
        if (s->player != p || s->avm2 != avm2 || !s->loading) continue;
        s->loading = 0;
        gen = s->generation;
        if (s->url && fetch_url_to_path(p, s->url, path, sizeof(path))) {
            size_t len = 0; u8 *data = fetch_read_file(path, &len);
            if (data) {
                s->expected_len = len;
                free(s->buf); s->buf = data; s->len = len;
                activate(s);
                if (!s->avm2) {
                    /* AVM1: Play.Start once the data is there; a paused stream still
                       delivers what is at time 0 (onMetaData) and reports Buffer.Full
                       when it plays */
                    size_t q; FTag t;
                    s->expected_finished = 1;
                    if (sniff(s)) for (q = s->offset; !flv_tag(s, q, &t); q = t.next) s->data_end_ts = t.ts;
                    if (s->start_pending) { s->start_pending = 0; status(s, "NetStream.Play.Start", "status", NULL); }
                    if (s->generation != gen) continue;
                    if (s->playing) { status(s, "NetStream.Buffer.Full", "status", NULL); continue; }
                    s->full_pending = 1;
                    for (q = s->offset; !flv_tag(s, q, &t) && t.ts <= s->stream_time; q = t.next) {
                        if (t.type == 18) script_tag(s, &t);
                        else if (t.type == 9) video_tag(s, &t);
                        if (s->generation != gen) break;
                        if (t.type != 8) s->offset = t.next;
                        else break;
                    }
                    continue;
                }
                status(s, "NetStream.Buffer.Full", "status", NULL);
                if (s->generation == gen) s->expected_finished = 1;
            }
        }
        if (i >= g_nall) break;
    }
}

void ns_play(NetStream *s, const char *url) {
    if (url) {
        char res[1100];
        fetch_url_resolve(fetch_movie_url(s->player->movie), url, res, sizeof(res));
        free(s->url); s->url = (char*)malloc(strlen(res) + 1); if (s->url) strcpy(s->url, res);
        reset_source(s);
        s->loading = 1;
    }
    s->playing = 1;
    activate(s);
    if (!s->avm2 && url) { s->start_pending = 1; s->full_pending = 0; s->ended = 0; s->empty_pending = 0; s->last_ts = 0; s->data_end_ts = 0; return; }
    status(s, "NetStream.Play.Start", "status", NULL);
}
void ns_pause(NetStream *s, int notify) { s->playing = 0; audio_xstream_pause(s->player, s, 1); if (notify) status(s, "NetStream.Pause.Notify", "status", "Pausing"); }
void ns_resume(NetStream *s) { s->playing = 1; audio_xstream_pause(s->player, s, 0); activate(s); }
void ns_toggle(NetStream *s) { s->playing = !s->playing; audio_xstream_pause(s->player, s, !s->playing); if (s->playing) activate(s); }
void ns_seek(NetStream *s, double ms, int notify) {
    s->has_seek = 1; s->seek_time = ms;
    activate(s);
    if (notify) { char d[64]; sprintf(d, "Start Seeking %lu", (unsigned long)(ms < 0 ? 0 : ms)); status(s, "NetStream.SeekStart.Notify", "status", d); }
}
int ns_is_live(NetStream *s) { int i; for (i = 0; i < g_nall; i++) if (g_all[i] == s) return 1; return 0; }
void ns_video_size(NetStream *s, int *w, int *h) { const Bitmap *b = ns_frame(s); *w = b ? b->w : 0; *h = b ? b->h : 0; if (b && (s->vcodec == VCODEC_VP6 || s->vcodec == VCODEC_VP6A)) { *w -= s->vadj >> 4; *h -= s->vadj & 15; if (*w < 1) *w = 1; if (*h < 1) *h = 1; } }
const Bitmap *ns_frame(NetStream *s) { return s && s->vdec && s->has_frame ? vdec_image(s->vdec) : NULL; }
size_t ns_bytes_loaded(NetStream *s) { return s->len; }
double ns_buffer_length(NetStream *s) { double b = (s->data_end_ts - s->stream_time) / 1000.0; return s->buf && b > 0 ? b : 0; }
size_t ns_bytes_total(NetStream *s) { return s->expected_len > s->len ? s->expected_len : s->len; }

/* the VM objects of streams with work pending stay alive */
void ns_mark_active(Player *p, int avm2, void (*fn)(void *ctx, void *ud), void *ctx) {
    int i;
    for (i = 0; i < g_nactive; i++) if (g_active[i]->player == p && g_active[i]->avm2 == avm2) fn(ctx, g_active[i]->ud);
    for (i = 0; i < g_nall; i++) if (g_all[i]->player == p && g_all[i]->avm2 == avm2 && g_all[i]->loading) fn(ctx, g_all[i]->ud);
}
