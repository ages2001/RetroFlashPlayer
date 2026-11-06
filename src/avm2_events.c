/* avm2_events.c - flash.events: Event classes, EventDispatcher, propagation,
 * Timer, setTimeout, broadcast (enterFrame & co). */
#include "avm2_flash_int.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------- */
/* Event                                                            */

#define EV_STOP 1
#define EV_STOP_NOW 2
#define EV_PREVENTED 4
#define EV_DISPATCHED 8   /* reached at least one listener: a re-dispatch clones it */

static void ev_set(VM2 *vm, A2Obj *e, const char *k, V2 v) { a2_dyn_set(vm, e, a2_str(vm, k), v); }
static V2 ev_get(VM2 *vm, V2 self, const char *k) { A2Prop *p = self.t == T_OBJ ? a2_dyn_find(self.u.o, a2_str(vm, k)) : NULL; return p ? p->v : v2_undef(); }

A2Obj *a2_make_event(VM2 *vm, A2Class *cls, A2Str *type, int bubbles, int cancelable) {
    A2Obj *e = a2_new(vm, cls ? cls : vm->c_event); e->kind = OK_EVENT;
    ev_set(vm, e, "type", v2_str(type)); ev_set(vm, e, "bubbles", v2_bool(bubbles)); ev_set(vm, e, "cancelable", v2_bool(cancelable));
    ev_set(vm, e, "target", v2_null()); ev_set(vm, e, "currentTarget", v2_null()); ev_set(vm, e, "eventPhase", v2_int(2));
    return e;
}
A2Obj *a2_new_event(VM2 *vm, A2Class *cls, A2Str *type, int bubbles) { return a2_make_event(vm, cls, type, bubbles, 0); }

static V2 Event_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *e = SELFO; if (!e) return v2_undef();
    e->kind = OK_EVENT;
    ev_set(vm, e, "type", v2_str(STR(0))); ev_set(vm, e, "bubbles", v2_bool(argc > 1 ? BOOL(1) : 0)); ev_set(vm, e, "cancelable", v2_bool(argc > 2 ? BOOL(2) : 0));
    ev_set(vm, e, "target", v2_null()); ev_set(vm, e, "currentTarget", v2_null()); ev_set(vm, e, "eventPhase", v2_int(2));
    return v2_undef();
}
#define EVGET(fn, key) static V2 fn(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return ev_get(vm, self, key); }
EVGET(ev_type, "type") EVGET(ev_bubbles, "bubbles") EVGET(ev_cancelable, "cancelable") EVGET(ev_target, "target") EVGET(ev_currentTarget, "currentTarget") EVGET(ev_eventPhase, "eventPhase")
static V2 ev_stopPropagation(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; if (SELFO) SELFO->ival |= EV_STOP; return v2_undef(); }
static V2 ev_stopImmediatePropagation(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; if (SELFO) SELFO->ival |= EV_STOP | EV_STOP_NOW; return v2_undef(); }
static V2 ev_preventDefault(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; if (SELFO && v2_tobool(vm, ev_get(vm, self, "cancelable"))) SELFO->ival |= EV_PREVENTED; return v2_undef(); }
static V2 ev_isDefaultPrevented(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_bool(SELFO && (SELFO->ival & EV_PREVENTED)); }
static V2 ev_clone(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *e = SELFO, *c; int idx = 0; A2Str *k; V2 v; (void)argc; (void)argv;
    if (!e) return v2_undef();
    /* the built-in clone() makes an instance of the native event class with its fields only:
       a script subclass that does not override clone() gets a plain native event back */
    { A2Class *nc = e->cls; while (nc && nc->abc) nc = nc->super; if (!nc) nc = vm->c_event;
      c = a2_new(vm, nc); c->kind = OK_EVENT;
      for (;;) { a2_dyn_enum(e, &idx, &k, &v); if (!idx) break; if (nc == e->cls || a2_find_trait_any(&nc->itraits, k)) a2_dyn_set(vm, c, k, v); } }
    ev_set(vm, c, "target", v2_null()); ev_set(vm, c, "currentTarget", v2_null()); ev_set(vm, c, "eventPhase", v2_int(2));
    { int i; for (i = 0; i < e->nslots && i < c->nslots; i++) c->slots[i] = e->slots[i]; }
    return v2_obj(c);
}
static V2 ev_formatToString(VM2 *vm, V2 self, int argc, V2 *argv) {
    char buf[1024]; int i; size_t n;
    sprintf(buf, "[%s", STR(0)->s); n = strlen(buf);
    for (i = 1; i < argc; i++) { A2Str *k = v2_tostr(vm, argv[i].t == T_UNDEF ? v2_null() : argv[i]); V2 v = a2_get_s(vm, self, k->s); A2Str *vs = v2_tostr(vm, v); if (n + k->len + vs->len + 6 >= sizeof(buf)) break; if (v.t == T_STR) sprintf(buf + n, " %s=\"%s\"", k->s, vs->s); else sprintf(buf + n, " %s=%s", k->s, vs->s); n = strlen(buf); }
    strcat(buf, "]");
    return v2_cstr(vm, buf);
}
/* toString of the built-in event classes: class name and its fields (as Flash's formatToString calls) */
static const char *ev_fields[][12] = {
    { "MouseEvent", "localX", "localY", "stageX", "stageY", "relatedObject", "ctrlKey", "altKey", "shiftKey", "buttonDown", "delta", NULL },
    { "FocusEvent", "relatedObject", "shiftKey", "keyCode", NULL },
    { "KeyboardEvent", "charCode", "keyCode", "keyLocation", "ctrlKey", "altKey", "shiftKey", NULL },
    { "TextEvent", "text", NULL }, { "ErrorEvent", "text", NULL }, { "IOErrorEvent", "text", NULL }, { "SecurityErrorEvent", "text", NULL }, { "AsyncErrorEvent", "text", "error", NULL },
    { "ProgressEvent", "bytesLoaded", "bytesTotal", NULL },
    { "HTTPStatusEvent", "status", "redirected", "responseURL", NULL },
    { "StatusEvent", "code", "level", NULL }, { "NetStatusEvent", "info", NULL }, { "DataEvent", "data", NULL },
    { "TimerEvent", NULL }, { "FullScreenEvent", "fullScreen", NULL }, { NULL }
};
static V2 ev_toString(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 a[16]; int n = 5, i, k; const char *cn = "Event"; A2Class *nc = SELFO ? SELFO->cls : NULL; (void)argc; (void)argv;
    while (nc && nc->abc) nc = nc->super;
    a[1] = v2_cstr(vm, "type"); a[2] = v2_cstr(vm, "bubbles"); a[3] = v2_cstr(vm, "cancelable"); a[4] = v2_cstr(vm, "eventPhase");
    if (nc && nc->name) for (i = 0; ev_fields[i][0]; i++) if (!strcmp(nc->name->s, ev_fields[i][0])) { cn = ev_fields[i][0]; for (k = 1; ev_fields[i][k] && n < 16; k++) a[n++] = v2_cstr(vm, ev_fields[i][k]); break; }
    a[0] = v2_cstr(vm, cn);
    return ev_formatToString(vm, self, n, a);
}

/* generic field accessors for subclasses: getter/setter by dynamic key */
#define EVFIELD(fn, key) static V2 fn##_get(VM2 *vm, V2 self, int argc, V2 *argv) { V2 v; (void)argc; (void)argv; v = ev_get(vm, self, key); return v; } static V2 fn##_set(VM2 *vm, V2 self, int argc, V2 *argv) { if (SELFO) ev_set(vm, SELFO, key, ARG(0)); return v2_undef(); }
EVFIELD(f_localX, "localX") EVFIELD(f_localY, "localY") EVFIELD(f_buttonDown, "buttonDown") EVFIELD(f_delta, "delta") EVFIELD(f_ctrlKey, "ctrlKey") EVFIELD(f_altKey, "altKey") EVFIELD(f_shiftKey, "shiftKey") EVFIELD(f_relatedObject, "relatedObject")
EVFIELD(f_charCode, "charCode") EVFIELD(f_keyCode, "keyCode") EVFIELD(f_keyLocation, "keyLocation") EVFIELD(f_text, "text") EVFIELD(f_bytesLoaded, "bytesLoaded") EVFIELD(f_bytesTotal, "bytesTotal") EVFIELD(f_info, "info") EVFIELD(f_errorID, "errorID") EVFIELD(f_error, "error")
EVFIELD(f_status, "status") EVFIELD(f_redirected, "redirected") EVFIELD(f_responseURL, "responseURL") EVFIELD(f_responseHeaders, "responseHeaders") EVFIELD(f_level, "level") EVFIELD(f_code, "code") EVFIELD(f_fullScreen, "fullScreen") EVFIELD(f_data, "data") EVFIELD(f_position, "position") EVFIELD(f_activating, "activating") EVFIELD(f_isRelatedObjectInaccessible, "isRelatedObjectInaccessible") EVFIELD(f_movementX, "movementX") EVFIELD(f_movementY, "movementY")
static V2 f_stageX(VM2 *vm, V2 self, int argc, V2 *argv) { V2 t = ev_get(vm, self, "target"); double lx = v2_tonum(vm, ev_get(vm, self, "localX")), ly = v2_tonum(vm, ev_get(vm, self, "localY")); (void)argc; (void)argv; if (t.t == T_OBJ && t.u.o->kind == OK_DISPLAY) { SWFMatrix w; double ox, oy; a2_world_matrix(t.u.o, &w); swf_matrix_apply(&w, lx * 20, ly * 20, &ox, &oy); return v2_num(ox / 20); } return v2_num(lx * 0.0 + 0.0); }
static V2 f_stageY(VM2 *vm, V2 self, int argc, V2 *argv) { V2 t = ev_get(vm, self, "target"); double lx = v2_tonum(vm, ev_get(vm, self, "localX")), ly = v2_tonum(vm, ev_get(vm, self, "localY")); (void)argc; (void)argv; if (t.t == T_OBJ && t.u.o->kind == OK_DISPLAY) { SWFMatrix w; double ox, oy; a2_world_matrix(t.u.o, &w); swf_matrix_apply(&w, lx * 20, ly * 20, &ox, &oy); return v2_num(oy / 20); } return v2_num(ly * 0.0 + 0.0); }
static V2 ev_updateAfterEvent(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_undef(); }

static V2 MouseEvent_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *e = SELFO; if (!e) return v2_undef();
    Event_ctor(vm, self, argc > 3 ? 3 : argc, argv); if (argc < 2) ev_set(vm, e, "bubbles", v2_bool(1));
    ev_set(vm, e, "localX", argc > 3 ? ARG(3) : v2_num(0.0 / 0.0)); ev_set(vm, e, "localY", argc > 4 ? ARG(4) : v2_num(0.0 / 0.0)); ev_set(vm, e, "relatedObject", argc > 5 ? ARG(5) : v2_null());
    ev_set(vm, e, "ctrlKey", v2_bool(argc > 6 ? BOOL(6) : 0)); ev_set(vm, e, "altKey", v2_bool(argc > 7 ? BOOL(7) : 0)); ev_set(vm, e, "shiftKey", v2_bool(argc > 8 ? BOOL(8) : 0)); ev_set(vm, e, "buttonDown", v2_bool(argc > 9 ? BOOL(9) : 0)); ev_set(vm, e, "delta", v2_int(argc > 10 ? INT(10) : 0));
    ev_set(vm, e, "movementX", v2_num(0)); ev_set(vm, e, "movementY", v2_num(0)); ev_set(vm, e, "isRelatedObjectInaccessible", v2_bool(0));
    return v2_undef();
}
static V2 KeyboardEvent_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *e = SELFO; if (!e) return v2_undef();
    Event_ctor(vm, self, argc > 3 ? 3 : argc, argv); if (argc < 2) ev_set(vm, e, "bubbles", v2_bool(1));
    ev_set(vm, e, "charCode", v2_uint(argc > 3 ? v2_touint(vm, argv[3]) : 0)); ev_set(vm, e, "keyCode", v2_uint(argc > 4 ? v2_touint(vm, argv[4]) : 0)); ev_set(vm, e, "keyLocation", v2_uint(argc > 5 ? v2_touint(vm, argv[5]) : 0));
    ev_set(vm, e, "ctrlKey", v2_bool(argc > 6 ? BOOL(6) : 0)); ev_set(vm, e, "altKey", v2_bool(argc > 7 ? BOOL(7) : 0)); ev_set(vm, e, "shiftKey", v2_bool(argc > 8 ? BOOL(8) : 0));
    return v2_undef();
}
static V2 TextEvent_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *e = SELFO; if (!e) return v2_undef(); Event_ctor(vm, self, argc > 3 ? 3 : argc, argv); ev_set(vm, e, "text", argc > 3 ? v2_str(STR(3)) : v2_str(vm->s_empty)); return v2_undef(); }
static V2 ErrorEvent_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *e = SELFO; if (!e) return v2_undef(); TextEvent_ctor(vm, self, argc, argv); ev_set(vm, e, "errorID", v2_int(argc > 4 ? INT(4) : 0)); return v2_undef(); }
static V2 ProgressEvent_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *e = SELFO; if (!e) return v2_undef(); Event_ctor(vm, self, argc > 3 ? 3 : argc, argv); ev_set(vm, e, "bytesLoaded", v2_num(argc > 3 ? NUM(3) : 0)); ev_set(vm, e, "bytesTotal", v2_num(argc > 4 ? NUM(4) : 0)); return v2_undef(); }
static V2 NetStatusEvent_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *e = SELFO; if (!e) return v2_undef(); Event_ctor(vm, self, argc > 3 ? 3 : argc, argv); ev_set(vm, e, "info", argc > 3 ? ARG(3) : v2_null()); return v2_undef(); }
static V2 FocusEvent_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *e = SELFO; if (!e) return v2_undef(); Event_ctor(vm, self, argc > 3 ? 3 : argc, argv); ev_set(vm, e, "relatedObject", argc > 3 ? ARG(3) : v2_null()); ev_set(vm, e, "shiftKey", v2_bool(argc > 4 ? BOOL(4) : 0)); ev_set(vm, e, "keyCode", v2_uint(argc > 5 ? v2_touint(vm, argv[5]) : 0)); return v2_undef(); }
static V2 HTTPStatusEvent_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *e = SELFO; if (!e) return v2_undef(); Event_ctor(vm, self, argc > 3 ? 3 : argc, argv); ev_set(vm, e, "status", v2_int(argc > 3 ? INT(3) : 0)); ev_set(vm, e, "redirected", v2_bool(argc > 4 ? BOOL(4) : 0)); ev_set(vm, e, "responseURL", v2_null()); ev_set(vm, e, "responseHeaders", v2_obj(a2_new_array(vm))); return v2_undef(); }
static V2 str_or_null(VM2 *vm, int argc, V2 *argv, int i) { V2 v = ARG(i); if (argc <= i) return v2_str(vm->s_empty); if (v.t == T_NULL || v.t == T_UNDEF) return v2_null(); return v2_str(v2_tostr(vm, v)); }
static V2 StatusEvent_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *e = SELFO; if (!e) return v2_undef(); Event_ctor(vm, self, argc > 3 ? 3 : argc, argv); ev_set(vm, e, "code", str_or_null(vm, argc, argv, 3)); ev_set(vm, e, "level", str_or_null(vm, argc, argv, 4)); return v2_undef(); }
static V2 ErrorEvent_ctor(VM2 *vm, V2 self, int argc, V2 *argv);
static V2 AsyncErrorEvent_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *e = SELFO; if (!e) return v2_undef(); ErrorEvent_ctor(vm, self, argc > 4 ? 4 : argc, argv); ev_set(vm, e, "error", argc > 4 ? ARG(4) : v2_null()); return v2_undef(); }
static V2 FullScreenEvent_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *e = SELFO; if (!e) return v2_undef(); Event_ctor(vm, self, argc > 3 ? 3 : argc, argv); ev_set(vm, e, "fullScreen", v2_bool(argc > 3 ? BOOL(3) : 0)); return v2_undef(); }
static V2 DataEvent_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *e = SELFO; if (!e) return v2_undef(); TextEvent_ctor(vm, self, argc, argv); ev_set(vm, e, "data", argc > 3 ? v2_str(STR(3)) : v2_str(vm->s_empty)); return v2_undef(); }
static V2 SampleDataEvent_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *e = SELFO; if (!e) return v2_undef(); Event_ctor(vm, self, argc > 3 ? 3 : argc, argv); ev_set(vm, e, "position", v2_num(argc > 3 ? NUM(3) : 0)); ev_set(vm, e, "data", argc > 4 ? ARG(4) : v2_null()); return v2_undef(); }
static V2 ActivityEvent_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *e = SELFO; if (!e) return v2_undef(); Event_ctor(vm, self, argc > 3 ? 3 : argc, argv); ev_set(vm, e, "activating", v2_bool(argc > 3 ? BOOL(3) : 0)); return v2_undef(); }

/* ---------------------------------------------------------------- */
/* EventDispatcher                                                  */

int a2_has_listener(A2Obj *o, A2Str *type) { A2Listener *l; for (l = o->listeners; l; l = l->next) if (l->type == type) return 1; return 0; }

static int is_bcast_type(VM2 *vm, A2Str *t) { return t == vm->s_enterFrame || t == vm->s_exitFrame || t == vm->s_frameConstructed || !strcmp(t->s, "render") || !strcmp(t->s, "activate") || !strcmp(t->s, "deactivate"); }
static void bcast_add(VM2 *vm, A2Obj *o, A2Str *type) {
    int i; for (i = 0; i < vm->nbcast; i++) if (vm->bcast[i] == o && vm->bcast_t[i] == type) return;
    if (vm->nbcast == vm->capbcast) {
        int nc = vm->capbcast ? vm->capbcast * 2 : 64;
        A2Obj **nb = (A2Obj**)realloc(vm->bcast, (size_t)nc * sizeof(A2Obj*)); A2Str **nt;
        if (!nb) return; vm->bcast = nb;
        nt = (A2Str**)realloc(vm->bcast_t, (size_t)nc * sizeof(A2Str*)); if (!nt) return; vm->bcast_t = nt;
        vm->capbcast = nc;
    }
    vm->bcast[vm->nbcast] = o; vm->bcast_t[vm->nbcast] = type; vm->nbcast++;
}
static void bcast_remove(VM2 *vm, A2Obj *o, A2Str *type) {
    int i; for (i = 0; i < vm->nbcast; i++) if (vm->bcast[i] == o && vm->bcast_t[i] == type) {
        memmove(vm->bcast + i, vm->bcast + i + 1, (size_t)(vm->nbcast - i - 1) * sizeof(A2Obj*));
        memmove(vm->bcast_t + i, vm->bcast_t + i + 1, (size_t)(vm->nbcast - i - 1) * sizeof(A2Str*));
        vm->nbcast--; return;
    }
}
void a2_add_bcast(VM2 *vm, A2Obj *o) { bcast_add(vm, o, vm->s_enterFrame); }

static V2 ed_addEventListener(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = SELFO; A2Str *type = STR(0); V2 fn = ARG(1); int capture = argc > 2 ? BOOL(2) : 0, prio = argc > 3 ? INT(3) : 0; A2Listener *l, **pp;
    if (!o) return v2_undef();
    if (fn.t != T_OBJ || (fn.u.o->kind != OK_FUNCTION && fn.u.o->kind != OK_CLASS)) { a2_throw_error(vm, "TypeError", "Parameter listener must be non-null.", 2007); return v2_undef(); }
    for (l = o->listeners; l; l = l->next) if (l->type == type && l->capture == capture && v2_strict_eq(vm, l->fn, fn)) return v2_undef();
    l = (A2Listener*)calloc(1, sizeof(A2Listener)); l->fn = fn; l->type = type; l->capture = capture; l->priority = prio;
    /* insert by priority (descending), after existing same-priority entries */
    pp = &o->listeners; while (*pp && (*pp)->priority >= prio) pp = &(*pp)->next;
    l->next = *pp; *pp = l;
    if (is_bcast_type(vm, type)) bcast_add(vm, o, type);
    return v2_undef();
}
static V2 ed_removeEventListener(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = SELFO; A2Str *type = STR(0); V2 fn = ARG(1); int capture = argc > 2 ? BOOL(2) : 0; A2Listener **pp;
    if (!o) return v2_undef();
    for (pp = &o->listeners; *pp; pp = &(*pp)->next) if ((*pp)->type == type && (*pp)->capture == capture && v2_strict_eq(vm, (*pp)->fn, fn)) { A2Listener *l = *pp; *pp = l->next; l->type = NULL; l->next = NULL; /* keep node alive if dispatch in progress: leak tiny */ free(l); break; }
    if (is_bcast_type(vm, type) && !a2_has_listener(o, type)) bcast_remove(vm, o, type);   /* the last listener leaves the broadcast list */
    return v2_undef();
}
static V2 ed_hasEventListener(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = SELFO; return v2_bool(o && a2_has_listener(o, STR(0))); }
static V2 ed_willTrigger(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = SELFO; A2Str *t = STR(0); while (o) { if (a2_has_listener(o, t)) return v2_bool(1); if (o->kind != OK_DISPLAY) break; o = a2_parent_of(vm, o); } return v2_bool(0); }

static void call_listeners(VM2 *vm, A2Obj *node, A2Obj *ev, A2Str *type, int capture) {
    /* snapshot the listener list: handlers may add/remove listeners */
    A2Listener *l; int n = 0, i; V2 *fns; int *caps;
    for (l = node->listeners; l; l = l->next) if (l->type == type) n++;
    if (!n) return;
    fns = (V2*)malloc((size_t)n * sizeof(V2)); caps = (int*)malloc((size_t)n * sizeof(int));
    for (l = node->listeners, i = 0; l; l = l->next) if (l->type == type) { fns[i] = l->fn; caps[i] = l->capture; i++; }
    for (i = 0; i < n; i++) {
        V2 arg[1]; int still = 0; A2Listener *q;
        if (caps[i] != capture) continue;
        for (q = node->listeners; q; q = q->next) if (q->type == type && v2_strict_eq(vm, q->fn, fns[i])) still = 1;
        if (!still) continue;
        ev->ival |= EV_DISPATCHED;
        arg[0] = v2_obj(ev);
        a2_call(vm, fns[i], (fns[i].t == T_OBJ && !fns[i].u.o->is_bound) ? v2_obj(vm->global) : v2_null(), 1, arg);
        if (vm->throwing) { extern void a2_report_uncaught(VM2 *vm); a2_report_uncaught(vm); }
        if (ev->ival & EV_STOP_NOW) break;
    }
    free(fns); free(caps);
}

void a2_dispatch_event_obj(VM2 *vm, A2Obj *target, A2Obj *ev) {
    A2Str *type; A2Obj *path[256]; int n = 0, i, bubbles;
    V2 tv; A2Obj *et = target;
    tv = ev_get(vm, v2_obj(ev), "type"); type = v2_tostr(vm, tv);
    bubbles = v2_tobool(vm, ev_get(vm, v2_obj(ev), "bubbles"));
    ev->ival &= ~(EV_STOP | EV_STOP_NOW);
    { A2Prop *tp = target->kind != OK_DISPLAY && target->dyn ? a2_dyn_find(target, a2_str(vm, "__edTarget")) : NULL; int any;
      if (tp && tp->v.t == T_OBJ) et = tp->v.u.o;
      if (et->kind == OK_DISPLAY) { A2Obj *p = a2_parent_of(vm, et); while (p && n < 256) { path[n++] = p; p = a2_parent_of(vm, p); } }
      /* nobody listening anywhere on the path: the event is left untouched (target stays null) */
      any = a2_has_listener(target, type); for (i = 0; i < n && !any; i++) any = a2_has_listener(path[i], type);
      if (!any) return;
      ev_set(vm, ev, "target", v2_obj(et)); }
    /* capture */
    for (i = n - 1; i >= 0 && !(ev->ival & EV_STOP); i--) { ev_set(vm, ev, "eventPhase", v2_int(1)); ev_set(vm, ev, "currentTarget", v2_obj(path[i])); call_listeners(vm, path[i], ev, type, 1); }
    if (ev->ival & EV_STOP) return;
    ev_set(vm, ev, "eventPhase", v2_int(2)); ev_set(vm, ev, "currentTarget", v2_obj(et));   /* EventDispatcher(target): the target is reported for both */
    call_listeners(vm, target, ev, type, 0);
    if (!bubbles) return;
    for (i = 0; i < n && !(ev->ival & EV_STOP); i++) { ev_set(vm, ev, "eventPhase", v2_int(3)); ev_set(vm, ev, "currentTarget", v2_obj(path[i])); call_listeners(vm, path[i], ev, type, 0); }
}
void a2_dispatch(VM2 *vm, A2Obj *target, A2Obj *event) { a2_dispatch_event_obj(vm, target, event); }
void a2_dispatch_simple(VM2 *vm, A2Obj *target, A2Str *type, int bubbles) {
    A2Obj *e;
    if (!target) return;
    if (!bubbles && !a2_has_listener(target, type)) return;
    e = a2_make_event(vm, vm->c_event, type, bubbles, 0);
    a2_dispatch_event_obj(vm, target, e);
}
static V2 ed_dispatchEvent(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = SELFO, *e; V2 ev = ARG(0);
    if (!o || ev.t != T_OBJ) { a2_throw_error(vm, "TypeError", "Parameter event must be non-null.", 2007); return v2_bool(0); }
    e = ev.u.o;
    {   /* some SWFs rely on the `target` getter being called */
        V2 tg = a2_get_s(vm, ev, "target");
        if (vm->throwing) return v2_bool(0);
        if (!v2_tobool(vm, tg)) goto direct;
    }
    {
        V2 c = a2_call_s(vm, ev, "clone", 0, NULL);
        if (vm->throwing) return v2_bool(0);
        if (c.t != T_OBJ) { a2_throw_error(vm, "TypeError", "Parameter event must be non-null.", 2007); return v2_bool(0); }
        e = c.u.o;
    }
direct:
    if (vm->debugger && vm->depth < 1000) {   /* the debug player shows the glue's native dispatchEventFunction frame */
        static A2Method *fm;
        if (!fm) { fm = (A2Method*)calloc(1, sizeof(A2Method)); if (fm) fm->dbg_name = a2_str(vm, "flash.events::EventDispatcher/dispatchEventFunction"); }
        if (fm) { vm->depth++; vm->cs[vm->depth] = fm; vm->cs_cls[vm->depth] = NULL; vm->cs_file[vm->depth] = NULL; a2_dispatch_event_obj(vm, o, e); vm->depth--; return v2_bool(!(e->ival & EV_PREVENTED)); }
    }
    a2_dispatch_event_obj(vm, o, e);
    return v2_bool(!(e->ival & EV_PREVENTED));
}
/* EventDispatcher.toString(): Object.prototype.toString looked up dynamically (scripts may replace it) */
static V2 ed_toString(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 f; (void)argc; (void)argv;
    f = vm->c_object && vm->c_object->prototype ? a2_get_s(vm, v2_obj(vm->c_object->prototype), "toString") : v2_undef();
    if (f.t == T_OBJ && !vm->throwing) return a2_call(vm, f, self, 0, NULL);
    { char b[200]; sprintf(b, "[object %.120s]", SELFO && SELFO->cls ? SELFO->cls->name->s : "EventDispatcher"); return v2_cstr(vm, b); }
}
/* new EventDispatcher(target): events dispatched through it name `target` and bubble along its parents */
static V2 EventDispatcher_ctor(VM2 *vm, V2 self, int argc, V2 *argv) { if (SELFO && argc > 0 && ARG(0).t == T_OBJ && ARG(0).u.o != SELFO) a2_dyn_set_hidden(vm, SELFO, a2_str(vm, "__edTarget"), ARG(0)); return v2_undef(); }

void a2_broadcast(VM2 *vm, A2Str *type) {
    int i, n = 0; A2Obj **snap;
    if (!vm->nbcast) return;
    snap = (A2Obj**)malloc((size_t)vm->nbcast * sizeof(A2Obj*));
    for (i = 0; i < vm->nbcast; i++) if (vm->bcast_t[i] == type) snap[n++] = vm->bcast[i];
    for (i = 0; i < n; i++) {
        int k, alive = 0;
        for (k = 0; k < vm->nbcast; k++) if (vm->bcast[k] == snap[i]) { alive = 1; break; }   /* a GC in a listener may have freed it */
        if (alive && a2_has_listener(snap[i], type)) {   /* broadcast events have only a target phase: capture listeners never see them */
            A2Obj *e = a2_make_event(vm, vm->c_event, type, 0, 0);
            ev_set(vm, e, "target", v2_obj(snap[i])); ev_set(vm, e, "currentTarget", v2_obj(snap[i])); ev_set(vm, e, "eventPhase", v2_int(2));
            call_listeners(vm, snap[i], e, type, 0);
        }
    }
    free(snap);
}
void a2_prune_bcast(VM2 *vm) { int i, j = 0; for (i = 0; i < vm->nbcast; i++) if (vm->bcast[i]->gc.mark) { vm->bcast_t[j] = vm->bcast_t[i]; vm->bcast[j++] = vm->bcast[i]; } vm->nbcast = j; }

/* ---------------------------------------------------------------- */
/* Timer                                                            */

static V2 Timer_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *t = SELFO; if (!t) return v2_undef();
    t->kind = OK_TIMER; t->dval = NUM(0); t->ival = argc > 1 ? INT(1) : 0;
    a2_dyn_set(vm, t, a2_str(vm, "__count"), v2_int(0)); a2_dyn_set(vm, t, a2_str(vm, "__running"), v2_bool(0)); a2_dyn_set(vm, t, a2_str(vm, "__acc"), v2_num(0));
    if (!(t->dval >= 0 && t->dval <= 2147483647.0)) { a2_throw_id(vm, "RangeError", 2066, NULL, NULL); }
    return v2_undef();
}
static void timer_register(VM2 *vm, A2Obj *t) { int i; for (i = 0; i < vm->ntimers; i++) if (vm->timers[i].timer == t) return; if (vm->ntimers < 128) vm->timers[vm->ntimers++].timer = t; }
static void timer_unregister(VM2 *vm, A2Obj *t) { int i; for (i = 0; i < vm->ntimers; i++) if (vm->timers[i].timer == t) { memmove(&vm->timers[i], &vm->timers[i + 1], (size_t)(vm->ntimers - i - 1) * sizeof(vm->timers[0])); vm->ntimers--; return; } }
static V2 tm_start(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *t = SELFO; (void)argc; (void)argv; if (!t) return v2_undef(); a2_dyn_set(vm, t, a2_str(vm, "__running"), v2_bool(1)); a2_dyn_set(vm, t, a2_str(vm, "__acc"), v2_num(0)); timer_register(vm, t); return v2_undef(); }
static V2 tm_stop(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *t = SELFO; (void)argc; (void)argv; if (!t) return v2_undef(); a2_dyn_set(vm, t, a2_str(vm, "__running"), v2_bool(0)); timer_unregister(vm, t); return v2_undef(); }
static V2 tm_reset(VM2 *vm, V2 self, int argc, V2 *argv) { tm_stop(vm, self, argc, argv); if (SELFO) a2_dyn_set(vm, SELFO, a2_str(vm, "__count"), v2_int(0)); return v2_undef(); }
static V2 tm_delay_get(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_num(SELFO ? SELFO->dval : 0); }
static V2 tm_delay_set(VM2 *vm, V2 self, int argc, V2 *argv) { double d = NUM(0); if (!(d >= 0 && d <= 2147483647.0)) { a2_throw_id(vm, "RangeError", 2066, NULL, NULL); return v2_undef(); } if (SELFO) { SELFO->dval = d; a2_dyn_set(vm, SELFO, a2_str(vm, "__acc"), v2_num(0)); } return v2_undef(); }   /* a new delay restarts the countdown */
static V2 tm_repeat_get(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_int(SELFO ? SELFO->ival : 0); }
static V2 tm_repeat_set(VM2 *vm, V2 self, int argc, V2 *argv) { if (SELFO) { SELFO->ival = INT(0); if (SELFO->ival > 0 && v2_toint(vm, a2_get_s(vm, self, "__count")) >= SELFO->ival) tm_stop(vm, self, 0, NULL); } return v2_undef(); }
static V2 tm_currentCount(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return a2_get_s(vm, self, "__count"); }
static V2 tm_running(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return v2_bool(v2_tobool(vm, a2_get_s(vm, self, "__running"))); }

static void timer_fire(VM2 *vm, A2Obj *t) {
    int count = v2_toint(vm, a2_get_s(vm, v2_obj(t), "__count")) + 1;
    A2Prop *fnp = a2_dyn_find(t, a2_str(vm, "__fn"));
    a2_dyn_set(vm, t, a2_str(vm, "__count"), v2_int(count));
    if (fnp) {
        A2Prop *ap = a2_dyn_find(t, a2_str(vm, "__args")); V2 r;
        if (t->ival == 1) { tm_stop(vm, v2_obj(t), 0, NULL); }
        { int na = ap && ap->v.t == T_OBJ ? ap->v.u.o->alen : 0; V2 *av = ap && ap->v.t == T_OBJ ? ap->v.u.o->arr : NULL; V2 pad[16]; int np = 0;
          /* the callback gets missing parameters as undefined (no argument count error) */
          if (fnp->v.t == T_OBJ && fnp->v.u.o->kind == OK_FUNCTION && fnp->v.u.o->method) np = fnp->v.u.o->method->nparams;
          if (np > na && np <= 16) { int k; for (k = 0; k < np; k++) pad[k] = k < na ? av[k] : v2_undef(); na = np; av = pad; }
          { extern const char *g_a2_stack_base; const char *ob = g_a2_stack_base;   /* Flash runs the callback from SetIntervalTimer through Function.apply */
            g_a2_stack_base = "\n\tat Function/http://adobe.com/AS3/2006/builtin::apply()\n\tat SetIntervalTimer/onTimer()\n\tat flash.utils::Timer/_timerDispatch()\n\tat flash.utils::Timer/tick()";
            r = a2_call(vm, fnp->v, v2_null(), na, av);
            if (vm->throwing) { extern void a2_report_uncaught(VM2 *vm); a2_report_uncaught(vm); }
            g_a2_stack_base = ob; } }
        (void)r;
        if (vm->throwing) { extern void a2_report_uncaught(VM2 *vm); a2_report_uncaught(vm); }
        return;
    }
    { A2Obj *e = a2_make_event(vm, vm->c_timerevent, vm->s_timer, 0, 0); a2_dispatch_event_obj(vm, t, e); }
    if (t->ival > 0 && count >= t->ival) { tm_stop(vm, v2_obj(t), 0, NULL); { A2Obj *e = a2_make_event(vm, vm->c_timerevent, vm->s_timerComplete, 0, 0); a2_dispatch_event_obj(vm, t, e); } }
}
void a2_timers_tick(VM2 *vm, double ms) {
    int i; A2Obj *snap[128]; int n = vm->ntimers;
    memcpy(snap, vm->timers, sizeof(snap[0]) * 0);
    for (i = 0; i < n; i++) snap[i] = vm->timers[i].timer;
    for (i = 0; i < n; i++) {
        A2Obj *t = snap[i]; double acc; int guard = 0;
        if (!v2_tobool(vm, a2_get_s(vm, v2_obj(t), "__running"))) continue;
        acc = v2_tonum(vm, a2_get_s(vm, v2_obj(t), "__acc")) + ms;
        while (acc >= t->dval && guard++ < 50 && v2_tobool(vm, a2_get_s(vm, v2_obj(t), "__running"))) { acc -= t->dval > 0 ? t->dval : ms; timer_fire(vm, t); if (t->dval <= 0) { acc = 0; break; } }
        a2_dyn_set(vm, t, a2_str(vm, "__acc"), v2_num(acc < 0 ? 0 : acc));
    }
}
V2 a2_set_timeout(VM2 *vm, V2 fn, double delay, int repeat, int argc, V2 *argv) {
    A2Obj *t = a2_new(vm, vm->c_timer); A2Obj *args = a2_new_array(vm); int i; static int next_id = 1;
    t->kind = OK_TIMER; t->dval = delay < 0 ? 0 : delay; t->ival = repeat ? 0 : 1;
    a2_dyn_set(vm, t, a2_str(vm, "__count"), v2_int(0)); a2_dyn_set(vm, t, a2_str(vm, "__running"), v2_bool(1)); a2_dyn_set(vm, t, a2_str(vm, "__acc"), v2_num(0));
    for (i = 0; i < argc; i++) a2_arr_push(vm, args, argv[i]);
    a2_dyn_set(vm, t, a2_str(vm, "__fn"), fn); a2_dyn_set(vm, t, a2_str(vm, "__args"), v2_obj(args)); a2_dyn_set(vm, t, a2_str(vm, "__id"), v2_int(next_id));
    timer_register(vm, t);
    return v2_uint((u32)next_id++);
}
void a2_clear_timeout(VM2 *vm, int id) { int i; for (i = 0; i < vm->ntimers; i++) { A2Prop *p = a2_dyn_find(vm->timers[i].timer, a2_str(vm, "__id")); if (p && v2_toint(vm, p->v) == id) { timer_unregister(vm, vm->timers[i].timer); return; } } }

/* ---------------------------------------------------------------- */

static A2Class *evclass(VM2 *vm, const char *name, A2Class *super, NativeFn2 ctor) { A2Class *c = a2_def_class(vm, "flash.events", name, super, OK_EVENT, ctor); return c; }

void a2_flash_events_init(VM2 *vm) {
    A2Class *c, *e, *te;
    vm->c_eventdispatcher = c = a2_def_class(vm, "flash.events", "EventDispatcher", vm->c_object, OK_PLAIN, EventDispatcher_ctor);
    a2_add_method(vm, c, "addEventListener", ed_addEventListener, 5); a2_add_method(vm, c, "removeEventListener", ed_removeEventListener, 3); a2_add_method(vm, c, "dispatchEvent", ed_dispatchEvent, 1);
    a2_add_method(vm, c, "hasEventListener", ed_hasEventListener, 1); a2_add_method(vm, c, "willTrigger", ed_willTrigger, 1); a2_add_method(vm, c, "toString", ed_toString, 0);
    c = a2_def_class(vm, "flash.events", "IEventDispatcher", vm->c_object, OK_PLAIN, NULL); c->flags = CF_INTERFACE;

    vm->c_event = e = evclass(vm, "Event", vm->c_object, Event_ctor);
    a2_add_getter(vm, e, "type", ev_type, NULL); a2_add_getter(vm, e, "bubbles", ev_bubbles, NULL); a2_add_getter(vm, e, "cancelable", ev_cancelable, NULL); a2_add_getter(vm, e, "target", ev_target, NULL); a2_add_getter(vm, e, "currentTarget", ev_currentTarget, NULL); a2_add_getter(vm, e, "eventPhase", ev_eventPhase, NULL);
    a2_add_method(vm, e, "stopPropagation", ev_stopPropagation, 0); a2_add_method(vm, e, "stopImmediatePropagation", ev_stopImmediatePropagation, 0); a2_add_method(vm, e, "preventDefault", ev_preventDefault, 0); a2_add_method(vm, e, "isDefaultPrevented", ev_isDefaultPrevented, 0);
    a2_add_method(vm, e, "clone", ev_clone, 0); a2_add_method(vm, e, "formatToString", ev_formatToString, 1); a2_add_method(vm, e, "toString", ev_toString, 0);
    a2_dyn_set(vm, e->prototype, vm->s_toString, v2_obj(a2_new_native_fn(vm, ev_toString, "toString", 0)));
    { extern void a2_sig(VM2 *vm, A2Class *c, const char *name, const char *sig); e->ctor_sig = "(String,Boolean=,Boolean=)";
      a2_sig(vm, e, "type", "String"); a2_sig(vm, e, "bubbles", "Boolean"); a2_sig(vm, e, "cancelable", "Boolean"); a2_sig(vm, e, "target", "Object"); a2_sig(vm, e, "currentTarget", "Object"); a2_sig(vm, e, "eventPhase", "uint");
      a2_sig(vm, e, "stopPropagation", "void()"); a2_sig(vm, e, "stopImmediatePropagation", "void()"); a2_sig(vm, e, "preventDefault", "void()"); a2_sig(vm, e, "isDefaultPrevented", "Boolean()");
      a2_sig(vm, e, "clone", "flash.events::Event()"); a2_sig(vm, e, "formatToString", "String(String)"); a2_sig(vm, e, "toString", "String()"); }
    { const char *k[] = { "ACTIVATE", "activate", "ADDED", "added", "ADDED_TO_STAGE", "addedToStage", "CANCEL", "cancel", "CHANGE", "change", "CLEAR", "clear", "CLOSE", "close", "COMPLETE", "complete", "CONNECT", "connect", "COPY", "copy", "CUT", "cut", "DEACTIVATE", "deactivate", "ENTER_FRAME", "enterFrame", "EXIT_FRAME", "exitFrame", "FRAME_CONSTRUCTED", "frameConstructed", "FULLSCREEN", "fullScreen", "ID3", "id3", "INIT", "init", "MOUSE_LEAVE", "mouseLeave", "OPEN", "open", "PASTE", "paste", "REMOVED", "removed", "REMOVED_FROM_STAGE", "removedFromStage", "RENDER", "render", "RESIZE", "resize", "SCROLL", "scroll", "SELECT", "select", "SELECT_ALL", "selectAll", "SOUND_COMPLETE", "soundComplete", "TAB_CHILDREN_CHANGE", "tabChildrenChange", "TAB_ENABLED_CHANGE", "tabEnabledChange", "TAB_INDEX_CHANGE", "tabIndexChange", "UNLOAD", "unload", "TEXT_INTERACTION_MODE_CHANGE", "textInteractionModeChange", "CONTEXT3D_CREATE", "context3DCreate", "FRAME_LABEL", "frameLabel", "CHANNEL_MESSAGE", "channelMessage", "CHANNEL_STATE", "channelState", "VIDEO_FRAME", "videoFrame", "WORKER_STATE", "workerState", "SUSPEND", "suspend", "EXITING", "exiting", "BROWSER_ZOOM_CHANGE", "browserZoomChange", NULL }; int i; for (i = 0; k[i]; i += 2) {   /* [API] versioned names (Flash Player 11.4 / AIR only) */
        int sv = vm->air ? 99 : vm->swf_version;
        if (sv < 17 && (!strcmp(k[i], "VIDEO_FRAME") || !strncmp(k[i], "CHANNEL_", 8) || !strcmp(k[i], "WORKER_STATE"))) continue;
        if (sv < 10 && !strcmp(k[i], "CONTEXT3D_CREATE")) continue;
        if (sv != 99 && (!strcmp(k[i], "SUSPEND") || !strcmp(k[i], "EXITING"))) continue;
        a2_const_str(vm, e, k[i], k[i + 1]); } }

    vm->c_mouseevent = c = evclass(vm, "MouseEvent", e, MouseEvent_ctor);
    a2_add_getter(vm, c, "localX", f_localX_get, f_localX_set); a2_add_getter(vm, c, "localY", f_localY_get, f_localY_set); a2_add_getter(vm, c, "stageX", f_stageX, NULL); a2_add_getter(vm, c, "stageY", f_stageY, NULL);
    a2_add_getter(vm, c, "buttonDown", f_buttonDown_get, f_buttonDown_set); a2_add_getter(vm, c, "delta", f_delta_get, f_delta_set); a2_add_getter(vm, c, "ctrlKey", f_ctrlKey_get, f_ctrlKey_set); a2_add_getter(vm, c, "altKey", f_altKey_get, f_altKey_set); a2_add_getter(vm, c, "shiftKey", f_shiftKey_get, f_shiftKey_set);
    a2_add_getter(vm, c, "relatedObject", f_relatedObject_get, f_relatedObject_set); a2_add_getter(vm, c, "movementX", f_movementX_get, f_movementX_set); a2_add_getter(vm, c, "movementY", f_movementY_get, f_movementY_set); a2_add_getter(vm, c, "isRelatedObjectInaccessible", f_isRelatedObjectInaccessible_get, f_isRelatedObjectInaccessible_set);
    a2_add_method(vm, c, "updateAfterEvent", ev_updateAfterEvent, 0);
    { const char *k[] = { "CLICK", "click", "DOUBLE_CLICK", "doubleClick", "MOUSE_DOWN", "mouseDown", "MOUSE_MOVE", "mouseMove", "MOUSE_OUT", "mouseOut", "MOUSE_OVER", "mouseOver", "MOUSE_UP", "mouseUp", "MOUSE_WHEEL", "mouseWheel", "ROLL_OUT", "rollOut", "ROLL_OVER", "rollOver", "RIGHT_CLICK", "rightClick", "RIGHT_MOUSE_DOWN", "rightMouseDown", "RIGHT_MOUSE_UP", "rightMouseUp", "MIDDLE_CLICK", "middleClick", "MIDDLE_MOUSE_DOWN", "middleMouseDown", "MIDDLE_MOUSE_UP", "middleMouseUp", "CONTEXT_MENU", "contextMenu", "RELEASE_OUTSIDE", "releaseOutside", NULL }; int i; for (i = 0; k[i]; i += 2) a2_const_str(vm, c, k[i], k[i + 1]); }
    vm->c_keyboardevent = c = evclass(vm, "KeyboardEvent", e, KeyboardEvent_ctor);
    a2_add_getter(vm, c, "charCode", f_charCode_get, f_charCode_set); a2_add_getter(vm, c, "keyCode", f_keyCode_get, f_keyCode_set); a2_add_getter(vm, c, "keyLocation", f_keyLocation_get, f_keyLocation_set);
    a2_add_getter(vm, c, "ctrlKey", f_ctrlKey_get, f_ctrlKey_set); a2_add_getter(vm, c, "altKey", f_altKey_get, f_altKey_set); a2_add_getter(vm, c, "shiftKey", f_shiftKey_get, f_shiftKey_set); a2_add_method(vm, c, "updateAfterEvent", ev_updateAfterEvent, 0);
    a2_const_str(vm, c, "KEY_DOWN", "keyDown"); a2_const_str(vm, c, "KEY_UP", "keyUp");
    vm->c_timerevent = c = evclass(vm, "TimerEvent", e, Event_ctor); a2_const_str(vm, c, "TIMER", "timer"); a2_const_str(vm, c, "TIMER_COMPLETE", "timerComplete"); a2_add_method(vm, c, "updateAfterEvent", ev_updateAfterEvent, 0);
    vm->c_textevent = te = evclass(vm, "TextEvent", e, TextEvent_ctor); a2_add_getter(vm, te, "text", f_text_get, f_text_set); a2_const_str(vm, te, "LINK", "link"); a2_const_str(vm, te, "TEXT_INPUT", "textInput");
    c = evclass(vm, "ErrorEvent", te, ErrorEvent_ctor); a2_add_getter(vm, c, "errorID", f_errorID_get, NULL); a2_const_str(vm, c, "ERROR", "error");
    { A2Class *ee = c; c = evclass(vm, "IOErrorEvent", ee, ErrorEvent_ctor); a2_const_str(vm, c, "IO_ERROR", "ioError"); a2_const_str(vm, c, "NETWORK_ERROR", "networkError"); a2_const_str(vm, c, "DISK_ERROR", "diskError"); a2_const_str(vm, c, "VERIFY_ERROR", "verifyError");
      c = evclass(vm, "SecurityErrorEvent", ee, ErrorEvent_ctor); a2_const_str(vm, c, "SECURITY_ERROR", "securityError");
      c = evclass(vm, "AsyncErrorEvent", ee, AsyncErrorEvent_ctor); a2_add_getter(vm, c, "error", f_error_get, f_error_set); a2_const_str(vm, c, "ASYNC_ERROR", "asyncError");
      c = evclass(vm, "UncaughtErrorEvent", ee, ErrorEvent_ctor); a2_const_str(vm, c, "UNCAUGHT_ERROR", "uncaughtError"); }
    c = evclass(vm, "ProgressEvent", e, ProgressEvent_ctor); a2_add_getter(vm, c, "bytesLoaded", f_bytesLoaded_get, f_bytesLoaded_set); a2_add_getter(vm, c, "bytesTotal", f_bytesTotal_get, f_bytesTotal_set); a2_const_str(vm, c, "PROGRESS", "progress"); a2_const_str(vm, c, "SOCKET_DATA", "socketData");
    c = evclass(vm, "NetStatusEvent", e, NetStatusEvent_ctor); a2_add_getter(vm, c, "info", f_info_get, f_info_set); a2_const_str(vm, c, "NET_STATUS", "netStatus");
    c = evclass(vm, "FocusEvent", e, FocusEvent_ctor); a2_add_getter(vm, c, "relatedObject", f_relatedObject_get, f_relatedObject_set); a2_add_getter(vm, c, "shiftKey", f_shiftKey_get, f_shiftKey_set); a2_add_getter(vm, c, "keyCode", f_keyCode_get, f_keyCode_set);
    a2_const_str(vm, c, "FOCUS_IN", "focusIn"); a2_const_str(vm, c, "FOCUS_OUT", "focusOut"); a2_const_str(vm, c, "KEY_FOCUS_CHANGE", "keyFocusChange"); a2_const_str(vm, c, "MOUSE_FOCUS_CHANGE", "mouseFocusChange");
    c = evclass(vm, "HTTPStatusEvent", e, HTTPStatusEvent_ctor); a2_add_getter(vm, c, "status", f_status_get, NULL); a2_add_getter(vm, c, "redirected", f_redirected_get, f_redirected_set); a2_add_getter(vm, c, "responseURL", f_responseURL_get, f_responseURL_set); a2_add_getter(vm, c, "responseHeaders", f_responseHeaders_get, f_responseHeaders_set); a2_const_str(vm, c, "HTTP_STATUS", "httpStatus"); a2_const_str(vm, c, "HTTP_RESPONSE_STATUS", "httpResponseStatus");
    c = evclass(vm, "StatusEvent", e, StatusEvent_ctor); a2_add_getter(vm, c, "code", f_code_get, f_code_set); a2_add_getter(vm, c, "level", f_level_get, f_level_set); a2_const_str(vm, c, "STATUS", "status");
    c = evclass(vm, "FullScreenEvent", e, FullScreenEvent_ctor); a2_add_getter(vm, c, "fullScreen", f_fullScreen_get, NULL); a2_const_str(vm, c, "FULL_SCREEN", "fullScreen"); a2_const_str(vm, c, "FULL_SCREEN_INTERACTIVE_ACCEPTED", "fullScreenInteractiveAccepted");
    c = evclass(vm, "DataEvent", te, DataEvent_ctor); a2_add_getter(vm, c, "data", f_data_get, f_data_set); a2_const_str(vm, c, "DATA", "data"); a2_const_str(vm, c, "UPLOAD_COMPLETE_DATA", "uploadCompleteData");
    c = evclass(vm, "SampleDataEvent", e, SampleDataEvent_ctor); a2_add_getter(vm, c, "position", f_position_get, f_position_set); a2_add_getter(vm, c, "data", f_data_get, f_data_set); a2_const_str(vm, c, "SAMPLE_DATA", "sampleData");
    c = evclass(vm, "ActivityEvent", e, ActivityEvent_ctor); a2_add_getter(vm, c, "activating", f_activating_get, f_activating_set); a2_const_str(vm, c, "ACTIVITY", "activity");
    c = evclass(vm, "ContextMenuEvent", e, Event_ctor); a2_const_str(vm, c, "MENU_ITEM_SELECT", "menuItemSelect"); a2_const_str(vm, c, "MENU_SELECT", "menuSelect");
    c = evclass(vm, "SyncEvent", e, Event_ctor); a2_const_str(vm, c, "SYNC", "sync");
    c = evclass(vm, "GestureEvent", e, Event_ctor); c = evclass(vm, "TransformGestureEvent", c, Event_ctor); c = evclass(vm, "TouchEvent", e, Event_ctor); a2_const_str(vm, c, "TOUCH_BEGIN", "touchBegin"); a2_const_str(vm, c, "TOUCH_END", "touchEnd"); a2_const_str(vm, c, "TOUCH_MOVE", "touchMove"); a2_const_str(vm, c, "TOUCH_TAP", "touchTap");
    c = evclass(vm, "ShaderEvent", e, Event_ctor); c = evclass(vm, "IMEEvent", te, TextEvent_ctor); c = evclass(vm, "AccelerometerEvent", e, Event_ctor); c = evclass(vm, "GeolocationEvent", e, Event_ctor); c = evclass(vm, "StageVideoEvent", e, Event_ctor); c = evclass(vm, "StageVideoAvailabilityEvent", e, Event_ctor); c = evclass(vm, "VideoEvent", e, Event_ctor); c = evclass(vm, "OutputProgressEvent", e, Event_ctor); c = evclass(vm, "DRMErrorEvent", e, Event_ctor); c = evclass(vm, "SoftKeyboardEvent", e, Event_ctor); c = evclass(vm, "PressAndTapGestureEvent", e, Event_ctor); c = evclass(vm, "AVHTTPStatusEvent", e, Event_ctor);
    c = evclass(vm, "EventPhase", vm->c_object, NULL); a2_add_const(vm, c, "CAPTURING_PHASE", v2_uint(1)); a2_add_const(vm, c, "AT_TARGET", v2_uint(2)); a2_add_const(vm, c, "BUBBLING_PHASE", v2_uint(3));

    vm->c_timer = c = a2_def_class(vm, "flash.utils", "Timer", vm->c_eventdispatcher, OK_TIMER, Timer_ctor);
    a2_add_method(vm, c, "start", tm_start, 0); a2_add_method(vm, c, "stop", tm_stop, 0); a2_add_method(vm, c, "reset", tm_reset, 0);
    a2_add_getter(vm, c, "delay", tm_delay_get, tm_delay_set); a2_add_getter(vm, c, "repeatCount", tm_repeat_get, tm_repeat_set); a2_add_getter(vm, c, "currentCount", tm_currentCount, NULL); a2_add_getter(vm, c, "running", tm_running, NULL);
}
