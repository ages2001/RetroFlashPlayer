/* aac.c - AAC-LC decoder for FLV / NetStream audio (ISO/IEC 14496-3 GA,
 * AudioSpecificConfig + raw_data_block): section data, scalefactors,
 * Huffman spectral data, pulse data, TNS, PNS, M/S and intensity stereo,
 * IMDCT (via a DCT-IV on an FFT) with sine / KBD windows and block switching.
 * HE-AAC streams decode their AAC-LC core (no SBR / PS).  The Huffman code
 * tables below are those of ISO/IEC 14496-3 (spectral codebooks 1..11 and
 * the scalefactor codebook). */
#include "audio.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const unsigned char hcb1_len[81] = {
    11, 9, 11, 10, 7, 10, 11, 9, 11, 10, 7, 10, 7, 5, 7, 9,
    7, 10, 11, 9, 11, 9, 7, 9, 11, 9, 11, 9, 7, 9, 7, 5,
    7, 9, 7, 9, 7, 5, 7, 5, 1, 5, 7, 5, 7, 9, 7, 9,
    7, 5, 7, 9, 7, 9, 11, 9, 11, 9, 7, 9, 11, 9, 11, 10,
    7, 9, 7, 5, 7, 9, 7, 10, 11, 9, 11, 10, 7, 9, 11, 9,
    11
};
static const unsigned long hcb1_code[81] = {
    0x7f8, 0x1f1, 0x7fd, 0x3f5, 0x68, 0x3f0, 0x7f7, 0x1ec,
    0x7f5, 0x3f1, 0x72, 0x3f4, 0x74, 0x11, 0x76, 0x1eb,
    0x6c, 0x3f6, 0x7fc, 0x1e1, 0x7f1, 0x1f0, 0x61, 0x1f6,
    0x7f2, 0x1ea, 0x7fb, 0x1f2, 0x69, 0x1ed, 0x77, 0x17,
    0x6f, 0x1e6, 0x64, 0x1e5, 0x67, 0x15, 0x62, 0x12,
    0x0, 0x14, 0x65, 0x16, 0x6d, 0x1e9, 0x63, 0x1e4,
    0x6b, 0x13, 0x71, 0x1e3, 0x70, 0x1f3, 0x7fe, 0x1e7,
    0x7f3, 0x1ef, 0x60, 0x1ee, 0x7f0, 0x1e2, 0x7fa, 0x3f3,
    0x6a, 0x1e8, 0x75, 0x10, 0x73, 0x1f4, 0x6e, 0x3f7,
    0x7f6, 0x1e0, 0x7f9, 0x3f2, 0x66, 0x1f5, 0x7ff, 0x1f7,
    0x7f4
};
static const unsigned char hcb2_len[81] = {
    9, 7, 9, 8, 6, 8, 9, 8, 9, 8, 6, 7, 6, 5, 6, 7,
    6, 8, 9, 7, 8, 8, 6, 8, 9, 7, 9, 8, 6, 7, 6, 5,
    6, 7, 6, 8, 6, 5, 6, 5, 3, 5, 6, 5, 6, 8, 6, 7,
    6, 5, 6, 8, 6, 8, 9, 7, 9, 8, 6, 8, 8, 7, 9, 8,
    6, 7, 6, 4, 6, 8, 6, 7, 9, 7, 9, 7, 6, 8, 9, 7,
    9
};
static const unsigned long hcb2_code[81] = {
    0x1f3, 0x6f, 0x1fd, 0xeb, 0x23, 0xea, 0x1f7, 0xe8,
    0x1fa, 0xf2, 0x2d, 0x70, 0x20, 0x6, 0x2b, 0x6e,
    0x28, 0xe9, 0x1f9, 0x66, 0xf8, 0xe7, 0x1b, 0xf1,
    0x1f4, 0x6b, 0x1f5, 0xec, 0x2a, 0x6c, 0x2c, 0xa,
    0x27, 0x67, 0x1a, 0xf5, 0x24, 0x8, 0x1f, 0x9,
    0x0, 0x7, 0x1d, 0xb, 0x30, 0xef, 0x1c, 0x64,
    0x1e, 0xc, 0x29, 0xf3, 0x2f, 0xf0, 0x1fc, 0x71,
    0x1f2, 0xf4, 0x21, 0xe6, 0xf7, 0x68, 0x1f8, 0xee,
    0x22, 0x65, 0x31, 0x2, 0x26, 0xed, 0x25, 0x6a,
    0x1fb, 0x72, 0x1fe, 0x69, 0x2e, 0xf6, 0x1ff, 0x6d,
    0x1f6
};
static const unsigned char hcb3_len[81] = {
    1, 4, 8, 4, 5, 8, 9, 9, 10, 4, 6, 9, 6, 6, 9, 9,
    9, 10, 9, 10, 13, 9, 9, 11, 11, 10, 12, 4, 6, 10, 6, 7,
    10, 10, 10, 12, 5, 7, 11, 6, 7, 10, 9, 9, 11, 9, 10, 13,
    8, 9, 12, 10, 11, 12, 8, 10, 15, 9, 11, 15, 13, 14, 16, 8,
    10, 14, 9, 10, 14, 12, 12, 15, 11, 12, 16, 10, 11, 15, 12, 12,
    15
};
static const unsigned long hcb3_code[81] = {
    0x0, 0x9, 0xef, 0xb, 0x19, 0xf0, 0x1eb, 0x1e6,
    0x3f2, 0xa, 0x35, 0x1ef, 0x34, 0x37, 0x1e9, 0x1ed,
    0x1e7, 0x3f3, 0x1ee, 0x3ed, 0x1ffa, 0x1ec, 0x1f2, 0x7f9,
    0x7f8, 0x3f8, 0xff8, 0x8, 0x38, 0x3f6, 0x36, 0x75,
    0x3f1, 0x3eb, 0x3ec, 0xff4, 0x18, 0x76, 0x7f4, 0x39,
    0x74, 0x3ef, 0x1f3, 0x1f4, 0x7f6, 0x1e8, 0x3ea, 0x1ffc,
    0xf2, 0x1f1, 0xffb, 0x3f5, 0x7f3, 0xffc, 0xee, 0x3f7,
    0x7ffe, 0x1f0, 0x7f5, 0x7ffd, 0x1ffb, 0x3ffa, 0xffff, 0xf1,
    0x3f0, 0x3ffc, 0x1ea, 0x3ee, 0x3ffb, 0xff6, 0xffa, 0x7ffc,
    0x7f2, 0xff5, 0xfffe, 0x3f4, 0x7f7, 0x7ffb, 0xff7, 0xff9,
    0x7ffa
};
static const unsigned char hcb4_len[81] = {
    4, 5, 8, 5, 4, 8, 9, 8, 11, 5, 5, 8, 5, 4, 8, 8,
    7, 10, 9, 8, 11, 8, 8, 10, 11, 10, 11, 4, 5, 8, 4, 4,
    8, 8, 8, 10, 4, 4, 8, 4, 4, 7, 8, 7, 9, 8, 8, 10,
    7, 7, 9, 10, 9, 10, 8, 8, 11, 8, 7, 10, 11, 10, 12, 8,
    7, 10, 7, 7, 9, 10, 9, 11, 11, 10, 12, 10, 9, 11, 11, 10,
    11
};
static const unsigned long hcb4_code[81] = {
    0x7, 0x16, 0xf6, 0x18, 0x8, 0xef, 0x1ef, 0xf3,
    0x7f8, 0x19, 0x17, 0xed, 0x15, 0x1, 0xe2, 0xf0,
    0x70, 0x3f0, 0x1ee, 0xf1, 0x7fa, 0xee, 0xe4, 0x3f2,
    0x7f6, 0x3ef, 0x7fd, 0x5, 0x14, 0xf2, 0x9, 0x4,
    0xe5, 0xf4, 0xe8, 0x3f4, 0x6, 0x2, 0xe7, 0x3,
    0x0, 0x6b, 0xe3, 0x69, 0x1f3, 0xeb, 0xe6, 0x3f6,
    0x6e, 0x6a, 0x1f4, 0x3ec, 0x1f0, 0x3f9, 0xf5, 0xec,
    0x7fb, 0xea, 0x6f, 0x3f7, 0x7f9, 0x3f3, 0xfff, 0xe9,
    0x6d, 0x3f8, 0x6c, 0x68, 0x1f5, 0x3ee, 0x1f2, 0x7f4,
    0x7f7, 0x3f1, 0xffe, 0x3ed, 0x1f1, 0x7f5, 0x7fe, 0x3f5,
    0x7fc
};
static const unsigned char hcb5_len[81] = {
    13, 12, 11, 11, 10, 11, 11, 12, 13, 12, 11, 10, 9, 8, 9, 10,
    11, 12, 12, 10, 9, 8, 7, 8, 9, 10, 11, 11, 9, 8, 5, 4,
    5, 8, 9, 11, 10, 8, 7, 4, 1, 4, 7, 8, 11, 11, 9, 8,
    5, 4, 5, 8, 9, 11, 11, 10, 9, 8, 7, 8, 9, 10, 11, 12,
    11, 10, 9, 8, 9, 10, 11, 12, 13, 12, 12, 11, 10, 10, 11, 12,
    13
};
static const unsigned long hcb5_code[81] = {
    0x1fff, 0xff7, 0x7f4, 0x7e8, 0x3f1, 0x7ee, 0x7f9, 0xff8,
    0x1ffd, 0xffd, 0x7f1, 0x3e8, 0x1e8, 0xf0, 0x1ec, 0x3ee,
    0x7f2, 0xffa, 0xff4, 0x3ef, 0x1f2, 0xe8, 0x70, 0xec,
    0x1f0, 0x3ea, 0x7f3, 0x7eb, 0x1eb, 0xea, 0x1a, 0x8,
    0x19, 0xee, 0x1ef, 0x7ed, 0x3f0, 0xf2, 0x73, 0xb,
    0x0, 0xa, 0x71, 0xf3, 0x7e9, 0x7ef, 0x1ee, 0xef,
    0x18, 0x9, 0x1b, 0xeb, 0x1e9, 0x7ec, 0x7f6, 0x3eb,
    0x1f3, 0xed, 0x72, 0xe9, 0x1f1, 0x3ed, 0x7f7, 0xff6,
    0x7f0, 0x3e9, 0x1ed, 0xf1, 0x1ea, 0x3ec, 0x7f8, 0xff9,
    0x1ffc, 0xffc, 0xff5, 0x7ea, 0x3f3, 0x3f2, 0x7f5, 0xffb,
    0x1ffe
};
static const unsigned char hcb6_len[81] = {
    11, 10, 9, 9, 9, 9, 9, 10, 11, 10, 9, 8, 7, 7, 7, 8,
    9, 10, 9, 8, 6, 6, 6, 6, 6, 8, 9, 9, 7, 6, 4, 4,
    4, 6, 7, 9, 9, 7, 6, 4, 4, 4, 6, 7, 9, 9, 7, 6,
    4, 4, 4, 6, 7, 9, 9, 8, 6, 6, 6, 6, 6, 8, 9, 10,
    9, 8, 7, 7, 7, 7, 8, 10, 11, 10, 9, 9, 9, 9, 9, 10,
    11
};
static const unsigned long hcb6_code[81] = {
    0x7fe, 0x3fd, 0x1f1, 0x1eb, 0x1f4, 0x1ea, 0x1f0, 0x3fc,
    0x7fd, 0x3f6, 0x1e5, 0xea, 0x6c, 0x71, 0x68, 0xf0,
    0x1e6, 0x3f7, 0x1f3, 0xef, 0x32, 0x27, 0x28, 0x26,
    0x31, 0xeb, 0x1f7, 0x1e8, 0x6f, 0x2e, 0x8, 0x4,
    0x6, 0x29, 0x6b, 0x1ee, 0x1ef, 0x72, 0x2d, 0x2,
    0x0, 0x3, 0x2f, 0x73, 0x1fa, 0x1e7, 0x6e, 0x2b,
    0x7, 0x1, 0x5, 0x2c, 0x6d, 0x1ec, 0x1f9, 0xee,
    0x30, 0x24, 0x2a, 0x25, 0x33, 0xec, 0x1f2, 0x3f8,
    0x1e4, 0xed, 0x6a, 0x70, 0x69, 0x74, 0xf1, 0x3fa,
    0x7ff, 0x3f9, 0x1f6, 0x1ed, 0x1f8, 0x1e9, 0x1f5, 0x3fb,
    0x7fc
};
static const unsigned char hcb7_len[64] = {
    1, 3, 6, 7, 8, 9, 10, 11, 3, 4, 6, 7, 8, 8, 9, 9,
    6, 6, 7, 8, 8, 9, 9, 10, 7, 7, 8, 8, 9, 9, 10, 10,
    8, 8, 9, 9, 10, 10, 10, 11, 9, 8, 9, 9, 10, 10, 11, 11,
    10, 9, 9, 10, 10, 11, 12, 12, 11, 10, 10, 10, 11, 11, 12, 12
};
static const unsigned long hcb7_code[64] = {
    0x0, 0x5, 0x37, 0x74, 0xf2, 0x1eb, 0x3ed, 0x7f7,
    0x4, 0xc, 0x35, 0x71, 0xec, 0xee, 0x1ee, 0x1f5,
    0x36, 0x34, 0x72, 0xea, 0xf1, 0x1e9, 0x1f3, 0x3f5,
    0x73, 0x70, 0xeb, 0xf0, 0x1f1, 0x1f0, 0x3ec, 0x3fa,
    0xf3, 0xed, 0x1e8, 0x1ef, 0x3ef, 0x3f1, 0x3f9, 0x7fb,
    0x1ed, 0xef, 0x1ea, 0x1f2, 0x3f3, 0x3f8, 0x7f9, 0x7fc,
    0x3ee, 0x1ec, 0x1f4, 0x3f4, 0x3f7, 0x7f8, 0xffd, 0xffe,
    0x7f6, 0x3f0, 0x3f2, 0x3f6, 0x7fa, 0x7fd, 0xffc, 0xfff
};
static const unsigned char hcb8_len[64] = {
    5, 4, 5, 6, 7, 8, 9, 10, 4, 3, 4, 5, 6, 7, 7, 8,
    5, 4, 4, 5, 6, 7, 7, 8, 6, 5, 5, 6, 6, 7, 8, 8,
    7, 6, 6, 6, 7, 7, 8, 9, 8, 7, 6, 7, 7, 8, 8, 10,
    9, 7, 7, 8, 8, 8, 9, 9, 10, 8, 8, 8, 9, 9, 9, 10
};
static const unsigned long hcb8_code[64] = {
    0xe, 0x5, 0x10, 0x30, 0x6f, 0xf1, 0x1fa, 0x3fe,
    0x3, 0x0, 0x4, 0x12, 0x2c, 0x6a, 0x75, 0xf8,
    0xf, 0x2, 0x6, 0x14, 0x2e, 0x69, 0x72, 0xf5,
    0x2f, 0x11, 0x13, 0x2a, 0x32, 0x6c, 0xec, 0xfa,
    0x71, 0x2b, 0x2d, 0x31, 0x6d, 0x70, 0xf2, 0x1f9,
    0xef, 0x68, 0x33, 0x6b, 0x6e, 0xee, 0xf9, 0x3fc,
    0x1f8, 0x74, 0x73, 0xed, 0xf0, 0xf6, 0x1f6, 0x1fd,
    0x3fd, 0xf3, 0xf4, 0xf7, 0x1f7, 0x1fb, 0x1fc, 0x3ff
};
static const unsigned char hcb9_len[169] = {
    1, 3, 6, 8, 9, 10, 10, 11, 11, 12, 12, 13, 13, 3, 4, 6,
    7, 8, 8, 9, 10, 10, 10, 11, 12, 12, 6, 6, 7, 8, 8, 9,
    10, 10, 10, 11, 12, 12, 12, 8, 7, 8, 9, 9, 10, 10, 11, 11,
    11, 12, 12, 13, 9, 8, 9, 9, 10, 10, 11, 11, 11, 12, 12, 12,
    13, 10, 9, 9, 10, 11, 11, 11, 12, 11, 12, 12, 13, 13, 11, 9,
    10, 11, 11, 11, 12, 12, 12, 12, 13, 13, 13, 11, 10, 10, 11, 11,
    12, 12, 13, 13, 13, 13, 13, 13, 11, 10, 10, 11, 11, 11, 12, 12,
    13, 13, 14, 13, 14, 11, 10, 11, 11, 12, 12, 12, 12, 13, 13, 14,
    14, 14, 12, 11, 11, 12, 12, 12, 13, 13, 13, 14, 14, 14, 15, 12,
    11, 12, 12, 12, 13, 13, 13, 13, 14, 14, 15, 15, 13, 12, 12, 12,
    13, 13, 13, 13, 14, 14, 14, 14, 15
};
static const unsigned long hcb9_code[169] = {
    0x0, 0x5, 0x37, 0xe7, 0x1de, 0x3ce, 0x3d9, 0x7c8,
    0x7cd, 0xfc8, 0xfdd, 0x1fe4, 0x1fec, 0x4, 0xc, 0x35,
    0x72, 0xea, 0xed, 0x1e2, 0x3d1, 0x3d3, 0x3e0, 0x7d8,
    0xfcf, 0xfd5, 0x36, 0x34, 0x71, 0xe8, 0xec, 0x1e1,
    0x3cf, 0x3dd, 0x3db, 0x7d0, 0xfc7, 0xfd4, 0xfe4, 0xe6,
    0x70, 0xe9, 0x1dd, 0x1e3, 0x3d2, 0x3dc, 0x7cc, 0x7ca,
    0x7de, 0xfd8, 0xfea, 0x1fdb, 0x1df, 0xeb, 0x1dc, 0x1e6,
    0x3d5, 0x3de, 0x7cb, 0x7dd, 0x7dc, 0xfcd, 0xfe2, 0xfe7,
    0x1fe1, 0x3d0, 0x1e0, 0x1e4, 0x3d6, 0x7c5, 0x7d1, 0x7db,
    0xfd2, 0x7e0, 0xfd9, 0xfeb, 0x1fe3, 0x1fe9, 0x7c4, 0x1e5,
    0x3d7, 0x7c6, 0x7cf, 0x7da, 0xfcb, 0xfda, 0xfe3, 0xfe9,
    0x1fe6, 0x1ff3, 0x1ff7, 0x7d3, 0x3d8, 0x3e1, 0x7d4, 0x7d9,
    0xfd3, 0xfde, 0x1fdd, 0x1fd9, 0x1fe2, 0x1fea, 0x1ff1, 0x1ff6,
    0x7d2, 0x3d4, 0x3da, 0x7c7, 0x7d7, 0x7e2, 0xfce, 0xfdb,
    0x1fd8, 0x1fee, 0x3ff0, 0x1ff4, 0x3ff2, 0x7e1, 0x3df, 0x7c9,
    0x7d6, 0xfca, 0xfd0, 0xfe5, 0xfe6, 0x1feb, 0x1fef, 0x3ff3,
    0x3ff4, 0x3ff5, 0xfe0, 0x7ce, 0x7d5, 0xfc6, 0xfd1, 0xfe1,
    0x1fe0, 0x1fe8, 0x1ff0, 0x3ff1, 0x3ff8, 0x3ff6, 0x7ffc, 0xfe8,
    0x7df, 0xfc9, 0xfd7, 0xfdc, 0x1fdc, 0x1fdf, 0x1fed, 0x1ff5,
    0x3ff9, 0x3ffb, 0x7ffd, 0x7ffe, 0x1fe7, 0xfcc, 0xfd6, 0xfdf,
    0x1fde, 0x1fda, 0x1fe5, 0x1ff2, 0x3ffa, 0x3ff7, 0x3ffc, 0x3ffd,
    0x7fff
};
static const unsigned char hcb10_len[169] = {
    6, 5, 6, 6, 7, 8, 9, 10, 10, 10, 11, 11, 12, 5, 4, 4,
    5, 6, 7, 7, 8, 8, 9, 10, 10, 11, 6, 4, 5, 5, 6, 6,
    7, 8, 8, 9, 9, 10, 10, 6, 5, 5, 5, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 7, 6, 6, 6, 6, 7, 7, 8, 8, 9, 9, 10,
    10, 8, 7, 6, 7, 7, 7, 8, 8, 8, 9, 10, 10, 11, 9, 7,
    7, 7, 7, 8, 8, 9, 9, 9, 10, 10, 11, 9, 8, 8, 8, 8,
    8, 9, 9, 9, 10, 10, 11, 11, 9, 8, 8, 8, 8, 8, 9, 9,
    10, 10, 10, 11, 11, 10, 9, 9, 9, 9, 9, 9, 10, 10, 10, 11,
    11, 12, 10, 9, 9, 9, 9, 10, 10, 10, 10, 11, 11, 11, 12, 11,
    10, 9, 10, 10, 10, 10, 10, 11, 11, 11, 11, 12, 11, 10, 10, 10,
    10, 10, 10, 11, 11, 12, 12, 12, 12
};
static const unsigned long hcb10_code[169] = {
    0x22, 0x8, 0x1d, 0x26, 0x5f, 0xd3, 0x1cf, 0x3d0,
    0x3d7, 0x3ed, 0x7f0, 0x7f6, 0xffd, 0x7, 0x0, 0x1,
    0x9, 0x20, 0x54, 0x60, 0xd5, 0xdc, 0x1d4, 0x3cd,
    0x3de, 0x7e7, 0x1c, 0x2, 0x6, 0xc, 0x1e, 0x28,
    0x5b, 0xcd, 0xd9, 0x1ce, 0x1dc, 0x3d9, 0x3f1, 0x25,
    0xb, 0xa, 0xd, 0x24, 0x57, 0x61, 0xcc, 0xdd,
    0x1cc, 0x1de, 0x3d3, 0x3e7, 0x5d, 0x21, 0x1f, 0x23,
    0x27, 0x59, 0x64, 0xd8, 0xdf, 0x1d2, 0x1e2, 0x3dd,
    0x3ee, 0xd1, 0x55, 0x29, 0x56, 0x58, 0x62, 0xce,
    0xe0, 0xe2, 0x1da, 0x3d4, 0x3e3, 0x7eb, 0x1c9, 0x5e,
    0x5a, 0x5c, 0x63, 0xca, 0xda, 0x1c7, 0x1ca, 0x1e0,
    0x3db, 0x3e8, 0x7ec, 0x1e3, 0xd2, 0xcb, 0xd0, 0xd7,
    0xdb, 0x1c6, 0x1d5, 0x1d8, 0x3ca, 0x3da, 0x7ea, 0x7f1,
    0x1e1, 0xd4, 0xcf, 0xd6, 0xde, 0xe1, 0x1d0, 0x1d6,
    0x3d1, 0x3d5, 0x3f2, 0x7ee, 0x7fb, 0x3e9, 0x1cd, 0x1c8,
    0x1cb, 0x1d1, 0x1d7, 0x1df, 0x3cf, 0x3e0, 0x3ef, 0x7e6,
    0x7f8, 0xffa, 0x3eb, 0x1dd, 0x1d3, 0x1d9, 0x1db, 0x3d2,
    0x3cc, 0x3dc, 0x3ea, 0x7ed, 0x7f3, 0x7f9, 0xff9, 0x7f2,
    0x3ce, 0x1e4, 0x3cb, 0x3d8, 0x3d6, 0x3e2, 0x3e5, 0x7e8,
    0x7f4, 0x7f5, 0x7f7, 0xffb, 0x7fa, 0x3ec, 0x3df, 0x3e1,
    0x3e4, 0x3e6, 0x3f0, 0x7e9, 0x7ef, 0xff8, 0xffe, 0xffc,
    0xfff
};
static const unsigned char hcb11_len[289] = {
    4, 5, 6, 7, 8, 8, 9, 10, 10, 10, 11, 11, 12, 11, 12, 12,
    10, 5, 4, 5, 6, 7, 7, 8, 8, 9, 9, 9, 10, 10, 10, 10,
    11, 8, 6, 5, 5, 6, 7, 7, 8, 8, 8, 9, 9, 9, 10, 10,
    10, 10, 8, 7, 6, 6, 6, 7, 7, 8, 8, 8, 9, 9, 9, 10,
    10, 10, 10, 8, 8, 7, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9,
    10, 10, 10, 10, 8, 8, 7, 7, 7, 7, 8, 8, 8, 9, 9, 9,
    9, 10, 10, 10, 10, 8, 9, 8, 8, 8, 8, 8, 8, 8, 9, 9,
    9, 10, 10, 10, 10, 10, 8, 9, 8, 8, 8, 8, 8, 8, 9, 9,
    9, 10, 10, 10, 10, 10, 10, 8, 10, 9, 8, 8, 9, 9, 9, 9,
    9, 10, 10, 10, 10, 10, 10, 11, 8, 10, 9, 9, 9, 9, 9, 9,
    9, 10, 10, 10, 10, 10, 10, 11, 11, 8, 11, 9, 9, 9, 9, 9,
    9, 10, 10, 10, 10, 10, 11, 10, 11, 11, 8, 11, 10, 9, 9, 10,
    9, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 8, 11, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 9, 11, 10, 9,
    9, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 9, 11, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 9, 12,
    10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 12, 12, 9,
    9, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 9,
    5
};
static const unsigned long hcb11_code[289] = {
    0x0, 0x6, 0x19, 0x3d, 0x9c, 0xc6, 0x1a7, 0x390,
    0x3c2, 0x3df, 0x7e6, 0x7f3, 0xffb, 0x7ec, 0xffa, 0xffe,
    0x38e, 0x5, 0x1, 0x8, 0x14, 0x37, 0x42, 0x92,
    0xaf, 0x191, 0x1a5, 0x1b5, 0x39e, 0x3c0, 0x3a2, 0x3cd,
    0x7d6, 0xae, 0x17, 0x7, 0x9, 0x18, 0x39, 0x40,
    0x8e, 0xa3, 0xb8, 0x199, 0x1ac, 0x1c1, 0x3b1, 0x396,
    0x3be, 0x3ca, 0x9d, 0x3c, 0x15, 0x16, 0x1a, 0x3b,
    0x44, 0x91, 0xa5, 0xbe, 0x196, 0x1ae, 0x1b9, 0x3a1,
    0x391, 0x3a5, 0x3d5, 0x94, 0x9a, 0x36, 0x38, 0x3a,
    0x41, 0x8c, 0x9b, 0xb0, 0xc3, 0x19e, 0x1ab, 0x1bc,
    0x39f, 0x38f, 0x3a9, 0x3cf, 0x93, 0xbf, 0x3e, 0x3f,
    0x43, 0x45, 0x9e, 0xa7, 0xb9, 0x194, 0x1a2, 0x1ba,
    0x1c3, 0x3a6, 0x3a7, 0x3bb, 0x3d4, 0x9f, 0x1a0, 0x8f,
    0x8d, 0x90, 0x98, 0xa6, 0xb6, 0xc4, 0x19f, 0x1af,
    0x1bf, 0x399, 0x3bf, 0x3b4, 0x3c9, 0x3e7, 0xa8, 0x1b6,
    0xab, 0xa4, 0xaa, 0xb2, 0xc2, 0xc5, 0x198, 0x1a4,
    0x1b8, 0x38c, 0x3a4, 0x3c4, 0x3c6, 0x3dd, 0x3e8, 0xad,
    0x3af, 0x192, 0xbd, 0xbc, 0x18e, 0x197, 0x19a, 0x1a3,
    0x1b1, 0x38d, 0x398, 0x3b7, 0x3d3, 0x3d1, 0x3db, 0x7dd,
    0xb4, 0x3de, 0x1a9, 0x19b, 0x19c, 0x1a1, 0x1aa, 0x1ad,
    0x1b3, 0x38b, 0x3b2, 0x3b8, 0x3ce, 0x3e1, 0x3e0, 0x7d2,
    0x7e5, 0xb7, 0x7e3, 0x1bb, 0x1a8, 0x1a6, 0x1b0, 0x1b2,
    0x1b7, 0x39b, 0x39a, 0x3ba, 0x3b5, 0x3d6, 0x7d7, 0x3e4,
    0x7d8, 0x7ea, 0xba, 0x7e8, 0x3a0, 0x1bd, 0x1b4, 0x38a,
    0x1c4, 0x392, 0x3aa, 0x3b0, 0x3bc, 0x3d7, 0x7d4, 0x7dc,
    0x7db, 0x7d5, 0x7f0, 0xc1, 0x7fb, 0x3c8, 0x3a3, 0x395,
    0x39d, 0x3ac, 0x3ae, 0x3c5, 0x3d8, 0x3e2, 0x3e6, 0x7e4,
    0x7e7, 0x7e0, 0x7e9, 0x7f7, 0x190, 0x7f2, 0x393, 0x1be,
    0x1c0, 0x394, 0x397, 0x3ad, 0x3c3, 0x3c1, 0x3d2, 0x7da,
    0x7d9, 0x7df, 0x7eb, 0x7f4, 0x7fa, 0x195, 0x7f8, 0x3bd,
    0x39c, 0x3ab, 0x3a8, 0x3b3, 0x3b9, 0x3d0, 0x3e3, 0x3e5,
    0x7e2, 0x7de, 0x7ed, 0x7f1, 0x7f9, 0x7fc, 0x193, 0xffd,
    0x3dc, 0x3b6, 0x3c7, 0x3cc, 0x3cb, 0x3d9, 0x3da, 0x7d3,
    0x7e1, 0x7ee, 0x7ef, 0x7f5, 0x7f6, 0xffc, 0xfff, 0x19d,
    0x1c2, 0xb5, 0xa1, 0x96, 0x97, 0x95, 0x99, 0xa0,
    0xa2, 0xac, 0xa9, 0xb1, 0xb3, 0xbb, 0xc0, 0x18f,
    0x4
};
static const unsigned char hcb_sf_len[121] = {
    18, 18, 18, 18, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
    19, 19, 19, 18, 19, 18, 17, 17, 16, 17, 16, 16, 16, 16, 15, 15,
    14, 14, 14, 14, 14, 14, 13, 13, 12, 12, 12, 11, 12, 11, 10, 10,
    10, 9, 9, 8, 8, 8, 7, 6, 6, 5, 4, 3, 1, 4, 4, 5,
    6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 10, 11, 11, 11, 11, 12,
    12, 13, 13, 13, 14, 14, 16, 15, 16, 15, 18, 19, 19, 19, 19, 19,
    19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
    19, 19, 19, 19, 19, 19, 19, 19, 19
};
static const unsigned long hcb_sf_code[121] = {
    0x3ffe8, 0x3ffe6, 0x3ffe7, 0x3ffe5, 0x7fff5, 0x7fff1, 0x7ffed, 0x7fff6,
    0x7ffee, 0x7ffef, 0x7fff0, 0x7fffc, 0x7fffd, 0x7ffff, 0x7fffe, 0x7fff7,
    0x7fff8, 0x7fffb, 0x7fff9, 0x3ffe4, 0x7fffa, 0x3ffe3, 0x1ffef, 0x1fff0,
    0xfff5, 0x1ffee, 0xfff2, 0xfff3, 0xfff4, 0xfff1, 0x7ff6, 0x7ff7,
    0x3ff9, 0x3ff5, 0x3ff7, 0x3ff3, 0x3ff6, 0x3ff2, 0x1ff7, 0x1ff5,
    0xff9, 0xff7, 0xff6, 0x7f9, 0xff4, 0x7f8, 0x3f9, 0x3f7,
    0x3f5, 0x1f8, 0x1f7, 0xfa, 0xf8, 0xf6, 0x79, 0x3a,
    0x38, 0x1a, 0xb, 0x4, 0x0, 0xa, 0xc, 0x1b,
    0x39, 0x3b, 0x78, 0x7a, 0xf7, 0xf9, 0x1f6, 0x1f9,
    0x3f4, 0x3f6, 0x3f8, 0x7f5, 0x7f4, 0x7f6, 0x7f7, 0xff5,
    0xff8, 0x1ff4, 0x1ff6, 0x1ff8, 0x3ff8, 0x3ff4, 0xfff0, 0x7ff4,
    0xfff6, 0x7ff5, 0x3ffe2, 0x7ffd9, 0x7ffda, 0x7ffdb, 0x7ffdc, 0x7ffdd,
    0x7ffde, 0x7ffd8, 0x7ffd2, 0x7ffd3, 0x7ffd4, 0x7ffd5, 0x7ffd6, 0x7fff2,
    0x7ffdf, 0x7ffe7, 0x7ffe8, 0x7ffe9, 0x7ffea, 0x7ffeb, 0x7ffe6, 0x7ffe0,
    0x7ffe1, 0x7ffe2, 0x7ffe3, 0x7ffe4, 0x7ffe5, 0x7ffd7, 0x7ffec, 0x7fff4,
    0x7fff3
};

/* ---- bit reader ---- */
typedef struct { const u8 *p; size_t nbits, pos; int err; } Bits;
static unsigned getbits(Bits *b, int n) {
    unsigned v = 0;
    while (n-- > 0) {
        if (b->pos >= b->nbits) { b->err = 1; v <<= 1; continue; }
        v = (v << 1) | ((b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1);
        b->pos++;
    }
    return v;
}

/* ---- Huffman trees built from (code, length) tables ---- */
typedef struct { short *node; int nnodes; } HTree;   /* node[2*i+bit]: >0 next node, <=0 -(symbol) - 1 ... */
static HTree g_tree[12];   /* 0: scalefactors, 1..11 spectral */
static int g_trees_built;
static int tree_build(HTree *t, const unsigned char *len, const unsigned long *code, int n) {
    int i, cap = 2 * n + 2;
    t->node = (short*)malloc((size_t)cap * 2 * sizeof(short));
    if (!t->node) return -1;
    for (i = 0; i < cap * 2; i++) t->node[i] = 0;   /* 0: empty */
    t->nnodes = 1;
    for (i = 0; i < n; i++) {
        int cur = 0, b;
        for (b = len[i] - 1; b >= 0; b--) {
            int bit = (int)((code[i] >> b) & 1), slot = cur * 2 + bit;
            if (b == 0) { t->node[slot] = (short)(-(i + 1)); break; }
            if (t->node[slot] <= 0) { if (t->nnodes >= cap) return -1; t->node[slot] = (short)t->nnodes++; }
            cur = t->node[slot];
        }
    }
    return 0;
}
static int huff(Bits *b, const HTree *t) {
    int cur = 0, g = 0;
    while (g++ < 32) {
        int v = t->node[cur * 2 + (int)getbits(b, 1)];
        if (v < 0) return -v - 1;
        if (v == 0 || b->err) { b->err = 1; return 0; }
        cur = v;
    }
    b->err = 1; return 0;
}
static const unsigned char *const hcb_len[12] = { hcb_sf_len, hcb1_len, hcb2_len, hcb3_len, hcb4_len, hcb5_len, hcb6_len, hcb7_len, hcb8_len, hcb9_len, hcb10_len, hcb11_len };
static const unsigned long *const hcb_code[12] = { hcb_sf_code, hcb1_code, hcb2_code, hcb3_code, hcb4_code, hcb5_code, hcb6_code, hcb7_code, hcb8_code, hcb9_code, hcb10_code, hcb11_code };
static const int hcb_n[12] = { 121, 81, 81, 81, 81, 81, 81, 64, 64, 169, 169, 289 };

/* ---- tables ---- */
static const unsigned short swb_long_96[42] = { 0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 108, 120, 132, 144, 156, 172, 188, 212, 240, 276, 320, 384, 448, 512, 576, 640, 704, 768, 832, 896, 960, 1024 };
static const unsigned short swb_long_64[48] = { 0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 100, 112, 124, 140, 156, 172, 192, 216, 240, 268, 304, 344, 384, 424, 464, 504, 544, 584, 624, 664, 704, 744, 784, 824, 864, 904, 944, 984, 1024 };
static const unsigned short swb_long_48[50] = { 0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72, 80, 88, 96, 108, 120, 132, 144, 160, 176, 196, 216, 240, 264, 292, 320, 352, 384, 416, 448, 480, 512, 544, 576, 608, 640, 672, 704, 736, 768, 800, 832, 864, 896, 928, 1024 };
static const unsigned short swb_long_32[52] = { 0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72, 80, 88, 96, 108, 120, 132, 144, 160, 176, 196, 216, 240, 264, 292, 320, 352, 384, 416, 448, 480, 512, 544, 576, 608, 640, 672, 704, 736, 768, 800, 832, 864, 896, 928, 960, 992, 1024 };
static const unsigned short swb_long_24[48] = { 0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 52, 60, 68, 76, 84, 92, 100, 108, 116, 124, 136, 148, 160, 172, 188, 204, 220, 240, 260, 284, 308, 336, 364, 396, 432, 468, 508, 552, 600, 652, 704, 768, 832, 896, 960, 1024 };
static const unsigned short swb_long_16[44] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 100, 112, 124, 136, 148, 160, 172, 184, 196, 212, 228, 244, 260, 280, 300, 320, 344, 368, 396, 424, 456, 492, 532, 572, 616, 664, 716, 772, 832, 896, 960, 1024 };
static const unsigned short swb_long_8[41] = { 0, 12, 24, 36, 48, 60, 72, 84, 96, 108, 120, 132, 144, 156, 172, 188, 204, 220, 236, 252, 268, 288, 308, 328, 348, 372, 396, 420, 448, 476, 508, 544, 580, 620, 664, 712, 764, 820, 880, 944, 1024 };
static const unsigned short swb_short_64[13] = { 0, 4, 8, 12, 16, 20, 24, 32, 40, 48, 64, 92, 128 };
static const unsigned short swb_short_48[15] = { 0, 4, 8, 12, 16, 20, 28, 36, 44, 56, 68, 80, 96, 112, 128 };
static const unsigned short swb_short_24[16] = { 0, 4, 8, 12, 16, 20, 24, 28, 36, 44, 52, 64, 76, 92, 108, 128 };
static const unsigned short swb_short_16[16] = { 0, 4, 8, 12, 16, 20, 24, 28, 32, 40, 48, 60, 72, 88, 108, 128 };
static const unsigned short swb_short_8[16] = { 0, 4, 8, 12, 16, 20, 24, 28, 36, 44, 52, 60, 72, 88, 108, 128 };
/* per sampling frequency index 0..11: 96000 88200 64000 48000 44100 32000 24000 22050 16000 12000 11025 8000 */
static const unsigned short *const swb_long[12] = { swb_long_96, swb_long_96, swb_long_64, swb_long_48, swb_long_48, swb_long_32, swb_long_24, swb_long_24, swb_long_16, swb_long_16, swb_long_16, swb_long_8 };
static const int nswb_long[12] = { 41, 41, 47, 49, 49, 51, 47, 47, 43, 43, 43, 40 };
static const unsigned short *const swb_short[12] = { swb_short_64, swb_short_64, swb_short_64, swb_short_48, swb_short_48, swb_short_48, swb_short_24, swb_short_24, swb_short_16, swb_short_16, swb_short_16, swb_short_8 };
static const int nswb_short[12] = { 12, 12, 12, 14, 14, 14, 15, 15, 15, 15, 15, 15 };
static const int tns_max_long[12] = { 31, 31, 34, 40, 42, 51, 46, 46, 42, 42, 42, 39 };
static const int tns_max_short[12] = { 9, 9, 10, 14, 14, 14, 14, 14, 14, 14, 14, 14 };
static const int sf_rates[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };

static float g_pow43[8192];
static float g_sine_long[1024], g_sine_short[128], g_kbd_long[1024], g_kbd_short[128];   /* rising halves */

static double bessel_i0(double x) { double s = 1, t = 1; int k; for (k = 1; k < 50; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; if (t < 1e-12 * s) break; } return s; }
static void kbd(float *w, int n, double alpha) {   /* n: half length */
    double *k = (double*)malloc((size_t)(n + 1) * sizeof(double)), sum = 0, acc = 0; int i;
    if (!k) return;
    for (i = 0; i <= n; i++) { double r = (double)(2 * i - n) / n; k[i] = bessel_i0(M_PI * alpha * sqrt(1.0 - r * r)); sum += k[i]; }
    for (i = 0; i < n; i++) { acc += k[i]; w[i] = (float)sqrt(acc / sum); }
    free(k);
}
static void init_tables(void) {
    int i;
    if (g_trees_built) return;
    for (i = 0; i < 12; i++) if (tree_build(&g_tree[i], hcb_len[i], hcb_code[i], hcb_n[i]) < 0) return;
    for (i = 0; i < 8192; i++) g_pow43[i] = (float)pow((double)i, 4.0 / 3.0);
    for (i = 0; i < 1024; i++) g_sine_long[i] = (float)sin(M_PI / 2048.0 * (i + 0.5));
    for (i = 0; i < 128; i++) g_sine_short[i] = (float)sin(M_PI / 256.0 * (i + 0.5));
    kbd(g_kbd_long, 1024, 4.0); kbd(g_kbd_short, 128, 6.0);
    g_trees_built = 1;
}

/* ---- IMDCT: n outputs from n/2 coefficients (2/n scaling as in the standard) ---- */
static void fft(float *re, float *im, int n) {   /* in place, radix 2, forward */
    int i, j, k, len;
    for (i = 1, j = 0; i < n; i++) { int bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; } }
    for (len = 2; len <= n; len <<= 1) {
        double ang = -2 * M_PI / len; float wr = (float)cos(ang), wi = (float)sin(ang);
        for (i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for (k = 0; k < len / 2; k++) {
                int a = i + k, b = i + k + len / 2;
                float tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr, ncr;
                re[b] = re[a] - tr; im[b] = im[a] - ti; re[a] += tr; im[a] += ti;
                ncr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = ncr;
            }
        }
    }
}
static void imdct(const float *X, float *y, int n) {
    int m = n / 2, h = m / 2, k, t;
    float re[512], im[512], u[1024], sc = 2.0f / n;
    for (k = 0; k < h; k++) {
        double a = -M_PI * (4 * k + 1) / (4.0 * m); float c = (float)cos(a), s = (float)sin(a);
        float xr = X[2 * k], xi = X[m - 1 - 2 * k];
        re[k] = xr * c - xi * s; im[k] = xr * s + xi * c;
    }
    fft(re, im, h);
    for (k = 0; k < h; k++) {
        double a = -M_PI * k / m; float c = (float)cos(a), s = (float)sin(a);
        float vr = re[k] * c - im[k] * s, vi = re[k] * s + im[k] * c;
        u[2 * k] = vr; u[m - 1 - 2 * k] = -vi;
    }
    for (t = 0; t < n; t++) {
        float v;
        if (t < h) v = u[h + t]; else if (t < 3 * h) v = -u[3 * h - 1 - t]; else v = -u[t - 3 * h];
        y[t] = v * sc;
    }
}

/* ---- decoder state ---- */
enum { ONLY_LONG = 0, LONG_START = 1, EIGHT_SHORT = 2, LONG_STOP = 3 };
enum { ZERO_HCB = 0, ESC_HCB = 11, NOISE_HCB = 13, INTENSITY_HCB2 = 14, INTENSITY_HCB = 15 };
typedef struct {
    int seq, shape, max_sfb, ngroups, glen[8], nwin, nswb;
    const unsigned short *swb;
    unsigned char cb[8][64]; int sf[8][64];
    int tns_present, tns_nfilt[8], tns_len[8][4], tns_order[8][4], tns_dir[8][4]; float tns_lpc[8][4][32];
    float coef[1024];
} ICS;
typedef struct { float overlap[1024]; int prev_shape; } ChanState;
struct AACDec {
    int sfi, rate, channels, configured;
    ChanState ch[2];
    ICS ics[2];
    unsigned rnd;
    int ms_present; unsigned char ms_used[8][64];
};

static void ics_info(AACDec *d, Bits *b, ICS *s) {
    int i;
    getbits(b, 1);   /* reserved */
    s->seq = (int)getbits(b, 2); s->shape = (int)getbits(b, 1);
    if (s->seq == EIGHT_SHORT) {
        int grp; s->max_sfb = (int)getbits(b, 4); grp = (int)getbits(b, 7);
        s->nwin = 8; s->ngroups = 1; s->glen[0] = 1;
        for (i = 6; i >= 0; i--) { if ((grp >> i) & 1) s->glen[s->ngroups - 1]++; else s->glen[s->ngroups++] = 1; }
        s->swb = swb_short[d->sfi]; s->nswb = nswb_short[d->sfi];
    } else {
        s->max_sfb = (int)getbits(b, 6);
        if (getbits(b, 1)) b->err = 1;   /* predictor data: AAC Main / LTP only */
        s->nwin = 1; s->ngroups = 1; s->glen[0] = 1;
        s->swb = swb_long[d->sfi]; s->nswb = nswb_long[d->sfi];
    }
    if (s->max_sfb > s->nswb) { s->max_sfb = s->nswb; b->err = 1; }
}
static int ics_decode(AACDec *d, Bits *b, ICS *s, int common_window) {
    int g, i, k, w, gg, global_gain, sfac, noise, isp, noise_first = 1;
    int q[1024];
    global_gain = (int)getbits(b, 8);
    if (!common_window) ics_info(d, b, s);
    if (b->err) return -1;
    /* section data */
    for (g = 0; g < s->ngroups; g++) {
        int bits = s->seq == EIGHT_SHORT ? 3 : 5, esc = (1 << bits) - 1;
        k = 0;
        while (k < s->max_sfb && !b->err) {
            int cb = (int)getbits(b, 4), len = 0, inc;
            do { inc = (int)getbits(b, bits); len += inc; } while (inc == esc && !b->err);
            if (cb == 12 || k + len > s->max_sfb) { b->err = 1; break; }
            for (i = 0; i < len; i++) s->cb[g][k + i] = (unsigned char)cb;
            k += len;
        }
        for (; k < 64; k++) s->cb[g][k] = ZERO_HCB;
    }
    if (b->err) return -1;
    /* scalefactors */
    sfac = global_gain; noise = global_gain - 90; isp = 0;
    for (g = 0; g < s->ngroups; g++) for (k = 0; k < s->max_sfb; k++) {
        int cb = s->cb[g][k];
        if (cb == ZERO_HCB) s->sf[g][k] = 0;
        else if (cb == INTENSITY_HCB || cb == INTENSITY_HCB2) { isp += huff(b, &g_tree[0]) - 60; s->sf[g][k] = isp; }
        else if (cb == NOISE_HCB) { if (noise_first) { noise_first = 0; noise += (int)getbits(b, 9) - 256; } else noise += huff(b, &g_tree[0]) - 60; s->sf[g][k] = noise; }
        else { sfac += huff(b, &g_tree[0]) - 60; s->sf[g][k] = sfac; }
    }
    /* pulse data */
    {   int pulse = (int)getbits(b, 1), np = 0, pstart = 0, poff[5], pamp[5];
        if (pulse) {
            if (s->seq == EIGHT_SHORT) return -1;
            np = (int)getbits(b, 2) + 1; pstart = (int)getbits(b, 6);
            for (i = 0; i < np; i++) { poff[i] = (int)getbits(b, 5); pamp[i] = (int)getbits(b, 4); }
        }
        /* TNS data */
        s->tns_present = (int)getbits(b, 1);
        if (s->tns_present) {
            int lng = s->seq != EIGHT_SHORT;
            for (w = 0; w < s->nwin; w++) {
                int f, res = 0;
                s->tns_nfilt[w] = (int)getbits(b, lng ? 2 : 1);
                if (s->tns_nfilt[w]) res = (int)getbits(b, 1);
                for (f = 0; f < s->tns_nfilt[w]; f++) {
                    int order, m;
                    s->tns_len[w][f] = (int)getbits(b, lng ? 6 : 4);
                    order = (int)getbits(b, lng ? 5 : 3);
                    s->tns_order[w][f] = order;
                    if (order) {
                        int compress, cbits; double iqfac, iqfac_m, par[32], a[32], tmp[32];
                        s->tns_dir[w][f] = (int)getbits(b, 1);
                        compress = (int)getbits(b, 1);
                        cbits = res + 3 - compress;
                        iqfac = ((1 << (res + 3 - 1)) - 0.5) / (M_PI / 2); iqfac_m = ((1 << (res + 3 - 1)) + 0.5) / (M_PI / 2);
                        for (i = 0; i < order; i++) {
                            int v = (int)getbits(b, cbits);
                            if (v & (1 << (cbits - 1))) v -= 1 << cbits;
                            par[i] = sin(v / (v >= 0 ? iqfac : iqfac_m));
                        }
                        a[0] = 1;   /* parcor to LPC (step-up recursion) */
                        for (m = 1; m <= order; m++) {
                            for (i = 1; i < m; i++) tmp[i] = a[i] + par[m - 1] * a[m - i];
                            for (i = 1; i < m; i++) a[i] = tmp[i];
                            a[m] = par[m - 1];
                        }
                        for (i = 0; i <= order; i++) s->tns_lpc[w][f][i] = (float)a[i];
                    }
                }
            }
        }
        if (getbits(b, 1)) return -1;   /* gain control: AAC SSR only */
        if (b->err) return -1;
        /* spectral data: quantized values, interleaved by window within a group */
        memset(q, 0, sizeof(q));
        for (g = 0, w = 0; g < s->ngroups; w += s->glen[g], g++) for (k = 0; k < s->max_sfb; k++) {
            int cb = s->cb[g][k], start = s->swb[k], end = s->swb[k + 1];
            if (cb == ZERO_HCB || cb == NOISE_HCB || cb >= INTENSITY_HCB2) continue;
            for (gg = 0; gg < s->glen[g]; gg++) {
                int *qq = q + (w + gg) * 128;
                for (i = start; i < end; ) {
                    int idx = huff(b, &g_tree[cb]), v[4], n = cb < 5 ? 4 : 2, j;
                    if (b->err) return -1;
                    if (cb < 5) {
                        v[0] = idx / 27; v[1] = (idx / 9) % 3; v[2] = (idx / 3) % 3; v[3] = idx % 3;
                        if (cb <= 2) for (j = 0; j < 4; j++) v[j] -= 1;
                    } else if (cb < 7) { v[0] = idx / 9 - 4; v[1] = idx % 9 - 4; }
                    else { int mod = cb < 9 ? 8 : cb < 11 ? 13 : 17; v[0] = idx / mod; v[1] = idx % mod; }
                    if (cb == 3 || cb == 4 || cb >= 7) for (j = 0; j < n; j++) if (v[j] && getbits(b, 1)) v[j] = -v[j];
                    if (cb == ESC_HCB) for (j = 0; j < 2; j++) if (v[j] == 16 || v[j] == -16) {
                        int nn = 4, sg = v[j] < 0 ? -1 : 1;
                        while (getbits(b, 1) && nn < 13 && !b->err) nn++;
                        v[j] = sg * ((1 << nn) + (int)getbits(b, nn));
                    }
                    for (j = 0; j < n && i + j < end; j++) qq[i + j] = v[j];
                    i += n;
                }
            }
        }
        if (b->err) return -1;
        if (pulse) {
            k = pstart < s->nswb ? s->swb[pstart] : 1024;
            for (i = 0; i < np; i++) { k += poff[i]; if (k < 1024) { if (q[k] > 0) q[k] += pamp[i]; else q[k] -= pamp[i]; } }
        }
    }
    /* dequantization and scaling; noise substitution */
    memset(s->coef, 0, sizeof(s->coef));
    for (g = 0, w = 0; g < s->ngroups; w += s->glen[g], g++) for (k = 0; k < s->max_sfb; k++) {
        int cb = s->cb[g][k], start = s->swb[k], end = s->swb[k + 1];
        if (cb == ZERO_HCB || cb >= INTENSITY_HCB2) continue;
        for (gg = 0; gg < s->glen[g]; gg++) {
            int base = (w + gg) * 128;
            if (cb == NOISE_HCB) {
                double e = 0, sc;
                for (i = start; i < end; i++) { float r; d->rnd = d->rnd * 1664525u + 1013904223u; r = (float)(int)d->rnd; s->coef[base + i] = r; e += (double)r * r; }
                sc = pow(2.0, 0.25 * s->sf[g][k]) / sqrt(e > 0 ? e : 1);
                for (i = start; i < end; i++) s->coef[base + i] = (float)(s->coef[base + i] * sc);
            } else {
                float sc = (float)pow(2.0, 0.25 * (s->sf[g][k] - 100));
                for (i = start; i < end; i++) { int v = q[base + i], a = v < 0 ? -v : v; float x = a < 8192 ? g_pow43[a] : (float)pow((double)a, 4.0 / 3.0); s->coef[base + i] = (v < 0 ? -x : x) * sc; }
            }
        }
    }
    return 0;
}
static void apply_tns(AACDec *d, ICS *s) {
    int w, f, lng = s->seq != EIGHT_SHORT, maxb = lng ? tns_max_long[d->sfi] : tns_max_short[d->sfi];
    if (!s->tns_present) return;
    if (maxb > s->max_sfb) maxb = s->max_sfb;
    for (w = 0; w < s->nwin; w++) {
        int bottom = s->nswb;
        float *c = s->coef + w * 128;
        for (f = 0; f < s->tns_nfilt[w]; f++) {
            int top = bottom, order = s->tns_order[w][f], start, end, size, inc, m, i, p;
            bottom = top - s->tns_len[w][f]; if (bottom < 0) bottom = 0;
            if (!order) continue;
            start = s->swb[bottom < maxb ? bottom : maxb]; end = s->swb[top < maxb ? top : maxb];
            size = end - start;
            if (size <= 0) continue;
            if (s->tns_dir[w][f]) { inc = -1; p = end - 1; } else { inc = 1; p = start; }
            for (m = 0; m < size; m++, p += inc)
                for (i = 1; i <= order && i <= m; i++) c[p] -= s->tns_lpc[w][f][i] * c[p - i * inc];
        }
    }
}
/* the windowed, overlapped time signal of one channel */
static void synth(ICS *s, ChanState *cs, float *out) {
    float buf[2048], y[2048];
    const float *lp = cs->prev_shape ? g_kbd_long : g_sine_long, *lc = s->shape ? g_kbd_long : g_sine_long;
    const float *sp = cs->prev_shape ? g_kbd_short : g_sine_short, *sc = s->shape ? g_kbd_short : g_sine_short;
    int i, w;
    if (s->seq == EIGHT_SHORT) {
        memset(buf, 0, sizeof(buf));
        for (w = 0; w < 8; w++) {
            const float *rise = w == 0 ? sp : sc;
            imdct(s->coef + w * 128, y, 256);
            for (i = 0; i < 128; i++) { buf[448 + w * 128 + i] += y[i] * rise[i]; buf[448 + w * 128 + 128 + i] += y[128 + i] * sc[127 - i]; }
        }
    } else {
        imdct(s->coef, y, 2048);
        for (i = 0; i < 2048; i++) {
            float wv;
            if (i < 1024) {
                if (s->seq == LONG_STOP) wv = i < 448 ? 0.0f : i < 576 ? sp[i - 448] : 1.0f;
                else wv = lp[i];
            } else {
                if (s->seq == LONG_START) wv = i < 1472 ? 1.0f : i < 1600 ? sc[127 - (i - 1472)] : 0.0f;
                else wv = lc[2047 - i];
            }
            buf[i] = y[i] * wv;
        }
    }
    for (i = 0; i < 1024; i++) { out[i] = cs->overlap[i] + buf[i]; cs->overlap[i] = buf[1024 + i]; }
    cs->prev_shape = s->shape;
}

AACDec *aac_new(void) { AACDec *d = (AACDec*)calloc(1, sizeof(AACDec)); init_tables(); if (d) d->rnd = 0x1f2e3d4cu; return d; }
void aac_free(AACDec *d) { free(d); }
/* AudioSpecificConfig; 0 when usable */
int aac_config(AACDec *d, const u8 *p, size_t n) {
    Bits b; int aot, sfi, chc;
    if (!d || !g_trees_built) return -1;
    b.p = p; b.nbits = n * 8; b.pos = 0; b.err = 0;
    aot = (int)getbits(&b, 5); if (aot == 31) aot = 32 + (int)getbits(&b, 6);
    sfi = (int)getbits(&b, 4); if (sfi == 15) { int r = (int)getbits(&b, 24), k; sfi = 11; for (k = 0; k < 12; k++) if (r >= (sf_rates[k] + (k < 11 ? sf_rates[k + 1] : 0)) / 2) { sfi = k; break; } }
    chc = (int)getbits(&b, 4);
    if (aot == 5 || aot == 29) {   /* HE-AAC: decode the AAC-LC core */
        int esfi = (int)getbits(&b, 4); if (esfi == 15) getbits(&b, 24);
        aot = (int)getbits(&b, 5);
    }
    if (b.err || sfi > 11 || chc < 1 || chc > 2 || (aot != 2 && aot != 1 && aot != 4)) return -1;
    d->sfi = sfi; d->rate = sf_rates[sfi]; d->channels = chc; d->configured = 1;
    memset(d->ch, 0, sizeof(d->ch));
    return 0;
}
/* one raw_data_block -> interleaved 16-bit PCM (1024 frames); returns frames */
int aac_decode(AACDec *d, const u8 *p, size_t n, s16 **out, int *rate, int *channels) {
    Bits b; float pcm[2][1024]; int nch = 0, i, c, done = 0, guard = 0; s16 *o;
    *out = NULL;
    if (!d || !d->configured) return 0;
    b.p = p; b.nbits = n * 8; b.pos = 0; b.err = 0;
    while (!done && !b.err && guard++ < 64) {
        int id = (int)getbits(&b, 3);
        switch (id) {
        case 0: case 3:   /* SCE, LFE */
            getbits(&b, 4);
            if (nch >= 2) return 0;
            if (ics_decode(d, &b, &d->ics[nch], 0) < 0) return 0;
            apply_tns(d, &d->ics[nch]);
            synth(&d->ics[nch], &d->ch[nch], pcm[nch]); nch++;
            break;
        case 1: {   /* CPE */
            ICS *l = &d->ics[0], *r = &d->ics[1]; int cw, g, k, w, gg, j;
            getbits(&b, 4);
            if (nch != 0) return 0;
            cw = (int)getbits(&b, 1);
            d->ms_present = 0;
            if (cw) {
                ics_info(d, &b, l);
                d->ms_present = (int)getbits(&b, 2);
                for (g = 0; g < l->ngroups; g++) for (k = 0; k < l->max_sfb; k++) d->ms_used[g][k] = (unsigned char)(d->ms_present == 1 ? getbits(&b, 1) : d->ms_present == 2);
                *r = *l;
            }
            if (ics_decode(d, &b, l, cw) < 0) return 0;
            if (cw) { r->seq = l->seq; r->shape = l->shape; r->max_sfb = l->max_sfb; r->ngroups = l->ngroups; memcpy(r->glen, l->glen, sizeof(l->glen)); r->nwin = l->nwin; r->swb = l->swb; r->nswb = l->nswb; }
            if (ics_decode(d, &b, r, cw) < 0) return 0;
            if (cw) for (g = 0, w = 0; g < l->ngroups; w += l->glen[g], g++) for (k = 0; k < l->max_sfb; k++) {
                int start = l->swb[k], end = l->swb[k + 1], rcb = r->cb[g][k];
                for (gg = 0; gg < l->glen[g]; gg++) {
                    float *lc = l->coef + (w + gg) * 128, *rc = r->coef + (w + gg) * 128;
                    if (rcb == INTENSITY_HCB || rcb == INTENSITY_HCB2) {   /* intensity stereo */
                        float sc = (float)pow(0.5, 0.25 * r->sf[g][k]) * (rcb == INTENSITY_HCB ? 1.0f : -1.0f);
                        if (d->ms_present == 1 && d->ms_used[g][k]) sc = -sc;
                        for (j = start; j < end; j++) rc[j] = lc[j] * sc;
                    } else if (d->ms_present && d->ms_used[g][k] && l->cb[g][k] < NOISE_HCB && rcb < NOISE_HCB) {   /* mid/side */
                        for (j = start; j < end; j++) { float m = lc[j], s2 = rc[j]; lc[j] = m + s2; rc[j] = m - s2; }
                    }
                }
            }
            apply_tns(d, l); apply_tns(d, r);
            synth(l, &d->ch[0], pcm[0]); synth(r, &d->ch[1], pcm[1]); nch = 2;
            break; }
        case 4: {   /* DSE */
            int align, cnt;
            getbits(&b, 4); align = (int)getbits(&b, 1); cnt = (int)getbits(&b, 8); if (cnt == 255) cnt += (int)getbits(&b, 8);
            if (align) b.pos = (b.pos + 7) & ~(size_t)7;
            b.pos += (size_t)cnt * 8;
            break; }
        case 6: {   /* FIL */
            int cnt = (int)getbits(&b, 4); if (cnt == 15) cnt += (int)getbits(&b, 8) - 1;
            b.pos += (size_t)cnt * 8;
            break; }
        case 7: done = 1; break;
        default: return 0;   /* CCE, PCE: not supported */
        }
    }
    if (nch == 0) return 0;
    o = (s16*)malloc(1024 * (size_t)nch * sizeof(s16));
    if (!o) return 0;
    for (i = 0; i < 1024; i++) for (c = 0; c < nch; c++) { float v = pcm[c][i]; long x = (long)(v >= 0 ? v + 0.5f : v - 0.5f); if (x > 32767) x = 32767; if (x < -32768) x = -32768; o[i * nch + c] = (s16)x; }
    *out = o; *rate = d->rate; *channels = nch;
    return 1024;
}
