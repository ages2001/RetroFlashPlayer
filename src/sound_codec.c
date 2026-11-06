/* sound_codec.c - PCM and SWF-ADPCM decoders (MP3 lives in mp3.c) */
#include "audio.h"
#include "reader.h"
#include <stdlib.h>
#include <string.h>

int decode_pcm(const u8 *d, size_t n, int bits16, int stereo, int big_endian, s16 **out) {
    int ch = stereo ? 2 : 1, frames, i;
    s16 *pcm;
    if (bits16) frames = (int)(n / (2 * ch)); else frames = (int)(n / ch);
    if (frames <= 0) { *out = NULL; return 0; }
    pcm = (s16*)malloc((size_t)frames * ch * sizeof(s16));
    if (!pcm) { *out = NULL; return 0; }
    for (i = 0; i < frames * ch; i++) {
        if (bits16) {
            if (big_endian) pcm[i] = (s16)((d[i * 2] << 8) | d[i * 2 + 1]);
            else pcm[i] = (s16)(d[i * 2] | (d[i * 2 + 1] << 8));
        } else pcm[i] = (s16)((d[i] - 128) << 8);
    }
    *out = pcm;
    return frames;
}

/* SWF ADPCM: IMA-style with 2..5 bit codes, 4096-sample blocks */
static const int ima_index_adjust[4][16] = {
    { -1, 2, -1, 2 },
    { -1, -1, 2, 4, -1, -1, 2, 4 },
    { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 },
    { -1, -1, -1, -1, -1, -1, -1, -1, 1, 2, 4, 6, 8, 10, 13, 16 }
};
static const int ima_step[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,
    130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,
    1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,
    5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,
    27086,29794,32767 };

int decode_adpcm(const u8 *d, size_t n, int stereo, s16 **out) {
    Reader r;
    int nbits, ch = stereo ? 2 : 1, cap = 0, frames = 0;
    s16 *pcm = NULL;
    reader_init(&r, d, n);
    nbits = (int)rb_u(&r, 2) + 2;
    while (!reader_eof(&r)) {
        int sample[2], index[2], c, k;
        for (c = 0; c < ch; c++) {
            sample[c] = rb_s(&r, 16);
            index[c] = (int)rb_u(&r, 6);
            if (index[c] > 88) index[c] = 88;
        }
        if (reader_eof(&r)) break;
        for (k = 0; k < 4096; k++) {
            if (frames + 1 > cap) {
                int nc = cap ? cap * 2 : 8192;
                s16 *np = (s16*)realloc(pcm, (size_t)nc * ch * sizeof(s16));
                if (!np) { free(pcm); *out = NULL; return 0; }
                pcm = np; cap = nc;
            }
            for (c = 0; c < ch; c++) {
                if (k == 0) { pcm[frames * ch + c] = (s16)sample[c]; continue; }
                {
                    int code, step, delta, sign, mag, i;
                    if (reader_eof(&r)) goto done;
                    code = (int)rb_u(&r, (unsigned)nbits);
                    step = ima_step[index[c]];
                    sign = code & (1 << (nbits - 1));
                    mag = code & ((1 << (nbits - 1)) - 1);
                    delta = step >> (nbits - 1);
                    for (i = nbits - 2; i >= 0; i--) if (mag & (1 << i)) delta += step >> (nbits - 2 - i);
                    if (sign) sample[c] -= delta; else sample[c] += delta;
                    if (sample[c] > 32767) sample[c] = 32767; if (sample[c] < -32768) sample[c] = -32768;
                    index[c] += ima_index_adjust[nbits - 2][code];
                    if (index[c] < 0) index[c] = 0; if (index[c] > 88) index[c] = 88;
                    pcm[frames * ch + c] = (s16)sample[c];
                }
            }
            frames++;
        }
    }
done:
    *out = pcm;
    return frames;
}

/* G.711 (FLV audio formats 7 and 8): 8-bit companded mono or stereo */
static s16 alaw1(u8 a) { int t, seg; a ^= 0x55; t = (a & 15) << 4; seg = (a & 0x70) >> 4; if (seg == 0) t += 8; else if (seg == 1) t += 0x108; else { t += 0x108; t <<= seg - 1; } return (s16)((a & 0x80) ? t : -t); }
static s16 ulaw1(u8 u) { int t; u = (u8)~u; t = ((u & 15) << 3) + 0x84; t <<= (u & 0x70) >> 4; return (s16)((u & 0x80) ? (0x84 - t) : (t - 0x84)); }
int decode_g711(const u8 *d, size_t n, int alaw, s16 **out) {
    s16 *o = (s16*)malloc((n ? n : 1) * sizeof(s16)); size_t i;
    *out = o;
    if (!o) return 0;
    for (i = 0; i < n; i++) o[i] = alaw ? alaw1(d[i]) : ulaw1(d[i]);
    return (int)n;   /* samples (the caller divides by the channel count) */
}
