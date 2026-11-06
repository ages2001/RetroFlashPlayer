#include "flashnt.h"

void swf_matrix_identity(SWFMatrix *m) {
    m->sx = 1.0; m->sy = 1.0;
    m->r0 = 0.0; m->r1 = 0.0;
    m->tx = 0.0; m->ty = 0.0;
}

/* Matrix form (SWF):  x' = sx*x + r1*y + tx
 *                     y' = r0*x + sy*y + ty
 * out = a then... actually we want: apply child(b) in parent(a) space,
 * so out(v) = a(b(v)).  Compose accordingly. */
void swf_matrix_mul(SWFMatrix *out, const SWFMatrix *a, const SWFMatrix *b) {
    SWFMatrix t;
    t.sx = a->sx * b->sx + a->r1 * b->r0;
    t.r0 = a->r0 * b->sx + a->sy * b->r0;
    t.r1 = a->sx * b->r1 + a->r1 * b->sy;
    t.sy = a->r0 * b->r1 + a->sy * b->sy;
    t.tx = a->sx * b->tx + a->r1 * b->ty + a->tx;
    t.ty = a->r0 * b->tx + a->sy * b->ty + a->ty;
    *out = t;
}

void swf_matrix_apply(const SWFMatrix *m, double x, double y,
                      double *ox, double *oy) {
    *ox = m->sx * x + m->r1 * y + m->tx;
    *oy = m->r0 * x + m->sy * y + m->ty;
}

void swf_cxform_identity(SWFCxform *c) {
    c->mr = c->mg = c->mb = c->ma = 256;
    c->ar = c->ag = c->ab = c->aa = 0;
}

static s16 clampi(int v, int lo, int hi) {
    return (s16)(v < lo ? lo : (v > hi ? hi : v));
}

/* child applied first, then parent. result mult is 8.8 */
void swf_cxform_mul(SWFCxform *out, const SWFCxform *p, const SWFCxform *c) {
    SWFCxform t;
    t.mr = clampi((p->mr * c->mr) / 256, -32768, 32767);
    t.mg = clampi((p->mg * c->mg) / 256, -32768, 32767);
    t.mb = clampi((p->mb * c->mb) / 256, -32768, 32767);
    t.ma = clampi((p->ma * c->ma) / 256, -32768, 32767);
    t.ar = clampi(p->ar + (p->mr * c->ar) / 256, -32768, 32767);
    t.ag = clampi(p->ag + (p->mg * c->ag) / 256, -32768, 32767);
    t.ab = clampi(p->ab + (p->mb * c->ab) / 256, -32768, 32767);
    t.aa = clampi(p->aa + (p->ma * c->aa) / 256, -32768, 32767);
    *out = t;
}
