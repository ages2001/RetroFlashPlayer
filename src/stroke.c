/* lyon_tessellation's stroke tessellator (fixed line width), ported from
 * lyon_tessellation 1.0 (stroke.rs) as Ruffle runs it: the same f32
 * arithmetic, the same joins, caps and folds, producing the same triangle
 * list. Overlapping triangles are kept (the GPU blends each one). */
#include "stroke.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define SP 0   /* SIDE_POSITIVE */
#define SN 1   /* SIDE_NEGATIVE */
#define NOV (-1)

typedef struct { float x, y; } LV;
static LV lv(float x, float y) { LV v; v.x = x; v.y = y; return v; }
static LV ladd(LV a, LV b) { return lv(a.x + b.x, a.y + b.y); }
static LV lsub(LV a, LV b) { return lv(a.x - b.x, a.y - b.y); }
static LV lmul(LV a, float k) { return lv(a.x * k, a.y * k); }
static LV lvdiv(LV a, float k) { return lv(a.x / k, a.y / k); }
static LV lneg(LV a) { return lv(-a.x, -a.y); }
static float ldot(LV a, LV b) { return a.x * b.x + a.y * b.y; }
static float lcross(LV a, LV b) { return a.x * b.y - a.y * b.x; }
static float lsq(LV a) { return a.x * a.x + a.y * a.y; }
static float llen(LV a) { return (float)sqrt((double)lsq(a)); }
static LV lnorm(LV a) { return lvdiv(a, llen(a)); }
static int lnan(float f) { return f != f; }

/* euclid's fast_atan2 */
static float fast_atan2f(float y, float x) {
    float xa = (float)fabs(x), ya = (float)fabs(y), a, s, r;
    a = (xa < ya ? xa : ya) / (xa > ya ? xa : ya);
    s = a * a;
    r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ya > xa) r = 1.57079637f - r;
    if (x < 0.0f) r = 3.14159274f - r;
    if (y < 0.0f) r = -r;
    return r;
}
static float angle_of(LV v) { return fast_atan2f(v.y, v.x); }
/* euclid Angle::angle_to */
static float angle_to(float from, float to) {
    float max = 3.14159274f * 2.0f, d = (float)fmod((double)(to - from), (double)max);
    return (float)fmod((double)(2.0f * d), (double)max) - d;
}

/* lyon math_utils::compute_normal */
static LV compute_normal(LV v1, LV v2) {
    LV n1 = lv(-v1.y, v1.x), v12 = ladd(v1, v2), t, n; float inv;
    if (lsq(v12) < 1e-4f) return lv(0, 0);
    t = lnorm(v12); n = lv(-t.y, t.x);
    inv = ldot(n, n1);
    if ((float)fabs(inv) < 1e-4f) return n1;
    return lvdiv(n, inv);
}

typedef struct { LV prev, next; int prev_vertex, next_vertex; int has_single; LV single; } SideP;
typedef struct { LV pos; float hw, adv; int join; int is_flat; SideP sp[2]; int fold[2]; } EP;

typedef struct {
    StrokeMesh *out;
    float hw, tol, miter_limit; int join, start_cap, end_cap;
    EP pb[3]; int pb_start, pb_count;
    EP firsts[2]; int nfirsts;
    float sub_path_start_adv, sq_merge; int may_need_empty_cap;
    LV vpos; float vhw; LV vnormal;
} SB;

static float fnan(void) { volatile double z = 0.0; return (float)(z / z); }
static void ep_default(EP *e) {
    int s;
    memset(e, 0, sizeof(*e));
    e->hw = fnan(); e->adv = e->hw;
    e->join = LJ_MITER;
    for (s = 0; s < 2; s++) { e->sp[s].prev = e->sp[s].next = lv(e->hw, e->hw); e->sp[s].prev_vertex = e->sp[s].next_vertex = NOV; e->sp[s].has_single = 0; }
}

static int add_vertex(SB *b) {
    StrokeMesh *m = b->out; LV p = ladd(b->vpos, lmul(b->vnormal, b->vhw));
    if (m->nv == m->capv) { int nc = m->capv ? m->capv * 2 : 64; float *nv = (float*)realloc(m->v, (size_t)nc * 2 * sizeof(float)); if (!nv) return NOV; m->v = nv; m->capv = nc; }
    m->v[m->nv * 2] = p.x; m->v[m->nv * 2 + 1] = p.y;
    return m->nv++;
}
static void add_tri(SB *b, int a, int c, int d) {
    StrokeMesh *m = b->out;
    if (a < 0 || c < 0 || d < 0) return;
    if (m->nt == m->capt) { int nc = m->capt ? m->capt * 2 : 64; int *nt = (int*)realloc(m->t, (size_t)nc * 3 * sizeof(int)); if (!nt) return; m->t = nt; m->capt = nc; }
    m->t[m->nt * 3] = a; m->t[m->nt * 3 + 1] = c; m->t[m->nt * 3 + 2] = d; m->nt++;
}

/* point buffer (the last three endpoints) */
static EP *pb_get(SB *b, int i) { return &b->pb[(i + b->pb_start) % 3]; }
static EP *pb_last(SB *b) { return pb_get(b, b->pb_count - 1); }
static void pb_push(SB *b, const EP *p) {
    if (b->pb_count < 3) { b->pb[b->pb_count++] = *p; return; }
    b->pb[b->pb_start] = *p; b->pb_start++; if (b->pb_start == 3) b->pb_start = 0;
}

static int miter_exceeded(LV n, float limit) { return lsq(n) > limit * limit * 4.0f; }

/* lyon_geom Line::intersection in f64 */
static int line_isect(LV p0, LV v0, LV p1, LV v1, LV *out) {
    double sx = p0.x, sy = p0.y, svx = v0.x, svy = v0.y, ox = p1.x, oy = p1.y, ovx = v1.x, ovy = v1.y, det, inv, a, bb;
    det = svx * ovy - svy * ovx;
    if (fabs(det) <= 1e-8) return 0;
    inv = 1.0 / det;
    a = sx * (sy + svy) - sy * (sx + svx);
    bb = ox * (oy + ovy) - oy * (ox + ovx);
    out->x = (float)((bb * svx - a * ovx) * inv); out->y = (float)((bb * svy - a * ovy) * inv);
    return 1;
}
static LV ltangent(LV v) { return lv(-v.y, v.x); }

static void get_clip_intersections(LV pn, LV nn, LV normal, float clip, LV *i1, LV *i2) {
    LV cp = lmul(lnorm(normal), clip), ct = ltangent(normal);
    if (!line_isect(cp, ct, pn, ltangent(pn), i1)) *i1 = normal;
    if (!line_isect(cp, ct, nn, ltangent(nn), i2)) *i2 = normal;
}

static void add_edge_triangles(SB *b, const EP *p0, const EP *p1) {
    int p0n = p0->sp[SN].next_vertex, p0p = p0->sp[SP].next_vertex, p1n = p1->sp[SN].prev_vertex, p1p = p1->sp[SP].prev_vertex;
    if (p0->fold[SP]) p0n = p0->sp[SP].prev_vertex;
    if (p0->fold[SN]) p0p = p0->sp[SN].prev_vertex;
    if (p1->fold[SP]) p1n = p1->sp[SP].next_vertex;
    if (p1->fold[SN]) p1p = p1->sp[SN].next_vertex;
    if (p0n == p1p) return;
    if (p0n != p0p && p0p != p1p) add_tri(b, p0n, p0p, p1p);
    if (p0n != p1n && p1p != p1n) add_tri(b, p0n, p1p, p1n);
}

static float circle_step(float radius, float tol) {
    if (tol > radius) tol = radius;
    return 2.0f * (float)acos((double)((radius - tol) / radius));
}
static unsigned subdivisions(float diff, float step) {
    float segs = (float)ceil((double)((float)fabs(diff) / step)), l;
    l = (float)floor((double)(float)(log((double)segs) / log(2.0)) + 0.5);
    if (!(l > 0)) return 0;
    if (l > 30) l = 30;
    return (unsigned)l;
}
static void tessellate_arc(SB *b, float a0, float a1, int va, int vb, unsigned n) {
    float mid; int v;
    if (n == 0) return;
    mid = (a0 + a1) * 0.5f;
    b->vnormal = lv((float)cos((double)mid), (float)sin((double)mid));
    v = add_vertex(b);
    add_tri(b, va, v, vb);
    tessellate_arc(b, a0, mid, va, v, n - 1);
    tessellate_arc(b, mid, a1, v, vb, n - 1);
}

static void tessellate_round_join(SB *b, EP *j, int side) {
    LV c = j->pos, sn = lsub(j->sp[side].prev, c), en = lsub(j->sp[side].next, c);
    int sv = j->sp[side].prev_vertex, ev = j->sp[side].next_vertex, t;
    float sign = side == SN ? 1.0f : -1.0f, sa = angle_of(sn), diff = angle_to(sa, angle_of(en)), ea, tf;
    if (diff * sign < 0.0f) diff = sign * (2.0f * 3.14159274f - (float)fabs(diff));
    ea = sa + diff;
    if (side == SN) { tf = sa; sa = ea; ea = tf; t = sv; sv = ev; ev = t; }
    tessellate_arc(b, sa, ea, sv, ev, subdivisions(diff, circle_step(j->hw, b->tol)));
}

static void tessellate_join(SB *b, EP *j) {
    int need[2];
    need[SP] = !j->sp[SP].has_single && !j->fold[SN];
    need[SN] = !j->sp[SN].has_single && !j->fold[SP];
    if (!j->fold[SP] && !j->fold[SN]) {
        if (need[SP] && need[SN]) {
            add_tri(b, j->sp[SP].prev_vertex, j->sp[SP].next_vertex, j->sp[SN].next_vertex);
            add_tri(b, j->sp[SP].prev_vertex, j->sp[SN].next_vertex, j->sp[SN].prev_vertex);
        } else if (!need[SP] && need[SN]) {
            add_tri(b, j->sp[SN].prev_vertex, j->sp[SP].prev_vertex, j->sp[SN].next_vertex);
        } else if (need[SP] && !need[SN]) {
            add_tri(b, j->sp[SN].prev_vertex, j->sp[SP].prev_vertex, j->sp[SP].next_vertex);
        }
    }
    if (j->join == LJ_ROUND) {
        if (need[SP]) tessellate_round_join(b, j, SP);
        if (need[SN]) tessellate_round_join(b, j, SN);
    }
}

static void add_join_base_vertices(SB *b, EP *j, int side) {
    if (j->sp[side].has_single) {
        int v; b->vnormal = lvdiv(lsub(j->sp[side].single, j->pos), j->hw); v = add_vertex(b);
        j->sp[side].prev_vertex = j->sp[side].next_vertex = v;
    } else {
        b->vnormal = lvdiv(lsub(j->sp[side].prev, j->pos), j->hw); j->sp[side].prev_vertex = add_vertex(b);
        b->vnormal = lvdiv(lsub(j->sp[side].next, j->pos), j->hw); j->sp[side].next_vertex = add_vertex(b);
    }
}

static void join_sides_fixed_width(SB *b, const EP *prev, EP *j, const EP *next) {
    LV pt = lsub(j->pos, prev->pos), nt = lsub(next->pos, j->pos), normal, fnormal, ext, n0, n1;
    float pl = llen(pt), nl = llen(nt), hw = b->vhw; int front, back, unclipped, fold = 0;
    pt = lvdiv(pt, pl); nt = lvdiv(nt, nl);
    if (lnan(j->adv)) j->adv = prev->adv + pl;
    normal = compute_normal(pt, nt);
    if (lcross(pt, nt) >= 0.0f) { front = SN; fnormal = lneg(normal); } else { front = SP; fnormal = normal; }
    back = 1 - front;
    ext = lmul(fnormal, hw);
    unclipped = (j->join == LJ_MITER || j->join == LJ_MITERCLIP) && !miter_exceeded(fnormal, b->miter_limit);
    if (!unclipped && ldot(nt, pt) < 0.0f) {
        float dn = ldot(ext, lneg(nt)) - nl, dp = ldot(ext, pt) - pl;
        if ((dn < dp ? dn : dp) > 0.0f || lsq(normal) < 1e-5f) { j->fold[front] = 1; fold = 1; }
    }
    n0 = lmul(lv(-pt.y, pt.x), hw); n1 = lmul(lv(-nt.y, nt.x), hw);
    j->sp[SP].prev = ladd(j->pos, n0); j->sp[SP].next = ladd(j->pos, n1);
    j->sp[SN].prev = lsub(j->pos, n0); j->sp[SN].next = lsub(j->pos, n1);
    if (!fold) {
        LV mp[2];
        mp[SP] = ladd(j->pos, lmul(normal, hw)); mp[SN] = lsub(j->pos, lmul(normal, hw));
        j->sp[back].has_single = 1; j->sp[back].single = mp[back];
        if (unclipped) { j->sp[front].has_single = 1; j->sp[front].single = mp[front]; }
        else if (j->join == LJ_MITERCLIP) {
            LV a = lsub(j->sp[front].prev, j->pos), c = lsub(j->sp[front].next, j->pos), pn, nn;
            get_clip_intersections(a, c, fnormal, b->miter_limit * 0.5f * hw, &pn, &nn);
            j->sp[front].prev = ladd(j->pos, pn); j->sp[front].next = ladd(j->pos, nn);
        }
    }
}

static int flattened_step(SB *b, EP *prev, EP *j, EP *next) {
    LV pe = lsub(j->pos, prev->pos), ne = lsub(next->pos, j->pos), normal, p0, p1, v0, v1;
    float pl = llen(pe), nl = llen(ne); int pv, nv;
    normal = compute_normal(lvdiv(pe, pl), lvdiv(ne, nl));
    if (lnan(j->adv)) j->adv = prev->adv + pl;
    if (lnan(next->adv)) next->adv = j->adv + nl;
    p0 = ladd(j->pos, lmul(normal, b->vhw));
    j->sp[SP].prev = j->sp[SP].next = p0; j->sp[SP].has_single = 1; j->sp[SP].single = p0;
    p1 = lsub(j->pos, lmul(normal, b->vhw));
    j->sp[SN].prev = j->sp[SN].next = p1; j->sp[SN].has_single = 1; j->sp[SN].single = p1;
    v0 = lsub(p0, prev->sp[SP].next); v1 = lsub(p1, prev->sp[SN].next);
    if (ldot(pe, v0) < 0.0f && ldot(pe, v1) < 0.0f) return 1;
    b->vnormal = normal; pv = add_vertex(b);
    b->vnormal = lneg(normal); nv = add_vertex(b);
    j->sp[SP].prev_vertex = j->sp[SP].next_vertex = pv;
    j->sp[SN].prev_vertex = j->sp[SN].next_vertex = nv;
    return 0;
}

static int too_close(SB *b, LV a, LV c) { return lsq(lsub(a, c)) < b->sq_merge; }

static int fixed_width_step(SB *b, EP next) {
    int count = b->pb_count;
    if (count > 0) {
        if (too_close(b, pb_last(b)->pos, next.pos)) { if (count == 1) b->may_need_empty_cap = 1; return 0; }
        if (count == 1) {
            EP *first = pb_last(b); LV edge = lsub(next.pos, first->pos), t, n; float len = llen(edge);
            if (lnan(next.adv)) next.adv = first->adv + len;
            t = lvdiv(edge, len); n = lmul(lv(-t.y, t.x), next.hw);
            first->sp[SP].next = ladd(first->pos, n); first->sp[SN].next = lsub(first->pos, n);
        }
    }
    if (count > 1) {
        EP *prev = pb_get(b, count - 2), *j = pb_get(b, count - 1); int fast = 0;
        b->vpos = j->pos; b->vhw = j->hw;
        if (j->is_flat) fast = ldot(lsub(j->pos, prev->pos), lsub(next.pos, j->pos)) > 0.0f;
        if (fast) { j->join = LJ_MITER; flattened_step(b, prev, j, &next); }
        else {
            join_sides_fixed_width(b, prev, j, &next);
            add_join_base_vertices(b, j, SN);
            add_join_base_vertices(b, j, SP);
        }
        if (count > 2) add_edge_triangles(b, prev, j);
        tessellate_join(b, j);
        if (count == 2 && b->nfirsts == 0) { b->firsts[0] = *prev; b->firsts[1] = *j; b->nfirsts = 2; }
    }
    pb_push(b, &next);
    return 1;
}

static void round_cap(SB *b, LV c, float r, LV sn, int sv, int ev, LV en, int is_start) {
    float sa, diff, ma, eang; unsigned n; int mv;
    (void)is_start;
    if (r < b->tol) return;
    sa = angle_of(sn); diff = angle_to(sa, angle_of(en)); ma = sa + diff; eang = ma + diff;
    n = subdivisions(diff, circle_step(r, b->tol));
    b->vpos = c; b->vhw = r;
    b->vnormal = lnorm(en); mv = add_vertex(b);
    add_tri(b, sv, mv, ev);
    tessellate_arc(b, sa, ma, sv, mv, n);
    tessellate_arc(b, ma, eang, mv, ev, n);
}

static void empty_cap(SB *b) {
    EP *p = pb_get(b, 0);
    b->vhw = p->hw; b->vpos = p->pos;
    if (b->start_cap == LC_SQUARE) {
        int a, c, d, e;
        b->vnormal = lv(1, 1); a = add_vertex(b);
        b->vnormal = lv(1, -1); c = add_vertex(b);
        b->vnormal = lv(-1, -1); d = add_vertex(b);
        b->vnormal = lv(-1, 1); e = add_vertex(b);
        add_tri(b, a, c, d); add_tri(b, a, d, e);
    } else if (b->start_cap == LC_ROUND) {
        int l, r; float rad = b->vhw; LV c = p->pos;
        b->vnormal = lv(-1, 0); l = add_vertex(b);
        b->vnormal = lv(1, 0); r = add_vertex(b);
        round_cap(b, c, rad, lv(-1, 0), l, r, lv(0, 1), 1);
        round_cap(b, c, rad, lv(1, 0), r, l, lv(0, -1), 0);
    }
}

static void last_edge(SB *b, const EP *p0, EP *p1, int is_first) {
    LV v = lsub(p1->pos, p0->pos); int side;
    p1->adv = p0->adv + llen(v);
    b->vpos = p1->pos; b->vhw = p1->hw;
    for (side = 0; side < 2; side++) {
        LV spos = p1->sp[side].prev;
        if (b->end_cap != LC_ROUND) {
            float clip = b->end_cap == LC_SQUARE ? p1->hw : 0.0f; LV n = lnorm(lsub(p1->pos, p0->pos)), is;
            if (line_isect(ladd(p1->pos, lmul(n, clip)), ltangent(n), spos, lsub(spos, p0->sp[side].next), &is)) p1->sp[side].prev = is;
        }
        b->vnormal = lvdiv(lsub(p1->sp[side].prev, p1->pos), p1->hw);
        p1->sp[side].prev_vertex = add_vertex(b);
    }
    if (!is_first) add_edge_triangles(b, p0, p1);
    if (b->end_cap == LC_ROUND) round_cap(b, p1->pos, p1->hw, lsub(p1->sp[SP].prev, p1->pos), p1->sp[SP].prev_vertex, p1->sp[SN].prev_vertex, v, 0);
}

static void first_edge(SB *b, EP *f, const EP *s) {
    int side;
    b->vpos = f->pos; b->vhw = f->hw;
    for (side = 0; side < 2; side++) {
        LV spos = f->sp[side].next;
        if (b->start_cap != LC_ROUND) {
            float clip = b->start_cap == LC_SQUARE ? f->hw : 0.0f; LV n = lnorm(lsub(f->pos, s->pos)), is;
            if (line_isect(ladd(f->pos, lmul(n, clip)), ltangent(n), spos, lsub(spos, s->sp[side].prev), &is)) spos = is;
            else spos = f->sp[side].next;
        }
        b->vnormal = lvdiv(lsub(spos, f->pos), f->hw);
        f->sp[side].next_vertex = add_vertex(b);
    }
    add_edge_triangles(b, f, s);
    if (b->start_cap == LC_ROUND) round_cap(b, f->pos, f->hw, lsub(f->sp[SN].next, f->pos), f->sp[SN].next_vertex, f->sp[SP].next_vertex, lsub(f->pos, s->pos), 1);
}

static void end_with_caps(SB *b) {
    int count = b->pb_count;
    if (b->may_need_empty_cap && count == 1) { empty_cap(b); return; }
    if (count >= 2) {
        EP p0 = *pb_get(b, count - 2), p1 = *pb_get(b, count - 1); LV t, n;
        t = lnorm(lsub(p1.pos, p0.pos)); n = lmul(lv(-t.y, t.x), p1.hw);
        p1.sp[SP].prev = ladd(p1.pos, n); p1.sp[SN].prev = lsub(p1.pos, n);
        last_edge(b, &p0, &p1, count == 2);
        b->sub_path_start_adv = p1.adv;
        if (count > 2) { p0 = b->firsts[0]; p1 = b->firsts[1]; }
        first_edge(b, &p0, &p1);
    }
}

static void close_path(SB *b) {
    EP p; int added, side;
    if (b->pb_count <= 2 || b->nfirsts < 1) return;
    p = b->firsts[0]; p.adv = fnan();
    added = fixed_width_step(b, p);
    if (!added) pb_last(b)->pos = p.pos;
    if (b->nfirsts >= 2) {
        EP *p0, *p1;
        fixed_width_step(b, b->firsts[1]);
        p0 = pb_get(b, b->pb_count - 2); p1 = pb_get(b, b->pb_count - 1);
        b->vpos = p0->pos; b->vhw = p0->hw;
        for (side = 0; side < 2; side++) {
            if (p0->sp[side].has_single) b->vnormal = lvdiv(lsub(p0->sp[side].single, p0->pos), p0->hw);
            else b->vnormal = lvdiv(lsub(p0->sp[side].next, p0->pos), p0->hw);
            p0->sp[side].next_vertex = add_vertex(b);
        }
        add_edge_triangles(b, p0, p1);
    }
}

/* ---- lyon_geom quadratic flattening (with the stroke tessellator's sharp-turn split) ---- */
static float pint(float x) { const float d = 0.67f; return x / (1.0f - d + (float)sqrt(sqrt((double)(d * d * d * d + 0.25f * x * x)))); }
static float pinv(float x) { const float bb = 0.39f; return x * (1.0f - bb + (float)sqrt((double)(bb * bb + 0.25f * x * x))); }
static LV qsample(LV f, LV c, LV t2p, float t) {
    float tt = t * t, ot = 1.0f - t, ot2 = ot * ot;
    return lv(f.x * ot2 + c.x * 2.0f * ot * t + t2p.x * tt, f.y * ot2 + c.y * 2.0f * ot * t + t2p.y * tt);
}
static LV llerp(LV a, LV c, float t) { float o = 1.0f - t; return lv(o * a.x + t * c.x, o * a.y + t * c.y); }

static LV cub_s(LV f, LV a, LV b, LV t2p, float t) {
    float tt = t * t, t3 = tt * t, o = 1.0f - t, o2 = o * o, o3 = o2 * o;
    return lv(f.x * o3 + a.x * 3.0f * o2 * t + b.x * 3.0f * o * tt + t2p.x * t3, f.y * o3 + a.y * 3.0f * o2 * t + b.y * 3.0f * o * tt + t2p.y * t3);
}
static void stroke_point(SB *b, LV pos, int is_flat);
/* a quadratic flattened with tolerance `tol`; the points are flattening steps
   except the final one of the last quadratic of a cubic */
static void quad_flat_t(SB *b, LV f, LV c, LV t, float tol, int last) {
    float vx = t.x - f.x, vy = t.y - f.y, wx = c.x - f.x, wy = c.y - f.y, cr = vx * wy - vy * wx, d, count = 0;
    int n = 0, i;
    if (!(f.x == t.x && f.y == t.y)) {
        d = (cr * cr) / (vx * vx + vy * vy);
        if (!(d <= tol * tol * 4.0f)) {
            float ddx = 2.0f * c.x - f.x - t.x, ddy = 2.0f * c.y - f.y - t.y, cross = (t.x - f.x) * ddy - (t.y - f.y) * ddx, inv = 1.0f / cross;
            float pf = ((c.x - f.x) * ddx + (c.y - f.y) * ddy) * inv, pt = ((t.x - c.x) * ddx + (t.y - c.y) * ddy) * inv;
            float scale = (float)fabs(cross) / ((float)sqrt((double)(ddx * ddx + ddy * ddy)) * (float)fabs(pt - pf));
            float i0, i1, idiff, u0, u1, divd, step;
            i0 = (float)0; i1 = (float)0;
            {
                const float dd = 0.67f, bb = 0.39f;
                i0 = pf / (1.0f - dd + (float)sqrt(sqrt((double)(dd * dd * dd * dd + 0.25f * pf * pf))));
                i1 = pt / (1.0f - dd + (float)sqrt(sqrt((double)(dd * dd * dd * dd + 0.25f * pt * pt))));
                idiff = i1 - i0;
                u0 = i0 * (1.0f - bb + (float)sqrt((double)(bb * bb + 0.25f * i0 * i0)));
                u1 = i1 * (1.0f - bb + (float)sqrt((double)(bb * bb + 0.25f * i1 * i1)));
                divd = 1.0f / (u1 - u0);
                count = (float)ceil(0.5f * (float)fabs(idiff) * (float)sqrt((double)(scale / tol)));
                if (!(count == count) || count > 1e6f || count < -1e6f) count = 0;
                step = idiff / count;
                n = (int)count;
                for (i = 1; i < n; i++) {
                    float x = i0 + step * (float)i, u = x * (1.0f - bb + (float)sqrt((double)(bb * bb + 0.25f * x * x))), tt = (u - u0) * divd;
                    stroke_point(b, qsample(f, c, t, tt), 1);
                }
            }
        }
    }
    stroke_point(b, t, !last);
}
static void stroke_point(SB *b, LV pos, int is_flat) {
    EP e; ep_default(&e);
    e.pos = pos; e.hw = b->hw; e.join = b->join; e.is_flat = is_flat;
    fixed_width_step(b, e);
}
static void quad_flat(SB *b, LV f, LV c, LV t) {
    float vx = t.x - f.x, vy = t.y - f.y, wx = c.x - f.x, wy = c.y - f.y, cr = vx * wy - vy * wx, d, count = 0, tol = b->tol;
    int n = 0, i;
    if (!(f.x == t.x && f.y == t.y)) {
        d = (cr * cr) / (vx * vx + vy * vy);
        if (!(d <= tol * tol * 4.0f)) {
            float ddx = 2.0f * c.x - f.x - t.x, ddy = 2.0f * c.y - f.y - t.y, cross = (t.x - f.x) * ddy - (t.y - f.y) * ddx, inv = 1.0f / cross;
            float pf = ((c.x - f.x) * ddx + (c.y - f.y) * ddy) * inv, pt = ((t.x - c.x) * ddx + (t.y - c.y) * ddy) * inv;
            float scale = (float)fabs(cross) / ((float)sqrt((double)(ddx * ddx + ddy * ddy)) * (float)fabs(pt - pf));
            float i0 = pint(pf), i1 = pint(pt), idiff = i1 - i0, u0 = pinv(i0), u1 = pinv(i1), divd = 1.0f / (u1 - u0), step;
            count = (float)ceil(0.5f * (float)fabs(idiff) * (float)sqrt((double)(scale / tol)));
            if (!(count == count) || count > 1e6f || count < -1e6f) count = 0;
            step = idiff / count;
            n = (int)count;
            for (i = 1; i < n; i++) {
                float u = pinv(i0 + step * (float)i), tt = (u - u0) * divd;
                stroke_point(b, qsample(f, c, t, tt), 1);
            }
        }
    }
    stroke_point(b, t, 0);
}
/* lyon stroke.rs find_sharp_turn */
static int find_sharp_turn(LV f, LV c, LV t, float *ts) {
    LV base = lsub(t, f), v = lsub(c, f), n = lv(-base.y, base.x), longax, rc, rt; float vb = ldot(v, base), vn = ldot(v, n), ang, sn, cs, div, tt;
    if ((vb >= 0.0f && vb <= ldot(base, base)) || (float)fabs(vn) * 2.0f >= (float)fabs(vb)) {
        if (lsq(base) * 30.0f > lsq(v)) return 0;
        longax = v;
    } else longax = base;
    ang = -angle_of(longax); sn = (float)sin((double)ang); cs = (float)cos((double)ang);
    rc = lv(v.x * cs - v.y * sn, v.y * cs + v.x * sn);
    rt = lv(base.x * cs - base.y * sn, base.y * cs + base.x * sn);
    div = 0.0f - 2.0f * rc.x + rt.x;
    if (div == 0.0f) return 0;
    tt = (0.0f - rc.x) / div;
    if (tt > 0.0f && tt < 1.0f) { *ts = tt; return 1; }
    return 0;
}

void lyon_stroke(StrokeMesh *out, const StrokeEv *ev, int nev, const StrokeStyle *st) {
    SB b; int i; LV cur = lv(0, 0);
    memset(&b, 0, sizeof(b));
    b.out = out; b.hw = st->width * 0.5f; b.tol = st->tol; b.miter_limit = st->miter_limit; b.join = st->join; b.start_cap = st->start_cap; b.end_cap = st->end_cap;
    b.sq_merge = st->tol * st->tol * 0.5f; if (st->width * st->width * 0.05f < b.sq_merge) b.sq_merge = st->width * st->width * 0.05f; if (b.sq_merge < 1e-8f) b.sq_merge = 1e-8f;
    b.vhw = b.hw;
    for (i = 0; i < nev; i++) {
        const StrokeEv *e = &ev[i];
        switch (e->kind) {
        case SE_BEGIN: {
            EP p; ep_default(&p);
            b.may_need_empty_cap = 0;
            p.pos = lv(e->x, e->y); p.hw = b.hw; p.adv = b.sub_path_start_adv; p.join = b.join;
            cur = p.pos;
            fixed_width_step(&b, p);
            break;
        }
        case SE_LINE: stroke_point(&b, lv(e->x, e->y), 0); cur = lv(e->x, e->y); break;
        case SE_QUAD: {
            LV f = cur, c = lv(e->cx, e->cy), t = lv(e->x, e->y); float ts;
            if (find_sharp_turn(f, c, t, &ts)) {
                LV sp = qsample(f, c, t, ts);
                quad_flat(&b, f, llerp(f, c, ts), sp);
                quad_flat(&b, sp, llerp(c, t, ts), t);
            } else quad_flat(&b, f, c, t);
            cur = t;
            break;
        }
        case SE_CUBIC: {
            /* lyon_geom CubicBezierSegment::for_each_flattened_with_t */
            LV f = cur, p1 = lv(e->cx, e->cy), p2 = lv(e->c2x, e->c2y), t = lv(e->x, e->y);
            float qt = b.tol * 0.4f, x, y, err, nq, step, t0 = 0.0f; int n, q;
            x = f.x - 3.0f * p1.x + 3.0f * p2.x - t.x; y = f.y - 3.0f * p1.y + 3.0f * p2.y - t.y;
            err = x * x + y * y;
            nq = (float)ceil(pow((double)(err / (432.0f * qt * qt)), 1.0 / 6.0)); if (!(nq >= 1.0f)) nq = 1.0f;
            step = 1.0f / nq; n = (int)nq;
            for (q = 0; q < n; q++) {
                float t1 = q < n - 1 ? t0 + step : 1.0f, dt = t1 - t0; LV s0, s1, d0, d1, k1, k2, a1, a2, qc;
                s0 = cub_s(f, p1, p2, t, t0); s1 = cub_s(f, p1, p2, t, t1);
                d0 = qsample(lsub(p1, f), lsub(p2, p1), lsub(t, p2), t0); d1 = qsample(lsub(p1, f), lsub(p2, p1), lsub(t, p2), t1);
                k1 = ladd(s0, lmul(d0, dt)); k2 = lsub(s1, lmul(d1, dt));
                a1 = lmul(lsub(lmul(k1, 3.0f), s0), 0.5f); a2 = lmul(lsub(lmul(k2, 3.0f), s1), 0.5f); qc = lmul(ladd(a1, a2), 0.5f);
                quad_flat_t(&b, s0, qc, s1, b.tol * 0.8f, q == n - 1);
                t0 = t1;
            }
            cur = t;
            break;
        }
        case SE_END: case SE_CLOSE: {
            int close = e->kind == SE_CLOSE;
            b.may_need_empty_cap |= close && b.pb_count == 1;
            if (close && b.pb_count > 2) close_path(&b); else end_with_caps(&b);
            b.pb_count = 0; b.pb_start = 0; b.nfirsts = 0;
            break;
        }
        default: break;
        }
    }
}

void stroke_mesh_free(StrokeMesh *m) { free(m->v); free(m->t); memset(m, 0, sizeof(*m)); }
