/* avm2_int.h - ActionScript 3 virtual machine internals */
#ifndef FLASHNT_AVM2_INT_H
#define FLASHNT_AVM2_INT_H

#include "abc.h"
#include "player.h"
#include <setjmp.h>

struct VM2; struct A2Obj; struct A2Class; struct A2Method;

enum { T_UNDEF = 0, T_NULL, T_BOOL, T_INT, T_UINT, T_NUM, T_STR, T_OBJ, T_HOLE };   /* T_HOLE: empty array slot (reads as undefined) */
#define V2_NORM(v) do { if ((v).t == T_HOLE) (v).t = T_UNDEF; } while (0)

typedef struct A2GC { struct A2GC *next; u8 kind; u8 mark; } A2GC;
enum { G_STR = 1, G_OBJ = 2, G_CLASS = 3 };

typedef struct A2Str { A2GC gc; u32 hash; u32 len; struct A2Str *inext; char *s; } A2Str;   /* always interned */

typedef struct V2 { u8 t; union { double n; s32 i; u32 u; int b; A2Str *s; struct A2Obj *o; } u; } V2;

typedef V2 (*NativeFn2)(struct VM2 *vm, V2 self, int argc, V2 *argv);

/* namespaces: identified by (kind, uri) */
typedef struct A2Ns { int kind; A2Str *uri; int id; } A2Ns;   /* id: private namespaces are distinct per ABC constant */
#define A2_NSID_RUNTIME (-1)   /* id of a non-private namespace supplied at run time (RTQName ns::name) */

typedef struct A2Method {
    AbcFile  *abc; u32 index;       /* ABC method, or */
    NativeFn2 native;               /* native implementation */
    short argmin, argmax;           /* C natives: argument count checked when argmax != 0 (-1 = any) */
    NativeFn2 abc_native;           /* body of a `native` method of the built-in ActionScript library */
    const void *native_data;        /* its registration data */
    A2Str    *name;
    int       nparams; int has_rest; int need_args; int need_activation;
    struct A2Class *owner;          /* class defining it (for super/protected) */
    int verify_state;               /* 0 unverified, 1 ok, -1 failed */
    const char *sig;                /* native signature "ret(p1,p2=)" for describeType, may be NULL */
    A2Str *dbg_name;                /* cached stack-trace name */
    int is_script_init;
    int is_static;                  /* class trait or class initialiser: runs in the class scope */
    int is_function;                /* created by newfunction (a closure) */
    void *rt;                       /* AbcRT the method belongs to */
    int freestanding;               /* associated by newfunction */
    int trait_bound;                /* referenced by a method/getter/setter trait (class or script) */
} A2Method;

enum { TK_SLOT = 0, TK_METHOD, TK_GETTER, TK_SETTER, TK_CLASS, TK_CONST };

typedef struct A2Trait {
    A2Str *name; A2Ns ns;
    int kind;
    int slot;                        /* slot index (slot/const/class) */
    A2Method *method, *getter, *setter;
    struct A2Class *type;            /* slot type (for coercion), may be NULL */
    struct A2Class *cls;             /* TK_CLASS: the class */
    int final_;
    V2  def; int has_def;            /* slot default value */
    u32 type_mn; AbcFile *abc;       /* unresolved slot type multiname */
    const AbcTrait *at, *at2;        /* source ABC trait (metadata) for slot/method/getter, and setter */
    AbcFile *abc2;                   /* ABC owning at2 */
    struct A2Trait *next;            /* bucket chain */
    struct A2Trait *onext;           /* declaration order */
} A2Trait;

typedef struct A2TraitMap { A2Trait **b; int nb; int n; A2Trait *first, *last; } A2TraitMap;

typedef struct A2Class {
    A2GC   gc;
    A2Str *name; A2Ns ns;
    struct A2Class *super;
    AbcFile *abc; AbcInstance *inst; AbcClass *cls; u32 cindex;
    A2TraitMap itraits;              /* instance traits incl. inherited */
    A2TraitMap ctraits;              /* static traits */
    int nislots, ncslots;
    A2Method *iinit, *cinit;
    struct A2Obj *obj;               /* Class object (statics in obj->slots) */
    struct A2Obj *prototype;
    struct A2Class **ifaces; int nifaces;
    int flags;                       /* CF_* */
    int inited;                      /* cinit ran */
    int initing;
    int api_air;                     /* library class of AIR's API only */
    NativeFn2 native_ctor;           /* called for natives / native-backed instances with self=new obj */
    NativeFn2 native_call;           /* Class called as a function (conversion); default coerce */
    int script;                      /* OK_GLOBAL classes: script index */
    void *rt;                        /* AbcRT owning this class */
    int kind;                        /* object kind instances get (OK_*) */
    int is_dynamic;                  /* dynamic class (non-sealed) */
    int ns_private;                  /* declared in a file-private namespace */
    A2Ns protected_ns;
    int symbol_char;                 /* SymbolClass: character id bound (-1 none) */
    struct SWFMovie *symbol_movie;   /* ... in this movie's dictionary */
    struct A2Obj *scope;             /* instance scope (script global + outer classes + this class) */
    struct A2Obj *cscope;            /* class scope (without the class object) */
    int depth;                       /* inheritance depth */
    const char *ctor_sig;            /* native constructor signature "(p1,p2=)" */
    int shadow_slots;                /* -1 unknown, 1 if a subclass slot shadows a base slot */
} A2Class;
/* static members are not inherited by subclass class objects (classes defined in ABC code;
   natively built classes still expose their base class statics) */
#define A2_STATIC_SUPER(c) ((c)->abc ? (A2Class*)NULL : (c)->super)

enum {
    OK_PLAIN = 0, OK_FUNCTION, OK_CLASS, OK_ARRAY, OK_NAMESPACE, OK_PRIM, OK_QNAME,
    OK_DISPLAY, OK_SCOPE, OK_ACTIVATION, OK_DICT, OK_BYTEARRAY, OK_VECTOR,
    OK_XML, OK_REGEXP, OK_DATE, OK_EVENT, OK_TIMER, OK_SOUND, OK_SOUNDCHANNEL, OK_PROXY,
    OK_GLOBAL, OK_CATCH, OK_BOUND, OK_ERROR, OK_POINT, OK_RECT, OK_MATRIX,
    OK_COLORTRANSFORM, OK_TEXTFORMAT, OK_GRAPHICS, OK_BITMAPDATA, OK_LOADER,
    OK_URLREQUEST, OK_SHAREDOBJ, OK_SOUNDTRANSFORM, OK_NETSTREAM
};

typedef struct A2Prop { A2Str *key; V2 v; struct A2Prop *next; struct A2Prop *onext, *oprev; int hidden; int deleted; } A2Prop;

typedef struct A2Listener { V2 fn; int capture; int priority; A2Str *type; struct A2Listener *next; } A2Listener;

typedef struct A2Obj {
    A2GC   gc;
    u8     kind;
    A2Class *cls;
    struct A2Obj *proto;
    V2    *slots; int nslots;
    A2Prop **dyn; int ndyn, dcount, ndeleted; A2Prop *dfirst, *dlast;   /* insertion order */
    u8     hash_enum;   /* script object: for-in walks an avmplus-style hashtable (a2_dyn_enum_hashed) */
    /* function */
    A2Method *method; struct A2Obj *scope; V2 receiver; int is_bound;
    /* class */
    A2Class *as_class;
    /* array / vector / scope chain */
    V2 *arr; int alen, acap; A2Class *vec_type; int vec_fixed; u32 biglen; /* sparse logical length (> alen) */
    /* namespace */
    A2Ns nsval;
    /* primitive wrapper */
    V2 prim;
    /* display object binding */
    MovieClip *clip; DisplayObject *dobj; struct A2Obj *parent_disp;
    A2Listener *listeners;
    /* misc payload */
    void *payload; int ival; double dval;
    /* frame scripts (MovieClip): fn per frame */
    V2 *frame_scripts; int nframe_scripts;
    /* dictionary pairs */
    V2 *dkeys; V2 *dvals; int dn, dcap;
    /* bytearray */
    u8 *bytes; int blen, bcap, bpos;
    /* graphics (drawing API) */
    void *gfx;
} A2Obj;

#define A2_STACK 65536
#define A2_SCOPES 16384      /* scope stack entries (each call copies its outer scope chain) */
#define A2_DEPTH 4096        /* call stack entries */
#define A2_MAX_DEPTH 3000    /* AS3 call depth that raises #1023 */

typedef struct A2Frame {
    struct A2Frame *caller;
    A2Method *m; AbcFile *abc; AbcBody *body;
    const u8 *code; u32 pc;
    V2 *locals; int nlocals;
    V2 *stack; int sp;                /* operand stack base pointer within vm stack */
    A2Obj **scope; int nscope; int scope_base;
    A2Str *saved_dxns;
    V2 self;
    A2Obj *saved_scope;               /* captured outer scope object chain (OK_SCOPE) */
    A2Obj *activation;
} A2Frame;

typedef struct VM2 {
    Player *player;
    A2GC *gc_list; size_t gc_count, gc_threshold;
    A2Str **intern; int nintern, capintern;
    V2 stack[A2_STACK]; int sp;
    A2Obj *scope_stack[A2_SCOPES]; int scope_sp;
    unsigned char scope_with[A2_SCOPES];  /* 1 = pushed by pushwith (dynamic lookup allowed) */
    struct A2Class *scope_decl[A2_SCOPES]; /* static type of a pushscope operand known from a coerce (NULL = its own class) */
    /* exceptions */
    int   throwing; V2 exc; int in_throw;
    /* classes by qualified name */
    struct { A2Str *name; A2Str *ns; A2Class *cls; } *classes; int nclasses, capclasses;
    A2Class *c_object, *c_class, *c_function, *c_array, *c_string, *c_number, *c_int, *c_uint, *c_boolean,
            *c_namespace, *c_error, *c_math, *c_date, *c_regexp, *c_xml, *c_xmllist, *c_vector, *c_dictionary,
            *c_bytearray, *c_eventdispatcher, *c_event, *c_mouseevent, *c_keyboardevent, *c_timerevent, *c_textevent,
            *c_displayobject, *c_interactive, *c_container, *c_sprite, *c_movieclip, *c_shape, *c_bitmap, *c_bitmapdata,
            *c_stage, *c_graphics, *c_textfield, *c_textformat, *c_point, *c_rectangle, *c_matrix, *c_colortransform,
            *c_timer, *c_sound, *c_soundchannel, *c_soundtransform, *c_loader, *c_urlrequest, *c_sharedobject,
            *c_simplebutton, *c_morphshape, *c_statictext, *c_video, *c_loaderinfo, *c_applicationdomain, *c_qname;
    A2Obj *global;                     /* shared global object (package scope) */
    A2Obj *dom_mem, *dom_default;      /* ApplicationDomain.domainMemory, and the built-in 1024 bytes used without one */
    A2Obj *stage_obj, *root_obj;
    A2Str *s_length, *s_prototype, *s_constructor, *s_toString, *s_valueOf, *s_empty, *s_undefined, *s_null,
          *s_object, *s_AS3, *s_enterFrame, *s_exitFrame, *s_addedToStage, *s_removedFromStage, *s_added, *s_removed,
          *s_frameConstructed, *s_mouseDown, *s_mouseUp, *s_mouseMove, *s_mouseWheel, *s_click, *s_keyDown, *s_keyUp, *s_timer, *s_timerComplete,
          *s_rollOver, *s_rollOut, *s_mouseOver, *s_mouseOut, *s_complete, *s_init, *s_resize, *s_soundComplete,
          *s_change, *s_textInput, *s_callee, *s_message, *s_name, *s_id3;
    A2Ns ns_public, ns_as3, ns_private_dummy;
    AbcFile **abcs; int nabcs;
    int *dom_parent; int ndom;        /* ApplicationDomains: 0 system, 1 the main movie's */
    int lookup_domain;                /* > 0: name lookups resolve in this domain */
    double time_ms;
    A2Obj **globals; int nglobals, capglobals;   /* script global objects (all ABCs) */
    int swf_version;
    /* string builder for `global += "x"` loops (avm2.c): an unpublished (not interned) string held
       only by slot sb_slot of sb_obj, grown in place; published at the first other access */
    struct A2Str *sb_str; struct A2Obj *sb_obj; int sb_slot; size_t sb_cap;
    int root_detached;                /* the main timeline was removed from the stage */
    u32 rng;
    int depth;
    A2Method *cs[A2_DEPTH];             /* call stack (indexed by depth) for stack traces */
    struct A2Class *cs_cls[A2_DEPTH];   /* native constructor frames (cs[d] NULL) */
    int cs_line[A2_DEPTH]; A2Str *cs_file[A2_DEPTH];   /* debugger mode: the frame's last debugline / debugfile */
    int quiet_uncaught;             /* uncaught errors are not written to the trace log (mouse event listeners) */
    int debugger;                   /* stack traces as the debug player prints them ([file:line], native glue frames) */
    int throw_from_native, depth_ctor_skip; const char *throw_frame;          /* the error being built was thrown by native code */
    A2Str *dxns;                    /* default xml namespace of the running method (NULL = "") */
    int trace;
    /* current frame for runtime namespace / default xml ns */
    A2Frame *cur;
    /* symbol class bindings: char id -> class */
    struct { int id; A2Class *cls; struct SWFMovie *movie; } *symbols; int nsymbols;
    /* timers */
    struct { A2Obj *timer; } timers[128]; int ntimers;
    /* per-object pending frame script execution */
    A2Obj *hover;                     /* current rollOver target */
    struct MovieClip *a1_pick;        /* the last hit test ended on an AVM1 object inside an AVM1Movie */
    A2Obj *pressed, *pressed_mid, *pressed_right; int button_down_mask;
    A2Obj *building_button; int root_children_done; int in_enterframe;
    int in_frame_script;
    MovieClip *fs_stack[256]; int fs_depth;      /* clips whose frame script is running */
    int in_fc;                        /* the frameConstructed broadcast of a frame is running */
    A2Obj *fc_reenter[16]; int nfc_reenter;   /* SWF9 clips that went to their own frame during it: enterFrame again afterwards */
    int fs_phase;                     /* the frame-script phase of a frame (or of a goto's inner frame) is running */
    A2Obj *fsq[256]; int nfsq;        /* frame scripts registered for the current frame during that phase (Ruffle frame_script_cleanup_queue) */
    /* DefineSceneAndFrameLabelData of the main timeline */
    struct { A2Str *name; int offset; } *scenes; int nscenes;
    struct { A2Str *name; int frame; } *slabels; int nslabels; int scenes_parsed, scene_tag_present;   /* SimpleButton whose states are under construction */              /* SimpleButton states under construction (all count as on stage) */
    /* broadcast listeners (enterFrame etc.), pruned by GC */
    A2Obj **orphans; unsigned char *orphan_tl; int norphans, caporphans;   /* parentless movie clips that still run their timelines (weak; orphan_tl: removed by a RemoveObject tag, dropped at the end of the frame) */
    A2Obj **bcast; struct A2Str **bcast_t; int nbcast, capbcast;   /* broadcast listeners per event type, in registration order */
    /* timeline objects placed this frame, constructed at ShowFrame */
    DisplayObject **pending; int npending, cappending;
    double frame_ms;
    A2Obj *focus;                     /* keyboard focus (InteractiveObject) */
    int focus_hl, stage_focus_rect_off;
    int air;                          /* the runtime is AIR (FLASHNT_RUNTIME=AIR) */
    const void *native_data;          /* data of the running library native */   /* focus highlight: 0 inactive, 1 hidden, 2 visible */
    double mouse_x, mouse_y;          /* stage pixels (sub-pixel positions from scripted input) */
    int root_constructed;
    int frame_no;                     /* frames run (Loader timing) */
    int nuncaught;                    /* uncaught errors reported so far */
    int wheel_delta;
    int click_index;                  /* 0 single click, 1 double, 2 triple (set by the host) */                  /* delta of the mouseWheel event being sent */
    void **defer_rt; int ndefer;      /* DoABC entry points waiting for the end of the frame's tags */
    int instance_counter;
    A2Obj *callee_obj;                /* closure being invoked (arguments.callee) */
} VM2;

/* values */
V2 v2_undef(void); V2 v2_hole(void); V2 v2_null(void); V2 v2_bool(int b); V2 v2_int(s32 i); V2 v2_uint(u32 u); V2 v2_num(double d);
V2 v2_str(A2Str *s); V2 v2_obj(A2Obj *o); V2 v2_cstr(VM2 *vm, const char *s);
A2Str *a2_intern(VM2 *vm, const char *s, size_t len);
A2Str *a2_str(VM2 *vm, const char *s);
A2Str *a2_intern_adopt(VM2 *vm, A2Str *r);   /* intern a built string: r itself or an equal one */
void  *a2_gc_alloc_str(VM2 *vm);             /* a bare (not interned) string header */
A2Str *a2_sb_publish(VM2 *vm);               /* intern the pending string builder (avm2.c) */
A2Str *a2_concat(VM2 *vm, A2Str *a, A2Str *b);
double v2_tonum(VM2 *vm, V2 v);
s32    v2_toint(VM2 *vm, V2 v);
u32    v2_touint(VM2 *vm, V2 v);
int    v2_tobool(VM2 *vm, V2 v);
A2Str *v2_tostr(VM2 *vm, V2 v);
A2Obj *v2_toobj(VM2 *vm, V2 v);
V2     v2_toprim(VM2 *vm, V2 v, int hint_str);
int    v2_strict_eq(VM2 *vm, V2 a, V2 b);
int    v2_eq(VM2 *vm, V2 a, V2 b);
int    v2_is_num(V2 v);
A2Class *v2_class_of(VM2 *vm, V2 v);
char  *v2_num_to_cstr(double d, char *buf);
char  *a2_format_number(double value, int mode, int precision, char *buffer);   /* 0 normal 1 fixed 2 precision 3 exponential */

/* objects */
A2Obj *a2_new(VM2 *vm, A2Class *cls);                 /* allocates with slots per class, no ctor */
A2Obj *a2_new_plain(VM2 *vm);
A2Obj *a2_new_array(VM2 *vm);
int a2_verify_body(VM2 *vm, A2Method *m, char *msg, int msglen);
A2Obj *a2_new_fn(VM2 *vm, A2Method *m, A2Obj *scope);
A2Class *a2_vec_elem_type(VM2 *vm, A2Class *c);
V2 a2_vec_coerce(VM2 *vm, V2 v, A2Class *type);
A2Str *a2_error_fmt(VM2 *vm, V2 v);
A2Str *a2_qname_uri(A2Obj *o); A2Str *a2_qname_local(VM2 *vm, A2Obj *o); int a2_qname_attr(A2Obj *o); V2 a2_make_qname(VM2 *vm, A2Str *uri, A2Str *local, int attr);
A2Str *a2_class_name_dotted(VM2 *vm, A2Class *c);
void a2_throw_coerce_error(VM2 *vm, V2 v, A2Class *type);
void a2_throw_id(VM2 *vm, const char *cls, int id, const char *a1, const char *a2);
A2Class *a2_vector_class_for(VM2 *vm, A2Class *t);
A2Obj *a2_new_native_fn(VM2 *vm, NativeFn2 fn, const char *name, int nparams);
/* natives of the built-in ActionScript library: "pkg::Class/name", "pkg::Class/get name",
 * "pkg::Class/set name", "pkg::Class$/name" (static), "pkg::name" (package function) */
void a2_register_native(const char *key, NativeFn2 fn);
void a2_register_native_data(const char *key, NativeFn2 fn, const void *data);   /* data: vm->native_data during the call */
void   a2_arr_push(VM2 *vm, A2Obj *a, V2 v);
V2     a2_arr_get(A2Obj *a, int i);
void   a2_arr_set(VM2 *vm, A2Obj *a, int i, V2 v);
int    a2_arr_set_sparse(VM2 *vm, A2Obj *a, u32 idx, V2 v); /* handles huge indices; returns 1 if handled */
V2     a2_arr_length_val(A2Obj *a);
void   a2_arr_setlen(VM2 *vm, A2Obj *a, int n);   /* logical length */
void   a2_arr_fill(VM2 *vm, A2Obj *a, int n);     /* grow the dense part with holes */
u32    a2_arr_len(A2Obj *a);                       /* logical length */
int    a2_arr_sealed(VM2 *vm, A2Obj *a);           /* sealed Array subclass: 1 (SWF <= 12), 2 (SWF 13+) */
A2Prop *a2_dyn_find(A2Obj *o, A2Str *key);
void   a2_dyn_set(VM2 *vm, A2Obj *o, A2Str *key, V2 v);
void   a2_dyn_set_hidden(VM2 *vm, A2Obj *o, A2Str *key, V2 v);   /* non-enumerable */
int    a2_dyn_del(A2Obj *o, A2Str *key);

/* property protocol (multiname = name + namespace candidates) */
typedef struct A2Name { A2Str *name; A2Ns *nss; int nns; int any_ns; int attr; } A2Name;
void a2_throw_readonly(VM2 *vm, V2 obj, const A2Name *n);
void a2_throw_cannot_create(VM2 *vm, V2 obj, const A2Name *n);
void a2_throw_not_found(VM2 *vm, V2 obj, const A2Name *n);
/* flash.utils.Proxy hooks (avm2_misc.c) */
int a2_proxy_get(VM2 *vm, A2Obj *o, const A2Name *n, V2 *out);
int a2_proxy_set(VM2 *vm, A2Obj *o, const A2Name *n, V2 v);
int a2_proxy_delete(VM2 *vm, A2Obj *o, const A2Name *n);
int a2_proxy_has(VM2 *vm, A2Obj *o, const A2Name *n);
V2  a2_proxy_call(VM2 *vm, A2Obj *o, const A2Name *n, int argc, V2 *argv);
V2  a2_proxy_descendants(VM2 *vm, A2Obj *o, const A2Name *n);
int a2_proxy_next_index(VM2 *vm, A2Obj *o, int idx);
V2  a2_proxy_next_name(VM2 *vm, A2Obj *o, int idx);
V2  a2_proxy_next_value(VM2 *vm, A2Obj *o, int idx);
int    a2_get(VM2 *vm, V2 obj, const A2Name *n, V2 *out);     /* 1 found */
int    a2_set(VM2 *vm, V2 obj, const A2Name *n, V2 v);
int    a2_init(VM2 *vm, V2 obj, const A2Name *n, V2 v);        /* initproperty (allows const) */
int    a2_has(VM2 *vm, V2 obj, const A2Name *n);
int    a2_delete(VM2 *vm, V2 obj, const A2Name *n);
V2     a2_call_prop(VM2 *vm, V2 obj, const A2Name *n, int argc, V2 *argv);
V2     a2_construct_prop(VM2 *vm, V2 obj, const A2Name *n, int argc, V2 *argv);
V2     a2_get_s(VM2 *vm, V2 obj, const char *name);             /* public simple name */
void   a2_set_s(VM2 *vm, V2 obj, const char *name, V2 v);
V2     a2_call_s(VM2 *vm, V2 obj, const char *name, int argc, V2 *argv);
A2Trait *a2_find_trait(A2TraitMap *tm, A2Str *name, const A2Name *n);
A2Trait *a2_find_trait_any(A2TraitMap *tm, A2Str *name);

/* calls */
V2     a2_call(VM2 *vm, V2 fn, V2 self, int argc, V2 *argv);
V2     a2_call_method(VM2 *vm, A2Method *m, A2Obj *scope, V2 self, int argc, V2 *argv);
V2     a2_construct(VM2 *vm, V2 ctor, int argc, V2 *argv);
V2     a2_construct_class(VM2 *vm, A2Class *c, int argc, V2 *argv);
void   a2_run_ctor(VM2 *vm, A2Class *c, A2Obj *self, int argc, V2 *argv);
void   a2_throw(VM2 *vm, V2 v);
void   a2_throw_error(VM2 *vm, const char *cls, const char *msg, int id);
A2Obj *a2_new_error(VM2 *vm, const char *cls, const char *msg, int id);

/* classes */
A2Class *a2_class_new(VM2 *vm, const char *ns, const char *name, A2Class *super, int kind);
void     a2_class_register(VM2 *vm, A2Class *c);
int a2_cur_domain(VM2 *vm); int a2_abc_domain(AbcFile *a); int a2_new_domain(VM2 *vm, int parent); int a2_domain_parent(VM2 *vm, int d);
A2Class *a2_class_find_dom(VM2 *vm, A2Str *ns, A2Str *name, int dom);
A2Class *a2_class_find(VM2 *vm, A2Str *ns, A2Str *name);
A2Class *a2_class_find_s(VM2 *vm, const char *qualified);   /* "flash.display::Sprite" or "Object" */
void     a2_class_init(VM2 *vm, A2Class *c);                  /* run cinit once */
int      a2_is_subclass(A2Class *c, A2Class *base);
int      a2_instanceof(VM2 *vm, V2 v, A2Class *c);
V2       a2_coerce(VM2 *vm, V2 v, A2Class *type);
void     a2_add_method(VM2 *vm, A2Class *c, const char *name, NativeFn2 fn, int nparams);
void     a2_add_static(VM2 *vm, A2Class *c, const char *name, NativeFn2 fn, int nparams);
void     a2_add_getter(VM2 *vm, A2Class *c, const char *name, NativeFn2 get, NativeFn2 set);
void     a2_add_const(VM2 *vm, A2Class *c, const char *name, V2 v);
void     a2_add_slot(VM2 *vm, A2Class *c, const char *name, A2Class *type);
A2Method *a2_native_method(VM2 *vm, NativeFn2 fn, const char *name, int nparams);
A2Obj   *a2_native_fn_obj(VM2 *vm, A2Class *c, const char *name);

/* scripts */
int    avm2_load_abc(VM2 *vm, const u8 *data, size_t len, int lazy);   /* parse + run scripts */
A2Obj *a2_new_scope(VM2 *vm, A2Obj **objs, int n);
int    a2_find_property(VM2 *vm, A2Obj **scopes, int n, const A2Name *name, V2 *out);  /* scope chain lookup; out = holder */
V2     a2_bound_fn(VM2 *vm, A2Method *m, A2Obj *scope, V2 self);
A2Class *a2_class_alloc(VM2 *vm, A2Str *ns, A2Str *name, A2Class *super, int kind, int reg);
void   a2_class_set_defaults(VM2 *vm, A2Class *c, A2Obj *o);
void   a2_dyn_enum(A2Obj *o, int *idx, A2Str **key, V2 *val);  /* idx: 0 start; returns next */

/* display / events */
A2Obj *a2_display_obj(VM2 *vm, MovieClip *clip, DisplayObject *d);    /* binding for a timeline object */
void   a2_dispatch(VM2 *vm, A2Obj *target, A2Obj *event);
A2Obj *a2_new_event(VM2 *vm, A2Class *cls, A2Str *type, int bubbles);
void   a2_dispatch_simple(VM2 *vm, A2Obj *target, A2Str *type, int bubbles);
void   a2_run_frame_scripts(VM2 *vm, A2Obj *mc, int frame);
void   a2_construct_display(VM2 *vm, MovieClip *clip, DisplayObject *d);

/* library setup */
void   a2_lib_init(VM2 *vm);
void   a2_flash_init(VM2 *vm);

/* GC */
void   a2_gc(VM2 *vm);
void   a2_mark_v(VM2 *vm, V2 v);
void   a2_mark_obj(VM2 *vm, A2Obj *o);
void   a2_mark_class(VM2 *vm, A2Class *c);

/* misc */
V2     a2_arg(int argc, V2 *argv, int i);
int    a2_is_index(A2Str *s, u32 *idx);
void   a2_dyn_enum_hashed(A2Obj *o, int *idx, A2Str **key, V2 *val);
A2Str *a2_class_name(VM2 *vm, A2Class *c, int qualified);
#define VM2OF(p) ((VM2*)(p)->avm2)

#endif
