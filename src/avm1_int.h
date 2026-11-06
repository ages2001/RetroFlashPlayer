/* avm1_int.h - AVM1 (ActionScript 1/2) internals shared by avm1*.c
 *
 * Semantics follow the Flash Player as documented by the Ruffle test
 * suite: values, conversions and comparisons depend on the SWF version of
 * the running code; objects are ordered property maps whose keys compare
 * case-insensitively up to SWF 6; there are two global environments (case
 * insensitive for SWF <= 6, case sensitive for SWF 7+).
 *
 * Memory: strings, objects, functions, scopes and constant pools are GC
 * nodes on one list.  Collection is mark-sweep and only runs between
 * ticks, when no script is executing, so C locals never need rooting.
 *
 * Errors: a thrown value or an engine error (recursion limits, timeout)
 * sets vm->err; every call site checks it and unwinds. */
#ifndef FLASHNT_AVM1_INT_H
#define FLASHNT_AVM1_INT_H

#include "avm1.h"
#include "player.h"

struct AObj;
struct AFunc;
struct Scope;
struct CPool;
struct ClipRef;
struct VM;
struct Act;

/* ---------------------------------------------------------------- */
/* GC nodes                                                         */

typedef struct GCHdr { struct GCHdr *next; u8 kind; u8 mark; } GCHdr;
enum { GC_STR = 1, GC_OBJ, GC_FN, GC_SCOPE, GC_POOL, GC_REF };

/* ---------------------------------------------------------------- */
/* strings: Latin-1 bytes or UTF-16 code units                      */

typedef struct AStr {
    GCHdr  gc;
    u32    len;          /* code units */
    u32    hash;         /* case-insensitive hash (Flash lowercase mapping) */
    u8     wide;         /* 0: d.b (Latin-1), 1: d.w (UTF-16) */
    u8     interned;
    union { u8 *b; u16 *w; } d;
    char  *utf8;         /* cached UTF-8 form (NUL terminated), lazily built */
} AStr;

#define STR_AT(s, i) ((s)->wide ? (s)->d.w[i] : (u16)(s)->d.b[i])

/* ---------------------------------------------------------------- */
/* values                                                           */

enum { V_UNDEF = 0, V_NULL, V_BOOL, V_NUM, V_STR, V_OBJ, V_CLIP };

typedef struct Value {
    u8 t;
    union { double n; int b; AStr *s; struct AObj *o; struct ClipRef *c; } u;
} Value;

/* a reference to a movie clip by path: survives the clip being removed and
 * placed again under the same name (Flash's "MovieClip" value type) */
typedef struct ClipRef {
    GCHdr  gc;
    AStr  *path;                 /* full dotted path "_level0.a.b" */
    int    level;
    AStr **seg; int nseg;        /* path segments below the level */
    struct AObj *cached;         /* last resolved object (may be removed) */
} ClipRef;

/* ---------------------------------------------------------------- */
/* properties                                                       */

enum {
    PA_DONT_ENUM = 1, PA_DONT_DELETE = 2, PA_READ_ONLY = 4,
    PA_VERSION_MASK = 0x1FFF << 3,
    PA_VERSION_6 = 0x0080, PA_VERSION_7 = 0x0500, PA_VERSION_8 = 0x1000,
    PA_VERSION_9 = 0x2000, PA_VERSION_10 = 0x4000,
    PA_PROTO_SWF7 = 0x8000,       /* a __proto__ link SWF6 movies do not follow */
    PA_ENUM_BY_VERSION = 0x0800,  /* not enumerated by the movies it is hidden from */
    PA_NEVER_ENUM = 0x0200,       /* the Linux player's namespace: a member it does not have (never enumerated) */
    PA_NOT_OWN = 0x0040           /* enumerated where an own member is, but hasOwnProperty does not report it */
};

typedef struct Prop {
    AStr  *key;                  /* NULL: deleted slot */
    Value  v;                    /* stored data */
    struct AObj *getter, *setter;  /* virtual property when getter != NULL */
    u32    id;                   /* identity for the property recursion limit */
    u16    attr;
} Prop;

typedef struct PropMap {
    Prop  *items; int n, cap;    /* insertion order (tombstones have key == NULL) */
    int    live;                 /* number of live items */
    int   *index; int icap;      /* open addressing: item index + 1, 0 empty */
} PropMap;

typedef struct Watch { AStr *name; struct AObj *cb; Value user; } Watch;

/* ---------------------------------------------------------------- */
/* objects                                                          */

enum {
    NK_NONE = 0, NK_SUPER, NK_BOOL, NK_NUMBER, NK_STRING, NK_ARRAY, NK_FUNCTION,
    NK_MOVIECLIP, NK_BUTTON, NK_EDITTEXT, NK_VIDEO, NK_DATE,
    NK_FILTER, NK_COLORTRANSFORM, NK_TRANSFORM, NK_TEXTFORMAT, NK_NETSTREAM,
    NK_BITMAPDATA, NK_XML, NK_XMLNODE, NK_SHAREDOBJECT, NK_XMLSOCKET,
    NK_FILEREF, NK_NETCONN, NK_LOCALCONN, NK_SOUND, NK_STYLESHEET, NK_TEXTSNAPSHOT
};

typedef struct AObj {
    GCHdr   gc;
    u8      nk;                  /* native kind */
    u8      nsub;                /* native sub kind (filters) */
    u8      has_ifaces;
    u8      flags;               /* filters: SFF_*; arrays: AOF_ARGUMENTS */
    PropMap props;
    Watch  *watch; int nwatch;
    struct AObj **ifaces; int nifaces;
    union {
        struct { struct AObj *self; int depth; } sup;
        int     b;
        double  n;
        AStr   *s;
        struct AFunc *fn;
        MovieClip *clip;         /* movie clip / button / text field instance */
        void   *p;               /* other natives: payload owned by the object */
    } nat;
    void  (*nfree)(struct AObj *o);   /* payload destructor */
    void  (*nmark)(struct VM *vm, struct AObj *o); /* payload GC marker */
    struct AObj *relay;          /* a movie clip made a Date, Boolean, Number or String by super(): that data */
} AObj;

#define A1_UNSET_MARK 0x5E7     /* Value.u.b of an undefined read from a guarded getter-setter */
#define AOF_ARGUMENTS 0x80     /* an activation's arguments array */
#define OBJ_IS_DISPLAY(o) ((o)->nk == NK_MOVIECLIP || (o)->nk == NK_BUTTON || (o)->nk == NK_EDITTEXT || (o)->nk == NK_VIDEO)

/* ---------------------------------------------------------------- */
/* functions                                                        */

typedef Value (*NativeFn)(struct Act *a, AObj *self, int argc, Value *argv);
typedef Value (*TableFn)(struct Act *a, AObj *self, int argc, Value *argv, int index);

enum { FK_NATIVE = 0, FK_TABLE, FK_ACTION };

enum {
    FF_PRELOAD_THIS = 0x01, FF_SUPPRESS_THIS = 0x02, FF_PRELOAD_ARGUMENTS = 0x04,
    FF_SUPPRESS_ARGUMENTS = 0x08, FF_PRELOAD_SUPER = 0x10, FF_SUPPRESS_SUPER = 0x20,
    FF_PRELOAD_ROOT = 0x40, FF_PRELOAD_PARENT = 0x80, FF_PRELOAD_GLOBAL = 0x100
};

typedef struct AParam { u8 reg; AStr *name; } AParam;

typedef struct AFunc {
    GCHdr    gc;
    u8       kind;
    NativeFn native;             /* FK_NATIVE: call; may be NULL (no-op) */
    NativeFn ctor;               /* native constructor (overrides `native` for new) */
    TableFn  tnative; int index; /* FK_TABLE */
    /* FK_ACTION */
    u8       swf_version;
    const u8 *base; u32 base_len;  /* buffer the code lives in (jumps may leave the body) */
    u32      start, len;         /* body = base[start, start + len) */
    AStr    *name;
    u8       nregs;
    u16      flags;
    int      nparams; AParam *params;
    struct Scope *scope;
    struct CPool *pool;
    struct ClipRef *base_clip;
} AFunc;

/* ---------------------------------------------------------------- */
/* scopes and constant pools                                        */

enum { SC_GLOBAL = 0, SC_TARGET, SC_LOCAL, SC_WITH };

typedef struct Scope {
    GCHdr  gc;
    struct Scope *parent;
    u8     cls;
    AObj  *values;
} Scope;

typedef struct CPool { GCHdr gc; int n; Value *v; } CPool;

/* ---------------------------------------------------------------- */
/* environments                                                     */

typedef struct Protos {
    AObj *object, *object_ctor, *function, *function_ctor, *array, *array_ctor,
         *string, *number, *boolean, *movieclip, *button, *textfield, *textformat,
         *sound, *color, *date, *date_ctor, *xmlnode, *xmlnode_ctor, *xml, *xml_ctor,
         *sharedobject, *sharedobject_ctor, *error, *bitmapdata, *matrix, *point,
         *rectangle, *colortransform, *transform, *loadvars, *stylesheet, *video,
         *bitmapfilter, *fileref, *mcl, *contextmenu, *contextmenuitem, *textsnapshot,
         *netconnection, *netstream, *localconnection, *xmlsocket, *camera, *microphone;
} Protos;

typedef struct Env {
    int    ready;
    AObj  *global;
    Scope *gscope;
    Protos p;
    PropMap registry;            /* Object.registerClass: symbol -> constructor (values) */
    struct AObj *lnx_native[24]; int nlnx;   /* Linux player 13+: classes "new" builds natively */
    struct AObj *lnx_xml;        /* ... and XML, whose objects show no constructor links at all */
} Env;

/* ---------------------------------------------------------------- */
/* execution                                                        */

enum { ER_FUNCTION = 0, ER_CONSTRUCTOR, ER_PROPERTY, ER_SPECIAL };

typedef struct ActId {
    const struct ActId *parent;
    u8   reason;
    u16  depth, function_count;
    u8   special_count;
    u32  property_id;
} ActId;

typedef struct Act {
    struct VM *vm;
    ActId   id;
    u8      swf_version;
    Scope  *scope;
    CPool  *pool;
    Value   this_;
    AObj   *callee;
    Value  *regs; int nregs;     /* local registers (nregs == 0: the 4 global ones) */
    MovieClip *base_clip;
    MovieClip *target_clip;      /* NULL: none (falls back to the root) */
    int     has_target;
} Act;

enum { ERR_NONE = 0, ERR_THROWN, ERR_FUNC_RECURSION, ERR_PROP_RECURSION, ERR_SPECIAL_RECURSION,
       ERR_PROTO_RECURSION, ERR_TIMEOUT, ERR_INVALID };

#define STACK_MAX 65536
#define MAX_TIMERS 256

typedef struct Timer {
    int    id; int active; int is_timeout;
    AObj  *fn; AObj *self; AStr *method;
    Value *args; int nargs;
    double interval, next;
} Timer;

typedef struct ClipEvent {
    u32 flags; int keycode;
    const u8 *code; u32 len;
} ClipEvent;

/* queued actions (Flash's action queue: initialize, construct, then the rest) */
enum { QA_NORMAL = 0, QA_INIT, QA_CONSTRUCT, QA_METHOD, QA_NOTIFY };
typedef struct QAction {
    MovieClip *clip;
    u8   kind, is_unload;
    const u8 *code; u32 len;               /* QA_NORMAL / QA_INIT */
    AObj *obj; AStr *name;                 /* QA_METHOD: obj.name(args); QA_NOTIFY: _global[name] listeners, method */
    AStr *method;
    AObj *ctor;                            /* QA_CONSTRUCT: registered class (NULL: none) */
    const u8 **ev_code; u32 *ev_len; int nev;   /* QA_CONSTRUCT: construct clip events */
    Value *args; int nargs;
} QAction;
typedef struct QList { QAction *a; int n, cap, head; } QList;

/* clip event flags (PlaceObject CLIPACTIONS, 32-bit form) */
enum {
    CE_LOAD = 0x1, CE_ENTER_FRAME = 0x2, CE_UNLOAD = 0x4, CE_MOUSE_MOVE = 0x8, CE_MOUSE_DOWN = 0x10,
    CE_MOUSE_UP = 0x20, CE_KEY_DOWN = 0x40, CE_KEY_UP = 0x80, CE_DATA = 0x100, CE_INITIALIZE = 0x200,
    CE_PRESS = 0x400, CE_RELEASE = 0x800, CE_RELEASE_OUTSIDE = 0x1000, CE_ROLL_OVER = 0x2000,
    CE_ROLL_OUT = 0x4000, CE_DRAG_OVER = 0x8000, CE_DRAG_OUT = 0x10000, CE_KEY_PRESS = 0x20000,
    CE_CONSTRUCT = 0x40000
};

typedef struct VM {
    Player *player;
    GCHdr  *gc_list; size_t gc_count, gc_threshold;
    Env     env[2];              /* [0] case insensitive (SWF <= 6), [1] case sensitive */
    Value  *stack; int sp, cap;
    Value   gregs[4];
    CPool  *cpool;               /* the global constant pool (last ConstantPool action) */
    int     err; Value thrown;
    int     halted;
    int     table_ctor;          /* a table native (ASnative) runs as a constructor */
    int     max_recursion;
    u32     next_prop_id;
    /* interned strings */
    AStr  **intern; int nintern, capintern;
    AStr   *small_int[256];      /* "0".."255" */
    /* timers */
    Timer   timers[MAX_TIMERS]; int next_timer_id;
    double  time_ms;
    int     get_time_calls; u32 time_offset;
    /* random */
    u32     rng;
    /* state */
    int     has_mouse_listener;
    int     swf_version;         /* of the root movie */
    int     trace;
    long    ops;                 /* actions since the last timeout check */
    long    exec_start;          /* clock() when the current frame's scripts began (0: unset) */
    double  max_exec_secs;       /* script timeout (default 15 s, ScriptLimits) */
    /* AVM1 frame */
    MovieClip *exec_list;            /* newest clip first */
    MovieClip **mix; int nmix, capmix;   /* roots of AVM1 movies inside an AVM2 player (AVM1Movie), oldest first */
    QList   queue[3];                /* initialize, construct, default */
    int     instance_counter;
    void   *loads;                   /* pending loads (avm1_load.c) */
    PropMap so_map;
    void   *net;                     /* NetConnection / LocalConnection state (avm1_amf.c) */                  /* SharedObject.getLocal: full name -> object */
    /* mouse (avm1_input.c) */
    MovieClip *m_hovered, *m_pressed;
    double  mouse_tx, mouse_ty;      /* stage twips */
    int     mouse_down, mouse_in_stage, drag_updating;
    double  drag_last_x, drag_last_y;
    int     highlight;               /* focus highlight: 0 inactive, 1 hidden, 2 visible */
    int     frame_running;
    /* drag */
    MovieClip *drag; int drag_lock; int drag_constrained; double drag_rect[4]; double drag_off_x, drag_off_y;
    int     quality, stage_focus_rect, sound_buf_time;
    int     use_new_invalid_bounds;
    /* well-known strings */
    AStr *s_proto, *s_prototype, *s_constructor, *s___constructor__, *s_length, *s_this,
         *s_arguments, *s_callee, *s_caller, *s_super, *s_root, *s_parent, *s_global,
         *s_toString, *s_valueOf, *s___resolve, *s_undefined, *s_null, *s_empty, *s_true,
         *s_false, *s_NaN, *s_Infinity, *s_mInfinity, *s_object_Object, *s_type_Object,
         *s_type_Function, *s_onEnterFrame, *s_onLoad, *s_onUnload, *s_onData,
         *s_onKeyDown, *s_onKeyUp, *s_onMouseDown, *s_onMouseUp, *s_onMouseMove,
         *s_onMouseWheel, *s_onPress, *s_onRelease, *s_onReleaseOutside, *s_onRollOver,
         *s_onRollOut, *s_onDragOver, *s_onDragOut, *s_onSetFocus, *s_onKillFocus,
         *s_onChanged, *s_onScroller, *s_onResize, *s_onSoundComplete, *s_onConstruct,
         *s_onInitialize, *s_text, *s_htmlText, *s_name, *s_x, *s_y, *s_level0,
         *s_broadcastMessage, *s__listeners, *s_addListener, *s_removeListener,
         *s_message, *s_Object, *s_Array, *s_Function, *s_String, *s_Number, *s_Boolean;
} VM;

#define VMOF(p) ((VM*)(p)->avm)
#define CASE_SENSITIVE(a) ((a)->swf_version >= 7)
#define ENV(a) (&(a)->vm->env[CASE_SENSITIVE(a) ? 1 : 0])
#define PROTOS(a) (&env_get((a)->vm, (a)->swf_version)->p)

/* ---------------------------------------------------------------- */
/* avm1_str.c                                                       */

void   *gc_alloc(VM *vm, size_t sz, int kind);
AStr   *str_latin1(VM *vm, const u8 *s, size_t len);
AStr   *str_utf16(VM *vm, const u16 *s, size_t len);
AStr   *str_utf8(VM *vm, const char *s, size_t len);   /* decode UTF-8 (invalid bytes -> U+FFFD) */
AStr   *str_cstr(VM *vm, const char *s);               /* UTF-8, NUL terminated */
AStr   *str_from_swf(VM *vm, const char *s, size_t len, int swf_version);  /* SWF 6+: UTF-8; older: Windows-1252 */
AStr   *str_intern(VM *vm, const char *s);              /* ASCII / UTF-8 literal */
AStr   *str_intern_str(VM *vm, AStr *s);
const char *str_c(VM *vm, AStr *s);                     /* UTF-8 view (cached) */
AStr   *str_concat(VM *vm, AStr *a, AStr *b);
AStr   *str_slice(VM *vm, AStr *s, u32 start, u32 end);
AStr   *str_from_units(VM *vm, const u16 *u, size_t n); /* picks Latin-1 when possible */
int     str_eq(const AStr *a, const AStr *b);
int     str_eq_ci(const AStr *a, const AStr *b);
int     str_eq_case(const AStr *a, const AStr *b, int case_sensitive);
int     str_eq_c(const AStr *a, const char *ascii);
int     str_eq_c_ci(const AStr *a, const char *ascii);
int     str_cmp(const AStr *a, const AStr *b);          /* code unit order */
long    str_find(const AStr *s, const AStr *pat, u32 from);
long    str_rfind(const AStr *s, const AStr *pat, long from);
long    str_find_unit(const AStr *s, u16 c, u32 from);
long    str_rfind_any(const AStr *s, const char *units);  /* last index of any ASCII unit */
long    str_find_any(const AStr *s, const char *units, u32 from);
int     str_starts_c(const AStr *s, const char *ascii);
int     str_starts_c_ci(const AStr *s, const char *ascii);
u16    *str_units(VM *vm, AStr *s, u16 *tmp, size_t tmpcap);  /* temporary UTF-16 copy (tmp or malloc) */
AStr   *str_lower(VM *vm, AStr *s);
AStr   *str_upper(VM *vm, AStr *s);
u16     swf_lower(u16 c);
u16     swf_upper(u16 c);
AStr   *str_num(VM *vm, double d);                      /* AVM1 number formatting */
AStr   *str_int(VM *vm, s32 i);
double  str_to_num(AStr *s, int swf_version);
double  str_parse_float(AStr *s, int strict);
int     str_to_index(const AStr *s, s32 *out);           /* canonical array index */

/* string builder */
typedef struct SBuf { u16 *u; size_t n, cap; } SBuf;
void    sb_init(SBuf *b);
void    sb_unit(SBuf *b, u16 c);
void    sb_str(SBuf *b, const AStr *s);
void    sb_ascii(SBuf *b, const char *s);
void    sb_utf8(SBuf *b, const char *s);
AStr   *sb_done(VM *vm, SBuf *b);
void    sb_free(SBuf *b);

/* ---------------------------------------------------------------- */
/* avm1_value.c                                                     */

Value   v_undef(void);
Value   v_null(void);
Value   v_bool(int b);
Value   v_num(double n);
Value   v_str(AStr *s);
Value   v_obj(AObj *o);
Value   v_cstr(VM *vm, const char *s);
Value   arg(int argc, Value *argv, int i);

int     v_is_prim(Value v);
int     v_same(Value a, Value b);                       /* Value PartialEq (NaN == NaN) */
double  v_num_of(Act *a, Value v);                      /* coerce_to_f64 */
s32     v_i32(Act *a, Value v);
u32     v_u32(Act *a, Value v);
u16     v_u16(Act *a, Value v);
s16     v_i16(Act *a, Value v);
u8      v_u8(Act *a, Value v);
AStr   *v_str_of(Act *a, Value v);                      /* coerce_to_string */
int     v_bool_of(Act *a, Value v);                     /* as_bool */
int     v_as_bool(Value v, int swf_version);
Value   v_prim(Act *a, Value v);                        /* to_primitive */
Value   v_prim_num(Act *a, Value v);                    /* to_primitive_num */
double  v_prim_as_num(Act *a, Value v);
AObj   *v_obj_of(Act *a, Value v);                      /* as_object (NULL for primitives) */
AObj   *v_to_obj(Act *a, Value v);                      /* coerce_to_object (boxes primitives) */
AObj   *v_to_obj_or_bare(Act *a, Value v);
AStr   *v_typeof(Act *a, Value v);
Value   v_lt(Act *a, Value x, Value y);                 /* abstract_lt: bool or undefined */
int     v_eq(Act *a, Value x, Value y);                 /* abstract_eq */
int     v_instanceof(Act *a, Value v, Value cls);
double  d_wrap32(double d);
s32     f64_i32(double d);
u32     f64_u32(double d);

/* ---------------------------------------------------------------- */
/* avm1_obj.c                                                       */

AObj   *obj_new(VM *vm, AObj *proto);                   /* __proto__ = proto (when non-NULL) */
AObj   *obj_new_v(VM *vm, Value proto);
AObj   *obj_bare(VM *vm);                               /* no __proto__ */
AObj   *obj_array(Act *a, int n, Value *items);
void    obj_free_payload(AObj *o);
AObj   *o_native_retarget(AObj *o, int nk);   /* the object a native constructor may (re)type, or NULL */
AObj   *o_native_relay(Act *a, AObj *o, int nk);  /* the same, or a movie clip's separate relay object */
AObj   *o_native_target(AObj *o);                       /* set_native target, NULL when already native */

/* raw property map */
Prop   *pm_get(PropMap *m, const AStr *key, int case_sensitive);
Prop   *pm_insert(VM *vm, PropMap *m, AStr *key, int case_sensitive);   /* existing or new (data undefined) */
int     pm_remove(PropMap *m, const AStr *key, int case_sensitive);
void    pm_free(PropMap *m);

/* object model (Ruffle semantics) */
Value   o_get(Act *a, AObj *o, AStr *name);
int     o_get_opt(Act *a, AObj *o, AStr *name, int call_resolve, Value *out);
Value   o_get_stored(Act *a, AObj *o, AStr *name);
void    o_set(Act *a, AObj *o, AStr *name, Value v);
int     o_get_local_stored(Act *a, AObj *o, AStr *name, Value *out);
/* Ruffle's resolve_class / resolve_prototype / instantiate_class_as_script on a
 * dotted path below _global ("flash.geom.Rectangle"): the first two read data
 * properties only (no getters, prototype chain or version attributes) */
AObj   *a1_resolve_class(Act *a, const char *path);
int     a1_resolve_proto(Act *a, const char *path, Value *out);
int     a1_class_proto(Act *a, AObj *cls, Value *out);   /* cls.prototype, data only */
void    avm1_warning(VM *vm, const char *msg);   /* a debug player warning (limited to 100) */
int     avm1_player_major(void);   /* the major version of the emulated player ($version) */
int     avm1_ruffle_shapes(void);  /* class shapes as Ruffle declares them (FLASHNT_AVM1_RUFFLE_SHAPES) */
/* behaviour that differs between Flash Player releases; the releases are the ones the
   Gnash testsuite expectations were recorded with (fp13, fp16-18, fp20, fp21, fp24+ are
   later builds than fp14 and fp19, which still behave like Flash 9-12 in these points) */
int     avm1_fp_sealed_arguments(void);
int     avm1_fp_box_needs_class(void);    /* new Object(primitive) is undefined without its class */   /* ASSetPropFlags cannot unhide arguments' properties */
AObj   *a1_new_with_proto(Act *a, const char *path);      /* Object::new(resolve_prototype(path)) */
Value   a1_instantiate_as_script(Act *a, const char *path, int argc, Value *argv);
void    o_set_local(Act *a, AObj *o, AStr *name, Value v, AObj *self);
int     o_get_data(Act *a, AObj *o, AStr *name, Value *out);  /* raw, own */
void    o_set_data(Act *a, AObj *o, AStr *name, Value v);
void    o_define(VM *vm, AObj *o, AStr *name, Value v, u16 attr);       /* case sensitive insert */
void    o_define_c(VM *vm, AObj *o, const char *name, Value v, u16 attr);
void    o_add_property(VM *vm, AObj *o, AStr *name, AObj *getter, AObj *setter, u16 attr);
void    o_add_property_case(Act *a, AObj *o, AStr *name, AObj *getter, AObj *setter, u16 attr);
int     o_delete(Act *a, AObj *o, AStr *name);
int     o_has_property(Act *a, AObj *o, AStr *name);
int     o_has_own_property(Act *a, AObj *o, AStr *name);
int     o_has_own_virtual(Act *a, AObj *o, AStr *name);
int     o_is_enumerable(Act *a, AObj *o, AStr *name);
Value   o_proto(Act *a, AObj *o);
int     o_prototype(Act *a, AObj *o, Value *out);       /* own "prototype" data */
AStr  **o_keys(Act *a, AObj *o, int include_hidden, int *n);   /* malloc'd array */
void    o_set_attributes(VM *vm, AObj *o, AStr *name, u16 set, u16 clear);
int     o_watch(Act *a, AObj *o, AStr *name, AObj *cb, Value user);
int     o_unwatch(Act *a, AObj *o, AStr *name);
void    o_set_interfaces(VM *vm, AObj *o, AObj **list, int n);
int     o_is_instance_of(Act *a, AObj *o, AObj *proto);
int     o_is_prototype_of(Act *a, AObj *o, AObj *other);
Value   o_call(Act *a, AObj *f, Value self, int argc, Value *argv);     /* Object::call */
Value   o_call_method(Act *a, AObj *o, AStr *name, int argc, Value *argv, int reason);
Value   o_construct(Act *a, AObj *ctor, int argc, Value *argv);
void    o_construct_on_existing(Act *a, AObj *ctor, AObj *self, int argc, Value *argv);
int     o_search_proto(Act *a, Value proto, AStr *name, AObj *self, int call_resolve, Value *out, int *depth);
AObj   *o_as_function(AObj *o);                         /* o itself when it is callable */
MovieClip *o_display(AObj *o);                          /* display object of a stage object (NULL: none / removed) */
MovieClip *o_display_no_super(AObj *o);
AObj   *o_super_self(AObj *o);
int     o_length(Act *a, AObj *o);                      /* array-like "length" */
void    o_set_length(Act *a, AObj *o, int n);
Value   o_get_element(Act *a, AObj *o, int i);
void    o_set_element(Act *a, AObj *o, int i, Value v);
void    o_delete_element(Act *a, AObj *o, int i);
int     o_has_element(Act *a, AObj *o, int i);

/* functions */
AObj   *fn_native(VM *vm, Env *env, NativeFn f, NativeFn ctor);       /* Function.prototype-based, no prototype object */
AObj   *fn_table(VM *vm, Env *env, TableFn f, int index);
AObj   *fn_class(VM *vm, Env *env, NativeFn ctor, NativeFn call, AObj *proto, int proto_first);
Value   fn_exec(Act *a, AObj *callee, Value self, int depth, int argc, Value *argv, int reason, u32 prop_id);
Value   fn_exec_ctor(Act *a, AObj *callee, Value self, int depth, int argc, Value *argv, int reason);
AObj   *obj_super(VM *vm, AObj *self, int depth);

/* ---------------------------------------------------------------- */
/* declarations (property_decl)                                     */

enum { D_END = 0, D_METHOD, D_PROPERTY, D_VALUE_NUM, D_VALUE_STR, D_VALUE_BOOL, D_VALUE_NULL, D_VALUE_UNDEF, D_TABLE, D_TABLE_PROPERTY };
typedef struct Decl {
    u8 kind;
    const char *name;
    NativeFn fn, set;           /* method / getter, setter */
    double num; const char *str;
    u16 attr;
    int index, set_index;        /* table natives */
} Decl;
#define DECL_METHOD(n, f, attr) { D_METHOD, n, f, NULL, 0, NULL, attr, 0, 0 }
#define DECL_GETSET(n, g, s, attr) { D_PROPERTY, n, g, s, 0, NULL, attr, 0, 0 }
#define DECL_NUM(n, v, attr) { D_VALUE_NUM, n, NULL, NULL, v, NULL, attr, 0, 0 }
#define DECL_STR(n, v, attr) { D_VALUE_STR, n, NULL, NULL, 0, v, attr, 0, 0 }
#define DECL_BOOL(n, v, attr) { D_VALUE_BOOL, n, NULL, NULL, v, NULL, attr, 0, 0 }
#define DECL_NULL(n, attr) { D_VALUE_NULL, n, NULL, NULL, 0, NULL, attr, 0, 0 }
#define DECL_END { D_END, NULL, NULL, NULL, 0, NULL, 0, 0, 0 }
void    decl_apply(VM *vm, Env *env, AObj *o, const Decl *d);
typedef struct TDecl { const char *name; int index, set_index; u16 attr; } TDecl;  /* set_index < 0: method; else getter=index setter=set_index (-2: no setter) */
void    decl_table(VM *vm, Env *env, AObj *o, TableFn f, const TDecl *d);

/* ---------------------------------------------------------------- */
/* avm1_interp.c                                                    */

Env    *env_get(VM *vm, int swf_version);
void    vm_push(VM *vm, Value v);
Value   vm_pop(VM *vm);
void    act_init_nothing(Act *a, VM *vm, MovieClip *base_clip, const char *name);  /* global scope, this = global */
void    act_init_clip(Act *a, VM *vm, MovieClip *clip);   /* target scope of `clip` */
int     act_case_sensitive(Act *a);
AObj   *act_global(Act *a);
void    act_throw(Act *a, Value v);
Value   act_run(Act *a, const u8 *base, u32 base_len, u32 start, u32 len, int *returned);
struct SWFMovie;
Value   act_run_code(Act *a, const struct SWFMovie *m, const u8 *code, u32 len, int *returned);
Value   act_get_variable(Act *a, AStr *path, AObj **owner);
void    act_set_variable(Act *a, AStr *path, Value v);
AObj   *act_resolve_target_path(Act *a, MovieClip *root, AObj *start, AStr *path, int first_element, int handle_this);
MovieClip *act_resolve_target_display(Act *a, MovieClip *start, Value target, int allow_empty);
MovieClip *act_target_or_root(Act *a);
MovieClip *act_target_or_base(Act *a);
MovieClip *act_get_level(Act *a, int level);
MovieClip *act_get_or_create_level(Act *a, int level);
Value   act_resolve(Act *a, AStr *name, AObj **owner);
Value   vm_call_value(Act *a, Value fn, Value self, int argc, Value *argv);
void    vm_report_error(Act *a);                        /* uncaught: trace a warning, halt on engine errors */
int     act_this_obj(Act *a, AObj **out);
Value   act_super_for(Act *a);

/* trace output (shared with AVM2) */
void    avm1_trace_out(const char *s);
void    avm1_trace_str(VM *vm, AStr *s);

/* ---------------------------------------------------------------- */
/* stage objects (avm1_display.c)                                   */

struct TextFieldState;

typedef struct TFBinding { MovieClip *tf; AStr *var; } TFBinding;

/* AVM1 state of a MovieClip / Button / TextField instance */
typedef struct A1Clip {
    MovieClip *next_exec;          /* execution list (newest clip first) */
    u8   in_exec, initialized, removed, pending_removal, post_inst;
    u8   script_placed, explicit_name, lock_root, kind;   /* kind: 0 clip, 1 button, 2 text */
    u8   enabled_cache, use_hand_cursor;
    u8   is_root;                  /* main timeline of a movie (root, level, loaded clip) */
    u8   mix_wait;                 /* AVM1Movie root created during this AVM2 frame: its first AVM1 frame is the next one */
    u8   has_fl; int fl;           /* _framesloaded override (load states) */
    ClipEvent *events; u32 event_flags; int nevents;
    void *drawing;                 /* drawing API (ShapeDef) */
    int  draw_fill, draw_line, draw_open, draw_has;
    s32  draw_x, draw_y, draw_start_x, draw_start_y;
    MovieClip *masker, *maskee;    /* setMask */
    int  focus_rect;               /* -1 null, 0 false, 1 true */
    struct TextFieldState *tfs;    /* text fields */
    AStr *tf_var;                  /* text field variable name */
    MovieClip *tf_bound;           /* clip holding the bound variable */
    TFBinding *bindings; int nbindings;   /* text fields bound to this clip's variables */
    int  btn_state;                /* buttons: 0 up 1 over 2 down */
    u8   btn_inited;               /* buttons: state children exist */
    DisplayObject *btn_hit;        /* buttons: hit area objects (not rendered) */
    struct AObj *drop_target;      /* _droptarget while dragged */
    void *filters;                 /* filter list (avm1_filters.c) */
    int  tab_index; u8 has_tab;    /* tabIndex (when has_tab) */
    int  st[5]; u8 has_st;
    struct AObj *style_sheet;      /* text fields: TextField.styleSheet */         /* sound transform (volume, ll, lr, rl, rr) when has_st */
    double drop_target_dummy;
    AStr *orig_path;               /* the path when its script object was made (references fall back to it) */
} A1Clip;

A1Clip *a1_of(MovieClip *c);
MovieClip *avm1_base_root(VM *vm);   /* _level0, or the newest AVM1Movie root inside an AVM2 player */
int     avm1_stage_roots(VM *vm, MovieClip **out, int cap);   /* the AVM1 levels for input, lowest first */

/* Flash's matrix arithmetic (dobj_geom.c) */
typedef struct RMat { float a, b, c, d; s32 tx, ty; } RMat;
void    rm_from(const SWFMatrix *m, RMat *o);
void    rm_identity(RMat *o);
void    rm_mul(const RMat *x, const RMat *y, RMat *o);
int     rm_inverse(const RMat *m, RMat *o);
void    rm_apply(const RMat *m, s32 x, s32 y, s32 *ox, s32 *oy);
void    rm_local_to_global(MovieClip *c, RMat *o);
s32     tw_from_px(double px);                            /* allocate on demand */

AObj   *clip_obj(VM *vm, MovieClip *c);                 /* the AS object of a clip (created on demand) */
AObj   *clip_obj_or_undef_v(VM *vm, MovieClip *c, Value *out);
MovieClip *obj_clip(AObj *o);                           /* live display object (NULL if removed) */
int     clip_removed(MovieClip *c);
MovieClip *clip_avm1_root(MovieClip *c);
MovieClip *clip_avm1_parent(MovieClip *c);
AStr   *clip_path(VM *vm, MovieClip *c);                /* _level0.a.b */
AStr   *clip_slash_path(VM *vm, MovieClip *c);          /* /a/b */
AStr   *clip_get_name(VM *vm, MovieClip *c);
MovieClip *clip_child_by_name(Act *a, MovieClip *c, AStr *name);
int     clip_is_movieclip(MovieClip *c);
int     clip_swf_version(MovieClip *c);
int     stage_get_property(Act *a, MovieClip *d, AStr *name, Value *out);
int     stage_set_property(Act *a, MovieClip *d, AStr *name, Value v);   /* display property: 1 if handled */
int     stage_has_property(Act *a, MovieClip *d, AStr *name);
void    stage_enumerate(Act *a, MovieClip *d, AStr ***keys, int *n, int *cap);
void    stage_notify_change(Act *a, MovieClip *d, AStr *name, Value v);
int     stage_resolve_path_property(Act *a, MovieClip *d, AStr *name, Value *out);
int     stage_prop_index_get(Act *a, MovieClip *d, int index, Value *out);
int     stage_prop_index_set(Act *a, MovieClip *d, int index, Value v);
Value   stage_prop_coerce(Act *a, int index, Value v);
int     stage_prop_read_only(int index);
Value   clipref_value(Act *a, AObj *o);                 /* V_CLIP for movie clips (V_OBJ otherwise) */
AObj   *clipref_resolve(Act *a, ClipRef *r, int *cached, MovieClip **dobj);
AStr   *clipref_string(Act *a, ClipRef *r);
ClipRef *clipref_from_path(VM *vm, AStr *path);
void    clip_goto_value(Act *a, MovieClip *c, Value frame, int play, int scene_offset);
void    clip_remove_display(Act *a, MovieClip *c);
void    clip_clone_sprite(Act *a, MovieClip *src, AStr *name, s32 depth, AObj *init);
void    clip_start_drag(Act *a, MovieClip *c, int lock_center, double *bounds);
int     clip_frame_label(MovieClip *c, AStr *label, int case_sensitive);

/* ---------------------------------------------------------------- */
/* library entry points                                             */

void    lib_create_globals(VM *vm, Env *env);
void    lib_object(VM *vm, Env *env);
void    lib_function(VM *vm, Env *env);
void    lib_globals_post(VM *vm, Env *env);
AObj   *lib_array(VM *vm, Env *env);
AObj   *lib_string(VM *vm, Env *env);
AObj   *lib_number(VM *vm, Env *env);
AObj   *lib_boolean(VM *vm, Env *env);
AObj   *lib_math(VM *vm, Env *env);
AObj   *lib_date(VM *vm, Env *env);

/* helpers used across the library */
AObj   *this_or(AObj *self);
Value   broadcast_message(Act *a, AObj *broadcaster, AStr *method, int argc, Value *argv, int *had_listener);
void    asbroadcaster_initialize(Act *a, AObj *o);
Value   array_join(Act *a, AObj *o, AStr *sep);
AStr   *escape_str(VM *vm, AStr *s);
Value   parse_int_value(Act *a, Value s, Value *radix);

/* ---------------------------------------------------------------- */
/* GC                                                               */

void    gc_collect(VM *vm);
void    gc_mark_value(VM *vm, Value v);
void    gc_mark_obj(VM *vm, AObj *o);
void    gc_mark_str(AStr *s);
void    gc_mark_hdr(VM *vm, GCHdr *h);

#endif
