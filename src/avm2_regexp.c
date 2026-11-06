/* avm2_regexp.c - ECMAScript regular expressions (backtracking matcher)
 * and the String methods that use them (split / match / replace / search). */
#include "avm2_int.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

extern int a2_utf8_decode(const char *s, u32 len, u32 **out);
extern A2Str *a2_utf8_encode(VM2 *vm, const u32 *u, int n);

/* ---------------------------------------------------------------- */
/* AST                                                              */

enum { RN_CHAR, RN_ANY, RN_CLASS, RN_SEQ, RN_ALT, RN_GROUP, RN_REPEAT, RN_BOL, RN_EOL, RN_WORDB, RN_NWORDB, RN_BACKREF, RN_LOOKAHEAD, RN_NLOOKAHEAD, RN_EMPTY, RN_LOOKBEHIND, RN_NLOOKBEHIND };

typedef struct RNode {
    int type;
    u32 ch;
    /* class: ranges */
    u32 *ranges; int nranges; int negate;
    struct RNode **kids; int nkids;
    int min, max, lazy;          /* repeat */
    int group;                   /* capture index (0 = non-capturing) */
    char *gname;
} RNode;

typedef struct Regex {
    RNode *root; int ngroups; int icase, multiline, dotall, extended, global;
    int never;                   /* a pattern PCRE refuses to compile: it matches nothing */
    char **names;                /* per group (NULL if unnamed) */
} Regex;

typedef struct { const u32 *p, *end; int ngroups; int icase, extended; int err; char **names; int ncap; int fail, inlb; } RP;

/* PCRE look-behind: every top-level alternative must have a fixed length (nested alternatives
   one common length); -1 if variable */
static int rn_fixlen(RNode *n) {
    int i, l, t = 0;
    switch (n->type) {
    case RN_CHAR: case RN_ANY: case RN_CLASS: return 1;
    case RN_SEQ: for (i = 0; i < n->nkids; i++) { l = rn_fixlen(n->kids[i]); if (l < 0) return -1; t += l; } return t;
    case RN_ALT: for (i = 0; i < n->nkids; i++) { l = rn_fixlen(n->kids[i]); if (l < 0 || (i && l != t)) return -1; t = l; } return t;
    case RN_GROUP: return n->nkids ? rn_fixlen(n->kids[0]) : 0;
    case RN_REPEAT: if (n->max != n->min) return -1; l = rn_fixlen(n->kids[0]); return l < 0 ? -1 : l * n->min;
    case RN_BACKREF: return -1;
    default: return 0;
    }
}

static RNode *rn_new(int type) { RNode *n = (RNode*)calloc(1, sizeof(RNode)); n->type = type; n->max = -1; return n; }
static void rn_add(RNode *p, RNode *k) { p->kids = (RNode**)realloc(p->kids, (size_t)(p->nkids + 1) * sizeof(RNode*)); p->kids[p->nkids++] = k; }
static void rn_free(RNode *n) { int i; if (!n) return; for (i = 0; i < n->nkids; i++) rn_free(n->kids[i]); free(n->kids); free(n->ranges); free(n->gname); free(n); }
static void cls_add(RNode *c, u32 a, u32 b) { c->ranges = (u32*)realloc(c->ranges, (size_t)(c->nranges + 1) * 2 * sizeof(u32)); c->ranges[c->nranges * 2] = a; c->ranges[c->nranges * 2 + 1] = b; c->nranges++; }
static void cls_add_escape(RNode *c, u32 e) {
    switch (e) {
    case 'd': cls_add(c, '0', '9'); break;
    case 'w': cls_add(c, 'a', 'z'); cls_add(c, 'A', 'Z'); cls_add(c, '0', '9'); cls_add(c, '_', '_'); break;
    case 's': cls_add(c, ' ', ' '); cls_add(c, '\t', '\r'); cls_add(c, 0xA0, 0xA0); cls_add(c, 0x2028, 0x2029); cls_add(c, 0xFEFF, 0xFEFF); break;
    }
}
static RNode *neg_class(u32 e) { RNode *c = rn_new(RN_CLASS); cls_add_escape(c, e); c->negate = 1; return c; }

static RNode *parse_alt(RP *p);

static u32 parse_escape_char(RP *p) {
    u32 c = *p->p++;
    switch (c) {
    case 'n': return '\n'; case 'r': return '\r'; case 't': return '\t'; case 'f': return '\f'; case 'v': return '\v'; case '0': return 0;
    case 'x': { u32 v = 0; int i; for (i = 0; i < 2 && p->p < p->end; i++) { u32 h = *p->p; if (h >= '0' && h <= '9') v = v * 16 + h - '0'; else if (h >= 'a' && h <= 'f') v = v * 16 + h - 'a' + 10; else if (h >= 'A' && h <= 'F') v = v * 16 + h - 'A' + 10; else break; p->p++; } return v; }
    case 'u': { u32 v = 0; int i; for (i = 0; i < 4 && p->p < p->end; i++) { u32 h = *p->p; if (h >= '0' && h <= '9') v = v * 16 + h - '0'; else if (h >= 'a' && h <= 'f') v = v * 16 + h - 'a' + 10; else if (h >= 'A' && h <= 'F') v = v * 16 + h - 'A' + 10; else break; p->p++; } return v; }
    case 'c': if (p->p < p->end) return *p->p++ % 32; return 'c';
    default: return c;
    }
}

static RNode *parse_class(RP *p) {
    RNode *c = rn_new(RN_CLASS);
    if (p->p < p->end && *p->p == '^') { c->negate = 1; p->p++; }
    if (p->p < p->end && *p->p == ']') { cls_add(c, ']', ']'); p->p++; }
    while (p->p < p->end && *p->p != ']') {
        u32 a, b;
        if (*p->p == '\\' && p->p + 1 < p->end) {
            u32 e = p->p[1];
            if (e == 'd' || e == 'w' || e == 's') { cls_add_escape(c, e); p->p += 2; continue; }
            if (e == 'D' || e == 'W' || e == 'S') { /* negated inside class: approximate by complement ranges */
                RNode t; memset(&t, 0, sizeof(t)); cls_add_escape(&t, e + 32);
                { u32 lo = 0; int i; /* ranges are small; build complement by sorting */
                  for (;;) { u32 best = 0xFFFFFFFFu, bhi = 0; int any = 0; for (i = 0; i < t.nranges; i++) if (t.ranges[i * 2] >= lo && t.ranges[i * 2] < best) { best = t.ranges[i * 2]; bhi = t.ranges[i * 2 + 1]; any = 1; } if (!any) { cls_add(c, lo, 0x10FFFF); break; } if (best > lo) cls_add(c, lo, best - 1); lo = bhi + 1; } free(t.ranges); }
                p->p += 2; continue;
            }
            if (e == 'b') { a = '\b'; p->p += 2; } else { p->p++; a = parse_escape_char(p); }
        } else a = *p->p++;
        b = a;
        if (p->p + 1 < p->end && *p->p == '-' && p->p[1] != ']') {
            p->p++;
            if (*p->p == '\\' && p->p + 1 < p->end) { p->p++; b = parse_escape_char(p); } else b = *p->p++;
        }
        if (p->icase) { if (a >= 'a' && a <= 'z' && b <= 'z') cls_add(c, a - 32, b - 32); if (a >= 'A' && a <= 'Z' && b <= 'Z') cls_add(c, a + 32, b + 32); }
        cls_add(c, a, b);
    }
    if (p->p < p->end) p->p++;    /* ] */
    return c;
}

static RNode *parse_atom(RP *p) {
    u32 c;
    if (p->p >= p->end) return NULL;
    c = *p->p;
    if (p->extended) { while (p->p < p->end && (*p->p == ' ' || *p->p == '\t' || *p->p == '\n' || *p->p == '\r')) p->p++; if (p->p >= p->end) return NULL; c = *p->p; if (c == '#') { while (p->p < p->end && *p->p != '\n') p->p++; return rn_new(RN_EMPTY); } }
    switch (c) {
    case '(': {
        RNode *g; int cap = 1, look = 0, neg = 0, lb = 0; char *name = NULL;
        p->p++;
        if (p->p + 2 < p->end && *p->p == '?' && p->p[1] == '<' && (p->p[2] == '=' || p->p[2] == '!')) {
            lb = 1; neg = p->p[2] == '!'; cap = 0; p->p += 3;
            g = rn_new(neg ? RN_NLOOKBEHIND : RN_LOOKBEHIND);
            p->inlb++; rn_add(g, parse_alt(p)); p->inlb--;
            if (p->p < p->end && *p->p == ')') p->p++; else p->err = 1;
            {   RNode *b = g->kids[0]; int i;
                if (b && b->type == RN_ALT) { for (i = 0; i < b->nkids; i++) if (rn_fixlen(b->kids[i]) < 0) p->fail = 1; }
                else if (b && rn_fixlen(b) < 0) p->fail = 1; }
            (void)lb; return g;
        }
        if (p->p + 1 < p->end && *p->p == '?') {
            if (p->p[1] == ':') { cap = 0; p->p += 2; }
            else if (p->p[1] == '=') { look = 1; cap = 0; p->p += 2; }
            else if (p->p[1] == '!') { look = 1; neg = 1; cap = 0; p->p += 2; }
            else if (p->p[1] == 'P' && p->p + 2 < p->end && p->p[2] == '<') { const u32 *s; p->p += 3; s = p->p; while (p->p < p->end && *p->p != '>') p->p++; { int n = (int)(p->p - s), i; name = (char*)malloc((size_t)n + 1); for (i = 0; i < n; i++) name[i] = (char)s[i]; name[n] = 0; } if (p->p < p->end) p->p++; }
            else if (p->p[1] == '<' && p->p + 2 < p->end && p->p[2] != '=' && p->p[2] != '!') { const u32 *s; p->p += 2; s = p->p; while (p->p < p->end && *p->p != '>') p->p++; { int n = (int)(p->p - s), i; name = (char*)malloc((size_t)n + 1); for (i = 0; i < n; i++) name[i] = (char)s[i]; name[n] = 0; } if (p->p < p->end) p->p++; }
            else { /* inline flags (?i) etc. */ p->p++; while (p->p < p->end && *p->p != ')') { if (*p->p == 'i') p->icase = 1; if (*p->p == 'x') p->extended = 1; p->p++; } if (p->p < p->end) p->p++; return rn_new(RN_EMPTY); }
        }
        g = rn_new(look ? (neg ? RN_NLOOKAHEAD : RN_LOOKAHEAD) : RN_GROUP);
        if (cap) { g->group = ++p->ngroups; p->names = (char**)realloc(p->names, (size_t)(p->ngroups + 1) * sizeof(char*)); p->names[p->ngroups] = name; }
        rn_add(g, parse_alt(p));
        if (p->p < p->end && *p->p == ')') p->p++; else p->err = 1;
        return g; }
    case '[': p->p++; return parse_class(p);
    case '.': p->p++; return rn_new(RN_ANY);
    case '^': p->p++; return rn_new(RN_BOL);
    case '$': p->p++; return rn_new(RN_EOL);
    case '\\': {
        u32 e;
        p->p++; if (p->p >= p->end) { RNode *n = rn_new(RN_CHAR); n->ch = '\\'; return n; }
        e = *p->p;
        if (e == 'd' || e == 'w' || e == 's') { RNode *n = rn_new(RN_CLASS); cls_add_escape(n, e); p->p++; return n; }
        if (e == 'D' || e == 'W' || e == 'S') { p->p++; return neg_class(e + 32); }
        if (e == 'b') { p->p++; return rn_new(RN_WORDB); }
        if (e == 'B') { p->p++; return rn_new(RN_NWORDB); }
        if (e >= '1' && e <= '9') { RNode *n = rn_new(RN_BACKREF); n->group = 0; while (p->p < p->end && *p->p >= '0' && *p->p <= '9') n->group = n->group * 10 + (int)(*p->p++ - '0'); return n; }
        { RNode *n = rn_new(RN_CHAR); n->ch = parse_escape_char(p); return n; } }
    case ')': return NULL;
    case '*': case '+': case '?': p->err = 1; p->p++; return rn_new(RN_EMPTY);
    default: { RNode *n = rn_new(RN_CHAR); n->ch = c; p->p++; return n; }
    }
}

static RNode *parse_seq(RP *p) {
    RNode *seq = rn_new(RN_SEQ);
    while (p->p < p->end && *p->p != '|' && *p->p != ')') {
        RNode *a = parse_atom(p);
        if (!a) break;
        if (p->extended) while (p->p < p->end && (*p->p == ' ' || *p->p == '\t' || *p->p == '\n')) p->p++;
        /* quantifier */
        while (p->p < p->end) {
            u32 q = *p->p; int min, max;
            if (q == '*') { min = 0; max = -1; p->p++; }
            else if (q == '+') { min = 1; max = -1; p->p++; }
            else if (q == '?') { min = 0; max = 1; p->p++; }
            else if (q == '{') {
                const u32 *s = p->p + 1; int a1 = 0, b1 = -1, ok = 0;
                if (s < p->end && *s >= '0' && *s <= '9') { while (s < p->end && *s >= '0' && *s <= '9') a1 = a1 * 10 + (int)(*s++ - '0'); b1 = a1;
                    if (s < p->end && *s == ',') { s++; if (s < p->end && *s >= '0' && *s <= '9') { b1 = 0; while (s < p->end && *s >= '0' && *s <= '9') b1 = b1 * 10 + (int)(*s++ - '0'); } else b1 = -1; }
                    if (s < p->end && *s == '}') { ok = 1; s++; } }
                if (!ok) break;
                p->p = s; min = a1; max = b1;
            } else break;
            { RNode *r = rn_new(RN_REPEAT); r->min = min; r->max = max; if (p->p < p->end && *p->p == '?') { r->lazy = 1; p->p++; } rn_add(r, a); a = r; }
        }
        rn_add(seq, a);
    }
    return seq;
}
static RNode *parse_alt(RP *p) {
    RNode *first = parse_seq(p), *alt;
    if (p->p >= p->end || *p->p != '|') return first;
    alt = rn_new(RN_ALT); rn_add(alt, first);
    while (p->p < p->end && *p->p == '|') { p->p++; rn_add(alt, parse_seq(p)); }
    return alt;
}

Regex *a2_regex_compile(const char *src, const char *flags) {
    Regex *re = (Regex*)calloc(1, sizeof(Regex)); RP p; u32 *u; int n;
    const char *f;
    for (f = flags ? flags : ""; *f; f++) { if (*f == 'i') re->icase = 1; else if (*f == 'm') re->multiline = 1; else if (*f == 's') re->dotall = 1; else if (*f == 'x') re->extended = 1; else if (*f == 'g') re->global = 1; }
    n = a2_utf8_decode(src, (u32)strlen(src), &u);
    memset(&p, 0, sizeof(p)); p.p = u; p.end = u + n; p.icase = re->icase; p.extended = re->extended;
    p.names = (char**)calloc(1, sizeof(char*));
    re->root = parse_alt(&p);
    if (p.p < p.end) p.err = 1;      /* unbalanced ) */
    re->ngroups = p.ngroups; re->names = p.names; re->never = p.fail;
    free(u);
    if (p.err) { /* keep it anyway; unmatched tokens degrade to literals */ }
    return re;
}
void a2_regex_free(Regex *re) { int i; if (!re) return; rn_free(re->root); for (i = 0; i <= re->ngroups; i++) free(re->names[i]); free(re->names); free(re); }

/* ---------------------------------------------------------------- */
/* matcher (continuation-passing backtracking)                      */

typedef struct { const u32 *s; int n; int *caps; Regex *re; int steps; } MS;

typedef struct Cont { int (*fn)(MS *m, int pos, struct Cont *c); RNode *node; struct Cont *next; int count; int start; } Cont;

static int match_node(MS *m, RNode *n, int pos, Cont *k);
static int run_cont(MS *m, int pos, Cont *k) { if (!k) return pos; return k->fn(m, pos, k); }

static int is_word(u32 c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
static u32 fold(u32 c) { if (c >= 'A' && c <= 'Z') return c + 32; if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32; return c; }
static int class_has(RNode *n, u32 c, int icase) {
    int i, hit = 0;
    for (i = 0; i < n->nranges; i++) { if (c >= n->ranges[i * 2] && c <= n->ranges[i * 2 + 1]) { hit = 1; break; } if (icase) { u32 f = fold(c), g = (c >= 'a' && c <= 'z') ? c - 32 : c; if ((f >= n->ranges[i * 2] && f <= n->ranges[i * 2 + 1]) || (g >= n->ranges[i * 2] && g <= n->ranges[i * 2 + 1])) { hit = 1; break; } } }
    return n->negate ? !hit : hit;
}

static int seq_cont(MS *m, int pos, Cont *c) {
    /* c->node = seq, c->count = index of next kid */
    RNode *seq = c->node;
    if (c->count >= seq->nkids) return run_cont(m, pos, c->next);
    { Cont nc; nc.fn = seq_cont; nc.node = seq; nc.next = c->next; nc.count = c->count + 1; nc.start = 0; return match_node(m, seq->kids[c->count], pos, &nc); }
}
static int group_close(MS *m, int pos, Cont *c) {
    int g = c->node->group, saved0 = m->caps[g * 2], saved1 = m->caps[g * 2 + 1], r;
    m->caps[g * 2] = c->start; m->caps[g * 2 + 1] = pos;
    r = run_cont(m, pos, c->next);
    if (r < 0) { m->caps[g * 2] = saved0; m->caps[g * 2 + 1] = saved1; }
    return r;
}
static int repeat_cont(MS *m, int pos, Cont *c);
static int repeat_try(MS *m, RNode *r, int pos, int count, int start, Cont *next) {
    /* try one more iteration (if allowed) then rest, or rest only, in greedy/lazy order */
    int res;
    if (m->steps++ > 2000000) return -1;
    if (r->lazy) {
        if (count >= r->min) { res = run_cont(m, pos, next); if (res >= 0) return res; }
        if (r->max < 0 || count < r->max) { Cont nc; nc.fn = repeat_cont; nc.node = r; nc.next = next; nc.count = count + 1; nc.start = pos; return match_node(m, r->kids[0], pos, &nc); }
        return -1;
    }
    if ((r->max < 0 || count < r->max) && !(count > r->min && pos == start)) {
        Cont nc; nc.fn = repeat_cont; nc.node = r; nc.next = next; nc.count = count + 1; nc.start = pos;
        res = match_node(m, r->kids[0], pos, &nc);
        if (res >= 0) return res;
    }
    if (count >= r->min) return run_cont(m, pos, next);
    return -1;
}
static int repeat_cont(MS *m, int pos, Cont *c) {
    /* an iteration of the repeated node finished at pos; prevent empty infinite loops */
    if (pos == c->start && c->count > c->node->min) return -1;
    return repeat_try(m, c->node, pos, c->count, c->start, c->next);
}
static int look_end(MS *m, int pos, Cont *c) { (void)m; (void)c; return pos; }

static int match_node(MS *m, RNode *n, int pos, Cont *k) {
    const u32 *s = m->s; int len = m->n;
    if (m->steps++ > 2000000) return -1;
    switch (n->type) {
    case RN_EMPTY: return run_cont(m, pos, k);
    case RN_CHAR: if (pos < len && (s[pos] == n->ch || (m->re->icase && fold(s[pos]) == fold(n->ch)))) return run_cont(m, pos + 1, k); return -1;
    case RN_ANY: if (pos < len && (m->re->dotall || (s[pos] != '\n' && s[pos] != '\r' && s[pos] != 0x2028 && s[pos] != 0x2029))) return run_cont(m, pos + 1, k); return -1;
    case RN_CLASS: if (pos < len && class_has(n, s[pos], m->re->icase)) return run_cont(m, pos + 1, k); return -1;
    case RN_SEQ: { Cont c; c.fn = seq_cont; c.node = n; c.next = k; c.count = 0; c.start = 0; return seq_cont(m, pos, &c); }
    case RN_ALT: { int i; for (i = 0; i < n->nkids; i++) { int r = match_node(m, n->kids[i], pos, k); if (r >= 0) return r; } return -1; }
    case RN_GROUP:
        if (n->group) { Cont c; c.fn = group_close; c.node = n; c.next = k; c.count = 0; c.start = pos; return match_node(m, n->kids[0], pos, &c); }
        return match_node(m, n->kids[0], pos, k);
    case RN_REPEAT: return repeat_try(m, n, pos, 0, -1, k);
    case RN_BOL: if (pos == 0 || (m->re->multiline && (s[pos - 1] == '\n' || s[pos - 1] == '\r'))) return run_cont(m, pos, k); return -1;
    case RN_EOL: if (pos == len || (m->re->multiline && (s[pos] == '\n' || s[pos] == '\r'))) return run_cont(m, pos, k); return -1;
    case RN_WORDB: case RN_NWORDB: { int a = pos > 0 && is_word(s[pos - 1]), b = pos < len && is_word(s[pos]); int at = a != b; if ((n->type == RN_WORDB) == !!at) return run_cont(m, pos, k); return -1; }
    case RN_BACKREF: {
        int g = n->group, st, en, i;
        if (g > m->re->ngroups) return -1;
        st = m->caps[g * 2]; en = m->caps[g * 2 + 1];
        if (st < 0 || en < 0) return run_cont(m, pos, k);
        if (pos + (en - st) > len) return -1;
        for (i = 0; i < en - st; i++) if (s[pos + i] != s[st + i] && !(m->re->icase && fold(s[pos + i]) == fold(s[st + i]))) return -1;
        return run_cont(m, pos + (en - st), k); }
    case RN_LOOKAHEAD: case RN_NLOOKAHEAD: {
        Cont c; int r; int *saved = (int*)malloc((size_t)(m->re->ngroups + 1) * 2 * sizeof(int));
        memcpy(saved, m->caps, (size_t)(m->re->ngroups + 1) * 2 * sizeof(int));
        c.fn = look_end; c.node = n; c.next = NULL; c.count = 0; c.start = 0;
        r = match_node(m, n->kids[0], pos, &c);
        if (n->type == RN_NLOOKAHEAD) { memcpy(m->caps, saved, (size_t)(m->re->ngroups + 1) * 2 * sizeof(int)); free(saved); if (r >= 0) return -1; return run_cont(m, pos, k); }
        if (r < 0) { memcpy(m->caps, saved, (size_t)(m->re->ngroups + 1) * 2 * sizeof(int)); free(saved); return -1; }
        r = run_cont(m, pos, k);
        if (r < 0) memcpy(m->caps, saved, (size_t)(m->re->ngroups + 1) * 2 * sizeof(int));
        free(saved); return r; }
    case RN_LOOKBEHIND: case RN_NLOOKBEHIND: {
        /* each fixed-length alternative must end exactly here */
        RNode *b = n->kids[0]; int i, nalt = (b && b->type == RN_ALT) ? b->nkids : 1, hit = 0, r;
        size_t csz = (size_t)(m->re->ngroups + 1) * 2 * sizeof(int); int *saved = (int*)malloc(csz);
        memcpy(saved, m->caps, csz);
        for (i = 0; i < nalt && !hit; i++) {
            RNode *a = (b && b->type == RN_ALT) ? b->kids[i] : b; int L = a ? rn_fixlen(a) : 0;
            if (L < 0 || pos - L < 0) continue;
            if (!a || match_node(m, a, pos - L, NULL) == pos) hit = 1;
        }
        if (n->type == RN_NLOOKBEHIND) { memcpy(m->caps, saved, csz); free(saved); if (hit) return -1; return run_cont(m, pos, k); }
        if (!hit) { memcpy(m->caps, saved, csz); free(saved); return -1; }
        r = run_cont(m, pos, k);
        if (r < 0) memcpy(m->caps, saved, csz);
        free(saved); return r; }
    }
    return -1;
}

/* try to match at positions >= start; returns match start or -1; caps filled (2*(ngroups+1)) */
int a2_regex_exec(Regex *re, const u32 *s, int n, int start, int *caps) {
    MS m; int pos;
    m.s = s; m.n = n; m.caps = caps; m.re = re;
    if (re->never) return -1;
    for (pos = start; pos <= n; pos++) {
        int i, end;
        for (i = 0; i < (re->ngroups + 1) * 2; i++) caps[i] = -1;
        m.steps = 0;
        end = match_node(&m, re->root, pos, NULL);
        if (end >= 0) { caps[0] = pos; caps[1] = end; return pos; }
    }
    return -1;
}

/* ---------------------------------------------------------------- */
/* RegExp class                                                     */

typedef struct { Regex *re; A2Str *source, *flags; } RxPayload;

static RxPayload *rx_of(VM2 *vm, V2 v) {
    (void)vm;
    if (v.t != T_OBJ || v.u.o->kind != OK_REGEXP) return NULL;
    return (RxPayload*)v.u.o->payload;
}
static V2 RegExp_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = self.t == T_OBJ ? self.u.o : NULL; RxPayload *rp; A2Str *src, *flags;
    if (!o) return v2_undef();
    o->kind = OK_REGEXP;
    if (argc > 0 && argv[0].t == T_OBJ && argv[0].u.o->kind == OK_REGEXP) {
        RxPayload *q = (RxPayload*)argv[0].u.o->payload;
        if (argc > 1 && argv[1].t != T_UNDEF) { a2_throw_error(vm, "TypeError", "Cannot supply flags when constructing one RegExp from another.", 1100); return v2_undef(); }
        src = q->source; flags = q->flags;
    }
    else { src = (argc > 0 && argv[0].t != T_UNDEF) ? v2_tostr(vm, argv[0]) : vm->s_empty; flags = (argc > 1 && argv[1].t != T_UNDEF) ? v2_tostr(vm, argv[1]) : vm->s_empty; }
    rp = (RxPayload*)calloc(1, sizeof(RxPayload));
    rp->source = src; rp->flags = flags; rp->re = a2_regex_compile(src->len ? src->s : "(?:)", flags->s);
    o->payload = rp; o->ival = 0;   /* lastIndex */
    return v2_undef();
}
static V2 RegExp_call(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; if (argc == 1 && argv[0].t == T_OBJ && argv[0].u.o->kind == OK_REGEXP) return argv[0]; return a2_construct_class(vm, vm->c_regexp, argc, argv); }
void a2_regexp_free_payload(A2Obj *o) { RxPayload *rp = (RxPayload*)o->payload; if (rp) { a2_regex_free(rp->re); free(rp); o->payload = NULL; } }
void a2_regexp_mark_payload(VM2 *vm, A2Obj *o) { RxPayload *rp = (RxPayload*)o->payload; (void)vm; if (rp) { rp->source->gc.mark = 1; rp->flags->gc.mark = 1; } }

/* RegExpObject::_exec (avmplus): with the g flag the search starts at lastIndex and lastIndex is
 * set to the end of the match (0 when nothing matches); the result array holds the groups up to
 * the highest one that matched, unmatched groups are undefined. */
static V2 exec_at(VM2 *vm, A2Obj *ro, A2Str *s, const u32 *u, int n, int start, int *mindex, int *mlen) {
    RxPayload *rp = (RxPayload*)ro->payload; int *caps, r, i, results; A2Obj *arr;
    *mindex = 0; *mlen = 0;
    if (!rp) return v2_null();
    if (start < 0 || start > n) return v2_null();
    caps = (int*)malloc((size_t)(rp->re->ngroups + 1) * 2 * sizeof(int));
    r = a2_regex_exec(rp->re, u, n, start, caps);
    if (r < 0) { free(caps); return v2_null(); }
    results = rp->re->ngroups + 1;
    arr = a2_new_array(vm);
    a2_dyn_set(vm, arr, a2_str(vm, "index"), v2_int(r));
    a2_dyn_set(vm, arr, a2_str(vm, "input"), v2_str(s));
    for (i = 0; i < results; i++) {
        if (caps[i * 2] >= 0) a2_arr_push(vm, arr, v2_str(a2_utf8_encode(vm, u + caps[i * 2], caps[i * 2 + 1] - caps[i * 2])));
        else a2_arr_push(vm, arr, v2_undef());
    }
    for (i = 1; i <= rp->re->ngroups; i++) if (rp->re->names[i]) a2_dyn_set(vm, arr, a2_str(vm, rp->re->names[i]), caps[i * 2] >= 0 ? v2_str(a2_utf8_encode(vm, u + caps[i * 2], caps[i * 2 + 1] - caps[i * 2])) : v2_str(vm->s_empty));
    *mindex = caps[0]; *mlen = caps[1] - caps[0];
    free(caps);
    return v2_obj(arr);
}
static V2 exec_impl(VM2 *vm, A2Obj *ro, A2Str *s, int set_last) {
    RxPayload *rp = (RxPayload*)ro->payload; u32 *u; int n, mi, ml, start; V2 r;
    if (!rp) return v2_null();
    n = a2_utf8_decode(s->s, s->len, &u);
    start = rp->re->global ? ro->ival : 0;
    r = exec_at(vm, ro, s, u, n, start, &mi, &ml);
    if (rp->re->global && set_last) ro->ival = mi + ml;
    free(u);
    return r;
}
static V2 rx_exec(VM2 *vm, V2 self, int argc, V2 *argv) { if (!rx_of(vm, self)) return v2_null(); return exec_impl(vm, self.u.o, v2_tostr(vm, a2_arg(argc, argv, 0)), 1); }
static V2 rx_test(VM2 *vm, V2 self, int argc, V2 *argv) { V2 r = rx_exec(vm, self, argc, argv); return v2_bool(r.t == T_OBJ); }
static V2 rx_source(VM2 *vm, V2 self, int argc, V2 *argv) { RxPayload *rp = rx_of(vm, self); (void)argc; (void)argv; return rp ? v2_str(rp->source) : v2_undef(); }
static V2 rx_global(VM2 *vm, V2 self, int argc, V2 *argv) { RxPayload *rp = rx_of(vm, self); (void)argc; (void)argv; return v2_bool(rp && rp->re->global); }
static V2 rx_ignoreCase(VM2 *vm, V2 self, int argc, V2 *argv) { RxPayload *rp = rx_of(vm, self); (void)argc; (void)argv; return v2_bool(rp && rp->re->icase); }
static V2 rx_multiline(VM2 *vm, V2 self, int argc, V2 *argv) { RxPayload *rp = rx_of(vm, self); (void)argc; (void)argv; return v2_bool(rp && rp->re->multiline); }
static V2 rx_dotall(VM2 *vm, V2 self, int argc, V2 *argv) { RxPayload *rp = rx_of(vm, self); (void)argc; (void)argv; return v2_bool(rp && rp->re->dotall); }
static V2 rx_extended(VM2 *vm, V2 self, int argc, V2 *argv) { RxPayload *rp = rx_of(vm, self); (void)argc; (void)argv; return v2_bool(rp && rp->re->extended); }
static V2 rx_lastIndex_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)vm; (void)argc; (void)argv; return v2_int(self.t == T_OBJ ? self.u.o->ival : 0); }
static V2 rx_lastIndex_set(VM2 *vm, V2 self, int argc, V2 *argv) { if (self.t == T_OBJ) self.u.o->ival = v2_toint(vm, a2_arg(argc, argv, 0)); return v2_undef(); }
static V2 rx_toString(VM2 *vm, V2 self, int argc, V2 *argv) { RxPayload *rp = rx_of(vm, self); char *b, fl[8]; int k = 0; V2 r; (void)argc; (void)argv; if (!rp) { if (self.t == T_OBJ && vm->c_regexp && self.u.o != vm->c_regexp->prototype) { a2_throw_coerce_error(vm, self, vm->c_regexp); return v2_undef(); } return v2_cstr(vm, "//"); } if (rp->re->global) fl[k++] = 'g'; if (rp->re->icase) fl[k++] = 'i'; if (rp->re->multiline) fl[k++] = 'm'; if (rp->re->dotall) fl[k++] = 's'; if (rp->re->extended) fl[k++] = 'x'; fl[k] = 0; b = (char*)malloc(rp->source->len + 12); sprintf(b, "/%s/%s", rp->source->s, fl); r = v2_cstr(vm, b); free(b); return r; }

/* ---------------------------------------------------------------- */
/* String helpers                                                   */

static Regex *tmp_regex_for(VM2 *vm, V2 v, int *owned) {
    RxPayload *rp = rx_of(vm, v);
    *owned = 0;
    if (rp) return rp->re;
    return NULL;
}

V2 a2_str_split(VM2 *vm, A2Str *s, V2 sep, u32 ulimit) {
    A2Obj *arr = a2_new_array(vm); int owned; Regex *re; A2Obj *ro; int limit = ulimit > 0x7fffffffUL ? 0x7fffffff : (int)ulimit;
    if (limit <= 0) return v2_obj(arr);
    re = tmp_regex_for(vm, sep, &owned);
    if (!re) {
        A2Str *d = v2_tostr(vm, sep);
        if (d->len == 0 && s->len == 0) { a2_arr_push(vm, arr, v2_str(s)); return v2_obj(arr); }
        if (d->len == 0) { u32 *u; int n, i; n = a2_utf8_decode(s->s, s->len, &u); for (i = 0; i < n && arr->alen < limit; i++) a2_arr_push(vm, arr, v2_str(a2_utf8_encode(vm, u + i, 1))); free(u); return v2_obj(arr); }
        { const char *p = s->s, *q; while ((q = strstr(p, d->s)) != NULL && arr->alen < limit) { a2_arr_push(vm, arr, v2_str(a2_intern(vm, p, (size_t)(q - p)))); p = q + d->len; } if (arr->alen < limit) a2_arr_push(vm, arr, v2_str(a2_intern(vm, p, strlen(p)))); }
        return v2_obj(arr);
    }
    ro = sep.u.o;
    {   /* RegExpObject::split (avmplus), including its limit quirk for capture groups */
        u32 *u; int n, start = 0, mi, ml, cnt = 0, empty_re = ((RxPayload*)ro->payload)->source->len == 0; V2 m;
        n = a2_utf8_decode(s->s, s->len, &u);
        while ((m = exec_at(vm, ro, s, u, n, start, &mi, &ml)).t == T_OBJ) {
            if (ml == 0) { ml = 0; mi = start + 1; if (!empty_re) break; }
            if (mi + ml > n) { start = mi + ml; break; }
            a2_arr_push(vm, arr, v2_str(a2_utf8_encode(vm, u + start, mi - start))); cnt++;
            if (cnt >= limit) break;
            { int j; for (j = 1; j < m.u.o->alen; j++) { a2_arr_push(vm, arr, a2_arr_get(m.u.o, j)); cnt++; if (cnt >= limit) break; } }
            start = mi + ml;
        }
        if (cnt < limit && start <= n) a2_arr_push(vm, arr, v2_str(a2_utf8_encode(vm, u + start, n - start)));
        free(u);
    }
    return v2_obj(arr);
}

V2 a2_str_match(VM2 *vm, A2Str *s, V2 pat) {
    Regex *re; int owned; A2Obj *ro;
    if (pat.t == T_UNDEF || pat.t == T_NULL) return v2_null();
    if (!rx_of(vm, pat)) { V2 a[1]; a[0] = v2_str(v2_tostr(vm, pat)); pat = a2_construct_class(vm, vm->c_regexp, 1, a); }
    ro = pat.u.o; re = tmp_regex_for(vm, pat, &owned);
    if (!re->global) return exec_impl(vm, ro, s, 0);
    {   /* RegExpObject::match (avmplus) */
        A2Obj *arr = a2_new_array(vm); u32 *u; int n, old = ro->ival, mi, ml;
        n = a2_utf8_decode(s->s, s->len, &u);
        ro->ival = 0;
        for (;;) {
            int last = ro->ival; V2 m = exec_at(vm, ro, s, u, n, ro->ival, &mi, &ml);
            ro->ival = mi + ml;
            if (m.t != T_OBJ || last == ro->ival) break;
            a2_arr_push(vm, arr, a2_arr_get(m.u.o, 0));
        }
        if (ro->ival == old) ro->ival++;
        free(u);
        return v2_obj(arr);
    }
}

V2 a2_str_search(VM2 *vm, A2Str *s, V2 pat) {
    Regex *re; int owned; u32 *u; int n, *caps, r;
    if (!rx_of(vm, pat)) { V2 a[1]; a[0] = v2_str(v2_tostr(vm, pat)); pat = a2_construct_class(vm, vm->c_regexp, 1, a); }
    re = tmp_regex_for(vm, pat, &owned);
    n = a2_utf8_decode(s->s, s->len, &u); caps = (int*)malloc((size_t)(re->ngroups + 1) * 2 * sizeof(int));
    r = a2_regex_exec(re, u, n, 0, caps);
    free(u); free(caps);
    return v2_int(r);
}

typedef struct { char *buf; size_t n, cap; } RB;
static void rb_put(RB *b, const char *s, size_t n) { if (b->n + n + 1 > b->cap) { while (b->n + n + 1 > b->cap) b->cap *= 2; b->buf = (char*)realloc(b->buf, b->cap); } memcpy(b->buf + b->n, s, n); b->n += n; }
static void rb_putu(VM2 *vm, RB *b, const u32 *u, int n) { A2Str *s = a2_utf8_encode(vm, u, n); rb_put(b, s->s, s->len); }
static void expand_replacement(VM2 *vm, RB *b, A2Str *rep, const u32 *u, int n, int *caps, int ngroups, char **names) {
    u32 i;
    for (i = 0; i < rep->len; i++) {
        char c = rep->s[i];
        if (c == '$' && i + 1 < rep->len) {
            char d = rep->s[i + 1];
            if (d == '$') { rb_put(b, "$", 1); i++; continue; }
            if (d == '&') { rb_putu(vm, b, u + caps[0], caps[1] - caps[0]); i++; continue; }
            if (d == '`') { rb_putu(vm, b, u, caps[0]); i++; continue; }
            if (d == '\'') { rb_putu(vm, b, u + caps[1], n - caps[1]); i++; continue; }
            if (d >= '0' && d <= '9') {
                int g = d - '0', used = 1;
                if (i + 2 < rep->len && rep->s[i + 2] >= '0' && rep->s[i + 2] <= '9' && g * 10 + (rep->s[i + 2] - '0') <= ngroups && g * 10 + (rep->s[i + 2] - '0') >= 1) { g = g * 10 + (rep->s[i + 2] - '0'); used = 2; }
                if (g >= 1 && g <= ngroups) { if (caps[g * 2] >= 0) rb_putu(vm, b, u + caps[g * 2], caps[g * 2 + 1] - caps[g * 2]); i += (used == 2 && d == '0') ? 1 : used; continue; }   /* Flash consumes one digit after a leading 0 */
            }
            if (d == '<') { const char *e = strchr(rep->s + i + 2, '>'); if (e) { int g; size_t nl = (size_t)(e - (rep->s + i + 2)); for (g = 1; g <= ngroups; g++) if (names && names[g] && strlen(names[g]) == nl && !strncmp(names[g], rep->s + i + 2, nl)) { if (caps[g * 2] >= 0) rb_putu(vm, b, u + caps[g * 2], caps[g * 2 + 1] - caps[g * 2]); break; } i = (u32)(e - rep->s); continue; } }
        }
        rb_put(b, &c, 1);
    }
}

V2 a2_str_replace(VM2 *vm, A2Str *s, V2 pat, V2 rep) {
    RB b;
    if (vm->player && vm->player->debug) fprintf(stderr, "[replace] '%s' pat type %d '%s' rep '%s'\n", s->s, pat.t, v2_tostr(vm, pat)->s, v2_tostr(vm, rep)->s); Regex *re; int owned; V2 r; int isfn = rep.t == T_OBJ && (rep.u.o->kind == OK_FUNCTION || rep.u.o->kind == OK_CLASS);
    A2Str *reps = isfn ? NULL : v2_tostr(vm, rep);
    b.cap = s->len + 64; b.n = 0; b.buf = (char*)malloc(b.cap);
    re = tmp_regex_for(vm, pat, &owned);
    if (!re) {
        A2Str *d = v2_tostr(vm, pat); const char *q = d->len ? strstr(s->s, d->s) : s->s;
        if (!q) { free(b.buf); return v2_str(s); }
        rb_put(&b, s->s, (size_t)(q - s->s));
        if (isfn) { V2 args[3]; V2 rv; args[0] = v2_str(d); args[1] = v2_int((s32)(q - s->s)); args[2] = v2_str(s); rv = a2_call(vm, rep, v2_null(), 3, args); { A2Str *rs = v2_tostr(vm, rv); rb_put(&b, rs->s, rs->len); } }
        else { u32 *u; int n, caps[2]; n = a2_utf8_decode(s->s, s->len, &u); { int pre; u32 *up; pre = a2_utf8_decode(s->s, (u32)(q - s->s), &up); free(up); caps[0] = pre; { u32 *ud; int dn = a2_utf8_decode(d->s, d->len, &ud); free(ud); caps[1] = pre + dn; } } expand_replacement(vm, &b, reps, u, n, caps, 0, NULL); free(u); }
        rb_put(&b, q + d->len, strlen(q + d->len));
        r = v2_str(a2_intern(vm, b.buf, b.n)); free(b.buf); return r;
    }
    {
        u32 *u; int n, *caps, pos = 0, last = 0;
        n = a2_utf8_decode(s->s, s->len, &u); caps = (int*)malloc((size_t)(re->ngroups + 1) * 2 * sizeof(int));
        while (pos <= n) {
            int m = a2_regex_exec(re, u, n, pos, caps);
            if (m < 0) break;
            rb_putu(vm, &b, u + last, caps[0] - last);
            if (isfn) {
                V2 *args = (V2*)malloc((size_t)(re->ngroups + 3) * sizeof(V2)); int i; V2 rv; A2Str *rs;
                args[0] = v2_str(a2_utf8_encode(vm, u + caps[0], caps[1] - caps[0]));
                for (i = 1; i <= re->ngroups; i++) args[i] = caps[i * 2] < 0 ? v2_str(vm->s_empty) : v2_str(a2_utf8_encode(vm, u + caps[i * 2], caps[i * 2 + 1] - caps[i * 2]));
                args[re->ngroups + 1] = v2_int(caps[0]); args[re->ngroups + 2] = v2_str(s);
                rv = a2_call(vm, rep, v2_null(), re->ngroups + 3, args); free(args);
                rs = v2_tostr(vm, rv); rb_put(&b, rs->s, rs->len);
            } else expand_replacement(vm, &b, reps, u, n, caps, re->ngroups, re->names);
            last = caps[1];
            if (caps[1] == caps[0]) { if (caps[0] < n) rb_putu(vm, &b, u + caps[0], 1); last = caps[0] + 1; pos = caps[0] + 1; } else pos = caps[1];
            if (!re->global) break;
        }
        if (last < n) rb_putu(vm, &b, u + last, n - last);
        free(u); free(caps);
    }
    r = v2_str(a2_intern(vm, b.buf, b.n)); free(b.buf); return r;
}

void a2_regexp_init(VM2 *vm) {
    A2Class *c = a2_class_new(vm, "", "RegExp", vm->c_object, OK_REGEXP);
    extern void a2_global_class(VM2 *vm, A2Class *c);
    vm->c_regexp = c; c->native_ctor = RegExp_ctor; c->native_call = RegExp_call; c->is_dynamic = 1;
    a2_add_method(vm, c, "exec", rx_exec, 1); a2_add_method(vm, c, "test", rx_test, 1);
    a2_add_getter(vm, c, "source", rx_source, NULL); a2_add_getter(vm, c, "global", rx_global, NULL); a2_add_getter(vm, c, "ignoreCase", rx_ignoreCase, NULL);
    a2_add_getter(vm, c, "multiline", rx_multiline, NULL); a2_add_getter(vm, c, "dotall", rx_dotall, NULL); a2_add_getter(vm, c, "extended", rx_extended, NULL);
    a2_add_getter(vm, c, "lastIndex", rx_lastIndex_get, rx_lastIndex_set);
    a2_add_method(vm, c, "toString", rx_toString, 0);
    a2_dyn_set_hidden(vm, c->prototype, vm->s_toString, v2_obj(a2_new_native_fn(vm, rx_toString, "toString", 0)));
    a2_dyn_set_hidden(vm, c->prototype, a2_str(vm, "exec"), v2_obj(a2_new_native_fn(vm, rx_exec, "exec", 1)));
    a2_dyn_set_hidden(vm, c->prototype, a2_str(vm, "test"), v2_obj(a2_new_native_fn(vm, rx_test, "test", 1)));
    a2_global_class(vm, c);
}
