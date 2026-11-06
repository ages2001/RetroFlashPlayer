/* avm2_bitmapdata.c - flash.display.BitmapData for AVM2.
 *
 * The pixels live in the shared BitmapData of bmd.c (premultiplied, the
 * operations of Ruffle's bitmap::operations); this file is the script
 * glue, following Ruffle's avm2/globals/flash/display/bitmap_data.rs for
 * argument handling and errors. */
#include "avm2_flash_int.h"
#include "render_filters.h"
#include "image.h"
#include "bmd.h"
#include "pbj.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

extern Bitmap *image_get(SWFMovie *m, int id);
extern void player_bmd_draw_blend(Player *p, BitmapData *t, DisplayObject *d, BitmapData *src, const SWFMatrix *m, const SWFCxform *cx, const s32 *clip, int smoothing, int blend);
extern A2Obj *a2_bytearray_new(VM2 *vm);
extern int a2_ba_write_u32(VM2 *vm, A2Obj *b, u32 v);
extern int a2_ba_read_u32(VM2 *vm, A2Obj *b, u32 *out);
extern V2 a2_vector_apply(VM2 *vm, V2 base, int argc, V2 *argv);
extern A2Obj *a2_clip_bitmap_data(MovieClip *c);
extern void throw_null_ref(VM2 *vm, V2 v);
V2 a2_get_s(VM2 *vm, V2 obj, const char *name);

static V2 dget(VM2 *vm, V2 self, const char *k) { A2Prop *p = self.t == T_OBJ ? a2_dyn_find(self.u.o, a2_str(vm, k)) : NULL; return p ? p->v : v2_undef(); }
static double dnum(VM2 *vm, V2 self, const char *k) { V2 v = dget(vm, self, k); return v.t == T_UNDEF ? (self.t == T_OBJ && self.u.o->kind == OK_POINT ? 0 : 0) : v2_tonum(vm, v); }
static s32 dint(VM2 *vm, V2 self, const char *k) { V2 v = dget(vm, self, k); return v.t == T_UNDEF ? 0 : v2_toint(vm, v); }

#define THREW (vm->throwing)

/* ---------------------------------------------------------------- */
/* payload                                                          */

BitmapData *a2_bitmapdata_of(A2Obj *o) { return o && o->kind == OK_BITMAPDATA ? (BitmapData*)o->payload : NULL; }
static BitmapData *bd_v(V2 v) { BitmapData *b = v.t == T_OBJ ? a2_bitmapdata_of(v.u.o) : NULL; if (b) bmd_own(b); return b; }   /* script access: shared symbol pixels become private first */
void a2_bitmapdata_free_payload(A2Obj *o) { bmd_unref((BitmapData*)o->payload); o->payload = NULL; }
int a2_bitmapdata_size(A2Obj *bd, int *w, int *h) { BitmapData *b = a2_bitmapdata_of(bd); if (!b) return 0; *w = (int)b->w; *h = (int)b->h; return 1; }
Bitmap *a2_bitmapdata_bitmap(A2Obj *bd) { return (Bitmap*)bmd_view(a2_bitmapdata_of(bd)); }

size_t g_a2_ext_bytes = 0;   /* pixel memory allocated since the last collection (GC pressure) */
static void set_payload(A2Obj *o, BitmapData *b) {
    if (b) g_a2_ext_bytes += (size_t)b->w * b->h * 4;
    if (o->kind == OK_BITMAPDATA && o->payload) bmd_unref((BitmapData*)o->payload);
    o->kind = OK_BITMAPDATA; o->payload = b;
}
static A2Obj *wrap(VM2 *vm, BitmapData *b) {
    A2Obj *o;
    if (!b) return NULL;
    o = a2_new(vm, vm->c_bitmapdata); set_payload(o, b);
    return o;
}
A2Obj *a2_bitmapdata_wrap(VM2 *vm, BitmapData *b) { return wrap(vm, bmd_ref(b)); }

/* Loader content for a decoded image: a Bitmap with its BitmapData (takes bm) */
A2Obj *a2_bitmap_from_decoded(VM2 *vm, Bitmap *bm, int transparent) {
    A2Obj *bd, *content; BitmapData *b;
    if (!transparent) { int i; for (i = 0; i < bm->w * bm->h; i++) bm->px[i] |= 0xff000000u; }
    b = bmd_from_straight(bm, transparent);
    free(bm->px); free(bm);
    if (!b) return NULL;
    bd = wrap(vm, b);
    content = a2_new_display(vm, vm->c_bitmap);
    a2_disp(content)->bitmap_data = bd; a2_disp(content)->bmp_w = (int)b->w; a2_disp(content)->bmp_h = (int)b->h;
    return content;
}

static BitmapData *from_symbol(VM2 *vm, SWFMovie *m, int id) {
    Bitmap *src = image_get(m, id);
    if (src) return bmd_from_symbol_image(src, 1);
    return bmd_new_pixels(0, 0, 1, NULL);     /* an empty (0x0) bitmap symbol */
}
/* a bitmap symbol's data as an instance of its linked BitmapData subclass, allocated without
   running the constructor (Ruffle BitmapDataObject::from_bitmap_data_internal) */
A2Obj *a2_bitmapdata_from_char_cls(VM2 *vm, SWFChar *ch, A2Class *cls) {
    Bitmap *src = image_get(ch->movie ? ch->movie : vm->player->movie, ch->id); A2Obj *o; BitmapData *b;
    if (!src) return NULL;
    b = bmd_from_symbol_image(src, 1); if (!b) return NULL;
    if (cls->abc && !cls->inited) a2_class_init(vm, cls);
    o = a2_new(vm, cls); o->kind = OK_BITMAPDATA; set_payload(o, b);
    return o;
}
A2Obj *a2_bitmapdata_from_char(VM2 *vm, SWFChar *ch) {
    Bitmap *src = image_get(ch->movie ? ch->movie : vm->player->movie, ch->id);
    if (!src) return NULL;
    return wrap(vm, bmd_from_straight(src, 1));
}

/* registry of BitmapData for fill ids: negative ids below -1 */
int a2_bitmapdata_register(VM2 *vm, A2Obj *bd) { A2Obj *reg; V2 r = dget(vm, v2_obj(vm->global), "__bdreg"); int i; if (r.t != T_OBJ) { reg = a2_new_array(vm); a2_dyn_set_hidden(vm, vm->global, a2_str(vm, "__bdreg"), v2_obj(reg)); } else reg = r.u.o; for (i = 0; i < reg->alen; i++) if (reg->arr[i].t == T_OBJ && reg->arr[i].u.o == bd) return -2 - i; a2_arr_push(vm, reg, v2_obj(bd)); return -2 - (reg->alen - 1); }
static Player *g_bd_player;
void a2_bitmapdata_set_player(Player *p) { g_bd_player = p; }
const Bitmap *a2_bitmapdata_lookup(int id) { Player *p = g_bd_player; VM2 *vm = p ? (VM2*)p->avm2 : NULL; V2 r; int i; if (!vm || id > -2) return NULL; r = dget(vm, v2_obj(vm->global), "__bdreg"); if (r.t != T_OBJ) return NULL; i = -2 - id; if (i < 0 || i >= r.u.o->alen || r.u.o->arr[i].t != T_OBJ) return NULL; return a2_bitmapdata_bitmap(r.u.o->arr[i].u.o); }

/* ---------------------------------------------------------------- */
/* argument helpers                                                 */

static V2 idx_get(VM2 *vm, A2Obj *o, int i) { char nb[16]; sprintf(nb, "%d", i); return a2_get_s(vm, v2_obj(o), nb); }
static int has(int argc, V2 *argv, int i) { return i < argc && argv[i].t != T_UNDEF && argv[i].t != T_HOLE; }
static s32 argi(VM2 *vm, int argc, V2 *argv, int i, s32 def) { return i < argc && argv[i].t != T_HOLE ? v2_toint(vm, argv[i]) : def; }
static u32 argu(VM2 *vm, int argc, V2 *argv, int i, u32 def) { return i < argc && argv[i].t != T_HOLE ? v2_touint(vm, argv[i]) : def; }
static int argb(VM2 *vm, int argc, V2 *argv, int i, int def) { return i < argc && argv[i].t != T_HOLE ? v2_tobool(vm, argv[i]) : def; }
static A2Obj *try_obj(int argc, V2 *argv, int i) { return i < argc && argv[i].t == T_OBJ ? argv[i].u.o : NULL; }

/* args.get_object(i, name): TypeError 2007 when null */
static A2Obj *need_obj(VM2 *vm, int argc, V2 *argv, int i, const char *name) {
    V2 v = a2_arg(argc, argv, i);
    if (v.t != T_OBJ) { a2_throw_id(vm, "TypeError", 2007, name, NULL); return NULL; }
    return v.u.o;
}
static int check_valid(VM2 *vm, BitmapData *b) {
    if (b->disposed) { a2_throw_id(vm, "ArgumentError", 2015, NULL, NULL); return 0; }
    return 1;
}
/* the receiver: NULL when not a BitmapData (or with a pending error) */
static BitmapData *self_valid(VM2 *vm, V2 self) { BitmapData *b = bd_v(self);
    if (!b) { if (self.t == T_OBJ && self.u.o->cls && vm->c_bitmapdata && a2_is_subclass(self.u.o->cls, vm->c_bitmapdata)) a2_throw_id(vm, "ArgumentError", 2015, NULL, NULL); return NULL; }   /* before the constructor ran */
    if (!check_valid(vm, b)) return NULL; return b; }

static s32 round_even(double n) {
    double f = floor(n), d = n - f, r;
    if (n != n) return (s32)0x80000000;
    if (d > 0.5) r = f + 1; else if (d < 0.5) r = f; else r = fmod(f, 2.0) == 0 ? f : f + 1;
    if (r > 2147483647.0 || r < -2147483648.0) return (s32)0x80000000;
    return (s32)r;
}
/* get_rectangle_x_y_width_height */
static void rect_xywh(VM2 *vm, A2Obj *r, s32 *out) {
    V2 rv = v2_obj(r);
    double x = dnum(vm, rv, "x"), y = dnum(vm, rv, "y"), w = dnum(vm, rv, "width"), h = dnum(vm, rv, "height");
    s32 xm = round_even(x + w), ym = round_even(y + h), xi = round_even(x), yi = round_even(y);
    out[0] = xi; out[1] = yi; out[2] = (s32)((u32)xm - (u32)xi); out[3] = (s32)((u32)ym - (u32)yi);
}
/* object_to_rectangle, in pixels: x_min y_min x_max y_max */
static void rect_minmax(VM2 *vm, A2Obj *r, s32 *out) {
    V2 rv = v2_obj(r);
    double x = dnum(vm, rv, "x"), y = dnum(vm, rv, "y"), w = dnum(vm, rv, "width"), h = dnum(vm, rv, "height");
    out[0] = round_even(x); out[1] = round_even(y); out[2] = round_even(x + w); out[3] = round_even(y + h);
}
static void point_i32(VM2 *vm, A2Obj *p, s32 *x, s32 *y) { *x = dint(vm, v2_obj(p), "x"); *y = THREW ? 0 : dint(vm, v2_obj(p), "y"); }

static s16 fx8(double f) { double v = f * 256.0; if (v != v) return 0; if (v >= 32767) return 32767; if (v <= -32768) return -32768; return (s16)v; }
static s16 i16of(double f) { if (f != f) return 0; if (f >= 32767) return 32767; if (f <= -32768) return -32768; return (s16)f; }
static void cxform_of(VM2 *vm, A2Obj *o, SWFCxform *cx) {
    V2 v = v2_obj(o);
    cx->mr = fx8(dnum(vm, v, "redMultiplier")); cx->mg = fx8(dnum(vm, v, "greenMultiplier")); cx->mb = fx8(dnum(vm, v, "blueMultiplier")); cx->ma = fx8(dnum(vm, v, "alphaMultiplier"));
    cx->ar = i16of(dnum(vm, v, "redOffset")); cx->ag = i16of(dnum(vm, v, "greenOffset")); cx->ab = i16of(dnum(vm, v, "blueOffset")); cx->aa = i16of(dnum(vm, v, "alphaOffset"));
}

static V2 new_rect(VM2 *vm, double x, double y, double w, double h) { V2 a[4]; a[0] = v2_num(x); a[1] = v2_num(y); a[2] = v2_num(w); a[3] = v2_num(h); return a2_construct_class(vm, vm->c_rectangle, 4, a); }

/* ---------------------------------------------------------------- */
/* constructor and properties                                       */

static V2 BD_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = SELFO; BitmapData *b = NULL; A2Class *c; u32 w, h, fill; int tr;
    if (!o) return v2_undef();
    for (c = o->cls; c && c != vm->c_bitmapdata; c = c->super) if (c->symbol_char > 0) {
        SWFMovie *m = c->symbol_movie ? c->symbol_movie : vm->player->movie; SWFChar *sc = swf_get_char(m, c->symbol_char);
        if (sc && sc->type == CHAR_BITMAP) b = from_symbol(vm, m, c->symbol_char);
        break;
    }
    if (!b) {
        w = (u32)argi(vm, argc, argv, 0, 0); if (THREW) return v2_undef();
        h = (u32)argi(vm, argc, argv, 1, 0); if (THREW) return v2_undef();
        tr = argb(vm, argc, argv, 2, 1);
        fill = argu(vm, argc, argv, 3, 0xFFFFFFFFu); if (THREW) return v2_undef();
        if (!bmd_size_valid(vm->player->movie ? vm->player->movie->version : 10, w, h)) { a2_throw_id(vm, "ArgumentError", 2015, NULL, NULL); return v2_undef(); }
        b = bmd_new(w, h, tr, fill);
        if (!b) { a2_throw_id(vm, "ArgumentError", 2015, NULL, NULL); return v2_undef(); }
    }
    set_payload(o, b);
    return v2_undef();
}

static V2 bd_width(VM2 *vm, V2 self, int argc, V2 *argv) { BitmapData *b = self_valid(vm, self); UNUSED; return b ? v2_int((s32)b->w) : v2_undef(); }
static V2 bd_height(VM2 *vm, V2 self, int argc, V2 *argv) { BitmapData *b = self_valid(vm, self); UNUSED; return b ? v2_int((s32)b->h) : v2_undef(); }
static V2 bd_transparent(VM2 *vm, V2 self, int argc, V2 *argv) { BitmapData *b = self_valid(vm, self); UNUSED; return b ? v2_bool(b->transparent) : v2_undef(); }
static V2 bd_rect(VM2 *vm, V2 self, int argc, V2 *argv) { BitmapData *b = bd_v(self); UNUSED; return b ? new_rect(vm, 0, 0, b->w, b->h) : v2_undef(); }

/* ---------------------------------------------------------------- */
/* pixels                                                           */

static V2 bd_getPixel(VM2 *vm, V2 self, int argc, V2 *argv) {
    BitmapData *b = self_valid(vm, self); u32 x, y;
    if (!b) return v2_undef();
    x = (u32)argi(vm, argc, argv, 0, 0); y = (u32)argi(vm, argc, argv, 1, 0); if (THREW) return v2_undef();
    return v2_uint(bmd_get_pixel(b, x, y));
}
static V2 bd_getPixel32(VM2 *vm, V2 self, int argc, V2 *argv) {
    BitmapData *b = self_valid(vm, self); u32 x, y;
    if (!b) return v2_undef();
    x = (u32)argi(vm, argc, argv, 0, 0); y = (u32)argi(vm, argc, argv, 1, 0); if (THREW) return v2_undef();
    return v2_uint(bmd_get_pixel32(b, x, y));
}
static V2 bd_setPixel(VM2 *vm, V2 self, int argc, V2 *argv) {
    BitmapData *b = bd_v(self); u32 x, y, c;
    if (!b) return v2_undef();
    x = (u32)argi(vm, argc, argv, 0, 0); y = (u32)argi(vm, argc, argv, 1, 0); c = argu(vm, argc, argv, 2, 0); if (THREW) return v2_undef();
    if (!b->disposed) { bmd_set_pixel(b, x, y, c); bmd_changed(b); }
    return v2_undef();
}
static V2 bd_setPixel32(VM2 *vm, V2 self, int argc, V2 *argv) {
    BitmapData *b = self_valid(vm, self); u32 x, y, c;
    if (!b) return v2_undef();
    x = (u32)argi(vm, argc, argv, 0, 0); y = (u32)argi(vm, argc, argv, 1, 0); c = argu(vm, argc, argv, 2, 0); if (THREW) return v2_undef();
    bmd_set_pixel32(b, x, y, c); bmd_changed(b);
    return v2_undef();
}

static void clamp_region(PxRegion *r, const s32 *xywh, BitmapData *b) { *r = reg_i32(xywh[0], xywh[1], xywh[2], xywh[3]); reg_clamp(r, b->w, b->h); }

static V2 bd_getPixels(VM2 *vm, V2 self, int argc, V2 *argv) {
    BitmapData *b = self_valid(vm, self); A2Obj *ro, *ba; s32 r[4]; PxRegion g; u32 x, y;
    if (!b) return v2_undef();
    if (!(ro = need_obj(vm, argc, argv, 0, "rect"))) return v2_undef();
    rect_xywh(vm, ro, r); if (THREW) return v2_undef();
    ba = a2_bytearray_new(vm); clamp_region(&g, r, b);
    for (y = g.y0; y < g.y1; y++) for (x = g.x0; x < g.x1; x++) if (!a2_ba_write_u32(vm, ba, bmd_unmul(b->px[y * b->w + x]))) return v2_undef();
    return v2_obj(ba);
}
static V2 bd_copyPixelsToByteArray(VM2 *vm, V2 self, int argc, V2 *argv) {
    BitmapData *b = self_valid(vm, self); A2Obj *ro, *ba; s32 r[4]; PxRegion g; u32 x, y;
    if (!b) return v2_undef();
    if (!(ro = need_obj(vm, argc, argv, 0, "rect"))) return v2_undef();
    if (!(ba = need_obj(vm, argc, argv, 1, "data"))) return v2_undef();
    rect_xywh(vm, ro, r); if (THREW) return v2_undef();
    clamp_region(&g, r, b);
    for (y = g.y0; y < g.y1; y++) for (x = g.x0; x < g.x1; x++) if (!a2_ba_write_u32(vm, ba, bmd_unmul(b->px[y * b->w + x]))) return v2_undef();
    return v2_undef();
}
static V2 bd_setPixels(VM2 *vm, V2 self, int argc, V2 *argv) {
    BitmapData *b; A2Obj *ro, *ba; s32 r[4]; PxRegion g; u32 x, y, c;
    if (!(ro = need_obj(vm, argc, argv, 0, "rect"))) return v2_undef();
    if (!(ba = need_obj(vm, argc, argv, 1, "inputByteArray"))) return v2_undef();
    if (!(b = bd_v(self)) || b->disposed) return v2_undef();
    rect_xywh(vm, ro, r); if (THREW) return v2_undef();
    clamp_region(&g, r, b);
    if (g.x1 > g.x0 && g.y1 > g.y0) {
        for (y = g.y0; y < g.y1; y++) for (x = g.x0; x < g.x1; x++) {
            if (!a2_ba_read_u32(vm, ba, &c)) { bmd_changed(b); return v2_undef(); }
            b->px[y * b->w + x] = bmd_premul(c, b->transparent);
        }
        bmd_changed(b);
    }
    return v2_undef();
}
static V2 bd_getVector(VM2 *vm, V2 self, int argc, V2 *argv) {
    BitmapData *b = self_valid(vm, self); A2Obj *ro, *v; s32 r[4]; PxRegion g; u32 x, y; V2 a[1], cv;
    if (!b) return v2_undef();
    if (!(ro = need_obj(vm, argc, argv, 0, "rect"))) return v2_undef();
    rect_xywh(vm, ro, r); if (THREW) return v2_undef();
    clamp_region(&g, r, b);
    a[0] = v2_obj(vm->c_uint->obj); cv = a2_vector_apply(vm, v2_obj(vm->c_vector->obj), 1, a);
    v = a2_new(vm, cv.u.o->as_class); v->kind = OK_VECTOR; v->vec_type = vm->c_uint;
    for (y = g.y0; y < g.y1; y++) for (x = g.x0; x < g.x1; x++) a2_arr_push(vm, v, v2_uint(bmd_unmul(b->px[y * b->w + x])));
    return v2_obj(v);
}
static double clampd(double v, double lo, double hi) { return v != v ? lo : v < lo ? lo : v > hi ? hi : v; }
static V2 bd_setVector(VM2 *vm, V2 self, int argc, V2 *argv) {
    BitmapData *b; A2Obj *ro, *vec; double x, y, w, h, x0, y0, x1, y1; u32 ux0, uy0, ux1, uy1, px, py; int i = 0;
    if (!(ro = need_obj(vm, argc, argv, 0, "rect"))) return v2_undef();
    if (!(vec = need_obj(vm, argc, argv, 1, "imputVector"))) return v2_undef();
    if (!(b = bd_v(self))) return v2_undef();
    x = dnum(vm, v2_obj(ro), "x"); y = dnum(vm, v2_obj(ro), "y"); w = dnum(vm, v2_obj(ro), "width"); h = dnum(vm, v2_obj(ro), "height");
    if (THREW) return v2_undef();
    x0 = clampd(x, 0, b->w); y0 = clampd(y, 0, b->h); x1 = clampd(x + w, x0, b->w); y1 = clampd(y + h, y0, b->h);
    ux0 = (u32)x0; uy0 = (u32)y0; ux1 = (u32)x1; uy1 = (u32)y1;
    if ((double)vec->alen < (double)(ux1 - ux0) * (double)(uy1 - uy0)) { a2_throw_id(vm, "RangeError", 2006, NULL, NULL); return v2_undef(); }
    if (b->disposed) return v2_undef();
    for (py = uy0; py < uy1; py++) for (px = ux0; px < ux1; px++) { u32 c = v2_touint(vm, vec->arr[i++]); b->px[py * b->w + px] = bmd_premul(c, b->transparent); }
    bmd_changed(b);
    return v2_undef();
}

static V2 bd_fillRect(VM2 *vm, V2 self, int argc, V2 *argv) {
    BitmapData *b; A2Obj *ro; u32 c; s32 r[4];
    if (!(ro = need_obj(vm, argc, argv, 0, "rect"))) return v2_undef();
    c = argu(vm, argc, argv, 1, 0); if (THREW) return v2_undef();
    if (!(b = self_valid(vm, self))) return v2_undef();
    rect_xywh(vm, ro, r); if (THREW) return v2_undef();
    bmd_fill_rect(b, r[0], r[1], r[2], r[3], c); bmd_changed(b);
    return v2_undef();
}
static V2 bd_floodFill(VM2 *vm, V2 self, int argc, V2 *argv) {
    BitmapData *b = bd_v(self); u32 x, y, c;
    if (!b || b->disposed) return v2_undef();
    x = (u32)argi(vm, argc, argv, 0, 0); y = (u32)argi(vm, argc, argv, 1, 0); c = argu(vm, argc, argv, 2, 0); if (THREW) return v2_undef();
    bmd_flood_fill(b, x, y, c); bmd_changed(b);
    return v2_undef();
}
static V2 bd_scroll(VM2 *vm, V2 self, int argc, V2 *argv) {
    BitmapData *b = self_valid(vm, self); s32 x, y;
    if (!b) return v2_undef();
    x = argi(vm, argc, argv, 0, 0); y = argi(vm, argc, argv, 1, 0); if (THREW) return v2_undef();
    bmd_scroll(b, x, y); bmd_changed(b);
    return v2_undef();
}
static V2 bd_noise(VM2 *vm, V2 self, int argc, V2 *argv) {
    BitmapData *b; u8 lo, hi; int ch, gray; s32 seed;
    lo = (u8)argu(vm, argc, argv, 1, 0); hi = (u8)argu(vm, argc, argv, 2, 255); ch = (int)(argu(vm, argc, argv, 3, 7) & 15); gray = argb(vm, argc, argv, 4, 0);
    if (THREW || !(b = self_valid(vm, self))) return v2_undef();
    seed = argi(vm, argc, argv, 0, 0); if (THREW) return v2_undef();
    bmd_noise(b, seed, lo, hi > lo ? hi : lo, ch, gray); bmd_changed(b);
    return v2_undef();
}
static V2 bd_perlinNoise(VM2 *vm, V2 self, int argc, V2 *argv) {
    BitmapData *b = bd_v(self); double bx, by, *offs; u32 oct, i; s32 seed; int stitch, fractal, ch, gray; A2Obj *oo;
    if (!b || b->disposed) return v2_undef();
    bx = v2_tonum(vm, a2_arg(argc, argv, 0)); by = v2_tonum(vm, a2_arg(argc, argv, 1));
    oct = argu(vm, argc, argv, 2, 0); seed = argi(vm, argc, argv, 3, 0);
    stitch = argb(vm, argc, argv, 4, 0); fractal = argb(vm, argc, argv, 5, 0);
    ch = (int)(argu(vm, argc, argv, 6, 7) & 15); gray = argb(vm, argc, argv, 7, 0);
    if (THREW) return v2_undef();
    if (oct > 4096) oct = 4096;
    oo = try_obj(argc, argv, 8);
    offs = (double*)calloc((size_t)oct * 2 + 2, sizeof(double));
    if (!offs) return v2_undef();
    for (i = 0; i < oct; i++) {
        if (oo && oo->kind == OK_ARRAY && (int)i < oo->alen && oo->arr[i].t == T_OBJ && a2_is_subclass(oo->arr[i].u.o->cls, vm->c_point)) {
            offs[i * 2] = dnum(vm, oo->arr[i], "x"); offs[i * 2 + 1] = dnum(vm, oo->arr[i], "y");
            if (THREW) { free(offs); return v2_undef(); }
        }
    }
    bmd_perlin_noise(b, bx, by, (int)oct, seed, stitch, fractal, ch, gray, offs); bmd_changed(b);
    free(offs);
    return v2_undef();
}

static V2 bd_colorTransform(VM2 *vm, V2 self, int argc, V2 *argv) {
    BitmapData *b = bd_v(self); A2Obj *ro, *co; s32 r[4]; SWFCxform cx;
    if (!b || b->disposed) return v2_undef();
    if (!(ro = need_obj(vm, argc, argv, 0, "rect"))) return v2_undef();
    rect_xywh(vm, ro, r); if (THREW) return v2_undef();
    if (!(co = need_obj(vm, argc, argv, 1, "colorTransform"))) return v2_undef();
    cxform_of(vm, co, &cx); if (THREW) return v2_undef();
    bmd_color_transform(b, r[0] > 0 ? (u32)r[0] : 0, r[1] > 0 ? (u32)r[1] : 0, (u32)(r[0] + r[2]), (u32)(r[1] + r[3]), &cx); bmd_changed(b);
    return v2_undef();
}
static V2 bd_getColorBoundsRect(VM2 *vm, V2 self, int argc, V2 *argv) {
    BitmapData *b = bd_v(self); u32 mask, col, out[4]; int find;
    if (!b || b->disposed) return v2_undef();
    find = argb(vm, argc, argv, 2, 1); mask = argu(vm, argc, argv, 0, 0); col = argu(vm, argc, argv, 1, 0); if (THREW) return v2_undef();
    bmd_color_bounds(b, find, mask, col, out);
    return new_rect(vm, out[0], out[1], out[2], out[3]);
}
static V2 bd_lock(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_undef(); }

/* ---------------------------------------------------------------- */
/* hitTest                                                          */

static V2 bd_hitTest(VM2 *vm, V2 self, int argc, V2 *argv) {
    BitmapData *b = bd_v(self), *o = NULL; A2Obj *fp, *co; s32 tlx, tly; u8 th;
    if (!b || b->disposed) return v2_bool(0);
    if (!(fp = need_obj(vm, argc, argv, 0, "firstPoint"))) return v2_undef();
    point_i32(vm, fp, &tlx, &tly); if (THREW) return v2_undef();
    { u32 t = argu(vm, argc, argv, 1, 0); th = (u8)(t > 255 ? 255 : t); }
    if (THREW) return v2_undef();
    co = try_obj(argc, argv, 2);
    if (!co) { a2_throw_id(vm, "TypeError", 2005, "0", "BitmapData"); return v2_undef(); }
    if (a2_is_subclass(co->cls, vm->c_point)) {
        s32 x, y; point_i32(vm, co, &x, &y); if (THREW) return v2_undef();
        return v2_bool(bmd_hit_test_point(b, th, (s32)((u32)x - (u32)tlx), (s32)((u32)y - (u32)tly)));
    }
    if (a2_is_subclass(co->cls, vm->c_rectangle)) {
        V2 cv = v2_obj(co); s32 x = dint(vm, cv, "x"), y = dint(vm, cv, "y"), w = dint(vm, cv, "width"), h = dint(vm, cv, "height");
        if (THREW) return v2_undef();
        return v2_bool(bmd_hit_test_rect(b, th, (s32)((u32)x - (u32)tlx), (s32)((u32)y - (u32)tly), w, h));
    }
    if ((o = a2_bitmapdata_of(co)) == NULL && co->kind == OK_DISPLAY && a2_is_subclass(co->cls, vm->c_bitmap) && a2_disp(co)->bitmap_data) o = a2_bitmapdata_of(a2_disp(co)->bitmap_data);
    if (o) {
        A2Obj *sp; s32 px, py; u32 t2;
        if (!check_valid(vm, o)) return v2_undef();
        if (!(sp = need_obj(vm, argc, argv, 3, "secondBitmapDataPoint"))) return v2_undef();
        point_i32(vm, sp, &px, &py); if (THREW) return v2_undef();
        t2 = argu(vm, argc, argv, 4, 1); if (THREW) return v2_undef();
        return v2_bool(bmd_hit_test_bitmap(b, tlx, tly, th, o, px, py, (u8)(t2 > 255 ? 255 : t2)));
    }
    a2_throw_id(vm, "TypeError", 2005, "0", "BitmapData");
    return v2_undef();
}

/* ---------------------------------------------------------------- */
/* copies between bitmaps                                           */

static V2 bd_copyPixels(VM2 *vm, V2 self, int argc, V2 *argv) {
    BitmapData *b = self_valid(vm, self), *s, *al; A2Obj *so, *ro, *po; s32 r[4], dx, dy;
    if (!b) return v2_undef();
    if (!(so = need_obj(vm, argc, argv, 0, "sourceBitmapData"))) return v2_undef();
    if (!(ro = need_obj(vm, argc, argv, 1, "sourceRect"))) return v2_undef();
    rect_xywh(vm, ro, r); if (THREW) return v2_undef();
    if (!(po = need_obj(vm, argc, argv, 2, "destPoint"))) return v2_undef();
    point_i32(vm, po, &dx, &dy); if (THREW) return v2_undef();
    if (!(s = a2_bitmapdata_of(so))) return v2_undef();
    if (!check_valid(vm, s)) return v2_undef();
    al = a2_bitmapdata_of(try_obj(argc, argv, 3));
    if (al) {
        s32 ax = 0, ay = 0; A2Obj *ap = try_obj(argc, argv, 4); int merge;
        if (ap) { point_i32(vm, ap, &ax, &ay); if (THREW) return v2_undef(); }
        merge = argb(vm, argc, argv, 5, 0);
        bmd_copy_pixels_alpha(b, s, r[0], r[1], r[2], r[3], dx, dy, al, ax, ay, merge);
    } else bmd_copy_pixels(b, s, r[0], r[1], r[2], r[3], dx, dy, argb(vm, argc, argv, 5, 0));
    bmd_changed(b);
    return v2_undef();
}
static V2 bd_copyChannel(VM2 *vm, V2 self, int argc, V2 *argv) {
    BitmapData *b = self_valid(vm, self), *s; A2Obj *so, *ro, *po; s32 r[4], dx, dy, sc, dc;
    if (!b) return v2_undef();
    if (!(so = need_obj(vm, argc, argv, 0, "sourceBitmapData"))) return v2_undef();
    if (!(ro = need_obj(vm, argc, argv, 1, "sourceRect"))) return v2_undef();
    if (!(po = need_obj(vm, argc, argv, 2, "destPoint"))) return v2_undef();
    point_i32(vm, po, &dx, &dy); if (THREW) return v2_undef();
    sc = (s32)argu(vm, argc, argv, 3, 0); dc = (s32)argu(vm, argc, argv, 4, 0); if (THREW) return v2_undef();
    if (!(s = a2_bitmapdata_of(so))) return v2_undef();
    rect_xywh(vm, ro, r); if (THREW) return v2_undef();
    if (s->disposed) return v2_undef();
    bmd_copy_channel(b, dx, dy, r[0], r[1], r[2], r[3], s, sc, dc); bmd_changed(b);
    return v2_undef();
}
static V2 bd_merge(VM2 *vm, V2 self, int argc, V2 *argv) {
    BitmapData *b = bd_v(self), *s; A2Obj *so, *ro, *po; s32 r[4], dx, dy, rm, gm, bm, am;
    if (!b || b->disposed) return v2_undef();
    if (!(so = need_obj(vm, argc, argv, 0, "sourceBitmapData"))) return v2_undef();
    if (!(ro = need_obj(vm, argc, argv, 1, "sourceRect"))) return v2_undef();
    rect_xywh(vm, ro, r); if (THREW) return v2_undef();
    if (!(po = need_obj(vm, argc, argv, 2, "destPoint"))) return v2_undef();
    point_i32(vm, po, &dx, &dy); if (THREW) return v2_undef();
    rm = (s32)argu(vm, argc, argv, 3, 0); gm = (s32)argu(vm, argc, argv, 4, 0); bm = (s32)argu(vm, argc, argv, 5, 0); am = (s32)argu(vm, argc, argv, 6, 0);
    if (THREW) return v2_undef();
    if ((s = a2_bitmapdata_of(so)) != NULL && !s->disposed) { bmd_merge(b, s, r[0], r[1], r[2], r[3], dx, dy, rm, gm, bm, am); bmd_changed(b); }
    return v2_undef();
}
static V2 bd_paletteMap(VM2 *vm, V2 self, int argc, V2 *argv) {
    static const int shifts[4] = { 16, 8, 0, 24 };
    BitmapData *b = self_valid(vm, self), *s; A2Obj *so, *ro, *po; s32 mm[4], dx, dy; u32 *ch; int k, i;
    if (!b) return v2_undef();
    if (!(so = need_obj(vm, argc, argv, 0, "sourceBitmapData"))) return v2_undef();
    s = a2_bitmapdata_of(so);
    if (!(ro = need_obj(vm, argc, argv, 1, "sourceRect"))) return v2_undef();
    rect_minmax(vm, ro, mm); if (THREW) return v2_undef();
    if (!(po = need_obj(vm, argc, argv, 2, "destPoint"))) return v2_undef();
    point_i32(vm, po, &dx, &dy); if (THREW) return v2_undef();
    ch = (u32*)malloc(4 * 256 * sizeof(u32));
    if (!ch) return v2_undef();
    for (k = 0; k < 4; k++) {
        A2Obj *ao = try_obj(argc, argv, 3 + k);
        for (i = 0; i < 256; i++) {
            if (ao) {
                V2 e = ao->kind == OK_ARRAY ? (i < ao->alen ? ao->arr[i] : v2_undef()) : idx_get(vm, ao, i);
                if (THREW) { free(ch); return v2_undef(); }
                ch[k * 256 + i] = v2_touint(vm, e.t == T_HOLE ? v2_undef() : e);
                if (THREW) { free(ch); return v2_undef(); }
            } else ch[k * 256 + i] = (u32)i << shifts[k];
        }
    }
    if (s && !s->disposed) { bmd_palette_map(b, s, mm[0], mm[1], (s32)((u32)mm[2] - (u32)mm[0]), (s32)((u32)mm[3] - (u32)mm[1]), dx, dy, ch); bmd_changed(b); }
    free(ch);
    return v2_undef();
}
static V2 bd_threshold(VM2 *vm, V2 self, int argc, V2 *argv) {
    BitmapData *b = bd_v(self), *s; A2Obj *so, *ro, *po; s32 r[4], dx, dy; V2 opv; A2Str *os; int op; u32 th, col, mask, cnt; int copy;
    if (!b || b->disposed) return v2_undef();
    if (!(so = need_obj(vm, argc, argv, 0, "sourceBitmapData"))) return v2_undef();
    if (!(ro = need_obj(vm, argc, argv, 1, "sourceRect"))) return v2_undef();
    if (!(po = need_obj(vm, argc, argv, 2, "destPoint"))) return v2_undef();
    point_i32(vm, po, &dx, &dy); if (THREW) return v2_undef();
    opv = a2_arg(argc, argv, 3);
    if (opv.t == T_NULL || opv.t == T_UNDEF) { a2_throw_id(vm, "TypeError", 2007, "operationStr", NULL); return v2_undef(); }
    os = v2_tostr(vm, opv); if (THREW) return v2_undef();
    th = argu(vm, argc, argv, 4, 0); col = argu(vm, argc, argv, 5, 0); mask = argu(vm, argc, argv, 6, 0xFFFFFFFFu); copy = argb(vm, argc, argv, 7, 0);
    if (THREW) return v2_undef();
    op = bmd_threshold_op(os->s);
    if (op < 0) { a2_throw_id(vm, "ArgumentError", 2005, "0", "Operation"); return v2_undef(); }
    rect_xywh(vm, ro, r); if (THREW) return v2_undef();
    if (!(s = a2_bitmapdata_of(so))) return v2_undef();
    if (!check_valid(vm, s)) return v2_undef();
    cnt = bmd_threshold(b, s, r[0], r[1], r[2], r[3], dx, dy, op, th, col, mask, copy); bmd_changed(b);
    return v2_uint(cnt);
}
static V2 bd_pixelDissolve(VM2 *vm, V2 self, int argc, V2 *argv) {
    BitmapData *b = self_valid(vm, self), *s; A2Obj *so, *ro, *po; s32 r[4], dx, dy, seed, n, res; u32 fill;
    if (!b) return v2_undef();
    if (!(so = need_obj(vm, argc, argv, 0, "sourceBitmapData"))) return v2_undef();
    if (!(ro = need_obj(vm, argc, argv, 1, "sourceRect"))) return v2_undef();
    rect_xywh(vm, ro, r); if (THREW) return v2_undef();
    if (!(po = need_obj(vm, argc, argv, 2, "destPoint"))) return v2_undef();
    point_i32(vm, po, &dx, &dy); if (THREW) return v2_undef();
    seed = argi(vm, argc, argv, 3, 0); n = argi(vm, argc, argv, 4, 0); if (THREW) return v2_undef();
    if (n < 0) { char nb[16]; sprintf(nb, "%ld", (long)n); a2_throw_id(vm, "RangeError", 2027, "numPixels", nb); return v2_undef(); }
    fill = argu(vm, argc, argv, 5, 0); if (THREW) return v2_undef();
    if (!(s = a2_bitmapdata_of(so))) return v2_undef();
    if (!check_valid(vm, s)) return v2_undef();
    res = bmd_pixel_dissolve(b, s, r[0], r[1], r[2], r[3], dx, dy, seed, n, fill); bmd_changed(b);
    return v2_int(res);
}
static V2 bd_compare(VM2 *vm, V2 self, int argc, V2 *argv) {
    BitmapData *b = bd_v(self), *o, *res; A2Obj *oo;
    if (!b) return v2_int(-1);
    if (!check_valid(vm, b)) return v2_undef();
    if (!(oo = need_obj(vm, argc, argv, 0, "otherBitmapData"))) return v2_undef();
    if (!(o = a2_bitmapdata_of(oo))) return v2_int(-2);
    if (!check_valid(vm, o)) return v2_undef();
    if (b->w != o->w) return v2_int(-3);
    if (b->h != o->h) return v2_int(-4);
    res = bmd_compare(b, o);
    if (!res) return v2_int(0);
    return v2_obj(wrap(vm, res));
}
static V2 bd_clone(VM2 *vm, V2 self, int argc, V2 *argv) {
    BitmapData *b = bd_v(self), *n; UNUSED;
    if (!b || b->disposed) return v2_undef();
    n = bmd_clone(b);
    return n ? v2_obj(wrap(vm, n)) : v2_undef();
}
static V2 bd_dispose(VM2 *vm, V2 self, int argc, V2 *argv) { BitmapData *b = bd_v(self); UNUSED; if (b) bmd_dispose(b); return v2_undef(); }

/* ---------------------------------------------------------------- */
/* draw / applyFilter                                               */

static int blend_index(const char *s) {
    static const char *names[] = { "normal", "", "layer", "multiply", "screen", "lighten", "darken", "difference", "add", "subtract", "invert", "alpha", "erase", "overlay", "hardlight", NULL };
    int i; for (i = 0; names[i]; i++) if (names[i][0] && !strcmp(s, names[i])) return i;
    return -1;
}
static V2 do_draw(VM2 *vm, V2 self, int argc, V2 *argv, int with_quality) {
    BitmapData *b = bd_v(self), *src = NULL; SWFMatrix m; SWFCxform cx; int blend = 0, smoothing; s32 clip[4], *clipp = NULL; A2Obj *so, *o; DisplayObject *d = NULL; int dq = -1;
    if (!b) return v2_undef();
    swf_matrix_identity(&m); swf_cxform_identity(&cx);
    if ((o = try_obj(argc, argv, 1)) != NULL) { a2_matrix_from_obj(vm, v2_obj(o), &m); if (THREW) return v2_undef(); }
    if ((o = try_obj(argc, argv, 2)) != NULL) { cxform_of(vm, o, &cx); if (THREW) return v2_undef(); }
    if (has(argc, argv, 3) && argv[3].t != T_NULL) {
        A2Str *bs = v2_tostr(vm, argv[3]); if (THREW) return v2_undef();
        if ((blend = blend_index(bs->s)) < 0) { a2_throw_id(vm, "ArgumentError", 2008, "blendMode", NULL); return v2_undef(); }
    }
    if ((o = try_obj(argc, argv, 4)) != NULL) { s32 mm[4]; rect_minmax(vm, o, mm); if (THREW) return v2_undef(); clip[0] = mm[0]; clip[1] = mm[1]; clip[2] = mm[2] - mm[0]; clip[3] = mm[3] - mm[1]; clipp = clip; }
    smoothing = argb(vm, argc, argv, 5, 0);
    if (!(so = need_obj(vm, argc, argv, 0, "source"))) return v2_undef();
    if ((src = a2_bitmapdata_of(so)) == NULL) {
        d = a2_dobj_of(so);
        if (!d && so != vm->stage_obj) return v2_undef();   /* the stage draws its whole display list */
    }
    if (with_quality && has(argc, argv, 6) && argv[6].t != T_NULL) {
        static const char *q[] = { "low", "medium", "high", "best", "8x8", "8x8linear", "16x16", "16x16linear", NULL };
        A2Str *qs = v2_tostr(vm, argv[6]); int i, ok = 0; char lc[16]; size_t k;
        if (THREW) return v2_undef();
        for (k = 0; k < sizeof(lc) - 1 && k < qs->len; k++) lc[k] = (char)(qs->s[k] >= 'A' && qs->s[k] <= 'Z' ? qs->s[k] + 32 : qs->s[k]);
        lc[k] = 0;
        for (i = 0; q[i]; i++) if (qs->len < sizeof(lc) && !strcmp(lc, q[i])) ok = 1;
        if (!ok) { a2_throw_id(vm, "ArgumentError", 2004, NULL, NULL); return v2_undef(); }
        for (i = 0; q[i]; i++) if (!strcmp(lc, q[i])) dq = i;
    }
    if (!check_valid(vm, b)) return v2_undef();
    if (src && src->disposed) return v2_undef();
    if (src && (blend == 11 || blend == 12)) return v2_undef();
    if (src && (blend == 0 || blend == 2) && bmd_draw_bitmap_fast(b, src, &m, &cx, blend, clipp)) { bmd_changed(b); return v2_undef(); }
    g_draw_quality = dq;
    player_bmd_draw_blend(vm->player, b, d, src, &m, &cx, clipp, smoothing, blend);
    g_draw_quality = -1;
    bmd_changed(b);
    return v2_undef();
}
static V2 bd_draw(VM2 *vm, V2 self, int argc, V2 *argv) { return do_draw(vm, self, argc, argv, 0); }
static V2 bd_drawWithQuality(VM2 *vm, V2 self, int argc, V2 *argv) { return do_draw(vm, self, argc, argv, 1); }

static V2 bd_applyFilter(VM2 *vm, V2 self, int argc, V2 *argv) {
    BitmapData *b = bd_v(self), *s; A2Obj *so, *ro, *po, *fo; s32 mm[4], dx, dy; u32 sx, sy, sw, sh; PxRegion sr, dr;
    if (!b) return v2_undef();
    if (!(so = need_obj(vm, argc, argv, 0, "sourceBitmapData"))) return v2_undef();
    s = a2_bitmapdata_of(so);
    if (!(ro = need_obj(vm, argc, argv, 1, "sourceRect"))) return v2_undef();
    rect_minmax(vm, ro, mm); if (THREW) return v2_undef();
    if (!(fo = need_obj(vm, argc, argv, 3, "filter"))) return v2_undef();
    if (!(po = need_obj(vm, argc, argv, 2, "destPoint"))) return v2_undef();
    point_i32(vm, po, &dx, &dy); if (THREW) return v2_undef();
    if (!s || s->disposed || b->disposed) return v2_undef();
    sx = mm[0] > 0 ? (u32)mm[0] : 0; sy = mm[1] > 0 ? (u32)mm[1] : 0;
    sw = mm[2] > mm[0] ? (u32)(mm[2] - mm[0]) : 0; sh = mm[3] > mm[1] ? (u32)(mm[3] - mm[1]) : 0;
    if (!sw || !sh) return v2_undef();
    {
        extern int a2_filter_record(VM2 *vm, V2 o, SwfFilter *f); SwfFilter f;
        if (fo->cls && a2_filter_record(vm, v2_obj(fo), &f)) { bmd_apply_filter(b, s, (s32)sx, (s32)sy, (s32)sw, (s32)sh, dx, dy, &f); swf_filters_free(NULL, 0); free(f.cmatrix); if (f.map_bmd) bmd_unref(f.map_bmd); pbj_job_unref(f.pbj); }
        else if (!vm->throwing) {
            sr = reg_whole(s->w, s->h); dr = reg_whole(b->w, b->h);
            reg_clamp_with_intersection(&dr, dx, dy, (s32)sx, (s32)sy, (s32)sw, (s32)sh, &sr);
            if (dr.x1 > dr.x0 && dr.y1 > dr.y0) { bmd_copy_region(s, b, sr, dr, 0); bmd_changed(b); }
        }
    }
    return v2_undef();
}

/* generateFilterRect / histogram / encode (AS-level in Flash) */
static V2 bd_generateFilterRect(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *ro = try_obj(argc, argv, 0), *fo = try_obj(argc, argv, 1); BitmapData *b = bd_v(self); A2Class *sf = a2_class_find_s(vm, "flash.filters::ShaderFilter");
    if (fo && sf && a2_is_subclass(fo->cls, sf) && b) return new_rect(vm, 0, 0, b->w, b->h);
    if (!ro) { throw_null_ref(vm, v2_null()); return v2_undef(); }
    {
        extern int a2_filter_record(VM2 *vm, V2 o, SwfFilter *f); SwfFilter f;
        double x = dnum(vm, v2_obj(ro), "x"), y = dnum(vm, v2_obj(ro), "y"), w = dnum(vm, v2_obj(ro), "width"), h = dnum(vm, v2_obj(ro), "height");
        if (fo && fo->cls && a2_filter_record(vm, v2_obj(fo), &f)) { bmd_filter_rect(&f, &x, &y, &w, &h); free(f.cmatrix); if (f.map_bmd) bmd_unref(f.map_bmd); pbj_job_unref(f.pbj); }
        if (vm->throwing) return v2_undef();
        return new_rect(vm, x, y, w, h);
    }
}
static V2 bd_histogram(VM2 *vm, V2 self, int argc, V2 *argv) {
    BitmapData *b = self_valid(vm, self); A2Obj *ro = try_obj(argc, argv, 0), *res, *ch[4]; s32 r[4]; PxRegion g; u32 x, y; double *cnt; int k, i; V2 a[1], cv, cvv;
    if (!b) return v2_undef();
    if (ro) { rect_xywh(vm, ro, r); if (THREW) return v2_undef(); } else { r[0] = r[1] = 0; r[2] = (s32)b->w; r[3] = (s32)b->h; }
    clamp_region(&g, r, b);
    cnt = (double*)calloc(4 * 256, sizeof(double));
    if (!cnt) return v2_undef();
    for (y = g.y0; y < g.y1; y++) for (x = g.x0; x < g.x1; x++) {
        u32 c = bmd_unmul(b->px[y * b->w + x]);
        cnt[0 * 256 + ((c >> 16) & 255)]++; cnt[1 * 256 + ((c >> 8) & 255)]++; cnt[2 * 256 + (c & 255)]++; cnt[3 * 256 + (c >> 24)]++;
    }
    a[0] = v2_obj(vm->c_number->obj); cv = a2_vector_apply(vm, v2_obj(vm->c_vector->obj), 1, a);
    a[0] = cv; cvv = a2_vector_apply(vm, v2_obj(vm->c_vector->obj), 1, a);
    res = a2_new(vm, cvv.u.o->as_class); res->kind = OK_VECTOR; res->vec_type = cv.u.o->as_class;
    for (k = 0; k < 4; k++) {
        ch[k] = a2_new(vm, cv.u.o->as_class); ch[k]->kind = OK_VECTOR; ch[k]->vec_type = vm->c_number;
        for (i = 0; i < 256; i++) a2_arr_push(vm, ch[k], v2_num(cnt[k * 256 + i]));
        a2_arr_push(vm, res, v2_obj(ch[k]));
    }
    free(cnt);
    return v2_obj(res);
}
static V2 bd_encode(VM2 *vm, V2 self, int argc, V2 *argv) { UNUSED; return v2_null(); }

void a2_bitmapdata_init(VM2 *vm) {
    A2Class *c;
    vm->c_bitmapdata = c = a2_def_class(vm, "flash.display", "BitmapData", vm->c_object, OK_BITMAPDATA, BD_ctor);
    c->ctor_sig = "(int,int,Boolean=,uint=)";
    a2_add_getter(vm, c, "width", bd_width, NULL); a2_add_getter(vm, c, "height", bd_height, NULL); a2_add_getter(vm, c, "transparent", bd_transparent, NULL); a2_add_getter(vm, c, "rect", bd_rect, NULL);
    a2_add_method(vm, c, "getPixel", bd_getPixel, 2); a2_add_method(vm, c, "getPixel32", bd_getPixel32, 2); a2_add_method(vm, c, "setPixel", bd_setPixel, 3); a2_add_method(vm, c, "setPixel32", bd_setPixel32, 3);
    a2_add_method(vm, c, "getPixels", bd_getPixels, 1); a2_add_method(vm, c, "setPixels", bd_setPixels, 2); a2_add_method(vm, c, "copyPixelsToByteArray", bd_copyPixelsToByteArray, 2);
    a2_add_method(vm, c, "getVector", bd_getVector, 1); a2_add_method(vm, c, "setVector", bd_setVector, 2);
    a2_add_method(vm, c, "fillRect", bd_fillRect, 2); a2_add_method(vm, c, "floodFill", bd_floodFill, 3); a2_add_method(vm, c, "scroll", bd_scroll, 2);
    a2_add_method(vm, c, "noise", bd_noise, 5); a2_add_method(vm, c, "perlinNoise", bd_perlinNoise, 9);
    a2_add_method(vm, c, "colorTransform", bd_colorTransform, 2); a2_add_method(vm, c, "getColorBoundsRect", bd_getColorBoundsRect, 3);
    a2_add_method(vm, c, "lock", bd_lock, 0); a2_add_method(vm, c, "unlock", bd_lock, 1);
    a2_add_method(vm, c, "hitTest", bd_hitTest, 5);
    a2_add_method(vm, c, "copyPixels", bd_copyPixels, 6); a2_add_method(vm, c, "copyChannel", bd_copyChannel, 5); a2_add_method(vm, c, "merge", bd_merge, 7);
    a2_add_method(vm, c, "paletteMap", bd_paletteMap, 7); a2_add_method(vm, c, "threshold", bd_threshold, 8); a2_add_method(vm, c, "pixelDissolve", bd_pixelDissolve, 6);
    a2_add_method(vm, c, "compare", bd_compare, 1); a2_add_method(vm, c, "clone", bd_clone, 0); a2_add_method(vm, c, "dispose", bd_dispose, 0);
    a2_add_method(vm, c, "draw", bd_draw, 6); a2_add_method(vm, c, "drawWithQuality", bd_drawWithQuality, 7); a2_add_method(vm, c, "applyFilter", bd_applyFilter, 4);
    a2_add_method(vm, c, "generateFilterRect", bd_generateFilterRect, 2); a2_add_method(vm, c, "histogram", bd_histogram, 1); a2_add_method(vm, c, "encode", bd_encode, 3);
}
