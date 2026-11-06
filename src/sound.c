/* sound.c - DefineSound decoding, stream sounds, software mixer */
#include "audio.h"
#include <stdio.h>
#include "reader.h"
#include <stdlib.h>
#include <string.h>

#define MAX_VOICES 32

typedef struct Voice {
    int      active;
    int      sound_id;          /* -1 for stream voices */
    MovieClip *stream_clip;
    const s16 *pcm; int nframes; int stereo; int rate;
    double   pos;               /* frame position */
    double   step;              /* rate / AUDIO_RATE */
    int      loops;             /* remaining extra loops */
    int      volume;            /* 0..100 */
    void    *owner;             /* script object told about the end (AVM1 Sound / AVM2 SoundChannel) */
    void    *dobj;              /* AVM1: display object the sound belongs to (MovieClip*) */
    SoundDef *sd;               /* the sound played (also the handle for stopping it) */
    int      inst;              /* instance id */
    int      st[5];             /* effective transform: volume, ll, lr, rl, rr */
    int      paused;            /* external streams (NetStream audio) */
    int      avm2;              /* AVM2 SoundChannel voice: its own completion list, owner is an A2Obj */
    int      reported;          /* AVM2: completion already reported (the tail is still in the output buffer) */
    int      ending;            /* dynamic voice (-4): no more data comes, finish when drained */
    double   dropped;           /* dynamic voice: frames already played and dropped from the buffer */
    float    peak[2];           /* amplitude of the last mixed block (SoundChannel.leftPeak/rightPeak) */
    /* stream: growable buffer */
    s16     *sbuf; int scap, slen;
    int      hold;              /* stream: waiting for g_stream_lead_ms of data before it plays */
    int      hold_nf, hold_wait;/* stream: buffer size when the hold began, and mix calls spent waiting */
} Voice;

/* A stream sound's blocks arrive one per frame, as the timeline plays, while
   the platform mixes ahead of the speaker by its output buffering.  With a
   lead (set by the Windows player to that buffering), a stream voice starts,
   and restarts after running dry, only once it holds that much audio, so it
   plays continuously instead of in pieces.  0 (the default, the test host):
   it plays whatever there is. */
double g_stream_lead_ms = 0;
int g_stream_starved;   /* times a stream voice ran dry (diagnostics) */

typedef struct StreamInfo { int fmt, rate, stereo, bits16; } StreamInfo;

typedef struct AudioState {
    Voice voices[MAX_VOICES];
    int   master;
    int   muted;
    struct { MovieClip *c; StreamInfo si; } streams[64];
    int   nstreams;
    void *finished[64]; int nfinished;
    AudioDone done[64]; int ndone;
    int   next_inst;
    int   done2[256]; int ndone2;    /* AVM2 voices that played to their end */
    float hist[2048][2]; int hpos;   /* the last two windows of mixed output (SoundMixer.computeSpectrum) */
} AudioState;

static const int rate_table[4] = { 5512, 11025, 22050, 44100 };

/* ---- DefineSound decode (cached in ch->parsed) ---- */

SoundDef *sound_get(SWFMovie *m, int id) {
    SWFChar *ch = swf_get_char(m, id);
    SoundDef *sd;
    Reader r;
    int flags, fmt, rate, bits16, stereo;
    u32 count;
    const u8 *data; size_t len;
    if (!ch || ch->type != CHAR_SOUND) return NULL;
    if (ch->parsed) return (SoundDef*)ch->parsed;
    reader_init(&r, ch->data, ch->len);
    r_u16(&r);
    flags = r_u8(&r);
    fmt = flags >> 4; rate = (flags >> 2) & 3; bits16 = (flags >> 1) & 1; stereo = flags & 1;
    count = r_u32(&r);
    data = ch->data + r.pos; len = ch->len - r.pos;
    sd = (SoundDef*)calloc(1, sizeof(SoundDef));
    if (!sd) return NULL;
    sd->rate = rate_table[rate]; sd->stereo = stereo; sd->format = fmt;
    sd->hdr_rate = sd->rate; sd->hdr_count = count;
    switch (fmt) {
    case 0: sd->nframes = decode_pcm(data, len, bits16, stereo, 0, &sd->pcm); break;
    case 3: sd->nframes = decode_pcm(data, len, bits16, stereo, 0, &sd->pcm); break;
    case 1: sd->nframes = decode_adpcm(data, len, stereo, &sd->pcm); break;
    case 2: {
        int mrate = sd->rate, mst = stereo;
        if (len > 2) sd->nframes = decode_mp3(data + 2, len - 2, &mrate, &mst, &sd->pcm);
        sd->rate = mrate; sd->stereo = mst;
        break;
    }
    default: sd->nframes = 0; break;
    }
    (void)count;
    ch->parsed = sd;
    return sd;
}

/* ---- voices ---- */

static Voice *voice_alloc(AudioState *a) {
    int i, steal = -1;
    double best = -1.0;
    for (i = 0; i < MAX_VOICES; i++) if (!a->voices[i].active) {
        Voice *v = &a->voices[i];
        free(v->sbuf);
        memset(v, 0, sizeof(Voice));
        v->st[0] = 100; v->st[1] = 100; v->st[4] = 100;
        v->inst = ++a->next_inst; if (v->inst <= 0) v->inst = a->next_inst = 1;
        return v;
    }
    /* All busy: steal the event voice closest to finishing so new event
       sounds keep playing (matches Flash better than dropping outright).
       Streaming voices (-1 SoundStreamBlock, -3/-4 stream) are protected. */
    for (i = 0; i < MAX_VOICES; i++) {
        Voice *v = &a->voices[i];
        double prog;
        if (!v->active) continue;
        if (v->sound_id == -1 || v->sound_id == -3 || v->sound_id == -4) continue;
        prog = v->nframes > 0 ? v->pos / (double)v->nframes : 1.0;
        if (prog > best) { best = prog; steal = i; }
    }
    if (steal >= 0) {
        Voice *v = &a->voices[steal];
        free(v->sbuf);
        memset(v, 0, sizeof(Voice));
        v->st[0] = 100; v->st[1] = 100; v->st[4] = 100;
        v->inst = ++a->next_inst; if (v->inst <= 0) v->inst = a->next_inst = 1;
        return v;
    }
    return NULL;
}

static int sndlog(void){ static int s=-1; if(s<0) s=getenv("RETROFP_SNDLOG")?1:0; return s; }
static Voice *voice_start(Player *p, SoundDef *sd, int id, int loops, int volume, void *owner, double start_frame) {
    AudioState *a = (AudioState*)p->audio;
    Voice *v;
    if (sndlog()) { extern int g_dbg_frame; static int n=0; fprintf(stderr, "SND start #%d frame=%d id=%d nframes=%d dur=%.2fs\n", ++n, g_dbg_frame, id, sd?sd->nframes:-1, (sd&&sd->rate)?sd->nframes/(double)sd->rate:0.0); fflush(stderr); }
    if (!a || !sd || sd->nframes <= 0) { if(sndlog()){fprintf(stderr,"SND start REJECT id=%d nframes=%d\n",id,sd?sd->nframes:-1);fflush(stderr);} return NULL; }
    v = voice_alloc(a);
    if (!v) return NULL;
    v->active = 1; v->sound_id = id; v->pcm = sd->pcm; v->nframes = sd->nframes; v->stereo = sd->stereo; v->rate = sd->rate;
    v->pos = start_frame; v->step = (double)sd->rate / AUDIO_RATE; v->loops = loops > 0 ? loops - 1 : 0; v->volume = volume; v->owner = owner;
    v->sd = sd;
    return v;
}

void *audio_create(Player *p) {
    AudioState *a = (AudioState*)calloc(1, sizeof(AudioState));
    if (a) a->master = 100;
    p->audio = a;
    return a;
}

void audio_free(Player *p) {
    AudioState *a = (AudioState*)p->audio; int i;
    if (!a) return;
    for (i = 0; i < MAX_VOICES; i++) free(a->voices[i].sbuf);
    free(a); p->audio = NULL;
}

/* StartSound: id u16 + SOUNDINFO */
void audio_start_sound(Player *p, MovieClip *c, const u8 *data, size_t len) {
    Reader r; int id, flags, loops = 1; u32 in = 0;
    SoundDef *sd;
    AudioState *a = (AudioState*)p->audio;
    if (!a) return;
    reader_init(&r, data, len);
    id = r_u16(&r);
    flags = r_u8(&r);
    if (flags & 0x20) { audio_stop_character(p, id); return; }      /* SyncStop */
    sd = sound_get(PMOV(p), id);
    if (!sd) return;
    if (flags & 0x10) {                                              /* SyncNoMultiple */
        int i; for (i = 0; i < MAX_VOICES; i++) if (a->voices[i].active && a->voices[i].sd == sd) return;
    }
    if (flags & 0x01) in = r_u32(&r);
    if (flags & 0x02) r_u32(&r);                                     /* out point */
    if (flags & 0x04) loops = r_u16(&r);
    { Voice *v = voice_start(p, sd, id, loops, 100, NULL, (double)in * sd->rate / 44100.0); if (v) v->dobj = c; }
}

/* ---- stream sounds (one per clip) ---- */

static Voice *stream_voice(AudioState *a, MovieClip *c, int create) {
    int i;
    for (i = 0; i < MAX_VOICES; i++) if (a->voices[i].active && a->voices[i].stream_clip == c) return &a->voices[i];
    if (!create) return NULL;
    {
        Voice *v = voice_alloc(a);
        if (!v) return NULL;
        v->active = 1; v->sound_id = -1; v->stream_clip = c; v->volume = 100; v->loops = 0; v->dobj = c;
        v->hold = g_stream_lead_ms > 0;
        return v;
    }
}

void audio_stream_head(Player *p, MovieClip *c, const u8 *data, size_t len) {
    Reader r; int flags, i; StreamInfo si;
    AudioState *a = (AudioState*)p->audio;
    if (!a || len < 4) return;
    reader_init(&r, data, len);
    r_u8(&r);
    flags = r_u8(&r);
    si.fmt = flags >> 4; si.rate = rate_table[(flags >> 2) & 3]; si.bits16 = (flags >> 1) & 1; si.stereo = flags & 1;
    for (i = 0; i < a->nstreams; i++) if (a->streams[i].c == c) { a->streams[i].si = si; return; }
    if (a->nstreams < 64) { a->streams[a->nstreams].c = c; a->streams[a->nstreams].si = si; a->nstreams++; }
}

static StreamInfo *stream_info_for(AudioState *a, MovieClip *c) {
    int i;
    for (i = 0; i < a->nstreams; i++) if (a->streams[i].c == c) return &a->streams[i].si;
    return NULL;
}

void audio_stream_block(Player *p, MovieClip *c, const u8 *data, size_t len) {
    AudioState *a = (AudioState*)p->audio;
    Voice *v; StreamInfo *si;
    s16 *pcm = NULL; int n = 0, rate = 44100, stereo = 1;
    if (!a || len == 0) return;
    si = stream_info_for(a, c);
    if (!si) {
        /* no head seen: assume MP3 stream block layout */
        if (len > 4) n = decode_mp3(data + 4, len - 4, &rate, &stereo, &pcm);
    } else if (si->fmt == 2) {
        if (len > 4) n = decode_mp3(data + 4, len - 4, &rate, &stereo, &pcm);
    } else if (si->fmt == 1) {
        n = decode_adpcm(data, len, si->stereo, &pcm); rate = si->rate; stereo = si->stereo;
    } else {
        n = decode_pcm(data, len, si->bits16, si->stereo, si->fmt == 0, &pcm); rate = si->rate; stereo = si->stereo;
    }
    if (n <= 0 || !pcm) { free(pcm); return; }
    v = stream_voice(a, c, 1);
    if (!v) { free(pcm); return; }
    {
        int ch = stereo ? 2 : 1, need = (v->slen + n) * ch;
        if (need > v->scap) {
            int nc = v->scap ? v->scap : 65536; s16 *nb;
            while (nc < need) nc *= 2;
            nb = (s16*)realloc(v->sbuf, (size_t)nc * sizeof(s16));
            if (!nb) { free(pcm); return; }
            v->sbuf = nb; v->scap = nc;
        }
        memcpy(v->sbuf + v->slen * ch, pcm, (size_t)n * ch * sizeof(s16));
        v->slen += n;
        v->pcm = v->sbuf; v->nframes = v->slen; v->stereo = stereo; v->rate = rate;
        v->step = (double)rate / AUDIO_RATE;
    }
    free(pcm);
}

/* ---- AS API ---- */

void audio_play_character(Player *p, int sound_id, int loops) { audio_play_character_ex(p, sound_id, loops, 100, NULL); }
void audio_play_character_ex(Player *p, int sound_id, int loops, int volume, void *owner) {
    SoundDef *sd = sound_get(PMOV(p), sound_id);
    if (sd) voice_start(p, sd, sound_id, loops < 1 ? 1 : loops, volume, owner, 0);
}
void audio_stop_character(Player *p, int sound_id) {
    AudioState *a = (AudioState*)p->audio; int i;
    if (!a) return;
    for (i = 0; i < MAX_VOICES; i++) if (a->voices[i].active && a->voices[i].sound_id == sound_id) a->voices[i].active = 0;
}
void audio_stop_all(Player *p) {
    AudioState *a = (AudioState*)p->audio; int i;
    if (!a) return;
    for (i = 0; i < MAX_VOICES; i++) { if (a->voices[i].sbuf) { free(a->voices[i].sbuf); a->voices[i].sbuf = NULL; } a->voices[i].active = 0; }
}
void audio_set_volume(Player *p, int sound_id, int volume) {
    AudioState *a = (AudioState*)p->audio; int i;
    if (!a) return;
    for (i = 0; i < MAX_VOICES; i++) if (a->voices[i].active && a->voices[i].sound_id == sound_id) a->voices[i].volume = volume;
}
void audio_set_master_volume(Player *p, int volume) { AudioState *a = (AudioState*)p->audio; if (a) a->master = volume; }
double audio_duration_ms(Player *p, int sound_id) { SoundDef *sd = sound_get(PMOV(p), sound_id); return sd && sd->rate ? sd->nframes * 1000.0 / sd->rate : 0; }
double audio_position_ms(Player *p, int sound_id) {
    AudioState *a = (AudioState*)p->audio; int i;
    if (!a) return 0;
    for (i = 0; i < MAX_VOICES; i++) if (a->voices[i].active && a->voices[i].sound_id == sound_id) return a->voices[i].pos * 1000.0 / a->voices[i].rate;
    return 0;
}

/* ---- mixer ---- */

void audio_mix(Player *p, s16 *buf, int nframes) {
    AudioState *a = (AudioState*)p->audio; int i, f;
    s32 *acc, lo[2], hi[2];
    memset(buf, 0, (size_t)nframes * 2 * sizeof(s16));
    if (!a) return;
    acc = (s32*)calloc((size_t)nframes * 2, sizeof(s32));
    if (!acc) return;
    for (i = 0; i < MAX_VOICES; i++) {
        Voice *v = &a->voices[i];
        int gain;
        if (!v->active || !v->pcm || v->paused) continue;
        if (v->sound_id == -1 && v->hold) {
            if (g_stream_lead_ms > 0 && (v->nframes - v->pos) * 1000.0 < g_stream_lead_ms * v->rate) {
                /* not enough lead yet.  Keep holding only while the buffer is
                   still growing toward the target; a short stream, or one whose
                   timeline has stopped feeding blocks, can never reach the lead,
                   so after a short grace play what there is instead of staying
                   silent forever. */
                if (v->nframes > v->hold_nf) { v->hold_nf = v->nframes; v->hold_wait = 0; continue; }
                if (++v->hold_wait < 12) continue;   /* ~280 ms grace for more blocks */
            }
            v->hold = 0; v->hold_wait = 0;
        }
        gain = v->volume * a->master / 100;
        if (gain < 0) gain = 0; if (gain > 100) gain = 100;
        gain = gain * v->st[0] / 100;
        lo[0] = lo[1] = 0x7fffffff; hi[0] = hi[1] = -0x7fffffff;
        for (f = 0; f < nframes; f++) {
            int ip = (int)v->pos; s32 l, r;
            if (ip >= v->nframes) {
                if (v->sound_id == -1 || v->sound_id == -3 || (v->sound_id == -4 && !v->ending)) { if (v->sound_id == -1) { g_stream_starved++; if (g_stream_lead_ms > 0) { v->hold = 1; v->hold_nf = v->nframes; v->hold_wait = 0; } } break; }   /* stream: starved, wait */
                if (v->loops > 0) { v->loops--; v->pos -= v->nframes; ip = (int)v->pos; if (ip < 0) ip = 0; }
                else {
                    v->active = 0;
                    if (v->avm2) { if (!v->reported && a->ndone2 < 256) a->done2[a->ndone2++] = v->inst; }
                    else {
                        if (v->owner && a->nfinished < 64) a->finished[a->nfinished++] = v->owner;
                        if (a->ndone < 64) { a->done[a->ndone].inst = v->inst; a->done[a->ndone].owner = v->owner; a->done[a->ndone].sd = v->sd; a->ndone++; }
                    }
                    break;
                }
            }
            if (v->stereo) { l = v->pcm[ip * 2]; r = v->pcm[ip * 2 + 1]; }
            else { l = r = v->pcm[ip]; }
            if (v->step != 1.0 && ip + 1 < v->nframes) {   /* resampling interpolates linearly (Ruffle's mixer, dasp Linear) */
                double fr = v->pos - ip; s32 l2, r2;
                if (v->stereo) { l2 = v->pcm[ip * 2 + 2]; r2 = v->pcm[ip * 2 + 3]; } else l2 = r2 = v->pcm[ip + 1];
                l = (s32)(l + (l2 - l) * fr); r = (s32)(r + (r2 - r) * fr);
            }
            {
                s32 ol = (l * v->st[1] + r * v->st[3]) / 100, or_ = (l * v->st[2] + r * v->st[4]) / 100;
                ol = ol * gain / 100; or_ = or_ * gain / 100;
                acc[f * 2] += ol; acc[f * 2 + 1] += or_;
                if (ol < lo[0]) lo[0] = ol; if (ol > hi[0]) hi[0] = ol;
                if (or_ < lo[1]) lo[1] = or_; if (or_ > hi[1]) hi[1] = or_;
            }
            v->pos += v->step;
        }
        if (f > 0 && hi[0] >= lo[0]) { v->peak[0] = (float)(hi[0] - lo[0]) / 65536.0f; v->peak[1] = (float)(hi[1] - lo[1]) / 65536.0f; }
    }
    for (f = 0; f < nframes * 2; f++) { s32 s = acc[f]; if (s > 32767) s = 32767; if (s < -32768) s = -32768; buf[f] = (s16)s; }
    for (f = 0; f < nframes; f++) {   /* output memory for computeSpectrum */
        a->hist[a->hpos][0] = buf[f * 2] / 32768.0f; a->hist[a->hpos][1] = buf[f * 2 + 1] / 32768.0f;
        a->hpos = (a->hpos + 1) & 2047;
    }
    if (sndlog()) { static int cc=0; int na=0, n18=0, k; s32 pk=0; char ids[160]; int ip=0;
        for (k=0;k<MAX_VOICES;k++){ Voice*v=&a->voices[k]; if(v->active&&v->pcm){na++; if(v->sound_id==18)n18++;
            if(v->sound_id!=18 && ip<140){ ip+=sprintf(ids+ip," id=%d pos=%d/%d hold=%d", v->sound_id,(int)v->pos,v->nframes,v->hold); } } }
        for (f=0;f<nframes*2;f++){ s32 x=buf[f]<0?-buf[f]:buf[f]; if(x>pk)pk=x; }
        if ((na>0 || pk>0) && (++cc % 4 == 0)) { fprintf(stderr,"SND mix active=%d id18=%d peak=%d%s\n", na, n18, (int)pk, ids); fflush(stderr); } }
    free(acc);
}

void *audio_take_finished(Player *p) {
    AudioState *a = (AudioState*)p->audio;
    if (!a || a->nfinished == 0) return NULL;
    return a->finished[--a->nfinished];
}

void audio_foreach_owner(Player *p, void (*fn)(void *ctx, void *owner), void *ctx) {
    AudioState *a = (AudioState*)p->audio; int i;
    if (!a) return;
    for (i = 0; i < MAX_VOICES; i++) if (a->voices[i].active && a->voices[i].owner && !a->voices[i].avm2) fn(ctx, a->voices[i].owner);
    for (i = 0; i < a->nfinished; i++) fn(ctx, a->finished[i]);
}
void audio_foreach_owner2(Player *p, void (*fn)(void *ctx, void *owner), void *ctx) {
    AudioState *a = (AudioState*)p->audio; int i;
    if (!a) return;
    for (i = 0; i < MAX_VOICES; i++) if (a->voices[i].active && a->voices[i].owner && a->voices[i].avm2) fn(ctx, a->voices[i].owner);
}

/* ---- instances (AVM1 Sound) ---- */

int audio_start_def(Player *p, SoundDef *sd, int loops, double in_sample44, void *dobj, void *owner) {
    Voice *v;
    if (!sd) return 0;
    v = voice_start(p, sd, -2, loops < 1 ? 1 : loops, 100, owner, in_sample44 * sd->rate / 44100.0);
    if (!v) return 0;
    v->dobj = dobj;
    return v->inst;
}

static void voice_kill(Voice *v) { if (v->sbuf) { free(v->sbuf); v->sbuf = NULL; } v->active = 0; }

void audio_stop_instance(Player *p, int inst) {
    AudioState *a = (AudioState*)p->audio; int i;
    if (!a || !inst) return;
    for (i = 0; i < MAX_VOICES; i++) if (a->voices[i].active && a->voices[i].inst == inst) voice_kill(&a->voices[i]);
}
void audio_stop_def(Player *p, SoundDef *sd) {
    AudioState *a = (AudioState*)p->audio; int i;
    if (!a || !sd) return;
    for (i = 0; i < MAX_VOICES; i++) if (a->voices[i].active && a->voices[i].sd == sd) voice_kill(&a->voices[i]);
}
void audio_stop_dobj(Player *p, int (*match)(void *ctx, void *dobj), void *ctx) {
    AudioState *a = (AudioState*)p->audio; int i;
    if (!a) return;
    for (i = 0; i < MAX_VOICES; i++) if (a->voices[i].active && a->voices[i].dobj && match(ctx, a->voices[i].dobj)) voice_kill(&a->voices[i]);
}
int audio_instance_pos(Player *p, int inst, double *ms) {
    AudioState *a = (AudioState*)p->audio; int i;
    if (!a) return 0;
    for (i = 0; i < MAX_VOICES; i++) if (a->voices[i].active && a->voices[i].inst == inst) { *ms = a->voices[i].rate ? (a->voices[i].pos + a->voices[i].dropped) * 1000.0 / a->voices[i].rate : 0; return 1; }
    return 0;
}
int audio_take_done(Player *p, AudioDone *out) {
    AudioState *a = (AudioState*)p->audio; int i;
    if (!a || !a->ndone) return 0;
    *out = a->done[0];
    for (i = 1; i < a->ndone; i++) a->done[i - 1] = a->done[i];
    a->ndone--;
    return 1;
}
void audio_foreach_voice(Player *p, void (*fn)(void *ctx, int inst, void *dobj, void *owner, int *st), void *ctx) {
    AudioState *a = (AudioState*)p->audio; int i;
    if (!a) return;
    for (i = 0; i < MAX_VOICES; i++) if (a->voices[i].active && !a->voices[i].avm2) fn(ctx, a->voices[i].inst, a->voices[i].dobj, a->voices[i].owner, a->voices[i].st);
}
int audio_voice_count(Player *p) { AudioState *a = (AudioState*)p->audio; int i, n = 0; if (!a) return 0; for (i = 0; i < MAX_VOICES; i++) n += a->voices[i].active; return n; }
double sound_def_duration_ms(SoundDef *sd) { return sd && sd->hdr_rate ? sd->hdr_count * 1000.0 / sd->hdr_rate : 0; }

/* a clip is being freed: its sounds play on without it */
void audio_forget_dobj(Player *p, void *dobj) {
    AudioState *a = (AudioState*)p->audio; int i;
    if (!a) return;
    for (i = 0; i < MAX_VOICES; i++) if (a->voices[i].dobj == dobj) { a->voices[i].dobj = NULL; if (a->voices[i].stream_clip == dobj) voice_kill(&a->voices[i]); }
}

/* an external MP3 file (Sound.loadSound): NULL when nothing decodes */
SoundDef *sound_def_from_mp3(const u8 *d, size_t n) {
    SoundDef *sd = (SoundDef*)calloc(1, sizeof(SoundDef)); int rate = 44100, st = 1;
    if (!sd) return NULL;
    sd->nframes = decode_mp3(d, n, &rate, &st, &sd->pcm);
    sd->rate = rate; sd->stereo = st; sd->format = 2;
    sd->hdr_rate = rate; sd->hdr_count = (u32)sd->nframes;
    if (sd->nframes <= 0 || !sd->pcm) { free(sd->pcm); free(sd); return NULL; }
    return sd;
}
void sound_def_free(SoundDef *sd) { if (sd) { free(sd->pcm); free(sd); } }

/* ---- external PCM streams (NetStream audio), keyed by their owner ---- */

static Voice *xstream_voice(AudioState *a, void *key, int create) {
    int i;
    for (i = 0; i < MAX_VOICES; i++) if (a->voices[i].active && a->voices[i].sound_id == -3 && (void*)a->voices[i].stream_clip == key) return &a->voices[i];
    if (!create) return NULL;
    {
        Voice *v = voice_alloc(a);
        if (!v) return NULL;
        v->active = 1; v->sound_id = -3; v->stream_clip = (MovieClip*)key; v->volume = 100;
        return v;
    }
}
void audio_xstream_push(Player *p, void *key, const s16 *pcm, int n, int rate, int stereo) {
    AudioState *a = (AudioState*)p->audio; Voice *v; int ch = stereo ? 2 : 1, need;
    if (!a || n <= 0 || !pcm) return;
    v = xstream_voice(a, key, 1);
    if (!v) return;
    if (v->slen && (v->rate != rate || v->stereo != stereo)) { v->slen = 0; v->pos = 0; }   /* the format changed */
    /* drop what was played */
    if ((int)v->pos > 0 && (int)v->pos <= v->slen) {
        int done = (int)v->pos, vc = v->stereo ? 2 : 1;
        memmove(v->sbuf, v->sbuf + (size_t)done * vc, (size_t)(v->slen - done) * vc * sizeof(s16));
        v->slen -= done; v->pos -= done;
    }
    need = (v->slen + n) * ch;
    if (need > v->scap) {
        int nc = v->scap ? v->scap : 65536; s16 *nb;
        while (nc < need) nc *= 2;
        nb = (s16*)realloc(v->sbuf, (size_t)nc * sizeof(s16));
        if (!nb) return;
        v->sbuf = nb; v->scap = nc;
    }
    memcpy(v->sbuf + (size_t)v->slen * ch, pcm, (size_t)n * ch * sizeof(s16));
    v->slen += n;
    v->pcm = v->sbuf; v->nframes = v->slen; v->stereo = stereo; v->rate = rate;
    v->step = (double)rate / AUDIO_RATE;
}
void audio_xstream_stop(Player *p, void *key) {
    AudioState *a = (AudioState*)p->audio; Voice *v;
    if (!a) return;
    v = xstream_voice(a, key, 0);
    if (v) voice_kill(v);
}
void audio_xstream_pause(Player *p, void *key, int paused) {
    AudioState *a = (AudioState*)p->audio; Voice *v;
    if (!a) return;
    v = xstream_voice(a, key, 0);
    if (v) v->paused = paused;
}
void audio_xstream_volume(Player *p, void *key, int volume) {
    AudioState *a = (AudioState*)p->audio; Voice *v;
    if (!a) return;
    v = xstream_voice(a, key, 0);
    if (v) v->volume = volume;
}


/* ---- AVM2 sound channels ---- */

static Voice *inst_voice(AudioState *a, int inst) {
    int i;
    if (!a || !inst) return NULL;
    for (i = 0; i < MAX_VOICES; i++) if (a->voices[i].active && a->voices[i].inst == inst) return &a->voices[i];
    return NULL;
}
int audio_start_avm2(Player *p, SoundDef *sd, int loops, double in_sample44, void *owner) {
    Voice *v;
    if (!sd) return 0;
    v = voice_start(p, sd, -2, loops < 1 ? 1 : loops, 100, owner, in_sample44 * sd->rate / 44100.0);
    if (!v) return 0;
    v->avm2 = 1;
    return v->inst;
}
/* a voice fed by script (Sound sampleData events): interleaved stereo at 44.1 kHz */
int audio_dyn_start(Player *p, void *owner) {
    AudioState *a = (AudioState*)p->audio; Voice *v;
    if (!a) return 0;
    v = voice_alloc(a);
    if (!v) return 0;
    v->active = 1; v->sound_id = -4; v->volume = 100; v->owner = owner; v->avm2 = 1;
    v->stereo = 1; v->rate = AUDIO_RATE; v->step = 1.0;
    return v->inst;
}
void audio_dyn_push(Player *p, int inst, const s16 *pcm, int n) {
    AudioState *a = (AudioState*)p->audio; Voice *v = inst_voice(a, inst); int need;
    if (!v || n <= 0 || !pcm) return;
    if ((int)v->pos > 0 && (int)v->pos <= v->slen) {   /* drop what was played */
        int done = (int)v->pos;
        memmove(v->sbuf, v->sbuf + (size_t)done * 2, (size_t)(v->slen - done) * 2 * sizeof(s16));
        v->slen -= done; v->pos -= done; v->dropped += done;
    }
    need = (v->slen + n) * 2;
    if (need > v->scap) {
        int nc = v->scap ? v->scap : 65536; s16 *nb;
        while (nc < need) nc *= 2;
        nb = (s16*)realloc(v->sbuf, (size_t)nc * sizeof(s16));
        if (!nb) return;
        v->sbuf = nb; v->scap = nc;
    }
    memcpy(v->sbuf + (size_t)v->slen * 2, pcm, (size_t)n * 2 * sizeof(s16));
    v->slen += n;
    v->pcm = v->sbuf; v->nframes = v->slen;
}
int audio_dyn_buffered(Player *p, int inst) { Voice *v = inst_voice((AudioState*)p->audio, inst); return v ? v->slen - (int)v->pos : -1; }
void audio_dyn_end(Player *p, int inst) { Voice *v = inst_voice((AudioState*)p->audio, inst); if (v) v->ending = 1; }
int audio_take_done2(Player *p, int *inst) {
    AudioState *a = (AudioState*)p->audio; int i;
    if (!a || !a->ndone2) return 0;
    *inst = a->done2[0];
    for (i = 1; i < a->ndone2; i++) a->done2[i - 1] = a->done2[i];
    a->ndone2--;
    return 1;
}
int audio_instance_peak(Player *p, int inst, float *l, float *r) { Voice *v = inst_voice((AudioState*)p->audio, inst); if (!v) return 0; *l = v->peak[0]; *r = v->peak[1]; return 1; }
void audio_instance_st(Player *p, int inst, const int *st) { Voice *v = inst_voice((AudioState*)p->audio, inst); if (v) memcpy(v->st, st, sizeof(v->st)); }
/* the non-script voices (timeline event and stream sounds) and their display objects */
void audio_foreach_timeline(Player *p, void (*fn)(void *ctx, void *dobj, int *st), void *ctx) {
    AudioState *a = (AudioState*)p->audio; int i;
    if (!a) return;
    for (i = 0; i < MAX_VOICES; i++) { Voice *v = &a->voices[i]; if (v->active && !v->avm2 && !v->owner) fn(ctx, v->dobj, v->st); }
}
/* the half of the output memory not being written: 1024 stereo frames */
void audio_sample_history(Player *p, float *out) {
    AudioState *a = (AudioState*)p->audio; int i, base;
    if (!a) { memset(out, 0, 2048 * sizeof(float)); return; }
    base = a->hpos < 1024 ? 1024 : 0;
    for (i = 0; i < 1024; i++) { out[i * 2] = a->hist[base + i][0]; out[i * 2 + 1] = a->hist[base + i][1]; }
}
/* Flash reports a sound complete once its last samples went to the output
   buffer: AVM2 voices within `ahead` output frames of their end count as done */
void audio_avm2_complete_ahead(Player *p, int ahead) {
    AudioState *a = (AudioState*)p->audio; int i;
    if (!a) return;
    for (i = 0; i < MAX_VOICES; i++) {
        Voice *v = &a->voices[i];
        if (!v->active || !v->avm2 || v->reported || v->sound_id != -2 || v->loops > 0 || v->step <= 0) continue;
        if ((v->nframes - v->pos) / v->step < ahead) { v->reported = 1; if (a->ndone2 < 256) a->done2[a->ndone2++] = v->inst; }
    }
}

/* a timeline stream sound is playing (the Windows player then keeps the
   timeline up with the sound by skipping the drawing of late frames) */
int audio_stream_active(Player *p) {
    AudioState *a = (AudioState*)p->audio; int i;
    if (!a) return 0;
    /* a held stream is buffering/stalled and produces no audio, so it must not
       drive the video pace: otherwise a stuck stream (e.g. a preloader whose
       timeline has stopped feeding blocks) forces the frame-sync catch-up and
       disrupts the frames that fire the real intro sounds */
    for (i = 0; i < MAX_VOICES; i++) if (a->voices[i].active && a->voices[i].sound_id == -1 && !a->voices[i].hold) return 1;
    return 0;
}
