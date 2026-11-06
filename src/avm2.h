/* avm2.h - ActionScript 3 runtime interface for the player */
#ifndef FLASHNT_AVM2_H
#define FLASHNT_AVM2_H

#include "player.h"

void  avm2_create(Player *p);                                 /* on first DoABC */
void  avm2_free(Player *p);
void  avm2_do_abc(Player *p, const u8 *data, size_t len);
void  avm2_symbol_class(Player *p, const u8 *data, size_t len);
void  avm2_object_placed(Player *p, DisplayObject *d);        /* timeline placed a new object */
void  avm2_frame_shown(Player *p, MovieClip *c);              /* ShowFrame reached */
void  avm2_goto_shown(Player *p, MovieClip *c);               /* an AS3 goto placed its frame */
int   avm2_dobj_removed(Player *p, DisplayObject *d);         /* timeline removal; 1 = AVM2 owns the record now */
void  avm2_dobj_detach(DisplayObject *d);                     /* record about to be freed */
void  avm2_clip_detach(MovieClip *c);
void  avm2_tick_begin(Player *p);
void  avm2_tick_entered(Player *p);
void  avm2_tick_end(Player *p);
void  avm2_mouse(Player *p, int x, int y, int down, int changed);
void  avm2_mouse_button(Player *p, int x, int y, int button, int down);
void  avm2_key(Player *p, int keycode, int down);
void  avm2_char(Player *p, int ch);
int   avm2_handles_char(Player *p);                           /* focused AVM2 text field */
void  avm2_resized(Player *p);

#endif
