/* lzma.c - LZMA1 decoder for ZWS (LZMA-compressed SWF, Flash 11+).
 * Range coder + literal/length/distance models, written from the format
 * description.  Decodes until the output buffer is full or an end marker. */
#include "inflate.h"
#include <stdlib.h>
#include <string.h>

#define kNumBitModelTotalBits 11
#define kBitModelTotal (1 << kNumBitModelTotalBits)
#define kNumMoveBits 5
#define kTopValue ((u32)1 << 24)

typedef struct {
    const u8 *in; size_t inlen, pos;
    u32 range, code;
    int corrupt;
} RC;

static void rc_init(RC *rc, const u8 *in, size_t n) {
    int i;
    rc->in = in; rc->inlen = n; rc->pos = 0; rc->range = 0xFFFFFFFFu; rc->code = 0; rc->corrupt = 0;
    for (i = 0; i < 5; i++) rc->code = (rc->code << 8) | (rc->pos < n ? in[rc->pos++] : 0);
}
static void rc_norm(RC *rc) {
    if (rc->range < kTopValue) { rc->range <<= 8; rc->code = (rc->code << 8) | (rc->pos < rc->inlen ? rc->in[rc->pos++] : 0); }
}
static int rc_bit(RC *rc, u16 *p) {
    u32 bound = (rc->range >> kNumBitModelTotalBits) * *p;
    int b;
    if (rc->code < bound) { rc->range = bound; *p += (kBitModelTotal - *p) >> kNumMoveBits; b = 0; }
    else { rc->range -= bound; rc->code -= bound; *p -= *p >> kNumMoveBits; b = 1; }
    rc_norm(rc);
    return b;
}
static u32 rc_direct(RC *rc, int n) {
    u32 r = 0;
    while (n--) {
        rc->range >>= 1;
        rc->code -= rc->range;
        { u32 t = 0 - (rc->code >> 31); rc->code += rc->range & t; r = (r << 1) + (t + 1); }
        rc_norm(rc);
    }
    return r;
}
static u32 rc_tree(RC *rc, u16 *probs, int nbits) {
    u32 m = 1; int i;
    for (i = 0; i < nbits; i++) m = (m << 1) + (u32)rc_bit(rc, &probs[m]);
    return m - ((u32)1 << nbits);
}
static u32 rc_tree_rev(RC *rc, u16 *probs, int nbits) {
    u32 m = 1, sym = 0; int i;
    for (i = 0; i < nbits; i++) { u32 b = (u32)rc_bit(rc, &probs[m]); m = (m << 1) + b; sym |= b << i; }
    return sym;
}

#define kNumStates 12
#define kNumPosBitsMax 4
#define kNumLenToPosStates 4
#define kNumAlignBits 4
#define kStartPosModelIndex 4
#define kEndPosModelIndex 14
#define kNumFullDistances (1 << (kEndPosModelIndex >> 1))
#define kMatchMinLen 2

typedef struct {
    u16 choice, choice2;
    u16 low[kNumPosBitsMax << 3];
    u16 mid[kNumPosBitsMax << 3];
    u16 high[256];
} LenCoder;

static void len_init(LenCoder *l) { u16 *p = (u16*)l; size_t i; for (i = 0; i < sizeof(*l) / 2; i++) p[i] = kBitModelTotal >> 1; }
static u32 len_decode(RC *rc, LenCoder *l, int pos_state) {
    if (!rc_bit(rc, &l->choice)) return rc_tree(rc, l->low + (pos_state << 3), 3);
    if (!rc_bit(rc, &l->choice2)) return 8 + rc_tree(rc, l->mid + (pos_state << 3), 3);
    return 16 + rc_tree(rc, l->high, 8);
}

long swf_lzma_decode(const u8 *props, const u8 *src, size_t srclen, u8 *out, size_t outcap) {
    int lc, lp, pb, i;
    u16 *literal;
    u16 is_match[kNumStates << kNumPosBitsMax], is_rep[kNumStates], is_rep_g0[kNumStates], is_rep_g1[kNumStates], is_rep_g2[kNumStates];
    u16 is_rep0_long[kNumStates << kNumPosBitsMax];
    u16 pos_slot[kNumLenToPosStates][64], pos_decoders[1 + kNumFullDistances - kEndPosModelIndex], align[1 << kNumAlignBits];
    LenCoder len, rep_len;
    RC rc;
    u32 rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0;
    int state = 0;
    size_t outpos = 0;
    u32 lit_states;

    if (props[0] >= 9 * 5 * 5) return FLASHNT_EFMT;
    lc = props[0] % 9; lp = (props[0] / 9) % 5; pb = props[0] / 45;
    lit_states = (u32)0x300 << (lc + lp);
    literal = (u16*)malloc(lit_states * sizeof(u16));
    if (!literal) return FLASHNT_ENOMEM;
    for (i = 0; (u32)i < lit_states; i++) literal[i] = kBitModelTotal >> 1;
#define INIT(a) for (i = 0; i < (int)(sizeof(a) / sizeof((a)[0])); i++) ((u16*)(a))[i] = kBitModelTotal >> 1
    INIT(is_match); INIT(is_rep); INIT(is_rep_g0); INIT(is_rep_g1); INIT(is_rep_g2); INIT(is_rep0_long);
    INIT(pos_decoders); INIT(align);
    { int a, b; for (a = 0; a < kNumLenToPosStates; a++) for (b = 0; b < 64; b++) pos_slot[a][b] = kBitModelTotal >> 1; }
    len_init(&len); len_init(&rep_len);
    rc_init(&rc, src, srclen);

    while (outpos < outcap) {
        int pos_state = (int)(outpos & ((1u << pb) - 1));
        if (rc.pos > rc.inlen + 4) break;
        if (!rc_bit(&rc, &is_match[(state << kNumPosBitsMax) + pos_state])) {
            /* literal */
            u8 prev = outpos ? out[outpos - 1] : 0;
            u16 *probs = literal + (u32)0x300 * ((((u32)outpos & ((1u << lp) - 1)) << lc) + (prev >> (8 - lc)));
            u32 sym = 1;
            if (state >= 7) {
                u32 match_byte = (outpos > rep0) ? out[outpos - rep0 - 1] : 0;
                do {
                    u32 match_bit = (match_byte >> 7) & 1; match_byte <<= 1;
                    { int bit = rc_bit(&rc, &probs[((1 + match_bit) << 8) + sym]); sym = (sym << 1) | (u32)bit;
                      if ((u32)bit != match_bit) break; }
                } while (sym < 0x100);
            }
            while (sym < 0x100) sym = (sym << 1) | (u32)rc_bit(&rc, &probs[sym]);
            out[outpos++] = (u8)sym;
            state = state < 4 ? 0 : (state < 10 ? state - 3 : state - 6);
            continue;
        }
        {
            u32 length;
            if (rc_bit(&rc, &is_rep[state])) {
                if (outpos == 0) break;
                if (!rc_bit(&rc, &is_rep_g0[state])) {
                    if (!rc_bit(&rc, &is_rep0_long[(state << kNumPosBitsMax) + pos_state])) {
                        /* short rep */
                        state = state < 7 ? 9 : 11;
                        out[outpos] = out[outpos - rep0 - 1]; outpos++;
                        continue;
                    }
                } else {
                    u32 dist;
                    if (!rc_bit(&rc, &is_rep_g1[state])) dist = rep1;
                    else { if (!rc_bit(&rc, &is_rep_g2[state])) dist = rep2; else { dist = rep3; rep3 = rep2; } rep2 = rep1; }
                    rep1 = rep0; rep0 = dist;
                }
                length = len_decode(&rc, &rep_len, pos_state);
                state = state < 7 ? 8 : 11;
            } else {
                u32 slot, dist;
                rep3 = rep2; rep2 = rep1; rep1 = rep0;
                length = len_decode(&rc, &len, pos_state);
                state = state < 7 ? 7 : 10;
                slot = rc_tree(&rc, pos_slot[length < kNumLenToPosStates - 1 ? length : kNumLenToPosStates - 1], 6);
                if (slot < kStartPosModelIndex) dist = slot;
                else {
                    int nbits = (int)(slot >> 1) - 1;
                    dist = (2 | (slot & 1)) << nbits;
                    if (slot < kEndPosModelIndex) dist += rc_tree_rev(&rc, pos_decoders + dist - slot, nbits);
                    else { dist += rc_direct(&rc, nbits - kNumAlignBits) << kNumAlignBits; dist += rc_tree_rev(&rc, align, kNumAlignBits); }
                }
                if (dist == 0xFFFFFFFFu) break;         /* end marker */
                rep0 = dist;
            }
            length += kMatchMinLen;
            if (rep0 >= outpos) { rc.corrupt = 1; break; }
            while (length-- && outpos < outcap) { out[outpos] = out[outpos - rep0 - 1]; outpos++; }
        }
    }
    free(literal);
    if (rc.corrupt) return FLASHNT_EFMT;
    return (long)outpos;
}
