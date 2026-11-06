/* FLASHNT - AVM2 bytecode verifier (subset of the Flash Player verifier).
 * Checks reachable code of a method body on first invocation:
 *   #1011 illegal opcode, #1015 dxns without SET_DXNS, #1020 fall off end,
 *   #1021 bad branch target, #1025 invalid register, #1032 constant pool index
 *   out of range; then a data-flow pass (see below) for stack/scope depths and
 *   a conservative set of type errors. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "avm2_int.h"
#include "abc.h"

/* operand kinds */
enum { OP_BAD = 0, OP_NONE, OP_U30, OP_U30U30, OP_S24, OP_U8, OP_SWITCH, OP_DEBUG, OP_S8 };
/* cpool kinds for the (first) u30 operand */
enum { CP_NONE = 0, CP_INT, CP_UINT, CP_DOUBLE, CP_STRING, CP_NS, CP_MN, CP_METHOD, CP_CLASS, CP_EXC, CP_LOCAL, CP_SLOT, CP_MNARGS };

static void opinfo(int op, int *kind, int *cp) {
    *cp = CP_NONE;
    switch (op) {
    case 0x01: case 0x02: case 0x03: case 0x07: case 0x09: case 0x1c: case 0x1d: case 0x1e: case 0x1f: case 0x20: case 0x21: case 0x23:
    case 0x26: case 0x27: case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x30: case 0x35: case 0x36: case 0x37: case 0x38:
    case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d: case 0x3e: case 0x47: case 0x48: case 0x50: case 0x51: case 0x52: case 0x57: case 0x64:
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: case 0x81: case 0x82:
    case 0x83: case 0x84: case 0x85: case 0x87: case 0x88: case 0x89: case 0x90: case 0x91: case 0x93: case 0x95: case 0x96:
    case 0x97: case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4: case 0xa5: case 0xa6: case 0xa7: case 0xa8: case 0xa9:
    case 0xaa: case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf: case 0xb0: case 0xb1: case 0xb3: case 0xb4: case 0xc0:
    case 0xc1: case 0xc4: case 0xc5: case 0xc6: case 0xc7: case 0xd0: case 0xd1: case 0xd2: case 0xd3: case 0xd4: case 0xd5:
    case 0xd6: case 0xd7: case 0xf2: case 0xf3:
        *kind = OP_NONE; return;
    case 0x04: case 0x05: case 0x59: case 0x5d: case 0x5e: case 0x5f: case 0x60: case 0x61: case 0x66: case 0x68: case 0x6a:
    case 0x80: case 0x86: case 0xb2:
        *kind = OP_U30; *cp = CP_MN; return;
    case 0x08: case 0x62: case 0x63: case 0x92: case 0x94: case 0xc2: case 0xc3:
        *kind = OP_U30; *cp = CP_LOCAL; return;
    case 0x67: *kind = OP_U30; return;   /* getouterscope */
    case 0x06: case 0x2c: case 0xf1: *kind = OP_U30; *cp = CP_STRING; return;
    case 0x2d: *kind = OP_U30; *cp = CP_INT; return;
    case 0x2e: *kind = OP_U30; *cp = CP_UINT; return;
    case 0x2f: *kind = OP_U30; *cp = CP_DOUBLE; return;
    case 0x31: *kind = OP_U30; *cp = CP_NS; return;
    case 0x40: *kind = OP_U30; *cp = CP_METHOD; return;
    case 0x58: *kind = OP_U30; *cp = CP_CLASS; return;
    case 0x5a: *kind = OP_U30; *cp = CP_EXC; return;
    case 0x41: case 0x42: case 0x49: case 0x53: case 0x55: case 0x56: case 0x6c: case 0x6d: case 0x6e: case 0x6f: case 0xf0:
        *kind = OP_U30; return;
    case 0x43: case 0x44: case 0x32: *kind = OP_U30U30; return;
    case 0x45: case 0x46: case 0x4a: case 0x4c: case 0x4e: case 0x4f: *kind = OP_U30U30; *cp = CP_MNARGS; return;
    case 0x0c: case 0x0d: case 0x0e: case 0x0f: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16:
    case 0x17: case 0x18: case 0x19: case 0x1a:
        *kind = OP_S24; return;
    case 0x24: *kind = OP_S8; return;
    case 0x25: *kind = OP_U30; return;       /* pushshort: u30 in the file format */
    case 0x65: *kind = OP_U8; return;
    case 0x1b: *kind = OP_SWITCH; return;
    case 0xef: *kind = OP_DEBUG; return;
    default: *kind = OP_BAD; return;
    }
}

typedef struct { const AbcFile *abc; const AbcBody *body; u8 *start; int err; char msg[256]; } Ver;

static void vfail(Ver *v, int code, const char *msg) { if (!v->err) { v->err = code; strncpy(v->msg, msg, 255); v->msg[255] = 0; } }
static int cp_check(Ver *v, int cp, u32 idx, u32 count) {
    char b[120];
    if (cp == CP_CLASS && idx >= count) {
        sprintf(b, "ClassInfo %lu exceeds class_count=%lu.", (unsigned long)idx, (unsigned long)count);
        vfail(v, 1060, b); return 0;
    }
    if ((cp == CP_STRING || cp == CP_INT || cp == CP_UINT || cp == CP_DOUBLE) ? (idx == 0 || idx >= count) : idx >= count) {
        sprintf(b, "Cpool index %lu is out of range %lu.", (unsigned long)idx, (unsigned long)count);
        vfail(v, 1032, b); return 0;
    }
    return 1;
}
#define BADTARGET "At least one branch target was not on a valid instruction in the method."
/* Branch from instruction at pc to tgt.  Backward targets must already be block starts
 * (a label, an earlier forward-branch target or an exception handler), as in the Flash verifier. */
static void branch(Ver *v, u32 pc, u32 tgt) {
    if (tgt >= v->body->code_len) { if (getenv("FLASHNT_DEBUG")) fprintf(stderr, "[verify] branch at %lu to %lu past the end\n", (unsigned long)pc, (unsigned long)tgt); vfail(v, 1021, BADTARGET); return; }
    if (tgt <= pc) { if (!v->start[tgt]) { if (getenv("FLASHNT_DEBUG")) fprintf(stderr, "[verify] backward branch at %lu to %lu without block start\n", (unsigned long)pc, (unsigned long)tgt); vfail(v, 1021, BADTARGET); } return; }
    v->start[tgt] = 1;
}

/* ------------------------------------------------------------------ */
/* Data-flow pass (after the structural checks succeeded): abstract
 * interpretation of the operand stack depth, scope depth and a small type
 * lattice, per block, merged at join points and exception handlers.
 *   #1017/#1018 scope overflow/underflow, #1019 getscopeobject out of bounds,
 *   #1023/#1024 stack overflow/underflow, #1030/#1031 unbalanced stack/scope,
 *   #1026/#1051/#1058 type errors.
 * Types are conservative: TY_UNK means "not known here" and never fails a
 * check; a type error is raised only for a type the Flash verifier infers too. */
enum { TY_UNK = 0, TY_ANY, TY_NULL, TY_VOID, TY_INT, TY_UINT, TY_NUMBER, TY_BOOL, TY_STRING, TY_NS,
       TY_OBJECT, TY_ARRAY, TY_FUNCTION, TY_NBUILTIN };
#define DF_MAXCT 200    /* class types: TY_NBUILTIN + index */
typedef unsigned char Ty;

typedef struct {
    VM2 *vm; A2Method *m; const AbcFile *abc; const AbcBody *b; const u8 *code; u32 len;
    const u8 *start;
    long maxstack; int nloc; long maxscope;   /* maxscope < 0: not checked */
    int W;                                    /* types per state: maxstack + nloc, 0 = untyped */
    int *bidx; int nst, capst;
    int *ssp_, *sssp_; Ty *ty; u8 *inq;
    u32 *wl; int nwl, capwl;
    A2Class *ct[DF_MAXCT]; u8 ctk[DF_MAXCT]; int nct;   /* ctk: 0 instance of, 1 the class object */
    int checktypes, lex_ok, queue;
    const char *mname;
    int err; char msg[256];
    long sp, ssp; Ty *cur;                    /* current state: cur[0..maxstack) stack, then locals */
} DF;

static void dfail(DF *d, int code, const char *msg) {
    if (!d->err) { d->err = code; strncpy(d->msg, msg, 255); d->msg[255] = 0; if (getenv("FLASHNT_DEBUG")) fprintf(stderr, "[verify] dataflow: %s\n", msg); }
}
static Ty ty_class(DF *d, A2Class *c, int k) {
    int i;
    if (!c) return TY_UNK;
    for (i = 0; i < d->nct; i++) if (d->ct[i] == c && d->ctk[i] == k) return (Ty)(TY_NBUILTIN + i);
    if (d->nct >= DF_MAXCT) return TY_UNK;
    d->ct[d->nct] = c; d->ctk[d->nct] = (u8)k; return (Ty)(TY_NBUILTIN + d->nct++);
}
static void ty_name(DF *d, Ty t, char *out, size_t cap) {
    static const char *bn[] = { "*", "*", "null", "void", "int", "uint", "Number", "Boolean", "String", "Namespace", "Object", "Array", "Function" };
    if (t < TY_NBUILTIN) { strncpy(out, bn[t], cap - 1); out[cap - 1] = 0; return; }
    { A2Class *c = d->ct[t - TY_NBUILTIN]; A2Str *n = a2_class_name(d->vm, c, 0);
      sprintf(out, "%.100s%s", n ? n->s : "?", d->ctk[t - TY_NBUILTIN] ? "$" : ""); }
}
static Ty peek(DF *d, long depth) { return (d->W && depth < d->sp) ? d->cur[d->sp - 1 - depth] : TY_UNK; }
static int need(DF *d, unsigned long n) { if ((unsigned long)d->sp < n) { dfail(d, 1024, "Stack underflow occurred."); return 0; } return 1; }
static void dpop(DF *d, unsigned long n) { if (need(d, n)) d->sp -= (long)n; }
static void dpush(DF *d, Ty t) {
    if (d->err) return;
    if (d->sp >= d->maxstack) { dfail(d, 1023, "Stack overflow occurred."); return; }
    if (d->W) d->cur[d->sp] = t;
    d->sp++;
}
static Ty lget(DF *d, u32 r) { return (d->W && r < (u32)d->nloc) ? d->cur[d->maxstack + r] : TY_UNK; }
static void lset(DF *d, u32 r, Ty t) { if (d->W && r < (u32)d->nloc) d->cur[d->maxstack + r] = t; }

/* merge the state (sp, ssp, types) into the block at pc */
static void df_merge(DF *d, u32 pc, long sp, long ssp, const Ty *types) {
    int i, k, changed = 0; char b[120];
    if (d->err || pc >= d->len) return;
    i = d->bidx[pc];
    if (i < 0) {
        if (d->nst >= d->capst) {
            int nc = d->capst ? d->capst * 2 : 16;
            d->ssp_ = (int*)realloc(d->ssp_, nc * sizeof(int)); d->sssp_ = (int*)realloc(d->sssp_, nc * sizeof(int));
            d->inq = (u8*)realloc(d->inq, nc); if (d->W) d->ty = (Ty*)realloc(d->ty, (size_t)nc * d->W);
            if (!d->ssp_ || !d->sssp_ || !d->inq || (d->W && !d->ty)) { dfail(d, -1, "out of memory"); return; }
            d->capst = nc;
        }
        i = d->nst++; d->bidx[pc] = i;
        d->ssp_[i] = (int)sp; d->sssp_[i] = (int)ssp; d->inq[i] = 0;
        if (d->W) {
            Ty *s = d->ty + (size_t)i * d->W;
            memset(s, TY_UNK, d->W);
            if (types) { memcpy(s, types, (size_t)sp); memcpy(s + d->maxstack, types + d->maxstack, d->nloc); }
        }
        changed = 1;
    } else {
        if (d->ssp_[i] != sp) { sprintf(b, "Stack depth is unbalanced. %ld != %d.", sp, d->ssp_[i]); dfail(d, 1030, b); return; }
        if (d->sssp_[i] != ssp) { sprintf(b, "Scope depth is unbalanced. %ld != %d.", ssp, d->sssp_[i]); dfail(d, 1031, b); return; }
        if (d->W) {
            Ty *s = d->ty + (size_t)i * d->W;
            for (k = 0; k < d->W; k++) {
                Ty in = types ? types[k] : TY_UNK;
                if (k >= sp && k < d->maxstack) continue;
                if (s[k] != in && s[k] != TY_UNK) { s[k] = TY_UNK; changed = 1; }
            }
        }
    }
    if (changed && d->queue && !d->inq[i]) {
        if (d->nwl >= d->capwl) { d->capwl = d->capwl ? d->capwl * 2 : 64; d->wl = (u32*)realloc(d->wl, d->capwl * sizeof(u32)); if (!d->wl) { dfail(d, -1, "out of memory"); return; } }
        d->wl[d->nwl++] = pc; d->inq[i] = 1;
    }
}
static void df_jump(DF *d, u32 tgt) { df_merge(d, tgt, d->sp, d->ssp, d->W ? d->cur : NULL); }

/* runtime multiname parts: *ns = 1 for a runtime namespace, *nm = 1 for a runtime name */
static int mn_rt(const AbcFile *abc, u32 mn, int *ns, int *nm) {
    int k = mn < abc->mn_count ? abc->multinames[mn].kind : 0;
    *ns = (k == MN_RTQNAME || k == MN_RTQNAME_A || k == MN_RTQNAME_L || k == MN_RTQNAME_LA);
    *nm = (k == MN_RTQNAME_L || k == MN_RTQNAME_LA || k == MN_MULTINAME_L || k == MN_MULTINAME_LA);
    return *ns + *nm;
}
/* an op on multiname mn: `before` values under the runtime parts (receiver), `after` above them */
static void df_mnop(DF *d, u32 mn, unsigned long before, unsigned long after, int pushes, Ty t) {
    int ns, nm; unsigned long tot = before + after + (unsigned long)mn_rt(d->abc, mn, &ns, &nm);
    if (!need(d, tot)) return;
    if (ns && d->checktypes) {
        Ty v = peek(d, (long)(after + (unsigned long)nm));
        if (v != TY_UNK && v != TY_NS) { char b[200], tn[120]; ty_name(d, v, tn, sizeof(tn)); sprintf(b, "Illegal operand type: %s must be Namespace.", tn); dfail(d, 1058, b); return; }
    }
    d->sp -= (long)tot;
    if (pushes) dpush(d, t);
}
/* instance slot count of a class defined entirely by loaded ABC (Object has none); -1 unknown */
static long slot_count(DF *d, A2Class *c) {
    A2Class *k;
    for (k = c; k && k != d->vm->c_object; k = k->super) if (!k->abc || k->abc->is_builtin) return -1;
    if (!k || d->vm->c_object->nislots != 0) return -1;
    return c->nislots;
}
static void df_slot(DF *d, u32 slot, long depth) {
    Ty t; char b[240], tn[120];
    if (!d->checktypes || d->err) return;
    t = peek(d, depth);
    if (t == TY_ANY) { dfail(d, 1051, "Illegal early binding access to *."); return; }
    if (t >= TY_NBUILTIN && !d->ctk[t - TY_NBUILTIN] && slot) {
        long n = slot_count(d, d->ct[t - TY_NBUILTIN]);
        if (n >= 0 && (long)slot > n) { ty_name(d, t, tn, sizeof(tn)); sprintf(b, "Slot %lu exceeds slotCount=%ld of %s.", (unsigned long)slot, n, tn); dfail(d, 1026, b); }
    }
}
extern A2Class *a2_verify_qname_class(VM2 *vm, AbcFile *abc, u32 mn);
static Ty coerce_type(DF *d, u32 mn) {
    VM2 *vm = d->vm; A2Class *c = a2_verify_qname_class(vm, (AbcFile*)d->abc, mn);
    if (!c) return TY_UNK;
    if (c == vm->c_namespace) return TY_NS;
    if (c == vm->c_int) return TY_INT;
    if (c == vm->c_uint) return TY_UINT;
    if (c == vm->c_number) return TY_NUMBER;
    if (c == vm->c_boolean) return TY_BOOL;
    if (c == vm->c_string) return TY_STRING;
    if (c == vm->c_object) return TY_OBJECT;
    return ty_class(d, c, 0);
}
/* getlex of a class defined by loaded ABC, when no trait of the method's scopes can shadow it */
static Ty lex_type(DF *d, u32 mn) {
    A2Class *c, *k; const AbcFile *abc = d->abc; const char *nm; u32 i;
    if (!d->lex_ok) return TY_UNK;
    c = a2_verify_qname_class(d->vm, (AbcFile*)abc, mn);
    if (!c || !c->abc || c->abc->is_builtin || !c->name) return TY_UNK;
    for (k = d->m->owner; k; k = k->super)
        if (a2_find_trait_any(&k->itraits, c->name) || a2_find_trait_any(&k->ctraits, c->name)) return TY_UNK;
    nm = c->name->s;
    for (i = 0; i < d->b->trait_count; i++) if (!strcmp(abc_mn_name(abc, d->b->traits[i].name), nm)) return TY_UNK;
    return ty_class(d, c, 1);
}

/* avmplus/Ruffle canThrow = 0 (the union of both lists, so fewer handlers are reached) */
static int can_throw(int op) {
    switch (op) {
    case 0x01: case 0xf2: case 0xf3: case 0x24: case 0x2f: case 0x27: case 0x2d: case 0x31: case 0x28: case 0x20:
    case 0x25: case 0x2c: case 0x26: case 0x2e: case 0x21: case 0x2a: case 0x2b: case 0x29: case 0x95: case 0x64:
    case 0x65: case 0x67: case 0x6e: case 0x62: case 0xd0: case 0xd1: case 0xd2: case 0xd3: case 0x63: case 0xd4:
    case 0xd5: case 0xd6: case 0xd7: case 0x08: case 0x09: case 0x10: case 0x11: case 0x12: case 0x19: case 0x1a:
    case 0x1b: case 0x02: case 0x96: case 0x1d: case 0x47: case 0x86: case 0x89: case 0xb2: case 0xac:
    case 0xef: case 0xf0: case 0xf1:
        return 0;
    }
    return 1;
}

/* walk one block from pc0 with the stored entry state */
static void df_block(DF *d, u32 pc0) {
    const u8 *code = d->code; u32 pc = pc0; int si = d->bidx[pc0]; u32 k;
    d->sp = d->ssp_[si]; d->ssp = d->sssp_[si];
    if (d->W) memcpy(d->cur, d->ty + (size_t)si * d->W, d->W);
    while (!d->err) {
        const u8 *p; int op, kind, cp; u32 a1 = 0, a2 = 0, next; s32 off = 0;
        if (pc >= d->len) return;
        if (pc != pc0 && (d->start[pc] || d->bidx[pc] >= 0)) { df_jump(d, pc); return; }
        op = code[pc]; opinfo(op, &kind, &cp);
        p = code + pc + 1;
        switch (kind) {
        case OP_U30: a1 = abc_read_u30(&p); break;
        case OP_U30U30: a1 = abc_read_u30(&p); a2 = abc_read_u30(&p); break;
        case OP_S24: off = abc_read_s24(&p); break;
        case OP_U8: a1 = *p++; break;
        case OP_S8: p++; break;
        case OP_SWITCH: break;
        case OP_DEBUG: p += 1; abc_read_u30(&p); p += 1; abc_read_u30(&p); break;
        case OP_NONE: break;
        default: return;
        }
        next = (u32)(p - code);
        {   /* registers and dxns (checked only in code the data flow reaches) */
            u32 r1 = 0, r2 = 0; int nr = 0; char b[400];
            if (cp == CP_LOCAL) { r1 = a1; nr = 1; }
            else if (op >= 0xd0 && op <= 0xd7) { r1 = (u32)(op & 3); nr = 1; }
            else if (op == 0x32) { r1 = a1; r2 = a2; nr = 2; }
            if (nr && (r1 >= (u32)d->nloc || (nr == 2 && r2 >= (u32)d->nloc))) {
                sprintf(b, "An invalid register %lu was accessed.", (unsigned long)(r1 >= (u32)d->nloc ? r1 : r2)); dfail(d, 1025, b); return;
            }
            if ((op == 0x06 || op == 0x07) && !(d->abc->methods[d->m->index].flags & MF_SET_DXNS)) {
                sprintf(b, "Method %.300s() cannot set default xml namespace", d->mname); dfail(d, 1015, b); return;
            }
        }
        if (can_throw(op)) {
            for (k = 0; k < d->b->exception_count && !d->err; k++) {
                const AbcException *e = &d->b->exceptions[k];
                if (e->from <= pc && pc < e->to) {
                    if (d->maxstack < 1) { dfail(d, 1023, "Stack overflow occurred."); return; }
                    df_merge(d, e->target, 1, 0, NULL);
                }
            }
            if (d->err) return;
        }
        switch (op) {
        /* control flow */
        case 0x03: dpop(d, 1); return;                               /* throw */
        case 0x47: return;                                           /* returnvoid */
        case 0x48: dpop(d, 1); return;                               /* returnvalue */
        case 0x10: df_jump(d, (u32)((s32)next + off)); return;       /* jump */
        case 0x11: case 0x12: dpop(d, 1); df_jump(d, (u32)((s32)next + off)); break;
        case 0x0c: case 0x0d: case 0x0e: case 0x0f: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x18:
        case 0x19: case 0x1a: dpop(d, 2); df_jump(d, (u32)((s32)next + off)); break;
        case 0x1b: {                                                 /* lookupswitch */
            const u8 *q = code + pc + 1; u32 cnt; s32 o;
            if (!need(d, 1)) return;
            if (d->checktypes) {
                Ty t = peek(d, 0);
                if (t != TY_UNK && t != TY_INT) { char b[200], tn[120]; ty_name(d, t, tn, sizeof(tn)); sprintf(b, "Illegal operand type: %s must be int.", tn); dfail(d, 1058, b); return; }
            }
            d->sp--;
            o = abc_read_s24(&q); df_jump(d, (u32)((s32)pc + o));
            cnt = abc_read_u30(&q);
            for (k = 0; k <= cnt && !d->err; k++) { o = abc_read_s24(&q); df_jump(d, (u32)((s32)pc + o)); }
            return;
        }
        /* scope */
        case 0x1c: case 0x30:                                        /* pushwith, pushscope */
            dpop(d, 1); if (d->err) return;
            if (d->maxscope >= 0 && d->ssp >= d->maxscope) { dfail(d, 1017, "Scope stack overflow occurred."); return; }
            d->ssp++; break;
        case 0x1d: if (d->ssp <= 0) { dfail(d, 1018, "Scope stack underflow occurred."); return; } d->ssp--; break;
        case 0x65:
            if ((long)a1 >= d->ssp) { char b[80]; sprintf(b, "Getscopeobject %lu is out of bounds.", (unsigned long)a1); dfail(d, 1019, b); return; }
            dpush(d, TY_UNK); break;
        case 0x64: case 0x67: case 0x6e: case 0x57: case 0x5a: case 0x5f: dpush(d, TY_UNK); break;   /* getglobalscope getouterscope getglobalslot newactivation newcatch finddef */
        case 0x6f: dpop(d, 1); break;                                /* setglobalslot */
        /* constants */
        case 0x20: dpush(d, TY_NULL); break;
        case 0x21: dpush(d, TY_VOID); break;
        case 0x24: case 0x25: case 0x2d: dpush(d, TY_INT); break;
        case 0x2e: dpush(d, TY_UINT); break;
        case 0x26: case 0x27: dpush(d, TY_BOOL); break;
        case 0x28: case 0x2f: dpush(d, TY_NUMBER); break;
        case 0x2c: dpush(d, TY_STRING); break;
        case 0x31: dpush(d, TY_NS); break;
        /* stack */
        case 0x29: dpop(d, 1); break;
        case 0x2a: { Ty t; if (!need(d, 1)) return; t = peek(d, 0); dpush(d, t); break; }
        case 0x2b: if (!need(d, 2)) return; if (d->W) { Ty t = d->cur[d->sp - 1]; d->cur[d->sp - 1] = d->cur[d->sp - 2]; d->cur[d->sp - 2] = t; } break;
        /* locals */
        case 0x62: dpush(d, lget(d, a1)); break;
        case 0xd0: case 0xd1: case 0xd2: case 0xd3: dpush(d, lget(d, (u32)(op - 0xd0))); break;
        case 0x63: if (!need(d, 1)) return; lset(d, a1, peek(d, 0)); d->sp--; break;
        case 0xd4: case 0xd5: case 0xd6: case 0xd7: if (!need(d, 1)) return; lset(d, (u32)(op - 0xd4), peek(d, 0)); d->sp--; break;
        case 0x08: case 0x92: case 0x94: case 0xc2: case 0xc3: lset(d, a1, TY_UNK); break;
        case 0x32: lset(d, a1, TY_UNK); lset(d, a2, TY_UNK); dpush(d, TY_BOOL); break;    /* hasnext2 */
        /* multiname ops */
        case 0x04: df_mnop(d, a1, 1, 0, 1, TY_UNK); break;           /* getsuper */
        case 0x05: df_mnop(d, a1, 1, 1, 0, TY_UNK); break;           /* setsuper */
        case 0x59: df_mnop(d, a1, 1, 0, 1, TY_UNK); break;           /* getdescendants */
        case 0x5d: case 0x5e: df_mnop(d, a1, 0, 0, 1, TY_UNK); break;
        case 0x60: dpush(d, lex_type(d, a1)); break;                 /* getlex */
        case 0x61: case 0x68: df_mnop(d, a1, 1, 1, 0, TY_UNK); break;
        case 0x66: df_mnop(d, a1, 1, 0, 1, TY_UNK); break;
        case 0x6a: df_mnop(d, a1, 1, 0, 1, TY_BOOL); break;
        case 0x45: case 0x46: case 0x4c: case 0x4a: df_mnop(d, a1, 1, a2, 1, TY_UNK); break;
        case 0x4e: case 0x4f: df_mnop(d, a1, 1, a2, 0, TY_UNK); break;
        case 0x80: dpop(d, 1); dpush(d, coerce_type(d, a1)); break;  /* coerce */
        case 0x86: dpop(d, 1); dpush(d, TY_UNK); break;              /* astype */
        case 0xb2: dpop(d, 1); dpush(d, TY_BOOL); break;             /* istype */
        /* calls */
        case 0x41: dpop(d, (unsigned long)a1 + 2); dpush(d, TY_UNK); break;     /* call */
        case 0x42: {                                                            /* construct */
            Ty t, r = TY_UNK;
            if (!need(d, (unsigned long)a1 + 1)) return;
            t = peek(d, (long)a1);
            if (t >= TY_NBUILTIN && d->ctk[t - TY_NBUILTIN]) r = ty_class(d, d->ct[t - TY_NBUILTIN], 0);
            d->sp -= (long)a1 + 1; dpush(d, r); break;
        }
        case 0x43: case 0x44: dpop(d, (unsigned long)a2 + 1); dpush(d, TY_UNK); break;   /* callmethod callstatic */
        case 0x49: dpop(d, (unsigned long)a1 + 1); break;                       /* constructsuper */
        case 0x53: dpop(d, (unsigned long)a1 + 1); dpush(d, TY_UNK); break;     /* applytype */
        case 0x55: dpop(d, (unsigned long)a1 * 2); dpush(d, TY_OBJECT); break;  /* newobject */
        case 0x56: dpop(d, a1); dpush(d, TY_ARRAY); break;                      /* newarray */
        case 0x40: dpush(d, TY_FUNCTION); break;                                /* newfunction */
        case 0x58: dpop(d, 1); dpush(d, TY_UNK); break;                         /* newclass */
        /* slots */
        case 0x6c: if (!need(d, 1)) return; df_slot(d, a1, 0); dpop(d, 1); dpush(d, TY_UNK); break;
        case 0x6d: if (!need(d, 2)) return; df_slot(d, a1, 1); dpop(d, 2); break;
        /* xml */
        case 0x06: break;
        case 0x07: dpop(d, 1); break;
        case 0x71: case 0x72: dpop(d, 1); dpush(d, TY_STRING); break;
        case 0x78: dpop(d, 1); dpush(d, TY_UNK); break;
        /* conversions / unary */
        case 0x70: case 0x85: case 0x95: dpop(d, 1); dpush(d, TY_STRING); break;
        case 0x73: case 0x83: case 0x97: case 0xc0: case 0xc1: case 0xc4: case 0x50: case 0x51: case 0x52: dpop(d, 1); dpush(d, TY_INT); break;
        case 0x74: case 0x88: dpop(d, 1); dpush(d, TY_UINT); break;
        case 0x75: case 0x84: case 0x90: case 0x91: case 0x93: dpop(d, 1); dpush(d, TY_NUMBER); break;
        case 0x76: case 0x81: case 0x96: dpop(d, 1); dpush(d, TY_BOOL); break;
        case 0x77: { Ty t; if (!need(d, 1)) return; t = peek(d, 0); d->sp--; dpush(d, t); break; }   /* convert_o */
        case 0x82: dpop(d, 1); dpush(d, TY_ANY); break;                          /* coerce_a */
        case 0x89: dpop(d, 1); dpush(d, TY_OBJECT); break;                       /* coerce_o */
        /* binary */
        case 0xa0: case 0x1e: case 0x1f: case 0x23: case 0x87: dpop(d, 2); dpush(d, TY_UNK); break;   /* add nextname hasnext nextvalue astypelate */
        case 0xa1: case 0xa2: case 0xa3: case 0xa4: dpop(d, 2); dpush(d, TY_NUMBER); break;
        case 0xa5: case 0xa6: case 0xa8: case 0xa9: case 0xaa: case 0xc5: case 0xc6: case 0xc7: dpop(d, 2); dpush(d, TY_INT); break;
        case 0xa7: dpop(d, 2); dpush(d, TY_UINT); break;
        case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf: case 0xb0: case 0xb1: case 0xb3: case 0xb4: dpop(d, 2); dpush(d, TY_BOOL); break;
        /* alchemy memory */
        case 0x35: case 0x36: case 0x37: dpop(d, 1); dpush(d, TY_INT); break;
        case 0x38: case 0x39: dpop(d, 1); dpush(d, TY_NUMBER); break;
        case 0x3a: case 0x3b: case 0x3c: case 0x3d: case 0x3e: dpop(d, 2); break;
        /* no stack effect: bkpt nop label debug* timestamp */
        default: break;
        }
        pc = next;
    }
}

static int df_run(VM2 *vm, A2Method *m, const AbcBody *b, const u8 *start, int nloc, const char *mname, char *outmsg) {
    DF d; u32 pc; int rc; size_t nstarts = 1;
    memset(&d, 0, sizeof(d));
    d.vm = vm; d.m = m; d.mname = mname; d.abc = m->abc; d.b = b; d.code = b->code; d.len = b->code_len; d.start = start;
    d.maxstack = b->max_stack > 0x100000 ? 0x100000 : (long)b->max_stack;
    d.nloc = nloc;
    d.maxscope = b->max_scope_depth >= b->init_scope_depth ? (long)(b->max_scope_depth - b->init_scope_depth) : -1;
    d.lex_ok = !m->is_function;
    for (pc = 0; pc < d.len; pc++) {
        if (start[pc]) nstarts++;
        if (d.code[pc] == 0x1c) d.lex_ok = 0;   /* a possible pushwith: lookups may bind dynamically */
    }
    d.W = (d.maxstack <= 4096 && nloc <= 4096) ? (int)d.maxstack + nloc : 0;
    if (d.W && (double)d.W * (double)nstarts > 16.0 * 1024 * 1024) d.W = 0;
    d.bidx = (int*)malloc((d.len + 1) * sizeof(int));
    d.cur = (Ty*)calloc(d.W + 1, 1);
    if (!d.bidx || !d.cur) { free(d.bidx); free(d.cur); return 0; }
    for (pc = 0; pc <= d.len; pc++) d.bidx[pc] = -1;
    /* fixpoint over stack/scope depths and types; type errors wait for the final types */
    d.queue = 1; d.checktypes = 0;
    df_merge(&d, 0, 0, 0, NULL);
    while (!d.err && d.nwl > 0) {
        u32 bp = d.wl[--d.nwl];
        d.inq[d.bidx[bp]] = 0;
        df_block(&d, bp);
    }
    if (!d.err && d.W) {
        d.queue = 0; d.checktypes = 1;
        for (pc = 0; pc < d.len && !d.err; pc++) if (d.bidx[pc] >= 0) df_block(&d, pc);
    }
    rc = d.err > 0 ? d.err : 0;
    if (rc) strcpy(outmsg, d.msg);
    free(d.bidx); free(d.cur); free(d.ssp_); free(d.sssp_); free(d.ty); free(d.inq); free(d.wl);
    return rc;
}

/* returns 0 if OK; otherwise an error code and fills msg */
int a2_verify_body(VM2 *vm, A2Method *m, char *msg, int msglen) {
    AbcFile *abc = m->abc; AbcMethod *am; AbcBody *b; Ver v; u32 k, pc, len; const u8 *code; int reachable = 1, rc;
    u32 nreg; int strict;
    const char *mname;
    char emsg[400];
    { extern A2Str *a2_method_trace_name(VM2 *vm, A2Method *m); A2Str *tn = a2_method_trace_name(vm, m); mname = tn ? tn->s : (m->name ? m->name->s : ""); }
    if (!abc) return 0;
    am = &abc->methods[m->index];
    if (am->body < 0) return 0;
    b = &abc->bodies[am->body]; code = b->code; len = b->code_len;
    /* registers: local_count, but never fewer than the parameters (+ rest/arguments) the runtime allocates */
    nreg = b->local_count; if (nreg < am->param_count + 1 + ((am->flags & (MF_NEED_REST | MF_NEED_ARGUMENTS)) ? 1 : 0)) nreg = am->param_count + 1 + ((am->flags & (MF_NEED_REST | MF_NEED_ARGUMENTS)) ? 1 : 0);
    strict = !abc->is_builtin;   /* the player's own library is trusted */
    if (len == 0) { if (msg) { strncpy(msg, "Code cannot fall off the end of a method.", msglen - 1); msg[msglen - 1] = 0; } return 1020; }
    {
    /* a handler counts only once some reachable code lies in its try range:
       scan, add the handlers that became live, scan again until nothing changes */
    u8 *seeded = (u8*)calloc(b->exception_count + 1, 1), *reach = (u8*)calloc(len + 1, 1); int round, changed = 1;
    v.start = NULL;
    for (round = 0; changed && round < 64; round++) {
    changed = 0; reachable = 1;
    if (v.start) free(v.start);
    memset(&v, 0, sizeof(v)); v.abc = abc; v.body = b;
    v.start = (u8*)calloc(len + 1, 1);
    v.start[0] = 1;
    for (k = 0; k < b->exception_count; k++) {
        AbcException *e = &b->exceptions[k];
        if (e->from > len || e->to > len || e->target >= len) { if (getenv("FLASHNT_DEBUG")) fprintf(stderr, "[verify] %s: exception %lu-%lu->%lu len %lu\n", mname, (unsigned long)e->from, (unsigned long)e->to, (unsigned long)e->target, (unsigned long)len); vfail(&v, 1021, BADTARGET); break; }
        if (!seeded[k]) { u32 q; for (q = e->from; q < e->to && q < len; q++) if (reach[q]) { seeded[k] = 1; break; } }
        if (seeded[k]) v.start[e->target] = 1;
    }
    pc = 0;
    while (!v.err && pc < len) {
        const u8 *p; int op, kind, cp; u32 a1 = 0, next, q;
        op = code[pc]; opinfo(op, &kind, &cp);
        if (!reachable) {
            if (v.start[pc] || op == 0x09) { reachable = 1; v.start[pc] = 1; }
            else {
                /* dead code: skip one instruction (unknown opcodes are 1 byte) */
                p = code + pc + 1;
                switch (kind) {
                case OP_U30: abc_read_u30(&p); break;
                case OP_U30U30: abc_read_u30(&p); abc_read_u30(&p); break;
                case OP_S24: p += 3; break;
                case OP_U8: case OP_S8: p += 1; break;
                case OP_SWITCH: { u32 cnt; p += 3; cnt = abc_read_u30(&p); p += 3 * (cnt + 1); break; }
                case OP_DEBUG: p += 1; abc_read_u30(&p); p += 1; abc_read_u30(&p); break;
                default: break;
                }
                next = (u32)(p - code);
                /* a block start inside the skipped instruction resumes there */
                for (q = pc + 1; q < next && q < len; q++) if (v.start[q]) break;
                pc = (q < next && q < len) ? q : next;
                continue;
            }
        }
        reach[pc] = 1;
        if (kind == OP_BAD) { sprintf(emsg, "Method %.300s() contained illegal opcode %d at offset %lu.", mname, op, (unsigned long)pc); vfail(&v, 1011, emsg); break; }
        if (op == 0x09) v.start[pc] = 1;    /* label */
        p = code + pc + 1;
        switch (kind) {
        case OP_NONE: break;
        case OP_U30: a1 = abc_read_u30(&p); break;
        case OP_U30U30: a1 = abc_read_u30(&p); abc_read_u30(&p); break;
        case OP_S24: { s32 off = abc_read_s24(&p); branch(&v, pc, (u32)((s32)(p - code) + off)); break; }
        case OP_U8: case OP_S8: p += 1; break;
        case OP_SWITCH: {
            s32 off = abc_read_s24(&p); u32 cnt;
            branch(&v, pc, (u32)((s32)pc + off));
            cnt = abc_read_u30(&p);
            for (k = 0; k <= cnt && !v.err; k++) { off = abc_read_s24(&p); branch(&v, pc, (u32)((s32)pc + off)); }
            break;
        }
        case OP_DEBUG: p += 1; abc_read_u30(&p); p += 1; abc_read_u30(&p); break;
        }
        if (v.err) break;
        next = (u32)(p - code);
        if (next > len) { vfail(&v, 1020, "Code cannot fall off the end of a method."); break; }
        for (q = pc + 1; q < next; q++) if (v.start[q]) { if (getenv("FLASHNT_DEBUG")) fprintf(stderr, "[verify] %s: target %lu inside instruction at %lu (op %02x)\n", mname, (unsigned long)q, (unsigned long)pc, op); vfail(&v, 1021, BADTARGET); break; }
        if (v.err) break;
        switch (cp) {
        case CP_INT: cp_check(&v, cp, a1, abc->int_count); break;
        case CP_UINT: cp_check(&v, cp, a1, abc->uint_count); break;
        case CP_DOUBLE: cp_check(&v, cp, a1, abc->double_count); break;
        case CP_STRING: cp_check(&v, cp, a1, abc->string_count); break;
        case CP_NS: cp_check(&v, cp, a1, abc->ns_count); break;
        case CP_MN: case CP_MNARGS: cp_check(&v, CP_STRING, a1, abc->mn_count); break;
        case CP_METHOD: cp_check(&v, cp, a1, abc->method_count); break;
        case CP_CLASS: cp_check(&v, cp, a1, abc->class_count); break;
        case CP_EXC: cp_check(&v, cp, a1, b->exception_count); break;
        default: break;
        }
        if (v.err) break;
        if ((op == 0x40 || op == 0x44) && m->rt && round == 0) {
            /* method association: newfunction needs a method not bound to a class, callstatic one that is */
            extern A2Method *a2_rt_method_peek(void *rt, u32 i);
            A2Method *t = a2_rt_method_peek(m->rt, a1);
            if (op == 0x40) { if (t && t->owner) { vfail(&v, 1107, "The ABC data is corrupt, attempt to read out of bounds."); break; } }
            else if (!t || (!t->owner && !t->trait_bound)) { vfail(&v, 1107, "The ABC data is corrupt, attempt to read out of bounds."); break; }
        }
        if (op == 0x10 || op == 0x47 || op == 0x48 || op == 0x03 || op == 0x1b) reachable = 0;
        pc = next;
    }
    for (k = 0; k < b->exception_count; k++) if (!seeded[k]) { AbcException *e = &b->exceptions[k]; u32 q; for (q = e->from; q < e->to && q < len; q++) if (reach[q]) { changed = 1; break; } }
    if (v.err) break;
    }
    free(seeded); free(reach);
    }
    if (!v.err && reachable) vfail(&v, 1020, "Code cannot fall off the end of a method.");
    if (!v.err && strict) {
        char dmsg[256]; int drc = df_run(vm, m, b, v.start, (int)nreg, mname, dmsg);
        if (drc) vfail(&v, drc, dmsg);
    }
    rc = v.err;
    if (rc && msg) { strncpy(msg, v.msg, msglen - 1); msg[msglen - 1] = 0; }
    free(v.start);
    return rc;
}
