/* avm1_classes.c - AVM1 globals around the stage and the system: Key,
 * Mouse, Stage, Selection, System (capabilities, security, IME),
 * Accessibility, Color, Sound, and the classes FLASHNT only declares
 * (their objects exist so that scripts can test for them). */
#include "avm1_int.h"
#include "audio.h"
#include "textfield.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define DE  PA_DONT_ENUM
#define DD  PA_DONT_DELETE
#define RO  PA_READ_ONLY
#define V6  PA_VERSION_6

extern AObj *lib_class(VM *vm, Env *env, NativeFn ctor, NativeFn call, AObj *super_proto, int proto_first, AObj **proto_out);
extern int avm1_last_key_code, avm1_last_key_ascii;

static void defm(VM *vm, Env *env, AObj *o, const char *name, NativeFn f, u16 attr) { o_define(vm, o, str_intern(vm, name), v_obj(fn_native(vm, env, f, NULL)), attr); }
static void defp(VM *vm, Env *env, AObj *o, const char *name, NativeFn g, NativeFn s, u16 attr) { o_add_property(vm, o, str_intern(vm, name), fn_native(vm, env, g, NULL), s ? fn_native(vm, env, s, NULL) : NULL, attr); }
static void defs(VM *vm, AObj *o, const char *name, const char *v, u16 attr) { o_define(vm, o, str_intern(vm, name), v_str(str_intern(vm, v)), attr); }
static void defn(VM *vm, AObj *o, const char *name, double v, u16 attr) { o_define(vm, o, str_intern(vm, name), v_num(v), attr); }

static Value noop(Act *a, AObj *s, int c, Value *v) { (void)a; (void)s; (void)c; (void)v; return v_undef(); }
static Value sec_allow_domain(Act *a, AObj *s, int c, Value *v) { (void)a; (void)s; (void)v; return v_bool(c > 0); }
static Value ret_false(Act *a, AObj *s, int c, Value *v) { (void)a; (void)s; (void)c; (void)v; return v_bool(0); }
static Value ret_true(Act *a, AObj *s, int c, Value *v) { (void)a; (void)s; (void)c; (void)v; return v_bool(1); }

/* a class whose constructor does nothing (DeclContext::empty_class) */
AObj *lib_empty_class(VM *vm, Env *env, AObj *object_proto) { return lib_class(vm, env, NULL, NULL, object_proto, 1, NULL); }

/* System.Product(name) */
static Value product_ctor(Act *a, AObj *self, int argc, Value *argv) {
    AObj *t = o_native_target(self);
    AStr *k = str_intern(a->vm, "name"); Prop *p = t ? pm_get(&t->props, k, 1) : NULL;
    if (p && (p->attr & PA_READ_ONLY)) return v_undef();   /* a second call cannot change it */
    if (t) o_define(a->vm, t, k, argc > 0 ? argv[0] : v_undef(), PA_DONT_DELETE | PA_READ_ONLY | PA_DONT_ENUM);
    return v_undef();
}

/* ---------------------------------------------------------------- */
/* Key                                                              */

static Value key_is_down(Act *a, AObj *self, int argc, Value *argv) {
    s32 k = v_i32(a, arg(argc, argv, 0)); (void)self;
    if (a->vm->err) return v_undef();
    return v_bool(k >= 0 && k < 256 && a->vm->player->key_down[k]);
}
static Value key_is_toggled(Act *a, AObj *self, int argc, Value *argv) {
    s32 k = v_i32(a, arg(argc, argv, 0)); (void)self;
    if (a->vm->err) return v_undef();
    return v_bool(k >= 0 && k < 256 && a->vm->player->key_toggled[k]);
}
static Value key_get_ascii(Act *a, AObj *self, int argc, Value *argv) { (void)a; (void)self; (void)argc; (void)argv; return v_num(avm1_last_key_ascii); }
static Value key_get_code(Act *a, AObj *self, int argc, Value *argv) { (void)a; (void)self; (void)argc; (void)argv; return v_num(avm1_last_key_code); }

AObj *lib_key(VM *vm, Env *env) {
    static const struct { const char *n; int v; } k[] = {
        { "CAPSLOCK", 20 }, { "BACKSPACE", 8 }, { "DELETEKEY", 46 }, { "INSERT", 45 }, { "ESCAPE", 27 },
        { "SHIFT", 16 }, { "CONTROL", 17 }, { "TAB", 9 }, { "END", 35 }, { "HOME", 36 }, { "PGDN", 34 },
        { "PGUP", 33 }, { "RIGHT", 39 }, { "LEFT", 37 }, { "DOWN", 40 }, { "UP", 38 }, { "SPACE", 32 },
        { "ENTER", 13 }, { "ALT", 18 }, { NULL, 0 } };
    AObj *o = obj_new(vm, env->p.object); int i; u16 at = DE | DD | RO;
    for (i = 0; k[i].n; i++) defn(vm, o, k[i].n, k[i].v, at);
    defm(vm, env, o, "getAscii", key_get_ascii, at);
    defm(vm, env, o, "getCode", key_get_code, at);
    defm(vm, env, o, "isDown", key_is_down, at);
    defm(vm, env, o, "isToggled", key_is_toggled, at);
    return o;
}

/* ---------------------------------------------------------------- */
/* Mouse                                                            */

static int g_mouse_visible = 1;
static Value mouse_show(Act *a, AObj *s, int c, Value *v) { int was = g_mouse_visible; (void)a; (void)s; (void)c; (void)v; g_mouse_visible = 1; return v_num(was ? 0 : 1); }
static Value mouse_hide(Act *a, AObj *s, int c, Value *v) { int was = g_mouse_visible; (void)a; (void)s; (void)c; (void)v; g_mouse_visible = 0; return v_num(was ? 0 : 1); }

AObj *lib_mouse(VM *vm, Env *env) {
    AObj *o = obj_new(vm, env->p.object); u16 at = DD | DE | RO;
    defm(vm, env, o, "show", mouse_show, at);
    defm(vm, env, o, "hide", mouse_hide, at);
    defm(vm, env, o, "setTrailer", noop, at);
    defm(vm, env, o, "setTrailerPosition", noop, at);
    defm(vm, env, o, "setTrailerMode", noop, at);
    return o;
}

/* ---------------------------------------------------------------- */
/* Stage                                                            */

static int g_stage_align;            /* bits: 1 L, 2 T, 4 R, 8 B */
static int g_stage_scale_mode;       /* 0 showAll 1 exactFit 2 noBorder 3 noScale */

/* Stage::build_matrices: stage size and the letterbox matrix (stage px -> device px) */
static SWFMovie *stage_movie(Player *p);
static void view_of(Player *p, double *vw, double *vh, double *sf) {
    SWFMovie *m = stage_movie(p);
    if (p->has_view) { *vw = p->view_w; *vh = p->view_h; *sf = p->view_sf > 0 ? p->view_sf : 1; }
    else { *vw = floor((m->stage.xmax - m->stage.xmin) / 20.0) * (p->zoom > 0 ? p->zoom : 1); *vh = floor((m->stage.ymax - m->stage.ymin) / 20.0) * (p->zoom > 0 ? p->zoom : 1); *sf = 1; }
}
void avm1_stage_size(Player *p, int *w, int *h) {
    SWFMovie *m = stage_movie(p);
    if (g_stage_scale_mode == 3) { double vw, vh, sf; view_of(p, &vw, &vh, &sf); *w = (int)floor(vw / sf + 0.5); *h = (int)floor(vh / sf + 0.5); }
    else { *w = (int)floor((m->stage.xmax - m->stage.xmin) / 20.0); *h = (int)floor((m->stage.ymax - m->stage.ymin) / 20.0); }
}
void avm1_view_matrix(Player *p, double *sx, double *sy, double *tx, double *ty) {
    SWFMovie *m = stage_movie(p); double vw, vh, sf, mw, mh, ma, va, wd, hd;
    view_of(p, &vw, &vh, &sf);
    if (!p->has_view) { *sx = *sy = p->zoom > 0 ? p->zoom : 1; *tx = *ty = 0; return; }
    mw = floor((m->stage.xmax - m->stage.xmin) / 20.0); mh = floor((m->stage.ymax - m->stage.ymin) / 20.0);
    if (mw <= 0) mw = 1; if (mh <= 0) mh = 1;
    ma = mw / mh; va = vw / vh;
    switch (g_stage_scale_mode) {
    case 1: *sx = vw / mw; *sy = vh / mh; break;
    case 2: *sx = *sy = va < ma ? vh / mh : vw / mw; break;
    case 3: *sx = *sy = sf; break;
    default: *sx = *sy = va > ma ? vh / mh : vw / mw; break;
    }
    wd = vw - mw * *sx; hd = vh - mh * *sy;
    *tx = (g_stage_align & 1) ? 0 : (g_stage_align & 4) ? wd : wd / 2;
    *ty = (g_stage_align & 2) ? 0 : (g_stage_align & 8) ? hd : hd / 2;
}
void avm1_stage_view_mode(int *mode, int *align) { *mode = g_stage_scale_mode; *align = g_stage_align; }
static int g_last_sw = -1, g_last_sh = -1;
/* the stage size may have changed: Stage.onResize, right away */
void avm1_stage_resized(VM *vm) {
    int w, h;
    avm1_stage_size(vm->player, &w, &h);
    if (g_last_sw < 0) { g_last_sw = w; g_last_sh = h; return; }
    if (w == g_last_sw && h == g_last_sh) return;
    g_last_sw = w; g_last_sh = h;
    {
        Act a; AObj *st; int had = 0;
        act_init_nothing(&a, vm, vm->player->root, "[Stage onResize]");
        st = v_obj_of(&a, o_get(&a, act_global(&a), str_intern(vm, "Stage")));
        if (!vm->err && st) broadcast_message(&a, st, vm->s_onResize, 0, NULL, &had);
        if (vm->err) vm_report_error(&a);
    }
}
static int g_stage_show_menu = 1;

static Value st_align(Act *a, AObj *s, int c, Value *v) {
    char b[5]; int n = 0; (void)s; (void)c; (void)v;
    if (g_stage_align & 1) b[n++] = 'L';
    if (g_stage_align & 2) b[n++] = 'T';
    if (g_stage_align & 4) b[n++] = 'R';
    if (g_stage_align & 8) b[n++] = 'B';
    b[n] = 0;
    return v_str(str_cstr(a->vm, b));
}
static Value st_set_align(Act *a, AObj *s, int argc, Value *argv) {
    AStr *x = v_str_of(a, arg(argc, argv, 0)); u32 i; int f = 0; (void)s;
    if (a->vm->err) return v_undef();
    for (i = 0; i < x->len; i++) {
        u16 c = swf_upper(STR_AT(x, i));
        if (c == 'L') f |= 1; else if (c == 'T') f |= 2; else if (c == 'R') f |= 4; else if (c == 'B') f |= 8;
    }
    g_stage_align = f;
    avm1_stage_resized(a->vm);
    return v_undef();
}
static SWFMovie *stage_movie(Player *p) { return p->root && p->root->movie && p->root->movie->body ? p->root->movie : p->movie; }
static Value st_width(Act *a, AObj *s, int c, Value *v) { int w, h; (void)s; (void)c; (void)v; avm1_stage_size(a->vm->player, &w, &h); return v_num(w); }
static Value st_height(Act *a, AObj *s, int c, Value *v) { int w, h; (void)s; (void)c; (void)v; avm1_stage_size(a->vm->player, &w, &h); return v_num(h); }
static const char *scale_names[4] = { "showAll", "exactFit", "noBorder", "noScale" };
static Value st_scale_mode(Act *a, AObj *s, int c, Value *v) { (void)s; (void)c; (void)v; return v_str(str_intern(a->vm, scale_names[g_stage_scale_mode])); }
static Value st_set_scale_mode(Act *a, AObj *s, int argc, Value *argv) {
    AStr *x = v_str_of(a, arg(argc, argv, 0)); int i; (void)s;
    if (a->vm->err) return v_undef();
    g_stage_scale_mode = 0;
    for (i = 0; i < 4; i++) if (str_eq_c_ci(x, scale_names[i])) g_stage_scale_mode = i;
    avm1_stage_resized(a->vm);
    return v_undef();
}
static Value st_show_menu(Act *a, AObj *s, int c, Value *v) { (void)a; (void)s; (void)c; (void)v; return v_bool(g_stage_show_menu); }
static Value st_set_show_menu(Act *a, AObj *s, int argc, Value *argv) { (void)s; g_stage_show_menu = v_as_bool(arg(argc, argv, 0), a->swf_version); return v_undef(); }
static Value st_display_state(Act *a, AObj *s, int c, Value *v) { (void)s; (void)c; (void)v; return v_str(str_intern(a->vm, a->vm->player->fullscreen ? "fullScreen" : "normal")); }
/* Stage.displayState = "fullScreen" | "normal" (any case): Stage.onFullScreen(bFull) right away */
static Value st_set_display_state(Act *a, AObj *s, int argc, Value *argv) {
    VM *vm = a->vm; Player *p = vm->player; AStr *x; int want;
    extern int plat_set_fullscreen(Player *p, int on);
    (void)s;
    x = v_str_of(a, arg(argc, argv, 0)); if (vm->err) return v_undef();
    if (str_eq_c_ci(x, "fullscreen")) want = 1; else if (str_eq_c_ci(x, "normal")) want = 0; else return v_undef();
    if (want == p->fullscreen) return v_undef();
    if (!plat_set_fullscreen(p, want)) return v_undef();
    p->fullscreen = want;
    {
        Act b; AObj *st; int had = 0; Value arg0 = v_bool(want);
        act_init_nothing(&b, vm, p->root, "[Stage onFullScreen]");
        st = v_obj_of(&b, o_get(&b, act_global(&b), str_intern(vm, "Stage")));
        if (!vm->err && st) broadcast_message(&b, st, str_intern(vm, "onFullScreen"), 1, &arg0, &had);
        if (vm->err) vm_report_error(&b);
    }
    return v_undef();
}

/* ASnative(666, n): the Stage accessors (odd: getter, even: setter) */
Value stage_native(Act *a, AObj *self, int argc, Value *argv, int index) {
    switch (index) {
    case 1: return st_scale_mode(a, self, argc, argv);
    case 2: return st_set_scale_mode(a, self, argc, argv);
    case 3: return st_align(a, self, argc, argv);
    case 4: return st_set_align(a, self, argc, argv);
    case 5: return st_width(a, self, argc, argv);
    case 7: return st_height(a, self, argc, argv);
    case 9: return st_show_menu(a, self, argc, argv);
    case 10: return st_set_show_menu(a, self, argc, argv);
    default: return v_undef();
    }
}

AObj *lib_stage(VM *vm, Env *env) {
    AObj *o = obj_new(vm, env->p.object);
    defp(vm, env, o, "align", st_align, st_set_align, 0);
    defp(vm, env, o, "scaleMode", st_scale_mode, st_set_scale_mode, 0);
    defp(vm, env, o, "height", st_height, NULL, 0);
    defp(vm, env, o, "width", st_width, NULL, 0);
    defp(vm, env, o, "showMenu", st_show_menu, st_set_show_menu, 0);
    defp(vm, env, o, "displayState", st_display_state, st_set_display_state, 0);
    return o;
}

/* ---------------------------------------------------------------- */
/* Selection                                                        */

static Value sel_focus_tfs(Act *a, TextFieldState **out) {
    MovieClip *f = a->vm->player->focus;
    *out = (f && !f->dead && f->a1) ? f->a1->tfs : NULL;
    if (*out && !(*out)->has_sel) *out = NULL;
    return v_undef();
}
static Value sel_begin(Act *a, AObj *s, int c, Value *v) { TextFieldState *t; (void)s; (void)c; (void)v; sel_focus_tfs(a, &t); if (!t) return v_num(-1); return v_num(t->sel_begin < t->sel_end ? t->sel_begin : t->sel_end); }
static Value sel_end(Act *a, AObj *s, int c, Value *v) { TextFieldState *t; (void)s; (void)c; (void)v; sel_focus_tfs(a, &t); if (!t) return v_num(-1); return v_num(t->sel_begin > t->sel_end ? t->sel_begin : t->sel_end); }
static Value sel_caret(Act *a, AObj *s, int c, Value *v) { TextFieldState *t; (void)s; (void)c; (void)v; sel_focus_tfs(a, &t); if (!t) return v_num(-1); return v_num(t->sel_end); }
static Value sel_get_focus(Act *a, AObj *s, int c, Value *v) {
    MovieClip *f = a->vm->player->focus; (void)s; (void)c; (void)v;
    if (!f || f->dead || clip_removed(f)) return v_null();
    return v_str(clip_path(a->vm, f));
}
static Value sel_set_focus(Act *a, AObj *s, int argc, Value *argv) {
    extern void avm1_focus_set(VM *vm, MovieClip *c);
    extern int avm1_focus_set_checked(VM *vm, MovieClip *c);
    Value t = arg(argc, argv, 0); (void)s;
    if (argc != 1) return v_bool(0);      /* Flash ignores calls with extra arguments */
    if (t.t == V_UNDEF || t.t == V_NULL) { avm1_focus_set(a->vm, NULL); return v_bool(1); }
    {
        MovieClip *c = act_resolve_target_display(a, act_target_or_root(a), t, 0);
        if (a->vm->err) return v_undef();
        if (c && avm1_focus_set_checked(a->vm, c)) return v_bool(1);
    }
    return v_bool(0);
}
static Value sel_set_selection(Act *a, AObj *s, int argc, Value *argv) {
    TextFieldState *t; s32 b, e; MovieClip *f = a->vm->player->focus; (void)s;
    t = (f && !f->dead && f->a1) ? f->a1->tfs : NULL;
    if (!t || argc != 2) return v_undef();   /* Flash needs exactly (begin, end) */
    b = v_i32(a, argv[0]); if (a->vm->err) return v_undef();
    e = argc > 1 ? v_i32(a, argv[1]) : 0x7FFFFFFF; if (a->vm->err) return v_undef();
    if (b < 0) b = 0; if (e < 0) e = 0;
    if (b > t->rt.len) b = t->rt.len; if (e > t->rt.len) e = t->rt.len;
    t->sel_begin = b; t->sel_end = e; t->has_sel = 1;
    return v_undef();
}

AObj *lib_selection(VM *vm, Env *env) {
    AObj *o = obj_new(vm, env->p.object); u16 at = DE | DD | RO;
    defm(vm, env, o, "getBeginIndex", sel_begin, at);
    defm(vm, env, o, "getEndIndex", sel_end, at);
    defm(vm, env, o, "getCaretIndex", sel_caret, at);
    defm(vm, env, o, "getFocus", sel_get_focus, at);
    defm(vm, env, o, "setFocus", sel_set_focus, at);
    defm(vm, env, o, "setSelection", sel_set_selection, at);
    return o;
}

/* ---------------------------------------------------------------- */
/* System                                                           */

static int g_exact_settings = 1, g_use_codepage;
int avm1_use_codepage(void) { return g_use_codepage; }
static Value sys_get_exact(Act *a, AObj *s, int c, Value *v) { (void)a; (void)s; (void)c; (void)v; return v_bool(g_exact_settings); }
static Value sys_set_exact(Act *a, AObj *s, int argc, Value *argv) { (void)s; g_exact_settings = v_as_bool(arg(argc, argv, 0), a->swf_version); return v_undef(); }
static Value sys_get_codepage(Act *a, AObj *s, int c, Value *v) { (void)a; (void)s; (void)c; (void)v; return v_bool(g_use_codepage); }
static Value sys_set_codepage(Act *a, AObj *s, int argc, Value *argv) { (void)s; g_use_codepage = v_as_bool(arg(argc, argv, 0), a->swf_version); return v_undef(); }

#define CAP_BOOL(fn, val) static Value fn(Act *a, AObj *s, int c, Value *v) { (void)a; (void)s; (void)c; (void)v; return v_bool(val); }
#define CAP_NUM(fn, val) static Value fn(Act *a, AObj *s, int c, Value *v) { (void)a; (void)s; (void)c; (void)v; return v_num(val); }
#define CAP_STR(fn, val) static Value fn(Act *a, AObj *s, int c, Value *v) { (void)s; (void)c; (void)v; return v_str(str_intern(a->vm, val)); }
CAP_BOOL(cap_has_accessibility, 1)
CAP_NUM(cap_pixel_aspect, 1)
CAP_STR(cap_screen_color, "color")
CAP_NUM(cap_screen_dpi, 72)
extern int a2_screen_w, a2_screen_h;   /* the desktop size (set by the platform layer) */
static Value cap_res_y(Act *a, AObj *s, int c, Value *v) { (void)a; (void)s; (void)c; (void)v; return v_num(a2_screen_h); }
static Value cap_res_x(Act *a, AObj *s, int c, Value *v) { (void)a; (void)s; (void)c; (void)v; return v_num(a2_screen_w); }
CAP_BOOL(cap_false, 0)
CAP_BOOL(cap_true, 1)
const char *g_flash_version = "WIN 32,0,0,465";   /* content checks the version (e.g. MochiCrypt refuses SWFs newer than the player); report the last release, as Ruffle does */
/* the major version in $version ("WIN 10,3,..." -> 10) */
int avm1_player_major(void) {
    static const char *cached; static int major;
    if (cached != g_flash_version) {
        const char *s = g_flash_version ? strchr(g_flash_version, ' ') : NULL;
        major = s ? atoi(s + 1) : 10; cached = g_flash_version;
    }
    return major;
}
/* test harness option: built-in classes shaped as Ruffle declares them (its recorded
   expectations, output.ruffle.txt) instead of as the Flash Player builds them */
int avm1_ruffle_shapes(void) { static int v = -1; if (v < 0) v = getenv("FLASHNT_AVM1_RUFFLE_SHAPES") != NULL; return v; }
int avm1_fp_box_needs_class(void) { int v = avm1_player_major(); return !(v == 18 || v >= 20); }
int avm1_fp_sealed_arguments(void) { int v = avm1_player_major(); return v == 13 || (v >= 15 && v != 19); }
static Value cap_version(Act *a, AObj *s, int c, Value *v) { (void)s; (void)c; (void)v; return v_str(str_intern(a->vm, g_flash_version)); }
CAP_STR(cap_player_type, "StandAlone")
static Value cap_max_idc(Act *a, AObj *s, int c, Value *v) { (void)s; (void)c; (void)v; return v_str(str_intern(a->vm, "5.1")); }
CAP_STR(cap_manufacturer, "Adobe Windows")
CAP_STR(cap_os, "Windows NT")
CAP_STR(cap_cpu, "x86")
CAP_STR(cap_language, "en")
static Value cap_server_string(Act *a, AObj *s, int c, Value *v) {
    (void)s; (void)c; (void)v;
    return v_str(str_intern(a->vm, "A=t&SA=t&SV=t&EV=t&MP3=t&AE=t&VE=t&ACC=t&PR=t&SP=t&SB=f&DEB=f&V=WIN%2010%2C3%2C183%2C90&M=Adobe%20Windows&R=1024x768&COL=color&AR=1.0&OS=Windows%20NT&ARCH=x86&L=en&IME=t&PR32=t&PR64=f&PT=StandAlone&AVD=f&LFD=f&WD=f&TLS=t&ML=5.1&DP=72"));
}

static AObj *lib_capabilities(VM *vm, Env *env) {
    AObj *o = obj_new(vm, env->p.object);
    defp(vm, env, o, "hasAccessibility", cap_has_accessibility, NULL, 0);
    defp(vm, env, o, "pixelAspectRatio", cap_pixel_aspect, NULL, 0);
    defp(vm, env, o, "screenColor", cap_screen_color, NULL, 0);
    defp(vm, env, o, "screenDPI", cap_screen_dpi, NULL, 0);
    defp(vm, env, o, "screenResolutionY", cap_res_y, NULL, 0);
    defp(vm, env, o, "screenResolutionX", cap_res_x, NULL, 0);
    defp(vm, env, o, "hasTLS", cap_true, NULL, 0);
    defp(vm, env, o, "hasVideoEncoder", cap_true, NULL, 0);
    defp(vm, env, o, "hasAudioEncoder", cap_true, NULL, 0);
    defp(vm, env, o, "hasMP3", cap_true, NULL, 0);
    defp(vm, env, o, "hasAudio", cap_true, NULL, 0);
    defp(vm, env, o, "serverString", cap_server_string, NULL, 0);
    defp(vm, env, o, "version", cap_version, NULL, 0);
    defp(vm, env, o, "hasStreamingAudio", cap_true, NULL, 0);
    defp(vm, env, o, "hasStreamingVideo", cap_true, NULL, 0);
    defp(vm, env, o, "hasEmbeddedVideo", cap_true, NULL, 0);
    defp(vm, env, o, "hasPrinting", cap_true, NULL, 0);
    defp(vm, env, o, "hasScreenPlayback", cap_true, NULL, 0);
    defp(vm, env, o, "hasScreenBroadcast", cap_false, NULL, 0);
    defp(vm, env, o, "isDebugger", cap_false, NULL, 0);
    defp(vm, env, o, "playerType", cap_player_type, NULL, 0);
    defp(vm, env, o, "avHardwareDisable", cap_false, NULL, 0);
    defp(vm, env, o, "localFileReadDisable", cap_false, NULL, 0);
    defp(vm, env, o, "windowlessDisable", cap_false, NULL, 0);
    defp(vm, env, o, "maxLevelIDC", cap_max_idc, NULL, 0);
    defp(vm, env, o, "isEmbeddedInAcrobat", cap_false, NULL, 0);
    defp(vm, env, o, "manufacturer", cap_manufacturer, NULL, 0);
    defp(vm, env, o, "os", cap_os, NULL, 0);
    defp(vm, env, o, "cpuArchitecture", cap_cpu, NULL, 0);
    defp(vm, env, o, "language", cap_language, NULL, 0);
    defp(vm, env, o, "hasIME", cap_true, NULL, 0);
    defp(vm, env, o, "supports32BitProcesses", cap_true, NULL, 0);
    defp(vm, env, o, "supports64BitProcesses", cap_false, NULL, 0);
    return o;
}

static Value sec_sandbox(Act *a, AObj *s, int c, Value *v) {
    extern const char *avm1_movie_url(Player *p, SWFMovie *m);
    Player *p = a->vm->player;
    SWFMovie *m = (a->base_clip && a->base_clip->movie) ? a->base_clip->movie : p->movie;
    const char *u = avm1_movie_url(p, m);
    (void)s; (void)c; (void)v;
    if (u && (!strncmp(u, "http:", 5) || !strncmp(u, "https:", 6))) return v_cstr(a->vm, "remote");
    return v_cstr(a->vm, (m && (m->file_attr & 1)) ? "localWithNetwork" : "localWithFile");
}
static Value sec_escape_domain(Act *a, AObj *s, int argc, Value *argv) { (void)s; if (argc < 1) return v_undef(); return v_str(v_str_of(a, argv[0])); }

AObj *lib_system(VM *vm, Env *env, AObj **security, AObj **capabilities, AObj **ime, AObj **product) {
    AObj *sys = obj_new(vm, env->p.object), *sec, *im;
    defp(vm, env, sys, "exactSettings", sys_get_exact, sys_set_exact, V6);
    defp(vm, env, sys, "useCodepage", sys_get_codepage, sys_set_codepage, 0);
    defm(vm, env, sys, "setClipboard", noop, 0);
    defm(vm, env, sys, "showSettings", noop, 0);
    defm(vm, env, sys, "onStatus", noop, 0);
    sec = obj_new(vm, env->p.object);
    defm(vm, env, sec, "allowDomain", sec_allow_domain, 0);
    defm(vm, env, sec, "allowInsecureDomain", noop, 0);
    defm(vm, env, sec, "loadPolicyFile", noop, 0);
    defp(vm, env, sec, "chooseLocalSwfPath", noop, NULL, 0);
    defm(vm, env, sec, "escapeDomain", sec_escape_domain, 0);
    defp(vm, env, sec, "sandboxType", sec_sandbox, NULL, 0);
    defm(vm, env, sec, "PolicyFileResolver", noop, 0);
    im = obj_new(vm, env->p.object);
    {
        static const char *n[] = { "UNKNOWN", "KOREAN", "JAPANESE_KATAKANA_HALF", "JAPANESE_KATAKANA_FULL", "JAPANESE_HIRAGANA", "CHINESE", "ALPHANUMERIC_HALF", "ALPHANUMERIC_FULL", NULL };
        int i; u16 at = DE | DD | RO;
        for (i = 0; n[i]; i++) defs(vm, im, n[i], n[i], at);
        defm(vm, env, im, "getEnabled", ret_false, at);
        defm(vm, env, im, "setEnabled", ret_false, at);
        defm(vm, env, im, "getConversionMode", noop, at);
        defm(vm, env, im, "setConversionMode", ret_false, at);
        defm(vm, env, im, "setCompositionString", ret_false, at);
        defm(vm, env, im, "doConversion", ret_true, at);
        defm(vm, env, im, "onIMEComposition", noop, at);
    }
    *security = sec; *capabilities = lib_capabilities(vm, env); *ime = im;
    {
        AObj *proto, *c = lib_class(vm, env, NULL, product_ctor, env->p.object, 1, &proto);
        defm(vm, env, proto, "isRunning", ret_false, 0);
        defm(vm, env, proto, "isInstalled", ret_false, 0);
        defm(vm, env, proto, "launch", noop, 0);
        defm(vm, env, proto, "download", noop, 0);
        defm(vm, env, proto, "validate", noop, 0);
        defm(vm, env, proto, "installedVersion", noop, 0);
        *product = c;
    }
    return sys;
}

/* ---------------------------------------------------------------- */
/* Accessibility, SoundCodec, TextRenderer                          */

static Value acc_method(Act *a, AObj *self, int argc, Value *argv, int index) { (void)a; (void)self; (void)argc; (void)argv; return index == 0 ? v_bool(0) : v_undef(); }
AObj *lib_accessibility(VM *vm, Env *env) {
    AObj *o = obj_new(vm, env->p.object); u16 at = DD | RO | V6;
    o_define(vm, o, str_intern(vm, "isActive"), v_obj(fn_table(vm, env, acc_method, 0)), at);
    o_define(vm, o, str_intern(vm, "sendEvent"), v_obj(fn_table(vm, env, acc_method, 1)), at);
    o_define(vm, o, str_intern(vm, "updateProperties"), v_obj(fn_table(vm, env, acc_method, 2)), at);
    return o;
}
AObj *lib_soundcodec(VM *vm, Env *env) {
    AObj *o = obj_new(vm, env->p.object);
    defs(vm, o, "SPEEX", "Speex", 0);
    defs(vm, o, "NELLYMOSER", "Nellymoser", 0);
    return o;
}
AObj *lib_textrenderer(VM *vm, Env *env, AObj *object_proto) { return lib_empty_class(vm, env, object_proto); }

/* ---------------------------------------------------------------- */
/* Color                                                            */

static MovieClip *color_target(Act *a, AObj *self) {
    Value t = o_get(a, self, str_intern(a->vm, "target"));
    if (a->vm->err || t.t == V_UNDEF) return NULL;
    return act_resolve_target_display(a, act_target_or_root(a), t, 0);
}
static Value color_ctor(Act *a, AObj *self, int argc, Value *argv) {
    o_define(a->vm, self, str_intern(a->vm, "target"), arg(argc, argv, 0), DD | RO | DE);
    return v_undef();
}
static Value color_get_rgb(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = color_target(a, self); SWFCxform *x; (void)argc; (void)argv;
    if (!c || !c->self) return v_undef();
    x = &c->self->cxform;
    return v_num((double)(s32)(((s32)x->ar << 16) | ((s32)x->ag << 8) | (s32)x->ab));
}
static Value color_set_rgb(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = color_target(a, self); s32 rgb; SWFCxform *x;
    if (!c || !c->self) return v_undef();
    c->self->script_moved = 1;
    rgb = v_i32(a, arg(argc, argv, 0)); if (a->vm->err) return v_undef();
    x = &c->self->cxform;
    x->mr = x->mg = x->mb = 0;
    x->ar = (u8)(rgb >> 16); x->ag = (u8)(rgb >> 8); x->ab = (u8)rgb;
    return v_undef();
}
static Value color_get_transform(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = color_target(a, self); SWFCxform *x; AObj *o; VM *vm = a->vm; (void)argc; (void)argv;
    if (!c || !c->self) return v_undef();
    x = &c->self->cxform;
    o = obj_new(vm, PROTOS(a)->object);
    o_set(a, o, str_intern(vm, "ra"), v_num(x->mr / 256.0 * 100.0));
    o_set(a, o, str_intern(vm, "ga"), v_num(x->mg / 256.0 * 100.0));
    o_set(a, o, str_intern(vm, "ba"), v_num(x->mb / 256.0 * 100.0));
    o_set(a, o, str_intern(vm, "aa"), v_num(x->ma / 256.0 * 100.0));
    o_set(a, o, str_intern(vm, "rb"), v_num(x->ar));
    o_set(a, o, str_intern(vm, "gb"), v_num(x->ag));
    o_set(a, o, str_intern(vm, "bb"), v_num(x->ab));
    o_set(a, o, str_intern(vm, "ab"), v_num(x->aa));
    return v_obj(o);
}
static int cset_mult(Act *a, AObj *t, const char *n, s16 *out) {
    AStr *k = str_intern(a->vm, n);
    if (o_has_own_property(a, t, k)) {
        double d = v_num_of(a, o_get(a, t, k));
        if (a->vm->err) return 0;
        *out = (s16)(u16)(u32)f64_i32(d * 256.0 / 100.0);
    }
    return 1;
}
static int cset_add(Act *a, AObj *t, const char *n, s16 *out) {
    AStr *k = str_intern(a->vm, n);
    if (o_has_own_property(a, t, k)) {
        Value v = o_get(a, t, k);
        if (a->vm->err) return 0;
        *out = v_i16(a, v);
        if (a->vm->err) return 0;
    }
    return 1;
}
static Value color_set_transform(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = color_target(a, self); SWFCxform x; AObj *t;
    if (!c || !c->self) return v_undef();
    c->self->script_moved = 1;
    x = c->self->cxform;
    t = v_to_obj_or_bare(a, arg(argc, argv, 0)); if (a->vm->err) return v_undef();
    if (!cset_mult(a, t, "ra", &x.mr) || !cset_mult(a, t, "ga", &x.mg) || !cset_mult(a, t, "ba", &x.mb) || !cset_mult(a, t, "aa", &x.ma)) return v_undef();
    if (!cset_add(a, t, "rb", &x.ar) || !cset_add(a, t, "gb", &x.ag) || !cset_add(a, t, "bb", &x.ab) || !cset_add(a, t, "ab", &x.aa)) return v_undef();
    c->self->cxform = x;
    return v_undef();
}

AObj *lib_color(VM *vm, Env *env, AObj *object_proto) {
    AObj *proto, *c = lib_class(vm, env, NULL, color_ctor, object_proto, 0, &proto); u16 at = DE | DD | RO;
    env->p.color = proto;
    defm(vm, env, proto, "setRGB", color_set_rgb, at);
    defm(vm, env, proto, "setTransform", color_set_transform, at);
    defm(vm, env, proto, "getRGB", color_get_rgb, at);
    defm(vm, env, proto, "getTransform", color_get_transform, at);
    return c;
}

/* ---------------------------------------------------------------- */
/* declared-only classes                                            */

#define EMPTY_LIB(fn, field) AObj *fn(VM *vm, Env *env, AObj *object_proto) { AObj *proto, *c = lib_class(vm, env, NULL, NULL, object_proto, 1, &proto); field; return c; }
/* Video, Camera, Microphone (Ruffle video.rs / camera.rs / microphone.rs declarations) */
/* Video.attachVideo(netStream): the Video shows the stream's pictures (null detaches) */
static Value video_attach(Act *a, AObj *self, int argc, Value *argv) {
    extern struct NetStream *avm1_netstream_of(AObj *o); MovieClip *c = o_display(self); (void)a;
    if (!c || !c->self) return v_undef();
    c->self->vstream = argc > 0 && argv[0].t == V_OBJ ? (void*)avm1_netstream_of(argv[0].u.o) : NULL;
    c->self->vid_cleared = 0;
    return v_undef();
}
static Value video_clear(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = o_display(self); (void)a; (void)argc; (void)argv;
    if (c && c->self) c->self->vid_cleared = 1;
    return v_undef();
}
static Value video_dim(AObj *self, int which) {
    extern void video_dims(Player *p, DisplayObject *d, int *vw, int *vh);
    MovieClip *c = o_display(self); int w = 0, h = 0;
    if (!c || !c->self) return v_undef();
    video_dims(c->player, c->self, &w, &h);
    return v_num(which ? h : w);
}
static Value video_width(Act *a, AObj *self, int argc, Value *argv) { (void)a; (void)argc; (void)argv; return video_dim(self, 0); }
static Value video_height(Act *a, AObj *self, int argc, Value *argv) { (void)a; (void)argc; (void)argv; return video_dim(self, 1); }
static Value video_smoothing(Act *a, AObj *self, int argc, Value *argv) { MovieClip *c = o_display(self); (void)a; (void)argc; (void)argv; return c && c->self ? v_bool(c->self->vid_smooth != 0) : v_undef(); }
static Value video_set_smoothing(Act *a, AObj *self, int argc, Value *argv) { MovieClip *c = o_display(self); if (c && c->self && argc > 0) c->self->vid_smooth = v_as_bool(argv[0], a->swf_version); return v_undef(); }
static Value video_deblocking(Act *a, AObj *self, int argc, Value *argv) { MovieClip *c = o_display(self); (void)a; (void)argc; (void)argv; return c && c->self ? v_num(c->self->vid_deblock) : v_undef(); }
static Value video_set_deblocking(Act *a, AObj *self, int argc, Value *argv) {
    MovieClip *c = o_display(self); double d;
    if (!c || !c->self || argc < 1) return v_undef();
    d = v_num_of(a, argv[0]); if (a->vm->err) return v_undef();
    c->self->vid_deblock = d == d ? (int)d : 0;
    return v_undef();
}
AObj *lib_video(VM *vm, Env *env, AObj *object_proto) {
    AObj *proto, *c = lib_class(vm, env, NULL, NULL, object_proto, 1, &proto);
    env->p.video = proto;
    defm(vm, env, proto, "attachVideo", video_attach, DE | DD | V6);
    if (!avm1_ruffle_shapes()) defm(vm, env, proto, "clear", video_clear, DE | DD | V6);
    return c;
}
/* the first Video object on the stage gives Video.prototype its properties
   (Gnash Video.as / NetStream-SquareTest: not there before, not for `new Video`) */
void a1_video_populate_proto(Act *a, AObj *proto) {
    VM *vm = a->vm; Env *env = ENV(a);
    static const char *const names[4] = { "smoothing", "deblocking", "height", "width" };
    static const NativeFn g[4] = { video_smoothing, video_deblocking, video_height, video_width };
    static const NativeFn st[4] = { video_set_smoothing, video_set_deblocking, NULL, NULL };
    int i;
    if (!proto) return;
    for (i = 0; i < 4; i++) {
        AStr *n = str_intern(vm, names[i]);
        if (o_has_own_property(a, proto, n)) continue;
        defp(vm, env, proto, names[i], g[i], st[i], DE | DD);
    }
}
/* Camera.get() / Microphone.get(): null, there is no capture device */
static Value no_device(Act *a, AObj *s, int c, Value *v) { (void)a; (void)s; (void)c; (void)v; return v_null(); }
static Value empty_names(Act *a, AObj *s, int c, Value *v) { (void)s; (void)c; (void)v; return v_obj(obj_array(a, 0, NULL)); }
AObj *lib_camera(VM *vm, Env *env, AObj *object_proto) {
    static const char *const m[] = { "setMode", "setQuality", "setKeyFrameInterval", "setMotionLevel", "setLoopback", "setCursor" };
    AObj *proto, *c = lib_class(vm, env, NULL, NULL, object_proto, 0, &proto); int i;
    env->p.camera = proto;
    for (i = 0; i < 6; i++) defm(vm, env, proto, m[i], noop, DE | DD);
    defm(vm, env, c, "get", no_device, 0);
    defp(vm, env, c, "names", empty_names, NULL, 0);
    return c;
}
AObj *lib_microphone(VM *vm, Env *env, AObj *object_proto) {
    static const char *const m[] = { "setSilenceLevel", "setRate", "setGain", "setUseEchoSuppression", "setCodec", "setFramesPerPacket", "setEncodeQuality" };
    AObj *proto, *c = lib_class(vm, env, NULL, NULL, object_proto, 0, &proto); int i;
    env->p.microphone = proto;
    for (i = 0; i < 7; i++) defm(vm, env, proto, m[i], noop, DE | DD);
    defm(vm, env, c, "get", no_device, 0);
    defp(vm, env, c, "names", empty_names, NULL, 0);
    return c;
}
AObj *lib_remote_lso_usage(VM *vm, Env *env, AObj *object_proto) {
    AObj *c = lib_class(vm, env, NULL, NULL, object_proto, 0, NULL);
    defm(vm, env, c, "getURLPageSupport", noop, DE);
    return c;
}
AObj *lib_asset_cache(VM *vm, Env *env, AObj *object_proto) {
    AObj *c = lib_class(vm, env, NULL, NULL, object_proto, 0, NULL);
    defm(vm, env, c, "purge", noop, DE);
    defm(vm, env, c, "getDiskUsage", noop, DE);
    return c;
}
static Value pj_zero(Act *a, AObj *s, int c, Value *v) { (void)a; (void)s; (void)c; (void)v; return v_num(0); }
static Value pj_none(Act *a, AObj *s, int c, Value *v) { (void)a; (void)s; (void)c; (void)v; return v_undef(); }
AObj *lib_printjob(VM *vm, Env *env, AObj *object_proto) {
    AObj *proto, *c = lib_class(vm, env, NULL, pj_none, object_proto, 1, &proto);
    o_define(vm, proto, str_intern(vm, "start"), v_obj(fn_native(vm, env, pj_none, NULL)), PA_DONT_ENUM | PA_DONT_DELETE | PA_VERSION_7);
    o_define(vm, proto, str_intern(vm, "addPage"), v_obj(fn_native(vm, env, pj_none, NULL)), PA_DONT_ENUM | PA_DONT_DELETE | PA_VERSION_7);
    o_define(vm, proto, str_intern(vm, "send"), v_obj(fn_native(vm, env, pj_none, NULL)), PA_DONT_ENUM | PA_DONT_DELETE | PA_VERSION_7);
    { static const char *props[] = { "paperHeight", "paperWidth", "pageHeight", "pageWidth", "orientation", NULL }; int i;
      for (i = 0; props[i]; i++) o_add_property(vm, proto, str_intern(vm, props[i]), fn_native(vm, env, pj_zero, NULL), NULL, PA_DONT_DELETE | PA_READ_ONLY); }
    return c;
}


Value transform_method(Act *a, AObj *self, int argc, Value *argv, int index) { (void)a; (void)self; (void)argc; (void)argv; (void)index; return v_undef(); }

/* sounds that finished: onSoundComplete */
void avm1_mark_extra(VM *vm) {
    (void)vm;
}

/* ---------------------------------------------------------------- */
/* navigation and loading                                           */

extern void (*g_fscommand_hook)(Player *p, const char *cmd, const char *args);
int avm1_fscommand(Act *a, AStr *url, AStr *target) {
    VM *vm = a->vm; const char *u = str_c(vm, url);
    if (strlen(u) >= 10 && str_starts_c_ci(url, "fscommand:")) {
        const char *cmd = u + 10;
        if (!strcmp(cmd, "quit")) vm->player->stopped = 1;
        if (g_fscommand_hook) g_fscommand_hook(vm->player, cmd, str_c(vm, target));
        return 1;
    }
    return 0;
}


/* ---------------------------------------------------------------- */
/* flash.external.ExternalInterface (no host container: unavailable) */

static AStr *xml_escape(Act *a, AStr *s) {
    SBuf b; u32 i;
    sb_init(&b);
    for (i = 0; i < s->len; i++) {
        u16 c = STR_AT(s, i);
        switch (c) {
        case '&': sb_ascii(&b, "&amp;"); break;
        case '"': sb_ascii(&b, "&quot;"); break;
        case '\'': sb_ascii(&b, "&apos;"); break;
        case '<': sb_ascii(&b, "&lt;"); break;
        case '>': sb_ascii(&b, "&gt;"); break;
        default: sb_unit(&b, c);
        }
    }
    return sb_done(a->vm, &b);
}

/* Ruffle's sequence of whole-string replacements */
static AStr *str_replace_all(Act *a, AStr *s, const char *from, const char *to) {
    AStr *f = str_intern(a->vm, from); SBuf b; u32 i = 0; long k;
    if (f->len == 0) return s;
    if (str_find(s, f, 0) < 0) return s;
    sb_init(&b);
    while ((k = str_find(s, f, i)) >= 0) {
        u32 j;
        for (j = i; j < (u32)k; j++) sb_unit(&b, STR_AT(s, j));
        sb_ascii(&b, to);
        i = (u32)k + f->len;
    }
    for (; i < s->len; i++) sb_unit(&b, STR_AT(s, i));
    return sb_done(a->vm, &b);
}

static Value ei_str_result(AStr *s) { return s->len ? v_str(s) : v_null(); }

static Value ei_escape_xml(Act *a, AObj *s, int argc, Value *argv) {
    AStr *t; (void)s;
    if (argc < 1) return v_null();
    t = v_str_of(a, argv[0]); if (a->vm->err) return v_undef();
    return ei_str_result(xml_escape(a, t));
}

static Value ei_unescape_xml(Act *a, AObj *s, int argc, Value *argv) {
    AStr *t; (void)s;
    if (argc < 1) return v_null();
    t = v_str_of(a, argv[0]); if (a->vm->err) return v_undef();
    t = str_replace_all(a, t, "&gt;", ">");
    t = str_replace_all(a, t, "&lt;", "<");
    t = str_replace_all(a, t, "&apos;", "'");
    t = str_replace_all(a, t, "&quot;", "\"");
    t = str_replace_all(a, t, "&amp;", "&");
    return ei_str_result(t);
}

static Value ei_js_quote(Act *a, AObj *s, int argc, Value *argv) {
    AStr *t; SBuf b; u32 i; (void)s;
    if (argc < 1) return v_null();
    t = v_str_of(a, argv[0]); if (a->vm->err) return v_undef();
    sb_init(&b);
    for (i = 0; i < t->len; i++) {
        u16 c = STR_AT(t, i);
        if (c == '"') sb_ascii(&b, "\\\"");
        else if (c == '\n') sb_ascii(&b, "\\n");
        else if (c == '\r') sb_ascii(&b, "\\r");
        else sb_unit(&b, c);
    }
    return ei_str_result(sb_done(a->vm, &b));
}

static Value ret_null(Act *a, AObj *s, int c, Value *v) { (void)a; (void)s; (void)c; (void)v; return v_null(); }

/* ---- container calls (ExternalInterface.call / addCallback) ---- */
#include "extval.h"
typedef struct { char *name; Value self; AObj *method; } EICallback;
static EICallback *g_eicb; static int g_neicb;

static ExtVal ei_from_avm1(Act *a, Value v, int depth) {
    switch (v.t) {
    case V_UNDEF: return xv_undef();
    case V_NULL: return xv_null();
    case V_BOOL: return xv_bool(v.u.b);
    case V_NUM: return xv_num(v.u.n);
    case V_STR: return xv_str(str_c(a->vm, v.u.s));
    case V_CLIP: return xv_null();
    default: break;
    }
    if (v.t == V_OBJ && v.u.o && depth < 64) {
        AObj *o = v.u.o; ExtVal r;
        if (o->nk == NK_ARRAY) {
            int n = o_length(a, o), i;
            r = xv_undef(); r.t = XV_LIST;
            for (i = 0; i < n && !a->vm->err; i++) xv_list_push(&r, ei_from_avm1(a, o_get_element(a, o, i), depth + 1));
            return r;
        } else {
            int n = 0, i; AStr **keys = o_keys(a, o, 0, &n);
            r = xv_undef(); r.t = XV_OBJ;
            for (i = 0; i < n && !a->vm->err; i++) { Value e = o_get(a, o, keys[i]); xv_obj_put(&r, str_c(a->vm, keys[i]), ei_from_avm1(a, e, depth + 1)); }
            free(keys);
            return r;
        }
    }
    return xv_null();
}
static Value ei_to_avm1(Act *a, const ExtVal *x) {
    int i;
    switch (x->t) {
    case XV_UNDEF: return v_undef();
    case XV_NULL: return v_null();
    case XV_BOOL: return v_bool(x->b);
    case XV_NUM: return v_num(x->n);
    case XV_STR: {
        const char *s = x->s ? x->s : ""; const char *q = s;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (a->swf_version < 9 && !*q) return v_cstr(a->vm, "null");
        return v_str(str_from_swf(a->vm, s, strlen(s), 6)); }
    case XV_LIST: {
        Value *items = (Value*)malloc(((size_t)x->count + 1) * sizeof(Value)); AObj *arr;
        if (!items) return v_undef();
        for (i = 0; i < x->count; i++) items[i] = ei_to_avm1(a, &x->items[i]);
        arr = obj_array(a, x->count, items); free(items);
        return arr ? v_obj(arr) : v_undef(); }
    case XV_OBJ: {
        AObj *o = obj_new(a->vm, PROTOS(a)->object);
        if (!o) return v_undef();
        for (i = 0; i < x->count; i++) o_set(a, o, str_from_swf(a->vm, x->keys[i], strlen(x->keys[i]), 6), ei_to_avm1(a, &x->items[i]));
        return v_obj(o); }
    }
    return v_undef();
}

static Value ei_available(Act *a, AObj *s, int c, Value *v) { (void)a; (void)s; (void)c; (void)v; return v_bool(g_ext_provider != NULL); }
static Value ei_add_callback(Act *a, AObj *s, int argc, Value *argv) {
    AStr *name; int i; (void)s;
    if (!g_ext_provider || argc < 3) return v_bool(0);
    name = v_str_of(a, argv[0]); if (a->vm->err) return v_undef();
    if (argv[2].t != V_OBJ || !argv[2].u.o) return v_bool(0);
    for (i = 0; i < g_neicb; i++) if (!strcmp(g_eicb[i].name, str_c(a->vm, name))) break;
    if (i == g_neicb) {
        EICallback *n = (EICallback*)realloc(g_eicb, (size_t)(g_neicb + 1) * sizeof(EICallback));
        if (!n) return v_bool(0);
        g_eicb = n; g_neicb++;
        g_eicb[i].name = (char*)malloc(strlen(str_c(a->vm, name)) + 1); if (g_eicb[i].name) strcpy(g_eicb[i].name, str_c(a->vm, name));
    }
    g_eicb[i].self = argv[1]; g_eicb[i].method = argv[2].u.o;
    return v_bool(1);
}
static Value ei_call(Act *a, AObj *s, int argc, Value *argv) {
    AStr *name; ExtVal *args, out; int i, n; Value r; (void)s;
    if (!g_ext_provider) return v_null();
    name = v_str_of(a, argc > 0 ? argv[0] : v_undef()); if (a->vm->err) return v_undef();
    n = argc > 1 ? argc - 1 : 0;
    args = (ExtVal*)calloc((size_t)n + 1, sizeof(ExtVal));
    if (!args) return v_null();
    for (i = 0; i < n; i++) { args[i] = ei_from_avm1(a, argv[i + 1], 0); if (a->vm->err) { int k; for (k = 0; k <= i; k++) xv_free(&args[k]); free(args); return v_undef(); } }
    out = xv_undef();
    g_ext_provider(a->vm->player, str_c(a->vm, name), n, args, &out);
    for (i = 0; i < n; i++) xv_free(&args[i]);
    free(args);
    r = ei_to_avm1(a, &out);
    xv_free(&out);
    return r;
}
/* the container calls a registered callback; 0 when there is none */
int avm1_ei_invoke(Player *p, const char *name, int argc, const ExtVal *args, ExtVal *out) {
    VM *vm = (VM*)p->avm; int i, k; Act a; Value *av, r;
    if (!vm) return 0;
    for (i = 0; i < g_neicb; i++) if (!strcmp(g_eicb[i].name, name)) break;
    if (i == g_neicb) return 0;
    act_init_nothing(&a, vm, p->root, "[ExternalInterface]");
    av = (Value*)malloc(((size_t)argc + 1) * sizeof(Value));
    if (!av) return 0;
    for (k = 0; k < argc; k++) av[k] = ei_to_avm1(&a, &args[k]);
    r = o_call(&a, g_eicb[i].method, g_eicb[i].self, argc, av);
    free(av);
    if (vm->err) { vm_report_error(&a); *out = xv_null(); }
    else *out = ei_from_avm1(&a, r, 0);
    { extern void avm1_run_actions(VM *vm); avm1_run_actions(vm); }
    return 1;
}
void avm1_ei_mark(VM *vm) {
    int i;
    for (i = 0; i < g_neicb; i++) { gc_mark_obj(vm, g_eicb[i].method); if (g_eicb[i].self.t == V_OBJ && g_eicb[i].self.u.o) gc_mark_obj(vm, g_eicb[i].self.u.o); }
}
void avm1_ei_free(void) { int i; for (i = 0; i < g_neicb; i++) free(g_eicb[i].name); free(g_eicb); g_eicb = NULL; g_neicb = 0; }

AObj *lib_external_interface(VM *vm, Env *env, AObj *object_proto) {
    AObj *c = lib_class(vm, env, NULL, NULL, object_proto, 0, NULL); u16 at = DE | DD | RO | PA_VERSION_8;
    defm(vm, env, c, "_initJS", noop, at);
    defm(vm, env, c, "_objectID", noop, at);
    defm(vm, env, c, "_addCallback", noop, at);
    defm(vm, env, c, "_evalJS", noop, at);
    defm(vm, env, c, "_callOut", noop, at);
    defm(vm, env, c, "_escapeXML", ei_escape_xml, at);
    defm(vm, env, c, "_unescapeXML", ei_unescape_xml, at);
    defm(vm, env, c, "_jsQuoteString", ei_js_quote, at);
    defm(vm, env, c, "_useSetReturnValueHack", noop, at);
    defp(vm, env, c, "available", ei_available, NULL, at);
    defm(vm, env, c, "addCallback", ei_add_callback, at);
    defm(vm, env, c, "call", ei_call, DE | DD | RO);
    return c;
}

/* flash.automation (stubs, like Flash Player without a test harness) */
Value automation_capture_method(Act *a, AObj *self, int argc, Value *argv, int index) { (void)a; (void)argc; (void)argv; return index == 100 ? v_obj(self) : v_undef(); }
Value automation_generator_method(Act *a, AObj *self, int argc, Value *argv, int index) { (void)a; (void)self; (void)argc; (void)argv; (void)index; return v_undef(); }
Value automation_config_method(Act *a, AObj *self, int argc, Value *argv, int index) { (void)a; (void)self; (void)argc; (void)argv; (void)index; return v_undef(); }
Value automation_capture_ctor(Act *a, AObj *self, int argc, Value *argv) { (void)a; (void)argc; (void)argv; return v_obj(self); }
