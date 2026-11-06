/* abc.c - ABC (ActionScript 3 bytecode) container parser */
#include "abc.h"
#include <stdlib.h>
#include <string.h>

typedef struct { const u8 *p, *end; int err; } AR;

static u32 r_u30(AR *r) {
    u32 v = 0; int shift = 0;
    while (r->p < r->end && shift < 35) {
        u8 b = *r->p++;
        v |= (u32)(b & 0x7f) << shift;
        if (!(b & 0x80) || shift == 28) return v;   /* avmplus reads at most 5 bytes */
        shift += 7;
    }
    r->err = 1; return v;
}
static s32 r_s32v(AR *r) { return (s32)r_u30(r); }
static u8  r_byte(AR *r) { if (r->p < r->end) return *r->p++; r->err = 1; return 0; }
static u16 r_u16le(AR *r) { u16 a = r_byte(r); return (u16)(a | (r_byte(r) << 8)); }
static double r_d64(AR *r) {
    union { u64 i; double d; } u; int i;
    u.i = 0;
    for (i = 0; i < 8; i++) u.i |= (u64)r_byte(r) << (8 * i);
    return u.d;
}

u32 abc_read_u30(const u8 **pp) {
    u32 v = 0; int shift = 0; const u8 *p = *pp;
    for (;;) { u8 b = *p++; v |= (u32)(b & 0x7f) << shift; if (!(b & 0x80) || shift >= 28) break; shift += 7; }
    *pp = p; return v;
}
s32 abc_read_s24(const u8 **pp) {
    const u8 *p = *pp; s32 v = p[0] | (p[1] << 8) | (p[2] << 16);
    if (v & 0x800000) v |= ~0xffffff;
    *pp += 3; return v;
}

static void *zalloc(size_t n) { return calloc(n ? n : 1, 1); }

static int parse_traits(AR *r, AbcTrait **out, u32 *count) {
    u32 n = r_u30(r), i;
    AbcTrait *t;
    if (n > 100000) { r->err = 1; return 0; }
    t = (AbcTrait*)zalloc(n * sizeof(AbcTrait));
    if (!t) { r->err = 1; return 0; }
    for (i = 0; i < n; i++) {
        u8 k;
        t[i].name = r_u30(r);
        k = r_byte(r);
        t[i].kind = k & 15; t[i].attrs = k >> 4;
        switch (t[i].kind) {
        case TRAIT_SLOT: case TRAIT_CONST:
            t[i].slot_id = r_u30(r); t[i].type_name = r_u30(r); t[i].vindex = r_u30(r);
            if (t[i].vindex) t[i].vkind = r_byte(r);
            break;
        case TRAIT_CLASS: t[i].slot_id = r_u30(r); t[i].classi = r_u30(r); break;
        case TRAIT_FUNCTION: t[i].slot_id = r_u30(r); t[i].method = r_u30(r); break;
        case TRAIT_METHOD: case TRAIT_GETTER: case TRAIT_SETTER:
            t[i].disp_id = r_u30(r); t[i].method = r_u30(r); break;
        default: r->err = 1; break;
        }
        if (t[i].attrs & TATTR_METADATA) {
            u32 mc = r_u30(r), k2;
            t[i].metadata_count = mc;
            t[i].metadata = (u32*)zalloc(mc * sizeof(u32));
            for (k2 = 0; k2 < mc; k2++) { u32 v = r_u30(r); if (t[i].metadata) t[i].metadata[k2] = v; }
        }
        if (r->err) break;
    }
    *out = t; *count = n;
    return !r->err;
}

int abc_error; u32 abc_error_arg, abc_error_arg2; int abc_fail_line;
AbcFile *abc_parse(const u8 *data, size_t len) {
    AR r; AbcFile *a; u32 i, j;
    size_t strtotal = 0;
    r.p = data; r.end = data + len; r.err = 0;
    a = (AbcFile*)zalloc(sizeof(AbcFile));
    if (!a) return NULL;
    a->minor = r_u16le(&r); a->major = r_u16le(&r);

    /* ints */
    a->int_count = r_u30(&r); if (a->int_count == 0) a->int_count = 1;
    a->ints = (s32*)zalloc(a->int_count * sizeof(s32));
    for (i = 1; i < a->int_count; i++) a->ints[i] = r_s32v(&r);
    a->uint_count = r_u30(&r); if (a->uint_count == 0) a->uint_count = 1;
    a->uints = (u32*)zalloc(a->uint_count * sizeof(u32));
    for (i = 1; i < a->uint_count; i++) a->uints[i] = r_u30(&r);
    a->double_count = r_u30(&r); if (a->double_count == 0) a->double_count = 1;
    a->doubles = (double*)zalloc(a->double_count * sizeof(double));
    a->doubles[0] = 0.0 / 0.0;
    for (i = 1; i < a->double_count; i++) a->doubles[i] = r_d64(&r);
    /* strings: copy NUL-terminated */
    a->string_count = r_u30(&r); if (a->string_count == 0) a->string_count = 1;
    if (a->string_count > 1000000 || r.err) { abc_fail_line = __LINE__; abc_free(a); return NULL; }
    a->strings = (const char**)zalloc(a->string_count * sizeof(char*));
    a->string_lens = (u32*)zalloc(a->string_count * sizeof(u32));
    {
        const u8 *save = r.p; AR r2 = r;
        for (i = 1; i < a->string_count; i++) { u32 n = r_u30(&r2); if (r2.p + n > r2.end) { r2.err = 1; break; } strtotal += n + 1; r2.p += n; }
        if (r2.err) { abc_fail_line = __LINE__; abc_free(a); return NULL; }
        a->owned = (u8*)zalloc(strtotal + 1); a->owned_len = strtotal + 1;
        r.p = save;
        {
            u8 *o = a->owned;
            a->strings[0] = (const char*)o; *o++ = 0;
            for (i = 1; i < a->string_count; i++) {
                u32 n = r_u30(&r);
                memcpy(o, r.p, n); o[n] = 0;
                a->strings[i] = (const char*)o; a->string_lens[i] = n;
                o += n + 1; r.p += n;
            }
        }
    }
    /* namespaces */
    a->ns_count = r_u30(&r); if (a->ns_count == 0) a->ns_count = 1;
    a->namespaces = (AbcNamespace*)zalloc(a->ns_count * sizeof(AbcNamespace));
    a->namespaces[0].kind = NS_PACKAGE; a->namespaces[0].name = 0;    /* "*" / any */
    for (i = 1; i < a->ns_count; i++) { a->namespaces[i].kind = r_byte(&r); a->namespaces[i].name = r_u30(&r); }
    a->nsset_count = r_u30(&r); if (a->nsset_count == 0) a->nsset_count = 1;
    a->nssets = (AbcNsSet*)zalloc(a->nsset_count * sizeof(AbcNsSet));
    for (i = 1; i < a->nsset_count; i++) {
        a->nssets[i].count = r_u30(&r);
        a->nssets[i].ns = (u32*)zalloc(a->nssets[i].count * sizeof(u32));
        for (j = 0; j < a->nssets[i].count; j++) a->nssets[i].ns[j] = r_u30(&r);
    }
    a->mn_count = r_u30(&r); if (a->mn_count == 0) a->mn_count = 1;
    a->multinames = (AbcMultiname*)zalloc(a->mn_count * sizeof(AbcMultiname));
    for (i = 1; i < a->mn_count; i++) {
        AbcMultiname *m = &a->multinames[i];
        m->kind = r_byte(&r);
        switch (m->kind) {
        case MN_QNAME: case MN_QNAME_A: m->ns = r_u30(&r); m->name = r_u30(&r); break;
        case MN_RTQNAME: case MN_RTQNAME_A: m->name = r_u30(&r); break;
        case MN_RTQNAME_L: case MN_RTQNAME_LA: break;
        case MN_MULTINAME: case MN_MULTINAME_A: m->name = r_u30(&r); m->ns_set = r_u30(&r); break;
        case MN_MULTINAME_L: case MN_MULTINAME_LA: m->ns_set = r_u30(&r); break;
        case MN_TYPENAME:
            m->type_base = r_u30(&r); m->type_param_count = r_u30(&r);
            m->type_params = (u32*)zalloc(m->type_param_count * sizeof(u32));
            for (j = 0; j < m->type_param_count; j++) m->type_params[j] = r_u30(&r);
            break;
        default: r.err = 1; break;
        }
        if (r.err) { abc_fail_line = __LINE__; abc_free(a); return NULL; }
    }
    /* methods */
    a->method_count = r_u30(&r);
    a->methods = (AbcMethod*)zalloc(a->method_count * sizeof(AbcMethod));
    for (i = 0; i < a->method_count; i++) {
        AbcMethod *m = &a->methods[i];
        m->param_count = r_u30(&r);
        m->return_type = r_u30(&r);
        m->param_types = (u32*)zalloc(m->param_count * sizeof(u32));
        for (j = 0; j < m->param_count; j++) m->param_types[j] = r_u30(&r);
        m->name = r_u30(&r);
        m->flags = r_byte(&r);
        m->body = -1;
        if (m->flags & MF_HAS_OPTIONAL) {
            m->option_count = r_u30(&r);
            m->options = (AbcOption*)zalloc(m->option_count * sizeof(AbcOption));
            for (j = 0; j < m->option_count; j++) { m->options[j].val = r_u30(&r); m->options[j].kind = r_byte(&r); }
        }
        if (m->flags & MF_HAS_PARAM_NAMES) {
            m->param_names = (u32*)zalloc(m->param_count * sizeof(u32));
            for (j = 0; j < m->param_count; j++) m->param_names[j] = r_u30(&r);
        }
        if (r.err) { abc_fail_line = __LINE__; abc_free(a); return NULL; }
    }
    /* metadata */
    a->metadata_count = r_u30(&r);
    a->metadata = (AbcMetadata*)zalloc(a->metadata_count * sizeof(AbcMetadata));
    for (i = 0; i < a->metadata_count; i++) {
        u32 n; AbcMetadata *m = &a->metadata[i];
        m->name = r_u30(&r); n = r_u30(&r);
        if (r.err || n > 100000) { abc_fail_line = __LINE__; abc_free(a); return NULL; }
        m->item_count = n; m->keys = (u32*)zalloc(n * sizeof(u32)); m->vals = (u32*)zalloc(n * sizeof(u32));
        for (j = 0; j < n; j++) m->keys[j] = r_u30(&r);
        for (j = 0; j < n; j++) m->vals[j] = r_u30(&r);
    }
    /* classes */
    a->class_count = r_u30(&r);
    a->instances = (AbcInstance*)zalloc(a->class_count * sizeof(AbcInstance));
    a->classes = (AbcClass*)zalloc(a->class_count * sizeof(AbcClass));
    for (i = 0; i < a->class_count; i++) {
        AbcInstance *in = &a->instances[i];
        in->name = r_u30(&r); in->super_name = r_u30(&r); in->flags = r_byte(&r);
        if (in->flags & CF_PROTECTED_NS) in->protected_ns = r_u30(&r);
        in->interface_count = r_u30(&r);
        in->interfaces = (u32*)zalloc(in->interface_count * sizeof(u32));
        for (j = 0; j < in->interface_count; j++) in->interfaces[j] = r_u30(&r);
        in->iinit = r_u30(&r);
        if (!parse_traits(&r, &in->traits, &in->trait_count)) { abc_fail_line = __LINE__; abc_free(a); return NULL; }
    }
    for (i = 0; i < a->class_count; i++) {
        a->classes[i].cinit = r_u30(&r);
        if (!parse_traits(&r, &a->classes[i].traits, &a->classes[i].trait_count)) { abc_fail_line = __LINE__; abc_free(a); return NULL; }
    }
    a->script_count = r_u30(&r);
    a->scripts = (AbcScript*)zalloc(a->script_count * sizeof(AbcScript));
    for (i = 0; i < a->script_count; i++) {
        a->scripts[i].init = r_u30(&r);
        if (!parse_traits(&r, &a->scripts[i].traits, &a->scripts[i].trait_count)) { abc_fail_line = __LINE__; abc_free(a); return NULL; }
    }
    a->body_count = r_u30(&r);
    a->bodies = (AbcBody*)zalloc(a->body_count * sizeof(AbcBody));
    for (i = 0; i < a->body_count; i++) {
        AbcBody *b = &a->bodies[i];
        b->method = r_u30(&r);
        b->max_stack = r_u30(&r); b->local_count = r_u30(&r);
        b->init_scope_depth = r_u30(&r); b->max_scope_depth = r_u30(&r);
        b->code_len = r_u30(&r);
        if (r.p + b->code_len > r.end) { abc_fail_line = __LINE__; abc_free(a); return NULL; }
        b->code = r.p; r.p += b->code_len;
        b->exception_count = r_u30(&r);
        b->exceptions = (AbcException*)zalloc(b->exception_count * sizeof(AbcException));
        for (j = 0; j < b->exception_count; j++) {
            AbcException *e = &b->exceptions[j];
            e->from = r_u30(&r); e->to = r_u30(&r); e->target = r_u30(&r); e->exc_type = r_u30(&r); e->var_name = r_u30(&r);
        }
        if (!parse_traits(&r, &b->traits, &b->trait_count)) { abc_fail_line = __LINE__; abc_free(a); return NULL; }
        if (b->method >= a->method_count) { abc_error = 1027; abc_error_arg = b->method; abc_error_arg2 = a->method_count; abc_free(a); return NULL; }
        if (a->methods[b->method].body >= 0 && !a->dup_body) a->dup_body = (s32)b->method + 1;   /* reported by the loader (#1121) */
        a->methods[b->method].body = (s32)i;
    }
    if (r.err) { abc_fail_line = __LINE__; abc_free(a); return NULL; }
    return a;
}

void abc_free(AbcFile *a) {
    u32 i;
    if (!a) return;
    free(a->ints); free(a->uints); free(a->doubles); free(a->strings); free(a->string_lens); free(a->owned);
    free(a->namespaces);
    if (a->metadata) { for (i = 0; i < a->metadata_count; i++) { free(a->metadata[i].keys); free(a->metadata[i].vals); } free(a->metadata); }
    if (a->nssets) { for (i = 0; i < a->nsset_count; i++) free(a->nssets[i].ns); free(a->nssets); }
    if (a->multinames) { for (i = 0; i < a->mn_count; i++) free(a->multinames[i].type_params); free(a->multinames); }
    if (a->methods) { for (i = 0; i < a->method_count; i++) { free(a->methods[i].param_types); free(a->methods[i].options); free(a->methods[i].param_names); } free(a->methods); }
#define FREE_TRAITS(t, n) do { u32 k_; for (k_ = 0; k_ < (n); k_++) free((t)[k_].metadata); free(t); } while (0)
    if (a->instances) { for (i = 0; i < a->class_count; i++) { free(a->instances[i].interfaces); FREE_TRAITS(a->instances[i].traits, a->instances[i].trait_count); } free(a->instances); }
    if (a->classes) { for (i = 0; i < a->class_count; i++) FREE_TRAITS(a->classes[i].traits, a->classes[i].trait_count); free(a->classes); }
    if (a->scripts) { for (i = 0; i < a->script_count; i++) FREE_TRAITS(a->scripts[i].traits, a->scripts[i].trait_count); free(a->scripts); }
    if (a->bodies) { for (i = 0; i < a->body_count; i++) { free(a->bodies[i].exceptions); FREE_TRAITS(a->bodies[i].traits, a->bodies[i].trait_count); } free(a->bodies); }
    free(a);
}

const char *abc_str(const AbcFile *a, u32 idx) { return (idx < a->string_count) ? a->strings[idx] : ""; }
const char *abc_mn_name(const AbcFile *a, u32 mn) {
    if (mn >= a->mn_count) return "";
    return abc_str(a, a->multinames[mn].name);
}
const char *abc_ns_uri(const AbcFile *a, u32 ns) { return (ns < a->ns_count) ? abc_str(a, a->namespaces[ns].name) : ""; }
int abc_ns_kind(const AbcFile *a, u32 ns) { return (ns < a->ns_count) ? a->namespaces[ns].kind : NS_PACKAGE; }
