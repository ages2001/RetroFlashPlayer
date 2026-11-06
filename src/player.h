/* player.h - display list, MovieClip timelines, frame advance, render.
 *
 * Model follows the Flash player: every MovieClip instance owns a
 * depth-sorted display list of children; each tick, every playing clip
 * executes the control tags of its next frame, then frame actions run,
 * then children tick.  Newly placed sprites run their first frame at
 * placement so they are visible immediately. */
#ifndef FLASHNT_PLAYER_H
#define FLASHNT_PLAYER_H

#include "swf.h"
#include "raster.h"

struct MovieClip;
struct Player;
struct AObj;
struct A1Clip;

typedef struct DisplayObject {
    int        depth;
    int        char_id;
    SWFChar   *ch;
    SWFMatrix  matrix;
    SWFCxform  cxform;
    int        ratio;
    char       name[64];
    int        clip_depth;
    int        visible;
    int        blend;
    void      *blend_shader;     /* blendShader: a PbjJob snapshot (pbj.h), ref-counted */
    int        cache_bmp;        /* cacheAsBitmap (PlaceObject3 / script) */
    int        has_opaque_bg;    /* opaqueBackground: the bounds filled with opaque_bg (0xRRGGBB) */
    u32        opaque_bg;
    int        btn_state;        /* buttons: 0 up 1 over 2 down 3 out-down */
#define BTN_STATE_BIT(st) ((st) == 1 ? 2 : (st) == 2 ? 4 : 1)
    int        state_mask;       /* AVM2 SimpleButton state children: 1 up 2 over 4 down 8 hit (0 = normal child) */
    int        place_frame;      /* 0-based timeline frame that placed this object */
    double     sr_rot, sr_xs, sr_ys, sr_skew;   /* AVM1: cached rotation (degrees), scales (percent) and skew */
    int        sr_cached;
    int        script_moved;     /* AVM1: transformed by script, the timeline no longer moves it */
    int        script_placed;    /* AVM1: created by attachMovie / duplicateMovieClip / createEmptyMovieClip ... */
    int        explicit_name;    /* the name came from the PlaceObject tag or script */
    int        tl_depth, in_dl;  /* AS3 timelines: the depth-list entry (Ruffle depth_list) apart from the render order kept in `depth` */
    int        tl_inst;          /* AS3: instantiated by a PlaceObject tag */
    int        placed_by_script; /* AS3: added / moved / removed by ActionScript (RemoveObject tags no longer take it away) */
    int        manual_construct; /* AVM2: placed before its parent's object was constructed; only the parent's constructor constructs it */
    struct MovieClip *owner;     /* timeline that placed this object */
    struct MovieClip *clip;      /* child instance if sprite / edit text */
    void      *obj2;             /* AVM2 DisplayObject binding */
    struct BitmapData *bmd;      /* AVM1 attachBitmap: the bitmap shown (ch is a private CHAR_BITMAP) */
    int        bmd_smooth;
    int        has_scroll_rect;
    int        has_scroll_applied; SWFRect scroll_applied;   /* the scrollRect as last rendered (transforms and hit tests use it) */  /* scrollRect: shows scroll_rect (local twips) at the origin, clipped */
    SWFRect    scroll_rect;
    struct SwfFilter *filters;   /* bitmap filters (filters.h), malloc'd */
    int        nfilters;
    int        has_scale9;       /* scale9Grid (stored only) */
    SWFRect    scale9;
    void      *vstream;          /* Video: the NetStream attached by script (NetStream*) */
    int        vid_smooth, vid_cleared, vid_deblock;
    u32       *fcache_px; int fcache_w, fcache_h; u64 fcache_sh, fcache_fh;   /* filter-result cache (player.c render_cached) */
    struct DisplayObject *next;  /* ascending depth */
} DisplayObject;

typedef struct MovieClip {
    struct Player   *player;
    SWFMovie        *movie;
    Timeline        *tl;
    int              owns_tl;
    int              cur_frame;      /* 0-based frame currently shown */
    int              tag_cursor;     /* next tag to execute */
    int              playing;
    int              ran_this_tick;
    int              root_copy;        /* another instance of a main timeline: its DoABC/SymbolClass tags are not run again */
    int              dead;           /* retired, awaiting free */
    int              frame_count;
    DisplayObject   *children;
    DisplayObject   *goto_pool;      /* timeline objects parked during a backward goto */
    int              goto_target;    /* 0-based target frame of the running goto */
    int              goto_busy;      /* a goto is running: scripts see the target frame */
    struct MovieClip *parent;
    DisplayObject   *self;           /* placement record in parent */
    struct AObj     *obj;            /* AVM1 object for this clip */
    struct A1Clip   *a1;             /* AVM1 per-instance state (avm1_display.c) */
    void            *obj2;           /* AVM2 object (A2Obj*) */
    int              frame_changed;  /* AVM2: frame scripts pending for cur_frame */
    int              last_sf;        /* AVM2: 1 + the frame whose script last ran (0: none); a script runs once per frame visit */
    int              prog_played;    /* AVM2: play()/gotoAndPlay() was called (isPlaying is false until then) */
    int              advanced;       /* AVM2: the playhead has moved at least once (isPlaying) */
    int              queued_goto, queued_play;   /* AVM2: goto issued by the clip's own frame script (-1 none) */
    int              skip_enter;     /* AVM2 (SWF9 gotos): skip the next timeline advance */
    int              born_fs;        /* AVM2: 1 + the frame number in whose frame-script phase it was constructed */
} MovieClip;

/* scrollRect (dobj_geom.c): an object's content matrix, and entering it for a hit test */
void dobj_content_matrix(const DisplayObject *d, SWFMatrix *out);
int  dobj_enter(const DisplayObject *d, const SWFMatrix *pw, double sx, double sy, SWFMatrix *w);

/* AVM1 content inside an AVM2 player (flash.display.AVM1Movie): its clips follow AVM1 rules */
#define CLIP_A1MIX(c) ((c)->movie && (c)->movie->a1mix)
#define CLIP_IS_A1(c) (!(c)->player->avm2 || CLIP_A1MIX(c))
#define CLIP_IS_A2(c) ((c)->player->avm2 && !CLIP_A1MIX(c))

typedef struct { MovieClip *clip; const u8 *data; size_t len; int kind; /* 0 DoAction, 1 load events */ } QueuedAction;

typedef struct Player {
    SWFMovie   *movie;
    SWFMovie   *cur_movie;         /* movie of the clip being drawn / run (loaded SWFs), NULL = movie */
    char       *base_dir;          /* directory of the main SWF (relative loads) */
    SWFMovie  **loaded; int nloaded;   /* movies brought in by Loader (freed with the player) */
    MovieClip  *root;
    DisplayObject *levels;         /* AVM1 _level1 and up on the stage (ascending depth = level) */
    Surface    *surf;
    int         layer_bg;          /* rendering a layer on a plain background (0xRRGGBB), -1 none */
    double      zoom;
    int         has_view; int view_w, view_h; double view_sf;   /* the host viewport (device px), when it differs from the movie */
    int         has_stage_mat; SWFMatrix stage_mat;   /* stage.transform.matrix set by script: applies to the stage's children */
    int         fullscreen;
    int         tick;
    int         stopped;
    void      (*mix_hook)(struct Player *p);   /* host: mixes one frame of audio (AVM2 calls it inside the frame, after timers and streams) */
    struct SWFMovie *swap_root;   /* an AVM1 _level0 load of an AVM2 movie: the player restarts with it after the frame */
    int         loop;              /* main timeline loops at the end (default 1) */
    MovieClip  *focus;             /* editable text field with keyboard focus */
    MovieClip  *removed_fs[32];    /* AVM2: clips a goto removed while their frame script was pending */
    int         nremoved_fs;
    int         caret_blink;
    SWFColor    bg;
    int         bg_set;            /* the stage background was set (SetBackgroundColor / stage.color): later tags do not change it */
    /* input state */
    int         mouse_x, mouse_y, mouse_down;
    double      mouse_fx, mouse_fy;   /* fractional pixel part of the last pointer position */
    int         key_down[256];
    u8          key_toggled[256];   /* Key.isToggled: flipped on each press */
    int         last_key;
    int         click_set, click_last_idx, click_last_x, click_last_y, click_have; double click_last_ms;   /* click series (double clicks) */
    int         key_ev_loc, key_ev_char;
    struct SWFMovie **fref_m; int *fref_id, nfref;   /* fonts referenced by script Font objects */
    int        *gfont, ngfont;                     /* Font.registerFont: indices into fref */   /* the key event being dispatched: keyLocation, charCode (-1: derive from the key code) */
    /* AVM1 global context (owned) */
    void       *avm;
    /* AVM2 (ActionScript 3) context, NULL for AVM1 movies */
    void       *avm2;
    /* audio seam */
    void       *audio;
    /* objects removed during a tick are freed only at the end of it, so
     * running scripts never hold dangling clip pointers */
    /* global FIFO of frame actions in encounter order */
    QueuedAction *queue; int nqueue, capqueue, qhead;
    DisplayObject *graveyard;      /* removed this tick: unload fires next tick */
    DisplayObject *graveyard_old;  /* unload fired: freed at end of this tick */
    int         debug;
} Player;

Player     *player_create(SWFMovie *m, double zoom);
void        player_free(Player *p);

/* advance every playing clip by one frame */
void        player_tick(Player *p);
/* draw current state to p->surf */
void        player_render(Player *p);

/* input from the platform layer (stage pixels / Flash key codes) */
void        player_mouse(Player *p, int x, int y, int down);
void        player_mouse_button(Player *p, int x, int y, int button, int down);
void        player_mouse_wheel(Player *p, int delta);
void        player_click_index(Player *p, int index);   /* the next press is click number index+1 of a series (-1: by time and distance) */   /* button: 1 middle, 2 right (AVM2 events) */
void        player_key(Player *p, int keycode, int down);
void        player_key_ex(Player *p, int keycode, int down, int location, int ch);   /* location: KeyLocation; ch: character (-1 unknown) */
void        player_char(Player *p, int ch);         /* typed character (WM_CHAR) */
void        player_set_focus(Player *p, MovieClip *c);
void        player_ime_preedit(Player *p, const char *utf8, int from, int to);   /* IME composition (from < 0: no cursor) */
void        player_ime_commit(Player *p, const char *utf8);
void        player_focus_lost(Player *p);
struct ExtVal;
void        player_call_internal(Player *p, const char *name, int argc, const struct ExtVal *args, struct ExtVal *out);   /* container -> movie */
void        player_text_control(Player *p, const char *name);   /* editing command for the focused field */   /* the window lost the keyboard focus */
void        player_flush_graveyard(Player *p);
void        player_queue_load(Player *p, MovieClip *c);

/* MovieClip control (used by AVM1) */
void        player_avm1_advance(MovieClip *c);           /* one timeline frame (Flash run_frame_internal) */
void        player_avm1_goto(MovieClip *c, int frame, int play);   /* 0-based; stops/plays like gotoAndStop/Play */
void        player_unlink_dobj(MovieClip *c, DisplayObject *d);
void        player_resort(MovieClip *c);
MovieClip  *player_make_clip(Player *p, MovieClip *parent, SWFChar *ch, DisplayObject *self);
void        player_insert_dobj(MovieClip *c, DisplayObject *d);
void        clip_goto_frame(MovieClip *c, int frame, int play);
int         clip_find_frame_label(MovieClip *c, const char *label);
MovieClip  *clip_find_child(MovieClip *c, const char *name);
int         clip_current_frame(const MovieClip *c);
void        clip_remove_all(MovieClip *c);
DisplayObject *clip_find_depth(MovieClip *c, int depth);

DisplayObject *player_instantiate(Player *p, MovieClip *parent, SWFChar *ch, int char_id, int depth);
void        player_destroy_dobj(DisplayObject *d);

/* AVM1 levels and loaded movies */
MovieClip  *player_level(Player *p, int level);
MovieClip  *player_level_create(Player *p, int level, SWFMovie *m);
SWFMovie   *player_stub_movie(Player *p, int version, const char *url, int frames);
void        player_clip_replace_movie(MovieClip *c, SWFMovie *m);

/* path resolution: "_root", "_parent", "/a/b", "a.b" relative to c */
MovieClip  *clip_resolve_path(MovieClip *c, const char *path);

/* the dictionary to resolve character ids in right now */
#define PMOV(p) ((p)->cur_movie ? (p)->cur_movie : (p)->movie)

#endif
