/* avm2_natives.c - C bodies of the `native` methods declared by the built-in
 * ActionScript library (as3/, from Ruffle's playerglobal).  Each is
 * registered under "pkg::Class/name" (see a2_register_native); arguments
 * arrive already coerced and defaulted per the AS signature. */
#include "avm2_flash_int.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* per-object native state: hidden properties */
static V2 hget(VM2 *vm, V2 self, const char *k, V2 def) { A2Prop *p = self.t == T_OBJ ? a2_dyn_find(self.u.o, a2_str(vm, k)) : NULL; return p ? p->v : def; }
static void hset(VM2 *vm, V2 self, const char *k, V2 v) { if (self.t == T_OBJ) a2_dyn_set_hidden(vm, self.u.o, a2_str(vm, k), v); }
static int non_null(VM2 *vm, V2 v, const char *name) { if (v.t == T_NULL || v.t == T_UNDEF) { a2_throw_id(vm, "TypeError", 2007, name, NULL); return 0; } return 1; }
static int one_of(const A2Str *s, const char *const *vals) { int i; for (i = 0; vals[i]; i++) if (!strcmp(s->s, vals[i])) return i; return -1; }

/* ---- flash.text.engine.TabStop ---- */
static const char *const tab_align[] = { "start", "center", "end", "decimal", NULL };
static V2 ts_get_alignment(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return hget(vm, self, "__alignment", v2_cstr(vm, "start")); }
static V2 ts_set_alignment(VM2 *vm, V2 self, int argc, V2 *argv) {
    if (!non_null(vm, ARG(0), "alignment")) return v2_undef();
    if (one_of(STR(0), tab_align) < 0) { a2_throw_id(vm, "ArgumentError", 2008, "alignment", NULL); return v2_undef(); }
    hset(vm, self, "__alignment", v2_str(STR(0)));
    return v2_undef();
}
static V2 ts_get_position(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return hget(vm, self, "__position", v2_num(0)); }
static V2 ts_set_position(VM2 *vm, V2 self, int argc, V2 *argv) { double v = NUM(0); if (v < 0) { a2_throw_id(vm, "ArgumentError", 2004, NULL, NULL); return v2_undef(); } hset(vm, self, "__position", v2_num(v)); return v2_undef(); }
static V2 ts_get_token(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return hget(vm, self, "__token", v2_str(vm->s_empty)); }
static V2 ts_set_token(VM2 *vm, V2 self, int argc, V2 *argv) { if (!non_null(vm, ARG(0), "decimalAlignmentToken")) return v2_undef(); hset(vm, self, "__token", v2_str(STR(0))); return v2_undef(); }

void a2_natives_init(VM2 *vm) {
    static int done;
    (void)vm;
    if (done) return;
    done = 1;
    { extern void a2_date_init_tz(void); a2_date_init_tz(); }
    { extern void a2_fte_init(VM2 *vm); a2_fte_init(vm); }
    { extern void a2_netstream_natives_init(void); a2_netstream_natives_init(); { extern void a2_socket_natives_init(void); a2_socket_natives_init(); } { extern void a2_fileref_natives_init(void); a2_fileref_natives_init(); } { extern void a2_localconn_natives_init(void); a2_localconn_natives_init(); } { extern void a2_netconn_natives_init(void); a2_netconn_natives_init(); } { extern void a2_extif_natives_init(void); a2_extif_natives_init(); } { extern void a2_memory_natives_init(void); a2_memory_natives_init(); } { extern void a2_collator_natives_init(void); a2_collator_natives_init(); } }
    a2_register_native("flash.text.engine::TabStop/get alignment", ts_get_alignment);
    a2_register_native("flash.text.engine::TabStop/set alignment", ts_set_alignment);
    a2_register_native("flash.text.engine::TabStop/get position", ts_get_position);
    a2_register_native("flash.text.engine::TabStop/set position", ts_set_position);
    a2_register_native("flash.text.engine::TabStop/get decimalAlignmentToken", ts_get_token);
    a2_register_native("flash.text.engine::TabStop/set decimalAlignmentToken", ts_set_token);
}

/* ---- flash.globalization.Collator (Ruffle's collator.rs: en-US, case-insensitive
   UTF-16 order, then lower case before upper case unless ignoreCase) ---- */
#include "casemap.h"
extern int a2_utf8_decode(const char *s, u32 len, u32 **out);
enum { CO_IGNORE_CASE = 1, CO_WIDTH = 2, CO_DIACRITICS = 4, CO_KANA = 8, CO_SYMBOLS = 16, CO_NUMERIC = 32 };
static int co_opts(VM2 *vm, V2 self) { A2Prop *p = self.t == T_OBJ ? a2_dyn_find(self.u.o, a2_str(vm, "__co_opts")) : NULL; return p ? v2_toint(vm, p->v) : 0; }
static void co_set(VM2 *vm, V2 self, const char *k, V2 v) { if (self.t == T_OBJ) a2_dyn_set_hidden(vm, self.u.o, a2_str(vm, k), v); }
static A2Str *co_nonnull(VM2 *vm, V2 v, const char *name) { if (v.t == T_NULL || v.t == T_UNDEF) { a2_throw_id(vm, "TypeError", 2007, name, NULL); return NULL; } return v2_tostr(vm, v); }
static V2 co_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Str *loc = co_nonnull(vm, ARG(0), "requestedLocaleIDName"), *mode;
    if (!loc) return v2_undef();
    mode = co_nonnull(vm, ARG(1), "initialMode");
    if (!mode) return v2_undef();
    if (strcmp(mode->s, "sorting") && strcmp(mode->s, "matching")) { a2_throw_id(vm, "ArgumentError", 1508, "initialMode", NULL); return v2_undef(); }
    co_set(vm, self, "__co_req", v2_str(loc));
    co_set(vm, self, "__co_opts", v2_int(!strcmp(mode->s, "matching") ? (CO_IGNORE_CASE | CO_WIDTH | CO_DIACRITICS | CO_KANA | CO_SYMBOLS) : 0));
    co_set(vm, self, "__co_last", v2_cstr(vm, "noError"));
    return v2_undef();
}
static V2 co_actual(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_cstr(vm, "en-US"); }
static V2 co_req(VM2 *vm, V2 self, int argc, V2 *argv) { A2Prop *p; UNUSED; p = self.t == T_OBJ ? a2_dyn_find(self.u.o, a2_str(vm, "__co_req")) : NULL; return p ? p->v : v2_null(); }
static V2 co_last(VM2 *vm, V2 self, int argc, V2 *argv) { A2Prop *p; UNUSED; p = self.t == T_OBJ ? a2_dyn_find(self.u.o, a2_str(vm, "__co_last")) : NULL; return p ? p->v : v2_cstr(vm, "noError"); }
static V2 co_get_flag(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_bool((co_opts(vm, self) & (int)(size_t)vm->native_data) != 0); }
static V2 co_set_flag(VM2 *vm, V2 self, int argc, V2 *argv) {
    int f = (int)(size_t)vm->native_data, o = co_opts(vm, self);
    o = v2_tobool(vm, ARG(0)) ? (o | f) : (o & ~f);
    co_set(vm, self, "__co_opts", v2_int(o));
    co_set(vm, self, "__co_last", v2_cstr(vm, "unsupportedError"));
    return v2_undef();
}
static int collate(VM2 *vm, int opts, A2Str *a, A2Str *b) {
    u32 *ua, *ub; int na = a2_utf8_decode(a->s, a->len, &ua), nb = a2_utf8_decode(b->s, b->len, &ub), i, r = 0;
    for (i = 0; i < na && i < nb && !r; i++) { u32 x = ua[i] < 0x10000 ? cm_to_lower((u16)ua[i]) : ua[i], y = ub[i] < 0x10000 ? cm_to_lower((u16)ub[i]) : ub[i]; if (x != y) r = x < y ? -1 : 1; }
    if (!r && na != nb) r = na < nb ? -1 : 1;
    if (!r && !(opts & CO_IGNORE_CASE)) {
        for (i = 0; i < na && i < nb; i++) if (ua[i] != ub[i]) {
            int la = ua[i] < 0x10000 && cm_to_lower((u16)ua[i]) == ua[i], lb = ub[i] < 0x10000 && cm_to_lower((u16)ub[i]) == ub[i];
            r = la && !lb ? -1 : !la && lb ? 1 : ua[i] < ub[i] ? -1 : 1;
            break;
        }
    }
    free(ua); free(ub); (void)vm;
    return r;
}
static V2 co_compare(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Str *a = co_nonnull(vm, ARG(0), "string1"), *b;
    if (!a) return v2_undef();
    b = co_nonnull(vm, ARG(1), "string2");
    if (!b) return v2_undef();
    return v2_int(collate(vm, co_opts(vm, self), a, b));
}
static V2 co_equals(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Str *a = co_nonnull(vm, ARG(0), "string1"), *b;
    if (!a) return v2_undef();
    b = co_nonnull(vm, ARG(1), "string2");
    if (!b) return v2_undef();
    return v2_bool(collate(vm, co_opts(vm, self), a, b) == 0);
}
static V2 co_locales(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 v = a2_construct_class(vm, a2_class_find_s(vm, "__AS3__.vec::Vector$String") ? a2_class_find_s(vm, "__AS3__.vec::Vector$String") : vm->c_vector, 0, NULL); UNUSED;
    if (v.t == T_OBJ) { V2 s = v2_cstr(vm, "en-US"); a2_call_s(vm, v, "push", 1, &s); }
    return v;
}
void a2_collator_natives_init(void) {
#define CO "flash.globalization::Collator/"
    static const struct { const char *n; int f; } fl[] = { { "ignoreCase", CO_IGNORE_CASE }, { "ignoreCharacterWidth", CO_WIDTH }, { "ignoreDiacritics", CO_DIACRITICS }, { "ignoreKanaType", CO_KANA }, { "ignoreSymbols", CO_SYMBOLS }, { "numericComparison", CO_NUMERIC }, { NULL, 0 } };
    static char keys[12][80]; int i;
    a2_register_native(CO "ctor", co_ctor);
    a2_register_native(CO "get actualLocaleIDName", co_actual);
    a2_register_native(CO "get requestedLocaleIDName", co_req);
    a2_register_native(CO "get lastOperationStatus", co_last);
    a2_register_native(CO "compare", co_compare);
    a2_register_native(CO "equals", co_equals);
    a2_register_native("flash.globalization::Collator$/getAvailableLocaleIDNames", co_locales);
    for (i = 0; fl[i].n; i++) {
        sprintf(keys[2 * i], CO "get %s", fl[i].n); a2_register_native_data(keys[2 * i], co_get_flag, (const void*)(size_t)fl[i].f);
        sprintf(keys[2 * i + 1], CO "set %s", fl[i].n); a2_register_native_data(keys[2 * i + 1], co_set_flag, (const void*)(size_t)fl[i].f);
    }
}
