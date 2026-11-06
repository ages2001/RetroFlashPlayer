/* audio.h - sound core (platform independent).  Decodes DefineSound and
 * SoundStreamBlock data to 16-bit PCM and mixes active voices on demand;
 * the platform layer (waveOut on NT) pulls from audio_mix(). */
#ifndef FLASHNT_AUDIO_H
#define FLASHNT_AUDIO_H

#include "player.h"

#define AUDIO_RATE 44100

void *audio_create(Player *p);
void  audio_free(Player *p);

/* timeline tags */
void  audio_start_sound (Player *p, MovieClip *c, const u8 *data, size_t len);
void  audio_stream_head (Player *p, MovieClip *c, const u8 *data, size_t len);
void  audio_stream_block(Player *p, MovieClip *c, const u8 *data, size_t len);

/* platform pulls mixed interleaved stereo 16-bit at AUDIO_RATE */
extern double g_stream_lead_ms;    /* see sound.c */
int   audio_stream_active(Player *p);
void  audio_mix(Player *p, s16 *buf, int nframes);

/* ActionScript Sound object */
void  audio_play_character(Player *p, int sound_id, int loops);
void  audio_play_character_ex(Player *p, int sound_id, int loops, int volume, void *owner);
void  audio_stop_character(Player *p, int sound_id);
void  audio_stop_all(Player *p);
void  audio_set_volume(Player *p, int sound_id, int volume);
void  audio_set_master_volume(Player *p, int volume);
double audio_duration_ms(Player *p, int sound_id);
/* pops the owner (AVM1 object) of a voice that finished since the last call; NULL when none */
void  *audio_take_finished(Player *p);
/* GC support: visit every owner object referenced by live voices / pending events */
void   audio_foreach_owner(Player *p, void (*fn)(void *ctx, void *owner), void *ctx);
double audio_position_ms(Player *p, int sound_id);

/* decoded sound */
typedef struct {
    int   rate;        /* Hz */
    int   stereo;
    int   nframes;
    s16  *pcm;         /* interleaved if stereo */
    int   format;      /* SWF format code */
    int   hdr_rate;    /* rate and sample count the DefineSound header declares (Sound.duration) */
    u32   hdr_count;
} SoundDef;

SoundDef *sound_get(SWFMovie *m, int id);

/* instances (AVM1 Sound objects, timeline sounds of a clip) */
typedef struct { int inst; void *owner; SoundDef *sd; } AudioDone;
int    audio_start_def(Player *p, SoundDef *sd, int loops, double in_sample44, void *dobj, void *owner);   /* instance id, 0 = none */
void   audio_stop_instance(Player *p, int inst);
void   audio_stop_def(Player *p, SoundDef *sd);
void   audio_stop_dobj(Player *p, int (*match)(void *ctx, void *dobj), void *ctx);
int    audio_instance_pos(Player *p, int inst, double *ms);          /* 0 when no longer playing */
int    audio_take_done(Player *p, AudioDone *out);                   /* sounds that ended, oldest first */
void   audio_foreach_voice(Player *p, void (*fn)(void *ctx, int inst, void *dobj, void *owner, int *st), void *ctx);
int    audio_voice_count(Player *p);
double sound_def_duration_ms(SoundDef *sd);
SoundDef *sound_def_from_mp3(const u8 *d, size_t n);
void   sound_def_free(SoundDef *sd);
void   audio_forget_dobj(Player *p, void *dobj);

/* AVM2 sound channels: voices with their own completion list (owner: the SoundChannel A2Obj) */
int    audio_start_avm2(Player *p, SoundDef *sd, int loops, double in_sample44, void *owner);
int    audio_dyn_start(Player *p, void *owner);                       /* script-fed stereo 44.1 kHz voice */
void   audio_dyn_push(Player *p, int inst, const s16 *pcm, int nframes);
int    audio_dyn_buffered(Player *p, int inst);                       /* frames not yet played, -1 when gone */
void   audio_dyn_end(Player *p, int inst);                            /* no more data: ends when drained */
int    audio_take_done2(Player *p, int *inst);                        /* AVM2 voices that ended, oldest first */
int    audio_instance_peak(Player *p, int inst, float *l, float *r);
void   audio_instance_st(Player *p, int inst, const int *st);         /* volume, ll, lr, rl, rr (percent) */
void   audio_foreach_timeline(Player *p, void (*fn)(void *ctx, void *dobj, int *st), void *ctx);
void   audio_foreach_owner2(Player *p, void (*fn)(void *ctx, void *owner), void *ctx);
void   audio_sample_history(Player *p, float *out);                   /* 1024 stereo frames */
void   audio_avm2_complete_ahead(Player *p, int ahead);              /* near-finished AVM2 voices count as done */

/* external PCM streams (NetStream audio), keyed by an owner pointer */
void   audio_xstream_push(Player *p, void *key, const s16 *pcm, int nframes, int rate, int stereo);
void   audio_xstream_stop(Player *p, void *key);
void   audio_xstream_pause(Player *p, void *key, int paused);
void   audio_xstream_volume(Player *p, void *key, int volume);

/* MP3 packets decoded in sequence (mp3.c) */
void  *mp3s_new(void);
int    mp3s_decode(void *st, const u8 *d, size_t n, int *rate, int *stereo, s16 **out);
void   mp3s_free(void *st);

/* decoders (sound_codec.c): return frames decoded into *out (malloc'd) */
int  decode_pcm  (const u8 *d, size_t n, int bits16, int stereo, int big_endian, s16 **out);
int  decode_adpcm(const u8 *d, size_t n, int stereo, s16 **out);
int  decode_g711 (const u8 *d, size_t n, int alaw, s16 **out);   /* samples */
/* aac.c: AAC-LC (AudioSpecificConfig, then raw_data_blocks of 1024 frames) */
typedef struct AACDec AACDec;
AACDec *aac_new(void);
void aac_free(AACDec *d);
int  aac_config(AACDec *d, const u8 *asc, size_t n);
int  aac_decode(AACDec *d, const u8 *data, size_t n, s16 **out, int *rate, int *channels);   /* frames */
int  decode_mp3  (const u8 *d, size_t n, int *rate, int *stereo, s16 **out);

#endif
