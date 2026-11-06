/* avm1_xml.c - the AVM1 XML and XMLNode classes.
 *
 * The document tree is a graph of XNode payloads, each owned by a hidden
 * GC object so that the collector can free a tree once nothing refers to
 * it any more.  A node gets its script object (an XMLNode instance, or
 * the XML object for a document root) lazily, the first time a script
 * sees it.  Semantics, including the slightly odd sibling bookkeeping and
 * the reversed attribute order of toString, follow the Flash Player as
 * documented by the Ruffle test suite.
 *
 * The parser is a small hand written reader that reproduces the events
 * (and the errors) of the XML reader Ruffle uses. */
#include "avm1_int.h"
#include <stdlib.h>
#include <string.h>

#define DE  PA_DONT_ENUM
#define DD  PA_DONT_DELETE
#define RO  PA_READ_ONLY

#define ELEMENT_NODE 1
#define TEXT_NODE    3

extern AObj *lib_class(VM *vm, Env *env, NativeFn ctor, NativeFn call, AObj *super_proto, int proto_first, AObj **proto_out);

typedef struct XNode {
    AObj   *gcobj;                   /* hidden GC object owning this payload */
    struct XNode *parent, *prev, *next;
    struct XNode **kids; int nkids, capkids;
    u8      type;
    u8      ns_fixed;                /* namespaceURI was settled by the parser */
    AStr   *name;                    /* nodeName (the tag of an element); NULL: none */
    AStr   *value;                   /* nodeValue (the text of a text node); NULL: none */
    AStr   *ns_uri;                  /* the parser's namespaceURI */
    u32     visit;                   /* walk stamp (trees can contain cycles) */
    AObj   *attrs;                   /* attribute object (no prototype) */
    AObj   *script;                  /* script object, NULL until first needed */
    AObj   *cached;                  /* cached childNodes array */
} XNode;

typedef struct XDoc {
    XNode  *root;
    AObj   *id_map;
    AStr   *decl, *doctype;      /* xmlDecl, docTypeDecl (NULL: undefined) */
    AStr   *content_type;
    int     status;
    int     loaded;              /* -1: undefined */
    int     ignore_white;
} XDoc;

enum { XS_OK = 0, XS_CDATA = -2, XS_DECL = -3, XS_DOCTYPE = -4, XS_COMMENT = -5,
       XS_MALFORMED = -6, XS_OOM = -7, XS_ATTR = -8, XS_MISMATCH_START = -9, XS_MISMATCH_END = -10 };

/* Attribute objects are walked newest first: that is the order Flash
 * enumerates properties in, and parsing defines them in reverse. */

/* ---------------------------------------------------------------- */
/* GC                                                               */

static void xnode_free(AObj *o) {
    XNode *x = (XNode*)o->nat.p;
    if (x) { free(x->kids); free(x); }
    o->nat.p = NULL;
}

static u32 g_visit;
/* the top of a tree (a tree made with cycles by old players: where the walk comes round) */
static XNode *tree_top(XNode *x) {
    XNode *r = x; u32 stamp = ++g_visit;
    while (r->parent && r->parent->visit != stamp) { r->visit = stamp; r = r->parent; }
    return r;
}

static void xnode_mark(VM *vm, AObj *o) {
    XNode *x = (XNode*)o->nat.p, *r, **st, **all; int sp = 0, cap = 64, n = 0, acap = 64, i; u32 stamp;
    if (!x) return;
    /* mark the whole tree at once, iteratively (trees can be deep and
     * sibling chains long) */
    r = tree_top(x);
    stamp = ++g_visit;
    st = (XNode**)malloc((size_t)cap * sizeof(XNode*));
    all = (XNode**)malloc((size_t)acap * sizeof(XNode*));
    if (!st || !all) { free(st); free(all); return; }
    st[sp++] = r; r->visit = stamp;
    while (sp > 0) {
        XNode *c = st[--sp];
        c->gcobj->gc.mark = 1;
        if (n == acap) { XNode **na = (XNode**)realloc(all, (size_t)acap * 2 * sizeof(XNode*)); if (!na) break; all = na; acap *= 2; }
        all[n++] = c;
        for (i = c->nkids - 1; i >= 0; i--) {
            XNode *k = c->kids[i];
            if (k->visit == stamp || (k->gcobj->gc.mark && k != x)) continue;
            k->visit = stamp;
            if (sp == cap) { XNode **ns = (XNode**)realloc(st, (size_t)cap * 2 * sizeof(XNode*)); if (!ns) { sp = 0; break; } st = ns; cap *= 2; }
            st[sp++] = k;
        }
    }
    free(st);
    for (i = 0; i < n; i++) {
        XNode *c = all[i];
        gc_mark_str(c->name); gc_mark_str(c->value); gc_mark_str(c->ns_uri);
        if (c->attrs) gc_mark_obj(vm, c->attrs);
        if (c->script) gc_mark_obj(vm, c->script);
        if (c->cached) gc_mark_obj(vm, c->cached);
        if (c->prev) gc_mark_obj(vm, c->prev->gcobj);
        if (c->next) gc_mark_obj(vm, c->next->gcobj);
    }
    free(all);
}

static void script_mark(VM *vm, AObj *o) {
    XNode *x = (XNode*)o->nat.p;
    if (x) gc_mark_obj(vm, x->gcobj);
}

static void doc_free(AObj *o) { free(o->nat.p); o->nat.p = NULL; }

static void doc_mark(VM *vm, AObj *o) {
    XDoc *d = (XDoc*)o->nat.p;
    if (!d) return;
    if (d->root) gc_mark_obj(vm, d->root->gcobj);
    if (d->id_map) gc_mark_obj(vm, d->id_map);
    gc_mark_str(d->decl); gc_mark_str(d->doctype); gc_mark_str(d->content_type);
}

/* ---------------------------------------------------------------- */
/* the tree                                                         */

static XNode *xn_new(VM *vm, int type, AStr *value) {
    XNode *x = (XNode*)calloc(1, sizeof(XNode)); AObj *g;
    if (!x) return NULL;
    g = obj_bare(vm);
    g->nat.p = x; g->nfree = xnode_free; g->nmark = xnode_mark;
    x->gcobj = g;
    x->type = (u8)type;
    if (type == ELEMENT_NODE) x->name = value; else x->value = value;
    x->attrs = obj_bare(vm);
    return x;
}

static XNode *as_node(AObj *o) {
    if (o && o->nk == NK_SUPER) o = o->nat.sup.self;
    if (!o) return NULL;
    if (o->nk == NK_XMLNODE) return (XNode*)o->nat.p;
    if (o->nk == NK_XML) { XDoc *d = (XDoc*)o->nat.p; return d ? d->root : NULL; }
    return NULL;
}

static XDoc *as_doc(AObj *o) {
    if (o && o->nk == NK_SUPER) o = o->nat.sup.self;
    return o && o->nk == NK_XML ? (XDoc*)o->nat.p : NULL;
}

static XNode *v_node(Value v) { return v.t == V_OBJ ? as_node(v.u.o) : NULL; }

static void introduce_script(XNode *x, AObj *o) {
    o->nk = NK_XMLNODE; o->nat.p = x; o->nmark = script_mark; o->nfree = NULL;
    x->script = o;
}

static AObj *script_of(Act *a, XNode *x) {
    if (!x->script) {
        AObj *ctor = PROTOS(a)->xmlnode_ctor, *o;
        Value proto = ctor ? o_get(a, ctor, a->vm->s_prototype) : v_undef();
        o = obj_new_v(a->vm, proto);
        if (x->script) return x->script;   /* the getter above could have run a script */
        introduce_script(x, o);
    }
    return x->script;
}

static Value script_or_null(Act *a, XNode *x) { return x ? v_obj(script_of(a, x)) : v_null(); }

static int child_pos(XNode *p, XNode *c) {
    int i;
    for (i = 0; i < p->nkids; i++) if (p->kids[i] == c) return i;
    return -1;
}

static void kids_remove(XNode *p, int pos) {
    memmove(p->kids + pos, p->kids + pos + 1, (size_t)(p->nkids - pos - 1) * sizeof(XNode*));
    p->nkids--;
}

static int kids_insert(XNode *p, int pos, XNode *c) {
    if (p->nkids == p->capkids) {
        int nc = p->capkids ? p->capkids * 2 : 4;
        XNode **nk = (XNode**)realloc(p->kids, (size_t)nc * sizeof(XNode*));
        if (!nk) return 0;
        p->kids = nk; p->capkids = nc;
    }
    if (pos > p->nkids) pos = p->nkids;
    memmove(p->kids + pos + 1, p->kids + pos, (size_t)(p->nkids - pos) * sizeof(XNode*));
    p->kids[pos] = c; p->nkids++;
    return 1;
}

static void refresh_cache(Act *a, XNode *x);
/* Flash Player 10 and older let SWF 5-7 movies make a node its own ancestor */
static int cycles_allowed(Act *a) { return a && a->swf_version < 8 && avm1_player_major() < 11; }

static void insert_child(Act *a, XNode *self, int pos, XNode *child) {
    XNode *an, *old = child->parent; u32 stamp = ++g_visit;
    if (!cycles_allowed(a))
        for (an = self; an && an->visit != stamp; an = an->parent) { if (an == child) return; an->visit = stamp; }   /* cyclic */
    if (old && old != self) {
        int op = child_pos(old, child);
        if (op >= 0) kids_remove(old, op);
        if (a) refresh_cache(a, old);
    }
    child->parent = self;
    if (!kids_insert(self, pos, child)) return;
    if (pos > self->nkids - 1) pos = self->nkids - 1;
    {
        XNode *np = pos > 0 ? self->kids[pos - 1] : NULL;
        XNode *nn = pos + 1 < self->nkids ? self->kids[pos + 1] : NULL;
        if (np) np->next = child;
        if (nn) nn->prev = child;
        child->prev = np; child->next = nn;
    }
}

static void append_child(Act *a, XNode *self, XNode *child) { insert_child(a, self, self->nkids, child); }

/* a node below itself (made by an old player): walking such a tree never ends */
static int has_cycle(XNode *x) {
    XNode **st; int sp = 0, cap = 64, i, cyc = 0; u32 stamp = ++g_visit;
    st = (XNode**)malloc((size_t)cap * sizeof(XNode*));
    if (!st) return 0;
    st[sp++] = x; x->visit = stamp;
    while (sp > 0 && !cyc) {
        XNode *c = st[--sp];
        for (i = 0; i < c->nkids; i++) {
            XNode *k = c->kids[i];
            if (k->visit == stamp) { cyc = 1; break; }
            k->visit = stamp;
            if (sp == cap) { XNode **ns = (XNode**)realloc(st, (size_t)cap * 2 * sizeof(XNode*)); if (!ns) { sp = 0; break; } st = ns; cap *= 2; }
            st[sp++] = k;
        }
    }
    free(st);
    return cyc;
}

static void remove_node(XNode *x) {
    if (x->parent) {
        int pos = child_pos(x->parent, x);
        if (pos >= 0) kids_remove(x->parent, pos);
        if (x->prev) x->prev->next = x->next;
        if (x->next) x->next->prev = x->prev;
        x->prev = x->next = NULL;
        x->parent = NULL;
    }
}

static void refresh_cache(Act *a, XNode *x) {
    int i;
    if (!x->cached) return;
    o_set_length(a, x->cached, 0);
    for (i = 0; i < x->nkids && !a->vm->err; i++) o_set_element(a, x->cached, i, v_obj(script_of(a, x->kids[i])));
}

static AObj *child_nodes(Act *a, XNode *x) {
    if (!x->cached) {
        x->cached = obj_array(a, 0, NULL);
        refresh_cache(a, x);
    }
    return x->cached;
}

static XNode *duplicate(VM *vm, XNode *x, int deep) {
    XNode *c = xn_new(vm, x->type, NULL); int i;
    if (!c) return NULL;
    c->name = x->name; c->value = x->value; c->ns_uri = x->ns_uri; c->ns_fixed = x->ns_fixed;
    for (i = x->attrs->props.n - 1; i >= 0; i--) {
        Prop *p = &x->attrs->props.items[i];
        if (!p->key || (p->attr & PA_DONT_ENUM)) continue;
        o_define(vm, c->attrs, p->key, p->v, 0);
    }
    if (deep)
        for (i = 0; i < x->nkids; i++) {
            XNode *k = duplicate(vm, x->kids[i], deep);
            if (k) insert_child(NULL, c, i, k);
        }
    return c;
}

static AStr *node_name(XNode *x) { return x->name; }
static AStr *node_value(XNode *x) { return x->value; }

static int starts_xmlns(AStr *k) { return str_starts_c(k, "xmlns"); }

/* xmlns:p declares the prefix p; any other xmlns... attribute (xmlns, a poorly
   formed "xmlns:", even xmlnsaaa) the default namespace */
static int lookup_ns(Act *a, XNode *x, AStr *prefix, Value *out) {
    XNode *n; int i; u32 stamp = ++g_visit;
    (void)a;
    for (n = x; n && n->visit != stamp; n = n->parent) {
        n->visit = stamp;
        for (i = n->attrs->props.n - 1; i >= 0; i--) {
            Prop *p = &n->attrs->props.items[i]; AStr *k = p->key;
            if (!k || (p->attr & PA_DONT_ENUM) || !starts_xmlns(k)) continue;
            if (prefix->len == 0) { if (k->len <= 6 || STR_AT(k, 5) != ':') { *out = p->v; return 1; } continue; }
            if (k->len == 6 + prefix->len && STR_AT(k, 5) == ':') {
                u32 j; int same = 1;
                for (j = 0; j < prefix->len; j++) if (STR_AT(k, 6 + j) != STR_AT(prefix, j)) { same = 0; break; }
                if (same) { *out = p->v; return 1; }
            }
        }
    }
    return 0;
}

/* quick-xml's escape(): & < > ' " */
static void sb_escaped(SBuf *b, AStr *s) {
    u32 i;
    for (i = 0; i < s->len; i++) {
        u16 c = STR_AT(s, i);
        switch (c) {
        case '&': sb_ascii(b, "&amp;"); break;
        case '<': sb_ascii(b, "&lt;"); break;
        case '>': sb_ascii(b, "&gt;"); break;
        case '\'': sb_ascii(b, "&apos;"); break;
        case '"': sb_ascii(b, "&quot;"); break;
        default: sb_unit(b, c);
        }
    }
}

static void write_node(Act *a, XNode *x, SBuf *b, int depth) {
    int i;
    if (a->vm->err || depth > 10000) return;
    if (depth == 0 && has_cycle(x)) { a->vm->err = ERR_TIMEOUT; return; }   /* Flash never comes back */
    if (x->type == ELEMENT_NODE) {
        /* an element without a name (the document, or a node whose nodeValue
           took the name away) writes its attributes and children only */
        if (x->name) { sb_unit(b, '<'); sb_str(b, x->name); }
        for (i = x->attrs->props.n - 1; i >= 0; i--) {
            Prop *p = &x->attrs->props.items[i]; AStr *v;
            if (!p->key || (p->attr & PA_DONT_ENUM)) continue;
            v = v_str_of(a, p->v); if (a->vm->err) return;
            sb_unit(b, ' '); sb_str(b, p->key); sb_ascii(b, "=\""); sb_escaped(b, v); sb_unit(b, '"');
        }
        if (x->name && x->nkids == 0) sb_ascii(b, " />");
        else {
            if (x->name) sb_unit(b, '>');
            for (i = 0; i < x->nkids; i++) write_node(a, x->kids[i], b, depth + 1);
            if (x->name) { sb_ascii(b, "</"); sb_str(b, x->name); sb_unit(b, '>'); }
        }
    } else if (x->value) sb_escaped(b, x->value);
}

/* ---------------------------------------------------------------- */
/* the parser                                                       */

typedef struct XP {
    Act *a; VM *vm;
    const u16 *u; u32 n, pos;
    XNode **open; int nopen, capopen;
    XDoc *doc;
    int ignore_white;
} XP;

static AStr *prefix_of(VM *vm, XNode *x);
static int is_ws(u16 c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static AStr *mk(XP *p, const u16 *u, u32 n) { return str_from_units(p->vm, u, n); }

/* &lt; &gt; &amp; &quot; &apos; &#N; &#xH; (anything else stays as is) */
static AStr *unescape(XP *p, const u16 *u, u32 n) {
    SBuf b; u32 i = 0;
    sb_init(&b);
    while (i < n) {
        u16 c = u[i];
        if (c == '&') {
            u32 j = i + 1; int done = 0;
            while (j < n && u[j] != ';' && j - i < 32) j++;
            if (j < n && u[j] == ';') {
                const u16 *e = u + i + 1; u32 el = j - i - 1;
                if (el >= 2 && e[0] == '#') {
                    u32 v = 0, k = 1; int ok = 1, hex = 0;
                    if (e[1] == 'x') { hex = 1; k = 2; if (el < 3) ok = 0; }
                    for (; k < el && ok; k++) {
                        u16 d = e[k]; u32 dv;
                        if (d >= '0' && d <= '9') dv = d - '0';
                        else if (hex && d >= 'a' && d <= 'f') dv = d - 'a' + 10;
                        else if (hex && d >= 'A' && d <= 'F') dv = d - 'A' + 10;
                        else if (!hex && k > 1) break;   /* &#229e2; is &#229; (the digits count) */
                        else { ok = 0; break; }
                        v = v * (hex ? 16 : 10) + dv;
                        if (v > 0x10FFFF) ok = 0;
                    }
                    if (ok && v != 0 && !(v >= 0xD800 && v <= 0xDFFF)) {
                        if (v >= 0x10000) { v -= 0x10000; sb_unit(&b, (u16)(0xD800 + (v >> 10))); sb_unit(&b, (u16)(0xDC00 + (v & 0x3FF))); }
                        else sb_unit(&b, (u16)v);
                        done = 1;
                    }
                } else {
                    static const struct { const char *name; u16 ch; } ents[] = { { "lt", '<' }, { "gt", '>' }, { "amp", '&' }, { "quot", '"' }, { "apos", '\'' } };
                    int k;
                    for (k = 0; k < 5 && !done; k++) {
                        const char *s = ents[k].name; u32 sl = (u32)strlen(s), q;
                        if (sl != el) continue;
                        for (q = 0; q < sl; q++) if (e[q] != (u16)(u8)s[q]) break;
                        if (q == sl) { sb_unit(&b, ents[k].ch); done = 1; }
                    }
                }
                if (done) { i = j + 1; continue; }
            }
        }
        sb_unit(&b, c);
        i++;
    }
    return sb_done(p->vm, &b);
}

static int push_open(XP *p, XNode *x) {
    if (p->nopen == p->capopen) {
        int nc = p->capopen ? p->capopen * 2 : 16;
        XNode **no = (XNode**)realloc(p->open, (size_t)nc * sizeof(XNode*));
        if (!no) return 0;
        p->open = no; p->capopen = nc;
    }
    p->open[p->nopen++] = x;
    return 1;
}

static void text_node(XP *p, AStr *s) {
    u32 i; int ws = 1;
    if (s->len == 0) return;
    for (i = 0; i < s->len; i++) if (!is_ws(STR_AT(s, i))) { ws = 0; break; }
    if (p->ignore_white && ws) return;
    {
        XNode *t = xn_new(p->vm, TEXT_NODE, s);
        if (t) append_child(p->a, p->open[p->nopen - 1], t);
    }
}

static int match_at(XP *p, u32 at, const char *s) {
    u32 i, l = (u32)strlen(s);
    if (at + l > p->n) return 0;
    for (i = 0; i < l; i++) if (p->u[at + i] != (u16)(u8)s[i]) return 0;
    return 1;
}

static int match_at_ci(XP *p, u32 at, const char *s) {
    u32 i, l = (u32)strlen(s);
    if (at + l > p->n) return 0;
    for (i = 0; i < l; i++) { u16 c = p->u[at + i]; if (c >= 'a' && c <= 'z') c = (u16)(c - 32); if (c != (u16)(u8)s[i]) return 0; }
    return 1;
}

static long find_seq(XP *p, u32 from, const char *s) {
    u32 i;
    for (i = from; i < p->n; i++) if (match_at(p, i, s)) return (long)i;
    return -1;
}

/* attributes of a start tag (content after the name); returns 0 on an
 * attribute error (Ruffle aborts the parse without setting the status);
 * of an attribute given twice, the first one counts */
static int parse_attrs(XP *p, XNode *x, const u16 *u, u32 n) {
    u32 i = 0;
    AStr **keys = NULL; AStr **vals = NULL; int nk = 0, cap = 0, k, ok = 1;
    for (;;) {
        u32 ks, ke; u16 q; u32 vs;
        while (i < n && is_ws(u[i])) i++;
        if (i >= n) break;
        ks = i;
        while (i < n && u[i] != '=' && !is_ws(u[i])) i++;
        ke = i;
        while (i < n && is_ws(u[i])) i++;
        if (i >= n || u[i] != '=') { ok = 0; break; }      /* ExpectedEq */
        i++;
        while (i < n && is_ws(u[i])) i++;
        if (i >= n || (u[i] != '"' && u[i] != '\'')) { ok = 0; break; }   /* UnquotedValue */
        q = u[i++]; vs = i;
        while (i < n && u[i] != q) i++;
        if (i >= n) { ok = 0; break; }                      /* ExpectedQuote */
        if (nk == cap) {
            int nc = cap ? cap * 2 : 8;
            AStr **nkk = (AStr**)realloc(keys, (size_t)nc * sizeof(AStr*)), **nvv;
            if (!nkk) { ok = 0; break; }
            keys = nkk;
            nvv = (AStr**)realloc(vals, (size_t)nc * sizeof(AStr*));
            if (!nvv) { ok = 0; break; }
            vals = nvv; cap = nc;
        }
        keys[nk] = mk(p, u + ks, ke - ks);
        for (k = 0; k < nk; k++) if (str_eq(keys[k], keys[nk])) break;
        if (k == nk) { vals[nk] = unescape(p, u + vs, i - vs); nk++; }
        i++;
    }
    if (ok)
        for (k = nk - 1; k >= 0; k--) {
            o_define(p->vm, x->attrs, keys[k], v_str(vals[k]), 0);
            if (str_eq_c(keys[k], "id")) o_define(p->vm, p->doc->id_map, vals[k], v_obj(script_of(p->a, x)), 0);
        }
    free(keys); free(vals);
    return ok;
}

/* returns 0 on success, otherwise stops with p->doc->status set */
static int xml_parse(XP *p) {
    while (p->pos < p->n) {
        u16 c = p->u[p->pos];
        if (c != '<') {
            u32 s = p->pos;
            while (p->pos < p->n && p->u[p->pos] != '<') p->pos++;
            text_node(p, unescape(p, p->u + s, p->pos - s));
            continue;
        }
        if (p->pos + 1 >= p->n) { p->doc->status = XS_MALFORMED; return 0; }
        c = p->u[p->pos + 1];
        if (c == '!') {
            if (match_at(p, p->pos, "<!--")) {
                long e = find_seq(p, p->pos + 4, "-->");
                if (e < 0) { p->doc->status = XS_MALFORMED; return 0; }
                p->pos = (u32)e + 3;
            } else if (match_at(p, p->pos, "<![CDATA[")) {
                long e = find_seq(p, p->pos + 9, "]]>");
                if (e < 0) { p->doc->status = XS_MALFORMED; return 0; }
                text_node(p, mk(p, p->u + p->pos + 9, (u32)e - p->pos - 9));
                p->pos = (u32)e + 3;
            } else if (match_at_ci(p, p->pos + 2, "DOCTYPE")) {
                u32 i = p->pos + 9; int depth = 1;
                while (i < p->n) {
                    if (p->u[i] == '<') depth++;
                    else if (p->u[i] == '>') { if (--depth == 0) break; }
                    i++;
                }
                if (i >= p->n) { p->doc->status = XS_MALFORMED; return 0; }
                p->doc->doctype = mk(p, p->u + p->pos, i + 1 - p->pos);   /* as written; the last one counts */
                p->pos = i + 1;
            } else { p->doc->status = XS_MALFORMED; return 0; }
        } else if (c == '?') {
            long e = find_seq(p, p->pos + 2, "?>"); u32 s = p->pos + 2, l;
            if (e < 0) { p->doc->status = XS_MALFORMED; return 0; }
            l = (u32)e - s;
            if (l >= 3 && match_at_ci(p, s, "XML") && (l == 3 || is_ws(p->u[s + 3]))) {
                /* as written; several of them add up */
                AStr *d = mk(p, p->u + p->pos, (u32)e + 2 - p->pos);
                p->doc->decl = p->doc->decl ? str_concat(p->vm, p->doc->decl, d) : d;
            }
            p->pos = (u32)e + 2;
        } else if (c == '/') {
            u32 s = p->pos + 2, i = s, e;
            while (i < p->n && p->u[i] != '>') i++;
            if (i >= p->n) { p->doc->status = XS_MALFORMED; return 0; }
            e = i;
            while (e > s && is_ws(p->u[e - 1])) e--;
            if (p->nopen <= 1) { p->doc->status = XS_MISMATCH_END; return 0; }
            {
                AStr *top = p->open[p->nopen - 1]->name; u32 k; int same = top && top->len == e - s;
                for (k = 0; same && k < e - s; k++) if (STR_AT(top, k) != p->u[s + k]) same = 0;
                if (!same) { p->doc->status = XS_MISMATCH_END; return 0; }
            }
            p->nopen--;
            p->pos = i + 1;
        } else {
            u32 s = p->pos + 1, i = s, e, ne; u16 q = 0; int empty = 0; XNode *x;
            while (i < p->n) {
                u16 d = p->u[i];
                if (q) { if (d == q) q = 0; }
                else if (d == '"' || d == '\'') q = d;
                else if (d == '>') break;
                i++;
            }
            if (i >= p->n) { p->doc->status = XS_MALFORMED; return 0; }
            e = i;
            if (e > s && p->u[e - 1] == '/') { empty = 1; e--; }
            ne = s;
            while (ne < e && !is_ws(p->u[ne])) ne++;
            x = xn_new(p->vm, ELEMENT_NODE, mk(p, p->u + s, ne - s));
            if (!x) { p->doc->status = XS_OOM; return 0; }
            if (!parse_attrs(p, x, p->u + ne, e - ne)) return 0;
            append_child(p->a, p->open[p->nopen - 1], x);
            {   /* the namespace is settled here (later attribute changes do not move it) */
                AStr *pf = prefix_of(p->vm, x); Value ns;
                x->ns_fixed = 1;
                x->ns_uri = pf && lookup_ns(p->a, x, pf, &ns) ? v_str_of(p->a, ns) : p->vm->s_empty;
                if (p->vm->err) return 0;
            }
            if (!empty && !push_open(p, x)) { p->doc->status = XS_OOM; return 0; }
            p->pos = i + 1;
        }
    }
    return 1;
}

static void doc_parse(Act *a, XDoc *d, AStr *text, int ignore_white) {
    XP p; u32 i; u16 *u;
    memset(&p, 0, sizeof(p));
    p.a = a; p.vm = a->vm; p.doc = d; p.ignore_white = ignore_white;
    d->status = XS_OK;
    u = (u16*)malloc((text->len ? text->len : 1) * sizeof(u16));
    if (!u) { d->status = XS_OOM; return; }
    for (i = 0; i < text->len; i++) u[i] = STR_AT(text, i);
    p.u = u; p.n = text->len;
    if (push_open(&p, d->root) && xml_parse(&p)) refresh_cache(a, d->root);
    free(p.open); free(u);
}

/* ---------------------------------------------------------------- */
/* XMLNode                                                          */

static Value xmlnode_ctor(Act *a, AObj *self, int argc, Value *argv) {
    XNode *x; AObj *t;
    if (argc >= 2) {
        u8 type = v_u8(a, argv[0]); AStr *v;
        if (a->vm->err) return v_undef();
        v = v_str_of(a, argv[1]); if (a->vm->err) return v_undef();
        x = xn_new(a->vm, type, v);
    } else x = xn_new(a->vm, TEXT_NODE, a->vm->s_empty);
    t = o_native_target(self);
    if (x && t) introduce_script(x, t);
    return v_obj(self);
}

static Value xn_append_child(Act *a, AObj *self, int argc, Value *argv) {
    XNode *x = as_node(self), *c = v_node(arg(argc, argv, 0));
    if (x && c && c->parent != x) { insert_child(a, x, x->nkids, c); refresh_cache(a, x); }
    return v_undef();
}

static Value xn_insert_before(Act *a, AObj *self, int argc, Value *argv) {
    XNode *x = as_node(self), *c = v_node(arg(argc, argv, 0)), *at = v_node(arg(argc, argv, 1)); int pos;
    if (x && c && at && c->parent != x && (pos = child_pos(x, at)) >= 0) { insert_child(a, x, pos, c); refresh_cache(a, x); }
    return v_undef();
}

static Value xn_clone_node(Act *a, AObj *self, int argc, Value *argv) {
    XNode *x = as_node(self), *c;
    if (!x) return v_undef();
    c = duplicate(a->vm, x, argc > 0 ? v_as_bool(argv[0], a->swf_version) : 0);
    return c ? v_obj(script_of(a, c)) : v_undef();
}

static Value xn_get_ns_for_prefix(Act *a, AObj *self, int argc, Value *argv) {
    XNode *x = as_node(self); AStr *pf; Value out;
    if (!x || argc < 1) return v_undef();
    pf = v_str_of(a, argv[0]); if (a->vm->err) return v_undef();
    return lookup_ns(a, x, pf, &out) ? out : v_null();
}

static Value xn_get_prefix_for_ns(Act *a, AObj *self, int argc, Value *argv) {
    XNode *x = as_node(self), *n; AStr *uri; int i;
    if (!x || argc < 1) return v_undef();
    uri = v_str_of(a, argv[0]); if (a->vm->err) return v_undef();
    {
        u32 stamp = ++g_visit;
        for (n = x; n && n->visit != stamp; n = n->parent) {
            n->visit = stamp;
            for (i = n->attrs->props.n - 1; i >= 0; i--) {
                Prop *p = &n->attrs->props.items[i]; AStr *v, *k = p->key;
                if (!k || (p->attr & PA_DONT_ENUM) || !starts_xmlns(k)) continue;
                v = v_str_of(a, p->v); if (a->vm->err) return v_undef();
                if (str_eq(v, uri)) return v_str(k->len > 6 && STR_AT(k, 5) == ':' ? str_slice(a->vm, k, 6, k->len) : a->vm->s_empty);
            }
        }
    }
    return v_null();
}

static Value xn_has_child_nodes(Act *a, AObj *self, int argc, Value *argv) {
    XNode *x = as_node(self); (void)a; (void)argc; (void)argv;
    return x ? v_bool(x->nkids > 0) : v_undef();
}

static Value xn_remove_node(Act *a, AObj *self, int argc, Value *argv) {
    XNode *x = as_node(self), *op; (void)argc; (void)argv;
    if (x) { op = x->parent; remove_node(x); if (op) refresh_cache(a, op); }
    return v_undef();
}

static Value xn_to_string(Act *a, AObj *self, int argc, Value *argv) {
    XNode *x = as_node(self); XDoc *d = as_doc(self); SBuf b; (void)argc; (void)argv;
    if (!x) return v_str(a->vm->s_empty);
    sb_init(&b);
    /* a document starts with its xmlDecl and docTypeDecl */
    if (d && d->decl) sb_str(&b, d->decl);
    if (d && d->doctype) sb_str(&b, d->doctype);
    write_node(a, x, &b, 0);
    if (a->vm->err) { sb_free(&b); return v_undef(); }
    return v_str(sb_done(a->vm, &b));
}

static Value xn_local_name(Act *a, AObj *self, int argc, Value *argv) {
    XNode *x = as_node(self); AStr *nm; long i; (void)argc; (void)argv;
    if (!x || !(nm = node_name(x))) return v_null();
    i = str_find_unit(nm, ':', 0);
    if (i >= 0 && (u32)i + 1 < nm->len) return v_str(str_slice(a->vm, nm, (u32)i + 1, nm->len));
    return v_str(nm);
}

static AStr *prefix_of(VM *vm, XNode *x) {
    AStr *nm = node_name(x); long i;
    if (!nm) return NULL;
    i = str_find_unit(nm, ':', 0);
    if (i >= 0 && (u32)i + 1 < nm->len) return str_slice(vm, nm, 0, (u32)i);
    return vm->s_empty;
}

static Value xn_prefix(Act *a, AObj *self, int argc, Value *argv) {
    XNode *x = as_node(self); AStr *pf; (void)argc; (void)argv;
    if (!x || !(pf = prefix_of(a->vm, x))) return v_null();
    return v_str(pf);
}

static Value xn_namespace_uri(Act *a, AObj *self, int argc, Value *argv) {
    XNode *x = as_node(self); AStr *pf; Value out; (void)argc; (void)argv;
    if (!x) return v_undef();
    if (!(pf = prefix_of(a->vm, x))) return v_null();
    if (x->ns_fixed) return v_str(x->ns_uri ? x->ns_uri : a->vm->s_empty);
    return lookup_ns(a, x, pf, &out) ? out : v_str(a->vm->s_empty);
}

static Value xn_node_name(Act *a, AObj *self, int argc, Value *argv) {
    XNode *x = as_node(self); AStr *s; (void)a; (void)argc; (void)argv;
    return x && (s = node_name(x)) ? v_str(s) : v_null();
}

/* Ruffle: one setter for both, storing the name of an element or the text of a text node */
static void ruffle_set_node(XNode *x, AStr *s) { if (x->type == ELEMENT_NODE) x->name = s; else x->value = s; }
/* a node keeps a name and a value; from Flash Player 10 on, setting one
   clears the other (Gnash XMLNode.as / XML.as per player version) */
static Value xn_set_node_name(Act *a, AObj *self, int argc, Value *argv) {
    XNode *x; AStr *s;
    if (argc < 1 || argv[0].t == V_UNDEF) return v_undef();
    x = as_node(self);
    if (x) { s = v_str_of(a, argv[0]); if (a->vm->err) return v_undef(); if (avm1_ruffle_shapes()) { ruffle_set_node(x, s); return v_undef(); } x->name = s; if (avm1_player_major() >= 10) x->value = NULL; }
    return v_undef();
}
static Value xn_set_node_value(Act *a, AObj *self, int argc, Value *argv) {
    XNode *x; AStr *s;
    if (argc < 1 || argv[0].t == V_UNDEF) return v_undef();
    x = as_node(self);
    if (x) { s = v_str_of(a, argv[0]); if (a->vm->err) return v_undef(); if (avm1_ruffle_shapes()) { ruffle_set_node(x, s); return v_undef(); } x->value = s; if (avm1_player_major() >= 10) x->name = NULL; }
    return v_undef();
}

static Value xn_node_type(Act *a, AObj *self, int argc, Value *argv) {
    XNode *x = as_node(self); (void)a; (void)argc; (void)argv;
    return x ? v_num(x->type) : v_undef();
}

static Value xn_node_value(Act *a, AObj *self, int argc, Value *argv) {
    XNode *x = as_node(self); AStr *s; (void)a; (void)argc; (void)argv;
    return x && (s = node_value(x)) ? v_str(s) : v_null();
}

static Value xn_child_nodes(Act *a, AObj *self, int argc, Value *argv) {
    XNode *x = as_node(self); (void)argc; (void)argv;
    return x ? v_obj(child_nodes(a, x)) : v_undef();
}

static Value xn_first_child(Act *a, AObj *self, int argc, Value *argv) {
    XNode *x = as_node(self); (void)argc; (void)argv;
    if (!x) return v_undef();
    return script_or_null(a, x->nkids ? x->kids[0] : NULL);
}

static Value xn_last_child(Act *a, AObj *self, int argc, Value *argv) {
    XNode *x = as_node(self); (void)argc; (void)argv;
    if (!x) return v_undef();
    return script_or_null(a, x->nkids ? x->kids[x->nkids - 1] : NULL);
}

static Value xn_parent_node(Act *a, AObj *self, int argc, Value *argv) {
    XNode *x = as_node(self); (void)argc; (void)argv;
    return x ? script_or_null(a, x->parent) : v_undef();
}

static Value xn_prev_sibling(Act *a, AObj *self, int argc, Value *argv) {
    XNode *x = as_node(self); (void)argc; (void)argv;
    return x ? script_or_null(a, x->prev) : v_undef();
}

static Value xn_next_sibling(Act *a, AObj *self, int argc, Value *argv) {
    XNode *x = as_node(self); (void)argc; (void)argv;
    return x ? script_or_null(a, x->next) : v_undef();
}

static Value xn_attributes(Act *a, AObj *self, int argc, Value *argv) {
    XNode *x = as_node(self); (void)a; (void)argc; (void)argv;
    return x ? v_obj(x->attrs) : v_undef();
}

static void defm(VM *vm, Env *env, AObj *o, const char *name, NativeFn f, u16 attr) { o_define(vm, o, str_intern(vm, name), v_obj(fn_native(vm, env, f, NULL)), attr); }
static void defp(VM *vm, Env *env, AObj *o, const char *name, NativeFn g, NativeFn s, u16 attr) { o_add_property(vm, o, str_intern(vm, name), fn_native(vm, env, g, NULL), s ? fn_native(vm, env, s, NULL) : NULL, attr); }

AObj *lib_xmlnode(VM *vm, Env *env, AObj *object_proto) {
    AObj *proto, *c = lib_class(vm, env, NULL, xmlnode_ctor, object_proto, 1, &proto); u16 at = 0;   /* (Flash also makes these DONT_DELETE; Ruffle and its tests do not) */
    env->p.xmlnode = proto; env->p.xmlnode_ctor = c;
    defm(vm, env, proto, "cloneNode", xn_clone_node, at);
    defm(vm, env, proto, "removeNode", xn_remove_node, at);
    defm(vm, env, proto, "insertBefore", xn_insert_before, at);
    defm(vm, env, proto, "appendChild", xn_append_child, at);
    defm(vm, env, proto, "hasChildNodes", xn_has_child_nodes, at);
    defm(vm, env, proto, "toString", xn_to_string, at);
    defm(vm, env, proto, "getNamespaceForPrefix", xn_get_ns_for_prefix, at);
    defm(vm, env, proto, "getPrefixForNamespace", xn_get_prefix_for_ns, at);
    defp(vm, env, proto, "attributes", xn_attributes, NULL, at);
    defp(vm, env, proto, "childNodes", xn_child_nodes, NULL, at);
    defp(vm, env, proto, "firstChild", xn_first_child, NULL, at);
    defp(vm, env, proto, "lastChild", xn_last_child, NULL, at);
    defp(vm, env, proto, "nextSibling", xn_next_sibling, NULL, at);
    defp(vm, env, proto, "nodeName", xn_node_name, xn_set_node_name, at);
    defp(vm, env, proto, "nodeType", xn_node_type, NULL, at);
    defp(vm, env, proto, "nodeValue", xn_node_value, xn_set_node_value, at);
    defp(vm, env, proto, "parentNode", xn_parent_node, NULL, at);
    defp(vm, env, proto, "previousSibling", xn_prev_sibling, NULL, at);
    defp(vm, env, proto, "prefix", xn_prefix, NULL, at);
    defp(vm, env, proto, "localName", xn_local_name, NULL, at);
    defp(vm, env, proto, "namespaceURI", xn_namespace_uri, NULL, at);
    return c;
}

/* ---------------------------------------------------------------- */
/* XML                                                              */

static XDoc *doc_new(Act *a, AObj *self) {
    XDoc *d = (XDoc*)calloc(1, sizeof(XDoc)); AObj *t = o_native_target(self);
    if (!d) return NULL;
    d->root = xn_new(a->vm, ELEMENT_NODE, NULL);
    d->id_map = obj_bare(a->vm);
    d->loaded = -1;
    d->content_type = str_intern(a->vm, "application/x-www-form-urlencoded");
    if (!d->root) { free(d); return NULL; }
    if (!t) t = obj_bare(a->vm);       /* already native: the document is lost */
    introduce_script(d->root, t);
    t->nk = NK_XML; t->nat.p = d; t->nfree = doc_free; t->nmark = doc_mark;
    return d;
}

static int ignore_white_of(Act *a, AObj *self) {
    Value v = o_get(a, self, str_intern(a->vm, "ignoreWhite"));
    return v_as_bool(v, a->swf_version);
}

static void add_doc_props(Act *a, AObj *proto);
static Value xml_ctor(Act *a, AObj *self, int argc, Value *argv) {
    XDoc *d;
    /* the first XML object gives the prototype contentType, docTypeDecl,
       ignoreWhite, loaded, status and xmlDecl (Gnash XML.as; Flash's own
       XML.prototype listing has none of them) */
    {
        Value pv; AObj *po;
        if (!avm1_ruffle_shapes() && o_get_local_stored(a, self, a->vm->s_proto, &pv) && (po = v_obj_of(a, pv)) != NULL) add_doc_props(a, po);
        if (a->vm->err) return v_undef();
    }
    d = doc_new(a, self);
    if (d && argc > 0) {
        AStr *text = v_str_of(a, argv[0]); int iw;
        if (a->vm->err) return v_undef();
        iw = ignore_white_of(a, self); if (a->vm->err) return v_undef();
        doc_parse(a, d, text, iw);
    }
    return v_obj(self);
}

static Value xml_create_element(Act *a, AObj *self, int argc, Value *argv) {
    AStr *s; XNode *x;
    if (!as_doc(self) || argc < 1) return v_undef();
    s = v_str_of(a, argv[0]); if (a->vm->err) return v_undef();
    x = xn_new(a->vm, ELEMENT_NODE, s);
    return x ? v_obj(script_of(a, x)) : v_undef();
}

static Value xml_create_text_node(Act *a, AObj *self, int argc, Value *argv) {
    AStr *s; XNode *x;
    if (!as_doc(self) || argc < 1) return v_undef();
    s = v_str_of(a, argv[0]); if (a->vm->err) return v_undef();
    x = xn_new(a->vm, TEXT_NODE, s);
    return x ? v_obj(script_of(a, x)) : v_undef();
}

static Value xml_get_bytes_loaded(Act *a, AObj *self, int argc, Value *argv) { (void)argc; (void)argv; return o_get(a, self, str_intern(a->vm, "_bytesLoaded")); }
static Value xml_get_bytes_total(Act *a, AObj *self, int argc, Value *argv) { (void)argc; (void)argv; return o_get(a, self, str_intern(a->vm, "_bytesTotal")); }

static Value xml_parse_xml(Act *a, AObj *self, int argc, Value *argv) {
    XDoc *d = as_doc(self);
    if (d) {
        while (d->root->nkids > 0) remove_node(d->root->kids[d->root->nkids - 1]);
        if (argc > 0) {
            AStr *text = v_str_of(a, argv[0]); int iw;
            if (a->vm->err) return v_undef();
            iw = ignore_white_of(a, self); if (a->vm->err) return v_undef();
            doc_parse(a, d, text, iw);
        }
    }
    return v_undef();
}

extern int avm1_load_vars(Act *a, AObj *target, AStr *url, AStr *post, int is_xml);

static void define_or_set(Act *a, AObj *o, const char *name, Value v) {
    AStr *k = str_intern(a->vm, name);
    if (!o_has_property(a, o, k)) o_define(a->vm, o, k, v, DE | DD);
    else o_set(a, o, k, v);
}

static void xml_fetch(Act *a, AObj *loader, AStr *url, XNode *send) {
    AStr *post = NULL;
    if (send) {
        SBuf b; sb_init(&b); write_node(a, send, &b, 0);
        if (a->vm->err) { sb_free(&b); return; }
        post = sb_done(a->vm, &b);
    }
    define_or_set(a, loader, "_bytesLoaded", v_num(0)); if (a->vm->err) return;
    define_or_set(a, loader, "_bytesTotal", v_undef()); if (a->vm->err) return;
    o_set(a, loader, str_intern(a->vm, "loaded"), v_bool(0)); if (a->vm->err) return;
    avm1_load_vars(a, loader, url, post, 1);
}

static Value xml_load(Act *a, AObj *self, int argc, Value *argv) {
    Value u = arg(argc, argv, 0); AStr *url;
    if (u.t == V_NULL) return v_bool(0);
    /* (any object can load this way: Gnash LoadVars.as uses it on a LoadVars) */
    url = v_str_of(a, u); if (a->vm->err) return v_undef();
    xml_fetch(a, self, url, NULL);
    return v_bool(1);
}

/* any object can take the answer (it gets loaded = false); anything else: false */
static Value xml_send_and_load(Act *a, AObj *self, int argc, Value *argv) {
    Value u = arg(argc, argv, 0), t = arg(argc, argv, 1); XDoc *d; AStr *url;
    if (t.t != V_OBJ) return v_bool(0);
    if (u.t == V_NULL) return v_undef();
    d = as_doc(self);
    if (d) {
        url = v_str_of(a, u); if (a->vm->err) return v_undef();
        xml_fetch(a, t.u.o, url, d->root);
        if (a->vm->err) return v_undef();
    }
    return v_bool(1);
}

/* XML.send(url[, target[, method]]): like getURL with the document as the data */
static Value xml_send(Act *a, AObj *self, int argc, Value *argv) {
    extern void avm1_navigate_locals(Act *a, AStr *url, AStr *target, int method);
    Value u = arg(argc, argv, 0); AStr *url, *target = a->vm->s_empty;
    if (u.t == V_UNDEF || u.t == V_NULL || !as_doc(self)) return v_bool(0);
    url = v_str_of(a, u); if (a->vm->err) return v_undef();
    if (argc > 1) { target = v_str_of(a, argv[1]); if (a->vm->err) return v_undef(); }
    (void)url; (void)target;
    return v_bool(1);
}

static Value xml_on_load(Act *a, AObj *self, int argc, Value *argv) { (void)a; (void)self; (void)argc; (void)argv; return v_undef(); }

/* addRequestHeader(name, value) / addRequestHeader([n1, v1, ...]): string pairs go
   to the _customHeaders array (made on the first call) */
static Value xml_add_request_header(Act *a, AObj *self, int argc, Value *argv) {
    AStr *k = str_intern(a->vm, "_customHeaders"); Value hv; AObj *h; Value pair[2];
    if (!o_get_local_stored(a, self, k, &hv)) {
        h = obj_array(a, 0, NULL); if (!h) return v_undef();
        o_define(a->vm, self, k, v_obj(h), DE | DD);
        hv = v_obj(h);
    }
    if (a->vm->err) return v_undef();
    if (hv.t != V_OBJ || hv.u.o->nk != NK_ARRAY) return v_undef();
    h = hv.u.o;
    if (argc >= 1 && argv[0].t == V_OBJ && argv[0].u.o->nk == NK_ARRAY) {
        AObj *src = argv[0].u.o; int n = o_length(a, src), i;
        if (a->vm->err) return v_undef();
        for (i = 0; i + 1 < n; i += 2) {
            pair[0] = o_get_element(a, src, i); if (a->vm->err) return v_undef();
            pair[1] = o_get_element(a, src, i + 1); if (a->vm->err) return v_undef();
            if (pair[0].t != V_STR || pair[1].t != V_STR) continue;
            o_call_method(a, h, str_intern(a->vm, "push"), 2, pair, ER_SPECIAL); if (a->vm->err) return v_undef();
        }
    } else if (argc >= 2 && argv[0].t == V_STR && argv[1].t == V_STR) {
        pair[0] = argv[0]; pair[1] = argv[1];
        o_call_method(a, h, str_intern(a->vm, "push"), 2, pair, ER_SPECIAL);
    }
    return v_undef();
}

static Value xml_on_data(Act *a, AObj *self, int argc, Value *argv) {
    Value src = arg(argc, argv, 0), b;
    if (src.t == V_UNDEF) {
        b = v_bool(0);
        o_call_method(a, self, a->vm->s_onLoad, 1, &b, ER_SPECIAL);
    } else {
        Value s = v_str(v_str_of(a, src));
        if (a->vm->err) return v_undef();
        o_call_method(a, self, str_intern(a->vm, "parseXML"), 1, &s, ER_SPECIAL);
        if (a->vm->err) return v_undef();
        o_set(a, self, str_intern(a->vm, "loaded"), v_bool(1));
        if (a->vm->err) return v_undef();
        b = v_bool(1);
        o_call_method(a, self, a->vm->s_onLoad, 1, &b, ER_SPECIAL);
    }
    return v_undef();
}

static Value xml_doctype(Act *a, AObj *self, int argc, Value *argv) {
    XDoc *d = as_doc(self); (void)a; (void)argc; (void)argv;
    return d && d->doctype ? v_str(d->doctype) : v_undef();
}
static Value xml_set_doctype(Act *a, AObj *self, int argc, Value *argv) {
    XDoc *d = as_doc(self); AStr *s;
    if (!d || argc < 1) return v_undef();
    s = v_str_of(a, argv[0]); if (!a->vm->err) d->doctype = s;
    return v_undef();
}

static Value xml_xmldecl(Act *a, AObj *self, int argc, Value *argv) {
    XDoc *d = as_doc(self); (void)a; (void)argc; (void)argv;
    return d && d->decl ? v_str(d->decl) : v_undef();
}
static Value xml_set_xmldecl(Act *a, AObj *self, int argc, Value *argv) {
    XDoc *d = as_doc(self); AStr *s;
    if (!d || argc < 1) return v_undef();
    s = v_str_of(a, argv[0]); if (!a->vm->err) d->decl = s;
    return v_undef();
}

static Value xml_content_type(Act *a, AObj *self, int argc, Value *argv) {
    XDoc *d = as_doc(self); (void)a; (void)argc; (void)argv;
    return d && d->content_type ? v_str(d->content_type) : v_undef();
}
static Value xml_set_content_type(Act *a, AObj *self, int argc, Value *argv) {
    XDoc *d = as_doc(self); AStr *s;
    if (!d || argc < 1) return v_undef();
    s = v_str_of(a, argv[0]); if (!a->vm->err) d->content_type = s;
    return v_undef();
}

static Value xml_ignore_white(Act *a, AObj *self, int argc, Value *argv) {
    XDoc *d = as_doc(self); (void)a; (void)argc; (void)argv;
    return d ? v_bool(d->ignore_white) : v_undef();
}
static Value xml_set_ignore_white(Act *a, AObj *self, int argc, Value *argv) {
    XDoc *d = as_doc(self);
    if (d && argc > 0) d->ignore_white = v_as_bool(argv[0], a->swf_version);
    return v_undef();
}

static Value xml_loaded(Act *a, AObj *self, int argc, Value *argv) {
    XDoc *d = as_doc(self); (void)a; (void)argc; (void)argv;
    return d && d->loaded >= 0 ? v_bool(d->loaded) : v_undef();
}
static Value xml_set_loaded(Act *a, AObj *self, int argc, Value *argv) {
    XDoc *d = as_doc(self);
    if (d && argc > 0) d->loaded = v_as_bool(argv[0], a->swf_version);
    return v_undef();
}

static Value xml_idmap(Act *a, AObj *self, int argc, Value *argv) {
    XDoc *d = as_doc(self); (void)a; (void)argc; (void)argv;
    return d ? v_obj(d->id_map) : v_undef();
}

static Value xml_status(Act *a, AObj *self, int argc, Value *argv) {
    XDoc *d = as_doc(self); (void)a; (void)argc; (void)argv;
    return d ? v_num(d->status) : v_undef();
}
/* status is an integer: what does not fit (NaN, infinities, too big) is
   -2^31, or 0 for Flash Player 9 */
static Value xml_set_status(Act *a, AObj *self, int argc, Value *argv) {
    XDoc *d = as_doc(self); double n;
    if (!d || argc < 1) return v_undef();
    n = v_num_of(a, argv[0]); if (a->vm->err) return v_undef();
    if (n != n || n >= 2147483648.0 || n <= -2147483649.0) d->status = avm1_player_major() <= 9 ? 0 : (int)(-2147483647 - 1);
    else d->status = (int)n;
    return v_undef();
}

static void add_doc_props(Act *a, AObj *proto) {
    static const char *const names[6] = { "contentType", "docTypeDecl", "ignoreWhite", "loaded", "status", "xmlDecl" };
    static const NativeFn g[6] = { xml_content_type, xml_doctype, xml_ignore_white, xml_loaded, xml_status, xml_xmldecl };
    static const NativeFn st[6] = { xml_set_content_type, xml_set_doctype, xml_set_ignore_white, xml_set_loaded, xml_set_status, xml_set_xmldecl };
    VM *vm = a->vm; Env *env = ENV(a); int i;
    for (i = 0; i < 6; i++) {
        AStr *n = str_intern(vm, names[i]);
        if (o_has_own_property(a, proto, n)) continue;
        o_add_property(vm, proto, n, fn_native(vm, env, g[i], NULL), fn_native(vm, env, st[i], NULL), 0);
    }
}

AObj *lib_xml(VM *vm, Env *env, AObj *xmlnode_proto) {
    AObj *proto, *c = lib_class(vm, env, xml_ctor, NULL, xmlnode_proto ? xmlnode_proto : env->p.object, 1, &proto); u16 at = 0;   /* (Flash also makes these DONT_DELETE; Ruffle and its tests do not) */
    env->p.xml = proto; env->p.xml_ctor = c;
    defm(vm, env, proto, "createElement", xml_create_element, at);
    defm(vm, env, proto, "createTextNode", xml_create_text_node, at);
    defm(vm, env, proto, "parseXML", xml_parse_xml, at);
    defm(vm, env, proto, "load", xml_load, at);
    if (avm1_ruffle_shapes()) {   /* Ruffle's XML.prototype: no send/onLoad/addRequestHeader, document properties up front */
        defm(vm, env, proto, "sendAndLoad", xml_send_and_load, at);
        defm(vm, env, proto, "onData", xml_on_data, at);
        defm(vm, env, proto, "getBytesLoaded", xml_get_bytes_loaded, at);
        defm(vm, env, proto, "getBytesTotal", xml_get_bytes_total, at);
        o_define(vm, proto, str_intern(vm, "contentType"), v_str(str_intern(vm, "application/x-www-form-urlencoded")), at | RO);
        defp(vm, env, proto, "docTypeDecl", xml_doctype, NULL, at | RO);
        o_define(vm, proto, str_intern(vm, "ignoreWhite"), v_bool(0), at);
        defp(vm, env, proto, "status", xml_status, NULL, at);
        defp(vm, env, proto, "xmlDecl", xml_xmldecl, NULL, at);
        defp(vm, env, proto, "idMap", xml_idmap, NULL, at);
        return c;
    }
    defm(vm, env, proto, "send", xml_send, at);
    defm(vm, env, proto, "sendAndLoad", xml_send_and_load, at);
    defm(vm, env, proto, "onLoad", xml_on_load, at);
    defm(vm, env, proto, "onData", xml_on_data, at);
    defm(vm, env, proto, "getBytesLoaded", xml_get_bytes_loaded, at);
    defm(vm, env, proto, "getBytesTotal", xml_get_bytes_total, at);
    defm(vm, env, proto, "addRequestHeader", xml_add_request_header, at);
    defp(vm, env, proto, "idMap", xml_idmap, NULL, DE);   /* (not seen by for..in: Gnash ExternalInterface.as) */
    return c;
}

/* the text of an XML object, for sending (LoadVars-style loaders) */
AStr *avm1_xml_string(Act *a, AObj *o) {
    XNode *x = as_node(o); SBuf b;
    if (!x) return NULL;
    sb_init(&b); write_node(a, x, &b, 0);
    return sb_done(a->vm, &b);
}
