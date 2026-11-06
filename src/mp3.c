/* mp3.c - MPEG-1/2 Layer III via minimp3 (CC0 / public domain,
 * src/thirdparty/minimp3.h).  This is the one piece of FLASHNT not written
 * from scratch: a correct Layer III decoder needs the ISO Huffman tables
 * verbatim.  Swap in another decoder by implementing decode_mp3(). */
#include "audio.h"
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#define MINIMP3_ONLY_MP3
#include "thirdparty/minimp3.h"
#include <stdlib.h>
#include <string.h>

int decode_mp3(const u8 *d, size_t n, int *rate, int *stereo, s16 **out) {
    mp3dec_t dec;
    mp3dec_frame_info_t info;
    s16 pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    s16 *buf = NULL; int cap = 0, frames = 0, ch = 0;
    size_t pos = 0;
    mp3dec_init(&dec);
    while (pos < n) {
        int got = mp3dec_decode_frame(&dec, d + pos, (int)(n - pos), pcm, &info);
        if (info.frame_bytes <= 0) break;
        pos += (size_t)info.frame_bytes;
        if (got <= 0) continue;
        if (!ch) { ch = info.channels; *rate = info.hz; *stereo = ch == 2; }
        if (info.channels != ch) continue;
        if ((frames + got) * ch > cap) {
            int nc = cap ? cap : 65536; s16 *nb;
            while (nc < (frames + got) * ch) nc *= 2;
            nb = (s16*)realloc(buf, (size_t)nc * sizeof(s16));
            if (!nb) break;
            buf = nb; cap = nc;
        }
        memcpy(buf + frames * ch, pcm, (size_t)got * ch * sizeof(s16));
        frames += got;
    }
    *out = buf;
    return frames;
}

/* a decoder kept across packets (stream sounds, FLV audio): frames may borrow
   bits from earlier packets, and a frame split over two packets is completed */
typedef struct { mp3dec_t dec; u8 *pend; size_t npend; } Mp3Stream;
void *mp3s_new(void) { Mp3Stream *s = (Mp3Stream*)calloc(1, sizeof(Mp3Stream)); if (s) mp3dec_init(&s->dec); return s; }
void mp3s_free(void *st) { Mp3Stream *s = (Mp3Stream*)st; if (s) { free(s->pend); free(s); } }
int mp3s_decode(void *st, const u8 *d, size_t n, int *rate, int *stereo, s16 **out) {
    Mp3Stream *s = (Mp3Stream*)st; mp3dec_frame_info_t info; s16 pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    s16 *buf = NULL; int cap = 0, frames = 0, ch = 0; size_t pos = 0, len; u8 *data;
    *out = NULL;
    if (!s) return decode_mp3(d, n, rate, stereo, out);
    len = s->npend + n;
    data = (u8*)malloc(len ? len : 1);
    if (!data) return 0;
    if (s->npend) memcpy(data, s->pend, s->npend);
    memcpy(data + s->npend, d, n);
    free(s->pend); s->pend = NULL; s->npend = 0;
    while (pos < len) {
        int got = mp3dec_decode_frame(&s->dec, data + pos, (int)(len - pos), pcm, &info);
        if (info.frame_bytes <= 0) {   /* an incomplete frame: keep it for the next packet */
            if (len - pos < 4096) { s->pend = (u8*)malloc(len - pos); if (s->pend) { memcpy(s->pend, data + pos, len - pos); s->npend = len - pos; } }
            break;
        }
        pos += (size_t)info.frame_bytes;
        if (got <= 0) continue;
        if (!ch) { ch = info.channels; *rate = info.hz; *stereo = ch == 2; }
        if (info.channels != ch) continue;
        if ((frames + got) * ch > cap) {
            int nc = cap ? cap : 16384; s16 *nb;
            while (nc < (frames + got) * ch) nc *= 2;
            nb = (s16*)realloc(buf, (size_t)nc * sizeof(s16));
            if (!nb) break;
            buf = nb; cap = nc;
        }
        memcpy(buf + frames * ch, pcm, (size_t)got * ch * sizeof(s16));
        frames += got;
    }
    free(data);
    *out = buf;
    return frames;
}
