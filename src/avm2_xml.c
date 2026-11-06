/* avm2_xml.c - E4X (ECMA-357) XML and XMLList for the AVM2, following the
 * specification's internal methods ([[Get]], [[Put]], [[Delete]], [[Replace]],
 * [[Insert]], ...) and Flash Player's deviations where they are known.
 *
 * Every XML node owns a wrapper A2Obj (payload = XNode); the wrapper is the
 * node's identity for the GC: a node marks its parent, children and
 * attributes, so a connected tree lives as long as any part of it is
 * reachable.  XMLList objects hold arrays of node pointers plus the E4X
 * target object / target property used by appends through empty lists. */
#include "avm2_int.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define ARG(i) a2_arg(argc, argv, i)
#define STR(i) v2_tostr(vm, ARG(i))
#define INT(i) v2_toint(vm, ARG(i))

enum { XN_ELEMENT, XN_TEXT, XN_ATTR, XN_COMMENT, XN_PI };

typedef struct XNs { A2Str *prefix; A2Str *uri; } XNs;     /* prefix NULL = undefined */

typedef struct XNode {
    int is_list;                    /* 0 */
    int kind;
    A2Str *local;                   /* local name; NULL = name is null (text/comment) */
    A2Str *uri;                     /* namespace uri (s_empty for none) */
    A2Str *prefix;                  /* preferred prefix for serialization, NULL unknown */
    A2Str *value;                   /* text / attribute / comment / pi value */
    int cdata;                      /* text node from a CDATA section */
    struct XNode *parent;
    struct XNode **kids; int nkids, capkids;
    struct XNode **attrs; int nattrs, capattrs;
    XNs *ns; int nns, capns;        /* namespace declarations in scope on this element */
    A2Obj *obj;                     /* wrapper (always present) */
    V2 notify;                      /* setNotification() function */
} XNode;

/* E4X name: uri NULL = any namespace; local "*" = any name */
typedef struct { A2Str *uri; A2Str *local; int attr; const A2Ns *nss; int nns; A2Str *dxns; } XQ;   /* nss: open namespace set for unqualified names */

typedef struct XList {
    int is_list;                    /* 1 */
    XNode **items; int n, cap;
    A2Obj *target;                  /* XML or XMLList wrapper, NULL */
    int has_tprop; XQ tprop;        /* target property */
} XList;

static int g_ignore_ws = 1, g_ignore_comments = 1, g_ignore_pi = 1, g_pretty = 1, g_indent = 2;

/* ---------------------------------------------------------------- */
/* basics                                                           */

static int is_xmlobj(V2 v) { return v.t == T_OBJ && v.u.o->kind == OK_XML && v.u.o->payload; }
static int is_list(V2 v) { return is_xmlobj(v) && *(int*)v.u.o->payload; }
static int is_node(V2 v) { return is_xmlobj(v) && !*(int*)v.u.o->payload; }
static XList *LST(V2 v) { return (XList*)v.u.o->payload; }
static XNode *NOD(V2 v) { return (XNode*)v.u.o->payload; }
static XList *lst_of(A2Obj *o) { return (o->payload && *(int*)o->payload) ? (XList*)o->payload : NULL; }
static XNode *node_of(A2Obj *o) { return (o->payload && !*(int*)o->payload) ? (XNode*)o->payload : NULL; }

static void throw_id(VM2 *vm, const char *cls, int id, const char *a1, const char *a2) {
    extern const char *a2_error_message(int id); const char *m = a2_error_message(id); char buf[700]; const char *p; size_t n = 0;
    if (!m) { sprintf(buf, "Error #%d", id); a2_throw_error(vm, cls, buf, id); return; }
    for (p = m; *p && n < 600; p++) {
        if (*p == '%' && (p[1] == '1' || p[1] == '2')) { const char *s = p[1] == '1' ? a1 : a2; if (s) { size_t l = strlen(s); if (l > 200) l = 200; memcpy(buf + n, s, l); n += l; } p++; continue; }
        buf[n++] = *p;
    }
    buf[n] = 0;
    a2_throw_error(vm, cls, buf, id);
}
static void type_error(VM2 *vm, int id, const char *a1, const char *a2) {
    throw_id(vm, "TypeError", id, a1, a2);
    /* the parser's frame shows up as an empty line in getStackTrace() */
    if (vm->throwing && vm->exc.t == T_OBJ) { A2Str *k = a2_str(vm, "__stackTrace"); A2Prop *p = a2_dyn_find(vm->exc.u.o, k); if (p && p->v.t == T_STR) p->v = v2_str(a2_concat(vm, a2_str(vm, "\n"), p->v.u.s)); }
}

static XNode *xn_new(VM2 *vm, int kind) {
    XNode *n = (XNode*)calloc(1, sizeof(XNode)); A2Obj *o = a2_new(vm, vm->c_xml);
    n->kind = kind; n->uri = vm->s_empty; n->value = vm->s_empty;
    o->kind = OK_XML; o->payload = n; n->obj = o;
    return n;
}
static A2Obj *new_list(VM2 *vm) { A2Obj *o = a2_new(vm, vm->c_xmllist); XList *l = (XList*)calloc(1, sizeof(XList)); l->is_list = 1; o->kind = OK_XML; o->payload = l; return o; }
static void list_add(XList *l, XNode *n) { if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 8; l->items = (XNode**)realloc(l->items, (size_t)l->cap * sizeof(XNode*)); } l->items[l->n++] = n; }
static void list_insert(XList *l, int at, XNode *n) { list_add(l, n); if (at < l->n - 1) { memmove(l->items + at + 1, l->items + at, (size_t)(l->n - 1 - at) * sizeof(XNode*)); l->items[at] = n; } }
static void list_remove_at(XList *l, int i) { if (i < 0 || i >= l->n) return; memmove(l->items + i, l->items + i + 1, (size_t)(l->n - i - 1) * sizeof(XNode*)); l->n--; }
static V2 wrap(XNode *n) { return v2_obj(n->obj); }

static void xn_add_kid(XNode *p, XNode *k) { if (p->nkids == p->capkids) { p->capkids = p->capkids ? p->capkids * 2 : 4; p->kids = (XNode**)realloc(p->kids, (size_t)p->capkids * sizeof(XNode*)); } p->kids[p->nkids++] = k; k->parent = p; }
static void xn_insert_kid(XNode *p, int at, XNode *k) { xn_add_kid(p, k); if (at < p->nkids - 1) { memmove(p->kids + at + 1, p->kids + at, (size_t)(p->nkids - 1 - at) * sizeof(XNode*)); p->kids[at] = k; } }
static int xn_kid_index(XNode *p, XNode *k) { int i; for (i = 0; i < p->nkids; i++) if (p->kids[i] == k) return i; return -1; }
static void xn_remove_kid_at(XNode *p, int i) { if (i < 0 || i >= p->nkids) return; p->kids[i]->parent = NULL; memmove(p->kids + i, p->kids + i + 1, (size_t)(p->nkids - i - 1) * sizeof(XNode*)); p->nkids--; }
static void xn_add_attr(XNode *p, XNode *a) { if (p->nattrs == p->capattrs) { p->capattrs = p->capattrs ? p->capattrs * 2 : 4; p->attrs = (XNode**)realloc(p->attrs, (size_t)p->capattrs * sizeof(XNode*)); } p->attrs[p->nattrs++] = a; a->parent = p; }
static void xn_remove_attr_at(XNode *p, int i) { if (i < 0 || i >= p->nattrs) return; p->attrs[i]->parent = NULL; memmove(p->attrs + i, p->attrs + i + 1, (size_t)(p->nattrs - i - 1) * sizeof(XNode*)); p->nattrs--; }
static void xn_detach(XNode *n) { XNode *p = n->parent; int i; if (!p) return; if (n->kind == XN_ATTR) { for (i = 0; i < p->nattrs; i++) if (p->attrs[i] == n) { xn_remove_attr_at(p, i); return; } } else { i = xn_kid_index(p, n); if (i >= 0) xn_remove_kid_at(p, i); } n->parent = NULL; }
static void xn_add_ns(XNode *e, A2Str *prefix, A2Str *uri) { if (e->nns == e->capns) { e->capns = e->capns ? e->capns * 2 : 4; e->ns = (XNs*)realloc(e->ns, (size_t)e->capns * sizeof(XNs)); } e->ns[e->nns].prefix = prefix; e->ns[e->nns].uri = uri; e->nns++; }
static int xn_is_ancestor(XNode *anc, XNode *n) { for (; n; n = n->parent) if (n == anc) return 1; return 0; }

void a2_xml_free_payload(A2Obj *o) {
    void *p = o->payload;
    if (!p) return;
    if (*(int*)p) { XList *l = (XList*)p; free(l->items); }
    else { XNode *n = (XNode*)p; free(n->kids); free(n->attrs); free(n->ns); }
    free(p); o->payload = NULL;
}
void a2_xml_mark_payload(VM2 *vm, A2Obj *o) {
    void *p = o->payload; int i;
    if (!p) return;
    if (*(int*)p) { XList *l = (XList*)p; for (i = 0; i < l->n; i++) a2_mark_obj(vm, l->items[i]->obj); a2_mark_obj(vm, l->target); if (l->has_tprop) { if (l->tprop.uri) l->tprop.uri->gc.mark = 1; if (l->tprop.local) l->tprop.local->gc.mark = 1; } }
    else { XNode *n = (XNode*)p;
        if (n->local) n->local->gc.mark = 1; if (n->uri) n->uri->gc.mark = 1; if (n->prefix) n->prefix->gc.mark = 1; if (n->value) n->value->gc.mark = 1;
        for (i = 0; i < n->nns; i++) { if (n->ns[i].prefix) n->ns[i].prefix->gc.mark = 1; if (n->ns[i].uri) n->ns[i].uri->gc.mark = 1; }
        if (n->parent) a2_mark_obj(vm, n->parent->obj);
        for (i = 0; i < n->nkids; i++) a2_mark_obj(vm, n->kids[i]->obj);
        for (i = 0; i < n->nattrs; i++) a2_mark_obj(vm, n->attrs[i]->obj);
        a2_mark_v(vm, n->notify);
    }
}

/* adopt node `n` into an already-allocated wrapper (constructor self) */
static void adopt(A2Obj *self, XNode *n) {
    A2Obj *old = n->obj;
    if (old == self) return;
    if (self->payload) a2_xml_free_payload(self);
    self->kind = OK_XML; self->payload = n; n->obj = self;
    old->payload = NULL;
}

/* ---------------------------------------------------------------- */
/* names                                                            */

static int is_star(A2Str *s) { return s && s->len == 1 && s->s[0] == '*'; }
static A2Str *default_ns(VM2 *vm) { return vm->dxns ? vm->dxns : vm->s_empty; }

/* QName / Namespace object helpers (classes live in avm2_lib.c) */
static int is_qname(VM2 *vm, V2 v) { (void)vm; return v.t == T_OBJ && v.u.o->kind == OK_QNAME; }
static int is_nsobj(V2 v) { return v.t == T_OBJ && v.u.o->kind == OK_NAMESPACE; }
static A2Str *ns_prefix_of(VM2 *vm, A2Obj *ns) { A2Prop *p = a2_dyn_find(ns, a2_str(vm, "__prefix")); return (p && p->v.t == T_STR) ? p->v.u.s : NULL; }
static V2 make_ns(VM2 *vm, A2Str *prefix, A2Str *uri) { V2 args[2]; if (prefix) { args[0] = v2_str(prefix); args[1] = v2_str(uri); return a2_construct_class(vm, vm->c_namespace, 2, args); } args[0] = v2_str(uri); return a2_construct_class(vm, vm->c_namespace, 1, args); }
static V2 make_qname(VM2 *vm, A2Str *uri, A2Str *local) { return a2_make_qname(vm, uri, local, 0); }
static V2 make_attr_qname(VM2 *vm, A2Str *uri, A2Str *local) { return a2_make_qname(vm, uri, local, 1); }
static int qname_is_attr(VM2 *vm, V2 q) { (void)vm; return q.t == T_OBJ && q.u.o->kind == OK_QNAME && a2_qname_attr(q.u.o); }

/* ToXMLName for a runtime value (string, QName) */
static void xq_from_value(VM2 *vm, V2 v, XQ *q, int attr_hint) {
    A2Str *s;
    q->attr = attr_hint; q->nss = NULL; q->nns = 0; q->dxns = NULL;
    if (is_qname(vm, v)) { q->uri = a2_qname_uri(v.u.o); q->local = a2_qname_local(vm, v.u.o); if (qname_is_attr(vm, v)) q->attr = 1; return; }
    s = v2_tostr(vm, v);
    if (s->len && s->s[0] == '@') { q->attr = 1; s = a2_intern(vm, s->s + 1, s->len - 1); }
    q->local = s;
    if (is_star(s)) { q->uri = NULL; return; }
    q->uri = q->attr ? vm->s_empty : default_ns(vm);
}
/* ToXMLName for an interpreter multiname */
static void xq_from_name(VM2 *vm, const A2Name *n, XQ *q) {
    A2Str *s = n->name; int null_name;
    q->attr = n->attr; q->nss = NULL; q->nns = 0; q->dxns = NULL;
    if (s->len && s->s[0] == '@') { q->attr = 1; s = a2_intern(vm, s->s + 1, s->len - 1); }
    null_name = s->len == 0;
    if (null_name) s = a2_str(vm, "*");      /* multiname with name index 0 = wildcard */
    q->local = s;
    if (n->any_ns == 2) { q->uri = is_star(s) ? vm->s_empty : NULL; return; }   /* QName with namespace index 0 */
    /* explicitly qualified (ns::name, ns::*) */
    if (!n->any_ns && n->nns == 1 && n->nss[0].kind == NS_NAMESPACE && (!null_name || n->nss[0].uri->len || n->nss[0].id == A2_NSID_RUNTIME)) { q->uri = n->nss[0].uri; return; }
    if (is_star(s)) { q->uri = NULL; return; }
    if (n->any_ns) { q->uri = NULL; return; }
    if (n->nns == 1 && n->nss[0].kind == NS_PACKAGE && n->nss[0].uri->len) { q->uri = n->nss[0].uri; return; }
    if (q->attr) { q->uri = vm->s_empty; if (n->nns > 1) { q->uri = NULL; q->dxns = vm->s_empty; q->nss = n->nss; q->nns = n->nns; } return; }
    /* unqualified element name: the default xml namespace plus every open (use namespace) namespace */
    q->uri = NULL; q->dxns = default_ns(vm);
    if (n->nns > 1) { q->nss = n->nss; q->nns = n->nns; }
}
static int xq_ns_ok(const XQ *q, A2Str *uri) {
    int i;
    if (q->dxns) {
        if (uri == q->dxns) return 1;
        for (i = 0; i < q->nns; i++) if ((q->nss[i].kind == NS_NAMESPACE || q->nss[i].kind == NS_PACKAGE) && q->nss[i].uri == uri) return 1;
        return 0;
    }
    return !q->uri || q->uri == uri;
}
static int xq_matches(const XQ *q, XNode *n) {
    if (q->attr) { if (n->kind != XN_ATTR) return 0; }
    else if (!is_star(q->local)) { if (n->kind != XN_ELEMENT) return 0; }
    else if (q->uri && n->kind != XN_ELEMENT) return 0;      /* ns::* only matches elements */
    if (!is_star(q->local) && n->local != q->local) return 0;
    return xq_ns_ok(q, n->uri);
}
/* XML 1.0 (third edition, appendix B) character classes, as avmplus' isXMLName uses them */
static const unsigned short xn_letter[] = {
    0x0041,0x005A, 0x0061,0x007A, 0x00C0,0x00D6, 0x00D8,0x00F6, 0x00F8,0x00FF, 0x0100,0x0131, 0x0134,0x013E,
    0x0141,0x0148, 0x014A,0x017E, 0x0180,0x01C3, 0x01CD,0x01F0, 0x01F4,0x01F5, 0x01FA,0x0217, 0x0250,0x02A8,
    0x02BB,0x02C1, 0x0386,0x0386, 0x0388,0x038A, 0x038C,0x038C, 0x038E,0x03A1, 0x03A3,0x03CE, 0x03D0,0x03D6,
    0x03DA,0x03DA, 0x03DC,0x03DC, 0x03DE,0x03DE, 0x03E0,0x03E0, 0x03E2,0x03F3, 0x0401,0x040C, 0x040E,0x044F,
    0x0451,0x045C, 0x045E,0x0481, 0x0490,0x04C4, 0x04C7,0x04C8, 0x04CB,0x04CC, 0x04D0,0x04EB, 0x04EE,0x04F5,
    0x04F8,0x04F9, 0x0531,0x0556, 0x0559,0x0559, 0x0561,0x0586, 0x05D0,0x05EA, 0x05F0,0x05F2, 0x0621,0x063A,
    0x0641,0x064A, 0x0671,0x06B7, 0x06BA,0x06BE, 0x06C0,0x06CE, 0x06D0,0x06D3, 0x06D5,0x06D5, 0x06E5,0x06E6,
    0x0905,0x0939, 0x093D,0x093D, 0x0958,0x0961, 0x0985,0x098C, 0x098F,0x0990, 0x0993,0x09A8, 0x09AA,0x09B0,
    0x09B2,0x09B2, 0x09B6,0x09B9, 0x09DC,0x09DD, 0x09DF,0x09E1, 0x09F0,0x09F1, 0x0A05,0x0A0A, 0x0A0F,0x0A10,
    0x0A13,0x0A28, 0x0A2A,0x0A30, 0x0A32,0x0A33, 0x0A35,0x0A36, 0x0A38,0x0A39, 0x0A59,0x0A5C, 0x0A5E,0x0A5E,
    0x0A72,0x0A74, 0x0A85,0x0A8B, 0x0A8D,0x0A8D, 0x0A8F,0x0A91, 0x0A93,0x0AA8, 0x0AAA,0x0AB0, 0x0AB2,0x0AB3,
    0x0AB5,0x0AB9, 0x0ABD,0x0ABD, 0x0AE0,0x0AE0, 0x0B05,0x0B0C, 0x0B0F,0x0B10, 0x0B13,0x0B28, 0x0B2A,0x0B30,
    0x0B32,0x0B33, 0x0B36,0x0B39, 0x0B3D,0x0B3D, 0x0B5C,0x0B5D, 0x0B5F,0x0B61, 0x0B85,0x0B8A, 0x0B8E,0x0B90,
    0x0B92,0x0B95, 0x0B99,0x0B9A, 0x0B9C,0x0B9C, 0x0B9E,0x0B9F, 0x0BA3,0x0BA4, 0x0BA8,0x0BAA, 0x0BAE,0x0BB5,
    0x0BB7,0x0BB9, 0x0C05,0x0C0C, 0x0C0E,0x0C10, 0x0C12,0x0C28, 0x0C2A,0x0C33, 0x0C35,0x0C39, 0x0C60,0x0C61,
    0x0C85,0x0C8C, 0x0C8E,0x0C90, 0x0C92,0x0CA8, 0x0CAA,0x0CB3, 0x0CB5,0x0CB9, 0x0CDE,0x0CDE, 0x0CE0,0x0CE1,
    0x0D05,0x0D0C, 0x0D0E,0x0D10, 0x0D12,0x0D28, 0x0D2A,0x0D39, 0x0D60,0x0D61, 0x0E01,0x0E2E, 0x0E30,0x0E30,
    0x0E32,0x0E33, 0x0E40,0x0E45, 0x0E81,0x0E82, 0x0E84,0x0E84, 0x0E87,0x0E88, 0x0E8A,0x0E8A, 0x0E8D,0x0E8D,
    0x0E94,0x0E97, 0x0E99,0x0E9F, 0x0EA1,0x0EA3, 0x0EA5,0x0EA5, 0x0EA7,0x0EA7, 0x0EAA,0x0EAB, 0x0EAD,0x0EAE,
    0x0EB0,0x0EB0, 0x0EB2,0x0EB3, 0x0EBD,0x0EBD, 0x0EC0,0x0EC4, 0x0F40,0x0F47, 0x0F49,0x0F69, 0x10A0,0x10C5,
    0x10D0,0x10F6, 0x1100,0x1100, 0x1102,0x1103, 0x1105,0x1107, 0x1109,0x1109, 0x110B,0x110C, 0x110E,0x1112,
    0x113C,0x113C, 0x113E,0x113E, 0x1140,0x1140, 0x114C,0x114C, 0x114E,0x114E, 0x1150,0x1150, 0x1154,0x1155,
    0x1159,0x1159, 0x115F,0x1161, 0x1163,0x1163, 0x1165,0x1165, 0x1167,0x1167, 0x1169,0x1169, 0x116D,0x116E,
    0x1172,0x1173, 0x1175,0x1175, 0x119E,0x119E, 0x11A8,0x11A8, 0x11AB,0x11AB, 0x11AE,0x11AF, 0x11B7,0x11B8,
    0x11BA,0x11BA, 0x11BC,0x11C2, 0x11EB,0x11EB, 0x11F0,0x11F0, 0x11F9,0x11F9, 0x1E00,0x1E9B, 0x1EA0,0x1EF9,
    0x1F00,0x1F15, 0x1F18,0x1F1D, 0x1F20,0x1F45, 0x1F48,0x1F4D, 0x1F50,0x1F57, 0x1F59,0x1F59, 0x1F5B,0x1F5B,
    0x1F5D,0x1F5D, 0x1F5F,0x1F7D, 0x1F80,0x1FB4, 0x1FB6,0x1FBC, 0x1FBE,0x1FBE, 0x1FC2,0x1FC4, 0x1FC6,0x1FCC,
    0x1FD0,0x1FD3, 0x1FD6,0x1FDB, 0x1FE0,0x1FEC, 0x1FF2,0x1FF4, 0x1FF6,0x1FFC, 0x2126,0x2126, 0x212A,0x212B,
    0x212E,0x212E, 0x2180,0x2182, 0x3007,0x3007, 0x3021,0x3029, 0x3041,0x3094, 0x30A1,0x30FA, 0x3105,0x312C,
    0x4E00,0x9FA5, 0xAC00,0xD7A3
};
static const unsigned short xn_digit[] = {
    0x0030,0x0039, 0x0660,0x0669, 0x06F0,0x06F9, 0x0966,0x096F, 0x09E6,0x09EF, 0x0A66,0x0A6F, 0x0AE6,0x0AEF,
    0x0B66,0x0B6F, 0x0BE7,0x0BEF, 0x0C66,0x0C6F, 0x0CE6,0x0CEF, 0x0D66,0x0D6F, 0x0E50,0x0E59, 0x0ED0,0x0ED9,
    0x0F20,0x0F29
};
static const unsigned short xn_combining[] = {
    0x0300,0x0345, 0x0360,0x0361, 0x0483,0x0486, 0x0591,0x05A1, 0x05A3,0x05B9, 0x05BB,0x05BD, 0x05BF,0x05BF,
    0x05C1,0x05C2, 0x05C4,0x05C4, 0x064B,0x0652, 0x0670,0x0670, 0x06D6,0x06DC, 0x06DD,0x06DF, 0x06E0,0x06E4,
    0x06E7,0x06E8, 0x06EA,0x06ED, 0x0901,0x0903, 0x093C,0x093C, 0x093E,0x094C, 0x094D,0x094D, 0x0951,0x0954,
    0x0962,0x0963, 0x0981,0x0983, 0x09BC,0x09BC, 0x09BE,0x09BE, 0x09BF,0x09BF, 0x09C0,0x09C4, 0x09C7,0x09C8,
    0x09CB,0x09CD, 0x09D7,0x09D7, 0x09E2,0x09E3, 0x0A02,0x0A02, 0x0A3C,0x0A3C, 0x0A3E,0x0A3E, 0x0A3F,0x0A3F,
    0x0A40,0x0A42, 0x0A47,0x0A48, 0x0A4B,0x0A4D, 0x0A70,0x0A71, 0x0A81,0x0A83, 0x0ABC,0x0ABC, 0x0ABE,0x0AC5,
    0x0AC7,0x0AC9, 0x0ACB,0x0ACD, 0x0B01,0x0B03, 0x0B3C,0x0B3C, 0x0B3E,0x0B43, 0x0B47,0x0B48, 0x0B4B,0x0B4D,
    0x0B56,0x0B57, 0x0B82,0x0B83, 0x0BBE,0x0BC2, 0x0BC6,0x0BC8, 0x0BCA,0x0BCD, 0x0BD7,0x0BD7, 0x0C01,0x0C03,
    0x0C3E,0x0C44, 0x0C46,0x0C48, 0x0C4A,0x0C4D, 0x0C55,0x0C56, 0x0C82,0x0C83, 0x0CBE,0x0CC4, 0x0CC6,0x0CC8,
    0x0CCA,0x0CCD, 0x0CD5,0x0CD6, 0x0D02,0x0D03, 0x0D3E,0x0D43, 0x0D46,0x0D48, 0x0D4A,0x0D4D, 0x0D57,0x0D57,
    0x0E31,0x0E31, 0x0E34,0x0E3A, 0x0E47,0x0E4E, 0x0EB1,0x0EB1, 0x0EB4,0x0EB9, 0x0EBB,0x0EBC, 0x0EC8,0x0ECD,
    0x0F18,0x0F19, 0x0F35,0x0F35, 0x0F37,0x0F37, 0x0F39,0x0F39, 0x0F3E,0x0F3E, 0x0F3F,0x0F3F, 0x0F71,0x0F84,
    0x0F86,0x0F8B, 0x0F90,0x0F95, 0x0F97,0x0F97, 0x0F99,0x0FAD, 0x0FB1,0x0FB7, 0x0FB9,0x0FB9, 0x20D0,0x20DC,
    0x20E1,0x20E1, 0x302A,0x302F, 0x3099,0x3099, 0x309A,0x309A
};
static const unsigned short xn_extender[] = {
    0x00B7,0x00B7, 0x02D0,0x02D0, 0x02D1,0x02D1, 0x0387,0x0387, 0x0640,0x0640, 0x0E46,0x0E46, 0x0EC6,0x0EC6,
    0x3005,0x3005, 0x3031,0x3035, 0x309D,0x309E, 0x30FC,0x30FE
};

static int xn_in(const unsigned short *t, size_t n, u32 c) { size_t i; for (i = 0; i < n; i += 2) { if (c < t[i]) return 0; if (c <= t[i + 1]) return 1; } return 0; }
#define XN_IN(t, c) xn_in(t, sizeof(t) / sizeof(t[0]), c)
static int utf8_cp(const unsigned char **p, const unsigned char *e, u32 *out) {
    const unsigned char *s = *p; u32 c = *s; int k, i;
    if (c < 0x80) { *out = c; *p = s + 1; return 1; }
    if ((c & 0xE0) == 0xC0) { k = 1; c &= 0x1F; } else if ((c & 0xF0) == 0xE0) { k = 2; c &= 0x0F; } else if ((c & 0xF8) == 0xF0) { k = 3; c &= 0x07; } else return 0;
    for (i = 1; i <= k; i++) { if (s + i >= e || (s[i] & 0xC0) != 0x80) return 0; c = (c << 6) | (s[i] & 0x3F); }
    *out = c; *p = s + k + 1; return 1;
}
/* isXMLName(): a letter or '_' followed by letters, digits, '.', '-', '_', combining chars, extenders */
int a2_is_xml_name(const char *str, size_t len) {
    const unsigned char *p = (const unsigned char*)str, *e = p + len; u32 c; int first = 1;
    if (!len) return 0;
    while (p < e) {
        if (!utf8_cp(&p, e, &c) || c > 0xFFFF) return 0;
        if (first) { if (!(c == '_' || XN_IN(xn_letter, c))) return 0; first = 0; continue; }
        if (!(c == '.' || c == '-' || c == '_' || XN_IN(xn_letter, c) || XN_IN(xn_digit, c) || XN_IN(xn_combining, c) || XN_IN(xn_extender, c))) return 0;
    }
    return 1;
}
static int valid_name(A2Str *s) {
    u32 i; if (!s || !s->len) return 0;
    for (i = 0; i < s->len; i++) { unsigned char c = (unsigned char)s->s[i];
        if (c >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' ) continue;
        if (i && ((c >= '0' && c <= '9') || c == '-' || c == '.')) continue;
        return 0; }
    if (s->len >= 3 && (s->s[0] == 'x' || s->s[0] == 'X') && (s->s[1] == 'm' || s->s[1] == 'M') && (s->s[2] == 'l' || s->s[2] == 'L') && s->len == 3) return 0;
    return 1;
}

/* namespace in scope for a node: search declarations up the tree */
static A2Str *lookup_prefix_uri(XNode *n, A2Str *prefix) {
    for (; n; n = n->parent) { int i; for (i = n->nns - 1; i >= 0; i--) if ((n->ns[i].prefix == prefix) || (!prefix && n->ns[i].prefix && n->ns[i].prefix->len == 0) || (prefix && prefix->len == 0 && !n->ns[i].prefix)) return n->ns[i].uri; }
    return NULL;
}
/* GetNamespace(q, inscope): find a declared namespace for uri (prefer matching prefix hint) */
typedef struct { XNs *v; int n, cap; } NsList;
static void nsl_add(NsList *l, A2Str *prefix, A2Str *uri) { if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 8; l->v = (XNs*)realloc(l->v, (size_t)l->cap * sizeof(XNs)); } l->v[l->n].prefix = prefix; l->v[l->n].uri = uri; l->n++; }
static int nsl_has(NsList *l, A2Str *prefix, A2Str *uri) { int i; for (i = 0; i < l->n; i++) if (l->v[i].uri == uri && ((!l->v[i].prefix && !prefix) || (l->v[i].prefix && prefix && l->v[i].prefix == prefix))) return 1; return 0; }
static XNs *nsl_find_uri(NsList *l, A2Str *uri, A2Str *hint) { int i; XNs *first = NULL; for (i = 0; i < l->n; i++) if (l->v[i].uri == uri) { if (hint && l->v[i].prefix == hint) return &l->v[i]; if (!first) first = &l->v[i]; } return first; }
static XNs *nsl_find_prefix(NsList *l, A2Str *prefix) { int i; for (i = 0; i < l->n; i++) if ((l->v[i].prefix == prefix) || (!l->v[i].prefix && prefix && prefix->len == 0) || (l->v[i].prefix && !prefix && l->v[i].prefix->len == 0)) return &l->v[i]; return NULL; }

/* [[InScopeNamespaces]] of an element: own declarations plus ancestors' not shadowed */
static void in_scope_namespaces(XNode *n, NsList *out) {
    XNode *y; int i;
    for (y = n; y; y = y->parent) {
        for (i = 0; i < y->nns; i++) {
            A2Str *pre = y->ns[i].prefix; int j, shadowed = 0;
            for (j = 0; j < out->n; j++) if ((out->v[j].prefix == pre) || (!out->v[j].prefix && pre && pre->len == 0) || (out->v[j].prefix && !pre && out->v[j].prefix->len == 0)) { shadowed = 1; break; }
            if (!shadowed) nsl_add(out, pre, y->ns[i].uri);
        }
    }
}

/* ---------------------------------------------------------------- */
/* parser                                                           */

typedef struct { VM2 *vm; const char *p; const char *end; int ignore_ws, ignore_comments, ignore_pi; int err; } XP;

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static void xp_ws(XP *x) { while (x->p < x->end && is_ws(*x->p)) x->p++; }
static int xp_lax;   /* SWF 9 string children: the old parser accepted any name characters */
static int name_start(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' || c >= 0x80 || (xp_lax && c == '@'); }
static int name_char(unsigned char c) { return name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.'; }
static A2Str *xp_name(XP *x) { const char *s = x->p; while (x->p < x->end && name_char((unsigned char)*x->p)) x->p++; return a2_intern(x->vm, s, (size_t)(x->p - s)); }

/* Flash's entity decoder: at '&' the text up to the next ';' is one token; unknown tokens are copied verbatim */
static A2Str *decode_entities(VM2 *vm, const char *s, size_t n) {
    char *buf; size_t o = 0, i; int touched = 0;
    for (i = 0; i < n; i++) if (s[i] == '&') { touched = 1; break; }
    if (!touched) return a2_intern(vm, s, n);
    buf = (char*)malloc(n * 4 + 8);
    for (i = 0; i < n; i++) {
        if (s[i] == '&') {
            size_t j = i + 1; while (j < n && s[j] != ';') j++;
            if (j < n) {
                size_t tl = j - i - 1; const char *t = s + i + 1;
                if (tl == 3 && !strncmp(t, "amp", 3)) buf[o++] = '&';
                else if (tl == 2 && !strncmp(t, "lt", 2)) buf[o++] = '<';
                else if (tl == 2 && !strncmp(t, "gt", 2)) buf[o++] = '>';
                else if (tl == 4 && !strncmp(t, "quot", 4)) buf[o++] = '"';
                else if (tl == 4 && !strncmp(t, "apos", 4)) buf[o++] = '\'';
                else if (tl >= 2 && t[0] == '#') {
                    char *e; u32 c = (t[1] == 'x' || t[1] == 'X') ? (u32)strtoul(t + 2, &e, 16) : (u32)strtoul(t + 1, &e, 10);
                    if (e == s + j && e > t + 1) { extern A2Str *a2_utf8_encode(VM2 *vm, const u32 *u, int n); A2Str *u = a2_utf8_encode(vm, &c, 1); memcpy(buf + o, u->s, u->len); o += u->len; }
                    else { memcpy(buf + o, s + i, j - i + 1); o += j - i + 1; }
                }
                else { memcpy(buf + o, s + i, j - i + 1); o += j - i + 1; }
                i = j; continue;
            }
        }
        buf[o++] = s[i];
    }
    { A2Str *r = a2_intern(vm, buf, o); free(buf); return r; }
}
static void split_qname(VM2 *vm, A2Str *q, A2Str **prefix, A2Str **local) { const char *c = strchr(q->s, ':'); if (!c) { *prefix = NULL; *local = q; return; } *prefix = a2_intern(vm, q->s, (size_t)(c - q->s)); *local = a2_str(vm, c + 1); }

/* parse the content of `parent` until a close tag or end of input; returns 0 on error (thrown) */
static int xp_parse_into(XP *x, XNode *parent, int depth) {
    VM2 *vm = x->vm;
    while (x->p < x->end && !x->err) {
        if (x->p[0] == '<') {
            if (x->p + 1 >= x->end) { type_error(vm, 1090, NULL, NULL); x->err = 1; return 0; }
            if (x->p[1] == '/') return 1;
            if (x->end - x->p >= 4 && !strncmp(x->p, "<!--", 4)) {
                const char *e = strstr(x->p + 4, "-->"); if (!e || e >= x->end) { type_error(vm, 1094, NULL, NULL); x->err = 1; return 0; }
                if (!x->ignore_comments) { XNode *c = xn_new(vm, XN_COMMENT); c->value = a2_intern(vm, x->p + 4, (size_t)(e - x->p - 4)); xn_add_kid(parent, c); }
                x->p = e + 3; continue;
            }
            if (x->end - x->p >= 9 && !strncmp(x->p, "<![", 3) && x->p[8] == '[' && (x->p[3] | 0x20) == 'c' && (x->p[4] | 0x20) == 'd' && (x->p[5] | 0x20) == 'a' && (x->p[6] | 0x20) == 't' && (x->p[7] | 0x20) == 'a') {   /* any case */
                const char *e = strstr(x->p + 9, "]]>"); if (!e || e >= x->end) { type_error(vm, 1091, NULL, NULL); x->err = 1; return 0; }
                { XNode *c = xn_new(vm, XN_TEXT); c->cdata = 1; c->value = a2_intern(vm, x->p + 9, (size_t)(e - x->p - 9)); xn_add_kid(parent, c); }
                x->p = e + 3; continue;
            }
            if (x->p[1] == '?') {
                const char *e = strstr(x->p + 2, "?>");
                if (!e || e >= x->end) {   /* an unterminated <?xml ...?> declaration has its own error */
                    int decl = x->end - x->p >= 5 && (x->p[2] | 0x20) == 'x' && (x->p[3] | 0x20) == 'm' && (x->p[4] | 0x20) == 'l' && (x->end - x->p == 5 || !name_char((unsigned char)x->p[5]));
                    type_error(vm, decl ? 1092 : 1097, NULL, NULL); x->err = 1; return 0;
                }
                { XP t = *x; A2Str *nm; t.p = x->p + 2; nm = xp_name(&t);
                  if (nm->len == 3 && (nm->s[0] == 'x' || nm->s[0] == 'X') && (nm->s[1] == 'm' || nm->s[1] == 'M') && (nm->s[2] == 'l' || nm->s[2] == 'L')) { /* xml declaration: ignored */ }
                  else if (!x->ignore_pi) { XNode *c = xn_new(vm, XN_PI); c->local = nm; xp_ws(&t); c->value = a2_intern(vm, t.p, t.p < e ? (size_t)(e - t.p) : 0); xn_add_kid(parent, c); } }
                x->p = e + 2; continue;
            }
            if (x->p[1] == '!') {
                if (x->end - x->p >= 9 && !strncmp(x->p, "<!DOCTYPE", 9)) { const char *e = x->p + 9; int lvl = 0; while (e < x->end) { if (*e == '[') lvl++; else if (*e == ']') lvl--; else if (*e == '>' && lvl <= 0) break; e++; } if (e >= x->end) { type_error(vm, 1093, NULL, NULL); x->err = 1; return 0; } x->p = e + 1; continue; }
                { const char *e = strchr(x->p, '>'); if (!e || e >= x->end) { type_error(vm, 1090, NULL, NULL); x->err = 1; return 0; } x->p = e + 1; continue; }
            }
            {
                XNode *el; A2Str *q, *pre, *loc; int i;
                x->p++;
                if (x->p >= x->end || !name_start((unsigned char)*x->p)) { type_error(vm, 1090, NULL, NULL); x->err = 1; return 0; }
                q = xp_name(x); split_qname(vm, q, &pre, &loc);
                if (!loc->len || (pre && !pre->len) || strchr(loc->s, ':')) { type_error(vm, 1084, q->s, NULL); x->err = 1; return 0; }
                el = xn_new(vm, XN_ELEMENT); el->local = loc; el->prefix = pre;
                xn_add_kid(parent, el);
                xp_ws(x);
                while (x->p < x->end && *x->p != '>' && *x->p != '/') {
                    A2Str *an, *apre, *aloc; XNode *a; char quote; const char *vs; A2Str *val;
                    if (!name_start((unsigned char)*x->p)) { type_error(vm, 1090, NULL, NULL); x->err = 1; return 0; }
                    an = xp_name(x); split_qname(vm, an, &apre, &aloc);
                    if (!aloc->len || (apre && !apre->len) || strchr(aloc->s, ':')) { type_error(vm, 1084, an->s, NULL); x->err = 1; return 0; }
                    xp_ws(x);
                    if (x->p >= x->end || *x->p != '=') { type_error(vm, 1090, NULL, NULL); x->err = 1; return 0; }
                    x->p++; xp_ws(x);
                    if (x->p >= x->end || (*x->p != '"' && *x->p != '\'')) { type_error(vm, x->p >= x->end ? 1095 : 1090, NULL, NULL); x->err = 1; return 0; }   /* unquoted value: malformed element */
                    quote = *x->p++; vs = x->p; while (x->p < x->end && *x->p != quote) x->p++;
                    if (x->p >= x->end) { type_error(vm, 1095, NULL, NULL); x->err = 1; return 0; }
                    val = decode_entities(vm, vs, (size_t)(x->p - vs)); x->p++;
                    if (apre && apre->len == 5 && !strcmp(apre->s, "xmlns")) { xn_add_ns(el, aloc, val); xp_ws(x); continue; }
                    if (!apre && aloc->len == 5 && !strcmp(aloc->s, "xmlns")) { xn_add_ns(el, vm->s_empty, val); xp_ws(x); continue; }
                    a = xn_new(vm, XN_ATTR); a->local = aloc; a->prefix = apre; a->value = val;
                    xn_add_attr(el, a);
                    xp_ws(x);
                }
                if (x->p >= x->end) { type_error(vm, 1090, NULL, NULL); x->err = 1; return 0; }
                /* resolve namespaces */
                { A2Str *u = lookup_prefix_uri(el, pre ? pre : vm->s_empty);
                  if (!u) { if (pre) { if (pre->len == 3 && !strcmp(pre->s, "xml")) u = a2_str(vm, "http://www.w3.org/XML/1998/namespace"); else { type_error(vm, 1083, pre->s, loc->s); x->err = 1; return 0; } } else u = default_ns(vm); }
                  el->uri = u; }
                for (i = 0; i < el->nattrs; i++) { XNode *a = el->attrs[i];
                    if (a->prefix) { A2Str *u = lookup_prefix_uri(el, a->prefix); if (!u) { if (a->prefix->len == 3 && !strcmp(a->prefix->s, "xml")) u = a2_str(vm, "http://www.w3.org/XML/1998/namespace"); else { type_error(vm, 1083, a->prefix->s, loc->s); x->err = 1; return 0; } } a->uri = u; }
                    else a->uri = vm->s_empty; }
                for (i = 0; i < el->nattrs; i++) { int j; for (j = 0; j < i; j++) if (el->attrs[i]->local == el->attrs[j]->local && el->attrs[i]->uri == el->attrs[j]->uri) { char an[300]; sprintf(an, "%s%s%s", el->attrs[i]->prefix ? el->attrs[i]->prefix->s : "", el->attrs[i]->prefix ? ":" : "", el->attrs[i]->local->s); type_error(vm, 1104, an, q->s); x->err = 1; return 0; } }
                if (*x->p == '/') { x->p++; if (x->p >= x->end || *x->p != '>') { type_error(vm, 1090, NULL, NULL); x->err = 1; return 0; } x->p++; continue; }
                x->p++;   /* '>' */
                if (depth > 1000) { type_error(vm, 1090, NULL, NULL); x->err = 1; return 0; }
                if (!xp_parse_into(x, el, depth + 1)) return 0;
                /* close tag */
                if (x->p >= x->end || x->end - x->p < 2 || x->p[0] != '<' || x->p[1] != '/') { type_error(vm, 1085, q->s, q->s); x->err = 1; return 0; }
                { A2Str *cq; x->p += 2; cq = xp_name(x);
                  if (cq != q) { type_error(vm, 1085, q->s, q->s); x->err = 1; return 0; }
                  while (x->p < x->end && *x->p != '>') x->p++;      /* Flash ignores anything after the name */
                  if (x->p >= x->end) { type_error(vm, 1090, NULL, NULL); x->err = 1; return 0; }   /* "</b": malformed */
                  x->p++; }
                continue;
            }
        }
        {
            const char *s = x->p; XNode *t;
            while (x->p < x->end && *x->p != '<') x->p++;
            if (x->ignore_ws) { const char *a = s, *b = x->p; while (a < b && is_ws(*a)) a++; while (b > a && is_ws(b[-1])) b--; if (a == b) continue; t = xn_new(vm, XN_TEXT); t->value = decode_entities(vm, a, (size_t)(b - a)); }
            else { t = xn_new(vm, XN_TEXT); t->value = decode_entities(vm, s, (size_t)(x->p - s)); }
            xn_add_kid(parent, t);
        }
    }
    return 1;
}

/* parse a string into a list of top-level nodes (returned as a detached root element) */
static XNode *parse_fragment(VM2 *vm, A2Str *s) {
    XP x; XNode *root = xn_new(vm, XN_ELEMENT);
    x.vm = vm; x.p = s->s; x.end = s->s + s->len; x.ignore_ws = g_ignore_ws; x.ignore_comments = g_ignore_comments; x.ignore_pi = g_ignore_pi; x.err = 0;
    root->local = vm->s_empty;
    if (!xp_parse_into(&x, root, 0)) return NULL;
    if (x.p < x.end) { /* stray close tag at top level */ type_error(vm, 1088, NULL, NULL); return NULL; }
    return root;
}
/* ToXML(string) */
static XNode *parse_xml(VM2 *vm, A2Str *s) {
    XNode *root = parse_fragment(vm, s), *r = NULL; int i, nel = 0;
    if (!root) return NULL;
    for (i = 0; i < root->nkids; i++) if (root->kids[i]->kind == XN_ELEMENT) { nel++; r = root->kids[i]; }
    if (nel > 1) { type_error(vm, 1088, NULL, NULL); return NULL; }
    if (nel == 1) {
        /* other top-level nodes (comments/PIs/whitespace) are dropped */
        xn_detach(r); return r;
    }
    /* no element: whitespace-only text is dropped; exactly one remaining node is the result */
    for (i = root->nkids - 1; i >= 0; i--) { XNode *k = root->kids[i]; int ws = 0; if (k->kind == XN_TEXT) { u32 j; ws = 1; for (j = 0; j < k->value->len; j++) if (!is_ws(k->value->s[j])) { ws = 0; break; } } if (ws) xn_remove_kid_at(root, i); }
    if (root->nkids == 0) { r = xn_new(vm, XN_TEXT); return r; }
    if (root->nkids > 1) { type_error(vm, 1088, NULL, NULL); return NULL; }
    r = root->kids[0];
    xn_detach(r); return r;
}

/* ---------------------------------------------------------------- */
/* serialization                                                    */

typedef struct { char *buf; size_t n, cap; } XB;
static void xb_put(XB *b, const char *s, size_t n) { if (b->n + n + 1 > b->cap) { while (b->n + n + 1 > b->cap) b->cap *= 2; b->buf = (char*)realloc(b->buf, b->cap); } memcpy(b->buf + b->n, s, n); b->n += n; b->buf[b->n] = 0; }
static void xb_puts(XB *b, const char *s) { xb_put(b, s, strlen(s)); }
static void xb_init(XB *b) { b->cap = 256; b->n = 0; b->buf = (char*)malloc(b->cap); b->buf[0] = 0; }
static A2Str *xb_done(VM2 *vm, XB *b) { A2Str *r = a2_intern(vm, b->buf, b->n); free(b->buf); return r; }
static void esc_elem(XB *b, const char *s, size_t n) { size_t i; for (i = 0; i < n; i++) { char c = s[i]; if (c == '<') xb_puts(b, "&lt;"); else if (c == '>') xb_puts(b, "&gt;"); else if (c == '&') xb_puts(b, "&amp;"); else xb_put(b, &c, 1); } }
static void esc_attr(XB *b, const char *s, size_t n) { size_t i; for (i = 0; i < n; i++) { char c = s[i]; if (c == '<') xb_puts(b, "&lt;"); else if (c == '&') xb_puts(b, "&amp;"); else if (c == '"') xb_puts(b, "&quot;"); else if (c == '\n') xb_puts(b, "&#xA;"); else if (c == '\r') xb_puts(b, "&#xD;"); else if (c == '\t') xb_puts(b, "&#x9;"); else xb_put(b, &c, 1); } }
static void xb_indent(XB *b, int n) { int i; for (i = 0; i < n; i++) xb_put(b, " ", 1); }
static int has_simple_content(XNode *n) { int i; if (n->kind == XN_COMMENT || n->kind == XN_PI) return 0; if (n->kind != XN_ELEMENT) return 1; for (i = 0; i < n->nkids; i++) if (n->kids[i]->kind == XN_ELEMENT) return 0; return 1; }

static A2Str *gen_prefix(VM2 *vm, NsList *l, A2Str *uri) {
    /* a unique prefix derived from the uri's tail (avmplus style: "aaa", "aab", ... when unavailable) */
    char buf[16]; int k, j; (void)uri;
    strcpy(buf, "aaa");
    for (k = 0; k < 17576; k++) {
        { A2Str *p = a2_str(vm, buf); if (!nsl_find_prefix(l, p)) return p; }
        for (j = 2; j >= 0; j--) { if (buf[j] < 'z') { buf[j]++; break; } buf[j] = 'a'; }
    }
    return a2_str(vm, "ns");
}

static void serialize(VM2 *vm, XB *b, XNode *n, NsList *anc, int indent, int pretty);
static void serialize_element(VM2 *vm, XB *b, XNode *x, NsList *anc, int indent, int pretty) {
    NsList decl; NsList all; int i; XNs *ns; A2Str *eprefix;
    int indent_children;
    memset(&decl, 0, sizeof(decl)); memset(&all, 0, sizeof(all));
    /* namespaceDeclarations = in-scope namespaces not already declared by ancestors */
    { NsList insc; memset(&insc, 0, sizeof(insc)); in_scope_namespaces(x, &insc);
      for (i = 0; i < insc.n; i++) if (!nsl_has(anc, insc.v[i].prefix, insc.v[i].uri)) nsl_add(&decl, insc.v[i].prefix, insc.v[i].uri);
      free(insc.v); }
    for (i = 0; i < anc->n; i++) nsl_add(&all, anc->v[i].prefix, anc->v[i].uri);
    for (i = 0; i < decl.n; i++) nsl_add(&all, decl.v[i].prefix, decl.v[i].uri);
    /* element namespace */
    ns = nsl_find_uri(&all, x->uri, NULL);   /* the first in-scope namespace with the uri, ancestors first (avmplus GetNamespace) */
    if (!ns && x->uri->len == 0) {
        /* no namespace: Flash never emits xmlns="" */
        static XNs none; none.prefix = vm->s_empty; none.uri = x->uri; ns = &none;
    }
    if (!ns) {
        A2Str *pre;
        { XNs *d = nsl_find_prefix(&all, vm->s_empty); if (!d && x->prefix && x->prefix->len && !nsl_find_prefix(&all, x->prefix)) pre = x->prefix; else if (d && d->uri != x->uri) pre = (x->prefix && x->prefix->len && !nsl_find_prefix(&all, x->prefix)) ? x->prefix : gen_prefix(vm, &all, x->uri); else pre = (x->prefix && x->prefix->len && !nsl_find_prefix(&all, x->prefix)) ? x->prefix : vm->s_empty; }
        nsl_add(&decl, pre, x->uri); nsl_add(&all, pre, x->uri); ns = &all.v[all.n - 1];
    } else if (!ns->prefix) { /* declared without prefix (undefined): assign one */
        A2Str *pre = x->uri->len ? gen_prefix(vm, &all, x->uri) : vm->s_empty; ns->prefix = pre; nsl_add(&decl, pre, x->uri);
    }
    eprefix = ns->prefix;
    if (pretty) xb_indent(b, indent);
    xb_puts(b, "<"); if (eprefix && eprefix->len) { xb_puts(b, eprefix->s); xb_puts(b, ":"); } xb_puts(b, x->local->s);
    /* attributes */
    for (i = 0; i < x->nattrs; i++) {
        XNode *a = x->attrs[i]; A2Str *apre = NULL;
        if (a->uri->len) {
            XNs *ans = nsl_find_uri(&all, a->uri, a->prefix);
            if (!ans || !ans->prefix) {   /* an in-scope default namespace with the attribute's uri serves it unprefixed (Flash) */
                XNs *d = nsl_find_prefix(&all, vm->s_empty);
                A2Str *pre = (a->prefix && a->prefix->len && !nsl_find_prefix(&all, a->prefix)) ? a->prefix : ((!d || d->uri == a->uri) ? vm->s_empty : gen_prefix(vm, &all, a->uri));
                if (ans && !ans->prefix) ans->prefix = pre; else { nsl_add(&all, pre, a->uri); ans = &all.v[all.n - 1]; }
                nsl_add(&decl, pre, a->uri);
            }
            apre = ans->prefix;
        }
        xb_puts(b, " "); if (apre && apre->len) { xb_puts(b, apre->s); xb_puts(b, ":"); } xb_puts(b, a->local->s); xb_puts(b, "=\""); esc_attr(b, a->value->s, a->value->len); xb_puts(b, "\"");
    }
    /* namespace declarations */
    for (i = 0; i < decl.n; i++) {
        if (decl.v[i].prefix && decl.v[i].prefix->len) { xb_puts(b, " xmlns:"); xb_puts(b, decl.v[i].prefix->s); xb_puts(b, "=\""); }
        else xb_puts(b, " xmlns=\"");
        esc_attr(b, decl.v[i].uri->s, decl.v[i].uri->len); xb_puts(b, "\"");
    }
    if (x->nkids == 0) { xb_puts(b, "/>"); free(decl.v); free(all.v); return; }
    xb_puts(b, ">");
    indent_children = pretty && !has_simple_content(x);
    for (i = 0; i < x->nkids; i++) {
        if (indent_children) xb_puts(b, "\n");
        serialize(vm, b, x->kids[i], &all, indent_children ? indent + g_indent : 0, indent_children);
    }
    if (indent_children) { xb_puts(b, "\n"); xb_indent(b, indent); }
    xb_puts(b, "</"); if (eprefix && eprefix->len) { xb_puts(b, eprefix->s); xb_puts(b, ":"); } xb_puts(b, x->local->s); xb_puts(b, ">");
    free(decl.v); free(all.v);
}
static void serialize(VM2 *vm, XB *b, XNode *n, NsList *anc, int indent, int pretty) {
    switch (n->kind) {
    case XN_TEXT: {
        const char *s = n->value->s, *e = s + n->value->len;
        if (n->cdata) { if (pretty) xb_indent(b, indent); xb_puts(b, "<![CDATA["); xb_put(b, s, (size_t)(e - s)); xb_puts(b, "]]>"); return; }
        if (pretty) { xb_indent(b, indent); while (s < e && is_ws(*s)) s++; while (e > s && is_ws(e[-1])) e--; }
        esc_elem(b, s, (size_t)(e - s)); return; }
    case XN_ATTR: esc_attr(b, n->value->s, n->value->len); return;
    case XN_COMMENT: if (pretty) xb_indent(b, indent); xb_puts(b, "<!--"); xb_put(b, n->value->s, n->value->len); xb_puts(b, "-->"); return;
    case XN_PI: if (pretty) xb_indent(b, indent); xb_puts(b, "<?"); xb_puts(b, n->local->s); if (n->value->len) { xb_puts(b, " "); xb_put(b, n->value->s, n->value->len); } xb_puts(b, "?>"); return;
    default: serialize_element(vm, b, n, anc, indent, pretty); return;
    }
}
static A2Str *node_to_xmlstring(VM2 *vm, XNode *n) {
    XB b; NsList anc; xb_init(&b); memset(&anc, 0, sizeof(anc));
    serialize(vm, &b, n, &anc, 0, g_pretty && g_indent >= 0);
    free(anc.v); return xb_done(vm, &b);
}
static A2Str *node_to_string(VM2 *vm, XNode *n) {
    XB b; int i;
    if (n->kind == XN_TEXT || n->kind == XN_ATTR) return n->value;
    if (!has_simple_content(n)) return node_to_xmlstring(vm, n);
    xb_init(&b);
    for (i = 0; i < n->nkids; i++) if (n->kids[i]->kind == XN_TEXT) xb_put(&b, n->kids[i]->value->s, n->kids[i]->value->len);
    return xb_done(vm, &b);
}
static A2Str *list_to_string(VM2 *vm, XList *l, int xmlstring) {
    XB b; int i; xb_init(&b);
    if (!xmlstring) {
        int simple = 1;
        if (l->n == 1) simple = has_simple_content(l->items[0]);
        else for (i = 0; i < l->n; i++) if (l->items[i]->kind == XN_ELEMENT) { simple = 0; break; }
        if (simple) { for (i = 0; i < l->n; i++) if (l->items[i]->kind != XN_COMMENT && l->items[i]->kind != XN_PI) { A2Str *s = node_to_string(vm, l->items[i]); xb_put(&b, s->s, s->len); } return xb_done(vm, &b); }
    }
    for (i = 0; i < l->n; i++) { A2Str *s = node_to_xmlstring(vm, l->items[i]); if (i) xb_puts(&b, "\n"); xb_put(&b, s->s, s->len); }
    return xb_done(vm, &b);
}
A2Str *a2_xml_tostring(VM2 *vm, A2Obj *o) { if (!o->payload) return vm->s_empty; if (*(int*)o->payload) return list_to_string(vm, (XList*)o->payload, 0); return node_to_string(vm, (XNode*)o->payload); }

/* ---------------------------------------------------------------- */
/* copies and conversions                                           */

static XNode *deep_copy(VM2 *vm, XNode *n) {
    XNode *c = xn_new(vm, n->kind); int i;
    c->local = n->local; c->uri = n->uri; c->prefix = n->prefix; c->value = n->value; c->cdata = n->cdata;
    for (i = 0; i < n->nns; i++) xn_add_ns(c, n->ns[i].prefix, n->ns[i].uri);
    for (i = 0; i < n->nattrs; i++) xn_add_attr(c, deep_copy(vm, n->attrs[i]));
    for (i = 0; i < n->nkids; i++) xn_add_kid(c, deep_copy(vm, n->kids[i]));
    return c;
}
/* new XML(xml): a copy that drops comments / processing instructions the settings ignore */
static XNode *deep_copy_filtered(VM2 *vm, XNode *n) {
    XNode *c = xn_new(vm, n->kind); int i;
    c->local = n->local; c->uri = n->uri; c->prefix = n->prefix; c->value = n->value; c->cdata = n->cdata;
    for (i = 0; i < n->nns; i++) xn_add_ns(c, n->ns[i].prefix, n->ns[i].uri);
    for (i = 0; i < n->nattrs; i++) xn_add_attr(c, deep_copy(vm, n->attrs[i]));
    for (i = 0; i < n->nkids; i++) {
        if ((n->kids[i]->kind == XN_COMMENT && g_ignore_comments) || (n->kids[i]->kind == XN_PI && g_ignore_pi)) continue;
        xn_add_kid(c, deep_copy_filtered(vm, n->kids[i]));
    }
    return c;
}
/* a copy of an element keeps the namespaces that were in scope from its ancestors (so names still serialize) */
static XNode *copy_with_scope(VM2 *vm, XNode *n) {
    XNode *c = deep_copy(vm, n);
    if (n->kind == XN_ELEMENT && n->parent) { NsList l; int i; memset(&l, 0, sizeof(l)); in_scope_namespaces(n->parent, &l); for (i = 0; i < l.n; i++) { int j, have = 0; for (j = 0; j < c->nns; j++) if (c->ns[j].prefix == l.v[i].prefix) have = 1; if (!have && l.v[i].uri->len) xn_add_ns(c, l.v[i].prefix, l.v[i].uri); } free(l.v); }
    return c;
}
static XNode *text_node(VM2 *vm, A2Str *s) { XNode *t = xn_new(vm, XN_TEXT); t->value = s; return t; }
static A2Str *value_to_string(VM2 *vm, V2 v) { if (is_xmlobj(v)) return a2_xml_tostring(vm, v.u.o); return v2_tostr(vm, v); }

/* ---------------------------------------------------------------- */
/* internal methods: XML                                             */

/* [[Insert]](i, V) */
static void notify(VM2 *vm, XNode *target, const char *cmd, V2 value, V2 detail);
static void xml_insert(VM2 *vm, XNode *x, int i, V2 v) {
    if (x->kind != XN_ELEMENT) return;
    if (i < 0) i = 0; if (i > x->nkids) i = x->nkids;
    if (is_node(v)) { XNode *n = NOD(v); if (xn_is_ancestor(n, x)) { type_error(vm, 1118, NULL, NULL); return; } }
    if (is_list(v)) { XList *l = LST(v); int k; for (k = 0; k < l->n; k++) { XNode *c = l->items[k]; if (c->parent) c = copy_with_scope(vm, c); else xn_detach(c); xn_insert_kid(x, i + k, c); } return; }
    if (is_node(v)) { XNode *c = NOD(v); if (c->kind == XN_ATTR || c->kind == XN_TEXT) c = text_node(vm, c->value); else if (c->parent) { extern struct SWFMovie *a2_code_movie(VM2 *vm); struct SWFMovie *cm = a2_code_movie(vm);
            if ((cm ? cm->version : vm->swf_version) >= 21 || c->parent == x) { if (c->parent == x) { int j; for (j = 0; j < x->nkids; j++) if (x->kids[j] == c && j < i) i--; } xn_detach(c); } } xn_insert_kid(x, i, c); return; }   /* the node itself is reparented (avmplus does not copy) */
    xn_insert_kid(x, i, text_node(vm, v2_tostr(vm, v)));
}
/* [[Replace]](i, V): replace child i (or append when i >= length) */
static void xml_replace(VM2 *vm, XNode *x, int i, V2 v) {
    if (x->kind != XN_ELEMENT) return;
    if (i < 0) i = 0;
    if (i >= x->nkids) { i = x->nkids; xml_insert(vm, x, i, v); return; }
    if (is_node(v) && NOD(v)->kind != XN_ATTR) {
        XNode *c = NOD(v);
        if (xn_is_ancestor(c, x)) { type_error(vm, 1118, NULL, NULL); return; }
        if (c->parent) c = copy_with_scope(vm, c);
        { XNode *old = x->kids[i]; xn_remove_kid_at(x, i); xn_insert_kid(x, i, c); notify(vm, x, "nodeChanged", wrap(c), wrap(old)); } return;
    }
    if (is_list(v)) { XList *l = LST(v); xn_remove_kid_at(x, i); xml_insert(vm, x, i, v); (void)l; return; }
    { XNode *old = x->kids[i]; A2Str *s = value_to_string(vm, v);
      if (old->kind == XN_TEXT) { old->value = s; return; }
      xn_remove_kid_at(x, i); xn_insert_kid(x, i, text_node(vm, s)); }
}

static V2 xml_get(VM2 *vm, XNode *x, const XQ *q);
static V2 list_get(VM2 *vm, A2Obj *lo, const XQ *q);

/* [[Get]] on XML (non-numeric names) */
static V2 xml_get(VM2 *vm, XNode *x, const XQ *q) {
    A2Obj *r = new_list(vm); XList *l = LST(v2_obj(r)); int i;
    l->target = x->obj; l->has_tprop = 1; l->tprop = *q;
    if (q->attr) { for (i = 0; i < x->nattrs; i++) if (xq_matches(q, x->attrs[i])) list_add(l, x->attrs[i]); }
    else if (x->kind == XN_ELEMENT) { for (i = 0; i < x->nkids; i++) if (xq_matches(q, x->kids[i])) list_add(l, x->kids[i]); }
    return v2_obj(r);
}
static V2 list_get(VM2 *vm, A2Obj *lo, const XQ *q) {
    XList *src = lst_of(lo); A2Obj *r = new_list(vm); XList *l = LST(v2_obj(r)); int i;
    l->target = lo; l->has_tprop = 1; l->tprop = *q;
    for (i = 0; i < src->n; i++) {
        XNode *x = src->items[i]; int k;
        if (x->kind != XN_ELEMENT) continue;
        if (q->attr) { for (k = 0; k < x->nattrs; k++) if (xq_matches(q, x->attrs[k])) list_add(l, x->attrs[k]); }
        else for (k = 0; k < x->nkids; k++) if (xq_matches(q, x->kids[k])) list_add(l, x->kids[k]);
    }
    return v2_obj(r);
}

/* find a namespace declaration (prefix) for an attribute/element uri when assigning qualified names */
static void ensure_ns_decl(VM2 *vm, XNode *el, A2Str *uri, A2Str *prefix) {
    if (!uri->len) return;
    if (prefix) { A2Str *u = lookup_prefix_uri(el, prefix); if (u == uri) return; xn_add_ns(el, prefix, uri); return; }
    { NsList l; int i, found = 0; memset(&l, 0, sizeof(l)); in_scope_namespaces(el, &l); for (i = 0; i < l.n; i++) if (l.v[i].uri == uri) found = 1; free(l.v); if (found) return; }
    (void)vm;
}

/* [[Put]] on XML */
/* XML.setNotification: the functions of the node and its ancestors are told about changes */
static void notify(VM2 *vm, XNode *target, const char *cmd, V2 value, V2 detail) {
    XNode *c; int g = 0;
    for (c = target; c && g++ < 4096; c = c->parent) {
        if (c->notify.t == T_OBJ && c->notify.u.o->kind == OK_FUNCTION) {
            V2 a[5]; int saved;
            a[0] = wrap(c); a[1] = v2_cstr(vm, cmd); a[2] = wrap(target); a[3] = value; a[4] = detail;
            a2_call(vm, c->notify, v2_null(), 5, a);
            saved = vm->throwing; if (saved) { vm->throwing = 0; vm->exc = v2_undef(); }
        }
    }
}
/* a text node (or a list of exactly one) assigns like a string */
static int is_textish(V2 v) { if (is_node(v)) return NOD(v)->kind == XN_TEXT; if (is_list(v)) return LST(v)->n == 1 && LST(v)->items[0]->kind == XN_TEXT; return 0; }
static void xml_put(VM2 *vm, XNode *x, const XQ *q, V2 v, A2Str *prefix_hint) {
    int i;
    if (x->kind != XN_ELEMENT) return;
    if (q->attr) {
        A2Str *s; XNode *a = NULL;
        if (is_list(v)) { XList *l = LST(v); XB b; int k; xb_init(&b); for (k = 0; k < l->n; k++) { A2Str *t = node_to_string(vm, l->items[k]); if (k) xb_puts(&b, " "); xb_put(&b, t->s, t->len); } s = xb_done(vm, &b); }
        else s = value_to_string(vm, v);
        if (!valid_name(q->local) && !is_star(q->local)) return;
        for (i = 0; i < x->nattrs; i++) if (xq_matches(q, x->attrs[i])) { if (!a) a = x->attrs[i]; else { xn_remove_attr_at(x, i); i--; } }
        if (!a && is_star(q->local)) return;   /* x.@* = v sets the first attribute and drops the others */
        if (a) { A2Str *old = a->value; a->value = s; notify(vm, x, "attributeChanged", v2_str(a->local), old ? v2_str(old) : v2_str(vm->s_empty)); return; }
        a = xn_new(vm, XN_ATTR); a->local = q->local; a->uri = q->uri ? q->uri : vm->s_empty; a->prefix = prefix_hint; a->value = s;
        if (a->uri->len) ensure_ns_decl(vm, x, a->uri, prefix_hint);
        xn_add_attr(x, a);
        notify(vm, x, "attributeAdded", v2_str(a->local), v2_str(s));
        return;
    }
    {
        int primitive_assign = (!is_xmlobj(v) || is_textish(v)) && !is_star(q->local);
        int idx = -1;
        if (!is_star(q->local) && !valid_name(q->local)) return;
        for (i = x->nkids - 1; i >= 0; i--) if (xq_matches(q, x->kids[i])) { if (idx < 0) idx = i; else { xn_remove_kid_at(x, i); idx--; } }
        if (idx < 0) {
            idx = x->nkids;
            if (primitive_assign) { XNode *y = xn_new(vm, XN_ELEMENT); y->local = q->local; y->uri = q->uri ? q->uri : (q->dxns ? q->dxns : default_ns(vm)); y->prefix = prefix_hint; if (y->uri->len) ensure_ns_decl(vm, x, y->uri, prefix_hint); xn_add_kid(x, y); }
        }
        if (primitive_assign) {
            XNode *y = idx < x->nkids ? x->kids[idx] : NULL; A2Str *s = value_to_string(vm, v);
            if (!y) return;
            if (y->kind != XN_ELEMENT) { xn_remove_kid_at(x, idx); xn_insert_kid(x, idx, text_node(vm, s)); return; }
            while (y->nkids) xn_remove_kid_at(y, y->nkids - 1);
            if (s->len) xn_add_kid(y, text_node(vm, s));
            notify(vm, y, "textSet", v2_str(s), v2_null());
            return;
        }
        if (is_list(v) && idx < x->nkids) { xn_remove_kid_at(x, idx); xml_insert(vm, x, idx, v); return; }
        xml_replace(vm, x, idx, v);
    }
}

/* [[Delete]] on XML */
static int xml_delete(VM2 *vm, XNode *x, const XQ *q) {
    int i;
    if (q->attr) { for (i = x->nattrs - 1; i >= 0; i--) if (xq_matches(q, x->attrs[i])) { XNode *a = x->attrs[i]; xn_remove_attr_at(x, i); notify(vm, x, "attributeRemoved", v2_str(a->local), a->value ? v2_str(a->value) : v2_str(vm->s_empty)); } return 1; }
    if (x->kind != XN_ELEMENT) return 1;
    for (i = x->nkids - 1; i >= 0; i--) if (xq_matches(q, x->kids[i])) { XNode *k = x->kids[i]; xn_remove_kid_at(x, i); notify(vm, x, "nodeRemoved", wrap(k), v2_null()); }
    return 1;
}

/* [[ResolveValue]] for XMLList */
static V2 list_resolve(VM2 *vm, A2Obj *lo) {
    XList *l = lst_of(lo); V2 base, tv;
    if (l->n > 0) return v2_obj(lo);
    if (!l->target || !l->has_tprop || l->tprop.attr || is_star(l->tprop.local)) return v2_null();
    if (is_list(v2_obj(l->target))) base = list_resolve(vm, l->target); else base = v2_obj(l->target);
    if (base.t != T_OBJ) return v2_null();
    if (is_list(base) && LST(base)->n > 1) { type_error(vm, 1089, NULL, NULL); return v2_null(); }   /* x.*.a.b = v */
    if (is_list(base) && LST(base)->n != 1) return v2_null();
    if (is_list(base)) base = wrap(LST(base)->items[0]);
    tv = xml_get(vm, NOD(base), &l->tprop);
    if (LST(tv)->n == 0) {
        xml_put(vm, NOD(base), &l->tprop, v2_str(vm->s_empty), NULL);
        tv = xml_get(vm, NOD(base), &l->tprop);
        if (LST(tv)->n == 0) return v2_null();
    }
    return tv;
}

/* [[Put]] on XMLList */
static void list_put_index(VM2 *vm, A2Obj *lo, u32 idx, V2 v);
static void list_put(VM2 *vm, A2Obj *lo, const XQ *q, V2 v, A2Str *prefix_hint) {
    XList *l = lst_of(lo);
    if (l->n > 1) { type_error(vm, 1089, NULL, NULL); return; }
    if (l->n == 0) {
        V2 r = list_resolve(vm, lo);
        if (r.t != T_OBJ) return;
        if (is_list(r)) { if (LST(r)->n != 1) return; list_add(l, LST(r)->items[0]); }
        else list_add(l, NOD(r));
    }
    xml_put(vm, l->items[0], q, v, prefix_hint);
}
static void list_put_index(VM2 *vm, A2Obj *lo, u32 idx, V2 v) {
    XList *l = lst_of(lo); int i = (int)idx; XNode *r = NULL;
    if (i < 0) return;
    if (l->target) {
        V2 rv = is_list(v2_obj(l->target)) ? list_resolve(vm, l->target) : v2_obj(l->target);
        if (rv.t == T_OBJ) { if (is_list(rv)) { if (LST(rv)->n == 1) r = LST(rv)->items[0]; else if (LST(rv)->n > 1) return; } else r = NOD(rv); }
    }
    if (i >= l->n) {
        XNode *y;
        if (!r && l->target) return;
        if (r && r->kind != XN_ELEMENT) return;
        if (l->has_tprop && l->tprop.attr) {
            if (!r) return;
            { int k; for (k = 0; k < r->nattrs; k++) if (xq_matches(&l->tprop, r->attrs[k])) return; }
            y = xn_new(vm, XN_ATTR); y->local = l->tprop.local; y->uri = l->tprop.uri ? l->tprop.uri : vm->s_empty;
        } else if (!l->has_tprop || is_star(l->tprop.local)) {
            if (l->n > 0 && l->items[l->n - 1]->kind == XN_ELEMENT) { XNode *last = l->items[l->n - 1]; y = xn_new(vm, XN_ELEMENT); y->local = last->local; y->uri = last->uri; y->prefix = last->prefix; }
            else y = xn_new(vm, XN_TEXT);
        }
        else { y = xn_new(vm, XN_ELEMENT); y->local = l->tprop.local; y->uri = l->tprop.uri ? l->tprop.uri : (l->tprop.dxns ? l->tprop.dxns : default_ns(vm)); }
        i = l->n;
        if (y->kind != XN_ATTR && r) {
            int j = r->nkids;
            if (i > 0) { int p = xn_kid_index(r, l->items[i - 1]); if (p >= 0) j = p + 1; }
            if (is_node(v) && NOD(v)->kind == XN_ELEMENT && !is_star(l->tprop.local)) { /* the element itself is inserted below */ }
            xn_insert_kid(r, j, y);
        } else if (y->kind == XN_ATTR && r) { xn_add_attr(r, y); }
        list_add(l, y);
    }
    if (!is_xmlobj(v) || (is_node(v) && (NOD(v)->kind == XN_ATTR)) || is_textish(v)) { if (is_node(v)) v = v2_str(NOD(v)->value); else if (is_list(v)) v = v2_str(LST(v)->items[0]->value); else v = v2_str(v2_tostr(vm, v)); }
    {
        XNode *xi = l->items[i]; XNode *parent = xi->parent;
        if (xi->kind == XN_ATTR) {
            XQ aq; memset(&aq, 0, sizeof(aq)); aq.attr = 1; aq.local = xi->local; aq.uri = xi->uri;
            if (parent) { xml_put(vm, parent, &aq, v, xi->prefix); { V2 g = xml_get(vm, parent, &aq); if (LST(g)->n) l->items[i] = LST(g)->items[0]; } }
            else xi->value = value_to_string(vm, v);
            return;
        }
        if (is_list(v)) {
            XList *c = LST(v); int k;
            if (parent) {
                int q = xn_kid_index(parent, xi);
                xn_remove_kid_at(parent, q);
                for (k = 0; k < c->n; k++) { XNode *cn = c->items[k]; if (cn->parent) cn = copy_with_scope(vm, cn); xn_insert_kid(parent, q + k, cn); c->items[k] = cn; }
            }
            list_remove_at(l, i);
            for (k = 0; k < c->n; k++) list_insert(l, i + k, c->items[k]);
            return;
        }
        if (is_node(v) || xi->kind == XN_TEXT || xi->kind == XN_COMMENT || xi->kind == XN_PI) {
            XNode *nn;
            if (is_node(v)) { nn = NOD(v); if (nn->parent && parent && nn->parent != parent) nn = copy_with_scope(vm, nn); else if (nn->parent == parent && parent) { /* moving within the same parent */ } }
            else nn = text_node(vm, v2_tostr(vm, v));
            if (parent) { int q = xn_kid_index(parent, xi); if (nn->parent == parent) { int pq = xn_kid_index(parent, nn); if (pq != q) { xn_remove_kid_at(parent, q); if (pq > q) pq--; xn_remove_kid_at(parent, pq); xn_insert_kid(parent, q, nn); } } else { xn_remove_kid_at(parent, q); xn_insert_kid(parent, q, nn); } }
            l->items[i] = nn;
            return;
        }
        { XQ star; memset(&star, 0, sizeof(star)); star.attr = 0; star.local = a2_str(vm, "*"); star.uri = NULL; xml_put(vm, xi, &star, v, NULL); }
    }
}

/* [[Delete]] on XMLList */
static int list_delete(VM2 *vm, A2Obj *lo, const XQ *q) { XList *l = lst_of(lo); int i; (void)vm; for (i = 0; i < l->n; i++) if (l->items[i]->kind == XN_ELEMENT) xml_delete(vm, l->items[i], q); return 1; }
static int list_delete_index(VM2 *vm, A2Obj *lo, u32 idx) { XList *l = lst_of(lo); if (idx < (u32)l->n) { XNode *k = l->items[idx], *p = k->parent; xn_detach(k); list_remove_at(l, (int)idx); if (p) notify(vm, p, k->kind == XN_ATTR ? "attributeRemoved" : "nodeRemoved", k->kind == XN_ATTR ? v2_str(k->local) : wrap(k), k->kind == XN_ATTR && k->value ? v2_str(k->value) : v2_null()); } return 1; }

/* ---------------------------------------------------------------- */
/* interpreter hooks                                                 */

static A2Str *prefix_hint_of(const A2Name *n) { (void)n; return NULL; }

V2 a2_xml_get(VM2 *vm, V2 obj, A2Name *n) {
    A2Obj *o = obj.u.o; u32 idx; XQ q;
    if (!o->payload) return v2_undef();
    if (a2_is_index(n->name, &idx) && !(n->name->len && n->name->s[0] == '@')) {
        XList *l = lst_of(o);
        if (l) return idx < (u32)l->n ? wrap(l->items[idx]) : v2_undef();
        return idx == 0 ? obj : v2_undef();
    }
    xq_from_name(vm, n, &q);
    {
        V2 r = lst_of(o) ? list_get(vm, o, &q) : xml_get(vm, node_of(o), &q);
        /* nothing found: fall back to class traits / prototype (methods used as values) */
        if (LST(r)->n == 0 && !q.attr) {
            A2Trait *t = a2_find_trait(&o->cls->itraits, n->name, n);
            if (t) { V2 v; a2_get(vm, obj, n, &v); return v; }
            { A2Prop *pp = a2_dyn_find(o->cls->prototype, n->name); if (pp) return pp->v; }
            if (o->cls->prototype->proto) { A2Prop *pp = a2_dyn_find(o->cls->prototype->proto, n->name); if (pp) return pp->v; }
        }
        return r;
    }
}
void a2_xml_set(VM2 *vm, V2 obj, A2Name *n, V2 v) {
    A2Obj *o = obj.u.o; u32 idx; XQ q;
    if (!o->payload) return;
    if (a2_is_index(n->name, &idx) && !(n->name->len && n->name->s[0] == '@')) {
        if (lst_of(o)) list_put_index(vm, o, idx, v);
        else type_error(vm, 1087, NULL, NULL);
        return;
    }
    xq_from_name(vm, n, &q);
    if (lst_of(o)) list_put(vm, o, &q, v, prefix_hint_of(n)); else xml_put(vm, node_of(o), &q, v, prefix_hint_of(n));
}
int a2_xml_delete(VM2 *vm, V2 obj, A2Name *n) {
    A2Obj *o = obj.u.o; u32 idx; XQ q;
    if (!o->payload) return 0;
    if (a2_is_index(n->name, &idx) && !(n->name->len && n->name->s[0] == '@')) {
        if (lst_of(o)) return list_delete_index(vm, o, idx);
        return 1;
    }
    xq_from_name(vm, n, &q);
    if (lst_of(o)) return list_delete(vm, o, &q);
    return xml_delete(vm, node_of(o), &q);
}
int a2_xml_has(VM2 *vm, V2 obj, const A2Name *n) {
    A2Obj *o = obj.u.o; u32 idx; XQ q; V2 r;
    if (!o->payload) return 0;
    if (a2_is_index(n->name, &idx) && !(n->name->len && n->name->s[0] == '@')) { XList *l = lst_of(o); return l ? idx < (u32)l->n : idx == 0; }
    xq_from_name(vm, n, &q);
    r = lst_of(o) ? list_get(vm, o, &q) : xml_get(vm, node_of(o), &q);
    if (LST(r)->n > 0) return 1;
    if (q.attr) return 0;
    if (a2_find_trait(&o->cls->itraits, n->name, n)) return 1;
    return a2_dyn_find(o->cls->prototype, n->name) != NULL || (o->cls->prototype->proto && a2_dyn_find(o->cls->prototype->proto, n->name) != NULL);
}
/* document-order descendants (attributes of an element come before its children) */
static void collect_descendants(XNode *x, const XQ *q, XList *l, int depth) {
    int i;
    if (depth > 2000) return;
    if (q->attr) { for (i = 0; i < x->nattrs; i++) if (xq_matches(q, x->attrs[i])) list_add(l, x->attrs[i]); }
    for (i = 0; i < x->nkids; i++) {
        XNode *k = x->kids[i];
        if (!q->attr && xq_matches(q, k)) list_add(l, k);
        if (k->kind == XN_ELEMENT) collect_descendants(k, q, l, depth + 1);
    }
}
V2 a2_xml_descendants(VM2 *vm, V2 obj, A2Name *n) {
    XQ q; A2Obj *r; XList *l;
    if (!is_xmlobj(obj)) { type_error(vm, 1016, a2_class_name(vm, v2_class_of(vm, obj), 1)->s, NULL); return v2_undef(); }
    xq_from_name(vm, n, &q);
    r = new_list(vm); l = LST(v2_obj(r));
    {
        XList *src = lst_of(obj.u.o); int i;
        if (src) { for (i = 0; i < src->n; i++) if (src->items[i]->kind == XN_ELEMENT) collect_descendants(src->items[i], &q, l, 0); }
        else if (node_of(obj.u.o)->kind == XN_ELEMENT) collect_descendants(node_of(obj.u.o), &q, l, 0);
    }
    return v2_obj(r);
}
int a2_xml_hasnext(VM2 *vm, A2Obj *o, int idx) { XList *l = lst_of(o); (void)vm; if (!o->payload) return 0; if (l) return idx < l->n ? idx + 1 : 0; return idx < 1 ? 1 : 0; }
V2 a2_xml_nextname(VM2 *vm, A2Obj *o, int idx) { char b[16]; XList *l = lst_of(o); if (idx <= 0 || (l ? idx > l->n : idx > 1)) return v2_null(); sprintf(b, "%d", idx - 1); return v2_cstr(vm, b); }
V2 a2_xml_nextvalue(VM2 *vm, A2Obj *o, int idx) { XList *l = lst_of(o); (void)vm; if (!o->payload) return v2_undef(); if (l) return idx - 1 < l->n ? wrap(l->items[idx - 1]) : v2_undef(); return v2_obj(o); }
V2 a2_xml_add(VM2 *vm, V2 a, V2 b) {
    A2Obj *r = new_list(vm); XList *l = LST(v2_obj(r)); int i;
    if (is_list(a)) { XList *p = LST(a); for (i = 0; i < p->n; i++) list_add(l, p->items[i]); l->target = p->target; l->has_tprop = p->has_tprop; l->tprop = p->tprop; } else if (is_node(a)) list_add(l, NOD(a));
    if (is_list(b)) { XList *p = LST(b); for (i = 0; i < p->n; i++) list_add(l, p->items[i]); } else if (is_node(b)) list_add(l, NOD(b));
    return v2_obj(r);
}
V2 a2_xml_escape(VM2 *vm, V2 v, int attr) {
    XB b; A2Str *s;
    /* esc_xelem: XML values insert their XML string (ToXMLString) */
    if (is_xmlobj(v) && !attr) return v2_str(is_list(v) ? list_to_string(vm, LST(v), 1) : node_to_xmlstring(vm, NOD(v)));
    s = value_to_string(vm, v); xb_init(&b);
    if (attr) esc_attr(&b, s->s, s->len); else esc_elem(&b, s->s, s->len);
    return v2_str(xb_done(vm, &b));
}
V2 a2_xml_from_string(VM2 *vm, A2Str *s) { XNode *n = parse_xml(vm, s); if (!n) { vm->throwing = 0; n = text_node(vm, vm->s_empty); } return wrap(n); }

/* abstract equality (11.5.1) */
static int node_equals(VM2 *vm, XNode *a, XNode *b);
static int node_equals(VM2 *vm, XNode *a, XNode *b) {
    int i;
    if ((a->kind == XN_TEXT || a->kind == XN_ATTR) && has_simple_content(b)) return node_to_string(vm, a) == node_to_string(vm, b);
    if ((b->kind == XN_TEXT || b->kind == XN_ATTR) && has_simple_content(a)) return node_to_string(vm, a) == node_to_string(vm, b);
    if (a->kind != b->kind) return 0;
    if (a->local != b->local || a->uri != b->uri) return 0;
    if (a->kind != XN_ELEMENT) return a->value == b->value;
    if (a->nattrs != b->nattrs || a->nkids != b->nkids) return 0;
    for (i = 0; i < a->nattrs; i++) { int j, f = 0; for (j = 0; j < b->nattrs; j++) if (b->attrs[j]->local == a->attrs[i]->local && b->attrs[j]->uri == a->attrs[i]->uri && b->attrs[j]->value == a->attrs[i]->value) { f = 1; break; } if (!f) return 0; }
    for (i = 0; i < a->nkids; i++) if (!node_equals(vm, a->kids[i], b->kids[i])) return 0;
    return 1;
}
int a2_xml_equals(VM2 *vm, V2 a, V2 b) {
    if (is_list(a)) {
        XList *l = LST(a);
        if (b.t == T_UNDEF) return l->n == 0;
        if (is_list(b)) { XList *m = LST(b); int i; if (l->n != m->n) return 0; for (i = 0; i < l->n; i++) if (!a2_xml_equals(vm, wrap(l->items[i]), wrap(m->items[i]))) return 0; return 1; }
        if (l->n == 1) return a2_xml_equals(vm, wrap(l->items[0]), b);
        return 0;
    }
    if (is_list(b)) return a2_xml_equals(vm, b, a);
    if (is_node(a) && is_node(b)) return node_equals(vm, NOD(a), NOD(b));
    if (is_node(a)) { XNode *n = NOD(a); if (b.t == T_UNDEF || b.t == T_NULL) return 0; if (has_simple_content(n)) return node_to_string(vm, n) == v2_tostr(vm, b); return 0; }
    if (is_node(b)) return a2_xml_equals(vm, b, a);
    return 0;
}

/* ---------------------------------------------------------------- */
/* constructors                                                     */

static V2 XML_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = self.t == T_OBJ ? self.u.o : NULL; V2 a = ARG(0); XNode *n;
    if (!o) return v2_undef();
    if (a.t == T_UNDEF || a.t == T_NULL) n = text_node(vm, vm->s_empty);
    else if (is_node(a)) n = deep_copy_filtered(vm, NOD(a));
    else if (is_list(a)) { XList *l = LST(a); if (l->n == 1) n = deep_copy_filtered(vm, l->items[0]); else { type_error(vm, 1088, NULL, NULL); return v2_undef(); } }
    else { n = parse_xml(vm, v2_tostr(vm, a)); if (!n) return v2_undef(); }
    adopt(o, n);
    return v2_undef();
}
static V2 XML_call(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; if (argc && is_node(argv[0])) return argv[0]; if (argc == 1 && is_list(argv[0])) { XList *l = LST(argv[0]); if (l->n == 1) return wrap(l->items[0]); type_error(vm, 1088, NULL, NULL); return v2_undef(); } if (argc && argv[0].t == T_NULL) { V2 z = v2_undef(); return a2_construct_class(vm, vm->c_xml, 1, &z); } return a2_construct_class(vm, vm->c_xml, argc, argv); }
static V2 XMLList_ctor(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Obj *o = self.t == T_OBJ ? self.u.o : NULL; XList *p; V2 a = ARG(0);
    if (!o) return v2_undef();
    if (o->payload) a2_xml_free_payload(o);
    o->kind = OK_XML; p = (XList*)calloc(1, sizeof(XList)); p->is_list = 1; o->payload = p;
    if (a.t == T_UNDEF || a.t == T_NULL) return v2_undef();
    if (is_list(a)) { XList *s = LST(a); int i; for (i = 0; i < s->n; i++) list_add(p, s->items[i]); p->target = s->target; p->has_tprop = s->has_tprop; p->tprop = s->tprop; return v2_undef(); }
    if (is_node(a)) { list_add(p, NOD(a)); return v2_undef(); }
    { XNode *root = parse_fragment(vm, v2_tostr(vm, a)); int i; if (!root) return v2_undef();
      for (i = 0; i < root->nkids; i++) list_add(p, root->kids[i]);
      for (i = 0; i < root->nkids; i++) root->kids[i]->parent = NULL; root->nkids = 0; }
    return v2_undef();
}
static V2 XMLList_call(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; if (argc && is_list(argv[0])) return argv[0]; return a2_construct_class(vm, vm->c_xmllist, argc, argv); }

/* ---------------------------------------------------------------- */
/* methods                                                          */

static XNode *self_node(VM2 *vm, V2 self, const char *method) {
    if (is_node(self)) return NOD(self);
    if (is_list(self)) { XList *l = LST(self); if (l->n == 1) return l->items[0]; type_error(vm, 1086, method, NULL); return NULL; }
    return NULL;
}
static V2 list_from(VM2 *vm, XNode *target, int has_tprop, XQ *q) { A2Obj *r = new_list(vm); XList *l = LST(v2_obj(r)); l->target = target ? target->obj : NULL; l->has_tprop = has_tprop; if (has_tprop) l->tprop = *q; return v2_obj(r); }
/* iterate "self" as a sequence of nodes */
static int self_items(V2 self, XNode ***items, XNode **one) { if (is_list(self)) { *items = LST(self)->items; return LST(self)->n; } if (is_node(self)) { *one = NOD(self); *items = one; return 1; } return 0; }

static V2 x_toString(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; if (!is_xmlobj(self)) return v2_str(vm->s_empty); return v2_str(a2_xml_tostring(vm, self.u.o)); }
static V2 x_toXMLString(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; if (!is_xmlobj(self)) return v2_str(vm->s_empty); if (is_list(self)) return v2_str(list_to_string(vm, LST(self), 1)); return v2_str(node_to_xmlstring(vm, NOD(self))); }
static V2 x_valueOf(VM2 *vm, V2 self, int argc, V2 *argv) { (void)vm; (void)argc; (void)argv; return self; }
static V2 x_length(VM2 *vm, V2 self, int argc, V2 *argv) { (void)vm; (void)argc; (void)argv; if (is_list(self)) return v2_int(LST(self)->n); return v2_int(1); }
static V2 x_name(VM2 *vm, V2 self, int argc, V2 *argv) { XNode *n = self_node(vm, self, "name"); (void)argc; (void)argv; if (!n || !n->local) return v2_null(); if (n->kind == XN_PI) return make_qname(vm, vm->s_empty, n->local); if (n->kind == XN_ATTR) return make_attr_qname(vm, n->uri, n->local); return make_qname(vm, n->uri, n->local); }
static V2 x_localName(VM2 *vm, V2 self, int argc, V2 *argv) { XNode *n = self_node(vm, self, "localName"); (void)argc; (void)argv; if (!n || !n->local) return v2_null(); return v2_str(n->local); }
static V2 x_nodeKind(VM2 *vm, V2 self, int argc, V2 *argv) { XNode *n = self_node(vm, self, "nodeKind"); (void)argc; (void)argv; if (!n) return v2_undef(); switch (n->kind) { case XN_ELEMENT: return v2_cstr(vm, "element"); case XN_ATTR: return v2_cstr(vm, "attribute"); case XN_COMMENT: return v2_cstr(vm, "comment"); case XN_PI: return v2_cstr(vm, "processing-instruction"); default: return v2_cstr(vm, "text"); } }
static V2 kids_of_kind(VM2 *vm, V2 self, int kind) {
    XNode **items, *one; int n = self_items(self, &items, &one), i, k; XQ q; V2 r; XList *l;
    memset(&q, 0, sizeof(q)); q.attr = 0; q.local = a2_str(vm, "*"); q.uri = NULL;
    r = list_from(vm, n == 1 && !is_list(self) ? items[0] : NULL, 1, &q); l = LST(r);
    if (is_list(self)) l->target = self.u.o;
    for (i = 0; i < n; i++) { XNode *e = items[i]; if (e->kind != XN_ELEMENT) continue; for (k = 0; k < e->nkids; k++) if (kind < 0 || e->kids[k]->kind == kind) list_add(l, e->kids[k]); }
    return r;
}
static V2 x_children(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return kids_of_kind(vm, self, -1); }
static V2 x_text(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return kids_of_kind(vm, self, XN_TEXT); }
static V2 x_comments(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; return kids_of_kind(vm, self, XN_COMMENT); }
static V2 x_processingInstructions(VM2 *vm, V2 self, int argc, V2 *argv) {
    V2 r = kids_of_kind(vm, self, XN_PI); XList *l = LST(r); int i;
    if (argc && argv[0].t != T_UNDEF && !is_star(v2_tostr(vm, argv[0]))) { A2Str *nm = v2_tostr(vm, argv[0]); for (i = l->n - 1; i >= 0; i--) if (l->items[i]->local != nm) list_remove_at(l, i); }
    return r;
}
static V2 x_elements(VM2 *vm, V2 self, int argc, V2 *argv) {
    XQ q; XNode **items, *one; int n = self_items(self, &items, &one), i, k; V2 r; XList *l;
    if (argc && argv[0].t != T_UNDEF) xq_from_value(vm, argv[0], &q, 0); else { memset(&q, 0, sizeof(q)); q.attr = 0; q.local = a2_str(vm, "*"); q.uri = NULL; }
    q.attr = 0;
    r = list_from(vm, NULL, 1, &q); l = LST(r); l->target = self.u.o;
    for (i = 0; i < n; i++) { XNode *e = items[i]; if (e->kind != XN_ELEMENT) continue; for (k = 0; k < e->nkids; k++) if (e->kids[k]->kind == XN_ELEMENT && xq_matches(&q, e->kids[k])) list_add(l, e->kids[k]); }
    return r;
}
static V2 x_attributes(VM2 *vm, V2 self, int argc, V2 *argv) { XQ q; (void)argc; (void)argv; memset(&q, 0, sizeof(q)); q.attr = 1; q.local = a2_str(vm, "*"); q.uri = NULL; return is_list(self) ? list_get(vm, self.u.o, &q) : xml_get(vm, NOD(self), &q); }
/* ToXMLName(null / undefined) throws */
static int xname_arg_bad(VM2 *vm, int argc, V2 *argv) {
    if (argc && (argv[0].t == T_NULL || argv[0].t == T_UNDEF)) { a2_throw_id(vm, "TypeError", argv[0].t == T_NULL ? 1009 : 1010, NULL, NULL); return 1; }
    return 0;
}
static V2 x_attribute(VM2 *vm, V2 self, int argc, V2 *argv) { XQ q; if (xname_arg_bad(vm, argc, argv)) return v2_undef(); if (!argc || argv[0].t == T_UNDEF) { xq_from_value(vm, v2_str(vm->s_undefined), &q, 1); } else xq_from_value(vm, argv[0], &q, 1); q.attr = 1; return is_list(self) ? list_get(vm, self.u.o, &q) : xml_get(vm, NOD(self), &q); }
static V2 x_child(VM2 *vm, V2 self, int argc, V2 *argv) {
    XQ q; u32 idx; A2Str *s;
    if (xname_arg_bad(vm, argc, argv)) return v2_undef();
    s = STR(0);
    if (!is_qname(vm, ARG(0)) && a2_is_index(s, &idx)) {
        XNode **items, *one; int n = self_items(self, &items, &one), i; V2 r = list_from(vm, NULL, 0, NULL); XList *l = LST(r); l->target = self.u.o;
        for (i = 0; i < n; i++) { XNode *e = items[i]; if (e->kind == XN_ELEMENT && idx < (u32)e->nkids) list_add(l, e->kids[idx]); }
        return r;
    }
    xq_from_value(vm, ARG(0), &q, 0);
    return is_list(self) ? list_get(vm, self.u.o, &q) : xml_get(vm, NOD(self), &q);
}
static V2 x_descendants(VM2 *vm, V2 self, int argc, V2 *argv) { A2Name n; XQ q; A2Ns ns; if (argc && argv[0].t != T_UNDEF) xq_from_value(vm, argv[0], &q, 0); else { memset(&q, 0, sizeof(q)); q.attr = 0; q.local = a2_str(vm, "*"); q.uri = NULL; }
    n.name = q.attr ? a2_concat(vm, a2_str(vm, "@"), q.local) : q.local; n.attr = q.attr; if (q.uri) { ns.kind = NS_NAMESPACE; ns.uri = q.uri; n.nss = &ns; n.nns = 1; n.any_ns = 0; } else { n.nss = &vm->ns_public; n.nns = 1; n.any_ns = 1; }
    return a2_xml_descendants(vm, self, &n); }
static V2 x_parent(VM2 *vm, V2 self, int argc, V2 *argv) { XNode *n; (void)argc; (void)argv; if (is_list(self)) { XList *p = LST(self); int i; if (!p->n) return v2_undef(); for (i = 1; i < p->n; i++) if (p->items[i]->parent != p->items[0]->parent) return v2_undef(); n = p->items[0]; } else n = NOD(self); (void)vm; if (!n || !n->parent) return v2_undef(); return wrap(n->parent); }
static void append_moving(VM2 *vm, XNode *n, XNode *c) {
    if (c->kind == XN_ATTR || c->kind == XN_TEXT) { xn_add_kid(n, text_node(vm, c->value)); return; }   /* text is copied */
    if (xn_is_ancestor(c, n)) { type_error(vm, 1118, NULL, NULL); return; }
    if (c->parent) {
        /* SWF 21+: the node leaves its old parent first; older movies leave it where it was too */
        extern struct SWFMovie *a2_code_movie(VM2 *vm); struct SWFMovie *cm = a2_code_movie(vm);
        if ((cm ? cm->version : vm->swf_version) >= 21) xn_detach(c);
    }
    xn_add_kid(n, c);
    notify(vm, n, "nodeAdded", wrap(c), v2_null());
}
/* SWF 9 movies (bug 558863 compatibility): appendChild/prependChild/replace parse a string argument as XML */
static V2 swf9_child(VM2 *vm, V2 v) {
    extern struct SWFMovie *a2_code_movie(VM2 *vm); struct SWFMovie *cm;
    if (is_xmlobj(v) || v.t == T_UNDEF || v.t == T_NULL) return v;
    cm = a2_code_movie(vm);
    if ((cm ? cm->version : vm->swf_version) > 9) return v;
    { V2 r; xp_lax = 1; r = a2_construct_class(vm, vm->c_xml, 1, &v); xp_lax = 0; return vm->throwing ? v : r; }
}
static V2 x_appendChild(VM2 *vm, V2 self, int argc, V2 *argv) {
    XNode *n = self_node(vm, self, "appendChild"); V2 v = swf9_child(vm, ARG(0));
    if (!n) return v2_undef();
    if (n->kind != XN_ELEMENT) return self;
    if (is_list(v)) { XList *l = LST(v); int k; for (k = 0; k < l->n; k++) append_moving(vm, n, l->items[k]); return self; }
    if (is_node(v)) { append_moving(vm, n, NOD(v)); return self; }
    /* children[length] = value: through the XMLList put so the new element takes the last child's name */
    { XQ star; V2 kids; memset(&star, 0, sizeof(star)); star.attr = 0; star.local = a2_str(vm, "*"); star.uri = NULL; kids = xml_get(vm, n, &star); list_put_index(vm, kids.u.o, (u32)LST(kids)->n, v2_str(v2_tostr(vm, v))); }
    return self;
}
static V2 x_prependChild(VM2 *vm, V2 self, int argc, V2 *argv) { XNode *n = self_node(vm, self, "prependChild"); V2 v; if (!n) return v2_undef(); v = swf9_child(vm, ARG(0)); if (n->kind == XN_ELEMENT) xml_insert(vm, n, 0, is_xmlobj(v) ? v : v2_str(v2_tostr(vm, v))); return self; }
static V2 x_insertChildAfter(VM2 *vm, V2 self, int argc, V2 *argv) { XNode *n = self_node(vm, self, "insertChildAfter"), *ref; int i; if (!n) return v2_undef(); if (n->kind != XN_ELEMENT) return v2_undef(); if (ARG(0).t == T_NULL || ARG(0).t == T_UNDEF) { xml_insert(vm, n, 0, ARG(1)); return self; } ref = is_node(ARG(0)) ? NOD(ARG(0)) : (is_list(ARG(0)) && LST(ARG(0))->n == 1 ? LST(ARG(0))->items[0] : NULL); for (i = 0; i < n->nkids; i++) if (n->kids[i] == ref) { xml_insert(vm, n, i + 1, ARG(1)); return self; } return v2_undef(); }
static V2 x_insertChildBefore(VM2 *vm, V2 self, int argc, V2 *argv) { XNode *n = self_node(vm, self, "insertChildBefore"), *ref; int i; if (!n) return v2_undef(); if (n->kind != XN_ELEMENT) return v2_undef(); if (ARG(0).t == T_NULL || ARG(0).t == T_UNDEF) { xml_insert(vm, n, n->nkids, ARG(1)); return self; } ref = is_node(ARG(0)) ? NOD(ARG(0)) : (is_list(ARG(0)) && LST(ARG(0))->n == 1 ? LST(ARG(0))->items[0] : NULL); for (i = 0; i < n->nkids; i++) if (n->kids[i] == ref) { xml_insert(vm, n, i, ARG(1)); return self; } return v2_undef(); }
static V2 x_copy(VM2 *vm, V2 self, int argc, V2 *argv) { (void)argc; (void)argv; if (is_list(self)) { A2Obj *r = new_list(vm); XList *p = LST(self); int i; for (i = 0; i < p->n; i++) list_add(LST(v2_obj(r)), deep_copy(vm, p->items[i])); return v2_obj(r); } if (!is_node(self)) return v2_undef(); return wrap(copy_with_scope(vm, NOD(self))); }
static V2 x_hasSimpleContent(VM2 *vm, V2 self, int argc, V2 *argv) { (void)vm; (void)argc; (void)argv; if (is_list(self)) { XList *p = LST(self); int i; if (p->n == 0) return v2_bool(1); if (p->n == 1) return v2_bool(has_simple_content(p->items[0])); for (i = 0; i < p->n; i++) if (p->items[i]->kind == XN_ELEMENT) return v2_bool(0); return v2_bool(1); } return v2_bool(is_node(self) ? has_simple_content(NOD(self)) : 1); }
static V2 x_hasComplexContent(VM2 *vm, V2 self, int argc, V2 *argv) { (void)vm; (void)argc; (void)argv; if (is_list(self)) { XList *p = LST(self); int i; if (p->n == 0) return v2_bool(0); if (p->n == 1) return v2_bool(p->items[0]->kind == XN_ELEMENT && !has_simple_content(p->items[0])); for (i = 0; i < p->n; i++) if (p->items[i]->kind == XN_ELEMENT) return v2_bool(1); return v2_bool(0); } return v2_bool(is_node(self) && NOD(self)->kind == XN_ELEMENT && !has_simple_content(NOD(self))); }
static V2 x_hasOwnProperty(VM2 *vm, V2 self, int argc, V2 *argv) {
    A2Name n; A2Str *s = STR(0); u32 idx;
    if (!is_xmlobj(self)) return v2_bool(0);
    if (is_qname(vm, ARG(0))) { XQ q; V2 r; xq_from_value(vm, ARG(0), &q, 0); r = is_list(self) ? list_get(vm, self.u.o, &q) : xml_get(vm, NOD(self), &q); return v2_bool(LST(r)->n > 0); }
    if (a2_is_index(s, &idx)) { XList *l = lst_of(self.u.o); return v2_bool(l ? idx < (u32)l->n : idx == 0); }
    n.name = s; n.nss = &vm->ns_public; n.nns = 1; n.any_ns = 0; n.attr = 0;
    { XQ q; V2 r; xq_from_name(vm, &n, &q); r = is_list(self) ? list_get(vm, self.u.o, &q) : xml_get(vm, NOD(self), &q); if (LST(r)->n > 0) return v2_bool(1); }
    /* methods are own properties of the prototype, not the object */
    return v2_bool(0);
}
static V2 x_propertyIsEnumerable(VM2 *vm, V2 self, int argc, V2 *argv) { A2Str *s = STR(0); u32 idx; if (a2_is_index(s, &idx)) { XList *l = is_xmlobj(self) ? lst_of(self.u.o) : NULL; return v2_bool(l ? idx < (u32)l->n : idx == 0); } return v2_bool(0); }
static V2 x_contains(VM2 *vm, V2 self, int argc, V2 *argv) { V2 v = ARG(0); if (is_list(self)) { XList *p = LST(self); int i; for (i = 0; i < p->n; i++) if (a2_xml_equals(vm, wrap(p->items[i]), v)) return v2_bool(1); return v2_bool(0); } if (!is_node(v)) return v2_bool(0); return v2_bool(a2_xml_equals(vm, self, v)); }
static V2 x_setName_impl(VM2 *vm, V2 self, int argc, V2 *argv) {
    XNode *n = self_node(vm, self, "setName"); V2 a = ARG(0);
    if (!n) return v2_undef();
    if (a.t == T_UNDEF) { type_error(vm, 1117, "undefined", NULL); return v2_undef(); }   /* QName(undefined) is not a name */
    if (n->kind == XN_TEXT || n->kind == XN_COMMENT) return v2_undef();
    if (is_qname(vm, a)) { A2Str *u = a2_qname_uri(a.u.o); A2Str *ls = a2_qname_local(vm, a.u.o); if (!valid_name(ls)) { type_error(vm, 1117, ls->s, NULL); return v2_undef(); } n->local = ls; if (n->kind == XN_PI) return v2_undef(); n->uri = u ? u : default_ns(vm); n->prefix = NULL; return v2_undef(); }
    { A2Str *s = v2_tostr(vm, a); if (!valid_name(s) || strchr(s->s, ':')) { type_error(vm, 1117, s->s, NULL); return v2_undef(); } n->local = s; if (n->kind == XN_ATTR) n->uri = vm->s_empty; else if (n->kind == XN_ELEMENT) { n->uri = default_ns(vm); n->prefix = NULL; } }
    return v2_undef();
}
static V2 x_setName(VM2 *vm, V2 self, int argc, V2 *argv) {
    XNode *n = is_node(self) ? NOD(self) : (is_list(self) && LST(self)->n == 1 ? LST(self)->items[0] : NULL);
    x_setName_impl(vm, self, argc, argv);
    if (!vm->throwing && n && n->kind != XN_TEXT && n->kind != XN_COMMENT && n->local) notify(vm, n, "nameSet", ARG(0), v2_str(n->local));
    return v2_undef();
}
static V2 x_setNotification(VM2 *vm, V2 self, int argc, V2 *argv) { XNode *n = is_node(self) ? NOD(self) : NULL; if (argc && argv[0].t != T_NULL && argv[0].t != T_UNDEF) { a2_coerce(vm, argv[0], vm->c_function); if (vm->throwing) return v2_undef(); } if (n) n->notify = ARG(0).t == T_OBJ && ARG(0).u.o->kind == OK_FUNCTION ? ARG(0) : v2_null(); return v2_undef(); }
static V2 x_notification(VM2 *vm, V2 self, int argc, V2 *argv) { XNode *n = is_node(self) ? NOD(self) : NULL; (void)vm; (void)argc; (void)argv; return n && n->notify.t == T_OBJ ? n->notify : v2_null(); }
static V2 x_setLocalName(VM2 *vm, V2 self, int argc, V2 *argv) { XNode *n = self_node(vm, self, "setLocalName"); A2Str *s; if (!n) return v2_undef(); if (n->kind == XN_TEXT || n->kind == XN_COMMENT) return v2_undef(); s = is_qname(vm, ARG(0)) ? a2_qname_local(vm, ARG(0).u.o) : STR(0); if (!valid_name(s) || strchr(s->s, ':')) { type_error(vm, 1117, s->s, NULL); return v2_undef(); } { A2Str *prev = n->local; n->local = s; if (prev) notify(vm, n, "nameSet", v2_str(s), v2_str(prev)); } return v2_undef(); }
static V2 x_setChildren(VM2 *vm, V2 self, int argc, V2 *argv) { XNode *n = self_node(vm, self, "setChildren"); XQ star; if (!n) return v2_undef(); memset(&star, 0, sizeof(star)); star.attr = 0; star.local = a2_str(vm, "*"); star.uri = NULL; xml_put(vm, n, &star, ARG(0), NULL); return self; }
static V2 x_replace(VM2 *vm, V2 self, int argc, V2 *argv) {
    XNode *n = self_node(vm, self, "replace"); u32 idx; XQ q; int i, first = -1; V2 v = ARG(1);
    if (!n) return v2_undef();
    v = swf9_child(vm, v);
    if (n->kind != XN_ELEMENT) return self;
    if (!is_qname(vm, ARG(0)) && a2_is_index(STR(0), &idx)) { if (idx < (u32)n->nkids) xml_replace(vm, n, (int)idx, is_xmlobj(v) ? v : v2_str(v2_tostr(vm, v))); return self; }
    xq_from_value(vm, ARG(0), &q, 0);
    for (i = n->nkids - 1; i >= 0; i--) if (xq_matches(&q, n->kids[i])) { if (first >= 0) xn_remove_kid_at(n, first); first = i; }
    if (first >= 0) { xml_replace(vm, n, first, is_xmlobj(v) ? v : v2_str(v2_tostr(vm, v))); }
    return self;
}
static V2 x_namespace(VM2 *vm, V2 self, int argc, V2 *argv) {
    XNode *n = self_node(vm, self, "namespace"); NsList l; V2 r = v2_undef(); int i;
    if (!n) return v2_undef();
    memset(&l, 0, sizeof(l)); in_scope_namespaces(n->kind == XN_ATTR ? n->parent : n, &l);
    if (argc && argv[0].t != T_UNDEF) {
        A2Str *pre = STR(0); XNs *f = nsl_find_prefix(&l, pre);
        if (f) r = make_ns(vm, f->prefix ? f->prefix : vm->s_empty, f->uri);
        free(l.v); return r;
    }
    if (n->kind == XN_TEXT || n->kind == XN_COMMENT || n->kind == XN_PI) { free(l.v); return v2_null(); }
    for (i = 0; i < l.n; i++) if (l.v[i].uri == n->uri && (!n->prefix || l.v[i].prefix == n->prefix)) { r = make_ns(vm, l.v[i].prefix, l.v[i].uri); free(l.v); return r; }
    for (i = 0; i < l.n; i++) if (l.v[i].uri == n->uri) { r = make_ns(vm, l.v[i].prefix, l.v[i].uri); free(l.v); return r; }
    free(l.v);
    return make_ns(vm, NULL, n->uri);   /* not in scope: new Namespace(uri), without prefix */
}
static V2 x_inScopeNamespaces(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *a = a2_new_array(vm); XNode *n = self_node(vm, self, "inScopeNamespaces"); NsList l; int i; (void)argc; (void)argv; if (!n) return v2_obj(a); memset(&l, 0, sizeof(l)); in_scope_namespaces(n->kind == XN_ATTR ? n->parent : n, &l); for (i = 0; i < l.n; i++) a2_arr_push(vm, a, make_ns(vm, l.v[i].prefix ? l.v[i].prefix : vm->s_empty, l.v[i].uri)); if (l.n == 0) a2_arr_push(vm, a, make_ns(vm, vm->s_empty, vm->s_empty)); free(l.v); return v2_obj(a); }
static V2 x_namespaceDeclarations(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *a = a2_new_array(vm); XNode *n = self_node(vm, self, "namespaceDeclarations"); int i; (void)argc; (void)argv; if (!n || n->kind != XN_ELEMENT) return v2_obj(a); for (i = 0; i < n->nns; i++) { A2Str *pu = n->parent ? lookup_prefix_uri(n->parent, n->ns[i].prefix) : NULL; if (pu == n->ns[i].uri && n->ns[i].uri->len == 0) continue; a2_arr_push(vm, a, make_ns(vm, n->ns[i].prefix ? n->ns[i].prefix : vm->s_empty, n->ns[i].uri)); } return v2_obj(a); }
static V2 x_addNamespace(VM2 *vm, V2 self, int argc, V2 *argv) {
    XNode *n; V2 a = ARG(0); A2Str *uri, *prefix = NULL; int i;
    if (argc < 1) { char b[200]; sprintf(b, "Argument count mismatch on %s/addNamespace(). Expected 1, got 0.", is_list(self) ? "XMLList" : "XML"); a2_throw_error(vm, "ArgumentError", b, 1063); return v2_undef(); }
    n = self_node(vm, self, "addNamespace");
    if (!n) return v2_undef();
    if (n->kind != XN_ELEMENT) return self;
    if (is_nsobj(a)) { uri = a.u.o->nsval.uri; prefix = ns_prefix_of(vm, a.u.o); } else uri = v2_tostr(vm, a);
    if (!prefix) return self;   /* a namespace without prefix is not added */
    for (i = 0; i < n->nns; i++) if (n->ns[i].prefix == prefix) { n->ns[i].uri = uri; return self; }
    xn_add_ns(n, prefix, uri);
    /* a conflicting prefix on the element / attributes loses its prefix */
    if (n->prefix == prefix && n->uri != uri) n->prefix = NULL;
    for (i = 0; i < n->nattrs; i++) if (n->attrs[i]->prefix == prefix && n->attrs[i]->uri != uri) n->attrs[i]->prefix = NULL;
    return self;
}
static V2 x_setNamespace(VM2 *vm, V2 self, int argc, V2 *argv) {
    XNode *n = self_node(vm, self, "setNamespace"); V2 a = ARG(0); A2Str *uri, *prefix = NULL;
    if (!n) return v2_undef();
    if (n->kind == XN_TEXT || n->kind == XN_COMMENT || n->kind == XN_PI) return v2_undef();
    if (is_nsobj(a)) { uri = a.u.o->nsval.uri; prefix = ns_prefix_of(vm, a.u.o); } else uri = v2_tostr(vm, a);
    n->uri = uri; n->prefix = prefix;
    if (n->kind == XN_ELEMENT) { if (prefix) { int i, f = 0; for (i = 0; i < n->nns; i++) if (n->ns[i].prefix == prefix) { n->ns[i].uri = uri; f = 1; } if (!f) xn_add_ns(n, prefix, uri); } else if (uri->len) { int i, f = 0; for (i = 0; i < n->nns; i++) if (n->ns[i].uri == uri) f = 1; if (!f && !lookup_prefix_uri(n, vm->s_empty)) { /* leave undeclared: serialization generates */ } } }
    else if (n->kind == XN_ATTR && n->parent && prefix) { int i, f = 0; XNode *p = n->parent; for (i = 0; i < p->nns; i++) if (p->ns[i].prefix == prefix) { f = 1; } if (!f) xn_add_ns(p, prefix, uri); }
    notify(vm, n, "namespaceSet", make_ns(vm, prefix, uri), v2_null());
    return v2_undef();
}
static V2 x_removeNamespace(VM2 *vm, V2 self, int argc, V2 *argv) {
    XNode *n = self_node(vm, self, "removeNamespace"); V2 a = ARG(0); A2Str *uri, *prefix = NULL; int i;
    if (!n) return v2_undef();
    if (n->kind != XN_ELEMENT) return self;
    if (is_nsobj(a)) { uri = a.u.o->nsval.uri; prefix = ns_prefix_of(vm, a.u.o); } else uri = v2_tostr(vm, a);
    /* cannot remove a namespace used by the element or its attributes */
    if (n->uri == uri && (!prefix || n->prefix == prefix)) return self;
    for (i = 0; i < n->nattrs; i++) if (n->attrs[i]->uri == uri && (!prefix || n->attrs[i]->prefix == prefix)) return self;
    for (i = n->nns - 1; i >= 0; i--) if (n->ns[i].uri == uri && (!prefix || n->ns[i].prefix == prefix)) { memmove(n->ns + i, n->ns + i + 1, (size_t)(n->nns - i - 1) * sizeof(XNs)); n->nns--; }
    for (i = 0; i < n->nkids; i++) if (n->kids[i]->kind == XN_ELEMENT) { V2 kv = wrap(n->kids[i]); x_removeNamespace(vm, kv, argc, argv); }
    return self;
}
static void normalize_node(VM2 *vm, XNode *x, int depth) {
    int i;
    if (x->kind != XN_ELEMENT || depth > 2000) return;
    for (i = 0; i < x->nkids; i++) {
        if (x->kids[i]->kind == XN_ELEMENT) normalize_node(vm, x->kids[i], depth + 1);
        else if (x->kids[i]->kind == XN_TEXT) {
            while (i + 1 < x->nkids && x->kids[i + 1]->kind == XN_TEXT) { x->kids[i]->value = a2_concat(vm, x->kids[i]->value, x->kids[i + 1]->value); xn_remove_kid_at(x, i + 1); }
            { const char *t = x->kids[i]->value->s; while (*t == ' ' || *t == '\t' || *t == '\r' || *t == '\n') t++; if (!*t) { xn_remove_kid_at(x, i); i--; } }   /* empty or whitespace-only text goes */
        }
    }
}
static V2 x_normalize(VM2 *vm, V2 self, int argc, V2 *argv) {
    (void)argc; (void)argv;
    if (is_node(self)) { normalize_node(vm, NOD(self), 0); return self; }
    if (is_list(self)) {
        XList *l = LST(self); int i;
        for (i = 0; i < l->n; i++) {
            XNode *x = l->items[i];
            if (x->kind == XN_ELEMENT) normalize_node(vm, x, 0);
            else if (x->kind == XN_TEXT) {
                while (i + 1 < l->n && l->items[i + 1]->kind == XN_TEXT) { x->value = a2_concat(vm, x->value, l->items[i + 1]->value); xn_detach(l->items[i + 1]); list_remove_at(l, i + 1); }
                if (x->value->len == 0) { xn_detach(x); list_remove_at(l, i); i--; }
            }
        }
    }
    return self;
}
static V2 x_childIndex(VM2 *vm, V2 self, int argc, V2 *argv) { XNode *n = self_node(vm, self, "childIndex"); (void)argc; (void)argv; if (!n || !n->parent || n->kind == XN_ATTR) return v2_int(-1); return v2_int(xn_kid_index(n->parent, n)); }
static V2 x_toJSON(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; (void)argc; (void)argv; return v2_cstr(vm, "XML"); }
static V2 xl_toJSON(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; (void)argc; (void)argv; return v2_cstr(vm, "XMLList"); }
static V2 x_settings_get(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = a2_new_plain(vm); (void)self; (void)argc; (void)argv; a2_set_s(vm, v2_obj(o), "ignoreComments", v2_bool(g_ignore_comments)); a2_set_s(vm, v2_obj(o), "ignoreProcessingInstructions", v2_bool(g_ignore_pi)); a2_set_s(vm, v2_obj(o), "ignoreWhitespace", v2_bool(g_ignore_ws)); a2_set_s(vm, v2_obj(o), "prettyPrinting", v2_bool(g_pretty)); a2_set_s(vm, v2_obj(o), "prettyIndent", v2_int(g_indent)); return v2_obj(o); }
static V2 x_setSettings(VM2 *vm, V2 self, int argc, V2 *argv) { V2 o = ARG(0); (void)self; if (o.t != T_OBJ && o.t != T_UNDEF && o.t != T_NULL) return v2_undef(); if (o.t != T_OBJ) { g_ignore_ws = g_ignore_comments = g_ignore_pi = g_pretty = 1; g_indent = 2; return v2_undef(); } { V2 v; v = a2_get_s(vm, o, "ignoreComments"); if (v.t != T_UNDEF) g_ignore_comments = v2_tobool(vm, v); v = a2_get_s(vm, o, "ignoreProcessingInstructions"); if (v.t != T_UNDEF) g_ignore_pi = v2_tobool(vm, v); v = a2_get_s(vm, o, "ignoreWhitespace"); if (v.t != T_UNDEF) g_ignore_ws = v2_tobool(vm, v); v = a2_get_s(vm, o, "prettyPrinting"); if (v.t != T_UNDEF) g_pretty = v2_tobool(vm, v); v = a2_get_s(vm, o, "prettyIndent"); if (v.t != T_UNDEF) g_indent = v2_toint(vm, v); } return v2_undef(); }
static V2 x_defaultSettings(VM2 *vm, V2 self, int argc, V2 *argv) { A2Obj *o = a2_new_plain(vm); (void)self; (void)argc; (void)argv; a2_set_s(vm, v2_obj(o), "ignoreComments", v2_bool(1)); a2_set_s(vm, v2_obj(o), "ignoreProcessingInstructions", v2_bool(1)); a2_set_s(vm, v2_obj(o), "ignoreWhitespace", v2_bool(1)); a2_set_s(vm, v2_obj(o), "prettyPrinting", v2_bool(1)); a2_set_s(vm, v2_obj(o), "prettyIndent", v2_int(2)); return v2_obj(o); }
#define XSTATIC(name, var) static V2 name##_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)vm; (void)self; (void)argc; (void)argv; return v2_bool(var); } static V2 name##_set(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; var = v2_tobool(vm, ARG(0)); return v2_undef(); }
XSTATIC(xs_ic, g_ignore_comments) XSTATIC(xs_ip, g_ignore_pi) XSTATIC(xs_iw, g_ignore_ws) XSTATIC(xs_pp, g_pretty)
static V2 xs_pi_get(VM2 *vm, V2 self, int argc, V2 *argv) { (void)vm; (void)self; (void)argc; (void)argv; return v2_int(g_indent); }
static V2 xs_pi_set(VM2 *vm, V2 self, int argc, V2 *argv) { (void)self; g_indent = INT(0); return v2_undef(); }

/* XMLList.toString: a list of a single attribute/text prints its value; otherwise per spec */
static V2 xl_toString(VM2 *vm, V2 self, int argc, V2 *argv) { return x_toString(vm, self, argc, argv); }

void a2_xml_init(VM2 *vm) {
    A2Class *c; extern void a2_global_class(VM2 *vm, A2Class *c);
    struct { const char *n; NativeFn2 f; int np; } common[] = {
        { "toString", x_toString, 0 }, { "toXMLString", x_toXMLString, 0 }, { "length", x_length, 0 }, { "name", x_name, 0 }, { "localName", x_localName, 0 },
        { "nodeKind", x_nodeKind, 0 }, { "children", x_children, 0 }, { "elements", x_elements, 1 }, { "text", x_text, 0 }, { "comments", x_comments, 0 }, { "processingInstructions", x_processingInstructions, 1 },
        { "attributes", x_attributes, 0 }, { "attribute", x_attribute, 1 }, { "child", x_child, 1 }, { "descendants", x_descendants, 1 }, { "parent", x_parent, 0 },
        { "appendChild", x_appendChild, 1 }, { "prependChild", x_prependChild, 1 }, { "insertChildAfter", x_insertChildAfter, 2 }, { "insertChildBefore", x_insertChildBefore, 2 },
        { "copy", x_copy, 0 }, { "hasSimpleContent", x_hasSimpleContent, 0 }, { "hasComplexContent", x_hasComplexContent, 0 }, { "hasOwnProperty", x_hasOwnProperty, 1 }, { "contains", x_contains, 1 },
        { "setName", x_setName, 1 }, { "setNotification", x_setNotification, 1 }, { "notification", x_notification, 0 }, { "setLocalName", x_setLocalName, 1 }, { "setChildren", x_setChildren, 1 }, { "replace", x_replace, 2 }, { "namespace", x_namespace, 1 }, { "addNamespace", x_addNamespace, 1 }, { "setNamespace", x_setNamespace, 1 }, { "removeNamespace", x_removeNamespace, 1 },
        { "inScopeNamespaces", x_inScopeNamespaces, 0 }, { "namespaceDeclarations", x_namespaceDeclarations, 0 },
        { "valueOf", x_valueOf, 0 }, { "normalize", x_normalize, 0 }, { "childIndex", x_childIndex, 0 }, { "propertyIsEnumerable", x_propertyIsEnumerable, 1 }, { NULL, NULL, 0 } };
    int i;
    vm->c_xml = c = a2_class_new(vm, "", "XML", vm->c_object, OK_XML); c->native_ctor = XML_ctor; c->native_call = XML_call; c->is_dynamic = 0; c->flags |= CF_FINAL; c->ctor_sig = "(*=)";
    for (i = 0; common[i].n; i++) a2_add_method(vm, c, common[i].n, common[i].f, common[i].np);
    a2_add_method(vm, c, "toJSON", x_toJSON, 1);
    a2_add_static(vm, c, "settings", x_settings_get, 0); a2_add_static(vm, c, "setSettings", x_setSettings, 1); a2_add_static(vm, c, "defaultSettings", x_defaultSettings, 0);
    { A2Trait *t; extern void a2_tm_put(A2TraitMap *tm, A2Trait *t);
      struct { const char *n; NativeFn2 g, s; } st[] = { { "ignoreComments", xs_ic_get, xs_ic_set }, { "ignoreProcessingInstructions", xs_ip_get, xs_ip_set }, { "ignoreWhitespace", xs_iw_get, xs_iw_set }, { "prettyPrinting", xs_pp_get, xs_pp_set }, { "prettyIndent", xs_pi_get, xs_pi_set } };
      for (i = 0; i < 5; i++) { t = (A2Trait*)calloc(1, sizeof(A2Trait)); t->name = a2_str(vm, st[i].n); t->ns = vm->ns_public; t->kind = TK_GETTER; t->getter = a2_native_method(vm, st[i].g, st[i].n, 0); t->setter = a2_native_method(vm, st[i].s, st[i].n, 1); t->getter->owner = c; t->setter->owner = c; a2_tm_put(&c->ctraits, t); } }
    for (i = 0; common[i].n; i++) a2_dyn_set_hidden(vm, c->prototype, a2_str(vm, common[i].n), v2_obj(a2_new_native_fn(vm, common[i].f, common[i].n, common[i].np)));
    a2_global_class(vm, c);
    vm->c_xmllist = c = a2_class_new(vm, "", "XMLList", vm->c_object, OK_XML); c->native_ctor = XMLList_ctor; c->native_call = XMLList_call; c->is_dynamic = 0; c->flags |= CF_FINAL; c->ctor_sig = "(*=)";
    for (i = 0; common[i].n; i++) a2_add_method(vm, c, common[i].n, common[i].n[0] == 't' && !strcmp(common[i].n, "toString") ? xl_toString : common[i].f, common[i].np);
    a2_add_method(vm, c, "toJSON", xl_toJSON, 1);
    for (i = 0; common[i].n; i++) a2_dyn_set_hidden(vm, c->prototype, a2_str(vm, common[i].n), v2_obj(a2_new_native_fn(vm, common[i].f, common[i].n, common[i].np)));
    a2_global_class(vm, c);
}

/* a method call the XML prototype does not answer: simple content forwards it
 * to the string value, a one-item list to its item (avmplus callProperty) */
int a2_xml_call_fallback(VM2 *vm, V2 obj, A2Name *n, int argc, V2 *argv, V2 *out) {
    A2Obj *o = obj.u.o; V2 prop; A2Prop *pp; A2Obj *pr;
    for (pr = o->cls ? o->cls->prototype : NULL; pr; pr = pr->proto) if ((pp = a2_dyn_find(pr, n->name)) != NULL && pp->v.t != T_UNDEF) return 0;
    if (!o->payload) return 0;
    prop = a2_xml_get(vm, obj, n);
    if (vm->throwing || !is_list(prop) || LST(prop)->n != 0) return 0;
    if (is_list(obj)) {
        XList *l = LST(obj);
        if (l->n != 1) return 0;
        *out = a2_call_prop(vm, wrap(l->items[0]), n, argc, argv);
        return 1;
    }
    if (!has_simple_content(NOD(obj))) return 0;
    *out = a2_call_prop(vm, v2_str(node_to_string(vm, NOD(obj))), n, argc, argv);
    return 1;
}
