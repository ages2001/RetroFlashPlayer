/* netstream.h - progressive FLV playback shared by AVM1 and AVM2 NetStream */
#ifndef FLASHNT_NETSTREAM_H
#define FLASHNT_NETSTREAM_H

#include "player.h"

typedef struct NSCallbacks {
    /* onStatus / netStatus with {code, level[, description]} */
    void (*status)(void *ud, const char *code, const char *level, const char *desc);
    /* a script data entry: `name` with its AMF0 value at `p`; returns the
       position after the value (NULL to stop) */
    const u8 *(*script)(void *ud, const char *name, const u8 *p, const u8 *end);
    /* an FLV audio tag (header byte included) */
    void (*audio_tag)(void *ud, const u8 *data, u32 size, s32 ts);
    /* the audio stream starts over (new source, seek) */
    void (*audio_reset)(void *ud);
} NSCallbacks;

typedef struct NetStream {
    Player *player; int avm2;
    NSCallbacks cb; void *ud;
    char *url;
    u8 *buf; size_t len;
    size_t expected_len; int expected_finished;
    int loading, playing;
    size_t offset, preload_offset; int sniffed;
    double stream_time;              /* ms */
    int has_seek; double seek_time;  /* ms */
    double buffer_time;              /* s */
    int generation;
    struct VDec *vdec; int vcodec, frame_id, has_frame; int vadj;   /* VP6 FLV size adjustment byte */ u32 frame_version;
    size_t audio_pos; void *mp3; void *aac; int volume;   /* audio: the next tag to play, the MP3 decoder, 0..100 */
    /* AVM1 (Flash Player): Play.Start and Buffer.Full wait for the data and for
       playback; the end of the stream is Play.Stop, then Buffer.Empty a tick later */
    int start_pending, full_pending, ended, empty_pending;
    double last_ts, data_end_ts;      /* ms: the last tag played, the last tag loaded */
} NetStream;

NetStream *ns_new(Player *p, int avm2, const NSCallbacks *cb, void *ud);
void ns_free(NetStream *s);
void ns_play(NetStream *s, const char *url);     /* url NULL: resume the current source */
void ns_pause(NetStream *s, int notify);
void ns_resume(NetStream *s);
void ns_toggle(NetStream *s);
void ns_seek(NetStream *s, double ms, int notify);
const Bitmap *ns_frame(NetStream *s);            /* the last decoded video frame */
size_t ns_bytes_loaded(NetStream *s);
size_t ns_bytes_total(NetStream *s);
double ns_buffer_length(NetStream *s);          /* s of data loaded ahead of the play head */
void ns_tick_all(Player *p, int avm2, double dt_ms);
void ns_process_loads(Player *p, int avm2);
void ns_mark_active(Player *p, int avm2, void (*fn)(void *ctx, void *ud), void *ctx);

#endif
