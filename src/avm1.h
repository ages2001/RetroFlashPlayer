/* avm1.h - ActionScript 1/2 (AVM1): the interface the player uses. */
#ifndef FLASHNT_AVM1_H
#define FLASHNT_AVM1_H

#include "player.h"

void  *avm1_create(Player *p);
void   avm1_free(Player *p);
void   avm1_set_script_limits(Player *p, int max_recursion, int timeout_secs);
void   avm1_reset_exec_clock(Player *p);

/* AVM1 movies: load (init actions, the root object, frame 1) and one frame */
void   avm1_start(Player *p);
void   avm1_frame(Player *p);

/* timeline hooks */
void   avm1_post_instantiation(Player *p, MovieClip *c);   /* a timeline placed `c` */
void   avm1_clip_run_frame(Player *p, MovieClip *c);       /* load / enterFrame, then the timeline */
void   avm1_queue_do_action(Player *p, MovieClip *c, const u8 *code, size_t len);
void   avm1_remove_child(Player *p, MovieClip *parent, DisplayObject *d);
void   avm1_clip_actions(Player *p, MovieClip *c, const u8 *data, size_t len);  /* PlaceObject CLIPACTIONS */

/* run code right away with `target` as the timeline */
void   avm1_run(Player *p, MovieClip *target, const u8 *code, size_t len);

/* lifecycle */
void   avm1_clip_created(Player *p, MovieClip *c);
void   avm1_clip_unload(Player *p, MovieClip *c);
void   avm1_clip_destroyed(Player *p, MovieClip *c);
void   avm1_clip_loaded(Player *p, MovieClip *c);
void   avm1_clip_load_events(Player *p, MovieClip *c);
void   avm1_clip_detach(Player *p, MovieClip *c);     /* memory about to be freed: drop back-pointers */
void   avm1_bind_video(Player *p, MovieClip *c);      /* timeline Video instance */
void   avm1_bind_textfield(Player *p, MovieClip *c);  /* EditText instance -> TextField */
void   avm1_bind_button(Player *p, MovieClip *c);     /* DefineButton instance -> Button */
void   avm1_tick(Player *p);

/* buttons */
void   avm1_button_method(Player *p, MovieClip *button, const char *handler);

/* focus moved to a text field (NULL = none) */
void   avm1_focus_changed(Player *p, MovieClip *c);

/* key / mouse events */
void   avm1_key_event(Player *p, int keycode, int down);
void   avm1_mouse_event(Player *p, int x, int y, int down, int changed);

void   avm1_trace_out(const char *s);

#endif
