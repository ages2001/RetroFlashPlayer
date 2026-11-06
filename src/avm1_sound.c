/* avm1_sound.c - the AVM1 Sound class, sound transforms and ID3 tags.
 *
 * Follows Ruffle's core/src/avm1/globals/sound.rs and the audio manager
 * of core/src/backend/audio.rs: a Sound either controls the global sound
 * (no target), the sounds of a movie clip (its target path), or a sound
 * it loaded itself (loadSound).  Transforms combine the instance's, the
 * clip chain's and the global one with Flash's integer arithmetic. */
#include "avm1_int.h"
#include "audio.h"
#include "fetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define DE  PA_DONT_ENUM
#define DD  PA_DONT_DELETE
#define RO  PA_READ_ONLY
#define V6  PA_VERSION_6

extern AObj *lib_class(VM *vm, Env *env, NativeFn ctor, NativeFn call, AObj *super_proto, int proto_first, AObj **proto_out);
extern void avm1_queue_method(VM *vm, MovieClip *c, AObj *o, AStr *name, int argc, Value *argv);
extern const char *avm1_movie_url(Player *p, SWFMovie *m);

typedef struct { AObj *obj; AStr *url; u32 load_id; } PendLoad;
static PendLoad *g_pend; static int g_npend;

enum { ST_VOL = 0, ST_LL, ST_LR, ST_RL, ST_RR };   /* volume, left->left, left->right, right->left, right->right */

static void st_default(int *st) { st[ST_VOL] = 100; st[ST_LL] = 100; st[ST_LR] = 0; st[ST_RL] = 0; st[ST_RR] = 100; }

/* SoundTransform::concat */
static void st_concat(int *a, const int *b) {
    const s64 M = 100; const s32 MASK = (1 << 30) - 1; s64 v, ll0 = a[ST_LL], lr0 = a[ST_LR], rl0 = a[ST_RL], rr0 = a[ST_RR], ll1 = b[ST_LL], lr1 = b[ST_LR], rl1 = b[ST_RL], rr1 = b[ST_RR];
    v = (s64)a[ST_VOL] * b[ST_VOL] / M; if (v < 0) v = -v;
    a[ST_VOL] = (s32)v & MASK;
    a[ST_LL] = (s32)((ll0 * ll1 + rl0 * lr1) / M) & MASK;
    a[ST_LR] = (s32)((lr0 * ll1 + rr0 * lr1) / M) & MASK;
    a[ST_RL] = (s32)((ll0 * rl1 + rl0 * rr1) / M) & MASK;
    a[ST_RR] = (s32)((lr0 * rl1 + rr0 * rr1) / M) & MASK;
}

static int st_pan(const int *st) {
    if (st[ST_LL] != 100) return 100 - (st[ST_LL] < 0 ? -st[ST_LL] : st[ST_LL]);
    return (st[ST_RR] < 0 ? -st[ST_RR] : st[ST_RR]) - 100;
}

static void st_with_pan(int *st, int pan) {
    if (pan >= 0) { st[ST_LL] = 100 - pan; st[ST_RR] = 100; }
    else { st[ST_LL] = 100; st[ST_RR] = 100 + pan; }
    st[ST_LR] = 0; st[ST_RL] = 0;
}

/* the global sound transform lives in the VM; clips keep theirs in A1Clip */
static int g_st[5] = { 100, 100, 0, 0, 100 };

static int *clip_st(MovieClip *c) {
    A1Clip *s = a1_of(c);
    if (!s) return NULL;
    if (!s->has_st) { st_default(s->st); s->has_st = 1; }
    return s->st;
}

/* ---------------------------------------------------------------- */
/* external sounds: decoded once, kept for the player's lifetime    */

typedef struct ExtList { SoundDef **d; int n, cap; } ExtList;
static ExtList g_ext;

static SoundDef *keep_ext(SoundDef *sd) {
    if (!sd) return NULL;
    if (g_ext.n == g_ext.cap) {
        int nc = g_ext.cap ? g_ext.cap * 2 : 8; SoundDef **nd = (SoundDef**)realloc(g_ext.d, (size_t)nc * sizeof(SoundDef*));
        if (!nd) { sound_def_free(sd); return NULL; }
        g_ext.d = nd; g_ext.cap = nc;
    }
    g_ext.d[g_ext.n++] = sd;
    return sd;
}

void avm1_sound_free_all(void) { int i; free(g_pend); g_pend = NULL; g_npend = 0; for (i = 0; i < g_ext.n; i++) sound_def_free(g_ext.d[i]); free(g_ext.d); memset(&g_ext, 0, sizeof(g_ext)); st_default(g_st); }

/* ---------------------------------------------------------------- */
/* Sound objects                                                    */

typedef struct SoundData {
    AStr *target;                 /* NULL: the global sound */
    SoundDef *sd;                 /* attached or loaded sound */
    int inst;                     /* last instance started */
    u32 position; int has_dur; u32 duration;
    int external, ext_streaming, ext_autoplay, ext_loading; u32 load_id;
    int ext_st[5];
    u32 bytes;                    /* size of the loaded sound file */
} SoundData;

static void sd_free(AObj *o) { free(o->nat.p); o->nat.p = NULL; }
static void sd_mark(VM *vm, AObj *o) { SoundData *s = (SoundData*)o->nat.p; (void)vm; if (s && s->target) gc_mark_str(s->target); }

static AObj *real(AObj *o) { return o && o->nk == NK_SUPER ? o->nat.sup.self : o; }
static SoundData *snd_of(AObj *o) { o = real(o); return o && o->nk == NK_SOUND ? (SoundData*)o->nat.p : NULL; }

static MovieClip *snd_owner(Act *a, SoundData *s) {
    MovieClip *c;
    if (!s->target || s->external) return NULL;
    c = act_resolve_target_display(a, act_target_or_root(a), v_str(s->target), 0);
    if (a->vm->err) { a->vm->err = 0; return NULL; }
    return c;
}
static int use_global(SoundData *s) { return s->target == NULL; }
static int has_valid_owner(Act *a, SoundData *s) { return use_global(s) || s->external || snd_owner(a, s) != NULL; }

/* the transform the getters report: NULL when the owner is gone */
static int *snd_transform(Act *a, SoundData *s) {
    MovieClip *c;
    if (s->external) return s->ext_st;
    if (use_global(s)) return g_st;
    c = snd_owner(a, s);
    return c ? clip_st(c) : NULL;
}

static Value snd_ctor(Act *a, AObj *self, int argc, Value *argv) {
    SoundData *s; AObj *t; AStr *target = NULL;
    if (argc > 0 && argv[0].t != V_UNDEF && argv[0].t != V_NULL) { target = v_str_of(a, argv[0]); if (a->vm->err) return v_undef(); }
    t = o_native_target(self);
    if (!t) return v_obj(self);
    s = (SoundData*)calloc(1, sizeof(SoundData));
    if (!s) return v_undef();
    s->target = target; st_default(s->ext_st);
    t->nk = NK_SOUND; t->nat.p = s; t->nfree = sd_free; t->nmark = sd_mark;
    return v_obj(self);
}

static Value snd_duration(Act *a, AObj *self, int argc, Value *argv);
static Value snd_position(Act *a, AObj *self, int argc, Value *argv);

/* Sound::set_sound: `position` and `duration` become getters of the object */
static void set_sound(Act *a, AObj *self, SoundData *s, SoundDef *sd) {
    VM *vm = a->vm; AStr *pos = str_intern(vm, "position"), *dur = str_intern(vm, "duration"); int hp, hd;
    s->sd = sd;
    hp = o_has_property(a, self, pos); vm->err = 0;
    hd = o_has_property(a, self, dur); vm->err = 0;
    if (!hp || !hd) {
        Env *e = env_get(vm, a->swf_version);
        o_add_property(vm, real(self), pos, fn_native(vm, e, snd_position, NULL), NULL, DE | DD);
        o_add_property(vm, real(self), dur, fn_native(vm, e, snd_duration, NULL), NULL, DE | DD);
    }
}

static int export_id(VM *vm, SWFMovie *m, AStr *name) {
    int i;
    if (!m) return -1;
    for (i = 0; i < m->nexports; i++) {
        AStr *e = str_from_swf(vm, m->exports[i].name, strlen(m->exports[i].name), m->version);
        if (str_eq(e, name)) return m->exports[i].id;
    }
    return -1;
}

/* the movie whose library a sound name is looked up in (NULL: none) */
static SWFMovie *lib_movie(Act *a, SoundData *s) {
    MovieClip *c;
    if (use_global(s) && !s->external) { c = a->base_clip ? clip_avm1_root(a->base_clip) : a->vm->player->root; return c && c->movie ? c->movie : a->vm->player->movie; }
    c = snd_owner(a, s);
    return c ? (c->movie ? c->movie : a->vm->player->movie) : NULL;
}

static SoundDef *find_lib_sound(Act *a, SWFMovie *m, AStr *name) {
    int id = export_id(a->vm, m, name);
    return id >= 0 ? sound_get(m, id) : NULL;
}

static Value snd_attach(Act *a, AObj *self, int argc, Value *argv) {
    SoundData *s = snd_of(self); AStr *name; SWFMovie *m; SoundDef *sd;
    if (!s) return v_undef();
    name = v_str_of(a, arg(argc, argv, 0)); if (a->vm->err) return v_undef();
    m = lib_movie(a, s);
    if (!m) return v_undef();
    sd = find_lib_sound(a, m, name);
    if (!sd) return v_undef();
    set_sound(a, self, s, sd);
    s->has_dur = 1; s->duration = (u32)floor(sound_def_duration_ms(sd) + 0.5);
    s->position = 0;
    return v_undef();
}

static Value snd_duration(Act *a, AObj *self, int argc, Value *argv) {
    SoundData *s = snd_of(self); (void)argc; (void)argv;
    if (s && has_valid_owner(a, s)) return s->has_dur ? v_num(s->duration) : v_undef();
    return v_undef();
}
static Value snd_position(Act *a, AObj *self, int argc, Value *argv) {
    SoundData *s = snd_of(self); (void)argc; (void)argv;
    if (s && s->sd && has_valid_owner(a, s)) return v_num(s->position);
    return v_undef();
}
static Value snd_noop(Act *a, AObj *self, int argc, Value *argv) { (void)a; (void)self; (void)argc; (void)argv; return v_undef(); }
/* getBytesLoaded / getBytesTotal: undefined until loadSound */
static Value snd_bytes(Act *a, AObj *self, int argc, Value *argv) { SoundData *s = snd_of(self); (void)a; (void)argc; (void)argv; return s && s->external ? v_num((double)s->bytes) : v_undef(); }
/* checkPolicyFile reads as a boolean on Sound objects (false until set) */
int avm1_sound_virtual(Act *a, AObj *o, AStr *name, Value *out) {
    SoundData *s = (SoundData*)o->nat.p;
    if (!s || !str_eq_ci(name, str_intern(a->vm, "checkPolicyFile"))) return 0;
    if (a->swf_version >= 7 && !str_eq_case(name, str_intern(a->vm, "checkPolicyFile"), 1)) return 0;
    *out = v_bool(0);
    return 1;
}

static Value snd_get_pan(Act *a, AObj *self, int argc, Value *argv) { SoundData *s = snd_of(self); int *st = s ? snd_transform(a, s) : NULL; (void)argc; (void)argv; return st ? v_num(st_pan(st)) : v_undef(); }
static Value snd_get_volume(Act *a, AObj *self, int argc, Value *argv) { SoundData *s = snd_of(self); int *st = s ? snd_transform(a, s) : NULL; (void)argc; (void)argv; return st ? v_num(st[ST_VOL]) : v_undef(); }

static Value snd_get_transform(Act *a, AObj *self, int argc, Value *argv) {
    VM *vm = a->vm; SoundData *s = snd_of(self); int *st = s ? snd_transform(a, s) : NULL, c[5]; AObj *o; (void)argc; (void)argv;
    if (!st) return v_undef();
    memcpy(c, st, sizeof(c));
    o = obj_new(vm, PROTOS(a)->object);
    o_set(a, o, str_intern(vm, "ll"), v_num(c[ST_LL])); if (vm->err) return v_undef();
    o_set(a, o, str_intern(vm, "lr"), v_num(c[ST_RL])); if (vm->err) return v_undef();
    o_set(a, o, str_intern(vm, "rr"), v_num(c[ST_RR])); if (vm->err) return v_undef();
    o_set(a, o, str_intern(vm, "rl"), v_num(c[ST_LR])); if (vm->err) return v_undef();
    return v_obj(o);
}

/* f64 -> i32 clamping (Clamp::clamp_to_i32) */
static s32 clamp_i32(double d) { if (d >= -2147483648.0 && d <= 2147483647.0) return (s32)d; return (s32)0x80000000; }

/* the transform a setter writes: external, owner, or global */
static int *setter_target(Act *a, SoundData *s) {
    MovieClip *c;
    if (s->external) return s->ext_st;
    c = snd_owner(a, s);
    if (c) return clip_st(c);
    if (use_global(s)) return g_st;
    return NULL;
}

static Value snd_set_pan(Act *a, AObj *self, int argc, Value *argv) {
    SoundData *s = snd_of(self); double d; int *st;
    if (!s) return v_undef();
    d = v_num_of(a, argc > 0 ? argv[0] : v_num(0)); if (a->vm->err) return v_undef();
    st = setter_target(a, s);
    if (st) st_with_pan(st, clamp_i32(d));
    return v_undef();
}

static Value snd_set_volume(Act *a, AObj *self, int argc, Value *argv) {
    SoundData *s = snd_of(self); double d; int *st;
    if (!s) return v_undef();
    d = v_num_of(a, argc > 0 ? argv[0] : v_num(0)); if (a->vm->err) return v_undef();
    st = setter_target(a, s);
    if (st) st[ST_VOL] = clamp_i32(d);
    return v_undef();
}

static Value snd_set_transform(Act *a, AObj *self, int argc, Value *argv) {
    VM *vm = a->vm; SoundData *s = snd_of(self); AObj *o; MovieClip *owner; int t[5], *dst; int i;
    static const char *keys[4] = { "ll", "rl", "lr", "rr" }; static const int slot[4] = { ST_LL, ST_LR, ST_RL, ST_RR };
    if (!s) return v_undef();
    o = v_to_obj_or_bare(a, arg(argc, argv, 0)); if (vm->err) return v_undef();
    owner = snd_owner(a, s);
    if (!owner && !use_global(s) && !s->external) return v_undef();
    dst = s->external ? s->ext_st : owner ? clip_st(owner) : g_st;
    if (!dst) return v_undef();
    memcpy(t, dst, sizeof(t));
    for (i = 0; i < 4; i++) {
        AStr *k = str_intern(vm, keys[i]);
        if (o_has_own_property(a, o, k)) { Value v = o_get(a, o, k); if (vm->err) return v_undef(); t[slot[i]] = v_i32(a, v); if (vm->err) return v_undef(); }
    }
    dst = s->external ? s->ext_st : owner ? clip_st(owner) : g_st;
    if (dst) memcpy(dst, t, sizeof(t));
    return v_undef();
}

static Value snd_start(Act *a, AObj *self, int argc, Value *argv) {
    SoundData *s = snd_of(self); double off, loops; int streaming; MovieClip *owner; int inst;
    if (!s || !has_valid_owner(a, s)) return v_undef();
    streaming = s->external && s->ext_streaming;
    off = v_num_of(a, argc > 0 ? argv[0] : v_num(0)); if (a->vm->err) return v_undef();
    loops = (argc > 1 && !streaming) ? v_num_of(a, argv[1]) : 1; if (a->vm->err) return v_undef();
    s = snd_of(self);
    if (!s || !s->sd) return v_undef();
    if (streaming && s->inst) audio_stop_instance(a->vm->player, s->inst);
    owner = snd_owner(a, s);
    {
        int l = loops != loops || loops <= 0 ? 0 : loops >= 65535 ? 65535 : (int)loops;
        if (l < 1) l = 1;
        inst = audio_start_def(a->vm->player, s->sd, l, off > 0 ? floor(off * 44100.0) : 0, owner, real(self));
    }
    if (inst) s->inst = inst;
    return v_undef();
}

static VM *g_match_vm; static AStr *g_match_path;
static int match_clip(void *ctx, void *dobj) {
    MovieClip *c = (MovieClip*)dobj, *t = (MovieClip*)ctx;
    if (c == t) return 1;
    if (!g_match_path) return 0;
    return str_eq(clip_path(g_match_vm, c), g_match_path);
}

static Value snd_stop(Act *a, AObj *self, int argc, Value *argv) {
    SoundData *s = snd_of(self); MovieClip *owner;
    if (!s) return v_undef();
    if (s->external) s->ext_autoplay = 0;
    if (argc > 0) {
        AStr *name = v_str_of(a, argv[0]); SWFMovie *m; SoundDef *sd;
        if (a->vm->err) return v_undef();
        m = lib_movie(a, s);
        if (!m) return v_undef();
        sd = find_lib_sound(a, m, name);
        if (sd) audio_stop_def(a->vm->player, sd);
    } else if (s->external) {
        if (s->sd) audio_stop_def(a->vm->player, s->sd);
        s->inst = 0;
    } else if ((owner = snd_owner(a, s)) != NULL) {
        g_match_vm = a->vm; g_match_path = clip_path(a->vm, owner);
        audio_stop_dobj(a->vm->player, match_clip, owner);
        g_match_path = NULL;
        s->inst = 0;
    } else if (use_global(s)) audio_stop_all(a->vm->player);
    return v_undef();
}

/* ---------------------------------------------------------------- */
/* ID3v2 (the frames the id3 crate turns into text and comments)    */

static AStr *id3_text(VM *vm, int enc, const u8 *p, size_t n) {
    SBuf b; size_t i;
    sb_init(&b);
    if (enc == 0) { for (i = 0; i < n && p[i]; i++) sb_unit(&b, p[i]); }
    else if (enc == 3) {
        size_t l = 0; while (l < n && p[l]) l++;
        { AStr *s = str_utf8(vm, (const char*)p, l); sb_free(&b); return s; }
    } else {
        int be = enc == 2;
        if (enc == 1 && n >= 2) { if (p[0] == 0xFF && p[1] == 0xFE) { be = 0; p += 2; n -= 2; } else if (p[0] == 0xFE && p[1] == 0xFF) { be = 1; p += 2; n -= 2; } }
        for (i = 0; i + 1 < n; i += 2) { u16 c = be ? (u16)((p[i] << 8) | p[i + 1]) : (u16)(p[i] | (p[i + 1] << 8)); if (!c) break; sb_unit(&b, c); }
    }
    return sb_done(vm, &b);
}

/* end of a terminated string in the given encoding (index after the terminator) */
static size_t id3_skip(int enc, const u8 *p, size_t n) {
    size_t i;
    if (enc == 0 || enc == 3) { for (i = 0; i < n; i++) if (!p[i]) return i + 1; return n; }
    for (i = 0; i + 1 < n; i += 2) if (!p[i] && !p[i + 1]) return i + 2;
    return n;
}

static const char *id3_v22(const char *id) {
    static const char *map[][2] = { { "TT2", "TIT2" }, { "TP1", "TPE1" }, { "TAL", "TALB" }, { "TRK", "TRCK" }, { "TYE", "TYER" }, { "TCO", "TCON" }, { "COM", "COMM" },
        { "TT1", "TIT1" }, { "TT3", "TIT3" }, { "TP2", "TPE2" }, { "TP3", "TPE3" }, { "TP4", "TPE4" }, { "TCM", "TCOM" }, { "TXT", "TEXT" }, { "TLA", "TLAN" },
        { "TBP", "TBPM" }, { "TCR", "TCOP" }, { "TDA", "TDAT" }, { "TEN", "TENC" }, { "TFT", "TFLT" }, { "TIM", "TIME" }, { "TKE", "TKEY" }, { "TLE", "TLEN" },
        { "TMT", "TMED" }, { "TOA", "TOPE" }, { "TOF", "TOFN" }, { "TOL", "TOLY" }, { "TOR", "TORY" }, { "TOT", "TOAL" }, { "TPA", "TPOS" }, { "TPB", "TPUB" },
        { "TRC", "TSRC" }, { "TRD", "TRDA" }, { "TSI", "TSIZ" }, { "TSS", "TSSE" }, { NULL, NULL } };
    int i;
    for (i = 0; map[i][0]; i++) if (!strcmp(map[i][0], id)) return map[i][1];
    return NULL;
}

static void load_id3(Act *a, AObj *self, const u8 *d, size_t n) {
    VM *vm = a->vm; int ver, flags; size_t size, pos, end; u8 *buf = NULL; const u8 *t; AObj *id3; AStr *k3 = str_intern(vm, "id3");
    if (n < 10 || memcmp(d, "ID3", 3)) return;
    ver = d[3]; flags = d[5];
    if (ver < 2 || ver > 4) return;
    size = ((size_t)(d[6] & 0x7F) << 21) | ((size_t)(d[7] & 0x7F) << 14) | ((size_t)(d[8] & 0x7F) << 7) | (d[9] & 0x7F);
    if (size + 10 > n) size = n - 10;
    t = d + 10;
    if (flags & 0x80 && ver < 4) {      /* unsynchronisation (whole tag) */
        size_t i, o = 0;
        buf = (u8*)malloc(size ? size : 1);
        if (!buf) return;
        for (i = 0; i < size; i++) { buf[o++] = t[i]; if (t[i] == 0xFF && i + 1 < size && t[i + 1] == 0) i++; }
        t = buf; size = o;
    }
    pos = 0; end = size;
    if (flags & 0x40 && ver >= 3 && size >= 4) {
        size_t eh = ver == 3 ? (((size_t)t[0] << 24) | ((size_t)t[1] << 16) | ((size_t)t[2] << 8) | t[3]) + 4 : ((size_t)(t[0] & 0x7F) << 21) | ((size_t)(t[1] & 0x7F) << 14) | ((size_t)(t[2] & 0x7F) << 7) | (t[3] & 0x7F);
        pos = eh;
    }
    id3 = obj_bare(vm);
    while (pos + (ver == 2 ? 6 : 10) <= end) {
        char id[5]; const char *fid; size_t fs; int fflags = 0, enc; const u8 *f; Value v1, v2; AStr *k1 = NULL, *k2;
        if (ver == 2) {
            memcpy(id, t + pos, 3); id[3] = 0;
            fs = ((size_t)t[pos + 3] << 16) | ((size_t)t[pos + 4] << 8) | t[pos + 5];
            pos += 6;
        } else {
            memcpy(id, t + pos, 4); id[4] = 0;
            if (ver == 4) fs = ((size_t)(t[pos + 4] & 0x7F) << 21) | ((size_t)(t[pos + 5] & 0x7F) << 14) | ((size_t)(t[pos + 6] & 0x7F) << 7) | (t[pos + 7] & 0x7F);
            else fs = ((size_t)t[pos + 4] << 24) | ((size_t)t[pos + 5] << 16) | ((size_t)t[pos + 6] << 8) | t[pos + 7];
            fflags = (t[pos + 8] << 8) | t[pos + 9];
            pos += 10;
        }
        if (!id[0] || fs > end - pos) break;           /* padding or a broken frame */
        f = t + pos; pos += fs;
        fid = ver == 2 ? id3_v22(id) : id;
        if (!fid) continue;
        if (ver == 3 && (fflags & 0x00C0)) continue;     /* compressed / encrypted */
        if (ver == 4 && (fflags & 0x000C)) continue;
        if (!fs) continue;
        enc = f[0];
        if (!strcmp(fid, "COMM")) {
            size_t p2; Value arrv; AObj *arr; int len;
            if (fs < 4) continue;
            p2 = 4 + id3_skip(enc, f + 4, fs - 4);
            v1 = v_str(id3_text(vm, enc, f + p2, fs - p2));
            k2 = str_intern(vm, fid);
            arrv = o_get(a, id3, k2); if (vm->err) { vm->err = 0; arrv = v_undef(); }
            if (arrv.t == V_OBJ) arr = arrv.u.o;
            else arr = obj_array(a, 0, NULL);
            len = o_length(a, arr); vm->err = 0;
            o_set_element(a, arr, len, v1); vm->err = 0;
            o_set_length(a, arr, len + 1); vm->err = 0;
            v2 = v_obj(arr);
            k1 = str_intern(vm, "comment");
        } else if (fid[0] == 'T' && strcmp(fid, "TXXX")) {
            v1 = v2 = v_str(id3_text(vm, enc, f + 1, fs - 1));
            k2 = str_intern(vm, fid);
            if (!strcmp(fid, "TALB")) k1 = str_intern(vm, "album");
            else if (!strcmp(fid, "TCON")) k1 = str_intern(vm, "genre");
            else if (!strcmp(fid, "TIT2")) k1 = str_intern(vm, "songname");
            else if (!strcmp(fid, "TPE1")) k1 = str_intern(vm, "artist");
            else if (!strcmp(fid, "TRCK")) k1 = str_intern(vm, "track");
            else if (!strcmp(fid, "TYER")) k1 = str_intern(vm, "year");
        } else continue;
        if (k1) { o_set(a, id3, k1, v1); vm->err = 0; }
        o_set(a, id3, k2, v2); vm->err = 0;
    }
    free(buf);
    if (!o_has_property(a, self, k3)) {
        o_set(a, self, k3, v_obj(id3)); vm->err = 0;
        o_set_attributes(vm, real(self), k3, DE | DD | RO, 0);
    }
    vm->err = 0;
    { Value t1 = v_bool(1); o_call_method(a, self, str_intern(vm, "onID3"), 1, &t1, ER_SPECIAL); vm->err = 0; }
}

/* ---------------------------------------------------------------- */
/* loadSound (local files load at once, like Ruffle's blocking loader) */

static void load_finish(Act *a, AObj *self, const char *full, u32 load_id);
extern void avm1_run_actions(VM *vm);

static Value snd_load(Act *a, AObj *self, int argc, Value *argv) {
    VM *vm = a->vm; SoundData *s = snd_of(self); AStr *url; int streaming; u32 load_id = 0; char full[1100];
    if (!s || !has_valid_owner(a, s) || argc < 1) return v_undef();
    url = v_str_of(a, argv[0]); if (vm->err) return v_undef();
    streaming = v_bool_of(a, arg(argc, argv, 1));
    s = snd_of(self); if (!s) return v_undef();
    if (s->external) {
        if (s->sd) audio_stop_def(vm->player, s->sd);
        if (s->ext_loading) { Value f = v_bool(0); o_call_method(a, self, str_intern(vm, "onLoad"), 1, &f, ER_SPECIAL); vm->err = 0; s = snd_of(self); if (!s) return v_undef(); }
        load_id = s->load_id + 1;
    }
    s->external = 1; s->ext_streaming = streaming; s->ext_autoplay = streaming; s->ext_loading = 1; s->load_id = load_id; st_default(s->ext_st);
    fetch_url_resolve(avm1_movie_url(vm->player, vm->player->movie), str_c(vm, url), full, sizeof(full));
    if (strncmp(full, "file:", 5)) {
        /* network loads complete later, at the end of the frame */
        PendLoad *pl = (PendLoad*)realloc(g_pend, (size_t)(g_npend + 1) * sizeof(PendLoad));
        if (pl) { g_pend = pl; g_pend[g_npend].obj = real(self); g_pend[g_npend].url = url; g_pend[g_npend].load_id = load_id; g_npend++; }
        return v_undef();
    }
    {
        extern void avm1_log_fetch(VM *vm, AStr *url, int post, const char *mime, const u8 *body, size_t blen);
        avm1_log_fetch(vm, url, 0, NULL, NULL, 0);
    }
    load_finish(a, self, full, load_id);
    return v_undef();
}

static void load_finish(Act *a, AObj *self, const char *full, u32 load_id) {
    VM *vm = a->vm; SoundData *s; char path[1100]; u8 *d = NULL; size_t n = 0; int ok = 0;
    if (fetch_url_to_path(vm->player, full, path, sizeof(path))) d = fetch_read_file(path, &n);
    s = snd_of(self); if (!s) { free(d); return; }
    if (s->load_id != load_id) { free(d); return; }
    s->bytes = (u32)n;
    if (d) {
        SoundDef *sd = sound_def_from_mp3(d, n);
        if (sd) {
            keep_ext(sd);
            set_sound(a, self, s, sd);
            s = snd_of(self);
            if (s) { s->has_dur = 1; s->duration = 0; }
            load_id3(a, self, d, n);
            s = snd_of(self);
            if (s) { s->duration = (u32)floor(sound_def_duration_ms(sd) + 0.5); }
            ok = 1;
        }
        free(d);
    }
    s = snd_of(self); if (!s) return;
    s->ext_loading = 0;
    { Value r = v_bool(ok); o_call_method(a, self, str_intern(vm, "onLoad"), 1, &r, ER_SPECIAL); if (vm->err) vm_report_error(a); vm->err = 0; }
    s = snd_of(self);
    if (s && s->ext_autoplay) snd_start(a, self, 0, NULL);
}

void avm1_sound_loads(VM *vm) {
    PendLoad *list = g_pend; int n = g_npend, i;
    if (!n) return;
    g_pend = NULL; g_npend = 0;
    for (i = 0; i < n; i++) {
        Act a; char full[1100];
        extern void avm1_log_fetch(VM *vm, AStr *url, int post, const char *mime, const u8 *body, size_t blen);
        act_init_nothing(&a, vm, vm->player->root, "[Loader]");
        avm1_log_fetch(vm, list[i].url, 0, NULL, NULL, 0);
        fetch_url_resolve(avm1_movie_url(vm->player, vm->player->movie), str_c(vm, list[i].url), full, sizeof(full));
        load_finish(&a, list[i].obj, full, list[i].load_id);
        avm1_run_actions(vm);
    }
    free(list);
}

/* ---------------------------------------------------------------- */
/* per frame: positions, completed sounds, transforms               */

typedef struct { VM *vm; } UpdCtx;

static void upd_voice(void *ctx, int inst, void *dobj, void *owner, int *st) {
    VM *vm = ((UpdCtx*)ctx)->vm; int t[5]; MovieClip *c = (MovieClip*)dobj; AObj *o = (AObj*)owner; SoundData *s = o ? snd_of(o) : NULL;
    if (s && s->external) memcpy(t, s->ext_st, sizeof(t)); else st_default(t);
    for (; c; c = c->parent) { A1Clip *ac = c->a1; if (ac && ac->has_st) st_concat(t, ac->st); }
    st_concat(t, g_st);
    memcpy(st, t, sizeof(t));
    if (s) { double ms; if (audio_instance_pos(vm->player, inst, &ms)) s->position = (u32)floor(ms + 0.5); }
}

void avm1_sound_frame(VM *vm) {
    Player *p = vm->player; AudioDone d; UpdCtx ctx;
    while (audio_take_done(p, &d)) {
        AObj *o = (AObj*)d.owner; SoundData *s = o ? snd_of(o) : NULL;
        if (!s) continue;
        s->position = (u32)floor(sound_def_duration_ms(d.sd) + 0.5);
        if (p->root) avm1_queue_method(vm, p->root, o, vm->s_onSoundComplete, 0, NULL);
    }
    ctx.vm = vm;
    audio_foreach_voice(p, upd_voice, &ctx);
}

static void mark_owner(void *ctx, void *owner) { gc_mark_obj((VM*)ctx, (AObj*)owner); }
void avm1_sound_mark(VM *vm) { int i; audio_foreach_owner(vm->player, mark_owner, vm); for (i = 0; i < g_npend; i++) { gc_mark_obj(vm, g_pend[i].obj); gc_mark_str(g_pend[i].url); } }

static void defm(VM *vm, Env *env, AObj *o, const char *name, NativeFn f, u16 attr) { o_define(vm, o, str_intern(vm, name), v_obj(fn_native(vm, env, f, NULL)), attr); }

AObj *lib_sound(VM *vm, Env *env, AObj *object_proto) {
    AObj *proto, *c = lib_class(vm, env, snd_ctor, NULL, object_proto, 1, &proto); u16 at = DE | DD | RO;
    env->p.sound = proto;
    defm(vm, env, proto, "getPan", snd_get_pan, at);
    defm(vm, env, proto, "getTransform", snd_get_transform, at);
    defm(vm, env, proto, "getVolume", snd_get_volume, at);
    defm(vm, env, proto, "setPan", snd_set_pan, at);
    defm(vm, env, proto, "setTransform", snd_set_transform, at);
    defm(vm, env, proto, "setVolume", snd_set_volume, at);
    defm(vm, env, proto, "stop", snd_stop, at);
    defm(vm, env, proto, "attachSound", snd_attach, at);
    defm(vm, env, proto, "start", snd_start, at);
    defm(vm, env, proto, "getDuration", snd_duration, at | V6);
    defm(vm, env, proto, "setDuration", snd_noop, at | V6);
    defm(vm, env, proto, "getPosition", snd_position, at | V6);
    defm(vm, env, proto, "setPosition", snd_noop, at | V6);
    defm(vm, env, proto, "loadSound", snd_load, at | V6);
    defm(vm, env, proto, "getBytesLoaded", snd_bytes, at | V6);
    defm(vm, env, proto, "getBytesTotal", snd_bytes, at | V6);
    return c;
}
