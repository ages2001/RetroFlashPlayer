/* avm2_flash_int.h - shared internals of the flash.* library files */
#ifndef FLASHNT_AVM2_FLASH_INT_H
#define FLASHNT_AVM2_FLASH_INT_H

#include "avm2_int.h"
#include "avm2.h"
#include "chars.h"
#include "shape.h"
#include "audio.h"
#include "render.h"

#define ARG(i) a2_arg(argc, argv, i)
#define NUM(i) v2_tonum(vm, ARG(i))
#define INT(i) v2_toint(vm, ARG(i))
#define STR(i) v2_tostr(vm, ARG(i))
#define BOOL(i) v2_tobool(vm, ARG(i))
#define SELFO (self.t == T_OBJ ? self.u.o : NULL)
#define UNUSED (void)vm; (void)self; (void)argc; (void)argv

/* per display object native state (A2Obj.payload for OK_DISPLAY) */
typedef struct A2Disp {
    int owns;                       /* the DisplayObject/MovieClip belong to this object (not on a timeline) */
    int is_stage, is_root, stage_child;
    int is_movie_root;
    int timeline_placed;            /* placed by a PlaceObject tag: the name is fixed for good */              /* main timeline of a loaded SWF (Loader content) */
    int button_state;               /* object is a SimpleButton state (no parent) */
    int orphan_listed;              /* in vm->orphans */
    int stage_notified;             /* addedToStage dispatched and not yet removedFromStage */   /* stage_child: added via stage.addChild (lives at the end of root's list) */
    /* transform cache (degrees / factors) */
    double rot, sx, sy, roty;       /* authoritative when `has_xform` (roty = y-axis angle) */
    double alpha; int has_alpha;    /* raw alpha as set by script (may exceed 0..1) */
    V2 opaque_bg; int has_opaque_bg;
    int has_xform;
    int has_m3d; double z;           /* Transform.matrix3D / z set: matrix reads null */
    double alpha_mul;
    /* drawing API */
    ShapeDef *drawing; int draw_fill, draw_line; s32 draw_x, draw_y, start_x, start_y; int draw_open;
    A2Obj *graphics;
    /* masking */
    MovieClip *mask; int is_mask; A2Obj *mask_obj;
    /* interactive */
    int mouse_enabled, mouse_children, button_mode, use_hand_cursor, tab_enabled, double_click;
    int focus_rect;
    int focus_rect_set, tab_enabled_set, tab_index_set, tab_index, no_tab_children;
    /* text field */
    int is_text; A2Str *text; A2Str *html_text; int tf_w, tf_h; u32 color; int size; int align; int wordwrap, multiline, html, border, background, autosize, password, selectable, input, embed;
    u32 bgcolor, bordercolor; A2Str *font; int leading, indent, lmargin, rmargin, bold, italic, max_chars, scroll_v;
    A2Obj *default_format;
    struct TextFieldState *tfs;   /* TextField content/layout (textfield.h) */
    /* bitmap */
    A2Obj *bitmap_data; int smoothing;
    int snapping;                 /* pixelSnapping: 0 auto, 1 never, 2 always */
    /* button */
    int btn_enabled;
    /* loader info */
    A2Obj *loader_info;
    /* scenes/labels cache */
    A2Obj *transform_obj;
    /* cached name for timeline-less objects */
    A2Str *name;
    /* dragging */
    int dragging; double drag_dx, drag_dy; int drag_lock; int has_drag_bounds; double drag_x0, drag_y0, drag_x1, drag_y1;
    int visible_override;
    int blend;
    int bmp_w, bmp_h;   /* Bitmap: size of bitmapData when it was assigned (survives dispose) */
    /* SimpleButton state objects */
    A2Obj *up, *over, *down, *hit;
    /* Video/Bitmap width/height for script objects */
    int obj_w, obj_h;
} A2Disp;

A2Disp *a2_disp(A2Obj *o);
DisplayObject *a2_dobj_of(A2Obj *o);
A2Obj *a2_parent_of(VM2 *vm, A2Obj *o);
int a2_on_stage(VM2 *vm, A2Obj *o);
A2Obj *a2_new_display(VM2 *vm, A2Class *cls);                     /* script-created display object with record+clip */
void a2_dispatch_event_obj(VM2 *vm, A2Obj *target, A2Obj *ev);
A2Obj *a2_make_event(VM2 *vm, A2Class *cls, A2Str *type, int bubbles, int cancelable);
void a2_broadcast(VM2 *vm, A2Str *type);
void a2_add_bcast(VM2 *vm, A2Obj *o);
int  a2_has_listener(A2Obj *o, A2Str *type);
void a2_flash_display_init(VM2 *vm);
void a2_flash_events_init(VM2 *vm);
void a2_flash_text_init(VM2 *vm);
void a2_flash_misc_init(VM2 *vm);
A2Class *a2_def_class(VM2 *vm, const char *ns, const char *name, A2Class *super, int kind, NativeFn2 ctor);
void a2_static_getter(VM2 *vm, A2Class *c, const char *name, NativeFn2 get, NativeFn2 set);
void a2_const_str(VM2 *vm, A2Class *c, const char *name, const char *v);
void a2_world_matrix(A2Obj *o, SWFMatrix *out);
void a2_local_bounds(VM2 *vm, A2Obj *o, SWFRect *r);
void a2_set_matrix_from_props(A2Obj *o);
A2Obj *a2_point(VM2 *vm, double x, double y);
A2Obj *a2_rect(VM2 *vm, double x, double y, double w, double h);
A2Obj *a2_matrix_obj(VM2 *vm, const SWFMatrix *m);
void a2_matrix_from_obj(VM2 *vm, V2 mo, SWFMatrix *m);
void a2_text_sync_from_char(VM2 *vm, A2Obj *o);
A2Str *a2_display_name(VM2 *vm, A2Obj *o);

#endif
